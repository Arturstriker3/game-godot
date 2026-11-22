extends RefCounted

## B3 — dado de região da cidade (state_regions.json + urban_share_reference.json).
##
## Helper compartilhado pelo autoload (`Economy`) e pelos harnesses de teste:
## resolve o bloco `region` de cada cidade a partir de
## `data/definitions/population/state_regions.json` (4.226 estados, 834 cidades)
## e anexa ao dict da cidade ANTES de `initialize_world` — assim o snapshot/CLI
## leva o dado junto. Também expõe a urbanização nacional (World Bank
## SP.URB.TOTL.IN.ZS, 10 nações) usada no mix de emprego do interior.
##
## Contrato por cidade (`cities[city_id]`): `population_region`,
## `urban_population`, `region_population` e `states[]` (ids de estado). Cada id
## é resolvido em `states[state_id]` = {name, population_2026, area_km2, ...}.
## Nome da região = estado de MAIOR população (o "polo") — regra declarada.

const STATE_REGIONS_PATH := "res://data/definitions/population/state_regions.json"
const URBAN_SHARE_PATH := "res://data/definitions/population/urban_share_reference.json"

static var _state_regions: Dictionary = {}
static var _urban_share: Dictionary = {}
static var _loaded := false


static func _load() -> void:
	if _loaded:
		return
	_loaded = true
	_state_regions = _json(STATE_REGIONS_PATH)
	var urban: Dictionary = _json(URBAN_SHARE_PATH)
	_urban_share = urban.get("nations", {}) if not urban.is_empty() else {}


static func _json(path: String) -> Dictionary:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		push_error("RegionData: nao abriu " + path)
		return {}
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data if typeof(data) == TYPE_DICTIONARY else {}


## Urbanização nacional (0..1) ou -1 sem dado (mix neutro declarado).
static func urban_share(nation: String) -> float:
	_load()
	var entry: Dictionary = _urban_share.get(nation, {})
	if entry.is_empty():
		return -1.0
	var value: float = float(entry.get("urban_share", -1.0))
	return value if value >= 0.0 else -1.0


## Anexa o bloco `region` a cada dict de cidade (in-place). Cidade sem entrada
## no state_regions fica sem o bloco (ausência explícita; região inerte).
static func attach(cities: Array) -> void:
	_load()
	if _state_regions.is_empty():
		return
	var cities_data: Dictionary = _state_regions.get("cities", {})
	var states_data: Dictionary = _state_regions.get("states", {})
	for city: Variant in cities:
		if typeof(city) != TYPE_DICTIONARY:
			continue
		var entry: Dictionary = cities_data.get(String(city.get("id", "")), {})
		if entry.is_empty():
			continue
		var states := _resolve_states(entry.get("states", []), states_data)
		city["region"] = {
			"population_region": float(entry.get("population_region", 0.0)),
			"urban_population": float(entry.get("urban_population", 0.0)),
			"region_population": float(entry.get("region_population", 0.0)),
			"urban_share": urban_share(String(city.get("nation", ""))),
			"name": _primary_name(states),
			"states": states,
		}


static func _resolve_states(ids: Array, states_data: Dictionary) -> Array:
	var out: Array = []
	for sid: Variant in ids:
		var state: Dictionary = states_data.get(String(sid), {})
		if state.is_empty():
			continue
		out.append({
			"id": String(sid),
			"name": String(state.get("name", "")),
			"population": float(state.get("population_2026", 0.0)),
			"area_km2": float(state.get("area_km2", 0.0)),
		})
	return out


## Nome da região = estado de MAIOR população (o "polo"). Sem estados, vazio.
static func _primary_name(states: Array) -> String:
	var best := ""
	var best_pop := -1.0
	for st: Variant in states:
		if typeof(st) != TYPE_DICTIONARY:
			continue
		var pop: float = float(st.get("population", 0.0))
		if pop > best_pop:
			best_pop = pop
			best = String(st.get("name", ""))
	return best
