extends Control

## Política no template T1 (Dossiê da Papelaria).
##
## O dossiê abre na nação do jogador e, ao clicar em outra nação na lista à
## direita, passa a mostrar o dossiê dela. A composição vem de
## `data/ui/templates/dossie.json`; os blocos que dependem da nação (recursos,
## relações e a própria lista) são montados aqui com os mesmos blocos do
## design system.
##
## Dados: população, cidades, produção e comida vêm do resumo da simulação;
## território, portos, jazidas, governo, capital e PIB PPC vêm do cenário
## (`WorldFacts`). Caixa do governo e diplomacia não são simulados — a tela
## declara a ausência em vez de inventar número ou relação.

signal close_requested
signal economy_requested
signal map_requested(nation_tag: String)

const SPEC_ID := "dossie"
const PLAYER_NATION := "GBR"
const MAX_RESOURCES := 4
const PICKER_PAGE := 10

var _summary: Dictionary = {}
var _tag := PLAYER_NATION
var _query := ""

var _root: UiTemplateBuilder.TemplateRoot
var _slots := {}
var _picker: UiTemplateBuilder.DataTableView
var _search: LineEdit
var _picker_tags: Array = []
var _structure_key := ""


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
	_apply_values()


## Seleciona a nação visível (também usado pelo teste e pela captura).
func select_nation(tag: String) -> void:
	if tag.is_empty() or tag == _tag:
		return
	_tag = tag
	_rebuild()


func current_nation() -> String:
	return _tag


# --- Montagem ----------------------------------------------------------------

func _rebuild() -> void:
	_structure_key = I18n.locale
	if _root != null and is_instance_valid(_root):
		remove_child(_root)
		_root.queue_free()
	_slots.clear()
	_picker = null
	_search = null

	var spec := _patch_spec(UiTemplateBuilder.load_spec(SPEC_ID))
	_root = UiTemplateBuilder.build(spec, _data(_tag))
	_root.name = "NationDossier"
	_root.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_root.close_requested.connect(func() -> void: close_requested.emit())
	_root.choice_selected.connect(_on_choice)
	add_child(_root)
	_index_slots()
	_apply_values()
	_refresh_picker(true)


## Copia a spec T1 e troca só os trechos que dependem da nação: selo, tamanho
## do nome, deltas dos KPIs, recursos, relações e a lista de nações.
func _patch_spec(spec: Dictionary) -> Dictionary:
	var blocks: Array = spec.get("blocks", [])
	if blocks.is_empty():
		return spec
	var frame: Dictionary = blocks[0]
	frame["params"] = {"number": "$day"}
	var body: Array = frame.get("body", [])
	if body.is_empty():
		return spec
	var row: Dictionary = body[0]
	var columns: Array = row.get("blocks", [])
	if columns.size() < 2:
		return spec

	var left: Dictionary = columns[0]
	_patch_profile(left)
	_patch_kpi(columns[1])

	# Lista de nações numa terceira coluna, à direita do dossiê.
	columns.append({
		"type": "column", "gap": 12, "width": 320,
		"blocks": [_picker_card(), {"type": "spacer", "grow": true}],
	})
	row["blocks"] = columns
	return spec


func _patch_profile(left: Dictionary) -> void:
	var items: Array = left.get("blocks", [])
	for block: Variant in items:
		if typeof(block) != TYPE_DICTIONARY:
			continue
		var entry: Dictionary = block
		var kind := String(entry.get("type", ""))
		if kind == "paper_card":
			for child: Variant in entry.get("blocks", []):
				if typeof(child) != TYPE_DICTIONARY:
					continue
				var field: Dictionary = child
				var text := String(field.get("text", ""))
				if text == "$nation":
					field["size"] = _name_size(I18n.nation_name(_tag))
					field["wrap"] = true
					field["wrap_width"] = 336
				elif text == "@tpl.dossie.capital_line":
					field["wrap"] = true
					field["wrap_width"] = 336
		elif kind == "stamp":
			entry["text"] = "$stamp"
		elif kind == "button":
			# Os dois botões viram uma linha única (o dossiê ganha a coluna
			# da lista de nações e precisa economizar altura).
			var index := items.find(entry)
			items.remove_at(index)
			items.insert(index, {
				"type": "row", "gap": 10,
				"blocks": [
					{"type": "button", "text": "@tpl.dossie.open_economy",
							"variant": "primary", "id": "economy", "w": 1},
					{"type": "button", "text": "@tpl.dossie.view_map",
							"variant": "paper", "id": "map", "w": 1},
				],
			})
			# Só havia um par de botões; o segundo sai na próxima passada.
			break
	# Remove o botão que sobrou da dupla original.
	var kept: Array = []
	for block: Variant in items:
		if typeof(block) == TYPE_DICTIONARY \
				and String((block as Dictionary).get("type", "")) == "button":
			continue
		kept.append(block)
	left["blocks"] = kept


func _patch_kpi(right: Dictionary) -> void:
	var items: Array = right.get("blocks", [])
	if items.is_empty():
		return
	var grid: Dictionary = items[0]
	var cards: Array = grid.get("blocks", [])
	for card: Variant in cards:
		if typeof(card) != TYPE_DICTIONARY:
			continue
		var entry: Dictionary = card
		match String(entry.get("label", "")):
			"@tpl.dossie.kpi.population":
				entry["value_slot"] = "kpi.population.value"
				entry["delta"] = "$population_delta"
				entry["delta_slot"] = "kpi.population.delta"
				entry["tone"] = "suave"
			"@tpl.dossie.kpi.gdp":
				entry["value_slot"] = "kpi.gdp.value"
				entry["delta"] = "$gdp_delta"
				entry["delta_slot"] = "kpi.gdp.delta"
				entry["tone"] = "suave"
			"@tpl.dossie.kpi.food":
				entry["value_slot"] = "kpi.food.value"
				entry["delta"] = "$food_delta"
				entry["delta_slot"] = "kpi.food.delta"
				entry["tone"] = "alerta"
			"@tpl.dossie.kpi.cash":
				entry["value_slot"] = "kpi.cash.value"
				entry["delta"] = "$cash_delta"
				entry["delta_slot"] = "kpi.cash.delta"
				entry["tone"] = "suave"
			_:
				continue
		entry["size"] = 24

	# Recursos e relações são gerados a partir dos dados da nação.
	if items.size() < 2:
		return
	var cards_row: Dictionary = items[1]
	var cards_list: Array = cards_row.get("blocks", [])
	for card: Variant in cards_list:
		if typeof(card) != TYPE_DICTIONARY:
			continue
		var entry: Dictionary = card
		match String(entry.get("title", "")):
			"@tpl.dossie.resources":
				entry["blocks"] = _resource_blocks()
			"@tpl.dossie.relations":
				entry["blocks"] = _relation_blocks()


func _picker_card() -> Dictionary:
	return {
		"type": "paper_card", "pad": 14, "gap": 8,
		"blocks": [
			{"type": "ruled_heading", "text": "@pol.picker.title", "size": 16},
			{"type": "search_field", "slot": "picker.search",
					"placeholder": "@pol.picker.search", "width": 286},
			{
				"type": "data_table", "slot": "picker.table",
				"page_size": PICKER_PAGE, "page": 1, "sort_col": -1,
				"descending": true, "footer_left": "@pol.picker.footer",
				"columns": [{"label": "@pol.col.nation", "weight": 1,
						"align": "left"}],
				"rows": [],
			},
			{"type": "label", "text": "@pol.picker.hint", "font": "typed",
					"size": 11, "tone": "suave", "wrap": true,
					"wrap_width": 280},
		],
	}


func _resource_blocks() -> Array:
	var deposits := WorldFacts.deposits(_tag)
	var world := WorldFacts.world_reserve_kt()
	var ranked: Array = []
	for resource in deposits:
		ranked.append({"id": resource, "kt": float(deposits[resource])})
	ranked.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return float(a["kt"]) > float(b["kt"]))

	var blocks: Array = []
	for index in mini(ranked.size(), MAX_RESOURCES):
		var entry: Dictionary = ranked[index]
		var share := float(entry["kt"]) / world if world > 0.0 else 0.0
		blocks.append({
			"type": "meter_row",
			"label": "@resource.%s" % String(entry["id"]),
			"value": share,
			"text": _share_text(share),
			"tone": "tinta",
		})
	if ranked.is_empty():
		blocks.append({
			"type": "label", "text": "@tpl.dossie.resources.empty",
			"font": "typed", "size": 12, "tone": "suave", "wrap": true,
			"wrap_width": 300,
		})

	var stats := _stats(_tag)
	var food := _food_ratio(stats)
	blocks.append({
		"type": "meter_row",
		"label": "@resource.wheat",
		"value": 0.0 if food < 0.0 else minf(food, 1.0),
		"text": _percent(food),
		"tone": "positive" if food >= 0.9 else "carimbo" if food >= 0.0 \
				else "tinta",
		"meter_slot": "res.wheat.meter",
		"value_slot": "res.wheat.value",
	})
	blocks.append({"type": "spacer", "grow": true})
	blocks.append({
		"type": "label", "text": "@tpl.dossie.resources.note",
		"font": "typed", "size": 12, "tone": "suave", "wrap": true,
		"wrap_width": 300,
	})
	return blocks


func _relation_blocks() -> Array:
	return [
		{"type": "kv_row", "key": "@tpl.dossie.relations.state",
				"value": "@tpl.dossie.peace", "tone": "positive"},
		{"type": "spacer", "grow": true},
		{
			"type": "label", "text": "@tpl.dossie.relations.note",
			"font": "typed", "size": 12, "tone": "suave", "wrap": true,
			"wrap_width": 300,
		},
	]


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

	_picker = _slots.get("picker.table") as UiTemplateBuilder.DataTableView
	if _picker != null:
		_picker.row_selected.connect(_on_picker_row)

	_search = _line_edit("picker.search")
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


# --- Lista de nações ----------------------------------------------------------

func _refresh_picker(select_current: bool) -> void:
	if _picker == null:
		return
	var needle := _query.to_lower()
	var rows: Array = []
	_picker_tags.clear()
	for nation in _nation_list():
		var tag := String(nation.get("tag", ""))
		var name := I18n.nation_name(tag)
		if not needle.is_empty() and not name.to_lower().contains(needle) \
				and not tag.to_lower().contains(needle):
			continue
		rows.append([name])
		_picker_tags.append(tag)

	var index := _picker_tags.find(_tag)
	if index < 0 and not _picker_tags.is_empty():
		index = 0
	_picker.update_rows(rows, index)
	if select_current and index >= 0:
		_focus_picker_row(index)


func _focus_picker_row(index: int) -> void:
	var holder: Variant = _picker.get("_rows_holder")
	if holder is VBoxContainer:
		for child in (holder as VBoxContainer).get_children():
			var row_index: Variant = child.get("index")
			if row_index != null and int(row_index) == index \
					and child is Control:
				(child as Control).grab_focus()
				return


func _nation_list() -> Array:
	var by_tag := {}
	for city in _summary.get("cities", []):
		var tag := String(city.get("nation", ""))
		if tag.is_empty():
			continue
		var entry: Dictionary = by_tag.get(tag, {"tag": tag, "population": 0.0})
		entry["population"] = float(entry["population"]) \
				+ float(city.get("population", 0.0))
		by_tag[tag] = entry
	if by_tag.is_empty():
		for tag in WorldFacts.nations():
			by_tag[tag] = {"tag": tag,
					"population": float(WorldFacts.nation(tag).get("population", 0.0))}
	var rows: Array = by_tag.values()
	rows.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return float(a.get("population", 0.0)) > float(b.get("population", 0.0)))
	return rows


func _on_search_changed(text: String) -> void:
	_query = text.strip_edges()
	_refresh_picker(true)


func _on_picker_row(index: int) -> void:
	if index < 0 or index >= _picker_tags.size():
		return
	var tag := String(_picker_tags[index])
	if tag == _tag:
		return
	# Adia: o clique vem da tabela que o rebuild vai liberar.
	call_deferred("select_nation", tag)


func _on_choice(id: String) -> void:
	match id:
		"economy":
			economy_requested.emit()
		"map":
			map_requested.emit(_tag)


# --- Desenho -----------------------------------------------------------------

func _apply_values() -> void:
	if _root == null:
		return
	var data := _data(_tag)
	_set_text("kpi.population.value", data["population"])
	_set_text("kpi.population.delta", data["population_delta"])
	_set_text("kpi.gdp.value", data["gdp"])
	_set_text("kpi.gdp.delta", data["gdp_delta"])
	_set_text("kpi.food.value", data["food"])
	_set_text("kpi.food.delta", data["food_delta"])
	_set_text("kpi.cash.value", data["cash"])
	_set_text("kpi.cash.delta", data["cash_delta"])
	_set_text("culture.line", data["culture_line"])

	var stats := _stats(_tag)
	var food := _food_ratio(stats)
	_set_text("res.wheat.value", _percent(food))
	var meter := _slots.get("res.wheat.meter") as UiTemplateBuilder.MeterBar
	if meter != null:
		meter.ratio = 0.0 if food < 0.0 else minf(food, 1.0)
		meter.queue_redraw()


# --- Dados -------------------------------------------------------------------

func _data(tag: String) -> Dictionary:
	var meta := WorldFacts.nation(tag)
	var stats := _stats(tag)
	var territory := WorldFacts.territory(tag)
	var food := _food_ratio(stats)
	var population := float(stats.get("population", 0.0))
	if population <= 0.0:
		population = float(meta.get("population", 0.0))

	return {
		"tag": tag,
		"day": str(maxi(int(_summary.get("day", 0)), 0)),
		"nation": I18n.nation_name(tag),
		"flag": String(meta.get("flag", "")),
		"capital": _capital_text(tag, meta),
		"government": _government_text(meta),
		"cities": _count_text(int(stats.get("cities", 0))),
		"provinces": _count_text(int(territory.get("provinces", 0))),
		"ports": _count_text(int(territory.get("ports", 0))),
		"area": _area_text(float(territory.get("area_km2", 0.0))),
		"population": _population_text(population),
		"gdp": _gdp_text(float(meta.get("gdp_ppp_bn", 0.0))),
		"food": _percent(food),
		"cash": "—",
		"population_delta": I18n.t("tpl.dossie.delta.scenario"),
		"gdp_delta": I18n.t("tpl.dossie.delta.scenario"),
		"food_delta": _coverage_text(stats),
		"cash_delta": I18n.t("tpl.dossie.delta.cash"),
		"stamp": I18n.t("tpl.dossie.own") if tag == PLAYER_NATION \
				else I18n.t("tpl.dossie.foreign"),
		"culture_line": _culture_line(tag),
	}


func _stats(tag: String) -> Dictionary:
	var stats := {"cities": 0, "population": 0.0, "production": 0.0,
			"need": 0.0, "stock": 0.0, "deficit": 0.0}
	for city in _summary.get("cities", []):
		if String(city.get("nation", "")) != tag:
			continue
		stats["cities"] = int(stats["cities"]) + 1
		stats["population"] = float(stats["population"]) \
				+ float(city.get("population", 0.0))
		stats["production"] = float(stats["production"]) \
				+ float(city.get("production", 0.0))
		stats["need"] = float(stats["need"]) + float(city.get("need", 0.0))
		stats["stock"] = float(stats["stock"]) + float(city.get("stockpile", 0.0))
		stats["deficit"] = float(stats["deficit"]) + float(city.get("deficit", 0.0))
	return stats


func _food_ratio(stats: Dictionary) -> float:
	var need := float(stats.get("need", 0.0))
	if need <= 0.0:
		return -1.0
	return float(stats.get("production", 0.0)) / need


func _coverage_text(stats: Dictionary) -> String:
	var need := float(stats.get("need", 0.0))
	if need <= 0.0:
		return I18n.t("tpl.dossie.delta.no_food")
	return I18n.t("tpl.dossie.delta.food", {
		"days": UiKit.number(float(stats.get("stock", 0.0)) / need, 1),
	})


## Nome localizado da capital quando a cidade dela está no resumo; senão o
## nome do cenário, sem inventar tradução.
func _capital_text(tag: String, meta: Dictionary) -> String:
	var capital := String(meta.get("capital", ""))
	if capital.is_empty():
		return "—"
	for city in _summary.get("cities", []):
		if String(city.get("nation", "")) != tag:
			continue
		if String(city.get("name", "")).to_lower() == capital.to_lower():
			return I18n.city_name(String(city.get("id", "")), capital)
	return capital


func _government_text(meta: Dictionary) -> String:
	var value := String(meta.get("government", ""))
	if value.is_empty():
		return "—"
	var key := "tpl.dossie.gov.%s" % value
	var text := I18n.t(key)
	return text if text != key else value


func _name_size(name: String) -> int:
	if name.length() <= 18:
		return 30
	if name.length() <= 26:
		return 26
	return 22


## "Cultura principal · aceitas" — principal + aceitas publicadas pela fonte.
## Sem dado, o dossiê mostra "—" em vez de inventar composição.
func _culture_line(tag: String) -> String:
	var primary := CultureDb.primary(tag)
	var names: Array[String] = []
	for entry: Variant in CultureDb.accepted(tag):
		var name := String((entry as Dictionary).get("name", "")).strip_edges()
		if not name.is_empty():
			names.append(name)
	var head := String(primary.get("name", "")).strip_edges()
	if head.is_empty() and names.is_empty():
		return I18n.t("common.na")
	if names.is_empty():
		return head
	if head.is_empty():
		return ", ".join(PackedStringArray(names))
	return "%s · %s" % [head, ", ".join(PackedStringArray(names))]


# --- Formatação ---------------------------------------------------------------

func _population_text(value: float) -> String:
	if value <= 0.0:
		return "—"
	if value >= 1_000_000_000.0:
		return UiKit.number(value / 1_000_000_000.0, 1) + " " \
				+ I18n.t("common.billion")
	if value >= 1_000_000.0:
		return UiKit.number(value / 1_000_000.0, 1) + " " \
				+ I18n.t("common.million")
	return UiKit.number(value / 1_000.0, 1) + " " + I18n.t("common.thousand")


func _gdp_text(bn: float) -> String:
	if bn <= 0.0:
		return "—"
	if bn >= 1000.0:
		return "$ " + UiKit.number(bn / 1000.0, 1) + " " \
				+ I18n.t("common.trillion")
	return "$ " + UiKit.number(bn, 1) + " " + I18n.t("common.billion")


func _area_text(area: float) -> String:
	if area <= 0.0:
		return "—"
	return UiKit.number(area) + " km²"


func _percent(ratio: float) -> String:
	if ratio < 0.0:
		return "—"
	return UiKit.number(ratio * 100.0) + "%"


func _share_text(share: float) -> String:
	if share <= 0.0:
		return "—"
	if share >= 0.01:
		return UiKit.number(share * 100.0) + "%"
	return UiKit.number(share * 100.0, 1) + "%"


func _count_text(value: int) -> String:
	return UiKit.number(value) if value > 0 else "—"


# --- Utilidades ---------------------------------------------------------------

func _set_text(slot: String, text: String) -> void:
	var node := _slots.get(slot) as Label
	if node != null:
		node.text = text
