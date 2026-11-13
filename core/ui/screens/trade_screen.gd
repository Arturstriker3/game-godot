extends Control

## Janela de Comércio e Logística no template T2 (Registro), com abas de pasta.
##
## O desenho vem de `data/ui/templates/comercio.json` (`UiTemplateBuilder`):
## moldura de janela, três abas de pasta e tabelas de papel. Este script só
## formata o resumo publicado pela simulação — busca, filtro, ordenação, página
## e seleção são estado local de interface e nunca tocam na simulação.
##
## Abas: **Pedidos** (lista do dia + trechos do pedido selecionado), **Rotas**
## (soma por modo de transporte) e **Mercado** (busca, filtros e a tabela por
## cidade, com o resumo nacional acima).

signal city_requested(city_id: String)
signal close_requested

const FILTER_ALL := "all"
const FILTER_DEFICIT := "deficit"
const FILTER_SCARCITY := "scarcity"
const LOW_COVERAGE_DAYS := 2.0
const TAB_ORDERS := 0
const TAB_ROUTES := 1
const TAB_MARKET := 2

var _summary: Dictionary = {}
var _tab := TAB_ORDERS
var _selected := 0
var _market_filter := FILTER_ALL
var _market_query := ""

var _root: UiTemplateBuilder.TemplateRoot
var _slots := {}
var _tabs: UiTemplateBuilder.FolderTabs
var _orders_table: UiTemplateBuilder.DataTableView
var _legs_table: UiTemplateBuilder.DataTableView
var _routes_table: UiTemplateBuilder.DataTableView
var _market_table: UiTemplateBuilder.DataTableView
var _market_ids: Array[String] = []
var _structure_key := ""


func _ready() -> void:
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)


## O HUD troca o painel modal escuro pela moldura de papel da spec.
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
	_tab = clampi(index, TAB_ORDERS, TAB_MARKET)
	if _tabs == null:
		_refresh()
		return
	_tabs.select(_tab)


# --- Montagem ----------------------------------------------------------------

func _rebuild() -> void:
	_structure_key = I18n.locale
	if _root != null and is_instance_valid(_root):
		remove_child(_root)
		_root.queue_free()
	_slots.clear()
	_tabs = null
	_orders_table = null
	_legs_table = null
	_routes_table = null
	_market_table = null

	_root = UiTemplateBuilder.build(UiTemplateBuilder.load_spec("comercio"))
	_root.name = "TradeRegistry"
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
		_tabs.select(_tab)
		_tab = _tabs.active

	_orders_table = _slots.get("trade.orders") as UiTemplateBuilder.DataTableView
	if _orders_table != null:
		_orders_table.row_selected.connect(_on_order_selected)
	_legs_table = _slots.get("trade.legs.table") \
			as UiTemplateBuilder.DataTableView
	_routes_table = _slots.get("trade.routes") as UiTemplateBuilder.DataTableView
	_market_table = _slots.get("trade.market") as UiTemplateBuilder.DataTableView
	if _market_table != null:
		_market_table.row_activated.connect(_on_market_row)

	var search := _line_edit("trade.search")
	if search != null:
		search.text = _market_query
		search.text_changed.connect(_on_market_search)

	for id in [FILTER_ALL, FILTER_DEFICIT, FILTER_SCARCITY]:
		var chip := _slots.get("trade.filter.%s" % id) as Button
		if chip == null:
			continue
		chip.pressed.connect(_set_market_filter.bind(id))


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
	_tab = index
	_refresh()


func _on_order_selected(index: int) -> void:
	_selected = index
	_refresh_legs()


func _set_market_filter(id: String) -> void:
	if _market_filter == id:
		return
	_market_filter = id
	_reset_market_page()
	_refresh_market()


func _on_market_search(text: String) -> void:
	_market_query = text.strip_edges()
	_reset_market_page()
	_refresh_market()


func _on_choice(id: String) -> void:
	if id == "clear":
		_clear_market_filters()


func _clear_market_filters() -> void:
	_market_filter = FILTER_ALL
	_market_query = ""
	var search := _line_edit("trade.search")
	if search != null:
		search.text = ""
	_reset_market_page()
	_refresh_market()


func _on_market_row(index: int) -> void:
	if index < 0 or index >= _market_ids.size():
		return
	city_requested.emit(_market_ids[index])


func _reset_market_page() -> void:
	if _market_table != null:
		_market_table.reset_page()


# --- Desenho -----------------------------------------------------------------

func _refresh() -> void:
	if _root == null:
		return
	_set_text("nav.title", I18n.t("trade.title"))
	_refresh_orders()
	_refresh_routes()
	_refresh_market()


func _refresh_orders() -> void:
	if _orders_table == null:
		return
	var orders := _orders()
	_selected = clampi(_selected, 0, maxi(orders.size() - 1, 0))

	var cells: Array = []
	for order in orders:
		cells.append([
			{"text": UiKit.number(float(order.get("day", 0))), "tone": "suave"},
			I18n.city_name(String(order.get("from_city", "")),
					String(order.get("from_city", ""))),
			I18n.city_name(String(order.get("to_city", "")),
					String(order.get("to_city", ""))),
			{"text": I18n.t("scope.%s" % String(order.get("scope", ""))),
					"tone": "suave"},
			UiKit.number(float(order.get("amount", 0.0))) + " t",
			UiKit.number(float(order.get("transit_hours", 0.0)), 1) + " h",
			UiKit.number(float(order.get("freight_cost", 0.0))),
		])

	_orders_table.update_rows(cells, _selected if not orders.is_empty() else -1)
	_set_hidden("trade.orders", orders.is_empty())
	_set_hidden("trade.orders.empty", not orders.is_empty())
	_set_text("trade.orders.count",
			I18n.t("tpl.common.result", {"n": UiKit.number(orders.size())}))
	_refresh_legs()


func _refresh_legs() -> void:
	if _legs_table == null:
		return
	var orders := _orders()
	_set_hidden("trade.legs", orders.is_empty())
	if orders.is_empty():
		return
	var order: Dictionary = orders[clampi(_selected, 0, orders.size() - 1)]
	_set_text("trade.legs.title", "%s → %s" % [
		I18n.city_name(String(order.get("from_city", "")),
				String(order.get("from_city", ""))),
		I18n.city_name(String(order.get("to_city", "")),
				String(order.get("to_city", ""))),
	])
	_set_text("trade.legs.amount",
			UiKit.number(float(order.get("amount", 0.0))) + " t")
	_set_text("trade.legs.hours",
			UiKit.number(float(order.get("transit_hours", 0.0)), 1) + " h")
	_set_text("trade.legs.freight",
			UiKit.number(float(order.get("freight_cost", 0.0))))

	var legs: Array = order.get("legs", [])
	var cells: Array = []
	for leg in legs:
		cells.append([
			{"text": I18n.t("mode.%s" % String(leg.get("mode", ""))),
					"tone": "suave"},
			"%s → %s" % [
				I18n.city_name(String(leg.get("from", "")), String(leg.get("from", ""))),
				I18n.city_name(String(leg.get("to", "")), String(leg.get("to", "")))],
			UiKit.number(float(leg.get("distance_km", 0.0))) + " km",
			UiKit.number(float(leg.get("load_tons", 0.0))) + " t",
			UiKit.number(float(leg.get("trips", 0.0))),
			UiKit.number(float(leg.get("transit_hours", 0.0)), 1) + " h",
			UiKit.number(float(leg.get("cost", 0.0))),
		])
	_legs_table.update_rows(cells, -1)
	_set_hidden("trade.legs.table", cells.is_empty())
	_set_hidden("trade.legs.empty", not cells.is_empty())


func _refresh_routes() -> void:
	if _routes_table == null:
		return
	var totals := _mode_totals()
	var cells: Array = []
	for mode in ["truck", "ship", "plane"]:
		var data: Dictionary = totals[mode]
		var empty := int(data["trips"]) == 0
		var tone := "suave" if empty else "tinta"
		cells.append([
			{"text": I18n.t("mode.%s" % mode), "tone": tone},
			{"text": UiKit.number(float(data["trips"])), "tone": tone},
			{"text": UiKit.number(float(data["km"])) + " km", "tone": tone},
			{"text": UiKit.number(float(data["tons"])) + " t", "tone": tone},
			{"text": UiKit.number(float(data["cost"])), "tone": tone},
		])
	_routes_table.update_rows(cells, -1)


func _refresh_market() -> void:
	if _market_table == null:
		return
	for id in [FILTER_ALL, FILTER_DEFICIT, FILTER_SCARCITY]:
		var chip := _slots.get("trade.filter.%s" % id) as Button
		if chip != null:
			UiTemplateBuilder.set_chip_active(chip, id == _market_filter)

	var rows := _market_rows()
	_market_ids.clear()
	var cells: Array = []
	for entry in rows:
		var city: Dictionary = entry["city"]
		var metrics: Dictionary = entry["metrics"]
		var deficit := float(metrics["deficit"])
		var coverage := float(metrics["coverage"])
		_market_ids.append(String(city["id"]))
		cells.append([
			I18n.city_name(String(city["id"]), String(city["name"])),
			UiKit.number(float(metrics["production"])) + " t",
			UiKit.number(float(metrics["need"])) + " t",
			UiKit.number(float(metrics["stockpile"])) + " t",
			{"text": UiKit.number(deficit) + " t",
					"tone": "carimbo" if deficit > 0.0 else "suave"},
			{"text": _coverage_label(coverage), "tone": _coverage_tone(coverage)},
			{"text": UiKit.number(float(metrics["imported"])) + " t",
					"tone": "suave"},
			{"text": UiKit.number(float(metrics["exported"])) + " t",
					"tone": "suave"},
		])

	_market_table.update_rows(cells, -1)
	_set_hidden("trade.market", rows.is_empty())
	_set_hidden("trade.market.empty", not rows.is_empty())
	_set_text("trade.count", I18n.t("econ.pill.count", {
		"shown": UiKit.number(rows.size()),
		"total": UiKit.number(_summary.get("cities", []).size()),
	}))

	var totals := _national_totals()
	_set_text("trade.national.offer",
			UiKit.number(float(totals["production"])) + " t")
	_set_text("trade.national.need", UiKit.number(float(totals["need"])) + " t")
	_set_text("trade.national.stock", UiKit.number(float(totals["stockpile"])) + " t")
	_set_text("trade.national.deficit", UiKit.number(float(totals["deficit"])) + " t")
	_set_text("trade.national.coverage", _coverage_label(float(totals["coverage"])))


# --- Dados do resumo ----------------------------------------------------------

func _orders() -> Array:
	return _summary.get("trade_log", _summary.get("trade_orders", []))


func _city_market_metrics(city: Dictionary) -> Dictionary:
	var need := float(city.get("need", 0.0))
	var stock := float(city.get("stockpile", 0.0))
	var imported := 0.0
	var exported := 0.0
	var today := int(_summary.get("day", 0))

	for order in _orders():
		# `trade_log` acumula vários dias; os fluxos do mercado são do dia atual.
		if order.has("day") and int(order["day"]) != today:
			continue
		var amount := float(order.get("amount", 0.0))
		if String(order.get("to_city", "")) == String(city["id"]):
			imported += amount
		if String(order.get("from_city", "")) == String(city["id"]):
			exported += amount

	return {
		"production": float(city.get("production", 0.0)),
		"need": need,
		"stockpile": stock,
		"deficit": float(city.get("deficit", 0.0)),
		"coverage": stock / need if need > 0.0 else INF,
		"imported": imported,
		"exported": exported,
	}


## Aplica busca e filtro sobre os dados reais. A ordenação fica com a tabela.
func _market_rows() -> Array:
	var needle := _market_query.to_lower()
	var rows: Array = []

	for city in _summary.get("cities", []):
		var metrics := _city_market_metrics(city)
		if _market_filter == FILTER_DEFICIT and float(metrics["deficit"]) <= 0.0:
			continue
		if _market_filter == FILTER_SCARCITY \
				and float(metrics["coverage"]) >= LOW_COVERAGE_DAYS:
			continue
		if not needle.is_empty():
			var name := I18n.city_name(String(city["id"]),
					String(city["name"])).to_lower()
			var nation := I18n.nation_name(String(city["nation"])).to_lower()
			if not (name.contains(needle) or nation.contains(needle)):
				continue
		rows.append({"city": city, "metrics": metrics})
	return rows


func _national_totals() -> Dictionary:
	var production := 0.0
	var need := 0.0
	var stock := 0.0
	var deficit := 0.0

	for city in _summary.get("cities", []):
		production += float(city.get("production", 0.0))
		need += float(city.get("need", 0.0))
		stock += float(city.get("stockpile", 0.0))
		deficit += float(city.get("deficit", 0.0))

	return {
		"production": production,
		"need": need,
		"stockpile": stock,
		"deficit": deficit,
		"coverage": stock / need if need > 0.0 else INF,
	}


## Soma por modo de transporte os trechos dos pedidos do dia.
func _mode_totals() -> Dictionary:
	var totals := {
		"truck": {"trips": 0, "km": 0.0, "tons": 0.0, "cost": 0.0},
		"ship": {"trips": 0, "km": 0.0, "tons": 0.0, "cost": 0.0},
		"plane": {"trips": 0, "km": 0.0, "tons": 0.0, "cost": 0.0},
	}

	for order in _orders():
		for leg in order.get("legs", []):
			var mode := String(leg.get("mode", ""))
			if not totals.has(mode):
				continue
			var bucket: Dictionary = totals[mode]
			bucket["trips"] = int(bucket["trips"]) + int(leg.get("trips", 0))
			bucket["km"] = float(bucket["km"]) \
					+ float(leg.get("distance_km", 0.0)) * float(leg.get("trips", 0))
			bucket["tons"] = float(bucket["tons"]) + float(leg.get("load_tons", 0.0))
			bucket["cost"] = float(bucket["cost"]) + float(leg.get("cost", 0.0))

	return totals


# --- Utilidades ---------------------------------------------------------------

## Cobertura de estoque em dias; sem demanda, mostra ausência em vez de zero.
func _coverage_label(days: float) -> String:
	if not is_finite(days):
		return I18n.t("common.na")
	return UiKit.number(days, 1) + " " + I18n.t("common.days_short")


func _coverage_tone(days: float) -> String:
	if not is_finite(days) or days >= 7.0:
		return "suave"
	return "alerta" if days >= LOW_COVERAGE_DAYS else "carimbo"


func _set_text(slot: String, text: String) -> void:
	var label := _slots.get(slot) as Label
	if label != null:
		label.text = text


func _set_hidden(slot: String, hidden: bool) -> void:
	var node := _slots.get(slot) as CanvasItem
	if node != null:
		node.visible = not hidden
