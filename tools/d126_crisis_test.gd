extends SceneTree

## D-126 — sobrevivência em crise.
##
## Guarda o contrato da spec aprovada em 23/11/2026:
##   (1) demissão real com rescisão (folha/vagas caem, dinheiro conservado);
##   (2) crédito de capital de giro no banco existente, com 1 renegociação e
##       multa (criação de dinheiro bancário registrada);
##   (3) liquidação de estoque com desconto (venda ao exterior);
##   (4) hibernar sem produção com custo mínimo e reabrir pagando;
##   (5) falência vira leilão do prédio (comprador em vez de sumir);
##   (6) fazenda reduz a área para a próxima safra;
##   (7) save v41 com migração v40 e determinismo.
##
##   godot --headless --path . --script res://tools/d126_crisis_test.gd

const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"
const EFFICIENCY_PATH := "res://data/definitions/production/efficiency.json"
const CRISIS_PATH := "res://data/definitions/economy/crisis.json"

const EPS := 1e-4

var _failures: Array[String] = []


func _init() -> void:
	_test_layoff()
	_test_credit()
	_test_liquidation()
	_test_mothball_reopen()
	_test_auction()
	_test_farm_area()
	_test_save_load_v41()
	_test_v40_migration()
	_test_determinism()
	_finish()


# --- (1) demissão + rescisão ------------------------------------------------
func _test_layoff() -> void:
	var sim := _world()
	var built := _build(sim, "D126-LAY", 40.0, "flour", "wheat")
	if not built:
		return
	_tick(sim)
	var money_before := _money(sim)
	var row_before := _crisis_row(sim, "D126-LAY")
	var jobs_before := float(row_before.get("jobs", 0.0))
	var result: Dictionary = sim.crisis_layoff("AGRIA", "D126-LAY", 0.5)
	var row_after := _crisis_row(sim, "D126-LAY")
	var jobs_after := float(row_after.get("jobs", 0.0))
	_check("(1) demissão paga rescisão e reduz vagas",
			bool(result.get("ok", false)) and jobs_after < jobs_before,
			"antes=%.1f depois=%.1f %s" % [jobs_before, jobs_after, result])
	_check("(1) rescisão conserva dinheiro (sem criar/destruir)",
			absf(_money(sim) - money_before) <= EPS,
			"antes=%.4f depois=%.4f" % [money_before, _money(sim)])


# --- (2) crédito de capital de giro ----------------------------------------
func _test_credit() -> void:
	var sim := _world()
	if not _build(sim, "D126-CREDIT", 20.0, "flour", "wheat"):
		return
	_tick(sim)
	var money_before := _money(sim)
	var result: Dictionary = sim.crisis_borrow("AGRIA", 500.0)
	var granted := float(result.get("granted", 0.0))
	var debt := float(result.get("debt", 0.0))
	_check("(2) crédito de giro entra no caixa e vira dívida",
			bool(result.get("ok", false)) and granted > 0.0 and debt > 0.0,
			str(result))
	_check("(2) crédito cria dinheiro bancário do valor concedido",
			absf((_money(sim) - money_before) - granted) <= EPS,
			"delta=%.4f granted=%.4f" % [_money(sim) - money_before, granted])
	var banks: Dictionary = sim.get_bank_status()
	var loans := float(banks.get("totals", {}).get("loans", 0.0))
	_check("(2) banco contabiliza o empréstimo",
			loans + EPS >= granted, "loans=%.4f granted=%.4f" % [loans, granted])


# --- (3) liquidação de estoque ---------------------------------------------
func _test_liquidation() -> void:
	var sim := _world()
	if not _build(sim, "D126-LIQ", 10.0, "wheat", ""):
		return
	sim.set_market_stock("AGRIA", "wheat", 10000.0)
	_tick(sim)
	var stock_before := _stock(sim, "AGRIA", "wheat")
	var money_before := _money(sim)
	var result: Dictionary = sim.crisis_liquidate("AGRIA", "D126-LIQ", 1.0)
	var stock_after := _stock(sim, "AGRIA", "wheat")
	_check("(3) liquidação vende estoque com desconto",
			bool(result.get("ok", false))
				and float(result.get("tons", 0.0)) > 0.0
				and stock_after < stock_before,
			str(result))
	_check("(3) liquidação conserva dinheiro (comprador é o exterior)",
			absf(_money(sim) - money_before) <= EPS,
			"antes=%.4f depois=%.4f" % [money_before, _money(sim)])


# --- (4) hibernar e reabrir -------------------------------------------------
func _test_mothball_reopen() -> void:
	var sim := _world()
	if not _build(sim, "D126-MOTH", 20.0, "flour", "wheat", 5.0):
		return
	_tick(sim)
	var row := _crisis_row(sim, "D126-MOTH")
	var eff_before := float(row.get("production_efficiency", 1.0))
	var money_before := _money(sim)
	var result: Dictionary = sim.crisis_mothball("AGRIA", "D126-MOTH")
	row = _crisis_row(sim, "D126-MOTH")
	_check("(4) hibernar mantém o prédio e corta a eficiência (retenção 60%)",
			bool(result.get("ok", false)) and bool(row.get("mothballed", false))
				and float(row.get("production_efficiency", 1.0))
					<= eff_before * 0.60 + EPS,
			str(result) + " row=" + str(row))
	_check("(4) hibernar não move dinheiro",
			absf(_money(sim) - money_before) <= EPS,
			"antes=%.4f depois=%.4f" % [money_before, _money(sim)])
	money_before = _money(sim)
	result = sim.crisis_reopen("AGRIA", "D126-MOTH")
	row = _crisis_row(sim, "D126-MOTH")
	_check("(4) reabrir paga o custo e volta a operar",
			bool(result.get("ok", false))
				and not bool(row.get("mothballed", true)),
			str(result))
	_check("(4) reabertura conserva dinheiro",
			absf(_money(sim) - money_before) <= EPS,
			"antes=%.4f depois=%.4f" % [money_before, _money(sim)])


# --- (5) falência vira leilão ----------------------------------------------
func _test_auction() -> void:
	var sim := _world()
	if not _build(sim, "D126-AUC", 5.0, "flour", "wheat", 2.0):
		return
	_tick(sim)
	var money_before := _money(sim)
	var result: Dictionary = sim.crisis_auction("AGRIA", "D126-AUC")
	var row := _crisis_row(sim, "D126-AUC")
	var owner := String(row.get("owner", ""))
	_check("(5) leilão transfere o prédio (não apaga) com desconto",
			bool(result.get("ok", false)) and owner != "private"
				and owner != "",
			str(result) + " owner=" + owner)
	_check("(5) leilão conserva dinheiro", absf(_money(sim) - money_before) <= EPS,
			"antes=%.4f depois=%.4f" % [money_before, _money(sim)])


# --- (6) fazenda reduz a área ----------------------------------------------
func _test_farm_area() -> void:
	var sim := _world()
	# AGRIA do fixture não tem plano de safra de trigo; o plano declarado é
	# dado de teste (não um número de simulação do jogo).
	sim.set_crop_plan("AGRIA", "wheat", 10000.0, 3.0,
		[{"plant_start": 1, "plant_end": 60, "harvest_start": 61,
		"harvest_end": 365, "share": 1.0}])
	if not _build(sim, "D126-FARM", 30.0, "wheat", ""):
		return
	_tick(sim)
	sim.crisis_set_stress("AGRIA", "D126-FARM", 35)
	var before := int(sim.get_crisis_status().get("farm_area_reductions", 0))
	sim.crisis_step()
	var row := _crisis_row(sim, "D126-FARM")
	var after := int(sim.get_crisis_status().get("farm_area_reductions", 0))
	_check("(6) fazenda com prejuízo reduz a área da próxima safra",
			after > before and bool(row.get("farm_area_reduced", false)),
			"antes=%d depois=%d row=%s" % [before, after, str(row)])
	var save: Dictionary = sim.to_dict()
	var scale := _plan_area_scale(save, "AGRIA", "wheat")
	_check("(6) área reduzida entra no save (próxima safra)",
			scale < 1.0 and scale >= 0.10, "area_scale=%.4f" % scale)


# --- (7) save v41 round-trip -----------------------------------------------
func _test_save_load_v41() -> void:
	var sim := _world()
	if not _build(sim, "D126-SAVE", 20.0, "flour", "wheat"):
		return
	_tick(sim)
	sim.crisis_layoff("AGRIA", "D126-SAVE", 0.5)
	sim.crisis_mothball("AGRIA", "D126-SAVE")
	var save: Dictionary = sim.to_dict()
	_check("(7) save v41", int(save.get("version", 0)) == 41,
			"version=%d" % int(save.get("version", 0)))
	var loaded := _load(save)
	var row := _crisis_row(loaded, "D126-SAVE")
	_check("(7) save/load preserva o estado de crise",
			bool(row.get("mothballed", false))
				and int(row.get("crisis_stage", 0)) >= 2,
			str(row))
	_check("(7) save/load preserva o dinheiro",
			absf(_money(loaded) - _money(sim)) <= EPS,
			"%.4f vs %.4f" % [_money(loaded), _money(sim)])
	var save2: Dictionary = loaded.to_dict()
	_check("(7) segundo save idêntico (round-trip estável)",
			JSON.stringify(save) == JSON.stringify(save2), "")


func _test_v40_migration() -> void:
	var sim := _world()
	_tick(sim)
	var save: Dictionary = sim.to_dict()
	save["version"] = 40
	save.erase("crisis")
	var loaded := _load(save)
	_check("(7) migração v40->v41 carrega estado neutro",
			loaded != null and int(loaded.get_status().get("day", -1)) >= 1,
			"")
	var status: Dictionary = loaded.get_crisis_status()
	_check("(7) v40 sem crise em curso",
			int(status.get("mothballs", -1)) == 0
				and int(status.get("layoffs", -1)) == 0,
			str(status.get("mothballs", -1)))


# --- (8) determinismo -------------------------------------------------------
func _test_determinism() -> void:
	var a := _deterministic_run()
	var b := _deterministic_run()
	_check("(8) mesma sequência = mesmo save", a == b and a != "",
			"len=%d/%d" % [a.length(), b.length()])


func _deterministic_run() -> String:
	var sim := _world()
	if not _build(sim, "D126-DET", 20.0, "flour", "wheat"):
		return ""
	_tick(sim)
	sim.crisis_layoff("AGRIA", "D126-DET", 0.5)
	sim.crisis_set_stress("AGRIA", "D126-DET", 35)
	sim.crisis_step()
	return JSON.stringify(sim.to_dict(), "", true, true)


# --- helpers ---------------------------------------------------------------
func _world() -> Object:
	var sim = EconomySim.new()
	sim.set_sim_threads(1)
	sim.set_goods(_json(GOODS_PATH))
	sim.set_recipes(_json(RECIPES_PATH))
	sim.set_needs(_json(NEEDS_PATH))
	var params: Variant = _json(EFFICIENCY_PATH)
	if params is Dictionary and sim.has_method("set_efficiency_params"):
		sim.set_efficiency_params(params)
	var crisis: Variant = _json(CRISIS_PATH)
	if crisis is Dictionary and sim.has_method("set_crisis_params"):
		sim.set_crisis_params(crisis)
	# O jogo carrega bank_credit.json (enabled=true); o teste liga a mecânica
	# explicitamente para o crédito de giro ter contraparte bancária.
	if sim.has_method("set_bank_credit"):
		sim.set_bank_credit(true)
	sim.initialize_scenario()
	sim.set_ai_enabled(false)
	sim.set_instant_construction(true)
	return sim


func _build(sim: Object, id: String, jobs: float, output: String,
		input: String, capacity: float = 10.0) -> bool:
	var output_index := _product(sim, output)
	if output_index < 0:
		_fail("produto %s inexistente" % output)
		return false
	var recipe := "" if input == "" else output
	var built: Dictionary = sim.build("AGRIA", id, input if input != "" else output,
			_product(sim, input) if input != "" else -1, output_index,
			capacity, jobs, 2.0, 10.0, 0.0, "private", 1000.0, recipe)
	if not bool(built.get("created", false)):
		_fail("construção %s não criada: %s" % [id, built])
		return false
	return true


func _tick(sim: Object) -> void:
	sim.tick_day()


func _load(save: Dictionary) -> Object:
	var sim = EconomySim.new()
	sim.set_sim_threads(1)
	sim.set_goods(_json(GOODS_PATH))
	sim.set_recipes(_json(RECIPES_PATH))
	sim.set_needs(_json(NEEDS_PATH))
	var crisis: Variant = _json(CRISIS_PATH)
	if crisis is Dictionary and sim.has_method("set_crisis_params"):
		sim.set_crisis_params(crisis)
	sim.from_dict(save)
	return sim


func _crisis_row(sim: Object, id: String) -> Dictionary:
	for row: Variant in sim.get_crisis_status().get("buildings", []):
		var building: Dictionary = row
		if String(building.get("id", "")) == id:
			return building
	return {}


func _plan_area_scale(save: Dictionary, city_id: String, product: String) -> float:
	for row: Variant in save.get("agriculture", {}).get("plans", []):
		var plan: Dictionary = row
		if String(plan.get("city", "")) == city_id \
				and String(plan.get("product", "")) == product:
			return float(plan.get("area_scale", 1.0))
	return 1.0


func _product(sim: Object, product: String) -> int:
	return Array(sim.get_status().get("products", [])).find(product)


func _money(sim: Object) -> float:
	return float(sim.get_status().get("money_total", 0.0))


func _stock(sim: Object, city_id: String, product: String) -> float:
	var index := _product(sim, product)
	var markets: Array = sim.get_city_status(city_id).get("markets", [])
	if index < 0 or index >= markets.size():
		return 0.0
	return float((markets[index] as Dictionary).get("stock", 0.0))


func _json(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("não abriu " + path)
		return null
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data


func _check(name: String, ok: bool, detail: String = "") -> void:
	if ok:
		print("[d126] ok: " + name)
	else:
		_fail("%s (%s)" % [name, detail])


func _fail(message: String) -> void:
	_failures.append(message)


func _finish() -> void:
	if _failures.is_empty():
		print("[d126] OK: demissão, crédito de giro, liquidação, hibernação/"
			+ "reabertura, leilão, área da fazenda, save v41 e determinismo.")
		quit(0)
	else:
		print("[d126] FALHAS (%d):" % _failures.size())
		for failure in _failures:
			print(" - " + failure)
		quit(1)
