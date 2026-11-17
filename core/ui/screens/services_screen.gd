extends Control

## Serviços (F8) no template T2 (Registro da Papelaria).
##
## Aba **Setores**: lista de cidades à esquerda (busca, nação e o resumo de
## serviços da nação) e a tabela dos 20 setores da cidade escolhida à direita —
## ícone, demanda, oferta, preço local, negócios e exportação. O emprego de
## serviços da cidade aparece no cabeçalho, e não por setor: `sectors[].jobs` do
## núcleo é o total da cidade rateado pela capacidade das empresas (ver
## `docs/ui/README.md`, frente F8), então repetiria o mesmo número em cada linha.
## Aba **Negócios**: os negócios abertos na nação da cidade escolhida, com o
## nome gerado pela cultura do dono, setor, cidade, cultura, empregados,
## lucro/prejuízo e situação (aberto/fechando), com busca, filtro por setor e
## paginação que preenche a altura.
##
## Fonte dos números: `get_status()` (cidades, contadores e o catálogo
## `service_sectors`) e `get_services_detail(city_id)` (setores e negócios
## do dia). O nome do negócio NÃO vem da simulação: é gerado aqui por
## `core/culture/business_name_generator.gd` a partir de setor + `name_seed` +
## cultura do dono, como `tools/services_test.gd` faz — nada disso volta para o
## núcleo. Busca, filtro, aba e seleção são estado de exibição.

signal city_requested(city_id: String)
signal close_requested

const SPEC_ID := "servicos"
const TAB_SECTORS := 0
const TAB_BUSINESSES := 1
const ALL := "all"
## Ordem do catálogo `services.json` (usada só como fallback do contrato).
const SECTORS := ["retail", "wholesale", "food_service", "hospitality_tourism",
		"freight_logistics", "passenger_transport", "finance",
		"insurance_pension", "health", "education", "telecom", "it",
		"media_advertising", "professional_services", "real_estate",
		"construction", "maintenance_repair", "personal_services",
		"leisure_culture", "cleaning_security_waste"]
const NAME_GENERATOR: Script = preload("res://core/culture/business_name_generator.gd")
const CULTURE_DB: Script = preload("res://core/culture/culture_db.gd")
## Cultura de reserva para o nome quando a cidade não publica mistura (mesma
## regra de `tools/services_test.gd`): a ausência é declarada, não inventada.
const NAME_FALLBACK_CULTURE := "americano"

var _summary: Dictionary = {}
var _tab := TAB_SECTORS
var _city_query := ""
var _business_query := ""
var _sector := ALL
var _selected_city := ""

var _root: UiTemplateBuilder.TemplateRoot
var _slots := {}
var _tabs: UiTemplateBuilder.FolderTabs
var _city_table: UiTemplateBuilder.DataTableView
var _sector_table: UiTemplateBuilder.DataTableView
var _business_table: UiTemplateBuilder.DataTableView
var _city_search: LineEdit
var _business_search: LineEdit
var _chips := {}
var _structure_key := ""

## Linhas visíveis (o índice da linha da tabela aponta para estas listas).
var _city_rows: Array = []
var _sector_rows: Array = []
var _business_rows: Array = []

## Cache por dia: o detalhe de uma cidade custa ~0,2 ms e só muda no dia novo.
var _day := -1
var _details := {}
var _nation_rows := {}
var _generator: RefCounted = null


func _ready() -> void:
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	CULTURE_DB.load_data()


func uses_window_shell() -> bool:
	return true


# --- API ---------------------------------------------------------------------

func refresh(summary: Dictionary) -> void:
	_summary = summary
	if _day != int(summary.get("day", -1)):
		_day = int(summary.get("day", -1))
		_details.clear()
		_nation_rows.clear()
	if _root == null or _structure_key != I18n.locale:
		_rebuild()
		return
	_refresh()


## Captura/testes: 0/"sectors" ou 1/"businesses".
func _set_tab(id: Variant) -> void:
	var index := TAB_BUSINESSES if (str(id) == "businesses" \
			or (id is int and int(id) == TAB_BUSINESSES)) else TAB_SECTORS
	_tab = index
	if _tabs != null:
		_tabs.select(index)
	_refresh()


func current_tab() -> int:
	return _tab


func current_city() -> String:
	return _selected_city


func current_sector() -> String:
	return _sector


## Seleciona uma cidade (captura/testes) e mantém tabelas e cartão coerentes.
func select_city(id: String) -> void:
	if id.is_empty():
		return
	_selected_city = id
	_refresh_cities()
	_refresh_nation()
	_refresh_sectors()
	if _tab == TAB_BUSINESSES:
		_refresh_businesses()


# --- Montagem ----------------------------------------------------------------

func _rebuild() -> void:
	_structure_key = I18n.locale
	if _root != null and is_instance_valid(_root):
		remove_child(_root)
		_root.queue_free()
	_slots.clear()
	_tabs = null
	_city_table = null
	_sector_table = null
	_business_table = null
	_city_search = null
	_business_search = null
	_chips.clear()

	_root = UiTemplateBuilder.build(UiTemplateBuilder.load_spec(SPEC_ID))
	_root.name = "ServicesRegistry"
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

	_city_table = _slots.get("services.cities") as UiTemplateBuilder.DataTableView
	if _city_table != null:
		_city_table.row_selected.connect(_on_city_selected)
		_city_table.row_activated.connect(_on_city_activated)
	_sector_table = _slots.get("services.sectors") as UiTemplateBuilder.DataTableView
	if _sector_table != null:
		_sector_table.row_activated.connect(_on_sector_activated)
	_business_table = _slots.get("services.businesses") as UiTemplateBuilder.DataTableView
	if _business_table != null:
		_business_table.row_selected.connect(_on_business_selected)
		_business_table.row_activated.connect(_on_business_activated)

	_city_search = _line_edit("services.city_search")
	if _city_search != null:
		_city_search.text_changed.connect(_on_city_search)
	_business_search = _line_edit("services.business_search")
	if _business_search != null:
		_business_search.text_changed.connect(_on_business_search)

	for id in [ALL] + SECTORS:
		var chip := _slots.get("services.filter.%s" % id) as Button
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
	_tab = clampi(index, TAB_SECTORS, TAB_BUSINESSES)
	_refresh()


func _on_city_search(text: String) -> void:
	_city_query = text.strip_edges()
	_refresh_cities()
	if _city_table != null:
		_city_table.reset_page()


func _on_business_search(text: String) -> void:
	_business_query = text.strip_edges()
	_refresh_businesses()
	if _business_table != null:
		_business_table.reset_page()


func _set_sector(id: String) -> void:
	if _sector == id:
		return
	_sector = id
	_refresh_businesses()
	if _business_table != null:
		_business_table.reset_page()


func _on_city_selected(index: int) -> void:
	if index < 0 or index >= _city_rows.size():
		return
	var id := String(_city_rows[index].get("id", ""))
	if id == _selected_city:
		return
	_selected_city = id
	_refresh_nation()
	_refresh_sectors()
	if _tab == TAB_BUSINESSES:
		_refresh_businesses()


func _on_city_activated(index: int) -> void:
	_on_city_selected(index)
	if not _selected_city.is_empty():
		city_requested.emit(_selected_city)


## Duplo clique (ou Enter) no setor: filtra os negócios por ele.
func _on_sector_activated(index: int) -> void:
	if index < 0 or index >= _sector_rows.size():
		return
	_sector = String(_sector_rows[index].get("id", ALL))
	_set_tab(TAB_BUSINESSES)


func _on_business_selected(index: int) -> void:
	if index < 0 or index >= _business_rows.size():
		return
	var city_id := String(_business_rows[index].get("city", ""))
	if city_id.is_empty() or city_id == _selected_city:
		return
	_selected_city = city_id
	_refresh_cities()
	_refresh_nation()
	_refresh_sectors()


func _on_business_activated(index: int) -> void:
	_on_business_selected(index)
	var city_id := String(_business_rows[index].get("city", "")) \
			if index >= 0 and index < _business_rows.size() else ""
	if not city_id.is_empty():
		city_requested.emit(city_id)


# --- Desenho -----------------------------------------------------------------

func _refresh() -> void:
	if _root == null:
		return
	_set_text("nav.title", I18n.t("services.title"))
	_refresh_cities()
	_refresh_nation()
	if _tab == TAB_BUSINESSES:
		_refresh_businesses()
	else:
		_refresh_sectors()


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
			{"text": _city_name(city), "size": 15},
			{"text": I18n.nation_name(String(city.get("nation", ""))), "size": 15},
			{"text": _millions(float(city.get("population", 0.0))), "size": 15},
		])
	var index := _selected_index(rows, _selected_city)
	if index < 0 and not rows.is_empty():
		index = 0
	_city_table.update_rows(cells, index)
	if index >= 0:
		_selected_city = String(rows[index].get("id", ""))
	_set_text("services.cities.count", I18n.t("services.cities.count", {
		"shown": UiKit.number(rows.size()),
		"total": UiKit.number(all.size()),
	}))


## Resumo da nação só com o que o status publica (sem chamar o detalhe).
func _refresh_nation() -> void:
	var city := _city()
	if city.is_empty():
		_set_text("services.nation.name", I18n.t("services.nation.none"))
		return
	var tag := String(city.get("nation", ""))
	var workers := 0.0
	var wage_total := 0.0
	var revenue := 0.0
	var profit := 0.0
	for other in _summary.get("cities", []):
		if String(other.get("nation", "")) != tag:
			continue
		var city_workers := float(other.get("service_workers", 0.0))
		workers += city_workers
		wage_total += float(other.get("service_wage", 0.0)) * city_workers
		revenue += float(other.get("service_revenue", 0.0))
		profit += float(other.get("service_profit", 0.0))
	_set_text("services.nation.name", I18n.nation_name(tag))
	_set_text("services.nation.workers", UiKit.number(workers))
	_set_text("services.nation.wage",
			UiKit.number(wage_total / workers, 3) if workers > 0.0 \
			else I18n.t("common.na"))
	_set_text("services.nation.revenue", _money(revenue))
	_set_text("services.nation.profit", _money(profit),
			UiKit.CARIMBO if profit < 0.0 else Color(0, 0, 0, 0))


func _refresh_sectors() -> void:
	if _sector_table == null:
		return
	var city := _city()
	var detail := _detail(String(city.get("id", "")))
	var sectors: Array = detail.get("sectors", [])
	var rows: Array = []
	var cells: Array = []
	for entry in sectors:
		var id := String((entry as Dictionary).get("id", ""))
		if id.is_empty():
			continue
		var exportable := _is_exportable(id)
		var exports := float((entry as Dictionary).get("exports", 0.0))
		rows.append({"id": id})
		cells.append([
			{
				"text": _sector_name(id),
				"icon": id,
				"sector": id,
				"size": 15,
			},
			{"text": _number((entry as Dictionary).get("demand", 0.0)),
					"tone": "tinta"},
			{"text": _number((entry as Dictionary).get("supply", 0.0)),
					"tone": "suave"},
			{"text": _price((entry as Dictionary).get("price", 0.0)),
					"tone": "tinta"},
			{"text": _number((entry as Dictionary).get("businesses", 0.0))},
			{"text": _number(exports) if exportable else I18n.t("common.na"),
					"tone": "tinta" if exports > 0.0 else "suave"},
		])
	_sector_rows = rows
	# Sem seleção fixa: a tabela de setores ordena por demanda e o duplo clique
	# leva o setor para a aba de negócios.
	_sector_table.update_rows(cells, -1)
	_set_hidden("services.sectors.empty", not rows.is_empty())
	_refresh_sectors_city(city, detail, rows.size())


## Cabeçalho da cidade: nome e o emprego de serviços publicado para a cidade
## inteira. O núcleo não abre esse emprego por setor (ver contrato no README), então
## a tabela não repete o total em cada linha.
func _refresh_sectors_city(city: Dictionary, detail: Dictionary, shown: int) -> void:
	if city.is_empty():
		_set_text("services.sectors.city", I18n.t("services.nation.none"))
		return
	var parts: Array[String] = [_city_name(city)]
	if shown > 0:
		parts.append(I18n.t("services.sectors.city.workers", {
			"workers": _number(float(city.get("service_workers", 0.0))),
		}))
		parts.append(I18n.t("services.sectors.city.businesses", {
			"businesses": _number(float(detail.get("service_businesses",
					city.get("service_businesses", 0.0)))),
		}))
	else:
		parts.append(I18n.t("services.sectors.city.none"))
	_set_text("services.sectors.city", " · ".join(parts))


func _refresh_businesses() -> void:
	if _business_table == null:
		return
	for id in _chips:
		UiTemplateBuilder.set_chip_active(_chips[id], id == _sector)
	var city := _city()
	var tag := String(city.get("nation", ""))
	var rows := _filtered_businesses(_nation_businesses_of(tag))
	_business_rows = rows
	var cells: Array = []
	for item in rows:
		var profit := float(item.get("profit", 0.0))
		var loss_days := int(item.get("loss_days", 0))
		cells.append([
			{"text": String(item.get("name", "")), "size": 15},
			{"text": String(item.get("sector_name", "")), "size": 15},
			{"text": String(item.get("city_name", "")), "size": 15,
					"tone": "tinta" if String(item.get("city", "")) \
					== _selected_city else "suave"},
			{"text": String(item.get("culture_name", "")), "size": 14,
					"chip": String(item.get("culture_color", ""))},
			{"text": UiKit.number(float(item.get("workers", 0.0)))},
			{"text": _money(profit),
					"tone": "carimbo" if profit < 0.0 else "positive"},
			{
				"text": I18n.t("services.status.closing", {"days": loss_days}) \
						if loss_days > 0 else I18n.t("services.status.open"),
				"tone": "carimbo" if loss_days > 0 else "suave",
			},
		])
	_business_table.update_rows(cells, _selected_index(rows, ""))
	_set_text("services.businesses.count", I18n.t("services.businesses.count", {
		"shown": UiKit.number(rows.size()),
		"total": UiKit.number(_nation_businesses_of(tag).size()),
	}))
	_set_text("services.businesses.scope", I18n.nation_name(tag) if not tag.is_empty() \
			else I18n.t("services.nation.none"))
	_set_hidden("services.businesses", rows.is_empty())
	_set_hidden("services.businesses.empty", not rows.is_empty())


# --- Dados -------------------------------------------------------------------

func _catalog() -> Array:
	return _summary.get("service_sectors", [])


func _catalog_entry(id: String) -> Dictionary:
	for entry in _catalog():
		if String((entry as Dictionary).get("id", "")) == id:
			return entry
	return {}


func _sector_name(id: String) -> String:
	var key := "services.sector." + id
	var text := I18n.t(key)
	if text != key:
		return text
	var entry := _catalog_entry(id)
	var name := String(entry.get("name_%s" % _locale_suffix(), ""))
	if not name.is_empty():
		return name
	return id


func _is_exportable(id: String) -> bool:
	return bool(_catalog_entry(id).get("exportable", false))


## Detalhe da cidade (setores + negócios) com cache por dia.
func _detail(city_id: String) -> Dictionary:
	if city_id.is_empty():
		return {}
	if _details.has(city_id):
		return _details[city_id]
	var economy := get_node_or_null("/root/Economy")
	var sim: Object = economy.get("sim") if economy != null else null
	if sim == null:
		return {}
	var detail: Dictionary = sim.get_services_detail(city_id)
	_details[city_id] = detail
	return detail


## Linhas da aba Negócios da nação (nome gerado uma vez por dia).
func _nation_businesses_of(tag: String) -> Array:
	if tag.is_empty():
		return []
	var key := "%s|%d" % [tag, _day]
	if _nation_rows.has(key):
		return _nation_rows[key]
	var rows: Array = []
	var cities: Array = []
	for city in _summary.get("cities", []):
		if String(city.get("nation", "")) == tag:
			cities.append(city)
	cities.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return String(a.get("id", "")) < String(b.get("id", "")))
	var generator := _name_generator()
	generator.reset()
	for city in cities:
		var city_id := String(city.get("id", ""))
		var place := String(city.get("name", ""))
		var culture_id := _city_culture(city_id)
		var culture_name: String = CULTURE_DB.culture_name(culture_id) \
				if not culture_id.is_empty() else I18n.t("common.na")
		var culture_color: Color = CULTURE_DB.culture_color(culture_id) \
				if not culture_id.is_empty() else UiKit.TINTA_SUAVE
		for business in _detail(city_id).get("businesses", []):
			var sector := String((business as Dictionary).get("sector", ""))
			var seed := int(float((business as Dictionary).get("name_seed", 0.0)))
			var name: String = generator.generate(culture_id, sector, place, seed)
			if name.is_empty():
				name = generator.generate(NAME_FALLBACK_CULTURE, sector, place, seed)
			rows.append({
				"id": int((business as Dictionary).get("id", 0)),
				"name": name,
				"city": city_id,
				"city_name": _city_name(city),
				"sector": sector,
				"sector_name": _sector_name(sector),
				"culture_name": culture_name,
				"culture_color": culture_color.to_html(false),
				"workers": float((business as Dictionary).get("workers", 0.0)),
				"profit": float((business as Dictionary).get("profit", 0.0)),
				"loss_days": int((business as Dictionary).get("loss_days", 0)),
			})
	_nation_rows[key] = rows
	return rows


func _name_generator() -> RefCounted:
	if _generator == null:
		_generator = NAME_GENERATOR.new()
	return _generator


## Cultura do dono: a primeira da mistura publicada da cidade ("" quando não há).
func _city_culture(city_id: String) -> String:
	var mix: Array = CULTURE_DB.city_mix(city_id)
	if mix.is_empty():
		return ""
	return String((mix[0] as Dictionary).get("id", ""))


func _city() -> Dictionary:
	for city in _summary.get("cities", []):
		if String((city as Dictionary).get("id", "")) == _selected_city:
			return city
	return {}


func _filtered_cities(cities: Array) -> Array:
	if _city_query.is_empty():
		return cities
	var needle := _city_query.to_lower()
	var result: Array = []
	for city in cities:
		var name := _city_name(city).to_lower()
		var nation := I18n.nation_name(String((city as Dictionary).get("nation", ""))).to_lower()
		if name.contains(needle) or nation.contains(needle):
			result.append(city)
	return result


func _filtered_businesses(rows: Array) -> Array:
	var result: Array = []
	var needle := _business_query.to_lower()
	for item in rows:
		var row: Dictionary = item
		if _sector != ALL and String(row.get("sector", "")) != _sector:
			continue
		if not needle.is_empty():
			var haystack := "%s %s %s %s" % [
				String(row.get("name", "")).to_lower(),
				String(row.get("sector_name", "")).to_lower(),
				String(row.get("city_name", "")).to_lower(),
				String(row.get("culture_name", "")).to_lower(),
			]
			if not haystack.contains(needle):
				continue
		result.append(row)
	return result


func _city_name(city: Dictionary) -> String:
	return I18n.city_name(String(city.get("id", "")), String(city.get("name", "")))


static func _locale_suffix() -> String:
	if I18n.locale == "en":
		return "en"
	if I18n.locale == "es":
		return "es"
	return "pt"


# --- Formatação --------------------------------------------------------------

static func _number(value: float) -> String:
	return UiKit.number(value)


static func _price(value: float) -> String:
	return UiKit.number(value, 2) if value > 0.0 else I18n.t("common.na")


## Dinheiro do setor: o núcleo publica o valor na moeda-âncora (XGS).
static func _money(value: float) -> String:
	return UiKit.number(value, 1)


static func _millions(value: float) -> String:
	return UiKit.number(value / 1_000_000.0, 1) + " M"


# --- Utilidades --------------------------------------------------------------

func _selected_index(rows: Array, id: String) -> int:
	if id.is_empty():
		return -1
	for index in rows.size():
		if String((rows[index] as Dictionary).get("id", "")) == id:
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
