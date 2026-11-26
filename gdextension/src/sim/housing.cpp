// §19 (W3) — MERCADO DE MORADIA. Módulo de domínio do SimCore, como
// freight/logistics/trade: C++ puro (STL via sim_core.h), sem godot_cpp.
//
// A fase roda depois de `phase_income` (comida e necessidades do dia já
// compradas) e ANTES do consumo de life/luxury do dia seguinte: a fase de
// renda reserva o aluguel do dia no orçamento discricionário (depois da
// comida, antes de life/luxury) e esta fase executa a transferência para os
// donos. Por cidade, em ordem determinística:
//   1. depreciação do estoque;
//   2. demanda por padrão (domicílios do estrato, com transbordo para baixo);
//   3. reancoragem do nível de aluguel na renda medida (uma vez) e ajuste
//      mensal por desequilíbrio (passo limitado, teto de ônus de aluguel);
//   4. pagamento diário do aluguel (renda do dia; nunca abaixo da reserva de
//      comida) e repasse aos donos por owner_share + manutenção;
//   5. abertura de obra de moradia na FILA ÚNICA §15 (`start_project`); o
//      avanço/entrega é o tick da construção (construction.cpp) e a conclusão
//      entra por `apply_completed_project`;
//   6. favela só no último caso (falta popular prolongada e nenhuma vaga);
//   7. sinal `housing_stress` por cidade.
//
// Determinístico: laço serial em ordem de cidade, sem acumulador global — o
// resultado é idêntico com threads=1 e 8 (a fase nem usa o pool).
#include "sim_core.h"

#include "housing.h"

#include "profiling.h"
#include "sim_constants.h"

#include <algorithm>
#include <cmath>

namespace {

// Evita NaN/Inf no estado: o invariante do tick reverte um estado corrompido,
// mas a fase não deve produzi-lo.
inline double safe_nonneg(double value) {
	return std::isfinite(value) ? std::max(0.0, value) : 0.0;
}

inline double safe_unit(double value) {
	return std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 0.0;
}

// Densidade máxima permitida para o padrão, limitada pelo gabarito da cidade:
// o padrão médio constrói prédio, o alto só torre onde o gabarito permite;
// popular é casa (LOW).
inline int density_for_tier(ogs::Density max_density, int tier) {
	const int low = static_cast<int>(ogs::DENSITY_LOW);
	const int mid = static_cast<int>(ogs::DENSITY_MID);
	const int high = static_cast<int>(ogs::DENSITY_HIGH);
	const int allowed = static_cast<int>(max_density);
	if (tier == ogs::TIER_ALTO) {
		return std::clamp(allowed, low, high);
	}
	if (tier == ogs::TIER_MEDIO) {
		return std::clamp(allowed, low, mid);
	}
	return low;
}

// Soma das unidades do estoque (todas as densidades e padrões).
inline double stock_units(const ogs::CityHousing& house) {
	double total = 0.0;
	for (int density = 0; density < ogs::DENSITY_COUNT; ++density) {
		for (int tier = 0; tier < ogs::TIER_COUNT; ++tier) {
			total += safe_nonneg(house.units[density][tier]);
		}
	}
	return total;
}

// Unidades de um padrão somadas em todas as densidades.
inline double tier_units(const ogs::CityHousing& house, int tier) {
	const int t = std::clamp(tier, 0, ogs::TIER_COUNT - 1);
	double total = 0.0;
	for (int density = 0; density < ogs::DENSITY_COUNT; ++density) {
		total += safe_nonneg(house.units[density][t]);
	}
	return total;
}

} // namespace

namespace ogs {

// ------------------------------------------------------------------
// Fase 5b — aluguel, construção e aperto de moradia.
// ------------------------------------------------------------------
void SimCore::phase_housing() {
	// Sem dado urbano (cenário sintético AGRIA/PORTUS, save anterior ao W0) a
	// fase não existe: ausência explícita, nenhum número é inventado.
	if (ws_.housing.size() < ws_.cities.size()
			|| ws_.land.size() < ws_.cities.size()
			|| ws_.city_gov.size() < ws_.cities.size()) {
		return;
	}
	OGS_SIM_ZONE(kPhaseHousing);

	for (std::size_t city_index = 0; city_index < ws_.cities.size();
			++city_index) {
		City& city = ws_.cities[city_index];
		CityHousing& house = ws_.housing[city_index];
		CityLand& land = ws_.land[city_index];
		CityGov& gov = ws_.city_gov[city_index];
		Pop& pop = city.pop;

		// --- 1a. Depreciação do estoque (demolição/substituição) ------------
		const double decay = kUrbanDepreciationPerYear / 365.0;
		for (int density = 0; density < DENSITY_COUNT; ++density) {
			for (int tier = 0; tier < TIER_COUNT; ++tier) {
				house.units[density][tier] =
					safe_nonneg(house.units[density][tier] * (1.0 - decay));
			}
		}
		// A condição média acompanha a idade do estoque (0..1).
		house.condition = safe_unit(house.condition - decay);

		// --- 1b. Obras de moradia: sem avanço aqui --------------------------
		// A obra de moradia vive na FILA ÚNICA da cidade
		// (`ws_.construction_projects`, kind CONSTRUCTION_HOUSING). O avanço é o
		// tick do §15 (SimCore::tick_construction) e a entrega entra por
		// housing::apply_completed_project — não há lista própria a percorrer.

		// --- 2. Oferta, demanda e base de domicílios ------------------------
		double supply[TIER_COUNT] = {};
		double stock_total = 0.0;
		for (int density = 0; density < DENSITY_COUNT; ++density) {
			for (int tier = 0; tier < TIER_COUNT; ++tier) {
				supply[tier] += house.units[density][tier];
				stock_total += house.units[density][tier];
			}
		}
		if (stock_total <= 0.0) {
			house.housing_stress = 0.0;
			house.stress_days = 0;
			continue;  // cidade sem estoque: não há mercado a simular
		}
		// A fonte define o estoque da cidade como população ÷ tamanho do
		// domicílio E reparte esse estoque por padrão pela MESMA distribuição de
		// renda que define os estratos (housing_stock_by_tier). Então a base de
		// domicílios do mercado é a repartição do estoque carregado: o mundo
		// nasce em equilíbrio e só o desvio (construção, decadência, população)
		// gera aperto/sobra. Copiado UMA vez.
		if (house.households <= 0.0) {
			house.households = stock_total;
		}
		if (house.pop_anchor <= 0.0) {
			house.pop_anchor = std::max(1.0, city.simulation_population);
		}
		double tier_base_sum = 0.0;
		for (int tier = 0; tier < TIER_COUNT; ++tier) {
			tier_base_sum += safe_nonneg(house.households_by_tier[tier]);
		}
		if (tier_base_sum <= 0.0) {
			for (int tier = 0; tier < TIER_COUNT; ++tier) {
				house.households_by_tier[tier] = supply[tier];
			}
			tier_base_sum = stock_total;
		}
		const double households = safe_nonneg(house.households);
		// A população da cidade escala a demanda (migração do W4 muda esse
		// número; hoje ele é constante e a demanda acompanha o estoque).
		const double population_scale = std::max(0.0,
			city.simulation_population) / std::max(1e-9, house.pop_anchor);
		double demand[TIER_COUNT] = {};
		for (int tier = 0; tier < TIER_COUNT; ++tier) {
			demand[tier] = safe_nonneg(house.households_by_tier[tier])
				* population_scale;
		}
		// Filtragem: o excedente de demanda do padrão alto desce para o médio e
		// o do médio desce para o popular. Quem não cabe em padrão algum é falta
		// de moradia de verdade (favela, último caso).
		for (int tier = TIER_ALTO; tier > TIER_POPULAR; --tier) {
			const double overflow = std::max(0.0, demand[tier] - supply[tier]);
			demand[tier] -= overflow;
			demand[tier - 1] += overflow;
		}
		const double popular_shortage_units = std::max(0.0,
			demand[TIER_POPULAR] - supply[TIER_POPULAR]);
		const double popular_shortage = popular_shortage_units
			/ std::max(1.0, demand[TIER_POPULAR]);
		// Falta PRÓPRIA do padrão popular (sem o transbordo dos padrões altos):
		// é a única medida que pode abrir favela. Assim a depreciação de 1%/ano
		// do estoque MÉDIO não fabrica falta popular e não gera favela num
		// mercado que só perdeu unidade média (Q-015/B5).
		const double poor_demand_units = std::max(1.0,
			safe_nonneg(house.households_by_tier[TIER_POPULAR])
				* population_scale);
		const double poor_shortage_units = std::max(0.0,
			poor_demand_units - supply[TIER_POPULAR]);
		const double poor_shortage = poor_shortage_units / poor_demand_units;
		double vacancy_units = 0.0;
		for (int tier = 0; tier < TIER_COUNT; ++tier) {
			house.vacancy[tier] = std::max(0.0, supply[tier] - demand[tier]);
			vacancy_units += house.vacancy[tier];
		}

		// --- 3. Renda de referência e reserva de comida ---------------------
		// Renda mensal por domicílio (âncora do aluguel) e a reserva de comida
		// que o aluguel NUNCA toca: comida tem prioridade sobre moradia.
		const double income_per_household_month = households > 1e-9
			? safe_nonneg(pop.disposable) * 30.0 / households : 0.0;
		const double food_unit_cost = pop.need > 1e-9
			? safe_nonneg(pop.food_spend) / pop.need : 0.0;
		const double food_reserve = safe_nonneg(pop.need)
			* kHousingFoodReserveDays * food_unit_cost;
		const double spendable = std::max(0.0,
			safe_nonneg(pop.savings) - food_reserve);
		// Unidades efetivamente alugadas (inquilinos) - só elas pagam.
		double tenant_units = 0.0;
		for (int tier = 0; tier < TIER_COUNT; ++tier) {
			tenant_units += supply[tier]
				* safe_unit(house.tenure[tier][TENURE_RENT]);
		}

		// --- 4. Reancoragem do nível e fechamento do mês --------------------
		// A fonte publica aluguel/preço em USD/m²; o núcleo usa XGS numa escala
		// própria (a renda medida da POP). O nível absoluto da fonte é ~2,7
		// ordens de grandeza acima da renda do núcleo, então o nível é
		// REANCORADO uma única vez na renda medida, preservando o gradiente
		// entre padrões da fonte (médio/alto relativos ao popular).
		if (!house.rent_anchored && income_per_household_month > 1e-9) {
			double base = 0.0;
			for (int tier = 0; tier < TIER_COUNT; ++tier) {
				const double rent = safe_nonneg(house.rent[tier]);
				if (rent > 0.0 && (base <= 0.0 || rent < base)) {
					base = rent;
				}
			}
			for (int tier = 0; tier < TIER_COUNT; ++tier) {
				double rent = safe_nonneg(house.rent[tier]);
				const double ratio = base > 0.0 ? rent / base : 1.0;
				house.rent_cap_ratio[tier] = std::clamp(ratio, 1.0, 2.5);
				rent = income_per_household_month * kHousingInitialBurden
					* house.rent_cap_ratio[tier];
				house.rent[tier] = safe_nonneg(rent);
				house.price[tier] = house.rent[tier] * 12.0
					/ kUrbanCapRate;
			}
			house.rent_anchored = true;
		}

		// A6: o fechamento do mês (nível de aluguel, incorporadora, favela) roda no
		// dia da cidade (`(dia - índice) % 30 == 0`), não para todas no dia 30. A
		// cobrança do aluguel e o sinal de aperto continuam DIÁRIOS: é o que
		// conserva dinheiro todo dia e mantém a comida à frente do aluguel.
		// `month_period <= 1` = todas as cidades todo dia (interruptor de teste,
		// mesmo sentido dos outros sistemas). `month_marker` guarda a janela
		// acumulada do ônus.
		const int month_period = ws_.cadence.monthly_period;
		const bool month_close = month_period <= 1
			? true
			: cadence_due(ws_.day, city_index, month_period);
		// Favela (ÚLTIMO caso): só quando não há vaga em padrão algum E a falta
		// é da própria POP pobre (> 5% dos domicílios populares) E os pobres
		// estão sem renda para o aluguel (aluguel popular no teto de ônus) ou a
		// falta é severa (≥ 20%: não é preço, é estoque). A depreciação do
		// padrão médio não entra nesta conta (Q-015/B5).
		const bool poor_priced_out = income_per_household_month > 1e-9
			&& house.rent[TIER_POPULAR] >= income_per_household_month
				* kHousingRentBurdenCap;
		const bool slum_conditions =
			vacancy_units <= households * kHousingSlumVacancyMax
			&& poor_shortage > kHousingSlumShortage
			&& (poor_priced_out
				|| poor_shortage >= kHousingSlumSevereShortage);
		house.stress_days = slum_conditions ? house.stress_days + 1 : 0;
		if (month_close) {
			// Fecha o mês: publica o ônus do mês QUE TERMINOU (acumulado cheio)
			// antes de zerar os acumuladores. Ler `rent_paid_month` no meio do
			// mês dava ~20% da fatura sempre (6 dias ÷ 30) — era o defeito de
			// medição que o Q-015/B2 leu como "só 20% é pago" (Q-015 fix).
			if (income_per_household_month > 0.0) {
				const double tenant_income_month =
					income_per_household_month * tenant_units;
				house.rent_burden = tenant_income_month > 1e-9
					? safe_unit(house.rent_paid_month / tenant_income_month)
					: 0.0;
			} else {
				house.rent_burden = 0.0;
			}
			house.month_marker = ws_.day;
			house.rent_paid_month = 0.0;
			house.rent_households_month = 0.0;
			house.rent_firms_month = 0.0;
			house.rent_government_month = 0.0;
			house.units_built_month = 0.0;

			// Ônus de MERCADO do mês fechado: fatura mensal (unidades alugadas ×
			// aluguel) ÷ renda mensal dos inquilinos. Usa o nível, não o pago,
			// para o piso não depender de liquidez/fome do dia.
			double bill_month = 0.0;
			for (int tier = 0; tier < TIER_COUNT; ++tier) {
				bill_month += tier_units(house, tier)
					* safe_unit(house.tenure[tier][TENURE_RENT])
					* safe_nonneg(house.rent[tier]);
			}
			const double tenant_income_month =
				income_per_household_month * tenant_units;
			const double market_burden =
				tenant_income_month > 1e-9
					? bill_month / tenant_income_month : 0.0;

			for (int tier = 0; tier < TIER_COUNT; ++tier) {
				// Desequilíbrio relativo no padrão (após a filtragem):
				// positivo = falta de unidade, negativo = sobra.
				const double imbalance = demand[tier] > 1e-9
					? (demand[tier] - supply[tier]) / demand[tier] : 0.0;
				const double step = std::clamp(
					imbalance * kHousingRentElasticity,
					-kHousingRentStepMax, kHousingRentStepMax);
				double rent = safe_nonneg(house.rent[tier]) * (1.0 + step);
				// Piso da faixa de ônus: se o ônus de MERCADO do mês fechado
				// (fatura ÷ renda dos inquilinos, independente do que foi pago)
				// caiu abaixo do piso, o nível é reancorado na renda corrente.
				// É a conversão do nível da fonte (renda de hoje), não um passo
				// de mercado — o passo de mercado é o `step` acima.
				if (market_burden > 0.0
						&& market_burden < kHousingBurdenFloor) {
					const double floor_rent = income_per_household_month
						* kHousingBurdenFloor * house.rent_cap_ratio[tier];
					rent = std::max(rent, floor_rent);
				}
				// Âncora de renda: o aluguel de um domicílio não passa de
				// kHousingRentBurdenCap da renda mensal do domicílio,
				// ponderado pelo gradiente de padrão da fonte.
				const double anchor = income_per_household_month
					* kHousingRentBurdenCap * house.rent_cap_ratio[tier];
				if (anchor > 0.0) {
					rent = std::min(rent, anchor);
				}
				house.rent[tier] = safe_nonneg(rent);
				// §19.6: valor do imóvel = aluguel anual ÷ cap rate.
				house.price[tier] = house.rent[tier] * 12.0
					/ kUrbanCapRate;
			}
		}

		// --- 5. Pagamento do aluguel ---------------------------------------
		// Aluguel cobrado da RENDA do dia: a fase de renda já reservou
		// `daily_rent_obligation` no orçamento discricionário (depois da comida,
		// antes de life/luxury). Aqui a obrigação é transferida aos donos; o
		// pagamento nunca passa da poupança acima da reserva de comida (se a
		// comida consumiu o orçamento, o aluguel do dia fica em falta — comida
		// primeiro). O que falta não vira dívida inventada.
		const double due = safe_nonneg(std::min(
			housing::daily_rent_obligation(ws_, city_index), spendable));
		// B2: o aluguel é pago pelos INQUILINOS de cada quintil (posse do §19
		// por estrato), limitado à poupança de cada um. O que a soma não cobre
		// fica em falta — comida e reserva primeiro (nunca dívida inventada).
		double paid = due;
		if (due > 0.0 && pop.strata_ready) {
			double weight[kIncomeStrata] = {};
			double weight_total = 0.0;
			for (int s = 0; s < kIncomeStrata; ++s) {
				weight[s] = std::max(0.0, pop.stratum_people[s])
					* safe_unit(pop.stratum_tenure[s][TENURE_RENT]);
				weight_total += weight[s];
			}
			if (weight_total <= 1e-9) {
				for (int s = 0; s < kIncomeStrata; ++s) {
					weight[s] = safe_nonneg(pop.stratum_savings[s]);
					weight_total += weight[s];
				}
			}
			double capacity[kIncomeStrata] = {};
			double capacity_total = 0.0;
			for (int s = 0; s < kIncomeStrata; ++s) {
				const double share = weight_total > 1e-9
					? due * weight[s] / weight_total : 0.0;
				capacity[s] = std::min(share,
					safe_nonneg(pop.stratum_savings[s]));
				capacity_total += capacity[s];
			}
			paid = std::min(due, capacity_total);
			const double scale = capacity_total > 1e-9 && paid < capacity_total
				? paid / capacity_total : 1.0;
			pop.savings = 0.0;
			for (int s = 0; s < kIncomeStrata; ++s) {
				const double amount = capacity[s] * scale;
				pop.stratum_savings[s] = std::max(0.0,
					pop.stratum_savings[s] - amount);
				pop.stratum_rent_paid[s] += amount;
				pop.savings += pop.stratum_savings[s];
			}
		} else if (paid > 0.0) {
			pop.savings = std::max(0.0, pop.savings - paid);
		}
		house.rent_paid_today = paid;
		if (paid > 0.0) {
			// Manutenção: os donos pagam a conservação do estoque ao setor de
			// serviços (reparos). Fração declarada do aluguel — o valor de
			// reposição do estoque é ordens de grandeza maior que a moeda em
			// circulação, então a manutenção só pode sair da renda dele.
			const double maintenance = paid * kHousingMaintenanceShare;
			const double net = paid - maintenance;
			city.service_receive(maintenance);
			double share_house = safe_unit(house.owner_share[OWNER_HOUSEHOLD]);
			double share_firm = safe_unit(house.owner_share[OWNER_FIRM]);
			double share_gov = safe_unit(house.owner_share[OWNER_GOVERNMENT]);
			const double share_sum = share_house + share_firm + share_gov;
			if (share_sum <= 1e-9) {
				share_house = 1.0;
				share_firm = 0.0;
				share_gov = 0.0;
			} else {
				share_house /= share_sum;
				share_firm /= share_sum;
				share_gov /= share_sum;
			}
			// Família dona → renda de capital da POP (creditada no próximo dia,
			// como o lucro distribuído); empresa → caixa das firmas; prefeitura
			// → caixa municipal (despesa do W2). Tudo é transferência: o
			// dinheiro total não muda.
			const double to_households = net * share_house;
			const double to_firms = net * share_firm;
			const double to_government = net * share_gov;
			city.rent_income_next += to_households;
			city.firm_receive(to_firms);
			gov.cash += to_government;
			house.rent_households_month += to_households;
			house.rent_firms_month += to_firms;
			house.rent_government_month += to_government;
			house.rent_paid_month += paid;
		}

		// --- 6. Construção de moradia (fila única §15, kind HOUSING) --------
		if (month_close) {
			// Incorporadora: constrói MID/ALTO quando há falta E o preço cobre
			// o custo com margem. O custo é expresso em ANOS DE ALUGUEL do
			// padrão (parâmetro declarado, independente da escala monetária do
			// mundo) mais a terra da zona residencial (W1 move o land_price).
			for (int tier = TIER_MEDIO; tier <= TIER_ALTO; ++tier) {
				if (housing::project_count(ws_, city_index)
						>= kHousingMaxProjects) {
					break;
				}
				const double shortage = std::max(0.0, demand[tier] - supply[tier]);
				if (shortage <= 0.0) {
					continue;
				}
				const int density = density_for_tier(
					land.max_density[ZONE_RES], tier);
				const double cost_unit = housing::build_cost_unit(
					ws_, city_index, density, tier);
				if (cost_unit <= 0.0) {
					continue;
				}
				if (house.price[tier]
						< cost_unit * (1.0 + kHousingDeveloperMargin)) {
					continue;
				}
				const double short_batch = std::min(shortage,
					demand[tier] * kHousingBatchShare);
				// O caixa das firmas limita o lote: a incorporadora não abre
				// obra que não pode pagar (sem isso a falta de caixa viraria
				// um no-op silencioso em start_project). Mínimo de 1 unidade.
				const double units = std::min(short_batch,
					std::max(0.0, city.firm_liquidity()) / cost_unit);
				if (units < 1.0) {
					continue;
				}
				// A obra entra na fila única; o tick do §15 compra material,
				// paga o setor e entrega as unidades ao fim do prazo.
				housing::start_project(ws_, *this, city_index,
					HOUSING_PROJECT_PRIVATE, density, tier, units);
			}
			// Autoconstrução da POP: só o padrão popular, só com falta e só com
			// a poupança acima da reserva de comida (a comida vem primeiro).
			if (popular_shortage > 0.02
					&& housing::project_count(ws_, city_index)
						< kHousingMaxProjects) {
				const double cost_unit = housing::build_cost_unit(
					ws_, city_index, DENSITY_LOW, TIER_POPULAR);
				const double units = std::min(popular_shortage_units,
					supply[TIER_POPULAR] * kHousingSelfBuildShare);
				const double cost = units * cost_unit;
				// Não compromete mais de um quarto da folga de poupança.
				if (cost > 0.0 && cost * 4.0 <= spendable) {
					housing::start_project(ws_, *this, city_index,
						HOUSING_PROJECT_SELF, DENSITY_LOW, TIER_POPULAR, units);
				}
			}
		}

		// --- 7. Favela (ÚLTIMO caso) ---------------------------------------
		// Só abre área informal quando a falta popular é grande, não há vaga em
		// padrão algum e o aperto já dura kHousingSlumDays dias seguidos.
		if (month_close && house.stress_days >= kHousingSlumDays
				&& slum_conditions) {
			double added = std::min(popular_shortage_units,
				demand[TIER_POPULAR] * kHousingSlumShare);
			added = safe_nonneg(added);
			if (added >= 1.0) {
				house.units[DENSITY_LOW][TIER_POPULAR] += added;
				house.informal_units = safe_nonneg(house.informal_units + added);
				land.informal_km2 = safe_nonneg(land.informal_km2
					+ added * kUrbanUnitM2[DENSITY_LOW][TIER_POPULAR]
						/ 1.0e6);
				// Posse: a unidade informal sai do aluguel/da casa própria e a
				// linha do padrão volta a somar 1.
				const double informal = safe_unit(
					house.informal_units
					/ std::max(1.0, supply[TIER_POPULAR] + added));
				const double legal = 1.0 - informal;
				const double own = safe_unit(
					house.tenure[TIER_POPULAR][TENURE_OWN]);
				const double rent = safe_unit(
					house.tenure[TIER_POPULAR][TENURE_RENT]);
				const double legal_sum = own + rent;
				house.tenure[TIER_POPULAR][TENURE_INFORMAL] = informal;
				house.tenure[TIER_POPULAR][TENURE_OWN] = legal_sum > 1e-9
					? own / legal_sum * legal : 0.0;
				house.tenure[TIER_POPULAR][TENURE_RENT] = legal_sum > 1e-9
					? rent / legal_sum * legal : 0.0;
				// Penalidade declarada de satisfação/saúde (moradia precária),
				// proporcional à fatia informal e limitada a 5 p.p.
				pop.needs_satisfaction = safe_unit(pop.needs_satisfaction
					- kHousingSlumSatisfaction * informal);
			}
		}

		// --- 8. Sinal de aperto habitacional -------------------------------
		// 0..1: falta de unidade no padrão popular + ônus de aluguel acima do
		// teto. É o insumo do W4 (migração) e da UI.
		const double burden_excess = income_per_household_month > 0.0
			? std::max(0.0, house.rent_burden - kHousingRentBurdenCap) : 0.0;
		house.housing_stress = safe_unit(
			popular_shortage + burden_excess * 2.0);
	}
}

// ------------------------------------------------------------------
// Moradia na fila única §15 (kind CONSTRUCTION_HOUSING).
// ------------------------------------------------------------------
namespace housing {

double build_cost_unit(const WorldState& ws, std::size_t city_index,
		int density, int tier) {
	if (city_index >= ws.housing.size() || city_index >= ws.land.size()) {
		return 0.0;
	}
	const CityHousing& house = ws.housing[city_index];
	const CityLand& land = ws.land[city_index];
	const int d = std::clamp(density, 0, DENSITY_COUNT - 1);
	const int t = std::clamp(tier, 0, TIER_COUNT - 1);
	const double rent = std::max(0.0, house.rent[t]);
	// Q-015 (integração): a terra que sustenta a unidade é o LOTE
	// (m² construído ÷ FAR da densidade), não a área construída. Sem o FAR o
	// custo da terra em zona de torre (FAR 6) entrava 6× acima do lote real e
	// o preço do solo calibrado (Q-015-C) re-quebrava o gate do incorporador.
	const double far = std::max(1.0, kUrbanFar[d]);
	const double land_area_m2 = kUrbanUnitM2[d][t] / far;
	const double unit_value = rent * 12.0 / kUrbanCapRate;
	const double land_cost = std::min(land_area_m2 * std::max(0.0, land.land_price[ZONE_RES]),
		kHousingLandShareMax * unit_value);
	if (rent <= 0.0 && land_cost <= 0.0) {
		return 0.0;  // sem preço nem terra: ausência explícita
	}
	return rent * 12.0 * kHousingBuildCostYears + land_cost;
}

double daily_rent_obligation(const WorldState& ws, std::size_t city_index) {
	if (city_index >= ws.cities.size() || city_index >= ws.housing.size()) {
		return 0.0;  // sem dado urbano: ausência explícita
	}
	const CityHousing& house = ws.housing[city_index];
	const City& city = ws.cities[city_index];
	const double stock_total = stock_units(house);
	if (stock_total <= 0.0 || !house.rent_anchored) {
		// Sem nível de aluguel em XGS (fonte ainda não reancorada) não há
		// obrigação a reservar: o primeiro tick ancora o nível na renda.
		return 0.0;
	}
	// Fatura mensal dos inquilinos e a renda deles. A fatura usa a posse de
	// ALUGUEL de cada padrão e as unidades do estoque (mesma conta da fase).
	double bill_month = 0.0;
	double tenant_units = 0.0;
	for (int tier = 0; tier < TIER_COUNT; ++tier) {
		const double tenants = tier_units(house, tier)
			* safe_unit(house.tenure[tier][TENURE_RENT]);
		tenant_units += tenants;
		bill_month += tenants * safe_nonneg(house.rent[tier]);
	}
	// Mesma base de domicílios da fase: o estoque é a base até o primeiro tick
	// definir `households`.
	const double households = house.households > 0.0
		? safe_nonneg(house.households) : stock_total;
	if (households <= 1e-9 || tenant_units <= 1e-9) {
		return 0.0;
	}
	const double income_per_household_month =
		safe_nonneg(city.pop.disposable) * 30.0 / households;
	const double tenant_income_day =
		income_per_household_month * tenant_units / 30.0;
	// Aluguel do dia = menor entre a fatura ÷ 30 e o teto de ônus da renda
	// dos inquilinos. É o valor que a fase de renda desconta do orçamento
	// discricionário e que a fase de moradia transfere aos donos.
	return safe_nonneg(std::min(bill_month / 30.0,
		tenant_income_day * kHousingRentBurdenCap));
}

int project_count(const WorldState& ws, std::size_t city_index) {
	if (city_index >= ws.cities.size()) {
		return 0;
	}
	const std::string& city_id = ws.cities[city_index].id;
	int count = 0;
	for (const ConstructionProject& project : ws.construction_projects) {
		if (project.city == city_id && project.kind == CONSTRUCTION_HOUSING) {
			++count;
		}
	}
	return count;
}

bool start_project(WorldState& ws, SimCore& core, std::size_t city_index,
		int owner_state, int density, int tier, double units) {
	if (city_index >= ws.cities.size() || city_index >= ws.housing.size()
			|| city_index >= ws.land.size() || city_index >= ws.city_gov.size()) {
		return false;
	}
	if (!std::isfinite(units) || units <= 0.0) {
		return false;
	}
	const int owner = std::clamp(owner_state, 0,
		static_cast<int>(HOUSING_PROJECT_GOVERNMENT));
	if (owner == HOUSING_PROJECT_IDLE) {
		return false;  // obra sem dono não existe
	}
	// Limite de obras de moradia SIMULTÂNEAS da cidade na fila única.
	if (project_count(ws, city_index) >= kHousingMaxProjects) {
		return false;
	}
	const int d = std::clamp(density, 0, DENSITY_COUNT - 1);
	const int t = std::clamp(tier, 0, TIER_COUNT - 1);
	const double cost_unit = build_cost_unit(ws, city_index, d, t);
	if (cost_unit <= 0.0) {
		return false;
	}
	City& city = ws.cities[city_index];
	ConstructionProject project;
	// Id determinístico por cidade/dono/padrão/densidade/dia: o mesmo dia não
	// abre duas obras iguais (o id duplicado é recusado abaixo).
	project.id = "HSG-" + city.id + "-" + std::to_string(owner) + "-"
		+ std::to_string(t) + "-" + std::to_string(d) + "-"
		+ std::to_string(ws.day);
	for (const ConstructionProject& existing : ws.construction_projects) {
		if (existing.id == project.id) {
			return false;  // id duplicado: nada é pago
		}
	}
	const double total_cost = units * cost_unit;
	if (!std::isfinite(total_cost) || total_cost <= 0.0) {
		return false;
	}
	// Caixa do dono: poupança da POP (autoconstrução), caixa das firmas
	// (incorporadora) ou caixa da prefeitura (moradia social). Sem caixa, a
	// obra não abre e NADA é pago.
	double budget = 0.0;
	if (owner == HOUSING_PROJECT_SELF) {
		budget = safe_nonneg(city.pop.savings);
	} else if (owner == HOUSING_PROJECT_GOVERNMENT) {
		budget = safe_nonneg(ws.city_gov[city_index].cash);
	} else {
		budget = city.firm_liquidity();
	}
	if (budget + 1e-9 < total_cost) {
		return false;
	}
	project.building_id = project.id;
	project.city = city.id;
	// Só a moradia social é do estado; incorporadora e autoconstrução são
	// capital privado (firmas e famílias).
	project.owner = owner == HOUSING_PROJECT_GOVERNMENT ? "state" : "private";
	project.resource.clear();
	project.output_product = -1;
	project.recipe = -1;
	project.input_product = -1;
	project.kind = CONSTRUCTION_HOUSING;
	project.capacity = units;
	project.total_cost = total_cost;
	project.reserved = total_cost;  // o add sobrescreve: total − terra
	project.housing_owner = owner;
	project.housing_density = d;
	project.housing_tier = t;
	project.housing_units = units;
	// O dono paga o custo inteiro na abertura; o add credita a parcela de
	// TERRA à prefeitura e reserva o resto (material + mão de obra).
	if (owner == HOUSING_PROJECT_SELF) {
		// B2: a autoconstrução sai da poupança das famílias rateada pelos
		// quintis (a soma dos estratos acompanha o escalar).
		core.charge_households(city.pop, total_cost);
	} else if (owner == HOUSING_PROJECT_GOVERNMENT) {
		ws.city_gov[city_index].cash =
			safe_nonneg(ws.city_gov[city_index].cash) - total_cost;
	} else {
		city.firm_pay(total_cost);
	}
	core.add_construction_project(project);
	return true;
}

bool has_social_project(const WorldState& ws, std::size_t city_index) {
	if (city_index >= ws.cities.size()) {
		return false;
	}
	const std::string& city_id = ws.cities[city_index].id;
	for (const ConstructionProject& project : ws.construction_projects) {
		if (project.city == city_id && project.kind == CONSTRUCTION_HOUSING
				&& project.housing_owner == HOUSING_PROJECT_GOVERNMENT) {
			return true;
		}
	}
	return false;
}

bool start_social_housing(WorldState& ws, SimCore& core,
		std::size_t city_index, double units) {
	if (city_index >= ws.cities.size() || city_index >= ws.housing.size()
			|| city_index >= ws.land.size() || city_index >= ws.city_gov.size()) {
		return false;
	}
	if (!std::isfinite(units) || units < 1.0) {
		return false;
	}
	const double cost_unit = build_cost_unit(
		ws, city_index, DENSITY_LOW, TIER_POPULAR);
	if (cost_unit <= 0.0) {
		return false;
	}
	// A prefeitura não contrata obra sem caixa: as unidades são reduzidas ao
	// que o caixa paga (mesma regra do W2 para expansão/adensamento).
	const double budget = safe_nonneg(ws.city_gov[city_index].cash);
	const double affordable = budget / cost_unit;
	const double paid_units = std::min(units, affordable);
	if (paid_units < 1.0) {
		return false;
	}
	return start_project(ws, core, city_index, HOUSING_PROJECT_GOVERNMENT,
		DENSITY_LOW, TIER_POPULAR, paid_units);
}

void apply_completed_project(WorldState& ws,
		const ConstructionProject& project) {
	const auto found = ws.city_lookup.find(project.city);
	if (found == ws.city_lookup.end() || found->second < 0
			|| static_cast<std::size_t>(found->second) >= ws.housing.size()) {
		return;  // cidade sumiu: a obra se perde, o dinheiro já foi pago
	}
	CityHousing& house = ws.housing[static_cast<std::size_t>(found->second)];
	const int density = std::clamp(project.housing_density, 0, DENSITY_COUNT - 1);
	const int tier = std::clamp(project.housing_tier, 0, TIER_COUNT - 1);
	const double units = safe_nonneg(project.housing_units);
	const int owner_state = std::clamp(project.housing_owner, 0,
		static_cast<int>(HOUSING_PROJECT_GOVERNMENT));
	// Estoque ANTES da entrega: a posse é recomputada em unidades.
	const double total_before = stock_units(house);
	house.units[density][tier] =
		safe_nonneg(house.units[density][tier] + units);
	house.units_built_month = safe_nonneg(house.units_built_month + units);
	if (units <= 0.0) {
		return;
	}
	// Posse GENÉRICA pelo dono do projeto: família na autoconstrução, firma na
	// incorporadora, prefeitura na moradia social. `owner_units` = unidades do
	// dono antes + as novas; a fração dele sai daí e as dos demais mantêm a
	// proporção (renormalizadas).
	const int owner = owner_state == HOUSING_PROJECT_SELF ? OWNER_HOUSEHOLD
		: owner_state == HOUSING_PROJECT_GOVERNMENT ? OWNER_GOVERNMENT
		: OWNER_FIRM;
	const double total_after = total_before + units;
	double previous[OWNER_COUNT] = {};
	for (int i = 0; i < OWNER_COUNT; ++i) {
		previous[i] = safe_unit(house.owner_share[i]);
	}
	const double owner_units = total_before * previous[owner] + units;
	const double owner_share = total_after > 1e-9
		? std::clamp(owner_units / total_after, 0.0, 1.0) : 0.0;
	double other_sum = 0.0;
	for (int i = 0; i < OWNER_COUNT; ++i) {
		if (i != owner) {
			other_sum += previous[i];
		}
	}
	for (int i = 0; i < OWNER_COUNT; ++i) {
		house.owner_share[i] = i == owner ? owner_share : 0.0;
	}
	if (other_sum <= 1e-9) {
		// Nenhum outro dono com fração: o resto vai para a família (fallback
		// declarado, o mesmo do W3 para a moradia social), somando 1. Quando
		// o próprio dono é a família, ela fica com tudo.
		if (owner != OWNER_HOUSEHOLD) {
			house.owner_share[OWNER_HOUSEHOLD] = std::max(
				0.0, 1.0 - owner_share);
		}
	} else {
		const double scale = std::max(0.0, 1.0 - owner_share) / other_sum;
		for (int i = 0; i < OWNER_COUNT; ++i) {
			if (i != owner) {
				house.owner_share[i] = previous[i] * scale;
			}
		}
	}
	if (owner_state == HOUSING_PROJECT_GOVERNMENT) {
		// Moradia social (W3): o estoque novo é do governo e a posse entra como
		// ALUGUEL subsidiado. A fração é recomputada em unidades e depois
		// renormalizada para o estoque legal do padrão.
		const double tier_after = std::max(1.0, tier_units(house, tier));
		const double tier_before = std::max(1.0, tier_after - units);
		const double informal = safe_unit(house.tenure[tier][TENURE_INFORMAL]);
		const double legal = std::max(0.0, 1.0 - informal);
		const double legal_before = tier_before * legal;
		const double rent_units_before =
			safe_unit(house.tenure[tier][TENURE_RENT]) * legal_before;
		const double own_units_before =
			safe_unit(house.tenure[tier][TENURE_OWN]) * legal_before;
		const double legal_after =
			std::max(1e-9, rent_units_before + units + own_units_before);
		const double rent_frac = (rent_units_before + units) / legal_after;
		house.tenure[tier][TENURE_INFORMAL] = informal;
		house.tenure[tier][TENURE_RENT] = std::clamp(
			rent_frac * legal, 0.0, legal);
		house.tenure[tier][TENURE_OWN] =
			std::max(0.0, legal - house.tenure[tier][TENURE_RENT]);
	}
}

} // namespace housing

} // namespace ogs
