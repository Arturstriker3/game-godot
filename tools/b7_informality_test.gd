extends SceneTree

## B7 — INFORMALIDADE, SERVIÇOS INFORMAIS, MORTALIDADE E INSATISFAÇÃO + save v24.
##
## Valida (1) a conservação de dinheiro e pessoas com a informalidade ativa,
## (2) o informal sem imposto de renda/contribuição, (3) o serviço informal
## absorvendo demanda — inclusive no INTERIOR com poupança insuficiente (a
## receita creditada é o débito efetivado), (4) a mortalidade subindo com a
## fome e concentrando o excesso em 0–14/65+, (5) a insatisfação reagindo
## dentro de [0,1] (com a normalização por bloco no interior) e (6) o
## round-trip do save v24 + migração v21.
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

const SAVE_VERSION := 25
const TAX_RATE := 0.15

var _failures: Array = []


func _init() -> void:
	_run()
	if _failures.is_empty():
		print("[b7] OK: informalidade, serviços informais, mortalidade, "
			+ "insatisfação e save v24.")
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
		"save deve ser v24 (é %s)" % str(save.get("version")))
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

	# --- (3b) Serviço informal do INTERIOR com poupança INSUFICIENTE: a receita
	# creditada aos quintis é EXATAMENTE o débito efetivado (não cria dinheiro).
	# Cenário: interior com informalidade total, renda diária muito acima do
	# caixa e poupança pequena — a compra tem de caber no que o quintil paga.
	if true:
		var save_poor: Dictionary = sim.to_dict()
		var cities_poor: Array = save_poor.get("cities", [])
		var city_poor: Dictionary = cities_poor[ci]
		var region_poor: Dictionary = city_poor.get("region", {})
		if region_poor.is_empty():
			_fail("cidade sem bloco region para o teste de poupança do interior")
		else:
			var region_before: Dictionary = sim.get_region_status(city_id)
			var food_spend_prev := float(region_before.get("food_spend", 0.0))
			_set_informal_all(city_poor, 1.0)
			# Poupança pequena, com folga sobre a comida do dia (10× o gasto de
			# ontem, piso 2.000); `cash` acompanha a soma dos quintis.
			var savings_total := maxf(2000.0, food_spend_prev * 10.0)
			var each := savings_total / 5.0
			var strata_poor: Dictionary = region_poor.get("strata", {})
			strata_poor["savings"] = [each, each, each, each, each]
			region_poor["strata"] = strata_poor
			region_poor["cash"] = savings_total
			# Renda absurda → demanda informal muito acima da poupança.
			region_poor["income"] = 1.0e9
			region_poor["wage_income"] = 1.0e9
			city_poor["region"] = region_poor
			cities_poor[ci] = city_poor
			save_poor["cities"] = cities_poor
			# Dia seguinte sem fecho semanal/mensal desta cidade (offset 1).
			save_poor["day"] = ci
			var sim_poor := EconomySim.new()
			_load_chains(sim_poor)
			sim_poor.from_dict(save_poor)
			var money_before := float(sim_poor.get_status().get("money_total", -1.0))
			sim_poor.tick_day()
			var poor_violations: Array = sim_poor.get_day_summary().get(
				"invariant_violations", [])
			var money_after := float(sim_poor.get_status().get("money_total", -1.0))
			var poor_service: Dictionary = sim_poor.get_region_status(city_id).get(
				"informal", {}).get("service", {})
			var poor_revenue := float(poor_service.get("revenue", -1.0))
			var poor_demand := float(poor_service.get("demand", 0.0))
			var poor_price := float(poor_service.get("price", 0.0))
			_check(poor_revenue >= 0.0,
				"interior: receita do serviço informal >= 0 (%.2f)" % poor_revenue)
			_check(poor_revenue <= savings_total + 1e-6,
				"interior: receita %.2f não pode passar da poupança %.2f (criaria dinheiro)"
				% [poor_revenue, savings_total])
			_check(poor_revenue > 0.0,
				"interior: cenário deve realizar serviço informal (receita > 0)")
			_check(poor_demand * poor_price > savings_total,
				"interior: cenário com demanda %.2f acima da poupança %.2f"
				% [poor_demand * poor_price, savings_total])
			_check(poor_violations.is_empty(),
				"interior: invariantes limpos no tick do serviço informal (violações: %s)"
				% str(poor_violations))
			var poor_drift := absf(money_after - money_before)
			_check(poor_drift <= maxf(1e-3, absf(money_before) * 1e-12),
				"interior: dinheiro conservado com poupança insuficiente (drift %.6f)"
				% poor_drift)

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

	# --- (4b) Perfil por FAIXA: o excesso de mortes concentra em 0–14 e 65+.
	# Dois mundos idênticos (mesmo save): CONTROLE e FOME (satisfaction_prev=0).
	# A diferença por faixa isola a mortalidade do excesso (nascimento e
	# envelhecimento são idênticos nos dois) e o perfil de vulnerabilidade da B7
	# pesa crianças e idosos — a taxa relativa de excesso é maior nesses grupos.
	if true:
		var base_bands: Dictionary = sim.to_dict()
		# Fecha o mês de `ci` no próximo tick (ver seção (4)).
		base_bands["day"] = ci + 29
		var control := EconomySim.new()
		_load_chains(control)
		control.from_dict(base_bands)
		var bands_before: Array = control.get_city_status(city_id).get(
			"pop", {}).get("age_bands", [])
		control.tick_day()
		var bands_control: Array = control.get_city_status(city_id).get(
			"pop", {}).get("age_bands", [])
		var save_bands: Dictionary = base_bands.duplicate(true)
		var cities_bands: Array = save_bands.get("cities", [])
		var city_bands: Dictionary = cities_bands[ci]
		var pop_bands: Dictionary = city_bands.get("pop", {})
		var mort_bands: Dictionary = pop_bands.get("mortality", {})
		mort_bands["satisfaction_prev"] = 0.0
		pop_bands["mortality"] = mort_bands
		city_bands["pop"] = pop_bands
		cities_bands[ci] = city_bands
		save_bands["cities"] = cities_bands
		var famine_bands := EconomySim.new()
		_load_chains(famine_bands)
		famine_bands.from_dict(save_bands)
		famine_bands.tick_day()
		var bands_famine: Array = famine_bands.get_city_status(city_id).get(
			"pop", {}).get("age_bands", [])
		if bands_before.size() != 5 or bands_control.size() != 5 \
				or bands_famine.size() != 5:
			_fail("faixas: age_bands deve ter 5 faixas (antes=%d controle=%d fome=%d)"
				% [bands_before.size(), bands_control.size(), bands_famine.size()])
		else:
			var excess: Array = []
			var deaths_control: Array = []
			var total_excess := 0.0
			for band in 5:
				var deaths_c := maxf(float(bands_before[band])
					- float(bands_control[band]), 0.0)
				var deaths_f := maxf(float(bands_before[band])
					- float(bands_famine[band]), 0.0)
				var extra := deaths_f - deaths_c
				excess.append(extra)
				deaths_control.append(deaths_c)
				total_excess += maxf(extra, 0.0)
			print("[b7] faixas: antes=%s excesso=%s mortes_controle=%s total=%.6f"
				% [str(bands_before), str(excess), str(deaths_control), total_excess])
			var young := float(excess[0])
			var old := float(excess[4])
			_check(young > 0.0 and old > 0.0,
				"faixas: excesso de mortes deve aparecer em 0–14 e 65+ (%.4f / %.4f)"
				% [young, old])
			# Excesso por mil pessoas de cada faixa: mede a vulnerabilidade sem
			# depender do tamanho da coorte (o envelhecimento, que contamina o
			# total da faixa, é idêntico nos dois mundos e sai na diferença).
			var population := 0.0
			for band in 5:
				population += maxf(float(bands_before[band]), 0.0)
			var per_thousand: Array = []
			for band in 5:
				per_thousand.append(
					maxf(float(excess[band]), 0.0) * 1000.0
						/ float(bands_before[band])
					if float(bands_before[band]) > 1e-9 else 0.0)
			if total_excess > 1e-9:
				var share := (young + old) / total_excess
				var vulnerable_pop := 0.0
				if population > 1e-9:
					vulnerable_pop = (maxf(float(bands_before[0]), 0.0)
						+ maxf(float(bands_before[4]), 0.0)) / population
				_check(share >= 0.50 and share >= vulnerable_pop * 1.15,
					"faixas: 0–14 + 65+ concentram o excesso (%.1f%% do excesso vs %.1f%% da população)"
					% [share * 100.0, vulnerable_pop * 100.0])
			_check(float(per_thousand[0]) > float(per_thousand[1])
				and float(per_thousand[0]) > float(per_thousand[2]),
				"faixas: 0–14 perde mais por mil que 15–24/25–44 (%.3f vs %.3f/%.3f)"
				% [per_thousand[0], per_thousand[1], per_thousand[2]])
			_check(float(per_thousand[4]) > float(per_thousand[1])
				and float(per_thousand[4]) > float(per_thousand[2]),
				"faixas: 65+ perde mais por mil que 15–24/25–44 (%.3f vs %.3f/%.3f)"
				% [per_thousand[4], per_thousand[1], per_thousand[2]])

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

	# --- (5b) Insatisfação da REGIÃO: normalização por pesos APLICÁVEIS. ----
	# O interior só aplica comida+inflação+informalidade (peso 0,50): o índice
	# deve dividir por 0,50, não pelo total 1,00 (o que capava o interior em
	# 0,50). Com informalidade=1 e inflação=1, as componentes ficam
	# 0,10/0,50=0,20 e 0,15/0,50=0,30 — e os componentes somam o índice.
	if true:
		var save_reg: Dictionary = sim.to_dict()
		var c_reg: Dictionary = save_reg.get("cities", [])[ci]
		_set_informal_all(c_reg, 1.0)
		var reg: Dictionary = c_reg.get("region", {})
		if reg.is_empty():
			_fail("cidade sem bloco region para testar a insatisfação do interior")
		else:
			var mort: Dictionary = reg.get("mortality", {})
			mort["food_inflation_month"] = 1.0
			reg["mortality"] = mort
			# Dia não-mensal para `ci` (tick → ci+1): formalização e inflação
			# mensais não reescrevem o que forçamos aqui.
			save_reg["day"] = ci
			var sim_reg := EconomySim.new()
			_load_chains(sim_reg)
			sim_reg.from_dict(save_reg)
			sim_reg.tick_day()
			var rdiss: Dictionary = sim_reg.get_region_status(city_id).get(
				"dissatisfaction", {})
			var rcomp: Dictionary = rdiss.get("components", {})
			var r_inf := float(rcomp.get("informality", -1.0))
			var r_infl := float(rcomp.get("inflation", -1.0))
			var r_food := float(rcomp.get("food", -1.0))
			var r_idx := float(rdiss.get("index", -1.0))
			_check(absf(r_inf - 0.20) < 0.02,
				"região: componente informalidade ~0,20 (0,10/0,50) — medido %.3f" % r_inf)
			_check(absf(r_infl - 0.30) < 0.02,
				"região: componente inflação ~0,30 (0,15/0,50) — medido %.3f" % r_infl)
			_check(absf(r_idx - (r_food + r_infl + r_inf)) < 1e-6,
				"região: componentes somam o índice (%.4f vs %.4f)"
				% [r_food + r_infl + r_inf, r_idx])

	# --- (6) Round-trip v24 (blocos B7 preservados) e migração v21 → v24. ---
	if true:
		var rt: Dictionary = sim.to_dict()
		var rt2 := EconomySim.new()
		_load_chains(rt2)
		rt2.from_dict(rt)
		var rt3: Dictionary = rt2.to_dict()
		_check(int(rt3.get("version", 0)) == SAVE_VERSION, "round-trip mantém v24")
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
			"v21 deve migrar para v24 (é %s)" % str(after.get("version")))
		# A migração semeia a informalidade (dado/âncora) e os contadores zerados.
		var mcity: Dictionary = after.get("cities", [])[ci]
		var minformal: Dictionary = mcity.get("pop", {}).get("informal", {})
		var mshare := float(minformal.get("share", -1.0))
		_check(mshare >= 0.0 and mshare <= 1.0,
			"v21→v24: informal_share semeado em [0,1] (%.3f)" % mshare)
		var mmort: Dictionary = mcity.get("pop", {}).get("mortality", {})
		_check(absf(float(mmort.get("excess_total", -1.0))) < 1e-9,
			"v21→v24: excess_deaths_total nasce 0")
