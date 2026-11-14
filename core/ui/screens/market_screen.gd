extends Control

## Mercado por nação e cidade no template T2 (Registro da Papelaria).
##
## Aba **Mercado**: lista de cidades à esquerda e a tabela dos 114 bens da
## cidade selecionada à direita (ícone, preço na moeda local, estoque, oferta,
## demanda, importação e exportação). Aba **Ofertas**: a ficha do bem escolhido
## com o preço local, o preço de importação e as melhores ofertas do dia
## (origem + frete + tarifa → entregue na moeda do comprador).
##
## A tela só formata o status publicado pela simulação. Busca, filtro de setor,
## aba e seleção são estado de exibição; nada é calculado como regra econômica
## e nenhum número é inventado quando o dia não publicou comércio do bem.

signal city_requested(city_id: String)
signal close_requested

const SPEC_ID := "mercado"
const GOODS_PATH := "res://data/definitions/economy/goods.json"
const TAB_MARKET := 0
const TAB_OFFERS := 1
const ALL := "all"
const SECTORS := ["food", "agro", "mining", "energy", "metal", "chemical",
		"machinery", "construction", "wood", "packaging", "textile",
		"transport", "electronics", "military", "luxury"]

static var _goods_cache: Array = []

var _summary: Dictionary = {}
var _tab := TAB_MARKET
var _city_query := ""
var _good_query := ""
var _sector := ALL
var _selected_city := ""
var _selected_good := ""

var _root: UiTemplateBuilder.TemplateRoot
var _slots := {}
var _tabs: UiTemplateBuilder.FolderTabs
var _city_table: UiTemplateBuilder.DataTableView
var _goods_table: UiTemplateBuilder.DataTableView
var _offers_table: UiTemplateBuilder.DataTableView
var _city_search: LineEdit
var _goods_search: LineEdit
var _chips := {}
var _structure_key := ""

## Linhas visíveis (a linha da tabela indexa estas listas).
var _city_rows: Array = []
var _good_rows: Array = []


func _ready() -> void:
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_ensure_goods()


func uses_window_shell() -> bool:
	return true


# --- API ---------------------------------------------------------------------

func refresh(summary: Dictionary) -> void:
	_summary = summary
	if _root == null or _structure_key != I18n.locale:
		_rebuild()
		return
	_refresh()


## Captura/testes: 0/"market" ou 1/"offers".
func _set_tab(id: Variant) -> void:
	var index := TAB_OFFERS if (str(id) == "offers" \
			or (id is int and int(id) == TAB_OFFERS)) else TAB_MARKET
	_tab = index
	if _tabs != null:
		_tabs.select(index)
	_refresh()


func current_city() -> String:
	return _selected_city


func current_good() -> String:
	return _selected_good


## Seleciona um bem pelo id (captura/testes) e mantém a tabela coerente.
func select_good(id: String) -> void:
	if id.is_empty():
		return
	_selected_good = id
	_refresh_goods()
	var index := _selected_index(_good_rows, id)
	if index >= 0 and _goods_table != null:
		_goods_table.set_selected(index)


## Seleciona uma cidade pelo id (captura/testes): tabela, nação e detalhe.
func select_city(id: String) -> void:
	if id.is_empty():
		return
	_selected_city = id
	_refresh_cities()
	_refresh_nation()
	_refresh_goods()
	_refresh_detail()
	var index := _selected_index(_city_rows, id)
	if index >= 0 and _city_table != null:
		_city_table.set_selected(index)


# --- Montagem ----------------------------------------------------------------

func _rebuild() -> void:
	_structure_key = I18n.locale
	if _root != null and is_instance_valid(_root):
		remove_child(_root)
		_root.queue_free()
	_slots.clear()
	_tabs = null
	_city_table = null
	_goods_table = null
	_offers_table = null
	_city_search = null
	_goods_search = null
	_chips.clear()

	_root = UiTemplateBuilder.build(UiTemplateBuilder.load_spec(SPEC_ID))
	_root.name = "MarketRegistry"
	_root.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	_root.close_requested.connect(func() -> void: close_requested.emit())
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
		_tabs.select(_tab)
		_tab = _tabs.active

	_city_table = _slots.get("market.cities") as UiTemplateBuilder.DataTableView
	if _city_table != null:
		_city_table.row_selected.connect(_on_city_selected)
		_city_table.row_activated.connect(_on_city_activated)
	_goods_table = _slots.get("market.goods") as UiTemplateBuilder.DataTableView
	if _goods_table != null:
		_goods_table.row_selected.connect(_on_good_selected)
		_goods_table.row_activated.connect(_on_good_activated)
	_offers_table = _slots.get("market.offers") as UiTemplateBuilder.DataTableView

	_city_search = _line_edit("market.city_search")
	if _city_search != null:
		_city_search.text_changed.connect(_on_city_search)
	_goods_search = _line_edit("market.goods_search")
	if _goods_search != null:
		_goods_search.text_changed.connect(_on_good_search)

	for id in [ALL] + SECTORS:
		var chip := _slots.get("market.sector.%s" % id) as Button
		if chip == null:
			continue
		chip.pressed.connect(_set_sector.bind(id))
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
	_tab = clampi(index, TAB_MARKET, TAB_OFFERS)


func _on_city_search(text: String) -> void:
	_city_query = text.strip_edges()
	_refresh_cities()
	if _city_table != null:
		_city_table.reset_page()


func _on_good_search(text: String) -> void:
	_good_query = text.strip_edges()
	_refresh_goods()
	if _goods_table != null:
		_goods_table.reset_page()


func _set_sector(id: String) -> void:
	if _sector == id:
		return
	_sector = id
	_refresh_goods()
	if _goods_table != null:
		_goods_table.reset_page()


func _on_city_selected(index: int) -> void:
	if index < 0 or index >= _city_rows.size():
		return
	_selected_city = String(_city_rows[index].get("id", ""))
	_refresh_goods()
	_refresh_nation()
	_refresh_detail()
	var current := _selected_index(_good_rows, _selected_good)
	if current >= 0 and _goods_table != null:
		_goods_table.set_selected(current)


func _on_city_activated(index: int) -> void:
	_on_city_selected(index)
	if not _selected_city.is_empty():
		city_requested.emit(_selected_city)


func _on_good_selected(index: int) -> void:
	if index < 0 or index >= _good_rows.size():
		return
	_selected_good = String(_good_rows[index].get("id", ""))
	_refresh_detail()


## Duplo clique no bem abre a aba de ofertas (o detalhe do preço entregue).
func _on_good_activated(index: int) -> void:
	_on_good_selected(index)
	_set_tab(TAB_OFFERS)


# --- Desenho -----------------------------------------------------------------

func _refresh() -> void:
	if _root == null:
		return
	_set_text("nav.title", I18n.t("market.title"))
	_refresh_cities()
	_refresh_nation()
	_refresh_goods()
	_refresh_detail()


func _refresh_cities() -> void:
	if _city_table == null:
		return
	var all: Array = _summary.get("cities", [])
	var rows := _filtered_cities(all)
	rows.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return float(a.get("population", 0.0)) > float(b.get("population", 0.0)))
	_city_rows = rows
	var cells: Array = []
	for city in rows:
		cells.append([
			I18n.city_name(String(city.get("id", "")),
					String(city.get("name", ""))),
			I18n.nation_name(String(city.get("nation", ""))),
			UiKit.number(float(city.get("population", 0.0)) / 1_000_000.0, 1)
					+ " M",
		])
	var index := _selected_index(rows, _selected_city)
	if index < 0 and not rows.is_empty():
		index = 0
	_city_table.update_rows(cells, index)
	if index >= 0:
		_selected_city = String(rows[index].get("id", ""))
	_set_text("market.cities.count", I18n.t("econ.pill.count", {
		"shown": UiKit.number(rows.size()),
		"total": UiKit.number(all.size()),
	}))


func _refresh_nation() -> void:
	var city := _city()
	if city.is_empty():
		_set_text("market.nation.name", I18n.t("market.nation.none"))
		return
	var tag := String(city.get("nation", ""))
	var cities := 0
	var production := 0.0
	var demand := 0.0
	var stock := 0.0
	for other in _summary.get("cities", []):
		if String(other.get("nation", "")) != tag:
			continue
		cities += 1
		production += float(other.get("production", 0.0))
		demand += float(other.get("need", 0.0))
		stock += float(other.get("stockpile", 0.0))
	_set_text("market.nation.name", I18n.nation_name(tag))
	_set_text("market.nation.cities", UiKit.number(cities))
	_set_text("market.nation.production", _tons(production) + "/d")
	_set_text("market.nation.demand", _tons(demand) + "/d")
	_set_text("market.nation.stock", _tons(stock))


func _refresh_goods() -> void:
	if _goods_table == null:
		return
	for id in _chips:
		UiTemplateBuilder.set_chip_active(_chips[id], id == _sector)
	var city := _city()
	var products: Array = _summary.get("products", [])
	var markets: Array = city.get("markets", [])
	var imported: Array = city.get("imported", [])
	var exported: Array = city.get("exported", [])
	var currency := String(city.get("currency", ""))

	var rows: Array = []
	var cells: Array = []
	var indices: Array[int] = []
	for index in products.size():
		var good := _good(String(products[index]))
		if good.is_empty():
			continue
		if _sector != ALL and String(good.get("sector", "")) != _sector:
			continue
		if not _matches_good(good):
			continue
		indices.append(index)
	indices.sort_custom(func(a: int, b: int) -> bool:
		return _good_name(_good(String(products[a]))).naturalnocasecmp_to(
				_good_name(_good(String(products[b])))) < 0)
	for index in indices:
		var good := _good(String(products[index]))
		var market: Dictionary = markets[index] if index < markets.size() else {}
		var price := float(market.get("price_local", 0.0))
		var stock := float(market.get("stock", 0.0))
		var supply := float(market.get("supply", 0.0))
		var demand := float(market.get("desired", 0.0))
		var id := String(good.get("id", ""))
		rows.append(good)
		cells.append([
			{
				"text": _good_name(good) + " · " + _sector_name(good),
				"icon": id,
				"icon_color": String(good.get("icon_color", "")),
			},
			{"text": _money(price, currency),
					"tone": "suave" if price <= 0.0 else "tinta"},
			UiKit.number(stock) + " t",
			{"text": UiKit.number(supply) + " t",
					"tone": "suave" if supply <= 0.0 else "tinta"},
			{"text": UiKit.number(demand) + " t",
					"tone": "suave" if demand <= 0.0 else "tinta"},
			{"text": UiKit.number(_at(imported, index)) + " t", "tone": "suave"},
			{"text": UiKit.number(_at(exported, index)) + " t", "tone": "suave"},
		])
	_good_rows = rows

	var selected := _selected_index(rows, _selected_good)
	if selected < 0 and not rows.is_empty():
		selected = 0
	_goods_table.update_rows(cells, selected)
	if selected >= 0:
		_selected_good = String(rows[selected].get("id", ""))
	_set_text("market.goods.count", I18n.t("econ.pill.count", {
		"shown": UiKit.number(rows.size()),
		"total": UiKit.number(products.size()),
	}))
	_refresh_detail()


func _refresh_detail() -> void:
	var city := _city()
	var good := _good(_selected_good)
	if city.is_empty() or good.is_empty():
		_set_text("market.detail.name", I18n.t("market.select.good"))
		return
	var index := _good_index(String(good.get("id", "")))
	var market: Dictionary = {}
	var markets: Array = city.get("markets", [])
	if index >= 0 and index < markets.size():
		market = markets[index]
	var import_array: Array = city.get("import_price", [])
	var import_anchor := _at(import_array, index)
	var fx := maxf(float(city.get("fx_rate", 1.0)), 1e-9)
	var currency := String(city.get("currency", ""))
	_set_text("market.detail.name", _good_name(good))
	_set_text_in("market.detail.sector", _sector_name(good))
	_set_text_in("market.detail.kind", I18n.t("market.kind.%s"
			% String(good.get("kind", "raw"))))
	_set_text("market.detail.price",
			_money(float(market.get("price_local", 0.0)), currency))
	_set_text("market.detail.import", _money(import_anchor / fx, currency))
	_set_text("market.detail.stock",
			UiKit.number(float(market.get("stock", 0.0))) + " t")
	_set_text("market.detail.demand",
			UiKit.number(float(market.get("desired", 0.0))) + " t")
	_set_text("market.detail.currency",
			currency if not currency.is_empty() else I18n.t("common.na"))
	_set_text("market.detail.fx", UiKit.number(float(city.get("fx_rate", 0.0)), 4))
	_set_text("market.detail.note", I18n.t("market.detail.note"))
	_refresh_offers(city, good)


## Ofertas do dia para o bem: só pedidos publicados da cidade compradora.
func _refresh_offers(city: Dictionary, good: Dictionary) -> void:
	if _offers_table == null:
		return
	var good_id := String(good.get("id", ""))
	var city_id := String(city.get("id", ""))
	var currency := String(city.get("currency", ""))
	var fx := maxf(float(city.get("fx_rate", 1.0)), 1e-9)
	var rows: Array = []
	for order in _orders():
		if String(order.get("good", "")) != good_id:
			continue
		if String(order.get("to_city", "")) != city_id:
			continue
		rows.append(order)
	rows.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return float(a.get("delivered_price", 0.0)) \
				< float(b.get("delivered_price", 0.0)))

	var cells: Array = []
	for order in rows:
		var seller_currency := String(order.get("seller_currency", ""))
		cells.append([
			I18n.city_name(String(order.get("from_city", "")),
					String(order.get("from_city", ""))),
			_money(float(order.get("origin_price", 0.0)), seller_currency),
			_money(float(order.get("freight_cost", 0.0)) / fx, currency),
			_money(float(order.get("tariff", 0.0)) / fx, currency),
			{"text": _money(float(order.get("delivered_price", 0.0)), currency),
					"strong": true},
			{"text": I18n.t("scope.%s" % String(order.get("scope", "regional"))),
					"tone": "suave"},
		])
	_offers_table.update_rows(cells, -1)
	_set_text("market.offers.count", I18n.t("market.offers.count",
			{"n": UiKit.number(rows.size())}))
	_set_hidden("market.offers", rows.is_empty())
	_set_hidden("market.offers.empty", not rows.is_empty())


# --- Dados -------------------------------------------------------------------

## Bens do goods.json (lista única; a ordem é o `order` do contrato).
static func _ensure_goods() -> void:
	if not _goods_cache.is_empty():
		return
	var file := FileAccess.open(GOODS_PATH, FileAccess.READ)
	if file == null:
		return
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	if typeof(data) == TYPE_ARRAY:
		_goods_cache = data


func _good(id: String) -> Dictionary:
	for good in _goods_cache:
		if String(good.get("id", "")) == id:
			return good
	return {}


func _good_index(id: String) -> int:
	for index in _goods_cache.size():
		if String(_goods_cache[index].get("id", "")) == id:
			return index
	return -1


func _city() -> Dictionary:
	for city in _summary.get("cities", []):
		if String(city.get("id", "")) == _selected_city:
			return city
	return {}


func _orders() -> Array:
	return _summary.get("trade_log", _summary.get("trade_orders", []))


func _filtered_cities(cities: Array) -> Array:
	if _city_query.is_empty():
		return cities
	var needle := _city_query.to_lower()
	var result: Array = []
	for city in cities:
		var name := I18n.city_name(String(city.get("id", "")),
				String(city.get("name", ""))).to_lower()
		var nation := I18n.nation_name(String(city.get("nation", ""))).to_lower()
		if name.contains(needle) or nation.contains(needle):
			result.append(city)
	return result


func _matches_good(good: Dictionary) -> bool:
	if _good_query.is_empty():
		return true
	var needle := _good_query.to_lower()
	if _good_name(good).to_lower().contains(needle):
		return true
	return String(good.get("id", "")).to_lower().contains(needle)


func _good_name(good: Dictionary) -> String:
	var id := String(good.get("id", ""))
	var key := "product." + id
	var translated := I18n.t(key)
	if translated == key:
		return String(good.get("name", id))
	return translated


func _sector_name(good: Dictionary) -> String:
	return I18n.t("market.sector.%s" % String(good.get("sector", "raw")))


static func _at(values: Array, index: int) -> float:
	return float(values[index]) if index >= 0 and index < values.size() else 0.0


static func _money(value: float, currency: String) -> String:
	if value <= 0.0:
		return I18n.t("common.na")
	var text := UiKit.number(value, 1)
	return "%s %s" % [text, currency] if not currency.is_empty() else text


static func _tons(value: float) -> String:
	return UiKit.number(value) + " t"


# --- Utilidades --------------------------------------------------------------

func _selected_index(rows: Array, id: String) -> int:
	if id.is_empty():
		return -1
	for index in rows.size():
		if String(rows[index].get("id", "")) == id:
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


## Slots de tag embrulham o rótulo: procura o primeiro Label descendente.
func _set_text_in(slot: String, text: String) -> void:
	var node := _slots.get(slot) as Node
	if node == null:
		return
	if node is Label:
		(node as Label).text = text
		return
	var pending: Array[Node] = [node]
	while not pending.is_empty():
		var current: Node = pending.pop_front()
		if current is Label:
			(current as Label).text = text
			return
		pending.append_array(current.get_children())


func _set_hidden(slot: String, hidden: bool) -> void:
	var node := _slots.get(slot) as CanvasItem
	if node != null:
		node.visible = not hidden
