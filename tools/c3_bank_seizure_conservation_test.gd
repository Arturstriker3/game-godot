extends SceneTree

const SaveSchema := preload("res://core/economy/save_schema.gd")

## Regressao t153/F0 -- CONSERVACAO EXATA NA EXECUCAO DO CALOTE (C3/C4).
##
## A fase de credito reporta ao invariante o dinheiro BANCARIO que criou ou
## destruiu no dia (juro pago e amortizacao destroem; dividendo cria). A
## execucao do calote apenas move o caixa do grupo (deposito) para o caixa do
## banco: os dois ja estao em `money_total`, entao nao ha dinheiro criado nem
## destruido. O bug (t153) contava a queda de `bank.deposits` da tomada como
## destruicao e travava o dia com "dinheiro nao conservado".
##
## Cenario: fixture AGRIA/PORTUS (DEMO/DEM). O grupo empresta ate o limite e o
## caixa e reduzido (transferido ao Tesouro, conservado) para ficar insolvente;
## resta caixa suficiente para o juro de dois dias, de modo que a EXECUCAO
## ainda tenha dinheiro a tomar (`seized > 0`) -- o caso que expoe o bug.
##
## Roda pelo runner: powershell -File tools/run_tests.ps1
##   -List c3_bank_seizure_conservation_test

const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"
const SERVICES_PATH := "res://data/definitions/economy/services.json"
const SECTOR_EMPLOYMENT_PATH := "res://data/definitions/economy/sector_employment.json"
const AGE_STRUCTURE_PATH := "res://data/definitions/population/age_structure_2026.json"

const EPS := 1e-6
const LEVERAGE_CAP := 10.0
const POLICY_RATE := 0.02
const DEFAULT_DAYS := 2

var _failures: Array[String] = []


func _init() -> void:
	_test_seizure_conserves_money()
	if _failures.is_empty():
		print("[c3-seizure] OK: a execucao do calote (deposito -> caixa do "
			+ "banco) nao cria nem destroi dinheiro; invariante fecha todo dia.")
		quit(0)
	else:
		print("[c3-seizure] FALHAS (%d):" % _failures.size())
		for failure: String in _failures:
			print(" - " + failure)
		quit(1)


func _test_seizure_conserves_money() -> void:
	var sim = _world(1)
	sim.set_bank_credit(true)
	sim.set_bank_policy(POLICY_RATE, LEVERAGE_CAP, 2.0, 30, 0.5)
	sim.set_bank_capital("DEMO", 1.0e6)
	var group_id := _first_group(sim)
	var borrow: Dictionary = sim.group_borrow(group_id, 1.0e9)
	_check("emprestimo concedido", float(borrow.get("granted", 0.0)) > 0.0,
		JSON.stringify(borrow))
	var debt := float(_bank_group(sim, group_id).get("debt", 0.0))
	var rate := _loan_rate(sim)
	# Caixa que cobre o juro de DEFAULT_DAYS dias e ainda sobra para a tomada.
	var kept := debt * rate / 365.0 * (DEFAULT_DAYS + 1.0) + 10.0
	var data: Dictionary = sim.to_dict()
	var moved := 0.0
	for entry: Dictionary in data.get("groups", []):
		if String(entry.get("id", "")) == group_id:
			moved = maxf(0.0, float(entry.get("cash", 0.0)) - kept)
			entry["cash"] = float(entry.get("cash", 0.0)) - moved
	var cities: Array = data.get("cities", [])
	_check("save tem cidades para o Tesouro", not cities.is_empty(),
		"cities=%d" % cities.size())
	if not cities.is_empty():
		var city: Dictionary = cities[0]
		city["government_budget"] = float(city.get("government_budget", 0.0)) \
			+ moved
	var loaded = _load(data)
	loaded.set_bank_policy(POLICY_RATE, LEVERAGE_CAP, 2.0, DEFAULT_DAYS, 0.5)
	var invariants := 0
	var defaulted := false
	var cash_at_default := 0.0
	var defaults_before := int(_bank_totals(loaded).get("defaults", 0))
	for day in 8:
		var cash_before := float(_bank_group(loaded, group_id).get("cash", 0.0))
		var summary: Dictionary = loaded.tick_day()
		var violations: Array = summary.get("invariant_violations", [])
		invariants += violations.size()
		for violation: Variant in violations:
			print("[c3-seizure] dia %s: %s" % [str(day + 1), str(violation)])
		var now := _bank_totals(loaded)
		if int(now.get("defaults", 0)) > defaults_before:
			defaulted = true
			cash_at_default = cash_before
			break
	_check("calote aconteceu", defaulted,
		"defaults=%s" % str(_bank_totals(loaded).get("defaults", 0)))
	_check("execucao do calote tomou caixa (seized > 0)",
		cash_at_default > EPS, "caixa no dia do calote=%.6f" % cash_at_default)
	_check("nenhuma violacao do invariante no calote", invariants == 0,
		"violacoes=%d" % invariants)
	var group := _bank_group(loaded, group_id)
	_check("grupo com divida zerada", absf(float(group.get("debt", -1.0))) <= EPS,
		"debt=" + str(group.get("debt")))
	_check("grupo com o calote registrado",
		int(group.get("defaults", 0)) == 1, "defaults=" + str(group.get("defaults")))
	# Um tick depois o mundo segue integro (a reversao do bug travava o relogio).
	var summary2: Dictionary = loaded.tick_day()
	var violations2: Array = summary2.get("invariant_violations", [])
	_check("dia seguinte tambem integro", violations2.is_empty(),
		"violacoes=%d" % violations2.size())
	var money_end := float(loaded.get_status().get("money_total", 0.0))
	_check("money_total finito", is_finite(money_end) and money_end > 0.0,
		"money=%.6f" % money_end)


# --- helpers --------------------------------------------------------------
func _world(days: int):
	var sim = EconomySim.new()
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
	for day in days:
		sim.tick_day()
	return sim


func _load(data: Dictionary):
	var sim = EconomySim.new()
	sim.set_goods(_json(GOODS_PATH))
	sim.set_recipes(_json(RECIPES_PATH))
	sim.set_needs(_json(NEEDS_PATH))
	sim.set_services(_json(SERVICES_PATH))
	sim.set_sector_employment(_json(SECTOR_EMPLOYMENT_PATH))
	sim.set_age_structure(_json(AGE_STRUCTURE_PATH))
	sim.from_dict(data)
	return sim


func _first_group(sim) -> String:
	return String((sim.get_groups()[0] as Dictionary).get("id", ""))


func _bank_totals(sim) -> Dictionary:
	return sim.get_bank_status().get("totals", {})


func _bank_group(sim, group_id: String) -> Dictionary:
	for row: Dictionary in sim.get_bank_status().get("groups", []):
		if String(row.get("id", "")) == group_id:
			return row
	return {}


func _loan_rate(sim) -> float:
	var nations: Array = sim.get_bank_status().get("nations", [])
	if nations.is_empty():
		return POLICY_RATE
	return float((nations[0] as Dictionary).get("loan_rate", POLICY_RATE))


func _check(name: String, ok: bool, detail: String) -> void:
	if not ok:
		_failures.append("%s (%s)" % [name, detail])


func _json(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	return JSON.parse_string(file.get_as_text())
