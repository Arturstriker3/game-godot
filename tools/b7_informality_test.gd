extends SceneTree

## B7 — INFORMALIDADE, SERVIÇOS INFORMAIS, MORTALIDADE E INSATISFAÇÃO + save v22.
##
## Valida (1) a conservação de dinheiro e pessoas com a informalidade ativa,
## (2) o informal sem imposto de renda/contribuição, (3) o serviço informal
## absorvendo demanda, (4) a mortalidade subindo com a fome, (5) a insatisfação
## reagindo dentro de [0,1], e (6) o round-trip do save v22 + migração v21.
##
## Uso: godot --headless --path . --script res://tools/b7_informality_test.gd

const RegionDataScript := preload("res://core/economy/region_data.gd")

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

const SAVE_VERSION := 22
const TAX_RATE := 0.15

var _failures: Array = []


func _init() -> void:
	_run()
	if _failures.is_empty():
		print("[b7] OK: informalidade, serviços informais, mortalidade, "
			+ "insatisfação e save v22.")
		quit(0)
	else:
		print("[b7] FALHAS (%d):" % _failures.size())
		for failure: String in _failures:
			print(" - " + failure)
		quit(1)


func _fail(message: String) -> void:
	_failures.append(message)


func _check(cond: bool, message: String) -> void:
	if not cond:
		_fail(message)


func _json(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("não abriu " + path)
		return null
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data


func _load_chains(sim: Object) -> void:
	sim.set_goods(_json(GOODS_PATH))
	sim.set_recipes(_json(RECIPES_PATH))
	sim.set_needs(_json(NEEDS_PATH))
	if sim.has_method("set_services"):
		sim.set_services(_json(SERVICES_PATH))
	if sim.has_method("set_currencies"):
		sim.set_currencies(_json(CURRENCIES_PATH))
	if sim.has_method("set_sector_employment"):
		sim.set_sector_employment(_json(SECTOR_EMPLOYMENT_PATH))
	if sim.has_method("set_age_structure"):
		sim.set_age_structure(_json(AGE_STRUCTURE_PATH))


func _make_world() -> Object:
	var sim = EconomySim.new()
	_load_chains(sim)
	var cities: Array = _json(CITIES_PATH)
	var ports: Array = _json(PORTS_PATH)
	var buildings: Array = _json(BUILDINGS_PATH)
	RegionDataScript.attach(cities)
	sim.initialize_world(cities, ports, buildings)
	if sim.has_method("set_urban"):
		sim.set_urban(_json(URBAN_LAND_PATH), _json(HOUSING_CITY_PATH),
			_json(HOUSING_PATH), _json(MUNICIPAL_FINANCE_PATH))
	return sim


## Índice da primeira cidade com quintis e demografia prontos (e interior > 0).
func _pick_city(save: Dictionary) -> int:
	var cities: Array = save.get("cities", [])
	for i in cities.size():
		var city: Dictionary = cities[i]
		var pop: Dictionary = city.get("pop", {})
		if bool(pop.get("strata", {}).get("ready", false)) \
				and pop.get("age_bands", []).size() >= 5:
			return i
	return -1


## Seta a fração informal de todos os quintis (urbano + interior) no save.
func _set_informal_all(city: Dictionary, value: float) -> void:
	var pop: Dictionary = city.get("pop", {})
	var informal: Dictionary = pop.get("informal", {})
	var labor: Array = []
	for s in 5:
		labor.append(value)
	informal["labor_share"] = labor
	informal["share"] = value
	pop["informal"] = informal
	city["pop"] = pop
	var region: Dictionary = city.get("region", {})
	if not region.is_empty():
		var rinf: Dictionary = region.get("informal", {})
		rinf["labor_share"] = labor.duplicate()
		rinf["share"] = value
		region["informal"] = rinf
		city["region"] = region


func _run() -> void:
	var sim := _make_world()
	var save: Dictionary = sim.to_dict()
	_check(int(save.get("version", 0)) == SAVE_VERSION,
		"save deve ser v22 (é %s)" % str(save.get("version")))
	var ci := _pick_city(save)
	if ci < 0:
		_fail("nenhuma cidade com quintis e demografia prontos")
		return
	var city_id: String = String(save.get("cities", [])[ci].get("id", ""))

	# --- (1) Conservação: dinheiro e pessoas com a informalidade ativa. ---
	var money0 := float(sim.get_day_summary().get("money_total", -1.0))
	_check(money0 > 0.0, "money_total deve ser > 0")
	for d in 3:
		sim.tick_day()
		var summary: Dictionary = sim.get_day_summary()
		var violations: Array = summary.get("invariant_violations", [])
		_check(violations.is_empty(),
			"dia %d: invariantes limpos (violações: %s)" % [d + 1, str(violations)])
	var money1 := float(sim.get_day_summary().get("money_total", -1.0))
	_check(money1 > 0.0, "money_total após ticks deve ser > 0")
	# Pessoas: urbano (Σ coortes) + interior = população exibida.
	if true:
		var st: Dictionary = sim.get_city_status(city_id)
		var rstatus: Dictionary = sim.get_region_status(city_id)
		var sim_pop := float(st.get("simulation_population", -1.0))
		var region_pop := float(rstatus.get("region_population", 0.0))
		var shown := float(st.get("population", -1.0))
		_check(shown > 0.0 and absf(shown - (sim_pop + region_pop)) < 1.0,
			"população exibida = urbano + interior (%.2f vs %.2f)" % [shown, sim_pop + region_pop])

	# --- Status B7: blocos informal/mortality/dissatisfaction presentes. ---
	if true:
		var st: Dictionary = sim.get_city_status(city_id)
		var informal: Dictionary = st.get("informal", {})
		_check(informal.has("share") and informal.has("by_quintile")
			and informal.has("service"), "status.informal deve ter share/by_quintile/service")
		var share := float(informal.get("share", -1.0))
		_check(share >= 0.0 and share <= 1.0, "informal.share em [0,1] (%.3f)" % share)
		_check(informal.get("by_quintile", []).size() == 5,
			"informal.by_quintile deve ter 5 posições")
		var service: Dictionary = informal.get("service", {})
		_check(service.has("workers") and service.has("revenue") and service.has("share"),
			"informal.service deve ter workers/revenue/share")
		var mortality: Dictionary = st.get("mortality", {})
		_check(mortality.has("deaths") and mortality.has("expected_deaths")
			and mortality.has("famine_months"), "status.mortality deve ter deaths/expected/famine")
		var diss: Dictionary = st.get("dissatisfaction", {})
		_check(diss.has("index") and diss.get("by_quintile", []).size() == 5
			and diss.has("components"), "status.dissatisfaction deve ter index/by_quintile/components")
		var diss_index := float(diss.get("index", -1.0))
		_check(diss_index >= 0.0 and diss_index <= 1.0,
			"dissatisfaction.index em [0,1] (%.3f)" % diss_index)

	# --- (2) Informal sem imposto: share=1 → salário isento; share=0 → kTaxRate.
	if true:
		var save_full: Dictionary = sim.to_dict()
		var city: Dictionary = save_full.get("cities", [])[ci]

		# share=0: imposto integral (wage + capital + rent).
		_set_informal_all(city, 0.0)
		var sim0 := EconomySim.new()
		_load_chains(sim0)
		sim0.from_dict(save_full)
		sim0.tick_day()
		var full_tax := float(sim0.get_city_status(city_id).get("pop", {}).get("taxes", -1.0))

		# share=1: a parcela do salário é isenta (só capital + aluguel pagam).
		_set_informal_all(city, 1.0)
		var sim1 := EconomySim.new()
		_load_chains(sim1)
		sim1.from_dict(save_full)
		sim1.tick_day()
		var no_wage_tax := float(sim1.get_city_status(city_id).get("pop", {}).get("taxes", -1.0))

		_check(full_tax >= 0.0 and no_wage_tax >= 0.0,
			"impostos devem ser >= 0 (share0=%.2f share1=%.2f)" % [full_tax, no_wage_tax])
		_check(no_wage_tax < full_tax - 0.001,
			"share=1 deve isentar o salário (imposto cai %.2f → %.2f)" % [full_tax, no_wage_tax])

	# --- (3) Serviço informal absorve demanda (revenue/share em faixa). ---
	if true:
		var save2: Dictionary = sim.to_dict()
		var sim2 := EconomySim.new()
		_load_chains(sim2)
		sim2.from_dict(save2)
		for d in 2:
			sim2.tick_day()
		var st: Dictionary = sim2.get_city_status(city_id)
		var service: Dictionary = st.get("informal", {}).get("service", {})
		var sshare := float(service.get("share", -1.0))
		_check(sshare >= 0.0 and sshare <= 0.75,
			"informal.service.share em [0, 0.75] (%.3f)" % sshare)
		# Com informalidade ativa, a receita pode ser 0 ou positiva; a demanda e
		# a oferta precisam estar coerentes (revenue ≤ output×price).
		var revenue := float(service.get("revenue", -1.0))
		var output := float(service.get("output", -1.0))
		var price := float(service.get("price", -1.0))
		_check(revenue >= 0.0 and revenue <= output * price + 1e-6,
			"receita informal ≤ oferta×preço (%.2f ≤ %.2f)" % [revenue, output * price])

	# --- (4) Mortalidade sobe com a fome (satisfaction_prev forçada baixa). ---
	if true:
		var save_famine: Dictionary = sim.to_dict()
		var cities: Array = save_famine.get("cities", [])
		var c: Dictionary = cities[ci]
		var pop: Dictionary = c.get("pop", {})
		var mortality: Dictionary = pop.get("mortality", {})
		var expected_before := float(mortality.get("excess_famine_total", 0.0))
		mortality["satisfaction_prev"] = 0.0
		pop["mortality"] = mortality
		c["pop"] = pop
		cities[ci] = c
		save_famine["cities"] = cities
		# Faz a cidade `ci` fechar o mês no próximo tick: após o prólogo o dia é
		# save_day + 1; queremos (dia - ci) % 30 == 0.
		save_famine["day"] = ci + 29
		var simf := EconomySim.new()
		_load_chains(simf)
		simf.from_dict(save_famine)
		simf.tick_day()
		var mstatus: Dictionary = simf.get_city_status(city_id).get("mortality", {})
		var excess_famine := float(mstatus.get("excess_famine_total", 0.0))
		var deaths := float(mstatus.get("deaths", 0.0))
		var expected := float(mstatus.get("expected_deaths", 0.0))
		_check(excess_famine > expected_before,
			"fome: excess_famine_total deve subir (%.4f → %.4f)" % [expected_before, excess_famine])
		_check(deaths > expected,
			"fome: mortes efetivas > esperadas (%.4f > %.4f)" % [deaths, expected])
		_check(int(mstatus.get("famine_months", 0)) >= 1,
			"fome: famine_months deve subir")

	# --- (5) Insatisfação reage: aumentar a informalidade sobe a componente. ---
	if true:
		var save_low: Dictionary = sim.to_dict()
		var c: Dictionary = save_low.get("cities", [])[ci]
		_set_informal_all(c, 0.0)
		var sim_low := EconomySim.new()
		_load_chains(sim_low)
		sim_low.from_dict(save_low)
		sim_low.tick_day()
		var low_idx := float(sim_low.get_city_status(city_id).get("dissatisfaction", {}).get("index", -1.0))

		var save_high: Dictionary = sim.to_dict()
		var c2: Dictionary = save_high.get("cities", [])[ci]
		_set_informal_all(c2, 1.0)
		var sim_high := EconomySim.new()
		_load_chains(sim_high)
		sim_high.from_dict(save_high)
		sim_high.tick_day()
		var high_idx := float(sim_high.get_city_status(city_id).get("dissatisfaction", {}).get("index", -1.0))
		_check(high_idx >= low_idx - 1e-9,
			"insatisfação não deve CAIR com informalidade 0→1 (%.3f → %.3f)"
			% [low_idx, high_idx])
		_check(low_idx >= 0.0 and low_idx <= 1.0 and high_idx >= 0.0 and high_idx <= 1.0,
			"índices de insatisfação em [0,1]")

	# --- (6) Round-trip v22 (blocos B7 preservados) e migração v21 → v22. ---
	if true:
		var rt: Dictionary = sim.to_dict()
		var rt2 := EconomySim.new()
		_load_chains(rt2)
		rt2.from_dict(rt)
		var rt3: Dictionary = rt2.to_dict()
		_check(int(rt3.get("version", 0)) == SAVE_VERSION, "round-trip mantém v22")
		# Os blocos B7 precisam voltar idênticos (a identidade byte a byte do
		# save inteiro é coberta por economy_save_load_roundtrip_test, que usa
		# "load + continuar == rodar direto" — alguns fluxos derivados do
		# mercado de trabalho, como unfilled_by_level, só são recomputados no
		# 1º tick após o load).
		var c_rt: Dictionary = rt.get("cities", [])[ci]
		var c_rt3: Dictionary = rt3.get("cities", [])[ci]
		_check(JSON.stringify(c_rt.get("pop", {}).get("informal", {}))
			== JSON.stringify(c_rt3.get("pop", {}).get("informal", {})),
			"round-trip: pop.informal idêntico")
		_check(JSON.stringify(c_rt.get("pop", {}).get("mortality", {}))
			== JSON.stringify(c_rt3.get("pop", {}).get("mortality", {})),
			"round-trip: pop.mortality idêntico")
		_check(JSON.stringify(c_rt.get("pop", {}).get("dissatisfaction", {}))
			== JSON.stringify(c_rt3.get("pop", {}).get("dissatisfaction", {})),
			"round-trip: pop.dissatisfaction idêntico")

		var v21: Dictionary = sim.to_dict()
		v21["version"] = 21
		for city: Dictionary in v21.get("cities", []):
			if city.get("pop", {}).has("informal"):
				city.get("pop", {}).erase("informal")
			if city.get("pop", {}).has("mortality"):
				city.get("pop", {}).erase("mortality")
			if city.get("pop", {}).has("dissatisfaction"):
				city.get("pop", {}).erase("dissatisfaction")
			var region: Dictionary = city.get("region", {})
			if region.has("informal"):
				region.erase("informal")
			if region.has("mortality"):
				region.erase("mortality")
			if region.has("dissatisfaction"):
				region.erase("dissatisfaction")
		var migrated := EconomySim.new()
		_load_chains(migrated)
		migrated.from_dict(v21)
		var after: Dictionary = migrated.to_dict()
		_check(int(after.get("version", 0)) == SAVE_VERSION,
			"v21 deve migrar para v22 (é %s)" % str(after.get("version")))
		# A migração semeia a informalidade (dado/âncora) e os contadores zerados.
		var mcity: Dictionary = after.get("cities", [])[ci]
		var minformal: Dictionary = mcity.get("pop", {}).get("informal", {})
		var mshare := float(minformal.get("share", -1.0))
		_check(mshare >= 0.0 and mshare <= 1.0,
			"v21→v22: informal_share semeado em [0,1] (%.3f)" % mshare)
		var mmort: Dictionary = mcity.get("pop", {}).get("mortality", {})
		_check(absf(float(mmort.get("excess_total", -1.0))) < 1e-9,
			"v21→v22: excess_deaths_total nasce 0")
