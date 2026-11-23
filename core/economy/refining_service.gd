extends RefCounted
class_name RefiningService

## F0/D-124 — REFINO: ponte entre o dado publico e o nucleo.
##
## Declara no nucleo, ANTES do dia 1 (mesmo contrato da geografia, agricultura,
## armazenagem e energia), as construcoes `refinery` que representam a
## capacidade de refino do ano-base:
##
##   * DADO: `data/definitions/transport/refining_capacity.json` — capacidade de
##     destilacao atmosferica 2024 por nacao (Energy Institute, Statistical
##     Review of World Energy 2025), ja convertida para t/dia (x 0,1364 t/barril
##     da EIA) e multiplicada pela FRACAO SIMULADA da nacao (populacao simulada /
##     exibida, a mesma razao do parque eletrico — D-116/D-119).
##   * ALOCACAO (regra declarada no proprio JSON): ate 3 cidades-porto do
##     `ports.json` (host city) por nacao, peso = populacao simulada; nacao sem
##     porto usa a maior cidade. Cada cidade recebe uma `refinery` com a receita
##     `gasoline_a` (co-producao gasolina + diesel + querosene).
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
		var targets := _targets(nation, cities, port_cities)
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


## Cidades-alvo da nacao (regra declarada): ate 3 cidades-porto, peso =
## populacao simulada; sem porto, a maior cidade.
func _targets(nation: String, cities: Array, port_cities: Dictionary) -> Array:
	var own: Array = []
	var port_ids: Dictionary = port_cities.get(nation, {})
	for item: Variant in cities:
		var city: Dictionary = item
		if String(city.get("nation", "")) != nation:
			continue
		own.append(city)
	if own.is_empty():
		return []
	var pool: Array = []
	for city: Dictionary in own:
		if port_ids.has(String(city.get("id", ""))):
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


## Eficiencia madura: rampa de planta nova (0,25) vira 1,0 e o capital por
## capacidade volta ao valor da receita (o seed nao e investimento novo).
func _mature(sim: Object, created: Array) -> int:
	if created.is_empty() or not sim.has_method("to_dict") \
			or not sim.has_method("from_dict"):
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
