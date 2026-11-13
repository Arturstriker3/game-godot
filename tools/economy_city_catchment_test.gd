extends SceneTree

## Teste da bacia de captação por cidade em PROVÍNCIAS admin-1 (agricultura e
## madeira pertencem à cidade mais próxima da mesma nação e mesma massa de terra
## — regra A1). A unidade é a província (atlas `provinces.json`, ~15,9 mil desde
## a Fase B), não o território natural (739) — este era grosso demais (França 11,
## Reino Unido 4, Bélgica 1) e deixava 9 das 33 cidades sem terra.
##
## Verifica:
##  (a) cada província de uma nação jogável é atribuída a ≥ 1 cidade e as
##      frações da província somam 1 (partição por área, sem perda/duplicação);
##  (b) a soma das áreas cultiváveis das cidades de uma nação = área cultivável
##      da nação (partição sem perda/duplicação);
##  (c) toda cidade jogável tem ≥ 1 província (nenhuma cidade sem terra);
##  (d) Marselha tem aptidão de trigo < Paris e > 0 (aptidão local);
##  (e) Caiena só fica com províncias da Guiana (lon < -30);
##  (f) a produção total de trigo da França após 365 dias não excede a
##      capacidade da área cultivável total (terra limitada).
##
## Uso: godot --headless --path . --script res://tools/economy_city_catchment_test.gd

const GeographyServiceScript := preload("res://core/economy/geography_service.gd")
const EconomicAIScript := preload("res://core/economy/ai/economic_ai.gd")

const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const PORTS_PATH := "res://data/definitions/scenario/ports.json"
const BUILDINGS_PATH := "res://data/definitions/scenario/buildings.json"
const GOODS_PATH := "res://data/definitions/economy/goods.json"
const PROVINCES_PATH := "res://map/map_data/atlas/provinces.json"

const DAYS := 365

var _failures: Array[String] = []
var _geo: Object = null


func _init() -> void:
	_run()
	if _failures.is_empty():
		print("[catchment] OK: bacia por cidade correta (partição, área, aptidão local, terra limitada).")
		quit(0)
	else:
		print("[catchment] FALHAS (%d):" % _failures.size())
		for failure in _failures:
			print(" - " + failure)
		quit(1)


func _fail(msg: String) -> void:
	_failures.append(msg)


func _load_array(path: String) -> Array:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("não abriu " + path)
		return []
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data if typeof(data) == TYPE_ARRAY else []


func _load_dict(path: String) -> Dictionary:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("não abriu " + path)
		return {}
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data if typeof(data) == TYPE_DICTIONARY else {}


func _cities_with_center(cities: Array) -> Array:
	var result: Array = []
	for city in cities:
		result.append({
			"id": String(city.get("id", "")),
			"nation": String(city.get("nation", "")),
			"center": Vector2(float(city.get("x", 0.0)), float(city.get("y", 0.0))),
		})
	return result


## Províncias jogáveis (id, nation, x, y) do atlas, indexadas por id.
func _playable_provinces() -> Dictionary:
	var raw: Dictionary = _load_dict(PROVINCES_PATH)
	var by_id := {}
	for record in raw.get("provinces", []):
		if typeof(record) != TYPE_DICTIONARY:
			continue
		var tag := String(record.get("nation", ""))
		if GeographyServiceScript.nation_for_province_tag(tag) == "":
			continue
		by_id[int(record.get("id", 0))] = record
	return by_id


func _run() -> void:
	var cities := _load_array(CITIES_PATH)
	if cities.is_empty():
		return
	var geo = GeographyServiceScript.new()
	_geo = geo
	var catchment: Dictionary = geo.assign_provinces(_cities_with_center(cities))
	if catchment.is_empty():
		_fail("assign_provinces retornou vazio")
		return

	_check_partition(catchment)
	_check_area_sum(catchment, geo, cities)
	_check_every_city_has_land(catchment, cities)
	_check_marseille_paris(geo)
	_check_cayenne(catchment)
	_check_france_wheat_cap(cities, geo)


## (a) Partição POR ÁREA: cada província de nação jogável aparece em ≥ 1 cidade
## (nunca perdida) e a soma das frações da província entre as cidades é 1
## (nunca duplicada). Uma província pode ser dividida apenas quando uma cidade
## ficaria sem terra (cidade cercada, ex. Kassel/Hesse) — ver
## `GeographyService._share_starved_provinces`.
func _check_partition(catchment: Dictionary) -> void:
	var playable := _playable_provinces()
	var nation_provinces := {}
	for pid in playable:
		var tag := String(playable[pid].get("nation", ""))
		nation_provinces[tag] = int(nation_provinces.get(tag, 0)) + 1

	var owners := {}
	var assigned_by_nation := {}
	for city_id in catchment:
		var seen_in_city := {}
		for pid_v in catchment[city_id]:
			var pid := int(pid_v)
			if seen_in_city.has(pid):
				_fail("província %d repetida na cidade %s" % [pid, String(city_id)])
			seen_in_city[pid] = true
			if not owners.has(pid):
				owners[pid] = []
				var tag := String(playable.get(pid, {}).get("nation", ""))
				assigned_by_nation[tag] = int(assigned_by_nation.get(tag, 0)) + 1
			(owners[pid] as Array).append(String(city_id))

	for pid in owners:
		var share_sum := 0.0
		for city_id in owners[pid]:
			share_sum += float(_geo.province_share(String(city_id), int(pid)))
		if playable.has(pid):
			if absf(share_sum - 1.0) > 0.001:
				_fail("província %d: soma das frações %.3f != 1 (área duplicada/perdida)" % [
					int(pid), share_sum])
		elif share_sum > 1.001:
			_fail("província doadora %d: frações somam %.3f (> 1)" % [int(pid), share_sum])

	for tag in nation_provinces:
		var expected := int(nation_provinces[tag])
		var assigned := int(assigned_by_nation.get(tag, 0))
		if assigned != expected:
			_fail("%s: %d províncias na nação, %d atribuídas (partição quebrada)" % [
				tag, expected, assigned])
	var shared := 0
	for pid in owners:
		if (owners[pid] as Array).size() > 1:
			shared += 1
	print("[catchment] (a) partição por área ok: %d províncias jogáveis em %d cidades (%d dividida(s))" % [
		owners.size(), catchment.size(), shared])


## (b) Conservação GLOBAL de área cultivável: a soma das áreas das cidades é a
## soma das províncias que alguma cidade recebeu (cada província entra uma vez,
## multiplicada pelas frações das cidades). Vale para as 66 nações de uma vez e
## cobre províncias doadoras (Vaticano) sem depender de lista fixa.
func _check_area_sum(catchment: Dictionary, geo: Object, cities: Array) -> void:
	var seen := {}
	var expected := 0.0
	for city_id in catchment:
		for pid_v in catchment[city_id]:
			var pid := int(pid_v)
			if seen.has(pid):
				continue
			seen[pid] = true
			expected += float(geo.province_cultivable_area_km2(pid))

	var city_sum := 0.0
	for city in cities:
		city_sum += float(geo.city_cultivable_area_km2(String(city.get("id", ""))))
	if absf(city_sum - expected) > maxf(1.0, 0.001 * expected):
		_fail("soma das áreas cultiváveis das cidades %.1f != províncias atribuídas %.1f km²" % [
			city_sum, expected])
	else:
		print("[catchment] (b) conservação global de área cultivável: %.1f km² em %d províncias" % [
			city_sum, seen.size()])


## (c) Toda cidade jogável tem ≥ 1 província (nenhuma cidade sem terra).
func _check_every_city_has_land(catchment: Dictionary, cities: Array) -> void:
	var no_land: Array[String] = []
	for city in cities:
		var city_id := String(city.get("id", ""))
		var tag := String(city.get("nation", ""))
		if not GeographyServiceScript.is_playable(tag):
			continue
		if (catchment.get(city_id, []) as Array).is_empty():
			no_land.append(city_id)
	if not no_land.is_empty():
		_fail("cidades sem terra: %s" % ", ".join(no_land))
	else:
		print("[catchment] (c) toda cidade jogável tem ≥ 1 província")


## (d) Marselha (bacia própria) tem aptidão de trigo < Paris e > 0.
func _check_marseille_paris(geo: Object) -> void:
	var marseille := float(geo.city_crop_suitability("FRA-MRS", "wheat"))
	var paris := float(geo.city_crop_suitability("FRA-PAR", "wheat"))
	if marseille <= 0.0:
		_fail("FRA-MRS trigo %.3f deveria ser > 0" % marseille)
	elif marseille >= paris:
		_fail("FRA-MRS trigo %.3f >= FRA-PAR %.3f (Marselha ainda planta como Paris)" % [
			marseille, paris])
	else:
		print("[catchment] (d) Marselha trigo %.3f < Paris %.3f (aptidão local)" % [
			marseille, paris])


## (e) Caiena só fica com províncias da Guiana (lon < -30), nunca da França
## continental.
func _check_cayenne(catchment: Dictionary) -> void:
	var playable := _playable_provinces()
	var pids: Array = catchment.get("FRA-CAY", [])
	for pid_v in pids:
		var record: Dictionary = playable.get(int(pid_v), {})
		var lon := float(record.get("x", 0.0)) / 5000.0 * 360.0 - 180.0
		if lon >= -30.0:
			_fail("Caiena herdou província fora da Guiana: %d (lon %.1f)" % [int(pid_v), lon])
	print("[catchment] (e) Caiena isolada: %d província(s) na Guiana (lon < -30)" % pids.size())


## (f) Produção total de trigo da França após 365 dias <= capacidade da área
## cultivável total (Σ área × cultivável × aptidão × produtividade FAO).
func _check_france_wheat_cap(cities: Array, geo: Object) -> void:
	var ports := _load_array(PORTS_PATH)
	var buildings := _load_array(BUILDINGS_PATH)
	var goods := _load_array(GOODS_PATH)

	# Capacidade física de trigo da França (t/dia), independente da simulação.
	var wheat_capacity := 0.0
	for city in cities:
		if String(city.get("nation", "")) == "FRA":
			var city_id := String(city.get("id", ""))
			wheat_capacity += float(geo.city_land_capacity_t_day(city_id, "wheat")) \
				* float(geo.city_crop_suitability(city_id, "wheat"))

	var sim = EconomySim.new()
	sim.initialize_demo_world(cities, ports, geo.resolve_initial_farms(buildings, cities))
	sim.demo_set_goods(goods)
	geo.apply_to_sim(sim, _cities_with_center(cities))
	var ai = EconomicAIScript.new()
	ai.setup(sim, geo)

	for day in DAYS:
		var status: Dictionary = sim.tick_demo()
		ai.run(int(status.get("day", 0)))

	var status: Dictionary = sim.get_demo_status()
	var products: Array = status.get("products", [])
	var wheat_index := int(products.find("wheat"))
	if wheat_index < 0:
		_fail("produto 'wheat' não encontrado no status")
		return
	var produced := 0.0
	for city in status.get("cities", []):
		if String(city.get("nation", "")) == "FRA":
			produced += float(city.get("markets", [])[wheat_index].get("supply", 0.0))

	print("[catchment] (f) trigo FRA produzido %.1f t/dia vs capacidade %.1f t/dia" % [
		produced, wheat_capacity])
	if produced > wheat_capacity + 1.0:
		_fail("França produz %.1f t/dia de trigo > capacidade %.1f (terra limitada violada)" % [
			produced, wheat_capacity])
