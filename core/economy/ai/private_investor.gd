extends RefCounted
class_name PrivateInvestor

## Investidor privado determinístico. Observa demanda não atendida, preços,
## custos, aptidão e rentabilidade; investe apenas quando o retorno esperado
## cabe no capital disponível e no horizonte de retorno.

const CAPITAL_SHARE := 0.35
const MIN_PAYBACK_DAYS := 150.0
const MIN_MARGIN_PER_TON := 0.5
const MIN_SHORTAGE_TONS := 0.5
const MIN_CAPACITY := 2.0
const MAX_CAPACITY := 40.0
const MIN_SUITABILITY := 0.30
const RISK_WINDOW := 30
const BASE_RISK := 0.15

const Templates := preload("res://core/economy/ai/industry_templates.gd")

var _sim: Object
var _knowledge: Object
var _decisions: Array[Dictionary] = []
var _history: Dictionary = {}
var _id_counter := 1


func setup(sim: Object, knowledge: Object) -> void:
	_sim = sim
	_knowledge = knowledge


func run(day: int, status: Dictionary) -> void:
	for city in status.get("cities", []):
		_run_city(day, city, status)


func drain() -> Array:
	var output: Array = _decisions.duplicate(true)
	_decisions.clear()
	return output


func _run_city(day: int, city: Dictionary, status: Dictionary) -> void:
	var city_id := String(city["id"])
	var firm_cash := float(city["firm_cash"])
	var available := firm_cash * CAPITAL_SHARE
	var capital_cost := float(status.get("capital_cost_per_capacity", 2000.0))
	if available < capital_cost * MIN_CAPACITY:
		return

	for resource_name in Templates.TEMPLATES:
		var template: Dictionary = Templates.TEMPLATES[resource_name]
		var product := int(template["output"])
		var market: Dictionary = city["markets"][product]
		var sold := float(market.get("sold", 0.0))
		var shortage := float(market.get("shortage", 0.0))
		var surplus := float(market.get("surplus", 0.0))

		if surplus > maxf(1.0, sold * 0.20):
			_log(day, city_id, "avoid", resource_name,
				"excesso de oferta local (%.1f t)" % surplus, 0.0, 0.0, 0.0)
			continue
		var system_surplus := _system_surplus(status, product)
		var system_sold := _system_sold(status, product)
		if system_surplus > maxf(1.0, system_sold * 0.25):
			_log(day, city_id, "avoid", resource_name,
				"excesso de oferta no sistema (%.1f t)" % system_surplus,
				0.0, 0.0, 0.0)
			continue
		if shortage < MIN_SHORTAGE_TONS:
			continue
		if not _resource_available(city, template):
			_log(day, city_id, "avoid", resource_name,
				"sem aptidão ou depósito conhecido", 0.0, 0.0, 0.0)
			continue
		if not _input_available(city, template):
			_log(day, city_id, "avoid", resource_name,
				"insumo indisponível", 0.0, 0.0, 0.0)
			continue

		var price := float(market["price"])
		var observed: float = Templates.observed_cost(city, product)
		var unit_cost: float = observed if observed >= 0.0 else Templates.variable_cost(resource_name, city)
		var margin: float = price - unit_cost
		if margin <= MIN_MARGIN_PER_TON:
			_log(day, city_id, "avoid", resource_name,
				"margem %.2f/t insuficiente" % margin, 0.0, 0.0, 0.0)
			continue

		var capacity := clampf(shortage, MIN_CAPACITY, MAX_CAPACITY)
		if capacity * capital_cost > available:
			capacity = available / capital_cost
		if capacity < MIN_CAPACITY:
			continue
		var cost := capacity * capital_cost
		var daily_profit := margin * capacity
		var payback := cost / maxf(daily_profit, 0.001)
		var risk := _price_risk(city_id, product, price)
		var adjusted_payback := payback / maxf(0.5, 1.0 - risk)
		if adjusted_payback > MIN_PAYBACK_DAYS:
			_log(day, city_id, "avoid", resource_name,
				"payback ajustado %.0f d > %.0f d" % [adjusted_payback, MIN_PAYBACK_DAYS],
				cost, daily_profit, risk)
			continue

		var existing := _find_producer(city, product)
		var result: Dictionary
		var action := "build"
		var target := ""
		if not existing.is_empty():
			target = String(existing["id"])
			action = "expand"
			result = _sim.demo_invest(city_id, target, cost)
		else:
			target = "AI-%s-%d" % [resource_name.to_upper(), _id_counter]
			_id_counter += 1
			result = _sim.demo_build(city_id, target, resource_name,
				int(template["input"]), product, capacity,
				capacity * float(template["jobs_per_ton"]), float(template["wage"]),
				capacity * float(template["opex_per_ton"]),
				float(template["input_ratio"]), "private", cost)

		var error := String(result.get("error", ""))
		if error != "":
			_log(day, city_id, "rejected", resource_name, error, cost, daily_profit, risk)
			continue
		_log(day, city_id, action, resource_name,
			"escassez %.1f t, margem %.2f/t, payback %.0f d, risco %.2f" % [
				shortage, margin, adjusted_payback, risk],
			cost, daily_profit, risk, target)
		break


func _resource_available(city: Dictionary, template: Dictionary) -> bool:
	if int(template["input"]) >= 0:
		return true
	var product := int(template["output"])
	var suitability: Array = city.get("suitability", [])
	if product >= suitability.size():
		return false
	return float(suitability[product]) >= MIN_SUITABILITY


func _input_available(city: Dictionary, template: Dictionary) -> bool:
	var input_product := int(template["input"])
	if input_product < 0:
		return true
	var market: Dictionary = city["markets"][input_product]
	if float(market.get("stock", 0.0)) + float(market.get("supply", 0.0)) > 0.1:
		return true
	for building in city.get("buildings", []):
		if int(building.get("output_product", -1)) == input_product:
			return true
	return false


func _find_producer(city: Dictionary, product: int) -> Dictionary:
	for building in city.get("buildings", []):
		if (int(building.get("output_product", -1)) == product
				and String(building.get("owner", "private")) != "state"):
			return building
	return {}


func _system_surplus(status: Dictionary, product: int) -> float:
	var total := 0.0
	for city in status.get("cities", []):
		total += maxf(0.0, float(city["markets"][product].get("surplus", 0.0)))
	return total


func _system_sold(status: Dictionary, product: int) -> float:
	var total := 0.0
	for city in status.get("cities", []):
		total += maxf(0.0, float(city["markets"][product].get("sold", 0.0)))
	return total


func _price_risk(city_id: String, product: int, price: float) -> float:
	var key := "%s|%d" % [city_id, product]
	if not _history.has(key):
		_history[key] = []
	var series: Array = _history[key]
	series.append(price)
	while series.size() > RISK_WINDOW:
		series.pop_front()
	if series.size() < 5:
		return BASE_RISK
	var mean := 0.0
	for value in series:
		mean += float(value)
	mean /= series.size()
	if mean <= 0.0:
		return BASE_RISK
	var variance := 0.0
	for value in series:
		variance += pow(float(value) - mean, 2.0)
	variance /= series.size()
	return clampf(sqrt(variance) / mean, 0.0, 0.5)


func _log(day: int, city_id: String, action: String, resource: String,
		reason: String, cost: float, daily_profit: float, risk: float,
		target: String = "") -> void:
	_decisions.append({
		"day": day,
		"agent": "private",
		"city": city_id,
		"action": action,
		"resource": resource,
		"target": target,
		"reason": reason,
		"cost": cost,
		"expected_daily_profit": daily_profit,
		"risk": risk,
	})
