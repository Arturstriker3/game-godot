extends SceneTree

## Cursor de mão no hover do mapa (cidade, porto e jazida) e seta fora.
##
## Simula `InputEventMouseMotion` com `Input.parse_input_event` (o mesmo caminho
## do mouse real) e confere o shape pretendido pelo `GameCursor` nos modos
## político e recursos. No `--headless` o DisplayServer não atualiza o cursor
## nativo: `Input.get_current_cursor_shape()` fica 0. O teste compara o shape
## interno (fonte da intenção) e, quando o servidor rastreia o cursor, também o
## valor nativo — sem fingir que o headless respondeu.
##
##   godot --headless --path . --script res://tools/cursor_hover_test.gd

const MAIN_SCENE := "res://game/main_game.tscn"
const GameCursorScript: Script = preload("res://core/ui/game_cursor.gd")
## Ponto de oceano em px do mapa (5000×2500): Atlântico equatorial, a ~1.700 km
## de qualquer costa - nem cidade, nem porto, nem jazida. A câmera é
## recentralizada nele antes de checar a seta.
const EMPTY_MAP_PX := Vector2(2080.0, 980.0)

var _failures: Array[String] = []
var _game: Node3D
var _hud
var _world: Node3D
var _camera: Camera3D
var _native_checked := false


func _init() -> void:
	_run.call_deferred()


func _run() -> void:
	root.size = Vector2i(1920, 1080)
	_game = (load(MAIN_SCENE) as PackedScene).instantiate()
	root.add_child(_game)
	for _i in 6:
		await process_frame

	_hud = _game.hud
	_world = _game.city_world
	var player := _game.get_node("Player") as Node3D
	player.camera_can_process = false
	player.camera_can_automatic_pan = false
	_camera = player.camera

	print("=== cursor_hover_test ===")
	print("display=", DisplayServer.get_name(),
			" native_cursor=", Input.get_current_cursor_shape(),
			" tracks_native=", GameCursorScript.tracks_native_shape())

	await _test_city_hover()
	await _test_deposit_hover()
	await _test_port_hover()
	await _test_empty_returns_arrow()

	_game.queue_free()
	await process_frame
	if _failures.is_empty():
		print("PASS: mão em cidade/porto/jazida e seta fora (político e recursos).")
		quit(0)
		return
	for failure in _failures:
		push_error("[cursor] " + failure)
	quit(1)


## Move o mouse de verdade (evento + posição do viewport) e espera o hover.
func _move_mouse(screen: Vector2) -> void:
	var motion := InputEventMouseMotion.new()
	motion.position = screen
	motion.global_position = screen
	Input.parse_input_event(motion)
	for _i in 3:
		await process_frame


func _expect(tag: String, expected: int) -> void:
	var actual: int = GameCursorScript.current_shape()
	if actual != expected:
		_failures.append("%s: shape %d != %d" % [tag, actual, expected])
	if GameCursorScript.tracks_native_shape() and not _native_checked:
		_native_checked = true
		var native := Input.get_current_cursor_shape()
		if native != expected:
			_failures.append("%s: cursor nativo %d != %d" % [tag, native, expected])


func _focus_on(local_world: Vector3) -> void:
	_world.position = Vector3.ZERO
	_world.position = -local_world
	for _i in 24:
		await process_frame


## Posição local do porto (sem o offset do mundo) para centralizar a câmera.
func _port_local(port_id: String) -> Vector3:
	for record in _world._port_nodes:
		if String(record.get("id", "")) == port_id:
			var position: Vector3 = record.get("position", Vector3.ZERO)
			return position + Vector3(_world._wrap_offset(position.x), 0.0, 0.0)
	return Vector3.ZERO


func _city_screen(city_id: String) -> Vector2:
	return _camera.unproject_position(_world.city_world_position(city_id))


func _test_city_hover() -> void:
	_hud.set_map_mode("political")
	_world.position = Vector3.ZERO
	await _focus_on(_world.city_world_position("GBR-LDN"))
	var screen := _city_screen("GBR-LDN")
	await _move_mouse(screen)
	if root.gui_get_hovered_control() != null:
		_failures.append("cidade político: GUI sob o mouse (%s)"
				% root.gui_get_hovered_control())
	if _world.pick_city(screen, _camera) != "GBR-LDN":
		_failures.append("cidade político: GBR-LDN não é pickável")
	_expect("cidade político", Input.CURSOR_POINTING_HAND)

	_hud.set_map_mode("resources")
	for _i in 4:
		await process_frame
	await _move_mouse(screen)
	if _world.pick_city(screen, _camera) != "GBR-LDN":
		_failures.append("cidade recursos: GBR-LDN não é pickável")
	_expect("cidade recursos", Input.CURSOR_POINTING_HAND)


func _test_deposit_hover() -> void:
	var layer = _world._mode_layer
	if layer == null or layer._total_pickable() <= 0:
		_failures.append("jazida: nenhum depósito no mapa de recursos")
		return
	_hud.set_map_mode("resources")
	# Primeiro depósito mineral que esteja na frente da câmera e fora da UI.
	var chosen := Vector3.INF
	var index := -1
	for i in layer._mineral_points.size():
		var local: Vector3 = layer._marker_world(layer._mineral_points[i])
		_world.position = Vector3.ZERO
		_world.position = -local
		for _f in 4:
			await process_frame
		var screen := _camera.unproject_position(_world.to_global(local))
		if root.gui_get_hovered_control() != null:
			continue
		if _world.pick_deposit(screen, _camera) != i:
			continue
		chosen = local
		index = i
		break
	if index < 0 or chosen == Vector3.INF:
		_failures.append("jazida: não achei depósito pickável fora da UI")
		return
	await _move_mouse(_camera.unproject_position(_world.to_global(chosen)))
	_expect("jazida recursos", Input.CURSOR_POINTING_HAND)


func _test_port_hover() -> void:
	_hud.set_map_mode("political")
	for _i in 4:
		await process_frame
	var port_id := ""
	var local := Vector3.ZERO
	for record in _world._port_nodes:
		var candidate := String(record.get("id", ""))
		if candidate.is_empty():
			continue
		var position := _port_local(candidate)
		await _focus_on(position)
		var screen := _camera.unproject_position(_world.to_global(position))
		if root.gui_get_hovered_control() != null:
			continue
		var picked: String = _world.pick_port(screen, _camera)
		if picked.is_empty():
			continue
		port_id = picked
		local = _port_local(picked)
		break
	if port_id.is_empty():
		_failures.append("porto: não achei porto pickável no nível PERTO")
		return
	await _focus_on(local)
	await _move_mouse(_camera.unproject_position(_world.to_global(local)))
	_expect("porto político", Input.CURSOR_POINTING_HAND)


func _test_empty_returns_arrow() -> void:
	for mode in ["political", "resources"]:
		_hud.set_map_mode(mode)
		# Recentraliza a câmera num trecho de oceano ANTES de olhar a seta: o
		# teste do porto deixa a câmera em cima dele, e o "ponto vazio" herdado
		# (centro da tela) seria o próprio porto recém-checado.
		await _focus_on(_world.pixel_to_world(EMPTY_MAP_PX))
		for _i in 4:
			await process_frame
		var screen := _find_empty_screen(mode)
		if screen == Vector2.INF:
			_failures.append("%s: nenhum ponto vazio na tela" % mode)
			continue
		await _move_mouse(screen)
		if root.gui_get_hovered_control() != null:
			_failures.append("%s: GUI sob o mouse no ponto vazio (%s)"
					% [mode, root.gui_get_hovered_control()])
		if _world.pick_city(screen, _camera) != "" \
				or _world.pick_port(screen, _camera) != "" \
				or (mode == "resources" \
				and _world.pick_deposit(screen, _camera) >= 0):
			_failures.append("%s: ponto vazio não está vazio" % mode)
		_expect("fora %s" % mode, Input.CURSOR_ARROW)


## Primeiro ponto da tela sem cidade/porto/jazida (nunca dentro das bordas da
## UI). O centro vem primeiro - é onde o oceano recentralizado cai; a varredura
## em anel cobre o caso de uma ilha aparecer no meio.
func _find_empty_screen(mode: String) -> Vector2:
	var viewport_size := Vector2(root.size)
	var center := viewport_size * 0.5
	var candidates: Array[Vector2] = [center]
	for radius in [160.0, 320.0, 480.0]:
		for step in 8:
			var angle := TAU * float(step) / 8.0
			candidates.append(center + Vector2(cos(angle), sin(angle)) * radius)
	for point in candidates:
		var rect := Rect2(Vector2.ZERO, viewport_size).grow(-96.0)
		if not rect.has_point(point):
			continue
		if _world.pick_city(point, _camera) != "" \
				or _world.pick_port(point, _camera) != "":
			continue
		if mode == "resources" and _world.pick_deposit(point, _camera) >= 0:
			continue
		return point
	return Vector2.INF
