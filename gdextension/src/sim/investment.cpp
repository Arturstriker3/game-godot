// Módulo de domínio: investimento, propriedade e grupos (F9)
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

// ------------------------------------------------------------------
// F9 — investimento e propriedade
// ------------------------------------------------------------------

int SimCore::demo_owner_kind(const std::string& owner, std::string* group_id,
		std::string* origin_nation) const {
	if (group_id != nullptr) {
		group_id->clear();
	}
	if (origin_nation != nullptr) {
		origin_nation->clear();
	}
	if (owner == "state") {
		return 0;
	}
	const std::string national_prefix = "national:";
	if (owner.rfind(national_prefix, 0) == 0) {
		if (group_id != nullptr) {
			*group_id = owner.substr(national_prefix.size());
		}
		return 1;
	}
	const std::string foreign_prefix = "foreign:";
	if (owner.rfind(foreign_prefix, 0) == 0) {
		const std::string rest = owner.substr(foreign_prefix.size());
		const std::size_t at = rest.find('@');
		if (group_id != nullptr) {
			*group_id = at == std::string::npos ? rest : rest.substr(0, at);
		}
		if (origin_nation != nullptr) {
			*origin_nation = at == std::string::npos
				? std::string() : rest.substr(at + 1);
		}
		return 2;
	}
	return 3;  // "private" e qualquer dono desconhecido
}
int SimCore::demo_group_index(const std::string& id) const {
	const auto found = ws_.group_lookup.find(id);
	return found == ws_.group_lookup.end() ? -1 : found->second;
}
DemoGroup* SimCore::find_demo_group(const std::string& id) {
	const int index = demo_group_index(id);
	return index < 0 ? nullptr : &ws_.groups[static_cast<std::size_t>(index)];
}
const DemoGroup* SimCore::find_demo_group(
		const std::string& id) const {
	const int index = demo_group_index(id);
	return index < 0 ? nullptr : &ws_.groups[static_cast<std::size_t>(index)];
}
int SimCore::demo_fdi_mode(const std::string& nation,
		const std::string& sector) const {
	const auto found = ws_.fdi_lookup.find(nation + "|" + sector);
	return found == ws_.fdi_lookup.end()
		? 0 : ws_.fdi_policies[static_cast<std::size_t>(found->second)].mode;
}
double SimCore::demo_sector_tax_holiday(const std::string& nation,
		const std::string& sector) const {
	const auto found = ws_.incentive_lookup.find(nation + "|" + sector);
	if (found == ws_.incentive_lookup.end()) {
		return 0.0;
	}
	return std::clamp(
		ws_.incentives[static_cast<std::size_t>(found->second)].tax_holiday,
		0.0, 1.0);
}
double SimCore::demo_sector_subsidy(const std::string& nation,
		const std::string& sector) const {
	const auto found = ws_.incentive_lookup.find(nation + "|" + sector);
	if (found == ws_.incentive_lookup.end()) {
		return 0.0;
	}
	return std::max(0.0,
		ws_.incentives[static_cast<std::size_t>(found->second)].subsidy);
}
double SimCore::demo_nation_confidence(const std::string& nation) const {
	const auto found = ws_.confidence.find(nation);
	return found == ws_.confidence.end() ? 1.0
		: std::clamp(found->second, 0.0, 1.0);
}
double SimCore::demo_credit_nation_households(const std::string& nation,
		double amount) {
	if (amount <= 0.0) {
		return 0.0;
	}
	// F9-perf: só as cidades da nação (índice ws_.cities_by_nation), não o
	// mundo. Antes eram duas varreduras de 834 cidades por grupo com dividendo
	// a pagar — a fase `fiscal` do tick pagava isso todo dia.
	const auto found = ws_.cities_by_nation.find(nation);
	if (found == ws_.cities_by_nation.end() || found->second.empty()) {
		return 0.0;
	}
	const std::vector<int>& indices = found->second;
	// O dono do capital é aproximado pelo estrato de maior escolaridade (a
	// proxy disponível no modelo para os proprietários); sem ele, população.
	double weight_total = 0.0;
	for (const int index : indices) {
		const DemoCity& city = ws_.cities[static_cast<std::size_t>(index)];
		double weight = city.pop.class_population[kDemoEducationLevels - 1];
		if (!(weight > 0.0)) {
			weight = city.simulation_population;
		}
		weight_total += weight;
	}
	if (!(weight_total > 0.0)) {
		return 0.0;
	}
	for (const int index : indices) {
		DemoCity& city = ws_.cities[static_cast<std::size_t>(index)];
		double weight = city.pop.class_population[kDemoEducationLevels - 1];
		if (!(weight > 0.0)) {
			weight = city.simulation_population;
		}
		city.capital_income_next += amount * weight / weight_total;
	}
	return amount;
}
void SimCore::demo_route_group_profit(int group_index,
		const std::string& host_nation, double profit, double revenue,
		DemoCity& city, double& firm_surplus) {
	if (group_index < 0 || group_index >= static_cast<int>(ws_.groups.size())) {
		return;
	}
	DemoGroup& group = ws_.groups[static_cast<std::size_t>(group_index)];
	group.revenue_total += revenue;
	group.profit_total += profit;
	if (profit > 0.0) {
		if (group.state_owned) {
			// Lucro de estatal -> orçamento do anfitrião (dinheiro conservado).
			const double moved = std::min(profit, firm_surplus);
			city.firm_cash -= moved;
			city.government_budget += moved;
			firm_surplus -= moved;
			return;
		}
		// Imposto corporativo: fica no anfitrião. A isenção do incentivo
		// setorial reduz a alíquota.
		const double holiday = demo_sector_tax_holiday(host_nation, group.sector);
		const double tax_rate = std::clamp(
			kDemoCorporateTaxRate * (1.0 - holiday), 0.0, 1.0);
		const double tax = std::min(profit * tax_rate, firm_surplus);
		city.firm_cash -= tax;
		city.government_budget += tax;
		firm_surplus -= tax;
		group.taxes_total += tax;
		ws_.group_flows[host_nation].taxes += tax;
		const double net = profit - tax;
		const double moved = std::min(net, firm_surplus);
		city.firm_cash -= moved;
		group.cash += moved;
		group.distributable += moved;
		firm_surplus -= moved;
		group.profit_by_host[host_nation] += moved;
		// Incentivo: subsídio ad valorem sobre a receita do ativo, pago pelo
		// governo do anfitrião (política explícita; 0 por padrão).
		const double subsidy_rate = demo_sector_subsidy(host_nation, group.sector);
		if (subsidy_rate > 0.0 && revenue > 0.0) {
			const double subsidy = std::min(
				std::max(0.0, city.government_budget), revenue * subsidy_rate);
			if (subsidy > 0.0) {
				city.government_budget -= subsidy;
				group.cash += subsidy;
				group.distributable += subsidy;
				ws_.group_flows[host_nation].subsidy += subsidy;
			}
		}
	} else if (profit < 0.0) {
		// O grupo recapitaliza o ativo até o limite do caixa; o resto da perda
		// continua na firma (pode levar a falência, como antes).
		const double inject = std::min(-profit, group.cash);
		if (inject > 0.0) {
			group.cash -= inject;
			city.firm_cash += inject;
		}
	}
}

// ------------------------------------------------------------------
// S6b — helpers de load/setup movidos da fachada (economy_sim.cpp).
// ------------------------------------------------------------------

void SimCore::demo_seed_groups() {
	if (ws_.groups_seeded || !ws_.groups.empty()) {
		return;
	}
	// Agrupa por (nação, setor do produto) — a regra documentada no
	// GAME_STATE/docs. `std::map` mantém a ordem determinística.
	std::map<std::string, std::vector<int>> buckets;
	for (std::size_t index = 0; index < ws_.buildings.size(); ++index) {
		const DemoBuilding& building = ws_.buildings[index];
		if (building.owner != "private") {
			continue;
		}
		const DemoCity* city = find_demo_city(building.city);
		if (city == nullptr) {
			continue;
		}
		std::string sector;
		if (building.output_product >= 0
				&& building.output_product < static_cast<int>(ws_.product_sectors.size())) {
			sector = ws_.product_sectors[static_cast<std::size_t>(building.output_product)];
		}
		if (sector.empty()) {
			sector = (building.recipe >= 0 || building.input_product >= 0)
				? "industry" : "primary";
		}
		buckets[city->nation + "|" + sector].push_back(static_cast<int>(index));
	}
	int chunk_serial = 0;
	for (auto& entry : buckets) {
		std::vector<int>& indices = entry.second;
		const std::size_t separator = entry.first.find('|');
		const std::string nation = entry.first.substr(0, separator);
		const std::string sector = separator == std::string::npos
			? std::string("primary") : entry.first.substr(separator + 1);
		std::sort(indices.begin(), indices.end(), [this](int a, int b) {
			return ws_.buildings[static_cast<std::size_t>(a)].id
				< ws_.buildings[static_cast<std::size_t>(b)].id;
		});
		for (std::size_t start = 0; start < indices.size();
				start += static_cast<std::size_t>(kDemoGroupChunkAssets)) {
			DemoGroup group;
			group.id = "G-" + demo_sane_token(nation) + "-"
				+ demo_sane_token(sector) + "-" + std::to_string(chunk_serial);
			group.origin_nation = nation;
			group.sector = sector;
			const int group_index = static_cast<int>(ws_.groups.size());
			ws_.group_lookup[group.id] = group_index;
			ws_.groups.push_back(group);
			const std::size_t end = std::min(indices.size(),
				start + static_cast<std::size_t>(kDemoGroupChunkAssets));
			for (std::size_t i = start; i < end; ++i) {
				DemoBuilding& building =
					ws_.buildings[static_cast<std::size_t>(indices[i])];
				building.owner = "national:" + group.id;
			}
			++chunk_serial;
		}
	}
	ws_.groups_seeded = true;
}
int SimCore::demo_recipe_index(const std::string& recipe_id) const {
	const auto found = ws_.recipe_lookup.find(recipe_id);
	if (found == ws_.recipe_lookup.end()) {
		return -1;
	}
	return found->second;
}
} // namespace ogs
