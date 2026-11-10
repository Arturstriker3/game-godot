extends Node3D

## Camada visual dos modos de mapa (população, economia, indústria, recursos).
##
## Um único plano sobre a base geográfica desenha halos de calor a partir das
## cidades. Os valores vêm exclusivamente do status do EconomySim e do
## GeographyService — não existe economia paralela. Trocar de modo só altera
## uniformes; nenhuma geometria é reconstruída.

const MODE_SHADER: Shader = preload("res://map/map_shaders/map_modes.gdshader")
const RESOURCE_TEXTURE: Texture2D = preload("res://map/map_data/atlas/resources.png")
const GeographyScript: Script = preload("res://core/economy/geography_service.gd")

const MAP_PX := Vector2(5000.0, 2500.0)
const MAX_CITIES := 16

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
var _resource_mesh: MeshInstance3D


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

	# Camada do mapa de recursos: textura estática gerada por
	# tools/build_resource_map.py (depósitos minerais + trigo).
	var resource_material := StandardMaterial3D.new()
	resource_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	resource_material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	resource_material.albedo_texture = RESOURCE_TEXTURE
	resource_material.texture_filter = \
			BaseMaterial3D.TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC
	resource_material.cull_mode = BaseMaterial3D.CULL_DISABLED

	var resource_mesh := MeshInstance3D.new()
	resource_mesh.name = "ResourceLayer"
	resource_mesh.mesh = plane
	resource_mesh.material_override = resource_material
	resource_mesh.position = Vector3(0.0, 0.145, 0.0)
	resource_mesh.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	resource_mesh.visible = false
	add_child(resource_mesh)
	_resource_mesh = resource_mesh

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


## Liga/desliga as camadas sem recriar os planos. O mapa de recursos usa uma
## textura própria; os demais mostram o campo de calor.
func set_mode(mode: String) -> void:
	_current = mode
	var resources := mode == "resources"
	var heat := MODE_COLORS.has(mode) and not resources
	if _resource_mesh != null:
		_resource_mesh.visible = resources
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


## Escala logarítmica: cidades muito populosas não apagam as menores.
static func _log_ratio(value: float, maximum: float) -> float:
	if maximum <= 0.0:
		return 0.0
	return clampf(log(1.0 + value) / log(1.0 + maximum), 0.0, 1.0)


static func _pad(values: PackedFloat32Array) -> PackedFloat32Array:
	values.resize(MAX_CITIES)
	return values
