extends SceneTree

const GeographyServiceScript := preload("res://core/economy/geography_service.gd")
const GroupNamerScript := preload("res://core/economy/group_namer.gd")

## B2 — ESTRATOS DE RENDA (quintis) COM PATRIMÔNIO.
##
## Valida o padrão REAL que a fase B2 promete (plano Parte D), sem âncora:
##  - o Gini medido das rendas dos quintis no dia N fica perto do Gini OBSERVADO
##    da nação (World Bank, `inequality.json`) — o núcleo não impõe o alvo;
##  - a fração da renda gasta em comida CAI com o estrato;
##  - a taxa de poupança é maior no topo e na meia-idade;
##  - a soma dos quintis é a população/poupança da cidade (conservação) e o
##    dinheiro do mundo não deriva.
##
## Uso: godot --headless --path . --script res://tools/strata_test.gd -- --days=365
## Saída: docs/economy/perf/b2_strata_validation.json (evidência) + exit != 0 em
## falha.

const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const PORTS_PATH := "res://data/definitions/scenario/ports.json"
const BUILDINGS_PATH := "res://data/definitions/scenario/buildings.json"
const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"
const SERVICES_PATH := "res://data/definitions/economy/services.json"
const CURRENCIES_PATH := "res://data/definitions/economy/currencies.json"
const SECTOR_EMPLOYMENT_PATH := "res://data/definitions/economy/sector_employment.json"
const AGE_STRUCTURE_PATH := "res://data/definitions/population/age_structure_2026.json"
const URBAN_LAND_PATH := "res://data/definitions/economy/urban_land.json"
const HOUSING_CITY_PATH := "res://data/definitions/economy/housing_city.json"
const HOUSING_PATH := "res://data/definitions/economy/housing.json"
const MUNICIPAL_FINANCE_PATH := "res://data/definitions/economy/municipal_finance.json"
const INEQUALITY_PATH := "res://data/definitions/economy/inequality.json"
const OUT_PATH := "res://docs/economy/perf/b2_strata_validation.json"
const NATIONS := ["BRA", "USA", "DEU", "IND", "NGA", "JPN", "CHN", "ZAF",
	"MEX", "FRA"]
const GINI_TOLERANCE := 0.08

var _days := 365
var _failures: Array = []
var _report := {}


func _init() -> void:
	for arg: String in OS.get_cmdline_user_args():
		if arg.begins_with("--days="):
			_days = maxi(1, int(arg.split("=", true, 1)[1]))
	print("[strata] dias=%d" % _days)
	_run()
	if _failures.is_empty():
		print("[strata] OK: quintis, Gini emergente, comida e poupança validados.")
		quit(0)
	else:
		print("[strata] FALHAS (%d):" % _failures.size())
		for failure: String in _failures:
			print(" - " + failure)
		quit(1)


func _fail(message: String) -> void:
	_failures.append(message)


func _json(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("não abriu " + path)
		return null
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data


func _run() -> void:
	var sim = EconomySim.new()
	sim.set_goods(_json(GOODS_PATH))
	sim.set_recipes(_json(RECIPES_PATH))
	sim.set_needs(_json(NEEDS_PATH))
	sim.set_services(_json(SERVICES_PATH))
	sim.set_currencies(_json(CURRENCIES_PATH))
	sim.set_sector_employment(_json(SECTOR_EMPLOYMENT_PATH))
	sim.set_age_structure(_json(AGE_STRUCTURE_PATH))
	var cities: Array = _json(CITIES_PATH)
	var ports: Array = _json(PORTS_PATH)
	var buildings: Array = _json(BUILDINGS_PATH)
	var geo := GeographyServiceScript.new()
	geo.load_data()
	buildings = geo.resolve_initial_farms(buildings, cities)
	sim.initialize_world(cities, ports, buildings)
	sim.set_urban(_json(URBAN_LAND_PATH), _json(HOUSING_CITY_PATH),
		_json(HOUSING_PATH), _json(MUNICIPAL_FINANCE_PATH))
	var centers: Array = []
	for city: Dictionary in cities:
		centers.append({
			"id": String(city.get("id", "")),
			"nation": String(city.get("nation", "")),
			"center": Vector2(float(city.get("x", 0.0)), float(city.get("y", 0.0))),
		})
	geo.apply_to_sim(sim, centers)
	GroupNamerScript.name_groups(sim)
	sim.seed_industry()
	sim.set_ai_enabled(true)

	var money_start := float(sim.to_dict().get("money_total", 0.0))
	var mobility_dev := 0.0
	var violations_total := 0
	var day0: Dictionary = sim.get_city_status(String(cities[0].get("id", "")))
	# Seeding do dia 0: 5 quintis, população 20% cada e dado de renda somando 1.
	_check_seeding(sim, cities, day0)

	for day in _days:
		var status: Dictionary = sim.tick_day()
		var violations: Array = status.get("invariant_violations", [])
		if violations.size() > 0:
			violations_total += violations.size()
			if violations_total <= 3:
				_fail("dia %d: invariante: %s" % [day + 1, String(violations[0])])
	var money_end := float(sim.to_dict().get("money_total", 0.0))
	var drift := absf(money_end - money_start) / maxf(absf(money_start), 1.0)
	if drift > 0.01:
		_fail("drift de dinheiro %.6f > 0,01 em %d dias" % [drift, _days])

	# Estatísticas por nação (todos os 834 status no dia final).
	var observed := _observed_gini()
	var by_nation := {}
	var sums := {}
	for city: Dictionary in cities:
		var city_id := String(city.get("id", ""))
		var tag := String(city.get("nation", ""))
		var status: Dictionary = sim.get_city_status(city_id)
		var pop: Dictionary = status.get("pop", {})
		var strata: Dictionary = pop.get("income_strata", {})
		if strata.is_empty():
			_fail("%s sem income_strata no status" % city_id)
			continue
		var people: Array = strata.get("population", [])
		var income: Array = strata.get("income", [])
		var savings: Array = strata.get("savings", [])
		var sum_people := 0.0
		var sum_income := 0.0
		var sum_savings := 0.0
		for i in people.size():
			sum_people += float(people[i])
			sum_income += float(income[i])
			sum_savings += float(savings[i])
		var population := float(pop.get("population", 0.0))
		if population > 0.0 and absf(sum_people - population) > population * 1e-6:
			_fail("%s: soma dos quintis %.3f != população %.3f" % [
				city_id, sum_people, population])
		if absf(sum_savings - float(pop.get("savings", 0.0))) > maxf(1.0, sum_savings * 1e-9):
			_fail("%s: soma da poupança dos quintis %.6f != escalar %.6f" % [
				city_id, sum_savings, float(pop.get("savings", 0.0))])
		if not by_nation.has(tag):
			by_nation[tag] = {"income": [0.0, 0.0, 0.0, 0.0, 0.0],
				"people": 0.0}
		# Mobilidade: com o fluxo mensal vivo, os quintis NÃO ficam congelados
		# em 20% da população. Guarda o maior desvio observado.
		if population > 0.0:
			for i in people.size():
				mobility_dev = maxf(mobility_dev,
					absf(float(people[i]) - population / 5.0) / population)
		var bucket: Dictionary = by_nation[tag]
		for i in people.size():
			bucket["income"][i] += float(income[i])
			bucket["people"] += float(people[i])
		if not sums.has(tag):
			sums[tag] = {"food": 0.0, "income": 0.0, "savings": 0.0,
				"rate": 0.0, "cities": 0}
		var agg: Dictionary = sums[tag]
		var food_share: Array = strata.get("food_income_share", [])
		var rate: Array = strata.get("saving_rate", [])
		for i in people.size():
			agg["food"] += float(food_share[i]) * float(income[i])
			agg["income"] += float(income[i])
			agg["savings"] += float(savings[i])
			agg["rate"] += float(rate[i])
		agg["cities"] += 1

	if mobility_dev <= 1e-6:
		_fail("mobilidade não moveu nenhum quintil em %d dias (quintis presos em 20%%)" % _days)
	var table: Array = []
	for tag: String in NATIONS:
		if not by_nation.has(tag):
			_fail("nação %s ausente do mundo" % tag)
			continue
		var bucket: Dictionary = by_nation[tag]
		var gini_sim := _gini(bucket["income"], float(bucket["people"]))
		var gini_data := float(observed.get(tag, -1.0))
		var agg: Dictionary = sums[tag]
		var strata_food: Array = []
		var strata_rate: Array = []
		var strata_savings: Array = []
		for i in 5:
			# Média por quintil ponderada pela renda: proxy das fatias do país.
			strata_food.append(float(agg["food"]) / maxf(float(agg["income"]), 1e-9))
			strata_rate.append(float(agg["rate"]) / maxf(float(agg["cities"]), 1.0))
		table.append({"nation": tag, "gini_data": gini_data,
			"gini_sim": gini_sim, "delta": gini_sim - gini_data})
		if gini_data > 0.0 and absf(gini_sim - gini_data) > GINI_TOLERANCE:
			_fail("%s: Gini simulado %.3f vs dado %.3f (|Δ| > %.2f)" % [
				tag, gini_sim, gini_data, GINI_TOLERANCE])

	# Padrão por estrato, agregado mundial (média das cidades, ponderada pela
	# renda do estrato): comida ÷ renda cai com o quintil; poupança sobe.
	var world := {"food": [0.0, 0.0, 0.0, 0.0, 0.0],
		"rate": [0.0, 0.0, 0.0, 0.0, 0.0],
		"income": [0.0, 0.0, 0.0, 0.0, 0.0]}
	# (Repetição leve: usa o mesmo laço de status, agora global.)
	for city: Dictionary in cities:
		var status: Dictionary = sim.get_city_status(String(city.get("id", "")))
		var strata: Dictionary = status.get("pop", {}).get("income_strata", {})
		var income: Array = strata.get("income", [])
		var food_share: Array = strata.get("food_income_share", [])
		var rate: Array = strata.get("saving_rate", [])
		for i in income.size():
			world["income"][i] += float(income[i])
			world["food"][i] += float(food_share[i]) * float(income[i])
			world["rate"][i] += float(rate[i])
	for i in 5:
		world["food"][i] = float(world["food"][i]) / maxf(float(world["income"][i]), 1e-9)
		world["rate"][i] = float(world["rate"][i]) / maxf(float(cities.size()), 1.0)
	if not (float(world["food"][0]) > float(world["food"][1])
			and float(world["food"][1]) > float(world["food"][2])):
		_fail("fração da renda em comida não cai do Q1 para o Q3: %s" % str(world["food"]))
	if not (float(world["rate"][4]) > float(world["rate"][0])):
		_fail("taxa de poupança do topo não é maior que a do fundo: %s" % str(world["rate"]))

	# Ciclo de vida: meia-idade poupa mais que jovem e idoso (fator por faixa).
	var lifecycle: Array = day0.get("pop", {}).get("income_strata", {}).get(
		"lifecycle_saving", [])
	if lifecycle.size() >= 5:
		if not (float(lifecycle[2]) > float(lifecycle[1])
				and float(lifecycle[2]) > float(lifecycle[4])):
			_fail("ciclo de vida: meia-idade não poupa mais que jovem/idoso: %s" % str(lifecycle))
	else:
		_fail("status sem lifecycle_saving")

	_report = {
		"days": _days,
		"money_drift": drift,
		"invariant_violations": violations_total,
		"nations": table,
		"world_food_income_share": world["food"],
		"world_saving_rate": world["rate"],
		"lifecycle_saving": lifecycle,
		"mobility_max_deviation": mobility_dev,
		"gini_tolerance": GINI_TOLERANCE,
	}
	var file := FileAccess.open(OUT_PATH, FileAccess.WRITE)
	if file != null:
		file.store_string(JSON.stringify(_report, "  "))
		file.close()
		print("[strata] evidência em " + OUT_PATH)
	print("[strata] Gini dado × simulado (dia %d):" % _days)
	for row: Dictionary in table:
		print("  %-4s dado=%.3f sim=%.3f Δ=%+.3f" % [row["nation"],
			row["gini_data"], row["gini_sim"], row["delta"]])
	print("[strata] comida/renda Q1..Q5 = %s" % str(world["food"]))
	print("[strata] poupança Q1..Q5 = %s" % str(world["rate"]))
	print("[strata] drift=%.8f violações=%d" % [drift, violations_total])


func _check_seeding(sim, cities: Array, day0: Dictionary) -> void:
	var pop: Dictionary = day0.get("pop", {})
	var strata: Dictionary = pop.get("income_strata", {})
	if strata.is_empty():
		_fail("dia 0: status sem income_strata (binário sem B2?)")
		return
	var people: Array = strata.get("population", [])
	if people.size() != 5:
		_fail("dia 0: %d quintis (esperado 5)" % people.size())
	# Nacional: quintis de renda observados normalizados por nação.
	var inequality: Dictionary = _json(INEQUALITY_PATH)
	var nations: Dictionary = inequality.get("nations", {})
	var checked := 0
	for city: Dictionary in cities:
		var tag := String(city.get("nation", ""))
		var status: Dictionary = sim.get_city_status(String(city.get("id", "")))
		var shares: Array = status.get("pop", {}).get("income_strata", {}).get(
			"income_share", [])
		if shares.size() != 5:
			_fail("%s: income_share com %d posições" % [city.get("id", ""), shares.size()])
			continue
		var sum := 0.0
		for value: Variant in shares:
			sum += float(value)
		if absf(sum - 1.0) > 1e-6:
			_fail("%s: income_share soma %.6f" % [city.get("id", ""), sum])
		# Onde a nação tem quintis OBSERVADOS, a semente tem de ser o dado.
		var row: Dictionary = nations.get(tag, {})
		var deciles: Dictionary = row.get("deciles", {})
		if deciles.has("lowest_20") and checked < 50:
			var expected := [
				float(deciles["lowest_20"]["value"]),
				float(deciles["second_20"]["value"]),
				float(deciles["third_20"]["value"]),
				float(deciles["fourth_20"]["value"]),
				float(deciles["highest_20"]["value"])]
			var total := 0.0
			for value: float in expected:
				total += value
			for i in 5:
				if absf(float(shares[i]) - expected[i] / total) > 1e-6:
					_fail("%s: quintil %d semeado %.4f != dado %.4f" % [
						city.get("id", ""), i, float(shares[i]), expected[i] / total])
					break
			checked += 1
	print("[strata] semeadura conferida em %d cidades com quintis observados" % checked)


func _observed_gini() -> Dictionary:
	var data: Dictionary = _json(INEQUALITY_PATH)
	var nations: Dictionary = data.get("nations", {})
	var out := {}
	for tag: String in nations:
		var row: Dictionary = nations[tag]
		if row.has("gini"):
			out[tag] = clampf(float(row["gini"]) / 100.0, 0.0, 1.0)
	return out


## Gini a partir das 5 rendas agregadas (quintis iguais em população).
func _gini(income: Array, people: float) -> float:
	var mean: Array = []
	var total := 0.0
	for value: Variant in income:
		mean.append(float(value))
		total += float(value)
	if total <= 0.0:
		return 0.0
	var average := total / float(income.size())
	var acc := 0.0
	for a: Variant in mean:
		for b: Variant in mean:
			acc += absf(float(a) - float(b))
	return clampf(acc / (2.0 * pow(float(income.size()), 2.0) * average), 0.0, 1.0)
