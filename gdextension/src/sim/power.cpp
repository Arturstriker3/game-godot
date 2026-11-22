// PARTE E / D-116 — energia elétrica: usinas, geração, redes, despacho por
// mérito, preço e consequências da falta. Ver sim/power.h e
// docs/economy/E2E3_power_grid.md.
//
// Determinismo: toda ordem é por índice/posição (nunca por iteração de hash);
// empates de custo são resolvidos por (custo, cidade, fonte, id).

#include "power.h"

#include "sim_core.h"
#include "world_state.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace ogs {

namespace {

const char* const kFuelNames[kPowerFuelCount] = {
	"hydro", "solar", "wind", "coal", "gas", "oil", "nuclear", "biomass",
	"other"};

// Prédio industrial (receita ou insumo declarado) — é o que sente o
// racionamento na produção e no emprego.
bool is_industrial(const Building& building) {
	return building.recipe >= 0 || building.input_product >= 0;
}

int month_of_day(int day) {
	const int doy = ((day % 365) + 365) % 365;
	return std::min(11, (doy * 12) / 365);
}

// Horas do ano para o fator de capacidade anual (calibração nacional).
constexpr double kPowerHoursPerYear = 8760.0;

// Produto de combustível de cada fonte térmica (-1 = sem compra de mercado).
int fuel_product_of(const WorldState& ws, int fuel) {
	const char* good = nullptr;
	switch (fuel) {
	case POWER_COAL: good = "coal"; break;
	case POWER_GAS: good = "gas"; break;
	case POWER_OIL: good = "oil"; break;
	case POWER_NUCLEAR: good = "uranium"; break;
	case POWER_BIOMASS: good = "wood"; break;
	default: good = nullptr; break;
	}
	if (good == nullptr) {
		return -1;
	}
	const auto found = ws.product_lookup.find(good);
	return found == ws.product_lookup.end() ? -1 : found->second;
}

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
	}
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
	}
	capex_per_mw_line_sd = std::max(0.0, capex_per_mw_line_sd);
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
	}
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

void after_load(WorldState& ws) {
	// Sem estado de energia semeado (`power_cities` vazio) nada é criado aqui:
	// a ausência é declarada e o PowerService decide o que aplicar. O vetor
	// só é realinhado quando já existe estado (save v35/semeado).
	if (!ws.power_cities.empty()
			&& ws.power_cities.size() != ws.cities.size()) {
		ws.power_cities.resize(ws.cities.size());
	}
	for (PowerPlant& plant : ws.power_plants) {
		if (plant.city_index < 0
				|| plant.city_index >= static_cast<int>(ws.cities.size())) {
			plant.city_index = -1;
		}
	}
	for (auto& row : ws.power_grids) {
		PowerGridState& grid = row.second;
		grid.connected = 0;
		grid.partial = 0;
		grid.isolated = 0;
		grid.exists = false;
	}
	for (std::size_t index = 0; index < ws.power_cities.size()
			&& index < ws.cities.size(); ++index) {
		PowerGridState& grid = ws.power_grids[ws.cities[index].nation];
		grid.nation = ws.cities[index].nation;
		const PowerCityState& state = ws.power_cities[index];
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
	ws.power_ready = ws.power_ready && !ws.power_cities.empty();
}

// E5 (D-117) — semeia as interconexões do ano-base. A capacidade do dado é
// escalada pela fração simulada da nação IMPORTADORA (o fluxo do dado é a
// importação dela, como a demanda Ember), do mesmo jeito que o parque.
void seed_interconnects(WorldState& ws,
		const std::vector<InterconnectDef>& defs, bool only_missing) {
	if (only_missing && ws.power_interconnects_ready) {
		return;
	}
	if (defs.empty()) {
		ws.interconnects.clear();
		ws.power_interconnects_ready = true;
		return;
	}
	if (ws.power_grids.empty()) {
		// Sem redes declaradas ainda: não marca pronto (o chamador reaplica
		// depois de `set_power_definitions`).
		return;
	}
	std::map<std::string, double> sim_pop;
	std::map<std::string, double> shown_pop;
	for (const City& city : ws.cities) {
		if (city.population > 0.0 && city.simulation_population > 0.0) {
			sim_pop[city.nation] += city.simulation_population;
			shown_pop[city.nation] += city.population;
		}
	}
	const auto nation_scale = [&sim_pop, &shown_pop](
			const std::string& nation) {
		const auto sim = sim_pop.find(nation);
		const auto shown = shown_pop.find(nation);
		if (sim == sim_pop.end() || shown == shown_pop.end()
				|| shown->second <= 0.0) {
			return 1.0;
		}
		return std::clamp(sim->second / shown->second, 0.0, 1.0);
	};
	ws.interconnects.clear();
	for (const InterconnectDef& def : defs) {
		if (def.a.empty() || def.b.empty() || def.a == def.b
				|| !(def.capacity_mw > 0.0)) {
			continue;
		}
		// Só redes NACIONAIS comerciam (D-116): operador + conectada nos dois
		// lados. Rede parcial/isolada fica de fora da E5.
		const auto grid_a = ws.power_grids.find(def.a);
		const auto grid_b = ws.power_grids.find(def.b);
		if (grid_a == ws.power_grids.end() || grid_b == ws.power_grids.end()
				|| !grid_a->second.exists || !grid_b->second.exists
				|| grid_a->second.connected <= 0
				|| grid_b->second.connected <= 0) {
			continue;
		}
		const std::string& deficit_nation = def.importer.empty()
			? def.a : def.importer;
		InterconnectState line;
		line.a = def.a;
		line.b = def.b;
		line.capacity_mw = def.capacity_mw * nation_scale(deficit_nation);
		line.built_day = ws.day;
		line.initial = true;
		// E7 (D-120): contrato de suprimento do ano-base. A direção é a do
		// dado (`importer`); a quantidade média (MW) é escalada pela fração
		// simulada do importador — a mesma escala da capacidade e do parque —
		// e nunca passa da capacidade da linha. Sem `importer` (par só de
		// capacidade), o contrato fica 0: a linha segue só no mercado de sobra.
		if (!def.importer.empty()
				&& (def.importer == def.a || def.importer == def.b)) {
			line.contract_importer = def.importer;
			line.contract_exporter = def.importer == def.a ? def.b : def.a;
			line.contract_mw = std::min(line.capacity_mw,
				std::max(0.0, def.contract_mw)
				* nation_scale(line.contract_importer));
		}
		ws.interconnects.push_back(line);
	}
	std::stable_sort(ws.interconnects.begin(), ws.interconnects.end(),
		[](const InterconnectState& left, const InterconnectState& right) {
			if (left.a != right.a) {
				return left.a < right.a;
			}
			return left.b < right.b;
		});
	ws.power_interconnects_ready = true;
}

// ---------------------------------------------------------------------------
// SimCore — API de declaração/obra.
// ---------------------------------------------------------------------------
void SimCore::set_power_definitions(const PowerParams& params,
		const std::map<std::string, PowerNationDef>& nations,
		const std::map<std::string, PowerCityDef>& cities,
		bool only_missing) {
	seed(ws_, params, nations, cities, only_missing);
}

void SimCore::set_power_params(const std::map<std::string, double>& values) {
	ws_.power_params.apply(values);
}

void SimCore::set_power_city(const std::string& city_id, int connection,
		double line_mw, double access_pct) {
	City* city = find_city(city_id);
	if (city == nullptr) {
		return;
	}
	const std::size_t index = static_cast<std::size_t>(city - ws_.cities.data());
	if (ws_.power_cities.size() != ws_.cities.size()) {
		ws_.power_cities.resize(ws_.cities.size());
	}
	PowerCityState& state = ws_.power_cities[index];
	state.has_data = true;
	state.connection = std::clamp(connection, 0, 2);
	state.line_mw = std::max(0.0, line_mw);
	state.access_pct = std::clamp(access_pct, 0.0, 100.0);
	after_load(ws_);
}

bool SimCore::build_power_plant(const std::string& city_id,
		const std::string& fuel_name, double mw, const std::string& group_id,
		std::string* err) {
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
	const double cost = mw * params.capex_per_mw[fuel];
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
	project.duration_days = params.build_days_plant;
	*cash -= cost;
	add_construction_project(project);
	return true;
}

bool SimCore::build_power_line(const std::string& city_id, double mw,
		const std::string& group_id, std::string* err) {
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
	if (!(mw > 0.0)) {
		return fail("capacidade inválida");
	}
	if (ws_.power_cities.size() != ws_.cities.size()) {
		ws_.power_cities.resize(ws_.cities.size());
	}
	const std::size_t index = static_cast<std::size_t>(city - ws_.cities.data());
	PowerCityState& state = ws_.power_cities[index];
	for (const ConstructionProject& project : ws_.construction_projects) {
		if (project.kind == CONSTRUCTION_POWER_LINE
				&& project.city == city->id) {
			return fail("obra em andamento");
		}
	}
	const PowerParams& params = ws_.power_params;
	const double cost = mw * params.capex_per_mw_line;
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
		owner = group->origin_nation == city->nation
			? "national:" + group->id
			: "foreign:" + group->id + "@" + group->origin_nation;
	}
	if (*cash < cost) {
		return fail("orçamento insuficiente");
	}
	ConstructionProject project;
	project.id = "PWL-" + city->id + "-" + std::to_string(ws_.day);
	project.building_id = project.id;
	project.city = city->id;
	project.owner = owner;
	project.group_id = group_id;
	project.kind = CONSTRUCTION_POWER_LINE;
	project.power_mw = mw;
	project.output_product = -1;
	project.total_cost = cost;
	project.duration_days = params.build_days_line;
	*cash -= cost;
	add_construction_project(project);
	(void)state;
	return true;
}

void SimCore::set_power_interconnects(
		const std::vector<InterconnectDef>& defs, bool only_missing) {
	seed_interconnects(ws_, defs, only_missing);
}

// E5 (D-117) — obra de interconexão entre duas REDES NACIONAIS. Mesmo padrão
// da linha de cidade (`build_power_line`): paga `capex_per_mw_line` × MW do
// caixa do dono, entra na fila §15 e, ao concluir, soma capacidade ao par.
bool SimCore::build_interconnect(const std::string& nation_a,
		const std::string& nation_b, double mw, const std::string& group_id,
		std::string* err) {
	const auto fail = [&err](const char* message) {
		if (err != nullptr) {
			*err = message;
		}
		return false;
	};
	if (nation_a.empty() || nation_b.empty() || nation_a == nation_b) {
		return fail("nações inválidas");
	}
	if (!(mw > 0.0)) {
		return fail("capacidade inválida");
	}
	const auto grid_a = ws_.power_grids.find(nation_a);
	const auto grid_b = ws_.power_grids.find(nation_b);
	if (grid_a == ws_.power_grids.end() || !grid_a->second.exists
			|| grid_a->second.connected <= 0) {
		return fail("rede nacional ausente: nação A");
	}
	if (grid_b == ws_.power_grids.end() || !grid_b->second.exists
			|| grid_b->second.connected <= 0) {
		return fail("rede nacional ausente: nação B");
	}
	std::string a = nation_a;
	std::string b = nation_b;
	if (b < a) {
		std::swap(a, b);
	}
	for (const ConstructionProject& project : ws_.construction_projects) {
		if (project.kind == CONSTRUCTION_INTERCONNECT
				&& project.power_nation_a == a
				&& project.power_nation_b == b) {
			return fail("obra em andamento");
		}
	}
	// Cidade que assina a obra: a maior CONECTADA da nação A (determinístico:
	// ordem do vetor, empate fica com a primeira).
	City* owner_city = nullptr;
	const std::size_t city_count = std::min(ws_.power_cities.size(),
		ws_.cities.size());
	for (std::size_t index = 0; index < city_count; ++index) {
		City& city = ws_.cities[index];
		if (city.nation != a
				|| ws_.power_cities[index].connection != POWER_CONNECTED) {
			continue;
		}
		if (owner_city == nullptr
				|| city.simulation_population > owner_city->simulation_population) {
			owner_city = &city;
		}
	}
	if (owner_city == nullptr) {
		return fail("cidade conectada ausente na nação A");
	}
	const PowerParams& params = ws_.power_params;
	const double cost = mw * params.capex_per_mw_line;
	double* cash = nullptr;
	std::string owner = "state";
	if (group_id.empty()) {
		cash = &owner_city->government_budget;
	} else {
		Group* group = find_group(group_id);
		if (group == nullptr) {
			return fail("grupo desconhecido");
		}
		cash = &group->cash;
		owner = group->origin_nation == a
			? "national:" + group->id
			: "foreign:" + group->id + "@" + group->origin_nation;
	}
	if (*cash < cost) {
		return fail("orçamento insuficiente");
	}
	ConstructionProject project;
	project.id = "PWI-" + a + "-" + b + "-" + std::to_string(ws_.day)
		+ "-" + std::to_string(ws_.construction_projects.size());
	project.building_id = project.id;
	project.city = owner_city->id;
	project.owner = owner;
	project.group_id = group_id;
	project.kind = CONSTRUCTION_INTERCONNECT;
	project.power_nation_a = a;
	project.power_nation_b = b;
	project.power_mw = mw;
	project.output_product = -1;
	project.total_cost = cost;
	project.duration_days = params.build_days_line;
	*cash -= cost;
	add_construction_project(project);
	return true;
}

// ---------------------------------------------------------------------------
// Fase do dia: demanda, despacho por mérito, preço, racionamento, efeitos.
// ---------------------------------------------------------------------------
void SimCore::phase_power() {
	if (!ws_.power_ready) {
		return;
	}
	WorldState& ws = ws_;
	const PowerParams& params = ws.power_params;
	const std::size_t city_count = ws.cities.size();
	if (ws.power_cities.size() != city_count) {
		ws.power_cities.resize(city_count);
	}
	if (ws.power_cities.empty()) {
		return;
	}
	const int month = month_of_day(ws.day);
	// Sazonalidade (E2/E3): a geracao ANUAL de cada fonte e ancorada na Ember,
	// mas a solar usa o PVOUT mensal. Sem deslocar as fontes despachaveis, o
	// inverno (PVOUT baixo) vira blecaute mesmo com o ano fechando. O fator
	// abaixo mantem a media anual = 1 e cobre o residuo mensal da solar; nao
	// muda VOLL, preco de escassez, racionamento, insatisfacao nem perdas.
	// Nuclear, hidro e vento ficam de fora (baseload/sem serie mensal).
	struct SeasonShape {
		double solar_year = 0.0;
		double dispatch_year = 0.0;
		std::array<double, kPowerMonthCount> solar_month{};
		std::array<double, kPowerMonthCount> factor{};
		bool has_factor = false;
	};
	std::map<std::string, SeasonShape> season;
	for (const PowerPlant& plant : ws.power_plants) {
		if (plant.city_index < 0
				|| plant.city_index >= static_cast<int>(city_count)) {
			continue;
		}
		const std::size_t index = static_cast<std::size_t>(plant.city_index);
		const City& city = ws.cities[index];
		const PowerCityState& state = ws.power_cities[index];
		double cf = std::clamp(params.cf[plant.fuel], 0.0, 1.0);
		if (plant.fuel == POWER_SOLAR) {
			double sum = 0.0;
			int months = 0;
			for (int m = 0; m < kPowerMonthCount; ++m) {
				if (state.solar_month[m] > 0.0) {
					sum += state.solar_month[m];
					months += 1;
				}
			}
			if (months > 0) {
				cf = sum / static_cast<double>(months);
			}
		}
		const double scale = std::clamp(state.cf_scale[plant.fuel], 0.0, 5.0);
		const double annual = std::max(0.0, plant.mw)
			* std::clamp(cf * scale, 0.0, 1.0) * kPowerHoursPerYear;
		SeasonShape& shape = season[city.nation];
		if (plant.fuel == POWER_SOLAR) {
			shape.solar_year += annual;
			for (int m = 0; m < kPowerMonthCount; ++m) {
				shape.solar_month[m] += std::max(0.0, plant.mw)
					* std::clamp(state.solar_month[m], 0.0, 1.0);
			}
		} else if (plant.fuel != POWER_NUCLEAR
				&& power_fuel_is_thermal(plant.fuel)) {
			shape.dispatch_year += annual;
		}
	}
	for (auto& entry : season) {
		SeasonShape& shape = entry.second;
		double solar_sum = 0.0;
		for (int m = 0; m < kPowerMonthCount; ++m) {
			solar_sum += shape.solar_month[m];
		}
		if (shape.dispatch_year <= 0.0 || shape.solar_year <= 0.0
				|| solar_sum <= 0.0) {
			for (int m = 0; m < kPowerMonthCount; ++m) {
				shape.factor[m] = 1.0;
			}
			continue;
		}
		const double ratio = shape.solar_year / shape.dispatch_year;
		for (int m = 0; m < kPowerMonthCount; ++m) {
			const double share = shape.solar_month[m] / solar_sum;
			shape.factor[m] = std::clamp(
				1.0 + ratio * (1.0 - 12.0 * share), 0.0, 5.0);
		}
		shape.has_factor = true;
	}
	struct PlantDay {
		double avail_mw = 0.0;
		double cost = 0.0;
	};
	std::vector<PlantDay> days(ws.power_plants.size());
	for (PowerPlant& plant : ws.power_plants) {
		plant.generated_mwh = 0.0;
	}
	std::vector<std::vector<int>> plants_by_city(city_count);
	for (std::size_t p = 0; p < ws.power_plants.size(); ++p) {
		PowerPlant& plant = ws.power_plants[p];
		if (plant.city_index < 0
				|| plant.city_index >= static_cast<int>(city_count)) {
			continue;
		}
		const std::size_t index = static_cast<std::size_t>(plant.city_index);
		const City& city = ws.cities[index];
		const PowerCityState& state = ws.power_cities[index];
		PlantDay& day = days[p];
		double cf = std::clamp(params.cf[plant.fuel], 0.0, 1.0);
		if (plant.fuel == POWER_SOLAR && state.solar_month[month] > 0.0) {
			cf = std::clamp(state.solar_month[month], 0.0, 1.0);
		}
		// Calibração nacional (D-116): a geração Ember do ano-base define o
		// fator efetivo da fonte naquela nação. O teto 1,0 mantém a física
		// (CF ≤ 1); 0 = a fonte não gerou no ano-base (dado).
		const double cf_scale = std::clamp(state.cf_scale[plant.fuel], 0.0, 5.0);
		day.avail_mw = std::max(0.0, plant.mw)
			* std::clamp(cf * cf_scale, 0.0, 1.0);
		if (plant.fuel != POWER_SOLAR && plant.fuel != POWER_NUCLEAR
				&& power_fuel_is_thermal(plant.fuel)) {
			const auto found = season.find(city.nation);
			if (found != season.end() && found->second.has_factor) {
				day.avail_mw = std::min(
					day.avail_mw * found->second.factor[month],
					std::max(0.0, plant.mw));
			}
		}
		double fuel_cost = 0.0;
		if (power_fuel_is_thermal(plant.fuel)
				&& params.fuel_t_per_mwh[plant.fuel] > 0.0) {
			const int product = fuel_product_of(ws, plant.fuel);
			double price = 0.0;
			bool local = false;
			if (product >= 0 && product < ws.products) {
				price = city.markets[product].price;
				if (price <= 0.0) {
					price = base_price(product);
				}
				local = city.markets[product].stock > 1e-9;
			}
			fuel_cost = params.fuel_t_per_mwh[plant.fuel] * price
				* (local ? 1.0 : params.fuel_import_premium);
		}
		// D-118: o custo marginal é um PREÇO, e o preço da energia vive na
		// unidade salário-dia. O termo de combustível (declarado como custo no
		// mercado de BENS, em XGS) entra convertido pela MESMA âncora, então a
		// ordem de mérito e os custos relativos ficam idênticos aos da regra
		// declarada; a compra do combustível continua paga ao preço de mercado
		// do bem (ver `settle`), e a diferença fica no resultado do setor.
		const double anchor = params.salary_day_xgs > 0.0
			? params.salary_day_xgs : kLaborWageInitial;
		day.cost = anchor * fuel_cost + plant.opex_per_mwh;
		plants_by_city[index].push_back(static_cast<int>(p));
	}

	// Demanda por cidade: âncora Ember do ano-base quando a nação tem o dado
	// (só a curva de renda relativa move a base) ou a curva global declarada;
	// acesso (dado) e perdas T&D entram no rateio e na perda efetiva.
	std::vector<double> demand_mwh(city_count, 0.0);
	for (std::size_t index = 0; index < city_count; ++index) {
		City& city = ws.cities[index];
		PowerCityState& state = ws.power_cities[index];
		state.demand_mwh = 0.0;
		state.served_mwh = 0.0;
		state.price = 0.0;
		if (!state.has_data) {
			// Ausência declarada: não se simula falta sem dado de energia.
			city.power_served_ratio = 1.0;
			city.power_industry_factor = 1.0;
			city.power_service_factor = 1.0;
			city.power_price = 0.0;
			city.power_stress = 0.0;
			city.power_cost_industry = 0.0;
			city.power_cost_per_value = 0.0;
			continue;
		}
		// A economia precifica a população SIMULADA (aglomerações); a exibida é
		// o retrato do mapa. Demanda, contas e custos usam a mesma escala do
		// parque semeado (fração simulada, ver `seed`).
		const double people = city.simulation_population > 0.0
			? city.simulation_population : city.population;
		const double access = std::clamp(state.access_pct / 100.0,
			params.access_floor, 1.0);
		const double income = std::clamp(std::max(0.0, city.external_income_index),
			0.05, params.income_index_cap);
		const double per_capita_kwh = params.kwh_per_capita_day
			* std::pow(income, params.income_elasticity) * access;
		// Perda efetiva: a base T&D do dado reduzida pela infra B5 de energia
		// (piso técnico > 0 — obra nunca zera a perda).
		const double infra = std::clamp(
			static_cast<double>(city.region.infra_power) / kInfraMaxLevel,
			0.0, 1.0);
		const double loss_pct = std::clamp(
			state.loss_pct * (1.0 - params.loss_infra_reduction * infra),
			params.loss_floor_pct, params.loss_max_pct);
		const double loss = std::clamp(loss_pct / 100.0, 0.0, 0.8);
		double mwh = 0.0;
		if (state.demand_base_mwh > 0.0 && state.demand_income0 > 0.0) {
			// Âncora nacional (D-116): a base Ember do ano-base só se move pela
			// razão de renda (renda_t / renda_0)^e. A perda T&D entra RELATIVA
			// à do ano-base (a Ember mede no lado da geração, já com perdas),
			// então obra de infraestrutura continua reduzindo a necessidade.
			const double growth = income / state.demand_income0;
			const double loss0 = std::clamp(
				state.demand_loss0 / 100.0, 0.0, 0.8);
			mwh = state.demand_base_mwh
				* std::pow(std::max(0.0, growth), params.income_elasticity)
				* (1.0 - loss0) / (1.0 - loss);
		} else {
			// Sem Ember para a nação: curva global declarada (a mesma do
			// power_params.json, calibrada no §6.1 do doc da E2/E3).
			mwh = people * per_capita_kwh / 1000.0 / (1.0 - loss);
		}
		state.demand_mwh = std::max(0.0, mwh);
		demand_mwh[index] = state.demand_mwh;
	}

	// Estado de despacho por rede: a rede nacional (operador) compartilha as
	// usinas das cidades conectadas/parciais; a isolada roda só com as suas.
	struct Dispatch {
		double served = 0.0;
		double price = 0.0;
	};
	const auto dispatch_pool = [&days, &ws](const std::vector<int>& pool,
			double demand, Dispatch& out) {
		out.served = 0.0;
		out.price = 0.0;
		for (const int plant_index : pool) {
			if (out.served >= demand) {
				break;
			}
			PlantDay& day = days[static_cast<std::size_t>(plant_index)];
			const double take = std::min(day.avail_mw * 24.0,
				demand - out.served);
			if (take <= 0.0) {
				continue;
			}
			ws.power_plants[static_cast<std::size_t>(plant_index)].generated_mwh
				+= take;
			out.served += take;
			out.price = day.cost;
		}
	};
	const auto sort_pool = [&days, &ws](std::vector<int>& pool) {
		std::stable_sort(pool.begin(), pool.end(),
			[&days, &ws](int a, int b) {
				const PlantDay& left = days[static_cast<std::size_t>(a)];
				const PlantDay& right = days[static_cast<std::size_t>(b)];
				if (left.cost != right.cost) {
					return left.cost < right.cost;
				}
				const PowerPlant& pa = ws.power_plants[static_cast<std::size_t>(a)];
				const PowerPlant& pb = ws.power_plants[static_cast<std::size_t>(b)];
				if (pa.fuel != pb.fuel) {
					return pa.fuel < pb.fuel;
				}
				if (pa.city_index != pb.city_index) {
					return pa.city_index < pb.city_index;
				}
				return pa.id < pb.id;
			});
	};

	// E5 (D-117): estado da rede NACIONAL do dia, usado no comércio com as
	// vizinhas. Só redes nacionais entram (D-116: cidade isolada/rede local
	// não comercia). `pool` é o mérito já ordenado; o excedente é o que sobrou
	// de capacidade sem despacho.
	struct GridTrade {
		bool national = false;
		std::vector<int> connected;
		std::vector<int> partial;
		std::vector<int> pool;
		double connected_demand = 0.0;
		double connected_served = 0.0;
		double domestic_price = 0.0;
		double marginal_price = 0.0;
		// E7 (D-120): meta do contrato de suprimento do dia e o que o
		// exportador realmente gerou para ele (antes da própria demanda).
		double contract_target = 0.0;
		double contract_done = 0.0;
	};
	std::map<std::string, GridTrade> trade_states;

	// E7 (D-120): contratos de suprimento do ano-base por exportador. Só
	// valem entre duas REDES NACIONAIS (mesma regra da E5) e até a
	// capacidade da linha; a quantidade já vem escalada do seed.
	std::map<std::string, double> contract_targets;
	for (const InterconnectState& line : ws.interconnects) {
		if (!(line.contract_mw > 0.0)
				|| line.contract_importer.empty()
				|| line.contract_exporter.empty()) {
			continue;
		}
		const auto imp = ws.power_grids.find(line.contract_importer);
		const auto exp = ws.power_grids.find(line.contract_exporter);
		if (imp == ws.power_grids.end() || exp == ws.power_grids.end()
				|| !imp->second.exists || !exp->second.exists
				|| imp->second.connected <= 0 || exp->second.connected <= 0) {
			continue;
		}
		contract_targets[line.contract_exporter] += line.contract_mw * 24.0;
	}

	// Zera os agregados da rede do dia.
	for (auto& row : ws.power_grids) {
		PowerGridState& grid = row.second;
		grid.demand_mwh = 0.0;
		grid.served_mwh = 0.0;
		grid.price = 0.0;
		grid.retail_margin = 0.0;
		for (int fuel = 0; fuel < kPowerFuelCount; ++fuel) {
			grid.generation_by_fuel[fuel] = 0.0;
		}
	}

	// Reserva a ordem determinística das nações (mapa ordenado).
	for (auto& row : ws.power_grids) {
		PowerGridState& grid = row.second;
		const std::string& nation = row.first;
		std::vector<int> connected;
		std::vector<int> partial;
		std::vector<int> isolated;
		for (std::size_t index = 0; index < city_count; ++index) {
			if (ws.cities[index].nation != nation
					|| !ws.power_cities[index].has_data) {
				continue;
			}
			const int connection = ws.power_cities[index].connection;
			if (connection == POWER_CONNECTED && grid.connected > 0) {
				connected.push_back(static_cast<int>(index));
			} else if (connection == POWER_PARTIAL
					&& (grid.connected > 0 || grid.partial > 0)) {
				partial.push_back(static_cast<int>(index));
			} else {
				isolated.push_back(static_cast<int>(index));
			}
		}
		// Rede nacional de fato: existe operador e ao menos uma conectada.
		const bool national = grid.connected > 0;
		if (!national) {
			isolated.insert(isolated.end(), connected.begin(), connected.end());
			isolated.insert(isolated.end(), partial.begin(), partial.end());
			connected.clear();
			partial.clear();
		}
		// Isoladas: mérito local; preço local.
		for (const int index : isolated) {
			std::vector<int> pool = plants_by_city[static_cast<std::size_t>(index)];
			sort_pool(pool);
			Dispatch out;
			dispatch_pool(pool, demand_mwh[static_cast<std::size_t>(index)], out);
			PowerCityState& state = ws.power_cities[static_cast<std::size_t>(index)];
			state.served_mwh = out.served;
			state.price = std::clamp(out.price * params.price_scale,
				out.served > 0.0 ? params.price_floor : 0.0,
				params.price_ceiling);
			if (out.served < demand_mwh[static_cast<std::size_t>(index)]
					- 1e-9) {
				state.price = params.price_ceiling;
			}
		}
		if (national) {
			// Pool nacional: usinas das conectadas e das parciais (o limite da
			// linha é aplicado na entrega da cidade parcial, simplificação
			// declarada — não se modela fluxo físico entre subestações).
			std::vector<int> pool;
			for (const int index : connected) {
				const std::vector<int>& local =
					plants_by_city[static_cast<std::size_t>(index)];
				pool.insert(pool.end(), local.begin(), local.end());
			}
			for (const int index : partial) {
				const std::vector<int>& local =
					plants_by_city[static_cast<std::size_t>(index)];
				pool.insert(pool.end(), local.begin(), local.end());
			}
			sort_pool(pool);
			double grid_demand = 0.0;
			for (const int index : connected) {
				grid_demand += demand_mwh[static_cast<std::size_t>(index)];
			}
			for (const int index : partial) {
				grid_demand += demand_mwh[static_cast<std::size_t>(index)];
			}
			Dispatch out;
			// E7 (D-120): o contrato de suprimento é despachado ANTES da
			// demanda marginal do exportador; a carga do mérito é demanda +
			// contratos. Se a disponibilidade não cobre as duas, o contrato cai
			// proporcionalmente (regra declarada) e a própria demanda divide a
			// escassez na mesma proporção.
			const auto target = contract_targets.find(nation);
			const double contract_target = target == contract_targets.end()
				? 0.0 : target->second;
			dispatch_pool(pool, grid_demand + contract_target, out);
			const double contract_done = contract_target > 0.0
				? std::min(contract_target,
					out.served * contract_target
						/ std::max(1e-9, grid_demand + contract_target))
				: 0.0;
			const double own_served = std::max(0.0, out.served - contract_done);
			const double price = own_served + 1e-9 < grid_demand
				? params.price_ceiling
				: std::clamp(out.price * params.price_scale,
					out.served > 0.0 ? params.price_floor : 0.0,
					params.price_ceiling);
			grid.price = price;
			// Geração por cidade (para o limite da linha parcial). A parcela
			// exportada no contrato não abastece a cidade: rateio declarado
			// pela fração servida doméstica.
			const double own_ratio = out.served > 1e-9
				? own_served / out.served : 0.0;
			std::vector<double> generation(city_count, 0.0);
			for (std::size_t p = 0; p < ws.power_plants.size(); ++p) {
				const PowerPlant& plant = ws.power_plants[p];
				if (plant.city_index >= 0
						&& plant.city_index < static_cast<int>(city_count)) {
					generation[static_cast<std::size_t>(plant.city_index)]
						+= plant.generated_mwh * own_ratio;
				}
			}
			// Parciais: servido = geração local + importação até a linha.
			double partial_served_total = 0.0;
			for (const int index : partial) {
				PowerCityState& state =
					ws.power_cities[static_cast<std::size_t>(index)];
				const double demand = demand_mwh[static_cast<std::size_t>(index)];
				const double local_gen = generation[static_cast<std::size_t>(index)];
				const double line_mwh = state.line_mw * 24.0;
				const double served = std::min(demand,
					local_gen + line_mwh);
				state.served_mwh = std::max(0.0, served);
				state.price = price;
				partial_served_total += state.served_mwh;
			}
			// Conectadas: rateio do restante proporcional à demanda.
			double connected_demand = 0.0;
			for (const int index : connected) {
				connected_demand += demand_mwh[static_cast<std::size_t>(index)];
			}
			const double remaining = std::max(0.0,
				own_served - partial_served_total);
			for (const int index : connected) {
				PowerCityState& state =
					ws.power_cities[static_cast<std::size_t>(index)];
				const double demand = demand_mwh[static_cast<std::size_t>(index)];
				const double share = connected_demand > 0.0
					? demand / connected_demand : 0.0;
				state.served_mwh = std::min(demand, remaining * share);
				state.price = price;
			}
			// Agregados da rede.
			for (const int index : connected) {
				const PowerCityState& state =
					ws.power_cities[static_cast<std::size_t>(index)];
				grid.demand_mwh += state.demand_mwh;
				grid.served_mwh += state.served_mwh;
			}
			for (const int index : partial) {
				const PowerCityState& state =
					ws.power_cities[static_cast<std::size_t>(index)];
				grid.demand_mwh += state.demand_mwh;
				grid.served_mwh += state.served_mwh;
			}
			for (std::size_t p = 0; p < ws.power_plants.size(); ++p) {
				const PowerPlant& plant = ws.power_plants[p];
				if (plant.city_index < 0
						|| plant.city_index >= static_cast<int>(city_count)) {
					continue;
				}
				if (ws.cities[static_cast<std::size_t>(plant.city_index)].nation
						!= nation) {
					continue;
				}
				grid.generation_by_fuel[plant.fuel] += plant.generated_mwh;
			}
			// E5: estado do dia para o comércio entre redes nacionais.
			GridTrade& trade = trade_states[nation];
			trade.national = true;
			trade.connected = connected;
			trade.partial = partial;
			trade.pool = pool;
			trade.connected_demand = connected_demand;
			trade.connected_served = 0.0;
			for (const int index : connected) {
				trade.connected_served +=
					ws.power_cities[static_cast<std::size_t>(index)].served_mwh;
			}
			trade.domestic_price = price;
			trade.marginal_price = out.served > 0.0
				? std::clamp(out.price * params.price_scale,
					params.price_floor, params.price_ceiling)
				: params.price_floor;
			trade.contract_target = contract_target;
			trade.contract_done = contract_done;
		} else {
			for (const int index : isolated) {
				const PowerCityState& state =
					ws.power_cities[static_cast<std::size_t>(index)];
				grid.demand_mwh += state.demand_mwh;
				grid.served_mwh += state.served_mwh;
			}
			for (std::size_t p = 0; p < ws.power_plants.size(); ++p) {
				const PowerPlant& plant = ws.power_plants[p];
				if (plant.city_index < 0
						|| plant.city_index >= static_cast<int>(city_count)) {
					continue;
				}
				if (ws.cities[static_cast<std::size_t>(plant.city_index)].nation
						!= nation) {
					continue;
				}
				grid.generation_by_fuel[plant.fuel] += plant.generated_mwh;
			}
		}
		// (Os indicadores da rede saem depois do comércio, em um laço próprio,
		// porque a fração servida muda quando entra energia importada.)
	}

	// ------------------------------------------------------------------
	// E5 (D-117) — comércio diário entre redes NACIONAIS vizinhas.
	//
	// Regra: depois do balanço de cada rede, a nação com excedente vende para
	// a vizinha em déficit até a capacidade da linha. O preço de fronteira é o
	// custo marginal do exportador (última usina despachada para a exportação)
	// dividido por (1 - perda): a perda é o piso técnico de transmissão que já
	// existe em `loss_floor_pct` (não se inventa coeficiente novo). O importador
	// paga esse preço; a conta entra no caixa do setor (`power_cash`), o canal
	// que já existia — soma zero mantida, nada é criado nem perdido. Cidade
	// isolada / rede local NÃO comercia (D-116).
	//
	// TODO(relações): guerra/sanção/hostilidade NÃO corta a linha. O mundo não
	// modela diplomacia (`tpl.dossie.relations.note`: "a diplomacia não é
	// simulada"); quando houver a API de relações, o gancho entra aqui.
	const double line_loss = std::clamp(
		params.loss_floor_pct / 100.0, 0.0, 0.5);

	// ------------------------------------------------------------------
	// E7 (D-120) — CONTRATO DE SUPRIMENTO do ano-base (fluxo BRUTO
	// bilateral). O exportador já despachou o contrato junto da própria
	// demanda (bloco nacional acima); aqui a energia é entregue ao importador
	// e o preço do contrato é o CUSTO MÉDIO de atacado do exportador (não o
	// spot marginal do dia), dividido por (1 - perda da linha). O contrato cai
	// proporcionalmente quando o exportador está curto (regra declarada no
	// despacho); o importador recebe só até o próprio déficit.
	//
	// O preço de escassez do atacado, o VOLL, o racionamento e as perdas NÃO
	// mudam: o contrato é uma camada de entrega, não um novo preço de falta.
	for (InterconnectState& line : ws.interconnects) {
		line.contract_flow_mwh = 0.0;
		line.contract_sent_mwh = 0.0;
		line.contract_price = 0.0;
		line.contract_value = 0.0;
		if (!(line.contract_mw > 0.0)
				|| line.contract_importer.empty()
				|| line.contract_exporter.empty()) {
			continue;
		}
		auto imp_it = trade_states.find(line.contract_importer);
		auto exp_it = trade_states.find(line.contract_exporter);
		if (imp_it == trade_states.end() || exp_it == trade_states.end()
				|| !imp_it->second.national || !exp_it->second.national
				|| exp_it->second.contract_target <= 1e-9) {
			continue;
		}
		GridTrade& importer = imp_it->second;
		GridTrade& exporter = exp_it->second;
		const double target_share = line.contract_mw * 24.0
			/ exporter.contract_target;
		double sent = exporter.contract_done * target_share;
		if (sent <= 1e-9) {
			continue;
		}
		double delivered = sent * (1.0 - line_loss);
		double unmet_total = 0.0;
		for (const int index : importer.connected) {
			const PowerCityState& state = ws.power_cities[index];
			unmet_total += std::max(0.0,
				state.demand_mwh - state.served_mwh);
		}
		if (delivered > unmet_total) {
			delivered = unmet_total;
			sent = line_loss < 1.0 ? delivered / (1.0 - line_loss) : 0.0;
		}
		if (sent <= 1e-9 || delivered <= 1e-9) {
			continue;
		}
		const double served_before = importer.connected_served;
		double remaining = delivered;
		for (std::size_t slot = 0; slot < importer.connected.size();
				++slot) {
			const int index = importer.connected[slot];
			PowerCityState& state = ws.power_cities[index];
			const double unmet = std::max(0.0,
				state.demand_mwh - state.served_mwh);
			double give = unmet_total > 0.0
				? delivered * unmet / unmet_total : 0.0;
			if (slot + 1 == importer.connected.size()) {
				give = remaining;
			}
			const double cap = std::max(0.0, std::min(unmet, remaining));
			give = std::clamp(give, 0.0, cap);
			state.served_mwh += give;
			remaining -= give;
		}
		importer.connected_served = 0.0;
		for (const int index : importer.connected) {
			importer.connected_served +=
				ws.power_cities[index].served_mwh;
		}
		const double received = importer.connected_served - served_before;
		if (received <= 1e-9) {
			continue;
		}
		const double received_ratio = delivered > 1e-9
			? received / delivered : 0.0;
		const double sent_actual = sent * received_ratio;
		// Custo médio do atacado do exportador no dia (pondera geração e
		// custo de cada usina; inclui o próprio contrato).
		double gen = 0.0;
		double cost_value = 0.0;
		for (std::size_t p = 0; p < ws.power_plants.size(); ++p) {
			const PowerPlant& plant = ws.power_plants[p];
			if (plant.city_index < 0
					|| plant.city_index >= static_cast<int>(city_count)
					|| plant.generated_mwh <= 0.0) {
				continue;
			}
			if (ws.cities[static_cast<std::size_t>(plant.city_index)].nation
					!= line.contract_exporter) {
				continue;
			}
			gen += plant.generated_mwh;
			cost_value += plant.generated_mwh
				* days[p].cost;
		}
		const double wholesale = gen > 1e-9
			? cost_value / gen : params.price_floor;
		const double border_price = std::clamp(
			wholesale * params.price_scale / (1.0 - line_loss),
			params.price_floor, params.price_ceiling);
		const double value = received * border_price;
		line.contract_flow_mwh = received;
		line.contract_sent_mwh = sent_actual;
		line.contract_price = border_price;
		line.contract_value = value;
		PowerGridState& imp_grid = ws.power_grids[line.contract_importer];
		PowerGridState& exp_grid = ws.power_grids[line.contract_exporter];
		imp_grid.import_mwh += received;
		imp_grid.import_value += value;
		exp_grid.export_mwh += sent_actual;
		exp_grid.export_value += value;
	}

	// ------------------------------------------------------------------
	// E5 (D-117) — comércio diário entre redes NACIONAIS vizinhas.
	//
	// Regra: depois do balanço de cada rede, a nação com excedente vende para
	// a vizinha em déficit até a capacidade da linha. O preço de fronteira é o
	// custo marginal do exportador (última usina despachada para a exportação)
	// dividido por (1 - perda): a perda é o piso técnico de transmissão que já
	// existe em `loss_floor_pct` (não se inventa coeficiente novo). O importador
	// paga esse preço; a conta entra no caixa do setor (`power_cash`), o canal
	// que já existia — soma zero mantida, nada é criado nem perdido. Cidade
	// isolada / rede local NÃO comercia (D-116).
	//
	// TODO(relações): guerra/sanção/hostilidade NÃO corta a linha. O mundo não
	// modela diplomacia (`tpl.dossie.relations.note`: "a diplomacia não é
	// simulada"); quando houver a API de relações, o gancho entra aqui.
	for (InterconnectState& line : ws.interconnects) {
		line.importer_today.clear();
		line.exporter_today.clear();
		line.flow_mwh = 0.0;
		line.sent_mwh = 0.0;
		line.price = 0.0;
		line.value = 0.0;
		auto left = trade_states.find(line.a);
		auto right = trade_states.find(line.b);
		if (left == trade_states.end() || right == trade_states.end()
				|| !left->second.national || !right->second.national) {
			continue;
		}
		const auto deficit_of = [](const GridTrade& grid) {
			return std::max(0.0,
				grid.connected_demand - grid.connected_served);
		};
		GridTrade* importer = nullptr;
		GridTrade* exporter = nullptr;
		const std::string* importer_tag = nullptr;
		const std::string* exporter_tag = nullptr;
		if (deficit_of(left->second) > 1e-6
				&& deficit_of(right->second) <= 1e-6) {
			importer = &left->second;
			importer_tag = &line.a;
			exporter = &right->second;
			exporter_tag = &line.b;
		} else if (deficit_of(right->second) > 1e-6) {
			importer = &right->second;
			importer_tag = &line.b;
			exporter = &left->second;
			exporter_tag = &line.a;
		}
		if (importer == nullptr || exporter == nullptr) {
			continue;
		}
		// Excedente do exportador: capacidade ainda não despachada no mérito.
		double spare_mwh = 0.0;
		for (const int plant_index : exporter->pool) {
			const std::size_t p = static_cast<std::size_t>(plant_index);
			spare_mwh += std::max(0.0, days[p].avail_mw * 24.0
				- ws.power_plants[p].generated_mwh);
		}
		double sent = std::min(
			std::max(0.0, line.capacity_mw) * 24.0, spare_mwh);
		if (sent <= 1e-9) {
			continue;
		}
		double delivered = sent * (1.0 - line_loss);
		const double deficit = deficit_of(*importer);
		if (delivered > deficit) {
			delivered = deficit;
			sent = line_loss < 1.0 ? delivered / (1.0 - line_loss) : 0.0;
		}
		if (sent <= 1e-9 || delivered <= 1e-9) {
			continue;
		}
		// Despacha o extra no mérito do exportador (gera de verdade: o
		// combustível e o O&M entram na liquidação do dia).
		double need = sent;
		double sent_done = 0.0;
		double marginal_cost = 0.0;
		for (const int plant_index : exporter->pool) {
			if (need <= 1e-9) {
				break;
			}
			const std::size_t p = static_cast<std::size_t>(plant_index);
			PowerPlant& plant = ws.power_plants[p];
			const double free_mwh = std::max(0.0,
				days[p].avail_mw * 24.0 - plant.generated_mwh);
			const double take = std::min(free_mwh, need);
			if (take <= 0.0) {
				continue;
			}
			plant.generated_mwh += take;
			need -= take;
			sent_done += take;
			marginal_cost = days[p].cost;
		}
		if (sent_done <= 1e-9) {
			continue;
		}
		delivered = sent_done * (1.0 - line_loss);
		// Preço de fronteira: o exportador recebe o custo marginal pela energia
		// que injeta; a perda da linha é paga pelo importador (÷ (1 - perda)).
		const double border_price = std::clamp(
			marginal_cost * params.price_scale / (1.0 - line_loss),
			params.price_floor, params.price_ceiling);
		const double served_before = importer->connected_served;
		// O importado entra nas CONECTADAS, pro rata do que falta. A cidade
		// parcial não recebe além da própria linha (D-116).
		double unmet_total = 0.0;
		for (const int index : importer->connected) {
			const PowerCityState& state = ws.power_cities[index];
			unmet_total += std::max(0.0,
				state.demand_mwh - state.served_mwh);
		}
		double remaining = delivered;
		for (std::size_t slot = 0; slot < importer->connected.size();
				++slot) {
			const int index = importer->connected[slot];
			PowerCityState& state = ws.power_cities[index];
			const double unmet = std::max(0.0,
				state.demand_mwh - state.served_mwh);
			double give = unmet_total > 0.0
				? delivered * unmet / unmet_total : 0.0;
			if (slot + 1 == importer->connected.size()) {
				give = remaining;
			}
			const double cap = std::max(0.0, std::min(unmet, remaining));
			give = std::clamp(give, 0.0, cap);
			state.served_mwh += give;
			remaining -= give;
		}
		importer->connected_served = 0.0;
		for (const int index : importer->connected) {
			importer->connected_served +=
				ws.power_cities[index].served_mwh;
		}
		const double received = importer->connected_served - served_before;
		if (received <= 1e-9) {
			continue;
		}
		const double value = received * border_price;
		// Preço do importador: se o importado fechou o déficit, a escassez
		// acaba e o preço doméstico volta ao marginal; o importado entra pelo
		// preço de fronteira. Se ainda falta, o preço de escassez continua.
		const double still_short = deficit_of(*importer);
		const double base_price = still_short > 1e-6
			? importer->domestic_price : importer->marginal_price;
		const double total_served = served_before + received;
		double weighted = base_price;
		if (total_served > 1e-9) {
			weighted = (served_before * base_price + value) / total_served;
		}
		weighted = std::clamp(weighted, params.price_floor,
			params.price_ceiling);
		for (const int index : importer->connected) {
			ws.power_cities[index].price = weighted;
		}
		for (const int index : importer->partial) {
			ws.power_cities[index].price = importer->domestic_price;
		}
		PowerGridState& imp_grid = ws.power_grids[*importer_tag];
		PowerGridState& exp_grid = ws.power_grids[*exporter_tag];
		imp_grid.price = weighted;
		imp_grid.import_mwh += received;
		imp_grid.import_value += value;
		exp_grid.export_mwh += sent_done;
		exp_grid.export_value += value;
		line.importer_today = *importer_tag;
		line.exporter_today = *exporter_tag;
		line.flow_mwh = received;
		line.sent_mwh = sent_done;
		line.price = border_price;
		line.value = value;
	}

	// ------------------------------------------------------------------
	// E7 (D-120) — TARIFA DE VAREJO por rede, depois do comércio.
	//
	// Atacado médio = custo ponderado da energia entregue à rede no dia
	// (geração doméstica a custo de mérito + importado ao preço de fronteira),
	// suavizado por média móvel declarada (`retail_scarcity_days`). A tarifa
	// paga pelo consumidor = atacado médio ÷ (1 - parcela de rede/tributos) +
	// repasse de escassez suavizado. O repasse de escassez só incide sobre a
	// fração NÃO servida (déficit pequeno não multiplica a conta inteira) e é
	// suavizado; o spot de escassez continua intacto para VOLL/racionamento.
	const double retail_divisor = std::max(0.1,
		1.0 - std::clamp(params.retail_network_share, 0.0, 0.9));
	const double window = std::max(0.0, params.retail_scarcity_days);
	const auto ema = [window](double previous, double sample) {
		if (window <= 1e-9 || previous <= 0.0) {
			return sample;
		}
		return previous + (sample - previous) / window;
	};
	std::vector<bool> in_national_grid(city_count, false);
	for (const auto& row : trade_states) {
		const GridTrade& trade = row.second;
		if (!trade.national) {
			continue;
		}
		for (const int index : trade.connected) {
			in_national_grid[static_cast<std::size_t>(index)] = true;
		}
		for (const int index : trade.partial) {
			in_national_grid[static_cast<std::size_t>(index)] = true;
		}
	}
	for (auto& row : ws.power_grids) {
		PowerGridState& grid = row.second;
		const std::string& nation = row.first;
		// O importado do dia já está nas cidades; o agregado da rede ainda é o
		// de antes do comércio (o laço de indicadores abaixo o refaz). A tarifa
		// usa a fração servida FINAL, então recalcula aqui.
		grid.demand_mwh = 0.0;
		grid.served_mwh = 0.0;
		for (std::size_t index = 0; index < city_count; ++index) {
			if (ws.cities[index].nation != nation
					|| !ws.power_cities[index].has_data) {
				continue;
			}
			grid.demand_mwh += ws.power_cities[index].demand_mwh;
			grid.served_mwh += ws.power_cities[index].served_mwh;
		}
		double gen = 0.0;
		double cost_value = 0.0;
		for (std::size_t p = 0; p < ws.power_plants.size(); ++p) {
			const PowerPlant& plant = ws.power_plants[p];
			if (plant.city_index < 0
					|| plant.city_index >= static_cast<int>(city_count)
					|| plant.generated_mwh <= 0.0
					|| ws.cities[static_cast<std::size_t>(plant.city_index)]
						.nation != nation) {
				continue;
			}
			gen += plant.generated_mwh;
			cost_value += plant.generated_mwh * days[p].cost;
		}
		const double import_mwh = std::max(0.0, grid.import_mwh);
		const double import_value = std::max(0.0, grid.import_value);
		const double supply = gen + import_mwh;
		const double daily = supply > 1e-9
			? (cost_value + import_value) / supply
			: std::max(params.price_floor, grid.price);
		grid.average_cost = ema(grid.average_cost, daily);
		const double shortfall = grid.demand_mwh > 1e-9
			? std::clamp((grid.demand_mwh - grid.served_mwh)
				/ grid.demand_mwh, 0.0, 1.0) : 0.0;
		const double premium = std::max(0.0,
			grid.price - grid.average_cost) * shortfall;
		grid.scarcity_pass = ema(grid.scarcity_pass, premium);
		grid.tariff = std::max(0.0,
			grid.average_cost / retail_divisor + grid.scarcity_pass);
	}
	for (std::size_t index = 0; index < city_count; ++index) {
		PowerCityState& state = ws.power_cities[index];
		if (!state.has_data) {
			continue;
		}
		if (in_national_grid[index]) {
			const PowerGridState& grid =
				ws.power_grids[ws.cities[index].nation];
			state.average_cost = grid.average_cost;
			state.tariff = grid.tariff;
			state.scarcity_pass = grid.scarcity_pass;
			continue;
		}
		// Cidade isolada (sem rede nacional): atacado médio do próprio pool.
		double gen = 0.0;
		double cost_value = 0.0;
		for (const int p : plants_by_city[index]) {
			const double generated =
				ws.power_plants[static_cast<std::size_t>(p)].generated_mwh;
			if (generated <= 0.0) {
				continue;
			}
			gen += generated;
			cost_value += generated * days[static_cast<std::size_t>(p)].cost;
		}
		const double daily = gen > 1e-9 ? cost_value / gen
			: std::max(params.price_floor, state.price);
		state.average_cost = ema(state.average_cost, daily);
		const double shortfall = state.demand_mwh > 1e-9
			? std::clamp((state.demand_mwh - state.served_mwh)
				/ state.demand_mwh, 0.0, 1.0) : 0.0;
		const double premium = std::max(0.0,
			state.price - state.average_cost) * shortfall;
		state.scarcity_pass = ema(state.scarcity_pass, premium);
		state.tariff = std::max(0.0,
			state.average_cost / retail_divisor + state.scarcity_pass);
	}

	// Agregados e indicadores da rede DEPOIS do comércio: a fração servida
	// inclui o importado; o racionamento e o VOLL são os mesmos do E3.
	for (auto& row : ws.power_grids) {
		PowerGridState& grid = row.second;
		grid.demand_mwh = 0.0;
		grid.served_mwh = 0.0;
		grid.interconnect_count = 0;
		for (std::size_t index = 0; index < city_count; ++index) {
			if (ws.cities[index].nation != row.first
					|| !ws.power_cities[index].has_data) {
				continue;
			}
			grid.demand_mwh += ws.power_cities[index].demand_mwh;
			grid.served_mwh += ws.power_cities[index].served_mwh;
		}
		for (const InterconnectState& line : ws.interconnects) {
			if (line.a == row.first || line.b == row.first) {
				grid.interconnect_count += 1;
			}
		}
		const double ratio = grid.demand_mwh > 0.0
			? grid.served_mwh / grid.demand_mwh : 1.0;
		if (ratio < params.ration_threshold) {
			grid.ration_days += 1;
			grid.lost_gdp += (grid.demand_mwh - grid.served_mwh)
				* std::max(grid.price, params.price_floor)
				* params.voll_multiple;
		} else {
			grid.ration_days = 0;
		}
	}

	// Consequências por cidade (produção, emprego, serviços, insatisfação).
	for (std::size_t index = 0; index < city_count; ++index) {
		City& city = ws.cities[index];
		PowerCityState& state = ws.power_cities[index];
		if (!state.has_data) {
			continue;
		}
		const double demand = state.demand_mwh;
		const double ratio = demand > 0.0
			? std::clamp(state.served_mwh / demand, 0.0, 1.0) : 1.0;
		state.served_mwh = std::min(state.served_mwh, demand);
		city.power_served_ratio = ratio;
		// E7 (D-120): a economia paga a TARIFA de varejo; o spot (`state.price`)
		// continua publicado como preço de atacado/escassez.
		city.power_price = state.tariff > 0.0 ? state.tariff : state.price;
		// Piso declarado: geração própria/fóssil na cidade segura uma fração
		// da indústria (gerador a diesel) — não é atalho, é a planta local.
		double local_fossil_mw = 0.0;
		for (const int p : plants_by_city[index]) {
			const PowerPlant& plant = ws.power_plants[static_cast<std::size_t>(p)];
			if (power_fuel_is_thermal(plant.fuel)) {
				local_fossil_mw += days[static_cast<std::size_t>(p)].avail_mw;
			}
		}
		const double demand_mw = demand / 24.0;
		double self_floor = 0.0;
		if (demand_mw > 1e-9) {
			self_floor = std::min(params.self_generation_floor,
				local_fossil_mw / demand_mw);
		}
		const double industry = std::clamp(std::max(ratio, self_floor),
			params.industry_floor, 1.0);
		city.power_industry_factor = industry;
		city.power_service_factor = std::clamp(
			params.service_factor_min + (1.0 - params.service_factor_min) * ratio,
			params.service_factor_min, 1.0);
		// Conta da indústria do dia e custo por unidade de valor (piso de
		// custo da formação de preço — Parte E, E3/7).
		const double industry_mwh = state.served_mwh * params.share_industry;
		city.power_cost_industry = industry_mwh
			* (state.tariff > 0.0 ? state.tariff : state.price);
		double value = 0.0;
		for (const int bi : ws.buildings_by_city[index]) {
			const Building& building = ws.buildings[static_cast<std::size_t>(bi)];
			if (is_industrial(building) && building.output_product >= 0
					&& building.output_product < ws.products) {
				value += std::max(0.0, building.capacity)
					* base_price(building.output_product);
			}
		}
		city.power_cost_per_value = value > 1e-9
			? std::min(city.power_cost_industry / value,
				std::max(0.0, params.industry_cost_share_max)) : 0.0;
		// Insatisfação: cresce com a duração do blecaute (dias seguidos).
		if (ratio < params.ration_threshold) {
			state.ration_days += 1;
			state.lost_gdp += (demand - state.served_mwh)
				* std::max(state.price, params.price_floor)
				* params.voll_multiple;
		} else {
			state.ration_days = 0;
		}
		const double duration = static_cast<double>(state.ration_days);
		const double duration_factor = duration
			/ (duration + std::max(1.0, params.stress_duration_days));
		city.power_stress = std::clamp((1.0 - ratio)
			* params.stress_per_unserved * duration_factor, 0.0, 1.0);
	}

	// IA: onde o racionamento persiste, o Estado abre obra de usina (fila
	// §15). Determinístico: nação em ordem de mapa, fonte mais barata com
	// potencial, cidade conectada de maior população com folga.
	for (auto& row : ws.power_grids) {
		PowerGridState& grid = row.second;
		if (!grid.exists || grid.ration_days < params.build_ration_days) {
			continue;
		}
		if (ws.day - grid.last_build_day < params.build_cooldown_days) {
			continue;
		}
		const std::string& nation = row.first;
		const double demand_mw = grid.demand_mwh / 24.0;
		const double wanted = std::max(5.0, demand_mw * params.build_share_demand);
		int best_fuel = -1;
		std::string best_city;
		double best_mw = 0.0;
		double best_cost = 0.0;
		for (int fuel = 0; fuel < kPowerFuelCount; ++fuel) {
			for (std::size_t index = 0; index < city_count; ++index) {
				const City& city = ws.cities[index];
				if (city.nation != nation) {
					continue;
				}
				const PowerCityState& state = ws.power_cities[index];
				if (state.connection == POWER_ISOLATED || !state.has_data) {
					continue;
				}
				double used = 0.0;
				for (const PowerPlant& plant : ws.power_plants) {
					if (plant.city_index == static_cast<int>(index)
							&& plant.fuel == fuel) {
						used += plant.mw;
					}
				}
				const double headroom = state.cap_mw[fuel] - used;
				if (headroom < wanted * 0.5) {
					continue;
				}
				const double mw = std::min(wanted, headroom);
				const double cost = mw * params.capex_per_mw[fuel];
				if (best_fuel < 0 || cost < best_cost) {
					best_fuel = fuel;
					best_city = city.id;
					best_mw = mw;
					best_cost = cost;
				}
			}
		}
		if (best_fuel < 0) {
			continue;
		}
		City* target = find_city(best_city);
		if (target == nullptr || target->government_budget < best_cost) {
			continue;
		}
		if (build_power_plant(best_city, power_fuel_name(best_fuel), best_mw,
				std::string(), nullptr)) {
			grid.last_build_day = ws.day;
		}
	}
}

// ---------------------------------------------------------------------------
// Liquidação financeira do dia (depois da contabilidade): consumidor paga ao
// gerador; combustível sai do mercado; O&M/salários e lucro do dono. Soma
// zero sempre — o caixa agregado do setor (`power_cash`) entra em money_total.
// ---------------------------------------------------------------------------
void SimCore::phase_power_settle() {
	if (!ws_.power_ready || ws_.power_cities.size() != ws_.cities.size()) {
		return;
	}
	WorldState& ws = ws_;
	const PowerParams& params = ws.power_params;
	double pool = ws.power_cash;
	// 1. Consumidor paga a TARIFA de varejo: famílias, firmas, serviços e
	// governo (E7/D-120). O gerador recebe o custo médio de atacado; a
	// diferença (rede + tributos + repasse de escassez) fica com a
	// distribuidora/Estado pelo caixa que já existe. Só o servido é cobrado; o
	// que o caixa não cobre NÃO vira dívida (perda do setor). Soma zero: o
	// dinheiro que sai do consumidor entra no caixa do setor ou no orçamento
	// do Estado, nunca some nem é criado.
	for (std::size_t index = 0; index < ws.cities.size(); ++index) {
		City& city = ws.cities[index];
		const PowerCityState& state = ws.power_cities[index];
		if (!state.has_data || state.served_mwh <= 0.0) {
			continue;
		}
		const double tariff = state.tariff > 0.0 ? state.tariff : state.price;
		if (tariff <= 0.0) {
			continue;
		}
		const double served = state.served_mwh;
		const double bill_h = served * params.share_households * tariff;
		const double bill_i = served * params.share_industry * tariff;
		const double bill_s = served * params.share_services * tariff;
		const double bill_g = served * params.share_other * tariff;
		const double before = city.pop.savings;
		charge_households(city.pop, bill_h);
		const double paid_h = std::max(0.0, before - city.pop.savings);
		pool += paid_h;
		const double paid_i = std::min(bill_i, std::max(0.0, city.firm_cash));
		city.firm_cash -= paid_i;
		pool += paid_i;
		const double paid_s = std::min(bill_s, std::max(0.0, city.service_cash));
		city.service_cash -= paid_s;
		pool += paid_s;
		const double paid_g = std::min(bill_g,
			std::max(0.0, city.government_budget));
		city.government_budget -= paid_g;
		pool += paid_g;
		// Remuneração do atacado (custo médio do período). A tarifa é
		// normalmente maior; a margem vai ao Estado/distribuidora. Se a tarifa
		// ficar abaixo do atacado, o Estado cobre até o próprio orçamento; o
		// resto é perda do setor (não se cria dinheiro).
		const double wholesale = state.average_cost > 0.0
			? state.average_cost : state.price;
		const double paid_total = paid_h + paid_i + paid_s + paid_g;
		const double margin = paid_total - served * wholesale;
		double to_state = 0.0;
		if (margin > 0.0) {
			to_state = margin;
		} else if (margin < 0.0) {
			to_state = -std::min(-margin,
				std::max(0.0, city.government_budget));
		}
		if (to_state > 0.0) {
			pool -= to_state;
			city.government_budget += to_state;
		} else if (to_state < 0.0) {
			const double subsidy = -to_state;
			city.government_budget -= subsidy;
			pool += subsidy;
		}
		const auto grid_found = ws.power_grids.find(city.nation);
		if (grid_found != ws.power_grids.end()) {
			grid_found->second.retail_margin += to_state;
		}
	}
	// 2. Custo das usinas do dia: combustível (mercado) + O&M + salários.
	const std::size_t plant_count = ws.power_plants.size();
	std::vector<double> fuel_local(plant_count, 0.0);
	std::vector<double> fuel_import(plant_count, 0.0);
	std::vector<int> fuel_product(plant_count, -1);
	std::vector<double> opex(plant_count, 0.0);
	std::vector<double> wages(plant_count, 0.0);
	double total_cost = 0.0;
	const int month = month_of_day(ws.day);
	for (std::size_t p = 0; p < plant_count; ++p) {
		PowerPlant& plant = ws.power_plants[p];
		const double gen = plant.generated_mwh;
		if (gen <= 0.0 || plant.city_index < 0
				|| plant.city_index >= static_cast<int>(ws.cities.size())) {
			continue;
		}
		City& city = ws.cities[static_cast<std::size_t>(plant.city_index)];
		if (power_fuel_is_thermal(plant.fuel)
				&& params.fuel_t_per_mwh[plant.fuel] > 0.0) {
			const int product = fuel_product_of(ws, plant.fuel);
			const double tons = gen * params.fuel_t_per_mwh[plant.fuel];
			double price = 0.0;
			double local = 0.0;
			if (product >= 0 && product < ws.products) {
				price = city.markets[product].price;
				if (price <= 0.0) {
					price = base_price(product);
				}
				Market& market = city.markets[product];
				local = std::min(tons, std::max(0.0, market.stock));
				// O combustível comprado sai do estoque do mercado da usina e
				// entra na demanda efetiva do dia (o preço reage à térmica).
				market.stock -= local;
				ws.consumed_today[product] += local;
				market.effective += tons;
			}
			fuel_product[p] = product;
			fuel_local[p] = local * price;
			fuel_import[p] = (tons - local) * price
				* params.fuel_import_premium;
		}
		// Salários seguem a operação do dia (geração ÷ disponibilidade).
		double cf = std::clamp(params.cf[plant.fuel], 0.0, 1.0);
		if (plant.fuel == POWER_SOLAR) {
			const PowerCityState& state =
				ws.power_cities[static_cast<std::size_t>(plant.city_index)];
			if (state.solar_month[month] > 0.0) {
				cf = std::clamp(state.solar_month[month], 0.0, 1.0);
			}
		}
		const double avail_mw = std::max(0.0, plant.mw) * cf;
		const double operating = avail_mw > 1e-9
			? std::clamp((gen / 24.0) / avail_mw, 0.0, 1.0) : 0.0;
		opex[p] = gen * plant.opex_per_mwh;
		wages[p] = plant.jobs * plant.wage * operating;
		total_cost += fuel_local[p] + fuel_import[p] + opex[p] + wages[p];
	}
	// 3. Paga o custo até o caixa (parcial = perda declarada do setor):
	// combustível aos produtores (ou ao exterior), O&M/salários na cidade.
	const double pay_ratio = total_cost > 1e-9
		? std::clamp(pool / total_cost, 0.0, 1.0) : 0.0;
	double paid_total = 0.0;
	for (std::size_t p = 0; p < plant_count; ++p) {
		const PowerPlant& plant = ws.power_plants[p];
		if (plant.generated_mwh <= 0.0) {
			continue;
		}
		City& city = ws.cities[static_cast<std::size_t>(plant.city_index)];
		const double fuel_paid = (fuel_local[p] + fuel_import[p]) * pay_ratio;
		if (fuel_paid > 0.0 && fuel_product[p] >= 0
				&& fuel_product[p] < ws.products) {
			// O combustível LOCAL saiu do mercado da própria cidade: quem
			// vendeu foram as firmas dessa cidade (é a única origem que o
			// mercado registra). A parcela IMPORTADA sai do país.
			const double fuel_list = fuel_local[p] + fuel_import[p];
			const double local_share = fuel_list > 1e-12
				? fuel_local[p] / fuel_list : 0.0;
			city.firm_cash += fuel_paid * local_share;
			ws.foreign_cash += fuel_paid * (1.0 - local_share);
			paid_total += fuel_paid;
		}
		const double opex_paid = opex[p] * pay_ratio;
		city.firm_cash += opex_paid;
		paid_total += opex_paid;
		const double wages_paid = wages[p] * pay_ratio;
		credit_households(city.pop, wages_paid);
		paid_total += wages_paid;
	}
	pool = std::max(0.0, pool - paid_total);
	// 4. Lucro (o que sobrou) ao dono; soma zero (sai de `power_cash`).
	double generated_total = 0.0;
	for (const PowerPlant& plant : ws.power_plants) {
		generated_total += std::max(0.0, plant.generated_mwh);
	}
	const double profit_total = pool;
	std::size_t last_owner = plant_count;
	for (std::size_t p = 0; p < plant_count; ++p) {
		if (ws.power_plants[p].generated_mwh > 0.0
				&& ws.power_plants[p].city_index >= 0) {
			last_owner = p;
		}
	}
	double distributed = 0.0;
	for (std::size_t p = 0; p < plant_count; ++p) {
		const PowerPlant& plant = ws.power_plants[p];
		if (plant.generated_mwh <= 0.0 || generated_total <= 1e-9
				|| profit_total <= 0.0) {
			continue;
		}
		City& city = ws.cities[static_cast<std::size_t>(plant.city_index)];
		const double profit = p == last_owner
			? std::max(0.0, profit_total - distributed)
			: profit_total * plant.generated_mwh / generated_total;
		distributed += profit;
		if (plant.owner_group.empty()) {
			city.government_budget += profit;
		} else {
			Group* group = find_group(plant.owner_group);
			if (group != nullptr) {
				group->cash += profit;
			} else {
				city.government_budget += profit;
			}
		}
	}
	ws.power_cash = 0.0;
}

} // namespace ogs
