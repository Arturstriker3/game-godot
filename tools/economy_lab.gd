extends SceneTree

## Laboratório de balanceamento econômico.
##
## Usa o MESMO núcleo (EconomySim, modo demo) e apenas o estado inicial é
## controlado: nada aqui cria uma segunda simulação nem inventa indicadores.
## Cada cenário de data/definitions/economy/balance.json é executado em cinco
## horizontes; o laboratório grava um CSV por execução, um resumo em
## docs/economy_lab/runs.csv e falha quando encontra uma violação dura.
##
## Uso:
##   godot --headless --path . --script res://tools/economy_lab.gd
##   godot --headless --path . --script res://tools/economy_lab.gd -- --only=04_escassez_alimentos --days=365

const CONFIG_PATH := "res://data/definitions/economy/balance.json"
const OUT_ROOT := "res://docs/economy_lab"
const MAX_VIOLATION_MESSAGES := 12

var _config: Dictionary = {}
var _products: Array[String] = []
var _base_prices: Array[float] = []
var _thresholds: Dictionary = {}
var _only := ""
var _days_override := 0
var _tag := ""
var _runs: Array[Dictionary] = []


func _init() -> void:
	_parse_args()
	if not _load_config():
		quit(1)
		return

	for scenario in _config["scenarios"]:
		if not _only.is_empty() and String(scenario["id"]) != _only:
			continue
		for horizon in _horizons():
			_runs.append(_run_scenario(scenario, horizon))

	_write_runs_csv()
	_write_runs_json()
	_print_table()

	var failures := 0
	var alerts := 0
	for run in _runs:
		failures += int(run["violations"])
		alerts += int(run["alerts"])
	if failures == 0:
		print("[lab] OK: %d execuções sem violação de invariantes." % _runs.size())
		if alerts > 0:
			print("[lab] AVISOS ECONÔMICOS: %d (ver runs.csv/json; não são falhas contábeis)." % alerts)
		quit(0)
	else:
		print("[lab] FALHAS: %d violações em %d execuções." % [failures, _runs.size()])
		quit(1)


func _parse_args() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--only="):
			_only = arg.get_slice("=", 1)
		elif arg.begins_with("--days="):
			_days_override = int(arg.get_slice("=", 1))
		elif arg.begins_with("--tag="):
			_tag = arg.get_slice("=", 1)


func _out_root() -> String:
	return OUT_ROOT if _tag.is_empty() else "%s/%s" % [OUT_ROOT, _tag]


func _load_config() -> bool:
	var text := FileAccess.get_file_as_string(CONFIG_PATH)
	if text.is_empty():
		push_error("não foi possível ler " + CONFIG_PATH)
		return false
	var parsed: Variant = JSON.parse_string(text)
	if not parsed is Dictionary:
		push_error("JSON inválido em " + CONFIG_PATH)
		return false
	_config = parsed
	_thresholds = _config.get("thresholds", {})
	for product in _config.get("products", []):
		_products.append(String(product))
	for price in _config.get("base_prices", []):
		_base_prices.append(float(price))
	return true


func _product_index(product: String) -> int:
	return _products.find(product)


func _horizons() -> Array[int]:
	if _days_override > 0:
		return [_days_override]
	var result: Array[int] = []
	for value in _config.get("horizons_days", []):
		result.append(int(value))
	return result


func _run_scenario(scenario: Dictionary, horizon: int) -> Dictionary:
	var scenario_id := String(scenario["id"])
	var sim = EconomySim.new()
	sim.initialize_demo()
	if _config.has("model"):
		sim.demo_set_balance(_config["model"])

	var state: Dictionary = sim.demo_to_dict()
	_apply_scenario(state, scenario)
	sim.demo_from_dict(state)

	var initial: Dictionary = sim.get_demo_status()
	var money_initial := float(initial["money_total"])
	var expected_population := _population_by_city(initial)
	var previous_prices := _price_snapshot(initial)
	var rows: Array[Dictionary] = []
	var violations: Array[String] = []
	var kinds := {}
	var alerts: Array[String] = []
	var alert_kinds := {}
	var alert_days := 0
	var max_unemployment := 0.0
	var min_satisfaction := 1.0
	var max_price_ratio := 0.0
	var max_stock_days := 0.0
	var invest_total := 0.0
	var sample_every := _sample_every(horizon)
	var final_status: Dictionary = initial

	for day in horizon:
		var current := day + 1
		var intervened := false

		if scenario.has("yearly_population_growth") and current > 1 \
				and (current - 1) % 365 == 0:
			var growth := float(scenario["yearly_population_growth"])
			_grow_population(sim, growth, expected_population)
			intervened = true

		if scenario.has("invest"):
			var invest: Dictionary = scenario["invest"]
			var every := int(invest.get("every_days", 30))
			if current > 1 and (current - 1) % every == 0:
				invest_total += _apply_invest(sim, invest)

		for event in scenario.get("build_events", []):
			if int(event.get("day", 0)) == current:
				_apply_build(sim, event)

		final_status = sim.tick_demo()

		var day_violations := _check(final_status, current, money_initial,
				expected_population, previous_prices)
		for message in day_violations:
			var kind := _kind_of(message)
			if not kinds.has(kind) and violations.size() < MAX_VIOLATION_MESSAGES:
				violations.append(message)
			kinds[kind] = int(kinds.get(kind, 0)) + 1

		var day_alerts := _alerts(final_status, current)
		if not day_alerts.is_empty():
			alert_days += 1
		for message in day_alerts:
			var kind := _alert_kind_of(message)
			if not alert_kinds.has(kind) and alerts.size() < MAX_VIOLATION_MESSAGES:
				alerts.append(message)
			alert_kinds[kind] = int(alert_kinds.get(kind, 0)) + 1

		previous_prices = _price_snapshot(final_status)
		max_unemployment = maxf(max_unemployment, _max_unemployment(final_status))
		min_satisfaction = minf(min_satisfaction, _min_satisfaction(final_status))
		max_price_ratio = maxf(max_price_ratio, _max_price_ratio(final_status))
		max_stock_days = maxf(max_stock_days, _max_stock_days(final_status))

		if intervened or (current - 1) % sample_every == 0 or current == horizon:
			for row in _rows_for(final_status):
				row["day"] = current
				rows.append(row)

	var csv_path := _write_city_csv(scenario_id, horizon, rows)
	return _summary(scenario, horizon, final_status, money_initial,
			max_unemployment, min_satisfaction, max_price_ratio, max_stock_days,
			invest_total, violations, kinds, alerts, alert_kinds, alert_days, csv_path)


# --- Cenários ----------------------------------------------------------------

func _apply_scenario(state: Dictionary, scenario: Dictionary) -> void:
	var remove_cities: Array = scenario.get("remove_cities", [])
	if not remove_cities.is_empty():
		var kept: Array = []
		for city in state["cities"]:
			if not remove_cities.has(String(city["id"])):
				kept.append(city)
		state["cities"] = kept

	var prefixes: Array = scenario.get("remove_buildings", [])
	if not prefixes.is_empty():
		var kept_buildings: Array = []
		for building in state["buildings"]:
			var matched := false
			for prefix in prefixes:
				if String(building["id"]).begins_with(String(prefix)):
					matched = true
					break
			if not matched:
				kept_buildings.append(building)
		state["buildings"] = kept_buildings

	var overrides: Dictionary = scenario.get("cities", {})
	for city in state["cities"]:
		var city_id := String(city["id"])
		if not overrides.has(city_id):
			continue
		_apply_city_override(city, overrides[city_id])


func _apply_city_override(city: Dictionary, spec: Dictionary) -> void:
	var pop: Dictionary = city["pop"]
	if spec.has("population"):
		var population := float(spec["population"])
		city["population"] = population
		pop["population"] = population
	if spec.has("service_share"):
		city["service_jobs"] = float(city["population"]) * float(spec["service_share"])
	if spec.has("firm_cash"):
		city["firm_cash"] = float(spec["firm_cash"])
	if spec.has("service_cash"):
		city["service_cash"] = float(spec["service_cash"])
	if spec.has("government_budget"):
		city["government_budget"] = float(spec["government_budget"])
	if spec.has("savings"):
		pop["savings"] = float(spec["savings"])
	if spec.has("market_stock"):
		_apply_market_values(city, "stock", spec["market_stock"])
	if spec.has("market_price"):
		_apply_market_values(city, "price", spec["market_price"])
	if spec.has("suitability"):
		var suitability: Array = city["suitability"]
		for product in spec["suitability"]:
			var index := _product_index(String(product))
			if index >= 0:
				suitability[index] = float(spec["suitability"][product])


func _apply_market_values(city: Dictionary, field: String, values: Array) -> void:
	var markets: Array = city["markets"]
	for index in mini(values.size(), markets.size()):
		var market: Dictionary = markets[index]
		market[field] = float(values[index])


func _grow_population(sim, growth: float, expected_population: Dictionary) -> void:
	var state: Dictionary = sim.demo_to_dict()
	var factor := 1.0 + growth
	for city in state["cities"]:
		var population := float(city["population"]) * factor
		city["population"] = population
		var pop: Dictionary = city["pop"]
		pop["population"] = population
		expected_population[String(city["id"])] = population
	sim.demo_from_dict(state)


func _apply_invest(sim, invest: Dictionary) -> float:
	var city_id := String(invest.get("city", ""))
	var building_id := String(invest.get("building", ""))
	var status: Dictionary = sim.get_demo_status()
	for city in status.get("cities", []):
		if String(city["id"]) != city_id:
			continue
		var amount := float(city["firm_cash"]) * float(invest.get("share", 0.0))
		if amount <= 0.0:
			return 0.0
		var result: Dictionary = sim.demo_invest(city_id, building_id, amount)
		if String(result.get("error", "")).is_empty():
			return float(result.get("invested", 0.0))
		return 0.0
	return 0.0


func _apply_build(sim, event: Dictionary) -> void:
	var result: Dictionary = sim.demo_build(
			String(event.get("city", "")), String(event.get("id", "")),
			String(event.get("resource", "")), int(event.get("input_product", -1)),
			int(event.get("output_product", 0)), float(event.get("capacity", 0.0)),
			float(event.get("jobs", 0.0)), float(event.get("wage", 0.0)),
			float(event.get("opex", 0.0)), float(event.get("input_per_output", 0.0)),
			String(event.get("owner", "private")), float(event.get("cost", 0.0)))
	if not String(result.get("error", "")).is_empty():
		push_warning("falha ao construir %s: %s" % [
			event.get("id", "?"), result["error"]])


# --- Violações ---------------------------------------------------------------

func _check(status: Dictionary, day: int, money_initial: float,
		expected_population: Dictionary, previous_prices: Dictionary) -> Array[String]:
	var messages: Array[String] = []
	var money_epsilon := float(_thresholds.get("money_epsilon", 0.5))
	var value_epsilon := float(_thresholds.get("value_epsilon", 0.01))
	var stock_epsilon := float(_thresholds.get("stock_epsilon", 0.001))
	var step_limit := float(_thresholds.get("daily_price_step_alert", 0.0501))

	if absf(float(status["money_total"]) - money_initial) > money_epsilon:
		messages.append("dia %d: dinheiro %.2f != %.2f" % [
			day, status["money_total"], money_initial])

	for city in status.get("cities", []):
		var id := String(city["id"])
		if expected_population.has(id) \
				and absf(float(city["population"]) - float(expected_population[id])) > 0.01:
			messages.append("dia %d: %s população %.2f != %.2f" % [
				day, id, city["population"], expected_population[id]])
		for key in ["firm_cash", "service_cash", "government_budget"]:
			if float(city[key]) < -value_epsilon:
				messages.append("dia %d: %s %s negativo (%.2f)" % [
					day, id, key, city[key]])
		var pop: Dictionary = city["pop"]
		if float(pop["savings"]) < -value_epsilon:
			messages.append("dia %d: %s poupança negativa (%.2f)" % [
				day, id, pop["savings"]])
		if absf(float(pop["employed"]) + float(pop["unemployed"])
				- float(pop["labor_force"])) > value_epsilon:
			messages.append("dia %d: %s emprego inconsistente" % [day, id])
		if float(pop["consumed"]) > float(pop["need"]) + value_epsilon:
			messages.append("dia %d: %s consumo acima da necessidade" % [day, id])
		if float(pop["consumed"]) < -value_epsilon \
				or float(pop["employed"]) < -value_epsilon \
				or float(pop["unemployed"]) < -value_epsilon \
				or float(pop["labor_force"]) < -value_epsilon:
			messages.append("dia %d: %s fluxo de trabalho/consumo negativo" % [day, id])
		if float(pop["satisfaction"]) < -value_epsilon \
				or float(pop["satisfaction"]) > 1.0 + value_epsilon:
			messages.append("dia %d: %s satisfação fora de [0,1] (%.3f)" % [
				day, id, pop["satisfaction"]])
		var produced := 0.0
		var capacity_unscaled := 0.0
		for building in city.get("buildings", []):
			produced += float(building["utilization"]) * float(building["capacity"])
			capacity_unscaled += float(building["capacity"])
			if not is_finite(float(building["profit"])):
				messages.append("dia %d: %s lucro não finito em %s" % [
					day, id, building["id"]])
		# Comparação com a capacidade nominal: output_scale pode cair no mesmo
		# tick (regra de prejuízo), então a capacidade efetiva do fim do dia é
		# menor que a usada para produzir.
		if produced > capacity_unscaled + value_epsilon:
			messages.append("dia %d: %s produção acima da capacidade (%.3f > %.3f)" % [
				day, id, produced, capacity_unscaled])
		if not is_finite(produced):
			messages.append("dia %d: %s produção não finita" % [day, id])
		var expected_gdp := float(city.get("service_revenue", 0.0))
		for building in city.get("buildings", []):
			expected_gdp += float(building["revenue"]) - float(building["input_value"])
		if absf(float(city["gdp"]) - expected_gdp) > value_epsilon:
			messages.append("dia %d: %s PIB != valor adicionado" % [day, id])
		if not is_finite(float(city["gdp"])):
			messages.append("dia %d: %s PIB não finito" % [day, id])

		var markets: Array = city["markets"]
		for index in markets.size():
			var market: Dictionary = markets[index]
			if not is_finite(float(market["stock"])):
				messages.append("dia %d: %s estoque não finito em %s" % [
					day, id, _products[index]])
			if float(market["stock"]) < -stock_epsilon:
				messages.append("dia %d: %s estoque negativo em %s" % [
					day, id, _products[index]])
			var price := float(market["price"])
			var base := _base_prices[index]
			if not is_finite(price) or price <= 0.0:
				messages.append("dia %d: %s preço inválido em %s (%.3f)" % [
					day, id, _products[index], price])
				continue
			if price < base * float(_thresholds.get("price_min_multiplier", 0.5)) \
					- value_epsilon:
				messages.append("dia %d: %s preço abaixo do piso em %s (%.2f)" % [
					day, id, _products[index], price])
			if price > base * float(_thresholds.get("price_max_multiplier", 3.0)) \
					+ value_epsilon:
				messages.append("dia %d: %s preço acima do teto em %s (%.2f)" % [
					day, id, _products[index], price])
			var key := "%s|%d" % [id, index]
			if previous_prices.has(key):
				var delta := absf(price - float(previous_prices[key]))
				if delta > base * step_limit + value_epsilon:
					messages.append("dia %d: %s salto de preço em %s (%.2f)" % [
						day, id, _products[index], delta])

	return messages


func _alerts(status: Dictionary, day: int) -> Array[String]:
	# Avisos econômicos: não são erros contábeis, mas estados implausíveis que
	# o relatório precisa explicar (desemprego alto, fome, estoque parado).
	var messages: Array[String] = []
	var unemployment_alert := float(_thresholds.get("unemployment_alert", 0.25))
	var satisfaction_alert := float(_thresholds.get("satisfaction_alert", 0.8))
	for city in status.get("cities", []):
		var id := String(city["id"])
		var pop: Dictionary = city["pop"]
		var labor := maxf(float(pop["labor_force"]), 1.0)
		var unemployment := float(pop["unemployed"]) / labor
		if unemployment > unemployment_alert + 0.001:
			messages.append("[desemprego] dia %d: %s desemprego %.1f%%" % [
				day, id, unemployment * 100.0])
		if float(pop["satisfaction"]) < satisfaction_alert - 0.001:
			messages.append("[satisfacao] dia %d: %s satisfação alimentar %.2f" % [
				day, id, pop["satisfaction"]])
		var markets: Array = city["markets"]
		for index in markets.size():
			var market: Dictionary = markets[index]
			var sold := maxf(float(market["sold"]), 0.001)
			var days := float(market["stock"]) / sold
			if float(market["stock"]) > 1.0 and days > 365.0:
				messages.append("[estoque] dia %d: %s estoque de %s em %.0f dias de venda" % [
					day, id, _products[index], days])
	if float(status.get("foreign_cash", 0.0)) < -1.0 and day % 30 == 0:
		messages.append("[conta_externa] dia %d: saldo externo %.0f (exportação paga com crédito externo)" % [
			day, status["foreign_cash"]])
	return messages


func _max_stock_days(status: Dictionary) -> float:
	var worst := 0.0
	for city in status.get("cities", []):
		for market in city["markets"]:
			# Piso de 1 t/dia evita que vendas residuais de 0,001 t transformem
			# qualquer estoque em "milhares de dias" e distorçam o indicador.
			var sold := maxf(float(market["sold"]), 1.0)
			worst = maxf(worst, float(market["stock"]) / sold)
	return worst


func _kind_of(message: String) -> String:
	var kind := message.get_slice(":", 1).strip_edges()
	var paren := kind.find("(")
	if paren > 0:
		kind = kind.substr(0, paren).strip_edges()
	return kind.substr(0, 28)


func _alert_kind_of(message: String) -> String:
	var end := message.find("]")
	if end > 0:
		return message.substr(1, end - 1)
	return message.substr(0, 28)


func _sum_counts(counts: Dictionary) -> int:
	var total := 0
	for value in counts.values():
		total += int(value)
	return total


# --- Amostragem --------------------------------------------------------------

func _rows_for(status: Dictionary) -> Array[Dictionary]:
	var rows: Array[Dictionary] = []
	for city in status.get("cities", []):
		var pop: Dictionary = city["pop"]
		var markets: Array = city["markets"]
		var profit := 0.0
		var capacity := 0.0
		var production := 0.0
		var food_production := 0.0
		var iron_production := 0.0
		for building in city.get("buildings", []):
			profit += float(building["profit"])
			capacity += float(building["capacity"]) * float(building["output_scale"])
			var building_output := float(building["utilization"]) * float(building["capacity"])
			production += building_output
			if int(building["output_product"]) == 0:
				food_production += building_output
			if int(building["output_product"]) == 3:
				iron_production += building_output
		rows.append({
			"city": String(city["id"]),
			"population": float(city["population"]),
			"money_total": float(status.get("money_total", 0.0)),
			"suitability_wheat": float(city.get("suitability", [0.0])[0]),
			"suitability_iron": float(city.get("suitability", [0.0, 0.0, 0.0, 0.0])[3]),
			"labor_force": float(pop["labor_force"]),
			"employed": float(pop["employed"]),
			"unemployed": float(pop["unemployed"]),
			"unemployment_rate": float(pop["unemployed"]) / maxf(float(pop["labor_force"]), 1.0),
			"wage_income": float(pop["wage_income"]),
			"wage_avg": float(pop["wage_income"]) / maxf(float(pop["employed"]), 1.0),
			"disposable": float(pop["disposable"]),
			"disposable_pc": float(pop["disposable"]) / maxf(float(city["population"]), 1.0),
			"savings": float(pop["savings"]),
			"gdp": float(city["gdp"]),
			"gdp_pc": float(city["gdp_per_capita"]),
			"living_standard": float(city["living_standard"]),
			"need": float(pop["need"]),
			"consumed": float(pop["consumed"]),
			"food_spend": float(pop["food_spend"]),
			"satisfaction": float(pop["satisfaction"]),
			"firm_cash": float(city["firm_cash"]),
			"service_cash": float(city["service_cash"]),
			"government_budget": float(city["government_budget"]),
			"service_revenue": float(city["service_revenue"]),
			"opex_revenue": float(city.get("opex_revenue", 0.0)),
			"freight_revenue": float(city.get("freight_revenue", 0.0)),
			"construction_revenue": float(city.get("construction_revenue", 0.0)),
			"service_profit": float(city["service_profit"]),
			"profit_total": profit,
			"capacity_total": capacity,
			"production_total": production,
			"food_production": food_production,
			"iron_production": iron_production,
			"stock_wheat": float(markets[0]["stock"]),
			"price_wheat": float(markets[0]["price"]),
			"stock_flour": float(markets[1]["stock"]),
			"price_flour": float(markets[1]["price"]),
			"stock_bread": float(markets[2]["stock"]),
			"price_bread": float(markets[2]["price"]),
			"stock_iron": float(markets[3]["stock"]),
			"price_iron": float(markets[3]["price"]),
			"imported_wheat": float(city["imported"][0]),
			"exported_wheat": float(city["exported"][0]),
			"imported_bread": float(city["imported"][2]),
			"exported_bread": float(city["exported"][2]),
			"exported_iron": float(city["exported"][3]),
		})
	return rows


func _sample_every(horizon: int) -> int:
	var table: Dictionary = _config.get("sample_every_days", {})
	return maxi(1, int(table.get(str(horizon), 1)))


func _population_by_city(status: Dictionary) -> Dictionary:
	var result := {}
	for city in status.get("cities", []):
		result[String(city["id"])] = float(city["population"])
	return result


func _price_snapshot(status: Dictionary) -> Dictionary:
	var result := {}
	for city in status.get("cities", []):
		var markets: Array = city["markets"]
		for index in markets.size():
			result["%s|%d" % [city["id"], index]] = float(markets[index]["price"])
	return result


func _max_unemployment(status: Dictionary) -> float:
	var worst := 0.0
	for city in status.get("cities", []):
		var pop: Dictionary = city["pop"]
		worst = maxf(worst, float(pop["unemployed"])
				/ maxf(float(pop["labor_force"]), 1.0))
	return worst


func _min_satisfaction(status: Dictionary) -> float:
	var worst := 1.0
	for city in status.get("cities", []):
		worst = minf(worst, float(city["pop"]["satisfaction"]))
	return worst


func _max_price_ratio(status: Dictionary) -> float:
	var worst := 0.0
	for city in status.get("cities", []):
		var markets: Array = city["markets"]
		for index in markets.size():
			var ratio := float(markets[index]["price"]) / _base_prices[index]
			worst = maxf(worst, ratio)
	return worst


# --- Saída -------------------------------------------------------------------

func _summary(scenario: Dictionary, horizon: int, final_status: Dictionary,
		money_initial: float, max_unemployment: float, min_satisfaction: float,
		max_price_ratio: float, max_stock_days: float, invest_total: float,
		violations: Array[String], kinds: Dictionary, alerts: Array[String],
		alert_kinds: Dictionary, alert_days: int, csv_path: String) -> Dictionary:
	var cities: Array = final_status.get("cities", [])
	var population := 0.0
	var gdp := 0.0
	var employed := 0.0
	var unemployed := 0.0
	var labor := 0.0
	var savings := 0.0
	var firm_cash := 0.0
	var service_cash := 0.0
	var government := 0.0
	var profit := 0.0
	var production := 0.0
	var capacity := 0.0
	var living_min := 1.0e30
	var consumed := 0.0
	var need := 0.0
	var trade_balance := 0.0
	var construction := 0.0
	for city in cities:
		population += float(city["population"])
		gdp += float(city["gdp"])
		employed += float(city["pop"]["employed"])
		unemployed += float(city["pop"]["unemployed"])
		labor += float(city["pop"]["labor_force"])
		savings += float(city["pop"]["savings"])
		firm_cash += float(city["firm_cash"])
		service_cash += float(city["service_cash"])
		government += float(city["government_budget"])
		living_min = minf(living_min, float(city["living_standard"]))
		consumed += float(city["pop"]["consumed"])
		need += float(city["pop"]["need"])
		construction += float(city.get("construction_revenue", 0.0))
		for building in city.get("buildings", []):
			profit += float(building["profit"])
			capacity += float(building["capacity"]) * float(building["output_scale"])
			production += float(building["utilization"]) * float(building["capacity"])
		for index in city.get("import_value", []).size():
			trade_balance += float(city["export_value"][index]) - float(city["import_value"][index])

	return {
		"scenario": String(scenario["id"]),
		"label": String(scenario.get("label", "")),
		"days": horizon,
		"cities": cities.size(),
		"population_end": population,
		"gdp_end": gdp,
		"gdp_pc_end": gdp / maxf(population, 1.0),
		"gdp_pc_final": gdp / maxf(population, 1.0),
		"construction_end": construction,
		"gdp_ex_construction_end": gdp - construction,
		"gdp_pc_ex_construction_end": (gdp - construction) / maxf(population, 1.0),
		"unemployment_end": unemployed / maxf(labor, 1.0),
		"unemployment_max": max_unemployment,
		"satisfaction_min": min_satisfaction,
		"satisfaction_end": _min_satisfaction(final_status),
		"price_ratio_max": max_price_ratio,
		"price_ratio_end": _max_price_ratio(final_status),
		"stock_days_max": max_stock_days,
		"stock_days_end": _max_stock_days(final_status),
		"living_standard_min": living_min if living_min < 1.0e29 else 0.0,
		"consumed_end": consumed,
		"need_end": need,
		"production_end": production,
		"capacity_end": capacity,
		"profit_end": profit,
		"savings_end": savings,
		"firm_cash_end": firm_cash,
		"service_cash_end": service_cash,
		"government_end": government,
		"trade_balance_end": trade_balance,
		"foreign_cash_end": float(final_status.get("foreign_cash", 0.0)),
		"transport_cash_end": float(final_status.get("transport_cash", 0.0)),
		"construction_cash_end": float(final_status.get("construction_cash", 0.0)),
		"money_initial": money_initial,
		"money_end": float(final_status["money_total"]),
		"money_drift": float(final_status["money_total"]) - money_initial,
		"invest_total": invest_total,
		"violations": violations.size(),
		"violation_kinds": kinds,
		"violation_samples": violations,
		"alerts": alert_days,
		"alert_days": alert_days,
		"alert_events": _sum_counts(alert_kinds),
		"alert_share": float(alert_days) / maxf(float(horizon), 1.0),
		"alert_kinds": alert_kinds,
		"alert_samples": alerts,
		"csv": csv_path,
	}


func _write_city_csv(scenario_id: String, horizon: int, rows: Array[Dictionary]) -> String:
	var directory := "%s/%s" % [_out_root(), scenario_id]
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(directory))
	var path := "%s/%dd.csv" % [directory, horizon]
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		push_error("não foi possível gravar " + path)
		return ""

	var columns := [
		"day", "city", "population", "money_total", "suitability_wheat",
		"suitability_iron", "labor_force", "employed", "unemployed",
		"unemployment_rate", "wage_income", "wage_avg", "disposable",
		"disposable_pc", "savings", "gdp", "gdp_pc", "living_standard", "need",
		"consumed", "food_spend", "satisfaction", "firm_cash", "service_cash",
		"government_budget", "service_revenue", "service_profit", "profit_total",
		"capacity_total", "production_total", "food_production", "iron_production", "stock_wheat", "price_wheat",
		"stock_flour",
		"price_flour", "stock_bread", "price_bread", "stock_iron", "price_iron",
		"imported_wheat", "exported_wheat", "imported_bread", "exported_bread",
		"exported_iron", "opex_revenue", "freight_revenue", "construction_revenue",
	]
	file.store_line(",".join(columns))
	for row in rows:
		var values := PackedStringArray()
		for column in columns:
			values.append(_csv_value(row.get(column, 0.0)))
		file.store_line(",".join(values))
	file.close()
	return path


func _csv_value(value: Variant) -> String:
	if value is String:
		return value
	return "%.6f" % float(value)


func _write_runs_csv() -> void:
	var root := _out_root()
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(root))
	var path := "%s/runs.csv" % root
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		push_error("não foi possível gravar " + path)
		return

	file.store_line("scenario,label,days,cities,population_end,gdp_end,gdp_pc_end,"
			+ "gdp_pc_ex_construction_end,"
			+ "unemployment_end,unemployment_max,satisfaction_min,satisfaction_end,"
			+ "price_ratio_max,price_ratio_end,stock_days_max,living_standard_min,"
			+ "production_end,consumed_end,need_end,profit_end,savings_end,"
			+ "firm_cash_end,service_cash_end,government_end,trade_balance_end,"
			+ "foreign_cash_end,money_initial,money_end,money_drift,invest_total,"
			+ "violations,alerts,csv")
	for run in _runs:
		file.store_line(",".join([
			String(run["scenario"]),
			'"%s"' % String(run["label"]).replace('"', "'"),
			str(run["days"]),
			str(run["cities"]),
			"%.3f" % float(run["population_end"]),
			"%.3f" % float(run["gdp_end"]),
			"%.8f" % float(run["gdp_pc_end"]),
			"%.8f" % float(run["gdp_pc_ex_construction_end"]),
			"%.4f" % float(run["unemployment_end"]),
			"%.4f" % float(run["unemployment_max"]),
			"%.4f" % float(run["satisfaction_min"]),
			"%.4f" % float(run["satisfaction_end"]),
			"%.4f" % float(run["price_ratio_max"]),
			"%.4f" % float(run["price_ratio_end"]),
			"%.2f" % float(run["stock_days_max"]),
			"%.4f" % float(run["living_standard_min"]),
			"%.4f" % float(run["production_end"]),
			"%.4f" % float(run["consumed_end"]),
			"%.4f" % float(run["need_end"]),
			"%.4f" % float(run["profit_end"]),
			"%.4f" % float(run["savings_end"]),
			"%.4f" % float(run["firm_cash_end"]),
			"%.4f" % float(run["service_cash_end"]),
			"%.4f" % float(run["government_end"]),
			"%.4f" % float(run["trade_balance_end"]),
			"%.4f" % float(run["foreign_cash_end"]),
			"%.2f" % float(run["money_initial"]),
			"%.2f" % float(run["money_end"]),
			"%.4f" % float(run["money_drift"]),
			"%.2f" % float(run["invest_total"]),
			str(run["violations"]),
			str(run["alerts"]),
			String(run["csv"]),
		]))
	file.close()


func _write_runs_json() -> void:
	var root := _out_root()
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(root))
	var path := "%s/runs.json" % root
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		push_error("não foi possível gravar " + path)
		return
	file.store_string(JSON.stringify({
		"tag": _tag,
		"generated_by": "tools/economy_lab.gd",
		"thresholds": _thresholds,
		"runs": _runs,
	}, "\t"))
	file.close()


func _print_table() -> void:
	print("[lab] cenário                     dias  desemp.  satisf.  preço×  PIB pc      viol  avisos")
	for run in _runs:
		print("[lab] %-28s %5d  %6.1f%%  %6.2f  %6.2f  %9.4f  %4d  %6d" % [
			run["scenario"], run["days"],
			float(run["unemployment_end"]) * 100.0,
			float(run["satisfaction_min"]),
			float(run["price_ratio_max"]),
			float(run["gdp_pc_end"]),
			int(run["violations"]),
			int(run["alerts"])])
	for run in _runs:
		if int(run["violations"]) == 0:
			continue
		print("[lab] violações em %s (%d dias):" % [run["scenario"], run["days"]])
		for message in run["violation_samples"]:
			print("   - " + String(message))
