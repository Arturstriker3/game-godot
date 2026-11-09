extends Node3D

## Captura automatizada do mapa em níveis de zoom fixos.
##
## Não altera economia, cidades nem cenas do jogo: apenas instancia o mapa
## atual (CityWorld) ou um baseline plano com os mesmos assets e fotografa a
## viewport. Cada execução grava PNGs + manifest.json com os parâmetros exatos
## de câmera, resolução e versão da engine.
##
## Uso:
##   godot --path . res://tools/map_capture.tscn -- --set=map3d --tag=before
##   godot --path . res://tools/map_capture.tscn -- --set=flat --tag=before
##   godot --path . res://tools/map_capture.tscn -- --set=all --tag=before
##
## Argumentos: --set=map3d|flat|legacy2d|all  --mode=political|geographic
##             --tag=<rótulo>  --out=<dir>  --width=1600  --height=900
##             --run=<nome da pasta da execução>

const CityWorldScript: Script = preload("res://map/city_world.gd")
const RELIEF: Texture2D = preload("res://map/map_data/normal.png")
const ATLAS_REGION_SDF: Texture2D = preload("res://map/map_data/atlas/region_sdf.png")
const ATLAS_REGION_COLOR: Texture2D = preload("res://map/map_data/atlas/region_color.png")
const ATLAS_WORLD_SDF: Texture2D = preload("res://map/map_data/atlas/world_sdf.png")
const ATLAS_WORLD_COLOR: Texture2D = preload("res://map/map_data/atlas/world_color.png")
const PAPER_SHADER: Shader = preload("res://map/map_shaders/paper_map.gdshader")
const POLITICAL_SHADER: Shader = preload("res://map/map_shaders/political_overlay.gdshader")

## Mesmo enquadramento do jogo (game/main_game.gd).
const PITCH_DEG := -56.0
const CAMERA_FOV := 75.0
const DISTANCES := [120.0, 52.0, 26.0, 13.0]
const ORTHO_SIZES := [500.0, 250.0, 125.0, 62.5]
const LEGACY_WAIT_FRAMES := 900

var _set := "map3d"
var _mode := "political"
var _tag := "before"
var _out_root := "res://docs/map_visual"
var _run := ""
var _width := 1600
var _height := 900
var _bench := false
## Opcional: id da cidade a destacar (ex.: FRA-PAR) para conferir a seleção.
var _select := ""
## Opcional: id da cidade a enquadrar no centro (ex.: FRA-CAY).
var _focus := ""
## Overrides de território: pares [city_id, tag] via --territory.
var _territory_pairs: Array[Array] = []

const BENCH_WARMUP := 45
const BENCH_FRAMES := 180

var _pivot: Node3D
var _camera: Camera3D
var _manifest := {"shots": {}, "warnings": []}


func _ready() -> void:
	_parse_args()
	if _run.is_empty():
		_run = Time.get_datetime_string_from_system().replace(":", "-")
	DirAccess.make_dir_recursive_absolute(
			ProjectSettings.globalize_path("%s/%s" % [_out_root, _run]))

	get_window().size = Vector2i(_width, _height)
	await get_tree().process_frame

	_manifest["engine"] = Engine.get_version_info().get("string", "")
	_manifest["renderer"] = ProjectSettings.get_setting(
			"rendering/renderer/rendering_method", "")
	_manifest["set"] = _set
	_manifest["mode"] = _mode
	_manifest["tag"] = _tag
	_manifest["resolution"] = [_width, _height]
	_manifest["timestamp"] = Time.get_datetime_string_from_system()
	_manifest["bench"] = {}

	if _bench:
		DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)

	match _set:
		"map3d":
			await _capture_map3d()
		"flat":
			await _capture_flat()
		"legacy2d":
			await _capture_legacy2d()
		"all":
			await _capture_map3d()
			await _clear_capture_nodes()
			await _capture_flat()
		_:
			push_error("conjunto desconhecido: %s" % _set)

	_write_manifest()
	get_tree().quit()


func _parse_args() -> void:
	for arg in OS.get_cmdline_user_args():
		var key := arg.get_slice("=", 0)
		var value := arg.get_slice("=", 1)
		match key:
			"--set":
				_set = value
			"--mode":
				_mode = value
			"--tag":
				_tag = value
			"--out":
				_out_root = value
			"--run":
				_run = value
			"--width":
				_width = int(value)
			"--height":
				_height = int(value)
			"--bench":
				_bench = value != "0" and value != "false"
			"--select":
				_select = value
			"--focus":
				_focus = value
			"--territory":
				_territory_pairs.clear()
				for chunk in value.split(","):
					if chunk.is_empty():
						continue
					var parts := chunk.split(":")
					if parts.size() == 2 and not parts[0].is_empty():
						_territory_pairs.append([parts[0], parts[1]])


# --- Mapa 3D atual ----------------------------------------------------------

func _capture_map3d() -> void:
	var status: Dictionary = Economy.sim.get_status()
	var cities: Array = status.get("cities", [])
	var ports: Array = status.get("ports", [])
	if cities.is_empty():
		_manifest["warnings"].append("nenhuma cidade no status da economia")
		return

	_build_environment()
	_build_camera_rig(PITCH_DEG)

	var world: Node3D = CityWorldScript.new()
	add_child(world)
	world.build(cities, ports)
	_apply_territory_overrides(world)
	_center_world(world, cities)
	if not _focus.is_empty():
		# `city_world_position()` devolve a posição GLOBAL (inclui o
		# world.position atual). Zera antes de medir para não somar o centro.
		world.position = Vector3.ZERO
		world.position = -world.city_world_position(_focus)
		_manifest["focus"] = _focus
	_manifest["city_count"] = cities.size()
	world.set_mode(_mode)

	for distance in DISTANCES:
		_camera.position = Vector3(0.0, 0.0, distance)
		await get_tree().process_frame
		if not _select.is_empty():
			world.select_city(_select)
		var urban := world.find_children("Urban_*", "", true, false)
		await _shot("map3d_%s_d%03d.png" % [_mode, int(distance)], {
			"kind": "perspective",
			"pitch_deg": PITCH_DEG,
			"fov": CAMERA_FOV,
			"distance": distance,
			"mode": _mode,
			"urban_cities": urban.size(),
		})
		if _bench:
			await _benchmark("d%03d" % int(distance))


## Mesmo procedimento de game/main_game.gd: centraliza as cidades na origem.
func _center_world(world: Node3D, cities: Array) -> void:
	var center: Vector3 = world.pixel_to_world(CityWorldScript.framing_center_px(cities))
	world.position = -center


## Aplica overrides de território (--territory=CITY:TAG,...) após o build.
func _apply_territory_overrides(world: Node3D) -> void:
	for pair in _territory_pairs:
		world.debug_set_city_nation(String(pair[0]), String(pair[1]))


# --- Baseline plano (mesmos assets, sem perspectiva nem cidades) ------------

func _capture_flat() -> void:
	_build_environment()
	_build_flat_map(_mode)
	_build_ortho_camera()

	for size in ORTHO_SIZES:
		_camera.size = size
		await _shot("flat_%s_s%03d.png" % [_mode, int(size)], {
			"kind": "orthographic",
			"ortho_size": size,
			"mode": _mode,
		})


func _build_flat_map(mode: String) -> void:
	var plane := PlaneMesh.new()
	plane.size = Vector2(1000.0, 500.0)

	var paper := ShaderMaterial.new()
	paper.shader = PAPER_SHADER
	paper.set_shader_parameter("region_sdf", ATLAS_REGION_SDF)
	paper.set_shader_parameter("world_sdf", ATLAS_WORLD_SDF)
	paper.set_shader_parameter("relief_texture", RELIEF)

	var ground := MeshInstance3D.new()
	ground.name = "FlatGround"
	ground.mesh = plane
	ground.material_override = paper
	add_child(ground)

	if mode != "political":
		return

	var political := ShaderMaterial.new()
	political.shader = POLITICAL_SHADER
	political.set_shader_parameter("region_sdf", ATLAS_REGION_SDF)
	political.set_shader_parameter("region_color", ATLAS_REGION_COLOR)
	political.set_shader_parameter("world_sdf", ATLAS_WORLD_SDF)
	political.set_shader_parameter("world_color", ATLAS_WORLD_COLOR)

	var overlay := MeshInstance3D.new()
	overlay.name = "FlatPolitical"
	overlay.mesh = plane
	overlay.material_override = political
	overlay.position = Vector3(0.0, 0.13, 0.0)
	overlay.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	add_child(overlay)


# --- Mapa 2D histórico (editor/map.tscn) — melhor esforço -------------------

func _capture_legacy2d() -> void:
	var scene: PackedScene = load("res://editor/editor.tscn")
	if scene == null:
		_manifest["warnings"].append("editor.tscn indisponível")
		return

	var editor := scene.instantiate()
	add_child(editor)

	var map: Node = editor.get("map")
	for _frame in LEGACY_WAIT_FRAMES:
		await get_tree().process_frame
		if map != null and map.get("province_image") != null:
			break
	if map == null or map.get("province_image") == null:
		_manifest["warnings"].append("mapa 2D histórico: banco não carregou a tempo")
		editor.queue_free()
		return

	for child in editor.get_children():
		if child is CanvasLayer:
			child.visible = false

	_build_camera_rig(PITCH_DEG)
	for distance in DISTANCES:
		_camera.position = Vector3(0.0, 0.0, distance)
		await _shot("legacy2d_%s_d%03d.png" % [_mode, int(distance)], {
			"kind": "perspective",
			"pitch_deg": PITCH_DEG,
			"fov": CAMERA_FOV,
			"distance": distance,
			"mode": _mode,
			"source": "res://editor/editor.tscn",
		})


# --- Infraestrutura ---------------------------------------------------------

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


func _build_camera_rig(pitch_deg: float) -> void:
	_pivot = Node3D.new()
	_pivot.rotation_degrees.x = pitch_deg
	add_child(_pivot)

	_camera = Camera3D.new()
	_camera.fov = CAMERA_FOV
	_camera.near = 0.1
	_camera.far = 4000.0
	_pivot.add_child(_camera)
	_camera.current = true


func _build_ortho_camera() -> void:
	_camera = Camera3D.new()
	_camera.projection = Camera3D.PROJECTION_ORTHOGONAL
	_camera.transform = Transform3D(
		Basis(Vector3.RIGHT, -PI * 0.5), Vector3(0.0, 200.0, 0.001))
	_camera.near = 0.1
	_camera.far = 1000.0
	add_child(_camera)
	_camera.current = true


func _clear_capture_nodes() -> void:
	for child in get_children():
		remove_child(child)
		child.queue_free()
	await get_tree().process_frame


## Mede tempo por frame, FPS, draw calls e memória com a câmera parada.
func _benchmark(label: String) -> void:
	for _frame in BENCH_WARMUP:
		await get_tree().process_frame

	var last := Time.get_ticks_usec()
	var total := 0.0
	var worst := 0.0
	for _frame in BENCH_FRAMES:
		await get_tree().process_frame
		var now := Time.get_ticks_usec()
		var delta_ms := float(now - last) / 1000.0
		last = now
		total += delta_ms
		worst = maxf(worst, delta_ms)

	var average := total / float(BENCH_FRAMES)
	var vram := Performance.get_monitor(Performance.RENDER_VIDEO_MEM_USED) / 1048576.0
	_manifest["bench"][label] = {
		"avg_ms": snappedf(average, 0.001),
		"worst_ms": snappedf(worst, 0.001),
		"fps": snappedf(1000.0 / maxf(average, 0.001), 0.1),
		"draw_calls": Performance.get_monitor(Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME),
		"primitives": Performance.get_monitor(Performance.RENDER_TOTAL_PRIMITIVES_IN_FRAME),
		"video_mem_mb": snappedf(vram, 0.1),
		"texture_mem_mb": snappedf(
				Performance.get_monitor(Performance.RENDER_TEXTURE_MEM_USED) / 1048576.0, 0.1),
	}
	print("[bench] %s avg=%.2f ms (%.1f fps) worst=%.2f ms draw=%d vram=%.1f MB" % [
		label, average, 1000.0 / maxf(average, 0.001), worst,
		_manifest["bench"][label]["draw_calls"], vram])


func _shot(file_name: String, params: Dictionary) -> void:
	await get_tree().process_frame
	await get_tree().process_frame
	await RenderingServer.frame_post_draw

	var image := get_viewport().get_texture().get_image()
	var path := "%s/%s/%s" % [_out_root, _run, file_name]
	var error: Error = image.save_png(path)

	var entry := params.duplicate()
	entry["path"] = path
	entry["resolution"] = [image.get_width(), image.get_height()]
	entry["saved"] = error == OK
	_manifest["shots"][file_name] = entry
	print("[capture] %s %dx%d %s" % [
		"ok" if error == OK else "erro", image.get_width(), image.get_height(), path])


func _write_manifest() -> void:
	var path := "%s/%s/manifest_%s_%s.json" % [_out_root, _run, _set, _mode]
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		push_error("manifest não gravado: %s" % path)
		return
	file.store_string(JSON.stringify(_manifest, "  "))
	file.close()
	print("[capture] manifest %s" % path)
