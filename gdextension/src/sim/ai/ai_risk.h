#pragma once
// D-131 (fase B) — R2 (concorrência) e R4 (exposição e anti-manada).
//
// Regras puras sobre o retrato do dia (`AiView`) e os caches do dia em
// `AiState` (`AiSupplyCache`/`AiExposureCache`). Nada aqui lê arquivo, toca o
// mundo ou depende de godot-cpp.
//
//   * R2 — oferta efetiva = existente + em construção + em rampa, no escopo
//     declarado (cesta alimentar = região; demais = mundo). `pending_committed`
//     (obras emitidas na MESMA fase) entra no desconto da decisão seguinte.
//     A demanda futura aplica a inclinação da média móvel de `tot_desired`.
//   * R4 — exposição de capital por agente (privado = cidade/dono `private`;
//     grupo = id), por bem e por setor, e o teto rolling de 30 dias por bem.
//
// Todos os números são defaults declarados (`ai_brain.json`). `enabled=false`
// não constrói cache nem consulta nada disto.
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "../world_state.h"
#include "ai_view.h"

namespace ogs {

struct AiState;

// Agente de exposição (R4): capital por bem e por setor.
struct AiExposure {
	double total = 0.0;
	std::vector<double> by_product;
	std::vector<double> by_sector;
	int goods = 0;
};

// Oferta/demanda do dia por escopo. `world_*` tem `products` posições;
// `region_*` é achatado `regions * products + product`.
struct AiSupplyCache {
	int products = 0;
	int regions = 0;
	std::vector<double> world_supply;
	std::vector<double> world_demand;
	std::vector<double> world_growth;
	// Capacidade NOMINAL existente (sem aptidão/eficiência) por produto: base
	// do teto de capacidade nova do anti-manada.
	std::vector<double> world_capacity;
	std::vector<double> region_supply;
	std::vector<double> region_demand;
	std::unordered_map<std::string, int> region_index;
};

// Exposição do dia por agente. `city` tem uma posição por cidade do retrato;
// `groups` é indexado pelo id do grupo. As duas fontes são atualizadas quando
// uma obra é emitida (o delta do mesmo dia entra na decisão seguinte).
struct AiExposureCache {
	int products = 0;
	int sectors = 0;
	std::vector<std::string> sector_names;
	std::vector<int> product_sector;
	std::vector<AiExposure> city;
	std::unordered_map<std::string, AiExposure> groups;
	std::unordered_map<std::string, int> city_index;
};

namespace ai_risk {

// Constrói os caches do dia (uma passagem pelas construções/obras). Só é
// chamado com `enabled=true`; o chamador guarda o resultado em `AiState`.
void build_supply_cache(const WorldState& ws, const AiView& view,
	const AiTemplates& templates, const AiBrainParams& params,
	const std::vector<std::vector<double>>& demand_history,
	AiSupplyCache& out);
void build_exposure_cache(const WorldState& ws, const AiView& view,
	const AiTemplates& templates, AiExposureCache& out);

// Índice da massa de terra no cache (-1 = não resolvida).
int region_slot(const AiSupplyCache& cache, const std::string& landmass);
// Índice do setor em `AiExposureCache::sector_names` (-1 = sem setor).
int sector_slot(const AiExposureCache& cache, int product);

// R2 — oferta efetiva no escopo. `region=true` usa a massa da cidade.
double supply_effective(const AiSupplyCache& cache, const AiState& state,
	int product, int region, bool region_scope);
// R2 — demanda futura = demanda do dia × (1 + crescimento × janela).
double demand_future(const AiSupplyCache& cache, int product, int region,
	bool region_scope, int growth_days);

// R4 — exposição do agente no cache (`city_id` para o privado, `group_id`
// para o grupo). O ponteiro devolvido vale enquanto o cache viver.
const AiExposure* city_exposure(const AiExposureCache& cache, int city);
const AiExposure* group_exposure(const AiExposureCache& cache,
	const std::string& group_id);

// R4 — o acréscimo de `cost` no bem `product` cabe nos tetos do agente?
// `cash` = caixa do agente na visão do dia. Também barra um 9º bem distinto.
bool exposure_allows(const AiBrainParams& params, const AiExposure& exposure,
	int product, int sector, double cost, double cash);

// R4 — anti-manada: a `N`-ésima planta do bem na janela de 30 dias e o teto
// de capacidade nova cabem? `existing_capacity` = capacidade nominal existente
// do bem no mundo. A PRIMEIRA planta da janela é sempre permitida (evita
// travar um bem novo com capacidade existente zero); o teto de capacidade
// vale a partir da segunda. Comida usa `food_multiplier`.
bool herd_allows(const AiState& state, const AiBrainParams& params,
	int product, bool food, int day, double capacity,
	double existing_capacity);
// Registra a planta criada (só no sucesso do comando).
void herd_record(AiState& state, int product, double capacity, int day);

// Atualiza a exposição do agente dono da obra criada.
void exposure_add(AiExposureCache& cache, const std::string& city_id,
	const std::string& owner, int product, double cost);

} // namespace ai_risk
} // namespace ogs
