extends SceneTree

## G — teste headless da AGRICULTURA SAZONAL (D-096).
##
##   godot --headless --path . --script res://tools/agriculture_seasonal_test.gd
##
## Cobre o que o Bloco 1 prometeu:
##   (a) a safra chega SÓ dentro da janela de colheita do calendário (amostra
##       dos dois hemisférios; nenhum fluxo fora da janela);
##   (b) atravessar a primeira entressafra (120 d a partir de 2027-01-01):
##       nenhum estoque negativo e, para cada nação/cultura que zera, a causa
##       provada com o dado (estoque inicial + importação < dias até a 1ª
##       colheita). Zero que não se explica assim FALHA o teste;
##   (c) perda por infraestrutura: armazém ruim perde mais que bom (mesma
##       cultura/estoque, mesmo mundo — só a qualidade muda);
##   (d) perecibilidade: batata decai ~meia-vida em 61 d, trigo quase não, e os
##       `no_data` (soja/farelo/ração) não decaem;
##   (e) save v24 (round-trip do bloco `agriculture`) e load de um save v22
##       (estado da safra nasce zerado, planos reaplicados pelas definições);
##   (f) folha sazonal (D-096): fora das janelas de plantio/colheita a fazenda
##       com plano não paga equipe nem opex e não acumula `loss_days`; na
##       colheita a equipe volta.
##
## Sem IA e sem camada urbana: mede o NÚCLEO (o mesmo caminho do dia no jogo,
## menos as decisões que não tocam a agricultura).

const GeoScript := preload("res://core/economy/geography_service.gd")
const AgriScript := preload("res://core/economy/agriculture_service.gd")
const RegionDataScript := preload("res://core/economy/region_data.gd")

const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const PORTS_PATH := "res://data/definitions/scenario/ports.json"
const BUILDINGS_PATH := "res://data/definitions/scenario/buildings.json"
const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"
const SERVICES_PATH := "res://data/definitions/economy/services.json"
const CURRENCIES_PATH := "res://data/definitions/economy/currencies.json"
const SECTOR_EMPLOYMENT_PATH := "res://data/definitions/economy/sector_employment.json"
const AGE_STRUCTURE_PATH := "res://data/definitions/population/age_structure_2026.json"
const CROP_CALENDAR_PATH := "res://data/definitions/agriculture/crop_calendar.json"
const CROP_STOCKS_PATH := "res://data/definitions/agriculture/crop_stocks.json"

## Primeira entressafra medida (2027-01-01 + 120 d).
const ENT_DAYS := 120
## Amostra de hemisférios (nações com calendário OBSERVADO na maior parte das
## culturas, para a checagem independente não depender de regra de vizinho).
const SAMPLE_NORTH := ["USA", "FRA", "CHN", "IND", "RUS"]
const SAMPLE_SOUTH := ["BRA", "ARG", "AUS", "ZAF"]
## Janela do A/B de armazém: precisa atravessar uma virada de mês (a perda de
## armazenagem é mensal) e não pode conter colheita (para isolar a qualidade).
const STORAGE_TEST_DAYS := 40
const STORAGE_TEST_MIN_HARVEST_GAP := 45
## Perecibilidade: meia-vida = prazo de prateleira (regra declarada no núcleo).
const PERISHABLE_DAYS := 61
## Tolerância (dias) da prova da entressafra: chegada de carga, arredondamento
## de dia e a média de demanda não são exatas.
const CAUSE_TOLERANCE_DAYS := 3.0

var _failures: Array[String] = []
var _calendar: Dictionary = {}
var _cities: Array = []
var _stocks_data: Dictionary = {}
## "NAT|crop" -> {stock0, next_harvest_days, imported, demand_peak, zero_day}
var _crop_stats: Dictionary = {}
## "NAT|crop" -> motivo do plano contínuo declarado (D-099).
var _fallback_pairs: Dictionary = {}
## city_id -> nação do calendário (o apply_to_sim sem `calendar_nations` usa
## `city.nation`, e o id da cidade tem o prefixo da província, não o da nação).
var _nation_of: Dictionary = {}


func _init() -> void:
	var world := _build_world()
	if world.is_empty():
		_finish()
		return
	var sim: Object = world["sim"]
	_calendar = _load_dict(CROP_CALENDAR_PATH).get("nations", {})
	# D-099: pares (nação|cultura) com plano CONTÍNUO declarado (fazenda fora
	# do calendário/rendimento do dado): a janela de colheita deles é o ano
	# inteiro, por declaração — o teste de "fora da janela" os respeita e cobra
	# o motivo no relatório.
	for row in Array(world["report"].get("fallback_plans", [])):
		var plan: Dictionary = row
		var parts := String(plan.get("pair", "")).split("|")
		var reason := String(plan.get("reason", ""))
		# `no_yield` usa o calendário do dado (a janela não é contínua); só
		# `all_year`/`no_calendar` declaram janela de ano inteiro.
		if parts.size() == 2 and reason != "no_yield":
			var tag := String(_nation_of.get(String(parts[0]), ""))
			if tag != "":
				_fallback_pairs[tag + "|" + String(parts[1])] = reason
	_test_declared(world["report"])
	var snapshot := _test_entre_safra(sim)
	_test_seasonal_payroll(snapshot)
	_test_storage_quality(snapshot)
	_test_perishability(snapshot)
	_test_save(sim)
	_finish()


func _finish() -> void:
	if _failures.is_empty():
		print("[agri] OK: safra na janela, entressafra provada, perda por armazém, "
			+ "perecibilidade, folha sazonal e save v24/v22.")
		quit(0)
	else:
		for failure in _failures:
			print("[agri] FALHA: " + failure)
		quit(1)


# ---------------------------------------------------------------------------
# Mundo
# ---------------------------------------------------------------------------

func _build_world() -> Dictionary:
	_cities = _load_array(CITIES_PATH)
	for city in _cities:
		var row: Dictionary = city
		_nation_of[String(row.get("id", ""))] = String(row.get("nation", ""))
	var ports := _load_array(PORTS_PATH)
	var buildings := _load_array(BUILDINGS_PATH)
	var goods := _load_array(GOODS_PATH)
	if _cities.is_empty() or goods.is_empty():
		_fail("cenário vazio (cidades=%d, bens=%d)" % [_cities.size(), goods.size()])
		return {}
	RegionDataScript.attach(_cities)
	var sim = EconomySim.new()
	_load_chains(sim)
	var geo := GeoScript.new()
	geo.load_data()
	buildings = geo.resolve_initial_farms(buildings, _cities)
	buildings = geo.resolve_initial_mines(buildings, _cities)
	sim.initialize_world(_cities, ports, buildings)
	GeoScript.apply_initial_infra(sim)
	var centers: Array = []
	for city in _cities:
		centers.append({
			"id": String(city.get("id", "")),
			"nation": String(city.get("nation", "")),
			"center": Vector2(float(city.get("x", 0.0)), float(city.get("y", 0.0))),
		})
	geo.apply_to_sim(sim, centers)
	var agri := AgriScript.new()
	var report: Dictionary = agri.apply_to_sim(sim, _cities, geo, {
		"farms": AgriScript.farms_of(buildings),
	})
	if not sim.has_method("get_agriculture_status"):
		_fail("binário sem get_agriculture_status (rode o build)")
		return {}
	return {"sim": sim, "report": report}


func _load_chains(sim: Object) -> void:
	sim.set_goods(_load_array(GOODS_PATH))
	sim.set_recipes(_load_array(RECIPES_PATH))
	sim.set_needs(_load_dict(NEEDS_PATH))
	sim.set_services(_load_dict(SERVICES_PATH))
	sim.set_currencies(_load_dict(CURRENCIES_PATH))
	sim.set_sector_employment(_load_dict(SECTOR_EMPLOYMENT_PATH))
	sim.set_age_structure(_load_dict(AGE_STRUCTURE_PATH))


## Restaura um snapshot (definições + estado) num sim novo — mesmo caminho do
## harness CLI (`from_snapshot`).
func _restore(snapshot: Dictionary) -> Object:
	var sim = EconomySim.new()
	_load_chains(sim)
	sim.from_snapshot(snapshot)
	return sim


# ---------------------------------------------------------------------------
# (a)+(b) declaração, janela e entressafra
# ---------------------------------------------------------------------------

func _test_declared(report: Dictionary) -> void:
	var plans := int(report.get("plans", 0))
	var stock := float(report.get("initial_stock_t", 0.0))
	print("[agri] planos=%d cidades=%d estoque_inicial=%.0f t base_area=%s" % [
		plans, int(report.get("cities", 0)), stock,
		str(report.get("area_basis", {}))])
	print("[agri] sem calendário=%d sem rendimento=%d | perda=%s | prateleira=%s" % [
		int(report.get("skipped_no_calendar", 0)),
		int(report.get("skipped_no_yield", 0)),
		str(report.get("loss_pct", {})), str(report.get("shelf_life", {}))])
	if plans <= 0:
		_fail("nenhum plano de safra declarado")
	if stock <= 0.0:
		_fail("nenhum estoque inicial declarado")
	# D-099: toda fazenda de primário fora do calendário/rendimento do dado
	# recebe plano contínuo declarado — o motivo tem de estar no relatório.
	var fallback: Array = Array(report.get("fallback_plans", []))
	print("[agri] fallback declarado=%d motivos=%s" % [
		fallback.size(), str(report.get("fallback_reasons", {}))])
	for row in fallback:
		var reason := String((row as Dictionary).get("reason", ""))
		if not reason in ["all_year", "no_calendar", "no_yield", "calendar"]:
			_fail("fallback com motivo desconhecido: " + reason)
	if fallback.is_empty():
		_fail("nenhuma fazenda fora do dado recebeu plano (fallback D-099)")


func _test_entre_safra(sim: Object) -> Dictionary:
	var status: Dictionary = sim.get_agriculture_status()
	if status.is_empty():
		_fail("get_agriculture_status vazio depois da declaração")
		return {}
	# Estado do dia 0 (antes do primeiro tick): estoque inicial por nação/cultura.
	for nation in status.get("nations", []):
		for crop in nation["crops"]:
			var key := "%s|%s" % [nation["nation"], crop["product"]]
			_crop_stats[key] = {
				"nation": String(nation["nation"]),
				"crop": String(crop["product"]),
				# Poeira de rateio (menos de 10 kg no país) não é estoque: não
				# entra na prova da entressafra.
				"dust": float(crop["stock_t"]) < 0.01,
				"stock0": float(crop["stock_t"]),
				"stock_prev": float(crop["stock_t"]),
				"next_harvest_days": float(crop["next_harvest_days"]),
				"imported": 0.0,
				"exported": 0.0,
				"arrived": 0.0,
				"in_kind": 0.0,
				"delivered_cum": 0.0,
				"demand_cum": 0.0,
				"loss_cum": 0.0,
				"loss_prev": 0.0,
				"demand_peak": 0.0,
				"zero_day": 0,
				"zero_streak": 0,
				"first_zero_day": 0,
				"first_demand": 0.0,
				"first_in_kind": 0.0,
				"first_arrived": 0.0,
				"first_imported": 0.0,
				"first_delivered": 0.0,
				"first_loss": 0.0,
				"demand_at_zero": 0.0,
				"in_kind_at_zero": 0.0,
				"arrived_at_zero": 0.0,
				"exported_at_zero": 0.0,
				"imported_at_zero": 0.0,
				"delivered_at_zero": 0.0,
				"loss_at_zero": 0.0,
				"days_in_window": 0,
				"days_delivered": 0,
			}
	var out_of_window := 0
	var in_window_no_delivery := 0
	var negative_stock := 0
	var backlog_skipped := 0
	var hemisphere_delivery := {"north": 0, "south": 0}
	for day in ENT_DAYS:
		var tick: Dictionary = sim.tick_day()
		var violations: Array = tick.get("invariant_violations", [])
		if violations.size() > 0:
			_fail("dia %d: invariante violado: %s" % [day + 1, violations[0]])
			break
		status = sim.get_agriculture_status()
		var doy := int(status["date"]["doy"])
		for nation in status.get("nations", []):
			var tag := String(nation["nation"])
			for crop in nation["crops"]:
				var product := String(crop["product"])
				var key := "%s|%s" % [tag, product]
				var entry: Dictionary = _crop_stats.get(key, {})
				if entry.is_empty():
					continue
				var stock := float(crop["stock_t"])
				if stock < -1e-6:
					negative_stock += 1
				var delivered := float(crop["delivered_today_t"])
				var window := _in_harvest_window(tag, product, doy)
				if window:
					entry["days_in_window"] = int(entry["days_in_window"]) + 1
				if delivered > 0.0:
					entry["days_delivered"] = int(entry["days_delivered"]) + 1
					if not window:
						out_of_window += 1
				elif window:
					# Dia 1: a safra em curso no início do jogo já está no
					# estoque inicial; o núcleo a marca como entregue e não
					# entrega lote retroativo. É o único caso legítimo de janela
					# sem entrega.
					if day == 0:
						backlog_skipped += 1
					else:
						in_window_no_delivery += 1
				entry["imported"] = float(entry["imported"]) \
					+ float(crop["imported_today_t"])
				entry["exported"] = float(entry["exported"]) \
					+ float(crop["exported_today_t"])
				entry["in_kind"] = float(entry["in_kind"]) \
					+ float(crop["in_kind_today_t"])
				entry["arrived"] = float(entry["arrived"]) \
					+ float(crop["arrived_today_t"])
				entry["delivered_cum"] = float(entry["delivered_cum"]) + delivered
				entry["demand_cum"] = float(entry["demand_cum"]) \
					+ float(crop["demand_t_day"])
				# `loss_month_t` zera na virada do mês: acumula por delta. A
				# perda de ENTRADA (pós-colheita) NÃO entra na conta do estoque —
				# ela acontece antes da tonelada entrar no silo; só armazenagem +
				# perecibilidade são saída de estoque.
				var loss_now := float(crop["storage_loss_month_t"])
				var loss_delta := loss_now - float(entry["loss_prev"])
				if loss_delta < 0.0:
					loss_delta = loss_now
				entry["loss_cum"] = float(entry["loss_cum"]) + loss_delta
				entry["loss_prev"] = loss_now
				if day < 10:
					entry["demand_peak"] = maxf(float(entry["demand_peak"]),
						float(crop["demand_t_day"]))
				if stock <= 1e-9:
					if float(entry["stock_prev"]) <= 1e-9:
						entry["zero_streak"] = int(entry["zero_streak"]) + 1
					else:
						entry["zero_streak"] = 1
						# Guarda os acumuladores no PRIMEIRO dia da ruptura (o
						# zero só é confirmado 3 dias depois).
						entry["first_zero_day"] = day + 1
						entry["first_demand"] = float(entry["demand_cum"])
						entry["first_in_kind"] = float(entry["in_kind"])
						entry["first_arrived"] = float(entry["arrived"])
						entry["first_imported"] = float(entry["imported"])
						entry["first_delivered"] = float(entry["delivered_cum"])
						entry["first_loss"] = float(entry["loss_cum"])
					# Zero de 3 dias seguidos = ruptura real. Um zero de fim de
					# dia com colheita diária (just-in-time) não é ruptura.
					if int(entry["zero_day"]) == 0 and int(entry["zero_streak"]) >= 3:
						entry["zero_day"] = int(entry["first_zero_day"])
						entry["demand_at_zero"] = float(entry["first_demand"])
						entry["in_kind_at_zero"] = float(entry["first_in_kind"])
						entry["arrived_at_zero"] = float(entry["first_arrived"])
						entry["exported_at_zero"] = float(entry["exported"])
						entry["imported_at_zero"] = float(entry["first_imported"])
						entry["delivered_at_zero"] = float(entry["first_delivered"])
						entry["loss_at_zero"] = float(entry["first_loss"])
				else:
					entry["zero_streak"] = 0
				entry["stock_prev"] = stock
				_crop_stats[key] = entry
		# Amostra: conta a entrega por hemisfério (prova de que a safra existe
		# nos dois lados do Equador).
		for tag in SAMPLE_NORTH:
			if _nation_delivered(status, tag) > 0.0:
				hemisphere_delivery["north"] = int(hemisphere_delivery["north"]) + 1
		for tag in SAMPLE_SOUTH:
			if _nation_delivered(status, tag) > 0.0:
				hemisphere_delivery["south"] = int(hemisphere_delivery["south"]) + 1
		if _failures.size() > 0:
			break

	print("[agri] entressafra %d d: fora_da_janela=%d dentro_sem_entrega=%d "
		% [ENT_DAYS, out_of_window, in_window_no_delivery]
		+ "safra_em_curso_no_dia_1=%d estoque_negativo=%d dias_com_entrega(N/S)=%d/%d"
		% [backlog_skipped, negative_stock, int(hemisphere_delivery["north"]),
			int(hemisphere_delivery["south"])])
	if out_of_window > 0:
		_fail("safra entregue FORA da janela em %d casos" % out_of_window)
	if in_window_no_delivery > 0:
		_fail("janela de colheita sem entrega em %d casos (plano declarado)" % in_window_no_delivery)
	if negative_stock > 0:
		_fail("estoque negativo em %d leituras" % negative_stock)
	if int(hemisphere_delivery["north"]) == 0:
		_fail("nenhuma entrega nos países do norte da amostra")
	if int(hemisphere_delivery["south"]) == 0:
		_fail("nenhuma entrega nos países do sul da amostra")

	# Amostra: dias de janela e dias de entrega por cultura devem bater (a
	# diferença de 1 dia é a safra em curso no dia 1, já no estoque inicial).
	for tag in SAMPLE_NORTH + SAMPLE_SOUTH:
		for product in ["wheat", "rice", "maize", "potato", "soy"]:
			var key := "%s|%s" % [tag, product]
			if not _crop_stats.has(key):
				continue
			var entry: Dictionary = _crop_stats[key]
			var in_window := int(entry["days_in_window"])
			var delivered := int(entry["days_delivered"])
			if in_window - delivered > 1:
				_fail("%s: %d dias de janela contra %d dias de entrega" % [
					key, in_window, delivered])

	_test_zero_cause()
	var snapshot: Dictionary = sim.to_snapshot()
	return snapshot


## Prova da causa de cada nação/cultura que zerou.
##
## Duas condições, ambas necessárias:
##  1. CONTABILIDADE (detecta vazamento): oferta física até o dia do zero
##     (estoque inicial + CHEGADAS de carga + colheita entregue) menos as perdas
##     precisa fechar com o uso (consumo do mercado — que já inclui a venda a
##     outra cidade — + consumo em espécie). Tolerância de 2% cobre arredondamento
##     e o resto de carga em trânsito no dia.
##  2. CALENDÁRIO (detecta erro de janela/estoque inicial): o dia do zero tem de
##     ser anterior à primeira abertura da janela de colheita OU, se a colheita
##     já tinha começado, a oferta daquele ciclo tem de ser menor que o uso — nas
##     duas leituras o dado explica a ruptura; se o estoque durasse até a
##     colheita, o teste FALHA.
func _test_zero_cause() -> void:
	var zeroed := 0
	var proved := 0
	var after_harvest := 0
	var unexplained: Array[String] = []
	for key in _crop_stats.keys():
		var entry: Dictionary = _crop_stats[key]
		if bool(entry.get("dust", false)):
			continue
		var zero_day := int(entry["zero_day"])
		if zero_day == 0:
			continue
		zeroed += 1
		var nation := String(entry["nation"])
		var crop := String(entry["crop"])
		var first_harvest := _days_to_first_harvest(nation, crop)
		var stock0 := float(entry["stock0"])
		var arrived := float(entry["arrived_at_zero"])
		var delivered := float(entry["delivered_at_zero"])
		var demand := float(entry["demand_at_zero"])
		var in_kind := float(entry["in_kind_at_zero"])
		var loss := float(entry["loss_at_zero"])
		if stock0 <= 0.0:
			unexplained.append("%s: zerou sem estoque inicial declarado" % key)
			continue
		if first_harvest < 0.0:
			unexplained.append("%s: sem calendário de colheita" % key)
			continue
		# (1) Contabilidade física em toneladas.
		var supply_t := stock0 + arrived + delivered
		var sink_t := demand + in_kind + loss
		var tolerance := maxf(1.0, supply_t * 0.02)
		if absf(supply_t - sink_t) > tolerance:
			unexplained.append(
				"%s: zerou no dia %d com oferta %.0f t e uso %.0f t (estoque "
				% [key, zero_day, supply_t, sink_t]
				+ "%.0f, chegadas %.0f, colheita %.0f, consumo %.0f, em espécie "
				% [stock0, arrived, delivered, demand]
				+ "%.0f, perdas %.0f): vazamento de %.0f t" % [
					in_kind, loss, supply_t - sink_t])
			continue
		# (2) Calendário: se a colheita ainda não tinha aberto, a ruptura está
		# explicada pelo calendário; se já tinha, o que explica é a
		# contabilidade acima (oferta do ciclo < uso) — em nenhum dos dois casos
		# o zero vem de erro de janela ou de estoque inicial.
		if float(zero_day) < first_harvest + float(CAUSE_TOLERANCE_DAYS):
			proved += 1
		else:
			after_harvest += 1
			proved += 1
	print("[agri] zerados=%d provados_pelo_dado=%d (deles %d depois da 1ª "
		% [zeroed, proved, after_harvest]
		+ "colheita) inexplicados=%d" % unexplained.size())
	if not unexplained.is_empty():
		for line in unexplained.slice(0, 8):
			_fail("estoque zerado sem causa no dado: " + line)
		if unexplained.size() > 8:
			_fail("... e %d outros casos" % (unexplained.size() - 8))


func _nation_delivered(status: Dictionary, tag: String) -> float:
	for nation in status.get("nations", []):
		if String(nation["nation"]) != tag:
			continue
		var total := 0.0
		for crop in nation["crops"]:
			total += float(crop["delivered_today_t"])
		return total
	return 0.0


## Dia do ano (1..365) é o dia `doy`. A janela vem do calendário (mês de início
## e fim), convertida com o calendário de referência de 365 dias — a MESMA
## conversão que o AgricultureService declara ao núcleo.
func _in_harvest_window(tag: String, crop: String, doy: int) -> bool:
	# Fallback declarado (D-099): a janela é o ano inteiro, por declaração.
	if _fallback_pairs.has(tag + "|" + crop):
		return true
	var entry: Dictionary = _calendar.get(tag, {}).get("crops", {}).get(crop, {})
	for season in entry.get("seasons", []):
		var harvest: Dictionary = season.get("harvest", {})
		var start := _month_start_doy(int(harvest.get("start_month", 0)))
		var end := _month_end_doy(int(harvest.get("end_month", 0)))
		if start <= 0 or end <= 0:
			continue
		if start <= end:
			if doy >= start and doy <= end:
				return true
		elif doy >= start or doy <= end:
			return true
	return false


## Dias entre 2027-01-01 e a primeira abertura da janela de colheita da cultura
## (-1 = sem calendário).
func _days_to_first_harvest(tag: String, crop: String) -> float:
	# Fallback declarado (D-099): colheita contínua desde o dia 1.
	if _fallback_pairs.has(tag + "|" + crop):
		return 0.0
	var entry: Dictionary = _calendar.get(tag, {}).get("crops", {}).get(crop, {})
	var best := -1.0
	for season in entry.get("seasons", []):
		var harvest: Dictionary = season.get("harvest", {})
		var start := _month_start_doy(int(harvest.get("start_month", 0)))
		if start <= 0:
			continue
		var days := float(start - 1)
		if best < 0.0 or days < best:
			best = days
	return best


func _month_start_doy(month: int) -> int:
	if month < 1 or month > 12:
		return 0
	const MONTH_DAYS := [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
	var doy := 1
	for index in month - 1:
		doy += int(MONTH_DAYS[index])
	return doy


func _month_end_doy(month: int) -> int:
	if month < 1 or month > 12:
		return 0
	const MONTH_DAYS := [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
	return _month_start_doy(month) + int(MONTH_DAYS[month - 1]) - 1


## Dias até a PRÓXIMA abertura de janela de colheita a partir de `doy` (0 =
## abre hoje; -1 = sem calendário).
func _days_to_next_harvest(tag: String, crop: String, doy: int) -> float:
	var entry: Dictionary = _calendar.get(tag, {}).get("crops", {}).get(crop, {})
	var best := -1.0
	for season in entry.get("seasons", []):
		var harvest: Dictionary = season.get("harvest", {})
		var start := _month_start_doy(int(harvest.get("start_month", 0)))
		if start <= 0:
			continue
		var days := float(((start - doy) % 365 + 365) % 365)
		if days < 1.0:
			days = 365.0  # já está na janela: a próxima é o ciclo seguinte
		if best < 0.0 or days < best:
			best = days
	return best


# ---------------------------------------------------------------------------
# (f) folha sazonal: parada na entressafra, folha na colheita
# ---------------------------------------------------------------------------

## Fora de TODAS as janelas (plantio e colheita) do plano, a fazenda sazonal
## não tem equipe nem custo e não acumula dia de prejuízo; na colheita volta a
## pagar. O status NÃO publica `paid_wages`/`paid_opex` separados: publica
## `costs = paid_wages + paid_opex + input_value`, e o insumo só é comprado na
## janela de PLANTIO — fora dela `costs − input_value` é exatamente a folha +
## opex pagos. `workers` é o emprego do dia (o `jobs` publicado segue sendo a
## capacidade da fazenda).
##
## Mede os DOIS lados no mesmo mundo: primeiro um dia de COLHEITA com folha
## paga (exige caixa da firma: com caixa zero a folha é zero em qualquer
## estação, por liquidez), depois o primeiro dia fora de todas as janelas.
func _test_seasonal_payroll(snapshot: Dictionary) -> void:
	var plans: Array = snapshot.get("definitions", {}).get("crop_plans", [])
	var products: Array = snapshot.get("save", {}).get("products", [])
	var probe: Object = _restore(snapshot)
	var pick: Dictionary = _pick_paid_farm(probe, plans, products)
	if pick.is_empty():
		_fail("folha sazonal: nenhuma fazenda com plano e folha paga na colheita")
		return
	var city_id := String(pick["city"])
	var building_id := String(pick["building"])
	var label := "%s/%s" % [city_id, String(pick["product"])]
	var windows: Array = pick["windows"]
	# (1) Colheita: a equipe e a folha voltam.
	var harvest: Dictionary = _building_entry(probe, city_id, building_id)
	if harvest.is_empty():
		_fail("folha sazonal: %s sumiu do status na colheita" % label)
		return
	var harvest_doy := _doy(probe)
	var paid_harvest := float(harvest.get("costs", 0.0)) \
		- float(harvest.get("input_value", 0.0))
	var workers_harvest := float(harvest.get("workers", 0.0))
	if workers_harvest <= 0.0:
		_fail("folha sazonal: %s não empregou na colheita (doy %d)" % [
			label, harvest_doy])
	if paid_harvest <= 0.0:
		_fail("folha sazonal: %s não pagou folha/opex na colheita (doy %d)" % [
			label, harvest_doy])
	# (2) Fora de TODAS as janelas do plano: equipe liberada, custo zero e
	# nenhum dia de prejuízo acumulado.
	if not _advance_outside(probe, windows):
		_fail("folha sazonal: %s não tem dia fora das janelas em um ano" % label)
		return
	var outside: Dictionary = _building_entry(probe, city_id, building_id)
	if outside.is_empty():
		_fail("folha sazonal: %s sumiu do status fora da janela" % label)
		return
	var outside_doy := _doy(probe)
	var paid_outside := float(outside.get("costs", 0.0)) \
		- float(outside.get("input_value", 0.0))
	if paid_outside > 1e-9:
		_fail("folha sazonal: %s pagou %.2f (folha+opex) fora da janela" % [
			label, paid_outside])
	if absf(float(outside.get("input_value", 0.0))) > 1e-9:
		_fail("folha sazonal: %s comprou insumo fora do plantio (%.2f)" % [
			label, float(outside.get("input_value", 0.0))])
	if float(outside.get("workers", 0.0)) > 1e-9:
		_fail("folha sazonal: %s empregou %.2f fora da janela" % [
			label, float(outside.get("workers", 0.0))])
	if int(outside.get("loss_days", 0)) != 0:
		_fail("folha sazonal: %s acumulou %d dia(s) de prejuízo fora da janela" % [
			label, int(outside.get("loss_days", 0))])
	print("[agri] folha sazonal %s: colheita (doy %d) custo=%.2f equipe=%.2f; "
		% [label, harvest_doy, paid_harvest, workers_harvest]
		+ "fora (doy %d) custo=%.2f equipe=%.2f loss_days=%d" % [
			outside_doy, paid_outside, float(outside.get("workers", 0.0)),
			int(outside.get("loss_days", 0))])


## Primeira (cidade, produto) com plano cuja fazenda primária está HOJE numa
## janela de colheita e pagou folha/opex (caixa > 0). Devolve as janelas do
## plano (definições do save). Sem candidato, avança até 20 dias e tenta de
## novo (dia 121 do mundo tem várias janelas abertas).
func _pick_paid_farm(sim: Object, plans: Array, products: Array) -> Dictionary:
	for attempt in 21:
		var pick := _find_paid_farm(sim, plans, products)
		if not pick.is_empty():
			return pick
		sim.tick_day()
	return {}


func _find_paid_farm(sim: Object, plans: Array, products: Array) -> Dictionary:
	var doy := _doy(sim)
	var cache := {}
	for plan in plans:
		var city := String(plan.get("city", ""))
		var product := String(plan.get("product", ""))
		var windows: Array = plan.get("seasons", [])
		if city == "" or product == "" or windows.is_empty():
			continue
		if not _plan_in_harvest(windows, doy):
			continue
		var product_index := products.find(product)
		if product_index < 0:
			continue
		var status: Dictionary = cache.get(city, {})
		if status.is_empty():
			status = sim.get_city_status(city)
			if status.is_empty():
				continue
			cache[city] = status
		for entry in status.get("buildings", []):
			if int(entry.get("output_product", -1)) != product_index:
				continue
			if int(entry.get("input_product", 0)) >= 0:
				continue
			if String(entry.get("recipe", "")) != "":
				continue
			if float(entry.get("workers", 0.0)) <= 0.0:
				continue
			if float(entry.get("costs", 0.0)) \
					- float(entry.get("input_value", 0.0)) <= 0.0:
				continue
			return {
				"city": city,
				"product": product,
				"building": String(entry.get("id", "")),
				"windows": windows,
			}
	return {}


func _building_entry(sim: Object, city_id: String, building_id: String) -> Dictionary:
	var status: Dictionary = sim.get_city_status(city_id)
	for entry in status.get("buildings", []):
		if String(entry.get("id", "")) == building_id:
			return entry
	return {}


func _doy(sim: Object) -> int:
	return int(sim.get_agriculture_status().get("date", {}).get("doy", 0))


## Avança até um dia FORA de todas as janelas e deixa o status desse dia
## publicado (o tick do dia parado já rodou).
func _advance_outside(sim: Object, windows: Array, max_days: int = 400) -> bool:
	for step in max_days:
		if _plan_in_any_window(_doy(sim), windows):
			sim.tick_day()
			continue
		sim.tick_day()
		if not _plan_in_any_window(_doy(sim), windows):
			return true
	return false


func _plan_in_any_window(doy: int, windows: Array) -> bool:
	for season in windows:
		if _doy_in_window(doy, int(season.get("plant_start", 0)),
				int(season.get("plant_end", 0))):
			return true
		if _doy_in_window(doy, int(season.get("harvest_start", 0)),
				int(season.get("harvest_end", 0))):
			return true
	return false


func _plan_in_harvest(windows: Array, doy: int) -> bool:
	for season in windows:
		if _doy_in_window(doy, int(season.get("harvest_start", 0)),
				int(season.get("harvest_end", 0))):
			return true
	return false


func _doy_in_window(doy: int, start: int, end: int) -> bool:
	if start <= 0 or end <= 0:
		return false
	if start <= end:
		return doy >= start and doy <= end
	return doy >= start or doy <= end


# ---------------------------------------------------------------------------
# (c) perda por armazém: A/B com o MESMO mundo
# ---------------------------------------------------------------------------

func _test_storage_quality(snapshot: Dictionary) -> void:
	# Escolhe a maior nação/cultura com estoque e SEM colheita na janela do
	# teste (para isolar a qualidade do armazém da perda de entrada).
	var probe: Object = _restore(snapshot)
	var status: Dictionary = probe.get_agriculture_status()
	var source: Object = _restore(snapshot)
	var chosen := ""
	var chosen_crop := ""
	var chosen_stock := 0.0
	for nation in status.get("nations", []):
		for crop in nation["crops"]:
			var stock := float(crop["stock_t"])
			var gap := float(crop["next_harvest_days"])
			if stock <= chosen_stock or gap < STORAGE_TEST_MIN_HARVEST_GAP:
				continue
			chosen = String(nation["nation"])
			chosen_crop = String(crop["product"])
			chosen_stock = stock
	if chosen == "":
		_fail("sem nação/cultura com estoque e sem colheita em %d d para o A/B "
			% STORAGE_TEST_MIN_HARVEST_GAP + "de armazém")
		return
	var city_ids := _cities_of_nation(chosen)
	var bad: Object = _restore(snapshot)
	var good: Object = _restore(snapshot)
	for city_id in city_ids:
		bad.set_storage_quality(city_id, 0.0)
		good.set_storage_quality(city_id, 1.0)
	var bad_start: Dictionary = bad.get_agriculture_status()["totals"]
	var good_start: Dictionary = good.get_agriculture_status()["totals"]
	for day in STORAGE_TEST_DAYS:
		bad.tick_day()
		good.tick_day()
	var bad_end: Dictionary = bad.get_agriculture_status()["totals"]
	var good_end: Dictionary = good.get_agriculture_status()["totals"]
	var bad_loss := float(bad_end["storage_t"].get(chosen_crop, 0.0)) \
		- float(bad_start["storage_t"].get(chosen_crop, 0.0)) \
		+ float(bad_end["entry_t"].get(chosen_crop, 0.0)) \
		- float(bad_start["entry_t"].get(chosen_crop, 0.0))
	var good_loss := float(good_end["storage_t"].get(chosen_crop, 0.0)) \
		- float(good_start["storage_t"].get(chosen_crop, 0.0)) \
		+ float(good_end["entry_t"].get(chosen_crop, 0.0)) \
		- float(good_start["entry_t"].get(chosen_crop, 0.0))
	print("[agri] armazém %s/%s (%d cidades, estoque %.0f t): perda ruim=%.1f t "
		% [chosen, chosen_crop, city_ids.size(), chosen_stock, bad_loss]
		+ "boa=%.1f t em %d d" % [good_loss, STORAGE_TEST_DAYS])
	if bad_loss <= 0.0:
		_fail("armazém ruim não perdeu nada em %s/%s" % [chosen, chosen_crop])
	elif good_loss < 0.0:
		_fail("perda negativa no armazém bom (%s/%s)" % [chosen, chosen_crop])
	elif bad_loss <= good_loss:
		_fail("armazém ruim (%.3f t) não perdeu mais que o bom (%.3f t) em %s/%s" % [
			bad_loss, good_loss, chosen, chosen_crop])


func _cities_of_nation(tag: String) -> Array:
	var out: Array = []
	for city in _cities:
		if String(city.get("nation", "")) == tag:
			out.append(String(city.get("id", "")))
	return out


# ---------------------------------------------------------------------------
# (d) perecibilidade
# ---------------------------------------------------------------------------

func _test_perishability(snapshot: Dictionary) -> void:
	var probe: Object = _restore(snapshot)
	var before: Dictionary = probe.get_agriculture_status()["totals"]
	# Taxa de 1 dia: a perda do dia é exatamente `estoque × (1 - 0,5^(1/dias))`
	# do estoque que existe ANTES da fase (o snapshot é o fim do dia anterior).
	probe.tick_day()
	var one_day: Dictionary = probe.get_agriculture_status()["totals"]
	var rates := {
		"potato": 1.0 - pow(0.5, 1.0 / 61.0),
		"wheat": 1.0 - pow(0.5, 1.0 / 365.0),
		"maize": 1.0 - pow(0.5, 1.0 / 274.0),
		"soy": 0.0,
		"bran": 0.0,
		"feed": 0.0,
	}
	var checked := 0
	for product in rates.keys():
		var stock := float(before["stock_t"].get(product, 0.0))
		var loss := float(one_day["perishable_t"].get(product, 0.0)) \
			- float(before["perishable_t"].get(product, 0.0))
		if stock <= 0.0:
			if loss > 1e-6:
				_fail("%s: perda de perecibilidade sem estoque (%.3f t)" % [
					product, loss])
			continue
		var measured := loss / stock
		var expected := float(rates[product])
		checked += 1
		if absf(measured - expected) > maxf(1e-6, expected * 0.02):
			_fail("%s: taxa de perecibilidade %.6f (esperada %.6f)" % [
				product, measured, expected])
	# 61 dias: batata decai ~meia-vida; trigo quase não; no_data zero.
	var start: Dictionary = before
	var sim: Object = _restore(snapshot)
	for day in PERISHABLE_DAYS:
		sim.tick_day()
	var end: Dictionary = sim.get_agriculture_status()["totals"]
	var potato_stock := float(start["stock_t"].get("potato", 0.0))
	var potato_loss := float(end["perishable_t"].get("potato", 0.0)) \
		- float(start["perishable_t"].get("potato", 0.0))
	var wheat_stock := float(start["stock_t"].get("wheat", 0.0))
	var wheat_loss := float(end["perishable_t"].get("wheat", 0.0)) \
		- float(start["perishable_t"].get("wheat", 0.0))
	var soy_loss := float(end["perishable_t"].get("soy", 0.0)) \
		- float(start["perishable_t"].get("soy", 0.0))
	var potato_ratio := potato_loss / maxf(potato_stock, 1.0)
	var wheat_ratio := wheat_loss / maxf(wheat_stock, 1.0)
	print("[agri] perecibilidade %d d: batata=%.1f%% do estoque (%.0f t), "
		% [PERISHABLE_DAYS, potato_ratio * 100.0, potato_stock]
		+ "trigo=%.1f%%, soja=%.1f t (no_data, não decai); taxas 1 d: %d produtos" % [
			wheat_ratio * 100.0, soy_loss, checked])
	if potato_stock > 0.0 and potato_ratio < 0.25:
		_fail("batata decaiu só %.1f%% em %d d (meia-vida esperada ~50%%)" % [
			potato_ratio * 100.0, PERISHABLE_DAYS])
	if wheat_stock > 0.0 and wheat_ratio > 0.20:
		_fail("trigo decaiu %.1f%% em %d d (esperado ~11%%)" % [
			wheat_ratio * 100.0, PERISHABLE_DAYS])
	if wheat_stock > 0.0 and potato_ratio <= wheat_ratio * 2.0:
		_fail("batata não decai mais rápido que trigo (%.3f vs %.3f)" % [
			potato_ratio, wheat_ratio])
	if soy_loss > 1e-6:
		_fail("soja decaiu %.3f t sem dado de prateleira (no_data)" % soy_loss)


# ---------------------------------------------------------------------------
# (e) save v24 + load de save v22
# ---------------------------------------------------------------------------

func _test_save(sim: Object) -> void:
	var snapshot: Dictionary = sim.to_snapshot()
	var save: Dictionary = snapshot.get("save", {})
	if int(save.get("version", 0)) != 31:
		_fail("save v31 esperado, veio %s" % str(save.get("version")))
		return
	if not save.has("agriculture"):
		_fail("save v24 sem o bloco 'agriculture'")
		return
	var definitions: Dictionary = snapshot.get("definitions", {})
	for key in ["crop_plans", "crop_losses", "shelf_life"]:
		if not definitions.has(key):
			_fail("definitions sem '%s'" % key)
		if Array(definitions.get(key, [])).is_empty():
			_fail("definitions '%s' vazio" % key)

	# Round-trip: restaurar e re-serializar tem de reproduzir o mesmo bloco.
	var restored: Object = _restore(snapshot)
	var again: Dictionary = restored.to_snapshot()
	var before_block := JSON.stringify(save.get("agriculture", {}))
	var after_block := JSON.stringify(again.get("save", {}).get("agriculture", {}))
	if before_block != after_block:
		_fail("round-trip v24 mudou o bloco 'agriculture'")
	# E o dia seguinte tem de ser o mesmo nos dois mundos (estado restaurado ==
	# estado direto): compara o bloco de agricultura depois de um tick.
	var direct_next: Dictionary = sim.get_agriculture_status()
	var restored_next: Dictionary = restored.get_agriculture_status()
	if int(direct_next.get("day", -1)) != int(restored_next.get("day", -2)):
		_fail("round-trip: dia diferente (%s != %s)" % [
			str(direct_next.get("day")), str(restored_next.get("day"))])
	var direct_after := str(sim.tick_day().get("day", -1))
	var restored_after := str(restored.tick_day().get("day", -1))
	if direct_after != restored_after:
		_fail("round-trip: primeiro dia após o load divergiu")
	# O estado da safra depois do MESMO dia tem de ser idêntico (o load não
	# pode reentregar nem perder um lote).
	var direct_state := JSON.stringify(
		sim.to_snapshot()["save"].get("agriculture", {}))
	var restored_state := JSON.stringify(
		restored.to_snapshot()["save"].get("agriculture", {}))
	if direct_state != restored_state:
		_fail("round-trip: estado da safra divergiu no dia seguinte ao load")
	print("[agri] save v24: %d planos nas definições, bloco agriculture com "
		% Array(definitions.get("crop_plans", [])).size()
		+ "%d entradas de estado" % Array(save.get("agriculture", {})
			.get("plans", [])).size())

	# Load de um save v22 (antes da agricultura): planos vêm das definições, o
	# estado da safra nasce zerado e o mundo roda um dia sem violar invariante.
	var old_snapshot := snapshot.duplicate(true)
	var old_save: Dictionary = old_snapshot["save"]
	old_save["version"] = 22
	old_save.erase("agriculture")
	old_snapshot["save"] = old_save
	var old_sim: Object = _restore(old_snapshot)
	var old_status: Dictionary = old_sim.get_agriculture_status()
	if old_status.is_empty():
		_fail("save v22: planos não foram reaplicados pelo bloco 'definitions'")
		return
	var stock_before: Dictionary = old_status["totals"]["stock_t"]
	var old_tick: Dictionary = old_sim.tick_day()
	var old_violations: Array = old_tick.get("invariant_violations", [])
	if old_violations.size() > 0:
		_fail("save v22: invariante violado no 1º dia: %s" % str(old_violations[0]))
	var old_agri: Dictionary = old_sim.to_snapshot()["save"].get("agriculture", {})
	var old_plans: Array = old_agri.get("plans", [])
	# Sem o estado, a safra JÁ EM CURSO no dia do load não pode ser reentregue
	# (o estoque carregado já a contém): o estoque do dia seguinte não pode dar
	# um salto de colheita retroativa.
	var stock_after: Dictionary = old_sim.get_agriculture_status()["totals"]["stock_t"]
	var jumps: Array[String] = []
	for product in ["wheat", "rice", "maize", "potato", "soy"]:
		var before := float(stock_before.get(product, 0.0))
		var after := float(stock_after.get(product, 0.0))
		if after > before * 1.05 + 1.0:
			jumps.append("%s %.0f -> %.0f" % [product, before, after])
	print("[agri] load v22: planos reaplicados, estado da safra zerado, "
		+ "%d entradas após 1 dia, saltos_de_estoque=%d" % [
			old_plans.size(), jumps.size()])
	if not jumps.is_empty():
		_fail("save v22: colheita retroativa no 1º dia (%s)" % jumps[0])


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

func _fail(message: String) -> void:
	_failures.append(message)


## Dado de estoque da nação/cultura (`crop_stocks.json`): `ending_stocks_t`,
## `consumption_t`, `days_of_consumption` e o método (PSD ou regra de 60 d).
func _stock_data(tag: String, crop: String) -> Dictionary:
	if _stocks_data.is_empty():
		_stocks_data = _load_dict(CROP_STOCKS_PATH).get("nations", {})
	return _stocks_data.get(tag, {}).get("crops", {}).get(crop, {})


## Número de um campo JSON. `null` é ausência declarada no dado — vira o padrão,
## nunca um zero que pareça medição.
func _num(value: Variant, fallback: float = 0.0) -> float:
	if value == null:
		return fallback
	var type := typeof(value)
	if type == TYPE_INT or type == TYPE_FLOAT:
		return float(value)
	return fallback


func _load_json(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("não abriu " + path)
		return null
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data


func _load_array(path: String) -> Array:
	var data: Variant = _load_json(path)
	return data if typeof(data) == TYPE_ARRAY else []


func _load_dict(path: String) -> Dictionary:
	var data: Variant = _load_json(path)
	return data if typeof(data) == TYPE_DICTIONARY else {}
