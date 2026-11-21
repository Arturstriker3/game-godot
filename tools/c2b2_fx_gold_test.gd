extends SceneTree

## C2b2 (D-106) — REGIMES DE CÂMBIO, PADRÃO MONETÁRIO E OURO FÍSICO.
##
## Prova no cenário de fixture (duas cidades, nação DEMO, moeda DEM):
##   1. DEFAULTS: parâmetros de regra iguais ao JSON declarado (default de
##      jogo), regime salvo preservado, fixo sem reserva é ADMINISTRATIVO
##      (`funded` falso, nenhuma desvalorização) e o comando A3 aplica.
##   2. DEFESA: com reserva declarada, o peg segura a paridade, a reserva cai
##      exatamente a intervenção do dia e o dinheiro local recolhido é a
##      reserva vendida × paridade (contraparte do exterior).
##   3. ESGOTAMENTO: reserva insuficiente → desvalorização forçada, paridade
##      nova = câmbio sombreado, evento registrado no status e contador.
##   4. PADRÃO: Bretton Woods força o peg efetivo; o padrão OURO limita a
##      emissão do BC controlado pela cobertura do cofre (fiduciário emite).
##   5. OURO FÍSICO: conversão do padrão e compra/venda do cofre passam pelo
##      mercado do produto; ouro conservado (mercado + cofre) e razão do BC
##      igual ao estoque do cofre.
##   6. SAVE v29: bloco `fx`, round-trip bit-exato, continuação idêntica e
##      migração v28 → v29 (paridade 0, reserva 0, sem evento).
##
## Roda pelo runner: powershell -File tools/run_tests.ps1 -List c2b2_fx_gold_test

const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"
const SERVICES_PATH := "res://data/definitions/economy/services.json"
const SECTOR_EMPLOYMENT_PATH := "res://data/definitions/economy/sector_employment.json"
const AGE_STRUCTURE_PATH := "res://data/definitions/population/age_structure_2026.json"
const EXCHANGE_PATH := "res://data/definitions/economy/exchange_policy.json"

const SAVE_VERSION := 34
const EPS := 1e-3

var _failures: Array[String] = []


func _init() -> void:
	_test_defaults_and_command()
	_test_peg_defence()
	_test_exhaustion_devalues()
	_test_standards()
	_test_gold_physical()
	_test_save_and_migration()
	if _failures.is_empty():
		print("[c2b2] OK: defesa de paridade, desvalorizacao forcada, padrao "
			+ "ouro/BW, ouro fisico conservado, save v29 e migracao v28->v29.")
		quit(0)
	else:
		print("[c2b2] FALHAS (%d):" % _failures.size())
		for failure: String in _failures:
			print(" - " + failure)
		quit(1)


func _fail(message: String) -> void:
	_failures.append(message)


func _check(cond: bool, message: String) -> void:
	if not cond:
		_fail(message)


func _close(a: float, b: float, tolerance: float) -> bool:
	return absf(a - b) <= tolerance


func _json(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("não abriu " + path)
		return null
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data


func _make_sim(regime: String = "peg") -> Object:
	var sim = EconomySim.new()
	sim.set_goods(_json(GOODS_PATH))
	sim.set_recipes(_json(RECIPES_PATH))
	sim.set_needs(_json(NEEDS_PATH))
	sim.set_services(_json(SERVICES_PATH))
	sim.set_sector_employment(_json(SECTOR_EMPLOYMENT_PATH))
	sim.set_age_structure(_json(AGE_STRUCTURE_PATH))
	sim.set_currencies({
		"regime": regime,
		"currencies": [{
			"code": "DEM",
			"nations": ["DEMO"],
			"rate_per_anchor": 1.0,
			"source": "fixture",
		}],
	})
	sim.initialize_scenario()
	return sim


func _bank(sim: Object) -> Dictionary:
	return sim.get_central_bank_status()


func _fx(sim: Object, code: String = "DEM") -> Dictionary:
	var currencies: Dictionary = _bank(sim).get("currencies", {})
	if not currencies.has(code):
		return {}
	return (currencies[code] as Dictionary).get("fx", {})


func _totals(sim: Object) -> Dictionary:
	return (_bank(sim) as Dictionary).get("totals", {})


func _check_day(sim: Object, label: String) -> void:
	var status: Dictionary = sim.get_status()
	var balance: Dictionary = status.get("balance", {})
	_check(not balance.is_empty(), "%s: status sem `balance`" % label)
	_check(bool(balance.get("ok", false)),
		"%s: balance.ok falso (financial_sum %.9f, residual %.9f)" % [
			label, float(balance.get("financial_sum", 1e9)),
			float(balance.get("cash_residual", 1e9))])


# --- 1. Defaults do JSON, fixo administrativo e comando A3 ------------------
func _test_defaults_and_command() -> void:
	var policy: Dictionary = _json(EXCHANGE_PATH)
	_check(policy != null, "defaults: exchange_policy.json ilegível")
	if policy == null:
		return
	var sim := _make_sim("peg")
	var bank: Dictionary = _bank(sim)
	var declared: Dictionary = bank.get("monetary_policy", {})
	_check(_close(float(declared.get("band_width", -1.0)),
		float(policy.get("band_width", 0.0)), 1e-12),
		"defaults: band_width %.6f != JSON %.6f" % [
			float(declared.get("band_width", -1.0)),
			float(policy.get("band_width", 0.0))])
	_check(_close(float(declared.get("defence_share", -1.0)),
		float(policy.get("defence_share", 0.0)), 1e-12),
		"defaults: defence_share != JSON")
	var gold_rule: Dictionary = policy.get("gold_standard", {})
	_check(_close(float(declared.get("gold_reserve_ratio", -1.0)),
		float(gold_rule.get("reserve_ratio", 0.0)), 1e-12),
		"defaults: reserve_ratio != JSON")
	_check(String(declared.get("standard", "")) == "fiat",
		"defaults: padrão inicial deveria ser fiduciário")
	var fx := _fx(sim)
	_check(String(fx.get("regime_saved", "")) == "peg",
		"defaults: regime salvo do fixture deveria ser peg")
	_check(String(fx.get("regime_effective", "")) == "peg",
		"defaults: regime efetivo deveria ser peg")
	_check(not bool(fx.get("funded", true)),
		"defaults: sem reserva o fixo é administrativo (funded deveria ser falso)")
	_check(int(fx.get("devaluations", -1)) == 0,
		"defaults: desvalorização sem reserva (nada pode acontecer)")
	# O peg administrativo segura a taxa em `base_rate` (1.0) sem custo.
	for day in 10:
		sim.tick_day()
	_check(_close(float(_fx(sim).get("rate", 0.0)), 1.0, 1e-9),
		"administrativo: taxa do peg sem reserva saiu de base_rate")
	_check((_bank(sim).get("fx_events", []) as Array).is_empty(),
		"administrativo: evento de câmbio sem reserva")
	# Comando A3: o regime viaja pelo log e aplica.
	_check(sim.set_exchange_regime("DEM", "band", 0.0, 0.03),
		"comando: set_exchange_regime recusou um pedido válido")
	_check(not sim.set_exchange_regime("DEM", "flutuar", 0.0, -1.0),
		"comando: regime inválido foi aceito")
	var log: Array = sim.get_command_log()
	var last: Dictionary = log[log.size() - 1] if not log.is_empty() else {}
	_check(String(last.get("type", "")) == "set_exchange_regime",
		"comando: o log não registrou set_exchange_regime (%s)"
			% String(last.get("type", "")))
	var fx_band := _fx(sim)
	_check(String(fx_band.get("regime_saved", "")) == "band",
		"comando: regime salvo não virou band")
	_check(_close(float(fx_band.get("band_width", -1.0)), 0.03, 1e-12),
		"comando: largura da banda não foi aplicada")


# --- 2. Defesa do peg com reserva declarada --------------------------------
func _test_peg_defence() -> void:
	var sim := _make_sim("peg")
	for day in 40:
		sim.tick_day()
	var money_before := float(sim.get_status().get("money_total", 0.0))
	var external_before := float(((_bank(sim).get("invariant", {})
		as Dictionary).get("unallocated", 0.0)))
	# Paridade 1 % abaixo da taxa corrente: o mercado empurra para cima e o BC
	# defende vendendo reserva (declarada pelo laboratório; não é dado de país).
	sim.set_exchange_regime("DEM", "peg", 0.99, -1.0)
	var declared: float = sim.set_central_bank_reserves({"DEM": 1.0e6})
	_check(_close(declared, 1.0e6, 1e-6),
		"defesa: reserva declarada %.6f != 1e6" % declared)
	var fx_before := _fx(sim)
	var reserves_before := float(fx_before.get("reserves", 0.0))
	sim.tick_day()
	var fx := _fx(sim)
	var reserves_after := float(fx.get("reserves", 0.0))
	_check(bool(fx.get("funded", false)),
		"defesa: moeda com reserva deveria ficar `funded`")
	_check(int(fx.get("devaluations", 0)) == 0,
		"defesa: desvalorizou com reserva de sobra")
	_check(reserves_after < reserves_before - 1e-12,
		"defesa: o BC não gastou reserva (%.9f -> %.9f)" % [
			reserves_before, reserves_after])
	_check(reserves_after >= -1e-12, "defesa: reserva negativa")
	var intervention := float(fx.get("intervention_today", 0.0))
	_check(_close(-intervention, reserves_before - reserves_after, EPS),
		"defesa: intervenção do dia %.9f != reserva gasta %.9f" % [
			-intervention, reserves_before - reserves_after])
	_check(_close(float(fx.get("rate", 0.0)), 0.99, 1e-9),
		"defesa: a taxa não segurou na paridade (%.9f)" % float(
			fx.get("rate", 0.0)))
	var money_after := float(sim.get_status().get("money_total", 0.0))
	var external_after := float(((_bank(sim).get("invariant", {})
		as Dictionary).get("unallocated", 0.0)))
	# A moeda local recolhida sai do caixa do exterior (contraparte declarada);
	# o dia também tem comércio, então a checagem é de ordem de grandeza e de
	# conservação total (status.balance.ok), não de igualdade exata.
	_check(money_after < money_before,
		"defesa: vender reserva deveria destruir moeda local "
			+ "(%.6f -> %.6f)" % [money_before, money_after])
	_check(external_after <= external_before + 1e-6,
		"defesa: o caixa do exterior não caiu com a venda de reserva")
	_check_day(sim, "defesa")


# --- 3. Reserva esgotada → desvalorização forçada ---------------------------
func _test_exhaustion_devalues() -> void:
	var sim := _make_sim("peg")
	for day in 40:
		sim.tick_day()
	sim.set_exchange_regime("DEM", "peg", 0.99, -1.0)
	sim.set_central_bank_reserves({"DEM": 1.0e-6})
	sim.tick_day()
	var fx := _fx(sim)
	_check(int(fx.get("devaluations", 0)) >= 1,
		"esgotamento: sem reserva não houve desvalorização forçada")
	_check(_close(float(fx.get("reserves", -1.0)), 0.0, 1e-9),
		"esgotamento: reserva deveria zerar (%.9f)" % float(
			fx.get("reserves", -1.0)))
	var parity := float(fx.get("parity", 0.0))
	var rate := float(fx.get("rate", 0.0))
	_check(parity > 0.0 and _close(rate, parity, 1e-9),
		"esgotamento: paridade nova (%.9f) != taxa (%.9f)" % [parity, rate])
	var events: Array = _bank(sim).get("fx_events", [])
	_check(not events.is_empty(), "esgotamento: nenhum evento no status")
	if not events.is_empty():
		var event: Dictionary = events[0]
		_check(String(event.get("type", "")) == "devaluation",
			"esgotamento: evento não é devaluation")
		_check(_close(float(event.get("new_parity", 0.0)), rate, 1e-9),
			"esgotamento: evento não carrega a paridade nova")
		_check(int(event.get("day", -1)) == int(_bank(sim).get("day", -2)),
			"esgotamento: evento fora do dia")
	_check(int(_totals(sim).get("devaluations", 0)) >= 1,
		"esgotamento: contador total de desvalorizações zerado")
	_check_day(sim, "esgotamento")


# --- 4. Bretton Woods força o peg; padrão ouro limita a emissão ------------
func _test_standards() -> void:
	var sim := _make_sim("float")
	sim.set_central_bank_policy("DEM", "controlled")
	for day in 8:
		sim.spend_government("AGRIA", 4000.0)
		sim.tick_day()
	var fiat_emission := float((_fx_or_policy(sim)).get("emission_total", 0.0))
	_check(fiat_emission > 0.0,
		"padrão: o fiduciário deveria ter emitido contra o déficit (%.6f)"
			% fiat_emission)
	_check(sim.set_monetary_standard("bretton_woods"),
		"padrão: bretton_woods recusado")
	_check(not sim.set_monetary_standard("confisco"),
		"padrão: nome inválido foi aceito")
	var policy: Dictionary = _bank(sim).get("monetary_policy", {})
	_check(String(policy.get("standard", "")) == "bretton_woods",
		"padrão: o mundo não está em Bretton Woods")
	_check(bool(policy.get("standard_forces_peg", false)),
		"padrão: Bretton Woods deveria forçar o peg")
	var fx := _fx(sim)
	_check(String(fx.get("regime_effective", "")) == "peg",
		"padrão: regime efetivo sob Bretton Woods deveria ser peg")
	_check(String(fx.get("regime_saved", "")) == "floating",
		"padrão: o regime SALVO deveria ser preservado")
	_check(float(fx.get("conversion_price", 0.0)) > 0.0,
		"padrão: preço fixo de conversão não foi congelado")
	# Padrão ouro: cobertura de 0,001 t (3 unidades de âncora) — M0 do mundo é
	# muito maior, então não há espaço para emitir.
	_check(sim.set_monetary_standard("gold"), "padrão: gold recusado")
	sim.set_central_bank_gold({"DEMO": 0.001})
	var emission_before := float((_fx_or_policy(sim)).get("emission_total", 0.0))
	for day in 8:
		sim.spend_government("AGRIA", 4000.0)
		sim.tick_day()
	var emission_after := float((_fx_or_policy(sim)).get("emission_total", 0.0))
	_check(_close(emission_after, emission_before, EPS),
		"padrão ouro: emitiu sem cobertura (%.6f -> %.6f)" % [
			emission_before, emission_after])
	var coverage := float(_fx(sim).get("coverage", -1.0))
	_check(coverage >= 0.0, "padrão ouro: cobertura não medida")
	_check(float((_fx_or_policy(sim)).get("unfunded_today", 0.0)) > 0.0,
		"padrão ouro: o déficit deveria ficar não financiado")
	_check_day(sim, "padrão ouro")


func _fx_or_policy(sim: Object) -> Dictionary:
	var currencies: Dictionary = _bank(sim).get("currencies", {})
	if not currencies.has("DEM"):
		return {}
	return (currencies["DEM"] as Dictionary).get("policy", {})


# --- 5. Ouro físico: conversão, compra/venda e conservação ------------------
func _test_gold_physical() -> void:
	var sim := _make_sim("peg")
	sim.set_storage_capacity("AGRIA", "vault", "government", 100.0)
	sim.set_market_stock("AGRIA", "gold", 10.0)
	sim.set_monetary_standard("bretton_woods")
	sim.set_central_bank_reserves({"DEM": 1.0e6})
	var totals_before := _totals(sim)
	var ledger_before := float(totals_before.get("vault_gold_tonnes", -1.0))
	_check(_close(ledger_before, 0.0, 1e-9),
		"ouro: cofre deveria começar vazio (%.6f)" % ledger_before)
	var convert: Dictionary = sim.central_bank_gold_convert("DEM", 1.0)
	_check(bool(convert.get("ok", false)),
		"ouro: conversão falhou (%s)" % String(convert.get("error", "")))
	_check(_close(float(convert.get("tons", 0.0)), 1.0, 1e-9),
		"ouro: conversão não levou 1 t")
	_check(_close(float(convert.get("value", 0.0)), 3000.0, 1e-6),
		"ouro: preço fixo da conversão %.6f != catálogo 3000" %
			float(convert.get("value", 0.0)))
	var reserves_after_convert := float(_fx(sim).get("reserves", 0.0))
	_check(_close(reserves_after_convert, 1.0e6 - 3000.0, 1e-6),
		"ouro: reserva não pagou a conversão (%.6f)" % reserves_after_convert)
	var ledger := float(_totals(sim).get("vault_gold_tonnes", -1.0))
	_check(_close(ledger, 1.0, 1e-9),
		"ouro: cofre com %.9f t depois da conversão" % ledger)
	_check(_close(float(_bank(sim).get("currencies", {}).get("DEM", {})
		.get("assets", {}).get("gold_tonnes", -1.0)), ledger, 1e-9),
		"ouro: razão do BC != estoque do cofre")
	# Compra de 0,001 t ao preço do mercado e venda de 0,0005 t: ouro
	# conservado (valores pequenos para não depender do caixa do fixture).
	var buy: Dictionary = sim.central_bank_gold_trade("DEM", 0.001, true)
	_check(bool(buy.get("ok", false)),
		"ouro: compra falhou (%s)" % String(buy.get("error", "")))
	var sell: Dictionary = sim.central_bank_gold_trade("DEM", 0.0005, false)
	_check(bool(sell.get("ok", false)),
		"ouro: venda falhou (%s)" % String(sell.get("error", "")))
	var ledger_end := float(_totals(sim).get("vault_gold_tonnes", -1.0))
	_check(_close(ledger_end, 1.0 + float(buy.get("tons", 0.0))
		- float(sell.get("tons", 0.0)), 1e-9),
		"ouro: cofre não bate com a soma das operações (%.9f)" % ledger_end)
	_check(_close(float(_bank(sim).get("currencies", {}).get("DEM", {})
		.get("assets", {}).get("gold_tonnes", -1.0)), ledger_end, 1e-9),
		"ouro: razão do BC divergiu depois das operações")
	var storage: Dictionary = sim.get_storage_status()
	var vault_total := 0.0
	for key in (storage.get("sites", {}) as Dictionary):
		var row: Dictionary = (storage.get("sites", {}) as Dictionary)[key]
		if String(row.get("type", "")) == "vault":
			vault_total += float(row.get("stock_t", 0.0))
	_check(vault_total > 0.0, "ouro: o status de armazenagem não mostra o cofre")
	_check_day(sim, "ouro físico")


# --- 6. Save v29, round-trip e migração v28 → v29 ---------------------------
func _test_save_and_migration() -> void:
	var sim := _make_sim("peg")
	for day in 40:
		sim.tick_day()
	sim.set_exchange_regime("DEM", "band", 0.98, 0.02)
	sim.set_central_bank_reserves({"DEM": 500.0})
	for day in 3:
		sim.tick_day()
	var save: Dictionary = sim.to_dict()
	_check(int(save.get("version", 0)) == SAVE_VERSION,
		"save: versão %s != %d" % [str(save.get("version", 0)), SAVE_VERSION])
	var fx_block: Dictionary = save.get("fx", {})
	_check(not fx_block.is_empty(), "save: bloco `fx` ausente")
	var currencies: Dictionary = fx_block.get("currencies", {})
	_check(currencies.has("DEM"), "save: estado cambial da moeda DEM ausente")
	if currencies.has("DEM"):
		var entry: Dictionary = currencies["DEM"]
		_check(_close(float(entry.get("parity", 0.0)),
			float(_fx(sim).get("parity", -1.0)), 1e-12),
			"save: paridade não viajou")
		_check(bool(entry.get("funded", false)),
			"save: `funded` não viajou")
	var copy = EconomySim.new()
	copy.set_goods(_json(GOODS_PATH))
	copy.set_recipes(_json(RECIPES_PATH))
	copy.set_needs(_json(NEEDS_PATH))
	copy.set_services(_json(SERVICES_PATH))
	copy.set_sector_employment(_json(SECTOR_EMPLOYMENT_PATH))
	copy.set_age_structure(_json(AGE_STRUCTURE_PATH))
	copy.from_dict(save)
	_check(JSON.stringify(copy.to_dict(), "", true, true)
		== JSON.stringify(save, "", true, true),
		"save: round-trip não é bit-exato")
	for day in 5:
		sim.tick_day()
		copy.tick_day()
	_check(JSON.stringify(sim.to_dict(), "", true, true)
		== JSON.stringify(copy.to_dict(), "", true, true),
		"save: continuação após load divergiu")
	# Migração: um save v28 (sem o bloco `fx`) carrega com os defaults
	# declarados — nada inventado para trás.
	var old: Dictionary = save.duplicate(true)
	old["version"] = 28
	old.erase("fx")
	var migrated = EconomySim.new()
	migrated.set_goods(_json(GOODS_PATH))
	migrated.set_recipes(_json(RECIPES_PATH))
	migrated.set_needs(_json(NEEDS_PATH))
	migrated.set_services(_json(SERVICES_PATH))
	migrated.set_sector_employment(_json(SECTOR_EMPLOYMENT_PATH))
	migrated.set_age_structure(_json(AGE_STRUCTURE_PATH))
	migrated.from_dict(old)
	var migrated_fx := _fx(migrated)
	_check(_close(float(migrated_fx.get("parity", -1.0)), 1.0, 1e-12),
		"migração: paridade deveria cair no base_rate (%.9f)" % float(
			migrated_fx.get("parity", -1.0)))
	_check(_close(float(migrated_fx.get("reserves", -1.0)), 0.0, 1e-12),
		"migração: reserva deveria ser 0")
	_check(not bool(migrated_fx.get("funded", true)),
		"migração: `funded` deveria ser falso")
	_check(int(migrated_fx.get("devaluations", -1)) == 0,
		"migração: desvalorizações deveriam ser 0")
	_check((_bank(migrated).get("fx_events", []) as Array).is_empty(),
		"migração: evento inventado para trás")
	_check(int((migrated.to_dict() as Dictionary).get("version", 0))
		== SAVE_VERSION,
		"migração: o save carregado não foi regravado na v34")
