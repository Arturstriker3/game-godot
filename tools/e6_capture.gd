extends Node

## Captura do mapa de energia (E6): instancia a CENA PRINCIPAL do jogo — com o
## mapa 3D de verdade, não só o HUD sobre fundo neutro — entra no modo energia e
## fotografa três enquadramentos do potencial renovável.
##
## Uso (nunca headless: precisa do renderizador para o mapa aparecer):
##   godot --path . res://tools/e6_capture.tscn -- --out=G:/caminho/fora/do/repo
##
## Argumentos: --out=<dir>  --width=1920  --height=1080  --locale=pt_BR|en|es
##
## Não altera dados, economia nem cenas do jogo: só lê o estado e muda o modo de
## mapa/filtros pela API pública do HUD (os sinais do jogo aplicam no mapa).

const MAIN_SCENE: PackedScene = preload("res://game/main_game.tscn")
## Mesmo enquadramento de game/main_game.gd.
const CAMERA_PITCH_DEG := -56.0
const WAIT_FRAMES := 6

## Enquadramentos pedidos no fechamento do E6. `filter` usa as chaves da
## legenda de energia (solar/wind/hydro) ou "all" para todas as fontes.
const SHOTS := [
	{"name": "e6_capture_world", "focus": "", "distance": 320.0, "filter": "all"},
	{"name": "e6_capture_sa_hydro", "focus": "BRA-CUI", "distance": 72.0,
			"filter": "hydro"},
	{"name": "e6_capture_sahara_solar", "focus": "DZA-TAM", "distance": 65.0,
			"filter": "solar"},
]

var _out_dir := "res://docs/ui/captures"
var _width := 1920
var _height := 1080
## Idioma da captura: o settings.cfg do operador não decide a evidência.
var _locale := "pt_BR"
var _game: Node


func _ready() -> void:
	_parse_args()
	DirAccess.make_dir_recursive_absolute(
			ProjectSettings.globalize_path(_out_dir))
	if not _locale.is_empty():
		I18n.set_locale(_locale)
	get_window().size = Vector2i(_width, _height)
	await get_tree().process_frame

	# Congela o relógio da economia: os marcadores de POTENCIAL são estáticos,
	# mas comboios/animações e o dia no HUD mudariam entre as fotos.
	Economy.set_speed(0)
	_game = MAIN_SCENE.instantiate()
	add_child(_game)
	await _wait_for_world()

	var hud: Node = _game.get("hud")
	var world: Node3D = _game.get("city_world")
	hud.set_map_mode("energy")
	# O modo energia parseia os JSON de potencial no primeiro uso.
	await _wait_frames(10)
	_disable_camera_input()
	print("[e6_capture] resolução %s" % get_window().size)

	for shot: Dictionary in SHOTS:
		_apply_energy_filter(hud, String(shot["filter"]))
		_frame_camera(world, String(shot["focus"]), float(shot["distance"]))
		await _wait_frames(WAIT_FRAMES)
		await _settle_lod(world)
		await RenderingServer.frame_post_draw
		var image := get_viewport().get_texture().get_image()
		var path := "%s/%s.png" % [_out_dir, String(shot["name"])]
		var error := image.save_png(ProjectSettings.globalize_path(path))
		print("[e6_capture] %s %dx%d filtro=%s marcadores=%d erro=%d" % [
			path, image.get_width(), image.get_height(),
			String(shot["filter"]), _marker_count(world), error,
		])
	get_tree().quit()


func _parse_args() -> void:
	for arg in OS.get_cmdline_user_args():
		var key := arg.get_slice("=", 0)
		var value := arg.get_slice("=", 1)
		match key:
			"--out":
				_out_dir = value
			"--width":
				_width = int(value)
			"--height":
				_height = int(value)
			"--locale":
				_locale = value


## Espera a cena principal construir o mapa (o build é síncrono no _ready, mas
## a camada de modos só existe depois dele) e a primeira geografia assentar.
func _wait_for_world() -> void:
	for _frame in 120:
		await get_tree().process_frame
		var world: Variant = _game.get("city_world")
		if world != null and world.get("_mode_layer") != null:
			return
	push_error("e6_capture: CityWorld não ficou pronto")


func _wait_frames(count: int) -> void:
	for _frame in count:
		await get_tree().process_frame


## Sem mouse/teclado na captura: o auto-pan por borda e o zoom por input
## moveriam a câmera durante os frames de espera.
func _disable_camera_input() -> void:
	var player: Node = _game.get_node_or_null("Player")
	if player == null:
		return
	player.set("camera_can_move", false)
	player.set("camera_can_zoom", false)
	player.set("camera_can_automatic_pan", false)


## Liga só a fonte pedida (ou todas). Usa o toggle do HUD para a legenda ficar
## coerente com o mapa; o sinal `energy_filter_changed` aplica no CityWorld.
func _apply_energy_filter(hud: Node, keep: String) -> void:
	var enabled: Dictionary = hud.get("_energy_enabled")
	if enabled.is_empty():
		return
	for source: String in ["solar", "wind", "hydro"]:
		var want_on: bool = keep == "all" or keep == source
		if bool(enabled.get(source, true)) != want_on:
			hud.call("_toggle_energy", source)


## Enquadra a câmera do jogo no centro pedido (id de cidade) e na distância de
## zoom; `distance` é a distância ao chão (mesma régua do CityWorld).
func _frame_camera(world: Node3D, focus: String, distance: float) -> void:
	var player: Node3D = _game.get_node_or_null("Player")
	if player == null:
		return
	var target := Vector3.ZERO
	if not focus.is_empty():
		target = world.city_world_position(focus)
	player.position = Vector3(target.x, 0.0, target.z)
	player.rotation = Vector3.ZERO
	player.camera_socket.rotation_degrees.x = CAMERA_PITCH_DEG
	player.camera.position = Vector3(0.0, 0.0, distance)


## Drena as filas de LOD (mesmo procedimento de tools/map_capture.gd) para a
## foto não sair com o tecido urbano pela metade.
func _settle_lod(world: Node3D) -> void:
	await get_tree().process_frame
	await get_tree().process_frame
	var idle := 0
	for _frame in 180:
		var urban_queue: Array = world.get("_urban_queue")
		var port_queue: Array = world.get("_port_model_queue")
		if urban_queue.is_empty() and port_queue.is_empty():
			idle += 1
			if idle >= 2:
				return
		else:
			idle = 0
		await get_tree().process_frame
	await get_tree().process_frame


## Marcadores de potencial desenhados no nível de zoom atual (leitura).
func _marker_count(world: Node3D) -> int:
	var layer: Variant = world.get("_mode_layer")
	if layer == null:
		return 0
	var drawn: Array = layer.get("_energy_drawn")
	return drawn.size()
