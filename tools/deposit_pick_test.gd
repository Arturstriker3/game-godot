extends SceneTree

## MEDIÇÃO do pick de depósitos no mapa "resources" (bug: Chaillac idx 1618 não
## responde a hover/clique; Ferdinand Mine idx 1232 responde).
##
## Roda NÃO-headless (precisa de render 3D):
##   godot --path . --script res://tools/deposit_pick_test.gd
##
## Determinístico (sem warp de mouse, que sofre offset da barra de título):
##   - espaço de get_mouse_position vs unproject_position (base vs janela);
##   - autocoerência de pick_deposit no centro projetado;
##   - bloqueio de GUI: caminha a árvore de Control e lista quem (visível,
##     mouse_filter != IGNORE) cobre o centro de cada depósito.
## Roda em duas molduras de câmera (padrão e centrada em Chaillac) × 3 janelas.
## Salva em docs/debug/deposit_pick_report.txt.

const MAIN_SCENE := "res://game/main_game.tscn"
const REPORT_PATH := "res://docs/debug/deposit_pick_report.txt"

const WINDOW_SIZES: Array[Vector2i] = [
	Vector2i(1920, 1080),
	Vector2i(1920, 1009),
	Vector2i(1366, 768),
]

const FOCUS_IDX := [0, 100]  # dois territórios arbitrários (agregados)

var _report: PackedStringArray = []
var _game: Node3D
var _player: Node3D
var _city_world
var _hud
var _mode_layer
var _camera: Camera3D


func _init() -> void:
	_run.call_deferred()


func _log(line: String) -> void:
	_report.append(line)
	print(line)


func _wait_frames(count: int) -> void:
	for _i in count:
		await process_frame


func _run() -> void:
	_game = (load(MAIN_SCENE) as PackedScene).instantiate()
	root.add_child(_game)
	await _wait_frames(8)

	_player = _game.get_node("Player") as Node3D
	_player.camera_can_process = false
	_player.camera_can_automatic_pan = false

	_city_world = _game.city_world
	_hud = _game.hud
	_mode_layer = _city_world._mode_layer
	_camera = _player.camera

	_hud.set_map_mode("resources")
	await _wait_frames(2)

	var chaillac: Array = _mode_layer._mineral_points[100]
	var chaillac_world: Vector3 = _mode_layer.to_global(_mode_layer._marker_world(chaillac))
	var default_pos: Vector3 = _player.global_position

	_log("=== deposit_pick_test (determinístico) ===")
	_log("mineral_points = %d" % _mode_layer._mineral_points.size())
	for idx in FOCUS_IDX:
		_log("idx %d = %s" % [idx, _mode_layer._mineral_points[idx]])
	_log("câmera padrão = %s ; chaillac_world = %s" % [default_pos, chaillac_world])

	for framing_name in ["padrão", "centrada_chaillac"]:
		# Reposiciona a câmera para cada moldura.
		_player.global_position = default_pos if framing_name == "padrão" \
				else Vector3(chaillac_world.x, 0.0, chaillac_world.z)
		await _wait_frames(2)
		for size in WINDOW_SIZES:
			await _measure_window(framing_name, size)

	DirAccess.make_dir_recursive_absolute("res://docs/debug")
	var f := FileAccess.open(REPORT_PATH, FileAccess.WRITE)
	if f != null:
		f.store_string("\n".join(_report) + "\n")
		f.close()
	_log("relatório salvo em " + REPORT_PATH)

	_game.queue_free()
	await process_frame
	quit(0)


func _measure_window(framing: String, size: Vector2i) -> void:
	root.size = size
	await _wait_frames(4)

	var vp: Viewport = root
	var base_size: Vector2 = vp.get_visible_rect().size
	var window_size: Vector2 = Vector2(root.size)

	_log("")
	_log("--- [%s] janela %dx%d (base %.0fx%.0f) ---"
			% [framing, window_size.x, window_size.y, base_size.x, base_size.y])

	var total_visible := 0
	var self_fails := 0
	var stop_blocks := 0
	var pass_blocks := 0
	var stop_by_node := {}
	var focus_lines := {}

	for idx in _mode_layer._mineral_points.size():
		var point: Array = _mode_layer._mineral_points[idx]
		var world: Vector3 = _mode_layer.to_global(_mode_layer._marker_world(point))
		if _camera.is_position_behind(world):
			continue
		var c := _camera.unproject_position(world)
		if c.x < 0.0 or c.y < 0.0 or c.x > base_size.x or c.y > base_size.y:
			continue
		total_visible += 1

		var r_self: int = _city_world.pick_deposit(c, _camera)
		if r_self != idx:
			self_fails += 1

		# Bloqueio de GUI: quais controls visíveis (mouse_filter != IGNORE)
		# cobrem o ponto.
		var stops: Array = []
		var passes: Array = []
		_collect_covering(_game, c, stops, passes)
		if not stops.is_empty():
			stop_blocks += 1
			for node in stops:
				var key := String(node.get_path())
				stop_by_node[key] = int(stop_by_node.get(key, 0)) + 1
		if not passes.is_empty():
			pass_blocks += 1

		if idx in FOCUS_IDX:
			focus_lines[idx] = ("    idx %d %-22s c=(%.0f,%.0f) r_self=%d stop=%s pass=%s" % [
				idx, String(point[7]), c.x, c.y, r_self,
				(_names(stops)), (_names(passes)),
			])

	_log("  visíveis=%d  falhas_autocoerência=%d  bloqueados_por_STOP=%d  cobertos_por_PASS=%d"
			% [total_visible, self_fails, stop_blocks, pass_blocks])
	for idx in FOCUS_IDX:
		if focus_lines.has(idx):
			_log(focus_lines[idx])
	if not stop_by_node.is_empty():
		_log("  nós STOP que cobrem depósitos:")
		for key in stop_by_node:
			_log("    %s ×%d" % [key, int(stop_by_node[key])])


## Coleta controls visíveis (mouse_filter != IGNORE) que cobrem `point`.
func _collect_covering(node: Node, point: Vector2, stops: Array, passes: Array) -> void:
	for child in node.get_children():
		if child is Control:
			var c := child as Control
			if c.is_visible_in_tree() and c.mouse_filter != Control.MOUSE_FILTER_IGNORE \
					and c.get_global_rect().has_point(point):
				if c.mouse_filter == Control.MOUSE_FILTER_STOP:
					stops.append(c)
				else:
					passes.append(c)
		_collect_covering(child, point, stops, passes)


func _names(nodes: Array) -> String:
	if nodes.is_empty():
		return "—"
	var parts: PackedStringArray = []
	for n in nodes:
		parts.append(String(n.get_path()))
	return "[" + "; ".join(parts) + "]"
