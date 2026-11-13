extends Node3D

## Captura do round de correções (mapa + HUD) em 1920x1080, sem abrir a cena
## principal: instancia o CityWorld real, o HUD real e a textura dinâmica do
## minimapa. Grava em `docs/ui/captures/fix_round/`:
##   - minimapa_mundo.png      (Europa, d120, HUD completo com o minimapa)
##   - nomes_paises_europa.png (Europa, d120)
##   - nomes_paises_mundo.png  (mundo, d260)
##
## Uso:
##   godot --path . res://tools/fix_round_capture.tscn

const CityWorldScript: Script = preload("res://map/city_world.gd")
const HudScript: Script = preload("res://core/ui/hud.gd")
const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const PORTS_PATH := "res://data/definitions/scenario/ports.json"
const OUT_DIR := "res://docs/ui/captures/fix_round"

const PITCH_DEG := -56.0
const CAMERA_FOV := 75.0
const EUROPE_DISTANCE := 120.0
const WORLD_DISTANCE := 260.0
const SETTLE_FRAMES := 40

var _world: Node3D
var _hud: CanvasLayer
var _pivot: Node3D
var _camera: Camera3D


func _ready() -> void:
	_run.call_deferred()


func _load_array(path: String) -> Array:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return []
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data if typeof(data) == TYPE_ARRAY else []


func _run() -> void:
	get_tree().root.size = Vector2i(1920, 1080)
	await get_tree().process_frame

	_world = CityWorldScript.new()
	add_child(_world)
	_world.build(_load_array(CITIES_PATH), _load_array(PORTS_PATH))

	_pivot = Node3D.new()
	_pivot.rotation_degrees.x = PITCH_DEG
	add_child(_pivot)
	_camera = Camera3D.new()
	_camera.fov = CAMERA_FOV
	_camera.near = 0.1
	_camera.far = 4000.0
	_pivot.add_child(_camera)
	_camera.current = true

	_hud = HudScript.new()
	add_child(_hud)
	_hud.set_map_mode("political")
	_hud.set_minimap_territory(_world.minimap_texture())
	await _settle()

	# Europa: minimapa e nomes de país no zoom padrão do mapa político.
	_world.position = -_world.city_world_position("BEL-BRU")
	_camera.position = Vector3(0.0, 0.0, EUROPE_DISTANCE)
	await _settle()
	_capture("nomes_paises_europa.png")
	_capture("minimapa_mundo.png")

	# Mundo: centro do plano, longe o bastante para ler vários continentes.
	_world.position = Vector3.ZERO
	_camera.position = Vector3(0.0, 0.0, WORLD_DISTANCE)
	await _settle()
	_capture("nomes_paises_mundo.png")

	print("[fix_round] capturas em " + OUT_DIR)
	get_tree().quit(0)


func _settle() -> void:
	for _i in SETTLE_FRAMES:
		await get_tree().process_frame


func _capture(file_name: String) -> void:
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(OUT_DIR))
	var image := get_viewport().get_texture().get_image()
	var path := "%s/%s" % [OUT_DIR, file_name]
	if image.save_png(ProjectSettings.globalize_path(path)) != OK:
		push_error("[fix_round] falhou ao gravar " + path)
