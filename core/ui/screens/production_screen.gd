extends VBoxContainer

## Produção nacional: a cadeia do trigo com os números reais de cada cidade.
## A simulação atual modela apenas trigo; as etapas seguintes aparecem como
## planejadas, sem números inventados.

signal city_requested(city_id: String)

const TABS := ["chain", "cities"]
const TAB_LABELS := {"chain": "prod.tab.chain", "cities": "prod.tab.cities"}
const CITY_WEIGHTS := [1.4, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0]
const CITY_ALIGN := [false, false, true, true, true, true, true]

var _summary: Dictionary = {}
var _tab := "chain"
var _tab_buttons := {}
var _body: VBoxContainer
var _scroll: ScrollContainer


func _ready() -> void:
	add_theme_constant_override("separation", UiKit.SPACE_MD)
	size_flags_vertical = Control.SIZE_EXPAND_FILL

	var tabs := HBoxContainer.new()
	tabs.add_theme_constant_override("separation", UiKit.SPACE_XS)
	for id in TABS:
		var button := UiKit.tab_button("")
		button.pressed.connect(_set_tab.bind(id))
		tabs.add_child(button)
		_tab_buttons[id] = button
	tabs.add_child(UiKit.spacer())
	add_child(tabs)
	add_child(UiKit.rule())

	_scroll = ScrollContainer.new()
	_scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	UiKit.bind_scroll_sfx(_scroll)
	add_child(_scroll)

	_body = VBoxContainer.new()
	_body.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_body.add_theme_constant_override("separation", UiKit.SPACE_MD)
	_scroll.add_child(_body)

	_render()


func refresh(summary: Dictionary) -> void:
	_summary = summary
	_render()


func _set_tab(id: String) -> void:
	_tab = id
	_render()


func _render() -> void:
	if _body == null:
		return
	for id in _tab_buttons:
		_tab_buttons[id].text = I18n.t(TAB_LABELS[id])
		UiKit.set_tab_active(_tab_buttons[id], id == _tab)

	UiKit.clear(_body)
	_body.add_child(_glance())

	if _tab == "chain":
		_build_chain()
	else:
		_build_cities()


func _glance() -> Control:
	var totals := _totals()
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", UiKit.SPACE_MD)
	for item in [
		["city.glance.production", _tons(totals["production"]) + I18n.t("common.per_day")],
		["city.glance.demand", _tons(totals["need"]) + I18n.t("common.per_day")],
		["econ.col.deficit", _tons(totals["deficit"])],
		["city.glance.coverage", _coverage_text(totals["coverage"])],
	]:
		var card := PanelContainer.new()
		card.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		var style := UiKit.flat(UiKit.PANEL_RAISED, UiKit.EDGE_SOFT, 10.0)
		style.content_margin_top = 8.0
		style.content_margin_bottom = 8.0
		card.add_theme_stylebox_override("panel", style)
		var box := VBoxContainer.new()
		box.add_theme_constant_override("separation", 0)
		box.add_child(UiKit.caption(I18n.t(String(item[0]))))
		box.add_child(UiKit.label(String(item[1]), UiKit.FS_TITLE, UiKit.TEXT))
		card.add_child(box)
		row.add_child(card)
	return row


func _build_chain() -> void:
	var card := UiKit.card(UiKit.SPACE_MD)
	card.add_child(_chain_heading())
	_body.add_child(card)

	var producers := _producers()
	var card_list := UiKit.card(UiKit.SPACE_MD)
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 2)
	card_list.add_child(box)
	box.add_child(UiKit.caption(I18n.t("city.producers.title"), UiKit.BRASS))
	box.add_child(UiKit.rule())

	if producers.is_empty():
		box.add_child(UiKit.paragraph(I18n.t("city.producers.empty"), UiKit.FS_SMALL,
				UiKit.TEXT_MUTED, 520.0))
	else:
		for city in producers:
			box.add_child(_city_row(city))
	_body.add_child(card_list)


func _chain_heading() -> Control:
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", UiKit.SPACE_SM)
	box.add_child(UiKit.caption(I18n.t("city.chain.title"), UiKit.BRASS))

	var flow := HBoxContainer.new()
	flow.add_theme_constant_override("separation", UiKit.SPACE_SM)
	flow.add_child(_chain_node(I18n.t("product.wheat"), "positive", true))
	flow.add_child(_arrow())
	flow.add_child(_chain_node(I18n.t("product.flour"), "warning", false))
	flow.add_child(_arrow())
	flow.add_child(_chain_node(I18n.t("product.bread"), "warning", false))
	box.add_child(flow)
	box.add_child(UiKit.paragraph(I18n.t("city.chain.note"), UiKit.FS_SMALL,
			UiKit.TEXT_DIM, 520.0))
	return box


func _chain_node(text: String, tone: String, modeled: bool) -> Control:
	var box := VBoxContainer.new()
	box.size_flags_horizontal = Control.SIZE_SHRINK_BEGIN
	box.add_theme_constant_override("separation", 2)
	box.add_child(UiKit.status_pill(text, tone))
	box.add_child(UiKit.caption(I18n.t("city.chain.modeled") if modeled \
			else I18n.t("city.chain.unmodeled"), UiKit.TEXT_MUTED))
	return box


func _arrow() -> Label:
	var arrow := UiKit.label("→", UiKit.FS_SUBTITLE, UiKit.TEXT_MUTED)
	arrow.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	return arrow


func _build_cities() -> void:
	var card := UiKit.card(UiKit.SPACE_MD)
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 2)
	card.add_child(box)
	box.add_child(UiKit.caption(I18n.t("prod.tab.cities"), UiKit.BRASS))
	box.add_child(UiKit.rule())
	box.add_child(UiKit.table_header([
		I18n.t("econ.col.city"),
		I18n.t("econ.col.nation"),
		I18n.t("econ.col.production"),
		I18n.t("city.consumed"),
		I18n.t("econ.col.stock"),
		I18n.t("econ.col.deficit"),
		I18n.t("city.coverage"),
	], CITY_WEIGHTS, CITY_ALIGN))

	for city in _sorted_cities():
		box.add_child(_city_row(city))
	_body.add_child(card)


func _city_row(city: Dictionary) -> Control:
	var need := float(city.get("need", 0.0))
	var stock := float(city.get("stockpile", 0.0))
	var coverage := INF if need <= 1e-9 else stock / need
	var deficit := float(city.get("deficit", 0.0))

	var row := UiKit.row_button()
	row.add_child(UiKit.table_row([
		I18n.city_name(city["id"], city["name"]),
		I18n.nation_name(city["nation"]),
		UiKit.number(float(city["production"])) + " t",
		UiKit.number(float(city["consumed"])) + " t",
		UiKit.number(stock) + " t",
		UiKit.number(deficit) + " t",
		_coverage_text(coverage),
	], CITY_WEIGHTS, [
		UiKit.TEXT, UiKit.nation_color(city["nation"]), UiKit.BRASS,
		UiKit.TEXT_DIM, UiKit.TEXT,
		UiKit.NEGATIVE if deficit > 0.0 else UiKit.POSITIVE,
		_coverage_color(coverage),
	], CITY_ALIGN))
	var id := String(city["id"])
	row.pressed.connect(func() -> void: city_requested.emit(id))
	return row


func _totals() -> Dictionary:
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
		"stock": stock,
		"deficit": deficit,
		"coverage": INF if need <= 1e-9 else stock / need,
	}


func _sorted_cities() -> Array:
	var cities: Array = _summary.get("cities", []).duplicate()
	cities.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return float(a.get("production", 0.0)) > float(b.get("production", 0.0)))
	return cities


func _producers() -> Array:
	var producers: Array = []
	for city in _sorted_cities():
		if float(city.get("production", 0.0)) > 0.0:
			producers.append(city)
	return producers


static func _tons(value: float) -> String:
	return UiKit.number(value) + " t"


func _coverage_text(days: float) -> String:
	if is_inf(days):
		return I18n.t("common.na")
	return UiKit.number(days, 1) + " " + I18n.t("common.days_short")


func _coverage_color(days: float) -> Color:
	if is_inf(days) or days >= 7.0:
		return UiKit.POSITIVE
	if days >= 2.0:
		return UiKit.WARNING
	return UiKit.NEGATIVE
