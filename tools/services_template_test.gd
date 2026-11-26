extends SceneTree

## Serviços (F8) no T2: abas Setores/Negócios, tabela dos 20 setores com ícone
## do Icon Forge e o selo do setor, lista de negócios com nome gerado pela
## cultura do dono, busca, filtro por setor e estado vazio honesto. Confere
## também o bloco curto de serviços na ficha da cidade.
##
##   godot --headless --path . --script res://tools/services_template_test.gd

const HUD_PATH := "res://core/ui/hud.gd"
## Ordem do catálogo (fallback quando o resumo não publica `service_sectors`).
const SECTORS := ["retail", "wholesale", "food_service", "hospitality_tourism",
		"freight_logistics", "passenger_transport", "finance",
		"insurance_pension", "health", "education", "telecom", "it",
		"media_advertising", "professional_services", "real_estate",
		"construction", "maintenance_repair", "personal_services",
		"leisure_culture", "cleaning_security_waste"]

var _failures: Array[String] = []
var _hud


func _init() -> void:
	_run.call_deferred()


func _run() -> void:
	root.size = Vector2i(1920, 1080)
	var hud_script := load(HUD_PATH) as Script
	if hud_script == null:
		_failures.append("não carregou %s" % HUD_PATH)
		_finish()
		return
	_hud = hud_script.new()
	root.add_child(_hud)

	print("=== services_template_test ===")
	if not await _wait_for_summary():
		_failures.append("economia não publicou resumo antes do teste")
		_finish()
		return

	_hud.open_screen("services")
	await _frames(5)
	var screen: Node = _hud.get("_screens").get("services")
	if screen == null:
		_failures.append("HUD sem tela de serviços")
		_finish()
		return
	if not screen.visible:
		_failures.append("serviços não ficou visível")

	var tabs: Variant = screen.get("_tabs")
	if tabs == null or tabs.pages.size() != 2:
		_failures.append("serviços sem as duas abas de pasta")
	else:
		print("abas=%d (%s/%s)" % [tabs.pages.size(), tabs.buttons[0].text,
				tabs.buttons[1].text])

	var cities: Variant = screen.get("_city_table")
	if cities == null or cities.rows.is_empty():
		_failures.append("tabela de cidades vazia")
	else:
		print("cidades=%d primeira=%s" % [cities.rows.size(), _cell(cities, 0, 0)])

	var sectors: Variant = screen.get("_sector_table")
	if sectors == null or sectors.rows.size() != 20:
		_failures.append("tabela de setores com %d linhas (esperado 20)"
				% (0 if sectors == null else sectors.rows.size()))
	else:
		print("setores=%d primeiro=%s demanda=%s preço=%s" % [sectors.rows.size(),
				_cell(sectors, 0, 0), _cell(sectors, 0, 1), _cell(sectors, 0, 3)])
		_check_sector_icons(screen, sectors)
		_check_sector_values(sectors)
		_check_sector_header(screen)

	var chips: Dictionary = screen.get("_chips")
	if chips.size() != 21:
		_failures.append("filtros de setor com %d chips (esperado 21 = todos + 20)"
				% chips.size())
	else:
		print("chips de setor=%d" % chips.size())

	screen.call("_set_tab", 1)
	await _frames(3)
	var businesses: Variant = screen.get("_business_table")
	if businesses == null:
		_failures.append("aba Negócios sem tabela")
		_finish()
		return
	if businesses.columns.size() != 7:
		_failures.append("negócios com %d colunas (esperado 7: nome, setor, cidade, "
				% businesses.columns.size()
				+ "cultura, empregados, lucro, situação)")
	var rows: Array = screen.get("_business_rows")
	print("negócios=%d nação=%s" % [rows.size(), _text(screen.get("_slots"),
			"services.businesses.scope")])
	_check_businesses(screen, rows)

	screen.call("_set_sector", "retail")
	await _frames(3)
	var retail: Array = screen.get("_business_rows")
	_check_retail_filter(retail)
	screen.call("_set_sector", "all")
	await _frames(2)

	screen.call("_on_business_search", "zzzz")
	await _frames(3)
	var empty: Control = _slot_node(screen.get("_root"), "services.businesses.empty")
	if businesses.rows.is_empty() and (empty == null or not empty.visible):
		_failures.append("busca sem resultado e sem estado vazio honesto")
	if not businesses.rows.is_empty():
		_failures.append("busca sem resultado manteve %d negócios"
				% businesses.rows.size())
	_check_hidden_table(screen)
	screen.call("_on_business_search", "")
	await _frames(3)
	if businesses.rows.is_empty():
		_failures.append("limpar a busca não devolveu os negócios")

	await _check_city_panel(screen)
	_finish()


## Cada setor tem o glifo do Icon Forge de serviços e o selo do setor legível.
func _check_sector_icons(screen: Node, table) -> void:
	var ids := _catalog_ids(screen)
	var paper := Color("#efe6cc")
	var missing: Array[String] = []
	var mixed: Array[String] = []
	var uncolored: Array[String] = []
	var faint: Array[String] = []
	for row in table.rows:
		var cell: Dictionary = row[0]
		var id := String(cell.get("icon", ""))
		if id.is_empty() or ProductIcons.texture(id) == null:
			missing.append(id if not id.is_empty() else "?")
			continue
		if not ProductIcons.icon_path(id).begins_with(ProductIcons.SERVICES_DIR):
			mixed.append(id)
		if not ids.has(id):
			_failures.append("setor fora do catálogo: %s" % id)
		var sector := String(cell.get("sector", ""))
		if not ProductIcons.SECTOR_TINTS.has(sector):
			uncolored.append(id)
			continue
		if ProductIcons.contrast(ProductIcons.INK,
				ProductIcons.seal_background(sector, paper)) < 3.0:
			faint.append(id)
	if not missing.is_empty():
		_failures.append("%d setores sem textura de ícone: %s"
				% [missing.size(), ", ".join(missing.slice(0, 5))])
	if not mixed.is_empty():
		_failures.append("%d setores fora do conjunto de serviços: %s"
				% [mixed.size(), ", ".join(mixed.slice(0, 5))])
	if not uncolored.is_empty():
		_failures.append("%d setores sem cor no selo: %s"
				% [uncolored.size(), ", ".join(uncolored.slice(0, 5))])
	if not faint.is_empty():
		_failures.append("%d selos sem contraste sobre o papel: %s"
				% [faint.size(), ", ".join(faint.slice(0, 5))])
	if missing.is_empty() and mixed.is_empty() and uncolored.is_empty() \
			and faint.is_empty():
		print("ícones=%d no conjunto de serviços, selo do setor com contraste ≥ 3:1"
				% table.rows.size())


## Demanda, oferta, preço, negócios vêm do detalhe do dia; a exportação só
## aparece para setor exportável (o resto é "—", não zero). Emprego por setor não
## é coluna: o núcleo não o publica (o cabeçalho mostra o total da cidade).
func _check_sector_values(table) -> void:
	var blank: Array[String] = []
	var exporting := 0
	for index in table.rows.size():
		for column in [1, 2, 3, 4]:
			if _cell(table, index, column).is_empty():
				blank.append("%d/%d" % [index, column])
		if _cell(table, index, 5) != _t("common.na"):
			exporting += 1
	if not blank.is_empty():
		_failures.append("%d células vazias na tabela de setores: %s"
				% [blank.size(), ", ".join(blank.slice(0, 5))])
	else:
		print("valores por setor completos; setores com exportação=%d" % exporting)


## O cabeçalho da cidade publica o emprego de serviços somado — o único número de
## emprego que o detalhe realmente abre por cidade.
func _check_sector_header(screen: Node) -> void:
	var text := _text(screen.get("_slots"), "services.sectors.city")
	if text.is_empty() or text == _t("services.nation.none"):
		_failures.append("cabeçalho dos setores sem cidade selecionada")
		return
	if text.contains("{"):
		_failures.append("cabeçalho dos setores com chave de i18n crua: " + text)
		return
	var digits := RegEx.create_from_string("[0-9]")
	if digits.search(text) == null:
		_failures.append("cabeçalho dos setores sem números publicados: " + text)
		return
	print("cabeçalho da cidade=%s" % text)


## Negócios: nome único gerado pela cultura do dono, setor do catálogo, cidade,
## cultura, empregados, lucro e situação coerente com `loss_days`.
func _check_businesses(screen: Node, rows: Array) -> void:
	if rows.is_empty():
		_failures.append("nenhum negócio publicado na nação")
		return
	var ids := _catalog_ids(screen)
	var used := {}
	var empty_name := 0
	var bad_sector := 0
	var bad_status := 0
	var repeated := 0
	var with_loss := 0
	for item in rows:
		var row: Dictionary = item
		var name := String(row.get("name", ""))
		if name.is_empty():
			empty_name += 1
			continue
		var key := name.to_lower()
		if used.has(key):
			repeated += 1
		used[key] = true
		if not ids.has(String(row.get("sector", ""))):
			bad_sector += 1
		if String(row.get("city", "")).is_empty():
			bad_sector += 1
		if String(row.get("culture_name", "")).is_empty():
			bad_sector += 1
		if int(row.get("loss_days", 0)) > 0:
			with_loss += 1
		var status_text := _t("services.status.closing",
				{"days": int(row.get("loss_days", 0))}) \
				if int(row.get("loss_days", 0)) > 0 else _t("services.status.open")
		if status_text.is_empty() or status_text.contains("{"):
			bad_status += 1
	if empty_name > 0:
		_failures.append("%d negócios sem nome gerado" % empty_name)
	if repeated > 0:
		_failures.append("%d nomes repetidos na lista" % repeated)
	if bad_sector > 0:
		_failures.append("%d negócios sem setor/cidade/cultura" % bad_sector)
	if bad_status > 0:
		_failures.append("%d situações sem rótulo" % bad_status)
	if empty_name == 0 and repeated == 0 and bad_sector == 0 and bad_status == 0:
		print("nomes únicos=%d; setores no catálogo; fechando=%d" % [used.size(),
				with_loss])


func _check_retail_filter(rows: Array) -> void:
	if rows.is_empty():
		_failures.append("filtro de setor não devolveu negócios")
		return
	var wrong := 0
	for item in rows:
		if String((item as Dictionary).get("sector", "")) != "retail":
			wrong += 1
	if wrong > 0:
		_failures.append("filtro do setor manteve %d negócios de outro setor" % wrong)
	else:
		print("filtro de setor: %d negócios de varejo" % rows.size())


## Com a busca sem resultado a tabela some e o cartão de vazio aparece.
func _check_hidden_table(screen: Node) -> void:
	if _text(screen.get("_slots"), "services.businesses.count").is_empty():
		_failures.append("contador de negócios vazio")


## A ficha da cidade mostra o bloco curto de serviços com números publicados.
func _check_city_panel(screen: Node) -> void:
	var city_id := String(screen.call("current_city"))
	if city_id.is_empty():
		_failures.append("tela de serviços sem cidade selecionada")
		return
	_hud.open_city(city_id, "overview")
	await _frames(4)
	var panel: Node = _hud.get("_city_screen")
	if panel == null or not panel.visible:
		_failures.append("ficha da cidade não abriu")
		return
	var slots: Dictionary = panel.get("_slots")
	var jobs := _text(slots, "ov.services.jobs")
	var workers := _text(slots, "ov.services.workers")
	var wage := _text(slots, "ov.services.wage")
	var profit := _text(slots, "ov.services.profit")
	print("ficha serviços: vagas=%s empregados=%s salário=%s lucro=%s" % [jobs,
			workers, wage, profit])
	if jobs.is_empty() or jobs == _t("common.na"):
		_failures.append("ficha sem vagas de serviço publicadas")
	if workers.is_empty() or wage.is_empty() or profit.is_empty():
		_failures.append("ficha com bloco de serviços incompleto")


func _catalog_ids(screen: Node) -> Dictionary:
	var out := {}
	for entry in _summary().get("service_sectors", []):
		out[String((entry as Dictionary).get("id", ""))] = true
	if out.is_empty():
		_failures.append("resumo sem catálogo service_sectors")
		for id in SECTORS:
			out[String(id)] = true
	return out


## Autoloads não são identificadores resolvíveis num script `--script`: o
## teste pega os nós do root (mesma solução de `tools/city_tooltip_test.gd`).
func _autoload(name: String) -> Node:
	return root.get_node_or_null(name)


func _t(key: String, params: Dictionary = {}) -> String:
	var i18n := _autoload("I18n")
	if i18n == null:
		return key
	return String(i18n.call("t", key, params))


func _summary() -> Dictionary:
	var economy := _autoload("Economy")
	if economy == null:
		return {}
	# A4: retrato sob demanda das telas (o resumo do dia é leve).
	var summary: Dictionary = economy.call("ui_snapshot")
	return summary


func _cell(table, row: int, column: int) -> String:
	if row >= table.rows.size():
		return ""
	var cells: Array = table.rows[row]
	if column >= cells.size():
		return ""
	var cell: Variant = cells[column]
	return String(cell.get("text", "")) if cell is Dictionary else str(cell)


func _text(slots: Dictionary, slot: String) -> String:
	var node := slots.get(slot) as Label
	return node.text if node != null else ""


func _slot_node(source: Node, slot: String) -> Control:
	if source == null:
		return null
	var pending: Array[Node] = [source]
	while not pending.is_empty():
		var node: Node = pending.pop_back()
		if node is Control and (node as Control).get_meta("tpl_slot", "") == slot:
			return node as Control
		for child in node.get_children():
			pending.append(child)
	return null


func _frames(count: int) -> void:
	for _frame in count:
		await process_frame


## A economia constrói o mundo no `_ready` do autoload e o PRIMEIRO dia roda no
## worker do tick (~1,9 s/dia no debug com o mundo real). Em headless o
## `process_frame` corre muito mais rápido que esse tick, então os 600 frames
## antigos venciam antes do resumo (a semeadura de 271 plantas da G2 alonga o
## dia 0). O teto agora é de TEMPO DE PAREDE (120 s), só deste teste: o
## resultado não depende do FPS do headless. O runner mantém o timeout próprio.
func _wait_for_summary() -> bool:
	var deadline := Time.get_ticks_msec() + 120000
	while Time.get_ticks_msec() < deadline:
		var eco := root.get_node_or_null("Economy")
		if eco != null and not (eco.get("last_summary") as Dictionary).is_empty():
			return true
		await process_frame
	return false


func _finish() -> void:
	_hud.queue_free()
	if _failures.is_empty():
		print("PASS: serviços com 20 setores, ícones, nomes por cultura e ficha")
		quit(0)
		return
	for failure in _failures:
		push_error("[services_template_test] " + failure)
	quit(1)
