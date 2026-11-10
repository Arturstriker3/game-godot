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
const RESOURCE_POINTS_PATH := "res://map/map_data/atlas/resource_points.json"
## Ordem dos recursos em resource_points.json (índice do mineral).
const MINERAL_KEYS := ["coal", "copper", "gas", "iron", "oil"]
const MINERAL_COLORS := [
	Color("#3E3B38"), Color("#C06A3C"), Color("#4E9FB0"),
	Color("#7C8894"), Color("#5B4A2E"),
]
const WHEAT_COLOR := Color("#D9B45A")

const MODE_COLORS := {
	"population": [Color("#3A5563"), Color("#C9A24A")],
	"economic": [Color("#3A5563"), Color("#7FA86B")],
	"industrial": [Color("#3A5563"), Color("#D08A3E")],
	"resources": [Color("#3A5563"), Color("#8FA36B")],
}

var _material: ShaderMaterial
var _uv := PackedVector2Array()
var _values: Dictionary = {}
var _current := "geographic"
var _heat_mesh: MeshInstance3D
var _mineral_mesh: MultiMeshInstance3D
var _wheat_mesh: MultiMeshInstance3D
var _mineral_points: Array = []
var _wheat_points: Array = []
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
	if _wheat_mesh != null:
		_wheat_mesh.visible = resources
	if _heat_mesh != null:
		_heat_mesh.visible = heat
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


## Constrói os marcadores do mapa de recursos: minerais (círculo) e trigo
## (losango), um MultiMesh por tipo. Billboard mantém o desenho nítido.
func _build_resource_markers() -> void:
	var data: Variant = JSON.parse_string(
			FileAccess.get_file_as_string(RESOURCE_POINTS_PATH))
	if typeof(data) != TYPE_DICTIONARY:
		return
	_mineral_points = data.get("minerals", [])
	_wheat_points = data.get("wheat", [])

	_mineral_mesh = _make_marker_layer("ResourceMinerals", RESOURCE_DOT)
	_wheat_mesh = _make_marker_layer("ResourceWheat", RESOURCE_WHEAT)
	_rebuild_markers()

	_mineral_mesh.visible = false
	_wheat_mesh.visible = false


## Aplica o filtro da legenda (chaves: coal/copper/gas/iron/oil/wheat).
## Dicionário vazio = tudo ligado.
func set_resource_filter(enabled: Dictionary) -> void:
	_resource_filter = enabled.duplicate()
	_rebuild_markers()


func _is_enabled(key: String) -> bool:
	return _resource_filter.is_empty() or bool(_resource_filter.get(key, true))


## Recria as instâncias apenas com os recursos ligados. 2.181 pontos: barato.
func _rebuild_markers() -> void:
	if _mineral_mesh == null or _wheat_mesh == null:
		return
	var minerals: Array = []
	for point in _mineral_points:
		if _is_enabled(String(MINERAL_KEYS[int(point[2])])):
			minerals.append(point)
	_fill_markers(_mineral_mesh, minerals, false)

	var wheat: Array = []
	if _is_enabled("wheat"):
		wheat = _wheat_points
	_fill_markers(_wheat_mesh, wheat, true)


func _fill_markers(layer: MultiMeshInstance3D, points: Array, is_wheat: bool) -> void:
	var multimesh := layer.multimesh
	multimesh.instance_count = points.size()
	for index in points.size():
		var point: Array = points[index]
		var color := WHEAT_COLOR
		var size := 0.55
		if is_wheat:
			size = 0.42 + 0.38 * clampf(float(point[2]), 0.0, 1.0)
		else:
			var color_index := clampi(int(point[2]), 0, MINERAL_COLORS.size() - 1)
			color = MINERAL_COLORS[color_index]
		_set_marker(layer, index, float(point[0]), float(point[1]), size, color)


func _make_marker_layer(node_name: String, texture: Texture2D) -> MultiMeshInstance3D:
	var material := StandardMaterial3D.new()
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	material.albedo_texture = texture
	material.billboard_mode = BaseMaterial3D.BILLBOARD_ENABLED
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
	layer.multimesh.set_instance_color(index, color)


## Escala logarítmica: cidades muito populosas não apagam as menores.
static func _log_ratio(value: float, maximum: float) -> float:
	if maximum <= 0.0:
		return 0.0
	return clampf(log(1.0 + value) / log(1.0 + maximum), 0.0, 1.0)


static func _pad(values: PackedFloat32Array) -> PackedFloat32Array:
	values.resize(MAX_CITIES)
	return values
