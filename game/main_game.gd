extends Node3D

## Cena principal: monta o mundo de cidades (fundo tático + cidades + Voronoi),
## enquadra a câmera sobre a Europa, conecta o comércio aos comboios 3D e sobe o HUD.

const CityWorldScript: Script = preload("res://map/city_world.gd")
const ConvoysScript: Script = preload("res://map/convoy_spacing.gd")
const HudScript: Script = preload("res://core/ui/hud.gd")

## Inclinação da câmera (graus) e distância do alvo.
const CAMERA_PITCH_DEG := -56.0
const CAMERA_DISTANCE := 52.0

## Limite do centro da câmera na folha (latitudes). Em X o mundo dá a volta e
## as cópias laterais garantem que não há vazio; em Z o centro fica entre estas
## latitudes. Além delas há uma folga elástica: dá para empurrar até
## CAMERA_OVERSHOOT unidades e a mola devolve ao soltar.
const CAMERA_LAT_NORTH := 78.0
const CAMERA_LAT_SOUTH := -60.0
const CAMERA_OVERSHOOT := 24.0
const CAMERA_SPRING_TIME := 0.18

## Vinheta 2D sutil sobre o mapa (abaixo do HUD).
const VIGNETTE_ENABLED := true
const VIGNETTE_SHADER: Shader = preload("res://core/ui/screen_vignette.gdshader")
const VIGNETTE_LAYER := 40

var city_world
var convoys
var hud
var _last_hover := ""


func _ready() -> void:
	GameCursor.install()
	# Roda depois da câmera e do jogador: o limite vale para qualquer movimento
	# (teclado, borda da tela, arrasto, move_to).
	process_priority = 100
	_build_city_world()
	_build_convoys()
	_setup_camera()
	_build_vignette()
	_build_hud()
	_refresh_minimap_territory()
	Economy.trade_orders_ready.connect(convoys.spawn_orders)
	Economy.speed_changed.connect(_on_speed_changed)
	_on_speed_changed(Economy.speed)
	hud.resource_filter_changed.connect(city_world.set_resource_filter)
	Economy.status_updated.connect(_on_status_updated)
	city_world.territory_changed.connect(_on_territory_changed)


## Mantém os modos de mapa (população, economia, indústria, recursos) vivos.
func _on_status_updated(summary: Dictionary) -> void:
	city_world.refresh_modes(summary.get("cities", []))


## Donos de território mudaram: atualiza a textura do minimapa.
func _on_territory_changed() -> void:
	_refresh_minimap_territory()


## Aplica no minimapa a textura de dono por província (mundo inteiro, cores do
## nations.csv). No build o sinal `territory_changed` ainda não disparou, então a
## folha ficaria com o minimap.png legado (só GBR/FRA/DEU).
func _refresh_minimap_territory() -> void:
	if hud == null or city_world == null:
		return
	var texture: Texture2D = city_world.minimap_texture()
	if texture != null:
		hud.set_minimap_territory(texture)


func _build_city_world() -> void:
	city_world = CityWorldScript.new()
	add_child(city_world)

	var status: Dictionary = Economy.sim.get_demo_status()
	var cities: Array = status["cities"]
	var ports: Array = status.get("ports", [])
	city_world.build(cities, ports)
	_center_world(cities)


## Coloca o centro das cidades na origem para a câmera enquadrar a Europa.
func _center_world(cities: Array) -> void:
	if cities.is_empty():
		return

	var center: Vector3 = city_world.pixel_to_world(CityWorldScript.framing_center_px(cities))
	city_world.position = -center


func _build_convoys() -> void:
	convoys = ConvoysScript.new()
	convoys.position = Vector3.ZERO
	city_world.add_child(convoys)


## Os veículos de comércio seguem o relógio da economia: 2x/3x aceleram e a
## pausa (índice 0) congela. O ritmo normal é SPEEDS[1] (60 s por dia).
func _on_speed_changed(index: int) -> void:
	if convoys == null:
		return
	var paused := index == 0
	var rate := 1.0
	if not paused:
		rate = float(Economy.SPEEDS[1]) / float(Economy.SPEEDS[index])
	convoys.set_time_scale(rate, paused)


## Câmera inclinada sobre o centro do mapa; o controle começa daqui.
func _setup_camera() -> void:
	var player := get_node_or_null("Player")
	if player == null:
		return

	player.global_position = Vector3.ZERO
	player.rotation = Vector3.ZERO
	player.camera_socket.rotation_degrees.x = CAMERA_PITCH_DEG
	player.camera.position = Vector3(0.0, 0.0, CAMERA_DISTANCE)


func _build_hud() -> void:
	hud = HudScript.new()
	add_child(hud)
	hud.city_focused.connect(_on_city_focused)
	hud.map_mode_requested.connect(_on_map_mode_requested)
	hud.map_point_requested.connect(_on_map_point_requested)
	hud.legend_reset_requested.connect(_on_legend_reset_requested)
	hud.window_closed.connect(_on_hud_window_closed)


## Faixa permitida para o centro da câmera em Z (coordenadas globais).
func _camera_z_bounds() -> Vector2:
	var height: float = city_world.PLANE_SIZE.y
	var north: float = (90.0 - CAMERA_LAT_NORTH) / 180.0 * height - height * 0.5
	var south: float = (90.0 - CAMERA_LAT_SOUTH) / 180.0 * height - height * 0.5
	return Vector2(north, south) + Vector2.ONE * city_world.position.z


## Destino de move_to sempre dentro da folha (sem usar a folga elástica).
func _clamp_camera_target(target: Vector3) -> Vector3:
	var bounds := _camera_z_bounds()
	target.z = clampf(target.z, bounds.x, bounds.y)
	return target


## Limite elástico: no máximo CAMERA_OVERSHOOT além da folha, e a mola puxa de
## volta. Enquanto o jogador empurra (tecla, borda ou arrasto) ele sente
## resistência; ao soltar, a câmera volta suavemente.
func _apply_camera_limit(player: Node3D, delta: float) -> void:
	var bounds := _camera_z_bounds()
	var z := clampf(player.global_position.z,
			bounds.x - CAMERA_OVERSHOOT, bounds.y + CAMERA_OVERSHOOT)
	var inside := clampf(z, bounds.x, bounds.y)
	if not is_equal_approx(z, inside):
		z = lerpf(z, inside, 1.0 - exp(-delta / CAMERA_SPRING_TIME))
		if absf(z - inside) < 0.05:
			z = inside
	if not is_equal_approx(z, player.global_position.z):
		player.global_position.z = z


func _build_vignette() -> void:
	if not VIGNETTE_ENABLED:
		return
	var layer := CanvasLayer.new()
	layer.name = "ScreenVignette"
	layer.layer = VIGNETTE_LAYER
	var rect := ColorRect.new()
	rect.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	rect.mouse_filter = Control.MOUSE_FILTER_IGNORE
	var material := ShaderMaterial.new()
	material.shader = VIGNETTE_SHADER
	rect.material = material
	layer.add_child(rect)
	add_child(layer)


## Clique no minimapa: leva a câmera ao ponto (UV do mapa -> mundo).
func _on_map_point_requested(map_uv: Vector2) -> void:
	var player := get_node_or_null("Player")
	if player == null:
		return
	var size: Vector2 = city_world.PLANE_SIZE
	var target: Vector3 = Vector3((map_uv.x - 0.5) * size.x, 0.0, (map_uv.y - 0.5) * size.y) \
			+ city_world.position
	target = _clamp_camera_target(target)
	player.move_to(Vector2(target.x, target.z), 4.0)


## Área do chão vista pela câmera, em UV do mapa, para o minimapa.
func _update_minimap(player) -> void:
	var camera: Camera3D = player.camera
	var size: Vector2 = city_world.PLANE_SIZE
	var local: Vector3 = player.global_position - city_world.position
	var center := Vector2(local.x / size.x + 0.5, local.z / size.y + 0.5)
	var distance: float = camera.position.z
	var half_v := tan(deg_to_rad(camera.fov * 0.5))
	var aspect := get_viewport().get_visible_rect().size.aspect()
	var pitch := absf(deg_to_rad(player.camera_socket.rotation_degrees.x))
	var ground := Vector2(2.0 * distance * half_v * aspect,
			2.0 * distance * half_v / maxf(sin(pitch), 0.2))
	hud.set_minimap_view(center, ground / size)


## Centraliza a câmera na cidade e destaca o marcador.
func _on_city_focused(city_id: String) -> void:
	city_world.select_city(city_id)
	var target: Vector3 = _clamp_camera_target(city_world.city_world_position(city_id))
	var player := get_node_or_null("Player")
	if player == null:
		return
	player.move_to(Vector2(target.x, target.z))


## Troca de modo de mapa: descarta a seleção/legenda do modo anterior antes de
## aplicar o novo. Sem isso, uma cidade selecionada no mapa de recursos ficava
## com o anel ativo e o primeiro clique no político só a desselecionava (a ficha
## T3 só abria no segundo clique).
func _on_map_mode_requested(mode: String) -> void:
	if city_world == null:
		return
	if city_world.current_mode() != mode:
		_clear_map_selection()
	city_world.set_mode(mode)


## Esc com seleção no mapa desseleciona e consome o evento; o menu de pausa só
## abre no Esc seguinte, pelo HUD (_unhandled_input).
func _input(event: InputEvent) -> void:
	if not event.is_action_pressed("ui_cancel") or event.is_echo():
		return
	if _handle_escape():
		get_viewport().set_input_as_handled()


## Esc com seleção no mapa: desseleciona e consome o evento (o HUD só abre o
## menu de pausa no Esc seguinte). Painéis abertos continuam fechando pelo HUD.
func _handle_escape() -> bool:
	if hud == null or city_world == null:
		return false
	if hud.is_overlay_open() or not city_world.has_selection():
		return false
	_clear_map_selection()
	return true


## Remove a seleção do mapa e devolve o painel lateral/legenda ao estado vazio.
func _clear_map_selection() -> void:
	if city_world == null:
		return
	city_world.clear_selection()
	if hud != null:
		hud.close_window()
		hud.show_general_legend()


## "Todos"/"Voltar" na legenda: limpa TODA a seleção ativa no mapa (anel de
## depósito/trigo, cidade/porto e realce de território) antes de voltar à lista.
func _on_legend_reset_requested() -> void:
	if city_world == null:
		return
	city_world.clear_selection()


## Fechar o painel/cartão do HUD também remove o anel do mapa.
func _on_hud_window_closed() -> void:
	if city_world != null:
		city_world.clear_selection()


## Clique no mapa seleciona o lugar/jazida sob o cursor; clicar de novo no
## MESMO item desseleciona. A janela do HUD tem prioridade sobre o mapa.
## Movimento do mouse atualiza o realce de hover.
func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventKey and event.pressed and not event.echo \
			and event.physical_keycode == KEY_SPACE:
		Economy.toggle_pause()
		get_viewport().set_input_as_handled()
		return

	if hud == null or hud.is_window_open():
		return
	if event is InputEventMouseMotion:
		# O hover também responde ao evento (e não só ao polling do _process):
		# além de ser a entrada real do jogador, é o caminho exercitado pelo
		# teste headless de cursor.
		_update_city_hover_at(event.position)
		return
	if not event.is_action_pressed("province_select"):
		return
	if _handle_map_click(get_viewport().get_mouse_position()):
		get_viewport().set_input_as_handled()


## Resolve o clique em pixels de tela. Fonte única do toggle: item diferente
## seleciona, item igual ou área vazia desseleciona. Devolve true se consumiu.
func _handle_map_click(mouse: Vector2) -> bool:
	var player := get_node_or_null("Player")
	if player == null or city_world == null or hud == null:
		return false
	var camera: Camera3D = player.camera
	# Mapa de recursos: clique no depósito mostra a ficha na legenda.
	if hud.map_mode() == "resources":
		var deposit: int = city_world.pick_deposit(mouse, camera)
		if deposit >= 0:
			GameCursor.set_city_hover(false)
			if city_world.selected_deposit() == deposit:
				city_world.set_deposit_selected(-1)
				hud.show_general_legend()
			else:
				city_world.select_city("")
				city_world.set_deposit_selected(deposit)
				hud.show_deposit_info(city_world.deposit_info(deposit))
			return true
		# Clique fora de depósito limpa o anel E volta a legenda geral (filtros).
		city_world.clear_selection()
		hud.show_general_legend()
	var port_id: String = city_world.pick_port(mouse, camera)
	if not port_id.is_empty():
		GameCursor.set_city_hover(false)
		if city_world.selected_place_id() == port_id:
			city_world.clear_selection()
		else:
			city_world.set_deposit_selected(-1)
			city_world.select_port(port_id)
			hud.open_port(city_world.port_name(port_id))
		return true
	var city_id: String = city_world.pick_city(mouse, camera)
	if city_id.is_empty():
		city_world.clear_selection()
		return false
	GameCursor.set_city_hover(false)
	if city_world.selected_place_id() == city_id:
		city_world.clear_selection()
		hud.close_window()
		return true
	city_world.set_deposit_selected(-1)
	if hud.map_mode() == "resources":
		# No mapa de recursos a legenda mostra o que a cidade acessa.
		city_world.select_city(city_id)
		hud.show_city_resources(city_id, city_world.city_resource_entries(city_id))
	else:
		hud.open_city(city_id)
	return true


## Realce/cursor da cidade sob o cursor. Roda por frame para não ficar preso
## quando o mouse entra na UI (o Motion não chega ao _unhandled_input ali).
func _update_city_hover_pass() -> void:
	if hud == null or city_world == null:
		return
	_update_city_hover_at(get_viewport().get_mouse_position())


## Núcleo do hover, com a posição de tela explícita: o `_process` passa a
## posição real do viewport e o teste headless chama este método direto (o
## headless não move o mouse do SO). Não abre janela nem depende de DisplayServer.
func _update_city_hover_at(mouse: Vector2) -> void:
	if city_world == null or hud == null:
		return
	var player := get_node_or_null("Player")
	var dragging: bool = player != null and player.has_method("is_dragging") \
			and player.is_dragging()
	var blocked: bool = hud.is_window_open() \
			or _gui_blocks_map_hover()
	if dragging or blocked:
		if not _last_hover.is_empty():
			_last_hover = ""
			city_world.set_hover("")
		city_world.set_deposit_hover(-1)
		GameCursor.set_state(dragging, "")
		return
	if player == null:
		return
	# Cidade, porto e jazida são clicáveis em TODOS os modos: o hover sempre vira
	# mão (nunca o I-beam de texto). No mapa de recursos a jazida tem prioridade
	# (badge sobre o ponto e clique abre a ficha do depósito na legenda).
	if hud.map_mode() == "resources":
		var deposit: int = city_world.pick_deposit(mouse, player.camera)
		if deposit >= 0:
			if not _last_hover.is_empty():
				_last_hover = ""
				city_world.set_hover("")
			city_world.set_deposit_hover(deposit)
			GameCursor.set_state(false, "deposit")
			return
	city_world.set_deposit_hover(-1)
	var port_id: String = city_world.pick_port(mouse, player.camera)
	var city_id: String = "" if not port_id.is_empty() else city_world.pick_city(mouse, player.camera)
	var hover_id: String = port_id if not port_id.is_empty() else city_id
	if hover_id != _last_hover:
		_last_hover = hover_id
		city_world.set_hover(hover_id)
	var kind: String = ""
	if not port_id.is_empty():
		kind = "port"
	elif not city_id.is_empty():
		kind = "city"
	GameCursor.set_state(false, kind)


## Algum Control do HUD está mesmo sob o mouse? Só conta o que PODE capturar o
## mouse (STOP/PASS); a raiz e as camadas de sobreposição são IGNORE e não
## aparecem aqui. Um Control invisível/cheio com STOP roubava o hover do mapa e
## devolvia a seta; `_map_hover_controls` mantém só o que realmente cobre tela.
func _gui_blocks_map_hover() -> bool:
	var hovered: Control = get_viewport().gui_get_hovered_control()
	if hovered == null:
		return false
	return hovered.mouse_filter != Control.MOUSE_FILTER_IGNORE


## Mantém a câmera dentro de um período do mapa: ao passar da borda em X, ela
## reaparece do outro lado, onde a cópia lateral do mundo continua a paisagem.
## Em Z aplica o limite elástico da folha.
func _process(delta: float) -> void:
	if city_world == null:
		return

	var player := get_node_or_null("Player") as Node3D
	if player == null:
		return

	var half_width: float = city_world.PLANE_SIZE.x * 0.5
	var offset: float = player.position.x - city_world.position.x
	if offset > half_width:
		player.position.x -= city_world.PLANE_SIZE.x
	elif offset < -half_width:
		player.position.x += city_world.PLANE_SIZE.x

	_apply_camera_limit(player, delta)

	_update_city_hover_pass()
	_update_city_tooltip()
	if hud != null:
		_update_minimap(player)


## Ficha de papel da cidade sob o mouse: o mapa só informa QUAL cidade está em
## hover e ONDE ela está na tela. O atraso de ~250 ms, o conteúdo e a decisão de
## esconder com sobreposição aberta ficam no HUD (camada de apresentação).
func _update_city_tooltip() -> void:
	if hud == null or city_world == null:
		return
	if hud.is_overlay_open() or _last_hover.is_empty() \
			or city_world.place_kind(_last_hover) != "city":
		hud.clear_city_tooltip()
		return
	var player := get_node_or_null("Player")
	if player == null:
		return
	var point: Vector2 = player.camera.unproject_position(
			city_world.city_world_position(_last_hover))
	hud.hover_city_tooltip(_last_hover, point, hud.map_mode())
