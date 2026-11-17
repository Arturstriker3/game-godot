extends RefCounted

## Agenda `sim.tick_and_publish()` na WorkerThreadPool, `days_per_task` dias por
## vez (1 no jogo normal; N no lote do modo TURBO de teste).
##
## Por que assim: o tick C++ de um mundo grande custa dezenas de milissegundos
## e não cabe no frame da virada. O trabalho roda numa thread do pool e a main
## thread só *consulta* (poll) até o dia terminar; depois do join ela monta o
## resumo leve do dia (A4) e a UI busca o resto por consulta sob demanda.
## (Antes, um Dictionary do mundo inteiro era publicado todo dia em buffer duplo.)
##
## Regras de segurança:
## - um único tick por vez (`start()` recusa se já existe um em andamento);
## - só existe UM escritor sobre o `sim`: enquanto a thread roda, a main thread
##   não chama nada do núcleo (nem a IA, nem o relógio). Quem orquestra é
##   `core/economy/economy.gd`.
## - NENHUM Variant/Dictionary do Godot é criado, escrito ou destruído pela
##   thread: ela só chama `tick_and_publish()`, que roda inteiro em C++ e
##   deixa o status publicado no double buffer do núcleo. O `poll()` da main
##   thread recebe o resumo leve do dia em O(1) (`get_day_summary()`).
## - `wait()`/`cancel()` só retornam com a tarefa CONCLUÍDA: nenhum chamador
##   solta o `sim` com o dia em andamento (era a causa do 0xC0000005 no
##   encerramento: a thread seguia viva quando o sim/cena eram liberados).
## - determinismo: o tick não depende do relógio de parede, então o resultado é
##   idêntico ao do caminho síncrono (comprovado em tools/world_scale_bench.gd e
##   tools/economy_tick_async_test.gd).

## Dias simulados por tarefa. 1 = relógio normal do jogo (um dia por tick);
## N = lote do modo TURBO, que existe só como ferramenta de teste/agente.
var days_per_task := 1

var _task_id := -1

## Sim do dia em andamento. Segura a referência enquanto a tarefa existe, para
## que o núcleo não seja liberado debaixo da thread.
var _sim: Object = null


## true enquanto o dia está sendo calculado na thread do pool.
func is_running() -> bool:
	return _task_id != -1


## Agenda o tick de `sim`. Devolve false se já existe um dia em andamento.
##
## Roda `days_per_task` dias seguidos na MESMA tarefa (bind): 1 é o jogo normal,
## N é o lote do TURBO. Continua havendo um único escritor e um único join por
## tarefa — o lote só encurta o número de idas à thread.
func start(sim: Object) -> bool:
	if _task_id != -1:
		return false
	_sim = sim
	_task_id = WorkerThreadPool.add_task(
		_tick_worker.bind(sim, days_per_task), false, "economy day tick")
	return true


## Devolve o resumo quando o dia termina; {} enquanto ele ainda roda.
func poll() -> Dictionary:
	if _task_id == -1:
		return {}
	if not WorkerThreadPool.is_task_completed(_task_id):
		return {}
	return _collect()


## Espera o dia terminar e devolve o resumo (usado por testes e ferramentas).
func wait() -> Dictionary:
	if _task_id == -1:
		return {}
	return _collect()


## Espera o dia terminar e descarta o resumo (encerramento do jogo).
##
## Não pode ser substituído por "largar a referência e sair": a thread do pool
## continuaria escrevendo no `sim` (e no status) depois que a cena fosse liberada.
func cancel() -> void:
	if _task_id == -1:
		_sim = null
		return
	_join()


## Junta a tarefa e devolve o resumo leve do dia (A4). SÓ roda na main thread, e
## só depois do join: o dicionário é construído aqui, fora da worker, sem cruzar
## Variant entre threads. O status completo (mundo inteiro) não é mais montado
## no tick — é consulta sob demanda de tools/testes/goldens.
func _collect() -> Dictionary:
	var sim := _join()
	if sim == null:
		return {}
	return sim.get_day_summary()


## `wait_for_task_completion` libera o ID no pool: consultar `is_task_completed`
## depois disso imprime "Invalid Task ID" e o `_task_id` nunca zeraria.
func _join() -> Object:
	var sim := _sim
	WorkerThreadPool.wait_for_task_completion(_task_id)
	_task_id = -1
	_sim = null
	return sim


func _tick_worker(sim: Object, days: int) -> void:
	# Nada de Variant/Dictionary aqui: o status nasce e é publicado no C++.
	for _day in maxi(1, days):
		sim.tick_and_publish()
