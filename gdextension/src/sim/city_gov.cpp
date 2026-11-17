// §19 (W2) — prefeitura: fluxo mensal de caixa e obras urbanas.
// C++ puro (STL via world_state.h/sim_constants.h), sem godot_cpp em sim/.
//
// Ordem: (1) avança as obras do dia; (2) no fechamento do mês cobra IPTU e
// taxas, paga o repasse do tesouro nacional, paga a manutenção urbana e decide
// a próxima obra. Tudo em série, na ordem de índice de cidade — determinístico
// com qualquer número de threads (as fases paralelas são outras).
#include "city_gov.h"

#include "sim_constants.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
// Nada de NaN/Inf no estado nem nos fluxos.
inline double safe_nonneg(double value) {
	return std::isfinite(value) ? std::max(0.0, value) : 0.0;
}
inline double safe_unit(double value) {
	return std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 0.0;
}
} // namespace

namespace ogs {
namespace city_gov {
namespace {

// Unidades residenciais do estoque (todas as densidades e padrões).
double housing_units(const CityHousing& house) {
	double total = 0.0;
	for (int density = 0; density < DENSITY_COUNT; ++density) {
		for (int tier = 0; tier < TIER_COUNT; ++tier) {
			total += safe_nonneg(house.units[density][tier]);
		}
	}
	return total;
}

// Capacidade física de uma zona em unidades: área × FAR do gabarito ÷ m² da
// unidade. Derivação local do W2 — o W1 (urban_land.cpp) é o dono da regra de
// ocupação e vai substituir estas duas funções.
double zone_capacity_units(const CityLand& land, int zone) {
	const int density = std::clamp(static_cast<int>(land.max_density[zone]),
		0, DENSITY_COUNT - 1);
	const double unit_m2 = std::max(1.0, kDemoUrbanUnitM2[density][TIER_POPULAR]);
	return safe_nonneg(land.area_km2[zone]) * 1.0e6
		* kDemoUrbanFar[density] / unit_m2;
}

// Capacidade de uma zona em negócios (serviços/fábricas urbanas): área × FAR ÷
// m² de piso por negócio (parâmetro declarado; o W1 refina por setor).
double zone_capacity_businesses(const CityLand& land, int zone) {
	const int density = std::clamp(static_cast<int>(land.max_density[zone]),
		0, DENSITY_COUNT - 1);
	return safe_nonneg(land.area_km2[zone]) * 1.0e6
		* kDemoUrbanFar[density] / kBusinessFloorM2;
}

// Unidades por km² que o gabarito permite (FAR ÷ m² da unidade popular).
double units_per_km2(int density) {
	const int d = std::clamp(density, 0, DENSITY_COUNT - 1);
	const double unit_m2 = std::max(1.0, kDemoUrbanUnitM2[d][TIER_POPULAR]);
	return 1.0e6 * kDemoUrbanFar[d] / unit_m2;
}

// Habitantes que `km2` da zona passam a atender quando a infraestrutura chega,
// no gabarito atual da zona.
double infra_people_from_area(const CityLand& land, int zone, double km2) {
	return units_per_km2(static_cast<int>(land.max_density[zone]))
		* safe_nonneg(km2) * kPeoplePerDwelling;
}

// Fecha a obra: o fundo reservado vira pagamento ao setor de construção
// (dinheiro conservado) e o efeito físico entra no estado urbano.
void complete_project(WorldState& ws, std::size_t city_index,
		CityGovProject& project) {
	ws.construction_cash += safe_nonneg(project.reserved);
	project.reserved = 0.0;
	project.days_left = 0;
	switch (project.kind) {
	case URBAN_PROJECT_EXPAND: {
		if (city_index >= ws.land.size()) {
			return;
		}
		CityLand& land = ws.land[city_index];
		const int zone = std::clamp(project.zone, 0, ZONE_COUNT - 1);
		const double km2 = std::min(safe_nonneg(project.area_km2),
			safe_nonneg(land.reserve_km2));
		if (km2 <= 0.0) {
			return;  // reserva consumida por outra obra: nada a abrir
		}
		land.area_km2[zone] += km2;
		land.reserve_km2 = std::max(0.0, land.reserve_km2 - km2);
		land.infra_capacity += infra_people_from_area(land, zone, km2);
		return;
	}
	case URBAN_PROJECT_DENSIFY: {
		if (city_index >= ws.land.size()) {
			return;
		}
		CityLand& land = ws.land[city_index];
		const int zone = std::clamp(project.zone, 0, ZONE_COUNT - 1);
		const int old_density = static_cast<int>(land.max_density[zone]);
		if (old_density < DENSITY_COUNT - 1) {
			land.max_density[zone] = static_cast<Density>(old_density + 1);
			// A capacidade de infraestrutura sobe pela diferença de unidades
			// por km² entre o gabarito antigo e o novo, na área da zona.
			land.infra_capacity += (units_per_km2(old_density + 1)
				- units_per_km2(old_density))
				* safe_nonneg(land.area_km2[zone]) * kPeoplePerDwelling;
		}
		return;
	}
	case URBAN_PROJECT_SOCIAL_HOUSING: {
		if (city_index >= ws.housing.size()) {
			return;
		}
		CityHousing& house = ws.housing[city_index];
		const double units = safe_nonneg(project.units);
		if (units <= 0.0) {
			return;
		}
		const double total_before = housing_units(house);
		house.units[DENSITY_LOW][TIER_POPULAR] += units;
		const double total_after = total_before + units;
		// A fração do governo sobe pelo estoque novo; os demais donos são
		// renormalizados para a soma continuar 1 (W3 assume daqui em diante).
		const double gov_units =
			total_before * safe_unit(house.owner_share[OWNER_GOVERNMENT]) + units;
		const double gov_share = total_after > 1e-9
			? std::clamp(gov_units / total_after, 0.0, 1.0) : 0.0;
		const double other_sum = safe_nonneg(house.owner_share[OWNER_HOUSEHOLD])
			+ safe_nonneg(house.owner_share[OWNER_FIRM]);
		if (other_sum <= 1e-9) {
			house.owner_share[OWNER_HOUSEHOLD] = 1.0 - gov_share;
			house.owner_share[OWNER_FIRM] = 0.0;
		} else {
			const double scale = (1.0 - gov_share) / other_sum;
			house.owner_share[OWNER_HOUSEHOLD] =
				safe_nonneg(house.owner_share[OWNER_HOUSEHOLD]) * scale;
			house.owner_share[OWNER_FIRM] =
				safe_nonneg(house.owner_share[OWNER_FIRM]) * scale;
		}
		house.owner_share[OWNER_GOVERNMENT] =
			1.0 - house.owner_share[OWNER_HOUSEHOLD] - house.owner_share[OWNER_FIRM];
		return;
	}
	default:
		return;
	}
}

// Avança o prazo das obras do dia e compacta os slots concluídos.
void advance_projects(WorldState& ws, std::size_t count) {
	for (std::size_t city_index = 0; city_index < count; ++city_index) {
		CityGov& gov = ws.city_gov[city_index];
		if (gov.project_count <= 0) {
			continue;
		}
		for (int slot = 0; slot < gov.project_count; ++slot) {
			CityGovProject& project = gov.projects[slot];
			if (project.days_left > 0) {
				--project.days_left;
			}
			if (project.days_left <= 0) {
				complete_project(ws, city_index, project);
			}
		}
		int kept = 0;
		for (int slot = 0; slot < gov.project_count; ++slot) {
			if (gov.projects[slot].days_left > 0) {
				if (kept != slot) {
					gov.projects[kept] = gov.projects[slot];
				}
				++kept;
			}
		}
		for (int slot = kept; slot < gov.project_count; ++slot) {
			gov.projects[slot] = CityGovProject{};
		}
		gov.project_count = kept;
	}
}

// Repasse do tesouro nacional: o orçamento público das cidades da nação paga o
// que a prefeitura de cada cidade pediu, na ordem do índice de cidade (nunca na
// ordem de um unordered_map). Se o tesouro não cobre, todos recebem na mesma
// proporção. Cada débito e cada crédito usam o MESMO número — conserva dinheiro.
void pay_transfers(WorldState& ws, std::size_t count,
		const std::vector<double>& want) {
	std::vector<std::string> order;
	std::unordered_map<std::string, std::vector<std::size_t>> by_nation;
	for (std::size_t city_index = 0; city_index < count; ++city_index) {
		const std::string& nation = ws.cities[city_index].nation;
		auto found = by_nation.find(nation);
		if (found == by_nation.end()) {
			order.push_back(nation);
			found = by_nation.emplace(nation, std::vector<std::size_t>{}).first;
		}
		found->second.push_back(city_index);
	}
	for (const std::string& nation : order) {
		const std::vector<std::size_t>& cities = by_nation.at(nation);
		double available = 0.0;
		double total_want = 0.0;
		for (const std::size_t city_index : cities) {
			available += safe_nonneg(ws.cities[city_index].government_budget);
			total_want += safe_nonneg(want[city_index]);
		}
		if (total_want <= 1e-9) {
			continue;
		}
		const double scale = available + 1e-9 < total_want
			? available / total_want : 1.0;
		for (const std::size_t city_index : cities) {
			const double take = safe_nonneg(want[city_index]) * scale;
			if (take <= 1e-9) {
				continue;
			}
			DemoCity& city = ws.cities[city_index];
			CityGov& gov = ws.city_gov[city_index];
			const double paid = std::min(take,
				safe_nonneg(city.government_budget));
			if (paid <= 0.0) {
				continue;
			}
			city.government_budget -= paid;
			gov.cash += paid;
			gov.month_revenue_transfer += paid;
			gov.month_transfer_paid += paid;
		}
	}
}

// Decisão de obra do mês (regra simples e determinística da spec §19 W2):
// zona acima de kBusyOccupancy -> adensar se a infraestrutura atende a
// população, senão expandir se há reserva de solo; sem zona cheia e com déficit
// popular -> moradia social. Respeita o caixa (sem dívida).
void decide_works(WorldState& ws, std::size_t city_index, int day) {
	DemoCity& city = ws.cities[city_index];
	CityGov& gov = ws.city_gov[city_index];
	const CityLand& land = ws.land[city_index];
	const CityHousing& house = ws.housing[city_index];

	const double cap_res = zone_capacity_units(land, ZONE_RES);
	const double occ_res = cap_res > 0.0 ? housing_units(house) / cap_res : 0.0;
	double businesses = 0.0;
	if (city_index < ws.service_by_city.size()) {
		businesses += static_cast<double>(ws.service_by_city[city_index].size());
	}
	if (city_index < ws.buildings_by_city.size()) {
		businesses += static_cast<double>(ws.buildings_by_city[city_index].size());
	}
	const double cap_com = zone_capacity_businesses(land, ZONE_COM);
	const double cap_ind = zone_capacity_businesses(land, ZONE_IND);
	const double occ_com = cap_com > 0.0 ? businesses / cap_com : 0.0;
	const double occ_ind = cap_ind > 0.0 ? businesses / cap_ind : 0.0;

	int busy_zone = -1;
	double worst = kBusyOccupancy;
	if (occ_res > worst) {
		busy_zone = ZONE_RES;
		worst = occ_res;
	}
	if (occ_com > worst) {
		busy_zone = ZONE_COM;
		worst = occ_com;
	}
	if (occ_ind > worst) {
		busy_zone = ZONE_IND;
		worst = occ_ind;
	}

	if (busy_zone >= 0) {
		// Adensar: só se o gabarito permite e a infraestrutura já atende a
		// população atual (densidade nova exige infra, spec W2).
		const bool can_densify =
			static_cast<int>(land.max_density[busy_zone]) < DENSITY_COUNT - 1
			&& land.infra_capacity + 1e-9 >= city.simulation_population;
		if (can_densify) {
			const double cost = safe_nonneg(land.area_km2[busy_zone])
				* kDemoUrbanInfraCostPerKm2 * kDensifyCostShare;
			if (start_densify(gov, busy_zone, cost, day)) {
				gov.month_expense_infrastructure += gov.projects[
					gov.project_count - 1].reserved;
				return;
			}
		}
		if (land.reserve_km2 > 1e-9) {
			const double km2 = std::min(kExpansionKm2, land.reserve_km2);
			const double cost = km2 * kDemoUrbanInfraCostPerKm2;
			if (start_expansion(gov, land, busy_zone, km2, cost, day)) {
				gov.month_expense_infrastructure += gov.projects[
					gov.project_count - 1].reserved;
				return;
			}
		}
		return;
	}

	// Déficit popular -> moradia social. W3 mede a demanda por estrato e o
	// aluguel subsidiado; até lá o déficit é o que falta de unidades para os
	// domicílios estimados (população ÷ pessoas por domicílio).
	const double households =
		safe_nonneg(city.simulation_population) / kPeoplePerDwelling;
	const double deficit = households - housing_units(house);
	const double unit_price = safe_nonneg(house.price[TIER_POPULAR]);
	if (deficit <= 1.0 || unit_price <= 1e-9) {
		return;
	}
	const double units = std::min(deficit, safe_nonneg(gov.cash) / unit_price);
	if (units < 1.0) {
		return;
	}
	if (start_social_housing(gov, units, units * unit_price, day)) {
		gov.month_expense_social_housing += gov.projects[
			gov.project_count - 1].reserved;
	}
}

// Fechamento mensal: receita, repasse, manutenção e decisão.
void monthly_cash_flow(WorldState& ws, std::size_t count, int day) {
	std::vector<double> transfer_want(count, 0.0);
	for (std::size_t city_index = 0; city_index < count; ++city_index) {
		DemoCity& city = ws.cities[city_index];
		CityGov& gov = ws.city_gov[city_index];
		const CityHousing& house = ws.housing[city_index];
		// O acumulado do mês anterior fica no save (a UI testemunha o último
		// fechamento); aqui começa o mês novo.
		gov.month_revenue_iptu = 0.0;
		gov.month_revenue_fees = 0.0;
		gov.month_revenue_transfer = 0.0;
		gov.month_expense_maintenance = 0.0;
		gov.month_expense_infrastructure = 0.0;
		gov.month_expense_social_housing = 0.0;
		gov.month_transfer_paid = 0.0;

		const double monthly_gdp =
			safe_nonneg(city.gdp) * static_cast<double>(kMonthDays);
		gov.target_revenue_month =
			safe_unit(gov.revenue_pct_gdp) * monthly_gdp;

		// IPTU: alíquota efetiva (peso do IPTU no total de tributos × carga
		// tributária) × PIB do mês. É a mesma forma "alíquota × valor do
		// estoque" da spec, com a alíquota calibrada pelo PIB — a alíquota crua
		// do W0 (~5-10% ao ano do estoque) cobraria ~10× a real.
		const double iptu_due =
			safe_nonneg(gov.iptu_rate) * kTotalTaxPctOfGdp * monthly_gdp;
		if (iptu_due > 0.0) {
			const double non_gov = 1.0
				- safe_unit(house.owner_share[OWNER_GOVERNMENT]);
			const double private_share =
				safe_nonneg(house.owner_share[OWNER_HOUSEHOLD])
				+ safe_nonneg(house.owner_share[OWNER_FIRM]);
			const double household_part = private_share > 1e-9
				? safe_nonneg(house.owner_share[OWNER_HOUSEHOLD]) / private_share
				: 0.0;
			const double due = iptu_due * non_gov;
			// Só se cobra o que existe em caixa: o modelo não tem dívida nem
			// execução fiscal (spec W2: "respeita caixa").
			const double hh_paid = std::min(due * household_part,
				safe_nonneg(city.pop.savings));
			const double firm_paid = std::min(due * (1.0 - household_part),
				safe_nonneg(city.firm_cash));
			city.pop.savings = safe_nonneg(city.pop.savings) - hh_paid;
			city.firm_cash = safe_nonneg(city.firm_cash) - firm_paid;
			gov.cash += hh_paid + firm_paid;
			gov.month_revenue_iptu = hh_paid + firm_paid;
		}

		// Taxas locais: alíquota sobre o faturamento de serviços do mês (o
		// faturamento do dia × dias do mês). Sai do caixa dos serviços, de onde
		// o faturamento saiu.
		const double fee_due = safe_nonneg(gov.fee_rate)
			* safe_nonneg(city.service_revenue)
			* static_cast<double>(kMonthDays);
		const double fee_paid = std::min(fee_due,
			safe_nonneg(city.service_cash));
		if (fee_paid > 0.0) {
			city.service_cash = safe_nonneg(city.service_cash) - fee_paid;
			gov.cash += fee_paid;
			gov.month_revenue_fees = fee_paid;
		}

		// Repasse desejado: fração observada da receita municipal alvo. O
		// pagamento é por nação no passo seguinte (tesouro comum).
		transfer_want[city_index] =
			safe_unit(gov.transfer_share) * gov.target_revenue_month;
	}
	pay_transfers(ws, count, transfer_want);
	for (std::size_t city_index = 0; city_index < count; ++city_index) {
		DemoCity& city = ws.cities[city_index];
		CityGov& gov = ws.city_gov[city_index];
		// Manutenção urbana paga ao setor de serviços da cidade (o serviço é
		// real; o lucro segue a distribuição normal do setor).
		const double maintenance_due =
			gov.target_revenue_month * kMaintenanceShareOfTarget;
		const double maintenance_paid =
			std::min(maintenance_due, safe_nonneg(gov.cash));
		if (maintenance_paid > 0.0) {
			gov.cash -= maintenance_paid;
			city.service_cash += maintenance_paid;
			gov.month_expense_maintenance = maintenance_paid;
		}
		if (!has_project(gov)) {
			decide_works(ws, city_index, day);
		}
	}
}

} // namespace

bool has_project(const CityGov& gov) {
	return gov.project_count > 0;
}

bool start_expansion(CityGov& gov, const CityLand& land, int zone, double km2,
		double cost, int day) {
	if (zone < 0 || zone >= ZONE_COUNT
			|| gov.project_count >= kCityGovMaxProjects) {
		return false;
	}
	const double area = std::min(safe_nonneg(km2), safe_nonneg(land.reserve_km2));
	const double budget = std::min(safe_nonneg(cost), safe_nonneg(gov.cash));
	if (area <= 1e-9 || budget <= 1e-9) {
		return false;
	}
	// Contrata-se só a área que o orçamento paga (obra não fica sem fundo).
	const double paid_area = safe_nonneg(cost) > budget
		? area * (budget / safe_nonneg(cost)) : area;
	CityGovProject& project = gov.projects[gov.project_count];
	project = CityGovProject{};
	project.kind = URBAN_PROJECT_EXPAND;
	project.zone = zone;
	project.area_km2 = paid_area;
	project.cost = budget;
	project.reserved = budget;
	project.days_left = kProjectDaysExpand;
	project.created_day = day;
	++gov.project_count;
	gov.cash = safe_nonneg(gov.cash) - budget;
	return true;
}

bool start_densify(CityGov& gov, int zone, double cost, int day) {
	if (zone < 0 || zone >= ZONE_COUNT
			|| gov.project_count >= kCityGovMaxProjects) {
		return false;
	}
	const double budget = std::min(safe_nonneg(cost), safe_nonneg(gov.cash));
	if (budget <= 1e-9) {
		return false;
	}
	CityGovProject& project = gov.projects[gov.project_count];
	project = CityGovProject{};
	project.kind = URBAN_PROJECT_DENSIFY;
	project.zone = zone;
	project.cost = budget;
	project.reserved = budget;
	project.days_left = kProjectDaysDensify;
	project.created_day = day;
	++gov.project_count;
	gov.cash = safe_nonneg(gov.cash) - budget;
	return true;
}

bool start_social_housing(CityGov& gov, double units, double cost, int day) {
	if (gov.project_count >= kCityGovMaxProjects) {
		return false;
	}
	const double budget = std::min(safe_nonneg(cost), safe_nonneg(gov.cash));
	if (budget <= 1e-9 || safe_nonneg(units) < 1.0) {
		return false;
	}
	const double paid_units = safe_nonneg(cost) > budget
		? safe_nonneg(units) * (budget / safe_nonneg(cost)) : safe_nonneg(units);
	if (paid_units < 1.0) {
		return false;
	}
	CityGovProject& project = gov.projects[gov.project_count];
	project = CityGovProject{};
	project.kind = URBAN_PROJECT_SOCIAL_HOUSING;
	project.units = paid_units;
	project.cost = budget;
	project.reserved = budget;
	project.days_left = kProjectDaysSocialHousing;
	project.created_day = day;
	++gov.project_count;
	gov.cash = safe_nonneg(gov.cash) - budget;
	return true;
}

void tick(WorldState& ws, int day) {
	const std::size_t count = std::min({ws.cities.size(), ws.land.size(),
		ws.housing.size(), ws.city_gov.size()});
	if (count == 0) {
		return;  // cenário sintético/save antigo: sem estado urbano
	}
	advance_projects(ws, count);
	if (day % kMonthDays != 0) {
		return;
	}
	monthly_cash_flow(ws, count, day);
}

double money_total(const WorldState& ws) {
	double total = 0.0;
	for (const CityGov& gov : ws.city_gov) {
		total += safe_nonneg(gov.cash);
		for (int slot = 0; slot < gov.project_count; ++slot) {
			total += safe_nonneg(gov.projects[slot].reserved);
		}
	}
	return total;
}

void snapshot(const WorldState& ws, DemoSnapshot& out) {
	out.land = ws.land;
	out.city_gov = ws.city_gov;
}

void restore(WorldState& ws, const DemoSnapshot& in) {
	ws.land = in.land;
	ws.city_gov = in.city_gov;
}

} // namespace city_gov
} // namespace ogs
