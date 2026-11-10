extends RefCounted
class_name GeographyService

## Liga as cidades aos dados naturais reais gerados por
## tools/build_natural_geography.py. É a única fonte de aptidão agrícola e de
## depósitos disponível em runtime: nenhuma cidade deve carregar valores de
## geografia hardcoded na simulação.

const NATURE_PATH := "res://data/definitions/natural/nature.json"
const AGRICULTURE_PATH := "res://data/definitions/natural/agriculture.json"
const DEPOSITS_PATH := "res://data/definitions/natural/deposits.json"
const NAV_MASK_PATH := "res://map/map_data/nav_mask.png"

## Regra de água (igual a tools/build_resource_map.py): nada no mar; só
## petróleo/gás a até 2 células da costa (extração futura só no litoral).
const COASTAL_RESOURCES := ["oil", "gas"]
const COASTAL_RADIUS_CELLS := 2
const MAP_W := 5000.0
const MAP_H := 2500.0

var _nature: Dictionary = {}
var _agriculture: Dictionary = {}
var _deposits_by_territory: Dictionary = {}
var _deposits: Array = []
var _nav_image: Image
var _territory_cache: Dictionary = {}
var _loaded := false


## Carrega os JSON naturais. Idempotente.
func load_data() -> bool:
	if _loaded:
		return true

	_nature = _read_json(NATURE_PATH).get("territories", {})
	_agriculture = _read_json(AGRICULTURE_PATH).get("territories", {})
	var deposits_data := _read_json(DEPOSITS_PATH)
	_deposits = deposits_data.get("deposits", [])
	_index_deposits(deposits_data)
	_loaded = not _nature.is_empty()
	return _loaded


## Território do centro informado, pelo centróide mais próximo em pixels.
func territory_for(city_id: String, center_px: Vector2) -> String:
	if not load_data():
		return ""
	if _territory_cache.has(city_id):
		return String(_territory_cache[city_id])

	var best_id := ""
	var best_distance := INF
	for territory_id in _nature:
		var centroid = _nature[territory_id].get("centroid_px", [])
		if typeof(centroid) != TYPE_ARRAY or centroid.size() < 2:
			continue
		var point := Vector2(float(centroid[0]), float(centroid[1]))
		var distance := point.distance_squared_to(center_px)
		if distance < best_distance:
			best_distance = distance
			best_id = String(territory_id)

	_territory_cache[city_id] = best_id
	return best_id


## Aptidão (0..1) de uma cultura no território da cidade.
func crop_suitability(city_id: String, center_px: Vector2, crop: String) -> float:
	var territory := territory_for(city_id, center_px)
	if territory == "":
		return 0.0
	var crops: Dictionary = _agriculture.get(territory, {})
	return clampf(float(crops.get(crop, 0.0)), 0.0, 1.0)


## Depósitos minerais/energéticos do território da cidade.
func deposits_for(city_id: String, center_px: Vector2) -> Array:
	var territory := territory_for(city_id, center_px)
	var entries: Array = _deposits_by_territory.get(territory, [])
	return entries.duplicate(true)


## Depósitos dentro de um raio em pixels do centro da cidade. É o critério
## usado pelo mapa de recursos: o que está perto da cidade conta para ela,
## independentemente do centroide do território.
func deposits_near(center_px: Vector2, radius_px: float) -> Array:
	if not load_data():
		return []
	var result: Array = []
	var radius_sq := radius_px * radius_px
	for deposit in _deposits:
		var point := Vector2(float(deposit.get("px", 0.0)), float(deposit.get("py", 0.0)))
		if point.distance_squared_to(center_px) > radius_sq:
			continue
		if not _deposit_allowed(deposit):
			continue
		result.append(deposit)
	return result


## Terra pode tudo; água só petróleo/gás encostado na costa.
func _deposit_allowed(deposit: Dictionary) -> bool:
	if _nav_image == null:
		_nav_image = Image.load_from_file(NAV_MASK_PATH)
	if _nav_image == null:
		return true
	var x := int(float(deposit.get("px", 0.0)) / MAP_W * _nav_image.get_width())
	var y := int(float(deposit.get("py", 0.0)) / MAP_H * _nav_image.get_height())
	if x < 0 or y < 0 or x >= _nav_image.get_width() or y >= _nav_image.get_height():
		return false
	if _nav_image.get_pixel(x, y).r >= 0.5:
		return true
	if not COASTAL_RESOURCES.has(String(deposit.get("resource", ""))):
		return false
	for dy in range(-COASTAL_RADIUS_CELLS, COASTAL_RADIUS_CELLS + 1):
		for dx in range(-COASTAL_RADIUS_CELLS, COASTAL_RADIUS_CELLS + 1):
			var nx := x + dx
			var ny := y + dy
			if nx < 0 or ny < 0 or nx >= _nav_image.get_width() \
					or ny >= _nav_image.get_height():
				continue
			if _nav_image.get_pixel(nx, ny).r >= 0.4:
				return true
	return false


## Resumo legível do território (auditoria e painéis).
func territory_summary(city_id: String, center_px: Vector2) -> Dictionary:
	var territory := territory_for(city_id, center_px)
	if territory == "":
		return {}
	var data: Dictionary = _nature.get(territory, {})
	return {
		"territory": territory,
		"owner": data.get("owner", ""),
		"climate": data.get("climate", ""),
		"relief": data.get("relief", ""),
		"wheat": crop_suitability(city_id, center_px, "wheat"),
		"deposits": deposits_for(city_id, center_px).size(),
	}


## Aplica a aptidão de trigo de cada cidade ao núcleo econômico.
## @param sim Instância de EconomySim inicializada no modo demo.
## @param cities Lista de dicionários com "id" e "center" (Vector2 em pixels).
## @returns Mapa cidade -> aptidão aplicada.
func apply_to_sim(sim: Object, cities: Array) -> Dictionary:
	var applied := {}
	for city in cities:
		var city_id := String(city.get("id", ""))
		var center: Vector2 = city.get("center", Vector2.ZERO)
		var wheat := crop_suitability(city_id, center, "wheat")
		sim.demo_set_suitability(city_id, "wheat", wheat)
		applied[city_id] = wheat
	return applied


func _index_deposits(deposits: Dictionary) -> void:
	_deposits_by_territory.clear()
	for deposit in deposits.get("deposits", []):
		var territory := String(deposit.get("territory", ""))
		if territory == "":
			continue
		if not _deposits_by_territory.has(territory):
			_deposits_by_territory[territory] = []
		_deposits_by_territory[territory].append(deposit)


func _read_json(path: String) -> Dictionary:
	if not FileAccess.file_exists(path):
		push_warning("GeographyService: arquivo ausente " + path)
		return {}
	var parsed = JSON.parse_string(FileAccess.get_file_as_string(path))
	if typeof(parsed) != TYPE_DICTIONARY:
		push_warning("GeographyService: JSON inválido " + path)
		return {}
	return parsed
