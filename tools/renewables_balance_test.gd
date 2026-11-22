extends SceneTree

## D-122 — BALANCEAMENTO DAS RENOVÁVEIS (decisão de design do dono).
##
## Aceites:
##   (1) custo: solar/eólica/hídrica custam MAIS que carvão/gás/óleo por MW
##       instalado e por MWh esperado (capital por MWh-ano + O&M), com O&M
##       próprio e prazo de hídrica maior; o status publica a tabela.
##   (2) produção: usina NOVA rende pelo potencial do sítio com teto de CF
##       realista e penalidade de balanceamento; solar/eólica/hídrica têm
##       variação diária determinística; a hídrica é sazonal; térmica é estável.
##   (3) intermitência não garante a demanda: crédito de capacidade baixo no pico
##       (a IA compara capex/credito = XGS por MW firme).
##   (4) curtailment: sem armazenamento, o excedente renovável é desperdiçado.
##   (5) ano-base INALTERADO (±1%): o parque existente continua reproduzindo a
##       geração Ember por fonte; save/load preserva o CF de sítio.
##
## Cenário: fixture AGRIA/PORTUS (nação DEMO, moeda DEM), como o
## e2e3_power_test. Roda pelo runner headless.

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

var _failures: Array[String] = []


func _init() -> void:
	_test_cost_table()
	_test_new_plant_potential_and_caps()
	_test_daily_variation_deterministic()
	_test_thermal_is_stable()
	_test_curtailment_without_storage()
	_test_year_base_unchanged()
	_test_new_hydro_season_mean_one()
	_test_save_roundtrip_new_plant()
	if _failures.is_empty():
		print("[renew] OK: capex/custo por MWh esperado, CF de sítio com teto, "
			+ "variação diária determinística, térmica estável, curtailment, "
			+ "ano-base ±1% e save do CF de sítio (D-122).")
		quit(0)
	else:
		print("[renew] FALHAS (%d):" % _failures.size())
		for failure: String in _failures:
			print(" - " + failure)
		quit(1)


# --- (1) custo declarado: renovável mais caro que térmica ------------------
func _test_cost_table() -> void:
	var params := _params()
	var sim = _world(_nation(), {
		"AGRIA": _city({"solar": 1.0, "wind": 1.0, "hydro": 1.0,
			"coal": 1.0, "gas": 1.0, "oil": 1.0}, {
			"solar": 10.0, "wind": 10.0, "hydro": 10.0,
			"coal": 10.0, "gas": 10.0, "oil": 10.0}),
	})
	var status: Dictionary = sim.get_power_status()
	var costs: Dictionary = status.get("build_costs", {})
	_check("(1) status publica a tabela de custo por fonte",
		costs.size() >= 9, "fontes=%d" % costs.size())
	var anchor := float(status.get("salary_day_xgs", 0.0))
	var renewals := ["solar", "wind", "hydro"]
	var thermals := ["coal", "gas", "oil"]
	for fuel: String in renewals:
		var row: Dictionary = costs.get(fuel, {})
		var declared := _param(params, "capex_per_mw_" + fuel)
		var mult := _param(params, "capex_mult_" + fuel)
		var capex := float(row.get("capex_per_mw", -1.0))
		_check("(1) %s: capex/MW = declarado x multiplicador x âncora" % fuel,
			absf(capex - declared * mult * anchor) <= 1e-6,
			"capex=%.6f esperado=%.6f" % [capex, declared * mult * anchor])
		# Capital por MWh-ano: capex / (8760 h x CF da usina nova).
		var cf_new := minf(float(row.get("cf_declared", 0.0)),
			float(row.get("cf_cap", 0.0))) \
			* float(row.get("balance_penalty", 0.0))
		var per_mwh := capex / (8760.0 * maxf(1e-9, cf_new))
		for other: String in thermals:
			var trow: Dictionary = costs.get(other, {})
			var tcapex := float(trow.get("capex_per_mw", 0.0))
			var tcf := maxf(1e-9, float(trow.get("cf_declared", 0.0)))
			var tper := tcapex / (8760.0 * tcf)
			_check("(1) %s é mais caro de construir que %s (capex/MW)" % [
					fuel, other],
				capex > tcapex,
				"%.2f vs %.2f" % [capex, tcapex])
			_check("(1) %s custa mais por MWh esperado que %s" % [
					fuel, other],
				per_mwh > tper,
				"%.3f vs %.3f" % [per_mwh, tper])
		_check("(1) %s: crédito de capacidade no pico é baixo" % fuel,
			float(row.get("capacity_credit", 1.0)) < 0.6,
			"credit=%s" % row.get("capacity_credit"))
		var opex := float(row.get("opex_per_mwh", 0.0))
		_check("(1) %s: O&M próprio e menor que a térmica" % fuel,
			opex > 0.0 and opex < float(costs["coal"].get(
				"opex_per_mwh", 0.0)),
			"opex=%s" % opex)
	_check("(1) hídrica tem prazo de obra maior",
		int(costs["hydro"].get("build_days", 0))
			> int(costs["solar"].get("build_days", 0)),
		"hidro=%s solar=%s" % [costs["hydro"].get("build_days"),
			costs["solar"].get("build_days")])
	# A IA vê a troca: custo por MW FIRME (capex/credito) põe a térmica na
	# frente da renovável, embora a renovável opere mais barato.
	var solar_credit := float(costs["solar"].get("capacity_credit", 0.0))
	var coal_credit := float(costs["coal"].get("capacity_credit", 0.0))
	var solar_firm := float(costs["solar"].get("capex_per_mw", 0.0)) \
		/ maxf(0.05, solar_credit)
	var coal_firm := float(costs["coal"].get("capex_per_mw", 0.0)) \
		/ maxf(0.05, coal_credit)
	_check("(2) IA: renovável é mais caro por MW firme que carvão",
		solar_firm > coal_firm and solar_credit < coal_credit,
		"solar=%.1f carvão=%.1f" % [solar_firm, coal_firm])


# --- (2) usina nova rende pelo sítio, com teto e penalidade ----------------
func _test_new_plant_potential_and_caps() -> void:
	var params := _params()
	# Sítio muito ensolarado (PVOUT 0,30 > teto 0,25) e muito ventoso (12 m/s).
	var solar_month: Array = []
	for _month in 12:
		solar_month.append(0.30)
	var sim = _world(_nation(100.0, 100.0, 100.0, 4.0), {
		"AGRIA": _city({"coal": 1.0}, {
			"solar": 100.0, "wind": 100.0, "hydro": 100.0},
			solar_month, 12.0),
	})
	sim.set_instant_construction(true)
	var built_solar: Dictionary = sim.build_power_plant("AGRIA", "solar", 1.0)
	var built_wind: Dictionary = sim.build_power_plant("AGRIA", "wind", 1.0)
	var built_hydro: Dictionary = sim.build_power_plant("AGRIA", "hydro", 1.0)
	_check("(2) obra nova de solar/eólica/hídrica aceita",
		bool(built_solar.get("started", false))
			and bool(built_wind.get("started", false))
			and bool(built_hydro.get("started", false)),
		"solar=%s wind=%s hydro=%s" % [built_solar.get("error"),
			built_wind.get("error"), built_hydro.get("error")])
	var new_plants: Array = sim.get_power_status().get("new_plants", [])
	var by_fuel: Dictionary = {}
	for row: Variant in new_plants:
		by_fuel[String((row as Dictionary).get("fuel", ""))] = row
	var solar_cap := float(params.get("cf_cap_solar", 0.0)) \
		* float(params.get("balance_penalty_solar", 0.0))
	var wind_cap := float(params.get("cf_cap_wind", 0.0)) \
		* float(params.get("balance_penalty_wind", 0.0))
	var hydro_cap := float(params.get("cf_hydro", 0.0)) \
		* float(params.get("balance_penalty_hydro", 0.0))
	var solar_cf := float(by_fuel.get("solar", {}).get("cf_base", -1.0))
	var wind_cf := float(by_fuel.get("wind", {}).get("cf_base", -1.0))
	var hydro_cf := float(by_fuel.get("hydro", {}).get("cf_base", -1.0))
	_check("(2) solar: CF do sítio limitado ao teto x penalidade",
		solar_cf > 0.0 and absf(solar_cf - solar_cap) <= 1e-9,
		"cf=%s teto=%s" % [solar_cf, solar_cap])
	_check("(2) eólica: CF do sítio limitado ao teto x penalidade",
		wind_cf > 0.0 and absf(wind_cf - wind_cap) <= 1e-9,
		"cf=%s teto=%s" % [wind_cf, wind_cap])
	_check("(2) hídrica: CF declarado x penalidade",
		hydro_cf > 0.0 and absf(hydro_cf - hydro_cap) <= 1e-9,
		"cf=%s esperado=%s" % [hydro_cf, hydro_cap])
	# Sítio sem dado de vento: usa o CF declarado (não inventa potencial).
	var plain = _world(_nation(), {
		"AGRIA": _city({"coal": 1.0}, {"wind": 100.0}),
	})
	plain.set_instant_construction(true)
	plain.build_power_plant("AGRIA", "wind", 1.0)
	var plain_rows: Array = plain.get_power_status().get("new_plants", [])
	var plain_cf := 0.0
	for row: Variant in plain_rows:
		plain_cf = float((row as Dictionary).get("cf_base", 0.0))
	_check("(2) sem vento medido a usina usa o CF declarado",
		absf(plain_cf - float(params.get("cf_wind", 0.0))
			* float(params.get("balance_penalty_wind", 0.0))) <= 1e-9,
		"cf=%s" % plain_cf)


# --- (2b) variação diária presente e determinística ------------------------
func _test_daily_variation_deterministic() -> void:
	var series_a := _solar_series()
	var series_b := _solar_series()
	_check("(2) solar: variação diária presente",
		_stddev(series_a) > 0.01, "serie=%s" % [series_a])
	_check("(2) solar: variação diária determinística",
		series_a == series_b, "%s != %s" % [series_a, series_b])
	var monotone := true
	for index in series_a.size() - 1:
		if not is_equal_approx(series_a[index], series_a[0]):
			monotone = false
	_check("(2) solar: a série não é constante", not monotone,
		"serie=%s" % [series_a])


func _solar_series() -> Array:
	var sim = _world(_nation(), {
		"AGRIA": _city({"coal": 1.0}, {"solar": 100.0}),
	})
	sim.set_instant_construction(true)
	sim.build_power_plant("AGRIA", "solar", 1.0)
	sim.set_external_income_index("AGRIA", _solar_income(5.2))
	var series: Array = []
	for _day in 15:
		sim.tick_day()
		series.append(_round6(_fuel(sim, "solar")))
	return series


## Renda que faz a demanda da cidade ficar perto de `target` MWh/dia.
func _solar_income(target: float) -> float:
	var sim = _world(_nation(), {
		"AGRIA": _city({"coal": 1.0}, {"solar": 100.0}),
	})
	sim.set_instant_construction(true)
	sim.build_power_plant("AGRIA", "solar", 1.0)
	sim.set_external_income_index("AGRIA", 1.0)
	sim.tick_day()
	var demand := float(_city_row(sim, "AGRIA").get("demand_mwh", 0.0))
	var income := 1.0
	for _attempt in 10:
		if demand <= 0.0:
			break
		income = clampf(income * pow(target / demand, 1.0 / 0.912238),
			0.05, 8.0)
		sim.set_external_income_index("AGRIA", income)
		sim.tick_day()
		demand = float(_city_row(sim, "AGRIA").get("demand_mwh", 0.0))
		if absf(demand - target) <= target * 0.01:
			break
	return income


# --- (3) térmica é estável ------------------------------------------------
func _test_thermal_is_stable() -> void:
	var park := 100.0
	var real_cf := 0.40
	var nation := _nation()
	nation["DEMO"]["generation_mwh_year"] = {"gas": park * real_cf * 8760.0}
	nation["DEMO"]["demand_base_mwh_day"] = 100000.0
	var sim = _world(nation, {"AGRIA": _city({"gas": park}, {"gas": 400.0})})
	var series: Array = []
	for _day in 5:
		sim.tick_day()
		series.append(_round6(_fuel(sim, "gas")))
	_check("(3) térmica despachável: geração estável dia a dia",
		_stddev(series) <= 1e-9 and series[0] > 0.0, "serie=%s" % [series])
	_check("(3) térmica calibrada reproduz o ano-base no dia",
		absf(series[0] - park * real_cf * 24.0) <= 1e-3,
		"geração=%s esperada=%s" % [series[0], park * real_cf * 24.0])


# --- (4) sem armazenamento o excedente é desperdiçado ----------------------
func _test_curtailment_without_storage() -> void:
	var park := 100.0
	var sim = _world(_nation(), {
		"AGRIA": _city({"solar": park}, {"solar": park}),
		"PORTUS": _city({}, {"solar": 1.0}),
	})
	sim.set_external_income_index("AGRIA", 0.05)
	sim.set_external_income_index("PORTUS", 0.05)
	sim.tick_day()
	var status: Dictionary = sim.get_power_status()
	var curtail := float(status.get("curtailment_mwh", -1.0))
	var avail := park * 0.22 * 24.0
	_check("(4) renovável em excesso é curtailed", curtail > 0.0,
		"curtailment=%s" % curtail)
	_check("(4) curtailment é a disponibilidade não despachada",
		absf(curtail - (avail - _fuel(sim, "solar"))) <= 1e-6,
		"curtailment=%s avail=%s geração=%s" % [
			curtail, avail, _fuel(sim, "solar")])
	_check("(4) a carga é servida mesmo com excedente desperdiçado",
		float(_grid(sim, "DEMO").get("served_ratio", 0.0)) > 0.999,
		"ratio=%s" % _grid(sim, "DEMO").get("served_ratio"))
	var grid: Dictionary = _grid(sim, "DEMO")
	_check("(4) a rede publica o próprio curtailment",
		absf(float(grid.get("curtailment_mwh", -1.0)) - curtail) <= 1e-6,
		"rede=%s mundo=%s" % [grid.get("curtailment_mwh"), curtail])


# --- (5) ano-base inalterado (±1%) por fonte -------------------------------
func _test_year_base_unchanged() -> void:
	var sources := {"solar": 0.22, "wind": 0.35, "hydro": 0.50}
	for fuel: String in sources:
		var park := 50.0
		var cf := float(sources[fuel])
		var anchor_mwh := park * cf * 8760.0
		var nation := _nation()
		var generation: Dictionary = {}
		generation[fuel] = anchor_mwh
		nation["DEMO"]["generation_mwh_year"] = generation
		nation["DEMO"]["demand_base_mwh_day"] = 100000.0
		var park_dict: Dictionary = {}
		park_dict[fuel] = park
		var cap_dict: Dictionary = {}
		cap_dict[fuel] = park * 2.0
		var sim = _world(nation, {"AGRIA": _city(park_dict, cap_dict)})
		var total := 0.0
		for _day in 365:
			sim.tick_day()
			total += _fuel(sim, fuel)
		var error := absf(total - anchor_mwh) / anchor_mwh
		_check("(5) ano-base Ember de %s inalterado (tolerância 1%%)" % fuel,
			error <= 0.01,
			"anual=%.3f âncora=%.3f erro=%.5f%%" % [
				total, anchor_mwh, error * 100.0])


# --- (2c) hídrica nova: sazonal e média anual 1 ---------------------------
func _test_new_hydro_season_mean_one() -> void:
	var sim = _world(_nation(), {
		"AGRIA": _city({"coal": 1.0}, {"hydro": 10.0}),
	})
	sim.set_instant_construction(true)
	sim.build_power_plant("AGRIA", "hydro", 1.0)
	var new_plants: Array = sim.get_power_status().get("new_plants", [])
	var cf := float((new_plants[0] as Dictionary).get("cf_base", 0.0))
	sim.set_external_income_index("AGRIA", 5.0)
	var monthly: Array = []
	var monthly_sum: Array = []
	for _month in 12:
		monthly_sum.append(0.0)
	var total := 0.0
	for day in 365:
		sim.tick_day()
		var generation := _fuel(sim, "hydro")
		total += generation
		monthly_sum[day / 31] += generation
	var expected := 1.0 * cf * 8760.0
	_check("(2) hídrica nova: média anual do fator é 1 (±1%)",
		absf(total - expected) / expected <= 0.01,
		"anual=%.3f esperado=%.3f" % [total, expected])
	var low: float = float(monthly_sum[0])
	var high: float = float(monthly_sum[0])
	for value: float in monthly_sum:
		low = minf(low, value)
		high = maxf(high, value)
	_check("(2) hídrica nova tem estação (cheia/seca)",
		high > low * 1.15, "mín=%.2f máx=%.2f" % [low, high])
	if monthly.is_empty():
		pass


# --- (5b) save/load preserva o CF de sítio --------------------------------
func _test_save_roundtrip_new_plant() -> void:
	var sim = _world(_nation(), {
		"AGRIA": _city({"coal": 1.0}, {"solar": 100.0}, [], 9.0),
	})
	sim.set_instant_construction(true)
	sim.build_power_plant("AGRIA", "solar", 1.0)
	var before: Array = sim.get_power_status().get("new_plants", [])
	var loaded = _load(sim.to_dict())
	var after: Array = loaded.get_power_status().get("new_plants", [])
	_check("(5) save/load preserva o CF de sítio da usina nova",
		before.size() == 1 and after.size() == 1
			and absf(float((before[0] as Dictionary).get("cf_base", 0.0))
				- float((after[0] as Dictionary).get("cf_base", 0.0)))
				<= 1e-12,
		"antes=%s depois=%s" % [before, after])


# --- helpers ---------------------------------------------------------------
func _world(nations: Dictionary, cities: Dictionary, days := 0,
		power_params: Dictionary = {}):
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
		"currencies": [{
			"code": "DEM", "name": "Demo", "symbol": "D",
			"nations": ["DEMO"], "rate_per_anchor": 1.0, "rate": 1.0,
			"base_rate": 1.0,
		}],
	})
	sim.initialize_scenario()
	sim.set_power_definitions(nations, cities, power_params, true)
	for _day in days:
		sim.tick_day()
	return sim


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


func _nation(access := 100.0, urban := 100.0, rural := 100.0,
		loss := 4.0) -> Dictionary:
	return {"DEMO": {
		"access_pct": access,
		"access_urban_pct": urban,
		"access_rural_pct": rural,
		"loss_pct": loss,
	}}


func _city(park: Dictionary, cap: Dictionary = {}, solar_month: Array = [],
		wind_ms := 0.0) -> Dictionary:
	return {
		"access_pct": 100.0,
		"loss_pct": 4.0,
		"park": park,
		"cap": cap,
		"solar_month": solar_month,
		"wind_ms": wind_ms,
	}


func _params() -> Dictionary:
	return (_json(POWER_PARAMS_PATH) as Dictionary).get("params", {})


func _param(params: Dictionary, key: String) -> float:
	return float(params.get(key, -1.0))


func _city_row(sim: Object, city_id: String) -> Dictionary:
	return (sim.get_power_status().get("cities", {}) as Dictionary).get(
		city_id, {})


func _grid(sim: Object, tag: String) -> Dictionary:
	return (sim.get_power_status().get("nations", {}) as Dictionary).get(
		tag, {})


func _fuel(sim: Object, fuel: String) -> float:
	var by_fuel: Dictionary = _grid(sim, "DEMO").get(
		"generation_mwh_by_fuel", {})
	return float(by_fuel.get(fuel, 0.0))


func _stddev(values: Array) -> float:
	if values.is_empty():
		return 0.0
	var mean := 0.0
	for value: Variant in values:
		mean += float(value)
	mean /= float(values.size())
	var variance := 0.0
	for value: Variant in values:
		variance += pow(float(value) - mean, 2.0)
	return sqrt(variance / float(values.size()))


func _round6(value: float) -> float:
	return snappedf(value, 0.000001)


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
