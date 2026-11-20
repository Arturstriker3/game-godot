extends SceneTree

## C1 (D-102) — CONTABILIDADE POR BALANÇO.
##
## Prova quatro coisas:
##   1. IDENTIDADE FINANCEIRA diária: a soma das posições financeiras do mundo
##      (famílias + firmas + governo + exterior) menos o passivo da autoridade
##      monetária é 0 em TODO dia — e a enumeração de caixa do balanço reproduz
##      o `money_total` canônico (checagem cruzada).
##   2. CONSERVAÇÃO de caixa continua valendo sem banco (C2).
##   3. PATRIMÔNIO coerente: o patrimônio das famílias é exatamente a soma da
##      terra/moradia/anel da B6 gravada no save, e o estoque do governo (G2)
##      é o dos armazéns públicos — por nação e no mundo.
##   4. SAVE v27: round-trip bit-exato, continuação idêntica e migração
##      v25 → v27 (o salário atrasado nasce 0; nada é inventado para trás).
##
## Roda pelo runner: powershell -File tools/run_tests.ps1 -List c1_balance_test

const GeographyServiceScript := preload("res://core/economy/geography_service.gd")
const LandPropertyScript := preload("res://core/economy/land_property.gd")
const AgricultureServiceScript := preload("res://core/economy/agriculture_service.gd")
const StorageServiceScript := preload("res://core/economy/storage_service.gd")
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

const SAVE_VERSION := 29
const EPS := 1e-3

var _failures: Array[String] = []


func _init() -> void:
	_test_fixture()
	_test_world()
	if _failures.is_empty():
		print("[c1] OK: identidade financeira diária, conservação, B6/G2, save v27.")
		quit(0)
	else:
		print("[c1] FALHAS (%d):" % _failures.size())
		for failure: String in _failures:
			print(" - " + failure)
		quit(1)


func _fail(message: String) -> void:
	_failures.append(message)


func _check(cond: bool, message: String) -> void:
	if not cond:
		_fail(message)


func _close(a: float, b: float, tolerance: float) -> bool:
	return absf(a - b) <= tolerance


func _rel_close(a: float, b: float, rel: float = 1e-9) -> bool:
	return absf(a - b) <= maxf(EPS, rel * maxf(absf(a), absf(b)))


func _json(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("não abriu " + path)
		return null
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data


func _load_chains(sim: Object) -> void:
	sim.set_goods(_json(GOODS_PATH))
	sim.set_recipes(_json(RECIPES_PATH))
	sim.set_needs(_json(NEEDS_PATH))
	if sim.has_method("set_services"):
		sim.set_services(_json(SERVICES_PATH))
	if sim.has_method("set_currencies"):
		sim.set_currencies(_json(CURRENCIES_PATH))
	if sim.has_method("set_sector_employment"):
		sim.set_sector_employment(_json(SECTOR_EMPLOYMENT_PATH))
	if sim.has_method("set_age_structure"):
		sim.set_age_structure(_json(AGE_STRUCTURE_PATH))


## Identidade do dia a partir do status publicado (é o que o jogo lê).
func _check_day_identity(sim: Object, label: String) -> void:
	var status: Dictionary = sim.get_status()
	var balance: Dictionary = status.get("balance", {})
	_check(not balance.is_empty(), "%s: status sem o bloco `balance`" % label)
	var financial_sum := float(balance.get("financial_sum", 1e9))
	var cash_residual := float(balance.get("cash_residual", 1e9))
	_check(absf(financial_sum) <= EPS,
		"%s: soma das posições financeiras != 0 (%.6f)" % [label, financial_sum])
	_check(absf(cash_residual) <= maxf(EPS, 1e-9
			* absf(float(balance.get("money_total", 0.0)))),
		"%s: enumeração de caixa diverge do money_total (%.6f)"
			% [label, cash_residual])
	_check(bool(balance.get("ok", false)), "%s: balance.ok falso" % label)


func _test_fixture() -> void:
	# --- AGRIA/PORTUS: cenário de validação, 40 dias ----------------------
	var sim = EconomySim.new()
	_load_chains(sim)
	sim.initialize_scenario()
	var money0 := float(sim.get_status().get("money_total", 0.0))
	_check(money0 > 0.0, "(fixture) money_total deveria ser > 0")
	for day in 40:
		sim.tick_day()
		_check_day_identity(sim, "fixture dia %d" % (day + 1))
		var money := float(sim.get_status().get("money_total", 0.0))
		_check(_close(money, money0, 1e-3),
			"(fixture) dinheiro não conservado no dia %d: %.6f != %.6f"
				% [day + 1, money, money0])

	var full: Dictionary = sim.get_balance_status()
	_check(bool((full.get("invariant", {}) as Dictionary).get("ok", false)),
		"(fixture) get_balance_status().invariant.ok falso")
	var world: Dictionary = full.get("world", {})
	for agent: String in ["families", "firms", "government", "external"]:
		_check(world.has(agent), "(fixture) balanço sem o agente '%s'" % agent)
	# Patrimônio = ativos − passivos, recalculado à mão.
	var families: Dictionary = world.get("families", {})
	var expected := float(families.get("cash", 0.0))
	expected += float(families.get("receivables", 0.0))
	expected += float(families.get("inventory", 0.0))
	expected += float(families.get("fixed_capital", 0.0))
	expected += float(families.get("property", 0.0))
	expected -= float(families.get("payables", 0.0))
	expected -= float(families.get("debt", 0.0))
	_check(_rel_close(float(families.get("net_worth", -1.0)), expected, 1e-9),
		"(fixture) net_worth das famílias incoerente: %.6f != %.6f"
			% [float(families.get("net_worth", -1.0)), expected])
	_check(float((world.get("firms", {}) as Dictionary)
		.get("fixed_capital", 0.0)) > 0.0,
		"(fixture) capital fixo das firmas deveria ser > 0 (as fazendas)")
	# Atrasado fecha dos dois lados (passivo da firma = ativo das famílias).
	var invariant: Dictionary = full.get("invariant", {})
	_check(_close(float(invariant.get("receivables", 0.0)),
		float(invariant.get("payables", 0.0)), EPS),
		"(fixture) atrasado a receber != a pagar")

	# --- Save v27: round-trip bit-exato e continuação idêntica -----------
	var save: Dictionary = sim.to_dict()
	_check(int(save.get("version", 0)) == SAVE_VERSION,
		"(fixture) save version %s != %d" % [str(save.get("version", 0)),
			SAVE_VERSION])
	var copy = EconomySim.new()
	_load_chains(copy)
	copy.from_dict(save)
	var copy_save: Dictionary = copy.to_dict()
	_check(JSON.stringify(copy_save, "", true, true)
		== JSON.stringify(save, "", true, true),
		"(fixture) round-trip do save v27 não é bit-exato")
	for day in 3:
		sim.tick_day()
		copy.tick_day()
	_check(JSON.stringify(sim.to_dict(), "", true, true)
		== JSON.stringify(copy.to_dict(), "", true, true),
		"(fixture) continuação após load divergiu do caminho direto")
	_check_day_identity(sim, "fixture pós round-trip")

	# --- Migração v25 → v27 ---------------------------------------------
	var old: Dictionary = save.duplicate(true)
	old["version"] = 25
	for city: Dictionary in old.get("cities", []):
		(city.get("pop", {}) as Dictionary).erase("arrears")
		city.erase("service_wage_arrears")
		city.erase("rural_wage_arrears")
		var region: Dictionary = city.get("region", {})
		if not region.is_empty():
			region.erase("arrears")
	for building: Dictionary in old.get("buildings", []):
		building.erase("wage_arrears")
	var migrated = EconomySim.new()
	_load_chains(migrated)
	migrated.from_dict(old)
	var migrated_save: Dictionary = migrated.to_dict()
	_check(int(migrated_save.get("version", 0)) == SAVE_VERSION,
		"(migração) v25 deveria sair como v27 (saiu %s)"
			% str(migrated_save.get("version", 0)))
	var arrears_total := 0.0
	for city: Dictionary in migrated_save.get("cities", []):
		arrears_total += float((city.get("pop", {}) as Dictionary)
			.get("arrears", 0.0))
		arrears_total += float(city.get("service_wage_arrears", 0.0))
		arrears_total += float(city.get("rural_wage_arrears", 0.0))
		var region: Dictionary = city.get("region", {})
		arrears_total += float(region.get("arrears", 0.0))
	for building: Dictionary in migrated_save.get("buildings", []):
		arrears_total += float(building.get("wage_arrears", 0.0))
	_check(absf(arrears_total) <= EPS,
		"(migração) v25 não pode inventar atrasado (%.6f)" % arrears_total)
	migrated.tick_day()
	_check_day_identity(migrated, "migração v25 -> v27")


func _make_world() -> Object:
	var sim = EconomySim.new()
	_load_chains(sim)
	sim.set_informality(_json(INFORMALITY_PATH))
	sim.set_health_access(_json(HEALTH_ACCESS_PATH))
	sim.set_sim_threads(1)
	var cities: Array = _json(CITIES_PATH)
	var ports: Array = _json(PORTS_PATH)
	var buildings: Array = _json(BUILDINGS_PATH)
	RegionDataScript.attach(cities)
	var geo := GeographyServiceScript.new()
	geo.load_data()
	buildings = geo.resolve_initial_farms(buildings, cities)
	if geo.has_method("resolve_initial_mines"):
		buildings = geo.resolve_initial_mines(buildings, cities)
	sim.initialize_world(cities, ports, buildings)
	GeographyServiceScript.apply_initial_infra(sim)
	if sim.has_method("set_urban"):
		sim.set_urban(_json(URBAN_LAND_PATH), _json(HOUSING_CITY_PATH),
			_json(HOUSING_PATH), _json(MUNICIPAL_FINANCE_PATH))
	var centers: Array = []
	for city: Dictionary in cities:
		centers.append({
			"id": String(city.get("id", "")),
			"nation": String(city.get("nation", "")),
			"center": Vector2(float(city.get("x", 0.0)),
				float(city.get("y", 0.0))),
		})
	geo.apply_to_sim(sim, centers)
	_check(LandPropertyScript.apply(sim) > 0,
		"(mundo) LandProperty.apply deveria semear as regiões")
	if sim.has_method("set_crop_plan"):
		var agri := AgricultureServiceScript.new()
		agri.apply_to_sim(sim, cities, geo, {
			"calendar_nations": agri.calendar_nations(cities),
			"initial_stock": true,
			"farms": AgricultureServiceScript.farms_of(buildings),
		})
	if sim.has_method("set_storage_capacity"):
		var storage := StorageServiceScript.new()
		storage.apply_to_sim(sim, cities, buildings,
			{"only_missing": true, "initial_stock": true})
	return sim


func _test_world() -> void:
	# --- Mundo real: 30 dias, com B6 e G2 aplicados ---------------------
	# A B6 fecha no calendário MENSAL de cada cidade — 30 d cobre a primeira
	# rodada de patrimônio de todas.
	var sim = _make_world()
	if sim == null:
		return
	for day in 30:
		sim.tick_day()
		if day % 10 == 9 or day == 29:
			_check_day_identity(sim, "mundo dia %d" % (day + 1))

	var full: Dictionary = sim.get_balance_status()
	_check(bool((full.get("invariant", {}) as Dictionary).get("ok", false)),
		"(mundo) invariant.ok falso")
	var world: Dictionary = full.get("world", {})
	var invariant: Dictionary = full.get("invariant", {})
	var receivables := float(invariant.get("receivables", 0.0))
	var payables := float(invariant.get("payables", 0.0))
	_check(receivables >= 0.0 and payables >= 0.0,
		"(mundo) atrasado negativo")
	_check(_rel_close(receivables, payables, 1e-9),
		"(mundo) atrasado a receber %.3f != a pagar %.3f"
			% [receivables, payables])
	_check(receivables > 0.0,
		"(mundo) esperava salário atrasado > 0 no mundo real (caixa "
		+ "insuficiente no dia 1); veio 0")
	# O atrasado é COBERTO pelo ativo das firmas (estoque + capital fixo):
	# a folha do ciclo é maior que o caixa, não insolvência.
	var firms_sheet: Dictionary = world.get("firms", {})
	_check(float(firms_sheet.get("net_worth", 0.0)) > payables,
		"(mundo) atrasado %.0f não é coberto pelo PL das firmas %.0f"
			% [payables, float(firms_sheet.get("net_worth", 0.0))])

	# --- B6: patrimônio das famílias = terra/moradia/anel do save -------
	var save: Dictionary = sim.to_dict()
	var nation_property := {}
	var nation_inventory_gov := {}
	var property_total := 0.0
	for city: Dictionary in save.get("cities", []):
		var nation := String(city.get("nation", ""))
		if not nation_property.has(nation):
			nation_property[nation] = 0.0
		var region: Dictionary = city.get("region", {})
		var prop: Dictionary = region.get("property", {})
		if prop.is_empty():
			continue
		var value := 0.0
		for key: String in ["land_value", "home_value", "ring_value"]:
			for item: Variant in prop.get(key, []):
				value += float(item)
		property_total += value
		nation_property[nation] = float(nation_property[nation]) + value
	var families_property := float((world.get("families", {}) as Dictionary)
		.get("property", 0.0))
	_check(families_property > 0.0,
		"(B6) patrimônio das famílias deveria ser > 0 no dia 30")
	_check(_rel_close(families_property, property_total, 1e-9),
		"(B6) patrimônio das famílias %.3f != soma das regiões %.3f"
			% [families_property, property_total])

	# --- G2: estoque do governo = armazéns públicos (leitura do save) ---
	var price_by_city := {}
	for city: Dictionary in save.get("cities", []):
		var prices := {}
		var goods: Array = save.get("products", [])
		var markets: Array = city.get("markets", [])
		for index in mini(markets.size(), goods.size()):
			prices[String(goods[index])] = float(
				(markets[index] as Dictionary).get("price", 0.0))
		price_by_city[String(city.get("id", ""))] = prices
	var gov_value := 0.0
	var private_value := 0.0
	var storage: Dictionary = save.get("storage", {})
	for site: Dictionary in storage.get("sites", []):
		var owner := String(site.get("owner", ""))
		var city_id := String(site.get("city", ""))
		var prices: Dictionary = price_by_city.get(city_id, {})
		for item: Dictionary in site.get("stock", []):
			var product := String(item.get("product", ""))
			var tons := float(item.get("tons", 0.0))
			var value := tons * float(prices.get(product, 0.0))
			if owner == "government":
				gov_value += value
			else:
				private_value += value
	_check(gov_value > 0.0, "(G2) reserva pública sem valor no balanço")
	_check(_rel_close(float((world.get("government", {}) as Dictionary)
		.get("inventory", 0.0)), gov_value, 1e-6),
		"(G2) estoque do governo %.3f != armazéns públicos %.3f"
			% [float((world.get("government", {}) as Dictionary)
				.get("inventory", 0.0)), gov_value])
	_check(private_value > 0.0, "(G2) estoque privado sem valor no balanço")

	# --- Por nação: aditividade de famílias e governo -------------------
	var by_nation: Dictionary = full.get("by_nation", {})
	_check(by_nation.size() > 100,
		"(nação) esperava > 100 nações, veio %d" % by_nation.size())
	var families_sum := 0.0
	var government_sum := 0.0
	var firms_cash_sum := 0.0
	for nation: String in by_nation:
		var row: Dictionary = by_nation[nation]
		var fam: Dictionary = row.get("families", {})
		var gov: Dictionary = row.get("government", {})
		families_sum += float(fam.get("net_worth", 0.0))
		government_sum += float(gov.get("net_worth", 0.0))
		firms_cash_sum += float((row.get("firms", {}) as Dictionary)
			.get("cash", 0.0))
		var expected_property := float(nation_property.get(nation, 0.0))
		_check(_rel_close(float(fam.get("property", 0.0)),
			expected_property, 1e-9),
			"(B6) %s: patrimônio %.3f != regiões %.3f"
				% [nation, float(fam.get("property", 0.0)),
					expected_property])
		var external: Dictionary = row.get("external", {})
		_check(not bool(external.get("decomposed", true)),
			"(nação) setor externo não pode ser rateado por nação")
	_check(_rel_close(families_sum,
		float((world.get("families", {}) as Dictionary)
			.get("net_worth", 0.0)), 1e-9),
		"(nação) soma do patrimônio das famílias por nação != mundo")
	_check(_rel_close(government_sum,
		float((world.get("government", {}) as Dictionary)
			.get("net_worth", 0.0)), 1e-9),
		"(nação) soma do governo por nação != mundo")
	# Caixa das firmas por nação não inclui grupos/fundos sem nação atribuível.
	_check(float((world.get("firms", {}) as Dictionary).get("cash", 0.0))
		>= firms_cash_sum - EPS,
		"(nação) caixa das firmas por nação excede o mundo")

	# --- Liquidez × solvência: as três classes somam o censo ------------
	var solvency: Dictionary = world.get("firms_solvency", {})
	var count := int(solvency.get("count", 0))
	var parts := int(solvency.get("liquid", 0)) \
		+ int(solvency.get("illiquid", 0)) \
		+ int(solvency.get("insolvent", 0))
	_check(count > 0, "(solvência) censo de plantas vazio")
	_check(count == parts, "(solvência) classes %d != plantas %d"
		% [parts, count])
