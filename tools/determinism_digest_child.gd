extends SceneTree

## Filho de `tools/determinism_cross_process_test.gd`.
##
## Roda o cenário AGRIA/PORTUS com a IA do C++ ligada por 12 dias e imprime o
## SHA-256 do save completo. O pai executa este script em dois processos
## separados e exige o mesmo digest — prova de determinismo ENTRE processos,
## não só entre duas instâncias no mesmo processo.

const DAYS := 12


func _init() -> void:
	var sim = EconomySim.new()
	sim.set_sim_threads(1)
	sim.initialize_scenario()
	sim.set_ai_enabled(true)
	for _day in DAYS:
		sim.tick_day()
	var payload := JSON.stringify(sim.to_dict(), "", true, true)
	print("[det] sha256=" + payload.sha256_text())
	quit(0)
