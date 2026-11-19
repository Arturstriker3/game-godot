extends RefCounted
class_name AgricultureService

## G — AGRICULTURA SAZONAL: ponte entre `data/definitions/agriculture/` e o
## núcleo (D-094 dados, D-096 mecânica).
##
## O que este serviço DECLARA no núcleo (API `set_crop_*`, antes do dia 1):
##   * plano de safra por (cidade, produto): área colhida, rendimento e as
##     janelas de plantio/colheita em dia do ano;
##   * estoque inicial (t) por (cidade, produto);
##   * perda pós-colheita e prateleira por produto;
##   * fertilizante nitrogenado (kg N/ha) por (cidade, produto).
##
## REGRA DE ÁREA POR CIDADE (declarada, sem número inventado):
##   `área_cidade = área_colhida_da_nação × participação_da_cidade`
##   participação = capacidade de terra da cidade / Σ capacidade da nação para
##   a cultura (a MESMA `city_land_capacity_t_day` que o GeographyService declara
##   ao núcleo). Sem capacidade declarada na nação (deserto, ilha sem lavoura,
##   dado ausente), a participação é a população de simulação da cidade — e o
##   plano fica marcado `area_basis: "population"` no relatório.
##
## O QUE NÃO É ESTIMADO AQUI:
##   * safra sem calendário (29 pares `no_data` + 33 `all_year`) NÃO recebe
##     plano: a produção diária das fazendas continua (ausência explícita);
##   * participação por safra não existe nos arquivos: 1/n por safra (declarado);
##   * perda pós-colheita é por NAÇÃO no dado e o contrato do núcleo é por
##     PRODUTO: usa-se a média ponderada pela produção nacional da cultura
##     (`production_t`), documentada no relatório (`loss_basis`);
##   * leite não é bem do jogo (o dado nacional existe, a cadeia não) — lacuna
##     declarada, nenhum bem novo é criado.

const CROP_CALENDAR_PATH := "res://data/definitions/agriculture/crop_calendar.json"
const CROP_YIELDS_PATH := "res://data/definitions/agriculture/crop_yields.json"
const CROP_STOCKS_PATH := "res://data/definitions/agriculture/crop_stocks.json"
const POSTHARVEST_LOSSES_PATH := "res://data/definitions/agriculture/postharvest_losses.json"
const FERTILIZER_N_PATH := "res://data/definitions/agriculture/fertilizer_n.json"
const LIVESTOCK_CYCLE_PATH := "res://data/definitions/agriculture/livestock_cycle.json"
const PRODUCT_SHELF_LIFE_PATH := "res://data/definitions/agriculture/product_shelf_life.json"

## Os cinco primários de comida do jogo (goods.json `kind: raw`, `food: true`).
const CROPS := ["wheat", "rice", "maize", "potato", "soy"]
## Os 12 bens da cadeia com prazo de prateleira publicado (product_shelf_life).
const CHAIN_PRODUCTS := [
	"wheat", "rice", "maize", "potato", "soy", "flour", "bran", "bread",
	"bagged_rice", "feed", "meat", "canned_food",
]
## Meses por ano do calendário de referência (365 dias, sem bissexto): o dado
## declara MÊS, não dia; a janela de um mês começa no dia 1 e termina no último
## dia do mês de referência. O bissexto desloca a posição em 1 dia (fração da
## safra), nunca a ordem dos meses.
const MONTH_DAYS := [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
## Regra declarada de estoque inicial quando o dado não tem `ending_stocks_t`:
## nenhum valor é inventado (o núcleo fica sem estoque para aquele par).
const STOCK_RULE_DAYS := 60.0

var _calendar: Dictionary = {}
var _yields: Dictionary = {}
var _stocks: Dictionary = {}
var _losses: Dictionary = {}
var _fertilizer: Dictionary = {}
var _livestock: Dictionary = {}
var _shelf: Dictionary = {}
var _loaded := false


## Carrega os 7 arquivos. Idempotente; devolve false quando algum falta (o
## chamador segue sem agricultura sazonal — ausência explícita).
func load_data() -> bool:
	if _loaded:
		return true
	_calendar = _read_dict(CROP_CALENDAR_PATH, "nations")
	_yields = _read_dict(CROP_YIELDS_PATH, "nations")
	_stocks = _read_dict(CROP_STOCKS_PATH, "nations")
	_losses = _read_dict(POSTHARVEST_LOSSES_PATH, "nations")
	_fertilizer = _read_dict(FERTILIZER_N_PATH, "nations")
	_livestock = _read_dict(LIVESTOCK_CYCLE_PATH, "nations")
	_shelf = _read_dict(PRODUCT_SHELF_LIFE_PATH, "products")
	_loaded = not (_calendar.is_empty() or _yields.is_empty() or _shelf.is_empty())
	return _loaded


## Empurra os dados para o núcleo. `cities` no formato de `cities.json`
## (id/nation/population) e `geo` = GeographyService já carregado (a fonte da
## capacidade de terra por cidade). Devolve o relatório do que foi declarado.
func apply_to_sim(sim: Object, cities: Array, geo: Object = null,
		options: Dictionary = {}) -> Dictionary:
	var report: Dictionary = {
		"plans": 0,
		"cities": 0,
		"area_ha": {},
		"skipped_no_calendar": 0,
		"skipped_no_yield": 0,
		# Os pares (NAT|crop) que ficaram sem plano, por motivo — o diagnóstico
		# do portão de comida (Bloco 2) precisa saber QUAIS, não só quantos.
		"skipped_calendar_pairs": [],
		"skipped_yield_pairs": [],
		"area_basis": {"land_capacity": 0, "population": 0},
		"initial_stock_t": 0.0,
		"loss_basis": "production_weighted_national",
		"loss_pct": {},
		"shelf_life": {},
	}
	if not load_data():
		push_warning("AgricultureService: dados de agricultura ausentes ou ilegíveis")
		return report
	if not sim.has_method("set_crop_plan"):
		push_warning("AgricultureService: binário sem set_crop_plan (rode o build)")
		return report

	# 1. Prateleira (12 bens) e perda pós-colheita por PRODUTO (média ponderada
	#    pela produção nacional — o dado é por nação).
	_declare_shelf_life(sim, report)
	_declare_losses(sim, report)

	# 2. Agrupa as cidades por nação e soma a capacidade de terra por cultura.
	var by_nation: Dictionary = {}
	var capacity: Dictionary = {}   # nation -> {crop: [capacities por cidade]}
	var city_ids: Array = []
	for city in cities:
		var city_id := String(city.get("id", ""))
		var nation := String(city.get("nation", ""))
		if city_id == "" or nation == "":
			continue
		if not by_nation.has(nation):
			by_nation[nation] = []
			capacity[nation] = {}
			for crop in CROPS:
				capacity[nation][crop] = {}
		var sim_population := _num(city.get("sim_population"))
		if sim_population <= 0.0:
			sim_population = _num(city.get("population"))
		by_nation[nation].append({
			"id": city_id,
			"population": maxf(sim_population, 0.0),
		})
		city_ids.append(city_id)
		for crop in CROPS:
			var value := 0.0
			if geo != null and geo.has_method("city_land_capacity_t_day"):
				value = float(geo.city_land_capacity_t_day(city_id, crop))
			capacity[nation][crop][city_id] = maxf(value, 0.0)

	# 3. Plano por (cidade, produto).
	for nation in by_nation.keys():
		var nation_calendar: Dictionary = _calendar.get(nation, {})
		var nation_crops: Dictionary = nation_calendar.get("crops", {})
		var nation_yields: Dictionary = _yields.get(nation, {}).get("crops", {})
		var nation_stocks: Dictionary = _stocks.get(nation, {}).get("crops", {})
		var nation_fert: Dictionary = _fertilizer.get(nation, {}).get("crops", {})
		for crop in CROPS:
			var calendar_entry: Dictionary = nation_crops.get(crop, {})
			var seasons := _season_windows(calendar_entry)
			if seasons.is_empty():
				report["skipped_no_calendar"] = int(report["skipped_no_calendar"]) + 1
				(report["skipped_calendar_pairs"] as Array).append(nation + "|" + crop)
				continue
			var yield_entry: Dictionary = nation_yields.get(crop, {})
			var area_ha := _num(yield_entry.get("area_harvested_ha"))
			var yield_t_ha := _num(yield_entry.get("yield_t_ha"))
			if area_ha <= 0.0 or yield_t_ha <= 0.0:
				report["skipped_no_yield"] = int(report["skipped_no_yield"]) + 1
				(report["skipped_yield_pairs"] as Array).append(nation + "|" + crop)
				continue
			# Participação de cada cidade: capacidade de terra da nação para a
			# cultura; sem capacidade, população de simulação (declarado).
			var total_capacity := 0.0
			var total_population := 0.0
			for city in by_nation[nation]:
				total_capacity += float(capacity[nation][crop].get(city["id"], 0.0))
				total_population += float(city["population"])
			var use_capacity := total_capacity > 0.0
			var basis := "land_capacity" if use_capacity else "population"
			report["area_basis"][basis] = int(report["area_basis"][basis]) + 1
			var share_sum := 0.0
			for city in by_nation[nation]:
				var city_id := String(city["id"])
				var share := 0.0
				if use_capacity:
					share = float(capacity[nation][crop].get(city_id, 0.0)) \
						/ total_capacity
				elif total_population > 0.0:
					share = float(city["population"]) / total_population
				if share <= 0.0:
					continue
				var city_area := area_ha * share
				if city_area <= 0.0:
					continue
				sim.set_crop_plan(city_id, crop, city_area, yield_t_ha, seasons)
				var fert_entry: Dictionary = nation_fert.get(crop, {})
				var n_kg_ha := _num(fert_entry.get("n_kg_ha"))
				if n_kg_ha > 0.0:
					sim.set_fertilizer_n(city_id, crop, n_kg_ha)
				# Estoque inicial: `ending_stocks_t` observado (ou a regra de 60
				# dias de consumo JÁ aplicada pelo construtor do dado). O mesmo
				# rateio da área: o silo fica onde está a lavoura (declarado).
				var stock_t := _initial_stock_t(nation_stocks.get(crop, {}))
				if stock_t > 0.0 and bool(options.get("initial_stock", true)):
					sim.set_crop_initial_stock(city_id, crop, stock_t * share)
					report["initial_stock_t"] = float(
						report["initial_stock_t"]) + stock_t * share
				report["plans"] = int(report["plans"]) + 1
				share_sum += share
			report["area_ha"][nation + "|" + crop] = area_ha * share_sum
	report["cities"] = by_nation.size()
	return report


## Janelas de safra em dia do ano. Vazio = sem calendário (o par não recebe
## plano e a produção diária da fazenda continua).
func _season_windows(calendar_entry: Dictionary) -> Array:
	var seasons: Array = calendar_entry.get("seasons", [])
	var out: Array = []
	if seasons.is_empty():
		return out
	var share := 1.0 / float(seasons.size())
	for season in seasons:
		var planting: Dictionary = season.get("planting", {})
		var harvest: Dictionary = season.get("harvest", {})
		var plant_start := _month_start_doy(int(planting.get("start_month", 0)))
		var plant_end := _month_end_doy(int(planting.get("end_month", 0)))
		var harvest_start := _month_start_doy(int(harvest.get("start_month", 0)))
		var harvest_end := _month_end_doy(int(harvest.get("end_month", 0)))
		if harvest_start <= 0 or harvest_end <= 0:
			continue
		out.append({
			"plant_start": plant_start,
			"plant_end": plant_end,
			"harvest_start": harvest_start,
			"harvest_end": harvest_end,
			"share": share,
		})
	return out


## Primeiro dia do ano de um mês (1..12); 0 = mês inválido (ausência).
func _month_start_doy(month: int) -> int:
	if month < 1 or month > 12:
		return 0
	var doy := 1
	for index in month - 1:
		doy += int(MONTH_DAYS[index])
	return doy


## Último dia do ano de um mês (1..12); 0 = mês inválido.
func _month_end_doy(month: int) -> int:
	if month < 1 or month > 12:
		return 0
	return _month_start_doy(month) + int(MONTH_DAYS[month - 1]) - 1


## Estoque inicial do par (nação, cultura): usa o observado quando existe;
## senão a regra de 60 dias de consumo que o PRÓPRIO dado já calculou
## (`ending_stocks_t` de método `rule_60_days`); sem dado nenhum, zero.
func _initial_stock_t(stock_entry: Dictionary) -> float:
	if stock_entry.is_empty():
		return 0.0
	var stock := _num(stock_entry.get("ending_stocks_t"))
	if stock > 0.0:
		return stock
	var consumption := _num(stock_entry.get("consumption_t"))
	if consumption > 0.0:
		return consumption * STOCK_RULE_DAYS / 365.0
	return 0.0


## Número de um campo JSON. `null` (ausência declarada no dado) vira o padrão —
## nunca um zero que pareça medição.
func _num(value: Variant, fallback: float = 0.0) -> float:
	if value == null:
		return fallback
	var type := typeof(value)
	if type == TYPE_INT or type == TYPE_FLOAT:
		return float(value)
	return fallback


## Prateleira dos 12 bens: `storage_days` observado; `no_data` (soja, farelo,
## ração) entra como 0 = NÃO decai (ausência explícita, documentada).
func _declare_shelf_life(sim: Object, report: Dictionary) -> void:
	for product in CHAIN_PRODUCTS:
		var entry: Dictionary = _shelf.get(product, {})
		var days := 0
		var status := String(entry.get("data_status", "no_data"))
		if status != "no_data":
			days = int(_num(entry.get("storage_days")))
		sim.set_shelf_life(product, days)
		report["shelf_life"][product] = days


## Perda pós-colheita por produto: média ponderada pela produção nacional do
## dado (`crop_yields.production_t`), porque o contrato do núcleo é por produto.
## `storage_share_pct` só é declarada quando o dado a tem (APHLIS, África).
func _declare_losses(sim: Object, report: Dictionary) -> void:
	for crop in CROPS:
		var weighted := 0.0
		var weight_total := 0.0
		var storage_weighted := 0.0
		var storage_weight_total := 0.0
		for nation in _losses.keys():
			var nation_yields: Dictionary = _yields.get(nation, {}).get("crops", {})
			var production := _num(nation_yields.get(crop, {}).get("production_t"))
			var entry: Dictionary = _losses[nation].get("crops", {}).get(crop, {})
			var loss := _num(entry.get("loss_pct"))
			if production <= 0.0 or loss <= 0.0:
				continue
			weighted += loss * production
			weight_total += production
			var storage: Variant = entry.get("storage_share_pct")
			if storage != null and _num(storage, -1.0) >= 0.0:
				storage_weighted += _num(storage) * production
				storage_weight_total += production
		if weight_total <= 0.0:
			report["loss_pct"][crop] = 0.0
			continue
		var loss_pct := weighted / weight_total
		var storage_share := -1.0
		if storage_weight_total > 0.0:
			storage_share = storage_weighted / storage_weight_total
		sim.set_crop_loss(crop, loss_pct, storage_share)
		report["loss_pct"][crop] = loss_pct


## Leitura de um bloco `{"<chave>": {...}}` do JSON. `{}` quando falta — a
## ausência é explícita (nenhum número é inventado).
func _read_dict(path: String, key: String) -> Dictionary:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		push_warning("AgricultureService: não abriu " + path)
		return {}
	var parsed: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	if typeof(parsed) != TYPE_DICTIONARY:
		push_warning("AgricultureService: JSON inválido " + path)
		return {}
	var block: Variant = (parsed as Dictionary).get(key, {})
	return block if typeof(block) == TYPE_DICTIONARY else {}


## Dados de pecuária (carcaça/leite por cabeça e ciclo). NÃO é declarado ao
## núcleo nesta parte: `feed → meat` já é fluxo contínuo em `recipes.json` e o
## dado declara `seasonality: continuous` (nenhum fator mensal é inventado).
## Leite fica como LACUNA: não existe bem `milk` no jogo.
func livestock_summary() -> Dictionary:
	if not load_data():
		return {}
	return {
		"products": _livestock.get("products", {}),
		"nations": (_livestock.get("nations", {}) as Dictionary).size(),
		"seasonality": "continuous",
		"milk": "lacuna: não existe bem milk no jogo",
	}
