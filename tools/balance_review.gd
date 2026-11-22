extends SceneTree

## Diagnóstico de balanceamento global (Fase A — só leitura).
##
## Roda o MESMO pipeline do jogo ao vivo (initialize_world + set_goods
## + GeographyService.apply_to_sim + EconomicAI) por um período longo e tabele,
## por nação e por bem, produção/dia, consumo/dia, estoque e satisfação alimentar.
## Não altera código nem dados: apenas mede e escreve
## docs/economy/balance_review_sim.json + tabelas no stdout.
##
##   godot --headless --path . --script res://tools/balance_review.gd [--days=730]

const GeographyServiceScript := preload("res://core/economy/geography_service.gd")
const EconomicAIScript := preload("res://core/economy/ai/economic_ai.gd")

const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const PORTS_PATH := "res://data/definitions/scenario/ports.json"
const BUILDINGS_PATH := "res://data/definitions/scenario/buildings.json"
const GOODS_PATH := "res://data/definitions/economy/goods.json"
const OUT_PATH := "res://docs/economy/balance_review_sim.json"

var _days := 730
var _fix_load := false


func _init() -> void:
	var args := OS.get_cmdline_user_args()
	for arg in args:
		if arg.begins_with("--days="):
			_days = int(arg.split("=")[1])
		elif arg == "--fix-load":
			_fix_load = true
	_run.call_deferred()


func _load_array(path: String) -> Array:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return []
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data if typeof(data) == TYPE_ARRAY else []


func _run() -> void:
	var cities := _load_array(CITIES_PATH)
	var ports := _load_array(PORTS_PATH)
	var buildings := _load_array(BUILDINGS_PATH)
	var goods := _load_array(GOODS_PATH)

	var sim = EconomySim.new()
	var geo = GeographyServiceScript.new()
	if _fix_load:
		geo.load_data()  # FIX: carrega antes (o jogo ao vivo não faz isso)
	# Resolve a fazenda inicial de cada cidade para o alimento básico de maior
	# aptidão × caloria da nação (o mesmo que o jogo ao vivo faz em economy.gd).
	sim.initialize_world(cities, ports, geo.resolve_initial_farms(buildings, cities))
	sim.set_goods(goods)
	var applied := geo.apply_to_sim(sim, _cities_with_center(cities))

	var ai = EconomicAIScript.new()
	ai.setup(sim, geo)

	var mid: Dictionary = {}
	var trace: Array = []
	for day in _days:
		var status: Dictionary = sim.tick_day()
		# Reusa o resumo do tick: a IA não chama get_status de novo.
		ai.run(int(status.get("day", 0)), status)
		if day == _days / 2 - 1:
			mid = _snapshot(sim)
		if day == 0 or (day + 1) % 30 == 0:
			trace.append(_trace_point(day + 1, sim))

	var final := _snapshot(sim)
	var status: Dictionary = sim.get_status()

	var report := {
		"meta": {
			"days": _days,
			"money_total": status.get("money_total", 0.0),
			"foreign_cash": status.get("foreign_cash", 0.0),
			"bankruptcies": status.get("bankruptcies", 0),
			"invariant_violations": status.get("invariant_violations", []),
		},
		"trace": trace,
		"products": status.get("products", []),
		"nations": final.get("nations", {}),
		"nations_mid": mid.get("nations", {}),
		"global": final.get("global", {}),
		"buildings": final.get("buildings", {}),
		"deposits": final.get("deposits", {}),
		"applied_geography": applied,
	}

	_write_json(OUT_PATH, report)
	_print_report(report, mid, final)
	# Cópia rotulada pelo modo, para preservar ambos os cenários.
	var mode := "fixed" if _fix_load else "broken"
	_write_json("res://docs/economy/balance_review_sim_%s.json" % mode, report)
	quit(0)


func _cities_with_center(cities: Array) -> Array:
	var result: Array = []
	for city in cities:
		result.append({
			"id": String(city.get("id", "")),
			"nation": String(city.get("nation", "")),
			"center": Vector2(float(city.get("x", 0.0)), float(city.get("y", 0.0))),
		})
	return result


## Congela o estado relevante do sim (por nação e global) sem depender do C++.
func _snapshot(sim: Object) -> Dictionary:
	var status: Dictionary = sim.get_status()
	var products: Array = status.get("products", [])
	var nations := {}
	var city_nation := {}
	for city in status.get("cities", []):
		# O status diário publica só o bloco leve: pop/markets/deposits vêm
		# sob demanda de get_city_status(id).
		var heavy := _heavy(sim, city)
		var tag := String(city.get("nation", ""))
		city_nation[String(city.get("id", ""))] = tag
		if not nations.has(tag):
			nations[tag] = {
				"population": 0.0,
				"cities": [],
				"food": {"consumed": 0.0, "need": 0.0, "satisfaction": 1.0},
				"goods": {},
				"reserves_kt": {},
			}
		nations[tag]["population"] += float(city.get("population", 0.0))
		nations[tag]["cities"].append(String(city.get("id", "")))
		var pop: Dictionary = heavy.get("pop", {})
		nations[tag]["food"]["consumed"] += float(pop.get("consumed", 0.0))
		nations[tag]["food"]["need"] += float(pop.get("need", 0.0))
		for product_index in products.size():
			var name := String(products[product_index])
			var market: Dictionary = heavy.get("markets", [])[product_index]
			if not nations[tag]["goods"].has(name):
				nations[tag]["goods"][name] = {
					"prod_t_day": 0.0, "cons_t_day": 0.0, "stock_t": 0.0,
					"export_t_day": 0.0, "export_value": 0.0,
				}
			nations[tag]["goods"][name]["prod_t_day"] += float(market.get("supply", 0.0))
			nations[tag]["goods"][name]["cons_t_day"] += float(market.get("demand", 0.0))
			nations[tag]["goods"][name]["stock_t"] += float(market.get("stock", 0.0))
			var exported: Array = city.get("exported", [])
			var export_value: Array = city.get("export_value", [])
			if product_index < exported.size():
				nations[tag]["goods"][name]["export_t_day"] += float(exported[product_index])
			if product_index < export_value.size():
				nations[tag]["goods"][name]["export_value"] += float(export_value[product_index])
		for product_name in heavy.get("deposits", {}):
			var entry: Dictionary = heavy.get("deposits", {})[product_name]
			if not nations[tag]["reserves_kt"].has(product_name):
				nations[tag]["reserves_kt"][product_name] = 0.0
			nations[tag]["reserves_kt"][product_name] += float(entry.get("remaining_kt", 0.0))
	for tag in nations:
		var food: Dictionary = nations[tag]["food"]
		food["satisfaction"] = food["consumed"] / food["need"] if food["need"] > 0.0 else 1.0
		food["deficit"] = maxf(0.0, food["need"] - food["consumed"])

	# Agregação global: produção/consumo por bem + reservas minerais restantes.
	var global := {"produced_t_day": {}, "consumed_t_day": {}, "stock_t": {},
		"reserves_kt": {}, "buildings_by_output": {}}
	for product_index in products.size():
		var name := String(products[product_index])
		global["produced_t_day"][name] = 0.0
		global["consumed_t_day"][name] = 0.0
		global["stock_t"][name] = 0.0
		global["reserves_kt"][name] = null
		global["buildings_by_output"][name] = 0
	for city in status.get("cities", []):
		var heavy := _heavy(sim, city)
		for product_index in products.size():
			var name := String(products[product_index])
			var market: Dictionary = heavy.get("markets", [])[product_index]
			global["produced_t_day"][name] += float(market.get("supply", 0.0))
			global["consumed_t_day"][name] += float(market.get("demand", 0.0))
			global["stock_t"][name] += float(market.get("stock", 0.0))
		for building in heavy.get("buildings", []):
			var output := int(building.get("output_product", -1))
			if output >= 0 and output < products.size():
				var name := String(products[output])
				global["buildings_by_output"][name] += 1
		var deposits: Dictionary = heavy.get("deposits", {})
		for product_name in deposits:
			var entry: Dictionary = deposits[product_name]
			if global["reserves_kt"].has(product_name):
				var current = global["reserves_kt"][product_name]
				global["reserves_kt"][product_name] = (0.0 if current == null else float(current)) \
					+ float(entry.get("remaining_kt", 0.0))

	# Lista de construções ativas no fim (para explicar o que foi produzido).
	var buildings_summary := []
	for city in status.get("cities", []):
		for building in _heavy(sim, city).get("buildings", []):
			buildings_summary.append({
				"city": String(city.get("id", "")),
				"id": String(building.get("id", "")),
				"output": int(building.get("output_product", -1)),
				"input": int(building.get("input_product", -1)),
				"capacity": float(building.get("capacity", 0.0)),
				"utilization": float(building.get("utilization", 0.0)),
				"owner": String(building.get("owner", "")),
				"bankrupt": bool(building.get("bankrupt", false)),
				"profit": float(building.get("profit", 0.0)),
			})

	return {"nations": nations, "global": global, "buildings": buildings_summary,
		"deposits": _deposits_snapshot(sim, status)}


## Detalhe pesado de uma cidade: pop/markets/deposits/buildings deixaram de
## sair no status diário e vêm sob demanda do núcleo.
static func _heavy(sim: Object, city: Dictionary) -> Dictionary:
	var heavy: Dictionary = sim.get_city_status(String(city.get("id", "")))
	return heavy


func _deposits_snapshot(sim: Object, status: Dictionary) -> Dictionary:
	var out := {}
	for city in status.get("cities", []):
		var heavy := _heavy(sim, city)
		var tag := String(city.get("nation", ""))
		if not out.has(tag):
			out[tag] = {}
		for product_name in heavy.get("deposits", {}):
			var entry: Dictionary = heavy.get("deposits", {})[product_name]
			out[tag][product_name] = {
				"remaining_kt": float(entry.get("remaining_kt", 0.0)),
				"quality": float(entry.get("quality", 0.0)),
				"depth_m": float(entry.get("depth_m", 0.0)),
			}
	return out


func _write_json(path: String, data: Variant) -> void:
	var dir := path.get_base_dir()
	if not DirAccess.dir_exists_absolute(dir):
		DirAccess.make_dir_recursive_absolute(dir)
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		push_error("não abriu para escrita: " + path)
		return
	file.store_string(JSON.stringify(data, "  "))
	file.close()
	print("[balance_review] escrito " + path)


func _print_report(report: Dictionary, mid: Dictionary, final: Dictionary) -> void:
	var products: Array = report.get("products", [])
	var nations: Dictionary = report.get("nations", {})
	var global: Dictionary = report.get("global", {})

	print("")
	print("===== BALANCE REVIEW (sim, %d dias) =====" % int(report["meta"]["days"]))
	print("dinheiro_total=%.2f foreign_cash=%.2f falências=%d violações=%d" % [
		float(report["meta"]["money_total"]),
		float(report["meta"]["foreign_cash"]),
		int(report["meta"]["bankruptcies"]),
		int(report["meta"]["invariant_violations"].size()),
	])

	print("")
	print("--- Alimentação por nação (fim) ---")
	for tag in nations:
		var food: Dictionary = nations[tag]["food"]
		var mid_food: Dictionary = mid["nations"].get(tag, {}).get("food", {})
		var trend := _trend(food["satisfaction"], mid_food.get("satisfaction", 0.0))
		print("  %-4s pop=%-9.0f cons=%.0f/%.0f sat=%.0f%% def=%.1f trend=%s" % [
			tag, nations[tag]["population"],
			food["consumed"], food["need"], food["satisfaction"] * 100.0,
			food["deficit"], trend,
		])

	print("")
	print("--- Produção/consumo por bem (fim, t/dia, global) ---")
	for name in products:
		var prod := float(global["produced_t_day"][name])
		var cons := float(global["consumed_t_day"][name])
		var stock := float(global["stock_t"][name])
		var reserve = global["reserves_kt"][name]
		var reserve_s := "—" if reserve == null else "%.0f kt" % float(reserve)
		var buildings := int(global["buildings_by_output"][name])
		var flag := ""
		if prod <= 0.0 and cons <= 0.0 and buildings == 0:
			flag = "SEM_PRODUTOR"
		elif prod <= 0.0 and cons > 0.0:
			flag = "SEM_PRODUTOR_COM_CONSUMO"
		elif cons <= 0.0 and prod > 0.0:
			flag = "SEM_CONSUMIDOR"
		print("  %-12s prod=%-9.3f cons=%-9.3f stock=%-10.3f reserva=%-12s constr=%d %s" % [
			name, prod, cons, stock, reserve_s, buildings, flag,
		])

	print("")
	print("--- Evolução (satisfação % / minas ferro) ---")
	for point in report.get("trace", []):
		var day := int(point["day"])
		var iron := int(point["iron_mines"])
		var parts := PackedStringArray()
		for tag in point["nations"]:
			var n: Dictionary = point["nations"][tag]
			parts.append("%s=%d%%" % [tag, int(float(n["sat"]) * 100.0)])
		print("  d%-4d minasFe=%d | %s" % [day, iron, " ".join(parts)])

	print("")
	print("--- Exportações por nação (fim): receita externa e anos até esgotar ---")
	for tag in nations:
		var n: Dictionary = nations[tag]
		var lines := PackedStringArray()
		for name in products:
			var good: Dictionary = n["goods"].get(name, {})
			var exp_t := float(good.get("export_t_day", 0.0))
			if exp_t <= 0.0:
				continue
			var value := float(good.get("export_value", 0.0))
			var rem_kt := float(n["reserves_kt"].get(name, -1.0))
			var years := ""
			if rem_kt >= 0.0 and exp_t > 0.0:
				years = "%.0f a" % (rem_kt * 1000.0 / (exp_t * 365.0))
			else:
				years = "renovável"
			lines.append("%s(%.1f t/d, %.0f/d, %s)" % [name, exp_t, value, years])
		if lines.is_empty():
			continue
		print("  %-4s %s" % [tag, " ".join(lines)])

	print("")
	print("--- Construções ativas no fim (por output) ---")
	for b in report.get("buildings", []):
		print("  %-9s %-24s out=%2d in=%2d cap=%-8.3f util=%.2f %s%s" % [
			b["city"], b["id"], b["output"], b["input"], b["capacity"],
			b["utilization"], b["owner"], " [FALIDA]" if b["bankrupt"] else "",
		])


## Ponto de rastreio da evolução (para explicar o colapso).
func _trace_point(day: int, sim: Object) -> Dictionary:
	var status: Dictionary = sim.get_status()
	var point := {"day": day}
	var nations := {}
	var iron_mine_count := 0
	for city in status.get("cities", []):
		var heavy := _heavy(sim, city)
		var tag := String(city.get("nation", ""))
		var pop: Dictionary = heavy.get("pop", {})
		var wheat: Dictionary = heavy.get("markets", [])[0]
		if not nations.has(tag):
			nations[tag] = {}
		nations[tag] = {
			"sat": float(pop.get("satisfaction", 0.0)),
			"gov_budget": float(city.get("government_budget", 0.0)),
			"firm_cash": float(city.get("firm_cash", 0.0)),
			"service_cash": float(city.get("service_cash", 0.0)),
			"wheat_stock": float(wheat.get("stock", 0.0)),
			"wheat_price": float(wheat.get("price", 0.0)),
		}
		for building in heavy.get("buildings", []):
			var output := int(building.get("output_product", -1))
			if output == 1:
				iron_mine_count += 1
	point["nations"] = nations
	point["iron_mines"] = iron_mine_count
	return point


func _trend(current: float, mid: float) -> String:
	if mid <= 0.0:
		return "?"
	var delta := current - mid
	if absf(delta) < 0.01:
		return "estável"
	if delta > 0.05:
		return "melhora"
	if delta < -0.05:
		return "COLAPSO"
	return "estável"
