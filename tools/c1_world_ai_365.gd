extends SceneTree

const TmpPaths := preload("res://tools/tmp_paths.gd")

## C1 — mundo real com IA por N dias, com a contabilidade por balanço ligada.
##
##   godot --headless --path . --script res://tools/c1_world_ai_365.gd -- \
##     [--days=365] [--out=docs/economy/perf/c1_world_ai_365.json] [--power=0]
##     [--refining=0] [--refining-diag=1] [--fuel-diag=1] [--oil-diag=1]
##     [--crisis=0]
##
## `--crisis=0` é o braço "sem a sobrevivência em crise" do A/B da D-126 (só
## medição): o JSON é aplicado com `enabled=0`, então o mundo roda como antes
## da D-126. Não existe no jogo: separa o efeito da crise da medição.
##
## `--power=0` é o braço "sem energia" do A/B (só medição): não aplica o dado
## de energia, então o mundo roda como antes da E2/E3. O controle versionado
## (`c6_gate_base.json`) é uma rodada antiga sem energia; esta flag reproduz o
## mesmo braço no código de hoje. `--refining=0` é o braço "sem refino" do A/B
## de falências da F0 (só medição): separa o que a semente de refino causa do
## que já existia. `--refining-diag=1` publica, no dia 1 e no fecho, capacidade,
## produção, vendas, estoque, preço, folha+opex e receita de cada
## `SEED-REFINERY-*` e a oferta de petróleo do cenário.
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
const RefiningScript := preload("res://core/economy/refining_service.gd")
const RegionDataScript := preload("res://core/economy/region_data.gd")
const TrajectoryDigest := preload("res://tools/trajectory_digest.gd")

const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const PORTS_PATH := "res://data/definitions/scenario/ports.json"
const BUILDINGS_PATH := "res://data/definitions/scenario/buildings.json"
const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
## D-125: defaults de balanceamento da eficiência de produção (mesmo caminho
## do autoload; o espelho C++ é o fallback).
const EFFICIENCY_PATH := "res://data/definitions/production/efficiency.json"
## D-126: defaults de balanceamento da sobrevivência em crise (mesmo caminho do
## autoload; o espelho compilado em `sim/crisis.h` é o fallback).
const CRISIS_PATH := "res://data/definitions/economy/crisis.json"
## D-131: cérebro econômico da IA (fase A; preço esperado + previsão de caixa).
const AI_BRAIN_PATH := "res://data/definitions/economy/ai_brain.json"
## G2: semente industrial do dia 0 (participação real × âncora × seed_fraction).
const BOOTSTRAP_PATH := "res://data/definitions/economy/bootstrap_industry.json"
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
# E2/E3 (D-119): braço "sem energia" do A/B, só medição (`--power=0`). Reproduz
# o mundo do controle (c6_gate_base) no código de hoje: sem o dado de energia
# aplicado não há parque, preço, racionamento nem ligação — o resto do mundo é
# o mesmo. Não existe no jogo; é parâmetro de linha de comando.
var _power_on := true
# F0/D-124: braco A/B SEM a semente de refino (`--refining=0`), so medicao.
# Nao existe no jogo: separa as falencias que o refino causa das que ja existem.
var _refining_on := true
# D-126: braço A/B SEM a sobrevivência em crise (`--crisis=0`), só medição.
# Não existe no jogo: separa o efeito da D-126 (demissão, crédito, hibernação,
# leilão) da medição do portão.
var _crisis_on := true
# D-131: braço A/B do cérebro econômico (`--ai-brain=1` / `OGS_C1_AI_BRAIN=1`).
# O JSON commitado vem com `enabled=true` (default do jogo desde a fase C); a
# flag `0` DESLIGA só nesta rodada de laboratório (o caminho off bit a bit).
var _ai_brain_on := false
var _ai_brain_set := false
# G2: braço A/B da semente industrial (`--bootstrap=0` / OGS_C1_BOOTSTRAP=0).
# Sem a flag vale o `enabled` do JSON (default do jogo = ligado).
var _bootstrap_on := true
var _bootstrap_set := false
# F0/D-124: diagnostico por refinaria semeada (`--refining-diag=1`): no dia 1
# e no fecho, capacidade/producao/vendas/estoque/preco/folha+opex/receita de
# cada `SEED-REFINERY-*` e a oferta de petroleo do cenario.
var _refining_diag := false
# D-125: override de laboratorio dos parametros de eficiencia (JSON com merge
# profundo sobre o arquivo commitado). Nao existe no jogo: serve so ao A/B.
var _eff_override_path := ""
# G2: override de laboratorio do cerebro da IA (JSON com merge profundo sobre
# `ai_brain.json`). Serve so ao A/B do potential_demand; nao existe no jogo.
var _brain_override_path := ""
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
# F0 (D-123): diagnóstico de combustíveis (--fuel-diag=1). Mede por nação, no
# dia 1 e no fecho, oferta × demanda de gasolina/diesel/querosene, capacidade
# de refino e frete da comida importada. Só medição: não muda regra nem mundo.
var _fuel_diag := false
# F0 (D-124 item 1): diagnostico de PETROLEO (--oil-diag=1). Mede oferta,
# demanda, preco e compradores por cidade/rota nos dias 1, 135, 136, 167 e no
# fecho, para separar mina sem comprador de preco de mercado. So medicao.
var _oil_diag := false
# D-125 (diagnóstico do algodão, só medição): por fazenda de algodão, o retrato
# a cada snapshot de falência (5 d e dia de mudança do contador), o preço do
# algodão no mundo e a contagem de fazendas de algodão/têxteis por nação.
# Não existe no jogo: serve só para explicar as falências da rodada base.
var _cotton_diag := false
# G2 (perf): custo por fase do nucleo (`set_profiling(true)` + `get_profile`).
# So medicao: instrumenta sem mudar o mundo (mesmo caminho do b6/g2).
var _profile := false
# G2 (perf): tempo de parede medido no GDScript (tick x status) quando
# `--profile=1`; o profiler do nucleo nao cobre a marshalling do Dictionary.
var _prof_tick_us := 0
var _prof_status_us := 0
var _cotton_farms: Dictionary = {}
var _cotton_counts: Array = []
var _cotton_prices: Array = []
var _cotton_last_day := 0
var _cotton_index := -1
# G1: diag de bens (preço MA30 e oferta/produção por nação) — só medição.
var _goods_diag_ids: Array[String] = []
var _goods_diag_prices: Dictionary = {}
var _goods_diag_supply: Dictionary = {}
var _goods_diag_sold: Dictionary = {}
var _goods_price_base: Dictionary = {}
var _products: PackedStringArray = PackedStringArray()
# Agregado do mercado de algodão do snapshot corrente (medido dentro da mesma
# passada pesada de `_buildings_snapshot` — sem uma segunda varredura).
var _cotton_day_prices: Array = []
var _cotton_day_sums: Dictionary = {}
const OIL_SAMPLE_DAYS: Array[int] = [1, 30, 60, 90, 120, 135, 136, 167]
# Receitas que produzem combustível: refinaria (co-produção) e as rotas
# alternativas declaradas em recipes.json (GTL/CTL).
const FUEL_RECIPES: Array[String] = ["gasoline_a", "diesel_a", "diesel_b"]
const FUEL_IDS: Array[String] = ["gasoline", "diesel", "jet_fuel"]
# Bens-controle do diagnóstico: se trigo e petróleo medem oferta/demanda e os
# combustíveis não, o zero é do mercado (não da leitura).
const CONTROL_GOODS: Array[String] = ["wheat", "oil"]
# Metadados de goods.json: setor, se é comida e o tipo (raw/processed).
var _goods_sector: Dictionary = {}
var _goods_food: Dictionary = {}
var _goods_kind: Dictionary = {}


func _init() -> void:
	# Permite rodar pelo runner canônico (tools/run_tests.ps1), que não passa
	# argumentos de usuário: `OGS_C1_OUT` escolhe o arquivo de saída e
	# `OGS_C1_CONTROL` o JSON de controle (o runner de testes não passa args).
	if OS.get_environment("OGS_C1_OUT") != "":
		_out = OS.get_environment("OGS_C1_OUT")
	elif OS.get_environment("OPENGS_TMP").strip_edges() != "":
		# Regra de disco: com OPENGS_TMP definido (o runner sempre define), o
		# JSON grande vai para o disco de temporários, não para o repo nem C:.
		_out = TmpPaths.path("c1_world_ai_365.json")
	# D-131: braço A/B do cérebro econômico pelo runner (que não passa args).
	if OS.get_environment("OGS_C1_AI_BRAIN") != "":
		_ai_brain_on = OS.get_environment("OGS_C1_AI_BRAIN") != "0"
		_ai_brain_set = true
	if OS.get_environment("OGS_C1_BOOTSTRAP") != "":
		_bootstrap_on = OS.get_environment("OGS_C1_BOOTSTRAP") != "0"
		_bootstrap_set = true
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--days="):
			_days = maxi(1, int(arg.split("=", true, 1)[1]))
		elif arg.begins_with("--property="):
			_property = arg.split("=", true, 1)[1] != "0"
		elif arg.begins_with("--power="):
			_power_on = arg.split("=", true, 1)[1] != "0"
		elif arg.begins_with("--refining="):
			_refining_on = arg.split("=", true, 1)[1] != "0"
		elif arg.begins_with("--crisis="):
			_crisis_on = arg.split("=", true, 1)[1] != "0"
		elif arg.begins_with("--ai-brain="):
			_ai_brain_on = arg.split("=", true, 1)[1] != "0"
			_ai_brain_set = true
		elif arg.begins_with("--bootstrap="):
			_bootstrap_on = arg.split("=", true, 1)[1] != "0"
			_bootstrap_set = true
		elif arg.begins_with("--refining-diag"):
			var refining_value := "1"
			if arg.contains("="):
				refining_value = arg.split("=", true, 1)[1]
			_refining_diag = refining_value != "0"
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
		elif arg.begins_with("--eff-override="):
			_eff_override_path = arg.split("=", true, 1)[1]
		elif arg.begins_with("--brain-override="):
			_brain_override_path = arg.split("=", true, 1)[1]
		elif arg.begins_with("--fuel-diag"):
			var fuel_value := "1"
			if arg.contains("="):
				fuel_value = arg.split("=", true, 1)[1]
			_fuel_diag = fuel_value != "0"
		elif arg.begins_with("--oil-diag"):
			var oil_value := "1"
			if arg.contains("="):
				oil_value = arg.split("=", true, 1)[1]
			_oil_diag = oil_value != "0"
		elif arg.begins_with("--cotton-diag"):
			var cotton_value := "1"
			if arg.contains("="):
				cotton_value = arg.split("=", true, 1)[1]
			_cotton_diag = cotton_value != "0"
		elif arg.begins_with("--profile"):
			var profile_value := "1"
			if arg.contains("="):
				profile_value = arg.split("=", true, 1)[1]
			_profile = profile_value != "0"
		elif arg.begins_with("--goods-diag="):
			# G1: lista de bens a vigiar (ex.: chips,microchips). Só medição.
			for raw_id in arg.split("=", true, 1)[1].split(",", false):
				var good_id := raw_id.strip_edges()
				if not good_id.is_empty():
					_goods_diag_ids.append(good_id)
	if _control_path.is_empty() and OS.get_environment("OGS_C1_CONTROL") != "":
		_control_path = OS.get_environment("OGS_C1_CONTROL")
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
		"tariff": float(power.get("average_tariff", 0.0)),
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
		# D-118: âncora salário-dia -> XGS derivada do mundo no seed e os
		# limites efetivos da regra (o JSON continua em salário-dia).
		"salary_day_xgs": float(final.get("salary_day_xgs", 0.0)),
		"price_floor": float(final.get("price_floor", 0.0)),
		"price_ceiling": float(final.get("price_ceiling", 0.0)),
		"average_price": bill / served if served > 0.0 else 0.0,
		"average_tariff": float(final.get("average_tariff", 0.0)),
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


## Merge profundo de dicionários (override de laboratório sobre o JSON base).
func _merge_dict(base: Dictionary, extra: Dictionary) -> void:
	for key: Variant in extra:
		var value: Variant = extra[key]
		if value is Dictionary and base.get(key) is Dictionary:
			_merge_dict(base[key], value)
		else:
			base[key] = value


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
	# D-125: parâmetros da eficiência de produção (mesma ordem do autoload).
	if sim.has_method("set_efficiency_params"):
		var efficiency: Variant = _load(EFFICIENCY_PATH)
		if efficiency is Dictionary:
			var params: Dictionary = efficiency
			if not _eff_override_path.is_empty():
				var override: Variant = _load(_eff_override_path)
				if override is Dictionary:
					_merge_dict(params, override)
				else:
					print("[c1] FALHA: override de eficiência ilegível: "
						+ _eff_override_path)
					return null
			sim.set_efficiency_params(params)
	# D-126: sobrevivência em crise (demissão, crédito de giro, liquidação,
	# hibernação, leilão e área). Antes do mundo, como no autoload. Só medição:
	# os contadores do fecho vão para o JSON de saída.
	if sim.has_method("set_crisis_params"):
		var crisis: Variant = _load(CRISIS_PATH)
		if crisis is Dictionary:
			var params: Dictionary = crisis
			if not _crisis_on:
				params["enabled"] = 0.0
			sim.set_crisis_params(params)
	# D-131 (fase A): cérebro econômico da IA. O JSON commitado vem com
	# enabled=false; a flag de laboratório só troca esse booleano nesta rodada.
	if sim.has_method("set_ai_brain_params"):
		var brain: Variant = _load(AI_BRAIN_PATH)
		if brain is Dictionary:
			var brain_params: Dictionary = brain.duplicate(true)
			if not _brain_override_path.is_empty():
				var override: Variant = _load(_brain_override_path)
				if override is Dictionary:
					_merge_dict(brain_params, override)
					print("[c1] G2 override do cerebro: " + _brain_override_path)
				else:
					print("[c1] FALHA: override do cerebro ilegivel: "
						+ _brain_override_path)
					return null
			if _ai_brain_set:
				brain_params["enabled"] = 1.0 if _ai_brain_on else 0.0
			sim.set_ai_brain_params(brain_params)
			print("[c1] D-131 cérebro da IA: enabled=%s"
				% ["1" if float(brain_params.get("enabled", 0.0)) > 0.5 else "0"])
	sim.set_needs(_load(NEEDS_PATH))
	# G2: semente industrial por produtores reais (mesma ordem do autoload).
	if sim.has_method("set_bootstrap_industry"):
		var bootstrap: Variant = _load(BOOTSTRAP_PATH)
		if bootstrap is Dictionary:
			var bootstrap_params: Dictionary = bootstrap
			if _bootstrap_set:
				bootstrap_params["enabled"] = _bootstrap_on
			sim.set_bootstrap_industry(bootstrap_params)
			print("[c1] G2 bootstrap industrial: enabled=%s"
				% ["1" if bool(bootstrap_params.get("enabled", false)) else "0"])
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
	if _power_on and sim.has_method("set_power_definitions"):
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
	# F0/D-124: refino do dado (EI 2024) calibrado pela demanda do dia 1 e pelo
	# teto da oferta simulada de petroleo; mesma ordem do autoload (ENERGIA ->
	# REFINO -> IA) e `seed_industry` ANTES do refino. `--refining=0` e o braco
	# A/B de medicao (nao existe no jogo).
	if _refining_on:
		var refining := RefiningScript.new()
		var refining_report := refining.apply_to_sim(sim, cities, ports)
		print(("[c1] refino: nações=%d plantas=%d capacidade=%.1f t/dia "
			+ "maduras=%d (%s)") % [
			(refining_report.get("nations", []) as Array).size(),
			int(refining_report.get("applied", 0)),
			float(refining_report.get("capacity_t_per_day", 0.0)),
			int(refining_report.get("matured", 0)),
			String(refining_report.get("reason", "ok"))])
	else:
		print("[c1] refino: DESLIGADO por flag (--refining=0, braço A/B de medição)")
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


## G2 (perf): custo por fase do nucleo (`get_profile`, µs acumulados) em µs/dia.
func _profile_block(sim: Object) -> Dictionary:
	var raw: Dictionary = sim.get_profile()
	var total_us := 0.0
	var per_phase: Dictionary = {}
	for key: Variant in raw:
		var value := float(raw[key])
		per_phase[String(key)] = value
		total_us += value
	var per_day: Dictionary = {}
	for key: Variant in per_phase:
		per_day[key] = float(per_phase[key]) / float(maxi(1, _days))
	return {
		"total_us": total_us,
		"total_us_per_day": total_us / float(maxi(1, _days)),
		"per_phase_us": per_phase,
		"per_phase_us_per_day": per_day,
	}


func _food_ratio(status: Dictionary) -> Dictionary:
	# Dono único da conta (portão e hash): tools/trajectory_digest.gd.
	return TrajectoryDigest.food_ratios(status)


## F0 (D-123): diagnóstico de combustíveis por nação. Mede, no dia `day`:
## oferta e demanda de mercado de gasolina/diesel/querosene, estoque, preço
## local ponderado pelo estoque, capacidade instalada de refino/rotas por
## receita e o frete pago por tonelada de comida importada (ordens do dia).
## Só leitura (`get_city_status`, `get_trade_orders`): nada é movido.
func _fuel_report(sim: Object, status: Dictionary, day: int) -> Dictionary:
	var products: PackedStringArray = sim.get_products()
	var tracked: Dictionary = {}
	for good_id: String in FUEL_IDS + CONTROL_GOODS:
		var index := products.find(good_id)
		if index >= 0:
			tracked[index] = good_id
	var markets: Dictionary = {}
	var refining: Dictionary = {}
	for city_row: Dictionary in (status.get("cities", []) as Array):
		var tag := String(city_row.get("nation", ""))
		if tag.is_empty():
			continue
		var city_status: Dictionary = sim.get_city_status(
			String(city_row.get("id", "")))
		var entry: Dictionary = markets.get(tag, {})
		for market_row: Dictionary in (city_status.get("markets", []) as Array):
			var product_index := int(market_row.get("product", -1))
			if not tracked.has(product_index):
				continue
			var good_id := String(tracked[product_index])
			var row: Dictionary = entry.get(good_id,
				{"supply": 0.0, "demand": 0.0, "desired": 0.0, "sold": 0.0,
				"shortage": 0.0, "stock": 0.0,
				"price_sum": 0.0, "price_weight": 0.0})
			row["supply"] = float(row["supply"]) \
				+ float(market_row.get("supply", 0.0))
			row["demand"] = float(row["demand"]) \
				+ float(market_row.get("demand", 0.0))
			# A necessidade das POPs entra em `desired` (não em `demand`): sem
			# isso o combustível parece não ter consumidor no mundo.
			row["desired"] = float(row["desired"]) \
				+ float(market_row.get("desired", 0.0))
			row["sold"] = float(row["sold"]) \
				+ float(market_row.get("sold", 0.0))
			row["shortage"] = float(row["shortage"]) \
				+ float(market_row.get("shortage", 0.0))
			row["stock"] = float(row["stock"]) \
				+ float(market_row.get("stock", 0.0))
			var weight := maxf(1.0, float(market_row.get("stock", 0.0)))
			row["price_sum"] = float(row["price_sum"]) \
				+ float(market_row.get("price", 0.0)) * weight
			row["price_weight"] = float(row["price_weight"]) + weight
			entry[good_id] = row
		markets[tag] = entry
		var capacity: Dictionary = refining.get(tag, {})
		for building_row: Dictionary in (city_status.get("buildings", []) as Array):
			var recipe := String(building_row.get("recipe", ""))
			if not FUEL_RECIPES.has(recipe):
				continue
			capacity[recipe] = float(capacity.get(recipe, 0.0)) \
				+ float(building_row.get("capacity", 0.0)) \
				* float(building_row.get("output_scale", 0.0))
		refining[tag] = capacity
	# Frete da comida importada: ordens do dia (custo e toneladas por destino).
	var imports: Dictionary = {}
	for order: Dictionary in sim.get_trade_orders():
		var good := String(order.get("good", ""))
		if not bool(_goods_food.get(good, false)):
			continue
		var to_tag := String(order.get("to_nation", ""))
		if to_tag.is_empty():
			continue
		var row: Dictionary = imports.get(to_tag,
			{"tons": 0.0, "freight": 0.0})
		row["tons"] = float(row["tons"]) + float(order.get("amount", 0.0))
		row["freight"] = float(row["freight"]) \
			+ float(order.get("freight_cost", 0.0))
		imports[to_tag] = row
	var rows: Dictionary = {}
	var world: Dictionary = {}
	for good_id: String in FUEL_IDS + CONTROL_GOODS:
		world[good_id] = {"supply": 0.0, "demand": 0.0, "desired": 0.0,
			"sold": 0.0, "shortage": 0.0, "stock": 0.0}
	var world_refining: Dictionary = {}
	for tag: Variant in markets:
		var tag_text := String(tag)
		var entry: Dictionary = markets[tag]
		var row: Dictionary = {"nation": tag_text}
		for good_id: String in FUEL_IDS + CONTROL_GOODS:
			var market: Dictionary = entry.get(good_id, {})
			var supply := float(market.get("supply", 0.0))
			var demand := float(market.get("demand", 0.0))
			var desired := float(market.get("desired", 0.0))
			var sold := float(market.get("sold", 0.0))
			var shortage := float(market.get("shortage", 0.0))
			var stock := float(market.get("stock", 0.0))
			var weight := float(market.get("price_weight", 0.0))
			row["%s_supply" % good_id] = supply
			row["%s_demand" % good_id] = demand
			row["%s_desired" % good_id] = desired
			row["%s_sold" % good_id] = sold
			row["%s_shortage" % good_id] = shortage
			row["%s_stock" % good_id] = stock
			row["%s_price" % good_id] = float(market.get("price_sum", 0.0)) / weight \
				if weight > 0.0 else 0.0
			world[good_id]["supply"] = float(world[good_id]["supply"]) + supply
			world[good_id]["demand"] = float(world[good_id]["demand"]) + demand
			world[good_id]["desired"] = float(world[good_id]["desired"]) + desired
			world[good_id]["sold"] = float(world[good_id]["sold"]) + sold
			world[good_id]["shortage"] = float(world[good_id]["shortage"]) + shortage
			world[good_id]["stock"] = float(world[good_id]["stock"]) + stock
		var capacity: Dictionary = refining.get(tag_text, {})
		var refinery_capacity := float(capacity.get("gasoline_a", 0.0))
		var alternative_capacity := float(capacity.get("diesel_a", 0.0)) \
			+ float(capacity.get("diesel_b", 0.0))
		row["refinery_capacity"] = refinery_capacity
		row["alternative_diesel_capacity"] = alternative_capacity
		world_refining["gasoline_a"] = float(world_refining.get("gasoline_a", 0.0)) \
			+ refinery_capacity
		world_refining["alternative_diesel"] = \
			float(world_refining.get("alternative_diesel", 0.0)) \
			+ alternative_capacity
		var imported: Dictionary = imports.get(tag_text, {})
		var tons := float(imported.get("tons", 0.0))
		row["food_import_t"] = tons
		row["food_freight_per_t"] = float(imported.get("freight", 0.0)) / tons \
			if tons > 0.0 else 0.0
		# Frota: diesel/querosene em estoque contra o alvo (o canal físico do
		# frete — sem diesel o embarque é escalado, mesmo com tarifa baixa).
		if sim.has_method("get_fleet"):
			var fleet: Dictionary = sim.get_fleet(tag_text)
			row["fleet_trucks"] = float(fleet.get("trucks", 0.0))
			row["fleet_ships"] = float(fleet.get("ships", 0.0))
			row["fleet_diesel"] = float(fleet.get("diesel", 0.0))
			row["fleet_diesel_target"] = float(fleet.get("diesel_target", 0.0))
			row["fleet_jet"] = float(fleet.get("jet_fuel", 0.0))
			row["fleet_jet_target"] = float(fleet.get("jet_target", 0.0))
		rows[tag_text] = row
	return {
		"day": day,
		"world": world,
		"world_refining": world_refining,
		"nations": rows,
	}


## Linha curta de console para o diagnóstico de combustível (só leitura).
func _print_fuel_report(report: Dictionary) -> void:
	var world: Dictionary = report.get("world", {})
	var refining: Dictionary = report.get("world_refining", {})
	var parts: Array[String] = []
	for fuel: String in FUEL_IDS:
		var row: Dictionary = world.get(fuel, {})
		parts.append("%s oferta=%.1f demanda=%.1f desejo=%.1f vendido=%.1f estoque=%.0f" % [fuel,
			float(row.get("supply", 0.0)), float(row.get("demand", 0.0)),
			float(row.get("desired", 0.0)), float(row.get("sold", 0.0)),
			float(row.get("stock", 0.0))])
	var check: Array[String] = []
	for good_id: String in CONTROL_GOODS:
		var row: Dictionary = world.get(good_id, {})
		check.append("%s oferta=%.1f demanda=%.1f" % [good_id,
			float(row.get("supply", 0.0)), float(row.get("demand", 0.0))])
	print("[fuel] dia %d | %s | %s | refino gasoline_a=%.1f alt_diesel=%.1f"
		% [int(report.get("day", 0)), " | ".join(parts), " | ".join(check),
			float(refining.get("gasoline_a", 0.0)),
			float(refining.get("alternative_diesel", 0.0))])
	var nations: Dictionary = report.get("nations", {})
	for tag: String in ["KNA", "KWT", "CIV", "USA", "SAU", "RUS"]:
		if not nations.has(tag):
			continue
		var row: Dictionary = nations[tag]
		print(("[fuel] %s diesel oferta=%.2f demanda=%.2f desejo=%.2f preço=%.2f "
			+ "| gas oferta=%.2f demanda=%.2f desejo=%.2f preço=%.2f | jet "
			+ "oferta=%.2f demanda=%.2f | refino=%.1f alt=%.1f | frota "
			+ "diesel=%.1f/%.1f | frete comida=%.2f/t (%.0f t)") % [tag,
			float(row.get("diesel_supply", 0.0)),
			float(row.get("diesel_demand", 0.0)),
			float(row.get("diesel_desired", 0.0)),
			float(row.get("diesel_price", 0.0)),
			float(row.get("gasoline_supply", 0.0)),
			float(row.get("gasoline_demand", 0.0)),
			float(row.get("gasoline_desired", 0.0)),
			float(row.get("gasoline_price", 0.0)),
			float(row.get("jet_fuel_supply", 0.0)),
			float(row.get("jet_fuel_demand", 0.0)),
			float(row.get("refinery_capacity", 0.0)),
			float(row.get("alternative_diesel_capacity", 0.0)),
			float(row.get("fleet_diesel", 0.0)),
			float(row.get("fleet_diesel_target", 0.0)),
			float(row.get("food_freight_per_t", 0.0)),
			float(row.get("food_import_t", 0.0))])


## F0/D-124: diagnóstico por refinaria semeada. Lê `get_city_status` e os
## mercados da cidade (só leitura). Vendas/estoque/preço de cada grau são do
## MERCADO da cidade, não da planta (o status não separa venda por construção);
## como o seed cria no máximo uma refinaria por cidade, na prática é a planta.
## `produced_main` = utilização × capacidade (saída principal, gasolina);
## `folha` = workers × wage e `opex_pago_est` = opex × max(0,05, uso) — o opex
## efetivamente pago também é limitado pelo caixa da firma (núcleo), por isso é
## uma estimativa. `costs` do status já inclui insumo (input_value).
func _refining_report(sim: Object, status: Dictionary, day: int) -> Dictionary:
	var products: PackedStringArray = sim.get_products()
	var fuel_index: Dictionary = {}
	for good_id: String in FUEL_IDS:
		var index := products.find(good_id)
		if index >= 0:
			fuel_index[good_id] = index
	var world_fuel: Dictionary = {}
	for good_id: String in FUEL_IDS:
		world_fuel[good_id] = {"desired": 0.0, "demand": 0.0, "supply": 0.0,
			"sold": 0.0, "stock": 0.0}
	var seeded: Array = []
	var nations: Dictionary = {}
	var world := {"capacity": 0.0, "produced_main": 0.0, "revenue": 0.0,
		"costs": 0.0, "input_value": 0.0, "folha": 0.0, "opex_pago_est": 0.0,
		"profit": 0.0}
	var oil_mine_capacity := 0.0
	var gas_mine_capacity := 0.0
	for city_row: Dictionary in (status.get("cities", []) as Array):
		var cid := String(city_row.get("id", ""))
		var nation := String(city_row.get("nation", ""))
		var city_status: Dictionary = sim.get_city_status(cid)
		var market_by_index: Dictionary = {}
		for market_row: Dictionary in (city_status.get("markets", []) as Array):
			var product_index := int(market_row.get("product", -1))
			market_by_index[product_index] = market_row
		for good_id: String in FUEL_IDS:
			if not fuel_index.has(good_id):
				continue
			var market: Dictionary = market_by_index.get(
				int(fuel_index[good_id]), {})
			var agg: Dictionary = world_fuel[good_id]
			agg["desired"] = float(agg["desired"]) \
				+ float(market.get("desired", 0.0))
			agg["demand"] = float(agg["demand"]) \
				+ float(market.get("demand", 0.0))
			agg["supply"] = float(agg["supply"]) \
				+ float(market.get("supply", 0.0))
			agg["sold"] = float(agg["sold"]) + float(market.get("sold", 0.0))
			agg["stock"] = float(agg["stock"]) + float(market.get("stock", 0.0))
		for building_row: Dictionary in (city_status.get("buildings", []) as Array):
			var building_id := String(building_row.get("id", ""))
			var output := int(building_row.get("output_product", -1))
			# Oferta primária do cenário (todas as minas, não só as semeadas).
			if output >= 0 and output < products.size():
				var output_name := String(products[output])
				if output_name == "oil":
					oil_mine_capacity += float(building_row.get("capacity", 0.0))
				elif output_name == "gas":
					gas_mine_capacity += float(building_row.get("capacity", 0.0))
			if not building_id.begins_with("SEED-REFINERY-"):
				continue
			var capacity := float(building_row.get("capacity", 0.0))
			var utilization := float(building_row.get("utilization", 0.0))
			var workers := float(building_row.get("workers", 0.0))
			var wage := float(building_row.get("wage", 0.0))
			var opex := float(building_row.get("opex", 0.0))
			var last_util := float(building_row.get("last_utilization", 0.0))
			var row := {
				"building_id": building_id,
				"city": cid,
				"nation": nation,
				"capacity": capacity,
				"output_scale": float(building_row.get("output_scale", 0.0)),
				"utilization": utilization,
				"produced_main": utilization * capacity,
				"revenue": float(building_row.get("revenue", 0.0)),
				"costs": float(building_row.get("costs", 0.0)),
				"input_value": float(building_row.get("input_value", 0.0)),
				"jobs": float(building_row.get("jobs", 0.0)),
				"workers": workers,
				"wage": wage,
				"opex": opex,
				"folha": workers * wage,
				"opex_pago_est": opex * clampf(last_util, 0.05, 1.0),
				"profit": float(building_row.get("profit", 0.0)),
				"loss_days": int(building_row.get("loss_days", 0)),
			}
			for good_id: String in FUEL_IDS:
				if not fuel_index.has(good_id):
					continue
				var market: Dictionary = market_by_index.get(
					int(fuel_index[good_id]), {})
				row["%s_sold" % good_id] = float(market.get("sold", 0.0))
				row["%s_stock" % good_id] = float(market.get("stock", 0.0))
				row["%s_price" % good_id] = float(market.get("price", 0.0))
				row["%s_desired" % good_id] = float(market.get("desired", 0.0))
			seeded.append(row)
			world["capacity"] = float(world["capacity"]) + capacity
			world["produced_main"] = float(world["produced_main"]) \
				+ utilization * capacity
			for key in ["revenue", "costs", "input_value", "profit"]:
				world[key] = float(world[key]) + float(row[key])
			world["folha"] = float(world["folha"]) + workers * wage
			world["opex_pago_est"] = float(world["opex_pago_est"]) \
				+ opex * clampf(last_util, 0.05, 1.0)
			var nation_row: Dictionary = nations.get(nation, {
				"capacity": 0.0, "plants": 0})
			nation_row["capacity"] = float(nation_row["capacity"]) + capacity
			nation_row["plants"] = int(nation_row["plants"]) + 1
			nations[nation] = nation_row
	if float(world["capacity"]) > 0.0:
		world["utilization"] = float(world["produced_main"]) \
			/ float(world["capacity"])
	else:
		world["utilization"] = 0.0
	return {
		"day": day,
		"seeded_count": seeded.size(),
		"world": world,
		"world_fuel": world_fuel,
		"oil_mine_capacity_t_per_day": oil_mine_capacity,
		"gas_mine_capacity_t_per_day": gas_mine_capacity,
		"nations": nations,
		"seeded": seeded,
	}


## Linha curta de console do diagnóstico de refino (só leitura).
func _print_refining_report(report: Dictionary) -> void:
	var world: Dictionary = report.get("world", {})
	var fuel: Dictionary = report.get("world_fuel", {})
	var gasoline: Dictionary = fuel.get("gasoline", {})
	var diesel: Dictionary = fuel.get("diesel", {})
	var jet: Dictionary = fuel.get("jet_fuel", {})
	print(("[refino] dia %d | plantas=%d capacidade=%.1f t/dia uso=%.3f | "
		+ "receita=%.0f custos=%.0f lucro=%.0f | gasolina desejo=%.1f "
		+ "vendido=%.1f oferta=%.1f | diesel vendido=%.1f | querosene vendido=%.1f "
		+ "| mina petroleo=%.1f t/dia") % [
		int(report.get("day", 0)), int(report.get("seeded_count", 0)),
		float(world.get("capacity", 0.0)), float(world.get("utilization", 0.0)),
		float(world.get("revenue", 0.0)), float(world.get("costs", 0.0)),
		float(world.get("profit", 0.0)), float(gasoline.get("desired", 0.0)),
		float(gasoline.get("sold", 0.0)), float(gasoline.get("supply", 0.0)),
		float(diesel.get("sold", 0.0)), float(jet.get("sold", 0.0)),
		float(report.get("oil_mine_capacity_t_per_day", 0.0))])


## F0 (D-124 item 1): diagnostico de PETROLEO. Mede, no dia `day`, oferta,
## demanda, desejo, vendido, estoque e preco do petroleo no mundo e por nacao;
## o estado de cada mina de petroleo (capacidade, uso, receita, lucro,
## loss_days); e os pedidos de comercio de petroleo por rota (origem ->
## destino, quantidade, frete, preco entregue). So leitura.
func _oil_report(sim: Object, status: Dictionary, day: int) -> Dictionary:
	var products: PackedStringArray = sim.get_products()
	var oil_index := products.find("oil")
	if oil_index < 0:
		return {"day": day, "reason": "core_without_oil"}
	var world := {"supply": 0.0, "demand": 0.0, "desired": 0.0, "sold": 0.0,
		"stock": 0.0, "price_sum": 0.0, "price_weight": 0.0}
	var nations: Dictionary = {}
	var cities: Array = []
	for city_row: Dictionary in (status.get("cities", []) as Array):
		var city_id := String(city_row.get("id", ""))
		var nation := String(city_row.get("nation", ""))
		var city_status: Dictionary = sim.get_city_status(city_id)
		var market: Dictionary = {}
		for market_row: Dictionary in (city_status.get("markets", []) as Array):
			if int(market_row.get("product", -1)) == oil_index:
				market = market_row
				break
		var mine_rows: Array = []
		for building_row: Dictionary in (city_status.get("buildings", []) as Array):
			if int(building_row.get("output_product", -1)) != oil_index:
				continue
			mine_rows.append({
				"id": String(building_row.get("id", "")),
				"capacity": float(building_row.get("capacity", 0.0)),
				"utilization": float(building_row.get("utilization", 0.0)),
				"revenue": float(building_row.get("revenue", 0.0)),
				"profit": float(building_row.get("profit", 0.0)),
				"loss_days": int(building_row.get("loss_days", 0)),
			})
		# Agregado do mundo/nação pelo mercado da cidade.
		var stock := float(market.get("stock", 0.0))
		var weight := maxf(1.0, stock)
		for key in ["supply", "demand", "desired", "sold", "stock"]:
			world[key] = float(world[key]) + float(market.get(key, 0.0))
		world["price_sum"] = float(world["price_sum"]) \
			+ float(market.get("price", 0.0)) * weight
		world["price_weight"] = float(world["price_weight"]) + weight
		var nation_row: Dictionary = nations.get(nation,
			{"supply": 0.0, "demand": 0.0, "desired": 0.0, "sold": 0.0,
			"stock": 0.0, "price_sum": 0.0, "price_weight": 0.0,
			"mines": 0, "mine_capacity": 0.0})
		for key in ["supply", "demand", "desired", "sold", "stock"]:
			nation_row[key] = float(nation_row[key]) + float(market.get(key, 0.0))
		nation_row["price_sum"] = float(nation_row["price_sum"]) \
			+ float(market.get("price", 0.0)) * weight
		nation_row["price_weight"] = float(nation_row["price_weight"]) + weight
		nation_row["mines"] = int(nation_row["mines"]) + mine_rows.size()
		for mine: Dictionary in mine_rows:
			nation_row["mine_capacity"] = float(nation_row["mine_capacity"]) \
				+ float(mine["capacity"])
		nations[nation] = nation_row
		# Cidade entra no retrato se tem mina de petroleo ou e dos EUA com
		# refinaria (as duas minas que quebram e as plantas vizinhas).
		var is_us := nation == "USA"
		var has_refinery := false
		if is_us:
			for building_row: Dictionary in (city_status.get("buildings", []) as Array):
				if String(building_row.get("id", "")).begins_with("SEED-REFINERY-"):
					has_refinery = true
					break
		if mine_rows.is_empty() and not has_refinery:
			continue
		cities.append({
			"city": city_id, "nation": nation,
			"mine": mine_rows,
			"oil": {
				"supply": float(market.get("supply", 0.0)),
				"demand": float(market.get("demand", 0.0)),
				"desired": float(market.get("desired", 0.0)),
				"sold": float(market.get("sold", 0.0)),
				"stock": stock,
				"price": float(market.get("price", 0.0)),
			},
		})
	if float(world["price_weight"]) > 0.0:
		world["price"] = float(world["price_sum"]) / float(world["price_weight"])
	else:
		world["price"] = 0.0
	for tag: Variant in nations:
		var row: Dictionary = nations[tag]
		row["price"] = float(row["price_sum"]) / float(row["price_weight"]) \
			if float(row["price_weight"]) > 0.0 else 0.0
	# Pedidos de petroleo do ultimo tick: quem compra de quem e por quanto.
	var routes: Dictionary = {}
	var us_routes: Array = []
	for order: Dictionary in sim.get_trade_orders():
		if String(order.get("good", "")) != "oil":
			continue
		var key := "%s>%s" % [String(order.get("from_city", "")),
			String(order.get("to_city", ""))]
		var row: Dictionary = routes.get(key, {
			"from_city": String(order.get("from_city", "")),
			"to_city": String(order.get("to_city", "")),
			"from_nation": String(order.get("from_nation", "")),
			"to_nation": String(order.get("to_nation", "")),
			"amount": 0.0, "freight": 0.0, "orders": 0,
			"delivered_price": float(order.get("delivered_price", 0.0)),
			"origin_price": float(order.get("origin_price", 0.0)),
			"transit_hours": float(order.get("transit_hours", 0.0)),
		})
		row["amount"] = float(row["amount"]) + float(order.get("amount", 0.0))
		row["freight"] = float(row["freight"]) + float(order.get("freight_cost", 0.0))
		row["orders"] = int(row["orders"]) + 1
		routes[key] = row
		if String(order.get("from_nation", "")) == "USA" \
				or String(order.get("to_nation", "")) == "USA":
			us_routes.append(order)
	return {
		"day": day,
		"world": world,
		"nations": nations,
		"cities": cities,
		"routes": routes.values(),
		"us_orders": us_routes,
	}


func _print_oil_report(report: Dictionary) -> void:
	var world: Dictionary = report.get("world", {})
	var usa: Dictionary = (report.get("nations", {}) as Dictionary).get("USA", {})
	print(("[oleo] dia %d | mundo oferta=%.1f demanda=%.1f desejo=%.1f "
		+ "vendido=%.1f estoque=%.1f preco=%.2f | USA oferta=%.1f demanda=%.1f "
		+ "vendido=%.1f preco=%.2f minas=%d cap=%.1f") % [
		int(report.get("day", 0)), float(world.get("supply", 0.0)),
		float(world.get("demand", 0.0)), float(world.get("desired", 0.0)),
		float(world.get("sold", 0.0)), float(world.get("stock", 0.0)),
		float(world.get("price", 0.0)), float(usa.get("supply", 0.0)),
		float(usa.get("demand", 0.0)), float(usa.get("sold", 0.0)),
		float(usa.get("price", 0.0)), int(usa.get("mines", 0)),
		float(usa.get("mine_capacity", 0.0))])


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
		# F0/D-123: a média de 30 d de TODAS as nações no fecho. `gate_first`
		# guarda o valor do dia em que a nação cruzou (no dia 1 um importador
		# sem estoque mede 0) — este bloco é o número do portão.
		"ratios": values,
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
	# D-125: índice do algodão no mundo (diagnóstico do algodão).
	_products = sim.get_products()
	_cotton_index = _products.find("cotton")
	if _profile and sim.has_method("set_profiling"):
		sim.set_profiling(true)
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
	var fuel_reports: Dictionary = {}
	# F0/D-124: diagnostico por refinaria semeada (so com --refining-diag=1).
	var refining_reports: Dictionary = {}
	# F0/D-124 item 1: diagnostico de petroleo (so com --oil-diag=1).
	var oil_reports: Dictionary = {}
	_load_goods_meta()

	for day in _days:
		if _fx_seed_days > 0 and day == 30:
			# Câmbio: as reservas do cenário são 30 dias da importação MEDIDA do
			# mundo (dia 30; no dia 0 não há comércio).
			var seeded: float = sim.seed_central_bank_reserves(_fx_seed_days)
			print("[c1] cenário C2b2: reservas = %.3f âncora no dia 30" % seeded)
		var _tick_start := Time.get_ticks_usec() if _profile else 0
		var summary: Dictionary = sim.tick_day()
		if _profile:
			_prof_tick_us += Time.get_ticks_usec() - _tick_start
		for violation in Array(summary.get("invariant_violations", [])):
			violation_count += 1
			if violation_samples.size() < 10:
				violation_samples.append("dia %d: %s"
					% [day + 1, String(violation)])
		var day_now := int(summary.get("day", 0))
		if day_now <= day_before:
			stalled += 1
		day_before = day_now
		var _status_start := Time.get_ticks_usec() if _profile else 0
		var status: Dictionary = sim.get_status()
		if _profile:
			_prof_status_us += Time.get_ticks_usec() - _status_start
		var money := float(status.get("money_total", 0.0))
		if money_start > 0.0:
			max_drift = maxf(max_drift, absf(money - money_start) / money_start)
		var ratio := _food_ratio(status)
		history.append(ratio)
		if history.size() > WINDOW:
			history.pop_front()
		if not _goods_diag_ids.is_empty():
			_goods_diag_sample(day_now, status)
		if _fuel_diag and day_now == 1:
			# F0 (D-123): retrato do dia 1 — antes de qualquer obra da IA.
			var fuel_day1 := _fuel_report(sim, status, day_now)
			fuel_reports["day_1"] = fuel_day1
			_print_fuel_report(fuel_day1)
		if _refining_diag and day_now == 1:
			refining_reports["day_1"] = _refining_report(sim, status, day_now)
		if _oil_diag and OIL_SAMPLE_DAYS.has(day_now):
			var oil_sample := _oil_report(sim, status, day_now)
			oil_reports["day_%d" % day_now] = oil_sample
			_print_oil_report(oil_sample)
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
			if _cotton_diag:
				_cotton_accumulate(day_now, snapshot, status)
		if day_now == 1:
			# Preço-base da comida para medir o repasse no ano (uma passada
			# pesada no começo; o resto lê só mercados das nações do portão).
			food_price_base = _food_price_by_nation(sim, status)
		var balance_block: Dictionary = status.get("balance", {})
		# C2b1-fix (D-105): linha canônica única (dinheiro 6 casas, falências,
		# comida, M0, estoque privado, crédito). Ver
		# tools/trajectory_digest.gd e docs/economy/inv_c2b1_gate/README.md.
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
	if _fuel_diag:
		# F0 (D-123): retrato do fecho (mesmo dia do portão, média de 30 d).
		var fuel_final := _fuel_report(sim, status_final,
			int(status_final.get("day", 0)))
		fuel_reports["final"] = fuel_final
		_print_fuel_report(fuel_final)
	if _refining_diag:
		var refining_final := _refining_report(sim, status_final,
			int(status_final.get("day", 0)))
		refining_reports["final"] = refining_final
		_print_refining_report(refining_final)
	if _oil_diag:
		var oil_final := _oil_report(sim, status_final,
			int(status_final.get("day", 0)))
		oil_reports["final"] = oil_final
		_print_oil_report(oil_final)
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
		control_below, gate_first, food_price_base, food.get("ratios", []))
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
		# D-126: soma assinada do M1 criado/destruido nos dias validos. O
		# invariante fecha `money_end - money_start = emission_total + este
		# contador`; o drift de credito e' este valor / money_start.
		"bank_money_delta_total": float(
			status_final.get("bank_money_delta_total", 0.0)),
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
		# D-126: sobrevivência em crise no fecho — demissões/rescisão, crédito de
		# giro e renegociações, liquidações ao exterior, hibernações/reaberturas,
		# leilões (com/sem comprador) e redução de área. Só medição.
		"crisis": _crisis_report(sim),
		"crisis_on": _crisis_on,
		"crisis_entities": _crisis_entities(sim),
		# D-131 (fase A): parâmetros/contadores do cérebro e investimentos de
		# plantas novas emitidos pela IA. Só medição.
		"ai_brain": _ai_brain_report(sim),
		"stock": _stock_report(sim, stock_start, status_final, stock_formed),
		"stock_timeline": stock_timeline,
		"monetary_timeline": monetary_timeline,
		# PARTE E (D-116): energia — preço médio, % servida, racionamento,
		# conexão das cidades e geração por fonte. Só medição.
		"power": _power_digest(sim, power_samples) if _power_on else {},
		"power_timeline": power_timeline,
		"power_off": not _power_on,
		# E2/E3/E5 (D-117) — diagnóstico por entidade e escala econômica.
		# Falência = edifício da firma apagado pelo núcleo; `bank_events` cobre
		# banco (crise C6) e `service_bankruptcies` cobre firma de serviço.
		"bankruptcies_detail": bankruptcies_detail,
		"service_bankruptcies": service_failed,
		"food_chain": food_chain,
		"food_gate_extras": gate_extras,
		"food_gate_control": _control_path,
		"goods_diag": _goods_diag_report() if not _goods_diag_ids.is_empty() else {},
		"efficiency_override": _eff_override_path,
		"brain_override": _brain_override_path,
		"fuel_diag": fuel_reports,
		"refining_diag": refining_reports,
		"oil_diag": oil_reports,
		"energy_economy": _energy_economy(energy_by_nation, power_final) \
			if _power_on else {},
		"interconnect_links": _link_report(link_flow, power_final) \
			if _power_on else {},
		"wall_s": (Time.get_ticks_msec() - wall_start) / 1000.0,
	}
	# G2 (perf): custo por fase do nucleo (us/dia); so instrumentacao.
	if _profile:
		out["profile"] = _profile_block(sim)
		out["profile"]["harness_tick_us_per_day"] = \
			float(_prof_tick_us) / float(maxi(1, _days))
		out["profile"]["harness_status_us_per_day"] = \
			float(_prof_status_us) / float(maxi(1, _days))
	if _fx_seed_days > 0:
		out["fx_scenario"] = _fx_report(sim, _fx_seed_days)
	if _cotton_diag:
		# D-125: diagnóstico do algodão (só medição).
		out["cotton_diag"] = _cotton_report(sim)
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
	if _cotton_diag:
		print(_cotton_summary(out.get("cotton_diag", {}), bankruptcies_detail))
	if not _goods_diag_ids.is_empty():
		_print_goods_diag(out["goods_diag"])
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
	var d126: Dictionary = out.get("crisis", {})
	if not d126.is_empty():
		print("[c1] D-126: demissões=%d vagas_cortadas=%.1f rescisão=%.0f "
			% [int(d126.get("layoffs", 0)), float(d126.get("jobs_cut", 0.0)),
				float(d126.get("severance_paid", 0.0))]
			+ "crédito=%.0f renegociações=%d multa=%.0f "
			% [float(d126.get("loans", 0.0)),
				int(d126.get("renegotiations", 0)),
				float(d126.get("penalty_paid", 0.0))]
			+ "liquidações=%d (%.0f t) hibernações=%d reaberturas=%d "
			% [int(d126.get("liquidations", 0)),
				float(d126.get("liquidated_t", 0.0)),
				int(d126.get("mothballs", 0)), int(d126.get("reopens", 0))]
			+ "leilões=%d sem_comprador=%d usinas=%d área=%d"
			% [int(d126.get("auctions", 0)), int(d126.get("auctions_unsold", 0)),
				int(d126.get("power_auctions", 0)),
				int(d126.get("farm_area_reductions", 0))])
		var entities: Dictionary = out.get("crisis_entities", {})
		if not entities.is_empty():
			print("[c1] D-126 hibernados por produto (top): %s | usinas=%d | estágios=%s"
				% [str(entities.get("mothballed_by_product", {})),
					int(entities.get("mothballed_plants", 0)),
					str(entities.get("buildings_by_stage", {}))])
	var d131: Dictionary = out.get("ai_brain", {})
	if not d131.is_empty():
		print("[c1] D-131 IA: enabled=%s tentativas=%d rejeitadas=%d (%.1f%%) "
			% ["1" if bool(d131.get("enabled", false)) else "0",
				int(d131.get("attempts", 0)), int(d131.get("rejected", 0)),
				float(d131.get("reject_share", 0.0)) * 100.0]
			+ "investimentos=%d (privado=%d grupo=%d)"
			% [int(d131.get("investments_total", 0)),
				int(d131.get("investments_private", 0)),
				int(d131.get("investments_group", 0))])
		print("[c1] D-131 B: candidatos únicos/mês=%d rejeitados caixa/mês=%d "
			+ "risco=%d anti-manada máx 30d=%d"
			% [int(d131.get("unique_candidates_month", 0)),
				int(d131.get("unique_rejected_month", 0)),
				int(d131.get("risk_rejected", 0)),
				int(d131.get("herd_max_plants_30d", 0))])
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


## D-131 (fase A): parâmetros/contadores do cérebro da IA e as plantas novas
## emitidas pela IA (`build` AI-* e `group_invest` AI-GRP-*). Só medição.
func _ai_brain_report(sim) -> Dictionary:
	if not sim.has_method("get_ai_brain_status"):
		return {}
	var status: Dictionary = sim.get_ai_brain_status()
	var private_builds := 0
	var group_builds := 0
	for entry: Variant in sim.get_command_log():
		if not entry is Dictionary:
			continue
		var command: Dictionary = entry
		var kind := String(command.get("type", ""))
		var strings: Array = command.get("s", [])
		if strings.size() < 2:
			continue
		var target := String(strings[1])
		if kind == "build" and target.begins_with("AI-") \
				and not target.begins_with("AI-GRP-"):
			private_builds += 1
		elif kind == "group_invest" and target.begins_with("AI-GRP-"):
			group_builds += 1
	return {
		"enabled": float((status.get("params", {}) as Dictionary)
			.get("enabled", 0.0)) > 0.5,
		"params": status.get("params", {}),
		"attempts": int(status.get("attempts", 0)),
		"rejected": int(status.get("rejected", 0)),
		"reject_share": float(status.get("reject_share", 0.0)),
		"investments_private": private_builds,
		# O log do grupo usa o id `GRP-*` (não `AI-GRP-*`); o contador do
		# núcleo é a fonte do que foi REALMENTE criado.
		"investments_group": int(status.get("group_created", group_builds)),
		"investments_total": private_builds
			+ int(status.get("group_created", group_builds)),
		"unique_candidates_month":
			int(status.get("unique_candidates_month", 0)),
		"unique_rejected_month":
			int(status.get("unique_rejected_month", 0)),
		"risk_rejected": int(status.get("risk_rejected", 0)),
		# D-131 fase C: distribuição por gate e contadores do grupo.
		"exposure_rejected": int(status.get("exposure_rejected", 0)),
		"herd_rejected": int(status.get("herd_rejected", 0)),
		"loss_blocked": int(status.get("loss_blocked", 0)),
		"competition_skipped": int(status.get("competition_skipped", 0)),
		"group_attempts": int(status.get("group_attempts", 0)),
		"group_rejected": int(status.get("group_rejected", 0)),
		"herd_max_plants_30d": int(status.get("herd_max_plants_30d", 0)),
		# D-131 fase C (diagnóstico): reprovações por gate e amostras.
		"gate_rejected": status.get("gate_rejected", {}),
		"group_gate_rejected": status.get("group_gate_rejected", {}),
		"samples": status.get("samples", []),
		"group_samples": status.get("group_samples", []),
		"zero_revenue": int(status.get("zero_revenue", 0)),
		"group_zero_revenue": int(status.get("group_zero_revenue", 0)),
		"group_executed": int(status.get("group_executed", 0)),
		"group_created": int(status.get("group_created", 0)),
	}


## D-126: contadores da sobrevivência em crise no fecho (só medição). O status
## do núcleo é a fonte única; o nome do campo segue o contrato do `crisis.json`.
func _crisis_report(sim) -> Dictionary:
	if not sim.has_method("get_crisis_status"):
		return {}
	var status: Dictionary = sim.get_crisis_status()
	var params: Dictionary = status.get("params", {})
	return {
		"enabled": float(params.get("enabled", 0.0)) > 0.5,
		"layoffs": int(status.get("layoffs", 0)),
		"jobs_cut": float(status.get("jobs_cut", 0.0)),
		"severance_paid": float(status.get("severance_paid", 0.0)),
		"loans": float(status.get("loans", 0.0)),
		"renegotiations": int(status.get("renegotiations", 0)),
		"penalty_paid": float(status.get("penalty_paid", 0.0)),
		"liquidations": int(status.get("liquidations", 0)),
		"liquidated_t": float(status.get("liquidated_t", 0.0)),
		"liquidated_value": float(status.get("liquidated_value", 0.0)),
		"mothballs": int(status.get("mothballs", 0)),
		"reopens": int(status.get("reopens", 0)),
		"reopen_paid": float(status.get("reopen_paid", 0.0)),
		"auctions": int(status.get("auctions", 0)),
		"auction_value": float(status.get("auction_value", 0.0)),
		"auctions_unsold": int(status.get("auctions_unsold", 0)),
		"farm_area_reductions": int(status.get("farm_area_reductions", 0)),
		"power_auctions": int(status.get("power_auctions", 0)),
		"power_auction_value": float(status.get("power_auction_value", 0.0)),
	}


## D-126: onde a crise está, por produto (só medição). Ajuda a separar a
## hibernação de fazenda (sazonal) da de indústria quando o portão se move.
func _crisis_entities(sim) -> Dictionary:
	if not sim.has_method("get_crisis_status"):
		return {}
	var status: Dictionary = sim.get_crisis_status()
	var by_product: Dictionary = {}
	var stage_counts: Dictionary = {}
	for row_value: Variant in status.get("buildings", []):
		var row: Dictionary = row_value
		var stage := int(row.get("crisis_stage", 0))
		if stage > 0:
			var stage_key := str(stage)
			stage_counts[stage_key] = int(stage_counts.get(stage_key, 0)) + 1
		if bool(row.get("mothballed", false)):
			var product := String(row.get("output_product", "?"))
			by_product[product] = int(by_product.get(product, 0)) + 1
	var top: Array = []
	for product: Variant in by_product:
		top.append([String(product), int(by_product[product])])
	top.sort_custom(func(a: Array, b: Array) -> bool:
		return int(a[1]) > int(b[1]))
	var top_dict: Dictionary = {}
	for index in mini(top.size(), 12):
		top_dict[String(top[index][0])] = int(top[index][1])
	var plants_mothballed := 0
	for plant_value: Variant in status.get("power_plants", []):
		if bool((plant_value as Dictionary).get("mothballed", false)):
			plants_mothballed += 1
	return {
		"mothballed_by_product": top_dict,
		"mothballed_products": top.size(),
		"mothballed_plants": plants_mothballed,
		"buildings_by_stage": stage_counts,
	}


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
		_goods_price_base[good_id] = float(row.get("base_price", 0.0))


## Retrato dos edifícios (id → última operação conhecida) + energia da cidade.
## Consulta de leitura (`get_city_status`); não altera o mundo nem o status
## leve que alimenta o hash de trajetória.
func _buildings_snapshot(sim: Object, cities: Array, power: Dictionary) -> Dictionary:
	var products: PackedStringArray = sim.get_products()
	var power_cities: Dictionary = power.get("cities", {})
	var out: Dictionary = {}
	if _cotton_diag:
		_cotton_day_prices = []
		_cotton_day_sums = {"sold": 0.0, "supply": 0.0, "demand": 0.0,
			"stock": 0.0, "shortage": 0.0, "surplus": 0.0}
	for city_row: Dictionary in cities:
		var cid := String(city_row.get("id", ""))
		var city_status: Dictionary = sim.get_city_status(cid)
		var grid_row: Dictionary = power_cities.get(cid, {})
		var markets: Array = city_status.get("markets", [])
		if _cotton_diag and _cotton_index >= 0 and _cotton_index < markets.size():
			_cotton_market_add(markets[_cotton_index])
		for building_row: Dictionary in (city_status.get("buildings", []) as Array):
			var output_index := int(building_row.get("output_product", -1))
			var output_name := ""
			if output_index >= 0 and output_index < products.size():
				output_name = products[output_index]
			var market_row: Dictionary = {}
			if output_index >= 0 and output_index < markets.size():
				market_row = markets[output_index]
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
				# D-125 (diagnóstico do algodão): equipe, eficiência e mercado
				# do produto da própria construção.
				"jobs": float(building_row.get("jobs", 0.0)),
				"workers": float(building_row.get("workers", 0.0)),
				"efficiency": float(building_row.get(
					"production_efficiency", -1.0)),
				"equipment": float(building_row.get("equipment", -1.0)),
				"last_utilization": float(building_row.get(
					"last_utilization", -1.0)),
				"market_price": float(market_row.get("price", 0.0)),
				"market_sold": float(market_row.get("sold", 0.0)),
				"market_stock": float(market_row.get("stock", 0.0)),
				"market_supply": float(market_row.get("supply", 0.0)),
				"market_demand": float(market_row.get("demand", 0.0)),
				"market_shortage": float(market_row.get("shortage", 0.0)),
				"market_surplus": float(market_row.get("surplus", 0.0)),
				"power_price": float(grid_row.get("price", 0.0)),
				"power_cost_industry": float(
					grid_row.get("power_cost_industry", 0.0)),
				"power_served_ratio": float(
					grid_row.get("served_ratio", 1.0)),
				"power_ration_days": int(grid_row.get("ration_days", 0)),
			}
	return out


# --- D-125: diagnóstico do algodão (só medição) ------------------------------
## Retrato por snapshot de falência: cada fazenda de algodão com equipe,
## eficiência, produção, receita/custo e mercado; a oferta/demanda de algodão
## no mundo; e a contagem de algodão/têxteis por nação. Nada muda o mundo.
func _cotton_accumulate(day_now: int, snapshot: Dictionary,
		status: Dictionary) -> void:
	var firm_cash: Dictionary = {}
	for city_row: Dictionary in (status.get("cities", []) as Array):
		firm_cash[String(city_row.get("id", ""))] = float(
			city_row.get("firm_cash", 0.0))
	var counts := {"day": day_now, "cotton_world": 0, "cotton_by_nation": {},
		"fabric_world": 0, "fabric_by_nation": {},
		"clothes_world": 0, "clothes_by_nation": {}}
	for building_id: Variant in snapshot:
		var record: Dictionary = snapshot[building_id]
		var sector := String(record.get("sector", ""))
		var nation := String(record.get("nation", ""))
		if sector == "fabric":
			counts["fabric_world"] = int(counts["fabric_world"]) + 1
			(counts["fabric_by_nation"] as Dictionary)[nation] = int(
				(counts["fabric_by_nation"] as Dictionary).get(nation, 0)) + 1
		elif sector == "clothes":
			counts["clothes_world"] = int(counts["clothes_world"]) + 1
			(counts["clothes_by_nation"] as Dictionary)[nation] = int(
				(counts["clothes_by_nation"] as Dictionary).get(nation, 0)) + 1
		elif sector != "cotton":
			continue
		counts["cotton_world"] = int(counts["cotton_world"]) + 1
		(counts["cotton_by_nation"] as Dictionary)[nation] = int(
			(counts["cotton_by_nation"] as Dictionary).get(nation, 0)) + 1
		var entry: Dictionary = _cotton_farms.get(String(building_id), {})
		if entry.is_empty():
			entry = {
				"building_id": String(building_id),
				"city": String(record.get("city", "")),
				"nation": nation,
				"owner": String(record.get("owner", "")),
				"ai_built": String(building_id).begins_with("AI-"),
				"first_seen_day": day_now,
				# Janela honesta do nascimento: entre o snapshot anterior e este.
				"born_after_day": _cotton_last_day,
				"rows": [],
			}
			_cotton_farms[String(building_id)] = entry
		(entry["rows"] as Array).append({
			"day": day_now,
			"firm_cash": float(firm_cash.get(String(record.get("city", "")),
				0.0)),
			"jobs": float(record.get("jobs", 0.0)),
			"workers": float(record.get("workers", 0.0)),
			"capacity": float(record.get("capacity", 0.0)),
			"efficiency": float(record.get("efficiency", -1.0)),
			"equipment": float(record.get("equipment", -1.0)),
			"utilization": float(record.get("utilization", 0.0)),
			"last_utilization": float(record.get("last_utilization", -1.0)),
			"revenue": float(record.get("revenue", 0.0)),
			"costs": float(record.get("costs", 0.0)),
			"profit": float(record.get("profit", 0.0)),
			"loss_days": int(record.get("loss_days", 0)),
			"price": float(record.get("market_price", 0.0)),
			"sold": float(record.get("market_sold", 0.0)),
			"stock": float(record.get("market_stock", 0.0)),
			"supply": float(record.get("market_supply", 0.0)),
			"demand": float(record.get("market_demand", 0.0)),
			"shortage": float(record.get("market_shortage", 0.0)),
			"surplus": float(record.get("market_surplus", 0.0)),
		})
	_cotton_last_day = day_now
	_cotton_counts.append(counts)
	_cotton_prices.append(_cotton_market_row(day_now))


## Soma o mercado de algodão de uma cidade (dentro de `_buildings_snapshot`).
func _cotton_market_add(market: Dictionary) -> void:
	var price := float(market.get("price", 0.0))
	if price > 0.0:
		_cotton_day_prices.append(price)
	for field: String in ["sold", "supply", "demand", "stock", "shortage",
			"surplus"]:
		_cotton_day_sums[field] = float(_cotton_day_sums.get(field, 0.0)) \
			+ float(market.get(field, 0.0))


## Oferta/demanda/preço do algodão no mundo no dia (mercados das cidades).
func _cotton_market_row(day_now: int) -> Dictionary:
	if _cotton_index < 0:
		return {"day": day_now}
	var prices: Array = _cotton_day_prices.duplicate()
	prices.sort()
	var median := 0.0
	if not prices.is_empty():
		median = float(prices[prices.size() / 2])
	var mean := 0.0
	if not prices.is_empty():
		for price: Variant in prices:
			mean += float(price)
		mean /= float(prices.size())
	return {"day": day_now, "cities": prices.size(), "mean": mean,
		"median": median, "min": float(prices[0]) if not prices.is_empty() else 0.0,
		"max": float(prices[-1]) if not prices.is_empty() else 0.0,
		"sold": float(_cotton_day_sums.get("sold", 0.0)),
		"supply": float(_cotton_day_sums.get("supply", 0.0)),
		"demand": float(_cotton_day_sums.get("demand", 0.0)),
		"stock": float(_cotton_day_sums.get("stock", 0.0)),
		"shortage": float(_cotton_day_sums.get("shortage", 0.0)),
		"surplus": float(_cotton_day_sums.get("surplus", 0.0))}


## Fecha o bloco do diagnóstico: planos de safra de algodão (definições do
## núcleo) para dizer se a cidade da fazenda tem plano sazonal fixo.
func _cotton_report(sim: Object) -> Dictionary:
	var plan_cities: Array = []
	var plan_rows: Array = []
	var snapshot: Dictionary = {}
	if sim.has_method("to_snapshot"):
		snapshot = sim.to_snapshot()
	var definitions: Dictionary = snapshot.get("definitions", {})
	for plan: Variant in (definitions.get("crop_plans", []) as Array):
		if typeof(plan) != TYPE_DICTIONARY:
			continue
		var row: Dictionary = plan
		if String(row.get("product", "")) != "cotton":
			continue
		var city := String(row.get("city", ""))
		if not plan_cities.has(city):
			plan_cities.append(city)
		plan_rows.append({"city": city,
			"area_ha": float(row.get("area_ha", 0.0)),
			"yield_t_ha": float(row.get("yield_t_ha", 0.0))})
	for building_id: Variant in _cotton_farms:
		var entry: Dictionary = _cotton_farms[building_id]
		entry["has_crop_plan"] = plan_cities.has(String(entry.get("city", "")))
	return {"farms": _cotton_farms, "prices": _cotton_prices,
		"counts": _cotton_counts, "cotton_plan_cities": plan_cities,
		"cotton_plans": plan_rows}


## Resumo impresso do diagnóstico do algodão: as fazendas fechadas, o último
## retrato antes da falência e a trajetória do preço.
func _cotton_summary(diag: Dictionary, bankruptcies: Array) -> String:
	var farms: Dictionary = diag.get("farms", {})
	var counts: Array = diag.get("counts", [])
	var lines := PackedStringArray()
	var last_count: Dictionary = counts[counts.size() - 1] if not counts.is_empty() else {}
	lines.append("[c1] D-125 algodão: fazendas=%d planos=%d fechadas=%d %s" % [
		farms.size(), (diag.get("cotton_plan_cities", []) as Array).size(),
		bankruptcies.size(),
		"mundo=%d" % int(last_count.get("cotton_world", 0))])
	for record: Dictionary in bankruptcies:
		var id := String(record.get("building_id", ""))
		if not id.begins_with("AI-COTTON"):
			continue
		var entry: Dictionary = farms.get(id, {})
		var rows: Array = entry.get("rows", [])
		var last: Dictionary = rows[rows.size() - 1] if not rows.is_empty() else {}
		lines.append(("  %s %s/%s nasceu>%d fechou=%d plano=%s "
			+ "eff=%.3f workers=%.1f/%.1f util=%.3f preço=%.2f "
			+ "rev=%.2f custo=%.2f loss=%d") % [
			id, String(entry.get("nation", "")),
			String(entry.get("city", "")),
			int(entry.get("born_after_day", 0)), int(record.get("day", 0)),
			"sim" if bool(entry.get("has_crop_plan", false)) else "nao",
			float(last.get("efficiency", -1.0)),
			float(last.get("workers", 0.0)), float(last.get("jobs", 0.0)),
			float(last.get("utilization", 0.0)), float(last.get("price", 0.0)),
			float(last.get("revenue", 0.0)), float(last.get("costs", 0.0)),
			int(last.get("loss_days", 0))])
	var price_lines := PackedStringArray()
	for row: Variant in (diag.get("prices", []) as Array):
		var price_row: Dictionary = row
		var day := int(price_row.get("day", 0))
		if day in [5, 90, 180, 270, 365]:
			price_lines.append("d%d mediana=%.3f min=%.2f max=%.2f sold=%.1f stock=%.0f" % [
				day, float(price_row.get("median", 0.0)),
				float(price_row.get("min", 0.0)), float(price_row.get("max", 0.0)),
				float(price_row.get("sold", 0.0)), float(price_row.get("stock", 0.0))])
	lines.append("  preço: " + " | ".join(price_lines))
	return "\n".join(lines)


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
				+ served * float(grid_row.get("tariff",
					grid_row.get("price", 0.0)))
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
		price_base: Dictionary, final_ratios: Array) -> Array:
	var final_lookup: Dictionary = {}
	for ratio_row: Dictionary in final_ratios:
		final_lookup[String(ratio_row.get("nation", ""))] = \
			float(ratio_row.get("food_avg30", -1.0))
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
			# Média de 30 d no fecho (o número do portão; `food_avg30` acima é
			# o valor do dia em que a nação cruzou pela primeira vez).
			"food_final30": float(final_lookup.get(tag_text, -1.0)),
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


# ---------------------------------------------------------------------------
# G1 — diag de bens (preço MA30 e oferta/produção por nação). Só medição.
# ---------------------------------------------------------------------------

## Amostra diária barata: `status.light` traz preço/vendido/oferta por cidade
## sem chamar `get_city_status`. `m_supply` é a oferta ao mercado do dia e serve
## de proxy de produção por nação (mesma leitura do `price_trajectory_365`).
func _goods_diag_sample(day: int, status: Dictionary) -> void:
	var light: Dictionary = status.get("light", {})
	var per_city := int(light.get("product_count", 0))
	var products: Array = status.get("products", [])
	var prices: PackedFloat64Array = light.get("m_price", PackedFloat64Array())
	var sold: PackedFloat64Array = light.get("m_sold", PackedFloat64Array())
	var supply: PackedFloat64Array = light.get("m_supply", PackedFloat64Array())
	var cities: Array = status.get("cities", [])
	if per_city <= 0 or products.is_empty():
		return
	for good_id: String in _goods_diag_ids:
		var index := products.find(good_id)
		if index < 0:
			continue
		var weighted := 0.0
		var weight := 0.0
		var plain := 0.0
		var priced := 0
		var by_nation: Dictionary = {}
		var sold_by_nation: Dictionary = {}
		for slot in cities.size():
			var at: int = slot * per_city + index
			if at >= prices.size():
				break
			var price := float(prices[at])
			var quantity := float(sold[at]) if at < sold.size() else 0.0
			if price > 0.0:
				priced += 1
				plain += price
				weighted += price * quantity
				weight += quantity
			var produced := float(supply[at]) if at < supply.size() else 0.0
			if produced > 0.0 or quantity > 0.0:
				var nation := String((cities[slot] as Dictionary).get("nation", ""))
				if not nation.is_empty():
					if produced > 0.0:
						by_nation[nation] = float(by_nation.get(nation, 0.0)) + produced
					if quantity > 0.0:
						sold_by_nation[nation] = \
							float(sold_by_nation.get(nation, 0.0)) + quantity
		var mean := 0.0
		if weight > 0.0:
			mean = weighted / weight
		elif priced > 0:
			mean = plain / float(priced)
		var series: Array = _goods_diag_prices.get(good_id, [])
		series.append(mean)
		_goods_diag_prices[good_id] = series
		var supply_series: Array = _goods_diag_supply.get(good_id, [])
		supply_series.append(by_nation)
		_goods_diag_supply[good_id] = supply_series
		var sold_series: Array = _goods_diag_sold.get(good_id, [])
		sold_series.append(sold_by_nation)
		_goods_diag_sold[good_id] = sold_series


func _mean_price(series: Array) -> float:
	var total := 0.0
	var count := 0
	for value: Variant in series:
		var price := float(value)
		if price > 0.0:
			total += price
			count += 1
	return total / count if count > 0 else 0.0


func _avg30_by_nation(series: Array) -> Dictionary:
	var window: Array = series.slice(
		maxi(0, series.size() - WINDOW), series.size())
	var totals: Dictionary = {}
	for day_row: Dictionary in window:
		for nation in day_row:
			totals[nation] = float(totals.get(nation, 0.0)) \
				+ float(day_row[nation])
	var days := maxi(1, window.size())
	var by_nation: Dictionary = {}
	for nation in totals:
		by_nation[nation] = float(totals[nation]) / float(days)
	return {"by_nation": by_nation, "days": days}


func _goods_diag_report() -> Dictionary:
	var out: Dictionary = {}
	for good_id: String in _goods_diag_ids:
		var series: Array = _goods_diag_prices.get(good_id, [])
		var first := series.slice(0, mini(WINDOW, series.size()))
		var last := series.slice(maxi(0, series.size() - WINDOW), series.size())
		var supply := _avg30_by_nation(_goods_diag_supply.get(good_id, []))
		var sold := _avg30_by_nation(_goods_diag_sold.get(good_id, []))
		var nations: Dictionary = {}
		for nation in supply["by_nation"]:
			if float(supply["by_nation"][nation]) > 1e-9:
				nations[nation] = true
		for nation in sold["by_nation"]:
			if float(sold["by_nation"][nation]) > 1e-9:
				nations[nation] = true
		var base := float(_goods_price_base.get(good_id, 0.0))
		var ma30_last := _mean_price(last)
		out[good_id] = {
			"base_price": base,
			"ma30_first": _mean_price(first),
			"ma30_last": ma30_last,
			"ratio_last_base": ma30_last / base if base > 0.0 else 0.0,
			"producing_nations_30d": nations.size(),
			"supply_avg30_by_nation": supply["by_nation"],
			"sold_avg30_by_nation": sold["by_nation"],
		}
	return out


func _print_goods_diag(report: Dictionary) -> void:
	for good_id in report:
		var row: Dictionary = report[good_id]
		var supply_total := 0.0
		for value: Variant in (row["supply_avg30_by_nation"] as Dictionary).values():
			supply_total += float(value)
		var sold_total := 0.0
		for value: Variant in (row["sold_avg30_by_nation"] as Dictionary).values():
			sold_total += float(value)
		print("[c1] G1 %s: base=%.2f MA30_dia0=%.2f MA30_final=%.2f razão=%.4f supply30d=%.4f sold30d=%.4f t/dia em %d países"
			% [String(good_id), float(row["base_price"]),
			float(row["ma30_first"]), float(row["ma30_last"]),
			float(row["ratio_last_base"]), supply_total, sold_total,
			int(row["producing_nations_30d"])])


