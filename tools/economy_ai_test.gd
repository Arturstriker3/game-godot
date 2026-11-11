extends SceneTree

## Testes da camada de IA econômica em três cenários:
## 1. escassez de alimento -> investimento na melhor cultura da cesta (B3);
## 2. excesso de trigo -> IA evita investir no setor;
## 3. descoberta de ferro -> governo cria estatal de mineração.
##
## Uso: godot --headless --script res://tools/economy_ai_test.gd

const GeoScript := preload("res://core/economy/geography_service.gd")
const AIScript := preload("res://core/economy/ai/economic_ai.gd")

const AGRIA_CENTER := Vector2(2532.6, 571.4)
const PORTUS_CENTER := Vector2(2560.4, 543.8)

## Cesta alimentar (B3): trigo, arroz, milho e batata — ids de produto do C++.
const FOOD_RESOURCES := ["wheat", "rice", "maize", "potato"]
const FOOD_PRODUCTS := [0, 2, 3, 16]

## Número máximo de prédios ociosos (utilização < 10%) aceito por cidade após
## 730 dias. Sem o anti-loop (B5), o governo empilha uma estatal a cada ciclo e
## a contagem estoura em dezenas.
const MAX_IDLE_BUILDINGS := 10

var _failures: Array[String] = []


func _init() -> void:
	var food_decisions := _scenario_food_scarcity()
	var glut_decisions := _scenario_wheat_glut()
	var iron_decisions := _scenario_iron_discovery()
	var loop_decisions := _scenario_no_build_loop()
	_write_logs(food_decisions, glut_decisions, iron_decisions, loop_decisions)

	if _failures.is_empty():
		print("[ai] OK: cenários de alimento, ferro e anti-loop reagiram de forma plausível.")
		quit(0)
	else:
		print("[ai] FALHAS (%d):" % _failures.size())
		for failure in _failures:
			print(" - " + failure)
		quit(1)


func _scenario_food_scarcity() -> Array:
	var world := _make_world()
	var sim = world["sim"]
	var ai = world["ai"]
	# Reduz a aptidão de trigo de Agria: a oferta de trigo cai. Mas a cesta
	# alimentar (B3) ainda tem milho/batata com aptidão alta, então a IA deve
	# investir na melhor cultura (aptidão × preço), não obrigatoriamente trigo.
	sim.demo_set_suitability("AGRIA", "wheat", 0.30)
	# Zera o estoque inicial de trigo de Agria para que a escassez se manifeste
	# de imediato (sem o colchão de 100 t mascará-la enquanto a IA prefere ferro).
	_inject_stock(sim, "AGRIA", 0, 0.0)
	_boost_cash(sim, "AGRIA", 80000.0, 100000.0)
	var capacity_before := _food_capacity(sim, "AGRIA")
	_run(world, 60)

	var decisions: Array = ai.get_decisions()
	var food_actions := _filter_food(decisions)
	var invested := false
	for decision in food_actions:
		var action := String(decision.get("action", ""))
		if action in ["build", "expand", "build_state"]:
			invested = true
	if not invested:
		_fail("alimento: IA não investiu apesar da escassez")
	var capacity_after := _food_capacity(sim, "AGRIA")
	if capacity_after <= capacity_before + 0.5:
		_fail("alimento: capacidade alimentar não cresceu (%.1f -> %.1f)" % [
			capacity_before, capacity_after])
	print("[ai] alimento: %.1f -> %.1f t/dia de capacidade, %d decisão(ões)" % [
		capacity_before, capacity_after, food_actions.size()])
	return decisions


func _scenario_wheat_glut() -> Array:
	var world := _make_world()
	var sim = world["sim"]
	var ai = world["ai"]
	_boost_cash(sim, "PORTUS", 200000.0, 150000.0)
	# Estoque herdado de um ciclo anterior de overprodução de trigo. O alvo de
	# estoque de 3 dias absorve 500 t em ~8 dias; por isso a janela do teste é
	# curta, para observar a decisão de cautela enquanto o excesso existe.
	_inject_stock(sim, "PORTUS", 0, 500.0)
	_run(world, 7)

	var decisions: Array = ai.get_decisions()
	var status: Dictionary = sim.get_demo_status()
	var portus := _city(status, "PORTUS")
	var wheat: Dictionary = portus["markets"][0]
	if float(wheat.get("surplus", 0.0)) <= 0.0:
		_fail("trigo: excesso de oferta não registrado")
	if not _has(decisions, "private", "avoid", "wheat"):
		_fail("trigo: IA não registrou a decisão de evitar o setor")
	for decision in decisions:
		if String(decision.get("resource", "")) != "wheat":
			continue
		if String(decision.get("action", "")) in ["build", "expand"]:
			_fail("trigo: IA investiu mesmo com excesso de oferta")
	print("[ai] trigo: excesso %.1f t, decisões de cautela registradas" % [
		float(wheat.get("surplus", 0.0))])
	return decisions


func _scenario_iron_discovery() -> Array:
	var world := _make_world()
	var sim = world["sim"]
	var ai = world["ai"]
	_boost_cash(sim, "AGRIA", 20000.0, 200000.0)
	# Depósito de ferro no território de Agria (conhecido desde o início, sem
	# prospecção: aptidão + reserva declaradas direto no núcleo).
	sim.demo_set_suitability("AGRIA", "iron", 1.0)
	sim.demo_set_deposit("AGRIA", "iron", 5000.0, 0.80, 200.0)
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
	if float(Array(status["produced_total"])[1]) <= 0.0:
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
		float(Array(status["produced_total"])[1])])
	return decisions


## B5 — anti-loop: 730 dias de escassez persistente com caixa alto. Sem o teto,
## cooldown e piso de utilização, o governo criaria uma estatal por ciclo
## (centenas) e a maioria ficaria ociosa.
func _scenario_no_build_loop() -> Array:
	var world := _make_world()
	var sim = world["sim"]
	var ai = world["ai"]
	# Aptidão baixa + estoque zerado → escassez crônica em ambas as cidades.
	sim.demo_set_suitability("AGRIA", "wheat", 0.30)
	sim.demo_set_suitability("PORTUS", "wheat", 0.30)
	_inject_stock(sim, "AGRIA", 0, 0.0)
	_inject_stock(sim, "PORTUS", 0, 0.0)
	_boost_cash(sim, "AGRIA", 500000.0, 500000.0)
	_boost_cash(sim, "PORTUS", 500000.0, 500000.0)
	_run(world, 730)

	var status: Dictionary = sim.get_demo_status()
	var worst_idle := 0
	for city in status.get("cities", []):
		var city_id := String(city.get("id", ""))
		var idle := 0
		var total := 0
		for building in city.get("buildings", []):
			total += 1
			if float(building.get("utilization", 0.0)) < 0.10:
				idle += 1
		worst_idle = maxi(worst_idle, idle)
		print("[ai] anti-loop: %s %d/%d prédios ociosos (<10%%)" % [city_id, idle, total])
		if idle > MAX_IDLE_BUILDINGS:
			_fail("anti-loop: %s tem %d prédios ociosos após 730 d (teto %d)" % [
				city_id, idle, MAX_IDLE_BUILDINGS])
	if worst_idle > MAX_IDLE_BUILDINGS:
		_fail("anti-loop: pior cidade com %d prédios ociosos" % worst_idle)
	return ai.get_decisions()


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


func _food_capacity(sim, city_id: String) -> float:
	var status: Dictionary = sim.get_demo_status()
	var city := _city(status, city_id)
	var total := 0.0
	for building in city.get("buildings", []):
		if int(building.get("output_product", -1)) in FOOD_PRODUCTS:
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


func _filter_food(decisions: Array) -> Array:
	var filtered: Array = []
	for decision in decisions:
		if String(decision.get("resource", "")) in FOOD_RESOURCES:
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


func _write_logs(food: Array, glut: Array, iron: Array, loop: Array) -> void:
	var directory := "res://docs/economy_ai"
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(directory))
	var payload := {
		"food_scarcity": food,
		"wheat_glut": glut,
		"iron_discovery": iron,
		"no_build_loop": loop,
	}
	var file := FileAccess.open("%s/decisions.json" % directory, FileAccess.WRITE)
	if file != null:
		file.store_string(JSON.stringify(payload, "  "))
		file.close()


func _fail(message: String) -> void:
	_failures.append(message)
