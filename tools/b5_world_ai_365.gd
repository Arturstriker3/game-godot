extends SceneTree

## B5 — mundo real com IA por 365 dias: falências, drift de dinheiro, comida
## (satisfação mínima por nação) e a contagem de OBRAS DE INFRA e CONCESSÕES que
## a IA executou. Salva o JSON em docs/economy/perf/b5_world_ai_365.json.
##
## Uso:
##   & $godot --headless --path . --script res://tools/b5_world_ai_365.gd

const GeographyServiceScript := preload("res://core/economy/geography_service.gd")
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

const OUT_PATH := "res://docs/economy/perf/b5_world_ai_365.json"
const DAYS := 365

## `--out=<caminho>` regrava a evidência em outro arquivo (o M2 gera a sua sem
## sobrescrever a da B5). Sem a flag, o destino é o histórico da B5.
var _out := OUT_PATH


func _json(path: String) -> Variant:
	var f := FileAccess.open(path, FileAccess.READ)
	var data: Variant = JSON.parse_string(f.get_as_text())
	f.close()
	return data


func _init() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--out="):
			_out = arg.split("=", true, 1)[1]
			if not _out.begins_with("res://") and not _out.begins_with("user://"):
				_out = "res://" + _out
	var cities: Array = _json(CITIES_PATH)
	var ports: Array = _json(PORTS_PATH)
	var buildings: Array = _json(BUILDINGS_PATH)
	RegionDataScript.attach(cities)
	var sim = EconomySim.new()
	sim.set_goods(_json(GOODS_PATH))
	sim.set_recipes(_json(RECIPES_PATH))
	sim.set_needs(_json(NEEDS_PATH))
	sim.set_services(_json(SERVICES_PATH))
	sim.set_currencies(_json(CURRENCIES_PATH))
	sim.set_sector_employment(_json(SECTOR_EMPLOYMENT_PATH))
	sim.set_age_structure(_json(AGE_STRUCTURE_PATH))
	var geo := GeographyServiceScript.new()
	geo.load_data()
	buildings = geo.resolve_initial_farms(buildings, cities)
	if geo.has_method("resolve_initial_mines"):
		buildings = geo.resolve_initial_mines(buildings, cities)
	sim.initialize_world(cities, ports, buildings)
	sim.set_urban(_json(URBAN_LAND_PATH), _json(HOUSING_CITY_PATH),
		_json(HOUSING_PATH), _json(MUNICIPAL_FINANCE_PATH))
	var centers: Array = []
	for c: Dictionary in cities:
		centers.append({
			"id": String(c.get("id", "")),
			"nation": String(c.get("nation", "")),
			"center": Vector2(float(c.get("x", 0.0)), float(c.get("y", 0.0))),
		})
	geo.apply_to_sim(sim, centers)
	sim.set_ai_enabled(true)

	var money_start := float(sim.get_status().get("money_total", 0.0))
	for day in DAYS:
		sim.tick_day()

	var status: Dictionary = sim.get_status()
	var save: Dictionary = sim.to_dict()
	var money_end := float(status.get("money_total", 0.0))

	# Contagem de infra (cada nível > 0 = uma obra concluída) e concessões.
	var infra_road := 0
	var infra_power := 0
	var infra_water := 0
	var regions_with_infra := 0
	for city: Dictionary in save.get("cities", []):
		var infra: Dictionary = city.get("region", {}).get("infra", {})
		var r := int(infra.get("road", 0))
		var p := int(infra.get("power", 0))
		var w := int(infra.get("water", 0))
		infra_road += r
		infra_power += p
		infra_water += w
		if r + p + w > 0:
			regions_with_infra += 1
	var concessions := 0
	for b: Dictionary in save.get("buildings", []):
		if float(b.get("royalty", 0.0)) > 0.0:
			concessions += 1

	# Satisfação alimentar mínima por nação (comida/segurança alimentar).
	var min_nation := ""
	var min_sat := 1.0
	var by_nation := {}
	for city: Dictionary in save.get("cities", []):
		var nation := String(city.get("nation", ""))
		var sat := float(city.get("pop", {}).get("satisfaction", 1.0))
		if not by_nation.has(nation):
			by_nation[nation] = 1.0
		by_nation[nation] = minf(float(by_nation[nation]), sat)
	for nation in by_nation:
		if float(by_nation[nation]) < min_sat:
			min_sat = float(by_nation[nation])
			min_nation = nation

	var out := {
		"days": DAYS,
		"bankruptcies": int(status.get("bankruptcies", -1)),
		"money_start": money_start,
		"money_end": money_end,
		"money_drift": money_end - money_start,
		"infra_road": infra_road,
		"infra_power": infra_power,
		"infra_water": infra_water,
		"infra_total": infra_road + infra_power + infra_water,
		"regions_with_infra": regions_with_infra,
		"concessions": concessions,
		"min_food_satisfaction": min_sat,
		"min_food_nation": min_nation,
	}
	var f := FileAccess.open(_out, FileAccess.WRITE)
	f.store_string(JSON.stringify(out, "  "))
	f.close()
	print("[b5-365] falências=%d drift=%.6f comida_min=%s(%.4f) infra=%d(%d estrada/%d luz/%d água em %d regiões) concessões=%d"
		% [out["bankruptcies"], out["money_drift"], min_nation, min_sat,
		infra_road + infra_power + infra_water, infra_road, infra_power,
		infra_water, regions_with_infra, concessions])
	print("[b5-365] evidência em " + _out)
	quit(0)
