extends RefCounted
class_name GovernmentAI

## Governo determinístico: cobre falhas de mercado em alimentos estratégicos e
## cria estatais quando o capital privado não chega. Nunca gasta acima da
## parcela orçamentária definida. Sem prospecção: todo depósito é conhecido
## desde a inicialização (via GeographyService.apply_to_sim).

const BUDGET_SHARE := 0.25
const MIN_SHORTAGE_TONS := 1.0
const MIN_STATE_CAPACITY := 2.0
const MAX_STATE_CAPACITY := 20.0
## Teto da capacidade de uma mina estatal (B4): o estado cobre uma base da
## demanda externa e o capital privado o resto. Substitui a antiga
## IRON_MINE_CAPACITY fixa — a capacidade agora é dimensionada pela demanda
## externa (× EXPORT_CAPACITY_FACTOR), para minas de demanda pequena (ouro/
## diamante/urânio/tungstênio, ~1 t/d) não nascerem com 10 t/d e falirem.
const MAX_STATE_MINE_CAPACITY := 10.0
const EXPORT_CAPACITY_FACTOR := 1.2
const HIGH_UNEMPLOYMENT := 0.65

## Anti-loop de construção (B5): não constrói se a utilização média efetiva dos
## prédios do mesmo tipo na cidade estiver abaixo do piso, se já houver o teto
## de construções do tipo, nem antes de um cooldown após a última construção.
const MIN_AVG_UTILIZATION := 0.60
const MAX_BUILDINGS_PER_TYPE := 6
const BUILD_COOLDOWN_DAYS := 30

## F3 — prioridade alimentar: a cidade com fome real constrói a cultura antes de
## abrir mina (o bloco de segurança alimentar abaixo); o capital restante segue
## para a mineração. Uma reserva de caixa para importação foi testada e
## descartada: bloqueava a renda mineral que paga a própria comida.

const Templates := preload("res://core/economy/ai/industry_templates.gd")

var _sim: Object
var _geo: Object
var _decisions: Array[Dictionary] = []
var _id_counter := 1
# Dia da última construção por cidade (base do cooldown).
var _last_build_day: Dictionary = {}


func setup(sim: Object, geo: Object) -> void:
	_sim = sim
	_geo = geo


func run(day: int, status: Dictionary) -> void:
	for city in status.get("cities", []):
		run_city(day, city, status)


## Planeja UMA cidade (ver `PrivateInvestor.run_city`): o orquestrador escalona
## as cidades no ciclo de 7 dias e fatia o trabalho entre frames.
func run_city(day: int, city: Dictionary, status: Dictionary) -> void:
	_run_city(day, city, status)


func drain() -> Array:
	var output: Array = _decisions.duplicate(true)
	_decisions.clear()
	return output


func _run_city(day: int, city: Dictionary, status: Dictionary) -> void:
	var city_id := String(city["id"])
	var budget := float(city["government_budget"])
	var capital_cost := float(status.get("capital_cost_per_capacity", 2000.0))
	var available := budget * BUDGET_SHARE
	if available <= 0.0:
		return

	# Índice O(1) das construções locais por produto (contagem e utilização
	# efetiva). _buildings_of_type/_avg_utilization varriam city["buildings"] a
	# cada mineral do laço quente (custo medido: ~14 varreduras × cidade). Aqui
	# é UMA varredura por cidade.
	var bindex := _building_index(city)

	# 1. Segurança alimentar (B3): quando há FOME REAL (déficit de consumo em
	# toneladas-equivalentes de grão), constrói a cultura com melhor aptidão ×
	# preço na cidade. O anti-loop B5 (cooldown/teto/utilização) segue no
	# _build_gate.
	var food_pop: Dictionary = city["pop"]
	# Déficit REAL (B3/bacia por província): necessidade − consumo, em toneladas-
	# equivalentes de grão. Usa o CONSUMO (não a produção local) — uma cidade
	# alimentada por comércio regional tem consumo satisfeito e NÃO deve
	# construir uma fazenda redundante que não consegue vender localmente (era a
	# causa do churn de milho em Manchester/Newcastle/Marselha, cuja bacia tem
	# aptidão de trigo baixa mas é atendida pela rede nacional). A produção local
	# (local_food_supply) segue informando o diagnóstico, não o gatilho.
	var food_deficit := maxf(0.0, float(food_pop.get("need", 0.0)) - float(food_pop.get("consumed", 0.0)))
	# O governo só cobre onde o capital privado NÃO chega: se a cidade já tem
	# qualquer produtor de alimento da cesta (fazenda inicial ou privada),
	# construir outra estatal é redundante e churna — o milho estatal de 20 t/d
	# em Manchester/Newcastle/Marselha era construído ao lado do AI-MAIZE privado
	# (2.185 t/d) e falia quando a escassez passava (balance_review §16).
	if food_deficit >= MIN_SHORTAGE_TONS and not _has_food_producer(city):
		var best := _best_food_crop(city)
		if not best.is_empty():
			var resource := String(best["resource"])
			var template: Dictionary = Templates.TEMPLATES[resource]
			var product := int(template["output"])
			var caloric := Templates.caloric_factor(resource)
			var capacity := clampf(food_deficit / caloric, MIN_STATE_CAPACITY, MAX_STATE_CAPACITY)
			# Terra limitada: teto pela área cultivável da cidade menos o já
			# instalado (mesma regra do investidor privado). Só quando há dado
			# de área (land_cap > 0): cidade sem bacia (ex.: cenário demo sem
			# nação) não tem teto de terra — a aptidão do fallback já decide.
			if _geo != null:
				var land_cap := float(_geo.city_land_capacity_t_day(city_id, resource))
				if land_cap > 0.0:
					var used := Templates.existing_capacity(city, product)
					capacity = minf(capacity, maxf(0.0, land_cap - used))
			if capacity * capital_cost >= available:
				capacity = available / capital_cost
			if capacity >= MIN_STATE_CAPACITY:
				var gate := _build_gate(city_id, city, day, product, bindex)
				if gate != "":
					_log(day, city_id, "avoid", resource, gate, capacity * capital_cost)
				else:
					var new_id := "STATE-%s-%d" % [resource.to_upper(), _id_counter]
					var result: Dictionary = _sim.demo_build(city_id, new_id, resource,
						int(template["input"]), product, capacity,
						capacity * float(template["jobs_per_ton"]), float(template["wage"]),
						capacity * float(template["opex_per_ton"]),
						float(template["input_ratio"]), "state", capacity * capital_cost)
					var error := String(result.get("error", ""))
					if error != "":
						_log(day, city_id, "rejected", resource, error, capacity * capital_cost)
					else:
						_id_counter += 1
						_last_build_day[city_id] = day
						_log(day, city_id, "build_state", resource,
							"segurança alimentar: déficit %.1f t-eq, aptidão×preço %.1f" % [
								food_deficit, float(best["score"])],
							capacity * capital_cost)
						return

	# 3. Mineração estratégica (B4): mina estatal para cada mineral com jazida
	# (aptidão > 0) e margem positiva ao preço de exportação, quando ainda não há
	# produtor na cidade. A margem inclui o fator de profundidade da jazida
	# (opex_factor via variable_cost_mined) e a capacidade é dimensionada pela
	# demanda externa (× 1.2, teto MAX_STATE_MINE_CAPACITY), não mais por uma
	# capacidade fixa — minas superdimensionadas operavam com utilização baixa
	# (0,10) e faliam em ciclo (churn). Os candidatos são ordenados por margem
	# decrescente para minerais de alto valor (diamante/urânio) não ficarem
	# sempre atrás do cooldown de construção.
	var suitability: Array = city.get("suitability", [0.0])
	var candidates: Array = []
	# Sem jazida declarada a cidade não pode abrir mina nenhuma: pula o laço de
	# 14 minerais inteiro (custo medido: ~14 varreduras de mercado × cidade).
	var deposits: Dictionary = city.get("deposits", {})
	if not deposits.is_empty():
		for resource in Templates.MINERAL_RESOURCES:
			var template: Dictionary = Templates.TEMPLATES[resource]
			var product := int(template["output"])
			if product >= suitability.size() \
					or float(suitability[product]) <= 0.0:
				continue
			if not _has_deposit(city, resource):
				continue
			if _buildings_of_type(city, product, bindex) > 0:
				continue
			var mineral_market: Dictionary = city["markets"][product]
			var margin: float = (Templates.anchor_price(city, product)
				- Templates.variable_cost_mined(resource, city))
			if margin <= 0.0 \
					or float(mineral_market.get("surplus", 0.0)) > 0.0:
				continue
			candidates.append({"resource": resource, "product": product,
				"template": template, "margin": margin})
	# Ranking por margem decrescente (ordem original).
	candidates.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return float(a["margin"]) > float(b["margin"]))
	for candidate in candidates:
		var resource: String = candidate["resource"]
		var product: int = candidate["product"]
		var template: Dictionary = candidate["template"]
		var margin: float = float(candidate["margin"])
		var gate := _build_gate(city_id, city, day, product, bindex)
		if gate != "":
			_log(day, city_id, "avoid", resource, gate, 0.0)
			continue
		var external_demand := Templates.external_demand_of(city, product)
		var capacity := minf(MAX_STATE_MINE_CAPACITY,
			maxf(MIN_STATE_CAPACITY, external_demand * EXPORT_CAPACITY_FACTOR))
		if capacity * capital_cost >= available:
			capacity = available / capital_cost
		if capacity < MIN_STATE_CAPACITY:
			continue
		var cost := capacity * capital_cost
		var new_id := "STATE-%s-%d" % [resource.to_upper(), _id_counter]
		var result: Dictionary = _sim.demo_build(city_id, new_id, resource,
			-1, product, capacity, capacity * float(template["jobs_per_ton"]),
			float(template["wage"]), capacity * float(template["opex_per_ton"]),
			0.0, "state", cost)
		var error := String(result.get("error", ""))
		if error == "":
			_id_counter += 1
			_last_build_day[city_id] = day
			_log(day, city_id, "build_state", resource,
				"recurso estratégico, margem %.2f/t, demanda externa %.1f t/d" % [
					margin, external_demand], cost)
			return
		_log(day, city_id, "rejected", resource, error, cost)

	# 4. Sem ação: registra por que não investiu.
	var pop: Dictionary = city["pop"]
	var labor_force := maxf(1.0, float(pop.get("labor_force", 0.0)))
	var unemployment := float(pop.get("unemployed", 0.0)) / labor_force
	if unemployment >= HIGH_UNEMPLOYMENT:
		_log(day, city_id, "avoid", "employment",
			"desemprego %.1f%% sem demanda adicional; evita excesso de oferta" % (unemployment * 100.0),
			0.0)


func _input_available(city: Dictionary, input_product: int) -> bool:
	if input_product < 0:
		return true
	var market: Dictionary = city["markets"][input_product]
	if float(market.get("stock", 0.0)) + float(market.get("supply", 0.0)) > 0.1:
		return true
	for building in city.get("buildings", []):
		if int(building.get("output_product", -1)) == input_product:
			return true
	return false


## true se a cidade já tem qualquer construção que produz um alimento da cesta.
## O governo só cobre onde o capital privado não chega (docstring da classe).
func _has_food_producer(city: Dictionary) -> bool:
	for resource in Templates.FOOD_RESOURCES:
		if Templates.has_producer(city, resource):
			return true
	return false


## Cultura alimentar com melhor aptidão × preço na cidade (B3). Retorna {} se
## nenhuma cultura for viável (aptidão <= 0 ou margem <= 0). Culturas são
## renováveis: não dependem de jazida.
func _best_food_crop(city: Dictionary) -> Dictionary:
	var suitability: Array = city.get("suitability", [])
	var best: Dictionary = {}
	var best_score := -1.0
	for resource in Templates.FOOD_RESOURCES:
		var template: Dictionary = Templates.TEMPLATES[resource]
		var product := int(template["output"])
		if product >= suitability.size() \
				or float(suitability[product]) <= 0.0:
			continue
		var price := Templates.anchor_price(city, product)
		# Viabilidade (bacia por província): uma fazenda cujo preço × aptidão não
		# paga o próprio custo variável (jobs·wage + opex) faliria em 60 dias e
		# não alimenta ninguém — o governo não deve construí-la. É o churn de
		# milho em Manchester/Newcastle (aptidão de milho ~0,38, abaixo do ponto
		# de equilíbrio ~0,42). O investidor privado aplica o mesmo gate (margem).
		if float(suitability[product]) * price \
				- Templates.variable_cost(resource, city) <= 0.0:
			continue
		# Prefere expandir a cultura já estabelecida (fix do churn de milho, B4).
		var established := Templates.has_producer(city, resource)
		var score := float(suitability[product]) * price * (2.0 if established else 1.0)
		if best.is_empty() or score > best_score:
			best_score = score
			best = {"resource": resource, "product": product, "score": score}
	return best


## Recurso primário disponível: aptidão > 0 (renovável) ou jazida com reserva
## restante (mineral). Transformações (input >= 0) não dependem de aptidão.
func _resource_available(city: Dictionary, resource: String, product: int) -> bool:
	var template: Dictionary = Templates.TEMPLATES[resource]
	if int(template["input"]) >= 0:
		return true
	var suitability: Array = city.get("suitability", [])
	if product >= suitability.size() \
			or float(suitability[product]) <= 0.0:
		return false
	if Templates.MINERAL_RESOURCES.has(resource):
		return _has_deposit(city, resource)
	return true


func _has_deposit(city: Dictionary, resource: String) -> bool:
	var deposits: Dictionary = city.get("deposits", {})
	if not deposits.has(resource):
		return false
	return float(deposits[resource].get("remaining_kt", 0.0)) > 0.0


## Bloqueio anti-loop: cooldown por cidade, teto por tipo e utilização efetiva.
## Retorna "" quando pode construir, senão o motivo da recusa. `bindex` é o
## índice O(1) de construções por produto (calculado 1× por cidade).
func _build_gate(city_id: String, city: Dictionary, day: int, product: int,
		bindex: Dictionary = {}) -> String:
	var last_build := int(_last_build_day.get(city_id, -BUILD_COOLDOWN_DAYS))
	if day - last_build < BUILD_COOLDOWN_DAYS:
		return "cooldown de construção (%d d desde a última)" % (day - last_build)
	if _buildings_of_type(city, product, bindex) >= MAX_BUILDINGS_PER_TYPE:
		return "teto de %d construções deste tipo" % MAX_BUILDINGS_PER_TYPE
	var utilization := _avg_utilization(city, product, bindex)
	if utilization < MIN_AVG_UTILIZATION:
		return "utilização média %.0f%% < %.0f%%" % [
			utilization * 100.0, MIN_AVG_UTILIZATION * 100.0]
	return ""


## Índice O(1) das construções locais por produto: {"counts": product→n,
## "util_sum": product→Σ util. efetiva, "util_n": product→n}. Construído 1× por
## cidade; _buildings_of_type/_avg_utilization o consultam em vez de varrer
## city["buildings"] a cada mineral (custo medido no laço quente).
func _building_index(city: Dictionary) -> Dictionary:
	var counts: Dictionary = {}
	var util_sum: Dictionary = {}
	var util_n: Dictionary = {}
	for building in city.get("buildings", []):
		var product := int(building.get("output_product", -1))
		if product < 0:
			continue
		counts[product] = int(counts.get(product, 0)) + 1
		var utilization := float(building.get("utilization", 0.0))
		var suitability := float(building.get("site_suitability", 1.0))
		var effective := clampf(utilization / maxf(suitability, 0.01), 0.0, 1.0)
		util_sum[product] = float(util_sum.get(product, 0.0)) + effective
		util_n[product] = int(util_n.get(product, 0)) + 1
	return {"counts": counts, "util_sum": util_sum, "util_n": util_n}


func _buildings_of_type(city: Dictionary, product: int, bindex: Dictionary = {}) -> int:
	if not bindex.is_empty():
		return int((bindex["counts"] as Dictionary).get(product, 0))
	var count := 0
	for building in city.get("buildings", []):
		if int(building.get("output_product", -1)) == product:
			count += 1
	return count


## Utilização média efetiva: normaliza pela aptidão do terreno (uma lavoura em
## região de aptidão 0,3 produz no máximo 30% da capacidade nominal — isso não é
## ociosidade). Transformações (site_suitability=1) ficam inalteradas. `bindex` é
## o índice O(1) de construções por produto (calculado 1× por cidade).
func _avg_utilization(city: Dictionary, product: int, bindex: Dictionary = {}) -> float:
	if not bindex.is_empty():
		var util_sum: Dictionary = bindex["util_sum"]
		var util_n: Dictionary = bindex["util_n"]
		var n: int = int(util_n.get(product, 0))
		if n == 0:
			return 1.0
		return float(util_sum.get(product, 0.0)) / float(n)
	var total := 0.0
	var count := 0
	for building in city.get("buildings", []):
		if int(building.get("output_product", -1)) != product:
			continue
		var utilization := float(building.get("utilization", 0.0))
		var suitability := float(building.get("site_suitability", 1.0))
		var effective := clampf(utilization / maxf(suitability, 0.01), 0.0, 1.0)
		total += effective
		count += 1
	if count == 0:
		return 1.0
	return total / float(count)


func _center(city: Dictionary) -> Vector2:
	var center: Array = city.get("center", [0.0, 0.0])
	if center.size() < 2:
		return Vector2.ZERO
	return Vector2(float(center[0]), float(center[1]))


func _log(day: int, city_id: String, action: String, resource: String,
		reason: String, cost: float) -> void:
	_decisions.append({
		"day": day,
		"agent": "government",
		"city": city_id,
		"action": action,
		"resource": resource,
		"target": "",
		"reason": reason,
		"cost": cost,
		"expected_daily_profit": 0.0,
		"risk": 0.0,
	})
