extends SceneTree

const SaveSchema := preload("res://core/economy/save_schema.gd")

## C3 (D-107) — CRÉDITO DE EMPRESA PELO BANCO AGREGADO (aceites A2–A7).
##
## Cenário: fixture AGRIA/PORTUS (duas cidades, nação DEMO, moeda DEM). O
## crédito nasce LIGADO no jogo desde a D-111; os casos que o exercitam ligam
## por comando A3 (explícito) e o default compilado é conferido em
## `_test_default_on`.
##
##   A2  identidade da C1 fecha em TODO dia com o banco ativo (60 d, capital
##       pedido de 1e6 — o que o Tesouro do fixture paga é DECLARADO no log).
##   A3  empréstimo cria M0 (Δm = A = Δemissão) e o pagamento destrói
##       (Δm = −principal − juro básico repassado ao BC).
##   A4  alavancagem responde ao juro: com a cobertura de juros ativa
##       (leverage_cap 10), 20 % empresta >20 % menos que 2 % em 60 d.
##       Com o teto default 1.0 a ALAVANCAGEM é o limite e o canal do juro
##       não aparece no fixture — os dois números saem no log (declarado).
##   A5  calote é perda do credor: Δpatrimônio do banco = juro do dia −
##       baixa; grupo com dívida 0, construções fechadas; identidade fecha.
##   A6  save v30 bit-exato, continuação 5 d idêntica, v29 antigo carrega com
##       crédito desligado e dívida 0.
##   A7  ausência declarada: sem capital/depósito o banco publica 0/absent e
##       nação desconhecida não recebe capital inventado.
##
## Roda pelo runner: powershell -File tools/run_tests.ps1 -List c3_bank_credit_test

const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"
const SERVICES_PATH := "res://data/definitions/economy/services.json"
const SECTOR_EMPLOYMENT_PATH := "res://data/definitions/economy/sector_employment.json"
const AGE_STRUCTURE_PATH := "res://data/definitions/population/age_structure_2026.json"

const EPS := 1e-6
const DAYS_IDENTITY := 60
const DAYS_LEVERAGE := 60
const DEFAULT_LEVERAGE_CAP := 1.0
const LEVERAGE_CAP := 10.0

var _failures: Array[String] = []


func _init() -> void:
	_test_default_on()
	_test_absence()
	_test_loan_creates_and_payment_destroys()
	_test_identity_with_bank()
	_test_leverage_responds_to_interest()
	_test_default_is_creditor_loss()
	_test_save()
	_test_status_contract()
	if _failures.is_empty():
		print("[c3-bank] OK: A2–A7 — empréstimo/pagamento, identidade diária, "
			+ "alavancagem × juro, calote com perda do banco, save v30 e "
			+ "ausência declarada.")
		quit(0)
	else:
		print("[c3-bank] FALHAS (%d):" % _failures.size())
		for failure: String in _failures:
			print(" - " + failure)
		quit(1)


# --- A7 -------------------------------------------------------------------
func _test_absence() -> void:
	var sim = _world(0)
	var bank: Dictionary = sim.get_bank_status()
	var policy: Dictionary = bank.get("policy", {})
	var totals: Dictionary = bank.get("totals", {})
	_check("A7 dívida/capital 0 sem banco",
		absf(float(totals.get("capital", -1.0))) <= EPS
			and absf(float(totals.get("group_debt", -1.0))) <= EPS,
		"capital=%s debt=%s" % [totals.get("capital"),
			totals.get("group_debt")])
	_check("A7 depósito é dinheiro bancário (C4)",
		String(policy.get("deposits", "")) == "firm deposits (bank money, C4)"
			and String(policy.get("resolution", "")) == "C6",
		String(policy.get("deposits", "")))
	_check("A7 nenhuma nação inventada",
		(bank.get("nations", []) as Array).is_empty(),
		"nations=%d" % (bank.get("nations", []) as Array).size())
	var refused: Dictionary = sim.set_bank_capital("XXX", 1000.0)
	_check("A7 nação sem cidade recusa capital",
		not bool(refused.get("ok", true))
			and absf(float(refused.get("paid", -1.0))) <= EPS,
		JSON.stringify(refused))


# --- A3 -------------------------------------------------------------------
func _test_loan_creates_and_payment_destroys() -> void:
	var sim = _world(1)
	sim.set_bank_credit(true)
	sim.set_bank_policy(0.02, LEVERAGE_CAP, 2.0, 30, 0.5)
	var group_id := _first_group(sim)
	var money0 := float(sim.get_status().get("money_total"))
	var emission0 := _emission(sim)
	var deposits0 := float(_bank_totals(sim).get("deposits", 0.0))
	var m0_0 := float(_bank_totals(sim).get("m0", 0.0))
	var borrow: Dictionary = sim.group_borrow(group_id, 1.0e9)
	var granted := float(borrow.get("granted", 0.0))
	_check("A3 empresta algo", granted > 0.0, JSON.stringify(borrow))
	var money1 := float(sim.get_status().get("money_total"))
	# C4 (D-110): o empréstimo cria DINHEIRO BANCÁRIO (depósito do tomador), não
	# M0 — M1 sobe A, M0 não muda e o BC não emite.
	_check("A3 empréstimo cria dinheiro bancário (Δm = A)",
		absf(money1 - money0 - granted) <= EPS,
		"Δm=%.6f A=%.6f" % [money1 - money0, granted])
	_check("A3 empréstimo cria depósito (Δdep = A)",
		absf(float(_bank_totals(sim).get("deposits", 0.0)) - deposits0
			- granted) <= EPS,
		"Δdep=%.6f A=%.6f" % [float(_bank_totals(sim).get("deposits", 0.0))
			- deposits0, granted])
	_check("A3 empréstimo NÃO é emissão do BC",
		absf(_emission(sim) - emission0) <= EPS,
		"Δemissão=%.6f" % (_emission(sim) - emission0))
	_check("A3 M0 não muda com o empréstimo",
		absf(float(_bank_totals(sim).get("m0", 0.0)) - m0_0) <= EPS,
		"ΔM0=%.6f" % (float(_bank_totals(sim).get("m0", 0.0)) - m0_0))
	var loans_count := int(_bank_totals(sim).get("loans_count", 0))
	_check("A3 fixture empresta só o pedido do teste", loans_count == 1,
		"loans_count=%d" % loans_count)
	var income0 := float(_bank_totals(sim).get("interest_income", 0.0))
	sim.tick_day()
	var loans_after := float(_bank_totals(sim).get("loans", 0.0))
	var paid_principal := granted - loans_after
	var paid_interest := float(_bank_totals(sim).get("interest_income", 0.0)) \
		- income0
	var money2 := float(sim.get_status().get("money_total"))
	print(("[c3-bank] A3 amortização: A=%.3f P=%.3f juro pago=%.3f Δm=%.3f"
		% [granted, paid_principal, paid_interest, money2 - money1]))
	_check("A3 amortização exige principal > 0", paid_principal > 0.0,
		"P=%.6f" % paid_principal)
	_check("A3 pagamento destrói dinheiro bancário (Δm = −P − juro pago)",
		absf(money2 - money1 + paid_principal + paid_interest) <= EPS,
		"Δm=%.6f esperado=%.6f" % [money2 - money1,
			-(paid_principal + paid_interest)])


# --- A2 -------------------------------------------------------------------
func _test_identity_with_bank() -> void:
	var sim = _world(1)
	sim.set_bank_credit(true)
	sim.set_bank_policy(0.02, LEVERAGE_CAP, 2.0, 30, 0.5)
	var capital: Dictionary = sim.set_bank_capital("DEMO", 1.0e6)
	print("[c3-bank] A2 capital pedido=1e6 pago=%.3f" % float(
		capital.get("paid", 0.0)))
	var group_id := _first_group(sim)
	var worst := 0.0
	for day in DAYS_IDENTITY:
		if day % 10 == 0:
			sim.group_borrow(group_id, 1.0e9)
		sim.tick_day()
		var invariant: Dictionary = sim.get_balance_status().get("invariant", {})
		var money := float(invariant.get("money_total", 0.0))
		var tolerance := maxf(1e-3, absf(money) * 1e-12)
		var residual := absf(float(invariant.get("financial_sum", 0.0)))
		worst = maxf(worst, residual / maxf(tolerance, 1e-12))
		_check("A2 dia %d: identidade da C1" % (day + 1),
			residual <= tolerance and bool(invariant.get("ok", false)),
			"financial_sum=%.12f tol=%.12f" % [residual, tolerance])
		_check("A2 dia %d: caixa residual 0" % (day + 1),
			absf(float(invariant.get("cash_residual", 1.0))) <= 1e-3,
			"cash_residual=" + str(invariant.get("cash_residual")))
	var totals := _bank_totals(sim)
	print(("[c3-bank] A2 fim: dívida=%.3f empréstimos=%d juros=%.3f "
		+ "pior_resíduo/tol=%.4f") % [float(totals.get("group_debt", 0.0)),
			int(totals.get("loans_count", 0)),
			float(totals.get("interest_expense", 0.0)), worst])


# --- A4 -------------------------------------------------------------------
func _test_leverage_responds_to_interest() -> void:
	var low := _debt_after(LEVERAGE_CAP, 0.02)
	var high := _debt_after(LEVERAGE_CAP, 0.20)
	print(("[c3-bank] A4 cap=%.1f: dívida 2%%=%.3f 20%%=%.3f (%.1f%% menor), "
		+ "empréstimos %d/%d") % [LEVERAGE_CAP, low, high,
			(1.0 - high / maxf(low, 1e-9)) * 100.0, _loans_last, _loans_high])
	_check("A4 juro alto encurta o crédito (>20% menos dívida)",
		high < low * 0.8, "low=%.3f high=%.3f" % [low, high])
	_check("A4 nº de empréstimos não aumenta com o juro",
		_loans_high <= _loans_last, "%d > %d" % [_loans_high, _loans_last])
	# Teto default 1.0: a alavancagem é o limite; o canal do juro fica inativo.
	var low_default := _debt_after(DEFAULT_LEVERAGE_CAP, 0.02)
	var high_default := _debt_after(DEFAULT_LEVERAGE_CAP, 0.20)
	print(("[c3-bank] A4 cap=1.0 (default): dívida 2%%=%.3f 20%%=%.3f — "
		+ "alavancagem domina, canal do juro inativo no fixture")
		% [low_default, high_default])


var _loans_last := 0
var _loans_high := 0


func _debt_after(cap: float, spread: float) -> float:
	var sim = _world(1)
	sim.set_bank_credit(true)
	sim.set_bank_policy(spread, cap, 2.0, 30, 0.5)
	var group_id := _first_group(sim)
	for day in DAYS_LEVERAGE:
		sim.group_borrow(group_id, 1.0e12)
		sim.tick_day()
	var totals := _bank_totals(sim)
	if absf(spread - 0.02) <= EPS:
		_loans_last = int(totals.get("loans_count", 0))
	else:
		_loans_high = int(totals.get("loans_count", 0))
	return float(totals.get("group_debt", 0.0))


# --- A5 -------------------------------------------------------------------
func _test_default_is_creditor_loss() -> void:
	var sim = _world(1)
	sim.set_bank_credit(true)
	sim.set_bank_policy(0.02, LEVERAGE_CAP, 2.0, 30, 0.5)
	sim.set_bank_capital("DEMO", 1.0e6)
	var group_id := _first_group(sim)
	var borrow: Dictionary = sim.group_borrow(group_id, 1.0e9)
	_check("A5 empréstimo grande concedido",
		float(borrow.get("granted", 0.0)) > 0.0, JSON.stringify(borrow))
	# Estado de teste CONSERVADO: o caixa do grupo vai para o Tesouro da cidade
	# (transferência de caixa, sem criar/destruir dinheiro); a dívida real do
	# empréstimo permanece e o grupo fica insolvente.
	var data: Dictionary = sim.to_dict()
	var moved := 0.0
	for entry: Dictionary in data.get("groups", []):
		if String(entry.get("id", "")) == group_id:
			moved = float(entry.get("cash", 0.0))
			entry["cash"] = 0.0
	var cities: Array = data.get("cities", [])
	_check("A5 save tem cidades para o Tesouro", not cities.is_empty(),
		"cities=%d" % cities.size())
	if not cities.is_empty():
		var city: Dictionary = cities[0]
		city["government_budget"] = float(city.get("government_budget", 0.0)) \
			+ moved
	var loaded = _load(data)
	loaded.set_bank_policy(0.02, LEVERAGE_CAP, 2.0, 2, 0.5)
	var money_pre := float(loaded.get_status().get("money_total"))
	var prev: Dictionary = loaded.get_bank_status().get("totals", {})
	var defaulted := false
	for day in 5:
		var debt_before := float(_bank_group(loaded, group_id).get("debt", 0.0))
		loaded.tick_day()
		var now: Dictionary = loaded.get_bank_status().get("totals", {})
		if int(now.get("defaults", 0)) <= int(prev.get("defaults", 0)):
			prev = now
			continue
		defaulted = true
		# Juro do dia cobrado pelo banco = dívida no início do dia × taxa anual
		# / 365 (a mesma conta de sim/bank.cpp); a baixa é o resto.
		var rate := float((loaded.get_bank_status().get("nations", [])[0] \
			as Dictionary).get("loan_rate", 0.0))
		var charged := debt_before * rate / 365.0
		var writeoff := float(now.get("writeoffs", 0.0)) \
			- float(prev.get("writeoffs", 0.0))
		var equity_delta := float(now.get("equity", 0.0)) \
			- float(prev.get("equity", 0.0))
		var money_delta := float(loaded.get_status().get("money_total")) - money_pre
		print(("[c3-bank] A5 calote no dia %d: baixa=%.3f juro do dia=%.3f "
			+ "Δpatrimônio=%.3f Δcaixa_total=%.3f")
			% [loaded.get_status().get("calendar_day"), writeoff, charged,
				equity_delta, money_delta])
		_check("A5 baixa do credor > 0", writeoff > 0.0, "%.6f" % writeoff)
		# C6 (D-114): o patrimônio negativo é RESOLVIDO no mesmo tick, então a
		# leitura antiga "Δpatrimônio = juro − baixa" e "equity < 0" não valem
		# mais: o buraco passa pela hierarquia (bail-in → fundo → Tesouro) e a
		# perda continua sendo do credor/depositante/Tesouro, não do mundo.
		var resolution := _last_resolution_event(loaded)
		_check("A5 banco resolvido pela C6", not resolution.is_empty(),
			"events=%s" % str(loaded.get_bank_status().get("events", [])))
		_check("A5 rombo da resolução > 0",
			float(resolution.get("gap", 0.0)) > 0.0,
			"gap=%s" % str(resolution.get("gap")))
		_check("A5 patrimônio não fica negativo depois da resolução",
			float(now.get("equity", 0.0)) >= -1.0,
			"equity=%.3f Δeq=%.3f" % [float(now.get("equity", 0.0)),
				equity_delta])
		_check("A5 dinheiro só cai no calote (caixa tomado/juro básico)",
			money_delta <= EPS, "Δm=%.6f" % money_delta)
		var row := _bank_group(loaded, group_id)
		_check("A5 grupo com dívida 0", absf(float(row.get("debt", -1.0))) <= EPS,
			"debt=" + str(row.get("debt")))
		_check("A5 grupo com 1 calote registrado",
			int(row.get("defaults", 0)) == 1, "defaults=" + str(row.get("defaults")))
		_check("A5 grupo sem atraso pendente",
			absf(float(row.get("arrears", -1.0))) <= EPS,
			"arrears=" + str(row.get("arrears")))
		var invariant: Dictionary = loaded.get_balance_status().get(
			"invariant", {})
		_check("A5 identidade fecha depois do calote",
			bool(invariant.get("ok", false)),
			"financial_sum=" + str(invariant.get("financial_sum")))
		break
	_check("A5 o calote aconteceu", defaulted, "nenhum default em 5 dias")
	# Um dia depois o balanço do grupo já reflete a execução (as construções
	# são fechadas no dia do calote; a leitura do grupo é do dia seguinte).
	loaded.tick_day()
	for row2: Dictionary in loaded.get_groups():
		_check("A5 construções do grupo fechadas (%s)" % row2.get("id"),
			int(row2.get("assets", -1)) == 0
				and absf(float(row2.get("capacity", -1.0))) <= EPS,
			"assets=%s capacity=%s" % [row2.get("assets"),
				row2.get("capacity")])


# --- A6 -------------------------------------------------------------------
func _test_save() -> void:
	var sim = _world(5)
	sim.set_bank_credit(true)
	sim.set_bank_policy(0.02, LEVERAGE_CAP, 2.0, 30, 0.5)
	sim.set_bank_capital("DEMO", 1.0e5)
	sim.group_borrow(_first_group(sim), 1.0e9)
	sim.tick_day()
	var saved: Dictionary = sim.to_dict()
	_check("A6 save na versao atual", int(saved.get("version", 0)) == SaveSchema.current(),
		"version=" + str(saved.get("version")))
	_check("A6 save publica o banco e o crédito",
		saved.has("banks")
			and (saved.get("banks", {}) as Dictionary).has("enabled")
			and (saved.get("banks", {}) as Dictionary).has("banks"),
		"keys=" + str((saved.get("banks", {}) as Dictionary).keys()))
	var loaded = _load(saved)
	# Os parâmetros de REGRA do laboratório (spread/teto de alavancagem) não
	# são estado salvo: reaplicados no carregado para a continuação.
	loaded.set_bank_policy(0.02, LEVERAGE_CAP, 2.0, 30, 0.5)
	_check("A6 round-trip v31 bit-exato",
		JSON.stringify(loaded.to_dict()) == JSON.stringify(saved), "difere")
	for day in 5:
		sim.tick_day()
		loaded.tick_day()
	_check("A6 continuação 5 d idêntica", JSON.stringify(
		sim.get_bank_status()) == JSON.stringify(loaded.get_bank_status())
			and absf(float(sim.get_status().get("money_total", 0.0))
				- float(loaded.get_status().get("money_total", 0.0))) <= EPS,
		"money sim=%s loaded=%s" % [sim.get_status().get("money_total"),
			loaded.get_status().get("money_total")])
	var legacy: Dictionary = saved.duplicate(true)
	legacy["version"] = 29
	legacy.erase("credit")
	legacy.erase("banks")
	for entry: Dictionary in legacy.get("groups", []):
		for key: String in ["debt", "credit_arrears", "credit_total",
				"credit_repaid", "credit_default_days", "credit_defaults"]:
			entry.erase(key)
	var old = _load(legacy)
	var old_bank: Dictionary = old.get_bank_status()
	_check("A6 v29 carrega com crédito desligado",
		not bool(old_bank.get("policy", {}).get("enabled", true)),
		"enabled=" + str(old_bank.get("policy", {}).get("enabled")))
	_check("A6 v29 carrega com dívida 0",
		absf(float(old_bank.get("totals", {}).get("group_debt", -1.0))) <= EPS,
		"debt=" + str(old_bank.get("totals", {}).get("group_debt")))
	_check("A6 v29 migra para a versao atual",
		int(old.to_dict().get("version", 0)) == SaveSchema.current(),
		"version=" + str(old.to_dict().get("version")))


# --- §5.2 -----------------------------------------------------------------
func _test_status_contract() -> void:
	var sim = _world(1)
	sim.set_bank_credit(true)
	sim.set_bank_policy(0.02, LEVERAGE_CAP, 2.0, 30, 0.5)
	sim.set_bank_capital("DEMO", 1.0e5)
	sim.group_borrow(_first_group(sim), 1.0e9)
	var bank: Dictionary = sim.get_bank_status()
	for key: String in ["policy", "totals", "nations", "groups", "invariant"]:
		_check("status A4 publica " + key, bank.has(key), "faltou " + key)
	var nations: Array = bank.get("nations", [])
	_check("status A4 publica a nação com banco", nations.size() == 1,
		"nations=%d" % nations.size())
	if not nations.is_empty():
		var row: Dictionary = nations[0]
		for key2: String in ["nation", "currency", "policy_rate", "loan_rate",
				"loans", "arrears", "bc_borrowing", "cash", "capital", "equity",
				"defaults", "writeoffs"]:
			_check("status A4 nação tem " + key2, row.has(key2), "faltou " + key2)
		_check("status A4 moeda vem da tabela de moedas",
			String(row.get("currency", "")) == "DEM",
			String(row.get("currency", "")))
		_check("status A4 taxa do empréstimo = básica + spread",
			absf(float(row.get("loan_rate", 0.0))
				- float(row.get("policy_rate", 0.0)) - 0.02) <= 1e-9,
			"loan=%s policy=%s" % [row.get("loan_rate"), row.get("policy_rate")])
	var groups: Array = bank.get("groups", [])
	_check("status A4 publica o grupo com posição", groups.size() == 1,
		"groups=%d" % groups.size())
	if not groups.is_empty():
		var row2: Dictionary = groups[0]
		for key3: String in ["id", "name", "origin_nation", "sector", "cash",
				"debt", "arrears", "credit_total", "credit_repaid",
				"default_days", "defaults"]:
			_check("status A4 grupo tem " + key3, row2.has(key3),
				"faltou " + key3)


# --- helpers --------------------------------------------------------------
## D-111 (C4b): o credito de empresa nasce LIGADO no mundo novo -- o default
## compilado (`WorldState::bank_credit_enabled`) tem de casar com o dado
## declarado em `bank_credit.json`, e o comando A3 tem de desligar de verdade.
func _test_default_on() -> void:
	var data: Variant = _json("res://data/definitions/economy/bank_credit.json")
	_check("D-111: dado declara credito LIGADO",
		data is Dictionary and bool((data as Dictionary).get("enabled", false)),
		"enabled no dado = " + str((data as Dictionary).get("enabled")))
	var sim = _world(0)
	var policy: Dictionary = sim.get_bank_status().get("policy", {})
	_check("D-111: mundo novo nasce com credito LIGADO (default compilado)",
		bool(policy.get("enabled", false)),
		"enabled=" + str(policy.get("enabled")))
	sim.set_bank_credit(false)
	policy = sim.get_bank_status().get("policy", {})
	_check("D-111: set_bank_credit(false) desliga explicitamente",
		not bool(policy.get("enabled", true)),
		"enabled=" + str(policy.get("enabled")))


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


## C6 (D-114): último evento de resolução do anel do status.
func _last_resolution_event(sim) -> Dictionary:
	var events: Array = sim.get_bank_status().get("events", [])
	for index in range(events.size() - 1, -1, -1):
		var event: Dictionary = events[index]
		if String(event.get("kind", "")) == "resolution":
			return event
	return {}


func _group_arrears(sim, group_id: String) -> float:
	return float(_bank_group(sim, group_id).get("arrears", 0.0))


func _emission(sim) -> float:
	return float(sim.get_central_bank_status().get("totals", {}).get(
		"emission_today", 0.0))


func _check(name: String, ok: bool, detail: String) -> void:
	if not ok:
		_failures.append("%s (%s)" % [name, detail])


func _json(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	return JSON.parse_string(file.get_as_text())
