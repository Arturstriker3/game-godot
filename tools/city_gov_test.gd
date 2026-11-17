extends SceneTree

## Teste do pacote W2 da §19 — prefeitura (docs/economy/s19_urban_spec.md).
##
## Confere o que a spec exige do W2:
##   - receitas = soma dos fluxos (caixa do mês fecha com IPTU + taxas + repasse
##     menos manutenção e obras);
##   - o repasse SAI do tesouro nacional: o que as prefeituras da nação recebem
##     é exatamente o que o orçamento público da mesma nação pagou;
##   - caixa da prefeitura nunca fica negativo;
##   - cidade com zona lotada DECIDE expandir (obra criada, orçamento reservado)
##     e a obra concluída abre área e capacidade de infraestrutura;
##   - save/load ida-e-volta com obra em andamento dá 0 diferenças;
##   - determinismo: threads=1 e threads=8 produzem o mesmo save.
##
## O teste NÃO altera os JSON: passa cópias em memória com a cidade de teste
## (BRA-SAO) comprimida para ~0,02 km² de área construída — assim a zona
## residencial fica lotada no dia 1 e a decisão de expansão é exercitada.

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

const TEST_CITY := "BRA-SAO"
const TEST_NATION := "BRA"
const MONTH_DAYS := 30
const CHECK_DAY := 30
const SAVE_DAY := 32
const LAST_DAY := 82
const TOLERANCE := 1.0e-6
const EXPAND_KIND := 0

var _failures: Array[String] = []


func _init() -> void:
	_run()
	if _failures.is_empty():
		print("[city_gov] OK: fluxo mensal, repasse do tesouro, caixa, decisao, "
			+ "conclusao, save/load e determinismo.")
		quit(0)
	else:
		for failure in _failures:
			print("[city_gov] FALHA: " + failure)
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


## JSON canônico: a comparação de save/load e de determinismo usa a mesma
## serialização do resto da suíte.
func _canonical(data: Dictionary) -> String:
	return JSON.stringify(data, "", true, true)


## Dados urbanos com a cidade de teste comprimida (em memória, sem tocar nos
## arquivos) e a nação de teste com receita/repasse observáveis.
func _urban_data() -> Dictionary:
	var land: Dictionary = _load_dict(URBAN_LAND_PATH).duplicate(true)
	var city: Dictionary = land.get("cities", {}).get(TEST_CITY, {})
	if city.is_empty():
		_fail("%s ausente de urban_land.json" % TEST_CITY)
	else:
		city["built_up_km2"] = 0.02
		city["expandable_reserve_km2"] = 4.0
		land["cities"][TEST_CITY] = city
	var finance: Dictionary = _load_dict(MUNICIPAL_FINANCE_PATH).duplicate(true)
	var nation: Dictionary = finance.get("nations", {}).get(TEST_NATION, {})
	if nation.is_empty():
		_fail("%s ausente de municipal_finance.json" % TEST_NATION)
	else:
		# Receita subnacional de 18% do PIB, 35% de transferência e 12% do total
		# de tributos em imposto predial: valores de teste, não da fonte.
		nation["subnational_revenue_pct_gdp"] = 18.0
		nation["central_transfers_pct_subnational"] = 35.0
		nation["property_tax_pct_total_tax"] = 12.0
		finance["nations"][TEST_NATION] = nation
	return {"land": land, "finance": finance}


func _load_chains(sim) -> void:
	sim.demo_set_goods(_load_array(GOODS_PATH))
	sim.demo_set_recipes(_load_array(RECIPES_PATH))
	sim.demo_set_needs(_load_dict(NEEDS_PATH))
	if sim.has_method("demo_set_services"):
		sim.demo_set_services(_load_dict(SERVICES_PATH))
	if sim.has_method("demo_set_currencies"):
		sim.demo_set_currencies(_load_dict(CURRENCIES_PATH))
	if sim.has_method("demo_set_sector_employment"):
		var sector := _load_dict(SECTOR_EMPLOYMENT_PATH)
		if not sector.is_empty():
			sim.demo_set_sector_employment(sector)


## Mundo real (cities/ports/buildings.json) com o estado urbano do W0 e o
## número de threads pedido.
func _make_world(threads: int, urban: Dictionary) -> Object:
	var sim = EconomySim.new()
	_load_chains(sim)
	if sim.has_method("demo_set_sim_threads"):
		sim.demo_set_sim_threads(threads)
	var cities := _load_array(CITIES_PATH)
	sim.initialize_demo_world(cities, _load_array(PORTS_PATH),
		_load_array(BUILDINGS_PATH))
	if not sim.has_method("demo_set_urban"):
		_fail("o binario GDExtension nao tem demo_set_urban (rode o build)")
		return sim
	sim.demo_set_urban(urban["land"], _load_dict(HOUSING_CITY_PATH),
		_load_dict(HOUSING_PATH), urban["finance"])
	return sim


func _gov(sim, city_id: String) -> Dictionary:
	var urban: Dictionary = sim.demo_get_urban_city(city_id)
	if urban.is_empty():
		_fail("%s sem estado urbano" % city_id)
		return {}
	return urban


## Tica um dia e exige invariantes limpos e caixa da prefeitura não negativo.
func _tick(sim, label: String) -> void:
	var status: Dictionary = sim.tick_demo()
	var violations: Array = status.get("invariant_violations", [])
	if violations.size() > 0:
		_fail("%s: invariante violado: %s" % [label, String(violations[0])])
	var cash := float(_gov(sim, TEST_CITY).get("gov", {}).get("cash", 0.0))
	if cash < -TOLERANCE:
		_fail("%s: caixa da prefeitura negativo: %.6f" % [label, cash])


func _received_transfer(sim, cities: Array) -> float:
	var total := 0.0
	for city_id in cities:
		var gov: Dictionary = _gov(sim, city_id).get("gov", {})
		total += float(gov.get("month_revenue_transfer", 0.0))
	return total


func _paid_transfer(sim, cities: Array) -> float:
	var total := 0.0
	for city_id in cities:
		var gov: Dictionary = _gov(sim, city_id).get("gov", {})
		total += float(gov.get("month_transfer_paid", 0.0))
	return total


func _nation_cities(sim, nation: String) -> Array:
	var ids: Array = []
	var status: Dictionary = sim.get_demo_status()
	for city in status.get("cities", []):
		if String(city.get("nation", "")) == nation:
			ids.append(String(city.get("id", "")))
	return ids


func _run() -> void:
	var urban := _urban_data()
	if not _failures.is_empty():
		return
	var nation_cities := []  # preenchido depois do mundo criado

	# --- Mundo A (threads=1): fluxo mensal, decisão e obra até o fim. --------
	var sim = _make_world(1, urban)
	nation_cities = _nation_cities(sim, TEST_NATION)
	if nation_cities.is_empty():
		_fail("nenhuma cidade de %s no mundo" % TEST_NATION)
		return

	var gov_before: Dictionary = _gov(sim, TEST_CITY).get("gov", {})
	if float(gov_before.get("target_revenue_month", 0.0)) != 0.0:
		_fail("alvo de receita do mes deveria comecar em 0")
	if float(gov_before.get("cash", 0.0)) != 0.0:
		_fail("caixa da prefeitura deveria comecar em 0")

	for day in range(1, CHECK_DAY):
		_tick(sim, "A dia %d" % day)
	var cash_before: float = float(_gov(sim, TEST_CITY).get("gov", {}).get("cash", 0.0))
	var area_before: float = _sum_area(_gov(sim, TEST_CITY))

	# Fechamento do mês (dia CHECK_DAY).
	_tick(sim, "A dia %d (fechamento)" % CHECK_DAY)
	var urban_after: Dictionary = _gov(sim, TEST_CITY)
	var gov: Dictionary = urban_after.get("gov", {})
	var cash_after := float(gov.get("cash", 0.0))
	var iptu := float(gov.get("month_revenue_iptu", 0.0))
	var fees := float(gov.get("month_revenue_fees", 0.0))
	var transfer := float(gov.get("month_revenue_transfer", 0.0))
	var maintenance := float(gov.get("month_expense_maintenance", 0.0))
	var infra := float(gov.get("month_expense_infrastructure", 0.0))
	var social := float(gov.get("month_expense_social_housing", 0.0))
	var target := float(gov.get("target_revenue_month", 0.0))

	if cash_after < -TOLERANCE:
		_fail("caixa negativo no fechamento: %.6f" % cash_after)
	if target <= 0.0:
		_fail("alvo de receita municipal zerado (receita_pct_gdp nao carregou)")
	if iptu <= 0.0:
		_fail("IPTU nao foi cobrado no fechamento (%.6f)" % iptu)
	if transfer <= 0.0:
		_fail("repasse do tesouro nao chegou (%.6f)" % transfer)
	if maintenance <= 0.0:
		_fail("manutencao urbana nao foi paga (%.6f)" % maintenance)

	# Receitas = soma dos fluxos: o caixa do mês fecha com as três receitas.
	var inflow := iptu + fees + transfer
	var outflow := maintenance + infra + social
	if not _close(cash_after - cash_before, inflow - outflow, 1.0e-9):
		_fail("caixa do mes nao fecha: delta=%.6f receitas=%.6f despesas=%.6f" % [
			cash_after - cash_before, inflow, outflow])
	if not _close(iptu, float(gov.get("month_revenue_iptu", 0.0))):
		_fail("rubrica de IPTU inconsistente")

	# Repasse sai do tesouro: recebido == pago dentro da nação.
	var received := _received_transfer(sim, nation_cities)
	var paid := _paid_transfer(sim, nation_cities)
	if received <= 0.0:
		_fail("nenhum repasse recebido na nacao de teste")
	if not _close(received, paid, 1.0e-9):
		_fail("repasse recebido (%.6f) != pago pelo tesouro (%.6f)" % [
			received, paid])
	if not _close(transfer, float(gov.get("month_revenue_transfer", 0.0))):
		_fail("repasse da cidade de teste inconsistente")

	# Decisão: zona lotada + gabarito alto (sem adensar) => expandir.
	var projects: Array = gov.get("projects", [])
	if projects.is_empty():
		_fail("prefeitura nao decidiu obra com a zona lotada")
	else:
		var project: Dictionary = projects[0]
		if int(project.get("kind", -1)) != EXPAND_KIND:
			_fail("obra decidida nao foi expansao (kind=%s)" % [
				str(project.get("kind"))])
		if float(project.get("area_km2", 0.0)) <= 0.0:
			_fail("expansao sem area")
		if not _close(float(project.get("reserved", 0.0)),
				float(project.get("cost", 0.0)), 1.0e-9):
			_fail("fundo reservado (%.3f) != custo da obra (%.3f)" % [
				float(project.get("reserved", 0.0)),
				float(project.get("cost", 0.0))])
		if not _close(infra, float(project.get("cost", 0.0))):
			_fail("despesa de infraestrutura (%.3f) != custo da obra (%.3f)" % [
				infra, float(project.get("cost", 0.0))])
	print("[city_gov] mes %d: iptu=%.1f taxas=%.1f repasse=%.1f manutencao=%.1f "
		% [CHECK_DAY, iptu, fees, transfer, maintenance]
		+ "obra=%.1f alvo=%.1f caixa=%.1f" % [infra, target, cash_after])

	# --- Save/load com obra em andamento: 0 diferenças ----------------------
	for day in range(CHECK_DAY, SAVE_DAY):
		_tick(sim, "A dia %d" % (day + 1))
	var snapshot: Dictionary = sim.demo_to_dict()
	if int(snapshot.get("version", 0)) != 13:
		_fail("save version %s != 13" % str(snapshot.get("version")))
	if not snapshot.has("city_gov"):
		_fail("save sem o bloco city_gov da prefeitura")
	var restored = EconomySim.new()
	_load_chains(restored)
	restored.demo_from_dict(snapshot)
	var resaved: Dictionary = restored.demo_to_dict()
	if _canonical(resaved) != _canonical(snapshot):
		_fail("round-trip do save divergiu com obra em andamento")
	var restored_gov: Dictionary = _gov(restored, TEST_CITY).get("gov", {})
	if Array(restored_gov.get("projects", [])).size() \
			!= Array(gov.get("projects", [])).size():
		_fail("obra nao voltou do save")
	print("[city_gov] save/load com obra em andamento: %s" % (
		"0 diffs" if _failures.is_empty() else "com falhas"))

	# --- Determinismo: threads=1 e threads=8 -------------------------------
	var other = _make_world(8, urban)
	for day in range(1, SAVE_DAY + 1):
		_tick(other, "B(t8) dia %d" % day)
	if _canonical(other.demo_to_dict()) != _canonical(snapshot):
		_fail("threads=8 divergiu do save de threads=1 no dia %d" % SAVE_DAY)
	print("[city_gov] determinismo threads=1 vs 8 no dia %d: %s" % [
		SAVE_DAY, "identico" if _failures.is_empty() else "com falhas"])

	# --- Obra concluída: área e infraestrutura crescem ----------------------
	var infra_before := float(urban_after.get("infra_capacity", 0.0))
	var reserve_before := float(urban_after.get("reserve_km2", 0.0))
	for day in range(SAVE_DAY, LAST_DAY):
		_tick(sim, "A dia %d" % (day + 1))
	var urban_end: Dictionary = _gov(sim, TEST_CITY)
	var gov_end: Dictionary = urban_end.get("gov", {})
	if float(gov_end.get("cash", 0.0)) < -TOLERANCE:
		_fail("caixa negativo no fim: %.6f" % float(gov_end.get("cash", 0.0)))
	var area_end: float = _sum_area(urban_end)
	# A prefeitura contrata a área que o caixa paga (sem dívida): a obra é
	# parcial, mas tem de abrir solo de verdade e consumir reserva.
	if not (area_end > area_before + 1.0e-6):
		_fail("expansao nao abriu area: antes=%.3f depois=%.3f" % [
			area_before, area_end])
	if not (float(urban_end.get("reserve_km2", 0.0)) < reserve_before - 1.0e-9):
		_fail("expansao nao consumiu reserva de solo: antes=%.3f depois=%.3f" % [
			reserve_before, float(urban_end.get("reserve_km2", 0.0))])
	if not (float(urban_end.get("infra_capacity", 0.0)) > infra_before):
		_fail("capacidade de infraestrutura nao cresceu: antes=%.1f depois=%.1f" % [
			infra_before, float(urban_end.get("infra_capacity", 0.0))])
	print("[city_gov] dia %d: area=%.3f km2 (antes %.3f) reserva=%.3f infra=%.0f caixa=%.1f" % [
		LAST_DAY, area_end, area_before,
		float(urban_end.get("reserve_km2", 0.0)),
		float(urban_end.get("infra_capacity", 0.0)),
		float(gov_end.get("cash", 0.0))])


func _sum_area(urban: Dictionary) -> float:
	var total := 0.0
	for value in urban.get("area_km2", []):
		total += float(value)
	return total
