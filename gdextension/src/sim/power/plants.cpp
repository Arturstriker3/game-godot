// H1f — sim/power/plants.cpp: usinas (custo, construção, combustível,
// parque inicial e estabilização pós-load). Blocos movidos VERBATIM de
// `sim/power.cpp`; nenhuma regra, ordem ou número mudou.
#include "power_internal.h"

#include "../sim_core.h"
#include "../world_state.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace ogs {

namespace {

const char* const kFuelNames[kPowerFuelCount] = {
	"hydro", "solar", "wind", "coal", "gas", "oil", "nuclear", "biomass",
	"other"};

} // namespace

const char* power_fuel_name(int fuel) {
	if (fuel < 0 || fuel >= kPowerFuelCount) {
		return "unknown";
	}
	return kFuelNames[fuel];
}

int power_fuel_from_name(const std::string& name) {
	for (int fuel = 0; fuel < kPowerFuelCount; ++fuel) {
		if (name == kFuelNames[fuel]) {
			return fuel;
		}
	}
	return -1;
}

bool power_fuel_is_thermal(int fuel) {
	return fuel == POWER_COAL || fuel == POWER_GAS || fuel == POWER_OIL
		|| fuel == POWER_NUCLEAR || fuel == POWER_BIOMASS
		|| fuel == POWER_OTHER;
}

bool power_fuel_is_renewable(int fuel) {
	return fuel == POWER_HYDRO || fuel == POWER_SOLAR || fuel == POWER_WIND;
}

double power_local_cf(const PowerParams& params, int fuel,
		double solar_month_mean, double wind_ms) {
	if (fuel < 0 || fuel >= kPowerFuelCount) {
		return 0.0;
	}
	double cf = std::clamp(params.cf[fuel], 0.0, 1.0);
	if (fuel == POWER_SOLAR && solar_month_mean > 0.0) {
		// PVOUT médio do sítio (kWh/kWp/dia ÷ 24 h) é o CF solar local.
		cf = solar_month_mean;
	} else if (fuel == POWER_WIND && wind_ms > 0.0) {
		// Curva declarada: a potência do vento cresce com v^3; o teto abaixo
		// mantém o CF dentro da faixa realista (Betz, disponibilidade, esteira).
		cf = params.wind_cf_ref * std::pow(
			std::max(0.0, wind_ms) / params.wind_cf_ref_ms,
			params.wind_cf_exponent);
	}
	cf = std::clamp(cf, 0.0, std::clamp(params.cf_cap[fuel], 0.0, 1.0));
	return std::clamp(cf * params.balance_penalty[fuel], 0.0, 1.0);
}


double power_new_capex_per_mw(const PowerParams& params, int fuel) {
	if (fuel < 0 || fuel >= kPowerFuelCount) {
		return 0.0;
	}
	return std::max(0.0, params.capex_per_mw[fuel])
		* std::max(0.0, params.capex_mult[fuel]);
}

int power_build_days(const PowerParams& params, int fuel) {
	if (fuel == POWER_HYDRO) {
		return std::max(params.build_days_plant, params.build_days_hydro);
	}
	return std::max(0, params.build_days_plant);
}

void apply_power_plant_project(WorldState& ws,
		const ConstructionProject& project) {
	if (ws.power_cities.size() != ws.cities.size()) {
		ws.power_cities.resize(ws.cities.size());
	}
	for (std::size_t index = 0; index < ws.cities.size(); ++index) {
		if (ws.cities[index].id != project.city) {
			continue;
		}
		PowerCityState& state = ws.power_cities[index];
		state.has_data = true;
		PowerPlant plant;
		plant.id = project.id;
		plant.city_index = static_cast<int>(index);
		plant.fuel = std::clamp(project.power_fuel, 0, kPowerFuelCount - 1);
		plant.mw = std::max(0.0, project.power_mw);
		plant.owner_group = project.group_id;
		plant.owner_nation = ws.cities[index].nation;
		plant.capex = project.total_cost;
		plant.opex_per_mwh = ws.power_params.opex_per_mwh[plant.fuel];
		// D-122: CF de sítio da usina nova (potencial local com teto e
		// penalidade). Térmica recebe o CF declarado da fonte.
		double solar_mean = 0.0;
		int solar_months = 0;
		for (int month = 0; month < kPowerMonthCount; ++month) {
			if (state.solar_month[month] > 0.0) {
				solar_mean += state.solar_month[month];
				solar_months += 1;
			}
		}
		if (solar_months > 0) {
			solar_mean /= static_cast<double>(solar_months);
		}
		plant.cf_base = power_local_cf(ws.power_params, plant.fuel,
			solar_mean, state.wind_ms);
		plant.jobs = plant.mw * 0.4;
		plant.wage = 20.0;
		plant.built_day = ws.day;
		ws.power_plants.push_back(plant);
		after_load(ws);
		return;
	}
}

void PowerParams::apply(const std::map<std::string, double>& v) {
	const auto get = [&v](const std::string& key, double& out) {
		const auto found = v.find(key);
		if (found != v.end()) {
			out = found->second;
		}
	};
	get("kwh_per_capita_day", kwh_per_capita_day);
	get("income_elasticity", income_elasticity);
	get("income_index_cap", income_index_cap);
	get("share_households", share_households);
	get("share_industry", share_industry);
	get("share_services", share_services);
	get("share_other", share_other);
	get("access_floor", access_floor);
	get("loss_floor_pct", loss_floor_pct);
	get("loss_max_pct", loss_max_pct);
	get("loss_infra_reduction", loss_infra_reduction);
	get("operator_access_pct", operator_access_pct);
	get("connected_score", connected_score);
	get("partial_score", partial_score);
	get("partial_line_share", partial_line_share);
	get("partial_line_min_mw", partial_line_min_mw);
	get("price_scale", price_scale);
	get("price_floor", price_floor_sd);
	get("price_ceiling", price_ceiling_sd);
	// D-118: âncora salário-dia -> XGS (derivada do mundo no seed; no save
	// ela vem no mesmo mapa). Ausente = 0 -> `resolve_money` usa o salário
	// inicial declarado até o seed derivar do sim.
	get("salary_day_xgs", salary_day_xgs);
	get("fuel_import_premium", fuel_import_premium);
	get("industry_floor", industry_floor);
	get("self_generation_floor", self_generation_floor);
	get("service_factor_min", service_factor_min);
	get("industry_cost_share_max", industry_cost_share_max);
	get("stress_per_unserved", stress_per_unserved);
	get("stress_duration_days", stress_duration_days);
	get("ration_threshold", ration_threshold);
	get("voll_multiple", voll_multiple);
	get("retail_network_share", retail_network_share);
	get("retail_scarcity_days", retail_scarcity_days);
	get("build_share_demand", build_share_demand);
	get("thermal_cap_multiple", thermal_cap_multiple);
	get("capex_per_mw_line", capex_per_mw_line_sd);
	get("line_build_share_demand", line_build_share_demand);
	for (int fuel = 0; fuel < kPowerFuelCount; ++fuel) {
		const std::string name = kFuelNames[fuel];
		get("cf_" + name, cf[fuel]);
		get("fuel_t_per_mwh_" + name, fuel_t_per_mwh[fuel]);
		get("capex_per_mw_" + name, capex_per_mw_sd[fuel]);
		get("opex_per_mwh_" + name, opex_per_mwh_sd[fuel]);
		// D-122: balanceamento das renováveis (defaults declarados).
		get("capex_mult_" + name, capex_mult[fuel]);
		get("cf_cap_" + name, cf_cap[fuel]);
		get("balance_penalty_" + name, balance_penalty[fuel]);
		get("capacity_credit_" + name, capacity_credit[fuel]);
		get("weather_amp_" + name, weather_amp[fuel]);
	}
	{
		double days = static_cast<double>(build_days_hydro);
		get("build_days_hydro", days);
		build_days_hydro = static_cast<int>(days);
	}
	get("hydro_season_amp", hydro_season_amp);
	get("wind_cf_ref_ms", wind_cf_ref_ms);
	get("wind_cf_ref", wind_cf_ref);
	get("wind_cf_exponent", wind_cf_exponent);
	const auto get_int = [&v](const std::string& key, int& out) {
		const auto found = v.find(key);
		if (found != v.end()) {
			out = static_cast<int>(found->second);
		}
	};
	get_int("build_ration_days", build_ration_days);
	get_int("build_cooldown_days", build_cooldown_days);
	get_int("build_days_plant", build_days_plant);
	get_int("build_days_line", build_days_line);
	// Normaliza as participações de demanda (soma 1; tudo zero = default).
	const double share_sum = share_households + share_industry
		+ share_services + share_other;
	if (share_sum <= 1e-9) {
		share_households = 0.30;
		share_industry = 0.42;
		share_services = 0.21;
		share_other = 0.07;
	} else {
		share_households /= share_sum;
		share_industry /= share_sum;
		share_services /= share_sum;
		share_other /= share_sum;
	}
	price_scale = std::max(0.0, price_scale);
	price_floor_sd = std::max(0.0, price_floor_sd);
	price_ceiling_sd = std::max(price_floor_sd, price_ceiling_sd);
	retail_network_share = std::clamp(retail_network_share, 0.0, 0.9);
	retail_scarcity_days = std::max(0.0, retail_scarcity_days);
	loss_floor_pct = std::clamp(loss_floor_pct, 0.0, 50.0);
	loss_max_pct = std::clamp(loss_max_pct, loss_floor_pct, 80.0);
	kwh_per_capita_day = std::max(0.0, kwh_per_capita_day);
	income_elasticity = std::max(0.0, income_elasticity);
	income_index_cap = std::max(0.1, income_index_cap);
	for (int fuel = 0; fuel < kPowerFuelCount; ++fuel) {
		cf[fuel] = std::clamp(cf[fuel], 0.0, 1.0);
		fuel_t_per_mwh[fuel] = std::max(0.0, fuel_t_per_mwh[fuel]);
		opex_per_mwh_sd[fuel] = std::max(0.0, opex_per_mwh_sd[fuel]);
		capex_per_mw_sd[fuel] = std::max(0.0, capex_per_mw_sd[fuel]);
		capex_mult[fuel] = std::clamp(capex_mult[fuel], 0.0, 20.0);
		cf_cap[fuel] = std::clamp(cf_cap[fuel], 0.0, 1.0);
		balance_penalty[fuel] = std::clamp(balance_penalty[fuel], 0.0, 1.0);
		capacity_credit[fuel] = std::clamp(capacity_credit[fuel], 0.0, 1.0);
		weather_amp[fuel] = std::clamp(weather_amp[fuel], 0.0, 0.9);
	}
	capex_per_mw_line_sd = std::max(0.0, capex_per_mw_line_sd);
	build_days_hydro = std::max(0, build_days_hydro);
	hydro_season_amp = std::clamp(hydro_season_amp, 0.0, 0.9);
	wind_cf_ref_ms = std::max(0.1, wind_cf_ref_ms);
	wind_cf_ref = std::clamp(wind_cf_ref, 0.0, 1.0);
	wind_cf_exponent = std::clamp(wind_cf_exponent, 0.0, 6.0);
	salary_day_xgs = std::max(0.0, salary_day_xgs);
	resolve_money();
}


void PowerParams::resolve_money() {
	// Âncora do mundo (XGS por salário-dia). Sem a âncora derivada, usa o
	// salário inicial declarado do núcleo — nunca 0 (preço não colapsa).
	const double anchor = salary_day_xgs > 0.0
		? salary_day_xgs : kLaborWageInitial;
	price_floor = std::max(0.0, price_floor_sd) * anchor;
	price_ceiling = std::max(price_floor_sd, price_ceiling_sd) * anchor;
	capex_per_mw_line = std::max(0.0, capex_per_mw_line_sd) * anchor;
	for (int fuel = 0; fuel < kPowerFuelCount; ++fuel) {
		opex_per_mwh[fuel] = std::max(0.0, opex_per_mwh_sd[fuel]) * anchor;
		capex_per_mw[fuel] = std::max(0.0, capex_per_mw_sd[fuel]) * anchor;
	}
}

// ---------------------------------------------------------------------------
// Semeadura (dado público) e load.
// ---------------------------------------------------------------------------
void power_params_to_map(const PowerParams& p,
		std::map<std::string, double>& out) {
	out.clear();
	const auto put = [&out](const std::string& key, double value) {
		out[key] = value;
	};
	put("kwh_per_capita_day", p.kwh_per_capita_day);
	put("income_elasticity", p.income_elasticity);
	put("income_index_cap", p.income_index_cap);
	put("share_households", p.share_households);
	put("share_industry", p.share_industry);
	put("share_services", p.share_services);
	put("share_other", p.share_other);
	put("access_floor", p.access_floor);
	put("loss_floor_pct", p.loss_floor_pct);
	put("loss_max_pct", p.loss_max_pct);
	put("loss_infra_reduction", p.loss_infra_reduction);
	put("operator_access_pct", p.operator_access_pct);
	put("connected_score", p.connected_score);
	put("partial_score", p.partial_score);
	put("partial_line_share", p.partial_line_share);
	put("partial_line_min_mw", p.partial_line_min_mw);
	put("price_scale", p.price_scale);
	put("price_floor", p.price_floor_sd);
	put("price_ceiling", p.price_ceiling_sd);
	put("fuel_import_premium", p.fuel_import_premium);
	put("industry_floor", p.industry_floor);
	put("self_generation_floor", p.self_generation_floor);
	put("service_factor_min", p.service_factor_min);
	put("industry_cost_share_max", p.industry_cost_share_max);
	put("stress_per_unserved", p.stress_per_unserved);
	put("stress_duration_days", p.stress_duration_days);
	put("ration_threshold", p.ration_threshold);
	put("voll_multiple", p.voll_multiple);
	put("retail_network_share", p.retail_network_share);
	put("retail_scarcity_days", p.retail_scarcity_days);
	put("build_ration_days", static_cast<double>(p.build_ration_days));
	put("build_cooldown_days", static_cast<double>(p.build_cooldown_days));
	put("build_share_demand", p.build_share_demand);
	put("thermal_cap_multiple", p.thermal_cap_multiple);
	put("build_days_plant", static_cast<double>(p.build_days_plant));
	put("build_days_line", static_cast<double>(p.build_days_line));
	put("capex_per_mw_line", p.capex_per_mw_line_sd);
	put("line_build_share_demand", p.line_build_share_demand);
	// D-118: a âncora salário-dia -> XGS vai no mapa (save v36) para o load
	// converter a regra com o MESMO fator do dia do seed, não com o salário
	// de hoje.
	put("salary_day_xgs", p.salary_day_xgs);
	for (int fuel = 0; fuel < kPowerFuelCount; ++fuel) {
		const std::string name = kFuelNames[fuel];
		put("cf_" + name, p.cf[fuel]);
		put("fuel_t_per_mwh_" + name, p.fuel_t_per_mwh[fuel]);
		put("capex_per_mw_" + name, p.capex_per_mw_sd[fuel]);
		put("opex_per_mwh_" + name, p.opex_per_mwh_sd[fuel]);
		// D-122 (save v38): balanceamento das renováveis no mapa de regra.
		put("capex_mult_" + name, p.capex_mult[fuel]);
		put("cf_cap_" + name, p.cf_cap[fuel]);
		put("balance_penalty_" + name, p.balance_penalty[fuel]);
		put("capacity_credit_" + name, p.capacity_credit[fuel]);
		put("weather_amp_" + name, p.weather_amp[fuel]);
	}
	put("build_days_hydro", static_cast<double>(p.build_days_hydro));
	put("hydro_season_amp", p.hydro_season_amp);
	put("wind_cf_ref_ms", p.wind_cf_ref_ms);
	put("wind_cf_ref", p.wind_cf_ref);
	put("wind_cf_exponent", p.wind_cf_exponent);
}

// D-118 — âncora salarial do sim: XGS por SALÁRIO-DIA. Média ponderada pela
// população simulada do salário diário semeado do trabalhador não qualificado
// (`Pop::wage_by_level[0]` — o mesmo nível da comparação real do §8.2 do doc
// da E2/E3). Derivada do estado do mundo no seed: determinística, não muda
// com o salário do dia e não é um número escolhido à mão. Sem população ou
// sem salário, usa o salário inicial declarado do núcleo (kLaborWageInitial).
double power_salary_day_xgs(const WorldState& ws) {
	double weighted = 0.0;
	double people = 0.0;
	for (const City& city : ws.cities) {
		const double pop = city.simulation_population > 0.0
			? city.simulation_population : city.population;
		const double wage = city.pop.wage_by_level[0];
		if (pop <= 0.0 || wage <= 0.0) {
			continue;
		}
		weighted += pop * wage;
		people += pop;
	}
	if (people <= 1e-9) {
		return kLaborWageInitial;
	}
	return std::clamp(weighted / people, 1e-4, 10.0);
}

void seed(WorldState& ws, const PowerParams& params,
		const std::map<std::string, PowerNationDef>& nations,
		const std::map<std::string, PowerCityDef>& cities,
		bool only_missing) {
	// D-118: a âncora salarial é derivada do mundo UMA vez (no seed) e vai ao
	// save; um save com o estado de energia manda, inclusive a regra já
	// convertida. Save anterior à âncora (sem a chave) é migrado aqui, uma vez.
	const bool has_save = only_missing && ws.power_ready;
	if (!has_save || ws.power_params.salary_day_xgs <= 0.0) {
		ws.power_params = params;
		ws.power_params.salary_day_xgs = power_salary_day_xgs(ws);
		ws.power_params.resolve_money();
	}
	// Save já tem estado de energia: as definições NÃO sobrescrevem o mundo.
	if (has_save) {
		return;
	}
	const PowerParams& money = ws.power_params;
	const std::size_t count = ws.cities.size();
	ws.power_cities.assign(count, PowerCityState());
	ws.power_plants.clear();
	ws.power_grids.clear();
	ws.interconnects.clear();
	ws.power_interconnects_ready = false;
	// Escala do universo simulado: a economia modela a população simulada
	// (aglomerações urbanas), não a população exibida do mapa. O dado de parque
	// é NACIONAL (mundo exibido); para MW e dinheiro viverem na mesma escala,
	// o parque de cada nação é multiplicado pela fração simulada/exibida da
	// nação. Sem simulação (fixture), a fração é 1 e o parque é o do dado.
	std::map<std::string, double> sim_pop;
	std::map<std::string, double> shown_pop;
	for (const City& city : ws.cities) {
		if (city.population > 0.0 && city.simulation_population > 0.0) {
			sim_pop[city.nation] += city.simulation_population;
			shown_pop[city.nation] += city.population;
		}
	}
	const auto nation_scale = [&sim_pop, &shown_pop](const std::string& nation) {
		const auto sim = sim_pop.find(nation);
		const auto shown = shown_pop.find(nation);
		if (sim == sim_pop.end() || shown == shown_pop.end()
				|| shown->second <= 0.0) {
			return 1.0;
		}
		return std::clamp(sim->second / shown->second, 0.0, 1.0);
	};
	// Ancoragem nacional (D-116): a demanda-base Ember do ano-base e a geração
	// real por fonte são rateadas entre as cidades e viram fator de capacidade
	// por nação × fonte. O rateio usa o MESMO peso da curva global (população
	// simulada × renda^e × acesso / (1 - perda)) medido no seed; a calibração
	// divide a geração real pela geração do modelo com o parque REAL (antes da
	// escala simulada, que se cancela na razão).
	struct NationAnchor {
		double park[kPowerFuelCount] = {};
		double model_gen_mwh[kPowerFuelCount] = {};
		double demand_weight = 0.0;
	};
	std::map<std::string, NationAnchor> anchors;
	std::vector<double> city_weight(count, 0.0);
	std::vector<double> city_income0(count, 0.0);
	std::vector<double> city_loss0(count, 0.0);
	const auto annual_cf = [&params](const PowerCityDef& def, int fuel) {
		double cf = std::clamp(params.cf[fuel], 0.0, 1.0);
		if (fuel == POWER_SOLAR) {
			double sum = 0.0;
			int months = 0;
			for (int month = 0; month < kPowerMonthCount; ++month) {
				if (def.solar_month[month] > 0.0) {
					sum += def.solar_month[month];
					months += 1;
				}
			}
			if (months > 0) {
				cf = sum / static_cast<double>(months);
			}
		}
		return std::clamp(cf, 0.0, 1.0);
	};
	for (std::size_t index = 0; index < count; ++index) {
		const City& city = ws.cities[index];
		const auto city_found = cities.find(city.id);
		const auto nation_found = nations.find(city.nation);
		if (city_found == cities.end() && nation_found == nations.end()) {
			continue;
		}
		PowerCityDef def;
		double access_pct = 100.0;
		double loss_pct = 10.0;
		if (nation_found != nations.end()) {
			access_pct = nation_found->second.access_pct;
			loss_pct = nation_found->second.loss_pct;
		}
		if (city_found != cities.end()) {
			def = city_found->second;
			access_pct = def.access_pct;
			loss_pct = def.loss_pct;
		}
		const double loss0 = std::clamp(loss_pct, params.loss_floor_pct,
			params.loss_max_pct);
		const double people = city.simulation_population > 0.0
			? city.simulation_population : city.population;
		const double income0 = std::clamp(
			std::max(0.0, city.external_income_index), 0.05,
			params.income_index_cap);
		const double access = std::clamp(access_pct / 100.0,
			params.access_floor, 1.0);
		const double loss = std::clamp(loss0 / 100.0, 0.0, 0.8);
		city_weight[index] = std::max(0.0, people)
			* std::pow(income0, params.income_elasticity) * access
			/ std::max(1e-9, 1.0 - loss);
		city_income0[index] = income0;
		city_loss0[index] = loss0;
		NationAnchor& anchor = anchors[city.nation];
		anchor.demand_weight += city_weight[index];
		for (int fuel = 0; fuel < kPowerFuelCount; ++fuel) {
			const double park = std::max(0.0, def.park[fuel]);
			anchor.park[fuel] += park;
			anchor.model_gen_mwh[fuel] += park * annual_cf(def, fuel)
				* kPowerHoursPerYear;
		}
	}
	for (std::size_t index = 0; index < count; ++index) {
		City& city = ws.cities[index];
		PowerCityState& state = ws.power_cities[index];
		const auto city_found = cities.find(city.id);
		const auto nation_found = nations.find(city.nation);
		if (city_found == cities.end() && nation_found == nations.end()) {
			// Sem dado de energia para a cidade/nação: ausência declarada.
			state.has_data = false;
			continue;
		}
		PowerCityDef def;
		double nation_access = 100.0;
		double nation_urban = 100.0;
		double nation_rural = 100.0;
		double nation_loss = 10.0;
		bool nation_data = false;
		if (nation_found != nations.end()) {
			const PowerNationDef& nation = nation_found->second;
			nation_access = nation.access_pct;
			nation_urban = nation.access_urban_pct;
			nation_rural = nation.access_rural_pct;
			nation_loss = nation.loss_pct;
			nation_data = nation.has_data;
		}
		if (city_found != cities.end()) {
			def = city_found->second;
		}
		// Parque e teto do dado nacional na escala da economia simulada.
		const double scale = nation_scale(city.nation);
		for (int fuel = 0; fuel < kPowerFuelCount; ++fuel) {
			def.park[fuel] = std::max(0.0, def.park[fuel]) * scale;
			def.cap[fuel] = std::max(0.0, def.cap[fuel]) * scale;
		}
		state.has_data = true;
		state.access_pct = std::clamp(
			city_found != cities.end() ? def.access_pct : nation_access,
			0.0, 100.0);
		state.loss_pct = std::clamp(
			city_found != cities.end() ? def.loss_pct : nation_loss,
			params.loss_floor_pct, params.loss_max_pct);
		// Âncora nacional (D-116): demanda-base × fração simulada, rateada pelo
		// peso de demanda da cidade no seed, e calibração do CF por fonte.
		const auto anchor_found = anchors.find(city.nation);
		for (int fuel = 0; fuel < kPowerFuelCount; ++fuel) {
			state.cf_scale[fuel] = 1.0;
			if (nation_found == nations.end() || anchor_found == anchors.end()) {
				continue;
			}
			const double real = nation_found->second.generation_mwh_year[fuel];
			const double model = anchor_found->second.model_gen_mwh[fuel];
			if (real >= 0.0 && model > 0.0) {
				// Dado, não chute: a fonte reproduz a geração Ember do ano-base.
				state.cf_scale[fuel] = std::clamp(real / model, 0.0, 5.0);
			}
		}
		if (nation_found != nations.end() && nation_found->second.has_demand
				&& nation_found->second.demand_base_mwh_day > 0.0
				&& anchor_found != anchors.end()
				&& anchor_found->second.demand_weight > 0.0) {
			state.demand_base_mwh = nation_found->second.demand_base_mwh_day
				* scale
				* (city_weight[index] / anchor_found->second.demand_weight);
			state.demand_income0 = city_income0[index];
			state.demand_loss0 = city_loss0[index];
		}
		double park_total = 0.0;
		for (int fuel = 0; fuel < kPowerFuelCount; ++fuel) {
			const double park = std::max(0.0, def.park[fuel]);
			park_total += park;
			state.cap_mw[fuel] = std::max(0.0, def.cap[fuel]);
			if (power_fuel_is_thermal(fuel)) {
				// Teto de expansão térmica: modelo declarado (não há potencial
				// geográfico para carvão/gás/óleo/nuclear).
				state.cap_mw[fuel] = std::max(state.cap_mw[fuel],
					park * params.thermal_cap_multiple);
			} else {
				// Renovável: o teto é o potencial declarado; sem potencial, o
				// próprio parque (não se inventa local novo).
				state.cap_mw[fuel] = std::max(state.cap_mw[fuel], park);
			}
			for (int month = 0; month < kPowerMonthCount; ++month) {
				state.solar_month[month] = def.solar_month[month];
			}
		}
		// D-122: vento do sítio (GWA) para a obra NOVA render pelo potencial.
		state.wind_ms = std::max(0.0, def.wind_ms);
		const bool operator_nation = nation_data
			&& nation_access >= params.operator_access_pct;
		const double score = 0.65 * nation_urban + 0.35 * nation_rural;
		if (operator_nation && score >= params.connected_score) {
			state.connection = POWER_CONNECTED;
			state.line_mw = 0.0;
		} else if (operator_nation && score >= params.partial_score) {
			state.connection = POWER_PARTIAL;
			state.line_mw = std::max(params.partial_line_min_mw,
				park_total * params.partial_line_share);
		} else {
			state.connection = POWER_ISOLATED;
			state.line_mw = 0.0;
		}
		for (int fuel = 0; fuel < kPowerFuelCount; ++fuel) {
			const double mw = std::max(0.0, def.park[fuel]);
			if (mw <= 0.0) {
				continue;
			}
			PowerPlant plant;
			plant.id = "PWR-" + city.id + "-" + kFuelNames[fuel];
			plant.city_index = static_cast<int>(index);
			plant.fuel = fuel;
			plant.mw = mw;
			plant.owner_nation = city.nation;
			plant.capex = mw * money.capex_per_mw[fuel];
			plant.opex_per_mwh = money.opex_per_mwh[fuel];
			plant.jobs = mw * 0.4;
			plant.wage = 20.0;
			plant.built_day = ws.day;
			plant.initial = true;
			ws.power_plants.push_back(plant);
		}
		PowerGridState& grid = ws.power_grids[city.nation];
		grid.nation = city.nation;
	}
	// Contagem de membros e existência da rede nacional.
	for (auto& row : ws.power_grids) {
		PowerGridState& grid = row.second;
		grid.exists = false;
		grid.connected = 0;
		grid.partial = 0;
		grid.isolated = 0;
		grid.ration_days = 0;
		grid.last_build_day = -1000000;
	}
	for (std::size_t index = 0; index < count; ++index) {
		const PowerCityState& state = ws.power_cities[index];
		PowerGridState& grid = ws.power_grids[ws.cities[index].nation];
		grid.nation = ws.cities[index].nation;
		if (state.connection == POWER_CONNECTED) {
			grid.connected += 1;
			grid.exists = true;
		} else if (state.connection == POWER_PARTIAL) {
			grid.partial += 1;
			grid.exists = true;
		} else {
			grid.isolated += 1;
		}
	}
	ws.power_ready = !ws.power_cities.empty();
	after_load(ws);
}

// SimCore — API de declaração de parâmetros e obra de usina (a API de
// rede/obra de linha continua em grid.cpp).
void SimCore::set_power_definitions(const PowerParams& params,
		const std::map<std::string, PowerNationDef>& nations,
		const std::map<std::string, PowerCityDef>& cities,
		bool only_missing) {
	seed(ws_, params, nations, cities, only_missing);
}

void SimCore::set_power_params(const std::map<std::string, double>& values) {
	ws_.power_params.apply(values);
}

bool SimCore::build_power_plant(const std::string& city_id,
		const std::string& fuel_name, double mw, const std::string& group_id,
		std::string* err, bool instant) {
	const auto fail = [&err](const char* message) {
		if (err != nullptr) {
			*err = message;
		}
		return false;
	};
	City* city = find_city(city_id);
	if (city == nullptr) {
		return fail("cidade desconhecida");
	}
	const int fuel = power_fuel_from_name(fuel_name);
	if (fuel < 0) {
		return fail("fonte desconhecida");
	}
	if (!(mw > 0.0)) {
		return fail("capacidade inválida");
	}
	if (ws_.power_cities.size() != ws_.cities.size()) {
		ws_.power_cities.resize(ws_.cities.size());
	}
	const std::size_t index = static_cast<std::size_t>(city - ws_.cities.data());
	PowerCityState& state = ws_.power_cities[index];
	const double cap = state.cap_mw[fuel];
	double used = 0.0;
	for (const PowerPlant& plant : ws_.power_plants) {
		if (plant.city_index == static_cast<int>(index) && plant.fuel == fuel) {
			used += plant.mw;
		}
	}
	for (const ConstructionProject& project : ws_.construction_projects) {
		if (project.kind == CONSTRUCTION_POWER_PLANT
				&& project.city == city->id && project.power_fuel == fuel) {
			used += project.power_mw;
		}
	}
	if (cap > 0.0 && used + mw > cap + 1e-9) {
		return fail("potencial esgotado");
	}
	const PowerParams& params = ws_.power_params;
	// D-122: a obra NOVA paga o capex com o multiplicador declarado da fonte;
	// o parque do ano-base mantém o capex semeado (só a jogabilidade muda).
	const double cost = mw * power_new_capex_per_mw(params, fuel);
	double* cash = nullptr;
	std::string owner = "state";
	if (group_id.empty()) {
		cash = &city->government_budget;
	} else {
		Group* group = find_group(group_id);
		if (group == nullptr) {
			return fail("grupo desconhecido");
		}
		cash = &group->cash;
		// F9: a obra do grupo viaja com o dono no formato canônico — o refund
		// e a conclusão usam a mesma string.
		owner = group->origin_nation == city->nation
			? "national:" + group->id
			: "foreign:" + group->id + "@" + group->origin_nation;
	}
	if (*cash < cost) {
		return fail("orçamento insuficiente");
	}
	ConstructionProject project;
	project.id = "PWR-" + city->id + "-" + fuel_name + "-"
		+ std::to_string(ws_.day) + "-" + std::to_string(ws_.power_plants.size());
	project.building_id = project.id;
	project.city = city->id;
	project.owner = owner;
	project.group_id = group_id;
	project.kind = CONSTRUCTION_POWER_PLANT;
	project.power_fuel = fuel;
	project.power_mw = mw;
	project.output_product = -1;
	project.total_cost = cost;
	project.duration_days = power_build_days(params, fuel);
	*cash -= cost;
	if (instant) {
		// Hook de teste/depuração (D-122): a usina entra em operação na hora.
		apply_power_plant_project(ws_, project);
		return true;
	}
	add_construction_project(project);
	return true;
}

} // namespace ogs
