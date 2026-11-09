extends SceneTree

## Testes da camada de IA econômica em três cenários:
## 1. escassez de trigo -> investimento em capacidade de trigo;
## 2. excesso de farinha -> IA evita investir no setor;
## 3. descoberta de ferro -> governo cria estatal de mineração.
##
## Uso: godot --headless --script res://tools/economy_ai_test.gd

const GeoScript := preload("res://core/economy/geography_service.gd")
const AIScript := preload("res://core/economy/ai/economic_ai.gd")

const AGRIA_CENTER := Vector2(2532.6, 571.4)
const PORTUS_CENTER := Vector2(2560.4, 543.8)

var _failures: Array[String] = []


func _init() -> void:
	var wheat_decisions := _scenario_wheat_scarcity()
	var glut_decisions := _scenario_flour_glut()
	var iron_decisions := _scenario_iron_discovery()
	_write_logs(wheat_decisions, glut_decisions, iron_decisions)

	if _failures.is_empty():
		print("[ai] OK: cenários de trigo, farinha e ferro reagiram de forma plausível.")
		quit(0)
	else:
		print("[ai] FALHAS (%d):" % _failures.size())
		for failure in _failures:
			print(" - " + failure)
		quit(1)


func _scenario_wheat_scarcity() -> Array:
	var world := _make_world()
	var sim = world["sim"]
	var ai = world["ai"]
	# Reduz a aptidão real de Agria: a oferta de trigo fica abaixo da demanda.
	sim.demo_set_suitability("AGRIA", "wheat", 0.30)
	_boost_cash(sim, "AGRIA", 80000.0, 100000.0)
	var capacity_before := _capacity(sim, "AGRIA", 0)
	_run(world, 60)

	var decisions: Array = ai.get_decisions()
	var wheat_actions := _filter_resource(decisions, "wheat")
	var invested := false
	for decision in wheat_actions:
		var action := String(decision.get("action", ""))
		if action in ["build", "expand", "build_state"]:
			invested = true
	if not invested:
		_fail("trigo: IA não investiu apesar da escassez")
	var capacity_after := _capacity(sim, "AGRIA", 0)
	if capacity_after <= capacity_before + 0.5:
		_fail("trigo: capacidade de trigo não cresceu (%.1f -> %.1f)" % [
			capacity_before, capacity_after])
	print("[ai] trigo: %.1f -> %.1f t/dia de capacidade, %d decisão(ões)" % [
		capacity_before, capacity_after, wheat_actions.size()])
	return decisions


func _scenario_flour_glut() -> Array:
	var world := _make_world()
	var sim = world["sim"]
	var ai = world["ai"]
	_boost_cash(sim, "PORTUS", 200000.0, 150000.0)
	# Estoque herdado de um ciclo anterior de overprodução.
	_inject_stock(sim, "PORTUS", 1, 500.0)
	_run(world, 30)

	var decisions: Array = ai.get_decisions()
	var status: Dictionary = sim.get_demo_status()
	var portus := _city(status, "PORTUS")
	var flour: Dictionary = portus["markets"][1]
	if float(flour.get("surplus", 0.0)) <= 0.0:
		_fail("farinha: excesso de oferta não registrado")
	if not _has(decisions, "private", "avoid", "flour"):
		_fail("farinha: IA não registrou a decisão de evitar o setor")
	for decision in decisions:
		if String(decision.get("resource", "")) != "flour":
			continue
		if String(decision.get("action", "")) in ["build", "expand"]:
			_fail("farinha: IA investiu mesmo com excesso de oferta")
	print("[ai] farinha: excesso %.1f t, decisões de cautela registradas" % [
		float(flour.get("surplus", 0.0))])
	return decisions


func _scenario_iron_discovery() -> Array:
	var world := _make_world()
	var sim = world["sim"]
	var ai = world["ai"]
	_boost_cash(sim, "AGRIA", 20000.0, 200000.0)
	# Descoberta: depósito de ferro no território de Agria.
	ai.discover("AGRIA", "iron", {
		"id": "SYN-IRON-1",
		"resource": "iron",
		"quality": 0.80,
		"recoverable_kt": 5000.0,
		"max_rate_kt_day": 30.0,
		"extraction_cost": 10.0,
		"discovery_difficulty": 0.20,
	})
	var budget_before := _government_budget(sim, "AGRIA")
	_run(world, 42)

	var decisions: Array = ai.get_decisions()
	var status: Dictionary = sim.get_demo_status()
	var mines := _buildings_with_resource(status, "iron")
	if mines.is_empty():
		_fail("ferro: nenhuma mina foi criada após a descoberta")
		return decisions
	var has_state := false
	for building in mines:
		if String(building.get("owner", "")) == "state":
			has_state = true
	if not has_state:
		_fail("ferro: a mina criada não é estatal")
	if float(Array(status["produced_total"])[3]) <= 0.0:
		_fail("ferro: mina não produziu")
	if not _has(decisions, "government", "build_state", "iron"):
		_fail("ferro: governo não registrou a estatal de mineração")
	var budget_after := _government_budget(sim, "AGRIA")
	if budget_after >= budget_before:
		_fail("ferro: orçamento público não foi consumido")
	if float(status.get("foreign_cash", 0.0)) >= 0.0:
		_fail("ferro: exportação não gerou fluxo externo")
	print("[ai] ferro: %d mina(s), orçamento %.0f -> %.0f, produção %.2f t" % [
		mines.size(), budget_before, budget_after,
		float(Array(status["produced_total"])[3])])
	return decisions


func _make_world() -> Dictionary:
	var sim = EconomySim.new()
	sim.initialize_demo()
	var geo = GeoScript.new()
	geo.apply_to_sim(sim, [
		{"id": "AGRIA", "center": AGRIA_CENTER},
		{"id": "PORTUS", "center": PORTUS_CENTER},
	])
	var ai = AIScript.new()
	if not ai.setup(sim, geo):
		_fail("IA: geografia não carregou")
	return {"sim": sim, "geo": geo, "ai": ai}


func _run(world: Dictionary, days: int) -> void:
	for index in days:
		var status: Dictionary = world["sim"].tick_demo()
		world["ai"].run(int(status["calendar_day"]))


func _boost_cash(sim, city_id: String, firm: float, government: float) -> void:
	var snapshot: Dictionary = sim.demo_to_dict()
	for city in snapshot["cities"]:
		if String(city["id"]) == city_id:
			city["firm_cash"] = firm
			city["government_budget"] = government
	sim.demo_from_dict(snapshot)


func _inject_stock(sim, city_id: String, product: int, amount: float) -> void:
	var snapshot: Dictionary = sim.demo_to_dict()
	for city in snapshot["cities"]:
		if String(city["id"]) == city_id:
			city["markets"][product]["stock"] = amount
	sim.demo_from_dict(snapshot)


func _capacity(sim, city_id: String, product: int) -> float:
	var status: Dictionary = sim.get_demo_status()
	var city := _city(status, city_id)
	var total := 0.0
	for building in city.get("buildings", []):
		if int(building.get("output_product", -1)) == product:
			total += float(building.get("capacity", 0.0))
	return total


func _government_budget(sim, city_id: String) -> float:
	return float(_city(sim.get_demo_status(), city_id).get("government_budget", 0.0))


func _buildings_with_resource(status: Dictionary, resource: String) -> Array:
	var found: Array = []
	for city in status.get("cities", []):
		for building in city.get("buildings", []):
			if String(building.get("resource", "")) == resource:
				found.append(building)
	return found


func _filter_resource(decisions: Array, resource: String) -> Array:
	var filtered: Array = []
	for decision in decisions:
		if String(decision.get("resource", "")) == resource:
			filtered.append(decision)
	return filtered


func _has(decisions: Array, agent: String, action: String, resource: String) -> bool:
	for decision in decisions:
		if String(decision.get("agent", "")) == agent \
				and String(decision.get("action", "")) == action \
				and String(decision.get("resource", "")) == resource:
			return true
	return false


func _city(status: Dictionary, city_id: String) -> Dictionary:
	for city in status.get("cities", []):
		if String(city["id"]) == city_id:
			return city
	return {}


func _write_logs(wheat: Array, glut: Array, iron: Array) -> void:
	var directory := "res://docs/economy_ai"
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(directory))
	var payload := {
		"wheat_scarcity": wheat,
		"flour_glut": glut,
		"iron_discovery": iron,
	}
	var file := FileAccess.open("%s/decisions.json" % directory, FileAccess.WRITE)
	if file != null:
		file.store_string(JSON.stringify(payload, "  "))
		file.close()


func _fail(message: String) -> void:
	_failures.append(message)
