// §19 (W3) — MERCADO DE MORADIA. Módulo de domínio do SimCore, como
// freight/logistics/trade: C++ puro (STL via sim_core.h), sem godot_cpp.
//
// A fase roda depois de `phase_income` (comida e necessidades do dia já
// compradas) e ANTES do consumo de luxo do dia seguinte — o aluguel só usa a
// poupança que sobrou acima da reserva de comida, então a comida tem
// prioridade. Por cidade, em ordem determinística:
//   1. depreciação do estoque e avanço das obras de moradia;
//   2. demanda por padrão (domicílios do estrato, com transbordo para baixo);
//   3. reancoragem do nível de aluguel na renda medida (uma vez) e ajuste
//      mensal por desequilíbrio (passo limitado, teto de ônus de aluguel);
//   4. pagamento diário do aluguel (poupança acima da reserva de comida) e
//      repasse aos donos por owner_share + manutenção;
//   5. construção de moradia (projeto §15 tipo "moradia");
//   6. favela só no último caso (falta popular prolongada e nenhuma vaga);
//   7. sinal `housing_stress` por cidade.
//
// Determinístico: laço serial em ordem de cidade, sem acumulador global — o
// resultado é idêntico com threads=1 e 8 (a fase nem usa o pool).
#include "sim_core.h"

#include "housing.h"
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

	// Materiais da obra de moradia (mesma tabela do §15) e seus pesos.
	static const char* const kMaterials[3] = {
		"construction_material", "steel_structure", "electrical_material"};
	static const double kMaterialWeight[3] = {0.50, 0.30, 0.20};

	for (std::size_t city_index = 0; city_index < ws_.cities.size();
			++city_index) {
		DemoCity& city = ws_.cities[city_index];
		CityHousing& house = ws_.housing[city_index];
		CityLand& land = ws_.land[city_index];
		CityGov& gov = ws_.city_gov[city_index];
		DemoPop& pop = city.pop;

		// --- 1a. Depreciação do estoque (demolição/substituição) ------------
		const double decay = kDemoUrbanDepreciationPerYear / 365.0;
		for (int density = 0; density < DENSITY_COUNT; ++density) {
			for (int tier = 0; tier < TIER_COUNT; ++tier) {
				house.units[density][tier] =
					safe_nonneg(house.units[density][tier] * (1.0 - decay));
			}
		}
		// A condição média acompanha a idade do estoque (0..1).
		house.condition = safe_unit(house.condition - decay);

		// --- 1b. Avanço das obras de moradia (§15, tipo "moradia") ----------
		for (int slot = 0; slot < kDemoHousingMaxProjects; ++slot) {
			HousingProject& project = house.projects[slot];
			if (project.state == HOUSING_PROJECT_IDLE) {
				continue;
			}
			project.days_left -= 1;
			if (project.days_left > 0) {
				continue;
			}
			const int density = std::clamp(project.density, 0, DENSITY_COUNT - 1);
			const int tier = std::clamp(project.tier, 0, TIER_COUNT - 1);
			const double units = safe_nonneg(project.units);
			house.units[density][tier] += units;
			house.units_built_month += units;
			project = HousingProject{};
		}

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
			* kDemoHousingFoodReserveDays * food_unit_cost;
		const double spendable = std::max(0.0,
			safe_nonneg(pop.savings) - food_reserve);
		// Unidades efetivamente alugadas (inquilinos) - só elas pagam.
		double tenant_units = 0.0;
		for (int tier = 0; tier < TIER_COUNT; ++tier) {
			tenant_units += supply[tier]
				* safe_unit(house.tenure[tier][TENURE_RENT]);
		}
		const double tenant_income_day = income_per_household_month
			* tenant_units / 30.0;

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
				rent = income_per_household_month * kDemoHousingInitialBurden
					* house.rent_cap_ratio[tier];
				house.rent[tier] = safe_nonneg(rent);
				house.price[tier] = house.rent[tier] * 12.0
					/ kDemoUrbanCapRate;
			}
			house.rent_anchored = true;
		}

		const bool month_close = ws_.day - house.month_marker >= 30;
		const bool slum_conditions = popular_shortage
				> kDemoHousingSlumShortage
			&& vacancy_units <= households * kDemoHousingSlumVacancyMax;
		house.stress_days = slum_conditions ? house.stress_days + 1 : 0;
		if (month_close) {
			house.month_marker = ws_.day;
			house.rent_paid_month = 0.0;
			house.rent_households_month = 0.0;
			house.rent_firms_month = 0.0;
			house.rent_government_month = 0.0;
			house.rent_burden = 0.0;
			house.units_built_month = 0.0;

			for (int tier = 0; tier < TIER_COUNT; ++tier) {
				// Desequilíbrio relativo no padrão (após a filtragem):
				// positivo = falta de unidade, negativo = sobra.
				const double imbalance = demand[tier] > 1e-9
					? (demand[tier] - supply[tier]) / demand[tier] : 0.0;
				const double step = std::clamp(
					imbalance * kDemoHousingRentElasticity,
					-kDemoHousingRentStepMax, kDemoHousingRentStepMax);
				double rent = safe_nonneg(house.rent[tier]) * (1.0 + step);
				// Âncora de renda: o aluguel de um domicílio não passa de
				// kDemoHousingRentBurdenCap da renda mensal do domicílio,
				// ponderado pelo gradiente de padrão da fonte.
				const double anchor = income_per_household_month
					* kDemoHousingRentBurdenCap * house.rent_cap_ratio[tier];
				if (anchor > 0.0) {
					rent = std::min(rent, anchor);
				}
				house.rent[tier] = safe_nonneg(rent);
				// §19.6: valor do imóvel = aluguel anual ÷ cap rate.
				house.price[tier] = house.rent[tier] * 12.0
					/ kDemoUrbanCapRate;
			}
		}

		// --- 5. Pagamento do aluguel ---------------------------------------
		// Inquilinos pagam o aluguel mensal ÷ 30. O pagamento é limitado pelo
		// ônus que a renda dos INQUILINOS suporta e pela poupança acima da
		// reserva de comida; o que não cabe fica como ônus (sinal de aperto),
		// não vira dívida inventada.
		double monthly_rent_bill = 0.0;
		for (int tier = 0; tier < TIER_COUNT; ++tier) {
			monthly_rent_bill += supply[tier]
				* safe_unit(house.tenure[tier][TENURE_RENT]) * house.rent[tier];
		}
		double paid = std::min(monthly_rent_bill / 30.0,
			std::min(tenant_income_day * kDemoHousingRentBurdenCap, spendable));
		paid = safe_nonneg(paid);
		house.rent_paid_today = paid;
		if (paid > 0.0) {
			pop.savings -= paid;
			// Manutenção: os donos pagam a conservação do estoque ao setor de
			// serviços (reparos). Fração declarada do aluguel — o valor de
			// reposição do estoque é ordens de grandeza maior que a moeda em
			// circulação, então a manutenção só pode sair da renda dele.
			const double maintenance = paid * kDemoHousingMaintenanceShare;
			const double net = paid - maintenance;
			city.service_cash += maintenance;
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
			city.capital_income_next += to_households;
			city.firm_cash += to_firms;
			gov.cash += to_government;
			house.rent_households_month += to_households;
			house.rent_firms_month += to_firms;
			house.rent_government_month += to_government;
			house.rent_paid_month += paid;
		}
		if (income_per_household_month > 0.0) {
			const double tenant_income_month = income_per_household_month
				* tenant_units;
			house.rent_burden = tenant_income_month > 1e-9
				? safe_unit(house.rent_paid_month / tenant_income_month) : 0.0;
		} else {
			house.rent_burden = 0.0;
		}

		// --- 6. Construção de moradia (projeto §15, tipo "moradia") --------
		auto free_slot = [&house]() -> int {
			for (int slot = 0; slot < kDemoHousingMaxProjects; ++slot) {
				if (house.projects[slot].state == HOUSING_PROJECT_IDLE) {
					return slot;
				}
			}
			return -1;
		};
		// Abre a obra: compra materiais no mercado local (§15), paga a mão de
		// obra ao setor de construção e agenda a entrega. O custo sai do dono
		// (firma ou POP) — nunca do nada. Sem material suficiente a obra não
		// abre (fica para o mês seguinte).
		auto open_project = [this, &city, &house](
				int slot, int state, int density, int tier, double units,
				double cost) -> bool {
			if (slot < 0 || slot >= kDemoHousingMaxProjects || units <= 0.0
					|| cost <= 0.0) {
				return false;
			}
			const bool self_build = state == HOUSING_PROJECT_SELF;
			if (self_build) {
				if (safe_nonneg(city.pop.savings) + 1e-9 < cost) {
					return false;
				}
			} else if (city.firm_cash + 1e-9 < cost) {
				return false;
			}
			const double material_budget = cost * kDemoHousingMaterialShare;
			double bought_cost = 0.0;
			double bought_qty[3] = {};
			int products[3] = {-1, -1, -1};
			for (int m = 0; m < 3; ++m) {
				products[m] = demo_product_index(kMaterials[m]);
				if (products[m] < 0) {
					continue;
				}
				DemoMarket& market = city.markets[products[m]];
				const double price = std::max(1e-9, market.price);
				const double qty = material_budget * kMaterialWeight[m] / price;
				if (market.stock + 1e-9 < qty) {
					return false;  // sem material: a obra não abre (nada pago)
				}
				bought_qty[m] = qty;
				bought_cost += qty * price;
			}
			// Só depois de garantir o material o dono paga a obra.
			if (self_build) {
				city.pop.savings -= cost;
			} else {
				city.firm_cash -= cost;
			}
			for (int m = 0; m < 3; ++m) {
				if (products[m] < 0 || bought_qty[m] <= 0.0) {
					continue;
				}
				DemoMarket& market = city.markets[products[m]];
				const double value = bought_qty[m] * std::max(1e-9, market.price);
				market.stock -= bought_qty[m];
				market.demand += bought_qty[m];
				market.sold += bought_qty[m];
				market.desired += bought_qty[m];
				city.firm_cash += value;
				city.revenue_by_product[products[m]] += value;
			}
			const double labor = std::max(0.0, cost - bought_cost);
			city.service_cash += labor;
			city.construction_revenue_pending += cost;
			HousingProject& project = house.projects[slot];
			project.state = state;
			project.density = density;
			project.tier = tier;
			project.units = units;
			project.days_left = kDemoHousingBuildDays;
			return true;
		};

		if (month_close) {
			// Incorporadora: constrói MID/ALTO quando há falta E o preço cobre
			// o custo com margem. O custo é expresso em ANOS DE ALUGUEL do
			// padrão (parâmetro declarado, independente da escala monetária do
			// mundo) mais a terra da zona residencial (W1 move o land_price).
			for (int tier = TIER_MEDIO; tier <= TIER_ALTO; ++tier) {
				const int slot = free_slot();
				if (slot < 0) {
					break;
				}
				const double shortage = std::max(0.0, demand[tier] - supply[tier]);
				if (shortage <= 0.0) {
					continue;
				}
				const int density = density_for_tier(
					land.max_density[ZONE_RES], tier);
				const double cost_unit = house.rent[tier] * 12.0
					* kDemoHousingBuildCostYears
					+ kDemoUrbanUnitM2[density][tier]
						* safe_nonneg(land.land_price[ZONE_RES]);
				if (house.price[tier]
						< cost_unit * (1.0 + kDemoHousingDeveloperMargin)) {
					continue;
				}
				const double units = std::min(shortage,
					demand[tier] * kDemoHousingBatchShare);
				open_project(slot, HOUSING_PROJECT_PRIVATE, density, tier,
					units, units * cost_unit);
			}
			// Autoconstrução da POP: só o padrão popular, só com falta e só com
			// a poupança acima da reserva de comida (a comida vem primeiro).
			if (popular_shortage > 0.02) {
				const int slot = free_slot();
				if (slot >= 0) {
					const double cost_unit =
						house.rent[TIER_POPULAR] * 12.0
							* kDemoHousingBuildCostYears
						+ kDemoUrbanUnitM2[DENSITY_LOW][TIER_POPULAR]
							* safe_nonneg(land.land_price[ZONE_RES]);
					const double units = std::min(popular_shortage_units,
						supply[TIER_POPULAR] * kDemoHousingSelfBuildShare);
					const double cost = units * cost_unit;
					// Não compromete mais de um quarto da folga de poupança.
					if (cost > 0.0 && cost * 4.0 <= spendable) {
						open_project(slot, HOUSING_PROJECT_SELF, DENSITY_LOW,
							TIER_POPULAR, units, cost);
					}
				}
			}
		}

		// --- 7. Favela (ÚLTIMO caso) ---------------------------------------
		// Só abre área informal quando a falta popular é grande, não há vaga em
		// padrão algum e o aperto já dura kDemoHousingSlumDays dias seguidos.
		if (month_close && house.stress_days >= kDemoHousingSlumDays
				&& slum_conditions) {
			double added = std::min(popular_shortage_units,
				demand[TIER_POPULAR] * kDemoHousingSlumShare);
			added = safe_nonneg(added);
			if (added >= 1.0) {
				house.units[DENSITY_LOW][TIER_POPULAR] += added;
				house.informal_units = safe_nonneg(house.informal_units + added);
				land.informal_km2 = safe_nonneg(land.informal_km2
					+ added * kDemoUrbanUnitM2[DENSITY_LOW][TIER_POPULAR]
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
					- kDemoHousingSlumSatisfaction * informal);
			}
		}

		// --- 8. Sinal de aperto habitacional -------------------------------
		// 0..1: falta de unidade no padrão popular + ônus de aluguel acima do
		// teto. É o insumo do W4 (migração) e da UI.
		const double burden_excess = income_per_household_month > 0.0
			? std::max(0.0, house.rent_burden - kDemoHousingRentBurdenCap) : 0.0;
		house.housing_stress = safe_unit(
			popular_shortage + burden_excess * 2.0);
	}
}

} // namespace ogs
