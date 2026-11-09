extends VBoxContainer

## Painel de cidade com abas, alimentado apenas pelo status publicado pela
## simulação. Nenhum indicador é inventado: campos que o modelo não possui
## aparecem como "não modelado", com explicação.
##
## A estrutura é montada uma vez por (cidade, aba, idioma) e depois apenas
## atualizada no lugar — rolagem e foco não são perdidos a cada dia simulado.

signal navigate_city(city_id: String, tab: String)

const TABS := ["overview", "population", "production", "market", "resources"]
const TAB_LABELS := {
	"overview": "city.tab.overview",
	"population": "city.tab.population",
	"production": "city.tab.production",
	"market": "city.tab.market",
	"resources": "city.tab.resources",
}
const SECTORS := ["steel", "manufacturing", "refining", "power", "storage"]
const RESOURCE_TABLE_WEIGHTS := [1.2, 1.0, 1.0, 0.8, 0.9]
const RESOURCE_TABLE_ALIGN := [false, false, true, true, true]
const MAX_DEPOSIT_ROWS := 8

var _summary: Dictionary = {}
var _city_id := ""
var _tab := "overview"
var _structure_key := ""

var _history := EconomyHistory.new()
var _geo := GeographyService.new()

var _tab_bar: HBoxContainer
var _tab_buttons := {}
var _pills: HBoxContainer
var _coverage_pill: PanelContainer
var _deficit_pill: PanelContainer
var _body: VBoxContainer
var _scroll: ScrollContainer

var _values := {}
var _charts := {}
var _meters := {}
var _rows := {}


func _ready() -> void:
	add_theme_constant_override("separation", UiKit.SPACE_MD)
	size_flags_vertical = Control.SIZE_EXPAND_FILL

	add_child(_build_header())
	add_child(UiKit.rule())
	add_child(_build_scroll())


func _build_header() -> HBoxContainer:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", UiKit.SPACE_SM)

	_tab_bar = UiKit.tab_bar()
	for id in TABS:
		var button := UiKit.tab_button("")
		button.pressed.connect(_set_tab.bind(id))
		_tab_bar.add_child(button)
		_tab_buttons[id] = button
	row.add_child(_tab_bar)

	row.add_child(UiKit.spacer())

	_pills = HBoxContainer.new()
	_pills.add_theme_constant_override("separation", UiKit.SPACE_SM)
	_pills.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	_coverage_pill = UiKit.status_pill("", "neutral")
	_deficit_pill = UiKit.status_pill("", "neutral")
	_pills.add_child(_coverage_pill)
	_pills.add_child(_deficit_pill)
	row.add_child(_pills)

	return row


func _build_scroll() -> ScrollContainer:
	_scroll = ScrollContainer.new()
	_scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED

	_body = VBoxContainer.new()
	_body.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_body.add_theme_constant_override("separation", UiKit.SPACE_MD)
	_scroll.add_child(_body)

	return _scroll


# --- API ---------------------------------------------------------------------

func open_city(city_id: String, summary: Dictionary) -> void:
	var changed := city_id != _city_id
	_city_id = city_id
	_summary = summary
	if changed:
		_history.clear()
	_sample_history()
	_structure_key = ""
	_render()


func refresh(summary: Dictionary) -> void:
	_summary = summary
	_sample_history()
	_render()


func current_tab() -> String:
	return _tab


func current_city_id() -> String:
	return _city_id


func set_tab(tab: String) -> void:
	if not TAB_LABELS.has(tab):
		return
	_tab = tab
	_structure_key = ""
	_render()


func _set_tab(tab: String) -> void:
	set_tab(tab)


# --- Ciclo de desenho ---------------------------------------------------------

func _sample_history() -> void:
	var city := _city()
	if city.is_empty():
		return
	_history.sample(int(_summary.get("day", 0)), city)


func _render() -> void:
	var city := _city()
	if city.is_empty() or _body == null:
		return

	_retranslate_tabs()
	for id in _tab_buttons:
		UiKit.set_tab_active(_tab_buttons[id], id == _tab)

	var key := "%s|%s|%s" % [_city_id, _tab, I18n.locale]
	if key != _structure_key:
		_structure_key = key
		_build_body(city)

	_refresh_values(city)


func _retranslate_tabs() -> void:
	for id in _tab_buttons:
		_tab_buttons[id].text = I18n.t(TAB_LABELS[id])


func _build_body(city: Dictionary) -> void:
	var position := _scroll.scroll_vertical
	UiKit.clear(_body)
	_values.clear()
	_charts.clear()
	_meters.clear()
	_rows.clear()

	_body.add_child(_glance_strip())

	var grid := GridContainer.new()
	grid.columns = 2
	grid.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	grid.add_theme_constant_override("h_separation", UiKit.SPACE_MD)
	grid.add_theme_constant_override("v_separation", UiKit.SPACE_MD)
	_body.add_child(grid)

	match _tab:
		"population":
			_build_population(grid, city)
		"production":
			_build_production(grid, city)
		"market":
			_build_market(grid, city)
		"resources":
			_build_resources(grid, city)
		_:
			_build_overview(grid, city)

	_scroll.set_deferred("scroll_vertical", position)


func _refresh_values(city: Dictionary) -> void:
	var pop := float(city.get("population", 0.0))
	var production := float(city.get("production", 0.0))
	var need := float(city.get("need", 0.0))
	var consumed := float(city.get("consumed", 0.0))
	var stock := float(city.get("stockpile", 0.0))
	var deficit := float(city.get("deficit", 0.0))
	var coverage := _coverage(stock, need)
	var jobs := float(city.get("jobs", 0.0))
	var income := float(city.get("income", 0.0))
	var industrialization := float(city.get("industrialization", 0.0))

	_set_value("glance.population", _compact(pop))
	_set_value("glance.production", _tons(production) + I18n.t("common.per_day"))
	_set_value("glance.demand", _tons(need) + I18n.t("common.per_day"))
	_set_value("glance.coverage", _coverage_text(coverage), _coverage_color(coverage))

	UiKit.apply_pill(_coverage_pill,
			I18n.t("city.glance.coverage") + " · " + _coverage_text(coverage),
			_coverage_tone(coverage))
	UiKit.apply_pill(_deficit_pill,
			I18n.t("city.deficit") + " · " + _tons(deficit),
			"negative" if deficit > 0.0 else "positive")

	_set_value("balance.production", _tons(production) + I18n.t("common.per_day"))
	_set_value("balance.consumed", _tons(consumed) + I18n.t("common.per_day"))
	_set_value("balance.need", _tons(need) + I18n.t("common.per_day"))
	_set_value("balance.stock", _tons(stock))
	_set_value("balance.deficit", _tons(deficit),
			UiKit.NEGATIVE if deficit > 0.0 else UiKit.POSITIVE)
	_set_value("balance.coverage", _coverage_text(coverage), _coverage_color(coverage))
	_set_meter("balance.coverage", coverage / 7.0, _coverage_color(coverage))

	_set_value("jobs.population", _compact(pop))
	_set_value("jobs.jobs", UiKit.number(jobs))
	_set_value("jobs.per_k", UiKit.number(jobs / maxf(pop, 1.0) * 1000.0, 1))
	_set_value("jobs.income", UiKit.number(income, 2), _level_color(income))
	_set_meter("jobs.income", income, _level_color(income))
	_set_value("jobs.industry", "%.0f%%" % (industrialization * 100.0),
			_level_color(industrialization))
	_set_meter("jobs.industry", industrialization, _level_color(industrialization))

	var orders := _related_orders()
	_set_value("flow.outgoing", _tons(_orders_total(orders, true)))
	_set_value("flow.incoming", _tons(_orders_total(orders, false)))
	_set_value("flow.orders", UiKit.number(orders.size()))
	_set_value("flow.transit", _tons(float(city.get("in_transit", 0.0))))
	_set_value("flow.dispatched", _tons(float(city.get("dispatched", 0.0))))

	_set_value("pop.consumed", _tons(consumed) + I18n.t("common.per_day"))
	_set_value("pop.consumed_kg", "%s kg" % UiKit.number(consumed * 1000.0 / maxf(pop, 1.0), 2))
	_set_value("pop.need_kg", "%s kg" % UiKit.number(need * 1000.0 / maxf(pop, 1.0), 2))
	_set_value("pop.attendance", "%.0f%%" % (minf(1.0, consumed / maxf(need, 1e-9)) * 100.0),
			_level_color(minf(1.0, consumed / maxf(need, 1e-9))))
	_set_meter("pop.attendance", minf(1.0, consumed / maxf(need, 1e-9)),
			_level_color(minf(1.0, consumed / maxf(need, 1e-9))))

	_refresh_charts()

	_set_value("prod.production", _tons(production) + I18n.t("common.per_day"))
	_set_value("prod.consumed", _tons(consumed) + I18n.t("common.per_day"))
	_set_value("prod.stock", _tons(stock))
	_set_value("prod.coverage", _coverage_text(coverage), _coverage_color(coverage))
	_set_value("prod.industry", "%.0f%%" % (industrialization * 100.0),
			_level_color(industrialization))

	_set_value("market.supply", _tons(production) + I18n.t("common.per_day"))
	_set_value("market.demand", _tons(need) + I18n.t("common.per_day"))
	_set_value("market.stock", _tons(stock))
	_set_value("market.shortage", _tons(deficit),
			UiKit.NEGATIVE if deficit > 0.0 else UiKit.POSITIVE)


func _refresh_charts() -> void:
	if _charts.has("flow"):
		(_charts["flow"] as UiChart).set_series([
			{"values": _history.series("wheat_stock"), "color": UiKit.STEEL, "fill": false},
			{"values": _history.series("deficit"), "color": UiKit.NEGATIVE, "fill": false},
		])
	if _charts.has("population"):
		(_charts["population"] as UiChart).set_series([
			{"values": _history.series("population"), "color": UiKit.BRASS, "fill": true},
		])
	if _charts.has("income"):
		(_charts["income"] as UiChart).set_series([
			{"values": _history.series("income"), "color": UiKit.POSITIVE, "fill": false},
			{"values": _history.series("industrialization"), "color": UiKit.STEEL, "fill": false},
		])
	if _charts.has("market"):
		(_charts["market"] as UiChart).set_series([
			{"values": _history.series("wheat_stock"), "color": UiKit.STEEL, "fill": true},
			{"values": _history.series("deficit"), "color": UiKit.NEGATIVE, "fill": false},
		])


# --- Faixa de destaque --------------------------------------------------------

func _glance_strip() -> Control:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", UiKit.SPACE_MD)
	for key in ["population", "production", "demand", "coverage"]:
		var card := PanelContainer.new()
		card.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		var style := UiKit.flat(UiKit.PANEL_RAISED, UiKit.EDGE_SOFT, 10.0)
		style.content_margin_top = 8.0
		style.content_margin_bottom = 8.0
		card.add_theme_stylebox_override("panel", style)

		var box := VBoxContainer.new()
		box.add_theme_constant_override("separation", 0)
		box.add_child(UiKit.caption(I18n.t("city.glance." + key)))
		var value := UiKit.label("—", UiKit.FS_TITLE, UiKit.TEXT)
		box.add_child(value)
		card.add_child(box)
		row.add_child(card)
		_values["glance." + key] = value
	return row


# --- Abas ---------------------------------------------------------------------

func _build_overview(grid: GridContainer, city: Dictionary) -> void:
	var balance := _section(grid, "city.balance.title", "—")
	_metric(balance, "balance.production", "city.production")
	_metric(balance, "balance.consumed", "city.consumed")
	_metric(balance, "balance.need", "city.need")
	_metric(balance, "balance.stock", "city.stock")
	_metric(balance, "balance.deficit", "city.deficit")
	_metric(balance, "balance.coverage", "city.coverage")
	_meters["balance.coverage"] = _inline_meter(balance)

	var jobs := _section(grid, "city.jobs.title", "city.income_index.note")
	_metric(jobs, "jobs.population", "city.glance.population")
	_metric(jobs, "jobs.jobs", "city.jobs")
	_metric(jobs, "jobs.per_k", "city.jobs_per_k")
	_metric(jobs, "jobs.income", "city.income_index")
	_meters["jobs.income"] = _inline_meter(jobs)
	_metric(jobs, "jobs.industry", "city.industrialization")
	_meters["jobs.industry"] = _inline_meter(jobs)

	var flow := _section(grid, "city.flow.title", "")
	_metric(flow, "flow.outgoing", "city.flow.outgoing")
	_metric(flow, "flow.incoming", "city.flow.incoming")
	_metric(flow, "flow.orders", "city.flow.orders")
	_metric(flow, "flow.transit", "city.transit")
	_metric(flow, "flow.dispatched", "city.dispatched")

	var history := _section(grid, "city.history.title", "city.history.note")
	var chart := UiChart.new(96.0)
	chart.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	history.add_content(chart)
	_charts["flow"] = chart
	history.add_content(_legend([
		[I18n.t("city.stock"), UiKit.STEEL],
		[I18n.t("city.deficit"), UiKit.NEGATIVE],
	]))


func _build_population(grid: GridContainer, _city: Dictionary) -> void:
	var work := _section(grid, "city.jobs.title", "city.income_index.note")
	_metric(work, "jobs.population", "city.glance.population")
	_metric(work, "jobs.jobs", "city.jobs")
	_metric(work, "jobs.per_k", "city.jobs_per_k")
	_metric(work, "jobs.income", "city.income_index")
	_meters["jobs.income"] = _inline_meter(work)
	_metric(work, "jobs.industry", "city.industrialization")
	_meters["jobs.industry"] = _inline_meter(work)

	var consumption := _section(grid, "city.consumed", "—")
	_metric(consumption, "pop.consumed", "city.consumed")
	_metric(consumption, "pop.consumed_kg", "city.per_person_kg")
	_metric(consumption, "pop.need_kg", "city.need_per_person_kg")
	_metric(consumption, "pop.attendance", "city.attendance")
	_meters["pop.attendance"] = _inline_meter(consumption)

	var trend := _section(grid, "city.history.title", "city.history.note")
	var chart := UiChart.new(96.0)
	chart.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	trend.add_content(chart)
	_charts["population"] = chart
	trend.add_content(_legend([[I18n.t("city.glance.population"), UiKit.BRASS]]))

	var income := _section(grid, "city.income_index", "city.history.note")
	var income_chart := UiChart.new(96.0)
	income_chart.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	income.add_content(income_chart)
	_charts["income"] = income_chart
	income.add_content(_legend([
		[I18n.t("city.income_index"), UiKit.POSITIVE],
		[I18n.t("city.industrialization"), UiKit.STEEL],
	]))

	grid.add_child(_unavailable_card())


func _build_production(grid: GridContainer, city: Dictionary) -> void:
	grid.add_child(_chain_card())

	var local := _section(grid, "city.tab.production", "—")
	_metric(local, "prod.production", "city.production")
	_metric(local, "prod.consumed", "city.consumed")
	_metric(local, "prod.stock", "city.stock")
	_metric(local, "prod.coverage", "city.coverage")
	_metric(local, "prod.industry", "city.industrialization")

	grid.add_child(_industry_card(city))
	grid.add_child(_producers_card())


func _build_market(grid: GridContainer, _city: Dictionary) -> void:
	var market := _section(grid, "city.supply", "—")
	_metric(market, "market.supply", "city.supply")
	_metric(market, "market.demand", "city.demand_short")
	_metric(market, "market.stock", "city.stock")
	_metric(market, "market.shortage", "city.deficit")

	var history := _section(grid, "city.history.title", "city.history.note")
	var chart := UiChart.new(96.0)
	chart.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	history.add_content(chart)
	_charts["market"] = chart
	history.add_content(_legend([
		[I18n.t("city.stock"), UiKit.STEEL],
		[I18n.t("city.deficit"), UiKit.NEGATIVE],
	]))

	grid.add_child(_unmodeled_card("city.market.unmodeled.title",
			"city.market.unmodeled.body"))
	grid.add_child(_orders_card())


func _build_resources(grid: GridContainer, city: Dictionary) -> void:
	_geo.load_data()
	var center := Vector2(float(city.get("x", 0.0)), float(city.get("y", 0.0)))
	var id := String(city.get("id", ""))
	var summary := _geo.territory_summary(id, center)

	if summary.is_empty():
		grid.add_child(_state_card(UiKit.empty_state(
				I18n.t("city.resources.title"), I18n.t("city.resources.empty"))))
		return

	var territory := _section(grid, "city.resources.title", String(summary.get("territory", "")))
	_metric_text(territory, "city.resources.climate", _value_label(String(summary.get("climate", ""))))
	_metric_text(territory, "city.resources.relief", _value_label(String(summary.get("relief", ""))))
	_metric_text(territory, "city.resources.wheat",
			"%.0f%%" % (float(summary.get("wheat", 0.0)) * 100.0))
	territory.add_content(UiKit.meter(float(summary.get("wheat", 0.0)), UiKit.POSITIVE, 5))
	_metric_text(territory, "city.resources.deposits",
			UiKit.number(float(summary.get("deposits", 0))))

	grid.add_child(_deposits_card(id, center))
	grid.add_child(_prospect_card())


# --- Cartões -----------------------------------------------------------------

func _section(grid: GridContainer, title_key: String, hint_text: String) -> UiSection:
	# O hint chega como chave de i18n; textos literais ("—", "") voltam iguais.
	var section := UiSection.new(I18n.t(title_key), I18n.t(hint_text))
	section.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	grid.add_child(section)
	return section


func _metric(section: UiSection, key: String, label_key: String) -> void:
	_values[key] = section.add_metric(I18n.t(label_key), "—")


func _metric_text(section: UiSection, label_key: String, value_text: String) -> void:
	section.add_metric(I18n.t(label_key), value_text)


func _inline_meter(section: UiSection) -> Control:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", UiKit.SPACE_SM)
	var meter := UiKit.meter(0.0, UiKit.STEEL, 4)
	meter.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(meter)
	section.add_content(row)
	return meter


func _unavailable_card() -> Control:
	return _unmodeled_card("city.unavailable.title", "city.unavailable.body")


func _unmodeled_card(title_key: String, body_key: String) -> Control:
	var card := UiKit.card(UiKit.SPACE_MD)
	card.size_flags_horizontal = Control.SIZE_EXPAND_FILL

	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", UiKit.SPACE_SM)
	card.add_child(box)

	var head := HBoxContainer.new()
	head.add_theme_constant_override("separation", UiKit.SPACE_SM)
	head.add_child(UiKit.status_pill(I18n.t("city.chain.unmodeled"), "warning"))
	head.add_child(UiKit.label(I18n.t(title_key), UiKit.FS_SUBTITLE, UiKit.TEXT))
	box.add_child(head)
	box.add_child(UiKit.paragraph(I18n.t(body_key), UiKit.FS_SMALL, UiKit.TEXT_DIM, 460.0))
	return card


func _state_card(state: Control) -> Control:
	var card := UiKit.card(UiKit.SPACE_MD)
	card.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	card.add_child(state)
	return card


func _chain_card() -> Control:
	var card := UiKit.card(UiKit.SPACE_MD)
	card.size_flags_horizontal = Control.SIZE_EXPAND_FILL

	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", UiKit.SPACE_SM)
	card.add_child(box)
	box.add_child(UiKit.caption(I18n.t("city.chain.title"), UiKit.BRASS))

	var flow := HBoxContainer.new()
	flow.add_theme_constant_override("separation", UiKit.SPACE_SM)
	flow.add_child(_chain_node(I18n.t("product.wheat"), "positive", true))
	flow.add_child(_chain_arrow())
	flow.add_child(_chain_node(I18n.t("product.flour"), "warning", false))
	flow.add_child(_chain_arrow())
	flow.add_child(_chain_node(I18n.t("product.bread"), "warning", false))
	box.add_child(flow)

	box.add_child(UiKit.paragraph(I18n.t("city.chain.note"), UiKit.FS_SMALL,
			UiKit.TEXT_DIM, 460.0))
	return card


func _chain_node(text: String, tone: String, modeled: bool) -> Control:
	var box := VBoxContainer.new()
	box.size_flags_horizontal = Control.SIZE_SHRINK_BEGIN
	box.add_theme_constant_override("separation", 2)

	var head := HBoxContainer.new()
	head.add_theme_constant_override("separation", UiKit.SPACE_XS)
	head.add_child(UiKit.status_pill(text, tone))
	box.add_child(head)
	box.add_child(UiKit.caption(I18n.t("city.chain.modeled") if modeled \
			else I18n.t("city.chain.unmodeled"), UiKit.TEXT_MUTED))
	return box


func _chain_arrow() -> Label:
	var arrow := UiKit.label("→", UiKit.FS_SUBTITLE, UiKit.TEXT_MUTED)
	arrow.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	return arrow


func _industry_card(city: Dictionary) -> Control:
	var card := UiKit.card(UiKit.SPACE_MD)
	card.size_flags_horizontal = Control.SIZE_EXPAND_FILL

	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", UiKit.SPACE_SM)
	card.add_child(box)
	box.add_child(UiKit.caption(I18n.t("city.industry.title"), UiKit.BRASS))

	var profile := IndustryProfile.derive(city)
	for sector in SECTORS:
		_row_share(box, I18n.t("city.industry." + sector),
				float(profile.get(sector, 0.0)))
	box.add_child(UiKit.paragraph(I18n.t("city.industry.note"), UiKit.FS_CAPTION,
			UiKit.TEXT_MUTED, 460.0))
	return card


func _row_share(parent: VBoxContainer, text: String, ratio: float) -> void:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", UiKit.SPACE_SM)
	row.add_child(UiKit.label(text, UiKit.FS_SMALL, UiKit.TEXT_DIM))
	row.add_child(UiKit.spacer())
	row.add_child(UiKit.label("%.0f%%" % (ratio * 100.0), UiKit.FS_SMALL, UiKit.TEXT))
	parent.add_child(row)
	parent.add_child(UiKit.meter(ratio, UiKit.STEEL, 4))


func _producers_card() -> Control:
	var card := UiKit.card(UiKit.SPACE_MD)
	card.size_flags_horizontal = Control.SIZE_EXPAND_FILL

	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 2)
	card.add_child(box)
	box.add_child(UiKit.caption(I18n.t("city.producers.title"), UiKit.BRASS))
	box.add_child(UiKit.rule())

	var producers := _producers()
	if producers.is_empty():
		box.add_child(UiKit.paragraph(I18n.t("city.producers.empty"), UiKit.FS_SMALL,
				UiKit.TEXT_MUTED, 420.0))
		return card

	box.add_child(UiKit.table_header([
		I18n.t("econ.col.city"),
		I18n.t("econ.col.nation"),
		I18n.t("econ.col.production"),
		I18n.t("econ.col.deficit"),
	], [1.4, 1.0, 1.0, 1.0], [false, false, true, true]))

	for city in producers:
		var row := UiKit.row_button()
		var deficit := float(city["deficit"])
		row.add_child(UiKit.table_row([
			I18n.city_name(city["id"], city["name"]),
			I18n.nation_name(city["nation"]),
			UiKit.number(float(city["production"])) + " t",
			UiKit.number(deficit) + " t",
		], [1.4, 1.0, 1.0, 1.0], [
			UiKit.TEXT, UiKit.nation_color(city["nation"]), UiKit.BRASS,
			UiKit.NEGATIVE if deficit > 0.0 else UiKit.POSITIVE,
		], [false, false, true, true]))
		var id := String(city["id"])
		row.pressed.connect(func() -> void: navigate_city.emit(id, "production"))
		box.add_child(row)

	return card


func _orders_card() -> Control:
	var card := UiKit.card(UiKit.SPACE_MD)
	card.size_flags_horizontal = Control.SIZE_EXPAND_FILL

	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 2)
	card.add_child(box)
	box.add_child(UiKit.caption(I18n.t("city.orders.title"), UiKit.BRASS))
	box.add_child(UiKit.rule())

	var orders := _related_orders()
	if orders.is_empty():
		box.add_child(UiKit.paragraph(I18n.t("city.orders.empty"), UiKit.FS_SMALL,
				UiKit.TEXT_MUTED, 420.0))
		return card

	box.add_child(UiKit.table_header([
		I18n.t("hud.day_short"),
		I18n.t("trade.col.from"),
		I18n.t("trade.col.to"),
		I18n.t("trade.col.amount"),
		I18n.t("trade.col.freight"),
	], [0.6, 1.3, 1.3, 1.0, 1.0], [true, false, false, true, true]))

	for order in orders.slice(0, 8):
		box.add_child(UiKit.table_row([
			UiKit.number(float(order.get("day", 0))),
			I18n.city_name(order["from_city"], order["from_city"]),
			I18n.city_name(order["to_city"], order["to_city"]),
			UiKit.number(float(order["amount"])) + " t",
			UiKit.number(float(order["freight_cost"])),
		], [0.6, 1.3, 1.3, 1.0, 1.0], [
			UiKit.TEXT_MUTED, UiKit.TEXT, UiKit.TEXT, UiKit.STEEL, UiKit.BRASS,
		], [true, false, false, true, true]))

	return card


func _deposits_card(city_id: String, center: Vector2) -> Control:
	var card := UiKit.card(UiKit.SPACE_MD)
	card.size_flags_horizontal = Control.SIZE_EXPAND_FILL

	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 2)
	card.add_child(box)
	box.add_child(UiKit.caption(I18n.t("city.resources.deposits"), UiKit.BRASS))
	box.add_child(UiKit.rule())

	var known: Array = []
	for deposit in _geo.deposits_for(city_id, center):
		if String(deposit.get("status", "")) != "Unknown":
			known.append(deposit)

	if known.is_empty():
		box.add_child(UiKit.paragraph(I18n.t("city.resources.empty"), UiKit.FS_SMALL,
				UiKit.TEXT_MUTED, 420.0))
		return card

	box.add_child(UiKit.table_header([
		I18n.t("city.resources.table.resource"),
		I18n.t("city.resources.table.status"),
		I18n.t("city.resources.table.quantity"),
		I18n.t("city.resources.table.quality"),
		I18n.t("city.resources.table.depth"),
	], RESOURCE_TABLE_WEIGHTS, RESOURCE_TABLE_ALIGN))

	for deposit in known.slice(0, MAX_DEPOSIT_ROWS):
		box.add_child(UiKit.table_row([
			I18n.t("resource.%s" % deposit.get("resource", "")),
			I18n.t("deposit.%s" % deposit.get("status", "")),
			"%s kt" % UiKit.number(float(deposit.get("recoverable_kt", 0.0))),
			UiKit.number(float(deposit.get("quality", 0.0)), 2),
			"%d m" % int(deposit.get("depth_m", 0)),
		], RESOURCE_TABLE_WEIGHTS, [
			UiKit.TEXT, UiKit.STEEL, UiKit.TEXT, UiKit.TEXT_DIM, UiKit.TEXT_DIM,
		], RESOURCE_TABLE_ALIGN))

	if known.size() > MAX_DEPOSIT_ROWS:
		box.add_child(UiKit.label(I18n.t("city.resources.more",
				{"count": known.size() - MAX_DEPOSIT_ROWS}), UiKit.FS_CAPTION,
				UiKit.TEXT_MUTED))
	return card


func _prospect_card() -> Control:
	var card := UiKit.card(UiKit.SPACE_MD)
	card.size_flags_horizontal = Control.SIZE_EXPAND_FILL

	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", UiKit.SPACE_SM)
	card.add_child(box)
	box.add_child(UiKit.caption(I18n.t("city.resources.prospect.title"), UiKit.BRASS))
	box.add_child(UiKit.paragraph(I18n.t("city.resources.prospect.body"), UiKit.FS_SMALL,
			UiKit.TEXT_DIM, 460.0))

	var action := UiKit.secondary_button(I18n.t("city.resources.prospect.action"))
	action.disabled = true
	action.tooltip_text = I18n.t("city.resources.prospect.disabled")
	action.size_flags_horizontal = Control.SIZE_SHRINK_BEGIN
	box.add_child(action)
	box.add_child(UiKit.caption(I18n.t("city.resources.prospect.disabled"), UiKit.TEXT_MUTED))
	return card


# --- Auxiliares ---------------------------------------------------------------

func _city() -> Dictionary:
	for city in _summary.get("cities", []):
		if String(city["id"]) == _city_id:
			return city
	return {}


func _producers() -> Array:
	var producers: Array = []
	for city in _summary.get("cities", []):
		if float(city.get("production", 0.0)) > 0.0:
			producers.append(city)
	producers.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return float(a.get("production", 0.0)) > float(b.get("production", 0.0)))
	return producers


func _related_orders() -> Array:
	var log: Array = _summary.get("trade_log", _summary.get("trade_orders", []))
	var related: Array = []
	for order in log:
		if String(order.get("from_city", "")) == _city_id \
				or String(order.get("to_city", "")) == _city_id:
			related.append(order)
	return related


func _orders_total(orders: Array, outgoing: bool) -> float:
	var total := 0.0
	for order in orders:
		var key := "from_city" if outgoing else "to_city"
		if String(order.get(key, "")) == _city_id:
			total += float(order.get("amount", 0.0))
	return total


func _set_value(key: String, text: String, color: Color = UiKit.TEXT) -> void:
	var node = _values.get(key)
	if node is Label:
		(node as Label).text = text
		(node as Label).add_theme_color_override("font_color", color)
	elif node is PanelContainer:
		var label := (node as PanelContainer).get_child(0) as Label
		if label != null:
			label.text = text


func _set_meter(key: String, ratio: float, color: Color) -> void:
	var meter := _meters.get(key) as Control
	if meter == null or meter.get_child_count() < 2:
		return
	var fill := meter.get_child(1) as ColorRect
	if fill == null:
		return
	fill.anchor_right = clampf(ratio, 0.0, 1.0)
	fill.offset_right = 0.0
	fill.color = color


func _legend(items: Array) -> Control:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", UiKit.SPACE_MD)
	for item in items:
		var pair := HBoxContainer.new()
		pair.add_theme_constant_override("separation", 4)
		var swatch := ColorRect.new()
		swatch.color = item[1]
		swatch.custom_minimum_size = Vector2(8.0, 8.0)
		swatch.size_flags_vertical = Control.SIZE_SHRINK_CENTER
		swatch.mouse_filter = Control.MOUSE_FILTER_IGNORE
		pair.add_child(swatch)
		pair.add_child(UiKit.label(String(item[0]), UiKit.FS_CAPTION, UiKit.TEXT_MUTED))
		row.add_child(pair)
	return row


func _value_label(raw: String) -> String:
	if raw.is_empty():
		return I18n.t("common.na")
	var key := "climate." + raw
	var translated := I18n.t(key)
	if translated == key:
		key = "relief." + raw
		translated = I18n.t(key)
	return raw.replace("_", " ").capitalize() if translated == key else translated


static func _tons(value: float) -> String:
	return UiKit.number(value) + " t"


static func _compact(value: float) -> String:
	if value >= 1_000_000.0:
		return UiKit.number(value / 1_000_000.0, 1) + " M"
	if value >= 1_000.0:
		return UiKit.number(value / 1_000.0, 1) + " k"
	return UiKit.number(value)


static func _coverage(stock: float, need: float) -> float:
	return INF if need <= 1e-9 else stock / need


static func _coverage_text(days: float) -> String:
	if is_inf(days):
		return I18n.t("common.na")
	return UiKit.number(days, 1) + " " + I18n.t("common.days_short")


static func _coverage_color(days: float) -> Color:
	if is_inf(days) or days >= 7.0:
		return UiKit.POSITIVE
	if days >= 2.0:
		return UiKit.WARNING
	return UiKit.NEGATIVE


static func _coverage_tone(days: float) -> String:
	if is_inf(days) or days >= 7.0:
		return "positive"
	if days >= 2.0:
		return "warning"
	return "negative"


static func _level_color(value: float) -> Color:
	if value >= 0.66:
		return UiKit.POSITIVE
	if value >= 0.33:
		return UiKit.WARNING
	return UiKit.NEGATIVE
