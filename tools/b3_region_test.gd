extends SceneTree

## B3 — REGIÃO DA CIDADE (estados inteiros). Teste de dado + estado + save.
##
## Etapa 1 (commit 1) valida a contagem de população (invariante dura), a
## semeadura do bloco da região, o round-trip de save sem tick e a migração
## v17 → v18. A etapa 2 (economia da região) estende este arquivo com
## conservação de dinheiro, consumo cidade↔região e determinismo de threads.
##
## Uso: godot --headless --path . --script res://tools/b3_region_test.gd

const RegionDataScript := preload("res://core/economy/region_data.gd")

const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const PORTS_PATH := "res://data/definitions/scenario/ports.json"
const BUILDINGS_PATH := "res://data/definitions/scenario/buildings.json"
const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"
const SERVICES_PATH := "res://data/definitions/economy/services.json"
const CURRENCIES_PATH := "res://data/definitions/economy/currencies.json"
const SECTOR_EMPLOYMENT_PATH := "res://data/definitions/economy/sector_employment.json"
const AGE_STRUCTURE_PATH := "res://data/definitions/population/age_structure_2026.json"
const URBAN_LAND_PATH := "res://data/definitions/economy/urban_land.json"
const HOUSING_CITY_PATH := "res://data/definitions/economy/housing_city.json"
const HOUSING_PATH := "res://data/definitions/economy/housing.json"
const MUNICIPAL_FINANCE_PATH := "res://data/definitions/economy/municipal_finance.json"

## Soma global de urban + interior (== soma de population_region == nações).
const EXPECTED_TOTAL := 8203501062.0
## Cidades com região 0 (urbano >= population_region; urbano capado).
const EXPECTED_ZERO_REGION := 18
const TOLERANCE := 0.5

var _failures: Array = []


func _init() -> void:
	_run()
	if _failures.is_empty():
		print("[b3] OK: população, semeadura, round-trip e migração v17→v18.")
		quit(0)
	else:
		print("[b3] FALHAS (%d):" % _failures.size())
		for failure: String in _failures:
			print(" - " + failure)
		quit(1)


func _fail(message: String) -> void:
	_failures.append(message)


func _json(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("não abriu " + path)
		return null
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data


func _load_chains(sim: Object) -> void:
	sim.set_goods(_json(GOODS_PATH))
	sim.set_recipes(_json(RECIPES_PATH))
	sim.set_needs(_json(NEEDS_PATH))
	if sim.has_method("set_services"):
		sim.set_services(_json(SERVICES_PATH))
	if sim.has_method("set_currencies"):
		sim.set_currencies(_json(CURRENCIES_PATH))
	if sim.has_method("set_sector_employment"):
		sim.set_sector_employment(_json(SECTOR_EMPLOYMENT_PATH))
	if sim.has_method("set_age_structure"):
		sim.set_age_structure(_json(AGE_STRUCTURE_PATH))


## Monta o mundo real (cities/ports/buildings.json) com a região anexada.
func _make_world() -> Object:
	var sim = EconomySim.new()
	_load_chains(sim)
	var cities: Array = _json(CITIES_PATH)
	var ports: Array = _json(PORTS_PATH)
	var buildings: Array = _json(BUILDINGS_PATH)
	RegionDataScript.attach(cities)
	sim.initialize_world(cities, ports, buildings)
	if sim.has_method("set_urban"):
		sim.set_urban(_json(URBAN_LAND_PATH), _json(HOUSING_CITY_PATH),
			_json(HOUSING_PATH), _json(MUNICIPAL_FINANCE_PATH))
	return sim


func _city_blocks(save: Dictionary) -> Array:
	return save.get("cities", [])


## (a) soma global urban + interior = 8.203.501.062 exatos.
func _check_global_sum(save: Dictionary) -> void:
	var total := 0.0
	var urban := 0.0
	var region := 0.0
	for city: Dictionary in _city_blocks(save):
		var region_block: Dictionary = city.get("region", {})
		urban += float(city.get("sim_population", 0.0))
		region += float(region_block.get("region_pop", 0.0))
		total += float(city.get("population", 0.0))
	if absf(total - EXPECTED_TOTAL) > TOLERANCE:
		_fail("soma global da população %f != %f" % [total, EXPECTED_TOTAL])
	if absf((urban + region) - EXPECTED_TOTAL) > TOLERANCE:
		_fail("soma global urban+interior %f != %f" % [
			urban + region, EXPECTED_TOTAL])
	print("[b3] soma global: total=%.0f urban=%.0f interior=%.0f" % [
		total, urban, region])


## (b) por cidade: urban + interior == population_region.
func _check_per_city(save: Dictionary) -> void:
	var mismatches := 0
	for city: Dictionary in _city_blocks(save):
		var region_block: Dictionary = city.get("region", {})
		var urban := float(city.get("sim_population", 0.0))
		var region_pop := float(region_block.get("region_pop", 0.0))
		var total := float(city.get("population", 0.0))
		if absf((urban + region_pop) - total) > TOLERANCE:
			mismatches += 1
			if mismatches <= 3:
				_fail("%s: urban+interior %f != population %f" % [
					String(city.get("id", "?")), urban + region_pop, total])
	if mismatches > 0:
		_fail("por cidade: %d cidades com urban+interior != population" % mismatches)
	else:
		print("[b3] por cidade: urban+interior == population em todas as cidades.")


## (c) 18 cidades com região 0 (urbano capado em population_region).
func _check_zero_regions(save: Dictionary) -> void:
	var zero := 0
	var capped := 0
	for city: Dictionary in _city_blocks(save):
		var region_block: Dictionary = city.get("region", {})
		if float(region_block.get("region_pop", 0.0)) <= 0.0:
			zero += 1
		if float(region_block.get("urban_population", 0.0)) \
				== float(region_block.get("population_region", 0.0)):
			capped += 1
	if zero != EXPECTED_ZERO_REGION:
		_fail("cidades com região 0: %d != %d" % [zero, EXPECTED_ZERO_REGION])
	print("[b3] cidades com região 0: %d (urbano capado em %d)." % [zero, capped])


## (d) save → load sem tick: round-trip bit-exato (JSON idêntico).
func _check_save_load_roundtrip(sim: Object) -> void:
	var snapshot: Dictionary = sim.to_dict()
	var restored = EconomySim.new()
	_load_chains(restored)
	restored.from_dict(snapshot)
	if JSON.stringify(restored.to_dict()) != JSON.stringify(snapshot):
		_fail("round-trip sem tick não preserva o estado (JSON difere)")
	else:
		print("[b3] round-trip sem tick: bit-exato.")


## (e) save v17 (sem o bloco `region`) → v18 carrega sem inventar número.
func _check_v17_migration() -> void:
	var sim := _make_world()
	var snapshot: Dictionary = sim.to_dict()
	var v17: Dictionary = snapshot.duplicate(true)
	v17["version"] = 17
	var cities: Array = v17.get("cities", []).duplicate(true)
	for city: Dictionary in cities:
		city.erase("region")
	v17["cities"] = cities
	var migrated = EconomySim.new()
	_load_chains(migrated)
	migrated.from_dict(v17)
	var after: Dictionary = migrated.to_dict()
	if int(after.get("version", 0)) != 18:
		_fail("migração: versão deveria ser 18 (é %s)" % str(after.get("version")))
		return
	var checked := 0
	for city: Dictionary in after.get("cities", []):
		var region_block: Dictionary = city.get("region", {})
		if not bool(region_block.get("ready", false)):
			_fail("%s: migração não marcou a região" % String(city.get("id", "?")))
			continue
		var expected := maxf(0.0,
			float(city.get("population", 0.0)) - float(city.get("sim_population", 0.0)))
		if absf(float(region_block.get("region_pop", 0.0)) - expected) > TOLERANCE:
			_fail("%s: região migrada %f != max(0, population-sim) %f" % [
				String(city.get("id", "?")),
				float(region_block.get("region_pop", 0.0)), expected])
		checked += 1
	if checked == 0:
		_fail("migração: nenhuma cidade com região")
	else:
		print("[b3] migração v17→v18: %d cidades carregadas sem crash." % checked)


func _run() -> void:
	var sim := _make_world()
	var save: Dictionary = sim.to_dict()
	_check_global_sum(save)
	_check_per_city(save)
	_check_zero_regions(save)
	_check_save_load_roundtrip(sim)
	_check_v17_migration()
