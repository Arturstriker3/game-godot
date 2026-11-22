extends Node3D

## Camada visual dos modos de mapa (população, economia, indústria, recursos).
##
## Um único plano sobre a base geográfica desenha halos de calor a partir das
## cidades. Os valores vêm exclusivamente do status do EconomySim e do
## GeographyService — não existe economia paralela. Trocar de modo só altera
## uniformes; nenhuma geometria é reconstruída.

const MODE_SHADER: Shader = preload("res://map/map_shaders/map_modes.gdshader")
const GeographyScript: Script = preload("res://core/economy/geography_service.gd")
const MapPickScript: Script = preload("res://map/map_pick.gd")

const MAP_PX := Vector2(5000.0, 2500.0)
const MAX_CITIES := 16

const RESOURCE_DOT: Texture2D = preload("res://assets/icons/places/resource_dot.png")
const RESOURCE_WHEAT: Texture2D = preload("res://assets/icons/places/resource_wheat.png")
const DEPOSIT_HOVER: Texture2D = preload("res://assets/icons/places/deposit_hover.png")
const RESOURCE_POINTS_PATH := "res://map/map_data/atlas/resource_points.json"
## Ordem dos recursos em resource_points.json (índice do mineral).
const MINERAL_KEYS := ["coal", "copper", "gas", "iron", "oil",
		"bauxite", "lead", "gold", "phosphate", "tungsten",
		"rare_earths", "sulfur", "diamond", "uranium"]
const MINERAL_COLORS := [
	Color("#3E3B38"), Color("#C06A3C"), Color("#4E9FB0"),
	Color("#7C8894"), Color("#5B4A2E"),
	Color("#B5553A"), Color("#6E7B8B"), Color("#D4A017"),
	Color("#C7B8E6"), Color("#33526B"), Color("#3FA57C"),
	Color("#F0E14A"), Color("#A8D5E2"), Color("#A6C23C"),
]
const WHEAT_COLOR := Color("#D9B45A")
const CROP_KEYS: Array[String] = ["wheat", "rice", "maize", "potato", "soy", "cotton", "wood"]
const CROP_COLORS := [
	Color("#D9B45A"), Color("#6FA8DC"), Color("#E8B028"),
	Color("#A98B5F"), Color("#7FA86B"), Color("#E8E0D0"),
	Color("#5B8C5A"),
]

## --- Mapa de energia (E6) ---------------------------------------------------
## Mapa de POTENCIAL renovável (onde vale instalar), na MESMA infraestrutura de
## marcadores do mapa de recursos: um MultiMesh por fonte, seleção/hover com os
## mesmos anéis, filtro e ficha na legenda.
##
## Fontes (ícones power_* ilustrados) e valor por cidade:
##   solar -> PVOUT kWh/kWp/ano (Global Solar Atlas);
##   wind  -> densidade de potência W/m² a 100 m (Global Wind Atlas);
##   hydro -> potencial técnico em MW (hydro_potential.json, E1).
## O valor não é convertido em MW para solar/eólica: o dado é de RECURSO, e
## inventar um teto de potência seria inventar dado de simulação.
const ENERGY_SOURCES: Array[String] = ["solar", "wind", "hydro"]
const ENERGY_TEXTURES := {
	"solar": preload("res://assets/icons/illustrated/places/power_solar.png"),
	"wind": preload("res://assets/icons/illustrated/places/power_wind.png"),
	"hydro": preload("res://assets/icons/illustrated/places/power_hydro.png"),
}
## Unidade declarada de cada fonte (a ficha e a legenda usam esta tabela).
const ENERGY_UNITS := {
	"solar": "kWh/kWp/ano",
	"wind": "W/m²",
	"hydro": "MW",
}
const ENERGY_RENEWABLE_PATH := \
		"res://data/definitions/energy/renewable_potential.json"
const ENERGY_HYDRO_PATH := \
		"res://data/definitions/energy/hydro_potential.json"
const ENERGY_PLANTS_PATH := \
		"res://data/definitions/energy/power_plants_2026_scaled.json"
const ENERGY_CITIES_PATH := "res://data/definitions/scenario/cities.json"
## Classes de potencial e limiares DECLARADOS a partir da distribuição das
## cidades COM DADO (valor > 0) por fonte, 2026-11: quartis p25/p50/p75
## calculados como `sorted[floor(q * n)]` em tools/e6_energy_map_test.gd, que
## reconfere estes números contra os arquivos. valor <= p25 = fraco;
## <= p50 = médio; <= p75 = bom; acima = excelente. Hídrico: 452 cidades com
## potencial > 0; as 397 sem bacia não geram marcador (vazio honesto).
const ENERGY_CLASSES: Array[String] = ["weak", "medium", "good", "excellent"]
const ENERGY_THRESHOLDS := {
	"solar": [1314.8, 1507.0, 1681.8],
	"wind": [117.9, 200.0, 301.5],
	"hydro": [262.5, 2331.3, 10586.5],
}
## Cor e tamanho do marcador pela CLASSE: excelente é o maior e verde forte,
## fraco o menor e cinza. A cor vale já no mundo inteiro — o ponto não compete
## mais com o ícone de cidade nem esconde a classe em zoom afastado.
const ENERGY_CLASS_COLORS := {
	"weak": Color("#8C8C8C"),
	"medium": Color("#D99A2B"),
	"good": Color("#A9C24B"),
	"excellent": Color("#3F8F43"),
}
const ENERGY_CLASS_SIZE := {
	"weak": 0.55, "medium": 0.82, "good": 1.12, "excellent": 1.50,
}
## Piso em px de TELA por classe: o marcador nunca vira pontinho, em nenhum
## zoom. O `power_*` senta num disco de fundo na cor da classe, que o separa do
## mapa e dos ícones de cidade.
const ENERGY_CLASS_MIN_PX := {
	"weak": 22.0, "medium": 26.0, "good": 31.0, "excellent": 36.0,
}
const ENERGY_DISC_SCALE := 1.42
const ENERGY_DISC_ALPHA := 0.96
const ENERGY_AGGREGATE_SCALE := 1.18
## Agregado de nação (zoom afastado): um pouco maior que o piso da própria
## classe, para ler como resumo do MELHOR sítio da nação.
const ENERGY_AGGREGATE_EXTRA_PX := 4.0
## Fração de mudança da distância da câmera que refaz o piso de tela. O piso é
## recalculado só quando o zoom muda de verdade (nunca por frame parado).
const ENERGY_FLOOR_STEP := 0.04
## Célula (px de mapa) da grade espacial de pick: evita projetar os ~2 mil
## marcadores por frame. 64 px = 12,8 unidades de mundo.
const ENERGY_PICK_CELL := 64.0
const ENERGY_PICK_CELLS := 2

const MODE_COLORS := {
	"population": [Color("#3A5563"), Color("#C9A24A")],
	"economic": [Color("#3A5563"), Color("#7FA86B")],
	"industrial": [Color("#3A5563"), Color("#D08A3E")],
	"resources": [Color("#3A5563"), Color("#8FA36B")],
}

## Tamanho do marcador (unidades de mundo), CONSTANTE para minerais E trigo:
## decisão de design — no mapa de recursos todos os depósitos têm o MESMO
## tamanho; reserva e aptidão aparecem apenas na ficha da legenda.
const MARKER_SIZE := 1.0
## Largura em pixels da textura do anel (deposit_hover.png) — usada para
## converter diâmetro em unidades de mundo ↔ pixel_size do Sprite3D.
const RING_TEXTURE_PX := 128.0
## Fator do anel sobre o tamanho do ponto: hover = 1.8×MARKER_SIZE, seleção =
## 2.8×MARKER_SIZE. Como o ponto agora é constante, os anéis também são — e
## sempre maiores que o ponto, para o anel nunca sumir dentro/sob ele.
const HOVER_RING_FACTOR := 1.8
const SELECTION_RING_FACTOR := 2.8
## Piso em TELA (px) para os anéis não desaparecerem em zoom afastado.
const HOVER_MIN_SCREEN_PX := 22.0
const SELECTION_MIN_SCREEN_PX := 30.0

var _material: ShaderMaterial
var _uv := PackedVector2Array()
var _values: Dictionary = {}
var _current := "geographic"
var _heat_mesh: MeshInstance3D
var _mineral_mesh: MultiMeshInstance3D
var _renewable_mesh: MultiMeshInstance3D
var _hover_ring: Sprite3D
var _selection_ring: Sprite3D
var _hover_time := 0.0
var _selected_deposit := -1
## Diâmetro LÓGICO (unidades de mundo) do anel de hover/seleção, antes do piso
## de tela — guardado para o _process reaplicar o piso a cada frame.
var _hover_diameter := 0.0
var _selection_diameter := 0.0
var _mineral_points: Array = []
var _renewable_points: Array = []
var _geo: RefCounted
## Último conjunto de cidades recebido em `setup`/`refresh`, guardado em
## colunas tipadas. NÃO guardamos o array de dicionários do status: ele é
## grande (mercados/prédios) e mantê-lo vivo transferia a liberação dos dicts
## do dia anterior para a virada (~0,5 ms). Só o que os modos precisam.
var _city_ids := PackedStringArray()
var _city_centers := PackedVector2Array()
var _city_population := PackedFloat64Array()
var _city_production := PackedFloat64Array()
var _city_industrialization := PackedFloat64Array()
## Quantas cidades geraram `_uv` (centros são estáticos; a UV é calculada uma vez).
var _uv_count := -1
## Filtros da legenda: {} = todos; senão chave -> bool.
var _resource_filter: Dictionary = {}

## --- Estado do mapa de energia (E6) -----------------------------------------
## Potencial por fonte e cidade: `_energy_values[fonte][city_id]` é o valor do
## recurso; `_energy_built[fonte][city_id]` é o MW já construído no parque
## atual (power_plants_2026_scaled.json). As posições vêm de cities.json.
var _energy_values: Dictionary = {}
var _energy_built_mw: Dictionary = {}
var _energy_city_px: Dictionary = {}
var _energy_cells: Dictionary = {}
var _energy_meshes: Dictionary = {}
## Disco de fundo por fonte (a cor da CLASSE) desenhado sob o `power_*`.
var _energy_disc_meshes: Dictionary = {}
## Último conjunto escrito nos MultiMesh, para refazer só o piso de tela quando
## o zoom muda dentro da mesma faixa (pan não reescreve nada).
var _energy_buckets: Dictionary = {}
var _energy_floor_distance := -1.0
## Marcadores visíveis no nível de zoom atual (cidade×fonte perto/médio,
## nação×fonte no longe). O pick devolve índice NESTA lista.
var _energy_drawn: Array = []
var _energy_filter: Dictionary = {}
var _energy_band := -1
var _energy_built := false
var _energy_hover := -1
var _energy_selected := -1
var _city_names: Dictionary = {}
var _city_nations: Dictionary = {}


## Constrói a camada e normaliza os indicadores de cada modo. `shared_geo` é o
## GeographyService já carregado pelo chamador (mapa/Economy) — reutilizado para
## não reparsear deposits.json (3,3 MB) numa segunda instância.
func setup(cities: Array, shared_geo: RefCounted = null) -> void:
	_material = ShaderMaterial.new()
	_material.shader = MODE_SHADER

	var plane := PlaneMesh.new()
	plane.size = Vector2(1000.0, 500.0)

	var mesh := MeshInstance3D.new()
	mesh.mesh = plane
	mesh.material_override = _material
	mesh.position = Vector3(0.0, 0.12, 0.0)
	mesh.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	add_child(mesh)
	_heat_mesh = mesh

	# Geografia carregada UMA vez. Antes cada `refresh` (virada do dia) criava
	# um GeographyService e reparseava deposits.json (3,3 MB) — ~2,9 s de
	# travamento por dia com o mapa de recursos ativo. Com o serviço compartilhado
	# do Economy, `load_data()` retorna na hora; sem ele, `set_geography()` chega
	# depois do build (carregamento em thread, ver CityWorld).
	_geo = shared_geo
	if _geo != null:
		_geo.load_data()

	# Camada do mapa de recursos: marcadores em billboard (nítidos em qualquer
	# zoom), gerados de map/map_data/atlas/resource_points.json.
	_build_resource_markers()

	# Parâmetros fixos do campo de calor (não dependem do dia).
	_material.set_shader_parameter("heat_radius", 0.055)
	_material.set_shader_parameter("opacity", 0.45)

	_capture(cities)
	if cities is Array:
		for entry: Variant in cities:
			if not (entry is Dictionary):
				continue
			var city: Dictionary = entry
			var city_id := String(city.get("id", ""))
			if city_id.is_empty():
				continue
			_city_names[city_id] = String(city.get("name", ""))
			_city_nations[city_id] = String(city.get("nation", ""))
	visible = false


func current_mode() -> String:
	return _current


## Recebe a geografia que chegou DEPOIS do setup (carregamento em thread no
## mapa, quando o Economy ainda não tinha o serviço). Os buffers de tela não
## mudam: os modos de calor não usam geografia e os marcadores de recurso vêm de
## resource_points.json. O que passa a ter dado é a ficha do depósito (nomes,
## fertilidade, donos).
func set_geography(geo: RefCounted) -> void:
	if geo == null or geo == _geo:
		return
	_geo = geo
	_geo.load_data()


## Atualiza os valores do modo ATIVO a partir do status atual sem recriar a
## camada. Modos de calor recalculam só a própria série; o mapa de recursos
## (marcadores) e o mapa político não têm custo por dia — as jazidas são
## estáticas e os centros das cidades não mudam.
## A4: recebe as COLUNAS por cidade (`Economy.city_columns()`), não mais uma
## Array de Dictionary — o mapa não monta o mundo para pintar o calor. Um Array
## de linhas (build/testes) continua aceito por compatibilidade.
func refresh(source: Variant) -> void:
	if source == null or source.is_empty():
		return
	_capture(source)
	if _current == "resources" or _current == "energy" \
			or not MODE_COLORS.has(_current):
		return
	_build_values(_current)
	_publish(_current)


## Liga/desliga as camadas sem recriar os planos. O mapa de recursos e o de
## energia usam marcadores; os demais mostram o campo de calor.
func set_mode(mode: String) -> void:
	_current = mode
	var resources := mode == "resources"
	var energy := mode == "energy"
	var heat := MODE_COLORS.has(mode) and not resources
	if energy:
		_ensure_energy_built()
	if _mineral_mesh != null:
		_mineral_mesh.visible = resources
	if _renewable_mesh != null:
		_renewable_mesh.visible = resources
	if _heat_mesh != null:
		_heat_mesh.visible = heat
	if _hover_ring != null and not resources and not energy:
		_hover_ring.visible = false
	if energy:
		# Nível de zoom atual: perto = usina, médio = cidade×fonte, longe =
		# nação×fonte. Evita 36 mil ícones no mapa afastado.
		_energy_band = -1
		_rebuild_energy()
	else:
		for fuel: Variant in _energy_meshes:
			var mesh: MultiMeshInstance3D = _energy_meshes[fuel]
			mesh.visible = false
		for fuel: Variant in _energy_disc_meshes:
			var disc: MultiMeshInstance3D = _energy_disc_meshes[fuel]
			disc.visible = false
		if _energy_hover >= 0:
			_energy_hover = -1
	_update_selection_ring()
	visible = resources or heat or energy

	if not heat:
		return

	var colors: Array = MODE_COLORS[mode]
	_material.set_shader_parameter("low_color", colors[0])
	_material.set_shader_parameter("high_color", colors[1])
	# Série sob demanda: garante valores do dia atual mesmo se o modo ficou
	# inativo por vários dias (custo: 40 cidades × geografia já carregada).
	_build_values(mode)
	_publish(mode)
	visible = true


## Copia só os campos que os modos usam — colunas tipadas (A4) ou, por
## compatibilidade com o build/testes, um Array de linhas de cidade.
func _capture(source: Variant) -> void:
	if source is Dictionary:
		var columns: Dictionary = source
		var ids: PackedStringArray = columns.get("id", PackedStringArray())
		var xs: PackedFloat64Array = columns.get("x", PackedFloat64Array())
		var ys: PackedFloat64Array = columns.get("y", PackedFloat64Array())
		var count := ids.size()
		_city_ids = ids
		_city_centers.resize(count)
		_city_population = columns.get("population", PackedFloat64Array())
		_city_production = columns.get("production", PackedFloat64Array())
		_city_industrialization = columns.get("industrialization",
				PackedFloat64Array())
		for index in count:
			_city_centers[index] = Vector2(
					xs[index] if index < xs.size() else 0.0,
					ys[index] if index < ys.size() else 0.0)
		return
	var cities: Array = source
	var count := cities.size()
	_city_ids.resize(count)
	_city_centers.resize(count)
	_city_population.resize(count)
	_city_production.resize(count)
	_city_industrialization.resize(count)
	for index in count:
		var city: Dictionary = cities[index]
		_city_ids[index] = String(city.get("id", ""))
		_city_centers[index] = Vector2(float(city.get("x", 0.0)), float(city.get("y", 0.0)))
		_city_population[index] = float(city.get("population", 0.0))
		_city_production[index] = float(city.get("production", 0.0))
		_city_industrialization[index] = float(city.get("industrialization", 0.0))


## Empacota UV (uma vez) e a série do modo pedido, normalizada 0..1.
func _build_values(mode: String) -> void:
	_ensure_uv()

	var max_population := 0.0
	var max_production := 0.0
	if mode == "population" or mode == "economic":
		for value in _city_population:
			max_population = maxf(max_population, value)
		for value in _city_production:
			max_production = maxf(max_production, value)

	# Recursos = jazidas PRÓPRIAS da cidade (regra A1). Antes contava as do
	# território: cidades vizinhas no mesmo território acendiam igual.
	var values := PackedFloat32Array()
	for index in _city_ids.size():
		var value := 0.0
		match mode:
			"population":
				value = _log_ratio(_city_population[index], max_population)
			"economic":
				value = clampf(_city_production[index] / maxf(max_production, 1.0), 0.0, 1.0)
			"industrial":
				value = clampf(_city_industrialization[index], 0.0, 1.0)
			"resources":
				var wheat := 0.0
				var deposits: Array = []
				if _geo != null:
					wheat = _geo.crop_suitability(
							_city_ids[index], _city_centers[index], "wheat")
					deposits = _geo.deposits_for_city(_city_ids[index])
				value = clampf(wheat * 0.75 + minf(float(deposits.size()), 4.0) * 0.08,
						0.0, 1.0)
		values.append(value)

	_values[mode] = _pad(values)


## UV normalizada dos centros. Os centros vêm do cenário e não mudam no dia a
## dia: calcula uma vez por conjunto de cidades.
func _ensure_uv() -> void:
	if _uv_count == _city_centers.size():
		return
	_uv.clear()
	for center in _city_centers:
		_uv.append(Vector2(center.x / MAP_PX.x, center.y / MAP_PX.y))
	_uv.resize(MAX_CITIES)
	_uv_count = _city_centers.size()
	_material.set_shader_parameter("city_uv", _uv)


func _publish(mode: String) -> void:
	_material.set_shader_parameter("city_count", _city_ids.size())
	_material.set_shader_parameter("city_value", _values[mode])


## Constrói os marcadores do mapa de recursos: minerais (círculo) e renováveis
## (losango), um MultiMesh por tipo. Billboard mantém o desenho nítido.
func _build_resource_markers() -> void:
	var data: Variant = JSON.parse_string(
			FileAccess.get_file_as_string(RESOURCE_POINTS_PATH))
	if typeof(data) != TYPE_DICTIONARY:
		return
	_mineral_points = data.get("minerals", [])
	_renewable_points = data.get("renewables", [])

	_mineral_mesh = _make_marker_layer("ResourceMinerals", RESOURCE_DOT)
	_renewable_mesh = _make_marker_layer("ResourceRenewables", RESOURCE_WHEAT)
	_rebuild_markers()

	# Anel de hover em volta do depósito sob o mouse.
	_hover_ring = Sprite3D.new()
	_hover_ring.name = "DepositHoverRing"
	_hover_ring.texture = DEPOSIT_HOVER
	_hover_ring.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	_hover_ring.transparent = true
	_hover_ring.no_depth_test = true
	_hover_ring.render_priority = 3
	_hover_ring.visible = false
	add_child(_hover_ring)

	# Anel de SELEÇÃO (persistente) do depósito clicado.
	_selection_ring = Sprite3D.new()
	_selection_ring.name = "DepositSelectionRing"
	_selection_ring.texture = DEPOSIT_HOVER
	_selection_ring.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	_selection_ring.transparent = true
	_selection_ring.no_depth_test = true
	_selection_ring.render_priority = 3
	_selection_ring.modulate = Color(1.0, 0.92, 0.72, 1.0)
	_selection_ring.visible = false
	add_child(_selection_ring)

	_mineral_mesh.visible = false
	_renewable_mesh.visible = false


## Aplica o filtro da legenda (chaves: coal/copper/gas/iron/oil/wheat).
## Dicionário vazio = tudo ligado.
func set_resource_filter(enabled: Dictionary) -> void:
	_resource_filter = enabled.duplicate()
	_rebuild_markers()


func _is_enabled(key: String) -> bool:
	return _resource_filter.is_empty() or bool(_resource_filter.get(key, true))


# --- Mapa de energia (E6) ----------------------------------------------------

## Carrega uma vez (lazy: o build do mapa político não paga o parse dos três
## arquivos). Posições de cities.json; potencial de renewable/hydro_potential;
## construído do parque atual. Cria um MultiMesh por fonte com a MESMA fábrica
## de marcadores do mapa de recursos.
func _ensure_energy_built() -> void:
	if _energy_built:
		return
	_energy_built = true
	_load_energy_cities()
	for source in ENERGY_SOURCES:
		_energy_values[source] = {}
		_energy_built_mw[source] = {}
		# Disco na cor da CLASSE por baixo do ícone (prioridade menor): destaca
		# o marcador do mapa e do ícone de cidade mesmo em zoom afastado.
		_energy_disc_meshes[source] = _make_marker_layer(
				"Energy%sDisc" % source.capitalize(), RESOURCE_DOT, 1)
		_energy_disc_meshes[source].visible = false
		_energy_meshes[source] = _make_marker_layer(
				"Energy%s" % source.capitalize(), ENERGY_TEXTURES[source], 3)
		_energy_meshes[source].visible = false
	var renewable: Variant = JSON.parse_string(
			FileAccess.get_file_as_string(ENERGY_RENEWABLE_PATH))
	if typeof(renewable) == TYPE_DICTIONARY:
		var rows: Dictionary = (renewable as Dictionary).get("by_city", {})
		for city_id: Variant in rows:
			var key := str(city_id)
			var row: Dictionary = rows[city_id]
			var solar: Dictionary = row.get("solar", {})
			var wind: Dictionary = row.get("wind", {})
			_set_energy_value("solar", key,
					_json_number(solar.get("pvout_kwh_kwp_year")))
			_set_energy_value("wind", key,
					_json_number(wind.get("power_density_w_m2")))
	var hydro: Variant = JSON.parse_string(
			FileAccess.get_file_as_string(ENERGY_HYDRO_PATH))
	if typeof(hydro) == TYPE_DICTIONARY:
		var rows: Dictionary = (hydro as Dictionary).get("by_city", {})
		for city_id: Variant in rows:
			var row: Dictionary = rows[city_id]
			_set_energy_value("hydro", str(city_id),
					_json_number(row.get("potential_mw")))
	_load_energy_built_mw()


func _load_energy_cities() -> void:
	var data: Variant = JSON.parse_string(
			FileAccess.get_file_as_string(ENERGY_CITIES_PATH))
	if not (data is Array):
		return
	for entry: Variant in data:
		if not (entry is Dictionary):
			continue
		var city: Dictionary = entry
		var city_id := str(city.get("id", ""))
		if city_id.is_empty():
			continue
		_energy_city_px[city_id] = Vector2(
				_json_number(city.get("x")), _json_number(city.get("y")))
		if not _city_names.has(city_id):
			_city_names[city_id] = str(city.get("name", ""))
		_city_nations[city_id] = str(city.get("nation", ""))


## JSON traz `null` onde o dado falta (ex.: potencial_hidro de cidade sem
## bacia). Converter `null` com `float()` estoura no GDScript; aqui o vazio vira
## 0 e é filtrado por `_set_energy_value`.
static func _json_number(value: Variant) -> float:
	if typeof(value) == TYPE_FLOAT or typeof(value) == TYPE_INT:
		return float(value)
	return 0.0


## Só entra cidade com posição no mapa e valor > 0: sem sítio ou sem recurso
## não há marcador (o vazio é honesto). Solar/eólica têm valor em todas as 849.
func _set_energy_value(source: String, city_id: String, value: float) -> void:
	if value <= 0.0 or not _energy_city_px.has(city_id):
		return
	var values: Dictionary = _energy_values[source]
	values[city_id] = value
	_energy_values[source] = values


## MW já construídos por cidade×fonte, do parque atual do jogo. É o lado
## "construído" da ficha; o "disponível" hídrico é potencial técnico - isto.
func _load_energy_built_mw() -> void:
	var data: Variant = JSON.parse_string(
			FileAccess.get_file_as_string(ENERGY_PLANTS_PATH))
	if typeof(data) != TYPE_DICTIONARY:
		return
	for raw: Variant in (data as Dictionary).get("plants", []):
		if not (raw is Dictionary):
			continue
		var plant: Dictionary = raw
		var source := str(plant.get("fuel_group", ""))
		if not ENERGY_SOURCES.has(source):
			continue
		var city_id := str(plant.get("city_id", ""))
		if city_id.is_empty():
			continue
		var mw := _json_number(plant.get("capacity_mw"))
		if mw <= 0.0:
			continue
		var rows: Dictionary = _energy_built_mw[source]
		rows[city_id] = float(rows.get(city_id, 0.0)) + mw
		_energy_built_mw[source] = rows


## Filtro da legenda de energia (chaves ENERGY_SOURCES). Vazio = tudo ligado.
func set_energy_filter(enabled: Dictionary) -> void:
	_energy_filter = enabled.duplicate()
	if _energy_built:
		_rebuild_energy(true)


func _energy_enabled(source: String) -> bool:
	return _energy_filter.is_empty() or bool(_energy_filter.get(source, true))


## Nível de zoom do mapa (0 longe, 1 médio, 2 perto). O parent é o CityWorld.
func energy_zoom_band() -> int:
	var parent := get_parent()
	if parent != null and parent.has_method("zoom_level"):
		return clampi(int(parent.call("zoom_level")), 0, 2)
	return clampi(_energy_band, 0, 2)


## Força uma faixa de zoom (teste determinístico, sem câmera).
func set_energy_zoom_band(band: int) -> void:
	_energy_band = -1
	_rebuild_energy(true, clampi(band, 0, 2))


## Reconstrói os marcadores do nível atual: perto/médio = cidade×fonte;
## longe = nação×fonte, no sítio da MELHOR cidade da fonte naquela nação (o
## marcador fica onde há capacidade, não num centro inventado). O filtro esconde
## a fonte sem descartar o dado.
func _rebuild_energy(force: bool = false, band_override: int = -1) -> void:
	if not _energy_built:
		return
	var band := band_override if band_override >= 0 else energy_zoom_band()
	if not force and band == _energy_band and not _energy_drawn.is_empty():
		return
	var kept_selected := _energy_marker_id(_energy_selected)
	var kept_hover := _energy_marker_id(_energy_hover)
	_energy_band = band
	_energy_drawn.clear()
	_energy_hover = -1
	_energy_selected = -1
	var buckets: Dictionary = {}
	if band >= 1:
		_collect_energy_cities(buckets)
	else:
		_collect_energy_nations(buckets)
	_rebuild_energy_cells()
	_write_energy_meshes(buckets)
	if not kept_selected.is_empty():
		_energy_selected = _energy_find_marker(kept_selected)
	if not kept_hover.is_empty():
		_energy_hover = _energy_find_marker(kept_hover)
	_update_selection_ring()


func _collect_energy_cities(buckets: Dictionary) -> void:
	for source in ENERGY_SOURCES:
		if not _energy_enabled(source):
			continue
		var values: Dictionary = _energy_values.get(source, {})
		var built: Dictionary = _energy_built_mw.get(source, {})
		for city_id: Variant in values:
			var value := float(values[city_id])
			var point: Vector2 = _energy_city_px[city_id]
			var marker := {
				"marker_id": _energy_key(source, String(city_id)),
				"kind": "city",
				"group": "city",
				"source": source,
				"value": value,
				"class": energy_class(source, value),
				"city_id": String(city_id),
				"nation": String(_city_nations.get(city_id, "")),
				"px": point.x,
				"py": point.y,
				"built_mw": float(built.get(city_id, 0.0)),
				"count": 1,
			}
			if source == "hydro":
				marker["potential_mw"] = value
				marker["available_mw"] = maxf(
						value - float(marker["built_mw"]), 0.0)
			_energy_drawn.append(marker)
			_append_energy_bucket(buckets, marker)


func _collect_energy_nations(buckets: Dictionary) -> void:
	var groups: Dictionary = {}
	for source in ENERGY_SOURCES:
		if not _energy_enabled(source):
			continue
		var values: Dictionary = _energy_values.get(source, {})
		var built: Dictionary = _energy_built_mw.get(source, {})
		for city_id: Variant in values:
			var nation := String(_city_nations.get(city_id, ""))
			if nation.is_empty():
				continue
			var key := _energy_key(source, nation)
			var group: Dictionary = groups.get(key, {})
			if group.is_empty():
				group = {
					"marker_id": key,
					"kind": "aggregate",
					"group": "nation",
					"source": source,
					"nation": nation,
					"value": 0.0,
					"class": "weak",
					"best_city_id": "",
					"city_id": "",
					"px": 0.0,
					"py": 0.0,
					"built_mw": 0.0,
					"potential_mw": 0.0,
					"available_mw": 0.0,
					"count": 0,
				}
			var value := float(values[city_id])
			group["count"] = int(group["count"]) + 1
			group["built_mw"] = float(group["built_mw"]) \
					+ float(built.get(city_id, 0.0))
			if source == "hydro":
				group["potential_mw"] = float(group["potential_mw"]) + value
			if value > float(group["value"]):
				group["value"] = value
				group["best_city_id"] = String(city_id)
				group["city_id"] = String(city_id)
				var point: Vector2 = _energy_city_px[city_id]
				group["px"] = point.x
				group["py"] = point.y
			groups[key] = group
	for key: Variant in groups:
		var group: Dictionary = groups[key]
		var source := String(group["source"])
		group["class"] = energy_class(source, float(group["value"]))
		if source == "hydro":
			group["available_mw"] = maxf(
					float(group["potential_mw"]) - float(group["built_mw"]), 0.0)
		_energy_drawn.append(group)
		_append_energy_bucket(buckets, group)


func _append_energy_bucket(buckets: Dictionary, marker: Dictionary) -> void:
	var source := String(marker["source"])
	var list: Array = buckets.get(source, [])
	list.append(marker)
	buckets[source] = list


static func _energy_key(source: String, place: String) -> String:
	return "%s|%s" % [source, place]


func _energy_marker_id(index: int) -> String:
	if index < 0 or index >= _energy_drawn.size():
		return ""
	return String((_energy_drawn[index] as Dictionary).get("marker_id", ""))


func _energy_find_marker(marker_id: String) -> int:
	for index in _energy_drawn.size():
		if String((_energy_drawn[index] as Dictionary).get("marker_id", "")) \
				== marker_id:
			return index
	return -1


## Grade espacial do pick, refeita a cada troca de faixa/nível de zoom. O pick
## percorre só as células em volta do cursor, não os ~2 mil marcadores.
func _rebuild_energy_cells() -> void:
	_energy_cells.clear()
	for index in _energy_drawn.size():
		var marker: Dictionary = _energy_drawn[index]
		var cell := Vector2i(int(float(marker["px"]) / ENERGY_PICK_CELL),
				int(float(marker["py"]) / ENERGY_PICK_CELL))
		var bucket: Array = _energy_cells.get(cell, [])
		bucket.append(index)
		_energy_cells[cell] = bucket


## Classe do potencial pela distribuição declarada em ENERGY_THRESHOLDS.
## Pública: a legenda, a ficha e os testes usam a MESMA régua.
func energy_class(source: String, value: float) -> String:
	var thresholds: Array = ENERGY_THRESHOLDS.get(source, [])
	if thresholds.size() != 3:
		return "weak"
	if value <= float(thresholds[0]):
		return "weak"
	if value <= float(thresholds[1]):
		return "medium"
	if value <= float(thresholds[2]):
		return "good"
	return "excellent"


## Escreve ícone + disco nos MultiMesh da faixa atual. A cor é a da CLASSE e o
## tamanho tem piso em px de tela (ver `_energy_marker_world_size`).
func _write_energy_meshes(buckets: Dictionary) -> void:
	_energy_buckets = buckets
	var camera := get_viewport().get_camera_3d()
	for source in ENERGY_SOURCES:
		var mesh: MultiMeshInstance3D = _energy_meshes.get(source)
		var disc: MultiMeshInstance3D = _energy_disc_meshes.get(source)
		if mesh == null:
			continue
		var list: Array = buckets.get(source, [])
		var multimesh := mesh.multimesh
		if multimesh.instance_count < list.size():
			multimesh.instance_count = list.size()
		if disc != null and disc.multimesh.instance_count < list.size():
			disc.multimesh.instance_count = list.size()
		for index in list.size():
			var marker: Dictionary = list[index]
			var class_key := String(marker.get("class", "weak"))
			var class_color: Color = ENERGY_CLASS_COLORS.get(class_key,
					ENERGY_CLASS_COLORS["weak"])
			var size := _energy_marker_world_size(marker, camera)
			if disc != null:
				_set_marker(disc, index, float(marker["px"]),
						float(marker["py"]), size * ENERGY_DISC_SCALE,
						Color(class_color.r, class_color.g, class_color.b,
								ENERGY_DISC_ALPHA))
			# O ícone entra em cores naturais; a classe é lida no disco/cor.
			_set_marker(mesh, index, float(marker["px"]), float(marker["py"]),
					size, Color.WHITE)
		multimesh.visible_instance_count = list.size()
		mesh.visible = _current == "energy" and not list.is_empty()
		if disc != null:
			disc.multimesh.visible_instance_count = list.size()
			disc.visible = mesh.visible


## Tamanho lógico pela CLASSE (excelente maior, fraco menor); o agregado de
## nação cresce um pouco para ler como resumo.
func _energy_marker_size(marker: Dictionary) -> float:
	var size := float(ENERGY_CLASS_SIZE.get(
			String(marker.get("class", "weak")), 0.55))
	if String(marker.get("kind", "city")) == "aggregate":
		size *= ENERGY_AGGREGATE_SCALE
	return size


## Piso legível em px de TELA por classe (agregado um pouco maior). Sem câmera
## (teste headless) fica o tamanho lógico — nada inventado.
func _energy_marker_min_px(marker: Dictionary) -> float:
	var min_px := float(ENERGY_CLASS_MIN_PX.get(
			String(marker.get("class", "weak")), 22.0))
	if String(marker.get("kind", "city")) == "aggregate":
		min_px += ENERGY_AGGREGATE_EXTRA_PX
	return min_px


## Tamanho final em unidades de mundo: o maior entre o lógico (classe) e o que
## mede `min_px` na tela na posição do marcador.
func _energy_marker_world_size(marker: Dictionary, camera: Camera3D) -> float:
	var base := _energy_marker_size(marker)
	if camera == null:
		return base
	var world := to_global(_marker_world(
			[float(marker["px"]), float(marker["py"])]))
	if camera.is_position_behind(world):
		return base
	var px_per_world := _screen_px_per_world(camera, world)
	if px_per_world <= 0.0001:
		return base
	return maxf(base, _energy_marker_min_px(marker) / px_per_world)


## Pixels de tela por unidade de mundo no eixo horizontal da câmera.
func _screen_px_per_world(camera: Camera3D, world: Vector3) -> float:
	var right := camera.global_transform.basis.x
	var a := camera.unproject_position(world)
	var b := camera.unproject_position(world + right)
	return a.distance_to(b)


## Refaz só os tamanhos quando o zoom muda dentro da mesma faixa. Pan puro não
## passa por aqui: a distância da câmera ao chão é a mesma.
func _refresh_energy_floor(camera: Camera3D) -> void:
	if camera == null or _energy_buckets.is_empty():
		return
	var parent := get_parent()
	var distance := 0.0
	if parent != null and parent.has_method("zoom_distance"):
		distance = float(parent.call("zoom_distance"))
	else:
		distance = camera.global_position.distance_to(to_global(Vector3.ZERO))
	if distance <= 0.0:
		return
	if _energy_floor_distance > 0.0 \
			and absf(distance - _energy_floor_distance) \
					<= _energy_floor_distance * ENERGY_FLOOR_STEP:
		return
	_energy_floor_distance = distance
	_write_energy_meshes(_energy_buckets)


## Cor e piso de tela da classe de um marcador (a legenda e o teste usam a
## MESMA tabela do desenho).
func energy_marker_visual(index: int) -> Dictionary:
	if index < 0 or index >= _energy_drawn.size():
		return {}
	var marker: Dictionary = _energy_drawn[index]
	var class_key := String(marker.get("class", "weak"))
	return {
		"class": class_key,
		"color": ENERGY_CLASS_COLORS.get(class_key, Color.WHITE),
		"min_px": _energy_marker_min_px(marker),
		"size_world": _energy_marker_size(marker),
		"aggregate": String(marker.get("kind", "city")) == "aggregate",
	}


## Usina/marcador sob o cursor (índice em `_energy_drawn`), ou -1.
func pick_energy_marker(screen_position: Vector2, camera: Camera3D) -> int:
	if camera == null or _current != "energy":
		return -1
	var best := -1
	var best_distance := INF
	if energy_zoom_band() >= 1:
		for index in _energy_candidates(screen_position, camera):
			var marker: Dictionary = _energy_drawn[index]
			var distance := _energy_pick_distance(marker, screen_position, camera)
			if distance >= 0.0 and distance < best_distance:
				best = index
				best_distance = distance
		return best
	for index in _energy_drawn.size():
		var marker: Dictionary = _energy_drawn[index]
		var distance := _energy_pick_distance(marker, screen_position, camera)
		if distance >= 0.0 and distance < best_distance:
			best = index
			best_distance = distance
	return best


## Candidatos no chão em volta do cursor: grade espacial em px de mapa, em vez
## de projetar os ~2 mil marcadores por frame.
func _energy_candidates(screen_position: Vector2, camera: Camera3D) -> Array:
	var result: Array = []
	var origin := camera.project_ray_origin(screen_position)
	var direction := camera.project_ray_normal(screen_position)
	if absf(direction.y) < 0.000001:
		return result
	var plane_y := to_global(_marker_world([0.0, 0.0])).y
	var distance := (plane_y - origin.y) / direction.y
	if distance <= 0.0:
		return result
	var hit := to_local(origin + direction * distance)
	var px := hit.x / 0.2 + MAP_PX.x * 0.5
	var py := hit.z / 0.2 + MAP_PX.y * 0.5
	var center := Vector2i(int(px / ENERGY_PICK_CELL), int(py / ENERGY_PICK_CELL))
	for dx in range(-ENERGY_PICK_CELLS, ENERGY_PICK_CELLS + 1):
		for dy in range(-ENERGY_PICK_CELLS, ENERGY_PICK_CELLS + 1):
			var cell := center + Vector2i(dx, dy)
			var bucket: Array = _energy_cells.get(cell, [])
			for index: Variant in bucket:
				result.append(int(index))
	return result


func _energy_pick_distance(marker: Dictionary, screen_position: Vector2,
		camera: Camera3D) -> float:
	var world := to_global(_marker_world([float(marker["px"]), float(marker["py"])]))
	if camera.is_position_behind(world):
		return -1.0
	var center := camera.unproject_position(world)
	var radius: float = MapPickScript.pick_radius_px(camera, world,
			_energy_marker_world_size(marker, camera) * 0.5,
			MapPickScript.DEPOSIT_RADIUS_FACTOR)
	var distance := center.distance_to(screen_position)
	return distance if distance <= radius else -1.0


## Hover do marcador de energia (anel branco) ou -1.
func set_energy_hover(index: int) -> void:
	if _hover_ring == null:
		return
	if index < 0 or index >= _energy_drawn.size() or _current != "energy":
		_hover_ring.visible = false
		return
	if index == _energy_selected:
		_hover_ring.visible = false
		return
	var marker: Dictionary = _energy_drawn[index]
	var point: Array = [float(marker["px"]), float(marker["py"])]
	_hover_ring.position = _marker_world(point) + Vector3(0.0, 0.01, 0.0)
	_hover_diameter = _energy_marker_world_size(marker,
			get_viewport().get_camera_3d()) * HOVER_RING_FACTOR
	_hover_ring.pixel_size = _hover_diameter / RING_TEXTURE_PX
	_hover_ring.modulate = Color(1.0, 1.0, 1.0, 0.8)
	_hover_ring.visible = true
	_hover_time = 0.0


## Seleção do marcador de energia (anel persistente) ou -1.
func set_energy_selected(index: int) -> void:
	_energy_selected = index
	_update_selection_ring()


func selected_energy_marker() -> int:
	return _energy_selected


## Ficha do sítio (cidade×fonte) ou do agregado de nação×fonte: valor do
## recurso + classe, construído no parque atual e, no hídrico, o disponível
## (potencial técnico - construído). Solar/eólica não têm teto de MW no dado:
## `available_mw` fica -1 e a UI mostra só o construído.
func energy_marker_info(index: int) -> Dictionary:
	if index < 0 or index >= _energy_drawn.size():
		return {}
	var marker: Dictionary = _energy_drawn[index]
	var source := String(marker.get("source", "solar"))
	var city_id := String(marker.get("city_id", ""))
	var nation := String(marker.get("nation", ""))
	# Nomes ficam CRUS aqui (id + fallback do cenário): a tradução é do HUD, que
	# já tem o I18n carregado. Isso mantém a camada utilizável em teste headless
	# sem autoload e evita acoplar o mapa ao idioma.
	var city_name := String(_city_names.get(city_id, "")) if not city_id \
			.is_empty() else ""
	return {
		"kind": "energy",
		"source": source,
		"value": float(marker.get("value", 0.0)),
		"unit": String(ENERGY_UNITS.get(source, "")),
		"class": String(marker.get("class", "weak")),
		"built_mw": float(marker.get("built_mw", 0.0)),
		"potential_mw": float(marker.get("potential_mw", -1.0)),
		"available_mw": float(marker.get("available_mw", -1.0)),
		"count": int(marker.get("count", 1)),
		"city_id": city_id,
		"city": city_name,
		"best_city_id": String(marker.get("best_city_id", "")),
		"nation": nation,
		"group": String(marker.get("group", "city")),
		"aggregate": String(marker.get("kind", "city")) == "aggregate",
	}


## Cidades com dado por fonte (a legenda e os testes leem daqui). Não depende
## do zoom: é o total do arquivo com valor > 0.
func energy_source_counts() -> Dictionary:
	if not _energy_built:
		_ensure_energy_built()
	var counts := {}
	for source in ENERGY_SOURCES:
		counts[source] = (_energy_values.get(source, {}) as Dictionary).size()
	return counts


## Limiares declarados por fonte (p25/p50/p75) — a legenda mostra a régua.
func energy_thresholds() -> Dictionary:
	return ENERGY_THRESHOLDS.duplicate(true)


## Valores por cidade de uma fonte (cópia, para teste/inspeção).
func energy_city_values(source: String) -> Dictionary:
	if not _energy_built:
		_ensure_energy_built()
	return (_energy_values.get(source, {}) as Dictionary).duplicate()


func energy_marker_count() -> int:
	return _energy_drawn.size()


## Marcadores visíveis por fonte (contagem da legenda/filtro e testes).
func energy_marker_counts() -> Dictionary:
	var counts := {}
	for marker: Dictionary in _energy_drawn:
		var source := String(marker.get("source", ""))
		counts[source] = int(counts.get(source, 0)) + 1
	return counts


## Índice do marcador por chave "fonte|cidade/nação" no nível atual, ou -1.
func energy_marker_index(marker_id: String) -> int:
	return _energy_find_marker(marker_id)


## Recria as instâncias apenas com os recursos ligados. Barato: ~1k pontos.
func _rebuild_markers() -> void:
	if _mineral_mesh == null or _renewable_mesh == null:
		return
	var minerals: Array = []
	for point in _mineral_points:
		if _is_enabled(String(MINERAL_KEYS[int(point[2])])):
			minerals.append(point)
	_fill_minerals(minerals)

	var renewables: Array = []
	for point in _renewable_points:
		var crop := String(CROP_KEYS[clampi(int(point[2]), 0, CROP_KEYS.size() - 1)])
		if _is_enabled(crop):
			renewables.append(point)
	_fill_renewables(renewables)


func _fill_minerals(points: Array) -> void:
	var multimesh := _mineral_mesh.multimesh
	multimesh.instance_count = points.size()
	for index in points.size():
		var point: Array = points[index]
		var color_index := clampi(int(point[2]), 0, MINERAL_COLORS.size() - 1)
		_set_marker(_mineral_mesh, index, float(point[0]), float(point[1]),
				MARKER_SIZE, MINERAL_COLORS[color_index])


func _fill_renewables(points: Array) -> void:
	var multimesh := _renewable_mesh.multimesh
	multimesh.instance_count = points.size()
	for index in points.size():
		var point: Array = points[index]
		var color_index := clampi(int(point[2]), 0, CROP_COLORS.size() - 1)
		_set_marker(_renewable_mesh, index, float(point[0]), float(point[1]),
				MARKER_SIZE, CROP_COLORS[color_index])


## Tamanho do marcador: CONSTANTE para minerais e trigo. A reserva e a aptidão
## não influenciam mais o tamanho — elas seguem só na ficha da legenda.
func _marker_size(_point: Array, _is_wheat: bool) -> float:
	return MARKER_SIZE


func _marker_world(point: Array) -> Vector3:
	return Vector3((float(point[0]) - MAP_PX.x * 0.5) * 0.2, 0.2,
			(float(point[1]) - MAP_PX.y * 0.5) * 0.2)


## Depósito sob o cursor (índice em _mineral_points), ou -1. Respeita o filtro.
## Mesmo critério do `pick_city`: distância em TELA com um raio mínimo de 26 px.
## O raio deriva de MARKER_SIZE (constante), logo é igual para todos os
## depósitos — a área de pega não varia com a reserva.
func pick_deposit(screen_position: Vector2, camera: Camera3D) -> int:
	if camera == null:
		return -1
	var best := -1
	var best_distance := INF
	var right := camera.global_transform.basis.x
	var mineral_count := _mineral_points.size()
	# Minerais (índices 0..M-1).
	for index in mineral_count:
		var point: Array = _mineral_points[index]
		var key := String(MINERAL_KEYS[clampi(int(point[2]), 0,
				MINERAL_KEYS.size() - 1)])
		if not _is_enabled(key):
			continue
		var distance := _pick_screen_distance(point, false, screen_position, camera, right)
		if distance >= 0.0 and distance < best_distance:
			best = index
			best_distance = distance
	# Renováveis (índices M..M+R-1), um por território (dominante).
	for index in _renewable_points.size():
		var point: Array = _renewable_points[index]
		var crop := String(CROP_KEYS[clampi(int(point[2]), 0, CROP_KEYS.size() - 1)])
		if not _is_enabled(crop):
			continue
		var distance := _pick_screen_distance(point, true, screen_position, camera, right)
		if distance >= 0.0 and distance < best_distance:
			best = mineral_count + index
			best_distance = distance
	return best


## Distância em tela do ponto ao cursor se estiver dentro do raio de pega, ou -1.
## Raio de tela pelo MESMO critério de cidade/porto (`MapPick`): footprint do
## marcador com o piso comum, mais a folga própria do ponto.
func _pick_screen_distance(point: Array, is_wheat: bool, screen_position: Vector2,
		camera: Camera3D, _right: Vector3) -> float:
	var world := to_global(_marker_world(point))
	if camera.is_position_behind(world):
		return -1.0
	var center := camera.unproject_position(world)
	var radius: float = MapPickScript.pick_radius_px(camera, world,
			_marker_size(point, is_wheat) * 0.5, MapPickScript.DEPOSIT_RADIUS_FACTOR)
	var distance := center.distance_to(screen_position)
	return distance if distance <= radius else -1.0


## Tipo de um índice de pick codificado (minerais 0..M-1, renováveis M..M+R-1).
func _pick_point(index: int) -> Array:
	var m := _mineral_points.size()
	if index < m:
		return _mineral_points[index]
	return _renewable_points[index - m]


func _pick_is_wheat(index: int) -> bool:
	return index >= _mineral_points.size()


## Recurso dominante do renovável clicado (índice codificado em point[2]).
func _pick_crop(index: int) -> String:
	var point: Array = _renewable_points[index - _mineral_points.size()]
	return String(CROP_KEYS[clampi(int(point[2]), 0, CROP_KEYS.size() - 1)])


func _pick_size(index: int) -> float:
	return _marker_size(_pick_point(index), _pick_is_wheat(index))


func _total_pickable() -> int:
	return _mineral_points.size() + _renewable_points.size()


## Mostra o badge "i" sobre o depósito sob o mouse (índice) ou esconde (-1).
func set_deposit_hover(index: int) -> void:
	if _hover_ring == null:
		return
	if index < 0 or index >= _total_pickable() or _current != "resources":
		_hover_ring.visible = false
		return
	# Se o depósito já está selecionado, o anel de seleção basta (evita o hover
	# desenhando em cima da seleção).
	if index == _selected_deposit:
		_hover_ring.visible = false
		return
	var point: Array = _pick_point(index)
	var size := _pick_size(index)
	_hover_ring.position = _marker_world(point) + Vector3(0.0, 0.01, 0.0)
	# Hover: anel menor e mais discreto (só indica "sob o cursor"). O fator 1.8
	# mantém o anel sempre maior que o ponto; o piso de tela é aplicado por
	# frame em _process.
	_hover_diameter = size * HOVER_RING_FACTOR
	_hover_ring.pixel_size = _hover_diameter / RING_TEXTURE_PX
	_hover_ring.modulate = Color(1.0, 1.0, 1.0, 0.8)
	_hover_ring.visible = true
	_hover_time = 0.0


## Marca o depósito selecionado (anel persistente) ou limpa (-1).
func set_deposit_selected(index: int) -> void:
	_selected_deposit = index
	_update_selection_ring()


## Depósito selecionado (-1 = nenhum), para o fluxo de desseleção.
func selected_deposit() -> int:
	return _selected_deposit


func _update_selection_ring() -> void:
	if _selection_ring == null:
		return
	if _current == "energy":
		if _energy_selected < 0 or _energy_selected >= _energy_drawn.size():
			_selection_ring.visible = false
			return
		var emarker: Dictionary = _energy_drawn[_energy_selected]
		_selection_ring.position = _marker_world(
				[float(emarker["px"]), float(emarker["py"])]) \
				+ Vector3(0.0, 0.02, 0.0)
		_selection_diameter = _energy_marker_world_size(emarker,
				get_viewport().get_camera_3d()) * SELECTION_RING_FACTOR
		_selection_ring.pixel_size = _selection_diameter / RING_TEXTURE_PX
		_selection_ring.modulate = Color(1.0, 0.86, 0.42, 1.0)
		_selection_ring.visible = true
		return
	if _selected_deposit < 0 or _selected_deposit >= _total_pickable() \
			or _current != "resources":
		_selection_ring.visible = false
		return
	var point: Array = _pick_point(_selected_deposit)
	var size := _pick_size(_selected_deposit)
	_selection_ring.position = _marker_world(point) + Vector3(0.0, 0.02, 0.0)
	# Seleção: anel bem maior, dourado e pulsante — distinto do hover.
	_selection_diameter = size * SELECTION_RING_FACTOR
	_selection_ring.pixel_size = _selection_diameter / RING_TEXTURE_PX
	_selection_ring.modulate = Color(1.0, 0.86, 0.42, 1.0)
	_selection_ring.visible = true


func _process(_delta: float) -> void:
	# Pulso contínuo (sem depender de estado): seleção mais forte que o hover.
	var t := Time.get_ticks_msec() / 1000.0
	var camera := get_viewport().get_camera_3d()
	if _current == "energy" and _energy_built:
		var band := energy_zoom_band()
		if band != _energy_band:
			_rebuild_energy(true, band)
		else:
			# Mesma faixa, mas o zoom pode ter mudado: o piso de px é recalculado
			# (só quando a distância muda 4%+; pan não reescreve nada).
			_refresh_energy_floor(camera)
	if _hover_ring != null and _hover_ring.visible:
		var pulse := 1.0 + 0.05 * sin(t * 5.0)
		_hover_ring.scale = Vector3(pulse, pulse, 1.0)
		_apply_ring_screen_floor(_hover_ring, _hover_diameter,
				HOVER_MIN_SCREEN_PX, camera)
	if _selection_ring != null and _selection_ring.visible:
		var spulse := 1.0 + 0.08 * sin(t * 3.0)
		_selection_ring.scale = Vector3(spulse, spulse, 1.0)
		_apply_ring_screen_floor(_selection_ring, _selection_diameter,
				SELECTION_MIN_SCREEN_PX, camera)


## Garante um piso em TELA para o anel: em zoom afastado o diâmetro lógico
## (constante MARKER_SIZE) projeta poucos pixels e o anel some. Aqui o
## pixel_size cresce até o anel medir ao menos `min_px` na tela, usando a
## câmera ativa (perspectiva). O piso só aumenta o anel — nunca o encolhe
## abaixo do tamanho lógico.
func _apply_ring_screen_floor(ring: Sprite3D, diameter: float, min_px: float,
		camera: Camera3D) -> void:
	var viewport_height := float(get_viewport().get_visible_rect().size.y)
	if camera == null or viewport_height <= 0.0:
		ring.pixel_size = diameter / RING_TEXTURE_PX
		return
	var distance := camera.global_position.distance_to(ring.global_position)
	var focal := viewport_height / (2.0 * tan(deg_to_rad(camera.fov) * 0.5))
	var min_world := min_px * distance / focal
	ring.pixel_size = maxf(diameter, min_world) / RING_TEXTURE_PX


## Dados de um depósito para a legenda.
## Pontos minerais carregados (x, y, recurso, reserva, qualidade, profundidade,
## status, nome, província, offshore). Usado pela cidade (acesso por território).
func mineral_points() -> Array:
	return _mineral_points


## Nível de esgotamento inicial, semeado pelo status MRDS. Sem prospecção:
## todo depósito é mina conhecida; "escassa" é alcançada dinamicamente na
## extração (entre moderada e residual). `code` é o STATUS_CODES do ponto.
func _situation_from_code(code: int) -> String:
	match code:
		0:  # Producer
			return "abundant"
		1:  # Past Producer
			return "residual"
		2, 3:  # Prospect / Occurrence
			return "intact"
		_:
			return "moderate"


func deposit_info(index: int) -> Dictionary:
	if index < 0 or index >= _total_pickable():
		return {}
	if _pick_is_wheat(index):
		# Renovável (cultura ou madeira): aptidão + fertilidade/cultivável do
		# território. Sem reserva — fluxo anual.
		var point: Array = _pick_point(index)
		var pos := Vector2(float(point[0]), float(point[1]))
		var territory := String(point[4]) if point.size() > 4 else ""
		var attrs: Dictionary = _geo.territory_attributes(pos) if _geo != null else {}
		var region: Dictionary = point[5] if point.size() > 5 else {}
		var territories: Array = region.get("territories", [])
		var region_count := territories.size() if not territories.is_empty() else 1
		return {
			"kind": "wheat",
			"resource": String(CROP_KEYS[clampi(int(point[2]), 0, CROP_KEYS.size() - 1)]),
			"suitability": float(point[3]) if point.size() > 3 else 0.0,
			"fertility": float(attrs.get("fertility", 0.0)),
			"cultivable": float(attrs.get("cultivable", 0.0)),
			"territory": territory,
			"name_pt": String(attrs.get("name_pt", "")),
			"name_en": String(attrs.get("name_en", "")),
			"name_es": String(attrs.get("name_es", "")),
			"climate": String(attrs.get("climate", "")),
			"soil_type": String(attrs.get("soil_type", "")),
			"relief": String(attrs.get("relief", "")),
			"aggregate": region_count > 1,
			"area_km2": float(region.get("area_km2", 0.0)),
			"region_count": region_count,
			"country": String(region.get("country", "")),
		}
	var point: Array = _mineral_points[index]
	var territory := String(point[7]) if point.size() > 7 else ""
	var resource := String(MINERAL_KEYS[clampi(int(point[2]), 0, MINERAL_KEYS.size() - 1)])
	var names: Dictionary = _geo.territory_name(territory) if _geo != null else {}
	var region: Dictionary = point[10] if point.size() > 10 else {}
	# Cidade(s) dona(s): o marcador pode agregar várias jazidas (regra A1 é por
	# jazida). Resolvemos os donos pelos territórios reais do marcador — nunca
	# "tudo à capital".
	var owners: Array = []
	if _geo != null:
		var territories: Array = []
		if region.has("territories"):
			territories = region.get("territories", [])
		elif territory != "":
			territories = [territory]
		owners = _geo.owners_for_territories(territories, resource)
	return {
		"kind": "mineral",
		"resource": resource,
		"count": int(point[8]) if point.size() > 8 else 1,
		"reserve_kt": float(point[3]) if point.size() > 3 else 0.0,
		"quality": float(point[4]) if point.size() > 4 else 0.0,
		"depth_m": int(point[5]) if point.size() > 5 else 0,
		"status": int(point[6]) if point.size() > 6 else 4,
		"situation": _situation_from_code(int(point[6]) if point.size() > 6 else 4),
		"territory": territory,
		"name_pt": String(names.get("name_pt", "")),
		"name_en": String(names.get("name_en", "")),
		"name_es": String(names.get("name_es", "")),
		"aggregate": bool(region.get("aggregate", false)),
		"area_km2": float(region.get("area_km2", 0.0)),
		"region_count": int(region.get("region_count", 1)),
		"country": String(region.get("country", "")),
		"owners": owners,
	}


func _make_marker_layer(node_name: String, texture: Texture2D,
		priority: int = 2) -> MultiMeshInstance3D:
	var material := StandardMaterial3D.new()
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	material.albedo_texture = texture
	material.billboard_mode = BaseMaterial3D.BILLBOARD_ENABLED
	# Sem keep_scale o billboard descarta a escala da instância do MultiMesh e
	# desenha todo marcador com o quad de 1.0 u. Com keep_scale o ponto respeita
	# MARKER_SIZE (constante) — mesmo tamanho para todos os depósitos.
	material.billboard_keep_scale = true
	material.vertex_color_use_as_albedo = true
	material.texture_filter = BaseMaterial3D.TEXTURE_FILTER_LINEAR_WITH_MIPMAPS
	material.cull_mode = BaseMaterial3D.CULL_DISABLED
	material.no_depth_test = true
	material.render_priority = priority

	var multimesh := MultiMesh.new()
	multimesh.transform_format = MultiMesh.TRANSFORM_3D
	multimesh.use_colors = true
	multimesh.instance_count = 0
	var quad := QuadMesh.new()
	quad.size = Vector2.ONE
	multimesh.mesh = quad

	var instance := MultiMeshInstance3D.new()
	instance.name = node_name
	instance.multimesh = multimesh
	instance.material_override = material
	instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	add_child(instance)
	return instance


func _set_marker(layer: MultiMeshInstance3D, index: int, px: float, py: float,
		size: float, color: Color) -> void:
	var position := Vector3((px - MAP_PX.x * 0.5) * 0.2, 0.2,
			(py - MAP_PX.y * 0.5) * 0.2)
	var basis := Basis().scaled(Vector3(size, size, size))
	layer.multimesh.set_instance_transform(index, Transform3D(basis, position))
	# A cor da instância entra no shader como linear; sem converter, o
	# tonemapper clareia os marcadores (cobre virava salmão).
	layer.multimesh.set_instance_color(index, color.srgb_to_linear())


## Escala logarítmica: cidades muito populosas não apagam as menores.
static func _log_ratio(value: float, maximum: float) -> float:
	if maximum <= 0.0:
		return 0.0
	return clampf(log(1.0 + value) / log(1.0 + maximum), 0.0, 1.0)


static func _pad(values: PackedFloat32Array) -> PackedFloat32Array:
	values.resize(MAX_CITIES)
	return values
