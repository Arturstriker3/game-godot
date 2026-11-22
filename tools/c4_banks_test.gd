extends SceneTree

## C4 (D-110) — BANCOS COM DONO (público, privado nacional e estrangeiro).
##
## Cenário: o fixture AGRIA/PORTUS (nação DEMO, moeda DEM) para a mecânica e o
## `banks.json` real para nomes/participações/presença do estrangeiro.
##
##   B2  participações por nação somam 1 (0,45/0,40/0,15 com estrangeiro;
##       0,55/0,45 sem ele) e o estrangeiro só existe quando o dado declara.
##   B3  migração v31 -> v32: o banco agregado da C3 é dividido pelos três
##       papéis CONSERVANDO os valores do agregado (bit-exato por nação).
##   B4  dinheiro bancário: empréstimo cria depósito (M1) e NÃO M0; pagamento
##       do principal e juro PAGO destroem; identidade da C1 fecha todo dia.
##   B5  dividendo do banco estrangeiro: remete ao país de origem pelo câmbio
##       da C2c (paridade) e credita a posição do exterior.
##   B6  nomes fictícios: todo nome é o de `banks.json` (ou vazio = no_data);
##       nenhuma marca real.
##   B7  save v32: round-trip bit-exato e continuação de 5 dias idêntica.
##
## Roda pelo runner: powershell -File tools/run_tests.ps1 -List c4_banks_test

const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"
const SERVICES_PATH := "res://data/definitions/economy/services.json"
const SECTOR_EMPLOYMENT_PATH := "res://data/definitions/economy/sector_employment.json"
const AGE_STRUCTURE_PATH := "res://data/definitions/population/age_structure_2026.json"
const BANKS_PATH := "res://data/definitions/economy/banks.json"

const EPS := 1e-6
const DAYS_IDENTITY := 20
const LEVERAGE_CAP := 10.0
# Marcas reais que NÃO podem aparecer: o dado é fictício por política do projeto.
const REAL_BRANDS: Array[String] = ["HSBC", "Santander", "JPMorgan", "Chase",
	"Citigroup", "Citibank", "BNP", "Deutsche", "Mizuho", "Mitsubishi",
	"ICBC", "Itaú", "Itau", "Bradesco", "BBVA", "UniCredit", "UBS",
	"Credit Suisse", "Goldman", "Morgan Stanley", "Barclays", "Wells Fargo"]

var _failures: Array[String] = []


func _init() -> void:
	_test_shares()
	_test_names_fictitious()
	_test_migration_v31_conserves()
	_test_bank_money()
	_test_foreign_remittance()
	_test_identity_with_credit()
	_test_save_v32()
	if _failures.is_empty():
		print("[c4-banks] OK: B2-B7 — participações, nomes fictícios, migração "
			+ "v31->v32 conservando, dinheiro bancário (M1 != M0), remessa do "
			+ "estrangeiro pela paridade, identidade diária e save v32.")
		quit(0)
	else:
		print("[c4-banks] FALHAS (%d):" % _failures.size())
		for failure: String in _failures:
			print(" - " + failure)
		quit(1)


# --- B2 -------------------------------------------------------------------
func _test_shares() -> void:
	var sim = _world(1)
	sim.set_bank_definitions(_json(BANKS_PATH))
	var banks: Array = sim.get_bank_status().get("banks", [])
	_check("B2 status publica bancos", banks.size() > 179 * 2,
		"banks=%d" % banks.size())
	var by_id: Dictionary = {}
	for row: Dictionary in banks:
		by_id[String(row.get("id", ""))] = row
	var sums: Dictionary = {}
	var mismatched := 0
	for row: Dictionary in banks:
		var nation := String(row.get("nation", ""))
		sums[nation] = float(sums.get(nation, 0.0)) + float(row.get("share", 0.0))
	for nation: Variant in sums:
		if absf(float(sums[nation]) - 1.0) > 1e-12:
			mismatched += 1
	_check("B2 participações somam 1 em toda nação", mismatched == 0,
		"nacoes_fora=%d" % mismatched)
	# Brasil: o dado declara os três papéis (público, privado e estrangeiro ESP).
	var bra_public: Dictionary = by_id.get("BRA/public", {})
	var bra_private: Dictionary = by_id.get("BRA/private", {})
	var bra_foreign: Dictionary = by_id.get("BRA/foreign", {})
	_check("B2 BRA tem os três papéis",
		not bra_public.is_empty() and not bra_private.is_empty()
			and not bra_foreign.is_empty(),
		"ids=%d" % by_id.size())
	_check("B2 BRA participações 0,45/0,40/0,15",
		absf(float(bra_public.get("share", 0.0)) - 0.45) <= 1e-12
			and absf(float(bra_private.get("share", 0.0)) - 0.40) <= 1e-12
			and absf(float(bra_foreign.get("share", 0.0)) - 0.15) <= 1e-12,
		"shares=%s/%s/%s" % [bra_public.get("share"),
			bra_private.get("share"), bra_foreign.get("share")])
	_check("B2 BRA estrangeiro é dono do exterior (origin)",
		String(bra_foreign.get("owner_kind", "")) == "foreign"
			and String(bra_foreign.get("owner_nation", "")) == "ESP",
		"owner=%s/%s" % [bra_foreign.get("owner_kind"),
			bra_foreign.get("owner_nation")])
	# Nação sem banco estrangeiro no dado (AFG: foreign no_data): dois papéis.
	var afg_foreign: Dictionary = by_id.get("AFG/foreign", {})
	var afg_public: Dictionary = by_id.get("AFG/public", {})
	var afg_private: Dictionary = by_id.get("AFG/private", {})
	_check("B2 AFG não inventa banco estrangeiro", afg_foreign.is_empty(),
		"AFG/foreign=%s" % str(afg_foreign))
	_check("B2 AFG sem estrangeiro: 0,55/0,45",
		absf(float(afg_public.get("share", 0.0)) - 0.55) <= 1e-12
			and absf(float(afg_private.get("share", 0.0)) - 0.45) <= 1e-12,
		"shares=%s/%s" % [afg_public.get("share"),
			afg_private.get("share")])
	# Estado e famílias são os donos declarados dos outros dois papéis.
	_check("B2 público é do Estado", String(bra_public.get("owner_kind", ""))
		== "state", String(bra_public.get("owner_kind", "")))
	_check("B2 privado é nacional", String(bra_private.get("owner_kind", ""))
		== "national", String(bra_private.get("owner_kind", "")))


# --- B6 -------------------------------------------------------------------
func _test_names_fictitious() -> void:
	var sim = _world(1)
	var data: Dictionary = _json(BANKS_PATH)
	sim.set_bank_definitions(data)
	var nations: Dictionary = data.get("nations", {})
	var banks: Array = sim.get_bank_status().get("banks", [])
	var wrong := 0
	var examples: Array[String] = []
	var brand_hits: Array[String] = []
	var with_name := 0
	for row: Dictionary in banks:
		var nation := String(row.get("nation", ""))
		var role := String(row.get("role", ""))
		var expected := _expected_name(nations, nation, role)
		var name := String(row.get("name", ""))
		if name != expected:
			wrong += 1
			if examples.size() < 5:
				examples.append("%s/%s esperado='%s' obtido='%s'"
					% [nation, role, expected, name])
		if not name.is_empty():
			with_name += 1
		for brand: String in REAL_BRANDS:
			if name.to_lower().contains(brand.to_lower()):
				brand_hits.append("%s=%s" % [nation, name])
	_check("B6 nome do banco é o de banks.json (ou vazio = no_data)",
		wrong == 0, "nomes_divergentes=%d %s" % [wrong, str(examples)])
	_check("B6 nomes existem para quem o dado declara", with_name > 250,
		"com_nome=%d" % with_name)
	_check("B6 nenhuma marca real", brand_hits.is_empty(),
		"marcas=%s" % str(brand_hits))
	# Um nome ausente no dado é declarado `no_data`, nunca inventado.
	var nodata := 0
	for row: Dictionary in banks:
		if String(row.get("name", "")).is_empty():
			_check("B6 banco sem nome declara no_data (%s)"
				% String(row.get("id", "")),
				String(row.get("name_status", "")) == "no_data",
				String(row.get("name_status", "")))
			nodata += 1
	print("[c4-banks] B6 bancos com nome=%d sem nome (no_data)=%d"
		% [with_name, nodata])


# --- B3 -------------------------------------------------------------------
func _test_migration_v31_conserves() -> void:
	var sim = _world(3)
	sim.set_bank_definitions(_definition_foreign())
	sim.set_bank_credit(true)
	sim.set_bank_policy(0.02, LEVERAGE_CAP, 2.0, 30, 0.5)
	sim.set_bank_capital("DEMO", 1.0e5, "public")
	sim.set_bank_capital("DEMO", 1.0e4, "private")
	sim.group_borrow(_first_group(sim), 1.0e9)
	sim.tick_day()
	var save: Dictionary = sim.to_dict()
	var totals: Dictionary = sim.get_bank_status().get("totals", {})
	# Reescreve o bloco no formato v31: um banco AGREGADO por nação.
	var block: Dictionary = save.get("banks", {})
	var aggregate: Dictionary = {
		"currency": "DEM",
		"loans": float(totals.get("loans", 0.0)),
		"arrears": float(totals.get("arrears", 0.0)),
		"bc_borrowing": float(totals.get("bc_borrowing", 0.0)),
		"cash": float(totals.get("cash", 0.0)),
		"capital": float(totals.get("capital", 0.0)),
		"interest_income": float(totals.get("interest_income", 0.0)),
		"interest_expense": float(totals.get("interest_expense", 0.0)),
		"writeoffs": float(totals.get("writeoffs", 0.0)),
		"defaults": int(totals.get("defaults", 0)),
		"loans_count": int(totals.get("loans_count", 0)),
		"last_default_day": -1,
	}
	save["version"] = 31
	save["banks"] = {"enabled": true, "nations": {"DEMO": aggregate}}
	var loaded = _load(save, false)
	var after: Dictionary = loaded.get_bank_status().get("totals", {})
	_check("B3 migração cria os três papéis",
		(loaded.get_bank_status().get("banks", []) as Array).size() == 3,
		"banks=%d" % (loaded.get_bank_status().get("banks", []) as Array).size())
	for key: String in ["loans", "arrears", "bc_borrowing", "cash", "capital",
			"interest_income", "interest_expense", "writeoffs"]:
		_check("B3 conserva %s" % key,
			absf(float(after.get(key, 0.0)) - float(aggregate.get(key, 0.0)))
				<= EPS,
			"antes=%s depois=%s" % [aggregate.get(key), after.get(key)])
	_check("B3 conserva loans_count",
		int(after.get("loans_count", -1)) == int(aggregate.get("loans_count")),
		"antes=%s depois=%s" % [aggregate.get("loans_count"),
			after.get("loans_count")])
	_check("B3 depósitos da C3 não são inventados",
		absf(float(after.get("deposits", -1.0))) <= EPS,
		"deposits=%s" % after.get("deposits"))
	_check("B3 versão migra para 35",
		int(save.get("version", 0)) == 31
			and int(loaded.to_dict().get("version", 0)) == 35,
		"version=%s" % loaded.to_dict().get("version"))
	var invariant: Dictionary = loaded.get_balance_status().get("invariant", {})
	_check("B3 identidade fecha depois da migração",
		bool(invariant.get("ok", false)),
		"financial_sum=" + str(invariant.get("financial_sum")))


# --- B4 -------------------------------------------------------------------
func _test_bank_money() -> void:
	var sim = _world(1)
	sim.set_bank_definitions(_definition_foreign())
	sim.set_bank_credit(true)
	sim.set_bank_policy(0.02, LEVERAGE_CAP, 2.0, 30, 0.5)
	var group_id := _first_group(sim)
	var money0 := float(sim.get_status().get("money_total"))
	var totals0: Dictionary = sim.get_bank_status().get("totals", {})
	var emission0 := float(sim.get_central_bank_status()
		.get("totals", {}).get("emission_today", 0.0))
	var borrow: Dictionary = sim.group_borrow(group_id, 1.0e9)
	var granted := float(borrow.get("granted", 0.0))
	_check("B4 empresta algo", granted > 0.0, JSON.stringify(borrow))
	var money1 := float(sim.get_status().get("money_total"))
	var totals1: Dictionary = sim.get_bank_status().get("totals", {})
	_check("B4 empréstimo cria M1 (Δm = A)",
		absf(money1 - money0 - granted) <= EPS,
		"Δm=%.6f A=%.6f" % [money1 - money0, granted])
	_check("B4 empréstimo cria depósito (Δdep = A)",
		absf(float(totals1.get("deposits", 0.0))
			- float(totals0.get("deposits", 0.0)) - granted) <= EPS,
		"Δdep=%.6f" % (float(totals1.get("deposits", 0.0))
			- float(totals0.get("deposits", 0.0))))
	_check("B4 M0 não muda (M1 = M0 + depósitos)",
		absf(float(totals1.get("m0", 0.0)) - float(totals0.get("m0", 0.0)))
			<= EPS,
		"ΔM0=%.6f" % (float(totals1.get("m0", 0.0))
			- float(totals0.get("m0", 0.0))))
	_check("B4 BC não emite no empréstimo",
		absf(float(sim.get_central_bank_status().get("totals", {})
			.get("emission_today", 0.0)) - emission0) <= EPS,
		"Δemissão=%.6f" % (float(sim.get_central_bank_status()
			.get("totals", {}).get("emission_today", 0.0)) - emission0))
	var income0 := float(totals1.get("interest_income", 0.0))
	sim.tick_day()
	var totals2: Dictionary = sim.get_bank_status().get("totals", {})
	var principal := granted - float(totals2.get("loans", 0.0))
	var interest := float(totals2.get("interest_income", 0.0)) - income0
	var money2 := float(sim.get_status().get("money_total"))
	_check("B4 pagamento destrói M1 (Δm = −P − juro pago)",
		absf(money2 - money1 + principal + interest) <= EPS,
		"Δm=%.6f P=%.6f I=%.6f" % [money2 - money1, principal, interest])
	var invariant: Dictionary = sim.get_balance_status().get("invariant", {})
	_check("B4 identidade da C1 fecha com dinheiro bancário",
		bool(invariant.get("ok", false)),
		"financial_sum=" + str(invariant.get("financial_sum")))
	# Bancos com dono: capital do público saiu do Tesouro e o patrimônio é do
	# dono; o caixa do banco é dinheiro do mundo (conservação).
	_check("B4 capital é claim do dono (público=Estado)",
		float(totals2.get("capital", 0.0)) >= 0.0
			and float(totals2.get("banks", 0.0)) == 3.0,
		"capital=%s banks=%s" % [totals2.get("capital"),
			totals2.get("banks")])


# --- B5 -------------------------------------------------------------------
func _test_foreign_remittance() -> void:
	var sim = _world(3)
	sim.set_bank_definitions(_definition_foreign())
	sim.set_bank_credit(true)
	sim.set_bank_policy(0.02, LEVERAGE_CAP, 2.0, 30, 0.5)
	sim.group_borrow(_first_group(sim), 1.0e9)
	var save: Dictionary = sim.to_dict()
	# O banco que emprestou passa a ser ESTRANGEIRO (dono USA), com patrimônio
	# (lucro retido) para distribuir: é o caminho do dividendo/remessa.
	var block: Dictionary = save.get("banks", {})
	var rows: Dictionary = block.get("banks", {})
	var min_loans := -1.0
	var lender_key := ""
	for id: Variant in rows:
		var row: Dictionary = rows[id]
		if float(row.get("loans", 0.0)) > min_loans:
			min_loans = float(row.get("loans", 0.0))
			lender_key = String(id)
	_check("B5 há banco com empréstimo para virar estrangeiro",
		min_loans > 0.0 and not lender_key.is_empty(), lender_key)
	if lender_key.is_empty():
		return
	var lender: Dictionary = rows[lender_key]
	lender["role"] = "foreign"
	lender["owner_kind"] = "foreign"
	lender["owner_nation"] = "USA"
	lender["name"] = "Banco Estrangeiro de Teste"
	lender["capital"] = 0.0
	lender["deposits"] = 0.0
	lender["cash"] = 0.0
	rows.erase(lender_key)
	rows["DEMO/foreign"] = lender
	block["banks"] = rows
	save["banks"] = block
	var loaded = _load(save, false)
	var day0 := int(loaded.get_status().get("day", 0))
	var ticks := (30 - (day0 % 30)) % 30
	if ticks == 0:
		ticks = 30
	for step in ticks:
		loaded.tick_day()
	var status: Dictionary = loaded.get_bank_status()
	var foreign_row: Dictionary = {}
	for row2: Dictionary in status.get("banks", []):
		if String(row2.get("id", "")) == "DEMO/foreign":
			foreign_row = row2
	_check("B5 banco estrangeiro publica dividendo",
		float(foreign_row.get("dividends", 0.0)) > 0.0,
		"dividends=%s" % foreign_row.get("dividends"))
	_check("B5 remessa em âncora > 0",
		float(foreign_row.get("remittance", 0.0)) > 0.0,
		"remittance=%s" % foreign_row.get("remittance"))
	_check("B5 remessa convertida pela paridade da C2c (USD 2,0)",
		absf(float(foreign_row.get("remittance_origin", 0.0))
			- 2.0 * float(foreign_row.get("remittance", 0.0))) <= 1e-6,
		"origem=%s ancora=%s" % [foreign_row.get("remittance_origin"),
			foreign_row.get("remittance")])
	_check("B5 remessa não excede o dividendo",
		float(foreign_row.get("remittance", 0.0))
			<= float(foreign_row.get("dividends", 0.0)) + EPS,
		"remessa=%s dividendo=%s" % [foreign_row.get("remittance"),
			foreign_row.get("dividends")])
	var invariant: Dictionary = loaded.get_balance_status().get("invariant", {})
	_check("B5 identidade fecha com a remessa",
		bool(invariant.get("ok", false)),
		"financial_sum=" + str(invariant.get("financial_sum")))


# --- B2 (identidade diária com crédito) -----------------------------------
func _test_identity_with_credit() -> void:
	var sim = _world(1)
	sim.set_bank_definitions(_definition_foreign())
	sim.set_bank_credit(true)
	sim.set_bank_policy(0.02, LEVERAGE_CAP, 2.0, 30, 0.5)
	sim.set_bank_capital("DEMO", 1.0e5, "public")
	sim.set_bank_capital("DEMO", 1.0e4, "private")
	sim.set_bank_capital("DEMO", 1.0e4, "foreign")
	var group_id := _first_group(sim)
	var worst := 0.0
	for day in DAYS_IDENTITY:
		if day % 5 == 0:
			sim.group_borrow(group_id, 1.0e9)
		var summary: Dictionary = sim.tick_day()
		_check("B-Y dia %d sem violação de invariante" % (day + 1),
			(summary.get("invariant_violations", []) as Array).is_empty(),
			str(summary.get("invariant_violations", [])))
		var invariant: Dictionary = sim.get_balance_status().get("invariant", {})
		var money := float(invariant.get("money_total", 0.0))
		var tolerance := maxf(1e-3, absf(money) * 1e-12)
		var residual := absf(float(invariant.get("financial_sum", 0.0)))
		worst = maxf(worst, residual / maxf(tolerance, 1e-12))
		_check("B-Y dia %d identidade fecha" % (day + 1),
			residual <= tolerance and bool(invariant.get("ok", false)),
			"financial_sum=%.12f tol=%.12f" % [residual, tolerance])
	print("[c4-banks] B-Y pior resíduo/tolerância=%.6f" % worst)


# --- B7 -------------------------------------------------------------------
func _test_save_v32() -> void:
	var sim = _world(5)
	sim.set_bank_definitions(_definition_foreign())
	sim.set_bank_credit(true)
	sim.set_bank_policy(0.02, LEVERAGE_CAP, 2.0, 30, 0.5)
	sim.set_bank_capital("DEMO", 1.0e5, "public")
	sim.group_borrow(_first_group(sim), 1.0e9)
	sim.tick_day()
	var saved: Dictionary = sim.to_dict()
	_check("B7 save é v35", int(saved.get("version", 0)) == 35,
		"version=" + str(saved.get("version")))
	_check("B7 save publica bancos com papel e dono",
		(saved.get("banks", {}) as Dictionary).has("banks")
			and (saved.get("banks", {}) as Dictionary).has("enabled"),
		str((saved.get("banks", {}) as Dictionary).keys()))
	var loaded = _load(saved, false)
	# Os parâmetros de REGRA do laboratório (spread/teto) não são estado salvo:
	# reaplicados no carregado para a continuação (como no teste da C3).
	loaded.set_bank_policy(0.02, LEVERAGE_CAP, 2.0, 30, 0.5)
	_check("B7 round-trip v32 bit-exato",
		JSON.stringify(loaded.to_dict()) == JSON.stringify(saved), "difere")
	for day in 5:
		sim.tick_day()
		loaded.tick_day()
	_check("B7 continuação 5 d idêntica",
		JSON.stringify(sim.get_bank_status())
			== JSON.stringify(loaded.get_bank_status())
			and absf(float(sim.get_status().get("money_total", 0.0))
				- float(loaded.get_status().get("money_total", 0.0))) <= EPS,
		"money sim=%s loaded=%s" % [sim.get_status().get("money_total"),
			loaded.get_status().get("money_total")])


# --- helpers --------------------------------------------------------------
func _world(days: int):
	var sim = EconomySim.new()
	sim.set_goods(_json(GOODS_PATH))
	sim.set_recipes(_json(RECIPES_PATH))
	sim.set_needs(_json(NEEDS_PATH))
	sim.set_services(_json(SERVICES_PATH))
	sim.set_sector_employment(_json(SECTOR_EMPLOYMENT_PATH))
	sim.set_age_structure(_json(AGE_STRUCTURE_PATH))
	# Duas moedas: DEM (nação DEMO, paridade 1) e USD (USA, paridade 2) — a
	# segunda existe para medir a conversão da remessa do banco estrangeiro.
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


# Definição sintética do fixture: os três papéis, com o estrangeiro de USA.
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


func _expected_name(nations: Dictionary, nation: String, role: String) -> String:
	if not nations.has(nation):
		return ""
	var entry: Dictionary = nations[nation]
	match role:
		"public":
			var pub: Dictionary = entry.get("public", {})
			return _text(pub.get("name")) \
				if _text(pub.get("status")) == "present" else ""
		"private":
			var pri: Dictionary = entry.get("private", {})
			return _text(pri.get("name")) \
				if _text(pri.get("status")) == "present" else ""
		"foreign":
			var fgn: Dictionary = entry.get("foreign", {})
			if _text(fgn.get("status")) != "present" \
					or _text(fgn.get("origin_iso3")).is_empty():
				return ""
			return _text(fgn.get("branch_candidate"))
	return ""


## Texto do dado: `null` (ausência declarada) vira "" — nunca "<null>".
func _text(value: Variant) -> String:
	if value == null:
		return ""
	return String(value)


func _first_group(sim) -> String:
	return String((sim.get_groups()[0] as Dictionary).get("id", ""))


func _foreign_cash(sim) -> float:
	var balance: Dictionary = sim.get_balance_status()
	var world: Dictionary = balance.get("world", {})
	var external: Dictionary = world.get("external", {})
	return float(external.get("cash", 0.0))


func _check(name: String, ok: bool, detail: String) -> void:
	if not ok:
		_failures.append("%s (%s)" % [name, detail])


func _json(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return {}
	return JSON.parse_string(file.get_as_text())
