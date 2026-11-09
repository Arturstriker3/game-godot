extends Control

## Validação do painel econômico de uma cidade com o cenário demo real.
## Todos os números exibidos vêm do EconomySim; nenhum dado é inventado aqui.
##
## Argumento opcional: `--auto` avança AUTO_DAYS dias e grava um screenshot.

const SHOT_DIR := "res://docs/economy_panel"
const AUTO_DAYS := 120

var _sim: RefCounted
var _status: Dictionary = {}
var _panel: CityEconomyPanel
var _day_label: Label
var _auto := false


func _ready() -> void:
	theme = UiKit.build_theme()
	_auto = "--auto" in OS.get_cmdline_user_args()

	_sim = EconomySim.new()
	_sim.initialize_demo()
	_build_ui()

	_status = _sim.get_demo_status()
	_panel.update(_status)

	if _auto:
		await _run_auto()


func _build_ui() -> void:
	var background := ColorRect.new()
	background.color = UiKit.BG_DEEP
	background.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	background.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(background)

	var margin := MarginContainer.new()
	margin.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	for side in ["left", "top", "right", "bottom"]:
		margin.add_theme_constant_override("margin_" + side, 20)
	add_child(margin)

	var root := VBoxContainer.new()
	root.add_theme_constant_override("separation", 12)
	margin.add_child(root)

	var toolbar := HBoxContainer.new()
	toolbar.add_theme_constant_override("separation", 10)
	root.add_child(toolbar)
	toolbar.add_child(UiKit.title("WarEconomy — monitoramento econômico", 20))
	toolbar.add_child(UiKit.spacer())

	_day_label = UiKit.caption("", UiKit.TEXT_DIM)
	toolbar.add_child(_day_label)
	toolbar.add_child(_button("1 dia", _step_once))
	toolbar.add_child(_button("30 dias", _step_month))
	toolbar.add_child(_button("Reiniciar", _reset))

	root.add_child(UiKit.rule())

	_panel = CityEconomyPanel.new()
	root.add_child(_panel)


func _button(text: String, action: Callable) -> Button:
	var button := Button.new()
	button.text = text
	button.custom_minimum_size.y = 30.0
	button.pressed.connect(action)
	return button


func _step_once() -> void:
	_advance(1)


func _step_month() -> void:
	_advance(30)


func _reset() -> void:
	_sim.initialize_demo()
	_status = _sim.get_demo_status()
	_panel.update(_status)
	_day_label.text = "DIA %d" % int(_status.get("day", 0))


func _advance(days: int) -> void:
	for _day in days:
		_status = _sim.tick_demo()
		_panel.update(_status, false)
	_panel.update(_status)
	_day_label.text = "DIA %d" % int(_status.get("day", 0))


func _run_auto() -> void:
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(SHOT_DIR))
	await get_tree().process_frame

	for _day in AUTO_DAYS:
		_status = _sim.tick_demo()
		_panel.update(_status, false)
	_day_label.text = "DIA %d" % int(_status.get("day", 0))
	_panel.update(_status)

	await get_tree().process_frame
	await get_tree().process_frame
	_report()

	var image := get_viewport().get_texture().get_image()
	image.save_png("%s/panel.png" % SHOT_DIR)
	get_tree().quit()


func _report() -> void:
	print("[panel] dia=%d" % int(_status.get("day", 0)))
	for city in _status.get("cities", []):
		var markets: Array = city["markets"]
		var prices := PackedStringArray()
		var stocks := PackedStringArray()
		for market in markets:
			prices.append("%.1f" % float(market["price"]))
			stocks.append("%.1f" % float(market["stock"]))
		print("  %-6s PIB=%.1f PIBpc=%.4f renda_pc=%.2f padrão=%.2f desemprego=%.1f%%" % [
			city["id"], float(city["gdp"]), float(city["gdp_per_capita"]),
			float(city["disposable_per_capita"]), float(city["living_standard"]),
			float(city["pop"]["unemployed"]) / maxf(1.0, float(city["pop"]["labor_force"])) * 100.0])
		print("         preços=%s estoques=%s export=%.1f import=%.1f" % [
			" / ".join(prices), " / ".join(stocks),
			_sum(city.get("export_value", [])), _sum(city.get("import_value", []))])


static func _sum(values: Array) -> float:
	var total := 0.0
	for value in values:
		total += float(value)
	return total
