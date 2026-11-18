extends SceneTree

## Posse mineral (regra A1): cada jazida pertence a UMA cidade do cenário —
## a mais próxima da MESMA nação e da MESMA massa de terra. Verifica:
## 1. partição: toda jazida de nação jogável tem exatamente 1 dono; nenhuma
##    jazida aparece em duas cidades e nenhuma se perde;
## 2. conservação: a reserva somada das cidades de uma nação é a reserva
##    nacional declarada por `deposits_for_nation`;
## 3. ficha: nenhuma cidade lista mineral sem jazida própria e o núcleo (C++)
##    recebe exatamente o conjunto de jazidas da cidade;
## 4. extratores: todo prédio mineral está numa cidade dona de jazida do produto
##    (nunca numa cidade que só tem o mineral via nação).
##
## Uso: godot --headless --path . --script res://tools/economy_deposit_ownership_test.gd

const GeoScript := preload("res://core/economy/geography_service.gd")
const Templates := preload("res://core/economy/ai/industry_templates.gd")

const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const PORTS_PATH := "res://data/definitions/scenario/ports.json"
const BUILDINGS_PATH := "res://data/definitions/scenario/buildings.json"
const GOODS_PATH := "res://data/definitions/economy/goods.json"
const DEPOSITS_PATH := "res://data/definitions/natural/deposits.json"
const NATURE_PATH := "res://data/definitions/natural/nature.json"
const PROVINCES_PATH := "res://map/map_data/atlas/provinces.json"

## Cidades que a regra de posse tinha deixado sem jazida própria e que o
## pipeline de posse corrigiu (docs/economy/deposit_lost_cities.json).
const TARGET_CITIES: Array[String] = [
	"QAT-DOH", "TLS-DIL", "TLS-MAL", "RWA-KIG", "SYR-ALE",
]
## Os 20 casos legítimos: a jazida perdida está mesmo no país vizinho e a
## cidade deve continuar sem jazida própria (não reabrir).
const LEGITIMATE_NEIGHBOR_CITIES: Array[String] = [
	"AFG-HER", "AFG-MAZ", "AGO-LUM", "BGD-KHU", "BHS-LUC", "CAN-INU",
	"CUW-WIL", "DJI-ALI", "DJI-DJI", "GNQ-BAT", "KGZ-OSH", "LSO-MAS",
	"MNG-DAR", "MNG-ULAA", "NED-ENS", "SAH-MEH", "SAH-TIC", "TCD-FAY",
	"VNM-HAI", "YEM-SAN",
]

## Dias de simulação com a IA ativa antes de auditar os extratores construídos.
const DAYS := 180

var _failures: Array[String] = []


func _init() -> void:
	var geo := GeoScript.new()
	if not geo.load_data():
		print("[deposit] FALHA: geografia não carregou")
		quit(1)
		return
	var cities: Array = _read_json_array(CITIES_PATH)
	var city_list := _city_list(cities)
	var nature: Dictionary = _read_json_dict(NATURE_PATH).get("territories", {})
	var deposits: Array = _read_json_dict(DEPOSITS_PATH).get("deposits", [])

	geo.assign_provinces(city_list)
	var assigned: Dictionary = geo.assign_deposits(city_list)

	_check_partition(geo, city_list, assigned, deposits, nature)
	_check_target_cities(geo)
	_check_conservation(geo, city_list)
	_print_table(geo, city_list)
	_check_extractors(geo, city_list, cities)

	if _failures.is_empty():
		print("[deposit] OK: cada jazida tem um único dono e todo extrator está na cidade dona.")
		quit(0)
	else:
		print("[deposit] FALHAS (%d):" % _failures.size())
		for failure in _failures:
			print(" - " + failure)
		quit(1)


# --- 1. Partição -------------------------------------------------------------

func _check_partition(geo, city_list: Array, assigned: Dictionary, deposits: Array,
		_nature: Dictionary) -> void:
	var tag_of_code := {}
	for tag: String in geo.nation_codes():
		tag_of_code[String(geo.nation_code(tag))] = String(tag)

	# Regra declarada no pipeline de posse (`tools/assign_deposit_owners.py`):
	# `owner` (ISO-3) quando a nação é jogável; sem ele, a província de
	# centróide mais próximo (regra histórica). Confere a amostra nos dois
	# caminhos — o campo `owner` tem de bater com o serviço e, quando falta, o
	# serviço tem de bater com a implementação bruta do centróide.
	var provinces: Array = _read_json_dict(PROVINCES_PATH).get("provinces", [])
	var checked := 0
	for deposit in deposits:
		if bool(deposit.get("facility", false)):
			continue
		if checked >= 200:
			break
		checked += 1
		var service_tag: String = geo.deposit_nation_tag(deposit)
		var owner := String(deposit.get("owner", ""))
		if owner != "" and tag_of_code.has(owner):
			if service_tag != String(tag_of_code[owner]):
				_fail("nação do depósito %s: owner=%s, serviço=%s" % [
					String(deposit.get("id", "")), owner, service_tag])
			continue
		var point := Vector2(float(deposit.get("px", 0.0)), float(deposit.get("py", 0.0)))
		var best_tag := ""
		var best_distance := INF
		for province in provinces:
			var distance := Vector2(float(province.get("x", 0.0)),
					float(province.get("y", 0.0))).distance_squared_to(point)
			if distance < best_distance:
				best_distance = distance
				best_tag = GeoScript.nation_for_province_tag(
						String(province.get("nation", "")))
		if best_tag != service_tag:
			_fail("nação do depósito %s: serviço=%s, bruto=%s" % [
				String(deposit.get("id", "")), service_tag, best_tag])

	# Esperado, lido do JSON cru com a regra do pipeline de posse.
	var expected_total := 0
	var expected_by_nation := {}
	for deposit in deposits:
		if bool(deposit.get("facility", false)):
			continue
		var tag: String = geo.deposit_nation_tag(deposit)
		if tag == "" or not tag_of_code.has(String(geo.nation_code(tag))):
			continue
		expected_total += 1
		expected_by_nation[tag] = int(expected_by_nation.get(tag, 0)) + 1

	# Cada jazida aparece no máximo uma vez, e o dono é da mesma nação.
	var seen := {}
	var got_total := 0
	for city_id in assigned:
		for deposit in assigned[city_id]:
			var key := String(deposit.get("id", ""))
			if key == "":
				key = "%s|%s|%s" % [str(deposit.get("px", 0.0)),
						str(deposit.get("py", 0.0)), String(deposit.get("resource", ""))]
			if seen.has(key):
				_fail("jazida duplicada: %s em %s e %s" % [key, seen[key], city_id])
				continue
			seen[key] = city_id
			got_total += 1
			var city_nation := _nation_of(city_list, String(city_id))
			var deposit_tag: String = geo.deposit_nation_tag(deposit)
			if deposit_tag != city_nation:
				_fail("jazida %s atribuída a %s (%s) mas pertence a %s" % [
					key, city_id, city_nation, deposit_tag])

	if got_total != expected_total:
		_fail("partição: %d jazidas atribuídas, esperado %d" % [got_total, expected_total])
	else:
		print("[deposit] partição: %d jazidas, cada uma com 1 dono (%d nações jogáveis)" % [
			got_total, expected_by_nation.size()])

	var got_by_nation := {}
	for tag in expected_by_nation:
		got_by_nation[tag] = 0
	for city_id in assigned:
		var tag := _nation_of(city_list, String(city_id))
		got_by_nation[tag] = int(got_by_nation.get(tag, 0)) + \
				(assigned[city_id] as Array).size()
	for tag in expected_by_nation:
		if int(got_by_nation.get(tag, 0)) != int(expected_by_nation[tag]):
			_fail("%s: %d jazidas com dono, esperado %d" % [
				tag, int(got_by_nation.get(tag, 0)), int(expected_by_nation[tag])])


# --- 2. Cidades-alvo e casos legítimos ---------------------------------------

## As 5 cidades que perderam a jazida na subdivisão têm de voltar a ter ≥ 1
## jazida própria; os 20 casos legítimos (jazida realmente no vizinho) têm de
## continuar sem jazida própria.
func _check_target_cities(geo) -> void:
	var missing: Array[String] = []
	for city_id in TARGET_CITIES:
		var own: Dictionary = geo.city_mineral_reserves(city_id)
		if own.is_empty():
			missing.append(city_id)
			continue
		var keys: Array = own.keys()
		keys.sort()
		var parts: Array = []
		for resource in keys:
			parts.append("%s×%d (%.1f kt)" % [resource,
				int(own[resource]["count"]), float(own[resource]["reserve_kt"])])
		print("[deposit] alvo %s: %s" % [city_id, ", ".join(parts)])
	if not missing.is_empty():
		_fail("cidades-alvo ainda sem jazida própria: %s" % ", ".join(missing))
	var reopened: Array[String] = []
	for city_id in LEGITIMATE_NEIGHBOR_CITIES:
		var own: Dictionary = geo.city_mineral_reserves(city_id)
		if not own.is_empty():
			reopened.append(city_id)
	if not reopened.is_empty():
		_fail("casos legítimos reabertos (não deveriam ter jazida própria): %s" % \
				", ".join(reopened))
	else:
		print("[deposit] casos legítimos: %d cidades continuam sem jazida própria" % \
				LEGITIMATE_NEIGHBOR_CITIES.size())


# --- 3. Conservação e ficha --------------------------------------------------

func _check_conservation(geo, city_list: Array) -> void:
	var by_nation := _cities_by_nation(city_list)
	for tag in by_nation:
		var code := String(geo.nation_code(tag))
		var national: Array = geo.deposits_for_nation(code)
		var national_reserve := {}
		for deposit in national:
			var resource := String(deposit.get("resource", ""))
			national_reserve[resource] = float(national_reserve.get(resource, 0.0)) \
					+ float(deposit.get("quantity_kt", 0.0))
		var city_reserve := {}
		for city in by_nation[tag]:
			var city_id := String(city["id"])
			var own: Dictionary = geo.city_mineral_reserves(city_id)
			# Ficha: cada mineral listado tem dono e reserva próprios.
			var owned: Array = geo.deposits_for_city(city_id)
			for deposit in owned:
				if String(geo.deposit_owner_city(deposit)) != city_id:
					_fail("%s: depósito próprio sem dono declarado" % city_id)
			for resource in own:
				if int(own[resource]["count"]) <= 0 \
						or float(own[resource]["reserve_kt"]) <= 0.0:
					_fail("%s lista %s sem jazida própria" % [city_id, resource])
				city_reserve[resource] = float(city_reserve.get(resource, 0.0)) \
						+ float(own[resource]["reserve_kt"])
		# Sem duplicação: soma das cidades == total nacional, por recurso.
		for resource in national_reserve:
			var expected := float(national_reserve[resource])
			var got := float(city_reserve.get(resource, 0.0))
			if absf(got - expected) > maxf(1.0, expected * 0.0001):
				_fail("%s/%s: reserva das cidades %.0f kt != nacional %.0f kt" % [
					tag, resource, got, expected])
		for resource in city_reserve:
			if not national_reserve.has(resource):
				_fail("%s/%s: cidade declara reserva que a nação não tem" % [tag, resource])


# --- 3. Extratores -----------------------------------------------------------

func _check_extractors(geo, city_list: Array, cities_raw: Array) -> void:
	var sim = EconomySim.new()
	# Mesmo caminho do jogo (`core/economy/economy.gd`): fazenda inicial +
	# minas semeadas pela produção real (D-086/D-087), então a IA.
	var buildings: Array = geo.resolve_initial_farms(
			_read_json_array(BUILDINGS_PATH), cities_raw)
	if geo.has_method("resolve_initial_mines"):
		buildings = geo.resolve_initial_mines(buildings, cities_raw)
	sim.initialize_world(cities_raw, _read_json_array(PORTS_PATH), buildings)
	sim.set_goods(_read_json_array(GOODS_PATH))
	geo.apply_to_sim(sim, city_list)
	# A5/D-075: a IA de investimento roda dentro do núcleo, no fim do dia. As
	# minas semeadas (D-086/D-087) e as construídas entram pelo mesmo caminho.
	sim.set_ai_enabled(true)
	for _day in DAYS:
		sim.tick_day()

	var status: Dictionary = sim.get_status()
	var mines := 0
	for city in status.get("cities", []):
		var city_id := String(city.get("id", ""))
		var heavy: Dictionary = sim.get_city_status(city_id)
		var own: Dictionary = geo.city_mineral_reserves(city_id)
		var declared: Dictionary = heavy.get("deposits", {})
		# O núcleo recebeu exatamente as jazidas da cidade (nem mais, nem menos).
		for resource in declared:
			if not own.has(resource):
				_fail("%s: núcleo tem depósito de %s que a cidade não possui" % [
					city_id, resource])
		for resource in own:
			if not declared.has(resource):
				_fail("%s: jazida própria de %s não chegou ao núcleo" % [city_id, resource])
		for building in heavy.get("buildings", []):
			var resource := String(building.get("resource", ""))
			if not Templates.MINERAL_RESOURCES.has(resource):
				continue
			mines += 1
			if not own.has(resource):
				_fail("%s: extrator de %s em cidade sem jazida própria" % [
					city_id, resource])
			elif float(declared.get(resource, {}).get("initial_kt", 0.0)) <= 0.0:
				_fail("%s: extrator de %s sem reserva declarada" % [city_id, resource])
	if mines == 0:
		_fail("nenhum extrator construído em %d dias — a checagem de localização ficou vazia" % DAYS)
	else:
		print("[deposit] extratores: %d prédio(s) mineral(is) após %d dias, todos na cidade dona" % [
			mines, DAYS])


# --- Tabela ------------------------------------------------------------------

func _print_table(geo, city_list: Array) -> void:
	var by_nation := _cities_by_nation(city_list)
	print("\n[deposit] minerais PRÓPRIOS por cidade (regra A1):")
	var total_none := 0
	for tag in by_nation:
		var rows: Array = []
		var none := 0
		for city in by_nation[tag]:
			var city_id := String(city["id"])
			var own: Dictionary = geo.city_mineral_reserves(city_id)
			if own.is_empty():
				none += 1
				rows.append("%s: (sem jazida)" % city_id)
				continue
			var keys: Array = own.keys()
			keys.sort()
			var parts: Array = []
			for key in keys:
				parts.append("%s×%d" % [key, int(own[key]["count"])])
			rows.append("%s: %s" % [city_id, ", ".join(parts)])
		total_none += none
		print("  %s (%d cidades, %d sem jazida)" % [tag, (by_nation[tag] as Array).size(), none])
		for row in rows:
			print("    " + String(row))
	print("  total: %d cidade(s) sem jazida própria" % total_none)


# --- Helpers -----------------------------------------------------------------

func _city_list(cities: Array) -> Array:
	var out: Array = []
	for city in cities:
		out.append({
			"id": String(city.get("id", "")),
			"nation": String(city.get("nation", "")),
			"center": Vector2(float(city.get("x", 0.0)), float(city.get("y", 0.0))),
			"landmass": String(city.get("landmass", "")),
		})
	return out


func _cities_by_nation(city_list: Array) -> Dictionary:
	var out := {}
	for city in city_list:
		var tag := String(city["nation"])
		if not out.has(tag):
			out[tag] = []
		(out[tag] as Array).append(city)
	return out


func _nation_of(city_list: Array, city_id: String) -> String:
	for city in city_list:
		if String(city["id"]) == city_id:
			return String(city["nation"])
	return ""


func _read_json_array(path: String) -> Array:
	var parsed = JSON.parse_string(FileAccess.get_file_as_string(path))
	if typeof(parsed) != TYPE_ARRAY:
		_fail("JSON inválido ou ausente: " + path)
		return []
	return parsed


func _read_json_dict(path: String) -> Dictionary:
	var parsed = JSON.parse_string(FileAccess.get_file_as_string(path))
	if typeof(parsed) != TYPE_DICTIONARY:
		_fail("JSON inválido ou ausente: " + path)
		return {}
	return parsed


func _fail(message: String) -> void:
	_failures.append(message)
