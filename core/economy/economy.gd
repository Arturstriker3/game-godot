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
## `sim` por vez — enquanto a thread roda, ninguém (nem a IA) mexe no núcleo.

## Emitido a cada tick com os pedidos de comércio do dia.
signal trade_orders_ready(orders: Array)

## Emitido a cada tick com o resumo completo (cidades, nações e pedidos).
signal status_updated(summary: Dictionary)

## Emitido quando a velocidade muda (botão ou atalho espaço). Índice 0 = pausa.
signal speed_changed(index: int)

## Segundos reais por dia simulado; o índice 0 é a pausa.
## Ritmo normal: 30 dias em 30 minutos (1 dia = 60 s); 2x = 30 s; 3x = 15 s.
const SPEEDS := [0.0, 60.0, 30.0, 15.0]
const SPEED_COUNT := 4
## Orçamento por frame do planejamento da IA (~1,5 ms). O dia só agenda; o
## trabalho das cidades é fatiado nos frames seguintes, fora da virada.
const AI_FRAME_BUDGET_US := 1500

## Agenda o tick do dia na thread do pool (ver tick_scheduler.gd).
const TickScheduler := preload("res://core/economy/tick_scheduler.gd")

## 1º de janeiro de 2026, 00:00 UTC.
const START_UNIX := 1767225600

## Cidades do cenário (geradas de data/definitions/scenario/cities.csv).
const CITIES_PATH := "res://data/definitions/scenario/cities.json"
## Portos do cenário (gerados de data/definitions/scenario/ports.csv).
const PORTS_PATH := "res://data/definitions/scenario/ports.json"
## Construções do cenário (geradas de data/definitions/scenario/buildings.csv).
const BUILDINGS_PATH := "res://data/definitions/scenario/buildings.json"
## Bens (id, nome, preço-base) — data-driven, editáveis sem recompilar o C++.
const GOODS_PATH := "res://data/definitions/economy/goods.json"
## Nações do cenário (geradas de data/definitions/scenario/nations.csv).
const NATIONS_PATH := "res://data/definitions/scenario/nations.json"
## Bandeiras por código ISO-2 (moddável: adicione assets/flags/<código>.png).
const FLAGS_DIR := "res://assets/flags/"

var sim: RefCounted
var last_summary: Dictionary = {}
var speed: int = 1
var _nations: Dictionary = {}  # tag -> {id, name, flag}

## Camada de decisão econômica (investidor privado + governo), rodando sobre o
## estado do demo em intervalo fixo. Criada no _ready depois do sim.
var _ai: Object = null

## Velocidade para a qual voltar ao sair da pausa (espaço).
var _resume_speed: int = 1

## Fração do dia corrente (0..1). Não depende da velocidade.
var _day_fraction := 0.0

## Agenda o dia na thread do pool; `is_running()` = dia em cálculo.
var _ticker: RefCounted = TickScheduler.new()

## Já juntamos a thread do dia? Ver `_shutdown()`.
var _shutting_down := false


func _ready() -> void:
	sim = EconomySim.new()
	# Resolve a fazenda inicial de cada cidade para o alimento básico de maior
	# aptidão × caloria da nação (antes de criar os prédios no núcleo). Sem dados,
	# mantém trigo.
	var geo := GeographyService.new()
	var cities := _load_cities()
	sim.initialize_demo_world(cities, _load_ports(), geo.resolve_initial_farms(_load_buildings(), cities))
	sim.demo_set_goods(_load_goods())
	_load_nations()
	# Aplica a geografia real (aptidão de culturas + reservas minerais) desde o
	# início — sem prospecção: todo depósito é conhecido.
	geo.apply_to_sim(sim, _cities_with_center())
	# Pré-aquece o cache do status (os dois slots): o primeiro dia publicado em
	# um slot frio pagaria a criação de ~20 mil dicionários (medido: 60 ms contra
	# 17 ms num dia quente, com 858 cidades). Aqui é custo de carga, não de tick.
	sim.demo_warm_status()
	_setup_ai(geo)

	# Adia o primeiro dia: a cena principal conecta os sinais no _ready. O dia
	# roda direto na thread do pool; o primeiro status sai no _process.
	_start_day.call_deferred()


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


## Cria a camada de decisão (EconomicAI) sobre o estado do demo, com o serviço
## de geografia para revelar depósitos e aptidão territorial.
func _setup_ai(geo: GeographyService) -> void:
	_ai = EconomicAI.new()
	_ai.setup(sim, geo)


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
	# 1. O dia que terminou na thread é publicado agora (sinais + IA).
	_poll_tick()
	# 2. Planejamento escalonado da IA: continua no frame seguinte, com
	# orçamento. Nunca durante um tick — só existe um escritor sobre o `sim`.
	if _ai != null and not _ticker.is_running() and _ai.is_busy():
		_ai.pump(AI_FRAME_BUDGET_US)
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


## Publica um dia concluído: sinais para HUD/comboios e planejamento da IA.
func _publish(summary: Dictionary) -> void:
	if summary.is_empty():
		return
	last_summary = summary
	status_updated.emit(last_summary)
	trade_orders_ready.emit(last_summary.get("trade_orders", []))
	# A camada de decisão roda depois do tick: as escolhas (investir, construir,
	# revelar depósito) valem a partir do próximo dia. Escalonada por cidade e
	# fatiada nos frames seguintes — o status do tick é reusado, sem um segundo
	# `get_demo_status()` (~6–9 ms).
	if _ai != null:
		_ai.plan(int(last_summary.get("day", 0)), last_summary)


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
	_ai = null
	sim = null
	last_summary = {}


func _exit_tree() -> void:
	_shutdown()


func _notification(what: int) -> void:
	if what == NOTIFICATION_WM_CLOSE_REQUEST or what == NOTIFICATION_PREDELETE:
		_shutdown()


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


## Data do jogo, contando os dias simulados a partir de 1º de janeiro de 2026.
func game_date() -> Dictionary:
	var days := int(last_summary.get("day", 0))
	return Time.get_datetime_dict_from_unix_time(START_UNIX + days * 86400)
