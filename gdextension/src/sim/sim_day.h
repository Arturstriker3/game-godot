#pragma once
// Ordem do dia da simulação, sem Godot (fase A1a: núcleo C++ puro).
//
// `tick_day` é a sequência exata que a fachada `EconomySim::tick_core`
// executava inline depois de `initialize_scenario()`/`seed_industry()`: um
// prólogo seguido de uma fase por etapa econômica. Mora na biblioteca para que
// um executável headless (CLI) rode N dias sem o engine.
//
// Contrato:
//   * mesma ordem, nenhuma fase a mais nem a menos — a ordem é o resultado
//     numérico do dia (dinheiro, preços, invariantes);
//   * sem lock, sem log e sem status: quem chama segura o mutex e trata
//     `ws.invariant_violations` (a fachada loga com push_error);
//   * nada de inicialização do mundo aqui (`initialize_scenario`,
//     `seed_industry` ficam na fachada).

#include "sim_core.h"
#include "world_state.h"

namespace ogs {

// Avança UM dia do cenário. Sem lock: o chamador serializa o acesso.
void tick_day(WorldState& ws, SimCore& core);

} // namespace ogs
