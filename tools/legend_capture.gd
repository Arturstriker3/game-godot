extends Node

## Captura os 4 estados da "Etiqueta de inventário" (legenda do mapa de
## recursos) com dados REAIS do jogo — filtros, cidade (Paris), área de trigo e
## depósito (Khouribga; fallback Chaillac) — numa única resolução, sem recorte.
##
## Uso:
##   godot --path . res://tools/legend_capture.tscn -- --size=1920x1080
##
## Grava em docs/ui/captures/legenda_v1/{resources,city,wheat,deposit}.png.

const MAIN_SCENE := "res://game/main_game.tscn"
const WAIT_FRAMES := 3

var _size := Vector2i(1920, 1080)
var _out_dir := "res://docs/ui/captures/legenda_v1"
var _game
var _hud
var _layer


func _ready() -> void:
	_parse_args()
	get_window().size = _size
	await get_tree().process_frame
	await get_tree().process_frame

	_game = (load(MAIN_SCENE) as PackedScene).instantiate()
	add_child(_game)
	await _wait_frames(10)

	_hud = _game.hud
	_hud.set_map_mode("resources")
	await _wait_frames(WAIT_FRAMES)

	# (a) Filtros de recurso (estado padrão do modo "resources").
	await _capture_state("resources", Callable())

	# (b) Cidade: Paris (FRA-PAR), com as entradas reais de recurso alcançadas.
	await _capture_state("city", func() -> void:
		_hud.show_city_resources("FRA-PAR",
				_game.city_world.city_resource_entries("FRA-PAR")))

	# (c) Área de trigo: primeiro ponto agrícola do mapa.
	_layer = _game.city_world._mode_layer
	var wheat_index: int = _layer._mineral_points.size()
	await _capture_state("wheat", func() -> void:
		_hud.show_deposit_info(_layer.deposit_info(wheat_index)))

	# (d) Depósito: Khouribga se existir, senão Chaillac.
	var index := _find_deposit("Khouribga")
	if index < 0:
		index = _find_deposit("Chaillac")
	await _capture_state("deposit", func() -> void:
		_hud.show_deposit_info(_layer.deposit_info(index)))

	# Libera a cena 3D antes de sair para evitar vazamento de RIDs de textura.
	_game.queue_free()
	await get_tree().process_frame
	get_tree().quit()


func _parse_args() -> void:
	for arg in OS.get_cmdline_user_args():
		var key := arg.get_slice("=", 0)
		var value := arg.get_slice("=", 1)
		match key:
			"--size":
				var parts := value.split("x")
				_size = Vector2i(int(parts[0]), int(parts[1]))
			"--out":
				_out_dir = value


func _wait_frames(n: int) -> void:
	for _i in n:
		await get_tree().process_frame


func _capture_state(state: String, action: Callable) -> void:
	if action.is_valid():
		action.call()
	for _i in WAIT_FRAMES:
		await get_tree().process_frame
	await RenderingServer.frame_post_draw

	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(_out_dir))
	var path := "%s/%s.png" % [_out_dir, state]
	var error := get_viewport().get_texture().get_image().save_png(path)
	if error != OK:
		push_error("falha ao gravar %s (erro %d)" % [path, error])
	else:
		print("capturado: ", ProjectSettings.globalize_path(path))
	_audit_legend(state)


## Reporta a caixa da etiqueta e qualquer estouro fora do viewport.
func _audit_legend(state: String) -> void:
	var legend: Control = _hud.get("_legend") as Control
	if legend == null or not legend.visible:
		print("[%s] legenda oculta" % state)
		return
	var rect := legend.get_global_rect()
	var vp := get_viewport().get_visible_rect().size
	var overflow := maxf(maxf(0.0, -rect.position.x), maxf(0.0, -rect.position.y))
	overflow = maxf(overflow, maxf(0.0, rect.end.x - vp.x))
	overflow = maxf(overflow, maxf(0.0, rect.end.y - vp.y))
	print("[%s] rect=%s overflow=%s" % [state, rect, snappedf(overflow, 0.1)])
	_audit_title(state)
	_audit_header(state)


## Posição global do botão de ação (Todos/Voltar) e do título — os centros-y
## devem coincidir (±1 px), e o topo do título e a margem direita do botão devem
## ser iguais nos 4 estados. A etiqueta é ancorada embaixo e cresce para cima,
## então o y global varia com a altura; o que importa é o OFFSET em relação ao
## topo da etiqueta.
func _audit_header(state: String) -> void:
	var legend: Control = _hud.get("_legend") as Control
	var button: Control = _hud.get("_legend_back") as Control
	if button == null or not is_instance_valid(button):
		button = _hud.get("_legend_all") as Control
	var title: Label = _hud.get("_legend_title") as Label
	if legend == null or button == null or title == null:
		print("[%s] sem cabeçalho" % state)
		return
	var legend_rect := legend.get_global_rect()
	var button_rect := button.get_global_rect()
	var title_rect := title.get_global_rect()
	print("[%s] centro-y título=%.1f | centro-y botão=%.1f | título topo=%.1f | margem_direita=%.1f" % [
		state,
		snappedf(title_rect.position.y + title_rect.size.y * 0.5, 0.1),
		snappedf(button_rect.position.y + button_rect.size.y * 0.5, 0.1),
		snappedf(title_rect.position.y - legend_rect.position.y, 0.1),
		snappedf(legend_rect.end.x - button_rect.end.x, 0.1)])


## Confirma que o título da etiqueta não está elidido (sem clip/ellipsis e com
## largura >= ao texto inteiro).
func _audit_title(state: String) -> void:
	var title: Label = _hud.get("_legend_title") as Label
	if title == null:
		print("[%s] sem título" % state)
		return
	var elided := title.size.x + 0.5 < title.get_combined_minimum_size().x
	print("[%s] título='%s' clip=%s overrun=%d size=%s min=%s elidido=%s" % [
		state, title.text, title.clip_text, title.text_overrun_behavior,
		title.size, title.get_combined_minimum_size(), elided])


## Índice do primeiro depósito cujo nome contém `needle`; -1 se não houver.
func _find_deposit(needle: String) -> int:
	for i in _layer._mineral_points.size():
		var point: Array = _layer._mineral_points[i]
		if point.size() > 7 and String(point[7]).contains(needle):
			return i
	return -1
