extends SceneTree

## F9 — teste de aceite de investimento e propriedade (docs §14).
##
## Mundo sintético determinístico com duas nações (AAA/BBB), uma cidade e uma
## fazenda de trigo cada. Exercita, contra o núcleo C++ real:
##   1. grupo investe onde o retorno compensa (e não onde há excesso);
##   2. IDE evita tarifa (ativo estrangeiro no anfitrião);
##   3. setor fechado bloqueia o investimento estrangeiro;
##   4. joint venture respeita o % mínimo local;
##   5. privatização transfere ativo E caixa;
##   6. nacionalização transfere o ativo e derruba a confiança/IDE;
##   7. dinheiro conservado nas operações e ao longo dos dias;
##   8. nomes de grupo únicos e não vazios.
##
##   godot --headless --path . --script res://tools/investment_test.gd

const GoodsPath := "res://data/definitions/economy/goods.json"
const RecipesPath := "res://data/definitions/economy/recipes.json"
const NeedsPath := "res://data/definitions/economy/needs.json"
const GroupInvestorScript := preload("res://core/economy/ai/group_investor.gd")
var _failures: Array[String] = []


func _init() -> void:
	_test_invest_where_return_pays()
	_test_fdi_avoids_tariff()
	_test_closed_policy_blocks()
	_test_joint_venture_share()
	_test_privatization_transfers_cash()
	_test_nationalization_drops_fdi()
	_test_money_conserved_over_days()
	if _failures.is_empty():
		print("[invest] OK: investimento, IDE, política, privatização e nacionalização.")
		quit(0)
	else:
		print("[invest] FALHAS (%d):" % _failures.size())
		for failure in _failures:
			print(" - " + failure)
		quit(1)


func _fail(message: String) -> void:
	_failures.append(message)


func _load_json(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("não abriu " + path)
		return null
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data


func _make_world() -> Dictionary:
	var sim = EconomySim.new()
	sim.set_goods(_load_json(GoodsPath))
	sim.set_recipes(_load_json(RecipesPath))
	sim.set_needs(_load_json(NeedsPath))
	# §15: o teste mede propriedade/IDE, não cronograma de obra — construção
	# instantânea para o ativo nascer na hora.
	sim.set_instant_construction(true)
	var cities := [
		{"id": "A1", "name": "Alfa", "nation": "AAA", "home_nation": "AAA",
			"population": 900000.0, "sim_population": 500000.0, "x": 1200.0,
			"y": 800.0, "landmass": "L1"},
		{"id": "B1", "name": "Beta", "nation": "BBB", "home_nation": "BBB",
			"population": 900000.0, "sim_population": 500000.0, "x": 1600.0,
			"y": 800.0, "landmass": "L1"},
	]
	var buildings := [
		{"id": "FA", "city_id": "A1", "resource": "wheat",
			"output_tons_per_day": 50.0, "aptitude": 1.0},
		{"id": "FB", "city_id": "B1", "resource": "wheat",
			"output_tons_per_day": 50.0, "aptitude": 1.0},
	]
	sim.initialize_world(cities, [], buildings)
	sim.set_suitability("A1", "wheat", 1.0)
	sim.set_suitability("B1", "wheat", 1.0)
	var groups: Array = sim.get_groups()
	var group_of := {}
	for group: Dictionary in groups:
		group_of[String(group["origin_nation"])] = String(group["id"])
	if not group_of.has("AAA") or not group_of.has("BBB"):
		_fail("semeadura: grupos AAA/BBB não foram criados")
	return {"sim": sim, "groups": group_of}


func _group(sim: Object, origin: String) -> Dictionary:
	for group: Dictionary in sim.get_groups():
		if String(group.get("origin_nation", "")) == origin:
			return group
	return {}


## Financia o grupo pela origem (nunca por índice: o ranking reordena) e
## devolve o id efetivamente creditado.
func _fund(sim: Object, origin: String, amount: float) -> String:
	var group_id := String(_group(sim, origin).get("id", ""))
	if group_id.is_empty():
		_fail("financiamento: sem grupo de origem %s" % origin)
		return ""
	var result: Dictionary = sim.fund_group(group_id, amount)
	if not bool(result.get("funded", false)):
		_fail("financiamento: %s não recebeu %.0f (%s)" % [
			group_id, amount, String(result.get("error", ""))])
		return ""
	return group_id


## Roda a decisão do grupo. A cadência da IA é por índice no ranking do dia
## (core/economy/ai/group_investor.gd), então o teste lê a posição atual e
## chama `run_day` num dia que caia na vez dele; sem tick entre a leitura e a
## decisão, o índice é o mesmo que a IA enxerga.
func _decide_group(investor: Object, sim: Object, group_id: String) -> bool:
	var groups: Array = sim.get_groups()
	for index in groups.size():
		if String((groups[index] as Dictionary).get("id", "")) != group_id:
			continue
		investor.run_day(GroupInvestorScript.INTERVAL_DAYS
			+ index % GroupInvestorScript.INTERVAL_DAYS, sim.get_status())
		return true
	_fail("decisão: grupo %s não aparece no ranking" % group_id)
	return false


func _city_status(status: Dictionary, city_id: String) -> Dictionary:
	for city: Dictionary in status.get("cities", []):
		if String(city.get("id", "")) == city_id:
			return city
	return {}


func _buildings(sim: Object, city_id: String) -> Array:
	# Construções são detalhe pesado: o status diário publica só os escalares
	# leves, então o bloco completo vem sob demanda pelo id da cidade.
	return sim.get_city_status(city_id).get("buildings", [])


func _money(sim: Object) -> float:
	return float(sim.get_status().get("money_total", 0.0))


func _test_invest_where_return_pays() -> void:
	var world := _make_world()
	var sim = world["sim"]
	var aaa: String = _fund(sim, "AAA", 200000.0)
	# A1 escasso e caro; B1 com estoque e barato (não deve atrair comida).
	sim.set_market_stock("A1", "wheat", 0.0)
	sim.set_market_stock("B1", "wheat", 4000.0)
	sim.set_market_price("A1", "wheat", 90.0)
	sim.set_market_price("B1", "wheat", 25.0)
	var status: Dictionary = sim.tick_day()
	var investor = GroupInvestorScript.new()
	investor.setup(sim, null)
	var group := _group(sim, "AAA")
	var opportunity: Dictionary = investor.best_opportunity(
		group, status.get("cities", []), status, true)
	if opportunity.is_empty():
		_fail("investimento: nenhuma oportunidade apesar do retorno alto em A1")
		return
	if String((opportunity["city"] as Dictionary).get("id", "")) != "A1":
		_fail("investimento: escolheu %s em vez de A1 (onde o retorno compensa)" % [
			String((opportunity["city"] as Dictionary).get("id", ""))])
	var cash_before := float(_group(sim, "AAA").get("cash", 0.0))
	_decide_group(investor, sim, aaa)
	var after := _group(sim, "AAA")
	var new_buildings := 0
	for building: Dictionary in _buildings(sim, "A1"):
		if String(building.get("owner_group", "")) == aaa:
			new_buildings += 1
	if new_buildings < 2:
		_fail("investimento: grupo não abriu ativo em A1 (tem %d)" % new_buildings)
		print("[invest] decisões: %s" % str(investor.get_decisions()))
	if float(after.get("cash", 0.0)) >= cash_before:
		_fail("investimento: caixa do grupo não foi debitado (%.0f -> %.0f)" % [
			cash_before, float(after.get("cash", 0.0))])
	print("[invest] investimento: %d ativo(s) do grupo em A1, caixa %.0f -> %.0f" % [
		new_buildings, cash_before, float(after.get("cash", 0.0))])


func _test_fdi_avoids_tariff() -> void:
	var world := _make_world()
	var sim = world["sim"]
	var aaa: String = _fund(sim, "AAA", 300000.0)
	# BBB tarifa 40% sobre AAA. A1 abundante; B1 escasso e caro.
	sim.set_tariff("BBB", "AAA", "*", 0.40)
	sim.set_market_stock("A1", "wheat", 5000.0)
	sim.set_market_price("A1", "wheat", 22.0)
	sim.set_market_stock("B1", "wheat", 0.0)
	sim.set_market_price("B1", "wheat", 100.0)
	# Caixa do anfitrião: BBB importa comida, mas paga a conta com exportação
	# não alimentar (mercado externo declarado). Sem isso a cidade-firma fica
	# abaixo do capital de giro e o lucro do ativo estrangeiro não sai do
	# anfitrião — o núcleo só paga dividendo do excedente de caixa (nunca
	# cria dinheiro). Cenário análogo a um exportador de commodity que atrai IDE.
	sim.set_external_demand("B1", "clothes", 400.0)
	sim.set_market_stock("B1", "clothes", 6000.0)
	sim.tick_day()
	var investor = GroupInvestorScript.new()
	investor.setup(sim, null)
	var group := _group(sim, "AAA")
	var opportunity: Dictionary = investor.best_opportunity(
		group, sim.get_status().get("cities", []), sim.get_status(), true)
	if opportunity.is_empty() or not bool(opportunity.get("foreign", false)):
		_fail("IDE: tarifa alta não levou o grupo a investir no anfitrião")
		return
	var result: Dictionary = sim.group_invest(aaa, "B1", "FDI-A-1", "wheat",
		int(opportunity["product"]), float(opportunity["capacity"]), "", "")
	if not bool(result.get("created", false)):
		_fail("IDE: núcleo recusou o investimento estrangeiro: %s" % [
			String(result.get("error", ""))])
		return
	var foreign := 0
	for building: Dictionary in _buildings(sim, "B1"):
		if String(building.get("owner_kind", "")) == "foreign":
			foreign += 1
	if foreign == 0:
		_fail("IDE: ativo criado não aparece como estrangeiro no status")
	var money_before := _money(sim)
	# Roda dias para o lucro repatriar.
	for day in 12:
		sim.tick_day()
	var investment: Dictionary = sim.get_investment_status()
	var flows: Array = investment.get("flows", [])
	var repatriated := 0.0
	for row: Dictionary in flows:
		if String(row.get("nation", "")) == "BBB":
			repatriated += float(row.get("repatriated_out", 0.0))
	if repatriated <= 0.0:
		_fail("IDE: nenhuma repatriação registrada para o anfitrião BBB")
	if absf(_money(sim) - money_before) > maxf(1.0, money_before * 1e-9):
		_fail("IDE: dinheiro não conservado (%.2f -> %.2f)" % [
			money_before, _money(sim)])
	print("[invest] IDE: ativo estrangeiro em B1, repatriação %.2f" % repatriated)


func _test_closed_policy_blocks() -> void:
	var world := _make_world()
	var sim = world["sim"]
	var aaa: String = _fund(sim, "AAA", 200000.0)
	sim.set_fdi_policy("BBB", "food", "closed")
	var result: Dictionary = sim.group_invest(aaa, "B1", "FDI-CLOSED",
		"wheat", 0, 5.0, "", "")
	if bool(result.get("created", false)):
		_fail("política: setor fechado aceitou investimento estrangeiro")
	if not String(result.get("error", "")).contains("fechado"):
		_fail("política: erro do setor fechado não é explícito (%s)" % [
			String(result.get("error", ""))])
	print("[invest] política: setor fechado recusado (%s)" % [
		String(result.get("error", ""))])


func _test_joint_venture_share() -> void:
	var world := _make_world()
	var sim = world["sim"]
	var aaa: String = _fund(sim, "AAA", 200000.0)
	var bbb: String = _fund(sim, "BBB", 100000.0)
	sim.set_fdi_policy("BBB", "food", "joint_venture", 0.30)
	var without_partner: Dictionary = sim.group_invest(aaa, "B1",
		"JV-NO-PARTNER", "wheat", 0, 5.0, "", "")
	if bool(without_partner.get("created", false)):
		_fail("JV: investimento sem parceiro local foi aceito")
	var partner_before := float(_group(sim, "BBB").get("cash", 0.0))
	var result: Dictionary = sim.group_invest(aaa, "B1", "JV-OK", "wheat", 0,
		5.0, "", bbb)
	if not bool(result.get("created", false)):
		_fail("JV: investimento com parceiro local recusado (%s)" % [
			String(result.get("error", ""))])
		return
	var partner_after := float(_group(sim, "BBB").get("cash", 0.0))
	var cost := float(result.get("cost", 0.0))
	var paid := partner_before - partner_after
	if absf(paid - cost * 0.30) > maxf(1.0, cost * 0.01):
		_fail("JV: parceiro pagou %.0f de %.0f (esperado 30%%)" % [paid, cost])
	var share := 0.0
	for building: Dictionary in _buildings(sim, "B1"):
		if String(building.get("id", "")) == "JV-OK":
			share = float(building.get("local_share", 0.0))
	if absf(share - 0.30) > 1e-6:
		_fail("JV: local_share publicado %.2f (esperado 0,30)" % share)
	print("[invest] JV: parceiro pagou %.0f (30%% de %.0f), share %.2f" % [
		paid, cost, share])


func _test_privatization_transfers_cash() -> void:
	var world := _make_world()
	var sim = world["sim"]
	var aaa: String = _fund(sim, "AAA", 200000.0)
	var state: Dictionary = sim.create_state_company("AAA", "Estatal Alfa",
		50000.0)
	if not bool(state.get("created", false)):
		_fail("privatização: estatal não foi criada")
		return
	var built: Dictionary = sim.build("A1", "STATE-A1", "wheat", -1, 0,
		10.0, 60.0, 2.0, 25.0, 0.0, "state", 20000.0)
	if not bool(built.get("created", false)):
		_fail("privatização: ativo estatal não foi criado (%s)" % [
			String(built.get("error", ""))])
		return
	var money_before := _money(sim)
	var cash_before := float(_group(sim, "AAA").get("cash", 0.0))
	var budget_before := float(_city_status(sim.get_status(), "A1").get(
		"government_budget", 0.0))
	var result: Dictionary = sim.privatize("A1", "STATE-A1", aaa)
	if not bool(result.get("ok", false)):
		_fail("privatização: recusada (%s)" % String(result.get("error", "")))
		return
	var cash_after := float(_group(sim, "AAA").get("cash", 0.0))
	var budget_after := float(_city_status(sim.get_status(), "A1").get(
		"government_budget", 0.0))
	var price := float(result.get("price", 0.0))
	if cash_before - cash_after < price - 1.0:
		_fail("privatização: comprador não pagou o preço (%.0f)" % (
			cash_before - cash_after))
	if budget_after - budget_before < price - 1.0:
		_fail("privatização: governo não recebeu o preço")
	if absf(_money(sim) - money_before) > 1.0:
		_fail("privatização: dinheiro não conservado")
	var owner_kind := ""
	for building: Dictionary in _buildings(sim, "A1"):
		if String(building.get("id", "")) == "STATE-A1":
			owner_kind = String(building.get("owner_kind", ""))
	if owner_kind != "national":
		_fail("privatização: dono publicado '%s' (esperado national)" % owner_kind)
	print("[invest] privatização: R$ %.0f, dono %s, dinheiro conservado" % [
		price, owner_kind])


func _test_nationalization_drops_fdi() -> void:
	var world := _make_world()
	var sim = world["sim"]
	var aaa: String = _fund(sim, "AAA", 200000.0)
	sim.set_fdi_policy("BBB", "food", "open")
	var built: Dictionary = sim.group_invest(aaa, "B1", "FDI-NAT", "wheat",
		0, 6.0, "", "")
	if not bool(built.get("created", false)):
		_fail("nacionalização: não foi possível criar o ativo estrangeiro (%s)" % [
			String(built.get("error", ""))])
		return
	var money_before := _money(sim)
	var result: Dictionary = sim.nationalize("B1", "FDI-NAT")
	if not bool(result.get("ok", false)):
		_fail("nacionalização: recusada (%s)" % String(result.get("error", "")))
		return
	if absf(_money(sim) - money_before) > 1.0:
		_fail("nacionalização: dinheiro não conservado")
	var owner_kind := ""
	for building: Dictionary in _buildings(sim, "B1"):
		if String(building.get("id", "")) == "FDI-NAT":
			owner_kind = String(building.get("owner_kind", ""))
	if owner_kind != "state":
		_fail("nacionalização: dono publicado '%s' (esperado state)" % owner_kind)
	var confidence := 1.0
	var second: Dictionary = sim.group_invest(aaa, "B1", "FDI-AFTER", "wheat",
		0, 5.0, "", "")
	if bool(second.get("created", false)):
		_fail("nacionalização: IDE novo foi aceito apesar da confiança derrubada")
	var investment: Dictionary = sim.get_investment_status()
	var nations: Dictionary = investment.get("nations", {})
	confidence = float((nations.get("BBB", {}) as Dictionary).get(
		"confidence", 1.0))
	if confidence >= 0.5:
		_fail("nacionalização: confiança de BBB em %.2f (esperado < 0,5)" % confidence)
	print("[invest] nacionalização: ativo estatal, confiança %.2f, IDE bloqueado (%s)" % [
		confidence, String(second.get("error", ""))])


func _test_money_conserved_over_days() -> void:
	var world := _make_world()
	var sim = world["sim"]
	var aaa: String = _fund(sim, "AAA", 200000.0)
	var bbb: String = _fund(sim, "BBB", 100000.0)
	sim.set_tariff("BBB", "AAA", "*", 0.25)
	sim.set_market_stock("B1", "wheat", 0.0)
	sim.set_market_price("B1", "wheat", 85.0)
	var start := _money(sim)
	for day in 40:
		var status: Dictionary = sim.tick_day()
		if not (status.get("invariant_violations", []) as Array).is_empty():
			_fail("conservação: invariante violado no dia %d: %s" % [
				day + 1, String(status["invariant_violations"][0])])
			return
	var end := _money(sim)
	if absf(end - start) > 1e-3:
		_fail("conservação: dinheiro %.2f -> %.2f em 40 dias" % [start, end])
	# Nomes únicos: gera para os grupos do mundo e confere.
	var investor = GroupInvestorScript.new()
	investor.setup(sim, null)
	var applied: int = investor.name_groups()
	if applied <= 0:
		_fail("nomes: nenhum nome de grupo aplicado")
	var names: Dictionary = {}
	for group: Dictionary in sim.get_groups():
		var name := String(group.get("name", ""))
		if name.is_empty():
			_fail("nomes: grupo %s sem nome" % String(group.get("id", "")))
			continue
		var key := name.to_lower()
		if names.has(key):
			_fail("nomes: duplicado '%s'" % name)
		names[key] = true
	print("[invest] conservação: %.2f -> %.2f em 40 dias | %d nomes únicos" % [
		start, end, names.size()])
