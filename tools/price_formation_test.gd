extends SceneTree

## Formação de preço (D-091 + D-092) — headless, exit != 0 em falha.
##
## Cobre os casos do fechamento da base e do EQUILÍBRIO:
##   (a) escassez real persistente  -> preço sobe ACIMA de 3× o base e PLATEIA
##       (sem +0,05×/dia indefinido);
##   (b) excesso de oferta          -> preço cai até o piso de custo;
##   (c) demanda efetiva × desejada -> desired gigante + effective pequeno NÃO
##       infla o preço (caso ferro: usina faminta em plena capacidade);
##   (d) nicho sem teto             -> mina estatal de nicho passa de 10 t/d;
##   (e) guarda numérica            -> sem NaN/inf/negativo após choque extremo;
##   (f) convergência após choque   -> preço 10× volta ao piso com oferta normal;
##   (g) elasticidade por classe    -> luxo cai mais que básico a 2× o preço;
##   (h) piso de custo              -> preço não fica abaixo de insumo+trabalho.
##
##   godot --headless --path . --script res://tools/price_formation_test.gd

const GeoScript := preload("res://core/economy/geography_service.gd")
const GroupNamerScript := preload("res://core/economy/group_namer.gd")

const GOODS_PATH := "res://data/definitions/economy/goods.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"

const AGRIA_CENTER := Vector2(2532.6, 571.4)
const PORTUS_CENTER := Vector2(2560.4, 543.8)

## Produtos primários do núcleo (ordem fixa de world_state.h / economy_sim.cpp).
const WHEAT := 0
const IRON := 1
const GOLD := 6
const COAL := 12

## Base do trigo no núcleo (world_state.h): 40.0.
const WHEAT_BASE := 40.0

var _failures: Array[String] = []


func _init() -> void:
	_test_scarcity_above_cap()
	_test_excess_drops_price()
	_test_effective_vs_desired()
	_test_niche_no_cap()
	_test_numeric_guard()
	_test_shock_converges()
	_test_elasticity_by_class()
	_test_cost_floor()

	if _failures.is_empty():
		print("[price_formation] OK: equilíbrio (markup), demanda elástica, piso de custo e guarda numérica.")
		quit(0)
	else:
		print("[price_formation] FALHAS (%d):" % _failures.size())
		for failure in _failures:
			print(" - " + failure)
		quit(1)


func _fail(msg: String) -> void:
	if not _failures.has(msg):
		_failures.append(msg)


func _make_world(ai: bool) -> EconomySim:
	var sim := EconomySim.new()
	sim.set_instant_construction(true)
	sim.initialize_scenario()
	var geo := GeoScript.new()
	geo.apply_to_sim(sim, [
		{"id": "AGRIA", "center": AGRIA_CENTER},
		{"id": "PORTUS", "center": PORTUS_CENTER},
	])
	sim.set_ai_enabled(ai)
	if ai:
		GroupNamerScript.name_groups(sim)
	return sim


func _run(sim: EconomySim, days: int) -> void:
	for _index in days:
		sim.tick_day()


func _market(sim: EconomySim, city_id: String, product: int) -> Dictionary:
	var markets: Array = sim.get_city_status(city_id).get("markets", [])
	if product < 0 or product >= markets.size():
		return {}
	return markets[product]


func _market_price(sim: EconomySim, city_id: String, product: int) -> float:
	return float(_market(sim, city_id, product).get("price", 0.0))


## ---------------------------------------------------------------------------
## (a) escassez real persistente -> preço sobe ACIMA de 3× o base.
## A comida é demanda FINANCIADA (effective = desired - latent, regra F7):
## necessidade 30 t/d (AGRIA) + 60 t/d (PORTUS) contra uma oferta quase nula
## (aptidão 0,02). Sem teto, o preço cruza 3× (120).
## ---------------------------------------------------------------------------
func _test_scarcity_above_cap() -> void:
	var sim := _make_world(false)
	# Oferece quase nada: aptidão mínima nas duas cidades e estoque zerado.
	sim.set_suitability("AGRIA", "wheat", 0.02)
	sim.set_suitability("PORTUS", "wheat", 0.02)
	sim.set_market_stock("AGRIA", "wheat", 0.0)
	sim.set_market_stock("PORTUS", "wheat", 0.0)

	var crossed := -1
	var peak := 1.0
	var trajectory: Array[float] = []
	for day in range(1, 81):
		_run(sim, 1)
		var p := _market_price(sim, "PORTUS", WHEAT)
		var ratio := p / WHEAT_BASE
		trajectory.append(ratio)
		peak = maxf(peak, ratio)
		if crossed < 0 and ratio > 3.0:
			crossed = day

	print("[price_formation] (a) trigo PORTUS: pico %.3fx, cruzou 3x no dia %d (trajetória: %s)" % [
		peak, crossed, _fmt_trajectory(trajectory)])
	if crossed < 0:
		_fail("(a) escassez persistente não levou o preço acima de 3× (pico %.3fx)" % peak)
	elif peak < 3.0:
		_fail("(a) pico %.3fx não passou de 3×" % peak)
	# D-092: sem equilíbrio o preço andava +0,05× do base/dia até 19,25× em 365 d.
	# Aqui ele pode passar de 3× (choque), mas precisa PLATEAR: o markup satura.
	var mid := trajectory[39]   # dia 40
	var late := trajectory[-1]  # dia 80
	if late > 8.0:
		_fail("(a) escassez virou escalada (%.2fx no dia 80)" % late)
	if late > mid * 1.15:
		_fail("(a) preço ainda subindo sem convergir (dia 40 %.2fx -> dia 80 %.2fx)" % [mid, late])


func _fmt_trajectory(traj: Array[float]) -> String:
	var parts: Array[String] = []
	for i in range(0, traj.size(), 8):
		parts.append("%d:%.2f" % [i + 1, traj[i]])
	return " ".join(parts)


## ---------------------------------------------------------------------------
## (b) excesso de oferta -> preço cai.
## Preço semeado em 3× (120) com estoque gigante e demanda normal: a escassez
## efetiva é ~0, o preço converge para o piso de custo (~1×).
## ---------------------------------------------------------------------------
func _test_excess_drops_price() -> void:
	var sim := _make_world(false)
	sim.set_market_price("AGRIA", "wheat", WHEAT_BASE * 3.0)
	sim.set_market_stock("AGRIA", "wheat", 100000.0)
	_run(sim, 60)
	var p := _market_price(sim, "AGRIA", WHEAT)
	var ratio := p / WHEAT_BASE
	print("[price_formation] (b) trigo AGRIA com excesso: 3.0x -> %.3fx" % ratio)
	if ratio > 2.0:
		_fail("(b) excesso não derrubou o preço (%.3fx após 60 d)" % ratio)


## ---------------------------------------------------------------------------
## (c) demanda efetiva × desejada (caso ferro).
## Mina de ferro (100 t/d) + usina FAMINTA de plena capacidade (1 Mt/d, insumo
## ferro) que nunca vende o produto: o `desired` do ferro vai ao teto da usina,
## mas o `effective` (só venda real) é ~0 — o preço do ferro NÃO infla.
## ---------------------------------------------------------------------------
func _test_effective_vs_desired() -> void:
	var sim := _make_world(false)
	var mine: Dictionary = sim.build("AGRIA", "T-MINE", "iron", -1, IRON,
		100.0, 10.0, 2.0, 5.0, 0.0, "state", 5000.0, "")
	var plant: Dictionary = sim.build("AGRIA", "T-PLANT", "coal", IRON, COAL,
		1000000.0, 100.0, 2.0, 5.0, 1.0, "state", 5000.0, "")
	if not bool(mine.get("created", false)) or not bool(plant.get("created", false)):
		_fail("(c) não criou mina/planta: mine=%s plant=%s" % [str(mine), str(plant)])
		return
	_run(sim, 30)
	var m := _market(sim, "AGRIA", IRON)
	var desired := float(m.get("desired", 0.0))
	var supply := float(m.get("supply", 0.0))
	var price := float(m.get("price", 0.0))
	var ratio := price / 60.0  # base do ferro = 60.0
	print("[price_formation] (c) ferro AGRIA: desired=%.0f supply=%.0f preço=%.2fx" % [
		desired, supply, ratio])
	if desired < supply * 100.0:
		_fail("(c) desired %.0f não é de plena capacidade vs supply %.0f (esperado >>)" % [desired, supply])
	if ratio > 2.0:
		_fail("(c) desired gigante inflou o preço do ferro (%.2fx; deveria ficar ~1x)" % ratio)


## ---------------------------------------------------------------------------
## (d) nicho sem teto: mina estatal de OURO passa de 10 t/d quando a demanda
## externa justifica. Antes da D-091 o teto era min(capacidade, 10).
## Demanda externa 100 t/d × 1,2 = 120, limitado pelo orçamento a 25 t/d.
## ---------------------------------------------------------------------------
func _test_niche_no_cap() -> void:
	var sim := _make_world(true)
	# Orçamento público que sustenta uma mina acima de 10 t/d.
	_boost_cash(sim, "AGRIA", 20000.0, 200000.0)
	sim.set_suitability("AGRIA", "gold", 1.0)
	sim.set_deposit("AGRIA", "gold", 5000.0, 0.80, 200.0)
	sim.set_external_demand("AGRIA", "gold", 100.0)
	_run(sim, 45)

	var max_cap := 0.0
	var count := 0
	for building in sim.get_city_status("AGRIA").get("buildings", []):
		if String(building.get("resource", "")) == "gold":
			count += 1
			max_cap = maxf(max_cap, float(building.get("capacity", 0.0)))
	print("[price_formation] (d) ouro AGRIA: %d mina(s), capacidade máx %.1f t/d" % [count, max_cap])
	if count == 0:
		_fail("(d) IA não abriu mina estatal de ouro")
	elif max_cap <= 10.0:
		_fail("(d) mina de ouro com teto de 10 t/d ainda ativo (capacidade %.1f)" % max_cap)


## ---------------------------------------------------------------------------
## (e) guarda numérica: choque de preço extremo + estoque vazio não produz
## NaN/inf/negativo ao longo dos ticks.
## ---------------------------------------------------------------------------
func _test_numeric_guard() -> void:
	var sim := _make_world(false)
	# Leva o preço ao teto de semeadura (10×) e esvazia a prateleira.
	sim.price_shock("wheat", 10.0)
	sim.set_market_stock("AGRIA", "wheat", 0.0)
	sim.set_market_stock("PORTUS", "wheat", 0.0)
	for day in range(1, 21):
		_run(sim, 1)
		for city_id in ["AGRIA", "PORTUS"]:
			var p := _market_price(sim, city_id, WHEAT)
			if not is_finite(p) or p <= 0.0:
				_fail("(e) preço inválido (%s) no dia %d: %s" % [city_id, day, str(p)])
				return
	var p := _market_price(sim, "AGRIA", WHEAT)
	print("[price_formation] (e) após choque 10x + estoque vazio: trigo AGRIA %.3fx (finito)" % [p / WHEAT_BASE])


## ---------------------------------------------------------------------------
## (f) convergência após choque: preço semeado em 10× volta ao piso de custo
## quando a oferta é normal. Antes da D-092 ele descia só 5% do base/dia.
## ---------------------------------------------------------------------------
func _test_shock_converges() -> void:
	var sim := _make_world(false)
	sim.set_market_price("AGRIA", "wheat", WHEAT_BASE * 10.0)
	sim.set_market_stock("AGRIA", "wheat", 100000.0)
	sim.set_market_stock("PORTUS", "wheat", 100000.0)
	_run(sim, 60)
	var p60 := _market_price(sim, "AGRIA", WHEAT) / WHEAT_BASE
	_run(sim, 60)
	var p120 := _market_price(sim, "AGRIA", WHEAT) / WHEAT_BASE
	print("[price_formation] (f) choque 10x com oferta normal: dia 60 %.3fx, dia 120 %.3fx" % [
		p60, p120])
	if p120 > 1.5:
		_fail("(f) choque não convergiu ao piso (%.3fx no dia 120)" % p120)
	if p120 > p60 * 1.05:
		_fail("(f) preço não parou de cair/subir: dia 60 %.3fx -> dia 120 %.3fx" % [p60, p120])


## ---------------------------------------------------------------------------
## (g) elasticidade por classe: com 2× o preço, o consumo de LUXO cai muito
## mais que o de BÁSICO (declarado: 0,15 básico, 1,20 luxo). Usa o cenário com
## goods/needs reais; os dois bens não têm produtor local, então o preço fica
## exatamente no valor semeado e a comparação isola a demanda.
## ---------------------------------------------------------------------------
func _test_elasticity_by_class() -> void:
	var clothes_base := _base_price_of("clothes")
	var jewelry_base := _base_price_of("jewelry")
	if clothes_base <= 0.0 or jewelry_base <= 0.0:
		_fail("(g) goods.json sem clothes/jewelry")
		return
	var one := _elasticity_run(1.0, clothes_base, jewelry_base)
	var two := _elasticity_run(2.0, clothes_base, jewelry_base)
	var basic_ratio := float(two["clothes"]) / maxf(float(one["clothes"]), 1e-9)
	var luxury_ratio := float(two["jewelry"]) / maxf(float(one["jewelry"]), 1e-9)
	print("[price_formation] (g) elasticidade 2x: básico %.3f (%.4f -> %.4f) | luxo %.3f (%.4f -> %.4f)" % [
		basic_ratio, float(one["clothes"]), float(two["clothes"]),
		luxury_ratio, float(one["jewelry"]), float(two["jewelry"])])
	# 2^-0,10 = 0,933 e 2^-1,20 = 0,435. Tolerância generosa cobre arredondamento
	# de compra/estoque, mas não confunde as classes.
	if basic_ratio < 0.90 or basic_ratio > 0.96:
		_fail("(g) básico fora da elasticidade esperada (%.3f; esperado ~0,933)" % basic_ratio)
	if luxury_ratio < 0.36 or luxury_ratio > 0.51:
		_fail("(g) luxo fora da elasticidade esperada (%.3f; esperado ~0,435)" % luxury_ratio)
	if basic_ratio < luxury_ratio + 0.20:
		_fail("(g) luxo não caiu mais que básico (%.3f vs %.3f)" % [luxury_ratio, basic_ratio])


func _elasticity_run(mult: float, clothes_base: float, jewelry_base: float) -> Dictionary:
	var sim := _make_needs_world()
	sim.set_market_stock("AGRIA", "clothes", 100000.0)
	sim.set_market_stock("AGRIA", "jewelry", 100000.0)
	sim.set_market_price("AGRIA", "clothes", clothes_base * mult)
	sim.set_market_price("AGRIA", "jewelry", jewelry_base * mult)
	_run(sim, 4)
	var clothes := _product_index(sim, "clothes")
	var jewelry := _product_index(sim, "jewelry")
	return {
		"clothes": float(_market(sim, "AGRIA", clothes).get("sold", 0.0)),
		"jewelry": float(_market(sim, "AGRIA", jewelry).get("sold", 0.0)),
	}


## ---------------------------------------------------------------------------
## (h) piso de custo: usina que compra insumo caro (ferro a 600) produzindo
## trigo não deixa o preço do produto abaixo do custo unitário. O insumo não
## tem produtor local, então o preço semeado não muda.
## ---------------------------------------------------------------------------
func _test_cost_floor() -> void:
	var sim := _make_world(false)
	var plant: Dictionary = sim.build("AGRIA", "T-BREAD", "wheat", IRON, WHEAT,
		100000.0, 100.0, 2.0, 5.0, 1.0, "state", 5000.0, "")
	if not bool(plant.get("created", false)):
		_fail("(h) não criou a usina de trigo com insumo ferro: %s" % str(plant))
		return
	sim.set_market_stock("AGRIA", "iron", 10000000.0)
	sim.set_market_price("AGRIA", "iron", 600.0)
	# Começa no teto de semeadura: evita 48 dias de prejuízo até o piso.
	sim.set_market_price("AGRIA", "wheat", WHEAT_BASE * 10.0)
	_run(sim, 60)
	var iron_price := _market_price(sim, "AGRIA", IRON)
	var wheat_price := _market_price(sim, "AGRIA", WHEAT)
	print("[price_formation] (h) piso de custo: ferro %.2f -> trigo %.2f (razão %.3f)" % [
		iron_price, wheat_price, wheat_price / maxf(iron_price, 0.001)])
	if wheat_price < iron_price * 1.05:
		_fail("(h) trigo abaixo do custo+markup (%.2f; ferro %.2f)" % [
			wheat_price, iron_price])


func _make_needs_world() -> EconomySim:
	var sim := EconomySim.new()
	sim.set_instant_construction(true)
	sim.set_goods(_load_array(GOODS_PATH))
	sim.set_needs(_load_dict(NEEDS_PATH))
	sim.initialize_scenario()
	return sim


func _product_index(sim: EconomySim, id: String) -> int:
	var products: Array = sim.get_status().get("products", [])
	return products.find(id)


func _base_price_of(id: String) -> float:
	for good in _load_array(GOODS_PATH):
		var entry: Dictionary = good
		if String(entry.get("id", "")) == id:
			return float(entry.get("base_price", 0.0))
	return 0.0


func _load_array(path: String) -> Array:
	var parsed: Variant = _load_json(path)
	return parsed if typeof(parsed) == TYPE_ARRAY else []


func _load_dict(path: String) -> Dictionary:
	var parsed: Variant = _load_json(path)
	return parsed if typeof(parsed) == TYPE_DICTIONARY else {}


func _load_json(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		push_error("price_formation_test: não abriu " + path)
		return null
	var text := file.get_as_text()
	file.close()
	return JSON.parse_string(text)


func _boost_cash(sim: EconomySim, city_id: String, firm: float, government: float) -> void:
	var snapshot: Dictionary = sim.to_dict()
	for city in snapshot.get("cities", []):
		if String(city.get("id", "")) == city_id:
			city["firm_cash"] = firm
			city["government_budget"] = government
	sim.from_dict(snapshot)
