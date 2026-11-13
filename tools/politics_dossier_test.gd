extends SceneTree

## Política no T1 (Dossiê): a nação do jogador, a troca para outra nação pela
## lista, os dados reais do cenário/simulação e a declaração de diplomacia não
## simulada.
##
##   godot --headless --path . --script res://tools/politics_dossier_test.gd

const HUD_PATH := "res://core/ui/hud.gd"

var _failures: Array[String] = []
var _hud


func _init() -> void:
	_run.call_deferred()


func _run() -> void:
	root.size = Vector2i(1920, 1080)
	var hud_script := load(HUD_PATH) as Script
	if hud_script == null:
		_failures.append("não carregou %s" % HUD_PATH)
		_finish()
		return
	_hud = hud_script.new()
	root.add_child(_hud)

	print("=== politics_dossier_test ===")
	if not await _wait_for_summary():
		_failures.append("economia não publicou resumo antes do teste")
		_finish()
		return

	_hud.open_screen("politics")
	await _frames(5)
	var screen: Node = _hud.get("_screens").get("politics")
	if screen == null:
		_failures.append("HUD sem tela de política")
		_finish()
		return
	if not bool(screen.call("uses_window_shell")):
		_failures.append("política não declara moldura própria (T1)")
	if String(screen.call("current_nation")) != "GBR":
		_failures.append("o dossiê não abriu na nação do jogador")

	var slots: Dictionary = screen.get("_slots")
	var population := _text(slots, "kpi.population.value")
	var gdp := _text(slots, "kpi.gdp.value")
	var food := _text(slots, "kpi.food.value")
	var cash := _text(slots, "kpi.cash.value")
	print("GBR pop=%s pib=%s comida=%s caixa=%s" % [population, gdp, food, cash])
	if population.is_empty() or population == "—":
		_failures.append("população do dossiê vazia")
	if not gdp.begins_with("$"):
		_failures.append("PIB do dossiê não veio do cenário: %s" % gdp)
	if not food.ends_with("%"):
		_failures.append("comida do dossiê não é autossuficiência: %s" % food)
	if cash != "—":
		_failures.append("caixa do governo deveria declarar ausência, veio %s" % cash)
	if not _has_label(screen.get("_root"),
			_i18n("tpl.dossie.relations.note")):
		_failures.append("nota de diplomacia não simulada ausente")
	if not _has_label(screen.get("_root"), _i18n("pol.picker.title")):
		_failures.append("lista de nações ausente")
	if not _has_label(screen.get("_root"),
			_i18n("pol.culture.line").to_upper()):
		_failures.append("rótulo de cultura principal/aceitas ausente")
	var culture := _text(slots, "culture.line")
	print("GBR cultura=%s" % culture)
	if culture.is_empty() or culture == "—":
		_failures.append("linha de cultura do dossiê vazia")

	# Bandeira: arte de 640 px (não o w40 do mapa), proporção do país
	# preservada (GBR 2:1 dentro do envelope 160×96) e moldura de 1 px.
	var flag_rect := _flag_rect(screen.get("_root"))
	if flag_rect == null or flag_rect.texture == null:
		_failures.append("dossiê sem bandeira desenhada")
	else:
		var tex := flag_rect.texture
		print("bandeira GBR=%dx%d exibida em %s" % [tex.get_width(),
				tex.get_height(), flag_rect.get_parent().custom_minimum_size])
		if tex.get_width() < 500:
			_failures.append("bandeira do dossiê usa a arte pequena: %d px"
					% tex.get_width())
		if flag_rect.stretch_mode != TextureRect.STRETCH_KEEP_ASPECT_CENTERED:
			_failures.append("bandeira do dossiê pode esticar (stretch_mode %d)"
					% flag_rect.stretch_mode)
		var fitted: Vector2 = (flag_rect.get_parent() as Control).custom_minimum_size
		if not fitted.is_equal_approx(Vector2(160.0, 80.0)):
			_failures.append("envelope da bandeira 2:1 deveria ser 160×80, veio %s"
					% fitted)

	var picker: Variant = screen.get("_picker")
	if picker == null:
		_failures.append("sem tabela de nações")
	else:
		print("lista de nações=%d" % picker.rows.size())
		if picker.rows.size() < 100:
			_failures.append("lista de nações incompleta: %d" % picker.rows.size())

	# Troca de nação: o dossiê passa a mostrar a França.
	screen.call("select_nation", "FRA")
	await _frames(4)
	slots = screen.get("_slots")
	var fra_population := _text(slots, "kpi.population.value")
	var fra_culture := _text(slots, "culture.line")
	print("FRA pop=%s pib=%s cultura=%s" % [fra_population,
			_text(slots, "kpi.gdp.value"), fra_culture])
	if String(screen.call("current_nation")) != "FRA":
		_failures.append("selecionar a França não trocou o dossiê")
	if fra_population == population:
		_failures.append("dossiê da França repetiu a população do Reino Unido")
	if fra_culture.is_empty() or fra_culture == "—":
		_failures.append("linha de cultura da França vazia")
	if fra_culture == culture:
		_failures.append("linha de cultura não trocou com a nação")

	# "Ver no mapa" emite a nação selecionada.
	var seen := {"tag": ""}
	screen.connect("map_requested", func(tag: String) -> void: seen["tag"] = tag)
	screen.call("_on_choice", "map")
	if String(seen["tag"]) != "FRA":
		_failures.append("Ver no mapa não emitiu FRA (veio %s)"
				% String(seen["tag"]))
	_finish()


func _text(slots: Dictionary, key: String) -> String:
	var node := slots.get(key) as Label
	return node.text if node != null else ""


func _i18n(key: String) -> String:
	var node := root.get_node_or_null("I18n")
	return "" if node == null else String(node.call("t", key))


func _has_label(source: Node, text: String) -> bool:
	if source == null:
		return false
	var pending: Array[Node] = [source]
	while not pending.is_empty():
		var node: Node = pending.pop_back()
		if node is Label and (node as Label).text == text:
			return true
		for child in node.get_children():
			pending.append(child)
	return false


## Primeiro TextureRect dentro de um envelope de bandeira do template.
func _flag_rect(source: Node) -> TextureRect:
	if source == null:
		return null
	var pending: Array[Node] = [source]
	while not pending.is_empty():
		var node: Node = pending.pop_back()
		if node is Control and node.has_meta("tpl_flag_envelope"):
			for child in node.get_children():
				if child is TextureRect:
					return child as TextureRect
		for child in node.get_children():
			pending.append(child)
	return null


func _frames(count: int) -> void:
	for _frame in count:
		await process_frame


func _wait_for_summary() -> bool:
	for _frame in 600:
		var eco := root.get_node_or_null("Economy")
		if eco != null and not (eco.get("last_summary") as Dictionary).is_empty():
			return true
		await process_frame
	return false


func _finish() -> void:
	_hud.queue_free()
	if _failures.is_empty():
		print("PASS: dossiê T1 com dados reais e troca de nação")
		quit(0)
		return
	for failure in _failures:
		push_error("[politics_test] " + failure)
	quit(1)
