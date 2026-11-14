extends SceneTree

## Medição de rebalanceio após a subdivisão de províncias (Fase B).
##
## Roda o MESMO pipeline do jogo ao vivo (initialize_demo_world + goods +
## GeographyService + EconomicAI) por N dias e grava, num JSON:
##   - fome por nação (média e pior cidade) no fim;
##   - geografia por cidade (províncias da bacia, área cultivável, aptidão do
##     trigo, floresta, jazidas: quantidade, tipos e reserva);
##   - agregados por nação (terra cultivável, reserva por recurso) e Gini entre
##     as cidades da mesma nação (terra e reserva total);
##   - reservas globais por recurso e cidades sem jazida.
##
## Só mede; não altera dados nem código. O JSON de saída não é versionado.
##
##   godot --headless --path . --script res://tools/economy_rebalance_scan.gd [--days=60] [--out=path] [--tag=after]

const GeographyServiceScript := preload("res://core/economy/geography_service.gd")
const EconomicAIScript := preload("res://core/economy/ai/economic_ai.gd")

const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const PORTS_PATH := "res://data/definitions/scenario/ports.json"
const BUILDINGS_PATH := "res://data/definitions/scenario/buildings.json"
const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"

var _days := 60
var _out := "res://docs/economy/economy_rebalance_scan.json"
var _tag := "scan"


func _init() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--days="):
			_days = int(arg.split("=")[1])
		elif arg.begins_with("--out="):
			_out = arg.split("=")[1]
		elif arg.begins_with("--tag="):
			_tag = arg.split("=")[1]
	_run.call_deferred()


func _load_json(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return null
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data


func _load_array(path: String) -> Array:
	var data: Variant = _load_json(path)
	return data if typeof(data) == TYPE_ARRAY else []


func _load_dict(path: String) -> Dictionary:
	var data: Variant = _load_json(path)
	return data if typeof(data) == TYPE_DICTIONARY else {}


func _cities_with_center(cities: Array) -> Array:
	var result: Array = []
	for city in cities:
		result.append({
			"id": String(city.get("id", "")),
			"nation": String(city.get("nation", "")),
			"center": Vector2(float(city.get("x", 0.0)), float(city.get("y", 0.0))),
		})
	return result


func _gini(values: Array) -> float:
	var positive: Array = []
	for value in values:
		if float(value) > 0.0:
			positive.append(float(value))
	if positive.size() < 2:
		return 0.0
	positive.sort()
	var n := positive.size()
	var total := 0.0
	for value in positive:
		total += value
	if total <= 0.0:
		return 0.0
	var weighted := 0.0
	for index in n:
		weighted += float(index + 1) * float(positive[index])
	return (2.0 * weighted) / (float(n) * total) - (float(n) + 1.0) / float(n)


func _run() -> void:
	var cities := _load_array(CITIES_PATH)
	var ports := _load_array(PORTS_PATH)
	var buildings := _load_array(BUILDINGS_PATH)
	var goods := _load_array(GOODS_PATH)
	var geo = GeographyServiceScript.new()
	geo.load_data()
	var sim = EconomySim.new()
	# F2/F3: bens, receitas e necessidades ANTES do mundo — os ids de insumo e
	# as categorias das POPs são resolvidos na carga.
	sim.demo_set_goods(goods)
	sim.demo_set_recipes(_load_array(RECIPES_PATH))
	sim.demo_set_needs(_load_dict(NEEDS_PATH))
	sim.initialize_demo_world(cities, ports, geo.resolve_initial_farms(buildings, cities))
	geo.apply_to_sim(sim, _cities_with_center(cities))
	var ai = EconomicAIScript.new()
	ai.setup(sim, geo)

	for day in _days:
		var status: Dictionary = sim.tick_demo()
		ai.run(int(status.get("day", 0)), status)

	var status: Dictionary = sim.get_demo_status()
	var nation_cities := {}
	var city_rows := {}
	var nation_rows := {}
	var global_reserve := {}
	var cities_zero_deposit := 0
	for city in status.get("cities", []):
		var cid := String(city.get("id", ""))
		var tag := String(city.get("nation", ""))
		if not nation_cities.has(tag):
			nation_cities[tag] = []
		nation_cities[tag].append(cid)
		var pop: Dictionary = city.get("pop", {})
		var deposit_count := 0
		var deposit_types := {}
		var reserve_total := 0.0
		for resource in city.get("deposits", {}):
			var entry: Dictionary = city.get("deposits", {})[resource]
			deposit_count += 1
			deposit_types[String(resource)] = int(deposit_types.get(String(resource), 0)) + 1
			var remaining := float(entry.get("remaining_kt", 0.0))
			reserve_total += remaining
			global_reserve[String(resource)] = float(global_reserve.get(String(resource), 0.0)) + remaining
		if deposit_count == 0:
			cities_zero_deposit += 1
		var cultivable := float(geo.city_cultivable_area_km2(cid))
		city_rows[cid] = {
			"nation": tag,
			"provinces": (geo._catchment.get(cid, []) as Array).size(),
			"cultivable_km2": cultivable,
			"wheat_suitability": float(geo.city_crop_suitability(cid, "wheat")),
			"forest_cover": float(geo.city_forest_cover(cid)),
			"deposit_count": deposit_count,
			"deposit_types": deposit_types,
			"reserve_kt": reserve_total,
			"need": float(pop.get("need", 0.0)),
			"consumed": float(pop.get("consumed", 0.0)),
			"satisfaction": float(pop.get("satisfaction", 0.0)),
			"population": float(city.get("population", 0.0)),
		}
	for tag in nation_cities:
		var ids: Array = nation_cities[tag]
		var need := 0.0
		var consumed := 0.0
		var cultivable := 0.0
		var reserve := 0.0
		var cultivable_values: Array = []
		var reserve_values: Array = []
		var worst := 1.0
		var worst_city := ""
		var zero := 0
		for cid in ids:
			var row: Dictionary = city_rows[cid]
			need += float(row["need"])
			consumed += float(row["consumed"])
			cultivable += float(row["cultivable_km2"])
			reserve += float(row["reserve_kt"])
			cultivable_values.append(float(row["cultivable_km2"]))
			reserve_values.append(float(row["reserve_kt"]))
			var sat := float(row["satisfaction"])
			if sat < worst:
				worst = sat
				worst_city = String(cid)
			if int(row["deposit_count"]) == 0:
				zero += 1
		nation_rows[tag] = {
			"cities": ids.size(),
			"need": need,
			"consumed": consumed,
			"satisfaction": consumed / need if need > 0.0 else 1.0,
			"worst_city": worst_city,
			"worst_satisfaction": worst,
			"cultivable_km2": cultivable,
			"reserve_kt": reserve,
			"gini_cultivable": _gini(cultivable_values),
			"gini_reserve": _gini(reserve_values),
			"cities_without_deposit": zero,
		}

	var hungry: Array = []
	var all_ok := 0
	var all_cities_ok := 0
	for tag in nation_rows:
		var row: Dictionary = nation_rows[tag]
		if float(row["satisfaction"]) < 0.70:
			hungry.append([tag, float(row["satisfaction"])])
		else:
			all_ok += 1
		if float(row["worst_satisfaction"]) >= 0.70:
			all_cities_ok += 1
	hungry.sort_custom(func(a, b): return a[1] < b[1])
	var gini_cultivable_values: Array = []
	var gini_reserve_values: Array = []
	for tag in nation_rows:
		gini_cultivable_values.append(float(nation_rows[tag]["gini_cultivable"]))
		gini_reserve_values.append(float(nation_rows[tag]["gini_reserve"]))

	var report := {
		"meta": {
			"tag": _tag,
			"days": _days,
			"cities": city_rows.size(),
			"nations": nation_rows.size(),
			"nations_over_70": all_ok,
			"nations_all_cities_over_70": all_cities_ok,
			"hungry": hungry,
			"cities_without_deposit": cities_zero_deposit,
			"money_total": float(status.get("money_total", 0.0)),
			"bankruptcies": int(status.get("bankruptcies", 0)),
			"invariant_violations": status.get("invariant_violations", []),
			"mean_gini_cultivable": _mean(gini_cultivable_values),
			"mean_gini_reserve": _mean(gini_reserve_values),
		},
		"global_reserve_kt": global_reserve,
		"nations": nation_rows,
		"cities": city_rows,
	}
	_write(report)
	print("[scan:%s] %d dias | %d cidades | fome>=70%%: %d/%d nações (todas as cidades: %d)" % [
		_tag, _days, city_rows.size(), all_ok, nation_rows.size(), all_cities_ok])
	print("[scan:%s] fome: %s" % [_tag, str(hungry)])
	print("[scan:%s] cidades sem jazida: %d | Gini terra média %.3f | Gini reserva média %.3f" % [
		_tag, cities_zero_deposit, _mean(gini_cultivable_values), _mean(gini_reserve_values)])
	print("[scan:%s] escrito %s" % [_tag, _out])
	quit(0)


func _mean(values: Array) -> float:
	if values.is_empty():
		return 0.0
	var total := 0.0
	for value in values:
		total += float(value)
	return total / float(values.size())


func _write(data: Variant) -> void:
	var dir := _out.get_base_dir()
	if not DirAccess.dir_exists_absolute(dir):
		DirAccess.make_dir_recursive_absolute(dir)
	var file := FileAccess.open(_out, FileAccess.WRITE)
	if file == null:
		push_error("não abriu para escrita: " + _out)
		return
	file.store_string(JSON.stringify(data, "  "))
	file.close()
