extends SceneTree

## §15 — construção como projeto físico. Guarda de regressão do cronograma de
## obra: (1) sem material a obra ATRASA (não falha); (2) o capital é RESERVADO
## no início (não pausa por liquidez — o custo saiu do caixa na criação); (3) ao
## concluir o prédio ENTRA EM OPERAÇÃO (produz).
##
##   godot --headless --path . --script res://tools/construction_project_test.gd

const GoodsPath := "res://data/definitions/economy/goods.json"
const RecipesPath := "res://data/definitions/economy/recipes.json"
const NeedsPath := "res://data/definitions/economy/needs.json"

const WHEAT := 0  # ordem estável: trigo é o produto 0

var _failures: Array[String] = []


func _init() -> void:
	_test_project_lifecycle()
	_test_money_conservation()
	if _failures.is_empty():
		print("[construction] OK: material -> atrasa; capital reservado -> não pausa; "
			+ "conclusao -> opera; dinheiro conservado.")
		quit(0)
	else:
		for failure in _failures:
			print("[construction] FALHA: " + failure)
		quit(1)


func _fail(msg: String) -> void:
	_failures.append(msg)


func _load(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("nao abriu " + path)
		return null
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data


func _new_sim() -> EconomySim:
	var sim := EconomySim.new()
	sim.set_goods(_load(GoodsPath))
	sim.set_recipes(_load(RecipesPath))
	sim.set_needs(_load(NeedsPath))
	# Padrão: construção física (projeto), não instantânea.
	sim.set_instant_construction(false)
	sim.initialize_scenario()
	return sim


func _projects(sim: EconomySim) -> Array:
	return sim.to_dict().get("construction_projects", [])


func _project(sim: EconomySim) -> Dictionary:
	var projects: Array = _projects(sim)
	return projects[0] if projects.size() > 0 else {}


func _set_government_budget(sim: EconomySim, city_id: String, budget: float) -> void:
	var snapshot: Dictionary = sim.to_dict()
	for city in snapshot["cities"]:
		if String(city["id"]) == city_id:
			city["government_budget"] = budget
	sim.from_dict(snapshot)


func _seed_materials(sim: EconomySim, tons: float) -> void:
	sim.set_market_stock("AGRIA", "construction_material", tons)
	sim.set_market_stock("AGRIA", "steel_structure", tons)
	sim.set_market_stock("AGRIA", "electrical_material", tons)


func _test_project_lifecycle() -> void:
	var sim := _new_sim()
	# 1. cria a obra (projeto físico, custo 10.000).
	var result: Dictionary = sim.build("AGRIA", "T-FARM", "wheat", -1, WHEAT,
		5.0, 0.0, 0.0, 0.0, 0.0, "state", 10000.0)
	if not bool(result.get("created", false)):
		_fail("obra: criação falhou (%s)" % String(result.get("error", "")))
		return
	if _projects(sim).size() != 1:
		_fail("obra: esperava 1 projeto em andamento, veio %d" % _projects(sim).size())
		return

	# 2. sem material -> ATRASA (não falha): dias não avançam e motivo é material.
	sim.tick_day()
	var project: Dictionary = _project(sim)
	if int(project.get("days_elapsed", -1)) != 0:
		_fail("sem material: projeto avançou (dias=%s)" % str(project.get("days_elapsed")))
	if not bool(project.get("paused", false)) \
			or not String(project.get("pause_reason", "")).begins_with("sem material"):
		_fail("sem material: projeto não marcou pausa por material (%s)" % String(
			project.get("pause_reason", "")))

	# 3. material entregue -> avança (pausa desfeita).
	_seed_materials(sim, 100.0)
	sim.tick_day()
	project = _project(sim)
	if int(project.get("days_elapsed", -1)) <= 0:
		_fail("material entregue: projeto não avançou (dias=%s)" % str(
			project.get("days_elapsed")))
	if bool(project.get("paused", false)):
		_fail("material entregue: projeto ainda pausado")

	# 4. caixa do dono zerado DEPOIS do início -> a obra NÃO pausa: o capital
	# já foi reservado no início (investimento, não caixa de giro). O `reserved`
	# cobre o avanço; dias continuam.
	var elapsed_before := int(_project(sim).get("days_elapsed", -1))
	_set_government_budget(sim, "AGRIA", 0.0)
	sim.tick_day()
	project = _project(sim)
	if int(project.get("days_elapsed", -1)) <= elapsed_before:
		_fail("capital reservado: projeto não avançou (dias %d -> %d)" % [
			elapsed_before, int(project.get("days_elapsed", -1))])
	if bool(project.get("paused", false)):
		_fail("capital reservado: projeto pausou (%s)" % String(
			project.get("pause_reason", "")))

	# 5. devolve o dinheiro -> retoma e conclui.
	_set_government_budget(sim, "AGRIA", 1000000.0)
	_seed_materials(sim, 100000.0)
	for day in 40:
		sim.tick_day()
		if _projects(sim).is_empty():
			break
	if not _projects(sim).is_empty():
		_fail("conclusão: projeto não terminou após 40 dias (dias=%s)" % str(
			_project(sim).get("days_elapsed")))

	# 6. conclusão -> prédio entra em operação (existe e produz trigo).
	var found := false
	for building in sim.get_city_status("AGRIA").get("buildings", []):
		if String(building.get("id", "")) == "T-FARM":
			found = true
			if float(building.get("capacity", 0.0)) < 5.0:
				_fail("conclusão: capacidade do prédio errada (%s)" % str(
					building.get("capacity")))
	if not found:
		_fail("conclusão: prédio não entrou em operação")
	var produced: Array = sim.get_status().get("produced_total", [])
	if produced.size() <= WHEAT or float(produced[WHEAT]) <= 0.0:
		_fail("conclusão: prédio não produziu trigo")


## O pagamento incremental sai do dono conforme a obra avança e entra em
## material (firma) + serviço de construção — o dinheiro total é conservado.
func _test_money_conservation() -> void:
	var sim := _new_sim()
	_seed_materials(sim, 100000.0)
	var money_before := float(sim.get_status().get("money_total", 0.0))
	var result: Dictionary = sim.build("AGRIA", "T-FARM2", "wheat", -1, WHEAT,
		5.0, 0.0, 0.0, 0.0, 0.0, "state", 10000.0)
	if not bool(result.get("created", false)):
		_fail("conservação: obra não criada (%s)" % String(result.get("error", "")))
		return
	for day in 30:
		sim.tick_day()
		var violations: Array = sim.get_status().get("invariant_violations", [])
		if violations.size() > 0:
			_fail("conservação: invariante violado no dia %d: %s" % [
				day + 1, String(violations[0])])
			return
	var money_after := float(sim.get_status().get("money_total", 0.0))
	if absf(money_after - money_before) > 0.01:
		_fail("conservação: dinheiro não conservado (%.2f -> %.2f)" % [
			money_before, money_after])
