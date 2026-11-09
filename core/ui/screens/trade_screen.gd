extends VBoxContainer

## Janela de Comércio e Logística: pedidos do dia e o detalhe de cada trecho.

const ORDER_WEIGHTS := [0.6, 1.4, 1.4, 1.0, 0.9, 0.9, 0.9]
const ORDER_ALIGN := [true, false, false, false, true, true, true]
const LEG_WEIGHTS := [0.8, 1.2, 1.2, 0.9, 0.8, 0.7, 0.8, 0.8]
const LEG_ALIGN := [false, false, false, true, true, true, true, true]
const MODE_WEIGHTS := [1.0, 0.8, 1.0, 1.0, 0.8]
const MODE_ALIGN := [false, true, true, true, true]

var _summary: Dictionary = {}
var _tab := 0
var _selected := -1
var _tab_buttons: Array[Button] = []
var _list_box: VBoxContainer
var _detail_box: VBoxContainer
var _columns: HBoxContainer


func _ready() -> void:
	add_theme_constant_override("separation", 12)
	size_flags_vertical = Control.SIZE_EXPAND_FILL

	add_child(_build_tabs())
	add_child(UiKit.rule())

	_columns = HBoxContainer.new()
	_columns.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_columns.add_theme_constant_override("separation", 16)
	add_child(_columns)

	_list_box = _make_scroll_column(3)
	_detail_box = _make_scroll_column(2)

	_render()


func _build_tabs() -> HBoxContainer:
	var bar := UiKit.tab_bar()
	for index in 2:
		var button := UiKit.tab_button("")
		button.pressed.connect(_set_tab.bind(index))
		bar.add_child(button)
		_tab_buttons.append(button)
	bar.add_child(UiKit.spacer())
	return bar


func _make_scroll_column(stretch: int) -> VBoxContainer:
	var column := VBoxContainer.new()
	column.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	column.size_flags_vertical = Control.SIZE_EXPAND_FILL
	column.add_theme_constant_override("separation", 2)

	var scroll := ScrollContainer.new()
	scroll.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.size_flags_stretch_ratio = stretch
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	scroll.add_child(column)
	_columns.add_child(scroll)
	return column


func refresh(summary: Dictionary) -> void:
	_summary = summary
	_selected = mini(_selected, _orders().size() - 1)
	if _selected < 0 and not _orders().is_empty():
		_selected = 0
	_retranslate()
	_render()


func _retranslate() -> void:
	if _tab_buttons.size() < 2:
		return
	_tab_buttons[0].text = I18n.t("trade.tab.orders")
	_tab_buttons[1].text = I18n.t("trade.tab.routes")


func _set_tab(index: int) -> void:
	_tab = index
	for i in _tab_buttons.size():
		UiKit.set_tab_active(_tab_buttons[i], i == _tab)
	_render()


func _orders() -> Array:
	return _summary.get("trade_log", _summary.get("trade_orders", []))


func _render() -> void:
	if not is_node_ready() or _list_box == null:
		return

	for i in _tab_buttons.size():
		UiKit.set_tab_active(_tab_buttons[i], i == _tab)

	UiKit.clear(_list_box)
	UiKit.clear(_detail_box)

	if _tab == 0:
		_render_orders()
	else:
		_render_routes()


func _render_orders() -> void:
	var orders := _orders()
	_list_box.add_child(UiKit.caption(I18n.t("trade.history")))
	_list_box.add_child(UiKit.rule())

	if orders.is_empty():
		_list_box.add_child(_empty_note(I18n.t("trade.empty")))
		_detail_box.add_child(_empty_note(I18n.t("trade.select")))
		return

	_list_box.add_child(UiKit.table_header([
		I18n.t("hud.day_short"),
		I18n.t("trade.col.from"),
		I18n.t("trade.col.to"),
		I18n.t("trade.col.scope"),
		I18n.t("trade.col.amount"),
		I18n.t("trade.col.hours"),
		I18n.t("trade.col.freight"),
	], ORDER_WEIGHTS, ORDER_ALIGN))

	for index in orders.size():
		var order: Dictionary = orders[index]
		var row := UiKit.row_button()
		row.set_selected(index == _selected)
		row.add_child(UiKit.table_row([
			UiKit.number(float(order.get("day", 0))),
			I18n.city_name(order["from_city"], order["from_city"]),
			I18n.city_name(order["to_city"], order["to_city"]),
			I18n.t("scope.%s" % order["scope"]),
			UiKit.number(float(order["amount"])) + " t",
			UiKit.number(float(order["transit_hours"]), 1) + " h",
			UiKit.number(float(order["freight_cost"])),
		], ORDER_WEIGHTS, [
			UiKit.TEXT_MUTED, UiKit.TEXT, UiKit.TEXT, UiKit.STEEL, UiKit.TEXT,
			UiKit.TEXT_DIM, UiKit.BRASS,
		], ORDER_ALIGN))
		row.pressed.connect(_select_order.bind(index))
		_list_box.add_child(row)

	_render_order_detail()


func _select_order(index: int) -> void:
	_selected = index
	_render()


func _render_order_detail() -> void:
	var orders := _orders()
	if _selected < 0 or _selected >= orders.size():
		_detail_box.add_child(_empty_note(I18n.t("trade.select")))
		return

	var order: Dictionary = orders[_selected]
	_detail_box.add_child(UiKit.caption(I18n.t("trade.legs")))
	_detail_box.add_child(UiKit.title("%s → %s" % [
		I18n.city_name(order["from_city"], order["from_city"]),
		I18n.city_name(order["to_city"], order["to_city"]),
	], 18))

	var header := HBoxContainer.new()
	header.add_theme_constant_override("separation", 18)
	header.add_child(_pair(I18n.t("trade.col.amount"), UiKit.number(float(order["amount"])) + " t"))
	header.add_child(_pair(I18n.t("trade.col.hours"),
			UiKit.number(float(order["transit_hours"]), 1) + " h"))
	header.add_child(_pair(I18n.t("trade.col.freight"),
			UiKit.number(float(order["freight_cost"]))))
	_detail_box.add_child(header)
	_detail_box.add_child(UiKit.rule())

	_detail_box.add_child(UiKit.table_header([
		I18n.t("trade.col.mode"),
		I18n.t("trade.col.from"),
		I18n.t("trade.col.to"),
		I18n.t("trade.col.distance"),
		I18n.t("trade.col.amount"),
		I18n.t("trade.col.trips"),
		I18n.t("trade.col.hours"),
		I18n.t("trade.col.freight"),
	], LEG_WEIGHTS, LEG_ALIGN))

	for leg in order.get("legs", []):
		var mode: String = leg["mode"]
		_detail_box.add_child(UiKit.table_row([
			I18n.t("mode.%s" % mode),
			I18n.city_name(leg["from"], leg["from"]),
			I18n.city_name(leg["to"], leg["to"]),
			UiKit.number(float(leg["distance_km"])) + " km",
			UiKit.number(float(leg["load_tons"])) + " t",
			UiKit.number(float(leg["trips"])),
			UiKit.number(float(leg["transit_hours"]), 1) + " h",
			UiKit.number(float(leg["cost"])),
		], LEG_WEIGHTS, [
			UiKit.STEEL, UiKit.TEXT, UiKit.TEXT, UiKit.TEXT_DIM, UiKit.TEXT,
			UiKit.TEXT_DIM, UiKit.TEXT_DIM, UiKit.BRASS,
		], LEG_ALIGN))


func _render_routes() -> void:
	var totals := _mode_totals()

	_list_box.add_child(UiKit.caption(I18n.t("trade.tab.routes")))
	_list_box.add_child(UiKit.rule())
	_list_box.add_child(UiKit.table_header([
		I18n.t("trade.col.mode"),
		I18n.t("trade.col.trips"),
		I18n.t("trade.col.distance"),
		I18n.t("trade.col.amount"),
		I18n.t("trade.col.freight"),
	], MODE_WEIGHTS, MODE_ALIGN))

	for mode in ["truck", "ship", "plane"]:
		var data: Dictionary = totals[mode]
		var color := UiKit.TEXT_DIM if int(data["trips"]) == 0 else UiKit.TEXT
		_list_box.add_child(UiKit.table_row([
			I18n.t("mode.%s" % mode),
			UiKit.number(float(data["trips"])),
			UiKit.number(float(data["km"])) + " km",
			UiKit.number(float(data["tons"])) + " t",
			UiKit.number(float(data["cost"])),
		], MODE_WEIGHTS, [
			color, color, color, color, UiKit.BRASS,
		], MODE_ALIGN))

	_detail_box.add_child(_empty_note(I18n.t("trade.tab.routes")))


## Soma por modo de transporte os trechos dos pedidos do dia.
func _mode_totals() -> Dictionary:
	var totals := {
		"truck": {"trips": 0, "km": 0.0, "tons": 0.0, "cost": 0.0},
		"ship": {"trips": 0, "km": 0.0, "tons": 0.0, "cost": 0.0},
		"plane": {"trips": 0, "km": 0.0, "tons": 0.0, "cost": 0.0},
	}

	for order in _orders():
		for leg in order.get("legs", []):
			var mode: String = leg["mode"]
			if not totals.has(mode):
				continue
			var bucket: Dictionary = totals[mode]
			bucket["trips"] = int(bucket["trips"]) + int(leg["trips"])
			bucket["km"] = float(bucket["km"]) + float(leg["distance_km"]) * float(leg["trips"])
			bucket["tons"] = float(bucket["tons"]) + float(leg["load_tons"])
			bucket["cost"] = float(bucket["cost"]) + float(leg["cost"])

	return totals


func _pair(caption_text: String, value_text: String) -> VBoxContainer:
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 2)
	box.add_child(UiKit.caption(caption_text))
	box.add_child(UiKit.label(value_text, 17, UiKit.TEXT))
	return box


func _empty_note(text: String) -> Label:
	var node := UiKit.label(text, 13, UiKit.TEXT_MUTED)
	node.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	node.custom_minimum_size.y = 40
	return node
