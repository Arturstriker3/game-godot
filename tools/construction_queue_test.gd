extends SceneTree

## §15 unificado — MORADIA, PRÉDIO E INFRA NA MESMA FILA DA CIDADE.
##
## Guarda de aceite da unificação das obras:
##   (1) fábrica e moradia disputam a MESMA capacidade do setor de construção:
##       com capacidade curta, a que começou depois PAUSA ("sem capacidade de
##       construç��o") e a primeira avança — a ordem de início decide;
##   (2) as duas compram material do MESMO mercado local (o estoque cai);
##   (3) a moradia paga TERRA à prefeitura: `land_cost` = fatia do orçamento,
##       `reserved` = total - terra e o caixa municipal recebe a terra;
##   (4) o dono paga o custo inteiro na abertura (caixa das firmas / prefeitura);
##   (5) save/load: a fila volta idêntica (round-trip 0 diffs);
##   (6) determinismo: threads=1 e 8 produzem o mesmo estado.
##
##   godot --headless --path . --script res://tools/construction_queue_test.gd

const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const PORTS_PATH := "res://data/definitions/scenario/ports.json"
const BUILDINGS_PATH := "res://data/definitions/scenario/buildings.json"
const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"
const SERVICES_PATH := "res://data/definitions/economy/services.json"
const CURRENCIES_PATH := "res://data/definitions/economy/currencies.json"
const SECTOR_EMPLOYMENT_PATH := "res://data/definitions/economy/sector_employment.json"
const URBAN_LAND_PATH := "res://data/definitions/economy/urban_land.json"
const HOUSING_CITY_PATH := "res://data/definitions/economy/housing_city.json"
const HOUSING_PATH := "res://data/definitions/economy/housing.json"
const MUNICIPAL_FINANCE_PATH := "res://data/definitions/economy/municipal_finance.json"

const CITY := "BRA-SAO"
const WARM_DAYS := 30        # um fechamento mensal: preço da terra > 0
const TOL := 1.0e-6
## XGS/dia por trabalhador do setor de construção (§15,
## kDemoConstructionValuePerWorkerDay em sim_constants.h).
const VALUE_PER_WORKER_DAY := 20.0
const FACTORY_COST := 900000.0
const HOUSING_UNITS := 20000.0

var _failures: Array[String] = []
var _goods_order: Dictionary = {}


func _init() -> void:
	_test_v13_migration()
	_test_queue_competition()
	_test_roundtrip()
	_test_thread_determinism()
	if _failures.is_empty():
		print("[queue] OK: capacidade compartilhada (uma atrasa), mesmo mercado, "
			+ "terra paga a prefeitura, dono paga, round-trip e threads=1/8.")
		quit(0)
	else:
		for failure in _failures:
			print("[queue] FALHA: " + failure)
		quit(1)


func _fail(msg: String) -> void:
	_failures.append(msg)


func _load_json(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("nao abriu " + path)
		return null
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data


func _load_dict(path: String) -> Dictionary:
	var data: Variant = _load_json(path)
	return data if typeof(data) == TYPE_DICTIONARY else {}


func _load_array(path: String) -> Array:
	var data: Variant = _load_json(path)
	return data if typeof(data) == TYPE_ARRAY else []


func _product_index(id: String) -> int:
	return int(_goods_order.get(id, -1))


## Mundo real (834 cidades) + estado urbano, pelo mesmo caminho do jogo.
func _make_world(limit: int = 0) -> EconomySim:
	var goods := _load_array(GOODS_PATH)
	for entry in goods:
		if typeof(entry) == TYPE_DICTIONARY:
			_goods_order[String(entry.get("id", ""))] = int(entry.get("order", -1))
	var sim := EconomySim.new()
	sim.set_goods(goods)
	sim.set_recipes(_load_array(RECIPES_PATH))
	sim.set_needs(_load_dict(NEEDS_PATH))
	sim.set_services(_load_dict(SERVICES_PATH))
	sim.set_currencies(_load_dict(CURRENCIES_PATH))
	sim.set_sector_employment(_load_dict(SECTOR_EMPLOYMENT_PATH))
	var cities := _load_array(CITIES_PATH)
	if limit > 0 and cities.size() > limit:
		cities = cities.slice(0, limit)
	sim.initialize_world(cities, _load_array(PORTS_PATH),
		_load_array(BUILDINGS_PATH))
	sim.set_urban(
		_load_dict(URBAN_LAND_PATH),
		_load_dict(HOUSING_CITY_PATH),
		_load_dict(HOUSING_PATH),
		_load_dict(MUNICIPAL_FINANCE_PATH))
	return sim


func _tick(sim: EconomySim, days: int, label: String) -> bool:
	for day in days:
		sim.tick_day()
		var violations: Array = sim.get_status().get("invariant_violations", [])
		if violations.size() > 0:
			_fail("%s: invariante violado no dia %d: %s" % [
				label, day + 1, String(violations[0])])
			return false
	return true


func _city_entry(snapshot: Dictionary, city_id: String) -> Dictionary:
	for city in snapshot.get("cities", []):
		if String(city.get("id", "")) == city_id:
			return city
	return {}


func _gov_entry(snapshot: Dictionary, city_id: String) -> Dictionary:
	for urban in snapshot.get("urban", []):
		if String(urban.get("city", "")) == city_id:
			return urban.get("gov", {})
	return {}


func _projects(sim: EconomySim, city_id: String = CITY) -> Array:
	var out: Array = []
	for project in sim.to_dict().get("construction_projects", []):
		if String(project.get("city", "")) == city_id:
			out.append(project)
	return out


func _project_of_kind(sim: EconomySim, kind: int, city_id: String = CITY) -> Dictionary:
	for project in _projects(sim, city_id):
		if int(project.get("kind", 0)) == kind:
			return project
	return {}


## Deixa a cidade do teste com caixa folgado, fila VAZIA e material semeado:
## o experimento isola a disputa por capacidade, não a falta de insumo.
func _prepare_city(sim: EconomySim) -> void:
	var snapshot: Dictionary = sim.to_dict()
	var kept: Array = []
	for project in snapshot.get("construction_projects", []):
		if String(project.get("city", "")) != CITY:
			kept.append(project)
	snapshot["construction_projects"] = kept
	for city in snapshot.get("cities", []):
		if String(city.get("id", "")) == CITY:
			city["firm_cash"] = 5.0e7
			# Capacidade alta no começo: a disputa é montada DEPOIS, com os
			# projetos já na fila (o tick seguinte usa este número).
			var workers: Array = city.get("sector_workers", [0.0, 0.0, 0.0, 0.0])
			while workers.size() < 4:
				workers.append(0.0)
			workers[1] = 1.0e6
			city["sector_workers"] = workers
	for urban in snapshot.get("urban", []):
		if String(urban.get("city", "")) == CITY:
			var gov: Dictionary = urban.get("gov", {})
			gov["cash"] = 5.0e9
			urban["gov"] = gov
	sim.from_dict(snapshot)
	for good in ["construction_material", "steel_structure",
			"electrical_material", "industrial_machinery", "wheat"]:
		sim.set_market_stock(CITY, good, 2.0e6)


## Ajusta a capacidade do setor de construção da cidade para `capacity` XGS/dia.
func _set_construction_capacity(sim: EconomySim, capacity: float) -> void:
	var snapshot: Dictionary = sim.to_dict()
	for city in snapshot.get("cities", []):
		if String(city.get("id", "")) == CITY:
			var workers: Array = city.get("sector_workers", [0.0, 0.0, 0.0, 0.0])
			while workers.size() < 4:
				workers.append(0.0)
			workers[1] = maxf(0.0, capacity) / VALUE_PER_WORKER_DAY
			city["sector_workers"] = workers
	sim.from_dict(snapshot)


## Custo diário contratado de um projeto: o fundo reservado (material + mão de
## obra) dividido pelo prazo — a régua que o tick cobra da capacidade.
func _day_cost(project: Dictionary) -> float:
	var duration := maxf(1.0, float(project.get("duration_days", 1)))
	return float(project.get("reserved", 0.0)) / duration


func _open_factory(sim: EconomySim) -> Dictionary:
	var flour := _product_index("flour")
	var firm_before := float(_city_entry(sim.to_dict(), CITY).get(
		"firm_cash", 0.0))
	var result: Dictionary = sim.build(CITY, "T-QUEUE-FAC", "flour", -1,
		flour, 40.0, 40.0, 1.8, 6.0, 1.0, "private", FACTORY_COST, "flour")
	result["firm_delta"] = firm_before - float(_city_entry(sim.to_dict(),
		CITY).get("firm_cash", 0.0))
	return result


func _open_housing(sim: EconomySim, units: float) -> Dictionary:
	var gov_before := float(_gov_entry(sim.to_dict(), CITY).get("cash", 0.0))
	var result: Dictionary = sim.urban_social_housing(CITY, units)
	result["gov_delta"] = gov_before - float(_gov_entry(sim.to_dict(),
		CITY).get("cash", 0.0))
	return result


## Um caso de disputa: `housing_first` decide quem começa antes. Com a
## capacidade curta, a SEGUNDA obra pausa ("sem capacidade de construç��o") e a
## primeira avança; a ordem de início é a prioridade determinística da fila.
func _case_competition(housing_first: bool) -> void:
	var label := "moradia-primeiro" if housing_first else "fabrica-primeiro"
	var sim := _make_world()
	if not _tick(sim, WARM_DAYS, label + ":aquecimento"):
		return
	var urban: Dictionary = sim.get_urban_city(CITY)
	if urban.is_empty():
		_fail("%s: %s sem estado urbano" % [label, CITY])
		return
	var land_price: Array = urban.get("land_price", [])
	if land_price.is_empty() or float(land_price[0]) <= 0.0:
		_fail("%s: terra sem preço medido na zona residencial" % label)
		return
	_prepare_city(sim)
	if _product_index("flour") < 0:
		_fail("%s: catálogo sem 'flour'" % label)
		return

	var housing: Dictionary = {}
	var factory: Dictionary = {}
	if housing_first:
		housing = _open_housing(sim, HOUSING_UNITS)
		factory = _open_factory(sim)
	else:
		factory = _open_factory(sim)
		housing = _open_housing(sim, HOUSING_UNITS)
	if not bool(factory.get("created", false)):
		_fail("%s: fábrica não entrou na fila (%s)" % [
			label, String(factory.get("error", ""))])
		return
	if not bool(housing.get("ok", false)):
		_fail("%s: moradia não entrou na fila" % label)
		return

	# --- (4) o DONO paga o custo inteiro na abertura. ----------------------
	if absf(float(factory.get("firm_delta", 0.0)) - FACTORY_COST) > FACTORY_COST * 1e-6:
		_fail("%s: fábrica debitou %.2f do dono, esperado %.2f" % [
			label, float(factory.get("firm_delta", 0.0)), FACTORY_COST])

	# --- (3) moradia na MESMA fila, com TERRA paga à prefeitura. -----------
	var housing_project := _project_of_kind(sim, 1)
	var factory_project := _project_of_kind(sim, 0)
	if housing_project.is_empty() or factory_project.is_empty():
		_fail("%s: fila sem as duas obras" % label)
		return
	var total_cost := float(housing_project.get("total_cost", 0.0))
	var land_cost := float(housing_project.get("land_cost", 0.0))
	var reserved := float(housing_project.get("reserved", 0.0))
	if total_cost <= 0.0:
		_fail("%s: moradia com total_cost zero" % label)
	if land_cost <= 0.0:
		_fail("%s: moradia não pagou terra (land_price > 0)" % label)
		return
	if absf(reserved - (total_cost - land_cost)) > total_cost * 1e-6:
		_fail("%s: reserved %.2f != total %.2f - terra %.2f" % [
			label, reserved, total_cost, land_cost])
	if int(housing_project.get("housing_owner", 0)) != 3:
		_fail("%s: dono da moradia %s != GOVERNMENT" % [
			label, str(housing_project.get("housing_owner"))])
	if absf(float(housing.get("gov_delta", 0.0)) - reserved) > total_cost * 1e-3:
		_fail("%s: caixa da prefeitura caiu %.2f, esperado %.2f (total %.2f, terra %.2f)" % [
			label, float(housing.get("gov_delta", 0.0)), reserved,
			total_cost, land_cost])

	# --- (1) capacidade curta: a SEGUNDA obra pausa, a primeira avança. -----
	var first_kind := 1 if housing_first else 0
	var second_kind := 0 if housing_first else 1
	var first_project := housing_project if housing_first else factory_project
	var second_project := factory_project if housing_first else housing_project
	var queue: Array = _projects(sim)
	if queue.size() != 2 or int(queue[0].get("kind", -1)) != first_kind \
			or int(queue[1].get("kind", -1)) != second_kind:
		_fail("%s: ordem da fila inesperada (kinds %s)" % [label, str(
			queue.map(func(item): return int(item.get("kind", -1))))])
		return
	var first_day := _day_cost(first_project)
	var second_day := _day_cost(second_project)
	if second_day <= first_day * 0.15:
		_fail("%s: teste inválido, a segunda obra não é grande o bastante (%.1f vs %.1f)" % [
			label, second_day, first_day])
		return
	var capacity := first_day * 1.05 + 1.0
	if capacity < 2100.0:
		_fail("%s: teste inválido, capacidade %.1f no piso de 2000 XGS/dia" % [
			label, capacity])
		return
	_set_construction_capacity(sim, capacity)
	var materials_before := _market_stock(sim, "construction_material")
	var day_before := int(sim.to_dict().get("day", 0))
	sim.tick_day()
	var first_after := _project_of_kind(sim, first_kind)
	var second_after := _project_of_kind(sim, second_kind)
	if int(first_after.get("days_elapsed", -1)) != 1 \
			or bool(first_after.get("paused", false)):
		_fail("%s: a primeira obra não avançou (dias=%s, pausa=%s)" % [
			label, str(first_after.get("days_elapsed")),
			String(first_after.get("pause_reason", ""))])
	if second_after.is_empty():
		_fail("%s: a segunda obra sumiu da fila" % label)
		return
	if int(second_after.get("days_elapsed", -1)) != 0:
		_fail("%s: a segunda obra avançou com a capacidade tomada (dias=%s)" % [
			label, str(second_after.get("days_elapsed"))])
	if not bool(second_after.get("paused", false)) or not String(
			second_after.get("pause_reason", "")).contains("capacidade"):
		_fail("%s: a segunda obra não pausou por capacidade (%s)" % [
			label, String(second_after.get("pause_reason", ""))])

	# --- (2) as duas compram do MESMO mercado: o estoque caiu. ------------
	var materials_after := _market_stock(sim, "construction_material")
	if materials_after >= materials_before - TOL:
		_fail("%s: mercado não consumiu construction_material (%.3f -> %.3f)" % [
			label, materials_before, materials_after])
	if int(sim.to_dict().get("day", 0)) <= day_before:
		_fail("%s: o dia não avançou" % label)

	# Capacidade de sobra: as duas retomam (a pausa é de capacidade, não falha).
	_set_construction_capacity(sim, 1.0e9)
	if not _tick(sim, 2, label + ":capacidade-folga"):
		return
	first_after = _project_of_kind(sim, first_kind)
	second_after = _project_of_kind(sim, second_kind)
	if int(first_after.get("days_elapsed", 0)) <= 1:
		_fail("%s: a primeira obra não retomou (dias=%s)" % [
			label, str(first_after.get("days_elapsed"))])
	if second_after.is_empty() or bool(second_after.get("paused", true)) \
			or int(second_after.get("days_elapsed", 0)) <= 0:
		_fail("%s: a segunda obra não retomou com capacidade de sobra (%s)" % [
			label, String(second_after.get("pause_reason", ""))])
	print("[queue] %s: primeira %s avança, segunda %s pausa (%.0f vs %.0f XGS/dia)." % [
		label, "moradia" if housing_first else "fábrica",
		"fábrica" if housing_first else "moradia", first_day, second_day])


func _test_v13_migration() -> void:
	# Save v13: a obra de moradia vivia em `housing[].projects`. O load v14 tem
	# que virá-la projeto kind HOUSING na fila única, com o prazo RESTANTE e
	# custo 0 (o capital já foi pago na versão antiga).
	var sim := _make_world()
	var snapshot: Dictionary = sim.to_dict()
	snapshot["version"] = 13
	snapshot["construction_projects"] = []
	var injected := false
	for entry in snapshot.get("housing", []):
		if String(entry.get("city", "")) == CITY:
			entry["projects"] = [{
				"state": 1, "density": 1, "tier": 2, "units": 25.0,
				"days_left": 7,
			}]
			injected = true
	if not injected:
		_fail("migracao: %s sem entrada no bloco housing" % CITY)
		return
	sim.from_dict(snapshot)
	var project := _project_of_kind(sim, 1)
	if project.is_empty():
		_fail("migracao v13: obra de moradia antiga não virou projeto da fila")
		return
	if int(project.get("housing_owner", 0)) != 1:
		_fail("migracao v13: dono %s != PRIVATE" % str(project.get("housing_owner")))
	if absf(float(project.get("housing_units", 0.0)) - 25.0) > TOL:
		_fail("migracao v13: unidades %.3f != 25" % float(
			project.get("housing_units", 0.0)))
	if int(project.get("duration_days", 0)) != 7:
		_fail("migracao v13: prazo %s != 7 dias restantes" % str(
			project.get("duration_days")))
	if float(project.get("total_cost", -1.0)) != 0.0 \
			or float(project.get("reserved", -1.0)) != 0.0:
		_fail("migracao v13: obra migrada cobrou de novo (total=%.2f)" % float(
			project.get("total_cost", -1.0)))
	# O schema novo sai em v16 (B1) e a obra antiga entrega as unidades.
	if int(sim.to_dict().get("version", 0)) != 17:
		_fail("migracao v13: save regravado não saiu como v16")
	var share_before := _owner_share_of(sim, 1)
	if not _tick(sim, 7, "migracao-v13"):
		return
	if _project_of_kind(sim, 1).size() > 0:
		_fail("migracao v13: obra migrada não terminou no prazo restante")
	# A entrega é medida no contador do mês: a depreciação do estoque corre em
	# paralelo (e some mais que 25 unidades em 7 dias numa metrópole).
	var built := float(sim.get_urban_city(CITY).get("units_built_month", 0.0))
	if built < 24.0:
		_fail("migracao v13: unidades entregues do mês %.2f != ~25" % built)
	if _owner_share_of(sim, 1) <= share_before:
		_fail("migracao v13: fração do dono (firma) não subiu com a entrega")
	print("[queue] migracao v13->v14: obra PRIVATE de 25 unidades, 7 dias -> "
		+ "projeto HOUSING na fila (custo 0) e entrega.")


func _owner_share_of(sim: EconomySim, owner: int) -> float:
	var urban: Dictionary = sim.get_urban_city(CITY)
	var share: Array = urban.get("owner_share", [])
	return float(share[owner]) if share.size() > owner else 0.0

func _units_of(sim: EconomySim, density: int, tier: int) -> float:
	var urban: Dictionary = sim.get_urban_city(CITY)
	var units: Array = urban.get("units", [])
	if units.size() <= density:
		return 0.0
	var row: Array = units[density]
	return float(row[tier]) if row.size() > tier else 0.0

func _test_queue_competition() -> void:
	# A ordem de INÍCIO é a prioridade: cada caso prova os dois lados.
	_case_competition(false)
	_case_competition(true)

func _market_stock(sim: EconomySim, good: String) -> float:
	# O detalhe do mercado por cidade vem do status sob demanda (S7-c): o campo
	# `product` da entrada é o índice do bem no catálogo.
	var product := _product_index(good)
	var status: Dictionary = sim.get_city_status(CITY)
	for market in status.get("markets", []):
		if int(market.get("product", -1)) == product:
			return float(market.get("stock", 0.0))
	return -1.0


func _test_roundtrip() -> void:
	var sim := _make_world()
	if not _tick(sim, 3, "roundtrip"):
		return
	# Uma obra de moradia em voo: o round-trip precisa carregar kind/campos.
	sim.urban_social_housing(CITY, 50.0)
	var before: Dictionary = sim.to_dict()
	sim.from_dict(before)
	var after: Dictionary = sim.to_dict()
	if JSON.stringify(before.get("construction_projects", [])) != JSON.stringify(
			after.get("construction_projects", [])):
		_fail("round-trip: fila de obras mudou na ida-e-volta")
	if JSON.stringify(before.get("housing", [])) != JSON.stringify(
			after.get("housing", [])):
		_fail("round-trip: bloco `housing` mudou na ida-e-volta")
	if not _tick(sim, 3, "roundtrip-pos-load"):
		return


func _test_thread_determinism() -> void:
	var one := _make_world()
	var eight := _make_world()
	for sim in [one, eight]:
		_prepare_city(sim)
		var flour := _product_index("flour")
		sim.build(CITY, "T-QUEUE-FAC", "flour", -1, flour, 40.0, 40.0,
			1.8, 6.0, 1.0, "private", FACTORY_COST, "flour")
		sim.urban_social_housing(CITY, HOUSING_UNITS)
	one.set_sim_threads(1)
	eight.set_sim_threads(8)
	if not _tick(one, 3, "threads=1"):
		return
	if not _tick(eight, 3, "threads=8"):
		return
	if JSON.stringify(one.to_dict()) != JSON.stringify(eight.to_dict()):
		_fail("determinismo: estado final difere entre threads=1 e 8")
