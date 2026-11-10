extends SceneTree

## MEDIÇÃO do tamanho uniforme dos depósitos (mapa "resources").
##
## Decisão de design: TODOS os marcadores (minerais E trigo) têm o MESMO
## tamanho (MARKER_SIZE), independente de reserva/aptidão. Consequentemente os
## anéis de hover (1.8×MARKER_SIZE) e seleção (2.8×MARKER_SIZE) e o raio de
## pick (max(26px, MARKER_SIZE*0.5*1.35 projetado)) também são uniformes.
##
## Aqui medimos, em pixels projetados (unproject_position, matemática pura), o
## diâmetro do ponto, dos anéis de hover/seleção e o raio de pick em 1618
## (Chaillac) e 1232 (Ferdinand) e exigimos que cada medida seja IGUAL (±1 px)
## entre os dois.
##
##   godot --headless --path . --script res://tools/deposit_ring_test.gd

const MAIN_SCENE := "res://game/main_game.tscn"
const FOCUS := [0, 100]  # dois territórios arbitrários (agregados)
const MAX_DELTA_PX := 1.0

var _failures: Array[String] = []


func _init() -> void:
	_run.call_deferred()


func _run() -> void:
	var game: Node3D = (load(MAIN_SCENE) as PackedScene).instantiate()
	root.add_child(game)
	for _i in 6:
		await process_frame

	var player := game.get_node("Player") as Node3D
	player.camera_can_process = false
	player.camera_can_automatic_pan = false
	var camera: Camera3D = player.camera
	var mode_layer = game.city_world._mode_layer

	game.hud.set_map_mode("resources")
	for _i in 2:
		await process_frame

	print("=== deposit_ring_test ===")
	print("mineral_points = %d" % mode_layer._mineral_points.size())
	print("MARKER_SIZE = %.3f (esperado: constante)" % mode_layer.MARKER_SIZE)
	var material = mode_layer._mineral_mesh.material_override
	print("billboard_keep_scale = %s (esperado: true)"
			% [material.billboard_keep_scale])
	if material.billboard_keep_scale != true:
		_failures.append("material.billboard_keep_scale não está ligado")

	var measures: Dictionary = {}
	for idx in FOCUS:
		measures[idx] = await _measure(mode_layer, camera, idx)

	game.queue_free()
	await process_frame

	_compare(measures, "dot")
	_compare(measures, "hover")
	_compare(measures, "sel")
	_compare(measures, "pick")

	if _failures.is_empty():
		print("PASS: ponto, hover, seleção e pick uniformes (±%dpx)"
				% int(MAX_DELTA_PX))
		quit(0)
		return
	for failure in _failures:
		push_error("[deposit_ring] " + failure)
	quit(1)


## Exige que a medida `key` seja igual (±MAX_DELTA_PX) entre os dois depósitos.
func _compare(measures: Dictionary, key: String) -> void:
	var va: float = measures[FOCUS[0]][key]
	var vb: float = measures[FOCUS[1]][key]
	var delta := absf(va - vb)
	if delta > MAX_DELTA_PX:
		_failures.append("%s: %d=%.1fpx vs %d=%.1fpx (Δ%.1fpx > %dpx)"
				% [key, FOCUS[0], va, FOCUS[1], vb, delta, int(MAX_DELTA_PX)])


## Mede os diâmetros projetados para um depósito (câmera olhando para ele).
func _measure(mode_layer, camera: Camera3D, idx: int) -> Dictionary:
	var point: Array = mode_layer._mineral_points[idx]
	var name: String = String(point[7])
	var size: float = mode_layer._marker_size(point, false)
	var center: Vector3 = mode_layer.to_global(mode_layer._marker_world(point))

	# Câmera olhando o depósito (projeção estável, depósito perto do centro).
	camera.global_position = center + Vector3(0.0, 50.0, 30.0)
	camera.look_at(center, Vector3.UP)
	await process_frame

	# Ponto desenhado: quad 1.0 × escala `size` (billboard_keep_scale = true).
	var dot_px := _projected_diameter(camera, center, size)

	# Hover (aplica o piso de tela em _process; aguarda um frame).
	mode_layer.set_deposit_hover(idx)
	await process_frame
	var hover_ring = mode_layer._hover_ring
	var hover_px := _projected_diameter(camera, hover_ring.global_position,
			hover_ring.pixel_size * mode_layer.RING_TEXTURE_PX * hover_ring.scale.x)

	# Seleção.
	mode_layer.set_deposit_selected(idx)
	await process_frame
	var sel_ring = mode_layer._selection_ring
	var sel_px := _projected_diameter(camera, sel_ring.global_position,
			sel_ring.pixel_size * mode_layer.RING_TEXTURE_PX * sel_ring.scale.x)

	# Raio de pick em tela: max(26px, raio projetado de MARKER_SIZE*0.5 * 1.35).
	var right := camera.global_transform.basis.x
	var center_px := camera.unproject_position(center)
	var edge_px := camera.unproject_position(
			center + right * (mode_layer.MARKER_SIZE * 0.5))
	var pick_px := maxf(26.0, center_px.distance_to(edge_px) * 1.35)

	print(("idx %d %-14s size=%.3f  dot=%.1fpx  hover=%.1fpx  sel=%.1fpx  "
			+ "pick=%.1fpx")
			% [idx, name, size, dot_px, hover_px, sel_px, pick_px])

	mode_layer.set_deposit_hover(-1)
	mode_layer.set_deposit_selected(-1)
	await process_frame

	return {"dot": dot_px, "hover": hover_px, "sel": sel_px, "pick": pick_px}


## Diâmetro em pixels de tela de um segmento horizontal de `diameter` unidades,
## centrado em `center`, projetado pela câmera.
func _projected_diameter(camera: Camera3D, center: Vector3, diameter: float) -> float:
	var right := camera.global_transform.basis.x
	var a := camera.unproject_position(center - right * diameter * 0.5)
	var b := camera.unproject_position(center + right * diameter * 0.5)
	return a.distance_to(b)
