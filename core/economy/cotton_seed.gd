extends RefCounted
class_name CottonSeedService

## G3 lote 1, correção 3 — ALGODÃO (a montante do tecido).
##
## O lote 1 semeia `clothes`/`fabric`/`shoes` em 90% da âncora. A oferta de
## algodão do dia 0 (as 3 cadeias finais de 20 t/dia do R2/Q-014) não cobre o
## tecido semeado: o algodão vira o gargalo, o custo unitário do tecido sobe e
## a cadeia inteira reprova no gate. Este serviço declara a ÁREA de safra de
## algodão dos PRODUTORES REAIS no dia 0 pelo MESMO contrato dos planos de
## safra (`set_crop_plan`) — nenhuma construção nova é criada.
##
## D-125 (rampa de 365 d): o plano de safra SEM construção colhe na eficiência
## madura (`eff::harvest_factor` = 1,0 quando não há fazenda rastreada para o
## par — ver `efficiency.h`), então a área nova entra madura no dia 0, como as
## plantas do bootstrap. Nenhum tratamento especial é necessário.
##
## ALVO (medido no mundo, nunca inventado):
##   min(capacidade instalada de `fabric`, demanda instalada de tecido da
##   cadeia) × algodão por tecido (`recipes.json`) − capacidade de algodão já
##   instalada. A capacidade vem do status do núcleo (plantas do seed do G2/G3
##   + cadeias legadas) e a demanda de tecido das receitas que consomem
##   `fabric` (clothes, shoes, uniforms, ...).
##
## RATEIO: participação de PRODUÇÃO dos produtores reais (`cotton_seed.json`,
## USDA FAS) e, dentro da nação, área cultivável × aptidão da cidade
## (`GeographyService`), só onde a aptidão ≥ `suitability_min`.
##
## JANELA: contínua declarada (1..365). O dado do jogo não tem calendário de
## algodão (`crop_calendar.json` cobre só os 5 primários de comida); inventar
## meses de plantio/colheita seria pior. Entrega 1/365 do orçamento por dia.
##
## LIGADO só com a semente industrial do G2/G3 (`bootstrap.enabled`): com a
## semente desligada nada é declarado e o baseline OFF fica bit-idêntico.

const DATA_PATH := "res://data/definitions/agriculture/cotton_seed.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const BOOTSTRAP_PATH := "res://data/definitions/economy/bootstrap_industry.json"
const GOOD := "cotton"
const YEAR_DAYS := 365
## Piso de população de cidade do seed industrial (mesmo gate do bootstrap):
## cidade abaixo disso não recebe planta semeada. Só usamos o piso para
## limitar a varredura do status pesado (buildings), não para decidir nada.
const CITY_SCAN_MIN_POPULATION := 1000000.0

var _data: Dictionary = {}
var _loaded := false


func load_data() -> bool:
	if _loaded:
		return true
	_data = _read_dict(DATA_PATH)
	_loaded = not _data.is_empty()
	return _loaded


## Declara os planos de safra de algodão. `cities` no formato de `cities.json`
## (id/nation/sim_population/x/y). Devolve o relatório do que foi declarado.
func apply_to_sim(sim: Object, cities: Array,
		options: Dictionary = {}) -> Dictionary:
	var report: Dictionary = {
		"applied": 0,
		"plans": 0,
		"reason": "ok",
		"target_t_day": 0.0,
		"area_ha": 0.0,
		"fabric_capacity_t_day": 0.0,
		"final_capacity_t_day": 0.0,
		"existing_cotton_t_day": 0.0,
		"unseeded_share": 0.0,
		"producers": [],
		"yield_t_ha": 0.0,
	}
	if sim == null or not sim.has_method("set_crop_plan") \
			or not sim.has_method("get_status"):
		report["reason"] = "core_without_crop_plan"
		return report
	if not load_data():
		report["reason"] = "no_data"
		return report
	if not _bootstrap_enabled(sim, options):
		report["reason"] = "bootstrap_off"
		return report
	var chain := _chain()
	if chain.is_empty():
		report["reason"] = "no_textile_chain"
		return report
	var capacity := _installed_capacity(sim, cities, chain)
	report["fabric_capacity_t_day"] = capacity["fabric"]
	report["final_capacity_t_day"] = capacity["final"]
	report["existing_cotton_t_day"] = capacity["cotton"]
	var cotton_per_fabric := float(chain.get("cotton_per_fabric", 0.0))
	var target := maxf(0.0, minf(float(capacity["fabric"]),
		float(capacity["final"])) * cotton_per_fabric
		- float(capacity["cotton"]))
	if target <= 0.0:
		report["reason"] = "no_textile_capacity"
		return report
	report["target_t_day"] = target

	var yield_t_ha := float(_data.get("yield_t_ha", 0.0))
	if yield_t_ha <= 0.0:
		report["reason"] = "no_yield"
		return report
	report["yield_t_ha"] = yield_t_ha
	var suitability_min := float(_data.get("suitability_min", 0.30))
	var capacity_min := float(_data.get("capacity_min_t_day", 1.0))
	var max_cities := maxi(1, int(_data.get("max_cities_per_nation", 3)))
	var seasons := _seasons()
	if seasons.is_empty():
		report["reason"] = "no_calendar"
		return report

	# Geografia: mesma regra de área por cidade do AgricultureService (bacia de
	# captação da cidade, aptidão média ponderada). Reaproveita a instância do
	# chamador (`options.geo`, já carregada); sem ela, carrega uma própria.
	var geo = options.get("geo", null)
	if geo == null:
		geo = GeographyService.new()
		if not geo.load_data():
			report["reason"] = "no_geography"
			return report
		geo.assign_provinces(cities)
	var by_nation: Dictionary = {}
	for city in cities:
		var tag := String((city as Dictionary).get("nation", ""))
		if tag == "":
			continue
		if not by_nation.has(tag):
			by_nation[tag] = []
		(by_nation[tag] as Array).append(city)

	var producers: Dictionary = _data.get("producers", {})
	var nations: Array = producers.keys()
	nations.sort()
	var applied := 0
	var area_total := 0.0
	var unseeded := maxf(0.0, 1.0 - _share_sum(producers))
	var used: Array = []
	for tag: String in nations:
		var share := float(producers.get(tag, 0.0))
		if share <= 0.0:
			continue
		var nation_target := share * target
		if nation_target < capacity_min:
			unseeded += share
			continue
		# Cidades com aptidão para o algodão, peso = área cultivável × aptidão.
		var rows: Array = []
		for city: Dictionary in (by_nation.get(tag, []) as Array):
			var city_id := String(city.get("id", ""))
			var aptitude: float = geo.city_crop_suitability(city_id, GOOD)
			if aptitude < suitability_min:
				continue
			var area_km2: float = geo.city_cultivable_area_km2(city_id)
			var weight: float = area_km2 * aptitude
			if weight <= 0.0:
				continue
			rows.append({
				"id": city_id,
				"weight": weight,
				"aptitude": aptitude,
			})
		if rows.is_empty():
			unseeded += share
			continue
		rows.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
			var left := float(a.get("weight", 0.0))
			var right := float(b.get("weight", 0.0))
			if left == right:
				return String(a.get("id", "")) < String(b.get("id", ""))
			return left > right)
		var picked: Array = rows.slice(0, mini(max_cities, rows.size()))
		var weight_sum := 0.0
		for row: Dictionary in picked:
			weight_sum += float(row["weight"])
		if weight_sum <= 0.0:
			unseeded += share
			continue
		var nation_applied := 0
		var nation_area := 0.0
		for row: Dictionary in picked:
			var city_target := nation_target * float(row["weight"]) / weight_sum
			if city_target < capacity_min:
				continue
			# `area_ha` do plano é ANUAL (área colhida), como no
			# AgricultureService: o orçamento do ano é área × rendimento.
			var area_ha := city_target * float(YEAR_DAYS) / yield_t_ha
			if area_ha <= 0.0:
				continue
			sim.set_crop_plan(String(row["id"]), GOOD, area_ha, yield_t_ha,
				seasons)
			nation_applied += 1
			nation_area += area_ha
		if nation_applied == 0:
			unseeded += share
			continue
		# O que a cidade não absorveu (abaixo de `capacity_min_t_day`) é sobra
		# declarada, não escondida.
		var applied_tons := nation_area * yield_t_ha
		if applied_tons < nation_target:
			unseeded += share * (1.0 - applied_tons / nation_target)
		applied += nation_applied
		area_total += nation_area
		used.append({"nation": tag, "share": share,
			"cities": nation_applied, "area_ha": nation_area})

	report["applied"] = applied
	report["plans"] = applied
	report["area_ha"] = area_total
	report["producers"] = used
	report["producer_nations"] = _nation_labels(used)
	report["unseeded_share"] = unseeded
	if applied == 0:
		report["reason"] = "no_eligible_city"
	return report


func _nation_labels(used: Array) -> String:
	var labels: Array = []
	for row: Variant in used:
		labels.append(String((row as Dictionary).get("nation", "")))
	return ",".join(labels)


## Só com a semente industrial ligada (G2/G3): OFF fica bit-idêntico. O dado do
## bootstrap vive no núcleo (`status.bootstrap.enabled`), a mesma fonte que o
## seed industrial usa — não há flag paralela.
func _bootstrap_enabled(sim: Object, options: Dictionary) -> bool:
	if options.has("enabled"):
		return bool(options["enabled"])
	var status: Dictionary = sim.get_status()
	var bootstrap: Dictionary = status.get("bootstrap", {})
	return bool(bootstrap.get("enabled", false))


## Cadeia do algodão a partir das receitas (sem número hardcoded):
##   * algodão por tecido = insumo `cotton` da receita que produz `fabric`;
##   * consumidores diretos de `fabric` e quanto usam por unidade.
func _chain() -> Dictionary:
	var recipes: Variant = _read_array(RECIPES_PATH)
	if typeof(recipes) != TYPE_ARRAY:
		return {}
	var cotton_per_fabric := 0.0
	var consumers: Dictionary = {}
	for entry: Variant in (recipes as Array):
		var recipe: Dictionary = entry
		var output := String(recipe.get("output", ""))
		var yield_t := maxf(0.000001, float(recipe.get("yield", 1.0)))
		var fabric_qty := 0.0
		for input_row: Variant in (recipe.get("inputs", []) as Array):
			var input: Dictionary = input_row
			var good := String(input.get("good", ""))
			var qty := float(input.get("qty", 0.0))
			if good == GOOD and output == "fabric":
				cotton_per_fabric += qty / yield_t
			elif good == "fabric" and output != "fabric":
				fabric_qty += qty / yield_t
		if fabric_qty > 0.0:
			consumers[output] = float(consumers.get(output, 0.0)) + fabric_qty
	if cotton_per_fabric <= 0.0 or consumers.is_empty():
		return {}
	return {"cotton_per_fabric": cotton_per_fabric, "consumers": consumers}


## Capacidades instaladas do dia 0, medidas no status do núcleo (nenhuma
## construção é criada aqui): tecido, demanda de tecido da cadeia (em t de
## tecido/dia) e algodão já produzido pelas lavouras semeadas antes.
##
## O status diário NÃO publica a lista de construções (bloco pesado): só
## `get_city_status` publica. Para não varrer as 849 cidades, a varredura fica
## nas cidades ≥ 1 M das nações produtoras do tecido/algodão (o bootstrap só
## planta em cidade ≥ 1 M; as cadeias legadas idem), lidas do próprio dado.
func _installed_capacity(sim: Object, cities: Array,
		chain: Dictionary) -> Dictionary:
	var products: Array = Array(sim.get_products())
	var fabric_index := products.find("fabric")
	var consumers: Dictionary = chain.get("consumers", {})
	var consumer_index: Dictionary = {}
	for output: String in consumers.keys():
		var index := products.find(output)
		if index >= 0:
			consumer_index[index] = float(consumers[output])
	var cotton_index := products.find(GOOD)
	var scan_nations := _scan_nations()
	var fabric := 0.0
	var final := 0.0
	var cotton := 0.0
	if fabric_index < 0:
		return {"fabric": fabric, "final": final, "cotton": cotton}
	for city: Dictionary in cities:
		var tag := String(city.get("nation", ""))
		if not scan_nations.has(tag):
			continue
		var population := float(city.get("sim_population", 0.0))
		if population < CITY_SCAN_MIN_POPULATION:
			continue
		var city_status: Dictionary = sim.get_city_status(
			String(city.get("id", "")))
		for building_row: Variant in (city_status.get("buildings", []) as Array):
			var building: Dictionary = building_row
			var output := int(building.get("output_product", -1))
			var capacity := float(building.get("capacity", 0.0))
			if capacity <= 0.0:
				continue
			if output == fabric_index:
				fabric += capacity
			elif consumer_index.has(output):
				final += capacity * float(consumer_index[output])
			elif output == cotton_index:
				cotton += capacity
	return {"fabric": fabric, "final": final, "cotton": cotton}


## Nações que podem ter planta do tecido ou lavoura de algodão: produtores do
## próprio algodão (dado local) + produtores das receitas que produzem tecido e
## dos bens que consomem tecido (dado da semente industrial do G2/G3).
func _scan_nations() -> Dictionary:
	var out: Dictionary = {}
	var producers: Dictionary = _data.get("producers", {})
	for tag: Variant in producers.keys():
		out[String(tag)] = true
	var bootstrap := _read_dict(BOOTSTRAP_PATH)
	for entry: Variant in (bootstrap.get("goods", []) as Array):
		var good: Dictionary = entry
		var id := String(good.get("id", ""))
		if id != "fabric" and not _chain_consumers().has(id):
			continue
		var good_producers: Dictionary = good.get("producers", {})
		for tag: Variant in good_producers.keys():
			out[String(tag)] = true
	return out


var _consumers_cache: Dictionary = {}


func _chain_consumers() -> Dictionary:
	if _consumers_cache.is_empty():
		var chain := _chain()
		_consumers_cache = chain.get("consumers", {})
	return _consumers_cache


## Janela contínua declarada do dado (sem meses inventados).
func _seasons() -> Array:
	var calendar: Dictionary = _data.get("calendar", {})
	if String(calendar.get("window", "")) != "continuous":
		return []
	var plant_start := int(calendar.get("plant_start", 1))
	var plant_end := int(calendar.get("plant_end", YEAR_DAYS))
	var harvest_start := int(calendar.get("harvest_start", 1))
	var harvest_end := int(calendar.get("harvest_end", YEAR_DAYS))
	if plant_end < plant_start or harvest_end < harvest_start:
		return []
	return [{
		"plant_start": plant_start,
		"plant_end": plant_end,
		"harvest_start": harvest_start,
		"harvest_end": harvest_end,
		"share": 1.0,
	}]


func _share_sum(producers: Dictionary) -> float:
	var total := 0.0
	for key: Variant in producers:
		total += float(producers[key])
	return total


func _read_dict(path: String) -> Dictionary:
	if not FileAccess.file_exists(path):
		return {}
	var parsed: Variant = JSON.parse_string(FileAccess.get_file_as_string(path))
	return parsed if typeof(parsed) == TYPE_DICTIONARY else {}


func _read_array(path: String) -> Array:
	if not FileAccess.file_exists(path):
		return []
	var parsed: Variant = JSON.parse_string(FileAccess.get_file_as_string(path))
	return parsed if typeof(parsed) == TYPE_ARRAY else []
