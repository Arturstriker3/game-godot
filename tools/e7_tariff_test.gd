extends SceneTree

## PARTE E / D-120 — E7: TARIFA DE VAREJO por rede e CONTRATO de importação.
##
## Aceites:
##   (1) conservação de dinheiro e soma zero do setor com a tarifa (a margem
##       de rede/estado sai do caixa do consumidor e entra no orçamento, sem
##       criar nem destruir dinheiro);
##   (2) ISL: um déficit de 0,05% leva o SPOT ao teto de escassez, mas a conta
##       do consumidor (tarifa) fica perto do custo médio — não 884% do PIB;
##   (3) racionamento continua doendo: produção/indústria e insatisfação caem
##       e o repasse de escassez aparece na tarifa (suavizado);
##   (4) contrato de suprimento: o exportador entrega antes da demanda
##       marginal, o contrato cai proporcionalmente na escassez do exportador
##       e o par sobrevive ao save/load (v37).
##
##   godot --headless --path . --script res://tools/e7_tariff_test.gd

const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"
const SERVICES_PATH := "res://data/definitions/economy/services.json"
const CURRENCIES_PATH := "res://data/definitions/economy/currencies.json"
const SECTOR_EMPLOYMENT_PATH := \
	"res://data/definitions/economy/sector_employment.json"
const AGE_STRUCTURE_PATH := \
	"res://data/definitions/population/age_structure_2026.json"
const POWER_PARAMS_PATH := "res://data/definitions/energy/power_params.json"

const EPS := 1e-6
const MONEY_EPSILON := 1e-3
const POPULATION := 50000.0

var _failures: Array[String] = []


func _init() -> void:
	_test_tariff_money_conservation()
	_test_isl_tiny_deficit_is_not_a_ceiling_bill()
	_test_rationing_still_hurts()
	_test_supply_contract_and_save()
	if _failures.is_empty():
		print("[e7] OK: tarifa de varejo (conservação, ISL, racionamento) e "
			+ "contrato de suprimento (entrega, escassez, save v37).")
		quit(0)
	else:
		for failure in _failures:
			print("[e7] FALHA: " + failure)
		quit(1)


# --- (1) conservação de dinheiro com a tarifa -------------------------------
func _test_tariff_money_conservation() -> void:
	var sim = _world({
		"AAA-ONE": _city({"coal": 60.0}, {"coal": 120.0}),
		"BBB-ONE": _city({"hydro": 80.0}, {"hydro": 160.0}),
	})
	var money_before := float(sim.get_status().get("money_total", 0.0))
	var tariff_seen := false
	for day in 5:
		sim.tick_day()
		var power: Dictionary = sim.get_power_status()
		var money := float(sim.get_status().get("money_total", 0.0))
		_check("(1) dinheiro conservado no dia %d" % (day + 1),
			absf(money - money_before) <= MONEY_EPSILON,
			"%.6f -> %.6f" % [money_before, money])
		_check("(1) caixa do setor fecha em zero no dia %d" % (day + 1),
			absf(float(power.get("cash", -1.0))) <= EPS,
			"power_cash=%s" % power.get("cash"))
		var violations: Array = sim.get_status().get(
			"invariant_violations", [])
		_check("(1) sem violação de invariante no dia %d" % (day + 1),
			violations.is_empty(), str(violations))
		for tag: Variant in power.get("nations", {}):
			var row: Dictionary = power["nations"][tag]
			if float(row.get("tariff", 0.0)) > 0.0:
				tariff_seen = true
	_check("(1) a tarifa de varejo foi publicada", tariff_seen, "tarifa=0")


# --- (2) ISL: déficit de 0,05% não vira conta no teto ----------------------
func _test_isl_tiny_deficit_is_not_a_ceiling_bill() -> void:
	# Uma rede hidro de 100 MW entrega 1.200 MWh/dia no fator 0,50. A renda é
	# calibrada num mundo descartável para a demanda ficar 0,05% ACIMA da
	# disponibilidade — o caso ISL do §8.6. O mundo do teste começa limpo para
	# o repasse de escassez não herdar o degrau da calibração.
	var available := 100.0 * 0.50 * 24.0
	var target := available * 1.0005
	var params := _params()
	var elasticity := maxf(0.01, _param(params, "income_elasticity"))
	var income := _calibrate_income(target, elasticity)
	var sim = _world({
		"ISL-ONE": _city({"hydro": 100.0}, {"hydro": 200.0}),
	})
	sim.set_external_income_index("ISL-ONE", income)
	sim.tick_day()
	var city: Dictionary = _city_power(sim, "ISL-ONE")
	var status: Dictionary = sim.get_power_status()
	var ceiling := float(status.get("price_ceiling", 0.0))
	var price := float(city.get("price", 0.0))
	var tariff := float(city.get("tariff", 0.0))
	var average := float(city.get("average_cost", 0.0))
	var served_ratio := float(city.get("served_ratio", 1.0))
	var share := clampf(_param(params, "retail_network_share"), 0.0, 0.9)
	_check("(2) déficit pequeno leva o spot ao teto",
		absf(price - ceiling) <= 1e-6 and served_ratio > 0.998,
		"price=%.6f ceiling=%.6f served=%.6f" % [
			price, ceiling, served_ratio])
	var base_tariff := average / (1.0 - share)
	_check("(2) tarifa fica perto do custo médio, não do teto",
		tariff > 0.0 and tariff < ceiling * 0.25
			and absf(tariff - base_tariff) <= base_tariff * 0.01,
		"tariff=%.6f base=%.6f avg=%.6f ceiling=%.6f" % [
			tariff, base_tariff, average, ceiling])
	var bill := float(city.get("served_mwh", 0.0)) * tariff
	var ceiling_bill := float(city.get("served_mwh", 0.0)) * price
	_check("(2) conta fica abaixo de 1,5% da conta no teto",
		bill <= ceiling_bill * 0.015, "bill=%.6f teto=%.6f" % [
			bill, ceiling_bill])


## Renda que faz a demanda do mundo ISL ficar em `target` MWh/dia. Roda num
## mundo descartável (a calibração não contamina o repasse de escassez).
func _calibrate_income(target: float, elasticity: float) -> float:
	var sim = _world({
		"ISL-ONE": _city({"hydro": 100.0}, {"hydro": 200.0}),
	})
	sim.set_external_income_index("ISL-ONE", 1.0)
	sim.tick_day()
	var demand := float(_city_power(sim, "ISL-ONE").get("demand_mwh", 0.0))
	var income := 1.0
	for _attempt in 8:
		if demand <= 0.0:
			break
		income = clampf(income * pow(target / demand, 1.0 / elasticity),
			0.05, 8.0)
		sim.set_external_income_index("ISL-ONE", income)
		sim.tick_day()
		demand = float(_city_power(sim, "ISL-ONE").get("demand_mwh", 0.0))
		if absf(demand - target) <= target * 0.002:
			break
	return income


# --- (3) racionamento continua doendo --------------------------------------
func _test_rationing_still_hurts() -> void:
	# 20 MW de hidro entregam 240 MWh/dia contra ~413 MWh: falta ~42%.
	var sim = _world({
		"AAA-ONE": _city({"hydro": 20.0}, {"hydro": 40.0}),
	})
	for day in 5:
		sim.tick_day()
	var city: Dictionary = _city_power(sim, "AAA-ONE")
	var grid: Dictionary = _grid(sim, "AAA")
	var served := float(city.get("served_ratio", 1.0))
	var industry := float(city.get("industry_factor", 1.0))
	var stress := float(city.get("stress", 0.0))
	var tariff := float(city.get("tariff", 0.0))
	var price := float(city.get("price", 0.0))
	var pass_ := float(city.get("scarcity_pass", 0.0))
	_check("(3) a energia não servida continua racionada", served < 0.995,
		"served=%.6f" % served)
	_check("(3) a produção industrial cai", industry < 0.95,
		"industry=%.6f" % industry)
	_check("(3) a insatisfação sobe", stress > 0.0, "stress=%.6f" % stress)
	_check("(3) a rede conta o dia como racionamento",
		int(grid.get("ration_days", 0)) >= 5,
		"ration_days=%s" % grid.get("ration_days"))
	_check("(3) o repasse de escassez entra na tarifa",
		pass_ > 0.0 and tariff > 0.0 and tariff < price,
		"pass=%.9f tariff=%.9f spot=%.9f" % [pass_, tariff, price])


# --- (4) contrato de suprimento + save v37 ---------------------------------
func _test_supply_contract_and_save() -> void:
	# CCC importa de BBB; BBB tem hidro com sobra. Contrato de 40 MW.
	var sim = _world({
		"BBB-ONE": _city({"hydro": 150.0}, {"hydro": 300.0}),
		"CCC-ONE": _city({"coal": 5.0}, {"coal": 10.0}),
	}, [{
		"a": "BBB", "b": "CCC", "importer": "CCC",
		"capacity_mw": 80.0, "contract_mw": 40.0,
	}])
	sim.tick_day()
	var status: Dictionary = sim.get_power_status()
	var links: Array = status.get("interconnects", [])
	_check("(4) a linha do contrato foi semeada", links.size() == 1,
		"links=%d" % links.size())
	if links.is_empty():
		return
	var line: Dictionary = links[0]
	var flow := float(line.get("contract_flow_mwh", 0.0))
	var sent := float(line.get("contract_sent_mwh", 0.0))
	var ccc: Dictionary = _city_power(sim, "CCC-ONE")
	var bbb: Dictionary = _city_power(sim, "BBB-ONE")
	_check("(4) o exportador entrega o contrato antes da demanda marginal",
		flow > 0.0 and absf(float(ccc.get("served_mwh", 0.0))) > 0.0,
		"flow=%.6f served_ccc=%.6f" % [
			flow, float(ccc.get("served_mwh", 0.0))])
	_check("(4) o contrato respeita a capacidade da linha",
		sent <= 80.0 * 24.0 + EPS, "sent=%.6f" % sent)
	_check("(4) o exportador ainda atende a própria demanda",
		absf(float(bbb.get("served_ratio", 0.0)) - 1.0) <= 1e-6,
		"ratio_bbb=%.6f" % float(bbb.get("served_ratio", 0.0)))
	var data: Dictionary = sim.to_dict()
	var loaded = _load(data)
	var a := JSON.stringify(status, "", true, true)
	var b := JSON.stringify(loaded.get_power_status(), "", true, true)
	_check("(4) save v37 preserva o contrato e a tarifa", a == b,
		"status diverge no round-trip")


# --- infraestrutura do fixture ---------------------------------------------
func _world(cities_power: Dictionary, links: Array = []):
	var sim = EconomySim.new()
	sim.set_sim_threads(1)
	sim.set_goods(_json(GOODS_PATH))
	sim.set_recipes(_json(RECIPES_PATH))
	sim.set_needs(_json(NEEDS_PATH))
	sim.set_services(_json(SERVICES_PATH))
	sim.set_sector_employment(_json(SECTOR_EMPLOYMENT_PATH))
	sim.set_age_structure(_json(AGE_STRUCTURE_PATH))
	sim.set_currencies({
		"regime": "peg",
		"currencies": [
			{"code": "AUX", "name": "Aux", "symbol": "A",
				"nations": ["AAA", "ISL", "BBB", "CCC"],
				"rate_per_anchor": 1.0, "rate": 1.0, "base_rate": 1.0},
		],
	})
	sim.initialize_world(_cities(), [], [])
	var nations := {
		"AAA": _nation(),
		"ISL": _nation(),
		"BBB": _nation(),
		"CCC": _nation(),
	}
	sim.set_power_definitions(nations, cities_power, {}, true)
	if not links.is_empty():
		sim.set_power_interconnects(links, true)
	return sim


func _cities() -> Array:
	var rows: Array = []
	var tags := ["AAA", "ISL", "BBB", "CCC"]
	for index in tags.size():
		var tag: String = tags[index]
		rows.append({
			"id": "%s-ONE" % tag, "name": tag, "nation": tag,
			"home_nation": tag, "landmass": "north",
			"population": POPULATION, "simulation_population": POPULATION,
			"sim_population": POPULATION,
			"x": 2500.0 + index * 20.0, "y": 600.0,
		})
	return rows


func _nation() -> Dictionary:
	return {
		"access_pct": 100.0,
		"access_urban_pct": 100.0,
		"access_rural_pct": 100.0,
		"loss_pct": 4.0,
	}


func _city(park: Dictionary, cap: Dictionary = {}) -> Dictionary:
	return {
		"access_pct": 100.0,
		"loss_pct": 4.0,
		"park": park,
		"cap": cap,
		"solar_month": [],
	}


func _load(data: Dictionary):
	var sim = EconomySim.new()
	sim.set_sim_threads(1)
	sim.set_goods(_json(GOODS_PATH))
	sim.set_recipes(_json(RECIPES_PATH))
	sim.set_needs(_json(NEEDS_PATH))
	sim.set_services(_json(SERVICES_PATH))
	sim.set_sector_employment(_json(SECTOR_EMPLOYMENT_PATH))
	sim.set_age_structure(_json(AGE_STRUCTURE_PATH))
	sim.from_dict(data)
	return sim


func _params() -> Dictionary:
	return (_json(POWER_PARAMS_PATH) as Dictionary).get("params", {})


func _param(params: Dictionary, key: String) -> float:
	return float(params.get(key, -1.0))


func _grid(sim: Object, tag: String) -> Dictionary:
	return (sim.get_power_status().get("nations", {}) as Dictionary).get(
		tag, {})


func _city_power(sim: Object, city_id: String) -> Dictionary:
	var cities: Dictionary = sim.get_power_status().get("cities", {})
	return cities.get(city_id, {})


func _check(name: String, ok: bool, detail: String) -> void:
	if not ok:
		_failures.append("%s (%s)" % [name, detail])


func _json(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return {}
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data
