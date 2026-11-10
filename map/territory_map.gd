extends RefCounted
class_name TerritoryMap

## Território dinâmico: cada província (Natural Earth admin-1) pertence a uma
## nação. Províncias de nações jogáveis são atribuídas à cidade mais próxima da
## mesma nação original; o dono é a nação da cidade atribuída (com override de
## debug). Províncias sem cidade mantêm o próprio país. O dono é publicado numa
## textura RGBA8 compacta (64 colunas) que o shader lê por id de província.
##
## A fonte de verdade da nação de cada cidade é o status do Economy, que já
## chega em `city_world.refresh_modes`. Esta classe só faz a atribuição visual.

const PROVINCES_PATH := "res://map/map_data/atlas/provinces.json"
const OWNER_COLUMNS := 64

## Rasters do mundo inteiro para gerar o minimapa dinâmico (dono por província).
const WORLD_PROVINCE_PATH := "res://map/map_data/atlas/world_province.png"
const WORLD_SDF_PATH := "res://map/map_data/atlas/world_sdf.png"
## Resolução do minimapa: mesma do minimap.png estático (512×256).
const MINIMAP_IMAGE_SIZE := Vector2i(512, 256)
## Cores do minimapa estático (tools/build_atlas_layers.py), para consistência.
const MINIMAP_SEA := Color(34.0 / 255.0, 64.0 / 255.0, 74.0 / 255.0)
const MINIMAP_LAND_FACTOR := 0.92

var _provinces: Array[Dictionary] = []
var _nation_colors: Dictionary = {}       # tag jogável -> Color (sRGB)
var _playable_tags: Array[String] = []    # ordem de nation_colors
var _tag_index: Dictionary = {}           # tag -> índice do dono (1-based)
var _assignment: Dictionary = {}          # province_id -> city_id
var _owners: Dictionary = {}              # province_id -> tag do dono
var _image: Image
var _texture: ImageTexture
var _world_province: Image
var _world_sdf: Image
var _minimap_image: Image
var _minimap_texture: ImageTexture
var _max_id := 0
var _loaded := false


## Lê provinces.json e monta os índices de dono e a lista de províncias.
func load_data() -> void:
	_provinces.clear()
	_nation_colors.clear()
	_playable_tags.clear()
	_tag_index.clear()
	_assignment.clear()
	_owners.clear()
	_max_id = 0
	_loaded = false

	var file := FileAccess.open(PROVINCES_PATH, FileAccess.READ)
	if file == null:
		push_error("TerritoryMap: não abriu %s" % PROVINCES_PATH)
		return
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	if typeof(data) != TYPE_DICTIONARY:
		push_error("TerritoryMap: %s não é um objeto JSON" % PROVINCES_PATH)
		return

	# Índices de dono: tags jogáveis primeiro (1..5, ordem de nation_colors).
	var colors: Dictionary = data.get("nation_colors", {})
	for tag: Variant in colors:
		var rgb: Array = colors[tag]
		_playable_tags.append(String(tag))
		_nation_colors[String(tag)] = Color(
				float(rgb[0]) / 255.0, float(rgb[1]) / 255.0, float(rgb[2]) / 255.0)
		_tag_index[String(tag)] = _playable_tags.size()

	# Depois, cada país não jogável (na ordem de aparição) recebe 6, 7, ...
	var next_index := _playable_tags.size() + 1
	var provinces: Array = data.get("provinces", [])
	for record: Variant in provinces:
		if typeof(record) != TYPE_DICTIONARY:
			continue
		var country := String(record.get("country", ""))
		if not _tag_index.has(country):
			_tag_index[country] = next_index
			next_index += 1

	for record: Variant in provinces:
		if typeof(record) != TYPE_DICTIONARY:
			continue
		var pid: int = int(record.get("id", 0))
		var rgb: Array = record.get("color", [0, 0, 0])
		_provinces.append({
			"id": pid,
			"nation": String(record.get("nation", "")),
			"country": String(record.get("country", "")),
			"color": Color(float(rgb[0]) / 255.0, float(rgb[1]) / 255.0, float(rgb[2]) / 255.0),
			"x": float(record.get("x", 0.0)),
			"y": float(record.get("y", 0.0)),
			"area": float(record.get("area_global", 0.0)),
		})
		_max_id = maxi(_max_id, pid)

	_loaded = not _provinces.is_empty()


func is_loaded() -> bool:
	return _loaded


## Atribui cada província jogável à cidade mais próxima da mesma REGIÃO NATURAL
## (`home_nation`), para o vínculo cidade↔território não mudar quando a nação
## (dono) muda.
func assign(cities: Array) -> void:
	_assignment.clear()
	var cities_by_home: Dictionary = {}
	for city: Variant in cities:
		if typeof(city) != TYPE_DICTIONARY:
			continue
		var home := String(city.get("home_nation", city.get("nation", "")))
		if not cities_by_home.has(home):
			cities_by_home[home] = []
		(cities_by_home[home] as Array).append(city)

	for province in _provinces:
		var nation := String(province["nation"])
		if not cities_by_home.has(nation):
			continue
		var nearest: Dictionary = _nearest_city(
				cities_by_home[nation], float(province["x"]), float(province["y"]))
		if not nearest.is_empty():
			_assignment[int(province["id"])] = String(nearest.get("id", ""))


static func _nearest_city(cities: Array, x: float, y: float) -> Dictionary:
	var best: Dictionary = {}
	var best_d := INF
	for city: Variant in cities:
		if typeof(city) != TYPE_DICTIONARY:
			continue
		var d := Vector2(float(city.get("x", 0.0)), float(city.get("y", 0.0))).distance_to(Vector2(x, y))
		if d < best_d:
			best_d = d
			best = city
	return best


## Calcula o dono de cada província e reescreve a textura RGBA8. Retorna true
## se o conjunto de donos mudou em relação à chamada anterior.
func update(cities: Array, overrides: Dictionary) -> bool:
	assign(cities)
	# id da cidade -> nação (dono) atual, para colorir o território.
	var city_nation: Dictionary = {}
	for city: Variant in cities:
		if typeof(city) == TYPE_DICTIONARY:
			city_nation[String(city.get("id", ""))] = String(city.get("nation", ""))
	var changed := false
	var new_owners: Dictionary = {}
	for province in _provinces:
		var pid: int = province["id"]
		var tag: String
		if _assignment.has(pid):
			var city_id := String(_assignment[pid])
			tag = String(overrides.get(city_id, city_nation.get(city_id, String(province["nation"]))))
		else:
			tag = String(province["country"])
		new_owners[pid] = tag
		if String(_owners.get(pid, "")) != tag:
			changed = true

	_write_owner_image(new_owners)
	_owners = new_owners
	return changed


## Textura do dono por província: rgb = cor do dono, a = índice do dono / 255.
func owner_texture() -> ImageTexture:
	return _texture


## Imagem CPU (RGBA8) que é enviada no update() da textura. A leitura da textura
## via get_image() não reflete o update() sob o renderizador dummy do headless,
## então testes/capturas leem esta imagem diretamente.
func owner_image() -> Image:
	return _image


## Dono atual por id de província (id -> tag).
func owners() -> Dictionary:
	return _owners.duplicate()


## Cidade dona da província ("" se a província não foi atribuída).
func city_for_province(province_id: int) -> String:
	return String(_assignment.get(province_id, ""))


## True se a posição (UV do mapa 0..1) está em terra firme (não no mar).
func land_at_uv(uv: Vector2) -> bool:
	var sdf := _world_sdf_raster()
	if sdf == null:
		return true
	var tx := clampi(int(uv.x * sdf.get_width()), 0, sdf.get_width() - 1)
	var ty := clampi(int(uv.y * sdf.get_height()), 0, sdf.get_height() - 1)
	return sdf.get_pixel(tx, ty).r > 0.5


## Dono (tag) da província na posição UV do mapa (0..1). Vazio se desconhecido.
func owner_at_uv(uv: Vector2) -> String:
	var ids := _world_province_raster()
	if ids == null:
		return ""
	var tx := clampi(int(uv.x * ids.get_width()), 0, ids.get_width() - 1)
	var ty := clampi(int(uv.y * ids.get_height()), 0, ids.get_height() - 1)
	var c := ids.get_pixel(tx, ty)
	var pid := int(round(c.r * 255.0 + c.g * 255.0 * 256.0))
	return String(_owners.get(pid, ""))


## Imagem RGB8 do minimapa: cor do dono por pixel, amostrando o raster de ids
## do mundo inteiro (world_province.png) e colorindo pelo dono atual. Água usa a
## cor de mar do atlas; terra usa a cor do dono com o mesmo fator do minimapa
## estático (0.92). O raster de id não tem zeros (o mar é preenchido com a
## província de terra mais próxima), então a máscara terra/água vem do canal R
## do world_sdf.png (costa: >128 = terra), a mesma convenção do shader.
func minimap_image(
		width: int = MINIMAP_IMAGE_SIZE.x,
		height: int = MINIMAP_IMAGE_SIZE.y) -> Image:
	if width <= 0:
		width = MINIMAP_IMAGE_SIZE.x
	if height <= 0:
		height = MINIMAP_IMAGE_SIZE.y

	var out := Image.create(width, height, false, Image.FORMAT_RGB8)
	out.fill(MINIMAP_SEA)

	var ids := _world_province_raster()
	var sdf := _world_sdf_raster()
	if ids == null or sdf == null or _provinces.is_empty():
		return out

	# id -> cor do dono (fonte de verdade: _owners + _color_for).
	var id_color: Dictionary = {}
	for province in _provinces:
		var pid: int = province["id"]
		var tag := String(_owners.get(pid, String(province["country"])))
		id_color[pid] = _color_for(tag, province)

	# Amostra nearest do raster cheio (não usa resize, que não existe em Image).
	var src_w := ids.get_width()
	var src_h := ids.get_height()
	for y in height:
		for x in width:
			var sx := int(float(x) * src_w / float(width))
			var sy := int(float(y) * src_h / float(height))
			var sdf_px: Color = sdf.get_pixel(sx, sy)
			if sdf_px.r * 255.0 <= 128.0:
				continue  # água: já preenchida com MINIMAP_SEA
			var id_px: Color = ids.get_pixel(sx, sy)
			var id := int(round(id_px.r * 255.0 + id_px.g * 255.0 * 256.0))
			if id_color.has(id):
				out.set_pixel(x, y, (id_color[id] as Color) * MINIMAP_LAND_FACTOR)
	return out


## Textura do minimapa pronta para o TextureRect (regenera a imagem sob demanda).
func minimap_texture() -> ImageTexture:
	_minimap_image = minimap_image()
	if _minimap_texture == null:
		_minimap_texture = ImageTexture.create_from_image(_minimap_image)
	else:
		_minimap_texture.update(_minimap_image)
	return _minimap_texture


func _world_province_raster() -> Image:
	if _world_province == null:
		_world_province = Image.load_from_file(WORLD_PROVINCE_PATH)
	return _world_province


func _world_sdf_raster() -> Image:
	if _world_sdf == null:
		_world_sdf = Image.load_from_file(WORLD_SDF_PATH)
	return _world_sdf


## Lista de províncias (id, nation, country, color, x, y, area).
func provinces() -> Array:
	return _provinces.duplicate()


## Âncora do rótulo de uma nação: centroide de província possuída mais próximo
## do centroide ponderado por área. Vazio se a nação não tem território.
func nation_anchor(tag: String) -> Dictionary:
	var total_area := 0.0
	var weighted := Vector2.ZERO
	var owned: Array[Dictionary] = []
	for province in _provinces:
		if String(_owners.get(int(province["id"]), "")) != tag:
			continue
		var area := float(province["area"])
		weighted += Vector2(float(province["x"]), float(province["y"])) * area
		total_area += area
		owned.append(province)
	if owned.is_empty():
		return {}

	var centroid := weighted / total_area
	var best: Dictionary = {}
	var best_d := INF
	for province in owned:
		var d := Vector2(float(province["x"]), float(province["y"])).distance_to(centroid)
		if d < best_d:
			best_d = d
			best = province
	return {
		"x": float(best["x"]),
		"y": float(best["y"]),
		"extent_px": sqrt(total_area),
	}


## True se a nação possui exatamente o conjunto original de províncias.
func is_original(tag: String) -> bool:
	var original_ids: Array[int] = []
	var owned_ids: Array[int] = []
	for province in _provinces:
		var pid: int = province["id"]
		if String(province["nation"]) == tag:
			original_ids.append(pid)
		if String(_owners.get(pid, "")) == tag:
			owned_ids.append(pid)
	if original_ids.size() != owned_ids.size():
		return false
	original_ids.sort()
	owned_ids.sort()
	for index in original_ids.size():
		if original_ids[index] != owned_ids[index]:
			return false
	return true


func _write_owner_image(owners: Dictionary) -> void:
	if _provinces.is_empty():
		return
	var height := int(ceil(float(_max_id + 1) / float(OWNER_COLUMNS)))
	if _image == null or _image.get_width() != OWNER_COLUMNS or _image.get_height() != height:
		_image = Image.create(OWNER_COLUMNS, height, false, Image.FORMAT_RGBA8)

	for province in _provinces:
		var pid: int = province["id"]
		var tag := String(owners.get(pid, String(province["country"])))
		var color := _color_for(tag, province)
		var index: int = int(_tag_index.get(tag, 0))
		_image.set_pixel(
				pid % OWNER_COLUMNS, int(pid / OWNER_COLUMNS),
				Color(color.r, color.g, color.b, float(index) / 255.0))

	if _texture == null:
		_texture = ImageTexture.create_from_image(_image)
	else:
		_texture.update(_image)


func _color_for(tag: String, province: Dictionary) -> Color:
	if _nation_colors.has(tag):
		return _nation_colors[tag]
	return province["color"]
