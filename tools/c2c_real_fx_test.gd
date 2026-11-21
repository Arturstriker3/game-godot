extends SceneTree

## C2c (D-108) — REGIMES CAMBIAIS REAIS, ÂNCORA, CESTA E MOEDA DE UNIÃO.
##
## Prova no cenário de fixture (nação DEMO; moedas sintéticas com os mesmos
## campos do `fx_regimes_2026.json`):
##   1. REGIME REAL: o regime inicial vem do dado (peg/floating), a âncora é
##      uma moeda QUALQUER (não só a de reserva) e a paridade é CRUZADA.
##   2. PEG CRUZADO: quando a moeda-âncora se move, o peg acompanha (taxa =
##      âncora ÷ paridade) em vez de ficar presa no XGS.
##   3. CESTA: alvo = média GEOMÉTRICA PONDERADA das taxas cruzadas; os pesos
##      viajam no status e no save.
##   4. EMISSOR/UNIÃO: só o emissor muda o regime; moeda compartilhada fica
##      travada ("moeda de união — regras de bloco futuras").
##   5. BULGÁRIA (D-108e): BGN fundida no EUR a 1 EUR = 1,95583 BGN conserva o
##      dinheiro (invariante de conservação fecha) e a paridade local.
##   6. SAVE v31: bloco `fx_regimes`, round-trip bit-exato e migração v30 → v31
##      (o save antigo recebe os regimes do arquivo).
##
## Roda pelo runner: powershell -File tools/run_tests.ps1 -List c2c_real_fx_test

const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"
const SERVICES_PATH := "res://data/definitions/economy/services.json"
const SECTOR_EMPLOYMENT_PATH := "res://data/definitions/economy/sector_employment.json"
const AGE_STRUCTURE_PATH := "res://data/definitions/population/age_structure_2026.json"

const SAVE_VERSION := 32
const EPS := 1e-6

var _failures: Array[String] = []


func _init() -> void:
	_test_peg_cross()
	_test_basket()
	_test_issuer_and_union()
	_test_bulgaria_succession()
	_test_save_and_migration()
	if _failures.is_empty():
		print("[c2c] OK: peg cruzado, cesta, emissor/uniao travada, BGN->EUR "
			+ "conservando valor, save v31 e migracao v30->v31.")
		quit(0)
	else:
		print("[c2c] FALHAS (%d):" % _failures.size())
		for failure: String in _failures:
			print(" - " + failure)
		quit(1)


func _fail(message: String) -> void:
	_failures.append(message)


func _check(cond: bool, message: String) -> void:
	if not cond:
		_fail(message)


func _close(a: float, b: float, tolerance: float = EPS) -> bool:
	return absf(a - b) <= tolerance


func _json(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("não abriu " + path)
		return null
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data


func _currencies(with_bgn: bool = false) -> Dictionary:
	var list: Array = [
			{"code": "DEM", "nations": ["DEMO"], "rate_per_anchor": 1.0,
				"source": "fixture"},
			{"code": "ANC", "nations": ["ANNA"], "rate_per_anchor": 2.0,
				"source": "fixture"},
			{"code": "BBB", "nations": ["BETA"], "rate_per_anchor": 1.0,
				"source": "fixture"},
			{"code": "AAA", "nations": ["ALFA"], "rate_per_anchor": 1.0,
				"source": "fixture"},
			{"code": "UNI", "nations": ["UUU", "VVV"],
				"rate_per_anchor": 1.0, "source": "fixture"},
			{"code": "EUR", "nations": ["EURO"], "rate_per_anchor": 0.66,
				"source": "fixture"},
	]
	if with_bgn:
		list.append({"code": "BGN", "nations": ["BULG"],
			"rate_per_anchor": 0.49, "source": "fixture"})
	return {"regime": null, "currencies": list}


func _regimes(with_bgn: bool = false) -> Dictionary:
	# Mesmo formato do data/definitions/economy/fx_regimes_2026.json.
	var rows: Dictionary = {
			"DEM": {"game_regime": "peg", "anchor": "ANC",
				"anchor_type": "currency", "nations": ["DEMO"],
				"shared_currency": false, "source": "fixture AREAER"},
			"ANC": {"game_regime": "floating", "anchor": null,
				"anchor_type": "none", "nations": ["ANNA"],
				"shared_currency": false, "source": "fixture AREAER"},
			"BBB": {"game_regime": "floating", "anchor": null,
				"anchor_type": "none", "nations": ["BETA"],
				"shared_currency": false, "source": "fixture AREAER"},
			"AAA": {"game_regime": "peg", "anchor": null,
				"anchor_type": "composite", "nations": ["ALFA"],
				"shared_currency": false, "source": "fixture AREAER",
				"basket": {"ANC": 0.5, "BBB": 0.5}},
			"UNI": {"game_regime": "floating", "anchor": null,
				"anchor_type": "none", "nations": ["UUU", "VVV"],
				"shared_currency": true, "source": "fixture AREAER"},
			"EUR": {"game_regime": "floating", "anchor": null,
				"anchor_type": "none", "nations": ["EURO"],
				"shared_currency": false, "source": "fixture AREAER"},
	}
	if with_bgn:
		rows["BGN"] = {"game_regime": "peg", "anchor": "EUR",
			"anchor_type": "currency", "nations": ["BULG"],
			"shared_currency": false, "source": "fixture AREAER"}
	return {"title": "fixture AREAER", "as_of": "2026-04-30",
		"currencies": rows}


func _make_sim(with_regimes: bool = true, with_bgn: bool = false) -> Object:
	var sim = EconomySim.new()
	sim.set_goods(_json(GOODS_PATH))
	sim.set_recipes(_json(RECIPES_PATH))
	sim.set_needs(_json(NEEDS_PATH))
	sim.set_services(_json(SERVICES_PATH))
	sim.set_sector_employment(_json(SECTOR_EMPLOYMENT_PATH))
	sim.set_age_structure(_json(AGE_STRUCTURE_PATH))
	if with_regimes and sim.has_method("set_fx_regimes"):
		sim.set_fx_regimes(_regimes(with_bgn))
	sim.set_currencies(_currencies(with_bgn))
	sim.initialize_scenario()
	return sim


func _fx(sim: Object, code: String) -> Dictionary:
	var currencies: Dictionary = sim.get_central_bank_status().get(
		"currencies", {})
	if not currencies.has(code):
		return {}
	return (currencies[code] as Dictionary).get("fx", {})


func _money(sim: Object) -> Dictionary:
	return sim.get_central_bank_status().get("invariant", {})


# --- 1. Regime real e peg CRUZADO ------------------------------------------
func _test_peg_cross() -> void:
	var sim := _make_sim()
	var fx := _fx(sim, "DEM")
	_check(String(fx.get("regime_saved", "")) == "peg",
		"peg: regime do arquivo não chegou (%s)" % fx.get("regime_saved", ""))
	_check(String(fx.get("anchor", "")) == "ANC",
		"peg: âncora deveria ser ANC (%s)" % fx.get("anchor", ""))
	_check(String(fx.get("issuer", "")) == "DEMO",
		"peg: emissor deveria ser DEMO (%s)" % fx.get("issuer", ""))
	_check(not bool(fx.get("locked", true)), "peg: moeda de um país só não trava")
	# Paridade CRUZADA: âncora 2,0 / base 1,0 = 2,0 (taxa nasce igual ao F5).
	_check(_close(float(fx.get("parity", 0.0)), 2.0, 1e-9),
		"peg: paridade cruzada deveria ser 2,0 (%s)" % str(fx.get("parity")))
	_check(_close(float(fx.get("target_rate", 0.0)), 1.0, 1e-9),
		"peg: alvo deveria ser a taxa inicial 1,0 (%s)" % str(
			fx.get("target_rate")))
	# A âncora se move (4,0): o peg tem de acompanhar (alvo 2,0).
	sim.set_currency_rate("ANC", 4.0, false)
	sim.tick_day()
	fx = _fx(sim, "DEM")
	_check(_close(float(fx.get("anchor_value", 0.0)), 4.0, 1e-9),
		"peg: valor da âncora deveria ser 4,0 (%s)" % str(
			fx.get("anchor_value")))
	_check(_close(float(fx.get("rate", 0.0)), 2.0, 1e-9),
		"peg: taxa deveria seguir a âncora (2,0), veio %s" % str(
			fx.get("rate")))
	var balance: Dictionary = sim.get_status().get("balance", {})
	_check(bool(balance.get("ok", false)),
		"peg: invariante de conservação quebrou (residual %s)" % str(
			balance.get("cash_residual")))


# --- 2. Cesta: média geométrica ponderada ----------------------------------
func _test_basket() -> void:
	var sim := _make_sim()
	var fx := _fx(sim, "AAA")
	_check(String(fx.get("anchor_type", "")) == "composite",
		"cesta: âncora deveria ser composite (%s)" % fx.get("anchor_type", ""))
	var basket: Dictionary = fx.get("anchor_basket", {})
	_check(basket.size() == 2 and basket.has("ANC") and basket.has("BBB"),
		"cesta: pesos não viajaram para o status (%s)" % str(basket))
	_check(_close(float(basket.get("ANC", 0.0)), 0.5, 1e-12),
		"cesta: peso de ANC deveria ser 0,5")
	# ANC = 2, BBB = 1 -> média geométrica 1,41421356; base 1,0.
	var expected_anchor := sqrt(2.0 * 1.0)
	_check(_close(float(fx.get("anchor_value", 0.0)), expected_anchor, 1e-9),
		"cesta: valor da cesta %.9f != %.9f" % [
			float(fx.get("anchor_value", 0.0)), expected_anchor])
	var parity := float(fx.get("parity", 0.0))
	_check(_close(parity, expected_anchor, 1e-9),
		"cesta: paridade cruzada deveria ser %.9f (%s)" % [
			expected_anchor, str(parity)])
	# Uma perna da cesta dobra: o alvo acompanha a média geométrica.
	sim.set_currency_rate("ANC", 4.0, false)
	sim.tick_day()
	fx = _fx(sim, "AAA")
	_check(_close(float(fx.get("anchor_value", 0.0)), sqrt(4.0 * 1.0), 1e-9),
		"cesta: média geométrica deveria ser 2,0 (%s)" % str(
			fx.get("anchor_value")))
	_check(_close(float(fx.get("rate", 0.0)),
		sqrt(4.0 * 1.0) / parity, 1e-9),
		"cesta: taxa deveria ser 2,0/paridade (%s)" % str(fx.get("rate")))


# --- 3. Emissor e moeda de união -------------------------------------------
func _test_issuer_and_union() -> void:
	var sim := _make_sim()
	# Não-emissor: recusado (só DEMO emite DEM).
	_check(not sim.set_exchange_regime("DEM", "band", 0.0, 0.02, "ANNA"),
		"emissor: comando de outra nação foi aceito")
	# Emissor: aceito e aplicado no dia seguinte (A3).
	_check(sim.set_exchange_regime("DEM", "band", 0.0, 0.02, "DEMO"),
		"emissor: comando do próprio emissor foi recusado")
	sim.tick_day()
	_check(String(_fx(sim, "DEM").get("regime_saved", "")) == "band",
		"emissor: regime não virou band")
	# União monetária: travada, ninguém muda.
	var fx_uni := _fx(sim, "UNI")
	_check(bool(fx_uni.get("locked", false)),
		"união: moeda compartilhada deveria estar travada")
	_check(not sim.set_exchange_regime("UNI", "peg", 0.0, -1.0),
		"união: comando de regime foi aceito numa moeda de união")
	# Moeda de um país só continua aceitando.
	_check(sim.set_exchange_regime("BBB", "band", 0.0, 0.02),
		"emissor: moeda de um país só recusou um pedido válido")


# --- 4. Bulgária no euro: conversão conserva valor -------------------------
func _test_bulgaria_succession() -> void:
	var sim := _make_sim(true, true)
	for day in 3:
		sim.tick_day()
	var before := _money(sim)
	# Paridade local declarada da moeda extinta (unidades de BGN por âncora).
	_check(sim.set_exchange_regime("BGN", "peg", 3.0, -1.0),
		"BGN: comando de paridade recusado")
	sim.tick_day()
	_check(_close(float(_fx(sim, "BGN").get("parity", 0.0)), 3.0, 1e-9),
		"BGN: paridade declarada não aplicou")
	var converted: float = sim.migrate_currency("BGN", "EUR", 1.95583)
	_check(_close(converted, 3.0, 1e-9),
		"BGN: conversão deveria devolver a paridade 3,0 (%s)" % str(converted))
	var after := _money(sim)
	_check(_close(float(after.get("money_total", 0.0)),
		float(before.get("money_total", -1.0)), 1e-6),
		"BGN: dinheiro total mudou na conversão (%.9f -> %.9f)" % [
			float(before.get("money_total", 0.0)),
			float(after.get("money_total", 0.0))])
	_check(bool(after.get("ok", false)),
		"BGN: invariante não fecha após a sucessão")
	var status: Dictionary = sim.get_central_bank_status()
	var currencies: Dictionary = status.get("currencies", {})
	_check(not currencies.has("BGN"), "BGN: moeda extinta continua no status")
	_check(currencies.has("EUR"), "BGN: moeda destino sumiu")
	if currencies.has("EUR"):
		var nations: Array = (currencies["EUR"] as Dictionary).get("nations", [])
		_check(nations.has("BULG"),
			"BGN: a nação emissora não passou para o EUR (%s)" % str(nations))
	var fx_eur := _fx(sim, "EUR")
	_check(_close(float(fx_eur.get("parity", 0.0)), 3.0 / 1.95583, 1e-9),
		"BGN: paridade local não foi convertida a 1,95583 (%s)" % str(
			fx_eur.get("parity")))
	_check(bool(sim.get_status().get("balance", {}).get("ok", false)),
		"BGN: balanço financeiro não fecha após a sucessão")


# --- 5. Save v31, round-trip e migração v30 -> v31 -------------------------
func _test_save_and_migration() -> void:
	var sim := _make_sim()
	for day in 5:
		sim.tick_day()
	var save: Dictionary = sim.to_dict()
	_check(int(save.get("version", 0)) == SAVE_VERSION,
		"save: versão %s != %d" % [str(save.get("version", 0)),
			SAVE_VERSION])
	var block: Dictionary = save.get("fx_regimes", {})
	_check(not block.is_empty(), "save: bloco `fx_regimes` ausente")
	var saved: Dictionary = block.get("currencies", {})
	_check(saved.has("DEM"), "save: regime da moeda DEM ausente")
	if saved.has("DEM"):
		var row: Dictionary = saved["DEM"]
		_check(String(row.get("anchor", "")) == "ANC",
			"save: âncora não viajou (%s)" % row.get("anchor", ""))
		_check(String(row.get("regime", "")) == "peg",
			"save: regime não viajou (%s)" % row.get("regime", ""))
	var copy = _bare_sim()
	copy.from_dict(save)
	_check(JSON.stringify(copy.to_dict(), "", true, true)
		== JSON.stringify(save, "", true, true),
		"save: round-trip v31 não é bit-exato")
	var fx_copy := _fx(copy, "DEM")
	_check(String(fx_copy.get("anchor", "")) == "ANC",
		"save: âncora não voltou no load")
	_check(_close(float(fx_copy.get("parity", 0.0)),
		float(_fx(sim, "DEM").get("parity", -1.0)), 1e-12),
		"save: paridade não voltou no load")
	for day in 3:
		sim.tick_day()
		copy.tick_day()
	_check(JSON.stringify(sim.to_dict(), "", true, true)
		== JSON.stringify(copy.to_dict(), "", true, true),
		"save: continuação após load divergiu")
	# Regime COMANDADO (D-108c) precisa sobreviver ao save/load: o load
	# restaura `exchange_regime` a partir de `fx_regimes`, então o comando
	# tem de escrever os dois lados.
	var commanded = _make_sim()
	_check(commanded.set_exchange_regime("BBB", "band", 0.0, 0.02),
		"save: comando de regime recusado na moeda BBB")
	commanded.tick_day()
	var commanded_save: Dictionary = commanded.to_dict()
	var commanded_copy = _bare_sim()
	commanded_copy.from_dict(commanded_save)
	_check(String(_fx(commanded_copy, "BBB").get("regime_saved", "")) == "band",
		"save: regime comandado band não sobreviveu ao load (%s)" % _fx(
			commanded_copy, "BBB").get("regime_saved", ""))
	_check(JSON.stringify(commanded_copy.to_dict(), "", true, true)
		== JSON.stringify(commanded_save, "", true, true),
		"save: round-trip de regime comandado não é bit-exato")
	for day in 3:
		commanded.tick_day()
		commanded_copy.tick_day()
	_check(JSON.stringify(commanded.to_dict(), "", true, true)
		== JSON.stringify(commanded_copy.to_dict(), "", true, true),
		"save: continuação com regime comandado divergiu")
	# Migração v30 -> v31: o save antigo (sem `fx_regimes`, com o "peg
	# administrativo" em toda moeda) recebe os regimes do arquivo.
	var legacy := _make_sim(true, true)
	for day in 2:
		legacy.tick_day()
	var old: Dictionary = legacy.to_dict()
	old["version"] = 30
	old.erase("fx_regimes")
	var old_currencies: Array = old.get("currencies", [])
	for index in old_currencies.size():
		(old_currencies[index] as Dictionary)["exchange_regime"] = "peg"
	var migrated = _bare_sim()
	migrated.set_fx_regimes(_regimes())
	migrated.from_dict(old)
	_check(int(migrated.to_dict().get("version", 0)) == SAVE_VERSION,
		"migração: o save carregado não foi regravado na v31")
	_check(String(_fx(migrated, "ANC").get("regime_saved", "")) == "floating",
		"migração: ANC deveria virar floating pelo arquivo (%s)" % _fx(
			migrated, "ANC").get("regime_saved", ""))
	_check(String(_fx(migrated, "DEM").get("regime_saved", "")) == "peg",
		"migração: DEM deveria continuar peg pelo arquivo")
	_check(String(_fx(migrated, "AAA").get("anchor_type", "")) == "composite",
		"migração: AAA deveria carregar a cesta")
	var migrated_eur := _fx(migrated, "EUR")
	var migrated_currencies: Dictionary = migrated.get_central_bank_status().get(
		"currencies", {})
	_check(not migrated_currencies.has("BGN"),
		"migração: BGN deveria ter sido fundida no EUR")
	_check((migrated_currencies.get("EUR", {}) as Dictionary).get(
		"nations", []).has("BULG"),
		"migração: a nação BULG não passou para o EUR")
	_check(not bool(migrated_eur.get("locked", true)),
		"migração: EUR de um país só não deveria travar")


func _bare_sim() -> Object:
	var sim = EconomySim.new()
	sim.set_goods(_json(GOODS_PATH))
	sim.set_recipes(_json(RECIPES_PATH))
	sim.set_needs(_json(NEEDS_PATH))
	sim.set_services(_json(SERVICES_PATH))
	sim.set_sector_employment(_json(SECTOR_EMPLOYMENT_PATH))
	sim.set_age_structure(_json(AGE_STRUCTURE_PATH))
	return sim
