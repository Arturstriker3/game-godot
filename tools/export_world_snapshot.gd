extends SceneTree

## Materializa o snapshot do mundo de partida (A1 — núcleo C++ fora do Godot).
##
## Monta o mundo real exatamente como `tools/world_equivalence.gd` monta. Sem
## `--ai=1` nada de `GeographyService`/`resolve_initial_farms` e, ao final,
## grava `to_snapshot()` — o par `{"definitions": ..., "save": ...}` que carrega
## o mundo inteiro, sem depender dos JSON de `data/definitions/`.
##
##   godot --headless --path . --script res://tools/export_world_snapshot.gd \
##     -- --out=snapshot.json [--urban=1] [--ai=0|1]
##
## - `--out` é obrigatório.
## - `--urban=1` (padrão) aplica solo, moradia e prefeitura por cidade com os
##   MESMOS quatro arquivos do autoload do jogo; `--urban=0` exporta sem a
##   camada urbana.
## - `--ai=1` (padrão 0) monta o mundo com a geografia real, como
##   `tools/world_equivalence.gd` com `--ai=1`: `GeoService.load_data()` +
##   `resolve_initial_farms` ANTES de `initialize_world`, `apply_to_sim` (e os
##   nomes dos grupos) depois de `set_urban` e antes de `seed_industry`. O
##   snapshot passa a carregar `definitions.land_capacity`. A IA NÃO é ligada
##   aqui: o export tem 0 dias.
## - `seed_industry()` entra depois do mundo: o bootstrap industrial do
##   jogo roda no primeiro tick, então sem ele o snapshot não é o estado de
##   partida real (o executável C++ recusa o arquivo).
## - Serialização: `to_snapshot_file()`, que grava `{"definitions": ...,
##   "save": ...}` pelo serializador da BIBLIOTECA (`ogs::save::dump_json`,
##   nlohmann). NÃO se usa `JSON.stringify`: o JSON do Godot não é
##   round-trip IEEE-exato para |x| < 1, e o snapshot tem de chegar ao CLI
##   bit a bit igual ao que sai do núcleo.
## - Saída: 0 ok, 1 falha. Toda falha vira linha `[snapshot] FALHA: ...`.

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
## B1: estrutura etária por nação (definição estática; já vai ao snapshot).
const AGE_STRUCTURE_PATH := "res://data/definitions/population/age_structure_2026.json"
## §19 (W0): mesmos caminhos que o autoload do jogo usa para a camada urbana.
const URBAN_LAND_PATH := "res://data/definitions/economy/urban_land.json"
const HOUSING_CITY_PATH := "res://data/definitions/economy/housing_city.json"
const HOUSING_PATH := "res://data/definitions/economy/housing.json"
const MUNICIPAL_FINANCE_PATH := "res://data/definitions/economy/municipal_finance.json"

var _out := ""
var _urban := true
## `--ai=1`: monta com geografia real (e nomes de grupo), como o harness de
## equivalência. A IA em si NÃO é ligada — o export não roda dia nenhum.
var _ai := false

var _failures: Array[String] = []


func _init() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--out="):
			_out = arg.split("=", true, 1)[1]
		elif arg.begins_with("--urban="):
			_urban = arg.split("=", true, 1)[1] != "0"
		elif arg.begins_with("--ai="):
			_ai = arg.split("=", true, 1)[1] != "0"

	if _out == "":
		print("[snapshot] FALHA: --out=<arquivo.json> e obrigatorio")
		quit(1)
		return

	_run()
	if _failures.is_empty():
		quit(0)
	else:
		for failure in _failures:
			print("[snapshot] FALHA: " + failure)
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
	if data == null:
		_fail(path + " nao e JSON valido")
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
	for city: Variant in cities:
		var entry: Dictionary = city
		result.append({
			"id": String(entry.get("id", "")),
			"nation": String(entry.get("nation", "")),
			"center": Vector2(float(entry.get("x", 0.0)), float(entry.get("y", 0.0))),
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
	# B1: estrutura etária por nação (definição estática), antes do mundo.
	var age_structure := _load_dict(AGE_STRUCTURE_PATH)
	if sim.has_method("set_age_structure") and not age_structure.is_empty():
		sim.set_age_structure(age_structure)
	if not _failures.is_empty():
		return

	var cities := _load_array(CITIES_PATH)
	var ports := _load_array(PORTS_PATH)
	var buildings := _load_array(BUILDINGS_PATH)
	if cities.is_empty() or buildings.is_empty():
		_fail("cenario vazio (cidades=%d, construcoes=%d)" % [
			cities.size(), buildings.size()])
		return

	# Com `--ai=1` o mundo nasce com a geografia real, como o harness de
	# equivalência: a fazenda inicial é resolvida ANTES de criar o mundo e, em
	# seguida, as minas históricas do D-086 (uma por cidade × mineral com
	# jazida), já no último estágio.
	var geo: Object = null
	if _ai:
		geo = GeographyServiceScript.new()
		geo.load_data()
		buildings = geo.resolve_initial_farms(buildings, cities)
		if geo.has_method("resolve_initial_mines"):
			buildings = geo.resolve_initial_mines(buildings, cities)
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

	# Geografia real (aptidão + depósitos) e nome dos grupos, como o harness com
	# `--ai=1`: DEPOIS da camada urbana e ANTES do bootstrap industrial.
	if geo != null:
		geo.apply_to_sim(sim, _cities_with_center(cities))
		GroupNamerScript.name_groups(sim)

	# O bootstrap industrial (§12.3) roda no PRIMEIRO tick do jogo, não na
	# criação do mundo: sem ele a foto do dia 0 não é o estado de partida.
	if not sim.has_method("seed_industry"):
		_fail("binario sem seed_industry (rode o build)")
		return
	sim.seed_industry()

	if not sim.has_method("to_snapshot_file"):
		_fail("binario sem to_snapshot_file (rode o build)")
		return

	# O arquivo é escrito pelo serializador da BIBLIOTECA (`ogs::save::dump_json`),
	# não por `JSON.stringify`: só esse caminho é round-trip IEEE-exato e é o
	# mesmo do CLI headless — é o que deixa replay e CLI comparáveis campo a campo.
	var dir := _out.get_base_dir()
	if dir != "" and not DirAccess.dir_exists_absolute(dir):
		DirAccess.make_dir_recursive_absolute(dir)
	if not sim.to_snapshot_file(_out):
		_fail("to_snapshot_file nao gravou " + _out)
		return

	# Contagens SÓ para a mensagem: `to_snapshot()` (Dictionary) passa pelo
	# JSON do Godot e não é gravado em lugar nenhum.
	if not sim.has_method("to_snapshot"):
		_fail("binario sem to_snapshot (rode o build)")
		return

	var snapshot: Dictionary = sim.to_snapshot()
	var save: Dictionary = snapshot.get("save", {})
	if save.is_empty() or not snapshot.has("definitions"):
		_fail("to_snapshot devolveu snapshot incompleto")
		return

	print("[snapshot] day=%d cidades=%d construcoes=%d obras=%d produtos=%d industry_seeded=%s" % [
		int(save.get("day", 0)),
		Array(save.get("cities", [])).size(),
		Array(save.get("buildings", [])).size(),
		Array(save.get("construction_projects", [])).size(),
		Array(save.get("products", [])).size(),
		str(bool(save.get("industry_seeded", false)))])
	print("[snapshot] escrito %s (%d bytes)" % [_out, _file_size(_out)])


func _file_size(path: String) -> int:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return 0
	var size := file.get_length()
	file.close()
	return size
