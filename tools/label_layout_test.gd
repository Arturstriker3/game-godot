extends Node3D

## Teste headless do layout dos rótulos do mapa (cidade + porto).
##
## Regras verificadas em zoom padrão (câmera a 52 u, pitch −56°, FOV 75):
##  (a) nenhum retângulo de rótulo/ícone visível se sobrepõe a outro;
##  (b) nenhuma cidade com âncora dentro da tela tem o nome escondido;
##  (c) cada cidade tem exatamente UMA bandeira visível (o porto da própria
##      cidade não duplica a bandeira);
##  (d) o selo do porto da própria cidade fica a ≥ PORT_MARKER_MIN_CITY_PX do
##      rótulo da cidade (nunca "come" o nome/ícone da cidade).
##
## Focos: Benelux/Ruhr, Ilhas Britânicas, Glasgow e Amsterdã, em 1920×1080 e
## 1366×768 (o layout é em pixels de tela, então a resolução importa).
##
## Uso:
##   godot --headless --path . res://tools/label_layout_test.tscn

const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const PORTS_PATH := "res://data/definitions/scenario/ports.json"

const PITCH_DEG := -56.0
const CAMERA_FOV := 75.0
const DEFAULT_DISTANCE := 52.0
const MIN_PORT_CITY_PX := 14.0

const SIZES := [Vector2i(1920, 1080), Vector2i(1366, 768)]
const FOCUSES := ["BEL-BRU", "GBR-MAN", "GBR-GLA", "NED-AMS"]

var _failures: Array[String] = []
var _world: Node3D
var _camera: Camera3D
var _pivot: Node3D


func _ready() -> void:
	_run.call_deferred()


func _load_array(path: String) -> Array:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_failures.append("não abriu " + path)
		return []
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data if typeof(data) == TYPE_ARRAY else []


func _run() -> void:
	var cities := _load_array(CITIES_PATH)
	var ports := _load_array(PORTS_PATH)
	if cities.is_empty():
		_finish()
		return

	get_tree().root.size = SIZES[0]
	await get_tree().process_frame

	_world = load("res://map/city_world.gd").new()
	add_child(_world)
	_world.build(cities, ports)

	_pivot = Node3D.new()
	_pivot.rotation_degrees.x = PITCH_DEG
	add_child(_pivot)
	_camera = Camera3D.new()
	_camera.fov = CAMERA_FOV
	_camera.near = 0.1
	_camera.far = 4000.0
	_pivot.add_child(_camera)
	_camera.current = true

	for size: Vector2i in SIZES:
		get_tree().root.size = size
		await get_tree().process_frame
		await get_tree().process_frame
		for focus in FOCUSES:
			_focus(focus)
			_camera.position = Vector3(0.0, 0.0, DEFAULT_DISTANCE)
			await get_tree().process_frame
			await get_tree().process_frame
			_check_layout(size, focus, cities)

	_finish()


func _focus(city_id: String) -> void:
	_world.position = Vector3.ZERO
	_world.position = -_world.city_world_position(city_id)


func _check_layout(size: Vector2i, focus: String, cities: Array) -> void:
	var screen := Rect2(Vector2.ZERO, Vector2(size))
	var report: Array = _world.label_layout_report()
	var visible: Array = []
	for entry: Dictionary in report:
		var rect: Rect2 = entry["rect"]
		if rect.intersects(screen):
			visible.append(entry)

	var tag := "%s @%dx%d" % [focus, size.x, size.y]

	# (a) 0 sobreposições de retângulos visíveis.
	for i in visible.size():
		for j in range(i + 1, visible.size()):
			var a: Dictionary = visible[i]
			var b: Dictionary = visible[j]
			var ra: Rect2 = a["rect"]
			var rb: Rect2 = b["rect"]
			if ra.intersects(rb):
				_failures.append("%s: rótulos sobrepostos %s:%s × %s:%s" % [
					tag, a["kind"], a["id"], b["kind"], b["id"]])

	# (b) nome de cidade visível dentro da tela não pode estar escondido.
	var shown := {}
	for entry: Dictionary in visible:
		if String(entry["kind"]) == "city" and bool(entry["text_visible"]):
			shown[String(entry["id"])] = true
	for city: Dictionary in cities:
		var city_id := String(city.get("id", ""))
		var world: Vector3 = _world.city_world_position(city_id)
		if _camera.is_position_behind(world):
			continue
		var projected := _camera.unproject_position(world)
		if not screen.grow(24.0).has_point(projected):
			continue
		if not shown.has(city_id):
			_failures.append("%s: nome da cidade %s escondido" % [tag, city_id])

	# (c) uma bandeira por cidade; porto da própria cidade sem bandeira.
	var flags := {}
	for entry: Dictionary in visible:
		if String(entry["kind"]) != "city" or not bool(entry["flag_visible"]):
			continue
		var city_id := String(entry["id"])
		flags[city_id] = int(flags.get(city_id, 0)) + 1
	for city_id in flags:
		if int(flags[city_id]) != 1:
			_failures.append("%s: cidade %s com %d bandeiras" % [
				tag, city_id, int(flags[city_id])])
	for entry: Dictionary in report:
		if String(entry["kind"]) == "port" and bool(entry["same_city"]) \
				and bool(entry["flag_visible"]):
			_failures.append("%s: porto %s da própria cidade duplica bandeira" % [
				tag, entry["id"]])

	# (d) distância mínima do selo do porto ao rótulo da própria cidade.
	var city_rects := {}
	for entry: Dictionary in report:
		if String(entry["kind"]) == "city":
			city_rects[String(entry["id"])] = entry["rect"]
	for entry: Dictionary in report:
		if String(entry["kind"]) != "port" or not bool(entry["same_city"]):
			continue
		var city_id := String(entry["city_id"])
		if not city_rects.has(city_id):
			continue
		var port_rect: Rect2 = entry["rect"]
		var city_rect: Rect2 = city_rects[city_id]
		if port_rect.grow(MIN_PORT_CITY_PX).intersects(city_rect):
			_failures.append("%s: porto %s colado no rótulo de %s" % [
				tag, entry["id"], city_id])

	# Evidência de deslocamento: o marcador deslocado nunca deve "voar" para
	# longe demais da âncora (linha-guia atravessando o mapa). Não é falha —
	# é medida impressa para comparar execuções (o porto de Belfast chegou a
	# 219 px antes do layout em anéis e ficou em ~93 px).
	var max_port := 0.0
	var max_port_id := ""
	for entry: Dictionary in report:
		if String(entry["kind"]) != "port":
			continue
		var offset: Vector2 = entry["offset"]
		if offset.length() > max_port:
			max_port = offset.length()
			max_port_id = String(entry["id"])
	print("[labels] %s: maior deslocamento de porto = %.1f px (%s)" % [
		tag, max_port, max_port_id])


func _finish() -> void:
	if _failures.is_empty():
		print("[labels] OK: 0 sobreposições, nomes visíveis, 1 bandeira/cidade e porto separado.")
		get_tree().quit(0)
	else:
		print("[labels] FALHAS (%d):" % _failures.size())
		for failure in _failures:
			print(" - " + failure)
		get_tree().quit(1)
