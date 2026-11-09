extends Node3D
class_name CityWorld

## Mundo baseado em cidades: fundo de mapa real (ne2.png), uma cidade 3D por
## nação e a forma/fronteira de cada nação calculada por Voronoi a partir das
## cidades. Os dados (posição, nação, população) vêm do núcleo C++ (EconomySim).

const MAP_SIZE_PX := Vector2(5000.0, 2500.0)
const PLANE_SIZE := Vector2(1000.0, 500.0)
const PIXEL_TO_WORLD := 0.2

const CITY_MODEL: PackedScene = preload("res://assets/models/city.glb")
const GPS_TEXTURE: Texture2D = preload("res://map/map_data/ne2.png")
const MAP_SHADER: Shader = preload("res://map/map_shaders/tactical_map.gdshader")
const LABEL_FONT: Font = preload("res://assets/fonts/BarlowCondensed-SemiBold.ttf")

const VORONOI_RESOLUTION := Vector2i(1000, 500)
const CITY_TARGET_SIZE := 5.0
const MARKER_COLOR := Color("#0A1212")
const MARKER_DOT := Color("#E8E2D0")

const NATION_COLORS := {
	"GBR": Color(0.78, 0.12, 0.16),
	"FRA": Color(0.10, 0.30, 0.85),
	"DEU": Color(0.36, 0.36, 0.40),
	"NED": Color(0.95, 0.45, 0.10),
	"BEL": Color(0.90, 0.75, 0.12),
}


var _content: Node3D


## Monta o mundo: chão GPS, fronteiras Voronoi e modelos de cidade.
func build(cities: Array) -> void:
	_content = Node3D.new()
	add_child(_content)

	_build_gps_plane()
	_build_voronoi(cities)
	_build_cities(cities)

	# Cópias laterais: o mundo dá a volta em X (cilíndrico), então quem sai por
	# um lado reaparece no outro.
	for offset in [-PLANE_SIZE.x, PLANE_SIZE.x]:
		var copy := _content.duplicate() as Node3D
		copy.position = Vector3(offset, 0.0, 0.0)
		add_child(copy)


func pixel_to_world(px: Vector2) -> Vector3:
	return Vector3(
		(px.x - MAP_SIZE_PX.x * 0.5) * PIXEL_TO_WORLD,
		0.0,
		(px.y - MAP_SIZE_PX.y * 0.5) * PIXEL_TO_WORLD,
	)


func _build_gps_plane() -> void:
	var plane := PlaneMesh.new()
	plane.size = PLANE_SIZE

	var material := ShaderMaterial.new()
	material.shader = MAP_SHADER
	material.set_shader_parameter("map_texture", GPS_TEXTURE)

	var mesh_instance := MeshInstance3D.new()
	mesh_instance.mesh = plane
	mesh_instance.material_override = material
	_content.add_child(mesh_instance)


func _build_voronoi(cities: Array) -> void:
	var texture := _generate_voronoi(cities)

	var plane := PlaneMesh.new()
	plane.size = PLANE_SIZE

	var material := StandardMaterial3D.new()
	material.albedo_texture = texture
	material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED

	var mesh_instance := MeshInstance3D.new()
	mesh_instance.mesh = plane
	mesh_instance.position = Vector3(0.0, 0.15, 0.0)
	mesh_instance.material_override = material
	_content.add_child(mesh_instance)


## Gera um diagrama de Voronoi: cada pixel pertence à cidade mais próxima.
## O preenchimento é translúcido e as bordas (troca de cidade) ficam opacas.
func _generate_voronoi(cities: Array) -> ImageTexture:
	var w: int = VORONOI_RESOLUTION.x
	var h: int = VORONOI_RESOLUTION.y

	var sites: Array = []
	for city in cities:
		sites.append({
			"pos": Vector2(city["x"], city["y"]),
			"color": NATION_COLORS.get(city["nation"], Color.WHITE),
		})
	if sites.is_empty():
		return null

	# Dono (índice do site) de cada pixel.
	var owner := PackedInt32Array()
	owner.resize(w * h)

	var sx: float = MAP_SIZE_PX.x / float(w)
	var sy: float = MAP_SIZE_PX.y / float(h)

	for j in range(h):
		for i in range(w):
			var px: float = (float(i) + 0.5) * sx
			var py: float = (float(j) + 0.5) * sy
			var best := -1
			var best_dist := INF
			for s in range(sites.size()):
				# Distância com wrap em X: o mapa é cilíndrico, então a borda
				# esquerda é vizinha da direita.
				var dx: float = absf(px - sites[s]["pos"].x)
				dx = minf(dx, MAP_SIZE_PX.x - dx)
				var dy: float = py - sites[s]["pos"].y
				var dist: float = dx * dx + dy * dy
				if dist < best_dist:
					best_dist = dist
					best = s
			owner[j * w + i] = best

	var bytes := PackedByteArray()
	bytes.resize(w * h * 4)

	for j in range(h):
		for i in range(w):
			var idx: int = j * w + i
			var s: int = owner[idx]
			var is_border := false
			if i > 0 and owner[idx - 1] != s:
				is_border = true
			elif i < w - 1 and owner[idx + 1] != s:
				is_border = true
			elif j > 0 and owner[idx - w] != s:
				is_border = true
			elif j < h - 1 and owner[idx + w] != s:
				is_border = true

			var color: Color = sites[s]["color"]
			color.a = 0.85 if is_border else 0.16
			var o: int = idx * 4
			bytes[o] = int(color.r * 255.0)
			bytes[o + 1] = int(color.g * 255.0)
			bytes[o + 2] = int(color.b * 255.0)
			bytes[o + 3] = int(color.a * 255.0)

	var image := Image.create_from_data(w, h, false, Image.FORMAT_RGBA8, bytes)
	return ImageTexture.create_from_image(image)


func _build_cities(cities: Array) -> void:
	for city in cities:
		var position: Vector3 = pixel_to_world(Vector2(city["x"], city["y"]))
		_spawn_city_marker(position, city["name"], NATION_COLORS.get(city["nation"], Color.WHITE))
		_spawn_city_model(position)


func _spawn_city_marker(position: Vector3, city_name: String, color: Color) -> void:
	_add_disc(position + Vector3(0.0, 0.14, 0.0), 1.55, 0.10, MARKER_COLOR)
	_add_disc(position + Vector3(0.0, 0.20, 0.0), 1.05, 0.12, color)
	_add_disc(position + Vector3(0.0, 0.27, 0.0), 0.34, 0.10, MARKER_DOT)
	_spawn_city_label(position, city_name, color)


## Disco plano usado para desenhar o marcador da cidade em camadas.
func _add_disc(position: Vector3, radius: float, height: float, color: Color) -> void:
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


## Nome da cidade em caixa alta, como legenda de carta militar.
func _spawn_city_label(position: Vector3, city_name: String, color: Color) -> void:
	var label := Label3D.new()
	label.text = city_name.to_upper()
	label.font = LABEL_FONT
	label.font_size = 30
	label.pixel_size = 0.024
	label.modulate = color.lightened(0.35)
	label.outline_modulate = Color(0.03, 0.05, 0.05, 0.9)
	label.outline_size = 12
	label.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	label.position = position + Vector3(0.0, 3.2, 0.0)
	_content.add_child(label)


func _spawn_city_model(position: Vector3) -> void:
	var model := CITY_MODEL.instantiate() as Node3D
	var scale := _scale_to_size(model, CITY_TARGET_SIZE)
	model.scale = Vector3.ONE * scale
	model.position = position + Vector3(0.0, 0.4, 0.0)
	_content.add_child(model)


## Escala o modelo para que seu maior eixo meça o tamanho alvo.
func _scale_to_size(root: Node3D, target: float) -> float:
	var bounds := _combined_aabb(root)
	var longest := maxf(bounds.size.x, maxf(bounds.size.y, bounds.size.z))
	return 1.0 if longest <= 0.0 else target / longest


func _combined_aabb(root: Node3D) -> AABB:
	var result := AABB()
	var found := false
	var stack: Array[Node] = [root]
	while not stack.is_empty():
		var node: Node = stack.pop_back()
		if node is MeshInstance3D and node.mesh != null:
			var box: AABB = node.transform * node.get_aabb()
			result = box if not found else result.merge(box)
			found = true
		stack.append_array(node.get_children())
	return result
