extends SceneTree

## Teste headless do cenário mínimo de duas cidades (POPs, trigo consumido
## diretamente). Executa 30 dias, confere conservação de dinheiro/população/
## estoque e aplica um choque de preço no trigo para observar queda de consumo.

const DAYS := 30
const SHOCK_DAYS := 10
const MONEY_EPSILON := 0.5
const STOCK_EPSILON := 0.001

var _failures: Array[String] = []


func _init() -> void:
	var sim = EconomySim.new()
	sim.initialize_scenario()

	var initial: Dictionary = sim.get_status()
	# Isola o choque de preço da renda externa. `kDemoExternalIncomePerCapita`
	# (0,012/pessoa/dia) é um fluxo do resto do mundo que, desde a renda por PIB
	# PPP (§20), não é gasto em serviços: em AGRIA/PORTUS ele soma mais por dia
	# do que o encarecimento da cesta sob choque de 100×, então a poupança
	# subiria por desenho — e o teste deixaria de medir o que anuncia medir.
	# A renda externa tem teste próprio (`economy_external_income_test.gd`).
	# Aqui o índice é zerado e conferido no fim: zero é explicitamente testado,
	# não presumido.
	for city in initial.get("cities", []):
		sim.set_external_income_index(String(city["id"]), 0.0)
	var money_initial := float(initial["money_total"])
	var population_initial := _population(initial)
	var wheat_baseline := 0.0
	var food_spend_baseline := 0.0
	var wheat_samples := 0

	for day in DAYS:
		var status: Dictionary = sim.tick_day()
		_check_conservation(sim, status, money_initial, population_initial, day + 1)
		if day >= DAYS - 10:
			wheat_baseline += float(status["consumed_today"][0])
			food_spend_baseline += _food_spend(sim)
			wheat_samples += 1

	var before_shock: Dictionary = sim.get_status()
	var savings_before := _savings(sim)
	sim.price_shock("wheat", 100.0)
	var wheat_after := 0.0
	var food_spend_after := 0.0
	var after_samples := 0
	for day in SHOCK_DAYS:
		var status: Dictionary = sim.tick_day()
		_check_conservation(sim, status, money_initial, population_initial, DAYS + day + 1)
		wheat_after += float(status["consumed_today"][0])
		food_spend_after += _food_spend(sim)
		after_samples += 1

	var average_before := wheat_baseline / maxf(1.0, float(wheat_samples))
	var average_after := wheat_after / maxf(1.0, float(after_samples))
	var spend_before := food_spend_baseline / maxf(1.0, float(wheat_samples))
	var spend_after := food_spend_after / maxf(1.0, float(after_samples))
	var savings_after := _savings(sim)

	_report(sim, before_shock, average_before, average_after, spend_before, spend_after,
			savings_before, savings_after)
	_check_economy(sim, before_shock, spend_before, spend_after, savings_before, savings_after)

	if _failures.is_empty():
		print("[population] OK: conservação e ciclo econômico validados.")
		quit(0)
	else:
		print("[population] FALHAS (%d):" % _failures.size())
		for failure in _failures:
			print(" - " + failure)
		quit(1)


func _check_conservation(sim, status: Dictionary, money_initial: float,
		population_initial: float, day: int) -> void:
	if absf(float(status["money_total"]) - money_initial) > MONEY_EPSILON:
		_failures.append("dia %d: dinheiro %.4f != %.4f" % [
			day, status["money_total"], money_initial])
	if absf(_population(status) - population_initial) > 0.001:
		_failures.append("dia %d: população %.3f != %.3f" % [
			day, _population(status), population_initial])
	for city in status["cities"]:
		var city_id := String(city["id"])
		# Mercados são detalhe pesado: vêm sob demanda pelo id da cidade.
		for market in sim.get_city_status(city_id).get("markets", []):
			if float(market["stock"]) < -STOCK_EPSILON:
				_failures.append("dia %d: estoque negativo em %s (%.4f)" % [
					day, city_id, market["stock"]])


func _check_economy(sim, status: Dictionary, spend_before: float, spend_after: float,
		savings_before: float, savings_after: float) -> void:
	var produced: Array = status["produced_total"]
	var consumed: Array = status["consumed_total"]
	if float(produced[0]) <= 0.0:
		_failures.append("sem produção de trigo")
	if float(consumed[0]) <= 0.0:
		_failures.append("sem consumo de trigo")

	for city in status["cities"]:
		var city_id := String(city["id"])
		# Construções e POP são detalhe pesado: vêm sob demanda por cidade.
		var heavy: Dictionary = sim.get_city_status(city_id)
		if float(city["gdp"]) <= 0.0:
			_failures.append("%s: PIB não positivo" % city_id)
		var building_profit := 0.0
		for building in heavy.get("buildings", []):
			building_profit += float(building["profit"])
		if building_profit <= 0.0:
			_failures.append("%s: lucro privado não positivo (%.2f)" % [
				city_id, building_profit])
		var pop: Dictionary = heavy.get("pop", {})
		if float(pop["satisfaction"]) < 0.5:
			_failures.append("%s: satisfação alimentar baixa (%.2f)" % [
				city["id"], pop["satisfaction"]])
		if float(pop["labor_force"]) <= 0.0:
			_failures.append("%s: força de trabalho inválida" % city["id"])
		if absf(float(pop["employed"]) + float(pop["unemployed"])
				- float(pop["labor_force"])) > 0.01:
			_failures.append("%s: emprego + desemprego != força de trabalho" % city["id"])

	if spend_before <= 0.0:
		_failures.append("gasto alimentar antes do choque não positivo")
	elif spend_after <= spend_before * 1.02:
		_failures.append("choque de preço não aumentou o gasto alimentar (%.2f -> %.2f)" % [
			spend_before, spend_after])
	if savings_after >= savings_before:
		_failures.append("choque de preço não reduziu a poupança (%.2f -> %.2f)" % [
			savings_before, savings_after])
	if _external_income(sim) > 0.001:
		_failures.append("renda externa não isolada no teste (%.3f)" % _external_income(sim))


func _population(status: Dictionary) -> float:
	var total := 0.0
	for city in status["cities"]:
		total += float(city["population"])
	return total


## POP é detalhe pesado: o status diário publica só os escalares leves, então
## cada cidade é lida sob demanda pelo id.
func _savings(sim) -> float:
	var total := 0.0
	for city in sim.get_status().get("cities", []):
		total += float(sim.get_city_status(String(city["id"])).get("pop", {}).get(
			"savings", 0.0))
	return total


func _food_spend(sim) -> float:
	var total := 0.0
	for city in sim.get_status().get("cities", []):
		total += float(sim.get_city_status(String(city["id"])).get("pop", {}).get(
			"food_spend", 0.0))
	return total


func _external_income(sim) -> float:
	var total := 0.0
	for city in sim.get_status().get("cities", []):
		total += float(sim.get_city_status(String(city["id"])).get("pop", {}).get(
			"external_income", 0.0))
	return total


func _report(sim, status: Dictionary, wheat_before: float, wheat_after: float,
		spend_before: float, spend_after: float,
		savings_before: float, savings_after: float) -> void:
	var produced: Array = status["produced_total"]
	var consumed: Array = status["consumed_total"]
	print("[population] dia=%d dinheiro=%.2f trigo_antes=%.2f trigo_depois=%.2f" % [
		status["day"], status["money_total"], wheat_before, wheat_after])
	print("  gasto_alimentar=%.2f->%.2f  poupança=%.2f->%.2f" % [
		spend_before, spend_after, savings_before, savings_after])
	print("  produzido(trigo)=%.1f consumido(trigo)=%.1f" % [produced[0], consumed[0]])
	for city in status["cities"]:
		var heavy: Dictionary = sim.get_city_status(String(city["id"]))
		var pop: Dictionary = heavy.get("pop", {})
		var income := float(pop["disposable"]) + float(pop["service_spend"])
		print(("  %-6s pop=%.0f PIB=%.1f PIBpc=%.5f emprego=%.0f desemprego=%.0f "
				+ "salario=%.1f renda=%.1f disp=%.1f consumo=%.2f sav=%.1f sat=%.2f") % [
			city["id"], city["population"], city["gdp"], city["gdp_per_capita"],
			pop["employed"], pop["unemployed"], pop["wage_income"],
			income, pop["disposable"], pop["consumed"], pop["savings"],
			pop["satisfaction"]])
		var markets: Array = heavy.get("markets", [])
		var prices := PackedStringArray()
		for market in markets:
			prices.append("%.1f" % market["price"])
		print("         preços(trigo)=%s estoque(trigo)=%.1f" % [
			" / ".join(prices), float(Array(markets)[0]["stock"])])
