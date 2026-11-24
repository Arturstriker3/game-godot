extends SceneTree

## Teste D-127 - subprodutos com balanco de massa real (só dado + builder).
##
##   1. Conservacao por receita (as 114): saida principal + subprodutos +
##      perda declarada (`co_loss`) NAO pode passar dos insumos; toda receita
##      que declara subproduto ou perda tem que FECHAR (igualdade) - a matéria
##      nao pode aparecer do nada nem sumir sem declaracao.
##   2. Serraria: 1 t de madeira (tora) -> 0,55 t de tabua + 0,12 t de serragem,
##      com 0,33 t de casca/cavaco/umidade como PERDA declarada (FAO/USDA FS -
##      ver docs/economy/D127_byproducts.md).
##   3. Rotas da serragem: serragem -> celulose 2,2:1 e serragem -> palete
##      1,15:1 (yield = 1/2,2 e 1/1,15), sem tirar a rota direta madeira ->
##      celulose. Preco de sobra segue a regra de subproduto (0,10 x o pai).
##   4. Nenhum bem novo: o catalogo continua sendo exatamente o da spec
##      (21 primarios + 94 bens); nenhum id de cavaco/casca foi inventado e
##      toda referencia de receita resolve.
##   5. Runtime: a serraria do cenario AGRIA entrega tabua e serragem na
##      proporcao do dado (o nucleo aplica `byproduct_qty` por lote).
##
## Cenário: AGRIA/PORTUS (`initialize_scenario`), o mesmo de
## `production_chain_test`; o estoque de madeira é um FIXTURE explícito do teste
## (não dado de simulação).
##
##   godot --headless --path . --script res://tools/d127_byproducts_test.gd

const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"
const SPEC_PATH := "res://docs/economy/production_chains_spec.json"

const EPSILON := 1e-6
const MASS_EPSILON := 1e-6
## `yield` da spec é gravado com 4 casas (0,4545 = 1/2,2), não exato.
const YIELD_EPSILON := 5e-4
## Regra de preço de sobra (`docs/economy/production_chains_data.md` §4.1):
## subproduto custa 10% do bem pai.
const BYPRODUCT_PRICE_SHARE := 0.10
const PRICE_EPSILON := 0.011

var _failures: Array[String] = []

var _goods: Array = []
var _recipes: Array = []
var _needs: Dictionary = {}
var _goods_by_id: Dictionary = {}
var _recipes_by_id: Dictionary = {}
var _goods_order: Dictionary = {}


func _init() -> void:
	_goods = _load_array(GOODS_PATH)
	_recipes = _load_array(RECIPES_PATH)
	_needs = _load_dict(NEEDS_PATH)
	for item: Dictionary in _goods:
		var id := String(item.get("id", ""))
		_goods_by_id[id] = item
	for item: Dictionary in _recipes:
		_recipes_by_id[String(item.get("id", ""))] = item
	for index in _goods.size():
		_goods_order[String((_goods[index] as Dictionary).get("id", ""))] = index

	if _goods.is_empty() or _recipes.is_empty():
		_fail("dados: goods/recipes nao carregaram")
	else:
		_test_mass_balance()
		_test_sawmill()
		_test_sawdust_routes()
		_test_no_new_goods()
		_test_runtime_sawmill()

	if _failures.is_empty():
		print("[d127] OK: balanco de massa das 114 receitas, serraria 0,55/0,12, " +
			"rotas da serragem 2,2:1 e 1,15:1, nenhum bem novo.")
		quit(0)
	else:
		print("[d127] FALHAS (%d):" % _failures.size())
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


func _input_mass(recipe: Dictionary) -> float:
	var total := 0.0
	for slot: Dictionary in recipe.get("inputs", []):
		total += float(slot.get("qty", 0.0))
	return total


## Saída principal (`yield`) + subprodutos (`byproducts[].qty`) + perda
## declarada (`co_loss`). É exatamente o que o núcleo aplica por lote: o
## `outputs[]` da co-produção é a mesma lista do `byproducts` já normalizada.
func _output_mass(recipe: Dictionary) -> float:
	var total := float(recipe.get("yield", 0.0))
	for slot: Dictionary in recipe.get("byproducts", []):
		total += float(slot.get("qty", 0.0))
	total += float(recipe.get("co_loss", 0.0))
	return total


func _declares_byproduct(recipe: Dictionary) -> bool:
	if float(recipe.get("co_loss", 0.0)) > 0.0:
		return true
	return not (recipe.get("byproducts", []) as Array).is_empty()


## 1. Conservação de massa em TODAS as receitas. `co_loss` é fração de insumo
## (mesma leitura de `tools/build_goods_chain.py`).
func _test_mass_balance() -> void:
	var closed := 0
	var leaks := 0
	for recipe: Dictionary in _recipes:
		var id := String(recipe.get("id", ""))
		var inputs := _input_mass(recipe)
		var outputs := _output_mass(recipe)
		if inputs <= 0.0:
			_fail("massa: receita %s sem insumo" % id)
			continue
		if float(recipe.get("co_loss", 0.0)) < 0.0:
			_fail("massa: %s com co_loss negativo" % id)
		if outputs > inputs + MASS_EPSILON:
			leaks += 1
			_fail("massa: %s cria materia do nada (saidas %.4f + perda > insumos %.4f)" % [
				id, outputs, inputs])
			continue
		if _declares_byproduct(recipe):
			if absf(outputs - inputs) > MASS_EPSILON:
				_fail("massa: %s declara subproduto/perda mas nao fecha (%.4f != %.4f)" % [
					id, outputs, inputs])
			else:
				closed += 1
	if leaks > 0:
		return
	print("[d127] massa: %d receitas | %d com subproduto/perda fechando exato | 0 violacoes" % [
		_recipes.size(), closed])


## 2. Serraria: os números da spec e a perda declarada.
func _test_sawmill() -> void:
	var recipe := _recipe("planks")
	if recipe.is_empty():
		_fail("serraria: receita 'planks' ausente")
		return
	var inputs: Array = recipe.get("inputs", [])
	if inputs.size() != 1 or String((inputs[0] as Dictionary).get("good", "")) != "wood" \
			or absf(float((inputs[0] as Dictionary).get("qty", 0.0)) - 1.0) > EPSILON:
		_fail("serraria: insumo esperado 1 t de madeira (obtido %s)" % str(inputs))
	if absf(float(recipe.get("yield", 0.0)) - 0.55) > EPSILON:
		_fail("serraria: tabua esperada 0,55 t/tora (obtida %.4f)" % float(
			recipe.get("yield", 0.0)))
	var byproducts: Array = recipe.get("byproducts", [])
	if byproducts.size() != 1 \
			or String((byproducts[0] as Dictionary).get("good", "")) != "sawdust" \
			or absf(float((byproducts[0] as Dictionary).get("qty", 0.0)) - 0.12) > EPSILON:
		_fail("serraria: serragem esperada 0,12 t/tora (obtido %s)" % str(byproducts))
	if absf(float(recipe.get("co_loss", 0.0)) - 0.33) > EPSILON:
		_fail("serraria: perda declarada esperada 0,33 (obtida %.4f)" % float(
			recipe.get("co_loss", 0.0)))
	var total := float(recipe.get("yield", 0.0))
	for slot: Dictionary in byproducts:
		total += float(slot.get("qty", 0.0))
	total += float(recipe.get("co_loss", 0.0))
	if absf(total - 1.0) > MASS_EPSILON:
		_fail("serraria: 0,55 tabua + 0,12 serragem + 0,33 perda != 1 t de tora")
	print("[d127] serraria: 1 madeira -> %.2f tabua + %.2f serragem | perda %.2f" % [
		float(recipe.get("yield", 0.0)),
		float((byproducts[0] as Dictionary).get("qty", 0.0)) if byproducts.size() == 1 else 0.0,
		float(recipe.get("co_loss", 0.0))])


## 3. Rotas da serragem e preço de sobra.
func _test_sawdust_routes() -> void:
	var routes := [
		{"id": "cellulose_b", "output": "cellulose", "input": 2.2},
		{"id": "pallets_b", "output": "pallets", "input": 1.15},
	]
	for route: Dictionary in routes:
		var recipe := _recipe(String(route["id"]))
		var rid := String(route["id"])
		if recipe.is_empty():
			_fail("serragem: rota '%s' ausente" % rid)
			continue
		var inputs: Array = recipe.get("inputs", [])
		if inputs.size() != 1 \
				or String((inputs[0] as Dictionary).get("good", "")) != "sawdust" \
				or absf(float((inputs[0] as Dictionary).get("qty", 0.0)) - 1.0) > EPSILON:
			_fail("serragem: %s deveria consumir 1 t de serragem (obtido %s)" % [rid, str(inputs)])
		if String(recipe.get("output", "")) != String(route["output"]):
			_fail("serragem: %s deveria produzir %s" % [rid, String(route["output"])])
		var expected_yield := 1.0 / float(route["input"])
		if absf(float(recipe.get("yield", 0.0)) - expected_yield) > YIELD_EPSILON:
			_fail("serragem: %s esperado yield %.4f (%.1f:1), obtido %.4f" % [
				rid, expected_yield, float(route["input"]), float(recipe.get("yield", 0.0))])
	# A rota direta madeira -> celulose continua existindo como alternativa.
	var direct := _recipe("cellulose_a")
	if direct.is_empty():
		_fail("serragem: rota direta 'cellulose_a' (madeira -> celulose) sumiu")
	elif String(direct.get("output", "")) != "cellulose":
		_fail("serragem: 'cellulose_a' nao produz celulose")
	# Regra de sobra: preço do subproduto = 0,10 x o pai.
	var rules := [{"child": "sawdust", "parent": "planks"}, {"child": "bran", "parent": "flour"}]
	for rule: Dictionary in rules:
		var child := _goods_by_id.get(String(rule["child"]), {}) as Dictionary
		var parent := _goods_by_id.get(String(rule["parent"]), {}) as Dictionary
		if child.is_empty() or parent.is_empty():
			_fail("sobra: bem ausente (%s/%s)" % [String(rule["child"]), String(rule["parent"])])
			continue
		var expected := float(parent.get("base_price", 0.0)) * BYPRODUCT_PRICE_SHARE
		if absf(float(child.get("base_price", 0.0)) - expected) > PRICE_EPSILON:
			_fail("sobra: %s = %.2f, esperado %.2f (10%% de %s)" % [String(rule["child"]),
				float(child.get("base_price", 0.0)), expected, String(rule["parent"])])
	var cellulose := _recipe("cellulose_b")
	print("[d127] serragem: celulose %.4f (2,2:1) | palete %.4f (1,15:1) | serragem %.2f/t" % [
		float(cellulose.get("yield", 0.0)),
		float(_recipe("pallets_b").get("yield", 0.0)),
		float((_goods_by_id.get("sawdust", {}) as Dictionary).get("base_price", 0.0))])


## 4. Nenhum bem novo: catálogo == spec (21 primários + 94 bens = 115).
func _test_no_new_goods() -> void:
	var spec := _load_dict(SPEC_PATH)
	if spec.is_empty():
		_fail("bens: spec nao carregou")
		return
	var expected: Dictionary = {}
	# `primaries` é lista de ids (texto); `goods` é lista de objetos com `id`.
	for primary: Variant in spec.get("primaries", []):
		expected[String(primary)] = true
	for good: Variant in spec.get("goods", []):
		if typeof(good) == TYPE_DICTIONARY:
			expected[String((good as Dictionary).get("id", ""))] = true
	var actual: Dictionary = {}
	for item: Dictionary in _goods:
		var id := String(item.get("id", ""))
		if actual.has(id):
			_fail("bens: id duplicado '%s'" % id)
		actual[id] = true
	for id: String in actual:
		if not expected.has(id):
			_fail("bens: '%s' nao existe na spec (bem inventado)" % id)
	for id: String in expected:
		if not actual.has(id):
			_fail("bens: '%s' da spec sumiu do catalogo" % id)
	if actual.size() != expected.size():
		_fail("bens: catalogo com %d bens, spec com %d" % [actual.size(), expected.size()])
	# O dono recusou "cavaco" como bem novo: nada de resíduo de madeira avulso.
	for forbidden in ["wood_chips", "cavaco", "sawmill_chips", "shavings", "bark"]:
		if actual.has(forbidden):
			_fail("bens: '%s' foi inventado (o dono nao quer cavaco como bem)" % forbidden)
	# Toda referência de receita resolve em bem existente.
	for recipe: Dictionary in _recipes:
		var id := String(recipe.get("id", ""))
		if not actual.has(String(recipe.get("output", ""))):
			_fail("receita %s: saida '%s' nao existe" % [id, String(recipe.get("output", ""))])
		for slot: Dictionary in recipe.get("inputs", []):
			if not actual.has(String(slot.get("good", ""))):
				_fail("receita %s: insumo '%s' nao existe" % [id, String(slot.get("good", ""))])
		for slot: Dictionary in recipe.get("byproducts", []):
			if not actual.has(String(slot.get("good", ""))):
				_fail("receita %s: subproduto '%s' nao existe" % [id, String(slot.get("good", ""))])
		for slot: Dictionary in recipe.get("outputs", []):
			if not actual.has(String(slot.get("good", ""))):
				_fail("receita %s: co-produto '%s' nao existe" % [id, String(slot.get("good", ""))])
	print("[d127] bens: %d no catalogo = %d na spec | %d receitas com referencias validas" % [
		actual.size(), expected.size(), _recipes.size()])


func _recipe(id: String) -> Dictionary:
	return _recipes_by_id.get(id, {}) as Dictionary


## Mundo demo com bens/receitas/necessidades carregados ANTES de criar as
## construções (mesma ordem do autoload Economy).
func _new_sim():
	var sim = EconomySim.new()
	sim.set_goods(_goods)
	sim.set_recipes(_recipes)
	sim.set_needs(_needs)
	sim.set_instant_construction(true)
	sim.initialize_scenario()
	return sim


## Fixture: estoque de mercado/caixa de AGRIA antes do primeiro tick.
func _fixture(sim, stocks: Dictionary) -> void:
	var snapshot: Dictionary = sim.to_dict()
	for city: Dictionary in snapshot.get("cities", []):
		if String(city.get("id", "")) != "AGRIA":
			continue
		city["firm_cash"] = 500000.0
		city["government_budget"] = 500000.0
		var markets: Array = city.get("markets", [])
		for good: String in stocks:
			var index := int(_goods_order.get(good, -1))
			if index < 0 or index >= markets.size():
				_fail("fixture: produto desconhecido " + good)
				continue
			var market: Dictionary = markets[index]
			market["stock"] = float(stocks[good])
			markets[index] = market
		city["markets"] = markets
	sim.from_dict(snapshot)


func _stock(city: Dictionary, good: String) -> float:
	var index := int(_goods_order.get(good, -1))
	var markets: Array = city.get("markets", [])
	if index < 0 or index >= markets.size():
		return -1.0
	return float((markets[index] as Dictionary).get("stock", 0.0))


func _produced(status: Dictionary, good: String) -> float:
	var index := int(_goods_order.get(good, -1))
	var produced: Array = status.get("produced_total", [])
	if index < 0 or index >= produced.size():
		return -1.0
	return float(produced[index])


## 5. Runtime: o núcleo aplica a serragem na proporção do dado (0,12/0,55 da
## tábua). A eficiência (D-125) escala os dois lados junto, então a razão vale
## em qualquer dia e independe da rampa.
func _test_runtime_sawmill() -> void:
	var sim = _new_sim()
	_fixture(sim, {"wood": 300.0})
	var planks_index := int(_goods_order.get("planks", -1))
	var built: Dictionary = sim.build("AGRIA", "T-SAWMILL", "planks", -1,
		planks_index, 10.0, 0.0, 0.0, 0.0, 1.0, "private", 40000.0, "planks")
	if not bool(built.get("created", false)):
		_fail("runtime: serraria nao criada (%s)" % String(built.get("error", "")))
		return
	var wood_before := _stock(sim.get_city_status("AGRIA"), "wood")
	var status: Dictionary = sim.get_status()
	for day in 2:
		status = sim.tick_day()
		var agria: Dictionary = sim.get_city_status("AGRIA")
		for good in ["wood", "planks", "sawdust"]:
			if _stock(agria, good) < -EPSILON:
				_fail("runtime: estoque negativo de %s no dia %d" % [good, day + 1])
	var agria := sim.get_city_status("AGRIA")
	var planks := _produced(status, "planks")
	var sawdust := _produced(status, "sawdust")
	var wood_used := wood_before - _stock(agria, "wood")
	if planks <= 0.0:
		_fail("runtime: serraria nao produziu tabua")
		return
	if sawdust <= 0.0:
		_fail("runtime: serragem (subproduto) nao produzida")
		return
	var expected_ratio := 0.12 / 0.55
	var ratio := sawdust / planks
	if absf(ratio - expected_ratio) > 1e-3:
		_fail("runtime: serragem/tabua %.4f, esperado %.4f (0,12/0,55)" % [ratio, expected_ratio])
	var expected_wood := planks / 0.55
	if absf(wood_used - expected_wood) > 1e-2:
		_fail("runtime: madeira consumida %.3f, esperado %.3f (tabua/0,55)" % [
			wood_used, expected_wood])
	print("[d127] runtime: madeira %.2f -> tabua %.2f + serragem %.2f (razao %.4f)" % [
		wood_used, planks, sawdust, ratio])
