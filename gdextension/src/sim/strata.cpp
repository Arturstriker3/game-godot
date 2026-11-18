// Módulo de domínio: estratos de renda com patrimônio (B2).
//
// A POP é cinco QUINTIS de renda por cidade, numa dimensão separada da
// escolaridade (3 níveis). A escolaridade define o acesso à vaga; o quintil
// define a renda de capital/dispersão salarial, o consumo, a poupança, a
// dívida (0 até a Parte C), o patrimônio e a posse de moradia.
//
// O que é DADO (`data/definitions/economy/inequality.json`, lido pelo
// GeographyService): os quintis de renda observados por nação (World Bank
// SI.DST, 154/179) e o Gini (SI.POV.GINI). O que é MODELO DECLARADO
// (sim_constants.h): o gradiente de escolaridade por quintil, a dispersão
// salarial dentro do nível, a concentração da riqueza, as taxas de poupança
// por quintil e por faixa etária e a mobilidade. Sem dado por nação, a
// repartição inicial é a proxy lognormal do Gini declarado (mesma família
// usada pela F7).
//
// O Gini do dia 365 NÃO é ancorado: ele é medido das rendas efetivamente pagas
// (salário por escolaridade × dispersão, capital/aluguel pelos donos,
// transferência per capita). Ver `docs/economy/B2_strata.md`.
//
// C++ puro (STL via sim_core.h), sem godot_cpp.

#include "sim_core.h"

#include "sim_constants.h"
#include "sim_labor.h"
#include "urban_types.h"

#include <algorithm>
#include <cmath>

namespace ogs {

namespace {

constexpr double kStrataEps = 1e-9;

// Padrão de moradia de cada quintil (liga ao estoque/dado do §19): os dois
// quintis de baixo moram no popular, os dois do meio no médio, o topo no alto.
constexpr int kStrataTier[kIncomeStrata] = {
	TIER_POPULAR, TIER_POPULAR, TIER_MEDIO, TIER_MEDIO, TIER_ALTO};

// Escolaridade da população em idade ativa de cada quintil (soma 1). MODELO:
// o gradiente de conclusão por renda (educação explica parte — não toda — da
// desigualdade de renda; o resto é dispersão dentro do nível). A dispersão
// salarial é calibrada nos quintis OBSERVADOS pela `seed_strata`.
constexpr double kStrataEducation[kIncomeStrata][kEducationLevels] = {
	{0.55, 0.33, 0.12},
	{0.45, 0.38, 0.17},
	{0.30, 0.42, 0.28},
	{0.18, 0.40, 0.42},
	{0.08, 0.32, 0.60},
};

double safe_nonneg(double value) {
	return std::max(0.0, std::isfinite(value) ? value : 0.0);
}

void normalize(double* values, int count) {
	double total = 0.0;
	for (int i = 0; i < count; ++i) {
		values[i] = safe_nonneg(values[i]);
		total += values[i];
	}
	if (total <= kStrataEps) {
		for (int i = 0; i < count; ++i) {
			values[i] = 1.0 / double(count);
		}
		return;
	}
	for (int i = 0; i < count; ++i) {
		values[i] /= total;
	}
}

} // namespace

// ---------------------------------------------------------------------------
// Semeadura
// ---------------------------------------------------------------------------
// `shares` = quintis de renda OBSERVADOS (fração da renda total, soma ~1) ou
// `nullptr`. A população de cada quintil é 20% (quintil = 20% de pessoas by
// definition); o dado entra na repartição da RENDA, não no tamanho do grupo.
void SimCore::seed_strata(City& city, const double* shares) {
	Pop& pop = city.pop;
	// B2 (correção): registra a ORIGEM da repartição. `set_income_gini` só
	// re-semeia a proxy; quintis observados nunca são sobrescritos por ela.
	pop.strata_from_data = shares != nullptr;
	double income_share[kIncomeStrata];
	if (shares != nullptr) {
		for (int s = 0; s < kIncomeStrata; ++s) {
			income_share[s] = safe_nonneg(shares[s]);
		}
		double sum = 0.0;
		for (int s = 0; s < kIncomeStrata; ++s) {
			sum += income_share[s];
		}
		if (sum <= kStrataEps) {
			for (int s = 0; s < kIncomeStrata; ++s) {
				income_share[s] = 1.0 / double(kIncomeStrata);
			}
		} else {
			for (int s = 0; s < kIncomeStrata; ++s) {
				income_share[s] /= sum;
			}
		}
	} else {
		// Proxy lognormal do Gini declarado: sigma do Gini e a fração da renda
		// de cada quintil pela integral da lognormal entre os quantis. Sem Gini
		// (cidade sintética): quintis iguais — ausência explícita.
		const double gini = std::clamp(city.income_gini, 0.0, 1.0);
		if (gini <= kStrataEps) {
			for (int s = 0; s < kIncomeStrata; ++s) {
				income_share[s] = 1.0 / double(kIncomeStrata);
			}
		} else {
			double sigma = std::sqrt(2.0) * normal_quantile(0.5 * (1.0 + gini));
			sigma = std::clamp(sigma, 0.15, 1.50);
			// Média 1 em unidades relativas (μ = −σ²/2 não entra na fração de
			// cada quintil: as duas pontas se cancelam na normalização).
			double previous = 0.0;
			for (int s = 0; s < kIncomeStrata; ++s) {
				const double upper = double(s + 1) / double(kIncomeStrata);
				// E[X · 1{q_prev<X<Q_next}] / E[X] = Φ(z_next − σ) − Φ(z_prev − σ),
				// com z = Φ⁻¹(q) porque x_q = exp(μ + σ·z). A versão anterior
				// tomava log(Φ⁻¹(q)) — Φ⁻¹ é negativo para q < 0,5 — e devolvia
				// quintis 0,0 nas duas metades pobres (Q5 = 73% com Gini 0,41).
				const double z_prev = normal_quantile(previous);
				const double z_next = normal_quantile(upper);
				income_share[s] = std::max(0.0,
					normal_cdf(z_next - sigma) - normal_cdf(z_prev - sigma));
				previous = upper;
			}
			normalize(income_share, kIncomeStrata);
		}
	}
	for (int s = 0; s < kIncomeStrata; ++s) {
		pop.strata_income_share[s] = income_share[s];
	}
	// População: 20% cada; se a cidade já tem população, ela manda.
	rescale_strata_population(pop);
	// Composição educacional, dispersão salarial (calibrada nos quintis) e
	// participação no capital (riqueza mais concentrada que a renda).
	double wage_weight[kIncomeStrata] = {};
	for (int s = 0; s < kIncomeStrata; ++s) {
		double mix[kEducationLevels];
		double mix_sum = 0.0;
		for (int e = 0; e < kEducationLevels; ++e) {
			pop.stratum_education_mix[s][e] =
				kStrataEducation[s][e];
			mix[e] = pop.stratum_education_mix[s][e];
			mix_sum += mix[e];
		}
		normalize(mix, kEducationLevels);
		for (int e = 0; e < kEducationLevels; ++e) {
			pop.stratum_education_mix[s][e] = mix[e];
		}
		// Peso salarial relativo da composição (prêmio por escolaridade do
		// próprio modelo; o nível absoluto vem do mercado de trabalho).
		double w = 0.0;
		for (int e = 0; e < kEducationLevels; ++e) {
			w += mix[e] * kLaborWageRatio[e];
		}
		wage_weight[s] = std::max(w, kStrataEps);
	}
	// Fração da renda que o capital/aluguel deve preencher (modelo). A renda
	// salarial semeada é o resto; `wage_factor` é a dispersão DENTRO do nível
	// que reproduz os quintis observados, normalizada para média 1.
	double wage_target[kIncomeStrata];
	double wage_target_sum = 0.0;
	for (int s = 0; s < kIncomeStrata; ++s) {
		const double capital_share = std::clamp(
			kStrataCapitalIncomeShare, 0.0, 0.9);
		wage_target[s] = income_share[s] * (1.0 - capital_share);
		wage_target_sum += wage_target[s];
	}
	if (wage_target_sum > kStrataEps) {
		for (int s = 0; s < kIncomeStrata; ++s) {
			wage_target[s] /= wage_target_sum;
		}
	}
	double scale = 0.0;
	for (int s = 0; s < kIncomeStrata; ++s) {
		scale += wage_target[s] / wage_weight[s];
	}
	const double u = scale > kStrataEps ? 1.0 / scale : 1.0;
	for (int s = 0; s < kIncomeStrata; ++s) {
		pop.stratum_wage_factor[s] = std::clamp(
			wage_target[s] * u / (1.0 / double(kIncomeStrata)) / wage_weight[s],
			0.02, 60.0);
	}
	// Capital/aluguel: participação âˆ renda^expoente (riqueza mais concentrada).
	double capital_weight[kIncomeStrata];
	for (int s = 0; s < kIncomeStrata; ++s) {
		capital_weight[s] = std::pow(std::max(income_share[s], 1e-6),
			kStrataWealthConcentration);
	}
	normalize(capital_weight, kIncomeStrata);
	for (int s = 0; s < kIncomeStrata; ++s) {
		pop.stratum_capital_share[s] = capital_weight[s];
	}
	// Posse de moradia: do dado §19 por padrão (housing_city) para o quintil.
	// `ws_.housing` é vetor paralelo a `cities` (mesmo índice).
	const std::size_t city_index = static_cast<std::size_t>(
		&city - ws_.cities.data());
	const CityHousing* house = city_index < ws_.housing.size()
		? &ws_.housing[city_index] : nullptr;
	bool has_tenure = false;
	for (int tier = 0; tier < TIER_COUNT && !has_tenure && house != nullptr;
			++tier) {
		for (int kind = 0; kind < TENURE_COUNT; ++kind) {
			if (house->tenure[tier][kind] > kStrataEps) {
				has_tenure = true;
				break;
			}
		}
	}
	for (int s = 0; s < kIncomeStrata; ++s) {
		const int tier = kStrataTier[s];
		if (has_tenure) {
			for (int kind = 0; kind < TENURE_COUNT; ++kind) {
				pop.stratum_tenure[s][kind] =
					safe_nonneg(house->tenure[tier][kind]);
			}
			double sum = pop.stratum_tenure[s][0] + pop.stratum_tenure[s][1]
				+ pop.stratum_tenure[s][2];
			if (sum > kStrataEps) {
				for (int kind = 0; kind < TENURE_COUNT; ++kind) {
					pop.stratum_tenure[s][kind] = std::clamp(
						pop.stratum_tenure[s][kind] / sum, 0.0, 1.0);
				}
			}
		} else {
			// Sem dado urbano (cenário sintético): ausência explícita. A
			// cidade não cobra aluguel e o status publica 0.
			for (int kind = 0; kind < TENURE_COUNT; ++kind) {
				pop.stratum_tenure[s][kind] = 0.0;
			}
		}
		// A favela (§19) é o ÚLTIMO caso do padrão popular: só o quintil mais
		// pobre pode ter posse informal. Se o dado do padrão popular (que
		// também sustenta o Q2) traz informal, o Q2 é renormalizado para
		// própria/alugada.
		if (s > 0 && pop.stratum_tenure[s][TENURE_INFORMAL] > kStrataEps) {
			const double informal = pop.stratum_tenure[s][TENURE_INFORMAL];
			const double legal = 1.0 - informal;
			pop.stratum_tenure[s][TENURE_INFORMAL] = 0.0;
			if (legal > kStrataEps) {
				pop.stratum_tenure[s][TENURE_OWN] /= legal;
				pop.stratum_tenure[s][TENURE_RENT] /= legal;
			} else {
				pop.stratum_tenure[s][TENURE_RENT] = 1.0;
			}
		}
		pop.stratum_level[s] = 0;
		pop.stratum_debt[s] = 0.0;
		pop.stratum_equity[s] = 0.0;
	}
	// Poupança existente (escalar da cidade) repartida pela renda: o dinheiro
	// já está no mundo, só passa a ter dono.
	const double total_savings = safe_nonneg(pop.savings);
	for (int s = 0; s < kIncomeStrata; ++s) {
		pop.stratum_savings[s] = total_savings * income_share[s];
		pop.stratum_wealth[s] = pop.stratum_savings[s];
		pop.stratum_wage[s] = 0.0;
		pop.stratum_capital[s] = 0.0;
		pop.stratum_rent[s] = 0.0;
		pop.stratum_transfer[s] = 0.0;
		pop.stratum_net_income[s] = 0.0;
		pop.stratum_rent_paid[s] = 0.0;
		pop.stratum_food_spend[s] = 0.0;
		pop.stratum_need[s] = 0.0;
		pop.stratum_consumed[s] = 0.0;
	}
	pop.strata_ready = true;
	refresh_saving_rates(city);
}

void SimCore::rescale_strata_population(Pop& pop) const {
	// Sem estrato semeado, a população de cada quintil é só 20% (leitura).
	double total = 0.0;
	for (int s = 0; s < kIncomeStrata; ++s) {
		total += safe_nonneg(pop.stratum_people[s]);
	}
	if (total <= kStrataEps) {
		for (int s = 0; s < kIncomeStrata; ++s) {
			pop.stratum_people[s] =
				safe_nonneg(pop.population) / double(kIncomeStrata);
		}
		return;
	}
	const double population = safe_nonneg(pop.population);
	for (int s = 0; s < kIncomeStrata; ++s) {
		pop.stratum_people[s] = population
			* safe_nonneg(pop.stratum_people[s]) / total;
	}
}

// ---------------------------------------------------------------------------
// Renda do dia por estrato
// ---------------------------------------------------------------------------
void SimCore::refresh_saving_rates(City& city) {
	Pop& pop = city.pop;
	// Ciclo de vida (B1): peso de cada faixa na taxa. Sem demografia, a
	// composição é derivada da idade ativa/aposentados paramétrica.
	double share[5] = {};
	if (pop.demography_ready) {
		double total = 0.0;
		for (int band = 0; band < 5; ++band) {
			share[band] = safe_nonneg(pop.age_band[band]);
			total += share[band];
		}
		if (total > kStrataEps) {
			for (int band = 0; band < 5; ++band) {
				share[band] /= total;
			}
		}
	} else {
		const double working = safe_nonneg(pop.working_age);
		const double olds = safe_nonneg(pop.retirees);
		const double total = std::max(working + olds, kStrataEps);
		share[1] = working * 0.20 / total;
		share[2] = working * 0.45 / total;
		share[3] = working * 0.35 / total;
		share[4] = olds / total;
	}
	double factor = 0.0;
	double weight = 0.0;
	for (int band = 0; band < 5; ++band) {
		pop.lifecycle_saving[band] = kStrataLifecycleSaving[band];
		factor += share[band] * kStrataLifecycleSaving[band];
		weight += share[band];
	}
	factor = weight > kStrataEps ? factor / weight : 1.0;
	for (int s = 0; s < kIncomeStrata; ++s) {
		pop.stratum_saving_rate[s] = std::clamp(
			kStrataSavingRate[s] * factor, 0.0, 0.60);
	}
}

void SimCore::refresh_strata_income(City& city) {
	Pop& pop = city.pop;
	if (!pop.strata_ready) {
		return;
	}
	rescale_strata_population(pop);
	refresh_saving_rates(city);
	const double population = std::max(pop.population, kStrataEps);
	// 1. Salário: a folha por escolaridade (já paga no dia) é repartida pela
	// composição educacional e pela dispersão de cada quintil.
	double wage_weight[kIncomeStrata] = {};
	double wage_total_weight = 0.0;
	for (int s = 0; s < kIncomeStrata; ++s) {
		double w = 0.0;
		for (int e = 0; e < kEducationLevels; ++e) {
			w += pop.stratum_education_mix[s][e]
				* safe_nonneg(pop.wage_by_level[e]);
		}
		w *= std::max(pop.stratum_wage_factor[s], 0.02);
		wage_weight[s] = w * std::max(pop.stratum_people[s], 0.0);
		wage_total_weight += wage_weight[s];
	}
	const double wage_bill = std::max(0.0, pop.wage_income);
	for (int s = 0; s < kIncomeStrata; ++s) {
		pop.stratum_wage[s] = wage_total_weight > kStrataEps
			? wage_bill * wage_weight[s] / wage_total_weight : 0.0;
	}
	// 2. Capital e aluguel recebido: pelos DONOS (participação de riqueza).
	const double capital_income = std::max(0.0, pop.capital_income);
	const double rent_income = std::max(0.0, pop.rent_income);
	double capital_weight_total = 0.0;
	for (int s = 0; s < kIncomeStrata; ++s) {
		capital_weight_total += pop.stratum_capital_share[s];
	}
	for (int s = 0; s < kIncomeStrata; ++s) {
		const double share = capital_weight_total > kStrataEps
			? pop.stratum_capital_share[s] / capital_weight_total
			: 1.0 / double(kIncomeStrata);
		pop.stratum_capital[s] = capital_income * share;
		pop.stratum_rent[s] = rent_income * share;
	}
	// 3. Transferências (devolução do imposto) e renda externa de subsistência.
	// A transferência é pro-rata ao imposto pago (orçamento equilibrado). A
	// renda externa mistura um piso igual (comida) com a renda de mercado (ver
	// kStrataExternalEqualShare) — as duas somam o total da cidade.
	const double transfers = std::max(0.0, pop.transfers);
	const double external = std::max(0.0, pop.external_income);
	double people_total = 0.0;
	for (int s = 0; s < kIncomeStrata; ++s) {
		people_total += std::max(0.0, pop.stratum_people[s]);
	}
	double market_weight[kIncomeStrata] = {};
	double market_total = 0.0;
	for (int s = 0; s < kIncomeStrata; ++s) {
		market_weight[s] = pop.stratum_wage[s] + pop.stratum_capital[s]
			+ pop.stratum_rent[s];
		market_total += market_weight[s];
	}
	double external_share[kIncomeStrata];
	for (int s = 0; s < kIncomeStrata; ++s) {
		const double equal = people_total > kStrataEps
			? std::max(0.0, pop.stratum_people[s]) / people_total
			: 1.0 / double(kIncomeStrata);
		const double market = market_total > kStrataEps
			? market_weight[s] / market_total : equal;
		external_share[s] = kStrataExternalEqualShare * equal
			+ (1.0 - kStrataExternalEqualShare) * market;
		// Transferência = devolução do imposto arrecadado (orçamento
		// equilibrado), na proporção do imposto pago. Aposentadoria/assistência
		// por idade fica para a fase que cruzar idade × estrato (limite da B2).
		const double transfer_share = market_total > kStrataEps
			? market_weight[s] / market_total : equal;
		pop.stratum_transfer[s] = transfers * transfer_share;
	}
	// Imposto, renda disponível e gasto em serviços: o imposto incide sobre a
	// renda de MERCADO do estrato (salário + capital + aluguel), como no
	// agregado; o serviço sai da renda DOMÉSTICA (a externa é piso de
	// subsistência). A soma dos estratos é o total da cidade.
	double service_weight[kIncomeStrata] = {};
	double service_weight_total = 0.0;
	for (int s = 0; s < kIncomeStrata; ++s) {
		const double taxable = pop.stratum_wage[s] + pop.stratum_capital[s]
			+ pop.stratum_rent[s];
		const double stratum_external = external * external_share[s];
		pop.stratum_net_income[s] = std::max(0.0,
			taxable + stratum_external + pop.stratum_transfer[s]
			- taxable * kTaxRate);
		service_weight[s] = std::max(0.0,
			taxable + pop.stratum_transfer[s] - taxable * kTaxRate);
		service_weight_total += service_weight[s];
	}
	const double service_spend = std::max(0.0, pop.service_spend);
	for (int s = 0; s < kIncomeStrata; ++s) {
		pop.stratum_service_spend[s] = service_weight_total > kStrataEps
			? service_spend * service_weight[s] / service_weight_total : 0.0;
	}
}

// ---------------------------------------------------------------------------
// Dinheiro das famílias depois da fase de renda
// ---------------------------------------------------------------------------
// As fases seguintes (aluguel, importação, serviços) debitam/creditam o
// escalar `pop.savings`. Para a soma dos quintis nunca divergir do escalar, o
// débito é rateado pela poupança de cada estrato (quem tem mais, paga mais) e
// o crédito, pela mesma chave. Sem estrato semeado, só o escalar se move.
// ---------------------------------------------------------------------------
// Gini dos quintis
// ---------------------------------------------------------------------------
double SimCore::strata_gini(const Pop& pop) const {
	// Gini da renda média por quintil (20% de população cada): soma das
	// diferenças pareadas sobre 2·n²·média. É MEDIDO das rendas efetivamente
	// pagas — não é âncora de nada.
	double mean[kIncomeStrata] = {};
	double total = 0.0;
	for (int s = 0; s < kIncomeStrata; ++s) {
		const double people = std::max(pop.stratum_people[s], 0.0);
		mean[s] = people > kStrataEps
			? std::max(0.0, pop.stratum_net_income[s]) / people : 0.0;
		total += mean[s];
	}
	const double average = total / double(kIncomeStrata);
	if (average <= kStrataEps) {
		return 0.0;
	}
	double sum = 0.0;
	for (int a = 0; a < kIncomeStrata; ++a) {
		for (int b = 0; b < kIncomeStrata; ++b) {
			sum += std::fabs(mean[a] - mean[b]);
		}
	}
	return std::clamp(sum / (2.0 * double(kIncomeStrata)
		* double(kIncomeStrata) * average), 0.0, 1.0);
}

void SimCore::charge_households(Pop& pop, double amount) const {
	const double value = std::max(0.0, amount);
	if (!pop.strata_ready || value <= kStrataEps) {
		pop.savings = std::max(0.0, pop.savings - value);
		return;
	}
	double total = 0.0;
	for (int s = 0; s < kIncomeStrata; ++s) {
		total += std::max(0.0, pop.stratum_savings[s]);
	}
	if (total <= kStrataEps) {
		pop.savings = std::max(0.0, pop.savings - std::min(value, pop.savings));
		return;
	}
	// O escalar e a soma dos quintis andam juntos: só sai o que a poupança
	// das famílias realmente tem (o resto viraria dinheiro inventado no
	// fechamento da fase de renda).
	const double paid = std::min(value, total);
	pop.savings = std::max(0.0, pop.savings - paid);
	double charged = 0.0;
	for (int s = 0; s < kIncomeStrata; ++s) {
		const double share = paid * std::max(0.0, pop.stratum_savings[s]) / total;
		pop.stratum_savings[s] = std::max(0.0, pop.stratum_savings[s] - share);
		charged += share;
	}
	// Arredondamento: o resíduo sai do maior estrato para a soma bater.
	if (charged < paid && total > kStrataEps) {
		int top = 0;
		for (int s = 1; s < kIncomeStrata; ++s) {
			if (pop.stratum_savings[s] > pop.stratum_savings[top]) {
				top = s;
			}
		}
		pop.stratum_savings[top] = std::max(0.0,
			pop.stratum_savings[top] - (paid - charged));
	}
}

void SimCore::credit_households(Pop& pop, double amount) const {
	const double value = std::max(0.0, amount);
	pop.savings += value;
	if (!pop.strata_ready || value <= kStrataEps) {
		return;
	}
	double total = 0.0;
	for (int s = 0; s < kIncomeStrata; ++s) {
		total += std::max(0.0, pop.stratum_savings[s]);
	}
	if (total <= kStrataEps) {
		pop.stratum_savings[0] += value;
		return;
	}
	double credited = 0.0;
	for (int s = 0; s < kIncomeStrata; ++s) {
		const double share = value * std::max(0.0, pop.stratum_savings[s]) / total;
		pop.stratum_savings[s] += share;
		credited += share;
	}
	pop.stratum_savings[kIncomeStrata - 1] += value - credited;
}

// ---------------------------------------------------------------------------
// Mobilidade mensal (A6) e patrimônio
// ---------------------------------------------------------------------------
void SimCore::phase_strata() {
	// Cadência mensal escalonada por índice de cidade (A6), laço SERIAL pela
	// mesma razão da demografia: ordem fixa = determinismo.
	const int month_period = ws_.cadence.monthly_period;
	for (std::size_t city_index = 0; city_index < ws_.cities.size();
			++city_index) {
		City& city = ws_.cities[city_index];
		Pop& pop = city.pop;
		if (pop.strata_ready) {
			rescale_strata_population(pop);
			if (cadence_due(ws_.day, city_index, month_period)) {
				// Renda per capita relativa: quem está acima da média da
				// cidade tem chance pequena de subir; abaixo, de descer.
				// Fluxo pequeno e determinístico (sem sorteio).
				double income_per_capita[kIncomeStrata] = {};
				double income_total = 0.0;
				double people_total = 0.0;
				for (int s = 0; s < kIncomeStrata; ++s) {
					const double people = std::max(0.0, pop.stratum_people[s]);
					people_total += people;
					income_total += std::max(0.0, pop.stratum_net_income[s]);
				}
				const double mean = people_total > kStrataEps
					? income_total / people_total : 0.0;
				if (mean > kStrataEps) {
					for (int s = 0; s < kIncomeStrata; ++s) {
						income_per_capita[s] = std::max(0.0,
							pop.stratum_net_income[s])
							/ std::max(pop.stratum_people[s], kStrataEps);
					}
					// De baixo para cima e de cima para baixo numa passada:
					// cada par adjacente move no máximo `rate` do estrato.
					double next_people[kIncomeStrata];
					double next_savings[kIncomeStrata];
					for (int s = 0; s < kIncomeStrata; ++s) {
						next_people[s] = pop.stratum_people[s];
						next_savings[s] = pop.stratum_savings[s];
					}
					for (int s = 0; s + 1 < kIncomeStrata; ++s) {
						const double pull = std::clamp(
							income_per_capita[s] / mean - 1.0, -1.0, 1.0);
						if (std::fabs(pull) < 1e-6) {
							continue;
						}
						const double rate = kStrataMobilityRate * std::fabs(pull);
						const double movers = std::min(
							pop.stratum_people[s], pop.stratum_people[s] * rate);
						if (movers <= kStrataEps) {
							continue;
						}
						const double share = movers
							/ std::max(pop.stratum_people[s], kStrataEps);
						const double savings = pop.stratum_savings[s] * share;
						const int target = pull > 0.0 ? s + 1 : s - 1;
						if (target < 0 || target >= kIncomeStrata) {
							continue;
						}
						next_people[s] -= movers;
						next_savings[s] -= savings;
						next_people[target] += movers;
						next_savings[target] += savings;
					}
					for (int s = 0; s < kIncomeStrata; ++s) {
						pop.stratum_people[s] = std::max(0.0, next_people[s]);
						pop.stratum_savings[s] = std::max(0.0, next_savings[s]);
					}
				}
			}
		}
		if (cadence_due(ws_.day, city_index, month_period)) {
			update_strata_wealth(city);
		}
	}
}

void SimCore::update_strata_wealth(City& city) {
	Pop& pop = city.pop;
	if (!pop.strata_ready) {
		return;
	}
	// Tamanho do domicílio: a fonte define `households` = população ÷ tamanho,
	// então a razão devolve o tamanho sem inventar número. Sem dado: 1 pessoa.
	const std::size_t city_index = static_cast<std::size_t>(
		&city - ws_.cities.data());
	const CityHousing* house = city_index < ws_.housing.size()
		? &ws_.housing[city_index] : nullptr;
	const double population = std::max(pop.population, kStrataEps);
	const double household_size = house != nullptr
		&& house->households > kStrataEps
		? std::clamp(population / house->households, 1.0, 8.0) : 1.0;
	for (int s = 0; s < kIncomeStrata; ++s) {
		const int tier = kStrataTier[s];
		const double own_units = std::max(0.0, pop.stratum_people[s])
			* std::clamp(pop.stratum_tenure[s][TENURE_OWN], 0.0, 1.0)
			/ household_size;
		const double home_value = house != nullptr ? own_units
			* std::max(0.0, house->price[tier]) : 0.0;
		pop.stratum_wealth[s] = std::max(0.0, pop.stratum_savings[s])
			+ home_value + std::max(0.0, pop.stratum_equity[s]);
	}
}

} // namespace ogs


