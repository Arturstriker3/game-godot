extends RefCounted

## B6 (D-095) — HECTARES DE TERRA AGRÍCOLA E GINI FUNDIÁRIO POR REGIÃO.
##
## Fonte: `data/definitions/economy/land_property.json` (FAOSTAT "Land Use"
## item 00006610, ano 2023, já em hectares; Gini fundiário FAO LINEQ v1;
## `docs/data/land_property.md`). O arquivo é NACIONAL: o rateio nacional →
## estado é a REGRA DECLARADA no próprio arquivo
## (`state_distribution.rule`), aplicada aqui e não em dado novo:
##
##   ha_estado = ha_nacao × w_estado ÷ Σ w
##   w_estado  = area_km2(estado) × aptidao_agricola(estado)
##   aptidao   = média da aptidão agrícola (0..1) dos territórios/províncias
##               que compõem o estado
##   Σ w = 0   → w = area_km2
##   clamp: ha_estado ≤ 100 × area_km2 (1 km² = 100 ha: no máximo o estado
##          inteiro é área agrícola); o excedente volta aos estados NÃO
##          clampados, proporcional a w. A soma dos estados é o total nacional.
##
## Aptidão: o arquivo cita os territórios TRT de `agriculture.json`; o projeto
## já carrega a MESMA aptidão por PROVÍNCIA em `province_nature.json` (raster
## WorldClim no centróide, mais fino que o TRT). Usa-se essa fonte, ponderada
## pela área real da província (`province_areas.json`) — a semântica 0..1 é a
## mesma e nenhum número é inventado. Sem província com dado, aptidão 0 e o
## peso cai para a área (regra declarada).
##
## Gini fundiário: `land_gini` da nação quando observado; senão a mediana da
## região do jogo (se n ≥ 3); senão a mediana global — as duas lidas do bloco
## `regional_reference` do arquivo (`fallback_rule`). Nada é interpolado.
##
## Chamada (autoload e harnesses) DEPOIS de `initialize_world` e também depois
## de um load: o núcleo só semeia quando a região ainda não tem patrimônio
## (`property_ready` falso), então um save v23 mantém o estado gravado e um
## save v22 recebe o dado (alíquota 0 por padrão).

const LAND_PROPERTY_PATH := "res://data/definitions/economy/land_property.json"
const STATE_REGIONS_PATH := "res://data/definitions/population/state_regions.json"
const PROVINCE_NATURE_PATH := "res://data/definitions/natural/province_nature.json"
const PROVINCE_AREAS_PATH := "res://data/definitions/natural/province_areas.json"
const NATIONS_PATH := "res://data/definitions/scenario/nations.json"

## 1 km² = 100 ha: teto físico do rateio (todo o estado agrícola).
const HA_PER_KM2 := 100.0
## Região do jogo com menos estados observados que isso cai na mediana global
## (regra declarada no arquivo: `n >= 3`).
const MIN_REGION_SAMPLE := 3
## Passadas de redistribuição do excedente dos estados clampados (converge em
## 1–2; o teto evita laço infinito em dado degenerado).
const MAX_CLAMP_PASSES := 8

static var _document: Dictionary = {}
static var _states: Dictionary = {}          # state_id -> row (state_regions)
static var _cities: Dictionary = {}          # city_id -> {states, nation}
static var _province_nature: Dictionary = {}
static var _province_areas: Dictionary = {}
static var _nation_region: Dictionary = {}   # tag -> região do jogo
static var _region_land: Dictionary = {}     # city_id -> {land_ha, ...}
static var _nation_ha: Dictionary = {}       # tag -> hectares atribuídos
static var _loaded := false


static func _json(path: String) -> Dictionary:
	if not FileAccess.file_exists(path):
		push_warning("LandProperty: arquivo ausente " + path)
		return {}
	var parsed: Variant = JSON.parse_string(
			FileAccess.get_file_as_string(path))
	if typeof(parsed) != TYPE_DICTIONARY:
		push_warning("LandProperty: JSON invalido " + path)
		return {}
	return parsed


static func _load() -> void:
	if _loaded:
		return
	_loaded = true
	_document = _json(LAND_PROPERTY_PATH)
	var regions: Dictionary = _json(STATE_REGIONS_PATH)
	_states = regions.get("states", {})
	_cities = regions.get("cities", {})
	_province_nature = _json(PROVINCE_NATURE_PATH).get("provinces", {})
	_province_areas = _json(PROVINCE_AREAS_PATH).get("areas", {})
	var nations: Variant = JSON.parse_string(
			FileAccess.get_file_as_string(NATIONS_PATH))
	if typeof(nations) == TYPE_ARRAY:
		for row: Variant in nations:
			if typeof(row) != TYPE_DICTIONARY:
				continue
			var entry: Dictionary = row
			_nation_region[String(entry.get("id", ""))] = String(
				entry.get("region", ""))
	_build()


## Regiões com hectares: `{city_id: {land_ha, land_gini, gini_observed}}`.
static func region_land() -> Dictionary:
	_load()
	return _region_land


## Aplica ao núcleo (depois de `initialize_world` ou de um load; idempotente no
## núcleo). @returns cidades com hectares aplicados (0 = nada mudou).
static func apply(sim: Object) -> int:
	if sim == null or not sim.has_method("set_region_property"):
		return 0
	var data := region_land()
	if data.is_empty():
		return 0
	sim.set_region_property(data)
	return data.size()


## Auditoria (teste/relatório): hectares nacionais do arquivo × atribuídos por
## estado, cobertura e o Gini efetivo de cada nação.
static func audit() -> Dictionary:
	_load()
	var nations: Dictionary = _document.get("nations", {})
	return {
		"nations": nations.size(),
		"nations_with_land": _count_with_land(nations),
		"nations_attributed": _nation_ha.size(),
		"region_land_cities": _region_land.size(),
		"national_ha": _nation_ha.duplicate(true),
	}


static func _count_with_land(nations: Dictionary) -> int:
	var count := 0
	for tag: String in nations:
		var row: Dictionary = nations[tag]
		var ha: Variant = row.get("agricultural_land_ha", null)
		if ha is int or ha is float:
			count += 1
	return count


## Gini efetivo de uma nação: observado → mediana da região (n ≥ 3) → global.
## Devolve `{value, observed, source}`; `value = 0` quando não há nem fallback.
static func _effective_gini(nation: String) -> Dictionary:
	var nations: Dictionary = _document.get("nations", {})
	var row: Dictionary = nations.get(nation, {})
	var observed: Variant = row.get("land_gini", null)
	if observed is int or observed is float:
		return {"value": float(observed), "observed": true,
			"source": "observed"}
	var reference: Dictionary = _document.get("regional_reference", {})
	var regions: Dictionary = reference.get("regions", {})
	var region := String(_nation_region.get(nation, ""))
	var regional: Dictionary = regions.get(region, {})
	if not regional.is_empty() and int(regional.get("n", 0)) >= MIN_REGION_SAMPLE:
		return {"value": float(regional.get("median", 0.0)), "observed": false,
			"source": "region:" + region}
	var global_row: Dictionary = reference.get("global", {})
	if global_row.has("median_land_gini"):
		return {"value": float(global_row.get("median_land_gini", 0.0)),
			"observed": false, "source": "global"}
	return {"value": 0.0, "observed": false, "source": "none"}


## Aptidão agrícola (0..1) de um estado: média ponderada pela área das
## províncias que ele contém. Sem província com dado → 0 (o peso cai na área).
static func _state_aptitude(state_row: Dictionary) -> float:
	var provinces: Array = state_row.get("provinces", [])
	var weighted := 0.0
	var weight := 0.0
	for province: Variant in provinces:
		var data: Dictionary = _province_nature.get(str(int(province)), {})
		if data.is_empty():
			continue
		var area := float(_province_areas.get(str(int(province)), 0.0))
		if area <= 0.0:
			continue
		weighted += area * clampf(float(data.get("cultivable", 0.0)), 0.0, 1.0)
		weight += area
	if weight <= 0.0:
		return 0.0
	return weighted / weight


## Rateio nacional → estado (regra do arquivo). Devolve `{state_id: ha}`; vazio
## quando a nação não tem hectares (XKX) ou a soma dos pesos é zero.
static func _nation_state_ha(nation: String) -> Dictionary:
	var nations: Dictionary = _document.get("nations", {})
	var row: Dictionary = nations.get(nation, {})
	var raw: Variant = row.get("agricultural_land_ha", null)
	if not (raw is int or raw is float):
		return {}
	var total := float(raw)
	if total <= 0.0:
		return {}
	var ids: Array = []
	var weight: Dictionary = {}
	var weight_sum := 0.0
	var area: Dictionary = {}
	for state_id: String in _states:
		var state_row: Dictionary = _states[state_id]
		if String(state_row.get("nation", "")) != nation:
			continue
		var state_area := maxf(float(state_row.get("area_km2", 0.0)), 0.0)
		ids.append(state_id)
		area[state_id] = state_area
		weight[state_id] = state_area * _state_aptitude(state_row)
		weight_sum += float(weight[state_id])
	ids.sort()
	if ids.is_empty():
		return {}
	if weight_sum <= 0.0:
		# Regra declarada: sem aptidão, o peso é a área (nunca proporção igual).
		weight_sum = 0.0
		for state_id: String in ids:
			weight[state_id] = float(area[state_id])
			weight_sum += float(weight[state_id])
	if weight_sum <= 0.0:
		return {}
	var ha: Dictionary = {}
	var remaining := total
	var open_ids := ids.duplicate()
	for pass_index in MAX_CLAMP_PASSES:
		if open_ids.is_empty():
			break
		var open_weight := 0.0
		for state_id: String in open_ids:
			open_weight += float(weight[state_id])
		if open_weight <= 0.0:
			break
		var violations: Array = []
		for state_id: String in open_ids:
			var value := remaining * float(weight[state_id]) / open_weight
			var cap := HA_PER_KM2 * float(area[state_id])
			if cap > 0.0 and value > cap:
				violations.append(state_id)
			else:
				ha[state_id] = value
		if violations.is_empty():
			remaining = 0.0
			break
		for state_id: String in violations:
			var cap := HA_PER_KM2 * float(area[state_id])
			ha[state_id] = cap
			remaining = maxf(0.0, remaining - cap)
			open_ids.erase(state_id)
	# Todo estado no teto e ainda sobrando hectare: não cabe fisicamente. O
	# total atribuído fica abaixo do nacional e a diferença NÃO é inventada.
	var attributed := 0.0
	for state_id: String in ha:
		attributed += float(ha[state_id])
	return ha


static func _build() -> void:
	_region_land.clear()
	_nation_ha.clear()
	if _document.is_empty() or _states.is_empty():
		return
	var nation_order: Array = []
	for state_id: String in _states:
		var nation := String((_states[state_id] as Dictionary).get("nation", ""))
		if nation != "" and not nation_order.has(nation):
			nation_order.append(nation)
	nation_order.sort()
	var state_ha: Dictionary = {}
	for nation: String in nation_order:
		var by_state := _nation_state_ha(nation)
		if by_state.is_empty():
			continue
		var total := 0.0
		for state_id: String in by_state:
			state_ha[state_id] = float(by_state[state_id])
			total += float(by_state[state_id])
		_nation_ha[nation] = total
	for city_id: String in _cities:
		var entry: Dictionary = _cities[city_id]
		var land := 0.0
		var nation := ""
		for state_id: Variant in entry.get("states", []):
			var key := String(state_id)
			land += float(state_ha.get(key, 0.0))
			if nation == "":
				nation = String((_states.get(key, {}) as Dictionary).get(
					"nation", ""))
		if land <= 0.0 or nation == "":
			continue  # sem hectare = região sem B6 (ausência explícita)
		var gini := _effective_gini(nation)
		_region_land[city_id] = {
			"land_ha": land,
			"land_gini": float(gini.get("value", 0.0)),
			"gini_observed": bool(gini.get("observed", false)),
		}
