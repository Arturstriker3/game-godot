extends Node

## Mede o custo da virada do dia no caminho REAL do jogo ao vivo.
##
## Usa o autoload `Economy` (mesmo cenário, geografia e IA do jogo) e um
## `TradeConvoys` real. Para cada dia cronometra:
##   1. `sim.tick_demo()`   — núcleo C++ (economia do dia)
##   2. `spawn_orders()`    — custo de enfileirar os pedidos do dia
##   3. `ai.plan(day)`      — agenda as cidades do dia (IA escalonada)
##   5. pump por frame      — IA fatiada (1,5 ms) + busca de rota/comboios
##   6. mapa (--map-mode)   — `city_world.refresh_modes`: camada do modo +
##                            textura de território, no mesmo frame da virada.
##
## Uso:
##   godot --headless --path . res://tools/perf_day_peak.tscn -- [--days=30] \
##       [--map-mode=none|political|resources] [--map-territory=0|1]

const ConvoysScript: Script = preload("res://core/economy/trade_convoys.gd")
const ModeLayerScript: Script = preload("res://map/map_mode_layer.gd")
const TerritoryMapScript: Script = preload("res://map/territory_map.gd")

## Espelha `Economy.AI_FRAME_BUDGET_US` (orçamento do planejamento por frame).
const AI_BUDGET_US := 1500

var _days := 30
var _map_mode := "none"
var _map_territory := true
var _pump_samples: Array[float] = []


func _ready() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--days="):
			_days = int(arg.split("=")[1])
		elif arg.begins_with("--map-mode="):
			_map_mode = String(arg.split("=")[1])
		elif arg.begins_with("--map-territory="):
			_map_territory = bool(int(arg.split("=")[1]))

	# Congela o relógio: quem dirige os dias é este laço.
	Economy.set_speed(0)
	var sim: Object = Economy.sim
	var ai: Object = Economy._ai
	if sim == null or ai == null:
		print("[perf] autoload Economy sem sim/IA")
		get_tree().quit(1)
		return

	var convoys = ConvoysScript.new()
	var build_start := Time.get_ticks_usec()
	add_child(convoys)
	var build_ms := _ms(build_start)

	# Mapa real: mesma `refresh_modes()` que `main_game._on_status_updated`
	# chama a cada dia (camada do modo + textura de território).
	var mode_layer = null
	var territory = null
	var map_build_ms := 0.0
	if _map_mode != "none":
		var map_cities: Array = sim.get_demo_status().get("cities", [])
		var map_start := Time.get_ticks_usec()
		mode_layer = ModeLayerScript.new()
		add_child(mode_layer)
		mode_layer.setup(map_cities)
		mode_layer.set_mode(_map_mode)
		if _map_territory:
			territory = TerritoryMapScript.new()
			territory.load_data()
			territory.assign(map_cities)
			territory.update(map_cities, {})
		map_build_ms = _ms(map_start)

	var tick_ms: Array[float] = []
	var enqueue_ms: Array[float] = []
	var ai_ms: Array[float] = []
	var ai_pump_ms: Array[float] = []
	var map_ms: Array[float] = []
	var layer_ms: Array[float] = []
	var territory_ms: Array[float] = []
	var turn_ms: Array[float] = []
	var pump_ms: Array[float] = []
	var pump_frames: Array[float] = []
	var peak_ms: Array[float] = []
	var orders_total := 0
	var max_orders := 0
	var active_total := 0
	var delivered_total := 0

	for day in range(1, _days + 1):
		# O dia anterior terminou: com os pools, os comboios do teste não rodam
		# tween nenhum; devolve todos ao pool para liberar as vagas do dia.
		# O que sai aqui é o que chegou ao destino no dia anterior.
		var delivered := _release_convoys(convoys)
		delivered_total += delivered

		var d0 := Time.get_ticks_usec()
		var summary: Dictionary = sim.tick_demo()
		var d1 := Time.get_ticks_usec()
		# Sem `get_demo_status()` extra: a IA reusa o próprio resumo do tick.
		var d2 := Time.get_ticks_usec()
		var orders: Array = summary.get("trade_orders", [])
		orders_total += orders.size()
		max_orders = maxi(max_orders, orders.size())
		convoys.spawn_orders(orders)
		var d3 := Time.get_ticks_usec()
		ai.plan(day, summary)
		var d4 := Time.get_ticks_usec()
		var map_cost := 0.0
		var layer_cost := 0.0
		var territory_cost := 0.0
		if mode_layer != null or territory != null:
			var cities: Array = summary.get("cities", [])
			if mode_layer != null:
				var l0 := Time.get_ticks_usec()
				mode_layer.refresh(cities)
				layer_cost = _ms(l0)
			if territory != null:
				var t0 := Time.get_ticks_usec()
				territory.update(cities, {})
				territory_cost = _ms(t0)
			map_cost = layer_cost + territory_cost

		# Pump: frames de trabalho até a fila de comboios E a IA esvaziarem.
		# Mesmo comportamento do jogo: a IA (orçada) e os comboios (orçados)
		# rodam em frames seguintes à virada, nunca todos no frame do tick.
		var frames := 0
		var pump_total := 0.0
		var pump_peak := 0.0
		var ai_pump_total := 0.0
		while true:
			var frame_cost := 0.0
			if ai.is_busy():
				frame_cost += _pump_ai(ai)
			var sampled := _pump_once(convoys)
			if sampled >= 0.0:
				frame_cost += sampled
			if not ai.is_busy() and sampled < 0.0:
				break
			frames += 1
			if sampled >= 0.0:
				pump_total += sampled
			ai_pump_total += frame_cost - maxf(sampled, 0.0)
			pump_peak = maxf(pump_peak, frame_cost)
			if frames > 20000:
				break
		# Só faz sentido contar depois do pump: é ele quem cria os veículos.
		var active: int = convoys._active_convoys
		active_total += active

		tick_ms.append(float(d1 - d0) / 1000.0)
		enqueue_ms.append(float(d3 - d2) / 1000.0)
		ai_ms.append(float(d4 - d3) / 1000.0)
		ai_pump_ms.append(ai_pump_total)
		map_ms.append(map_cost)
		layer_ms.append(layer_cost)
		territory_ms.append(territory_cost)
		turn_ms.append(float(d4 - d0) / 1000.0 + map_cost)
		pump_ms.append(pump_total)
		pump_frames.append(float(frames))
		peak_ms.append(maxf(float(d4 - d0) / 1000.0 + map_cost, pump_peak))

		print("[perf] dia %3d: tick %6.1f  fila %5.2f  plano IA %5.2f  mapa %5.2f  virada %6.1f | pump %6.2f ms em %3d frames (pico %5.2f, IA %5.2f)  [%d pedidos, %d comboios, %d entregues]" % [
			day, tick_ms[-1], enqueue_ms[-1], ai_ms[-1], map_cost, turn_ms[-1],
			pump_total, frames, pump_peak, ai_pump_total,
			orders.size(), active, delivered])

	print("\n[perf] virada do dia — %d dias, sob demanda (pump por frame)" % _days)
	print("[perf] grade de navegação/prontidão dos comboios: %.1f ms (uma vez)" % build_ms)
	if _map_mode != "none":
		print("[perf] mapa (%s, território=%s) montado em %.1f ms (uma vez)" % [
			_map_mode, str(_map_territory), map_build_ms])
	print("[perf] pedidos/dia: média %.1f, máximo %d; comboios ativos/dia (pós-pump): %.1f; entregues/dia: %.1f" % [
		float(orders_total) / float(_days), max_orders,
		float(active_total) / float(_days), float(delivered_total) / float(_days)])
	print("[perf] %-26s %8s %8s %8s" % ["etapa", "média", "p95", "máx"])
	_report("1. tick_demo (C++)", tick_ms)
	_report("2. enfileirar pedidos", enqueue_ms)
	_report("3. IA planejar (agenda do dia)", ai_ms)
	_report("   IA trabalho fatiado/dia", ai_pump_ms)
	if _map_mode != "none":
		_report("4. mapa (camada+territ.)", map_ms)
		_report("   - camada do modo", layer_ms)
		_report("   - território", territory_ms)
	_report("   VIRADA (tick+plano+mapa)", turn_ms)
	_report("   PUMP comboios/dia", pump_ms)
	_report("   PUMP por frame", _pump_samples)
	_report("   FRAME DE PICO (virada|frames)", peak_ms)
	var frames_total := 0.0
	for value in pump_frames:
		frames_total += value
	print("[perf] pump: %.1f frames/dia em média; orçamento %d µs/frame" % [
		frames_total / float(_days), convoys.FRAME_BUDGET_US])

	get_tree().quit(0)


## Um frame de IA (orçado). Devolve o custo em ms.
func _pump_ai(ai) -> float:
	var start := Time.get_ticks_usec()
	ai.pump(AI_BUDGET_US)
	return float(Time.get_ticks_usec() - start) / 1000.0


## Um frame de pump (mede o custo e devolve o tempo gasto, ou -1 se a fila
## já está vazia).
func _pump_once(convoys) -> float:
	if convoys._queue.is_empty() and convoys._prep.is_empty() and not convoys._nav.active:
		return -1.0
	var start := Time.get_ticks_usec()
	convoys._pump(start + convoys.FRAME_BUDGET_US)
	var elapsed := float(Time.get_ticks_usec() - start) / 1000.0
	_pump_samples.append(elapsed)
	return elapsed


## Encerra os comboios do dia como o tween faria no jogo (sem rodar 60 s de
## animação): mata os tweens e devolve os nós ao pool. Devolve quantos comboios
## chegaram ao destino (os que estavam ativos no fim do dia anterior).
func _release_convoys(convoys) -> int:
	for tween in convoys._tweens:
		if tween != null and tween.is_valid():
			tween.kill()
	convoys._tweens.clear()
	var alive: Array = convoys._convoys.duplicate()
	for convoy in alive:
		if is_instance_valid(convoy):
			convoys._finish_convoy(convoy)
	return alive.size()


func _ms(start_usec: int) -> float:
	return float(Time.get_ticks_usec() - start_usec) / 1000.0


func _report(label: String, values: Array[float]) -> void:
	if values.is_empty():
		return
	var sorted := values.duplicate()
	sorted.sort()
	var total := 0.0
	for value in values:
		total += value
	var mean := total / float(values.size())
	var p95_index: int = clampi(int(ceil(float(sorted.size()) * 0.95)) - 1, 0, sorted.size() - 1)
	print("[perf] %-26s %8.2f %8.2f %8.2f  ms" % [label, mean, sorted[p95_index], sorted[sorted.size() - 1]])
