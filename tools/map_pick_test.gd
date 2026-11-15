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
## LONGE: só a capital tem marcador, então só ela responde a clique/hover; as
## demais cidades e os portos não são alvo nesse nível.
##
##   godot --headless --path . --script res://tools/map_pick_test.gd

const MAIN_SCENE := "res://game/main_game.tscn"
const MAX_DELTA_PX := 1.0
const CAMERA_OFFSET := Vector3(0.0, 50.0, 30.0)
## Mesmo enquadramento a > 178 u do alvo (nível LONGE do LOD).
const CAMERA_OFFSET_FAR := Vector3(0.0, 200.0, 120.0)
## Enquadramento a ~117 u do alvo (nível MÉDIO do LOD: 66..168 u).
const CAMERA_OFFSET_MID := Vector3(0.0, 100.0, 60.0)
## Espelha `CityWorld.ZoomLevel` (usar a classe aqui forçaria o compile de
## city_world.gd antes dos autoloads no modo --script).
const MID_LEVEL := 1
const FAR_LEVEL := 2
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
	await _test_far_capitals_only()
	await _test_mid_capitals_and_player()
	await _test_mode_switch_clears_selection()

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
	# F7: clicar no porto abre a Logística (antes o clique só selecionava e o
	# painel de portos ficava inalcançável pelo mapa). Com as telas de dados
	# desligadas (`main_game.SHOW_DATA_SCREENS = false`) o clique só seleciona.
	if _hud.data_screens_enabled:
		if not _hud.is_overlay_open():
			_failures.append("porto não abriu o painel de Logística")
		elif String(_hud._active) != "logistics":
			_failures.append("porto abriu '%s' em vez de Logística"
					% String(_hud._active))
		_hud.close_window()
		if not _world.selected_place_id().is_empty():
			_failures.append("fechar Logística não desselecionou o porto %s" % port)
	else:
		if _hud.is_overlay_open():
			_failures.append("telas de dados ocultas: o porto abriu sobreposição")
		_world.clear_selection()
	# Clicar em área vazia também desseleciona o porto.
	_game._handle_map_click(center)
	var empty_point := _find_empty_screen_point()
	if empty_point == Vector2.INF:
		_failures.append("não achei ponto vazio para desselecionar o porto")
	else:
		_game._handle_map_click(empty_point)
		if _world.has_selection():
			_failures.append("clique vazio não desselecionou o porto %s" % port)
	_hud.close_window()
	# Clique repetido no mesmo porto: seleciona (Logística) e desseleciona.
	_game._handle_map_click(center)
	_game._handle_map_click(center)
	if not _world.selected_place_id().is_empty():
		_failures.append("clique repetido não desselecionou o porto %s" % port)
	if _hud.is_overlay_open():
		_failures.append("clique repetido no porto deixou Logística aberta")
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
	if _hud.data_screens_enabled:
		if not _hud.is_window_open():
			_failures.append("painel da cidade não abriu com o clique")
		_hud.close_window()
		if not _world.selected_place_id().is_empty():
			_failures.append("fechar o painel não desselecionou a cidade %s" % city)
	else:
		# Ficha T3 desligada: o clique deixa só o realce/anel do mapa.
		if _hud.is_window_open():
			_failures.append("telas de dados ocultas: a cidade abriu a ficha T3")
		_world.clear_selection()
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
	# (Só existe com as telas de dados ligadas.)
	if not city.is_empty() and _hud.data_screens_enabled:
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


# --- LONGE: só capitais -------------------------------------------------------

## No nível LONGE o mapa desenha apenas capitais (estrela + selo): cidade comum
## e porto não podem responder a clique nem a hover. A capital continua
## clicável e com o anel de hover no ícone.
func _test_far_capitals_only() -> void:
	_close_overlay()
	_world.clear_selection()
	_world.set_hover("")
	var capital := await _find_far_city(true)
	var common := await _find_far_city(false)
	if capital.is_empty() or common.is_empty():
		_failures.append("LONGE: sem capital/cidade comum na tela para testar")
		return
	await _focus_far(_world.city_world_position(capital))
	var capital_center := _camera.unproject_position(
			_world.city_world_position(capital))
	var capital_hit: String = _world.pick_city(capital_center, _camera)
	var capital_radius: float = _world.place_pick_radius_px(capital, _camera)
	if capital_hit != capital or capital_radius <= 0.0:
		_failures.append("LONGE: capital %s não respondeu (hit='%s' raio=%.1f)"
				% [capital, capital_hit, capital_radius])
	_world.set_hover(capital)
	if _world._hover == null or not _world._hover.visible:
		_failures.append("LONGE: capital %s sem anel de hover" % capital)
	_world.set_hover("")

	await _focus_far(_world.city_world_position(common))
	var common_center := _camera.unproject_position(
			_world.city_world_position(common))
	var common_hit: String = _world.pick_city(common_center, _camera)
	var common_radius: float = _world.place_pick_radius_px(common, _camera)
	if common_hit == common:
		_failures.append("LONGE: cidade comum %s ainda é clicável" % common)
	if common_radius > 0.0:
		_failures.append("LONGE: cidade comum %s com raio de pick %.1f"
				% [common, common_radius])
	_world.set_hover(common)
	if _world._hover != null and _world._hover.visible:
		_failures.append("LONGE: cidade comum %s com anel de hover" % common)
	_world.set_hover("")

	var port := await _find_far_port()
	if not port.is_empty():
		await _focus_far(_world.port_world_position(port))
		var port_center := _camera.unproject_position(
				_world.port_world_position(port))
		if _world.pick_port(port_center, _camera) == port:
			_failures.append("LONGE: porto %s ainda é clicável" % port)
	print("LONGE: capital=%s clicável/hover | comum=%s e porto=%s sem alvo"
			% [capital, common, port])
	# Sem bolinha solta: no LONGE o único marcador é a estrela da capital.
	await _settle_icons()
	if _mm_visible(_world.get("_city_ring_mm")) != 0:
		_failures.append("LONGE: %d bolinhas de selo ainda desenhadas"
				% _mm_visible(_world.get("_city_ring_mm")))
	_world.set_hover("")
	_close_overlay()


## Cidade (capital ou comum) cujo centro foi para a tela no LONGE e cujo pick
## responde como esperado (capital acerta; comum não acerta ela mesma).
func _find_far_city(capital: bool) -> String:
	for record in _world._city_nodes:
		if bool(record.get("capital", false)) != capital:
			continue
		var id := String(record["id"])
		await _focus_far(_world.city_world_position(id))
		var center := _camera.unproject_position(_world.city_world_position(id))
		if not Rect2(Vector2.ZERO, Vector2(root.size)).grow(-4.0).has_point(center):
			continue
		var hit: String = _world.pick_city(center, _camera)
		if capital:
			if hit == id:
				return id
		elif hit != id:
			return id
	return ""


## Porto não fundido no tecido urbano que fica na tela no LONGE.
func _find_far_port() -> String:
	for record in _world._port_nodes:
		if bool(record.get("fused", false)):
			continue
		var id := String(record["id"])
		await _focus_far(_world.port_world_position(id))
		var center := _camera.unproject_position(_world.port_world_position(id))
		if Rect2(Vector2.ZERO, Vector2(root.size)).grow(-4.0).has_point(center):
			return id
	return ""


## Coloca a câmera no nível LONGE (> 178 u) do alvo e espera o LOD trocar de
## nível e o fade de 150 ms terminar (nome/bandeira ainda visíveis por um
## instante continuariam clicáveis e falseariam as asserções).
func _focus_far(world_pos: Vector3) -> void:
	_camera.global_position = world_pos + CAMERA_OFFSET_FAR
	_camera.look_at(world_pos, Vector3.UP)
	for _frame in 40:
		await process_frame
		var fading: Variant = _world.get("_level_fade_queue")
		if _world.zoom_level() == FAR_LEVEL and fading is Array \
				and fading.is_empty():
			break


# --- MÉDIO: capitais + nação do jogador ---------------------------------------

## No nível MÉDIO só capitais e cidades da nação do jogador têm marcador/nome —
## e são os únicos alvos de clique. As demais não respondem (era ali que as
## bolinhas apareciam), e o buffer do selo tem exatamente uma instância por
## glifo desenhado (sem bola solta).
func _test_mid_capitals_and_player() -> void:
	_close_overlay()
	_world.clear_selection()
	_world.set_player_nation("")
	_world.set_hover("")

	var capital := "BEL-BRU"
	var common := "BEL-ANR"
	if not _world._records_by_id.has(capital) \
			or not _world._records_by_id.has(common):
		capital = await _find_mid_city(true)
		common = await _find_mid_city(false)
	if capital.is_empty() or common.is_empty():
		_failures.append("MÉDIO: sem capital/cidade comum na tela para testar")
		return

	await _focus_mid(_world.city_world_position(capital))
	var capital_center := _camera.unproject_position(
			_world.city_world_position(capital))
	var capital_hit: String = _world.pick_city(capital_center, _camera)
	var capital_radius: float = _world.place_pick_radius_px(capital, _camera)
	if capital_hit != capital or capital_radius <= 0.0:
		_failures.append("MÉDIO: capital %s não respondeu (hit='%s' raio=%.1f)"
				% [capital, capital_hit, capital_radius])
	_world.set_hover(capital)
	if _world._hover == null or not _world._hover.visible:
		_failures.append("MÉDIO: capital %s sem anel de hover" % capital)
	_world.set_hover("")

	await _focus_mid(_world.city_world_position(common))
	var common_center := _camera.unproject_position(
			_world.city_world_position(common))
	var common_hit: String = _world.pick_city(common_center, _camera)
	var common_radius: float = _world.place_pick_radius_px(common, _camera)
	if common_hit == common:
		_failures.append("MÉDIO: cidade comum %s ainda é clicável" % common)
	if common_radius > 0.0:
		_failures.append("MÉDIO: cidade comum %s com raio de pick %.1f"
				% [common, common_radius])
	_world.set_hover(common)
	if _world._hover != null and _world._hover.visible:
		_failures.append("MÉDIO: cidade comum %s com anel de hover" % common)
	_world.set_hover("")

	_check_mid_visible_only("MÉDIO")
	_check_ring_matches_glyphs("MÉDIO")

	# Exceção: as cidades da nação do jogador voltam com ícone normal + nome.
	var record: Dictionary = _world._records_by_id[common]
	var nation := String(record.get("nation", ""))
	_world.set_player_nation(nation)
	await _settle_mid()
	if not bool(_world._city_marker_wanted(record)):
		_failures.append("MÉDIO: cidade %s da nação %s sem marcador"
				% [common, nation])
	if float(record.get("label_target", 0.0)) < 1.0:
		_failures.append("MÉDIO: cidade %s da nação %s sem alvo de nome"
				% [common, nation])
	var player_center := _camera.unproject_position(
			_world.city_world_position(common))
	var player_hit: String = _world.pick_city(player_center, _camera)
	var player_radius: float = _world.place_pick_radius_px(common, _camera)
	if player_hit != common or player_radius <= 0.0:
		_failures.append(
				"MÉDIO: cidade %s da nação do jogador não respondeu (hit='%s' raio=%.1f)"
				% [common, player_hit, player_radius])
	_world.set_hover(common)
	if _world._hover == null or not _world._hover.visible:
		_failures.append("MÉDIO: cidade %s do jogador sem anel de hover" % common)
	_world.set_hover("")
	_check_mid_visible_only("MÉDIO nação do jogador")
	_check_ring_matches_glyphs("MÉDIO nação do jogador")

	print("MÉDIO: capital=%s clicável/hover | comum=%s sem alvo | %s (%s) com ícone+nome"
			% [capital, common, common, nation])
	_world.set_player_nation("")
	_world.clear_selection()
	_world.set_hover("")
	_close_overlay()


## TROCA DE MODO: cidade selecionada no mapa de recursos não pode "roubar" o
## primeiro clique do mapa político. Ao voltar ao político a seleção/legenda do
## modo anterior é limpa e o PRIMEIRO clique na cidade abre a ficha T3.
func _test_mode_switch_clears_selection() -> void:
	_close_overlay()
	_world.clear_selection()
	_world.set_player_nation("")
	_hud.set_map_mode("resources")
	await process_frame

	var city := ""
	for record in _world._city_nodes:
		var id := String(record["id"])
		await _focus(_world.city_world_position(id))
		var center := _camera.unproject_position(_world.city_world_position(id))
		if _world.pick_city(center, _camera) != id:
			continue
		# Uma jazida sob a cidade teria prioridade no modo recursos; escolhe uma
		# cidade sem depósito para testar o caminho cidade → legenda Cidade.
		if _world.pick_deposit(center, _camera) >= 0:
			continue
		city = id
		break
	if city.is_empty():
		_failures.append("TROCA DE MODO: sem cidade sem depósito para testar")
		_hud.set_map_mode("political")
		return

	await _focus(_world.city_world_position(city))
	var center := _camera.unproject_position(_world.city_world_position(city))
	_game._handle_map_click(center)
	if _world.selected_place_id() != city:
		_failures.append("TROCA DE MODO: clique no modo recursos não selecionou %s"
				% city)
	# A legenda da cidade (dados de acesso a recursos) só existe com as telas de
	# dados ligadas; desligada, o clique mantém apenas o realce do mapa.
	var expected_legend := "city" if _hud.data_screens_enabled else "filters"
	if _hud._legend_state != expected_legend:
		_failures.append("TROCA DE MODO: legenda no estado %s (esperado %s)"
				% [_hud._legend_state, expected_legend])

	_hud.set_map_mode("political")
	await process_frame
	if _world.has_selection():
		_failures.append("TROCA DE MODO: seleção do modo recursos sobreviveu")
	if _hud._legend_state != "filters":
		_failures.append("TROCA DE MODO: legenda ficou no estado %s"
				% _hud._legend_state)
	if _hud._legend.visible or _hud._legend.mouse_filter != Control.MOUSE_FILTER_IGNORE:
		_failures.append("TROCA DE MODO: legenda oculta ainda captura input")

	# O primeiro clique no político tem de abrir a ficha, não só desselecionar.
	await _focus(_world.city_world_position(city))
	center = _camera.unproject_position(_world.city_world_position(city))
	_game._handle_map_click(center)
	if _world.selected_place_id() != city:
		_failures.append("TROCA DE MODO: 1º clique não selecionou %s" % city)
	if _hud.data_screens_enabled:
		if not _hud.is_window_open():
			_failures.append("TROCA DE MODO: 1º clique não abriu a ficha T3")
		_hud.close_window()
		_world.clear_selection()
		_hud.show_general_legend()
		print("TROCA DE MODO: %s selecionada em recursos → político abre a ficha no 1º clique"
				% city)
	else:
		if _hud.is_window_open():
			_failures.append("TROCA DE MODO: telas ocultas e a ficha T3 abriu")
		_world.clear_selection()
		_hud.show_general_legend()
		print("TROCA DE MODO: %s selecionada em recursos → político só realça (telas de dados ocultas)"
				% city)


## No MÉDIO, toda cidade na lista visível é capital ou pertence ao jogador.
func _check_mid_visible_only(tag: String) -> void:
	var player := String(_world.get("_player_nation"))
	for record in _world._visible_city_records:
		if bool(record.get("capital", false)):
			continue
		if player != "" and String(record.get("nation", "")) == player:
			continue
		_failures.append("%s: cidade %s com marcador no nível médio"
				% [tag, String(record.get("id", ""))])


## Sem bolinha solta: o buffer do selo tem uma instância por glifo desenhado.
func _check_ring_matches_glyphs(tag: String) -> void:
	var rings := _mm_visible(_world.get("_city_ring_mm"))
	var glyphs := _mm_visible(_world.get("_city_icon_mm")) \
			+ _mm_visible(_world.get("_city_star_mm"))
	if rings != glyphs:
		_failures.append("%s: %d selos para %d ícones (bolinha solta)"
				% [tag, rings, glyphs])


func _mm_visible(instance: Variant) -> int:
	if not (instance is MultiMeshInstance3D):
		return 0
	var mm: MultiMesh = (instance as MultiMeshInstance3D).multimesh
	return mm.visible_instance_count if mm != null else 0


## Cidade (capital ou comum) na tela no MÉDIO, com o pick coerente com o nível.
func _find_mid_city(capital: bool) -> String:
	for record in _world._city_nodes:
		if bool(record.get("capital", false)) != capital:
			continue
		var id := String(record["id"])
		await _focus_mid(_world.city_world_position(id))
		var center := _camera.unproject_position(_world.city_world_position(id))
		if not Rect2(Vector2.ZERO, Vector2(root.size)).grow(-4.0).has_point(center):
			continue
		var hit: String = _world.pick_city(center, _camera)
		if capital:
			if hit == id:
				return id
		elif hit != id:
			return id
	return ""


## Coloca a câmera no nível MÉDIO (~117 u) e espera nível, fade e buffers.
func _focus_mid(world_pos: Vector3) -> void:
	_camera.global_position = world_pos + CAMERA_OFFSET_MID
	_camera.look_at(world_pos, Vector3.UP)
	for _frame in 60:
		await process_frame
		var fading: Variant = _world.get("_level_fade_queue")
		if _world.zoom_level() == MID_LEVEL and fading is Array \
				and fading.is_empty():
			break
	await _settle_icons()


## Deixa o buffer de ícones do MultiMesh refletir o nível já assentado.
func _settle_icons() -> void:
	for _frame in 60:
		await process_frame
		if not bool(_world.get("_icons_dirty")):
			return


## Espera o layout fatiado do MÉDIO assentar depois de trocar a nação do jogador.
func _settle_mid() -> void:
	for _frame in 120:
		await process_frame
		var fading: Variant = _world.get("_level_fade_queue")
		if _world.zoom_level() == MID_LEVEL and fading is Array \
				and fading.is_empty() \
				and not bool(_world.get("_layout_job_active")) \
				and not bool(_world.get("_layout_dirty")) \
				and not bool(_world.get("_layout_pending")) \
				and not bool(_world.get("_layout_after_scan")):
			break
	await _settle_icons()


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
