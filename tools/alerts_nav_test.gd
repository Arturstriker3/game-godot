extends SceneTree

## Verifica que a tela de Alertas (T2 compacto) leva à cidade na aba certa.
##
## Monta o HUD real sobre o resumo publicado pela economia e aciona a primeira
## linha da tabela como o Enter/clique faria. O esperado é a pilha do HUD ganhar
## uma entrada `city` cuja aba é a que explica o alerta: mercado para déficit e
## estoque baixo, produção para cidade sem produção.
##
##   godot --headless --path . --script res://tools/alerts_nav_test.gd

const ALERTS_MODEL: Script = preload("res://core/ui/alerts.gd")
const HUD_PATH := "res://core/ui/hud.gd"

const TAB_BY_KIND := {
	"deficit": "market",
	"low_stock": "market",
	"no_production": "production",
}

var _failures: Array[String] = []
var _hud


func _init() -> void:
	_run.call_deferred()


func _run() -> void:
	root.size = Vector2i(1920, 1080)
	# `hud.gd` usa os autoloads (I18n, UiKit…): só compila depois que eles
	# existem, por isso o carregamento é em tempo de execução e não `preload`.
	var hud_script := load(HUD_PATH) as Script
	if hud_script == null:
		_failures.append("não carregou %s" % HUD_PATH)
		_finish()
		return
	_hud = hud_script.new()
	root.add_child(_hud)

	print("=== alerts_nav_test ===")
	if not await _wait_for_summary():
		_failures.append("economia não publicou resumo antes do teste")
		_finish()
		return

	_hud.open_screen("alerts")
	for _frame in 4:
		await process_frame

	var screen: Node = _hud.get("_screens").get("alerts")
	if screen == null:
		_failures.append("HUD sem tela de alertas")
		_finish()
		return
	if not screen.visible:
		_failures.append("tela de alertas não ficou visível após open_screen")
	if not _hud.get("_window_layer").visible:
		_failures.append("camada das telas não abriu (esperado T2)")
	if not screen.has_method("uses_window_shell") \
			or not bool(screen.call("uses_window_shell")):
		_failures.append("tela de alertas não declara moldura própria")

	var alerts: Array = ALERTS_MODEL.derive(_summary())
	if alerts.is_empty():
		print("sem alertas no dia 1: nada a navegar (estado vazio)")
		_finish()
		return
	print("alertas=%d primeiro=%s (%s)"
			% [alerts.size(), String(alerts[0]["city_id"]), String(alerts[0]["kind"])])

	for index in mini(alerts.size(), 3):
		var alert: Dictionary = alerts[index]
		var city_id := String(alert["city_id"])
		var kind := String(alert["kind"])
		var expected := String(TAB_BY_KIND.get(kind, "market"))
		_hud.open_screen("alerts")
		for _frame in 3:
			await process_frame
		screen.call("_on_row_activated", index)
		for _frame in 3:
			await process_frame

		var stack: Array = _hud.current_context()
		if stack.size() != 2:
			_failures.append("%s: pilha com %d níveis (esperado 2)"
					% [city_id, stack.size()])
			continue
		var entry: Dictionary = stack.back()
		if String(entry.get("kind", "")) != "city":
			_failures.append("%s: topo da pilha é %s"
					% [city_id, String(entry.get("kind", ""))])
			continue
		if String(entry.get("id", "")) != city_id:
			_failures.append("%s: ficha abriu a cidade %s"
					% [city_id, String(entry.get("id", ""))])
		if String(entry.get("tab", "")) != expected:
			_failures.append("%s (%s): aba %s, esperado %s"
					% [city_id, kind, String(entry.get("tab", "")), expected])
		var panel: Node = _hud.get("_city_screen")
		if panel != null and String(panel.call("current_tab")) != expected:
			_failures.append("%s: ficha mostrando a aba %s"
					% [city_id, String(panel.call("current_tab"))])
		_hud.close_window()
		for _frame in 2:
			await process_frame

	_finish()


## O primeiro tick da economia é adiado; espera o resumo existir.
func _wait_for_summary() -> bool:
	for _frame in 600:
		if not _summary().is_empty():
			return true
		await process_frame
	return false


## Autoload resolvido em tempo de execução: em `--script` o identificador
## global do autoload não existe na hora de compilar o script.
func _eco() -> Node:
	return root.get_node_or_null("Economy")


func _summary() -> Dictionary:
	var eco := _eco()
	if eco == null:
		return {}
	# A4: as telas consomem o retrato sob demanda (cidades/nações/pedidos).
	if eco.has_method("ui_snapshot"):
		return eco.call("ui_snapshot")
	return eco.get("last_summary")


func _finish() -> void:
	_hud.queue_free()
	if _failures.is_empty():
		print("PASS: linha do alerta abre a cidade na aba do dado")
		quit(0)
		return
	for failure in _failures:
		push_error("[alerts_nav] " + failure)
	quit(1)
