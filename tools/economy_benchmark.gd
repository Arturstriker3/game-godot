extends SceneTree

## Benchmark da economia sintética em 10, 100, 1.000 e 5.000 cidades.
## Mede tick médio/p95, memória e custo de cada fase. Também verifica
## determinismo reexecutando o mesmo cenário e comparando o checksum.

const COUNTS := [10, 100, 1000, 5000]
const BUILDINGS_PER_CITY := 4
const DAYS := 30
const WARMUP_DAYS := 5
const SEED := 12345

var _failures: Array[String] = []


func _init() -> void:
	var results: Array = []
	for count in COUNTS:
		results.append(_run_count(count))

	var deterministic := _check_determinism()
	_write_results(results, deterministic)
	_print_table(results)

	if _failures.is_empty():
		print("[benchmark] OK: cenários concluídos e determinismo confirmado.")
		quit(0)
	else:
		print("[benchmark] FALHAS (%d):" % _failures.size())
		for failure in _failures:
			print(" - " + failure)
		quit(1)


func _run_count(count: int) -> Dictionary:
	var sim = EconomySim.new()
	sim.initialize_benchmark(count, BUILDINGS_PER_CITY, SEED)
	var godot_memory_before := OS.get_static_memory_usage()
	var result: Dictionary = sim.run_benchmark(DAYS, WARMUP_DAYS)
	var godot_memory_after := OS.get_static_memory_usage()
	result["godot_memory_delta"] = godot_memory_after - godot_memory_before
	result["measured_city_count"] = sim.get_benchmark_city_count()
	if int(result["city_count"]) != count:
		_fail("cidades: esperado %d, obtido %d" % [count, result["city_count"]])
	if float(result["avg_tick_us"]) <= 0.0:
		_fail("%d cidades: tick médio inválido" % count)
	if float(result["p95_tick_us"]) < float(result["avg_tick_us"]):
		_fail("%d cidades: p95 menor que a média" % count)
	return result


func _check_determinism() -> Dictionary:
	var first = EconomySim.new()
	first.initialize_benchmark(100, BUILDINGS_PER_CITY, 7)
	var a: Dictionary = first.run_benchmark(15, 3)

	var second = EconomySim.new()
	second.initialize_benchmark(100, BUILDINGS_PER_CITY, 7)
	var b: Dictionary = second.run_benchmark(15, 3)

	if absf(float(a["checksum"]) - float(b["checksum"])) > 1e-6:
		_fail("determinismo: checksum divergente (%.6f != %.6f)" % [
			float(a["checksum"]), float(b["checksum"])])
	return {
		"cities": 100,
		"days": 15,
		"checksum_first": float(a["checksum"]),
		"checksum_second": float(b["checksum"]),
		"equal": absf(float(a["checksum"]) - float(b["checksum"])) <= 1e-6,
	}


func _print_table(results: Array) -> void:
	print("[benchmark] build de depuração; mediana de %d dias após %d de aquecimento" % [
		DAYS, WARMUP_DAYS])
	print("cidades constr links | avg_ms p95_ms max_ms | mem_MB | mercado pop prod comércio contab preços invest (us)")
	for result in results:
		print("%6d %6d %5d | %6.3f %6.3f %6.3f | %6.2f | %7.1f %7.1f %7.1f %8.1f %7.1f %6.1f %6.1f" % [
			int(result["city_count"]), int(result["building_count"]),
			int(result["link_count"]), float(result["avg_tick_ms"]),
			float(result["p95_tick_ms"]), float(result["max_tick_ms"]),
			float(result["memory_bytes"]) / (1024.0 * 1024.0),
			float(result["market_us"]), float(result["pop_us"]),
			float(result["production_us"]), float(result["trade_us"]),
			float(result["accounting_us"]), float(result["prices_us"]),
			float(result["investment_us"]),
		])


func _write_results(results: Array, deterministic: Dictionary) -> void:
	var directory := "res://docs/economy_benchmark"
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(directory))
	var payload := {
		"godot_version": Engine.get_version_info(),
		"build": "debug",
		"days": DAYS,
		"warmup_days": WARMUP_DAYS,
		"buildings_per_city": BUILDINGS_PER_CITY,
		"seed": SEED,
		"results": results,
		"determinism": deterministic,
	}
	var file := FileAccess.open("%s/results.json" % directory, FileAccess.WRITE)
	if file == null:
		_fail("não foi possível escrever results.json")
		return
	file.store_string(JSON.stringify(payload, "  "))
	file.close()


func _fail(message: String) -> void:
	_failures.append(message)
