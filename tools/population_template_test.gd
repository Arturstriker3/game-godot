extends SceneTree

## População no T2: abas Nações/Cidades, busca, ficha lateral e navegação para
## a ficha T3 na aba População. Também confere o atalho nação → cidades dela.
##
##   godot --headless --path . --script res://tools/population_template_test.gd

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

	print("=== population_template_test ===")
	if not await _wait_for_summary():
		_failures.append("economia não publicou resumo antes do teste")
		_finish()
		return

	_hud.open_screen("population")
	await _frames(4)
	var screen: Node = _hud.get("_screens").get("population")
	if screen == null:
		_failures.append("HUD sem tela de população")
		_finish()
		return
	if not screen.visible:
		_failures.append("população não ficou visível")
	if not bool(screen.call("uses_window_shell")):
		_failures.append("população não declara moldura própria (T2)")

	var tabs: Variant = screen.get("_tabs")
	if tabs == null or tabs.pages.size() != 2:
		_failures.append("população sem as duas abas de pasta")
	else:
		print("abas=%d (%s/%s)" % [tabs.pages.size(), tabs.buttons[0].text,
				tabs.buttons[1].text])

	var nations: Variant = screen.get("_tables").get("nations")
	if nations == null or nations.rows.is_empty():
		_failures.append("aba Nações sem nações")
	else:
		print("nações=%d primeira=%s cultura=%s pop=%s" % [nations.rows.size(),
				_cell(nations, 0, 0), _cell(nations, 0, 1), _cell(nations, 0, 2)])
		if _cell(nations, 0, 1).is_empty():
			_failures.append("coluna Cultura principal vazia")
		if _cell(nations, 0, 1) == "—":
			_failures.append("nação com cultura principal ausente")

	# Cartão lateral da nação: lista de culturas montada pelo CultureMixView.
	screen.call("select_nation", "BRA")
	await _frames(3)
	var nation_view: Variant = screen.get("_nation_cultures")
	if nation_view == null or (nation_view as Node).get_child_count() == 0:
		_failures.append("cartão da nação sem lista de culturas")
	else:
		var detail := _slot_node(screen.get("_root"), "nations.card.detail.name") \
				as Label
		var expected := _nation_name("BRA")
		if detail == null or detail.text != expected:
			_failures.append("cartão da nação não mostra o Brasil (%s)"
					% (detail.text if detail != null else "sem slot"))
		print("Brasil: %d culturas no cartão" % (nation_view as Node).get_child_count())

	screen.call("_set_tab", 1)
	await _frames(3)
	var cities: Variant = screen.get("_tables").get("cities")
	if cities == null or cities.rows.is_empty():
		_failures.append("aba Cidades sem cidades")
	else:
		print("cidades=%d primeira=%s cultura=%s pop=%s atend=%s"
				% [cities.rows.size(), _cell(cities, 0, 0),
				_cell(cities, 0, 2), _cell(cities, 0, 3), _cell(cities, 0, 7)])
		if _cell(cities, 0, 2).is_empty() or _cell(cities, 0, 2) == "—":
			_failures.append("coluna Cultura da cidade vazia")
	var city_view: Variant = screen.get("_city_cultures")
	if city_view == null or (city_view as Node).get_child_count() == 0:
		_failures.append("ficha lateral da cidade sem lista de culturas")

	var search: Variant = screen.get("_search")
	if search == null:
		_failures.append("aba Cidades sem busca")
	else:
		search.text = "zzzz"
		search.text_changed.emit("zzzz")
		await _frames(3)
		if cities != null and not cities.rows.is_empty():
			_failures.append("busca sem resultado manteve %d linhas"
					% cities.rows.size())
		var empty: Control = _slot_node(screen.get("_root"), "cities.empty")
		if empty == null or not empty.visible:
			_failures.append("estado vazio da busca não apareceu")
		search.text = ""
		search.text_changed.emit("")
		await _frames(3)

	# Duplo clique na primeira nação filtra as cidades dela.
	screen.call("_set_tab", 0)
	await _frames(2)
	screen.call("_on_row_activated", 0, "nations")
	await _frames(3)
	var shown: Array = screen.get("_shown_cities")
	if shown.is_empty() or shown.size() >= 807:
		_failures.append("duplo clique na nação não filtrou as cidades (%d)"
				% shown.size())
	else:
		var tag := String(shown[0].get("nation", ""))
		for city in shown:
			if String(city.get("nation", "")) != tag:
				_failures.append("filtro da nação misturou %s com %s"
						% [tag, String(city.get("nation", ""))])
				break
		print("nação→cidades=%d (%s)" % [shown.size(), tag])

	# Ativação de uma cidade empilha a ficha T3 na aba População.
	screen.call("_on_row_activated", 0, "cities")
	await _frames(3)
	var stack: Array = _hud.current_context()
	if stack.size() != 2 or String(stack.back().get("kind", "")) != "city":
		_failures.append("clique na cidade não abriu a ficha")
	elif String(stack.back().get("tab", "")) != "population":
		_failures.append("ficha abriu na aba %s, esperado population"
				% String(stack.back().get("tab", "")))

	# A ficha T3 desenha a mesma lista na aba População.
	var sheet: Node = _hud.get("_city_screen")
	if sheet == null:
		_failures.append("HUD sem ficha de cidade")
	else:
		var sheet_view: Variant = sheet.get("_culture_view")
		if sheet_view == null or (sheet_view as Node).get_child_count() == 0:
			_failures.append("ficha da cidade sem bloco de culturas")
		else:
			var city_id := String(sheet.call("current_city_id"))
			var mix := _city_mix_size(city_id)
			print("ficha %s: %d culturas" % [city_id, (sheet_view as Node).get_child_count()])
			if mix != (sheet_view as Node).get_child_count():
				_failures.append("ficha desenhou %d de %d culturas"
						% [(sheet_view as Node).get_child_count(), mix])
	_finish()


func _city_mix_size(city_id: String) -> int:
	var db := load("res://core/culture/culture_db.gd")
	return (db.city_mix(city_id) as Array).size()


func _nation_name(tag: String) -> String:
	var node := root.get_node_or_null("I18n")
	return "" if node == null else String(node.call("nation_name", tag))


func _cell(table, row: int, column: int) -> String:
	if row >= table.rows.size():
		return ""
	var cells: Array = table.rows[row]
	if column >= cells.size():
		return ""
	var cell: Variant = cells[column]
	return String(cell.get("text", "")) if cell is Dictionary else str(cell)


func _slot_node(source: Node, slot: String) -> Control:
	if source == null:
		return null
	var pending: Array[Node] = [source]
	while not pending.is_empty():
		var node: Node = pending.pop_back()
		if node is Control and (node as Control).get_meta("tpl_slot", "") == slot:
			return node as Control
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
		print("PASS: população em T2 com abas, busca, nação→cidades e ficha T3")
		quit(0)
		return
	for failure in _failures:
		push_error("[population_test] " + failure)
	quit(1)
