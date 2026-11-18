extends SceneTree

## Trajetória de preço no mundo real (365 d, IA + urbano) — diagnóstico do
## equilíbrio de preço da D-092.
##
##   godot --headless --path . --script res://tools/price_trajectory_365.gd
##     [--days=365] [--every=30] [--out=user://price_trajectory.csv]
##
## - Monta o mundo como `tools/ore_price_365.gd` (geografia real, camada urbana,
##   IA de grupos ligada, threads = 1) e roda `--days` dias.
## - Amostra a cada `--every` dias a razão preço ÷ base ponderada pela
##   quantidade vendida (`m_price`/`m_sold` do bloco leve), para os bens que a
##   D-091/D-092 precisa vigiar: ferro, carvão, petróleo, gás, cobre, bauxita,
##   trigo, arroz e ouro.
## - Imprime uma linha `[price] dia=N ...` por amostra, a tabela final e grava
##   CSV no `--out`. Exit 0 ok, 1 se um invariante violar.

const GeographyServiceScript := preload("res://core/economy/geography_service.gd")
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
const URBAN_LAND_PATH := "res://data/definitions/economy/urban_land.json"
const HOUSING_CITY_PATH := "res://data/definitions/economy/housing_city.json"
const HOUSING_PATH := "res://data/definitions/economy/housing.json"
const MUNICIPAL_FINANCE_PATH := "res://data/definitions/economy/municipal_finance.json"

const TARGETS := ["iron", "coal", "oil", "gas", "copper", "bauxite",
	"wheat", "rice", "gold"]
## Alimentos da cesta (needs.json): somados em `produced_today`/`consumed_today`.
const FOOD_IDS := ["wheat", "rice", "maize", "potato", "soy", "bread", "meat",
	"canned_food", "bagged_rice"]

var _days := 365
var _every := 30
var _out := "user://price_trajectory.csv"


func _init() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--days="):
			_days = int(arg.split("=", true, 1)[1])
		elif arg.begins_with("--every="):
			_every = maxi(1, int(arg.split("=", true, 1)[1]))
		elif arg.begins_with("--out="):
			_out = arg.split("=", true, 1)[1]
	var goods := _load_array(GOODS_PATH)
	var cities := _load_array(CITIES_PATH)
	var ports := _load_array(PORTS_PATH)
	var buildings := _load_array(BUILDINGS_PATH)
	var sim = EconomySim.new()
	sim.set_sim_threads(1)
	sim.set_goods(goods)
	sim.set_recipes(_load_array(RECIPES_PATH))
	sim.set_needs(_load_dict(NEEDS_PATH))
	sim.set_services(_load_dict(SERVICES_PATH))
	sim.set_currencies(_load_dict(CURRENCIES_PATH))
	sim.set_sector_employment(_load_dict(SECTOR_EMPLOYMENT_PATH))
	sim.set_age_structure(_load_dict(AGE_STRUCTURE_PATH))
	var geo = GeographyServiceScript.new()
	geo.load_data()
	buildings = geo.resolve_initial_farms(buildings, cities)
	buildings = geo.resolve_initial_mines(buildings, cities)
	sim.initialize_world(cities, ports, buildings)
	sim.set_urban(_load_dict(URBAN_LAND_PATH), _load_dict(HOUSING_CITY_PATH),
		_load_dict(HOUSING_PATH), _load_dict(MUNICIPAL_FINANCE_PATH))
	var with_center: Array = []
	for city in cities:
		var entry: Dictionary = city
		with_center.append({
			"id": String(entry.get("id", "")),
			"nation": String(entry.get("nation", "")),
			"center": Vector2(float(entry.get("x", 0.0)), float(entry.get("y", 0.0))),
		})
	geo.apply_to_sim(sim, with_center)
	sim.set_ai_enabled(true)
	GroupNamerScript.name_groups(sim)

	var base := {}
	for good in goods:
		var entry: Dictionary = good
		base[String(entry.get("id", ""))] = float(entry.get("base_price", 0.0))

	var rows: Array[Dictionary] = []
	var status: Dictionary = sim.get_status()
	for day in range(1, _days + 1):
		status = sim.tick_day()
		var violations: Array = status.get("invariant_violations", [])
		if violations.size() > 0:
			print("[price] FALHA dia %d: %s" % [day, String(violations[0])])
			quit(1)
			return
		if day % _every == 0 or day == _days:
			var row := {"day": day,
				"bankruptcies": int(status.get("bankruptcies", 0)),
				"money": float(status.get("money_total", 0.0))}
			for target in TARGETS:
				row[target] = _weighted_ratio(status, base, target)
			row["food"] = _food_ratio(status)
			row["food_prod"] = _food_today(status, "produced_today")
			row["food_cons"] = _food_today(status, "consumed_today")
			rows.append(row)
			var parts: Array[String] = []
			for target in TARGETS:
				parts.append("%s=%.2fx" % [target, float(row[target])])
			print("[price] dia=%d falencias=%d comida=%.3f prod=%.0f cons=%.0f %s" % [
				day, int(row["bankruptcies"]), float(row["food"]),
				float(row["food_prod"]), float(row["food_cons"]),
				" ".join(parts)])

	print("[price] --- tabela final (razão ao base, ponderada por venda) ---")
	print("[price] dia " + " ".join(TARGETS))
	for row in rows:
		var cells: Array[String] = []
		for target in TARGETS:
			cells.append("%.2f" % float(row[target]))
		print("[price] %3d %s" % [int(row["day"]), " ".join(cells)])
	_write_csv(rows)
	quit(0)


func _weighted_ratio(status: Dictionary, base: Dictionary, target: String) -> float:
	var light: Dictionary = status.get("light", {})
	var prices: PackedFloat64Array = light.get("m_price", PackedFloat64Array())
	var sold: PackedFloat64Array = light.get("m_sold", PackedFloat64Array())
	var per_city := int(light.get("product_count", 0))
	var products: Array = status.get("products", [])
	var index := products.find(target)
	if index < 0 or per_city <= 0:
		return 0.0
	var city_count: int = status["cities"].size()
	var weighted := 0.0
	var weight := 0.0
	var plain := 0.0
	var priced := 0
	for slot in city_count:
		var at: int = slot * per_city + index
		if at >= prices.size():
			break
		var price := float(prices[at])
		if price <= 0.0:
			continue
		priced += 1
		plain += price
		var quantity := float(sold[at]) if at < sold.size() else 0.0
		weighted += price * quantity
		weight += quantity
	var mean := 0.0
	if weight > 0.0:
		mean = weighted / weight
	elif priced > 0:
		mean = plain / float(priced)
	var base_price := float(base.get(target, 0.0))
	return mean / base_price if base_price > 0.0 else 0.0


func _food_ratio(status: Dictionary) -> float:
	var light: Dictionary = status.get("light", {})
	var need: PackedFloat64Array = light.get("p_need", PackedFloat64Array())
	var consumed: PackedFloat64Array = light.get("p_consumed", PackedFloat64Array())
	var need_total := 0.0
	var consumed_total := 0.0
	for index in mini(need.size(), consumed.size()):
		need_total += float(need[index])
		consumed_total += float(consumed[index])
	return consumed_total / need_total if need_total > 0.0 else 1.0


func _food_today(status: Dictionary, key: String) -> float:
	var products: Array = status.get("products", [])
	var values: Array = status.get(key, [])
	var total := 0.0
	for id in FOOD_IDS:
		var index := products.find(id)
		if index >= 0 and index < values.size():
			total += float(values[index])
	return total


func _write_csv(rows: Array[Dictionary]) -> void:
	var file := FileAccess.open(_out, FileAccess.WRITE)
	if file == null:
		push_error("price_trajectory_365: não abriu " + _out)
		return
	var header: Array[String] = ["day", "bankruptcies", "money", "food",
		"food_prod", "food_cons"]
	header.append_array(TARGETS)
	file.store_line(",".join(header))
	for row in rows:
		var cells: Array[String] = [
			str(int(row["day"])), str(int(row["bankruptcies"])),
			"%.4f" % float(row["money"]), "%.6f" % float(row["food"]),
			"%.4f" % float(row["food_prod"]), "%.4f" % float(row["food_cons"])]
		for target in TARGETS:
			cells.append("%.6f" % float(row[target]))
		file.store_line(",".join(cells))
	file.close()
	print("[price] escrito " + _out)


func _load_array(path: String) -> Array:
	var parsed: Variant = _load(path)
	return parsed if typeof(parsed) == TYPE_ARRAY else []


func _load_dict(path: String) -> Dictionary:
	var parsed: Variant = _load(path)
	return parsed if typeof(parsed) == TYPE_DICTIONARY else {}


func _load(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		push_error("price_trajectory_365: não abriu " + path)
		return null
	var text := file.get_as_text()
	file.close()
	return JSON.parse_string(text)
