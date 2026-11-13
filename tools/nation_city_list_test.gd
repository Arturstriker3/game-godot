extends SceneTree

## Lista das cidades da nação do jogador junto ao minimapa (item 5): ordem por
## população, alerta de déficit, clique centraliza, duplo clique abre a ficha e
## a ficha recolhe quando uma tela abre.
##
##   godot --headless --path . --script res://tools/nation_city_list_test.gd

const HUD_PATH := "res://core/ui/hud.gd"
const NATION := "GBR"

var _failures: Array[String] = []
var _hud
var _i18n: Node
var _economy: Node
var _clicked := ""
var _list


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
	if _hud == null:
		_failures.append("HUD nao instanciou")
		_finish()
		return
	root.add_child(_hud)
	print("=== nation_city_list_test ===")
	if not await _wait_for_summary():
		_failures.append("economia nao publicou resumo antes do teste")
		_finish()
		return

	_list = _hud.get("_city_list")
	if _list == null:
		_failures.append("HUD sem lista de cidades da nacao")
		_finish()
		return
	_hud.city_focused.connect(func(city_id: String) -> void: _clicked = city_id)

	var expected := _nation_cities()
	if expected.is_empty():
		_failures.append("resumo sem cidades da nacao %s" % NATION)
		_finish()
		return
	_check_rows(expected)
	_check_alerts(expected)

	# Clique simples: centraliza (mesmo sinal do minimapa).
	var first_id := String(expected[0]["id"])
	_send_click(_list.row_at(0), false)
	await _frames(2)
	if _clicked != first_id:
		_failures.append("clique nao pediu foco em %s (veio '%s')"
				% [first_id, _clicked])

	# Duplo clique: abre a ficha T3 na cidade.
	_clicked = ""
	_send_click(_list.row_at(1), true)
	await _frames(3)
	if not _hud.call("is_window_open"):
		_failures.append("duplo clique nao abriu a ficha")
	var panel = _hud.get("_city_screen")
	if panel != null and String(panel.call("current_city_id")) != String(expected[1]["id"]):
		_failures.append("ficha abriu em %s, esperado %s"
				% [panel.call("current_city_id"), expected[1]["id"]])
	_hud.call("close_window")
	await _frames(2)

	# Recolher/expandir mantém a lista colada abaixo da topbar e acima do
	# minimapa: o topo fica fixo e a altura cresce/encolhe para baixo.
	var expanded: Rect2 = _list.get_global_rect()
	_list.call("set_collapsed", true)
	await _frames(2)
	var collapsed: Rect2 = _list.get_global_rect()
	if not _list.call("is_collapsed"):
		_failures.append("lista nao recolheu")
	if collapsed.size.y >= expanded.size.y:
		_failures.append("lista recolhida nao encolheu (%.0f >= %.0f)"
				% [collapsed.size.y, expanded.size.y])
	if not is_equal_approx(collapsed.position.y, expanded.position.y):
		_failures.append("lista recolhida perdeu a ancora superior")
	_list.call("set_collapsed", false)
	await _frames(2)

	# Tela aberta: some; fechar a tela devolve a ficha.
	_hud.call("open_screen", "statistics")
	await _frames(3)
	if _list.visible:
		_failures.append("lista visivel com uma tela aberta")
	_hud.call("close_window")
	await _frames(2)
	if not _list.visible:
		_failures.append("lista nao voltou ao fechar a tela")

	# Trocar idioma nao quebra a lista (rotulos e ordem preservados).
	_i18n.call("set_locale", "en")
	await _frames(3)
	if _list.call("row_count") != expected.size():
		_failures.append("linhas mudaram ao trocar de idioma")
	if not String(_list.get("_header").text).contains(
			String(_i18n.call("nation_name", NATION))):
		_failures.append("cabecalho sem o nome da nacao em ingles")
	_i18n.call("set_locale", "pt_BR")
	await _frames(2)

	# Nada da ficha sai da tela.
	var rect: Rect2 = _list.get_global_rect()
	var view: Rect2 = _list.get_viewport_rect()
	if rect.position.x < 0.0 or rect.position.y < 0.0 \
			or rect.end.x > view.size.x + 1.0 or rect.end.y > view.size.y + 1.0:
		_failures.append("lista fora da tela: %s" % rect)

	# O ícone de alerta tambem aparece quando o status traz deficit (a partida
	# teste pode nao ter cidade GBR em deficit no dia publicado).
	_list.call("set_rows", "teste", [{
		"id": "X", "name": "Cidade X", "population": "1", "alert": true,
	}])
	await _frames(1)
	if not _row_texts(_list.call("row_at", 0)).has("!"):
		_failures.append("linha com deficit nao desenhou o icone de alerta")
	_hud.call("_refresh_city_list")
	await _frames(1)
	_finish()


## Cidades GBR do resumo, ordenadas por populacao decrescente (o contrato da
## lista). O teste compara com a mesma ordenacao, nao com um numero fixo.
func _nation_cities() -> Array:
	var cities: Array = []
	for city in _summary().get("cities", []):
		if String(city.get("nation", "")) == NATION:
			cities.append(city)
	cities.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return float(a.get("population", 0.0)) > float(b.get("population", 0.0)))
	return cities


func _check_rows(expected: Array) -> void:
	if int(_list.call("row_count")) != expected.size():
		_failures.append("lista com %d linhas, esperado %d"
				% [int(_list.call("row_count")), expected.size()])
		return
	var previous := INF
	for index in expected.size():
		var city: Dictionary = expected[index]
		var row: Control = _list.call("row_at", index)
		if row == null:
			_failures.append("linha %d ausente" % index)
			continue
		if String(row.get("city_id")) != String(city["id"]):
			_failures.append("linha %d fora de ordem: %s != %s"
					% [index, row.get("city_id"), city["id"]])
		var texts := _row_texts(row)
		var name := String(_i18n.call("city_name", String(city["id"]),
				String(city.get("name", ""))))
		if not texts.has(name):
			_failures.append("linha %d sem o nome '%s': %s"
					% [index, name, " | ".join(texts)])
		var population := float(city.get("population", 0.0))
		if population > previous:
			_failures.append("linha %d quebrou a ordem por populacao" % index)
		previous = population
	print("lista %s: %d cidades (maior=%s)" % [NATION, expected.size(),
			String(expected[0].get("id", ""))])


func _check_alerts(expected: Array) -> void:
	var expected_alerts := 0
	for index in expected.size():
		var city: Dictionary = expected[index]
		if float(city.get("deficit", 0.0)) > 0.0:
			expected_alerts += 1
			var row: Control = _list.call("row_at", index)
			if row != null and not _row_texts(row).has("!"):
				_failures.append("linha %d com deficit sem icone de alerta"
						% index)
	print("alertas de deficit na lista: %d" % expected_alerts)


func _send_click(row: Control, double: bool) -> void:
	if row == null:
		_failures.append("linha ausente para o clique")
		return
	var event := InputEventMouseButton.new()
	event.button_index = MOUSE_BUTTON_LEFT
	event.pressed = true
	event.double_click = double
	row.call("_gui_input", event)


func _row_texts(row: Control) -> Array[String]:
	var out: Array[String] = []
	var pending: Array[Node] = [row]
	while not pending.is_empty():
		var node: Node = pending.pop_back()
		if node is Label:
			var text := (node as Label).text.strip_edges()
			if not text.is_empty():
				out.append(text)
		for child in node.get_children():
			pending.append(child)
	return out


func _summary() -> Dictionary:
	return _hud.get("_summary")


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
	if _hud != null and is_instance_valid(_hud):
		_hud.queue_free()
	if _failures.is_empty():
		print("PASS: lista de cidades da nacao ordenada, clicavel, recolhivel e i18n")
		quit(0)
		return
	for failure in _failures:
		push_error("[nation_city_list] " + failure)
	quit(1)
