extends RefCounted
class_name GeographyService

## Liga as cidades aos dados naturais reais gerados por
## tools/build_natural_geography.py. É a única fonte de aptidão agrícola e de
## depósitos disponível em runtime: nenhuma cidade deve carregar valores de
## geografia hardcoded na simulação.

const NATURE_PATH := "res://data/definitions/natural/nature.json"
const AGRICULTURE_PATH := "res://data/definitions/natural/agriculture.json"
const DEPOSITS_PATH := "res://data/definitions/natural/deposits.json"
## Natureza/aptidão POR PROVÍNCIA (admin-1), gerada por
## tools/build_province_nature.py com WorldClim 2.1 no centróide de cada
## província. É a fonte fina: o território (739 regiões) é grosso e o seu
## centróide caía no deserto — Egito/Israel/Líbano ficavam com aptidão 0.
## Sem o arquivo, o serviço mantém o fallback por território.
const PROVINCE_NATURE_PATH := "res://data/definitions/natural/province_nature.json"
const NAV_MASK_PATH := "res://map/map_data/nav_mask.png"
const CITIES_PATH := "res://data/definitions/scenario/cities.json"
## Províncias admin-1 do atlas (Natural Earth), a unidade da bacia de captação
## agrícola. Cada província tem id global, nation (tag do jogo), country (ISO-3)
## e centróide em pixels 5000×2500.
const PROVINCES_PATH := "res://map/map_data/atlas/provinces.json"
## Grade espacial (px) para achar a província mais próxima de um ponto sem
## varrer as 4.529 a cada depósito. Célula ~900 km no equador.
const PROVINCE_GRID_CELL := 50.0

## Regra de água (igual a tools/build_resource_map.py): nada de recurso
## terrestre no mar; petróleo/gás offshore só até 20 células (~320 km) da
## costa — cobre pré-sal e Mar do Norte sem pontos no meio do oceano.
const COASTAL_RESOURCES := ["oil", "gas"]
const COASTAL_RADIUS_CELLS := 20
const MAP_W := 5000.0
const MAP_H := 2500.0

## Cenário: a lista de nações jogáveis é a fonte de verdade dos códigos. Não há
## mais uma tabela fixa de 8 tags — qualquer nação de `nations.json` participa
## da geografia (províncias, bacia e minérios).
const NATIONS_PATH := "res://data/definitions/scenario/nations.json"

## Tag do jogo -> código ISO-3 do Natural Earth (adm0_a3). A identidade cobre
## quase todas; só NED (NLD) e XKX (KOS) divergem. KOS não tem território na base
## natural atual (lacuna declarada), então XKX fica sem jazida própria.
const NATION_CODE_OVERRIDES := {"NED": "NLD", "XKX": "KOS"}
## Tag do jogo -> tag `nation` do atlas admin-1 (provinces.json). Só Kosovo
## diverge (XKX no jogo, KOS no atlas).
const PROVINCE_TAG_OVERRIDES := {"XKX": "KOS"}
## Fração mínima de uma província doada quando a nação NÃO tem nenhuma província
## própria (Vaticano é o único caso). A regra de fração normal divide pela
## distância; aqui o piso evita cidade sem terra e mantém a doação simbólica
## (0,1% de uma província italiana ≈ 17 km², compatível com um microestado).
const CITY_STATE_MIN_SHARE := 0.001

static var _nation_codes_cache: Dictionary = {}
static var _province_tag_by_nation: Dictionary = {}
static var _nation_by_province_tag: Dictionary = {}
static var _playable_cache: Dictionary = {}
static var _registry_ready := false
## tag do jogo -> {region, population, gdp_ppp_bn} de nations.json. Fonte da
## renda externa (PIB PPP per capita) aplicada ao núcleo.
static var _nation_rows_cache: Dictionary = {}
## tag -> {index, per_capita_ppp, estimated, region}. Calculado uma vez.
static var _nation_income_cache: Dictionary = {}
static var _nation_income_meta: Dictionary = {}
static var _nation_income_ready := false

## Renda externa: piso do índice (fração da média mundial). Sem piso, países
## pobres ficariam com ~0,04 da média e a cidade sem terra não importaria nada —
## fome total por falta de um dado, não por economia. 10% mantém a fragilidade
## (0,0012/pessoa/dia) sem zerar. O índice é renormalizado DEPOIS do piso para a
## média mundial ponderada por população continuar 1,0 (ver balance_review §20).
const NATION_INCOME_FLOOR := 0.10

## Culturas básicas da cesta alimentar (B3) e fator calórico (tonelada-
## equivalente de grão; fonte FAO/USDA em economy_sim.h). A fazenda inicial
## planta a cultura de maior aptidão × caloria — mais alimento por
## tonelada-equivalente na mesma área/trabalhadores.
const STAPLE_CROPS: Array[String] = ["wheat", "rice", "maize", "potato", "soy"]
const STAPLE_CALORIC := {"wheat": 1.0, "rice": 1.0, "maize": 1.0, "potato": 0.25, "soy": 1.3}

## Necessidade alimentar do modelo demo (t/dia por habitante). Espelha
## `kDemoNeedPerCapita` em `gdextension/src/economy_sim.cpp`; mantido explícito
## aqui porque a fazenda inicial é dimensionada em GDScript antes do sim rodar.
const FOOD_NEED_PER_CAPITA := 0.0003
## Margem comercial da fazenda inicial: a capacidade efetiva é 10% acima da
## necessidade local (produção vendável para regiões com déficit). Sem folga,
## nenhuma cidade teria excedente para exportar e uma nação sem terra não teria
## de quem comprar. O excedente só é produzido se houver demanda (fase 4b segue
## as vendas do dia anterior), então a folga não vira estoque parado.
const INITIAL_FARM_EXPORT_MARGIN := 1.10

## Área (km²) por território, pré-computada por tools/compute_territory_areas.py
## com o MESMO Voronoi equirretangular de tools/build_resource_map.py. É uma
## estimativa de ordem de grandeza (data_quality:"estimate_display").
const TERRITORY_AREAS_PATH := "res://data/definitions/natural/territory_areas.json"
## Área (km²) por província, pré-computada por tools/compute_province_areas.py
## com o MESMO método (Voronoi dos centróides sobre o nav_mask, equirretangular).
const PROVINCE_AREAS_PATH := "res://data/definitions/natural/province_areas.json"

## Produtividade média mundial (t/ha/ano) por cultura da cesta — médias FAO
## aproximadas (ordem de grandeza, não precisão): trigo 3,5; arroz (casca) 4,6;
## milho 5,8; batata (fresca) 21; soja 2,8. Convertidas para t/km²/dia
## (× 100 ha/km² ÷ 365 dias). É o TETO físico de produção de uma fazenda por
## km² de área cultivável. Documentado em docs/economy/balance_review.md §16.
const CROP_YIELD_T_PER_KM2_PER_DAY := {
	"wheat": 3.5 * 100.0 / 365.0,   # ~0,959 t/km²/dia
	"rice": 4.6 * 100.0 / 365.0,    # ~1,260 t/km²/dia
	"maize": 5.8 * 100.0 / 365.0,   # ~1,589 t/km²/dia
	"potato": 21.0 * 100.0 / 365.0, # ~5,753 t/km²/dia
	"soy": 2.8 * 100.0 / 365.0,     # ~0,767 t/km²/dia
}

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
var _city_landmass: Dictionary = {}
var _area_by_territory: Dictionary = {}
var _provinces: Array = []              # [{id, nation, country, x, y}] (admin-1)
var _area_by_province: Dictionary = {}  # province_id -> km²
var _province_territory: Dictionary = {}  # province_id -> territory_id (herança)
## province_id (int) -> {crops, cultivable, forest_cover, ...} do
## province_nature.json. Ausente = província sem raster (centróide em água):
## cai no fallback do território, declarado em vez de inventado.
var _province_nature: Dictionary = {}
## ISO-3 da nação -> [province_id] (aptidão nacional = melhor província).
var _province_by_code: Dictionary = {}
## Grade espacial das províncias para `_nearest_province()` (depósitos).
var _province_grid: Dictionary = {}     # Vector2i -> [province_id]
var _province_center: Dictionary = {}   # province_id -> Vector2 px
var _province_tag: Dictionary = {}      # province_id -> tag do jogo
var _catchment: Dictionary = {}  # city_id -> [province_id] (atribuição A1)
## city_id -> {province_id: fração}. Ausente = 1.0 (província inteira). Só é
## fracionária na divisão de uma província entre uma cidade cercada e a vizinha
## da mesma nação (ver `_share_starved_provinces`).
var _province_share: Dictionary = {}
## Regra A1 dos minerais: cada jazida pertence a UMA cidade (mesma nação e
## mesma massa de terra). Preenchido por `assign_deposits` e reutilizado pela
## legenda da cidade, pelo painel e pelo mapa — todos leem o MESMO dono.
var _deposits_by_city: Dictionary = {}   # city_id -> [depósito]
var _deposit_owner_key: Dictionary = {}  # "px|py|recurso" -> city_id
var _assign_signature := ""
## Cidades do cenário (cities.json) normalizadas — usadas quando o chamador não
## passa a lista de cidades (legenda/painel/mapa).
var _roster: Array = []
var _roster_ready := false
var _loaded := false


# --- Registro de nações (cenário, não hardcoded) ---------------------------

## Carrega `nations.json` uma vez e monta os dois mapas de código:
## tag do jogo -> ISO-3 natural e tag do jogo -> tag de província do atlas.
## Sem o arquivo, cai na lista histórica de 8 tags (nunca quebra o boot).
static func _load_registry() -> void:
	if _registry_ready:
		return
	_registry_ready = true
	_nation_codes_cache.clear()
	_province_tag_by_nation.clear()
	_nation_by_province_tag.clear()
	_playable_cache.clear()
	_nation_rows_cache.clear()
	_nation_income_cache.clear()
	_nation_income_meta.clear()
	_nation_income_ready = false
	var parsed = JSON.parse_string(FileAccess.get_file_as_string(NATIONS_PATH))
	var tags: Array = []
	if typeof(parsed) == TYPE_ARRAY:
		for nation in parsed:
			if typeof(nation) == TYPE_DICTIONARY and String(nation.get("id", "")) != "":
				tags.append(String(nation["id"]))
				_nation_rows_cache[String(nation["id"])] = {
					"region": String(nation.get("region", "")),
					"population": float(nation.get("population", 0.0)),
					"gdp_ppp_bn": float(nation.get("gdp_ppp_bn", 0.0)),
				}
	if tags.is_empty():
		tags = ["GBR", "FRA", "DEU", "NED", "BEL", "VEN", "SUR", "GUY"]
	for tag: String in tags:
		var code := String(NATION_CODE_OVERRIDES.get(tag, tag))
		var province_tag := String(PROVINCE_TAG_OVERRIDES.get(tag, tag))
		_nation_codes_cache[tag] = code
		_province_tag_by_nation[tag] = province_tag
		_nation_by_province_tag[province_tag] = tag
		_playable_cache[tag] = true


## tag do jogo -> código ISO-3 do Natural Earth ("" se não jogável).
static func nation_code(tag: String) -> String:
	_load_registry()
	return String(_nation_codes_cache.get(tag, ""))


## tag do jogo -> tag `nation` usada pelas províncias do atlas.
static func province_tag(tag: String) -> String:
	_load_registry()
	return String(_province_tag_by_nation.get(tag, ""))


## tag de província do atlas -> tag do jogo ("" se não jogável).
static func nation_for_province_tag(province_tag: String) -> String:
	_load_registry()
	return String(_nation_by_province_tag.get(province_tag, ""))


## Mapa tag do jogo -> ISO-3 natural (cópia; não mutar).
func nation_codes() -> Dictionary:
	_load_registry()
	return _nation_codes_cache.duplicate()


## Todas as nações do cenário, em ordem alfabética de tag.
static func playable_tags() -> Array:
	_load_registry()
	var tags: Array = _playable_cache.keys()
	tags.sort()
	return tags


static func is_playable(tag: String) -> bool:
	_load_registry()
	return _playable_cache.has(tag)


# --- Renda externa por PIB PPP per capita (balance_review §20) --------------

## Mediana de uma lista de floats (cópia ordenada; lista vazia -> 0.0).
static func _median(values: Array) -> float:
	if values.is_empty():
		return 0.0
	var sorted := values.duplicate()
	sorted.sort()
	var count := sorted.size()
	if count % 2 == 1:
		return float(sorted[count / 2])
	return (float(sorted[count / 2 - 1]) + float(sorted[count / 2])) * 0.5


## Calcula, uma vez, o índice de renda externa de cada nação:
##  1. PIB PPP per capita real = gdp_ppp_bn × 1e9 / population (nations.json);
##  2. nação sem PIB/população -> MEDIANA DA SUA REGIÃO, marcada `estimated`
##     (sem mediana regional -> mediana mundial);
##  3. índice = per capita / média mundial ponderada por população, com PISO
##     NATION_INCOME_FLOOR, renormalizado até a média ponderada voltar a 1,0.
## A média ponderada 1,0 preserva a escala do núcleo (0,012/pessoa/dia).
## Não inventa renda: quem não tem dado é declarado estimado pela região.
static func _build_nation_income() -> void:
	_load_registry()
	if _nation_income_ready:
		return
	_nation_income_ready = true
	var per_capita := {}
	var region_values := {}
	for tag: String in _nation_rows_cache:
		var row: Dictionary = _nation_rows_cache[tag]
		var gdp := float(row.get("gdp_ppp_bn", 0.0)) * 1_000_000_000.0
		var population := float(row.get("population", 0.0))
		if gdp <= 0.0 or population <= 0.0:
			continue
		var value := gdp / population
		per_capita[tag] = value
		var region := String(row.get("region", ""))
		if not region_values.has(region):
			region_values[region] = []
		region_values[region].append(value)
	var region_median := {}
	for region: String in region_values:
		region_median[region] = _median(region_values[region])
	var world_median := _median(per_capita.values())
	if world_median <= 0.0:
		# Sem nations.json válido: não declara renda nenhuma (índice neutro).
		_nation_income_meta = {"available": false, "reason": "nations.json sem PIB"}
		return
	# Média mundial ponderada pela população das nações com dado.
	var weight_total := 0.0
	var weighted_sum := 0.0
	for tag: String in per_capita:
		var population := float(_nation_rows_cache[tag].get("population", 0.0))
		weight_total += population
		weighted_sum += per_capita[tag] * population
	if weight_total <= 0.0:
		_nation_income_meta = {"available": false, "reason": "população ausente"}
		return
	var world_mean := weighted_sum / weight_total
	var raw := {}
	var estimated: Array = []
	for tag: String in _nation_rows_cache:
		if per_capita.has(tag):
			raw[tag] = per_capita[tag]
		else:
			estimated.append(tag)
			var region := String(_nation_rows_cache[tag].get("region", ""))
			raw[tag] = float(region_median.get(region, world_median))
	# Índice com piso + renormalização iterativa (o piso muda a média).
	var index := {}
	for tag: String in raw:
		index[tag] = maxf(float(raw[tag]) / world_mean, NATION_INCOME_FLOOR)
	var total_weight := 0.0
	for tag: String in _nation_rows_cache:
		total_weight += float(_nation_rows_cache[tag].get("population", 0.0))
	if total_weight <= 0.0:
		total_weight = 1.0
	for _pass in 8:
		var mean := 0.0
		for tag: String in index:
			mean += float(index[tag]) \
					* float(_nation_rows_cache[tag].get("population", 0.0))
		mean /= total_weight
		if mean <= 0.0 or absf(mean - 1.0) < 1e-9:
			break
		for tag: String in index:
			index[tag] = float(index[tag]) / mean
	var floored: Array = []
	_nation_income_cache.clear()
	for tag: String in index:
		var value := float(index[tag])
		var is_floored := value <= NATION_INCOME_FLOOR * 1.02
		if is_floored:
			floored.append(tag)
		_nation_income_cache[tag] = {
			"index": value,
			"per_capita_ppp": float(raw[tag]),
			"estimated": estimated.has(tag),
			"region": String(_nation_rows_cache[tag].get("region", "")),
			"floored": is_floored,
		}
	estimated.sort()
	floored.sort()
	_nation_income_meta = {
		"available": true,
		"world_mean_ppp_per_capita": world_mean,
		"world_median_ppp_per_capita": world_median,
		"nations": index.size(),
		"nations_with_data": per_capita.size(),
		"estimated_nations": estimated,
		"floored_nations": floored,
		"floor_index": NATION_INCOME_FLOOR,
		"region_median_ppp_per_capita": region_median,
	}


## Perfil completo da renda externa (auditoria e testes). `by_nation` tem, por
## tag, o índice, o PIB PPP per capita de origem, a região e as marcas
## `estimated` (PIB ausente -> mediana regional) e `floored` (piso aplicado).
func nation_income_profile() -> Dictionary:
	_build_nation_income()
	return {
		"meta": _nation_income_meta.duplicate(true),
		"by_nation": _nation_income_cache.duplicate(true),
	}


## Índice de renda externa de uma nação (1,0 = média mundial; 0,0 = sem dado e
## sem registro). Cidade sem nação conhecida recebe 1,0 (comportamento legado do
## cenário sintético, nunca 0 por acidente).
func external_income_index(tag: String) -> float:
	_build_nation_income()
	var entry: Dictionary = _nation_income_cache.get(tag, {})
	return float(entry.get("index", 1.0))


## Carrega os JSON naturais. Idempotente.
func load_data() -> bool:
	if _loaded:
		return true

	_nature = _read_json(NATURE_PATH).get("territories", {})
	_agriculture = _read_json(AGRICULTURE_PATH).get("territories", {})
	var deposits_data := _read_json(DEPOSITS_PATH)
	_deposits = deposits_data.get("deposits", [])
	_index_deposits(deposits_data)
	_city_landmass = _load_city_landmasses()
	_area_by_territory = _read_json(TERRITORY_AREAS_PATH).get("areas", {})
	_load_provinces()
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


## Província cujo centróide está mais próximo de um ponto (grade espacial).
## Usada para dizer a NAÇÃO de um depósito: o dado do MRDS é o ponto real, e a
## província admin-1 é a menor unidade com nacionalidade no projeto. Empate fica
## com o menor id (determinístico). 0 = nenhuma (dados não carregados).
func _nearest_province(center_px: Vector2) -> int:
	if _province_grid.is_empty():
		return 0
	var gx := int(floor(center_px.x / PROVINCE_GRID_CELL))
	var gy := int(floor(center_px.y / PROVINCE_GRID_CELL))
	var best_pid := 0
	var best_distance := INF
	var max_ring := int(ceil(MAP_W / PROVINCE_GRID_CELL))
	for ring in range(0, max_ring + 1):
		for dy in range(-ring, ring + 1):
			for dx in range(-ring, ring + 1):
				if maxi(absi(dx), absi(dy)) != ring:
					continue
				var cell: Variant = _province_grid.get(Vector2i(gx + dx, gy + dy))
				if cell == null:
					continue
				for province_id: int in cell:
					var point: Vector2 = _province_center.get(province_id, Vector2.ZERO)
					var distance := point.distance_squared_to(center_px)
					if distance < best_distance \
							or (is_equal_approx(distance, best_distance) and province_id < best_pid):
						best_distance = distance
						best_pid = province_id
		# Um anel k só pode conter ponto mais próximo se k*CELL < distância atual.
		if best_pid != 0 and sqrt(best_distance) <= ring * PROVINCE_GRID_CELL:
			break
	return best_pid


## Tag do jogo da nação dona de um ponto no mapa, pela província mais próxima.
## É a regra de posse mineral: cada depósito pertence à nação da província onde
## o ponto real do MRDS cai, não ao território natural (739 regiões grossas).
func nation_tag_at(center_px: Vector2) -> String:
	if not load_data():
		return ""
	return String(_province_tag.get(_nearest_province(center_px), ""))


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
## território mais próximo da capital). A nação de cada depósito vem da
## província mais próxima do ponto real (regra de `nation_tag_at`).
func deposits_for_nation(code: String) -> Array:
	if not load_data():
		return []
	var result: Array = []
	for deposit in _deposits:
		if bool(deposit.get("facility", false)):
			continue
		var point := Vector2(float(deposit.get("px", 0.0)), float(deposit.get("py", 0.0)))
		var tag := nation_tag_at(point)
		if tag != "" and nation_code(tag) == code:
			result.append(deposit)
	return result


## Melhor aptidão da cultura entre as províncias da nação (a nação planta na
## sua melhor região). A fonte fina é province_nature.json: cada província tem
## a própria aptidão medida. Sem ela, cai no melhor território (legado).
func crop_suitability_nation(code: String, crop: String) -> float:
	if not load_data():
		return 0.0
	var best := 0.0
	var found := false
	for province_id in _province_by_code.get(code, []):
		var province_data: Dictionary = _province_nature.get(str(province_id), {})
		if province_data.is_empty():
			continue
		found = true
		var crops: Dictionary = province_data.get("crops", {})
		best = maxf(best, clampf(float(crops.get(crop, 0.0)), 0.0, 1.0))
	if found:
		return best
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
## Cópia dos edifícios com o campo "resource" preenchido (o C++ mapeia o nome ao
## produto). Cidade sem nação conhecida mantém "wheat". Terra limitada e rateio
## NACIONAL: cada cidade dimensiona a fazenda inicial para a PRÓPRIA necessidade
## na aptidão real da sua bacia; o excedente nacional (necessidade × margem) é
## rateado pela terra ociosa de quem tem folga. Cidade urbana sem terra não
## recebe meta própria — ela compra da região, como São Paulo compra do
## interior. Quando a soma das bacias não basta, o que falta é déficit real e
## vira importação (§19). `aptitude` vai no dicionário para o núcleo dimensionar
## folha/opex pela fração apta da terra (não pela capacidade nominal inteira).
func resolve_initial_farms(buildings: Array, cities: Array) -> Array:
	assign_provinces(cities)
	var nation_of := {}
	var population_of := {}
	for city in cities:
		var city_id := String(city.get("id", ""))
		nation_of[city_id] = String(city.get("nation", ""))
		population_of[city_id] = float(city.get("population", 0.0))
	# 1ª passada: cultura, aptidão e capacidade física de cada fazenda.
	var entries: Array = []
	var land_eff_by_city := {}
	var need_by_nation := {}
	var land_eff_by_nation := {}
	var self_by_nation := {}
	for building in buildings:
		var entry: Dictionary = building.duplicate(true)
		var city_id := String(entry.get("city_id", ""))
		var tag := String(nation_of.get(city_id, ""))
		var code := nation_code(tag)
		var resource := _initial_staple(code, city_id)
		entry["resource"] = resource
		var aptitude := city_crop_suitability(city_id, resource) if code != "" else 1.0
		aptitude = clampf(aptitude, 0.0, 1.0)
		entry["aptitude"] = aptitude
		var land_cap := 0.0
		if code != "":
			land_cap = city_land_capacity_t_day(city_id, resource)
		var land_eff := land_cap * aptitude
		land_eff_by_city[city_id] = land_eff
		if code != "":
			var need := float(population_of.get(city_id, 0.0)) * FOOD_NEED_PER_CAPITA
			need_by_nation[tag] = float(need_by_nation.get(tag, 0.0)) + need
			land_eff_by_nation[tag] = float(land_eff_by_nation.get(tag, 0.0)) + land_eff
			# Terra que a própria cidade usa para se alimentar (a menor entre a
			# necessidade e a sua capacidade física).
			self_by_nation[tag] = float(self_by_nation.get(tag, 0.0)) + minf(need, land_eff)
		entries.append({"entry": entry, "city_id": city_id, "tag": tag, "code": code,
			"resource": resource, "aptitude": aptitude, "land_cap": land_cap,
			"land_eff": land_eff,
			"need": float(population_of.get(city_id, 0.0)) * FOOD_NEED_PER_CAPITA})
	# 2ª passada: a cidade primeiro se alimenta da própria bacia; o excedente
	# nacional (necessidade × margem) é rateado pela TERRA OCIOSA de quem ainda
	# tem folga. O rateio anterior dividia a meta nacional pela terra apta de
	# cada cidade — Viena, com 15% da terra da Áustria e 89% da população,
	# recebia 138 t/dia para uma necessidade de 720 e virava importadora
	# permanente sem renda para pagar (§19). Alocar a necessidade primeiro só
	# muda QUEM produz, não o total nacional (que continua limitado por
	# `national_eff`).
	var resolved: Array = []
	for item in entries:
		var entry: Dictionary = item["entry"]
		var code: String = item["code"]
		if code != "":
			var capacity := float(entry.get("output_tons_per_day", 0.0))
			var aptitude: float = item["aptitude"]
			var land_cap: float = item["land_cap"]
			var land_eff: float = item["land_eff"]
			var city_need: float = item["need"]
			var tag: String = item["tag"]
			var nation_land: float = float(land_eff_by_nation.get(tag, 0.0))
			var nation_need: float = float(need_by_nation.get(tag, 0.0))
			var nation_self: float = float(self_by_nation.get(tag, 0.0))
			if aptitude > 0.0 and nation_land > 0.0:
				var target_eff := minf(city_need, land_eff)
				var national_eff := minf(nation_need * INITIAL_FARM_EXPORT_MARGIN, nation_land)
				var spare := land_eff - target_eff
				if spare > 0.0 and national_eff > nation_self:
					var nation_spare := maxf(nation_land - nation_self, 0.0)
					if nation_spare > 0.0:
						target_eff += minf(
							(national_eff - nation_self) * spare / nation_spare, spare)
				# Só aumenta a capacidade do CSV: as 8 nações originais já eram
				# calibradas e não podem perder produção (comida ≥ 98%).
				capacity = maxf(capacity, target_eff / aptitude)
			if land_cap > 0.0 and capacity > land_cap:
				capacity = land_cap
			entry["output_tons_per_day"] = capacity
		resolved.append(entry)
	return resolved


## Cultura da fazenda inicial: trigo quando viável NA BACIA DA CIDADE (aptidão
## ≥ 0,30); senão o básico de maior aptidão × caloria também na cidade; sem
## aptidão local, cai para a melhor cultura nacional; sem dado nenhum, trigo.
## Antes a viabilidade era nacional (melhor território do país): uma cidade de
## Azerbaijão com trigo 0,0 mas milho 0,05 recebia trigo, e a fazenda nascia
## inerte (aptidão 0) e bloqueava o governo de construir a cultura viável —
## causa de AZE/ISL/SAU ficarem em 0% (§19).
## O critério é ESTRITAMENTE local: uma cidade com trigo 0,0 e soja 0,45 recebe
## soja. O atalho "sem trigo local, mas nação de trigo → trigo" deixava 19
## cidades (São Paulo, Xangai, Moscou, Houston...) com fazenda de aptidão 0 —
## prédio sem empregos, sem folha e sem produção, que não alimentava ninguém e
## tirava a renda familiar da cidade (§19). Só as 8 cidades SEM aptidão alguma
## (ISL, SAU, CAN-OTT, IDN-SUR, MEX-TIJ, ZAF-CAP) caem na melhor cultura
## nacional — e, na prática, importam: não há terra na base para elas.
func _initial_staple(code: String, city_id: String = "") -> String:
	if code == "":
		return "wheat"
	if city_id != "" and city_crop_suitability(city_id, "wheat") >= 0.30:
		return "wheat"
	var best := "wheat"
	var best_score := -1.0
	for crop in STAPLE_CROPS:
		var aptitude := city_crop_suitability(city_id, crop) if city_id != "" else 0.0
		var score := aptitude * float(STAPLE_CALORIC.get(crop, 0.0))
		if score > best_score:
			best_score = score
			best = crop
	if best_score <= 0.0:
		return best_staple_nation(code)
	return best


## Maior cobertura florestal entre as províncias da nação (fonte fina). Sem
## province_nature.json, cai no melhor território (legado).
func forest_cover_nation(code: String) -> float:
	if not load_data():
		return 0.0
	var best := 0.0
	var found := false
	for province_id in _province_by_code.get(code, []):
		var province_data: Dictionary = _province_nature.get(str(province_id), {})
		if province_data.is_empty():
			continue
		found = true
		best = maxf(best, clampf(
			float(province_data.get("forest_cover", 0.0)), 0.0, 1.0))
	if found:
		return best
	for territory_id in _nation_territories(code):
		best = maxf(best, clampf(
			float(_nature.get(territory_id, {}).get("forest_cover", 0.0)), 0.0, 1.0))
	return best


# --- Bacia de captação por cidade (regra A1: nação + landmass + distância) ---

## Atribui cada PROVÍNCIA de uma nação jogável a UMA cidade — a cidade da
## MESMA nação mais próxima do centróide da província, restrita à MESMA massa
## de terra (landmass da cidade mais próxima de qualquer nação, proxy de
## continente — o mesmo critério de `_assign_deposits`). Sem cidade da nação na
## landmass, cai para a cidade mais próxima da nação (qualquer landmass), para
## nunca perder uma província. Províncias de nações não jogáveis (sem tag no
## registro de nations.json) são ignoradas. A soma das províncias por nação é uma partição:
## nenhuma província entra em duas cidades e nenhuma fica de fora. A unidade é a
## PROVÍNCIA admin-1 (fina): o TERRITÓRIO era grosso demais (França 11, Reino
## Unido 4, Bélgica 1) e deixava 9 das 33 cidades sem terra.
##
## Exceção (cidade cercada): com poucas províncias para muitas cidades (Alemanha
## tem 16 províncias para 8 cidades e três delas ficam sobre Berlim/Hamburgo),
## uma cidade pode não ser a mais próxima de NENHUMA província da nação —
## Kassel, na Hesse, cujo centróide fica mais perto de Frankfurt. Nesse caso a
## província mais próxima é DIVIDIDA entre as duas cidades na proporção inversa
## das distâncias (`_share_starved_provinces`), mantendo a partição POR ÁREA
## (Σ frações da província = 1, soma das cidades = nação): a cidade recebe terra
## em vez de ficar sem, e não há área duplicada. `province_share` informa a
## fração; todo cálculo de área/aptidão da bacia usa essa fração.
## Retorna {city_id: [province_id]} e cacheia em `_catchment`.
func assign_provinces(cities: Array) -> Dictionary:
	if not load_data():
		return {}
	var norm := _normalized_cities(cities)
	var catchment := {}
	for province in _provinces:
		var tag := nation_for_province_tag(String(province.get("nation", "")))
		if tag == "":
			continue
		var point := Vector2(float(province.get("x", 0.0)), float(province.get("y", 0.0)))
		var city_id := _nearest_city_of_nation(norm, tag, point)
		if city_id == "":
			continue
		if not catchment.has(city_id):
			catchment[city_id] = []
		catchment[city_id].append(int(province.get("id", 0)))
	_share_starved_provinces(norm, catchment)
	_catchment = catchment
	return catchment


## Fração da província pertencente à cidade (1.0 = província inteira). Só é
## fracionária quando a província foi dividida para uma cidade cercada herdar
## terra em vez de ficar sem (ver `_share_starved_provinces`).
func province_share(city_id: String, province_id: int) -> float:
	return float(_province_share.get(city_id, {}).get(province_id, 1.0))


## Garante terra para toda cidade jogável: cada cidade que ficou sem nenhuma
## província recebe a fração da província da nação mais próxima do seu centro,
## tomada do dono atual na proporção inversa das distâncias
## (fração = d_dono / (d_cidade + d_dono), logo < 0,5 — o dono continua com a
## maioria e nunca perde a província). Determinístico: percorre as nações e as
## cidades na ordem recebida. Só age em cidades sem terra; nas demais o
## resultado é idêntico ao passe principal de `assign_provinces`.
func _share_starved_provinces(cities: Array, catchment: Dictionary) -> void:
	_province_share.clear()
	var by_nation := {}
	for city in cities:
		var tag := String(city.get("tag", ""))
		if not is_playable(tag):
			continue
		if not by_nation.has(tag):
			by_nation[tag] = []
		by_nation[tag].append(city)
	for tag: String in by_nation:
		var peers: Array = by_nation[tag]
		for city in peers:
			var city_id := String(city["id"])
			if not (catchment.get(city_id, []) as Array).is_empty():
				continue
			_share_nearest_province(city, peers, cities, catchment)


## Corpo de `_share_starved_provinces` para UMA cidade sem terra. A província
## candidata respeita a MESMA massa de terra da cidade (regra A1); só sem
## candidata na landmass cai para a mais próxima da nação (nunca deixa a cidade
## sem terra).
func _share_nearest_province(city: Dictionary, _peers: Array, all_cities: Array,
		catchment: Dictionary) -> void:
	var tag := String(city.get("tag", ""))
	var center: Vector2 = city["center"]
	var landmass := String(city.get("landmass", ""))
	var nearest := _nearest_nation_province(tag, center, all_cities, landmass)
	if nearest.is_empty():
		nearest = _nearest_nation_province(tag, center, all_cities, "")
	# Nação sem NENHUMA província no atlas (Vaticano): doa uma fração simbólica
	# da província mais próxima, mesmo de outro país, para a cidade não ficar
	# sem terra. A regra de fração abaixo continua valendo quando há dono.
	var city_state := nearest.is_empty()
	if nearest.is_empty():
		nearest = _nearest_any_province(center, all_cities, landmass)
	if nearest.is_empty():
		nearest = _nearest_any_province(center, all_cities, "")
	if nearest.is_empty():
		return
	var pid := int(nearest.get("id", 0))
	var point := Vector2(float(nearest.get("x", 0.0)), float(nearest.get("y", 0.0)))
	var owner_tag := nation_for_province_tag(String(nearest.get("nation", "")))
	var owner_id := String(city["id"]) if owner_tag == "" \
			else _nearest_city(all_cities, owner_tag, point, "")
	if owner_id == String(city["id"]):
		return
	if owner_id == "":
		# Província de nação não jogável: não há cidade dona do restante; a
		# cidade recebe só o piso simbólico (o resto segue sem dono no sim).
		var city_state_shares: Dictionary = _province_share.get(String(city["id"]), {})
		city_state_shares[pid] = CITY_STATE_MIN_SHARE
		_province_share[String(city["id"])] = city_state_shares
		if not catchment.has(String(city["id"])):
			catchment[String(city["id"])] = []
		(catchment[String(city["id"])] as Array).append(pid)
		return
	var owner_center := Vector2.ZERO
	for peer in all_cities:
		if String(peer["id"]) == owner_id:
			owner_center = peer["center"]
			break
	var owner_distance := owner_center.distance_to(point)
	var city_distance := center.distance_to(point)
	var city_share := owner_distance / (city_distance + owner_distance) \
			if city_distance + owner_distance > 0.0 else 0.5
	if city_state:
		city_share = maxf(city_share, CITY_STATE_MIN_SHARE)
	var owner_shares: Dictionary = _province_share.get(owner_id, {})
	var owner_share := float(owner_shares.get(pid, 1.0))
	# Piso de segurança em divisões em cadeia: o dono nunca fica com zero.
	city_share = minf(city_share, owner_share * 0.5)
	if city_share <= 0.0:
		return
	owner_shares[pid] = owner_share - city_share
	_province_share[owner_id] = owner_shares
	var city_shares: Dictionary = _province_share.get(String(city["id"]), {})
	city_shares[pid] = city_share
	_province_share[String(city["id"])] = city_shares
	if not catchment.has(String(city["id"])):
		catchment[String(city["id"])] = []
	(catchment[String(city["id"])] as Array).append(pid)


## Província da nação mais próxima de `center`, restrita à massa de terra
## `landmass` quando informada ("" = qualquer massa). Determinístico: empate fica
## com a primeira na ordem do atlas.
func _nearest_nation_province(tag: String, center: Vector2, cities: Array,
		landmass: String) -> Dictionary:
	var nearest: Dictionary = {}
	var best_distance := INF
	var atlas_tag := province_tag(tag)
	if atlas_tag == "":
		return nearest
	for province in _provinces:
		if String(province.get("nation", "")) != atlas_tag:
			continue
		var point := Vector2(float(province.get("x", 0.0)), float(province.get("y", 0.0)))
		if landmass != "" and _landmass_of(point, cities) != landmass:
			continue
		var distance := center.distance_to(point)
		if distance < best_distance:
			best_distance = distance
			nearest = province
	return nearest


## Província mais próxima de QUALQUER nação (fallback de nação sem província).
## Prefere a mesma massa de terra; sem candidata nela, aceita qualquer uma.
func _nearest_any_province(center: Vector2, cities: Array, landmass: String) -> Dictionary:
	var nearest: Dictionary = {}
	var best_distance := INF
	for province in _provinces:
		var point := Vector2(float(province.get("x", 0.0)), float(province.get("y", 0.0)))
		if landmass != "" and _landmass_of(point, cities) != landmass:
			continue
		var distance := center.distance_to(point)
		if distance < best_distance:
			best_distance = distance
			nearest = province
	return nearest


## Lista de cidades normalizada (id/tag/center/landmass) a partir de dicionários
## que usam "center" (Vector2 ou Array) OU "x"/"y" (floats) — resolve ambos.
func _normalized_cities(cities: Array) -> Array:
	var out: Array = []
	for city in cities:
		var center_v: Variant = city.get("center", null)
		var point: Vector2
		if center_v is Vector2:
			point = center_v
		elif center_v is Array and center_v.size() >= 2:
			point = Vector2(float(center_v[0]), float(center_v[1]))
		else:
			point = Vector2(float(city.get("x", 0.0)), float(city.get("y", 0.0)))
		var city_id := String(city.get("id", ""))
		out.append({
			"id": city_id,
			"tag": String(city.get("nation", "")),
			"center": point,
			"landmass": String(city.get("landmass", _city_landmass.get(city_id, ""))),
		})
	return out


## Aptidão (0..1) de uma cultura na BACIA da cidade: média ponderada pela área
## cultivável (área_km² × fração cultivável) de cada PROVÍNCIA atribuída. A
## aptidão e a fração cultivável de cada província herdam do território natural
## que a contém. Sem província, 0 (cidade sem terra não produz — importa). É o
## valor que `apply_to_sim` declara ao núcleo (substitui o máximo nacional).
func city_crop_suitability(city_id: String, crop: String) -> float:
	var provinces: Array = _catchment.get(city_id, [])
	var weighted := 0.0
	var weight := 0.0
	for province_id in provinces:
		var pid := int(province_id)
		var area := float(_area_by_province.get(pid, 0.0)) * province_share(city_id, pid)
		var cultivable: float
		var value: float
		var province_data: Dictionary = _province_nature.get(str(pid), {})
		if not province_data.is_empty():
			# Fonte fina: clima real amostrado no centróide DESTA província.
			cultivable = clampf(float(province_data.get("cultivable", 0.0)), 0.0, 1.0)
			var crops: Dictionary = province_data.get("crops", {})
			value = clampf(float(crops.get(crop, 0.0)), 0.0, 1.0)
		else:
			# Fallback declarado: província sem raster herda o território.
			var tid := String(_province_territory.get(pid, ""))
			if tid == "":
				continue
			cultivable = clampf(
				float(_nature.get(tid, {}).get("cultivable", 0.0)), 0.0, 1.0)
			value = clampf(float(_agriculture.get(tid, {}).get(crop, 0.0)), 0.0, 1.0)
		var w := area * cultivable
		weighted += w * value
		weight += w
	return weighted / weight if weight > 0.0 else 0.0


## Cobertura florestal (0..1) na bacia da cidade: média ponderada pela ÁREA
## total das províncias (a floresta cobre terra, independentemente da fração
## cultivável).
func city_forest_cover(city_id: String) -> float:
	var provinces: Array = _catchment.get(city_id, [])
	var weighted := 0.0
	var weight := 0.0
	for province_id in provinces:
		var pid := int(province_id)
		var area := float(_area_by_province.get(pid, 0.0)) * province_share(city_id, pid)
		var value: float
		var province_data: Dictionary = _province_nature.get(str(pid), {})
		if not province_data.is_empty():
			value = clampf(float(province_data.get("forest_cover", 0.0)), 0.0, 1.0)
		else:
			var tid := String(_province_territory.get(pid, ""))
			if tid == "":
				continue
			value = clampf(
				float(_nature.get(tid, {}).get("forest_cover", 0.0)), 0.0, 1.0)
		weighted += area * value
		weight += area
	return weighted / weight if weight > 0.0 else 0.0


## Área cultivável (km²) da bacia da cidade: Σ área da província × fração
## cultivável (herdada do território que a contém) × fração da província que
## pertence à cidade (divisão de cidade cercada — ver `_share_starved_provinces`).
func city_cultivable_area_km2(city_id: String) -> float:
	var total := 0.0
	for province_id in _catchment.get(city_id, []):
		total += province_cultivable_area_km2(int(province_id)) \
				* province_share(city_id, int(province_id))
	return total


## Área cultivável (km²) de uma província: área da própria província × fração
## cultivável herdada do território natural que a contém. Útil para auditoria e
## para a partição da bacia (soma das cidades = soma das províncias da nação).
func province_cultivable_area_km2(province_id: int) -> float:
	var province_data: Dictionary = _province_nature.get(str(province_id), {})
	if not province_data.is_empty():
		return float(_area_by_province.get(province_id, 0.0)) \
			* clampf(float(province_data.get("cultivable", 0.0)), 0.0, 1.0)
	var tid := String(_province_territory.get(province_id, ""))
	if tid == "":
		return 0.0
	return float(_area_by_province.get(province_id, 0.0)) \
		* clampf(float(_nature.get(tid, {}).get("cultivable", 0.0)), 0.0, 1.0)


## Teto físico de capacidade NOMINAL (t/dia) de fazendas da cultura na cidade:
## área cultivável × produtividade média FAO (t/km²/dia). A produção efetiva é
## capacidade × aptidão, então o teto nominal × aptidão = área cultivável efetiva
## (Σ área × cultivável × aptidão) × produtividade — o teto físico real.
## Culturas sem produtividade cadastrada (algodão/madeira) retornam 0 (sem teto
## aplicado pela IA — a demanda externa delas já é pequena).
func city_land_capacity_t_day(city_id: String, crop: String) -> float:
	var per_km2 := float(CROP_YIELD_T_PER_KM2_PER_DAY.get(crop, 0.0))
	if per_km2 <= 0.0:
		return 0.0
	return city_cultivable_area_km2(city_id) * per_km2


## Depósitos minerais/energéticos do TERRITÓRIO da cidade. NÃO é a regra de
## posse: várias cidades podem estar no mesmo território e veriam a mesma
## lista. Use `deposits_for_city` para o que a cidade realmente possui.
func deposits_for(city_id: String, center_px: Vector2) -> Array:
	var territory := territory_for(city_id, center_px)
	var entries: Array = _deposits_by_territory.get(territory, [])
	return entries.duplicate(true)


# --- Posse mineral por cidade (regra A1) -----------------------------------

## Cidades do cenário (cities.json) no formato id/nation/center/landmass.
## Fonte única para resolver o dono de uma jazida fora do caminho que chama
## `apply_to_sim` (legenda da cidade, painel e mapa de recursos).
func scenario_cities() -> Array:
	if _roster_ready:
		return _roster
	_roster_ready = true
	_roster = []
	var parsed = JSON.parse_string(FileAccess.get_file_as_string(CITIES_PATH))
	if typeof(parsed) != TYPE_ARRAY:
		push_warning("GeographyService: cities.json inválido " + CITIES_PATH)
		return _roster
	for city in parsed:
		_roster.append({
			"id": String(city.get("id", "")),
			"nation": String(city.get("nation", "")),
			"center": Vector2(float(city.get("x", 0.0)), float(city.get("y", 0.0))),
			"landmass": String(city.get("landmass", "")),
		})
	return _roster


## Garante a atribuição A1 com as cidades do cenário quando ninguém chamou
## `assign_deposits` ainda (caminho da legenda/painel/mapa).
func ensure_deposit_ownership() -> void:
	if not load_data():
		return
	if _deposits_by_city.is_empty():
		assign_deposits(scenario_cities())


## Jazidas que pertencem à cidade (regra A1). Vazio = a cidade não tem jazida
## própria — ela importa o mineral, não o produz. Cópia: o chamador não altera
## o cache.
func deposits_for_city(city_id: String) -> Array:
	ensure_deposit_ownership()
	var entries: Array = _deposits_by_city.get(city_id, [])
	return entries.duplicate(true)


## Minerais PRÓPRIOS da cidade com contagem e reserva agregada:
## {recurso: {"count": int, "reserve_kt": float}}. É o que a legenda e o painel
## mostram — nunca o total nacional.
func city_mineral_reserves(city_id: String) -> Dictionary:
	var out := {}
	for deposit in deposits_for_city(city_id):
		var resource := String(deposit.get("resource", ""))
		var qty := float(deposit.get("quantity_kt", 0.0))
		if resource == "" or qty <= 0.0:
			continue
		if not out.has(resource):
			out[resource] = {"count": 0, "reserve_kt": 0.0}
		out[resource]["count"] = int(out[resource]["count"]) + 1
		out[resource]["reserve_kt"] = float(out[resource]["reserve_kt"]) + qty
	return out


## Cidade dona de um depósito ("" se ele não pertence a nenhuma cidade
## jogável). A chave é posição + recurso, a mesma usada na atribuição.
func deposit_owner_city(deposit: Dictionary) -> String:
	ensure_deposit_ownership()
	return String(_deposit_owner_key.get(_deposit_key(deposit), ""))


## Cidades donas das jazidas do recurso nos territórios informados. É como o
## marcador agregado do mapa ("reúne N jazidas") declara o dono sem atribuir
## tudo à capital: cada território contribui com as cidades que realmente
## possuem aquelas jazidas.
func owners_for_territories(territories: Array, resource: String) -> Array:
	ensure_deposit_ownership()
	var seen := {}
	var out: Array = []
	for territory_id in territories:
		for deposit in _deposits_by_territory.get(String(territory_id), []):
			if String(deposit.get("resource", "")) != resource:
				continue
			var owner := deposit_owner_city(deposit)
			if owner == "" or seen.has(owner):
				continue
			seen[owner] = true
			out.append(owner)
	return out


## Atribui cada depósito nacional a UMA cidade (regra A1). Retorna
## {city_id: [depósito, ...]} sem duplicação: cada depósito entra em exatamente
## uma cidade, então a soma das reservas por nação no sim é a soma dos depósitos
## da nação. Depósitos de nações não jogáveis (sem tag no registro) são
## ignorados. Idempotente: a mesma lista de cidades reaproveita o cache.
func assign_deposits(cities: Array) -> Dictionary:
	var norm := _normalized_cities(cities)
	var signature := _assignment_signature(norm)
	if signature == _assign_signature:
		return _deposits_by_city
	_assign_signature = signature

	var assigned := {}
	var owner_key := {}
	for deposit in _deposits:
		if bool(deposit.get("facility", false)):
			continue
		var point := Vector2(float(deposit.get("px", 0.0)), float(deposit.get("py", 0.0)))
		# Dono pela província mais próxima do PONTO REAL do depósito. O
		# território natural agrupa 739 regiões grossas do mundo antigo; usar o
		# adm0_a3 dele jogava 14,8% dos depósitos na nação errada (e deixava 98
		# nações sem nenhum). A província admin-1 é o dado fino do atlas.
		var tag := nation_tag_at(point)
		if tag == "" or not is_playable(tag):
			continue
		var city_id := _nearest_city_of_nation(norm, tag, point)
		if city_id == "":
			continue
		if not assigned.has(city_id):
			assigned[city_id] = []
		assigned[city_id].append(deposit)
		owner_key[_deposit_key(deposit)] = city_id

	_deposits_by_city = assigned
	_deposit_owner_key = owner_key
	return _deposits_by_city


## Chave estável de um depósito. Usa o `id` quando existe (deposits.json tem
## ids únicos, inclusive para jazidas de mesma posição/recurso); sem id (extrato
## do mapa), cai para posição + recurso. Não inclui quantidade: a mesma jazida
## aparece com campos levemente diferentes nos dois JSONs e continua a mesma.
func _deposit_key(deposit: Dictionary) -> String:
	var deposit_id := String(deposit.get("id", ""))
	if deposit_id != "":
		return deposit_id
	return "%s|%s|%s" % [str(snappedf(float(deposit.get("px", 0.0)), 0.1)),
			str(snappedf(float(deposit.get("py", 0.0)), 0.1)),
			String(deposit.get("resource", ""))]


func _assignment_signature(cities: Array) -> String:
	var parts := PackedStringArray()
	for city in cities:
		parts.append("%s/%s/%s/%s" % [String(city["id"]), String(city["tag"]),
				String(city["landmass"]), str(city["center"])])
	return "|".join(parts)


## Alias legado da atribuição (mesma regra A1). Mantido para chamadores antigos.
func _assign_deposits(cities: Array) -> Dictionary:
	return assign_deposits(cities)


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


## Resumo legível do território (auditoria e painéis). `deposits` conta apenas
## as jazidas que PERTENCEM à cidade (regra A1), não as do território — senão
## cidades vizinhas no mesmo território mostrariam o mesmo número.
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
		"deposits": deposits_for_city(city_id).size(),
	}


## Aplica a aptidão de cada cultura e os depósitos minerais de cada cidade ao
## núcleo econômico. As culturas renováveis entram como aptidão (0..1); os
## minerais entram como reserva agregada (demo_set_deposit) + aptidão 1. O C++
## ignora produtos que não conhece (ex.: carvão/cobre/gás/petróleo ficam só no
## mapa por enquanto).
##
## Também declara a RENDA EXTERNA de cada cidade: índice = PIB PPP per capita da
## nação / média mundial ponderada por população (nations.json), com piso e
## normalização (ver `_build_nation_income`). É renda de serviços/exportações
## não modelados, paga pelo setor externo — não é subsídio uniforme.
##
## Aptidão de cultura POR CIDADE: cada PROVÍNCIA da nação é atribuída a uma
## única cidade (regra A1, ver assign_provinces) e a aptidão da cidade é a
## média ponderada pela área cultivável da sua bacia (madeira: pela área com
## forest_cover). Substitui o antigo máximo nacional — antes todas as cidades de
## uma nação plantavam como a melhor região (Marselha como a Beauce).
##
## Regra de reserva mineral (A1): cada depósito nacional é atribuído a UMA
## cidade — a cidade da MESMA nação mais próxima do depósito, restrita à mesma
## massa de terra (landmass das cidades, proxy de continente). Territórios
## ultramarinos (ex.: Guiana Francesa) só alimentam cidades na mesma massa de
## terra: Caiena não herda minério da França continental e Paris não herda o da
## Guiana. Sem duplicação: a soma das reservas por nação no sim é a soma dos
## depósitos da nação. Cidade sem nação conhecida mantém o fallback legado do
## próprio território (`deposits_for`). A legenda da cidade, o painel e o mapa
## leem a MESMA atribuição por `deposits_for_city` — o acesso nacional
## (`deposits_for_nation`) deixou de ser usado na ficha.
## @param sim Instância de EconomySim inicializada no modo demo.
## @param cities Lista de dicionários com "id", "nation" e "center" (Vector2 px).
## @returns Mapa cidade -> {crop: aptidão, mineral: reserva_kt}.
func apply_to_sim(sim: Object, cities: Array) -> Dictionary:
	# Garante os dados carregados mesmo quando o chamador não chama load_data()
	# antes (era o caminho real do jogo ao vivo, que declarava aptidão 0 e zero
	# depósitos para todas as nações). Idempotente; retorna vazio se falhar.
	if not load_data():
		return {}
	var applied := {}
	var crops := ["wheat", "rice", "maize", "potato", "soy", "cotton"]
	assign_provinces(cities)
	var deposits_by_city := assign_deposits(cities)
	for city in cities:
		var city_id := String(city.get("id", ""))
		var center: Vector2 = city.get("center", Vector2.ZERO)
		var code := nation_code(String(city.get("nation", "")))
		var city_applied := {}
		# Renda externa proporcional ao PIB PPP per capita da nação (§20).
		# Declarada aqui porque é a mesma ponte dados->núcleo da geografia. O
		# índice NÃO entra no mapa retornado: ele é {cultura: aptidão,
		# mineral: reserva_kt} e os consumidores somam tudo que não é cultura
		# como reserva. O valor sai por `external_income_index`/perfil.
		var income_index := external_income_index(String(city.get("nation", "")))
		sim.demo_set_external_income_index(city_id, income_index)
		for crop in crops:
			# Aptidão DA CIDADE (bacia de captação), não mais o máximo nacional:
			# Marselha planta como o seu entorno, não como a Beauce. Cidade sem
			# nação conhecida mantém o fallback legado do próprio território.
			var suitability := city_crop_suitability(city_id, crop) if code != "" \
					else crop_suitability(city_id, center, crop)
			sim.demo_set_suitability(city_id, crop, suitability)
			city_applied[crop] = suitability
		var wood := city_forest_cover(city_id) if code != "" \
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
		# Depósitos atribuídos pela regra A1 (uma cidade por depósito, mesma nação
		# e mesma massa de terra). Cidade sem nação conhecida usa o território
		# próprio (fallback legado, sem agregação nacional).
		var deposits: Array = deposits_by_city.get(city_id, []) if code != "" \
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
			# Sem arredondar para inteiro: jazidas de 12 t (0,012 kt) sumiriam
			# (reserva 0) e a cidade ficaria sem o mineral que os dados declaram.
			"reserve_kt": maxf(snappedf(total, 0.001), 0.001),
			"quality": snappedf(agg["quality_w"] / total, 0.001),
			"depth_m": int(round(agg["depth_w"] / total)),
		}
	return out


## Cidade de `tag` mais próxima de `point`, restrita à mesma massa de terra
## (landmass da cidade mais próxima de qualquer nação — proxy de continente).
## Sem cidade na massa de terra, cai para a cidade mais próxima da nação
## (qualquer landmass), para nunca perder um depósito da nação.
func _nearest_city_of_nation(cities: Array, tag: String, point: Vector2) -> String:
	var landmass := _landmass_of(point, cities)
	var candidate := _nearest_city(cities, tag, point, landmass)
	if candidate != "":
		return candidate
	return _nearest_city(cities, tag, point, "")


func _nearest_city(cities: Array, tag: String, point: Vector2, landmass: String) -> String:
	var best_id := ""
	var best_distance := INF
	for city in cities:
		if String(city["tag"]) != tag:
			continue
		if landmass != "" and String(city["landmass"]) != landmass:
			continue
		var distance: float = (city["center"] as Vector2).distance_squared_to(point)
		if distance < best_distance:
			best_distance = distance
			best_id = String(city["id"])
	return best_id


func _landmass_of(point: Vector2, cities: Array) -> String:
	var best := ""
	var best_distance := INF
	for city in cities:
		var distance: float = (city["center"] as Vector2).distance_squared_to(point)
		if distance < best_distance:
			best_distance = distance
			best = String(city["landmass"])
	return best


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


## Massa de terra (landmass do cenário) por cidade, lida de cities.json. É o
## proxy de "continente" usado na regra A1 para isolar territórios ultramarinos
## (Guiana x França continental). Retorna {} se o arquivo não abrir.
func _load_city_landmasses() -> Dictionary:
	var out := {}
	if not FileAccess.file_exists(CITIES_PATH):
		return out
	var parsed = JSON.parse_string(FileAccess.get_file_as_string(CITIES_PATH))
	if typeof(parsed) != TYPE_ARRAY:
		return out
	for city in parsed:
		out[String(city.get("id", ""))] = String(city.get("landmass", ""))
	return out


## Carrega as províncias admin-1 (atlas) e a área de cada uma; monta o índice de
## herança província -> território natural (o território de centróide mais
## próximo, que "contém" a província). É a fonte da aptidão agrícola e da
## cobertura florestal em granularidade fina (a unidade TERRITÓRIO é grossa
## demais — França tem só 11 territórios, o que deixava 9 das 33 cidades sem
## terra).
func _load_provinces() -> void:
	_provinces.clear()
	_area_by_province.clear()
	_province_territory.clear()
	_province_by_code.clear()
	_province_grid.clear()
	_province_center.clear()
	_province_tag.clear()
	_province_nature = _read_json(PROVINCE_NATURE_PATH).get("provinces", {})
	var raw := _read_json(PROVINCES_PATH)
	var province_areas: Dictionary = _read_json(PROVINCE_AREAS_PATH).get("areas", {})
	for record in raw.get("provinces", []):
		if typeof(record) != TYPE_DICTIONARY:
			continue
		var pid := int(record.get("id", 0))
		if pid <= 0:
			continue
		var point := Vector2(float(record.get("x", 0.0)), float(record.get("y", 0.0)))
		_provinces.append({
			"id": pid,
			"nation": String(record.get("nation", "")),
			"country": String(record.get("country", "")),
			"x": point.x,
			"y": point.y,
		})
		_area_by_province[pid] = float(province_areas.get(str(pid), 0.0))
		_province_center[pid] = point
		var tag := nation_for_province_tag(String(record.get("nation", "")))
		_province_tag[pid] = tag
		if tag != "":
			var code := nation_code(tag)
			if not _province_by_code.has(code):
				_province_by_code[code] = []
			(_province_by_code[code] as Array).append(pid)
		var cell := Vector2i(int(floor(point.x / PROVINCE_GRID_CELL)),
				int(floor(point.y / PROVINCE_GRID_CELL)))
		if not _province_grid.has(cell):
			_province_grid[cell] = []
		(_province_grid[cell] as Array).append(pid)
	# Herança da aptidão/cobertura florestal pelo centróide da província: o
	# território natural mais próximo (não restrito ao país — províncias
	# ultramarinas como a Guiana Francesa herdam do território tropical vizinho,
	# não da França continental, que fica a ~8.000 km).
	for province in _provinces:
		var point := Vector2(float(province["x"]), float(province["y"]))
		_province_territory[int(province["id"])] = _nearest_territory(point)


func _read_json(path: String) -> Dictionary:
	if not FileAccess.file_exists(path):
		push_warning("GeographyService: arquivo ausente " + path)
		return {}
	var parsed = JSON.parse_string(FileAccess.get_file_as_string(path))
	if typeof(parsed) != TYPE_DICTIONARY:
		push_warning("GeographyService: JSON inválido " + path)
		return {}
	return parsed
