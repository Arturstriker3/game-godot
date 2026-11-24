extends Node3D

const TmpPaths := preload("res://tools/tmp_paths.gd")

## Harness de escala da IA (economia): mede o custo do `plan()` e de cada
## `pump()` da IA escalonada, além do LOD de comboios, num mundo sintético.
##
## Complementa `tools/world_scale_bench`: ali o pico do pump é agregado por dia;
## aqui cada pump é registrado com o número de cidades que processou, o que
## separa pico de frame (pump) de custo da virada (plan/tick), e permite ver se
## o pior pump é o último do dia (que libera o status).
##
## Uso:
##   godot --headless --path . res://tools/ai_scale_bench.tscn -- \
##       --nations=190 --cities=650 --days=14 --ports=120 [--pretick=20]

const ConvoysScript: Script = preload("res://map/convoy_spacing.gd")
const EconomicAIScript: Script = preload("res://core/economy/ai/economic_ai.gd")
const GOODS_PATH := "res://data/definitions/economy/goods.json"
const AI_BUDGET_US := 1500

var _nations := 190
var _cities := 650
var _days := 14
var _ports := 120
var _pretick := 0


class StubGeo extends RefCounted:
	func load_data() -> bool:
		return true

	func city_land_capacity_t_day(_city_id: String, _resource: String) -> float:
		return 0.0


func _ready() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--nations="):
			_nations = int(arg.split("=")[1])
		elif arg.begins_with("--cities="):
			_cities = int(arg.split("=")[1])
		elif arg.begins_with("--days="):
			_days = int(arg.split("=")[1])
		elif arg.begins_with("--ports="):
			_ports = int(arg.split("=")[1])
		elif arg.begins_with("--pretick="):
			_pretick = int(arg.split("=")[1])
	_run.call_deferred()


func _run() -> void:
	var world := _gen_world(mini(_nations, _cities), _cities, _ports)
	var sim = EconomySim.new()
	sim.initialize_world(world["cities"], world["ports"], world["buildings"])
	sim.set_goods(_load_goods())

	var ai = EconomicAIScript.new()
	ai.interval_days = 7
	ai.setup(sim, StubGeo.new())
	var has_totals := "totals_builds" in ai.private_investor
	for _pre in _pretick:
		sim.tick_day()

	var tick_ms: Array[float] = []
	var plan_ms: Array[float] = []
	var prepare_ms: Array[float] = []
	var pump_ms: Array[float] = []
	var pump_cities: Array[int] = []
	var frames_per_day: Array[int] = []
	var day_totals: Array[float] = []
	var day_peaks: Array[float] = []
	var calls: Array = []
	for day in _days:
		var t0 := Time.get_ticks_usec()
		var summary: Dictionary = sim.tick_day()
		tick_ms.append(_ms(t0))
		var p0 := Time.get_ticks_usec()
		ai.plan(day, summary)
		plan_ms.append(_ms(p0))
		prepare_ms.append(float(ai.private_investor.last_prepare_ms))
		var frames := 0
		var day_total := 0.0
		var day_peak := 0.0
		while ai.is_busy():
			var before: int = int(ai._pending_pos)
			var size: int = int(ai._pending.size())
			var q0 := Time.get_ticks_usec()
			ai.pump(AI_BUDGET_US)
			var cost := _ms(q0)
			pump_ms.append(cost)
			day_total += cost
			day_peak = maxf(day_peak, cost)
			var after: int = int(ai._pending_pos)
			var processed := (size - before) if after <= before else (after - before)
			pump_cities.append(processed)
			calls.append({"day": day, "ms": cost, "cities": processed,
				"final": after <= before})
			frames += 1
			if frames > 5000:
				break
		day_totals.append(day_total)
		day_peaks.append(day_peak)
		frames_per_day.append(frames)

	var worst := 0
	for index in pump_ms.size():
		if pump_ms[index] > pump_ms[worst]:
			worst = index
	var worst_day := 0
	for index in day_totals.size():
		if day_totals[index] > day_totals[worst_day]:
			worst_day = index
	var non_final_max := 0.0
	var final_max := 0.0
	for call in calls:
		if bool(call["final"]):
			final_max = maxf(final_max, float(call["ms"]))
		else:
			non_final_max = maxf(non_final_max, float(call["ms"]))
	var convoys := _measure_convoys(sim, world["cities"].size())

	var result := {
		"world": {"nations": mini(_nations, _cities), "cities": world["cities"].size(),
			"ports": world["ports"].size(), "days": _days, "pretick": _pretick},
		"tick_ms": _stats(tick_ms),
		"plan_ms": _stats(plan_ms),
		"prepare_ms": _stats(prepare_ms),
		"pump_ms": _stats(pump_ms),
		"pump_cities": _stats_ints(pump_cities),
		"frames_per_day": _stats_ints(frames_per_day),
		"day_totals": _stats(day_totals),
		"day_peaks": _stats(day_peaks),
		"worst_pump": {"ms": pump_ms[worst], "cities": pump_cities[worst]},
		"worst_day": worst_day,
		"non_final_max_ms": non_final_max,
		"final_max_ms": final_max,
		"totals_builds": int(ai.private_investor.totals_builds),
		"convoys": convoys,
	}
	print("[ai-scale] mundo: %d cidades, %d nações, %d dias (pretick %d)" % [
		world["cities"].size(), result["world"]["nations"], _days, _pretick])
	print("[ai-scale] tick C++: média %.2f ms (máx %.2f)" % [
		result["tick_ms"]["mean"], result["tick_ms"]["max"]])
	print("[ai-scale] IA plan(): média %.2f ms, máx %.2f; totais medidos %d× (média %.2f ms, máx %.2f)" % [
		result["plan_ms"]["mean"], result["plan_ms"]["max"], result["totals_builds"],
		result["prepare_ms"]["mean"], result["prepare_ms"]["max"]])
	print("[ai-scale] IA pump(): média %.2f ms, p95 %.2f, máx %.2f; cidades/pump média %.1f, máx %d" % [
		result["pump_ms"]["mean"], result["pump_ms"]["p95"], result["pump_ms"]["max"],
		result["pump_cities"]["mean"], result["pump_cities"]["max"]])
	print("[ai-scale] pior pump: %.2f ms com %d cidades; não-finais máx %.2f, finais máx %.2f" % [
		result["worst_pump"]["ms"], result["worst_pump"]["cities"],
		non_final_max, final_max])
	print("[ai-scale] dia mais caro: #%d %.2f ms (média %.2f, máx do dia %.2f)" % [
		result["worst_day"], result["day_totals"]["max"], result["day_totals"]["mean"],
		result["day_peaks"]["max"]])
	print("[ai-scale] comboios: %d pedidos/dia, fila %.1f, ativos máx %d, agregados/dia %.1f, pump máx %.2f ms" % [
		result["convoys"]["orders_per_day"], result["convoys"]["queued"],
		result["convoys"]["active_max"], result["convoys"]["aggregated_per_day"],
		result["convoys"]["pump_max_ms"]])
	var path := TmpPaths.path("ai_scale_bench.json")
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file != null:
		file.store_string(JSON.stringify(result, "  "))
		file.close()
		print("[ai-scale] JSON: " + path)
	get_tree().quit(0)


func _measure_convoys(sim, city_count: int) -> Dictionary:
	var convoys = ConvoysScript.new()
	add_child(convoys)
	var orders_total := 0
	var queued := 0
	var active_max := 0
	var aggregated := 0
	var pump_max := 0.0
	var days := mini(5, _days)
	for _day in days:
		_release_convoys(convoys)
		var summary: Dictionary = sim.tick_day()
		var orders: Array = summary.get("trade_orders", [])
		orders_total += orders.size()
		convoys.spawn_orders(orders)
		queued += convoys._queue.size()
		var frames := 0
		while not convoys._queue.is_empty() or not convoys._prep.is_empty() \
				or convoys._nav.active:
			var t0 := Time.get_ticks_usec()
			convoys._pump(Time.get_ticks_usec() + convoys.FRAME_BUDGET_US)
			pump_max = maxf(pump_max, _ms(t0))
			frames += 1
			if frames > 5000:
				break
		active_max = maxi(active_max, convoys._active_convoys)
	aggregated = int(convoys.stats.get("aggregated", 0))
	return {
		"orders_per_day": int(round(float(orders_total) / float(maxi(days, 1)))),
		"queued": float(queued) / float(maxi(days, 1)),
		"active_max": active_max,
		"aggregated_per_day": float(aggregated) / float(maxi(days, 1)),
		"pump_max_ms": pump_max,
		"cities": city_count,
	}


func _release_convoys(convoys) -> void:
	for tween in convoys._tweens:
		if tween != null and tween.is_valid():
			tween.kill()
	convoys._tweens.clear()
	for convoy in convoys._convoys.duplicate():
		if is_instance_valid(convoy):
			convoys._finish_convoy(convoy)


func _gen_world(nations: int, city_target: int, port_target: int) -> Dictionary:
	var rng := RandomNumberGenerator.new()
	rng.seed = 20261111
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
	while delta != 0:
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
			cities.append({
				"id": city_id, "nation": tag, "home_nation": tag,
				"name": "Cidade %s-%02d" % [tag, local], "population": pop,
				"production": 0.0, "x": x, "y": y,
				"landmass": String(region["name"]), "capital": local == 0,
			})
			var factor := 0.55 + float((nation_index * 7 + local * 13) % 10) / 10.0
			buildings.append({
				"city_id": city_id, "id": city_id + "-FARM",
				"name": city_id + " farm", "resource": "wheat",
				"output_tons_per_day": pop * 0.0003 * factor,
			})
			if city_index % port_step == 0 and ports.size() < port_target:
				ports.append({
					"id": city_id + "-P", "name": city_id + " port", "nation": tag,
					"x": x + rng.randf_range(-4.0, 4.0),
					"y": y + rng.randf_range(-4.0, 4.0),
					"landmass": String(region["name"]),
				})
			city_index += 1
	return {"cities": cities, "ports": ports, "buildings": buildings}


func _load_goods() -> Array:
	var file := FileAccess.open(GOODS_PATH, FileAccess.READ)
	if file == null:
		return []
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data if typeof(data) == TYPE_ARRAY else []


func _stats(values: Array[float]) -> Dictionary:
	if values.is_empty():
		return {}
	var sorted := values.duplicate()
	sorted.sort()
	var total := 0.0
	for value in values:
		total += value
	var p95_index := clampi(int(ceil(float(sorted.size()) * 0.95)) - 1, 0, sorted.size() - 1)
	return {"n": values.size(), "mean": total / float(values.size()),
		"p95": sorted[p95_index], "max": sorted[sorted.size() - 1]}


func _stats_ints(values: Array[int]) -> Dictionary:
	var floats: Array[float] = []
	for value in values:
		floats.append(float(value))
	return _stats(floats)


func _ms(start_usec: int) -> float:
	return float(Time.get_ticks_usec() - start_usec) / 1000.0
