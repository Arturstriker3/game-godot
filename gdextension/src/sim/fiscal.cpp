// Módulo de domínio: fiscal — equalização fiscal nacional e dividendos
// (S14 do docs/economy/core_refactor_plan.md).
// SimCore:: métodos movidos de sim_core.cpp — realocação byte-idêntica.
#include "sim_core.h"

#include "profiling.h"
#include "sim_constants.h"
#include "sim_labor.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace ogs {
void SimCore::phase_fiscal() {
	// Equalização fiscal per capita dentro de cada nação: o orçamento público é
	// repartido entre as cidades proporcionalmente à população. Uma cidade rica
	// financia a segurança alimentar de uma cidade pobre da mesma nação (ex.:
	// Paris sustenta Cayenne). Conserva dinheiro — só move government_budget
	// entre cidades da MESMA nação.
	std::unordered_map<std::string, std::vector<DemoCity*>> by_nation;
	by_nation.reserve(ws_.cities.size() / 4 + 8);
	for (DemoCity& city : ws_.cities) {
		by_nation[city.nation].push_back(&city);
	}
	// Cada nação é independente: a ordem de varredura não muda o resultado.
	for (auto& entry : by_nation) {
		std::vector<DemoCity*>& cities = entry.second;
		double total_budget = 0.0;
		double total_population = 0.0;
		for (DemoCity* city : cities) {
			total_budget += city->government_budget;
			total_population += city->simulation_population;
		}
		if (cities.size() < 2 || total_population <= 0.0) {
			continue;
		}
		for (DemoCity* city : cities) {
			city->government_budget =
				total_budget * (city->simulation_population / total_population);
		}
	}
	if (ws_.groups.empty()) {
		// Sem grupos não há dividendo, mas a fase continua cronometrada.
		OGS_SIM_PHASE_LAP(kDemoPhaseFiscal);
		return;
	}
	// Recuperação lenta da confiança depois do prazo da nacionalização.
	for (auto it = ws_.confidence_penalty_day.begin();
			it != ws_.confidence_penalty_day.end();) {
		if (ws_.day >= it->second) {
			auto confidence = ws_.confidence.find(it->first);
			if (confidence != ws_.confidence.end()) {
				confidence->second = std::min(1.0, confidence->second + 0.005);
			}
			if (confidence == ws_.confidence.end()
					|| confidence->second >= 1.0) {
				it = ws_.confidence_penalty_day.erase(it);
				continue;
			}
		}
		++it;
	}
	for (std::size_t index = 0; index < ws_.groups.size(); ++index) {
		DemoGroup& group = ws_.groups[index];
		if (group.state_owned || group.distributable <= 1e-9) {
			continue;
		}
		// Dividendo ao estrato dono: fração do lucro acumulado. O que sobra
		// fica como capital para o próximo investimento.
		double payable = group.distributable * kDemoGroupDividendShare;
		payable = std::min(payable, group.cash);
		if (payable <= 1e-9) {
			continue;
		}
		double host_total = 0.0;
		for (const auto& row : group.profit_by_host) {
			host_total += std::max(0.0, row.second);
		}
		if (host_total <= 1e-9) {
			const double credited = demo_credit_nation_households(
				group.origin_nation, payable);
			if (credited > 0.0) {
				group.cash -= credited;
				group.distributable = std::max(0.0, group.distributable - credited);
				group.dividends_total += credited;
				ws_.group_flows[group.origin_nation].dividends += credited;
			}
			continue;
		}
		double credited_total = 0.0;
		std::vector<std::string> exhausted;
		for (auto& row : group.profit_by_host) {
			const double share = payable * std::max(0.0, row.second) / host_total;
			if (share <= 1e-9) {
				continue;
			}
			const bool domestic = row.first == group.origin_nation;
			const double credited = demo_credit_nation_households(
				group.origin_nation, share);
			if (credited <= 0.0) {
				continue;
			}
			credited_total += credited;
			if (domestic) {
				ws_.group_flows[row.first].dividends += credited;
			} else {
				// Repatriação: sai do anfitrião, entra na origem.
				ws_.group_flows[row.first].repatriated_out += credited;
				ws_.group_flows[group.origin_nation].repatriated_in += credited;
			}
			row.second -= share;
			if (row.second <= 1e-9) {
				exhausted.push_back(row.first);
			}
		}
		for (const std::string& host : exhausted) {
			group.profit_by_host.erase(host);
		}
		if (credited_total > 0.0) {
			group.cash -= credited_total;
			group.distributable = std::max(0.0,
				group.distributable - credited_total);
			group.dividends_total += credited_total;
		}
	}

	OGS_SIM_PHASE_LAP(kDemoPhaseFiscal);
}
} // namespace ogs
