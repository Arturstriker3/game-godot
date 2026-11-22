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
	get("price_floor", price_floor);
	get("price_ceiling", price_ceiling);
	get("fuel_import_premium", fuel_import_premium);
	get("industry_floor", industry_floor);
	get("self_generation_floor", self_generation_floor);
	get("service_factor_min", service_factor_min);
	get("stress_per_unserved", stress_per_unserved);
	get("stress_duration_days", stress_duration_days);
	get("ration_threshold", ration_threshold);
	get("voll_multiple", voll_multiple);
	get("build_share_demand", build_share_demand);
	get("thermal_cap_multiple", thermal_cap_multiple);
	get("capex_per_mw_line", capex_per_mw_line);
	get("line_build_share_demand", line_build_share_demand);
	for (int fuel = 0; fuel < kPowerFuelCount; ++fuel) {
		const std::string name = kFuelNames[fuel];
		get("cf_" + name, cf[fuel]);
		get("fuel_t_per_mwh_" + name, fuel_t_per_mwh[fuel]);
		get("capex_per_mw_" + name, capex_per_mw[fuel]);
		get("opex_per_mwh_" + name, opex_per_mwh[fuel]);
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
	price_floor = std::max(0.0, price_floor);
	price_ceiling = std::max(price_floor, price_ceiling);
	loss_floor_pct = std::clamp(loss_floor_pct, 0.0, 50.0);
	loss_max_pct = std::clamp(loss_max_pct, loss_floor_pct, 80.0);
	kwh_per_capita_day = std::max(0.0, kwh_per_capita_day);
	income_elasticity = std::max(0.0, income_elasticity);
	income_index_cap = std::max(0.1, income_index_cap);
	for (int fuel = 0; fuel < kPowerFuelCount; ++fuel) {
		cf[fuel] = std::clamp(cf[fuel], 0.0, 1.0);
		fuel_t_per_mwh[fuel] = std::max(0.0, fuel_t_per_mwh[fuel]);
		opex_per_mwh[fuel] = std::max(0.0, opex_per_mwh[fuel]);
		capex_per_mw[fuel] = std::max(0.0, capex_per_mw[fuel]);
	}
}

// ---------------------------------------------------------------------------
// Semeadura (dado público) e load.
// ---------------------------------------------------------------------------
void seed(WorldState& ws, const PowerParams& params,
		const std::map<std::string, PowerNationDef>& nations,
		const std::map<std::string, PowerCityDef>& cities,
		bool only_missing) {
	ws.power_params = params;
	// Save já tem estado de energia: as definições NÃO sobrescrevem o mundo.
	if (only_missing && ws.power_ready) {
		return;
	}
	const std::size_t count = ws.cities.size();
	ws.power_cities.assign(count, PowerCityState());
	ws.power_plants.clear();
	ws.power_grids.clear();
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
		state.has_data = true;
		state.access_pct = std::clamp(
			city_found != cities.end() ? def.access_pct : nation_access,
			0.0, 100.0);
		state.loss_pct = std::clamp(
			city_found != cities.end() ? def.loss_pct : nation_loss,
			params.loss_floor_pct, params.loss_max_pct);
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
			plant.capex = mw * params.capex_per_mw[fuel];
			plant.opex_per_mwh = params.opex_per_mwh[fuel];
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
	if (ws.power_cities.size() != ws.cities.size()) {
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
	if (group_id.empty()) {
		cash = &city->government_budget;
	} else {
		Group* group = find_group(group_id);
		if (group == nullptr) {
			return fail("grupo desconhecido");
		}
		cash = &group->cash;
	}
	if (*cash < cost) {
		return fail("orçamento insuficiente");
	}
	ConstructionProject project;
	project.id = "PWR-" + city->id + "-" + fuel_name + "-"
		+ std::to_string(ws_.day) + "-" + std::to_string(ws_.power_plants.size());
	project.building_id = project.id;
	project.city = city->id;
	project.owner = group_id.empty() ? "state" : "group:" + group_id;
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
	if (group_id.empty()) {
		cash = &city->government_budget;
	} else {
		Group* group = find_group(group_id);
		if (group == nullptr) {
			return fail("grupo desconhecido");
		}
		cash = &group->cash;
	}
	if (*cash < cost) {
		return fail("orçamento insuficiente");
	}
	ConstructionProject project;
	project.id = "PWL-" + city->id + "-" + std::to_string(ws_.day);
	project.building_id = project.id;
	project.city = city->id;
	project.owner = group_id.empty() ? "state" : "group:" + group_id;
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
		day.avail_mw = std::max(0.0, plant.mw) * cf;
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
		day.cost = std::max(0.0, fuel_cost + plant.opex_per_mwh);
		plants_by_city[index].push_back(static_cast<int>(p));
	}

	// Demanda por cidade: renda (dado), acesso (dado) e perdas T&D.
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
		const double people = city.population > 0.0
			? city.population : city.simulation_population;
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
		const double mwh = people * per_capita_kwh / 1000.0 / (1.0 - loss);
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

	// Zera os agregados da rede do dia.
	for (auto& row : ws.power_grids) {
		PowerGridState& grid = row.second;
		grid.demand_mwh = 0.0;
		grid.served_mwh = 0.0;
		grid.price = 0.0;
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
			dispatch_pool(pool, grid_demand, out);
			const double price = out.served + 1e-9 < grid_demand
				? params.price_ceiling
				: std::clamp(out.price * params.price_scale,
					out.served > 0.0 ? params.price_floor : 0.0,
					params.price_ceiling);
			grid.price = price;
			// Geração por cidade (para o limite da linha parcial).
			std::vector<double> generation(city_count, 0.0);
			for (std::size_t p = 0; p < ws.power_plants.size(); ++p) {
				const PowerPlant& plant = ws.power_plants[p];
				if (plant.city_index >= 0
						&& plant.city_index < static_cast<int>(city_count)) {
					generation[static_cast<std::size_t>(plant.city_index)]
						+= plant.generated_mwh;
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
			const double remaining = std::max(0.0, out.served - partial_served_total);
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
		// Indicadores da rede.
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
		city.power_price = state.price;
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
		city.power_cost_industry = industry_mwh * state.price;
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
			? city.power_cost_industry / value : 0.0;
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
	// 1. Consumidor paga: famílias, firmas, serviços e governo. Só o servido
	// é cobrado; o que o caixa não cobre NÃO vira dívida (perda do setor).
	for (std::size_t index = 0; index < ws.cities.size(); ++index) {
		City& city = ws.cities[index];
		const PowerCityState& state = ws.power_cities[index];
		if (!state.has_data || state.served_mwh <= 0.0 || state.price <= 0.0) {
			continue;
		}
		const double served = state.served_mwh;
		const double bill_h = served * params.share_households * state.price;
		const double bill_i = served * params.share_industry * state.price;
		const double bill_s = served * params.share_services * state.price;
		const double bill_g = served * params.share_other * state.price;
		const double before = city.pop.savings;
		charge_households(city.pop, bill_h);
		pool += std::max(0.0, before - city.pop.savings);
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
