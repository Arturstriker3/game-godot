extends Node3D

## Bench do caminho de render do mapa com a câmera em cima da Europa.
##
## Mede o custo REAL de `CityWorld._process` durante pan contínuo e zoom
## (nível perto/médio/longe), além do frame parado em cada nível. Reporta
## média/p95/p99/máximo em ms, contagem de nós e um proxy de draw calls
## (geometria visível: MultiMesh + Label3D + Sprite3D + MeshInstance3D).
##
## Usa o cenário real (`cities.json`/`ports.json`) e o mesmo enquadramento do
## jogo (pitch -56°, fov 75°, 1920x1080). Não abre a cena principal.
##
## Cada cenário assenta antes de medir (fila de modelos, fade de nível e layout
## fatiado concluídos) e roda 24 frames de aquecimento FORA das amostras; o que
## é reportado é o custo de regime, com 600 amostras por cenário.
##
## Uso:
##   godot --headless --path . res://tools/camera_pan_bench.tscn -- \
##       --tag=before --out=res://docs/map_visual/lod_levels/camera_pan_bench_before.json
##   [--frames=600] [--world=res://map/city_world.gd]  (A/B: caminho do script)

const WORLD_PATH := "res://map/city_world.gd"
const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const PORTS_PATH := "res://data/definitions/scenario/ports.json"

const PITCH_DEG := -56.0
const CAMERA_FOV := 75.0
const NEAR_DISTANCE := 52.0
const MID_DISTANCE := 120.0
const FAR_DISTANCE := 260.0
## O aquecimento roda DEPOIS do assentamento (fila de modelos, fade de nível e
## layout fatiado concluídos) e fica fora das amostras: o que sobra medido é o
## custo de regime, não o da transição.
const WARMUP_FRAMES := 24
const SAMPLE_FRAMES := 600
const SETTLE_FRAMES := 900
const PAN_STEP := 1.2

var _tag := "run"
var _out := ""
var _focus := "BEL-BRU"
var _samples := SAMPLE_FRAMES
var _world_path := WORLD_PATH
var _world: Node3D
var _pivot: Node3D
var _camera: Camera3D
var _results := {}


func _ready() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--tag="):
			_tag = arg.split("=")[1]
		elif arg.begins_with("--out="):
			_out = arg.split("=")[1]
		elif arg.begins_with("--focus="):
			_focus = arg.split("=")[1]
		elif arg.begins_with("--frames="):
			_samples = maxi(int(arg.split("=")[1]), 60)
		elif arg.begins_with("--world="):
			_world_path = arg.split("=")[1]
	get_tree().root.size = Vector2i(1920, 1080)
	_run.call_deferred()


func _load_array(path: String) -> Array:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return []
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data if typeof(data) == TYPE_ARRAY else []


func _run() -> void:
	await get_tree().process_frame
	var cities := _load_array(CITIES_PATH)
	var ports := _load_array(PORTS_PATH)
	if cities.is_empty():
		print("[camera_bench] sem cidades; abortando")
		get_tree().quit(1)
		return

	var build_start := Time.get_ticks_usec()
	var script: Script = load(_world_path) as Script
	if script == null:
		print("[camera_bench] mundo não carregou: %s" % _world_path)
		get_tree().quit(1)
		return
	_world = script.new() as Node3D
	add_child(_world)
	_world.build(cities, ports)
	var build_ms := _elapsed_ms(build_start)

	_pivot = Node3D.new()
	_pivot.rotation_degrees.x = PITCH_DEG
	add_child(_pivot)
	_camera = Camera3D.new()
	_camera.fov = CAMERA_FOV
	_camera.near = 0.1
	_camera.far = 4000.0
	_pivot.add_child(_camera)
	_camera.current = true

	# Centraliza a Europa na origem (o mapa inteiro pode estar deslocado).
	_world.position = Vector3.ZERO
	if _world._records_by_id.has(_focus) or _world.has_method("city_world_position"):
		_world.position = -_world.city_world_position(_focus)
	await get_tree().process_frame
	_world.set_process(false)

	# Aquece: constrói o que o nível perto enfileira e assenta o layout.
	_aim_zoom(NEAR_DISTANCE)
	_settle()
	_wait(WARMUP_FRAMES)

	_results["tag"] = _tag
	_results["focus"] = _focus
	_results["cities"] = cities.size()
	_results["ports"] = ports.size()
	_results["build_ms"] = snappedf(build_ms, 0.01)
	_results["nodes"] = _count_nodes(_world)
	_results["engine"] = Engine.get_version_info().get("string", "")
	_results["sample_frames"] = _samples

	_results["near_pan"] = _measure_pan(NEAR_DISTANCE, PAN_STEP)
	_results["mid_pan"] = _measure_pan(MID_DISTANCE, PAN_STEP)
	_results["far_pan"] = _measure_pan(FAR_DISTANCE, PAN_STEP)
	_results["zoom_pan"] = _measure_zoom_pan()

	_aim_zoom(NEAR_DISTANCE)
	_results["near_idle"] = _measure_idle()
	_aim_zoom(MID_DISTANCE)
	_results["mid_idle"] = _measure_idle()
	_aim_zoom(FAR_DISTANCE)
	_results["far_idle"] = _measure_idle()

	_results["parts"] = {
		"labels": _world._label_pool.size(),
		"city_flag_nodes": _count_flag_nodes(),
		"multimesh_nodes": _count_multimesh(),
		"visible_proxy_draws": _visible_draw_proxy(),
	}
	_print_report()
	if not _out.is_empty():
		_write_json(_out, _results)
	get_tree().quit(0)


func _aim_zoom(distance: float) -> void:
	_pivot.position = Vector3.ZERO
	_camera.position = Vector3(0.0, 0.0, distance)
	_world._process(1.0 / 60.0)


func _wait(frames: int) -> void:
	for _i in frames:
		_world._process(1.0 / 60.0)


## Roda o mundo até filas, fade de nível e layout fatiado assentarem. Fica fora
## das amostras medido: o alvo do bench é o custo em regime, não a transição.
func _settle() -> void:
	for _i in SETTLE_FRAMES:
		_world._process(1.0 / 60.0)
		if _list_empty("_urban_queue") and _list_empty("_port_model_queue") \
				and _list_empty("_level_fade_queue") \
				and not _flag_on("_layout_job_active") \
				and not _flag_on("_layout_dirty") \
				and not _flag_on("_layout_pending"):
			return
	push_warning("[camera_bench] assentamento não terminou em %d frames"
			% SETTLE_FRAMES)


## Propriedade-lista do mundo está vazia (ou não existe nesta versão do script).
func _list_empty(prop: String) -> bool:
	var value: Variant = _world.get(prop)
	return not (value is Array) or (value as Array).is_empty()


## Propriedade booleana do mundo está ligada (ou não existe nesta versão).
func _flag_on(prop: String) -> bool:
	var value: Variant = _world.get(prop)
	return value is bool and bool(value)


func _measure_pan(distance: float, step: float) -> Dictionary:
	_aim_zoom(distance)
	_settle()
	_wait(WARMUP_FRAMES)
	var samples: Array[float] = []
	for _i in _samples:
		_pivot.position.x += step
		samples.append(_time_process())
	return _summarize(samples)


func _measure_zoom_pan() -> Dictionary:
	_aim_zoom(NEAR_DISTANCE)
	_settle()
	_wait(WARMUP_FRAMES)
	var samples: Array[float] = []
	for index in _samples:
		var t := float(index) / float(_samples - 1)
		var wave := 0.5 - 0.5 * cos(t * TAU)
		_camera.position.z = lerpf(NEAR_DISTANCE, FAR_DISTANCE, wave)
		_pivot.position.x += PAN_STEP
		samples.append(_time_process())
	return _summarize(samples)


func _measure_idle() -> Dictionary:
	_settle()
	_wait(WARMUP_FRAMES)
	var samples: Array[float] = []
	for _i in _samples:
		samples.append(_time_process())
	return _summarize(samples)


func _time_process() -> float:
	var start := Time.get_ticks_usec()
	_world._process(1.0 / 60.0)
	return _elapsed_ms(start)


static func _summarize(samples: Array[float]) -> Dictionary:
	var sorted := samples.duplicate()
	sorted.sort()
	var total := 0.0
	for value in samples:
		total += value
	var count := maxi(samples.size(), 1)
	return {
		"avg_ms": snappedf(total / float(count), 0.001),
		"p95_ms": snappedf(sorted[clampi(int(float(count) * 0.95), 0, count - 1)], 0.001),
		"p99_ms": snappedf(sorted[clampi(int(float(count) * 0.99), 0, count - 1)], 0.001),
		"max_ms": snappedf(sorted[count - 1], 0.001),
		"frames": count,
	}


func _count_nodes(root: Node) -> int:
	var stack: Array[Node] = [root]
	var count := 0
	while not stack.is_empty():
		var node: Node = stack.pop_back()
		count += 1
		stack.append_array(node.get_children())
	return count


func _count_multimesh() -> int:
	var count := 0
	for node in _world.find_children("*", "MultiMeshInstance3D", true, false):
		count += 1
	return count


func _count_flag_nodes() -> int:
	var count := 0
	for record in _world._city_nodes:
		if record.get("flag") is Sprite3D:
			count += 1
	return count


## Proxy de draw calls: geometria visível na árvore do mapa (uma draw call por
## nó visível, aproximação conservadora; um MeshInstance3D multi-surfície conta
## como 1).
func _visible_draw_proxy() -> int:
	var count := 0
	for node in _world.find_children("*", "GeometryInstance3D", true, false):
		var geometry := node as GeometryInstance3D
		if not geometry.is_visible_in_tree():
			continue
		if geometry is MultiMeshInstance3D:
			var mm := (geometry as MultiMeshInstance3D).multimesh
			if mm != null and mm.instance_count > 0:
				count += 1
		else:
			count += 1
	for label in _world.find_children("*", "Label3D", true, false):
		if (label as Label3D).is_visible_in_tree():
			count += 1
	return count


func _print_report() -> void:
	print("[camera_bench] %s | build %.0f ms | nós %d | draws(proxy) %d" % [
		_tag, _results["build_ms"], _results["nodes"], _results["parts"]["visible_proxy_draws"]])
	for key in ["near_pan", "mid_pan", "far_pan", "zoom_pan", "near_idle", "mid_idle", "far_idle"]:
		var data: Dictionary = _results[key]
		print("[camera_bench] %-9s avg %.2f | p95 %.2f | p99 %.2f | max %.2f ms" % [
			key, data["avg_ms"], data["p95_ms"], data["p99_ms"], data["max_ms"]])


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
