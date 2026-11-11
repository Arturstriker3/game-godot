extends Node

## Mede o custo da virada do dia no caminho REAL do jogo ao vivo.
##
## Usa o autoload `Economy` (mesmo cenário, geografia e IA do jogo) e um
## `TradeConvoys` real. Para cada dia cronometra:
##   1. `sim.tick_demo()`   — núcleo C++ (economia do dia)
##   2. `get_demo_status()` — só nos dias em que a IA roda (a cada 7)
##   3. `spawn_orders()`    — custo de enfileirar os pedidos do dia
##   4. `ai.run(day)`       — investidor privado + governo
##   5. pump por frame      — busca de rota + criação de comboios (orçado)
##
## Uso:
##   godot --headless --path . res://tools/perf_day_peak.tscn -- [--days=30]

const ConvoysScript: Script = preload("res://core/economy/trade_convoys.gd")

var _days := 30
var _pump_samples: Array[float] = []


func _ready() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--days="):
			_days = int(arg.split("=")[1])

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

	var tick_ms: Array[float] = []
	var status_ms: Array[float] = []
	var enqueue_ms: Array[float] = []
	var ai_ms: Array[float] = []
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
		var status_cost := 0.0
		if day % 7 == 0:
			sim.get_demo_status()
			status_cost = _ms(d1)
		var d2 := Time.get_ticks_usec()
		var orders: Array = summary.get("trade_orders", [])
		orders_total += orders.size()
		max_orders = maxi(max_orders, orders.size())
		convoys.spawn_orders(orders)
		var d3 := Time.get_ticks_usec()
		ai.run(day)
		var d4 := Time.get_ticks_usec()

		# Pump: frames de trabalho até a fila esvaziar.
		var frames := 0
		var pump_total := 0.0
		var pump_peak := 0.0
		while true:
			var sampled := _pump_once(convoys)
			if sampled < 0.0:
				break
			frames += 1
			pump_total += sampled
			pump_peak = maxf(pump_peak, sampled)
			if frames > 20000:
				break
		var d5 := Time.get_ticks_usec()
		# Só faz sentido contar depois do pump: é ele quem cria os veículos.
		var active: int = convoys._active_convoys
		active_total += active

		tick_ms.append(float(d1 - d0) / 1000.0)
		status_ms.append(status_cost)
		enqueue_ms.append(float(d3 - d2) / 1000.0)
		ai_ms.append(float(d4 - d3) / 1000.0)
		turn_ms.append(float(d4 - d0) / 1000.0)
		pump_ms.append(pump_total)
		pump_frames.append(float(frames))
		peak_ms.append(maxf(float(d4 - d0) / 1000.0, pump_peak))

		print("[perf] dia %3d: tick %6.1f  status %5.1f  fila %5.2f  IA %5.1f  virada %6.1f | pump %6.2f ms em %3d frames (pico %5.2f)  [%d pedidos, %d comboios, %d entregues]" % [
			day, tick_ms[-1], status_ms[-1], enqueue_ms[-1], ai_ms[-1], turn_ms[-1],
			pump_total, frames, pump_peak, orders.size(), active, delivered])

	print("\n[perf] virada do dia — %d dias, sob demanda (pump por frame)" % _days)
	print("[perf] grade de navegação/prontidão dos comboios: %.1f ms (uma vez)" % build_ms)
	print("[perf] pedidos/dia: média %.1f, máximo %d; comboios ativos/dia (pós-pump): %.1f; entregues/dia: %.1f" % [
		float(orders_total) / float(_days), max_orders,
		float(active_total) / float(_days), float(delivered_total) / float(_days)])
	print("[perf] %-26s %8s %8s %8s" % ["etapa", "média", "p95", "máx"])
	_report("1. tick_demo (C++)", tick_ms)
	_report("2. get_demo_status (dias de IA)", status_ms)
	_report("3. enfileirar pedidos", enqueue_ms)
	_report("4. IA econômica", ai_ms)
	_report("   VIRADA (tick+status+IA)", turn_ms)
	_report("   PUMP total/dia", pump_ms)
	_report("   PUMP por frame", _pump_samples)
	_report("   FRAME DE PICO (virada|pump)", peak_ms)
	var frames_total := 0.0
	for value in pump_frames:
		frames_total += value
	print("[perf] pump: %.1f frames/dia em média; orçamento %d µs/frame" % [
		frames_total / float(_days), convoys.FRAME_BUDGET_US])

	get_tree().quit(0)


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
