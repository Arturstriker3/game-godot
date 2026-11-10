extends CanvasLayer

## HUD principal do WarEconomy — "Industrial Strategy, Premium Dark".
##
## Estrutura: barra superior com nação, calendário, indicadores e velocidade;
## doca lateral de painéis; janela modal com pilha de navegação; menu (Esc).
## O HUD não simula nada: lê `Economy` e desenha. Todo texto vem do `I18n`.

const ECONOMY_SCREEN: Script = preload("res://core/ui/screens/economy_screen.gd")
const TRADE_SCREEN: Script = preload("res://core/ui/screens/trade_screen.gd")
const PRODUCTION_SCREEN: Script = preload("res://core/ui/screens/production_screen.gd")
const POPULATION_SCREEN: Script = preload("res://core/ui/screens/population_screen.gd")
const POLITICS_SCREEN: Script = preload("res://core/ui/screens/politics_screen.gd")
const ALERTS_SCREEN: Script = preload("res://core/ui/screens/alerts_screen.gd")
const ALERTS_MODEL: Script = preload("res://core/ui/alerts.gd")
const CITY_PANEL: Script = preload("res://core/ui/city_panel.gd")

## Contexto pedido pelo HUD para o mapa (câmera e camada de dados).
signal city_focused(city_id: String)
signal map_mode_requested(mode: String)
## Filtro da legenda de recursos: chave -> visível. Vazio = todos.
signal resource_filter_changed(enabled: Dictionary)
## Clique no minimapa: ponto do mapa em UV (0..1).
signal map_point_requested(map_uv: Vector2)

const PLAYER_NATION := "GBR"

## Altura da barra superior (fase 1b: 48 → 64 px, +1/3, para legibilidade).
const TOP_BAR_HEIGHT := 64.0
## Tipografia da barra escalada proporcionalmente à altura (×4/3).
const BAR_NAME_SIZE := 24
const BAR_DATE_SIZE := 17
const BAR_CAPTION_SIZE := 16
const BAR_VALUE_SIZE := 23
const WINDOW_MIN := Vector2(960.0, 560.0)
const WINDOW_MAX := Vector2(1800.0, 1000.0)
const WINDOW_RATIO := Vector2(0.86, 0.84)

const SCREEN_ORDER := ["economy", "trade", "production", "population", "politics"]
const MAX_ORDER_LOG := 240
const SCREEN_LABELS := {
	"economy": "dock.economy",
	"trade": "dock.trade",
	"production": "dock.production",
	"population": "dock.population",
	"politics": "dock.politics",
	"alerts": "hud.alerts",
}
const SCREEN_HINTS := {
	"economy": "econ.hint",
	"trade": "trade.hint",
	"production": "prod.hint",
	"population": "pop.hint",
	"politics": "pol.hint",
	"alerts": "alerts.hint",
}
## Botões de camada do mapa (quadrados, acima do minimapa). Os demais modos
## (população/economia/indústria) continuam no mapa, mas sem botão por enquanto.
const MAP_MODES := ["political", "resources"]
const MODE_ICONS := {
	"political": preload("res://assets/ui/map_mode_political.png"),
	"resources": preload("res://assets/ui/map_mode_resources.png"),
}
## Ícones dos quatro indicadores da barra superior (paleta C). wheat/truck são
## reaproveitados de assets/icons/products/; population/deficit são próprios.
const METRIC_ICONS := {
	"population": preload("res://assets/icons/population.svg"),
	"stock": preload("res://assets/icons/products/wheat.svg"),
	"deficit": preload("res://assets/icons/deficit.svg"),
	"transit": preload("res://assets/icons/products/truck.svg"),
}
## Sino de notificações (branco, tingível) — o badge numérico é desenhado por cima.
const BELL_TEXTURE: Texture2D = preload("res://assets/icons/bell.svg")
## Legenda do mapa de recursos; as cores são as mesmas de
## tools/build_resource_map.py (map/map_data/atlas/resource_points.json).
const RESOURCE_KEYS := ["coal", "copper", "gas", "iron", "oil"]
const RESOURCE_LEGEND := [
	{"key": "coal", "color": Color("#3E3B38")},
	{"key": "copper", "color": Color("#C06A3C")},
	{"key": "gas", "color": Color("#4E9FB0")},
	{"key": "iron", "color": Color("#7C8894")},
	{"key": "oil", "color": Color("#5B4A2E")},
	{"key": "wheat", "color": Color("#D9B45A")},
]
const RESOURCE_POINTS_PATH := "res://map/map_data/atlas/resource_points.json"

var _summary: Dictionary = {}
var _order_log: Array = []
var _last_logged_day := -1
var _active := ""
var _screens := {}
var _stack: Array[Dictionary] = []

var _root: Control
var _top_holder: PanelContainer
var _dock_holder: VBoxContainer
var _window_layer: Control
var _backdrop: ColorRect
var _window: PanelContainer
var _window_crumb: Label
var _window_title: Label
var _window_subtitle: Label
var _window_body: VBoxContainer
var _window_hint: Label
var _back_button: Button
var _city_screen: VBoxContainer
var _alerts_button: Button
var _legend: PanelContainer
var _legend_box: VBoxContainer
var _legend_title: Label
var _legend_hint: Label
var _legend_all: Button
var _legend_labels := {}
var _resource_enabled := {}
var _resource_counts := {}
var _resource_checks := {}
var _resource_rows := {}
var _mode := "political"

var _menu_layer: Control
var _menu_credits: Label
var _menu_fullscreen: Button
var _minimap: Minimap

var _metric_values := {}
var _date_label: Label
var _day_label: Label
var _day_fill: ColorRect
var _speed_buttons: Array[Button] = []
var _dock_buttons := {}

var _date_base := ""
var _last_clock_hour := -1
var _alerts_badge: PanelContainer
var _alerts_badge_label: Label
var _settings_layer: Control
var _settings_buttons := {}


func _ready() -> void:
	layer = 50
	UiKit.set_locale(I18n.locale)

	_build_root()
	_build_top_bar()
	_build_dock()
	_build_window()
	_build_menu()
	_build_settings()
	_build_minimap()
	_build_legend()

	Economy.status_updated.connect(_on_status)
	Economy.speed_changed.connect(func(_index: int) -> void: _update_top_bar())
	I18n.locale_changed.connect(_on_locale_changed)
	get_viewport().size_changed.connect(_apply_window_size)

	_on_status(_initial_summary())
	_apply_window_size()


func _process(_delta: float) -> void:
	if _day_fill != null and is_instance_valid(_day_fill):
		_day_fill.anchor_right = Economy.day_progress()
	_update_clock()


func _unhandled_input(event: InputEvent) -> void:
	if event.is_action_pressed("ui_cancel"):
		if _settings_layer != null and _settings_layer.visible:
			_close_settings()
		elif _menu_layer.visible:
			_close_menu()
		elif _window_layer.visible:
			_close_screen()
		else:
			_open_menu()
		get_viewport().set_input_as_handled()
		return

	if event is InputEventKey and event.pressed and not event.echo \
			and event.physical_keycode == KEY_F11:
		_toggle_fullscreen()
		get_viewport().set_input_as_handled()
		return

	if _window_layer.visible or _menu_layer.visible:
		return
	if event is InputEventKey and event.pressed and not event.echo:
		var key := (event as InputEventKey).keycode
		var index := key - KEY_1
		if index >= 0 and index < SCREEN_ORDER.size():
			_open_screen(SCREEN_ORDER[index])
			get_viewport().set_input_as_handled()


# --- API pública (também usada pelos testes de UI) ---------------------------

func open_screen(screen_id: String) -> void:
	_open_screen(screen_id)


func open_city(city_id: String, tab: String = "overview") -> void:
	_on_city_navigate(city_id, tab)


func open_port(port_name: String) -> void:
	if _root != null and not port_name.is_empty():
		UiKit.toast(_root, "Porto: " + port_name, "neutral")


func toggle_menu() -> void:
	if _menu_layer.visible:
		_close_menu()
	else:
		_open_menu()


func is_window_open() -> bool:
	return _window_layer.visible


func close_window() -> void:
	_close_screen()


# --- Estrutura ---------------------------------------------------------------

func _build_root() -> void:
	_root = Control.new()
	_root.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_root.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_root.theme = UiKit.build_theme()
	add_child(_root)


func _initial_summary() -> Dictionary:
	if not Economy.last_summary.is_empty():
		return Economy.last_summary
	return Economy.sim.get_demo_status()


## Configura o minimapa (título localizado + abas de tipo de mapa) e aplica o
## modo ativo. Rechamado ao trocar idioma para atualizar rótulos e tooltips.
func _configure_minimap() -> void:
	if _minimap == null:
		return
	_minimap.set_title(I18n.t("map.minimap.title"))
	var tooltips := {}
	for mode in MAP_MODES:
		tooltips[mode] = I18n.t("map." + mode)
	_minimap.set_map_modes(MAP_MODES, MODE_ICONS, tooltips, _mode)


## Sincroniza o estado visual do minimapa e da legenda com o modo ativo.
func _refresh_mode_bar() -> void:
	if _minimap != null:
		_minimap.set_active_mode(_mode)
	if _legend != null:
		_legend.visible = _mode == "resources"


## Legenda/filtro do mapa de recursos (canto inferior esquerdo). Cada linha é
## uma caixa de seleção; o mapa e a faixa das cidades reagem na hora.
func _build_legend() -> void:
	_legend = PanelContainer.new()
	_legend.set_anchors_and_offsets_preset(Control.PRESET_BOTTOM_LEFT)
	var padding := 10.0
	_legend.offset_left = UiKit.SPACE_LG
	_legend.offset_right = UiKit.SPACE_LG + 214.0
	_legend.offset_bottom = -UiKit.SPACE_LG
	_legend.offset_top = _legend.offset_bottom - 248.0
	var style := UiKit.surface(Color(0.055, 0.09, 0.085, 0.94), padding)
	style.border_color = UiKit.EDGE
	style.set_border_width_all(1)
	_legend.add_theme_stylebox_override("panel", style)
	_legend.mouse_filter = Control.MOUSE_FILTER_STOP

	_legend_box = VBoxContainer.new()
	_legend_box.add_theme_constant_override("separation", 3)
	_legend.add_child(_legend_box)
	_render_legend_filters()
	_legend.visible = _mode == "resources"
	_root.add_child(_legend)


## Conteúdo padrão da legenda: filtros por recurso + contagem + "Todos".
func _render_legend_filters() -> void:
	if _legend_box == null:
		return
	_clear_legend_content()

	var header := HBoxContainer.new()
	_legend_box.add_child(header)
	_legend_title = UiKit.caption(I18n.t("map.legend.title"))
	_legend_title.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	header.add_child(_legend_title)
	_legend_all = Button.new()
	_legend_all.text = I18n.t("map.legend.all")
	_legend_all.flat = true
	_legend_all.focus_mode = Control.FOCUS_NONE
	_legend_all.tooltip_text = I18n.t("map.legend.all")
	_legend_all.add_theme_font_size_override("font_size", 12)
	_legend_all.add_theme_color_override("font_color", UiKit.BRASS)
	_legend_all.add_theme_color_override("font_hover_color", UiKit.TEXT)
	_legend_all.pressed.connect(_select_all_resources)
	header.add_child(_legend_all)

	if _resource_counts.is_empty():
		_resource_counts = _resource_counts_from_file()
	if _resource_enabled.is_empty():
		for entry in RESOURCE_LEGEND:
			_resource_enabled[String(entry["key"])] = true
	for entry in RESOURCE_LEGEND:
		var key := String(entry["key"])
		var row := _make_resource_row(key, entry["color"])
		_legend_box.add_child(row)
		_resource_rows[key] = row

	_legend_hint = UiKit.caption(I18n.t("map.legend.hint"))
	_legend_hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_legend_hint.custom_minimum_size = Vector2(190.0, 0.0)
	_legend_box.add_child(_legend_hint)

	_refresh_resource_rows()


## Limpa o conteúdo da legenda e zera as referências dos widgets.
func _clear_legend_content() -> void:
	UiKit.clear(_legend_box)
	_legend_labels.clear()
	_resource_checks.clear()
	_resource_rows.clear()
	_legend_title = null
	_legend_hint = null
	_legend_all = null


## Restaura a legenda geral (filtros) no mapa de recursos — usado quando a
## seleção de depósito é limpa (clique no vazio).
func show_general_legend() -> void:
	if _legend_box != null:
		_render_legend_filters()
		_legend.visible = _mode == "resources"


## Cabeçalho da legenda de informação (com "Voltar").
func _legend_header(title: String) -> void:
	var header := HBoxContainer.new()
	_legend_box.add_child(header)
	var label := UiKit.caption(title)
	label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	header.add_child(label)
	var back := Button.new()
	back.text = I18n.t("map.legend.back")
	back.flat = true
	back.focus_mode = Control.FOCUS_NONE
	back.add_theme_font_size_override("font_size", 12)
	back.add_theme_color_override("font_color", UiKit.BRASS)
	back.add_theme_color_override("font_hover_color", UiKit.TEXT)
	back.pressed.connect(_render_legend_filters)
	header.add_child(back)


func _legend_row(color: Color, text: String, detail: String,
		swatch: bool = true, tip: String = "") -> HBoxContainer:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 6.0)
	if swatch:
		var block := ColorRect.new()
		block.custom_minimum_size = Vector2(14.0, 14.0)
		block.color = color
		block.size_flags_vertical = Control.SIZE_SHRINK_CENTER
		block.mouse_filter = Control.MOUSE_FILTER_IGNORE
		row.add_child(block)
	var label := UiKit.label(text, 14, UiKit.TEXT_DIM)
	label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	if not tip.is_empty():
		label.tooltip_text = tip
		label.mouse_filter = Control.MOUSE_FILTER_STOP
	row.add_child(label)
	if not detail.is_empty():
		var value := UiKit.caption(detail)
		value.size_flags_vertical = Control.SIZE_SHRINK_CENTER
		row.add_child(value)
	return row


## Substitui a legenda pelos dados do depósito clicado.
func show_deposit_info(info: Dictionary) -> void:
	if _legend_box == null or info.is_empty():
		return
	_clear_legend_content()
	var resource := String(info.get("resource", ""))
	var title := I18n.t("map.legend.deposit")
	var field := String(info.get("name", ""))
	if not field.is_empty():
		title += " — " + field
	_legend_header(title)
	_legend_box.add_child(_legend_row(_resource_color(resource),
			I18n.t("resource." + resource), ""))
	_legend_box.add_child(_legend_row(Color.TRANSPARENT,
			I18n.t("map.legend.reserve"), _format_mt(float(info.get("quantity_kt", 0.0))),
			false, I18n.t("map.legend.reserve_tip")))
	_legend_box.add_child(_legend_row(Color.TRANSPARENT,
			I18n.t("map.legend.quality"),
			"%d%%" % int(round(float(info.get("quality", 0.0)) * 100.0)),
			false, I18n.t("map.legend.quality_tip")))
	_legend_box.add_child(_legend_row(Color.TRANSPARENT,
			I18n.t("map.legend.depth"), "%d m" % int(info.get("depth_m", 0)),
			false, I18n.t("map.legend.depth_tip")))
	_legend_box.add_child(_legend_row(Color.TRANSPARENT,
			I18n.t("map.legend.status"),
			I18n.t("resource.status." + _status_key(int(info.get("status", 4)))),
			false, I18n.t("map.legend.status_tip")))
	var hint := UiKit.caption(I18n.t("map.legend.deposit_hint"))
	hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	hint.custom_minimum_size = Vector2(190.0, 0.0)
	_legend_box.add_child(hint)
	_legend.visible = true


## Substitui a legenda pelos recursos que a cidade acessa.
func show_city_resources(city_id: String, entries: Array) -> void:
	if _legend_box == null:
		return
	_clear_legend_content()
	var city := _find_city(city_id)
	var title := I18n.t("map.legend.city_resources")
	if not city.is_empty():
		title = I18n.city_name(String(city["id"]), String(city["name"]))
	_legend_header(title)
	if entries.is_empty():
		var empty := UiKit.caption(I18n.t("map.legend.city_empty"))
		empty.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		empty.custom_minimum_size = Vector2(190.0, 0.0)
		_legend_box.add_child(empty)
	else:
		for entry in entries:
			var key := String(entry["kind"])
			var detail := ""
			if key == "wheat":
				detail = "%d%%" % int(round(float(entry.get("value", 0.0)) * 100.0))
			else:
				detail = "×%d · %s" % [
					int(entry.get("count", 1)),
					_format_mt(float(entry.get("reserve_kt", 0.0))),
				]
			_legend_box.add_child(_legend_row(_resource_color(key),
					I18n.t("resource." + key), detail))
	var hint := UiKit.caption(I18n.t("map.legend.city_hint"))
	hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	hint.custom_minimum_size = Vector2(190.0, 0.0)
	_legend_box.add_child(hint)
	_legend.visible = true


func _resource_color(key: String) -> Color:
	for entry in RESOURCE_LEGEND:
		if String(entry["key"]) == key:
			return entry["color"]
	return UiKit.TEXT_DIM


func _status_key(code: int) -> String:
	match code:
		0:
			return "producer"
		1:
			return "past"
		2:
			return "prospect"
		3:
			return "occurrence"
		_:
			return "unknown"


func _format_mt(kt: float) -> String:
	var mt := kt / 1000.0
	return UiKit.number(mt, 1 if mt < 100.0 else 0) + " Mt"


func _make_resource_row(key: String, color: Color) -> Button:
	var button := Button.new()
	button.focus_mode = Control.FOCUS_NONE
	button.custom_minimum_size = Vector2(186.0, 22.0)
	button.add_theme_stylebox_override("normal",
			UiKit.flat(Color(0, 0, 0, 0), Color(0, 0, 0, 0), 0.0))
	button.add_theme_stylebox_override("hover", UiKit.surface(UiKit.PANEL_HOVER, 3.0))
	button.add_theme_stylebox_override("pressed", UiKit.surface(UiKit.PANEL_RAISED, 3.0))
	button.pressed.connect(func() -> void: _toggle_resource(key))

	var row := HBoxContainer.new()
	row.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	row.offset_left = 4.0
	row.offset_right = -4.0
	row.mouse_filter = Control.MOUSE_FILTER_IGNORE
	row.add_theme_constant_override("separation", 6)
	button.add_child(row)

	var check := Panel.new()
	check.custom_minimum_size = Vector2(12.0, 12.0)
	check.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	check.mouse_filter = Control.MOUSE_FILTER_IGNORE
	row.add_child(check)
	_resource_checks[key] = check

	var swatch := ColorRect.new()
	swatch.custom_minimum_size = Vector2(14.0, 14.0)
	swatch.color = color
	swatch.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	swatch.mouse_filter = Control.MOUSE_FILTER_IGNORE
	row.add_child(swatch)

	var label := UiKit.label(I18n.t("resource." + key), 14, UiKit.TEXT_DIM)
	label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(label)
	_legend_labels[key] = label

	var count := UiKit.caption(str(int(_resource_counts.get(key, 0))))
	count.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	row.add_child(count)
	return button


func _resource_counts_from_file() -> Dictionary:
	var counts := {"coal": 0, "copper": 0, "gas": 0, "iron": 0, "oil": 0, "wheat": 0}
	var data: Variant = JSON.parse_string(
			FileAccess.get_file_as_string(RESOURCE_POINTS_PATH))
	if typeof(data) != TYPE_DICTIONARY:
		return counts
	for point in data.get("minerals", []):
		var index := clampi(int(point[2]), 0, RESOURCE_KEYS.size() - 1)
		var key := String(RESOURCE_KEYS[index])
		counts[key] = int(counts[key]) + 1
	counts["wheat"] = (data.get("wheat", []) as Array).size()
	return counts


func _toggle_resource(key: String) -> void:
	_resource_enabled[key] = not bool(_resource_enabled.get(key, true))
	_refresh_resource_rows()
	resource_filter_changed.emit(_resource_enabled.duplicate())


func _select_all_resources() -> void:
	for key in _resource_enabled:
		_resource_enabled[key] = true
	_refresh_resource_rows()
	resource_filter_changed.emit(_resource_enabled.duplicate())


func _refresh_resource_rows() -> void:
	for key in _resource_rows:
		var enabled := bool(_resource_enabled.get(key, true))
		var check: Panel = _resource_checks[key]
		var style := UiKit.flat(
				UiKit.BRASS if enabled else Color(0, 0, 0, 0),
				UiKit.BRASS if enabled else UiKit.EDGE, 0.0)
		style.set_corner_radius_all(2)
		check.add_theme_stylebox_override("panel", style)
		var label: Label = _legend_labels[key]
		label.add_theme_color_override("font_color",
				UiKit.TEXT if enabled else UiKit.TEXT_MUTED)
		(_resource_rows[key] as Button).modulate = Color(1, 1, 1, 1.0 if enabled else 0.72)


## Atualiza os textos da legenda ao trocar de idioma.
func _refresh_legend() -> void:
	if _legend_title != null:
		_legend_title.text = I18n.t("map.legend.title")
	if _legend_hint != null:
		_legend_hint.text = I18n.t("map.legend.hint")
	if _legend_all != null:
		_legend_all.text = I18n.t("map.legend.all")
	for key in _legend_labels:
		(_legend_labels[key] as Label).text = I18n.t("resource." + String(key))


func _set_map_mode(mode: String) -> void:
	if not MAP_MODES.has(mode):
		return
	_mode = mode
	_refresh_mode_bar()
	if mode == "resources" and _legend_box != null:
		_render_legend_filters()
	map_mode_requested.emit(mode)


func set_map_mode(mode: String) -> void:
	_set_map_mode(mode)


func map_mode() -> String:
	return _mode


# --- Minimapa ----------------------------------------------------------------

func _build_minimap() -> void:
	_minimap = Minimap.new()
	_minimap.set_anchors_and_offsets_preset(Control.PRESET_BOTTOM_RIGHT)
	_root.add_child(_minimap)
	var full := Minimap.total_size()
	_minimap.offset_right = -UiKit.SPACE_LG
	_minimap.offset_bottom = -UiKit.SPACE_XL
	_minimap.offset_left = _minimap.offset_right - full.x
	_minimap.offset_top = _minimap.offset_bottom - full.y
	_minimap.point_requested.connect(func(uv: Vector2) -> void: map_point_requested.emit(uv))
	_minimap.mode_requested.connect(_set_map_mode)
	_configure_minimap()


## Área vista pela câmera, em UV do mapa (chamado por main_game.gd).
func set_minimap_view(center_uv: Vector2, size_uv: Vector2) -> void:
	if _minimap != null:
		_minimap.set_view(center_uv, size_uv)


## Textura do território do minimapa (dono por província; main_game.gd).
func set_minimap_territory(texture: Texture2D) -> void:
	if _minimap != null:
		_minimap.set_territory_texture(texture)


# --- Barra superior ----------------------------------------------------------

func _build_top_bar() -> void:
	_top_holder = PanelContainer.new()
	_top_holder.set_anchors_and_offsets_preset(Control.PRESET_TOP_WIDE)
	_top_holder.offset_top = 0.0
	_top_holder.offset_bottom = TOP_BAR_HEIGHT
	_top_holder.mouse_filter = Control.MOUSE_FILTER_STOP

	# Sem borda nem arredondamento: a barra encosta em y=0 sem fresta do mapa.
	var style := StyleBoxFlat.new()
	style.bg_color = UiKit.BARRA_FUNDO
	style.set_border_width_all(0)
	style.set_corner_radius_all(0)
	style.content_margin_left = UiKit.SPACE_MD
	style.content_margin_right = UiKit.SPACE_MD
	style.content_margin_top = 0.0
	style.content_margin_bottom = 0.0
	_top_holder.add_theme_stylebox_override("panel", style)
	_root.add_child(_top_holder)

	# Borda inferior dupla (filete): duas linhas de 1 px com vão de 2 px.
	var filete_top := ColorRect.new()
	filete_top.color = UiKit.BARRA_FILETE
	filete_top.mouse_filter = Control.MOUSE_FILTER_IGNORE
	filete_top.set_anchors_and_offsets_preset(Control.PRESET_TOP_WIDE)
	filete_top.offset_top = TOP_BAR_HEIGHT - 4.0
	filete_top.offset_bottom = TOP_BAR_HEIGHT - 3.0
	_root.add_child(filete_top)

	var filete_bottom := ColorRect.new()
	filete_bottom.color = UiKit.BARRA_FILETE
	filete_bottom.mouse_filter = Control.MOUSE_FILTER_IGNORE
	filete_bottom.set_anchors_and_offsets_preset(Control.PRESET_TOP_WIDE)
	filete_bottom.offset_top = TOP_BAR_HEIGHT - 1.0
	filete_bottom.offset_bottom = TOP_BAR_HEIGHT
	_root.add_child(filete_bottom)

	_render_top_bar()


func _render_top_bar() -> void:
	UiKit.clear(_top_holder)
	_metric_values.clear()
	_speed_buttons.clear()

	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", UiKit.SPACE_LG)
	_top_holder.add_child(row)

	row.add_child(_nation_block())
	row.add_child(_vertical_rule())
	row.add_child(_clock_block())
	row.add_child(UiKit.spacer())

	_metric_values["population"] = _metric_block(row, "hud.population", "hud.tip.population", METRIC_ICONS["population"])
	row.add_child(_vertical_rule())
	_metric_values["stock"] = _metric_block(row, "hud.stock", "hud.tip.stock", METRIC_ICONS["stock"])
	row.add_child(_vertical_rule())
	_metric_values["deficit"] = _metric_block(row, "hud.deficit", "hud.tip.deficit", METRIC_ICONS["deficit"])
	row.add_child(_vertical_rule())
	_metric_values["transit"] = _metric_block(row, "hud.transit", "hud.tip.transit", METRIC_ICONS["transit"])
	row.add_child(_vertical_rule())

	row.add_child(_speed_block())

	_alerts_button = _bell_button()
	_alerts_button.pressed.connect(func() -> void: _open_screen("alerts"))
	row.add_child(_alerts_button)

	var menu := _bar_button("≡", I18n.t("hud.menu"))
	menu.pressed.connect(_open_menu)
	row.add_child(menu)

	_update_top_bar()


func _nation_block() -> Control:
	var box := HBoxContainer.new()
	box.add_theme_constant_override("separation", UiKit.SPACE_SM + 2)
	box.size_flags_vertical = Control.SIZE_SHRINK_CENTER

	var swatch := ColorRect.new()
	swatch.color = UiKit.nation_color(PLAYER_NATION)
	swatch.custom_minimum_size = Vector2(4.0, 40.0)
	swatch.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	swatch.mouse_filter = Control.MOUSE_FILTER_IGNORE
	box.add_child(swatch)

	var flag := TextureRect.new()
	flag.texture = Economy.flag_texture(PLAYER_NATION)
	flag.custom_minimum_size = Vector2(40.0, 27.0)
	flag.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	flag.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	flag.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	flag.mouse_filter = Control.MOUSE_FILTER_IGNORE
	box.add_child(flag)

	var text := VBoxContainer.new()
	text.add_theme_constant_override("separation", 0)
	text.size_flags_vertical = Control.SIZE_SHRINK_CENTER

	var name_label := UiKit.label(I18n.nation_name(PLAYER_NATION), BAR_NAME_SIZE, UiKit.BARRA_TEXTO)
	name_label.add_theme_font_override("font", UiKit.PAPER_DISPLAY_FONT)
	name_label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	text.add_child(name_label)

	box.add_child(text)
	return box


func _clock_block() -> Control:
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 1)
	box.size_flags_vertical = Control.SIZE_SHRINK_CENTER

	_date_label = UiKit.label("", BAR_DATE_SIZE, UiKit.BARRA_FILETE)
	_date_label.add_theme_font_override("font", UiKit.PAPER_BODY_FONT)
	_date_label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	box.add_child(_date_label)

	var bottom := HBoxContainer.new()
	bottom.add_theme_constant_override("separation", UiKit.SPACE_SM + 2)

	_day_label = _metric_caption("")
	bottom.add_child(_day_label)
	bottom.add_child(_day_progress_bar())
	box.add_child(bottom)

	return box


func _day_progress_bar() -> Control:
	var track := Control.new()
	track.custom_minimum_size = Vector2(128.0, 4.0)
	track.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	track.mouse_filter = Control.MOUSE_FILTER_IGNORE

	var background := ColorRect.new()
	background.color = Color(UiKit.BARRA_FILETE, 0.25)
	background.mouse_filter = Control.MOUSE_FILTER_IGNORE
	background.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	track.add_child(background)

	_day_fill = ColorRect.new()
	_day_fill.color = UiKit.BARRA_FILETE
	_day_fill.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_day_fill.anchor_right = 0.0
	_day_fill.anchor_bottom = 1.0
	_day_fill.offset_right = 0.0
	_day_fill.offset_bottom = 0.0
	track.add_child(_day_fill)

	return track


## Indicador da barra: ícone tingido, rótulo pequeno e valor em destaque.
func _metric_block(row: HBoxContainer, key: String, tip_key: String,
		icon: Texture2D) -> Label:
	var box := HBoxContainer.new()
	box.add_theme_constant_override("separation", UiKit.SPACE_SM)
	box.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	box.tooltip_text = I18n.t(tip_key)

	var glyph := TextureRect.new()
	glyph.texture = icon
	glyph.custom_minimum_size = Vector2(24.0, 24.0)
	glyph.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	glyph.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	glyph.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	glyph.mouse_filter = Control.MOUSE_FILTER_IGNORE
	glyph.modulate = UiKit.BARRA_ROTULO
	box.add_child(glyph)

	var text := VBoxContainer.new()
	text.add_theme_constant_override("separation", 0)
	text.size_flags_vertical = Control.SIZE_SHRINK_CENTER

	text.add_child(_metric_caption(I18n.t(key)))
	var value := UiKit.label("0", BAR_VALUE_SIZE, UiKit.BARRA_TEXTO)
	value.add_theme_font_override("font", UiKit.PAPER_BODY_FONT)
	value.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	text.add_child(value)
	box.add_child(text)

	row.add_child(box)
	return value


## Rótulo pequeno da barra superior: Special Elite, sem caixa alta.
func _metric_caption(text: String) -> Label:
	var node := UiKit.label(text, BAR_CAPTION_SIZE, UiKit.BARRA_ROTULO)
	node.add_theme_font_override("font", UiKit.PAPER_BODY_FONT)
	return node


func _vertical_rule() -> ColorRect:
	var node := ColorRect.new()
	node.color = Color(UiKit.BARRA_FILETE, 0.35)
	node.custom_minimum_size = Vector2(1.0, 35.0)
	node.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	node.mouse_filter = Control.MOUSE_FILTER_IGNORE
	return node


func _speed_block() -> Control:
	var box := HBoxContainer.new()
	box.add_theme_constant_override("separation", 2)
	box.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	box.tooltip_text = I18n.t("hud.speed")

	for index in Economy.SPEED_COUNT:
		var button := Button.new()
		# Sem foco: senão o espaço (ui_accept) reativa o botão em vez de pausar.
		button.focus_mode = Control.FOCUS_NONE
		button.custom_minimum_size = Vector2(45.0, 37.0)
		button.size_flags_vertical = Control.SIZE_SHRINK_CENTER
		button.add_theme_font_override("font", UiKit.PAPER_BODY_FONT)
		button.add_theme_font_size_override("font_size", BAR_CAPTION_SIZE)
		button.tooltip_text = I18n.t("hud.paused") if index == 0 else "%dx" % index
		button.pressed.connect(_on_speed_pressed.bind(index))
		box.add_child(button)
		_speed_buttons.append(button)

	return box


## Estilo dos botões de velocidade: borda fina barra_filete; ativo preenchido
## barra_filete com texto barra_fundo.
func _speed_button_style(button: Button, active: bool) -> void:
	if active:
		button.add_theme_stylebox_override("normal",
				UiKit.flat(UiKit.BARRA_FILETE, UiKit.BARRA_FILETE, 4.0))
		button.add_theme_stylebox_override("hover",
				UiKit.flat(UiKit.BARRA_FILETE, UiKit.BARRA_FILETE, 4.0))
		button.add_theme_stylebox_override("pressed",
				UiKit.flat(UiKit.BARRA_FILETE, UiKit.BARRA_FILETE, 4.0))
		button.add_theme_stylebox_override("focus", UiKit.outline(UiKit.BARRA_FUNDO))
		button.add_theme_color_override("font_color", UiKit.BARRA_FUNDO)
		button.add_theme_color_override("font_hover_color", UiKit.BARRA_FUNDO)
		button.add_theme_color_override("font_pressed_color", UiKit.BARRA_FUNDO)
	else:
		button.add_theme_stylebox_override("normal",
				UiKit.flat(Color(0, 0, 0, 0), UiKit.BARRA_FILETE, 4.0))
		button.add_theme_stylebox_override("hover",
				UiKit.flat(Color(UiKit.BARRA_FILETE, 0.16), UiKit.BARRA_FILETE, 4.0))
		button.add_theme_stylebox_override("pressed",
				UiKit.flat(Color(UiKit.BARRA_FILETE, 0.30), UiKit.BARRA_FILETE, 4.0))
		button.add_theme_stylebox_override("focus", UiKit.outline(UiKit.BARRA_FILETE))
		button.add_theme_color_override("font_color", UiKit.BARRA_ROTULO)
		button.add_theme_color_override("font_hover_color", UiKit.BARRA_TEXTO)


## Botão da barra no mesmo estilo dos botões de velocidade (borda BARRA_FILETE).
func _bar_button(glyph: String, tooltip_text: String) -> Button:
	var button := Button.new()
	button.text = glyph
	button.focus_mode = Control.FOCUS_NONE
	button.custom_minimum_size = Vector2(43.0, 40.0)
	button.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	button.add_theme_font_override("font", UiKit.PAPER_BODY_FONT)
	button.add_theme_font_size_override("font_size", BAR_CAPTION_SIZE)
	button.tooltip_text = tooltip_text
	_speed_button_style(button, false)
	return button


## Sino de notificações: ícone SVG branco tingido + badge numérico vermelho.
func _bell_button() -> Button:
	var button := _bar_button("", I18n.t("hud.alerts"))

	var glyph := TextureRect.new()
	glyph.texture = BELL_TEXTURE
	glyph.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	glyph.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	glyph.mouse_filter = Control.MOUSE_FILTER_IGNORE
	glyph.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	glyph.offset_left = 10.0
	glyph.offset_top = 9.0
	glyph.offset_right = -10.0
	glyph.offset_bottom = -9.0
	glyph.modulate = UiKit.BARRA_ROTULO
	button.add_child(glyph)

	_alerts_badge = PanelContainer.new()
	var badge_style := UiKit.flat(UiKit.CARIMBO_BARRA, UiKit.CARIMBO_BARRA, 0.0)
	badge_style.set_corner_radius_all(9)
	_alerts_badge.add_theme_stylebox_override("panel", badge_style)
	_alerts_badge.set_anchors_and_offsets_preset(Control.PRESET_TOP_RIGHT)
	_alerts_badge.offset_left = -18.0
	_alerts_badge.offset_top = -6.0
	_alerts_badge.offset_right = 0.0
	_alerts_badge.offset_bottom = 12.0
	_alerts_badge.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_alerts_badge.visible = false
	button.add_child(_alerts_badge)

	_alerts_badge_label = UiKit.label("0", 12, Color.WHITE)
	_alerts_badge_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	_alerts_badge_label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	_alerts_badge_label.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_alerts_badge.add_child(_alerts_badge_label)

	return button


func _update_top_bar() -> void:
	if _date_label == null:
		return

	var date := Economy.game_date()
	_date_base = "%d %s %d" % [
		int(date["day"]), I18n.t("month.%d" % int(date["month"])), int(date["year"]),
	]
	_last_clock_hour = -1
	_update_clock()
	_day_label.text = I18n.t("hud.day", {"day": UiKit.number(float(_summary.get("day", 0)))})
	var paused := Economy.speed == 0
	if paused:
		_day_label.text = I18n.t("hud.paused")
	_day_label.add_theme_color_override("font_color",
			UiKit.WARNING if paused else UiKit.BARRA_ROTULO)
	var cities: Array = _summary.get("cities", [])
	var population := 0.0
	var stock := 0.0
	var deficit := 0.0
	var transit := 0.0
	for city in cities:
		population += float(city["population"])
		stock += float(city["stockpile"])
		deficit += float(city["deficit"])
		transit += float(city["in_transit"])

	_set_metric("population", UiKit.number(population / 1_000_000.0, 1) + " M")
	_set_metric("stock", UiKit.number(stock) + " t")
	_set_metric("deficit", UiKit.number(deficit) + " t",
			UiKit.CARIMBO_BARRA if deficit > 0.0 else UiKit.POSITIVO)
	_set_metric("transit", UiKit.number(transit) + " t", UiKit.BARRA_TEXTO)

	for index in _speed_buttons.size():
		_speed_buttons[index].text = _speed_label(index)
		_speed_button_style(_speed_buttons[index], index == Economy.speed)

	if _alerts_button != null:
		var alerts := _alerts()
		var count := alerts.size()
		if _alerts_badge != null:
			_alerts_badge.visible = count > 0
		if _alerts_badge_label != null:
			_alerts_badge_label.text = str(count)
		_alerts_button.tooltip_text = I18n.t("hud.alerts") if count == 0 \
				else I18n.t("alerts.count", {"count": count})


func _speed_label(index: int) -> String:
	return "II" if index == 0 else "%dx" % index


## Hora derivada da fração do dia da barra de progresso (0..1): HH:00.
func _update_clock() -> void:
	if _date_label == null or _date_base.is_empty():
		return
	var hour := int(floor(Economy.day_progress() * 24.0)) % 24
	if hour == _last_clock_hour:
		return
	_last_clock_hour = hour
	_date_label.text = "%s · %02d:00" % [_date_base, hour]


func _set_metric(key: String, text: String, color: Color = UiKit.TEXT) -> void:
	if not _metric_values.has(key):
		return
	var label: Label = _metric_values[key]
	label.text = text
	label.add_theme_color_override("font_color", color)


# --- Doca --------------------------------------------------------------------

func _build_dock() -> void:
	_dock_holder = VBoxContainer.new()
	_dock_holder.set_anchors_and_offsets_preset(Control.PRESET_TOP_RIGHT)
	_dock_holder.offset_left = -(UiKit.DOCK_WIDTH + 10.0)
	_dock_holder.offset_right = -10.0
	_dock_holder.offset_top = TOP_BAR_HEIGHT + 10.0
	_dock_holder.offset_bottom = TOP_BAR_HEIGHT + 10.0 \
			+ SCREEN_ORDER.size() * (UiKit.DOCK_BUTTON_HEIGHT + 6.0)
	_dock_holder.add_theme_constant_override("separation", 6)
	_root.add_child(_dock_holder)

	_render_dock()


func _render_dock() -> void:
	UiKit.clear(_dock_holder)
	_dock_buttons.clear()

	for index in SCREEN_ORDER.size():
		var id: String = SCREEN_ORDER[index]
		var button := Button.new()
		button.focus_mode = Control.FOCUS_NONE
		button.text = I18n.t(SCREEN_LABELS[id])
		button.custom_minimum_size.y = UiKit.DOCK_BUTTON_HEIGHT
		button.add_theme_font_size_override("font_size", UiKit.FS_SMALL)
		button.tooltip_text = "%d · %s" % [index + 1, I18n.t(SCREEN_LABELS[id])]
		button.pressed.connect(_toggle_screen.bind(id))
		_dock_holder.add_child(button)
		_dock_buttons[id] = button

	_refresh_dock_state()


func _refresh_dock_state() -> void:
	var root_screen := ""
	for entry in _stack:
		if String(entry["kind"]) == "screen":
			root_screen = String(entry["id"])
			break
	for id in _dock_buttons:
		UiKit.set_tab_active(_dock_buttons[id], id == root_screen)


# --- Janela ------------------------------------------------------------------

func _build_window() -> void:
	_window_layer = Control.new()
	_window_layer.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_window_layer.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_window_layer.visible = false
	_root.add_child(_window_layer)

	_backdrop = ColorRect.new()
	_backdrop.color = UiKit.OVERLAY
	_backdrop.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_backdrop.mouse_filter = Control.MOUSE_FILTER_STOP
	_backdrop.gui_input.connect(_on_backdrop_input)
	_window_layer.add_child(_backdrop)

	var center := CenterContainer.new()
	center.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	center.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_window_layer.add_child(center)

	_window = PanelContainer.new()
	_window.custom_minimum_size = WINDOW_MIN
	_window.mouse_filter = Control.MOUSE_FILTER_STOP
	var style := UiKit.flat(UiKit.PANEL, UiKit.EDGE, 20.0)
	style.content_margin_top = UiKit.SPACE_LG
	style.content_margin_bottom = UiKit.SPACE_MD
	_window.add_theme_stylebox_override("panel", style)
	center.add_child(_window)

	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", UiKit.SPACE_MD)
	_window.add_child(box)

	box.add_child(_window_header())
	box.add_child(UiKit.rule(Color(UiKit.BRASS, 0.35)))

	_window_body = VBoxContainer.new()
	_window_body.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_window_body.add_theme_constant_override("separation", UiKit.SPACE_MD)
	box.add_child(_window_body)

	box.add_child(UiKit.rule())
	box.add_child(_window_footer())

	_instantiate_screens()


func _window_header() -> Control:
	var header := HBoxContainer.new()
	header.add_theme_constant_override("separation", UiKit.SPACE_SM)

	_back_button = UiKit.ghost_button("‹ " + I18n.t("common.back"))
	_back_button.custom_minimum_size.y = 26.0
	_back_button.visible = false
	_back_button.pressed.connect(_go_back)
	header.add_child(_back_button)

	var titles := VBoxContainer.new()
	titles.add_theme_constant_override("separation", 0)
	titles.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	_window_crumb = UiKit.caption("", UiKit.BRASS)
	titles.add_child(_window_crumb)
	_window_title = UiKit.title("", 22)
	titles.add_child(_window_title)
	_window_subtitle = UiKit.caption("", UiKit.TEXT_MUTED)
	titles.add_child(_window_subtitle)
	header.add_child(titles)

	header.add_child(UiKit.spacer())

	_window_hint = UiKit.label("", UiKit.FS_CAPTION + 1, UiKit.TEXT_MUTED)
	_window_hint.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	header.add_child(_window_hint)

	var close := UiKit.icon_button("×", I18n.t("common.close"))
	close.pressed.connect(_close_screen)
	header.add_child(close)

	return header


func _window_footer() -> Control:
	var footer := HBoxContainer.new()
	footer.add_theme_constant_override("separation", UiKit.SPACE_SM)

	var shortcuts := UiKit.label(I18n.t("hud.shortcuts"), UiKit.FS_CAPTION, UiKit.TEXT_MUTED)
	footer.add_child(shortcuts)
	footer.add_child(UiKit.spacer())

	var esc := UiKit.status_pill("ESC", "neutral")
	footer.add_child(esc)
	return footer


func _instantiate_screens() -> void:
	for id in SCREEN_ORDER:
		var screen: Node = _new_screen(id)
		screen.visible = false
		screen.size_flags_vertical = Control.SIZE_EXPAND_FILL
		_window_body.add_child(screen)
		_screens[id] = screen

	_screens["economy"].city_requested.connect(_on_city_requested)
	_screens["trade"].city_requested.connect(_on_city_market)
	_screens["production"].city_requested.connect(_on_city_production)
	_screens["population"].city_requested.connect(_on_city_population)

	var alerts_screen: Node = ALERTS_SCREEN.new()
	alerts_screen.visible = false
	alerts_screen.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_window_body.add_child(alerts_screen)
	_screens["alerts"] = alerts_screen
	alerts_screen.alert_requested.connect(_on_city_requested)

	_city_screen = CITY_PANEL.new()
	_city_screen.visible = false
	_city_screen.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_city_screen.navigate_city.connect(_on_city_navigate)
	_window_body.add_child(_city_screen)


func _new_screen(id: String) -> Node:
	match id:
		"economy":
			return ECONOMY_SCREEN.new()
		"trade":
			return TRADE_SCREEN.new()
		"production":
			return PRODUCTION_SCREEN.new()
		"population":
			return POPULATION_SCREEN.new()
		"alerts":
			return ALERTS_SCREEN.new()
		_:
			return POLITICS_SCREEN.new()


func _apply_window_size() -> void:
	if _window == null:
		return
	var viewport_size := get_viewport().get_visible_rect().size
	_window.custom_minimum_size = Vector2(
		clampf(viewport_size.x * WINDOW_RATIO.x, WINDOW_MIN.x, WINDOW_MAX.x),
		clampf(viewport_size.y * WINDOW_RATIO.y, WINDOW_MIN.y, WINDOW_MAX.y))


func _toggle_screen(id: String) -> void:
	if _active == id and _window_layer.visible and _stack.size() <= 1:
		_close_screen()
		return
	_open_screen(id)


## Pilha de contexto: raiz = painel; cidades e cadeias são empilhadas por cima.
func _open_screen(id: String) -> void:
	_stack.clear()
	_stack.append({"kind": "screen", "id": id})
	_show_top()


func _open_city(city_id: String, tab: String) -> void:
	if not _stack.is_empty() and String(_stack.back()["kind"]) == "city" \
			and String(_stack.back()["id"]) == city_id:
		_stack.back()["tab"] = tab
		_show_top()
		return
	_stack.append({"kind": "city", "id": city_id, "tab": tab})
	_show_top()


func _show_top() -> void:
	if _stack.is_empty():
		_close_screen()
		return

	var was_open := _window_layer.visible
	var entry: Dictionary = _stack.back()
	var kind := String(entry["kind"])
	var id := String(entry["id"])
	_active = id if kind == "screen" else "city"

	_city_screen.visible = false
	for screen_id in _screens:
		_screens[screen_id].visible = false

	if kind == "screen":
		_screens[id].visible = true
		_screens[id].refresh(_summary)
		_window_title.text = I18n.t(SCREEN_LABELS[id])
		_window_subtitle.text = _screen_subtitle(id)
		_window_hint.text = I18n.t(SCREEN_HINTS[id])
	else:
		var city := _find_city(id)
		_city_screen.visible = true
		_city_screen.open_city(id, _summary)
		if not entry.has("tab") or String(entry["tab"]).is_empty():
			entry["tab"] = _city_screen.current_tab()
		_city_screen.set_tab(String(entry["tab"]))
		_window_title.text = I18n.t("city.title") if city.is_empty() \
				else I18n.city_name(city["id"], city["name"])
		_window_subtitle.text = I18n.t("hud.player_nation") if city.is_empty() \
				else I18n.nation_name(city["nation"])
		_window_hint.text = I18n.t("city.hint")
		city_focused.emit(id)

	_back_button.visible = _stack.size() > 1
	_render_crumb()
	_window_layer.visible = true
	_apply_window_size()
	_refresh_dock_state()
	UiKit.focus_first(_window_body)
	if get_viewport().gui_get_focus_owner() == null:
		UiKit.focus_first(_window)
	UiSfx.play(UiSfx.Sound.OPEN if was_open else UiSfx.Sound.MAXIMIZE)


func _render_crumb() -> void:
	var parts := PackedStringArray()
	for entry in _stack:
		if String(entry["kind"]) == "screen":
			parts.append(I18n.t(SCREEN_LABELS[String(entry["id"])]))
		else:
			var city := _find_city(String(entry["id"]))
			parts.append(I18n.t("city.title") if city.is_empty() \
					else I18n.city_name(city["id"], city["name"]))
	_window_crumb.text = " › ".join(parts)


## Volta um nível; se já está na raiz, fecha a janela.
func _go_back() -> void:
	if _stack.size() <= 1:
		_close_screen()
		return
	UiSfx.play(UiSfx.Sound.BACK)
	_stack.pop_back()
	_show_top()


func back() -> void:
	_go_back()


func current_context() -> Array:
	return _stack.duplicate(true)


func _screen_subtitle(id: String) -> String:
	var cities: Array = _summary.get("cities", [])
	match id:
		"economy":
			return I18n.t("econ.summary", {
				"cities": UiKit.number(cities.size()),
				"deficit": UiKit.number(_deficit_count(cities)),
				"transit": UiKit.number(_transit_total(cities)) + " t",
			})
		"trade":
			return I18n.t("trade.subtitle")
		"production":
			return I18n.t("dock.production")
		"population":
			var population := 0.0
			for city in cities:
				population += float(city["population"])
			return I18n.t("hud.population") + " " \
					+ UiKit.number(population / 1_000_000.0, 1) + " M"
		"alerts":
			return I18n.t("alerts.subtitle", {"count": _alerts().size()})
		_:
			return I18n.t("dock.politics")


func _deficit_count(cities: Array) -> int:
	var total := 0
	for city in cities:
		if float(city["deficit"]) > 0.0:
			total += 1
	return total


func _alerts() -> Array:
	return ALERTS_MODEL.derive(_summary)


func _transit_total(cities: Array) -> float:
	var total := 0.0
	for city in cities:
		total += float(city["in_transit"])
	return total


func _close_screen() -> void:
	if _window_layer.visible:
		UiSfx.play(UiSfx.Sound.MINIMIZE)
	_active = ""
	_stack.clear()
	_window_layer.visible = false
	_city_screen.visible = false
	_back_button.visible = false
	_refresh_dock_state()


func _on_backdrop_input(event: InputEvent) -> void:
	if event is InputEventMouseButton and event.button_index == MOUSE_BUTTON_LEFT \
			and event.pressed:
		_close_screen()


# --- Detalhe da cidade --------------------------------------------------------

func _on_city_requested(city_id: String) -> void:
	_on_city_navigate(city_id, "overview")


func _on_city_navigate(city_id: String, tab: String) -> void:
	# Se a janela já está aberta em outra tela/cidade, empilha o novo contexto.
	_open_city(city_id, tab)


func _on_city_production(city_id: String) -> void:
	_on_city_navigate(city_id, "production")


func _on_city_market(city_id: String) -> void:
	_on_city_navigate(city_id, "market")


func _on_city_population(city_id: String) -> void:
	_on_city_navigate(city_id, "population")


func _find_city(city_id: String) -> Dictionary:
	for city in _summary.get("cities", []):
		if city["id"] == city_id:
			return city
	return {}


# --- Menu (Esc) ---------------------------------------------------------------

func _build_menu() -> void:
	_menu_layer = Control.new()
	_menu_layer.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_menu_layer.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_menu_layer.visible = false
	_root.add_child(_menu_layer)

	_render_menu_card()


## Conteúdo do menu: mantido separado para a troca de idioma recriar só ele.
func _render_menu_card() -> void:
	var backdrop := ColorRect.new()
	backdrop.color = UiKit.OVERLAY
	backdrop.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	backdrop.mouse_filter = Control.MOUSE_FILTER_STOP
	backdrop.gui_input.connect(func(event: InputEvent) -> void:
		if event is InputEventMouseButton and event.button_index == MOUSE_BUTTON_LEFT \
				and event.pressed:
			_close_menu())
	_menu_layer.add_child(backdrop)

	var center := CenterContainer.new()
	center.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	center.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_menu_layer.add_child(center)

	var panel := UiKit.card(UiKit.SPACE_XL)
	panel.custom_minimum_size.x = 460.0
	panel.mouse_filter = Control.MOUSE_FILTER_STOP
	center.add_child(panel)

	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", UiKit.SPACE_MD)
	panel.add_child(box)

	box.add_child(UiKit.title("WarEconomy", UiKit.FS_DISPLAY))
	box.add_child(UiKit.caption(I18n.t("hud.subtitle"), UiKit.TEXT_MUTED))
	box.add_child(UiKit.rule(Color(UiKit.BRASS, 0.35)))

	var resume := UiKit.primary_button(I18n.t("hud.menu.resume"))
	resume.pressed.connect(_close_menu)
	box.add_child(resume)

	var credits := UiKit.secondary_button(I18n.t("hud.menu.credits"))
	credits.pressed.connect(_toggle_credits)
	box.add_child(credits)

	var settings := UiKit.secondary_button(I18n.t("hud.menu.settings"))
	settings.pressed.connect(_open_settings)
	box.add_child(settings)

	_menu_fullscreen = UiKit.secondary_button("")
	_menu_fullscreen.pressed.connect(_toggle_fullscreen)
	box.add_child(_menu_fullscreen)
	_refresh_fullscreen_button()

	_menu_credits = UiKit.paragraph(I18n.t("hud.credits.body"), UiKit.FS_SMALL,
			UiKit.TEXT_MUTED, 400.0)
	_menu_credits.visible = false
	box.add_child(_menu_credits)

	var quit := UiKit.ghost_button(I18n.t("hud.menu.quit"))
	quit.pressed.connect(func() -> void: get_tree().quit())
	box.add_child(quit)

	box.add_child(UiKit.rule())
	box.add_child(UiKit.caption(I18n.t("hud.shortcuts"), UiKit.TEXT_MUTED))


func _open_menu() -> void:
	_menu_layer.visible = true
	if _menu_credits != null:
		_menu_credits.visible = false
	UiKit.focus_first(_menu_layer)
	UiSfx.play(UiSfx.Sound.OPEN)


func _close_menu() -> void:
	if _menu_layer.visible:
		UiSfx.play(UiSfx.Sound.CLOSE)
	_menu_layer.visible = false


func _toggle_credits() -> void:
	_menu_credits.visible = not _menu_credits.visible


func _is_fullscreen() -> bool:
	return DisplayServer.window_get_mode() == DisplayServer.WINDOW_MODE_FULLSCREEN


func _toggle_fullscreen() -> void:
	if _is_fullscreen():
		DisplayServer.window_set_mode(DisplayServer.WINDOW_MODE_WINDOWED)
	else:
		DisplayServer.window_set_mode(DisplayServer.WINDOW_MODE_FULLSCREEN)
		# Garante que a janela fique em primeiro plano e com foco, para a
		# barra de tarefas do Windows não ficar por cima.
		DisplayServer.window_move_to_foreground()
		get_window().grab_focus()
	_refresh_fullscreen_button()
	UiSfx.play(UiSfx.Sound.TOGGLE)


func _refresh_fullscreen_button() -> void:
	if _menu_fullscreen == null:
		return
	_menu_fullscreen.text = I18n.t(
		"hud.menu.fullscreen.on" if _is_fullscreen() else "hud.menu.fullscreen.off")


# --- Configurações (painel de papel sobre o menu) ------------------------------

func _build_settings() -> void:
	_settings_layer = Control.new()
	_settings_layer.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_settings_layer.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_settings_layer.visible = false
	_root.add_child(_settings_layer)
	_render_settings_card()


## Conteúdo do painel: ficha de papel com título e três botões de idioma.
## Recriado ao trocar idioma para atualizar os rótulos e o destaque.
func _render_settings_card() -> void:
	UiKit.clear(_settings_layer)
	_settings_buttons.clear()

	var backdrop := ColorRect.new()
	backdrop.color = UiKit.OVERLAY
	backdrop.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	backdrop.mouse_filter = Control.MOUSE_FILTER_STOP
	backdrop.gui_input.connect(_on_settings_backdrop_input)
	_settings_layer.add_child(backdrop)

	var center := CenterContainer.new()
	center.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	center.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_settings_layer.add_child(center)

	var panel := PanelContainer.new()
	panel.custom_minimum_size.x = 420.0
	panel.mouse_filter = Control.MOUSE_FILTER_STOP
	panel.add_theme_stylebox_override("panel", UiKit.paper_stylebox())
	center.add_child(panel)

	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", UiKit.SPACE_MD)
	panel.add_child(box)

	var title := UiKit.title(I18n.t("hud.menu.settings"), UiKit.FS_TITLE)
	title.add_theme_color_override("font_color", UiKit.TINTA)
	box.add_child(title)

	box.add_child(UiKit.rule(Color(UiKit.CARIMBO, 0.45)))

	for code in I18n.SUPPORTED_LOCALES:
		var button := _make_language_button(code)
		box.add_child(button)
		_settings_buttons[code] = button

	box.add_child(UiKit.rule(Color(UiKit.TINTA_SUAVE, 0.4)))
	var hint := UiKit.label(I18n.t("hud.shortcuts"), UiKit.FS_CAPTION, UiKit.TINTA_SUAVE)
	box.add_child(hint)


## Reconstrói o painel de configurações preservando se está aberto.
func _render_settings_texts() -> void:
	var was_visible := _settings_layer.visible
	_render_settings_card()
	_settings_layer.visible = was_visible
	if was_visible:
		UiKit.focus_first(_settings_layer)


## Nome localizado de um idioma na língua ativa.
func _language_name(code: String) -> String:
	return I18n.t("lang." + code)


func _make_language_button(code: String) -> Button:
	var button := Button.new()
	button.text = _language_name(code)
	button.custom_minimum_size.y = UiKit.CONTROL_HEIGHT
	button.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	button.focus_mode = Control.FOCUS_ALL
	button.add_theme_font_override("font", UiKit.PAPER_BODY_FONT)
	button.add_theme_font_size_override("font_size", UiKit.FS_SMALL)
	button.tooltip_text = _language_name(code)
	button.pressed.connect(_on_settings_language.bind(code))
	_style_language_choice(button, code == I18n.locale)
	return button


## Estilo de ficha datilografada: idioma ativo em tinta sólida, demais em papel.
func _style_language_choice(button: Button, active: bool) -> void:
	if active:
		button.add_theme_stylebox_override("normal",
				UiKit.flat(UiKit.TINTA, UiKit.TINTA, 8.0))
		button.add_theme_stylebox_override("hover",
				UiKit.flat(UiKit.TINTA, UiKit.TINTA, 8.0))
		button.add_theme_stylebox_override("pressed",
				UiKit.flat(UiKit.TINTA, UiKit.TINTA, 8.0))
		button.add_theme_stylebox_override("focus", UiKit.outline(UiKit.CARIMBO))
		button.add_theme_color_override("font_color", UiKit.PAPEL_CLARO)
		button.add_theme_color_override("font_hover_color", UiKit.PAPEL_CLARO)
		button.add_theme_color_override("font_pressed_color", UiKit.PAPEL_CLARO)
		button.add_theme_color_override("font_focus_color", UiKit.PAPEL_CLARO)
	else:
		button.add_theme_stylebox_override("normal",
				UiKit.flat(UiKit.PAPEL_CLARO, UiKit.TINTA_SUAVE, 8.0))
		button.add_theme_stylebox_override("hover",
				UiKit.flat(UiKit.PAPEL, UiKit.CARIMBO, 8.0))
		button.add_theme_stylebox_override("pressed",
				UiKit.flat(UiKit.ABA_INATIVA, UiKit.TINTA_SUAVE, 8.0))
		button.add_theme_stylebox_override("focus", UiKit.outline(UiKit.CARIMBO))
		button.add_theme_color_override("font_color", UiKit.TINTA)
		button.add_theme_color_override("font_hover_color", UiKit.TINTA)
		button.add_theme_color_override("font_pressed_color", UiKit.TINTA_SUAVE)
		button.add_theme_color_override("font_focus_color", UiKit.TINTA)


func _on_settings_language(code: String) -> void:
	I18n.save_locale(code)
	I18n.set_locale(code)


func _open_settings() -> void:
	_render_settings_card()
	_settings_layer.visible = true
	_menu_layer.visible = false
	UiKit.focus_first(_settings_layer)
	UiSfx.play(UiSfx.Sound.OPEN)


func _close_settings() -> void:
	if _settings_layer != null and _settings_layer.visible:
		UiSfx.play(UiSfx.Sound.CLOSE)
	_settings_layer.visible = false
	_menu_layer.visible = true


func _on_settings_backdrop_input(event: InputEvent) -> void:
	if event is InputEventMouseButton and event.button_index == MOUSE_BUTTON_LEFT \
			and event.pressed:
		_close_settings()


# --- Eventos ------------------------------------------------------------------

## Guarda os pedidos de cada dia para a janela de Comércio não ficar vazia.
func _log_orders(summary: Dictionary) -> void:
	var day := int(summary.get("day", 0))
	if day > _last_logged_day:
		for order in summary.get("trade_orders", []):
			var entry: Dictionary = order.duplicate()
			entry["day"] = day
			_order_log.push_front(entry)
		_last_logged_day = day
		while _order_log.size() > MAX_ORDER_LOG:
			_order_log.pop_back()
	summary["trade_log"] = _order_log


func _on_status(summary: Dictionary) -> void:
	_summary = summary
	_log_orders(summary)
	_update_top_bar()

	if _window_layer.visible and not _stack.is_empty():
		var entry: Dictionary = _stack.back()
		if String(entry["kind"]) == "screen":
			var id := String(entry["id"])
			_window_subtitle.text = _screen_subtitle(id)
			_screens[id].refresh(summary)
		else:
			_city_screen.refresh(summary)


func _on_locale_changed(_locale: String) -> void:
	UiKit.set_locale(I18n.locale)
	_render_top_bar()
	_render_dock()
	_configure_minimap()
	_refresh_mode_bar()
	_refresh_legend()
	_render_menu_texts()
	_render_settings_texts()

	if _window_layer.visible and not _stack.is_empty():
		_show_top()


## Reconstrói apenas os textos do menu, reaproveitando a mesma camada.
func _render_menu_texts() -> void:
	var was_visible := _menu_layer.visible
	_menu_layer.visible = false
	UiKit.clear(_menu_layer)
	_render_menu_card()
	_menu_layer.visible = was_visible


func _on_speed_pressed(index: int) -> void:
	Economy.set_speed(index)
	_update_top_bar()
