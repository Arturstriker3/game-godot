// R1b - Tesouro Nacional + orcamento regional. C++ puro (STL), sem godot_cpp
// em sim/. Ver o contrato em treasury.h e a doc em docs/economy/rework/R1.md.
#include "treasury.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
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

double budget_weight(double pop_share, int city_count, double floor_fraction) {
	if (city_count <= 0) {
		return 0.0;
	}
	const double share = safe_unit(pop_share);
	const double floor = safe_unit(floor_fraction)
		/ static_cast<double>(city_count);
	return std::max(share, floor);
}

double start_month(WorldState& ws) {
	if (!enabled(ws)) {
		return 0.0;
	}
	const double share = safe_unit(ws.regional_budget_share);
	if (share <= 0.0) {
		// Sem dotacao: os contadores ficam zerados (nada estacionado).
		clear_counters(ws);
		ws.regional_budget_started = true;
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
	double limit_total = 0.0;
	for (const std::string& nation : order) {
		const double available =
			safe_nonneg(balance(ws, nation)) * share;
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
			weights[slot] = budget_weight(
				pop_share, static_cast<int>(cities.size()),
				ws.regional_budget_floor_fraction);
			weight_sum += weights[slot];
		}
		// Ultima regiao leva o resto EXATO: a soma das dotacoes e igual ao
		// valor disponivel do Tesouro (sem sobra de arredondamento).
		double remaining = available;
		for (std::size_t slot = 0; slot < cities.size(); ++slot) {
			double amount = slot + 1 == cities.size()
				? remaining
				: available * weights[slot] / (weight_sum > 0.0
					? weight_sum : 1.0);
			amount = std::clamp(amount, 0.0, remaining);
			remaining = std::max(0.0, remaining - amount);
			City& city = ws.cities[cities[slot]];
			const double works = safe_unit(ws.regional_budget_works_share)
				* amount;
			const double services = amount - works;
			city.regional_budget_limit_month = amount;
			city.regional_budget_spent_month = 0.0;
			city.regional_budget_deferred_month = 0.0;
			city.government_budget = services;
			if (cities[slot] < ws.city_gov.size()) {
				ws.city_gov[cities[slot]].cash = works;
			} else {
				// Sem camada urbana (save antigo/cenario minimo) nao ha conta
				// de obras: o limite inteiro fica nos servicos.
				city.government_budget += works;
			}
			city.regional_budget_open_services = city.government_budget;
			city.regional_budget_open_works =
				cities[slot] < ws.city_gov.size()
				? ws.city_gov[cities[slot]].cash : 0.0;
			limit_total += amount;
		}
	}
	ws.regional_budget_started = true;
	return limit_total;
}

void absorb_public_cash(WorldState& ws) {
	const std::size_t count = std::min(ws.cities.size(), ws.city_gov.size());
	for (std::size_t index = 0; index < ws.cities.size(); ++index) {
		City& city = ws.cities[index];
		double amount = safe_nonneg(city.government_budget);
		if (index < count) {
			amount += safe_nonneg(ws.city_gov[index].cash);
			ws.city_gov[index].cash = 0.0;
		}
		city.government_budget = 0.0;
		if (amount > 0.0) {
			collect(ws, city.nation, amount);
		}
		city.regional_budget_open_services = 0.0;
		city.regional_budget_open_works = 0.0;
	}
}

double reconcile(WorldState& ws) {
	if (!enabled(ws)) {
		return 0.0;
	}
	const std::size_t count = std::min(ws.cities.size(), ws.city_gov.size());
	double spent_total = 0.0;
	for (std::size_t index = 0; index < ws.cities.size(); ++index) {
		City& city = ws.cities[index];
		// Contadores sao o restante da dotacao do mes; nunca negativos.
		double services_now = safe_nonneg(city.government_budget);
		double works_now = index < count
			? safe_nonneg(ws.city_gov[index].cash) : 0.0;
		const double spent =
			(city.regional_budget_open_services - services_now)
			+ (city.regional_budget_open_works - works_now);
		if (spent > 0.0) {
			double& cash = ws.nation_treasury[city.nation];
			// Saldo ANTES do debito: e o maximo que o Tesouro pode honrar.
			const double balance = safe_nonneg(cash);
			const double paid = std::min(spent, balance);
			cash = std::max(0.0, balance - paid);
			city.regional_budget_spent_month += paid;
			city.regional_budget_spent_total += paid;
			ws.treasury_spent_total += paid;
			spent_total += paid;
			if (paid + 1e-9 < spent) {
				// Sem saldo no Tesouro: o excesso nao e gasto (despesa
				// adiada). O contador volta a refletir o que nao foi pago,
				// mas o adiado NUNCA excede o saldo que existia antes do
				// debito: sem o clamp um gasto sem lastro voltava inteiro ao
				// contador e a despesa seguinte voltava a criar dinheiro
				// (QA t173). Com os limites derivados do saldo da abertura o
				// ramo e inalcancavel; o clamp e defensivo.
				const double deferred = std::min(spent - paid, balance);
				if (deferred > 0.0) {
					city.government_budget += deferred;
					services_now += deferred;
					city.regional_budget_deferred_month += deferred;
				}
			}
		} else if (spent < 0.0) {
			// Receita publica do intervalo: entra no Tesouro e NAO aumenta a
			// dotacao do mes (o limite e um teto, nao uma carteira).
			collect(ws, city.nation, -spent);
			city.government_budget = city.regional_budget_open_services;
			if (index < count) {
				ws.city_gov[index].cash = city.regional_budget_open_works;
			}
			services_now = city.government_budget;
			works_now = index < count
				? ws.city_gov[index].cash : 0.0;
		}
		city.regional_budget_open_services = services_now;
		city.regional_budget_open_works = works_now;
	}
	return spent_total;
}

void clear_counters(WorldState& ws) {
	const std::size_t count = std::min(ws.cities.size(), ws.city_gov.size());
	for (std::size_t index = 0; index < ws.cities.size(); ++index) {
		City& city = ws.cities[index];
		city.government_budget = 0.0;
		if (index < count) {
			ws.city_gov[index].cash = 0.0;
		}
		city.regional_budget_limit_month = 0.0;
		city.regional_budget_spent_month = 0.0;
		city.regional_budget_deferred_month = 0.0;
		city.regional_budget_open_services = 0.0;
		city.regional_budget_open_works = 0.0;
	}
	ws.regional_budget_started = false;
}

void drain_to_regions(WorldState& ws) {
	// Total publico por nacao = Tesouro + restantes dos contadores virtuais.
	std::map<std::string, double> per_nation = ws.nation_treasury;
	const std::size_t count = std::min(ws.cities.size(), ws.city_gov.size());
	for (std::size_t index = 0; index < ws.cities.size(); ++index) {
		City& city = ws.cities[index];
		per_nation[city.nation] += safe_nonneg(city.government_budget);
		city.government_budget = 0.0;
		if (index < count) {
			per_nation[city.nation] += safe_nonneg(ws.city_gov[index].cash);
			ws.city_gov[index].cash = 0.0;
		}
		city.regional_budget_limit_month = 0.0;
		city.regional_budget_spent_month = 0.0;
		city.regional_budget_deferred_month = 0.0;
		city.regional_budget_open_services = 0.0;
		city.regional_budget_open_works = 0.0;
	}
	ws.nation_treasury.clear();
	for (std::size_t index = 0; index < ws.cities.size(); ++index) {
		City& city = ws.cities[index];
		const auto found = per_nation.find(city.nation);
		if (found == per_nation.end()) {
			continue;
		}
		const double amount = safe_nonneg(found->second);
		per_nation.erase(found);
		if (amount > 0.0) {
			city.government_budget += amount;
		}
	}
	// Nacao sem cidade no mundo: o saldo nao se perde.
	double leftover = 0.0;
	for (const auto& row : per_nation) {
		leftover += safe_nonneg(row.second);
	}
	ws.construction_cash += leftover;
	ws.regional_budget_started = false;
}

} // namespace treasury
} // namespace ogs
