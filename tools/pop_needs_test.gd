extends SceneTree

## Teste headless da F3: necessidades das POPs com substituição, níveis por
## renda, duráveis com reposição anual e consumo institucional do governo.
##
## Instancia só `EconomySim` (sem autoload) → runner `--script`:
##   godot --headless --path . --script res://tools/pop_needs_test.gd
##
## Cenário controlado: uma cidade sem construções. Os estoques de mercado e a
## poupança/governo são injetados por `to_dict()`/`from_dict()` (o save
## é o contrato público do estado), então o teste mede exatamente a regra da
## fase 5 — sem depender de produção, frete ou IA.

const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"

const CITY_ID := "NEEDCITY"
const POPULATION := 100000.0
const MONEY_EPSILON := 0.5
const EPSILON := 1e-6

var _failures: Array[String] = []
var _goods: Array = []
var _recipes: Array = []
var _needs: Dictionary = {}
var _index: Dictionary = {}


func _init() -> void:
	_goods = _load_array(GOODS_PATH)
	_recipes = _load_array(RECIPES_PATH)
	_needs = _load_dict(NEEDS_PATH)
	if _goods.is_empty() or _recipes.is_empty() or _needs.is_empty():
		_fail("dados das cadeias/necessidades não carregaram")
		_finish()
		return
	for entry in _goods:
		_index[String((entry as Dictionary).get("id", ""))] = int(
			(entry as Dictionary).get("order", 0))

	_test_substitution("bread")
	_test_substitution("rice")
	_test_levels_by_income()
	_test_durable_decay()
	_test_government_consumption()
	_test_demand_signal()
	_finish()


func _finish() -> void:
	if _failures.is_empty():
		print("[pop_needs] OK: substituição, níveis, duráveis e governo validados")
		quit(0)
		return
	print("[pop_needs] FALHAS (%d):" % _failures.size())
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


func _load_array(path: String) -> Array:
	var data: Variant = _load_json(path)
	return data if typeof(data) == TYPE_ARRAY else []


func _load_dict(path: String) -> Dictionary:
	var data: Variant = _load_json(path)
	return data if typeof(data) == TYPE_DICTIONARY else {}


func _product(id: String) -> int:
	return int(_index.get(id, -1))


## Cria um mundo de uma cidade com os dados reais das cadeias/necessidades.
func _new_sim() -> Object:
	var sim := EconomySim.new()
	sim.set_goods(_goods)
	sim.set_recipes(_recipes)
	sim.set_needs(_needs)
	# O teste mede a REGRA do nível de consumo, não a calibração da folga de
	# custo de vida do jogo: com folga 1,0 a renda injetada define o nível
	# diretamente (a folga calibrada vive em income_basket_margin_).
	sim.set_balance({"income_basket_margin": 1.0})
	var city := {
		"id": CITY_ID,
		"name": "Need City",
		"nation": "TST",
		"home_nation": "TST",
		"landmass": "TESTLAND",
		"population": POPULATION,
		"sim_population": POPULATION,
		"x": 100.0,
		"y": 100.0,
	}
	sim.initialize_world([city], [], [])
	return sim


## Aplica um estado controlado via save/load e devolve o sim pronto.
##
## F7 (contrato novo): o nível de consumo da POP é definido pela RENDA CORRENTE
## do estrato, não pela poupança — "consumo efetivo respeita renda". Por isso o
## teste que mede renda alta injeta renda de verdade: vagas no setor de serviços
## (`service_share`) pagas por `service_cash` e salários por nível
## (`wage_levels`). A poupança continua sendo injetada para o orçamento de compra
## (o dinheiro é conservado), mas sozinha não eleva o nível.
func _inject(sim: Object, stocks: Dictionary, savings: float,
		government: float, durable: Dictionary = {},
		service_share: float = 0.0, wage_levels: Array = []) -> void:
	var save: Dictionary = sim.to_dict()
	var city: Dictionary = save["cities"][0]
	city["external_income_index"] = 0.0
	city["government_budget"] = government
	city["firm_cash"] = 0.0
	city["service_cash"] = 0.0
	# Sem setor de serviços: o teste isola a renda disponível que ele injeta.
	city["service_jobs"] = 0.0
	city["service_wage"] = 0.0
	city["capital_income_next"] = 0.0
	city["pop"]["savings"] = savings
	city["pop"]["wage_income"] = 0.0
	city["pop"]["capital_income"] = 0.0
	if service_share > 0.0:
		# Renda corrente real: o setor de serviços emprega e paga os salários
		# do mercado de trabalho (mesma regra do jogo, sem atalho de caixa).
		city["service_jobs"] = POPULATION * service_share
		city["service_cash"] = 100000000.0
		if not wage_levels.is_empty() and city["pop"].has("labor"):
			city["pop"]["labor"]["wage_by_level"] = wage_levels.duplicate()
	for id in stocks:
		var product := _product(String(id))
		if product < 0:
			_fail("bem desconhecido no estoque: " + String(id))
			continue
		city["markets"][product]["stock"] = float(stocks[id])
	# Estado de duráveis injetado pelo save (contrato da F3).
	if not city["pop"].has("needs"):
		city["pop"]["needs"] = {}
	city["pop"]["needs"]["durable_stock"] = durable.duplicate(true)
	sim.from_dict(save)


## Mercado e POP são detalhe pesado: o status diário publica só os escalares
## leves, então o bloco completo vem sob demanda pelo id da cidade.
func _city_heavy(sim: Object) -> Dictionary:
	return sim.get_city_status(CITY_ID)


func _market(sim: Object, id: String) -> Dictionary:
	var product := _product(id)
	var markets: Array = _city_heavy(sim).get("markets", [])
	if product < 0 or product >= markets.size():
		return {}
	return markets[product]


func _pop(sim: Object) -> Dictionary:
	return _city_heavy(sim).get("pop", {})


func _consumed_today(status: Dictionary, id: String) -> float:
	var product := _product(id)
	var values: Array = status.get("consumed_today", [])
	return float(values[product]) if product >= 0 and product < values.size() else 0.0


func _check_conservation(sim: Object, status: Dictionary, money_initial: float,
		label: String) -> void:
	var money := float(status.get("money_total", 0.0))
	if absf(money - money_initial) > MONEY_EPSILON:
		_fail("%s: dinheiro %.2f != %.2f" % [label, money, money_initial])
	for market in _city_heavy(sim).get("markets", []):
		if float(market["stock"]) < -1e-9:
			_fail("%s: estoque negativo (%.4f)" % [label, float(market["stock"])])
			break


## Só um alimento substituto disponível precisa manter a satisfação básica.
func _test_substitution(only: String) -> void:
	var sim := _new_sim()
	var stocks := {only: 1000.0}
	# Orçamento próprio folgado: o teste mede substituição, não renda.
	_inject(sim, stocks, 100000.0, 1000000.0)
	var before: Dictionary = sim.get_status()
	var money := float(before["money_total"])
	var status: Dictionary = sim.tick_day()
	_check_conservation(sim, status, money, "substituição " + only)
	var pop := _pop(sim)
	var need := float(pop["need"])
	var consumed := float(pop["consumed"])
	if need <= 0.0 or consumed < need * 0.95:
		_fail("substituição %s: satisfação básica %.3f (consumido %.3f/%.3f)" % [
			only, consumed / maxf(need, 1e-9), consumed, need])
	if _consumed_today(status, only) <= 0.0:
		_fail("substituição %s: nenhum consumo do substituto disponível" % only)
	var other := "rice" if only == "bread" else "bread"
	if _consumed_today(status, other) > 0.0:
		_fail("substituição %s: consumiu %s sem estoque" % [only, other])
	print("[pop_needs] substituição %-5s: satisfação %.3f (%.0f%%)" % [
		only, consumed / maxf(need, 1e-9), 100.0 * consumed / maxf(need, 1e-9)])


## Renda disponível define o nível alcançado: basic → life → luxury.
func _test_levels_by_income() -> void:
	var stocks := {
		"bread": 1000.0, "clothes": 100.0, "meat": 100.0, "canned_food": 100.0,
		"shoes": 100.0, "medicine": 100.0, "furniture": 100.0, "phone": 100.0,
		"alcohol": 100.0, "fuel": 100.0, "jewelry": 100.0, "computer": 100.0,
		"car": 100.0, "luxury_furniture": 100.0, "drone": 100.0,
		"premium_spirits": 100.0,
	}
	var low := _new_sim()
	# Governo com 1M (provisão pública de serviços) e POP sem emprego nem
	# poupança injetada: resta à POP o que o governo/serviços cobrem. A cesta
	# alimentar vem do estoque de carne (o alimento mais barato por caloria).
	_inject(low, {"meat": 1000.0}, 0.0, 1000000.0)
	# O nível de renda calculado no dia vale para o dia SEGUINTE (contrato do
	# núcleo), então o teste roda dois dias: o primeiro ancora a renda, o
	# segundo consome conforme ela.
	low.tick_day()
	var low_status: Dictionary = low.tick_day()
	var low_pop := _pop(low)
	if int(low_pop["needs_level"]) != 1:
		_fail("renda baixa: nível %d (esperado 1 — só basic)" % int(
			low_pop["needs_level"]))
	# B2 (D-083): o consumo é por QUINTIL e limitado pelo orçamento de cada um.
	# `income_strata.level` é o TETO que o estrato pode pagar, não o que ele
	# consumiu; o teste confere que as cinco faixas existem no status e mede a
	# satisfação EFETIVA: sem bens de life/luxury no estoque, o agregado fica
	# perto da parcela coberta por provisão pública/serviços (medido 0,20 da
	# B7), nunca no padrão de renda alta (1,0 com estoque e renda).
	var low_levels: Array = (low_pop.get("income_strata", {}) as Dictionary).get(
			"level", [])
	if low_levels.size() != 5:
		_fail("renda baixa: status sem income_strata.level[5]")
	if float(low_pop["life_satisfaction"]) > 0.25:
		_fail("renda baixa: satisfação life %.3f (esperado <= 0.25, só o topo)" % float(
			low_pop["life_satisfaction"]))
	if float(low_pop["luxury_satisfaction"]) > 0.01:
		_fail("renda baixa: satisfação luxury %.3f sem orçamento" % float(
			low_pop["luxury_satisfaction"]))

	var high := _new_sim()
	_inject(high, stocks, 100000000.0, 1000000.0, {}, 0.30,
		[0.30, 0.35, 0.40])
	high.tick_day()
	var high_status: Dictionary = high.tick_day()
	var high_pop := _pop(high)
	if int(high_pop["needs_level"]) < 2:
		_fail("renda alta: nível %d (esperado >= 2)" % int(
			high_pop["needs_level"]))
	var high_levels: Array = (high_pop.get("income_strata", {}) as Dictionary).get(
			"level", [])
	if high_levels.size() != 5:
		_fail("renda alta: status sem income_strata.level[5]")
	if float(high_pop["life_satisfaction"]) <= 0.5:
		_fail("renda alta: satisfação life %.3f baixa com estoque e renda" % float(
			high_pop["life_satisfaction"]))
	if float(high_pop["needs_satisfaction"]) <= float(low_pop["needs_satisfaction"]):
		_fail("renda alta: satisfação agregada %.3f <= renda baixa %.3f" % [
			float(high_pop["needs_satisfaction"]),
			float(low_pop["needs_satisfaction"])])
	print("[pop_needs] níveis: baixa=%d (agregada %.2f) alta=%d (agregada %.2f)" % [
		int(low_pop["needs_level"]), float(low_pop["needs_satisfaction"]),
		int(high_pop["needs_level"]), float(high_pop["needs_satisfaction"])])
	print("[pop_needs] baixa: disp=%.1f sav=%.1f food=%.1f needs=%.1f basic=%.2f life=%.2f" % [
		float(low_pop["disposable"]), float(low_pop["savings"]),
		float(low_pop["food_spend"]), float(low_pop["needs_spend"]),
		float(low_pop["basic_satisfaction"]), float(low_pop["life_satisfaction"])])
	print("[pop_needs] teto por quintil: baixa=%s alta=%s | luxury baixa=%.2f alta=%.2f" % [
		str(low_levels), str(high_levels),
		float(low_pop["luxury_satisfaction"]),
		float(high_pop["luxury_satisfaction"])])


## 365 dias sem compras: o estoque durável perde exatamente 10%.
func _test_durable_decay() -> void:
	var sim := _new_sim()
	var durable := {"furniture": 100.0}
	_inject(sim, {}, 0.0, 0.0, durable)
	var before: Dictionary = sim.get_status()
	var money := float(before["money_total"])
	var initial := float(_pop(sim)["durable_stock"])
	if absf(initial - 100.0) > 0.01:
		_fail("duráveis: estoque inicial %.3f (esperado 100)" % initial)
	for day in 365:
		var status: Dictionary = sim.tick_day()
		if day % 90 == 0:
			_check_conservation(sim, status, money, "duráveis dia %d" % (day + 1))
	var after: Dictionary = sim.get_status()
	_check_conservation(sim, after, money, "duráveis 365 d")
	var remaining := float(_pop(sim)["durable_stock"])
	if absf(remaining - 90.0) > 0.01:
		_fail("duráveis: 365 d deixaram %.4f t (esperado 90, perda de 10%%)" % remaining)
	print("[pop_needs] duráveis: %.1f t -> %.3f t em 365 d (perda %.2f%%)" % [
		initial, remaining, 100.0 * (initial - remaining) / maxf(initial, 1e-9)])


## O Estado compra paper e computer pelo orçamento público (dinheiro conservado).
func _test_government_consumption() -> void:
	var sim := _new_sim()
	_inject(sim, {"paper": 1000.0, "computer": 1000.0}, 0.0, 1000000.0)
	var before: Dictionary = sim.get_status()
	var money := float(before["money_total"])
	var gov_before := float((before["cities"][0] as Dictionary)["government_budget"])
	var firm_before := float((before["cities"][0] as Dictionary)["firm_cash"])
	var status: Dictionary = sim.tick_day()
	_check_conservation(sim, status, money, "governo")
	var city: Dictionary = status["cities"][0]
	var gov_spend := float(city.get("government_goods_spend", 0.0))
	if gov_spend <= 0.0:
		_fail("governo: gasto institucional não registrado")
	if _consumed_today(status, "paper") <= 0.0:
		_fail("governo: não consumiu paper")
	if _consumed_today(status, "computer") <= 0.0:
		_fail("governo: não consumiu computer")
	if float(city["government_budget"]) >= gov_before:
		_fail("governo: orçamento não diminuiu (%.2f -> %.2f)" % [
			gov_before, float(city["government_budget"])])
	if float(city["firm_cash"]) <= firm_before:
		_fail("governo: firma não recebeu o pagamento")
	var computer_market := _market(sim, "computer")
	print("[pop_needs] governo: gasto %.2f | paper %.3f t | computer %.3f t (sold %.3f, desired %.3f)" % [
		gov_spend, _consumed_today(status, "paper"), _consumed_today(status, "computer"),
		float(computer_market.get("sold", 0.0)), float(computer_market.get("desired", 0.0))])


## Demanda de bem final vira demanda de mercado mesmo sem estoque (sinal p/ IA).
func _test_demand_signal() -> void:
	var sim := _new_sim()
	# Poupança acima do colchão alimentar (10 anos de cesta) para a POP ter
	# orçamento discricionário e registrar a demanda desejada.
	_inject(sim, {"bread": 1000.0}, 100000000.0, 1000000.0)
	var status: Dictionary = sim.tick_day()
	for id in ["clothes", "medicine", "phone", "car"]:
		var market := _market(sim, id)
		if float(market.get("desired", 0.0)) <= 0.0:
			_fail("demanda: %s sem demanda desejada (IA não veria escassez)" % id)
		if float(market.get("shortage", 0.0)) <= 0.0:
			_fail("demanda: %s sem shortage com estoque zero" % id)
	print("[pop_needs] demanda: clothes desired=%.2f shortage=%.2f" % [
		float(_market(sim, "clothes").get("desired", 0.0)),
		float(_market(sim, "clothes").get("shortage", 0.0))])
