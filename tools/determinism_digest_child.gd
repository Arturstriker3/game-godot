extends SceneTree

## Filho de `tools/determinism_cross_process_test.gd`.
##
## Roda o cenário AGRIA/PORTUS com a IA do C++ e o cérebro econômico LIGADO
## (o mesmo `data/definitions/economy/ai_brain.json` do jogo) por 60 dias,
## com as THREADS PADRÃO do jogo (`set_sim_threads` não é chamado: 0 deixa o
## núcleo decidir), e imprime o SHA-256 do save completo. O pai executa este
## script em dois processos separados e exige o mesmo digest - prova de
## determinismo ENTRE processos, não só entre duas instâncias no mesmo
## processo.
##
## Por que o cérebro precisa estar ligado: o save passa a carregar o bloco
## `definitions.ai_brain` e o caminho da IA muda de comportamento quando ele
## está ativo; dois processos só provam determinismo do que de fato rodou.
## O default compilado é `false`; quem liga é o JSON. (O fixture AGRIA/PORTUS
## não abre planta nova - a fazenda de cada produto já existe -, então o ramo
## de crédito pela caixa viva não é exercitado aqui; ele é coberto pela rodada
## de 365 d com o cérebro ligado, `c1_world_ai_365`.)
## As threads padrão cobrem o caminho concorrente do tick (a IA roda depois do
## `tick_core`), que um teste com 1 thread não vê.

const DAYS := 60
const AI_BRAIN_PATH := "res://data/definitions/economy/ai_brain.json"


func _init() -> void:
	var sim = EconomySim.new()
	sim.initialize_scenario()
	var brain: Variant = _load_json(AI_BRAIN_PATH)
	if not (brain is Dictionary) or not bool((brain as Dictionary).get("enabled", false)):
		_die("ai_brain.json ausente ou com enabled=false")
		return
	sim.set_ai_brain_params(brain)
	sim.set_ai_enabled(true)
	# O cérebro TEM de estar aplicado: sem isto o digest de 60 dias seria o
	# mesmo de antes (o bloco `if (brain)` não roda) e o teste não provaria nada.
	var applied := 0.0
	if sim.has_method("get_ai_brain_status"):
		var brain_status: Dictionary = sim.get_ai_brain_status()
		applied = float((brain_status.get("params", {}) as Dictionary).get(
			"enabled", 0.0))
	if applied <= 0.5:
		_die("set_ai_brain_params não ligou o cérebro (enabled=%.1f)" % applied)
		return
	for _day in DAYS:
		sim.tick_day()
	var status: Dictionary = sim.get_status()
	var attempts := 0
	if sim.has_method("get_ai_brain_status"):
		attempts = int((sim.get_ai_brain_status() as Dictionary).get("attempts", 0))
	# O cenário AGRIA/PORTUS já nasce com uma fazenda por produto e não declara
	# necessidades: o investidor não abre planta nova aqui, então `attempts`
	# pode ser 0. A cobertura do crédito pela caixa viva e da recusa do teto
	# D-126 (D-131) vem da rodada de 365 d com o cérebro ligado
	# (`c1_world_ai_365`, sentinelas do mapa); este teste cobre o determinismo
	# do cérebro ENTRE processos com as threads padrão.
	print("[det] days=%d ai_brain=on attempts=%d" % [
		int(status.get("day", 0)), attempts])
	var payload := JSON.stringify(sim.to_dict(), "", true, true)
	print("[det] sha256=" + payload.sha256_text())
	quit(0)


func _die(message: String) -> void:
	print("[det] ERRO: " + message)
	push_error("[det] " + message)
	quit(1)


func _load_json(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		push_error("[det] não abriu " + path)
		return null
	var parsed: Variant = JSON.parse_string(file.get_as_text())
	if parsed == null:
		push_error("[det] JSON inválido em " + path)
	return parsed
