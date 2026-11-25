#pragma once
// G2 (spec G2_bootstrap_industrial.md §3) — DEMANDA POTENCIAL NA VISÃO DA IA.
//
// Potencial[produto] de uma cidade = demanda final de `needs.json` (mesmas
// âncoras do consumo, incluindo o computador institucional) propagada para
// TRÁS pela árvore de receitas vigente, com desconto por profundidade.
//
// Regras declaradas:
//   * uma rota por produto (maior soma qty/yield entre as receitas que o
//     produzem; empate = ordem do JSON = menor índice de receita);
//   * profundidade máxima 8 (corte de ciclo);
//   * produtos sem receita (minério/fazenda primária) NÃO recebem potencial;
//   * substitutos dividem a categoria igualmente;
//   * `durable_scale` como no consumo (reposição anual de duráveis);
//   * cap = teto por produto como múltiplo da âncora final da cidade.
//
// A mesma função serve os dois usos: física (semente A, soma mundial) e sinal
// (B, piso por cidade em `snapshot_ai_view`). Nada aqui toca o mundo vivo.
#include "world_state.h"

#include <cstddef>
#include <vector>

namespace ogs {

struct PotentialDemandIndex {
	// Receitas por produto de saída (índices em `WorldState::recipes`).
	std::vector<std::vector<int>> recipes_by_output;
	// Rota escolhida por produto (-1 = sem receita).
	std::vector<int> chosen_recipe;
};

// Monta o índice de rotas (uma vez por snapshot/semeadura).
void build_potential_demand_index(const WorldState& ws,
	PotentialDemandIndex& index);

// Potencial por produto de UMA cidade. `out` fica com `ws.products` posições.
// Sem `needs_loaded` ou cidade inválida, devolve tudo zero (ausência honesta).
void potential_demand_for_city(const WorldState& ws,
	const PotentialDemandIndex& index, int city_index,
	const AiBrainParams& params, std::vector<double>& out);

// Soma mundial do potencial (âncora física da semente A).
void potential_demand_world(const WorldState& ws,
	const PotentialDemandIndex& index, const AiBrainParams& params,
	std::vector<double>& out);

// ---------------------------------------------------------------------------
// G2 (perf) — CACHE MENSAL do piso por cidade.
//
// A propagacao depende de: populacao da cidade, needs.json (categorias,
// durabilidade, governo), receitas vigentes (rota escolhida) e os parametros
// do piso. Nada disso muda dentro do mes, entao o piso e recalculado no dia 0
// e a cada `potential_demand.recompute_days` dias (default 30) e reusado nos
// dias do meio. A populacao flutua dia a dia; o cache a congela DE PROPOSITO
// (piso mensal declarado, determinista): o custo por dia deixa de pagar a
// propagacao e a trajetoria continua reproduzivel.
//
// `stale` = cache vazio, dimensoes diferentes, definicoes mudadas, dia
// andou para tras (save/load) ou o periodo venceu.
// ---------------------------------------------------------------------------
struct PotentialDemandCache {
	// Piso por cidade x produto (indice = city * products + product).
	std::vector<double> values;
	int cities = 0;
	int products = 0;
	int day = -1;  // dia do ultimo recalculo (ws.day)
	// Assinatura minima das definicoes que mudam a propagacao.
	std::size_t recipes = 0;
	std::size_t needs_levels = 0;
	std::size_t government_products = 0;
	double durable_decay = 0.0;
	double discount = -1.0;
	double cap = -1.0;
	bool needs_loaded = false;
};

bool potential_demand_cache_stale(const WorldState& ws,
	const PotentialDemandCache& cache);

// Recalcula o piso de TODAS as cidades e carimba o cache com o dia corrente.
void potential_demand_refresh_cache(const WorldState& ws,
	PotentialDemandCache& cache);

} // namespace ogs
