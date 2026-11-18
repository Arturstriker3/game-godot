extends SceneTree

## B7 — mundo real com IA por 365 dias: falências, drift de dinheiro, comida
## (satisfação mín./média por nação), MORTES EXTRAS POR FOME por país (top 10,
## `mortality.excess_famine_total` acumulado por cidade/região), informalidade
## inicial × final (mundo + amostra) e insatisfação média por quintil (mundo).
## Salva o JSON em docs/economy/perf/b7_world_ai_365.json.
##
## Uso:
##   & $godot --headless --path . --script res://tools/b7_world_ai_365.gd

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
const INFORMALITY_PATH := "res://data/definitions/economy/informality.json"
const HEALTH_ACCESS_PATH := "res://data/definitions/population/health_access.json"
const URBAN_LAND_PATH := "res://data/definitions/economy/urban_land.json"
const HOUSING_CITY_PATH := "res://data/definitions/economy/housing_city.json"
const HOUSING_PATH := "res://data/definitions/economy/housing.json"
const MUNICIPAL_FINANCE_PATH := "res://data/definitions/economy/municipal_finance.json"

const OUT_PATH := "res://docs/economy/perf/b7_world_ai_365.json"
const DAYS := 365
const SAMPLE_TAGS: Array[String] = ["USA", "BRA", "IND", "NGA", "DEU", "JPN"]


func _json(path: String) -> Variant:
	var f := FileAccess.open(path, FileAccess.READ)
	var data: Variant = JSON.parse_string(f.get_as_text())
	f.close()
	return data


## Média ponderada por população do bloco informal de uma lista de cidades.
func _informality_share(cities: Array) -> float:
	var weighted := 0.0
	var pop := 0.0
	for c: Dictionary in cities:
		var urban_pop := float(c.get("pop", {}).get("population", 0.0))
		var urban_share := float(c.get("pop", {}).get("informal", {}).get("share", 0.0))
		var region: Dictionary = c.get("region", {})
		var region_pop := 0.0
		var region_share := 0.0
		if not region.is_empty():
			region_pop = float(region.get("region_pop", 0.0))
			region_share = float(region.get("informal", {}).get("share", 0.0))
		weighted += urban_share * urban_pop + region_share * region_pop
		pop += urban_pop + region_pop
	return weighted / pop if pop > 0.0 else 0.0


func _init() -> void:
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
	sim.set_informality(_json(INFORMALITY_PATH))
	sim.set_health_access(_json(HEALTH_ACCESS_PATH))
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
	var initial: Dictionary = sim.to_dict()
	var initial_cities: Array = initial.get("cities", [])

	# Violação de invariante faz o núcleo REVERTER o tick (`restore_state`): o
	# dia não avança e o mundo trava. Registramos cada ocorrência e os dias
	# parados — sem isso o laço de 365 iterações esconde o congelamento.
	var violations: Array = []
	var violation_count := 0
	var stalled_days := 0
	var day_before := int(sim.get_status().get("day", 0))
	for day in DAYS:
		sim.tick_day()
		var summary: Dictionary = sim.get_day_summary()
		for violation: Variant in summary.get("invariant_violations", []):
			++violation_count
			if violations.size() < 10:
				violations.append("iteração %d: %s" % [day + 1, String(violation)])
		var day_now := int(summary.get("day", 0))
		if day_now <= day_before:
			++stalled_days
		day_before = day_now

	var status: Dictionary = sim.get_status()
	var save: Dictionary = sim.to_dict()
	var money_end := float(status.get("money_total", 0.0))
	var final_cities: Array = save.get("cities", [])

	# --- Comida (satisfação alimentar urbana) mín./média por nação. ---------
	var food_min := 1.0
	var food_min_nation := ""
	var food_by_nation := {}
	for c: Dictionary in final_cities:
		var tag := String(c.get("nation", ""))
		var sat := float(c.get("pop", {}).get("satisfaction", 1.0))
		var pop := float(c.get("pop", {}).get("population", 0.0))
		if not food_by_nation.has(tag):
			food_by_nation[tag] = {"min": 1.0, "weighted": 0.0, "pop": 0.0}
		food_by_nation[tag]["min"] = minf(float(food_by_nation[tag]["min"]), sat)
		food_by_nation[tag]["weighted"] += sat * pop
		food_by_nation[tag]["pop"] += pop
		if sat < food_min:
			food_min = sat
			food_min_nation = tag
	var food_mean_by_nation := {}
	for tag in food_by_nation:
		var e: Dictionary = food_by_nation[tag]
		food_mean_by_nation[tag] = e["weighted"] / e["pop"] if float(e["pop"]) > 0.0 else 0.0

	# --- Mortes extras por fome (urbano + interior) por nação. ---------------
	var famine_by_nation := {}
	for c: Dictionary in final_cities:
		var tag := String(c.get("nation", ""))
		var urban := float(c.get("pop", {}).get("mortality", {}).get(
			"excess_famine_total", 0.0))
		var region: Dictionary = c.get("region", {})
		var interior := 0.0
		if not region.is_empty():
			interior = float(region.get("mortality", {}).get(
				"excess_famine_total", 0.0))
		famine_by_nation[tag] = float(famine_by_nation.get(tag, 0.0)) \
			+ urban + interior
	var famine_rows: Array = []
	for tag in famine_by_nation:
		famine_rows.append({"nation": tag, "total": float(famine_by_nation[tag])})
	famine_rows.sort_custom(func(a, b): return float(a["total"]) > float(b["total"]))
	var famine_top10: Array = famine_rows.slice(0, 10)

	# --- Informalidade inicial × final (mundo + amostra). --------------------
	var informal_world := {
		"initial": _informality_share(initial_cities),
		"final": _informality_share(final_cities),
	}
	var informal_nations := {}
	for tag: String in SAMPLE_TAGS:
		var init_cities := []
		var fin_cities := []
		for c: Dictionary in initial_cities:
			if String(c.get("nation", "")) == tag:
				init_cities.append(c)
		for c: Dictionary in final_cities:
			if String(c.get("nation", "")) == tag:
				fin_cities.append(c)
		informal_nations[tag] = {
			"initial": _informality_share(init_cities),
			"final": _informality_share(fin_cities),
		}

	# --- Insatisfação média por quintil (mundo), ponderada por população. ----
	var diss_q := [0.0, 0.0, 0.0, 0.0, 0.0]
	var diss_pop := 0.0
	for c: Dictionary in final_cities:
		var pop_block: Dictionary = c.get("pop", {})
		var strata: Dictionary = pop_block.get("strata", {})
		var people: Array = strata.get("people", [])
		var by_q: Array = pop_block.get("dissatisfaction", {}).get("by_stratum", [])
		for s in 5:
			var p := float(people[s]) if s < people.size() else 0.0
			var v := float(by_q[s]) if s < by_q.size() else 0.0
			diss_q[s] += v * p
			diss_pop += p
		var region: Dictionary = c.get("region", {})
		if not region.is_empty():
			var r_strata: Dictionary = region.get("strata", {})
			var r_people: Array = r_strata.get("people", [])
			var r_by_q: Array = region.get("dissatisfaction", {}).get("by_stratum", [])
			for s in 5:
				var p := float(r_people[s]) if s < r_people.size() else 0.0
				var v := float(r_by_q[s]) if s < r_by_q.size() else 0.0
				diss_q[s] += v * p
				diss_pop += p
	if diss_pop > 0.0:
		for s in 5:
			diss_q[s] = diss_q[s] / diss_pop

	var out := {
		"days": DAYS,
		"final_day": int(status.get("day", 0)),
		"stalled_days": stalled_days,
		"invariant_violations": violation_count,
		"invariant_violation_samples": violations,
		"bankruptcies": int(status.get("bankruptcies", -1)),
		"money_start": money_start,
		"money_end": money_end,
		"money_drift": money_end - money_start,
		"food": {
			"min_satisfaction": food_min,
			"min_nation": food_min_nation,
			"mean_by_nation": food_mean_by_nation,
		},
		"famine_excess_deaths_top10": famine_top10,
		"informality": {
			"world": informal_world,
			"nations": informal_nations,
		},
		"dissatisfaction_by_quintile_world": diss_q,
	}
	var f := FileAccess.open(OUT_PATH, FileAccess.WRITE)
	f.store_string(JSON.stringify(out, "  "))
	f.close()
	var top: Dictionary = famine_top10[0] if famine_top10.size() > 0 \
		else {"nation": "-", "total": 0.0}
	print("[b7-365] dia_final=%d travados=%d violações=%d falências=%d drift=%.6f comida_min=%s(%.4f) fome_top=%s(%.0f) informal_mundo=%.3f→%.3f diss_q0=%.3f"
		% [out["final_day"], stalled_days, violation_count, out["bankruptcies"],
		out["money_drift"], food_min_nation, food_min,
		String(top["nation"]), float(top["total"]),
		informal_world["initial"], informal_world["final"], diss_q[0]])
	if violation_count > 0:
		for sample: String in violations:
			print("[b7-365] violação: " + sample)
	quit(0)
