extends SceneTree

## Formação de preço (D-091) — headless, exit != 0 em falha.
##
## Cobre os cinco casos do fechamento da base:
##   (a) escassez real persistente  -> preço sobe ACIMA de 3× o base (trajetória + n);
##   (b) excesso de oferta          -> preço cai;
##   (c) demanda efetiva × desejada -> desired gigante + effective pequeno NÃO
##       infla o preço (caso ferro: usina faminta em plena capacidade);
##   (d) nicho sem teto             -> mina estatal de nicho passa de 10 t/d;
##   (e) guarda numérica            -> sem NaN/inf/negativo após choque extremo.
##
##   godot --headless --path . --script res://tools/price_formation_test.gd

const GeoScript := preload("res://core/economy/geography_service.gd")
const GroupNamerScript := preload("res://core/economy/group_namer.gd")

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

	if _failures.is_empty():
		print("[price_formation] OK: teto removido, demanda efetiva, nicho e guarda numérica.")
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


func _boost_cash(sim: EconomySim, city_id: String, firm: float, government: float) -> void:
	var snapshot: Dictionary = sim.to_dict()
	for city in snapshot.get("cities", []):
		if String(city.get("id", "")) == city_id:
			city["firm_cash"] = firm
			city["government_budget"] = government
	sim.from_dict(snapshot)
