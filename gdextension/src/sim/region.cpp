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

// Demografia do interior (MENSAL, cadência A6): nascimentos, mortes e
// envelhecimento, reaproveitando B1 (age_structures da nação, kAgeDeathWeight e
// as mesmas taxas de envelhecimento 1/15,1/10,1/20,1/20). B7: as mortes são
// MODULADAS por fome/saúde. Atualiza region_pop, os quintis (20% cada) e
// devolve para o chamador reconciliar a cidade.
void region_demography_month(WorldState& ws, SimCore& core, City& city) {
	Region& region = city.region;
	if (!region.ready || !region.demography_ready) {
		return;
	}
	const auto found = ws.age_structures.find(city.nation);
	if (found == ws.age_structures.end() || !found->second.valid) {
		return;
	}
	const AgeStructure& structure = found->second;
	double band[kAgeBands];
	double total = 0.0;
	for (int index = 0; index < kAgeBands; ++index) {
		band[index] = std::max(0.0, region.age_band[index]);
		total += band[index];
	}
	if (total <= 0.0) {
		return;
	}
	// Fluxo do MÊS: nascimentos (tabela) e mortes MODULADAS por fome/saúde (B7).
	// A satisfação do interior é a FRESCA (a demografia roda depois do consumo
	// diário, no mesmo dia da cadência).
	const double births = total * structure.births_per_1000 / 1000.0 / 12.0;
	double death_band[kAgeBands] = {};
	core.region_mortality_month(region, city, death_band);
	// Envelhecimento: fração anual da faixa dividida por 12 (a última faixa não
	// tem para onde sair).
	double aging[kAgeBands] = {};
	for (int index = 0; index + 1 < kAgeBands; ++index) {
		aging[index] = band[index] * kAgeAgingAnnual[index] / 12.0;
	}
	double next[kAgeBands];
	next[0] = band[0] + births - death_band[0] - aging[0];
	for (int index = 1; index + 1 < kAgeBands; ++index) {
		next[index] = band[index] + aging[index - 1] - death_band[index]
			- aging[index];
	}
	next[kAgeBands - 1] = band[kAgeBands - 1] + aging[kAgeBands - 2]
		- death_band[kAgeBands - 1];
	double new_total = 0.0;
	for (int index = 0; index < kAgeBands; ++index) {
		next[index] = std::max(0.0, next[index]);
		new_total += next[index];
	}
	if (new_total <= 0.0) {
		return;
	}
	for (int index = 0; index < kAgeBands; ++index) {
		region.age_band[index] = next[index];
	}
	region.region_pop = new_total;
	// Quintis acompanham a população (20% cada); a poupança/caixa não muda.
	for (int s = 0; s < kIncomeStrata; ++s) {
		region.stratum_people[s] = new_total / double(kIncomeStrata);
	}
}

} // namespace

// ---------------------------------------------------------------------------
// Caixa da região (B3): crédito reparte pela RENDA dos quintis, débito pela
// POUPANÇA. Públicos desde a B6 (o aluguel do anel usa a mesma repartição, com
// o par débito/crédito somando zero — dinheiro conservado).
// ---------------------------------------------------------------------------
void SimCore::region_credit(Region& region, double amount) {
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

void SimCore::region_charge(Region& region, double amount) {
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

// ---------------------------------------------------------------------------
// B5 — infraestrutura da região (estrada, energia, água): multiplicadores
// derivados. Cobertura de cada tipo = nível/máximo (0..1); o nível é salvo
// (save v21) e os multiplicadores são recalculados a cada uso (não persistidos).
// ---------------------------------------------------------------------------
int SimCore::infra_total_level(const Region& region) {
	return region.infra_road + region.infra_power + region.infra_water;
}

double SimCore::infra_access_multiplier(const Region& region,
		double penalty_max) {
	// Custo de acesso: penalidade máxima com estrada 0, zero na máxima. O
	// frete/overhead é pago ao setor de transporte (não cria dinheiro).
	if (kInfraMaxLevel <= 0) {
		return 1.0;
	}
	const double cover = std::clamp(
		double(region.infra_road) / double(kInfraMaxLevel), 0.0, 1.0);
	return 1.0 + std::max(0.0, penalty_max) * (1.0 - cover);
}

double SimCore::infra_productivity_multiplier(const Region& region) {
	if (kInfraMaxLevel <= 0) {
		return 1.0;
	}
	const int level[kInfraTypeCount] = {
		region.infra_road, region.infra_power, region.infra_water};
	double covered = 0.0;
	for (int t = 0; t < kInfraTypeCount; ++t) {
		covered += kInfraProductivityWeight[t]
			* (double(level[t]) / double(kInfraMaxLevel));
	}
	return 1.0 + kInfraProductivityGain * std::clamp(covered, 0.0, 1.0);
}

double SimCore::infra_exodus_multiplier(const Region& region) {
	if (kInfraMaxLevel <= 0) {
		return 1.0;
	}
	const int level[kInfraTypeCount] = {
		region.infra_road, region.infra_power, region.infra_water};
	double covered = 0.0;
	for (int t = 0; t < kInfraTypeCount; ++t) {
		covered += kInfraExodusWeight[t]
			* (double(level[t]) / double(kInfraMaxLevel));
	}
	// Sem infra, expulsa mais (1 + boost); com cobertura total, expulsa na base.
	return 1.0 + kInfraExodusBoostMax * (1.0 - std::clamp(covered, 0.0, 1.0));
}

int SimCore::infra_level(const Region& region, int type) {
	switch (type) {
		case 0: return region.infra_road;
		case 1: return region.infra_power;
		case 2: return region.infra_water;
		default: return -1;
	}
}

int SimCore::infra_level_up(Region& region, int type) {
	int* target = nullptr;
	switch (type) {
		case 0: target = &region.infra_road; break;
		case 1: target = &region.infra_power; break;
		case 2: target = &region.infra_water; break;
		default: return -1;
	}
	*target = std::min(kInfraMaxLevel, *target + 1);
	return *target;
}

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
			// B5: a produtividade do trabalho primário escala com a infra da
			// região (estrada/luz/água, pesos declarados) — salário maior com
			// infra melhor, pago do MESMO caixa da firma (dinheiro conservado).
			const double gross = region.employment * kFarmWage
				* kRegionRuralWageDiscount * double(week_period)
				* infra_productivity_multiplier(region);
			const double paid = std::min(gross, std::max(0.0, city.firm_liquidity()));
			// C1: o que o caixa da firma não cobriu do salário rural é SALÁRIO
			// ATRASADO — passivo das firmas da cidade-polo e ativo das famílias
			// do interior. Vale a regra do período corrente (o não pago da
			// semana substitui o anterior). O pagamento não muda.
			region.arrears_receivable = std::max(0.0, gross - paid);
			city.rural_wage_arrears = region.arrears_receivable;
			const double tax = paid * kTaxRate;
			const double net = std::max(0.0, paid - tax);
			city.firm_pay(paid, City::kR1Wages);
			city.government_budget += tax;
			region_credit(region, net);
			// Publicação em base DIÁRIA (o salário é pago por semana; o status
			// expõe o equivalente diário, com o bruto semanal explícito).
			const double days = double(std::max(1, week_period));
			region.wage_income = paid / days;
			region.taxes = tax / days;
			region.income = net / days;
		}

		// --- Diário: consumo de comida no mercado da cidade-polo. B5: o custo
		// de ACESSO (frete/overhead da estrada) encarece o que a região compra;
		// o overhead vai ao setor de transporte (dinheiro conservado), o preço
		// do mercado continua indo à firma. ---
		const double access = infra_access_multiplier(
			region, ws_.infra_access_penalty);
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
			// B5: o preço efetivo para a região inclui o overhead de acesso
			// (frete da estrada). A firma recebe o preço do mercado; a diferença
			// (frete) vai ao setor de transporte — dinheiro conservado.
			const double base_price = market.price;
			const double price = base_price <= 0.0
				? 0.0 : base_price * access;
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
			// D-091: a comida da região é financiada (desired_tons limitado pelo
			// orçamento), então entra integral na pressão de preço.
			market.effective += desired_tons;
			const double quantity = std::min(desired_tons, market.stock);
			if (quantity <= 0.0) {
				continue;
			}
			const double paid_food = quantity * price;
			const double firm_share = quantity * base_price;
			const double freight = std::max(0.0, paid_food - firm_share);
			market.stock -= quantity;
			market.demand += quantity;
			market.sold += quantity;
			budget -= paid_food;
			region.food_spend += paid_food;
			region.food_consumed += quantity * factor;
			city.revenue_by_product[product] += firm_share;
			city.firm_receive(firm_share);
			ws_.transport_cash += freight;
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

		// --- Diário: manutenção da infra da região (B5). Despesa NACIONAL
		// (debita o caixa do governo, a prefeitura não paga) paga ao setor de
		// serviços/construção — dinheiro conservado. ---
		region.infra_maintenance = kInfraMaintenancePerCapitaPerLevelPerDay
			* double(infra_total_level(region)) * region_pop;
		if (region.infra_maintenance > kRegionEps) {
			const double paid = std::min(region.infra_maintenance,
				std::max(0.0, city.government_budget));
			city.government_budget = std::max(0.0,
				city.government_budget - paid);
			city.service_receive(paid);
		}

		// --- Diário: serviços informais do interior (B7, agregado sem firma).
		// Desvia uma fatia da renda diária do interior para os quintis informais
		// (caixa conservado). ---
		compute_region_informal_services(city);

		// --- Mensal (A6): demografia do interior (nascimentos/mortes/
		// envelhecimento, reaproveitando B1; mortes moduladas por fome/saúde na
		// B7), formalização do trabalho informal e fecho dos quintis. ---
		if (cadence_due(ws_.day, city_index, month_period)) {
			// B7: inflação de comida do interior (custo = gasto/consumido);
			// o primeiro fechamento só grava a referência.
			const double unit_cost = region.food_consumed > kRegionEps
				? region.food_spend / region.food_consumed : 0.0;
			if (region.food_price_ref <= 0.0) {
				region.food_price_ref = unit_cost;
				region.food_inflation_month = 0.0;
			} else if (region.food_price_ref > 0.0) {
				region.food_inflation_month = unit_cost
					/ region.food_price_ref - 1.0;
				region.food_price_ref = unit_cost;
			}
			region_demography_month(ws_, *this, city);
			reconcile_city_population(city);
		}
	}
}

} // namespace ogs
