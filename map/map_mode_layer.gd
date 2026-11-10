extends Node3D

## Camada visual dos modos de mapa (população, economia, indústria, recursos).
##
## Um único plano sobre a base geográfica desenha halos de calor a partir das
## cidades. Os valores vêm exclusivamente do status do EconomySim e do
## GeographyService — não existe economia paralela. Trocar de modo só altera
## uniformes; nenhuma geometria é reconstruída.

const MODE_SHADER: Shader = preload("res://map/map_shaders/map_modes.gdshader")
const GeographyScript: Script = preload("res://core/economy/geography_service.gd")

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
## Filtros da legenda: {} = todos; senão chave -> bool.
var _resource_filter: Dictionary = {}


## Constrói a camada e normaliza os indicadores de cada modo.
func setup(cities: Array) -> void:
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

	# Camada do mapa de recursos: marcadores em billboard (nítidos em qualquer
	# zoom), gerados de map/map_data/atlas/resource_points.json.
	_build_resource_markers()

	_build_values(cities)
	visible = false


func current_mode() -> String:
	return _current


## Recalcula os valores a partir do status atual sem recriar a camada.
func refresh(cities: Array) -> void:
	if cities.is_empty():
		return
	_uv.clear()
	_values.clear()
	_build_values(cities)
	if MODE_COLORS.has(_current):
		set_mode(_current)


## Liga/desliga as camadas sem recriar os planos. O mapa de recursos usa
## marcadores; os demais mostram o campo de calor.
func set_mode(mode: String) -> void:
	_current = mode
	var resources := mode == "resources"
	var heat := MODE_COLORS.has(mode) and not resources
	if _mineral_mesh != null:
		_mineral_mesh.visible = resources
	if _renewable_mesh != null:
		_renewable_mesh.visible = resources
	if _heat_mesh != null:
		_heat_mesh.visible = heat
	if _hover_ring != null and not resources:
		_hover_ring.visible = false
	_update_selection_ring()
	visible = resources or heat

	if not heat:
		return

	var colors: Array = MODE_COLORS[mode]
	_material.set_shader_parameter("low_color", colors[0])
	_material.set_shader_parameter("high_color", colors[1])
	_material.set_shader_parameter("city_uv", _uv)
	_material.set_shader_parameter("city_value", _values[mode])
	visible = true


## Empacota UV e valores normalizados por modo. Cidades ausentes viram zeros.
func _build_values(cities: Array) -> void:
	var max_population := 0.0
	var max_production := 0.0
	for city in cities:
		max_population = maxf(max_population, float(city.get("population", 0.0)))
		max_production = maxf(max_production, float(city.get("production", 0.0)))

	var geography: RefCounted = GeographyScript.new()
	geography.load_data()
	_geo = geography

	var population := PackedFloat32Array()
	var economic := PackedFloat32Array()
	var industrial := PackedFloat32Array()
	var resources := PackedFloat32Array()

	for city in cities:
		var center := Vector2(float(city.get("x", 0.0)), float(city.get("y", 0.0)))
		_uv.append(Vector2(center.x / MAP_PX.x, center.y / MAP_PX.y))

		var population_value := float(city.get("population", 0.0))
		population.append(_log_ratio(population_value, max_population))

		var production := float(city.get("production", 0.0))
		economic.append(clampf(production / maxf(max_production, 1.0), 0.0, 1.0))

		industrial.append(clampf(float(city.get("industrialization", 0.0)), 0.0, 1.0))

		var wheat: float = geography.crop_suitability(String(city.get("id", "")), center, "wheat")
		var deposits: Array = geography.deposits_for(String(city.get("id", "")), center)
		resources.append(clampf(wheat * 0.75 + minf(float(deposits.size()), 4.0) * 0.08, 0.0, 1.0))

	population = _pad(population)
	economic = _pad(economic)
	industrial = _pad(industrial)
	resources = _pad(resources)
	_uv.resize(MAX_CITIES)

	_values["population"] = population
	_values["economic"] = economic
	_values["industrial"] = industrial
	_values["resources"] = resources

	_material.set_shader_parameter("city_count", cities.size())
	_material.set_shader_parameter("heat_radius", 0.055)
	_material.set_shader_parameter("opacity", 0.45)


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
func _pick_screen_distance(point: Array, is_wheat: bool, screen_position: Vector2,
		camera: Camera3D, right: Vector3) -> float:
	var world := to_global(_marker_world(point))
	if camera.is_position_behind(world):
		return -1.0
	var center := camera.unproject_position(world)
	var edge := camera.unproject_position(
			world + right * (_marker_size(point, is_wheat) * 0.5))
	var radius := maxf(26.0, center.distance_to(edge) * 1.35)
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


func _update_selection_ring() -> void:
	if _selection_ring == null:
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
		}
	var point: Array = _mineral_points[index]
	var territory := String(point[7]) if point.size() > 7 else ""
	var resource := String(MINERAL_KEYS[clampi(int(point[2]), 0, MINERAL_KEYS.size() - 1)])
	var names: Dictionary = _geo.territory_name(territory) if _geo != null else {}
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
	}


func _make_marker_layer(node_name: String, texture: Texture2D) -> MultiMeshInstance3D:
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
	material.render_priority = 2

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
