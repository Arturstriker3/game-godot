extends SceneTree

## B5 — INFRAESTRUTURA DO ESTADO + CONCESSÕES (F9) + save v21.
##
## Valida (1) os efeitos de cada infra (custo de acesso, produtividade, êxodo),
## (2) a obra/manutenção de infra (custo, caixa nacional, conservação), (3) a
## conservação de dinheiro nas concessões (conceder/estatizar + royalty) e o
## round-trip de save, e (4) a migração v20 → v21 (níveis nascem 0, royalty 0).
##
## Uso: godot --headless --path . --script res://tools/b5_state_levers_test.gd

const RegionDataScript := preload("res://core/economy/region_data.gd")

const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const PORTS_PATH := "res://data/definitions/scenario/ports.json"
const BUILDINGS_PATH := "res://data/definitions/scenario/buildings.json"
const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"
const SERVICES_PATH := "res://data/definitions/economy/services.json"
const CURRENCIES_PATH := "res://data/definitions/economy/currencies.json"
const SECTOR_EMPLOYMENT_PATH := "res://data/definitions/economy/sector_employment.json"
const AGE_STRUCTURE_PATH := "res://data/definitions/population/age_structure_2026.json"
const URBAN_LAND_PATH := "res://data/definitions/economy/urban_land.json"
const HOUSING_CITY_PATH := "res://data/definitions/economy/housing_city.json"
const HOUSING_PATH := "res://data/definitions/economy/housing.json"
const MUNICIPAL_FINANCE_PATH := "res://data/definitions/economy/municipal_finance.json"

const SAVE_VERSION := 28
# Parâmetros espelhados de sim_constants.h (âncoras do teste).
const ACCESS_PENALTY_MAX := 0.30
const EXODUS_BOOST_MAX := 0.50
# D2: royalty declarado no teste (o padrão do núcleo também é 0,05).
const ROYALTY_RATE := 0.05
# D2: planta secundária que consome o minério do estoque — a receita atribuída
# ao insumo sobe sem entrar caixa (transferência interna entre construções).
const CONCESSION_PLANT_CAPACITY := 1.0e8
const CONCESSION_IRON_STOCK := 2.0e6
const CONCESSION_IRON_PRICE := 170.0

var _failures: Array = []


func _init() -> void:
	_run()
	if _failures.is_empty():
		print("[b5] OK: infra (efeitos/obra/manutenção), concessões e save v21.")
		quit(0)
	else:
		print("[b5] FALHAS (%d):" % _failures.size())
		for failure: String in _failures:
			print(" - " + failure)
		quit(1)


func _fail(message: String) -> void:
	_failures.append(message)


func _check(cond: bool, message: String) -> void:
	if not cond:
		_fail(message)


func _json(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("não abriu " + path)
		return null
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data


func _load_chains(sim: Object) -> void:
	sim.set_goods(_json(GOODS_PATH))
	sim.set_recipes(_json(RECIPES_PATH))
	sim.set_needs(_json(NEEDS_PATH))
	if sim.has_method("set_services"):
		sim.set_services(_json(SERVICES_PATH))
	if sim.has_method("set_currencies"):
		sim.set_currencies(_json(CURRENCIES_PATH))
	if sim.has_method("set_sector_employment"):
		sim.set_sector_employment(_json(SECTOR_EMPLOYMENT_PATH))
	if sim.has_method("set_age_structure"):
		sim.set_age_structure(_json(AGE_STRUCTURE_PATH))


func _make_world() -> Object:
	var sim = EconomySim.new()
	_load_chains(sim)
	var cities: Array = _json(CITIES_PATH)
	var ports: Array = _json(PORTS_PATH)
	var buildings: Array = _json(BUILDINGS_PATH)
	RegionDataScript.attach(cities)
	sim.initialize_world(cities, ports, buildings)
	if sim.has_method("set_urban"):
		sim.set_urban(_json(URBAN_LAND_PATH), _json(HOUSING_CITY_PATH),
			_json(HOUSING_PATH), _json(MUNICIPAL_FINANCE_PATH))
	return sim


## Primeira cidade com região pronta e população do interior > 0.
func _pick_city(save: Dictionary) -> String:
	for city: Dictionary in save.get("cities", []):
		var region: Dictionary = city.get("region", {})
		if bool(region.get("ready", false)) \
				and float(region.get("region_pop", 0.0)) > 0.0:
			return String(city.get("id", ""))
	return ""


## Entrada de concessão do ativo no status da região ({} se não aparecer).
func _concession(sim: Object, city_id: String, building_id: String) -> Dictionary:
	for c: Dictionary in sim.get_region_status(city_id).get("concessions", []):
		if String(c.get("id", "")) == building_id:
			return c
	return {}


func _run() -> void:
	var sim := _make_world()
	var save: Dictionary = sim.to_dict()
	var city_id := _pick_city(save)
	if city_id == "":
		_fail("nenhuma cidade com região pronta")
		return

	# --- (1) Efeitos de cada infra via multiplicadores do status. ---
	var st: Dictionary = sim.get_region_status(city_id)
	var infra: Dictionary = st.get("infra", {})
	_check(infra.has("road") and infra.has("power") and infra.has("water"),
		"status.infra deve expor road/power/water")
	_check(int(infra.get("road", -1)) == 0 and int(infra.get("power", -1)) == 0
		and int(infra.get("water", -1)) == 0, "infra inicial deve ser 0")
	_check(absf(float(infra.get("access_multiplier", 1.0)) - (1.0 + ACCESS_PENALTY_MAX)) < 1e-9,
		"acesso com estrada 0 deve ser 1 + penalidade máxima")
	_check(absf(float(infra.get("productivity_multiplier", 0.0)) - 1.0) < 1e-9,
		"produtividade com infra 0 deve ser 1.0")
	_check(absf(float(infra.get("exodus_multiplier", 0.0)) - (1.0 + EXODUS_BOOST_MAX)) < 1e-9,
		"êxodo com infra 0 deve ser 1 + boost máximo")

	# --- (2) Obra de infra (não instantânea): custo, prazo, conservação. ---
	var money_before := float(sim.get_status().get("money_total", 0.0))
	var res: Dictionary = sim.infra_build(city_id, 0)  # estrada
	_check(bool(res.get("started", false)), "obra de estrada deve iniciar: %s" % str(res))
	var cost := float(res.get("cost", 0.0))
	_check(cost > 0.0, "custo da obra deve ser > 0")
	_check(int(res.get("duration_days", 0)) == 90, "prazo da obra de infra = 90 dias")
	_check(res.has("project_id"), "obra de infra (não instantânea) deve ter project_id")
	var money_after := float(sim.get_status().get("money_total", 0.0))
	_check(absf(money_after - money_before) < 1e-6,
		"conservação de dinheiro na obra de infra (Δ=%.6f)" % (money_after - money_before))
	# Obra repetida do MESMO tipo em voo é recusada (anti-duplicação).
	var res2: Dictionary = sim.infra_build(city_id, 0)
	_check(not bool(res2.get("started", false)),
		"obra duplicada do mesmo tipo deve ser recusada")

	# --- (2b) Obra instantânea sobe o nível e conserva dinheiro. ---
	var sim2 := _make_world()
	sim2.set_instant_construction(true)
	var m2a := float(sim2.get_status().get("money_total", 0.0))
	var ir: Dictionary = sim2.infra_build(city_id, 0)
	_check(bool(ir.get("started", false)), "obra instantânea deve iniciar")
	var m2b := float(sim2.get_status().get("money_total", 0.0))
	_check(absf(m2b - m2a) < 1e-6,
		"obra instantânea conserva dinheiro (Δ=%.6f)" % (m2b - m2a))
	var st2: Dictionary = sim2.get_region_status(city_id)
	_check(int(st2.get("infra", {}).get("road", -1)) == 1,
		"estrada instantânea deve subir para 1")

	# --- (3) Concessões: cria mina/fazenda estatal e concede (conservação). ---
	var sim3 := _make_world()
	sim3.set_instant_construction(true)
	var bres: Dictionary = sim3.build(city_id, "B5_TEST_FARM", "wheat", -1, 0,
		1.0, 5.0, 0.5, 1.0, 0.0, "state", 500.0, "")
	_check(bool(bres.get("created", false)), "criar fazenda estatal: %s" % str(bres))
	var groups: Array = sim3.get_investment_status().get("groups", [])
	var buyer_id := ""
	for g: Dictionary in groups:
		if not bool(g.get("state_owned", false)):
			buyer_id = String(g.get("id", ""))
			break
	if buyer_id == "":
		_fail("nenhum grupo privado para concessão")
	else:
		sim3.fund_group(buyer_id, 20000.0)
		var m0 := float(sim3.get_status().get("money_total", 0.0))
		var cr: Dictionary = sim3.concede(city_id, "B5_TEST_FARM", buyer_id, 0.0)
		_check(bool(cr.get("ok", false)), "conceder deve funcionar: %s" % str(cr))
		var m1 := float(sim3.get_status().get("money_total", 0.0))
		_check(absf(m1 - m0) < 1e-6,
			"concessão conserva dinheiro (Δ=%.6f)" % (m1 - m0))
		var cstatus: Dictionary = sim3.get_region_status(city_id)
		var found := false
		for c: Dictionary in cstatus.get("concessions", []):
			if String(c.get("id", "")) == "B5_TEST_FARM":
				found = true
				_check(String(c.get("owner", "")) != "state",
					"dono da concessão deve deixar de ser state")
				_check(float(c.get("royalty", -1.0)) > 0.0,
					"royalty da concessão deve ser > 0")
		_check(found, "concessão deve aparecer no status")
		# Estatizar de volta (conservação).
		var m2 := float(sim3.get_status().get("money_total", 0.0))
		var nr: Dictionary = sim3.nationalize(city_id, "B5_TEST_FARM")
		_check(bool(nr.get("ok", false)), "estatizar deve funcionar")
		var m3 := float(sim3.get_status().get("money_total", 0.0))
		_check(absf(m3 - m2) < 1e-6,
			"estatizar conserva dinheiro (Δ=%.6f)" % (m3 - m2))
		# D1: estatizar extingue a concessão (sem royalty residual).
		var nstat := _concession(sim3, city_id, "B5_TEST_FARM")
		_check(String(nstat.get("owner", "")) == "state",
			"D1: estatizado deve voltar a owner=state (%s)" % str(nstat.get("owner")))
		_check(absf(float(nstat.get("royalty", -1.0))) < 1e-9,
			"D1: estatizar deve zerar o royalty (%s)" % str(nstat.get("royalty")))
		_check(absf(float(nstat.get("royalty_paid", -1.0))) < 1e-9,
			"D1: estatizar deve zerar o royalty_paid")
		# D1: privatizar também limpa o royalty (não é concessão).
		var pr: Dictionary = sim3.privatize(city_id, "B5_TEST_FARM", buyer_id)
		_check(bool(pr.get("ok", false)), "D1: privatizar deve funcionar: %s" % str(pr))
		var pstat := _concession(sim3, city_id, "B5_TEST_FARM")
		_check(String(pstat.get("owner", "")) != "state",
			"D1: privatizado não pode seguir estatal (%s)" % str(pstat.get("owner")))
		_check(absf(float(pstat.get("royalty", -1.0))) < 1e-9,
			"D1: privatizar deve zerar o royalty residual (%s)" % str(pstat.get("royalty")))
		_check(absf(float(pstat.get("royalty_paid", -1.0))) < 1e-9,
			"D1: privatizar deve zerar o royalty_paid")

	# --- (3b) D2: royalty efetivo limitado pelo caixa da firma. ---
	# Regressão: com `firm_cash = max(0, firm_cash - royalty)`, o governo
	# recebia o royalty cheio e a firma pagava menos quando o caixa era menor
	# que o devido — dinheiro criado e invariante violada. Cenário controlado
	# via save: receita atribuída alta (devido = 5e7) e caixa da firma = 0.
	var sim4 := _make_world()
	sim4.set_instant_construction(true)
	var products4: Array = sim4.to_dict().get("products", [])
	var iron_index := -1
	var steel_index := -1
	for p4 in products4.size():
		if String(products4[p4]) == "iron":
			iron_index = p4
		elif String(products4[p4]) == "steel":
			steel_index = p4
	_check(iron_index >= 0 and steel_index >= 0, "D2: produtos iron/steel no mundo")
	if iron_index < 0 or steel_index < 0:
		return
	sim4.set_suitability(city_id, "iron", 0.5)
	var mine_res: Dictionary = sim4.build(city_id, "B5_TEST_MINE", "iron", -1,
		iron_index, 1.0, 5.0, 0.5, 1.0, 0.0, "state", 500.0, "")
	_check(bool(mine_res.get("created", false)), "D2: criar mina estatal: %s" % str(mine_res))
	# Planta de aço: consome ferro do estoque (crédito de receita ao insumo, sem
	# caixa) e produz OUTRO bem — assim não dilui a fatia de receita da mina.
	var plant_res: Dictionary = sim4.build(city_id, "B5_TEST_PLANT", "iron",
		iron_index, steel_index, CONCESSION_PLANT_CAPACITY, 0.0, 0.0, 0.0, 1.0,
		"private", 500.0, "")
	_check(bool(plant_res.get("created", false)), "D2: criar planta: %s" % str(plant_res))
	sim4.set_market_stock(city_id, "iron", CONCESSION_IRON_STOCK)
	sim4.set_market_price(city_id, "iron", CONCESSION_IRON_PRICE)
	var buyer2 := ""
	for g2: Dictionary in sim4.get_investment_status().get("groups", []):
		if not bool(g2.get("state_owned", false)):
			buyer2 = String(g2.get("id", ""))
			break
	if buyer2 == "":
		_fail("D2: nenhum grupo privado para concessão")
	else:
		sim4.fund_group(buyer2, 20000.0)
		var cr2: Dictionary = sim4.concede(city_id, "B5_TEST_MINE", buyer2, ROYALTY_RATE)
		_check(bool(cr2.get("ok", false)), "D2: conceder deve funcionar: %s" % str(cr2))
		var save2: Dictionary = sim4.to_dict()
		# Caixa da firma = 0 no início do dia: o royalty devido (≈1,7e7) não
		# cabe no caixa e tem de ser pago só até o que existe.
		for city2: Dictionary in save2.get("cities", []):
			if String(city2.get("id", "")) == city_id:
				city2["firm_cash"] = 0.0
		var sim5 := EconomySim.new()
		_load_chains(sim5)
		sim5.from_dict(save2)
		sim5.tick_day()
		# get_status() é cacheado; o resumo do dia é montado a cada chamada e
		# traz a violação de conservação do próprio tick (o bug criava dinheiro
		# e phase_invariants revertia o dia).
		var st5: Dictionary = sim5.get_day_summary()
		var violations: Array = st5.get("invariant_violations", [])
		_check(violations.is_empty(),
			"D2: invariantes limpos (violações: %s)" % str(violations))
		var paid := -1.0
		for c3: Dictionary in sim5.get_region_status(city_id).get("concessions", []):
			if String(c3.get("id", "")) == "B5_TEST_MINE":
				paid = float(c3.get("royalty_paid", -1.0))
		var used := minf(CONCESSION_IRON_STOCK,
			CONCESSION_PLANT_CAPACITY * 0.25)
		var due := used * CONCESSION_IRON_PRICE * ROYALTY_RATE
		_check(paid >= 0.0 and paid < due * 0.5,
			"D2: royalty efetivo deve respeitar o caixa (pago=%.2f, devido=%.0f)"
				% [paid, due])
		print("[b5] D2: royalty devido≈%.0f pago=%.2f caixa_inicial=0 violações=%d"
			% [due, paid, violations.size()])

	# --- Round-trip: royalty persiste no save. ---
	var rt: Dictionary = sim3.to_dict()
	var rt2 := EconomySim.new()
	_load_chains(rt2)
	rt2.from_dict(rt)
	var rt3: Dictionary = rt2.to_dict()
	_check(int(rt3.get("version", 0)) == SAVE_VERSION, "round-trip deve manter v28")

	# --- (4) Migração v20 → v21. ---
	var v20: Dictionary = sim.to_dict()
	v20["version"] = 20
	for city: Dictionary in v20.get("cities", []):
		var region: Dictionary = city.get("region", {})
		if region.has("infra"):
			region.erase("infra")
	for b: Dictionary in v20.get("buildings", []):
		if b.has("royalty"):
			b.erase("royalty")
	var migrated := EconomySim.new()
	_load_chains(migrated)
	migrated.from_dict(v20)
	var after: Dictionary = migrated.to_dict()
	_check(int(after.get("version", 0)) == SAVE_VERSION,
		"v20 deve migrar para v27 (é %s)" % str(after.get("version")))
	var mcity := _pick_city(after)
	if mcity != "":
		var mi := 0
		for city2: Dictionary in after.get("cities", []):
			if String(city2.get("id", "")) == mcity:
				break
			mi += 1
		var minfra: Dictionary = after.get("cities", [])[mi].get("region", {}).get("infra", {})
		_check(int(minfra.get("road", -1)) == 0 and int(minfra.get("power", -1)) == 0
			and int(minfra.get("water", -1)) == 0,
			"v20→v21: infra nasce 0")
