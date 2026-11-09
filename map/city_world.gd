extends Node3D
class_name CityWorld

## Mundo baseado em cidades: fundo de satélite real (satellite.jpg), uma cidade
## 3D por nação e a camada geopolítica com as fronteiras reais (Natural Earth)
## sobre o terreno. Os dados (posição, nação, população) vêm do núcleo C++.
##
## Três níveis de representação, escolhidos pela distância da câmera:
##   local         — cidade orgânica com módulos 3D detalhados;
##   intermediário — a mesma cidade em volumes simplificados (silhueta igual);
##   continental   — marcador discreto e rótulo.
## A cidade é construída sob demanda e liberada ao afastar (histerese).

const MAP_SIZE_PX := Vector2(5000.0, 2500.0)
const PLANE_SIZE := Vector2(1000.0, 500.0)
const PIXEL_TO_WORLD := 0.2

const GPS_TEXTURE: Texture2D = preload("res://map/map_data/satellite.jpg")
const RELIEF_TEXTURE: Texture2D = preload("res://map/map_data/normal.png")
const MAP_SHADER: Shader = preload("res://map/map_shaders/satellite_map.gdshader")
const BORDERS_TEXTURE: Texture2D = preload("res://map/map_data/country_borders.png")
const FILL_TEXTURE: Texture2D = preload("res://map/map_data/nation_fill.png")
const POLITICAL_SHADER: Shader = preload("res://map/map_shaders/political_overlay.gdshader")
const LABEL_FONT: Font = preload("res://assets/fonts/BarlowCondensed-SemiBold.ttf")

const ModeLayerScript: Script = preload("res://map/map_mode_layer.gd")
const UrbanGrowthScript: Script = preload("res://core/urban/urban_growth_manager.gd")
const IndustryProfileScript: Script = preload("res://core/urban/industry_profile.gd")

const MARKER_COLOR := Color("#0A1212")
const MARKER_DOT := Color("#E8E2D0")

## Orçamento de renderização das cidades.
const MAX_DETAILED_CITIES := 40
const MAX_CITY_LABELS := 24

## Regras de LOD (unidades do mundo, 1 unidade ≈ 40 km no equador).
## Uma única representação da cidade, sem modelos concorrentes:
##   próximo        (< 34)       -> módulos 3D detalhados (UrbanRenderer)
##   intermediário  (34 .. 100)  -> a mesma cidade em caixas simplificadas
##   distante       (> 100)      -> marcador discreto + rótulo
## A cidade é construída a 96 u e liberada a 112 u (histerese); o marcador
## entra com fade só depois que a cidade já sumiu.
const URBAN_BUILD_DISTANCE := 96.0
const CITY_VISIBLE_DISTANCE := 100.0
const MARKER_FADE_IN_START := 100.0
const MARKER_FADE_IN_END := 112.0
## Escala da cidade no mapa. As cidades ficam em posições geográficas reais:
## Bruxelas–Amsterdã dista ~4,5 u, então o diâmetro precisa ficar bem abaixo
## disso para não encostar nas vizinhas.
const URBAN_SCALE := 0.45
const LABEL_PIXEL_PER_UNIT := 0.0008
const LABEL_PIXEL_MIN := 0.006
const LABEL_PIXEL_MAX := 0.12

const NATION_COLORS := {
	"GBR": Color("#8C3A3C"),
	"FRA": Color("#3C5A86"),
	"DEU": Color("#5E6668"),
	"NED": Color("#A1653A"),
	"BEL": Color("#A39244"),
}

var _content: Node3D
var _political_layer: MeshInstance3D
var _political_material: ShaderMaterial
var _mode_layer: Node3D
var _city_nodes: Array[Dictionary] = []
var _mode := "political"
var _selection: MeshInstance3D
var _selection_dot: MeshInstance3D
var _hover: MeshInstance3D
var _selected_id := ""


## Monta o mundo: chão de satélite, camada geopolítica e modelos de cidade.
func build(cities: Array) -> void:
	_content = Node3D.new()
	add_child(_content)

	_build_gps_plane()
	_build_political_layer()
	_build_mode_layer(cities)
	_build_cities(cities)
	set_mode(_mode)

	# Cópias laterais: o mundo dá a volta em X (cilíndrico), então quem sai por
	# um lado reaparece no outro.
	for offset in [-PLANE_SIZE.x, PLANE_SIZE.x]:
		var copy := _content.duplicate() as Node3D
		copy.position = Vector3(offset, 0.0, 0.0)
		add_child(copy)


## Troca o modo de mapa sem reconstruir a geometria: só visibilidade e uniformes.
func set_mode(mode: String) -> void:
	_mode = mode
	if _political_layer != null:
		_political_layer.visible = mode != "geographic"
	if _political_material != null:
		_political_material.set_shader_parameter(
				"fill_opacity", 0.11 if mode == "political" else 0.028)
		_political_material.set_shader_parameter(
				"border_opacity", 0.55 if mode == "political" else 0.42)
	if _mode_layer != null:
		_mode_layer.set_mode(mode)


func current_mode() -> String:
	return _mode


## Reaplica os valores dos modos quando a economia avança; sem reconstruir nada.
func refresh_modes(cities: Array) -> void:
	if _mode_layer != null:
		_mode_layer.refresh(cities)


func pixel_to_world(px: Vector2) -> Vector3:
	return Vector3(
		(px.x - MAP_SIZE_PX.x * 0.5) * PIXEL_TO_WORLD,
		0.0,
		(px.y - MAP_SIZE_PX.y * 0.5) * PIXEL_TO_WORLD,
	)


## Cidade mais próxima do clique em tela, dentro do raio informado.
## Sem física: projeta cada cidade para a viewport, o que funciona em qualquer
## nível de zoom e não interfere no algoritmo territorial.
func pick_city(screen_position: Vector2, camera: Camera3D, radius_px: float = 26.0) -> String:
	if camera == null:
		return ""
	var best_id := ""
	var best_distance := radius_px
	for record in _city_nodes:
		var world: Vector3 = to_global(record["position"])
		if camera.is_position_behind(world):
			continue
		var distance := camera.unproject_position(world).distance_to(screen_position)
		if distance <= best_distance:
			best_distance = distance
			best_id = String(record["id"])
	return best_id


## Posição global da cidade (para centralizar a câmera).
func city_world_position(city_id: String) -> Vector3:
	for record in _city_nodes:
		if String(record["id"]) == city_id:
			return to_global(record["position"])
	return Vector3.ZERO


func selected_city() -> String:
	return _selected_id


## Seleção discreta: um anel fino rente ao chão + um ponto central pequeno.
## Não esconde edifícios nem atravessa o terreno (fica a ~0,15 u de altura).
func select_city(city_id: String) -> void:
	_selected_id = city_id
	if _selection == null:
		_create_indicators()
	if _selection == null:
		return
	var active := not city_id.is_empty()
	_selection.visible = active
	_selection_dot.visible = active
	if active:
		var footprint := _city_footprint(city_id)
		var base: Vector3 = city_world_position(city_id) + Vector3(0.0, 0.15, 0.0)
		_selection.scale = Vector3(footprint, 1.0, footprint)
		_selection.global_position = base
		_selection_dot.global_position = base + Vector3(0.0, 0.03, 0.0)


## Realce de hover: versão ainda mais discreta, no mesmo ponto da seleção.
func set_hover(city_id: String) -> void:
	if _selection == null:
		_create_indicators()
	if _hover == null:
		return
	if city_id == _selected_id:
		_hover.visible = false
		return
	_hover.visible = not city_id.is_empty()
	if _hover.visible:
		var footprint := _city_footprint(city_id)
		_hover.scale = Vector3(footprint, 1.0, footprint)
		_hover.global_position = city_world_position(city_id) + Vector3(0.0, 0.15, 0.0)


## Raio da cidade em unidades de mundo, para o anel de seleção abraçar o tecido.
func _city_footprint(city_id: String) -> float:
	for record in _city_nodes:
		if String(record["id"]) == city_id:
			var urban = record.get("urban")
			if urban != null and urban.has_method("extent"):
				return maxf(1.3, float(urban.extent()) * URBAN_SCALE + 0.35)
			break
	return 1.8


func _create_indicators() -> void:
	_selection = _make_ring("CitySelection", 1.0, 1.025, Color("#C9A24A"), 0.30)
	_selection.visible = false
	add_child(_selection)

	_selection_dot = _make_dot("CitySelectionDot", Color("#C9A24A"), 0.65)
	_selection_dot.visible = false
	add_child(_selection_dot)

	_hover = _make_ring("CityHover", 1.0, 1.025, Color("#E8E2D0"), 0.16)
	_hover.visible = false
	add_child(_hover)


func _make_ring(node_name: String, inner: float, outer: float,
		color: Color, alpha: float) -> MeshInstance3D:
	var torus := TorusMesh.new()
	torus.inner_radius = inner
	torus.outer_radius = outer
	torus.rings = 48
	torus.ring_segments = 6

	var ring_color := color
	ring_color.a = alpha

	var material := StandardMaterial3D.new()
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	material.albedo_color = ring_color

	var instance := MeshInstance3D.new()
	instance.name = node_name
	instance.mesh = torus
	instance.material_override = material
	# TorusMesh já nasce deitado no plano XZ; nada de girar (senão vira arco).
	instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	return instance


func _make_dot(node_name: String, color: Color, alpha: float) -> MeshInstance3D:
	var cylinder := CylinderMesh.new()
	cylinder.top_radius = 0.12
	cylinder.bottom_radius = 0.12
	cylinder.height = 0.02
	cylinder.radial_segments = 24

	var dot_color := color
	dot_color.a = alpha

	var material := StandardMaterial3D.new()
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	material.albedo_color = dot_color

	var instance := MeshInstance3D.new()
	instance.name = node_name
	instance.mesh = cylinder
	instance.material_override = material
	instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	return instance


func _build_gps_plane() -> void:
	var plane := PlaneMesh.new()
	plane.size = PLANE_SIZE

	var material := ShaderMaterial.new()
	material.shader = MAP_SHADER
	material.set_shader_parameter("map_texture", GPS_TEXTURE)
	material.set_shader_parameter("relief_texture", RELIEF_TEXTURE)

	var mesh_instance := MeshInstance3D.new()
	mesh_instance.mesh = plane
	mesh_instance.material_override = material
	_content.add_child(mesh_instance)


func _build_political_layer() -> void:
	var plane := PlaneMesh.new()
	plane.size = PLANE_SIZE

	_political_material = ShaderMaterial.new()
	_political_material.shader = POLITICAL_SHADER
	_political_material.set_shader_parameter("fill_texture", FILL_TEXTURE)
	_political_material.set_shader_parameter("border_texture", BORDERS_TEXTURE)

	_political_layer = MeshInstance3D.new()
	_political_layer.name = "PoliticalLayer"
	_political_layer.mesh = plane
	_political_layer.position = Vector3(0.0, 0.13, 0.0)
	_political_layer.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	_political_layer.material_override = _political_material
	_content.add_child(_political_layer)


func _build_mode_layer(cities: Array) -> void:
	_mode_layer = ModeLayerScript.new()
	_mode_layer.name = "ModeLayer"
	add_child(_mode_layer)
	_mode_layer.setup(cities)


func _build_cities(cities: Array) -> void:
	# Prioriza as maiores cidades para o detalhe 3D; o resto usa MultiMesh.
	var ordered := cities.duplicate()
	ordered.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return float(a.get("population", 0.0)) > float(b.get("population", 0.0))
	)
	var detailed := mini(MAX_DETAILED_CITIES, ordered.size())
	for index in detailed:
		var city: Dictionary = ordered[index]
		var position: Vector3 = pixel_to_world(Vector2(city["x"], city["y"]))
		var color: Color = NATION_COLORS.get(city["nation"], Color.WHITE)
		var discs := _spawn_city_marker(position, city["name"], color)
		var label := _add_city_label(position, city["name"], color,
				index < MAX_CITY_LABELS)
		_city_nodes.append({
			"id": String(city.get("id", "")),
			"city": city,
			"position": position,
			"discs": discs,
			"label": label,
			"urban": null,
		})

	if ordered.size() > detailed:
		_build_city_multimesh(ordered.slice(detailed))


func _build_city_multimesh(cities: Array) -> void:
	if cities.is_empty():
		return
	var mesh := CylinderMesh.new()
	mesh.top_radius = 0.55
	mesh.bottom_radius = 0.55
	mesh.height = 0.08
	mesh.radial_segments = 12

	var material := StandardMaterial3D.new()
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.vertex_color_use_as_albedo = true

	var multimesh := MultiMesh.new()
	multimesh.transform_format = MultiMesh.TRANSFORM_3D
	multimesh.use_colors = true
	multimesh.mesh = mesh
	multimesh.instance_count = cities.size()

	var basis := Basis()
	for index in cities.size():
		var city: Dictionary = cities[index]
		var position: Vector3 = pixel_to_world(Vector2(city["x"], city["y"]))
		multimesh.set_instance_transform(index,
			Transform3D(basis, position + Vector3(0.0, 0.16, 0.0)))
		multimesh.set_instance_color(index,
			NATION_COLORS.get(city["nation"], Color.WHITE))

	var instance := MultiMeshInstance3D.new()
	instance.multimesh = multimesh
	instance.material_override = material
	_content.add_child(instance)


## Marcador estratégico em camadas; devolve os discos para o controle de LOD.
func _spawn_city_marker(position: Vector3, _city_name: String, color: Color) -> Array:
	var discs: Array = []
	discs.append(_add_disc(position + Vector3(0.0, 0.14, 0.0), 0.85, 0.10, MARKER_COLOR))
	discs.append(_add_disc(position + Vector3(0.0, 0.20, 0.0), 0.57, 0.12, color))
	discs.append(_add_disc(position + Vector3(0.0, 0.27, 0.0), 0.19, 0.10, MARKER_DOT))
	return discs


## Disco plano usado para desenhar o marcador da cidade em camadas.
func _add_disc(position: Vector3, radius: float, height: float, color: Color) -> MeshInstance3D:
	var mesh := CylinderMesh.new()
	mesh.top_radius = radius
	mesh.bottom_radius = radius
	mesh.height = height
	mesh.radial_segments = 24

	var material := StandardMaterial3D.new()
	material.albedo_color = color
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED

	var instance := MeshInstance3D.new()
	instance.mesh = mesh
	instance.material_override = material
	instance.position = position
	_content.add_child(instance)
	return instance


## Nome da cidade em caixa alta, como legenda de carta militar.
func _add_city_label(position: Vector3, city_name: String, color: Color,
		with_label: bool) -> Label3D:
	if not with_label:
		return null
	var label := Label3D.new()
	label.text = city_name.to_upper()
	label.font = LABEL_FONT
	label.font_size = 30
	label.pixel_size = 0.019
	label.modulate = color.lightened(0.35)
	label.outline_modulate = Color(0.03, 0.05, 0.05, 0.9)
	label.outline_size = 10
	label.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	label.position = position + Vector3(0.0, 1.0, 0.0)
	_content.add_child(label)
	return label


## LOD por distância: rótulo, marcador e cidade modular.
func _process(_delta: float) -> void:
	if _city_nodes.is_empty():
		return
	var camera := get_viewport().get_camera_3d()
	if camera == null:
		return

	var camera_position := camera.global_position
	for record in _city_nodes:
		var world_position: Vector3 = to_global(record["position"])
		var distance := camera_position.distance_to(world_position)
		_apply_city_lod(record, distance, world_position)


func _apply_city_lod(record: Dictionary, distance: float, world_position: Vector3) -> void:
	var label: Label3D = record["label"]
	if label != null:
		label.pixel_size = clampf(LABEL_PIXEL_PER_UNIT * distance,
				LABEL_PIXEL_MIN, LABEL_PIXEL_MAX)

	# Marcador discreto (continental): aparece só quando a cidade já sumiu.
	var marker_alpha := _fade_in(distance, MARKER_FADE_IN_START, MARKER_FADE_IN_END)
	for disc in record["discs"]:
		var mesh := disc as MeshInstance3D
		mesh.visible = marker_alpha > 0.01
		mesh.transparency = 1.0 - marker_alpha

	_update_urban(record, distance, world_position)


## Rampa de entrada: 0 antes de `start`, 1 após `end`.
func _fade_in(distance: float, start: float, end: float) -> float:
	return clampf((distance - start) / (end - start), 0.0, 1.0)


## Constrói a cidade modular apenas quando a câmera se aproxima e a libera ao
## afastar. Histerese evita reconstrução em vaivém.
func _update_urban(record: Dictionary, distance: float, world_position: Vector3) -> void:
	var urban: Node3D = record.get("urban")
	if urban == null:
		if distance <= URBAN_BUILD_DISTANCE:
			record["urban"] = _build_urban(record, world_position)
		return
	# Constrói uma vez e só alterna a visibilidade: sem hitch a cada zoom.
	urban.visible = distance <= CITY_VISIBLE_DISTANCE


func _build_urban(record: Dictionary, world_position: Vector3) -> Node3D:
	var city: Dictionary = record["city"]
	var manager := UrbanGrowthScript.new() as Node3D
	manager.name = "Urban_" + String(record["id"])
	manager.position = to_local(world_position) + Vector3(0.0, 0.16, 0.0)
	manager.scale = Vector3.ONE * URBAN_SCALE
	_content.add_child(manager)

	manager.setup(String(record["id"]))
	manager.apply_indicators({
		"population": float(city.get("population", 0.0)),
		"industrialization": float(city.get("industrialization", 0.0)),
		"income": float(city.get("income", 0.5)),
		"jobs": float(city.get("jobs", 0.0)),
	})
	manager.apply_industry(IndustryProfileScript.derive(city))
	return manager
