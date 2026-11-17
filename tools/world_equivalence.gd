extends SceneTree

## Equivalência funcional A1–A5 do plano `docs/plans/perf_pop_financas_plano.md`.
##
## Roda o mundo real (cenário + IA + camada urbana S19) por N dias com semente
## fixa e grava um JSON canônico — `meta`, `save` (`to_dict()`) e `status`
## (`get_status()`) — em chaves ordenadas e precisão total. O par de SHA-256
## continua sendo a checagem grossa; o dump existe para o comparador
## `tools/world_equivalence_compare.py` apontar o PRIMEIRO caminho divergente.
##
##   godot --headless --path . --script res://tools/world_equivalence.gd \
##     -- --days=60 --ai=1 --urban=1 --seed=0 --threads=0 --out=mundo.json \
##        [--meta-out=golden.json]
##
## - `--out` é obrigatório (sem ele nada roda: 60 d não se repetem de graça).
## - `--meta-out` (opcional) grava só o `meta` sem `generated_utc` — um golden
##   pequeno e versionável (sha do save, sha do status e a config), para
##   `tools/world_equivalence.ps1 -Golden/-Baseline` comparar sem carregar o
##   dump de 110 MB. Não altera o dump principal nem os shas.
## - `--threads=0` deixa o núcleo decidir o caminho serial/paralelo (mesmo
##   contrato de `set_sim_threads`).
## - `--seed` entra em `meta` apenas: o núcleo não tem RNG, o mundo é
##   determinístico por construção. Nenhuma moeda é perturbada aqui — a
##   perturbação de câmbio (±1e-12) vive em `tools/economy_world_test.gd` e não
##   faz parte da equivalência de refactor.
## - Saída: 0 ok, 1 falha. Toda falha vira linha `[equiv] FALHA: ...`.

const GeographyServiceScript := preload("res://core/economy/geography_service.gd")
const GroupNamerScript := preload("res://core/economy/group_namer.gd")

const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const PORTS_PATH := "res://data/definitions/scenario/ports.json"
const BUILDINGS_PATH := "res://data/definitions/scenario/buildings.json"
const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"
const SERVICES_PATH := "res://data/definitions/economy/services.json"
const CURRENCIES_PATH := "res://data/definitions/economy/currencies.json"
const SECTOR_EMPLOYMENT_PATH := "res://data/definitions/economy/sector_employment.json"
## §19 (W0): mesmos caminhos que o autoload do jogo usa para a camada urbana.
const URBAN_LAND_PATH := "res://data/definitions/economy/urban_land.json"
const HOUSING_CITY_PATH := "res://data/definitions/economy/housing_city.json"
const HOUSING_PATH := "res://data/definitions/economy/housing.json"
const MUNICIPAL_FINANCE_PATH := "res://data/definitions/economy/municipal_finance.json"

var _days := 60
var _ai := true
var _urban := true
var _seed := 0
var _threads := 0
var _out := ""
var _meta_out := ""

var _failures: Array[String] = []


func _init() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--days="):
			_days = maxi(1, int(arg.split("=", true, 1)[1]))
		elif arg.begins_with("--ai="):
			_ai = arg.split("=", true, 1)[1] != "0"
		elif arg.begins_with("--urban="):
			_urban = arg.split("=", true, 1)[1] != "0"
		elif arg.begins_with("--seed="):
			_seed = int(arg.split("=", true, 1)[1])
		elif arg.begins_with("--threads="):
			_threads = maxi(0, int(arg.split("=", true, 1)[1]))
		elif arg.begins_with("--out="):
			_out = arg.split("=", true, 1)[1]
		elif arg.begins_with("--meta-out="):
			_meta_out = arg.split("=", true, 1)[1]

	if _out == "":
		print("[equiv] FALHA: --out=<arquivo.json> e obrigatorio")
		quit(1)
		return

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


## Cidades no formato que o GeographyService.apply_to_sim espera.
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

	# Mesma ordem do jogo ao vivo: definições ANTES de criar o mundo.
	sim.set_goods(_load_array(GOODS_PATH))
	sim.set_recipes(_load_array(RECIPES_PATH))
	sim.set_needs(_load_dict(NEEDS_PATH))
	if sim.has_method("set_services"):
		sim.set_services(_load_dict(SERVICES_PATH))
	if sim.has_method("set_currencies"):
		sim.set_currencies(_load_dict(CURRENCIES_PATH))
	if sim.has_method("set_sector_employment"):
		var sector := _load_dict(SECTOR_EMPLOYMENT_PATH)
		if not sector.is_empty():
			sim.set_sector_employment(sector)
	if sim.has_method("set_sim_threads"):
		sim.set_sim_threads(_threads)
	if not _failures.is_empty():
		return

	var cities := _load_array(CITIES_PATH)
	var ports := _load_array(PORTS_PATH)
	var buildings := _load_array(BUILDINGS_PATH)
	if cities.is_empty() or buildings.is_empty():
		_fail("cenario vazio (cidades=%d, construcoes=%d)" % [
			cities.size(), buildings.size()])
		return

	var geo: Object = null
	if _ai:
		geo = GeographyServiceScript.new()
		geo.load_data()
		buildings = geo.resolve_initial_farms(buildings, cities)

	sim.initialize_world(cities, ports, buildings)

	# §19 (W0): solo, moradia e prefeitura por cidade, como o autoload faz.
	if _urban:
		if not sim.has_method("set_urban"):
			_fail("binario sem set_urban (rode o build)")
			return
		sim.set_urban(_load_dict(URBAN_LAND_PATH),
			_load_dict(HOUSING_CITY_PATH), _load_dict(HOUSING_PATH),
			_load_dict(MUNICIPAL_FINANCE_PATH))
		if not _failures.is_empty():
			return

	if geo != null:
		geo.apply_to_sim(sim, _cities_with_center(cities))
		# A IA econômica roda no núcleo, como fase do dia (A5). Os grupos são
		# nomeados SEM `places`: esta corrida não carrega a cultura das cidades.
		sim.set_ai_enabled(true)
		GroupNamerScript.name_groups(sim)

	# Aquece os dois slots do status (custo de alocação fora do primeiro tick).
	if sim.has_method("warm_status"):
		sim.warm_status()

	for _day in _days:
		# A IA já rodou dentro do `tick_day()` (fase pós-status): o retorno é o
		# status do dia ANTES das decisões dela.
		var status: Dictionary = sim.tick_day()
		var violations: Array = status.get("invariant_violations", [])
		if violations.size() > 0:
			_fail("dia %d: invariante violado: %s" % [
				int(status.get("day", 0)), String(violations[0])])
			return

	var save_dict: Dictionary = sim.to_dict()
	var status_dict: Dictionary = sim.get_status()

	# Chaves ordenadas + precisão total: o mesmo string serve de hash e de dump.
	var save_json := JSON.stringify(save_dict, "  ", true, true)
	var status_json := JSON.stringify(status_dict, "  ", true, true)
	var save_sha := save_json.sha256_text()
	var status_sha := status_json.sha256_text()

	var result := {
		"meta": {
			"days": _days,
			"ai": _ai,
			"urban": _urban,
			"seed": _seed,
			"threads": _threads,
			"products": Array(status_dict.get("products", [])).size(),
			"money_total": float(status_dict.get("money_total", 0.0)),
			"bankruptcies": int(status_dict.get("bankruptcies", 0)),
			"service_businesses": int(status_dict.get("service_businesses", 0)),
			"save_sha256": save_sha,
			"status_sha256": status_sha,
			"generated_utc": Time.get_datetime_string_from_system(true, true),
		},
		"save": save_dict,
		"status": status_dict,
	}

	var text := JSON.stringify(result, "  ", true, true) + "\n"

	var dir := _out.get_base_dir()
	if dir != "" and not DirAccess.dir_exists_absolute(dir):
		DirAccess.make_dir_recursive_absolute(dir)
	var file := FileAccess.open(_out, FileAccess.WRITE)
	if file == null:
		_fail("nao abriu para escrita: " + _out)
		return
	file.store_string(text)
	file.close()

	print("[equiv] dias=%d ai=%s urban=%s seed=%d threads=%d | save=%s | status=%s | money=%.2f falencias=%d" % [
		_days, str(_ai), str(_urban), _seed, _threads, save_sha, status_sha,
		float(result["meta"]["money_total"]), int(result["meta"]["bankruptcies"])])
	print("[equiv] escrito " + _out)

	# Golden pequeno e versionavel: o mesmo `meta`, sem `generated_utc` (relogio
	# de parede). `config.threads_effective` fica de fora: o núcleo nao expoe o
	# numero efetivo de workers, so o pedido (`threads`).
	if _meta_out != "":
		var meta_copy: Dictionary = result["meta"].duplicate(true)
		meta_copy.erase("generated_utc")
		if not _write_json(_meta_out, meta_copy):
			return
		print("[equiv] meta escrito " + _meta_out)


func _write_json(path: String, data: Dictionary) -> bool:
	var dir := path.get_base_dir()
	if dir != "" and not DirAccess.dir_exists_absolute(dir):
		DirAccess.make_dir_recursive_absolute(dir)
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		_fail("nao abriu para escrita: " + path)
		return false
	file.store_string(JSON.stringify(data, "  ", true, true) + "\n")
	file.close()
	return true
