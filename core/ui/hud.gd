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
## Clique no minimapa: ponto do mapa em UV (0..1).
signal map_point_requested(map_uv: Vector2)

const PLAYER_NATION := "GBR"
const IMAGERY_CREDIT := "Imagery © Esri, Maxar, Earthstar Geographics"

## Altura da barra superior reduzida para 48 px (estilo HOI3: mais compacto).
const TOP_BAR_HEIGHT := 48.0
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
const MAP_MODES := ["political", "geographic", "population", "economic", "industrial", "resources"]

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
var _mode_bar: HBoxContainer
var _mode_buttons := {}
var _mode := "political"

var _menu_layer: Control
var _menu_credits: Label
var _menu_fullscreen: Button
var _credit_label: Label
var _minimap: Minimap

var _metric_values := {}
var _date_label: Label
var _day_label: Label
var _day_fill: ColorRect
var _speed_buttons: Array[Button] = []
var _dock_buttons := {}


func _ready() -> void:
	layer = 50
	UiKit.set_locale(I18n.locale)

	_build_root()
	_build_top_bar()
	_build_dock()
	_build_window()
	_build_menu()
	_build_credit()
	_build_mode_bar()
	_build_minimap()

	Economy.status_updated.connect(_on_status)
	I18n.locale_changed.connect(_on_locale_changed)
	get_viewport().size_changed.connect(_apply_window_size)

	_on_status(_initial_summary())
	_apply_window_size()


func _process(_delta: float) -> void:
	if _day_fill != null and is_instance_valid(_day_fill):
		_day_fill.anchor_right = Economy.day_progress()


func _unhandled_input(event: InputEvent) -> void:
	if event.is_action_pressed("ui_cancel"):
		if _menu_layer.visible:
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


func open_city(city_id: String) -> void:
	_on_city_requested(city_id)


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
	return Economy.sim.get_status()


## Crédito obrigatório da imagem de satélite usada no fundo do mapa.
func _build_credit() -> void:
	_credit_label = UiKit.caption(IMAGERY_CREDIT, UiKit.TEXT_MUTED)
	_credit_label.set_anchors_and_offsets_preset(Control.PRESET_BOTTOM_RIGHT)
	_credit_label.offset_left = -360.0
	_credit_label.offset_top = -18.0
	_credit_label.offset_right = -12.0
	_credit_label.offset_bottom = -5.0
	_credit_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	_credit_label.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_credit_label.add_theme_color_override("font_outline_color", Color(0.02, 0.03, 0.04, 0.85))
	_credit_label.add_theme_constant_override("outline_size", 6)
	_root.add_child(_credit_label)


## Seletor de camada de dados do mapa, no canto inferior esquerdo.
func _build_mode_bar() -> void:
	_mode_bar = HBoxContainer.new()
	_mode_bar.set_anchors_and_offsets_preset(Control.PRESET_BOTTOM_LEFT)
	_mode_bar.offset_left = UiKit.SPACE_LG
	_mode_bar.offset_top = -34.0
	_mode_bar.offset_bottom = -UiKit.SPACE_MD
	_mode_bar.add_theme_constant_override("separation", UiKit.SPACE_XS)
	_root.add_child(_mode_bar)

	_render_mode_bar()


func _render_mode_bar() -> void:
	UiKit.clear(_mode_bar)
	_mode_buttons.clear()
	for mode in MAP_MODES:
		var chip := UiKit.filter_chip(I18n.t("map." + mode))
		chip.tooltip_text = I18n.t("map.hint")
		chip.pressed.connect(func() -> void: _set_map_mode(mode))
		_mode_bar.add_child(chip)
		_mode_buttons[mode] = chip
	_refresh_mode_bar()


func _refresh_mode_bar() -> void:
	for mode in _mode_buttons:
		UiKit.set_chip_active(_mode_buttons[mode], mode == _mode)


func _set_map_mode(mode: String) -> void:
	if not MAP_MODES.has(mode):
		return
	_mode = mode
	_refresh_mode_bar()
	map_mode_requested.emit(mode)


func set_map_mode(mode: String) -> void:
	_set_map_mode(mode)


func map_mode() -> String:
	return _mode


# --- Minimapa ----------------------------------------------------------------

func _build_minimap() -> void:
	_minimap = Minimap.new()
	_minimap.set_anchors_and_offsets_preset(Control.PRESET_BOTTOM_RIGHT)
	var full := Minimap.MINIMAP_SIZE + Vector2.ONE * Minimap.BORDER_WIDTH * 2.0
	_minimap.offset_right = -UiKit.SPACE_LG
	_minimap.offset_bottom = -UiKit.SPACE_XL
	_minimap.offset_left = _minimap.offset_right - full.x
	_minimap.offset_top = _minimap.offset_bottom - full.y
	_minimap.point_requested.connect(func(uv: Vector2) -> void: map_point_requested.emit(uv))
	_root.add_child(_minimap)


## Área vista pela câmera, em UV do mapa (chamado por main_game.gd).
func set_minimap_view(center_uv: Vector2, size_uv: Vector2) -> void:
	if _minimap != null:
		_minimap.set_view(center_uv, size_uv)


# --- Barra superior ----------------------------------------------------------

func _build_top_bar() -> void:
	_top_holder = PanelContainer.new()
	_top_holder.set_anchors_and_offsets_preset(Control.PRESET_TOP_WIDE)
	_top_holder.offset_bottom = TOP_BAR_HEIGHT
	_top_holder.mouse_filter = Control.MOUSE_FILTER_STOP

	var style := UiKit.flat(UiKit.PANEL, UiKit.EDGE, 14.0)
	style.content_margin_left = UiKit.SPACE_MD
	style.content_margin_right = UiKit.SPACE_MD
	style.content_margin_top = 0.0
	style.content_margin_bottom = 0.0
	_top_holder.add_theme_stylebox_override("panel", style)
	_root.add_child(_top_holder)

	var accent := ColorRect.new()
	accent.color = Color(UiKit.BRASS, 0.35)
	accent.mouse_filter = Control.MOUSE_FILTER_IGNORE
	accent.set_anchors_and_offsets_preset(Control.PRESET_TOP_WIDE)
	accent.offset_top = TOP_BAR_HEIGHT - 1.0
	accent.offset_bottom = TOP_BAR_HEIGHT
	_root.add_child(accent)

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

	_metric_values["population"] = _metric_block(row, "hud.population", "hud.tip.population")
	row.add_child(_vertical_rule())
	_metric_values["stock"] = _metric_block(row, "hud.stock", "hud.tip.stock")
	row.add_child(_vertical_rule())
	_metric_values["deficit"] = _metric_block(row, "hud.deficit", "hud.tip.deficit")
	row.add_child(_vertical_rule())
	_metric_values["transit"] = _metric_block(row, "hud.transit", "hud.tip.transit")
	row.add_child(_vertical_rule())

	row.add_child(_speed_block())
	row.add_child(_language_button())

	_alerts_button = UiKit.icon_button("!", I18n.t("hud.alerts"))
	_alerts_button.custom_minimum_size = Vector2(32.0, 30.0)
	_alerts_button.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	_alerts_button.pressed.connect(func() -> void: _open_screen("alerts"))
	row.add_child(_alerts_button)

	var menu := UiKit.icon_button("≡", I18n.t("hud.menu"))
	menu.custom_minimum_size = Vector2(32.0, 30.0)
	menu.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	menu.pressed.connect(_open_menu)
	row.add_child(menu)

	_update_top_bar()


func _nation_block() -> Control:
	var box := HBoxContainer.new()
	box.add_theme_constant_override("separation", UiKit.SPACE_SM + 2)
	box.size_flags_vertical = Control.SIZE_SHRINK_CENTER

	var swatch := ColorRect.new()
	swatch.color = UiKit.nation_color(PLAYER_NATION)
	swatch.custom_minimum_size = Vector2(4.0, 30.0)
	swatch.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	swatch.mouse_filter = Control.MOUSE_FILTER_IGNORE
	box.add_child(swatch)

	var text := VBoxContainer.new()
	text.add_theme_constant_override("separation", 0)
	text.size_flags_vertical = Control.SIZE_SHRINK_CENTER

	var name_label := UiKit.title(I18n.nation_name(PLAYER_NATION), 18)
	name_label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	text.add_child(name_label)
	text.add_child(UiKit.caption(I18n.t("hud.player_nation"), UiKit.TEXT_MUTED))

	box.add_child(text)
	return box


func _clock_block() -> Control:
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 1)
	box.size_flags_vertical = Control.SIZE_SHRINK_CENTER

	_date_label = UiKit.title("", 18)
	_date_label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	box.add_child(_date_label)

	var bottom := HBoxContainer.new()
	bottom.add_theme_constant_override("separation", UiKit.SPACE_SM + 2)

	_day_label = UiKit.caption("", UiKit.STEEL)
	bottom.add_child(_day_label)
	bottom.add_child(_day_progress_bar())
	box.add_child(bottom)

	return box


func _day_progress_bar() -> Control:
	var track := Control.new()
	track.custom_minimum_size = Vector2(96.0, 3.0)
	track.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	track.mouse_filter = Control.MOUSE_FILTER_IGNORE

	var background := ColorRect.new()
	background.color = UiKit.EDGE_SOFT
	background.mouse_filter = Control.MOUSE_FILTER_IGNORE
	background.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	track.add_child(background)

	_day_fill = ColorRect.new()
	_day_fill.color = UiKit.BRASS
	_day_fill.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_day_fill.anchor_right = 0.0
	_day_fill.anchor_bottom = 1.0
	_day_fill.offset_right = 0.0
	_day_fill.offset_bottom = 0.0
	track.add_child(_day_fill)

	return track


## Indicador da barra: rótulo pequeno acima, valor em destaque, tooltip.
func _metric_block(row: HBoxContainer, key: String, tip_key: String) -> Label:
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 0)
	box.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	box.tooltip_text = I18n.t(tip_key)

	box.add_child(UiKit.caption(I18n.t(key)))
	var value := UiKit.label("0", UiKit.FS_TITLE, UiKit.TEXT)
	value.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	box.add_child(value)

	row.add_child(box)
	return value


func _vertical_rule() -> ColorRect:
	var node := ColorRect.new()
	node.color = UiKit.EDGE_SOFT
	node.custom_minimum_size = Vector2(1.0, 26.0)
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
		button.custom_minimum_size = Vector2(34.0, 28.0)
		button.size_flags_vertical = Control.SIZE_SHRINK_CENTER
		button.add_theme_font_size_override("font_size", UiKit.FS_CAPTION + 1)
		button.tooltip_text = I18n.t("hud.paused") if index == 0 else "%dx" % index
		button.pressed.connect(_on_speed_pressed.bind(index))
		box.add_child(button)
		_speed_buttons.append(button)

	return box


func _language_button() -> Button:
	var button := UiKit.ghost_button(I18n.short_label())
	button.custom_minimum_size = Vector2(42.0, 28.0)
	button.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	button.add_theme_font_size_override("font_size", UiKit.FS_CAPTION + 1)
	button.tooltip_text = I18n.t("hud.language")
	button.pressed.connect(_on_language_pressed)
	return button


func _update_top_bar() -> void:
	if _date_label == null:
		return

	var date := Economy.game_date()
	_date_label.text = "%d %s %d" % [
		int(date["day"]), I18n.t("month.%d" % int(date["month"])), int(date["year"]),
	]
	_day_label.text = I18n.t("hud.day", {"day": UiKit.number(float(_summary.get("day", 0)))})
	var paused := Economy.speed == 0
	if paused:
		_day_label.text = I18n.t("hud.paused")
	_day_label.add_theme_color_override("font_color",
			UiKit.WARNING if paused else UiKit.STEEL)
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
			UiKit.NEGATIVE if deficit > 0.0 else UiKit.POSITIVE)
	_set_metric("transit", UiKit.number(transit) + " t", UiKit.STEEL)

	for index in _speed_buttons.size():
		_speed_buttons[index].text = _speed_label(index)
		UiKit.set_tab_active(_speed_buttons[index], index == Economy.speed)

	if _alerts_button != null:
		var alerts := _alerts()
		_alerts_button.text = "!" if alerts.is_empty() else "!%d" % alerts.size()
		_alerts_button.add_theme_color_override("font_color",
				UiKit.TEXT_DIM if alerts.is_empty() else UiKit.WARNING)
		_alerts_button.tooltip_text = I18n.t("hud.alerts") if alerts.is_empty() \
				else I18n.t("alerts.count", {"count": alerts.size()})


func _speed_label(index: int) -> String:
	return "II" if index == 0 else "%dx" % index


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

	var backdrop := ColorRect.new()
	backdrop.color = UiKit.OVERLAY
	backdrop.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	backdrop.mouse_filter = Control.MOUSE_FILTER_STOP
	backdrop.gui_input.connect(func(event: InputEvent) -> void:
		if event is InputEventMouseButton and event.button_index == MOUSE_BUTTON_LEFT \
				and event.pressed:
			_close_menu())
	_menu_layer.add_child(backdrop)

	_render_menu_card()


## Conteúdo do menu: mantido separado para a troca de idioma recriar só ele.
func _render_menu_card() -> void:
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
	_refresh_fullscreen_button()
	UiSfx.play(UiSfx.Sound.TOGGLE)


func _refresh_fullscreen_button() -> void:
	if _menu_fullscreen == null:
		return
	_menu_fullscreen.text = I18n.t(
		"hud.menu.fullscreen.on" if _is_fullscreen() else "hud.menu.fullscreen.off")


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
	_render_mode_bar()
	_render_menu_texts()
	_credit_label.text = IMAGERY_CREDIT

	if _window_layer.visible and not _stack.is_empty():
		_show_top()


## Reconstrói apenas os textos do menu, reaproveitando a mesma camada.
func _render_menu_texts() -> void:
	_menu_layer.visible = false
	UiKit.clear(_menu_layer)
	_render_menu_card()


func _on_speed_pressed(index: int) -> void:
	Economy.set_speed(index)
	_update_top_bar()


func _on_language_pressed() -> void:
	I18n.set_locale(I18n.next_locale())
