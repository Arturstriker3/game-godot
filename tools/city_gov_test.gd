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

	# Decisão (INTEGRADA): zona lotada + gabarito alto (sem adensar) => expandir.
	# A obra agora é um projeto §15 (`construction_projects`) com marcador
	# urbano, pago pelo caixa da prefeitura — o `gov.projects` local do W2 fica
	# só para ler saves antigos.
	var projects: Array = _urban_projects(sim, TEST_CITY)
	if projects.is_empty():
		_fail("prefeitura nao decidiu obra com a zona lotada")
	else:
		var project: Dictionary = projects[0]
		if String(project.get("city", "")) != TEST_CITY:
			_fail("obra decidida em outra cidade (%s)" % String(
				project.get("city", "")))
		if int(project.get("urban_action", -1)) != 1:  # URBAN_EXPAND
			_fail("obra decidida nao foi expansao (acao=%s)" % [
				str(project.get("urban_action"))])
		if float(project.get("urban_km2", 0.0)) <= 0.0:
			_fail("expansao sem area")
		if float(project.get("total_cost", 0.0)) <= 0.0:
			_fail("expansao sem custo")
		if String(project.get("owner", "")) != "state":
			_fail("obra urbana sem dono estatal (%s)" % String(
				project.get("owner", "")))
		if not _close(infra, float(project.get("total_cost", 0.0))):
			_fail("despesa de infraestrutura (%.3f) != custo da obra (%.3f)" % [
				infra, float(project.get("total_cost", 0.0))])
	print("[city_gov] mes %d: iptu=%.1f taxas=%.1f repasse=%.1f manutencao=%.1f "
		% [CHECK_DAY, iptu, fees, transfer, maintenance]
		+ "obra=%.1f alvo=%.1f caixa=%.1f" % [infra, target, cash_after])

	# --- Save/load com obra em andamento: 0 diferenças ----------------------
	var failures_before_save := _failures.size()
	for day in range(CHECK_DAY, SAVE_DAY):
		_tick(sim, "A dia %d" % (day + 1))
	var snapshot: Dictionary = sim.demo_to_dict()
	if int(snapshot.get("version", 0)) != 14:
		_fail("save version %s != 14" % str(snapshot.get("version")))
	if not snapshot.has("city_gov"):
		_fail("save sem o bloco city_gov da prefeitura")
	var restored = EconomySim.new()
	_load_chains(restored)
	restored.demo_from_dict(snapshot)
	var resaved: Dictionary = restored.demo_to_dict()
	if _canonical(resaved) != _canonical(snapshot):
		_fail("round-trip do save divergiu com obra em andamento")
	var restored_gov: Dictionary = _gov(restored, TEST_CITY).get("gov", {})
	if Array(_urban_projects(restored, TEST_CITY)).size() != projects.size():
		_fail("obra nao voltou do save")
	print("[city_gov] save/load com obra em andamento: %s" % (
		"0 diffs" if _failures.size() == failures_before_save else "com falhas"))

	# --- Determinismo: threads=1 e threads=8 -------------------------------
	var failures_before_det := _failures.size()
	var other = _make_world(8, urban)
	for day in range(1, SAVE_DAY + 1):
		_tick(other, "B(t8) dia %d" % day)
	if _canonical(other.demo_to_dict()) != _canonical(snapshot):
		_fail("threads=8 divergiu do save de threads=1 no dia %d" % SAVE_DAY)
	print("[city_gov] determinismo threads=1 vs 8 no dia %d: %s" % [
		SAVE_DAY, "identico" if _failures.size() == failures_before_det
		else "com falhas"])

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

	# --- Custo crescente: 1 km² = 2 meses de receita municipal (spec §19). ---
	# No dia CHECK_DAY a cidade ainda tem o raio/reserva do carregamento, então
	# os fatores de distância e relevo valem 1 e o custo por km² é exatamente
	# kDemoUrbanInfraCostMonths × alvo do mês (conta documentada em
	# sim_constants.h: 12 meses × 55% ÷ 2 = 3,3 km²/ano).
	var cost_km2 := float(urban_after.get("expansion_cost_per_km2", 0.0))
	if not _close(cost_km2, 2.0 * target, 1.0e-9):
		_fail("custo/km² (%.1f) != 2 meses de receita alvo (%.1f)" % [
			cost_km2, 2.0 * target])
	print("[city_gov] custo de 1 km2 = %.0f XGS (2 meses do alvo %.0f)" % [
		cost_km2, target])

	# --- Moradia social: obra do W3 com dono GOVERNMENT, paga pelo caixa. ---
	_test_social_housing(urban)


## §19 (integração W2→W3): a prefeitura financia moradia social pelo projeto
## de moradia do W3; a entrega soma unidades e a posse vai para o governo.
func _test_social_housing(urban: Dictionary) -> void:
	var sim = _make_world(1, urban)
	for day in range(1, MONTH_DAYS + 1):
		_tick(sim, "S dia %d" % day)
	var cash := 5000000.0
	_grant_gov_cash(sim, TEST_CITY, cash)
	var before: Dictionary = _gov(sim, TEST_CITY)
	var owner_before := float(Array(before.get("owner_share", []))[2])
	var stock_before := 0.0
	for row in before.get("units", []):
		for value in row:
			stock_before += float(value)
	var result: Dictionary = sim.demo_urban_social_housing(TEST_CITY, 40.0)
	if not bool(result.get("ok", false)):
		_fail("moradia social nao abriu")
		return
	# A obra do governo entra na FILA ÚNICA (§15) com kind 1 (moradia) e dono 3.
	if _gov_housing_projects(sim.demo_to_dict()) != 1:
		_fail("moradia social: esperava 1 obra do governo na fila, veio %d" % [
			_gov_housing_projects(sim.demo_to_dict())])
	var cash_after_open := float(_gov(sim, TEST_CITY).get("gov", {}).get("cash", 0.0))
	var paid := float(result.get("cost", 0.0))
	if paid <= 0.0 or not _close(cash, cash_after_open + paid, 1.0e-6):
		_fail("moradia social: caixa %.1f -> %.1f nao fecha com custo %.1f" % [
			cash, cash_after_open, paid])
	for day in range(MONTH_DAYS + 1, MONTH_DAYS + 95):
		_tick(sim, "S dia %d" % day)
	var after: Dictionary = _gov(sim, TEST_CITY)
	var snapshot_after: Dictionary = sim.demo_to_dict()
	if _gov_housing_projects(snapshot_after) != 0:
		_fail("moradia social: obra do governo ainda em andamento depois do prazo")
	var owner_after := float(Array(after.get("owner_share", []))[2])
	# A posse do governo sobe pela proporção das unidades entregues no estoque
	# (tolerância de 2× para a depreciação diária do período).
	var expected_share := float(result.get("units", 0.0)) \
		/ maxf(1.0, stock_before)
	if owner_after - owner_before < 0.5 * expected_share:
		_fail("moradia social: posse do governo subiu %.8f, esperado ~%.8f" % [
			owner_after - owner_before, expected_share])
	print("[city_gov] moradia social: %.1f unidades, %.0f XGS do caixa, posse do governo %.6f -> %.6f (esperado +%.6f)" % [
		float(result.get("units", 0.0)), paid, owner_before, owner_after,
		expected_share])


## Injeta caixa na prefeitura pelo bloco urbano do save (teste, sem gambiarra
## de estado interno): o save do W0/W2 carrega `gov.cash` por cidade.
func _grant_gov_cash(sim, city_id: String, cash: float) -> void:
	var snapshot: Dictionary = sim.demo_to_dict()
	for entry in snapshot.get("urban", []):
		if String(entry.get("city", "")) == city_id:
			var gov: Dictionary = entry.get("gov", {})
			gov["cash"] = cash
			entry["gov"] = gov
	sim.demo_from_dict(snapshot)


func _sum_area(urban: Dictionary) -> float:
	var total := 0.0
	for value in urban.get("area_km2", []):
		total += float(value)
	return total


## Obras urbanas em andamento = projetos §15 com marcador de infraestrutura.
## `city_id` filtra (a prefeitura de TODA cidade cheia decide no mesmo mês).
func _urban_projects(sim, city_id: String = "") -> Array:
	var result: Array = []
	for project in sim.demo_to_dict().get("construction_projects", []):
		if int(project.get("urban_action", 0)) == 0:
			continue
		if city_id != "" and String(project.get("city", "")) != city_id:
			continue
		result.append(project)
	return result


## Obras de MORADIA do governo nesta cidade: kind 1 (HOUSING) na fila única §15
## com `housing_owner` 3 (GOVERNMENT). Lê o snapshot já serializado.
func _gov_housing_projects(snapshot: Dictionary) -> int:
	var count := 0
	for project in snapshot.get("construction_projects", []):
		if String(project.get("city", "")) != TEST_CITY:
			continue
		if int(project.get("kind", 0)) == 1 \
				and int(project.get("housing_owner", 0)) == 3:
			count += 1
	return count
