extends Node

## Autoload `Economy`: acopla o núcleo C++ (EconomySim) ao jogo.
##
## Avança um dia por tick e publica o resumo para o HUD e para os comboios.
## A simulação é por cidade: cada nação possui uma ou mais cidades, e cada
## cidade tem população, produção e demanda próprias.
##
## O relógio é um acumulador da FRAÇÃO do dia (0..1), e não um `Timer`: trocar
## de velocidade (ou pausar) preserva o progresso do dia em vez de reiniciá-lo.

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

## 1º de janeiro de 2026, 00:00 UTC.
const START_UNIX := 1767225600

## Cidades do cenário (geradas de data/definitions/scenario/cities.csv).
const CITIES_PATH := "res://data/definitions/scenario/cities.json"
## Portos do cenário (gerados de data/definitions/scenario/ports.csv).
const PORTS_PATH := "res://data/definitions/scenario/ports.json"
## Nações do cenário (geradas de data/definitions/scenario/nations.csv).
const NATIONS_PATH := "res://data/definitions/scenario/nations.json"
## Bandeiras por código ISO-2 (moddável: adicione assets/flags/<código>.png).
const FLAGS_DIR := "res://assets/flags/"

var sim: RefCounted
var last_summary: Dictionary = {}
var speed: int = 1
var _nations: Dictionary = {}  # tag -> {id, name, flag}

## Velocidade para a qual voltar ao sair da pausa (espaço).
var _resume_speed: int = 1

## Fração do dia corrente (0..1). Não depende da velocidade.
var _day_fraction := 0.0


func _ready() -> void:
	sim = EconomySim.new()
	sim.initialize(_load_cities(), _load_ports())
	_load_nations()

	# Adia o primeiro dia: a cena principal conecta os sinais no _ready.
	_run_day.call_deferred()


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
	if speed == 0:
		return
	_day_fraction += delta / SPEEDS[speed]
	if _day_fraction >= 1.0:
		_day_fraction = fmod(_day_fraction, 1.0)
		_run_day()


func _run_day() -> void:
	last_summary = sim.tick()
	status_updated.emit(last_summary)
	trade_orders_ready.emit(last_summary["trade_orders"])


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
