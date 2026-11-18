extends SceneTree

## Preço mundial dos minerais no dia N do mundo real (IA + urbano), como fração
## do `base_price` do catálogo — o critério "sem colapso de preço de minério" da
## D-086/D-087 (`docs/economy/mine_stages.md` §7.4).
##
##   godot --headless --path . --script res://tools/ore_price_365.gd -- [--days=365]
##
## - Monta o mundo como `tools/world_equivalence.gd` (geografia real com
##   `resolve_initial_farms` + `resolve_initial_mines`, camada urbana, IA de
##   grupos ligada, threads = 1) e roda `--days` dias conferindo
##   `invariant_violations` a cada dia.
## - Lê os preços do bloco leve (`status.light.m_price`, arrays planos
##   cidade×produto) e pondera pela quantidade vendida (`m_sold`) das 834
##   cidades; sem peso, cai na média simples das cidades com preço > 0.
## - A razão usa o `base_price` de `goods.json` — a MESMA âncora do teto de 3×
##   do núcleo, então razão 3,000 significa "no teto".
## - Saída: uma linha `[ore] <mineral>: ...` por mineral e `[ore] dia=N ...`
##   com falências e dinheiro; exit 0 ok, 1 violação de invariante.
## - Diagnóstico: não entra em nenhuma bateria.

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

## Minerais rastreados pela D-086 (petróleo, gás, carvão, ferro, cobre, bauxita).
const TARGETS := ["oil", "gas", "coal", "iron", "copper", "bauxite"]

var _days := 365


func _init() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--days="):
			_days = int(arg.split("=", true, 1)[1])
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
		base[String(Dictionary(good).get("id", ""))] = float(
			Dictionary(good).get("base_price", 0.0))

	var status: Dictionary = sim.get_status()
	for day in _days:
		status = sim.tick_day()
		var violations: Array = status.get("invariant_violations", [])
		if violations.size() > 0:
			print("[ore] FALHA dia %d: %s" % [day + 1, String(violations[0])])
			quit(1)
			return
	_report(status, base)
	quit(0)


func _report(status: Dictionary, base: Dictionary) -> void:
	var light: Dictionary = status.get("light", {})
	var prices: PackedFloat64Array = light.get("m_price", PackedFloat64Array())
	var sold: PackedFloat64Array = light.get("m_sold", PackedFloat64Array())
	var per_city := int(light.get("product_count", 0))
	var products: Array = status.get("products", [])
	var city_count: int = status["cities"].size()
	print("[ore] dia=%d cidades=%d produtos=%d falencias=%d dinheiro=%.4f" % [
		int(status.get("day", _days)), city_count, products.size(),
		int(status.get("bankruptcies", 0)), float(status.get("money_total", 0.0))])
	for target in TARGETS:
		var index := products.find(target)
		if index < 0:
			print("[ore] %s: produto ausente" % target)
			continue
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
		var ratio := mean / base_price if base_price > 0.0 else 0.0
		print("[ore] %s: preco medio=%.4f base=%.4f razao=%.3f cidades_com_preco=%d peso=%.1f" % [
			target, mean, base_price, ratio, priced, weight])


func _load_array(path: String) -> Array:
	var parsed: Variant = _load(path)
	return parsed if typeof(parsed) == TYPE_ARRAY else []


func _load_dict(path: String) -> Dictionary:
	var parsed: Variant = _load(path)
	return parsed if typeof(parsed) == TYPE_DICTIONARY else {}


func _load(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		push_error("ore_price_365: não abriu " + path)
		return null
	var text := file.get_as_text()
	file.close()
	return JSON.parse_string(text)
