extends Node3D

## Cena principal: monta o mundo de cidades (fundo tÃƒÂ¡tico + cidades + Voronoi),
## enquadra a cÃƒÂ¢mera sobre a Europa, conecta o comÃƒÂ©rcio aos comboios 3D e sobe o HUD.

const CityWorldScript: Script = preload("res://map/city_world.gd")
const ConvoysScript: Script = preload("res://core/economy/trade_convoys.gd")
const HudScript: Script = preload("res://core/ui/hud.gd")

## InclinaÃƒÂ§ÃƒÂ£o da cÃƒÂ¢mera (graus) e distÃƒÂ¢ncia do alvo.
const CAMERA_PITCH_DEG := -56.0
const CAMERA_DISTANCE := 52.0

## Limite do centro da cÃƒÂ¢mera na folha (latitudes). Em X o mundo dÃƒÂ¡ a volta e
## as cÃƒÂ³pias laterais garantem que nÃƒÂ£o hÃƒÂ¡ vazio; em Z o centro fica entre estas
## latitudes. AlÃƒÂ©m delas hÃƒÂ¡ uma folga elÃƒÂ¡stica: dÃƒÂ¡ para empurrar atÃƒÂ©
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
	# Roda depois da cÃƒÂ¢mera e do jogador: o limite vale para qualquer movimento
	# (teclado, borda da tela, arrasto, move_to).
	process_priority = 100
	_build_city_world()
	_build_convoys()
	_setup_camera()
	_build_vignette()
	_build_hud()
	Economy.trade_orders_ready.connect(convoys.spawn_orders)
	Economy.speed_changed.connect(_on_speed_changed)
	_on_speed_changed(Economy.speed)
	hud.resource_filter_changed.connect(city_world.set_resource_filter)
	Economy.status_updated.connect(_on_status_updated)
	city_world.territory_changed.connect(_on_territory_changed)


## MantÃƒÂ©m os modos de mapa (populaÃƒÂ§ÃƒÂ£o, economia, indÃƒÂºstria, recursos) vivos.
func _on_status_updated(summary: Dictionary) -> void:
	city_world.refresh_modes(summary.get("cities", []))


## Donos de território mudaram: atualiza a textura do minimapa.
func _on_territory_changed() -> void:
	if hud != null:
		hud.set_minimap_territory(city_world.minimap_texture())


func _build_city_world() -> void:
	city_world = CityWorldScript.new()
	add_child(city_world)

	var status: Dictionary = Economy.sim.get_status()
	var cities: Array = status["cities"]
	var ports: Array = status.get("ports", [])
	city_world.build(cities, ports)
	_center_world(cities)


## Coloca o centro das cidades na origem para a cÃƒÂ¢mera enquadrar a Europa.
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


## CÃƒÂ¢mera inclinada sobre o centro do mapa; o controle comeÃƒÂ§a daqui.
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


## Faixa permitida para o centro da cÃƒÂ¢mera em Z (coordenadas globais).
func _camera_z_bounds() -> Vector2:
	var height: float = city_world.PLANE_SIZE.y
	var north: float = (90.0 - CAMERA_LAT_NORTH) / 180.0 * height - height * 0.5
	var south: float = (90.0 - CAMERA_LAT_SOUTH) / 180.0 * height - height * 0.5
	return Vector2(north, south) + Vector2.ONE * city_world.position.z


## Destino de move_to sempre dentro da folha (sem usar a folga elÃƒÂ¡stica).
func _clamp_camera_target(target: Vector3) -> Vector3:
	var bounds := _camera_z_bounds()
	target.z = clampf(target.z, bounds.x, bounds.y)
	return target


## Limite elÃƒÂ¡stico: no mÃƒÂ¡ximo CAMERA_OVERSHOOT alÃƒÂ©m da folha, e a mola puxa de
## volta. Enquanto o jogador empurra (tecla, borda ou arrasto) ele sente
## resistÃƒÂªncia; ao soltar, a cÃƒÂ¢mera volta suavemente.
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


## Clique no minimapa: leva a cÃƒÂ¢mera ao ponto (UV do mapa -> mundo).
func _on_map_point_requested(map_uv: Vector2) -> void:
	var player := get_node_or_null("Player")
	if player == null:
		return
	var size: Vector2 = city_world.PLANE_SIZE
	var target: Vector3 = Vector3((map_uv.x - 0.5) * size.x, 0.0, (map_uv.y - 0.5) * size.y) \
			+ city_world.position
	target = _clamp_camera_target(target)
	player.move_to(Vector2(target.x, target.z), 4.0)


## ÃƒÂrea do chÃƒÂ£o vista pela cÃƒÂ¢mera, em UV do mapa, para o minimapa.
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


## Centraliza a cÃƒÂ¢mera na cidade e destaca o marcador.
func _on_city_focused(city_id: String) -> void:
	city_world.select_city(city_id)
	var target: Vector3 = _clamp_camera_target(city_world.city_world_position(city_id))
	var player := get_node_or_null("Player")
	if player == null:
		return
	player.move_to(Vector2(target.x, target.z))


func _on_map_mode_requested(mode: String) -> void:
	city_world.set_mode(mode)


## Clique no mapa seleciona a cidade mais prÃƒÂ³xima; a janela do HUD tem
## prioridade sobre o mapa. Movimento do mouse atualiza o realce de hover.
func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventKey and event.pressed and not event.echo \
			and event.physical_keycode == KEY_SPACE:
		Economy.toggle_pause()
		get_viewport().set_input_as_handled()
		return

	if hud == null or hud.is_window_open():
		return
	if not event.is_action_pressed("province_select"):
		return
	var player := get_node_or_null("Player")
	if player == null:
		return
	var mouse := get_viewport().get_mouse_position()
	# Mapa de recursos: clique no depósito mostra a ficha na legenda.
	if hud.map_mode() == "resources":
		var deposit: int = city_world.pick_deposit(mouse, player.camera)
		if deposit >= 0:
			hud.show_deposit_info(city_world.deposit_info(deposit))
			get_viewport().set_input_as_handled()
			return
	var port_id: String = city_world.pick_port(mouse, player.camera)
	if not port_id.is_empty():
		GameCursor.set_city_hover(false)
		city_world.select_port(port_id)
		hud.open_port(city_world.port_name(port_id))
		get_viewport().set_input_as_handled()
		return
	var city_id: String = city_world.pick_city(mouse, player.camera)
	if city_id.is_empty():
		return
	GameCursor.set_city_hover(false)
	if hud.map_mode() == "resources":
		# No mapa de recursos a legenda mostra o que a cidade acessa.
		city_world.select_city(city_id)
		hud.show_city_resources(city_id, city_world.city_resource_entries(city_id))
	else:
		hud.open_city(city_id)
	get_viewport().set_input_as_handled()


## Realce/cursor da cidade sob o cursor. Roda por frame para nÃƒÂ£o ficar preso
## quando o mouse entra na UI (o Motion nÃƒÂ£o chega ao _unhandled_input ali).
func _update_city_hover_pass() -> void:
	if city_world == null or hud == null:
		return
	var player := get_node_or_null("Player")
	var dragging: bool = player != null and player.has_method("is_dragging") \
			and player.is_dragging()
	var blocked: bool = hud.is_window_open() \
			or get_viewport().gui_get_hovered_control() != null
	if dragging or blocked:
		if not _last_hover.is_empty():
			_last_hover = ""
			city_world.set_hover("")
		city_world.set_deposit_hover(-1)
		GameCursor.set_state(dragging, "")
		return
	if player == null:
		return
	var mouse := get_viewport().get_mouse_position()
	# Mapa de recursos: depósito tem prioridade (cursor "i", badge sobre o ponto
	# e clique mostram a ficha do depósito na legenda).
	if hud.map_mode() == "resources":
		var deposit: int = city_world.pick_deposit(mouse, player.camera)
		if deposit >= 0:
			if not _last_hover.is_empty():
				_last_hover = ""
				city_world.set_hover("")
			city_world.set_deposit_hover(deposit)
			GameCursor.set_state(false, "info")
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
		kind = "info" if hud.map_mode() != "political" else "city"
	GameCursor.set_state(false, kind)


## MantÃƒÂ©m a cÃƒÂ¢mera dentro de um perÃƒÂ­odo do mapa: ao passar da borda em X, ela
## reaparece do outro lado, onde a cÃƒÂ³pia lateral do mundo continua a paisagem.
## Em Z aplica o limite elÃƒÂ¡stico da folha.
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
	if hud != null:
		_update_minimap(player)
