extends SceneTree

## Teste da mecânica de porto v1 (docs/economy/port_design.md), em mundo
## sintético e determinístico:
##   1. porto nacional: cidade SEM porto exporta pelo porto da nação (a
##      cidade-sede `port_of` não é dona exclusiva do tráfego);
##   2. rota entre massas de terra = caminhão cidade→porto, navio porto→porto,
##      caminhão porto→cidade;
##   3. capacidade diária: porto saturado gera fila (vazão acima da
##      capacidade, atraso e demurrage) e a chegada é adiada;
##   4. expansão: custo pelo orçamento da sede, obra de 30 dias, capacidade
##      maior que a vazão que saturava o cais;
##   5. nação sem porto não recebe travessia marítima (só terrestre);
##   6. dinheiro conservado em todos os dias (fila e obra só movem caixa).
##
## Uso: godot --headless --path . --script res://tools/port_capacity_test.gd

const MONEY_EPSILON := 0.5
## Capacidade artificial do porto exportador no teste de saturação (t/dia).
const SATURATED_CAPACITY := 2000.0
const EXPANSION_TONS := 5000.0
const EXPANSION_COST := EXPANSION_TONS * 500.0
const BUILD_DAYS := 30

var _failures: Array[String] = []
var _money_initial := 0.0


func _init() -> void:
	var sim = EconomySim.new()
	sim.initialize_world(_cities(), _ports(), _buildings())

	var initial: Dictionary = sim.get_status()
	_money_initial = float(initial["money_total"])
	_check_initial_ports(initial)

	# Capacidade artificial antes do 1º dia: a carga de 2 700 t do comprador
	# AAA-C (3 dias de cobertura para 900 t/dia) excede o cais de 2 000 t/dia.
	sim.set_port_capacity("AAA-P1", SATURATED_CAPACITY)

	var status: Dictionary = sim.tick_day()
	_check_conservation(status, 1)
	_check_no_port_no_sea(status)
	var order := _find_order(status, "AAA-B", "AAA-C")
	if order.is_empty():
		_fail("dia 1: nenhum pedido AAA-B -> AAA-C (sem rota marítima)")
	else:
		_check_sea_route(order)
	_check_host_not_exclusive(status)

	# Saturação: vazão acima da capacidade, fila e chegada adiada.
	var port: Dictionary = _find_port(status, "AAA-P1")
	var throughput := float(port.get("throughput", 0.0))
	var queue := float(port.get("queue", 0.0))
	var delay := float(port.get("delay_hours", 0.0))
	if throughput <= SATURATED_CAPACITY:
		_fail("saturação: vazão %.1f t não passou da capacidade %.1f t" % [
			throughput, SATURATED_CAPACITY])
	if queue <= 0.0:
		_fail("saturação: porto sem fila (queue=%.2f)" % queue)
	if delay <= 0.0:
		_fail("saturação: porto sem atraso (delay=%.2f h)" % delay)
	print("[port] saturação: vazão=%.1f t, fila=%.1f t, atraso=%.1f h" % [
		throughput, queue, delay])
	_check_delayed_arrival(sim, 1)

	# Expansão: obra paga pela sede e concluída em 30 dias.
	var before_expansion: Dictionary = sim.get_status()
	var host_budget := _city_budget(before_expansion, "AAA-A")
	var money_before := float(before_expansion["money_total"])
	var capacity_before := float(_find_port(before_expansion, "AAA-P1")["capacity_t_per_day"])
	var result: Dictionary = sim.expand_port("AAA-P1", EXPANSION_TONS)
	if not bool(result.get("expanded", false)):
		_fail("expansão: recusada (%s)" % String(result.get("error", "?")))
	if absf(float(result.get("cost", 0.0)) - EXPANSION_COST) > 0.01:
		_fail("expansão: custo %.2f != %.2f" % [
			float(result.get("cost", 0.0)), EXPANSION_COST])
	if int(result.get("days", 0)) != BUILD_DAYS:
		_fail("expansão: prazo %d != %d dias" % [
			int(result.get("days", 0)), BUILD_DAYS])
	var after_payment: Dictionary = sim.get_status()
	if absf(host_budget - _city_budget(after_payment, "AAA-A") - EXPANSION_COST) > 0.01:
		_fail("expansão: orçamento da sede não debitado")
	if absf(float(after_payment["money_total"]) - money_before) > MONEY_EPSILON:
		_fail("expansão: dinheiro não conservado")

	for day in range(2, 2 + BUILD_DAYS):
		status = sim.tick_day()
		_check_conservation(status, day)
	var capacity_after := float(_find_port(status, "AAA-P1")["capacity_t_per_day"])
	if absf(capacity_after - (capacity_before + EXPANSION_TONS)) > 0.01:
		_fail("expansão: capacidade %.1f != %.1f" % [
			capacity_after, capacity_before + EXPANSION_TONS])
	# A vazão que saturava o cais (vista no dia 1) cabe na capacidade nova: com
	# a obra concluída o mesmo fluxo diário não gera fila.
	if capacity_after < throughput:
		_fail("expansão: capacidade %.1f < vazão que saturava (%.1f)" % [
			capacity_after, throughput])
	print("[port] expansão: capacidade %.0f -> %.0f t/dia (nível %d), vazão de pico %.0f t" % [
		capacity_before, capacity_after,
		int(_find_port(status, "AAA-P1")["level"]), throughput])

	status = sim.tick_day()
	_check_conservation(status, 2 + BUILD_DAYS)
	var expanded_port: Dictionary = _find_port(status, "AAA-P1")
	if float(expanded_port["queue"]) > 0.01:
		_fail("expansão: porto ainda congestionado (fila %.1f t)" % [
			float(expanded_port["queue"])])
	if float(expanded_port["throughput"]) <= 0.0:
		_fail("expansão: nenhuma carga passou no cais depois da obra")
	print("[port] pós-obra: vazão=%.1f t, fila=%.1f t" % [
		float(expanded_port["throughput"]), float(expanded_port["queue"])])

	if _failures.is_empty():
		print("[port] OK: porto nacional, fila, expansão e conservação validados.")
		quit(0)
		return
	print("[port] FALHAS (%d):" % _failures.size())
	for failure in _failures:
		print(" - " + failure)
	quit(1)


## Nação AAA em duas massas de terra (norte/sul) e BBB sem porto:
##   AAA-A: sede do porto AAA-P1 (norte), sem lavoura, importa por caminhão.
##   AAA-B: terra adentro, SEM porto, exporta pelo AAA-P1 (regra nacional).
##   AAA-C: no sul, sede do AAA-P2, sem lavoura: importa por navio.
##   BBB-D: no sul, SEM porto: não pode receber carga marítima.
func _cities() -> Array:
	return [
		{"id": "AAA-A", "name": "A", "nation": "AAA", "home_nation": "AAA",
			"landmass": "north", "population": 6_000_000.0,
			"x": 2500.0, "y": 600.0},
		{"id": "AAA-B", "name": "B", "nation": "AAA", "home_nation": "AAA",
			"landmass": "north", "population": 1_000_000.0,
			"x": 2500.0, "y": 640.0},
		{"id": "AAA-C", "name": "C", "nation": "AAA", "home_nation": "AAA",
			"landmass": "south", "population": 3_000_000.0,
			"x": 2600.0, "y": 900.0},
		{"id": "BBB-D", "name": "D", "nation": "BBB", "home_nation": "BBB",
			"landmass": "south", "population": 1_000_000.0,
			"x": 2650.0, "y": 920.0},
	]


func _ports() -> Array:
	return [
		# Sede AAA-A; o cais fica a ~11 km da cidade (trecho terrestre existe).
		{"id": "AAA-P1", "name": "Porto A", "nation": "AAA",
			"landmass": "north", "port_of": "AAA-A",
			"x": 2500.0, "y": 600.1},
		{"id": "AAA-P2", "name": "Porto C", "nation": "AAA",
			"landmass": "south", "port_of": "AAA-C",
			"x": 2600.0, "y": 900.4},
	]


func _buildings() -> Array:
	return [
		{"city_id": "AAA-B", "id": "AAA-B-FARM", "name": "fazenda B",
			"resource": "wheat", "output_tons_per_day": 10_000.0},
	]


func _check_initial_ports(status: Dictionary) -> void:
	var ports: Array = status.get("ports", [])
	if ports.size() != 2:
		_fail("status: esperava 2 portos (tem %d)" % ports.size())
		return
	var a: Dictionary = _find_port(status, "AAA-P1")
	if a.is_empty():
		return
	if String(a.get("host_city", "")) != "AAA-A":
		_fail("porto: host_city deveria ser AAA-A (port_of)")
	# Capacidade de nível 1 pela população da sede: 1000 + 0,002 × 6 M = 13 000.
	if absf(float(a.get("capacity_t_per_day", 0.0)) - 13000.0) > 0.01:
		_fail("porto: capacidade inicial %.1f != 13000 t/dia" % [
			float(a.get("capacity_t_per_day", 0.0))])
	if int(a.get("level", 0)) != 1:
		_fail("porto: nível inicial deveria ser 1")


## BBB-D está no sul sem porto: nenhuma oferta marítima pode chegar.
func _check_no_port_no_sea(status: Dictionary) -> void:
	for order in status.get("trade_orders", []):
		if String(order.get("to_city", "")) == "BBB-D":
			_fail("estratégia: BBB-D (sem porto) recebeu pedido %s" %
				String(order.get("from_city", "?")))
		for leg in order.get("legs", []):
			if String(leg.get("mode", "")) == "ship":
				if String(order.get("from_nation", "")) == "BBB" \
						or String(order.get("to_nation", "")) == "BBB":
					_fail("estratégia: travessia marítima envolvendo nação sem porto")


func _check_sea_route(order: Dictionary) -> void:
	var legs: Array = order.get("legs", [])
	if legs.size() < 2:
		_fail("rota: pedido marítimo sem porto (legs=%d)" % legs.size())
		return
	var modes: Array[String] = []
	for leg in legs:
		modes.append(String(leg.get("mode", "")))
	if not modes.has("ship"):
		_fail("rota: nenhum trecho de navio (modes=%s)" % str(modes))
	# Primeiro trecho terrestre termina no porto da nação do vendedor.
	if not String(legs[0].get("to", "")).begins_with("AAA-P"):
		_fail("rota: primeiro trecho não termina no porto nacional (%s)" %
			String(legs[0].get("to", "?")))
	if not String(legs[0].get("from", "")).begins_with("AAA-B"):
		_fail("rota: carga deveria sair de AAA-B (saiu de %s)" %
			String(legs[0].get("from", "?")))
	var ship: Dictionary = legs[modes.find("ship")]
	if String(ship.get("from", "")).is_empty() or String(ship.get("to", "")).is_empty():
		_fail("rota: trecho de navio sem cais de origem/destino")


## A sede do porto não é a única cidade que o usa: AAA-B (sem porto) exporta.
func _check_host_not_exclusive(status: Dictionary) -> void:
	var used := false
	for order in status.get("trade_orders", []):
		if String(order.get("from_city", "")) != "AAA-B":
			continue
		for leg in order.get("legs", []):
			if String(leg.get("to", "")) == "AAA-P1":
				used = true
	if not used:
		_fail("porto nacional: AAA-B (sem porto, não-sede) não exportou por AAA-P1")
	else:
		print("[port] AAA-B (sem porto) exportou pelo cais da sede AAA-A.")


func _check_delayed_arrival(sim, day: int) -> void:
	var data: Dictionary = sim.to_dict()
	var delayed := 0
	for shipment in data.get("in_transit", []):
		if int(shipment.get("arrival_day", 0)) > day + 1:
			delayed += 1
	if delayed == 0:
		_fail("fila: nenhuma carga teve a chegada adiada")


func _check_conservation(status: Dictionary, day: int) -> void:
	if absf(float(status["money_total"]) - _money_initial) > MONEY_EPSILON:
		_fail("dia %d: dinheiro %.4f != %.4f" % [
			day, float(status["money_total"]), _money_initial])
	if int(status.get("invariant_violations", []).size()) > 0:
		_fail("dia %d: invariante violado: %s" % [
			day, String(status["invariant_violations"][0])])


func _find_order(status: Dictionary, from_city: String, to_city: String) -> Dictionary:
	for order in status.get("trade_orders", []):
		if String(order.get("from_city", "")) == from_city \
				and String(order.get("to_city", "")) == to_city:
			return order
	return {}


func _find_port(status: Dictionary, port_id: String) -> Dictionary:
	for port in status.get("ports", []):
		if String(port.get("id", "")) == port_id:
			return port
	return {}


func _city_budget(status: Dictionary, city_id: String) -> float:
	for city in status.get("cities", []):
		if String(city.get("id", "")) == city_id:
			return float(city.get("government_budget", 0.0))
	return 0.0


func _fail(message: String) -> void:
	_failures.append(message)
