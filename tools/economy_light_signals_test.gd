extends SceneTree

## Teste dos sinais leves por cidade (S7-c, fatia 1): confere que os arrays
## planos de `status["light"]` são bit a bit idênticos aos campos do bloco
## pesado por cidade (markets/pop/buildings). É a guarda de equivalência que
## a migração da IA (fatia 2) usa para garantir as MESMAS decisões. Roda
## headless e falha com código diferente de zero.

const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"
const SERVICES_PATH := "res://data/definitions/economy/services.json"
const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const PORTS_PATH := "res://data/definitions/scenario/ports.json"
const BUILDINGS_PATH := "res://data/definitions/scenario/buildings.json"

var _failures: Array[String] = []


func _init() -> void:
	_run()
	if _failures.is_empty():
		print("[light_signals] OK: sinais leves == bloco pesado por cidade.")
		quit(0)
	else:
		print("[light_signals] FALHAS (%d):" % _failures.size())
		for failure in _failures:
			print(" - " + failure)
		quit(1)


func _fail(message: String) -> void:
	_failures.append(message)


func _load_array(path: String) -> Array:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("dados: nao abriu " + path)
		return []
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data if typeof(data) == TYPE_ARRAY else []


func _load_dict(path: String) -> Dictionary:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("dados: nao abriu " + path)
		return {}
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data if typeof(data) == TYPE_DICTIONARY else {}


func _run() -> void:
	var sim = EconomySim.new()
	if not sim.has_method("get_city_status"):
		_fail("binário sem get_city_status (DLL desatualizada?)")
		return
	sim.demo_set_goods(_load_array(GOODS_PATH))
	sim.demo_set_recipes(_load_array(RECIPES_PATH))
	sim.demo_set_needs(_load_dict(NEEDS_PATH))
	sim.demo_set_services(_load_dict(SERVICES_PATH))
	sim.initialize_demo_world(_load_array(CITIES_PATH), _load_array(PORTS_PATH),
		_load_array(BUILDINGS_PATH))

	for day in 3:
		sim.tick_demo()

	var status: Dictionary = sim.get_demo_status()
	var cities: Array = status.get("cities", [])
	if cities.is_empty():
		_fail("status sem cidades")
		return
	var light: Dictionary = status.get("light", {})
	if light.is_empty():
		_fail("status sem bloco 'light'")
		return
	var products: int = int(light.get("product_count", 0))
	var city_count: int = int(light.get("city_count", 0))
	if products <= 0:
		_fail("light sem product_count")
		return
	if city_count != cities.size():
		_fail("light.city_count (%d) != cidades (%d)" % [city_count, cities.size()])
		return

	var market_fields := {
		"m_price": "price", "m_stock": "stock", "m_supply": "supply",
		"m_surplus": "surplus", "m_sold": "sold", "m_shortage": "shortage",
		"m_desired": "desired",
	}
	var pop_fields := {
		"p_need": "need", "p_consumed": "consumed",
		"p_satisfaction": "satisfaction", "p_labor_force": "labor_force",
		"p_unemployed": "unemployed",
	}
	var market_len: int = city_count * products
	for key: String in market_fields:
		var arr: PackedFloat64Array = light.get(key, PackedFloat64Array())
		if arr.size() != market_len:
			_fail("light.%s tamanho %d != %d" % [key, arr.size(), market_len])
	for key: String in pop_fields:
		var arr: PackedFloat64Array = light.get(key, PackedFloat64Array())
		if arr.size() != city_count:
			_fail("light.%s tamanho %d != %d" % [key, arr.size(), city_count])

	if not _failures.is_empty():
		return

	# Lê os arrays uma vez só (PackedFloat64Array lê por índice sem ponte).
	var arrays: Dictionary = {}
	for key: String in market_fields:
		arrays[key] = light[key]
	for key: String in pop_fields:
		arrays[key] = light[key]

	var checked_markets := 0
	for city_index in cities.size():
		var city: Dictionary = cities[city_index]
		var markets: Array = city.get("markets", [])
		var pop: Dictionary = city.get("pop", {})
		for product in products:
			if product >= markets.size():
				_fail("cidade %d sem mercado %d" % [city_index, product])
				return
			var market: Dictionary = markets[product]
			for key: String in market_fields:
				var expected := float(market.get(market_fields[key], 0.0))
				var got := float((arrays[key] as PackedFloat64Array)[city_index * products + product])
				if expected != got:
					_fail("cidade %d produto %d %s: light=%.17g heavy=%.17g" % [
						city_index, product, market_fields[key], got, expected])
					return
			checked_markets += 1
		for key: String in pop_fields:
			var expected := float(pop.get(pop_fields[key], 0.0))
			var got := float((arrays[key] as PackedFloat64Array)[city_index])
			if expected != got:
				_fail("cidade %d %s: light=%.17g heavy=%.17g" % [
					city_index, pop_fields[key], got, expected])
				return

	# Incumbentes: reconstrói do bloco pesado e compara com a tabela plana.
	var expected_city: Array[int] = []
	var expected_resource: Array[String] = []
	var expected_group: Array[String] = []
	for city_index in cities.size():
		var city: Dictionary = cities[city_index]
		for building: Dictionary in city.get("buildings", []):
			var owner_group := String(building.get("owner_group", ""))
			if owner_group.is_empty():
				continue
			var building_resource := String(building.get("resource", ""))
			if building_resource.is_empty():
				continue
			expected_city.append(city_index)
			expected_resource.append(building_resource)
			expected_group.append(owner_group)

	var i_city: PackedInt32Array = light.get("i_city", PackedInt32Array())
	var i_resource: PackedStringArray = light.get("i_resource", PackedStringArray())
	var i_group: PackedStringArray = light.get("i_owner_group", PackedStringArray())
	if i_city.size() != expected_city.size() \
			or i_resource.size() != expected_city.size() \
			or i_group.size() != expected_city.size():
		_fail("incumbentes: light (%d,%d,%d) != esperado %d" % [
			i_city.size(), i_resource.size(), i_group.size(), expected_city.size()])
		return
	for index in expected_city.size():
		if i_city[index] != expected_city[index] \
				or i_resource[index] != expected_resource[index] \
				or i_group[index] != expected_group[index]:
			_fail("incumbente %d difere: light=(%d,%s,%s) heavy=(%d,%s,%s)" % [
				index, i_city[index], i_resource[index], i_group[index],
				expected_city[index], expected_resource[index], expected_group[index]])
			return

	print("[light_signals] %d mercados × %d cidades e %d incumbentes conferidos." % [
		checked_markets, cities.size(), expected_city.size()])
