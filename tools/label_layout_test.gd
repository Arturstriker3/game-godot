extends Node3D

## Teste headless dos rótulos do mapa (cidade + porto), no padrão simples:
##  (a) ZERO linhas-guia (sem Line2D / CanvasLayer de líderes);
##  (b) nenhum nome visível se sobrepõe a outro nome visível;
##  (c) toda capital com âncora na tela mostra o nome, no zoom padrão e no
##      zoom out;
##  (d) cada ícone de porto fica a no máximo ~12 px do ponto de costa do
##      ports.csv (nunca deslocado para longe, nunca com linha);
##  (e) no zoom out só capitais e cidades grandes mantêm o nome (LOD), e nenhum
##      rótulo atravessa outro;
##  (f) todo nome de nação visível está encaixado no território (≥95% do
##      retângulo dentro do dono) e não cruza outro nome de nação nem o rótulo de
##      capital do LONGE (cruzar o texto de uma cidade é permitido: o país é
##      tinta de fundo);
##  (g) no LONGE (d260) os nomes de nação continuam aparecendo — no chão para os
##      países grandes, sob a estrela da capital para os pequenos.
##
## Focos: Benelux/Ruhr (BEL-BRU), Ilhas Britânicas (GBR-LDN), Glasgow, Amsterdã
## e Paris, em 1920×1080 e 1366×768; Bálcãs, Caribe e França no LONGE.
##
## Uso:
##   godot --headless --path . res://tools/label_layout_test.tscn

const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const PORTS_PATH := "res://data/definitions/scenario/ports.json"

const PITCH_DEG := -56.0
const CAMERA_FOV := 75.0
const DEFAULT_DISTANCE := 52.0
const ZOOM_OUT_DISTANCE := 120.0
const FAR_DISTANCE := 260.0
const MAX_PORT_OFFSET_PX := 12.5
## Encaixe mínimo do nome de nação no próprio território (regra
## `CityWorld._fit_nation_text`): 95% do retângulo de amostragem dentro do dono.
const NATION_INSIDE_MIN := 0.95
## Folga de colisão do layout (`LABEL_COLLIDE_PAD_PX` em `map/city_world.gd`):
## um nome cede quando o bloco vizinho encosta — não só quando há interseção
## estrita. O teste precisa da mesma folga para reconhecer quem bloqueou.
const LABEL_YIELD_PAD_PX := 3.0
## Nome de nação legível: piso de ~14 px e tinta mínima de 0,53 (alfa da cor
## 0,66 × fade). O tamanho é proporcional ao eixo do país na tela (55–70%), com
## tolerância para o encaixe no território (`pixel_size_max`) e o shrink.
const NATION_SCREEN_MIN_PX := 13.0
const NATION_INK_MIN_ALPHA := 0.53
const NATION_PROPORTION_MIN := 0.60

const SIZES := [Vector2i(1920, 1080), Vector2i(1366, 768)]
const FOCUSES := ["BEL-BRU", "GBR-LDN", "GBR-GLA", "NED-AMS", "FRA-PAR"]
const ZOOM_OUT_FOCUSES := ["BEL-BRU", "FRA-PAR"]
const FAR_FOCUSES := ["SRB-BEL", "CUB-HAV", "FRA-PAR"]

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
	# A UI do jogo está em pt-BR: os nomes de nação no mapa vêm do mesmo I18n e
	# precisam sair localizados (FRANÇA, EGITO, ITÁLIA), não o nome inglês do
	# atlas. `set_locale` não grava preferência do usuário.
	I18n.set_locale("pt_BR")
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
			await _settle()
			_check_default(size, focus, cities)
		for focus in ZOOM_OUT_FOCUSES:
			_focus(focus)
			_camera.position = Vector3(0.0, 0.0, ZOOM_OUT_DISTANCE)
			await _settle()
			_check_zoom_out(size, focus, cities)
		for focus in FAR_FOCUSES:
			_focus(focus)
			_camera.position = Vector3(0.0, 0.0, FAR_DISTANCE)
			await _settle()
			_check_far(size, focus)

	_finish()


## Espera o layout fatiado, o fade de nível e o fade dos nomes de nação
## assentarem (máx. 90 frames). O fade de 150 ms pode atravessar os dois frames
## antigos do teste; sem isso a checagem lia o layout anterior.
func _settle() -> void:
	for _frame in 90:
		await get_tree().process_frame
		if not bool(_world.get("_layout_job_active")) \
				and not bool(_world.get("_layout_dirty")) \
				and not bool(_world.get("_layout_pending")) \
				and not bool(_world.get("_layout_after_scan")) \
				and not bool(_world.get("_nation_labels_dirty")) \
				and (_world.get("_level_fade_queue") as Array).is_empty() \
				and (_world.get("_nation_fade_active") as Array).is_empty():
			return


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


## (c2) Toda capital na tela tem bandeira CARREGADA e visível. A bandeira é o
## último elemento a sumir junto com o nome: se o nome colidiu e foi ocultado,
## a bandeira continua; se o nome está visível, a bandeira também precisa estar.
func _check_capital_flags(tag: String, report: Array, screen: Rect2) -> void:
	var checked := 0
	for entry: Dictionary in report:
		if String(entry["kind"]) != "city" or not bool(entry["capital"]):
			continue
		var rect: Rect2 = entry["rect"]
		if rect.size.x <= 0.0 or not rect.intersects(screen):
			continue
		checked += 1
		var id := String(entry["id"])
		if not bool(entry.get("flag_loaded", false)):
			_failures.append("%s: capital %s sem bandeira carregada" % [tag, id])
			continue
		if bool(entry["text_visible"]) and not bool(entry["flag_visible"]):
			_failures.append("%s: capital %s com nome mas sem bandeira" % [tag, id])
	if checked == 0:
		_failures.append("%s: nenhuma capital com bandeira no layout" % tag)


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
	_check_capital_flags(tag, report, screen)
	_check_ports(tag, report)
	_check_port_clearance(tag, report, screen)
	_check_nation_labels(tag, report, _world.nation_label_report(), screen)

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
	_check_capital_flags(tag, report, screen)
	_check_ports(tag, report)
	_check_nation_labels(tag, report, _world.nation_label_report(), screen)

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


## (f) Nomes de nação: encaixe no território e nenhum cruzamento entre eles.
## Todo nome visível tem ≥95% do retângulo dentro do dono (a mesma regra de
## `CityWorld._fit_nation_text`), nenhum cruza outro nome de nação nem o rótulo
## de capital do LONGE. Cruzar o TEXTO de uma cidade é permitido por projeto: o
## nome do país é tinta de fundo (render_priority 2) e o nome da cidade fica por
## cima - a alternativa era a Europa sem nenhum nome de país no PERTO/MÉDIO.
func _check_nation_labels(tag: String, city_report: Array, nation_report: Array,
		screen: Rect2) -> void:
	var nations: Array = []
	var capitals: Array = []
	var city_texts := 0
	for entry: Dictionary in city_report:
		if String(entry.get("kind", "")) == "city" \
				and bool(entry.get("text_visible", false)):
			city_texts += 1
	for entry: Dictionary in nation_report:
		if not bool(entry.get("visible", false)):
			continue
		var rect: Rect2 = entry.get("rect", Rect2())
		if rect.size.x <= 0.0 or not rect.intersects(screen):
			continue
		match String(entry.get("kind", "")):
			"nation":
				var inside := float(entry.get("inside", 1.0))
				if inside < NATION_INSIDE_MIN:
					_failures.append(
							"%s: nome de nação %s com %.0f%% fora do território"
							% [tag, entry.get("tag", "?"), (1.0 - inside) * 100.0])
				var nation_tag := String(entry.get("tag", ""))
				var shown := String(entry.get("text", ""))
				var full_name := I18n.nation_name(nation_tag).to_upper()
				var short_name := I18n.nation_short(nation_tag).to_upper()
				if shown != full_name and shown != short_name:
					_failures.append(
							"%s: nome de nação %s fora do pt-BR ('%s'; esperado '%s')"
							% [tag, nation_tag, shown, full_name])
				var screen_h := float(entry.get("screen_h", 0.0))
				if screen_h < NATION_SCREEN_MIN_PX:
					_failures.append(
							"%s: nome de nação %s com %.1f px (piso %.0f)"
							% [tag, nation_tag, screen_h, NATION_SCREEN_MIN_PX])
				var ink_alpha := float(entry.get("ink_alpha", 0.0)) \
						* float(entry.get("alpha", 0.0))
				if ink_alpha < NATION_INK_MIN_ALPHA:
					_failures.append(
							"%s: nome de nação %s com tinta fraca (%.2f)"
							% [tag, nation_tag, ink_alpha])
				var target_h := float(entry.get("screen_target_h", 0.0))
				if target_h > 0.0 and screen_h < target_h * NATION_PROPORTION_MIN:
					_failures.append(
							"%s: nome de nação %s com %.1f px; alvo proporcional %.1f px"
							% [tag, nation_tag, screen_h, target_h])
				nations.append(entry)
			"capital":
				capitals.append(entry)

	for i in nations.size():
		for j in range(i + 1, nations.size()):
			var a: Dictionary = nations[i]
			var b: Dictionary = nations[j]
			if (a["rect"] as Rect2).intersects(b["rect"]):
				_failures.append("%s: nomes de nação sobrepostos %s × %s" % [
					tag, a.get("tag", "?"), b.get("tag", "?")])
	for nation: Dictionary in nations:
		var nr: Rect2 = nation["rect"]
		for capital: Dictionary in capitals:
			if nr.intersects(capital["rect"]):
				_failures.append("%s: nome de nação %s sobre a capital %s" % [
					tag, nation.get("tag", "?"), capital.get("tag", "?")])

	# Marcador de capital (estrela + bandeira) é intocável: o nome do país
	# desliza ao longo do próprio eixo para outra posição livre ou simplesmente
	# não aparece — nunca cobre o símbolo do lugar.
	for entry: Dictionary in city_report:
		if String(entry.get("kind", "")) != "city" \
				or not bool(entry.get("capital", false)) \
				or not bool(entry.get("marker_visible", false)):
			continue
		var icon_rect: Rect2 = entry.get("icon_rect", Rect2())
		var flag_rect: Rect2 = entry.get("flag_rect", Rect2())
		for nation: Dictionary in nations:
			var nr: Rect2 = nation["rect"]
			if icon_rect.size.x > 0.0 \
					and icon_rect.grow(LABEL_YIELD_PAD_PX).intersects(nr):
				_failures.append("%s: nome de nação %s sobre o marcador da capital %s" % [
					tag, nation.get("tag", "?"), entry.get("id", "?")])
			elif flag_rect.size.x > 0.0 \
					and flag_rect.grow(LABEL_YIELD_PAD_PX).intersects(nr):
				_failures.append("%s: nome de nação %s sobre a bandeira de %s" % [
					tag, nation.get("tag", "?"), entry.get("id", "?")])

	# O hover de cidade/porto reordena só o layout de cidade: se ele invalidasse
	# os nomes de nação, mexer o mouse faria os nomes dos países piscarem.
	var before := _visible_nation_tags(nation_report)
	_world.set_hover("BEL-BRU")
	if bool(_world.get("_nation_labels_dirty")):
		_failures.append("%s: hover de cidade invalidou os nomes de nação" % tag)
	if _visible_nation_tags(_world.nation_label_report()) != before:
		_failures.append("%s: hover de cidade mudou os nomes de nação" % tag)
	_world.set_hover("")

	if not nations.is_empty():
		print("[labels] %s: %d nomes de nação sobre %d nomes de cidade, menor encaixe %.0f%%" % [
			tag, nations.size(), city_texts,
			_nation_min_inside(nations) * 100.0])


func _nation_min_inside(nations: Array) -> float:
	var lowest := 1.0
	for entry: Dictionary in nations:
		lowest = minf(lowest, float(entry.get("inside", 1.0)))
	return lowest


## Tags visíveis dos nomes de nação (chão + sob a estrela), para comparar antes
## e depois de um evento que não deveria mexer neles.
func _visible_nation_tags(report: Array) -> Array[String]:
	var out: Array[String] = []
	for entry: Dictionary in report:
		if String(entry.get("kind", "")) == "city" \
				or not bool(entry.get("visible", false)):
			continue
		out.append(String(entry.get("kind", "")) + ":" + String(entry.get("tag", "")))
	out.sort()
	return out


## (g) LONGE: os nomes de nação continuam no mapa (chão nos países grandes,
## sob a estrela da capital nos pequenos) e passam nas mesmas checagens.
func _check_far(size: Vector2i, focus: String) -> void:
	var tag := _tag(size, focus, FAR_DISTANCE)
	var screen := Rect2(Vector2.ZERO, Vector2(size))
	var report: Array = _world.label_layout_report()
	var nation_report: Array = _world.nation_label_report()

	_check_no_lines(tag)
	_check_nation_labels(tag, report, nation_report, screen)

	var on_floor := 0
	var on_capital := 0
	for entry: Dictionary in nation_report:
		if not bool(entry.get("visible", false)):
			continue
		if not (entry.get("rect", Rect2()) as Rect2).intersects(screen):
			continue
		if String(entry.get("kind", "")) == "nation":
			on_floor += 1
		elif String(entry.get("kind", "")) == "capital":
			on_capital += 1
	if on_floor + on_capital == 0:
		_failures.append("%s: nenhum nome de nação no LONGE" % tag)
	print("[labels] %s: LONGE com %d nomes no chão e %d sob a capital" % [
		tag, on_floor, on_capital])


func _finish() -> void:
	if _failures.is_empty():
		print("[labels] OK: 0 linhas, 0 sobreposições, capitais e portos na costa.")
		get_tree().quit(0)
	else:
		print("[labels] FALHAS (%d):" % _failures.size())
		for failure in _failures:
			print(" - " + failure)
		get_tree().quit(1)
