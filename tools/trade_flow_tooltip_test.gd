extends SceneTree

## Tooltip das setas de comércio no modo rotas.
##
## Cobre o caminho REAL do `_process` do jogo (`_update_city_hover_at` +
## `_update_tooltip`), não uma chamada direta ao HUD: é esse caminho que o t20
## quebrou, porque a ficha de cidade e a de fluxo se apagavam no mesmo frame.
## Confere atraso de ~250 ms, conteúdo (origem → destino, produto, volume),
## `clear_city_tooltip` sem efeito sobre o fluxo e saída da seta escondendo.
##
##   godot --headless --path . --script res://tools/trade_flow_tooltip_test.gd

const MAIN_SCENE := "res://game/main_game.tscn"
const DELAY_WAIT := 0.35
## Área longe da barra superior (64 px) e da borda inferior; os Controls que
## cobrem o mapa (dock, minimapa) são descartados pelo teste de GUI do hover.
const SAFE_RECT := Rect2(60.0, 80.0, 1790.0, 900.0)

var _failures: Array[String] = []
var _game: Node3D
var _world: Node3D
var _hud
var _camera: Camera3D
var _i18n: Node


func _init() -> void:
	_run.call_deferred()


func _run() -> void:
	root.size = Vector2i(1920, 1080)
	_i18n = root.get_node("I18n")
	_game = (load(MAIN_SCENE) as PackedScene).instantiate()
	root.add_child(_game)
	for _i in 8:
		await process_frame
	_world = _game.city_world
	_hud = _game.hud
	var player := _game.get_node("Player") as Node3D
	player.camera_can_process = false
	player.camera_can_automatic_pan = false
	_camera = player.camera
	var economy := root.get_node("Economy")
	economy.set_speed(0)
	for _day in 7:
		economy._run_day()
	_hud.set_map_mode("routes")
	for _i in 40:
		await process_frame
	# Daqui em diante quem dirige o hover é o teste (o _process do jogo faria
	# o mesmo a cada frame, com a posição real do mouse).
	_game.set_process(false)
	print("=== trade_flow_tooltip_test ===")
	var flows: int = _world.trade_flow_count()
	print("flows=", flows)
	if flows <= 0:
		_failures.append("modo rotas sem fluxos desenhados")
		_finish()
		return

	var target: Dictionary = await _find_flow_point()
	if target.is_empty():
		_failures.append("nenhuma seta com ponto livre de cidade/porto")
		_finish()
		return
	var index := int(target["index"])
	var point: Vector2 = target["point"]
	_move_mouse(point)
	await process_frame
	_game.call("_update_city_hover_at", point)
	if int(_game.get("_last_flow")) != index:
		_failures.append("hover pass não pegou a seta %d (veio %s)"
				% [index, str(_game.get("_last_flow"))])
		_finish()
		return

	# 1. Antes do atraso, nada aparece.
	_game.call("_update_tooltip")
	if _hud.call("city_tooltip_visible"):
		_failures.append("tooltip apareceu antes dos ~250 ms")
	# 2. Depois do atraso, a ficha do fluxo aparece.
	await create_timer(DELAY_WAIT).timeout
	_game.call("_update_tooltip")
	if not _hud.call("city_tooltip_visible"):
		_failures.append("tooltip da seta não apareceu depois do atraso")
		_finish()
		return
	var info: Dictionary = _world.trade_flow_info(index)
	var texts := _tooltip_texts()
	_expect(texts, _t("map.routes.product"), "produto")
	_expect(texts, _t("map.routes.volume"), "volume")
	_expect(texts, _t("product." + String(info.get("good", ""))), "bem")
	_expect(texts, _t("map.routes.sea") if String(info.get("mode", "")) == "sea"
			else _t("map.routes.land"), "modal")

	# 3. O update da ficha de cidade não pode apagar a ficha do fluxo.
	_hud.call("clear_city_tooltip")
	_game.call("_update_tooltip")
	if not _hud.call("city_tooltip_visible"):
		_failures.append("clear_city_tooltip apagou a ficha do fluxo")

	# 4. Sair da seta esconde (o clear do fluxo, e só ele, fecha a ficha).
	var empty := _find_empty_point()
	_move_mouse(empty)
	await process_frame
	_game.call("_update_city_hover_at", empty)
	_game.call("_update_tooltip")
	if _hud.call("city_tooltip_visible"):
		_failures.append("tooltip continuou visível fora da seta")

	# 5. A ficha de cidade continua funcionando no mesmo dono.
	var city := await _find_city_point()
	if city.is_empty():
		print("aviso: nenhuma cidade sob o mouse; ficha de cidade não conferida")
	else:
		var city_point: Vector2 = city["point"]
		var city_id := String(city["id"])
		_move_mouse(city_point)
		await process_frame
		_game.call("_update_city_hover_at", city_point)
		_game.call("_update_tooltip")
		await create_timer(DELAY_WAIT).timeout
		_game.call("_update_tooltip")
		if not _hud.call("city_tooltip_visible"):
			_failures.append("ficha de cidade não apareceu depois da seta")
		elif not " | ".join(_tooltip_texts()).contains(
				String(_i18n.call("city_name", city_id, city_id))):
			_failures.append("ficha de cidade sem o nome de %s" % city_id)
	_finish()


## Primeiro ponto de uma seta que não esteja sobre cidade nem porto, cuja
## projeção cai na área segura e onde nenhum Control do HUD captura o mouse
## (o dock/minimapa ficam nas bordas).
func _find_flow_point() -> Dictionary:
	var tried := 0
	var stats := {"rect": 0, "free": 0, "pick": 0}
	var count: int = _world.trade_flow_count()
	for index in count:
		var info: Dictionary = _world.trade_flow_info(index)
		var points: PackedVector3Array = info.get("points", PackedVector3Array())
		if points.size() < 3:
			continue
		for point_index in range(1, points.size() - 1):
			var screen: Vector2 = _camera.unproject_position(
					_world.to_global(points[point_index]))
			if not SAFE_RECT.has_point(screen):
				continue
			stats["rect"] += 1
			if not _world.pick_city(screen, _camera).is_empty():
				continue
			if not _world.pick_port(screen, _camera).is_empty():
				continue
			stats["free"] += 1
			if _world.pick_trade_flow(screen, _camera) != index:
				continue
			stats["pick"] += 1
			_move_mouse(screen)
			await process_frame
			if root.gui_get_hovered_control() != null:
				continue
			return {"index": index, "point": screen}
		tried += 1
		if tried > 40:
			break
	print("debug candidatos retangulo=", stats["rect"], " livres=", stats["free"],
			" seta=", stats["pick"])
	return {}


## Ponto seguro sem seta, cidade ou porto (saída do hover).
func _find_empty_point() -> Vector2:
	for y in range(140, 820, 40):
		for x in range(140, 1480, 40):
			var screen := Vector2(float(x), float(y))
			if _world.pick_trade_flow(screen, _camera) >= 0:
				continue
			if not _world.pick_city(screen, _camera).is_empty():
				continue
			if not _world.pick_port(screen, _camera).is_empty():
				continue
			return screen
	return SAFE_RECT.position + Vector2(12.0, 12.0)


## Cidade visível com ponto de tela na área segura, sem porto no mesmo ponto e
## onde nenhum Control do HUD captura o mouse (a barra lateral de painéis cobre
## a faixa esquerda do mapa e o hover ali é do painel, não do mapa).
func _find_city_point() -> Dictionary:
	for record in _world.get("_city_nodes"):
		if not (record is Dictionary):
			continue
		var id := String(record.get("id", ""))
		if id.is_empty():
			continue
		var screen: Vector2 = _camera.unproject_position(
				_world.city_world_position(id))
		if not SAFE_RECT.has_point(screen):
			continue
		if _world.pick_city(screen, _camera) != id:
			continue
		if not _world.pick_port(screen, _camera).is_empty():
			continue
		_move_mouse(screen)
		await process_frame
		if root.gui_get_hovered_control() != null:
			continue
		return {"id": id, "point": screen}
	return {}


func _move_mouse(screen: Vector2) -> void:
	var motion := InputEventMouseMotion.new()
	motion.position = screen
	motion.global_position = screen
	Input.parse_input_event(motion)


func _tooltip_texts() -> Array[String]:
	var out: Array[String] = []
	var pending: Array[Node] = [_hud.get("_city_tooltip") as Node]
	while not pending.is_empty():
		var node: Node = pending.pop_back()
		if node is Label and not (node as Label).text.is_empty():
			out.append((node as Label).text)
		for child in node.get_children():
			pending.append(child)
	return out


func _t(key: String) -> String:
	return String(_i18n.call("t", key))


func _expect(texts: Array[String], needle: String, what: String) -> void:
	if needle.is_empty() or not texts.has(needle):
		_failures.append("tooltip sem %s ('%s'): %s"
				% [what, needle, " | ".join(texts)])


func _finish() -> void:
	if _game != null:
		_game.queue_free()
	if _failures.is_empty():
		print("PASS: tooltip da seta (atraso, conteúdo, clear de cidade, saída e cidade)")
		quit(0)
		return
	for failure in _failures:
		push_error("[trade_flow_tooltip] " + failure)
	quit(1)
