extends Node

## Mede o custo da virada do dia no caminho REAL do jogo ao vivo.
##
## Usa o autoload `Economy` (mesmo cenário, geografia e IA do jogo) e um
## `TradeConvoys` real. Dois modos de tick:
##
## - `--tick-mode=sync` (padrão histórico): chama `sim.tick_day()` na main
##   thread. O tick C++ aparece inteiro dentro do "frame da virada" — é o que
##   este harness sempre mediu, mas NÃO é o caminho do jogo.
## - `--tick-mode=worker`: agenda `tick_and_publish()` no
##   `WorkerThreadPool` exatamente como `Economy._process` faz (via
##   `core/economy/tick_scheduler.gd`) e mede, por frame, só o que a main
##   thread paga: `poll()` + publicação (fila de pedidos, IA, mapa, log do HUD).
##   É o caminho real do jogo; o tempo do tick vira "parede do worker".
##
## Para cada dia cronometra:
##   1. tick C++            — `tick_day()` (sync) ou parede do worker
##   1b. `poll()` da main   — consulta do dia concluído (worker)
##   2. `spawn_orders()`    — custo de enfileirar os pedidos do dia
##   3. `ai.plan(day)`      — agenda as cidades do dia (IA escalonada)
##   3b. log de pedidos     — espelho de `hud._log_orders` (duplica os pedidos)
##   5. pump por frame      — IA fatiada (1,5 ms) + busca de rota/comboios
##   6. mapa (--map-mode)   — `city_world.refresh_modes`: camada do modo +
##                            textura de território, no mesmo frame da virada.
##
## `--profile=1` liga `set_profiling` e reporta as fases do tick C++.
##
## Uso:
##   godot --headless --path . res://tools/perf_day_peak.tscn -- [--days=30] \
##       [--tick-mode=sync|worker] [--profile=0|1] \
##       [--map-mode=none|political|resources] [--map-territory=0|1] \
##       [--threads=N] [--json=<caminho>]
##
## `--json=<caminho>` grava ao final um JSON UTF-8 (chaves ordenadas) com
## `meta`, `metrics` por etapa e, com `--profile=1`, `phases` do tick C++.
## É o insumo de `tools/bench_canonical.ps1` (bench canônico A0 —
## docs/plans/perf_pop_financas_plano.md). Sem `--json`, nada é gravado.

const ConvoysScript: Script = preload("res://map/convoy_spacing.gd")
const ModeLayerScript: Script = preload("res://map/map_mode_layer.gd")
const TerritoryMapScript: Script = preload("res://map/territory_map.gd")
const TickSchedulerScript: Script = preload("res://core/economy/tick_scheduler.gd")
const AlertsScript: Script = preload("res://core/ui/alerts.gd")

## Espelha `Economy.AI_FRAME_BUDGET_US` (orçamento do planejamento por frame).
const AI_BUDGET_US := 1500
## Espelha `hud.MAX_ORDER_LOG` (pedidos retidos na janela de Comércio).
const MAX_ORDER_LOG := 240
## Fases do tick C++, na ordem em que o núcleo as reporta.
const PHASES := [
	"arrival", "reset", "labor", "production", "raw", "income", "services",
	"trade", "accounting", "prices", "clamp", "fiscal", "fx", "invariants",
	"status", "status_cities", "status_orders", "status_nations", "status_misc",
	"trade_food", "trade_food_buyers", "trade_goods", "trade_goods_loop",
]

var _days := 30
var _map_mode := "none"
var _map_territory := true
var _tick_mode := "sync"
var _profile := false
var _threads := 0
## Caminho do JSON de saída (`--json`); vazio = não grava nada.
var _json_path := ""
var _pump_samples: Array[float] = []
var _phase_us: Dictionary = {}
## Estado do log de pedidos do HUD (espelho de `_log_orders`).
var _order_log: Array = []
var _last_logged_day := -1
## Cursor do log de decisões da IA (quantas já foram resumidas por dia).
var _decisions_seen := 0
## Consumidor fictício das métricas do HUD (evita que o medidor seja dobrado).
var _hud_sink := 0.0


func _ready() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--days="):
			_days = int(arg.split("=")[1])
		elif arg.begins_with("--map-mode="):
			_map_mode = String(arg.split("=")[1])
		elif arg.begins_with("--map-territory="):
			_map_territory = bool(int(arg.split("=")[1]))
		elif arg.begins_with("--tick-mode="):
			_tick_mode = String(arg.split("=")[1])
		elif arg.begins_with("--profile="):
			_profile = bool(int(arg.split("=")[1]))
		elif arg.begins_with("--threads="):
			_threads = int(arg.split("=")[1])
		elif arg.begins_with("--json="):
			# O caminho pode conter "=": corta só o prefixo.
			_json_path = arg.substr("--json=".length())

	# Congela o relógio: quem dirige os dias é este laço.
	Economy.set_speed(0)
	# O autoload agenda o 1º dia no `_ready` via `call_deferred` — que roda
	# DEPOIS deste `_ready`. Deixa esse dia nascer, junta-o e desliga o
	# `_process` do autoload: senão ele publica o próprio dia no meio da
	# medição (e chama `ai.plan`, que drena a fila que estamos medindo).
	await get_tree().process_frame
	Economy._ticker.cancel()
	Economy.set_process(false)
	var sim: Object = Economy.sim
	var ai: Object = Economy._ai
	if sim == null or ai == null:
		print("[perf] autoload Economy sem sim/IA")
		get_tree().quit(1)
		return
	if _threads != 0 and sim.has_method("set_sim_threads"):
		# S16: A/B do paralelismo (threads=1 serial vs threads=N). Default 0
		# deixa o núcleo no automático (min(núcleos-2, 8)).
		sim.set_sim_threads(_threads)

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
		var map_cities: Array = sim.get_status().get("cities", [])
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
	var poll_ms: Array[float] = []
	var enqueue_ms: Array[float] = []
	var ai_ms: Array[float] = []
	var hud_log_ms: Array[float] = []
	var ai_pump_ms: Array[float] = []
	var map_ms: Array[float] = []
	var layer_ms: Array[float] = []
	var territory_ms: Array[float] = []
	var turn_ms: Array[float] = []
	var pump_ms: Array[float] = []
	var pump_frames: Array[float] = []
	var peak_ms: Array[float] = []
	# Custo total do dia (tick + plano da IA + IA fatiada), uma amostra por dia.
	var day_total_ms: Array[float] = []
	var building_series: Array[float] = []
	var orders_total := 0
	var max_orders := 0
	var active_total := 0
	var delivered_total := 0

	var worker := _tick_mode == "worker"
	var scheduler = TickSchedulerScript.new() if worker else null
	if _profile:
		sim.set_profiling(true)

	for day in range(1, _days + 1):
		# O dia anterior terminou: com os pools, os comboios do teste não rodam
		# tween nenhum; devolve todos ao pool para liberar as vagas do dia.
		# O que sai aqui é o que chegou ao destino no dia anterior.
		var delivered := _release_convoys(convoys)
		delivered_total += delivered

		# 1. O dia: direto na main (sync) ou na thread do pool (worker), que é
		# o caminho de `Economy._process`/`tick_scheduler.gd`. No worker, o
		# `tick_ms` vira o tempo de PAREDE do dia; a main só paga o `poll()`.
		var tick_cost := 0.0
		var poll_cost := 0.0
		var summary: Dictionary = {}
		if worker:
			var w0 := Time.get_ticks_usec()
			scheduler.start(sim)
			while true:
				var p0 := Time.get_ticks_usec()
				summary = scheduler.poll()
				poll_cost = _ms(p0)
				if not summary.is_empty():
					break
				# Um frame de jogo enquanto o dia roda na thread: a main não
				# faz nada do núcleo (é o contrato do tick_scheduler).
				await get_tree().process_frame
			tick_cost = _ms(w0)
		else:
			var d0 := Time.get_ticks_usec()
			summary = sim.tick_day()
			tick_cost = _ms(d0)

		if _profile:
			var profile: Dictionary = sim.get_profile()
			for phase in PHASES:
				if not _phase_us.has(phase):
					_phase_us[phase] = [] as Array[float]
				(_phase_us[phase] as Array[float]).append(
					float(profile.get(phase, 0.0)) / 1000.0)

		# Frame da virada: TUDO o que a main thread faz depois de ter o dia.
		var frame_start := Time.get_ticks_usec()
		var d2 := Time.get_ticks_usec()
		var orders: Array = summary.get("trade_orders", [])
		orders_total += orders.size()
		max_orders = maxi(max_orders, orders.size())
		convoys.spawn_orders(orders)
		var d3 := Time.get_ticks_usec()
		ai.plan(day, summary)
		var d4 := Time.get_ticks_usec()
		var l0 := Time.get_ticks_usec()
		_hud_snapshot(summary)
		var hud_log_cost := _ms(l0)
		var map_cost := 0.0
		var layer_cost := 0.0
		var territory_cost := 0.0
		if mode_layer != null or territory != null:
			var cities: Array = summary.get("cities", [])
			if mode_layer != null:
				var ml0 := Time.get_ticks_usec()
				mode_layer.refresh(cities)
				layer_cost = _ms(ml0)
			if territory != null:
				var t0 := Time.get_ticks_usec()
				territory.update(cities, {})
				territory_cost = _ms(t0)
			map_cost = layer_cost + territory_cost
		var publish_cost := _ms(frame_start)

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
		# Tamanho do mundo no dia (fora das janelas cronometradas): o tick cresce
		# com o número de construções — o investimento da IA e dos grupos abre
		# ativos no meio do run.
		var buildings := _count_buildings(sim, summary)
		building_series.append(float(buildings))
		var agents_text := _agent_summary(ai)

		# A "virada" é o frame da main: no sync inclui o tick (histórico); no
		# worker inclui só o poll + publicação — o tick só soma no parede.
		var frame_cost := publish_cost + (tick_cost if not worker else poll_cost)
		tick_ms.append(tick_cost)
		poll_ms.append(poll_cost)
		enqueue_ms.append(float(d3 - d2) / 1000.0)
		ai_ms.append(float(d4 - d3) / 1000.0)
		hud_log_ms.append(hud_log_cost)
		ai_pump_ms.append(ai_pump_total)
		map_ms.append(map_cost)
		layer_ms.append(layer_cost)
		territory_ms.append(territory_cost)
		turn_ms.append(frame_cost)
		pump_ms.append(pump_total)
		pump_frames.append(float(frames))
		peak_ms.append(maxf(frame_cost, pump_peak))
		# Dia completo: tick (parede no worker) + planejamento da IA + a IA
		# fatiada gasta no pump. É a métrica-alvo do plano A0 (<= 15 ms p95).
		day_total_ms.append(tick_cost + ai_ms[-1] + ai_pump_total)

		print("[perf] dia %3d: tick %6.1f  fila %5.2f  plano IA %5.2f  mapa %5.2f  virada %6.1f | pump %6.2f ms em %3d frames (pico %5.2f, IA %5.2f)  [%d pedidos, %d comboios, %d entregues, %d construções]%s" % [
			day, tick_cost, enqueue_ms[-1], ai_ms[-1], map_cost, frame_cost,
			pump_total, frames, pump_peak, ai_pump_total,
			orders.size(), active, delivered, buildings, agents_text])
		if _profile:
			var profile: Dictionary = sim.get_profile()
			print("[perf]          fases: status %5.1f (cidades %5.1f, pedidos %4.1f, nações %4.1f)  trade %5.1f  arrival %5.1f  produção %4.1f  renda %4.1f  preços %4.1f" % [
				float(profile.get("status", 0.0)) / 1000.0,
				float(profile.get("status_cities", 0.0)) / 1000.0,
				float(profile.get("status_orders", 0.0)) / 1000.0,
				float(profile.get("status_nations", 0.0)) / 1000.0,
				float(profile.get("trade", 0.0)) / 1000.0,
				float(profile.get("arrival", 0.0)) / 1000.0,
				float(profile.get("production", 0.0)) / 1000.0,
				float(profile.get("income", 0.0)) / 1000.0,
				float(profile.get("prices", 0.0)) / 1000.0])

	print("\n[perf] virada do dia — %d dias, tick=%s, sob demanda (pump por frame)" % [
		_days, _tick_mode])
	print("[perf] grade de navegação/prontidão dos comboios: %.1f ms (uma vez)" % build_ms)
	if _map_mode != "none":
		print("[perf] mapa (%s, território=%s) montado em %.1f ms (uma vez)" % [
			_map_mode, str(_map_territory), map_build_ms])
	print("[perf] pedidos/dia: média %.1f, máximo %d; comboios ativos/dia (pós-pump): %.1f; entregues/dia: %.1f" % [
		float(orders_total) / float(_days), max_orders,
		float(active_total) / float(_days), float(delivered_total) / float(_days)])
	if not building_series.is_empty():
		var max_buildings := 0.0
		for value in building_series:
			max_buildings = maxf(max_buildings, value)
		print("[perf] construções no mundo: dia 1 %.0f → dia %d %.0f (máx %.0f) — o tick cresce com elas" % [
			building_series[0], _days, building_series[-1], max_buildings])
	print("[perf] %-32s %8s %8s %8s %8s" % ["etapa", "média", "p50", "p95", "máx"])
	_report("1. tick C++ (%s)" % ("parede do worker" if worker else "síncrono"), tick_ms)
	if _days > 2:
		# Os dois primeiros dias pagam a criação dos dois slots do status
		# (double buffer) do zero; em regime o custo é o do dia típico.
		var regime: Array[float] = []
		for index in range(2, tick_ms.size()):
			regime.append(tick_ms[index])
		_report("   tick em regime (dia > 2)", regime)
	if worker:
		_report("1b. poll da main (dia pronto)", poll_ms)
	_report("2. enfileirar pedidos", enqueue_ms)
	_report("3. IA planejar (agenda do dia)", ai_ms)
	_report("3b. log de pedidos (HUD)", hud_log_ms)
	_report("   IA trabalho fatiado/dia", ai_pump_ms)
	if _map_mode != "none":
		_report("4. mapa (camada+territ.)", map_ms)
		_report("   - camada do modo", layer_ms)
		_report("   - território", territory_ms)
	_report("   VIRADA (frame da main)", turn_ms)
	_report("   PUMP comboios/dia", pump_ms)
	_report("   PUMP por frame", _pump_samples)
	_report("   FRAME DE PICO (virada|frames)", peak_ms)
	_report("   DIA TOTAL (tick+plano+IA)", day_total_ms)
	if _profile:
		_report_phases()
	var frames_total := 0.0
	for value in pump_frames:
		frames_total += value
	print("[perf] pump: %.1f frames/dia em média; orçamento %d µs/frame" % [
		frames_total / float(_days), convoys.FRAME_BUDGET_US])
	# Memória do processo com o mundo REAL carregado (858 cidades): é o custo
	# estático do cenário, sim, IA e comboios — sem o mapa 3D.
	var live_status: Dictionary = Economy.sim.get_status()
	var static_mb := float(OS.get_static_memory_usage()) / (1024.0 * 1024.0)
	print("[perf] memória estática com %d cidades: %.1f MB" % [
		(live_status.get("cities", []) as Array).size(), static_mb])

	if not _json_path.is_empty():
		var metrics: Dictionary = {
			"tick_wall_ms": _stats(tick_ms),
			"poll_ms": _stats(poll_ms),
			"enqueue_orders_ms": _stats(enqueue_ms),
			"ai_plan_ms": _stats(ai_ms),
			"hud_log_ms": _stats(hud_log_ms),
			"ai_pump_ms": _stats(ai_pump_ms),
			"map_ms": _stats(map_ms),
			"turn_ms": _stats(turn_ms),
			"pump_ms": _stats(pump_ms),
			"frame_peak_ms": _stats(peak_ms),
			"day_total_ms": _stats(day_total_ms),
		}
		if not _write_json(metrics, static_mb):
			get_tree().quit(1)
			return

	get_tree().quit(0)


## Construções somadas no mundo (tamanho do mundo que o tick percorre). O status
## diário publica só o bloco leve: as construções vêm sob demanda por cidade.
func _count_buildings(sim: Object, summary: Dictionary) -> int:
	var total := 0
	for city: Dictionary in summary.get("cities", []):
		var heavy: Dictionary = sim.get_city_status(String(city.get("id", "")))
		total += (heavy.get("buildings", []) as Array).size()
	return total


## Decisões e construções do dia por agente (privado/governo/grupo). É o que
## explica o crescimento do mundo: o tick percorre mais construções no dia
## seguinte. Lê só a cauda nova do log de decisões, fora das janelas medidas.
func _agent_summary(ai) -> String:
	var decisions: Array = ai.decisions
	var counts := {"private": 0, "government": 0, "group": 0}
	var builds := {"private": 0, "government": 0, "group": 0}
	for index in range(_decisions_seen, decisions.size()):
		var entry: Variant = decisions[index]
		if typeof(entry) != TYPE_DICTIONARY:
			continue
		var agent := String((entry as Dictionary).get("agent", ""))
		if not counts.has(agent):
			continue
		counts[agent] = int(counts[agent]) + 1
		if String((entry as Dictionary).get("action", "")).begins_with("build"):
			builds[agent] = int(builds[agent]) + 1
	_decisions_seen = decisions.size()
	return "  IA: privado %d dec/%d obras, governo %d/%d, grupo %d/%d" % [
		counts["private"], builds["private"], counts["government"],
		builds["government"], counts["group"], builds["group"]]


## Um frame de IA (orçado). Devolve o custo em ms.
func _pump_ai(ai) -> float:
	var start := Time.get_ticks_usec()
	ai.pump(AI_BUDGET_US)
	return float(Time.get_ticks_usec() - start) / 1000.0


## Espelho do custo que o HUD paga por dia (`hud._on_status` com a janela
## fechada): duplica os pedidos para a janela de Comércio, soma as métricas da
## barra do topo sobre as 858 cidades e deriva os alertas.
func _hud_snapshot(summary: Dictionary) -> void:
	_log_orders(summary)
	var population := 0.0
	var stock := 0.0
	var deficit := 0.0
	var transit := 0.0
	for city in summary.get("cities", []):
		population += float(city["population"])
		stock += float(city["stockpile"])
		deficit += float(city["deficit"])
		transit += float(city["in_transit"])
	_hud_sink = population + stock + deficit + transit
	_hud_sink += float(AlertsScript.derive(summary).size())


## Espelho do custo que o HUD paga por dia (`hud._log_orders`): duplica cada
## pedido do dia para a janela de Comércio e poda o log em `MAX_ORDER_LOG`.
func _log_orders(summary: Dictionary) -> void:
	var day := int(summary.get("day", 0))
	if day > _last_logged_day:
		for order in summary.get("trade_orders", []):
			var entry: Dictionary = order.duplicate()
			entry["day"] = day
			_order_log.push_front(entry)
		_last_logged_day = day
		while _order_log.size() > MAX_ORDER_LOG:
			_order_log.pop_back()


## Perfil por fase do núcleo (média/p50/p95/máx em ms), ordenado por custo.
func _report_phases() -> void:
	print("[perf] %-24s %8s %8s %8s %8s" % ["fase do tick C++", "média", "p50", "p95", "máx"])
	var ranked: Array = []
	for phase in PHASES:
		var values: Array[float] = _phase_us.get(phase, [] as Array[float])
		if values.is_empty():
			continue
		var stats := _stats(values)
		ranked.append([phase, stats["mean"], stats["p50"], stats["p95"], stats["max"]])
	ranked.sort_custom(func(a, b): return a[1] > b[1])
	for row in ranked:
		print("[perf] %-24s %8.2f %8.2f %8.2f %8.2f" % [row[0], row[1], row[2], row[3], row[4]])


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
	var stats := _stats(values)
	print("[perf] %-26s %8.2f %8.2f %8.2f %8.2f  ms" % [
		label, stats["mean"], stats["p50"], stats["p95"], stats["max"]])


## Estatísticas de uma série em ms: média, p50, p95 (posto, nearest-rank),
## máximo e n. É a mesma conta da tabela impressa e do JSON.
func _stats(values: Array[float]) -> Dictionary:
	if values.is_empty():
		return {"mean": 0.0, "p50": 0.0, "p95": 0.0, "max": 0.0, "n": 0}
	var sorted := values.duplicate()
	sorted.sort()
	var count := sorted.size()
	var total := 0.0
	for value in values:
		total += value
	return {
		"mean": total / float(count),
		"p50": sorted[clampi(int(ceil(float(count) * 0.5)) - 1, 0, count - 1)],
		"p95": sorted[clampi(int(ceil(float(count) * 0.95)) - 1, 0, count - 1)],
		"max": sorted[count - 1],
		"n": count,
	}


## Grava o resultado em JSON UTF-8 (chaves ordenadas): `meta`, `metrics` por
## etapa e `phases` do tick C++ quando `--profile=1`. Floats crus (quem
## arredonda é a tabela impressa). Devolve false se não conseguiu gravar.
func _write_json(metrics: Dictionary, memory_static_mb: float) -> bool:
	var data: Dictionary = {
		"meta": {
			"days": _days,
			"tick_mode": _tick_mode,
			"profile": _profile,
			"threads_arg": _threads,
			"godot_version": String(Engine.get_version_info().get("string", "")),
			"generated_utc": "%sZ" % Time.get_datetime_string_from_system(true),
			"memory_static_mb": memory_static_mb,
		},
		"metrics": metrics,
	}
	if _profile:
		var phases: Dictionary = {}
		for phase in PHASES:
			var values: Array[float] = _phase_us.get(phase, [] as Array[float])
			if values.is_empty():
				continue
			phases[phase] = _stats(values)
		data["phases"] = phases

	var base_dir := _json_path.get_base_dir()
	if not base_dir.is_empty():
		# Caminho absoluto do SO passa direto; `res://`/`user://` viram nativo.
		DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(base_dir))
	var file := FileAccess.open(_json_path, FileAccess.WRITE)
	if file == null:
		print("[perf] falha ao gravar JSON em %s (erro %d)" % [
			_json_path, FileAccess.get_open_error()])
		return false
	file.store_string(JSON.stringify(data, "  ", true, false))
	file.close()
	print("[perf] JSON gravado: %s" % _json_path)
	return true
