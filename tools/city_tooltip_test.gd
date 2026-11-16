extends SceneTree

## Tooltip rico de cidade (item 3): atraso de ~250 ms, conteúdo do status, modo
## de recursos, esconde ao sair e nunca aparece com tela aberta.
##
##   godot --headless --path . --script res://tools/city_tooltip_test.gd

const HUD_PATH := "res://core/ui/hud.gd"
const DELAY_WAIT := 0.35

var _failures: Array[String] = []
var _hud
var _i18n: Node
var _economy: Node


func _init() -> void:
	_run.call_deferred()


func _run() -> void:
	root.size = Vector2i(1920, 1080)
	_i18n = root.get_node_or_null("I18n")
	_economy = root.get_node_or_null("Economy")
	if _i18n == null or _economy == null:
		_failures.append("autoloads I18n/Economy ausentes")
		_finish()
		return
	var hud_script := load(HUD_PATH) as Script
	if hud_script == null:
		_failures.append("nao carregou %s" % HUD_PATH)
		_finish()
		return
	_hud = hud_script.new()
	root.add_child(_hud)
	print("=== city_tooltip_test ===")
	if not await _wait_for_summary():
		_failures.append("economia nao publicou resumo antes do teste")
		_finish()
		return

	var city := _first_city_with(true)
	if city.is_empty():
		_failures.append("nenhuma cidade com porto no resumo")
		_finish()
		return
	var point := Vector2(760.0, 520.0)

	# 1. Antes do atraso, nada aparece.
	_hud.hover_city_tooltip(String(city["id"]), point, "political")
	await _frames(2)
	if _hud.call("city_tooltip_visible"):
		_failures.append("tooltip apareceu antes dos ~250 ms")

	# 2. Depois do atraso, a ficha aparece com os dados do status.
	await create_timer(DELAY_WAIT).timeout
	_hud.hover_city_tooltip(String(city["id"]), point, "political")
	await _frames(2)
	if not _hud.call("city_tooltip_visible"):
		_failures.append("tooltip nao apareceu depois do atraso")
		_finish()
		return
	var texts := _tooltip_texts()
	_expect(texts, _t("tooltip.city.population"), "populacao")
	_expect(texts, _t("tooltip.city.food"), "comida")
	_expect(texts, _t("tooltip.city.deficit"), "deficit")
	_expect(texts, _t("tooltip.city.deposits"), "jazidas")
	_expect(texts, _t("tooltip.city.port"), "porto")
	_expect(texts, _city_name(city), "nome da cidade")
	_expect(texts, _nation_name(String(city.get("nation", ""))), "nacao")

	var before := _card().global_position
	_hud.hover_city_tooltip(String(city["id"]), point + Vector2(120.0, 60.0),
			"political")
	await _frames(2)
	if _card().global_position == before:
		_failures.append("tooltip nao acompanhou o ponto da cidade")

	# 3. Modo recursos mostra o recurso principal entre as jazidas.
	var mine := _city_with_deposits()
	if mine.is_empty():
		print("aviso: nenhuma cidade com jazida no resumo; modo recursos nao conferido")
	else:
		_hud.preview_city_tooltip(String(mine["id"]), point, "resources")
		await _frames(2)
		var resource_text := _tooltip_texts()
		var main_label := _main_resource_label(mine)
		if main_label.is_empty() \
				or not " | ".join(resource_text).contains(main_label):
			_failures.append("modo recursos sem o recurso principal (%s): %s"
					% [main_label, " | ".join(resource_text)])

	# 4. Sair da cidade (ou parar de mirar) esconde.
	_hud.clear_city_tooltip()
	await _frames(1)
	if _hud.call("city_tooltip_visible"):
		_failures.append("tooltip continuou visivel depois de sair do hover")

	# 5. Tela aberta: nunca aparece, mesmo com o mouse parado em cima.
	_hud.open_screen("statistics")
	await _frames(3)
	_hud.hover_city_tooltip(String(city["id"]), point, "political")
	await create_timer(DELAY_WAIT).timeout
	_hud.hover_city_tooltip(String(city["id"]), point, "political")
	await _frames(2)
	if _hud.call("city_tooltip_visible"):
		_failures.append("tooltip apareceu com uma tela aberta")
	_hud.close_window()
	await _frames(2)

	# 6. Perto da borda, a ficha continua inteira na tela.
	_hud.preview_city_tooltip(String(city["id"]), Vector2(1910.0, 1070.0),
			"political")
	await _frames(2)
	var view := _card().get_viewport_rect()
	var rect := _card().get_global_rect()
	if rect.end.x > view.size.x + 1.0 or rect.end.y > view.size.y + 1.0 \
			or rect.position.x < -1.0 or rect.position.y < -1.0:
		_failures.append("tooltip fora da tela na borda: %s" % rect)
	_finish()


func _card() -> Control:
	return _hud.get("_city_tooltip") as Control


func _tooltip_texts() -> Array[String]:
	var out: Array[String] = []
	var pending: Array[Node] = [_card()]
	while not pending.is_empty():
		var node: Node = pending.pop_back()
		if node is Label and not (node as Label).text.is_empty():
			out.append((node as Label).text)
		for child in node.get_children():
			pending.append(child)
	return out


## Cidade do resumo com porto (ou sem, quando `want_port` = false).
func _first_city_with(want_port: bool) -> Dictionary:
	for city in _summary().get("cities", []):
		var ports := WorldFacts.ports_for_city(String(city.get("id", "")))
		if want_port == ports.is_empty():
			continue
		return city
	return {}


func _city_with_deposits() -> Dictionary:
	# O status diário é leve: as jazidas vêm sob demanda em get_city_status(id).
	for city in _summary().get("cities", []):
		var sim: Object = _economy.get("sim")
		if sim == null or not sim.has_method("get_city_status"):
			break
		var heavy: Dictionary = sim.get_city_status(String(city.get("id", "")))
		if not (heavy.get("deposits", {}) as Dictionary).is_empty():
			return heavy
	return {}


func _main_resource_label(city: Dictionary) -> String:
	var best := -1.0
	var resource := ""
	for key in city.get("deposits", {}):
		var entry: Dictionary = (city.get("deposits", {}) as Dictionary)[key]
		var remaining := float(entry.get("remaining_kt", 0.0))
		if remaining > best:
			best = remaining
			resource = String(key)
	return "" if resource.is_empty() else _t("resource." + resource)


func _expect(texts: Array[String], needle: String, what: String) -> void:
	if needle.is_empty() or not texts.has(needle):
		_failures.append("tooltip sem %s ('%s'): %s"
				% [what, needle, " | ".join(texts)])


func _summary() -> Dictionary:
	return _hud.get("_summary")


func _city_name(city: Dictionary) -> String:
	return String(_i18n.call("city_name", String(city.get("id", "")),
			String(city.get("name", ""))))


func _nation_name(tag: String) -> String:
	return String(_i18n.call("nation_name", tag))


func _t(key: String) -> String:
	return String(_i18n.call("t", key))


func _frames(count: int) -> void:
	for _frame in count:
		await process_frame


func _wait_for_summary() -> bool:
	for _frame in 600:
		if not (_economy.get("last_summary") as Dictionary).is_empty():
			return true
		await process_frame
	return false


func _finish() -> void:
	if _hud != null:
		_hud.queue_free()
	if _failures.is_empty():
		print("PASS: tooltip de cidade com atraso, dados, recursos, borda e tela aberta")
		quit(0)
		return
	for failure in _failures:
		push_error("[city_tooltip] " + failure)
	quit(1)
