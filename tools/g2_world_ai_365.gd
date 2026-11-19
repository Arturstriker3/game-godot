extends SceneTree

## G — Bloco 2b: mundo real com IA por 365 dias, com a agricultura sazonal.
##
##   godot --headless --path . --script res://tools/g2_world_ai_365.gd -- \
##     [--days=365] [--agriculture=1] [--no-initial-stock=1] [--out=path.json]
##
## Monta o mundo pelo caminho do AUTOLOAD: cidades + geografia real + infra B5
## inicial + camada urbana + B7 (informalidade/saúde) + AgricultureService
## (planos com o calendário herdado da nação original) + IA do núcleo,
## `threads=1`.
##
## Mede:
##   * PORTÃO DE COMIDA do M2: média dos últimos 30 d de consumido/necessidade
##     por nação (< 0,975 = fora), com a tabela das piores e a causa dominante;
##   * invariantes, falências, drift de dinheiro e dia final;
##   * preço mensal de trigo/arroz/milho/soja/batata por HEMISFÉRIO (razão ao
##     preço-base, ponderada pela quantidade vendida);
##   * estoque médio por mês de cada cultura;
##   * perdas da agricultura no ano (entrada/armazém/perecibilidade) e estoque
##     final.
##
## `--no-initial-stock=1` roda a sonda que isola o mecanismo (mesmos planos, sem
## o estoque do dia 1). `--agriculture=0` é o A/B de controle.

const GeoScript := preload("res://core/economy/geography_service.gd")
const AgriScript := preload("res://core/economy/agriculture_service.gd")
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

const CROPS: Array[String] = ["wheat", "rice", "maize", "soy", "potato"]
## Amostra pedida: norte USA/CHN/IND/FRA; sul BRA/ARG/AUS/ZAF.
const NORTH: Array[String] = ["USA", "CHN", "IND", "FRA"]
const SOUTH: Array[String] = ["BRA", "ARG", "AUS", "ZAF"]
const GATE := 0.975
const WINDOW := 30
const PRICE_EVERY := 30

var _days := 365
var _agriculture := true
var _initial_stock := true
var _out := "res://docs/economy/perf/g2_world_ai_365_rerun.json"


func _init() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--days="):
			_days = maxi(0, int(arg.split("=", true, 1)[1]))
		elif arg.begins_with("--agriculture="):
			_agriculture = arg.split("=", true, 1)[1] != "0"
		elif arg.begins_with("--no-initial-stock="):
			_initial_stock = arg.split("=", true, 1)[1] == "0"
		elif arg.begins_with("--out="):
			_out = arg.split("=", true, 1)[1]
	var boom := _run()
	if boom.is_empty():
		quit(1)
		return
	_finish(boom)


func _finish(out: Dictionary) -> void:
	var dir := _out.get_base_dir()
	if dir != "" and not DirAccess.dir_exists_absolute(dir):
		DirAccess.make_dir_recursive_absolute(dir)
	var file := FileAccess.open(_out, FileAccess.WRITE)
	if file == null:
		print("[g2] FALHA: nao abriu %s" % _out)
		quit(1)
		return
	file.store_string(JSON.stringify(out, "  ", true))
	file.close()
	var food: Dictionary = out["food"]
	print("[g2] dia_final=%d falencias=%d drift=%+.6f violacoes=%d | "
		% [int(out["final_day"]), int(out["bankruptcies"]),
		float(out["money_drift"]), int(out["invariant_violations"])]
		+ "portao=%d/%d abaixo de %.3f (mediana %.4f, min %s %.4f) | "
		% [int(food["below_count"]), int(food["nations"]), GATE,
		float(food["median"]), String(food["min_nation"]), float(food["min"])]
		+ "hash=%s" % String(out["trajectory_hash"]))
	var composition: Dictionary = out.get("bankrupt_composition", {})
	print("[g2] falencias: total=%d com_plano_safra=%d produtos=%s" % [
		int(composition.get("total", 0)), int(composition.get("with_crop_plan", 0)),
		str(composition.get("by_product", {}))])
	for row in Array(food["below"]).slice(0, 15):
		print("[g2]   %s comida=%.3f causa=%s" % [String(row["tag"]),
			float(row["food"]), String(row.get("cause", ""))])
	quit(0)


# ---------------------------------------------------------------------------
# Mundo
# ---------------------------------------------------------------------------

func _run() -> Dictionary:
	var cities: Array = _load(CITIES_PATH) as Array
	var ports: Array = _load(PORTS_PATH) as Array
	var buildings: Array = _load(BUILDINGS_PATH) as Array
	var goods: Array = _load(GOODS_PATH) as Array
	if cities.is_empty() or goods.is_empty():
		print("[g2] FALHA: cenário vazio")
		return {}
	var base_price: Dictionary = {}
	for row in goods:
		var item: Dictionary = row
		var good_id := String(item.get("id", ""))
		base_price[good_id] = float(item.get("base_price", 1.0))
	RegionDataScript.attach(cities)
	var sim = EconomySim.new()
	sim.set_goods(goods)
	sim.set_recipes(_load(RECIPES_PATH))
	sim.set_needs(_load(NEEDS_PATH))
	sim.set_services(_load(SERVICES_PATH))
	sim.set_currencies(_load(CURRENCIES_PATH))
	sim.set_sector_employment(_load(SECTOR_EMPLOYMENT_PATH))
	sim.set_age_structure(_load(AGE_STRUCTURE_PATH))
	sim.set_informality(_load(INFORMALITY_PATH))
	sim.set_health_access(_load(HEALTH_ACCESS_PATH))
	if sim.has_method("set_sim_threads"):
		sim.set_sim_threads(1)
	var geo := GeoScript.new()
	geo.load_data()
	buildings = geo.resolve_initial_farms(buildings, cities)
	if geo.has_method("resolve_initial_mines"):
		buildings = geo.resolve_initial_mines(buildings, cities)
	sim.initialize_world(cities, ports, buildings)
	GeoScript.apply_initial_infra(sim)
	if sim.has_method("set_urban"):
		sim.set_urban(_load(URBAN_LAND_PATH), _load(HOUSING_CITY_PATH),
			_load(HOUSING_PATH), _load(MUNICIPAL_FINANCE_PATH))
	var centers: Array = []
	for city in cities:
		centers.append({
			"id": String((city as Dictionary).get("id", "")),
			"nation": String((city as Dictionary).get("nation", "")),
			"center": Vector2(float((city as Dictionary).get("x", 0.0)),
				float((city as Dictionary).get("y", 0.0))),
		})
	geo.apply_to_sim(sim, centers)
	var agri := AgriScript.new()
	var agri_report: Dictionary = {}
	if _agriculture and sim.has_method("set_crop_plan"):
		agri_report = agri.apply_to_sim(sim, cities, geo, {
			"calendar_nations": agri.calendar_nations(cities),
			"initial_stock": _initial_stock,
			"farms": AgriScript.farms_of(buildings),
		})
	sim.set_ai_enabled(true)
	# D-099: censo das fazendas iniciais × plano de safra — quem a regra de
	# liquidez NÃO cobre (par cidade×cultura sem calendário/rendimento).
	var farm_census := _farm_plan_census(sim, buildings, cities)
	print("[g2] fazendas: %s | sem plano: %s" % [
		str(farm_census.get("by_product", {})),
		str(farm_census.get("unplanned_by_nation", {}))])

	# Cidade-sonda de cada nação da amostra (a maior): preço local por cultura.
	var probes := _probe_cities(cities)
	var probe_products := {}
	for crop in CROPS:
		probe_products[crop] = _product_index(sim, crop)

	var money_start := float(sim.get_status().get("money_total", 0.0))
	var history: Array = []          # razão comida por nação, por dia
	var violation_count := 0
	var violation_samples: Array = []
	var stalled := 0
	var day_before := int(sim.get_status().get("day", 0))
	var price_series: Array = []
	var stock_by_month: Dictionary = {}
	var hash_lines: PackedStringArray = []
	var monthly_sum: Dictionary = {}
	var monthly_days := {}
	# Acumuladores por (nação, cultura) do ano — alimentam a causa dominante.
	var acc: Dictionary = {}
	var loss_seen: Dictionary = {}   # valor do mês anterior (perda é mensal)
	_seed_accumulators(sim, acc)

	for day in _days:
		var summary: Dictionary = sim.tick_day()
		for violation in Array(summary.get("invariant_violations", [])):
			++violation_count
			if violation_samples.size() < 10:
				violation_samples.append("dia %d: %s" % [day + 1, String(violation)])
		var day_now := int(summary.get("day", 0))
		if day_now <= day_before:
			++stalled
		day_before = day_now
		var status: Dictionary = sim.get_status()
		var ratio := _food_ratio(status)
		history.append(ratio)
		if history.size() > WINDOW:
			history.pop_front()
		var month := _month_of(int(status.get("calendar_day", day + 1)))
		hash_lines.append("%d:%.2f:%d" % [day_now,
			float(status.get("money_total", 0.0)),
			int(status.get("bankruptcies", -1))])
		var agri_status: Dictionary = sim.get_agriculture_status() \
			if sim.has_method("get_agriculture_status") else {}
		if not agri_status.is_empty():
			for row in Array(agri_status.get("nations", [])):
				var nation_row: Dictionary = row
				for crop_row in Array(nation_row.get("crops", [])):
					var item: Dictionary = crop_row
					var key := String(nation_row.get("nation", "")) + "|" \
						+ String(item.get("product", ""))
					if not acc.has(key):
						acc[key] = {"demand": 0.0, "imported": 0.0, "loss": 0.0,
							"stock0": 0.0, "demand0": 0.0, "loss_seen": 0.0}
					var entry: Dictionary = acc[key]
					entry["demand"] = float(entry["demand"]) \
						+ float(item.get("demand_t_day", 0.0))
					entry["imported"] = float(entry["imported"]) \
						+ float(item.get("imported_today_t", 0.0))
					var loss_now := float(item.get("entry_loss_month_t", 0.0)) \
						+ float(item.get("storage_loss_month_t", 0.0))
					if loss_now < float(entry["loss_seen"]):
						# Virou o mês: fecha o total do mês anterior.
						entry["loss"] = float(entry["loss"]) \
							+ float(entry["loss_seen"])
					entry["loss_seen"] = loss_now
			for crop_name in CROPS:
				var stock := 0.0
				for row in Array(agri_status.get("nations", [])):
					for crop_row in Array((row as Dictionary).get("crops", [])):
						var item: Dictionary = crop_row
						if String(item.get("product", "")) == crop_name:
							stock += float(item.get("stock_t", 0.0))
				monthly_sum[crop_name] = stock
		if int(day_now) % PRICE_EVERY == 0 or day == _days - 1:
			price_series.append(_price_sample(sim, probes, probe_products,
				base_price, int(day_now), int(month)))
			stock_by_month[int(day_now)] = {
				"month": int(month),
				"crops": monthly_sum.duplicate(),
			}

	var status_final: Dictionary = sim.get_status()
	var money_end := float(status_final.get("money_total", 0.0))
	# D-099: composição das falências que restarem (produto, plano de safra e
	# fábrica) — evidência para não "esconder" o número no aceite.
	var bankrupt_composition := _bankrupt_composition(sim)
	var food := _food_gate(history)
	var agri_final: Dictionary = sim.get_agriculture_status() \
		if sim.has_method("get_agriculture_status") else {}
	for key in acc:
		acc[key]["loss"] = float(acc[key]["loss"]) + float(acc[key]["loss_seen"])
	var unplanned := _unplanned_demand(sim, cities, _planned_pairs(sim),
		probe_products)
	var nations := _nations_table(food, agri_final, status_final, acc, unplanned,
		agri_report)
	# Causa dominante e tabela das piores.
	for tag in nations:
		var row: Dictionary = nations[tag]
		row["cause"] = _dominant_cause(row)
	var worst: Array = []
	for tag in nations:
		worst.append(nations[tag])
	worst.sort_custom(func(a, b): return float(a["food_avg30"]) < float(b["food_avg30"]))
	for index in worst.size():
		(worst[index] as Dictionary)["rank"] = index + 1
	var below: Array = []
	for row in worst:
		if float(row["food_avg30"]) < GATE:
			below.append({
				"tag": String(row["nation"]),
				"food": float(row["food_avg30"]),
				"cause": String(row["cause"]),
			})
	var hemispheres := _hemispheres(price_series)
	return {
		"days": _days,
		"threads": 1,
		"final_day": int(status_final.get("day", 0)),
		"stalled_days": stalled,
		"invariant_violations": violation_count,
		"invariant_violation_samples": violation_samples,
		"bankruptcies": int(status_final.get("bankruptcies", -1)),
		"bankrupt_composition": bankrupt_composition,
		"farm_census": farm_census,
		"money_start": money_start,
		"money_end": money_end,
		"money_drift": money_end - money_start,
		"food": {
			"gate": GATE,
			"window_days": WINDOW,
			"nations": food["nations"],
			"below_count": below.size(),
			"below": below,
			"median": food["median"],
			"mean": food["mean"],
			"min": food["min"],
			"min_nation": food["min_nation"],
		},
		"worst15": worst.slice(0, 15),
		"nations": nations,
		"agriculture": {
			"report": agri_report,
			"initial_stock_t": float(agri_report.get("initial_stock_t", 0.0)),
			"totals": _agri_totals(agri_final),
			"stock_by_month": stock_by_month,
		},
		"prices": {
			"every_days": PRICE_EVERY,
			"hemispheres": hemispheres,
			"series": price_series,
		},
		"trajectory_hash": _sha("\n".join(hash_lines)),
	}


# ---------------------------------------------------------------------------
# Comida
# ---------------------------------------------------------------------------

## Média por nação (30 d) de consumido/necessidade, a partir das colunas leves
## do status (p_need/p_consumed por cidade).
func _food_ratio(status: Dictionary) -> Dictionary:
	var light: Dictionary = status.get("light", {})
	var need: PackedFloat64Array = light.get("p_need", PackedFloat64Array())
	var consumed: PackedFloat64Array = light.get("p_consumed", PackedFloat64Array())
	var cities: Array = status.get("cities", [])
	if cities.is_empty() or need.size() < cities.size() \
			or consumed.size() < cities.size():
		return {}
	var by_nation: Dictionary = {}
	for slot in cities.size():
		var tag := String((cities[slot] as Dictionary).get("nation", ""))
		if tag == "":
			continue
		if not by_nation.has(tag):
			by_nation[tag] = {"need": 0.0, "consumed": 0.0}
		by_nation[tag]["need"] = float(by_nation[tag]["need"]) + float(need[slot])
		by_nation[tag]["consumed"] = float(by_nation[tag]["consumed"]) \
			+ float(consumed[slot])
	var out: Dictionary = {}
	for tag in by_nation:
		var total := float(by_nation[tag]["need"])
		out[tag] = float(by_nation[tag]["consumed"]) / total if total > 0.0 else 1.0
	return out


func _food_gate(history: Array) -> Dictionary:
	var tags: Dictionary = {}
	for day_ratio in history:
		for tag in (day_ratio as Dictionary):
			if not tags.has(tag):
				tags[tag] = {"sum": 0.0, "days": 0, "min": 1.0}
			tags[tag]["sum"] = float(tags[tag]["sum"]) \
				+ float((day_ratio as Dictionary)[tag])
			tags[tag]["days"] = int(tags[tag]["days"]) + 1
			tags[tag]["min"] = minf(float(tags[tag]["min"]),
				float((day_ratio as Dictionary)[tag]))
	var values: Array = []
	var min_value := 1.0
	var min_tag := ""
	var mean_acc := 0.0
	for tag in tags:
		var entry: Dictionary = tags[tag]
		var value := float(entry["sum"]) / float(maxi(1, int(entry["days"])))
		values.append({"tag": String(tag), "food": value})
		mean_acc += value
		if value < min_value:
			min_value = value
			min_tag = String(tag)
	values.sort_custom(func(a, b): return float(a["food"]) < float(b["food"]))
	var median := 0.0
	if not values.is_empty():
		var middle := values.size() / 2
		median = float(values[middle]["food"]) if values.size() % 2 == 1 \
			else (float(values[middle - 1]["food"]) + float(values[middle]["food"])) / 2.0
	return {
		"nations": values.size(),
		"median": median,
		"mean": mean_acc / float(maxi(1, values.size())),
		"min": min_value,
		"min_nation": min_tag,
		"values": values,
	}


# ---------------------------------------------------------------------------
# Ficha por nação (agricultura)
# ---------------------------------------------------------------------------

## Estoque do dia 0 por (nação, cultura) — base do sinal `estoque_inicial_curto`.
func _seed_accumulators(sim: Object, acc: Dictionary) -> void:
	if not sim.has_method("get_agriculture_status"):
		return
	var status: Dictionary = sim.get_agriculture_status()
	if status.is_empty():
		return
	for row in Array(status.get("nations", [])):
		var nation_row: Dictionary = row
		for crop_row in Array(nation_row.get("crops", [])):
			var item: Dictionary = crop_row
			var key := String(nation_row.get("nation", "")) + "|" \
				+ String(item.get("product", ""))
			acc[key] = {
				"demand": 0.0, "imported": 0.0, "loss": 0.0,
				"stock0": float(item.get("stock_t", 0.0)),
				"demand0": float(item.get("demand_t_day", 0.0)),
				"loss_seen": 0.0,
			}


## Fatia da demanda alimentar que o dado deixou SEM plano, por nação (lida dos
## mercados das cidades no dia final). É o sinal de `sem_calendario`/
## `sem_rendimento`.
func _unplanned_demand(sim: Object, cities: Array, planned: Dictionary,
		products: Dictionary) -> Dictionary:
	var out: Dictionary = {}
	for city in cities:
		var row: Dictionary = city
		var city_id := String(row.get("id", ""))
		var tag := String(row.get("nation", ""))
		if city_id == "" or tag == "":
			continue
		if not out.has(tag):
			out[tag] = {"total": 0.0, "unplanned": 0.0}
		var markets: Array = sim.get_city_status(city_id).get("markets", [])
		for crop in CROPS:
			var index := int(products.get(crop, -1))
			if index < 0 or index >= markets.size():
				continue
			var demand := float((markets[index] as Dictionary).get("demand", 0.0))
			out[tag]["total"] = float(out[tag]["total"]) + demand
			if not planned.has(city_id + "|" + crop):
				out[tag]["unplanned"] = float(out[tag]["unplanned"]) + demand
	return out


## Pares (cidade|cultura) que já têm plano — das definições do snapshot.
func _planned_pairs(sim: Object) -> Dictionary:
	var out: Dictionary = {}
	var snapshot: Dictionary = sim.to_snapshot()
	for plan in Array((snapshot.get("definitions", {}) as Dictionary).get(
			"crop_plans", [])):
		var row: Dictionary = plan
		out[String(row.get("city", "")) + "|" + String(row.get("product", ""))] = true
	return out


## D-099: quem ainda falece no dia final. O núcleo conta as construções
## removidas por falência por produto e quantas eram primárias COM plano de
## safra (`get_bankruptcy_diag`, contadores de runtime, zerados no load).
## Diagnóstico: não altera o mundo nem entra no status.
func _bankrupt_composition(sim: Object) -> Dictionary:
	var diag: Dictionary = sim.get_bankruptcy_diag() \
		if sim.has_method("get_bankruptcy_diag") else {}
	return {
		"total": int(diag.get("total", 0)),
		"with_crop_plan": int(diag.get("with_crop_plan", 0)),
		"by_product": Dictionary(diag.get("by_product", {})),
	}


## D-099: censo das fazendas iniciais (a construção que o `resolve_initial_farms`
## planta em cada cidade) contra os planos declarados. Uma fazenda SEM plano é
## exatamente a que a regra de liquidez não cobre: par (cidade, cultura) fora do
## calendário/rendimento do dado.
func _farm_plan_census(sim: Object, buildings: Array, cities: Array) -> Dictionary:
	var planned := _planned_pairs(sim)
	var nation_of: Dictionary = {}
	for city in cities:
		var row: Dictionary = city
		nation_of[String(row.get("id", ""))] = String(row.get("nation", ""))
	var by_product: Dictionary = {}
	var unplanned: Dictionary = {}
	for building in buildings:
		var farm: Dictionary = building
		var product := String(farm.get("resource", ""))
		if not CROPS.has(product):
			continue
		var city_id := String(farm.get("city_id", ""))
		var entry: Dictionary = by_product.get(product,
			{"farms": 0, "planned": 0})
		entry["farms"] = int(entry["farms"]) + 1
		if planned.has(city_id + "|" + product):
			entry["planned"] = int(entry["planned"]) + 1
		else:
			var tag := String(nation_of.get(city_id, ""))
			var counts: Dictionary = unplanned.get(product, {})
			counts[tag] = int(counts.get(tag, 0)) + 1
			unplanned[product] = counts
		by_product[product] = entry
	return {"by_product": by_product, "unplanned_by_nation": unplanned}


func _nations_table(food: Dictionary, agri_status: Dictionary,
		status: Dictionary, acc: Dictionary, unplanned: Dictionary,
		report: Dictionary) -> Dictionary:
	var out: Dictionary = {}
	var avg: Dictionary = {}
	for row in Array(food.get("values", [])):
		avg[String((row as Dictionary)["tag"])] = float((row as Dictionary)["food"])
	var calendar_pairs: Dictionary = {}
	for pair in Array(report.get("skipped_calendar_pairs", [])):
		calendar_pairs[String(pair).split("|")[0]] = true
	var yield_pairs: Dictionary = {}
	for pair in Array(report.get("skipped_yield_pairs", [])):
		yield_pairs[String(pair).split("|")[0]] = true
	for tag in avg:
		out[tag] = {
			"nation": String(tag),
			"food_avg30": float(avg[tag]),
			"stock_cover_days": 0.0,
			"storage_quality": -1.0,
			"crops": {},
			"year_totals": {"demand_t": 0.0, "imported_t": 0.0,
				"supply_t": 0.0, "losses_t": 0.0},
			"cause_scores": {},
		}
	for row in Array(agri_status.get("nations", [])):
		var nation_row: Dictionary = row
		var tag := String(nation_row.get("nation", ""))
		if not out.has(tag):
			continue
		var entry: Dictionary = out[tag]
		entry["storage_quality"] = float(nation_row.get("storage_quality", -1.0))
		var min_cover := -1.0
		for crop_row in Array(nation_row.get("crops", [])):
			var crop: Dictionary = crop_row
			var name := String(crop.get("product", ""))
			var cover := float(crop.get("days_cover", -1.0))
			if cover >= 0.0 and (min_cover < 0.0 or cover < min_cover):
				min_cover = cover
			var key := tag + "|" + name
			var totals: Dictionary = acc.get(key, {})
			entry["crops"][name] = {
				"planned": true,
				"stock_t": float(crop.get("stock_t", 0.0)),
				"demand_t_day": float(crop.get("demand_t_day", 0.0)),
				"days_cover": cover,
				"production_plan_year_t": float(crop.get("production_year_t", 0.0)),
				"imported_year_t": float(totals.get("imported", 0.0)),
				"loss_year_t": float(totals.get("loss", 0.0)),
				"supply_year_t": float(crop.get("production_year_t", 0.0)),
				"skipped": "",
			}
			entry["year_totals"]["demand_t"] = float(
				entry["year_totals"]["demand_t"]) + float(totals.get("demand", 0.0))
			entry["year_totals"]["imported_t"] = float(
				entry["year_totals"]["imported_t"]) + float(totals.get("imported", 0.0))
			entry["year_totals"]["losses_t"] = float(
				entry["year_totals"]["losses_t"]) + float(totals.get("loss", 0.0))
			entry["year_totals"]["supply_t"] = float(
				entry["year_totals"]["supply_t"]) + float(crop.get("production_year_t", 0.0))
		entry["stock_cover_days"] = min_cover if min_cover >= 0.0 else 0.0
		# Estoque inicial curto: cobertura do dia 1 contra 30 d de consumo.
		var initial_cover := 0.0
		for crop_row in Array(nation_row.get("crops", [])):
			var name := String((crop_row as Dictionary).get("product", ""))
			var totals: Dictionary = acc.get(tag + "|" + name, {})
			var demand0 := float(totals.get("demand0", 0.0))
			if demand0 > 0.0:
				var days := float(totals.get("stock0", 0.0)) / demand0
				initial_cover = days if initial_cover <= 0.0 else minf(initial_cover, days)
		var missing := clampf(1.0 - float(entry["food_avg30"]), 0.0, 1.0)
		var demand_year := float(entry["year_totals"]["demand_t"])
		var unplanned_row: Dictionary = unplanned.get(tag, {})
		var unplanned_share := 0.0
		if float(unplanned_row.get("total", 0.0)) > 0.0:
			unplanned_share = float(unplanned_row.get("unplanned", 0.0)) \
				/ float(unplanned_row["total"])
		var loss_share := clampf(float(entry["year_totals"]["losses_t"])
			/ maxf(1.0, demand_year), 0.0, 0.05)
		var import_short := 0.0
		if demand_year > 0.0:
			import_short = clampf(1.0 - float(entry["year_totals"]["imported_t"])
				/ demand_year, 0.0, 1.0)
		var storage := float(entry["storage_quality"])
		var scores: Dictionary = {
			"sem_calendario": 0.0,
			"sem_rendimento": 0.0,
			"estoque_inicial_curto": clampf(1.0 - initial_cover / 30.0, 0.0, 1.0),
			"armazem_ruim": loss_share if storage >= 0.0 and storage < 0.55 else 0.0,
			"importacao_insuficiente": import_short,
			"perda_de_estoque": loss_share if storage >= 0.55 else 0.0,
		}
		if unplanned_share > 0.0:
			if calendar_pairs.has(tag):
				scores["sem_calendario"] = unplanned_share
			elif yield_pairs.has(tag):
				scores["sem_rendimento"] = unplanned_share
		entry["cause_scores"] = scores
	# Nação SEM nenhum plano (o dado não deixa cultivar): a causa ainda tem de
	# ser calculada — a demanda sem plano é o sinal, não um zero mudo.
	for tag in out:
		var entry: Dictionary = out[tag]
		if not (entry["cause_scores"] as Dictionary).is_empty():
			continue
		var unplanned_row: Dictionary = unplanned.get(tag, {})
		var share := 0.0
		if float(unplanned_row.get("total", 0.0)) > 0.0:
			share = float(unplanned_row.get("unplanned", 0.0)) \
				/ float(unplanned_row["total"])
		var scores: Dictionary = {
			"sem_calendario": share if calendar_pairs.has(tag) else 0.0,
			"sem_rendimento": share if yield_pairs.has(tag) else 0.0,
		}
		entry["cause_scores"] = scores
	return out


## Regra declarada no doc §8.2: a causa é a maior fração da cesta não atendida
## (1 − satisfação). País com ≥ 180 dias de estoque em casa é `acesso/renda` —
## há comida e a fome é de preço/renda.
func _dominant_cause(row: Dictionary) -> String:
	var missing := clampf(1.0 - float(row.get("food_avg30", 1.0)), 0.0, 1.0)
	if missing <= 0.0:
		return "acesso/renda"
	if float(row.get("stock_cover_days", 0.0)) >= 180.0:
		return "acesso/renda"
	var scores: Dictionary = row.get("cause_scores", {})
	var best := "acesso/renda"
	var best_value := 0.0
	for key in scores:
		if float(scores[key]) > best_value:
			best_value = float(scores[key])
			best = String(key)
	return best


func _agri_totals(agri_status: Dictionary) -> Dictionary:
	var stock: Dictionary = {}
	var loss: Dictionary = {}
	for row in Array(agri_status.get("nations", [])):
		for crop_row in Array((row as Dictionary).get("crops", [])):
			var crop: Dictionary = crop_row
			var name := String(crop.get("product", ""))
			stock[name] = float(stock.get(name, 0.0)) + float(crop.get("stock_t", 0.0))
			loss[name] = float(loss.get(name, 0.0)) \
				+ float(crop.get("entry_loss_month_t", 0.0)) \
				+ float(crop.get("storage_loss_month_t", 0.0))
	return {"stock_t": stock, "loss_month_t": loss}


# ---------------------------------------------------------------------------
# Preço
# ---------------------------------------------------------------------------

func _probe_cities(cities: Array) -> Dictionary:
	var best: Dictionary = {}
	for city in cities:
		var row: Dictionary = city
		var tag := String(row.get("nation", ""))
		if tag == "":
			continue
		var population := float(row.get("population", 0.0))
		if not best.has(tag) or population > float(best[tag]["population"]):
			best[tag] = {"id": String(row.get("id", "")), "population": population}
	var out: Dictionary = {}
	for tag in NORTH + SOUTH:
		if best.has(tag):
			out[tag] = String(best[tag]["id"])
	return out


func _product_index(sim: Object, product: String) -> int:
	var products: Array = sim.get_products()
	for index in products.size():
		if String(products[index]) == product:
			return index
	return -1


## Preço de cada cultura na cidade-sonda, razão ao base e ponderada pela venda.
func _price_sample(sim: Object, probes: Dictionary, products: Dictionary,
		base_price: Dictionary, day: int, month: int) -> Dictionary:
	var nations: Dictionary = {}
	for tag in probes:
		var markets: Array = sim.get_city_status(String(probes[tag])).get(
			"markets", [])
		var row: Dictionary = {}
		for crop_name in CROPS:
			var index := int(products.get(crop_name, -1))
			if index < 0 or index >= markets.size():
				continue
			var market: Dictionary = markets[index]
			var base := float(base_price.get(crop_name, 1.0))
			if base <= 0.0:
				continue
			row[crop_name] = {
				"ratio": float(market.get("price", base)) / base,
				"sold": float(market.get("sold", 0.0)),
			}
		nations[tag] = row
	return {"day": day, "month": month, "nations": nations}


func _hemispheres(series: Array) -> Dictionary:
	var out: Dictionary = {"north": {}, "south": {}}
	for side in out:
		var tags: Array = NORTH if side == "north" else SOUTH
		for crop_name in CROPS:
			var indices: Array = []
			var max_ratio := 0.0
			var min_ratio := 999.0
			var max_month := 0
			var min_month := 0
			for sample in series:
				var row: Dictionary = sample
				var total := 0.0
				var weight := 0.0
				for tag in tags:
					var nation_row: Dictionary = (row["nations"] as Dictionary).get(tag, {})
					var crop: Dictionary = nation_row.get(crop_name, {})
					if crop.is_empty():
						continue
					var sold := float(crop.get("sold", 0.0))
					total += float(crop.get("ratio", 0.0)) * sold
					weight += sold
				if weight <= 0.0:
					continue
				var ratio := total / weight
				indices.append(snappedf(ratio, 0.0001))
				if ratio > max_ratio:
					max_ratio = ratio
					max_month = int(row["month"])
				if ratio < min_ratio:
					min_ratio = ratio
					min_month = int(row["month"])
			out[side][crop_name] = {
				"indices": indices,
				"max_ratio": max_ratio, "max_month": max_month,
				"min_ratio": min_ratio, "min_month": min_month,
			}
	return out


# ---------------------------------------------------------------------------
# Apoio
# ---------------------------------------------------------------------------

func _month_of(day: int) -> int:
	var month_days := [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
	var left := posmod(day - 1, 365) + 1
	for month in 12:
		if left <= int(month_days[month]):
			return month + 1
		left -= int(month_days[month])
	return 12


func _sha(text: String) -> String:
	var context := HashingContext.new()
	context.start(HashingContext.HASH_SHA256)
	context.update(text.to_utf8_buffer())
	return context.finish().hex_encode()


func _load(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return null
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data
