extends VBoxContainer

## Janela de Economia: visão por nação e por cidade, com abertura do detalhe.

signal city_requested(city_id: String)

const NATION_WEIGHTS := [2.0, 1.0, 1.1, 1.1, 1.1, 1.0, 1.0]
const NATION_ALIGN := [false, true, true, true, true, true, true]
const CITY_WEIGHTS := [1.0, 1.5, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0]
const CITY_ALIGN := [false, false, true, true, true, true, true, true]

var _summary: Dictionary = {}
var _tab := 0
var _tab_buttons: Array[Button] = []
var _summary_label: Label
var _body: VBoxContainer
var _hint: Label


func _ready() -> void:
	add_theme_constant_override("separation", 12)
	size_flags_vertical = Control.SIZE_EXPAND_FILL

	_summary_label = UiKit.label("", 13, UiKit.TEXT_DIM)
	add_child(_summary_label)

	add_child(_build_tabs())
	add_child(UiKit.rule())

	_body = VBoxContainer.new()
	_body.add_theme_constant_override("separation", 2)
	_body.size_flags_horizontal = Control.SIZE_EXPAND_FILL

	var scroll := ScrollContainer.new()
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	scroll.add_child(_body)
	add_child(scroll)

	_hint = UiKit.label("", 12, UiKit.TEXT_MUTED)
	add_child(_hint)

	_render()


func _build_tabs() -> HBoxContainer:
	var bar := UiKit.tab_bar()
	for index in 2:
		var button := UiKit.tab_button("")
		button.pressed.connect(_set_tab.bind(index))
		bar.add_child(button)
		_tab_buttons.append(button)
	bar.add_child(UiKit.spacer())
	return bar


## Atualiza os dados vindos do núcleo C++.
func refresh(summary: Dictionary) -> void:
	_summary = summary
	_retranslate()
	_render()


## Reconstrói todos os rótulos quando o idioma muda.
func _retranslate() -> void:
	if _tab_buttons.size() < 2:
		return
	_tab_buttons[0].text = I18n.t("econ.tab.nations")
	_tab_buttons[1].text = I18n.t("econ.tab.cities")
	_hint.text = I18n.t("econ.hint")


func _set_tab(index: int) -> void:
	_tab = index
	for i in _tab_buttons.size():
		UiKit.set_tab_active(_tab_buttons[i], i == _tab)
	_render()


func _render() -> void:
	if not is_node_ready() or _body == null:
		return

	for i in _tab_buttons.size():
		UiKit.set_tab_active(_tab_buttons[i], i == _tab)

	var cities: Array = _summary.get("cities", [])
	_summary_label.text = _summary_text(cities)

	UiKit.clear(_body)
	if cities.is_empty():
		return

	if _tab == 0:
		_render_nations()
	else:
		_render_cities()


func _summary_text(cities: Array) -> String:
	var deficit := 0
	var transit := 0.0
	for city in cities:
		if float(city["deficit"]) > 0.0:
			deficit += 1
		transit += float(city["in_transit"])

	return I18n.t("econ.summary", {
		"cities": UiKit.number(cities.size()),
		"deficit": UiKit.number(deficit),
		"transit": UiKit.number(transit) + " t",
	})


func _render_nations() -> void:
	_body.add_child(UiKit.table_header([
		I18n.t("econ.col.nation"),
		I18n.t("econ.col.population"),
		I18n.t("econ.col.production"),
		I18n.t("econ.col.need"),
		I18n.t("econ.col.stock"),
		I18n.t("econ.col.deficit"),
		I18n.t("econ.col.transit"),
	], NATION_WEIGHTS, NATION_ALIGN))
	_body.add_child(UiKit.rule())

	for nation in _summary.get("nations", []):
		var tag: String = nation["tag"]
		var deficit := float(nation["deficit"])

		var row := UiKit.row_button()
		row.add_child(UiKit.table_row([
			I18n.nation_name(tag),
			UiKit.number(float(nation["population"]) / 1_000_000.0, 1) + " M",
			UiKit.number(float(nation["production"])) + " t",
			UiKit.number(float(nation["need"])) + " t",
			UiKit.number(float(nation["stockpile"])) + " t",
			UiKit.number(deficit) + " t",
			UiKit.number(float(nation["in_transit"])) + " t",
		], NATION_WEIGHTS, [
			UiKit.nation_color(tag), UiKit.TEXT, UiKit.TEXT_DIM, UiKit.TEXT_DIM,
			UiKit.TEXT, UiKit.NEGATIVE if deficit > 0.0 else UiKit.POSITIVE,
			UiKit.STEEL,
		], NATION_ALIGN))

		row.pressed.connect(_request_nation_city.bind(tag))
		_body.add_child(row)


func _render_cities() -> void:
	_body.add_child(UiKit.table_header([
		I18n.t("econ.col.city"),
		I18n.t("econ.col.nation"),
		I18n.t("econ.col.population"),
		I18n.t("econ.col.production"),
		I18n.t("econ.col.need"),
		I18n.t("econ.col.stock"),
		I18n.t("econ.col.deficit"),
		I18n.t("econ.col.transit"),
	], CITY_WEIGHTS, CITY_ALIGN))
	_body.add_child(UiKit.rule())

	for city in _summary.get("cities", []):
		var deficit := float(city["deficit"])
		var row := UiKit.row_button()
		row.add_child(UiKit.table_row([
			I18n.city_name(city["id"], city["name"]),
			I18n.nation_name(city["nation"]),
			UiKit.number(float(city["population"]) / 1_000_000.0, 1) + " M",
			UiKit.number(float(city["production"])) + " t",
			UiKit.number(float(city["need"])) + " t",
			UiKit.number(float(city["stockpile"])) + " t",
			UiKit.number(deficit) + " t",
			UiKit.number(float(city["in_transit"])) + " t",
		], CITY_WEIGHTS, [
			UiKit.TEXT, UiKit.nation_color(city["nation"]), UiKit.TEXT_DIM,
			UiKit.TEXT_DIM, UiKit.TEXT, UiKit.TEXT,
			UiKit.NEGATIVE if deficit > 0.0 else UiKit.POSITIVE, UiKit.STEEL,
		], CITY_ALIGN))

		row.pressed.connect(func() -> void: city_requested.emit(city["id"]))
		_body.add_child(row)


## Uma nação pode ter várias cidades; abre a primeira delas.
func _request_nation_city(tag: String) -> void:
	for city in _summary.get("cities", []):
		if city["nation"] == tag:
			city_requested.emit(city["id"])
			return
