extends Control

## Captura e auditoria automatizada da interface, sem alterar jogo nem dados.
##
## Instancia o HUD sobre um fundo neutro, percorre os estados principais
## (HUD, quatro painéis, busca sem resultados, detalhe de cidade e menu) em
## várias resoluções, grava PNGs e um relatório objetivo de layout.
##
## Uso:
##   godot --path . res://tools/ui_capture.tscn -- --phase=after \
##         --sizes=1920x1080,1600x900,1366x768,2560x1440
##
## Argumentos: --phase=<rótulo>  --sizes=<WxH,...>  --out=<dir>  --run=<pasta>
## O relatório não afirma inspeção visual: mede retângulos, textos e recortes.

const HudScript: Script = preload("res://core/ui/hud.gd")
const SATELLITE_PATH := "res://map/map_data/satellite.jpg"

const WAIT_FRAMES := 3
const DEFAULT_SIZES := ["1920x1080", "1600x900", "1366x768", "2560x1440"]

## Prefixos de chaves de i18n usados pelo jogo; um rótulo que comece com um
## deles seguido de ponto é uma chave que vazou para a interface.
const I18N_PREFIXES := ["alerts", "city", "climate", "common", "deposit", "dock",
		"econ", "hud", "map", "mode", "month", "pol", "pop", "prod", "product",
		"relief", "resource", "scope", "trade"]

var _phase := "after"
var _out_root := "res://docs/ui/captures"
var _run := ""
var _sizes: Array[String] = []
var _plain := false
var _hud
var _report := {"shots": [], "audits": [], "skipped": [], "phase": ""}


func _ready() -> void:
	_parse_args()
	if _run.is_empty():
		_run = Time.get_datetime_string_from_system().replace(":", "-")
	if _sizes.is_empty():
		_sizes.assign(DEFAULT_SIZES)

	_report["phase"] = _phase
	_report["timestamp"] = Time.get_datetime_string_from_system()
	_report["engine"] = Engine.get_version_info().get("string", "")
	_report["stretch"] = {
		"mode": ProjectSettings.get_setting("display/window/stretch/mode", ""),
		"aspect": ProjectSettings.get_setting("display/window/stretch/aspect", ""),
	}

	_build_background()
	_build_hud()
	await _wait_for_data()

	for size in _sizes:
		await _capture_resolution(size)

	_write_report()
	get_tree().quit()


func _parse_args() -> void:
	for arg in OS.get_cmdline_user_args():
		var key := arg.get_slice("=", 0)
		var value := arg.get_slice("=", 1)
		match key:
			"--phase":
				_phase = value
			"--out":
				_out_root = value
			"--run":
				_run = value
			"--sizes":
				_sizes.assign(value.split(",", false))
			"--plain":
				_plain = value != "0" and value != "false"


## Fundo neutro: textura real do mapa escurecida, ou cor chapada com --plain.
func _build_background() -> void:
	var texture: Texture2D = null if _plain else load(SATELLITE_PATH) as Texture2D
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


func _build_hud() -> void:
	_hud = HudScript.new()
	add_child(_hud)


## O primeiro tick da economia é adiado; espera o resumo existir.
func _wait_for_data() -> void:
	for _frame in 180:
		if not Economy.last_summary.is_empty():
			return
		await get_tree().process_frame
	push_warning("economia não publicou resumo antes da captura")


func _capture_resolution(size_text: String) -> void:
	var parts := size_text.split("x")
	var width := int(parts[0])
	var height := int(parts[1]) if parts.size() > 1 else int(float(width) * 0.5625)

	get_window().size = Vector2i(width, height)
	await get_tree().process_frame
	await get_tree().process_frame

	# Cada resolução começa do HUD limpo, sem menu ou janela herdados.
	_reset_ui()

	var directory := "%s/%s/%s/%dx%d" % [_out_root, _phase, _run, width, height]
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(directory))

	await _capture_state(directory, width, height, "00_hud", Callable())
	await _capture_state(directory, width, height, "01_economy_nations",
			func() -> void: _show_screen("economy"))
	await _capture_state(directory, width, height, "02_economy_cities",
			func() -> void: _open_economy_tab(1))
	if _has_search():
		await _capture_state(directory, width, height, "03_economy_empty",
				func() -> void: _search_economy("zzzz"))
	else:
		_report["skipped"].append("03_economy_empty: HUD sem busca")
	await _capture_state(directory, width, height, "04_trade",
			func() -> void: _show_screen("trade"))
	await _capture_state(directory, width, height, "05_production",
			func() -> void: _show_screen("production"))
	await _capture_state(directory, width, height, "06_politics",
			func() -> void: _show_screen("politics"))
	await _capture_state(directory, width, height, "07_city",
			func() -> void: _show_city(_first_city_id()))
	if _menu_available():
		await _capture_state(directory, width, height, "08_menu", _open_menu)
	else:
		_report["skipped"].append("08_menu: HUD sem menu de pausa")

	# Fluxo incremental: cidade → abas → alertas → produção → cidade produtora → voltar.
	await _capture_state(directory, width, height, "09_city_population",
			func() -> void: _city_tab("population"))
	await _capture_state(directory, width, height, "10_city_production",
			func() -> void: _city_tab("production"))
	await _capture_state(directory, width, height, "11_city_market",
			func() -> void: _city_tab("market"))
	await _capture_state(directory, width, height, "12_city_resources",
			func() -> void: _city_tab("resources"))
	await _capture_state(directory, width, height, "13_alerts",
			func() -> void: _show_screen("alerts"))
	await _capture_state(directory, width, height, "14_production_cities",
			func() -> void: _production_tab("cities"))
	await _capture_state(directory, width, height, "15_city_to_producer",
			_open_producer_city)
	await _capture_state(directory, width, height, "16_back_to_city", _back)
	await _capture_state(directory, width, height, "17_trade_market",
			func() -> void: _trade_market(""))
	await _capture_state(directory, width, height, "18_trade_market_empty",
			func() -> void: _trade_market("zzzz"))
	await _capture_state(directory, width, height, "19_population_nations",
			func() -> void: _population_tab(0))
	await _capture_state(directory, width, height, "20_population_cities",
			func() -> void: _population_tab(1))
	_reset_ui()


## Fecha menu e janela para o próximo estado ou resolução começar neutro.
func _reset_ui() -> void:
	if _menu_available():
		var menu: Control = _hud.get("_menu_layer")
		if menu.visible:
			_hud.toggle_menu()
	_close_window()


func _menu_available() -> bool:
	return _hud.has_method("toggle_menu") and "_menu_layer" in _hud


func _has_search() -> bool:
	var screens: Dictionary = _hud.get("_screens")
	if screens == null or screens.get("economy") == null:
		return false
	return "_search" in screens["economy"] and screens["economy"].get("_search") != null


# --- Adaptadores: o mesmo roteiro serve ao HUD novo e ao anterior ----------

func _show_screen(id: String) -> void:
	if _hud.has_method("open_screen"):
		_hud.open_screen(id)
	else:
		_hud.call("_open_screen", id)


func _show_city(city_id: String) -> void:
	if _hud.has_method("open_city"):
		_hud.open_city(city_id)
	else:
		_hud.call("_on_city_requested", city_id)


func _is_window_open() -> bool:
	if _hud.has_method("is_window_open"):
		return _hud.is_window_open()
	if not "_window_layer" in _hud:
		return false
	var layer: Control = _hud.get("_window_layer")
	return layer != null and layer.visible


func _close_window() -> void:
	if _hud.has_method("close_window"):
		_hud.close_window()
		return
	if not "_window_layer" in _hud:
		return
	var layer: Control = _hud.get("_window_layer")
	if layer != null:
		layer.visible = false


func _capture_state(directory: String, width: int, height: int, state: String,
		action: Callable) -> void:
	if action.is_valid():
		action.call()
	for _frame in WAIT_FRAMES:
		await get_tree().process_frame
	await RenderingServer.frame_post_draw

	var path := "%s/%s.png" % [directory, state]
	var image := get_viewport().get_texture().get_image()
	var error := image.save_png(path)

	_report["shots"].append({
		"resolution": "%dx%d" % [width, height],
		"state": state,
		"path": path,
		"error": error,
	})
	_report["audits"].append(_audit("%dx%d" % [width, height], state))


## Fecha a janela de painéis antes de fotografar o menu de pausa.
func _open_menu() -> void:
	_close_window()
	_hud.toggle_menu()


func _open_economy_tab(index: int) -> void:
	_show_screen("economy")
	var screen: Node = _hud.get("_screens")["economy"]
	screen.call("_set_tab", index)


func _search_economy(text: String) -> void:
	_open_economy_tab(1)
	var screen: Node = _hud.get("_screens")["economy"]
	var field: LineEdit = screen.get("_search")
	field.text = text
	field.text_changed.emit(text)


func _first_city_id() -> String:
	var cities: Array = Economy.last_summary.get("cities", [])
	return cities[0]["id"] if not cities.is_empty() else ""


func _city_tab(tab: String) -> void:
	_show_city(_first_city_id())
	var panel: Node = _hud.get("_city_screen")
	if panel != null and panel.has_method("set_tab"):
		panel.call("set_tab", tab)


func _production_tab(tab: String) -> void:
	_show_screen("production")
	var screen: Node = _hud.get("_screens")["production"]
	if screen != null and screen.has_method("_set_tab"):
		screen.call("_set_tab", tab)


## Simula o clique numa cidade produtora a partir da aba Produção da cidade.
func _open_producer_city() -> void:
	_city_tab("production")
	var panel: Node = _hud.get("_city_screen")
	if panel == null or not panel.has_method("_producers"):
		_report["skipped"].append("15_city_to_producer: painel sem lista de produtores")
		return
	var current := String(panel.call("current_city_id"))
	for city in panel.call("_producers"):
		var id := String(city["id"])
		if id == current:
			continue
		panel.emit_signal("navigate_city", id, "production")
		return
	_report["skipped"].append("15_city_to_producer: nenhuma outra cidade produtora")


func _back() -> void:
	if _hud.has_method("back"):
		_hud.back()


func _trade_market(query: String) -> void:
	_show_screen("trade")
	var screen: Node = _hud.get("_screens")["trade"]
	screen.call("_set_tab", 2)
	screen.call("_on_market_search", query)


func _population_tab(index: int) -> void:
	_show_screen("population")
	var screen: Node = _hud.get("_screens")["population"]
	screen.call("_set_tab", index)


func _context() -> Array:
	if _hud != null and _hud.has_method("current_context"):
		return _hud.current_context()
	return []


# --- Auditoria objetiva de layout -------------------------------------------

func _audit(resolution: String, state: String) -> Dictionary:
	var viewport_size := get_viewport().get_visible_rect().size
	var data := {
		"resolution": resolution,
		"state": state,
		"window": [get_window().size.x, get_window().size.y],
		"viewport": [viewport_size.x, viewport_size.y],
		"window_open": _is_window_open(),
		"context": _context(),
		"focus": "",
		"issues": [],
		"elements": {},
		"labels": [],
	}

	var focus := get_viewport().gui_get_focus_owner()
	if focus != null:
		data["focus"] = str(focus.get_path())
		if not focus.is_visible_in_tree():
			data["issues"].append({"kind": "focus_hidden", "node": data["focus"]})

	_walk(self, viewport_size, data, 0, false)
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
			if depth <= 4 and not control.name.is_empty():
				data["elements"][str(control.get_path())] = {
					"class": control.get_class(),
					"rect": [rect.position.x, rect.position.y, rect.size.x, rect.size.y],
				}
			if control is Label:
				_check_label(control as Label, data)
				var text := (control as Label).text.strip_edges()
				if not text.is_empty() and data["labels"].size() < 40:
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
	if _looks_like_key(label.text.strip_edges()):
		data["issues"].append({
			"kind": "i18n_key",
			"node": str(label.get_path()),
			"text": label.text.strip_edges(),
		})
	if label.text.strip_edges().is_empty() or label.autowrap_mode != TextServer.AUTOWRAP_OFF:
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


## Detecta chaves de i18n vazando como texto (prefixo conhecido + ponto).
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


func _write_report() -> void:
	var path := "%s/%s/%s/layout_report.json" % [_out_root, _phase, _run]
	DirAccess.make_dir_recursive_absolute(
			ProjectSettings.globalize_path(path.get_base_dir()))
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		push_error("não foi possível gravar %s" % path)
		return
	file.store_string(JSON.stringify(_report, "  "))
	file.close()
