extends Control

## Ficha de cidade no template T3 (Papelaria): shell lateral com abas de pasta.
##
## O layout vem de `data/ui/templates/ficha.json`; este script só formata os
## valores publicados pela simulação e atualiza os nós marcados com `slot` no
## lugar — trocar de aba, de cidade ou de idioma reconstrói a folha, mas um dia
## novo apenas reescreve textos, medidores, gráficos e tabelas. Nada é inventado:
## indicadores ausentes viram "—" e as abas sem dado mostram a nota de ausência.
##
## A navegação continua no HUD: `navigate_city` empilha outra cidade (tabelas de
## produtores/pedidos) e `back_requested` volta um nível da pilha de contexto.

signal navigate_city(city_id: String, tab: String)
signal back_requested
signal close_requested

const TABS := ["overview", "population", "production", "market", "resources"]
const TAB_LABELS := {
	"overview": "city.tab.overview",
	"population": "city.tab.population",
	"production": "city.tab.production",
	"market": "city.tab.market",
	"resources": "city.tab.resources",
}
const SECTORS := ["steel", "manufacturing", "refining", "power", "storage"]
const MAX_DEPOSIT_ROWS := 8
const MAX_PRODUCER_ROWS := 48
const MAX_ORDER_ROWS := 24
const CHART_SAMPLES := 15
## Cobertura em dias que enche a barra do gráfico de abastecimento.
const COVERAGE_FULL_DAYS := 7.0

var _summary: Dictionary = {}
var _city_id := ""
var _tab := "overview"
var _crumb := ""
var _show_back := false
var _structure_key := ""
var _root: UiTemplateBuilder.TemplateRoot
var _tabs: UiTemplateBuilder.FolderTabs
var _slots := {}
var _producer_ids: Array[String] = []

var _history := EconomyHistory.new()
var _geo := GeographyService.new()
var _geo_loaded := false
var _geo_city_id := ""
var _geo_summary: Dictionary = {}


func _ready() -> void:
	# A folha ocupa a tela inteira, mas só a coluna lateral capta o mouse.
	mouse_filter = Control.MOUSE_FILTER_IGNORE


# --- API ---------------------------------------------------------------------

func open_city(city_id: String, summary: Dictionary) -> void:
	var changed := city_id != _city_id
	_city_id = city_id
	_summary = summary
	if changed:
		_history.clear()
		_geo_city_id = ""
		_geo_summary = {}
	_producer_ids.clear()
	_sample_history()
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
	if _tabs == null:
		_tab = tab
		_render()
		return
	_tabs.select(TABS.find(tab))


func set_crumb(crumb: String, show_back: bool) -> void:
	_crumb = crumb
	_show_back = show_back
	_refresh_nav()


# --- Montagem ----------------------------------------------------------------

func _render() -> void:
	if _city().is_empty():
		_clear()
		return
	var key := "%s|%s" % [_city_id, I18n.locale]
	if key != _structure_key or _root == null:
		_rebuild()
		return
	_refresh()


func _rebuild() -> void:
	_structure_key = "%s|%s" % [_city_id, I18n.locale]
	if _root != null and is_instance_valid(_root):
		remove_child(_root)
		_root.queue_free()
	_root = UiTemplateBuilder.ficha(_context())
	_root.name = "CitySheet"
	_root.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_root.close_requested.connect(func() -> void: close_requested.emit())
	_root.choice_selected.connect(_on_choice)
	add_child(_root)
	_index_slots()
	_refresh()


func _clear() -> void:
	if _root != null and is_instance_valid(_root):
		remove_child(_root)
		_root.queue_free()
	_root = null
	_slots.clear()
	_tabs = null
	_structure_key = ""


func _index_slots() -> void:
	_slots.clear()
	_producer_ids.clear()
	var pending: Array[Node] = [_root]
	while not pending.is_empty():
		var node: Node = pending.pop_back()
		if node is Control:
			var slot := String((node as Control).get_meta("tpl_slot", ""))
			if not slot.is_empty():
				_slots[slot] = node
		for child in node.get_children():
			pending.append(child)

	_tabs = _slots.get("tabs") as UiTemplateBuilder.FolderTabs
	if _tabs != null:
		_tabs.tab_changed.connect(_on_tab_changed)
		_tabs.select(TABS.find(_tab))
	var producers := _slots.get("prod.producers") \
			as UiTemplateBuilder.DataTableView
	if producers != null:
		producers.row_activated.connect(_on_producer_row)


func _on_choice(id: String) -> void:
	if id == "back":
		back_requested.emit()


func _on_tab_changed(index: int) -> void:
	if index < 0 or index >= TABS.size():
		return
	if TABS[index] == _tab:
		return
	_tab = TABS[index]
	_refresh()


func _on_producer_row(index: int) -> void:
	if index < 0 or index >= _producer_ids.size():
		return
	navigate_city.emit(_producer_ids[index], "production")


# --- Ciclo de desenho ---------------------------------------------------------

func _sample_history() -> void:
	var city := _city()
	if city.is_empty():
		return
	_history.sample(int(_summary.get("day", 0)), city)


func _refresh() -> void:
	if _root == null or _city().is_empty():
		return
	_refresh_nav()
	_refresh_overview()
	_refresh_population()
	_refresh_production()
	_refresh_market()
	_refresh_resources()


func _refresh_nav() -> void:
	_set_text("nav.crumb", _crumb)
	_set_text("nav.day", _day_text())
	_set_hidden("nav.back", not _show_back)
	var city := _city()
	_set_text("ov.city", I18n.t("city.title") if city.is_empty() \
			else I18n.city_name(String(city.get("id", "")),
					String(city.get("name", ""))))


# --- Abas ---------------------------------------------------------------------

func _refresh_overview() -> void:
	var city := _city()
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
	var orders := _related_orders()

	_set_text("ov.kpi.population", _compact(pop))
	_set_text("ov.kpi.production", _tons(production) + I18n.t("common.per_day"))
	_set_text("ov.kpi.demand", _tons(need) + I18n.t("common.per_day"))
	_set_text("ov.kpi.coverage", _coverage_text(coverage),
			_coverage_color(coverage))
	_set_hidden("ov.tag.deficit", deficit <= 0.0)

	_set_text("ov.bal.production", _tons(production) + I18n.t("common.per_day"))
	_set_text("ov.bal.consumed", _tons(consumed) + I18n.t("common.per_day"))
	_set_text("ov.bal.need", _tons(need) + I18n.t("common.per_day"))
	_set_text("ov.bal.stock", _tons(stock))
	_set_text("ov.bal.deficit", _tons(deficit),
			UiKit.NEGATIVE if deficit > 0.0 else UiKit.POSITIVE)
	_set_text("ov.bal.coverage_text", _coverage_text(coverage),
			_coverage_color(coverage))
	_set_meter("ov.bal.coverage", coverage / COVERAGE_FULL_DAYS,
			_coverage_color(coverage))

	_set_text("ov.jobs.population", _compact(pop))
	_set_text("ov.jobs.count", UiKit.number(jobs))
	_set_text("ov.jobs.per_k", UiKit.number(jobs / maxf(pop, 1.0) * 1000.0, 1))
	_set_text("ov.jobs.income_text", UiKit.number(income, 2),
			_level_color(income))
	_set_text("ov.jobs.industry_text", _percent(industrialization),
			_level_color(industrialization))
	_set_meter("ov.jobs.income", income, _level_color(income))
	_set_meter("ov.jobs.industry", industrialization,
			_level_color(industrialization))

	_set_text("ov.flow.outgoing", _tons(_orders_total(orders, true)))
	_set_text("ov.flow.incoming", _tons(_orders_total(orders, false)))
	_set_text("ov.flow.orders", UiKit.number(orders.size()))
	_set_text("ov.flow.transit", _tons(float(city.get("in_transit", 0.0))))
	_set_text("ov.flow.dispatched", _tons(float(city.get("dispatched", 0.0))))

	_set_chart("ov.chart", _coverage_series(), _coverage_tones())
	_set_text("ov.chart.range", _chart_range())


func _refresh_population() -> void:
	var city := _city()
	var pop := float(city.get("population", 0.0))
	var need := float(city.get("need", 0.0))
	var consumed := float(city.get("consumed", 0.0))
	var jobs := float(city.get("jobs", 0.0))
	var income := float(city.get("income", 0.0))
	var industrialization := float(city.get("industrialization", 0.0))
	var attendance := minf(1.0, consumed / maxf(need, 1e-9))

	_set_text("pop.jobs.population", _compact(pop))
	_set_text("pop.jobs.count", UiKit.number(jobs))
	_set_text("pop.jobs.per_k", UiKit.number(jobs / maxf(pop, 1.0) * 1000.0, 1))
	_set_text("pop.jobs.income_text", UiKit.number(income, 2),
			_level_color(income))
	_set_text("pop.jobs.industry_text", _percent(industrialization),
			_level_color(industrialization))
	_set_meter("pop.jobs.income", income, _level_color(income))
	_set_meter("pop.jobs.industry", industrialization,
			_level_color(industrialization))

	_set_text("pop.consumed", _tons(consumed) + I18n.t("common.per_day"))
	_set_text("pop.consumed_kg", "%s kg" % UiKit.number(
			consumed * 1000.0 / maxf(pop, 1.0), 2))
	_set_text("pop.need_kg", "%s kg" % UiKit.number(
			need * 1000.0 / maxf(pop, 1.0), 2))
	_set_text("pop.attendance_text", _percent(attendance),
			_level_color(attendance))
	_set_meter("pop.attendance", attendance, _level_color(attendance))

	var population_series: Array[float] = []
	for value in _history.series("population"):
		population_series.append(value)
	_set_chart_values("pop.chart.population", population_series)
	var income_series: Array[float] = []
	for value in _history.series("income"):
		income_series.append(clampf(value, 0.0, 1.0))
	_set_chart_values("pop.chart.income", income_series)


func _refresh_production() -> void:
	var city := _city()
	var production := float(city.get("production", 0.0))
	var consumed := float(city.get("consumed", 0.0))
	var need := float(city.get("need", 0.0))
	var stock := float(city.get("stockpile", 0.0))
	var industrialization := float(city.get("industrialization", 0.0))
	var coverage := _coverage(stock, need)

	_set_text("prod.production", _tons(production) + I18n.t("common.per_day"))
	_set_text("prod.consumed", _tons(consumed) + I18n.t("common.per_day"))
	_set_text("prod.stock", _tons(stock))
	_set_text("prod.coverage", _coverage_text(coverage),
			_coverage_color(coverage))
	_set_text("prod.industry", _percent(industrialization),
			_level_color(industrialization))

	var profile := IndustryProfile.derive(city)
	for sector in SECTORS:
		var ratio := clampf(float(profile.get(sector, 0.0)), 0.0, 1.0)
		_set_meter("prod.ind." + sector, ratio, _level_color(ratio))
		_set_text("prod.ind.%s_text" % sector, _percent(ratio),
				_level_color(ratio))

	var producers := _producers()
	_producer_ids.clear()
	var rows: Array = []
	for city_row in producers.slice(0, MAX_PRODUCER_ROWS):
		var deficit := float(city_row["deficit"])
		rows.append([
			I18n.city_name(city_row["id"], city_row["name"]),
			I18n.nation_name(city_row["nation"]),
			UiKit.number(float(city_row["production"])) + " t",
			{"text": UiKit.number(deficit) + " t",
					"tone": "carimbo" if deficit > 0.0 else "suave"},
		])
		_producer_ids.append(String(city_row["id"]))
	_set_table("prod.producers", rows, "prod.producers.empty", rows.is_empty())


func _refresh_market() -> void:
	var city := _city()
	var production := float(city.get("production", 0.0))
	var need := float(city.get("need", 0.0))
	var stock := float(city.get("stockpile", 0.0))
	var deficit := float(city.get("deficit", 0.0))

	_set_text("mkt.supply", _tons(production) + I18n.t("common.per_day"))
	_set_text("mkt.demand", _tons(need) + I18n.t("common.per_day"))
	_set_text("mkt.stock", _tons(stock))
	_set_text("mkt.shortage", _tons(deficit),
			UiKit.NEGATIVE if deficit > 0.0 else UiKit.POSITIVE)

	var stock_series: Array[float] = []
	var samples: Array[float] = []
	for value in _history.series("wheat_stock"):
		samples.append(value)
	var start := maxi(samples.size() - CHART_SAMPLES, 0)
	for index in range(start, samples.size()):
		stock_series.append(samples[index])
	_set_chart_values("mkt.chart", stock_series)

	var orders := _related_orders()
	var rows: Array = []
	for order in orders.slice(0, MAX_ORDER_ROWS):
		rows.append([
			UiKit.number(float(order.get("day", 0))),
			I18n.city_name(order["from_city"], order["from_city"]),
			I18n.city_name(order["to_city"], order["to_city"]),
			UiKit.number(float(order["amount"])) + " t",
			UiKit.number(float(order["freight_cost"])),
		])
	_set_table("mkt.orders", rows, "mkt.orders.empty", rows.is_empty())


func _refresh_resources() -> void:
	var city := _city()
	if _tab != "resources" and not _geo_loaded:
		return
	if not _geo_loaded:
		_geo.load_data()
		_geo_loaded = true
	var id := String(city.get("id", ""))
	if id != _geo_city_id:
		_geo_city_id = id
		_geo_summary = _geo.territory_summary(id, Vector2(
				float(city.get("x", 0.0)), float(city.get("y", 0.0))))

	var summary := _geo_summary
	var has_territory := not summary.is_empty()
	_set_hidden("res.territory", not has_territory)
	_set_hidden("res.territory_empty", has_territory)
	_set_hidden("res.deposits_section", not has_territory)
	if not has_territory:
		return

	_set_text("res.territory_label", String(summary.get("territory", "")))
	_set_text("res.climate", _value_label(String(summary.get("climate", ""))))
	_set_text("res.relief", _value_label(String(summary.get("relief", ""))))
	_set_text("res.deposits_count", UiKit.number(
			float(summary.get("deposits", 0))))
	var wheat := clampf(float(summary.get("wheat", 0.0)), 0.0, 1.0)
	_set_text("res.fertility_text", _percent(wheat), _level_color(wheat))
	_set_meter("res.fertility", wheat, _level_color(wheat))

	var known: Array = []
	for deposit in _geo.deposits_for_city(id):
		if String(deposit.get("status", "")) != "Unknown":
			known.append(deposit)
	var rows: Array = []
	for deposit in known.slice(0, MAX_DEPOSIT_ROWS):
		rows.append([
			I18n.t("resource.%s" % deposit.get("resource", "")),
			I18n.t("deposit.%s" % deposit.get("status", "")),
			"%s kt" % UiKit.number(float(deposit.get("recoverable_kt", 0.0))),
			UiKit.number(float(deposit.get("quality", 0.0)), 2),
			"%d m" % int(deposit.get("depth_m", 0)),
		])
	_set_table("res.deposits", rows, "res.deposits_empty", rows.is_empty())
	var extra := maxi(known.size() - MAX_DEPOSIT_ROWS, 0)
	_set_hidden("res.deposits_more", extra <= 0)
	if extra > 0:
		_set_text("res.deposits_more",
				I18n.t("city.resources.more", {"count": extra}))


# --- Slots --------------------------------------------------------------------

func _set_text(slot: String, text: String,
		color: Color = Color(0, 0, 0, 0)) -> void:
	var label := _slots.get(slot) as Label
	if label == null:
		return
	label.text = text
	if color.a > 0.0:
		label.add_theme_color_override("font_color", color)


func _set_hidden(slot: String, hidden: bool) -> void:
	var node := _slots.get(slot) as Control
	if node != null:
		node.visible = not hidden


func _set_meter(slot: String, ratio: float, color: Color) -> void:
	var meter := _slots.get(slot) as UiTemplateBuilder.MeterBar
	if meter == null:
		return
	meter.ratio = clampf(ratio, 0.0, 1.0)
	meter.fill = color
	meter.queue_redraw()


func _set_chart(slot: String, values: Array[float],
		tones: Array[Color] = []) -> void:
	var node := _slots.get(slot) as UiTemplateBuilder.BarChart
	if node == null:
		return
	node.values.assign(values)
	node.tones.assign(tones)
	node.queue_redraw()


func _set_chart_values(slot: String, values: Array[float]) -> void:
	var bar := _slots.get(slot) as UiTemplateBuilder.BarChart
	if bar != null:
		bar.values.assign(values)
		bar.queue_redraw()
		return
	var line := _slots.get(slot) as UiTemplateBuilder.LineChart
	if line != null:
		line.values.assign(values)
		line.queue_redraw()


func _set_table(slot: String, rows: Array, empty_slot: String,
		empty: bool) -> void:
	var table := _slots.get(slot) as UiTemplateBuilder.DataTableView
	if table != null:
		table.update_rows(rows)
		_set_hidden(slot, empty)
	_set_hidden(empty_slot, not empty)


# --- Dados --------------------------------------------------------------------

func _context() -> Dictionary:
	var city := _city()
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
	var profile := IndustryProfile.derive(city)
	var industry := {}
	for sector in SECTORS:
		industry[sector] = _percent(float(profile.get(sector, 0.0)))
	var resources := _context_resources(city)

	return {
		"crumb": _crumb,
		"day_label": _day_text(),
		"city": I18n.t("city.title") if city.is_empty() \
				else I18n.city_name(String(city.get("id", "")),
						String(city.get("name", ""))),
		"tag": {"deficit": I18n.t("city.deficit")},
		"kpi": {
			"population": _compact(pop),
			"production": _tons(production) + I18n.t("common.per_day"),
			"demand": _tons(need) + I18n.t("common.per_day"),
			"coverage": _coverage_text(coverage),
		},
		"balance": {
			"production": _tons(production) + I18n.t("common.per_day"),
			"consumed": _tons(consumed) + I18n.t("common.per_day"),
			"need": _tons(need) + I18n.t("common.per_day"),
			"stock": _tons(stock),
			"deficit": _tons(deficit),
			"coverage": _coverage_text(coverage),
		},
		"jobs": {
			"population": _compact(pop),
			"count": UiKit.number(jobs),
			"per_k": UiKit.number(jobs / maxf(pop, 1.0) * 1000.0, 1),
			"income": UiKit.number(income, 2),
			"industry": _percent(industrialization),
		},
		"flow": {
			"outgoing": _tons(0.0),
			"incoming": _tons(0.0),
			"orders": "0",
			"transit": _tons(float(city.get("in_transit", 0.0))),
			"dispatched": _tons(float(city.get("dispatched", 0.0))),
		},
		"chart": {"range": _chart_range()},
		"population": {
			"consumed": _tons(consumed) + I18n.t("common.per_day"),
			"consumed_kg": "%s kg" % UiKit.number(
					consumed * 1000.0 / maxf(pop, 1.0), 2),
			"need_kg": "%s kg" % UiKit.number(need * 1000.0 / maxf(pop, 1.0), 2),
			"attendance": _percent(minf(1.0, consumed / maxf(need, 1e-9))),
		},
		"production": {
			"production": _tons(production) + I18n.t("common.per_day"),
			"consumed": _tons(consumed) + I18n.t("common.per_day"),
			"stock": _tons(stock),
			"coverage": _coverage_text(coverage),
			"industry": _percent(industrialization),
		},
		"industry": industry,
		"market": {
			"supply": _tons(production) + I18n.t("common.per_day"),
			"demand": _tons(need) + I18n.t("common.per_day"),
			"stock": _tons(stock),
			"shortage": _tons(deficit),
		},
		"resources": resources,
	}


## A geografia só é carregada na aba Recursos (custo de carga alto).
func _context_resources(city: Dictionary) -> Dictionary:
	if _tab == "resources" and not city.is_empty():
		if not _geo_loaded:
			_geo.load_data()
			_geo_loaded = true
		var id := String(city.get("id", ""))
		if id != _geo_city_id:
			_geo_city_id = id
			_geo_summary = _geo.territory_summary(id, Vector2(
					float(city.get("x", 0.0)), float(city.get("y", 0.0))))
	if _geo_summary.is_empty():
		return {"territory": "", "climate": "—", "relief": "—",
				"fertility": "—", "deposits": "0"}
	var summary := _geo_summary
	return {
		"territory": String(summary.get("territory", "")),
		"climate": _value_label(String(summary.get("climate", ""))),
		"relief": _value_label(String(summary.get("relief", ""))),
		"fertility": _percent(float(summary.get("wheat", 0.0))),
		"deposits": UiKit.number(float(summary.get("deposits", 0))),
	}


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


## Cobertura de estoque por dia, limitada ao eixo do gráfico (7 dias).
func _coverage_series() -> Array[float]:
	var stock := _history.series("wheat_stock")
	var need := _history.series("wheat_need")
	var values: Array[float] = []
	var start := maxi(stock.size() - CHART_SAMPLES, 0)
	for index in range(start, stock.size()):
		var demand := need[index] if index < need.size() else 0.0
		var days := COVERAGE_FULL_DAYS if demand <= 1e-9 \
				else clampf(stock[index] / demand, 0.0, COVERAGE_FULL_DAYS)
		values.append(days)
	return values


## Tom de cada barra do gráfico de abastecimento: a altura é o estoque em dias,
## mas a cor segue a fração da escala cheia (7 dias = 100%) — vermelho abaixo de
## 70%, alerta entre 70% e 85%, tinta acima. É a leitura da nota do template
## ("vermelho abaixo de 70%"), não um dado novo.
func _coverage_tones() -> Array[Color]:
	var tones: Array[Color] = []
	for days in _coverage_series():
		var ratio := clampf(days / COVERAGE_FULL_DAYS, 0.0, 1.0)
		if ratio < 0.70:
			tones.append(UiKit.CARIMBO)
		elif ratio < 0.85:
			tones.append(UiKit.ALERTA)
		else:
			tones.append(UiKit.TINTA)
	return tones


func _chart_range() -> String:
	var days := _history.series("day")
	if days.size() < 2:
		return ""
	return "%s %d → %s %d" % [I18n.t("hud.day_short"), int(days[0]),
			I18n.t("hud.day_short"), int(days[days.size() - 1])]


func _day_text() -> String:
	return I18n.t("tpl.ficha.day", {"day": int(_summary.get("day", 0))})


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


static func _percent(ratio: float) -> String:
	return "%.0f%%" % (clampf(ratio, 0.0, 1.0) * 100.0)


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


static func _level_color(value: float) -> Color:
	if value >= 0.66:
		return UiKit.POSITIVE
	if value >= 0.33:
		return UiKit.WARNING
	return UiKit.NEGATIVE
