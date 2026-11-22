// informality.cpp — B7: informalidade, serviços informais, mortalidade e
// insatisfação. Módulo de domínio do núcleo.
//
// O DADO é a taxa de emprego informal por nação (informality.json) e a cobertura
// de saúde de referência (health_access.json) — definições estáticas no
// WorldState. O resto é MODELO DECLARADO (sim_constants.h). Nada aqui cria
// dinheiro nem pessoas: a renda informal renormaliza a folha já paga, o serviço
// informal desvia uma fatia do gasto das famílias (conservado) e a mortalidade
// modula a tabela base (com contador de excesso).
//
// C++ puro (STL via sim_core.h), sem godot_cpp.

#include "sim_core.h"

#include "sim_constants.h"
#include "sim_labor.h"

#include <algorithm>
#include <cmath>

namespace ogs {

namespace {

constexpr double kB7Eps = 1e-12;

// Índice de preço do serviço FORMAL da cidade: média dos preços dos setores
// ponderada pela demanda estrutural das famílias (pop_share). Proxy do custo do
// serviço formal; sem setor com peso/preço, devolve 0 (o chamador usa a âncora
// de `personal_services` escalada pelo salário local).
double city_formal_price_index(const WorldState& ws, const City& city) {
	double num = 0.0;
	double den = 0.0;
	for (int sector = 0; sector < kMaxServiceSectors; ++sector) {
		const ServiceSector& definition =
			ws.service_sectors[static_cast<std::size_t>(sector)];
		const double w = definition.pop_share[0] + definition.pop_share[1]
			+ definition.pop_share[2];
		if (w <= 0.0) {
			continue;
		}
		const double p = city.service_markets[sector].price;
		if (p <= 0.0) {
			continue;
		}
		num += p * w;
		den += w;
	}
	return den > kB7Eps ? num / den : 0.0;
}

// Âncora de preço de `personal_services` escalada pelo salário local (mesma
// régua do passo 8 de services_day), usada quando não há preço formal medido.
double personal_services_anchor(const WorldState& ws, const City& city) {
	for (int sector = 0; sector < kMaxServiceSectors; ++sector) {
		const ServiceSector& definition =
			ws.service_sectors[static_cast<std::size_t>(sector)];
		if (definition.id != "personal_services") {
			continue;
		}
		const double wage = std::max(0.0, city.service_wage);
		const double anchor_scale = std::clamp(
			wage / kServiceWage, kServiceAnchorScaleMin, kServiceAnchorScaleMax);
		return std::max(1e-6, definition.price * anchor_scale
			* kServiceMarginCalibration);
	}
	return 1.0;
}

// Reparte a fração informal inicial `base` entre os quintis pelo gradiente
// declarado: a média ponderada por população reproduz exatamente `base`.
void seed_informal_distribution(const double* people, double* share, double base) {
	double people_total = 0.0;
	for (int s = 0; s < kIncomeStrata; ++s) {
		people_total += std::max(0.0, people[s]);
	}
	if (people_total <= kB7Eps) {
		people_total = 1.0;
	}
	double weight_avg = 0.0;
	for (int s = 0; s < kIncomeStrata; ++s) {
		weight_avg += kInformalQuintileGradient[s]
			* std::max(0.0, people[s]) / people_total;
	}
	if (weight_avg <= kB7Eps) {
		weight_avg = 1.0;
	}
	for (int s = 0; s < kIncomeStrata; ++s) {
		share[s] = std::clamp(
			base * kInformalQuintileGradient[s] / weight_avg, 0.0, 1.0);
	}
}

// Cobertura de infra B5 da região (0..1), usada como driver da formalização.
double infra_coverage(const Region& region) {
	if (kInfraMaxLevel <= 0) {
		return 0.0;
	}
	const double covered = kInfraProductivityWeight[0]
		* (double(region.infra_road) / double(kInfraMaxLevel))
		+ kInfraProductivityWeight[1]
			* (double(region.infra_power) / double(kInfraMaxLevel))
		+ kInfraProductivityWeight[2]
			* (double(region.infra_water) / double(kInfraMaxLevel));
	return std::clamp(covered, 0.0, 1.0);
}

// Severidade de mortalidade (fome + saúde) a partir da satisfação alimentar e
// do gap de saúde. `food_gap^2` concentra o efeito de fome aguda.
double mortality_severity(double satisfaction, double health_ref,
		double health_access) {
	const double food_gap = std::clamp(
		(kFoodMortalityThreshold - satisfaction) / kFoodMortalityThreshold,
		0.0, 1.0);
	const double health_gap = std::clamp(
		1.0 - health_access / std::max(health_ref, 1e-9), 0.0, 1.0);
	return std::clamp(kFamineMortalityWeight * food_gap * food_gap
			+ kHealthMortalityWeight * health_gap, 0.0, kMortalityMaxExcess);
}

} // namespace

// ---------------------------------------------------------------------------
// Dado de entrada (informalidade e saúde) e semeadura.
// ---------------------------------------------------------------------------

double SimCore::nation_informal_base(const City& city) const {
	const auto found = ws_.informal_rates.find(city.nation);
	if (found != ws_.informal_rates.end()) {
		return std::clamp(found->second, 0.0, 1.0);
	}
	// Âncora de renda (ILOSTAT por PIB PPP): sector_anchor_values().informal.
	double anchors[3] = {0.0, 0.0, 0.0};
	sector_anchor_values(effective_productivity(city), anchors);
	return std::clamp(anchors[2], 0.0, 1.0);
}

double SimCore::nation_health_access(const City& city) const {
	const auto found = ws_.health_access.find(city.nation);
	if (found != ws_.health_access.end()) {
		return std::clamp(found->second, 0.0, 1.0);
	}
	return kHealthAccessDefault;
}

void SimCore::seed_informality(City& city) {
	// Cenário sintético (AGRIA/PORTUS, world_mode false) não tem dado de nação:
	// informalidade fica 0 (ausência explícita) e o comportamento legado é
	// preservado.
	if (!ws_.world_mode) {
		return;
	}
	const double base = nation_informal_base(city);
	const double health = nation_health_access(city);
	Pop& pop = city.pop;
	if (pop.strata_ready && !pop.informality_seeded) {
		seed_informal_distribution(pop.stratum_people,
			pop.informal_labor_share, base);
		pop.health_access_ref = health;
		pop.informality_seeded = true;
		refresh_informal_aggregate(pop);
	}
	Region& region = city.region;
	if (region.ready && region.strata_ready && !region.informality_seeded) {
		const double rbase = std::clamp(
			base * kRegionInformalRuralFactor, 0.0, 1.0);
		seed_informal_distribution(region.stratum_people,
			region.informal_labor_share, rbase);
		region.health_access_ref = health;
		region.informality_seeded = true;
		refresh_informal_aggregate(region);
	}
}

void SimCore::refresh_informal_aggregate(Pop& pop) const {
	double people_total = 0.0;
	double weighted = 0.0;
	for (int s = 0; s < kIncomeStrata; ++s) {
		people_total += std::max(0.0, pop.stratum_people[s]);
		weighted += pop.informal_labor_share[s]
			* std::max(0.0, pop.stratum_people[s]);
	}
	pop.informal_share = people_total > kB7Eps
		? std::clamp(weighted / people_total, 0.0, 1.0) : 0.0;
}

void SimCore::refresh_informal_aggregate(Region& region) const {
	double people_total = 0.0;
	double weighted = 0.0;
	for (int s = 0; s < kIncomeStrata; ++s) {
		people_total += std::max(0.0, region.stratum_people[s]);
		weighted += region.informal_labor_share[s]
			* std::max(0.0, region.stratum_people[s]);
	}
	region.informal_share = people_total > kB7Eps
		? std::clamp(weighted / people_total, 0.0, 1.0) : 0.0;
}

// ---------------------------------------------------------------------------
// Formalização mensal (cadência A6, mesmo dia do fechamento de estratos).
// ---------------------------------------------------------------------------

void SimCore::formalize_informality(City& city) {
	const double income_index = std::clamp(
		effective_productivity(city), 0.0, 1.0);
	const double infra = infra_coverage(city.region);
	Pop& pop = city.pop;
	if (pop.strata_ready) {
		for (int s = 0; s < kIncomeStrata; ++s) {
			// Escolaridade acima do básico do quintil (níveis 1 e 2).
			const double education_above = 1.0
				- pop.stratum_education_mix[s][0];
			const double driver = kFormalizationIncome * income_index
				+ kFormalizationEducation * education_above
				+ kFormalizationInfra * infra
				+ kFormalizationEnforcement;
			const double target = std::clamp(
				pop.informal_labor_share[s] * (1.0 - driver), 0.0, 1.0);
			pop.informal_labor_share[s] +=
				(target - pop.informal_labor_share[s])
				* kInformalFormalizationRate;
			pop.informal_labor_share[s] = std::clamp(
				pop.informal_labor_share[s], 0.0, 1.0);
		}
		refresh_informal_aggregate(pop);
	}
	Region& region = city.region;
	if (region.ready && region.strata_ready) {
		// O interior não tem escolaridade por quintil (bloco simplificado):
		// o driver de educação fica 0 (ausência explícita).
		for (int s = 0; s < kIncomeStrata; ++s) {
			const double driver = kFormalizationIncome * income_index
				+ kFormalizationInfra * infra
				+ kFormalizationEnforcement;
			const double target = std::clamp(
				region.informal_labor_share[s] * (1.0 - driver), 0.0, 1.0);
			region.informal_labor_share[s] +=
				(target - region.informal_labor_share[s])
				* kInformalFormalizationRate;
			region.informal_labor_share[s] = std::clamp(
				region.informal_labor_share[s], 0.0, 1.0);
		}
		refresh_informal_aggregate(region);
	}
}

// ---------------------------------------------------------------------------
// Serviços informais (agregado, sem firma individual).
// ---------------------------------------------------------------------------

double SimCore::compute_informal_services(City& city) {
	Pop& pop = city.pop;
	// Reseta os fluxos do dia (o bloco é recalculado a cada dia).
	pop.informal_service_workers = 0.0;
	pop.informal_service_output = 0.0;
	pop.informal_service_demand = 0.0;
	pop.informal_service_revenue = 0.0;
	pop.informal_service_price = 0.0;
	pop.informal_service_share = 0.0;
	pop.informal_wage_bill = 0.0;
	for (int s = 0; s < kIncomeStrata; ++s) {
		pop.informal_wage_bill += std::max(0.0, pop.informal_income[s]);
	}
	if (!pop.strata_ready || pop.labor_force <= 1e-9) {
		return 0.0;
	}
	double people_total = 0.0;
	for (int s = 0; s < kIncomeStrata; ++s) {
		people_total += std::max(0.0, pop.stratum_people[s]);
	}
	if (people_total <= kB7Eps) {
		return 0.0;
	}
	// Trabalhadores informais: força de trabalho repartida pelos quintis × a
	// fração informal de cada um × a participação do serviço informal.
	double workers = 0.0;
	for (int s = 0; s < kIncomeStrata; ++s) {
		workers += (pop.labor_force * std::max(0.0, pop.stratum_people[s])
			/ people_total) * pop.informal_labor_share[s];
	}
	workers *= kInformalServiceEmploymentShare;
	const double output = workers * kInformalServiceUnitsPerWorker
		* kInformalProductivityFactor;
	double formal_price = city_formal_price_index(ws_, city);
	if (formal_price <= 0.0) {
		formal_price = personal_services_anchor(ws_, city);
	}
	const double price = formal_price * kInformalPriceDiscount;
	// Demanda: fatia do gasto doméstico em serviços desviada para o informal.
	const double low_share = (std::max(0.0, pop.stratum_people[0])
			+ std::max(0.0, pop.stratum_people[1])) / people_total;
	const double unemployment_rate = pop.unemployed
		/ std::max(pop.labor_force, 1e-9);
	const double compete = kInformalDemandBase
		* std::pow(std::max(formal_price, 1e-9) / std::max(price, 1e-9),
			kInformalDemandPriceElasticity)
		* (1.0 + kInformalDemandUnemployment * unemployment_rate)
		* (0.5 + 0.5 * low_share);
	const double share = std::clamp(compete, 0.0, kInformalDemandMax);
	const double pool = std::max(0.0, pop.service_spend);
	const double demand_money = pool * share;
	const double capacity_money = output * price;
	const double revenue = std::min(demand_money, capacity_money);
	// O dinheiro realizado vai para os trabalhadores informais (crédito nos
	// quintis na proporção da renda informal) — NÃO entra em service_cash nem
	// em firm_cash. O restante do pool segue o caminho formal. Conservação:
	// service_cash desce e a poupança das famílias sobe na MESMA quantia, por
	// isso a receita realizada é limitada ao caixa do setor: creditar a receita
	// cheia com o débito truncado em zero criaria dinheiro.
	const double paid = std::min(revenue, std::max(0.0, city.service_cash));
	pop.informal_service_workers = workers;
	pop.informal_service_output = output;
	pop.informal_service_demand = price > 1e-9 ? demand_money / price : 0.0;
	pop.informal_service_revenue = paid;
	pop.informal_service_price = price;
	pop.informal_service_share = share;
	if (paid > kB7Eps) {
		double informal_income_total = 0.0;
		for (int s = 0; s < kIncomeStrata; ++s) {
			informal_income_total += std::max(0.0, pop.informal_income[s]);
		}
		if (informal_income_total > kB7Eps) {
			double credited = 0.0;
			for (int s = 0; s < kIncomeStrata - 1; ++s) {
				const double credit = paid * std::max(0.0, pop.informal_income[s])
					/ informal_income_total;
				pop.stratum_savings[s] += credit;
				credited += credit;
			}
			pop.stratum_savings[kIncomeStrata - 1] += paid - credited;
			pop.savings += paid;
		} else {
			credit_households(pop, paid);
		}
		city.service_cash = std::max(0.0, city.service_cash - paid);
	}
	return paid;
}

void SimCore::compute_region_informal_services(City& city) {
	Region& region = city.region;
	region.informal_service_workers = 0.0;
	region.informal_service_output = 0.0;
	region.informal_service_demand = 0.0;
	region.informal_service_revenue = 0.0;
	region.informal_service_price = 0.0;
	region.informal_service_share = 0.0;
	if (!region.ready || !region.strata_ready) {
		return;
	}
	const double region_pop = std::max(0.0, region.region_pop);
	// O interior não decompõe a força de trabalho por quintil: usa a
	// participação rural (kRegionLaborShare) como proxy da força de trabalho.
	const double workers = region_pop * kRegionLaborShare
		* region.informal_share * kInformalServiceEmploymentShare;
	const double output = workers * kInformalServiceUnitsPerWorker
		* kInformalProductivityFactor;
	double formal_price = city_formal_price_index(ws_, city);
	if (formal_price <= 0.0) {
		formal_price = personal_services_anchor(ws_, city);
	}
	const double price = formal_price * kInformalPriceDiscount;
	// Sem desemprego no interior (bloco simplificado): o termo de desemprego
	// fica neutro (1 + 0). Quintis do interior são 20% cada -> low_share 0,4.
	const double compete = kInformalDemandBase
		* std::pow(std::max(formal_price, 1e-9) / std::max(price, 1e-9),
			kInformalDemandPriceElasticity)
		* (0.5 + 0.5 * 0.4);
	const double share = std::clamp(compete, 0.0, kInformalDemandMax);
	// Pool = fração declarada da renda diária do interior (kServiceSpendShare),
	// paga da poupança dos quintis do interior e creditada a eles mesmos como
	// renda informal (o caixa agregado da região não muda).
	const double pool = std::max(0.0, region.income) * kServiceSpendShare;
	const double demand_money = pool * share;
	const double capacity_money = output * price;
	// A compra é LIMITADA à capacidade de pagamento do interior: sem dinheiro
	// em conta, o quintil não compra serviço informal. Sem esse teto a receita
	// creditada (revenue) passava do débito efetivado (payable) e o serviço
	// informal CRIAVA dinheiro — drift de centenas de unidades no fechamento
	// mensal e, com IA, o tick revertido pelo invariante (dia travado).
	double total_savings = 0.0;
	for (int s = 0; s < kIncomeStrata; ++s) {
		total_savings += std::max(0.0, region.stratum_savings[s]);
	}
	const double revenue = std::min(std::min(demand_money, capacity_money),
		total_savings);
	region.informal_service_workers = workers;
	region.informal_service_output = output;
	region.informal_service_demand = price > 1e-9 ? demand_money / price : 0.0;
	region.informal_service_revenue = revenue;
	region.informal_service_price = price;
	region.informal_service_share = share;
	if (revenue > kB7Eps) {
		// Redistribuição DENTRO do interior (conserva dinheiro): o pagamento do
		// serviço sai da poupança geral dos quintis (consumidores) e a renda vai
		// para os trabalhadores informais (rateado pela renda informal). O caixa
		// agregado (region.cash = Σ poupança) não muda.
		// O débito de cada quintil é proporcional à poupança dele, com o resíduo
		// do arredondamento no último e limitado ao que ele tem. O crédito é
		// rateado sobre EXATAMENTE o total debitado — nem um centavo a mais.
		double informal_income_total = 0.0;
		for (int s = 0; s < kIncomeStrata; ++s) {
			informal_income_total += std::max(0.0, region.informal_income[s]);
		}
		double debited = 0.0;
		for (int s = 0; s < kIncomeStrata; ++s) {
			const double savings = std::max(0.0, region.stratum_savings[s]);
			const double proportional = total_savings > kB7Eps
				? revenue * savings / total_savings : 0.0;
			const double debit = std::clamp(
				s == kIncomeStrata - 1 ? revenue - debited : proportional,
				0.0, savings);
			region.stratum_savings[s] = savings - debit;
			debited += debit;
		}
		double credited = 0.0;
		for (int s = 0; s < kIncomeStrata - 1; ++s) {
			const double credit = informal_income_total > kB7Eps
				? debited * std::max(0.0, region.informal_income[s])
					/ informal_income_total
				: debited / double(kIncomeStrata);
			region.stratum_savings[s] += credit;
			credited += credit;
		}
		region.stratum_savings[kIncomeStrata - 1] += debited - credited;
		// O caixa agregado é a soma dos quintis (invariante B3).
		region.cash = 0.0;
		for (int s = 0; s < kIncomeStrata; ++s) {
			region.cash += std::max(0.0, region.stratum_savings[s]);
		}
	}
}

// ---------------------------------------------------------------------------
// Mortalidade mensal (fome + saúde).
// ---------------------------------------------------------------------------

double SimCore::mortality_month(Pop& pop, const City& city,
		double* death_band) const {
	for (int band = 0; band < kAgeBands; ++band) {
		death_band[band] = 0.0;
	}
	const auto found = ws_.age_structures.find(city.nation);
	if (found == ws_.age_structures.end() || !found->second.valid
			|| !pop.demography_ready) {
		return 0.0;
	}
	const AgeStructure& structure = found->second;
	double total = 0.0;
	for (int band = 0; band < kAgeBands; ++band) {
		total += std::max(0.0, pop.age_band[band]);
	}
	if (total <= 0.0) {
		return 0.0;
	}
	const double base_deaths = total
		* structure.deaths_per_1000 / 1000.0 / 12.0;
	const double severity = mortality_severity(
		std::clamp(pop.satisfaction_prev, 0.0, 1.0),
		std::max(pop.health_access_ref, 1e-9), nation_health_access(city));
	const double mult = 1.0 + severity;
	// Repartição por faixa com vulnerabilidade (crianças 0-14 e idosos 65+
	// pesam mais).
	double shape[kAgeBands];
	double shape_total = 0.0;
	for (int band = 0; band < kAgeBands; ++band) {
		shape[band] = std::max(0.0, pop.age_band[band])
			* kAgeDeathWeight[band]
			* (1.0 + severity * kAgeMortalityVulnerability[band]);
		shape_total += shape[band];
	}
	for (int band = 0; band < kAgeBands; ++band) {
		death_band[band] = shape_total > 0.0
			? base_deaths * mult * shape[band] / shape_total
			: base_deaths * mult / double(kAgeBands);
	}
	const double famine_severity = std::clamp(
		kFamineMortalityWeight
			* std::pow(std::clamp((kFoodMortalityThreshold
				- std::clamp(pop.satisfaction_prev, 0.0, 1.0))
				/ kFoodMortalityThreshold, 0.0, 1.0), 2.0),
		0.0, kMortalityMaxExcess);
	pop.deaths_expected = base_deaths;
	pop.deaths_total = base_deaths * mult;
	pop.excess_deaths_total += base_deaths * severity;
	pop.excess_deaths_famine_total += base_deaths * famine_severity;
	if (std::clamp(pop.satisfaction_prev, 0.0, 1.0) < kFoodMortalityThreshold) {
		pop.famine_months += 1;
	} else {
		pop.famine_months = 0;
	}
	return base_deaths;
}

double SimCore::region_mortality_month(Region& region, const City& city,
		double* death_band) const {
	for (int band = 0; band < kAgeBands; ++band) {
		death_band[band] = 0.0;
	}
	const auto found = ws_.age_structures.find(city.nation);
	if (found == ws_.age_structures.end() || !found->second.valid
			|| !region.demography_ready) {
		return 0.0;
	}
	const AgeStructure& structure = found->second;
	double total = 0.0;
	for (int band = 0; band < kAgeBands; ++band) {
		total += std::max(0.0, region.age_band[band]);
	}
	if (total <= 0.0) {
		return 0.0;
	}
	const double base_deaths = total
		* structure.deaths_per_1000 / 1000.0 / 12.0;
	const double severity = mortality_severity(
		std::clamp(region.satisfaction, 0.0, 1.0),
		std::max(region.health_access_ref, 1e-9), nation_health_access(city));
	const double mult = 1.0 + severity;
	double shape[kAgeBands];
	double shape_total = 0.0;
	for (int band = 0; band < kAgeBands; ++band) {
		shape[band] = std::max(0.0, region.age_band[band])
			* kAgeDeathWeight[band]
			* (1.0 + severity * kAgeMortalityVulnerability[band]);
		shape_total += shape[band];
	}
	for (int band = 0; band < kAgeBands; ++band) {
		death_band[band] = shape_total > 0.0
			? base_deaths * mult * shape[band] / shape_total
			: base_deaths * mult / double(kAgeBands);
	}
	const double famine_severity = std::clamp(
		kFamineMortalityWeight
			* std::pow(std::clamp((kFoodMortalityThreshold
				- std::clamp(region.satisfaction, 0.0, 1.0))
				/ kFoodMortalityThreshold, 0.0, 1.0), 2.0),
		0.0, kMortalityMaxExcess);
	region.deaths_expected = base_deaths;
	region.deaths_total = base_deaths * mult;
	region.excess_deaths_total += base_deaths * severity;
	region.excess_deaths_famine_total += base_deaths * famine_severity;
	if (std::clamp(region.satisfaction, 0.0, 1.0) < kFoodMortalityThreshold) {
		region.famine_months += 1;
	} else {
		region.famine_months = 0;
	}
	return base_deaths;
}

// ---------------------------------------------------------------------------
// Insatisfação (medição 0..1) e fase social.
// ---------------------------------------------------------------------------

void SimCore::compute_dissatisfaction(City& city) {
	// Normalização POR BLOCO: cada bloco (urbano e interior) divide pela soma
	// dos pesos que ele APLICA de fato. Normalizar o interior pelo total (1,00)
	// o capava em 0,50 (só comida+inflação+informalidade = 0,50) e o índice não
	// alcançava 1,0 no cenário extremo. Terra (kDissatisfactionLand) segue como
	// gancho B6 (peso 0) e não entra em nenhum dos dois blocos hoje.
	const double urban_weight_sum = kDissatisfactionUnemployment
		+ kDissatisfactionRent + kDissatisfactionFood
		+ kDissatisfactionInflation + kDissatisfactionInformality
		+ kDissatisfactionPower;
	const double urban_weight_div = urban_weight_sum > kB7Eps
		? urban_weight_sum : 1.0;
	const double region_weight_sum = kDissatisfactionFood
		+ kDissatisfactionInflation + kDissatisfactionInformality
		+ kDissatisfactionPower;
	const double region_weight_div = region_weight_sum > kB7Eps
		? region_weight_sum : 1.0;

	// Urbano.
	{
		Pop& pop = city.pop;
		const double power_stress = std::clamp(city.power_stress, 0.0, 1.0);
		double people_total = 0.0;
		for (int s = 0; s < kIncomeStrata; ++s) {
			people_total += std::max(0.0, pop.stratum_people[s]);
		}
		const double unemployment_rate = pop.unemployed
			/ std::max(pop.labor_force, 1e-9);
		double youth_share = 0.0;
		if (pop.demography_ready) {
			youth_share = std::clamp(pop.age_band[1]
				/ std::max(pop.labor_force, 1e-9), 0.0, 1.0);
		}
		const double u = unemployment_rate
			* (1.0 + kDissatisfactionYouthWeight * youth_share);
		const double infl = std::clamp(
			std::max(pop.food_inflation_month, 0.0), 0.0, 1.0);
		double agg = 0.0;
		double comp_unemployment = 0.0;
		double comp_rent = 0.0;
		double comp_food = 0.0;
		double comp_inflation = 0.0;
		double comp_informality = 0.0;
		for (int s = 0; s < kIncomeStrata; ++s) {
			const double rent = std::clamp(pop.stratum_rent_paid[s]
				/ std::max(pop.stratum_net_income[s], 1e-9), 0.0, 1.0);
			const double food = std::clamp(1.0 - pop.stratum_consumed[s]
				/ std::max(pop.stratum_need[s], 1e-9), 0.0, 1.0);
			const double inf = std::clamp(pop.informal_labor_share[s], 0.0, 1.0);
			const double idx = std::clamp(
				(kDissatisfactionUnemployment * u
					+ kDissatisfactionRent * rent
					+ kDissatisfactionFood * food
					+ kDissatisfactionInflation * infl
					+ kDissatisfactionInformality * inf
					+ kDissatisfactionPower * power_stress) / urban_weight_div,
				0.0, 1.0);
			pop.dissatisfaction_by_stratum[s] = idx;
			const double people = std::max(0.0, pop.stratum_people[s]);
			agg += idx * people;
			comp_unemployment += kDissatisfactionUnemployment * u / urban_weight_div
				* people;
			comp_rent += kDissatisfactionRent * rent / urban_weight_div * people;
			comp_food += kDissatisfactionFood * food / urban_weight_div * people;
			comp_inflation += kDissatisfactionInflation * infl / urban_weight_div
				* people;
			comp_informality += kDissatisfactionInformality * inf / urban_weight_div
				* people;
		}
		pop.dissatisfaction = people_total > kB7Eps
			? std::clamp(agg / people_total, 0.0, 1.0) : 0.0;
		pop.dissatisfaction_unemployment = people_total > kB7Eps
			? comp_unemployment / people_total : 0.0;
		pop.dissatisfaction_rent = people_total > kB7Eps
			? comp_rent / people_total : 0.0;
		pop.dissatisfaction_food = people_total > kB7Eps
			? comp_food / people_total : 0.0;
		pop.dissatisfaction_inflation = people_total > kB7Eps
			? comp_inflation / people_total : 0.0;
		pop.dissatisfaction_informality = people_total > kB7Eps
			? comp_informality / people_total : 0.0;
	}

	// Interior.
	{
		Region& region = city.region;
		const double power_stress = std::clamp(city.power_stress, 0.0, 1.0);
		if (!region.ready || !region.strata_ready) {
			return;
		}
		double people_total = 0.0;
		for (int s = 0; s < kIncomeStrata; ++s) {
			people_total += std::max(0.0, region.stratum_people[s]);
		}
		const double infl = std::clamp(
			std::max(region.food_inflation_month, 0.0), 0.0, 1.0);
		double agg = 0.0;
		double comp_food = 0.0;
		double comp_inflation = 0.0;
		double comp_informality = 0.0;
		for (int s = 0; s < kIncomeStrata; ++s) {
			const double food = std::clamp(1.0 - region.stratum_consumed[s]
				/ std::max(region.stratum_need[s], 1e-9), 0.0, 1.0);
			const double inf = std::clamp(region.informal_labor_share[s], 0.0, 1.0);
			const double idx = std::clamp(
				(kDissatisfactionFood * food
					+ kDissatisfactionInflation * infl
					+ kDissatisfactionInformality * inf
					+ kDissatisfactionPower * power_stress) / region_weight_div,
				0.0, 1.0);
			region.dissatisfaction_by_stratum[s] = idx;
			const double people = std::max(0.0, region.stratum_people[s]);
			agg += idx * people;
			comp_food += kDissatisfactionFood * food / region_weight_div * people;
			comp_inflation += kDissatisfactionInflation * infl / region_weight_div
				* people;
			comp_informality += kDissatisfactionInformality * inf / region_weight_div
				* people;
		}
		region.dissatisfaction = people_total > kB7Eps
			? std::clamp(agg / people_total, 0.0, 1.0) : 0.0;
		region.dissatisfaction_unemployment = 0.0;
		region.dissatisfaction_rent = 0.0;
		region.dissatisfaction_food = people_total > kB7Eps
			? comp_food / people_total : 0.0;
		region.dissatisfaction_inflation = people_total > kB7Eps
			? comp_inflation / people_total : 0.0;
		region.dissatisfaction_informality = people_total > kB7Eps
			? comp_informality / people_total : 0.0;
	}
}

void SimCore::phase_social() {
	// Insatisfação é MEDIÇÃO pura (não move dinheiro nem pessoas): laço SERIAL
	// para a ordem de soma ficar determinística (não há acumulador global, mas
	// a ordem fixa preserva o determinismo como nas demais fases mensais).
	for (std::size_t city_index = 0; city_index < ws_.cities.size();
			++city_index) {
		compute_dissatisfaction(ws_.cities[city_index]);
	}
}

} // namespace ogs
