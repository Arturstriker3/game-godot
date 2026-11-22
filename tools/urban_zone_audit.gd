extends SceneTree

## Q-015 / frente C — auditoria de CAPACIDADE DAS ZONAS URBANAS no dia 0.
##
## Carrega o mundo real (834 cidades) com a camada urbana §19 exatamente como o
## autoload do jogo faz (`set_urban`) e imprime, para todas as cidades:
##   ocupação RES/COM/IND (zone_occupancy), uso e capacidade de cada zona,
##   a distribuição p10/p50/p90 por faixa de população urbana e as contagens
##   acima de 0,90 e acima de 1,00.
##
## Não roda dias: lê o estado inicial (dia 0) — é a régua do "a cidade nasce
## cheia ou vazia?". `--out` grava o retrato completo (uma linha por cidade)
## para o antes/depois.
##
##   godot --headless --path . --script res://tools/urban_zone_audit.gd
##     [--tag=before] [--out=user://urban_zone_audit/before.json]

const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const PORTS_PATH := "res://data/definitions/scenario/ports.json"
const BUILDINGS_PATH := "res://data/definitions/scenario/buildings.json"
const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"
const SERVICES_PATH := "res://data/definitions/economy/services.json"
const CURRENCIES_PATH := "res://data/definitions/economy/currencies.json"
const SECTOR_EMPLOYMENT_PATH := "res://data/definitions/economy/sector_employment.json"
const URBAN_LAND_PATH := "res://data/definitions/economy/urban_land.json"
const HOUSING_CITY_PATH := "res://data/definitions/economy/housing_city.json"
const HOUSING_PATH := "res://data/definitions/economy/housing.json"
const MUNICIPAL_FINANCE_PATH := "res://data/definitions/economy/municipal_finance.json"

const ZONE_RES := 0
const ZONE_COM := 1
const ZONE_IND := 2
const ZONE_NAMES := ["RES", "COM", "IND"]

## Faixas de população URBANA (a mesma que alimenta gabarito e estoque).
const BANDS := [
	["<300k", 0, 300000],
	["300k-1M", 300000, 1000000],
	["1M-5M", 1000000, 5000000],
	[">5M", 5000000, 0],
]

var _tag := "audit"
var _out := ""

func _init() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--tag="):
			_tag = arg.split("=", true, 1)[1]
		elif arg.begins_with("--out="):
			_out = arg.split("=", true, 1)[1]
	_run()

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
		_fail(path + " nao e array")
		return []
	return data

func _load_dict(path: String) -> Dictionary:
	var data: Variant = _load_json(path)
	if typeof(data) != TYPE_DICTIONARY:
		_fail(path + " nao e objeto")
		return {}
	return data

func _fail(message: String) -> void:
	printerr("[audit] ERRO: " + message)
	quit(1)

func _run() -> void:
	var cities := _load_array(CITIES_PATH)
	var ports := _load_array(PORTS_PATH)
	var buildings := _load_array(BUILDINGS_PATH)
	var goods := _load_array(GOODS_PATH)
	if cities.is_empty() or buildings.is_empty() or goods.is_empty():
		_fail("cenario vazio")
		return
	var sim = EconomySim.new()
	if not sim.has_method("set_urban") or not sim.has_method("get_urban_city"):
		_fail("binario sem a camada urbana (§19) — rode o build")
		return
	sim.set_goods(goods)
	sim.set_recipes(_load_array(RECIPES_PATH))
	sim.set_needs(_load_dict(NEEDS_PATH))
	if sim.has_method("set_services"):
		sim.set_services(_load_dict(SERVICES_PATH))
	if sim.has_method("set_currencies"):
		sim.set_currencies(_load_dict(CURRENCIES_PATH))
	var sector := _load_dict(SECTOR_EMPLOYMENT_PATH)
	if sim.has_method("set_sector_employment") and not sector.is_empty():
		sim.set_sector_employment(sector)
	sim.initialize_world(cities, ports, buildings)
	sim.set_urban(_load_dict(URBAN_LAND_PATH), _load_dict(HOUSING_CITY_PATH),
		_load_dict(HOUSING_PATH), _load_dict(MUNICIPAL_FINANCE_PATH))
	# A indústria de referência (§12.3) é semeada no PRIMEIRO tick, não na carga.
	# Sem chamá-la, a foto do dia 0 mostraria IND = 0 por ausência do estoque
	# inicial — não por não caber. Semeia antes de ler para medir o que existe.
	if sim.has_method("seed_industry"):
		sim.seed_industry()

	var land: Dictionary = _load_dict(URBAN_LAND_PATH).get("cities", {})
	var rows: Array = []
	var by_band: Dictionary = {}
	for band in BANDS:
		by_band[band[0]] = {"res": [], "com": [], "ind": [], "n": 0,
			"over_1": 0, "over_09": 0}
	for city in cities:
		var cid := String(city["id"])
		var data: Dictionary = sim.get_urban_city(cid)
		if data.is_empty():
			continue
		var occ: Array = data.get("zone_occupancy", [])
		var usage: Array = data.get("zone_usage", [])
		var cap: Array = data.get("zone_capacity", [])
		if occ.size() != 3 or usage.size() != 3 or cap.size() != 3:
			_fail(cid + ": arrays de zona com tamanho != 3")
			return
		var urban_pop := int(city.get("urban_population", 0))
		var band_name := _band_of(urban_pop)
		var entry: Dictionary = by_band[band_name]
		entry["n"] = int(entry["n"]) + 1
		var over_1 := false
		var over_09 := false
		for zone in 3:
			var value := float(occ[zone])
			entry[["res", "com", "ind"][zone]].append(value)
			if value > 1.0:
				over_1 = true
			if value > 0.9:
				over_09 = true
		if over_1:
			entry["over_1"] = int(entry["over_1"]) + 1
		if over_09:
			entry["over_09"] = int(entry["over_09"]) + 1
		rows.append({
			"id": cid,
			"name": city.get("name", ""),
			"nation": city.get("nation", ""),
			"urban_population": urban_pop,
			"sim_population": int(city.get("sim_population", 0)),
			"band": band_name,
			"height_class": String(land.get(cid, {}).get("height_class", "")),
			"built_up_km2": float(land.get(cid, {}).get("built_up_km2", 0.0)),
			"area_km2": data.get("area_km2", []),
			"max_density": data.get("max_density", []),
			"zone_usage": usage,
			"zone_capacity": cap,
			"zone_occupancy": occ,
			"informal_units": float(data.get("informal_units", 0.0)),
			"informal_km2": float(data.get("informal_km2", 0.0)),
			"land_price": data.get("land_price", []),
		})

	print("")
	print("=== AUDITORIA DE ZONAS URBANAS — dia 0 (%s) ===" % _tag)
	print("%-9s %4s %18s %18s %18s" % ["faixa", "n", "RES p10/p50/p90", "COM p10/p50/p90", "IND p10/p50/p90"])
	for band in BANDS:
		var entry: Dictionary = by_band[band[0]]
		print("%-9s %4d %6.3f/%6.3f/%6.3f %6.3f/%6.3f/%6.3f %6.3f/%6.3f/%6.3f  >1:%d >0.9:%d" % [
			band[0], int(entry["n"]),
			_pct(entry["res"], 0.1), _pct(entry["res"], 0.5), _pct(entry["res"], 0.9),
			_pct(entry["com"], 0.1), _pct(entry["com"], 0.5), _pct(entry["com"], 0.9),
			_pct(entry["ind"], 0.1), _pct(entry["ind"], 0.5), _pct(entry["ind"], 0.9),
			int(entry["over_1"]), int(entry["over_09"])])
	var all_res: Array = []
	var all_com: Array = []
	var all_ind: Array = []
	var total_over_1 := 0
	var total_over_09 := 0
	for band in BANDS:
		all_res.append_array(by_band[band[0]]["res"])
		all_com.append_array(by_band[band[0]]["com"])
		all_ind.append_array(by_band[band[0]]["ind"])
		total_over_1 += int(by_band[band[0]]["over_1"])
		total_over_09 += int(by_band[band[0]]["over_09"])
	print("%-9s %4d %6.3f/%6.3f/%6.3f %6.3f/%6.3f/%6.3f %6.3f/%6.3f/%6.3f  >1:%d >0.9:%d" % [
		"TODAS", rows.size(),
		_pct(all_res, 0.1), _pct(all_res, 0.5), _pct(all_res, 0.9),
		_pct(all_com, 0.1), _pct(all_com, 0.5), _pct(all_com, 0.9),
		_pct(all_ind, 0.1), _pct(all_ind, 0.5), _pct(all_ind, 0.9),
		total_over_1, total_over_09])
	print("cidades com alguma zona >1,0: %d | >0,90: %d" % [total_over_1, total_over_09])
	print("")
	print("%-9s %-16s %10s %5s %9s %10s %10s %8s %8s %8s" % [
		"id", "nome", "pop_urb", "gab", "km2", "uso_res", "cap_res", "occ_RES", "occ_COM", "occ_IND"])
	for cid in ["BRA-SAO", "JPN-TOK", "NGA-LAG", "IND-MUM", "USA-NEW", "AUS-WEI", "NPL-POK"]:
		for row in rows:
			if row["id"] != cid:
				continue
			print("%-9s %-16s %10d %5s %9.1f %10.0f %10.0f %8.3f %8.3f %8.3f" % [
				cid, String(row["name"]).substr(0, 16), row["urban_population"],
				row["height_class"], row["built_up_km2"], float(row["zone_usage"][ZONE_RES]),
				float(row["zone_capacity"][ZONE_RES]), float(row["zone_occupancy"][ZONE_RES]),
				float(row["zone_occupancy"][ZONE_COM]),
				float(row["zone_occupancy"][ZONE_IND])])
			break

	if _out != "":
		var dir := _out.get_base_dir()
		var absolute_dir := dir
		if dir.begins_with("res://") or dir.begins_with("user://"):
			absolute_dir = ProjectSettings.globalize_path(dir)
		if absolute_dir != "" and not DirAccess.dir_exists_absolute(absolute_dir):
			DirAccess.make_dir_recursive_absolute(absolute_dir)
		var file := FileAccess.open(_out, FileAccess.WRITE)
		if file == null:
			_fail("nao abriu para escrita: " + _out)
			return
		file.store_string(JSON.stringify({"tag": _tag, "day": 0, "cities": rows}, "  "))
		file.close()
		print("[audit] escrito %s (%d cidades)" % [_out, rows.size()])
	quit(0)

func _band_of(population: int) -> String:
	for band in BANDS:
		var low := int(band[1])
		var high := int(band[2])
		if population >= low and (high == 0 or population < high):
			return String(band[0])
	return ">5M"

## Percentil (interpolação linear), igual ao relatório de sweep.
func _pct(values: Array, q: float) -> float:
	if values.is_empty():
		return NAN
	var sorted := values.duplicate()
	sorted.sort()
	var k := (sorted.size() - 1) * q
	var lower := int(floor(k))
	var upper := int(ceil(k))
	if lower == upper:
		return float(sorted[lower])
	return float(sorted[lower]) + (float(sorted[upper]) - float(sorted[lower])) * (k - lower)
