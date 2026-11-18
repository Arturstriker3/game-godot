extends SceneTree

## B4 — urbanização VIVA ao longo de 365 dias (mundo real, IA + urbano), por
## país (top 10 do roster) e mundo, no dia 0 × dia 365, com os fluxos anuais.
##
## Lê a população do SAVE (urbano vivo = `sim_population`, interior vivo =
## `region.region_pop`) e confere a taxa do mundo contra
## `get_status().migration.urbanization` (mesma fórmula: urbano ÷ (urbano+interior)).
##
## Uso:
##   & $godot --headless --path . --script res://tools/b4_urbanization_365.gd
##   --out=docs/economy/perf/b4_urbanization_365.json

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

const DEFAULT_OUT := "res://docs/economy/perf/b4_urbanization_365.json"
const DAYS := 365
## Top 10 do roster por população (mesma lista do sweep/docs da B4).
const MAIN_NATIONS: Array[String] = [
	"BRA", "CHN", "IND", "NGA", "ETH", "USA", "IDN", "PAK", "BGD", "EGY",
]


func _json(path: String) -> Variant:
	var f := FileAccess.open(path, FileAccess.READ)
	var data: Variant = JSON.parse_string(f.get_as_text())
	f.close()
	return data


## Soma por nação: urbano vivo, interior vivo e acumuladores de migração.
func _aggregate(save: Dictionary) -> Dictionary:
	var by_nation := {}
	var world := {"urban": 0.0, "region": 0.0, "rural_in": 0.0,
		"city_in": 0.0, "city_out": 0.0}
	for city: Dictionary in save.get("cities", []):
		var nation := String(city.get("nation", ""))
		var region: Dictionary = city.get("region", {})
		var migration: Dictionary = region.get("migration", {})
		var urban := float(city.get("sim_population", 0.0))
		var rural := float(region.get("region_pop", 0.0))
		var rural_in := float(migration.get("rural_in_total", 0.0))
		var city_in := float(migration.get("city_in_total", 0.0))
		var city_out := float(migration.get("city_out_total", 0.0))
		if not by_nation.has(nation):
			by_nation[nation] = {"urban": 0.0, "region": 0.0,
				"rural_in": 0.0, "city_in": 0.0, "city_out": 0.0}
		var b: Dictionary = by_nation[nation]
		b["urban"] += urban
		b["region"] += rural
		b["rural_in"] += rural_in
		b["city_in"] += city_in
		b["city_out"] += city_out
		world["urban"] += urban
		world["region"] += rural
		world["rural_in"] += rural_in
		world["city_in"] += city_in
		world["city_out"] += city_out
	return {"world": world, "nations": by_nation}


func _urban_share(block: Dictionary) -> float:
	var urban := float(block["urban"])
	var total := urban + float(block["region"])
	return urban / total if total > 0.0 else 0.0


func _row(start: Dictionary, end: Dictionary) -> Dictionary:
	return {
		"urban_start": float(start["urban"]),
		"region_start": float(start["region"]),
		"urbanization_start": _urban_share(start),
		"urban_end": float(end["urban"]),
		"region_end": float(end["region"]),
		"urbanization_end": _urban_share(end),
		"rural_in_total": float(end["rural_in"]),
		"city_in_total": float(end["city_in"]),
		"city_out_total": float(end["city_out"]),
	}


func _init() -> void:
	var out_path := DEFAULT_OUT
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--out="):
			out_path = "res://" + arg.substr(6)
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

	var start := _aggregate(sim.to_dict())
	var status_start: Dictionary = sim.get_status().get("migration", {})
	for day in DAYS:
		sim.tick_day()
	var end := _aggregate(sim.to_dict())
	var status_end: Dictionary = sim.get_status().get("migration", {})
	var world_status := float(status_end.get("urbanization", 0.0))

	var world_row := _row(start["world"], end["world"])
	if absf(world_row["urbanization_end"] - world_status) > 1e-9:
		push_error("urbanização do mundo (save) %.9f != status %.9f" % [
			world_row["urbanization_end"], world_status])
		print("[b4-urban] FALHA: status %.9f != save %.9f" % [
			world_status, world_row["urbanization_end"]])
		quit(1)
		return
	print("[b4-urban] mundo: urbanization %.6f → %.6f (status %.6f), rural_in %.0f, city_in %.0f, city_out %.0f" % [
		world_row["urbanization_start"], world_row["urbanization_end"],
		world_status, world_row["rural_in_total"],
		world_row["city_in_total"], world_row["city_out_total"]])

	var nations: Array = []
	var by_start: Dictionary = start["nations"]
	var by_end: Dictionary = end["nations"]
	for tag in MAIN_NATIONS:
		if not by_start.has(tag) or not by_end.has(tag):
			continue
		var row := _row(by_start[tag], by_end[tag])
		row["tag"] = tag
		nations.append(row)
		print("[b4-urban] %s urbanization %.6f → %.6f rural_in %.0f" % [
			tag, row["urbanization_start"], row["urbanization_end"],
			row["rural_in_total"]])

	var report := {
		"meta": {
			"days": DAYS,
			"ai": true,
			"urban": true,
			"threads": 1,
			"formula": "urbanization = urbano / (urbano + interior), populações vivas",
			"world_status_urbanization_end": world_status,
		},
		"world": world_row,
		"nations": nations,
	}
	var f := FileAccess.open(out_path, FileAccess.WRITE)
	f.store_string(JSON.stringify(report, "  "))
	f.close()
	print("[b4-urban] evidência em %s" % out_path)
	quit(0)
