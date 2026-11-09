extends VBoxContainer

## Detalhe da cidade, aberto dentro da janela principal como sub-tela.
## A navegação de volta fica no cabeçalho da janela do HUD.

const ORDER_WEIGHTS := [1.4, 1.0, 0.9, 0.9]
const ORDER_ALIGN := [false, true, true, true]

var _city_id := ""
var _summary: Dictionary = {}
var _body: VBoxContainer


func _ready() -> void:
	add_theme_constant_override("separation", UiKit.SPACE_LG)
	size_flags_vertical = Control.SIZE_EXPAND_FILL

	var scroll := ScrollContainer.new()
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	add_child(scroll)

	_body = VBoxContainer.new()
	_body.add_theme_constant_override("separation", UiKit.SPACE_MD)
	_body.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_body.size_flags_vertical = Control.SIZE_EXPAND_FILL
	scroll.add_child(_body)


func open_city(city_id: String, summary: Dictionary) -> void:
	_city_id = city_id
	_summary = summary
	_render()


## Cidade exibida no momento (usada pelo cabeçalho da janela do HUD).
func current_city_id() -> String:
	return _city_id


func refresh(summary: Dictionary) -> void:
	_summary = summary
	_render()


func _find_city() -> Dictionary:
	for city in _summary.get("cities", []):
		if city["id"] == _city_id:
			return city
	return {}


func _render() -> void:
	if _body == null:
		return

	UiKit.clear(_body)
	var city := _find_city()
	if city.is_empty():
		return

	_body.add_child(_header(city))
	_body.add_child(UiKit.rule())

	var columns := HBoxContainer.new()
	columns.add_theme_constant_override("separation", 32)
	columns.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_body.add_child(columns)

	columns.add_child(_numbers(city))
	columns.add_child(_vertical_rule())
	columns.add_child(_orders())


func _header(city: Dictionary) -> Control:
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", UiKit.SPACE_XS)

	var top := HBoxContainer.new()
	top.add_theme_constant_override("separation", UiKit.SPACE_SM + 2)

	var swatch := ColorRect.new()
	swatch.color = UiKit.nation_color(city["nation"])
	swatch.custom_minimum_size = Vector2(4.0, 22.0)
	swatch.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	swatch.mouse_filter = Control.MOUSE_FILTER_IGNORE
	top.add_child(swatch)

	top.add_child(UiKit.spacer())

	var deficit := float(city["deficit"])
	var transit := float(city["in_transit"])
	top.add_child(UiKit.status_pill(
			I18n.t("city.deficit") + " " + UiKit.number(deficit) + " t",
			"negative" if deficit > 0.0 else "positive"))
	top.add_child(UiKit.status_pill(
			I18n.t("city.transit") + " " + UiKit.number(transit) + " t", "steel"))
	box.add_child(top)

	box.add_child(UiKit.paragraph(
			I18n.t("city.population") + " " + UiKit.number(float(city["population"])),
			UiKit.FS_SMALL, UiKit.TEXT_DIM))

	return box


func _numbers(city: Dictionary) -> Control:
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 12)
	box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.size_flags_stretch_ratio = 1.0

	var grid := GridContainer.new()
	grid.columns = 2
	grid.add_theme_constant_override("h_separation", 24)
	grid.add_theme_constant_override("v_separation", 8)
	box.add_child(grid)

	var deficit := float(city["deficit"])
	_add_stat(grid, I18n.t("city.production"), UiKit.number(float(city["production"])) + " t")
	_add_stat(grid, I18n.t("city.need"), UiKit.number(float(city["need"])) + " t")
	_add_stat(grid, I18n.t("city.consumed"), UiKit.number(float(city["consumed"])) + " t")
	_add_stat(grid, I18n.t("city.stock"), UiKit.number(float(city["stockpile"])) + " t")
	_add_stat(grid, I18n.t("city.deficit"), UiKit.number(deficit) + " t",
			UiKit.NEGATIVE if deficit > 0.0 else UiKit.POSITIVE)
	_add_stat(grid, I18n.t("city.dispatched"),
			UiKit.number(float(city["dispatched"])) + " t", UiKit.BRASS)
	_add_stat(grid, I18n.t("city.transit"),
			UiKit.number(float(city["in_transit"])) + " t", UiKit.STEEL)

	box.add_child(_coverage(city))
	return box


func _add_stat(grid: GridContainer, caption_text: String, value_text: String,
		color: Color = UiKit.TEXT) -> void:
	grid.add_child(UiKit.caption(caption_text))
	var value := UiKit.label(value_text, 16, color)
	value.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	value.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	grid.add_child(value)


func _coverage(city: Dictionary) -> Control:
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 6)

	var need := float(city["need"])
	var stock := float(city["stockpile"])
	var days := 0.0 if need <= 0.0 else stock / need

	var top := HBoxContainer.new()
	top.add_child(UiKit.caption(I18n.t("city.coverage")))
	top.add_child(UiKit.spacer())
	top.add_child(UiKit.label(I18n.t("city.coverage_days", {
		"days": UiKit.number(days, 1),
	}), 12, UiKit.TEXT_DIM))
	box.add_child(top)
	box.add_child(UiKit.meter(days / 7.0, UiKit.coverage_color(days), 6))

	return box


func _vertical_rule() -> ColorRect:
	var node := ColorRect.new()
	node.color = UiKit.EDGE_SOFT
	node.custom_minimum_size = Vector2(1.0, 0.0)
	node.size_flags_vertical = Control.SIZE_EXPAND_FILL
	node.mouse_filter = Control.MOUSE_FILTER_IGNORE
	return node


func _orders() -> Control:
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 4)
	box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.size_flags_stretch_ratio = 1.4

	box.add_child(UiKit.caption(I18n.t("city.orders")))

	var related: Array = []
	var log: Array = _summary.get("trade_log", _summary.get("trade_orders", []))
	for order in log:
		if order["from_city"] == _city_id or order["to_city"] == _city_id:
			related.append(order)

	if related.is_empty():
		box.add_child(UiKit.label(I18n.t("city.no_orders"), 12, UiKit.TEXT_MUTED))
	else:
		box.add_child(UiKit.table_header([
			I18n.t("trade.col.from"),
			I18n.t("trade.col.amount"),
			I18n.t("trade.col.hours"),
			I18n.t("trade.col.freight"),
		], ORDER_WEIGHTS, ORDER_ALIGN))

		for order in related:
			box.add_child(UiKit.table_row([
				I18n.city_name(order["from_city"], order["from_city"]),
				UiKit.number(float(order["amount"])) + " t",
				UiKit.number(float(order["transit_hours"]), 1) + " h",
				UiKit.number(float(order["freight_cost"])),
			], ORDER_WEIGHTS, [
				UiKit.TEXT, UiKit.TEXT, UiKit.TEXT_DIM, UiKit.BRASS,
			], ORDER_ALIGN))

	return box
