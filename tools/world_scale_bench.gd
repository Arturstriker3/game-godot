extends Node3D

const TmpPaths := preload("res://tools/tmp_paths.gd")

## Benchmark oficial de escala mundial (sintético, em memória).
##
## Gera ~190 nações e N cidades (padrão 650) com portos e uma fazenda por
## cidade, e mede o caminho real do jogo:
##   1. geração do mundo e carga no núcleo C++ (initialize_world);
##   2. tick_day por dia, com o tempo de cada fase do C++;
##   3. save/load (to_dict + JSON);
##   4. tick assíncrono (WorkerThreadPool) e determinismo contra o tick
##      síncrono — mesmo resultado depois de `--determinism` dias;
##   5. opcional: comboios (pedidos + A*) e mapa/rótulos.
##
## Uso (headless, como cena por causa de autoloads/viewport):
##   godot --headless --path . res://tools/world_scale_bench.tscn -- \
##       --cities=650 --nations=190 --ports=120 --tick-days=10 --warmup=2 \
##       --determinism=30 [--convoys=0] [--labels=0] \
##       [--tag=antes] [--out=<arquivo.json>]
##
## Os blocos de comboios/mapa ficam DESLIGADOS por padrão: eles dependem de
## subsistemas em evolução paralela e não entram na meta de tempo do tick.

const GOODS_PATH := "res://data/definitions/economy/goods.json"


## Fases do tick C++, na ordem em que o núcleo as reporta. As quatro últimas
## subdividem a fase `status` (cidades, pedidos, nações e o resto).
const PHASES := [
	"arrival", "reset", "labor", "production", "raw", "income", "trade",
	"accounting", "prices", "clamp", "fiscal", "invariants", "status",
	"status_cities", "status_orders", "status_nations", "status_misc",
]

var _nations := 190
var _cities := 650
var _ports := 120
var _tick_days := 10
var _warmup := 2
var _determinism := 30
var _convoys := false
var _labels := false
var _tag := ""
var _out := ""
var _results := {}


func _ready() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--nations="):
			_nations = int(arg.split("=")[1])
		elif arg.begins_with("--cities="):
			_cities = int(arg.split("=")[1])
		elif arg.begins_with("--ports="):
			_ports = int(arg.split("=")[1])
		elif arg.begins_with("--tick-days="):
			_tick_days = int(arg.split("=")[1])
		elif arg.begins_with("--warmup="):
			_warmup = int(arg.split("=")[1])
		elif arg.begins_with("--determinism="):
			_determinism = int(arg.split("=")[1])
		elif arg.begins_with("--convoys="):
			_convoys = arg.split("=")[1] != "0"
		elif arg.begins_with("--labels="):
			_labels = arg.split("=")[1] != "0"
		elif arg.begins_with("--tag="):
			_tag = String(arg.split("=")[1])
		elif arg.begins_with("--out="):
			_out = String(arg.split("=")[1])
	get_tree().root.size = Vector2i(1920, 1080)
	# Menos cidades que nações não faz sentido (cada nação tem capital).
	_nations = clampi(_nations, 1, maxi(_cities, 1))
	_run.call_deferred()


func _run() -> void:
	var mem_start := OS.get_static_memory_usage()
	var gen_start := Time.get_ticks_usec()
	var world := _gen_world(_nations, _cities, _ports)
	var gen_ms := _ms(gen_start)
	print("[scale] %s mundo sintético: %d nações, %d cidades, %d portos, %d fazendas" % [
		_label(), _nations, world["cities"].size(), world["ports"].size(),
		world["buildings"].size()])

	var sim = EconomySim.new()
	var load_start := Time.get_ticks_usec()
	sim.initialize_world(world["cities"], world["ports"], world["buildings"])
	sim.set_goods(_load_goods())
	var init_ms := _ms(load_start)
	var mem_loaded := OS.get_static_memory_usage()

	# 1. Tick C++ síncrono, com o perfil por fase do núcleo.
	sim.set_profiling(true)
	var tick_ms: Array[float] = []
	var phase_us: Dictionary = {}
	var orders_count: Array[int] = []
	var in_transit: Array[int] = []
	for day in _tick_days + _warmup:
		var t0 := Time.get_ticks_usec()
		var summary: Dictionary = sim.tick_day()
		var cost := _ms(t0)
		if day >= _warmup:
			tick_ms.append(cost)
			var profile: Dictionary = sim.get_profile()
			for phase in PHASES:
				if not phase_us.has(phase):
					phase_us[phase] = [] as Array[float]
				(phase_us[phase] as Array[float]).append(float(profile.get(phase, 0.0)) / 1000.0)
			orders_count.append((summary.get("trade_orders", []) as Array).size())
			var transit := 0.0
			for city in summary.get("cities", []):
				transit += float(city.get("in_transit", 0.0))
			in_transit.append(int(transit))
	sim.set_profiling(false)

	# 2. Determinismo: mesmo resultado do tick síncrono (mesmo mundo, outro sim).
	var determinism := {}
	if _determinism > 0:
		determinism = await _measure_determinism(world)

	# 3. Memória/save/load.
	var mem_before_save := OS.get_static_memory_usage()
	var s0 := Time.get_ticks_usec()
	var saved: Dictionary = sim.to_dict()
	var save_ms := _ms(s0)
	var j0 := Time.get_ticks_usec()
	var json := JSON.stringify(saved)
	var json_ms := _ms(j0)
	var sim2 = EconomySim.new()
	var l0 := Time.get_ticks_usec()
	sim2.from_dict(saved)
	var from_dict_ms := _ms(l0)
	var mem_after_save := OS.get_static_memory_usage()

	# 4. Opcional: comboios (pedidos/dia, teto de visíveis, A* fatiado).
	var convoy_block := {}
	if _convoys:
		convoy_block = _measure_convoys(sim, 10)

	# 6. Opcional: mapa/rótulos/LOD.
	var labels := {}
	if _labels:
		labels = await _measure_labels(world["cities"], world["ports"])

	var mem_end := OS.get_static_memory_usage()
	_results = {
		"tag": _tag,
		"world": {
			"nations": _nations,
			"cities": world["cities"].size(),
			"ports": world["ports"].size(),
			"buildings": world["buildings"].size(),
			"population": world["population"],
		},
		"timing": {
			"generate_ms": gen_ms,
			"initialize_demo_world_ms": init_ms,
			"tick_ms": _stats(tick_ms),
			"tick_phases_ms": _phase_stats(phase_us),
			"save_ms": save_ms,
			"json_ms": json_ms,
			"json_bytes": json.length(),
			"from_dict_ms": from_dict_ms,
		},
		"async": determinism,
		"convoys": convoy_block,
		"flows": {
			"orders_per_day": _stats_ints(orders_count),
			"in_transit_units": _stats_ints(in_transit),
		},
		"memory": {
			"start_bytes": mem_start,
			"loaded_bytes": mem_loaded,
			"before_save_bytes": mem_before_save,
			"after_save_bytes": mem_after_save,
			"end_bytes": mem_end,
			"delta_mb": float(mem_end - mem_start) / (1024.0 * 1024.0),
		},
		"labels": labels,
	}
	_print_report()
	_write_json()
	get_tree().quit(0 if _all_checks_ok() else 1)


## Duas simulações do mesmo mundo: uma avança N dias no tick síncrono, a outra
## pelo caminho assíncrono real do jogo (WorkerThreadPool, um dia por vez, com
## poll por frame). Ao fim, o estado tem de ser idêntico — inclusive o resumo.
func _measure_determinism(world: Dictionary) -> Dictionary:
	var sync_sim = EconomySim.new()
	sync_sim.initialize_world(world["cities"], world["ports"], world["buildings"])
	sync_sim.set_goods(_load_goods())
	var async_sim = EconomySim.new()
	async_sim.initialize_world(world["cities"], world["ports"], world["buildings"])
	async_sim.set_goods(_load_goods())

	var sync_start := Time.get_ticks_usec()
	sync_sim.set_profiling(true)
	var sync_days_ms: Array[float] = []
	var sync_profile: Dictionary = {}
	for _day in _determinism:
		var s0 := Time.get_ticks_usec()
		sync_sim.tick_day()
		sync_days_ms.append(_ms(s0))
		sync_profile = sync_sim.get_profile()
	var sync_total_ms := _ms(sync_start)
	sync_sim.set_profiling(false)
	print("[scale] %s determinismo síncrono: %s ms/dia | último dia %s" % [
		_label(), " ".join(sync_days_ms.map(func(v): return "%.1f" % v)),
		sync_profile])

	var scheduler = preload("res://core/economy/tick_scheduler.gd").new()
	var wall_start := Time.get_ticks_usec()
	var main_frame_ms: Array[float] = []
	var tick_start_ms: Array[float] = []
	for _day in _determinism:
		var f0 := Time.get_ticks_usec()
		scheduler.start(async_sim)
		tick_start_ms.append(_ms(f0))
		var frames := 0
		while true:
			var p0 := Time.get_ticks_usec()
			var summary: Dictionary = scheduler.poll()
			main_frame_ms.append(_ms(p0))
			frames += 1
			if not summary.is_empty():
				break
			await get_tree().process_frame
			if frames > 20000:
				break
	var wall_ms := _ms(wall_start)

	var sync_status: Dictionary = sync_sim.get_status()
	var async_status: Dictionary = async_sim.get_status()
	var sync_orders: Array = sync_status.get("trade_orders", [])
	var async_orders: Array = async_status.get("trade_orders", [])
	var a := _digest(sync_sim, sync_status, sync_orders)
	var b := _digest(async_sim, async_status, async_orders)
	return {
		"days": _determinism,
		"identical": a == b,
		"sync_digest": a,
		"async_digest": b,
		"sync_total_ms": sync_total_ms,
		"async_wall_ms": wall_ms,
		"async_start_ms": _stats(tick_start_ms),
		"async_main_frame_ms": _stats(main_frame_ms),
	}


func _measure_convoys(sim, days: int) -> Dictionary:
	var convoys = load("res://core/economy/trade_convoys.gd").new()
	var c0 := Time.get_ticks_usec()
	add_child(convoys)
	var build_ms := _ms(c0)
	var orders: Array[int] = []
	var queued: Array[int] = []
	var pump_ms: Array[float] = []
	var peak_ms: Array[float] = []
	var frames: Array[int] = []
	var active: Array[int] = []
	for _day in days:
		_release_convoys(convoys)
		var summary: Dictionary = sim.tick_day()
		var day_orders: Array = summary.get("trade_orders", [])
		orders.append(day_orders.size())
		var e0 := Time.get_ticks_usec()
		convoys.spawn_orders(day_orders)
		var enqueue_ms := _ms(e0)
		queued.append(convoys._queue.size())
		var count := 0
		var total := enqueue_ms
		var peak := enqueue_ms
		while true:
			if convoys._queue.is_empty() and convoys._prep.is_empty() \
					and not convoys._nav.active:
				break
			var q0 := Time.get_ticks_usec()
			convoys._pump(Time.get_ticks_usec() + convoys.FRAME_BUDGET_US)
			var cost := _ms(q0)
			total += cost
			peak = maxf(peak, cost)
			count += 1
			if count > 5000:
				break
		pump_ms.append(total)
		peak_ms.append(peak)
		frames.append(count)
		active.append(convoys._active_convoys)
	return {
		"build_ms": build_ms,
		"orders": _stats_ints(orders),
		"queued": _stats_ints(queued),
		"active": _stats_ints(active),
		"pump_ms": _stats(pump_ms),
		"peak_ms": _stats(peak_ms),
		"frames": _stats_ints(frames),
		"stats": convoys.stats,
	}


func _measure_labels(cities: Array, ports: Array) -> Dictionary:
	var world = load("res://map/city_world.gd").new()
	var b0 := Time.get_ticks_usec()
	add_child(world)
	world.build(cities, ports)
	var build_ms := _ms(b0)
	var nodes := _count_nodes(world)
	var cam := Camera3D.new()
	cam.fov = 75.0
	cam.near = 0.1
	cam.far = 4000.0
	add_child(cam)
	cam.current = true
	var focus: Vector3 = world.pixel_to_world(Vector2(2550.0, 610.0))
	var samples_near: Array[float] = []
	var samples_far: Array[float] = []
	cam.global_position = focus + Vector3(0.0, 32.0, 55.0)
	cam.look_at(focus, Vector3.UP)
	await get_tree().process_frame
	var near_report: Array = []
	for i in 30:
		var t0 := Time.get_ticks_usec()
		world._process(0.0)
		samples_near.append(_ms(t0))
		near_report = world.label_layout_report()
	cam.global_position = focus + Vector3(0.0, 180.0, 350.0)
	cam.look_at(focus, Vector3.UP)
	await get_tree().process_frame
	var far_report: Array = []
	for i in 30:
		var t0 := Time.get_ticks_usec()
		world._process(0.0)
		samples_far.append(_ms(t0))
		far_report = world.label_layout_report()
	return {
		"build_ms": build_ms,
		"nodes": nodes,
		"near_process_ms": _stats(samples_near),
		"far_process_ms": _stats(samples_far),
		"near": _label_stats(near_report),
		"far": _label_stats(far_report),
	}


func _gen_world(nations: int, city_target: int, port_target: int) -> Dictionary:
	var rng := RandomNumberGenerator.new()
	rng.seed = 20261111
	# Toda nação precisa de ao menos 1 cidade: pedir 40 cidades para 190 nações
	# deixa o ajuste de cotas abaixo sem solução (delta nunca chega a zero e o
	# laço gira para sempre — foi o que travou o bench por 30 min a 100% CPU).
	nations = clampi(nations, 1, maxi(city_target, 1))
	var regions := [
		{"name": "europe", "x0": 2350.0, "x1": 2820.0, "y0": 430.0, "y1": 720.0},
		{"name": "north_america", "x0": 900.0, "x1": 1800.0, "y0": 500.0, "y1": 900.0},
		{"name": "south_america", "x0": 1400.0, "x1": 1920.0, "y0": 950.0, "y1": 1420.0},
		{"name": "africa", "x0": 2450.0, "x1": 3120.0, "y0": 850.0, "y1": 1420.0},
		{"name": "middle_east", "x0": 2850.0, "x1": 3220.0, "y0": 770.0, "y1": 1010.0},
		{"name": "asia", "x0": 3000.0, "x1": 4220.0, "y0": 500.0, "y1": 1120.0},
		{"name": "oceania", "x0": 4200.0, "x1": 4820.0, "y0": 1140.0, "y1": 1520.0},
	]
	var quotas: Array[int] = []
	for i in nations:
		if i < 24:
			quotas.append(10)
		elif i < 70:
			quotas.append(5)
		elif i < 130:
			quotas.append(2)
		else:
			quotas.append(1)
	var total := 0
	for q in quotas:
		total += q
	var delta := city_target - total
	var cursor := 0
	# Guarda de segurança: se todas as cotas já estão no piso (1), o delta
	# negativo não tem mais como ser absorvido — sair em vez de girar.
	var steps := 0
	while delta != 0:
		steps += 1
		if steps > nations * 16:
			break
		var idx := cursor % nations
		if delta > 0:
			quotas[idx] += 1
			delta -= 1
		elif quotas[idx] > 1:
			quotas[idx] -= 1
			delta += 1
		cursor += 1

	var cities: Array = []
	var ports: Array = []
	var buildings: Array = []
	var population := 0.0
	var city_index := 0
	var port_step := maxi(1, int(round(float(city_target) / float(maxi(port_target, 1)))))
	for nation_index in nations:
		var tag := "N%03d" % nation_index
		var region: Dictionary = regions[nation_index % regions.size()]
		var scale := 0.15e6
		if nation_index < 24:
			scale = 4.0e6
		elif nation_index < 70:
			scale = 1.2e6
		elif nation_index < 130:
			scale = 0.5e6
		for local in quotas[nation_index]:
			var city_id := "%s-C%02d" % [tag, local]
			var x := float(region["x0"]) + rng.randf() * (float(region["x1"]) - float(region["x0"]))
			var y := float(region["y0"]) + rng.randf() * (float(region["y1"]) - float(region["y0"]))
			var pop := scale * (0.6 + rng.randf() * 1.2)
			if local == 0:
				pop *= 1.5
			pop = clampf(pop, 30000.0, 20000000.0)
			population += pop
			cities.append({
				"id": city_id,
				"nation": tag,
				"home_nation": tag,
				"name": "Cidade %s-%02d" % [tag, local],
				"population": pop,
				"production": 0.0,
				"x": x,
				"y": y,
				"landmass": String(region["name"]),
				"capital": local == 0,
			})
			var factor := 0.55 + float((nation_index * 7 + local * 13) % 10) / 10.0
			buildings.append({
				"city_id": city_id,
				"id": city_id + "-FARM",
				"name": city_id + " farm",
				"resource": "wheat",
				"output_tons_per_day": pop * 0.0003 * factor,
			})
			if city_index % port_step == 0 and ports.size() < port_target:
				ports.append({
					"id": city_id + "-P",
					"name": city_id + " port",
					"nation": tag,
					"x": x + rng.randf_range(-4.0, 4.0),
					"y": y + rng.randf_range(-4.0, 4.0),
					"landmass": String(region["name"]),
				})
			city_index += 1
	return {
		"cities": cities,
		"ports": ports,
		"buildings": buildings,
		"population": population,
	}


func _load_goods() -> Array:
	var file := FileAccess.open(GOODS_PATH, FileAccess.READ)
	if file == null:
		return []
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data if typeof(data) == TYPE_ARRAY else []


func _release_convoys(convoys) -> void:
	for tween in convoys._tweens:
		if tween != null and tween.is_valid():
			tween.kill()
	convoys._tweens.clear()
	for convoy in convoys._convoys.duplicate():
		if is_instance_valid(convoy):
			convoys._finish_convoy(convoy)


func _count_nodes(node: Node) -> int:
	var total := 1
	for child in node.get_children():
		total += _count_nodes(child)
	return total


func _label_stats(report: Array) -> Dictionary:
	var city_labels := 0
	var city_text := 0
	var capital_text := 0
	var port_icons := 0
	var texts: Array[Rect2] = []
	var overlaps := 0
	for entry in report:
		if String(entry.get("kind", "")) == "city":
			city_labels += 1
			if bool(entry.get("text_visible", false)):
				city_text += 1
				if bool(entry.get("capital", false)):
					capital_text += 1
				var rect: Rect2 = entry.get("text_rect", Rect2())
				for other in texts:
					if rect.intersects(other):
						overlaps += 1
				texts.append(rect)
		elif bool(entry.get("marker_visible", false)):
			port_icons += 1
	return {
		"city_entries": city_labels,
		"city_names_visible": city_text,
		"capital_names_visible": capital_text,
		"port_icons_visible": port_icons,
		"name_overlaps": overlaps,
	}


## Hash do mundo: o status diário publica só o bloco leve, então estoques e
## preços (o que fecha a equivalência sync×async) vêm de get_city_status.
func _digest(sim: Object, status: Dictionary, orders: Array) -> String:
	# Resumo compacto e estável: dia, caixas, estoques e pedidos do dia.
	var parts: Array[String] = [str(status.get("day", 0))]
	parts.append("%.6f" % float(status.get("money_total", 0.0)))
	parts.append("%.6f" % float(status.get("gdp_total", 0.0)))
	parts.append(str(status.get("bankruptcies", 0)))
	var cities: Array = status.get("cities", [])
	for city in cities:
		var heavy: Dictionary = sim.get_city_status(String(city.get("id", "")))
		parts.append("%.6f" % float(city.get("population", 0.0)))
		parts.append("%.6f" % float(city.get("firm_cash", 0.0)))
		parts.append("%.6f" % float(city.get("government_budget", 0.0)))
		for market in heavy.get("markets", []):
			parts.append("%.6f" % float(market.get("stock", 0.0)))
			parts.append("%.6f" % float(market.get("price", 0.0)))
	for order in orders:
		parts.append("%s>%s:%.6f" % [
			String(order.get("from_city", "")), String(order.get("to_city", "")),
			float(order.get("amount", 0.0))])
	return "\n".join(parts).sha256_text()


func _stats(values: Array[float]) -> Dictionary:
	if values.is_empty():
		return {}
	var sorted := values.duplicate()
	sorted.sort()
	var total := 0.0
	for value in values:
		total += value
	var p95_index: int = clampi(int(ceil(float(sorted.size()) * 0.95)) - 1, 0, sorted.size() - 1)
	return {
		"n": values.size(),
		"mean": total / float(values.size()),
		"p95": sorted[p95_index],
		"max": sorted[sorted.size() - 1],
	}


func _stats_ints(values: Array[int]) -> Dictionary:
	if values.is_empty():
		return {}
	var floats: Array[float] = []
	for value in values:
		floats.append(float(value))
	return _stats(floats)


func _phase_stats(phases: Dictionary) -> Dictionary:
	var result := {}
	var total := 0.0
	for phase in PHASES:
		var values: Array[float] = phases.get(phase, [] as Array[float])
		var stats := _stats(values)
		if not stats.is_empty():
			total += float(stats["mean"])
		result[phase] = stats
	result["sum"] = total
	return result


func _ms(start_usec: int) -> float:
	return float(Time.get_ticks_usec() - start_usec) / 1000.0


func _label() -> String:
	return "[%s]" % _tag if not _tag.is_empty() else ""


func _all_checks_ok() -> bool:
	var async_block: Dictionary = _results.get("async", {})
	if async_block.is_empty():
		return true
	return bool(async_block.get("identical", false))


func _print_report() -> void:
	var t: Dictionary = _results["timing"]
	print("[scale] %s tick C++ (%d cidades): média %.2f ms, p95 %.2f, máx %.2f" % [
		_label(), _results["world"]["cities"], t["tick_ms"]["mean"],
		t["tick_ms"]["p95"], t["tick_ms"]["max"]])
	var phases: Dictionary = t["tick_phases_ms"]
	var ranked: Array = []
	for phase in PHASES:
		if phases.get(phase, {}).is_empty():
			continue
		ranked.append([phase, float(phases[phase]["mean"])])
	ranked.sort_custom(func(a, b): return a[1] > b[1])
	var parts: Array[String] = []
	for i in min(5, ranked.size()):
		parts.append("%s %.2f" % [ranked[i][0], ranked[i][1]])
	print("[scale] %s fases (média ms): %s" % [_label(), ", ".join(parts)])
	print("[scale] %s save %.1f ms, JSON %.1f ms (%.1f MB), load %.1f ms" % [
		_label(), t["save_ms"], t["json_ms"],
		float(t["json_bytes"]) / (1024.0 * 1024.0), t["from_dict_ms"]])
	var a: Dictionary = _results.get("async", {})
	if not a.is_empty():
		print("[scale] %s async: %d dias, idêntico=%s, trabalho %.1f ms/dia, main-frame %.2f ms (máx %.2f), parede %.1f ms" % [
			_label(), a["days"], str(a["identical"]),
			float(a["sync_total_ms"]) / float(a["days"]),
			a["async_main_frame_ms"]["mean"], a["async_main_frame_ms"]["max"],
			a["async_wall_ms"]])
	var c: Dictionary = _results.get("convoys", {})
	if not c.is_empty():
		print("[scale] %s comboios: %.1f pedidos/dia, fila %.1f, ativos %.1f, pump %.1f ms/dia em %.1f frames (pico %.2f)" % [
			_label(), c["orders"]["mean"], c["queued"]["mean"], c["active"]["mean"],
			c["pump_ms"]["mean"], c["frames"]["mean"], c["peak_ms"]["max"]])
	print("[scale] %s memória: %.1f MB (delta)" % [
		_label(), _results["memory"]["delta_mb"]])
	if _labels:
		var l: Dictionary = _results["labels"]
		print("[scale] %s mapa: build %.0f ms, %d nós, _process perto %.2f / longe %.2f; nomes %d, portos %d, sobreposições %d" % [
			_label(), l["build_ms"], l["nodes"], l["near_process_ms"]["mean"],
			l["far_process_ms"]["mean"], l["near"]["city_names_visible"],
			l["near"]["port_icons_visible"], l["near"]["name_overlaps"]])


func _write_json() -> void:
	var path := _out
	if path.is_empty():
		path = TmpPaths.path("world_scale_bench.json")
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		return
	file.store_string(JSON.stringify(_results, "  "))
	file.close()
	print("[scale] %s JSON: %s" % [_label(), path])
