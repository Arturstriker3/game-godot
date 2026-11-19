extends RefCounted
class_name StorageService

## G2 (D-101) — ARMAZENAGEM: ponte entre os dados da G2 e o núcleo.
##
## Declara no núcleo, ANTES do dia 1 (mesmo contrato dos setters da geografia e
## da agricultura):
##   * o mapeamento produto→tipo de armazém (`goods_storage_map.json`, 113 bens);
##   * a capacidade inicial de cada armazém — DADO real quando publicado
##     (`storage_capacity.json`) e, quando ele não existe, a REGRA DECLARADA do
##     núcleo (meses de produção, espelho de `kStorageInitialMonths`);
##   * a semente do dia 1 da reserva estratégica pública
##     (`strategic_reserves.json`: petróleo público e grãos públicos).
##
## O QUE É DADO: capacidade de grãos (Mt), câmara fria (m³), gás (TWh), estoque
## público de petróleo (kt) e de grãos (t), reservas de ouro (t) e o mapa
## produto→tipo. O QUE É MODELO DECLARADO AQUI (constantes): a repartição
## governo×privado da capacidade, o rateio nacional→cidade e as conversões de
## unidade que o JSON não traz (m³→t do frio, TWh→t do gás). Nenhum valor por
## país é inventado: quando o dado falta, vale a regra declarada ou o vazio
## (`basis: none` do arquivo).
##
## RATEIO NACIONAL→CIDADE (declarado): a capacidade de um tipo é dividida entre
## as cidades da nação pelo PESO DE PRODUÇÃO dos bens que aquele tipo guarda (as
## mesmas fazendas/minas iniciais que o mundo já tem); sem produção daquele
## tipo, o peso é a população da cidade; sem produção e sem população, rateio
## igual. O relatório diz qual base foi usada (`rateio_basis`) e de onde veio o
## número (`capacity_source`: dado do JSON ou regra do núcleo).
##
## COFRE E PAIOL: são ESTRUTURA nesta parte (D-100) — não guardam estoque no
## núcleo. A reserva de ouro do BC (dado real, 161 nações) é LIDA e REPORTADA,
## não semeada: aplicar tonelada de ouro num cofre que não tem fluxo de estoque
## criaria um número sem lastro. Fica registrado no relatório (`gold_*`).

const CAPACITY_PATH := "res://data/definitions/economy/storage_capacity.json"
const RESERVES_PATH := "res://data/definitions/economy/strategic_reserves.json"
const MAP_PATH := "res://data/definitions/economy/goods_storage_map.json"

## Espelhos das constantes do núcleo (`gdextension/src/sim/sim_constants.h`).
## Mudar lá exige mudar aqui — o teste confere os dois números.
const STORAGE_INITIAL_MONTHS := 2.0      # kStorageInitialMonths
const STORAGE_GOVERNMENT_SHARE := 0.25   # kStorageGovernmentShare
const YEAR_DAYS := 365.0

## Conversões de unidade que o JSON não declara. MODELO DECLARADO (calibrável),
## não dado:
##   * frio: 4 m³ de câmara fria por tonelada guardada (pallet de congelado);
##   * gás: 1 t de GNL = 14,28 MWh (48,7 MMBtu) → 1 TWh = 1e6/14,28 = 70.028 t.
const COLD_M3_PER_TON := 4.0
const GAS_TWH_TO_TONS := 70028.0
const GRAIN_MT_TO_T := 1_000_000.0
const OIL_KT_TO_T := 1_000.0

## Nome do armazém no JSON → nome do tipo no núcleo (`storage_type_from_name`).
const WAREHOUSE_TO_TYPE := {
	"silo": "silo",
	"cold_room": "cold",
	"fuel_tank": "fuel_tank",
	"gas_storage": "gas",
	"bulk_yard": "bulk_yard",
	"general_warehouse": "general",
	"vault": "vault",
	"military_depot": "magazine",
}
## Tipos que só existem como ESTRUTURA (D-100): sem capacidade/estoque.
const STRUCTURE_ONLY := ["vault", "magazine"]
## Tipos operacionais na ordem do núcleo (relatório estável).
const OPERATIONAL_TYPES := ["silo", "cold", "fuel_tank", "gas", "bulk_yard", "general"]
## Produtos de grão do silo que rateiam o estoque público de grãos (o dado é
## agregado; o grão não é publicado).
const GRAIN_PRODUCTS := ["wheat", "rice", "maize", "soy"]

var _capacity_nations: Dictionary = {}
var _reserve_nations: Dictionary = {}
var _goods: Dictionary = {}
var _coverage: Dictionary = {}
var _loaded := false


## Carrega os três JSONs. Idempotente. Devolve false quando todos faltam — o
## chamador segue sem G2 no GDScript e o núcleo mantém a tabela declarada.
func load_data() -> bool:
	if _loaded:
		return true
	_capacity_nations = _read_dict(CAPACITY_PATH, "nations")
	_reserve_nations = _read_dict(RESERVES_PATH, "nations")
	_goods = _read_dict(MAP_PATH, "goods")
	_coverage = _read_dict(MAP_PATH, "coverage")
	_loaded = not (_capacity_nations.is_empty() and _reserve_nations.is_empty()
		and _goods.is_empty())
	if not _loaded:
		push_warning("StorageService: dados da G2 ausentes ou ilegíveis")
	return _loaded


## Nome do tipo no núcleo para um armazém do JSON ("gas_storage" → "gas").
static func core_type(json_name: String) -> String:
	return String(WAREHOUSE_TO_TYPE.get(json_name, ""))


## Empurra os dados para o núcleo. `cities` no formato de `cities.json`
## (id/nation/population) e `buildings` as construções JÁ resolvidas pela
## geografia (fazendas e minas com o campo `resource` e `output_tons_per_day`).
##
## Opções:
##   * `only_missing` — não toca em site (cidade, tipo, dono) que já existe no
##     núcleo (load de save v25: capacidade e estoque vieram do save, inclusive
##     o que a IA construiu). Default `false`;
##   * `initial_stock` — `false` não semeia a reserva pública do dia 1 (sonda).
##     Default `true`.
func apply_to_sim(sim: Object, cities: Array, buildings: Array,
		options: Dictionary = {}) -> Dictionary:
	var report := _empty_report(options)
	if not load_data():
		return report
	if not sim.has_method("set_storage_capacity"):
		push_warning("StorageService: binário sem set_storage_capacity (rode o build)")
		return report
	report["applied"] = true

	# 1. Mapa produto→tipo PRIMEIRO: capacidade e estoque dependem de qual tipo
	#    aceita qual produto no núcleo.
	_declare_product_types(sim, report)

	# 2. Cidades da nação, peso de produção por tipo (e por grão, para a reserva
	#    pública) e o que já existe no núcleo (load de save).
	var by_nation := _cities_by_nation(cities)
	var weights := _type_weights(buildings)
	var existing := {}
	if bool(options.get("only_missing", false)):
		existing = _existing_sites(sim)
	var seed_stock := bool(options.get("initial_stock", true))

	# 3. Capacidade e semente por nação.
	for tag in by_nation.keys():
		var nation_cities: Array = by_nation[tag]
		var capacity := _nation_capacity(tag, nation_cities, weights)
		var splits: Dictionary = {}
		for type in OPERATIONAL_TYPES:
			var total := float(capacity["total"].get(type, 0.0))
			if total <= 0.0:
				continue
			var government := minf(float(capacity["government"].get(type, 0.0)),
				total)
			var basis := String(capacity["basis"].get(type, "none"))
			var split := _split_capacity(total, government, nation_cities,
				weights, type, basis)
			if split.is_empty():
				# Sem cidade com peso: a capacidade do dado não tem onde ficar.
				report["capacity_unplaced_t"][type] = \
					float(report["capacity_unplaced_t"].get(type, 0.0)) + total
				_add_source(report, type, basis, total)
				continue
			splits[type] = split
			_write_capacity(sim, split, type, existing, report)
		# 4. Reserva pública do dia 1 (petróleo e grãos públicos do dado).
		if seed_stock:
			_seed_oil(sim, tag, splits, existing, report)
			_seed_grain(sim, tag, nation_cities, weights, splits, existing,
				report)
		# 5. Ouro do BC: lido e reportado, não semeado (cofre é estrutura).
		var gold := _gold_tonnes(tag)
		report["gold_reserve_t"] = float(report["gold_reserve_t"]) + gold
		if gold > 0.0:
			report["gold_nations"][tag] = gold
	report["nations"] = by_nation.size()
	report["cities"] = cities.size()
	# Diagnóstico de ESCALA: produção anual do jogo por tipo e quantos meses de
	# produção a capacidade inicial cobre. O dado é real (t) e o volume do jogo
	# ainda não está na escala real (plano, Parte F "cuidado de escala"): a
	# diferença fica MEDIDA aqui, não escondida.
	var game_annual: Dictionary = {}
	for city_id in weights.keys():
		var row: Dictionary = weights[city_id]
		for type in OPERATIONAL_TYPES:
			game_annual[type] = float(game_annual.get(type, 0.0)) \
				+ float(row.get(type, 0.0))
	for type in OPERATIONAL_TYPES:
		var annual := float(game_annual.get(type, 0.0))
		report["game_annual_t"][type] = annual
		var capacity_t := float(report["capacity_t"].get(type, 0.0))
		report["months_of_game_production"][type] = \
			capacity_t * 12.0 / annual if annual > 0.0 else 0.0
	return report


# ---------------------------------------------------------------------------
# Declaração no núcleo
# ---------------------------------------------------------------------------

func _declare_product_types(sim: Object, report: Dictionary) -> void:
	var applied := 0
	var skipped: Array = []
	for product in _goods.keys():
		var entry: Dictionary = _goods[product]
		var type := core_type(String(entry.get("primary", "")))
		if type == "":
			skipped.append(String(product))
			continue
		sim.set_storage_product_type(String(product), type)
		applied += 1
	report["product_types_applied"] = applied
	report["product_types_skipped"] = skipped
	report["product_types_total"] = _goods.size()


func _write_capacity(sim: Object, split: Array, type: String,
		existing: Dictionary, report: Dictionary) -> void:
	for row in split:
		var city_id := String(row["city"])
		var private_t := float(row["private"])
		var government_t := float(row["government"])
		if private_t > 0.0:
			if existing.has(city_id + "|" + type + "|1"):
				report["skipped_existing_sites"] = \
					int(report["skipped_existing_sites"]) + 1
			else:
				sim.set_storage_capacity(city_id, type, "private", private_t)
				report["sites_written"] = int(report["sites_written"]) + 1
				report["private_capacity_t"][type] = \
					float(report["private_capacity_t"].get(type, 0.0)) + private_t
		if government_t > 0.0:
			if existing.has(city_id + "|" + type + "|0"):
				report["skipped_existing_sites"] = \
					int(report["skipped_existing_sites"]) + 1
			else:
				sim.set_storage_capacity(city_id, type, "government", government_t)
				report["sites_written"] = int(report["sites_written"]) + 1
				report["government_capacity_t"][type] = \
					float(report["government_capacity_t"].get(type, 0.0)) \
					+ government_t
		report["capacity_t"][type] = float(report["capacity_t"].get(type, 0.0)) \
			+ private_t + government_t
		_add_source(report, type, String(row["source"]), private_t + government_t)
		var rateio: Dictionary = report["rateio_basis"][type]
		rateio[String(row["basis"])] = \
			float(rateio.get(String(row["basis"]), 0.0)) + private_t + government_t


# ---------------------------------------------------------------------------
# Capacidade
# ---------------------------------------------------------------------------

## Capacidade (t) por tipo da nação, separada em total/governo e com a base de
## cada número: `data` (JSON publicado, inclusive a regra declarada do próprio
## arquivo) ou `rule` (regra do núcleo — meses de produção — porque o JSON não
## tem o valor).
func _nation_capacity(tag: String, nation_cities: Array,
		weights: Dictionary) -> Dictionary:
	var capacity: Dictionary = _capacity_nations.get(tag, {})
	var out := {"total": {}, "government": {}, "basis": {}}
	for type in OPERATIONAL_TYPES:
		out["total"][type] = 0.0
		out["government"][type] = 0.0
		out["basis"][type] = "rule"

	# --- grãos: silo (dado em Mt) ---
	var grain: Dictionary = capacity.get("grain", {})
	var grain_mt := _num(grain.get("capacity_mt"))
	var silo_total := grain_mt * GRAIN_MT_TO_T
	if silo_total > 0.0:
		out["basis"]["silo"] = "data"
	else:
		silo_total = _fallback_capacity(nation_cities, weights, "silo")
	var public_capacity := _num(_reserve_nations.get(tag, {})
		.get("grain_public", {}).get("capacity_t"))
	var silo_gov := public_capacity if public_capacity > 0.0 \
		else silo_total * STORAGE_GOVERNMENT_SHARE
	out["total"]["silo"] = silo_total
	out["government"]["silo"] = minf(silo_gov, silo_total)

	# --- câmara fria: dado em m³ → t ---
	var cold: Dictionary = capacity.get("cold", {})
	var cold_m3 := _num(cold.get("capacity_m3"))
	var cold_total := cold_m3 / COLD_M3_PER_TON
	if cold_total > 0.0:
		out["basis"]["cold"] = "data"
	else:
		cold_total = _fallback_capacity(nation_cities, weights, "cold")
	out["total"]["cold"] = cold_total
	out["government"]["cold"] = cold_total * STORAGE_GOVERNMENT_SHARE

	# --- gás: dado em TWh → t; sem dado, 0 (regra declarada do arquivo) ---
	var gas: Dictionary = capacity.get("gas", {})
	var gas_twh := _num(gas.get("working_capacity_twh"))
	var gas_total := gas_twh * GAS_TWH_TO_TONS
	if gas_total > 0.0:
		out["basis"]["gas"] = "data"
	out["total"]["gas"] = gas_total
	out["government"]["gas"] = gas_total * STORAGE_GOVERNMENT_SHARE

	# --- petróleo: a reserva pública é do GOVERNO; o tanque privado é da
	#     indústria (regra declarada: meses de produção) ---
	var oil: Dictionary = _reserve_nations.get(tag, {}).get("oil", {})
	var oil_capacity_kt := _num(oil.get("capacity_kt"))
	var fuel_private := _fallback_capacity(nation_cities, weights, "fuel_tank")
	var fuel_gov := oil_capacity_kt * OIL_KT_TO_T
	# O estoque público DOCUMENTADO implica ao menos essa capacidade de tanque
	# (o volume existe em algum lugar); a capacidade publicada manda quando é
	# maior. Sem as duas, 0 — a regra do arquivo é "reserva inicial = 0".
	fuel_gov = maxf(fuel_gov,
		_num(oil.get("public_stocks_kt")) * OIL_KT_TO_T)
	if fuel_gov > 0.0:
		out["basis"]["fuel_tank"] = "data"
	out["total"]["fuel_tank"] = fuel_private + fuel_gov
	out["government"]["fuel_tank"] = fuel_gov

	# --- pátio e armazém geral: só regra declarada ---
	out["total"]["bulk_yard"] = _fallback_capacity(nation_cities, weights,
		"bulk_yard")
	out["government"]["bulk_yard"] = \
		float(out["total"]["bulk_yard"]) * STORAGE_GOVERNMENT_SHARE
	out["total"]["general"] = _fallback_capacity(nation_cities, weights,
		"general")
	out["government"]["general"] = \
		float(out["total"]["general"]) * STORAGE_GOVERNMENT_SHARE
	return out


## Regra declarada (espelho de `kStorageInitialMonths`): capacidade = produção
## anual dos bens que o tipo guarda × meses / 12. Sem produção do tipo, 0 — a
## ausência é explícita (a IA privada constrói depois, pela regra de payback).
func _fallback_capacity(nation_cities: Array, weights: Dictionary,
		type: String) -> float:
	var annual := 0.0
	for row in nation_cities:
		annual += float(weights.get(String(row["id"]), {}).get(type, 0.0))
	return annual * STORAGE_INITIAL_MONTHS / 12.0


## Rateio de `total` (privado + governo) entre as cidades pelo peso do tipo
## (produção → população → igual). Devolve `[{city, private, government,
## basis, source}]` com as somas EXATAS (a última cidade recebe o resto).
## `source` = de onde veio o número (`data` do JSON ou `rule` do núcleo);
## `basis` = como foi rateado entre as cidades.
func _split_capacity(total: float, government: float, nation_cities: Array,
		weights: Dictionary, type: String, source: String) -> Array:
	var rows: Array = []
	var sum := 0.0
	for city in nation_cities:
		var city_id := String(city["id"])
		var weight := float(weights.get(city_id, {}).get(type, 0.0))
		var row_basis := "production"
		if weight <= 0.0:
			weight = float(city.get("weight", 0.0))
			row_basis = "population"
		rows.append({"city": city_id, "weight": maxf(weight, 0.0),
			"basis": row_basis})
		sum += maxf(weight, 0.0)
	if rows.is_empty():
		return []
	if sum <= 0.0:
		# Sem produção e sem população: rateio igual (declarado).
		for row in rows:
			row["weight"] = 1.0
			row["basis"] = "equal"
		sum = float(rows.size())
	var private_total := maxf(total - government, 0.0)
	var private_parts := _cumulative_share(private_total, rows, sum)
	var government_parts := _cumulative_share(government, rows, sum)
	var out: Array = []
	for index in rows.size():
		var private_t := float(private_parts[index])
		var government_t := float(government_parts[index])
		if private_t <= 0.0 and government_t <= 0.0:
			continue
		out.append({"city": String(rows[index]["city"]), "private": private_t,
			"government": government_t, "basis": String(rows[index]["basis"]),
			"source": source, "total": private_t + government_t})
	return out


## Fatia cumulativa de `amount` pelos pesos; a última linha fecha a soma.
func _cumulative_share(amount: float, rows: Array, sum: float) -> Array:
	var parts: Array = []
	var assigned := 0.0
	for index in rows.size():
		var part := 0.0
		if index == rows.size() - 1:
			part = maxf(amount - assigned, 0.0)
		else:
			part = amount * float(rows[index]["weight"]) / sum
			assigned += part
		parts.append(part)
	return parts


# ---------------------------------------------------------------------------
# Reserva pública (semente do dia 1)
# ---------------------------------------------------------------------------

func _seed_oil(sim: Object, tag: String, splits: Dictionary,
		existing: Dictionary, report: Dictionary) -> void:
	var oil: Dictionary = _reserve_nations.get(tag, {}).get("oil", {})
	var stocks_t := _num(oil.get("public_stocks_kt")) * OIL_KT_TO_T
	if stocks_t <= 0.0:
		return
	var split: Array = splits.get("fuel_tank", [])
	var placed := _place_stock(sim, split, "fuel_tank", "oil", stocks_t,
		existing)
	report["initial_stock_t"]["oil"] = \
		float(report["initial_stock_t"].get("oil", 0.0)) + placed
	report["initial_stock_unplaced_t"]["oil"] = \
		float(report["initial_stock_unplaced_t"].get("oil", 0.0)) \
		+ maxf(stocks_t - placed, 0.0)


func _seed_grain(sim: Object, tag: String, nation_cities: Array,
		weights: Dictionary, splits: Dictionary, existing: Dictionary,
		report: Dictionary) -> void:
	var public: Dictionary = _reserve_nations.get(tag, {}).get("grain_public", {})
	var stocks_t := _num(public.get("stocks_t"))
	if stocks_t <= 0.0:
		return
	var split: Array = splits.get("silo", [])
	# O dado é AGREGADO (não diz o grão): rateio declarado pela participação da
	# produção de cada grão do silo na nação. Sem produção de grão, o estoque
	# público não tem produto a que pertencer (fica não acomodado).
	var grain_weight := 0.0
	var product_weight: Dictionary = {}
	for product in GRAIN_PRODUCTS:
		var total := 0.0
		for city in nation_cities:
			total += float(weights.get(String(city["id"]), {})
				.get("_product_" + product, 0.0))
		product_weight[product] = total
		grain_weight += total
	if grain_weight <= 0.0 or split.is_empty():
		report["initial_stock_unplaced_t"]["grain"] = \
			float(report["initial_stock_unplaced_t"].get("grain", 0.0)) + stocks_t
		report["grain_stock_basis"] = "unplaced_no_grain_production"
		return
	var placed := 0.0
	for product in GRAIN_PRODUCTS:
		var share := float(product_weight[product]) / grain_weight
		if share <= 0.0:
			continue
		placed += _place_stock(sim, split, "silo", String(product),
			stocks_t * share, existing)
	report["initial_stock_t"]["grain"] = \
		float(report["initial_stock_t"].get("grain", 0.0)) + placed
	report["initial_stock_unplaced_t"]["grain"] = \
		float(report["initial_stock_unplaced_t"].get("grain", 0.0)) \
		+ maxf(stocks_t - placed, 0.0)
	report["grain_stock_basis"] = "production_share"


## Distribui `tons` entre os sites de GOVERNO pela capacidade de cada um e grava
## no núcleo (`set_storage_stock`). Devolve quantas toneladas foram acomodadas.
func _place_stock(sim: Object, split: Array, type: String, product: String,
		tons: float, existing: Dictionary) -> float:
	var capacity := 0.0
	for row in split:
		if not existing.has(String(row["city"]) + "|" + type + "|0"):
			capacity += maxf(float(row["government"]), 0.0)
	if capacity <= 0.0:
		return 0.0
	var amount := minf(tons, capacity)
	var placed := 0.0
	var assigned := 0.0
	for index in split.size():
		var row: Dictionary = split[index]
		var city_id := String(row["city"])
		if existing.has(city_id + "|" + type + "|0"):
			continue
		var site_capacity := maxf(float(row["government"]), 0.0)
		if site_capacity <= 0.0:
			continue
		var part := amount * site_capacity / capacity
		# Última linha com capacidade fecha a soma (evita 1e-12 perdido).
		part = minf(part, amount - assigned)
		if placed + part >= amount - 1e-9:
			part = amount - assigned
		if part <= 0.0:
			continue
		sim.set_storage_stock(city_id, type, "government", product, part)
		placed += part
		assigned += part
		if assigned >= amount - 1e-9:
			break
	return placed


# ---------------------------------------------------------------------------
# Apoio
# ---------------------------------------------------------------------------

func _gold_tonnes(tag: String) -> float:
	var gold: Dictionary = _reserve_nations.get(tag, {}).get("gold", {})
	return maxf(_num(gold.get("tonnes")), 0.0)


func _cities_by_nation(cities: Array) -> Dictionary:
	var out: Dictionary = {}
	for city in cities:
		var row: Dictionary = city
		var tag := String(row.get("nation", ""))
		var city_id := String(row.get("id", ""))
		if tag == "" or city_id == "":
			continue
		var population := _num(row.get("sim_population"))
		if population <= 0.0:
			population = _num(row.get("population"))
		if not out.has(tag):
			out[tag] = []
		(out[tag] as Array).append({"id": city_id,
			"weight": maxf(population, 0.0)})
	return out


## `{ city_id: { tipo: produção_anual_t, "_product_<id>": produção_anual_t } }`
## a partir das fazendas/minas iniciais.
func _type_weights(buildings: Array) -> Dictionary:
	var out: Dictionary = {}
	for building in buildings:
		var row: Dictionary = building
		var city_id := String(row.get("city_id", ""))
		var product := String(row.get("resource", ""))
		if city_id == "" or product == "":
			continue
		var daily := _num(row.get("output_tons_per_day"))
		if daily <= 0.0:
			continue
		var annual := daily * YEAR_DAYS
		if not out.has(city_id):
			out[city_id] = {}
		var entry: Dictionary = out[city_id]
		entry["_product_" + product] = \
			float(entry.get("_product_" + product, 0.0)) + annual
		var type := ""
		if _goods.has(product):
			type = core_type(String(_goods[product].get("primary", "")))
		if type == "" or STRUCTURE_ONLY.has(type):
			continue
		entry[type] = float(entry.get(type, 0.0)) + annual
	return out


## `{ "cidade|tipo|dono": true }` do que JÁ existe no núcleo (load de save).
func _existing_sites(sim: Object) -> Dictionary:
	var out: Dictionary = {}
	if not sim.has_method("get_storage_status"):
		return out
	var status: Dictionary = sim.get_storage_status()
	var sites: Dictionary = status.get("sites", {})
	for key in sites.keys():
		var row: Dictionary = sites[key]
		var owner := "0" if String(row.get("owner", "")) == "government" else "1"
		out[String(row.get("city", "")) + "|" + String(row.get("type", "")) \
			+ "|" + owner] = true
	return out


func _add_source(report: Dictionary, type: String, source: String,
		tons: float) -> void:
	var row: Dictionary = report["capacity_source"][type]
	row[source] = float(row.get(source, 0.0)) + tons


func _empty_report(options: Dictionary) -> Dictionary:
	var capacity_source: Dictionary = {}
	var rateio_basis: Dictionary = {}
	for type in OPERATIONAL_TYPES:
		capacity_source[type] = {}
		rateio_basis[type] = {}
	return {
		"applied": false,
		"only_missing": bool(options.get("only_missing", false)),
		"nations": 0,
		"cities": 0,
		"product_types_applied": 0,
		"product_types_total": 0,
		"product_types_skipped": [],
		"sites_written": 0,
		"skipped_existing_sites": 0,
		"capacity_t": {},
		"government_capacity_t": {},
		"private_capacity_t": {},
		"capacity_source": capacity_source,
		"rateio_basis": rateio_basis,
		"capacity_unplaced_t": {},
		"game_annual_t": {},
		"months_of_game_production": {},
		"initial_stock_t": {},
		"initial_stock_unplaced_t": {},
		"grain_stock_basis": "",
		"gold_reserve_t": 0.0,
		"gold_nations": {},
		"gold_applied": false,
		"gold_reason": "vault e estrutura nesta parte (D-100); ouro do BC nao tem fluxo de estoque",
		"conversions": {
			"grain_mt_to_t": GRAIN_MT_TO_T,
			"oil_kt_to_t": OIL_KT_TO_T,
			"cold_m3_per_ton": COLD_M3_PER_TON,
			"gas_twh_to_t": GAS_TWH_TO_TONS,
		},
		"rules": {
			"initial_months": STORAGE_INITIAL_MONTHS,
			"government_share": STORAGE_GOVERNMENT_SHARE,
		},
		"coverage": _coverage.duplicate(true),
	}


static func _num(value: Variant) -> float:
	if value == null:
		return 0.0
	match typeof(value):
		TYPE_FLOAT, TYPE_INT:
			return float(value)
		TYPE_STRING:
			return float(value) if String(value).is_valid_float() else 0.0
	return 0.0


func _read_dict(path: String, key: String) -> Dictionary:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		push_warning("StorageService: não abriu " + path)
		return {}
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	if typeof(data) != TYPE_DICTIONARY:
		push_warning("StorageService: JSON inválido " + path)
		return {}
	var block: Variant = (data as Dictionary).get(key, {})
	return block if typeof(block) == TYPE_DICTIONARY else {}
