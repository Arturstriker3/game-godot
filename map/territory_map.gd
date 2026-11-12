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
const GeographyScript := preload("res://core/economy/geography_service.gd")
const OWNER_COLUMNS := 64

## Cores do mapa político por nação. Fonte ÚNICA: `color` de
## `data/definitions/scenario/nations.csv` — as 231 nações, inclusive as 8
## jogáveis (o `nation_colors` de `provinces.json` carrega os mesmos valores, só
## para o atlas estático e o editor). Quando a cor do CSV fica indistinguível da
## de um VIZINHO, a paleta de papelaria abaixo é a rede de segurança (regra
## MAPCOLOR: vizinhos nunca com o mesmo tom); com os dados da F3 ela não entra.
const NATIONS_CSV := "res://data/definitions/scenario/nations.csv"
## Tons "papelaria" (baixa saturação, mesmo peso visual do atlas) usados só para
## desempatar vizinhos. Ordem estável: o resultado é determinístico.
const PAPER_PALETTE: Array[Color] = [
	Color("#86a877"),  # sálvia
	Color("#86a5c0"),  # aço
	Color("#c08f55"),  # ocre
	Color("#b4788a"),  # rosa queimado
	Color("#8a78a8"),  # lavanda
	Color("#5f9e94"),  # verde-água
	Color("#c6bc6e"),  # mostarda
	Color("#8a7566"),  # umbro
]
## Distância RGB mínima entre as cores de dois vizinhos para ainda lerem como
## nações diferentes no zoom padrão.
const COLOR_MIN_DISTANCE := 0.16
## Passo (px) da amostragem do raster de províncias ao montar a adjacência.
## 16 px do raster de 8192 = ~0,7° de arco: suficiente para fronteiras
## nacionais, sem varrer 33 milhões de texels na abertura do mapa.
const ADJACENCY_STEP := 16

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
var _palette: Dictionary = {}             # tag (jogável ou do cenário) -> Color
var _adjacency: Dictionary = {}           # tag -> {tag vizinha: true}
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
## Assinatura do conjunto de cidades que gerou `_assignment`. Os centros e a
## nação de origem não mudam no dia a dia: `assign` é O(províncias × cidades)
## e não precisa rodar a cada virada.
var _assign_signature := ""
## Assinatura dos donos (nação das cidades + overrides). Sem mudança, a textura
## de território não é recalculada nem reenviada.
var _owner_signature_cached := ""


## Lê provinces.json e monta os índices de dono e a lista de províncias.
func load_data() -> void:
	_provinces.clear()
	_nation_colors.clear()
	_palette.clear()
	_adjacency.clear()
	_playable_tags.clear()
	_tag_index.clear()
	_assignment.clear()
	_owners.clear()
	_max_id = 0
	_loaded = false
	_assign_signature = ""
	_owner_signature_cached = ""

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

	_build_nation_palette()
	_loaded = not _provinces.is_empty()


## Monta a cor de cada nação: TODAS vêm de nations.csv (as 8 jogáveis também —
## `provinces.json` só repete os mesmos valores para o atlas/editor). A cor do
## CSV é trocada pela paleta de papelaria apenas se repetir o tom de um vizinho
## (ver PAPER_PALETTE). Chave = tag de dono usada em `_owner_image`.
func _build_nation_palette() -> void:
	_palette.clear()
	var scenario := scenario_colors()
	# Fallback do atlas enquanto nations.csv não abrir (nunca em jogo normal):
	# mantém as jogáveis pintadas em vez de cair no bege neutro.
	if scenario.is_empty():
		for tag: Variant in _nation_colors:
			_palette[String(tag)] = _nation_colors[tag]
		return
	_adjacency = _country_adjacency()
	var tags: Array = scenario.keys()
	tags.sort()
	for tag: Variant in tags:
		var name := String(tag)
		var wanted: Color = scenario[tag]
		_palette[name] = wanted if _palette_fits(name, wanted) \
				else _paper_color_for(name, wanted)


## Cor de cada nação em `nations.csv` (coluna `color`, hex `#rrggbb`). Estática
## para o CityWorld pintar os marcadores com a MESMA fonte antes de o
## TerritoryMap existir (a ordem do build cria as cidades primeiro).
static func scenario_colors() -> Dictionary:
	var out: Dictionary = {}
	var file := FileAccess.open(NATIONS_CSV, FileAccess.READ)
	if file == null:
		push_warning("TerritoryMap: não abriu " + NATIONS_CSV)
		return out
	var header := file.get_csv_line()
	var id_index := header.find("id")
	var color_index := header.find("color")
	if id_index < 0 or color_index < 0:
		file.close()
		return out
	var last := maxi(id_index, color_index)
	while not file.eof_reached():
		var row := file.get_csv_line()
		if row.size() <= last:
			continue
		var tag := String(row[id_index]).strip_edges()
		var hex := String(row[color_index]).strip_edges()
		if tag.is_empty() or hex.is_empty():
			continue
		var color := Color.from_string(hex, Color(0.0, 0.0, 0.0, 0.0))
		if color.a > 0.0:
			out[tag] = color
	file.close()
	return out


## A cor `color` pode representar `tag` sem se confundir com os vizinhos já
## atribuídos? (distância RGB mínima de COLOR_MIN_DISTANCE)
func _palette_fits(tag: String, color: Color) -> bool:
	var neighbors: Dictionary = _adjacency.get(tag, {})
	for neighbor: Variant in neighbors:
		var other: Color = _palette.get(String(neighbor), Color(-1.0, 0.0, 0.0))
		if other.r < 0.0:
			continue
		if _color_distance(color, other) < COLOR_MIN_DISTANCE:
			return false
	return true


## Primeiro tom de papelaria que não se confunde com um vizinho; se todos já
## estiverem ocupados, o de maior distância mínima (determinístico pela ordem).
func _paper_color_for(tag: String, fallback: Color) -> Color:
	for color: Color in PAPER_PALETTE:
		if _palette_fits(tag, color):
			return color
	var best := fallback
	var best_score := -1.0
	var neighbors: Dictionary = _adjacency.get(tag, {})
	for color: Color in PAPER_PALETTE:
		var min_distance := 1.0
		for neighbor: Variant in neighbors:
			var other: Color = _palette.get(String(neighbor), Color(-1.0, 0.0, 0.0))
			if other.r < 0.0:
				continue
			min_distance = minf(min_distance, _color_distance(color, other))
		if min_distance > best_score:
			best_score = min_distance
			best = color
	return best


static func _color_distance(a: Color, b: Color) -> float:
	var dr := a.r - b.r
	var dg := a.g - b.g
	var db := a.b - b.b
	return sqrt(dr * dr + dg * dg + db * db)


## Adjacência entre nações lida do raster de províncias do atlas: dois texels
## vizinhos de províncias diferentes geram uma aresta entre os países. A
## amostragem usa ADJACENCY_STEP (o raster tem 8192×4096): uma grade de ~512×256
## pontos basta para as fronteiras nacionais.
func _country_adjacency() -> Dictionary:
	var edges: Dictionary = {}
	var ids := _world_province_raster()
	if ids == null or _provinces.is_empty():
		return edges
	if ids.get_format() != Image.FORMAT_RGB8:
		ids.convert(Image.FORMAT_RGB8)
	var tag_by_id := PackedStringArray()
	tag_by_id.resize(_max_id + 1)
	for province in _provinces:
		var country := String(province["country"])
		var tag: String = GeographyScript.nation_for_province_tag(country)
		tag_by_id[int(province["id"])] = tag if tag != "" else country
	var width := ids.get_width()
	var height := ids.get_height()
	var step := ADJACENCY_STEP
	var columns := int(ceil(float(width) / float(step)))
	var rows := int(ceil(float(height) / float(step)))
	var sampled: Array[PackedInt32Array] = []
	var land: Array[PackedByteArray] = []
	# O raster de províncias PREENCHE o mar com a província de terra mais
	# próxima (id 0 nunca aparece). Sem a máscara de terra, duas costas a até
	# ADJACENCY_STEP pixels viram "vizinhas" (Gibraltar, Órmuz, Bioko) e a
	# paleta trocava cores por um par que não existe no mapa. A máscara é o
	# canal R do world_sdf.png (>0,5 = terra), a mesma convenção do shader.
	var sdf := _world_sdf_raster()
	var sdf_w := sdf.get_width() if sdf != null else 0
	var sdf_h := sdf.get_height() if sdf != null else 0
	for ry in rows:
		var row := PackedInt32Array()
		var land_row := PackedByteArray()
		row.resize(columns)
		land_row.resize(columns)
		var py := mini(ry * step, height - 1)
		var ty := clampi(int(float(py) / float(height) * float(sdf_h)), 0, sdf_h - 1)
		for rx in columns:
			var px := mini(rx * step, width - 1)
			var color := ids.get_pixel(px, py)
			row[rx] = int(round(color.r * 255.0 + color.g * 255.0 * 256.0))
			if sdf != null:
				var tx := clampi(int(float(px) / float(width) * float(sdf_w)), 0, sdf_w - 1)
				land_row[rx] = 1 if sdf.get_pixel(tx, ty).r > 0.5 else 0
			else:
				land_row[rx] = 1
		sampled.append(row)
		land.append(land_row)
	for ry in rows:
		for rx in columns:
			var id_a: int = sampled[ry][rx]
			if id_a <= 0 or id_a >= tag_by_id.size() or land[ry][rx] == 0:
				continue
			if rx + 1 < columns and land[ry][rx + 1] != 0:
				_connect_nations(edges, id_a, int(sampled[ry][rx + 1]), tag_by_id)
			if ry + 1 < rows and land[ry + 1][rx] != 0:
				_connect_nations(edges, id_a, int(sampled[ry + 1][rx]), tag_by_id)
	return edges


static func _connect_nations(edges: Dictionary, id_a: int, id_b: int,
		tag_by_id: PackedStringArray) -> void:
	if id_b <= 0 or id_a == id_b:
		return
	if id_b >= tag_by_id.size():
		return
	var tag_a := tag_by_id[id_a]
	var tag_b := tag_by_id[id_b]
	if tag_a == "" or tag_b == "" or tag_a == tag_b:
		return
	if not edges.has(tag_a):
		edges[tag_a] = {}
	if not edges.has(tag_b):
		edges[tag_b] = {}
	var set_a: Dictionary = edges[tag_a]
	set_a[tag_b] = true
	var set_b: Dictionary = edges[tag_b]
	set_b[tag_a] = true


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
		# Tag do atlas -> tag do jogo (KOS -> XKX). Nações sem cidade na
		# província mantêm o próprio país (não jogável).
		var home := GeographyScript.nation_for_province_tag(nation)
		if home == "" or not cities_by_home.has(home):
			continue
		var nearest: Dictionary = _nearest_city(
				cities_by_home[home], float(province["x"]), float(province["y"]))
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
## se o conjunto de donos mudou em relação à chamada anterior. Só reescreve a
## textura quando algo mudou (a virada do dia chama isto a cada dia).
func update(cities: Array, overrides: Dictionary) -> bool:
	_assign_if_needed(cities)
	# Sem mudança de donos (nação da cidade ou override), nada a recalcular:
	# a virada do dia chama isto com o mesmo conjunto. O custo era ~3,5 ms/dia
	# varrendo 4.529 províncias e reenviando a textura.
	var signature := _assign_signature + "\n#\n" + _owner_signature(cities, overrides)
	if signature == _owner_signature_cached and _texture != null:
		return false
	_owner_signature_cached = signature

	# id da cidade -> nação (dono) atual, para colorir o território.
	var city_nation: Dictionary = {}
	for city: Variant in cities:
		if typeof(city) != TYPE_DICTIONARY:
			continue
		var city_tag := String(city.get("nation", ""))
		city_nation[String(city.get("id", ""))] = city_tag
		# Tag de cidade que não é um `country` do atlas (XKX) ainda precisa de
		# índice no canal alfa do dono. O teto de 255 já era o limite do atlas
		# (250 países) e nenhuma tag nova o ultrapassa.
		if city_tag != "" and not _tag_index.has(city_tag) and _tag_index.size() < 255:
			_tag_index[city_tag] = _tag_index.size() + 1
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

	if changed or _texture == null:
		_write_owner_image(new_owners)
	_owners = new_owners
	return changed


## Assinatura dos insumos do dono: nação de cada cidade + overrides. Estável
## durante a partida; só muda quando um território realmente muda de dono.
func _owner_signature(cities: Array, overrides: Dictionary) -> String:
	var parts := PackedStringArray()
	for city: Variant in cities:
		if typeof(city) != TYPE_DICTIONARY:
			continue
		parts.append("%s|%s" % [String(city.get("id", "")),
			String(city.get("nation", ""))])
	var keys: Array = overrides.keys()
	keys.sort()
	for key: Variant in keys:
		parts.append("o:%s|%s" % [String(key), String(overrides[key])])
	return "\n".join(parts)


## Reatribui províncias→cidade só quando o conjunto de cidades muda (id, nação
## de origem ou centro). `refresh_modes` roda a cada dia; a atribuição é estática
## durante a partida.
func _assign_if_needed(cities: Array) -> void:
	var parts := PackedStringArray()
	for city: Variant in cities:
		if typeof(city) != TYPE_DICTIONARY:
			continue
		parts.append("%s|%s|%.3f|%.3f" % [
			String(city.get("id", "")),
			String(city.get("home_nation", city.get("nation", ""))),
			float(city.get("x", 0.0)), float(city.get("y", 0.0))])
	var signature := "\n".join(parts)
	if signature == _assign_signature:
		return
	_assign_signature = signature
	assign(cities)


## Textura do dono por província: rgb = cor do dono, a = índice do dono / 255.
func owner_texture() -> ImageTexture:
	return _texture


## Imagem CPU (RGBA8) que é enviada no update() da textura. A leitura da textura
## via get_image() não reflete o update() sob o renderizador dummy do headless,
## então testes/capturas leem esta imagem diretamente.
func owner_image() -> Image:
	return _image


## Índice do dono (1-based) usado na textura `province_owner` para destacar a
## nação no shader. -1 se a tag não é conhecida.
func owner_index(tag: String) -> int:
	return int(_tag_index.get(tag, -1))


## Cor do dono no mapa político (a mesma escrita na textura de províncias).
func nation_color(tag: String) -> Color:
	if _palette.has(tag):
		return _palette[tag]
	return _nation_colors.get(tag, Color(0.0, 0.0, 0.0, 0.0))


## Tags vizinhas (fronteira no raster de províncias), ordenadas. Exposto para o
## teste de cores garantir que vizinhos nunca compartilham o mesmo tom.
func adjacent_nations(tag: String) -> Array[String]:
	var out: Array[String] = []
	var neighbors: Dictionary = _adjacency.get(tag, {})
	for neighbor: Variant in neighbors:
		out.append(String(neighbor))
	out.sort()
	return out


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


## Cor final do dono no mapa político. A paleta vem de nations.csv (231 nações),
## com a paleta de papelaria apenas como rede de segurança; tags fora do cenário
## caem na cor da província do atlas.
func _color_for(tag: String, province: Dictionary) -> Color:
	if _palette.has(tag):
		return _palette[tag]
	if _nation_colors.has(tag):
		return _nation_colors[tag]
	return province["color"]
