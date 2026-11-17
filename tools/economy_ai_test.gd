extends SceneTree

## Testes da IA econômica — agora lida pelo LOG DE COMANDOS da A3, porque a IA
## decidiu mudar de casa: privado, governo e grupos rodam no núcleo C++, dentro
## do `tick_day()` (A5). O teste continua com três cenários do mundo demo:
## 1. escassez de alimento -> investimento na melhor cultura da cesta (B3);
## 2. excesso de trigo -> IA não investe no setor em PORTUS;
## 3. descoberta de ferro -> governo abre estatal de mineração.
## Mais o anti-loop (B5).
##
## `sim.tick_day()` devolve o status do dia ANTES da fase da IA; o que a IA fez
## chega por `sim.get_command_log()` (Array de Dictionary com `type`, `day`,
## `apply_day`, `s`, `v` e `i`) e o estado corrente por `get_city_status(id)`.
##
## Uso: godot --headless --script res://tools/economy_ai_test.gd

const GeoScript := preload("res://core/economy/geography_service.gd")
const GroupNamerScript := preload("res://core/economy/group_namer.gd")

const AGRIA_CENTER := Vector2(2532.6, 571.4)
const PORTUS_CENTER := Vector2(2560.4, 543.8)

## Cesta alimentar (B3): trigo, arroz, milho, batata e soja — ids de produto do C++.
const FOOD_RESOURCES := ["wheat", "rice", "maize", "potato", "soy"]
const FOOD_PRODUCTS := [0, 2, 3, 16, 17]
## Produto `wheat` no núcleo (entrada 0 de `products`).
const WHEAT_PRODUCT := 0

## Número máximo de prédios ociosos (utilização < 10%) aceito por cidade após
## 730 dias. Sem o anti-loop (B5), o governo empilha uma estatal a cada ciclo e
## a contagem estoura em dezenas.
const MAX_IDLE_BUILDINGS := 10

var _failures: Array[String] = []


func _init() -> void:
	var food_log := _scenario_food_scarcity()
	var glut_log := _scenario_wheat_glut()
	var iron_log := _scenario_iron_discovery()
	var loop_log := _scenario_no_build_loop()
	_write_logs(food_log, glut_log, iron_log, loop_log)

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
	# Reduz a aptidão de trigo de Agria: a oferta de trigo cai. Mas a cesta
	# alimentar (B3) ainda tem milho/batata com aptidão alta, então a IA deve
	# investir na melhor cultura (aptidão × preço), não obrigatoriamente trigo.
	sim.set_suitability("AGRIA", "wheat", 0.30)
	# Zera o estoque inicial de trigo de Agria para que a escassez se manifeste
	# de imediato (sem o colchão de 100 t mascará-la enquanto a IA prefere ferro).
	_inject_stock(sim, "AGRIA", 0, 0.0)
	_boost_cash(sim, "AGRIA", 80000.0, 100000.0)
	var capacity_before := _food_capacity(sim, "AGRIA")
	_run(world, 60)

	var command_log: Array = sim.get_command_log()
	# `build` carrega o recurso em `s[2]`; `invest` só a cidade (`s[0]`).
	var food_builds := 0
	var agria_invests := 0
	for entry: Dictionary in command_log:
		var type := String(entry.get("type", ""))
		var slots: Array = entry.get("s", [])
		if type == "build" and slots.size() > 2 \
				and String(slots[2]) in FOOD_RESOURCES:
			food_builds += 1
		elif type == "invest" and slots.size() > 0 \
				and String(slots[0]) == "AGRIA":
			agria_invests += 1
	if food_builds == 0 and agria_invests == 0:
		_fail("alimento: IA não investiu apesar da escassez (nenhum build de comida, nenhum invest em AGRIA)")
	var capacity_after := _food_capacity(sim, "AGRIA")
	if capacity_after <= capacity_before + 0.5:
		_fail("alimento: capacidade alimentar não cresceu (%.1f -> %.1f)" % [
			capacity_before, capacity_after])
	print("[ai] alimento: %.1f -> %.1f t/dia de capacidade, %d build(s) de comida, %d invest(s) em AGRIA" % [
		capacity_before, capacity_after, food_builds, agria_invests])
	return command_log


func _scenario_wheat_glut() -> Array:
	var world := _make_world()
	var sim = world["sim"]
	_boost_cash(sim, "PORTUS", 200000.0, 150000.0)
	# Estoque herdado de um ciclo anterior de overprodução de trigo. O alvo de
	# estoque de 3 dias absorve 500 t em ~8 dias; por isso a janela do teste é
	# curta, para observar a decisão de cautela enquanto o excesso existe.
	_inject_stock(sim, "PORTUS", 0, 500.0)
	# Prédios de trigo de PORTUS ANTES da rodada: `invest` não diz o recurso,
	# então a checagem de cautela é pelo id do prédio expandido.
	var wheat_buildings := _wheat_building_ids(sim, "PORTUS")
	# O cenário demo é estruturalmente ~5% curto em produção diária (85,5 t de
	# capacidade para 90 t de necessidade); o estoque de 500 t cobre o consumo,
	# mas não é produção do dia. A IA pode, corretamente, expandir comida na
	# cidade deficitária (AGRIA). O contrato deste teste é o GLUT LOCAL: a
	# cidade com o excesso (PORTUS) não investe no setor.
	_run(world, 7)

	var command_log: Array = sim.get_command_log()
	# Mercado é detalhe pesado: vem sob demanda pelo id da cidade.
	var portus: Dictionary = sim.get_city_status("PORTUS")
	var portus_markets: Array = portus.get("markets", [])
	if portus_markets.is_empty():
		_fail("trigo: PORTUS sem mercados no status da cidade")
		return command_log
	var wheat: Dictionary = portus_markets[0]
	if float(wheat.get("surplus", 0.0)) <= 0.0:
		_fail("trigo: excesso de oferta não registrado")
	for entry: Dictionary in command_log:
		var type := String(entry.get("type", ""))
		var slots: Array = entry.get("s", [])
		if slots.size() < 2:
			continue
		if type == "build" and slots.size() > 2 \
				and String(slots[0]) == "PORTUS" and String(slots[2]) == "wheat":
			_fail("trigo: IA abriu lavoura de trigo em PORTUS mesmo com excesso de oferta")
			break
		if type == "invest" and String(slots[0]) == "PORTUS" \
				and String(slots[1]) in wheat_buildings:
			_fail("trigo: IA expandiu produtor de trigo em PORTUS mesmo com excesso de oferta")
			break
	print("[ai] trigo: excesso %.1f t, nenhum investimento em trigo em PORTUS" % [
		float(wheat.get("surplus", 0.0))])
	return command_log


func _scenario_iron_discovery() -> Array:
	var world := _make_world()
	var sim = world["sim"]
	_boost_cash(sim, "AGRIA", 20000.0, 200000.0)
	# Depósito de ferro no território de Agria (conhecido desde o início, sem
	# prospecção: aptidão + reserva declaradas direto no núcleo).
	sim.set_suitability("AGRIA", "iron", 1.0)
	sim.set_deposit("AGRIA", "iron", 5000.0, 0.80, 200.0)
	var budget_before := _government_budget(sim, "AGRIA")
	_run(world, 42)

	var command_log: Array = sim.get_command_log()
	var status: Dictionary = sim.get_status()
	# Estatal: `build` carrega o dono em `s[3]` ("state"/"private").
	var state_builds := 0
	for entry: Dictionary in command_log:
		var slots: Array = entry.get("s", [])
		if String(entry.get("type", "")) != "build" or slots.size() <= 3:
			continue
		if String(slots[2]) == "iron" and String(slots[3]) == "state":
			state_builds += 1
	if state_builds == 0:
		_fail("ferro: governo não emitiu Build estatal de ferro")
	var mines := _buildings_with_resource(sim, status, "iron")
	if mines.is_empty():
		_fail("ferro: nenhuma mina foi criada após a descoberta")
		return command_log
	var has_state := false
	for building in mines:
		if String(building.get("owner", "")) == "state":
			has_state = true
	if not has_state:
		_fail("ferro: a mina criada não é estatal")
	if float(Array(status["produced_total"])[1]) <= 0.0:
		_fail("ferro: mina não produziu")
	var budget_after := _government_budget(sim, "AGRIA")
	if budget_after >= budget_before:
		_fail("ferro: orçamento público não foi consumido")
	if float(status.get("foreign_cash", 0.0)) >= 0.0:
		_fail("ferro: exportação não gerou fluxo externo")
	print("[ai] ferro: %d Build estatal, %d mina(s), orçamento %.0f -> %.0f, produção %.2f t" % [
		state_builds, mines.size(), budget_before, budget_after,
		float(Array(status["produced_total"])[1])])
	return command_log


## B5 — anti-loop: 730 dias de escassez persistente com caixa alto. Sem o teto,
## cooldown e piso de utilização, o governo criaria uma estatal por ciclo
## (centenas) e a maioria ficaria ociosa.
func _scenario_no_build_loop() -> Array:
	var world := _make_world()
	var sim = world["sim"]
	# Aptidão baixa + estoque zerado → escassez crônica em ambas as cidades.
	sim.set_suitability("AGRIA", "wheat", 0.30)
	sim.set_suitability("PORTUS", "wheat", 0.30)
	_inject_stock(sim, "AGRIA", 0, 0.0)
	_inject_stock(sim, "PORTUS", 0, 0.0)
	_boost_cash(sim, "AGRIA", 500000.0, 500000.0)
	_boost_cash(sim, "PORTUS", 500000.0, 500000.0)
	_run(world, 730)

	var status: Dictionary = sim.get_status()
	var worst_idle := 0
	for city in status.get("cities", []):
		var city_id := String(city.get("id", ""))
		var idle := 0
		var total := 0
		# Construções vêm do detalhe pesado sob demanda (não do status diário).
		for building in sim.get_city_status(city_id).get("buildings", []):
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
	return sim.get_command_log()


func _make_world() -> Dictionary:
	var sim = EconomySim.new()
	# §15: o teste mede a DECISÃO da IA, não o cronograma de obra — construção
	# instantânea para a mina estatal nascer e produzir dentro do horizonte.
	sim.set_instant_construction(true)
	sim.initialize_scenario()
	var geo = GeoScript.new()
	geo.apply_to_sim(sim, [
		{"id": "AGRIA", "center": AGRIA_CENTER},
		{"id": "PORTUS", "center": PORTUS_CENTER},
	])
	# A IA (privado, governo, grupos) roda no núcleo, como fase do dia (A5);
	# os grupos recebem nome pela cultura de origem (só o nome é GDScript).
	sim.set_ai_enabled(true)
	GroupNamerScript.name_groups(sim)
	return {"sim": sim, "geo": geo}


## Um dia: `tick_day()` já roda a fase da IA depois de publicar o status.
func _run(world: Dictionary, days: int) -> void:
	var sim = world["sim"]
	for _index in days:
		sim.tick_day()


func _boost_cash(sim, city_id: String, firm: float, government: float) -> void:
	var snapshot: Dictionary = sim.to_dict()
	for city in snapshot["cities"]:
		if String(city["id"]) == city_id:
			city["firm_cash"] = firm
			city["government_budget"] = government
	sim.from_dict(snapshot)


func _inject_stock(sim, city_id: String, product: int, amount: float) -> void:
	var snapshot: Dictionary = sim.to_dict()
	for city in snapshot["cities"]:
		if String(city["id"]) == city_id:
			city["markets"][product]["stock"] = amount
	sim.from_dict(snapshot)


func _food_capacity(sim, city_id: String) -> float:
	var total := 0.0
	for building in sim.get_city_status(city_id).get("buildings", []):
		if int(building.get("output_product", -1)) in FOOD_PRODUCTS:
			total += float(building.get("capacity", 0.0))
	return total


## Ids dos prédios de trigo de uma cidade (recurso declarado ou produto `wheat`).
func _wheat_building_ids(sim, city_id: String) -> Array:
	var ids: Array = []
	for building in sim.get_city_status(city_id).get("buildings", []):
		var resource := String(building.get("resource", ""))
		var product := int(building.get("output_product", -1))
		if resource == "wheat" or product == WHEAT_PRODUCT:
			ids.append(String(building.get("id", "")))
	return ids


func _government_budget(sim, city_id: String) -> float:
	return float(_city(sim.get_status(), city_id).get("government_budget", 0.0))


func _buildings_with_resource(sim, status: Dictionary, resource: String) -> Array:
	var found: Array = []
	for city in status.get("cities", []):
		var city_id := String(city.get("id", ""))
		for building in sim.get_city_status(city_id).get("buildings", []):
			if String(building.get("resource", "")) == resource:
				found.append(building)
	return found


func _city(status: Dictionary, city_id: String) -> Dictionary:
	for city in status.get("cities", []):
		if String(city["id"]) == city_id:
			return city
	return {}


## Log de comandos de cada cenário (o que a IA fez, da A3). Não é commitado:
## fica como evidência local da rodada.
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
