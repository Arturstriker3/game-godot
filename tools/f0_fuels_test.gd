extends SceneTree

## F0 — combustíveis por grau (gasoline / diesel / jet_fuel).
##
## O bem genérico `fuel` saiu da cadeia. Este teste guarda o contrato da F0:
##   (1) a refinaria gasoline_a produz as três saídas nas frações do dado
##       (EIA 2024: 45/29/9) e a conservação da receita (83% do petróleo vira
##       produto; 17% é a perda declarada em co_loss);
##   (2) a frota queima o grau de cada modal: caminhão/navio precisam de diesel,
##       avião precisa de querosene — sem aviões, jet não é exigido;
##   (3) a usina a óleo queima diesel (não petróleo cru);
##   (4) o consumo doméstico/militar está mapeado no dado (gasolina na POP,
##       diesel no exército e na frota, jet na aviação);
##   (5) um save v38 (com `fuel`) migra para v39: estoque do mercado e do
##       armazém rateado nas três frações, frota rateada pelo consumo de cada
##       modal e receita fuel_a..fuel_d resolvida para a receita que a substitui.
##
##   godot --headless --path . --script res://tools/f0_fuels_test.gd

const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"
const POWER_PARAMS_PATH := "res://data/definitions/energy/power_params.json"

const EPS := 1e-6
const REFINERY_CAPACITY := 500.0
const REFINERY_DAYS := 3
const LEGACY_FUEL_STOCK := 83.0
const LEGACY_FLEET_FUEL := 1000.0
# Frações declaradas em recipes.json (gasoline_a: 45/29/9 de 83 t úteis).
const GASOLINE_SHARE := 45.0 / 83.0
const DIESEL_SHARE := 29.0 / 83.0
const JET_SHARE := 9.0 / 83.0
# sim_constants.h (kFleetFuelPerTruckDay/ShipDay/PlaneDay).
const FLEET_TRUCK_DAY := 0.12
const FLEET_SHIP_DAY := 0.80
const FLEET_PLANE_DAY := 1.50

var _failures: Array[String] = []


func _init() -> void:
	_test_refinery_coproduction()
	_test_refinery_margin()
	_test_fleet_grades()
	_test_power_oil_burns_diesel()
	_test_needs_mapping()
	_test_save_migration()
	_finish()


# --- (1) refinaria: 3 saídas nas frações, com conservação -------------------
func _test_refinery_coproduction() -> void:
	var sim := _world()
	var gasoline := _product(sim, "gasoline")
	var diesel := _product(sim, "diesel")
	var jet := _product(sim, "jet_fuel")
	var oil := _product(sim, "oil")
	if gasoline < 0 or diesel < 0 or jet < 0:
		_fail("(1) goods.json sem gasoline/diesel/jet_fuel")
		return
	if oil < 0:
		_fail("(1) goods.json sem oil")
		return
	sim.set_instant_construction(true)
	sim.set_market_stock("AGRIA", "oil", 5000.0)
	var built: Dictionary = sim.build("AGRIA", "F0-REFINERY", "oil", -1,
			gasoline, REFINERY_CAPACITY, 200.0, 2.0, 12.0, 0.0,
			"private", 10000.0, "gasoline_a")
	if not bool(built.get("created", false)):
		_fail("(1) refinaria não foi construída: %s" % built)
		return
	var base: Dictionary = sim.get_status()
	var oil_before := _market_stock(sim, "AGRIA", "oil")
	var before := {
		"gasoline": _array_value(base, "produced_total", gasoline),
		"diesel": _array_value(base, "produced_total", diesel),
		"jet": _array_value(base, "produced_total", jet),
	}
	var last: Dictionary = {}
	for _day in REFINERY_DAYS:
		last = sim.tick_day()
	var made_gasoline := _array_value(last, "produced_total", gasoline) \
			- float(before["gasoline"])
	var made_diesel := _array_value(last, "produced_total", diesel) \
			- float(before["diesel"])
	var made_jet := _array_value(last, "produced_total", jet) \
			- float(before["jet"])
	# A entrada industrial não entra em `consumed_total` (esse é o consumo da
	# POP): a conservação é medida pelo estoque de petróleo que saiu do mercado.
	var used_oil := oil_before - _market_stock(sim, "AGRIA", "oil")
	print("[f0] refinaria %d d: gasoline=%.3f diesel=%.3f jet=%.3f (oil=%.3f)" % [
		REFINERY_DAYS, made_gasoline, made_diesel, made_jet, used_oil])
	_check("(1) refinaria produz as três saídas",
			made_gasoline > EPS and made_diesel > EPS and made_jet > EPS,
			"gasoline=%.6f diesel=%.6f jet=%.6f" % [
				made_gasoline, made_diesel, made_jet])
	if made_gasoline <= EPS:
		return
	_check("(1) fração diesel = 29/45 da gasolina",
			absf(made_diesel - made_gasoline * DIESEL_SHARE / GASOLINE_SHARE)
				<= EPS * maxf(1.0, made_gasoline),
			"diesel=%.6f esperado=%.6f" % [
				made_diesel,
				made_gasoline * DIESEL_SHARE / GASOLINE_SHARE])
	_check("(1) fração querosene = 9/45 da gasolina",
			absf(made_jet - made_gasoline * JET_SHARE / GASOLINE_SHARE)
				<= EPS * maxf(1.0, made_gasoline),
			"jet=%.6f esperado=%.6f" % [
				made_jet, made_gasoline * JET_SHARE / GASOLINE_SHARE])
	_check("(1) conservação: saídas = 83% do petróleo (perda 17%)",
			absf(made_gasoline + made_diesel + made_jet
				- used_oil * (0.45 + 0.29 + 0.09))
				<= EPS * maxf(1.0, used_oil),
			"saídas=%.6f petróleo=%.6f" % [
				made_gasoline + made_diesel + made_jet, used_oil])


# --- (6) margem da refinaria com co-produtos (D-123) ----------------------
# A queixa t138: a IA comparava a refinaria pelo custo do lote dividido só pela
# gasolina (100/t) e nunca a escolhia, embora a FIRMA a operasse com margem.
# Aqui a conta é a mesma de `production.cpp` (D-092 + D-123), com os preços-base
# declarados em goods.json: a receita das saídas extras tem de pagar o lote.
func _test_refinery_margin() -> void:
	var prices := _base_prices(_json(GOODS_PATH))
	var recipe := _recipe(_json(RECIPES_PATH), "gasoline_a")
	var main := "gasoline"
	if recipe.is_empty() or not prices.has(main) or not prices.has("oil"):
		_fail("(6) goods/recipes sem gasoline_a ou preços-base")
		return
	var yield_main := float(recipe.get("yield", 0.0))
	if yield_main <= EPS:
		_fail("(6) gasoline_a sem yield")
		return
	var unit_input := 0.0
	for item in recipe.get("inputs", []):
		unit_input += float(item.get("qty", 0.0)) / yield_main \
				* float(prices.get(String(item.get("good", "")), 0.0))
	var byproduct_revenue := 0.0
	for item in recipe.get("byproducts", []):
		byproduct_revenue += float(item.get("qty", 0.0)) / yield_main \
				* float(prices.get(String(item.get("good", "")), 0.0))
	# Custo de trabalho/opex por tonelada de produto principal (params da receita).
	var labor_opex := float(recipe.get("jobs_per_ton", 0.0)) \
			* float(recipe.get("wage", 0.0)) \
			+ float(recipe.get("opex_per_ton", 0.0))
	var net_cost := unit_input + labor_opex
	var margin_with := float(prices[main]) + byproduct_revenue - net_cost
	var margin_without := float(prices[main]) - net_cost
	print(("[f0] margem refinaria: insumo=%.2f co-prod=%.2f trabalho/opex=%.2f "
		+ "margem=%.2f (sem crédito=%.2f)") % [
			unit_input, byproduct_revenue, labor_opex, margin_with,
			margin_without])
	_check("(6) com co-produtos a margem da refinaria é positiva",
			margin_with > 0.0, "margem=%.2f" % margin_with)
	_check("(6) sem o crédito a margem seria negativa (o defeito t138)",
			margin_without < 0.0, "margem=%.2f" % margin_without)


# --- (2) frota: cada modal exige o seu grau --------------------------------
func _test_fleet_grades() -> void:
	var sim := _world()
	for _day in 4:
		sim.tick_day()
	# Sem diesel (mas com querosene): caminhão/navio não embarcam.
	sim.set_fleet("DEMO", 50.0, 5.0, 0.0, 0.0, 100000.0, 100.0)
	var blocked: Dictionary = sim.tick_day()
	var blocked_fleet: Dictionary = sim.get_fleet("DEMO")
	_check("(2) sem diesel nenhum pedido terrestre embarca",
			_order_count(blocked) == 0
				and float(blocked_fleet.get("missed_no_fuel", 0.0)) > 0.0,
			"pedidos=%d sem_fuel=%.1f" % [_order_count(blocked),
				float(blocked_fleet.get("missed_no_fuel", 0.0))])
	# Com diesel e sem querosene, sem avião: a carga sai e o consumo é diesel.
	sim.set_fleet("DEMO", 50.0, 5.0, 0.0, 100000.0, 0.0, 100.0)
	var shipped: Dictionary = sim.tick_day()
	var shipped_fleet: Dictionary = sim.get_fleet("DEMO")
	print("[f0] frota sem jet: pedidos=%d diesel_usado=%.3f jet_usado=%.3f" % [
		_order_count(shipped),
		float(shipped_fleet.get("diesel_used_today", 0.0)),
		float(shipped_fleet.get("jet_used_today", 0.0))])
	_check("(2) com diesel e sem aviões a carga embarca",
			_order_count(shipped) > 0,
			"pedidos=%d" % _order_count(shipped))
	_check("(2) o modal terrestre debita só diesel",
			float(shipped_fleet.get("diesel_used_today", 0.0)) > 0.0
				and absf(float(shipped_fleet.get("jet_used_today", 0.0))) <= EPS,
			"diesel=%.6f jet=%.6f" % [
				float(shipped_fleet.get("diesel_used_today", 0.0)),
				float(shipped_fleet.get("jet_used_today", 0.0))])


# --- (3) usina a óleo queima diesel ---------------------------------------
func _test_power_oil_burns_diesel() -> void:
	var sim := _world()
	sim.set_power_definitions(
			{"DEMO": _power_nation()},
			{"AGRIA": _power_city({"oil": 100.0}),
				"PORTUS": _power_city({})},
			{}, true)
	var diesel := _product(sim, "diesel")
	if diesel < 0:
		_fail("(3) goods.json sem diesel")
		return
	sim.set_market_stock("AGRIA", "diesel", 1000.0)
	sim.set_market_price("AGRIA", "diesel", 60.0)
	sim.tick_day()
	var power: Dictionary = sim.get_power_status()
	var by_fuel: Dictionary = (power.get("nations", {}) as Dictionary) \
			.get("DEMO", {}).get("generation_mwh_by_fuel", {})
	var generation := float(by_fuel.get("oil", 0.0))
	var params: Dictionary = (_json(POWER_PARAMS_PATH) as Dictionary) \
			.get("params", {})
	var tons := generation * float(params.get("fuel_t_per_mwh_oil", 0.0))
	var consumed := _array_value(sim.get_status(), "consumed_today", diesel)
	print("[f0] usina a óleo: geração=%.2f MWh diesel=%.4f t" % [
		generation, consumed])
	_check("(3) a usina a óleo gera e compra diesel",
			generation > EPS and tons > EPS,
			"geração=%.3f MWh esperado=%.4f t" % [generation, tons])
	_check("(3) o diesel consumido é o declarado por MWh",
			absf(consumed - tons) <= 1e-6,
			"consumed_today=%.6f esperado=%.6f" % [consumed, tons])


# --- (4) o dado mapeia cada consumidor no grau certo ----------------------
func _test_needs_mapping() -> void:
	var needs: Dictionary = _json(NEEDS_PATH)
	var levels: Dictionary = needs.get("levels", {})
	var life: Dictionary = levels.get("life", {})
	var categories: Dictionary = life.get("categories", {})
	var transport: Dictionary = categories.get("transport", {})
	var substitutes: Array = transport.get("substitutes", [])
	_check("(4) consumo doméstico de transporte usa gasolina",
			substitutes == ["gasoline"], str(substitutes))
	var institutional: Dictionary = needs.get("institutional", {})
	var pop_life: Array = institutional.get("pop_life", [])
	var army: Array = institutional.get("army", [])
	var fleet: Array = institutional.get("vehicle_fleet", [])
	_check("(4) POP consome gasolina em pop_life",
			pop_life.has("gasoline") and not pop_life.has("fuel"), str(pop_life))
	_check("(4) exército consome diesel (não fuel)",
			army.has("diesel") and not army.has("fuel"), str(army))
	_check("(4) frota declara diesel e jet_fuel",
			fleet.has("diesel") and fleet.has("jet_fuel")
				and not fleet.has("fuel"), str(fleet))


# --- (5) migração do save v38 -> v39 --------------------------------------
func _test_save_migration() -> void:
	var original := _world()
	for _day in 2:
		original.tick_day()
	var save: Dictionary = original.to_dict()
	var control := _load(save)
	var legacy: Dictionary = save.duplicate(true)
	legacy["version"] = 38
	# 1. O bem `fuel` volta ao FIM da lista de produtos do save antigo, com um
	#    mercado por cidade alinhado no mesmo índice.
	var products: Array = legacy.get("products", [])
	products.append("fuel")
	var legacy_fuel_index := products.size() - 1
	legacy["products"] = products
	for city_row in legacy.get("cities", []):
		var markets: Array = (city_row as Dictionary).get("markets", [])
		markets.append({
			"stock": LEGACY_FUEL_STOCK,
			"price": 50.0,
			"demand": 0.0,
			"supply": 0.0,
			"desired": 0.0,
			"sold": 0.0,
			"sold_prev": 0.0,
			"shortage": 0.0,
			"surplus": 0.0,
		})
		(city_row as Dictionary)["markets"] = markets
	# 2. Armazém com estoque genérico.
	var storage: Dictionary = legacy.get("storage", {})
	for site in storage.get("sites", []):
		var stock: Array = (site as Dictionary).get("stock", [])
		stock.append({"product": "fuel", "tons": LEGACY_FUEL_STOCK,
			"inflow_t": 0.0})
		(site as Dictionary)["stock"] = stock
	# 3. Frota com um único `fuel` (o save v38 não tem diesel/jet).
	var jet_share := 0.0
	for fleet_row in legacy.get("fleets", []):
		var entry: Dictionary = fleet_row
		var diesel_day := float(entry.get("trucks", 0.0)) * FLEET_TRUCK_DAY \
				+ float(entry.get("ships", 0.0)) * FLEET_SHIP_DAY
		var jet_day := float(entry.get("planes", 0.0)) * FLEET_PLANE_DAY
		var total_day := diesel_day + jet_day
		jet_share = jet_day / total_day if total_day > EPS else 0.0
		for key in ["diesel", "diesel_target", "jet_fuel", "jet_target",
				"diesel_used_total", "jet_used_total",
				"diesel_bought_total", "jet_bought_total"]:
			entry.erase(key)
		entry["fuel"] = LEGACY_FLEET_FUEL
		entry["fuel_target"] = LEGACY_FLEET_FUEL
	# 4. Uma construção e uma obra da receita antiga `fuel_a`.
	var buildings: Array = legacy.get("buildings", [])
	if buildings.is_empty():
		_fail("(5) save sem construções para migrar")
		return
	var building: Dictionary = buildings[0]
	building["recipe"] = "fuel_a"
	building["output_product"] = legacy_fuel_index
	building["input_product"] = -1
	legacy["buildings"] = buildings

	var migrated := _load(legacy)
	var gasoline := _product(migrated, "gasoline")
	var diesel := _product(migrated, "diesel")
	var jet := _product(migrated, "jet_fuel")
	_check("(5) taxa de consumo da frota migrada coerente",
			jet_share >= 0.0 and jet_share < 1.0, "jet_share=%.6f" % jet_share)
	_check("(5) o mundo migrado não tem o bem fuel",
			_product(migrated, "fuel") < 0
				and gasoline >= 0 and diesel >= 0 and jet >= 0,
			"fuel=%d" % _product(migrated, "fuel"))
	# Mercado: controle (v39 sem fuel) + frações declaradas.
	var control_markets: Array = _city_markets(control)
	var migrated_markets: Array = _city_markets(migrated)
	var control_gas := float(control_markets[gasoline].get("stock", 0.0))
	var moved_gas := float(migrated_markets[gasoline].get("stock", 0.0))
	var control_diesel := float(control_markets[diesel].get("stock", 0.0))
	var moved_diesel := float(migrated_markets[diesel].get("stock", 0.0))
	var control_jet := float(control_markets[jet].get("stock", 0.0))
	var moved_jet := float(migrated_markets[jet].get("stock", 0.0))
	print("[f0] migração mercado: gasoline %.3f->%.3f diesel %.3f->%.3f jet %.3f->%.3f" % [
		control_gas, moved_gas, control_diesel, moved_diesel,
		control_jet, moved_jet])
	_check("(5) estoque de fuel vira gasolina na fração 45/83",
			absf(moved_gas - control_gas
				- LEGACY_FUEL_STOCK * GASOLINE_SHARE) <= 1e-6,
			"delta=%.6f esperado=%.6f" % [
				moved_gas - control_gas, LEGACY_FUEL_STOCK * GASOLINE_SHARE])
	_check("(5) estoque de fuel vira diesel na fração 29/83",
			absf(moved_diesel - control_diesel
				- LEGACY_FUEL_STOCK * DIESEL_SHARE) <= 1e-6,
			"delta=%.6f esperado=%.6f" % [
				moved_diesel - control_diesel, LEGACY_FUEL_STOCK * DIESEL_SHARE])
	_check("(5) estoque de fuel vira querosene na fração 9/83",
			absf(moved_jet - control_jet
				- LEGACY_FUEL_STOCK * JET_SHARE) <= 1e-6,
			"delta=%.6f esperado=%.6f" % [
				moved_jet - control_jet, LEGACY_FUEL_STOCK * JET_SHARE])
	# Frota: o `fuel` único do save antigo é rateado pelo consumo de cada modal.
	# O save v38 não tem diesel/jet gravados (as chaves nem existiam): o
	# estoque migrado é exatamente a divisão do total antigo.
	var control_fleet: Dictionary = control.get_fleet("DEMO")
	var migrated_fleet: Dictionary = migrated.get_fleet("DEMO")
	var expected_jet := LEGACY_FLEET_FUEL * jet_share
	var expected_diesel := LEGACY_FLEET_FUEL * (1.0 - jet_share)
	print("[f0] migração frota: controle diesel=%.3f jet=%.3f | migrado diesel=%.3f jet=%.3f" % [
		float(control_fleet.get("diesel", 0.0)),
		float(control_fleet.get("jet_fuel", 0.0)),
		float(migrated_fleet.get("diesel", 0.0)),
		float(migrated_fleet.get("jet_fuel", 0.0))])
	_check("(5) frota: diesel herda a parcela de caminhão/navio",
			absf(float(migrated_fleet.get("diesel", 0.0))
				- expected_diesel) <= 1e-6,
			"diesel=%.6f esperado=%.6f" % [
				float(migrated_fleet.get("diesel", 0.0)), expected_diesel])
	_check("(5) frota: querosene herda a parcela dos aviões",
			absf(float(migrated_fleet.get("jet_fuel", 0.0))
				- expected_jet) <= 1e-6,
			"jet=%.6f esperado=%.6f" % [
				float(migrated_fleet.get("jet_fuel", 0.0)), expected_jet])
	_check("(5) frota: total migrado conserva o estoque antigo",
			absf(float(migrated_fleet.get("diesel", 0.0))
				+ float(migrated_fleet.get("jet_fuel", 0.0))
				- LEGACY_FLEET_FUEL) <= 1e-6,
			"total=%.6f" % (float(migrated_fleet.get("diesel", 0.0))
				+ float(migrated_fleet.get("jet_fuel", 0.0))))
	# Receita antiga resolvida para a nova e produto principal correto.
	var migrated_save: Dictionary = migrated.to_dict()
	var migrated_building: Dictionary = migrated_save["buildings"][0]
	_check("(5) receita fuel_a vira gasoline_a",
			String(migrated_building.get("recipe", "")) == "gasoline_a",
			String(migrated_building.get("recipe", "")))
	_check("(5) construção do fuel genérico produz gasoline",
			int(migrated_building.get("output_product", -1)) == gasoline,
			"output=%d gasoline=%d" % [
				int(migrated_building.get("output_product", -1)), gasoline])
	_check("(5) save migrado sai na versão atual",
			int(migrated_save.get("version", 0)) >= 39,
			"version=%d" % int(migrated_save.get("version", 0)))


# --- helpers --------------------------------------------------------------
func _world() -> Object:
	var sim = EconomySim.new()
	sim.set_sim_threads(1)
	sim.set_goods(_json(GOODS_PATH))
	sim.set_recipes(_json(RECIPES_PATH))
	sim.set_needs(_json(NEEDS_PATH))
	sim.initialize_scenario()
	return sim


func _load(data: Dictionary) -> Object:
	var sim = EconomySim.new()
	sim.set_sim_threads(1)
	sim.set_goods(_json(GOODS_PATH))
	sim.set_recipes(_json(RECIPES_PATH))
	sim.set_needs(_json(NEEDS_PATH))
	sim.from_dict(data)
	return sim


func _city_markets(sim: Object) -> Array:
	return sim.get_city_status("AGRIA").get("markets", [])


func _power_nation() -> Dictionary:
	return {"DEMO": {
		"access_pct": 100.0,
		"access_urban_pct": 100.0,
		"access_rural_pct": 100.0,
		"loss_pct": 4.0,
	}}


func _power_city(park: Dictionary) -> Dictionary:
	return {
		"access_pct": 100.0,
		"loss_pct": 4.0,
		"park": park,
		"cap": {},
		"solar_month": [],
	}


func _product(sim: Object, product: String) -> int:
	return Array(sim.get_status().get("products", [])).find(product)


func _order_count(status: Dictionary) -> int:
	return Array(status.get("trade_orders", [])).size()


func _market_stock(sim: Object, city_id: String, product: String) -> float:
	var index := _product(sim, product)
	var markets: Array = sim.get_city_status(city_id).get("markets", [])
	if index < 0 or index >= markets.size():
		return 0.0
	return float((markets[index] as Dictionary).get("stock", 0.0))


func _array_value(status: Dictionary, key: String, index: int) -> float:
	var values: Array = status.get(key, [])
	if index < 0 or index >= values.size():
		return 0.0
	return float(values[index])


func _base_prices(goods: Variant) -> Dictionary:
	var prices := {}
	for good in Array(goods):
		var entry: Dictionary = good
		prices[String(entry.get("id", ""))] = float(
				entry.get("base_price", 0.0))
	return prices


func _recipe(recipes: Variant, id: String) -> Dictionary:
	for item in Array(recipes):
		var entry: Dictionary = item
		if String(entry.get("id", "")) == id:
			return entry
	return {}


func _json(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("não abriu " + path)
		return null
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data


func _check(name: String, ok: bool, detail: String) -> void:
	if ok:
		print("[f0] ok: " + name)
	else:
		_fail("%s (%s)" % [name, detail])


func _fail(message: String) -> void:
	_failures.append(message)


func _finish() -> void:
	if _failures.is_empty():
		print("[f0] OK: refinaria (45/29/9, conservação), frota por grau, "
			+ "usina a óleo->diesel, mapeamento de consumo e migração v39.")
		quit(0)
	else:
		print("[f0] FALHAS (%d):" % _failures.size())
		for failure in _failures:
			print(" - " + failure)
		quit(1)
