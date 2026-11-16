#pragma once
// Cenário de validação AGRIA/PORTUS (S14 do docs/economy/core_refactor_plan.md).
// C++ puro (só STL, via sim_core.h) — nada de godot_cpp. A fachada EconomySim
// (economy_sim.*) chama `initialize_demo_fixture(ws_, sim_core_)` depois de
// resetar o cache de status, que é estado Godot e NÃO pertence ao núcleo.

#include "sim_core.h"

namespace ogs {

// Popula `ws` com o cenário mínimo de validação (Agria/Portus, fazendas de
// trigo, mercado de trabalho, frota DEMO, serviços e grupos iniciais) e chama
// os helpers de pós-inicialização do `core` (índices, frota, serviços, grupos).
// O cache de status e as versões dele ficam na fachada.
void initialize_demo_fixture(WorldState& ws, SimCore& core);

} // namespace ogs
