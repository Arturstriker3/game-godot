extends VBoxContainer

## Janela de Política e Diplomacia: situação das nações, matriz de relações
## (todas em paz) e um dossiê montado apenas com dados da simulação.

const NATION_WEIGHTS := [2.0, 0.9, 1.2, 1.2]
const NATION_ALIGN := [false, true, true, false]

var _summary: Dictionary = {}
var _tab := 0
var _tab_buttons: Array[Button] = []
var _body: VBoxContainer


func _ready() -> void:
	add_theme_constant_override("separation", UiKit.SPACE_MD)
	size_flags_vertical = Control.SIZE_EXPAND_FILL

	add_child(_build_tabs())
	add_child(UiKit.rule())

	_body = VBoxContainer.new()
	_body.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_body.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_body.add_theme_constant_override("separation", 4)

	var card := UiKit.card(UiKit.SPACE_MD)
	var scroll := ScrollContainer.new()
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	scroll.add_child(card)
	card.add_child(_body)
	add_child(scroll)

	_render()


func _build_tabs() -> HBoxContainer:
	var bar := UiKit.tab_bar()
	for index in 3:
		var button := UiKit.tab_button("")
		button.pressed.connect(_set_tab.bind(index))
		bar.add_child(button)
		_tab_buttons.append(button)
	bar.add_child(UiKit.spacer())
	return bar


func refresh(summary: Dictionary) -> void:
	_summary = summary
	_retranslate()
	_render()


func _retranslate() -> void:
	if _tab_buttons.size() < 3:
		return
	_tab_buttons[0].text = I18n.t("pol.tab.nations")
	_tab_buttons[1].text = I18n.t("pol.tab.diplomacy")
	_tab_buttons[2].text = I18n.t("pol.tab.dossier")


func _set_tab(index: int) -> void:
	_tab = index
	_render()


func _render() -> void:
	if not is_node_ready() or _body == null:
		return

	for i in _tab_buttons.size():
		UiKit.set_tab_active(_tab_buttons[i], i == _tab)

	UiKit.clear(_body)
	match _tab:
		0:
			_render_nations()
		1:
			_render_diplomacy()
		_:
			_render_dossier()


func _render_nations() -> void:
	_body.add_child(UiKit.table_header([
		I18n.t("pol.col.nation"),
		I18n.t("pol.col.cities"),
		I18n.t("pol.col.population"),
		I18n.t("pol.col.status"),
	], NATION_WEIGHTS, NATION_ALIGN))
	_body.add_child(UiKit.rule())

	for nation in _summary.get("nations", []):
		var tag: String = nation["tag"]
		_body.add_child(UiKit.table_row([
			I18n.nation_name(tag),
			UiKit.number(float(nation["city_count"])),
			UiKit.number(float(nation["population"])),
			I18n.t("pol.status.peace"),
		], NATION_WEIGHTS, [
			UiKit.nation_color(tag), UiKit.TEXT_DIM, UiKit.TEXT, UiKit.POSITIVE,
		], NATION_ALIGN))


func _render_diplomacy() -> void:
	var nations: Array = _summary.get("nations", [])
	var column_count: int = nations.size() + 1

	var grid := GridContainer.new()
	grid.columns = column_count
	grid.add_theme_constant_override("h_separation", 1)
	grid.add_theme_constant_override("v_separation", 1)
	_body.add_child(grid)

	grid.add_child(_matrix_cell("", true))
	for nation in nations:
		grid.add_child(_matrix_cell(nation["tag"], true, UiKit.nation_color(nation["tag"])))

	for row_nation in nations:
		grid.add_child(_matrix_cell(I18n.nation_name(row_nation["tag"]), false,
				UiKit.nation_color(row_nation["tag"])))
		for column_nation in nations:
			if row_nation["tag"] == column_nation["tag"]:
				grid.add_child(_matrix_cell(I18n.t("common.na"), false, UiKit.TEXT_MUTED))
			else:
				grid.add_child(_matrix_cell(I18n.t("pol.status.peace"), false, UiKit.POSITIVE))

	var note := UiKit.paragraph(I18n.t("pol.relations.empty"), UiKit.FS_CAPTION + 1,
			UiKit.TEXT_MUTED)
	_body.add_child(UiKit.rule())
	_body.add_child(note)


## Célula da matriz de relações; cabeçalhos usam caixa alta pequena.
func _matrix_cell(text: String, is_header: bool, color: Color = UiKit.TEXT) -> Label:
	var cell := UiKit.caption(text, color) if is_header \
			else UiKit.label(text, UiKit.FS_SMALL, color)
	cell.custom_minimum_size = Vector2(96.0, 28.0)
	cell.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	cell.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	cell.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	return cell


func _render_dossier() -> void:
	var intro := UiKit.paragraph(I18n.t("pol.dossier.empty"), UiKit.FS_CAPTION + 1,
			UiKit.TEXT_MUTED)
	_body.add_child(intro)
	_body.add_child(UiKit.rule())

	for nation in _summary.get("nations", []):
		var deficit := float(nation["deficit"])
		var key := "pol.dossier.line" if deficit > 0.0 else "pol.dossier.healthy"
		var text := UiKit.paragraph(I18n.t(key, {
			"nation": I18n.nation_name(nation["tag"]),
			"cities": UiKit.number(float(nation["city_count"])),
			"population": UiKit.number(float(nation["population"])),
			"deficit": UiKit.number(deficit),
		}), UiKit.FS_SMALL)
		_body.add_child(text)
