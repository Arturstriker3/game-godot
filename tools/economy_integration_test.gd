extends SceneTree

## Teste de integração dos quatro sistemas econômicos: geografia natural,
## cidades/POPs, produção (trigo consumido diretamente), comércio, investimento
## e save/load. Roda headless e falha com código diferente de zero.

const GeographyServiceScript := preload("res://core/economy/geography_service.gd")

const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"

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
	# Base de 737 territórios: cidades vizinhas da Europa caem em territórios
	# distintos (AGRIA=FRA, PORTUS=BEL). A referência à Austrália garante que o
	# `territory_for` também distingue regiões distantes.
	if agria == portus:
		_fail("geografia: cidades distintas no mesmo território")
	var australia := geo.territory_for("PROBE_AUSTRALIA", Vector2(4375.0, 1597.0))
	if australia == "":
		_fail("geografia: território de referência distante não resolvido")
	if agria == australia:
		_fail("geografia: Europa e Austrália no mesmo território")

	var wheat := geo.crop_suitability("AGRIA", AGRIA_CENTER, "wheat")
	if wheat < 0.0 or wheat > 1.0:
		_fail("geografia: aptidão de trigo de AGRIA fora de 0..1 (%.3f)" % wheat)

	var deposits: Array = geo.deposits_for("PORTUS", PORTUS_CENTER)
	print("[integration] AGRIA=%s trigo=%.3f PORTUS=%s depósitos=%d" % [
		agria, wheat, portus, deposits.size()])


## Bens, receitas e cesta alimentar carregados antes de criar o mundo (F2): o
## núcleo resolve os ids de insumo na carga e os arrays por produto passam a
## ter a contagem do goods.json (113). Sem isso o C++ mantém os 21 primários.
var _expected_products := 21


func _load_chains(sim) -> void:
	var goods := _load_json_array(GOODS_PATH)
	sim.set_goods(goods)
	_expected_products = maxi(21, goods.size())
	sim.set_recipes(_load_json_array(RECIPES_PATH))
	sim.set_needs(_load_json_dict(NEEDS_PATH))


func _load_json_array(path: String) -> Array:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("dados: nao abriu " + path)
		return []
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data if typeof(data) == TYPE_ARRAY else []


func _load_json_dict(path: String) -> Dictionary:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("dados: nao abriu " + path)
		return {}
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data if typeof(data) == TYPE_DICTIONARY else {}


func _test_economy() -> void:
	var sim = EconomySim.new()
	_load_chains(sim)
	# §15: o teste de integração mede conservação/cadeia, não cronograma de obra
	# — liga a construção instantânea para o `invest`/`build` do teste
	# materializar o prédio na hora (comportamento anterior).
	sim.set_instant_construction(true)
	sim.initialize_scenario()

	var geo = GeographyServiceScript.new()
	geo.apply_to_sim(sim, [
		{"id": "AGRIA", "center": AGRIA_CENTER},
		{"id": "PORTUS", "center": PORTUS_CENTER},
	])

	var status: Dictionary = sim.get_status()
	_check_status_contract(status)
	_check_building_resources(sim, status)

	var money_initial := float(status["money_total"])
	var population_initial := _population(status)

	for day in DAYS:
		status = sim.tick_day()
		_check_conservation(sim, status, money_initial, population_initial, day + 1)
		if int(status["calendar_day"]) != day + 1:
			_fail("calendário: dia %d != %d" % [status["calendar_day"], day + 1])

	_check_chain(status)
	_check_wages_and_demand(sim, status)
	_check_gdp(sim, status)
	_check_investment(sim)
	_check_save_load(sim)
	_check_save_migration()
	_check_save_migration_v7()
	_check_suitability_gate()


func _check_status_contract(status: Dictionary) -> void:
	if String(status.get("mode", "")) != "demo":
		_fail("status: modo deveria ser demo")
	if int(status.get("calendar_day", -1)) != 0:
		_fail("status: calendário inicial deveria ser 0")
	if Array(status.get("products", [])).size() != _expected_products:
		_fail("status: lista de produtos incompleta (%d/%d)" % [
			Array(status.get("products", [])).size(), _expected_products])
	if Array(status.get("cities", [])).size() != 2:
		_fail("status: cenário deveria ter duas cidades")
	for city in status.get("cities", []):
		if Array(city.get("suitability", [])).size() != _expected_products:
			_fail("%s: aptidões ausentes no status" % city.get("id", "?"))
		for field in ["imported", "exported", "import_value", "export_value"]:
			if Array(city.get(field, [])).size() != _expected_products:
				_fail("%s: %s ausente no status" % [city.get("id", "?"), field])


func _check_building_resources(sim, status: Dictionary) -> void:
	var links := {"wheat": 0}
	var found := {"wheat": false}
	for city in status.get("cities", []):
		var city_id := String(city.get("id", ""))
		var expected_suitability := float(Array(city.get("suitability", [1.0]))[0])
		# O status diário publica só os escalares leves; o detalhe pesado
		# (construções, mercados, POP, serviços) vem sob demanda por cidade.
		for building in sim.get_city_status(city_id).get("buildings", []):
			var resource := String(building.get("resource", ""))
			if not links.has(resource):
				_fail("%s: construção sem recurso declarado" % building.get("id", "?"))
				continue
			found[resource] = true
			if int(building.get("output_product", -1)) != links[resource]:
				_fail("%s: produto de saída não corresponde ao recurso" % building.get("id", "?"))
			if int(building.get("input_product", 0)) != -1:
				_fail("%s: extração primária não deveria ter insumo" % building.get("id", "?"))
			if absf(float(building.get("site_suitability", -1.0)) - expected_suitability) > 0.01:
				_fail("%s: aptidão territorial não aplicada" % building.get("id", "?"))
	for resource in found:
		if not found[resource]:
			_fail("cadeia: nenhuma construção produz %s" % resource)


func _check_conservation(sim, status: Dictionary, money_initial: float,
		population_initial: float, day: int) -> void:
	if absf(float(status["money_total"]) - money_initial) > MONEY_EPSILON:
		_fail("dia %d: dinheiro %.4f != %.4f" % [
			day, status["money_total"], money_initial])
	if absf(_population(status) - population_initial) > EPSILON:
		_fail("dia %d: população %.3f != %.3f" % [
			day, _population(status), population_initial])
	for city in status.get("cities", []):
		var city_id := String(city.get("id", ""))
		for market in sim.get_city_status(city_id).get("markets", []):
			if float(market["stock"]) < -EPSILON:
				_fail("dia %d: estoque negativo em %s (%.4f)" % [
					day, city_id, market["stock"]])
			if float(market["desired"]) + EPSILON < float(market["demand"]):
				_fail("dia %d: %s comprou mais que a demanda desejada" % [
					day, city_id])
			if float(market["shortage"]) < -EPSILON:
				_fail("dia %d: %s escassez negativa" % [day, city_id])
			if float(market["surplus"]) < -EPSILON:
				_fail("dia %d: %s excesso negativo" % [day, city_id])


func _check_chain(status: Dictionary) -> void:
	var produced: Array = status["produced_total"]
	var consumed: Array = status["consumed_total"]
	if float(produced[0]) <= 0.0:
		_fail("cadeia: trigo sem produção")
	if float(consumed[0]) <= 0.0:
		_fail("cadeia: trigo não consumido")
	print("[integration] produzido(trigo)=%.1f consumido(trigo)=%.1f" % [
		produced[0], consumed[0]])


func _check_wages_and_demand(sim, status: Dictionary) -> void:
	for city in status.get("cities", []):
		# POP e construções são detalhe pesado: vêm sob demanda pelo id da cidade.
		var heavy: Dictionary = sim.get_city_status(String(city.get("id", "")))
		var pop: Dictionary = heavy.get("pop", {})
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
		for building in heavy.get("buildings", []):
			profit += float(building["profit"])
		if profit <= 0.0:
			_fail("%s: lucro privado não positivo (%.2f)" % [city["id"], profit])


func _check_gdp(sim, status: Dictionary) -> void:
	for city in status.get("cities", []):
		var expected := float(city.get("service_revenue", 0.0))
		for building in sim.get_city_status(String(city.get("id", ""))).get("buildings", []):
			expected += float(building["revenue"]) - float(building["input_value"])
		var gdp := float(city["gdp"])
		if absf(gdp - expected) > 0.01:
			_fail("%s: PIB %.2f != valor adicionado %.2f" % [city["id"], gdp, expected])
		var per_capita := gdp / maxf(float(city["population"]), 1.0)
		if absf(float(city["gdp_per_capita"]) - per_capita) > 1e-9:
			_fail("%s: PIB per capita inconsistente" % city["id"])


func _check_investment(sim) -> void:
	var before: Dictionary = sim.get_status()
	var city: Dictionary = before["cities"][0]
	var city_id := String(city["id"])
	# Construções vêm do detalhe pesado sob demanda (não do status diário).
	var building: Dictionary = sim.get_city_status(city_id).get("buildings", [])[0]
	var cash_before := float(city["firm_cash"])
	var capacity_before := float(building["capacity"])
	var money_before := float(before["money_total"])

	var result: Dictionary = sim.invest(city["id"], building["id"], 1000.0)
	if absf(float(result["invested"]) - 1000.0) > EPSILON:
		_fail("investimento: capital não aplicado (%s)" % result["error"])

	var after: Dictionary = sim.get_status()
	var cash_after := float(after["cities"][0]["firm_cash"])
	var capacity_after := float(
		sim.get_city_status(city_id).get("buildings", [])[0]["capacity"])
	if absf((cash_before - cash_after) - 1000.0) > 0.01:
		_fail("investimento: caixa debitado incorretamente")
	if capacity_after <= capacity_before + EPSILON:
		_fail("investimento: capacidade não expandiu")
	if absf(float(after["money_total"]) - money_before) > MONEY_EPSILON:
		_fail("investimento: dinheiro não conservado")

	var rejected: Dictionary = sim.invest(city["id"], building["id"], 1.0e12)
	if String(rejected["error"]) == "":
		_fail("investimento: aceitou capital inexistente")


func _check_save_load(sim) -> void:
	var snapshot: Dictionary = sim.to_dict()
	# F9 subiu o schema para 11 (grupos, IDE, incentivos e confiança);
	# §15 subiu para 12 (obras como projeto físico); §19 W0 para 13 (solo
	# urbano, moradia e prefeitura por cidade); v14 unifica as obras de
	# moradia na fila §15 (kind CONSTRUCTION_HOUSING); v15 é a cadência A6;
	# v16 (B1) adiciona as coortes etárias (`pop.age_bands`); v17 (B2)
	# adiciona os quintis de renda (`pop.strata`) e `rent_income_next`; v18
	# (D-086) adiciona o estágio de mina (`stage`, `stage_reserve_kt`,
	# `stage_mined_kt` e `deepen_stage` nas obras); v19 (B3) a região da cidade
	# (`city.region`); v20 (B4) os acumuladores de migração (`region.migration`).
	# v24 (D-096) o estado da safra por plano; v25 (D-101) o bloco `storage`
	# (armazém, capacidade e estoque guardado); v26 (C1/D-102) o salário
	# atrasado (passivo da firma + ativo das famílias).
	# Mudança esperada: a versão do schema sobe — o conteúdo das regras antigas
	# não muda.
	# v27 (C2/D-103) o banco central por moeda: `monetary_policy`, `central_bank`
	# e, em cada moeda, `exchange_regime` e `bank_independence`; v28 (C2b1/D-104)
	# o bloco `monetary`; v29 (C2b2/D-106) o bloco `fx` (paridade, banda,
	# reservas, conversão do ouro e eventos) e o ouro físico no cofre;
	# v30 (C3/D-107) o bloco `banks` (crédito e banco por nação) e a dívida
	# dos grupos.
	if int(snapshot.get("version", 0)) != 32:
		_fail("save: versão do schema deveria ser 32 (é %s)" % str(snapshot.get("version")))
	# F8 (v10): negócios de serviço e, por cidade, o bloco `services`.
	if not snapshot.has("service_businesses"):
		_fail("save: bloco 'service_businesses' da F8 ausente")
	# F9 (v11): grupos e propriedade, fluxos de dividendo/repatriação,
	# política de IDE, incentivos setoriais e confiança do anfitrião.
	for key: String in ["groups", "group_flows", "fdi_policies",
			"sector_incentives", "confidence"]:
		if not snapshot.has(key):
			_fail("save: bloco '%s' da F9 ausente" % key)
	# F4: a frota/combustível por nação faz parte do save (v5) e precisa
	# sobreviver ao round-trip; sem o bloco, o load rederivaria a frota.
	if not snapshot.has("fleets") or Array(snapshot.get("fleets", [])).is_empty():
		_fail("save: bloco 'fleets' da F4 ausente")
	# F5: moedas e tarifas (v6) — presentes mesmo vazias, para o save ser
	# explícito sobre o câmbio do mundo.
	if not snapshot.has("currencies") or not snapshot.has("tariffs"):
		_fail("save: blocos 'currencies'/'tariffs' da F5 ausentes")
	if Array(snapshot.get("products", [])).size() != _expected_products:
		_fail("save: lista de produtos incompleta")
	var restored = EconomySim.new()
	# O load resolve a receita de cada construção por id: as tabelas precisam
	# estar carregadas antes de from_dict.
	_load_chains(restored)
	restored.from_dict(snapshot)

	if JSON.stringify(restored.to_dict()) != JSON.stringify(snapshot):
		_fail("save/load: round-trip não preserva o estado")

	var original_next: Dictionary = sim.tick_day()
	var restored_next: Dictionary = restored.tick_day()
	if JSON.stringify(original_next) != JSON.stringify(restored_next):
		_fail("save/load: próximo tick diverge após carregar")


## A2: migração de índice de produto v1/v2 -> v3. Um save antigo traz
## farinha(1)/pão(2)/ferro(3); o layout atual é ferro(1)/arroz(2)... A migração
## deve remapear o ferro para 1 e descartar farinha/pão (construções e mercados).
func _check_save_migration() -> void:
	var v1 := {
		"version": 1,
		"active": true,
		"world_mode": false,
		"day": 5,
		"transport_cash": 0.0,
		"foreign_cash": 0.0,
		"construction_cash": 0.0,
		"produced_total": [1.0, 2.0, 3.0, 4.0],
		"consumed_total": [1.0, 2.0, 3.0, 4.0],
		"cities": [{
			"id": "AGRIA", "name": "Agria", "nation": "AGR",
			"center": [0.0, 0.0], "population": 1000.0,
			"markets": [
				_v1_market(10.0, 40.0),   # trigo (índice 0)
				_v1_market(999.0, 1.0),   # farinha (descartado)
				_v1_market(888.0, 2.0),   # pão (descartado)
				_v1_market(111.0, 55.5),  # ferro -> novo índice 1
			],
		}],
		"buildings": [
			{"id": "FARM-A-1", "city": "AGRIA", "output_product": 0},   # trigo
			{"id": "MILL-1", "city": "AGRIA", "output_product": 1},     # farinha (descartado)
			{"id": "BAKERY-1", "city": "AGRIA", "output_product": 2},   # pão (descartado)
			{"id": "MINE-1", "city": "AGRIA", "output_product": 3},     # ferro -> 1
		],
		"in_transit": [],
	}
	var migrated := EconomySim.new()
	_load_chains(migrated)
	migrated.from_dict(v1)
	var after: Dictionary = migrated.to_dict()

	var cities: Array = after.get("cities", [])
	if cities.is_empty():
		_fail("migração: nenhuma cidade carregada")
		return
	var markets: Array = cities[0].get("markets", [])
	if markets.size() != _expected_products:
		_fail("migração: mercados deveriam ter %d posições (%d)" % [
			_expected_products, markets.size()])
		return
	# Ferro (novo índice 1) herda estoque/preço do antigo índice 3.
	if absf(float(markets[1]["stock"]) - 111.0) > EPSILON \
			or absf(float(markets[1]["price"]) - 55.5) > EPSILON:
		_fail("migração: ferro não remapeado (%.2f/%.2f)" % [
			float(markets[1]["stock"]), float(markets[1]["price"])])
	# Trigo (índice 0) permanece.
	if absf(float(markets[0]["stock"]) - 10.0) > EPSILON:
		_fail("migração: trigo não preservado (%.2f)" % float(markets[0]["stock"]))

	var buildings: Array = after.get("buildings", [])
	var outputs: Array = []
	for building in buildings:
		outputs.append(int(building.get("output_product", -1)))
	if buildings.size() != 2:
		_fail("migração: moinho/padaria deveriam ser descartados (%d construções)" % buildings.size())
	elif not (outputs.has(1) and outputs.has(0)):
		_fail("migração: mina de ferro deveria virar output 1 (outputs=%s)" % str(outputs))
	print("[integration] migração v1->v3: mercados e construções remapeados corretamente.")


## Migração por id do save v7 com 114 produtos: `body_armor` (order 112) sai e
## `infantry_gear` (113) passa a ocupar 112. As posições por produto (mercados,
## produção, construções e cargas) têm de seguir o id, não o índice antigo.
func _check_save_migration_v7() -> void:
	var sim := EconomySim.new()
	_load_chains(sim)
	sim.initialize_scenario()
	sim.tick_day()
	var current: Dictionary = sim.to_dict()
	var products: Array = current.get("products", [])
	if products.size() != _expected_products or String(products[112]) != "infantry_gear":
		_fail("migração v7: layout atual inesperado (%d produtos, último=%s)" % [
			products.size(), "" if products.is_empty() else String(products[-1])])
		return

	var old := current.duplicate(true)
	old["version"] = 7
	var old_products: Array = products.duplicate()
	old_products.insert(112, "body_armor")
	old["products"] = old_products

	# Sentinela no índice 112 (body_armor); o valor real de infantry_gear fica 113.
	var old_produced: Array = Array(current["produced_total"]).duplicate()
	old_produced.insert(112, 777.0)
	old["produced_total"] = old_produced
	var old_consumed: Array = Array(current["consumed_total"]).duplicate()
	old_consumed.insert(112, 777.0)
	old["consumed_total"] = old_consumed

	var old_cities: Array = Array(current["cities"]).duplicate(true)
	for city in old_cities:
		var markets: Array = Array(city["markets"]).duplicate(true)
		markets.insert(112, _v1_market(1234.0, 56.78))
		city["markets"] = markets
	old["cities"] = old_cities

	# Uma construção em body_armor (descartada) e uma em infantry_gear (vira 112).
	old["buildings"] = [
		{"id": "ARMOR-OLD-1", "city": "AGRIA", "kind": 1,
			"output_product": 112, "recipe": "body_armor_a", "capacity": 5.0},
		{"id": "INFANTRY-113", "city": "AGRIA", "kind": 1,
			"output_product": 113, "recipe": "infantry_gear", "capacity": 5.0},
	]
	# body_armor em trânsito é descartado; infantry_gear vira produto 112.
	old["in_transit"] = [
		{"to_city": "AGRIA", "product": 112, "amount": 5.0, "arrival_day": 999},
		{"to_city": "AGRIA", "product": 113, "amount": 7.0, "arrival_day": 999},
	]

	var migrated := EconomySim.new()
	_load_chains(migrated)
	migrated.from_dict(old)
	var after: Dictionary = migrated.to_dict()

	var after_products: Array = after.get("products", [])
	if after_products.size() != _expected_products or after_products.has("body_armor") \
			or String(after_products[112]) != "infantry_gear":
		_fail("migração v7: lista de produtos não remapeada (%d, body_armor=%s)" % [
			after_products.size(), str(after_products.has("body_armor"))])
		return

	var after_produced: Array = Array(after["produced_total"])
	if after_produced.size() != _expected_products \
			or not is_equal_approx(float(after_produced[112]), float(old_produced[113])):
		_fail("migração v7: produced_total não seguiu o id")
	var after_consumed: Array = Array(after["consumed_total"])
	if after_consumed.size() != _expected_products \
			or not is_equal_approx(float(after_consumed[112]), float(old_consumed[113])):
		_fail("migração v7: consumed_total não seguiu o id")

	var after_markets: Array = Array(Array(after["cities"])[0]["markets"])
	var old_markets: Array = Array(old_cities[0]["markets"])
	if after_markets.size() != _expected_products:
		_fail("migração v7: mercados com %d posições" % after_markets.size())
	elif absf(float(after_markets[112]["stock"]) - float(old_markets[113]["stock"])) > EPSILON:
		_fail("migração v7: mercado de infantry_gear não herdou o índice 113")

	var outputs: Array = []
	for building in after.get("buildings", []):
		outputs.append(int(building.get("output_product", -1)))
	if outputs != [112]:
		_fail("migração v7: construções esperadas [112] (%s)" % str(outputs))

	var transit: Array = after.get("in_transit", [])
	if transit.size() != 1 or int(transit[0]["product"]) != 112 \
			or absf(float(transit[0]["amount"]) - 7.0) > EPSILON:
		_fail("migração v7: trânsito não remapeado (%s)" % str(transit))

	# Round-trip do save já migrado: carregar de novo tem de reproduzir o dict.
	var restored := EconomySim.new()
	_load_chains(restored)
	restored.from_dict(after)
	if JSON.stringify(restored.to_dict()) != JSON.stringify(after):
		_fail("migração v7: round-trip do save migrado divergiu")
	else:
		print("[integration] migração v7->v8: body_armor descartado por id e round-trip estável.")


func _v1_market(stock: float, price: float) -> Dictionary:
	return {
		"stock": stock, "price": price, "demand": 0.0, "supply": 0.0,
		"desired": 0.0, "sold": 0.0, "sold_prev": 0.0,
		"shortage": 0.0, "surplus": 0.0,
	}


func _check_suitability_gate() -> void:
	var gate = EconomySim.new()
	gate.initialize_scenario()
	gate.set_suitability("AGRIA", "wheat", 0.0)
	gate.set_suitability("PORTUS", "wheat", 0.0)
	var status: Dictionary = gate.tick_day()
	if float(status["produced_today"][0]) > EPSILON:
		_fail("geografia: aptidão zero ainda produz trigo")
	var blocked: Dictionary = gate.invest("AGRIA", "FARM-A-1", 100.0)
	if String(blocked["error"]) == "":
		_fail("geografia: investimento sem aptidão foi aceito")


func _population(status: Dictionary) -> float:
	var total := 0.0
	for city in status.get("cities", []):
		total += float(city["population"])
	return total


func _fail(message: String) -> void:
	_failures.append(message)
