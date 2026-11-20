extends SceneTree

## Guarda de retrocompatibilidade do save v11 (revisão Q-009, pré-S6 `93c5440`).
##
## O pré-S6 adicionou 5 campos ao save sem subir o schema (continua v11):
##   `bankruptcies`, `depleted_mines`, `industry_seeded` (raiz) e, por
##   construção, `bankrupt`/`depleted`. A promessa é: um save v11 gerado ANTES
##   de `93c5440` (sem esses campos) carrega SEM erro e com defaults sensatos
##   (`bankruptcies=0`, `depleted_mines=0`, `industry_seeded=false`,
##   `bankrupt=false`, `depleted=false`).
##
## Também valida que nomes não-ASCII (Córdoba, São Paulo, Québec…) sobrevivem ao
##   round-trip de encoding: o lado `std::string -> String` do save usa
##   `sim_str()` (String::utf8), não `String(const char*)` (Latin-1), que antes
##   duplicava a codificação a cada load (Córdoba -> CÃ³rdoba -> CÃÂ³rdoba…).
##
##   godot --headless --path . --script res://tools/economy_save_load_v11_compat_test.gd

const GeographyServiceScript := preload("res://core/economy/geography_service.gd")

const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const PORTS_PATH := "res://data/definitions/scenario/ports.json"
const BUILDINGS_PATH := "res://data/definitions/scenario/buildings.json"
const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"
const SERVICES_PATH := "res://data/definitions/economy/services.json"
const CURRENCIES_PATH := "res://data/definitions/economy/currencies.json"
const SECTOR_EMPLOYMENT_PATH := "res://data/definitions/economy/sector_employment.json"

## Campos aditivos do pré-S6 (93c5440) que um save v11 antigo NÃO tem.
const NEW_ROOT_KEYS := ["bankruptcies", "depleted_mines", "industry_seeded"]
const NEW_BUILDING_KEYS := ["bankrupt", "depleted"]

var _failures: Array[String] = []


func _init() -> void:
	_test_ascii_roundtrip()
	_test_v11_old_save_compat()
	if _failures.is_empty():
		print("[v11compat] OK: nomes não-ASCII preservados e save v11 antigo carrega com defaults sensatos.")
		quit(0)
	else:
		for failure in _failures:
			print("[v11compat] FALHA: " + failure)
		quit(1)


func _fail(msg: String) -> void:
	_failures.append(msg)


func _load_json(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("nao abriu " + path)
		return null
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data


func _load_array(path: String) -> Array:
	var data: Variant = _load_json(path)
	if typeof(data) != TYPE_ARRAY:
		_fail(path + " nao e um array JSON")
		return []
	return data


func _load_dict(path: String) -> Dictionary:
	var data: Variant = _load_json(path)
	if typeof(data) != TYPE_DICTIONARY:
		_fail(path + " nao e um objeto JSON")
		return {}
	return data


func _load_chains(sim) -> void:
	sim.set_goods(_load_array(GOODS_PATH))
	sim.set_recipes(_load_array(RECIPES_PATH))
	sim.set_needs(_load_dict(NEEDS_PATH))
	if sim.has_method("set_services"):
		sim.set_services(_load_dict(SERVICES_PATH))
	if sim.has_method("set_currencies"):
		sim.set_currencies(_load_dict(CURRENCIES_PATH))
	if sim.has_method("set_sector_employment"):
		var sector := _load_dict(SECTOR_EMPLOYMENT_PATH)
		if not sector.is_empty():
			sim.set_sector_employment(sector)


func _make_world() -> Object:
	var sim = EconomySim.new()
	_load_chains(sim)
	var cities := _load_array(CITIES_PATH)
	var ports := _load_array(PORTS_PATH)
	var buildings := _load_array(BUILDINGS_PATH)
	if cities.is_empty() or buildings.is_empty():
		_fail("cenario vazio")
		return sim
	sim.initialize_world(cities, ports, buildings)
	return sim


## Mapa id -> nome a partir do cities.json original (fonte de verdade dos
## nomes, incluindo os não-ASCII).
func _original_city_names() -> Dictionary:
	var names := {}
	var cities := _load_array(CITIES_PATH)
	for city in cities:
		var id := String(city.get("id", ""))
		var name := String(city.get("name", ""))
		if id != "":
			names[id] = name
	return names


## Os nomes de cidade do save batem com o cities.json original? Se o encoding
## estivesse errado, "Córdoba" viraria "CÃ³rdoba" (e pioraria a cada load).
func _check_city_names(save: Dictionary, expected: Dictionary, label: String) -> void:
	var cities: Array = save.get("cities", [])
	if cities.is_empty():
		_fail(label + ": save sem cidades")
		return
	var mismatches := 0
	for city in cities:
		var id := String(city.get("id", ""))
		var name := String(city.get("name", ""))
		var original: String = String(expected.get(id, ""))
		if original == "":
			continue
		if name != original:
			mismatches += 1
			if mismatches <= 5:
				_fail(label + ": nome corrompido '%s' -> '%s' (id %s)" % [original, name, id])
	if mismatches == 0:
		print("[v11compat] %s: %d nomes de cidade batem com o JSON original." % [label, cities.size()])


## Teste 1: nomes não-ASCII sobrevivem ao round-trip (save -> load -> save).
func _test_ascii_roundtrip() -> void:
	var sim := _make_world()
	for day in 2:
		var status: Dictionary = sim.tick_day()
		var violations: Array = status.get("invariant_violations", [])
		if violations.size() > 0:
			_fail("ascii: dia %d: invariante violado" % [day + 1])
			return

	var expected := _original_city_names()
	var first_save: Dictionary = sim.to_dict()
	_check_city_names(first_save, expected, "1º save")

	# Round-trip: carrega num sim novo e re-serializa. A codificação não pode
	# piorar (antes do fix, cada load duplicava o mojibake).
	var restored = EconomySim.new()
	_load_chains(restored)
	restored.from_dict(first_save)
	var second_save: Dictionary = restored.to_dict()
	_check_city_names(second_save, expected, "pós-load")


## Teste 2: um save v11 gerado ANTES de 93c5440 (sem os campos novos) carrega
## sem erro e com defaults sensatos. Simula-se removendo as chaves novas.
func _test_v11_old_save_compat() -> void:
	var sim := _make_world()
	sim.tick_day()
	var full_save: Dictionary = sim.to_dict()

	if int(full_save.get("version", 0)) != 30:
		_fail("compat: esperava schema v30, veio %s" % str(full_save.get("version")))
		return

	# Garante que os campos novos EXISTEM no save atual (senão o teste de
	# remoção não estaria exercitando o caminho do pré-S6).
	for key in NEW_ROOT_KEYS:
		if not full_save.has(key):
			_fail("compat: save atual não tem o campo novo '%s'" % key)
	# §15 (v12): o save atual também publica as obras em andamento.
	if not full_save.has("construction_projects"):
		_fail("compat: save atual não tem o campo novo 'construction_projects'")

	# Constrói o "save v11 antigo": sem as chaves novas e sem as obras (v11 não
	# tinha projeto em voo). Rebaixa a versão para 11 para exercitar a migração.
	var old_save: Dictionary = full_save.duplicate(true)
	for key in NEW_ROOT_KEYS:
		old_save.erase(key)
	old_save.erase("construction_projects")
	old_save["version"] = 11
	var buildings: Array = old_save.get("buildings", [])
	for building in buildings:
		for key in NEW_BUILDING_KEYS:
			if (building as Dictionary).has(key):
				(building as Dictionary).erase(key)
	old_save["buildings"] = buildings

	var restored = EconomySim.new()
	_load_chains(restored)
	restored.from_dict(old_save)  # não pode falhar

	var round: Dictionary = restored.to_dict()
	# Defaults sensatos: os campos voltam com o valor default do load.
	if int(round.get("bankruptcies", -1)) != 0:
		_fail("compat: bankruptcies default deveria ser 0 (é %s)" % str(round.get("bankruptcies")))
	if int(round.get("depleted_mines", -1)) != 0:
		_fail("compat: depleted_mines default deveria ser 0 (é %s)" % str(round.get("depleted_mines")))
	if bool(round.get("industry_seeded", true)) != false:
		_fail("compat: industry_seeded default deveria ser false")
	for building in round.get("buildings", []):
		if bool((building as Dictionary).get("bankrupt", true)) != false:
			_fail("compat: building.bankrupt default deveria ser false")
			break
		if bool((building as Dictionary).get("depleted", true)) != false:
			_fail("compat: building.depleted default deveria ser false")
			break
	print("[v11compat] save v11 antigo (sem os 5 campos novos) carregou sem erro e com defaults 0/false.")
