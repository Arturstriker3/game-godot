extends RefCounted
class_name RefiningService

## F0/D-124 — REFINO: ponte entre o dado publico e o nucleo.
##
## Declara no nucleo, ANTES do dia 1 (mesmo contrato da geografia, agricultura,
## armazenagem e energia), as construcoes `refinery` que representam a
## capacidade de refino do ano-base:
##
##   * DADO: `data/definitions/transport/refining_capacity.json` — capacidade
##     calibrada do ano-base (D-124): a GEOGRAFIA e a share de cada nacao no
##     refino mundial (Energy Institute, Statistical Review of World Energy
##     2025, 2024); a ESCALA vem do proprio mundo simulado — demanda mundial de
##     gasolina no dia 1 / utilizacao real (0,80), com teto da oferta simulada
##     de petroleo (capacidade das minas x yield 0,45). A fracao simulada
##     continua no JSON como referencia, mas NAO e aplicada (a ancora ja esta na
##     escala do mundo; aplicar as duas descontaria duas vezes).
##   * ALOCACAO (regra declarada no proprio JSON): TODA cidade com mina de
##     petroleo recebe uma `refinery` (o insumo nasce junto), com peso =
##     capacidade de petroleo local; nacao sem mina usa ate 3 cidades-porto do
##     `ports.json` (host city), peso = populacao simulada. Cada cidade recebe
##     a receita `gasoline_a` (co-producao gasolina + diesel + querosene).
##   * EFICIENCIA MADURA: o refino e estoque PRE-EXISTENTE, nao obra nova. O
##     nucleo cria predio com `output_scale` 0,25 (rampa); o servico devolve a
##     escala a 1,0 e o capital por capacidade da receita no save do dia 0.
##   * ORDEM: `seed_industry()` roda ANTES do refino. Ele desiste se o mundo ja
##     tem industria (`recipe >= 0`); sem essa ordem a refinaria bloquearia a
##     farinha/pao do bootstrap.
##
## Nada e inventado: nacao sem linha no dado nao ganha refinaria (ausencia
## honesta), e os agregados regionais "Other ..." nao sao rateados entre paises.

const DATA_PATH := "res://data/definitions/transport/refining_capacity.json"
const PORTS_PATH := "res://data/definitions/scenario/ports.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const RECIPE_ID := "gasoline_a"
const PRODUCT_ID := "gasoline"
const MAX_CITIES_PER_NATION := 3
const MIN_CITY_CAPACITY_T := 1.0
const BUILDING_PREFIX := "SEED-REFINERY-"

var _data: Dictionary = {}
var _capital_per_capacity := 12000.0
var _loaded := false


func load_data() -> bool:
	if _loaded:
		return true
	_data = _read_dict(DATA_PATH)
	_capital_per_capacity = _recipe_capital_per_capacity()
	_loaded = true
	return not (_data.get("by_country", {}) as Dictionary).is_empty()


## Aplica o refino no nucleo. `cities` e a lista do cenario (id + nation +
## population + sim_population); `ports` e a lista de portos do cenario.
## `options.only_nations`: restringe a semear (teste/laboratorio).
func apply_to_sim(sim: Object, cities: Array, ports: Array = [],
		options: Dictionary = {}) -> Dictionary:
	if sim == null or not sim.has_method("build") \
			or not sim.has_method("get_products"):
		return {"applied": 0, "reason": "core_without_build"}
	load_data()
	var by_country: Dictionary = _data.get("by_country", {})
	if by_country.is_empty():
		return {"applied": 0, "reason": "no_data"}

	# Ordem: farinha/pao ANTES do refino (ver o cabecalho).
	if sim.has_method("seed_industry"):
		sim.seed_industry()

	# Save que ja tem refino manda: nada e reescrito.
	if sim.has_method("get_status") and _has_refining(sim):
		return {"applied": 0, "reason": "world_already_has_refining"}

	var products: Array = Array(sim.get_products())
	var product_index := products.find(PRODUCT_ID)
	if product_index < 0:
		return {"applied": 0, "reason": "core_without_gasoline"}

	var only: Dictionary = {}
	for tag: Variant in options.get("only_nations", []):
		only[String(tag)] = true
	var port_cities := _port_cities(cities, ports)
	# D-124: cidade com mina de petroleo tem prioridade (a refinaria nasce junto
	# do insumo; sem isso a planta de cidade-porto sem petroleo local nunca
	# comeca — o mercado de insumo nao abre demanda efetiva no dia 1).
	var oil_cities := _oil_cities(sim)
	var created: Array = []
	var total_capacity := 0.0
	var nations: Array[String] = []
	var skipped: Array[String] = []
	sim.set_instant_construction(true)
	for tag: Variant in by_country:
		var nation := String(tag)
		if not only.is_empty() and not only.has(nation):
			continue
		var entry: Dictionary = by_country[nation]
		var capacity := float(entry.get("capacity_t_per_day", 0.0))
		if capacity <= 0.0:
			continue
		var targets := _targets(nation, cities, port_cities, oil_cities)
		if targets.is_empty():
			skipped.append(nation)
			continue
		var weight_sum := 0.0
		for target: Dictionary in targets:
			weight_sum += float(target.get("weight", 0.0))
		if weight_sum <= 0.0:
			for target: Dictionary in targets:
				target["weight"] = 1.0
			weight_sum = float(targets.size())
		var built_in_nation := 0
		for slot in targets.size():
			var target: Dictionary = targets[slot]
			var city_capacity := capacity \
				* float(target.get("weight", 0.0)) / weight_sum
			if city_capacity < MIN_CITY_CAPACITY_T:
				continue
			var city_id := String(target["id"])
			var building_id := "%s%s-%d" % [BUILDING_PREFIX, city_id, slot]
			var result: Dictionary = sim.build(city_id, building_id,
				"refinery", -1, product_index, city_capacity, 0.0, 0.0, 0.0,
				1.0, "private", 0.0, RECIPE_ID)
			if not bool(result.get("created", false)):
				skipped.append(nation)
				continue
			created.append({
				"id": building_id,
				"city_id": city_id,
				"nation": nation,
				"capacity_t_per_day": city_capacity,
			})
			total_capacity += city_capacity
			built_in_nation += 1
		if built_in_nation > 0:
			nations.append(nation)
	sim.set_instant_construction(false)

	var matured := _mature(sim, created)
	return {
		"applied": created.size(),
		"capacity_t_per_day": total_capacity,
		"nations": nations,
		"skipped": skipped,
		"matured": matured,
		"created": created,
	}


# --- helpers --------------------------------------------------------------

func _read_dict(path: String) -> Dictionary:
	if not FileAccess.file_exists(path):
		return {}
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return {}
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data if typeof(data) == TYPE_DICTIONARY else {}


func _recipe_capital_per_capacity() -> float:
	for item: Variant in Array(_read_array(RECIPES_PATH)):
		var recipe: Dictionary = item
		if String(recipe.get("id", "")) == RECIPE_ID:
			return maxf(1.0, float(recipe.get("capital_per_capacity", 12000.0)))
	return 12000.0


func _read_array(path: String) -> Array:
	if not FileAccess.file_exists(path):
		return []
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return []
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data if typeof(data) == TYPE_ARRAY else []


## Portos por nacao: host city (`port_of`) de cada porto do cenario.
func _port_cities(cities: Array, ports: Array) -> Dictionary:
	var by_nation: Dictionary = {}
	for item: Variant in ports:
		var port: Dictionary = item
		var host := String(port.get("port_of", port.get("host_city", "")))
		var nation := String(port.get("nation", ""))
		if host.is_empty() or nation.is_empty():
			continue
		var known: Dictionary = by_nation.get(nation, {})
		known[host] = true
		by_nation[nation] = known
	return by_nation


## Cidades com mina de petroleo do mundo (id da cidade -> capacidade de
## petroleo instalada, t/dia). Leitura do status; o seed roda antes do dia 1 e
## a mina ja existe na geografia. A capacidade e o peso da planta: a refinaria
## nasce do tamanho do insumo local, nao da populacao da cidade.
func _oil_cities(sim: Object) -> Dictionary:
	var out: Dictionary = {}
	if sim == null or not sim.has_method("get_status") \
			or not sim.has_method("get_city_status"):
		return out
	var products: Array = Array(sim.get_products())
	var oil_index := products.find("oil")
	if oil_index < 0:
		return out
	var status: Dictionary = sim.get_status()
	for city_row: Variant in status.get("cities", []):
		var city: Dictionary = city_row
		var city_id := String(city.get("id", ""))
		var city_status: Dictionary = sim.get_city_status(city_id)
		var capacity := 0.0
		for building_row: Variant in city_status.get("buildings", []):
			var building: Dictionary = building_row
			if int(building.get("output_product", -1)) == oil_index \
					and float(building.get("capacity", 0.0)) > 0.0:
				capacity += float(building.get("capacity", 0.0))
		if capacity > 0.0:
			out[city_id] = capacity
	return out


## Cidades-alvo da nacao (regra declarada): TODA cidade com mina de petroleo
## recebe a planta (o insumo nasce junto), com peso = capacidade de petroleo
## local; sem mina, ate 3 cidades-porto, peso = populacao simulada (sem porto,
## a maior cidade). O teto de 3 e o da regra de portos, nao o da mina: um pais
## com 9 cidades produtoras tem 9 plantas, cada uma do tamanho do seu campo.
func _targets(nation: String, cities: Array, port_cities: Dictionary,
		oil_cities: Dictionary) -> Array:
	var own: Array = []
	var port_ids: Dictionary = port_cities.get(nation, {})
	for item: Variant in cities:
		var city: Dictionary = item
		if String(city.get("nation", "")) != nation:
			continue
		own.append(city)
	if own.is_empty():
		return []
	# 1. Refinaria junto do petroleo: todas as cidades com mina, maiores
	#    campos primeiro. O peso e a capacidade local (t/dia), nao a populacao:
	#    a planta e dimensionada pelo insumo que tem ao lado.
	var oil_rows: Array = []
	for city: Dictionary in own:
		var city_id := String(city.get("id", ""))
		if oil_cities.has(city_id):
			oil_rows.append({
				"id": city_id,
				"weight": maxf(1e-6, float(oil_cities[city_id])),
			})
	oil_rows.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		var left := float(a.get("weight", 0.0))
		var right := float(b.get("weight", 0.0))
		if left == right:
			return String(a.get("id", "")) < String(b.get("id", ""))
		return left > right)
	if not oil_rows.is_empty():
		return oil_rows
	# 2. Nacao sem mina: portos do cenario (host city), peso = populacao.
	var pool: Array = []
	for city: Dictionary in own:
		var city_id := String(city.get("id", ""))
		if port_ids.has(city_id):
			pool.append(city)
	if pool.is_empty():
		pool = own
	pool.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		var left := float(a.get("sim_population", 0.0))
		var right := float(b.get("sim_population", 0.0))
		if left == right:
			return String(a.get("id", "")) < String(b.get("id", ""))
		return left > right)
	var targets: Array = []
	for slot in mini(MAX_CITIES_PER_NATION, pool.size()):
		var city: Dictionary = pool[slot]
		targets.append({
			"id": String(city.get("id", "")),
			"weight": maxf(0.0, float(city.get("sim_population", 0.0))),
		})
	return targets


## O nucleo ja tem refinaria? (save carregado manda; nada e reescrito.)
func _has_refining(sim: Object) -> bool:
	var status: Dictionary = sim.get_status()
	for city_row: Variant in status.get("cities", []):
		var city: Dictionary = city_row
		for building: Variant in city.get("buildings", []):
			if String((building as Dictionary).get("recipe", "")) == RECIPE_ID:
				return true
	return false


## Eficiencia madura: o seed do refino e estoque do ano-base, nao planta nova.
## D-125: a API do nucleo marca so as refinarias criadas (`efficiency = 1`,
## trator/maquina do ano-base), sem tocar em obra nova de save carregado.
func _mature(sim: Object, created: Array) -> int:
	if created.is_empty():
		return 0
	if sim.has_method("mature_world_efficiency"):
		var ids: Array = []
		for entry: Variant in created:
			ids.append(String((entry as Dictionary).get("id", "")))
		sim.mature_world_efficiency(ids)
		return created.size()
	if not sim.has_method("to_dict") or not sim.has_method("from_dict"):
		return 0
	var ids: Dictionary = {}
	for entry: Variant in created:
		ids[String((entry as Dictionary).get("id", ""))] = true
	var save: Dictionary = sim.to_dict()
	var patched := 0
	for row: Variant in save.get("buildings", []):
		var building: Dictionary = row
		if not ids.has(String(building.get("id", ""))):
			continue
		building["output_scale"] = 1.0
		building["last_utilization"] = 1.0
		building["ramp_days"] = 0
		building["capital_per_capacity"] = _capital_per_capacity
		patched += 1
	if patched == 0:
		return 0
	sim.from_dict(save)
	return patched
