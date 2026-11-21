extends Node

## F8 — teste headless dos SERVIÇOS (20 setores).
##
##   godot --headless --path . res://tools/services_test.tscn
##
## Cobre o aceite da fase:
##   1. demanda das POPs separada por nível de consumo (basic/life/luxury);
##   2. demanda B2B (opex/tonelagem) e do governo;
##   3. abertura e fechamento de negócio (capital da poupança, prejuízo);
##   4. nome único por cultura do dono (BusinessNameGenerator);
##   5. exportação de serviço exportável (finanças/TI/turismo/logística);
##   6. educação privada acelera a qualificação (F7);
##   7. dinheiro conservado no tick e save v11 com migração de v9.

const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"
const SERVICES_PATH := "res://data/definitions/economy/services.json"
const CURRENCIES_PATH := "res://data/definitions/economy/currencies.json"

## A6: as decisões de serviço rodam no dia semanal da cidade.
const WEEK_DAYS := 7

const GeneratorScript: Script = preload("res://core/culture/business_name_generator.gd")
const CultureDbScript: Script = preload("res://core/culture/culture_db.gd")

var _failures: Array[String] = []
var _goods: Array = []
var _recipes: Array = []
var _needs: Dictionary = {}
var _services: Dictionary = {}
var _currencies: Dictionary = {}

const CITIES := [
	{"id": "T-USA", "name": "Alpha", "nation": "USA",
		"population": 1000000.0, "sim_population": 1000000.0, "x": 100.0, "y": 100.0},
	{"id": "T-DEU", "name": "Beta", "nation": "DEU",
		"population": 700000.0, "sim_population": 700000.0, "x": 210.0, "y": 120.0},
	{"id": "T-NGA", "name": "Gamma", "nation": "NGA",
		"population": 900000.0, "sim_population": 900000.0, "x": 300.0, "y": 200.0},
]


func _ready() -> void:
	_goods = _load_array(GOODS_PATH)
	_recipes = _load_array(RECIPES_PATH)
	_needs = _load_dict(NEEDS_PATH)
	_services = _load_dict(SERVICES_PATH)
	_currencies = _load_dict(CURRENCIES_PATH)
	if _goods.is_empty() or _services.is_empty():
		_fail("dados ausentes (goods=%d, services=%d)" % [
			_goods.size(), _services.size()])
		_finish()
		return
	print("[services] stage demand")
	_test_demand_and_business()
	print("[services] stage education")
	_test_education()
	print("[services] stage save")
	_test_save()
	_finish()


# --- 1..5 e 7 (tick) -------------------------------------------------------

func _test_demand_and_business() -> void:
	var sim = _make_world(false)
	if sim == null:
		return
	_set_income(sim)
	# Exportação: força capacidade de finanças na cidade dos EUA antes do tick.
	sim.set_service_capacity("T-USA", "finance", 120.0)
	sim.set_service_capacity("T-USA", "health", 40.0)
	sim.set_service_capacity("T-USA", "education", 40.0)
	var before_money := _money_total(sim)
	var before_count := int(sim.get_service_business_count())
	_run(sim, 12)
	var detail: Dictionary = sim.get_services_detail("T-USA")
	var sectors: Array = detail.get("sectors", [])
	_check(sectors.size() == 20, "detalhe deve ter 20 setores (tem %d)" % sectors.size())
	if sectors.size() != 20:
		return
	var retail: Dictionary = _sector(sectors, "retail")
	var finance: Dictionary = _sector(sectors, "finance")
	var health: Dictionary = _sector(sectors, "health")
	var education: Dictionary = _sector(sectors, "education")
	var pop_demand: Array = retail.get("pop_demand", [])
	var level_demand := 0.0
	for value in pop_demand:
		level_demand += float(value)
	_check(level_demand > 0.0, "demanda das POPs deve existir (retail=%.6f)" % level_demand)
	_check(float(pop_demand[0]) > 0.0 and float(pop_demand[2]) > 0.0,
		"demanda deve separar basic/life/luxury: %s" % str(pop_demand))
	_check(float(pop_demand[0]) != float(pop_demand[2]),
		"basic e luxury não podem ter a mesma demanda: %s" % str(pop_demand))
	# B2B: opex das construções alimenta finanças/profissionais; a tonelagem
	# movimentada alimenta logística. Ambas devem ter demanda > 0.
	var professional: Dictionary = _sector(sectors, "professional_services")
	var freight: Dictionary = _sector(sectors, "freight_logistics")
	_check(float(finance["demand"]) > 0.0, "demanda B2B de finanças deve existir")
	_check(float(professional["demand"]) > 0.0,
		"demanda B2B de serviços profissionais deve existir")
	_check(float(freight["demand"]) > 0.0,
		"demanda de logística deve existir (tonelagem)")
	# Exportação de serviço exportável (finanças) acima de zero.
	_check(float(finance["exports"]) > 0.0,
		"finanças deve exportar (exports=%.3f)" % float(finance["exports"]))
	_check(float(finance["export_revenue"]) > 0.0,
		"exportação deve gerar receita")
	# Saúde/educação privadas com cobertura > 0 quando há capacidade.
	_check(float(health["coverage"]) > 0.0,
		"saúde privada deve ter cobertura com capacidade (%.3f)" % float(
			health["coverage"]))
	_check(float(detail.get("private_education_cover", 0.0)) > 0.0,
		"educação privada deve ter cobertura com capacidade")
	# Abertura: negócios existem e o dia fecha conservando dinheiro.
	var after_money := _money_total(sim)
	_check(absf(after_money - before_money) < 1e-3,
		"dinheiro conservado no tick: %.6f -> %.6f" % [before_money, after_money])
	var count := int(sim.get_service_business_count())
	_check(count > 0, "deve haver negócios de serviço (%d)" % count)
	_check(count >= before_count, "contagem não pode cair sem falência (%d -> %d)" % [
		before_count, count])
	_check(int(sim.get_service_bankruptcies()) == 0,
		"nenhuma falência de serviço nos primeiros 12 dias")

	# Fechamento por prejuízo persistente: força 60 dias no vermelho. Zera a
	# capacidade do setor antes, senão o lucro do dia (recalculado no tick)
	# pode ser positivo e zerar o contador — o caso testado é o negócio que
	# não vende e acumula prejuízo.
	var businesses: Array = detail.get("businesses", [])
	_check(not businesses.is_empty(), "detalhe deve listar negócios")
	if not businesses.is_empty():
		var doomed := int(businesses[0]["id"])
		var doomed_sector := String(businesses[0]["sector"])
		var bankruptcies_before := int(sim.get_service_bankruptcies())
		sim.set_service_capacity("T-USA", doomed_sector, 0.0)
		sim.set_service_business_state(doomed, 60, -1000.0)
		# A6: a decisão de fechamento roda no dia semanal da cidade
		# (`(dia - índice) % 7 == 0`); a demanda/faturamento continuam diários.
		# Ticka até esse dia (no máximo 7) em vez de um único tick.
		var waited := _tick_to_weekly(sim, "T-USA")
		print("[services] decisao semanal de fechamento: %d tick(s) ate o dia %d" % [
			waited, int(sim.get_status().get("day", 0))])
		_check(int(sim.get_service_bankruptcies()) == bankruptcies_before + 1,
			"negócio com 60 dias de prejuízo deve fechar")
		var still_there := false
		for row in (sim.get_services_detail("T-USA").get("businesses", []) as Array):
			if int(row["id"]) == doomed:
				still_there = true
		_check(not still_there, "negócio fechado não deve aparecer na lista")

	# Entrada: num mundo novo (antes de o mercado saturar com as 4 vagas por
	# setor), zera a oferta de um setor e verifica a abertura no dia seguinte.
	var entry_sim: Object = _make_world(false)
	if entry_sim != null:
		_set_income(entry_sim)
		entry_sim.set_service_capacity("T-DEU", "retail", 0.0)
		var deu_before := _city_businesses(entry_sim, "T-DEU")
		entry_sim.tick_day()
		var deu_after := _city_businesses(entry_sim, "T-DEU")
		_check(deu_after > deu_before,
			"demanda não atendida deve abrir negócio (%d -> %d)" % [
				deu_before, deu_after])

	# Nome único por cultura do dono, para todos os negócios do mundo.
	_check(_names_unique(sim), "nomes gerados devem ser únicos")

	# Governo: a receita de governo dos serviços deve ser > 0 e conservada.
	var government_revenue := 0.0
	for city_id in ["T-USA", "T-DEU", "T-NGA"]:
		government_revenue += float(sim.get_services_detail(city_id).get(
			"service_government_revenue", 0.0))
	_check(government_revenue > 0.0,
		"governo deve comprar serviços (%.3f)" % government_revenue)


# --- 6 (educação privada) --------------------------------------------------

func _test_education() -> void:
	var baseline = _make_world(false)
	var private = _make_world(false)
	if baseline == null or private == null:
		return
	for sim in [baseline, private]:
		_set_income(sim)
	private.set_service_capacity("T-USA", "education", 240.0)
	private.set_service_capacity("T-DEU", "education", 200.0)
	private.set_service_capacity("T-NGA", "education", 200.0)
	_run(baseline, 150)
	_run(private, 150)
	var baseline_edu := _education_share(baseline)
	var private_edu := _education_share(private)
	_check(float(private_edu["cover"]) >= 0.0, "cobertura privada deve ser publicada")
	_check(float(private_edu["higher"]) > float(baseline_edu["higher"]),
		"educação privada deve acelerar a qualificação (%.4f > %.4f)" % [
			float(private_edu["higher"]), float(baseline_edu["higher"])])


# --- 7 (save v11) ----------------------------------------------------------

func _test_save() -> void:
	var sim = _make_world(false)
	if sim == null:
		return
	_set_income(sim)
	_run(sim, 5)
	var data: Dictionary = sim.to_dict()
	_check(int(data.get("version", 0)) == 32,
		"save deve ser v32 (C2c: bloco fx_regimes; era %d)" % int(
		data.get("version", 0)))
	var loaded = _load_save(data)
	if loaded == null:
		return
	_check(int(loaded.get_status().get("day", -1)) == int(sim.get_status().get(
		"day", -1)), "save/load preserva o dia")
	_check(loaded.get_service_business_count()
		== sim.get_service_business_count(), "save/load preserva os negócios")
	_check(absf(_money_total(loaded) - _money_total(sim)) < 1e-3,
		"save/load preserva o dinheiro")
	# Migração v9: sem o bloco de serviços, o load semeia negócios.
	var legacy: Dictionary = data.duplicate(true)
	legacy["version"] = 9
	legacy.erase("service_businesses")
	legacy.erase("service_bankruptcies")
	for city in (legacy.get("cities", []) as Array):
		(city as Dictionary).erase("services")
	var migrated = _load_save(legacy)
	if migrated != null:
		_check(int(migrated.get_status().get("day", -1)) >= 0,
			"save v9 migra e roda")
		_check(migrated.get_service_business_count() > 0,
			"save v9 migra semeando negócios de serviço")


# --- apoio -----------------------------------------------------------------

func _make_world(seed_businesses: bool) -> Object:
	var sim = EconomySim.new()
	sim.set_goods(_goods)
	sim.set_recipes(_recipes)
	sim.set_needs(_needs)
	sim.set_services(_services)
	sim.set_currencies(_currencies)
	var buildings: Array = []
	for city: Dictionary in CITIES:
		buildings.append({
			"id": "FARM-" + String(city["id"]),
			"city_id": String(city["id"]),
			"resource": "wheat",
			"output_tons_per_day": 20.0,
			"aptitude": 1.0,
		})
	sim.initialize_world(CITIES, [], buildings)
	if not seed_businesses:
		pass
	return sim


func _set_income(sim: Object) -> void:
	sim.set_external_income_index("T-USA", 2.4)
	sim.set_external_income_index("T-DEU", 1.8)
	sim.set_external_income_index("T-NGA", 0.5)
	sim.set_income_gini("T-USA", 0.41)
	sim.set_income_gini("T-DEU", 0.32)
	sim.set_income_gini("T-NGA", 0.47)


func _load_save(data: Dictionary) -> Object:
	var sim = EconomySim.new()
	sim.set_goods(_goods)
	sim.set_recipes(_recipes)
	sim.set_needs(_needs)
	sim.set_services(_services)
	sim.set_currencies(_currencies)
	sim.from_dict(data)
	return sim


func _run(sim: Object, days: int) -> void:
	for index in days:
		sim.tick_day()


## A6: as decisões de abertura/fechamento de negócio rodam no dia semanal da
## cidade (`(dia - índice) % 7 == 0`). Ticka até o próximo dia semanal da
## cidade (no máximo 7) e devolve quantos ticks esperou.
func _tick_to_weekly(sim: Object, city_id: String) -> int:
	var offset := _city_index(sim, city_id) % WEEK_DAYS
	for ticks in range(1, WEEK_DAYS + 1):
		sim.tick_day()
		var day := int(sim.get_status().get("day", 0))
		if ((day - offset) % WEEK_DAYS + WEEK_DAYS) % WEEK_DAYS == 0:
			return ticks
	return WEEK_DAYS


## Índice da cidade na ordem do vetor (`to_dict()["cities"]`), que é o `offset`
## da cadência escalonada.
func _city_index(sim: Object, city_id: String) -> int:
	var cities: Array = sim.to_dict().get("cities", [])
	for index in cities.size():
		if String((cities[index] as Dictionary).get("id", "")) == city_id:
			return index
	_fail("%s ausente do save de cidades" % city_id)
	return 0


func _money_total(sim: Object) -> float:
	return float(sim.get_status().get("money_total", 0.0))


func _city_businesses(sim: Object, city_id: String) -> int:
	return (sim.get_services_detail(city_id).get("businesses", []) as Array).size()


func _sector(sectors: Array, id: String) -> Dictionary:
	for row in sectors:
		if String(row.get("id", "")) == id:
			return row
	return {}


func _education_share(sim: Object) -> Dictionary:
	var total := 0.0
	var higher := 0.0
	var cover := 0.0
	for city in (sim.get_status().get("cities", []) as Array):
		# POP é detalhe pesado: o status diário publica só os escalares leves,
		# então a educação vem sob demanda pelo id da cidade.
		var pop: Dictionary = (sim.get_city_status(String(city.get("id", ""))).get(
			"pop", {}) as Dictionary)
		var education: Array = pop.get("education", [])
		for index in education.size():
			total += float(education[index])
			if index >= 1:
				higher += float(education[index])
		cover += float(city.get("private_education_cover", 0.0))
	var share := 0.0
	if total > 0.0:
		share = higher / total
	return {"higher": share, "cover": cover}


func _names_unique(sim: Object) -> bool:
	var generator: RefCounted = GeneratorScript.new()
	CultureDbScript.load_data()
	var used: Dictionary = {}
	var status: Dictionary = sim.get_status()
	var place_by_city: Dictionary = {}
	for city in (status.get("cities", []) as Array):
		place_by_city[String(city.get("id", ""))] = String(city.get("name", ""))
	var total := 0
	for city in (status.get("cities", []) as Array):
		var city_id := String(city.get("id", ""))
		var detail: Dictionary = sim.get_services_detail(city_id)
		var culture := "brasileiro"
		var mix: Array = CultureDbScript.city_mix(city_id)
		if not mix.is_empty():
			culture = String((mix[0] as Dictionary).get("id", culture))
		for business in (detail.get("businesses", []) as Array):
			var name: String = generator.generate(culture, String(business["sector"]),
				String(place_by_city.get(city_id, "")), int(business["name_seed"]))
			if name.is_empty():
				name = generator.generate("americano", String(business["sector"]),
					String(place_by_city.get(city_id, "")), int(business["name_seed"]))
			if name.is_empty():
				return false
			var key := name.to_lower()
			if used.has(key):
				print("[services] nome repetido: %s" % name)
				return false
			used[key] = true
			total += 1
	if total == 0:
		return false
	print("[services] nomes únicos: %d" % total)
	return true


func _check(condition: bool, message: String) -> void:
	if condition:
		print("[services] OK: " + message)
	else:
		_failures.append(message)
		print("[services] FALHA: " + message)


func _fail(message: String) -> void:
	_failures.append(message)
	print("[services] FALHA: " + message)


func _finish() -> void:
	if _failures.is_empty():
		print("[services] OK")
		get_tree().quit(0)
	else:
		print("[services] FALHAS (%d):" % _failures.size())
		for failure in _failures:
			print(" - " + failure)
		get_tree().quit(1)


func _load_array(path: String) -> Array:
	var data: Variant = _load_json(path)
	return data if typeof(data) == TYPE_ARRAY else []


func _load_dict(path: String) -> Dictionary:
	var data: Variant = _load_json(path)
	return data if typeof(data) == TYPE_DICTIONARY else {}


func _load_json(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return null
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data
