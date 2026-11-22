extends SceneTree

## PARTE E / D-116 — ENERGIA ELÉTRICA: aceites da E2 (usinas/geração) e da
## E3 (rede, despacho, preço e consequências da falta).
##
## Cenário: fixture AGRIA/PORTUS (duas cidades, nação DEMO, moeda DEM). O
## parque, o acesso e as perdas são DECLARADOS pelo teste via
## `set_power_definitions` — o fixture não tem dado público de energia, então
## cada caso isola o mecanismo. Números de modelo vêm de
## `data/definitions/energy/power_params.json` (espelho de `sim/power.h`).
##
##   (1) mérito e preço marginal: a usina mais barata entra primeiro e a
##       ÚLTIMA ligada define o preço; demanda maior liga a mais cara.
##   (2) racionamento: fator industrial quase proporcional à fração servida
##       e desemprego sobe (o fixture não tem indústria; o emprego cai pelo
##       canal de serviços, que é o real empregador do cenário).
##   (3) blecaute longo pesa mais: a insatisfação (`stress`) cresce com a
##       duração em dias seguidos.
##   (4) o PIB contrai e o `lost_gdp` (VOLL declarado) aparece.
##   (5) combustível da térmica sai do mercado e entra na demanda do dia.
##   (6) conservação de dinheiro e soma financeira zero no setor de energia.
##   (7) cidade isolada × conexão parcial × conectada (limite da linha).
##   (8) obra de linha muda o estado da cidade para conectada.
##   (9) save v35: round-trip bit-exato, continuação idêntica e o dado de
##       energia NÃO sobrescreve o estado que veio do save.
##  (10) determinismo: mesma entrada → mesmo mundo (status de energia).
##  (11) ausência declarada: cidade sem dado não recebe racionamento fabricado.
##
## Roda pelo runner: powershell -File tools/run_tests.ps1 -List e2e3_power_test

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
const SAVE_VERSION := 35
const DAYS_CONSEQUENCE := 12
const DAYS_SAVE := 6
const DAYS_DETERMINISM := 15

var _failures: Array[String] = []


func _init() -> void:
	_test_merit_order_and_marginal_price()
	_test_ration_industry_and_unemployment()
	_test_long_blackout_weighs_more()
	_test_gdp_contracts()
	_test_fuel_leaves_market()
	_test_money_conservation_and_zero_sum()
	_test_connection_states()
	_test_line_work_changes_state()
	_test_save_v35_roundtrip()
	_test_determinism()
	_test_absence_declared()
	if _failures.is_empty():
		print("[power] OK: mérito/preço marginal, racionamento (indústria, "
			+ "emprego, satisfação, PIB), combustível, conservação, "
			+ "conexão (isolada/parcial/conectada), obra de linha, save v35, "
			+ "determinismo e ausência declarada.")
		quit(0)
	else:
		print("[power] FALHAS (%d):" % _failures.size())
		for failure: String in _failures:
			print(" - " + failure)
		quit(1)


# --- (1) mérito e preço marginal ------------------------------------------
func _test_merit_order_and_marginal_price() -> void:
	var params := _params()
	var cities := {
		"AGRIA": _city({"coal": 200.0}, {"coal": 400.0}),
		"PORTUS": _city({"hydro": 200.0}, {"hydro": 400.0}),
	}
	var sim = _world(_nation(), cities)
	# Demanda baixa (0,2× a renda): só a hidro (mais barata) precisa entrar.
	sim.set_external_income_index("AGRIA", 0.2)
	sim.set_external_income_index("PORTUS", 0.2)
	sim.tick_day()
	var low_price := float(sim.get_power_status().get("average_price", 0.0))
	_check("(1) mérito: só a hidro despacha na demanda baixa",
		_fuel(sim, "hydro") > 0.0 and _fuel(sim, "coal") <= EPS,
		"hydro=%.3f coal=%.3f" % [_fuel(sim, "hydro"), _fuel(sim, "coal")])
	var hydro_cost := _param(params, "opex_per_mwh_hydro")
	_check("(1) preço marginal = custo da hidro (última usina ligada)",
		absf(low_price - hydro_cost) <= 1e-6,
		"preço=%.6f custo_hidro=%.6f" % [low_price, hydro_cost])

	# Demanda de renda 1,0: o carvão vira a última usina e define o preço.
	var coal_price := _market_price(sim, "AGRIA", "coal")
	var coal_stock := _market_stock(sim, "AGRIA", "coal")
	sim.set_external_income_index("AGRIA", 1.0)
	sim.set_external_income_index("PORTUS", 1.0)
	sim.tick_day()
	var high_price := float(sim.get_power_status().get("average_price", 0.0))
	var premium := _param(params, "fuel_import_premium")
	var multiplier := 1.0 if coal_stock > 1e-9 else premium
	var coal_cost := _param(params, "fuel_t_per_mwh_coal") * coal_price \
		* multiplier + _param(params, "opex_per_mwh_coal")
	_check("(1) mérito: o carvão entra quando a hidro não cobre a demanda",
		_fuel(sim, "coal") > 0.0,
		"coal=%.3f MWh" % _fuel(sim, "coal"))
	_check("(1) preço marginal = custo do carvão (última usina ligada)",
		absf(high_price - coal_cost) <= 1e-6,
		"preço=%.6f custo_carvão=%.6f (mercado=%.4f, %s)" % [
			high_price, coal_cost, coal_price,
			"local" if multiplier == 1.0 else "importado"])
	_check("(1) preço sobe quando a usina mais cara é despachada",
		high_price > low_price + 1e-3,
		"baixo=%.6f alto=%.6f" % [low_price, high_price])


# --- (2) racionamento: indústria e desemprego -----------------------------
func _test_ration_industry_and_unemployment() -> void:
	# O fixture não tem prédio industrial; a produção industrial é publicada
	# pelo próprio fator da D-116 (`industry_factor`), que multiplica a
	# produção e a folha industrial no núcleo. O emprego é medido no bloco da
	# cidade (serviços são o empregador real do cenário).
	var partial := {
		"AGRIA": _city({"hydro": 60.0}, {"hydro": 400.0}),
		"PORTUS": _city({"hydro": 60.0}, {"hydro": 400.0}),
	}
	var sim_partial = _world(_nation(), partial, 1)
	var row: Dictionary = _power_city(sim_partial, "AGRIA")
	var ratio := float(row.get("served_ratio", -1.0))
	var factor := float(row.get("industry_factor", -1.0))
	_check("(2) racionamento parcial acontece (0 < fração servida < 1)",
		ratio > 0.2 and ratio < 0.95,
		"served_ratio=%.4f" % ratio)
	_check("(2) produção industrial quase proporcional à fração servida",
		absf(factor - ratio) <= 1e-6,
		"industry=%.6f served=%.6f" % [factor, ratio])

	var served_cities := {
		"AGRIA": _city({"hydro": 400.0}, {"hydro": 800.0}),
		"PORTUS": _city({"hydro": 800.0}, {"hydro": 1600.0}),
	}
	var dark_cities := {
		"AGRIA": _city({}, {}),
		"PORTUS": _city({}, {}),
	}
	var served = _world(_nation(), served_cities, DAYS_CONSEQUENCE)
	var dark = _world(_nation(), dark_cities, DAYS_CONSEQUENCE)
	var served_row: Dictionary = _power_city(served, "AGRIA")
	var dark_row: Dictionary = _power_city(dark, "AGRIA")
	_check("(2) mundo iluminado serve ~100% e o escuro ~0%",
		float(served_row.get("served_ratio", 0.0)) > 0.99
			and float(dark_row.get("served_ratio", 1.0)) < 1e-3,
		"servido=%.4f escuro=%.4f" % [
			float(served_row.get("served_ratio", 0.0)),
			float(dark_row.get("served_ratio", 1.0))])
	var served_pop: Dictionary = _city_status(served, "AGRIA").get("pop", {})
	var dark_pop: Dictionary = _city_status(dark, "AGRIA").get("pop", {})
	var employed_on := float(served_pop.get("employed", 0.0))
	var employed_off := float(dark_pop.get("employed", 0.0))
	var unemployed_on := float(served_pop.get("unemployed", 0.0))
	var unemployed_off := float(dark_pop.get("unemployed", 0.0))
	_check("(2) racionamento sobe o desemprego",
		unemployed_off > unemployed_on + 1e-6,
		"desempregado %.2f → %.2f (empregado %.2f → %.2f)" % [
			unemployed_on, unemployed_off, employed_on, employed_off])
	_check("(2) racionamento derruba o emprego",
		employed_off < employed_on - 1e-6,
		"empregado %.2f → %.2f" % [employed_on, employed_off])


# --- (3) blecaute longo pesa mais -----------------------------------------
func _test_long_blackout_weighs_more() -> void:
	var cities := {"AGRIA": _city({}, {}), "PORTUS": _city({}, {})}
	var sim = _world(_nation(), cities)
	var stress: Array[float] = []
	for day in DAYS_CONSEQUENCE:
		sim.tick_day()
		stress.append(float(_power_city(sim, "AGRIA").get("stress", -1.0)))
	_check("(3) blecaute gera insatisfação já no primeiro dia",
		stress.size() > 0 and stress[0] > 0.0,
		"stress dia 1 = %.4f" % (stress[0] if stress.size() > 0 else -1.0))
	_check("(3) blecaute longo pesa mais (stress cresce com a duração)",
		stress.size() == DAYS_CONSEQUENCE and stress[-1] > stress[0] + 0.1,
		"dia 1 = %.4f, dia %d = %.4f" % [
			stress[0] if stress.size() > 0 else -1.0, stress.size(),
			stress[-1] if stress.size() > 0 else -1.0])
	var increasing := true
	for index in range(1, stress.size()):
		if stress[index] < stress[index - 1] - 1e-9:
			increasing = false
	_check("(3) stress não cai enquanto o blecaute continua", increasing,
		str(stress))
	_check("(3) sem energia o racionamento é registrado",
		int(_power_city(sim, "AGRIA").get("ration_days", 0)) == DAYS_CONSEQUENCE,
		"ration_days=%s" % _power_city(sim, "AGRIA").get("ration_days"))


# --- (4) PIB contrai -------------------------------------------------------
func _test_gdp_contracts() -> void:
	var served_cities := {
		"AGRIA": _city({"hydro": 400.0}, {"hydro": 800.0}),
		"PORTUS": _city({"hydro": 800.0}, {"hydro": 1600.0}),
	}
	var dark_cities := {"AGRIA": _city({}, {}), "PORTUS": _city({}, {})}
	var served = _world(_nation(), served_cities, DAYS_CONSEQUENCE)
	var dark = _world(_nation(), dark_cities, DAYS_CONSEQUENCE)
	var gdp_on := float(_city_status(served, "AGRIA").get("gdp", 0.0))
	var gdp_off := float(_city_status(dark, "AGRIA").get("gdp", 0.0))
	_check("(4) PIB contrai com o racionamento", gdp_off < gdp_on - 1e-6,
		"PIB %.2f → %.2f" % [gdp_on, gdp_off])
	var lost_on := float(_power_city(served, "AGRIA").get("lost_gdp", -1.0))
	var lost_off := float(_power_city(dark, "AGRIA").get("lost_gdp", 0.0))
	_check("(4) PIB perdido (VOLL) só existe com falta",
		absf(lost_on) <= EPS and lost_off > 0.0,
		"servido=%.2f escuro=%.2f" % [lost_on, lost_off])


# --- (5) combustível sai do mercado ---------------------------------------
func _test_fuel_leaves_market() -> void:
	var params := _params()
	var cities := {
		"AGRIA": _city({"coal": 100.0}, {"coal": 200.0}),
		"PORTUS": _city({"hydro": 10.0}, {"hydro": 20.0}),
	}
	var sim = _world(_nation(), cities)
	sim.set_market_stock("AGRIA", "coal", 1000.0)
	sim.set_market_price("AGRIA", "coal", 4.0)
	var stock_before := _market_stock(sim, "AGRIA", "coal")
	var coal_index := _product_index(sim, "coal")
	sim.tick_day()
	var generation := _fuel(sim, "coal")
	var tons := generation * _param(params, "fuel_t_per_mwh_coal")
	var stock_after := _market_stock(sim, "AGRIA", "coal")
	var consumed := _consumed_today(sim, coal_index)
	_check("(5) a térmica gera e compra combustível",
		generation > 0.0 and tons > 0.0,
		"geração=%.2f MWh, %.4f t" % [generation, tons])
	_check("(5) o combustível da usina entra no consumo do dia",
		absf(consumed - tons) <= 1e-6,
		"consumed_today=%.6f esperado=%.6f" % [consumed, tons])
	_check("(5) o estoque do mercado cai com a compra da usina",
		stock_before - stock_after >= tons * 0.5 - EPS,
		"estoque %.4f → %.4f (%.4f t)" % [
			stock_before, stock_after, stock_before - stock_after])


# --- (6) conservação de dinheiro e soma zero ------------------------------
func _test_money_conservation_and_zero_sum() -> void:
	var cities := {
		"AGRIA": _city({"coal": 120.0}, {"coal": 240.0}),
		"PORTUS": _city({"hydro": 200.0}, {"hydro": 400.0}),
	}
	var sim = _world(_nation(), cities)
	var money_before := float(sim.get_status().get("money_total", 0.0))
	var served_total := 0.0
	for day in 5:
		sim.tick_day()
		var power: Dictionary = sim.get_power_status()
		var money := float(sim.get_status().get("money_total", 0.0))
		_check("(6) dinheiro conservado no dia %d de energia" % (day + 1),
			absf(money - money_before) <= MONEY_EPSILON,
			"%.6f → %.6f" % [money_before, money])
		_check("(6) soma financeira do setor fecha em zero (dia %d)"
				% (day + 1),
			absf(float(power.get("cash", -1.0))) <= EPS,
			"power_cash=%s" % power.get("cash"))
		served_total = float(power.get("served_mwh", 0.0))
		var violations: Array = sim.get_status().get("invariant_violations", [])
		_check("(6) sem violação de invariante no dia %d" % (day + 1),
			violations.is_empty(), str(violations))
	_check("(6) houve cobrança real (energia servida > 0)", served_total > 0.0,
		"served_mwh=%.2f" % served_total)


# --- (7) isolada × parcial × conectada ------------------------------------
func _test_connection_states() -> void:
	var cities := {
		"AGRIA": _city({"hydro": 5.0}, {"hydro": 10.0}),
		"PORTUS": _city({"hydro": 300.0}, {"hydro": 600.0}),
	}
	var sim = _world(_nation(), cities)
	# Isolada: só a usina local de AGRIA abastece (60 MWh de 937,5).
	sim.set_power_city("AGRIA", 0, 0.0, 100.0)
	sim.tick_day()
	var isolated: Dictionary = _power_city(sim, "AGRIA")
	_check("(7) estado isolado publicado",
		String(isolated.get("connection", "")) == "isolated",
		String(isolated.get("connection", "")))
	var served_isolated := float(isolated.get("served_mwh", 0.0))
	var grid_isolated: Dictionary = _grid(sim, "DEMO")
	# Parcial: a linha de 5 MW importa mais 120 MWh (teto da conexão).
	sim.set_power_city("AGRIA", 1, 5.0, 100.0)
	sim.tick_day()
	var partial: Dictionary = _power_city(sim, "AGRIA")
	_check("(7) estado parcial publicado",
		String(partial.get("connection", "")) == "partial",
		String(partial.get("connection", "")))
	var served_partial := float(partial.get("served_mwh", 0.0))
	_check("(7) conexão parcial serve mais que a isolada e respeita a linha",
		served_partial > served_isolated + 1.0
			and served_partial <= served_isolated + 5.0 * 24.0 + 1e-6,
		"isolada=%.2f parcial=%.2f" % [served_isolated, served_partial])
	# Conectada: entra na rede nacional (parque de PORTUS cobre tudo).
	sim.set_power_city("AGRIA", 2, 0.0, 100.0)
	sim.tick_day()
	var connected: Dictionary = _power_city(sim, "AGRIA")
	_check("(7) estado conectado publicado",
		String(connected.get("connection", "")) == "connected",
		String(connected.get("connection", "")))
	var served_connected := float(connected.get("served_mwh", 0.0))
	var demand := float(connected.get("demand_mwh", 0.0))
	_check("(7) conectada à rede nacional é servida por inteiro",
		served_connected > served_partial + 1.0
			and absf(served_connected - demand) <= 1e-6,
		"servido=%.2f demanda=%.2f" % [served_connected, demand])
	_check("(7) a rede publica os estados no dia isolado",
		int(grid_isolated.get("connected", 0)) == 1
			and int(grid_isolated.get("isolated", 0)) == 1,
		str(grid_isolated))
	_check("(7) conectada entra na rede nacional",
		int(_grid(sim, "DEMO").get("connected", 0)) == 2
			and int(_grid(sim, "DEMO").get("isolated", 0)) == 0,
		str(_grid(sim, "DEMO")))


# --- (8) obra de linha muda o estado --------------------------------------
func _test_line_work_changes_state() -> void:
	var cities := {
		"AGRIA": _city({"hydro": 5.0}, {"hydro": 10.0}),
		"PORTUS": _city({"hydro": 300.0}, {"hydro": 600.0}),
	}
	var sim = _world(_nation(), cities)
	sim.set_power_city("AGRIA", 0, 0.0, 100.0)
	# Obra de linha: 120 dias é o prazo real; aqui o prazo vira 1 dia (hook de
	# teste declarado) para provar que a CONCLUSÃO muda o estado — o estado
	# inicial/parcial continua o mesmo mecanismo.
	sim.set_power_params({"build_days_line": 1.0})
	sim.set_instant_construction(true)
	# O fixture nasce sem estoque dos materiais de obra: semeia o suficiente
	# para a linha avançar (a obra consome do mercado local, como no jogo).
	for good: String in ["construction_material", "steel_structure",
			"electrical_material"]:
		sim.set_market_stock("AGRIA", good, 1000.0)
	var before := _power_city(sim, "AGRIA")
	var money_before := float(sim.get_status().get("money_total", 0.0))
	var built: Dictionary = sim.build_power_line("AGRIA", 10.0)
	_check("(8) obra de linha aceita", bool(built.get("started", false)),
		String(built.get("error", "")))
	# O avanço das obras roda no fechamento do dia; 3 dias cobrem início,
	# prazo de 1 dia e a publicação do estado novo.
	for day in 3:
		sim.tick_day()
	var after := _power_city(sim, "AGRIA")
	_check("(8) a cidade isolada vira conectada após a obra",
		String(before.get("connection", "")) == "isolated"
			and String(after.get("connection", "")) == "connected",
		"%s → %s" % [before.get("connection"), after.get("connection")])
	_check("(8) a linha entrega a capacidade contratada",
		float(after.get("line_mw", 0.0)) >= 10.0 - EPS,
		"line_mw=%s" % after.get("line_mw"))
	_check("(8) obra de linha não cria nem destrói dinheiro",
		absf(float(sim.get_status().get("money_total", 0.0)) - money_before)
			<= MONEY_EPSILON,
		"%.6f → %.6f" % [money_before,
			float(sim.get_status().get("money_total", 0.0))])


# --- (9) save/load v35 -----------------------------------------------------
func _test_save_v35_roundtrip() -> void:
	var cities := {
		"AGRIA": _city({"coal": 120.0}, {"coal": 240.0}),
		"PORTUS": _city({"hydro": 200.0}, {"hydro": 400.0}),
	}
	var sim = _world(_nation(), cities, DAYS_SAVE)
	var data: Dictionary = sim.to_dict()
	_check("(9) save grava a versão v35",
		int(data.get("version", 0)) == SAVE_VERSION,
		"version=%s" % data.get("version"))
	var block: Variant = data.get("power")
	_check("(9) save tem o bloco de energia pronto",
		block is Dictionary and bool((block as Dictionary).get("ready", false)),
		str(block is Dictionary))
	var before := _canon_power(sim)
	var loaded = _load(data)
	_check("(9) round-trip bit-exato do status de energia",
		before == _canon_power(loaded), "status divergiu após o load")
	# A definição do dado público não sobrescreve o estado do save.
	var plants_before := int(loaded.get_power_status().get("plants", -1))
	var reapplied := int(loaded.set_power_definitions(
		_nation(), cities, {}, true))
	var plants_after := int(loaded.get_power_status().get("plants", -2))
	_check("(9) only_missing não re-semeia o parque do save",
		plants_before == plants_after,
		"plants %d → %d (set_power_definitions=%d)" % [plants_before,
			plants_after, reapplied])
	# Continuação idêntica ao caminho direto.
	sim.tick_day()
	loaded.tick_day()
	_check("(9) continuação idêntica ao caminho direto",
		before != _canon_power(sim) and _canon_power(sim) == _canon_power(loaded),
		"dia=%s/%s preço=%s/%s" % [
			_canon_power(sim).hash(), _canon_power(loaded).hash(),
			sim.get_power_status().get("average_price"),
			loaded.get_power_status().get("average_price")])


# --- (10) determinismo -----------------------------------------------------
func _test_determinism() -> void:
	var cities := {
		"AGRIA": _city({"hydro": 150.0}, {"hydro": 300.0}),
		"PORTUS": _city({"coal": 100.0}, {"coal": 200.0}),
	}
	var a = _world(_nation(), cities, DAYS_DETERMINISM)
	var b = _world(_nation(), cities, DAYS_DETERMINISM)
	_check("(10) dois mundos iguais têm o mesmo status de energia",
		_canon_power(a) == _canon_power(b), "status divergiu")
	_check("(10) dinheiro e PIB batem campo a campo",
		float(a.get_status().get("money_total", 0.0))
			== float(b.get_status().get("money_total", 0.0))
			and float(_city_status(a, "AGRIA").get("gdp", 0.0))
			== float(_city_status(b, "AGRIA").get("gdp", 0.0)),
		"money=%s/%s" % [a.get_status().get("money_total"),
			b.get_status().get("money_total")])


# --- (11) ausência declarada ----------------------------------------------
func _test_absence_declared() -> void:
	# Sem nação nem cidade no dado público: `has_data=false` e a ausência NÃO
	# vira blecaute (nada de racionamento fabricado).
	var sim = _world({}, {}, 3)
	var power: Dictionary = sim.get_power_status()
	var row: Dictionary = _power_city(sim, "AGRIA")
	_check("(11) cidade sem dado de energia fica declarada",
		not bool(row.get("has_data", true)),
		str(row.get("has_data")))
	_check("(11) sem dado não há racionamento fabricado",
		int(power.get("rationing_cities", -1)) == 0
			and float(row.get("served_ratio", 0.0)) == 1.0,
		"rationing=%s ratio=%s" % [power.get("rationing_cities"),
			row.get("served_ratio")])


# --- helpers ---------------------------------------------------------------
func _world(nations: Dictionary, cities: Dictionary, days := 0):
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
		"currencies": [{
			"code": "DEM", "name": "Demo", "symbol": "D",
			"nations": ["DEMO"], "rate_per_anchor": 1.0, "rate": 1.0,
			"base_rate": 1.0,
		}],
	})
	sim.initialize_scenario()
	sim.set_power_definitions(nations, cities, {}, true)
	for day in days:
		sim.tick_day()
	return sim


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


func _nation(access := 100.0, urban := 100.0, rural := 100.0,
		loss := 4.0) -> Dictionary:
	return {"DEMO": {
		"access_pct": access,
		"access_urban_pct": urban,
		"access_rural_pct": rural,
		"loss_pct": loss,
	}}


func _city(park: Dictionary, cap: Dictionary = {}, access := 100.0,
		loss := 4.0) -> Dictionary:
	return {
		"access_pct": access,
		"loss_pct": loss,
		"park": park,
		"cap": cap,
		"solar_month": [],
	}


func _params() -> Dictionary:
	return (_json(POWER_PARAMS_PATH) as Dictionary).get("params", {})


func _param(params: Dictionary, key: String) -> float:
	return float(params.get(key, -1.0))


func _power_city(sim: Object, city_id: String) -> Dictionary:
	return (sim.get_power_status().get("cities", {}) as Dictionary).get(
		city_id, {})


func _grid(sim: Object, tag: String) -> Dictionary:
	return (sim.get_power_status().get("nations", {}) as Dictionary).get(
		tag, {})


func _fuel(sim: Object, fuel: String) -> float:
	var by_fuel: Dictionary = _grid(sim, "DEMO").get(
		"generation_mwh_by_fuel", {})
	return float(by_fuel.get(fuel, 0.0))


func _city_status(sim: Object, city_id: String) -> Dictionary:
	return sim.get_city_status(city_id)


func _product_index(sim: Object, product: String) -> int:
	return (sim.get_products() as Array).find(product)


func _market_price(sim: Object, city_id: String, product: String) -> float:
	var index := _product_index(sim, product)
	var markets: Array = _city_status(sim, city_id).get("markets", [])
	if index < 0 or index >= markets.size():
		return 0.0
	return float((markets[index] as Dictionary).get("price", 0.0))


func _market_stock(sim: Object, city_id: String, product: String) -> float:
	var index := _product_index(sim, product)
	var markets: Array = _city_status(sim, city_id).get("markets", [])
	if index < 0 or index >= markets.size():
		return 0.0
	return float((markets[index] as Dictionary).get("stock", 0.0))


func _consumed_today(sim: Object, index: int) -> float:
	var consumed: Variant = sim.get_status().get("consumed_today", [])
	if index < 0 or index >= (consumed as Array).size():
		return 0.0
	return float((consumed as Array)[index])


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
