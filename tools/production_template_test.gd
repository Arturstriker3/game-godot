extends SceneTree

## Produção no T2: abas de pasta Cadeia/Cidades, tabelas com dados reais,
## busca, ficha lateral e navegação para a ficha T3 da cidade.
##
##   godot --headless --path . --script res://tools/production_template_test.gd

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

	print("=== production_template_test ===")
	if not await _wait_for_summary():
		_failures.append("economia não publicou resumo antes do teste")
		_finish()
		return

	_hud.open_screen("production")
	await _frames(4)
	var screen: Node = _hud.get("_screens").get("production")
	if screen == null:
		_failures.append("HUD sem tela de produção")
		_finish()
		return
	if not screen.visible:
		_failures.append("produção não ficou visível")
	if not bool(screen.call("uses_window_shell")):
		_failures.append("produção não declara moldura própria (T2)")

	var tabs: Variant = screen.get("_tabs")
	if tabs == null or tabs.pages.size() != 2:
		_failures.append("produção sem as duas abas de pasta")
	else:
		print("abas=%d (%s/%s)" % [tabs.pages.size(), tabs.buttons[0].text,
				tabs.buttons[1].text])

	var chain: Variant = screen.get("_tables").get("chain")
	if chain == null or chain.rows.is_empty():
		_failures.append("aba Cadeia sem cidades produtoras")
	else:
		print("cadeia=%d primeira=%s" % [chain.rows.size(), _cell(chain, 0, 0)])

	screen.call("_set_tab", "cities")
	await _frames(3)
	var cities: Variant = screen.get("_tables").get("cities")
	if cities == null or cities.rows.is_empty():
		_failures.append("aba Cidades sem cidades")
	else:
		print("cidades=%d primeira=%s" % [cities.rows.size(), _cell(cities, 0, 0)])

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

	# Ativação da primeira linha empilha a ficha T3 na aba Produção.
	screen.call("_on_row_activated", 0, "cities")
	await _frames(3)
	var stack: Array = _hud.current_context()
	if stack.size() != 2 or String(stack.back().get("kind", "")) != "city":
		_failures.append("clique na tabela não abriu a ficha da cidade")
	elif String(stack.back().get("tab", "")) != "production":
		_failures.append("ficha abriu na aba %s, esperado production"
				% String(stack.back().get("tab", "")))
	else:
		print("ficha=%s aba=%s" % [String(stack.back().get("id", "")),
				String(stack.back().get("tab", ""))])
	_finish()


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
		print("PASS: produção em T2 com abas, busca, ficha e navegação para o T3")
		quit(0)
		return
	for failure in _failures:
		push_error("[production_test] " + failure)
	quit(1)
