// Implementação da demanda potencial (G2 §3). Ver potential_demand.h.
#include "potential_demand.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace ogs {
namespace {

// Profundidade máxima da propagação (corte de ciclo declarado).
constexpr int kMaxPotentialDepth = 8;

} // namespace

void build_potential_demand_index(const WorldState& ws,
		PotentialDemandIndex& index) {
	const int products = std::max(0, ws.products);
	index.recipes_by_output.assign(static_cast<std::size_t>(products),
		std::vector<int>());
	for (std::size_t recipe_index = 0; recipe_index < ws.recipes.size();
			++recipe_index) {
		const Recipe& recipe = ws.recipes[recipe_index];
		if (recipe.output < 0 || recipe.output >= products) {
			continue;
		}
		index.recipes_by_output[static_cast<std::size_t>(recipe.output)]
			.push_back(static_cast<int>(recipe_index));
	}
	index.chosen_recipe.assign(static_cast<std::size_t>(products), -1);
	for (int product = 0; product < products; ++product) {
		double best_ratio = -1.0;
		int best = -1;
		// A ordem do JSON (índice crescente) é o desempate declarado.
		for (const int recipe_index :
				index.recipes_by_output[static_cast<std::size_t>(product)]) {
			const Recipe& recipe =
				ws.recipes[static_cast<std::size_t>(recipe_index)];
			if (recipe.yield <= 0.0) {
				continue;
			}
			double ratio = 0.0;
			for (int slot = 0; slot < recipe.input_count; ++slot) {
				ratio += recipe.input_qty[slot] / recipe.yield;
			}
			if (ratio > best_ratio + 1e-12) {
				best_ratio = ratio;
				best = recipe_index;
			}
		}
		index.chosen_recipe[static_cast<std::size_t>(product)] = best;
	}
}

void potential_demand_for_city(const WorldState& ws,
		const PotentialDemandIndex& index, int city_index,
		const AiBrainParams& params, std::vector<double>& out) {
	const int products = std::max(0, ws.products);
	out.assign(static_cast<std::size_t>(products), 0.0);
	if (!ws.needs_loaded || city_index < 0
			|| city_index >= static_cast<int>(ws.cities.size())) {
		return;  // ausência declarada: sem cesta carregada não há potencial
	}
	const City& city = ws.cities[static_cast<std::size_t>(city_index)];
	const double population = std::max(0.0, city.simulation_population);
	if (population <= 0.0) {
		return;
	}
	const double durable_scale = std::clamp(
		ws.durable_decay * 365.0 / 12.0, 0.0, 1.0);

	// 1. Demanda final (âncora) e acumulador. Substitutos dividem a categoria
	// igualmente; duráveis pagam a reposição anual como no consumo.
	std::vector<double> final_anchor(static_cast<std::size_t>(products), 0.0);
	for (const NeedsLevel& level : ws.needs_levels) {
		for (const NeedsCategory& category : level.categories) {
			int slots = 0;
			for (std::size_t slot = 0; slot < category.substitutes.size();
					++slot) {
				const int product = category.substitutes[slot];
				if (product < 0 || product >= products) {
					continue;
				}
				if (slot < category.equivalence.size()
						&& category.equivalence[slot] > 0.0) {
					++slots;
				}
			}
			if (slots <= 0 || category.per_capita_day <= 0.0) {
				continue;
			}
			const double per_slot =
				category.per_capita_day * population
				/ static_cast<double>(slots);
			for (std::size_t slot = 0; slot < category.substitutes.size();
					++slot) {
				const int product = category.substitutes[slot];
				if (product < 0 || product >= products) {
					continue;
				}
				const double factor = slot < category.equivalence.size()
					? category.equivalence[slot] : 0.0;
				if (factor <= 0.0) {
					continue;
				}
				double tons = per_slot / factor;
				const bool durable =
					product < static_cast<int>(ws.product_durable.size())
					&& ws.product_durable[static_cast<std::size_t>(product)]
						!= 0;
				if (durable) {
					tons *= durable_scale;
				}
				final_anchor[static_cast<std::size_t>(product)] += tons;
			}
		}
	}
	// Computador institucional (governo): mesma âncora do consumo.
	for (std::size_t index = 0; index < ws.government_products.size();
			++index) {
		if (index >= ws.government_per_capita_day.size()) {
			break;
		}
		const int product = ws.government_products[index];
		if (product < 0 || product >= products) {
			continue;
		}
		const double tons =
			std::max(0.0, ws.government_per_capita_day[index]) * population;
		final_anchor[static_cast<std::size_t>(product)] += tons;
	}
	out = final_anchor;

	// 2. Propagação para trás, uma aresta por nível (frente de onda). Produto
	// sem receita não recebe potencial.
	const double discount = std::clamp(params.potential_discount, 0.0, 1.0);
	std::vector<double> frontier = final_anchor;
	std::vector<double> next(static_cast<std::size_t>(products), 0.0);
	for (int depth = 0; depth < kMaxPotentialDepth; ++depth) {
		std::fill(next.begin(), next.end(), 0.0);
		bool any = false;
		for (int product = 0; product < products; ++product) {
			const double amount = frontier[static_cast<std::size_t>(product)];
			if (amount <= 0.0) {
				continue;
			}
			const int recipe_index =
				index.chosen_recipe[static_cast<std::size_t>(product)];
			if (recipe_index < 0
					|| recipe_index >= static_cast<int>(ws.recipes.size())) {
				continue;
			}
			const Recipe& recipe =
				ws.recipes[static_cast<std::size_t>(recipe_index)];
			if (recipe.yield <= 0.0) {
				continue;
			}
			for (int slot = 0; slot < recipe.input_count; ++slot) {
				const int input = recipe.inputs[slot];
				if (input < 0 || input >= products) {
					continue;
				}
				// Só produtos com receita recebem potencial (minério/fazenda
				// primária ficam de fora).
				if (index.chosen_recipe[static_cast<std::size_t>(input)] < 0) {
					continue;
				}
				const double qty = recipe.input_qty[slot] / recipe.yield;
				if (qty <= 0.0 || discount <= 0.0) {
					continue;
				}
				next[static_cast<std::size_t>(input)] += amount * qty * discount;
				any = true;
			}
		}
		if (!any) {
			break;
		}
		for (int product = 0; product < products; ++product) {
			out[static_cast<std::size_t>(product)] +=
				next[static_cast<std::size_t>(product)];
		}
		frontier.swap(next);
	}

	// 3. Teto: múltiplo da âncora final da cidade (cap=1,0 não limita quem não
	// tem demanda final própria, ex.: chips/microchips).
	if (params.potential_cap > 0.0) {
		for (int product = 0; product < products; ++product) {
			const double anchor =
				final_anchor[static_cast<std::size_t>(product)];
			if (anchor > 0.0) {
				out[static_cast<std::size_t>(product)] = std::min(
					out[static_cast<std::size_t>(product)],
					params.potential_cap * anchor);
			}
		}
	}
}

void potential_demand_world(const WorldState& ws,
		const PotentialDemandIndex& index, const AiBrainParams& params,
		std::vector<double>& out) {
	const int products = std::max(0, ws.products);
	out.assign(static_cast<std::size_t>(products), 0.0);
	std::vector<double> city_potential;
	for (int city_index = 0;
			city_index < static_cast<int>(ws.cities.size()); ++city_index) {
		potential_demand_for_city(ws, index, city_index, params, city_potential);
		for (int product = 0; product < products; ++product) {
			out[static_cast<std::size_t>(product)] +=
				city_potential[static_cast<std::size_t>(product)];
		}
	}
}

} // namespace ogs
