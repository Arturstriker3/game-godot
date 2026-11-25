extends SceneTree

## Diagnóstico de falências e fome (só leitura). Roda o MESMO pipeline do jogo
## (initialize_world + goods + GeographyService + EconomicAI) e registra,
## dia a dia, que construções desapareceram (falência) com o último estado
## conhecido; no fim, tabela por nação/cidade do alimento atendido e do caixa.
##
##   godot --headless --path . --script res://tools/economy_bankruptcy_diag.gd [--days=730]

const GeographyServiceScript := preload("res://core/economy/geography_service.gd")

const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const PORTS_PATH := "res://data/definitions/scenario/ports.json"
const BUILDINGS_PATH := "res://data/definitions/scenario/buildings.json"
const GOODS_PATH := "res://data/definitions/economy/goods.json"
const OUT_PATH := "res://docs/economy/bankruptcy_diag.json"

var _days := 730
var _meta := {}


func _init() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--days="):
			_days = int(arg.split("=")[1])
	_run.call_deferred()


func _load(path: String) -> Array:
	var data: Variant = JSON.parse_string(FileAccess.get_file_as_string(path))
	return data if typeof(data) == TYPE_ARRAY else []


func _run() -> void:
	var cities := _load(CITIES_PATH)
	var ports := _load(PORTS_PATH)
	var buildings := _load(BUILDINGS_PATH)
	var goods := _load(GOODS_PATH)
	var products: Array = []
	var keys := {}
	for good in goods:
		var id := String(good.get("id", good.get("name", "")))
		keys[id] = keys.size()
		products.append(id)
	var product_name := func(index: int) -> String:
		return String(products[index]) if index >= 0 and index < products.size() else str(index)

	var sim = EconomySim.new()
	var geo = GeographyServiceScript.new()
	geo.load_data()
	sim.initialize_world(cities, ports, geo.resolve_initial_farms(buildings, cities))
	sim.set_goods(goods)
	geo.apply_to_sim(sim, _cities_with_center(cities))
	sim.set_ai_enabled(true)

	var previous: Dictionary = {}
	var closures: Array = []
	var first_seen: Dictionary = {}
	for day in _days:
		var status: Dictionary = sim.tick_day()
		var current := {}
		for city in status.get("cities", []):
			var cid := String(city.get("id", ""))
			for building in _heavy(sim, city).get("buildings", []):
				var bid := "%s|%s" % [cid, String(building.get("id", ""))]
				current[bid] = {
					"city": cid, "nation": String(city.get("nation", "")),
					"id": String(building.get("id", "")),
					"output": int(building.get("output_product", -1)),
					"capacity": float(building.get("capacity", 0.0)),
					"utilization": float(building.get("utilization", 0.0)),
					"owner": String(building.get("owner", "")),
					"profit": float(building.get("profit", 0.0)),
					"bankrupt": bool(building.get("bankrupt", false)),
				}
				if not first_seen.has(bid):
					first_seen[bid] = {"day": day + 1, "output": int(building.get("output_product", -1))}
		if day > 0:
			for bid in previous:
				if not current.has(bid):
					var gone: Dictionary = (previous[bid] as Dictionary).duplicate()
					gone["closed_day"] = day + 1
					gone["born_day"] = int(first_seen.get(bid, {}).get("day", 0))
					gone["lifetime_days"] = (day + 1) - int(gone["born_day"])
					gone["product"] = product_name.call(int(gone["output"]))
					closures.append(gone)
		previous = current
	var status: Dictionary = sim.get_status()
	var final := _snapshot_by_nation(sim, status)
	var summary := _summarize(closures, final)
	_write_json(OUT_PATH, {"meta": {"days": _days,
		"bankruptcies": int(status.get("bankruptcies", 0)),
		"depleted_mines": int(status.get("depleted_mines", 0)),
		"money_total": float(status.get("money_total", 0.0)),
		"foreign_cash": float(status.get("foreign_cash", 0.0)),
		"invariant_violations": status.get("invariant_violations", [])},
		"summary": summary, "closures": closures, "nations": final})
	_meta = {"bankruptcies": int(status.get("bankruptcies", 0)),
		"depleted_mines": int(status.get("depleted_mines", 0))}
	_print(summary, final, closures)
	quit(0)


func _cities_with_center(cities: Array) -> Array:
	var result: Array = []
	for city in cities:
		result.append({"id": String(city.get("id", "")),
			"nation": String(city.get("nation", "")),
			"center": Vector2(float(city.get("x", 0.0)), float(city.get("y", 0.0)))})
	return result


## Detalhe pesado da cidade (pop/deposits/buildings): o status diário publica
## só o bloco leve, então o diagnóstico vem sob demanda de get_city_status(id).
static func _heavy(sim: Object, city: Dictionary) -> Dictionary:
	var heavy: Dictionary = sim.get_city_status(String(city.get("id", "")))
	return heavy


func _snapshot_by_nation(sim: Object, status: Dictionary) -> Dictionary:
	var nations := {}
	var products: Array = status.get("products", [])
	for city in status.get("cities", []):
		var heavy := _heavy(sim, city)
		var tag := String(city.get("nation", ""))
		if not nations.has(tag):
			nations[tag] = {"need": 0.0, "consumed": 0.0, "pop": 0.0, "cities": []}
		var n: Dictionary = nations[tag]
		var pop: Dictionary = heavy.get("pop", {})
		n["need"] += float(pop.get("need", 0.0))
		n["consumed"] += float(pop.get("consumed", 0.0))
		n["pop"] += float(city.get("population", 0.0))
		var imports := 0.0
		var imported: Array = city.get("imported", [])
		for slot in range(min(imported.size(), products.size())):
			var name := String(products[slot])
			if name in ["wheat", "rice", "maize", "potato", "soy"]:
				imports += float(imported[slot])
		n["cities"].append({"id": String(city.get("id", "")),
			"need": float(pop.get("need", 0.0)), "consumed": float(pop.get("consumed", 0.0)),
			"imported_today": imports, "firm_cash": float(city.get("firm_cash", 0.0)),
			"gov_budget": float(city.get("government_budget", 0.0)),
			"service_cash": float(city.get("service_cash", 0.0)),
			"pop": float(city.get("population", 0.0)),
			"savings": float(pop.get("savings", 0.0)),
			"wage_income": float(pop.get("wage_income", 0.0)),
			"food_spend": float(pop.get("food_spend", 0.0)),
			"disposable": float(pop.get("disposable", 0.0)),
			"gdp": float(city.get("gdp", 0.0)),
			"external_demand": city.get("external_demand", []),
			"deposits": heavy.get("deposits", {}),
			"buildings": heavy.get("buildings", [])})
	for tag in nations:
		var n: Dictionary = nations[tag]
		n["sat"] = n["consumed"] / n["need"] if n["need"] > 0.0 else 1.0
	return nations


func _summarize(closures: Array, final: Dictionary) -> Dictionary:
	var by_product := {}
	var by_owner := {}
	var by_nation := {}
	var by_city := {}
	var by_lifetime := {"<=60": 0, "61-120": 0, "121-365": 0, ">365": 0}
	for c in closures:
		var p := String(c.get("product", ""))
		var o := String(c.get("owner", ""))
		var n := String(c.get("nation", ""))
		by_product[p] = int(by_product.get(p, 0)) + 1
		by_owner[o] = int(by_owner.get(o, 0)) + 1
		by_nation[n] = int(by_nation.get(n, 0)) + 1
		var ck := "%s %s" % [String(c.get("city", "")), p]
		by_city[ck] = int(by_city.get(ck, 0)) + 1
		var life := int(c.get("lifetime_days", 0))
		if life <= 60:
			by_lifetime["<=60"] += 1
		elif life <= 120:
			by_lifetime["61-120"] += 1
		elif life <= 365:
			by_lifetime["121-365"] += 1
		else:
			by_lifetime[">365"] += 1
	var hungry: Array = []
	for tag in final:
		if float(final[tag]["sat"]) < 0.70:
			hungry.append([tag, float(final[tag]["sat"])])
	hungry.sort_custom(func(a, b): return a[1] < b[1])
	return {"closures": closures.size(), "by_product": by_product, "by_owner": by_owner,
		"by_nation": by_nation, "by_city_top": _top(by_city, 25),
		"by_lifetime": by_lifetime, "hungry": hungry}


func _top(dict: Dictionary, limit: int) -> Array:
	var items: Array = dict.keys()
	items.sort_custom(func(a, b): return int(dict[a]) > int(dict[b]))
	var out: Array = []
	for i in range(min(limit, items.size())):
		out.append([items[i], dict[items[i]]])
	return out


func _print(summary: Dictionary, final: Dictionary, closures: Array) -> void:
	print("")
	print("===== DIAGNÓSTICO %d dias =====" % _days)
	print("falências=%d (insolvência)  fechamentos=%d (inclui mina esgotada)"
		% [int(_meta.get("bankruptcies", 0)), int(summary["closures"])])
	print("minas encerradas por esgotamento=%d" % int(_meta.get("depleted_mines", 0)))
	print("por bem: %s" % str(summary["by_product"]))
	print("por dono: %s" % str(summary["by_owner"]))
	print("por nação: %s" % str(summary["by_nation"]))
	print("por vida: %s" % str(summary["by_lifetime"]))
	print("top cidade+bem: %s" % str(summary["by_city_top"]))
	print("fome (<70%%): %s" % str(summary["hungry"]))
	print("")
	print("--- piores nações: satisfação / caixa / importação do dia ---")
	var tags: Array = final.keys()
	tags.sort_custom(func(a, b): return float(final[a]["sat"]) < float(final[b]["sat"]))
	for i in range(min(20, tags.size())):
		var tag: String = tags[i]
		var n: Dictionary = final[tag]
		print("%-4s sat=%3.0f%% need=%-8.1f cons=%-8.1f pop=%-10.0f" % [
			tag, 100.0 * float(n["sat"]), n["need"], n["consumed"], n["pop"]])
		for city in n["cities"]:
			print("     %-9s need=%-7.1f cons=%-7.1f imp=%-6.1f firm=%-10.0f gov=%-10.0f svc=%-9.0f sav=%-10.0f wage=%-9.0f food=%-9.0f gdp=%-10.0f"
				% [city["id"], city["need"], city["consumed"], city["imported_today"],
					city["firm_cash"], city["gov_budget"], city["service_cash"],
					city["savings"], city["wage_income"], city["food_spend"], city["gdp"]])
			for building in city.get("buildings", []):
				print("        %-24s %-8s out=%-2d cap=%-9.1f util=%.2f profit=%-8.0f costs=%-8.0f"
					% [String(building.get("id", "")), String(building.get("owner", "")),
						int(building.get("output_product", -1)), float(building.get("capacity", 0.0)),
						float(building.get("utilization", 0.0)), float(building.get("profit", 0.0)),
						float(building.get("costs", 0.0))])
	print("")
	print("--- amostra de falências (últimas 30) ---")
	for i in range(max(0, closures.size() - 30), closures.size()):
		var c: Dictionary = closures[i]
		print("  %-9s %-24s %-10s out=%-9s born=d%d closed=d%d life=%d cap=%.1f util=%.2f profit=%.0f"
			% [c["city"], c["id"], c["owner"], c["product"], c["born_day"],
				c["closed_day"], c["lifetime_days"], c["capacity"], c["utilization"], c["profit"]])


func _write_json(path: String, data: Variant) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		push_error("não abriu " + path)
		return
	file.store_string(JSON.stringify(data, "  "))
	file.close()
	print("[diag] escrito " + path)
