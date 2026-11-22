extends Node3D

## Camada OPCIONAL do mapa de energia (E6): redes e calor do potencial.
##
## Ela complementa os marcadores de POTENCIAL do `map_mode_layer.gd` — que usam
## a MESMA infraestrutura do mapa de recursos — sem duplicar nada deles:
##   - redes: um ponto por cidade com a cor do estado publicado pelo núcleo
##     (conectada/parcial/isolada) e as interconexões E5 como retas na espessura
##     da capacidade e na cor do fluxo do dia;
##   - calor: preenchimento das regiões por fonte (solar/eólico/hídrico),
##     rasterizado de `renewable_potential.json` / `hydro_potential.json` com a
##     MESMA métrica dos marcadores, intensidade relativa ao máximo do mundo.
##
## Tudo desligado por padrão: o mapa de energia paga só os marcadores. Ligada, a
## camada custa 2 draw calls (redes) + 1 por fonte de calor.

const MAP_PX := Vector2(5000.0, 2500.0)
const PIXEL_TO_WORLD := 0.2
const RESOURCE_DOT: Texture2D = preload("res://assets/icons/places/resource_dot.png")
const RENEWABLE_PATH := "res://data/definitions/energy/renewable_potential.json"
const HYDRO_PATH := "res://data/definitions/energy/hydro_potential.json"

## Cores do estado da cidade (mesma paleta do resto do mapa: tinta/latão/aço).
const CONNECTION_COLORS := {
	"connected": Color("#6F8F5F"),
	"partial": Color("#C9A24A"),
	"isolated": Color("#A3452F"),
}
const DOT_SIZE := 0.62
const DOT_ALPHA := 0.9

## Interconexões: espessura em px de TELA pela capacidade (escala log) e cor pelo
## fluxo do dia (ociosa = cinza; ativa = aço; só contrato = latão discreto).
const LINE_MIN_PX := 1.2
const LINE_MAX_PX := 6.0
const LINE_IDLE := Color(0.42, 0.42, 0.42)
const LINE_FLOW := Color("#6F8F8C")
const LINE_CONTRACT := Color("#C9A24A")
const LINE_HEIGHT := 0.19

## Potencial: textura de calor de 500×250 px sobre o plano de 1000×500 u.
const HEAT_WIDTH := 500
const HEAT_HEIGHT := 250
const HEAT_RADIUS := 7
const HEAT_ALPHA := 0.62
## Fator do calor quando ele entra SOZINHO porque só uma fonte está filtrada:
## o mapa continua legível (o calor é pano de fundo, não assunto).
const HEAT_AUTO_FACTOR := 0.55
const HEAT_COLORS := {
	"solar": Color("#E8B028"),
	"wind": Color("#6F8F8C"),
	"hydro": Color("#6FA8DC"),
}

var _active := false
var _overlays: Dictionary = {"grid": false, "solar": false, "wind": false, "hydro": false}
## Fonte cujo calor foi ligado pelo filtro de fonte única (entra discreto).
var _auto_heat := ""
var _status: Dictionary = {}
var _city_px: Dictionary = {}
var _city_nation: Dictionary = {}
var _nation_px: Dictionary = {}
var _dot_mesh: MultiMeshInstance3D
var _line_mesh: MultiMeshInstance3D
var _line_material: StandardMaterial3D
var _potential_meshes: Dictionary = {}
var _potential_built: Dictionary = {}
var _potential_data: Dictionary = {}
var _band := -2


func _ready() -> void:
	_dot_mesh = _make_dot_layer()
	_line_mesh = _make_line_layer()
	visible = false
	if not _status.is_empty():
		_rebuild_dots()
		_rebuild_lines()


## Guarda o centro (px de mapa) e a nação de cada cidade do cenário. Os
## marcadores de energia do mode layer usam a mesma projeção.
func setup(cities: Array) -> void:
	_city_px.clear()
	_city_nation.clear()
	_nation_px.clear()
	for entry: Variant in cities:
		if not (entry is Dictionary):
			continue
		var city: Dictionary = entry
		var city_id := str(city.get("id", ""))
		if city_id.is_empty():
			continue
		var point := Vector2(_json_number(city.get("x")),
				_json_number(city.get("y")))
		var nation := str(city.get("nation", ""))
		_city_px[city_id] = point
		_city_nation[city_id] = nation
		if not nation.is_empty():
			var current: Vector2 = _nation_px.get(nation, Vector2.ZERO)
			_nation_px[nation] = current + point
	# Média simples dos centros das cidades: âncora declarada da nação (não é a
	# capital nem um centro geográfico oficial).
	for nation: Variant in _nation_px:
		var count := 0
		for value: Variant in _city_nation.values():
			if String(value) == String(nation):
				count += 1
		var anchor: Vector2 = _nation_px[nation]
		_nation_px[nation] = anchor / float(maxi(count, 1))
	_rebuild_lines()


## Liga/desliga a camada inteira (modo energia). Mantém os dados e texturas.
func set_active(active: bool) -> void:
	_active = active
	visible = active
	if not active:
		for mesh: MultiMeshInstance3D in [_dot_mesh, _line_mesh]:
			if mesh != null:
				mesh.visible = false
		for kind: Variant in _potential_meshes:
			var potential: MeshInstance3D = _potential_meshes[kind]
			if potential != null:
				potential.visible = false
		return
	_refresh_visibility()


## Toggles da legenda. `grid` = cidades + interconexões; solar/wind/hydro =
## camada de calor do potencial. Qualquer chave ausente fica desligada. `auto`
## (opcional) nomeia a fonte cujo calor foi ligado pelo filtro de fonte única:
## ela desenha em opacidade baixa para não tapar os marcadores.
func set_overlays(overlays: Dictionary) -> void:
	for kind in ["grid", "solar", "wind", "hydro"]:
		_overlays[kind] = bool(overlays.get(kind, false))
	_auto_heat = String(overlays.get("auto", ""))
	_refresh_visibility()


func overlays() -> Dictionary:
	return _overlays.duplicate()


## Fonte com calor automático ("" = nenhum). O teste confere este estado.
func auto_heat() -> String:
	return _auto_heat


## Status de energia do dia: pinta o estado das cidades e o fluxo das linhas.
func refresh(status: Dictionary) -> void:
	if status.is_empty():
		return
	_status = status
	_rebuild_dots()
	_rebuild_lines()


func _refresh_visibility() -> void:
	if not _active:
		return
	_dot_mesh.visible = bool(_overlays.get("grid", false))
	_line_mesh.visible = bool(_overlays.get("grid", false))
	for kind in ["solar", "wind", "hydro"]:
		var enabled := bool(_overlays.get(kind, false))
		if enabled:
			_ensure_potential(kind)
		var mesh: MeshInstance3D = _potential_meshes.get(kind)
		if mesh != null:
			mesh.visible = enabled
			var material := mesh.material_override as StandardMaterial3D
			if material != null:
				# Calor automático de fonte única: mesma textura, opacidade baixa.
				var alpha := HEAT_AUTO_FACTOR if String(kind) == _auto_heat else 1.0
				material.albedo_color = Color(1.0, 1.0, 1.0, alpha)


# --- Redes -------------------------------------------------------------------

func _make_dot_layer() -> MultiMeshInstance3D:
	return _make_marker_layer("EnergyGridCities", RESOURCE_DOT, 2)


func _make_line_layer() -> MultiMeshInstance3D:
	var material := StandardMaterial3D.new()
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	material.vertex_color_use_as_albedo = true
	material.cull_mode = BaseMaterial3D.CULL_DISABLED
	material.no_depth_test = true
	material.render_priority = 4
	_line_material = material

	var multimesh := MultiMesh.new()
	multimesh.transform_format = MultiMesh.TRANSFORM_3D
	multimesh.use_colors = true
	multimesh.instance_count = 0
	multimesh.mesh = _unit_ground_quad()
	var node := MultiMeshInstance3D.new()
	node.name = "EnergyInterconnects"
	node.multimesh = multimesh
	node.material_override = material
	node.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	node.visible = false
	add_child(node)
	return node


## Quad unitário deitado no chão (plano XZ): o transform leva o eixo X ao
## comprimento da linha e o eixo Z à largura.
static func _unit_ground_quad() -> ArrayMesh:
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = PackedVector3Array([
		Vector3(-0.5, 0.0, 0.5), Vector3(0.5, 0.0, 0.5),
		Vector3(0.5, 0.0, -0.5), Vector3(-0.5, 0.0, -0.5)])
	arrays[Mesh.ARRAY_NORMAL] = PackedVector3Array([
		Vector3.UP, Vector3.UP, Vector3.UP, Vector3.UP])
	arrays[Mesh.ARRAY_INDEX] = PackedInt32Array([0, 1, 2, 0, 2, 3])
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return mesh


func _make_marker_layer(node_name: String, texture: Texture2D,
		priority: int) -> MultiMeshInstance3D:
	var material := StandardMaterial3D.new()
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	material.albedo_texture = texture
	material.billboard_mode = BaseMaterial3D.BILLBOARD_ENABLED
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

	var node := MultiMeshInstance3D.new()
	node.name = node_name
	node.multimesh = multimesh
	node.material_override = material
	node.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	node.visible = false
	add_child(node)
	return node


func _rebuild_dots() -> void:
	if _dot_mesh == null:
		return
	var cities: Dictionary = _status.get("cities", {})
	var rows: Array = []
	for city_id: Variant in cities:
		var row: Dictionary = cities[city_id]
		if not bool(row.get("has_data", false)):
			continue
		var point: Variant = _city_px.get(String(city_id))
		if point == null:
			continue
		rows.append({"px": point, "connection": String(row.get("connection", ""))})
	var multimesh := _dot_mesh.multimesh
	if multimesh.instance_count < rows.size():
		multimesh.instance_count = rows.size()
	for index in rows.size():
		var row: Dictionary = rows[index]
		var point: Vector2 = row["px"]
		var color: Color = CONNECTION_COLORS.get(String(row["connection"]),
				Color(0.55, 0.55, 0.55))
		_set_marker(_dot_mesh, index, point.x, point.y, DOT_SIZE,
				Color(color.r, color.g, color.b, DOT_ALPHA))
	multimesh.visible_instance_count = rows.size()
	_dot_mesh.visible = bool(_overlays.get("grid", false)) and not rows.is_empty()


func _rebuild_lines() -> void:
	if _line_mesh == null:
		return
	var lines: Array = _status.get("interconnects", [])
	var multimesh := _line_mesh.multimesh
	var written := 0
	var camera := get_viewport().get_camera_3d()
	var max_capacity := 1.0
	for line: Variant in lines:
		if line is Dictionary:
			max_capacity = maxf(max_capacity, float(line.get("capacity_mw", 0.0)))
	for line: Variant in lines:
		if not (line is Dictionary):
			continue
		var row: Dictionary = line
		var a: Variant = _nation_px.get(String(row.get("a", "")))
		var b: Variant = _nation_px.get(String(row.get("b", "")))
		if a == null or b == null:
			continue
		var from_px: Vector2 = a
		var to_px: Vector2 = b
		var from := _pixel_to_world(from_px)
		var to := _pixel_to_world(to_px)
		to.y = from.y
		var delta := to - from
		delta.y = 0.0
		var length := delta.length()
		if length < 0.5:
			continue
		var capacity := float(row.get("capacity_mw", 0.0))
		var ratio := 0.0
		if max_capacity > 0.0 and capacity > 0.0:
			ratio = clampf(log(1.0 + capacity) / log(1.0 + max_capacity), 0.0, 1.0)
		var thickness_px := lerpf(LINE_MIN_PX, LINE_MAX_PX, ratio)
		var width := 0.12
		if camera != null:
			var px_per_world := _px_per_world(camera, to_global(from + delta * 0.5))
			if px_per_world > 0.0001:
				width = thickness_px / px_per_world
		var flow := float(row.get("flow_mwh", 0.0))
		var contract := float(row.get("contract_flow_mwh", 0.0))
		var color := LINE_IDLE
		if flow > 0.0 and capacity > 0.0:
			color = LINE_IDLE.lerp(LINE_FLOW, clampf(flow / capacity, 0.35, 1.0))
		elif contract > 0.0:
			color = LINE_CONTRACT.darkened(0.15)
		var alpha := 0.85 if flow > 0.0 or contract > 0.0 else 0.35
		var direction := delta / length
		var side := Vector3(-direction.z, 0.0, direction.x)
		var basis := Basis(direction * length, Vector3.UP, side * width)
		var origin := from + delta * 0.5 + Vector3(0.0, LINE_HEIGHT, 0.0)
		if multimesh.instance_count <= written:
			multimesh.instance_count = written + 1
		multimesh.set_instance_transform(written, Transform3D(basis, origin))
		multimesh.set_instance_color(written,
				Color(color.r, color.g, color.b, alpha).srgb_to_linear())
		written += 1
	multimesh.visible_instance_count = written
	_line_mesh.visible = bool(_overlays.get("grid", false)) and written > 0


func _px_per_world(camera: Camera3D, point: Vector3) -> float:
	var a := camera.unproject_position(point)
	var b := camera.unproject_position(point + Vector3(1.0, 0.0, 0.0))
	return a.distance_to(b)


## Recalcula a espessura das linhas quando a faixa de zoom muda (a largura é
## constante em px de tela). Pan não reescreve nada.
func _process(_delta: float) -> void:
	if not _active or not bool(_overlays.get("grid", false)):
		return
	var parent := get_parent()
	if parent == null or not parent.has_method("zoom_level"):
		return
	var band := int(parent.call("zoom_level"))
	if band == _band:
		return
	_band = band
	_rebuild_lines()


# --- Potencial renovável -----------------------------------------------------

func _ensure_potential(kind: String) -> void:
	if bool(_potential_built.get(kind, false)):
		return
	_potential_built[kind] = true
	_load_potential_data()
	var values := _potential_values(kind)
	if values.is_empty():
		return
	var image := _rasterize_heat(values, HEAT_COLORS.get(kind, Color.WHITE))
	if image == null:
		return
	var texture := ImageTexture.create_from_image(image)
	var material := StandardMaterial3D.new()
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	material.albedo_texture = texture
	material.cull_mode = BaseMaterial3D.CULL_DISABLED
	material.no_depth_test = true
	material.render_priority = 1
	var plane := PlaneMesh.new()
	plane.size = Vector2(MAP_PX.x * PIXEL_TO_WORLD, MAP_PX.y * PIXEL_TO_WORLD)
	var mesh := MeshInstance3D.new()
	mesh.name = "EnergyPotential%s" % kind.capitalize()
	mesh.mesh = plane
	mesh.material_override = material
	mesh.position = Vector3(0.0, 0.14, 0.0)
	mesh.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	mesh.visible = false
	add_child(mesh)
	_potential_meshes[kind] = mesh


## Valores por cidade: solar = PVOUT anual (kWh/kWp/ano); vento = densidade de
## potência a 100 m (W/m²); hídrico = potencial técnico (MW) — as MESMAS
## métricas dos marcadores. Fonte ausente não entra.
func _potential_values(kind: String) -> Dictionary:
	var out: Dictionary = {}
	var city_rows: Dictionary = _potential_data.get("cities", {})
	for city_id: Variant in city_rows:
		if not _city_px.has(String(city_id)):
			continue
		var row: Dictionary = city_rows[city_id]
		var value := 0.0
		match kind:
			"solar":
				var solar: Dictionary = row.get("solar", {})
				value = _json_number(solar.get("pvout_kwh_kwp_year"))
			"wind":
				var wind: Dictionary = row.get("wind", {})
				value = _json_number(wind.get("power_density_w_m2"))
			"hydro":
				var hydro: Dictionary = _potential_data.get("hydro", {}).get(
						String(city_id), {})
				value = _json_number(hydro.get("potential_mw"))
		if value > 0.0:
			out[city_id] = value
	return out


func _load_potential_data() -> void:
	if not _potential_data.is_empty():
		return
	var renewable: Variant = JSON.parse_string(
			FileAccess.get_file_as_string(RENEWABLE_PATH))
	var hydro: Variant = JSON.parse_string(
			FileAccess.get_file_as_string(HYDRO_PATH))
	var cities: Dictionary = {}
	if renewable is Dictionary:
		cities = (renewable as Dictionary).get("by_city", {})
	var hydro_cities: Dictionary = {}
	if hydro is Dictionary:
		hydro_cities = (hydro as Dictionary).get("by_city", {})
	_potential_data = {"cities": cities, "hydro": hydro_cities}


## Rasteriza os valores num halo gaussiano de 500×250 e devolve a imagem RGBA
## (alfa = intensidade relativa ao máximo do mundo). Sem valor = sem halo.
func _rasterize_heat(values: Dictionary, color: Color) -> Image:
	var width := HEAT_WIDTH
	var height := HEAT_HEIGHT
	var accumulator := PackedFloat32Array()
	accumulator.resize(width * height)
	var maximum := 0.0
	for value: Variant in values.values():
		maximum = maxf(maximum, float(value))
	if maximum <= 0.0:
		return null
	var kernel: Array = []
	var radius := HEAT_RADIUS
	for dy in range(-radius, radius + 1):
		for dx in range(-radius, radius + 1):
			var distance := sqrt(float(dx * dx + dy * dy))
			if distance > float(radius):
				continue
			var falloff := 1.0 - distance / float(radius + 1)
			kernel.append([dx, dy, falloff * falloff])
	for city_id: Variant in values:
		var point: Vector2 = _city_px[city_id]
		var cx := int(point.x / MAP_PX.x * float(width))
		var cy := int(point.y / MAP_PX.y * float(height))
		var weight := float(values[city_id]) / maximum
		for cell: Array in kernel:
			var px: int = cx + int(cell[0])
			var py: int = cy + int(cell[1])
			if px < 0 or py < 0 or px >= width or py >= height:
				continue
			accumulator[py * width + px] += weight * float(cell[2])
	var image := Image.create(width, height, false, Image.FORMAT_RGBA8)
	for y in height:
		for x in width:
			var value := clampf(accumulator[y * width + x], 0.0, 1.0)
			if value <= 0.01:
				continue
			image.set_pixel(x, y, Color(color.r, color.g, color.b,
					value * HEAT_ALPHA))
	return image


## JSON traz `null` onde o dado falta; `float(null)` estoura no GDScript.
static func _json_number(value: Variant) -> float:
	if typeof(value) == TYPE_FLOAT or typeof(value) == TYPE_INT:
		return float(value)
	return 0.0


# --- Utilidades de posição (as mesmas do map_mode_layer) ----------------------

func _pixel_to_world(point: Vector2) -> Vector3:
	return Vector3((point.x - MAP_PX.x * 0.5) * PIXEL_TO_WORLD, 0.2,
			(point.y - MAP_PX.y * 0.5) * PIXEL_TO_WORLD)


func _set_marker(layer: MultiMeshInstance3D, index: int, px: float, py: float,
		size: float, color: Color) -> void:
	var position := _pixel_to_world(Vector2(px, py))
	var basis := Basis().scaled(Vector3(size, size, size))
	layer.multimesh.set_instance_transform(index, Transform3D(basis, position))
	layer.multimesh.set_instance_color(index, color.srgb_to_linear())
