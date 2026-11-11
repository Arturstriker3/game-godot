extends SceneTree

## Teste do caminho real do jogo ao vivo.
##
## O autoload `Economy._ready()` faz `initialize_demo_world` + `demo_set_goods`
## + `GeographyService.apply_to_sim` SEM chamar `load_data()` antes — as funções
## de acesso por nação (`crop_suitability_nation`/`forest_cover_nation`/
## `deposits_for_nation`) também não carregavam os dados. Resultado (bug crítico,
## balance_review §3.1): aptidão 0 em todas as culturas e zero depósitos para as
## 8 nações jogáveis desde o dia 1.
##
## Este teste reproduz esse caminho exato (SEM `load_data()` prévio) e falha se
## alguma das 8 nações jogáveis ficar com aptidão 0 em todas as culturas ou sem
## depósitos quando os dados naturais têm depósitos para ela.
##
## Uso: godot --headless --path . --script res://tools/economy_live_path_test.gd

const GeographyServiceScript := preload("res://core/economy/geography_service.gd")

const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const PORTS_PATH := "res://data/definitions/scenario/ports.json"
const BUILDINGS_PATH := "res://data/definitions/scenario/buildings.json"
const GOODS_PATH := "res://data/definitions/economy/goods.json"
const DEPOSITS_PATH := "res://data/definitions/natural/deposits.json"
const NATURE_PATH := "res://data/definitions/natural/nature.json"

## As 8 nações jogáveis (mesma ordem de GeographyService.NATION_CODE).
const NATIONS := ["GBR", "FRA", "DEU", "NED", "BEL", "VEN", "SUR", "GUY"]

var _failures: Array[String] = []


func _init() -> void:
	_run()
	if _failures.is_empty():
		print("[live_path] OK: as 8 nações receberam aptidão e depósitos sem load_data() prévio.")
		quit(0)
	else:
		print("[live_path] FALHAS (%d):" % _failures.size())
		for failure in _failures:
			print(" - " + failure)
		quit(1)


func _load_array(path: String) -> Array:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_failures.append("não abriu " + path)
		return []
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data if typeof(data) == TYPE_ARRAY else []


func _load_dict(path: String) -> Dictionary:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_failures.append("não abriu " + path)
		return {}
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data if typeof(data) == TYPE_DICTIONARY else {}


func _run() -> void:
	var cities := _load_array(CITIES_PATH)
	var ports := _load_array(PORTS_PATH)
	var buildings := _load_array(BUILDINGS_PATH)
	var goods := _load_array(GOODS_PATH)
	if cities.is_empty():
		return

	# 1. Referência independente: quais nações têm depósito nos dados naturais.
	var nations_with_deposits := _nations_with_deposits()

	# 2. Mesmo pipeline do jogo ao vivo, SEM load_data() prévio.
	var sim = EconomySim.new()
	sim.initialize_demo_world(cities, ports, buildings)
	sim.demo_set_goods(goods)
	var geo = GeographyServiceScript.new()
	var applied: Dictionary = geo.apply_to_sim(sim, _cities_with_center(cities))

	if applied.is_empty():
		_failures.append("apply_to_sim retornou vazio (dados não carregaram)")
		return

	# 3. Verifica cada nação jogável.
	var crops := ["wheat", "rice", "maize", "potato", "soy", "cotton", "wood"]
	for tag in NATIONS:
		var nation_cities := _cities_of_nation(cities, tag)
		if nation_cities.is_empty():
			_failures.append("%s: nenhuma cidade no cenário" % tag)
			continue
		var best_crop := 0.0
		var reserve_kt := 0.0
		var deposit_names: Array = []
		for city in nation_cities:
			var city_applied: Dictionary = applied.get(String(city.get("id", "")), {})
			for crop in crops:
				best_crop = maxf(best_crop, float(city_applied.get(crop, 0.0)))
			for key in city_applied:
				if not crops.has(String(key)):
					reserve_kt += float(city_applied.get(key, 0.0))
					if float(city_applied.get(key, 0.0)) > 0.0:
						deposit_names.append(String(key))
		if best_crop <= 0.0:
			_failures.append("%s: aptidão 0 em todas as culturas (%.3f)" % [tag, best_crop])
		if nations_with_deposits.has(tag) and reserve_kt <= 0.0:
			_failures.append("%s: dados têm depósitos, mas nenhuma reserva foi declarada" % tag)
		print("[live_path] %-4s culturas_max=%.3f reserva=%.0f kt depósitos=%s" % [
			tag, best_crop, reserve_kt, ", ".join(deposit_names)])


## Cidades de uma nação (id + center no formato esperado por apply_to_sim).
func _cities_of_nation(cities: Array, tag: String) -> Array:
	var result: Array = []
	for city in cities:
		if String(city.get("nation", "")) == tag:
			result.append(city)
	return result


func _cities_with_center(cities: Array) -> Array:
	var result: Array = []
	for city in cities:
		result.append({
			"id": String(city.get("id", "")),
			"nation": String(city.get("nation", "")),
			"center": Vector2(float(city.get("x", 0.0)), float(city.get("y", 0.0))),
		})
	return result


## Conjunto de tags de nação cujos territórios têm depósitos nos dados naturais.
func _nations_with_deposits() -> Dictionary:
	var code_to_tag := {}
	for tag: String in GeographyServiceScript.NATION_CODE:
		code_to_tag[String(GeographyServiceScript.NATION_CODE[tag])] = String(tag)

	var nature: Dictionary = _load_dict(NATURE_PATH).get("territories", {})
	var territory_code := {}
	for territory_id: String in nature:
		territory_code[territory_id] = String(
			nature[territory_id].get("adm0_a3", ""))

	var deposits: Array = _load_dict(DEPOSITS_PATH).get("deposits", [])
	var tags := {}
	for deposit: Dictionary in deposits:
		var territory := String(deposit.get("territory", ""))
		if territory == "":
			continue
		var code: String = String(territory_code.get(territory, ""))
		if code_to_tag.has(code):
			tags[String(code_to_tag[code])] = true
	return tags


func _fail(message: String) -> void:
	_failures.append(message)
