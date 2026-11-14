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
## Cor do contorno do destaque de nação (borda de latão do atlas político).
const HIGHLIGHT_BRASS := Color("#c9a24a")
## Espessura do contorno do destaque de nação, em pixels de tela (shader).
const HIGHLIGHT_BORDER_PX := 2.0

## Grade do modelo de rótulo de nação (mesma proporção 2:1 do mapa). 512×256
## dá células de ~9,8 px do mapa: suficiente para achar o maior pedaço contíguo
## e o polo de inacessibilidade de países pequenos sem varrer os 33 milhões de
## texels do raster de 8192×4096 na abertura.
const NATION_LABEL_GRID := Vector2i(512, 256)
## Sistema de coordenadas do mapa (o mesmo `pixel_to_world` do CityWorld):
## 5000×2500 px. Usado para converter a grade de rótulos em px do mapa.
const MAP_SIZE_PX := Vector2(5000.0, 2500.0)

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
## Modelo de rótulo por nação (maior pedaço contíguo): cache invalidado quando
## os donos mudam. `_pid_canonical` mapeia id de província -> tag canônica do
## dono atual, para a checagem de encaixe do rótulo.
var _nation_models: Dictionary = {}
var _nation_models_ready := false
var _pid_canonical: Dictionary = {}
## Últimos insumos vistos, em colunas tipadas. A virada do dia chama `update()`
## todo dia com o mesmo conjunto de cidades; comparar coluna a coluna custa
## ~0,3 ms em 858 cidades, enquanto montar duas assinaturas de texto (com
## `%.3f` por cidade) custava ~4 ms/dia no frame da virada.
var _input_ids := PackedStringArray()
var _input_homes := PackedStringArray()
var _input_nations := PackedStringArray()
var _input_xy := PackedFloat64Array()
var _override_keys := PackedStringArray()
var _override_vals := PackedStringArray()
## Destaque de nação (hover do nome/bandeira): tag canônica e índice do dono
## usados pelo shader. `_highlight_index` = -1 sem destaque.
var _highlight_tag := ""
var _highlight_index := -1
## Materiais do mapa político que já receberam `apply_nation_highlight` (a
## instância é a MESMA criada pelo CityWorld). Guardados como referência FRACA
## e reescritos a cada troca de destaque: sem isto, `set_highlight_nation`
## mudava só o estado interno e o material ficava com o latão do destaque
## anterior para sempre (contorno dourado sem destaque ativo).
var _highlight_materials: Array[WeakRef] = []


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
	_nation_models.clear()
	_nation_models_ready = false
	_pid_canonical.clear()
	_input_ids = PackedStringArray()
	_input_homes = PackedStringArray()
	_input_nations = PackedStringArray()
	_input_xy = PackedFloat64Array()
	_override_keys = PackedStringArray()
	_override_vals = PackedStringArray()
	_highlight_tag = ""
	_highlight_index = -1
	_highlight_materials.clear()

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
	var changed := _sync_inputs(cities, overrides)
	if changed & 1 != 0:
		assign(cities)
	# Sem mudança de insumos (cidades, nação de origem/atual ou override), nada
	# a recalcular: a virada do dia chama isto com o mesmo conjunto.
	if changed == 0 and _texture != null:
		return false

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
	var owners_changed := false
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
			owners_changed = true

	if owners_changed or _texture == null:
		_write_owner_image(new_owners)
	_owners = new_owners
	if owners_changed:
		_nation_models.clear()
		_nation_models_ready = false
		_pid_canonical.clear()
	return owners_changed


## Compara os insumos do território (id, região de origem, nação e centro de
## cada cidade + overrides) com a última chamada. Devolve um bitmask:
## 1 = atribuição província→cidade precisa ser refeita, 2 = donos mudaram,
## 4 = overrides mudaram. Sem mudança, não aloca nada.
func _sync_inputs(cities: Array, overrides: Dictionary) -> int:
	var changed := 0
	var city_flags := 0
	if cities.size() != _input_ids.size():
		city_flags = 1 | 2
	else:
		for index in cities.size():
			var city: Variant = cities[index]
			if typeof(city) != TYPE_DICTIONARY:
				continue
			var nation := String(city.get("nation", ""))
			if String(city.get("id", "")) != _input_ids[index] \
					or String(city.get("home_nation", nation)) != _input_homes[index] \
					or float(city.get("x", 0.0)) != _input_xy[index * 2] \
					or float(city.get("y", 0.0)) != _input_xy[index * 2 + 1]:
				city_flags |= 1
			if nation != _input_nations[index]:
				city_flags |= 2
			if (city_flags & 3) == 3:
				break
	var keys: Array = overrides.keys()
	keys.sort()
	if keys.size() != _override_keys.size():
		changed |= 4
	else:
		for index in keys.size():
			if String(keys[index]) != _override_keys[index] \
					or String(overrides[keys[index]]) != _override_vals[index]:
				changed |= 4
				break
	changed |= city_flags
	if changed == 0:
		return 0
	_fill_inputs(cities, overrides, keys)
	return changed


## Guarda os insumos do dia para a comparação do dia seguinte.
func _fill_inputs(cities: Array, overrides: Dictionary, keys: Array) -> void:
	_input_ids.resize(cities.size())
	_input_homes.resize(cities.size())
	_input_nations.resize(cities.size())
	_input_xy.resize(cities.size() * 2)
	for index in cities.size():
		var city: Variant = cities[index]
		if typeof(city) != TYPE_DICTIONARY:
			continue
		var nation := String(city.get("nation", ""))
		_input_ids[index] = String(city.get("id", ""))
		_input_homes[index] = String(city.get("home_nation", nation))
		_input_nations[index] = nation
		_input_xy[index * 2] = float(city.get("x", 0.0))
		_input_xy[index * 2 + 1] = float(city.get("y", 0.0))
	_override_keys.resize(keys.size())
	_override_vals.resize(keys.size())
	for index in keys.size():
		_override_keys[index] = String(keys[index])
		_override_vals[index] = String(overrides[keys[index]])


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


## Realça no mapa político TODO o território da nação `tag`: preenchimento 8%
## mais claro e contorno de latão de 2 px (uniformes `highlight_nation`,
## `highlight_color` e `highlight_border_px` de atlas_political.gdshader).
## Devolve o índice do dono usado pelo shader (-1 = tag desconhecida/sem
## território, e o destaque é limpo).
##
## O destaque é por ÍNDICE de dono (a chave da textura `province_owner`), a
## mesma unidade que o destaque da seleção de cidade já usava: uma tag cujo
## código de atlas difere do roster (KOS/XKX) ilumina só os índices daquele
## código. Para as 8 nações jogáveis o índice é único.
##
## Os materiais registrados por `apply_nation_highlight` são reescritos aqui e
## em `clear_highlight` — quem chama a API não precisa (nem deve) lembrar de
## reaplicar: sem destaque o uniforme volta a -1 e o latão desaparece por
## completo.
func set_highlight_nation(tag: String) -> int:
	if tag.is_empty():
		clear_highlight()
		return -1
	var canonical := canonical_tag(tag)
	var index := owner_index(canonical)
	if index <= 0 and canonical != tag:
		index = owner_index(tag)
	if index <= 0:
		# Tag do roster -> código do atlas (XKX -> KOS, SSD -> SDS).
		index = _owner_index_canonical(canonical)
	if index <= 0:
		clear_highlight()
		return -1
	_highlight_tag = canonical
	_highlight_index = index
	_push_highlight()
	return index


## Remove o destaque de nação (o shader volta a `highlight_nation = -1`).
func clear_highlight() -> void:
	_highlight_tag = ""
	_highlight_index = -1
	_push_highlight()


## Tag (canônica) da nação destacada; "" quando não há destaque. Exposto para
## a interface ligar o hover do nome/bandeira (a escrita no material é
## `apply_nation_highlight`).
func highlight_nation_tag() -> String:
	return _highlight_tag


## Índice do dono destacado no shader; -1 quando não há destaque.
func highlight_nation_index() -> int:
	return _highlight_index


## Escreve o destaque atual no material do mapa político e REGISTRA o material
## para as próximas trocas (set/clear reescrevem sozinhos). Materiais cujo
## shader não declara `highlight_nation` (overlay dos modos de calor) são
## ignorados.
func apply_nation_highlight(material: Material) -> void:
	var shader_material := material as ShaderMaterial
	if shader_material == null or shader_material.shader == null:
		return
	if not _has_uniform(shader_material.shader, "highlight_nation"):
		return
	if not _is_registered(shader_material):
		_highlight_materials.append(weakref(shader_material))
	_write_highlight(shader_material)


## Reaplica o destaque em todos os materiais registrados (chamado por
## `set_highlight_nation`/`clear_highlight`). Materiais já liberados são
## descartados da lista.
func _push_highlight() -> void:
	var alive: Array[WeakRef] = []
	for ref: WeakRef in _highlight_materials:
		var shader_material := ref.get_ref() as ShaderMaterial
		if shader_material == null:
			continue
		alive.append(ref)
		_write_highlight(shader_material)
	_highlight_materials = alive


func _is_registered(shader_material: ShaderMaterial) -> bool:
	for ref: WeakRef in _highlight_materials:
		if ref.get_ref() == shader_material:
			return true
	return false


func _write_highlight(shader_material: ShaderMaterial) -> void:
	shader_material.set_shader_parameter("highlight_nation", _highlight_index)
	shader_material.set_shader_parameter("highlight_color", HIGHLIGHT_BRASS)
	shader_material.set_shader_parameter("highlight_border_px", HIGHLIGHT_BORDER_PX)


## Primeiro índice de dono cujo tag canônico é `canonical` (a mesma nação pode
## ter mais de um código no raster: o da província e o da tag de cidade).
func _owner_index_canonical(canonical: String) -> int:
	for key: Variant in _tag_index:
		if canonical_tag(String(key)) == canonical:
			return int(_tag_index[key])
	return -1


static func _has_uniform(shader: Shader, uniform_name: String) -> bool:
	for uniform: Dictionary in shader.get_shader_uniform_list():
		if String(uniform.get("name", "")) == uniform_name:
			return true
	return false


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


## Tag canônica (a do nations.csv/jogo) de um dono. O raster admin-0 usa tags
## que divergem do cenário em três casos — NLD/NED, KOS/XKX e SDS/SSD — e sem a
## normalização o minimapa e o mapa político cairiam na cor neutra do atlas.
## `province` só é necessário para o caso NLD (o registro tem `nation = NED`).
func canonical_tag(tag: String, province: Dictionary = {}) -> String:
	if _palette.has(tag):
		return tag
	var mapped := GeographyScript.nation_for_province_tag(tag)
	if mapped != "":
		return mapped
	if not province.is_empty():
		var nation := String(province.get("nation", ""))
		if _palette.has(nation):
			return nation
		var mapped_nation := GeographyScript.nation_for_province_tag(nation)
		if mapped_nation != "":
			return mapped_nation
	return tag


## Tags canônicas de TODAS as nações com território no mundo (donos atuais).
func nation_tags() -> Array[String]:
	var found: Dictionary = {}
	for province in _provinces:
		var pid: int = province["id"]
		var raw := String(_owners.get(pid, String(province["country"])))
		found[canonical_tag(raw, province)] = true
	var tags: Array[String] = []
	for tag: Variant in found:
		tags.append(String(tag))
	tags.sort()
	return tags


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


## Âncora do rótulo de uma nação: polo de inacessibilidade (centro do maior
## círculo inscrito) do MAIOR pedaço contíguo do território, com o eixo principal
## (PCA) e as dimensões úteis. O formato antigo (`x`, `y`, `extent_px`) continua
## válido para os chamadores legados.
func nation_anchor(tag: String) -> Dictionary:
	var model := nation_label_model(tag)
	if model.is_empty():
		return {}
	return {
		"x": float(model["x"]),
		"y": float(model["y"]),
		"extent_px": float(model["extent_px"]),
	}


## Modelo de TODOS os rótulos de nação (tag -> dicionário). Calculado uma vez por
## conjunto de donos: amostra o raster de províncias numa grade 512×256, separa
## o maior componente contíguo de cada nação e deriva polo, eixo e dimensões.
func nation_label_models() -> Dictionary:
	if not _nation_models_ready:
		_build_nation_label_models()
	return _nation_models


func nation_label_model(tag: String) -> Dictionary:
	var models := nation_label_models()
	var model: Dictionary = models.get(tag, {})
	return model


## Tag canônica do dono numa UV do mapa (0..1). Vazio no mar/desconhecido.
func canonical_owner_at_uv(uv: Vector2) -> String:
	if uv.x < 0.0 or uv.x > 1.0 or uv.y < 0.0 or uv.y > 1.0:
		return ""
	if not land_at_uv(uv):
		return ""
	return canonical_tag(owner_at_uv(uv))


func _build_nation_label_models() -> void:
	_nation_models_ready = true
	_nation_models.clear()
	_pid_canonical.clear()
	var ids := _world_province_raster()
	var sdf := _world_sdf_raster()
	if ids == null or _provinces.is_empty():
		return

	# id de província -> tag canônica do dono atual (fonte: _owners + cenário).
	for province: Dictionary in _provinces:
		var pid := int(province["id"])
		_pid_canonical[pid] = canonical_tag(
				String(_owners.get(pid, String(province["country"]))), province)

	var gw := NATION_LABEL_GRID.x
	var gh := NATION_LABEL_GRID.y
	var total := gw * gh
	var grid := PackedInt32Array()
	grid.resize(total)
	var tag_ids: Dictionary = {}
	var id_tags: Array[String] = []
	var iw := ids.get_width()
	var ih := ids.get_height()
	var sw := sdf.get_width() if sdf != null else 0
	var sh := sdf.get_height() if sdf != null else 0
	for gy in gh:
		var sy := clampi(int((float(gy) + 0.5) / float(gh) * float(ih)), 0, ih - 1)
		var sfy := clampi(int((float(gy) + 0.5) / float(gh) * float(sh)), 0, sh - 1) \
				if sdf != null else 0
		for gx in gw:
			if sdf != null:
				var sfx := clampi(int((float(gx) + 0.5) / float(gw) * float(sw)),
						0, sw - 1)
				if sdf.get_pixel(sfx, sfy).r <= 0.5:
					continue
			var sx := clampi(int((float(gx) + 0.5) / float(gw) * float(iw)),
					0, iw - 1)
			var color := ids.get_pixel(sx, sy)
			var pid := int(round(color.r * 255.0 + color.g * 255.0 * 256.0))
			var tag := String(_pid_canonical.get(pid, ""))
			if tag.is_empty():
				continue
			var tid := int(tag_ids.get(tag, 0))
			if tid == 0:
				id_tags.append(tag)
				tid = id_tags.size()
				tag_ids[tag] = tid
			grid[gy * gw + gx] = tid

	# Maior componente 4-conectado de cada nação, com wrap em X.
	var seen := PackedByteArray()
	seen.resize(total)
	var largest: Dictionary = {}
	var queue := PackedInt32Array()
	for start in total:
		if seen[start] != 0 or grid[start] == 0:
			continue
		var tid := grid[start]
		queue.clear()
		queue.append(start)
		seen[start] = 1
		var comp := PackedInt32Array()
		var head := 0
		while head < queue.size():
			var cell := queue[head]
			head += 1
			comp.append(cell)
			@warning_ignore("integer_division")
			var cy := cell / gw
			var cx := cell - cy * gw
			for dir in 4:
				var nx := cx + (1 if dir == 0 else (-1 if dir == 1 else 0))
				var ny := cy + (1 if dir == 2 else (-1 if dir == 3 else 0))
				if ny < 0 or ny >= gh:
					continue
				if nx < 0:
					nx += gw
				elif nx >= gw:
					nx -= gw
				var next := ny * gw + nx
				if seen[next] == 0 and grid[next] == tid:
					seen[next] = 1
					queue.append(next)
		var previous: PackedInt32Array = largest.get(tid, PackedInt32Array())
		if comp.size() > previous.size():
			largest[tid] = comp

	for tid: Variant in largest:
		var comp: PackedInt32Array = largest[tid]
		var tag: String = id_tags[int(tid) - 1]
		_nation_models[tag] = _model_from_component(comp, gw, gh)


## Deriva polo, eixo (limitado a ±35°) e dimensões do maior componente. As
## coordenadas X são "desenroladas" para o componente atravessar o antimeridiano
## como um bloco contíguo (Rússia, Fiji).
func _model_from_component(comp: PackedInt32Array, gw: int, gh: int) -> Dictionary:
	var ref := comp[0]
	@warning_ignore("integer_division")
	var ref_y := ref / gw
	var ref_x := ref - ref_y * gw
	var count := comp.size()
	var xs := PackedInt32Array()
	xs.resize(count)
	var ys := PackedInt32Array()
	ys.resize(count)
	var min_x := gw * 2
	var max_x := -1
	var min_y := gh
	var max_y := -1
	for index in count:
		var cell := comp[index]
		@warning_ignore("integer_division")
		var y := cell / gw
		var x := cell - y * gw
		# Desenrola em torno da célula de referência: um país normal fica
		# compacto; só quem cruza o antimeridiano vira um bloco contínuo.
		var unwrapped := ref_x + (posmod(x - ref_x + gw / 2, gw) - gw / 2)
		xs[index] = unwrapped
		ys[index] = y
		min_x = mini(min_x, unwrapped)
		max_x = maxi(max_x, unwrapped)
		min_y = mini(min_y, y)
		max_y = maxi(max_y, y)

	# PCA: eixo principal do componente (coordenadas de célula, X desenrolado).
	var mean_x := 0.0
	var mean_y := 0.0
	for index in count:
		mean_x += float(xs[index]) + 0.5
		mean_y += float(ys[index]) + 0.5
	mean_x /= float(count)
	mean_y /= float(count)
	var cov_xx := 0.0
	var cov_yy := 0.0
	var cov_xy := 0.0
	for index in count:
		var dx := float(xs[index]) + 0.5 - mean_x
		var dy := float(ys[index]) + 0.5 - mean_y
		cov_xx += dx * dx
		cov_yy += dy * dy
		cov_xy += dx * dy
	var angle := 0.5 * atan2(2.0 * cov_xy, cov_xx - cov_yy)
	# PCA é um eixo não orientado: normaliza para (-90°, 90°].
	if angle > PI * 0.5:
		angle -= PI
	elif angle <= -PI * 0.5:
		angle += PI
	# Eixo natural (sem limite) fica disponível como segunda tentativa de
	# inclinação quando o rótulo reto não cabe (Itália, Chile, Noruega).
	var natural_angle := angle

	# Primeiro mede o país no eixo natural (sem limite), para decidir se vale
	# inclinar o nome. O mapa de grand strategy lê melhor um rótulo reto: só
	# inclina quando o território é claramente alongado E o eixo é raso. Foi o
	# clamp cego a ±35° que deixava o "EGYPT" atravessado no deserto (o eixo
	# real do Egito é quase N-S, então o texto saía num arco torto).
	var natural_axis := Vector2(cos(angle), sin(angle))
	var natural_perp := Vector2(-natural_axis.y, natural_axis.x)
	var natural_min_axis := INF
	var natural_max_axis := -INF
	var natural_min_perp := INF
	var natural_max_perp := -INF
	for index in count:
		var dx := float(xs[index]) + 0.5 - mean_x
		var dy := float(ys[index]) + 0.5 - mean_y
		var a := dx * natural_axis.x + dy * natural_axis.y
		var p := dx * natural_perp.x + dy * natural_perp.y
		natural_min_axis = minf(natural_min_axis, a)
		natural_max_axis = maxf(natural_max_axis, a)
		natural_min_perp = minf(natural_min_perp, p)
		natural_max_perp = maxf(natural_max_perp, p)
	var natural_axis_span := natural_max_axis - natural_min_axis
	var natural_perp_span := natural_max_perp - natural_min_perp
	var max_tilt := deg_to_rad(22.0)
	if absf(angle) > max_tilt or natural_axis_span < natural_perp_span * 1.6:
		angle = 0.0

	var axis := Vector2(cos(angle), sin(angle))
	var perp := Vector2(-axis.y, axis.x)
	var min_axis := INF
	var max_axis := -INF
	var min_perp := INF
	var max_perp := -INF
	for index in count:
		var dx := float(xs[index]) + 0.5 - mean_x
		var dy := float(ys[index]) + 0.5 - mean_y
		var a := dx * axis.x + dy * axis.y
		var p := dx * perp.x + dy * perp.y
		min_axis = minf(min_axis, a)
		max_axis = maxf(max_axis, a)
		min_perp = minf(min_perp, p)
		max_perp = maxf(max_perp, p)

	# Polo de inacessibilidade: maior distância ao contorno do componente,
	# calculada por transformada de distância em duas passadas no bounding box.
	var bw := max_x - min_x + 1
	var bh := max_y - min_y + 1
	var mask := PackedByteArray()
	mask.resize(bw * bh)
	for index in count:
		mask[(ys[index] - min_y) * bw + (xs[index] - min_x)] = 1
	var dist := PackedFloat32Array()
	dist.resize(bw * bh)
	dist.fill(INF)
	var diagonal := 1.41421356
	for y in bh:
		for x in bw:
			var i := y * bw + x
			if mask[i] == 0:
				continue
			if x == 0 or x == bw - 1 or y == 0 or y == bh - 1 \
					or mask[i - 1] == 0 or mask[i + 1] == 0 \
					or mask[i - bw] == 0 or mask[i + bw] == 0:
				dist[i] = 1.0
	for y in bh:
		for x in bw:
			var i := y * bw + x
			if mask[i] == 0:
				continue
			var d := dist[i]
			if x > 0:
				d = minf(d, dist[i - 1] + 1.0)
			if y > 0:
				d = minf(d, dist[i - bw] + 1.0)
				if x > 0:
					d = minf(d, dist[i - bw - 1] + diagonal)
				if x + 1 < bw:
					d = minf(d, dist[i - bw + 1] + diagonal)
			dist[i] = d
	for y in range(bh - 1, -1, -1):
		for x in range(bw - 1, -1, -1):
			var i := y * bw + x
			if mask[i] == 0:
				continue
			var d := dist[i]
			if x + 1 < bw:
				d = minf(d, dist[i + 1] + 1.0)
			if y + 1 < bh:
				d = minf(d, dist[i + bw] + 1.0)
				if x > 0:
					d = minf(d, dist[i + bw - 1] + diagonal)
				if x + 1 < bw:
					d = minf(d, dist[i + bw + 1] + diagonal)
			dist[i] = d
	var pole_i := 0
	var pole_dist := 0.0
	for i in dist.size():
		if mask[i] == 0:
			continue
		if dist[i] > pole_dist:
			pole_dist = dist[i]
			pole_i = i
	@warning_ignore("integer_division")
	var pole_y := min_y + pole_i / bw
	var pole_x := min_x + pole_i - (pole_y - min_y) * bw
	pole_x = posmod(pole_x, gw)

	var cell_w := MAP_SIZE_PX.x / float(gw)
	var cell_h := MAP_SIZE_PX.y / float(gh)
	var width_px := (max_axis - min_axis + 1.0) * cell_w
	var height_px := (max_perp - min_perp + 1.0) * cell_h
	var area_px := float(count) * cell_w * cell_h
	return {
		"tag": "",
		"x": (float(pole_x) + 0.5) * cell_w,
		"y": (float(pole_y) + 0.5) * cell_h,
		"anchor_px": Vector2((float(pole_x) + 0.5) * cell_w,
				(float(pole_y) + 0.5) * cell_h),
		"angle": angle,
		"natural_angle": natural_angle,
		"natural_width_px": (natural_axis_span + 1.0) * cell_w,
		"natural_height_px": (natural_perp_span + 1.0) * cell_h,
		"width_px": width_px,
		"height_px": height_px,
		"axis_px": width_px,
		"perp_px": height_px,
		"tilted": absf(angle) > 0.001,
		"radius_px": pole_dist * minf(cell_w, cell_h),
		"area_px": area_px,
		"extent_px": sqrt(area_px),
		"cells": count,
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
	var canonical := canonical_tag(tag, province)
	if _palette.has(canonical):
		return _palette[canonical]
	if _nation_colors.has(canonical):
		return _nation_colors[canonical]
	if _palette.has(tag):
		return _palette[tag]
	if _nation_colors.has(tag):
		return _nation_colors[tag]
	return province["color"]
