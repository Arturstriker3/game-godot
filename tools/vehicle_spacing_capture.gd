extends Node3D

## Captura de perto dos veículos de comércio para conferir o espaçamento de
## caminhões (estrada) e navios (porto) depois da mudança em
## `map/convoy_spacing.gd`.
##
## Usa o cenário e os pedidos REAIS da economia (nada de comboio sintético):
## avança alguns dias como o `convoy_lifecycle_test`, deixa os tweens andarem
## um pouco e congela. A câmera enquadra o aglomerado de caminhões mais denso e
## o navio mais próximo de um porto de partida. Com `--bench=1` mede também o
## custo por frame com esses veículos na tela.
##
## Uso (janela, igual ao map_capture; nunca abre a cena principal):
##   godot --path . res://tools/vehicle_spacing_capture.tscn -- \
##       --run=after --tag=after --bench=1

const CityWorldScript: Script = preload("res://map/city_world.gd")
const ConvoysScript: Script = preload("res://map/convoy_spacing.gd")

const OUT_ROOT := "res://docs/map_visual/vehicle_spacing"
const PITCH_DEG := -56.0
const CAMERA_FOV := 75.0
const SHOT_DISTANCES: Array[float] = [26.0, 13.0]
const BENCH_WARMUP := 60
const BENCH_FRAMES := 180
## Segundos de tween (ritmo acelerado) antes de congelar: o suficiente para os
## veículos saírem do ponto exato de origem e a faixa aparecer.
const TWEEN_RATE := 10.0
const TWEEN_SECONDS := 0.35

var _run := ""
var _tag := "after"
var _width := 1600
var _height := 900
var _bench := false
## Dias de pedidos reais enfileirados antes da foto (1..3).
var _days := 2
## Usa o script-base (sem faixas/escala nova) para uma foto "antes".
var _legacy := false

var _world: Node3D
var _convoys: Node3D
var _pivot: Node3D
var _camera: Camera3D
var _manifest := {
	"engine": "",
	"renderer": "",
	"tag": "",
	"day": 0,
	"convoys": {},
	"shots": {},
	"bench": {},
	"warnings": [],
}


func _ready() -> void:
	_parse_args()
	if _run.is_empty():
		_run = Time.get_datetime_string_from_system().replace(":", "-")
	DirAccess.make_dir_recursive_absolute(
			ProjectSettings.globalize_path("%s/%s" % [OUT_ROOT, _run]))
	get_window().size = Vector2i(_width, _height)
	await get_tree().process_frame

	_manifest["engine"] = Engine.get_version_info().get("string", "")
	_manifest["renderer"] = ProjectSettings.get_setting(
			"rendering/renderer/rendering_method", "")
	_manifest["tag"] = _tag
	_manifest["legacy"] = _legacy
	_manifest["resolution"] = [_width, _height]
	_manifest["timestamp"] = Time.get_datetime_string_from_system()
	if _bench:
		# Sem vsync, senão o número medido é só o intervalo de 60 Hz.
		DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	await _run_capture()
	_write_manifest()
	get_tree().quit(0)


func _parse_args() -> void:
	for arg in OS.get_cmdline_user_args():
		var key := arg.get_slice("=", 0)
		var value := arg.get_slice("=", 1)
		match key:
			"--run":
				_run = value
			"--tag":
				_tag = value
			"--width":
				_width = int(value)
			"--height":
				_height = int(value)
			"--bench":
				_bench = value != "0" and value != "false"
			"--days":
				_days = clampi(int(value), 1, 3)
			"--legacy":
				_legacy = value != "0" and value != "false"


func _run_capture() -> void:
	Economy.set_speed(0)
	await get_tree().process_frame
	if Economy._ticker.is_running():
		Economy._ticker.wait()
	Economy.set_process(false)
	var sim: Object = Economy.sim
	var ai: Object = Economy._ai
	if sim == null or ai == null:
		_manifest["warnings"].append("autoload Economy sem sim/IA")
		return

	var status: Dictionary = sim.get_status()
	var cities: Array = status.get("cities", [])
	var ports: Array = status.get("ports", [])
	if cities.is_empty():
		_manifest["warnings"].append("nenhuma cidade no status da economia")
		return

	_build_environment()
	_build_camera_rig()
	_world = CityWorldScript.new()
	add_child(_world)
	_world.build(cities, ports)
	_world.set_mode("political")
	# Mesmo enquadramento do jogo: cidades centradas na origem.
	_world.position = -_world.pixel_to_world(
			CityWorldScript.framing_center_px(cities))
	_manifest["city_count"] = cities.size()
	_manifest["port_count"] = ports.size()

	var script: Script = ConvoysScript
	if _legacy:
		script = preload("res://core/economy/trade_convoys.gd")
	_convoys = script.new()
	_world.add_child(_convoys)
	_convoys.set_time_scale(1.0, true)

	var day := 0
	for next_day in range(1, _days + 1):
		var summary: Dictionary = sim.tick_day()
		ai.run(next_day)
		day = next_day
		_convoys.spawn_orders(summary.get("trade_orders", []))
		_drain_convoys()
		var counts := _mode_counts()
		print("[vehicle] dia %d: %d comboios (%d caminhões, %d navios, %d aviões)" % [
			day, _convoys._convoys.size(), counts["truck"], counts["ship"],
			counts["plane"]])
		if _convoys._convoys.size() >= 20:
			break
	_manifest["day"] = day

	# Deixa os tweens andarem um pouco (congelados de novo logo em seguida):
	# os veículos saem do ponto de origem e as faixas ficam visíveis.
	_convoys.set_time_scale(TWEEN_RATE, false)
	await get_tree().create_timer(TWEEN_SECONDS).timeout
	_convoys.set_time_scale(1.0, true)
	await get_tree().process_frame

	var modes := _mode_counts()
	_manifest["convoys"] = modes
	_manifest["convoy_total"] = _convoys._convoys.size()

	await _capture_trucks()
	await _capture_ship()
	if _bench:
		await _benchmark("road_near")


func _mode_counts() -> Dictionary:
	var modes := {"truck": 0, "ship": 0, "plane": 0}
	for convoy: Node3D in _convoys._convoys:
		var mode := String(convoy.get_meta("mode", "?"))
		modes[mode] = int(modes.get(mode, 0)) + 1
	return modes


## Consome as rotas do dia sem esperar frames (mesmo laço do `perf_day_peak`).
func _drain_convoys() -> void:
	var guard := 0
	while guard < 20000:
		guard += 1
		if _convoys._queue.is_empty() and _convoys._prep.is_empty() \
				and not _convoys._nav.active:
			return
		_convoys._pump(Time.get_ticks_usec() + _convoys.FRAME_BUDGET_US)


# --- Fotos -----------------------------------------------------------------

func _capture_trucks() -> void:
	var trucks: Array[Node3D] = []
	for convoy: Node3D in _convoys._convoys:
		if String(convoy.get_meta("mode", "")) == "truck":
			trucks.append(convoy)
	if trucks.is_empty():
		_manifest["warnings"].append("nenhum caminhão ativo na captura")
		return

	# Par de caminhões mais próximo: é o caso em que o espaçamento novo aparece
	# (dois veículos no mesmo corredor/faixa). O ponto de partida é o mais
	# interessante ("saída da cidade"), mas se os dois já se afastaram, o ponto
	# médio atual ainda mostra a fila na rota.
	var current := _closest_pair(trucks, false)
	var starts := _closest_pair(trucks, true)
	var use_start := float(starts["distance"]) < float(current["distance"])
	var chosen: Dictionary = starts if use_start else current
	var focus: Vector3 = chosen["focus"]
	var pair_distance := float(chosen["distance"])
	var close_distance := clampf(pair_distance * 1.5, 3.0, 13.0)
	_manifest["truck_pair"] = {
		"distance": snappedf(pair_distance, 0.01),
		"current_distance": snappedf(float(current["distance"]), 0.01),
		"start_distance": snappedf(float(starts["distance"]), 0.01),
		"source": "start" if use_start else "current",
		"focus": [snappedf(focus.x, 0.01), snappedf(focus.z, 0.01)],
	}
	await _shot_at("road_trucks_d013.png", focus, 13.0)
	await _shot_at("road_trucks_close_d%03d.png" % int(round(close_distance)),
			focus, close_distance)
	# Escala do caminhão: um enquadramento bem curto sobre o mesmo ponto.
	await _shot_at("truck_scale_d003.png", focus, 3.0)


## Par mais próximo (posição atual ou ponto de partida) e o ponto médio dele.
func _closest_pair(trucks: Array[Node3D], use_start: bool) -> Dictionary:
	var first: Vector3 = _truck_point(trucks[0], use_start)
	var second := first
	var closest := INF
	for i in trucks.size():
		for j in range(i + 1, trucks.size()):
			var a := _truck_point(trucks[i], use_start)
			var b := _truck_point(trucks[j], use_start)
			var distance := a.distance_to(b)
			if distance < closest:
				closest = distance
				first = a
				second = b
	if is_inf(closest):
		closest = 0.0
	return {"distance": closest, "focus": (first + second) * 0.5}


func _truck_point(convoy: Node3D, use_start: bool) -> Vector3:
	if use_start and convoy.has_meta("leg_start"):
		return _convoys.to_global(convoy.get_meta("leg_start") as Vector3)
	return convoy.global_position


func _capture_ship() -> void:
	var ships: Array[Node3D] = []
	for convoy: Node3D in _convoys._convoys:
		if String(convoy.get_meta("mode", "")) == "ship":
			ships.append(convoy)
	if ships.is_empty():
		_manifest["warnings"].append("nenhum navio ativo na captura")
		return
	# O navio mais próximo do próprio ponto de partida (a costa/porto): com o
	# tween só um pouco adiantado, ele ainda está na saída do porto.
	var choice: Node3D = ships[0]
	var best_distance := INF
	for convoy: Node3D in ships:
		if not convoy.has_meta("leg_start"):
			continue
		# `leg_start` é local ao nó de comboios; a posição do comboio é global.
		var start: Vector3 = _convoys.to_global(
				convoy.get_meta("leg_start") as Vector3)
		var distance := convoy.global_position.distance_to(start)
		if distance < best_distance:
			best_distance = distance
			choice = convoy
	if is_inf(best_distance):
		best_distance = 0.0
	_manifest["ship_focus"] = {
		"distance_from_start": snappedf(best_distance, 0.01),
		"focus": [snappedf(choice.global_position.x, 0.01),
				snappedf(choice.global_position.z, 0.01)],
	}
	for distance in SHOT_DISTANCES:
		await _shot_at("port_ship_d%03d.png" % int(distance),
				choice.global_position, distance)


func _shot_at(file_name: String, focus: Vector3, distance: float) -> void:
	_pivot.global_position = Vector3(focus.x, 0.0, focus.z)
	_camera.position = Vector3(0.0, 0.0, distance)
	await get_tree().process_frame
	await get_tree().process_frame
	await RenderingServer.frame_post_draw
	var image := get_viewport().get_texture().get_image()
	var path := "%s/%s/%s" % [OUT_ROOT, _run, file_name]
	var error: Error = image.save_png(path)
	_manifest["shots"][file_name] = {
		"path": path,
		"distance": distance,
		"focus": [snappedf(focus.x, 0.01), snappedf(focus.z, 0.01)],
		"error": error,
	}
	print("[vehicle] %s (%dx%d) foco=(%.2f, %.2f) d=%.0f" % [
		file_name, image.get_width(), image.get_height(),
		focus.x, focus.z, distance])


## Mede tempo por frame com a câmera parada e os comboios visíveis.
func _benchmark(label: String) -> void:
	for _frame in BENCH_WARMUP:
		await get_tree().process_frame
	var last := Time.get_ticks_usec()
	var total := 0.0
	var worst := 0.0
	var samples: Array[float] = []
	for _frame in BENCH_FRAMES:
		await get_tree().process_frame
		var now := Time.get_ticks_usec()
		var delta_ms := float(now - last) / 1000.0
		last = now
		total += delta_ms
		worst = maxf(worst, delta_ms)
		samples.append(delta_ms)
	samples.sort()
	var average := total / float(BENCH_FRAMES)
	var p95 := samples[clampi(int(ceil(float(samples.size()) * 0.95)) - 1,
			0, samples.size() - 1)]
	_manifest["bench"][label] = {
		"avg_ms": snappedf(average, 0.001),
		"p95_ms": snappedf(p95, 0.001),
		"worst_ms": snappedf(worst, 0.001),
		"frames": BENCH_FRAMES,
		"convoy_total": _convoys._convoys.size(),
	}
	print("[vehicle] bench %s avg=%.2f p95=%.2f worst=%.2f ms (%d comboios)" % [
		label, average, p95, worst, _convoys._convoys.size()])


# --- Infraestrutura --------------------------------------------------------

func _build_environment() -> void:
	var environment := Environment.new()
	environment.background_mode = Environment.BG_COLOR
	environment.background_color = Color(0.0392157, 0.054902, 0.0588235)
	environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
	environment.ambient_light_color = Color(0.619608, 0.658824, 0.580392)
	environment.ambient_light_energy = 0.45
	environment.tonemap_mode = Environment.TONE_MAPPER_FILMIC
	var holder := WorldEnvironment.new()
	holder.environment = environment
	add_child(holder)
	var light := DirectionalLight3D.new()
	light.transform = Transform3D(
		Vector3(-0.8660254, -0.43301278, 0.25),
		Vector3(0.0, 0.49999997, 0.86602545),
		Vector3(-0.50000006, 0.75, -0.43301266),
		Vector3.ZERO)
	light.shadow_enabled = true
	add_child(light)


func _build_camera_rig() -> void:
	_pivot = Node3D.new()
	_pivot.rotation_degrees.x = PITCH_DEG
	add_child(_pivot)
	_camera = Camera3D.new()
	_camera.fov = CAMERA_FOV
	_camera.near = 0.1
	_camera.far = 4000.0
	_pivot.add_child(_camera)
	_camera.current = true


func _write_manifest() -> void:
	var file := FileAccess.open("%s/%s/manifest.json" % [OUT_ROOT, _run],
			FileAccess.WRITE)
	if file == null:
		push_error("não abriu o manifest")
		return
	file.store_string(JSON.stringify(_manifest, "  "))
