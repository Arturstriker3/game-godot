extends SceneTree

## Teste de determinismo do save/load do núcleo econômico C++ (revisão Q-008/S5).
##
## Verifica que "salvar no dia N, carregar e rodar até M" produz exatamente o
## mesmo estado que "rodar direto até M". É a guarda da migração do estado para
## `ogs::WorldState` (S5a): se algum campo saiu do save (`demo_to_dict`/
## `demo_from_dict`) ou algum estado transitório do `SimCore` fosse, por engano,
## necessário ao dia seguinte, o caminho "load + continua" divergiria do "direto".
##
## Roda SEM IA (núcleo determinístico): a IA vive em GDScript (EconomicAI) e o
## estado dela NÃO faz parte do save C++ — o determinismo da IA é coberto pelos
## goldens S0 (`real_60_ai`). Aqui o que se isola é o round-trip do núcleo.
##
##   godot --headless --path . --script res://tools/economy_save_load_roundtrip_test.gd \
##     [--days=60] [--save-at=30]

const GeographyServiceScript := preload("res://core/economy/geography_service.gd")

const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const PORTS_PATH := "res://data/definitions/scenario/ports.json"
const BUILDINGS_PATH := "res://data/definitions/scenario/buildings.json"
const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"
const SERVICES_PATH := "res://data/definitions/economy/services.json"
const CURRENCIES_PATH := "res://data/definitions/economy/currencies.json"
const SECTOR_EMPLOYMENT_PATH := "res://data/definitions/economy/sector_employment.json"

var _days := 60
var _save_at := 30
var _failures: Array[String] = []


func _init() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--days="):
			_days = maxi(1, int(arg.split("=", true, 1)[1]))
		elif arg.begins_with("--save-at="):
			_save_at = clampi(int(arg.split("=", true, 1)[1]), 1, _days - 1)
	_run()
	if _failures.is_empty():
		quit(0)
	else:
		for failure in _failures:
			print("[roundtrip] FALHA: " + failure)
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


## Carrega as tabelas externas (bens/receitas/necessidades/serviços/moedas/
## emprego) ANTES de inicializar o mundo — mesma ordem do jogo ao vivo. O load
## (`demo_from_dict`) resolve a receita de cada construção por id, então as
## tabelas precisam estar carregadas no destino também.
func _load_chains(sim) -> void:
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


## Cria o mundo real (cities/ports/buildings.json), sem IA — mesmo caminho do
## golden `real_60_noai`. Devolve o `EconomySim` pronto para o tick.
func _make_world() -> Object:
	var sim = EconomySim.new()
	_load_chains(sim)
	var cities := _load_array(CITIES_PATH)
	var ports := _load_array(PORTS_PATH)
	var buildings := _load_array(BUILDINGS_PATH)
	if cities.is_empty() or buildings.is_empty():
		_fail("cenario vazio (cidades=%d, construcoes=%d)" % [
			cities.size(), buildings.size()])
		return sim
	sim.initialize_demo_world(cities, ports, buildings)
	return sim


## Tica `days` dias e devolve {save_sha256, status_sha256} do estado final.
func _run_straight(days: int) -> Dictionary:
	var sim := _make_world()
	for day in days:
		var status: Dictionary = sim.tick_demo()
		var violations: Array = status.get("invariant_violations", [])
		if violations.size() > 0:
			_fail("direto: dia %d: invariante violado: %s" % [day + 1, String(violations[0])])
			break
	return _final_hashes(sim)


## Tica `save_at` dias, salva, carrega num EconomySim novo e tica o restante.
## Devolve {save_sha256, status_sha256} do estado final.
func _run_split(save_at: int, days: int) -> Dictionary:
	var sim := _make_world()
	for day in save_at:
		var status: Dictionary = sim.tick_demo()
		var violations: Array = status.get("invariant_violations", [])
		if violations.size() > 0:
			_fail("split: dia %d: invariante violado antes do save: %s" % [
				day + 1, String(violations[0])])
			break
	var snapshot: Dictionary = sim.demo_to_dict()

	var restored = EconomySim.new()
	_load_chains(restored)
	restored.demo_from_dict(snapshot)

	for day in range(save_at, days):
		var status: Dictionary = restored.tick_demo()
		var violations: Array = status.get("invariant_violations", [])
		if violations.size() > 0:
			_fail("split: dia %d: invariante violado após carregar: %s" % [
				day + 1, String(violations[0])])
			break
	return _final_hashes(restored)


func _final_hashes(sim) -> Dictionary:
	var save_dict: Dictionary = sim.demo_to_dict()
	var status_dict: Dictionary = sim.get_demo_status()
	return {
		"save_sha256": _sha(JSON.stringify(save_dict, "", true, true)),
		"status_sha256": _sha(JSON.stringify(status_dict, "", true, true)),
	}


func _run() -> void:
	var straight := _run_straight(_days)
	var split := _run_split(_save_at, _days)

	print("[roundtrip] direto %dd     save=%s status=%s" % [
		_days, straight.get("save_sha256", "?"), straight.get("status_sha256", "?")])
	print("[roundtrip] split %d+%dd  save=%s status=%s" % [
		_save_at, _days - _save_at, split.get("save_sha256", "?"), split.get("status_sha256", "?")])

	if _failures.is_empty():
		if String(straight.get("save_sha256", "")) != String(split.get("save_sha256", "")):
			_fail("save: direto != split (load no dia %d não reproduz o estado direto)" % _save_at)
		if String(straight.get("status_sha256", "")) != String(split.get("status_sha256", "")):
			_fail("status: direto != split (load no dia %d não reproduz o estado direto)" % _save_at)

	if _failures.is_empty():
		print("[roundtrip] OK: save no dia %d + continuar até %d == rodar %d dias direto (0 diffs)." % [
			_save_at, _days, _days])
