extends SceneTree

## Perfil do tick C++ no cenário REAL do jogo (858 cidades), sem mapa.
##
## Usa o autoload `Economy` — mesmo cenário, geografia e núcleo do jogo — e roda
## o dia de forma SÍNCRONA na main thread com `demo_set_profiling`, para separar
## o custo de cada fase do núcleo. Serve para comparar com o benchmark sintético
## (`world_scale_bench --cities=650`, ~27 ms/tick) sem carregar o mapa 3D.
##
## O autoload é acessado pelo nó (e não pelo nome global: em `--script` o nome
## do autoload não existe como constante de compilação). O relógio do jogo é
## congelado e o dia agendado no `_ready` do autoload é juntado antes de medir:
## existe um único escritor sobre o núcleo.
##
## Uso: godot --headless --path . --script res://tools/real_tick_profile.gd [--days=5]

const PHASES := [
	"arrival", "reset", "labor", "production", "raw", "income", "trade",
	"accounting", "prices", "clamp", "fiscal", "invariants", "status",
	"status_cities", "status_orders", "status_nations", "status_misc",
	"trade_food", "trade_food_buyers", "trade_goods", "trade_goods_loop",
]

var _days := 5
var _frames := 0
var _values: Dictionary = {}
var _done := false


func _process(_delta: float) -> bool:
	if _done:
		return true
	_frames += 1
	var economy := root.get_node_or_null("Economy")
	if economy == null:
		# Autoloads entram na árvore depois do primeiro frame deste MainLoop.
		if _frames > 120:
			print("[real_tick] autoload Economy não encontrado")
			quit(1)
			return true
		return false
	# Espera o `_start_day.call_deferred()` do autoload assentar e o junta:
	# depois disso o relógio fica parado e só este laço escreve no núcleo.
	economy.set_speed(0)
	economy._ticker.cancel()
	if _frames < 3:
		return false
	_done = true
	_run(economy.sim)
	return true


func _run(sim: Object) -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--days="):
			_days = int(arg.split("=")[1])

	var status: Dictionary = sim.get_demo_status()
	var cities: Array = status.get("cities", [])
	print("[real_tick] autoload Economy carregado em %d ms (cenário + geografia + pré-aquecimento do status)" % [
		Time.get_ticks_msec()])
	# Tamanho do que a fase `status` reescreve: construções e jazidas por cidade.
	var buildings := 0
	var deposits := 0
	var markets := 0
	for city in cities:
		buildings += (city.get("buildings", []) as Array).size()
		deposits += (city.get("deposits", {}) as Dictionary).size()
		markets += (city.get("markets", []) as Array).size()
	print("[real_tick] cenário real: %d cidades (%d construções, %d jazidas, %d mercados), %d pedidos, dia %s" % [
		cities.size(), buildings, deposits, markets,
		(status.get("trade_orders", []) as Array).size(), str(status.get("day", 0))])
	sim.demo_set_profiling(true)

	var tick_ms: Array[float] = []
	for day in range(_days):
		var start := Time.get_ticks_usec()
		sim.tick_demo()
		tick_ms.append(float(Time.get_ticks_usec() - start) / 1000.0)
		var profile: Dictionary = sim.demo_get_profile()
		for phase in PHASES:
			if not _values.has(phase):
				_values[phase] = [] as Array[float]
			(_values[phase] as Array[float]).append(
				float(profile.get(phase, 0.0)) / 1000.0)
		print("[real_tick] dia %d: %.1f ms (tick completo)" % [day + 1, tick_ms[-1]])

	sim.demo_set_profiling(false)
	var total := 0.0
	for value in tick_ms:
		total += value
	var sorted := tick_ms.duplicate()
	sorted.sort()
	print("[real_tick] tick completo: média %.1f ms, p95 %.1f ms, máx %.1f ms (%d dias)" % [
		total / float(tick_ms.size()),
		sorted[clampi(int(ceil(float(sorted.size()) * 0.95)) - 1, 0, sorted.size() - 1)],
		sorted[sorted.size() - 1], tick_ms.size()])

	print("[real_tick] %-20s %8s %8s %8s  %s" % ["fase", "média", "p95", "máx", "share"])
	var ranked: Array = []
	var phase_total := 0.0
	for phase in PHASES:
		var values: Array[float] = _values.get(phase, [] as Array[float])
		if values.is_empty():
			continue
		var sum := 0.0
		var vsorted := values.duplicate()
		vsorted.sort()
		for value in values:
			sum += value
		phase_total += sum / float(values.size())
		ranked.append([phase, sum / float(values.size()),
			vsorted[clampi(int(ceil(float(vsorted.size()) * 0.95)) - 1, 0, vsorted.size() - 1)],
			vsorted[vsorted.size() - 1]])
	ranked.sort_custom(func(a, b): return a[1] > b[1])
	for row in ranked:
		print("[real_tick] %-20s %8.2f %8.2f %8.2f  %5.1f%%" % [
			row[0], row[1], row[2], row[3], 100.0 * row[1] / maxf(phase_total, 0.001)])
	print("[real_tick] soma das fases: %.1f ms/dia" % phase_total)
	quit(0)
