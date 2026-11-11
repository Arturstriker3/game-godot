extends RefCounted
class_name PrivateInvestor

## Investidor privado determinístico. Observa demanda não atendida, preços,
## custos, aptidão e rentabilidade; investe apenas quando o retorno esperado
## cabe no capital disponível e no horizonte de retorno.

const CAPITAL_SHARE := 0.35
const MIN_PAYBACK_DAYS := 150.0
## Horizonte de payback para bens bulk de baixo valor (enxofre/carvão/fosfato/
## bauxita): extratores de margem fina por tonelada mas reserva de décadas. Sem
## isso a mina de enxofre (margem ~7–11/t → payback 180–290 d) nunca passa o
## gate de 150 d e fica com extração 0 (B4).
const MIN_PAYBACK_DAYS_BULK := 730.0
const EXPORT_CAPACITY_FACTOR := 1.2
const MIN_MARGIN_PER_TON := 0.5
const MIN_SHORTAGE_TONS := 0.5
const MIN_CAPACITY := 2.0
const MAX_CAPACITY := 40.0
const MIN_SUITABILITY := 0.30
const RISK_WINDOW := 30
const BASE_RISK := 0.15

const Templates := preload("res://core/economy/ai/industry_templates.gd")

var _sim: Object
var _geo: Object
var _decisions: Array[Dictionary] = []
var _history: Dictionary = {}
var _id_counter := 1


func setup(sim: Object, geo: Object) -> void:
	_sim = sim
	_geo = geo


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

	var pop: Dictionary = city["pop"]
	# Déficit estrutural (B3): necessidade − produção local, em t-eq de grão.
	# Usa a produção (não o consumo) para detectar dependência de importação.
	var food_deficit := maxf(0.0, float(pop.get("need", 0.0)) - Templates.local_food_supply(city))

	# 1. Alimento (B3): déficit alimentar (t-eq) → melhor cultura (aptidão ×
	# preço). Sem déficit, só registra o excesso de oferta (diagnóstico).
	if food_deficit >= MIN_SHORTAGE_TONS:
		if _try_food(day, city, status, food_deficit, available, capital_cost):
			return
	else:
		for resource_name in Templates.FOOD_RESOURCES:
			var product := int(Templates.TEMPLATES[resource_name]["output"])
			var market: Dictionary = city["markets"][product]
			var surplus := float(market.get("surplus", 0.0))
			var sold := float(market.get("sold", 0.0))
			if surplus > maxf(1.0, sold * 0.20):
				_log(day, city_id, "avoid", resource_name,
					"excesso de oferta local (%.1f t)" % surplus, 0.0, 0.0, 0.0)

	# 2. Matérias-primas não alimentares (ferro): lógica existente.
	for resource_name in Templates.TEMPLATES:
		if Templates.FOOD_RESOURCES.has(resource_name):
			continue
		var product := int(Templates.TEMPLATES[resource_name]["output"])
		var shortage := float(city["markets"][product].get("shortage", 0.0))
		if _try_template(day, city, status, resource_name, shortage, available, capital_cost):
			return


## Investe na cultura alimentar com melhor aptidão × preço (B3). Converte o
## déficit em toneladas reais pelo fator calórico da cultura e usa os mesmos
## gates de margem/payback do resto da IA.
func _try_food(day: int, city: Dictionary, status: Dictionary, food_deficit: float,
		available: float, capital_cost: float) -> bool:
	var suitability: Array = city.get("suitability", [])
	var best := ""
	var best_score := -1.0
	for resource_name in Templates.FOOD_RESOURCES:
		var template: Dictionary = Templates.TEMPLATES[resource_name]
		var product := int(template["output"])
		if product >= suitability.size() or float(suitability[product]) < MIN_SUITABILITY:
			continue
		var price := float(city["markets"][product].get("price", 0.0))
		# Prefere expandir uma cultura JÁ estabelecida na cidade (fix do churn de
		# milho redundante, B4): quem já é autossuficiente em um alimento não deve
		# introduzir um segundo em paralelo só por aptidão×preço marginalmente
		# maior. O bônus 2× faz o investidor expandir o produtor existente em vez
		# de diversificar.
		var established := Templates.has_producer(city, resource_name)
		var score := float(suitability[product]) * price * (2.0 if established else 1.0)
		if score > best_score:
			best_score = score
			best = resource_name
	if best == "":
		return false
	var shortage := food_deficit / Templates.caloric_factor(best)
	return _try_template(day, city, status, best, shortage, available, capital_cost)


## Gates comuns de investimento (excesso de oferta, margem, payback) e a
## construção/expansão em si. Reusado por alimentos e por ferro.
func _try_template(day: int, city: Dictionary, status: Dictionary, resource_name: String,
		shortage: float, available: float, capital_cost: float) -> bool:
	var city_id := String(city["id"])
	var template: Dictionary = Templates.TEMPLATES[resource_name]
	var product := int(template["output"])
	var market: Dictionary = city["markets"][product]
	var sold := float(market.get("sold", 0.0))
	var surplus := float(market.get("surplus", 0.0))

	if surplus > maxf(1.0, sold * 0.20):
		_log(day, city_id, "avoid", resource_name,
			"excesso de oferta local (%.1f t)" % surplus, 0.0, 0.0, 0.0)
		return false
	var system_surplus := _system_surplus(status, product)
	var system_sold := _system_sold(status, product)
	# Correção de raiz da biestabilidade (balance_review §11.3): o teto de
	# "excesso de oferta no sistema" NÃO se aplica a alimentos da cesta. Ele lia
	# o excedente GLOBAL (a FRA acumulava trigo conforme Londres importava menos)
	# e barrava a expansão da fazenda de Londres ANTES da autossuficiência —
	# Londres seguia importando, o orçamento drenava e a cidade caía no
	# equilíbrio de baixo emprego. Para a cesta, vale a demanda REAL da cidade
	# (o déficit local), não o excedente do mercado mundial.
	if system_surplus > maxf(1.0, system_sold * 0.25) \
			and not Templates.FOOD_RESOURCES.has(resource_name):
		_log(day, city_id, "avoid", resource_name,
			"excesso de oferta no sistema (%.1f t)" % system_surplus, 0.0, 0.0, 0.0)
		return false
	if shortage < MIN_SHORTAGE_TONS:
		return false
	if not _resource_available(city, resource_name, template):
		_log(day, city_id, "avoid", resource_name,
			"sem aptidão ou depósito conhecido", 0.0, 0.0, 0.0)
		return false
	if not _input_available(city, template):
		_log(day, city_id, "avoid", resource_name,
			"insumo indisponível", 0.0, 0.0, 0.0)
		return false

	var price := float(market["price"])
	var observed: float = Templates.observed_cost(city, product)
	var is_mineral := Templates.MINERAL_RESOURCES.has(resource_name)
	# Margem "honesta" para minas: inclui o fator de profundidade da jazida
	# (opex_factor) quando ainda não há produtor observado. Sem isso a IA
	# investiria em jazidas muito fundas de mineral de baixo valor.
	var unit_cost: float
	if observed >= 0.0:
		unit_cost = observed
	elif is_mineral:
		unit_cost = Templates.variable_cost_mined(resource_name, city)
	else:
		unit_cost = Templates.variable_cost(resource_name, city)
	var margin: float = price - unit_cost
	if margin <= MIN_MARGIN_PER_TON:
		_log(day, city_id, "avoid", resource_name,
			"margem %.2f/t insuficiente" % margin, 0.0, 0.0, 0.0)
		return false

	var capacity := clampf(shortage, MIN_CAPACITY, MAX_CAPACITY)
	if is_mineral:
		# Mesmo dimensionamento da mina estatal (B4): teto na demanda externa
		# × 1.2, para o investidor privado também não superexpandir além do que
		# o mercado externo absorve (evita o churn de minas superdimensionadas).
		var external_demand := Templates.external_demand_of(city, product)
		capacity = clampf(minf(shortage, external_demand * EXPORT_CAPACITY_FACTOR),
			MIN_CAPACITY, MAX_CAPACITY)
	# Terra limitada: a capacidade de uma fazenda (alimento da cesta) não pode
	# ultrapassar a capacidade física da área cultivável da cidade (regra nova).
	# O teto é a área cultivável × produtividade FAO, menos o que já está
	# instalado (a produção efetiva segue = capacidade × aptidão). Só quando há
	# dado de área (land_cap > 0): cidade sem bacia (ex.: cenário demo sem
	# nação) não tem teto de terra — a aptidão do fallback já decide.
	if Templates.FOOD_RESOURCES.has(resource_name) and _geo != null:
		var land_cap := float(_geo.city_land_capacity_t_day(city_id, resource_name))
		if land_cap > 0.0:
			var used := Templates.existing_capacity(city, product)
			capacity = minf(capacity, maxf(0.0, land_cap - used))
	if capacity * capital_cost > available:
		capacity = available / capital_cost
	if capacity < MIN_CAPACITY:
		return false
	var cost := capacity * capital_cost
	var daily_profit := margin * capacity
	var payback := cost / maxf(daily_profit, 0.001)
	var risk := _price_risk(city_id, product, price)
	var adjusted_payback := payback / maxf(0.5, 1.0 - risk)
	var payback_limit := MIN_PAYBACK_DAYS_BULK \
		if Templates.BULK_LOW_VALUE_RESOURCES.has(resource_name) \
		else MIN_PAYBACK_DAYS
	if adjusted_payback > payback_limit:
		_log(day, city_id, "avoid", resource_name,
			"payback ajustado %.0f d > %.0f d" % [adjusted_payback, payback_limit],
			cost, daily_profit, risk)
		return false

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
		return false
	_log(day, city_id, action, resource_name,
		"escassez %.1f t, margem %.2f/t, payback %.0f d, risco %.2f" % [
			shortage, margin, adjusted_payback, risk],
		cost, daily_profit, risk, target)
	return true


func _resource_available(city: Dictionary, resource_name: String, template: Dictionary) -> bool:
	if int(template["input"]) >= 0:
		return true
	var product := int(template["output"])
	var suitability: Array = city.get("suitability", [])
	if product >= suitability.size() or float(suitability[product]) < MIN_SUITABILITY:
		return false
	if Templates.MINERAL_RESOURCES.has(resource_name):
		return _has_deposit(city, resource_name)
	return true


func _has_deposit(city: Dictionary, resource: String) -> bool:
	var deposits: Dictionary = city.get("deposits", {})
	if not deposits.has(resource):
		return false
	return float(deposits[resource].get("remaining_kt", 0.0)) > 0.0


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
