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

## Regra de água (igual a tools/build_resource_map.py): nada de recurso
## terrestre no mar; petróleo/gás offshore só até 20 células (~320 km) da
## costa — cobre pré-sal e Mar do Norte sem pontos no meio do oceano.
const COASTAL_RESOURCES := ["oil", "gas"]
const COASTAL_RADIUS_CELLS := 20
const MAP_W := 5000.0
const MAP_H := 2500.0

## Tag de nação do jogo -> código ISO-3 do Natural Earth (adm0_a3).
## Só a Holanda diverge (NED no jogo, NLD no Natural Earth).
const NATION_CODE := {
	"GBR": "GBR", "FRA": "FRA", "DEU": "DEU", "NED": "NLD",
	"BEL": "BEL", "VEN": "VEN", "SUR": "SUR", "GUY": "GUY",
}

## Culturas básicas da cesta alimentar (B3) e fator calórico (tonelada-
## equivalente de grão; fonte FAO em economy_sim.h). A fazenda inicial planta a
## cultura de maior aptidão × caloria — mais alimento por tonelada-equivalente
## na mesma área/trabalhadores.
const STAPLE_CROPS: Array[String] = ["wheat", "rice", "maize", "potato"]
const STAPLE_CALORIC := {"wheat": 1.0, "rice": 1.0, "maize": 1.0, "potato": 0.25}

## Demanda externa LIMITADA (B4): o teto de exportação de um bem bruto por
## cidade é a participação da nação no mercado mundial. Minerais: reserva
## esgotada em ~25 anos (o mesmo horizonte de `max_rate_kt_day` do deposits.json);
## piso de 1 t/d para depósitos pequenos de alto valor (ouro/diamante/urânio)
## não ficarem inertes. Renováveis (soja/algodão/madeira): base fixa × aptidão.
## Ordens de grandeza, não precisão — ver balance_review §14.
const DEPLETION_YEARS := 25.0
const RENEWABLE_EXPORT_BASE := 40.0  # t/dia a aptidão 1.0
const MIN_EXTERNAL_DEMAND := 1.0     # t/dia (piso)

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
	var best_id := _nearest_territory(center_px)
	_territory_cache[city_id] = best_id
	return best_id


## Território mais próximo de um ponto (sem cache por cidade).
func _nearest_territory(center_px: Vector2) -> String:
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
	return best_id


## Atributos naturais do território de um ponto (aptidão agrícola, fertilidade,
## cultivável, clima, solo, relevo, água). Usado pela ficha de recursos.
func territory_attributes(center_px: Vector2) -> Dictionary:
	if not load_data():
		return {}
	var territory := _nearest_territory(center_px)
	if territory == "":
		return {}
	var data: Dictionary = _nature.get(territory, {})
	var crops: Dictionary = _agriculture.get(territory, {})
	return {
		"territory": territory,
		"name_pt": String(data.get("name_pt", "")),
		"name_en": String(data.get("name_en", "")),
		"name_es": String(data.get("name_es", "")),
		"wheat": clampf(float(crops.get("wheat", 0.0)), 0.0, 1.0),
		"fertility": clampf(float(data.get("fertility", 0.0)), 0.0, 1.0),
		"cultivable": clampf(float(data.get("cultivable", 0.0)), 0.0, 1.0),
		"water": clampf(float(data.get("water", 0.0)), 0.0, 1.0),
		"climate": String(data.get("climate", "")),
		"soil_type": String(data.get("soil_type", "")),
		"relief": String(data.get("relief", "")),
	}


## Nome localizado (pt/en/es) de um território pelo id.
func territory_name(territory_id: String) -> Dictionary:
	if not load_data():
		return {}
	var data: Dictionary = _nature.get(territory_id, {})
	return {
		"name_pt": String(data.get("name_pt", "")),
		"name_en": String(data.get("name_en", "")),
		"name_es": String(data.get("name_es", "")),
	}


## Aptidão (0..1) de uma cultura no território da cidade.
func crop_suitability(city_id: String, center_px: Vector2, crop: String) -> float:
	var territory := territory_for(city_id, center_px)
	if territory == "":
		return 0.0
	var crops: Dictionary = _agriculture.get(territory, {})
	return clampf(float(crops.get(crop, 0.0)), 0.0, 1.0)


## Cobertura florestal (0..1) do território da cidade — a "aptidão" da madeira.
func forest_cover(city_id: String, center_px: Vector2) -> float:
	var territory := territory_for(city_id, center_px)
	if territory == "":
		return 0.0
	return clampf(float(_nature.get(territory, {}).get("forest_cover", 0.0)), 0.0, 1.0)


## Territórios (ids) cujo adm0_a3 corresponde ao código ISO-3 informado.
func _nation_territories(code: String) -> Array:
	var result: Array = []
	for territory_id in _nature:
		if String(_nature[territory_id].get("adm0_a3", "")) == code:
			result.append(String(territory_id))
	return result


## Depósitos de TODOS os territórios da nação (acesso nacional, não só o
## território mais próximo da capital).
func deposits_for_nation(code: String) -> Array:
	if not load_data():
		return []
	var result: Array = []
	for territory_id in _nation_territories(code):
		result.append_array(_deposits_by_territory.get(territory_id, []))
	return result


## Melhor aptidão da cultura entre os territórios da nação (a nação planta na
## sua melhor região).
func crop_suitability_nation(code: String, crop: String) -> float:
	if not load_data():
		return 0.0
	var best := 0.0
	for territory_id in _nation_territories(code):
		var crops: Dictionary = _agriculture.get(territory_id, {})
		best = maxf(best, clampf(float(crops.get(crop, 0.0)), 0.0, 1.0))
	return best


## Cultura básica de maior aptidão × fator calórico da nação. É o alimento que
## a fazenda inicial planta: maximiza toneladas-equivalentes de grão na mesma
## área. Sem dados (ou sem territórios), trigo (comportamento legado).
func best_staple_nation(code: String) -> String:
	if not load_data():
		return "wheat"
	var best := "wheat"
	var best_score := -1.0
	for crop in STAPLE_CROPS:
		var score := crop_suitability_nation(code, crop) \
			* float(STAPLE_CALORIC.get(crop, 0.0))
		if score > best_score:
			best_score = score
			best = crop
	return best


## Resolve a cultura de cada fazenda inicial. Mitigação da biestabilidade
## (§11/§15): troca o trigo SÓ onde ele é inviável (aptidão < 0,30). Cidades com
## trigo viável mantêm trigo — a troca indiscriminada (maior aptidão × caloria)
## tirava a DEU do mercado de trigo e derrubava a GBR para 4% (§11.3). Retorna
## cópia dos edifícios com o campo "resource" preenchido (o C++ mapeia o nome ao
## produto). Cidade sem nação conhecida mantém "wheat". A capacidade nominal
## (área/trabalhadores) não muda: a produção efetiva segue = capacidade ×
## aptidão da cultura escolhida.
func resolve_initial_farms(buildings: Array, cities: Array) -> Array:
	var nation_of := {}
	for city in cities:
		nation_of[String(city.get("id", ""))] = String(city.get("nation", ""))
	var resolved: Array = []
	for building in buildings:
		var entry: Dictionary = building.duplicate(true)
		var tag := String(nation_of.get(String(entry.get("city_id", "")), ""))
		var code := String(NATION_CODE.get(tag, ""))
		entry["resource"] = _initial_staple(code)
		resolved.append(entry)
	return resolved


## Cultura da fazenda inicial: trigo quando viável (aptidão ≥ 0,30); senão o
## básico de maior aptidão × caloria (arroz nos trópicos). Sem dados, trigo.
func _initial_staple(code: String) -> String:
	if code == "":
		return "wheat"
	if crop_suitability_nation(code, "wheat") >= 0.30:
		return "wheat"
	return best_staple_nation(code)


## Maior cobertura florestal entre os territórios da nação.
func forest_cover_nation(code: String) -> float:
	if not load_data():
		return 0.0
	var best := 0.0
	for territory_id in _nation_territories(code):
		best = maxf(best, clampf(
			float(_nature.get(territory_id, {}).get("forest_cover", 0.0)), 0.0, 1.0))
	return best


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


## Aplica a aptidão de cada cultura e os depósitos minerais de cada cidade ao
## núcleo econômico. As culturas renováveis entram como aptidão (0..1); os
## minerais entram como reserva agregada (demo_set_deposit) + aptidão 1. O C++
## ignora produtos que não conhece (ex.: carvão/cobre/gás/petróleo ficam só no
## mapa por enquanto).
## @param sim Instância de EconomySim inicializada no modo demo.
## @param cities Lista de dicionários com "id" e "center" (Vector2 em pixels).
## @returns Mapa cidade -> {crop: aptidão, mineral: reserva_kt}.
func apply_to_sim(sim: Object, cities: Array) -> Dictionary:
	# Garante os dados carregados mesmo quando o chamador não chama load_data()
	# antes (era o caminho real do jogo ao vivo, que declarava aptidão 0 e zero
	# depósitos para todas as nações). Idempotente; retorna vazio se falhar.
	if not load_data():
		return {}
	var applied := {}
	var crops := ["wheat", "rice", "maize", "potato", "soy", "cotton"]
	for city in cities:
		var city_id := String(city.get("id", ""))
		var center: Vector2 = city.get("center", Vector2.ZERO)
		var code := String(NATION_CODE.get(String(city.get("nation", "")), ""))
		var city_applied := {}
		for crop in crops:
			var suitability := crop_suitability_nation(code, crop) if code != "" \
					else crop_suitability(city_id, center, crop)
			sim.demo_set_suitability(city_id, crop, suitability)
			city_applied[crop] = suitability
		var wood := forest_cover_nation(code) if code != "" \
				else forest_cover(city_id, center)
		sim.demo_set_suitability(city_id, "wood", wood)
		city_applied["wood"] = wood
		# Renováveis não alimentares (soja/algodão/madeira): demanda externa
		# limitada = base × aptidão (aptidão já declarada acima/agora).
		for renewable in ["soy", "cotton", "wood"]:
			var renewable_ceiling := RENEWABLE_EXPORT_BASE \
					* float(city_applied.get(renewable, 0.0))
			if renewable_ceiling > 0.0:
				sim.demo_set_external_demand(city_id, renewable, renewable_ceiling)
		var deposits := deposits_for_nation(code) if code != "" \
				else deposits_for(city_id, center)
		var minerals := _aggregate_deposits(deposits)
		for resource in minerals:
			var deposit: Dictionary = minerals[resource]
			sim.demo_set_suitability(city_id, resource, 1.0)
			sim.demo_set_deposit(city_id, resource,
					deposit["reserve_kt"], deposit["quality"], deposit["depth_m"])
			city_applied[resource] = deposit["reserve_kt"]
			# Teto de exportação = reserva esgotada em ~25 anos, com piso.
			var ceiling := maxf(
				float(deposit["reserve_kt"]) * 1000.0 / (DEPLETION_YEARS * 365.0),
				MIN_EXTERNAL_DEMAND)
			sim.demo_set_external_demand(city_id, resource, ceiling)
		applied[city_id] = city_applied
	return applied


## Agrega depósitos por recurso: soma a reserva e faz média ponderada de
## qualidade/profundidade (peso = reserva). Usado para declarar a jazida única
## por cidade no núcleo econômico.
func _aggregate_deposits(deposits: Array) -> Dictionary:
	var totals := {}
	for deposit in deposits:
		var resource := String(deposit.get("resource", ""))
		var qty := float(deposit.get("quantity_kt", 0.0))
		if resource == "" or qty <= 0.0:
			continue
		if not totals.has(resource):
			totals[resource] = {"reserve_kt": 0.0, "quality_w": 0.0, "depth_w": 0.0}
		totals[resource]["reserve_kt"] += qty
		totals[resource]["quality_w"] += float(deposit.get("quality", 0.5)) * qty
		totals[resource]["depth_w"] += float(deposit.get("depth_m", 0.0)) * qty
	var out := {}
	for resource in totals:
		var agg: Dictionary = totals[resource]
		var total: float = agg["reserve_kt"]
		out[resource] = {
			"reserve_kt": round(total),
			"quality": snappedf(agg["quality_w"] / total, 0.001),
			"depth_m": int(round(agg["depth_w"] / total)),
		}
	return out


func _index_deposits(deposits: Dictionary) -> void:
	_deposits_by_territory.clear()
	for deposit in deposits.get("deposits", []):
		if bool(deposit.get("facility", false)):
			continue  # fundição/refinaria: não é depósito mineral
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
