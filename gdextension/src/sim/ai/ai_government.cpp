// Implementação de ai_government.h (A5): porte fiel do governo da economia
// (`core/economy/ai/government_ai.gd`, 350 linhas) sobre o retrato do dia.
//
// Regra de ouro (a mesma de ai_private.cpp): cada função é a TRANSCRIÇÃO de uma
// função do GDScript — mesma ordem de leitura, mesmas condições, mesmas
// constantes e mesmas operações em ponto flutuante. Não introduza critério novo
// nem reordene os testes: o aceite é a equivalência com o GDScript.
//
// Os únicos desvios são de CONTEXTO:
//   * os dados do dia vêm da `AiView` congelada (ai_view.h) em vez do
//     dicionário `status`/`light`: o `run_city` do GDScript rebinda o `light` na
//     cidade, aqui a view já é por cidade;
//   * `capital_cost_per_capacity` do status é a constante do núcleo
//     (`kCapitalPerCapacity` = 2000), como no investidor privado;
//   * o teto de terra vem de `City::land_capacity` (o mesmo valor que o
//     `GeographyService.city_land_capacity_t_day` declara ao núcleo) e
//     `has_any_deposit` é `ai_d_begin < ai_d_end` (== `d_offset`);
//   * o contador de id e o dia da última obra vivem em `AiState`
//     (`government_id`, `last_build_day`), nunca em membros deste struct;
//   * nenhum log de decisão: `_log("avoid"/"rejected"/"build_state")` não tem
//     efeito no mundo nem entra no save. O bloco 4 do GDScript (diagnóstico de
//     desemprego) existe SÓ para esse log e por isso não foi portado — a
//     constante fica declarada para documentar o que falta.
//
// Ordem das ações por cidade (== `_run_city`): segurança alimentar e, se ela
// não construiu, mineração estratégica por margem decrescente. Uma ação por
// cidade.

#include "ai_government.h"

#include "ai_phase.h"
#include "ai_view.h"
#include "../sim_constants.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace ogs {
namespace {

// Constantes de `government_ai.gd` (mesmos valores, mesmo significado).
constexpr double kBudgetShare = 0.25;
constexpr double kMinShortageTons = 1.0;
constexpr double kMinStateCapacity = 2.0;
constexpr double kMaxStateCapacity = 20.0;
// Teto da capacidade de uma mina estatal (B4): o estado cobre uma base da
// demanda externa e o capital privado o resto (× kExportCapacityFactor).
constexpr double kMaxStateMineCapacity = 10.0;
constexpr double kExportCapacityFactor = 1.2;
// Só alimentava o log de diagnóstico do bloco 4 (não portado).
constexpr double kHighUnemployment = 0.65;
// Anti-loop de construção (B5): cooldown por cidade, teto por tipo e piso de
// utilização média efetiva.
constexpr double kMinAvgUtilization = 0.60;
constexpr int kMaxBuildingsPerType = 6;
constexpr int kBuildCooldownDays = 30;

// Maiúsculas ASCII (== `String.to_upper()` para os ids de bem, que são ASCII)
// usadas no id `STATE-<RECURSO>-<n>`.
std::string ascii_upper(const std::string& text) {
	std::string out = text;
	for (char& letter : out) {
		if (letter >= 'a' && letter <= 'z') {
			letter = static_cast<char>(letter - 'a' + 'A');
		}
	}
	return out;
}

// Bloqueio anti-loop (== `_build_gate`): cooldown por cidade, teto por tipo e
// utilização efetiva. `true` = pode construir. O GDScript devolve o motivo
// (usado só no log `avoid`); aqui o motivo não tem leitor.
bool build_gate(AiContext& ctx, const std::string& city_id, int city_index,
		int product, const AiBuildingIndex& index) {
	const auto found = ctx.state.last_build_day.find(city_id);
	// Ausência = última obra em `-kBuildCooldownDays` (== `.get(id, -30)`).
	const int last_build = found == ctx.state.last_build_day.end()
		? -kBuildCooldownDays
		: found->second;
	if (ctx.day - last_build < kBuildCooldownDays) {
		return false;
	}
	if (ai_buildings_of_type(ctx.view, city_index, product, index)
			>= kMaxBuildingsPerType) {
		return false;
	}
	return ai_avg_utilization(ctx.view, city_index, product, index)
		>= kMinAvgUtilization;
}

// true se a cidade já tem QUALQUER produtor de alimento da cesta — construção
// (== `Templates.has_producer`) ou obra NOVA em voo
// (== `Templates.has_pending_build`, §15/D-060: obra de comida em andamento já
// é produtor futuro). O governo só cobre onde o capital privado não chega.
bool has_food_producer(const AiContext& ctx, int city_index) {
	const AiView& view = ctx.view;
	for (const int index : ctx.templates.food) {
		const AiTemplate& tmpl =
			ctx.templates.all[static_cast<std::size_t>(index)];
		const int product = tmpl.product;
		if (ai_local_has(view, city_index, product)) {
			return true;
		}
		if (ai_has_pending_build(view, city_index, product)) {
			return true;
		}
	}
	return false;
}

// Cultura escolhida por `best_food_crop` (== o dicionário `best` do GDScript):
// índice em `AiTemplates::all` e a pontuação aptidão × preço.
struct FoodCrop {
	int index = -1;
	double score = 0.0;
};

// Cultura alimentar com melhor aptidão × preço na cidade (== `_best_food_crop`).
// `false` quando nenhuma cultura é viável (aptidão <= 0 ou margem <= 0).
// Primeira vence o empate (comparação estrita, como o `score > best_score`).
bool best_food_crop(const AiContext& ctx, int city_index, FoodCrop& out) {
	const AiView& view = ctx.view;
	const AiCityRow& city =
		view.city_rows[static_cast<std::size_t>(city_index)];
	const std::vector<double>& suitability = city.suitability;
	double best_score = -1.0;
	for (const int index : ctx.templates.food) {
		const AiTemplate& tmpl =
			ctx.templates.all[static_cast<std::size_t>(index)];
		const int product = tmpl.product;
		if (product < 0 || product >= static_cast<int>(suitability.size())
				|| suitability[static_cast<std::size_t>(product)] <= 0.0) {
			continue;
		}
		const double price = ai_market(view, city_index, product, kAiPrice);
		// Viabilidade: uma fazenda cujo preço × aptidão não paga o próprio custo
		// variável faliria em 60 dias e não alimenta ninguém.
		if (suitability[static_cast<std::size_t>(product)] * price
				- ai_variable_cost(view, ctx.templates, city_index, tmpl) <= 0.0) {
			continue;
		}
		// Prefere expandir a cultura já estabelecida (fix do churn de milho, B4).
		const bool established = ai_local_has(view, city_index, product);
		const double score = suitability[static_cast<std::size_t>(product)]
			* price * (established ? 2.0 : 1.0);
		if (out.index < 0 || score > best_score) {
			best_score = score;
			out.index = index;
			out.score = score;
		}
	}
	return out.index >= 0;
}

// Candidato de mineração: índice em `AiTemplates::all` e a margem ao preço de
// exportação (com a profundidade da jazida em `variable_cost_mined`).
struct MineCandidate {
	int index = -1;
	double margin = 0.0;
};

// Plano de UMA cidade (== `_run_city`), mesma ordem e mesmas constantes.
void plan_city(AiContext& ctx, int city_index) {
	const AiView& view = ctx.view;
	const AiCityRow& city = view.city_rows[static_cast<std::size_t>(city_index)];
	const std::string city_id = city.id;
	const double budget = city.government_budget;
	const double capital_cost = kCapitalPerCapacity;
	const double available = budget * kBudgetShare;
	if (available <= 0.0) {
		return;
	}

	// Índice O(1) das construções locais por produto (== `building_index`): UMA
	// varredura por cidade, embora o laço de minerais o consulte por produto.
	const AiBuildingIndex bindex = ai_build_index(view, city_index);

	// 1. Segurança alimentar (B3): fome REAL (necessidade − consumo, em t-eq de
	// grão) e nenhum produtor de alimento na cidade. O consumo (não a produção
	// local) é o gatilho: cidade atendida por comércio regional não constrói
	// fazenda redundante.
	const double food_deficit = std::max(0.0,
		ai_pop(view, city_index, kAiNeed) - ai_pop(view, city_index, kAiConsumed));
	if (food_deficit >= kMinShortageTons
			&& !has_food_producer(ctx, city_index)) {
		FoodCrop best;
		if (best_food_crop(ctx, city_index, best)) {
			const AiTemplate& tmpl =
				ctx.templates.all[static_cast<std::size_t>(best.index)];
			const std::string& resource = tmpl.resource;
			const int product = tmpl.product;
			// Fator calórico > 0 para toda a cesta: a divisão nunca é por zero.
			double capacity = std::clamp(food_deficit / tmpl.caloric_factor,
				kMinStateCapacity, kMaxStateCapacity);
			// Terra limitada: teto pela área cultivável menos o já instalado
			// (mesma regra do investidor privado). Só quando há dado de área
			// (land_cap > 0): cidade sem bacia não tem teto de terra.
			const double land_cap = ai_land_capacity(view, city_index, product);
			if (land_cap > 0.0) {
				const double used = ai_existing_capacity(view, city_index, product);
				capacity = std::min(capacity, std::max(0.0, land_cap - used));
			}
			if (capacity * capital_cost >= available) {
				capacity = available / capital_cost;
			}
			if (capacity >= kMinStateCapacity) {
				if (build_gate(ctx, city_id, city_index, product, bindex)) {
					const double cost = capacity * capital_cost;
					const std::string target = "STATE-" + ascii_upper(resource)
						+ "-" + std::to_string(ctx.state.government_id);
					const AiActionResult result = ai_emit_build(ctx, city_id,
						target, resource, tmpl.input, product, capacity,
						capacity * tmpl.jobs_per_ton, tmpl.wage,
						capacity * tmpl.opex_per_ton, tmpl.input_ratio, "state",
						cost, std::string());
					if (result.error.empty()) {
						// Id e cooldown só avançam no SUCESSO (como o GDScript).
						ctx.state.government_id += 1;
						ctx.state.last_build_day[city_id] = ctx.day;
						return;
					}
				}
				// Recusa (rejeição do núcleo ou gate): segue para a mineração.
			}
		}
	}

	// 2. Mineração estratégica (B4): mina estatal para cada mineral com jazida
	// (aptidão > 0) e margem positiva ao preço de exportação, quando ainda não
	// há produtor. Sem jazida declarada a cidade não pode abrir mina nenhuma:
	// pula o laço inteiro (== `has_any_deposit`).
	std::vector<MineCandidate> candidates;
	if (ai_d_begin(view, city_index) < ai_d_end(view, city_index)) {
		for (const int index : ctx.templates.minerals) {
			const AiTemplate& tmpl =
				ctx.templates.all[static_cast<std::size_t>(index)];
			const int product = tmpl.product;
			if (product < 0
					|| product >= static_cast<int>(city.suitability.size())
					|| city.suitability[static_cast<std::size_t>(product)]
						<= 0.0) {
				continue;
			}
			if (!ai_has_deposit(view, city_index, product)) {
				continue;
			}
			if (ai_buildings_of_type(view, city_index, product, bindex) > 0) {
				continue;
			}
			// §15/D-060: obra de mina em andamento conta como produtor futuro.
			if (ai_has_pending_build(view, city_index, product)) {
				continue;
			}
			const double margin =
				ai_market(view, city_index, product, kAiPrice)
				- ai_variable_cost_mined(view, ctx.templates, city_index, tmpl);
			if (margin <= 0.0
					|| ai_market(view, city_index, product, kAiSurplus) > 0.0) {
				continue;
			}
			MineCandidate candidate;
			candidate.index = index;
			candidate.margin = margin;
			candidates.push_back(candidate);
		}
	}
	// Ranking por margem decrescente: `std::sort` com o MESMO comparador do
	// `sort_custom` (não-estável, como o do Godot; o desempate é irrelevante
	// porque as margens são distintas nos dados).
	std::sort(candidates.begin(), candidates.end(),
		[](const MineCandidate& a, const MineCandidate& b) {
			return a.margin > b.margin;
		});
	for (const MineCandidate& candidate : candidates) {
		const AiTemplate& tmpl =
			ctx.templates.all[static_cast<std::size_t>(candidate.index)];
		const std::string& resource = tmpl.resource;
		const int product = tmpl.product;
		if (!build_gate(ctx, city_id, city_index, product, bindex)) {
			continue;
		}
		// Capacidade dimensionada pela demanda externa (× 1,2, teto de mina
		// estatal): mina de demanda pequena não nasce com 10 t/d.
		const double external_demand =
			product >= 0
				&& product < static_cast<int>(city.external_demand.size())
			? city.external_demand[static_cast<std::size_t>(product)]
			: 0.0;
		double capacity = std::min(kMaxStateMineCapacity,
			std::max(kMinStateCapacity, external_demand * kExportCapacityFactor));
		if (capacity * capital_cost >= available) {
			capacity = available / capital_cost;
		}
		if (capacity < kMinStateCapacity) {
			continue;
		}
		const double cost = capacity * capital_cost;
		const std::string target = "STATE-" + ascii_upper(resource) + "-"
			+ std::to_string(ctx.state.government_id);
		// Extração primária: input -1 e insumo/produto 0.0 (literal do GDScript).
		const AiActionResult result = ai_emit_build(ctx, city_id, target,
			resource, -1, product, capacity, capacity * tmpl.jobs_per_ton,
			tmpl.wage, capacity * tmpl.opex_per_ton, 0.0, "state", cost,
			std::string());
		if (result.error.empty()) {
			ctx.state.government_id += 1;
			ctx.state.last_build_day[city_id] = ctx.day;
			return;
		}
		// Rejeitado: tenta o próximo candidato (ordem de margem).
	}
}

} // namespace

void AiGovernment::run_city(AiContext& ctx, int city_index) {
	if (city_index < 0 || city_index >= ctx.view.cities) {
		return;
	}
	// O GDScript rebinda o `light` na cidade aqui; a view já é por cidade.
	plan_city(ctx, city_index);
}

} // namespace ogs
