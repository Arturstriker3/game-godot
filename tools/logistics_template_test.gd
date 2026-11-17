extends SceneTree

## Logística no T2: Frota, Câmbio, Tarifas e Portos com dados reais, aplicação
## de tarifa (set_tariff) e expansão de porto (expand_port).
##
##   godot --headless --path . --script res://tools/logistics_template_test.gd

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

	print("=== logistics_template_test ===")
	if not await _wait_for_summary():
		_failures.append("economia não publicou resumo antes do teste")
		_finish()
		return

	_hud.open_screen("logistics")
	await _frames(5)
	var screen: Node = _hud.get("_screens").get("logistics")
	if screen == null:
		_failures.append("HUD sem tela de logística")
		_finish()
		return
	if not screen.visible:
		_failures.append("logística não ficou visível")

	var tabs: Variant = screen.get("_tabs")
	if tabs == null or tabs.pages.size() != 4:
		_failures.append("logística sem as quatro abas de pasta")
	else:
		print("abas=%d (%s/%s/%s/%s)" % [tabs.pages.size(),
				tabs.buttons[0].text, tabs.buttons[1].text,
				tabs.buttons[2].text, tabs.buttons[3].text])

	var fleet: Variant = screen.get("_fleet_table")
	if fleet == null or fleet.rows.is_empty():
		_failures.append("aba Frota sem nações com frota")
	else:
		print("frota=%d primeira=%s caminhões=%s"
				% [fleet.rows.size(), _cell(fleet, 0, 0), _cell(fleet, 0, 1)])
		var usage := _text(screen.get("_slots"), "fleet.trucks.text")
		if usage.is_empty():
			_failures.append("cartão da frota sem uso/saúde")

	screen.call("_set_tab", 1)
	await _frames(3)
	var fx: Variant = screen.get("_fx_table")
	if fx == null or fx.rows.is_empty():
		_failures.append("aba Câmbio sem moedas")
	else:
		print("câmbio=%d primeira=%s" % [fx.rows.size(), _cell(fx, 0, 0)])
		if _text(screen.get("_slots"), "fx.code") in ["", "—"]:
			_failures.append("cartão de câmbio sem moeda da nação")
		if _text(screen.get("_slots"), "fx.tariff_revenue") == "":
			_failures.append("cartão de câmbio sem receita de tarifas")

	screen.call("_set_tab", 2)
	await _frames(3)
	var partner: String = String(screen.get("_selected_partner"))
	var good: String = String(screen.get("_selected_good"))
	if partner.is_empty() or good.is_empty():
		_failures.append("editor de tarifa sem parceiro/bem")
	else:
		screen.call("_set_rate", 0.10)
		screen.call("_on_choice", "tariff_apply")
		await _frames(3)
		var result := _text(screen.get("_slots"), "tariff.result")
		print("tarifa: %s ← %s · %s · 10%% → %s"
				% [String(screen.get("_selected_nation")), partner, good, result])
		if not result.contains("10") and not result.contains("Tarifa aplicada"):
			_failures.append("resultado da tarifa não confirma a aplicação")
		var tariffs: Variant = screen.get("_tariff_table")
		if tariffs == null or tariffs.rows.is_empty():
			_failures.append("tabela de tarifas continua vazia após aplicar")
		else:
			print("tarifas=%d primeira=%s" % [tariffs.rows.size(),
					_cell(tariffs, 0, 0)])

	screen.call("_set_tab", 3)
	await _frames(3)
	var ports: Variant = screen.get("_port_table")
	if ports == null or ports.rows.is_empty():
		_failures.append("aba Portos sem portos")
	else:
		print("portos=%d primeiro=%s" % [ports.rows.size(), _cell(ports, 0, 0)])
		if _text(screen.get("_slots"), "port.name") in ["", "—"]:
			_failures.append("cartão do porto sem nome")
		screen.call("_set_expansion", 0.10)
		screen.call("_on_choice", "port_expand")
		await _frames(3)
		var port_result := _text(screen.get("_slots"), "port.result")
		print("expansão: %s" % port_result)
		if port_result.is_empty():
			_failures.append("expansão sem retorno na tela")
	_finish()


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
		print("PASS: logística com frota, câmbio, tarifa aplicada e porto")
		quit(0)
		return
	for failure in _failures:
		push_error("[logistics_test] " + failure)
	quit(1)
