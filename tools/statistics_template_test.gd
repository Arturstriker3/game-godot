extends SceneTree

## Estatísticas no T5 (Quadro comparativo): ranking por indicador, grupo
## Mundo/região, posição da nação do jogador e estado vazio da tendência.
##
##   godot --headless --path . --script res://tools/statistics_template_test.gd

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

	print("=== statistics_template_test ===")
	if not await _wait_for_summary():
		_failures.append("economia não publicou resumo antes do teste")
		_finish()
		return

	_hud.open_screen("statistics")
	await _frames(5)
	var screen: Node = _hud.get("_screens").get("statistics")
	if screen == null:
		_failures.append("HUD sem tela de estatísticas")
		_finish()
		return
	if not screen.visible:
		_failures.append("estatísticas não ficou visível")
	if not bool(screen.call("uses_window_shell")):
		_failures.append("estatísticas não declara moldura própria (T5)")

	var slots: Dictionary = screen.get("_slots")
	if String(screen.call("current_indicator")) != "population":
		_failures.append("indicador inicial diferente de população")
	_check_ranking(screen, slots, "população", "")
	print("posição (mundo)=%s nota=%s" % [_text(slots, "position.rank"),
			_text(slots, "position.note")])

	screen.call("set_indicator", 1)
	await _frames(3)
	_check_ranking(screen, slots, "PIB", "$")
	screen.call("set_indicator", 3)
	await _frames(3)
	_check_ranking(screen, slots, "minerais", "t")

	screen.call("set_indicator", 0)
	screen.call("set_group", 1)
	await _frames(3)
	var rank := _text(slots, "position.rank")
	var note := _text(slots, "position.note")
	print("posição (região)=%s nota=%s" % [rank, note])
	if rank == "—" or not rank.is_valid_int() or int(rank) > 50:
		_failures.append("posição regional fora do grupo: %s" % rank)
	if note.is_empty():
		_failures.append("nota da posição vazia")

	if not _has_label(screen.get("_root"),
			_i18n("tpl.quadro.trend.empty.title")):
		_failures.append("cartão de tendência sem estado vazio")
	_finish()


## Confere que o ranking tem linhas, valores com o sufixo do indicador e a
## nação do jogador destacada.
func _check_ranking(screen: Node, slots: Dictionary, label: String,
		suffix: String) -> void:
	var rows: VBoxContainer = slots.get("ranking.rows")
	if rows == null or rows.get_child_count() < 5:
		_failures.append("ranking de %s com %d linhas" % [label,
				0 if rows == null else rows.get_child_count()])
		return
	var highlighted := false
	for child in rows.get_children():
		for label_node in _labels(child):
			if label_node.text.contains("★"):
				highlighted = true
	var values: Array = []
	for child in rows.get_children():
		var labels := _labels(child)
		if not labels.is_empty():
			values.append(labels.back().text)
	print("%s: %d linhas | %s" % [label, rows.get_child_count(),
			" / ".join(values.slice(0, 4))])
	if not highlighted:
		_failures.append("ranking de %s não destaca a nação do jogador" % label)
	var first := String(values[0]) if not values.is_empty() else ""
	if not suffix.is_empty() and not first.contains(suffix):
		_failures.append("valor de %s sem unidade %s: %s" % [label, suffix, first])
	if _text(slots, "position.rank") == "—":
		_failures.append("cartão de posição vazio em %s" % label)


func _labels(node: Node) -> Array[Label]:
	var out: Array[Label] = []
	var pending: Array[Node] = [node]
	while not pending.is_empty():
		var current: Node = pending.pop_front()
		if current is Label:
			out.append(current as Label)
		pending.append_array(current.get_children())
	return out


func _text(slots: Dictionary, key: String) -> String:
	var node := slots.get(key) as Label
	return node.text if node != null else ""


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


func _i18n(key: String) -> String:
	var node := root.get_node_or_null("I18n")
	return "" if node == null else String(node.call("t", key))


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
		print("PASS: quadro T5 com ranking, grupos, posição e tendência vazia")
		quit(0)
		return
	for failure in _failures:
		push_error("[statistics_test] " + failure)
	quit(1)
