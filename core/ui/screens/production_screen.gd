extends Control

## Produção no template T2 (Registro da Papelaria).
##
## Duas abas de pasta: **Cadeia** (o trigo e as cidades produtoras) e
## **Cidades** (produção, consumo e cobertura de todas as cidades). A tabela, a
## busca e a ficha lateral são estado de exibição; os números vêm do resumo
## publicado pela simulação, sem cálculo econômico aqui.
##
## Clicar numa linha seleciona e alimenta a ficha; Enter ou duplo clique abre a
## ficha T3 da cidade na aba Produção (o HUD empilha a cidade sobre esta tela).

signal city_requested(city_id: String)
signal close_requested

const SPEC_ID := "producao"
const TAB_IDS := ["chain", "cities"]

var _summary: Dictionary = {}
var _tab_index := 0
var _query := ""
var _selected := {"chain": "", "cities": ""}

var _root: UiTemplateBuilder.TemplateRoot
var _slots := {}
var _tables := {}
var _tabs: UiTemplateBuilder.FolderTabs
var _search: LineEdit
var _structure_key := ""

## Linhas da última atualização, na ordem das tabelas (índice = linha).
var _chain_rows: Array = []
var _city_rows: Array = []


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


## Aceita "chain"/"cities" (captura) ou o índice da aba.
func _set_tab(id: Variant) -> void:
	var index := 1 if (str(id) == "cities" or (id is int and int(id) == 1)) else 0
	_tab_index = index
	if _tabs != null:
		_tabs.select(index)
	_refresh()


# --- Montagem ----------------------------------------------------------------

func _rebuild() -> void:
	_structure_key = I18n.locale
	if _root != null and is_instance_valid(_root):
		remove_child(_root)
		_root.queue_free()
	_slots.clear()
	_tables.clear()
	_tabs = null
	_search = null

	_root = UiTemplateBuilder.build(UiTemplateBuilder.load_spec(SPEC_ID))
	_root.name = "ProductionRegistry"
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
		_tabs.select(_tab_index)

	for index in TAB_IDS.size():
		var table := _slots.get("%s.table" % TAB_IDS[index]) \
				as UiTemplateBuilder.DataTableView
		if table == null:
			continue
		table.row_selected.connect(_on_row_selected.bind(TAB_IDS[index]))
		table.row_activated.connect(_on_row_activated.bind(TAB_IDS[index]))
		_tables[TAB_IDS[index]] = table

	_search = _line_edit("cities.search")
	if _search != null:
		_search.text = _query
		_search.text_changed.connect(_on_search_changed)


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
	_tab_index = clampi(index, 0, TAB_IDS.size() - 1)


func _on_search_changed(text: String) -> void:
	_query = text.strip_edges()
	_refresh_cities()
	var table := _tables.get("cities") as UiTemplateBuilder.DataTableView
	if table != null:
		table.reset_page()


func _on_choice(id: String) -> void:
	if id == "open_chain" or id == "open_cities":
		_open_selected(id.get_slice("_", 1))


func _on_row_selected(index: int, tab: String) -> void:
	var rows: Array = _rows_for(tab)
	if index < 0 or index >= rows.size():
		return
	_selected[tab] = String(rows[index].get("id", ""))
	_update_card(tab, index)


func _on_row_activated(index: int, tab: String) -> void:
	_on_row_selected(index, tab)
	var id := String(_selected.get(tab, ""))
	if not id.is_empty():
		city_requested.emit(id)


func _open_selected(tab: String) -> void:
	var id := String(_selected.get(tab, ""))
	if not id.is_empty():
		city_requested.emit(id)


# --- Desenho -----------------------------------------------------------------

func _refresh() -> void:
	if _root == null:
		return
	_refresh_chain()
	_refresh_cities()


func _refresh_chain() -> void:
	var table := _tables.get("chain") as UiTemplateBuilder.DataTableView
	if table == null:
		return
	_chain_rows = _producers()
	var cells: Array = []
	for city in _chain_rows:
		cells.append(_cells(city, false))
	var index := _selected_index("chain", _chain_rows)
	table.update_rows(cells, index)
	_selected["chain"] = String(_chain_rows[index].get("id", "")) if index >= 0 else ""

	_set_text("chain.count", I18n.t("tpl.common.result",
			{"n": UiKit.number(_chain_rows.size())}))
	_set_hidden("chain.table", _chain_rows.is_empty())
	_set_hidden("chain.card", _chain_rows.is_empty())
	_set_hidden("chain.empty", not _chain_rows.is_empty())
	_update_card("chain", index)


func _refresh_cities() -> void:
	var table := _tables.get("cities") as UiTemplateBuilder.DataTableView
	if table == null:
		return
	var all: Array = _summary.get("cities", [])
	_city_rows = _filtered_cities()
	var cells: Array = []
	for city in _city_rows:
		cells.append(_cells(city, true))
	var index := _selected_index("cities", _city_rows)
	table.update_rows(cells, index)
	_selected["cities"] = String(_city_rows[index].get("id", "")) if index >= 0 else ""

	_set_text("cities.count", I18n.t("tpl.common.result", {
		"n": UiKit.number(_city_rows.size()) if _query.is_empty() else \
				"%s / %s" % [UiKit.number(_city_rows.size()), UiKit.number(all.size())],
	}))
	_set_hidden("cities.table", _city_rows.is_empty())
	_set_hidden("cities.card", _city_rows.is_empty())
	_set_hidden("cities.empty", not _city_rows.is_empty())
	_update_card("cities", index)


## Células na ordem das colunas da spec (a mesma dos dois blocos).
func _cells(city: Dictionary, with_need: bool) -> Array:
	var production := float(city.get("production", 0.0))
	var stock := float(city.get("stockpile", 0.0))
	var deficit := float(city.get("deficit", 0.0))
	var consumed := float(city.get("consumed", 0.0))
	var need := float(city.get("need", 0.0))
	var coverage := INF if need <= 1e-9 else stock / need
	var cells: Array = [
		I18n.city_name(String(city.get("id", "")), String(city.get("name", ""))),
		I18n.nation_name(String(city.get("nation", ""))),
		UiKit.number(production) + " t",
	]
	if with_need:
		cells.append(UiKit.number(consumed) + " t")
	cells.append(UiKit.number(stock) + " t")
	cells.append({"text": UiKit.number(deficit) + " t",
			"tone": "carimbo" if deficit > 0.0 else "suave"})
	cells.append({"text": _coverage_text(coverage),
			"tone": _coverage_tone(coverage)})
	return cells


func _update_card(tab: String, index: int) -> void:
	var rows: Array = _rows_for(tab)
	if index < 0 or index >= rows.size():
		return
	var city: Dictionary = rows[index]
	var prefix := "%s.card" % tab
	var need := float(city.get("need", 0.0))
	var stock := float(city.get("stockpile", 0.0))
	var deficit := float(city.get("deficit", 0.0))
	var coverage := INF if need <= 1e-9 else stock / need

	_set_text(prefix + ".name", I18n.city_name(String(city.get("id", "")),
			String(city.get("name", ""))))
	_set_hidden(prefix + ".tag", deficit <= 0.0)
	_set_text(prefix + ".kv.nation", I18n.nation_name(String(city.get("nation", ""))))
	_set_text(prefix + ".kv.production",
			UiKit.number(float(city.get("production", 0.0))) + " t")
	if tab == "cities":
		_set_text(prefix + ".kv.consumed",
				UiKit.number(float(city.get("consumed", 0.0))) + " t")
	else:
		_set_text(prefix + ".kv.need", UiKit.number(need) + " t")
	_set_text(prefix + ".kv.stock", UiKit.number(stock) + " t")
	if tab == "cities":
		_set_text(prefix + ".kv.deficit", UiKit.number(deficit) + " t",
				UiKit.CARIMBO if deficit > 0.0 else UiKit.TINTA)
	_set_text(prefix + ".kv.coverage", _coverage_text(coverage),
			UiKit.coverage_color(coverage))
	_set_text(prefix + ".note", _note_text(stock, need, deficit))


## Nota da ficha: cobertura e, quando há, a demanda não atendida. Derivada dos
## campos do resumo; sem demanda, não se inventa número.
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


# --- Dados -------------------------------------------------------------------

func _rows_for(tab: String) -> Array:
	return _chain_rows if tab == "chain" else _city_rows


func _selected_index(tab: String, rows: Array) -> int:
	var wanted := String(_selected.get(tab, ""))
	for index in rows.size():
		if String(rows[index].get("id", "")) == wanted:
			return index
	return 0 if not rows.is_empty() else -1


func _producers() -> Array:
	var producers: Array = []
	for city in _sorted_cities():
		if float(city.get("production", 0.0)) > 0.0:
			producers.append(city)
	return producers


func _filtered_cities() -> Array:
	var cities := _sorted_cities()
	if _query.is_empty():
		return cities
	var needle := _query.to_lower()
	var result: Array = []
	for city in cities:
		var name := I18n.city_name(String(city.get("id", "")),
				String(city.get("name", ""))).to_lower()
		var nation := I18n.nation_name(String(city.get("nation", ""))).to_lower()
		if name.contains(needle) or nation.contains(needle):
			result.append(city)
	return result


func _sorted_cities() -> Array:
	var cities: Array = _summary.get("cities", []).duplicate()
	cities.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return float(a.get("production", 0.0)) > float(b.get("production", 0.0)))
	return cities


# --- Utilidades ---------------------------------------------------------------

func _coverage_text(days: float) -> String:
	if is_inf(days):
		return I18n.t("common.na")
	return UiKit.number(days, 1) + " " + I18n.t("common.days_short")


func _coverage_tone(days: float) -> String:
	if is_inf(days) or days >= 7.0:
		return "positive"
	if days >= 2.0:
		return "alerta"
	return "carimbo"


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
