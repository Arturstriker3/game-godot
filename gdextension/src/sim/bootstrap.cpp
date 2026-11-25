// Implementação do planejamento da semente industrial (G2 §2).
#include "bootstrap.h"

#include "potential_demand.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <string>

namespace ogs {
namespace bootstrap {
namespace {

int find_product(const WorldState& ws, const std::string& id) {
	for (std::size_t index = 0; index < ws.product_names.size(); ++index) {
		if (ws.product_names[index] == id) {
			return static_cast<int>(index);
		}
	}
	return -1;
}

bool has_local_input(const City& city, int product) {
	if (product < 0 || product >= kMaxProducts) {
		return false;
	}
	if (city.suitability[product] >= 0.30) {
		return true;
	}
	return city.deposits[product].remaining_kt > 0.0;
}

// Variante da receita para a cidade: a primeira cujo insumo tem aptidão ou
// jazida local >=0,30; sem alternativa local, a primeira na ordem do JSON.
int choose_recipe(const WorldState& ws, const City& city, int product) {
	int first = -1;
	for (std::size_t index = 0; index < ws.recipes.size(); ++index) {
		const Recipe& recipe = ws.recipes[index];
		if (recipe.output != product) {
			continue;
		}
		if (first < 0) {
			first = static_cast<int>(index);
		}
		for (int slot = 0; slot < recipe.input_count; ++slot) {
			if (has_local_input(city, recipe.inputs[slot])) {
				return static_cast<int>(index);
			}
		}
	}
	return first;
}

double city_score(const WorldState& ws, const City& city, int product,
		const Recipe& recipe, double capacity) {
	(void)product;
	double hits = 0.0;
	for (const std::string& id : ws.bootstrap.score_inputs) {
		const int input = find_product(ws, id);
		if (input >= 0 && has_local_input(city, input)) {
			hits += 1.0;
		}
	}
	const double coverage = ws.bootstrap.score_inputs.empty()
		? 0.0 : hits / static_cast<double>(ws.bootstrap.score_inputs.size());
	const double jobs = std::max(0.0, capacity * recipe.jobs_per_ton);
	double schooling = 1.0;
	if (jobs > 1e-9) {
		const double mid = std::min(1.0,
			city.pop.education[1] / (0.40 * jobs));
		const double high = std::min(1.0,
			city.pop.education[2] / (0.38 * jobs));
		schooling = mid * high;
	}
	return coverage + schooling
		+ 0.01 * std::log10(1.0 + std::max(0.0, city.government_budget));
}

} // namespace

Plan plan(const WorldState& ws) {
	Plan out;
	const BootstrapIndustry& cfg = ws.bootstrap;
	if (!cfg.enabled || cfg.goods.empty()) {
		return out;
	}
	PotentialDemandIndex index;
	build_potential_demand_index(ws, index);
	std::vector<double> anchor;
	potential_demand_world(ws, index, ws.ai_brain, anchor);
	const int products = std::max(0, ws.products);

	// Cidades por nação (std::map = ordem determinística).
	std::map<std::string, std::vector<int>> by_nation;
	for (int city_index = 0;
			city_index < static_cast<int>(ws.cities.size()); ++city_index) {
		by_nation[ws.cities[static_cast<std::size_t>(city_index)].nation]
			.push_back(city_index);
	}
	// Orçamento restante por cidade (teto `budget_share` do municipal).
	std::map<int, double> budget_left;
	const auto budget_of = [&ws, &cfg, &budget_left](int city_index) {
		auto found = budget_left.find(city_index);
		if (found != budget_left.end()) {
			return found->second;
		}
		const double amount = std::max(0.0,
			ws.cities[static_cast<std::size_t>(city_index)].government_budget
			* cfg.budget_share);
		budget_left[city_index] = amount;
		return amount;
	};

	for (const BootstrapGood& good : cfg.goods) {
		const int product = find_product(ws, good.id);
		if (product < 0 || product >= products) {
			BootstrapPlanRow row;
			row.good = good.id;
			row.reason = "no_recipe";
			out.rows.push_back(row);
			continue;
		}
		const double world_anchor = anchor[static_cast<std::size_t>(product)];
		if (world_anchor <= 0.0) {
			continue;  // ausência honesta: sem âncora não há semente
		}
		const double seed_world = world_anchor * cfg.seed_fraction;
		// Nações por participação desc e ISO3 asc (§2.7).
		std::vector<BootstrapProducer> producers = good.producers;
		std::sort(producers.begin(), producers.end(),
			[](const BootstrapProducer& a, const BootstrapProducer& b) {
				if (a.share != b.share) {
					return a.share > b.share;
				}
				return a.nation < b.nation;
			});
		for (const BootstrapProducer& producer : producers) {
			if (producer.nation == "outros") {
				out.unseeded_share += producer.share;
				continue;
			}
			const auto nation_it = by_nation.find(producer.nation);
			if (nation_it == by_nation.end() || nation_it->second.empty()) {
				out.unseeded_share += producer.share;
				BootstrapPlanRow row;
				row.good = good.id;
				row.nation = producer.nation;
				row.reason = "no_city";
				out.rows.push_back(row);
				continue;
			}
			const double target = producer.share * seed_world;
			if (target < cfg.capacity_min) {
				out.unseeded_share += producer.share;
				BootstrapPlanRow row;
				row.good = good.id;
				row.nation = producer.nation;
				row.reason = "capacity_min";
				out.rows.push_back(row);
				continue;
			}
			// Filtro de cidade: 1ª cidade >=5 M; sem ela, a maior >=1 M.
			std::vector<int> candidates;
			for (const int city_index : nation_it->second) {
				if (ws.cities[static_cast<std::size_t>(city_index)]
						.simulation_population >= 5000000.0) {
					candidates.push_back(city_index);
				}
			}
			if (candidates.empty()) {
				int largest = -1;
				for (const int city_index : nation_it->second) {
					const City& city =
						ws.cities[static_cast<std::size_t>(city_index)];
					if (city.simulation_population < 1000000.0) {
						continue;
					}
					if (largest < 0
							|| city.simulation_population
								> ws.cities[static_cast<std::size_t>(largest)]
									.simulation_population) {
						largest = city_index;
					}
				}
				if (largest < 0) {
					out.unseeded_share += producer.share;
					BootstrapPlanRow row;
					row.good = good.id;
					row.nation = producer.nation;
					row.reason = "no_city";
					out.rows.push_back(row);
					continue;
				}
				candidates.push_back(largest);
			}
			// Ordem de escolha: score desc, nação asc, city.id asc.
			std::sort(candidates.begin(), candidates.end(),
				[&ws, &cfg, product, target](int left, int right) {
					const City& a =
						ws.cities[static_cast<std::size_t>(left)];
					const City& b =
						ws.cities[static_cast<std::size_t>(right)];
					const Recipe& ra = ws.recipes[
						static_cast<std::size_t>(
							choose_recipe(ws, a, product))];
					const Recipe& rb = ws.recipes[
						static_cast<std::size_t>(
							choose_recipe(ws, b, product))];
					const double sa = city_score(ws, a, product, ra,
						std::max(cfg.capacity_min, target));
					const double sb = city_score(ws, b, product, rb,
						std::max(cfg.capacity_min, target));
					if (sa != sb) {
						return sa > sb;
					}
					if (a.nation != b.nation) {
						return a.nation < b.nation;
					}
					return a.id < b.id;
				});
			// Teto por planta: divide em blocos de `capacity_max`.
			const int chunks = std::max(1,
				static_cast<int>(std::ceil(target / cfg.capacity_max)));
			const double chunk_capacity = target / static_cast<double>(chunks);
			const int max_cities = std::max(cfg.max_cities_per_nation, chunks);
			int used_cities = 0;
			double remaining = target;
			for (const int city_index : candidates) {
				if (remaining <= 1e-12) {
					break;
				}
				if (used_cities >= max_cities) {
					break;
				}
				const City& city =
					ws.cities[static_cast<std::size_t>(city_index)];
				const int recipe_index = choose_recipe(ws, city, product);
				if (recipe_index < 0) {
					continue;
				}
				const Recipe& recipe =
					ws.recipes[static_cast<std::size_t>(recipe_index)];
				const double capacity = std::min(chunk_capacity,
					std::min(cfg.capacity_max, remaining));
				const double cost = capacity * recipe.capital_per_capacity;
				BootstrapPlanRow row;
				row.good = good.id;
				row.nation = producer.nation;
				row.city = city.id;
				row.capacity = capacity;
				row.cost = cost;
				row.recipe = recipe_index;
				if (cost > budget_of(city_index) + 1e-6) {
					row.reason = "budget";
					out.rows.push_back(row);
					continue;
				}
				budget_left[city_index] -= cost;
				row.seeded = true;
				out.rows.push_back(row);
				++out.seeded;
				++used_cities;
				remaining = std::max(0.0, remaining - capacity);
			}
			if (remaining > 1e-12) {
				out.shortfall += remaining;
				BootstrapPlanRow row;
				row.good = good.id;
				row.nation = producer.nation;
				row.reason = "budget";
				out.rows.push_back(row);
			}
		}
	}
	return out;
}

} // namespace bootstrap
} // namespace ogs
