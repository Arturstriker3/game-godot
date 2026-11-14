extends SceneTree

## Mercado no T2: abas Bens/Ofertas, tabela dos 113 bens com ícone e preço na
## moeda local, filtro por setor, busca e detalhe do preço entregue.
##
##   godot --headless --path . --script res://tools/market_template_test.gd

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

	print("=== market_template_test ===")
	if not await _wait_for_summary():
		_failures.append("economia não publicou resumo antes do teste")
		_finish()
		return

	_hud.open_screen("market")
	await _frames(5)
	var screen: Node = _hud.get("_screens").get("market")
	if screen == null:
		_failures.append("HUD sem tela de mercado")
		_finish()
		return
	if not screen.visible:
		_failures.append("mercado não ficou visível")

	var tabs: Variant = screen.get("_tabs")
	if tabs == null or tabs.pages.size() != 2:
		_failures.append("mercado sem as duas abas de pasta")
	else:
		print("abas=%d (%s/%s)" % [tabs.pages.size(), tabs.buttons[0].text,
				tabs.buttons[1].text])

	var cities: Variant = screen.get("_city_table")
	if cities == null or cities.rows.is_empty():
		_failures.append("tabela de cidades vazia")
	else:
		print("cidades=%d primeira=%s" % [cities.rows.size(), _cell(cities, 0, 0)])

	var goods: Variant = screen.get("_goods_table")
	if goods == null or goods.rows.size() != 113:
		_failures.append("tabela de bens com %d linhas (esperado 113)"
				% (0 if goods == null else goods.rows.size()))
	else:
		var first: Dictionary = goods.rows[0][0]
		print("bens=%d primeiro=%s preço=%s" % [goods.rows.size(),
				String(first.get("text", "")), _cell(goods, 0, 1)])
		if String(first.get("icon", "")).is_empty():
			_failures.append("primeira célula de bem sem ícone")
		var price := _cell(goods, 0, 1)
		if price.is_empty():
			_failures.append("preço local vazio")
		_check_icons(goods)

	screen.call("_set_sector", "food")
	await _frames(3)
	if goods != null and goods.rows.size() >= 113:
		_failures.append("filtro de setor não reduziu a tabela")
	else:
		print("setor food=%d" % (0 if goods == null else goods.rows.size()))
	screen.call("_set_sector", "all")
	await _frames(2)

	screen.call("_on_good_search", "zzzz")
	await _frames(3)
	if goods != null and not goods.rows.is_empty():
		_failures.append("busca sem resultado manteve %d bens"
				% goods.rows.size())
	screen.call("_on_good_search", "")
	await _frames(2)

	screen.call("_set_tab", 1)
	await _frames(3)
	var offers: Variant = screen.get("_offers_table")
	if offers == null:
		_failures.append("aba Ofertas sem tabela")
	else:
		var empty: Control = _slot_node(screen.get("_root"), "market.offers.empty")
		print("ofertas=%d vazio=%s detalhe=%s" % [offers.rows.size(),
				str(empty != null and empty.visible),
				_text(screen.get("_slots"), "market.detail.name")])
		if offers.rows.is_empty() and (empty == null or not empty.visible):
			_failures.append("sem ofertas e sem estado vazio honesto")
		if not offers.rows.is_empty() and empty != null and empty.visible:
			_failures.append("ofertas presentes com estado vazio visível")
		if offers.columns.size() != 7:
			_failures.append("ofertas com %d colunas (esperado 7: cidade, origem, tarifa, CIF, frete, total, camada)"
					% offers.columns.size())
		_check_offer_costs(screen)

	screen.call("_set_tab", 0)
	await _frames(2)
	screen.call("_on_city_selected", 0)
	await _frames(3)
	if _text(screen.get("_slots"), "market.nation.name").is_empty():
		_failures.append("resumo da nação vazio")
	_finish()


## O pedido cru misturava unidades: origem por tonelada e frete/tarifa TOTAIS
## da carga. O cálculo da tela tem de devolver tudo por tonelada e fechar:
## 300 + 5 = 305 CIF; 305 + 15 = 320 total.
func _check_offer_costs(screen: Node) -> void:
	var costs: Dictionary = screen.call("offer_costs", {
		"amount": 20.0,
		"fx_rate": 2.0,
		"seller_fx_rate": 2.0,
		"origin_price": 300.0,
		"origin_price_anchor": 600.0,
		"tariff": 200.0,
		"freight_cost": 400.0,
		"delivered_price": 320.0,
	})
	var origin := float(costs.get("origin", -1.0))
	var tariff := float(costs.get("tariff", -1.0))
	var cif := float(costs.get("cif", -1.0))
	var freight := float(costs.get("freight", -1.0))
	var total := float(costs.get("total", -1.0))
	if not is_equal_approx(origin, 300.0) or not is_equal_approx(tariff, 5.0) \
			or not is_equal_approx(cif, 305.0) \
			or not is_equal_approx(freight, 15.0) \
			or not is_equal_approx(cif + freight, total):
		_failures.append("custo/t não fecha: origem=%.2f tarifa=%.2f CIF=%.2f frete=%.2f total=%.2f"
				% [origin, tariff, cif, freight, total])
	else:
		print("custo/t: origem=%.2f tarifa=%.2f CIF=%.2f frete=%.2f total=%.2f"
				% [origin, tariff, cif, freight, total])


## Cada linha tem o glifo do Icon Forge e o selo do setor legível sobre o papel.
## Um conjunto só: nenhum bem resolve para `assets/icons/products/` (o antigo
## game-icons herdado) e o selo não é tingido com a cor do bem.
func _check_icons(table) -> void:
	var paper := Color("#efe6cc")
	var missing: Array[String] = []
	var mixed: Array[String] = []
	var faint: Array[String] = []
	var uncolored: Array[String] = []
	for row in table.rows:
		var cell: Dictionary = row[0]
		var id := String(cell.get("icon", ""))
		if id.is_empty() or ProductIcons.texture(id) == null:
			missing.append(id if not id.is_empty() else "?")
			continue
		if not ProductIcons.icon_path(id).begins_with(ProductIcons.GOODS_DIR):
			mixed.append(id)
		var sector := String(cell.get("sector", ""))
		if not ProductIcons.SECTOR_TINTS.has(sector):
			uncolored.append(id)
			continue
		var seal_bg := ProductIcons.seal_background(sector, paper)
		if ProductIcons.contrast(ProductIcons.INK, seal_bg) < 3.0:
			faint.append(id)
	if not missing.is_empty():
		_failures.append("%d bens sem textura de ícone: %s"
				% [missing.size(), ", ".join(missing.slice(0, 5))])
	if not mixed.is_empty():
		_failures.append("%d bens fora do conjunto do Icon Forge: %s"
				% [mixed.size(), ", ".join(mixed.slice(0, 5))])
	if not uncolored.is_empty():
		_failures.append("%d bens em setor sem cor no selo: %s"
				% [uncolored.size(), ", ".join(uncolored.slice(0, 5))])
	if not faint.is_empty():
		_failures.append("%d selos sem contraste sobre o papel: %s"
				% [faint.size(), ", ".join(faint.slice(0, 5))])
	if missing.is_empty() and mixed.is_empty() and uncolored.is_empty() \
			and faint.is_empty():
		print("ícones=%d no Icon Forge, selo do setor com contraste ≥ 3:1"
				% table.rows.size())


func _cell(table, row: int, column: int) -> String:
	if row >= table.rows.size():
		return ""
	var cells: Array = table.rows[row]
	if column >= cells.size():
		return ""
	var cell: Variant = cells[column]
	return String(cell.get("text", "")) if cell is Dictionary else str(cell)


func _text(slots: Dictionary, slot: String) -> String:
	var node := slots.get(slot) as Label
	return node.text if node != null else ""


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
		print("PASS: mercado com 113 bens, ícones, moeda local e ofertas")
		quit(0)
		return
	for failure in _failures:
		push_error("[market_test] " + failure)
	quit(1)
