extends Control

## Captura das bandeiras do Dossiê (T1) em 1920x1080, sem abrir a cena
## principal: instancia o HUD real sobre o papel do mapa, abre Política e troca
## a nação para conferir a proporção de cada bandeira.
##
## Grava em `docs/ui/captures/fix_round/`:
##   - dossie_bandeira.png        (GBR, 2:1)
##   - dossie_bandeira_suica.png  (CHE, 1:1)
##   - dossie_bandeira_catar.png  (QAT, ~1:2,55)
##
## Uso (janela, não headless):
##   godot --path . res://tools/flag_capture.tscn

const HudScript: Script = preload("res://core/ui/hud.gd")
const BACKDROP_PATH := "res://map/map_data/paper_flat.png"
const OUT_DIR := "res://docs/ui/captures/fix_round"
const SIZE := Vector2i(1920, 1080)
const SETTLE_FRAMES := 6

## Nação → arquivo. A ordem é a do relatório: 2:1, 1:1 e 1:2,55.
const SHOTS := [
	["GBR", "dossie_bandeira.png"],
	["CHE", "dossie_bandeira_suica.png"],
	["QAT", "dossie_bandeira_catar.png"],
]

var _hud


func _ready() -> void:
	_build_backdrop()
	_hud = HudScript.new()
	add_child(_hud)
	_run.call_deferred()


func _build_backdrop() -> void:
	var texture := load(BACKDROP_PATH) as Texture2D
	if texture != null:
		var backdrop := TextureRect.new()
		backdrop.texture = texture
		backdrop.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
		backdrop.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_COVERED
		backdrop.modulate = Color(0.55, 0.58, 0.56)
		backdrop.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
		backdrop.mouse_filter = Control.MOUSE_FILTER_IGNORE
		add_child(backdrop)
	else:
		var base := ColorRect.new()
		base.color = Color("#1B2523")
		base.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
		base.mouse_filter = Control.MOUSE_FILTER_IGNORE
		add_child(base)
	var shade := ColorRect.new()
	shade.color = Color(0.02, 0.03, 0.04, 0.25)
	shade.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	shade.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(shade)


func _run() -> void:
	get_window().size = SIZE
	await _wait_for_data()
	_hud.open_screen("politics")
	await _settle()

	for shot in SHOTS:
		var tag := String(shot[0])
		var screen: Node = _hud.get("_screens").get("politics")
		if screen == null:
			push_error("[flag_capture] HUD sem tela de política")
			get_tree().quit(1)
			return
		screen.call("select_nation", tag)
		await _settle()
		_capture(String(shot[1]), tag, screen)

	print("[flag_capture] capturas em " + OUT_DIR)
	get_tree().quit(0)


func _wait_for_data() -> void:
	for _frame in 240:
		if not Economy.last_summary.is_empty():
			return
		await get_tree().process_frame
	push_warning("[flag_capture] economia não publicou resumo antes da captura")


func _settle() -> void:
	for _frame in SETTLE_FRAMES:
		await get_tree().process_frame
	await RenderingServer.frame_post_draw


func _capture(file_name: String, tag: String, screen: Node) -> void:
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(OUT_DIR))
	var path := "%s/%s" % [OUT_DIR, file_name]
	var image := get_viewport().get_texture().get_image()
	if image.save_png(path) != OK:
		push_error("[flag_capture] falhou ao gravar " + path)
		return
	print("[flag_capture] %s (%s) → %s" % [tag, _flag_info(screen), path])


## Textura e envelope da bandeira em exibição (para o log da captura).
func _flag_info(screen: Node) -> String:
	var holder := _flag_holder(screen.get("_root"))
	if holder == null:
		return "sem bandeira"
	for child in holder.get_children():
		if child is TextureRect:
			var texture: Texture2D = (child as TextureRect).texture
			if texture == null:
				return "bandeira sem textura"
			return "%dx%d em %s" % [texture.get_width(), texture.get_height(),
					holder.custom_minimum_size]
	return "bandeira sem textura"


func _flag_holder(source: Node) -> Control:
	if source == null:
		return null
	var pending: Array[Node] = [source]
	while not pending.is_empty():
		var node: Node = pending.pop_back()
		if node is Control and node.has_meta("tpl_flag_envelope"):
			return node as Control
		for child in node.get_children():
			pending.append(child)
	return null
