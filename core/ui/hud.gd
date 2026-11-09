extends CanvasLayer

## HUD principal no estilo HOI3 modernizado: barra superior persistente, doca
## de diretórios à direita, janelas densas de dados e o detalhe da cidade.
##
## O HUD não simula nada: apenas lê `Economy` e desenha. Toda a cópia de texto
## vem do autoload `I18n`, então trocar de idioma reconstrói a interface.

const ECONOMY_SCREEN: Script = preload("res://core/ui/screens/economy_screen.gd")
const TRADE_SCREEN: Script = preload("res://core/ui/screens/trade_screen.gd")
const PRODUCTION_SCREEN: Script = preload("res://core/ui/screens/production_screen.gd")
const POLITICS_SCREEN: Script = preload("res://core/ui/screens/politics_screen.gd")
const CITY_DETAIL: Script = preload("res://core/ui/city_detail.gd")

const PLAYER_NATION := "GBR"
const IMAGERY_CREDIT := "Imagery © Esri, Maxar, Earthstar Geographics"

const TOP_BAR_HEIGHT := 52.0
const DOCK_WIDTH := 96.0
const DOCK_BUTTON_HEIGHT := 46.0
const WINDOW_SIZE := Vector2(1240.0, 700.0)

const SCREEN_ORDER := ["economy", "trade", "production", "politics"]
const MAX_ORDER_LOG := 240
const SCREEN_LABELS := {
	"economy": "dock.economy",
	"trade": "dock.trade",
	"production": "dock.production",
	"politics": "dock.politics",
}

var _summary: Dictionary = {}
var _order_log: Array = []
var _last_logged_day := -1
var _active := ""
var _screens := {}

var _root: Control
var _top_holder: Control
var _dock_holder: Control
var _window_layer: Control
var _backdrop: ColorRect
var _window: PanelContainer
var _window_title: Label
var _window_close: Button
var _window_body: VBoxContainer
var _city_screen: VBoxContainer

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
	_build_credit()

	Economy.status_updated.connect(_on_status)
	I18n.locale_changed.connect(_on_locale_changed)

	_on_status(_initial_summary())


func _process(_delta: float) -> void:
	if _day_fill != null and is_instance_valid(_day_fill):
		_day_fill.anchor_right = Economy.day_progress()


func _initial_summary() -> Dictionary:
	if not Economy.last_summary.is_empty():
		return Economy.last_summary
	return Economy.sim.get_status()


func _build_root() -> void:
	_root = Control.new()
	_root.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_root.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_root.theme = UiKit.build_theme()
	add_child(_root)


## Crédito obrigatório da imagem de satélite usada no fundo do mapa.
func _build_credit() -> void:
	var label := UiKit.caption(IMAGERY_CREDIT, UiKit.TEXT_MUTED)
	label.set_anchors_and_offsets_preset(Control.PRESET_BOTTOM_RIGHT)
	label.offset_left = -340.0
	label.offset_top = -18.0
	label.offset_right = -10.0
	label.offset_bottom = -4.0
	label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	label.mouse_filter = Control.MOUSE_FILTER_IGNORE
	label.add_theme_color_override("font_outline_color", Color(0.02, 0.03, 0.04, 0.85))
	label.add_theme_constant_override("outline_size", 6)
	_root.add_child(label)


# ---------------------------------------------------------------- barra superior

func _build_top_bar() -> void:
	_top_holder = PanelContainer.new()
	_top_holder.set_anchors_and_offsets_preset(Control.PRESET_TOP_WIDE)
	_top_holder.offset_bottom = TOP_BAR_HEIGHT

	var style := UiKit.flat(UiKit.PANEL, UiKit.EDGE, 14.0)
	style.content_margin_top = 0.0
	style.content_margin_bottom = 0.0
	_top_holder.add_theme_stylebox_override("panel", style)
	_root.add_child(_top_holder)

	# Filete de latão na base da barra: assinatura da carta militar.
	var accent := ColorRect.new()
	accent.color = Color(0.788, 0.635, 0.290, 0.35)
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
	row.add_theme_constant_override("separation", 18)
	row.alignment = BoxContainer.ALIGNMENT_BEGIN
	_top_holder.add_child(row)

	row.add_child(_nation_block())
	row.add_child(_vertical_rule())
	row.add_child(_clock_block())
	row.add_child(UiKit.spacer())

	_metric_values["population"] = _metric(row, "hud.population", "0")
	_metric_values["stock"] = _metric(row, "hud.stock", "0")
	_metric_values["deficit"] = _metric(row, "hud.deficit", "0")
	_metric_values["transit"] = _metric(row, "hud.transit", "0")

	row.add_child(_vertical_rule())
	row.add_child(_speed_block())
	row.add_child(_language_button())

	_update_top_bar()


func _nation_block() -> Control:
	var box := HBoxContainer.new()
	box.add_theme_constant_override("separation", 10)
	box.size_flags_vertical = Control.SIZE_SHRINK_CENTER

	var swatch := ColorRect.new()
	swatch.color = UiKit.nation_color(PLAYER_NATION)
	swatch.custom_minimum_size = Vector2(16.0, 26.0)
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
	box.add_theme_constant_override("separation", 2)
	box.size_flags_vertical = Control.SIZE_SHRINK_CENTER

	_date_label = UiKit.title("", 18)
	_date_label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	box.add_child(_date_label)

	var bottom := HBoxContainer.new()
	bottom.add_theme_constant_override("separation", 10)

	_day_label = UiKit.caption("", UiKit.STEEL)
	bottom.add_child(_day_label)

	bottom.add_child(_day_progress_bar())
	box.add_child(bottom)

	return box


func _day_progress_bar() -> Control:
	var track := Control.new()
	track.custom_minimum_size = Vector2(110.0, 4.0)
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


func _metric(row: HBoxContainer, key: String, value: String) -> Label:
	var pair := UiKit.metric(I18n.t(key), value)
	var box: Control = pair[0]
	box.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	row.add_child(box)
	return pair[1]


func _vertical_rule() -> ColorRect:
	var node := ColorRect.new()
	node.color = UiKit.EDGE_SOFT
	node.custom_minimum_size = Vector2(1.0, 28.0)
	node.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	node.mouse_filter = Control.MOUSE_FILTER_IGNORE
	return node


func _speed_block() -> Control:
	var box := HBoxContainer.new()
	box.add_theme_constant_override("separation", 4)
	box.size_flags_vertical = Control.SIZE_SHRINK_CENTER

	for index in Economy.SPEED_COUNT:
		var button := Button.new()
		button.custom_minimum_size = Vector2(34.0, 28.0)
		button.add_theme_font_size_override("font_size", 13)
		button.pressed.connect(_on_speed_pressed.bind(index))
		box.add_child(button)
		_speed_buttons.append(button)

	return box


func _language_button() -> Button:
	var button := Button.new()
	button.custom_minimum_size = Vector2(46.0, 28.0)
	button.add_theme_font_size_override("font_size", 12)
	button.tooltip_text = I18n.t("hud.language")
	button.pressed.connect(_on_language_pressed)
	button.text = I18n.short_label()
	return button


func _update_top_bar() -> void:
	if _date_label == null:
		return

	var date := Economy.game_date()
	_date_label.text = "%d %s %d" % [
		int(date["day"]), I18n.t("month.%d" % int(date["month"])), int(date["year"]),
	]
	_day_label.text = I18n.t("hud.day", {"day": UiKit.number(float(_summary.get("day", 0)))})

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
	_set_metric("deficit", UiKit.number(deficit) + " t")
	_set_metric("transit", UiKit.number(transit) + " t")

	_metric_values["deficit"].add_theme_color_override("font_color",
			UiKit.NEGATIVE if deficit > 0.0 else UiKit.POSITIVE)

	for index in _speed_buttons.size():
		_speed_buttons[index].text = _speed_label(index)
		UiKit.set_tab_active(_speed_buttons[index], index == Economy.speed)


func _speed_label(index: int) -> String:
	return "II" if index == 0 else "%dx" % index


func _set_metric(key: String, text: String) -> void:
	if _metric_values.has(key):
		_metric_values[key].text = text


# ---------------------------------------------------------------- doca

func _build_dock() -> void:
	_dock_holder = VBoxContainer.new()
	_dock_holder.set_anchors_and_offsets_preset(Control.PRESET_TOP_RIGHT)
	_dock_holder.offset_left = -(DOCK_WIDTH + 10.0)
	_dock_holder.offset_right = -10.0
	_dock_holder.offset_top = TOP_BAR_HEIGHT + 10.0
	_dock_holder.offset_bottom = TOP_BAR_HEIGHT + 10.0 \
			+ SCREEN_ORDER.size() * (DOCK_BUTTON_HEIGHT + 6.0)
	_dock_holder.add_theme_constant_override("separation", 6)
	_root.add_child(_dock_holder)

	_render_dock()


func _render_dock() -> void:
	UiKit.clear(_dock_holder)
	_dock_buttons.clear()

	for id in SCREEN_ORDER:
		var button := Button.new()
		button.text = I18n.t(SCREEN_LABELS[id])
		button.custom_minimum_size.y = DOCK_BUTTON_HEIGHT
		button.add_theme_font_size_override("font_size", 11)
		button.toggle_mode = false
		button.pressed.connect(_toggle_screen.bind(id))
		_dock_holder.add_child(button)
		_dock_buttons[id] = button

	_refresh_dock_state()


func _refresh_dock_state() -> void:
	for id in _dock_buttons:
		UiKit.set_tab_active(_dock_buttons[id], id == _active)


# ---------------------------------------------------------------- janelas

func _build_window() -> void:
	_window_layer = Control.new()
	_window_layer.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_window_layer.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_window_layer.visible = false
	_root.add_child(_window_layer)

	_backdrop = ColorRect.new()
	_backdrop.color = Color(0.02, 0.03, 0.04, 0.55)
	_backdrop.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_backdrop.mouse_filter = Control.MOUSE_FILTER_STOP
	_backdrop.gui_input.connect(_on_backdrop_input)
	_window_layer.add_child(_backdrop)

	var center := CenterContainer.new()
	center.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	center.mouse_filter = Control.MOUSE_FILTER_IGNORE
	_window_layer.add_child(center)

	_window = PanelContainer.new()
	_window.custom_minimum_size = WINDOW_SIZE
	_window.mouse_filter = Control.MOUSE_FILTER_STOP
	var style := UiKit.flat(UiKit.PANEL, UiKit.EDGE, 20.0)
	style.content_margin_top = 16.0
	style.content_margin_bottom = 18.0
	_window.add_theme_stylebox_override("panel", style)
	center.add_child(_window)

	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 12)
	_window.add_child(box)

	var header := HBoxContainer.new()
	header.add_theme_constant_override("separation", 12)
	box.add_child(header)

	_window_title = UiKit.title("", 24)
	header.add_child(_window_title)
	header.add_child(UiKit.spacer())

	var close := Button.new()
	close.text = I18n.t("common.close")
	close.custom_minimum_size.y = 30
	close.pressed.connect(_close_screen)
	header.add_child(close)
	_window_close = close

	box.add_child(UiKit.rule(Color(0.788, 0.635, 0.290, 0.35)))

	_window_body = VBoxContainer.new()
	_window_body.size_flags_vertical = Control.SIZE_EXPAND_FILL
	box.add_child(_window_body)

	_instantiate_screens()


func _instantiate_screens() -> void:
	for id in SCREEN_ORDER:
		var screen: Node = _new_screen(id)
		screen.visible = false
		screen.size_flags_vertical = Control.SIZE_EXPAND_FILL
		_window_body.add_child(screen)
		_screens[id] = screen

	_screens["economy"].city_requested.connect(_on_city_requested)

	_city_screen = CITY_DETAIL.new()
	_city_screen.visible = false
	_city_screen.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_city_screen.closed.connect(_on_city_closed)
	_window_body.add_child(_city_screen)


func _new_screen(id: String) -> Node:
	match id:
		"economy":
			return ECONOMY_SCREEN.new()
		"trade":
			return TRADE_SCREEN.new()
		"production":
			return PRODUCTION_SCREEN.new()
		_:
			return POLITICS_SCREEN.new()


func _toggle_screen(id: String) -> void:
	if _active == id:
		_close_screen()
		return
	_open_screen(id)


func _open_screen(id: String) -> void:
	_active = id
	_window_title.text = I18n.t(SCREEN_LABELS[id])

	_city_screen.visible = false
	for screen_id in _screens:
		_screens[screen_id].visible = screen_id == id

	_screens[id].refresh(_summary)
	_window_layer.visible = true
	_refresh_dock_state()


func _close_screen() -> void:
	_active = ""
	_window_layer.visible = false
	_city_screen.visible = false
	_refresh_dock_state()


func _on_backdrop_input(event: InputEvent) -> void:
	if event is InputEventMouseButton and event.button_index == MOUSE_BUTTON_LEFT \
			and event.pressed:
		_close_screen()


# ---------------------------------------------------------------- detalhe da cidade

func _on_city_requested(city_id: String) -> void:
	for id in _screens:
		_screens[id].visible = false

	_active = "city"
	_window_title.text = I18n.t("city.title")
	_window_layer.visible = true
	_city_screen.visible = true
	_city_screen.open_city(city_id, _summary)
	_refresh_dock_state()


func _on_city_closed() -> void:
	_city_screen.visible = false
	_open_screen("economy")


# ---------------------------------------------------------------- eventos

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

	var active_screen: Node = _screens.get(_active)
	if active_screen != null and active_screen.visible:
		active_screen.refresh(summary)

	if _city_screen.visible:
		_city_screen.refresh(summary)


func _on_locale_changed(_locale: String) -> void:
	UiKit.set_locale(I18n.locale)
	_render_top_bar()
	_render_dock()

	if _city_screen.visible:
		_window_title.text = I18n.t("city.title")
		_city_screen.refresh(_summary)
	elif not _active.is_empty():
		_window_title.text = I18n.t(SCREEN_LABELS[_active])
		_window_close.text = I18n.t("common.close")
		_screens[_active].refresh(_summary)


func _on_speed_pressed(index: int) -> void:
	Economy.set_speed(index)
	_update_top_bar()


func _on_language_pressed() -> void:
	I18n.set_locale(I18n.next_locale())
