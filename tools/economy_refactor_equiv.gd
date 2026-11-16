extends SceneTree

## Harness de equivalência byte a byte do núcleo econômico C++ (S0 do
## `docs/economy/core_refactor_plan.md`). Roda o cenário real (ou o AGRIA/PORTUS)
## por N dias, captura `demo_to_dict()` (save integral), `get_demo_status()`
## (status integral) e `demo_get_profile()` (meta informativa) e compara o
## SHA-256 do JSON canônico (chaves ordenadas, precisão total) contra um golden.
##
##   godot --headless --path . --script res://tools/economy_refactor_equiv.gd \
##     [--mode=demo|world] [--days=60] [--ai=0|1] [--profile=0|1] \
##     [--out=path.json] [--baseline=path.json]
##
## - Sem `--baseline`, grava (ou imprime) o golden do binário em uso.
## - Com `--baseline`, compara `save_sha256` e `status_sha256` (0 diferenças =
##   passo verde); `profile` é tempo e NÃO entra na equivalência estrita.
## - Determinismo: rodar duas vezes sem baseline tem de dar o MESMO sha.

const GeographyServiceScript := preload("res://core/economy/geography_service.gd")
const EconomicAIScript := preload("res://core/economy/ai/economic_ai.gd")

const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const PORTS_PATH := "res://data/definitions/scenario/ports.json"
const BUILDINGS_PATH := "res://data/definitions/scenario/buildings.json"
const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"
const SERVICES_PATH := "res://data/definitions/economy/services.json"
const CURRENCIES_PATH := "res://data/definitions/economy/currencies.json"
const SECTOR_EMPLOYMENT_PATH := "res://data/definitions/economy/sector_employment.json"

const AGRIA_CENTER := Vector2(2532.6, 571.4)
const PORTUS_CENTER := Vector2(2560.4, 543.8)

var _mode := "world"
var _days := 60
var _ai := true
var _profile := false
var _threads := 1
var _out := ""
var _baseline := ""

var _failures: Array[String] = []


func _init() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--mode="):
			_mode = arg.split("=", true, 1)[1]
		elif arg.begins_with("--days="):
			_days = maxi(1, int(arg.split("=", true, 1)[1]))
		elif arg.begins_with("--ai="):
			_ai = arg.split("=", true, 1)[1] != "0"
		elif arg.begins_with("--profile="):
			_profile = arg.split("=", true, 1)[1] != "0"
		elif arg.begins_with("--threads="):
			_threads = maxi(1, int(arg.split("=", true, 1)[1]))
		elif arg.begins_with("--out="):
			_out = arg.split("=", true, 1)[1]
		elif arg.begins_with("--baseline="):
			_baseline = arg.split("=", true, 1)[1]
	_run()
	if _failures.is_empty():
		quit(0)
	else:
		for failure in _failures:
			print("[equiv] FALHA: " + failure)
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


func _load_array(path: String) -> Array:
	var data: Variant = _load_json(path)
	if typeof(data) != TYPE_ARRAY:
		_fail(path + " nao e um array JSON")
		return []
	return data


func _load_dict(path: String) -> Dictionary:
	var data: Variant = _load_json(path)
	if typeof(data) != TYPE_DICTIONARY:
		_fail(path + " nao e um objeto JSON")
		return {}
	return data


func _sha(json_str: String) -> String:
	return json_str.sha256_text()


func _cities_with_center(cities: Array) -> Array:
	var result: Array = []
	for city in cities:
		result.append({
			"id": String(city.get("id", "")),
			"nation": String(city.get("nation", "")),
			"center": Vector2(float(city.get("x", 0.0)), float(city.get("y", 0.0))),
		})
	return result


func _run() -> void:
	var sim = EconomySim.new()

	# F2/F3/F8/F9: bens, receitas, necessidades, serviços, moedas e emprego
	# setorial entram ANTES de criar o mundo (mesma ordem do jogo ao vivo).
	sim.demo_set_goods(_load_array(GOODS_PATH))
	sim.demo_set_recipes(_load_array(RECIPES_PATH))
	sim.demo_set_needs(_load_dict(NEEDS_PATH))
	if sim.has_method("demo_set_services"):
		sim.demo_set_services(_load_dict(SERVICES_PATH))
	if sim.has_method("demo_set_currencies"):
		sim.demo_set_currencies(_load_dict(CURRENCIES_PATH))
	if sim.has_method("demo_set_sector_employment"):
		var sector := _load_dict(SECTOR_EMPLOYMENT_PATH)
		if not sector.is_empty():
			sim.demo_set_sector_employment(sector)
	if sim.has_method("demo_set_profiling"):
		sim.demo_set_profiling(_profile)
	if sim.has_method("demo_set_sim_threads"):
		sim.demo_set_sim_threads(_threads)

	var geo: Object = null
	var ai: Object = null

	if _mode == "demo":
		# AGRIA/PORTUS (2 cidades), sem IA — mesmo setup do economy_integration_test.
		sim.initialize_demo()
		geo = GeographyServiceScript.new()
		geo.load_data()
		geo.apply_to_sim(sim, [
			{"id": "AGRIA", "center": AGRIA_CENTER},
			{"id": "PORTUS", "center": PORTUS_CENTER},
		])
	else:
		var cities := _load_array(CITIES_PATH)
		var ports := _load_array(PORTS_PATH)
		var buildings := _load_array(BUILDINGS_PATH)
		if cities.is_empty() or buildings.is_empty():
			_fail("cenario vazio (cidades=%d, construcoes=%d)" % [
				cities.size(), buildings.size()])
			return
		if _ai:
			geo = GeographyServiceScript.new()
			geo.load_data()
			buildings = geo.resolve_initial_farms(buildings, cities)
		sim.initialize_demo_world(cities, ports, buildings)
		if geo != null:
			geo.apply_to_sim(sim, _cities_with_center(cities))
			ai = EconomicAIScript.new()
			ai.setup(sim, geo)

	# Dia 0 já publicado; aquece os dois slots do status (o custo de alocação
	# não entra no primeiro tick) — indiferente para o hash, mas deixa o perfil
	# comparável entre execuções.
	if sim.has_method("demo_warm_status"):
		sim.demo_warm_status()

	for day in _days:
		var status: Dictionary = sim.tick_demo()
		if ai != null:
			ai.run(int(status.get("day", day + 1)), status)
		var violations: Array = status.get("invariant_violations", [])
		if violations.size() > 0:
			_fail("dia %d: invariante violado: %s" % [day + 1, String(violations[0])])
			return

	var save_dict: Dictionary = sim.demo_to_dict()
	var status_dict: Dictionary = sim.get_demo_status()
	var profile_dict: Dictionary = {}
	if sim.has_method("demo_get_profile"):
		profile_dict = sim.demo_get_profile()

	var save_sha := _sha(JSON.stringify(save_dict, "", true, true))
	var status_sha := _sha(JSON.stringify(status_dict, "", true, true))

	var result := {
		"meta": {
			"mode": _mode,
			"days": _days,
			"ai": _ai,
			"products": Array(status_dict.get("products", [])).size(),
			"money_total": float(status_dict.get("money_total", 0.0)),
			"bankruptcies": int(status_dict.get("bankruptcies", 0)),
			"service_businesses": int(status_dict.get("service_businesses", 0)),
		},
		"save_sha256": save_sha,
		"status_sha256": status_sha,
		"profile": profile_dict,
	}

	print("[equiv] modo=%s dias=%d ai=%s | save=%s | status=%s | money=%.2f falencias=%d" % [
		_mode, _days, str(_ai), save_sha, status_sha,
		float(result["meta"]["money_total"]), int(result["meta"]["bankruptcies"])])

	if _out != "":
		var dir := _out.get_base_dir()
		if dir != "" and not DirAccess.dir_exists_absolute(dir):
			DirAccess.make_dir_recursive_absolute(dir)
		var file := FileAccess.open(_out, FileAccess.WRITE)
		if file == null:
			_fail("nao abriu para escrita: " + _out)
			return
		file.store_string(JSON.stringify(result, "", true, true) + "\n")
		file.close()
		print("[equiv] escrito " + _out)

	if _baseline != "":
		_compare_baseline(result)


func _compare_baseline(result: Dictionary) -> void:
	var data: Variant = _load_json(_baseline)
	if typeof(data) != TYPE_DICTIONARY:
		_fail("baseline ilegivel: " + _baseline)
		return
	var baseline: Dictionary = data
	var diffs: Array[String] = []
	if String(baseline.get("save_sha256", "")) != String(result["save_sha256"]):
		diffs.append("save (save_sha256 %s != %s)" % [
			String(baseline.get("save_sha256", "")), String(result["save_sha256"])])
	if String(baseline.get("status_sha256", "")) != String(result["status_sha256"]):
		diffs.append("status (status_sha256 %s != %s)" % [
			String(baseline.get("status_sha256", "")), String(result["status_sha256"])])
	var meta: Dictionary = baseline.get("meta", {})
	var result_meta: Dictionary = result["meta"]
	if String(meta.get("mode", "")) != String(result_meta["mode"]) \
			or int(meta.get("days", 0)) != int(result_meta["days"]) \
			or bool(meta.get("ai", false)) != bool(result_meta["ai"]):
		diffs.append("meta divergente (modo/dias/ai)")
	if diffs.is_empty():
		print("[equiv] OK: 0 diferenças vs " + _baseline)
	else:
		for diff in diffs:
			_fail(diff)
