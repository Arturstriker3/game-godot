extends RefCounted

## Agenda `sim.tick_demo()` na WorkerThreadPool, um dia por vez.
##
## Por que assim: o tick C++ de um mundo grande custa dezenas de milissegundos
## e não cabe no frame da virada. O trabalho roda numa thread do pool e a main
## thread só *consulta* (poll) até o dia terminar; o resumo é publicado de uma
## vez, então a UI sempre lê o último status COMPLETO (double-buffer natural).
##
## Regras de segurança:
## - um único tick por vez (`start()` recusa se já existe um em andamento);
## - só existe UM escritor sobre o `sim`: enquanto a thread roda, a main thread
##   não chama nada do núcleo (nem a IA, nem o relógio). Quem orquestra é
##   `core/economy/economy.gd`.
## - determinismo: o tick não depende do relógio de parede, então o resultado é
##   idêntico ao do caminho síncrono (comprovado em tools/world_scale_bench.gd e
##   tools/economy_tick_async_test.gd).

var _task_id := -1
var _result: Dictionary = {}


## true enquanto o dia está sendo calculado na thread do pool.
func is_running() -> bool:
	return _task_id != -1


## Agenda o tick de `sim`. Devolve false se já existe um dia em andamento.
func start(sim: Object) -> bool:
	if _task_id != -1:
		return false
	_result = {}
	_task_id = WorkerThreadPool.add_task(
		_tick_worker.bind(sim), false, "economy day tick")
	return true


## Devolve o resumo quando o dia termina; {} enquanto ele ainda roda.
func poll() -> Dictionary:
	if _task_id == -1:
		return {}
	if not WorkerThreadPool.is_task_completed(_task_id):
		return {}
	WorkerThreadPool.wait_for_task_completion(_task_id)
	_task_id = -1
	var summary := _result
	_result = {}
	return summary


## Espera o dia terminar (usado por testes e ferramentas headless).
func wait() -> Dictionary:
	if _task_id == -1:
		return {}
	# `wait_for_task_completion` libera o ID no pool: consultar `is_task_completed`
	# depois disso imprime "Invalid Task ID" e o `_task_id` nunca zeraria.
	WorkerThreadPool.wait_for_task_completion(_task_id)
	_task_id = -1
	var summary := _result
	_result = {}
	return summary


func _tick_worker(sim: Object) -> void:
	_result = sim.tick_demo()
