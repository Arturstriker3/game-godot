extends SceneTree

## Teste do caminho real do jogo ao vivo.
##
## O autoload `Economy._ready()` faz `initialize_world` + `set_goods`
## + `GeographyService.apply_to_sim` SEM chamar `load_data()` antes — as funções
## de acesso por nação (`crop_suitability_nation`/`forest_cover_nation`/
## `deposits_for_nation`) também não carregavam os dados. Resultado (bug crítico,
## balance_review §3.1): aptidão 0 em todas as culturas e zero depósitos para as
## 8 nações jogáveis desde o dia 1.
##
## Este teste reproduz esse caminho exato (SEM `load_data()` prévio) e falha se
## alguma nação jogável (todas as de nations.json) ficar com aptidão 0 em todas
## as culturas ou sem depósitos quando os dados naturais têm depósitos para ela.
##
## Uso: godot --headless --path . --script res://tools/economy_live_path_test.gd

const GeographyServiceScript := preload("res://core/economy/geography_service.gd")

const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const PORTS_PATH := "res://data/definitions/scenario/ports.json"
const BUILDINGS_PATH := "res://data/definitions/scenario/buildings.json"
const GOODS_PATH := "res://data/definitions/economy/goods.json"
const DEPOSITS_PATH := "res://data/definitions/natural/deposits.json"
const PROVINCE_NATURE_PATH := "res://data/definitions/natural/province_nature.json"

var _failures: Array[String] = []


func _init() -> void:
	_run()
	if _failures.is_empty():
		print("[live_path] OK: as %d nações receberam aptidão e depósitos sem load_data() prévio." % GeographyServiceScript.playable_tags().size())
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
	var geo = GeographyServiceScript.new()
	var nations_with_deposits := _nations_with_deposits(geo)

	# 2. Mesmo pipeline do jogo ao vivo, SEM load_data() prévio.
	var sim = EconomySim.new()
	sim.initialize_world(cities, ports, buildings)
	sim.set_goods(goods)
	var applied: Dictionary = geo.apply_to_sim(sim, _cities_with_center(cities))

	if applied.is_empty():
		_failures.append("apply_to_sim retornou vazio (dados não carregaram)")
		return

	# 3. Verifica cada nação jogável (todas as 66 do nations.json).
	var crops := ["wheat", "rice", "maize", "potato", "soy", "cotton", "wood"]
	# Aptidão 0 só é falha se a nação TEM províncias no dado natural. Ilhas
	# minúsculas (São Bartolomeu, Kiribati, Sint Maarten) não têm admin-1 no
	# atlas: a ausência é declarada, não é um bug de carregamento.
	var nature_nations := _nations_with_provinces()
	for tag: String in _playable_tags(cities):
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
			if nature_nations.has(tag):
				_failures.append("%s: aptidão 0 em todas as culturas (%.3f)" % [tag, best_crop])
			else:
				print("[live_path] %-4s sem províncias no dado natural (ausência declarada)" % tag)
		if nations_with_deposits.has(tag) and reserve_kt <= 0.0:
			_failures.append("%s: dados têm depósitos, mas nenhuma reserva foi declarada" % tag)
		print("[live_path] %-4s culturas_max=%.3f reserva=%.0f kt depósitos=%s" % [
			tag, best_crop, reserve_kt, ", ".join(deposit_names)])

	_check_no_duplication(applied, cities, geo)
	_check_main_minerals(applied)
	_check_overseas_isolation(applied, geo)


## (a) Nenhuma reserva contada 2×: a soma das reservas declaradas por nação
## deve bater com a soma dos depósitos da nação (dentro da tolerância de
## arredondamento por cidade — 1 kt por cidade-recurso).
func _check_no_duplication(applied: Dictionary, cities: Array, geo) -> void:
	var crops := ["wheat", "rice", "maize", "potato", "soy", "cotton", "wood"]
	var deposit_totals := _nation_deposit_totals(geo)
	for tag: String in _playable_tags(cities):
		if not deposit_totals.has(tag):
			continue
		var declared := 0.0
		for city in _cities_of_nation(cities, tag):
			var city_applied: Dictionary = applied.get(String(city.get("id", "")), {})
			for key in city_applied:
				if not crops.has(String(key)):
					declared += float(city_applied.get(key, 0.0))
		var expected: float = deposit_totals[tag]
		# Tolerância = arredondamento por cidade-recurso (0,5 kt cada) + margem
		# relativa de 0,1%. Duplicação (2×) excede em ordens de grandeza.
		var tolerance: float = maxf(2.0, 0.001 * expected)
		if absf(declared - expected) > tolerance:
			_failures.append("%s: reserva declarada %.0f != depósitos %.0f (duplicação/perda)" % [
				tag, declared, expected])
		else:
			print("[live_path] %-4s soma_reservas=%.0f kt == depósitos=%.0f kt (ok)" % [
				tag, declared, expected])


## Soma da quantidade (kt) dos depósitos de cada nação jogável nos dados. A
## nação de cada depósito vem do pipeline de posse (`owner`), a mesma regra do
## serviço, exposta em `deposit_nation_tag`.
func _nation_deposit_totals(geo) -> Dictionary:
	var deposits: Array = _load_dict(DEPOSITS_PATH).get("deposits", [])
	var totals := {}
	for deposit: Dictionary in deposits:
		if bool(deposit.get("facility", false)):
			continue
		var tag: String = geo.deposit_nation_tag(deposit)
		if tag == "":
			continue
		totals[tag] = float(totals.get(tag, 0.0)) + float(deposit.get("quantity_kt", 0.0))
	return totals


## (b) DEU/GBR/FRA mantêm seus minerais principais (carvão do Ruhr para a DEU,
## carvão da GBR e ferro/bauxita da FRA), mesmo sem o agregado nacional direto.
func _check_main_minerals(applied: Dictionary) -> void:
	var expected := {"DEU": ["coal"], "GBR": ["coal"], "FRA": ["iron", "bauxite"]}
	for tag: String in expected:
		for mineral: String in expected[tag]:
			var found := 0.0
			for city in _cities_of_nation(_load_array(CITIES_PATH), tag):
				found += float(applied.get(String(city.get("id", "")), {}).get(mineral, 0.0))
			if found <= 0.0:
				_failures.append("%s: mineral principal %s ausente após a atribuição" % [tag, mineral])


## (c) Isolamento ultramarino por massa de terra: cada cidade só declara os
## depósitos da SUA massa de terra (`landmass` de cities.json — o mesmo proxy de
## continente da regra A1 em `GeographyService._landmass_of`). A versão anterior
## separava "Guiana (lon < -30) x continente" e chamava todo o resto de
## continental: a Nova Caledônia (lon ≈ 165) caía nesse balde e a reserva de
## ferro/bauxita de Nouméa era cobrada de Paris. O jogo declara a reserva na
## cidade da MESMA massa (Nouméa); o recorte por longitude era defeito do teste.
func _check_overseas_isolation(applied: Dictionary, geo) -> void:
	var crops := ["wheat", "rice", "maize", "potato", "soy", "cotton", "wood"]
	var cities := _load_array(CITIES_PATH)
	# Massas de terra onde a FRA tem cidade: as únicas que podem declarar.
	var landmasses := {}
	for city in cities:
		if String(city.get("nation", "")) != "FRA":
			continue
		landmasses[String(city.get("landmass", ""))] = true
	# Esperado por massa: cada depósito da FRA vai para a massa da cidade FRA
	# mais próxima do ponto (regra A1 com fallback "cidade mais próxima da
	# nação"). Guiana, Córsega e Nova Caledônia nunca entram no continente.
	var expected := {}
	for deposit: Dictionary in _load_dict(DEPOSITS_PATH).get("deposits", []):
		if bool(deposit.get("facility", false)):
			continue
		if geo.deposit_nation_tag(deposit) != "FRA":
			continue
		var landmass := _landmass_of_nearest_city(cities, "FRA", deposit)
		if landmass == "":
			continue
		if not expected.has(landmass):
			expected[landmass] = {}
		var resource := String(deposit.get("resource", ""))
		var qty := float(deposit.get("quantity_kt", 0.0))
		expected[landmass][resource] = float(expected[landmass].get(resource, 0.0)) + qty
	# A França continental agora tem várias cidades: os depósitos nacionais são
	# divididos entre elas pela regra A1, então a soma por massa de terra agrega
	# todas as cidades daquela massa.
	var keys: Array = landmasses.keys()
	keys.sort()
	for landmass: String in keys:
		var declared := _sum_applied_by_landmass(applied, cities, "FRA", landmass)
		_check_landmass_side("FRA " + landmass, declared,
			expected.get(landmass, {}), crops)


## Massa de terra (campo `landmass`) da cidade `tag` mais próxima do depósito,
## em coordenadas do plano 5000×2500. Espelha `_assign_deposits`/A1: o depósito
## pertence à massa da cidade da nação mais próxima, nunca a um balde fixo.
func _landmass_of_nearest_city(cities: Array, tag: String, deposit: Dictionary) -> String:
	var px := float(deposit.get("px", 0.0))
	var py := float(deposit.get("py", 0.0))
	var best := ""
	var best_distance := INF
	for city in cities:
		if String(city.get("nation", "")) != tag:
			continue
		var dx := float(city.get("x", 0.0)) - px
		var dy := float(city.get("y", 0.0)) - py
		var distance := dx * dx + dy * dy
		if distance < best_distance:
			best_distance = distance
			best = String(city.get("landmass", ""))
	return best


## Soma as reservas minerais (chaves que não são culturas) de todas as cidades
## da nação em uma massa de terra. Espelha a regra A1 (um depósito -> uma
## cidade), agregado quando a nação tem mais de uma cidade por massa de terra.
func _sum_applied_by_landmass(applied: Dictionary, cities: Array, tag: String,
		landmass: String) -> Dictionary:
	var crops := ["wheat", "rice", "maize", "potato", "soy", "cotton", "wood"]
	var result := {}
	for city in cities:
		if String(city.get("nation", "")) != tag:
			continue
		if String(city.get("landmass", "")) != landmass:
			continue
		var city_applied: Dictionary = applied.get(String(city.get("id", "")), {})
		for key: String in city_applied:
			if crops.has(key):
				continue
			result[key] = float(result.get(key, 0.0)) + float(city_applied.get(key, 0.0))
	return result


## Verifica que a soma das cidades da massa de terra declarou exatamente os
## depósitos dela: (1) cada mineral bate com a reserva; (2) nenhum mineral de
## fora aparece. Tolera o arredondamento por cidade-recurso (0,5 kt cada).
func _check_landmass_side(label: String, declared_by_side: Dictionary,
		expected_totals: Dictionary, crops: Array) -> void:
	var minerals := {}
	for key: String in declared_by_side:
		if not crops.has(key):
			minerals[key] = float(declared_by_side[key])
	for mineral: String in expected_totals:
		var declared := float(minerals.get(mineral, 0.0))
		var expected := float(expected_totals[mineral])
		var tolerance := maxf(2.0, 0.001 * expected)
		if absf(declared - expected) > tolerance:
			_failures.append("%s %s: %.0f kt != %.0f kt (isolamento por landmass quebrado)" % [
				label, mineral, declared, expected])
	for mineral: String in minerals:
		if not expected_totals.has(mineral):
			_failures.append("%s tem %s que não existe na sua massa de terra" % [label, mineral])


## Nações presentes no cenário e registradas como jogáveis (ordem alfabética).
func _playable_tags(cities: Array) -> Array:
	var tags := {}
	for city in cities:
		var tag := String(city.get("nation", ""))
		if GeographyServiceScript.is_playable(tag):
			tags[tag] = true
	var out: Array = tags.keys()
	out.sort()
	return out


## Cidades de uma nação (id + center no formato esperado por apply_to_sim).
## Nações que têm ao menos uma província (admin-1) no dado natural fino.
func _nations_with_provinces() -> Dictionary:
	var provinces: Dictionary = _load_dict(PROVINCE_NATURE_PATH).get("provinces", {})
	var out := {}
	for province_id: String in provinces:
		var nation := String(provinces[province_id].get("nation", ""))
		if nation != "":
			out[nation] = true
	return out


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


## Conjunto de tags de nação cujos depósitos existem nos dados naturais, pela
## regra de posse declarada (`owner`, com fallback de centróide).
func _nations_with_deposits(geo) -> Dictionary:
	var deposits: Array = _load_dict(DEPOSITS_PATH).get("deposits", [])
	var tags := {}
	for deposit: Dictionary in deposits:
		if bool(deposit.get("facility", false)):
			continue
		var tag: String = geo.deposit_nation_tag(deposit)
		if tag != "":
			tags[tag] = true
	return tags


func _fail(message: String) -> void:
	_failures.append(message)
