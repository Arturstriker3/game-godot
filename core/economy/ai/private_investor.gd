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

## Totais do sistema, medidos UMA vez por dia no `plan()` do escalonador (ver
## `prepare_totals`). Antes, cada cidade varria todas as cidades por produto
## (`_system_surplus`/`_system_sold`): O(cidades² × produtos), 442 ms/dia com
## 650 cidades. Aqui: uma passada O(cidades × produtos) por dia.
##
## Somatórios: {"surplus": PackedFloat64Array, "sold": ..., "demand": ...}
## indexada pelo id de produto do C++. Os agregados POR NAÇÃO (população, need,
## deficit, produção) já vêm prontos do núcleo em `status["nations"]` (ver
## `nation_summary`) — não são recalculados aqui.
var _sum_surplus := PackedFloat64Array()
var _sum_sold := PackedFloat64Array()
var _sum_demand := PackedFloat64Array()
var _totals_status: Dictionary = {}
## Quantas vezes os totais foram medidos (telemetria: deve ser 1 por dia) e
## quanto custou a última medição (ms).
var totals_builds := 0
var last_prepare_ms := 0.0


func setup(sim: Object, geo: Object) -> void:
	_sim = sim
	_geo = geo


func run(day: int, status: Dictionary) -> void:
	prepare_totals(status)
	for city in status.get("cities", []):
		run_city(day, city, status)


## Mede os totais do sistema uma única vez por status (uma passada pelas
## cidades). O escalonador chama isto no `plan()`; `run_city` só recalcula se
## receber um status diferente (chamada síncrona de ferramentas/testes).
##
## Quando o status vem do `tick_demo()`/`get_demo_status()` do núcleo, ele já
## traz `market_totals` (soma por produto calculada em C++ na thread do tick):
## ler 3 PackedFloat64Array custa ~0,02 ms. O laço em GDScript abaixo fica só
## como retaguarda para status construídos à mão (testes/ferramentas) — com
## 858 cidades ele custava ~13 ms no frame da virada.
func prepare_totals(status: Dictionary) -> void:
	var started := Time.get_ticks_usec()
	totals_builds += 1
	_totals_status = status
	var totals: Dictionary = status.get("market_totals", {})
	if not totals.is_empty():
		_sum_surplus = totals.get("surplus", PackedFloat64Array())
		_sum_sold = totals.get("sold", PackedFloat64Array())
		_sum_demand = totals.get("demand", PackedFloat64Array())
		last_prepare_ms = _elapsed_ms(started)
		return
	var cities: Array = status.get("cities", [])
	if cities.is_empty():
		_sum_surplus = PackedFloat64Array()
		_sum_sold = PackedFloat64Array()
		_sum_demand = PackedFloat64Array()
		last_prepare_ms = _elapsed_ms(started)
		return
	var product_count: int = (cities[0].get("markets", []) as Array).size()
	if product_count <= 0:
		_sum_surplus = PackedFloat64Array()
		_sum_sold = PackedFloat64Array()
		_sum_demand = PackedFloat64Array()
		last_prepare_ms = _elapsed_ms(started)
		return
	# Packed arrays LOCAIS durante a soma: guardá-las no dicionário antes do
	# laço faria cada escrita copiar o buffer (copy-on-write) — o Array de
	# Variants tinha o mesmo custo por conversão. O dicionário recebe o
	# resultado só no fim.
	var system_surplus := PackedFloat64Array()
	var system_sold := PackedFloat64Array()
	var system_demand := PackedFloat64Array()
	system_surplus.resize(product_count)
	system_sold.resize(product_count)
	system_demand.resize(product_count)
	for city in cities:
		var markets: Array = city.get("markets", [])
		var limit: int = mini(product_count, markets.size())
		for product in limit:
			var market: Dictionary = markets[product]
			var surplus := float(market["surplus"])
			if surplus > 0.0:
				system_surplus[product] += surplus
			var sold := float(market["sold"])
			if sold > 0.0:
				system_sold[product] += sold
			var demand := float(market["demand"])
			if demand > 0.0:
				system_demand[product] += demand
	_sum_surplus = system_surplus
	_sum_sold = system_sold
	_sum_demand = system_demand
	last_prepare_ms = _elapsed_ms(started)


func _elapsed_ms(started_usec: int) -> float:
	return float(Time.get_ticks_usec() - started_usec) / 1000.0


## Total do sistema para um produto e campo ("surplus", "sold", "demand").
## 0,0 quando o produto não existe no status (ausência declarada, não estimada).
func system_total(product: int, field: String) -> float:
	var values: PackedFloat64Array
	if field == "surplus":
		values = _sum_surplus
	elif field == "sold":
		values = _sum_sold
	else:
		values = _sum_demand
	if product < 0 or product >= values.size():
		return 0.0
	return values[product]


## Agregado de uma nação como o núcleo o publica em `status["nations"]`
## (população, need, deficit, produção, city_count). Devolve {} quando a nação
## não aparece no status — ausência de dado, não zero.
func nation_summary(nation: String) -> Dictionary:
	for entry in _totals_status.get("nations", []):
		if String(entry.get("tag", "")) == nation:
			return entry
	return {}


## Planeja UMA cidade. O orquestrador escalona as cidades ao longo do ciclo de
## 7 dias e fatia o trabalho entre frames; a cadência por cidade segue 7 dias.
func run_city(day: int, city: Dictionary, status: Dictionary) -> void:
	if not is_same(_totals_status, status):
		prepare_totals(status)
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
	if not is_same(_totals_status, status):
		prepare_totals(status)
	return system_total(product, "surplus")


func _system_sold(status: Dictionary, product: int) -> float:
	if not is_same(_totals_status, status):
		prepare_totals(status)
	return system_total(product, "sold")


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
