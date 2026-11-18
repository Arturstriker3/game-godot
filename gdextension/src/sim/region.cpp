// region.cpp — economia da região (B3, estados inteiros). Módulo de domínio do
// interior: um bloco por cidade, simplificado. Ver region.h para o desenho.
//
// C++ puro (STL via sim_core.h), sem godot_cpp.

#include "sim_core.h"

#include "region.h"
#include "sim_constants.h"

#include <algorithm>
#include <cmath>

namespace ogs {

namespace {

constexpr double kRegionEps = 1e-12;

// Credita dinheiro no caixa da região, repartindo entre os quintis pela renda
// (o salário do primário vai para os quintis na proporção da renda).
void region_credit(Region& region, double amount) {
	const double value = std::max(0.0, amount);
	region.cash += value;
	if (!region.strata_ready || value <= kRegionEps) {
		return;
	}
	double share_total = 0.0;
	for (int s = 0; s < kIncomeStrata; ++s) {
		share_total += std::max(0.0, region.strata_income_share[s]);
	}
	if (share_total <= kRegionEps) {
		region.stratum_savings[0] += value;
		return;
	}
	double credited = 0.0;
	for (int s = 0; s < kIncomeStrata; ++s) {
		const double share = value
			* std::max(0.0, region.strata_income_share[s]) / share_total;
		region.stratum_savings[s] += share;
		credited += share;
	}
	region.stratum_savings[kIncomeStrata - 1] += value - credited;
}

// Debita dinheiro do caixa da região, repartindo pela poupança de cada quintil
// (quem tem mais, paga mais — espelho de charge_households).
void region_charge(Region& region, double amount) {
	const double value = std::max(0.0, amount);
	if (!region.strata_ready || value <= kRegionEps) {
		region.cash = std::max(0.0, region.cash - value);
		return;
	}
	double total = 0.0;
	for (int s = 0; s < kIncomeStrata; ++s) {
		total += std::max(0.0, region.stratum_savings[s]);
	}
	const double paid = std::min(value, total);
	region.cash = std::max(0.0, region.cash - paid);
	if (total <= kRegionEps) {
		return;
	}
	double charged = 0.0;
	for (int s = 0; s < kIncomeStrata; ++s) {
		const double share = paid
			* std::max(0.0, region.stratum_savings[s]) / total;
		region.stratum_savings[s] = std::max(0.0,
			region.stratum_savings[s] - share);
		charged += share;
	}
	if (charged < paid) {
		int top = 0;
		for (int s = 1; s < kIncomeStrata; ++s) {
			if (region.stratum_savings[s] > region.stratum_savings[top]) {
				top = s;
			}
		}
		region.stratum_savings[top] = std::max(0.0,
			region.stratum_savings[top] - (paid - charged));
	}
}

} // namespace

void SimCore::phase_region() {
	// Laço SERIAL (determinístico): a ordem das cidades é fixa. O interior de
	// uma cidade não toca o de outra (só lê o mercado da própria cidade), mas a
	// ordem fixa preserva a ordem de soma dos acumuladores globais.
	const int week_period = ws_.cadence.weekly_period;
	const int month_period = ws_.cadence.monthly_period;
	const int food_count = static_cast<int>(ws_.food_products.size());
	for (std::size_t city_index = 0; city_index < ws_.cities.size();
			++city_index) {
		City& city = ws_.cities[city_index];
		Region& region = city.region;
		if (!region.ready) {
			continue;  // sem dado de região = inerte
		}
		const double region_pop = std::max(0.0, region.region_pop);

		// --- Semanal (A6): emprego/salário/renda do setor primário. ---
		if (cadence_due(ws_.day, city_index, week_period)) {
			region.employment = region_pop * kRegionLaborShare
				* std::clamp(region.employment_mix[0], 0.0, 1.0);
			// Salário bruto da SEMANA: o primário (fazendas/minas/madeira) é o
			// empregador; desconto rural declarado. Limitado ao caixa da firma
			// (contraparte: débito no caixa do pagador, conservação).
			const double gross = region.employment * kFarmWage
				* kRegionRuralWageDiscount * double(week_period);
			const double paid = std::min(gross, std::max(0.0, city.firm_cash));
			region.wage_income = paid;
			region.taxes = paid * kTaxRate;
			region.income = std::max(0.0, paid - region.taxes);
			city.firm_cash = std::max(0.0, city.firm_cash - paid);
			city.government_budget += region.taxes;
			region_credit(region, region.income);
		}

		// --- Diário: consumo de comida no mercado da cidade-polo (sem frete). ---
		region.food_need = region_pop * kNeedPerCapita;
		region.food_spend = 0.0;
		region.food_consumed = 0.0;
		double budget = std::max(0.0, region.cash);
		double remaining_need = region.food_need;
		// Disponibilidade em t-eq por slot (mesma proporção da compra urbana).
		double avail[kMaxFoodProducts] = {};
		double total_avail = 0.0;
		for (int slot = 0; slot < food_count; ++slot) {
			const int product = food_product(slot);
			avail[slot] = city.markets[product].stock * food_calories(product);
			total_avail += avail[slot];
		}
		for (int slot = 0;
				slot < food_count && remaining_need > kRegionEps; ++slot) {
			const int product = food_product(slot);
			const double factor = food_calories(product);
			if (factor <= 0.0) {
				continue;
			}
			Market& market = city.markets[product];
			const double price = market.price;
			if (price <= 0.0) {
				continue;
			}
			double want_ge = total_avail > 0.0
				? remaining_need * (avail[slot] / total_avail)
				: (slot == 0 ? remaining_need : 0.0);
			want_ge = std::min(want_ge, remaining_need);
			double desired_tons = want_ge / factor;
			if (desired_tons * price > budget) {
				desired_tons = std::max(0.0, budget) / price;
			}
			if (desired_tons <= kRegionEps) {
				continue;
			}
			market.desired += desired_tons;
			const double quantity = std::min(desired_tons, market.stock);
			if (quantity <= 0.0) {
				continue;
			}
			const double paid_food = quantity * price;
			market.stock -= quantity;
			market.demand += quantity;
			market.sold += quantity;
			budget -= paid_food;
			region.food_spend += paid_food;
			region.food_consumed += quantity * factor;
			city.revenue_by_product[product] += paid_food;
			city.firm_cash += paid_food;
			ws_.consumed_total[product] += quantity;
			ws_.consumed_today[product] += quantity;
			remaining_need -= quantity * factor;
		}
		region.satisfaction = region.food_need > kRegionEps
			? std::clamp(region.food_consumed / region.food_need, 0.0, 1.0)
			: 1.0;
		// O dinheiro gasto sai do caixa da região (contraparte da compra).
		region_charge(region, region.food_spend);
		// Per-quintil: necessidade/custo atribuídos por pessoa (quintil = 20%).
		for (int s = 0; s < kIncomeStrata; ++s) {
			region.stratum_need[s] = region.food_need / double(kIncomeStrata);
			region.stratum_food_spend[s] =
				region.food_spend / double(kIncomeStrata);
			region.stratum_consumed[s] =
				region.food_consumed / double(kIncomeStrata);
		}

		// --- Mensal (A6): estratos/poupança. A população do interior é FIXA
		// nesta fase (a demografia regional é refinamento posterior, B4); o
		// fecho mensal fica reservado para a mobilidade/sucessão. ---
		if (cadence_due(ws_.day, city_index, month_period)) {
			// (nada por ora — reservado)
		}
	}
}

} // namespace ogs
