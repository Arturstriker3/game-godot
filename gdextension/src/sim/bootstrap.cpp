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

bool has_recipe_for(const WorldState& ws, int product) {
	for (const Recipe& recipe : ws.recipes) {
		if (recipe.output == product) {
			return true;
		}
	}
	return false;
}

// Variante da receita para a cidade: a primeira cujo insumo tem aptidão ou
// jazida local >=0,30; sem alternativa local, a primeira na ordem do JSON.
// Devolve -1 quando o produto não tem receita (chamador trata como ausência).
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

// G3 (L2): consumo de `product` pelos planos já semeados de `source_good`, na
// receita escolhida para cada planta (capacidade × qty / yield). É a
// necessidade do lote — não a âncora mundial do bem-insumo.
double dependent_input_need(const WorldState& ws,
		const std::vector<BootstrapPlanRow>& rows,
		const std::string& source_good, int product) {
	double need = 0.0;
	for (const BootstrapPlanRow& row : rows) {
		if (!row.seeded || row.good != source_good) {
			continue;
		}
		if (row.recipe < 0
				|| row.recipe >= static_cast<int>(ws.recipes.size())) {
			continue;
		}
		const Recipe& recipe = ws.recipes[static_cast<std::size_t>(row.recipe)];
		if (recipe.yield <= 0.0) {
			continue;
		}
		for (int slot = 0; slot < recipe.input_count; ++slot) {
			if (recipe.inputs[slot] == product) {
				need += row.capacity * recipe.input_qty[slot] / recipe.yield;
			}
		}
	}
	return need;
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
		if (!has_recipe_for(ws, product)) {
			// Sem receita não há planta: declara (não omite nem indexa -1).
			BootstrapPlanRow row;
			row.good = good.id;
			row.reason = "no_recipe";
			out.rows.push_back(row);
			continue;
		}
		// G3 (L2): insumo do lote — o alvo é o consumo do bem pelos planos já
		// semeados do bem-fonte. Esse insumo não tem âncora de demanda final
		// própria, então o caminho `âncora × seed_fraction` não se aplica.
		const bool input_tied = !good.capacity_from.empty();
		double seed_world = 0.0;
		if (input_tied) {
			seed_world = dependent_input_need(ws, out.rows,
				good.capacity_from, product);
			if (seed_world <= 0.0) {
				// Ausência honesta: sem plano do bem-fonte não há consumidor.
				BootstrapPlanRow row;
				row.good = good.id;
				row.reason = "no_input_need";
				out.rows.push_back(row);
				continue;
			}
		} else {
			const double world_anchor = anchor[static_cast<std::size_t>(product)];
			if (world_anchor <= 0.0) {
				// Ausência honesta: sem âncora não há semente, mas fica
				// declarado (aviso) em vez de omitir o bem em silêncio.
				BootstrapPlanRow row;
				row.good = good.id;
				row.reason = "no_anchor";
				out.rows.push_back(row);
				continue;
			}
			// G3: o bem pode declarar a própria fração (lotes L1–L5). Ausente
			// (0) herda o global — o G2 continua idêntico.
			const double fraction = good.seed_fraction > 0.0
				? good.seed_fraction : cfg.seed_fraction;
			seed_world = world_anchor * fraction;
		}
		// Nações por participação desc e ISO3 asc (§2.7).
		std::vector<BootstrapProducer> producers = good.producers;
		std::sort(producers.begin(), producers.end(),
			[](const BootstrapProducer& a, const BootstrapProducer& b) {
				if (a.share != b.share) {
					return a.share > b.share;
				}
				return a.nation < b.nation;
			});
		// Insumo do lote: as participações só DISTRIBUEM a necessidade e são
		// normalizadas sem o residual "outros" (o insumo precisa ser coberto;
		// deixar 20% em "outros" deixaria o lote sem embalagem).
		double share_total = 0.0;
		if (input_tied) {
			for (const BootstrapProducer& producer : producers) {
				if (producer.nation != "outros") {
					share_total += producer.share;
				}
			}
			if (share_total <= 0.0) {
				BootstrapPlanRow row;
				row.good = good.id;
				row.reason = "no_producer";
				out.rows.push_back(row);
				continue;
			}
		}
		for (const BootstrapProducer& producer : producers) {
			if (producer.nation == "outros") {
				if (!input_tied) {
					out.unseeded_share += producer.share;
				}
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
			const double target = producer.share
				* (input_tied ? seed_world / share_total : seed_world);
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
					const int ra_index = choose_recipe(ws, a, product);
					const int rb_index = choose_recipe(ws, b, product);
					// Guarda: produto sem receita não é candidata (índice -1
					// não pode entrar em ws.recipes); sem receita = pior score.
					const double sa = ra_index >= 0
						? city_score(ws, a, product,
							ws.recipes[static_cast<std::size_t>(ra_index)],
							std::max(cfg.capacity_min, target))
						: -1e300;
					const double sb = rb_index >= 0
						? city_score(ws, b, product,
							ws.recipes[static_cast<std::size_t>(rb_index)],
							std::max(cfg.capacity_min, target))
						: -1e300;
					if (sa != sb) {
						return sa > sb;
					}
					if (a.nation != b.nation) {
						return a.nation < b.nation;
					}
					return a.id < b.id;
				});
			// G3 (correção 2): bem de massa pode declarar teto proporcional à
			// cidade (`max_plant_capacity_per_100k`) em vez do teto fixo de
			// `capacity_max`. Aí o alvo do país é dividido pelo número de
			// cidades-alvo (as elegíveis, sem criar blocos artificiais de
			// 20 t/dia) e cada planta é limitada só por X t/dia por 100 mil
			// habitantes simulados. `max_plant_capacity_per_100k == 0` mantém o
			// caminho do G2 bit a bit (blocos de `capacity_max`).
			const bool proportional = good.max_plant_capacity_per_100k > 0.0;
			double chunk_capacity = 0.0;
			int max_cities = 0;
			if (proportional) {
				// Não divide em migalhas: cada cidade-alvo recebe ao menos
				// `capacity_min` (o alvo pequeno usa menos cidades).
				int target_cities = static_cast<int>(candidates.size());
				if (cfg.capacity_min > 0.0) {
					target_cities = std::min(target_cities, std::max(1,
						static_cast<int>(target / cfg.capacity_min)));
				}
				max_cities = std::max(1, target_cities);
				chunk_capacity = target / static_cast<double>(max_cities);
			} else {
				const int chunks = std::max(1,
					static_cast<int>(std::ceil(target / cfg.capacity_max)));
				chunk_capacity = target / static_cast<double>(chunks);
				max_cities = std::max(cfg.max_cities_per_nation, chunks);
			}
			// Alvo coberto por N plantas de target/N pode deixar um resíduo de
			// ponto flutuante; o limiar acompanha o tamanho do alvo.
			const double remaining_eps =
				proportional ? 1e-9 * std::max(1.0, target) : 1e-12;
			int used_cities = 0;
			double remaining = target;
			for (const int city_index : candidates) {
				if (remaining <= remaining_eps) {
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
				double capacity = std::min(chunk_capacity,
					std::min(cfg.capacity_max, remaining));
				if (proportional) {
					const double city_ceiling =
						good.max_plant_capacity_per_100k
						* city.simulation_population / 100000.0;
					const double planned = std::min(chunk_capacity, city_ceiling);
					if (planned < cfg.capacity_min) {
						continue;  // cidade pequena demais para planta mínima
					}
					capacity = std::min(remaining, planned);
				}
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
			if (remaining > remaining_eps) {
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
