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
const IRON_MINE_CAPACITY := 10.0
const HIGH_UNEMPLOYMENT := 0.65

const Templates := preload("res://core/economy/ai/industry_templates.gd")

var _sim: Object
var _decisions: Array[Dictionary] = []
var _id_counter := 1


func setup(sim: Object) -> void:
	_sim = sim


func run(day: int, status: Dictionary) -> void:
	for city in status.get("cities", []):
		_run_city(day, city, status)


func drain() -> Array:
	var output: Array = _decisions.duplicate(true)
	_decisions.clear()
	return output


func _run_city(day: int, city: Dictionary, status: Dictionary) -> void:
	var city_id := String(city["id"])
	var budget := float(city["government_budget"])
	var available := budget * BUDGET_SHARE
	var capital_cost := float(status.get("capital_cost_per_capacity", 2000.0))
	var center := _center(city)
	if available <= 0.0:
		return

	# 1. Segurança alimentar: estatal quando há escassez e o setor privado
	# não cobre (ou quando o governo é o comprador estratégico).
	var food_candidates: Array = []
	for resource in Templates.FOOD_RESOURCES:
		var product := Templates.output_of(resource)
		var market: Dictionary = city["markets"][product]
		var shortage := float(market.get("shortage", 0.0))
		var surplus := float(market.get("surplus", 0.0))
		if shortage < MIN_SHORTAGE_TONS or surplus > 0.0:
			continue
		# Necessidade nacional: não constrói se outra cidade já tem excesso.
		var system_shortage := 0.0
		var system_surplus := 0.0
		for other in status.get("cities", []):
			var other_market: Dictionary = other["markets"][product]
			system_shortage += maxf(0.0, float(other_market.get("shortage", 0.0)))
			system_surplus += maxf(0.0, float(other_market.get("surplus", 0.0)))
		if system_shortage <= system_surplus:
			continue
		var template: Dictionary = Templates.TEMPLATES[resource]
		if not _input_available(city, int(template["input"])):
			continue
		var capacity := clampf(shortage, MIN_STATE_CAPACITY, MAX_STATE_CAPACITY)
		if capacity * capital_cost > available:
			capacity = available / capital_cost
		if capacity < MIN_STATE_CAPACITY:
			continue
		food_candidates.append({
			"resource": resource,
			"capacity": capacity,
			"cost": capacity * capital_cost,
			"shortage": shortage,
		})
	# Trigo é estratégico: prioriza o primeiro da lista.
	for candidate in food_candidates:
		var resource := String(candidate["resource"])
		var template: Dictionary = Templates.TEMPLATES[resource]
		var product := int(template["output"])
		var capacity := float(candidate["capacity"])
		var cost := float(candidate["cost"])
		var new_id := "STATE-%s-%d" % [resource.to_upper(), _id_counter]
		var result: Dictionary = _sim.demo_build(city_id, new_id, resource,
			int(template["input"]), product, capacity,
			capacity * float(template["jobs_per_ton"]), float(template["wage"]),
			capacity * float(template["opex_per_ton"]),
			float(template["input_ratio"]), "state", cost)
		var error := String(result.get("error", ""))
		if error != "":
			_log(day, city_id, "rejected", resource, error, cost)
			continue
		_id_counter += 1
		_log(day, city_id, "build_state", resource,
			"segurança alimentar: escassez %.1f t, emprego e demanda nacional" % float(candidate["shortage"]),
			cost)
		return

	# 3. Mineração estratégica: mina estatal se o território tem ferro
	# (aptidão > 0, aplicada na inicialização), margem positiva e sem produção.
	var suitability: Array = city.get("suitability", [0.0])
	if suitability.size() > 3 and float(suitability[3]) > 0.0:
		var iron_market: Dictionary = city["markets"][3]
		var has_mine := false
		for building in city.get("buildings", []):
			if int(building.get("output_product", -1)) == 3:
				has_mine = true
				break
		var margin: float = (float(iron_market["price"])
			- Templates.variable_cost("iron", city))
		if not has_mine and margin > 0.0 and float(iron_market.get("surplus", 0.0)) <= 0.0:
			var capacity := minf(IRON_MINE_CAPACITY, available / capital_cost)
			if capacity >= MIN_STATE_CAPACITY:
				var cost := capacity * capital_cost
				var new_id := "STATE-IRON-%d" % _id_counter
				var result: Dictionary = _sim.demo_build(city_id, new_id, "iron",
					-1, 3, capacity, capacity * 3.35, 1.0, capacity * 6.65,
					0.0, "state", cost)
				var error := String(result.get("error", ""))
				if error == "":
					_id_counter += 1
					_log(day, city_id, "build_state", "iron",
						"recurso estratégico descoberto, margem %.2f/t" % margin, cost)
					return
				_log(day, city_id, "rejected", "iron", error, cost)

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
