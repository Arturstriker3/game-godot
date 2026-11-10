extends SceneTree

## Teste de integração dos quatro sistemas econômicos: geografia natural,
## cidades/POPs, produção (trigo -> farinha -> pão), comércio, investimento e
## save/load. Roda headless e falha com código diferente de zero.

const GeographyServiceScript := preload("res://core/economy/geography_service.gd")

const DAYS := 20
const MONEY_EPSILON := 0.5
const EPSILON := 0.001

const AGRIA_CENTER := Vector2(2532.6, 571.4)
const PORTUS_CENTER := Vector2(2560.4, 543.8)

var _failures: Array[String] = []


func _init() -> void:
	_test_geography()
	_test_economy()

	if _failures.is_empty():
		print("[integration] OK: geografia, cadeia, investimento e save/load validados.")
		quit(0)
	else:
		print("[integration] FALHAS (%d):" % _failures.size())
		for failure in _failures:
			print(" - " + failure)
		quit(1)


func _test_geography() -> void:
	var geo = GeographyServiceScript.new()
	if not geo.load_data():
		_fail("geografia: dados naturais não carregaram")
		return

	var agria := geo.territory_for("AGRIA", AGRIA_CENTER)
	var portus := geo.territory_for("PORTUS", PORTUS_CENTER)
	if agria == "" or portus == "":
		_fail("geografia: território não resolvido")
	if agria == portus:
		_fail("geografia: cidades distintas no mesmo território")

	var wheat := geo.crop_suitability("AGRIA", AGRIA_CENTER, "wheat")
	if wheat < 0.0 or wheat > 1.0:
		_fail("geografia: aptidão de trigo de AGRIA fora de 0..1 (%.3f)" % wheat)

	var deposits: Array = geo.deposits_for("PORTUS", PORTUS_CENTER)
	print("[integration] AGRIA=%s trigo=%.3f PORTUS=%s depósitos=%d" % [
		agria, wheat, portus, deposits.size()])


func _test_economy() -> void:
	var sim = EconomySim.new()
	sim.initialize_demo()

	var geo = GeographyServiceScript.new()
	geo.apply_to_sim(sim, [
		{"id": "AGRIA", "center": AGRIA_CENTER},
		{"id": "PORTUS", "center": PORTUS_CENTER},
	])

	var status: Dictionary = sim.get_demo_status()
	_check_status_contract(status)
	_check_building_resources(status)

	var money_initial := float(status["money_total"])
	var population_initial := _population(status)

	for day in DAYS:
		status = sim.tick_demo()
		_check_conservation(status, money_initial, population_initial, day + 1)
		if int(status["calendar_day"]) != day + 1:
			_fail("calendário: dia %d != %d" % [status["calendar_day"], day + 1])

	_check_chain(status)
	_check_wages_and_demand(status)
	_check_gdp(status)
	_check_investment(sim)
	_check_save_load(sim)
	_check_suitability_gate()


func _check_status_contract(status: Dictionary) -> void:
	if String(status.get("mode", "")) != "demo":
		_fail("status: modo deveria ser demo")
	if int(status.get("calendar_day", -1)) != 0:
		_fail("status: calendário inicial deveria ser 0")
	if Array(status.get("products", [])).size() != 18:
		_fail("status: lista de produtos incompleta")
	if Array(status.get("cities", [])).size() != 2:
		_fail("status: cenário deveria ter duas cidades")
	for city in status.get("cities", []):
		if Array(city.get("suitability", [])).size() != 18:
			_fail("%s: aptidões ausentes no status" % city.get("id", "?"))
		for field in ["imported", "exported", "import_value", "export_value"]:
			if Array(city.get(field, [])).size() != 18:
				_fail("%s: %s ausente no status" % [city.get("id", "?"), field])


func _check_building_resources(status: Dictionary) -> void:
	var links := {"wheat": 0, "flour": 1, "bread": 2}
	var found := {"wheat": false, "flour": false, "bread": false}
	for city in status.get("cities", []):
		var expected_suitability := float(Array(city.get("suitability", [1.0]))[0])
		for building in city.get("buildings", []):
			var resource := String(building.get("resource", ""))
			if not links.has(resource):
				_fail("%s: construção sem recurso declarado" % building.get("id", "?"))
				continue
			found[resource] = true
			if int(building.get("output_product", -1)) != links[resource]:
				_fail("%s: produto de saída não corresponde ao recurso" % building.get("id", "?"))
			if resource == "wheat" and int(building.get("input_product", 0)) != -1:
				_fail("%s: extração primária não deveria ter insumo" % building.get("id", "?"))
			if resource != "wheat" and int(building.get("input_product", -1)) < 0:
				_fail("%s: transformação sem insumo declarado" % building.get("id", "?"))
			if resource == "wheat" and absf(float(building.get("site_suitability", -1.0)) - expected_suitability) > 0.01:
				_fail("%s: aptidão territorial não aplicada" % building.get("id", "?"))
	for resource in found:
		if not found[resource]:
			_fail("cadeia: nenhuma construção produz %s" % resource)


func _check_conservation(status: Dictionary, money_initial: float,
		population_initial: float, day: int) -> void:
	if absf(float(status["money_total"]) - money_initial) > MONEY_EPSILON:
		_fail("dia %d: dinheiro %.4f != %.4f" % [
			day, status["money_total"], money_initial])
	if absf(_population(status) - population_initial) > EPSILON:
		_fail("dia %d: população %.3f != %.3f" % [
			day, _population(status), population_initial])
	for city in status.get("cities", []):
		for market in city.get("markets", []):
			if float(market["stock"]) < -EPSILON:
				_fail("dia %d: estoque negativo em %s (%.4f)" % [
					day, city["id"], market["stock"]])
			if float(market["desired"]) + EPSILON < float(market["demand"]):
				_fail("dia %d: %s comprou mais que a demanda desejada" % [
					day, city["id"]])
			if float(market["shortage"]) < -EPSILON:
				_fail("dia %d: %s escassez negativa" % [day, city["id"]])
			if float(market["surplus"]) < -EPSILON:
				_fail("dia %d: %s excesso negativo" % [day, city["id"]])


func _check_chain(status: Dictionary) -> void:
	var produced: Array = status["produced_total"]
	var consumed: Array = status["consumed_total"]
	for index in 3:
		if float(produced[index]) <= 0.0:
			_fail("cadeia: produto %d sem produção" % index)
	if float(consumed[2]) <= 0.0:
		_fail("cadeia: pão não consumido")
	print("[integration] produzido(t/far/pão)=%.1f/%.1f/%.1f" % [
		produced[0], produced[1], produced[2]])


func _check_wages_and_demand(status: Dictionary) -> void:
	for city in status.get("cities", []):
		var pop: Dictionary = city["pop"]
		if float(pop["wage_income"]) <= 0.0:
			_fail("%s: POPs sem salários" % city["id"])
		if float(pop["food_spend"]) <= 0.0:
			_fail("%s: POPs não compram alimentos" % city["id"])
		if float(pop["disposable"]) <= 0.0:
			_fail("%s: renda disponível não positiva" % city["id"])
		if absf(float(pop["employed"]) + float(pop["unemployed"])
				- float(pop["labor_force"])) > 0.01:
			_fail("%s: emprego + desemprego != força de trabalho" % city["id"])
		if float(pop["satisfaction"]) < 0.5:
			_fail("%s: satisfação alimentar baixa (%.2f)" % [
				city["id"], pop["satisfaction"]])
		var profit := 0.0
		for building in city.get("buildings", []):
			profit += float(building["profit"])
		if profit <= 0.0:
			_fail("%s: lucro privado não positivo (%.2f)" % [city["id"], profit])


func _check_gdp(status: Dictionary) -> void:
	for city in status.get("cities", []):
		var expected := float(city.get("service_revenue", 0.0))
		for building in city.get("buildings", []):
			expected += float(building["revenue"]) - float(building["input_value"])
		var gdp := float(city["gdp"])
		if absf(gdp - expected) > 0.01:
			_fail("%s: PIB %.2f != valor adicionado %.2f" % [city["id"], gdp, expected])
		var per_capita := gdp / maxf(float(city["population"]), 1.0)
		if absf(float(city["gdp_per_capita"]) - per_capita) > 1e-9:
			_fail("%s: PIB per capita inconsistente" % city["id"])


func _check_investment(sim) -> void:
	var before: Dictionary = sim.get_demo_status()
	var city: Dictionary = before["cities"][0]
	var building: Dictionary = city["buildings"][0]
	var cash_before := float(city["firm_cash"])
	var capacity_before := float(building["capacity"])
	var money_before := float(before["money_total"])

	var result: Dictionary = sim.demo_invest(city["id"], building["id"], 1000.0)
	if absf(float(result["invested"]) - 1000.0) > EPSILON:
		_fail("investimento: capital não aplicado (%s)" % result["error"])

	var after: Dictionary = sim.get_demo_status()
	var cash_after := float(after["cities"][0]["firm_cash"])
	var capacity_after := float(after["cities"][0]["buildings"][0]["capacity"])
	if absf((cash_before - cash_after) - 1000.0) > 0.01:
		_fail("investimento: caixa debitado incorretamente")
	if capacity_after <= capacity_before + EPSILON:
		_fail("investimento: capacidade não expandiu")
	if absf(float(after["money_total"]) - money_before) > MONEY_EPSILON:
		_fail("investimento: dinheiro não conservado")

	var rejected: Dictionary = sim.demo_invest(city["id"], building["id"], 1.0e12)
	if String(rejected["error"]) == "":
		_fail("investimento: aceitou capital inexistente")


func _check_save_load(sim) -> void:
	var snapshot: Dictionary = sim.demo_to_dict()
	var restored = EconomySim.new()
	restored.demo_from_dict(snapshot)

	if JSON.stringify(restored.demo_to_dict()) != JSON.stringify(snapshot):
		_fail("save/load: round-trip não preserva o estado")

	var original_next: Dictionary = sim.tick_demo()
	var restored_next: Dictionary = restored.tick_demo()
	if JSON.stringify(original_next) != JSON.stringify(restored_next):
		_fail("save/load: próximo tick diverge após carregar")


func _check_suitability_gate() -> void:
	var gate = EconomySim.new()
	gate.initialize_demo()
	gate.demo_set_suitability("AGRIA", "wheat", 0.0)
	gate.demo_set_suitability("PORTUS", "wheat", 0.0)
	var status: Dictionary = gate.tick_demo()
	if float(status["produced_today"][0]) > EPSILON:
		_fail("geografia: aptidão zero ainda produz trigo")
	var blocked: Dictionary = gate.demo_invest("AGRIA", "FARM-A-1", 100.0)
	if String(blocked["error"]) == "":
		_fail("geografia: investimento sem aptidão foi aceito")


func _population(status: Dictionary) -> float:
	var total := 0.0
	for city in status.get("cities", []):
		total += float(city["population"])
	return total


func _fail(message: String) -> void:
	_failures.append(message)
