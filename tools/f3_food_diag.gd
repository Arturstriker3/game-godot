extends SceneTree

## Diagnóstico da regressão de comida da F3 (EGY/SAH/UZB em 365 d com IA).
## Roda o MESMO pipeline do `economy_world_test --ai=1` e imprime uma série
## temporal por nação: comida (consumo/necessidade), poupança, orçamento,
## importação e preços dos alimentos. Serve para comparar o worktree atual com
## o baseline F2 (bac1b13), onde roda sem os campos novos.
##
##   godot --headless --path . --script res://tools/f3_food_diag.gd -- \
##     --days=365 --ai=1 --every=30 --nations=EGY,SAH,UZB \
##     --out=res://docs/economy/perf/tmp_f3fix/diag.csv
##
## Q-014/R3: `--instant=1` roda com construção instantânea (emula o mundo
## PRÉ-§15) para isolar o efeito do cronograma de obra na comida.
##
## Não é um teste: nunca falha por asserção.

const GeographyServiceScript := preload("res://core/economy/geography_service.gd")

const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const PORTS_PATH := "res://data/definitions/scenario/ports.json"
const BUILDINGS_PATH := "res://data/definitions/scenario/buildings.json"
const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"
## Q-014/R3: o diagnóstico precisa do MESMO pipeline do `economy_world_test`
## (serviços, moedas e âncoras de emprego) — sem eles a série não é comparável
## ao sweep/relatório do mundo.
const SERVICES_PATH := "res://data/definitions/economy/services.json"
const CURRENCIES_PATH := "res://data/definitions/economy/currencies.json"
const SECTOR_EMPLOYMENT_PATH := "res://data/definitions/economy/sector_employment.json"

const FOOD_IDS := ["wheat", "rice", "maize", "potato", "soy"]

var _days := 365
var _every := 30
var _ai_enabled := true
var _nations: Array[String] = ["EGY", "SAH", "UZB"]
var _out := ""
var _goods: Array = []
var _food_products: Array[int] = []
var _rows: Array[String] = []
var _use_needs := true
## A/B do Q-014/R3: construção instantânea emula o comportamento PRÉ-§15
## (capacidade entra no dia da decisão). Sem a flag, o mundo roda com obra
## física (padrão). Só diagnostica: não é critério.
var _instant := false


func _init() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--days="):
			_days = maxi(1, int(arg.split("=")[1]))
		elif arg.begins_with("--every="):
			_every = maxi(1, int(arg.split("=")[1]))
		elif arg.begins_with("--ai="):
			_ai_enabled = arg.split("=", true, 1)[1] == "1"
		elif arg.begins_with("--nations="):
			_nations.clear()
			for tag in arg.split("=", true, 1)[1].split(","):
				_nations.append(tag.strip_edges())
		elif arg.begins_with("--out="):
			_out = arg.split("=", true, 1)[1]
		elif arg.begins_with("--needs="):
			_use_needs = arg.split("=", true, 1)[1] == "1"
		elif arg.begins_with("--instant="):
			_instant = arg.split("=", true, 1)[1] == "1"
	_run()
	quit(0)


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


func _run() -> void:
	var cities := _load_array(CITIES_PATH)
	var ports := _load_array(PORTS_PATH)
	var buildings := _load_array(BUILDINGS_PATH)
	_goods = _load_array(GOODS_PATH)
	var sim = EconomySim.new()
	var has_chains: bool = sim.has_method("set_recipes") \
		and sim.has_method("set_needs")
	sim.set_goods(_goods)
	if _instant and sim.has_method("set_instant_construction"):
		sim.set_instant_construction(true)
	if has_chains:
		sim.set_recipes(_load_array(RECIPES_PATH))
		if _use_needs:
			sim.set_needs(_load_dict(NEEDS_PATH))
		if sim.has_method("set_services"):
			sim.set_services(_load_dict(SERVICES_PATH))
		if sim.has_method("set_currencies"):
			sim.set_currencies(_load_dict(CURRENCIES_PATH))
		var sector := _load_dict(SECTOR_EMPLOYMENT_PATH)
		if sim.has_method("set_sector_employment") and not sector.is_empty():
			sim.set_sector_employment(sector)
	for index in _goods.size():
		var id := String((_goods[index] as Dictionary).get("id", ""))
		if FOOD_IDS.has(id):
			_food_products.append(int((_goods[index] as Dictionary).get("order", index)))
	var geo: Object = null
	if _ai_enabled and has_chains:
		geo = GeographyServiceScript.new()
		geo.load_data()
		buildings = geo.resolve_initial_farms(buildings, cities)
	sim.initialize_world(cities, ports, buildings)
	# A IA roda no C++ (D-131); o detalhe das decisões não é exposto por este
	# caminho, então `ai` fica nulo e os blocos de decisão viram no-op.
	var ai: Object = null
	if geo != null:
		geo.apply_to_sim(sim, _cities_with_center(cities))
		sim.set_ai_enabled(true)
	var status: Dictionary = sim.get_status()
	_emit(sim, 0, status, ai)
	for day in _days:
		status = sim.tick_day()
		var current := int(status.get("day", day + 1))
		if current % _every == 0 or day + 1 == _days:
			_emit(sim, current, status, ai)
	_flush()
	# Diagnóstico de falências: construções que a IA abriu e não existem no
	# status final (falidas/removidas). Ajuda a separar falha de fazenda nova
	# de mina antiga sem tocar no núcleo C++.
	if ai != null:
		var final_ids: Dictionary = {}
		for building in status.get("buildings", []):
			final_ids[String(building.get("id", ""))] = true
		var failed: Dictionary = {}
		for decision in ai.get_decisions():
			var action := String(decision.get("action", ""))
			if action != "build" and action != "build_state":
				continue
			var target := String(decision.get("target", ""))
			if target == "" or final_ids.has(target):
				continue
			var key := "%s:%s:%s" % [String(decision.get("city", "")),
				String(decision.get("resource", "")), action]
			failed[key] = int(failed.get(key, 0)) + 1
		print("  FALIDAS (construções da IA ausentes no fim): %s" % str(failed))


func _cities_with_center(cities: Array) -> Array:
	var result: Array = []
	for city in cities:
		result.append({
			"id": String(city.get("id", "")),
			"nation": String(city.get("nation", "")),
			"center": Vector2(float(city.get("x", 0.0)), float(city.get("y", 0.0))),
		})
	return result


## Detalhe pesado da cidade (markets/pop/buildings): o status diário publica
## só o bloco leve, então o diagnóstico vem sob demanda de get_city_status(id).
static func _heavy(sim: Object, city: Dictionary) -> Dictionary:
	var heavy: Dictionary = sim.get_city_status(String(city.get("id", "")))
	return heavy


func _emit(sim: Object, day: int, status: Dictionary, ai: Object) -> void:
	var global := {
		"production": 0.0, "consumed": 0.0, "imported": 0.0, "stock": 0.0,
		"desired": 0.0, "price": 0.0, "n": 0,
	}
	for city in status.get("cities", []):
		var imported: Array = city.get("imported", [])
		var markets: Array = _heavy(sim, city).get("markets", [])
		for product in _food_products:
			if product < imported.size():
				global["imported"] = float(global["imported"]) + float(imported[product])
			if product < markets.size():
				var market: Dictionary = markets[product]
				global["production"] = float(global["production"]) + float(market.get("supply", 0.0))
				global["stock"] = float(global["stock"]) + float(market.get("stock", 0.0))
				global["desired"] = float(global["desired"]) + float(market.get("desired", 0.0))
				global["price"] = float(global["price"]) + float(market.get("price", 0.0))
				global["n"] = int(global["n"]) + 1
	var totals: Dictionary = {}
	for tag in _nations:
		totals[tag] = {
			"cities": 0, "need": 0.0, "consumed": 0.0, "savings": 0.0,
			"external": 0.0, "budget": 0.0, "imported": 0.0, "stock": 0.0,
			"production": 0.0, "price": 0.0, "price_n": 0, "farms": 0,
			"plant": 0.0, "firm_cash": 0.0, "transfers": 0.0, "disposable": 0.0,
		}
	for city in status.get("cities", []):
		var tag := String(city.get("nation", ""))
		if not totals.has(tag):
			continue
		var row: Dictionary = totals[tag]
		var heavy := _heavy(sim, city)
		var pop: Dictionary = heavy.get("pop", {})
		row["cities"] = int(row["cities"]) + 1
		row["need"] = float(row["need"]) + float(pop.get("need", 0.0))
		row["consumed"] = float(row["consumed"]) + float(pop.get("consumed", 0.0))
		row["savings"] = float(row["savings"]) + float(pop.get("savings", 0.0))
		row["external"] = float(row["external"]) + float(pop.get("external_income", 0.0))
		row["transfers"] = float(row["transfers"]) + float(pop.get("transfers", 0.0))
		row["disposable"] = float(row["disposable"]) + float(pop.get("disposable", 0.0))
		row["budget"] = float(row["budget"]) + float(city.get("government_budget", 0.0))
		row["firm_cash"] = float(row["firm_cash"]) + float(city.get("firm_cash", 0.0))
		var imported: Array = city.get("imported", [])
		var markets: Array = heavy.get("markets", [])
		for product in _food_products:
			if product < markets.size():
				row["production"] = float(row["production"]) \
					+ float((markets[product] as Dictionary).get("supply", 0.0))
			if product < imported.size():
				row["imported"] = float(row["imported"]) + float(imported[product])
			if product < markets.size():
				var market: Dictionary = markets[product]
				row["stock"] = float(row["stock"]) + float(market.get("stock", 0.0))
				row["price"] = float(row["price"]) + float(market.get("price", 0.0))
				row["price_n"] = int(row["price_n"]) + 1
		for building in heavy.get("buildings", []):
			if String(building.get("recipe", "")) != "":
				row["plant"] = float(row["plant"]) + float(building.get("capacity", 0.0))
			elif String(building.get("resource", "")) in FOOD_IDS:
				row["farms"] = int(row["farms"]) + 1
	var line := "day=%d" % day
	var price_n := maxi(1, int(global["n"]))
	line += " | MUNDO prod=%.0f stock=%.0f desired=%.0f imp=%.0f price=%.2f" % [
		float(global["production"]), float(global["stock"]), float(global["desired"]),
		float(global["imported"]), float(global["price"]) / float(price_n)]
	for tag in _nations:
		var row: Dictionary = totals[tag]
		var need := float(row["need"])
		var food := float(row["consumed"]) / need if need > 0.0 else 1.0
		var price_nation := maxi(1, int(row["price_n"]))
		line += " | %s food=%.3f sav=%.0f ext=%.0f budget=%.0f firm=%.0f imp=%.0f stock=%.0f price=%.2f farms=%d plant=%.0f" % [
			tag, food, float(row["savings"]), float(row["external"]),
			float(row["budget"]), float(row["firm_cash"]), float(row["imported"]),
			float(row["stock"]), float(row["price"]) / float(price_nation), int(row["farms"]),
			float(row["plant"])]
	_rows.append(line)
	print(line)
	# Pior cidade de cada nação: o agregado esconde a megacidade sem caixa.
	var worst: Dictionary = {}
	for city in status.get("cities", []):
		var tag := String(city.get("nation", ""))
		if not totals.has(tag):
			continue
		var pop: Dictionary = _heavy(sim, city).get("pop", {})
		var satisfaction := float(pop.get("satisfaction", 0.0))
		if not worst.has(tag) or satisfaction < float(worst[tag].get("satisfaction", 2.0)):
			worst[tag] = {"id": String(city.get("id", "")), "satisfaction": satisfaction,
				"firm": float(city.get("firm_cash", 0.0)),
				"budget": float(city.get("government_budget", 0.0)),
				"savings": float(pop.get("savings", 0.0)),
				"need": float(pop.get("need", 0.0)),
				"consumed": float(pop.get("consumed", 0.0)),
				"income": float(pop.get("disposable", 0.0)),
				"imported": 0.0, "stock": 0.0, "price": 0.0}
	var worst_parts: Array[String] = []
	for tag in _nations:
		if not worst.has(tag):
			continue
		var city_row: Dictionary = worst[tag]
		for city in status.get("cities", []):
			if String(city.get("id", "")) != String(city_row["id"]):
				continue
			var imported: Array = city.get("imported", [])
			var markets: Array = _heavy(sim, city).get("markets", [])
			for product in _food_products:
				if product < imported.size():
					city_row["imported"] = float(city_row["imported"]) + float(imported[product])
				if product < markets.size():
					city_row["stock"] = float(city_row["stock"]) \
						+ float((markets[product] as Dictionary).get("stock", 0.0))
					city_row["price"] = maxf(float(city_row["price"]),
						float((markets[product] as Dictionary).get("price", 0.0)))
			break
		worst_parts.append("%s@%s sat=%.2f firm=%.0f gov=%.0f sav=%.0f imp=%.0f stock=%.0f" % [
			tag, String(city_row["id"]), float(city_row["satisfaction"]),
			float(city_row["firm"]), float(city_row["budget"]),
			float(city_row["savings"]), float(city_row["imported"]),
			float(city_row["stock"])])
	print("  pior cidade: " + " | ".join(worst_parts))
	# Semeadura da F3: quem recebeu as correntes finais (estratégicas).
	if day <= 1:
		for building in status.get("buildings", []):
			var id := String(building.get("id", ""))
			if id.begins_with("SEED-") or bool(building.get("strategic", false)):
				print("  SEED %s: %s (%s)" % [String(building.get("city", "")), id,
					String(building.get("resource", ""))])
	if ai != null and day % _every == 0:
		var counts: Dictionary = {}
		var per_nation: Dictionary = {}
		for decision in ai.get_decisions():
			if String(decision.get("action", "")) != "build" \
					and String(decision.get("action", "")) != "expand" \
					and String(decision.get("action", "")) != "build_state":
				continue
			var key := "%s:%s" % [String(decision.get("agent", "")), String(decision.get("resource", ""))]
			counts[key] = int(counts.get(key, 0)) + 1
			var city_id := String(decision.get("city", ""))
			var tag := city_id.get_slice("-", 0)
			if _nations.has(tag):
				var nation_key := "%s:%s" % [tag, key]
				per_nation[nation_key] = int(per_nation.get(nation_key, 0)) + 1
		print("  IA ate %d: %s" % [day, str(counts)])
		print("  IA por nação ate %d: %s" % [day, str(per_nation)])


func _flush() -> void:
	if _out == "":
		return
	var dir := _out.get_base_dir()
	if dir != "" and not DirAccess.dir_exists_absolute(dir):
		DirAccess.make_dir_recursive_absolute(dir)
	var file := FileAccess.open(_out, FileAccess.WRITE)
	if file == null:
		return
	file.store_string("\n".join(_rows) + "\n")
	file.close()
