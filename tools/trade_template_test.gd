extends SceneTree

## Verifica a janela de Comércio no T2 (abas de pasta) com dados reais.
##
## Monta o HUD sobre o resumo publicado pela economia e checa: moldura própria
## (sem painel modal escuro), Pedidos com tabela e trechos, Rotas com os três
## modos, Mercado com busca/filtro e estado vazio, e o clique numa cidade do
## mercado empilhando a ficha T3.
##
##   godot --headless --path . --script res://tools/trade_template_test.gd

const HUD_PATH := "res://core/ui/hud.gd"
const TAB_MARKET := 2

var _failures: Array[String] = []
var _hud


func _init() -> void:
	_run.call_deferred()


func _run() -> void:
	root.size = Vector2i(1920, 1080)
	var hud_script := load(HUD_PATH) as Script
	if hud_script == null:
		push_error("[trade_test] não carregou %s" % HUD_PATH)
		quit(1)
		return
	_hud = hud_script.new()
	root.add_child(_hud)

	print("=== trade_template_test ===")
	if not await _wait_for_summary():
		_failures.append("economia não publicou resumo antes do teste")
		_finish()
		return

	_hud.open_screen("trade")
	await _frames(4)
	var screen: Node = _hud.get("_screens").get("trade")
	if screen == null:
		_failures.append("HUD sem tela de comércio")
		_finish()
		return
	if not screen.visible:
		_failures.append("comércio não ficou visível")
	if _hud.get("_window").visible:
		_failures.append("painel modal escuro continuou aberto (esperado T2)")
	if not bool(screen.call("uses_window_shell")):
		_failures.append("comércio não declara moldura própria")

	var orders: Variant = screen.get("_orders_table")
	if orders == null:
		_failures.append("sem tabela de pedidos")
	else:
		print("pedidos=%d linha0=%s" % [orders.rows.size(),
				_cell(orders, 0, 1)])
		if orders.rows.is_empty():
			_failures.append("tabela de pedidos vazia no dia 1")
		var legs: Variant = screen.get("_legs_table")
		if legs == null or legs.rows.is_empty():
			_failures.append("trechos do pedido selecionado vazios")
		else:
			print("trechos=%d primeiro=%s" % [legs.rows.size(),
					_cell(legs, 0, 1)])

	var routes: Variant = screen.get("_routes_table")
	if routes == null or routes.rows.size() != 3:
		_failures.append("aba Rotas sem as três linhas de modo")
	else:
		print("rotas=%d modos=%s/%s/%s" % [routes.rows.size(),
				_cell(routes, 0, 0), _cell(routes, 1, 0), _cell(routes, 2, 0)])

	screen.call("_set_tab", TAB_MARKET)
	await _frames(3)
	var market: Variant = screen.get("_market_table")
	if market == null:
		_failures.append("sem tabela de mercado")
		_finish()
		return
	var total: int = market.rows.size()
	print("mercado=%d primeira=%s" % [total, _cell(market, 0, 0)])
	if total <= 0:
		_failures.append("mercado sem cidades")

	screen.call("_on_market_search", "zzzz")
	await _frames(3)
	if not market.rows.is_empty():
		_failures.append("busca sem resultado manteve %d linhas"
				% market.rows.size())
	var empty: Control = _slot_node(screen.get("_root"), "trade.market.empty")
	if empty == null or not empty.visible:
		_failures.append("estado vazio do mercado não apareceu")

	screen.call("_clear_market_filters")
	await _frames(3)
	if market.rows.size() != total:
		_failures.append("limpar filtros devolveu %d de %d linhas"
				% [market.rows.size(), total])
	if empty != null and empty.visible:
		_failures.append("estado vazio do mercado continuou visível")

	# Clique (ativação) na primeira cidade do mercado abre a ficha T3.
	screen.call("_on_market_row", 0)
	await _frames(3)
	var stack: Array = _hud.current_context()
	if stack.size() != 2 or String(stack.back().get("kind", "")) != "city":
		_failures.append("clique no mercado não abriu a ficha da cidade")
	_finish()


func _cell(table, row: int, column: int) -> String:
	if row >= table.rows.size():
		return ""
	var cells: Array = table.rows[row]
	if column >= cells.size():
		return ""
	var cell: Variant = cells[column]
	return String(cell.get("text", "")) if cell is Dictionary else str(cell)


## Nó com `slot` na spec, sem depender do tipo do gerador (que usa autoloads).
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
		print("PASS: comércio em T2 com pedidos, rotas, mercado e ficha")
		quit(0)
		return
	for failure in _failures:
		push_error("[trade_test] " + failure)
	quit(1)
