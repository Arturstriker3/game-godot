extends VBoxContainer

## Janela de Comércio e Logística: pedidos do dia, rotas e o mercado por cidade.

signal city_requested(city_id: String)

const ORDER_WEIGHTS := [0.6, 1.4, 1.4, 1.0, 0.9, 0.9, 0.9]
const ORDER_ALIGN := [true, false, false, false, true, true, true]
const LEG_WEIGHTS := [0.8, 1.2, 1.2, 0.9, 0.8, 0.7, 0.8, 0.8]
const LEG_ALIGN := [false, false, false, true, true, true, true, true]
const MODE_WEIGHTS := [1.0, 0.8, 1.0, 1.0, 0.8]
const MODE_ALIGN := [false, true, true, true, true]
const MARKET_WEIGHTS := [1.5, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0]
const MARKET_ALIGN := [false, true, true, true, true, true, true, true]

## Ordem das colunas do mercado; o índice é usado pela ordenação.
const SORT_KEYS := ["city", "production", "need", "stockpile", "deficit", "coverage",
		"imported", "exported"]
const FILTER_ALL := "all"
const FILTER_DEFICIT := "deficit"
const FILTER_SCARCITY := "scarcity"
const LOW_COVERAGE_DAYS := 2.0

var _summary: Dictionary = {}
var _tab := 0
var _selected := -1
var _tab_buttons: Array[Button] = []
var _list_box: VBoxContainer
var _detail_box: VBoxContainer
var _columns: HBoxContainer
var _list_scroll: ScrollContainer
var _detail_scroll: ScrollContainer

var _market_scroll: ScrollContainer
var _market_box: VBoxContainer
var _market_national: VBoxContainer
var _market_table: VBoxContainer
var _market_search: LineEdit
var _market_count: PanelContainer
var _market_chips := {}
var _unmodeled_chips: Array[Button] = []
var _market_filter := FILTER_ALL
var _market_query := ""
var _sort_index := 5
var _sort_desc := false


func _ready() -> void:
	add_theme_constant_override("separation", UiKit.SPACE_MD)
	size_flags_vertical = Control.SIZE_EXPAND_FILL

	add_child(_build_tabs())
	add_child(UiKit.rule())

	_columns = HBoxContainer.new()
	_columns.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_columns.add_theme_constant_override("separation", UiKit.SPACE_LG)
	add_child(_columns)

	_list_box = _make_scroll_column(3)
	_detail_box = _make_scroll_column(2)
	_list_scroll = _columns.get_child(0) as ScrollContainer
	_detail_scroll = _columns.get_child(1) as ScrollContainer

	_build_market()

	_render()


func _build_tabs() -> HBoxContainer:
	var bar := UiKit.tab_bar()
	for index in 3:
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

	var card := UiKit.card(UiKit.SPACE_MD)
	card.size_flags_horizontal = Control.SIZE_EXPAND_FILL

	var scroll := ScrollContainer.new()
	scroll.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.size_flags_stretch_ratio = stretch
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	scroll.add_child(card)
	card.add_child(column)
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
	if _tab_buttons.size() < 3:
		return
	_tab_buttons[0].text = I18n.t("trade.tab.orders")
	_tab_buttons[1].text = I18n.t("trade.tab.routes")
	_tab_buttons[2].text = I18n.t("trade.tab.market")
	_market_search.placeholder_text = I18n.t("econ.search")
	_market_chips[FILTER_ALL].text = I18n.t("econ.filter.all")
	_market_chips[FILTER_DEFICIT].text = I18n.t("econ.filter.deficit")
	_market_chips[FILTER_SCARCITY].text = I18n.t("trade.market.scarcity")
	for chip in _unmodeled_chips:
		chip.tooltip_text = I18n.t("trade.market.unmodeled")


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
	_columns.visible = _tab != 2
	_market_scroll.visible = _tab == 2
	_detail_scroll.visible = _tab == 0

	if _tab == 0:
		_render_orders()
	elif _tab == 1:
		_render_routes()
	else:
		_render_market()


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


# --- Mercado -----------------------------------------------------------------

func _build_market() -> void:
	_market_scroll = ScrollContainer.new()
	_market_scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_market_scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	_market_scroll.visible = false
	add_child(_market_scroll)

	var card := UiKit.card(UiKit.SPACE_MD)
	card.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_market_scroll.add_child(card)

	_market_box = VBoxContainer.new()
	_market_box.add_theme_constant_override("separation", UiKit.SPACE_SM)
	_market_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	card.add_child(_market_box)

	_market_box.add_child(_build_market_toolbar())

	_market_national = VBoxContainer.new()
	_market_national.add_theme_constant_override("separation", 0)
	_market_box.add_child(_market_national)
	_market_box.add_child(UiKit.rule())

	_market_table = VBoxContainer.new()
	_market_table.add_theme_constant_override("separation", 2)
	_market_table.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_market_box.add_child(_market_table)


func _build_market_toolbar() -> VBoxContainer:
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", UiKit.SPACE_SM)

	var products := UiKit.toolbar()
	products.add_child(UiKit.caption(I18n.t("trade.market.products")))
	for id in ["wheat", "flour", "bread"]:
		var chip := UiKit.filter_chip(I18n.t("product." + id))
		chip.set_pressed_no_signal(id == "wheat")
		chip.disabled = id != "wheat"
		if chip.disabled:
			chip.tooltip_text = I18n.t("trade.market.unmodeled")
			_unmodeled_chips.append(chip)
		products.add_child(chip)
	products.add_child(UiKit.spacer())
	box.add_child(products)

	var row := UiKit.toolbar()
	for id in [FILTER_ALL, FILTER_DEFICIT, FILTER_SCARCITY]:
		var chip := UiKit.filter_chip("")
		chip.pressed.connect(_set_market_filter.bind(id))
		row.add_child(chip)
		_market_chips[id] = chip
	row.add_child(UiKit.spacer())

	_market_search = UiKit.search_field("")
	_market_search.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	_market_search.text_changed.connect(_on_market_search)
	row.add_child(_market_search)

	_market_count = UiKit.status_pill("", "steel")
	row.add_child(_market_count)
	box.add_child(row)

	return box


func _set_market_filter(id: String) -> void:
	_market_filter = id
	_render_market()


func _on_market_search(text: String) -> void:
	_market_query = text.strip_edges()
	_render_market()


func _sort_by(index: int) -> void:
	if index == _sort_index:
		_sort_desc = not _sort_desc
	else:
		_sort_index = index
		_sort_desc = index != 0
	_render_market()


func _render_market() -> void:
	if _market_table == null:
		return

	UiKit.clear(_market_national)
	UiKit.clear(_market_table)

	for id in _market_chips:
		UiKit.set_chip_active(_market_chips[id], id == _market_filter)

	var rows := _market_rows()
	var total: int = _summary.get("cities", []).size()
	UiKit.apply_pill(_market_count, I18n.t("econ.pill.count", {
		"shown": UiKit.number(rows.size()),
		"total": UiKit.number(total),
	}), "steel")

	_market_national.add_child(_national_strip())

	if rows.is_empty():
		_market_table.add_child(UiKit.empty_state(
				I18n.t("econ.empty.filtered"), I18n.t("econ.empty.filtered.body"),
				I18n.t("econ.filter.clear"), _clear_market_filters))
		return

	_market_table.add_child(UiKit.caption(I18n.t("trade.market.by_city")))
	_market_table.add_child(UiKit.sort_header([
		I18n.t("econ.col.city"),
		I18n.t("trade.col.offer"),
		I18n.t("econ.col.need"),
		I18n.t("econ.col.stock"),
		I18n.t("econ.col.deficit"),
		I18n.t("city.coverage"),
		I18n.t("trade.col.imports"),
		I18n.t("trade.col.exports"),
	], MARKET_WEIGHTS, MARKET_ALIGN, _sort_index, _sort_desc, _sort_by))
	_market_table.add_child(UiKit.rule())

	for row_data in rows:
		_market_table.add_child(_market_row(row_data))


func _market_row(data: Dictionary) -> Control:
	var city: Dictionary = data["city"]
	var metrics: Dictionary = data["metrics"]

	var row := UiKit.row_button()
	row.add_child(UiKit.table_row([
		I18n.city_name(city["id"], city["name"]),
		UiKit.number(float(metrics["production"])) + " t",
		UiKit.number(float(metrics["need"])) + " t",
		UiKit.number(float(metrics["stockpile"])) + " t",
		UiKit.number(float(metrics["deficit"])) + " t",
		UiKit.number(float(metrics["coverage"]), 1) + " " + I18n.t("common.days_short"),
		UiKit.number(float(metrics["imported"])) + " t",
		UiKit.number(float(metrics["exported"])) + " t",
	], MARKET_WEIGHTS, [
		UiKit.TEXT,
		UiKit.TEXT_DIM,
		UiKit.TEXT_DIM,
		UiKit.TEXT,
		UiKit.NEGATIVE if float(metrics["deficit"]) > 0.0 else UiKit.POSITIVE,
		UiKit.coverage_color(float(metrics["coverage"])),
		UiKit.STEEL,
		UiKit.TEXT_MUTED,
	], MARKET_ALIGN))

	var city_id := String(city["id"])
	row.pressed.connect(func() -> void: city_requested.emit(city_id))
	return row


func _national_strip() -> Control:
	var totals := _national_totals()
	var card := UiKit.card(UiKit.SPACE_MD)
	card.size_flags_horizontal = Control.SIZE_EXPAND_FILL

	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", UiKit.SPACE_XL)
	card.add_child(row)

	var label := VBoxContainer.new()
	label.add_theme_constant_override("separation", 0)
	label.add_child(UiKit.caption(I18n.t("trade.market.national"), UiKit.BRASS))
	label.add_child(UiKit.label(I18n.t("product.wheat"), UiKit.FS_SUBTITLE, UiKit.TEXT))
	row.add_child(label)

	row.add_child(_pair(I18n.t("trade.col.offer"),
			UiKit.number(float(totals["production"])) + " t"))
	row.add_child(_pair(I18n.t("econ.col.need"),
			UiKit.number(float(totals["need"])) + " t"))
	row.add_child(_pair(I18n.t("econ.col.stock"),
			UiKit.number(float(totals["stockpile"])) + " t"))
	row.add_child(_pair(I18n.t("econ.col.deficit"),
			UiKit.number(float(totals["deficit"])) + " t"))
	row.add_child(_pair(I18n.t("city.coverage"),
			UiKit.number(float(totals["coverage"]), 1) + " " + I18n.t("common.days_short")))

	var note := UiKit.caption(I18n.t("trade.market.prices"), UiKit.TEXT_MUTED)
	note.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	card.add_child(note)
	return card


func _clear_market_filters() -> void:
	_market_query = ""
	_market_filter = FILTER_ALL
	_market_search.text = ""
	_render_market()


func _city_market_metrics(city: Dictionary) -> Dictionary:
	var need := float(city.get("need", 0.0))
	var stock := float(city.get("stockpile", 0.0))
	var imported := 0.0
	var exported := 0.0

	for order in _orders():
		var amount := float(order.get("amount", 0.0))
		if String(order.get("to_city", "")) == String(city["id"]):
			imported += amount
		if String(order.get("from_city", "")) == String(city["id"]):
			exported += amount

	return {
		"production": float(city.get("production", 0.0)),
		"need": need,
		"stockpile": stock,
		"deficit": float(city.get("deficit", 0.0)),
		"coverage": stock / need if need > 0.0 else 0.0,
		"imported": imported,
		"exported": exported,
	}


## Aplica busca, filtro e ordenação sobre os dados reais, sem tocar na simulação.
func _market_rows() -> Array:
	var needle := _market_query.to_lower()
	var rows: Array = []

	for city in _summary.get("cities", []):
		var metrics := _city_market_metrics(city)
		if _market_filter == FILTER_DEFICIT and float(metrics["deficit"]) <= 0.0:
			continue
		if _market_filter == FILTER_SCARCITY \
				and float(metrics["coverage"]) >= LOW_COVERAGE_DAYS:
			continue
		if not needle.is_empty():
			var name := I18n.city_name(city["id"], city["name"]).to_lower()
			var nation := I18n.nation_name(city["nation"]).to_lower()
			if not (name.contains(needle) or nation.contains(needle)):
				continue
		rows.append({"city": city, "metrics": metrics})

	rows.sort_custom(_compare_market_rows)
	return rows


func _compare_market_rows(a: Dictionary, b: Dictionary) -> bool:
	var key := String(SORT_KEYS[_sort_index])
	if key == "city":
		var left := I18n.city_name(a["city"]["id"], a["city"]["name"])
		var right := I18n.city_name(b["city"]["id"], b["city"]["name"])
		return left > right if _sort_desc else left < right

	var left_value := float(a["metrics"][key])
	var right_value := float(b["metrics"][key])
	return left_value > right_value if _sort_desc else left_value < right_value


func _national_totals() -> Dictionary:
	var production := 0.0
	var need := 0.0
	var stock := 0.0
	var deficit := 0.0

	for city in _summary.get("cities", []):
		production += float(city.get("production", 0.0))
		need += float(city.get("need", 0.0))
		stock += float(city.get("stockpile", 0.0))
		deficit += float(city.get("deficit", 0.0))

	return {
		"production": production,
		"need": need,
		"stockpile": stock,
		"deficit": deficit,
		"coverage": stock / need if need > 0.0 else 0.0,
	}


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
	box.add_child(UiKit.label(value_text, UiKit.FS_SUBTITLE, UiKit.TEXT))
	return box


func _empty_note(text: String) -> Label:
	var node := UiKit.label(text, UiKit.FS_SMALL, UiKit.TEXT_MUTED)
	node.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	node.custom_minimum_size.y = 40
	return node
