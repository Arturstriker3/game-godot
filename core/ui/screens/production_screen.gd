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
const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"

static var _goods_cache: Array = []
static var _recipes_cache: Array = []
static var _needs_cache: Dictionary = {}

var _summary: Dictionary = {}
var _tab_index := 0
var _query := ""
var _selected := {"chain": "", "cities": ""}
var _selected_good := "wheat"

var _root: UiTemplateBuilder.TemplateRoot
var _slots := {}
var _tables := {}
var _tabs: UiTemplateBuilder.FolderTabs
var _search: LineEdit
var _chain_select: UiTemplateBuilder.PaperSelect
var _structure_key := ""

## Linhas da última atualização, na ordem das tabelas (índice = linha).
var _chain_rows: Array = []
var _city_rows: Array = []


func _ready() -> void:
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_ensure_chain_data()


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
	_chain_select = null

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

	_chain_select = _slots.get("chain.good") as UiTemplateBuilder.PaperSelect
	if _chain_select != null:
		_chain_select.selected.connect(_select_good)


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
	_ensure_chain_data()
	_refresh_chain_goods()
	_refresh_recipes()
	var table := _tables.get("chain") as UiTemplateBuilder.DataTableView
	if table == null:
		return
	_chain_rows = _good_producers()
	var cells: Array = []
	for row in _chain_rows:
		cells.append(_chain_cells(row))
	var index := _selected_index("chain", _chain_rows)
	table.update_rows(cells, index)
	_selected["chain"] = String(_chain_rows[index].get("id", "")) if index >= 0 else ""

	_set_text("chain.count", I18n.t("tpl.common.result",
			{"n": UiKit.number(_chain_rows.size())}))
	_set_hidden("chain.table", _chain_rows.is_empty())
	_set_hidden("chain.card", _chain_rows.is_empty())
	_set_hidden("chain.empty", not _chain_rows.is_empty())
	_update_card("chain", index)


## Seletor do bem da cadeia (113 ids de goods.json, na ordem do contrato).
func _refresh_chain_goods() -> void:
	if _chain_select == null:
		return
	if _selected_good.is_empty():
		_selected_good = "wheat"
	var items: Array = []
	for good in _goods_cache:
		items.append({
			"id": String(good.get("id", "")),
			"label": _good_name(String(good.get("id", ""))),
		})
	_chain_select.set_items(items, _selected_good)


## Receitas alternativas do bem: insumos → produto → subprodutos → consumo.
func _refresh_recipes() -> void:
	var table := _slots.get("chain.recipes") as UiTemplateBuilder.DataTableView
	if table == null:
		return
	var recipes: Array = []
	for recipe in _recipes_cache:
		if String(recipe.get("output", "")) == _selected_good:
			recipes.append(recipe)
	var cells: Array = []
	for recipe in recipes:
		cells.append([
			{
				"text": _recipe_name(recipe),
				"icon": _selected_good,
				"sector": _good_sector(_selected_good),
			},
			_goods_cell(recipe.get("inputs", [])),
			UiKit.number(float(recipe.get("yield", 1.0)), 2),
			_goods_cell(recipe.get("byproducts", [])),
			_origin_cell(_selected_good),
			_consumers_cell(_selected_good),
		])
	if cells.is_empty():
		cells.append([
			{
				"text": I18n.t("chain.recipe.extraction"),
				"icon": _selected_good,
				"sector": _good_sector(_selected_good),
			},
			{"text": I18n.t("chain.recipe.none"), "tone": "suave"},
			"1",
			{"text": I18n.t("common.na"), "tone": "suave"},
			_origin_cell(_selected_good),
			_consumers_cell(_selected_good),
		])
	table.update_rows(cells, -1)
	_set_hidden("chain.recipes", cells.is_empty())
	_set_hidden("chain.recipes.empty", not cells.is_empty())


## Célula com os ícones dos bens e o texto "Nome ×qtd · ...".
func _goods_cell(items: Array) -> Dictionary:
	var icons: Array = []
	var parts: Array = []
	for item in items:
		var id := String((item as Dictionary).get("good", ""))
		var qty := float((item as Dictionary).get("qty", 1.0))
		icons.append({"id": id, "sector": _good_sector(id)})
		if parts.size() < 3:
			parts.append("%s ×%s" % [_good_name(id), _qty_text(qty)])
	if parts.is_empty():
		return {"text": I18n.t("common.na"), "tone": "suave"}
	var text := " · ".join(parts)
	if items.size() > parts.size():
		text += " (+%d)" % (items.size() - parts.size())
	return {"text": text, "icons": icons, "size": 15}


## Produtores do bem selecionado, com a oferta/estoque/demanda do mercado.
func _good_producers() -> Array:
	var index := _good_index(_selected_good)
	if index < 0:
		return []
	var rows: Array = []
	for city in _summary.get("cities", []):
		var markets: Array = city.get("markets", [])
		if index >= markets.size():
			continue
		var market: Dictionary = markets[index]
		var supply := float(market.get("supply", 0.0))
		var stock := float(market.get("stock", 0.0))
		if supply <= 0.0 and stock <= 0.0:
			continue
		rows.append({
			"id": String(city.get("id", "")),
			"name": String(city.get("name", "")),
			"nation": String(city.get("nation", "")),
			"supply": supply,
			"stock": stock,
			"demand": float(market.get("desired", 0.0)),
		})
	rows.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return float(a.get("supply", 0.0)) > float(b.get("supply", 0.0)))
	return rows


func _chain_cells(row: Dictionary) -> Array:
	var demand := float(row.get("demand", 0.0))
	var stock := float(row.get("stock", 0.0))
	var coverage := INF if demand <= 1e-9 else stock / demand
	return [
		I18n.city_name(String(row.get("id", "")), String(row.get("name", ""))),
		I18n.nation_name(String(row.get("nation", ""))),
		UiKit.number(float(row.get("supply", 0.0))) + " t",
		UiKit.number(stock) + " t",
		{"text": UiKit.number(demand) + " t",
				"tone": "carimbo" if demand > 0.0 and stock <= 0.0 else "suave"},
		{"text": _coverage_text(coverage), "tone": _coverage_tone(coverage)},
	]


func _select_good(id: String) -> void:
	if id.is_empty() or id == _selected_good:
		return
	_selected_good = id
	_refresh_chain()
	var table := _tables.get("chain") as UiTemplateBuilder.DataTableView
	if table != null:
		table.reset_page()


func _good_index(id: String) -> int:
	for index in _goods_cache.size():
		if String(_goods_cache[index].get("id", "")) == id:
			return index
	return -1


func _good_name(id: String) -> String:
	var key := "product." + id
	var translated := I18n.t(key)
	if translated != key:
		return translated
	for good in _goods_cache:
		if String(good.get("id", "")) == id:
			return String(good.get("name", id))
	return id


func _good_sector(id: String) -> String:
	for good in _goods_cache:
		if String(good.get("id", "")) == id:
			return String(good.get("sector", ""))
	return ""


func _recipe_name(recipe: Dictionary) -> String:
	var id := String(recipe.get("id", ""))
	var key := "chain.recipe." + id
	var translated := I18n.t(key)
	return translated if translated != key else id


## Célula "onde é produzido": cidades com oferta do bem, com o tipo de origem
## (jazida quando a cidade tem reserva, fazenda quando tem aptidão agrícola,
## fábrica quando a oferta vem de receita). Nada é inventado: só entra cidade
## que publicou oferta > 0 no mercado do bem.
func _origin_cell(good_id: String) -> Dictionary:
	var sites := _origin_sites(good_id)
	if sites.is_empty():
		return {"text": I18n.t("chain.origin.none"), "tone": "suave"}
	var first: Dictionary = sites[0]
	var names: Array = []
	for index in mini(sites.size(), 2):
		names.append(I18n.city_name(String(sites[index].get("id", "")),
				String(sites[index].get("name", ""))))
	var text := "%s · %s" % [
		I18n.t("chain.origin.%s" % String(first.get("kind", "factory"))),
		", ".join(names),
	]
	if sites.size() > names.size():
		text += " +%d" % (sites.size() - names.size())
	return {"text": text, "size": 15}


func _origin_sites(good_id: String) -> Array:
	var index := _good_index(good_id)
	if index < 0:
		return []
	var sites: Array = []
	for city in _summary.get("cities", []):
		var markets: Array = city.get("markets", [])
		if index >= markets.size():
			continue
		var supply := float((markets[index] as Dictionary).get("supply", 0.0))
		if supply <= 0.0:
			continue
		var kind := "factory"
		var deposits: Dictionary = city.get("deposits", {})
		if deposits.has(good_id) \
				and float((deposits[good_id] as Dictionary).get("remaining_kt", 0.0)) > 0.0:
			kind = "deposit"
		elif _at(city.get("suitability", []), index) > 0.0:
			kind = "farm"
		sites.append({
			"id": String(city.get("id", "")),
			"name": String(city.get("name", "")),
			"kind": kind,
			"supply": supply,
		})
	sites.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return float(a.get("supply", 0.0)) > float(b.get("supply", 0.0)))
	return sites


## Célula "quem consome": texto curto + ícones dos bens que usam este como
## insumo (o que a cadeia produz a partir dele).
func _consumers_cell(good_id: String) -> Dictionary:
	var cell := {"text": _consumers(good_id), "tone": "suave", "size": 15}
	var icons := _consumer_icons(good_id)
	if not icons.is_empty():
		cell["icons"] = icons
	return cell


func _consumer_icons(good_id: String) -> Array:
	var icons: Array = []
	var seen: Dictionary = {}
	for recipe in _recipes_cache:
		var uses := false
		for item in recipe.get("inputs", []):
			if String((item as Dictionary).get("good", "")) == good_id:
				uses = true
				break
		if not uses:
			continue
		var output := String(recipe.get("output", ""))
		if output.is_empty() or seen.has(output):
			continue
		seen[output] = true
		icons.append({"id": output, "sector": _good_sector(output)})
		if icons.size() >= 4:
			break
	return icons


## Quem consome o bem: receitas que o usam como insumo, categorias das POPs
## que o aceitam como substituto e consumo institucional declarado.
func _consumers(good_id: String) -> String:
	var found: Array = []
	for recipe in _recipes_cache:
		var inputs: Array = recipe.get("inputs", [])
		var uses := false
		for item in inputs:
			if String((item as Dictionary).get("good", "")) == good_id:
				uses = true
				break
		if uses:
			var output := String(recipe.get("output", ""))
			var label := _good_name(output)
			if not found.has(label):
				found.append(label)
	for level_id in ["basic", "life", "luxury"]:
		var level: Dictionary = _needs_cache.get("levels", {}).get(level_id, {})
		for category_id in level.get("categories", {}):
			var category: Dictionary = level["categories"][category_id]
			var substitutes: Array = category.get("substitutes", [])
			if substitutes.has(good_id):
				var label := "%s · %s" % [I18n.t("chain.need.%s" % category_id),
						I18n.t("chain.level.%s" % level_id)]
				if not found.has(label):
					found.append(label)
	var institutional: Dictionary = _needs_cache.get("institutional", {})
	for key in institutional:
		if (institutional[key] as Array).has(good_id):
			var label := I18n.t("chain.institutional.%s" % key)
			if label == "chain.institutional.%s" % key:
				label = String(key)
			if not found.has(label):
				found.append(label)
	if found.is_empty():
		return I18n.t("chain.consumers.none")
	var shown: Array = found.slice(0, 2)
	var extra := found.size() - shown.size()
	var text := ", ".join(shown)
	return text + (" (+%d)" % extra if extra > 0 else "")


static func _qty_text(value: float) -> String:
	return UiKit.number(value, 1) if not is_equal_approx(value, roundf(value)) \
			else UiKit.number(value)


static func _at(values: Array, index: int) -> float:
	return float(values[index]) if index >= 0 and index < values.size() else 0.0


static func _ensure_chain_data() -> void:
	if _goods_cache.is_empty():
		var goods := _load_array(GOODS_PATH)
		_goods_cache = goods
	if _recipes_cache.is_empty():
		_recipes_cache = _load_array(RECIPES_PATH)
	if _needs_cache.is_empty():
		var file := FileAccess.open(NEEDS_PATH, FileAccess.READ)
		if file != null:
			var data: Variant = JSON.parse_string(file.get_as_text())
			file.close()
			if typeof(data) == TYPE_DICTIONARY:
				_needs_cache = data


static func _load_array(path: String) -> Array:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return []
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data if typeof(data) == TYPE_ARRAY else []


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
	var row: Dictionary = rows[index]
	var prefix := "%s.card" % tab
	if tab == "chain":
		var demand := float(row.get("demand", 0.0))
		var stock := float(row.get("stock", 0.0))
		var coverage := INF if demand <= 1e-9 else stock / demand
		_set_text(prefix + ".name", I18n.city_name(String(row.get("id", "")),
				String(row.get("name", ""))))
		_set_hidden(prefix + ".tag", stock > 0.0)
		_set_text(prefix + ".kv.nation",
				I18n.nation_name(String(row.get("nation", ""))))
		_set_text(prefix + ".kv.production",
				UiKit.number(float(row.get("supply", 0.0))) + " t")
		_set_text(prefix + ".kv.need", UiKit.number(demand) + " t")
		_set_text(prefix + ".kv.stock", UiKit.number(stock) + " t")
		_set_text(prefix + ".kv.coverage", _coverage_text(coverage),
				UiKit.coverage_color(coverage))
		_set_text(prefix + ".note", _note_text(stock, demand, 0.0))
		return
	var city := row
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
