extends SceneTree

## C6 (D-114) — CRISE BANCÁRIA, CORRIDA E RESOLUÇÃO.
##
## Cenário: o fixture AGRIA/PORTUS (nação DEMO, moeda DEM) para a mecânica; um
## banco de USA é acrescentado por save para medir o contágio ENTRE nações.
## Todas as regras vêm de `data/definitions/economy/bank_crisis.json` (default
## de jogo) e o espelho compilado é conferido contra o arquivo.
##
##   (a) choque de 30 % na carteira do banco privado -> confiança cai ->
##       corrida -> BC redesconta -> banco SOBREVIVE (sem resolução).
##   (b) choque que torna insolvente -> bail-in + seguro + Tesouro, com os
##       custos fechando EXATAMENTE o rombo.
##   (c) contágio na mesma nação (banco público cai) e não em outra (USA fica
##       calmo).
##   (d) dolarizado/sem moeda própria: SEM emprestador de última instância
##       (sem redesconto; a corrida para no caixa disponível).
##   (e) save v34: round-trip bit-exato e continuação idêntica.
##   (f) conservação de dinheiro e identidade da C1 em TODOS os dias.
##
## Roda pelo runner: powershell -File tools/run_tests.ps1 -List c6_bank_crisis_test

const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"
const SERVICES_PATH := "res://data/definitions/economy/services.json"
const SECTOR_EMPLOYMENT_PATH := "res://data/definitions/economy/sector_employment.json"
const AGE_STRUCTURE_PATH := "res://data/definitions/population/age_structure_2026.json"
const CRISIS_PATH := "res://data/definitions/economy/bank_crisis.json"

const EPS := 1e-6
const LEVERAGE_CAP := 10.0
const PRIVATE := "DEMO/private"
const PUBLIC := "DEMO/public"
const FOREIGN := "DEMO/foreign"
const USA_BANK := "USA/private"

var _failures: Array[String] = []


func _init() -> void:
	_test_parameters_match_data()
	_test_run_lolr_survives()
	_test_resolution_costs_close_gap()
	_test_contagion_scope()
	_test_dollarized_without_lender()
	_test_save_v34()
	if _failures.is_empty():
		print("[c6-crisis] OK: (a)-(f) corrida com redesconto, resolução com "
			+ "custos fechando o rombo, contágio por nação, dolarizado sem "
			+ "emprestador, save v34 e conservação de dinheiro.")
		quit(0)
	else:
		print("[c6-crisis] FALHAS (%d):" % _failures.size())
		for failure: String in _failures:
			print(" - " + failure)
		quit(1)


# --- parâmetros: JSON x espelho compilado ---------------------------------
func _test_parameters_match_data() -> void:
	var sim = _world(0)
	var effective: Dictionary = sim.set_bank_crisis({})
	var data: Dictionary = _json(CRISIS_PATH)
	_check("P fonte declarada",
		String(data.get("source", "")) == "default de jogo",
		String(data.get("source", "")))
	var capital: Dictionary = data.get("capital", {})
	var liquidity: Dictionary = data.get("liquidity", {})
	var losses: Dictionary = data.get("losses", {})
	var confidence: Dictionary = data.get("confidence", {})
	var weights: Dictionary = confidence.get("weights", {})
	var lolr: Dictionary = data.get("lolr", {})
	var insurance: Dictionary = data.get("insurance", {})
	var resolution: Dictionary = data.get("resolution", {})
	var pairs := [
		["capital_min", capital.get("min_ratio")],
		["capital_healthy", capital.get("confidence_healthy_ratio")],
		["liquidity_min", liquidity.get("min_ratio")],
		["loss_scale", losses.get("confidence_scale")],
		["loss_ema_days", losses.get("ema_days")],
		["smoothing", confidence.get("smoothing")],
		["run_threshold", confidence.get("run_threshold")],
		["run_share", confidence.get("run_share")],
		["flight_to_quality", confidence.get("flight_to_quality")],
		["w_solvency", weights.get("solvency")],
		["w_liquidity", weights.get("liquidity")],
		["w_losses", weights.get("losses")],
		["w_contagion", weights.get("contagion")],
		["w_market", weights.get("market")],
		["lolr_spread", lolr.get("spread")],
		["lolr_haircut", lolr.get("collateral_haircut")],
		["lolr_max_assets", lolr.get("max_share_of_assets")],
		["insured_share", insurance.get("insured_share")],
		["premium_daily", insurance.get("premium_daily")],
		["fund_target_ratio", insurance.get("fund_target_ratio")],
		["capital_flight_share", resolution.get("capital_flight_share")],
		["insolvency_tolerance", resolution.get("insolvency_tolerance")],
		["insolvency_tolerance_ratio",
			resolution.get("insolvency_tolerance_ratio")],
		["treasury_debt_rescue", resolution.get("treasury_debt_rescue")],
		["takeover_public", resolution.get("takeover_public")],
	]
	for pair: Array in pairs:
		var key := String(pair[0])
		var expected: Variant = pair[1]
		var actual: Variant = effective.get(key)
		if expected is bool:
			_check("P %s = %s (JSON x núcleo)" % [key, str(expected)],
				bool(actual) == bool(expected), "nucleo=%s" % str(actual))
		else:
			_check("P %s = %s (JSON x núcleo)" % [key, str(expected)],
				absf(float(actual) - float(expected)) <= 1e-12,
				"nucleo=%s" % str(actual))


# --- (a) corrida com redesconto: o banco sobrevive ------------------------
func _test_run_lolr_survives() -> void:
	var sim = _stage(0.32, "private", true)
	var loans := float(_bank(sim, PRIVATE).get("loans", 0.0))
	_check("A banco privado tem carteira para o choque",
		loans > 1000.0, "loans=%.2f" % loans)
	var shock: Dictionary = sim.bank_shock(PRIVATE, 0.0, 0.30)
	_check("A choque de 30 % aplicado",
		float(shock.get("written", 0.0)) > 0.0, JSON.stringify(shock))
	var bank: Dictionary = _bank(sim, PRIVATE)
	_check("A banco solvente depois do choque (não é o caso de resolução)",
		float(bank.get("equity", 0.0)) >= 0.0,
		"equity=%.2f" % float(bank.get("equity", 0.0)))
	sim.tick_day()
	_check("A confiança começa a cair no dia do choque",
		float(_bank(sim, PRIVATE).get("confidence", 1.0)) <= 0.98,
		"conf=%.4f" % float(_bank(sim, PRIVATE).get("confidence", 1.0)))

	var saw_run := false
	var saw_lolr := false
	var worst := 0.0
	var resolved := false
	for day in 45:
		var summary: Dictionary = sim.tick_day()
		_check("A dia %d sem violação de invariante" % (day + 1),
			(summary.get("invariant_violations", []) as Array).is_empty(),
			str(summary.get("invariant_violations", [])))
		var row := _bank(sim, PRIVATE)
		var totals: Dictionary = sim.get_bank_status().get("totals", {})
		if float(row.get("run_withdrawn", 0.0)) > 0.0:
			saw_run = true
		if float(row.get("lolr_borrowed", 0.0)) > 0.0:
			saw_lolr = true
		worst = maxf(worst, float(totals.get("confidence_min", 1.0)))
		if bool(row.get("resolved", false)):
			resolved = true
		_check_identity(sim, "A dia %d" % (day + 1))
	var final := _bank(sim, PRIVATE)
	_check("A houve corrida (saque medido)", saw_run,
		"run_withdrawn=%.2f" % float(final.get("run_withdrawn", 0.0)))
	_check("A BC emprestou (emprestador de última instância)", saw_lolr,
		"lolr=%.2f" % float(final.get("lolr_borrowed", 0.0)))
	_check("A banco SOBREVIVEU (não foi resolvido)", not resolved,
		"resolution=%s" % String(final.get("resolution", "")))
	_check("A banco segue solvente", float(final.get("equity", 0.0)) >= -EPS,
		"equity=%.2f" % float(final.get("equity", 0.0)))
	_check("A redesconto entrou no balanço do BC (dívida do banco)",
		float(final.get("bc_borrowing", 0.0)) > 0.0,
		"bc_borrowing=%.2f" % float(final.get("bc_borrowing", 0.0)))
	print("[c6-crisis] A: loans=%.2f saque=%.2f lolr=%.2f conf_min=%.4f"
		% [loans, float(final.get("run_withdrawn", 0.0)),
		float(final.get("lolr_borrowed", 0.0)), worst])


# --- (b) insolvência: bail-in + seguro + Tesouro, custos = rombo ----------
func _test_resolution_costs_close_gap() -> void:
	var sim = _stage(0.10, "private", true)
	# Alguns dias de prêmio antes do choque: o fundo de seguro tem saldo.
	for day in 5:
		sim.tick_day()
	var fund_before := float(sim.get_bank_status().get("totals", {})
		.get("fund", 0.0))
	_check("B fundo de seguro acumulou prêmio", fund_before > 0.0,
		"fund=%.6f" % fund_before)
	var treasury_before := _treasury_cash(sim)
	# Perda de 60 %: o rombo passa do depósito NÃO segurado (40 % dos
	# depósitos), então o fundo e o Tesouro precisam entrar — o caso completo
	# da hierarquia de resolução.
	var shock: Dictionary = sim.bank_shock(PRIVATE, 0.0, 0.60)
	_check("B choque aplicado", float(shock.get("written", 0.0)) > 0.0,
		JSON.stringify(shock))
	var bank: Dictionary = _bank(sim, PRIVATE)
	_check("B banco insolvente antes da resolução",
		float(bank.get("equity", 0.0)) < 0.0,
		"equity=%.2f" % float(bank.get("equity", 0.0)))
	var summary: Dictionary = sim.tick_day()
	_check("B dia da resolução sem violação de invariante",
		(summary.get("invariant_violations", []) as Array).is_empty(),
		str(summary.get("invariant_violations", [])))
	_check_identity(sim, "B dia da resolução")
	var event: Dictionary = _last_resolution(sim, PRIVATE)
	_check("B evento de resolução registrado", not event.is_empty(),
		str(sim.get_bank_status().get("events", [])))
	if event.is_empty():
		return
	var gap := float(event.get("gap", 0.0))
	var cost_depositors := float(event.get("cost_depositors", 0.0))
	var cost_fund := float(event.get("cost_fund", 0.0))
	var cost_treasury := float(event.get("cost_treasury", 0.0))
	var cost_takeover := float(event.get("cost_takeover", 0.0))
	var cost_total := cost_depositors + cost_fund + cost_treasury \
		+ cost_takeover
	_check("B bail-in de depósito não segurado", cost_depositors > 0.0,
		"custo_depositantes=%.6f" % cost_depositors)
	_check("B fundo de seguro pagou", cost_fund > 0.0,
		"custo_fundo=%.6f" % cost_fund)
	_check("B Tesouro cobriu o resto (caixa/dívida)",
		cost_treasury > 0.0, "custo_tesouro=%.6f" % cost_treasury)
	_check("B rombo do evento positivo", gap > 0.0, "gap=%.6f" % gap)
	_check("B custos somam EXATAMENTE o rombo",
		absf(cost_total - gap) <= maxf(1e-3, absf(gap) * 1e-9),
		"soma=%.9f rombo=%.9f" % [cost_total, gap])
	var insurance: Array = sim.get_bank_status().get("insurance", [])
	var payouts := 0.0
	for row: Dictionary in insurance:
		if String(row.get("nation", "")) == "DEMO":
			payouts = float(row.get("payouts", 0.0))
	_check("B fundo pagou o mesmo que o evento registra",
		absf(payouts - cost_fund) <= 1e-6,
		"payouts=%.6f custo=%.6f" % [payouts, cost_fund])
	var treasury_after := _treasury_cash(sim)
	_check("B Tesouro tinha caixa antes do resgate", treasury_before > 0.0,
		"antes=%.2f" % treasury_before)
	print("[c6-crisis] B tesouro antes=%.2f depois=%.2f"
		% [treasury_before, treasury_after])
	var resolved := _bank(sim, PRIVATE)
	_check("B banco marcado como resolvido", bool(resolved.get("resolved", false)),
		String(resolved.get("resolution", "")))
	print("[c6-crisis] B: rombo=%.2f depositantes=%.2f fundo=%.2f tesouro=%.2f"
		% [gap, cost_depositors, cost_fund, cost_treasury])


# --- (c) contágio: mesma nação cai, outra não -----------------------------
func _test_contagion_scope() -> void:
	var setup = _stage_with_usa_bank(0.32)
	var sim = setup["sim"]
	# O banco público precisa de posição para a confiança dele ser medida (um
	# banco vazio não tem sinal). Capital em caixa e um dia de referência.
	var paid: Dictionary = sim.set_bank_capital("DEMO", 20000.0, "public")
	_check("C banco público capitalizado para medir contágio",
		float(paid.get("paid", 0.0)) > 0.0, JSON.stringify(paid))
	sim.tick_day()
	var shock: Dictionary = sim.bank_shock(PRIVATE, 0.0, 0.30)
	_check("C choque aplicado", float(shock.get("written", 0.0)) > 0.0,
		JSON.stringify(shock))
	var public_before := float(_bank(sim, PUBLIC).get("confidence", 1.0))
	var usa_before := float(_bank(sim, USA_BANK).get("confidence", 1.0))
	for day in 12:
		sim.tick_day()
	var public_after := float(_bank(sim, PUBLIC).get("confidence", 1.0))
	var usa_after := float(_bank(sim, USA_BANK).get("confidence", 1.0))
	_check("C banco da mesma nação perde confiança (contágio)",
		public_after < public_before - 1e-9,
		"antes=%.4f depois=%.4f" % [public_before, public_after])
	_check("C banco de OUTRA nação não é contaminado",
		absf(usa_after - usa_before) <= 1e-12,
		"antes=%.4f depois=%.4f" % [usa_before, usa_after])
	print("[c6-crisis] C: public %.4f -> %.4f | USA %.4f -> %.4f"
		% [public_before, public_after, usa_before, usa_after])


# --- (d) dolarizado: sem emprestador de última instância ------------------
func _test_dollarized_without_lender() -> void:
	var sim = _stage(0.32, "foreign", true)
	# Moeda sem política própria: DEMO passa a "no separate legal tender".
	sim.set_fx_regimes({
		"source": "teste C6 (dolarizado)",
		"currencies": {
			"DEM": {
				"nations": ["DEMO"],
				"shared_currency": true,
				"exceptions": {"DEMO": "no_separate_legal_tender"},
				"game_regime": "peg",
			},
		},
	})
	var cash_before := float(_bank(sim, FOREIGN).get("cash", 0.0))
	var shock: Dictionary = sim.bank_shock(FOREIGN, 0.0, 0.30)
	_check("D choque aplicado", float(shock.get("written", 0.0)) > 0.0,
		JSON.stringify(shock))
	var saw_run := false
	for day in 40:
		var summary: Dictionary = sim.tick_day()
		_check("D dia %d sem violação de invariante" % (day + 1),
			(summary.get("invariant_violations", []) as Array).is_empty(),
			str(summary.get("invariant_violations", [])))
		var row := _bank(sim, FOREIGN)
		if float(row.get("run_withdrawn", 0.0)) > 0.0:
			saw_run = true
		_check("D dia %d sem redesconto (não há emprestador)" % (day + 1),
			float(row.get("lolr_borrowed", 0.0)) <= EPS,
			"lolr=%.6f" % float(row.get("lolr_borrowed", 0.0)))
		_check_identity(sim, "D dia %d" % (day + 1))
	var row := _bank(sim, FOREIGN)
	_check("D houve corrida", saw_run,
		"run=%.2f" % float(row.get("run_withdrawn", 0.0)))
	_check("D BC não criou M0 para o banco (sem emprestador)",
		float(row.get("bc_borrowing", 0.0)) <= EPS,
		"bc_borrowing=%.6f" % float(row.get("bc_borrowing", 0.0)))
	_check("D corrida parou no caixa disponível",
		float(row.get("run_withdrawn", 0.0)) <= cash_before + EPS,
		"saque=%.2f caixa=%.2f" % [float(row.get("run_withdrawn", 0.0)),
			cash_before])
	print("[c6-crisis] D: caixa=%.2f saque=%.2f lolr=%.6f conf=%.4f run_days=%d"
		% [cash_before, float(row.get("run_withdrawn", 0.0)),
		float(row.get("lolr_borrowed", 0.0)),
		float(row.get("confidence", 1.0)), int(row.get("run_days", 0))])


# --- (e) save v34 --------------------------------------------------------
func _test_save_v34() -> void:
	var sim = _stage(0.32, "private", true)
	sim.bank_shock(PRIVATE, 0.0, 0.30)
	for day in 6:
		sim.tick_day()
	var saved: Dictionary = sim.to_dict()
	_check("E save é v34", int(saved.get("version", 0)) == 34,
		"version=%s" % str(saved.get("version")))
	var block: Dictionary = saved.get("bank_crisis", {})
	_check("E save publica o bloco bank_crisis",
		block.has("enabled") and block.has("policy") and block.has("funds"),
		str(block.keys()))
	# Sem reaplicar as definições: o save do teste já traz nomes e as
	# participações do cenário (100 % no papel-alvo), como o round-trip exige.
	var loaded = _load(saved, false)
	loaded.set_bank_credit(true)
	loaded.set_bank_policy(0.02, LEVERAGE_CAP, 2.0, 30, 0.01)
	_check("E round-trip v34 bit-exato",
		JSON.stringify(loaded.to_dict()) == JSON.stringify(saved), "difere")
	for day in 5:
		sim.tick_day()
		loaded.tick_day()
	_check("E continuação 5 d idêntica",
		JSON.stringify(sim.get_bank_status())
			== JSON.stringify(loaded.get_bank_status()),
		"status difere")
	_check("E dinheiro igual na continuação",
		absf(float(sim.get_status().get("money_total", 0.0))
			- float(loaded.get_status().get("money_total", 0.0))) <= EPS,
		"sim=%s loaded=%s" % [sim.get_status().get("money_total"),
			loaded.get_status().get("money_total")])


# --- helpers -------------------------------------------------------------
## Monta o mundo, injeta capital no papel e empresta aos grupos.
## `capital_share` é o capital em fração da carteira final do banco.
func _stage(capital_share: float, role: String, credit: bool):
	var sim = _world(3)
	# O fixture tem UM grupo; o tomador é sorteado pelo hash do id contra as
	# participações. Para o papel-alvo receber a carteira, o save do teste
	# declara 100 % para ele (não é dinheiro: só a participação do papel).
	var save: Dictionary = sim.to_dict()
	var rows: Dictionary = save.get("banks", {}).get("banks", {})
	for id: Variant in rows:
		(rows[id] as Dictionary)["share"] = 1.0 \
			if String(id) == _role_id(role) else 0.0
	save["banks"]["banks"] = rows
	sim = _load(save, false)
	# O lucro diário do grupo é medição do último dia e não vai ao save: um
	# tick depois da carga o limite de crédito volta a existir.
	sim.tick_day()
	var groups: Array = sim.get_groups()
	# 1. Empréstimo: os pedidos vão pela fila A3 e entram no tick seguinte.
	if credit:
		sim.set_bank_credit(true)
		# amortização lenta: a carteira do teste fica estável entre os dias
		sim.set_bank_policy(0.02, LEVERAGE_CAP, 2.0, 30, 0.01)
		for group: Dictionary in groups:
			sim.group_borrow(String(group.get("id", "")), 200000.0)
		sim.tick_day()
	# 2. Capital do papel-alvo (caixa pago pelo dono) e um dia para a
	# referência de confiança do depositante acompanhar o reforço.
	var book := _book(sim, _role_id(role))
	if book <= 0.0:
		return sim
	var capital := capital_share * book
	var paid: Dictionary = sim.set_bank_capital("DEMO", capital, role)
	_check("S capital do papel %s pago" % role,
		float(paid.get("paid", 0.0)) > 0.0, JSON.stringify(paid))
	sim.tick_day()
	return sim


func _book(sim, id: String) -> float:

	var row := _bank(sim, id)
	return float(row.get("loans", 0.0)) + float(row.get("arrears", 0.0))


func _role_id(role: String) -> String:
	return "DEMO/" + role


func _stage_with_usa_bank(capital_share: float):
	var sim = _stage(capital_share, "private", true)
	# Banco de USA sem posição: zero é equilíbrio (nada a socorrer), só serve
	# para medir se o contágio atravessa a fronteira.
	var save: Dictionary = sim.to_dict()
	var rows: Dictionary = save.get("banks", {}).get("banks", {})
	rows[USA_BANK] = {
		"id": USA_BANK, "nation": "USA", "role": "private",
		"name": "", "name_status": "no_data", "owner_kind": "national",
		"owner_nation": "", "share": 1.0, "currency": "USD",
		"loans": 0.0, "arrears": 0.0, "deposits": 0.0,
		"bc_borrowing": 0.0, "cash": 0.0, "capital": 0.0,
		"interest_income": 0.0, "interest_expense": 0.0,
		"writeoffs": 0.0, "dividends": 0.0, "remittance": 0.0,
		"remittance_origin": 0.0, "harvest_interest": 0.0,
		"defaults": 0, "loans_count": 0, "last_default_day": -1,
		"last_dividend_day": -1,
	}
	save["banks"]["banks"] = rows
	var loaded = _load(save, false)
	return {"sim": loaded}


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
		"currencies": [
			{
				"code": "DEM", "name": "Demo", "symbol": "D",
				"nations": ["DEMO"], "rate_per_anchor": 1.0, "rate": 1.0,
				"base_rate": 1.0,
			},
			{
				"code": "USD", "name": "Dollar", "symbol": "$",
				"nations": ["USA"], "rate_per_anchor": 2.0, "rate": 2.0,
				"base_rate": 2.0,
			},
		],
	})
	sim.initialize_scenario()
	sim.set_bank_definitions(_definition_foreign())
	for day in days:
		sim.tick_day()
	return sim


func _load(data: Dictionary, definitions: bool = true):
	var sim = EconomySim.new()
	sim.set_goods(_json(GOODS_PATH))
	sim.set_recipes(_json(RECIPES_PATH))
	sim.set_needs(_json(NEEDS_PATH))
	sim.set_services(_json(SERVICES_PATH))
	sim.set_sector_employment(_json(SECTOR_EMPLOYMENT_PATH))
	sim.set_age_structure(_json(AGE_STRUCTURE_PATH))
	if definitions:
		sim.set_bank_definitions(_definition_foreign())
	sim.from_dict(data)
	return sim


func _definition_foreign() -> Dictionary:
	return {
		"nations": {
			"DEMO": {
				"public": {"status": "present", "name": "Banco Publico de Teste"},
				"private": {"status": "present", "name": "Banco Privado de Teste"},
				"foreign": {"status": "present",
					"branch_candidate": "Banco Estrangeiro de Teste",
					"origin_iso3": "USA"},
			},
		},
	}


func _bank(sim, id: String) -> Dictionary:
	for row: Dictionary in sim.get_bank_status().get("banks", []):
		if String(row.get("id", "")) == id:
			return row
	return {}


func _last_resolution(sim, id: String) -> Dictionary:
	var events: Array = sim.get_bank_status().get("events", [])
	for index in range(events.size() - 1, -1, -1):
		var event: Dictionary = events[index]
		if String(event.get("kind", "")) == "resolution" \
				and String(event.get("bank", "")) == id:
			return event
	return {}


func _treasury_cash(sim) -> float:

	var total := 0.0
	for city: Dictionary in sim.get_status().get("cities", []):
		if String(city.get("nation", "")) == "DEMO":
			total += float(city.get("government_budget", 0.0))
	return total


func _check_identity(sim, label: String) -> void:
	var invariant: Dictionary = sim.get_balance_status().get("invariant", {})
	_check("%s identidade da C1 fecha" % label,
		bool(invariant.get("ok", false)), str(invariant))


func _check(name: String, ok: bool, detail: String) -> void:
	if not ok:
		_failures.append("%s (%s)" % [name, detail])


func _json(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return {}
	return JSON.parse_string(file.get_as_text())
