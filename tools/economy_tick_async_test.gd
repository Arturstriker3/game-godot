extends SceneTree

## 60 dias no caminho SÍNCRONO (tick_demo direto, na main thread) contra 60 dias
## no caminho ASSÍNCRONO do jogo (WorkerThreadPool via tick_scheduler.gd) têm de
## terminar com os MESMOS números: o dia não pode depender de quem o calcula.
##
## Também valida as garantias do agendador e do status reaproveitado:
##  - nunca dois ticks simultâneos (`start()` recusa com um dia em andamento);
##  - o status publicado é um snapshot: o dia anterior continua legível depois
##    do tick seguinte (double buffer do núcleo C++);
##  - a main thread não paga o trabalho do dia (cada `poll()` é O(1)).
##
## Uso: godot --headless --path . --script res://tools/economy_tick_async_test.gd

const TickScheduler := preload("res://core/economy/tick_scheduler.gd")

const DAYS := 60
const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const PORTS_PATH := "res://data/definitions/scenario/ports.json"
const BUILDINGS_PATH := "res://data/definitions/scenario/buildings.json"
const GOODS_PATH := "res://data/definitions/economy/goods.json"

var _failures: Array[String] = []


func _init() -> void:
	var world := _load_world()
	if world.is_empty():
		print("[tick_async] FALHA: cenário não carregou.")
		quit(1)
		return

	var sync_sim = _build(world)
	var async_sim = _build(world)

	var sync_start := Time.get_ticks_usec()
	for _day in DAYS:
		sync_sim.tick_demo()
	var sync_ms := float(Time.get_ticks_usec() - sync_start) / 1000.0

	var scheduler = TickScheduler.new()
	var wall_start := Time.get_ticks_usec()
	var main_ms := 0.0
	var main_peak := 0.0
	var frames := 0
	var refused := 0
	var snapshots: Array = []
	for _day in DAYS:
		# Snapshot do dia anterior: tem de continuar intacto depois do próximo.
		snapshots.append(async_sim.get_demo_status().get("day", -1))
		if not scheduler.start(async_sim):
			_failures.append("start() recusou sem tick em andamento")
			break
		if scheduler.start(async_sim):
			_failures.append("start() aceitou um segundo tick simultâneo")
		else:
			refused += 1
		while true:
			var t0 := Time.get_ticks_usec()
			var summary: Dictionary = scheduler.poll()
			var cost := float(Time.get_ticks_usec() - t0) / 1000.0
			main_ms += cost
			main_peak = maxf(main_peak, cost)
			frames += 1
			if not summary.is_empty():
				break
			OS.delay_usec(200)
			if frames > 200000:
				_failures.append("tick assíncrono não terminou")
				break
	var async_ms := float(Time.get_ticks_usec() - wall_start) / 1000.0

	_test_same_numbers(sync_sim, async_sim)
	_test_snapshot_double_buffer(async_sim, snapshots)
	_test_backlog(refused)

	print("[tick_async] %d dias: síncrono %.0f ms (%.1f ms/dia), assíncrono %.0f ms de parede" % [
		DAYS, sync_ms, sync_ms / float(DAYS), async_ms])
	print("[tick_async] main thread no caminho assíncrono: %.1f ms no total de %d polls (pico %.2f ms)" % [
		main_ms, frames, main_peak])

	if _failures.is_empty():
		print("[tick_async] OK: 60 dias síncronos e assíncronos idênticos, um tick por vez.")
		quit(0)
	else:
		print("[tick_async] FALHAS (%d):" % _failures.size())
		for failure in _failures:
			print(" - " + failure)
		quit(1)


## Mesmo mundo, mesmo resultado: dinheiro, falências, dia e o retrato das
## cidades/pedidos têm de bater exatamente (o tick é determinístico).
func _test_same_numbers(sync_sim, async_sim) -> void:
	var sync_status: Dictionary = sync_sim.get_demo_status()
	var async_status: Dictionary = async_sim.get_demo_status()
	for key in ["day", "money_total", "bankruptcies"]:
		if sync_status.get(key) != async_status.get(key):
			_failures.append("%s difere: síncrono %s, assíncrono %s" % [
				key, str(sync_status.get(key)), str(async_status.get(key))])
	var sync_digest := _digest(sync_status)
	var async_digest := _digest(async_status)
	if sync_digest != async_digest:
		_failures.append("digest das cidades difere: %s != %s" % [
			sync_digest, async_digest])
	var sync_orders: Array = sync_status.get("trade_orders", [])
	var async_orders: Array = async_status.get("trade_orders", [])
	if sync_orders.size() != async_orders.size():
		_failures.append("pedidos de comércio: %d != %d" % [
			sync_orders.size(), async_orders.size()])
	elif not sync_orders.is_empty() and str(sync_orders) != str(async_orders):
		_failures.append("pedidos de comércio com valores diferentes")


## O status publicado só muda quando o próximo dia é publicado: segurar o
## dicionário de ontem não mostra o dia de hoje pela metade.
func _test_snapshot_double_buffer(sim, days: Array) -> void:
	if days.size() < 2:
		return
	for index in range(1, days.size()):
		if int(days[index]) != int(days[index - 1]) + 1:
			_failures.append("dia do snapshot %d fora de ordem: %s -> %s" % [
				index, str(days[index - 1]), str(days[index])])
			return
	var held: Dictionary = sim.get_demo_status()
	var held_day := int(held.get("day", -1))
	var held_money := float(held.get("money_total", 0.0))
	sim.tick_demo()
	if int(held.get("day", -1)) != held_day:
		_failures.append("o status retido mudou de dia depois de um novo tick")
	if absf(float(held.get("money_total", 0.0)) - held_money) > 0.001:
		_failures.append("o money_total do status retido mudou depois de um novo tick")


## Teto de backlog: com um dia na thread, um segundo tick é recusado.
func _test_backlog(refused: int) -> void:
	if refused != DAYS:
		_failures.append("start() duplicado recusado %d de %d vezes" % [refused, DAYS])


func _digest(status: Dictionary) -> String:
	var rows: Array = []
	for city in status.get("cities", []):
		rows.append([
			city.get("id", ""),
			snappedf(float(city.get("population", 0.0)), 0.001),
			snappedf(float(city.get("gdp", 0.0)), 0.001),
			snappedf(float(city.get("stockpile", 0.0)), 0.0001),
			snappedf(float(city.get("government_budget", 0.0)), 0.001),
			snappedf(float(city.get("firm_cash", 0.0)), 0.001),
		])
	return str(rows.hash())


func _build(world: Dictionary):
	var sim = EconomySim.new()
	sim.initialize_demo_world(world["cities"], world["ports"], world["buildings"])
	sim.demo_set_goods(world["goods"])
	return sim


func _load_world() -> Dictionary:
	var cities := _load_json(CITIES_PATH)
	var ports := _load_json(PORTS_PATH)
	var buildings := _load_json(BUILDINGS_PATH)
	var goods := _load_json(GOODS_PATH)
	if cities.is_empty() or buildings.is_empty():
		return {}
	return {
		"cities": cities,
		"ports": ports,
		"buildings": buildings,
		"goods": goods,
	}


func _load_json(path: String) -> Array:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		push_error("tick_async: não abriu %s" % path)
		return []
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data if typeof(data) == TYPE_ARRAY else []
