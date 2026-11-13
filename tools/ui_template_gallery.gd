extends Control

## Galeria/captura dos cinco templates de papel (gerador UiTemplateBuilder).
##
## Instancia o HUD real e o mapa de fundo, monta cada template por cima e grava
## PNGs + um relatório objetivo de layout. Não altera o HUD nem a economia;
## apenas pausa a velocidade (T4) enquanto a captura do telegrama acontece.
##
## Uso:
##   godot --path . res://tools/ui_template_gallery.tscn
##   godot --path . res://tools/ui_template_gallery.tscn -- --sizes=1920x1080,1600x900
##
## Saídas: docs/ui/templates/captures/<id>.png e layout_report.json.

const HudScript: Script = preload("res://core/ui/hud.gd")
const BACKDROP_PATH := "res://map/map_data/paper_flat.png"
const OUT_DIR := "res://docs/ui/templates/captures"
const WAIT_FRAMES := 4
const TEMPLATE_LAYER := 60

const TEMPLATES := [
	{"id": "dossie", "label": "T1 Dossiê da nação"},
	{"id": "registro", "label": "T2 Registro de cidades"},
	{"id": "ficha", "label": "T3 Ficha com abas"},
	{"id": "telegrama", "label": "T4 Telegrama"},
	{"id": "quadro", "label": "T5 Quadro comparativo"},
]

## Prefixos de chaves de i18n: um rótulo com prefixo + ponto é chave vazada.
const I18N_PREFIXES := ["alerts", "city", "climate", "common", "deposit", "dock",
		"econ", "hud", "map", "mode", "month", "nation", "pol", "pop", "prod",
		"product", "relief", "resource", "scope", "tpl", "trade"]

var _sizes: Array[String] = ["1920x1080"]
var _locales: Array[String] = ["pt_BR", "en", "es"]
var _hud
var _layer: CanvasLayer
var _stage: Control
var _current: Control
var _report := {"phase": "templates", "shots": [], "audits": [], "skipped": []}
var _speed_before := 1


func _ready() -> void:
	_parse_args()
	_report["timestamp"] = Time.get_datetime_string_from_system()
	_report["engine"] = Engine.get_version_info().get("string", "")
	_report["stretch"] = {
		"mode": ProjectSettings.get_setting("display/window/stretch/mode", ""),
		"aspect": ProjectSettings.get_setting("display/window/stretch/aspect", ""),
	}
	_speed_before = Economy.speed

	_build_background()
	_hud = HudScript.new()
	add_child(_hud)
	await _wait_for_data()

	_layer = CanvasLayer.new()
	_layer.layer = TEMPLATE_LAYER
	add_child(_layer)
	_stage = Control.new()
	_stage.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_stage.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_layer.add_child(_stage)

	for size in _sizes:
		await _capture_size(size)

	Economy.set_speed(_speed_before)
	I18n.set_locale("pt_BR")
	_write_report()
	get_tree().quit()


func _parse_args() -> void:
	for arg in OS.get_cmdline_user_args():
		var key := arg.get_slice("=", 0)
		var value := arg.get_slice("=", 1)
		match key:
			"--sizes":
				_sizes.assign(value.split(",", false))
			"--locales":
				_locales.assign(value.split(",", false))
			"--out":
				_report["out"] = value


func _build_background() -> void:
	var texture: Texture2D = load(BACKDROP_PATH) as Texture2D
	if texture != null:
		var backdrop := TextureRect.new()
		backdrop.texture = texture
		backdrop.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
		backdrop.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_COVERED
		backdrop.modulate = Color(0.62, 0.66, 0.62)
		backdrop.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
		backdrop.mouse_filter = Control.MOUSE_FILTER_IGNORE
		add_child(backdrop)
	else:
		var base := ColorRect.new()
		base.color = Color("#24434a")
		base.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
		base.mouse_filter = Control.MOUSE_FILTER_IGNORE
		add_child(base)


func _wait_for_data() -> void:
	for _frame in 180:
		if not Economy.last_summary.is_empty():
			return
		await get_tree().process_frame
	push_warning("economia não publicou resumo antes da galeria")


func _capture_size(size_text: String) -> void:
	var parts := size_text.split("x")
	var width := int(parts[0])
	var height := int(parts[1]) if parts.size() > 1 else int(float(width) * 0.5625)
	get_window().size = Vector2i(width, height)
	await get_tree().process_frame
	await get_tree().process_frame

	var directory := OUT_DIR
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(directory))

	for entry in TEMPLATES:
		var id := String(entry["id"])
		var spec := UiTemplateBuilder.load_spec(id)
		if spec.is_empty():
			_report["skipped"].append("%s: spec ausente" % id)
			continue

		# Cada template é montado uma vez por idioma; o PNG sai em pt-BR e
		# todos os idiomas entram na auditoria de layout.
		for locale in _locales:
			I18n.set_locale(locale)
			await _show(spec, id)
			if locale == _locales[0]:
				await _save_shot(directory, id, width, height)
			_report["audits"].append(_audit("%dx%d" % [width, height], id, locale))
			_clear()

	# O idioma volta ao padrão para o próximo tamanho.
	I18n.set_locale(_locales[0])


func _show(spec: Dictionary, id: String) -> void:
	_clear()
	var pauses := UiTemplateBuilder.pauses_game(spec)
	if pauses:
		Economy.set_speed(0)
	_current = UiTemplateBuilder.build(spec)
	_current.name = "Shot_%s" % id
	_stage.add_child(_current)
	_current.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_current.close_requested.connect(_clear)
	for _frame in WAIT_FRAMES:
		await get_tree().process_frame
	# A barra superior precisa refletir a pausa do telegrama.
	if pauses and _hud.has_method("_update_top_bar"):
		_hud.call("_update_top_bar")
		await get_tree().process_frame


func _clear() -> void:
	if _current != null and is_instance_valid(_current):
		_stage.remove_child(_current)
		_current.queue_free()
	_current = null
	Economy.set_speed(_speed_before)


func _save_shot(directory: String, id: String, width: int, height: int) -> void:
	await RenderingServer.frame_post_draw
	var suffix := "" if width == 1920 and height == 1080 else "_%dx%d" % [width, height]
	var path := "%s/%s%s.png" % [directory, id, suffix]
	var image := get_viewport().get_texture().get_image()
	var error := image.save_png(path)
	_report["shots"].append({
		"template": id,
		"resolution": "%dx%d" % [width, height],
		"path": path,
		"error": error,
	})


# --- Auditoria objetiva -----------------------------------------------------

func _audit(resolution: String, template: String, locale: String) -> Dictionary:
	var viewport_size := get_viewport().get_visible_rect().size
	var data := {
		"resolution": resolution,
		"template": template,
		"locale": locale,
		"viewport": [viewport_size.x, viewport_size.y],
		"pauses_game": UiTemplateBuilder.pauses_game(
				UiTemplateBuilder.load_spec(template)),
		"focus": "",
		"issues": [],
		"elements": {},
		"labels": [],
	}
	if _current == null:
		data["issues"].append({"kind": "missing_template"})
		return data

	var focus := get_viewport().gui_get_focus_owner()
	if focus != null and _is_inside(focus, _current):
		data["focus"] = str(focus.get_path())
		if not focus.is_visible_in_tree():
			data["issues"].append({"kind": "focus_hidden", "node": data["focus"]})

	_walk(_current, viewport_size, data, 0, false)
	return data


func _walk(node: Node, viewport_size: Vector2, data: Dictionary, depth: int,
		inside_scroll: bool) -> void:
	for child in node.get_children():
		if child is Control and not (child as Control).is_visible_in_tree():
			continue
		if child is Control:
			var control := child as Control
			var rect := control.get_global_rect()
			var scrolling := inside_scroll or control is ScrollContainer

			if not inside_scroll:
				_check_bounds(control, rect, viewport_size, data)
			if depth <= 4 and not control.name.is_empty() and not control is Label:
				data["elements"][str(control.get_path())] = {
					"class": control.get_class(),
					"rect": [rect.position.x, rect.position.y, rect.size.x, rect.size.y],
				}
			if control is Label:
				_check_label(control as Label, data)
				var text := (control as Label).text.strip_edges()
				if not text.is_empty() and data["labels"].size() < 60:
					data["labels"].append(text)
			if control is ScrollContainer:
				_check_scroll(control as ScrollContainer, data)

		_walk(child, viewport_size, data, depth + 1,
				inside_scroll or child is ScrollContainer)


func _check_bounds(control: Control, rect: Rect2, viewport_size: Vector2,
		data: Dictionary) -> void:
	if rect.size.x <= 0.0 or rect.size.y <= 0.0:
		return
	var overflow := 0.0
	overflow = maxf(overflow, -rect.position.x)
	overflow = maxf(overflow, -rect.position.y)
	overflow = maxf(overflow, rect.end.x - viewport_size.x)
	overflow = maxf(overflow, rect.end.y - viewport_size.y)
	if overflow > 2.0:
		data["issues"].append({
			"kind": "offscreen",
			"node": str(control.get_path()),
			"overflow": snappedf(overflow, 0.1),
		})


func _check_label(label: Label, data: Dictionary) -> void:
	var text := label.text.strip_edges()
	if _looks_like_key(text):
		data["issues"].append({
			"kind": "i18n_key",
			"node": str(label.get_path()),
			"text": text,
		})
	if text.is_empty() or label.autowrap_mode != TextServer.AUTOWRAP_OFF:
		return
	var font: Font = label.get_theme_font("font")
	var size_px: int = label.get_theme_font_size("font_size")
	if font == null or size_px <= 0:
		return
	var width := font.get_string_size(label.text, HORIZONTAL_ALIGNMENT_LEFT, -1,
			size_px).x
	if width > label.size.x + 2.0:
		data["issues"].append({
			"kind": "text_elided" if label.clip_text else "text_overflow",
			"node": str(label.get_path()),
			"text_width": snappedf(width, 0.1),
			"box_width": snappedf(label.size.x, 0.1),
		})


func _looks_like_key(text: String) -> bool:
	if text.length() < 6 or text.contains(" "):
		return false
	return I18N_PREFIXES.has(text.get_slice(".", 0)) and text.contains(".")


func _check_scroll(scroll: ScrollContainer, data: Dictionary) -> void:
	var bar := scroll.get_v_scroll_bar()
	if bar == null or not bar.visible:
		return
	if bar.max_value - bar.page <= 0.5:
		return
	data["issues"].append({
		"kind": "scroll_needed",
		"node": str(scroll.get_path()),
		"hidden_pixels": snappedf(bar.max_value - bar.page, 0.1),
	})


func _is_inside(node: Node, root: Node) -> bool:
	var cursor := node
	while cursor != null:
		if cursor == root:
			return true
		cursor = cursor.get_parent()
	return false


func _write_report() -> void:
	var dir := String(_report.get("out", OUT_DIR))
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(dir))
	var path := "%s/layout_report.json" % dir
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		push_error("não foi possível gravar %s" % path)
		return
	file.store_string(JSON.stringify(_report, "  "))
	file.close()
