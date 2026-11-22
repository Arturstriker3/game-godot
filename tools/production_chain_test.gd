extends SceneTree

## Teste das cadeias de produção (F2) no núcleo demo:
##   1. moinho (trigo -> farinha + farelo) + padaria (farinha -> pão): receita
##      de 1 insumo com SUBPRODUTO, cadeia de 3 níveis operando;
##   2. receita de 4 insumos (motor): produção limitada pelo insumo mais
##      escasso e consumo exatamente igual à saída (coeficiente 1:1);
##   3. alternativa de receita escolhida pelo MENOR CUSTO corrente
##      (fábrica de ração: trigo -> milho com choque de preço);
##   4. trabalho: uma fábrica nova não reduz a folha da fazenda, a fábrica
##      contrata sem passar da própria folha e o emprego nunca excede a
##      força de trabalho (mercado de trabalho por escolaridade, F7).
##
## Cenário: AGRIA/PORTUS (initialize_scenario), o mesmo dos testes de população e
## de porto. Estoque/caixa de partida são um FIXTURE explícito do teste (não
## dado de simulação): sem insumo estocado nenhuma receita pode rodar no dia 1.
##
##   godot --headless --path . --script res://tools/production_chain_test.gd

const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"

const EPSILON := 1e-4

var _failures: Array[String] = []
var _goods_order: Dictionary = {}

var _goods: Array = []
var _recipes: Array = []
var _needs: Dictionary = {}


func _init() -> void:
	_goods = _load_array(GOODS_PATH)
	_recipes = _load_array(RECIPES_PATH)
	_needs = _load_dict(NEEDS_PATH)
	for index in _goods.size():
		_goods_order[String((_goods[index] as Dictionary).get("id", ""))] = index

	if _goods.is_empty() or _recipes.is_empty():
		_fail("dados: goods/recipes nao carregaram")
	else:
		_test_flour_bread_chain()
		_test_multi_input_recipe()
		_test_recipe_alternative()
		_test_labor_priority()

	if _failures.is_empty():
		print("[chain] OK: multi-insumo, subproduto, alternativa e trabalho validados.")
		quit(0)
	else:
		print("[chain] FALHAS (%d):" % _failures.size())
		for failure in _failures:
			print(" - " + failure)
		quit(1)


func _fail(msg: String) -> void:
	_failures.append(msg)


func _load_array(path: String) -> Array:
	var data: Variant = _load_json(path)
	return data if typeof(data) == TYPE_ARRAY else []


func _load_dict(path: String) -> Dictionary:
	var data: Variant = _load_json(path)
	return data if typeof(data) == TYPE_DICTIONARY else {}


func _load_json(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("dados: nao abriu " + path)
		return null
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data


func _product(id: String) -> int:
	return int(_goods_order.get(id, -1))


## Mundo demo com bens/receitas/necessidades carregados ANTES de criar as
## construções (mesma ordem do autoload Economy).
func _new_sim():
	var sim = EconomySim.new()
	sim.set_goods(_goods)
	sim.set_recipes(_recipes)
	sim.set_needs(_needs)
	# §15: o teste mede a CADEIA de produção, não o cronograma de obra — liga a
	# construção instantânea para `build` materializar o prédio na hora.
	sim.set_instant_construction(true)
	sim.initialize_scenario()
	return sim


## Fixture: estoque de mercado/caixa de AGRIA antes do primeiro tick. AGRIA é
## o cenário sintético do teste; os números não representam o mundo real.
func _fixture(sim, stocks: Dictionary, population: float = -1.0) -> void:
	var snapshot: Dictionary = sim.to_dict()
	for city: Dictionary in snapshot.get("cities", []):
		if String(city.get("id", "")) != "AGRIA":
			continue
		city["firm_cash"] = 500000.0
		city["government_budget"] = 500000.0
		if population > 0.0:
			city["population"] = population
			city["sim_population"] = population
			city["service_jobs"] = population * 0.30
			var pop: Dictionary = city.get("pop", {})
			pop["population"] = population
		var markets: Array = city.get("markets", [])
		for good: String in stocks:
			var index := _product(good)
			if index < 0 or index >= markets.size():
				_fail("fixture: produto desconhecido " + good)
				continue
			var market: Dictionary = markets[index]
			market["stock"] = float(stocks[good])
			markets[index] = market
		city["markets"] = markets
	sim.from_dict(snapshot)


## O status diário publica só os escalares leves; o bloco pesado da cidade
## (mercados, construções, POP) vem sob demanda pelo id.
func _city(sim: Object, id: String) -> Dictionary:
	return sim.get_city_status(id)


func _building(city: Dictionary, id: String) -> Dictionary:
	for building: Dictionary in city.get("buildings", []):
		if String(building.get("id", "")) == id:
			return building
	return {}


func _stock(city: Dictionary, good: String) -> float:
	var index := _product(good)
	var markets: Array = city.get("markets", [])
	if index < 0 or index >= markets.size():
		return -1.0
	return float((markets[index] as Dictionary).get("stock", 0.0))


func _produced(status: Dictionary, good: String) -> float:
	var index := _product(good)
	var produced: Array = status.get("produced_total", [])
	if index < 0 or index >= produced.size():
		return -1.0
	return float(produced[index])


## 1. Cadeia de 3 níveis: trigo -> farinha (+ farelo) -> pão.
func _test_flour_bread_chain() -> void:
	var sim = _new_sim()
	_fixture(sim, {"wheat": 300.0})
	var flour := _product("flour")
	var bread := _product("bread")
	var mill: Dictionary = sim.build("AGRIA", "T-MILL", "flour", -1, flour,
		10.0, 0.0, 0.0, 0.0, 1.0, "private", 40000.0, "flour")
	var bakery: Dictionary = sim.build("AGRIA", "T-BAKERY", "bread", -1, bread,
		8.0, 0.0, 0.0, 0.0, 1.0, "private", 32000.0, "bread")
	if not bool(mill.get("created", false)):
		_fail("cadeia: moinho nao criado (%s)" % String(mill.get("error", "")))
		return
	if not bool(bakery.get("created", false)):
		_fail("cadeia: padaria nao criada (%s)" % String(bakery.get("error", "")))
		return

	var wheat_before := 0.0
	var status: Dictionary = sim.get_status()
	wheat_before = _stock(_city(sim, "AGRIA"), "wheat")
	for day in 4:
		status = sim.tick_day()
		var agria := _city(sim, "AGRIA")
		for good in ["wheat", "flour", "bran", "bread"]:
			if _stock(agria, good) < -EPSILON:
				_fail("cadeia: estoque negativo de %s no dia %d" % [good, day + 1])
		if float(agria.get("pop", {}).get("satisfaction", 0.0)) <= 0.0:
			_fail("cadeia: POPs sem comida no dia %d" % (day + 1))

	var agria := _city(sim, "AGRIA")
	var mill_status := _building(agria, "T-MILL")
	var bakery_status := _building(agria, "T-BAKERY")
	if String(mill_status.get("recipe", "")) != "flour":
		_fail("cadeia: moinho nao usa a receita 'flour' (%s)" % str(
			mill_status.get("recipe", "")))
	var byproducts: Array = mill_status.get("byproducts", [])
	var has_bran := false
	for item: Dictionary in byproducts:
		if String(item.get("good", "")) == "bran":
			has_bran = true
	if not has_bran:
		_fail("cadeia: farelo ausente nos subprodutos do moinho")
	if _produced(status, "flour") <= 0.0:
		_fail("cadeia: farinha nao produzida")
	if _produced(status, "bran") <= 0.0:
		_fail("cadeia: farelo (subproduto) nao produzido")
	if _produced(status, "bread") <= 0.0:
		_fail("cadeia: pao nao produzido (farinha do dia anterior nao chegou)")
	if String(bakery_status.get("recipe", "")) != "bread":
		_fail("cadeia: padaria sem receita 'bread'")
	var wheat_after := _stock(agria, "wheat")
	if wheat_after > wheat_before + 1e-6:
		_fail("cadeia: trigo do moinho nao foi consumido")
	print("[chain] trigo %.1f -> farelo %.1f | farinha %.1f | pao %.1f" % [
		wheat_before - wheat_after, _produced(status, "bran"),
		_produced(status, "flour"), _produced(status, "bread")])


## 2. Receita de 4 insumos: o mais escasso limita a saída e cada insumo
## consumido é exatamente a saída (coeficiente 1:1 da receita engine_a).
func _test_multi_input_recipe() -> void:
	var sim = _new_sim()
	_fixture(sim, {
		"steel": 50.0, "mechanical_parts": 4.0, "wires": 1000.0,
		"cast_parts": 1000.0,
	})
	var engine := _product("engine")
	var built: Dictionary = sim.build("AGRIA", "T-ENGINE", "engine", -1,
		engine, 10.0, 0.0, 0.0, 0.0, 1.0, "private", 60000.0, "engine_a")
	if not bool(built.get("created", false)):
		_fail("multi: fabrica de motor nao criada (%s)" % String(built.get("error", "")))
		return

	var inputs := ["steel", "mechanical_parts", "wires", "cast_parts"]
	var before: Dictionary = {}
	var status: Dictionary = sim.get_status()
	for good in inputs:
		before[good] = _stock(_city(sim, "AGRIA"), good)
	for day in 2:
		status = sim.tick_day()
		var agria := _city(sim, "AGRIA")
		for good in inputs:
			if _stock(agria, good) < -EPSILON:
				_fail("multi: estoque negativo de %s no dia %d" % [good, day + 1])

	var agria := _city(sim, "AGRIA")
	var produced := _produced(status, "engine")
	if produced <= 0.0:
		_fail("multi: motor nao produzido")
	var building := _building(agria, "T-ENGINE")
	if String(building.get("recipe", "")) != "engine_a":
		_fail("multi: receita alternativa escolhida sem insumo (engine_b)")
	for good in inputs:
		var consumed: float = float(before[good]) - _stock(agria, good)
		if absf(consumed - produced) > 1e-3:
			_fail("multi: %s consumido %.4f != saida %.4f" % [good, consumed, produced])
	if _stock(agria, "mechanical_parts") > 1e-3:
		_fail("multi: insumo mais escasso nao foi limitante (resta %.3f)" % _stock(
			agria, "mechanical_parts"))
	print("[chain] motor %.2f t | insumo escasso restante %.3f | consumos %s" % [
		produced, _stock(agria, "mechanical_parts"),
		", ".join(inputs.map(func(g): return "%.2f" % (float(before[g]) - _stock(agria, g))))])


## 3. Alternativa pelo menor custo corrente: com trigo 40/t (yield 0,6 ->
## 66,67/t de ração) e milho 42/t (yield 1 -> 42/t), a fábrica de ração escolhe
## feed_a. Um choque no milho (x2 -> 84/t) inverte para feed_f (trigo).
func _test_recipe_alternative() -> void:
	var sim = _new_sim()
	_fixture(sim, {"wheat": 300.0, "maize": 100.0})
	var feed := _product("feed")
	var built: Dictionary = sim.build("AGRIA", "T-FEED", "feed", -1, feed,
		10.0, 0.0, 0.0, 0.0, 1.0, "private", 40000.0, "feed_a")
	if not bool(built.get("created", false)):
		_fail("alternativa: fabrica de racao nao criada (%s)" % String(built.get(
			"error", "")))
		return
	var status: Dictionary = sim.tick_day()
	var chosen := String(_building(_city(sim, "AGRIA"), "T-FEED").get("recipe", ""))
	if chosen != "feed_a":
		_fail("alternativa: com milho 42 e trigo 66,7, esperado feed_a (obtido %s)" % chosen)
	sim.price_shock("maize", 2.0)
	status = sim.tick_day()
	chosen = String(_building(_city(sim, "AGRIA"), "T-FEED").get("recipe", ""))
	if chosen != "feed_f":
		_fail("alternativa: com milho a 84, esperado feed_f (obtido %s)" % chosen)
	print("[chain] racao: alternativa trocada por menor custo corrente (%s)" % chosen)


## 4. F7 substituiu o racionamento por classe (§5.3 do F2) pelo mercado de
## trabalho por escolaridade: não há mais prioridade explícita de fazenda
## sobre fábrica — a fazenda continua cheia porque as vagas de nível básico
## preenchem primeiro, e o emprego total é limitado pela força de trabalho.
## Cidade pequena (2000 hab., força de trabalho ~1100): a fazenda mantém a
## folha e a fábrica contrata (sem exceder a própria folha).
func _test_labor_priority() -> void:
	var sim = _new_sim()
	_fixture(sim, {}, 2000.0)
	var status: Dictionary = sim.get_status()
	var agria := _city(sim, "AGRIA")
	var farm_before := _building(agria, "FARM-A-1")
	if farm_before.is_empty():
		_fail("trabalho: fazenda de AGRIA ausente")
		return
	var farm_jobs := float(farm_before.get("jobs", 0.0))

	var engine := _product("engine")
	var built: Dictionary = sim.build("AGRIA", "T-ENGINE-LABOR", "engine", -1,
		engine, 20.0, 0.0, 0.0, 0.0, 1.0, "private", 120000.0, "engine_a")
	if not bool(built.get("created", false)):
		_fail("trabalho: fabrica nao criada (%s)" % String(built.get("error", "")))
		return

	status = sim.tick_day()
	agria = _city(sim, "AGRIA")
	var farm := _building(agria, "FARM-A-1")
	var factory := _building(agria, "T-ENGINE-LABOR")
	var workers := float(farm.get("workers", 0.0))
	if absf(workers - farm_jobs) > 0.01:
		_fail("trabalho: fazenda perdeu mao de obra (%.1f de %.1f)" % [workers, farm_jobs])
	var factory_jobs := float(factory.get("jobs", 0.0))
	var factory_workers := float(factory.get("workers", 0.0))
	if factory_jobs <= 0.0:
		_fail("trabalho: fabrica sem empregos declarados")
	elif factory_workers <= 0.0:
		_fail("trabalho: fabrica nao contratou (%.1f/%.1f)" % [
			factory_workers, factory_jobs])
	elif factory_workers > factory_jobs + 0.01:
		_fail("trabalho: fabrica contratou acima da folha (%.1f/%.1f)" % [
			factory_workers, factory_jobs])
	var pop: Dictionary = agria.get("pop", {})
	if float(pop.get("employed", 0.0)) > float(pop.get("labor_force", 0.0)) + 0.01:
		_fail("trabalho: empregados acima da forca de trabalho")
	print("[chain] trabalho: fazenda %.0f/%.0f | fabrica %.1f/%.1f | desempregados %.1f" % [
		workers, farm_jobs, factory_workers, factory_jobs,
		float(pop.get("unemployed", 0.0))])
