extends SceneTree

## Diagnóstico (só leitura): para cada cidade, compara a necessidade alimentar
## (pop × 0,0003 t/dia) com o que a bacia cultivável consegue entregar na cultura
## escolhida. Não altera dados nem o sim.
##
##   godot --headless --path . --script res://tools/economy_farm_capacity_dump.gd

const GeographyServiceScript := preload("res://core/economy/geography_service.gd")

const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const BUILDINGS_PATH := "res://data/definitions/scenario/buildings.json"


func _init() -> void:
	_run.call_deferred()


func _load(path: String) -> Array:
	var data: Variant = JSON.parse_string(FileAccess.get_file_as_string(path))
	return data if typeof(data) == TYPE_ARRAY else []


func _run() -> void:
	var cities := _load(CITIES_PATH)
	var buildings := _load(BUILDINGS_PATH)
	var geo = GeographyServiceScript.new()
	geo.load_data()
	var farms := geo.resolve_initial_farms(buildings, cities)
	var by_city := {}
	for farm in farms:
		by_city[String(farm.get("city_id", ""))] = farm
	var nation: Dictionary = {}
	var rows: Array = []
	for city in cities:
		var cid := String(city.get("id", ""))
		var tag := String(city.get("nation", ""))
		var pop := float(city.get("population", 0.0))
		var need := pop * 0.0003
		var farm: Dictionary = by_city.get(cid, {})
		var crop := String(farm.get("resource", "wheat"))
		var nominal := float(farm.get("output_tons_per_day", 0.0))
		var suit := geo.city_crop_suitability(cid, crop)
		var land := geo.city_land_capacity_t_day(cid, crop)
		var effective := nominal * suit
		var max_effective := land * suit
		var needed_nominal := need / suit if suit > 0.0 else INF
		if not nation.has(tag):
			nation[tag] = {"need": 0.0, "eff": 0.0, "max": 0.0, "pop": 0.0,
				"nominal": 0.0, "cities": 0, "land_limited": 0}
		var n: Dictionary = nation[tag]
		n["need"] += need
		n["eff"] += effective
		n["max"] += max_effective
		n["nominal"] += nominal
		n["pop"] += pop
		n["cities"] += 1
		if needed_nominal > land:
			n["land_limited"] += 1
		rows.append([cid, tag, crop, pop, need, suit, nominal, effective, land, max_effective,
			needed_nominal])
	for row in rows:
		print("%-9s %-4s %-7s pop=%-9.0f need=%-7.2f suit=%.3f nominal=%-8.1f eff=%-7.2f " \
			% [row[0], row[1], row[2], row[3], row[4], row[5], row[6], row[7]] \
			+ "land=%-8.1f maxeff=%-7.2f need_nominal=%-8.1f" % [row[8], row[9], row[10]])
	print("")
	print("--- por nação: necessidade × produção efetiva × teto de terra ---")
	var tags: Array = nation.keys()
	tags.sort()
	var world_need := 0.0
	var world_eff := 0.0
	var world_max := 0.0
	for tag in tags:
		var n: Dictionary = nation[tag]
		world_need += n["need"]
		world_eff += n["eff"]
		world_max += n["max"]
		print("%-4s cid=%-2d pop=%-10.0f need=%-9.1f eff=%-9.1f (%.0f%%) max_teto=%-9.1f (%.0f%%) limitadas=%d"
			% [tag, n["cities"], n["pop"], n["need"], n["eff"],
				100.0 * n["eff"] / n["need"] if n["need"] > 0.0 else 0.0,
				n["max"], 100.0 * n["max"] / n["need"] if n["need"] > 0.0 else 0.0,
				n["land_limited"]])
	print("")
	print("MUNDO need=%.1f eff=%.1f (%.1f%%) max_teto=%.1f (%.1f%%)"
		% [world_need, world_eff, 100.0 * world_eff / world_need,
			world_max, 100.0 * world_max / world_need])
	quit(0)
