extends Node

## Autoload `Economy`: acopla o núcleo C++ (EconomySim) ao jogo.
##
## Avança um dia por tick e publica o resumo para o HUD e para os comboios.
## A simulação é por cidade: cada nação possui uma ou mais cidades, e cada
## cidade tem população, produção e demanda próprias.

## Emitido a cada tick com os pedidos de comércio do dia.
signal trade_orders_ready(orders: Array)

## Emitido a cada tick com o resumo completo (cidades, nações e pedidos).
signal status_updated(summary: Dictionary)

## Segundos reais por dia simulado; o índice 0 é a pausa.
const SPEEDS := [0.0, 1.5, 0.75, 0.3]
const SPEED_COUNT := 4

## 1º de janeiro de 2026, 00:00 UTC.
const START_UNIX := 1767225600

var sim: RefCounted
var last_summary: Dictionary = {}
var speed: int = 1

var _timer: Timer


func _ready() -> void:
	sim = EconomySim.new()
	sim.initialize()

	_timer = Timer.new()
	_timer.wait_time = SPEEDS[speed]
	_timer.timeout.connect(_run_day)
	add_child(_timer)
	_timer.start()

	# Adia o primeiro dia: a cena principal conecta os sinais no _ready.
	_run_day.call_deferred()


func _run_day() -> void:
	last_summary = sim.tick()
	status_updated.emit(last_summary)
	trade_orders_ready.emit(last_summary["trade_orders"])


## Troca a velocidade; 0 pausa o relógio.
func set_speed(index: int) -> void:
	speed = clampi(index, 0, SPEED_COUNT - 1)
	if speed == 0:
		_timer.stop()
		return
	_timer.wait_time = SPEEDS[speed]
	_timer.start()


## Fração do dia corrente já decorrida (0..1), para a barra do HUD.
func day_progress() -> float:
	if speed == 0 or _timer.wait_time <= 0.0:
		return 0.0
	return clampf(1.0 - _timer.time_left / _timer.wait_time, 0.0, 1.0)


## Data do jogo, contando os dias simulados a partir de 1º de janeiro de 2026.
func game_date() -> Dictionary:
	var days := int(last_summary.get("day", 0))
	return Time.get_datetime_dict_from_unix_time(START_UNIX + days * 86400)
