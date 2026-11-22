extends SceneTree

## Guardo da A4 (status sob demanda): cada consulta do binding tem de devolver
## EXATAMENTE o campo correspondente do `get_status()` no mesmo dia.
##
## O jogo não monta mais o Dictionary do mundo inteiro no tick: ele publica o
## resumo leve (`get_day_summary`) e lê colunas/fichas/pedidos sob demanda. Este
## teste roda o mundo real (IA + §19 + geografia) por alguns dias e compara:
##   - `get_city_rows()`/`get_city_info(id)` == `status["cities"]`
##   - `get_city_columns()` == cada escalar de `status["cities"][i]`
##   - `get_nation_rows()`/`get_nation_status(tag)` == `status["nations"]`
##   - `get_trade_orders()`, `get_ports()`, `get_currencies()` (linhas),
##     `get_tariffs()`, `get_products()`, `get_light_columns()`
##   - `get_day_summary()` (campos + totais) e os rankings top-N
## e mede a ficha de UMA cidade (alvo: < 1 ms).
##
##   godot --headless --path . --script res://tools/status_query_test.gd \
##     -- --days=8 --ai=1 --urban=1 --threads=0
##
## Saída: 0 ok, 1 falha.

const GeographyServiceScript := preload("res://core/economy/geography_service.gd")
const GroupNamerScript := preload("res://core/economy/group_namer.gd")

const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const PORTS_PATH := "res://data/definitions/scenario/ports.json"
const BUILDINGS_PATH := "res://data/definitions/scenario/buildings.json"
const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"
const SERVICES_PATH := "res://data/definitions/economy/services.json"
const CURRENCIES_PATH := "res://data/definitions/economy/currencies.json"
const SECTOR_EMPLOYMENT_PATH := "res://data/definitions/economy/sector_employment.json"
const URBAN_LAND_PATH := "res://data/definitions/economy/urban_land.json"
const HOUSING_CITY_PATH := "res://data/definitions/economy/housing_city.json"
const HOUSING_PATH := "res://data/definitions/economy/housing.json"
const MUNICIPAL_FINANCE_PATH := "res://data/definitions/economy/municipal_finance.json"

## Campos das colunas por cidade (os mesmos nomes do bloco leve do status).
const SCALAR_FIELDS: Array[String] = [
	"population", "simulation_population", "gdp", "gdp_per_capita",
	"external_income_index", "firm_cash", "service_cash",
	"government_budget", "government_goods_spend", "service_revenue",
	"service_jobs", "service_workers", "service_jobs_filled",
	"in_kind_food", "service_profit", "opex_revenue", "freight_revenue",
	"construction_revenue", "service_wage", "disposable_per_capita",
	"living_standard", "production", "jobs", "income",
	"industrialization", "stockpile", "need", "deficit", "consumed",
	"in_transit", "fx_rate", "tariff_revenue", "productivity_index",
]

var _days := 8
var _ai := true
var _urban := true
var _threads := 0
var _failures: Array[String] = []
var _checks := 0


func _init() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--days="):
			_days = maxi(1, int(arg.split("=", true, 1)[1]))
		elif arg.begins_with("--ai="):
			_ai = arg.split("=", true, 1)[1] != "0"
		elif arg.begins_with("--urban="):
			_urban = arg.split("=", true, 1)[1] != "0"
		elif arg.begins_with("--threads="):
			_threads = maxi(0, int(arg.split("=", true, 1)[1]))

	_run()
	if _failures.is_empty():
		print("[status_query] OK: %d comparações, %d dias, ai=%s urban=%s" % [
			_checks, _days, str(_ai), str(_urban)])
		quit(0)
	else:
		for failure in _failures:
			print("[status_query] FALHA: " + failure)
		quit(1)


func _fail(message: String) -> void:
	_failures.append(message)


func _load_json(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("não abriu " + path)
		return null
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data


func _load_array(path: String) -> Array:
	var data: Variant = _load_json(path)
	return data as Array if typeof(data) == TYPE_ARRAY else []


func _load_dict(path: String) -> Dictionary:
	var data: Variant = _load_json(path)
	return data as Dictionary if typeof(data) == TYPE_DICTIONARY else {}


func _cities_with_center(cities: Array) -> Array:
	var result: Array = []
	for city in cities:
		result.append({
			"id": String(city.get("id", "")),
			"nation": String(city.get("nation", "")),
			"center": Vector2(float(city.get("x", 0.0)), float(city.get("y", 0.0))),
		})
	return result


## Igualdade forte: JSON ordenado e com precisão total (mesma regra dos goldens).
func _same(label: String, left: Variant, right: Variant) -> void:
	_checks += 1
	var a := JSON.stringify(left, "", true, true)
	var b := JSON.stringify(right, "", true, true)
	if a != b:
		var at := 0
		var limit := mini(mini(a.length(), b.length()), 160)
		while at < limit and a[at] == b[at]:
			at += 1
		_fail("%s difere na posição %d: consulta=%s ... status=%s ..." % [
			label, at, a.substr(maxi(0, at - 40), 90), b.substr(maxi(0, at - 40), 90)])


func _run() -> void:
	var sim = EconomySim.new()
	sim.set_goods(_load_array(GOODS_PATH))
	sim.set_recipes(_load_array(RECIPES_PATH))
	sim.set_needs(_load_dict(NEEDS_PATH))
	sim.set_services(_load_dict(SERVICES_PATH))
	sim.set_currencies(_load_dict(CURRENCIES_PATH))
	sim.set_sector_employment(_load_dict(SECTOR_EMPLOYMENT_PATH))
	sim.set_sim_threads(_threads)

	var cities := _load_array(CITIES_PATH)
	var ports := _load_array(PORTS_PATH)
	var buildings := _load_array(BUILDINGS_PATH)
	if cities.is_empty() or buildings.is_empty():
		_fail("cenário vazio")
		return

	var geo: Object = null
	if _ai:
		geo = GeographyServiceScript.new()
		geo.load_data()
		buildings = geo.resolve_initial_farms(buildings, cities)
	sim.initialize_world(cities, ports, buildings)
	if _urban:
		sim.set_urban(_load_dict(URBAN_LAND_PATH), _load_dict(HOUSING_CITY_PATH),
			_load_dict(HOUSING_PATH), _load_dict(MUNICIPAL_FINANCE_PATH))
	if geo != null:
		geo.apply_to_sim(sim, _cities_with_center(cities))
		sim.set_ai_enabled(true)
		GroupNamerScript.name_groups(sim)

	for _day in _days:
		sim.tick_day()

	var status: Dictionary = sim.get_status()
	if status.is_empty() or int(status.get("day", -1)) != _days:
		_fail("status vazio ou dia errado: %s" % str(status.get("day", null)))
		return

	_check_city_rows(sim, status)
	_check_city_columns(sim, status)
	_check_nations(sim, status)
	_check_world_blocks(sim, status)
	_check_day_summary(sim, status)
	_check_rankings(sim, status)
	_check_city_card_latency(sim, status)


func _check_city_rows(sim, status: Dictionary) -> void:
	var rows: Array = sim.get_city_rows()
	var cities: Array = status.get("cities", [])
	_same("get_city_rows() == status.cities", rows, cities)
	if rows.size() != cities.size():
		return
	for index in rows.size():
		_checks += 1
		var row: Dictionary = rows[index]
		var city: Dictionary = cities[index]
		if String(row.get("id", "")) != String(city.get("id", "")):
			_fail("ordem das linhas difere em %d: %s vs %s" % [
				index, row.get("id", ""), city.get("id", "")])
			return
	# Ficha de uma cidade == a linha do status (amostra de 40 + as duas pontas).
	var samples: Array[int] = [0, cities.size() - 1]
	for index in range(1, cities.size(), maxi(1, cities.size() / 40)):
		samples.append(index)
	for index in samples:
		var city: Dictionary = cities[index]
		var info: Dictionary = sim.get_city_info(String(city.get("id", "")))
		_same("get_city_info(%s)" % city.get("id", ""), info, city)
	if not sim.get_city_info("NAO_EXISTE").is_empty():
		_fail("get_city_info(cidade inexistente) deveria devolver {}")


func _check_city_columns(sim, status: Dictionary) -> void:
	var columns: Dictionary = sim.get_city_columns()
	var cities: Array = status.get("cities", [])
	if int(columns.get("count", -1)) != cities.size():
		_fail("get_city_columns().count = %s, esperado %d" % [
			columns.get("count", null), cities.size()])
		return
	var ids: PackedStringArray = columns.get("id", PackedStringArray())
	var names: PackedStringArray = columns.get("name", PackedStringArray())
	var nations: PackedStringArray = columns.get("nation", PackedStringArray())
	for index in cities.size():
		var city: Dictionary = cities[index]
		if String(city.get("id", "")) != ids[index]:
			_fail("coluna id difere em %d" % index)
			return
		if String(city.get("name", "")) != names[index]:
			_fail("coluna name difere em %d" % index)
			return
		if String(city.get("nation", "")) != nations[index]:
			_fail("coluna nation difere em %d" % index)
			return
	for field in SCALAR_FIELDS:
		var values: PackedFloat64Array = columns.get(field, PackedFloat64Array())
		if values.size() != cities.size():
			_fail("coluna '%s' com %d valores para %d cidades" % [
				field, values.size(), cities.size()])
			continue
		for index in cities.size():
			var expected := float((cities[index] as Dictionary).get(field, 0.0))
			if values[index] != expected:
				_fail("coluna '%s'[%d] (%s) = %.17g, status = %.17g" % [
					field, index, (cities[index] as Dictionary).get("id", ""),
					values[index], expected])
				break
		_checks += 1


func _check_nations(sim, status: Dictionary) -> void:
	var rows: Array = sim.get_nation_rows()
	var nations: Array = status.get("nations", [])
	_same("get_nation_rows() == status.nations", rows, nations)
	var samples: Array[int] = []
	for index in range(0, nations.size(), maxi(1, nations.size() / 12)):
		samples.append(index)
	if not nations.is_empty():
		samples.append(nations.size() - 1)
	for index in samples:
		var nation: Dictionary = nations[index]
		var card: Dictionary = sim.get_nation_status(String(nation.get("tag", "")))
		_same("get_nation_status(%s)" % nation.get("tag", ""), card, nation)
	var columns: Dictionary = sim.get_nation_columns()
	if int(columns.get("count", -1)) != nations.size():
		_fail("get_nation_columns().count = %s, esperado %d" % [
			columns.get("count", null), nations.size()])
	for index in nations.size():
		var nation: Dictionary = nations[index]
		var tag: PackedStringArray = columns.get("tag", PackedStringArray())
		if String(nation.get("tag", "")) != tag[index]:
			_fail("coluna tag de nação difere em %d" % index)
			break
	var population: PackedFloat64Array = columns.get("population",
			PackedFloat64Array())
	for field in ["population", "production", "stockpile", "need", "deficit"]:
		var values: PackedFloat64Array = columns.get(field, PackedFloat64Array())
		for index in nations.size():
			var expected := float((nations[index] as Dictionary).get(field, 0.0))
			if values[index] != expected:
				_fail("coluna nação '%s'[%d] = %.17g, status = %.17g" % [
					field, index, values[index], expected])
				break
		_checks += 1
	if population.size() != nations.size():
		_fail("coluna population de nações com tamanho errado")


func _check_world_blocks(sim, status: Dictionary) -> void:
	_same("get_trade_orders()", sim.get_trade_orders(),
		status.get("trade_orders", []))
	_same("get_ports()", sim.get_ports(), status.get("ports", []))
	_same("get_currency_rows()", sim.get_currency_rows(),
		status.get("currencies", []))
	_same("get_tariffs()", sim.get_tariffs(), status.get("tariffs", []))
	_same("get_products()", sim.get_products(), status.get("products", []))
	_same("get_light_columns()", sim.get_light_columns(),
		status.get("light", {}))


func _check_day_summary(sim, status: Dictionary) -> void:
	var summary: Dictionary = sim.get_day_summary()
	for field in ["day", "calendar_day", "money_total", "transport_cash",
			"foreign_cash", "construction_cash", "bankruptcies",
			"depleted_mines", "service_businesses", "service_bankruptcies"]:
		_checks += 1
		if str(summary.get(field)) != str(status.get(field)):
			_fail("resumo '%s' = %s, status = %s" % [
				field, summary.get(field), status.get(field)])
	_same("resumo.invariant_violations", summary.get("invariant_violations", []),
		status.get("invariant_violations", []))
	_same("resumo.products", summary.get("products", []),
		status.get("products", []))
	# Totais do mundo == soma das linhas, na ordem das cidades.
	var cities: Array = status.get("cities", [])
	var totals := {"population": 0.0, "production": 0.0, "stockpile": 0.0,
		"need": 0.0, "consumed": 0.0, "deficit": 0.0, "in_transit": 0.0}
	for city in cities:
		for field in totals:
			totals[field] = float(totals[field]) + float(city.get(field, 0.0))
	for field in totals:
		_checks += 1
		if float(summary.get(field, 0.0)) != float(totals[field]):
			_fail("total '%s' = %.17g, soma das cidades = %.17g" % [
				field, float(summary.get(field, 0.0)), float(totals[field])])


func _check_rankings(sim, status: Dictionary) -> void:
	var cities: Array = status.get("cities", [])
	for field in ["population", "production", "gdp"]:
		var ranking: Array = sim.get_city_ranking(field, 10)
		var expected: Array = []
		for city in cities:
			expected.append(city)
		expected.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
			var left := float(a.get(field, 0.0))
			var right := float(b.get(field, 0.0))
			if left != right:
				return left > right
			return String(a.get("id", "")) < String(b.get("id", "")))
		if ranking.size() != mini(10, expected.size()):
			_fail("ranking '%s' com %d linhas" % [field, ranking.size()])
			continue
		for index in ranking.size():
			var row: Dictionary = ranking[index]
			var city: Dictionary = expected[index]
			_checks += 1
			if String(row.get("id", "")) != String(city.get("id", "")) \
					or float(row.get("value", 0.0)) != float(city.get(field, 0.0)):
				_fail("ranking '%s'[%d] = %s/%.17g, esperado %s/%.17g" % [
					field, index, row.get("id", ""), float(row.get("value", 0.0)),
					city.get("id", ""), float(city.get(field, 0.0))])
				break
	var nations: Array = status.get("nations", [])
	var nation_ranking: Array = sim.get_nation_ranking("population", 5)
	if nation_ranking.size() != mini(5, nations.size()):
		_fail("ranking de nações com %d linhas" % nation_ranking.size())
	else:
		var best_tag := ""
		var best_value := -INF
		for nation in nations:
			var value := float((nation as Dictionary).get("population", 0.0))
			var tag := String((nation as Dictionary).get("tag", ""))
			if value > best_value or (value == best_value and tag < best_tag):
				best_value = value
				best_tag = tag
		_checks += 1
		if String(nation_ranking[0].get("tag", "")) != best_tag:
			_fail("ranking de nações[0] = %s, esperado %s" % [
				nation_ranking[0].get("tag", ""), best_tag])


## A ficha de UMA cidade tem de custar < 1 ms (consulta sob demanda da UI).
func _check_city_card_latency(sim, status: Dictionary) -> void:
	var cities: Array = status.get("cities", [])
	if cities.is_empty():
		return
	var id := String((cities[cities.size() / 2] as Dictionary).get("id", ""))
	const RUNS := 20
	var t0 := Time.get_ticks_usec()
	for _run in RUNS:
		var card: Dictionary = sim.get_city_status(id)
		if card.is_empty():
			_fail("get_city_status(%s) vazio" % id)
			return
	var average_ms := float(Time.get_ticks_usec() - t0) / float(RUNS) / 1000.0
	print("[status_query] ficha de cidade: %.3f ms (média de %d)" % [
		average_ms, RUNS])
	_checks += 1
	if average_ms >= 1.0:
		_fail("ficha de cidade custa %.3f ms (alvo < 1 ms)" % average_ms)
