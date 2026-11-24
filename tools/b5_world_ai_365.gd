extends SceneTree

## B5 (D-089) — mundo real com IA por N dias e INFRAESTRUTURA INICIAL.
##
## Mede o portão de comida (média dos últimos 30 dias por nação), falências,
## drift de dinheiro, obras de infraestrutura concluídas pela IA e concessões,
## em três modos:
##   --infra=real            níveis iniciais do dado real (World Bank) — o jogo;
##   --infra=0               todas as regiões em 0 (comportamento da B5/D-089
##                           antes de ligar o dado);
##   --access-penalty=0      (com --infra=real) isola o frete de acesso: a
##                           penalidade máxima de estrada 0 vira 0.
##
## O caminho de montagem é o do autoload (`Economy._ready`): mesmos arquivos,
## `RegionData.attach`, `apply_initial_infra` DEPOIS do mundo, geografia, IA.
##
## Uso:
##   & $godot --headless --path . --script res://tools/b5_world_ai_365.gd \
##     -- --infra=real --out=docs/economy/perf/b5_infra_real_365.json
##
## `--out` aceita caminho relativo (vira `res://`). Sem ele, grava o histórico
## da B5 em `docs/economy/perf/b5_world_ai_365.json`.

const GeographyServiceScript := preload("res://core/economy/geography_service.gd")
const RegionDataScript := preload("res://core/economy/region_data.gd")
const GroupNamerScript := preload("res://core/economy/group_namer.gd")

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
## B7: informalidade e saúde por nação — o jogo carrega os dois; sem eles o A/B
## não mede o mundo de hoje.
const INFORMALITY_PATH := "res://data/definitions/economy/informality.json"
const HEALTH_ACCESS_PATH := "res://data/definitions/population/health_access.json"
const URBAN_LAND_PATH := "res://data/definitions/economy/urban_land.json"
const HOUSING_CITY_PATH := "res://data/definitions/economy/housing_city.json"
const HOUSING_PATH := "res://data/definitions/economy/housing.json"
const MUNICIPAL_FINANCE_PATH := "res://data/definitions/economy/municipal_finance.json"

const OUT_PATH := "res://docs/economy/perf/b5_world_ai_365.json"
## Portão de comida do M2 (`docs/economy/M2_marco.md`): nação com a média de
## 30 dias abaixo de 0,975 está fora do portão.
const FOOD_GATE := 0.975
const FOOD_WINDOW := 30

var _days := 365
var _out := OUT_PATH
var _infra := "real"
var _penalty := -1.0
var _tag := ""


func _json(path: String) -> Variant:
	var f := FileAccess.open(path, FileAccess.READ)
	if f == null:
		push_error("[b5-365] nao abriu " + path)
		return null
	var data: Variant = JSON.parse_string(f.get_as_text())
	f.close()
	return data


func _init() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--days="):
			_days = maxi(1, int(arg.split("=", true, 1)[1]))
		elif arg.begins_with("--out="):
			_out = arg.split("=", true, 1)[1]
			# Caminho absoluto do SO (ex.: G:/opengs_tmp/...) passa direto; so o
			# relativo vira res://, como antes.
			if not _out.begins_with("res://") and not _out.begins_with("user://") \
					and not _out.is_absolute_path():
				_out = "res://" + _out
		elif arg.begins_with("--infra="):
			_infra = arg.split("=", true, 1)[1]
		elif arg.begins_with("--access-penalty="):
			_penalty = float(arg.split("=", true, 1)[1])
		elif arg.begins_with("--tag="):
			_tag = arg.split("=", true, 1)[1]
	if _tag == "":
		_tag = "%s_pen%.2f" % [_infra, _penalty]
	_run()


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


## Soma dos níveis de infra (e regiões com infra) do save.
func _infra_totals(save: Dictionary) -> Dictionary:
	var road := 0
	var power := 0
	var water := 0
	var regions := 0
	for city: Dictionary in save.get("cities", []):
		var infra: Dictionary = city.get("region", {}).get("infra", {})
		var r := int(infra.get("road", 0))
		var p := int(infra.get("power", 0))
		var w := int(infra.get("water", 0))
		road += r
		power += p
		water += w
		if r + p + w > 0:
			regions += 1
	return {"road": road, "power": power, "water": water,
		"total": road + power + water, "regions": regions}


func _run() -> void:
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
	# B5: os níveis iniciais dependem do modo do A/B. `=0` reproduz a B5/D-089
	# anterior (todas as regiões em 0); `real` é o jogo depois do dado.
	if _infra == "real":
		GeographyServiceScript.apply_initial_infra(sim)
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
	GroupNamerScript.name_groups(sim)
	if _penalty >= 0.0 and sim.has_method("set_balance"):
		# Gancho de laboratório da B5: penalidade máxima de acesso (estrada 0).
		sim.set_balance({"infra_access_penalty": _penalty})
	sim.set_ai_enabled(true)

	var money_start := float(sim.get_status().get("money_total", 0.0))
	var initial_infra := _infra_totals(sim.to_dict())

	# Violação de invariante faz o núcleo REVERTER o tick: registra para não
	# esconder congelamento atrás das N iterações.
	var violations: Array = []
	var violation_count := 0
	var stalled_days := 0
	var day_before := int(sim.get_status().get("day", 0))
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

	var status: Dictionary = sim.get_status()
	var save: Dictionary = sim.to_dict()
	var money_end := float(status.get("money_total", 0.0))
	var final_infra := _infra_totals(save)

	# --- Portão de comida (média de 30 d por nação, consumido/necessidade). ---
	var food_avg := {}
	for day_row: Dictionary in food_history:
		for tag in day_row:
			if not food_avg.has(tag):
				food_avg[tag] = {"total": 0.0, "days": 0}
			food_avg[tag]["total"] = float(food_avg[tag]["total"]) \
				+ float(day_row[tag])
			food_avg[tag]["days"] = int(food_avg[tag]["days"]) + 1
	var values: Array = []
	for tag in food_avg:
		var row: Dictionary = food_avg[tag]
		var measured := int(row["days"])
		if measured <= 0:
			continue
		values.append({"tag": String(tag), "food": float(row["total"]) / float(measured)})
	values.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return float(a["food"]) < float(b["food"]))
	var below: Array = []
	var sum_food := 0.0
	for row: Dictionary in values:
		sum_food += float(row["food"])
		if float(row["food"]) < FOOD_GATE:
			below.append({"tag": String(row["tag"]), "food": float(row["food"])})
	var median := 0.0
	if values.size() > 0:
		var middle := values.size() / 2
		if values.size() % 2 == 1:
			median = float(values[middle]["food"])
		else:
			median = (float(values[middle - 1]["food"])
				+ float(values[middle]["food"])) * 0.5

	var concessions := 0
	for b: Dictionary in save.get("buildings", []):
		if float(b.get("royalty", 0.0)) > 0.0:
			concessions += 1

	var works := {
		"road": int(final_infra["road"]) - int(initial_infra["road"]),
		"power": int(final_infra["power"]) - int(initial_infra["power"]),
		"water": int(final_infra["water"]) - int(initial_infra["water"]),
		"total": int(final_infra["total"]) - int(initial_infra["total"]),
	}
	var out := {
		"tag": _tag,
		"mode": "infra_real" if _infra == "real" else "infra_zero",
		"access_penalty": _penalty if _penalty >= 0.0 else 0.30,
		"days": _days,
		"final_day": int(status.get("day", 0)),
		"stalled_days": stalled_days,
		"invariant_violations": violation_count,
		"invariant_violation_samples": violations,
		"bankruptcies": int(status.get("bankruptcies", -1)),
		"money_start": money_start,
		"money_end": money_end,
		"money_drift": money_end - money_start,
		"food": {
			"gate": FOOD_GATE,
			"window_days": FOOD_WINDOW,
			"nations": values.size(),
			"below_count": below.size(),
			"below": below,
			"min": float(values[0]["food"]) if values.size() > 0 else 0.0,
			"min_nation": String(values[0]["tag"]) if values.size() > 0 else "",
			"median": median,
			"mean": sum_food / float(values.size()) if values.size() > 0 else 0.0,
		},
		"infra": {
			"initial": initial_infra,
			"final": final_infra,
			"works": works,
		},
		"concessions": concessions,
	}
	var dir := _out.get_base_dir()
	if dir != "" and not DirAccess.dir_exists_absolute(dir):
		DirAccess.make_dir_recursive_absolute(dir)
	var f := FileAccess.open(_out, FileAccess.WRITE)
	if f == null:
		push_error("[b5-365] nao abriu para escrita " + _out)
		quit(1)
		return
	f.store_string(JSON.stringify(out, "  ") + "\n")
	f.close()
	print("[b5-365] %s dia_final=%d travados=%d violações=%d falências=%d drift=%.6f portão=%d/%d (pior %s %.3f) infra_inicial=%d obras=%d (%d estrada/%d luz/%d água) concessões=%d"
		% [out["tag"], out["final_day"], stalled_days, violation_count,
		out["bankruptcies"], out["money_drift"], below.size(), values.size(),
		out["food"]["min_nation"], out["food"]["min"],
		int(initial_infra["total"]), works["total"], works["road"],
		works["power"], works["water"], concessions])
	print("[b5-365] evidência em " + _out)
	quit(0 if violation_count == 0 else 1)
