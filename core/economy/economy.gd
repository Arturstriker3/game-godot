extends Node

## Autoload `Economy`: acopla o núcleo C++ (EconomySim) ao jogo.
##
## Avança um dia por tick e publica o resumo para o HUD e para os comboios.
## A simulação é por cidade: cada nação possui uma ou mais cidades, e cada
## cidade tem população, produção e demanda próprias.
##
## O relógio é um acumulador da FRAÇÃO do dia (0..1), e não um `Timer`: trocar
## de velocidade (ou pausar) preserva o progresso do dia em vez de reiniciá-lo.
##
## O dia roda numa thread do WorkerThreadPool (core/economy/tick_scheduler.gd):
## a main thread só *consulta* até o tick terminar, e o status completo é
## publicado de uma vez. Com 650 cidades o dia custa ~60 ms de trabalho, mas o
## frame não paga nada disso. Regra de segurança: existe UM escritor sobre o
## `sim` por vez — enquanto a thread roda, ninguém de fora mexe no núcleo.
##
## A IA econômica (privado, governo e grupos) roda DENTRO dessa thread, como
## fase do `tick_day` do núcleo: aqui só se liga a flag (`set_ai_enabled`) e se
## nomeiam os grupos (cultura de origem).
##
## TURBO (ferramenta de teste/agente, NÃO feature do jogador): ligado só por
## `--turbo...` na linha de comando ou `OGS_TURBO...` no ambiente
## (core/economy/turbo_config.gd). Roda um lote de dias por tarefa e ignora o
## relógio, para acelerar testes e medições. Sem a flag, o caminho é o de
## sempre. Nada de UI/atalho/i18n publica este modo.

## Emitido a cada tick com os pedidos de comércio do dia.
signal trade_orders_ready(orders: Array)

## Emitido a cada tick com o resumo leve do dia (totais/contadores/violações).
## A4: cidades, nações e pedidos não vêm mais aqui — são consultados sob demanda.
signal status_updated(summary: Dictionary)

## Emitido quando a velocidade muda (botão ou atalho espaço). Índice 0 = pausa.
signal speed_changed(index: int)

## Segundos reais por dia simulado; o índice 0 é a pausa.
## Ritmo normal: 30 dias em 30 minutos (1 dia = 60 s); 2x = 30 s; 3x = 15 s.
const SPEEDS := [0.0, 60.0, 30.0, 15.0]
const SPEED_COUNT := 4

## Nomes dos grupos (cultura de origem) — a única parte da IA que segue em
## GDScript; a decisão roda no núcleo (A5).
const GroupNamerScript := preload("res://core/economy/group_namer.gd")

## Agenda o tick do dia na thread do pool (ver tick_scheduler.gd).
const TickScheduler := preload("res://core/economy/tick_scheduler.gd")

## Configuração do modo TURBO — FERRAMENTA DE TESTE/AGENTE, não feature do
## jogador: só a linha de comando ou o ambiente ativam (ver turbo_config.gd).
## Sem a flag o jogo se comporta exatamente como antes.
const TurboConfigScript := preload("res://core/economy/turbo_config.gd")

## B3: dado de região (estados inteiros) — anexa o bloco `region` a cada dict
## de cidade antes de initialize_world (state_regions.json + urban_share).
const RegionDataScript := preload("res://core/economy/region_data.gd")
## B6: hectares de terra agrícola e Gini fundiário por região (dado + regra de
## rateio declarada em land_property.json).
const LandPropertyScript := preload("res://core/economy/land_property.gd")
## G (D-096): agricultura sazonal — declarada depois da geografia.
const AgricultureServiceScript := preload("res://core/economy/agriculture_service.gd")
## G2 (D-101): armazenagem — capacidade real do país, mapa produto→tipo e a
## reserva pública do dia 1, declarados depois da agricultura e antes da IA.
const StorageServiceScript := preload("res://core/economy/storage_service.gd")
## PARTE E (D-116): energia — acesso/perdas, parque, potencial e o PVOUT mensal
## do dado público; declarados antes do primeiro dia e reaplicados no load.
const PowerServiceScript := preload("res://core/economy/power_service.gd")
## F0/D-124: refino — capacidade real do EI 2024 por país; semeia as refinarias
## do dia 1 (depois de `seed_industry`, para não bloquear farinha/pão).
const RefiningServiceScript := preload("res://core/economy/refining_service.gd")
## 1º de janeiro de 2027, 00:00 UTC (D-084). 1798761600 = 2027-01-01T00:00:00Z.
## A base de dados (WPP/WorldPop 2026, extração 2025) NÃO é reprojetada: o
## calendário do jogo começa em 2027 e a projeção da base é dívida de calibração.
const START_UNIX := 1798761600

## Cidades do cenário (geradas de data/definitions/scenario/cities.csv).
const CITIES_PATH := "res://data/definitions/scenario/cities.json"
## Portos do cenário (gerados de data/definitions/scenario/ports.csv).
const PORTS_PATH := "res://data/definitions/scenario/ports.json"
## Construções do cenário (geradas de data/definitions/scenario/buildings.csv).
const BUILDINGS_PATH := "res://data/definitions/scenario/buildings.json"
## Bens (id, nome, preço-base) — data-driven, editáveis sem recompilar o C++.
const GOODS_PATH := "res://data/definitions/economy/goods.json"
## Receitas industriais (1..6 insumos, alternativas e subprodutos).
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
## D-125: eficiência de produção (curva por classe, transições, equipamento,
## modos da refinaria e custo de troca da IA). Defaults de balanceamento
## declarados; o espelho compilado em `sim/efficiency.h` é o fallback.
const EFFICIENCY_PATH := "res://data/definitions/production/efficiency.json"
## D-126: sobrevivência em crise (rescisão, crédito de giro, liquidação,
## hibernação, leilão e área). Defaults de balanceamento declarados.
const CRISIS_PATH := "res://data/definitions/economy/crisis.json"
## D-131 (fase A): cérebro econômico da IA (previsão de caixa mensal e preço
## esperado). Defaults de balanceamento declarados; `enabled=false` por padrão
## mantém as decisões da IA como antes.
const AI_BRAIN_PATH := "res://data/definitions/economy/ai_brain.json"
## Necessidades das POPs (cesta básica data-driven; níveis completos em F3).
const NEEDS_PATH := "res://data/definitions/economy/needs.json"
## Serviços (F8): 20 setores, compradores, insumos e mistura de escolaridade.
const SERVICES_PATH := "res://data/definitions/economy/services.json"
## Moedas e taxas de câmbio por nação (F5; âncora XGS, regime peg/float).
const CURRENCIES_PATH := "res://data/definitions/economy/currencies.json"
# C4 (D-110): bancos com dono (nomes fictícios/genéricos do dado).
const BANKS_PATH := "res://data/definitions/economy/banks.json"
## C2c (D-108): regime REAL por moeda (IMF AREAER 30/04/2026), âncora (moeda
## ou cesta), emissor e trava de união monetária. Ausente = o dado do F5 manda.
const FX_REGIMES_PATH := "res://data/definitions/economy/fx_regimes_2026.json"
## F9: estrutura de emprego por setor (âncoras ILO + coeficientes agrícolas).
const SECTOR_EMPLOYMENT_PATH := "res://data/definitions/economy/sector_employment.json"
## B1: estrutura etária por nação (definição estática; coortes ainda não).
const AGE_STRUCTURE_PATH := "res://data/definitions/population/age_structure_2026.json"
## B7: taxa de emprego informal por nação (ILOSTAT/ILO) e cobertura de saúde
## de referência (WHO UHC SCI). O loader tolera arquivo ausente (cai na âncora
## de renda / kHealthAccessDefault quando a nação não tem entrada).
const INFORMALITY_PATH := "res://data/definitions/economy/informality.json"
const HEALTH_ACCESS_PATH := "res://data/definitions/population/health_access.json"
## §19 (W0): solo urbano, moradia e prefeitura por cidade.
const URBAN_LAND_PATH := "res://data/definitions/economy/urban_land.json"
const HOUSING_CITY_PATH := "res://data/definitions/economy/housing_city.json"
const HOUSING_PATH := "res://data/definitions/economy/housing.json"
const MUNICIPAL_FINANCE_PATH := "res://data/definitions/economy/municipal_finance.json"
## Nações do cenário (geradas de data/definitions/scenario/nations.csv).
const NATIONS_PATH := "res://data/definitions/scenario/nations.json"
## Bandeiras por código ISO-2 (moddável: adicione assets/flags/<código>.png).
const FLAGS_DIR := "res://assets/flags/"

var sim: RefCounted
var last_summary: Dictionary = {}
var speed: int = 1
var _nations: Dictionary = {}  # tag -> {id, name, flag}

## G (decisão do dono): calendário de safra gravado na REGIÃO no início do mundo
## — `{ city_id: nação original }`, do `cities.json` (antes de qualquer
## conquista). A produção usa este calendário para sempre: trocar o dono não
## muda a época da safra. O plano resultante (janelas em dia do ano) viaja no
## save (`definitions.crop_plans`).
var calendar_nations: Dictionary = {}

## A4: retrato das telas de dados, montado sob demanda uma vez por dia
## (Economy.ui_snapshot()). Vazio enquanto nenhuma tela precisou dele.
var _ui_snapshot: Dictionary = {}
var _ui_snapshot_day: int = -1
var _ui_snapshot_version: int = -1

## Velocidade para a qual voltar ao sair da pausa (espaço).
var _resume_speed: int = 1

## Fração do dia corrente (0..1). Não depende da velocidade.
var _day_fraction := 0.0

## Agenda o dia na thread do pool; `is_running()` = dia em cálculo.
var _ticker: RefCounted = TickScheduler.new()

## Modo TURBO (ferramenta de teste) e bench: os args de usuário e o ambiente
## são lidos aqui, no `_init` da config. Jogo normal: `enabled = false`,
## `bench_days = 0`.
var _turbo = TurboConfigScript.new(OS.get_cmdline_user_args())

## Último dia publicado no turbo. -1 = nada publicado ainda (sempre publica).
var _turbo_publish_day := -1

## Bench: primeiro dia publicado e o relógio de parede no mesmo instante.
var _bench_first_day := -1
var _bench_t0_usec := 0

## Já juntamos a thread do dia? Ver `_shutdown()`.
var _shutting_down := false


func _ready() -> void:
	# TURBO (ferramenta de teste): lote de dias por tarefa e uma linha no log.
	# Sem a flag o lote é 1 — o tick de um dia de sempre (days_per_frame só vale
	# com o turbo ligado).
	_ticker.days_per_task = _turbo.days_per_frame if _turbo.enabled else 1
	if _turbo.enabled:
		print("[turbo] ON " + _turbo.describe())
	sim = EconomySim.new()
	# Dados das cadeias ANTES de inicializar o mundo: o núcleo resolve os ids
	# dos produtos/insumos e as receitas no momento em que cria as construções.
	sim.set_goods(_load_goods())
	sim.set_recipes(_load_json_array(RECIPES_PATH))
	# D-125: parâmetros da eficiência de produção (defaults declarados). Antes
	# do mundo: a planta nova e o seed maduro leem a curva já aplicada.
	if sim.has_method("set_efficiency_params"):
		var efficiency := _load_json_dict_optional(EFFICIENCY_PATH)
		if not efficiency.is_empty():
			sim.set_efficiency_params(efficiency)
	# D-126: parâmetros da sobrevivência em crise (defaults declarados). Antes
	# do mundo, para o mesmo balanceamento valer desde o dia 0.
	if sim.has_method("set_crisis_params"):
		var crisis := _load_json_dict_optional(CRISIS_PATH)
		if not crisis.is_empty():
			sim.set_crisis_params(crisis)
	# D-131: parâmetros do cérebro econômico da IA (fase A). Antes do mundo,
	# como crise/eficiência; `enabled=false` no JSON mantém o comportamento.
	if sim.has_method("set_ai_brain_params"):
		var brain := _load_json_dict_optional(AI_BRAIN_PATH)
		if not brain.is_empty():
			sim.set_ai_brain_params(brain)
	sim.set_needs(_load_json_dict(NEEDS_PATH))
	# F8: serviços (catálogo). Antes do mundo: os negócios iniciais e o preço-âncora
	# dos 20 setores dependem dele.
	sim.set_services(_load_json_dict(SERVICES_PATH))
	# C2c (D-108): regimes reais ANTES das moedas — o mundo nasce com o regime
	# do IMF AREAER (e não com o "peg administrativo" único da C2a).
	if sim.has_method("set_fx_regimes"):
		sim.set_fx_regimes(_load_json_dict_optional(FX_REGIMES_PATH))
	# F5: moedas ANTES do mundo, para o preço local de cada cidade nascer na
	# moeda da nação (goods.json está em âncora XGS).
	sim.set_currencies(_load_json_dict(CURRENCIES_PATH))
	# F9: emprego setorial (âncoras ILO por nível de renda + coeficientes da
	# agricultura). Binário anterior à F9 não tem o método: mantém o espelho
	# compilado das mesmas âncoras.
	if sim.has_method("set_sector_employment"):
		sim.set_sector_employment(_load_json_dict(SECTOR_EMPLOYMENT_PATH))
	# B1: estrutura etária por nação (definição estática). Antes do mundo, como
	# as demais definições; binário anterior ao B1 não tem o método.
	if sim.has_method("set_age_structure"):
		sim.set_age_structure(_load_json_dict(AGE_STRUCTURE_PATH))
	# B7: informalidade e cobertura de saúde por nação (definições estáticas).
	# Antes do mundo; ausência de arquivo vira {} e o núcleo cai na âncora.
	if sim.has_method("set_informality"):
		sim.set_informality(_load_json_dict_optional(INFORMALITY_PATH))
	if sim.has_method("set_health_access"):
		sim.set_health_access(_load_json_dict_optional(HEALTH_ACCESS_PATH))
	# Resolve a fazenda inicial de cada cidade para o alimento básico de maior
	# aptidão × caloria da nação (antes de criar os prédios no núcleo). Sem dados,
	# mantém trigo.
	var geo := GeographyService.new()
	var cities := _load_cities()
	# B3: anexa o bloco `region` (estados inteiros) a cada cidade antes de criar
	# o mundo — o núcleo e o snapshot/CLI levam o dado junto.
	RegionDataScript.attach(cities)
	var farms := geo.resolve_initial_mines(
		geo.resolve_initial_farms(_load_buildings(), cities), cities)
	sim.initialize_world(cities, _load_ports(), farms)
	# B5 (D-089): infraestrutura INICIAL do estado a partir do dado real (World
	# Bank). DEPOIS do mundo (as regiões existem) e antes do primeiro dia; o
	# dado é o mesmo do snapshot/CLI e dos harnesses de equivalência.
	GeographyService.apply_initial_infra(sim)
	# §19 (W0): solo, moradia e prefeitura por cidade. DEPOIS do mundo: os JSON
	# casam pelo id da cidade. Binário anterior à §19 não tem o método (mantém o
	# estado sem dado urbano, como antes).
	if sim.has_method("set_urban"):
		sim.set_urban(
			_load_json_dict(URBAN_LAND_PATH),
			_load_json_dict(HOUSING_CITY_PATH),
			_load_json_dict(HOUSING_PATH),
			_load_json_dict(MUNICIPAL_FINANCE_PATH))
	# B6 (D-095): PATRIMÔNIO DA REGIÃO — hectares agrícolas e Gini fundiário do
	# dado real (`land_property.json`) com a regra de rateio nacional→estado
	# declarada no próprio arquivo. DEPOIS do mundo e do `set_urban` (o preço da
	# moradia do interior usa o preço popular do polo). O núcleo só semeia a
	# região que ainda não tem patrimônio (save v23 manda; v22 recebe o dado).
	LandPropertyScript.apply(sim)
	_load_nations()
	# C4 (D-110): bancos com dono — nomes fictícios/genéricos de `banks.json` e
	# presença do banco estrangeiro. DEPOIS do mundo (o banco carrega a moeda da
	# nação e o Banco Central da C2a). Sem o arquivo, nada é inventado: os
	# bancos ficam sem nome (`no_data`) e só público/privado existem.
	if sim.has_method("set_bank_definitions"):
		sim.set_bank_definitions(_load_json_dict_optional(BANKS_PATH))
	# C6 (D-114): os parâmetros da crise (bank_crisis.json) têm espelho
	# compilado em `sim/bank_crisis.h`, que é o default do jogo e do CLI — o
	# GDScript NÃO sobrescreve, para jogo e linha de comando verem o mesmo
	# estado (mesma convenção do bank_credit.json da C3/D-111). O teste C6
	# confere que o JSON e o espelho são iguais.
	# Aplica a geografia real (aptidão de culturas + reservas minerais) desde o
	# início — sem prospecção: todo depósito é conhecido.
	geo.apply_to_sim(sim, _cities_with_center())
	# G (D-096): agricultura sazonal DEPOIS da geografia (a área por cidade vem
	# da capacidade de terra que a geografia declara). Declara plano de safra,
	# estoque inicial, perdas, prateleira e fertilizante; sem os arquivos o
	# núcleo mantém a produção diária das fazendas (ausência explícita).
	if sim.has_method("set_crop_plan"):
		var agri := AgricultureServiceScript.new()
		# O calendário é herdado da nação ORIGINAL da região e fica guardado no
		# autoload: a conquista troca o dono, não a época da safra.
		calendar_nations = agri.calendar_nations(cities)
		var agri_report := agri.apply_to_sim(sim, cities, geo,
			{"calendar_nations": calendar_nations,
			"farms": AgricultureServiceScript.farms_of(farms)})
		print("[agricultura] planos=%d cidades=%d estoque=%.0f t base=%s" % [
			int(agri_report.get("plans", 0)),
			int(agri_report.get("cities", 0)),
			float(agri_report.get("initial_stock_t", 0.0)),
			str(agri_report.get("area_basis", {}))])
	# G2 (D-101): ARMAZENAGEM — o mapa produto→tipo do bem, a capacidade
	# inicial (COBERTURA real do dado × produção do jogo; regra declarada de
	# meses quando o dado não tem cobertura) e a reserva pública do dia 1
	# (dias de importação/consumo × fluxo do jogo). DEPOIS da agricultura (a
	# produção que rateia a capacidade é a mesma do plano de safra) e antes da
	# IA. `only_missing`: save v25 manda — nada de reescrever a
	# capacidade/estoque que veio no save (nem o da IA).
	if sim.has_method("set_storage_capacity"):
		var storage := StorageServiceScript.new()
		var storage_report := storage.apply_to_sim(sim, cities, farms,
			{"only_missing": true})
		var capacity_total := 0.0
		var capacity_data := 0.0
		var capacity_rule := 0.0
		for type in storage_report.get("capacity_t", {}):
			capacity_total += float(storage_report["capacity_t"][type])
			var basis: Dictionary = storage_report["capacity_source"].get(type, {})
			capacity_data += float(basis.get("data", 0.0))
			capacity_rule += float(basis.get("rule", 0.0))
		print("[armazenagem] tipos=%d/%d sites=%d capacidade=%.0f t (dado %.0f + regra %.0f) reserva=%.0f t ouro=%.0f t" % [
			int(storage_report.get("product_types_applied", 0)),
			int(storage_report.get("product_types_total", 0)),
			int(storage_report.get("sites_written", 0)),
			capacity_total, capacity_data, capacity_rule,
			float(storage_report.get("initial_stock_t", {}).get("oil", 0.0))
				+ float(storage_report.get("initial_stock_t", {}).get("grain", 0.0)),
			float(storage_report.get("gold_reserve_t", 0.0))])
	# PARTE E (D-116): ENERGIA — acesso/perdas (World Bank), parque instalado
	# (GPPD escalonado à Ember), potencial hídrico e solar/eólico e o PVOUT
	# mensal. DEPOIS do mundo (as cidades existem) e antes da IA. `only_missing`:
	# save v35 manda; save v34 e anterior recebe o parque real aqui.
	if sim.has_method("set_power_definitions"):
		var power := PowerServiceScript.new()
		var power_report := power.apply_to_sim(sim, cities, {"only_missing": true})
		print("[energia] cidades=%d parque=%.0f MW nações=%d operador=%d solar_mensal=%d" % [
			int(power_report.get("cities_with_park", 0)),
			float(power_report.get("park_mw", 0.0)),
			int(power_report.get("nations", 0)),
			(power_report.get("operator_nations", []) as Array).size(),
			int(power_report.get("cities_with_solar_month", 0))])
	# F0/D-124: REFINO — capacidade de destilação 2024 do EI por nação, na
	# fração simulada da economia. DEPOIS da energia (a cidade já existe com
	# parque) e ANTES da IA. O serviço chama `seed_industry` primeiro: sem essa
	# ordem a refinaria faria o bootstrap achar que o mundo já tem indústria e
	# pular a farinha/pão.
	if sim.has_method("build") and sim.has_method("get_products"):
		var refining := RefiningServiceScript.new()
		var refining_report := refining.apply_to_sim(sim, cities, _load_ports())
		print("[refino] nações=%d plantas=%d capacidade=%.0f t/dia maduras=%d (%s)" % [
			(refining_report.get("nations", []) as Array).size(),
			int(refining_report.get("applied", 0)),
			float(refining_report.get("capacity_t_per_day", 0.0)),
			int(refining_report.get("matured", 0)),
			String(refining_report.get("reason", "ok"))])
	# A IA econômica (privado, governo, grupos) roda no núcleo, na virada do dia
	# (A5). Ligada aqui: o primeiro `tick_day` já decide.
	sim.set_ai_enabled(true)
	# F9: nomes dos grupos pela cultura de origem; a maior cidade da nacao e o
	# {place} dos padroes. Sem cultura mapeada, o fallback (setor + nacao)
	# mantem o nome unico.
	GroupNamerScript.name_groups(sim, _capital_names())
	# A4: nada de pré-aquecer o status — o tick não monta mais o mundo inteiro.
	# A primeira consulta pesada (tela de dados aberta) paga a própria alocação.

	# Bench (ferramenta de teste): velocidade forçada, antes do primeiro dia.
	# `bench_speed = 0` pausa o relógio normal — é escolha de quem roda o bench.
	if _turbo.bench_speed >= 0:
		speed = _turbo.bench_speed

	# Adia o primeiro dia: a cena principal conecta os sinais no _ready. O dia
	# roda direto na thread do pool; o primeiro status sai no _process.
	_start_day.call_deferred()


## G: reaplica no núcleo o que FALTAR depois de carregar um save. O plano já
## veio em `definitions` (janelas herdadas no início do mundo), então nada é
## redeclarado por cima; um par que falte, se houver, nasce com o calendário
## ORIGINAL da região — nunca com o dono atual. Sem semente de estoque: o
## estoque do dia 1 está no save.
func refresh_agriculture() -> Dictionary:
	if sim == null or not sim.has_method("set_crop_plan"):
		return {}
	var agri := AgricultureServiceScript.new()
	if calendar_nations.is_empty():
		calendar_nations = agri.calendar_nations(_load_cities())
	var geo := GeographyService.new()
	geo.load_data()
	var cities := _load_cities()
	var farms := geo.resolve_initial_mines(
		geo.resolve_initial_farms(_load_buildings(), cities), cities)
	return agri.apply_to_sim(sim, cities, geo, {
		"calendar_nations": calendar_nations,
		"only_missing": true,
		"initial_stock": false,
		"farms": AgricultureServiceScript.farms_of(farms),
	})


## G2: reaplica no núcleo o que FALTAR depois de carregar um save. Save v25
## traz o bloco `storage` (capacidade e estoque, inclusive o que a IA
## construiu) e `only_missing` não toca em site existente; save v24 e anterior
## não tem o bloco e recebe a capacidade inicial (dado ou regra) aqui. Sem
## semente de estoque: a reserva do dia 1 só existe em mundo novo.
func refresh_storage() -> Dictionary:
	if sim == null or not sim.has_method("set_storage_capacity"):
		return {}
	var geo := GeographyService.new()
	geo.load_data()
	var cities := _load_cities()
	var buildings := geo.resolve_initial_mines(
		geo.resolve_initial_farms(_load_buildings(), cities), cities)
	var storage := StorageServiceScript.new()
	return storage.apply_to_sim(sim, cities, buildings, {
		"only_missing": true,
		"initial_stock": false,
	})


## PARTE E (D-116): reaplica no núcleo o que FALTAR depois de carregar um save.
## Save v35 traz o bloco `power` (estado, regra e parque — inclusive o que a IA
## construiu) e `only_missing` não toca no que veio no save; save v34 e anterior
## não tem o bloco e recebe o parque real do dado aqui. Sem custo extra: só
## declara, não cria usina nem muda conexão de cidade.
func refresh_power() -> Dictionary:
	if sim == null or not sim.has_method("set_power_definitions"):
		return {}
	var power := PowerServiceScript.new()
	return power.apply_to_sim(sim, _load_cities(), {"only_missing": true})


## F0/D-124: reaplica no núcleo o refino que FALTAR depois de carregar um save.
## Save v39 já traz as construções `refinery` no estado e o serviço para em
## `world_already_has_refining` (save manda); save v38 e anterior não tem o
## bloco e recebe a capacidade do dado aqui, como no dia 0.
func refresh_refining() -> Dictionary:
	if sim == null or not sim.has_method("build"):
		return {}
	var refining := RefiningServiceScript.new()
	return refining.apply_to_sim(sim, _load_cities(), _load_ports())


## F9: nome da maior cidade de cada nacao (usado como {place} no nome dos
## grupos). Le as cidades do cenario; nao inventa nome onde nao ha dado.
func _capital_names() -> Dictionary:
	var tallest: Dictionary = {}
	for city in _load_cities():
		var tag := String(city.get("nation", ""))
		var population := float(city.get("population", 0.0))
		if tag.is_empty():
			continue
		if not tallest.has(tag) or population > float(tallest[tag]["population"]):
			tallest[tag] = {"population": population, "name": String(city.get("name", ""))}
	var names: Dictionary = {}
	for tag in tallest:
		names[tag] = String(tallest[tag]["name"])
	return names


## Lista de cidades no formato que o GeographyService.apply_to_sim espera.
func _cities_with_center() -> Array:
	var result: Array = []
	for city in _load_cities():
		result.append({
			"id": String(city.get("id", "")),
			"nation": String(city.get("nation", "")),
			"center": Vector2(float(city.get("x", 0.0)), float(city.get("y", 0.0))),
		})
	return result


## Lê as cidades do cenário (JSON) e devolve o array para o núcleo C++.
func _load_cities() -> Array:
	var file := FileAccess.open(CITIES_PATH, FileAccess.READ)
	if file == null:
		push_error("Economy: não abriu %s" % CITIES_PATH)
		return []
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	if typeof(data) != TYPE_ARRAY:
		push_error("Economy: %s não é um array JSON" % CITIES_PATH)
		return []
	return data


## Lê os portos do cenário (JSON) e devolve o array para o núcleo C++.
func _load_ports() -> Array:
	var file := FileAccess.open(PORTS_PATH, FileAccess.READ)
	if file == null:
		push_error("Economy: não abriu %s" % PORTS_PATH)
		return []
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	if typeof(data) != TYPE_ARRAY:
		push_error("Economy: %s não é um array JSON" % PORTS_PATH)
		return []
	return data


## Lê as construções do cenário (JSON) e devolve o array para o núcleo C++.
func _load_buildings() -> Array:
	var file := FileAccess.open(BUILDINGS_PATH, FileAccess.READ)
	if file == null:
		push_error("Economy: não abriu %s" % BUILDINGS_PATH)
		return []
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	if typeof(data) != TYPE_ARRAY:
		push_error("Economy: %s não é um array JSON" % BUILDINGS_PATH)
		return []
	return data


## Lê os bens (id, nome, preço-base) para o núcleo C++.
func _load_goods() -> Array:
	var file := FileAccess.open(GOODS_PATH, FileAccess.READ)
	if file == null:
		push_error("Economy: não abriu %s" % GOODS_PATH)
		return []
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	if typeof(data) != TYPE_ARRAY:
		push_error("Economy: %s não é um array JSON" % GOODS_PATH)
		return []
	return data


## Lê um array JSON de dados (receitas). Ausência vira [] sem quebrar a carga.
func _load_json_array(path: String) -> Array:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		push_error("Economy: não abriu %s" % path)
		return []
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data if typeof(data) == TYPE_ARRAY else []


## Lê um dicionário JSON de dados (necessidades das POPs).
func _load_json_dict(path: String) -> Dictionary:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		push_error("Economy: não abriu %s" % path)
		return {}
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data if typeof(data) == TYPE_DICTIONARY else {}


## Lê um dicionário JSON OPCIONAL (ausência vira {} sem erro). Usado para os
## dados B7 (informality.json / health_access.json), que o núcleo tolera ausentes
## caindo na âncora de renda / kHealthAccessDefault.
func _load_json_dict_optional(path: String) -> Dictionary:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return {}
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data if typeof(data) == TYPE_DICTIONARY else {}


## Lê as nações do cenário (JSON) para `_nations` (tag -> {name, flag}).
func _load_nations() -> void:
	_nations.clear()
	var file := FileAccess.open(NATIONS_PATH, FileAccess.READ)
	if file == null:
		push_error("Economy: não abriu %s" % NATIONS_PATH)
		return
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	if typeof(data) != TYPE_ARRAY:
		return
	for nation: Variant in data:
		if typeof(nation) == TYPE_DICTIONARY:
			_nations[String(nation["id"])] = nation


## Textura da bandeira de uma nação (ou null). Moddável: `flag` referencia
## `assets/flags/<flag>.png`.
func flag_texture(tag: String) -> Texture2D:
	var nation: Dictionary = _nations.get(tag, {})
	var flag := String(nation.get("flag", ""))
	if flag.is_empty():
		return null
	return load(FLAGS_DIR + flag + ".png") as Texture2D


func _process(delta: float) -> void:
	if _shutting_down:
		return
	# 1. O dia que terminou na thread é publicado agora (sinais).
	_poll_tick()
	# O bench (ou um encerramento na publicação) pode ter fechado tudo acima.
	if _shutting_down:
		return
	# 2. TURBO (ferramenta de teste): sem relógio. Um dia atrás do outro, sem
	# intervalo; `speed` e `_day_fraction` são ignorados de propósito.
	if _turbo.enabled:
		if not _ticker.is_running():
			_start_day()
		return
	if speed == 0:
		return
	_day_fraction += delta / SPEEDS[speed]
	# Teto de backlog: se o dia anterior ainda está na thread, o relógio espera.
	# Nunca há dois ticks simultâneos (nem dois escritores no núcleo).
	if _day_fraction >= 1.0:
		_day_fraction = 1.0
		if not _ticker.is_running():
			_day_fraction = 0.0
			_start_day()


## Agenda o dia do cenário na thread do pool.
func _start_day() -> void:
	if not _ticker.start(sim):
		push_warning("Economy: dia já em andamento na thread; relógio aguarda.")


## Publica o dia concluído (se houver). O status é o snapshot completo do
## núcleo — a UI e a IA leem o último dia inteiro, nunca um dia pela metade.
func _poll_tick() -> void:
	_publish(_ticker.poll())


## Publica um dia concluído: sinais para HUD/comboios. A IA já rodou dentro do
## `tick_and_publish()` do núcleo ANTES de fechar o dia (A5/D-076). A4: o sinal
## carrega só o RESUMO LEVE do dia (totais, contadores, violações); cidades,
## nações, portos e pedidos chegam por consulta sob demanda (`ui_snapshot`,
## `city_columns`, `get_trade_orders`), nunca montando o mundo inteiro no tick.
func _publish(summary: Dictionary) -> void:
	if summary.is_empty():
		return
	var day := int(summary.get("day", 0))
	# TURBO (teste): a menor granularidade de publicação é o lote. Sem a flag
	# este bloco não roda e o caminho é o de sempre.
	if _turbo.enabled:
		if _turbo_publish_day >= 0 and day - _turbo_publish_day < _turbo.render_every:
			return
		_turbo_publish_day = day
	# Bench (teste): mede a parede do primeiro dia publicado até `bench_days`.
	if _turbo.bench_days > 0:
		if _bench_first_day < 0:
			_bench_first_day = day
			_bench_t0_usec = Time.get_ticks_usec()
		if day >= _turbo.bench_days:
			_bench_report(day)
			return
	last_summary = summary
	_ui_snapshot_day = -1  # o retrato da UI é do dia; invalida sem alocar
	_ui_snapshot_version = -1
	status_updated.emit(last_summary)
	trade_orders_ready.emit(
			sim.get_trade_orders() if sim != null else [])


## Retrato sob demanda das telas de dados (A4). Monta `cities`/`nations`/
## `ports`/`currencies`/`tariffs`/`light`/`trade_orders` a partir das consultas
## do núcleo, uma vez por dia. Só é chamado quando alguma tela/​ficha está
## aberta — tela oculta não paga. Nunca é montado no tick.
func ui_snapshot() -> Dictionary:
	if sim == null or last_summary.is_empty():
		return {}
	var day := int(last_summary.get("day", 0))
	# `get_data_version()` sobe a cada dia E a cada mutação (tarifa, obra,
	# choque): um comando do jogador no mesmo dia não deixa a tela com o
	# retrato velho.
	var version := int(sim.get_data_version())
	if day == _ui_snapshot_day and version == _ui_snapshot_version \
			and not _ui_snapshot.is_empty():
		return _ui_snapshot
	var snapshot := last_summary.duplicate()
	snapshot["cities"] = sim.get_city_rows()
	snapshot["nations"] = sim.get_nation_rows()
	snapshot["ports"] = sim.get_ports()
	snapshot["currencies"] = sim.get_currency_rows()
	snapshot["tariffs"] = sim.get_tariffs()
	snapshot["service_sectors"] = sim.get_service_sectors()
	snapshot["light"] = sim.get_light_columns()
	snapshot["trade_orders"] = sim.get_trade_orders()
	_ui_snapshot = snapshot
	_ui_snapshot_day = day
	_ui_snapshot_version = version
	return _ui_snapshot


## Colunas por cidade (A4): Packed arrays por campo, lidas direto do estado.
## É o caminho barato do HUD/mapa/histórico — não constrói Dictionary por cidade.
func city_columns() -> Dictionary:
	if sim == null:
		return {}
	return sim.get_city_columns()


## Fecha o bench: uma linha no log, join da thread e saída com código 0.
## Roda com e sem turbo (`mode=normal` mede o relógio de sempre).
func _bench_report(to_day: int) -> void:
	var wall_s := float(Time.get_ticks_usec() - _bench_t0_usec) / 1_000_000.0
	var days_per_s := 0.0
	if wall_s > 0.0:
		days_per_s = float(to_day - _bench_first_day) / wall_s
	var mode := "turbo" if _turbo.enabled else "normal"
	print("[bench] mode=%s from_day=%d to_day=%d wall_s=%.3f days_per_s=%.2f" % [
		mode, _bench_first_day, to_day, wall_s, days_per_s])
	_shutdown()
	get_tree().quit(0)


## Avança um dia de forma síncrona (testes e ferramentas headless).
func _run_day() -> void:
	_start_day()
	_publish(_ticker.wait())


## Encerramento (sair do jogo, fechar a janela, fim de um teste headless).
##
## O dia roda numa thread do pool. Soltar o `sim` (ou deixar a cena ser
## liberada) com a thread ainda viva dava access violation 0xC0000005
## intermitente NO FIM do processo — inclusive em testes que chamam `quit()`
## sem nunca pedir um tick: o autoload `Economy` agenda o 1º dia no `_ready`,
## que roda mesmo com `--script`. Por isso o join é obrigatório aqui.
func _shutdown() -> void:
	if _shutting_down:
		return
	_shutting_down = true
	speed = 0
	if _ticker != null:
		_ticker.cancel()
	sim = null
	last_summary = {}
	_ui_snapshot = {}
	_ui_snapshot_day = -1


func _exit_tree() -> void:
	_shutdown()


func _notification(what: int) -> void:
	if what == NOTIFICATION_WM_CLOSE_REQUEST or what == NOTIFICATION_PREDELETE:
		_shutdown()


## O modo TURBO (ferramenta de teste/agente) está ativo? Sem a CLI/ambiente: false.
func is_turbo() -> bool:
	return _turbo.enabled


## Configuração do TURBO/bench já resolvida (ferramentas de teste leem daqui).
func turbo_config() -> RefCounted:
	return _turbo


## Troca a velocidade; 0 pausa o relógio. A fração do dia é preservada, então
## mudar de 1x para 2x (ou pausar e voltar) não reinicia o dia.
func set_speed(index: int) -> void:
	speed = clampi(index, 0, SPEED_COUNT - 1)
	speed_changed.emit(speed)


## Alterna pausa/play preservando a velocidade anterior (atalho espaço).
func toggle_pause() -> void:
	if speed == 0:
		set_speed(_resume_speed if _resume_speed > 0 else 1)
	else:
		_resume_speed = speed
		set_speed(0)


## Fração do dia corrente já decorrida (0..1), para a barra do HUD. Congela ao
## pausar em vez de voltar a zero.
func day_progress() -> float:
	return _day_fraction


## Data do jogo, contando os dias simulados a partir de 1º de janeiro de 2027.
func game_date() -> Dictionary:
	var days := int(last_summary.get("day", 0))
	return Time.get_datetime_dict_from_unix_time(START_UNIX + days * 86400)
