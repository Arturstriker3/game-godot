extends Control

## Painel do cenário de validação: duas cidades com população agregada (POPs),
## trigo, farinha e pão. Todos os números vêm do EconomySim em C++.

const SHOT_DIR := "res://docs/population_prototype"
const AUTO_DAYS := 30

var _sim: RefCounted
var _status: Dictionary = {}
var _day_label: Label
var _content: VBoxContainer
var _auto := false


func _ready() -> void:
	_auto = "--auto" in OS.get_cmdline_user_args()
	_sim = EconomySim.new()
	_sim.initialize_demo()
	_status = _sim.get_demo_status()
	_build_ui()
	_refresh()

	if _auto:
		DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(SHOT_DIR))
		await get_tree().process_frame
		for _day in AUTO_DAYS:
			_step()
		await get_tree().process_frame
		var image := get_viewport().get_texture().get_image()
		image.save_png("%s/panel.png" % SHOT_DIR)
		get_tree().quit()


func _build_ui() -> void:
	var background := ColorRect.new()
	background.color = UiKit.BG_DEEP
	background.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	background.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(background)

	var margin := MarginContainer.new()
	margin.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	for side in ["left", "top", "right", "bottom"]:
		margin.add_theme_constant_override("margin_" + side, 24)
	add_child(margin)

	var root := VBoxContainer.new()
	root.add_theme_constant_override("separation", 14)
	margin.add_child(root)

	var header := HBoxContainer.new()
	header.add_theme_constant_override("separation", 12)
	root.add_child(header)
	header.add_child(UiKit.title("População e economia — cenário de validação", 24))
	header.add_child(UiKit.spacer())
	_day_label = UiKit.caption("", UiKit.BRASS)
	header.add_child(_day_label)
	header.add_child(_button("Avançar dia", _step))
	header.add_child(_button("10 dias", _step_ten))
	header.add_child(_button("Choque no pão (5x)", _shock))
	header.add_child(_button("Reiniciar", _reset))

	root.add_child(UiKit.rule())

	_content = VBoxContainer.new()
	_content.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_content.add_theme_constant_override("separation", 12)
	root.add_child(_content)

	root.add_child(UiKit.caption(
		"Fonte: EconomySim (C++) — dados do cenário, sem números fictícios.", UiKit.TEXT_MUTED))


func _button(text: String, action: Callable) -> Button:
	var button := Button.new()
	button.text = text
	button.custom_minimum_size.y = 30.0
	button.pressed.connect(action)
	return button


func _step() -> void:
	_status = _sim.tick_demo()
	_refresh()


func _step_ten() -> void:
	for _index in 10:
		_status = _sim.tick_demo()
	_refresh()


func _shock() -> void:
	_sim.demo_price_shock("bread", 5.0)
	_refresh()


func _reset() -> void:
	_sim.initialize_demo()
	_status = _sim.get_demo_status()
	_refresh()


func _refresh() -> void:
	_day_label.text = "Dia %d" % int(_status.get("day", 0))
	UiKit.clear(_content)

	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 16)
	row.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_content.add_child(row)
	for city in _status.get("cities", []):
		row.add_child(_city_card(city))

	var produced: Array = _status.get("produced_total", [0.0, 0.0, 0.0])
	var consumed: Array = _status.get("consumed_total", [0.0, 0.0, 0.0])
	_content.add_child(UiKit.label(
		"Dinheiro total %.0f  |  produzido: trigo %.0f t · farinha %.0f t · pão %.0f t"
		% [float(_status.get("money_total", 0.0)),
		   float(produced[0]), float(produced[1]), float(produced[2])],
		13, UiKit.TEXT_DIM))
	_content.add_child(UiKit.label(
		"consumido: trigo %.0f t · farinha %.0f t · pão %.0f t"
		% [float(consumed[0]), float(consumed[1]), float(consumed[2])],
		13, UiKit.TEXT_DIM))


func _city_card(city: Dictionary) -> Control:
	var panel := PanelContainer.new()
	panel.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	panel.size_flags_vertical = Control.SIZE_EXPAND_FILL
	var style := UiKit.flat(UiKit.PANEL, UiKit.EDGE, 18.0)
	style.content_margin_top = 14.0
	style.content_margin_bottom = 14.0
	panel.add_theme_stylebox_override("panel", style)

	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", 8)
	panel.add_child(box)

	box.add_child(UiKit.title(String(city["name"]), 22))
	box.add_child(UiKit.caption("%s · população %s" % [
		city["id"], UiKit.number(float(city["population"]))], UiKit.TEXT_DIM))
	box.add_child(UiKit.rule())

	var pop: Dictionary = city["pop"]
	var grid := GridContainer.new()
	grid.columns = 2
	grid.add_theme_constant_override("h_separation", 18)
	grid.add_theme_constant_override("v_separation", 4)
	box.add_child(grid)

	var employed := float(pop["employed"])
	var labor_force := maxf(1.0, float(pop["labor_force"]))
	var unemployment_rate := float(pop["unemployed"]) / labor_force * 100.0
	_row(grid, "Força de trabalho", UiKit.number(labor_force))
	_row(grid, "Empregados", UiKit.number(employed))
	_row(grid, "Desempregados", "%s (%.1f%%)" % [
		UiKit.number(float(pop["unemployed"])), unemployment_rate])
	_row(grid, "Dependentes", UiKit.number(float(pop["dependents"])))
	_row(grid, "Salário médio", UiKit.number(float(pop["wage_income"]) / maxf(employed, 1.0), 2))
	_row(grid, "Renda disponível", UiKit.number(float(pop["disposable"])))
	_row(grid, "Consumo alimentar", "%.1f t/dia" % float(pop["consumed"]))
	_row(grid, "Satisfação", "%.0f%%" % (float(pop["satisfaction"]) * 100.0))
	_row(grid, "PIB", UiKit.number(float(city["gdp"])))
	_row(grid, "PIB per capita", UiKit.number(float(city["gdp_per_capita"]), 4))
	_row(grid, "Poupança das famílias", UiKit.number(float(pop["savings"])))
	_row(grid, "Lucro privado do dia", UiKit.number(float(city["service_profit"]) + _building_profit(city)))

	var prices := PackedStringArray()
	var stocks := PackedStringArray()
	for market in city["markets"]:
		prices.append("%.1f" % float(market["price"]))
		stocks.append("%.1f" % float(market["stock"]))
	_row(grid, "Preços", " / ".join(prices))
	_row(grid, "Estoques (t)", " / ".join(stocks))
	return panel


func _row(grid: GridContainer, title: String, value: String) -> void:
	grid.add_child(UiKit.caption(title, UiKit.TEXT_MUTED))
	var label := UiKit.label(value, 14, UiKit.TEXT)
	label.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
	label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	grid.add_child(label)


func _building_profit(city: Dictionary) -> float:
	var total := 0.0
	for building in city.get("buildings", []):
		total += float(building["profit"])
	return total
