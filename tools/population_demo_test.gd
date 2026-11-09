extends SceneTree

## Teste headless do cenário mínimo de duas cidades (POPs, trigo, farinha, pão).
## Executa 30 dias, confere conservação de dinheiro/população/estoque e aplica
## um choque de preço no pão para observar substituição e queda de consumo.

const DAYS := 30
const SHOCK_DAYS := 10
const MONEY_EPSILON := 0.5
const STOCK_EPSILON := 0.001

var _failures: Array[String] = []


func _init() -> void:
	var sim = EconomySim.new()
	sim.initialize_demo()

	var initial: Dictionary = sim.get_demo_status()
	var money_initial := float(initial["money_total"])
	var population_initial := _population(initial)
	var bread_baseline := 0.0
	var food_spend_baseline := 0.0
	var bread_samples := 0

	for day in DAYS:
		var status: Dictionary = sim.tick_demo()
		_check_conservation(status, money_initial, population_initial, day + 1)
		if day >= DAYS - 10:
			bread_baseline += float(status["consumed_today"][2])
			food_spend_baseline += _food_spend(status)
			bread_samples += 1

	var before_shock: Dictionary = sim.get_demo_status()
	var savings_before := _savings(before_shock)
	sim.demo_price_shock("bread", 100.0)
	var bread_after := 0.0
	var food_spend_after := 0.0
	var after_samples := 0
	for day in SHOCK_DAYS:
		var status: Dictionary = sim.tick_demo()
		_check_conservation(status, money_initial, population_initial, DAYS + day + 1)
		bread_after += float(status["consumed_today"][2])
		food_spend_after += _food_spend(status)
		after_samples += 1

	var average_before := bread_baseline / maxf(1.0, float(bread_samples))
	var average_after := bread_after / maxf(1.0, float(after_samples))
	var spend_before := food_spend_baseline / maxf(1.0, float(bread_samples))
	var spend_after := food_spend_after / maxf(1.0, float(after_samples))
	var savings_after := _savings(sim.get_demo_status())

	_report(before_shock, average_before, average_after, spend_before, spend_after,
			savings_before, savings_after)
	_check_economy(before_shock, spend_before, spend_after, savings_before, savings_after)

	if _failures.is_empty():
		print("[population] OK: conservação e ciclo econômico validados.")
		quit(0)
	else:
		print("[population] FALHAS (%d):" % _failures.size())
		for failure in _failures:
			print(" - " + failure)
		quit(1)


func _check_conservation(status: Dictionary, money_initial: float,
		population_initial: float, day: int) -> void:
	if absf(float(status["money_total"]) - money_initial) > MONEY_EPSILON:
		_failures.append("dia %d: dinheiro %.4f != %.4f" % [
			day, status["money_total"], money_initial])
	if absf(_population(status) - population_initial) > 0.001:
		_failures.append("dia %d: população %.3f != %.3f" % [
			day, _population(status), population_initial])
	for city in status["cities"]:
		for market in city["markets"]:
			if float(market["stock"]) < -STOCK_EPSILON:
				_failures.append("dia %d: estoque negativo em %s (%.4f)" % [
					day, city["id"], market["stock"]])


func _check_economy(status: Dictionary, spend_before: float, spend_after: float,
		savings_before: float, savings_after: float) -> void:
	var produced: Array = status["produced_total"]
	var consumed: Array = status["consumed_total"]
	if float(produced[0]) <= 0.0:
		_failures.append("sem produção de trigo")
	if float(produced[1]) <= 0.0:
		_failures.append("sem produção de farinha")
	if float(produced[2]) <= 0.0:
		_failures.append("sem produção de pão")
	if float(consumed[2]) <= 0.0:
		_failures.append("sem consumo de pão")

	for city in status["cities"]:
		if float(city["gdp"]) <= 0.0:
			_failures.append("%s: PIB não positivo" % city["id"])
		var building_profit := 0.0
		for building in city.get("buildings", []):
			building_profit += float(building["profit"])
		if building_profit <= 0.0:
			_failures.append("%s: lucro privado não positivo (%.2f)" % [
				city["id"], building_profit])
		var pop: Dictionary = city["pop"]
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


func _population(status: Dictionary) -> float:
	var total := 0.0
	for city in status["cities"]:
		total += float(city["population"])
	return total


func _savings(status: Dictionary) -> float:
	var total := 0.0
	for city in status["cities"]:
		total += float(city["pop"]["savings"])
	return total


func _food_spend(status: Dictionary) -> float:
	var total := 0.0
	for city in status["cities"]:
		total += float(city["pop"]["food_spend"])
	return total


func _report(status: Dictionary, bread_before: float, bread_after: float,
		spend_before: float, spend_after: float,
		savings_before: float, savings_after: float) -> void:
	var produced: Array = status["produced_total"]
	var consumed: Array = status["consumed_total"]
	print("[population] dia=%d dinheiro=%.2f pão_antes=%.2f pão_depois=%.2f" % [
		status["day"], status["money_total"], bread_before, bread_after])
	print("  gasto_alimentar=%.2f->%.2f  poupança=%.2f->%.2f" % [
		spend_before, spend_after, savings_before, savings_after])
	print("  produzido(t/far/pão)=%.1f/%.1f/%.1f consumido=%.1f/%.1f/%.1f" % [
		produced[0], produced[1], produced[2], consumed[0], consumed[1], consumed[2]])
	for city in status["cities"]:
		var pop: Dictionary = city["pop"]
		var income := float(pop["disposable"]) + float(pop["service_spend"])
		print(("  %-6s pop=%.0f PIB=%.1f PIBpc=%.5f emprego=%.0f desemprego=%.0f "
				+ "salario=%.1f renda=%.1f disp=%.1f consumo=%.2f sav=%.1f sat=%.2f") % [
			city["id"], city["population"], city["gdp"], city["gdp_per_capita"],
			pop["employed"], pop["unemployed"], pop["wage_income"],
			income, pop["disposable"], pop["consumed"], pop["savings"],
			pop["satisfaction"]])
		var prices := PackedStringArray()
		for market in city["markets"]:
			prices.append("%.1f" % market["price"])
		print("         preços(t/far/pão)=%s estoque(t/far/pão)=%.1f/%.1f/%.1f" % [
			" / ".join(prices),
			city["markets"][0]["stock"], city["markets"][1]["stock"],
			city["markets"][2]["stock"]])
