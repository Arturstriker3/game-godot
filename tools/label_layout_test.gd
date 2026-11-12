extends Node3D

## Teste headless dos rótulos do mapa (cidade + porto), no padrão simples:
##  (a) ZERO linhas-guia (sem Line2D / CanvasLayer de líderes);
##  (b) nenhum nome visível se sobrepõe a outro nome visível;
##  (c) toda capital com âncora na tela mostra o nome, no zoom padrão e no
##      zoom out;
##  (d) cada ícone de porto fica a no máximo ~12 px do ponto de costa do
##      ports.csv (nunca deslocado para longe, nunca com linha);
##  (e) no zoom out só capitais e cidades grandes mantêm o nome (LOD), e nenhum
##      rótulo atravessa outro.
##
## Focos: Benelux/Ruhr (BEL-BRU), Ilhas Britânicas (GBR-LDN), Glasgow, Amsterdã
## e Paris, em 1920×1080 e 1366×768.
##
## Uso:
##   godot --headless --path . res://tools/label_layout_test.tscn

const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const PORTS_PATH := "res://data/definitions/scenario/ports.json"

const PITCH_DEG := -56.0
const CAMERA_FOV := 75.0
const DEFAULT_DISTANCE := 52.0
const ZOOM_OUT_DISTANCE := 120.0
const MAX_PORT_OFFSET_PX := 12.5
## Folga de colisão do layout (`LABEL_COLLIDE_PAD_PX` em `map/city_world.gd`):
## um nome cede quando o bloco vizinho encosta — não só quando há interseção
## estrita. O teste precisa da mesma folga para reconhecer quem bloqueou.
const LABEL_YIELD_PAD_PX := 3.0

const SIZES := [Vector2i(1920, 1080), Vector2i(1366, 768)]
const FOCUSES := ["BEL-BRU", "GBR-LDN", "GBR-GLA", "NED-AMS", "FRA-PAR"]
const ZOOM_OUT_FOCUSES := ["BEL-BRU", "FRA-PAR"]

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
			_check_default(size, focus, cities)
		for focus in ZOOM_OUT_FOCUSES:
			_focus(focus)
			_camera.position = Vector3(0.0, 0.0, ZOOM_OUT_DISTANCE)
			await get_tree().process_frame
			await get_tree().process_frame
			_check_zoom_out(size, focus, cities)

	_finish()


func _focus(city_id: String) -> void:
	_world.position = Vector3.ZERO
	_world.position = -_world.city_world_position(city_id)


func _tag(size: Vector2i, focus: String, distance: float) -> String:
	return "%s d%d @%dx%d" % [focus, int(distance), size.x, size.y]


## (a) Nenhuma linha-guia pode existir na cena do mapa.
func _check_no_lines(tag: String) -> void:
	var lines := _world.find_children("*", "Line2D", true, false)
	if not lines.is_empty():
		_failures.append("%s: %d Line2D de linha-guia" % [tag, lines.size()])
	if _world.get_node_or_null("LabelLeaders") != null:
		_failures.append("%s: CanvasLayer LabelLeaders existe" % tag)


## (b)/(e) Nenhum nome visível pode cruzar outro nome visível.
func _check_text_overlaps(tag: String, report: Array, screen: Rect2) -> void:
	var texts: Array = []
	for entry: Dictionary in report:
		if String(entry["kind"]) != "city" or not bool(entry["text_visible"]):
			continue
		var rect: Rect2 = entry["text_rect"]
		if rect.intersects(screen):
			texts.append(entry)
	for i in texts.size():
		for j in range(i + 1, texts.size()):
			var a: Dictionary = texts[i]
			var b: Dictionary = texts[j]
			var ra: Rect2 = a["text_rect"]
			var rb: Rect2 = b["text_rect"]
			if ra.intersects(rb):
				_failures.append("%s: nomes sobrepostos %s × %s" % [
					tag, a["id"], b["id"]])


## (c) Toda capital com âncora na tela tem nome visível — com uma exceção
## geométrica: quando duas capitais ocupam o mesmo punhado de pixels (Roma e
## Vaticano ficam a menos de 1 px; Skopje, Pristina, Tirana e Podgorica caem no
## mesmo canto dos Bálcãs), sem linhas-guia só um nome cabe.
## A capital de menor população pode ceder, e SÓ para outra capital (nunca para
## uma cidade comum), que é a regra de prioridade do layout.
func _check_capitals(tag: String, report: Array, cities: Array, screen: Rect2) -> void:
	var shown := {}
	var entry_by_id := {}
	for entry: Dictionary in report:
		if String(entry["kind"]) != "city":
			continue
		entry_by_id[String(entry["id"])] = entry
		if bool(entry["text_visible"]):
			shown[String(entry["id"])] = true
	for city: Dictionary in cities:
		if not bool(city.get("capital", false)):
			continue
		var city_id := String(city.get("id", ""))
		var world: Vector3 = _world.city_world_position(city_id)
		if _camera.is_position_behind(world):
			continue
		var projected := _camera.unproject_position(world)
		if not screen.grow(24.0).has_point(projected):
			continue
		if shown.has(city_id):
			continue
		var own: Dictionary = entry_by_id.get(city_id, {})
		if own.is_empty():
			_failures.append("%s: capital %s sem nome (fora do layout)" % [
				tag, city_id])
			continue
		var rect: Rect2 = own["text_rect"]
		var population := float(city.get("population", 0.0))
		# Cedeu a uma capital MAIOR (Roma sobre Vaticano; Skopje sobre Podgorica;
		# o ícone da capital vizinha também ocupa o bloco). Como as capitais são
		# aceitas antes de qualquer cidade comum, só outra capital pode tirar o
		# nome de uma capital.
		var yielded_to_capital := false
		var blocker := ""
		for other: Dictionary in report:
			if String(other["kind"]) != "city" \
					or String(other["id"]) == city_id \
					or not bool(other["capital"]):
				continue
			if not (other["rect"] as Rect2).grow(LABEL_YIELD_PAD_PX) \
					.intersects(rect):
				continue
			if blocker == "":
				blocker = String(other["id"])
			if float(other["population"]) > population:
				yielded_to_capital = true
				break
		if not yielded_to_capital:
			_failures.append("%s: capital %s sem nome (bloqueada por %s)" % [
				tag, city_id, blocker if blocker != "" else "cidade comum"])


## (d) Todo porto a no máximo ~12 px do ponto de costa. Porto fundido na cidade
## (badge) tem deslocamento zero; os demais só se afastam da costa o mínimo.
func _check_ports(tag: String, report: Array) -> void:
	for entry: Dictionary in report:
		if String(entry["kind"]) != "port":
			continue
		var offset: Vector2 = entry["offset"]
		if offset.length() > MAX_PORT_OFFSET_PX:
			_failures.append("%s: porto %s a %.1f px da costa" % [
				tag, entry["id"], offset.length()])
		if bool(entry["flag_visible"]):
			_failures.append("%s: porto %s com bandeira" % [tag, entry["id"]])
		if bool(entry["text_visible"]):
			_failures.append("%s: porto %s com nome" % [tag, entry["id"]])


## Porto não pode entrar no rótulo de cidade nem em outro porto (badge fundido
## na própria cidade é a única exceção, por definição).
func _check_port_clearance(tag: String, report: Array, screen: Rect2) -> void:
	var cities: Array = []
	var ports: Array = []
	for entry: Dictionary in report:
		if String(entry["kind"]) == "city":
			cities.append(entry)
		elif bool(entry["marker_visible"]) and not bool(entry["fused"]):
			ports.append(entry)
	for port: Dictionary in ports:
		var rect: Rect2 = port["rect"]
		if not rect.intersects(screen):
			continue
		for city: Dictionary in cities:
			if rect.intersects(city["rect"]):
				_failures.append("%s: porto %s sobre o rótulo de %s" % [
					tag, port["id"], city["id"]])
				break
	for i in ports.size():
		for j in range(i + 1, ports.size()):
			var a: Dictionary = ports[i]
			var b: Dictionary = ports[j]
			if (a["rect"] as Rect2).intersects(b["rect"]):
				_failures.append("%s: portos sobrepostos %s × %s" % [
					tag, a["id"], b["id"]])


func _check_default(size: Vector2i, focus: String, cities: Array) -> void:
	var tag := _tag(size, focus, DEFAULT_DISTANCE)
	var screen := Rect2(Vector2.ZERO, Vector2(size))
	var report: Array = _world.label_layout_report()

	_check_no_lines(tag)
	_check_text_overlaps(tag, report, screen)
	_check_capitals(tag, report, cities, screen)
	_check_ports(tag, report)
	_check_port_clearance(tag, report, screen)

	# Evidência (não falha): cobertura de nomes no zoom padrão.
	var on_screen := 0
	var named := 0
	for city: Dictionary in cities:
		var city_id := String(city.get("id", ""))
		var world: Vector3 = _world.city_world_position(city_id)
		if _camera.is_position_behind(world):
			continue
		if not screen.grow(24.0).has_point(_camera.unproject_position(world)):
			continue
		on_screen += 1
		for entry: Dictionary in report:
			if String(entry["kind"]) == "city" and String(entry["id"]) == city_id \
					and bool(entry["text_visible"]):
				named += 1
				break
	if on_screen > 0:
		print("[labels] %s: %d/%d nomes visíveis (%.0f%%)" % [
			tag, named, on_screen, 100.0 * float(named) / float(on_screen)])


func _check_zoom_out(size: Vector2i, focus: String, cities: Array) -> void:
	var tag := _tag(size, focus, ZOOM_OUT_DISTANCE)
	var screen := Rect2(Vector2.ZERO, Vector2(size))
	var report: Array = _world.label_layout_report()

	_check_no_lines(tag)
	_check_text_overlaps(tag, report, screen)
	_check_capitals(tag, report, cities, screen)
	_check_ports(tag, report)

	# No zoom out, só capitais e cidades grandes mantêm nome.
	var visible := 0
	for entry: Dictionary in report:
		if String(entry["kind"]) != "city" or not bool(entry["text_visible"]):
			continue
		var rect: Rect2 = entry["text_rect"]
		if not rect.intersects(screen):
			continue
		visible += 1
		if not bool(entry["capital"]) \
				and float(entry["population"]) < 1000000.0:
			_failures.append("%s: cidade pequena %s com nome no zoom out" % [
				tag, entry["id"]])
	if visible == 0:
		_failures.append("%s: nenhum rótulo no zoom out (nem capitais)" % tag)


func _finish() -> void:
	if _failures.is_empty():
		print("[labels] OK: 0 linhas, 0 sobreposições, capitais e portos na costa.")
		get_tree().quit(0)
	else:
		print("[labels] FALHAS (%d):" % _failures.size())
		for failure in _failures:
			print(" - " + failure)
		get_tree().quit(1)
