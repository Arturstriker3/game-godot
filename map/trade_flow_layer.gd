extends Node3D
class_name TradeFlowLayer

## Setas de comércio agregadas por par de cidades e modal.
##
## O núcleo C++ já publica os pedidos do dia (`trade_orders_ready`); o mapa
## guarda os últimos 7 dias deste processo e desenha os maiores fluxos como
## arcos com seta. Não há economia paralela: volume = soma de `amount` dos
## pedidos do par dividida pelo número de dias guardados; produto principal =
## bem de maior volume no agregado.
##
## Render: quatro `MultiMeshInstance3D` (faixa e cabeça, terrestre e marítimo)
## com a geometria unitária de um quad e de um triângulo. Cada instância carrega
## comprimento, largura e direção no próprio transform, então a malha só é
## reescrita quando muda o CONJUNTO de fluxos (pedidos novos), a FAIXA de zoom
## (largura em px muda com a altura da câmera) ou o wrap do mundo. Pan puro não
## toca na geometria: o custo por frame é uma comparação de faixa. Desligada,
## `_process` retorna antes de qualquer trabalho.

const HISTORY_DAYS := 7
## Top-N por nível de zoom (LONGE/MÉDIO/PERTO). O total do mundo é ~60.
const TOP_FAR := 24
const TOP_MID := 40
const TOP_NEAR := 60
const ARC_SEGMENTS := 24
const ARC_LIFT := 0.32
const ARC_CONTROL_LIFT := 0.9
const MIN_THICKNESS_PX := 1.0
const MAX_THICKNESS_PX := 8.0
const ARROW_HEAD_PX := 9.0
const PICK_RADIUS_PX := 7.0
## Mesma largura do plano do mapa (`CityWorld.PLANE_SIZE.x`), usada no wrap.
const PLANE_WIDTH := 1000.0
## Folga da caixa da MultiMesh: evita recálculo de AABB a cada rebuild.
const FLOW_AABB := AABB(Vector3(-700.0, -120.0, -700.0), Vector3(1400.0, 240.0, 1400.0))
## Cores do design system: terrestre em latão, marítimo em aço.
const LAND_COLOR := Color("#C9A24A")
const SEA_COLOR := Color("#6F8F8C")
const FLOW_ALPHA := 0.86

var _world: Node3D
var _history: Array[Dictionary] = []
var _flows: Array[Dictionary] = []
var _drawn: Array[Dictionary] = []
var _city_positions: Dictionary = {}
var _city_nations: Dictionary = {}
var _dirty := true
var _band := -2
## X local da câmera no último rebuild: só o salto do wrap (meia largura do
## plano) invalida a geometria durante o pan.
var _camera_x_at_build := 0.0
var _land_ribbon: MultiMeshInstance3D
var _land_head: MultiMeshInstance3D
var _sea_ribbon: MultiMeshInstance3D
var _sea_head: MultiMeshInstance3D
var _land_material: StandardMaterial3D
var _sea_material: StandardMaterial3D
## Filtro por modal da legenda: desligar esconde as setas daquele modal sem
## descartar o agregado de 7 dias (ligar de novo volta a desenhar).
var _show_land := true
var _show_sea := true


func _ready() -> void:
	_land_material = _flow_material(LAND_COLOR)
	_sea_material = _flow_material(SEA_COLOR)
	var quad := _unit_quad()
	var head := _unit_head()
	var capacity := TOP_NEAR * ARC_SEGMENTS
	_land_ribbon = _make_multimesh(quad, _land_material, capacity)
	_land_ribbon.name = "LandRibbons"
	_land_head = _make_multimesh(head, _land_material, TOP_NEAR)
	_land_head.name = "LandHeads"
	_sea_ribbon = _make_multimesh(quad, _sea_material, capacity)
	_sea_ribbon.name = "SeaRibbons"
	_sea_head = _make_multimesh(head, _sea_material, TOP_NEAR)
	_sea_head.name = "SeaHeads"
	for node: MultiMeshInstance3D in [_land_ribbon, _land_head, _sea_ribbon, _sea_head]:
		add_child(node)
	visible = false


## Quad unitário no plano XY (normal +Z): o transform da instância leva o eixo X
## ao comprimento do segmento e o Y à largura na tela.
static func _unit_quad() -> ArrayMesh:
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = PackedVector3Array([
		Vector3(-0.5, 0.5, 0.0), Vector3(0.5, 0.5, 0.0),
		Vector3(0.5, -0.5, 0.0), Vector3(-0.5, -0.5, 0.0)])
	arrays[Mesh.ARRAY_NORMAL] = PackedVector3Array([
		Vector3.BACK, Vector3.BACK, Vector3.BACK, Vector3.BACK])
	arrays[Mesh.ARRAY_INDEX] = PackedInt32Array([0, 1, 2, 0, 2, 3])
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return mesh


## Triângulo unitário da cabeça de seta: ponta em +0,5 X, base em -0,5 X.
static func _unit_head() -> ArrayMesh:
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = PackedVector3Array([
		Vector3(0.5, 0.0, 0.0), Vector3(-0.5, 0.5, 0.0),
		Vector3(-0.5, -0.5, 0.0)])
	arrays[Mesh.ARRAY_NORMAL] = PackedVector3Array([
		Vector3.BACK, Vector3.BACK, Vector3.BACK])
	arrays[Mesh.ARRAY_INDEX] = PackedInt32Array([0, 1, 2])
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return mesh


func _make_multimesh(mesh: Mesh, material: Material,
		capacity: int) -> MultiMeshInstance3D:
	var multimesh := MultiMesh.new()
	multimesh.transform_format = MultiMesh.TRANSFORM_3D
	multimesh.use_colors = false
	multimesh.mesh = mesh
	multimesh.custom_aabb = FLOW_AABB
	multimesh.instance_count = maxi(capacity, 1)
	multimesh.visible_instance_count = 0
	var node := MultiMeshInstance3D.new()
	node.multimesh = multimesh
	node.material_override = material
	node.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	node.visible = false
	return node


func _flow_material(color: Color) -> StandardMaterial3D:
	var material := StandardMaterial3D.new()
	material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	material.albedo_color = Color(color.r, color.g, color.b, color.a * FLOW_ALPHA)
	material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	material.cull_mode = BaseMaterial3D.CULL_DISABLED
	material.no_depth_test = true
	material.render_priority = 6
	return material


## Liga a camada ao mapa: guarda a posição local de cada cidade do roster.
func setup(world: Node3D) -> void:
	_world = world
	_city_positions.clear()
	_city_nations.clear()
	if _world == null:
		return
	for record in _world.get("_city_nodes"):
		if not (record is Dictionary):
			continue
		var id := String(record.get("id", ""))
		if id.is_empty():
			continue
		_city_positions[id] = record.get("position", Vector3.ZERO)
		_city_nations[id] = String(record.get("nation", ""))
	_dirty = true


## Recebe os pedidos de um dia. O mesmo dia substitui a entrada anterior; a
## janela rolante fica com os `HISTORY_DAYS` mais recentes.
func record_orders(orders: Array, day: int) -> void:
	for index in range(_history.size() - 1, -1, -1):
		if int(_history[index].get("day", 0)) == day:
			_history.remove_at(index)
	_history.append({"day": day, "orders": orders.duplicate()})
	while _history.size() > HISTORY_DAYS:
		_history.pop_front()
	_aggregate()
	_dirty = true


## Liga/desliga o desenho (modo rotas). O agregado continua sendo mantido.
func set_active(active: bool) -> void:
	visible = active
	if active:
		_dirty = true
		return
	_drawn.clear()
	for node: MultiMeshInstance3D in [_land_ribbon, _land_head,
			_sea_ribbon, _sea_head]:
		if node != null and node.multimesh != null:
			node.multimesh.visible_instance_count = 0
			node.visible = false


func is_active() -> bool:
	return visible


## Filtro por modal (legenda do modo rotas). Só reescreve a geometria; o
## agregado e a janela de dias continuam intactos.
func set_modal_filter(show_land: bool, show_sea: bool) -> void:
	if show_land == _show_land and show_sea == _show_sea:
		return
	_show_land = show_land
	_show_sea = show_sea
	_dirty = true


func _modal_enabled(modal: String) -> bool:
	return _show_sea if modal == "sea" else _show_land


## Limite top-N do nível de zoom atual (o mesmo que a legenda exibe).
func flow_limit() -> int:
	return _flow_limit()


## Total de pares agregados na janela, antes do corte de top-N e do filtro.
func flow_total() -> int:
	return _flows.size()


## Tamanho da janela rolante em dias (a legenda mostra "últimos N dias").
func history_days() -> int:
	return HISTORY_DAYS


## Fluxo sob o cursor, ou -1. Considera as polilinhas desenhadas no último
## rebuild; a distância é medida em pixels de tela (7 px de tolerância).
func pick_flow(screen_position: Vector2, camera: Camera3D) -> int:
	if not visible or camera == null:
		return -1
	var best := PICK_RADIUS_PX
	var best_index := -1
	for index in _drawn.size():
		var points: PackedVector3Array = _drawn[index].get("points", PackedVector3Array())
		if points.size() < 2:
			continue
		for point_index in range(1, points.size()):
			var a := points[point_index - 1]
			var b := points[point_index]
			var a_world := to_global(a)
			var b_world := to_global(b)
			if camera.is_position_behind(a_world) or camera.is_position_behind(b_world):
				continue
			var distance := _segment_distance(screen_position,
					camera.unproject_position(a_world),
					camera.unproject_position(b_world))
			if distance < best:
				best = distance
				best_index = index
	return best_index


func flow_info(index: int) -> Dictionary:
	if index < 0 or index >= _drawn.size():
		return {}
	return _drawn[index].duplicate(true)


func flow_count() -> int:
	return _drawn.size()


## Só dois gatilhos reescrevem os buffers: o conjunto de fluxos mudou (`_dirty`)
## ou a faixa de zoom mudou (a largura em px depende da altura da câmera). O pan
## não entra aqui — os arcos vivem no mundo e não mudam de forma ao deslizar.
func _process(_delta: float) -> void:
	if not visible or _world == null:
		return
	var camera := get_viewport().get_camera_3d()
	if camera == null:
		return
	var band := -1
	if _world.has_method("zoom_level"):
		band = int(_world.call("zoom_level"))
	if band != _band:
		_band = band
		_dirty = true
	# Mudança de cópia lateral (wrap): a cada meia largura do plano os arcos
	# têm de ser refeitos para a cópia mais próxima da câmera.
	if absf(_world.to_local(camera.global_position).x - _camera_x_at_build) \
			> PLANE_WIDTH * 0.5:
		_dirty = true
	if not _dirty:
		return
	_dirty = false
	_rebuild(camera)


## Agrega os pedidos da janela por par + modal. Volume é t/dia e cada par tem
## um produto principal (maior soma entre os bens do par).
func _aggregate() -> void:
	var aggregate: Dictionary = {}
	var days := maxi(1, _history.size())
	for entry in _history:
		var orders: Array = entry.get("orders", [])
		for order in orders:
			if not (order is Dictionary):
				continue
			var from_id := String(order.get("from_city", ""))
			var to_id := String(order.get("to_city", ""))
			if from_id.is_empty() or to_id.is_empty():
				continue
			if not _city_positions.has(from_id) or not _city_positions.has(to_id):
				continue
			var amount := float(order.get("amount", 0.0))
			if amount <= 0.0:
				continue
			var modal := _order_modal(order)
			var key := "%s|%s|%s" % [from_id, to_id, modal]
			var flow: Dictionary = aggregate.get(key, {})
			if flow.is_empty():
				flow = {
					"from_city": from_id,
					"to_city": to_id,
					"from_nation": String(order.get("from_nation",
							_city_nations.get(from_id, ""))),
					"to_nation": String(order.get("to_nation",
							_city_nations.get(to_id, ""))),
					"mode": modal,
					"amount": 0.0,
					"goods": {},
				}
			flow["amount"] = float(flow["amount"]) + amount
			var good := String(order.get("good", "wheat"))
			var goods: Dictionary = flow["goods"]
			goods[good] = float(goods.get(good, 0.0)) + amount
			aggregate[key] = flow

	_flows.clear()
	for key in aggregate:
		var flow: Dictionary = aggregate[key]
		var goods: Dictionary = flow["goods"]
		var main_good := ""
		var main_amount := -1.0
		for good in goods:
			var value := float(goods[good])
			if value > main_amount:
				main_amount = value
				main_good = String(good)
		flow["good"] = main_good
		flow["tons_per_day"] = float(flow["amount"]) / float(days)
		flow.erase("amount")
		flow.erase("goods")
		_flows.append(flow)
	_flows.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return float(a.get("tons_per_day", 0.0)) \
				> float(b.get("tons_per_day", 0.0))
	)


func _order_modal(order: Dictionary) -> String:
	for leg in order.get("legs", []):
		if String(leg.get("mode", "")) == "ship":
			return "sea"
	return "land"


func _flow_limit() -> int:
	if _world == null:
		return TOP_NEAR
	match int(_world.call("zoom_level")):
		0:
			return TOP_FAR
		1:
			return TOP_MID
		_:
			return TOP_NEAR


func _rebuild(camera: Camera3D) -> void:
	_drawn.clear()
	_camera_x_at_build = _world.to_local(camera.global_position).x
	if _flows.is_empty() or _city_positions.is_empty():
		_write_instances(_land_ribbon, [])
		_write_instances(_land_head, [])
		_write_instances(_sea_ribbon, [])
		_write_instances(_sea_head, [])
		return
	var limit := _flow_limit()
	var max_volume := maxf(float(_flows[0].get("tons_per_day", 1.0)), 0.0001)
	var log_max := log(1.0 + max_volume)
	var ribbons: Dictionary = {"land": [], "sea": []}
	var heads: Dictionary = {"land": [], "sea": []}
	var drawn_count := 0
	for index in _flows.size():
		if drawn_count >= limit:
			break
		var flow: Dictionary = _flows[index]
		if not _modal_enabled(String(flow["mode"])):
			continue
		var from_id := String(flow["from_city"])
		var to_id := String(flow["to_city"])
		var from_local: Vector3 = _city_positions[from_id]
		var to_local: Vector3 = _city_positions[to_id]
		var wrap := 0.0
		if _world.has_method("_wrap_offset"):
			wrap = float(_world.call("_wrap_offset", from_local.x))
		from_local += Vector3(wrap, 0.0, 0.0)
		to_local += Vector3(wrap, 0.0, 0.0)
		# Cópia lateral mais curta: um fluxo que cruza a emenda do mundo não
		# atravessa o mapa inteiro.
		var dx := to_local.x - from_local.x
		if absf(dx) > PLANE_WIDTH * 0.5:
			to_local.x -= signf(dx) * PLANE_WIDTH
		var distance := from_local.distance_to(to_local)
		if distance < 0.8:
			continue
		var volume := float(flow.get("tons_per_day", 0.0))
		var ratio := log(1.0 + volume) / log_max if log_max > 0.0 else 0.0
		var thickness_px := lerpf(MIN_THICKNESS_PX, MAX_THICKNESS_PX,
				clampf(ratio, 0.0, 1.0))
		var is_sea := String(flow["mode"]) == "sea"
		var modal := "sea" if is_sea else "land"
		var color: Color = SEA_COLOR if is_sea else LAND_COLOR
		var points := _arc_points(from_local, to_local)
		var drawn := flow.duplicate(true)
		drawn["points"] = points
		drawn["color"] = color
		drawn["thickness_px"] = thickness_px
		_drawn.append(drawn)
		_build_arc(camera, points, thickness_px, ribbons[modal], heads[modal])
		drawn_count += 1
	_write_instances(_land_ribbon, ribbons["land"])
	_write_instances(_land_head, heads["land"])
	_write_instances(_sea_ribbon, ribbons["sea"])
	_write_instances(_sea_head, heads["sea"])


## Converte a polilinha do arco em instâncias: um quad por segmento (comprimento
## e largura no transform) e um triângulo na ponta.
func _build_arc(camera: Camera3D, points: PackedVector3Array,
		thickness_px: float, ribbons: Array, heads: Array) -> void:
	if points.size() < 2:
		return
	for index in range(1, points.size()):
		var a := points[index - 1]
		var b := points[index]
		var tangent := b - a
		if tangent.length_squared() <= 0.000001:
			continue
		var length := tangent.length()
		tangent = tangent / length
		var side := Vector3(tangent.z, 0.0, -tangent.x)
		if side.length_squared() <= 0.000001:
			continue
		var mid := (a + b) * 0.5
		var px_per_world := _px_per_world(camera, to_global(mid), side)
		if px_per_world <= 0.0001:
			continue
		var width := thickness_px / px_per_world
		ribbons.append(_instance_transform(mid, tangent * length,
				side * width, tangent.cross(side)))
	var tip := points[points.size() - 1]
	var back_point := points[points.size() - 2]
	var head_tangent := tip - back_point
	if head_tangent.length_squared() <= 0.000001:
		return
	head_tangent = head_tangent.normalized()
	var head_side := Vector3(head_tangent.z, 0.0, -head_tangent.x)
	if head_side.length_squared() <= 0.000001:
		return
	var head_px := _px_per_world(camera, to_global(tip), head_side)
	if head_px <= 0.0001:
		return
	var head_length := maxf(ARROW_HEAD_PX / head_px,
			thickness_px * 2.4 / head_px)
	var head_width := maxf(thickness_px * 1.9 / head_px,
			ARROW_HEAD_PX * 0.7 / head_px)
	var center := tip - head_tangent * (head_length * 0.5)
	heads.append(_instance_transform(center, head_tangent * head_length,
			head_side * head_width, head_tangent.cross(head_side)))


static func _instance_transform(origin: Vector3, x_axis: Vector3,
		y_axis: Vector3, z_axis: Vector3) -> Transform3D:
	return Transform3D(Basis(x_axis, y_axis, z_axis), origin)


## Escreve só as instâncias usadas e esconde o nó quando não há nenhuma. O
## buffer tem capacidade fixa (TOP_NEAR), então nada é recriado no rebuild.
func _write_instances(node: MultiMeshInstance3D, transforms: Array) -> void:
	if node == null or node.multimesh == null:
		return
	var multimesh := node.multimesh
	var count := mini(transforms.size(), multimesh.instance_count)
	for index in count:
		multimesh.set_instance_transform(index, transforms[index])
	multimesh.visible_instance_count = count
	node.visible = count > 0


func _arc_points(from_local: Vector3, to_local: Vector3) -> PackedVector3Array:
	var points := PackedVector3Array()
	var mid := (from_local + to_local) * 0.5
	var direction := (to_local - from_local).normalized()
	var side := Vector3(-direction.z, 0.0, direction.x)
	var bow := clampf(from_local.distance_to(to_local) * 0.14, 1.5, 42.0)
	var control := mid + side * bow + Vector3(0.0, ARC_CONTROL_LIFT, 0.0)
	var start := from_local + Vector3(0.0, ARC_LIFT, 0.0)
	var end := to_local + Vector3(0.0, ARC_LIFT, 0.0)
	for index in ARC_SEGMENTS + 1:
		var t := float(index) / float(ARC_SEGMENTS)
		var one := 1.0 - t
		var point := start * (one * one) + control * (2.0 * one * t) \
				+ end * (t * t)
		points.append(point)
	return points


## Pixels de tela por unidade de mundo numa direção, na profundidade do ponto.
func _px_per_world(camera: Camera3D, point: Vector3, axis: Vector3) -> float:
	if axis.length_squared() <= 0.000001:
		return 0.0
	var a := camera.unproject_position(point)
	var b := camera.unproject_position(point + axis)
	return a.distance_to(b)


static func _segment_distance(point: Vector2, a: Vector2, b: Vector2) -> float:
	var ab := b - a
	var length_squared := ab.length_squared()
	if length_squared <= 0.0001:
		return point.distance_to(a)
	var t := clampf((point - a).dot(ab) / length_squared, 0.0, 1.0)
	return point.distance_to(a + ab * t)
