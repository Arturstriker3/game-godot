extends Control

## Logística no template T2: **Frota**, **Câmbio**, **Tarifas** e **Portos**.
##
## A tela só exibe o que o núcleo publica (frota/combustível por nação, moedas e
## tarifas, capacidade/fila dos portos) e usa as APIs de decisão do jogador
## `set_tariff` e `expand_port`. Nenhum número é inventado: nação sem
## frota mostra o estado vazio, e sem tarifa registrada a tabela fica vazia.

signal close_requested

const SPEC_ID := "logistica"
const GOODS_PATH := "res://data/definitions/economy/goods.json"
const TAB_FLEET := 0
const TAB_FX := 1
const TAB_TARIFF := 2
const TAB_PORTS := 3
const RATES := [0.0, 0.05, 0.10, 0.25, 0.50]
const EXPANSIONS := [0.10, 0.25, 0.50]

static var _goods_cache: Array = []

var _summary: Dictionary = {}
var _tab := TAB_FLEET
var _selected_nation := ""
var _nation_query := ""
var _fx_query := ""
var _port_query := ""
var _selected_port := ""
var _selected_partner := ""
var _selected_good := "wheat"
var _rate := 0.0
var _expansion := 0.25
var _tariff_result := ""
var _port_result := ""
var _live_tariffs: Array = []

var _root: UiTemplateBuilder.TemplateRoot
var _slots := {}
var _tabs: UiTemplateBuilder.FolderTabs
var _fleet_table: UiTemplateBuilder.DataTableView
var _fx_table: UiTemplateBuilder.DataTableView
var _tariff_table: UiTemplateBuilder.DataTableView
var _port_table: UiTemplateBuilder.DataTableView
var _fleet_select: UiTemplateBuilder.PaperSelect
var _fx_select: UiTemplateBuilder.PaperSelect
var _tariff_nation_select: UiTemplateBuilder.PaperSelect
var _tariff_partner_select: UiTemplateBuilder.PaperSelect
var _tariff_good_select: UiTemplateBuilder.PaperSelect
var _chips := {}
var _structure_key := ""
var _port_rows: Array = []
var _fleet_rows: Array = []
var _fx_rows: Array = []
var _tariff_rows: Array = []


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


## Captura/testes: 0..3 ou "fleet"/"fx"/"tariff"/"ports".
func _set_tab(id: Variant) -> void:
	var names := ["fleet", "fx", "tariff", "ports"]
	var index := TAB_FLEET
	if id is int:
		index = clampi(int(id), 0, names.size() - 1)
	else:
		var found := names.find(str(id))
		index = found if found >= 0 else TAB_FLEET
	_tab = index
	if _tabs != null:
		_tabs.select(index)
	_refresh()


func current_nation() -> String:
	return _selected_nation


func current_port() -> String:
	return _selected_port


func current_tab() -> int:
	return _tab


## Abre a aba Portos já no porto escolhido (clique no porto do mapa).
func select_port_by_name(port_name: String) -> void:
	for port in _summary.get("ports", []):
		if String(port.get("name", "")) == port_name:
			_selected_port = String(port.get("id", ""))
			_tab = TAB_PORTS
			if _tabs != null:
				_tabs.select(TAB_PORTS)
			_refresh_ports()
			_refresh_port_detail()
			if _port_table != null:
				_port_table.set_selected(_index_of(_port_rows, "id", _selected_port))
			return


# --- Montagem ----------------------------------------------------------------

func _rebuild() -> void:
	_structure_key = I18n.locale
	if _root != null and is_instance_valid(_root):
		remove_child(_root)
		_root.queue_free()
	_slots.clear()
	_tabs = null
	_fleet_table = null
	_fx_table = null
	_tariff_table = null
	_port_table = null
	_chips.clear()

	_root = UiTemplateBuilder.build(UiTemplateBuilder.load_spec(SPEC_ID))
	_root.name = "LogisticsRegistry"
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

	_fleet_table = _slots.get("fleet.nations") as UiTemplateBuilder.DataTableView
	if _fleet_table != null:
		_fleet_table.row_selected.connect(_on_fleet_selected)
		_fleet_table.row_activated.connect(_on_fleet_selected)
	_fx_table = _slots.get("fx.table") as UiTemplateBuilder.DataTableView
	_port_table = _slots.get("port.table") as UiTemplateBuilder.DataTableView
	if _port_table != null:
		_port_table.row_selected.connect(_on_port_selected)
		_port_table.row_activated.connect(_on_port_selected)
	_tariff_table = _slots.get("tariff.table") as UiTemplateBuilder.DataTableView

	_fleet_select = _slots.get("fleet.nation") as UiTemplateBuilder.PaperSelect
	_fx_select = _slots.get("fx.nation") as UiTemplateBuilder.PaperSelect
	_tariff_nation_select = _slots.get("tariff.nation") \
			as UiTemplateBuilder.PaperSelect
	_tariff_partner_select = _slots.get("tariff.partner") \
			as UiTemplateBuilder.PaperSelect
	_tariff_good_select = _slots.get("tariff.good") as UiTemplateBuilder.PaperSelect
	for select in [_fleet_select, _fx_select, _tariff_nation_select]:
		if select != null:
			select.selected.connect(_select_nation)
	if _tariff_partner_select != null:
		_tariff_partner_select.selected.connect(_select_partner)
	if _tariff_good_select != null:
		_tariff_good_select.selected.connect(_select_good)

	for id in [0, 5, 10, 25, 50]:
		var chip := _slots.get("tariff.rate.%d" % id) as Button
		if chip != null:
			chip.pressed.connect(_set_rate.bind(float(id) / 100.0))
			_chips["rate.%d" % id] = chip
	for ratio in EXPANSIONS:
		var chip := _slots.get("port.expand.%d" % int(ratio * 100.0)) as Button
		if chip != null:
			chip.pressed.connect(_set_expansion.bind(ratio))
			_chips["expand.%d" % int(ratio * 100.0)] = chip

	var fleet_search := _line_edit("fleet.search")
	if fleet_search != null:
		fleet_search.text_changed.connect(_on_fleet_search)
	var fx_search := _line_edit("fx.search")
	if fx_search != null:
		fx_search.text_changed.connect(_on_fx_search)
	var port_search := _line_edit("port.search")
	if port_search != null:
		port_search.text_changed.connect(_on_port_search)


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
	_tab = clampi(index, TAB_FLEET, TAB_PORTS)


func _on_fleet_selected(index: int) -> void:
	if index < 0 or index >= _fleet_rows.size():
		return
	_select_nation(String(_fleet_rows[index].get("tag", "")))


func _on_port_selected(index: int) -> void:
	if index < 0 or index >= _port_rows.size():
		return
	_selected_port = String(_port_rows[index].get("id", ""))
	_refresh_port_detail()


func _on_fleet_search(text: String) -> void:
	_nation_query = text.strip_edges()
	_refresh_fleet()
	if _fleet_table != null:
		_fleet_table.reset_page()


func _on_fx_search(text: String) -> void:
	_fx_query = text.strip_edges()
	_refresh_fx()
	if _fx_table != null:
		_fx_table.reset_page()


func _on_port_search(text: String) -> void:
	_port_query = text.strip_edges()
	_refresh_ports()
	if _port_table != null:
		_port_table.reset_page()


func _select_nation(tag: String) -> void:
	if tag.is_empty() or tag == _selected_nation:
		return
	_selected_nation = tag
	for select in [_fleet_select, _fx_select, _tariff_nation_select]:
		if select != null:
			(select as UiTemplateBuilder.PaperSelect).select(tag)
	_refresh_fleet_detail()
	_refresh_fx_card()
	_refresh_tariffs()
	if _fleet_table != null:
		_fleet_table.set_selected(_index_of(_fleet_rows, "tag", tag))


func _select_partner(tag: String) -> void:
	_selected_partner = tag
	_refresh_tariff_cost()


func _select_good(id: String) -> void:
	_selected_good = id
	_refresh_tariff_cost()


func _set_rate(rate: float) -> void:
	_rate = rate
	_refresh_tariff_cost()


func _set_expansion(ratio: float) -> void:
	_expansion = ratio
	_refresh_port_cost()


func _on_choice(id: String) -> void:
	if id == "tariff_apply":
		_apply_tariff()
	elif id == "port_expand":
		_expand_port()


# --- Desenho -----------------------------------------------------------------

func _refresh() -> void:
	if _root == null:
		return
	_set_text("nav.title", I18n.t("logistics.title"))
	_ensure_defaults()
	_populate_selects()
	_refresh_fleet()
	_refresh_fleet_detail()
	_refresh_fx()
	_refresh_fx_card()
	_refresh_tariffs()
	_refresh_ports()
	_refresh_port_detail()


func _ensure_defaults() -> void:
	var nations: Array = _summary.get("nations", [])
	if _selected_nation.is_empty() and not nations.is_empty():
		_selected_nation = "GBR"
		var found := false
		for nation in nations:
			if String(nation.get("tag", "")) == _selected_nation:
				found = true
				break
		if not found:
			_selected_nation = String(nations[0].get("tag", ""))
	if _selected_partner.is_empty():
		_selected_partner = _first_partner()


func _populate_selects() -> void:
	var nations: Array = []
	for nation in _summary.get("nations", []):
		nations.append({
			"id": String(nation.get("tag", "")),
			"label": I18n.nation_name(String(nation.get("tag", ""))),
		})
	for select in [_fleet_select, _fx_select, _tariff_nation_select]:
		if select != null:
			(select as UiTemplateBuilder.PaperSelect).set_items(nations,
					_selected_nation)
	if _tariff_partner_select != null:
		_tariff_partner_select.set_items(nations, _selected_partner)
	if _tariff_good_select != null:
		var goods: Array = []
		for good in _goods_cache:
			goods.append({
				"id": String(good.get("id", "")),
				"label": _good_name(good),
			})
		_tariff_good_select.set_items(goods, _selected_good)


func _refresh_fleet() -> void:
	if _fleet_table == null:
		return
	var rows: Array = []
	for nation in _summary.get("nations", []):
		if not nation.has("fleet"):
			continue
		var tag := String(nation.get("tag", ""))
		var name := I18n.nation_name(tag)
		if not _nation_query.is_empty() \
				and not name.to_lower().contains(_nation_query.to_lower()):
			continue
		rows.append(nation)
	rows.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return float((a.get("fleet", {}) as Dictionary).get("trucks", 0.0)) \
				> float((b.get("fleet", {}) as Dictionary).get("trucks", 0.0)))
	_fleet_rows = rows
	var cells: Array = []
	for nation in rows:
		var fleet: Dictionary = nation.get("fleet", {})
		cells.append([
			I18n.nation_name(String(nation.get("tag", ""))),
			UiKit.number(float(fleet.get("trucks", 0.0))),
			UiKit.number(float(fleet.get("ships", 0.0))),
			UiKit.number(float(fleet.get("planes", 0.0))),
			UiKit.number(float(fleet.get("diesel", 0.0))) + " t",
			UiKit.number(float(fleet.get("jet_fuel", 0.0))) + " t",
			UiKit.number(float(fleet.get("queue", 0.0))) + " t",
		])
	var index := _index_of(rows, "tag", _selected_nation)
	_fleet_table.update_rows(cells, index)
	if index >= 0:
		_selected_nation = String(rows[index].get("tag", ""))
	_set_text("fleet.count", I18n.t("tpl.common.result",
			{"n": UiKit.number(rows.size())}))


func _refresh_fleet_detail() -> void:
	var nation := _nation(_selected_nation)
	if nation.is_empty():
		return
	var fleet: Dictionary = nation.get("fleet", {})
	var live := _live_fleet(_selected_nation)
	var health: Array = fleet.get("health", [1.0, 1.0, 1.0])
	_set_meter_text("fleet.trucks", fleet, live, "trucks", 0, health)
	_set_meter_text("fleet.ships", fleet, live, "ships", 1, health)
	_set_meter_text("fleet.planes", fleet, live, "planes", 2, health)
	_set_text("fleet.diesel",
			UiKit.number(float(fleet.get("diesel", 0.0))) + " t")
	_set_text("fleet.jet",
			UiKit.number(float(fleet.get("jet_fuel", 0.0))) + " t")
	_set_text("fleet.parts",
			UiKit.number(float(fleet.get("spare_parts", 0.0))) + " t")
	_set_text("fleet.queue", UiKit.number(float(fleet.get("queue", 0.0))) + " t")
	_set_text("fleet.missed_fleet",
			UiKit.number(float(fleet.get("missed_no_fleet", 0.0))) + " t")
	_set_text("fleet.missed_fuel",
			UiKit.number(float(fleet.get("missed_no_fuel", 0.0))) + " t")
	_set_text("fleet.source", I18n.t("fleet.source.%s"
			% String(fleet.get("source", "declared"))))


func _set_meter_text(slot: String, fleet: Dictionary, live: Dictionary,
		key: String, health_index: int, health: Array) -> void:
	var total := float(fleet.get(key, 0.0))
	var busy := float(live.get("%s_busy" % key, -1.0))
	var value := float(health[health_index]) if health_index < health.size() else 1.0
	var ratio := 0.0
	if total > 0.0 and busy >= 0.0:
		ratio = clampf(busy / total, 0.0, 1.0)
	var text := "—"
	if total > 0.0 and busy >= 0.0:
		text = I18n.t("fleet.usage", {
			"used": UiKit.number(busy),
			"free": UiKit.number(maxf(total - busy, 0.0)),
			"health": UiKit.number(value * 100.0),
		})
	elif total > 0.0:
		text = I18n.t("fleet.total_health", {
			"total": UiKit.number(total),
			"health": UiKit.number(value * 100.0),
		})
	var meter := _slots.get("%s.meter" % slot) as UiTemplateBuilder.MeterBar
	if meter != null:
		meter.ratio = ratio
		meter.fill = _health_color(value)
		meter.queue_redraw()
	_set_text("%s.text" % slot, text)


func _refresh_fx() -> void:
	if _fx_table == null:
		return
	var rows: Array = []
	var needle := _fx_query.to_lower()
	for currency in _summary.get("currencies", []):
		var code := String(currency.get("code", ""))
		var source := String(currency.get("source", ""))
		if not needle.is_empty() and not code.to_lower().contains(needle) \
				and not source.to_lower().contains(needle):
			continue
		rows.append(currency)
	_fx_rows = rows
	var cells: Array = []
	for currency in rows:
		cells.append([
			String(currency.get("code", "")),
			UiKit.number(float(currency.get("rate", 0.0)), 4),
			I18n.t("fx.regime.%s" % String(currency.get("regime", "peg"))),
			UiKit.number(float(currency.get("trade_balance", 0.0)), 1),
			{"text": String(currency.get("source", "")), "tone": "suave"},
		])
	_fx_table.update_rows(cells, -1)
	_set_text("fx.count", I18n.t("tpl.common.result",
			{"n": UiKit.number(rows.size())}))


func _refresh_fx_card() -> void:
	var code := _nation_currency(_selected_nation)
	var currency := _currency(code)
	if currency.is_empty():
		_set_text("fx.code", I18n.t("common.na"))
		_set_text("fx.rate", I18n.t("common.na"))
		_set_text("fx.regime", I18n.t("common.na"))
		_set_text("fx.base", I18n.t("common.na"))
		_set_text("fx.balance", I18n.t("common.na"))
		_set_text("fx.tariff_revenue", I18n.t("common.na"))
		_set_text("fx.source", I18n.t("common.na"))
		return
	_set_text("fx.code", code)
	_set_text("fx.rate", UiKit.number(float(currency.get("rate", 0.0)), 4))
	_set_text("fx.regime", I18n.t("fx.regime.%s"
			% String(currency.get("regime", "peg"))))
	_set_text("fx.base", UiKit.number(float(currency.get("base_rate",
			currency.get("rate", 0.0))), 4))
	_set_text("fx.balance", UiKit.number(float(currency.get("trade_balance", 0.0)), 1))
	var revenue := _nation_tariff_revenue(_selected_nation)
	_set_text("fx.tariff_revenue", UiKit.number(revenue, 1) + " " + code \
			if not code.is_empty() else UiKit.number(revenue, 1))
	_set_text("fx.source", String(currency.get("source", I18n.t("common.na"))))


func _refresh_tariffs() -> void:
	if _tariff_table == null:
		return
	for value in RATES:
		var chip: Button = _chips.get("rate.%d" % int(value * 100.0))
		if chip != null:
			UiTemplateBuilder.set_chip_active(chip, is_equal_approx(_rate, value))
	var rows: Array = []
	for tariff in _tariffs():
		if String(tariff.get("buyer", "")) != _selected_nation:
			continue
		rows.append(tariff)
	_tariff_rows = rows
	var cells: Array = []
	for tariff in rows:
		cells.append([
			I18n.nation_name(String(tariff.get("buyer", ""))),
			I18n.nation_name(String(tariff.get("seller", "")))
					if String(tariff.get("seller", "")) != "*" \
					else I18n.t("tariff.any"),
			_good_name_by_id(String(tariff.get("product", "")))
					if String(tariff.get("product", "")) != "*" \
					else I18n.t("tariff.any"),
			UiKit.number(float(tariff.get("rate", 0.0)) * 100.0) + "%",
		])
	_tariff_table.update_rows(cells, -1)
	_set_hidden("tariff.table", rows.is_empty())
	_set_hidden("tariff.empty", not rows.is_empty())
	_refresh_tariff_cost()


func _refresh_tariff_cost() -> void:
	var buyer := I18n.nation_name(_selected_nation)
	var partner := I18n.nation_name(_selected_partner)
	var good := _good_name_by_id(_selected_good)
	_set_text("tariff.result", _tariff_result if not _tariff_result.is_empty() \
			else I18n.t("tariff.hint", {
				"buyer": buyer,
				"partner": partner,
				"good": good,
				"rate": UiKit.number(_rate * 100.0),
			}))


func _apply_tariff() -> void:
	if _selected_nation.is_empty() or _selected_partner.is_empty() \
			or _selected_good.is_empty():
		return
	if Economy.sim == null or not Economy.sim.has_method("set_tariff"):
		_tariff_result = I18n.t("tariff.unavailable")
		_refresh_tariff_cost()
		return
	Economy.sim.set_tariff(_selected_nation, _selected_partner,
			_selected_good, _rate)
	_tariff_result = I18n.t("tariff.applied", {
		"buyer": I18n.nation_name(_selected_nation),
		"partner": I18n.nation_name(_selected_partner),
		"good": _good_name_by_id(_selected_good),
		"rate": UiKit.number(_rate * 100.0),
	})
	_live_tariffs = Economy.sim.get_tariffs()
	_refresh_tariffs()
	_refresh_tariff_cost()


func _refresh_ports() -> void:
	if _port_table == null:
		return
	var rows: Array = []
	var needle := _port_query.to_lower()
	for port in _summary.get("ports", []):
		if not needle.is_empty():
			var haystack := "%s %s %s" % [
				String(port.get("name", "")),
				I18n.nation_name(String(port.get("nation", ""))),
				String(port.get("host_city", "")),
			]
			if not haystack.to_lower().contains(needle):
				continue
		rows.append(port)
	rows.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return float(a.get("capacity_t_per_day", 0.0)) \
				> float(b.get("capacity_t_per_day", 0.0)))
	_port_rows = rows
	var cells: Array = []
	for port in rows:
		cells.append([
			String(port.get("name", "")),
			I18n.nation_name(String(port.get("nation", ""))),
			UiKit.number(float(port.get("level", 1))),
			_throughput(float(port.get("capacity_t_per_day", 0.0))),
			_throughput(float(port.get("throughput", 0.0))),
			UiKit.number(float(port.get("queue", 0.0))) + " t",
		])
	var index := _index_of(rows, "id", _selected_port)
	if index < 0 and not rows.is_empty():
		index = 0
	_port_table.update_rows(cells, index)
	if index >= 0:
		_selected_port = String(rows[index].get("id", ""))
	_set_text("port.count", I18n.t("tpl.common.result",
			{"n": UiKit.number(rows.size())}))


func _refresh_port_detail() -> void:
	var port := _port()
	if port.is_empty():
		_set_text("port.name", I18n.t("port.select"))
		return
	_set_text("port.name", String(port.get("name", "")))
	var host := String(port.get("host_city", ""))
	_set_text("port.host", I18n.city_name(host, host) if not host.is_empty() \
			else I18n.t("common.na"))
	_set_text("port.level", UiKit.number(float(port.get("level", 1))))
	_set_text("port.capacity", _throughput(float(port.get("capacity_t_per_day", 0.0))))
	_set_text("port.throughput", _throughput(float(port.get("throughput", 0.0))))
	_set_text("port.queue", UiKit.number(float(port.get("queue", 0.0))) + " t")
	_set_text("port.delay", UiKit.number(float(port.get("delay_hours", 0.0)), 1)
			+ " h")
	var utilization := clampf(float(port.get("utilization", 0.0)), 0.0, 1.0)
	var meter := _slots.get("port.occupancy.meter") as UiTemplateBuilder.MeterBar
	if meter != null:
		meter.ratio = utilization
		meter.fill = UiKit.CARIMBO if utilization >= 0.9 \
				else UiKit.ALERTA if utilization >= 0.7 else UiKit.TINTA
		meter.queue_redraw()
	_set_text("port.occupancy.text", UiKit.number(utilization * 100.0) + "%")
	for value in EXPANSIONS:
		var chip: Button = _chips.get("expand.%d" % int(value * 100.0))
		if chip != null:
			UiTemplateBuilder.set_chip_active(chip,
					is_equal_approx(_expansion, value))
	_refresh_port_cost()


func _refresh_port_cost() -> void:
	var port := _port()
	if port.is_empty():
		_set_text("port.cost", I18n.t("port.expand.none"))
		return
	var amount := _expansion_amount(port)
	_set_text("port.cost", I18n.t("port.expand.cost", {
		"amount": UiKit.number(amount),
		"cost": UiKit.number(amount * 500.0),
	}))
	if not _port_result.is_empty():
		_set_text("port.result", _port_result)


func _expand_port() -> void:
	var port := _port()
	if port.is_empty() or Economy.sim == null \
			or not Economy.sim.has_method("expand_port"):
		_port_result = I18n.t("port.expand.unavailable")
		_set_text("port.result", _port_result)
		return
	var amount := _expansion_amount(port)
	var result: Dictionary = Economy.sim.expand_port(_selected_port, amount)
	if bool(result.get("expanded", false)):
		_port_result = I18n.t("port.expand.ok", {
			"amount": UiKit.number(amount),
			"cost": UiKit.number(float(result.get("cost", amount * 500.0))),
			"days": UiKit.number(float(result.get("days", 30))),
		})
	else:
		_port_result = I18n.t("port.expand.error", {
			"error": String(result.get("error", "—")),
		})
	_set_text("port.result", _port_result)


# --- Dados -------------------------------------------------------------------

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


func _nation(tag: String) -> Dictionary:
	for nation in _summary.get("nations", []):
		if String(nation.get("tag", "")) == tag:
			return nation
	return {}


func _live_fleet(tag: String) -> Dictionary:
	if Economy.sim == null or not Economy.sim.has_method("get_fleet"):
		return {}
	var result: Dictionary = Economy.sim.get_fleet(tag)
	if result.has("error"):
		return {}
	return result


func _nation_currency(tag: String) -> String:
	for city in _summary.get("cities", []):
		if String(city.get("nation", "")) == tag:
			return String(city.get("currency", ""))
	return ""


func _currency(code: String) -> Dictionary:
	for currency in _summary.get("currencies", []):
		if String(currency.get("code", "")) == code:
			return currency
	return {}


func _nation_tariff_revenue(tag: String) -> float:
	var total := 0.0
	for city in _summary.get("cities", []):
		if String(city.get("nation", "")) == tag:
			total += float(city.get("tariff_revenue", 0.0))
	return total


func _tariffs() -> Array:
	if not _live_tariffs.is_empty():
		return _live_tariffs
	return _summary.get("tariffs", [])


func _first_partner() -> String:
	for order in _summary.get("trade_log", _summary.get("trade_orders", [])):
		if String(order.get("to_nation", "")) == _selected_nation:
			var seller := String(order.get("from_nation", ""))
			if not seller.is_empty():
				return seller
	for nation in _summary.get("nations", []):
		var tag := String(nation.get("tag", ""))
		if not tag.is_empty() and tag != _selected_nation:
			return tag
	return ""


func _good_name_by_id(id: String) -> String:
	var key := "product." + id
	var translated := I18n.t(key)
	if translated != key:
		return translated
	for good in _goods_cache:
		if String(good.get("id", "")) == id:
			return String(good.get("name", id))
	return id


func _good_name(good: Dictionary) -> String:
	return _good_name_by_id(String(good.get("id", "")))


func _port() -> Dictionary:
	for port in _summary.get("ports", []):
		if String(port.get("id", "")) == _selected_port:
			return port
	return {}


func _expansion_amount(port: Dictionary) -> float:
	var capacity := float(port.get("capacity_t_per_day", 0.0))
	return maxf(1000.0, roundf(capacity * _expansion / 1000.0) * 1000.0)


# --- Utilidades --------------------------------------------------------------

static func _throughput(value: float) -> String:
	if value >= 1_000_000.0:
		return UiKit.number(value / 1_000_000.0, 2) + " Mt/d"
	if value >= 1_000.0:
		return UiKit.number(value / 1_000.0, 1) + " kt/d"
	return UiKit.number(value) + " t/d"


static func _money(value: float, currency: String) -> String:
	if value <= 0.0:
		return I18n.t("common.na")
	var text := UiKit.number(value, 1)
	return "%s %s" % [text, currency] if not currency.is_empty() else text


static func _health_color(value: float) -> Color:
	if value >= 0.8:
		return UiKit.POSITIVO
	if value >= 0.5:
		return UiKit.ALERTA
	return UiKit.CARIMBO


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
	if node != null:
		node.visible = not hidden
