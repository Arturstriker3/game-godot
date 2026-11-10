extends SceneTree

## Teste do contrato único: o modelo demo (ciclo completo) inicializado a partir
## do cenário real (cities/ports/buildings.json) em vez do cenário AGRIA/PORTUS.
## Valida os invariantes em runtime, os campos de compatibilidade do status e a
## ausência de NaN ao longo de uma partida. Roda headless e falha com != 0.

const GeographyServiceScript := preload("res://core/economy/geography_service.gd")

const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const PORTS_PATH := "res://data/definitions/scenario/ports.json"
const BUILDINGS_PATH := "res://data/definitions/scenario/buildings.json"

const DAYS := 60

var _failures: Array[String] = []


func _init() -> void:
	_test_world()
	if _failures.is_empty():
		print("[world] OK: modelo demo rodou %d dias no cenario real sem violacao." % DAYS)
		quit(0)
	else:
		print("[world] FALHAS (%d):" % _failures.size())
		for failure in _failures:
			print(" - " + failure)
		quit(1)


func _fail(msg: String) -> void:
	_failures.append(msg)


func _load_array(path: String) -> Array:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("nao abriu " + path)
		return []
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	if typeof(data) != TYPE_ARRAY:
		_fail(path + " nao e um array JSON")
		return []
	return data


func _test_world() -> void:
	var cities := _load_array(CITIES_PATH)
	var ports := _load_array(PORTS_PATH)
	var buildings := _load_array(BUILDINGS_PATH)
	if cities.is_empty() or buildings.is_empty():
		_fail("cenario vazio (cidades=%d, construcoes=%d)" % [cities.size(), buildings.size()])
		return

	var sim = EconomySim.new()
	sim.initialize_demo_world(cities, ports, buildings)

	# A produção de buildings.csv é efetiva (já reflete a geografia, como no
	# modelo legado). A aptidão de trigo do GeographyService fica para a
	# melhoria 1.9 (terreno/clima na produção); aqui a suitability padrão é 1.0.

	var status: Dictionary = sim.get_demo_status()

	# Contrato de status: campos de compatibilidade com o formato legado.
	for city in status["cities"]:
		var cid := String(city["id"])
		for key in ["x", "y", "nation", "population", "production", "jobs",
				"income", "industrialization", "stockpile", "need"]:
			if not city.has(key):
				_fail("%s: falta campo de compatibilidade '%s'" % [cid, key])
		if not is_finite(float(city["population"])):
			_fail("%s: populacao nao finita" % cid)

	var money_initial := float(status["money_total"])

	for day in DAYS:
		status = sim.tick_demo()
		var violations: Array = status.get("invariant_violations", [])
		if violations.size() > 0:
			_fail("dia %d: invariante violado: %s" % [day + 1, String(violations[0])])
			break
		for city in status["cities"]:
			var cid := String(city["id"])
			for key in ["firm_cash", "service_cash", "government_budget"]:
				if not is_finite(float(city[key])):
					_fail("%s: %s NaN no dia %d" % [cid, key, day + 1])
			for market in city.get("markets", []):
				if not is_finite(float(market["price"])):
					_fail("%s: preco NaN no dia %d" % [cid, day + 1])
		if _failures.size() > 0:
			break

	var money_final := float(status["money_total"])
	print("[world] dinheiro inicial=%.2f final=%.2f drift=%.4f" % [
		money_initial, money_final, money_final - money_initial])
	print("[world] resumo final:")
	for city in status["cities"]:
		var pop: Dictionary = city["pop"]
		print("  %-8s pop=%.0f cons=%.0f/%.0f (%.0f%%) trigo_estoque=%.1f firm=%.0f" % [
			String(city["id"]),
			float(city["population"]),
			float(pop["consumed"]),
			float(pop["need"]),
			float(pop["satisfaction"]) * 100.0,
			float(city["stockpile"]),
			float(city["firm_cash"]),
		])
