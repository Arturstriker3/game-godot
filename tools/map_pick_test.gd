extends SceneTree

## Verifica o raio ÚNICO de picking em tela e a desseleção no mapa.
##
## Raio: cidade, porto/construção e jazida usam o MESMO piso de tela
## (`MapPick.MIN_RADIUS_PX = 26 px`) e o mesmo cálculo por footprint projetado.
## Aqui medimos o raio efetivo de uma cidade e de um porto na MESMA distância de
## câmera e exigimos igualdade (±1 px); também testamos o acerto dentro do raio e
## o erro logo fora.
##
## Desseleção: clique repetido no mesmo item, clique em área vazia, Esc e
## fechamento do painel removem o anel — em cidade, porto/construção e jazida.
##
##   godot --headless --path . --script res://tools/map_pick_test.gd

const MAIN_SCENE := "res://game/main_game.tscn"
const MAX_DELTA_PX := 1.0
const CAMERA_OFFSET := Vector3(0.0, 50.0, 30.0)
## Canto superior esquerdo: era aqui que o rótulo de Caracas (projetado no plano
## da câmera) roubava o clique "vazio" e abria o painel.
const EMPTY_SCREEN := Vector2(4.0, 4.0)

var _failures: Array[String] = []
var _game: Node3D
var _camera: Camera3D
var _world: Node3D
var _hud


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

	print("=== map_pick_test ===")
	await _test_radius()
	await _test_port_toggle()
	await _test_city_toggle()
	await _test_deposit_toggle()
	await _test_empty_and_escape()

	_game.queue_free()
	await process_frame

	if _failures.is_empty():
		print("PASS: raio de pick uniforme (±%dpx) e desseleção em cidade, porto e jazida"
				% int(MAX_DELTA_PX))
		quit(0)
		return
	for failure in _failures:
		push_error("[map_pick] " + failure)
	quit(1)


# --- Raio único ---------------------------------------------------------------

func _test_radius() -> void:
	_close_overlay()
	_world.clear_selection()
	var city := await _find_city()
	var port := await _find_port()
	if city.is_empty() or port.is_empty():
		_failures.append("não achei cidade/porto clicável para medir o raio")
		return

	# Mesma distância de câmera para os dois: o raio em tela tem de coincidir.
	await _focus(_world.city_world_position(city))
	var city_radius: float = _world.place_pick_radius_px(city, _camera)
	await _focus(_world.port_world_position(port))
	var port_radius: float = _world.place_pick_radius_px(port, _camera)
	var delta := absf(city_radius - port_radius)
	print("cidade %-14s raio=%.1fpx | porto %-14s raio=%.1fpx | Δ=%.1fpx"
			% [city, city_radius, port, port_radius, delta])
	if delta > MAX_DELTA_PX:
		_failures.append("raio cidade/porto difere: %.1fpx vs %.1fpx (Δ%.1fpx)"
				% [city_radius, port_radius, delta])
	if city_radius < 25.0 or port_radius < 25.0:
		_failures.append("raio abaixo do piso de 26px: cidade=%.1f porto=%.1f"
				% [city_radius, port_radius])

	# Acerta dentro do raio e erra logo fora (mesma fonte para hover e clique).
	await _focus(_world.city_world_position(city))
	_check_hit("city", city, city_radius)
	await _focus(_world.port_world_position(port))
	_check_hit("port", port, port_radius)

	# O anel de hover tem raio fixo de 26 px e centro no ícone; o pick mantém
	# o próprio footprint (piso de 26 px).
	await _test_rings(city, port)


## O anel (hover e seleção) tem raio fixo de 26 px em tela e fica centrado no
## ícone do lugar, em qualquer zoom. Antes ele usava o footprint da cidade
## (1,8 u no fallback): no zoom próximo virava um anel de centenas de pixels,
## deslocado do símbolo desenhado.
func _test_rings(city: String, port: String) -> void:
	_world.clear_selection()
	var expected := MapPick.MIN_RADIUS_PX

	await _focus(_world.city_world_position(city))
	_world.set_hover(city)
	var city_hover: float = MapPick.icon_radius_px(_camera,
			_world._hover.global_position, _world._hover.scale.x)
	_check_ring_center("cidade hover", city, _world._hover.global_position)
	_world.set_hover("")
	_world.select_city(city)
	var city_select: float = MapPick.icon_radius_px(_camera,
			_world._selection.global_position, _world._selection.scale.x)
	_check_ring_center("cidade seleção", city, _world._selection.global_position)
	var city_pick: float = _world.place_pick_radius_px(city, _camera)
	_world.clear_selection()

	await _focus(_world.port_world_position(port))
	_world.set_hover(port)
	var port_hover: float = MapPick.icon_radius_px(_camera,
			_world._hover.global_position, _world._hover.scale.x)
	_check_ring_center("porto hover", port, _world._hover.global_position)
	_world.set_hover("")
	_world.select_port(port)
	var port_select: float = MapPick.icon_radius_px(_camera,
			_world._selection.global_position, _world._selection.scale.x)
	_check_ring_center("porto seleção", port, _world._selection.global_position)
	var port_pick: float = _world.place_pick_radius_px(port, _camera)
	_world.clear_selection()

	print(("anel cidade hover=%.1f seleção=%.1f pick=%.1f | "
			+ "porto hover=%.1f seleção=%.1f pick=%.1f")
			% [city_hover, city_select, city_pick,
			port_hover, port_select, port_pick])
	for pair in [["cidade hover", city_hover], ["cidade seleção", city_select],
			["porto hover", port_hover], ["porto seleção", port_select]]:
		if absf(float(pair[1]) - expected) > MAX_DELTA_PX:
			_failures.append("%s %.1fpx != %.0fpx"
					% [String(pair[0]), float(pair[1]), expected])
	if city_pick < expected - MAX_DELTA_PX \
			or port_pick < expected - MAX_DELTA_PX:
		_failures.append("pick abaixo do piso: cidade=%.1f porto=%.1f"
				% [city_pick, port_pick])


## O centro projetado do anel tem de coincidir com o ícone do lugar (±2 px).
func _check_ring_center(tag: String, place_id: String, ring_world: Vector3) -> void:
	var record: Dictionary = _world._records_by_id[place_id]
	var icon: Vector3
	if _world.place_kind(place_id) == "city":
		icon = _world._city_icon_world(record)
	else:
		# O ícone do porto é o `marker_world` (pode ser o badge ao lado da
		# cidade); não comparar com o ponto bruto de costa.
		icon = _world.to_global(record["marker_world"])
	var delta := _camera.unproject_position(ring_world).distance_to(
			_camera.unproject_position(icon))
	if delta > 2.0:
		_failures.append("%s %s: anel a %.1f px do ícone" % [tag, place_id, delta])


func _check_hit(kind: String, place_id: String, radius: float) -> void:
	var world_pos: Vector3 = _world.city_world_position(place_id) if kind == "city" \
			else _world.port_world_position(place_id)
	# O rótulo da cidade amplia a área clicável de propósito (clicar no nome
	# seleciona). Aqui medimos o raio do ÍCONE: escondemos o nome na medição.
	var label: Label3D = null
	var label_visible := false
	if kind == "city":
		for record in _world._city_nodes:
			if String(record["id"]) == place_id:
				label = record.get("label")
				break
		if label != null:
			label_visible = label.visible
			label.visible = false
	var center := _camera.unproject_position(world_pos)
	var inside := _find_inside_point(kind, place_id, center, radius)
	var outside := _find_empty_outside_point(center, radius)
	var hit_inside := _pick(kind, inside)
	var hit_outside := _pick(kind, outside) if outside != Vector2.INF else ""
	if label != null:
		label.visible = label_visible
	if hit_inside != place_id:
		print("DEBUG inside kind=%s id=%s center=%s radius=%.1f got='%s'"
				% [kind, place_id, center, radius, hit_inside])
		print("DEBUG cam=%s target=%s viewport=%s"
				% [_camera.global_position, world_pos, root.size])
		var right := _camera.global_transform.basis.x
		for record in _world._city_nodes:
			if String(record["id"]) == hit_inside:
				var lbl: Label3D = record.get("label")
				var lw: Vector3 = _world.to_global(lbl.position)
				var em := float(lbl.font_size) * lbl.pixel_size
				var tw := lbl.font.get_string_size(lbl.text, HORIZONTAL_ALIGNMENT_LEFT,
						-1, lbl.font_size).x * lbl.pixel_size
				var half := right * (tw * 0.5 + em * 1.6)
				var a := _camera.unproject_position(lw - half)
				var b := _camera.unproject_position(lw + half)
				var top := _camera.unproject_position(lw + Vector3.UP * em * 0.7)
				print("DEBUG winner label_world=%s a=%s b=%s top=%s text='%s' em=%.3f tw=%.3f r=%.1f d=%.1f"
						% [lw, a, b, top, lbl.text, em, tw, a.distance_to(top),
						_world._segment_distance(inside, a, b)])
		_failures.append("%s %s não respondeu a %.1fpx do centro"
				% [kind, place_id, radius - 1.0])
	if outside == Vector2.INF:
		_failures.append("%s %s: não achei ponto vazio fora do raio"
				% [kind, place_id])
	elif hit_outside == place_id:
		_failures.append("%s %s respondeu a %.1fpx do centro (fora do raio)"
				% [kind, place_id, center.distance_to(outside)])


func _pick(kind: String, screen_position: Vector2) -> String:
	if kind == "city":
		return _world.pick_city(screen_position, _camera)
	return _world.pick_port(screen_position, _camera)


## Ponto logo dentro do raio onde o próprio lugar vence o pick. Outro rótulo
## pode cobrir o lado esquerdo (Le Havre ao lado de Paris), então varremos as
## direções em vez de fixar uma.
func _find_inside_point(kind: String, place_id: String, center: Vector2,
		radius: float) -> Vector2:
	var distance := maxf(radius - 1.0, 0.0)
	for step in 24:
		var angle := TAU * float(step) / 24.0
		var point := center + Vector2(cos(angle), sin(angle)) * distance
		if _pick(kind, point) == place_id:
			return point
	return center


## Ponto fora do raio onde NADA é clicável (nem cidade, nem porto). Testar logo
## ao lado podia acertar o rótulo de outra cidade, que é uma área clicável real.
func _find_empty_outside_point(center: Vector2, radius: float) -> Vector2:
	var viewport := Rect2(Vector2.ZERO, Vector2(root.size))
	for extra in [4.0, 8.0, 16.0, 24.0, 40.0]:
		var distance := radius + float(extra)
		for step in 24:
			var angle := TAU * float(step) / 24.0
			var point := center + Vector2(cos(angle), sin(angle)) * distance
			if not viewport.grow(-2.0).has_point(point):
				continue
			if _world.pick_city(point, _camera).is_empty() \
					and _world.pick_port(point, _camera).is_empty():
				return point
	return Vector2.INF


# --- Desseleção ---------------------------------------------------------------

func _test_port_toggle() -> void:
	_close_overlay()
	var port := await _find_port()
	if port.is_empty():
		return
	await _focus(_world.port_world_position(port))
	var center := _camera.unproject_position(_world.port_world_position(port))
	_game._handle_map_click(center)
	if _world.selected_place_id() != port:
		_failures.append("clique não selecionou o porto %s" % port)
		return
	if _hud.is_overlay_open():
		_failures.append("porto abriu overlay indevido")
		_close_overlay()
	# Clicar em área vazia também desseleciona o porto.
	var empty_point := _find_empty_screen_point()
	if empty_point == Vector2.INF:
		_failures.append("não achei ponto vazio para desselecionar o porto")
	else:
		_game._handle_map_click(empty_point)
		if _world.has_selection():
			_failures.append("clique vazio não desselecionou o porto %s" % port)
	# Clique repetido no mesmo porto.
	_game._handle_map_click(center)
	_game._handle_map_click(center)
	if not _world.selected_place_id().is_empty():
		_failures.append("clique repetido não desselecionou o porto %s" % port)
	_close_overlay()


func _test_city_toggle() -> void:
	_close_overlay()
	var city := await _find_city()
	if city.is_empty():
		return
	await _focus(_world.city_world_position(city))
	var center := _camera.unproject_position(_world.city_world_position(city))
	_game._handle_map_click(center)
	if _world.selected_place_id() != city:
		_failures.append("clique não selecionou a cidade %s" % city)
		return
	if not _hud.is_window_open():
		_failures.append("painel da cidade não abriu com o clique")
	_hud.close_window()
	if not _world.selected_place_id().is_empty():
		_failures.append("fechar o painel não desselecionou a cidade %s" % city)
	if _hud.is_overlay_open():
		_failures.append("fechar o painel deixou overlay aberto")
	# Clique repetido: seleciona de novo e clica no mesmo ponto.
	_world.select_city(city)
	_game._handle_map_click(center)
	if not _world.selected_place_id().is_empty():
		_failures.append("clique repetido não desselecionou a cidade %s" % city)
	if _hud.is_overlay_open():
		_failures.append("clique repetido na cidade deixou painel aberto")
	_close_overlay()


func _test_deposit_toggle() -> void:
	_close_overlay()
	_hud.set_map_mode("resources")
	await process_frame
	var layer = _world._mode_layer
	for index in mini(80, layer._mineral_points.size()):
		var point: Array = layer._mineral_points[index]
		var key := String(layer.MINERAL_KEYS[clampi(int(point[2]), 0,
				layer.MINERAL_KEYS.size() - 1)])
		if not layer._is_enabled(key):
			continue
		var world_pos: Vector3 = layer.to_global(layer._marker_world(point))
		await _focus(world_pos)
		var center := _camera.unproject_position(world_pos)
		if _world.pick_deposit(center, _camera) != index:
			continue
		_game._handle_map_click(center)
		if _world.selected_deposit() != index:
			_failures.append("clique não selecionou a jazida %d" % index)
			break
		# Clique em área vazia limpa o anel da jazida.
		var empty_point := _find_empty_screen_point()
		if empty_point == Vector2.INF:
			_failures.append("não achei ponto vazio para desselecionar a jazida")
		else:
			_game._handle_map_click(empty_point)
			if _world.selected_deposit() != -1:
				_failures.append("clique vazio não desselecionou a jazida %d" % index)
		# Clique repetido no mesmo ponto.
		_game._handle_map_click(center)
		_game._handle_map_click(center)
		if _world.selected_deposit() != -1:
			_failures.append("clique repetido não desselecionou a jazida %d" % index)
		break
	_hud.set_map_mode("political")
	await process_frame
	_close_overlay()


func _test_empty_and_escape() -> void:
	_close_overlay()
	# Clique em área vazia.
	var city := await _find_city()
	if not city.is_empty():
		_world.select_city(city)
		var empty_point := _find_empty_screen_point()
		if empty_point == Vector2.INF:
			_failures.append("não achei ponto vazio na tela para o clique")
		else:
			_game._handle_map_click(empty_point)
			if _world.has_selection():
				_failures.append("clique vazio não desselecionou a cidade %s" % city)
			if _hud.is_overlay_open():
				_failures.append("clique vazio abriu painel (ponto %s)" % empty_point)
		_close_overlay()

	# Esc em porto/construção: consome o evento, limpa a seleção e NÃO abre menu.
	_close_overlay()
	var port := await _find_port()
	if not port.is_empty():
		_world.select_port(port)
		if _hud.is_overlay_open():
			_failures.append("overlay aberto antes do Esc no porto")
			_close_overlay()
		await _press_escape()
		if _world.has_selection():
			_failures.append("Esc não desselecionou o porto %s" % port)
		if _hud._menu_layer.visible:
			_failures.append("Esc com porto selecionado abriu o menu de pausa")
		_close_overlay()

	# Esc em cidade: idem.
	if not city.is_empty():
		_world.select_city(city)
		if _hud.is_overlay_open():
			_failures.append("overlay aberto antes do Esc na cidade")
			_close_overlay()
		await _press_escape()
		if _world.has_selection():
			_failures.append("Esc não desselecionou a cidade %s" % city)
		if _hud._menu_layer.visible:
			_failures.append("Esc com cidade selecionada abriu o menu de pausa")
		_close_overlay()

	# Esc com o painel da cidade aberto: fecha o painel, não abre o menu.
	if not city.is_empty():
		await _focus(_world.city_world_position(city))
		var city_center := _camera.unproject_position(_world.city_world_position(city))
		_game._handle_map_click(city_center)
		if not _hud.is_window_open():
			_failures.append("painel da cidade não abriu para o teste de Esc")
		else:
			await _press_escape()
			if _hud.is_window_open():
				_failures.append("Esc não fechou o painel da cidade")
			if _world.has_selection():
				_failures.append("fechar o painel com Esc não desselecionou a cidade")
			if _hud._menu_layer.visible:
				_failures.append("Esc com painel aberto abriu o menu de pausa")
		_close_overlay()

	# Esc em jazida.
	_hud.set_map_mode("resources")
	await process_frame
	_world.set_deposit_selected(0)
	await _press_escape()
	if _world.has_selection():
		_failures.append("Esc não desselecionou a jazida")
	if _hud._menu_layer.visible:
		_failures.append("Esc com jazida selecionada abriu o menu de pausa")
	_hud.set_map_mode("political")
	await process_frame
	_close_overlay()

	# Esc sem seleção nem janela: o mapa não consome; o HUD abre o menu.
	_world.clear_selection()
	_close_overlay()
	if _game._handle_escape():
		_failures.append("Esc sem seleção foi consumido pelo mapa")
	await _press_escape()
	if not _hud._menu_layer.visible:
		_failures.append("Esc sem seleção/janela não abriu o menu de pausa")
	_close_overlay()


# --- Apoio --------------------------------------------------------------------

## Deixa o HUD sem painel/menu/ajustes entre um caso e outro. Um overlay aberto
## faz o `_handle_escape()` do mapa devolver false e contamina os testes de Esc.
func _close_overlay() -> void:
	if _hud == null:
		return
	_hud.close_window()
	if _hud._menu_layer.visible:
		_hud.toggle_menu()
	if _hud._settings_layer != null and _hud._settings_layer.visible:
		_hud._close_settings()


## Primeiro ponto da tela sem cidade nem porto clicáveis. Prioriza o canto que
## expunha o bug do rótulo de Caracas.
func _find_empty_screen_point() -> Vector2:
	if _is_empty_screen(EMPTY_SCREEN):
		return EMPTY_SCREEN
	var size := Vector2(root.size)
	var y := 32.0
	while y < size.y:
		var x := 32.0
		while x < size.x:
			var point := Vector2(x, y)
			if _is_empty_screen(point):
				return point
			x += 32.0
		y += 32.0
	return Vector2.INF


func _is_empty_screen(point: Vector2) -> bool:
	if not _world.pick_city(point, _camera).is_empty():
		return false
	if not _world.pick_port(point, _camera).is_empty():
		return false
	if _hud.map_mode() == "resources" \
			and _world.pick_deposit(point, _camera) >= 0:
		return false
	return true


## Envia um Esc real pelo viewport: o mapa tem a primeira chance (`_input`) e,
## sem seleção, o HUD recebe em `_unhandled_input` e abre o menu.
func _press_escape() -> void:
	var event := InputEventAction.new()
	event.action = "ui_cancel"
	event.pressed = true
	root.push_input(event)
	await process_frame


## Primeira cidade cujo centro é clicável e que não tem porto por cima.
func _find_city() -> String:
	for record in _world._city_nodes:
		var id := String(record["id"])
		var world_pos: Vector3 = _world.to_global(record["position"])
		await _focus(world_pos)
		var center := _camera.unproject_position(world_pos)
		if _world.pick_city(center, _camera) == id \
				and _world.pick_port(center, _camera).is_empty():
			return id
	return ""


## Primeiro porto cujo centro é clicável (construção no mapa).
func _find_port() -> String:
	for record in _world._port_nodes:
		if bool(record.get("fused", false)):
			continue
		var id := String(record["id"])
		var world_pos: Vector3 = _world.to_global(record["position"])
		await _focus(world_pos)
		var center := _camera.unproject_position(world_pos)
		if _world.pick_port(center, _camera) == id:
			return id
	return ""


## Coloca a câmera sempre à MESMA distância do alvo.
func _focus(world_pos: Vector3) -> void:
	_camera.global_position = world_pos + CAMERA_OFFSET
	_camera.look_at(world_pos, Vector3.UP)
	await process_frame
