extends SceneTree

const SaveSchema := preload("res://core/economy/save_schema.gd")

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
##   (g) banco insolvente em nação dolarizada com o Tesouro sem caixa: o
##       resgate vira título em poder do exterior (`external_bonds`), os
##       custos fecham o rombo, a identidade passa e o mundo avança; o título
##       externo sobrevive ao save e à carga (R4).
##   R1. após a incorporação pelo banco público, `bank::totals` conta cada
##       acumulador UMA vez (total = soma das linhas = custo do evento).
##   R3. save no meio do dia, com perda pendente (`loss_today` ≠ 0): o valor
##       volta na carga e a continuação é idêntica.
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
	_test_rescue_external_bond()
	_test_totals_once_after_takeover()
	_test_save_pending_loss()
	_test_save_v34()
	if _failures.is_empty():
		print("[c6-crisis] OK: (a)-(g) corrida com redesconto, resolução com "
			+ "custos fechando o rombo, contágio por nação, dolarizado sem "
			+ "emprestador, resgate sem BC em título do exterior, totais da "
			+ "incorporação contados uma vez, save v34 com perda pendente e "
			+ "com título externo, e conservação de dinheiro.")
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
		["enabled", data.get("enabled")],
		["capital_min", capital.get("min_ratio")],
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


# --- (g) resgate sem BC e com o Tesouro sem caixa -------------------------
# O rombo que sobra vira dívida pública em poder do EXTERIOR (`external_bonds`,
# paga pela posição externa). Sem a ponta do credor, `financial_sum` ficaria em
# −valor e o tick seria revertido todo dia (mundo congelado).
func _test_rescue_external_bond() -> void:
	var sim = _stage(0.10, "private", true)
	# R2c: o resgate por dívida pública é DESLIGADO por default (o
	# `bank_crisis.json` grava false); este cenário é justamente o do caminho
	# de dívida (dolarizado sem BC), então a alavanca é ligada explícita.
	sim.set_bank_crisis({"treasury_debt_rescue": true})
	# Alguns dias de prêmio antes do choque: o fundo de seguro tem saldo.
	for day in 5:
		sim.tick_day()
	# Tesouro sem caixa: o orçamento público das cidades DEMO vai a zero.
	sim = _drain_treasury(sim)
	sim.set_bank_policy(0.02, LEVERAGE_CAP, 2.0, 30, 0.01)
	# Dolarizado: sem BC para carregar o título do resgate.
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
	var treasury_before := _treasury_cash(sim)
	_check("G Tesouro sem caixa antes do resgate",
		treasury_before <= EPS, "tesouro=%.6f" % treasury_before)
	var debt_before := _government_debt(sim)
	# Perda de 60 %: o rombo passa do depósito NÃO segurado e do fundo, e o
	# Tesouro (sem caixa e sem BC) tem de emitir dívida carregada pelo exterior.
	var shock: Dictionary = sim.bank_shock(PRIVATE, 0.0, 0.60)
	_check("G choque aplicado", float(shock.get("written", 0.0)) > 0.0,
		JSON.stringify(shock))
	var day_before := int(sim.get_status().get("day", 0))
	var summary: Dictionary = sim.tick_day()
	_check("G dia da resolução sem violação de invariante",
		(summary.get("invariant_violations", []) as Array).is_empty(),
		str(summary.get("invariant_violations", [])))
	_check("G mundo avançou (o tick não foi revertido)",
		int(sim.get_status().get("day", 0)) == day_before + 1,
		"antes=%d depois=%d" % [day_before,
			int(sim.get_status().get("day", 0))])
	_check_identity(sim, "G dia da resolução")
	var event: Dictionary = _last_resolution(sim, PRIVATE)
	_check("G evento de resolução registrado", not event.is_empty(),
		str(sim.get_bank_status().get("events", [])))
	if event.is_empty():
		return
	var gap := float(event.get("gap", 0.0))
	var cost_total := float(event.get("cost_depositors", 0.0)) \
		+ float(event.get("cost_fund", 0.0)) \
		+ float(event.get("cost_treasury", 0.0)) \
		+ float(event.get("cost_takeover", 0.0))
	_check("G Tesouro sem caixa socorreu por dívida emitida",
		float(event.get("cost_treasury", 0.0)) > 0.0,
		"custo_tesouro=%.6f" % float(event.get("cost_treasury", 0.0)))
	# O caixa do Tesouro não pagou: a dívida pública (gov_bonds) cresceu no
	# resgate. Sem BC, esse título só pode ficar com o exterior.
	var issued := _government_debt(sim) - debt_before
	_check("G dívida pública emitida no resgate", issued > 0.0,
		"divida_emitida=%.6f" % issued)
	_check("G dívida emitida cabe no custo do Tesouro",
		issued <= float(event.get("cost_treasury", 0.0)) + 1e-6,
		"emitida=%.6f custo=%.6f" % [issued,
			float(event.get("cost_treasury", 0.0))])
	_check("G nenhuma emissão de M0 no ramo sem BC",
		absf(float(event.get("emission", 0.0))) <= EPS,
		"emissao=%.6f" % float(event.get("emission", 0.0)))
	_check("G custos somam EXATAMENTE o rombo",
		absf(cost_total - gap) <= maxf(1e-3, absf(gap) * 1e-9),
		"soma=%.9f rombo=%.9f" % [cost_total, gap])
	_check("G banco resolvido", bool(_bank(sim, PRIVATE)
		.get("resolved", false)), String(_bank(sim, PRIVATE)
		.get("resolution", "")))
	# Depois da resolução: o invariante continua fechando e o relógio anda —
	# era aqui que o mundo congelava com o rombo sem credor.
	for day in 5:
		var after: Dictionary = sim.tick_day()
		_check("G dia %d após o resgate sem violação" % (day + 1),
			(after.get("invariant_violations", []) as Array).is_empty(),
			str(after.get("invariant_violations", [])))
		_check_identity(sim, "G dia %d após o resgate" % (day + 1))
	_check("G mundo seguiu depois do resgate",
		int(sim.get_status().get("day", 0)) >= day_before + 6,
		"dia=%d" % int(sim.get_status().get("day", 0)))
	# R4: o título em poder do exterior (`external_bonds`, no bloco
	# `monetary` do save) tem de sobreviver ao round-trip — é a ponta do
	# credor que mantém a identidade financeira fechada depois da carga.
	var saved: Dictionary = sim.to_dict()
	var monetary: Dictionary = saved.get("monetary", {})
	var external: Dictionary = monetary.get("external_bonds", {})
	var external_value := float(external.get("DEMO", 0.0))
	_check("R4 save tem título externo (external_bonds ≠ 0)",
		external_value > 0.0, "external_bonds=%s" % str(external))
	var loaded = _load(saved, false)
	loaded.set_bank_credit(true)
	loaded.set_bank_policy(0.02, LEVERAGE_CAP, 2.0, 30, 0.01)
	_check("R4 round-trip bit-exato com título externo",
		JSON.stringify(loaded.to_dict()) == JSON.stringify(saved), "difere")
	_check("R4 external_bonds preservado na carga",
		absf(float(loaded.to_dict().get("monetary", {})
			.get("external_bonds", {}).get("DEMO", 0.0))
			- external_value) <= 1e-12, "difere")
	_check_identity(loaded, "R4 após a carga")
	print("[c6-crisis] R4: external_bonds=%.2f" % external_value)
	for day in 5:
		var original: Dictionary = sim.tick_day()
		var after: Dictionary = loaded.tick_day()
		_check("R4 dia %d após a carga sem violação (original)" % (day + 1),
			(original.get("invariant_violations", []) as Array).is_empty(),
			str(original.get("invariant_violations", [])))
		_check("R4 dia %d após a carga sem violação (carregado)" % (day + 1),
			(after.get("invariant_violations", []) as Array).is_empty(),
			str(after.get("invariant_violations", [])))
		_check_identity(loaded, "R4 dia %d carregado" % (day + 1))
	_check("R4 continuação com o título externo idêntica",
		JSON.stringify(sim.get_bank_status())
			== JSON.stringify(loaded.get_bank_status()), "status difere")
	_check("R4 dinheiro igual na continuação",
		absf(float(sim.get_status().get("money_total", 0.0))
			- float(loaded.get_status().get("money_total", 0.0))) <= EPS,
		"sim=%s loaded=%s" % [sim.get_status().get("money_total"),
			loaded.get_status().get("money_total")])
	print("[c6-crisis] G: rombo=%.2f depositantes=%.2f fundo=%.2f tesouro=%.2f"
		% [gap, float(event.get("cost_depositors", 0.0)),
		float(event.get("cost_fund", 0.0)),
		float(event.get("cost_treasury", 0.0))])


# --- R1: totais após a incorporação contam cada acumulador 1× --------------
# A casca absorvida sai do `bank::totals` com a carteira zerada, mas os
# acumuladores de MEDIÇÃO que não foram copiados ao banco público (saques,
# redesconto, seguro, bail-in, Tesouro e fuga) continuam nela e têm de entrar
# UMA vez — e só uma: o total de cada um é comparado com o custo do evento.
func _test_totals_once_after_takeover() -> void:
	var sim = _stage(0.10, "private", true)
	for day in 5:
		sim.tick_day()
	# Banco público capitalizado ANTES de zerar o Tesouro: o capital sai do
	# orçamento das cidades e a incorporação precisa de um banco de pé.
	var capitalized: Dictionary = sim.set_bank_capital("DEMO", 20000.0,
		"public")
	_check("R1 banco público capitalizado para absorver",
		float(capitalized.get("paid", 0.0)) > 0.0, JSON.stringify(capitalized))
	# Tesouro sem caixa e sem resgate por dívida: o que não couber no caixa que
	# aparecer no dia vai para a incorporação pelo banco público.
	sim = _drain_treasury(sim)
	sim.set_bank_policy(0.02, LEVERAGE_CAP, 2.0, 30, 0.01)
	var policy: Dictionary = sim.set_bank_crisis({
		"treasury_debt_rescue": false,
		"takeover_public": true,
	})
	_check("R1 resgate por dívida desligado (residual vai à incorporação)",
		not bool(policy.get("treasury_debt_rescue", true)),
		JSON.stringify(policy))
	var treasury_before := _treasury_cash(sim)
	_check("R1 Tesouro sem caixa antes do resgate",
		treasury_before <= EPS, "tesouro=%.6f" % treasury_before)
	var shock: Dictionary = sim.bank_shock(PRIVATE, 0.0, 0.60)
	_check("R1 choque aplicado", float(shock.get("written", 0.0)) > 0.0,
		JSON.stringify(shock))
	var summary: Dictionary = sim.tick_day()
	_check("R1 dia da incorporação sem violação de invariante",
		(summary.get("invariant_violations", []) as Array).is_empty(),
		str(summary.get("invariant_violations", [])))
	_check_identity(sim, "R1 dia da incorporação")
	var event: Dictionary = _last_resolution(sim, PRIVATE)
	_check("R1 evento de resolução registrado", not event.is_empty(),
		str(sim.get_bank_status().get("events", [])))
	if event.is_empty():
		return
	var shell := _bank(sim, PRIVATE)
	var status: Dictionary = sim.get_bank_status()
	var totals: Dictionary = status.get("totals", {})
	_check("R1 casca absorvida pelo banco público",
		String(shell.get("absorbed_into", "")) == PUBLIC,
		"absorbed_into=%s" % String(shell.get("absorbed_into", "")))
	_check("R1 residual virou custo de incorporação",
		float(event.get("cost_takeover", 0.0)) > 0.0,
		"custo_incorporacao=%.6f" % float(event.get("cost_takeover", 0.0)))
	_check("R1 casca sem carteira (saldos zerados na incorporação)",
		absf(float(shell.get("loans", 0.0))) <= EPS
			and absf(float(shell.get("deposits", 0.0))) <= EPS
			and absf(float(shell.get("cash", 0.0))) <= EPS,
		"loans=%.6f deposits=%.6f cash=%.6f"
			% [float(shell.get("loans", 0.0)),
				float(shell.get("deposits", 0.0)),
				float(shell.get("cash", 0.0))])
	var cost_total := float(event.get("cost_depositors", 0.0)) \
		+ float(event.get("cost_fund", 0.0)) \
		+ float(event.get("cost_treasury", 0.0)) \
		+ float(event.get("cost_takeover", 0.0))
	_check("R1 custos somam EXATAMENTE o rombo",
		absf(cost_total - float(event.get("gap", 0.0)))
			<= maxf(1e-3, absf(float(event.get("gap", 0.0))) * 1e-9),
		"soma=%.9f rombo=%.9f" % [cost_total, float(event.get("gap", 0.0))])
	# Cada acumulador entra UMA vez no total: total = soma das linhas (casca +
	# banco público) e, para os que pagaram o rombo, = custo do evento.
	var accumulators := ["run_withdrawn", "run_cash", "run_public",
		"capital_flight", "lolr_borrowed", "lolr_interest",
		"insured_payout", "bailin_cut", "treasury_rescue"]
	for key: String in accumulators:
		var row_sum := 0.0
		for row: Dictionary in status.get("banks", []):
			row_sum += float(row.get(key, 0.0))
		_check("R1 totals.%s = soma das linhas (1×)" % key,
			absf(float(totals.get(key, 0.0)) - row_sum) <= 1e-6,
			"totals=%.9f linhas=%.9f"
				% [float(totals.get(key, 0.0)), row_sum])
	_check("R1 bail-in no total = custo do evento (1×)",
		absf(float(totals.get("bailin_cut", 0.0))
			- float(event.get("cost_depositors", 0.0))) <= 1e-6,
		"total=%.9f evento=%.9f" % [float(totals.get("bailin_cut", 0.0)),
			float(event.get("cost_depositors", 0.0))])
	_check("R1 seguro no total = custo do evento (1×)",
		absf(float(totals.get("insured_payout", 0.0))
			- float(event.get("cost_fund", 0.0))) <= 1e-6,
		"total=%.9f evento=%.9f"
			% [float(totals.get("insured_payout", 0.0)),
				float(event.get("cost_fund", 0.0))])
	_check("R1 Tesouro no total = custo do evento (1×)",
		absf(float(totals.get("treasury_rescue", 0.0))
			- float(event.get("cost_treasury", 0.0))) <= 1e-6,
		"total=%.9f evento=%.9f"
			% [float(totals.get("treasury_rescue", 0.0)),
				float(event.get("cost_treasury", 0.0))])
	_check("R1 uma resolução contada (inclusive a casca)",
		int(totals.get("resolutions", 0)) == 1,
		"resolutions=%s" % str(totals.get("resolutions")))
	_check("R1 um evento contado", int(totals.get("crisis_events", 0)) == 1,
		"crisis_events=%s" % str(totals.get("crisis_events")))
	# A casca absorvida não tem saldo, mas a resolução dela continua contada
	# (`resolutions` varre todos os bancos; `banks` é o contrato da C4).
	# Acumuladores COPIADOS ao banco público: zerados na casca (senão o total
	# contaria duas vezes) e presentes no banco público.
	var public_row := _bank(sim, PUBLIC)
	_check("R1 acumuladores copiados zerados na casca",
		absf(float(shell.get("writeoffs", 0.0))) <= EPS
			and int(shell.get("defaults", 0)) == 0
			and int(shell.get("loans_count", 0)) == 0,
		"writeoffs=%.6f defaults=%s loans_count=%s"
			% [float(shell.get("writeoffs", 0.0)),
				str(shell.get("defaults")), str(shell.get("loans_count"))])
	_check("R1 acumuladores copiados estão no banco público",
		float(public_row.get("writeoffs", 0.0)) > 0.0,
		"writeoffs=%.6f" % float(public_row.get("writeoffs", 0.0)))
	print("[c6-crisis] R1: rombo=%.2f depositantes=%.2f fundo=%.2f takeover=%.2f"
		% [float(event.get("gap", 0.0)),
			float(event.get("cost_depositors", 0.0)),
			float(event.get("cost_fund", 0.0)),
			float(event.get("cost_takeover", 0.0))])


# --- R3: save com perda pendente (`loss_today`) ---------------------------
# O choque escreve `loss_today`; salvar ANTES do tick e recarregar tem de
# preservar o valor e continuar idêntico (a EMA do dia seguinte parte dele).
func _test_save_pending_loss() -> void:
	var sim = _stage(0.32, "private", true)
	var shock: Dictionary = sim.bank_shock(PRIVATE, 0.0, 0.30)
	_check("R3 choque aplicado", float(shock.get("written", 0.0)) > 0.0,
		JSON.stringify(shock))
	var saved: Dictionary = sim.to_dict()
	var saved_loss := _saved_loss(saved, PRIVATE)
	_check("R3 save tem perda pendente (loss_today ≠ 0)",
		saved_loss > 0.0, "loss_today=%.9f" % saved_loss)
	var loaded = _load(saved, false)
	loaded.set_bank_credit(true)
	loaded.set_bank_policy(0.02, LEVERAGE_CAP, 2.0, 30, 0.01)
	_check("R3 round-trip bit-exato com perda pendente",
		JSON.stringify(loaded.to_dict()) == JSON.stringify(saved), "difere")
	_check("R3 loss_today preservado na carga",
		absf(_saved_loss(loaded.to_dict(), PRIVATE) - saved_loss) <= 1e-12,
		"sim=%.12f loaded=%.12f"
			% [saved_loss, _saved_loss(loaded.to_dict(), PRIVATE)])
	print("[c6-crisis] R3: loss_today=%.6f" % saved_loss)
	for day in 5:
		var original: Dictionary = sim.tick_day()
		var after: Dictionary = loaded.tick_day()
		_check("R3 dia %d após a carga sem violação (original)" % (day + 1),
			(original.get("invariant_violations", []) as Array).is_empty(),
			str(original.get("invariant_violations", [])))
		_check("R3 dia %d após a carga sem violação (carregado)" % (day + 1),
			(after.get("invariant_violations", []) as Array).is_empty(),
			str(after.get("invariant_violations", [])))
		_check_identity(sim, "R3 dia %d original" % (day + 1))
		_check_identity(loaded, "R3 dia %d carregado" % (day + 1))
	_check("R3 continuação idêntica com a perda pendente",
		JSON.stringify(sim.get_bank_status())
			== JSON.stringify(loaded.get_bank_status()), "status difere")
	_check("R3 dinheiro igual na continuação",
		absf(float(sim.get_status().get("money_total", 0.0))
			- float(loaded.get_status().get("money_total", 0.0))) <= EPS,
		"sim=%s loaded=%s" % [sim.get_status().get("money_total"),
			loaded.get_status().get("money_total")])


# --- (e) save v34 --------------------------------------------------------
func _test_save_v34() -> void:
	var sim = _stage(0.32, "private", true)
	sim.bank_shock(PRIVATE, 0.0, 0.30)
	for day in 6:
		sim.tick_day()
	var saved: Dictionary = sim.to_dict()
	_check("E save é v%d" % SaveSchema.current(), int(saved.get("version", 0)) == SaveSchema.current(),
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


## Perda reconhecida no dia (`loss_today`) da linha de banco no SAVE — o
## status publicado não expõe o campo, e é ele que a carga tem de preservar.
func _saved_loss(data: Dictionary, id: String) -> float:
	var rows: Dictionary = data.get("banks", {}).get("banks", {})
	var row: Dictionary = rows.get(id, {})
	return float(row.get("loss_today", 0.0))


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


## Dívida pública emitida (soma de `gov_bonds` no Ledger; não é caixa).
func _government_debt(sim) -> float:
	return float(sim.get_balance_status().get("world", {})
		.get("government", {}).get("debt", 0.0))


## Zera o orçamento público (o "Tesouro" do modelo) de TODAS as cidades da
## nação e devolve o sim recarregado do próprio save (round-trip sem inventar
## estado). As regras de crédito são reaplicadas por quem chama.
func _drain_treasury(sim):
	var save: Dictionary = sim.to_dict()
	for city: Variant in save.get("cities", []):
		var row: Dictionary = city
		if String(row.get("nation", "")) == "DEMO":
			row["government_budget"] = 0.0
	return _load(save, false)


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
