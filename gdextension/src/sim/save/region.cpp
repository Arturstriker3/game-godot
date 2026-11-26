// H1g — save/load por domínio: região (interior) por cidade.
//
// Blocos movidos VERBATIM de save_state.cpp (mesmas linhas, mesma ordem de
// escrita de chaves). Qualquer reordenação muda os bytes do save.
#include "save_internal.h"

#include "../sim_constants.h"
#include "../sim_core.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace ogs::save {
using namespace detail;

namespace region {

void write(Dictionary& entry, const City& city) {
	const Region& region = city.region;
	Dictionary region_entry = make_object();
	region_entry["ready"] = region.ready;
	region_entry["name"] = sim_str(region.name.c_str());
	region_entry["population_region"] = region.population_region;
	region_entry["urban_population"] = region.urban_population;
	region_entry["region_pop"] = region.region_pop;
	region_entry["urban_share"] = region.urban_share;
	region_entry["cash"] = region.cash;
	// B7: a renda do dia (semanal) é usada pelo serviço informal diário
	// (compute_region_informal_services); sem persistir, o load zerava a
	// renda e a fatia informal divergia do caminho direto. São fluxos
	// derivados, mas o serviço informal depende deles entre os fechos
	// semanais — por isso viajam no save.
	region_entry["wage_income"] = region.wage_income;
	region_entry["income"] = region.income;
	region_entry["taxes"] = region.taxes;
	Array mix = make_array();
	mix.append(region.employment_mix[0]);
	mix.append(region.employment_mix[1]);
	mix.append(region.employment_mix[2]);
	region_entry["employment_mix"] = mix;
	Array region_states = make_array();
	for (const RegionState& state : region.states) {
		Dictionary state_entry = make_object();
		state_entry["id"] = sim_str(state.id.c_str());
		state_entry["name"] = sim_str(state.name.c_str());
		state_entry["population"] = state.population;
		state_entry["area_km2"] = state.area_km2;
		region_states.append(state_entry);
	}
	region_entry["states"] = region_states;			Array region_age_bands = make_array();
	if (region.demography_ready) {
		for (int band = 0; band < kAgeBands; ++band) {
			region_age_bands.append(std::max(0.0, region.age_band[band]));
		}
	}
	region_entry["age_bands"] = region_age_bands;
	Dictionary region_strata = make_object();
	region_strata["ready"] = region.strata_ready;
	Array region_income_share = make_array();
	Array region_people = make_array();
	Array region_savings = make_array();
	for (int s = 0; s < kIncomeStrata; ++s) {
		region_income_share.append(region.strata_income_share[s]);
		region_people.append(region.stratum_people[s]);
		region_savings.append(region.stratum_savings[s]);
	}
	region_strata["income_share"] = region_income_share;
	region_strata["people"] = region_people;
	region_strata["savings"] = region_savings;
	region_entry["strata"] = region_strata;
	// C1 (save v26): salário rural atrasado a receber (ativo das
	// famílias do interior).
	region_entry["arrears"] = region.arrears_receivable;
	// B4 (save v20): migração — acumuladores (o fluxo do mês é derivado).
	Dictionary region_migration = make_object();
	region_migration["rural_in_total"] = region.migration_rural_in_total;
	region_migration["city_in_total"] = region.migration_city_in_total;
	region_migration["city_out_total"] = region.migration_city_out_total;
	region_entry["migration"] = region_migration;
	// B5 (save v21): infraestrutura do estado — níveis (a manutenção do
	// dia é derivada e não vai ao save).
	Dictionary region_infra = make_object();
	region_infra["road"] = region.infra_road;
	region_infra["power"] = region.infra_power;
	region_infra["water"] = region.infra_water;
	region_entry["infra"] = region_infra;
	// B6 (save v23): patrimônio da região — terra, moradia, anel. Os
	// valores de ativo NÃO são dinheiro; a alíquota (0 por padrão) e a
	// receita potencial são campo publicado, não arrecadação.
	{
		Dictionary region_property = make_object();
		region_property["ready"] = region.property_ready;
		region_property["land_ha"] = region.land_ha;
		region_property["land_gini"] = region.land_gini;
		region_property["land_gini_observed"] =
			region.land_gini_observed;
		region_property["land_value_per_ha"] = region.land_value_per_ha;
		region_property["land_book_value_per_ha"] =
			region.land_book_value_per_ha;
		region_property["land_income_month"] = region.land_income_month;
		region_property["commodity_index"] = region.commodity_index;
		region_property["housing_price"] = region.housing_price;
		region_property["housing_book_price"] = region.housing_book_price;
		region_property["housing_rent"] = region.housing_rent;
		region_property["ring_population"] = region.ring_population;
		region_property["ring_price"] = region.ring_price;
		region_property["ring_book_price"] = region.ring_book_price;
		region_property["ring_rent"] = region.ring_rent;
		region_property["ring_rent_paid_month"] =
			region.ring_rent_paid_month;
		region_property["migration_ring_in_total"] =
			region.migration_ring_in_total;
		region_property["migration_month_ring_in"] =
			region.migration_month_ring_in;
		region_property["tax_rate"] = region.property_tax_rate;
		region_property["tax_potential"] = region.property_tax_potential;
		const auto put_values = [&region_property](
				const char* key, const double* values) {
			Array row = make_array();
			for (int s = 0; s < kIncomeStrata; ++s) {
				row.append(values[s]);
			}
			region_property[key] = row;
		};
		put_values("land_value", region.stratum_land_value);
		put_values("home_value", region.stratum_home_value);
		put_values("ring_value", region.stratum_ring_value);
		put_values("wealth", region.stratum_wealth);
		region_entry["property"] = region_property;
	}
	// B7 (save v22): informalidade, mortalidade e insatisfação (interior).
	region_entry["informal"] = save_informal_block(region.informal_share,
		region.informal_labor_share, region.informal_income,
		region.informal_wage_bill, region.informal_service_workers,
		region.informal_service_output, region.informal_service_demand,
		region.informal_service_revenue, region.informal_service_price,
		region.informal_service_share);
	region_entry["mortality"] = save_mortality_block(region.deaths_expected,
		region.deaths_total, region.excess_deaths_total,
		region.excess_deaths_famine_total, region.famine_months,
		region.food_price_ref, region.food_inflation_month,
		region.health_access_ref, region.satisfaction_prev);
	region_entry["dissatisfaction"] = save_dissatisfaction_block(
		region.dissatisfaction, region.dissatisfaction_by_stratum,
		region.dissatisfaction_unemployment, region.dissatisfaction_rent,
		region.dissatisfaction_food, region.dissatisfaction_inflation,
		region.dissatisfaction_informality);
	entry["region"] = region_entry;
}

void read(Dictionary& entry, City& city, const WorldState& ws,
		SimCore& core, int version) {
	// B3 (save v19): região da cidade (estados inteiros). Save v18 e
	// anteriores migram no mundo real: `region_pop = max(0, population -
	// simulation_population)` (dado já no save), coortes/estratos da nação e
	// mix neutro (sem dado de urbanização). Nada é inventado.
	Region region;
	const Dictionary saved_region = entry.get("region", Dictionary());
	if (version >= 19 && !saved_region.is_empty()) {
		region.ready = bool(saved_region.get("ready", true));
		region.name = std::string(
			String(saved_region.get("name", "")).utf8().get_data());
		region.population_region = safe_nonneg(
			double(saved_region.get("population_region", 0.0)));
		region.urban_population = safe_nonneg(
			double(saved_region.get("urban_population", 0.0)));
		region.region_pop = safe_nonneg(
			double(saved_region.get("region_pop", 0.0)));
		const double urban_share = double(
			saved_region.get("urban_share", -1.0));
		region.urban_share = std::isfinite(urban_share)
			&& urban_share >= 0.0 ? std::clamp(urban_share, 0.0, 1.0)
			: -1.0;
		region.cash = std::max(0.0,
			double(saved_region.get("cash", 0.0)));
		// B7: renda do dia (semanal), usada pelo serviço informal diário.
		region.wage_income = safe_nonneg(
			double(saved_region.get("wage_income", 0.0)));
		region.income = safe_nonneg(
			double(saved_region.get("income", 0.0)));
		region.taxes = safe_nonneg(
			double(saved_region.get("taxes", 0.0)));
		const Array mix = saved_region.get("employment_mix", Array());
		for (int m = 0; m < 3 && m < mix.size(); ++m) {
			region.employment_mix[m] = safe_nonneg(double(mix[m]));
		}
		const Array region_states = saved_region.get("states", Array());
		for (int index = 0; index < region_states.size(); ++index) {
			if (region_states[index].get_type() != Variant::DICTIONARY) {
				continue;
			}
			const Dictionary state_entry = region_states[index];
			RegionState state;
			state.id = std::string(
				String(state_entry.get("id", "")).utf8().get_data());
			state.name = std::string(
				String(state_entry.get("name", "")).utf8().get_data());
			state.population = safe_nonneg(
				double(state_entry.get("population", 0.0)));
			state.area_km2 = safe_nonneg(
				double(state_entry.get("area_km2", 0.0)));
			region.states.push_back(std::move(state));
		}
		const Array region_age_bands =
			saved_region.get("age_bands", Array());
		if (region_age_bands.size() >= kAgeBands) {
			double total = 0.0;
			for (int band = 0; band < kAgeBands; ++band) {
				region.age_band[band] =
					std::max(0.0, double(region_age_bands[band]));
				total += region.age_band[band];
			}
			if (total > 0.0) {
				region.demography_ready = true;
			}
		}
		const Dictionary region_strata =
			saved_region.get("strata", Dictionary());
		region.strata_ready = bool(region_strata.get("ready", true));
		const auto read_region_strata = [&region_strata](
				const char* key, double* out) {
			const Array values = region_strata.get(key, Array());
			for (int s = 0; s < kIncomeStrata; ++s) {
				out[s] = s < values.size()
					? std::max(0.0, double(values[s])) : 0.0;
			}
		};
		read_region_strata("income_share", region.strata_income_share);
		read_region_strata("people", region.stratum_people);
		read_region_strata("savings", region.stratum_savings);
		// C1 (save v26): salário rural atrasado a receber. v25 e
		// anteriores = 0.
		region.arrears_receivable = version >= 26
			? std::max(0.0, double(saved_region.get("arrears", 0.0)))
			: 0.0;
		// B4 (save v20): acumuladores de migração. v19 e anteriores não
		// têm o bloco — nascem zerados (a migração conta do dia 0 do
		// save novo; nada é inventado para trás).
		const Dictionary region_migration =
			saved_region.get("migration", Dictionary());
		region.migration_rural_in_total = safe_nonneg(
			double(region_migration.get("rural_in_total", 0.0)));
		region.migration_city_in_total = safe_nonneg(
			double(region_migration.get("city_in_total", 0.0)));
		region.migration_city_out_total = safe_nonneg(
			double(region_migration.get("city_out_total", 0.0)));
		// B5 (save v21): níveis de infra. v20 e anteriores não têm o
		// bloco — nascem zerados (nível 0 = ausência explícita; nada é
		// inventado para trás). O valor é limitado a 0..kInfraMaxLevel.
		const Dictionary region_infra =
			saved_region.get("infra", Dictionary());
		region.infra_road = std::clamp(int(region_infra.get("road", 0)),
			0, kInfraMaxLevel);
		region.infra_power = std::clamp(int(region_infra.get("power", 0)),
			0, kInfraMaxLevel);
		region.infra_water = std::clamp(int(region_infra.get("water", 0)),
			0, kInfraMaxLevel);
		// B6 (save v23): patrimônio da região. v22 e anteriores NÃO têm
		// o bloco: a região fica inerte (`property_ready` falso) até o
		// dado ser reaplicado pelo GDScript (`set_region_property`), e a
		// alíquota nasce 0 — nada é inventado para trás.
		if (version >= 23) {
			const Dictionary region_property =
				saved_region.get("property", Dictionary());
			region.land_ha = safe_nonneg(
				double(region_property.get("land_ha", 0.0)));
			// Sem hectares não há patrimônio: a região fica inerte mesmo
			// que o bloco exista (ausência explícita).
			region.property_ready =
				bool(region_property.get("ready", true))
				&& region.land_ha > 0.0;
			region.land_gini = std::clamp(
				double(region_property.get("land_gini", 0.0)), 0.0, 1.0);
			region.land_gini_observed =
				bool(region_property.get("land_gini_observed", false));
			region.land_value_per_ha = safe_nonneg(
				double(region_property.get("land_value_per_ha", 0.0)));
			region.land_book_value_per_ha = safe_nonneg(
				double(region_property.get("land_book_value_per_ha", 0.0)));
			region.land_income_month = safe_nonneg(
				double(region_property.get("land_income_month", 0.0)));
			region.commodity_index = safe_nonneg(
				double(region_property.get("commodity_index", 1.0)));
			region.housing_price = safe_nonneg(
				double(region_property.get("housing_price", 0.0)));
			region.housing_book_price = safe_nonneg(
				double(region_property.get("housing_book_price", 0.0)));
			region.housing_rent = safe_nonneg(
				double(region_property.get("housing_rent", 0.0)));
			region.ring_population = safe_nonneg(
				double(region_property.get("ring_population", 0.0)));
			region.ring_price = safe_nonneg(
				double(region_property.get("ring_price", 0.0)));
			region.ring_book_price = safe_nonneg(
				double(region_property.get("ring_book_price", 0.0)));
			region.ring_rent = safe_nonneg(
				double(region_property.get("ring_rent", 0.0)));
			region.ring_rent_paid_month = safe_nonneg(
				double(region_property.get("ring_rent_paid_month", 0.0)));
			region.migration_ring_in_total = safe_nonneg(
				double(region_property.get("migration_ring_in_total", 0.0)));
			region.migration_month_ring_in = safe_nonneg(
				double(region_property.get("migration_month_ring_in", 0.0)));
			region.property_tax_rate = std::clamp(
				double(region_property.get("tax_rate", 0.0)), 0.0, 1.0);
			region.property_tax_potential = safe_nonneg(
				double(region_property.get("tax_potential", 0.0)));
			const auto read_property_values = [&region_property](
					const char* key, double* out) {
				const Array values =
					region_property.get(key, Array());
				for (int s = 0; s < kIncomeStrata; ++s) {
					out[s] = s < values.size()
						? safe_nonneg(double(values[s])) : 0.0;
				}
			};
			read_property_values("land_value", region.stratum_land_value);
			read_property_values("home_value", region.stratum_home_value);
			read_property_values("ring_value", region.stratum_ring_value);
			read_property_values("wealth", region.stratum_wealth);
			// O anel é um subconjunto do interior (invariante de pessoas).
			region.ring_population = std::min(region.ring_population,
				std::max(0.0, region.region_pop));
		}
		// B7 (save v22): informalidade, mortalidade e insatisfação do
		// interior. v21 e anteriores migram na segunda passada.
		if (version >= 22) {
			load_informal_block(saved_region.get("informal", Dictionary()),
				region.informal_share, region.informal_labor_share,
				region.informal_income, region.informal_wage_bill,
				region.informal_service_workers, region.informal_service_output,
				region.informal_service_demand, region.informal_service_revenue,
				region.informal_service_price, region.informal_service_share);
			load_mortality_block(saved_region.get("mortality", Dictionary()),
				region.deaths_expected, region.deaths_total,
				region.excess_deaths_total, region.excess_deaths_famine_total,
				region.famine_months, region.food_price_ref,
				region.food_inflation_month, region.health_access_ref,
				region.satisfaction_prev);
			load_dissatisfaction_block(
				saved_region.get("dissatisfaction", Dictionary()),
				region.dissatisfaction, region.dissatisfaction_by_stratum,
				region.dissatisfaction_unemployment, region.dissatisfaction_rent,
				region.dissatisfaction_food, region.dissatisfaction_inflation,
				region.dissatisfaction_informality);
			region.informality_seeded = true;
		}
	} else if (ws.world_mode) {
		// v18 → v19: interior = população regional exibida - população de
		// simulação (ambos já no save). Cenário sintético (world_mode
		// false) continua sem região (ausência explícita).
		region.ready = true;
		region.population_region = safe_nonneg(city.population);
		region.urban_population = safe_nonneg(city.simulation_population);
		region.region_pop = std::max(0.0,
			city.population - city.simulation_population);
		region.urban_share = -1.0;
	}
	city.region = std::move(region);
	if (city.region.ready) {
		// Reconciliia sem re-semear os quintis já carregados do save
		// (seed_region resetaria a poupança/people lidas acima).
		core.recompute_region_mix(city);
		core.apply_region_demography(city);
		if (!city.region.strata_ready) {
			core.seed_region_strata(city, nullptr);
		}
	}
}

} // namespace region

} // namespace ogs::save
