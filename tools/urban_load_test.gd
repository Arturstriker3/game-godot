extends SceneTree

## Teste do pacote W0 da §19 (solo urbano, moradia e prefeitura).
##
## Carrega os quatro JSON pelo mesmo caminho do jogo (initialize_world +
## set_urban) e confere, numa amostra de metrópoles e de uma cidade
## pequena, o que a spec exige: área por zona, unidades por padrão, posse,
## aluguel/preço em XGS > 0 e prefeitura com taxas. Depois faz ida-e-volta de
## save/load e exige estado urbano idêntico.
##
##   godot --headless --path . --script res://tools/urban_load_test.gd

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

## Amostra da spec: duas metrópoles grandes, uma metrópole de país pobre e uma
## cidade pequena (Weipa, 3,8 mil hab., gabarito baixo).
const SAMPLE_CITIES: Array[String] = ["BRA-SAO", "JPN-TOK", "NGA-LAG", "AUS-WEI"]
const TOLERANCE := 1.0e-6

var _failures: Array[String] = []


func _init() -> void:
	_run()
	if _failures.is_empty():
		print("[urban] OK: amostra, unidades, posse, aluguel/preço e prefeitura.")
		quit(0)
	else:
		for failure in _failures:
			print("[urban] FALHA: " + failure)
		quit(1)


func _fail(msg: String) -> void:
	_failures.append(msg)


func _load_json(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("nao abriu " + path)
		return null
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data


func _load_dict(path: String) -> Dictionary:
	var data: Variant = _load_json(path)
	if typeof(data) != TYPE_DICTIONARY:
		_fail(path + " nao e um objeto JSON")
		return {}
	return data


func _load_array(path: String) -> Array:
	var data: Variant = _load_json(path)
	if typeof(data) != TYPE_ARRAY:
		_fail(path + " nao e um array JSON")
		return []
	return data


func _close(a: float, b: float, tolerance: float = TOLERANCE) -> bool:
	return absf(a - b) <= tolerance * maxf(1.0, maxf(absf(a), absf(b)))


func _sum(values: Array) -> float:
	var total := 0.0
	for value in values:
		total += float(value)
	return total


func _make_world() -> Object:
	var sim = EconomySim.new()
	sim.set_goods(_load_array(GOODS_PATH))
	sim.set_recipes(_load_array(RECIPES_PATH))
	sim.set_needs(_load_dict(NEEDS_PATH))
	if sim.has_method("set_services"):
		sim.set_services(_load_dict(SERVICES_PATH))
	if sim.has_method("set_currencies"):
		sim.set_currencies(_load_dict(CURRENCIES_PATH))
	if sim.has_method("set_sector_employment"):
		sim.set_sector_employment(_load_dict(SECTOR_EMPLOYMENT_PATH))
	var cities := _load_array(CITIES_PATH)
	if cities.is_empty():
		_fail("cities.json vazio")
		return sim
	sim.initialize_world(cities, _load_array(PORTS_PATH), _load_array(BUILDINGS_PATH))
	if not sim.has_method("set_urban"):
		_fail("o binario GDExtension nao tem set_urban (rode o build)")
		return sim
	sim.set_urban(
		_load_dict(URBAN_LAND_PATH),
		_load_dict(HOUSING_CITY_PATH),
		_load_dict(HOUSING_PATH),
		_load_dict(MUNICIPAL_FINANCE_PATH))
	return sim


func _run() -> void:
	var sim := _make_world()
	if not _failures.is_empty():
		return
	var land_data: Dictionary = _load_dict(URBAN_LAND_PATH).get("cities", {})
	var housing_data: Dictionary = _load_dict(HOUSING_CITY_PATH).get("cities", {})
	if land_data.size() != 849 or housing_data.size() != 849:
		_fail("cobertura: urban_land=%d housing_city=%d (esperado 849)" % [
			land_data.size(), housing_data.size()])

	var total_units := 0.0
	var total_area := 0.0
	for city_id in SAMPLE_CITIES:
		var urban: Dictionary = sim.get_urban_city(city_id)
		if urban.is_empty():
			_fail("%s sem estado urbano" % city_id)
			continue
		var source_land: Dictionary = land_data.get(city_id, {})
		var source_housing: Dictionary = housing_data.get(city_id, {})
		var area: Array = urban.get("area_km2", [])
		var units: Array = urban.get("units", [])
		var tenure: Array = urban.get("tenure", [])
		var owner: Array = urban.get("owner_share", [])
		var rent: Array = urban.get("rent", [])
		var price: Array = urban.get("price", [])
		var gov: Dictionary = urban.get("gov", {})
		total_area += _sum(area)

		# Área por zona = built_up × zoning_split da fonte.
		if not _close(_sum(area), float(source_land.get("built_up_km2", -1.0))):
			_fail("%s area soma %.6f != built_up %.6f" % [
				city_id, _sum(area), float(source_land.get("built_up_km2", 0.0))])
		for zone in 3:
			if float(area[zone]) <= 0.0:
				_fail("%s zona %d com area zero" % [city_id, zone])
		if float(urban.get("reserve_km2", 0.0)) <= 0.0:
			_fail("%s sem reserva expansivel" % city_id)

		# Unidades por padrão = housing_stock_by_tier da fonte.
		var tier_totals := [0.0, 0.0, 0.0]
		for density in 3:
			var row: Array = units[density]
			for tier in 3:
				tier_totals[tier] += float(row[tier])
				total_units += float(row[tier])
		var source_by_tier: Dictionary = source_housing.get("housing_stock_by_tier", {})
		var tier_keys := ["popular", "medium", "high"]
		for tier in 3:
			if not _close(tier_totals[tier], float(source_by_tier.get(tier_keys[tier], -1.0))):
				_fail("%s tier %s: unidades %.3f != fonte %.3f" % [
					city_id, tier_keys[tier], tier_totals[tier],
					float(source_by_tier.get(tier_keys[tier], 0.0))])
			if float(rent[tier]) <= 0.0:
				_fail("%s tier %s sem aluguel em XGS" % [city_id, tier_keys[tier]])
			if float(price[tier]) <= 0.0:
				_fail("%s tier %s sem preco em XGS" % [city_id, tier_keys[tier]])
			if float(rent[tier]) > 0.0:
				# Fonte tem price-to-rent de 20 anos e cap rate fixo 0,05:
				# preço = aluguel × 12 ÷ 0,05 = aluguel × 240.
				var ratio := float(price[tier]) / float(rent[tier])
				if absf(ratio - 240.0) > 1.0:
					_fail("%s tier %s: price/rent %.2f != 240" % [
						city_id, tier_keys[tier], ratio])

		# Posse por estrato e donos somam 1.
		for tier in 3:
			if not _close(_sum(tenure[tier]), 1.0, 1.0e-9):
				_fail("%s tenure tier %d soma %.9f" % [
					city_id, tier, _sum(tenure[tier])])
		if not _close(_sum(owner), 1.0, 1.0e-9):
			_fail("%s owner_share soma %.9f" % [city_id, _sum(owner)])

		# Prefeitura: taxas positivas, repasse em 0..1.
		if float(gov.get("iptu_rate", 0.0)) <= 0.0:
			_fail("%s sem iptu_rate" % city_id)
		if float(gov.get("fee_rate", 0.0)) <= 0.0:
			_fail("%s sem fee_rate" % city_id)
		var transfer := float(gov.get("transfer_share", -1.0))
		if transfer <= 0.0 or transfer > 1.0:
			_fail("%s transfer_share fora de 0..1: %.3f" % [city_id, transfer])

		print("[urban] %-8s area=%.2f km2 reserva=%.1f unidades=%.0f informal=%.0f "
			% [city_id, _sum(area), float(urban.get("reserve_km2", 0.0)),
				tier_totals[0] + tier_totals[1] + tier_totals[2],
				float(urban.get("informal_units", 0.0))]
			+ "aluguel(P/M/A)=%.1f/%.1f/%.1f XGS iptu=%.3f repasse=%.2f" % [
				rent[0], rent[1], rent[2], float(gov.get("iptu_rate", 0.0)), transfer])

	if total_units <= 0.0:
		_fail("nenhuma unidade carregada no mundo inteiro")
	if total_area <= 0.0:
		_fail("nenhuma area urbana carregada no mundo inteiro")

	# Save v14: a ida-e-volta restaura exatamente o estado urbano da amostra.
	var before: Dictionary = {}
	for city_id in SAMPLE_CITIES:
		before[city_id] = JSON.stringify(sim.get_urban_city(city_id), "", true, true)
	var snapshot: Dictionary = sim.to_dict()
	if int(snapshot.get("version", 0)) != 26:
		_fail("save version %s != 26" % str(snapshot.get("version")))
	var restored = EconomySim.new()
	restored.set_goods(_load_array(GOODS_PATH))
	restored.set_recipes(_load_array(RECIPES_PATH))
	restored.set_needs(_load_dict(NEEDS_PATH))
	if restored.has_method("set_services"):
		restored.set_services(_load_dict(SERVICES_PATH))
	if restored.has_method("set_currencies"):
		restored.set_currencies(_load_dict(CURRENCIES_PATH))
	restored.from_dict(snapshot)
	for city_id in SAMPLE_CITIES:
		var after: String = JSON.stringify(restored.get_urban_city(city_id), "", true, true)
		if after != String(before[city_id]):
			_fail("%s: save/load urbano divergiu" % city_id)
	print("[urban] save/load v14 da amostra: %s" % (
		"identico" if _failures.is_empty() else "com falhas"))

	# Migração v12→v13: um save SEM o bloco `urban` carrega sem estado urbano
	# (vetores vazios), não quebra o tick e o próximo save já sai como v14.
	var v12: Dictionary = snapshot.duplicate(true)
	v12.erase("urban")
	v12["version"] = 12
	var legacy = EconomySim.new()
	legacy.set_goods(_load_array(GOODS_PATH))
	legacy.set_recipes(_load_array(RECIPES_PATH))
	legacy.set_needs(_load_dict(NEEDS_PATH))
	if legacy.has_method("set_services"):
		legacy.set_services(_load_dict(SERVICES_PATH))
	if legacy.has_method("set_currencies"):
		legacy.set_currencies(_load_dict(CURRENCIES_PATH))
	legacy.from_dict(v12)
	if not Dictionary(legacy.get_urban_city("BRA-SAO")).is_empty():
		_fail("v12->v14: save antigo nao deveria ter estado urbano")
	if int(legacy.to_dict().get("version", 0)) != 26:
		_fail("v12->v26: save migrado nao saiu como v26")
	print("[urban] migracao v12->v26: sem estado urbano, schema 26.")

