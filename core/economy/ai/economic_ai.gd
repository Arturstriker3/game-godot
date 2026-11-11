extends RefCounted
class_name EconomicAI

## Orquestrador da camada de decisão econômica. Roda DEPOIS do tick do
## EconomySim e mantém um log explicável de cada decisão. Sem LLM: as decisões
## são heurísticas determinísticas.
##
## Planejamento ESCALONADO (sem pico na virada do dia): cada cidade tem sua vez
## dentro do ciclo de `interval_days` (cidade de índice i planeja nos dias em
## que `dia % interval_days == i % interval_days`). A cadência por cidade
## continua sendo um planejamento a cada 7 dias — o balanceamento é equivalente
## —, mas o trabalho diário cai para ~1/7 do antigo "tudo no dia múltiplo de 7".
##
## Fatiamento por frame: `plan()` só enfileira as cidades do dia; `pump()` roda
## até o orçamento (1,5 ms) e devolve o controle. O autoload `Economy` chama
## `pump()` nos frames seguintes, fora do frame da virada.
##
## O status vem do próprio `tick_demo()` (que já devolve o dicionário completo):
## não há mais um `get_demo_status()` extra de ~6–9 ms por ciclo.

const PrivateInvestorScript := preload("res://core/economy/ai/private_investor.gd")
const GovernmentAIScript := preload("res://core/economy/ai/government_ai.gd")
const Templates := preload("res://core/economy/ai/industry_templates.gd")

var sim: Object
var geo: Object
var private_investor: Object
var government: Object
var decisions: Array[Dictionary] = []
var interval_days := 7

## Fila do dia corrente (cidades ainda não planejadas) e índice de progresso.
var _pending: Array = []
var _pending_pos := 0
var _pending_day := 0
var _pending_status: Dictionary = {}
## Número de cidades conhecido; evita buscar status em dias sem nenhuma cidade
## agendada (cenários pequenos, ex.: testes com 2 cidades).
var _city_count := 0


func setup(sim_node: Object, geography: Object) -> bool:
	sim = sim_node
	geo = geography
	private_investor = PrivateInvestorScript.new()
	private_investor.setup(sim, geo)
	government = GovernmentAIScript.new()
	government.setup(sim, geo)
	return geo.load_data()


## Executa a IA do dia de forma SÍNCRONA (ferramentas, testes e balance_review).
## `summary` pode ser o dicionário devolvido por `tick_demo()` — evita um
## `get_demo_status()` redundante.
func run(day: int, summary: Dictionary = {}) -> Array:
	plan(day, summary)
	_drain_all()
	return decisions


## Enfileira as cidades cuja vez é este dia. `summary` é o status do tick; se
## vazio, busca `get_demo_status()` só quando há cidade agendada.
func plan(day: int, summary: Dictionary = {}) -> void:
	if is_busy() and _pending_day != day:
		# Garantia de ordem: um dia novo nunca começa com a fila do anterior
		# pela metade (no jogo o dia dura 60 s; em ferramentas, run() é síncrono).
		_drain_all()

	_pending.clear()
	_pending_pos = 0
	var phase := day % interval_days
	if _city_count > 0 and phase >= mini(_city_count, interval_days):
		return

	var status := summary
	if status.is_empty():
		status = sim.get_demo_status()
	var cities: Array = status.get("cities", [])
	_city_count = cities.size()
	_pending_day = day
	_pending_status = status
	for index in cities.size():
		if index % interval_days == phase:
			_pending.append(cities[index])


## Continua o planejamento do dia até gastar `frame_budget_us` (~1,5 ms). O
## trabalho é por cidade inteira (privado + governo), então o orçamento é um
## teto suave: uma cidade pode terminar acima dele, mas nunca a fila inteira.
func pump(frame_budget_us: int) -> Array:
	if _pending_pos >= _pending.size():
		return decisions
	var start := Time.get_ticks_usec()
	while _pending_pos < _pending.size():
		_run_city(_pending[_pending_pos])
		_pending_pos += 1
		if Time.get_ticks_usec() - start >= frame_budget_us:
			break
	if _pending_pos >= _pending.size():
		_pending.clear()
		_pending_status = {}
		_pending_pos = 0
	return decisions


func is_busy() -> bool:
	return _pending_pos < _pending.size()


func get_decisions() -> Array:
	return decisions.duplicate(true)


func print_log() -> void:
	for decision in decisions:
		print("[AI] dia %d %-10s %-6s %-10s %-8s custo %.0f | %s" % [
			int(decision.get("day", 0)),
			String(decision.get("agent", "")),
			String(decision.get("city", "")),
			String(decision.get("action", "")),
			String(decision.get("resource", "")),
			float(decision.get("cost", 0.0)),
			String(decision.get("reason", "")),
		])


func write_log(path: String) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		return
	file.store_string(JSON.stringify(decisions, "  "))
	file.close()


## Uma cidade: investidor privado + governo compartilham o mesmo status do dia.
func _run_city(city: Dictionary) -> void:
	private_investor.run_city(_pending_day, city, _pending_status)
	government.run_city(_pending_day, city, _pending_status)
	decisions.append_array(private_investor.drain())
	decisions.append_array(government.drain())


func _drain_all() -> void:
	while _pending_pos < _pending.size():
		_run_city(_pending[_pending_pos])
		_pending_pos += 1
	_pending.clear()
	_pending_status = {}
	_pending_pos = 0


func _find_city(status: Dictionary, city_id: String) -> Dictionary:
	for city in status.get("cities", []):
		if String(city["id"]) == city_id:
			return city
	return {}


func _center(city: Dictionary) -> Vector2:
	var center: Array = city.get("center", [0.0, 0.0])
	if center.size() < 2:
		return Vector2.ZERO
	return Vector2(float(center[0]), float(center[1]))
