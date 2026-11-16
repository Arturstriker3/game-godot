extends SceneTree

## Teste do `get_city_status(city_id)` (S7-c, fatia aditiva): confere que o
## detalhe de uma cidade sob demanda é campo a campo idêntico ao bloco por
## cidade do `get_demo_status()` — mesma simulação, mesma cidade. Roda headless
## e falha com código diferente de zero.

const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"
const SERVICES_PATH := "res://data/definitions/economy/services.json"

var _failures: Array[String] = []


func _init() -> void:
	_run()
	if _failures.is_empty():
		print("[city_status] OK: get_city_status == bloco por cidade do status.")
		quit(0)
	else:
		print("[city_status] FALHAS (%d):" % _failures.size())
		for failure in _failures:
			print(" - " + failure)
		quit(1)


func _load_json_array(path: String) -> Array:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("dados: nao abriu " + path)
		return []
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data if typeof(data) == TYPE_ARRAY else []


func _load_json_dict(path: String) -> Dictionary:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("dados: nao abriu " + path)
		return {}
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data if typeof(data) == TYPE_DICTIONARY else {}


func _fail(message: String) -> void:
	_failures.append(message)


func _city_by_id(status: Dictionary, city_id: String) -> Dictionary:
	for city in status.get("cities", []):
		if String(city.get("id", "")) == city_id:
			return city
	return {}


func _run() -> void:
	var sim = EconomySim.new()
	if not sim.has_method("get_city_status"):
		_fail("binário sem get_city_status (DLL desatualizada?)")
		return
	sim.demo_set_goods(_load_json_array(GOODS_PATH))
	sim.demo_set_recipes(_load_json_array(RECIPES_PATH))
	sim.demo_set_needs(_load_json_dict(NEEDS_PATH))
	sim.demo_set_services(_load_json_dict(SERVICES_PATH))
	sim.initialize_demo()

	# Roda alguns dias para os campos diários (mercados, POP, fluxos) mudarem.
	for day in 3:
		sim.tick_demo()

	var status: Dictionary = sim.get_demo_status()
	var cities: Array = status.get("cities", [])
	if cities.is_empty():
		_fail("status sem cidades")
		return

	for city in cities:
		var city_id := String(city.get("id", ""))
		if city_id.is_empty():
			continue
		var on_demand: Dictionary = sim.get_city_status(city_id)
		if on_demand.is_empty():
			_fail("get_city_status(%s) devolveu vazio" % city_id)
			continue
		# Compara a serialização canônica (chaves ordenadas) dos dois dicionários.
		var a: String = JSON.stringify(city, "", true, true)
		var b: String = JSON.stringify(on_demand, "", true, true)
		if a != b:
			_fail("get_city_status(%s) difere do bloco do status diário" % city_id)
		# Chave pesada presente (mercados) como amostra do detalhe completo.
		if not on_demand.has("markets") or not on_demand.has("pop"):
			_fail("get_city_status(%s) sem markets/pop" % city_id)

	# Cidade inexistente devolve dicionário vazio (ausência declarada, não erro).
	if not sim.get_city_status("NAO_EXISTE").is_empty():
		_fail("get_city_status(cidade inexistente) deveria devolver {}")
