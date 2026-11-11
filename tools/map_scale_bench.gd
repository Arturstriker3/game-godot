extends Node3D

## Benchmark de escala do MAPA (CityWorld) com dados sintéticos em memória.
##
## Gera N cidades e M portos espalhados pela folha (sem tocar nas definições do
## cenário) e mede o caminho real do mapa: `build()`, contagem de nós, memória,
## `_process` no regime (perto/longe/pan, depois de a fila de LOD esvaziar) e o
## custo/pico de construir a primeira cidade detalhada. O build acontece uma vez,
## não é frame de jogo, então é medido e reportado à parte.
##
## Uso (headless):
##   godot --headless --path . res://tools/map_scale_bench.tscn -- \
##       --cities=650 --ports=650 --tag=before --out=res://docs/map_visual/map_scale_before.json
##
## Metas (mundo de 650 cidades/650 portos): build <= 1,5 s, memória <= +250 MB,
## `_process` <= 2 ms (perto, longe e em pan) e nenhum frame de `_process` > 16 ms.
## O script imprime o veredito e sai com código 0 (o benchmark documenta; quem
## compara é o relatório).

const CityWorldScript: Script = preload("res://map/city_world.gd")

const TARGET_BUILD_MS := 1500.0
const TARGET_MEMORY_MB := 250.0
const TARGET_PROCESS_MS := 2.0
const TARGET_FRAME_MS := 16.0

const PITCH_DEG := -56.0
const CAMERA_FOV := 75.0
const NEAR_DISTANCE := 52.0
const FAR_DISTANCE := 420.0
const FRAMES := 120

var _city_count := 650
var _port_count := 650
var _tag := "run"
var _out := ""
var _world: Node3D
var _pivot: Node3D
var _camera: Camera3D
var _rng := RandomNumberGenerator.new()


func _ready() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--cities="):
			_city_count = maxi(1, int(arg.split("=")[1]))
		elif arg.begins_with("--ports="):
			_port_count = maxi(0, int(arg.split("=")[1]))
		elif arg.begins_with("--tag="):
			_tag = arg.split("=")[1]
		elif arg.begins_with("--out="):
			_out = arg.split("=")[1]
	get_tree().root.size = Vector2i(1920, 1080)
	_run.call_deferred()


func _run() -> void:
	await get_tree().process_frame
	var world := _gen_world()
	print("[map_scale] %d cidades, %d portos (sintéticos, %s)" % [
		world["cities"].size(), world["ports"].size(), _tag])

	var mem_before := _mem_mb()
	var build_start := Time.get_ticks_usec()
	_world = CityWorldScript.new()
	add_child(_world)
	_world.build(world["cities"], world["ports"])
	var build_ms := _elapsed_ms(build_start)
	var mem_after := _mem_mb()
	var nodes := _count_nodes(_world)
	print("[map_scale] build %.1f ms | nós %d | memória +%.1f MB" % [
		build_ms, nodes, mem_after - mem_before])

	_build_camera()
	# A câmera começa em cima de uma cidade grande (a de maior população).
	var focus: Array = world["cities"].duplicate()
	focus.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return float(a["population"]) > float(b["population"]))
	_aim_at(Vector2(float(focus[0]["x"]), float(focus[0]["y"])), NEAR_DISTANCE)
	await get_tree().process_frame

	# `_process` do mapa é chamado manualmente para medir o custo sem o laço da
	# engine; o nó fica com o processamento automático desligado.
	_world.set_process(false)
	var first_ms := _time_process()
	# Esvazia as filas de modelo (a primeira aproximação) antes de medir o
	# regime: o custo de construir cada cidade/porto detalhado é reportado à
	# parte (`lod_drain_*`), como o pico que ele realmente é.
	var drain_start := Time.get_ticks_usec()
	var drain_frames := 0
	while (not _world._urban_queue.is_empty() \
			or not _world._port_model_queue.is_empty()) and drain_frames < 1200:
		_world._process(1.0 / 60.0)
		drain_frames += 1
	var drain_ms := _elapsed_ms(drain_start)
	var near := _measure_process(FRAMES)
	_aim_at(Vector2(float(focus[0]["x"]), float(focus[0]["y"])), FAR_DISTANCE)
	await get_tree().process_frame
	var far := _measure_process(FRAMES)
	# Câmera em movimento (pan contínuo): o layout de tela é refeito no caminho,
	# então este é o pior caso real do `_process` — o número que a meta mede.
	_aim_at(Vector2(float(focus[0]["x"]), float(focus[0]["y"])), NEAR_DISTANCE)
	await get_tree().process_frame
	var pan := _measure_pan(FRAMES)

	var peak := maxf(first_ms, maxf(near["max"], maxf(far["max"], pan["max"])))
	var result := {
		"tag": _tag,
		"cities": world["cities"].size(),
		"ports": world["ports"].size(),
		"build_ms": build_ms,
		"nodes": nodes,
		"memory_mb": mem_after - mem_before,
		"lod_drain_ms": drain_ms,
		"lod_drain_frames": drain_frames,
		"process_near_avg_ms": near["avg"],
		"process_near_max_ms": near["max"],
		"process_far_avg_ms": far["avg"],
		"process_far_max_ms": far["max"],
		"process_pan_avg_ms": pan["avg"],
		"process_pan_max_ms": pan["max"],
		"first_lod_ms": first_ms,
		"peak_frame_ms": peak,
		"engine": Engine.get_version_info()["string"],
		"targets": {
			"build_ms": TARGET_BUILD_MS,
			"memory_mb": TARGET_MEMORY_MB,
			"process_ms": TARGET_PROCESS_MS,
			"frame_ms": TARGET_FRAME_MS,
		},
	}
	var checks := {
		"build": build_ms <= TARGET_BUILD_MS,
		"memory": (mem_after - mem_before) <= TARGET_MEMORY_MB,
		"process": maxf(maxf(near["avg"], far["avg"]), pan["avg"]) \
				<= TARGET_PROCESS_MS,
		"frame": peak <= TARGET_FRAME_MS,
	}
	result["checks"] = checks
	result["pass"] = not checks.values().has(false)
	print("[map_scale] %s | _process perto %.2f ms (máx %.2f) | longe %.2f ms (máx %.2f) | pan %.2f ms (máx %.2f)" % [
		"PASS" if bool(result["pass"]) else "FAIL",
		near["avg"], near["max"], far["avg"], far["max"], pan["avg"], pan["max"]])
	print("[map_scale] fila de LOD: %d frames / %.1f ms (construção detalhada)" % [
		drain_frames, drain_ms])
	print("[map_scale] primeiro frame de LOD %.1f ms | pico de frame %.1f ms" % [
		first_ms, peak])
	if not _out.is_empty():
		_write_json(_out, result)
		print("[map_scale] relatório: " + _out)
	get_tree().quit(0)


## Mundo sintético determinístico: cidades espalhadas pela folha, portos perto
## do litoral (offset curto da cidade) e população em cauda longa.
func _gen_world() -> Dictionary:
	_rng.seed = 20261111
	var cities: Array = []
	var ports: Array = []
	for index in _city_count:
		var px := Vector2(
			_rng.randf_range(180.0, 4820.0),
			_rng.randf_range(420.0, 2080.0))
		var population := _population_for(index)
		cities.append({
			"id": "SYN-C%04d" % index,
			"nation": "SYN%03d" % (index % 190),
			"home_nation": "SYN%03d" % (index % 190),
			"name": "Cidade %d" % index,
			"population": population,
			"x": px.x,
			"y": px.y,
			"landmass": "synthetic",
			"capital": index % 65 == 0,
		})
	for index in _port_count:
		var host: Dictionary = cities[index % cities.size()]
		var angle := _rng.randf_range(0.0, TAU)
		var px := Vector2(float(host["x"]), float(host["y"])) \
				+ Vector2(cos(angle), sin(angle)) * _rng.randf_range(8.0, 60.0)
		ports.append({
			"id": "SYN-P%04d" % index,
			"name": "Porto %d" % index,
			"nation": String(host["nation"]),
			"x": clampf(px.x, 40.0, 4960.0),
			"y": clampf(px.y, 60.0, 2440.0),
			"landmass": "synthetic",
		})
	return {"cities": cities, "ports": ports}


## Cauda longa: ~5% acima de 1 M, ~20% entre 500 mil e 1 M, o resto pequeno.
func _population_for(index: int) -> float:
	if index < 32:
		return _rng.randf_range(1_000_000.0, 12_000_000.0)
	if index < 160:
		return _rng.randf_range(500_000.0, 1_000_000.0)
	return _rng.randf_range(40_000.0, 499_000.0)


func _build_camera() -> void:
	_pivot = Node3D.new()
	_pivot.rotation_degrees.x = PITCH_DEG
	add_child(_pivot)
	_camera = Camera3D.new()
	_camera.fov = CAMERA_FOV
	_camera.near = 0.1
	_camera.far = 4000.0
	_pivot.add_child(_camera)
	_camera.current = true


func _aim_at(px: Vector2, distance: float) -> void:
	_world.position = Vector3.ZERO
	_pivot.position = _world.pixel_to_world(px)
	_camera.position = Vector3(0.0, 0.0, distance)


func _measure_process(frames: int) -> Dictionary:
	var samples: Array[float] = []
	for _i in frames:
		samples.append(_time_process())
	return _summarize(samples)


## Pan contínuo: desloca o pivô antes de cada `_process`, de modo que o layout
## de tela (projeção + colisão de rótulos) entre no caminho medido.
func _measure_pan(frames: int) -> Dictionary:
	var samples: Array[float] = []
	for _i in frames:
		_pivot.position.x += 1.6
		samples.append(_time_process())
	return _summarize(samples)


static func _summarize(samples: Array[float]) -> Dictionary:
	var total := 0.0
	var peak := 0.0
	for value in samples:
		total += value
		peak = maxf(peak, value)
	return {"avg": total / float(maxi(samples.size(), 1)), "max": peak}


func _time_process() -> float:
	var start := Time.get_ticks_usec()
	_world._process(1.0 / 60.0)
	return _elapsed_ms(start)


func _count_nodes(root: Node) -> int:
	var stack: Array[Node] = [root]
	var count := 0
	while not stack.is_empty():
		var node: Node = stack.pop_back()
		count += 1
		stack.append_array(node.get_children())
	return count


func _mem_mb() -> float:
	return float(OS.get_static_memory_usage()) / (1024.0 * 1024.0)


static func _elapsed_ms(start_usec: int) -> float:
	return float(Time.get_ticks_usec() - start_usec) / 1000.0


func _write_json(path: String, data: Dictionary) -> void:
	var absolute := ProjectSettings.globalize_path(path)
	var dir := absolute.get_base_dir()
	if not DirAccess.dir_exists_absolute(dir):
		DirAccess.make_dir_recursive_absolute(dir)
	var file := FileAccess.open(absolute, FileAccess.WRITE)
	if file == null:
		push_warning("não gravou " + absolute)
		return
	file.store_string(JSON.stringify(data, "  "))
	file.close()
