extends SceneTree

## C1 — mundo real com IA por N dias, com a contabilidade por balanço ligada.
##
##   godot --headless --path . --script res://tools/c1_world_ai_365.gd -- \
##     [--days=365] [--out=docs/economy/perf/c1_world_ai_365.json]
##
## Monta o mundo pelo caminho do AUTOLOAD (cidades + geografia real + infra B5
## inicial + camada urbana + B7 + agricultura sazonal + armazenagem G2 + IA do
## núcleo, `threads=1`) e mede o aceite da C1:
##   * INVARIANTE FINANCEIRO do dia (soma das posições = 0) e conservação de
##     caixa — 0 violações;
##   * falências e dias travados;
##   * drift máximo do caixa contra o dia 0;
##   * PORTÃO DE COMIDA do M2 (média de 30 d de consumido/necessidade por nação,
##     < 0,975 = fora) — a base da D-101 é 85/179;
##   * BALANÇO por nação (PL das famílias/firmas/governo, patrimônio B6,
##     estoque G2, salário atrasado e liquidez × solvência das plantas).
##
## Só medição: não move dinheiro nem muda regra. `--days` curto serve de fumaça.

const GeoScript := preload("res://core/economy/geography_service.gd")
const LandPropertyScript := preload("res://core/economy/land_property.gd")
const AgriScript := preload("res://core/economy/agriculture_service.gd")
const StorageScript := preload("res://core/economy/storage_service.gd")
const PowerScript := preload("res://core/economy/power_service.gd")
const RegionDataScript := preload("res://core/economy/region_data.gd")
const TrajectoryDigest := preload("res://core/economy/trajectory_digest.gd")

const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const PORTS_PATH := "res://data/definitions/scenario/ports.json"
const BUILDINGS_PATH := "res://data/definitions/scenario/buildings.json"
const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"
const SERVICES_PATH := "res://data/definitions/economy/services.json"
const CURRENCIES_PATH := "res://data/definitions/economy/currencies.json"
const FX_REGIMES_PATH := "res://data/definitions/economy/fx_regimes_2026.json"
const SECTOR_EMPLOYMENT_PATH := "res://data/definitions/economy/sector_employment.json"
const AGE_STRUCTURE_PATH := "res://data/definitions/population/age_structure_2026.json"
const INFORMALITY_PATH := "res://data/definitions/economy/informality.json"
const HEALTH_ACCESS_PATH := "res://data/definitions/population/health_access.json"
const URBAN_LAND_PATH := "res://data/definitions/economy/urban_land.json"
const HOUSING_CITY_PATH := "res://data/definitions/economy/housing_city.json"
const HOUSING_PATH := "res://data/definitions/economy/housing.json"
const MUNICIPAL_FINANCE_PATH := "res://data/definitions/economy/municipal_finance.json"
# C4 (D-110): bancos com dono (nomes fictícios/genéricos; presença do estrangeiro).
const BANKS_PATH := "res://data/definitions/economy/banks.json"

const GATE := 0.975
const WINDOW := 30
# E2/E3/E5 (D-117) — diagnóstico fino: a lista de edifícios é fotografada a
# cada N dias e sempre no dia em que o contador de falências muda, para nomear
# a firma que fechou (o núcleo apaga o edifício no tick, então só um retrato
# anterior permite o registro por entidade).
const BANKRUPTCY_SNAPSHOT_DAYS := 5
# Calibração pré-C6 (D-113): a política monetária é amostrada ao longo do ano
# (trajetória MENSAL da mediana da taxa e da inflação), não só no fecho. Sem
# isso o colapso da taxa no 1º ano só aparecia como um número final.
const MONETARY_SAMPLE_DAYS := [7, 15, 30, 45, 60, 63, 90, 120, 150, 180, 210,
	240, 270, 300, 330, 365]
const SAMPLE_NATIONS: Array[String] = ["USA", "CHN", "IND", "BRA", "DEU", "NGA",
	"RUS", "IDN", "MEX", "ZAF"]
# E2/E3 (D-117): nações do diagnóstico fino de energia (QA t80) — sempre no
# bloco `energy_economy.sample`, além do topo/base por conta/PIB.
const ENERGY_SAMPLE_NATIONS: Array[String] = ["USA", "CHN", "IND", "BRA", "DEU",
	"GBR", "DNK", "BGD", "THA", "ITA", "FRA", "NGA", "JPN", "CAN", "MEX",
	"NLD", "BEL", "NOR", "SWE", "IRL", "LAO", "MYS"]

var _days := 365
var _property := false
var _out := "res://docs/economy/perf/c1_world_ai_365.json"
var _fx_seed_days := 0
# C3 (D-107) + D-111: forçar o crédito de empresa por flag. Desde a D-111 o
# default do jogo é LIGADO; a flag continua servindo ao A/B (--credit=0 desliga).
# Só laboratório: não existe botão/atalho/HUD para isso no jogo.
var _credit := false
var _credit_set := false
# C4 (D-110) + D-111: A/B do CANAL do juro no envelope de crédito da safra
# (D-105). Desde a D-111 o default do jogo é LIGADO; --harvest-rate=0 desliga.
var _harvest_rate := false
var _harvest_rate_set := false
# E2/E3 (D-117): controle do portão de comida (JSON de uma rodada SEM energia)
# para separar as nações que a energia leva ao limiar das que já estavam lá.
var _control_path := ""
# Metadados de goods.json: setor, se é comida e o tipo (raw/processed).
var _goods_sector: Dictionary = {}
var _goods_food: Dictionary = {}
var _goods_kind: Dictionary = {}


func _init() -> void:
	# Permite rodar pelo runner canônico (tools/run_tests.ps1), que não passa
	# argumentos de usuário: `OGS_C1_OUT` escolhe o arquivo de saída.
	if OS.get_environment("OGS_C1_OUT") != "":
		_out = OS.get_environment("OGS_C1_OUT")
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--days="):
			_days = maxi(1, int(arg.split("=", true, 1)[1]))
		elif arg.begins_with("--property="):
			_property = arg.split("=", true, 1)[1] != "0"
		elif arg.begins_with("--out="):
			_out = arg.split("=", true, 1)[1]
		elif arg.begins_with("--credit="):
			_credit = arg.split("=", true, 1)[1] != "0"
			_credit_set = true
		elif arg.begins_with("--harvest-rate="):
			_harvest_rate = arg.split("=", true, 1)[1] != "0"
			_harvest_rate_set = true
		elif arg.begins_with("--fx-seed-days="):
			# C2b2 (D-106): cenário de estresse — funda as reservas de cada
			# moeda com N dias de importação MEDIDA do mundo e o peg passa a ser
			# defendido (reserva esgotada = desvalorização forçada).
			_fx_seed_days = maxi(0, int(arg.split("=", true, 1)[1]))
		elif arg.begins_with("--control="):
			_control_path = arg.split("=", true, 1)[1]
	_run()


## PARTE E (D-116) — linha do dia de energia (agregados do mundo).
func _power_sample(day: int, power: Dictionary) -> Dictionary:
	var nations: Dictionary = power.get("nations", {})
	var grids := 0
	var connected := 0
	var partial := 0
	var isolated := 0
	for tag: Variant in nations:
		var row: Dictionary = nations[tag]
		if not bool(row.get("exists", false)):
			continue
		grids += 1
		connected += int(row.get("connected", 0))
		partial += int(row.get("partial", 0))
		isolated += int(row.get("isolated", 0))
	return {
		"day": day,
		"price": float(power.get("average_price", 0.0)),
		"served_ratio": float(power.get("served_ratio", 0.0)),
		"demand_mwh": float(power.get("demand_mwh", 0.0)),
		"served_mwh": float(power.get("served_mwh", 0.0)),
		"rationing_cities": int(power.get("rationing_cities", 0)),
		"max_ration_days": int(power.get("max_ration_days", 0)),
		"loss_pct": float(power.get("loss_pct", 0.0)),
		"interconnect_flow_mwh": float(power.get("interconnect_flow_mwh", 0.0)),
		"interconnect_value": float(power.get("interconnect_value", 0.0)),
		"grids": grids,
		"connected": connected,
		"partial": partial,
		"isolated": isolated,
	}


## Fração simulada por nação (D-116): população simulada ÷ população exibida da
## região modelada, com o mesmo clamp [0,1] de `sim/power.cpp`. Parque, demanda
## e capacidade de linha são multiplicados por ela; publicá-la ao lado do
## servido deixa o diagnóstico auditável (déficit × fração simulada).
func _nation_scale(sim: Object) -> Dictionary:
	var sim_pop: Dictionary = {}
	var shown_pop: Dictionary = {}
	var status: Dictionary = sim.get_status()
	for city_row: Dictionary in (status.get("cities", []) as Array):
		var tag := String(city_row.get("nation", ""))
		var pop := float(city_row.get("population", 0.0))
		var sim_p := float(city_row.get("simulation_population", 0.0))
		if tag.is_empty() or pop <= 0.0 or sim_p <= 0.0:
			continue
		sim_pop[tag] = float(sim_pop.get(tag, 0.0)) + sim_p
		shown_pop[tag] = float(shown_pop.get(tag, 0.0)) + pop
	var out: Dictionary = {}
	for tag: Variant in sim_pop:
		var shown := float(shown_pop.get(tag, 0.0))
		out[tag] = clampf(float(sim_pop[tag]) / shown, 0.0, 1.0) \
			if shown > 0.0 else 1.0
	return out


## PARTE E (D-116) — digest do ano: preço médio ponderado pelo servido, % de
## demanda servida, racionamento (pico e nações no fecho), conexão por cidade e
## geração por fonte. O top 5 por demanda vem com a capacidade da matriz Ember
## (dado de E1) ao lado — capacidade não é geração, a comparação é declarada.
func _power_digest(sim, samples: Array) -> Dictionary:
	if samples.is_empty():
		return {"ready": false}
	var demand := 0.0
	var served := 0.0
	var bill := 0.0
	var max_ration := 0
	var days_rationing := 0
	var min_ratio := 1.0
	for row: Dictionary in samples:
		demand += float(row.get("demand_mwh", 0.0))
		served += float(row.get("served_mwh", 0.0))
		bill += float(row.get("price", 0.0)) * float(row.get("served_mwh", 0.0))
		max_ration = maxi(max_ration, int(row.get("rationing_cities", 0)))
		if int(row.get("rationing_cities", 0)) > 0:
			days_rationing += 1
		min_ratio = minf(min_ratio, float(row.get("served_ratio", 1.0)))
	var final: Dictionary = sim.get_power_status()
	var nations: Dictionary = final.get("nations", {})
	var grids := 0
	var connected := 0
	var partial := 0
	var isolated := 0
	var rationing: Array = []
	var by_nation: Array = []
	for tag: Variant in nations:
		var row2: Dictionary = nations[tag]
		if not bool(row2.get("exists", false)):
			continue
		grids += 1
		connected += int(row2.get("connected", 0))
		partial += int(row2.get("partial", 0))
		isolated += int(row2.get("isolated", 0))
		by_nation.append({
			"nation": String(tag),
			"price": float(row2.get("price", 0.0)),
			"demand_mwh": float(row2.get("demand_mwh", 0.0)),
			"served_mwh": float(row2.get("served_mwh", 0.0)),
			"served_ratio": float(row2.get("served_ratio", 0.0)),
			"ration_days": int(row2.get("ration_days", 0)),
			"connected": int(row2.get("connected", 0)),
			"partial": int(row2.get("partial", 0)),
			"isolated": int(row2.get("isolated", 0)),
			"import_mwh": float(row2.get("import_mwh", 0.0)),
			"export_mwh": float(row2.get("export_mwh", 0.0)),
			"import_value": float(row2.get("import_value", 0.0)),
			"export_value": float(row2.get("export_value", 0.0)),
			"interconnect_count": int(row2.get("interconnect_count", 0)),
			"generation_mwh_by_fuel": row2.get("generation_mwh_by_fuel", {}),
		})
		if float(row2.get("served_ratio", 1.0)) < 0.995:
			rationing.append(String(tag))
	rationing.sort()
	by_nation.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return float(a["served_mwh"]) > float(b["served_mwh"]))
	# Âncora nacional (D-116): o dado Ember do ano-base ao lado do modelo, para
	# cada nação com rede. Sem isso o JSON não permite separar importador real
	# de déficit de capacidade/dado.
	var anchors: Dictionary = _ember_demand()
	var ember: Dictionary = _ember_capacity()
	# Fração simulada por nação (D-116): a MESMA razão que `sim/power.cpp` usa
	# para escalar parque, demanda e linhas (população simulada ÷ população
	# exibida da região modelada). Publicada para o diagnóstico ser auditável:
	# sem ela não se separa "déficit" de "fração simulada".
	var scale_rows: Dictionary = _nation_scale(sim)
	var span_days := float(maxi(1, samples.size()))
	for row5: Dictionary in by_nation:
		var tag5 := String(row5["nation"])
		var anchor: Variant = anchors.get(tag5, null)
		if typeof(anchor) == TYPE_DICTIONARY:
			var ember_row: Dictionary = anchor
			row5["ember_year"] = int(ember_row.get("year", 0))
			row5["ember_demand_twh"] = ember_row.get("demand_twh")
			row5["ember_generation_twh"] = ember_row.get("total_generation_twh")
			row5["ember_net_imports_twh"] = ember_row.get("net_imports_twh")
			row5["ember_low_coverage"] = bool(
				ember_row.get("low_coverage", false))
			row5["ember_has_national_capacity"] = bool(
				ember_row.get("has_national_capacity", false))
		var cap: Variant = ember.get(tag5, null)
		if typeof(cap) == TYPE_DICTIONARY:
			var cap_row: Dictionary = cap
			row5["ember_capacity_year"] = int(cap_row.get("year", 0))
			row5["ember_total_mw"] = float(cap_row.get("total_mw", 0.0))
			row5["ember_by_source"] = cap_row.get("by_source", {})
		# Escala e comparação na unidade do modelo (D-116): a Ember do ano-base
		# entra multiplicada pela fração simulada, a mesma do parque e da
		# demanda. Sem isso, "servido baixo" e "fração simulada baixa" se
		# confundem — foi o que aconteceu no QA t82 (32-35% era o fator de
		# capacidade do parque, não a fração servida).
		var row_scale := float(scale_rows.get(tag5, 1.0))
		row5["model_scale"] = row_scale
		var gen5: Variant = row5.get("ember_generation_twh")
		if typeof(gen5) == TYPE_FLOAT or typeof(gen5) == TYPE_INT:
			var gen_model := float(gen5) * 1000000.0 * row_scale
			row5["ember_generation_mwh_model"] = gen_model
			row5["served_vs_ember_generation"] = (
				float(row5["served_mwh"]) * span_days / gen_model) \
				if gen_model > 0.0 else 0.0
		var dem5: Variant = row5.get("ember_demand_twh")
		if typeof(dem5) == TYPE_FLOAT or typeof(dem5) == TYPE_INT:
			var dem_model := float(dem5) * 1000000.0 * row_scale / 365.0
			row5["demand_vs_ember_demand"] = (
				float(row5["demand_mwh"]) / dem_model) \
				if dem_model > 0.0 else 0.0
	var top: Array = by_nation.slice(0, 5)
	var city_total := connected + partial + isolated
	# Custo de energia da indústria por unidade de valor: as cidades com
	# planta industrial do bootstrap expõem o repasse ao preço.
	var cost_rows: Array = []
	var city_rows: Dictionary = final.get("cities", {})
	for cid: Variant in city_rows:
		var row4: Dictionary = city_rows[cid]
		var per_value := float(row4.get("power_cost_per_value", 0.0))
		if per_value > 0.0:
			cost_rows.append({
				"city": String(cid),
				"nation": String(row4.get("nation", "")),
				"per_value": per_value,
				"cost": float(row4.get("power_cost_industry", 0.0)),
				"price": float(row4.get("price", 0.0)),
				"served_mwh": float(row4.get("served_mwh", 0.0)),
			})
	cost_rows.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return float(a["per_value"]) > float(b["per_value"]))
	# E5 (D-117): importadores do dia (modelo) x importador Ember do ano-base,
	# e a separação do racionamento entre importadores reais e o resto.
	#
	# ATENÇÃO (bug do runner, corrigido): `import_mwh`/`export_mwh` da rede são
	# ACUMULADOS no ano por `power.cpp` (não são resetados no laço diário), então
	# a média em MW é `mwh / (days * 24)` — dividir por 24 infla 365×. A demanda
	# diária da rede, essa sim, vem do dia: o total anual é `demand_mwh * days`.
	var days := maxi(1, samples.size())
	var annual_demand := 0.0
	for row_dem: Dictionary in by_nation:
		annual_demand += float(row_dem.get("demand_mwh", 0.0)) * float(days)
	var importers: Array = []
	var ration_importers: Array = []
	var ration_others: Array = []
	var imported_total := 0.0
	for row6: Dictionary in by_nation:
		var tag6 := String(row6["nation"])
		var imported := float(row6.get("import_mwh", 0.0))
		var exported := float(row6.get("export_mwh", 0.0))
		imported_total += imported
		var ember_net6: Variant = row6.get("ember_net_imports_twh")
		var ember_mw: Variant = null
		if typeof(ember_net6) == TYPE_FLOAT or typeof(ember_net6) == TYPE_INT:
			ember_mw = float(ember_net6) * 1000000.0 / 8760.0
		if imported > 0.0:
			importers.append({
				"nation": tag6,
				"import_mwh": imported,
				"export_mwh": exported,
				"import_value": float(row6.get("import_value", 0.0)),
				"ember_net_imports_twh": ember_net6,
				"served_ratio": float(row6.get("served_ratio", 0.0)),
				"import_mw": imported / (24.0 * float(days)),
				"export_mw": exported / (24.0 * float(days)),
				"import_share": imported / annual_demand if annual_demand > 0.0 else 0.0,
				"ember_import_mw": ember_mw,
			})
		if rationing.has(tag6):
			var is_importer := ember_mw != null and float(ember_mw) > 0.0
			if is_importer:
				ration_importers.append(tag6)
			else:
				ration_others.append(tag6)
	importers.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return float(a["import_mwh"]) > float(b["import_mwh"]))
	return {
		"ready": true,
		"days": samples.size(),
		"average_price": bill / served if served > 0.0 else 0.0,
		"served_ratio": served / demand if demand > 0.0 else 1.0,
		"min_served_ratio": min_ratio,
		"days_with_rationing": days_rationing,
		"max_rationing_cities": max_ration,
		"rationing_nations": rationing,
		"rationing_importers": ration_importers,
		"rationing_others": ration_others,
		"nations_detail": by_nation,
		"grids": grids,
		"connected_cities": connected,
		"partial_cities": partial,
		"isolated_cities": isolated,
		"connected_share": float(connected) / float(city_total) if city_total > 0 else 0.0,
		"loss_pct": float(final.get("loss_pct", 0.0)),
		"installed_mw_by_fuel": final.get("installed_mw_by_fuel", {}),
		"generation_mwh_by_fuel": final.get("generation_mwh_by_fuel", {}),
		# Caixa agregado do setor elétrico: se o preço recupera o custo marginal,
		# ele não drena; se a escala do preço fosse cortada, este caixa/razão de
		# pagamento cairia (evidência de que preço ≠ amortecedor).
		"power_cash": float(final.get("cash", 0.0)),
		"demand_mwh": float(final.get("demand_mwh", 0.0)),
		"served_mwh": float(final.get("served_mwh", 0.0)),
		"interconnect_links": int(final.get("interconnect_links", 0)),
		"interconnect_flow_mwh": float(final.get("interconnect_flow_mwh", 0.0)),
		"interconnect_sent_mwh": float(final.get("interconnect_sent_mwh", 0.0)),
		"interconnect_value": float(final.get("interconnect_value", 0.0)),
		# Média anual (o `demand_mwh` do fecho é diário; o ledger de comércio é
		# anual). `import_share` = importado no ano / demanda do ano.
		"annual_demand_mwh": annual_demand,
		"annual_import_mwh": imported_total,
		"import_share": imported_total / annual_demand if annual_demand > 0.0 else 0.0,
		"top10_importers": importers.slice(0, 10),
		"top5_by_served": top,
		"industry_cost_top": cost_rows.slice(0, 8),
		"ember_top5_capacity": _ember_top5(),
	}


func _ember_capacity() -> Dictionary:
	const PATH := "res://data/definitions/energy/national_capacity.json"
	if not FileAccess.file_exists(PATH):
		return {}
	var data: Variant = _load(PATH)
	if typeof(data) != TYPE_DICTIONARY:
		return {}
	var nations: Variant = (data as Dictionary).get("nations", {})
	return nations if typeof(nations) == TYPE_DICTIONARY else {}


func _ember_demand() -> Dictionary:
	const PATH := "res://data/definitions/energy/national_demand.json"
	var data: Variant = _load(PATH)
	if typeof(data) != TYPE_DICTIONARY:
		return {}
	var nations: Variant = (data as Dictionary).get("by_nation", {})
	return nations if typeof(nations) == TYPE_DICTIONARY else {}


func _ember_top5() -> Array:
	const PATH := "res://data/definitions/energy/national_capacity.json"
	var data: Variant = _load(PATH)
	if typeof(data) != TYPE_DICTIONARY:
		return []
	var top: Variant = (data as Dictionary).get("top_by_total_mw", [])
	return top if typeof(top) == TYPE_ARRAY else []


func _load(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return null
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data


func _make_world() -> Object:
	var cities: Array = _load(CITIES_PATH)
	var ports: Array = _load(PORTS_PATH)
	var buildings: Array = _load(BUILDINGS_PATH)
	var goods: Array = _load(GOODS_PATH)
	if cities.is_empty() or goods.is_empty():
		print("[c1] FALHA: cenário vazio")
		return null
	RegionDataScript.attach(cities)
	var sim = EconomySim.new()
	sim.set_goods(goods)
	sim.set_recipes(_load(RECIPES_PATH))
	sim.set_needs(_load(NEEDS_PATH))
	sim.set_services(_load(SERVICES_PATH))
	# C2c (D-108): regime real por moeda ANTES do carregamento das moedas.
	if sim.has_method("set_fx_regimes"):
		sim.set_fx_regimes(_load(FX_REGIMES_PATH))
	sim.set_currencies(_load(CURRENCIES_PATH))
	sim.set_sector_employment(_load(SECTOR_EMPLOYMENT_PATH))
	sim.set_age_structure(_load(AGE_STRUCTURE_PATH))
	sim.set_informality(_load(INFORMALITY_PATH))
	sim.set_health_access(_load(HEALTH_ACCESS_PATH))
	sim.set_sim_threads(1)
	var geo := GeoScript.new()
	geo.load_data()
	buildings = geo.resolve_initial_farms(buildings, cities)
	if geo.has_method("resolve_initial_mines"):
		buildings = geo.resolve_initial_mines(buildings, cities)
	sim.initialize_world(cities, ports, buildings)
	GeoScript.apply_initial_infra(sim)
	# C4 (D-110): bancos com dono do dado (nomes fictícios/genéricos). Depois do
	# mundo: a moeda de cada nação e o BC da C2a já existem.
	if sim.has_method("set_bank_definitions"):
		sim.set_bank_definitions(_load(BANKS_PATH))
	if sim.has_method("set_urban"):
		sim.set_urban(_load(URBAN_LAND_PATH), _load(HOUSING_CITY_PATH),
			_load(HOUSING_PATH), _load(MUNICIPAL_FINANCE_PATH))
	var centers: Array = []
	for city: Dictionary in cities:
		centers.append({
			"id": String(city.get("id", "")),
			"nation": String(city.get("nation", "")),
			"center": Vector2(float(city.get("x", 0.0)),
				float(city.get("y", 0.0))),
		})
	geo.apply_to_sim(sim, centers)
	# B6 (patrimônio da região): o autoload aplica em produção; aqui é opcional
	# para o A/B do 365 d continuar comparável ao mundo da D-101.
	if _property:
		LandPropertyScript.apply(sim)
	if sim.has_method("set_crop_plan"):
		var agri := AgriScript.new()
		agri.apply_to_sim(sim, cities, geo, {
			"calendar_nations": agri.calendar_nations(cities),
			"initial_stock": true,
			"farms": AgriScript.farms_of(buildings),
		})
	if sim.has_method("set_storage_capacity"):
		var storage := StorageScript.new()
		storage.apply_to_sim(sim, cities, buildings,
			{"only_missing": true, "initial_stock": true})
	# PARTE E (D-116): energia — parque, acesso/perdas e potencial do dado
	# público, declarados depois do mundo e antes da IA (mesma ordem do autoload).
	if sim.has_method("set_power_definitions"):
		var power := PowerScript.new()
		var power_report := power.apply_to_sim(sim, cities, {"only_missing": true})
		print(("[c1] energia: parque=%.0f MW cidades=%d nações=%d operador=%d "
			+ "âncora=%d geração_real=%d") % [
			float(power_report.get("park_mw", 0.0)),
			int(power_report.get("cities_with_park", 0)),
			int(power_report.get("nations", 0)),
			(power_report.get("operator_nations", []) as Array).size(),
			int(power_report.get("nations_with_demand_anchor", 0)),
			int(power_report.get("nations_with_generation_calibration", 0))])
	sim.set_ai_enabled(true)
	# D-111: sem flag vale o default do jogo (LIGADO). Com flag, ela MANDA --
	# inclusive `--credit=0`/`--harvest-rate=0` (braço "off" do A/B).
	if _credit_set:
		sim.set_bank_credit(_credit)
		print("[c1] C3: credito de empresa %s por flag (--credit=%d)"
			% ["LIGADO" if _credit else "DESLIGADO", 1 if _credit else 0])
	if _harvest_rate_set:
		sim.set_balance({
			"harvest_credit_rate": 1.0 if _harvest_rate else 0.0,
			"income_basket_margin": 0.0,
		})
		print("[c1] C4: juro do envelope da safra %s por flag (--harvest-rate=%d)"
			% ["LIGADO" if _harvest_rate else "DESLIGADO",
				1 if _harvest_rate else 0])
	return sim


func _food_ratio(status: Dictionary) -> Dictionary:
	# Dono único da conta (portão e hash): core/economy/trajectory_digest.gd.
	return TrajectoryDigest.food_ratios(status)


func _food_gate(history: Array) -> Dictionary:
	var tags: Dictionary = {}
	for day_ratio in history:
		for tag in (day_ratio as Dictionary):
			if not tags.has(tag):
				tags[tag] = {"sum": 0.0, "days": 0}
			tags[tag]["sum"] = float(tags[tag]["sum"]) \
				+ float((day_ratio as Dictionary)[tag])
			tags[tag]["days"] = int(tags[tag]["days"]) + 1
	var values: Array = []
	var mean_acc := 0.0
	for tag in tags:
		var entry: Dictionary = tags[tag]
		var value := float(entry["sum"]) / float(maxi(1, int(entry["days"])))
		values.append({"nation": String(tag), "food_avg30": value})
		mean_acc += value
	values.sort_custom(func(a, b): return float(a["food_avg30"]) < float(b["food_avg30"]))
	var median := 0.0
	if not values.is_empty():
		var middle := values.size() / 2
		median = float(values[middle]["food_avg30"]) if values.size() % 2 == 1 \
			else (float(values[middle - 1]["food_avg30"])
				+ float(values[middle]["food_avg30"])) / 2.0
	var below: Array = []
	for row: Dictionary in values:
		if float(row["food_avg30"]) < GATE:
			below.append(String(row["nation"]))
	return {
		"nations": values.size(),
		"median": median,
		"mean": mean_acc / float(maxi(1, values.size())),
		"below_gate": below.size(),
		"below": below,
		"worst": values.slice(0, mini(10, values.size())),
	}


## C2b1 (D-104): política monetária medida no fecho do mundo — taxa básica por
## moeda e inflação por país. A inflação de 12 meses só existe com 13 amostras
## do CPI (dia 390); antes disso a ANUALIZADA é publicada no lugar e a ausência
## do número de 12 meses é declarada (`nations_has_12m`).
func _monetary_report(sim: Object) -> Dictionary:
	var bank: Dictionary = sim.get_central_bank_status()
	var rates: Array = []
	var controlled := 0
	var with_inflation := 0
	var currencies: Dictionary = bank.get("currencies", {})
	for code: Variant in currencies:
		var row: Dictionary = currencies[code]
		var policy: Dictionary = row.get("policy", {})
		rates.append(float(policy.get("rate", 0.0)) * 100.0)
		if String(policy.get("independence_effective", "")) == "controlled":
			controlled += 1
		if bool(policy.get("has_inflation", false)):
			with_inflation += 1
	var infl_12m: Array = []
	var infl_ann: Array = []
	var cpi: Dictionary = bank.get("cpi", {})
	for nation: Variant in cpi:
		var row: Dictionary = cpi[nation]
		if bool(row.get("has_12m", false)):
			infl_12m.append(float(row.get("inflation_12m", 0.0)) * 100.0)
		if bool(row.get("has_annualized", false)):
			infl_ann.append(float(row.get("inflation_annualized", 0.0)) * 100.0)
	var totals: Dictionary = bank.get("totals", {})
	return {
		"currencies": currencies.size(),
		"controlled": controlled,
		"currencies_with_inflation": with_inflation,
		"rate_pct": _spread(rates),
		"inflation_12m_pct": _spread(infl_12m),
		"nations_has_12m": infl_12m.size(),
		"inflation_annualized_pct": _spread(infl_ann),
		"nations_annualized": infl_ann.size(),
		"emission_total": float(totals.get("emission_total", 0.0)),
		"emission_today": float(totals.get("emission_today", 0.0)),
		"securities": float(totals.get("securities", 0.0)),
		"government_debt": float(totals.get("government_debt", 0.0)),
	}


## C2c (D-108): regimes reais — mistura inicial/efetiva, âncoras, trava de
## união e as desvalorizações forçadas do mundo default (quais moedas).
func _monetary_timeline_row(sim: Object, day: int) -> Dictionary:
	# Calibração pré-C6 (D-113): uma linha por amostra do ano com a distribuição
	# da taxa básica, do alvo da regra e do hiato por moeda, e da inflação por
	# país. Só medição — lê o status público da C2a/C2b1.
	var bank: Dictionary = sim.get_central_bank_status()
	var rates: Array = []
	var targets: Array = []
	var gaps: Array = []
	var infl_used: Array = []
	var currencies: Dictionary = bank.get("currencies", {})
	for code: Variant in currencies:
		var row: Dictionary = currencies[code]
		var policy: Dictionary = row.get("policy", {})
		rates.append(float(policy.get("rate", 0.0)) * 100.0)
		targets.append(float(policy.get("target_rate", 0.0)) * 100.0)
		gaps.append(float(policy.get("gap", 0.0)))
		if bool(policy.get("has_inflation", false)):
			infl_used.append(float(policy.get("inflation_used", 0.0)) * 100.0)
	var infl_12m: Array = []
	var infl_ann: Array = []
	var cpi: Dictionary = bank.get("cpi", {})
	for nation: Variant in cpi:
		var row: Dictionary = cpi[nation]
		if bool(row.get("has_12m", false)):
			infl_12m.append(float(row.get("inflation_12m", 0.0)) * 100.0)
		if bool(row.get("has_annualized", false)):
			infl_ann.append(float(row.get("inflation_annualized", 0.0)) * 100.0)
	return {
		"day": day,
		"rate_pct": _spread(rates),
		"target_pct": _spread(targets),
		"gap_pp": _spread(gaps),
		"inflation_used_pct": _spread(infl_used),
		"inflation_12m_pct": _spread(infl_12m),
		"nations_has_12m": infl_12m.size(),
		"inflation_annualized_pct": _spread(infl_ann),
	}


func _fx_summary(sim: Object) -> Dictionary:
	var bank: Dictionary = sim.get_central_bank_status()
	var currencies: Dictionary = bank.get("currencies", {})
	var by_regime: Dictionary = {}
	var by_anchor: Dictionary = {}
	var devalued: Array = []
	var devaluations := 0
	var locked := 0
	for code: Variant in currencies:
		var row: Dictionary = currencies[code]
		var fx: Dictionary = row.get("fx", {})
		var regime := String(fx.get("regime_saved", ""))
		by_regime[regime] = int(by_regime.get(regime, 0)) + 1
		var anchor := String(fx.get("anchor", ""))
		if not anchor.is_empty():
			by_anchor[anchor] = int(by_anchor.get(anchor, 0)) + 1
		if bool(fx.get("locked", false)):
			locked += 1
		var count := int(fx.get("devaluations", 0))
		devaluations += count
		if count > 0:
			devalued.append({"currency": String(code), "count": count,
				"last_day": int(fx.get("last_event_day", -1))})
	return {
		"source": String(bank.get("monetary_policy", {}).get(
			"regime_source", "")),
		"by_regime": by_regime,
		"by_anchor": by_anchor,
		"locked_currencies": locked,
		"devaluations": devaluations,
		"devalued_currencies": devalued,
	}


## D-105: estado da flag de juro no envelope de crédito da safra. `-1` = build
## sem a flag (base C2a), onde o termo não existe.
func _harvest_credit_rate(sim: Object) -> Variant:
	if not sim.has_method("get_central_bank_status"):
		return -1
	var bank: Dictionary = sim.get_central_bank_status()
	var policy: Dictionary = bank.get("monetary_policy", {})
	if not policy.has("harvest_credit_rate"):
		return -1
	return bool(policy.get("harvest_credit_rate", false))


## C2b2 (D-106): relatório do cenário de câmbio — quantas moedas desvalorizaram,
## quantas ficaram sem reserva, cobertura e eventos. Só medição.
func _fx_report(sim: Object, seed_days: int) -> Dictionary:
	var bank: Dictionary = sim.get_central_bank_status()
	var totals: Dictionary = bank.get("totals", {})
	var currencies: Dictionary = bank.get("currencies", {})
	var devalued: Array = []
	var funded := 0
	var coverage_sum := 0.0
	var coverage_n := 0
	for code: Variant in currencies:
		var fx: Dictionary = (currencies[code] as Dictionary).get("fx", {})
		if bool(fx.get("funded", false)):
			funded += 1
		if int(fx.get("devaluations", 0)) > 0:
			devalued.append({
				"currency": String(code),
				"devaluations": int(fx.get("devaluations", 0)),
				"deviation": float(fx.get("deviation", 0.0)),
			})
		var cover := float(fx.get("coverage", 0.0))
		if cover > 0.0:
			coverage_sum += cover
			coverage_n += 1
	devalued.sort_custom(func(a, b):
		return int(a["devaluations"]) > int(b["devaluations"]))
	return {
		"seed_days": seed_days,
		"funded_currencies": funded,
		"devaluations": int(totals.get("devaluations", 0)),
		"currencies_devalued": devalued.size(),
		"devalued_top": devalued.slice(0, mini(10, devalued.size())),
		"reserves": float(totals.get("reserves", 0.0)),
		"vault_gold_tonnes": float(totals.get("vault_gold_tonnes", 0.0)),
		"coverage_mean": coverage_sum / float(maxi(1, coverage_n)),
		"events": (bank.get("fx_events", []) as Array).slice(0, 10),
	}


func _spread(values: Array) -> Dictionary:
	if values.is_empty():
		return {"n": 0, "median": 0.0, "p10": 0.0, "p90": 0.0}
	var sorted := values.duplicate()
	sorted.sort()
	var n := sorted.size()
	var median := float(sorted[n / 2]) if n % 2 == 1 \
		else (float(sorted[n / 2 - 1]) + float(sorted[n / 2])) * 0.5
	return {
		"n": n,
		"median": median,
		"p10": float(sorted[clampi(int(floor(0.10 * (n - 1))), 0, n - 1)]),
		"p90": float(sorted[clampi(int(ceil(0.90 * (n - 1))), 0, n - 1)]),
	}


func _balance_report(sim: Object, status: Dictionary) -> Dictionary:
	if not sim.has_method("get_balance_status"):
		# Build sem a C1 (A/B): o resto do harness continua comparável.
		return {"invariant": {"ok": true}, "arrears": {}, "world": {},
			"sample_nations": {}, "nations": 0}
	var balance: Dictionary = sim.get_balance_status()
	var world: Dictionary = balance.get("world", {})
	var by_nation: Dictionary = balance.get("by_nation", {})
	var cities: Array = status.get("cities", [])
	var population_by_nation := {}
	for city: Dictionary in cities:
		var tag := String(city.get("nation", ""))
		population_by_nation[tag] = float(population_by_nation.get(tag, 0.0)) \
			+ float(city.get("simulation_population", 0.0))
	var sample: Dictionary = {}
	for tag in SAMPLE_NATIONS:
		if not by_nation.has(tag):
			continue
		var row: Dictionary = by_nation[tag]
		var fam: Dictionary = row.get("families", {})
		var firms: Dictionary = row.get("firms", {})
		var gov: Dictionary = row.get("government", {})
		var people := float(population_by_nation.get(tag, 0.0))
		sample[tag] = {
			"families_net_worth": float(fam.get("net_worth", 0.0)),
			"families_cash": float(fam.get("cash", 0.0)),
			"families_property": float(fam.get("property", 0.0)),
			"families_receivables": float(fam.get("receivables", 0.0)),
			"firms_net_worth": float(firms.get("net_worth", 0.0)),
			"firms_inventory": float(firms.get("inventory", 0.0)),
			"firms_payables": float(firms.get("payables", 0.0)),
			"government_net_worth": float(gov.get("net_worth", 0.0)),
			"net_worth_per_capita": float(fam.get("net_worth", 0.0))
				/ people if people > 0.0 else 0.0,
		}
	var families: Dictionary = world.get("families", {})
	var firms: Dictionary = world.get("firms", {})
	var government: Dictionary = world.get("government", {})
	var external: Dictionary = world.get("external", {})
	var solvency: Dictionary = world.get("firms_solvency", {})
	var invariant: Dictionary = balance.get("invariant", {})
	return {
		"invariant": invariant,
		"arrears": {
			"plants": float(invariant.get("arrears_plants", 0.0)),
			"service": float(invariant.get("arrears_service", 0.0)),
			"rural": float(invariant.get("arrears_rural", 0.0)),
		},
		"world": {
			"families_net_worth": float(families.get("net_worth", 0.0)),
			"families_cash": float(families.get("cash", 0.0)),
			"families_receivables": float(families.get("receivables", 0.0)),
			"families_property": float(families.get("property", 0.0)),
			"families_inventory": float(families.get("inventory", 0.0)),
			"firms_net_worth": float(firms.get("net_worth", 0.0)),
			"firms_inventory": float(firms.get("inventory", 0.0)),
			"firms_fixed_capital": float(firms.get("fixed_capital", 0.0)),
			"firms_payables": float(firms.get("payables", 0.0)),
			"government_net_worth": float(government.get("net_worth", 0.0)),
			"government_inventory": float(government.get("inventory", 0.0)),
			"external_cash": float(external.get("cash", 0.0)),
			"firms_solvency": solvency,
		},
		"sample_nations": sample,
		"nations": by_nation.size(),
	}


func _run() -> void:
	var sim = _make_world()
	if sim == null:
		quit(1)
		return
	if _fx_seed_days > 0 and sim.has_method("seed_central_bank_reserves"):
		# C2b2 (D-106): o cenário funda as reservas DEPOIS de 30 dias de comércio
		# (no dia 0 não há importação medida); o laço abaixo faz a chamada.
		print("[c1] cenário C2b2: reservas = %d dias de importação no dia 30"
			% _fx_seed_days)
	var money_start := float(sim.get_status().get("money_total", 0.0))
	var history: Array = []
	var violation_count := 0
	var violation_samples: Array = []
	var stalled := 0
	var max_drift := 0.0
	var day_before := int(sim.get_status().get("day", 0))
	var hash_lines := PackedStringArray()
	var wall_start := Time.get_ticks_msec()
	var stock_start: Dictionary = {}
	var stock_formed: Dictionary = {}
	var stock_timeline: Array = []
	var monetary_timeline: Array = []
	var power_samples: Array = []
	var power_timeline: Array = []
	# E2/E3/E5 (D-117) — diagnóstico por entidade e escala econômica.
	var bankruptcies_detail: Array = []
	var buildings_prev: Dictionary = {}
	var buildings_prev_day := 0
	var bankrupt_counter := 0
	var energy_by_nation: Dictionary = {}
	var gate_first: Dictionary = {}
	var link_flow: Dictionary = {}
	var food_price_base: Dictionary = {}
	_load_goods_meta()

	for day in _days:
		if _fx_seed_days > 0 and day == 30:
			# Câmbio: as reservas do cenário são 30 dias da importação MEDIDA do
			# mundo (dia 30; no dia 0 não há comércio).
			var seeded: float = sim.seed_central_bank_reserves(_fx_seed_days)
			print("[c1] cenário C2b2: reservas = %.3f âncora no dia 30" % seeded)
		var summary: Dictionary = sim.tick_day()
		for violation in Array(summary.get("invariant_violations", [])):
			violation_count += 1
			if violation_samples.size() < 10:
				violation_samples.append("dia %d: %s"
					% [day + 1, String(violation)])
		var day_now := int(summary.get("day", 0))
		if day_now <= day_before:
			stalled += 1
		day_before = day_now
		var status: Dictionary = sim.get_status()
		var money := float(status.get("money_total", 0.0))
		if money_start > 0.0:
			max_drift = maxf(max_drift, absf(money - money_start) / money_start)
		var ratio := _food_ratio(status)
		history.append(ratio)
		if history.size() > WINDOW:
			history.pop_front()
		# E2/E3/E5 (D-117): energia do dia — uma leitura por dia serve à
		# amostra do ano, ao ledger por ligação e ao diagnóstico por nação.
		var power_day: Dictionary = {}
		if sim.has_method("get_power_status"):
			power_day = sim.get_power_status()
		if bool(power_day.get("ready", false)):
			var power_row := _power_sample(day_now, power_day)
			power_samples.append(power_row)
			if int(day_now) % 30 == 0 or day == _days - 1:
				power_timeline.append(power_row)
			_accumulate_energy(energy_by_nation, status, power_day)
			_accumulate_links(link_flow, power_day)
		# Cadeia do portão: primeiro dia em que a média de 30 d cruza o limiar.
		_track_gate_first(history, gate_first, day_now)
		# Falências por entidade: o núcleo apaga o edifício no tick, então o
		# retrato anterior (a cada N dias e no dia da mudança) é o que nomeia a
		# firma que fechou. `get_city_status` é consulta de leitura.
		var bankrupt_now := int(status.get("bankruptcies", 0))
		if bankrupt_now != bankrupt_counter \
				or day_now % BANKRUPTCY_SNAPSHOT_DAYS == 0:
			var snapshot := _buildings_snapshot(sim,
				status.get("cities", []) as Array, power_day)
			if bankrupt_now != bankrupt_counter and not buildings_prev.is_empty():
				var vanished: Array = []
				for building_id: Variant in buildings_prev:
					if snapshot.has(building_id):
						continue
					var record: Dictionary = \
						(buildings_prev[building_id] as Dictionary).duplicate()
					record["day"] = day_now
					record["last_seen_day"] = buildings_prev_day
					record["counter_delta"] = bankrupt_now - bankrupt_counter
					vanished.append(record)
				vanished.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
					return String(a["building_id"]) < String(b["building_id"]))
				bankruptcies_detail.append_array(vanished)
			buildings_prev = snapshot
			buildings_prev_day = day_now
			bankrupt_counter = bankrupt_now
		if day_now == 1:
			# Preço-base da comida para medir o repasse no ano (uma passada
			# pesada no começo; o resto lê só mercados das nações do portão).
			food_price_base = _food_price_by_nation(sim, status)
		var balance_block: Dictionary = status.get("balance", {})
		# C2b1-fix (D-105): linha canônica única (dinheiro 6 casas, falências,
		# comida, M0, estoque privado, crédito). Ver
		# core/economy/trajectory_digest.gd e docs/economy/inv_c2b1_gate/README.md.
		hash_lines.append(TrajectoryDigest.line(day_now, status, sim))
		if day == 0 and sim.has_method("get_stock_status"):
			# C5 (D-112): a bolsa nasce no primeiro tick; a variação do ano é
			# medida deste fechamento até o último.
			stock_start = sim.get_stock_status()
		if day_now == 63 and sim.has_method("get_stock_status"):
			# C5-fix (D-112): o índice só é publicado depois do aquecimento;
			# os papéis são medidos do dia de formação até o fim do ano.
			stock_formed = sim.get_stock_status()
		if [63, 120, 200, 300, 365].has(day_now) and sim.has_method("get_stock_status"):
			stock_timeline.append(_stock_timeline_row(sim, day_now))
		# Calibração pré-C6 (D-113): trajetória monetária mensal (mediana/p10/p90).
		if MONETARY_SAMPLE_DAYS.has(day_now):
			monetary_timeline.append(_monetary_timeline_row(sim, day_now))
		if int(day_now) % 30 == 0 or day == _days - 1:
			print("[c1] progresso dia %d/%d t=%.1fs"
				% [int(day_now), _days,
					(Time.get_ticks_msec() - wall_start) / 1000.0])

	var status_final: Dictionary = sim.get_status()
	var money_end := float(status_final.get("money_total", 0.0))
	var food := _food_gate(history)
	var balance := _balance_report(sim, status_final)
	var trajectory := TrajectoryDigest.sha256(hash_lines)
	# E2/E3/E5 (D-117) — diagnóstico fino do portão e da escala econômica.
	var gate_below: Array = food.get("below", [])
	var control_below := _control_below()
	var gate_extras: Array = []
	for tag: Variant in gate_below:
		if not control_below.has(String(tag)):
			gate_extras.append(String(tag))
	var power_final: Dictionary = sim.get_power_status() \
		if sim.has_method("get_power_status") else {}
	var food_chain := _food_chain(sim, status_final, power_final, gate_below,
		control_below, gate_first, food_price_base)
	var service_failed := 0
	if sim.has_method("get_service_bankruptcies"):
		service_failed = int(sim.get_service_bankruptcies())
	var out := {
		"days": _days,
		"threads": 1,
		"property": _property,
		"final_day": int(status_final.get("day", 0)),
		"stalled_days": stalled,
		"invariant_violations": violation_count,
		"invariant_violation_samples": violation_samples,
		"bankruptcies": int(status_final.get("bankruptcies", -1)),
		"money_start": money_start,
		"money_end": money_end,
		"drift": (money_end - money_start) / money_start if money_start > 0.0 else 0.0,
		"max_daily_drift": max_drift,
		"food_gate": food,
		"balance": balance,
		"trajectory_sha256": trajectory,
		"trajectory_fields": TrajectoryDigest.FIELDS,
		"trajectory_end": TrajectoryDigest.totals(sim),
		"harvest_credit_rate": _harvest_credit_rate(sim),
		"monetary": _monetary_report(sim),
		"fx": _fx_summary(sim),
		# C4 (D-110): totais dos bancos no fim do mundo — depósitos, retido,
		# dividendo e remessa do banco estrangeiro. Só medição.
		"banks": _banks_totals(sim),
		# C6 (D-114): crise bancária no fecho — confiança, corridas,
		# resoluções, fundo de seguro e redesconto. Só medição.
		"bank_crisis": _bank_crisis_report(sim),
		"stock": _stock_report(sim, stock_start, status_final, stock_formed),
		"stock_timeline": stock_timeline,
		"monetary_timeline": monetary_timeline,
		# PARTE E (D-116): energia — preço médio, % servida, racionamento,
		# conexão das cidades e geração por fonte. Só medição.
		"power": _power_digest(sim, power_samples),
		"power_timeline": power_timeline,
		# E2/E3/E5 (D-117) — diagnóstico por entidade e escala econômica.
		# Falência = edifício da firma apagado pelo núcleo; `bank_events` cobre
		# banco (crise C6) e `service_bankruptcies` cobre firma de serviço.
		"bankruptcies_detail": bankruptcies_detail,
		"service_bankruptcies": service_failed,
		"food_chain": food_chain,
		"food_gate_extras": gate_extras,
		"food_gate_control": _control_path,
		"energy_economy": _energy_economy(energy_by_nation, power_final),
		"interconnect_links": _link_report(link_flow, power_final),
		"wall_s": (Time.get_ticks_msec() - wall_start) / 1000.0,
	}
	if _fx_seed_days > 0:
		out["fx_scenario"] = _fx_report(sim, _fx_seed_days)
	# C3/D-111: o bloco do crédito é medido sempre que o mundo tem crédito
	# ligado — por flag de laboratório OU por default do jogo (D-111).
	var credit := _credit_report(sim)
	if _credit or bool(credit["enabled"]):
		out["credit"] = credit
	var file := FileAccess.open(_out, FileAccess.WRITE)
	if file == null:
		print("[c1] FALHA: não escreveu " + _out)
	else:
		file.store_string(JSON.stringify(out, "  ", true))
		file.close()
	print("[c1] dias=%d falências=%d violações=%d travados=%d drift=%.6f "
		% [_days, int(out["bankruptcies"]), violation_count, stalled,
			float(out["drift"])]
		+ "portão=%d/%d mediana=%.4f trajectory=%s"
		% [int(food["below_gate"]), int(food["nations"]),
			float(food["median"]), String(out["trajectory_sha256"]).substr(0, 8)])
	print("[c1] D-117: extras vs controle=%d %s | falências por entidade=%d"
		% [gate_extras.size(), str(gate_extras), bankruptcies_detail.size()])
	if not bankruptcies_detail.is_empty():
		var lines := PackedStringArray()
		for record: Dictionary in bankruptcies_detail:
			lines.append("%s/%s/%s dia %d" % [String(record.get("nation", "")),
				String(record.get("sector", "")), String(record.get("building_id", "")),
				int(record.get("day", 0))])
		print("[c1] D-117 falências: " + " | ".join(lines))
	var crisis_report: Dictionary = out.get("bank_crisis", {})
	print("[c1] C6: corridas=%d resoluções=%d eventos=%d fundo=%.2f prêmios=%.2f "
		% [int(crisis_report.get("runs", 0)),
		int(crisis_report.get("resolutions", 0)),
		int(crisis_report.get("crisis_events", 0)),
		float(crisis_report.get("fund", 0.0)),
		float(crisis_report.get("premiums", 0.0))]
		+ "redesconto=%.2f fuga=%.2f confiança_min=%.4f"
		% [float(crisis_report.get("lolr_borrowed", 0.0)),
		float(crisis_report.get("capital_flight", 0.0)),
		float(crisis_report.get("confidence_min", 1.0))])
	print("[c1] balanço: financeiro=%.6f caixa_residual=%.6f PL_famílias=%.0f "
		% [float((balance["invariant"] as Dictionary).get("financial_sum", 0.0)),
			float((balance["invariant"] as Dictionary).get("cash_residual", 0.0)),
			float((balance["world"] as Dictionary).get("families_net_worth", 0.0))]
		+ "atrasado=%.0f saída=" % float((balance["invariant"] as Dictionary)
			.get("receivables", 0.0)) + _out)
	var stock_line: Dictionary = out.get("stock", {})
	if not stock_line.is_empty():
		print("[c1] bolsa (C5): indice=%.1f variacao p10/p50/p90=%.2f%%/%.2f%%/%.2f%%"
			% [float(stock_line.get("index_global", 100.0)),
				100.0 * float(stock_line.get("variation_p10", 0.0)),
				100.0 * float(stock_line.get("variation_p50", 0.0)),
				100.0 * float(stock_line.get("variation_p90", 0.0))]
			+ " confianca p10/p50/p90=%.3f/%.3f/%.3f papeis=%d dividendo=%.0f"
			% [float(stock_line.get("confidence_p10", 1.0)),
				float(stock_line.get("confidence_p50", 1.0)),
				float(stock_line.get("confidence_p90", 1.0)),
				int(stock_line.get("listed", 0)),
				float(stock_line.get("dividends_paid", 0.0))]
			+ " investimento=%.0f"
			% float(stock_line.get("investment_capex_total", 0.0)))
	print("[c1] OK: 365 d medidos")
	if _credit or bool(credit["enabled"]):
		print("[c1] crédito (C3): ligado=%s empréstimos=%d dívida=%.3f "
			% [str(credit["enabled"]), int(credit["loans_count"]),
				float(credit["group_debt"])]
			+ "calotes=%d baixas=%.3f patrimônio_banco=%.3f"
			% [int(credit["defaults"]), float(credit["writeoffs"]),
				float(credit["equity"])])
	quit(0)


func _credit_report(sim) -> Dictionary:
	# C3 (D-107) + D-111: o crédito de empresa medido no fim do mundo. Medido
	# sempre que o mundo tem crédito ligado (default do jogo ou flag).
	var bank: Dictionary = sim.get_bank_status()
	var totals: Dictionary = bank.get("totals", {})
	return {
		"enabled": bool((bank.get("policy", {}) as Dictionary).get(
			"enabled", false)),
		"group_debt": float(totals.get("group_debt", 0.0)),
		"loans": float(totals.get("loans", 0.0)),
		"bc_borrowing": float(totals.get("bc_borrowing", 0.0)),
		"capital": float(totals.get("capital", 0.0)),
		"equity": float(totals.get("equity", 0.0)),
		"loans_count": int(totals.get("loans_count", 0)),
		"defaults": int(totals.get("defaults", 0)),
		"writeoffs": float(totals.get("writeoffs", 0.0)),
	}


## C6 (D-114): medição da crise no fecho do mundo (só leitura).
func _bank_crisis_report(sim) -> Dictionary:
	if not sim.has_method("get_bank_status"):
		return {}
	var status: Dictionary = sim.get_bank_status()
	var totals: Dictionary = status.get("totals", {})
	var out := {}
	for key in ["confidence_min", "confidence_max", "confidence_sum", "fund",
			"premiums", "payouts", "runs", "run_days", "resolutions",
			"run_withdrawn", "run_cash", "run_public", "capital_flight",
			"lolr_borrowed", "lolr_interest", "insured_payout", "bailin_cut",
			"treasury_rescue", "crisis_events"]:
		out[key] = totals.get(key, 0.0)
	out["insurance"] = status.get("insurance", [])
	out["events"] = status.get("events", [])
	return out


func _banks_totals(sim) -> Dictionary:
	# C4 (D-110): agregados dos bancos na data final (medidos do estado).
	var status: Dictionary = sim.get_bank_status()
	var totals: Dictionary = status.get("totals", {})
	var out := {}
	var keys := ["cash", "capital", "equity", "deposits", "loans",
		"interest_income", "interest_expense", "retained", "dividends",
		"remittance", "remittance_origin", "harvest_interest",
		"defaults", "writeoffs", "loans_count", "m0", "m1"]
	for key in keys:
		out[key] = totals.get(key, 0.0)
	return out


## Percentil linear de uma lista JÁ ordenada (0..1); vazio = 0.
func _pct(values: Array, fraction: float) -> float:
	if values.is_empty():
		return 0.0
	var position := fraction * float(values.size() - 1)
	var low := int(floor(position))
	var high := int(ceil(position))
	var weight := position - float(low)
	return float(values[low]) * (1.0 - weight) + float(values[high]) * weight


func _stock_report(sim, start: Dictionary, final_status: Dictionary,
		formed: Dictionary = {}) -> Dictionary:
	# C5 (D-112): índice das praças no ano (variação por bolsa), confiança
	# final, dividendos/remessa e o investimento medido dos grupos. Só medição.
	if not sim.has_method("get_stock_status"):
		return {}
	var end: Dictionary = sim.get_stock_status()
	var totals: Dictionary = end.get("totals", {})
	var out: Dictionary = {
		"index_global": float(totals.get("index_global", 100.0)),
		"market_cap": float(totals.get("market_cap", 0.0)),
		"book_equity": float(totals.get("book_equity", 0.0)),
		"listed": int(totals.get("listed", 0)),
		"exchanges": int(totals.get("exchanges", 0)),
		"dividends_paid": float(totals.get("dividends_paid", 0.0)),
		"remittance": float(totals.get("remittance", 0.0)),
		"remittance_origin": float(totals.get("remittance_origin", 0.0)),
		"equity_identity": float(totals.get("equity_identity", 0.0)),
	}
	var start_index: Dictionary = {}
	for row: Dictionary in start.get("exchanges", []):
		start_index[String(row.get("nation", ""))] = float(
			row.get("index", 100.0))
	var changes: Array = []
	var confidences: Array = []
	for row: Dictionary in end.get("exchanges", []):
		var nation := String(row.get("nation", ""))
		confidences.append(float(row.get("confidence", 1.0)))
		if not start_index.has(nation):
			continue
		var base := float(start_index[nation])
		if base <= 0.0 or float(row.get("index_base", 0.0)) <= 0.0:
			continue
		changes.append(float(row.get("index", 100.0)) / base - 1.0)
	changes.sort()
	confidences.sort()
	out["exchange_changes"] = changes.size()
	out["variation_p10"] = _pct(changes, 0.10)
	out["variation_p50"] = _pct(changes, 0.50)
	out["variation_p90"] = _pct(changes, 0.90)
	out["confidence_p10"] = _pct(confidences, 0.10)
	out["confidence_p50"] = _pct(confidences, 0.50)
	out["confidence_p90"] = _pct(confidences, 0.90)
	var qs: Array = []
	for row: Dictionary in end.get("tickers", []):
		if bool(row.get("listed", false)):
			qs.append(float(row.get("q", 0.0)))
	qs.sort()
	out["q_p50"] = _pct(qs, 0.50)
	# Investimento total medido: capex acumulado dos grupos (C3/C4).
	var invested := 0.0
	for group: Dictionary in sim.get_groups():
		invested += float(group.get("capex_total", 0.0))
	out["investment_capex_total"] = invested
	out["bankruptcies"] = int(final_status.get("bankruptcies", -1))
	# C5-fix (D-112): variação por PAPEL da formação do índice até o fim do ano,
	# com a causa medida (lucro de referência e confiança da praça).
	var paper_changes: Array = []
	var paper_detail: Array = []
	# D-113: piso de base da variação = preço mínimo de listagem declarado.
	var base_floor := float((end.get("policy", {}) as Dictionary).get(
		"min_listed_price", 0.0))
	var below_base := 0
	var base_ema: Dictionary = {}
	var base_conf: Dictionary = {}
	var base_price: Dictionary = {}
	var base_rate: Dictionary = {}
	var end_rate: Dictionary = {}
	for row: Dictionary in formed.get("exchanges", []):
		base_conf[String(row.get("nation", ""))] = float(
			row.get("confidence", 1.0))
		base_rate[String(row.get("nation", ""))] = float(
			row.get("policy_rate", 0.0))
	for row: Dictionary in end.get("exchanges", []):
		end_rate[String(row.get("nation", ""))] = float(
			row.get("policy_rate", 0.0))
	for row: Dictionary in formed.get("tickers", []):
		base_ema[String(row.get("group_id", ""))] = float(
			row.get("profit_ema", 0.0))
		base_price[String(row.get("group_id", ""))] = float(
			row.get("price", 0.0))
	for row: Dictionary in end.get("tickers", []):
		if not bool(row.get("listed", false)):
			continue
		var group_id := String(row.get("group_id", ""))
		var old_price := float(base_price.get(group_id, 0.0))
		if old_price <= 0.0 or not base_price.has(group_id):
			continue
		# D-113: a bolsa não publica variação percentual sobre base abaixo do
		# piso de listagem declarado. Papel assim é CONTADO, não escondido.
		if old_price < base_floor:
			below_base += 1
			continue
		var new_price := float(row.get("price", 0.0))
		var change := new_price / old_price - 1.0
		var old_ema := float(base_ema.get(group_id, 0.0))
		var new_ema := float(row.get("profit_ema", 0.0))
		var nation := String(row.get("exchange", ""))
		var old_conf := float(base_conf.get(nation, 1.0))
		var new_conf := float(_confidence_of(end, nation))
		paper_changes.append(change)
		paper_detail.append({
			"group_id": group_id,
			"nation": nation,
			"change": change,
			"price_start": old_price,
			"price_end": new_price,
			"profit_ema_start": old_ema,
			"profit_ema_end": new_ema,
			"profit_change": (new_ema / old_ema - 1.0) if old_ema > 0.0 else null,
			"confidence_start": old_conf,
			"confidence_end": new_conf,
			"confidence_change": new_conf / old_conf - 1.0 if old_conf > 0.0 else null,
			"rate_start": float(base_rate.get(nation, 0.0)),
			"rate_end": float(end_rate.get(nation, 0.0)),
			"pe": float(row.get("pe", 0.0)),
			"q": float(row.get("q", 0.0)),
		})
	paper_changes.sort()
	out["paper_changes"] = paper_changes.size()
	out["paper_below_base"] = below_base
	out["paper_base_floor"] = base_floor
	out["paper_variation_p10"] = _pct(paper_changes, 0.10)
	out["paper_variation_p50"] = _pct(paper_changes, 0.50)
	out["paper_variation_p90"] = _pct(paper_changes, 0.90)
	# Calibração pré-C6 (D-113): PORTE do papel no dia de formação — preço e
	# lucro anualizado medido. É o que define micro-cap de base ~0 (a variação
	# relativa sobre preço perto de zero não é informação de mercado).
	var formation_price: Array = []
	var formation_earnings: Array = []
	for row: Dictionary in formed.get("tickers", []):
		formation_price.append(float(row.get("price", 0.0)))
		formation_earnings.append(float(row.get("profit_ema", 0.0)) * 365.0)
	formation_price.sort()
	formation_earnings.sort()
	out["formation_papers"] = formation_price.size()
	out["formation_price_pct"] = {
		"p1": _pct(formation_price, 0.01),
		"p5": _pct(formation_price, 0.05),
		"p10": _pct(formation_price, 0.10),
		"p25": _pct(formation_price, 0.25),
		"p50": _pct(formation_price, 0.50),
		"p90": _pct(formation_price, 0.90),
		"p99": _pct(formation_price, 0.99),
	}
	out["formation_earnings_pct"] = {
		"p1": _pct(formation_earnings, 0.01),
		"p5": _pct(formation_earnings, 0.05),
		"p10": _pct(formation_earnings, 0.10),
		"p25": _pct(formation_earnings, 0.25),
		"p50": _pct(formation_earnings, 0.50),
		"p90": _pct(formation_earnings, 0.90),
		"p99": _pct(formation_earnings, 0.99),
	}
	paper_detail.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return absf(float(a["change"])) > absf(float(b["change"])))
	out["paper_extremes"] = paper_detail.slice(0, 5)
	return out


## Linha do tempo agregada da bolsa (só medição): o que move o índice — lucro
## de referência, patrimônio medido, múltiplo (taxa) ou confiança.
func _stock_timeline_row(sim, day: int) -> Dictionary:
	var status: Dictionary = sim.get_stock_status()
	var totals: Dictionary = status.get("totals", {})
	var earnings := 0.0
	for row: Dictionary in status.get("tickers", []):
		if bool(row.get("listed", false)):
			earnings += float(row.get("profit_ema", 0.0))
	var rates: Array = []
	var confs: Array = []
	for row: Dictionary in status.get("exchanges", []):
		rates.append(float(row.get("policy_rate", 0.0)))
		confs.append(float(row.get("confidence", 1.0)))
	rates.sort()
	confs.sort()
	return {
		"day": day,
		"listed": int(totals.get("listed", 0)),
		"market_cap": float(totals.get("market_cap", 0.0)),
		"book_equity": float(totals.get("book_equity", 0.0)),
		"index_global": float(totals.get("index_global", 100.0)),
		"earnings_daily_sum": earnings,
		"rate_median": _pct(rates, 0.50),
		"confidence_median": _pct(confs, 0.50),
	}


## Confiança da praça no status (1.0 se a nação não aparece).
func _confidence_of(status: Dictionary, nation: String) -> float:
	for row: Dictionary in status.get("exchanges", []):
		if String(row.get("nation", "")) == nation:
			return float(row.get("confidence", 1.0))
	return 1.0


# ---------------------------------------------------------------------------
# E2/E3/E5 (D-117) — diagnóstico fino (só medição; nada move dinheiro).
# ---------------------------------------------------------------------------

## Metadados de goods.json: setor (agricultura/indústria alimentícia), se é
## comida e o tipo (raw/intermediate/final). Uma leitura por rodada.
func _load_goods_meta() -> void:
	_goods_sector.clear()
	_goods_food.clear()
	_goods_kind.clear()
	var goods: Variant = _load(GOODS_PATH)
	if typeof(goods) != TYPE_ARRAY:
		return
	for item: Variant in goods:
		if typeof(item) != TYPE_DICTIONARY:
			continue
		var row: Dictionary = item
		var good_id := String(row.get("id", ""))
		_goods_sector[good_id] = String(row.get("sector", ""))
		_goods_food[good_id] = bool(row.get("food", false))
		_goods_kind[good_id] = String(row.get("kind", ""))


## Retrato dos edifícios (id → última operação conhecida) + energia da cidade.
## Consulta de leitura (`get_city_status`); não altera o mundo nem o status
## leve que alimenta o hash de trajetória.
func _buildings_snapshot(sim: Object, cities: Array, power: Dictionary) -> Dictionary:
	var products: PackedStringArray = sim.get_products()
	var power_cities: Dictionary = power.get("cities", {})
	var out: Dictionary = {}
	for city_row: Dictionary in cities:
		var cid := String(city_row.get("id", ""))
		var city_status: Dictionary = sim.get_city_status(cid)
		var grid_row: Dictionary = power_cities.get(cid, {})
		for building_row: Dictionary in (city_status.get("buildings", []) as Array):
			var output_index := int(building_row.get("output_product", -1))
			var output_name := ""
			if output_index >= 0 and output_index < products.size():
				output_name = products[output_index]
			var building_id := String(building_row.get("id", ""))
			out[building_id] = {
				"building_id": building_id,
				"city": cid,
				"nation": String(city_status.get("nation", "")),
				"sector": output_name,
				"owner": String(building_row.get("owner", "")),
				"revenue": float(building_row.get("revenue", 0.0)),
				"costs": float(building_row.get("costs", 0.0)),
				"profit": float(building_row.get("profit", 0.0)),
				"loss_days": int(building_row.get("loss_days", 0)),
				"capacity": float(building_row.get("capacity", 0.0)),
				"utilization": float(building_row.get("utilization", 0.0)),
				"power_price": float(grid_row.get("price", 0.0)),
				"power_cost_industry": float(
					grid_row.get("power_cost_industry", 0.0)),
				"power_served_ratio": float(
					grid_row.get("served_ratio", 1.0)),
				"power_ration_days": int(grid_row.get("ration_days", 0)),
			}
	return out


## Acumula, por nação, a conta de energia e o PIB do dia (mesma unidade XGS:
## as duas grandezas saem do status do mesmo mundo). A razão anual é o teste de
## escala econômica do preço.
func _accumulate_energy(acc: Dictionary, status: Dictionary,
		power: Dictionary) -> void:
	var power_cities: Dictionary = power.get("cities", {})
	for city_row: Dictionary in (status.get("cities", []) as Array):
		var tag := String(city_row.get("nation", ""))
		if tag.is_empty():
			continue
		var entry: Dictionary = acc.get(tag, {
			"bill": 0.0, "gdp": 0.0, "demand_mwh": 0.0, "served_mwh": 0.0})
		entry["gdp"] = float(entry["gdp"]) \
			+ float(city_row.get("gdp", 0.0))
		var cid := String(city_row.get("id", ""))
		var grid_row: Dictionary = power_cities.get(cid, {})
		if not grid_row.is_empty():
			var served := float(grid_row.get("served_mwh", 0.0))
			entry["bill"] = float(entry["bill"]) \
				+ served * float(grid_row.get("price", 0.0))
			entry["demand_mwh"] = float(entry["demand_mwh"]) \
				+ float(grid_row.get("demand_mwh", 0.0))
			entry["served_mwh"] = float(entry["served_mwh"]) + served
		acc[tag] = entry


## Acumula o fluxo anual de cada ligação (o status por ligação é do dia).
func _accumulate_links(acc: Dictionary, power: Dictionary) -> void:
	var nations: Dictionary = power.get("nations", {})
	for row: Dictionary in (power.get("interconnects", []) as Array):
		var a := String(row.get("a", ""))
		var b := String(row.get("b", ""))
		if a.is_empty() or b.is_empty():
			continue
		var key := a + "-" + b
		var entry: Dictionary = acc.get(key, {
			"a": a, "b": b,
			"capacity_mw": float(row.get("capacity_mw", 0.0)),
			"flow_mwh": 0.0,
			"sent_mwh": 0.0,
			"value": 0.0,
			"flow_days": 0,
			"importer_days": 0,
			"importer_short_days": 0,
			"importers": {},
		})
		var flow := float(row.get("flow_mwh", 0.0))
		entry["flow_mwh"] = float(entry["flow_mwh"]) + flow
		entry["sent_mwh"] = float(entry["sent_mwh"]) \
			+ float(row.get("sent_mwh", 0.0))
		entry["value"] = float(entry["value"]) + float(row.get("value", 0.0))
		if flow > 0.0:
			entry["flow_days"] = int(entry["flow_days"]) + 1
		var importer := String(row.get("importer", ""))
		if not importer.is_empty():
			entry["importer_days"] = int(entry["importer_days"]) + 1
			var seen: Dictionary = entry["importers"]
			seen[importer] = int(seen.get(importer, 0)) + 1
			if nations.has(importer) and float((nations[importer] as Dictionary)
					.get("served_ratio", 1.0)) < 0.995:
				entry["importer_short_days"] = \
					int(entry["importer_short_days"]) + 1
		acc[key] = entry


## Primeiro dia em que a média de 30 d por nação cruza o portão de comida.
func _track_gate_first(history: Array, gate_first: Dictionary,
		day: int) -> void:
	var sums: Dictionary = {}
	for day_ratio: Dictionary in history:
		for tag: Variant in day_ratio:
			var tag_text := String(tag)
			var entry: Dictionary = sums.get(tag_text, {"sum": 0.0, "days": 0})
			entry["sum"] = float(entry["sum"]) + float(day_ratio[tag])
			entry["days"] = int(entry["days"]) + 1
			sums[tag_text] = entry
	for tag: Variant in sums:
		var tag_text := String(tag)
		if gate_first.has(tag_text):
			continue
		var entry: Dictionary = sums[tag]
		var average := float(entry["sum"]) / float(maxi(1, int(entry["days"])))
		if average < GATE:
			gate_first[tag_text] = {"day": day, "food_avg30": average}


## Nações que já cruzavam o portão no controle sem energia (rodada anterior).
func _control_below() -> Array:
	if _control_path.is_empty():
		return []
	var data: Variant = _load(_control_path)
	if typeof(data) != TYPE_DICTIONARY:
		return []
	var gate: Variant = (data as Dictionary).get("food_gate", {})
	if typeof(gate) != TYPE_DICTIONARY:
		return []
	var below: Variant = (gate as Dictionary).get("below", [])
	return below if typeof(below) == TYPE_ARRAY else []


## Preço médio da comida por nação (ponderado por estoque) — base do ano e
## fecho. Mercados só existem no `get_city_status` (passada pesada).
func _food_price_by_nation(sim: Object, status: Dictionary) -> Dictionary:
	var products: PackedStringArray = sim.get_products()
	var raw: Dictionary = {}
	for city_row: Dictionary in (status.get("cities", []) as Array):
		var tag := String(city_row.get("nation", ""))
		if tag.is_empty():
			continue
		var cid := String(city_row.get("id", ""))
		var city_status: Dictionary = sim.get_city_status(cid)
		var entry: Dictionary = raw.get(tag, {"sum": 0.0, "weight": 0.0})
		for market_row: Dictionary in (city_status.get("markets", []) as Array):
			var product_index := int(market_row.get("product", -1))
			if product_index < 0 or product_index >= products.size():
				continue
			var good_id := products[product_index]
			if not bool(_goods_food.get(good_id, false)):
				continue
			var weight := maxf(1.0, float(market_row.get("stock", 0.0)))
			entry["sum"] = float(entry["sum"]) \
				+ float(market_row.get("price", 0.0)) * weight
			entry["weight"] = float(entry["weight"]) + weight
		raw[tag] = entry
	var out: Dictionary = {}
	for tag: Variant in raw:
		var entry: Dictionary = raw[tag]
		var weight := float(entry["weight"])
		out[String(tag)] = float(entry["sum"]) / weight if weight > 0.0 \
			else 0.0
	return out


func _empty_food_entry() -> Dictionary:
	return {
		"population": 0.0,
		"labor_force": 0.0,
		"unemployed": 0.0,
		"satisfaction_weighted": 0.0,
		"food_spend": 0.0,
		"gdp": 0.0,
		"agro_output_t": 0.0,
		"food_output_t": 0.0,
		"food_revenue": 0.0,
		"food_buildings": 0,
		"price_sum": 0.0,
		"price_weight": 0.0,
		"power_demand": 0.0,
		"power_served": 0.0,
		"power_bill": 0.0,
		"max_ration_days": 0,
		"isolated_cities": 0,
	}


## Cadeia do portão: agro, indústria alimentícia, renda/desemprego, preço e
## energia no fecho, para as nações do portão e do controle. Passada pesada
## única (mercados exigem `get_city_status`).
func _food_chain(sim: Object, status: Dictionary, power: Dictionary,
		gate_below: Array, control_below: Array, gate_first: Dictionary,
		price_base: Dictionary) -> Array:
	var wanted: Dictionary = {}
	for tag: Variant in gate_below:
		wanted[String(tag)] = true
	for tag: Variant in control_below:
		wanted[String(tag)] = true
	if wanted.is_empty():
		return []
	var products: PackedStringArray = sim.get_products()
	var power_cities: Dictionary = power.get("cities", {})
	var acc: Dictionary = {}
	for city_row: Dictionary in (status.get("cities", []) as Array):
		var tag := String(city_row.get("nation", ""))
		if not wanted.has(tag):
			continue
		var cid := String(city_row.get("id", ""))
		var city_status: Dictionary = sim.get_city_status(cid)
		var entry: Dictionary = acc.get(tag, _empty_food_entry())
		var pop: Dictionary = city_status.get("pop", {})
		var people := float(pop.get("population", 0.0))
		entry["population"] = float(entry["population"]) + people
		entry["labor_force"] = float(entry["labor_force"]) \
			+ float(pop.get("labor_force", 0.0))
		entry["unemployed"] = float(entry["unemployed"]) \
			+ float(pop.get("unemployed", 0.0))
		entry["satisfaction_weighted"] = \
			float(entry["satisfaction_weighted"]) \
			+ float(pop.get("satisfaction", 0.0)) * people
		entry["food_spend"] = float(entry["food_spend"]) \
			+ float(pop.get("food_spend", 0.0))
		entry["gdp"] = float(entry["gdp"]) \
			+ float(city_status.get("gdp", 0.0))
		for building_row: Dictionary in (city_status.get("buildings", []) as Array):
			var output_index := int(building_row.get("output_product", -1))
			if output_index < 0 or output_index >= products.size():
				continue
			var good_id := products[output_index]
			if not bool(_goods_food.get(good_id, false)):
				continue
			var produced := float(building_row.get("capacity", 0.0)) \
				* float(building_row.get("utilization", 0.0)) \
				* float(building_row.get("output_scale", 0.0))
			if String(_goods_kind.get(good_id, "")) == "raw":
				entry["agro_output_t"] = float(entry["agro_output_t"]) \
					+ produced
			else:
				entry["food_output_t"] = float(entry["food_output_t"]) \
					+ produced
			entry["food_revenue"] = float(entry["food_revenue"]) \
				+ float(building_row.get("revenue", 0.0))
			entry["food_buildings"] = int(entry["food_buildings"]) + 1
		for market_row: Dictionary in (city_status.get("markets", []) as Array):
			var product_index := int(market_row.get("product", -1))
			if product_index < 0 or product_index >= products.size():
				continue
			var good_id := products[product_index]
			if not bool(_goods_food.get(good_id, false)):
				continue
			var weight := maxf(1.0, float(market_row.get("stock", 0.0)))
			entry["price_sum"] = float(entry["price_sum"]) \
				+ float(market_row.get("price", 0.0)) * weight
			entry["price_weight"] = float(entry["price_weight"]) + weight
		var grid_row: Dictionary = power_cities.get(cid, {})
		if not grid_row.is_empty():
			var demand := float(grid_row.get("demand_mwh", 0.0))
			var served := float(grid_row.get("served_mwh", 0.0))
			entry["power_demand"] = float(entry["power_demand"]) + demand
			entry["power_served"] = float(entry["power_served"]) + served
			entry["power_bill"] = float(entry["power_bill"]) \
				+ served * float(grid_row.get("price", 0.0))
			entry["max_ration_days"] = maxi(int(entry["max_ration_days"]),
				int(grid_row.get("ration_days", 0)))
			if String(grid_row.get("connection", "")) == "isolated":
				entry["isolated_cities"] = \
					int(entry["isolated_cities"]) + 1
		acc[tag] = entry
	var rows: Array = []
	for tag: Variant in acc:
		var tag_text := String(tag)
		var entry: Dictionary = acc[tag]
		var people := float(entry["population"])
		var labor := float(entry["labor_force"])
		var weight := float(entry["price_weight"])
		var price := float(entry["price_sum"]) / weight if weight > 0.0 \
			else 0.0
		var base := float(price_base.get(tag_text, 0.0))
		var demand := float(entry["power_demand"])
		var served := float(entry["power_served"])
		var gate_row: Dictionary = {}
		if gate_first.has(tag_text):
			gate_row = gate_first[tag_text]
		rows.append({
			"nation": tag_text,
			"in_gate": gate_below.has(tag_text),
			"in_control": control_below.has(tag_text),
			"gate_day": int(gate_row.get("day", -1)),
			"food_avg30": float(gate_row.get("food_avg30", -1.0)),
			"population": people,
			"gdp": float(entry["gdp"]),
			"gdp_per_capita": float(entry["gdp"]) / people if people > 0.0 \
				else 0.0,
			"unemployment": float(entry["unemployed"]) / labor if labor > 0.0 \
				else 0.0,
			"satisfaction": float(entry["satisfaction_weighted"]) / people \
				if people > 0.0 else 0.0,
			"food_spend_per_capita": float(entry["food_spend"]) / people \
				if people > 0.0 else 0.0,
			"agro_output_t": float(entry["agro_output_t"]),
			"food_output_t": float(entry["food_output_t"]),
			"food_buildings": int(entry["food_buildings"]),
			"food_revenue": float(entry["food_revenue"]),
			"food_price": price,
			"food_price_ratio": price / base if base > 0.0 else 0.0,
			"power_served": served / demand if demand > 0.0 else 1.0,
			"power_price": float(entry["power_bill"]) / served if served > 0.0 \
				else 0.0,
			"power_ration_days": int(entry["max_ration_days"]),
			"isolated_cities": int(entry["isolated_cities"]),
		})
	rows.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return float(a["food_avg30"]) < float(b["food_avg30"]))
	return rows


## Escala econômica do preço: conta de energia / PIB por nação, e a parcela da
## energia no valor industrial (`power_cost_per_value`, já limitado por
## `industry_cost_share_max`). Referência pública no bloco `reference`.
func _energy_economy(acc: Dictionary, power: Dictionary) -> Dictionary:
	var rows: Array = []
	var percents: Array = []
	var total_bill := 0.0
	var total_gdp := 0.0
	for tag: Variant in acc:
		var entry: Dictionary = acc[tag]
		var gdp := float(entry.get("gdp", 0.0))
		if gdp <= 0.0:
			continue
		var bill := float(entry.get("bill", 0.0))
		total_bill += bill
		total_gdp += gdp
		var percent := bill / gdp * 100.0
		percents.append(percent)
		rows.append({
			"nation": String(tag),
			"bill": bill,
			"gdp": gdp,
			"bill_pct_gdp": percent,
			"demand_mwh": float(entry.get("demand_mwh", 0.0)),
			"served_mwh": float(entry.get("served_mwh", 0.0)),
		})
	rows.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return float(a["bill_pct_gdp"]) > float(b["bill_pct_gdp"]))
	percents.sort()
	var industry: Array = []
	var power_cities: Dictionary = power.get("cities", {})
	for cid: Variant in power_cities:
		var value := float((power_cities[cid] as Dictionary)
			.get("power_cost_per_value", 0.0)) * 100.0
		if value > 0.0:
			industry.append(value)
	industry.sort()
	var low_start := maxi(0, rows.size() - 10)
	var sample: Array = []
	for row: Dictionary in rows:
		var tag := String(row["nation"])
		if ENERGY_SAMPLE_NATIONS.has(tag):
			sample.append(row)
	return {
		"countries": rows.size(),
		"bill_pct_gdp": {
			"p10": _pct(percents, 0.10),
			"p50": _pct(percents, 0.50),
			"p90": _pct(percents, 0.90),
			"max": _pct(percents, 1.0),
		},
		"bill_pct_gdp_top": rows.slice(0, mini(10, rows.size())),
		"bill_pct_gdp_low": rows.slice(low_start, rows.size()),
		"aggregate_bill_pct_gdp": total_bill / total_gdp * 100.0 \
			if total_gdp > 0.0 else 0.0,
		"sample": sample,
		"industry_cost_pct_value": {
			"cities": industry.size(),
			"p10": _pct(industry, 0.10),
			"p50": _pct(industry, 0.50),
			"p90": _pct(industry, 0.90),
			"max": _pct(industry, 1.0),
			"cap_pct": 5.0,
		},
		"reference": {
			"bill_pct_gdp_band": [1.5, 4.0],
			"bill_pct_gdp_source": "IEA World Energy Outlook/World Energy "
				+ "Investment; Eurostat electricity prices; Banco Mundial "
				+ "(gasto de eletricidade ~1,5-4% do PIB na maioria dos paises)",
			"industry_cost_pct_value_band": [1.0, 4.0],
			"industry_cost_pct_value_source": "IEA/World Bank: intensidade "
				+ "eletrica industrial ~0,2-0,4 kWh por US$ de valor agregado "
				+ "a preco de ~0,08-0,12 US$/kWh (faixa de custo 1,5-4%)",
		},
	}


## Fluxo anual por ligação e as ligações com capacidade e fluxo zero (a
## pergunta do QA: o exportador vizinho tinha excedente no modelo?).
func _link_report(acc: Dictionary, power: Dictionary) -> Dictionary:
	var static_links: Dictionary = {}
	var data: Variant = _load("res://data/definitions/energy/interconnects.json")
	if typeof(data) == TYPE_DICTIONARY:
		for row: Dictionary in ((data as Dictionary).get("links", []) as Array):
			var key := "%s-%s" % [String(row.get("a", "")),
				String(row.get("b", ""))]
			static_links[key] = row
	var nations: Dictionary = power.get("nations", {})
	var rows: Array = []
	var zero: Array = []
	for key: Variant in acc:
		var key_text := String(key)
		var entry: Dictionary = acc[key]
		var capacity := float(entry.get("capacity_mw", 0.0))
		var flow := float(entry.get("flow_mwh", 0.0))
		var base: Dictionary = static_links.get(key_text, {})
		var importer := String(base.get("importer", ""))
		var importer_row: Dictionary = nations.get(importer, {})
		var exporter := String(base.get("exporter", ""))
		var exporter_row: Dictionary = nations.get(exporter, {})
		var row := {
			"a": String(entry.get("a", "")),
			"b": String(entry.get("b", "")),
			"capacity_mw": capacity,
			"flow_mwh": flow,
			"flow_mw_avg": flow / (24.0 * float(maxi(1, _days))),
			"flow_days": int(entry.get("flow_days", 0)),
			"utilization": flow / (24.0 * capacity * float(maxi(1, _days))) \
				if capacity > 0.0 else 0.0,
			"data_importer": importer,
			"data_exporter": exporter,
			"importer_served_ratio": float(
				importer_row.get("served_ratio", 1.0)),
			"exporter_served_ratio": float(
				exporter_row.get("served_ratio", 1.0)),
			"importer_short_days": int(entry.get("importer_short_days", 0)),
			"importers_seen": entry.get("importers", {}),
		}
		rows.append(row)
		if capacity > 0.0 and flow <= 0.0:
			zero.append(row)
	rows.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return float(a["flow_mwh"]) > float(b["flow_mwh"]))
	zero.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return float(a["importer_served_ratio"]) < float(
			b["importer_served_ratio"]))
	var with_flow := 0
	for row: Dictionary in rows:
		if float(row["flow_mwh"]) > 0.0:
			with_flow += 1
	return {
		"links": rows.size(),
		"links_with_flow": with_flow,
		"links_zero_flow": zero.size(),
		"top_by_flow": rows.slice(0, mini(15, rows.size())),
		"zero_flow": zero.slice(0, mini(30, zero.size())),
	}

