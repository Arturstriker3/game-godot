extends SceneTree

## Teste de regressão da biestabilidade de liquidez (balance_review §11.3 /
## GAME_STATE §15 "fome com alimento em estoque").
##
## Reproduz a PERTURBAÇÃO que derrubava a GBR de 100% para 4%: trocar a cultura
## inicial pela de MAIOR aptidão × caloria em TODAS as nações (DEU → milho,
## trópicos → arroz), o que tira a DEU do mercado de trigo e esvazia o trigo
## tropical. Sem a correção de raiz do sinal de escassez (demanda real + renda),
## Londres entra no equilíbrio de baixo emprego (fazenda a 4%) e a satisfação da
## GBR desaba para ~4% no fim de 730 dias. Com a correção, a GBR deve ficar
## >= 70% mesmo SEM a mitigação da fazenda inicial (que só troca onde o trigo é
## inviável, aptidão < 0,30).
##
## Uso: godot --headless --path . --script res://tools/economy_bistability_test.gd

const GeographyServiceScript := preload("res://core/economy/geography_service.gd")
const EconomicAIScript := preload("res://core/economy/ai/economic_ai.gd")

const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const PORTS_PATH := "res://data/definitions/scenario/ports.json"
const BUILDINGS_PATH := "res://data/definitions/scenario/buildings.json"
const GOODS_PATH := "res://data/definitions/economy/goods.json"

const DAYS := 730
const GBR_TARGET := 0.70

var _failures: Array[String] = []


func _init() -> void:
	_run()
	if _failures.is_empty():
		print("[bistability] OK: GBR sustentada (>= %.0f%%) sob a perturbação de culturas." % (GBR_TARGET * 100.0))
		quit(0)
	else:
		print("[bistability] FALHAS (%d):" % _failures.size())
		for failure in _failures:
			print(" - " + failure)
		quit(1)


func _load_array(path: String) -> Array:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_failures.append("não abriu " + path)
		return []
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data if typeof(data) == TYPE_ARRAY else []


## Perturbação: troca a cultura inicial pela de maior aptidão × caloria em TODAS
## as nações (o comportamento pré-mitigação). DEU → milho, VEN/SUR/GUY → arroz.
func _full_swap_farms(geo: Object, buildings: Array, cities: Array) -> Array:
	var nation_of := {}
	for city in cities:
		nation_of[String(city.get("id", ""))] = String(city.get("nation", ""))
	var resolved: Array = []
	for building in buildings:
		var entry: Dictionary = building.duplicate(true)
		var tag := String(nation_of.get(String(entry.get("city_id", "")), ""))
		var code := String(geo.NATION_CODE.get(tag, ""))
		entry["resource"] = geo.best_staple_nation(code) if code != "" else "wheat"
		resolved.append(entry)
	return resolved


func _run() -> void:
	var cities := _load_array(CITIES_PATH)
	var ports := _load_array(PORTS_PATH)
	var buildings := _load_array(BUILDINGS_PATH)
	var goods := _load_array(GOODS_PATH)
	if cities.is_empty() or buildings.is_empty():
		return

	var sim = EconomySim.new()
	var geo = GeographyServiceScript.new()
	# Perturbação: troca total (DEU → milho, trópicos → arroz), SEM a mitigação.
	sim.initialize_demo_world(cities, ports, _full_swap_farms(geo, buildings, cities))
	sim.demo_set_goods(goods)
	geo.apply_to_sim(sim, _cities_with_center(cities))

	var ai = EconomicAIScript.new()
	ai.setup(sim, geo)

	print("day\tGBR_farm_workers\tGBR_farm_util\tGBR_budget\tGBR_sat")
	for day in DAYS:
		var status: Dictionary = sim.tick_demo()
		ai.run(int(status.get("day", 0)))
		if (day + 1) % 30 == 0:
			_trace_london(day + 1, sim)

	var final: Dictionary = sim.get_demo_status()
	var london := _city(final, "GBR-LDN")
	if london.is_empty():
		_failures.append("GBR-LDN não encontrada no status final")
		return

	var satisfaction := float(london["pop"]["satisfaction"])
	print("[bistability] GBR satisfação final = %.1f%% (meta >= %.0f%%)" % [
		satisfaction * 100.0, GBR_TARGET * 100.0])

	var violations: Array = final.get("invariant_violations", [])
	if violations.size() > 0:
		_failures.append("invariante violado: %s" % String(violations[0]))
	if satisfaction < GBR_TARGET:
		_failures.append("GBR colapsou: satisfação %.1f%% < %.0f%%" % [
			satisfaction * 100.0, GBR_TARGET * 100.0])


func _city(status: Dictionary, city_id: String) -> Dictionary:
	for city in status.get("cities", []):
		if String(city["id"]) == city_id:
			return city
	return {}


func _trace_london(day: int, sim: Object) -> void:
	var status: Dictionary = sim.get_demo_status()
	var london := _city(status, "GBR-LDN")
	if london.is_empty():
		return
	var workers := 0.0
	var util_sum := 0.0
	var cap_sum := 0.0
	for building in london.get("buildings", []):
		var output := int(building.get("output_product", -1))
		var resource := String(building.get("resource", ""))
		if output < 0:
			continue
		# Só fazendas de alimento (trigo/arroz/milho/batata), não minas de ferro.
		var is_food := resource in ["wheat", "rice", "maize", "potato"]
		if not is_food:
			continue
		workers += float(building.get("workers", 0.0))
		util_sum += float(building.get("utilization", 0.0)) * float(building.get("capacity", 0.0))
		cap_sum += float(building.get("capacity", 0.0))
	var util := util_sum / cap_sum if cap_sum > 0.0 else 0.0
	print("%d\t%.0f\t%.2f\t%.0f\t%.3f" % [
		day, workers, util,
		float(london.get("government_budget", 0.0)),
		float(london["pop"]["satisfaction"]),
	])


func _cities_with_center(cities: Array) -> Array:
	var result: Array = []
	for city in cities:
		result.append({
			"id": String(city.get("id", "")),
			"nation": String(city.get("nation", "")),
			"center": Vector2(float(city.get("x", 0.0)), float(city.get("y", 0.0))),
		})
	return result
