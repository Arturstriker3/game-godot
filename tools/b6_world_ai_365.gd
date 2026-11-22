extends SceneTree

## B6 — mundo real com IA por 365 dias: falências, drift de dinheiro,
## violações de invariante e o QUE A B6 MUDOU no mundo.
##
## Mede patrimônio da região (terra, moradia do interior e anel), população e
## fluxo para o anel, receita potencial do imposto (alíquota 0), Gini de
## riqueza do mundo e por nação (top 5 / bottom 5), e compara com o mundo SEM
## a B6 quando `--control=1` (a mesma configuração, sem semear o patrimônio).
##
## Salva o JSON em docs/economy/perf/b6_world_ai_365.json (ou no `--out=`).
##
##   & $godot --headless --path . --script res://tools/b6_world_ai_365.gd
##   & $godot --headless --path . --script res://tools/b6_world_ai_365.gd -- \
##       --days=365 --control=1 --out=res://docs/economy/perf/b6_world_ai_365_control.json

const GeographyServiceScript := preload("res://core/economy/geography_service.gd")
const RegionDataScript := preload("res://core/economy/region_data.gd")
const LandPropertyScript := preload("res://core/economy/land_property.gd")

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

const OUT_PATH := "res://docs/economy/perf/b6_world_ai_365.json"
const DEFAULT_DAYS := 365
## Portão de comida do M2 (`docs/economy/M2_marco.md`): nação com a média de
## 30 dias abaixo de 0,975 está fora do portão.
const FOOD_GATE := 0.975
const FOOD_WINDOW := 30

var _days := DEFAULT_DAYS
var _control := false
var _out := ""
var _profile := false
var _failures: Array[String] = []


## Comida do dia por nação, do bloco LEVE do status (`p_need`/`p_consumed` por
## cidade — a MESMA métrica do sweep/portão do M2).
func _nation_food_day(status: Dictionary) -> Dictionary:
	var light: Dictionary = status.get("light", {})
	var need_by_city: PackedFloat64Array = light.get("p_need", PackedFloat64Array())
	var consumed_by_city: PackedFloat64Array = light.get(
		"p_consumed", PackedFloat64Array())
	var cities: Array = status.get("cities", [])
	if cities.is_empty() or need_by_city.size() < cities.size() \
			or consumed_by_city.size() < cities.size():
		return {}
	var need: Dictionary = {}
	var consumed: Dictionary = {}
	for slot in cities.size():
		var tag := String((cities[slot] as Dictionary).get("nation", ""))
		if tag == "":
			continue
		need[tag] = float(need.get(tag, 0.0)) + float(need_by_city[slot])
		consumed[tag] = float(consumed.get(tag, 0.0)) \
			+ float(consumed_by_city[slot])
	var out: Dictionary = {}
	for tag in need:
		var total := float(need[tag])
		out[tag] = float(consumed[tag]) / total if total > 0.0 else 1.0
	return out


func _json(path: String) -> Variant:
	var f := FileAccess.open(path, FileAccess.READ)
	if f == null:
		_failures.append("não abriu " + path)
		return null
	var data: Variant = JSON.parse_string(f.get_as_text())
	f.close()
	return data


## Gini de uma distribuição por quintil: mesmo estimador da B6 no núcleo
## (diferenças pareadas / 2·n·total), em GDScript para a leitura do save.
func _quintile_gini(values: Array) -> float:
	var total := 0.0
	for value: Variant in values:
		total += float(value)
	if total <= 0.0:
		return 0.0
	var pairwise := 0.0
	for a in values.size():
		for b in values.size():
			pairwise += absf(float(values[a]) - float(values[b]))
	return pairwise / (2.0 * float(values.size()) * total)


func _init() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--days="):
			_days = maxi(1, int(arg.split("=", true, 1)[1]))
		elif arg.begins_with("--control="):
			_control = arg.split("=", true, 1)[1] != "0"
		elif arg.begins_with("--out="):
			_out = arg.split("=", true, 1)[1]
		elif arg.begins_with("--profile="):
			_profile = arg.split("=", true, 1)[1] != "0"
	if _out == "":
		_out = OUT_PATH
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
	# B6: o MESMO patrimônio que o autoload do jogo semeia depois do `set_urban`.
	if not _control:
		var seeded := LandPropertyScript.apply(sim)
		if seeded <= 0:
			_failures.append("LandProperty.apply não semeou nenhuma região")
	sim.set_ai_enabled(true)
	if _profile:
		sim.set_profiling(true)

	var money_start := float(sim.get_status().get("money_total", 0.0))
	var violations: Array = []
	var violation_count := 0
	var stalled_days := 0
	var day_before := int(sim.get_status().get("day", 0))
	var wall_start := Time.get_ticks_msec()
	var food_history: Array = []
	for day in _days:
		# `tick_day()` devolve o status do dia (com o bloco leve por cidade, de
		# onde sai a comida por nação). `get_day_summary()` NÃO traz `light`.
		var day_status: Dictionary = sim.tick_day()
		for violation: Variant in day_status.get("invariant_violations", []):
			violation_count += 1
			if violations.size() < 10:
				violations.append("iteração %d: %s" % [day + 1, String(violation)])
		var day_now := int(day_status.get("day", 0))
		if day_now <= day_before:
			stalled_days += 1
		day_before = day_now
		var day_food := _nation_food_day(day_status)
		if not day_food.is_empty():
			food_history.append(day_food)
			if food_history.size() > FOOD_WINDOW:
				food_history.pop_front()
	var wall_s := float(Time.get_ticks_msec() - wall_start) / 1000.0

	var status: Dictionary = sim.get_status()
	var save: Dictionary = sim.to_dict()
	var money_end := float(status.get("money_total", 0.0))

	# --- Portão de comida (média de 30 d por nação, consumido/necessidade). ---
	var food_avg := {}
	for day_row: Dictionary in food_history:
		for tag in day_row:
			if not food_avg.has(tag):
				food_avg[tag] = {"total": 0.0, "days": 0}
			food_avg[tag]["total"] = float(food_avg[tag]["total"]) \
				+ float(day_row[tag])
			food_avg[tag]["days"] = int(food_avg[tag]["days"]) + 1
	var food_values: Array = []
	for tag in food_avg:
		var row: Dictionary = food_avg[tag]
		var measured := int(row["days"])
		if measured <= 0:
			continue
		food_values.append({"tag": String(tag),
			"food": float(row["total"]) / float(measured)})
	food_values.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return float(a["food"]) < float(b["food"]))
	var food_below: Array = []
	var sum_food := 0.0
	for row: Dictionary in food_values:
		sum_food += float(row["food"])
		if float(row["food"]) < FOOD_GATE:
			food_below.append({"tag": String(row["tag"]),
				"food": float(row["food"])})
	var food_median := 0.0
	if food_values.size() > 0:
		var middle := food_values.size() / 2
		if food_values.size() % 2 == 1:
			food_median = float(food_values[middle]["food"])
		else:
			food_median = (float(food_values[middle - 1]["food"])
				+ float(food_values[middle]["food"])) * 0.5

	# --- Patrimônio da região (save + status por cidade). -------------------
	var regions_ready := 0
	var land_ha := 0.0
	var land_value := 0.0
	var home_value := 0.0
	var ring_value := 0.0
	var ring_population := 0.0
	var region_population := 0.0
	var ring_rent_month := 0.0
	var tax_potential := 0.0
	var ring_in_total := 0.0
	var wealth_total := 0.0
	var wealth_world := [0.0, 0.0, 0.0, 0.0, 0.0]
	var land_observed := 0
	for c: Dictionary in save.get("cities", []):
		var region: Dictionary = c.get("region", {})
		region_population += float(region.get("region_pop", 0.0))
		var property: Dictionary = region.get("property", {})
		if property.is_empty() or not bool(property.get("ready", false)):
			continue
		regions_ready += 1
		if bool(property.get("land_gini_observed", false)):
			land_observed += 1
		land_ha += float(property.get("land_ha", 0.0))
		land_value += float(property.get("land_ha", 0.0)) \
			* float(property.get("land_value_per_ha", 0.0))
		var row_home: Array = property.get("home_value", [])
		for value: Variant in row_home:
			home_value += float(value)
		var row_ring: Array = property.get("ring_value", [])
		for value: Variant in row_ring:
			ring_value += float(value)
		var row_wealth: Array = property.get("wealth", [])
		for s in row_wealth.size():
			wealth_total += float(row_wealth[s])
			if s < 5:
				wealth_world[s] += float(row_wealth[s])
		ring_population += float(property.get("ring_population", 0.0))
		ring_rent_month += float(property.get("ring_rent_paid_month", 0.0))
		tax_potential += float(property.get("tax_potential", 0.0))
		ring_in_total += float(property.get("migration_ring_in_total", 0.0))
		# Riqueza urbana por quintil entra no Gini de riqueza do mundo.
		var strata: Array = c.get("pop", {}).get("strata", {}).get("wealth", [])
		for s in strata.size():
			if s < 5:
				wealth_world[s] += float(strata[s])

	# --- Gini de riqueza por nação (top 5 / bottom 5). ----------------------
	var nations: Array = status.get("nations", [])
	var rows: Array = []
	for entry: Dictionary in nations:
		if not entry.has("wealth_gini"):
			continue
		rows.append({
			"nation": String(entry.get("tag", "")),
			"wealth_gini": float(entry.get("wealth_gini", 0.0)),
			"population": float(entry.get("population", 0.0)),
		})
	rows.sort_custom(func(a, b): return float(a["wealth_gini"]) > float(b["wealth_gini"]))
	var top10: Array = rows.slice(0, mini(10, rows.size()))
	var bottom10: Array = rows.slice(maxi(0, rows.size() - 10), rows.size())
	var gini_world := _quintile_gini(wealth_world)
	var profile_us: Dictionary = sim.get_profile() if _profile else {}
	var profile_total_us := 0.0
	if _profile:
		for key: Variant in profile_us:
			profile_total_us += float(profile_us[key])

	var out := {
		"days": _days,
		"control_without_b6": _control,
		"final_day": int(status.get("day", 0)),
		"stalled_days": stalled_days,
		"invariant_violations": violation_count,
		"invariant_violation_samples": violations,
		"bankruptcies": int(status.get("bankruptcies", -1)),
		"money_start": money_start,
		"money_end": money_end,
		"money_drift": money_end - money_start,
		"wall_s": wall_s,
		"food": {
			"gate": FOOD_GATE,
			"window_days": FOOD_WINDOW,
			"nations": food_values.size(),
			"below_count": food_below.size(),
			"below": food_below,
			"min": float(food_values[0]["food"]) if food_values.size() > 0 else 0.0,
			"min_nation": String(food_values[0]["tag"]) if food_values.size() > 0 else "",
			"median": food_median,
			"mean": sum_food / float(food_values.size()) if food_values.size() > 0 else 0.0,
		},
		"property": {
			"regions_ready": regions_ready,
			"regions_land_gini_observed": land_observed,
			"land_ha": land_ha,
			"land_value": land_value,
			"home_value": home_value,
			"ring_value": ring_value,
			"total_value": land_value + home_value + ring_value,
			"interior_wealth_total": wealth_total,
			"ring_population": ring_population,
			"ring_share_of_interior": ring_population / region_population
				if region_population > 0.0 else 0.0,
			"ring_in_total": ring_in_total,
			"ring_rent_paid_month": ring_rent_month,
			"tax_potential_month": tax_potential,
		},
		"wealth_gini_world": gini_world,
		"wealth_by_quintile_world": wealth_world,
		"wealth_gini_top10": top10,
		"wealth_gini_bottom10": bottom10,
	}
	if _profile:
		out["profile_us_total"] = profile_total_us
		out["profile_us_per_day"] = profile_total_us / float(_days)
		out["profile_us"] = profile_us
	var file := FileAccess.open(_out, FileAccess.WRITE)
	if file == null:
		_failures.append("não abriu para escrita: " + _out)
	else:
		file.store_string(JSON.stringify(out, "  "))
		file.close()
	var property: Dictionary = out["property"]
	print("[b6-365] dia_final=%d travados=%d violações=%d falências=%d drift=%.6f wall=%.1fs"
		% [out["final_day"], stalled_days, violation_count, out["bankruptcies"],
		out["money_drift"], wall_s])
	print("[b6-365] propriedade: regiões=%d terra=%.0f ha valor_terra=%.0f valor_total=%.0f anel_pop=%.0f (%.3f%%) aluguel_anel_mes=%.1f gini_riqueza=%.4f"
		% [regions_ready, land_ha, land_value, property["total_value"],
		ring_population, 100.0 * float(property["ring_share_of_interior"]),
		ring_rent_month, gini_world])
	var food: Dictionary = out["food"]
	print("[b6-365] portão de comida: %d/%d abaixo de %.3f (pior %s %.4f, mediana %.4f)"
		% [int(food["below_count"]), int(food["nations"]), FOOD_GATE,
		String(food["min_nation"]), float(food["min"]), float(food["median"])])
	if _profile:
		var share := 0.0
		if profile_total_us > 0.0:
			share = 100.0 * float(profile_us.get("property", 0.0)) / profile_total_us
		print("[b6-365] perfil: property=%.1f µs/dia de %.1f µs/dia (%.2f%% do tick)"
			% [float(profile_us.get("property", 0.0)) / float(_days),
			profile_total_us / float(_days), share])
	if not top10.is_empty() and not bottom10.is_empty():
		var top: Dictionary = top10[0]
		var bottom: Dictionary = bottom10[bottom10.size() - 1]
		print("[b6-365] Gini de riqueza: top=%s(%.4f) bottom=%s(%.4f); escrito %s"
			% [String(top["nation"]), float(top["wealth_gini"]),
			String(bottom["nation"]), float(bottom["wealth_gini"]), _out])
	for failure: String in _failures:
		print("[b6-365] FALHA: " + failure)
	for sample: String in violations:
		print("[b6-365] violação: " + sample)
	quit(0 if _failures.is_empty() and violation_count == 0 else 1)
