extends SceneTree

## PARTE E / E5 (D-117) — COMÉRCIO DE ENERGIA ENTRE REDES NACIONAIS VIZINHAS.
##
## Mundo sintético com três nações (AAA, BBB, CCC), uma cidade cada, e o
## parque/regra DECLARADOS pelo teste via `set_power_definitions` (o fixture
## não tem dado público de energia):
##
##   * AAA = importadora (carvão pequeno, demanda alta);
##   * BBB = exportadora (hidro com sobra);
##   * CCC = terceira rede nacional (para a obra de interconexão nova).
##
## Aceites:
##   (1) linha só existe entre redes NACIONAIS e o seed escala a capacidade;
##   (2) despacho diário: o exportador gera o excedente, o importador recebe
##       `enviado × (1 - perda)` — conservação de ENERGIA;
##   (3) preço de fronteira = custo marginal do exportador ÷ (1 - perda) e o
##       importador paga (valor = recebido × preço); dinheiro conservado;
##   (4) a capacidade da linha limita o fluxo e o racionamento continua;
##   (5) o importado fecha o déficit e tira o preço de escassez;
##   (6) cidade/rede LOCAL (isolada) não comercia (D-116);
##   (7) `build_interconnect` é obra (custo, fila) e conclui criando/somando
##       capacidade ao par;
##   (8) save v36: round-trip do status de energia (com as linhas), continuação
##       idêntica e o dado do ano-base não sobrescreve o save;
##   (9) determinismo: duas entradas iguais → mesmo status.
##
## Corte por HOSTILIDADE não é testado: o mundo não modela relações/diplomacia
## (`tpl.dossie.relations.note`), então não há API para cortar a linha. O gancho
## está declarado como TODO em sim/power.cpp (bloco do comércio da E5).
##
## Roda pelo runner: powershell -File tools/run_tests.ps1 -List e5_interconnect_test

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
const MONEY_EPSILON := 1e-3
const SAVE_VERSION := 36
const POPULATION := 50000.0

var _failures: Array[String] = []


func _init() -> void:
	_test_seed_only_national()
	_test_trade_energy_and_price()
	_test_capacity_limits_flow()
	_test_import_removes_scarcity()
	_test_isolated_does_not_trade()
	_test_build_interconnect_work()
	_test_save_v36_roundtrip()
	_test_determinism()
	if _failures.is_empty():
		print("[e5] OK: linha só entre redes nacionais, fluxo e preço de "
			+ "fronteira, conservação de energia/dinheiro, limite da linha, "
			+ "fim da escassez, isolada sem comércio, obra de interconexão, "
			+ "save v36 e determinismo.")
		quit(0)
	else:
		print("[e5] FALHAS (%d):" % _failures.size())
		for failure: String in _failures:
			print(" - " + failure)
		quit(1)


# --- (1) seed: só redes nacionais -----------------------------------------
func _test_seed_only_national() -> void:
	var sim = _world(_parks())
	# Nenhuma interconexão declarada ainda: o dado é aplicado pelo serviço.
	var status: Dictionary = sim.get_power_status()
	_check("(1) mundo sem dado de linhas começa com 0 interconexões",
		(status.get("interconnects", []) as Array).is_empty(),
		"links=%d" % (status.get("interconnects", []) as Array).size())
	# Roda o serviço de energia (mesmo caminho do jogo) para semear o ano-base.
	var seeded := int(sim.set_power_interconnects([
		{"a": "AAA", "b": "BBB", "importer": "AAA", "capacity_mw": 100.0},
		{"a": "AAA", "b": "ZZZ", "importer": "AAA", "capacity_mw": 50.0},
	], true))
	_check("(1) só o par com duas redes nacionais é semeado", seeded == 1,
		"semeadas=%d" % seeded)
	var rows: Array = sim.get_power_status().get("interconnects", [])
	_check("(1) a linha semeada é a do par AAA-BBB", rows.size() == 1
			and String((rows[0] as Dictionary).get("a", "")) == "AAA"
			and String((rows[0] as Dictionary).get("b", "")) == "BBB",
		"rows=%s" % JSON.stringify(rows))


# --- (2)/(3) comércio: energia, preço e dinheiro ---------------------------
func _test_trade_energy_and_price() -> void:
	var params := _params()
	var loss := _param(params, "loss_floor_pct") / 100.0
	var sim = _world(_parks())
	sim.set_power_interconnects([
		{"a": "AAA", "b": "BBB", "importer": "AAA", "capacity_mw": 500.0},
	], true)
	var money_before := float(sim.get_status().get("money_total", 0.0))
	sim.tick_day()
	var power: Dictionary = sim.get_power_status()
	var rows: Array = power.get("interconnects", [])
	_check("(2) a linha comercializa no dia", rows.size() == 1
			and float((rows[0] as Dictionary).get("flow_mwh", 0.0)) > 0.0,
		"rows=%s" % JSON.stringify(rows))
	var line: Dictionary = rows[0]
	var flow := float(line.get("flow_mwh", 0.0))
	var sent := float(line.get("sent_mwh", 0.0))
	var border := float(line.get("price", 0.0))
	var value := float(line.get("value", 0.0))
	_check("(2) o importador é AAA e o exportador é BBB",
		String(line.get("importer", "")) == "AAA"
			and String(line.get("exporter", "")) == "BBB",
		"%s <- %s" % [line.get("importer"), line.get("exporter")])
	_check("(2) energia conservada: recebido = enviado × (1 - perda)",
		absf(flow - sent * (1.0 - loss)) <= 1e-3,
		"recebido=%.6f enviado=%.6f perda=%.4f" % [flow, sent, loss])
	var aaa: Dictionary = _grid(sim, "AAA")
	var bbb: Dictionary = _grid(sim, "BBB")
	_check("(2) o importador recebe exatamente o fluxo da linha",
		absf(float(aaa.get("import_mwh", 0.0)) - flow) <= 1e-6
			and absf(float(bbb.get("export_mwh", 0.0)) - sent) <= 1e-6,
		"AAA importa=%.6f BBB exporta=%.6f" % [
			float(aaa.get("import_mwh", 0.0)),
			float(bbb.get("export_mwh", 0.0))])
	# Preço: a hidro do BBB é a última usina do despacho de exportação; o preço
	# de fronteira é o O&M dela dividido por (1 - perda). D-118: o O&M do JSON
	# está em SALÁRIO-DIA; a âncora do mundo (publicada no status) converte.
	var anchor := float(power.get("salary_day_xgs", 0.0))
	_check("(3) D-118: âncora salário-dia derivada do mundo", anchor > 0.0,
		"salary_day_xgs=%.9f" % anchor)
	var opex_hydro := _param(params, "opex_per_mwh_hydro") * anchor
	var expected := opex_hydro / (1.0 - loss)
	_check("(3) preço de fronteira = marginal do exportador ÷ (1 - perda)",
		absf(border - expected) <= 1e-6,
		"fronteira=%.6f esperado=%.6f" % [border, expected])
	_check("(3) valor pago = recebido × preço de fronteira",
		absf(value - flow * border) <= 1e-6,
		"valor=%.6f calc=%.6f" % [value, flow * border])
	_check("(3) os dois lados do ledger batem (import_value = export_value)",
		absf(float(aaa.get("import_value", 0.0))
			- float(bbb.get("export_value", 0.0))) <= 1e-6
			and absf(float(aaa.get("import_value", 0.0)) - value) <= 1e-6,
		"AAA=%.6f BBB=%.6f" % [float(aaa.get("import_value", 0.0)),
			float(bbb.get("export_value", 0.0))])
	var money_after := float(sim.get_status().get("money_total", 0.0))
	_check("(3) dinheiro conservado no dia do comércio",
		absf(money_after - money_before) <= MONEY_EPSILON,
		"%.6f -> %.6f" % [money_before, money_after])
	# A exportação é GERAÇÃO de verdade: aparece na fonte do BBB.
	var by_fuel: Dictionary = bbb.get("generation_mwh_by_fuel", {})
	var hydro := float(by_fuel.get("hydro", 0.0))
	_check("(2) a hidro do exportador gerou o próprio serviço + o enviado",
		hydro >= sent - 1e-6,
		"hidro=%.6f enviado=%.6f" % [hydro, sent])


# --- (4) a capacidade da linha limita --------------------------------------
func _test_capacity_limits_flow() -> void:
	var params := _params()
	var loss := _param(params, "loss_floor_pct") / 100.0
	var capacity := 5.0
	var sim = _world(_parks())
	sim.set_power_interconnects([
		{"a": "AAA", "b": "BBB", "importer": "AAA", "capacity_mw": capacity},
	], true)
	sim.tick_day()
	var line: Dictionary = (sim.get_power_status().get(
		"interconnects", []) as Array)[0]
	var flow := float(line.get("flow_mwh", 0.0))
	_check("(4) fluxo limitado pela capacidade da linha",
		absf(flow - capacity * 24.0 * (1.0 - loss)) <= 1e-3,
		"fluxo=%.6f teto=%.6f" % [flow, capacity * 24.0 * (1.0 - loss)])
	var aaa: Dictionary = _grid(sim, "AAA")
	_check("(4) o importador continua racionando com a linha curta",
		float(aaa.get("served_ratio", 1.0)) < 0.995,
		"ratio=%.6f" % float(aaa.get("served_ratio", 1.0)))


# --- (5) o importado tira a escassez ---------------------------------------
func _test_import_removes_scarcity() -> void:
	var params := _params()
	var sim = _world(_parks())
	sim.set_power_interconnects([
		{"a": "AAA", "b": "BBB", "importer": "AAA", "capacity_mw": 500.0},
	], true)
	sim.tick_day()
	var power: Dictionary = sim.get_power_status()
	var ceiling := _param(params, "price_ceiling") \
		* float(power.get("salary_day_xgs", 0.0))
	var aaa: Dictionary = _grid(sim, "AAA")
	_check("(5) com a linha folgada o importador é servido",
		float(aaa.get("served_ratio", 0.0)) >= 0.995,
		"ratio=%.6f" % float(aaa.get("served_ratio", 0.0)))
	_check("(5) o preço de escassez (teto) sai quando o déficit fecha",
		float(aaa.get("price", ceiling)) < ceiling,
		"preço=%.6f teto=%.6f" % [float(aaa.get("price", 0.0)), ceiling])


# --- (6) rede local não comercia (D-116) -----------------------------------
func _test_isolated_does_not_trade() -> void:
	var sim = _world(_parks())
	sim.set_power_interconnects([
		{"a": "AAA", "b": "BBB", "importer": "AAA", "capacity_mw": 500.0},
	], true)
	# AAA vira cidade ISOLADA: a linha existe no mundo, mas a rede local não
	# participa do comércio.
	sim.set_power_city("AAA-ONE", 0, 0.0, 100.0)
	sim.tick_day()
	var line: Dictionary = (sim.get_power_status().get(
		"interconnects", []) as Array)[0]
	_check("(6) cidade isolada não importa energia",
		float(line.get("flow_mwh", 0.0)) <= EPS
			and String(line.get("importer", "")) == "",
		"fluxo=%.6f importador=%s" % [float(line.get("flow_mwh", 0.0)),
			line.get("importer")])


# --- (7) obra de interconexão nova -----------------------------------------
func _test_build_interconnect_work() -> void:
	var sim = _world(_parks())
	# Sem linha declarada entre AAA e CCC.
	sim.set_power_params({"build_days_line": 1.0})
	sim.set_instant_construction(true)
	for good: String in ["construction_material", "steel_structure",
			"electrical_material"]:
		sim.set_market_stock("AAA-ONE", good, 1000.0)
	var params := _params()
	var capex := _param(params, "capex_per_mw_line")
	var money_before := float(sim.get_status().get("money_total", 0.0))
	var built: Dictionary = sim.build_interconnect("AAA", "CCC", 10.0)
	_check("(7) obra de interconexão aceita",
		bool(built.get("started", false)), String(built.get("error", "")))
	_check("(7) obra de interconexão não cria nem destrói dinheiro",
		absf(float(sim.get_status().get("money_total", 0.0)) - money_before)
			<= MONEY_EPSILON,
		"%.6f -> %.6f" % [money_before,
			float(sim.get_status().get("money_total", 0.0))])
	# Uma segunda obra do mesmo par é recusada enquanto a primeira está na fila.
	var again: Dictionary = sim.build_interconnect("AAA", "CCC", 5.0)
	_check("(7) segunda obra do mesmo par é recusada em andamento",
		not bool(again.get("started", true))
			and String(again.get("error", "")) == "obra em andamento",
		String(again.get("error", "")))
	for day in 3:
		sim.tick_day()
	var rows: Array = sim.get_power_status().get("interconnects", [])
	var found := false
	var capacity := 0.0
	for raw: Variant in rows:
		var row: Dictionary = raw
		if String(row.get("a", "")) == "AAA" and String(row.get("b", "")) == "CCC":
			found = true
			capacity = float(row.get("capacity_mw", 0.0))
	_check("(7) a obra concluída cria a linha AAA-CCC com a capacidade",
		found and absf(capacity - 10.0) <= 1e-6,
		"found=%s capacity=%.6f (capex=%.2f)" % [found, capacity, capex])
	# Nações sem rede nacional: a obra é recusada (não se inventa rede).
	var invalid: Dictionary = sim.build_interconnect("AAA", "ZZZ", 10.0)
	_check("(7) nação sem rede nacional é recusada",
		not bool(invalid.get("started", true)),
		String(invalid.get("error", "")))


# --- (8) save v36 ----------------------------------------------------------
func _test_save_v36_roundtrip() -> void:
	var sim = _world(_parks())
	sim.set_power_interconnects([
		{"a": "AAA", "b": "BBB", "importer": "AAA", "capacity_mw": 500.0},
	], true)
	for day in 4:
		sim.tick_day()
	var data: Dictionary = sim.to_dict()
	_check("(8) save grava a versão v36",
		int(data.get("version", 0)) == SAVE_VERSION,
		"version=%s" % data.get("version"))
	var block: Variant = data.get("power")
	_check("(8) bloco de energia tem as interconexões prontas",
		block is Dictionary
			and (block as Dictionary).get("interconnects", []) is Array
			and ((block as Dictionary).get("interconnects", []) as Array).size() == 1
			and bool((block as Dictionary).get("interconnects_ready", false)),
		"block=%s" % str(block is Dictionary))
	var before := _canon_power(sim)
	var loaded = _load(data)
	_check("(8) round-trip bit-exato do status de energia com as linhas",
		before == _canon_power(loaded), "status divergiu após o load")
	# O dado do ano-base não sobrescreve o estado do save.
	var links_before := (loaded.get_power_status().get(
		"interconnects", []) as Array).size()
	var reapplied := int(loaded.set_power_interconnects([
		{"a": "AAA", "b": "BBB", "importer": "AAA", "capacity_mw": 999.0},
	], true))
	var links_after := (loaded.get_power_status().get(
		"interconnects", []) as Array).size()
	var capacity_after := float((loaded.get_power_status().get(
		"interconnects", []) as Array)[0].get("capacity_mw", 0.0))
	_check("(8) only_missing não re-semeia nem reescala as linhas do save",
		links_before == links_after and reapplied == 0
			and absf(capacity_after - 500.0) <= 1e-6,
		"links %d -> %d, semeadas=%d, capacidade=%.6f" % [links_before,
			links_after, reapplied, capacity_after])
	# Continuação idêntica ao caminho direto.
	sim.tick_day()
	loaded.tick_day()
	_check("(8) continuação idêntica ao caminho direto",
		before != _canon_power(sim) and _canon_power(sim) == _canon_power(loaded),
		"dia=%s/%s" % [_canon_power(sim).hash(), _canon_power(loaded).hash()])


# --- (9) determinismo ------------------------------------------------------
func _test_determinism() -> void:
	var a = _world(_parks())
	var b = _world(_parks())
	for sim in [a, b]:
		sim.set_power_interconnects([
			{"a": "AAA", "b": "BBB", "importer": "AAA", "capacity_mw": 500.0},
		], true)
		for day in 6:
			sim.tick_day()
	_check("(9) dois mundos iguais têm o mesmo status de energia",
		_canon_power(a) == _canon_power(b), "status divergiu")
	_check("(9) dinheiro e PIB batem campo a campo",
		float(a.get_status().get("money_total", 0.0))
			== float(b.get_status().get("money_total", 0.0))
			and float(_city_status(a, "AAA-ONE").get("gdp", 0.0))
			== float(_city_status(b, "AAA-ONE").get("gdp", 0.0)),
		"money=%s/%s" % [a.get_status().get("money_total"),
			b.get_status().get("money_total")])


# --- helpers ---------------------------------------------------------------
func _parks() -> Dictionary:
	# AAA: carvão pequeno (caro, insuficiente); BBB: hidro com sobra;
	# CCC: hidro pequena (só para existir rede nacional).
	return {
		"AAA-ONE": _city({"coal": 10.0}, {"coal": 40.0}),
		"BBB-ONE": _city({"hydro": 100.0}, {"hydro": 200.0}),
		"CCC-ONE": _city({"hydro": 5.0}, {"hydro": 10.0}),
	}


func _world(cities_power: Dictionary):
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
		"currencies": [
			{"code": "AUX", "name": "Aux", "symbol": "A",
				"nations": ["AAA"], "rate_per_anchor": 1.0, "rate": 1.0,
				"base_rate": 1.0},
			{"code": "BUX", "name": "Bux", "symbol": "B",
				"nations": ["BBB"], "rate_per_anchor": 1.0, "rate": 1.0,
				"base_rate": 1.0},
			{"code": "CUX", "name": "Cux", "symbol": "C",
				"nations": ["CCC"], "rate_per_anchor": 1.0, "rate": 1.0,
				"base_rate": 1.0},
		],
	})
	sim.initialize_world(_cities(), [], [])
	var nations := {
		"AAA": _nation(),
		"BBB": _nation(),
		"CCC": _nation(),
	}
	sim.set_power_definitions(nations, cities_power, {}, true)
	return sim


func _cities() -> Array:
	var rows: Array = []
	for index in 3:
		var tag: String = ["AAA", "BBB", "CCC"][index]
		rows.append({
			"id": "%s-ONE" % tag, "name": tag, "nation": tag,
			"home_nation": tag, "landmass": "north",
			"population": POPULATION, "simulation_population": POPULATION,
			"sim_population": POPULATION,
			"x": 2500.0 + index * 20.0, "y": 600.0,
		})
	return rows


func _nation(access := 100.0) -> Dictionary:
	return {
		"access_pct": access,
		"access_urban_pct": access,
		"access_rural_pct": access,
		"loss_pct": 4.0,
	}


func _city(park: Dictionary, cap: Dictionary = {}) -> Dictionary:
	return {
		"access_pct": 100.0,
		"loss_pct": 4.0,
		"park": park,
		"cap": cap,
		"solar_month": [],
	}


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


func _params() -> Dictionary:
	return (_json(POWER_PARAMS_PATH) as Dictionary).get("params", {})


func _param(params: Dictionary, key: String) -> float:
	return float(params.get(key, -1.0))


func _grid(sim: Object, tag: String) -> Dictionary:
	return (sim.get_power_status().get("nations", {}) as Dictionary).get(
		tag, {})


func _city_status(sim: Object, city_id: String) -> Dictionary:
	return sim.get_city_status(city_id)


func _canon_power(sim: Object) -> String:
	return JSON.stringify(sim.get_power_status(), "", true, true)


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
