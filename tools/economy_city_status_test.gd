extends SceneTree

## Teste do `get_city_status(city_id)` (S7-c, fatia final): o bloco por cidade
## do `get_status()` agora é LEVE (sem markets/pop/services/buildings/
## deposits) e o detalhe pesado vive em `get_city_status(id)`. Este guarda
## confere que (1) o detalhe sob demanda tem as chaves pesadas, (2) o bloco
## diário NÃO as tem, e (3) os escalares leves coincidem entre os dois. Roda
## headless e falha com código diferente de zero.

const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"
const SERVICES_PATH := "res://data/definitions/economy/services.json"

var _failures: Array[String] = []


func _init() -> void:
	_run()
	if _failures.is_empty():
		print("[city_status] OK: detalhe sob demanda completo; bloco diario leve.")
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
	sim.set_goods(_load_json_array(GOODS_PATH))
	sim.set_recipes(_load_json_array(RECIPES_PATH))
	sim.set_needs(_load_json_dict(NEEDS_PATH))
	sim.set_services(_load_json_dict(SERVICES_PATH))
	sim.initialize_scenario()

	# Roda alguns dias para os campos diários (mercados, POP, fluxos) mudarem.
	for day in 3:
		sim.tick_day()

	var status: Dictionary = sim.get_status()
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
		# (1) Detalhe sob demanda tem o bloco pesado completo (markets/pop/
		# buildings/deposits sempre; `services` só quando o catálogo F8 carrega).
		for key in ["markets", "pop", "buildings", "deposits"]:
			if not on_demand.has(key):
				_fail("get_city_status(%s) sem bloco pesado '%s'" % [city_id, key])
		if status.has("service_sectors") and not on_demand.has("services"):
			_fail("get_city_status(%s) sem bloco pesado 'services'" % city_id)
		# (2) O bloco diário é leve: NÃO publica os blocos pesados.
		for key in ["markets", "pop", "services", "buildings", "deposits"]:
			if city.has(key):
				_fail("bloco diário de %s ainda publica '%s' (deveria ser leve)" % [city_id, key])
		# (3) Escalares leves coincidem entre o bloco diário e o sob demanda.
		for key in ["id", "name", "nation", "population", "simulation_population",
				"gdp", "gdp_per_capita", "firm_cash", "service_cash",
				"government_budget", "production", "jobs", "income",
				"industrialization", "stockpile", "need", "deficit", "consumed",
				"in_transit", "fx_rate", "tariff_revenue", "productivity_index",
				"living_standard", "disposable_per_capita"]:
			if str(city.get(key, "")) != str(on_demand.get(key, "")):
				_fail("get_city_status(%s) difere do bloco diário em '%s' (%s vs %s)" % [
					city_id, key, str(city.get(key, "")), str(on_demand.get(key, ""))])

	# Cidade inexistente devolve dicionário vazio (ausência declarada, não erro).
	if not sim.get_city_status("NAO_EXISTE").is_empty():
		_fail("get_city_status(cidade inexistente) deveria devolver {}")
