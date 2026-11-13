extends Control

## Tela de Economia no template T2 (Registro da Papelaria).
##
## O desenho vem de `data/ui/templates/registro.json` (`UiTemplateBuilder.registro`):
## janela de papel, busca, filtros, contador, tabela ordenável/paginada e a ficha
## resumida à direita. Este script só formata o resumo publicado pela simulação —
## busca, filtro, ordenação, página e seleção são estado local de interface e
## nunca tocam na simulação.
##
## A linha selecionada ganha a tarja CARIMBO e alimenta a ficha da direita.
## Enter/duplo clique (ou o botão da ficha) empilha a cidade no HUD, que abre a
## ficha T3; o "voltar" da ficha devolve esta tela com seleção e filtros intactos.

signal city_requested(city_id: String)
signal close_requested

const FILTER_ALL := "all"
const FILTER_DEFICIT := "deficit"
const FILTER_TRANSIT := "transit"
const VIEW_NATIONS := 0
const VIEW_CITIES := 1

var _summary: Dictionary = {}
var _view := VIEW_NATIONS
var _filter := FILTER_ALL
var _query := ""

var _root: UiTemplateBuilder.TemplateRoot
var _slots := {}
var _tables := {}
var _chips := {}
var _tabs: UiTemplateBuilder.FolderTabs
var _search: LineEdit
var _nation_search: LineEdit
var _structure_key := ""

## Linhas da última atualização, na mesma ordem das tabelas (índice = linha).
var _nation_rows: Array = []
var _city_rows: Array = []
var _selected_nation := ""
var _selected_city := ""


func _ready() -> void:
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)


## A tela desenha a própria moldura de papel (template T2), sobre o mapa.
func uses_window_shell() -> bool:
	return true


# --- API ---------------------------------------------------------------------

func refresh(summary: Dictionary) -> void:
	_summary = summary
	if _root == null or _structure_key != I18n.locale:
		_rebuild()
		return
	_refresh()


func _set_tab(index: int) -> void:
	_view = VIEW_CITIES if index == VIEW_CITIES else VIEW_NATIONS
	if _tabs == null:
		_refresh()
		return
	_tabs.select(_view)


# --- Montagem ----------------------------------------------------------------

func _rebuild() -> void:
	_structure_key = I18n.locale
	if _root != null and is_instance_valid(_root):
		remove_child(_root)
		_root.queue_free()
	_slots.clear()
	_tables.clear()
	_chips.clear()
	_tabs = null
	_search = null
	_nation_search = null

	_root = UiTemplateBuilder.registro()
	_root.name = "EconomyRegistry"
	_root.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_root.close_requested.connect(func() -> void: close_requested.emit())
	_root.choice_selected.connect(_on_choice)
	add_child(_root)
	_index_slots()
	_refresh()


func _index_slots() -> void:
	var pending: Array[Node] = [_root]
	while not pending.is_empty():
		var node: Node = pending.pop_back()
		if node is Control:
			var slot := String((node as Control).get_meta("tpl_slot", ""))
			if not slot.is_empty():
				_slots[slot] = node
		for child in node.get_children():
			pending.append(child)

	_tabs = _slots.get("tabs") as UiTemplateBuilder.FolderTabs
	if _tabs != null:
		_tabs.tab_changed.connect(_on_tab_changed)
		_tabs.select(_view)
		_view = _tabs.active

	for entry in [[VIEW_NATIONS, "nations"], [VIEW_CITIES, "cities"]]:
		var table := _slots.get("%s.table" % entry[1]) \
				as UiTemplateBuilder.DataTableView
		if table == null:
			continue
		table.row_selected.connect(_on_row_selected.bind(entry[0]))
		table.row_activated.connect(_on_row_activated.bind(entry[0]))
		_tables[entry[0]] = table

	_search = _line_edit("cities.search")
	_nation_search = _line_edit("nations.search")
	for field in [_search, _nation_search]:
		if field != null:
			field.text_changed.connect(_on_search_changed)

	for id in [FILTER_ALL, FILTER_DEFICIT, FILTER_TRANSIT]:
		var chip := _slots.get("filter.%s" % id) as Button
		if chip == null:
			continue
		chip.pressed.connect(_set_filter.bind(id))
		_chips[id] = chip


func _line_edit(slot: String) -> LineEdit:
	var node := _slots.get(slot) as Node
	if node == null:
		return null
	var pending: Array[Node] = [node]
	while not pending.is_empty():
		var current: Node = pending.pop_front()
		if current is LineEdit:
			return current as LineEdit
		pending.append_array(current.get_children())
	return null


# --- Eventos -----------------------------------------------------------------

func _on_tab_changed(index: int) -> void:
	_view = index
	_refresh_nav()


func _set_filter(id: String) -> void:
	if _filter == id:
		return
	_filter = id
	_refresh()
	_reset_page(VIEW_CITIES)


func _on_search_changed(text: String) -> void:
	_query = text.strip_edges()
	for field in [_search, _nation_search]:
		if field != null and field.text != text:
			field.text = text
	_refresh()
	_reset_page(VIEW_NATIONS)
	_reset_page(VIEW_CITIES)


func _reset_page(view: int) -> void:
	var table := _tables.get(view) as UiTemplateBuilder.DataTableView
	if table != null:
		table.reset_page()


func _clear_filters() -> void:
	_filter = FILTER_ALL
	_query = ""
	for field in [_search, _nation_search]:
		if field != null:
			field.text = ""
	_refresh()
	_reset_page(VIEW_NATIONS)
	_reset_page(VIEW_CITIES)


func _on_choice(id: String) -> void:
	match id:
		"clear":
			_clear_filters()
		"open":
			_open_selected()


func _on_row_selected(index: int, view: int) -> void:
	if view == VIEW_CITIES:
		if index < 0 or index >= _city_rows.size():
			return
		_selected_city = String(_city_rows[index].get("id", ""))
		_update_city_card(index)
		return
	if index < 0 or index >= _nation_rows.size():
		return
	_selected_nation = String(_nation_rows[index].get("tag", ""))
	_update_nation_card(index)


func _on_row_activated(index: int, view: int) -> void:
	_on_row_selected(index, view)
	_open_selected()


## Só a cidade tem ficha T3; numa nação, abre a primeira cidade do registro
## (o antigo clique na linha da nação fazia o mesmo).
func _open_selected() -> void:
	if _view == VIEW_CITIES:
		if not _selected_city.is_empty():
			city_requested.emit(_selected_city)
		return
	var city_id := _first_city_of_nation(_selected_nation)
	if not city_id.is_empty():
		city_requested.emit(city_id)


func _first_city_of_nation(tag: String) -> String:
	for city in _summary.get("cities", []):
		if String(city.get("nation", "")) == tag:
			return String(city.get("id", ""))
	return ""


# --- Desenho -----------------------------------------------------------------

func _refresh() -> void:
	if _root == null:
		return
	_refresh_nav()
	_refresh_nations()
	_refresh_cities()


func _refresh_nav() -> void:
	var key := "tpl.registro.title" if _view == VIEW_CITIES \
			else "tpl.registro.title.nations"
	_set_text("nav.title", I18n.t(key))
	if _tabs != null and _tabs.active != _view:
		_tabs.select(_view)


func _refresh_nations() -> void:
	var table := _tables.get(VIEW_NATIONS) as UiTemplateBuilder.DataTableView
	if table == null:
		return
	var all: Array = _summary.get("nations", [])
	var rows := _filtered_nations(all)
	_nation_rows = rows

	var cells: Array = []
	for nation in rows:
		var deficit := float(nation.get("deficit", 0.0))
		cells.append([
			I18n.nation_name(String(nation.get("tag", ""))),
			UiKit.number(float(nation.get("population", 0.0)) / 1_000_000.0, 1) + " M",
			UiKit.number(float(nation.get("production", 0.0))) + " t",
			UiKit.number(float(nation.get("need", 0.0))) + " t",
			UiKit.number(float(nation.get("stockpile", 0.0))) + " t",
			{"text": UiKit.number(deficit) + " t",
					"tone": "carimbo" if deficit > 0.0 else "suave"},
			UiKit.number(float(nation.get("in_transit", 0.0))) + " t",
		])

	var index := _index_of(_nation_rows, "tag", _selected_nation)
	if index < 0:
		index = 0 if not rows.is_empty() else -1
	table.update_rows(cells, index)
	if index >= 0:
		_selected_nation = String(rows[index].get("tag", ""))

	_set_text("nations.count", I18n.t("econ.pill.count", {
		"shown": UiKit.number(rows.size()),
		"total": UiKit.number(all.size()),
	}))
	_update_empty(VIEW_NATIONS, rows.is_empty(), all.is_empty())
	_update_nation_card(index)


func _refresh_cities() -> void:
	var table := _tables.get(VIEW_CITIES) as UiTemplateBuilder.DataTableView
	if table == null:
		return
	var all: Array = _summary.get("cities", [])
	var rows := _filtered_cities(all)
	_city_rows = rows

	var cells: Array = []
	for city in rows:
		var deficit := float(city.get("deficit", 0.0))
		cells.append([
			I18n.city_name(String(city.get("id", "")),
					String(city.get("name", ""))),
			I18n.nation_name(String(city.get("nation", ""))),
			UiKit.number(float(city.get("population", 0.0)) / 1_000_000.0, 1) + " M",
			UiKit.number(float(city.get("production", 0.0))) + " t",
			UiKit.number(float(city.get("need", 0.0))) + " t",
			UiKit.number(float(city.get("stockpile", 0.0))) + " t",
			{"text": UiKit.number(deficit) + " t",
					"tone": "carimbo" if deficit > 0.0 else "suave"},
			UiKit.number(float(city.get("in_transit", 0.0))) + " t",
		])

	var index := _index_of(_city_rows, "id", _selected_city)
	if index < 0:
		index = 0 if not rows.is_empty() else -1
	table.update_rows(cells, index)
	if index >= 0:
		_selected_city = String(rows[index].get("id", ""))

	_update_chips()
	_set_text("cities.count", I18n.t("econ.pill.count", {
		"shown": UiKit.number(rows.size()),
		"total": UiKit.number(all.size()),
	}))
	_update_empty(VIEW_CITIES, rows.is_empty(), all.is_empty())
	_update_city_card(index)


func _update_chips() -> void:
	var counts := _city_counts()
	for id in _chips:
		var chip: Button = _chips[id]
		chip.text = I18n.t("tpl.registro.filter.%s" % id,
				{"n": UiKit.number(int(counts.get(id, 0)))})
		UiTemplateBuilder.set_chip_active(chip, id == _filter)


func _update_empty(view: int, empty: bool, no_data: bool) -> void:
	var prefix := "nations" if view == VIEW_NATIONS else "cities"
	_set_hidden("%s.table" % prefix, empty)
	_set_hidden("%s.card" % prefix, empty)
	_set_hidden("%s.empty" % prefix, not empty)
	if not empty:
		return
	if no_data:
		_set_text("%s.empty.title" % prefix,
				I18n.t("tpl.registro.empty.%s" % prefix))
		_set_text("%s.empty.body" % prefix, I18n.t("tpl.registro.empty.wait"))
	else:
		_set_text("%s.empty.title" % prefix,
				I18n.t("econ.empty.filtered") if view == VIEW_CITIES \
				else I18n.t("tpl.registro.empty.nations_filtered"))
		_set_text("%s.empty.body" % prefix,
				I18n.t("econ.empty.filtered.body") if view == VIEW_CITIES \
				else I18n.t("tpl.registro.empty.nations_body"))
	_set_hidden("%s.empty.clear" % prefix, no_data)


func _update_nation_card(index: int) -> void:
	if index < 0 or index >= _nation_rows.size():
		return
	var nation: Dictionary = _nation_rows[index]
	var deficit := float(nation.get("deficit", 0.0))
	_set_text("nations.card.name", I18n.nation_name(String(nation.get("tag", ""))))
	_set_hidden("nations.card.tag", deficit <= 0.0)
	_set_text("nations.card.kv.cities",
			UiKit.number(float(nation.get("city_count", 0.0))))
	_set_text("nations.card.kv.population",
			UiKit.number(float(nation.get("population", 0.0)) / 1_000_000.0, 1) + " M")
	_set_text("nations.card.kv.production",
			UiKit.number(float(nation.get("production", 0.0))) + " t")
	_set_text("nations.card.kv.need",
			UiKit.number(float(nation.get("need", 0.0))) + " t")
	_set_text("nations.card.kv.stock",
			UiKit.number(float(nation.get("stockpile", 0.0))) + " t")
	_set_text("nations.card.kv.deficit", UiKit.number(deficit) + " t",
			UiKit.CARIMBO if deficit > 0.0 else UiKit.TINTA)
	_set_text("nations.card.kv.transit",
			UiKit.number(float(nation.get("in_transit", 0.0))) + " t")
	_set_text("nations.card.note", _note_text(
			float(nation.get("stockpile", 0.0)),
			float(nation.get("need", 0.0)), deficit))


func _update_city_card(index: int) -> void:
	if index < 0 or index >= _city_rows.size():
		return
	var city: Dictionary = _city_rows[index]
	var deficit := float(city.get("deficit", 0.0))
	_set_text("cities.card.name", I18n.city_name(String(city.get("id", "")),
			String(city.get("name", ""))))
	_set_hidden("cities.card.tag", deficit <= 0.0)
	_set_text("cities.card.kv.nation",
			I18n.nation_name(String(city.get("nation", ""))))
	_set_text("cities.card.kv.population",
			UiKit.number(float(city.get("population", 0.0)) / 1_000_000.0, 1) + " M")
	_set_text("cities.card.kv.production",
			UiKit.number(float(city.get("production", 0.0))) + " t")
	_set_text("cities.card.kv.need",
			UiKit.number(float(city.get("need", 0.0))) + " t")
	_set_text("cities.card.kv.stock",
			UiKit.number(float(city.get("stockpile", 0.0))) + " t")
	_set_text("cities.card.kv.deficit", UiKit.number(deficit) + " t",
			UiKit.CARIMBO if deficit > 0.0 else UiKit.TINTA)
	_set_text("cities.card.kv.transit",
			UiKit.number(float(city.get("in_transit", 0.0))) + " t")
	_set_text("cities.card.note", _note_text(
			float(city.get("stockpile", 0.0)),
			float(city.get("need", 0.0)), deficit))


## Nota da ficha: cobertura do estoque e, quando há, a demanda não atendida.
## Derivada dos campos do status; sem demanda não se inventa número.
func _note_text(stock: float, need: float, deficit: float) -> String:
	if need <= 0.0:
		return I18n.t("tpl.registro.note.empty")
	var days := stock / need
	if deficit > 0.0:
		return I18n.t("tpl.registro.note.short", {
			"amount": UiKit.number(deficit) + " t",
			"days": UiKit.number(days, 1),
		})
	return I18n.t("tpl.registro.note.coverage", {"days": UiKit.number(days, 1)})


# --- Filtros -----------------------------------------------------------------

func _filtered_nations(nations: Array) -> Array:
	if _query.is_empty():
		return nations
	var needle := _query.to_lower()
	var result: Array = []
	for nation in nations:
		var name := I18n.nation_name(String(nation.get("tag", ""))).to_lower()
		if name.contains(needle) \
				or String(nation.get("tag", "")).to_lower().contains(needle):
			result.append(nation)
	return result


func _filtered_cities(cities: Array) -> Array:
	var result: Array = []
	for city in cities:
		if _filter == FILTER_DEFICIT and float(city.get("deficit", 0.0)) <= 0.0:
			continue
		if _filter == FILTER_TRANSIT \
				and float(city.get("in_transit", 0.0)) <= 0.0:
			continue
		if not _matches_query(city):
			continue
		result.append(city)
	return result


func _matches_query(city: Dictionary) -> bool:
	if _query.is_empty():
		return true
	var needle := _query.to_lower()
	var name := I18n.city_name(String(city.get("id", "")),
			String(city.get("name", ""))).to_lower()
	var nation := I18n.nation_name(String(city.get("nation", ""))).to_lower()
	return name.contains(needle) or nation.contains(needle)


## Contagem dos chips: considera a busca, não o filtro ativo.
func _city_counts() -> Dictionary:
	var counts := {FILTER_ALL: 0, FILTER_DEFICIT: 0, FILTER_TRANSIT: 0}
	for city in _summary.get("cities", []):
		if not _matches_query(city):
			continue
		counts[FILTER_ALL] = int(counts[FILTER_ALL]) + 1
		if float(city.get("deficit", 0.0)) > 0.0:
			counts[FILTER_DEFICIT] = int(counts[FILTER_DEFICIT]) + 1
		if float(city.get("in_transit", 0.0)) > 0.0:
			counts[FILTER_TRANSIT] = int(counts[FILTER_TRANSIT]) + 1
	return counts


# --- Utilidades ---------------------------------------------------------------

func _index_of(rows: Array, key: String, value: String) -> int:
	if value.is_empty():
		return -1
	for index in rows.size():
		if String(rows[index].get(key, "")) == value:
			return index
	return -1


func _set_text(slot: String, text: String,
		color: Color = Color(0, 0, 0, 0)) -> void:
	var node := _slots.get(slot) as Label
	if node == null:
		return
	node.text = text
	if color.a > 0.0:
		node.add_theme_color_override("font_color", color)


func _set_hidden(slot: String, hidden: bool) -> void:
	var node := _slots.get(slot) as CanvasItem
	if node == null:
		return
	node.visible = not hidden
