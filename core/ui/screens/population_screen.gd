extends Control

## População no template T2 (Registro da Papelaria).
##
## Duas abas de pasta: **Nações** (população, cidades, empregos, renda e
## atendimento agregados por nação) e **Cidades** (os mesmos campos por cidade).
## A tabela, a busca e a ficha lateral são estado de exibição; todos os números
## vêm do resumo publicado pela simulação.
##
## O que o modelo não cobre (desemprego, salários nominais, poupança, migração)
## é declarado no cartão de nota em vez de virar número.

signal city_requested(city_id: String)
signal close_requested

const SPEC_ID := "populacao"
const TAB_IDS := ["nations", "cities"]

var _summary: Dictionary = {}
var _tab_index := 0
var _query := ""
var _selected_city := ""
var _selected_nation := ""

var _root: UiTemplateBuilder.TemplateRoot
var _slots := {}
var _tables := {}
var _tabs: UiTemplateBuilder.FolderTabs
var _search: LineEdit
var _structure_key := ""

## Cidades visíveis na tabela (busca aplicada); o índice da linha aponta aqui.
var _shown_cities: Array = []
## Nações visíveis na tabela; a linha selecionada alimenta o cartão da direita.
var _shown_nations: Array = []
var _nation_cultures: CultureMixView
var _city_cultures: CultureMixView


func _ready() -> void:
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)


func uses_window_shell() -> bool:
	return true


# --- API ---------------------------------------------------------------------

func refresh(summary: Dictionary) -> void:
	_summary = summary
	if _root == null or _structure_key != I18n.locale:
		_rebuild()
		return
	_refresh()


## Aceita o índice da aba (captura) ou "nations"/"cities".
func _set_tab(id: Variant) -> void:
	var index := 1 if (str(id) == "cities" or (id is int and int(id) == 1)) else 0
	_tab_index = index
	if _tabs != null:
		_tabs.select(index)
	_refresh()


## Seleciona a nação do cartão lateral (captura e duplo clique).
func select_nation(tag: String) -> void:
	if tag.is_empty():
		return
	_selected_nation = tag
	_refresh_nations()


func current_nation() -> String:
	return _selected_nation


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
	_nation_cultures = null
	_city_cultures = null

	_root = UiTemplateBuilder.build(UiTemplateBuilder.load_spec(SPEC_ID))
	_root.name = "PopulationRegistry"
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

	var nation_search := _line_edit("nations.search")
	if nation_search != null:
		nation_search.text = _query
		nation_search.text_changed.connect(_on_search_changed)

	_nation_cultures = _mount_cultures("nations.card.cultures",
			"NationCultures")
	_city_cultures = _mount_cultures("cities.card.cultures", "CityCultures")


## Monta a lista de culturas no envelope do template (um componente por cartão).
func _mount_cultures(slot: String, node_name: String) -> CultureMixView:
	var holder := _slots.get(slot) as Control
	if holder == null:
		return null
	var view := CultureMixView.new()
	view.name = node_name
	holder.add_child(view)
	return view


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
	for slot in ["nations.search", "cities.search"]:
		var field := _line_edit(slot)
		if field != null and field.text != text:
			field.text = text
	_refresh()
	for table in _tables.values():
		(table as UiTemplateBuilder.DataTableView).reset_page()


## Duplo clique numa nação leva às cidades dela (busca pelo nome localizado).
func _on_row_activated(index: int, tab: String) -> void:
	if tab == "nations":
		if index < 0 or index >= _shown_nations.size():
			return
		var nation: Dictionary = _shown_nations[index]
		_query = I18n.nation_name(String(nation.get("tag", "")))
		_set_tab("cities")
		_on_search_changed(_query)
		return
	_on_row_selected(index, tab)
	_open_selected()


func _on_row_selected(index: int, tab: String) -> void:
	if tab == "nations":
		if index < 0 or index >= _shown_nations.size():
			return
		_selected_nation = String(_shown_nations[index].get("tag", ""))
		_update_nation_card(_shown_nations[index])
		return
	if tab != "cities" or index < 0 or index >= _shown_cities.size():
		return
	_selected_city = String(_shown_cities[index].get("id", ""))
	_update_city_card(_shown_cities[index])


func _on_choice(id: String) -> void:
	if id == "open":
		_open_selected()


func _open_selected() -> void:
	if not _selected_city.is_empty():
		city_requested.emit(_selected_city)


# --- Desenho -----------------------------------------------------------------

func _refresh() -> void:
	if _root == null:
		return
	_refresh_totals()
	_refresh_nations()
	_refresh_cities()


func _refresh_totals() -> void:
	var totals := _totals()
	_set_text("nations.card.kv.population", _population_text(
			float(totals["population"])))
	_set_text("nations.card.kv.jobs", UiKit.number(float(totals["jobs"])))
	_set_text("nations.card.kv.income",
			UiKit.number(float(totals["income"]) * 100.0) + "%")
	_set_text("nations.card.kv.consumption",
			UiKit.number(float(totals["consumed"])) + " t")
	_set_text("nations.card.kv.attendance",
			UiKit.number(float(totals["attendance"]) * 100.0) + "%",
			_attendance_color(float(totals["attendance"])))


func _refresh_nations() -> void:
	var table := _tables.get("nations") as UiTemplateBuilder.DataTableView
	if table == null:
		return
	var all := _nation_rows()
	var rows := _filtered_nations(all)
	_shown_nations = rows
	var cells: Array = []
	for nation in rows:
		var tag := String(nation.get("tag", ""))
		cells.append([
			I18n.nation_name(tag),
			_primary_culture_cell(CultureDb.primary(tag)),
			_population_text(float(nation.get("population", 0.0))),
			UiKit.number(float(nation.get("cities", 0.0))),
			UiKit.number(float(nation.get("jobs", 0.0))),
			UiKit.number(float(nation.get("income", 0.0)) * 100.0) + "%",
			{"text": UiKit.number(float(nation.get("attendance", 0.0)) * 100.0)
					+ "%", "tone": _attendance_tone(
					float(nation.get("attendance", 0.0)))},
		])
	var index := _nation_index(rows)
	table.update_rows(cells, index)
	if index >= 0:
		_selected_nation = String(rows[index].get("tag", ""))
	_update_nation_card(rows[index] if index >= 0 else {})
	_set_text("nations.count", I18n.t("tpl.common.result",
			{"n": _count_text(rows.size(), all.size())}))
	_set_hidden("nations.table", rows.is_empty())
	_set_hidden("nations.empty", not rows.is_empty())


func _refresh_cities() -> void:
	var table := _tables.get("cities") as UiTemplateBuilder.DataTableView
	if table == null:
		return
	var all := _city_rows()
	var rows := _filtered_cities(all)
	_shown_cities = rows
	var cells: Array = []
	for city in rows:
		var attendance := _attendance(city)
		cells.append([
			I18n.city_name(String(city.get("id", "")), String(city.get("name", ""))),
			I18n.nation_name(String(city.get("nation", ""))),
			_primary_culture_cell(_city_top_culture(String(city.get("id", "")))),
			_population_text(float(city.get("population", 0.0))),
			UiKit.number(float(city.get("jobs", 0.0))),
			UiKit.number(float(city.get("income", 0.0)) * 100.0) + "%",
			UiKit.number(float(city.get("consumed", 0.0))) + " t",
			{"text": UiKit.number(attendance * 100.0) + "%",
					"tone": _attendance_tone(attendance)},
		])
	var index := _index_of(rows, _selected_city)
	table.update_rows(cells, index)
	_selected_city = String(rows[index].get("id", "")) if index >= 0 else ""

	_set_text("cities.count", I18n.t("tpl.common.result",
			{"n": _count_text(rows.size(), all.size())}))
	_set_hidden("cities.table", rows.is_empty())
	_set_hidden("cities.card", rows.is_empty())
	_set_hidden("cities.empty", not rows.is_empty())
	_update_city_card(rows[index] if index >= 0 else {})


func _update_nation_card(nation: Dictionary) -> void:
	var has_nation := not nation.is_empty()
	_set_hidden("nations.card.detail", not has_nation)
	if not has_nation:
		return
	var tag := String(nation.get("tag", ""))
	_set_text("nations.card.detail.name", I18n.nation_name(tag))
	var mix := CultureDb.nation_mix(tag)
	if _nation_cultures != null:
		_nation_cultures.show_mix(mix, I18n.t("pop.culture.empty"), true)
	var marked := false
	for item: Variant in mix:
		if String((item as Dictionary).get("role", "")) != "":
			marked = true
			break
	_set_hidden("nations.card.marks", not marked)
	_set_text("nations.card.source",
			CultureMixView.source_line(CultureDb.nation_source(tag)))


func _update_city_card(city: Dictionary) -> void:
	if city.is_empty():
		return
	var attendance := _attendance(city)
	_set_text("cities.card.name", I18n.city_name(String(city.get("id", "")),
			String(city.get("name", ""))))
	_set_text("cities.card.kv.nation", I18n.nation_name(String(city.get("nation", ""))))
	_set_text("cities.card.kv.population", _population_text(
			float(city.get("population", 0.0))))
	_set_text("cities.card.kv.jobs", UiKit.number(float(city.get("jobs", 0.0))))
	_set_text("cities.card.kv.income",
			UiKit.number(float(city.get("income", 0.0)) * 100.0) + "%")
	_set_text("cities.card.kv.consumed",
			UiKit.number(float(city.get("consumed", 0.0))) + " t")
	_set_text("cities.card.kv.attendance",
			UiKit.number(attendance * 100.0) + "%", _attendance_color(attendance))

	var city_id := String(city.get("id", ""))
	if _city_cultures != null:
		_city_cultures.show_mix(CultureDb.city_mix(city_id),
				I18n.t("pop.culture.empty"), false)
	var source := CultureDb.city_source(city_id)
	var estimate := bool(source.get("estimate", false))
	_set_hidden("cities.card.estimate", not estimate)
	var line := "" if estimate else CultureMixView.source_line(source)
	_set_text("cities.card.source", line)
	_set_hidden("cities.card.source", line.is_empty())


# --- Dados -------------------------------------------------------------------

## Proporção da demanda de trigo efetivamente consumida.
func _attendance(city: Dictionary) -> float:
	var need := float(city.get("need", 0.0))
	if need <= 0.0:
		return 1.0
	return clampf(float(city.get("consumed", 0.0)) / need, 0.0, 1.0)


func _attendance_color(ratio: float) -> Color:
	if ratio >= 0.95:
		return UiKit.POSITIVE
	if ratio >= 0.80:
		return UiKit.WARNING
	return UiKit.NEGATIVE


func _attendance_tone(ratio: float) -> String:
	if ratio >= 0.95:
		return "positive"
	if ratio >= 0.80:
		return "alerta"
	return "carimbo"


func _totals() -> Dictionary:
	var population := 0.0
	var jobs := 0.0
	var consumed := 0.0
	var need := 0.0
	var weighted_income := 0.0

	for city in _summary.get("cities", []):
		var city_population := float(city.get("population", 0.0))
		population += city_population
		jobs += float(city.get("jobs", 0.0))
		consumed += float(city.get("consumed", 0.0))
		need += float(city.get("need", 0.0))
		weighted_income += city_population * float(city.get("income", 0.0))

	return {
		"population": population,
		"jobs": jobs,
		"consumed": consumed,
		"need": need,
		"income": weighted_income / population if population > 0.0 else 0.0,
		"attendance": consumed / need if need > 0.0 else 1.0,
	}


## Agrega as cidades por nação; a ordem é por população (maior primeiro).
func _nation_rows() -> Array:
	var buckets := {}
	for city in _summary.get("cities", []):
		var tag := String(city.get("nation", ""))
		if tag.is_empty():
			continue
		if not buckets.has(tag):
			buckets[tag] = {"tag": tag, "population": 0.0, "cities": 0.0,
					"jobs": 0.0, "consumed": 0.0, "need": 0.0,
					"weighted_income": 0.0}
		var bucket: Dictionary = buckets[tag]
		var city_population := float(city.get("population", 0.0))
		bucket["population"] = float(bucket["population"]) + city_population
		bucket["cities"] = float(bucket["cities"]) + 1.0
		bucket["jobs"] = float(bucket["jobs"]) + float(city.get("jobs", 0.0))
		bucket["consumed"] = float(bucket["consumed"]) + float(city.get("consumed", 0.0))
		bucket["need"] = float(bucket["need"]) + float(city.get("need", 0.0))
		bucket["weighted_income"] = float(bucket["weighted_income"]) \
				+ city_population * float(city.get("income", 0.0))

	var rows: Array = []
	for tag in buckets:
		var bucket: Dictionary = buckets[tag]
		var population := float(bucket["population"])
		var need := float(bucket["need"])
		rows.append({
			"tag": tag,
			"population": population,
			"cities": bucket["cities"],
			"jobs": bucket["jobs"],
			"income": float(bucket["weighted_income"]) / population \
					if population > 0.0 else 0.0,
			"attendance": float(bucket["consumed"]) / need if need > 0.0 else 1.0,
		})
	rows.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return float(a["population"]) > float(b["population"]))
	return rows


func _city_rows() -> Array:
	var cities: Array = _summary.get("cities", []).duplicate()
	cities.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return float(a.get("population", 0.0)) > float(b.get("population", 0.0)))
	return cities


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


# --- Utilidades ---------------------------------------------------------------

func _population_text(value: float) -> String:
	if value >= 1_000_000_000.0:
		return UiKit.number(value / 1_000_000_000.0, 2) + " " \
				+ I18n.t("common.billion")
	if value >= 1_000_000.0:
		return UiKit.number(value / 1_000_000.0, 1) + " " \
				+ I18n.t("common.million")
	if value >= 1_000.0:
		return UiKit.number(value / 1_000.0, 1) + " " \
				+ I18n.t("common.thousand")
	return UiKit.number(value)


func _count_text(shown: int, total: int) -> String:
	if shown == total:
		return UiKit.number(shown)
	return "%s / %s" % [UiKit.number(shown), UiKit.number(total)]


func _index_of(rows: Array, city_id: String) -> int:
	if city_id.is_empty():
		return 0 if not rows.is_empty() else -1
	for index in rows.size():
		if String(rows[index].get("id", "")) == city_id:
			return index
	return 0 if not rows.is_empty() else -1


## Índice da nação selecionada; sem ela, a primeira (maior população).
func _nation_index(rows: Array) -> int:
	if _selected_nation.is_empty():
		return 0 if not rows.is_empty() else -1
	for index in rows.size():
		if String(rows[index].get("tag", "")) == _selected_nation:
			return index
	return 0 if not rows.is_empty() else -1


# --- Culturas -----------------------------------------------------------------

## Célula "Cultura principal" da tabela: nome + chip na cor da fonte.
func _primary_culture_cell(entry: Dictionary) -> Dictionary:
	if entry.is_empty():
		return {"text": I18n.t("common.na"), "tone": "suave", "size": 15}
	return {
		"text": String(entry.get("name", "")),
		"chip": String(entry.get("color", "")),
		"size": 15,
	}


## Cultura de maior participação na cidade (a fonte já manda "outras" ao fim).
func _city_top_culture(city_id: String) -> Dictionary:
	var mix := CultureDb.city_mix(city_id)
	return mix[0] if not mix.is_empty() else {}


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
