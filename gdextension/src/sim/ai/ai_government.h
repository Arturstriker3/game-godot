#pragma once
// A5 — governo (IA estatal) da economia em C++.
//
// PORTE FIEL de `core/economy/ai/government_ai.gd` (350 linhas): as mesmas
// funções, na mesma ordem de leitura, com as mesmas condições, constantes e
// operações em ponto flutuante. Nada aqui decide por critério novo — o aceite é
// a equivalência com o GDScript.
//
// Mapa função → função (GDScript → C++):
//   run / run_city           → run_city (plano de UMA cidade; a view já é por
//                              cidade, o rebind do `light` não existe aqui)
//   _run_city                → plan_city
//   _build_gate              → build_gate
//   _has_food_producer       → has_food_producer
//   _best_food_crop          → best_food_crop
//   _building_index          → ai_build_index (ai_view.h)
//   _buildings_of_type       → ai_buildings_of_type (ai_view.h)
//   _avg_utilization         → ai_avg_utilization (ai_view.h)
//   _has_deposit             → ai_has_deposit (ai_view.h)
//   _input_available /
//     _resource_available    → NÃO portados: o fluxo do governo não os chama
//                              (confirmado no .gd — quem filtra é
//                              `_best_food_crop` e o laço de minerais, ambos
//                              com aptidão/jazida direto).
//
// Contrato:
//   * uma ação por cidade: segurança alimentar (déficit real ≥ 1 t-eq e nenhum
//     produtor de alimento na cidade) e, se ela não construir, a mina estatal
//     de maior margem (candidatos ordenados por margem decrescente);
//   * anti-loop B5: cooldown de 30 dias por cidade (`AiState::last_build_day`),
//     teto de 6 construções do tipo e utilização média efetiva ≥ 60%;
//   * margem honesta com `ai_variable_cost_mined` (opex × profundidade);
//   * gasto limitado a 25% do orçamento do governo (`BUDGET_SHARE`);
//   * leitura só da `AiView` congelada do dia (ai_view.h), nunca do WorldState
//     vivo; id `STATE-<RECURSO>-<n>` com o contador em `AiState::government_id`,
//     incrementado SÓ no sucesso;
//   * sem log de decisão (`avoid`/`rejected`/`build_state`): não tem efeito no
//     mundo nem entra no save.

namespace ogs {

struct AiContext;

struct AiGovernment {
	void run_city(AiContext& ctx, int city_index);
};

} // namespace ogs
