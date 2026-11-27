// R1b - Tesouro Nacional + envelopes mensais. C++ puro (STL), sem godot_cpp em
// sim/. Ver o contrato em treasury.h e a doc em docs/economy/rework/R1.md.
#include "treasury.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <unordered_map>
#include <vector>

namespace ogs {
namespace treasury {
namespace {

inline double safe_nonneg(double value) {
	return std::isfinite(value) ? std::max(0.0, value) : 0.0;
}

inline double safe_unit(double value) {
	return std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 0.0;
}

} // namespace

bool enabled(const WorldState& ws) {
	return ws.national_treasury && ws.treasury_config_error.empty();
}

void collect(WorldState& ws, const std::string& nation, double amount) {
	const double value = safe_nonneg(amount);
	if (value <= 0.0) {
		return;
	}
	ws.nation_treasury[nation] += value;
	ws.treasury_collected_total += value;
}

double balance(const WorldState& ws, const std::string& nation) {
	const auto found = ws.nation_treasury.find(nation);
	return found == ws.nation_treasury.end() ? 0.0 : found->second;
}

double total_balance(const WorldState& ws) {
	double total = 0.0;
	for (const auto& row : ws.nation_treasury) {
		total += row.second;
	}
	return total;
}

double envelope_weight(double pop_share, int city_count, double floor_fraction) {
	if (city_count <= 0) {
		return 0.0;
	}
	const double share = safe_unit(pop_share);
	const double floor = safe_unit(floor_fraction)
		/ static_cast<double>(city_count);
	return std::max(share, floor);
}

double distribute_envelopes(WorldState& ws) {
	if (!enabled(ws)) {
		return 0.0;
	}
	const double share = safe_unit(ws.treasury_envelope_share);
	if (share <= 0.0) {
		return 0.0;
	}
	// Ordem de primeira aparicao das nacoes (deterministica, como o status).
	std::vector<std::string> order;
	std::unordered_map<std::string, std::vector<std::size_t>> by_nation;
	by_nation.reserve(ws.cities.size() / 4 + 8);
	for (std::size_t index = 0; index < ws.cities.size(); ++index) {
		const std::string& nation = ws.cities[index].nation;
		auto found = by_nation.find(nation);
		if (found == by_nation.end()) {
			order.push_back(nation);
			found = by_nation.emplace(nation, std::vector<std::size_t>{}).first;
		}
		found->second.push_back(index);
	}
	double distributed_total = 0.0;
	for (const std::string& nation : order) {
		auto found = ws.nation_treasury.find(nation);
		if (found == ws.nation_treasury.end()) {
			continue;
		}
		const double available = safe_nonneg(found->second) * share;
		if (available <= 0.0) {
			continue;
		}
		const std::vector<std::size_t>& cities = by_nation.at(nation);
		double total_population = 0.0;
		for (const std::size_t index : cities) {
			total_population += safe_nonneg(
				ws.cities[index].simulation_population);
		}
		// Sem populacao (save editado/cenario sintetico): rateio igualitario.
		std::vector<double> weights(cities.size(), 0.0);
		double weight_sum = 0.0;
		for (std::size_t slot = 0; slot < cities.size(); ++slot) {
			const double pop_share = total_population > 0.0
				? safe_nonneg(ws.cities[cities[slot]].simulation_population)
					/ total_population
				: 1.0 / static_cast<double>(cities.size());
			weights[slot] = envelope_weight(
				pop_share, static_cast<int>(cities.size()),
				ws.treasury_floor_fraction);
			weight_sum += weights[slot];
		}
		if (weight_sum <= 0.0) {
			continue;
		}
		// Ultima regiao leva o resto EXATO: a soma dos envelopes e igual ao
		// valor debitado do Tesouro (conservacao, sem sobra de arredondamento).
		double remaining = available;
		for (std::size_t slot = 0; slot < cities.size(); ++slot) {
			double amount = slot + 1 == cities.size()
				? remaining
				: available * weights[slot] / weight_sum;
			amount = std::clamp(amount, 0.0, remaining);
			remaining = std::max(0.0, remaining - amount);
			if (amount <= 0.0) {
				continue;
			}
			const std::size_t city_index = cities[slot];
			City& city = ws.cities[city_index];
			const double works = safe_unit(ws.treasury_envelope_works_share)
				* amount;
			const double services = amount - works;
			city.government_budget += services;
			if (city_index < ws.city_gov.size()) {
				ws.city_gov[city_index].cash += works;
			} else {
				// Sem camada urbana (save antigo/cenario minimo) nao ha conta de
				// obras: o envelope inteiro fica no orcamento publico.
				city.government_budget += works;
			}
			city.envelope_received_month += amount;
			city.envelope_received_total += amount;
			distributed_total += amount;
		}
		found->second = std::max(0.0, found->second - available);
	}
	ws.treasury_distributed_total += distributed_total;
	return distributed_total;
}

void drain_to_regions(WorldState& ws) {
	for (auto& row : ws.nation_treasury) {
		if (row.second <= 0.0) {
			row.second = 0.0;
			continue;
		}
		for (City& city : ws.cities) {
			if (city.nation == row.first) {
				city.government_budget += row.second;
				row.second = 0.0;
				break;
			}
		}
	}
}

} // namespace treasury
} // namespace ogs
