extends RefCounted
class_name WorldFacts

## Dados reais do cenário que a simulação ao vivo não publica no resumo:
## ficha da nação (governo, capital, região, PIB PPC), território (províncias e
## área), portos e jazidas minerais.
##
## Nada aqui é inventado nem calculado: os arquivos são lidos uma vez e
## guardados em cache. A tela de Política e o quadro de Estatísticas usam estes
## valores como *dados de cenário*; a economia ao vivo continua mandando nos
## números de produção, necessidade e estoque.

const PLAYER_NATION := "GBR"

const NATIONS_PATH := "res://data/definitions/scenario/nations.json"
const PORTS_PATH := "res://data/definitions/scenario/ports.json"
const PROVINCES_PATH := "res://map/map_data/atlas/provinces.json"
const PROVINCE_AREAS_PATH := "res://data/definitions/natural/province_areas.json"
const DEPOSITS_PATH := "res://data/definitions/natural/deposits.json"

## Ordem em que os grupos regionais aparecem no quadro comparativo.
const REGIONS := ["europe", "asia", "africa", "north_america", "south_america",
		"middle_east", "oceania"]

static var _nations: Dictionary = {}
static var _territory: Dictionary = {}
static var _deposits: Dictionary = {}
static var _world_reserve := 0.0
static var _nations_loaded := false
static var _territory_loaded := false
static var _deposits_loaded := false


## tag -> {id, name, flag, color, capital, currency, government, ai_profile,
## region, population, gdp_ppp_bn}. Copiado do nations.json do cenário.
static func nations() -> Dictionary:
	if _nations_loaded:
		return _nations
	_nations_loaded = true
	var data: Variant = _read_json(NATIONS_PATH)
	if typeof(data) != TYPE_ARRAY:
		return _nations
	for entry: Variant in data:
		if typeof(entry) != TYPE_DICTIONARY:
			continue
		var tag := String(entry.get("id", ""))
		if not tag.is_empty():
			_nations[tag] = entry
	return _nations


static func nation(tag: String) -> Dictionary:
	return nations().get(tag, {})


## tag -> {provinces, area_km2, ports}. Províncias e área vêm do atlas
## (`provinces.json` + `province_areas.json`), portos do cenário.
static func territory(tag: String) -> Dictionary:
	_load_territory()
	return _territory.get(tag, {"provinces": 0, "area_km2": 0.0, "ports": 0})


## tag -> {resource: recoverable_kt} — reserva restante das jazidas do cenário.
static func deposits(tag: String) -> Dictionary:
	_load_deposits()
	return _deposits.get(tag, {})


static func world_reserve_kt() -> float:
	_load_deposits()
	return _world_reserve


## Nomes de recursos na mesma ordem do extrato de jazidas (pt/en/es no I18n).
static func resource_key(resource: String) -> String:
	return "resource." + resource


# --- Carga -------------------------------------------------------------------

static func _load_territory() -> void:
	if _territory_loaded:
		return
	_territory_loaded = true
	var areas: Dictionary = {}
	var area_data: Variant = _read_json(PROVINCE_AREAS_PATH)
	if typeof(area_data) == TYPE_DICTIONARY:
		areas = area_data.get("areas", {})

	var province_data: Variant = _read_json(PROVINCES_PATH)
	if typeof(province_data) != TYPE_DICTIONARY:
		return
	for province: Variant in province_data.get("provinces", []):
		if typeof(province) != TYPE_DICTIONARY:
			continue
		var tag := String(province.get("nation", ""))
		if tag.is_empty():
			continue
		var bucket: Dictionary = _territory.get(tag, {
			"provinces": 0, "area_km2": 0.0, "ports": 0,
		})
		bucket["provinces"] = int(bucket["provinces"]) + 1
		var area := float(areas.get(str(int(province.get("id", 0))), 0.0))
		bucket["area_km2"] = float(bucket["area_km2"]) + area
		_territory[tag] = bucket

	var port_data: Variant = _read_json(PORTS_PATH)
	if typeof(port_data) == TYPE_ARRAY:
		for port: Variant in port_data:
			if typeof(port) != TYPE_DICTIONARY:
				continue
			var tag := String(port.get("nation", ""))
			if tag.is_empty():
				continue
			var bucket: Dictionary = _territory.get(tag, {
				"provinces": 0, "area_km2": 0.0, "ports": 0,
			})
			bucket["ports"] = int(bucket["ports"]) + 1
			_territory[tag] = bucket


static func _load_deposits() -> void:
	if _deposits_loaded:
		return
	_deposits_loaded = true
	var data: Variant = _read_json(DEPOSITS_PATH)
	if typeof(data) != TYPE_DICTIONARY:
		return
	for deposit: Variant in data.get("deposits", []):
		if typeof(deposit) != TYPE_DICTIONARY:
			continue
		var reserve := float(deposit.get("recoverable_kt", 0.0))
		if reserve <= 0.0:
			continue
		_world_reserve += reserve
		var owner := String(deposit.get("owner", ""))
		if owner.is_empty():
			continue
		var resource := String(deposit.get("resource", ""))
		if resource.is_empty():
			continue
		var bucket: Dictionary = _deposits.get(owner, {})
		bucket[resource] = float(bucket.get(resource, 0.0)) + reserve
		_deposits[owner] = bucket


static func _read_json(path: String) -> Variant:
	if not FileAccess.file_exists(path):
		push_warning("WorldFacts: arquivo ausente %s" % path)
		return null
	return JSON.parse_string(FileAccess.get_file_as_string(path))
