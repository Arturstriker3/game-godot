extends Node3D

## Cena principal: monta o mundo de cidades (fundo tático + cidades + Voronoi),
## enquadra a câmera sobre a Europa, conecta o comércio aos comboios 3D e sobe o HUD.

const CityWorldScript: Script = preload("res://map/city_world.gd")
const ConvoysScript: Script = preload("res://core/economy/trade_convoys.gd")
const HudScript: Script = preload("res://core/ui/hud.gd")

## Inclinação da câmera (graus) e distância do alvo.
const CAMERA_PITCH_DEG := -56.0
const CAMERA_DISTANCE := 52.0

var city_world
var convoys
var hud


func _ready() -> void:
	_build_city_world()
	_build_convoys()
	_setup_camera()
	_build_hud()
	Economy.trade_orders_ready.connect(convoys.spawn_orders)
	Economy.status_updated.connect(_on_status_updated)


## Mantém os modos de mapa (população, economia, indústria, recursos) vivos.
func _on_status_updated(summary: Dictionary) -> void:
	city_world.refresh_modes(summary.get("cities", []))


func _build_city_world() -> void:
	city_world = CityWorldScript.new()
	add_child(city_world)

	var cities: Array = Economy.sim.get_status()["cities"]
	city_world.build(cities)
	_center_world(cities)


## Coloca o centro das cidades na origem para a câmera enquadrar a Europa.
func _center_world(cities: Array) -> void:
	if cities.is_empty():
		return

	var center := Vector3.ZERO
	for city in cities:
		center += city_world.pixel_to_world(Vector2(city["x"], city["y"]))
	city_world.position = -center / float(cities.size())


func _build_convoys() -> void:
	convoys = ConvoysScript.new()
	convoys.position = Vector3.ZERO
	city_world.add_child(convoys)


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


## Centraliza a câmera na cidade e destaca o marcador.
func _on_city_focused(city_id: String) -> void:
	city_world.select_city(city_id)
	var target: Vector3 = city_world.city_world_position(city_id)
	var player := get_node_or_null("Player")
	if player == null:
		return
	player.move_to(Vector2(target.x, target.z))


func _on_map_mode_requested(mode: String) -> void:
	city_world.set_mode(mode)


## Clique no mapa seleciona a cidade mais próxima; a janela do HUD tem
## prioridade sobre o mapa.
func _unhandled_input(event: InputEvent) -> void:
	if hud == null or hud.is_window_open():
		return
	if not event.is_action_pressed("province_select"):
		return
	var player := get_node_or_null("Player")
	if player == null:
		return
	var city_id: String = city_world.pick_city(
			get_viewport().get_mouse_position(), player.camera)
	if city_id.is_empty():
		return
	hud.open_city(city_id)
	get_viewport().set_input_as_handled()


## Mantém a câmera dentro de um período do mapa: ao passar da borda em X, ela
## reaparece do outro lado, onde a cópia lateral do mundo continua a paisagem.
func _process(_delta: float) -> void:
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
