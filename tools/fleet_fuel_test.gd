extends SceneTree

## F4 — frete exige frota e combustível.
##
## Cenário mínimo AGRIA/PORTUS (determinístico, 2 cidades, 1 nação DEMO), com os
## 113 bens carregados para os estoques de fuel/truck/spare_parts existirem.
## Valida o contrato da frota nacional:
##   - frota inicial é derivada de população/PIB (`source: derived`);
##   - sem caminhão, nenhum pedido terrestre embarca;
##   - com caminhão e sem combustível, nenhum pedido embarca;
##   - com frota e combustível, o pedido embarca;
##   - comprar `truck` no mercado interno aumenta a capacidade da frota;
##   - um mês sem `spare_parts` reduz a saúde (capacidade efetiva) da frota.
##
##   godot --headless --path . --script res://tools/fleet_fuel_test.gd

const GOODS_PATH := "res://data/definitions/economy/goods.json"
const DEMO_NATION := "DEMO"

var _failures: Array[String] = []


func _init() -> void:
	var goods := _load_array(GOODS_PATH)
	if goods.is_empty():
		_fail("goods.json vazio")
		_finish()
		return
	var sim = EconomySim.new()
	sim.demo_set_goods(goods)
	sim.initialize_demo()

	# Aquecimento: produção/estoque e o primeiro pedido de trigo.
	for _day in 4:
		sim.tick_demo()
	var warm: Dictionary = sim.get_demo_status()
	if _order_count(warm) <= 0:
		_fail("cenário AGRIA/PORTUS não gerou pedido no aquecimento")

	var derived: Dictionary = sim.demo_get_fleet(DEMO_NATION)
	if String(derived.get("source", "")) != "derived":
		_fail("frota inicial deveria ser derivada (source=%s)" % [
			String(derived.get("source", "?"))])
	if float(derived.get("trucks", 0.0)) <= 0.0:
		_fail("frota inicial derivada sem caminhões")
	print("[fleet] derivada: trucks=%.0f ships=%.0f planes=%.0f fuel=%.1f parts=%.1f" % [
		float(derived.get("trucks", 0.0)), float(derived.get("ships", 0.0)),
		float(derived.get("planes", 0.0)), float(derived.get("fuel", 0.0)),
		float(derived.get("spare_parts", 0.0))])

	# 1. Sem frota: nenhum pedido terrestre sai (fuel disponível).
	sim.demo_set_fleet(DEMO_NATION, 0.0, 0.0, 0.0, 100000.0, 100.0)
	var no_fleet: Dictionary = sim.tick_demo()
	if _order_count(no_fleet) > 0:
		_fail("embarcou %d pedidos sem frota" % _order_count(no_fleet))
	var fleet_after_block: Dictionary = sim.demo_get_fleet(DEMO_NATION)
	if float(fleet_after_block.get("queue_today", 0.0)) <= 0.0:
		_fail("fila não registrou a carga barrada por falta de frota")
	print("[fleet] sem frota: pedidos=%d fila=%.1f t (sem_veiculo=%.1f)" % [
		_order_count(no_fleet), float(fleet_after_block.get("queue_today", 0.0)),
		float(fleet_after_block.get("missed_no_fleet", 0.0))])

	# 2. Frota, mas sem combustível: não embarca.
	sim.demo_set_fleet(DEMO_NATION, 50.0, 5.0, 5.0, 0.0, 100.0)
	var no_fuel: Dictionary = sim.tick_demo()
	if _order_count(no_fuel) > 0:
		_fail("embarcou %d pedidos sem combustível" % _order_count(no_fuel))
	var fleet_no_fuel: Dictionary = sim.demo_get_fleet(DEMO_NATION)
	if float(fleet_no_fuel.get("missed_no_fuel", 0.0)) <= 0.0:
		_fail("fila não registrou a carga barrada por falta de combustível")
	print("[fleet] sem fuel: pedidos=%d fila=%.1f t (sem_fuel=%.1f)" % [
		_order_count(no_fuel), float(fleet_no_fuel.get("queue_today", 0.0)),
		float(fleet_no_fuel.get("missed_no_fuel", 0.0))])

	# 3. Frota + combustível: o pedido volta a embarcar.
	sim.demo_set_fleet(DEMO_NATION, 50.0, 5.0, 5.0, 100000.0, 100.0)
	var with_fleet: Dictionary = sim.tick_demo()
	if _order_count(with_fleet) <= 0:
		_fail("não embarcou com frota e combustível disponíveis")
	print("[fleet] com frota: pedidos=%d" % _order_count(with_fleet))

	# 4. Compra de veículo no mercado interno aumenta a capacidade.
	sim.demo_set_market_stock("AGRIA", "truck", 20.0)
	var before_buy: Dictionary = sim.demo_get_fleet(DEMO_NATION)
	var buy: Dictionary = sim.demo_fleet_buy(DEMO_NATION, "truck", 4.0)
	var after_buy: Dictionary = sim.demo_get_fleet(DEMO_NATION)
	print("[fleet] compra truck: bought=%.2f spent=%.2f trucks %.2f -> %.2f" % [
		float(buy.get("bought", 0.0)), float(buy.get("spent", 0.0)),
		float(before_buy.get("trucks", 0.0)), float(after_buy.get("trucks", 0.0))])
	if float(buy.get("bought", 0.0)) < 3.99:
		_fail("compra de truck não achou o estoque de teste (bought=%.2f, erro=%s)" % [
			float(buy.get("bought", 0.0)), String(buy.get("error", ""))])
	if float(after_buy.get("trucks", 0.0)) <= float(before_buy.get("trucks", 0.0)):
		_fail("compra de truck não aumentou a frota")
	if float(after_buy.get("capacity_tons", 0.0)) <= float(before_buy.get("capacity_tons", 0.0)):
		_fail("compra de truck não aumentou a capacidade")
	var demand_status: Dictionary = sim.get_demo_status()
	if _market_demand(demand_status, "truck") <= 0.0:
		_fail("compra de truck não apareceu como demanda no mercado")

	# 5. Desgaste: um mês sem peças reduz a saúde/capacidade efetiva.
	sim.demo_set_fleet(DEMO_NATION, 100.0, 10.0, 2.0, 100000.0, 0.0)
	var before_wear: Dictionary = sim.demo_get_fleet(DEMO_NATION)
	for _day in 30:
		sim.tick_demo()
	var after_wear: Dictionary = sim.demo_get_fleet(DEMO_NATION)
	var health: Array = after_wear.get("health", [])
	var truck_health: float = float(health[0]) if health.size() > 0 else 1.0
	print("[fleet] desgaste 30 d sem peças: health=%.3f capacidade=%.0f -> %.0f" % [
		truck_health, float(before_wear.get("capacity_tons", 0.0)),
		float(after_wear.get("capacity_tons", 0.0))])
	if truck_health >= 0.999:
		_fail("saúde da frota não caiu sem spare_parts em 30 dias")
	if float(after_wear.get("capacity_tons", 0.0)) >= float(before_wear.get("capacity_tons", 0.0)):
		_fail("capacidade efetiva não caiu sem peças")

	_finish()


func _order_count(status: Dictionary) -> int:
	return Array(status.get("trade_orders", [])).size()


func _market_demand(status: Dictionary, product: String) -> float:
	var products: Array = status.get("products", [])
	var index := products.find(product)
	if index < 0:
		return 0.0
	var total := 0.0
	for city in status.get("cities", []):
		var markets: Array = city.get("markets", [])
		if index < markets.size():
			total += float(markets[index].get("desired", 0.0))
	return total


func _load_array(path: String) -> Array:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("nao abriu " + path)
		return []
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	if typeof(data) != TYPE_ARRAY:
		_fail(path + " nao e um array JSON")
		return []
	return data


func _fail(message: String) -> void:
	_failures.append(message)


func _finish() -> void:
	if _failures.is_empty():
		print("[fleet] OK: frota/combustível/compra/desgaste validados.")
		quit(0)
	else:
		print("[fleet] FALHAS (%d):" % _failures.size())
		for failure in _failures:
			print(" - " + failure)
		quit(1)
