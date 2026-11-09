extends VBoxContainer

## Janela de Economia: visão por nação e por cidade, com abertura do detalhe.
##
## A tela só formata os dados que recebe em `refresh()`; busca e filtros são
## estado local de interface e nunca tocam na simulação.

signal city_requested(city_id: String)

const NATION_WEIGHTS := [2.0, 1.0, 1.1, 1.1, 1.1, 1.0, 1.0]
const NATION_ALIGN := [false, true, true, true, true, true, true]
const CITY_WEIGHTS := [1.0, 1.5, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0]
const CITY_ALIGN := [false, false, true, true, true, true, true, true]

const FILTER_ALL := "all"
const FILTER_DEFICIT := "deficit"
const FILTER_TRANSIT := "transit"

var _summary: Dictionary = {}
var _tab := 0
var _filter := FILTER_ALL
var _query := ""

var _tab_buttons: Array[Button] = []
var _filter_chips := {}
var _filter_row: HBoxContainer
var _search: LineEdit
var _body: VBoxContainer
var _count_pill: PanelContainer


func _ready() -> void:
	add_theme_constant_override("separation", UiKit.SPACE_MD)
	size_flags_vertical = Control.SIZE_EXPAND_FILL

	add_child(_build_header())
	add_child(UiKit.rule())
	add_child(_build_scroll())

	_render()


func _build_header() -> VBoxContainer:
	var box := VBoxContainer.new()
	box.add_theme_constant_override("separation", UiKit.SPACE_SM)

	var tabs := UiKit.tab_bar()
	for index in 2:
		var button := UiKit.tab_button("")
		button.pressed.connect(_set_tab.bind(index))
		tabs.add_child(button)
		_tab_buttons.append(button)
	tabs.add_child(UiKit.spacer())

	_search = UiKit.search_field("")
	_search.size_flags_vertical = Control.SIZE_SHRINK_CENTER
	_search.text_changed.connect(_on_search_changed)
	tabs.add_child(_search)
	box.add_child(tabs)

	box.add_child(_build_filters())
	return box


func _build_filters() -> HBoxContainer:
	_filter_row = UiKit.toolbar()
	for id in [FILTER_ALL, FILTER_DEFICIT, FILTER_TRANSIT]:
		var chip := UiKit.filter_chip("")
		chip.pressed.connect(_set_filter.bind(id))
		_filter_row.add_child(chip)
		_filter_chips[id] = chip

	_filter_row.add_child(UiKit.spacer())

	_count_pill = UiKit.status_pill("", "steel")
	_filter_row.add_child(_count_pill)
	return _filter_row


func _build_scroll() -> ScrollContainer:
	var scroll := ScrollContainer.new()
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	UiKit.bind_scroll_sfx(scroll)

	var card := UiKit.card(UiKit.SPACE_MD)
	card.size_flags_vertical = Control.SIZE_EXPAND_FILL
	scroll.add_child(card)

	_body = VBoxContainer.new()
	_body.add_theme_constant_override("separation", 2)
	_body.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	card.add_child(_body)

	return scroll


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
	_search.placeholder_text = I18n.t("econ.search")
	_filter_chips[FILTER_ALL].text = I18n.t("econ.filter.all")
	_filter_chips[FILTER_DEFICIT].text = I18n.t("econ.filter.deficit")
	_filter_chips[FILTER_TRANSIT].text = I18n.t("econ.filter.transit")


func _set_tab(index: int) -> void:
	_tab = index
	_render()


func _set_filter(id: String) -> void:
	_filter = id
	_render()


func _on_search_changed(text: String) -> void:
	_query = text.strip_edges()
	_render()


func _render() -> void:
	if not is_node_ready() or _body == null:
		return

	for i in _tab_buttons.size():
		UiKit.set_tab_active(_tab_buttons[i], i == _tab)

	# Os filtros de déficit/trânsito só fazem sentido na visão por cidade.
	var chips_visible := _tab == 1
	for id in _filter_chips:
		_filter_chips[id].visible = chips_visible
		UiKit.set_chip_active(_filter_chips[id], id == _filter)

	UiKit.clear(_body)

	if _tab == 0:
		_render_nations()
		_update_results(_summary.get("nations", []).size(), _filtered_nations().size())
	else:
		_render_cities()
		_update_results(_summary.get("cities", []).size(), _filtered_cities().size())


func _update_results(total: int, shown: int) -> void:
	var text := I18n.t("econ.pill.count", {
		"shown": UiKit.number(shown),
		"total": UiKit.number(total),
	})
	var label := _count_pill.get_child(0) as Label
	label.text = text
	_count_pill.tooltip_text = text


func _filtered_nations() -> Array:
	var nations: Array = _summary.get("nations", [])
	if _query.is_empty():
		return nations

	var needle := _query.to_lower()
	var result: Array = []
	for nation in nations:
		var name := I18n.nation_name(nation["tag"]).to_lower()
		if name.contains(needle) or String(nation["tag"]).to_lower().contains(needle):
			result.append(nation)
	return result


func _filtered_cities() -> Array:
	var cities: Array = _summary.get("cities", [])
	var needle := _query.to_lower()
	var result: Array = []

	for city in cities:
		if _filter == FILTER_DEFICIT and float(city["deficit"]) <= 0.0:
			continue
		if _filter == FILTER_TRANSIT and float(city["in_transit"]) <= 0.0:
			continue
		if not needle.is_empty():
			var name := I18n.city_name(city["id"], city["name"]).to_lower()
			var nation := I18n.nation_name(city["nation"]).to_lower()
			if not (name.contains(needle) or nation.contains(needle)):
				continue
		result.append(city)

	return result


func _render_nations() -> void:
	var nations := _filtered_nations()
	if nations.is_empty():
		_body.add_child(_empty_result())
		return

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

	for nation in nations:
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
	var cities := _filtered_cities()
	if cities.is_empty():
		_body.add_child(_empty_result())
		return

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

	for city in cities:
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


## Estado vazio de busca/filtro: não confundir com "sem dados da simulação".
func _empty_result() -> Control:
	return UiKit.empty_state(
			I18n.t("econ.empty.filtered"),
			I18n.t("econ.empty.filtered.body"),
			I18n.t("econ.filter.clear"),
			_clear_filters)


func _clear_filters() -> void:
	_query = ""
	_filter = FILTER_ALL
	_search.text = ""
	_render()


## Uma nação pode ter várias cidades; abre a primeira delas.
func _request_nation_city(tag: String) -> void:
	for city in _summary.get("cities", []):
		if city["nation"] == tag:
			city_requested.emit(city["id"])
			return
