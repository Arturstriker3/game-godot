#pragma once
// G2 — PLANEJAMENTO DA SEMENTE INDUSTRIAL (spec §2.3–§2.5).
//
// Função pura: lê o dado estático (`WorldState::bootstrap`), a demanda-âncora
// (mesma propagação de `potential_demand`) e as receitas vivas, e devolve as
// plantas que a semente criaria. `seed_industry` só executa o plano com
// `build()`. Nenhum RNG, nenhum `unordered_map`: duas chamadas no mesmo mundo
// devolvem o mesmo plano (determinismo testável sem Godot).
#include "world_state.h"

#include <vector>

namespace ogs {
namespace bootstrap {

struct Plan {
	std::vector<BootstrapPlanRow> rows;  // ordem: bem declarado → nação → cidade
	double shortfall = 0.0;              // capacidade que não coube no orçamento
	double unseeded_share = 0.0;         // participação residual declarada
	int seeded = 0;                      // plantas com plano válido
};

Plan plan(const WorldState& ws);

} // namespace bootstrap
} // namespace ogs
