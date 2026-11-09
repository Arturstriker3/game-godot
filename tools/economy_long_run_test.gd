extends SceneTree

## Simulação longa (365 dias) do cenário de duas cidades. Registra preços,
## estoques, produção, consumo, emprego, salários, lucros, PIB e padrão de vida
## em CSV, e falha quando encontra instabilidade contábil ou de mercado.
##
## Uso: godot --headless --script res://tools/economy_long_run_test.gd -- --tag=before

const GeographyServiceScript := preload("res://core/economy/geography_service.gd")

const DAYS := 365
const MONEY_EPSILON := 0.5
const VALUE_EPSILON := 0.01
const STOCK_EPSILON := 0.001

const AGRIA_CENTER := Vector2(2532.6, 571.4)
const PORTUS_CENTER := Vector2(2560.4, 543.8)

var _failures: Array[String] = []
var _rows: Array[Dictionary] = []
var _tag := "run"
var _money_initial := 0.0
var _population_initial := 0.0


func _init() -> void:
	_tag = _read_tag()
	var sim = EconomySim.new()
	sim.initialize_demo()
	GeographyServiceScript.new().apply_to_sim(sim, [
		{"id": "AGRIA", "center": AGRIA_CENTER},
		{"id": "PORTUS", "center": PORTUS_CENTER},
	])

	var initial: Dictionary = sim.get_demo_status()
	_money_initial = float(initial["money_total"])
	_population_initial = _population(initial)

	for day in DAYS:
		var status: Dictionary = sim.tick_demo()
		_check_invariants(status, day + 1)
		_record(status, day + 1)
		if (day + 1) % 30 == 0:
			_print_month(status)

	_write_csv()
	_print_summary()
	if _failures.is_empty():
		print("[long_run:%s] OK: 365 dias sem violação de conservação." % _tag)
		quit(0)
	else:
		print("[long_run:%s] PROBLEMAS (%d):" % [_tag, _failures.size()])
		for failure in _failures:
			print(" - " + failure)
		quit(1)


func _check_invariants(status: Dictionary, day: int) -> void:
	if absf(float(status["money_total"]) - _money_initial) > MONEY_EPSILON:
		_fail("dia %d: dinheiro %.4f != %.4f" % [
			day, status["money_total"], _money_initial])
	if absf(_population(status) - _population_initial) > 0.01:
		_fail("dia %d: população alterada" % day)

	for city in status.get("cities", []):
		var id := String(city["id"])
		for cash_key in ["firm_cash", "service_cash", "government_budget"]:
			if float(city[cash_key]) < -VALUE_EPSILON:
				_fail("dia %d: %s com %s negativo (%.2f)" % [
					day, id, cash_key, city[cash_key]])
		var pop: Dictionary = city["pop"]
		if float(pop["savings"]) < -VALUE_EPSILON:
			_fail("dia %d: %s com poupança negativa (%.2f)" % [
				day, id, pop["savings"]])
		if absf(float(pop["employed"]) + float(pop["unemployed"])
				- float(pop["labor_force"])) > 0.01:
			_fail("dia %d: %s emprego inconsistente" % [day, id])
		var expected_gdp := float(city.get("service_revenue", 0.0))
		for building in city.get("buildings", []):
			expected_gdp += float(building["revenue"]) - float(building["input_value"])
		if absf(float(city["gdp"]) - expected_gdp) > VALUE_EPSILON:
			_fail("dia %d: %s PIB != valor adicionado" % [day, id])
		for product in city.get("markets", []):
			if float(product["stock"]) < -STOCK_EPSILON:
				_fail("dia %d: %s estoque negativo" % [day, id])


func _record(status: Dictionary, day: int) -> void:
	for city in status.get("cities", []):
		var pop: Dictionary = city["pop"]
		var markets: Array = city["markets"]
		var profit := 0.0
		var produced := {"wheat": 0.0, "flour": 0.0, "bread": 0.0}
		for building in city.get("buildings", []):
			profit += float(building["profit"])
			produced[String(building["resource"])] += float(building["revenue"])
		var wage_bill := float(pop["wage_income"])
		var employed := float(pop["employed"])
		var disposable_pc := float(pop["disposable"]) / maxf(float(city["population"]), 1.0)
		_rows.append({
			"day": day,
			"city": String(city["id"]),
			"population": float(city["population"]),
			"gdp": float(city["gdp"]),
			"gdp_pc": float(city["gdp_per_capita"]),
			"disposable_pc": disposable_pc,
			"wage_avg": wage_bill / maxf(employed, 1.0),
			"employed": employed,
			"unemployed": float(pop["unemployed"]),
			"unemployment_rate": float(pop["unemployed"])
				/ maxf(float(pop["labor_force"]), 1.0),
			"satisfaction": float(pop["satisfaction"]),
			"savings": float(pop["savings"]),
			"firm_cash": float(city["firm_cash"]),
			"service_cash": float(city["service_cash"]),
			"government_budget": float(city["government_budget"]),
			"service_revenue": float(city.get("service_revenue", 0.0)),
			"service_profit": float(city.get("service_profit", 0.0)),
			"profit": profit,
			"price_wheat": float(markets[0]["price"]),
			"price_flour": float(markets[1]["price"]),
			"price_bread": float(markets[2]["price"]),
			"stock_wheat": float(markets[0]["stock"]),
			"stock_flour": float(markets[1]["stock"]),
			"stock_bread": float(markets[2]["stock"]),
			"produced": produced,
			"consumed_wheat": float(pop["consumed"]),
			"food_spend": float(pop["food_spend"]),
			"money_total": float(status["money_total"]),
		})


func _print_month(status: Dictionary) -> void:
	for city in status.get("cities", []):
		var pop: Dictionary = city["pop"]
		var markets: Array = city["markets"]
		var profit := 0.0
		for building in city.get("buildings", []):
			profit += float(building["profit"])
		print(("dia %3d %-6s preços=%.1f/%.1f/%.1f estoques=%.0f/%.0f/%.0f "
				+ "PIBpc=%.5f renda_pc=%.5f salário=%.2f desemp=%.1f%% sat=%.2f "
				+ "lucro=%.0f caixa=%.0f serv=%.0f gov=%.0f poup=%.0f") % [
			status["calendar_day"], city["id"],
			markets[0]["price"], markets[1]["price"], markets[2]["price"],
			markets[0]["stock"], markets[1]["stock"], markets[2]["stock"],
			city["gdp_per_capita"],
			float(pop["disposable"]) / maxf(float(city["population"]), 1.0),
			float(pop["wage_income"]) / maxf(float(pop["employed"]), 1.0),
			100.0 * float(pop["unemployed"]) / maxf(float(pop["labor_force"]), 1.0),
			pop["satisfaction"], profit, city["firm_cash"],
			city["service_cash"], city["government_budget"], pop["savings"]])


func _print_summary() -> void:
	var columns := ["gdp_pc", "disposable_pc", "wage_avg", "unemployment_rate",
		"satisfaction", "profit", "firm_cash", "service_cash", "savings",
		"price_wheat", "price_flour", "price_bread"]
	for city_id in ["AGRIA", "PORTUS"]:
		var first := _first_row(city_id)
		var last := _last_row(city_id)
		print("[long_run:%s] %s primeiro/último dia:" % [_tag, city_id])
		for column in columns:
			print("  %-18s %12.4f -> %12.4f" % [column, first[column], last[column]])


func _first_row(city_id: String) -> Dictionary:
	for row in _rows:
		if row["city"] == city_id:
			return row
	return {}


func _last_row(city_id: String) -> Dictionary:
	for index in range(_rows.size() - 1, -1, -1):
		if _rows[index]["city"] == city_id:
			return _rows[index]
	return {}


func _write_csv() -> void:
	var directory := "res://docs/economy_long_run"
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(directory))
	var path := "%s/%s.csv" % [directory, _tag]
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		_fail("não foi possível escrever " + path)
		return
	file.store_line("day,city,population,gdp,gdp_pc,disposable_pc,wage_avg,employed,"
		+ "unemployed,unemployment_rate,satisfaction,savings,firm_cash,service_cash,"
		+ "government_budget,service_revenue,service_profit,profit,price_wheat,"
		+ "price_flour,price_bread,stock_wheat,stock_flour,stock_bread,food_spend,money_total")
	var line_format := ("%d,%s,%.4f,%.6f,%.8f,%.8f,%.6f,%.4f,%.4f,%.6f,%.6f,%.4f,"
		+ "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f")
	for row in _rows:
		var values: Array = [
			int(row["day"]), String(row["city"]), row["population"], row["gdp"],
			row["gdp_pc"], row["disposable_pc"], row["wage_avg"], row["employed"],
			row["unemployed"], row["unemployment_rate"], row["satisfaction"],
			row["savings"], row["firm_cash"], row["service_cash"],
			row["government_budget"], row["service_revenue"], row["service_profit"],
			row["profit"], row["price_wheat"], row["price_flour"],
			row["price_bread"], row["stock_wheat"], row["stock_flour"],
			row["stock_bread"], row["food_spend"], row["money_total"],
		]
		file.store_line(line_format % values)
	file.close()
	print("[long_run:%s] CSV: %s" % [_tag, path])


func _read_tag() -> String:
	var arguments := OS.get_cmdline_args()
	arguments.append_array(OS.get_cmdline_user_args())
	for argument in arguments:
		if argument.begins_with("--tag=") or argument.begins_with("tag="):
			return argument.get_slice("=", 1)
	return "run"


func _population(status: Dictionary) -> float:
	var total := 0.0
	for city in status.get("cities", []):
		total += float(city["population"])
	return total


func _fail(message: String) -> void:
	_failures.append(message)
