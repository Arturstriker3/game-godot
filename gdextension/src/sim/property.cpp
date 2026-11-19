// property.cpp — B6: patrimônio imobiliário da REGIÃO (terra produtiva,
// moradia do interior e anel periurbano da cidade-polo).
//
// Agregado por região, cadência MENSAL (A6), sem prédio a prédio e sem
// compra/venda individual. C++ puro (STL via sim_core.h), sem godot_cpp.
//
// Modelo (docs/economy/B6_region_property.md, D-095):
//   - TERRA: os hectares vêm do dado (land_property.json; o GDScript aplica a
//     regra de rateio nacional→estado e declara por região em
//     `set_region_property`). O valor por hectare é a renda fundiária ANUAL da
//     região capitalizada pelo cap rate (kPropertyCapRate) e multiplicada pela
//     infra B5 e pelo índice de commodities (preço ÷ preço-base D0 dos
//     produtos primários da cidade-polo). Amortecimento mensal declarado.
//   - MORADIA: a do interior é autoconstruída (sem mercado de solo): vale uma
//     fração declarada da unidade POPULAR do polo. O ANEL PERIURBANO vale o
//     preço do polo descontado por distância (raio efetivo real da região) e
//     pela estrada B5; o aluguel sobe com a fatia da população que vive nele.
//   - PROPRIEDADE POR QUINTIL: a terra segue o Gini FUNDIÁRIO declarado (mesma
//     lognormal da B2) quando observado; sem ele, a repartição de RENDA da
//     região. Moradia e anel seguem a concentração de capital
//     (kStrataWealthConcentration). Terra + moradia + anel entram em
//     `stratum_wealth` (poupança + imóveis + terra + participações 0 até C5).
//   - ANEL × MIGRAÇÃO B4: quando o polo está cheio, o excedente do êxodo
//     assenta no anel (vaga mensal declarada) em vez de ser barrado. O morador
//     do anel continua em `region_pop` (é um SUBCONJUNTO): pessoas conservadas.
//   - DINHEIRO: valor de ativo não é dinheiro. O único fluxo é o aluguel do
//     anel, debitado dos quintis (pela poupança) e creditado aos MESMOS quintis
//     donos (pela renda) na MESMA quantia: `money_total` inalterado.
//   - BALANÇO (pronto para a C1): cada ativo publicado em `get_region_status`
//     tem tipo, dono, quantidade, valor de mercado, valor contábil (1ª
//     avaliação) e fluxo mensal — a contabilidade por balanço só lê os campos.

#include "sim_core.h"

#include "profiling.h"
#include "sim_constants.h"

#include <algorithm>
#include <cmath>

namespace ogs {

namespace {

constexpr double kPropEps = 1e-12;

// Fração de uma distribuição por quintil (20% de pessoas cada) a partir de um
// Gini, pela MESMA lognormal da B2: E[X·1{Q_prev<X<Q_next}]/E[X] =
// Φ(z_next−σ) − Φ(z_prev−σ), com z = Φ⁻¹(q). Gini ~0 devolve quintis iguais.
void shares_from_gini(double gini, double* shares) {
	gini = std::clamp(gini, 0.0, 1.0);
	double total = 0.0;
	if (gini <= kPropEps) {
		for (int s = 0; s < kIncomeStrata; ++s) {
			shares[s] = 1.0 / double(kIncomeStrata);
		}
		return;
	}
	double sigma = std::sqrt(2.0) * normal_quantile(0.5 * (1.0 + gini));
	sigma = std::clamp(sigma, 0.15, 1.50);
	double previous = 0.0;
	for (int s = 0; s < kIncomeStrata; ++s) {
		const double upper = double(s + 1) / double(kIncomeStrata);
		shares[s] = std::max(0.0,
			normal_cdf(normal_quantile(upper) - sigma)
			- normal_cdf(normal_quantile(previous) - sigma));
		total += shares[s];
		previous = upper;
	}
	if (total <= kPropEps) {
		for (int s = 0; s < kIncomeStrata; ++s) {
			shares[s] = 1.0 / double(kIncomeStrata);
		}
		return;
	}
	for (int s = 0; s < kIncomeStrata; ++s) {
		shares[s] /= total;
	}
}

// Normaliza um vetor de pesos; tudo zero → quintis iguais (ausência explícita).
void normalize_shares(double* shares) {
	double total = 0.0;
	for (int s = 0; s < kIncomeStrata; ++s) {
		shares[s] = std::max(0.0, shares[s]);
		total += shares[s];
	}
	if (total <= kPropEps) {
		for (int s = 0; s < kIncomeStrata; ++s) {
			shares[s] = 1.0 / double(kIncomeStrata);
		}
		return;
	}
	for (int s = 0; s < kIncomeStrata; ++s) {
		shares[s] /= total;
	}
}

// Donos de moradia/anel: riqueza mais concentrada que a renda (mesma regra da
// participação no capital da B2, `kStrataWealthConcentration`). A região não tem
// participação no capital própria (B3 só guarda renda/pessoas/poupança), então a
// posse segue a MESMA concentração aplicada à repartição de renda da região.
void ownership_shares(const Region& region, double* shares) {
	for (int s = 0; s < kIncomeStrata; ++s) {
		shares[s] = std::pow(std::max(region.strata_income_share[s], 1e-6),
			kStrataWealthConcentration);
	}
	normalize_shares(shares);
}

// Tamanho do domicílio da região: o MESMO da cidade-polo. A fonte (§19) define
// o estoque urbano como população ÷ tamanho do domicílio, então a razão
// `pop_anchor ÷ households` devolve o tamanho sem inventar número. 0 = sem
// dado urbano (a B6 fica inerte para moradia naquela região).
double household_size(const WorldState& ws, std::size_t city_index) {
	if (city_index >= ws.housing.size()) {
		return 0.0;
	}
	const CityHousing& house = ws.housing[city_index];
	if (!(house.households > kPropEps) || !(house.pop_anchor > kPropEps)) {
		return 0.0;
	}
	return house.pop_anchor / house.households;
}

// Raio efetivo da região (km) a partir da ÁREA REAL dos estados anexados
// (admin-1 do Natural Earth): círculo de mesma área.
double region_radius_km(const Region& region) {
	double area = 0.0;
	for (const RegionState& state : region.states) {
		area += std::max(0.0, state.area_km2);
	}
	if (area <= kPropEps) {
		return 0.0;
	}
	return std::sqrt(area / 3.14159265358979323846);
}

} // namespace

// ---------------------------------------------------------------------------
// Semeadura (chamada pela fachada depois do mundo/load, via GDScript)
// ---------------------------------------------------------------------------
void SimCore::seed_region_property(City& city, double land_ha,
		double land_gini, bool gini_observed) {
	Region& region = city.region;
	if (!region.ready || region.property_ready) {
		return;  // sem região ou já tem estado (save v23 manda)
	}
	if (!(land_ha > 0.0) || !std::isfinite(land_ha)) {
		return;  // sem hectare = ausência explícita (não se inventa terra)
	}
	region.property_ready = true;
	region.land_ha = land_ha;
	region.land_gini = std::clamp(
		std::isfinite(land_gini) ? land_gini : 0.0, 0.0, 1.0);
	region.land_gini_observed = gini_observed && region.land_gini > 0.0;
	// O valor de mercado nasce no PRIMEIRO fechamento mensal: no dia 0 ainda não
	// há renda primária medida — nada de âncora inventada.
	region.land_value_per_ha = 0.0;
	region.land_book_value_per_ha = 0.0;
	region.ring_population = 0.0;
	region.migration_ring_in_total = 0.0;
	region.migration_month_ring_in = 0.0;
	region.property_tax_rate = std::clamp(
		std::isfinite(region.property_tax_rate) ? region.property_tax_rate
			: kPropertyTaxDefaultRate, 0.0, 1.0);
}

// ---------------------------------------------------------------------------
// Multiplicadores e medidas derivadas
// ---------------------------------------------------------------------------
double SimCore::property_infra_multiplier(const Region& region) {
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
	return 1.0 + kPropertyInfraValueGain * std::clamp(covered, 0.0, 1.0);
}

double SimCore::region_commodity_index(const City& city) const {
	const std::size_t city_index = static_cast<std::size_t>(
		&city - ws_.cities.data());
	if (city_index >= ws_.buildings_by_city.size()) {
		return 1.0;
	}
	double weighted = 0.0;
	double weight = 0.0;
	for (const int bi : ws_.buildings_by_city[city_index]) {
		const Building& building = ws_.buildings[static_cast<std::size_t>(bi)];
		// Extração primária (fazenda/mina): receita e insumo não declarados.
		if (building.recipe >= 0 || building.input_product >= 0) {
			continue;
		}
		if (building.output_product < 0 || building.output_product >= ws_.products) {
			continue;
		}
		const double w = std::max(0.0,
			building.capacity * building.output_scale);
		if (w <= kPropEps) {
			continue;
		}
		const double base = base_price(building.output_product);
		const double price = city.markets[building.output_product].price;
		if (!(base > 0.0) || !(price > 0.0) || !std::isfinite(price)) {
			continue;
		}
		weighted += w * (price / base);
		weight += w;
	}
	if (weight <= kPropEps) {
		return 1.0;  // sem produtor primário: neutro (ausência explícita)
	}
	return std::clamp(weighted / weight,
		kPropertyCommodityIndexMin, kPropertyCommodityIndexMax);
}

double SimCore::region_property_value(const Region& region) const {
	double total = 0.0;
	for (int s = 0; s < kIncomeStrata; ++s) {
		total += std::max(0.0, region.stratum_land_value[s])
			+ std::max(0.0, region.stratum_home_value[s])
			+ std::max(0.0, region.stratum_ring_value[s]);
	}
	return total;
}

double SimCore::ring_vacancy_people(const City& city) const {
	const Region& region = city.region;
	if (!region.ready || !region.property_ready) {
		return 0.0;
	}
	// Vaga MENSAL do anel: fatia declarada da população urbana. O anel não tem
	// estoque medido — a periferia informal se constrói ao ritmo da chegada.
	const double vacancy = kPropertyRingMonthlyAbsorb
		* std::max(0.0, city.simulation_population);
	const double room = std::max(0.0,
		region.region_pop - region.ring_population);
	return std::min(vacancy, room);
}

double SimCore::ring_home_cost_per_day(const City& city) const {
	const Region& region = city.region;
	if (!region.ready || !region.property_ready) {
		return 0.0;
	}
	const double size = household_size(ws_, static_cast<std::size_t>(
		&city - ws_.cities.data()));
	if (!(size > kPropEps)) {
		return 0.0;
	}
	// Aluguel mensal por unidade ÷ pessoas do domicílio ÷ dias do mês.
	return std::max(0.0, region.ring_rent) / size
		/ double(std::max(1, kUrbanMonthDays));
}

double SimCore::quintile_gini(const double* values, int count) {
	if (values == nullptr || count <= 0) {
		return 0.0;
	}
	double total = 0.0;
	for (int index = 0; index < count; ++index) {
		if (!std::isfinite(values[index]) || values[index] < 0.0) {
			return 0.0;
		}
		total += values[index];
	}
	if (total <= kPropEps) {
		return 0.0;
	}
	double pairwise = 0.0;
	for (int a = 0; a < count; ++a) {
		for (int b = 0; b < count; ++b) {
			pairwise += std::abs(values[a] - values[b]);
		}
	}
	return std::clamp(pairwise / (2.0 * double(count) * total), 0.0, 1.0);
}

// ---------------------------------------------------------------------------
// Fase mensal
// ---------------------------------------------------------------------------
void SimCore::phase_property() {
	const int month_period = ws_.cadence.monthly_period;
	OGS_SIM_ZONE(kPhaseProperty);
	for (std::size_t city_index = 0; city_index < ws_.cities.size();
			++city_index) {
		City& city = ws_.cities[city_index];
		Region& region = city.region;
		if (!region.ready || !region.property_ready) {
			continue;
		}
		// Invariante do anel: ele é um subconjunto do interior (pessoas nunca
		// são criadas; a demografia do mês pode ter reduzido o interior).
		region.ring_population = std::min(
			std::max(0.0, region.ring_population),
			std::max(0.0, region.region_pop));
		if (!cadence_due(ws_.day, city_index, month_period)) {
			continue;
		}

		const double size = household_size(ws_, city_index);
		const CityHousing* house = city_index < ws_.housing.size()
			? &ws_.housing[city_index] : nullptr;
		const double pole_price = house != nullptr
			? std::max(0.0, house->price[TIER_POPULAR]) : 0.0;

		// --- 1. TERRA: renda fundiária capitalizada × infra B5 × commodities.
		const double land_ha = std::max(0.0, region.land_ha);
		region.land_income_month = std::max(0.0, region.wage_income)
			* double(kUrbanMonthDays);
		region.commodity_index = region_commodity_index(city);
		double target_per_ha = 0.0;
		if (land_ha > kPropEps && kPropertyCapRate > 0.0
				&& region.land_income_month > 0.0) {
			const double annual_income_per_ha =
				region.land_income_month * 12.0 / land_ha;
			target_per_ha = annual_income_per_ha / kPropertyCapRate
				* property_infra_multiplier(region)
				* region.commodity_index;
		}
		if (target_per_ha > 0.0) {
			if (region.land_value_per_ha <= kPropEps) {
				// Primeira avaliação: ancora no alvo (sem base anterior).
				region.land_value_per_ha = target_per_ha;
			} else {
				const double step = region.land_value_per_ha
					* kPropertyValueStepMax;
				const double delta = std::clamp(
					(target_per_ha - region.land_value_per_ha)
						* kPropertyValueDamping, -step, step);
				region.land_value_per_ha = std::max(0.0,
					region.land_value_per_ha + delta);
			}
		}
		// Sem renda medida no mês, o valor FICA no último observado (ausência
		// explícita; zero inventado derrubaria o ativo de quem só não pagou).
		if (region.land_book_value_per_ha <= kPropEps
				&& region.land_value_per_ha > kPropEps) {
			region.land_book_value_per_ha = region.land_value_per_ha;
		}
		const double land_value = land_ha * region.land_value_per_ha;

		// --- 2. Distribuição da TERRA por quintil (Gini fundiário ou renda).
		double land_share[kIncomeStrata];
		if (region.land_gini_observed) {
			shares_from_gini(region.land_gini, land_share);
		} else {
			// Fallback declarado: a concentração de terra segue a regra de renda
			// já existente na região (quintis observados ou proxy do Gini).
			for (int s = 0; s < kIncomeStrata; ++s) {
				land_share[s] = region.strata_income_share[s];
			}
			normalize_shares(land_share);
		}
		for (int s = 0; s < kIncomeStrata; ++s) {
			region.stratum_land_value[s] = land_value * land_share[s];
		}

		// --- 3. MORADIA do interior (autoconstruída) e ANEL periurbano.
		// Moradores do ANEL não ocupam casa rural: o estoque do interior é a
		// população do interior MENOS o anel (o anel tem o seu próprio estoque).
		const double rural_pop = std::max(0.0,
			region.region_pop - region.ring_population);
		const double housing_units = size > kPropEps ? rural_pop / size : 0.0;
		region.housing_price = pole_price * kPropertyRuralHomeShare;
		region.housing_rent = region.housing_price * kPropertyCapRate / 12.0;
		if (region.housing_book_price <= kPropEps
				&& region.housing_price > kPropEps) {
			region.housing_book_price = region.housing_price;
		}
		// Anel: mesmo preço popular do polo, descontado por distância real e
		// pela estrada B5 (o preço não tem dinâmica própria).
		const double radius_km = region_radius_km(region);
		const double road_cover = kInfraMaxLevel > 0
			? std::clamp(double(region.infra_road) / double(kInfraMaxLevel),
				0.0, 1.0) : 0.0;
		const double distance_discount = 1.0 / (1.0
			+ kPropertyRingDistanceDiscount * radius_km / 100.0);
		const double road_discount = 1.0
			- kPropertyRingRoadPenalty * (1.0 - road_cover);
		region.ring_price = pole_price * distance_discount * road_discount;
		if (region.ring_book_price <= kPropEps && region.ring_price > kPropEps) {
			region.ring_book_price = region.ring_price;
		}
		const double ring_units = size > kPropEps
			? std::max(0.0, region.ring_population) / size : 0.0;
		const double ring_share = std::clamp(region.ring_population
			/ std::max(1.0, city.simulation_population + region.ring_population),
			0.0, 1.0);
		region.ring_rent = std::max(0.0, region.ring_price)
			* kPropertyCapRate / 12.0
			* (1.0 + kPropertyRingRentPressure * ring_share);
		// Aluguel do anel: débito dos quintis (poupança) e crédito dos MESMOS
		// quintis donos (renda) na MESMA quantia — `money_total` não muda.
		const double nominal_rent = ring_units * region.ring_rent;
		double available = 0.0;
		for (int s = 0; s < kIncomeStrata; ++s) {
			available += std::max(0.0, region.stratum_savings[s]);
		}
		const double rent_paid = std::min(nominal_rent, available);
		if (rent_paid > kPropEps) {
			region_charge(region, rent_paid);
			region_credit(region, rent_paid);
		}
		region.ring_rent_paid_month = rent_paid;

		// --- 4. Distribuição da moradia/anel por quintil (donos) e PATRIMÔNIO.
		double owner_share[kIncomeStrata];
		ownership_shares(region, owner_share);
		for (int s = 0; s < kIncomeStrata; ++s) {
			region.stratum_home_value[s] =
				housing_units * region.housing_price * owner_share[s];
			region.stratum_ring_value[s] =
				ring_units * region.ring_price * owner_share[s];
			region.stratum_wealth[s] = std::max(0.0, region.stratum_savings[s])
				+ region.stratum_land_value[s]
				+ region.stratum_home_value[s]
				+ region.stratum_ring_value[s];
		}

		// --- 5. Imposto sobre a propriedade: só CAMPO + receita potencial.
		const double tax_base = region_property_value(region);
		region.property_tax_potential = std::clamp(
			region.property_tax_rate, 0.0, 1.0) * tax_base / 12.0;
	}
	OGS_SIM_PHASE_LAP(kPhaseProperty);
}

} // namespace ogs
