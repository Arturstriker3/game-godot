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
## ESCALA (D-101, decisão do coordenador; mesmo princípio dos portos): o volume
## do jogo ainda NÃO está na escala real, então a tonelagem real NÃO é aplicada
## como absoluta. O dado real entra como COBERTURA — meses de produção
## (`grain.months_of_production`) ou dias de importação/consumo
## (`oil.days_net_imports`, `grain_public.stocks_t / consumo`) — e a cobertura é
## aplicada ao FLUXO DO JOGO do país e do tipo (produção anual das fazendas e
## minas iniciais, a mesma que a agricultura usa):
##
##   capacidade_jogo = cobertura_real (meses) × fluxo_jogo_anual / 12
##   reserva_jogo    = cobertura_real (dias)   × fluxo_jogo_anual / 365
##
## Quando a produção do jogo for calibrada para a escala real, os números
## absolutos coincidem. Sem cobertura real para o tipo (câmara fria, pátio,
## armazém geral, tanque privado) vale a REGRA DECLARADA do núcleo
## (`kStorageInitialMonths`, espelhado em `STORAGE_INITIAL_MONTHS`); sem fluxo do
## jogo para o tipo, a capacidade é 0 e a ausência fica reportada (a IA privada
## constrói depois, pela regra de payback). O relatório publica, por tipo, a
## cobertura usada, o fluxo do jogo e os meses de cobertura RESULTANTES — a
## dívida de escala fica medida, não escondida.
##
## OURO do BC: continua em TONELADAS REAIS e não é semeado — não é fluxo de
## produção do jogo; o cofre é estrutura nesta parte (D-100).
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
## Denominadores de COBERTURA real (D-101): produção de gás 2024 (kt/ano) e
## consumo de grãos por país (para transformar estoque público em dias).
const EXTRACTION_PATH := "res://data/definitions/natural/extraction_production_2024.json"
const CROP_STOCKS_PATH := "res://data/definitions/agriculture/crop_stocks.json"
const EXTRACTION_KEY := "commodities"
const CROP_STOCKS_KEY := "nations"

## Espelhos das constantes do núcleo (`gdextension/src/sim/sim_constants.h`).
## R0.3: são FALLBACK de binário antigo — `_read_core_constants()` consome a
## API `EconomySim.get_storage_constants()` e sobrescreve estes valores.
const STORAGE_INITIAL_MONTHS := 2.0      # kStorageInitialMonths
const STORAGE_GOVERNMENT_SHARE := 0.25   # kStorageGovernmentShare
const YEAR_DAYS := 365.0

## Conversões de unidade que o JSON não declara. MODELO DECLARADO (calibrável),
## não dado:
##   * frio: 4 m³ de câmara fria por tonelada guardada (pallet de congelado);
##   * gás: 1 t de GNL = 14,28 MWh (48,7 MMBtu) → 1 TWh = 1e6/14,28 = 70.028 t.
##     `extraction_production_2024` publica gás em kt com 1 t = 51,0 GJ → a
##     conversão declarada do próprio arquivo é 70,5 kt por TWh.
const COLD_M3_PER_TON := 4.0
const GAS_TWH_TO_TONS := 70028.0
const GAS_KT_PER_TWH := 70.5
const GRAIN_MT_TO_T := 1_000_000.0
const OIL_KT_TO_T := 1_000.0
const DAYS_PER_YEAR := 365.0

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
var _extraction: Dictionary = {}
var _crop_stocks: Dictionary = {}
var _loaded := false
## R0.3: valores efetivos (API do núcleo quando presente; senão os fallbacks).
var _initial_months := STORAGE_INITIAL_MONTHS
var _government_share := STORAGE_GOVERNMENT_SHARE


## R0.3 — lê as constantes de armazenagem do NÚCLEO (fonte: sim_constants.h).
## As constantes do topo ficam só como fallback de binário antigo.
func _read_core_constants() -> void:
	if not ClassDB.class_exists("EconomySim"):
		return
	var values: Dictionary = EconomySim.get_storage_constants()
	_initial_months = float(values.get("initial_months",
		STORAGE_INITIAL_MONTHS))
	_government_share = float(values.get("government_share",
		STORAGE_GOVERNMENT_SHARE))


## Carrega os JSONs. Idempotente. Devolve false quando os três da G2 faltam — o
## chamador segue sem G2 no GDScript e o núcleo mantém a tabela declarada. Os
## dois arquivos de DENOMINADOR (extração e consumo de grãos) são opcionais: sem
## eles a cobertura daquele tipo cai na regra declarada (reportado).
func load_data() -> bool:
	if _loaded:
		return true
	_capacity_nations = _read_dict(CAPACITY_PATH, "nations")
	_reserve_nations = _read_dict(RESERVES_PATH, "nations")
	_goods = _read_dict(MAP_PATH, "goods")
	_coverage = _read_dict(MAP_PATH, "coverage")
	_extraction = _read_dict(EXTRACTION_PATH, EXTRACTION_KEY)
	_crop_stocks = _read_dict(CROP_STOCKS_PATH, CROP_STOCKS_KEY)
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
	_read_core_constants()
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
			var flow := float(capacity["flow"].get(type, 0.0))
			var months := float(capacity["coverage"].get(type, 0.0))
			var basis := String(capacity["basis"].get(type, "none"))
			report["game_annual_t"][type] = \
				float(report["game_annual_t"].get(type, 0.0)) + flow
			report["coverage_weighted"][type] = \
				float(report["coverage_weighted"].get(type, 0.0)) + months * flow
			if months > 0.0 and flow <= 0.0:
				report["flow_zero_nations"][type] = \
					int(report["flow_zero_nations"].get(type, 0)) + 1
			var total := float(capacity["total"].get(type, 0.0))
			if total <= 0.0:
				continue
			var government := minf(float(capacity["government"].get(type, 0.0)),
				total)
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
		_add_real_data(report, tag)
		# 4. Reserva pública do dia 1 (petróleo e grãos públicos do dado,
		#    aplicados como DIAS de cobertura sobre o fluxo do jogo).
		if seed_stock:
			_seed_oil(sim, tag, splits, existing, capacity, report)
			_seed_grain(sim, tag, nation_cities, weights, splits, existing,
				capacity, report)
		# 5. Ouro do BC: lido e reportado; desde a C2b2 (D-106) também é
		#    colocado como OURO FÍSICO no cofre (passo 5b).
		var gold := _gold_tonnes(tag)
		report["gold_reserve_t"] = float(report["gold_reserve_t"]) + gold
		if gold > 0.0:
			report["gold_nations"][tag] = gold
		# 5b. C2b2 (D-106): ouro FÍSICO. As toneladas do dado real entram no
		#     cofre do governo da maior cidade da nação (a capacidade do cofre
		#     é a própria tonelagem). No load o cofre do save manda: um site já
		#     existente não é reabastecido com o dado.
		if gold > 0.0 and seed_stock:
			var vault_city := _largest_city(nation_cities)
			if vault_city != "" \
					and not existing.has(vault_city + "|vault|0"):
				sim.set_storage_capacity(vault_city, "vault", "government",
					gold)
				sim.set_storage_stock(vault_city, "vault", "government",
					"gold", gold)
				report["gold_placed_t"] = \
					float(report["gold_placed_t"]) + gold
				report["gold_city"][tag] = vault_city
			else:
				report["gold_unplaced_t"] = \
					float(report["gold_unplaced_t"]) + gold
		elif gold > 0.0:
			report["gold_unplaced_t"] = \
				float(report["gold_unplaced_t"]) + gold
	report["nations"] = by_nation.size()
	report["cities"] = cities.size()
	# 6. C2a (D-103): o ouro do cofre vira a reserva do banco central DA MOEDA.
	#    É medição sobre dado real (D-101) — não cria ouro nem muda caixa.
	#    `gold_nations` está por nação; o núcleo agrega pela moeda. O contrato da
	#    D-101 fica intacto: `gold_applied` continua false (o ouro NÃO entra como
	#    bem físico negociado); o que entra é o ativo do BC, medido.
	if sim.has_method("set_central_bank_gold"):
		var gold_declared := 0.0
		if not (report["gold_nations"] as Dictionary).is_empty():
			gold_declared = float(sim.set_central_bank_gold(
				report["gold_nations"]))
		report["gold_central_bank_t"] = gold_declared
	# 6b. C2b2 (D-106): o cofre deixa de ser "só estrutura" para o bem gold.
	#     `gold_applied` diz se a tonelada do dado virou estoque físico.
	if float(report["gold_placed_t"]) > 0.0:
		report["gold_applied"] = true
		report["gold_reason"] = ""
	# Diagnóstico de ESCALA: produção anual do jogo por tipo, cobertura real
	# usada (meses, ponderada pelo fluxo do jogo) e quantos meses de produção a
	# capacidade inicial cobre. Depois da D-101 os dois últimos coincidem por
	# construção (capacidade = cobertura × fluxo / 12); a diferença entre eles
	# mede o erro da regra declarada onde não há cobertura real.
	for type in OPERATIONAL_TYPES:
		var annual := float(report["game_annual_t"].get(type, 0.0))
		var capacity_t := float(report["capacity_t"].get(type, 0.0))
		report["coverage_months"][type] = \
			float(report["coverage_weighted"].get(type, 0.0)) / annual \
			if annual > 0.0 else 0.0
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
## cada número: `data` (cobertura real do JSON) ou `rule` (regra do núcleo —
## meses de produção — porque o JSON não tem a cobertura).
##
## D-101: capacidade = COBERTURA REAL (meses) × fluxo do jogo do tipo / 12. O
## fluxo é a produção anual das fazendas/minas iniciais da nação (a mesma base
## do rateio entre cidades). Preencher a tonelagem real como absoluta deixava
## 89 meses de cobertura num mundo cujo volume ainda é 1/100 do real; a
## cobertura é a grandeza que independe da escala.
func _nation_capacity(tag: String, nation_cities: Array,
		weights: Dictionary) -> Dictionary:
	var out := {"total": {}, "government": {}, "basis": {}, "coverage": {},
		"flow": {}, "government_days": {}}
	for type in OPERATIONAL_TYPES:
		var flow := _annual_flow(nation_cities, weights, type)
		out["flow"][type] = flow
		if type == "fuel_tank":
			# Tanque: o PRIVADO não tem cobertura publicada (regra de meses); a
			# reserva pública tem DIAS de importação (dado) — as duas somam.
			var private_t := _rule_capacity(flow, _initial_months)
			var public_t := _oil_public_capacity(tag, flow, out)
			out["total"]["fuel_tank"] = private_t + public_t
			out["government"]["fuel_tank"] = public_t
			out["basis"]["fuel_tank"] = "data" if public_t > 0.0 else "rule"
			out["coverage"]["fuel_tank"] = \
				(private_t + public_t) * 12.0 / flow if flow > 0.0 else 0.0
			continue
		var coverage := _coverage_months(tag, type)
		var months := float(coverage["months"])
		out["coverage"][type] = months
		out["basis"][type] = String(coverage["basis"])
		var total := _rule_capacity(flow, months)
		out["total"][type] = total
		var government := -1.0
		if type == "silo":
			government = _grain_public_capacity(tag, flow, out)
		if government < 0.0:
			government = total * _government_share
		out["government"][type] = minf(maxf(government, 0.0), total)
	return out


## COBERTURA REAL em meses de produção do tipo (D-101). `basis` diz de onde
## veio: `data` (meses calculados pela fonte a partir da capacidade e da
## produção real), `rule` (regra declarada do núcleo) ou `none` (o arquivo diz
## que o jogo começa com 0 — gás sem capacidade documentada).
func _coverage_months(tag: String, type: String) -> Dictionary:
	var capacity: Dictionary = _capacity_nations.get(tag, {})
	if type == "silo":
		var grain: Dictionary = capacity.get("grain", {})
		var months := _num(grain.get("months_of_production"))
		if months > 0.0:
			return {"months": months, "basis": "data"}
		return {"months": _initial_months, "basis": "rule"}
	if type == "gas":
		var gas: Dictionary = capacity.get("gas", {})
		var twh := _num(gas.get("working_capacity_twh"))
		var production_twh := _real_production_kt(tag, "gas") / GAS_KT_PER_TWH
		if twh > 0.0 and production_twh > 0.0:
			return {"months": twh / production_twh * 12.0, "basis": "data"}
		# Regra declarada do arquivo: sem capacidade documentada o jogo começa
		# com 0 e o armazém é construído (não há número a inventar).
		return {"months": 0.0, "basis": "none"}
	# Câmara fria, pátio, armazém geral e tanque privado: sem base publicada de
	# produção/consumo real do tipo por nação no repositório (o frio tem m³,
	# não a produção de perecíveis; o pátio e o armazém geral não têm
	# capacidade observada). Regra declarada do núcleo.
	return {"months": _initial_months, "basis": "rule"}


## Capacidade de GOVERNO do silo a partir da rede pública de grãos (Índia,
## Indonésia, Filipinas): capacidade publicada ÷ consumo real de grãos do país
## = dias; os dias valem sobre o fluxo do jogo. Sem consumo publicado, volta
## para a fatia declarada (-1).
func _grain_public_capacity(tag: String, flow: float, out: Dictionary) -> float:
	var public: Dictionary = _reserve_nations.get(tag, {}).get("grain_public", {})
	var capacity_t := _num(public.get("capacity_t"))
	var consumption := _grain_consumption(tag)
	if capacity_t <= 0.0 or consumption <= 0.0 or flow <= 0.0:
		return -1.0
	var days := capacity_t / consumption * DAYS_PER_YEAR
	out["government_days"]["silo"] = days
	return days / DAYS_PER_YEAR * flow


## Capacidade de GOVERNO do tanque a partir da reserva pública de petróleo: os
## DIAS de importação líquida publicados dão a cobertura da reserva; a
## capacidade do tanque guarda a mesma razão capacidade/estoque do dado.
func _oil_public_capacity(tag: String, flow: float, out: Dictionary) -> float:
	var oil: Dictionary = _reserve_nations.get(tag, {}).get("oil", {})
	var days := _num(oil.get("days_net_imports"))
	if days <= 0.0 or flow <= 0.0:
		return 0.0
	var capacity_kt := _num(oil.get("capacity_kt"))
	var stocks_kt := _num(oil.get("public_stocks_kt"))
	var capacity_days := days
	if capacity_kt > 0.0 and stocks_kt > 0.0:
		capacity_days = days * capacity_kt / stocks_kt
	out["government_days"]["fuel_tank"] = capacity_days
	return capacity_days / DAYS_PER_YEAR * flow


## FLUXO do jogo: produção anual (t) dos bens que o tipo guarda, somada nas
## fazendas/minas iniciais da nação. É o volume ao qual a cobertura real se
## aplica; sem ele, a capacidade é 0 (ausência explícita) e a IA constrói
## depois pela regra de payback.
func _annual_flow(nation_cities: Array, weights: Dictionary,
		type: String) -> float:
	var annual := 0.0
	for row in nation_cities:
		annual += float(weights.get(String(row["id"]), {}).get(type, 0.0))
	return annual


## Regra declarada (espelho de `kStorageInitialMonths`): capacidade = fluxo
## anual do jogo × meses / 12. Sem fluxo, 0.
func _rule_capacity(flow: float, months: float) -> float:
	if flow <= 0.0 or months <= 0.0:
		return 0.0
	return flow * months / 12.0


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
		existing: Dictionary, capacity: Dictionary, report: Dictionary) -> void:
	var oil: Dictionary = _reserve_nations.get(tag, {}).get("oil", {})
	var days := _num(oil.get("days_net_imports"))
	var flow := float(capacity["flow"].get("fuel_tank", 0.0))
	if days <= 0.0 or flow <= 0.0:
		# Sem dias publicados ou sem fluxo do jogo para escalar: a reserva
		# pública não é aplicada (nada de tonelada absoluta sem escala).
		if _num(oil.get("public_stocks_kt")) > 0.0:
			report["reserve_unscaled"][tag] = "oil"
		return
	var stocks_t := days / DAYS_PER_YEAR * flow
	var split: Array = splits.get("fuel_tank", [])
	var placed := _place_stock(sim, split, "fuel_tank", "oil", stocks_t,
		existing)
	report["initial_stock_t"]["oil"] = \
		float(report["initial_stock_t"].get("oil", 0.0)) + placed
	report["initial_stock_unplaced_t"]["oil"] = \
		float(report["initial_stock_unplaced_t"].get("oil", 0.0)) \
		+ maxf(stocks_t - placed, 0.0)
	report["initial_stock_days"]["oil"] = days
	report["initial_stock_basis"]["oil"] = "days_net_imports"


func _seed_grain(sim: Object, tag: String, nation_cities: Array,
		weights: Dictionary, splits: Dictionary, existing: Dictionary,
		capacity: Dictionary, report: Dictionary) -> void:
	var public: Dictionary = _reserve_nations.get(tag, {}).get("grain_public", {})
	var stocks_real := _num(public.get("stocks_t"))
	var consumption := _grain_consumption(tag)
	var flow := float(capacity["flow"].get("silo", 0.0))
	if stocks_real <= 0.0:
		return
	if consumption <= 0.0 or flow <= 0.0:
		report["reserve_unscaled"][tag] = "grain"
		return
	# COBERTURA: o estoque público real cobre X dias do consumo real do país; X
	# dias valem sobre o fluxo de grãos DO JOGO (D-101).
	var days := stocks_real / consumption * DAYS_PER_YEAR
	var stocks_t := days / DAYS_PER_YEAR * flow
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
	report["initial_stock_days"]["grain"] = days
	report["initial_stock_basis"]["grain"] = "days_of_consumption"
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


## C2b2 (D-106): maior cidade da nação (a que recebe o cofre de ouro). O
## desempate é o id, para a escolha ser determinística.
func _largest_city(nation_cities: Array) -> String:
	var best := ""
	var best_pop := -1.0
	for city in nation_cities:
		if city is not Dictionary:
			continue
		var id := String((city as Dictionary).get("id", ""))
		if id == "":
			continue
		var pop := float((city as Dictionary).get("simulation_population",
			(city as Dictionary).get("population", 0.0)))
		if pop > best_pop + 1e-9 or (absf(pop - best_pop) <= 1e-9
				and (best == "" or id < best)):
			best = id
			best_pop = pop
	return best


## Produção real anual (kt) de um bem mineral do país
## (`extraction_production_2024`, publicado em kt/ano). Zero quando o país não
## aparece — ausência explícita.
func _real_production_kt(tag: String, commodity: String) -> float:
	var block: Dictionary = _extraction.get(commodity, {})
	var countries: Dictionary = block.get("countries", {})
	return maxf(_num(countries.get(tag)), 0.0)


## Consumo real anual de grãos do país (t), soma dos quatro grãos do silo em
## `crop_stocks` (USDA PSD Domestic Consumption, ou a regra declarada do
## arquivo). É o denominador que transforma estoque/estoque público em DIAS.
func _grain_consumption(tag: String) -> float:
	var crops: Dictionary = _crop_stocks.get(tag, {}).get("crops", {})
	var total := 0.0
	for product in GRAIN_PRODUCTS:
		total += maxf(_num((crops.get(product, {}) as Dictionary)
			.get("consumption_t")), 0.0)
	return total


## Soma no relatório a MAGNITUDE real lida para a nação (mesmo quando ela não
## pode ser aplicada por falta de fluxo do jogo): a dívida de escala fica
## visível em vez de desaparecer.
func _add_real_data(report: Dictionary, tag: String) -> void:
	var capacity: Dictionary = _capacity_nations.get(tag, {})
	var oil: Dictionary = _reserve_nations.get(tag, {}).get("oil", {})
	var real: Dictionary = report["real_data"]
	real["grain_mt"] = float(real.get("grain_mt", 0.0)) \
		+ _num((capacity.get("grain", {}) as Dictionary).get("capacity_mt"))
	real["cold_m3"] = float(real.get("cold_m3", 0.0)) \
		+ _num((capacity.get("cold", {}) as Dictionary).get("capacity_m3"))
	real["gas_twh"] = float(real.get("gas_twh", 0.0)) \
		+ _num((capacity.get("gas", {}) as Dictionary)
			.get("working_capacity_twh"))
	real["oil_capacity_kt"] = float(real.get("oil_capacity_kt", 0.0)) \
		+ _num(oil.get("capacity_kt"))
	real["oil_stocks_kt"] = float(real.get("oil_stocks_kt", 0.0)) \
		+ _num(oil.get("public_stocks_kt"))


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
		"coverage_weighted": {},
		"coverage_months": {},
		"months_of_game_production": {},
		"flow_zero_nations": {},
		"real_data": {},
		"initial_stock_t": {},
		"initial_stock_unplaced_t": {},
		"initial_stock_days": {},
		"initial_stock_basis": {},
		"reserve_unscaled": {},
		"grain_stock_basis": "",
		"gold_reserve_t": 0.0,
		"gold_nations": {},
		"gold_placed_t": 0.0,
		"gold_unplaced_t": 0.0,
		"gold_city": {},
		"gold_applied": false,
		"gold_reason": "vault e estrutura nesta parte (D-100); ouro do BC nao tem fluxo de estoque",
		"gold_central_bank_t": 0.0,
		"conversions": {
			"grain_mt_to_t": GRAIN_MT_TO_T,
			"oil_kt_to_t": OIL_KT_TO_T,
			"cold_m3_per_ton": COLD_M3_PER_TON,
			"gas_twh_to_t": GAS_TWH_TO_TONS,
			"gas_kt_per_twh": GAS_KT_PER_TWH,
		},
		"rules": {
			"initial_months": _initial_months,
			"government_share": _government_share,
			"scale": "cobertura_real x fluxo_do_jogo (D-101)",
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
