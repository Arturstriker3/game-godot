#pragma once
// A5 — investidor por GRUPO/EMPRESA (F9) da economia em C++.
//
// PORTE FIEL de `core/economy/ai/group_investor.gd` (890 linhas): as mesmas
// funções, na mesma ordem de leitura, com as mesmas condições, constantes e
// operações em ponto flutuante. Nada aqui decide por critério novo — o aceite é
// a equivalência com o GDScript.
//
// Mapa função → função (GDScript → C++):
//   queue_day / run_slice / drain   → run (o dia inteiro de uma vez: o C++ roda
//                                     síncrono, sem fatiamento por frame)
//   _prepare_slice (passo 0)        → run: fila dos grupos da vez e do ranking
//   _prepare_slice (passo 1)        → run: comida segura + sinais do dia
//   _next_queued / _begin_group     → begin_group (gate de caixa mínimo)
//   _advance_group                  → decide_group (percorre os recursos e a
//                                     base de cada um; melhor estritamente >)
//   _finish_group                   → decide_group + execute (uma por grupo)
//   _resources_for                  → resources_for (cesta + setor)
//   _ensure_sector_map              → resources_for (produtos do goods.json)
//   _resource_context               → resource_context (template do recurso)
//   _build_base_slice               → base_for (varredura completa das cidades)
//   _append_candidate               → append_candidate (filtros exatos)
//   _score_candidate                → score_candidate (risco × retorno)
//   _execute                        → execute (group_invest + débito local)
//   _load_signals                   → load_signals (pending global + caches)
//   _measure_pending_global         → load_signals
//   _policy / _confidence / _tariff → fdi_policy / nation_confidence /
//                                     tariff_rate
//   _is_incumbent                   → is_incumbent (índice nação|recurso)
//   _partner_for                    → partner_for (ranking, caixa local)
//   _compute_food_secure            → compute_food_secure (satisfação média)
//
// NÃO portados (sem efeito no mundo nem no save):
//   * `name_groups` — nomeação por cultura, feita no cliente (o C++ só decide);
//   * `get_decisions` / `take_decisions` / `clear_decisions` — log de decisão;
//   * `best_opportunity` — caminho de teste/ferramenta do GDScript.
//
// Contrato:
//   * cadência BAIXA por grupo: o grupo na posição `i` do ranking decide quando
//     `day % 30 == i % 30`, NA ORDEM do ranking (`get_group_ranking`);
//   * enquanto a satisfação alimentar média do mundo < 0,98, o grupo só
//     considera a cesta; senão a cesta + os recursos do seu setor na ordem do
//     goods.json;
//   * por recurso, uma base de candidatos (varredura de TODAS as cidades, na
//     ordem do índice) independente do grupo; o que resta por grupo é o risco
//     (tarifa, confiança, política de IDE, JV) e o caixa;
//   * uma ação por grupo/dia: `group_invest` (obra física) ou nada;
//   * tudo é lido da `AiView` congelada (ai_view.h) e dos sinais do núcleo
//     (`ws.fdi_policies`, `ws.confidence`, `SimCore::tariff_rate_text`) —
//     nunca do WorldState vivo além disso;
//   * o ÚNICO estado que cruza dias é `AiState::group_cash` (cópia local do
//     caixa, inicializada pela fase antes de chamar `run`); este struct não
//     guarda nada entre dias.

namespace ogs {

struct AiContext;

struct AiGroupInvestor {
	// Um dia inteiro de decisões de grupo (== `run_day` do GDScript). Chamado
	// por `AiPhase::run` DEPOIS de `AiPrivateInvestor::prepare` e ANTES das
	// cidades — a mesma ordem do `plan()` do `economic_ai.gd`.
	void run(AiContext& ctx);
};

} // namespace ogs
