extends VBoxContainer
class_name CityEconomyPanel

## Painel econômico de uma cidade, alimentado apenas pelo status publicado pelo
## EconomySim.
##
## Os cartões são montados uma única vez; cada tick apenas atualiza textos,
## medidores e séries dos gráficos. O histórico é amostrado em um anel
## limitado, portanto nenhuma entidade da simulação é varrida por frame.

signal city_selected(city_id: String)

const PRODUCT_NAMES := ["Trigo", "Farinha", "Pão", "Ferro"]
const PRODUCT_COLORS := [UiKit.BRASS, UiKit.STEEL, UiKit.POSITIVE, UiKit.WARNING]
const TABLE_WEIGHTS := [1.7, 1.0, 1.0, 1.0, 1.0, 1.0]
const TABLE_ALIGN := [false, true, true, true, true, true]
const CHART_HEIGHT := 52.0
const WIDE_COLUMNS := 3
const NARROW_COLUMNS := 2
const WIDE_BREAKPOINT := 1500.0

var _status: Dictionary = {}
var _city_id := ""
var _tab_ids := PackedStringArray()
var _active_tab := ""
var _history := EconomyHistory.new()

var _title: Label
var _subtitle: Label
var _day_label: Label
var _tabs: HBoxContainer
var _grid: GridContainer

var _structure := -1
var _values: Dictionary = {}
var _rows: Dictionary = {}
var _charts: Dictionary = {}
var _meters: Dictionary = {}
var _history_hint: Label


func _init() -> void:
	add_theme_constant_override("separation", 10)
	size_flags_vertical = Control.SIZE_EXPAND_FILL

	var header := HBoxContainer.new()
	header.add_theme_constant_override("separation", 12)
	add_child(header)

	var titles := VBoxContainer.new()
	titles.add_theme_constant_override("separation", 0)
	_title = UiKit.title("", 24)
	titles.add_child(_title)
	_subtitle = UiKit.label("", 12, UiKit.TEXT_DIM)
	titles.add_child(_subtitle)
	header.add_child(titles)
	header.add_child(UiKit.spacer())

	_day_label = UiKit.caption("", UiKit.BRASS)
	_day_label.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	header.add_child(_day_label)

	add_child(UiKit.rule())

	_tabs = UiKit.tab_bar()
	add_child(_tabs)

	var scroll := ScrollContainer.new()
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	add_child(scroll)

	_grid = GridContainer.new()
	_grid.columns = NARROW_COLUMNS
	_grid.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_grid.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_grid.add_theme_constant_override("h_separation", 12)
	_grid.add_theme_constant_override("v_separation", 12)
	scroll.add_child(_grid)


func _ready() -> void:
	resized.connect(_apply_columns)
	_apply_columns()


## Em telas largas o painel usa três colunas; em telas estreitas, duas.
func _apply_columns() -> void:
	var columns := WIDE_COLUMNS if size.x >= WIDE_BREAKPOINT else NARROW_COLUMNS
	if _grid.columns != columns:
		_grid.columns = columns


## Amostra o dia e atualiza a interface. Em execuções longas (auto-teste) use
## `rebuild = false` para amostrar sem redesenhar.
func update(status: Dictionary, rebuild: bool = true) -> void:
	_status = status
	var cities: Array = status.get("cities", [])
	if cities.is_empty():
		return

	if _city_id.is_empty() or _find_city(_city_id).is_empty():
		_city_id = String(cities[0]["id"])
		_history.clear()

	var city := _find_city(_city_id)
	if city.is_empty():
		return

	_history.sample(int(status.get("day", 0)), city)
	if not rebuild:
		return

	_render_tabs(cities)
	_title.text = String(city.get("name", ""))
	_subtitle.text = "%s · %s habitantes" % [
		city.get("id", ""), UiKit.number(float(city.get("population", 0.0)))]
	_day_label.text = "DIA %d" % int(status.get("day", 0))

	var markets: Array = city.get("markets", [])
	if _structure != markets.size():
		_build_sections(markets.size())
		_structure = markets.size()
	_apply_values(city)


func set_city(city_id: String) -> void:
	if city_id == _city_id or _status.is_empty():
		return
	_city_id = city_id
	_history.clear()
	city_selected.emit(city_id)
	update(_status)


func _find_city(city_id: String) -> Dictionary:
	for city in _status.get("cities", []):
		if String(city["id"]) == city_id:
			return city
	return {}


func _render_tabs(cities: Array) -> void:
	var ids := PackedStringArray()
	for city in cities:
		ids.append(String(city["id"]))

	if ids != _tab_ids:
		_tab_ids = ids
		_active_tab = ""
		UiKit.clear(_tabs)
		for city in cities:
			var button := UiKit.tab_button(String(city["name"]))
			var id := String(city["id"])
			button.pressed.connect(func() -> void: set_city(id))
			_tabs.add_child(button)

	# Remarcar as abas recria estilos; só quando a cidade ativa muda.
	if _active_tab == _city_id:
		return
	_active_tab = _city_id
	for index in _tabs.get_child_count():
		UiKit.set_tab_active(_tabs.get_child(index) as Button, ids[index] == _city_id)


# --- Montagem dos cartões (uma vez por estrutura de mercado) ----------------

func _build_sections(product_count: int) -> void:
	UiKit.clear(_grid)
	_values.clear()
	_rows.clear()
	_charts.clear()
	_meters.clear()

	_grid.add_child(_build_population())
	_grid.add_child(_build_income())
	_grid.add_child(_build_gdp())
	_grid.add_child(_build_production(product_count))
	_grid.add_child(_build_market(product_count))
	_grid.add_child(_build_trade(product_count))


func _build_population() -> UiSection:
	var section := UiSection.new("População e emprego", "—")
	_metric(section, "pop_population", "População")
	_metric(section, "pop_labor_force", "Força de trabalho")
	_metric(section, "pop_employed", "Empregados")
	_metric(section, "pop_unemployed", "Desempregados")
	_meters["pop"] = _register_meter(section, "Desemprego no histórico")
	_metric(section, "pop_dependents", "Dependentes")
	section.add_rule()
	_charts["population"] = _chart(section, CHART_HEIGHT, true)
	section.add_note("Taxa de desemprego — série real dos ticks")
	_history_hint = section.hint
	return section


func _build_income() -> UiSection:
	var section := UiSection.new("Renda e padrão de vida", "por dia")
	_metric(section, "income_disposable", "Renda disponível")
	_metric(section, "income_per_capita", "Renda por pessoa")
	_metric(section, "income_wage", "Salário médio")
	_metric(section, "income_consumed", "Consumo alimentar")
	_metric(section, "income_satisfaction", "Satisfação alimentar")
	_meters["satisfaction"] = _register_meter(section, "Satisfação no dia")
	_metric(section, "income_living", "Padrão de vida")
	_metric(section, "income_savings", "Poupança das famílias")
	section.add_rule()
	_charts["income"] = _chart(section, CHART_HEIGHT, true)
	section.add_note("Padrão de vida: cestas alimentares por pessoa e por dia")
	return section


func _build_gdp() -> UiSection:
	var section := UiSection.new("Produto interno bruto", "por dia")
	_metric(section, "gdp_total", "PIB do dia")
	_metric(section, "gdp_per_capita", "PIB per capita")
	_metric(section, "gdp_firm_cash", "Caixa das empresas")
	_metric(section, "gdp_service_cash", "Caixa dos serviços")
	_metric(section, "gdp_budget", "Orçamento público")
	section.add_rule()
	_charts["gdp"] = _chart(section, CHART_HEIGHT + 8.0, false)
	section.add_note("PIB = valor adicionado das construções + receita de serviços")
	return section


func _build_production(product_count: int) -> UiSection:
	var section := UiSection.new("Produção por setor", "toneladas por dia")
	section.add_content(UiKit.table_header(
			["Setor", "Produção", "Estoque", "Preço"], [1.7, 1.0, 1.0, 1.0],
			[false, true, true, true]))

	var rows: Array = []
	for product in product_count:
		var row := UiKit.table_row([
			_product_name(product), "—", "—", "—",
		], [1.7, 1.0, 1.0, 1.0],
			[_product_color(product), UiKit.TEXT, UiKit.TEXT, UiKit.BRASS],
			[false, true, true, true])
		section.add_content(row)
		rows.append(row)
	_rows["production"] = rows

	section.add_rule()
	_metric(section, "production_total", "Produção total")
	_metric(section, "production_stock", "Estoque total")
	return section


func _build_market(product_count: int) -> UiSection:
	var section := UiSection.new("Mercado local", "preço por tonelada")
	section.add_content(UiKit.table_header(
			["Produto", "Preço", "Oferta", "Demanda", "Estoque", "Escassez"],
			TABLE_WEIGHTS, TABLE_ALIGN))

	var rows: Array = []
	for product in product_count:
		var row := UiKit.table_row([
			_product_name(product), "—", "—", "—", "—", "—",
		], TABLE_WEIGHTS, [_product_color(product)], TABLE_ALIGN)
		section.add_content(row)
		rows.append(row)
	_rows["market"] = rows

	section.add_rule()
	_charts["market"] = _chart(section, CHART_HEIGHT + 8.0, false)
	section.add_content(_legend(PRODUCT_NAMES, _product_colors()))
	section.add_note("Histórico de preços — série real dos ticks")
	return section


func _build_trade(product_count: int) -> UiSection:
	var section := UiSection.new("Comércio exterior", "fluxos do dia")
	section.add_content(UiKit.table_header(
			["Produto", "Exportado", "Valor", "Importado", "Valor"],
			[1.7, 1.0, 1.0, 1.0, 1.0], [false, true, true, true, true]))

	var rows: Array = []
	for product in product_count:
		var row := UiKit.table_row([
			_product_name(product), "—", "—", "—", "—",
		], [1.7, 1.0, 1.0, 1.0, 1.0],
			[_product_color(product), UiKit.TEXT, UiKit.BRASS, UiKit.TEXT, UiKit.BRASS],
			[false, true, true, true, true])
		section.add_content(row)
		rows.append(row)
	_rows["trade"] = rows

	section.add_rule()
	_metric(section, "trade_export", "Exportações")
	_metric(section, "trade_import", "Importações")
	_metric(section, "trade_balance", "Saldo comercial")
	section.add_rule()
	_charts["trade"] = _chart(section, CHART_HEIGHT + 8.0, true)
	section.add_content(_legend(["Exportações", "Importações"],
			[UiKit.POSITIVE, UiKit.NEGATIVE]))
	return section


# --- Atualização dos valores (todo tick, sem criar nós) ---------------------

func _apply_values(city: Dictionary) -> void:
	var pop: Dictionary = city.get("pop", {})
	var markets: Array = city.get("markets", [])

	var labor_force := maxf(1.0, float(pop.get("labor_force", 0.0)))
	var unemployed := float(pop.get("unemployed", 0.0))
	var employed := maxf(1.0, float(pop.get("employed", 0.0)))
	var rate := unemployed / labor_force
	var satisfaction := float(pop.get("satisfaction", 0.0))

	_set_value("pop_population", UiKit.number(float(pop.get("population", 0.0))))
	_set_value("pop_labor_force", UiKit.number(labor_force))
	_set_value("pop_employed", UiKit.number(float(pop.get("employed", 0.0))), UiKit.POSITIVE)
	_set_value("pop_unemployed", "%s · %.1f%%" % [
		UiKit.number(unemployed), rate * 100.0], _unemployment_color(rate))
	_set_meter("pop", rate, _unemployment_color(rate))
	_set_value("pop_dependents", UiKit.number(float(pop.get("dependents", 0.0))))

	_set_value("income_disposable", UiKit.number(float(pop.get("disposable", 0.0))))
	_set_value("income_per_capita",
			UiKit.number(float(city.get("disposable_per_capita", 0.0)), 2))
	_set_value("income_wage", UiKit.number(float(pop.get("wage_income", 0.0)) / employed, 2))
	_set_value("income_consumed", "%.1f t/dia" % float(pop.get("consumed", 0.0)))
	_set_value("income_satisfaction", "%.0f%%" % (satisfaction * 100.0),
			_satisfaction_color(satisfaction))
	_set_meter("satisfaction", satisfaction, _satisfaction_color(satisfaction))
	_set_value("income_living", "%.1f cestas/dia" % float(city.get("living_standard", 0.0)),
			UiKit.BRASS)
	_set_value("income_savings", UiKit.number(float(pop.get("savings", 0.0))))

	_set_value("gdp_total", UiKit.number(float(city.get("gdp", 0.0)), 1))
	_set_value("gdp_per_capita", UiKit.number(float(city.get("gdp_per_capita", 0.0)), 4),
			UiKit.BRASS)
	_set_value("gdp_firm_cash", UiKit.number(float(city.get("firm_cash", 0.0))))
	_set_value("gdp_service_cash", UiKit.number(float(city.get("service_cash", 0.0))))
	_set_value("gdp_budget", UiKit.number(float(city.get("government_budget", 0.0))))

	_apply_production(markets)
	_apply_market(markets)
	_apply_trade(city)

	_charts["population"].set_series([_series("unemployment", UiKit.NEGATIVE)])
	_charts["income"].set_series([_series("living_standard", UiKit.BRASS)])
	_charts["gdp"].set_series([_series("gdp_per_capita", UiKit.STEEL)])
	if _history_hint != null:
		_history_hint.text = "%d dias" % _history.count()


func _apply_production(markets: Array) -> void:
	var rows: Array = _rows["production"]
	var produced := 0.0
	var stock := 0.0
	for index in mini(rows.size(), markets.size()):
		var market: Dictionary = markets[index]
		_set_cell(rows[index], 1, "%.1f" % float(market.get("supply", 0.0)))
		_set_cell(rows[index], 2, "%.1f" % float(market.get("stock", 0.0)))
		_set_cell(rows[index], 3, UiKit.number(float(market.get("price", 0.0)), 1))
		produced += float(market.get("supply", 0.0))
		stock += float(market.get("stock", 0.0))
	_set_value("production_total", "%.1f t/dia" % produced)
	_set_value("production_stock", "%.1f t" % stock)


func _apply_market(markets: Array) -> void:
	var rows: Array = _rows["market"]
	var series: Array = []
	for index in mini(rows.size(), markets.size()):
		var market: Dictionary = markets[index]
		var shortage := float(market.get("shortage", 0.0))
		_set_cell(rows[index], 1, UiKit.number(float(market.get("price", 0.0)), 1))
		_set_cell(rows[index], 2, "%.1f" % float(market.get("supply", 0.0)))
		_set_cell(rows[index], 3, "%.1f" % float(market.get("demand", 0.0)))
		_set_cell(rows[index], 4, "%.1f" % float(market.get("stock", 0.0)))
		_set_cell(rows[index], 5, "—" if shortage <= 0.001 else "%.1f" % shortage,
				UiKit.NEGATIVE if shortage > 0.001 else UiKit.TEXT_MUTED)
		series.append({
			"values": _history.product_series("prices", index),
			"color": _product_color(index),
			"fill": false,
		})
	_charts["market"].set_series(series)


func _apply_trade(city: Dictionary) -> void:
	var rows: Array = _rows["trade"]
	var exported: Array = city.get("exported", [])
	var imported: Array = city.get("imported", [])
	var export_value: Array = city.get("export_value", [])
	var import_value: Array = city.get("import_value", [])

	for index in rows.size():
		_set_cell(rows[index], 1, "%.1f t" % _at(exported, index))
		_set_cell(rows[index], 2, UiKit.number(_at(export_value, index)))
		_set_cell(rows[index], 3, "%.1f t" % _at(imported, index))
		_set_cell(rows[index], 4, UiKit.number(_at(import_value, index)))

	var total_export := _sum(export_value)
	var total_import := _sum(import_value)
	var balance := total_export - total_import

	_set_value("trade_export", UiKit.number(total_export), UiKit.POSITIVE)
	_set_value("trade_import", UiKit.number(total_import), UiKit.NEGATIVE)
	_set_value("trade_balance", UiKit.number(balance),
			UiKit.POSITIVE if balance >= 0.0 else UiKit.NEGATIVE)
	_charts["trade"].set_series([
		_series("export_value", UiKit.POSITIVE, false),
		_series("import_value", UiKit.NEGATIVE, false),
	])


# --- Auxiliares -------------------------------------------------------------

func _metric(section: UiSection, key: String, caption_text: String) -> void:
	_values[key] = section.add_metric(caption_text, "—")


## Medidor com rótulo próprio; o preenchimento é atualizado sem recriar nós.
func _register_meter(section: UiSection, caption_text: String) -> Control:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 8)

	var caption_node := UiKit.label(caption_text, 11, UiKit.TEXT_MUTED)
	caption_node.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(caption_node)

	var meter := UiKit.meter(0.0, UiKit.TEXT_MUTED, 5)
	meter.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(meter)

	section.add_content(row)
	return meter


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


func _set_value(key: String, text: String, color: Color = UiKit.TEXT) -> void:
	var label := _values.get(key) as Label
	if label == null:
		return
	label.text = text
	label.add_theme_color_override("font_color", color)


func _set_cell(row: HBoxContainer, index: int, text: String,
		color: Color = UiKit.TEXT) -> void:
	if index >= row.get_child_count():
		return
	var cell := row.get_child(index) as Label
	if cell == null:
		return
	cell.text = text
	cell.add_theme_color_override("font_color", color)


func _chart(section: UiSection, height: float, baseline_zero: bool) -> UiChart:
	var chart := UiChart.new(height)
	chart.set_baseline_zero(baseline_zero)
	section.add_content(chart)
	return chart


func _series(field: String, color: Color, fill: bool = true) -> Dictionary:
	return {"values": _history.series(field), "color": color, "fill": fill}


func _legend(names: Array, colors: Array) -> Control:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 12)

	for index in names.size():
		var item := HBoxContainer.new()
		item.add_theme_constant_override("separation", 4)

		var swatch := ColorRect.new()
		swatch.color = colors[index]
		swatch.custom_minimum_size = Vector2(8.0, 8.0)
		swatch.size_flags_vertical = Control.SIZE_SHRINK_CENTER
		swatch.mouse_filter = Control.MOUSE_FILTER_IGNORE
		item.add_child(swatch)
		item.add_child(UiKit.label(String(names[index]), 11, UiKit.TEXT_MUTED))
		row.add_child(item)

	return row


func _product_name(product: int) -> String:
	return PRODUCT_NAMES[product] if product >= 0 and product < PRODUCT_NAMES.size() else "—"


func _product_color(product: int) -> Color:
	return PRODUCT_COLORS[product] if product >= 0 and product < PRODUCT_COLORS.size() \
			else UiKit.TEXT


func _product_colors() -> Array:
	var colors: Array = []
	for product in PRODUCT_NAMES.size():
		colors.append(_product_color(product))
	return colors


static func _unemployment_color(rate: float) -> Color:
	if rate >= 0.15:
		return UiKit.NEGATIVE
	if rate >= 0.08:
		return UiKit.WARNING
	return UiKit.POSITIVE


static func _satisfaction_color(satisfaction: float) -> Color:
	if satisfaction >= 0.95:
		return UiKit.POSITIVE
	if satisfaction >= 0.80:
		return UiKit.WARNING
	return UiKit.NEGATIVE


static func _at(values: Array, index: int) -> float:
	return float(values[index]) if index >= 0 and index < values.size() else 0.0


static func _sum(values: Array) -> float:
	var total := 0.0
	for value in values:
		total += float(value)
	return total
