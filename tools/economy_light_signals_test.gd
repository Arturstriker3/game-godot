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

	# Construções (CSR): reconstrói do bloco pesado e compara com a tabela plana.
	var b_offset: PackedInt32Array = light.get("b_offset", PackedInt32Array())
	var b_id: PackedStringArray = light.get("b_id", PackedStringArray())
	var b_output_product: PackedInt32Array = light.get("b_output_product", PackedInt32Array())
	var b_owner: PackedStringArray = light.get("b_owner", PackedStringArray())
	var b_capacity: PackedFloat64Array = light.get("b_capacity", PackedFloat64Array())
	var b_utilization: PackedFloat64Array = light.get("b_utilization", PackedFloat64Array())
	var b_costs: PackedFloat64Array = light.get("b_costs", PackedFloat64Array())
	var b_site: PackedFloat64Array = light.get("b_site_suitability", PackedFloat64Array())
	if b_offset.size() != city_count + 1:
		_fail("light.b_offset tamanho %d != %d" % [b_offset.size(), city_count + 1])
		return
	var checked_buildings := 0
	for city_index in cities.size():
		var city: Dictionary = cities[city_index]
		var buildings: Array = city.get("buildings", [])
		var bstart: int = int(b_offset[city_index])
		var bend: int = int(b_offset[city_index + 1])
		if bend - bstart != buildings.size():
			_fail("cidade %d: b_offset span %d != buildings %d" % [
				city_index, bend - bstart, buildings.size()])
			return
		for index in range(bstart, bend):
			var b: Dictionary = buildings[index - bstart]
			if b_id[index] != String(b.get("id", "")):
				_fail("cidade %d construção %d: b_id light=%s heavy=%s" % [
					city_index, index - bstart, b_id[index], String(b.get("id", ""))])
				return
			if b_output_product[index] != int(b.get("output_product", -1)):
				_fail("cidade %d construção %d: b_output_product light=%d heavy=%d" % [
					city_index, index - bstart, b_output_product[index],
					int(b.get("output_product", -1))])
				return
			if b_owner[index] != String(b.get("owner", "")):
				_fail("cidade %d construção %d: b_owner light=%s heavy=%s" % [
					city_index, index - bstart, b_owner[index], String(b.get("owner", ""))])
				return
			if b_capacity[index] != float(b.get("capacity", 0.0)):
				_fail("cidade %d construção %d: b_capacity light=%.17g heavy=%.17g" % [
					city_index, index - bstart, b_capacity[index],
					float(b.get("capacity", 0.0))])
				return
			if b_utilization[index] != float(b.get("utilization", 0.0)):
				_fail("cidade %d construção %d: b_utilization light=%.17g heavy=%.17g" % [
					city_index, index - bstart, b_utilization[index],
					float(b.get("utilization", 0.0))])
				return
			if b_costs[index] != float(b.get("costs", 0.0)):
				_fail("cidade %d construção %d: b_costs light=%.17g heavy=%.17g" % [
					city_index, index - bstart, b_costs[index],
					float(b.get("costs", 0.0))])
				return
			if b_site[index] != float(b.get("site_suitability", 1.0)):
				_fail("cidade %d construção %d: b_site_suitability light=%.17g heavy=%.17g" % [
					city_index, index - bstart, b_site[index],
					float(b.get("site_suitability", 1.0))])
				return
			checked_buildings += 1

	# Jazidas (CSR): reconstrói do bloco pesado e compara com a tabela plana.
	var d_offset: PackedInt32Array = light.get("d_offset", PackedInt32Array())
	var d_resource: PackedStringArray = light.get("d_resource", PackedStringArray())
	var d_initial: PackedFloat64Array = light.get("d_initial_kt", PackedFloat64Array())
	var d_remaining: PackedFloat64Array = light.get("d_remaining_kt", PackedFloat64Array())
	var d_quality: PackedFloat64Array = light.get("d_quality", PackedFloat64Array())
	var d_depth: PackedFloat64Array = light.get("d_depth_m", PackedFloat64Array())
	var d_efficiency: PackedFloat64Array = light.get("d_efficiency", PackedFloat64Array())
	var d_opex: PackedFloat64Array = light.get("d_opex_factor", PackedFloat64Array())
	var d_extracted: PackedFloat64Array = light.get("d_extracted_kt", PackedFloat64Array())
	var d_depleted: PackedInt32Array = light.get("d_depleted", PackedInt32Array())
	if d_offset.size() != city_count + 1:
		_fail("light.d_offset tamanho %d != %d" % [d_offset.size(), city_count + 1])
		return
	var checked_deposits := 0
	for city_index in cities.size():
		var city: Dictionary = cities[city_index]
		var deposits: Dictionary = city.get("deposits", {})
		var dstart: int = int(d_offset[city_index])
		var dend: int = int(d_offset[city_index + 1])
		if dend - dstart != deposits.size():
			_fail("cidade %d: d_offset span %d != deposits %d" % [
				city_index, dend - dstart, deposits.size()])
			return
		for index in range(dstart, dend):
			var resource := String(d_resource[index])
			if not deposits.has(resource):
				_fail("cidade %d: jazida light %s sem bloco pesado" % [city_index, resource])
				return
			var entry: Dictionary = deposits[resource]
			if d_initial[index] != float(entry.get("initial_kt", 0.0)):
				_fail("cidade %d %s: d_initial_kt light=%.17g heavy=%.17g" % [
					city_index, resource, d_initial[index],
					float(entry.get("initial_kt", 0.0))])
				return
			if d_remaining[index] != float(entry.get("remaining_kt", 0.0)):
				_fail("cidade %d %s: d_remaining_kt light=%.17g heavy=%.17g" % [
					city_index, resource, d_remaining[index],
					float(entry.get("remaining_kt", 0.0))])
				return
			if d_quality[index] != float(entry.get("quality", 0.0)):
				_fail("cidade %d %s: d_quality light=%.17g heavy=%.17g" % [
					city_index, resource, d_quality[index],
					float(entry.get("quality", 0.0))])
				return
			if d_depth[index] != float(entry.get("depth_m", 0.0)):
				_fail("cidade %d %s: d_depth_m light=%.17g heavy=%.17g" % [
					city_index, resource, d_depth[index],
					float(entry.get("depth_m", 0.0))])
				return
			if d_efficiency[index] != float(entry.get("efficiency", 0.0)):
				_fail("cidade %d %s: d_efficiency light=%.17g heavy=%.17g" % [
					city_index, resource, d_efficiency[index],
					float(entry.get("efficiency", 0.0))])
				return
			if d_opex[index] != float(entry.get("opex_factor", 0.0)):
				_fail("cidade %d %s: d_opex_factor light=%.17g heavy=%.17g" % [
					city_index, resource, d_opex[index],
					float(entry.get("opex_factor", 0.0))])
				return
			if d_extracted[index] != float(entry.get("extracted_kt", 0.0)):
				_fail("cidade %d %s: d_extracted_kt light=%.17g heavy=%.17g" % [
					city_index, resource, d_extracted[index],
					float(entry.get("extracted_kt", 0.0))])
				return
			if d_depleted[index] != (1 if bool(entry.get("depleted", false)) else 0):
				_fail("cidade %d %s: d_depleted light=%d heavy=%d" % [
					city_index, resource, d_depleted[index],
					1 if bool(entry.get("depleted", false)) else 0])
				return
			checked_deposits += 1

	print("[light_signals] %d mercados × %d cidades, %d incumbentes, %d construções e %d jazidas conferidos." % [
		checked_markets, cities.size(), expected_city.size(), checked_buildings,
		checked_deposits])
