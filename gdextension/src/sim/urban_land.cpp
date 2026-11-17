// §19 (W1) — solo urbano e zoneamento. Implementação do módulo declarado em
// urban_land.h. C++ puro: sem godot_cpp. Determinístico: percorre cidades na
// ordem do vetor e nunca escreve acumulador global.
#include "urban_land.h"

#include "sim_constants.h"
#include "sim_core.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace ogs {
namespace urban_land {

namespace {

// Fábrica/planta URBANA: indústria com receita (secundária) ou fábrica legada
// (insumo declarado). Minas e fazendas (extração primária) ficam fora da área
// urbana e não consomem zona industrial.
inline bool is_urban_factory(const DemoBuilding& building) {
	return building.recipe >= 0 || building.input_product >= 0;
}

inline bool is_urban_factory(const DemoConstructionProject& project) {
	return !project.expand_existing
		&& (project.recipe >= 0 || project.input_product >= 0);
}

// Custo comprometido e ainda não pago de um dono numa cidade (mesma regra de
// sim/construction.cpp: o dono não inicia obra que estoure o caixa atual).
inline double committed_cost(const WorldState& ws, const std::string& city_id,
		const std::string& owner) {
	double committed = 0.0;
	for (const DemoConstructionProject& project : ws.construction_projects) {
		if (project.city != city_id) {
			continue;
		}
		const bool match = owner == "state" ? project.owner == "state"
			: owner == "private" ? project.owner == "private"
			: !project.group_id.empty() && project.group_id == owner;
		if (match) {
			committed += std::max(0.0, project.total_cost - project.paid);
		}
	}
	return committed;
}

// Índice da cidade pelo id (-1 se não existe).
inline int city_index_of(const WorldState& ws, const std::string& city_id) {
	const auto found = ws.city_lookup.find(city_id);
	if (found == ws.city_lookup.end() || found->second < 0) {
		return -1;
	}
	return found->second;
}

} // namespace

double zone_capacity(const CityLand& land, int zone) {
	if (zone < 0 || zone >= ZONE_COUNT) {
		return 0.0;
	}
	const double area_m2 = std::max(0.0, land.area_km2[zone]) * 1.0e6;
	if (area_m2 <= 0.0) {
		return 0.0;
	}
	const int density = std::clamp(
		static_cast<int>(land.max_density[zone]), 0, DENSITY_COUNT - 1);
	const double far = kDemoUrbanFar[density];
	if (far <= 0.0) {
		return 0.0;
	}
	const double per_unit_m2 = zone == ZONE_RES
		? kDemoUrbanUnitM2[density][TIER_MEDIO]
		: zone == ZONE_COM ? kDemoUrbanBusinessM2 : kDemoUrbanFactoryM2;
	if (per_unit_m2 <= 0.0) {
		return 0.0;
	}
	return area_m2 * far / per_unit_m2;
}

double zone_usage(const WorldState& ws, std::size_t city_index, int zone) {
	if (city_index >= ws.cities.size() || zone < 0 || zone >= ZONE_COUNT) {
		return 0.0;
	}
	const std::string& city_id = ws.cities[city_index].id;
	double usage = 0.0;
	if (zone == ZONE_RES) {
		if (city_index >= ws.housing.size()) {
			return 0.0;
		}
		const CityHousing& house = ws.housing[city_index];
		for (int density = 0; density < DENSITY_COUNT; ++density) {
			for (int tier = 0; tier < TIER_COUNT; ++tier) {
				usage += std::max(0.0, house.units[density][tier]);
			}
		}
		return usage;
	}
	if (zone == ZONE_COM) {
		// Índice por cidade (montado no início do dia) evita varrer 60 mil
		// negócios a cada decisão de abertura; o fallback cobre um estado em
		// que o índice ainda não foi refeito.
		if (city_index < ws.service_by_city.size()) {
			for (const int business_index : ws.service_by_city[city_index]) {
				if (business_index >= 0
						&& static_cast<std::size_t>(business_index)
							< ws.service_businesses.size()
						&& !ws.service_businesses[
							static_cast<std::size_t>(business_index)].bankrupt) {
					usage += 1.0;
				}
			}
			return usage;
		}
		for (const DemoServiceBusiness& business : ws.service_businesses) {
			if (!business.bankrupt && business.city == city_id) {
				usage += 1.0;
			}
		}
		return usage;
	}
	// IND: plantas em operação + obras de fábrica em andamento na cidade.
	if (city_index < ws.buildings_by_city.size()) {
		for (const int building_index : ws.buildings_by_city[city_index]) {
			if (building_index >= 0
					&& static_cast<std::size_t>(building_index) < ws.buildings.size()
					&& is_urban_factory(
						ws.buildings[static_cast<std::size_t>(building_index)])) {
				usage += 1.0;
			}
		}
	} else {
		for (const DemoBuilding& building : ws.buildings) {
			if (building.city == city_id && is_urban_factory(building)) {
				usage += 1.0;
			}
		}
	}
	for (const DemoConstructionProject& project : ws.construction_projects) {
		if (project.city == city_id && is_urban_factory(project)) {
			usage += 1.0;
		}
	}
	return usage;
}

double zone_occupancy(const WorldState& ws, std::size_t city_index, int zone) {
	if (city_index >= ws.land.size() || zone < 0 || zone >= ZONE_COUNT) {
		return 0.0;
	}
	const double capacity = zone_capacity(ws.land[city_index], zone);
	const double usage = zone_usage(ws, city_index, zone);
	if (capacity <= 0.0) {
		return usage > 0.0 ? 1.0e9 : 0.0;
	}
	return usage / capacity;
}

bool zone_has_room(const WorldState& ws, std::size_t city_index, int zone,
		double slots) {
	if (city_index >= ws.land.size() || zone < 0 || zone >= ZONE_COUNT) {
		// Sem solo carregado não há limite de zona (cenário sintético/save
		// antigo): ausência explícita, o comportamento anterior é preservado.
		return true;
	}
	const double capacity = zone_capacity(ws.land[city_index], zone);
	const double usage = zone_usage(ws, city_index, zone);
	return usage + std::max(0.0, slots) <= capacity + 1e-9;
}

void signal_zone_demand(CityLand& land, int zone) {
	if (zone >= 0 && zone < ZONE_COUNT) {
		land.zone_demand[zone] += 1.0;
	}
}

void phase_month(WorldState& ws) {
	if (ws.land.empty() || ws.cities.empty()) {
		return;
	}
	if (kDemoUrbanMonthDays <= 0 || ws.day % kDemoUrbanMonthDays != 0) {
		return;
	}
	const std::size_t count = std::min(
		{ws.land.size(), ws.housing.size(), ws.cities.size()});
	if (count == 0) {
		return;
	}
	// Uso por cidade calculado numa passada (a fase roda uma vez por mês): as
	// unidades de moradia, os negócios de serviço e as plantas industriais são
	// acumulados por índice de cidade, na ordem estável dos vetores.
	std::vector<double> usage(count * ZONE_COUNT, 0.0);
	for (std::size_t index = 0; index < count; ++index) {
		const CityHousing& house = ws.housing[index];
		double units = 0.0;
		for (int density = 0; density < DENSITY_COUNT; ++density) {
			for (int tier = 0; tier < TIER_COUNT; ++tier) {
				units += std::max(0.0, house.units[density][tier]);
			}
		}
		usage[index * ZONE_COUNT + ZONE_RES] = units;
	}
	for (const DemoServiceBusiness& business : ws.service_businesses) {
		if (business.bankrupt) {
			continue;
		}
		const int index = city_index_of(ws, business.city);
		if (index >= 0 && static_cast<std::size_t>(index) < count) {
			usage[static_cast<std::size_t>(index) * ZONE_COUNT + ZONE_COM] += 1.0;
		}
	}
	for (const DemoBuilding& building : ws.buildings) {
		if (!is_urban_factory(building)) {
			continue;
		}
		const int index = city_index_of(ws, building.city);
		if (index >= 0 && static_cast<std::size_t>(index) < count) {
			usage[static_cast<std::size_t>(index) * ZONE_COUNT + ZONE_IND] += 1.0;
		}
	}
	for (const DemoConstructionProject& project : ws.construction_projects) {
		if (!is_urban_factory(project)) {
			continue;
		}
		const int index = city_index_of(ws, project.city);
		if (index >= 0 && static_cast<std::size_t>(index) < count) {
			usage[static_cast<std::size_t>(index) * ZONE_COUNT + ZONE_IND] += 1.0;
		}
	}

	for (std::size_t index = 0; index < count; ++index) {
		CityLand& land = ws.land[index];
		const DemoCity& city = ws.cities[index];
		const CityHousing& house = ws.housing[index];
		// Base do preço da terra: parcela do terreno no valor do imóvel médio
		// (XGS/m²) na densidade permitida. Sem preço na fonte (0) o preço da
		// terra NÃO é inventado: fica como está (ausência explícita).
		const int density = std::clamp(
			static_cast<int>(land.max_density[ZONE_RES]), 0, DENSITY_COUNT - 1);
		const double unit_m2 = kDemoUrbanUnitM2[density][TIER_MEDIO];
		const double price_per_m2 = unit_m2 > 0.0
			? std::max(0.0, house.price[TIER_MEDIO]) * kDemoUrbanLandShareOfHousePrice
				/ unit_m2 : 0.0;
		if (price_per_m2 > 0.0) {
			// Renda média local: índice da nação relativo à média mundial
			// (1,0), linear e limitado — renda alta encarece o solo.
			const double income = std::isfinite(city.external_income_index)
				? city.external_income_index : 1.0;
			const double income_factor = 1.0 + kDemoUrbanLandIncomeGain
				* std::clamp(income - 1.0, -0.5, 1.0);
			for (int zone = 0; zone < ZONE_COUNT; ++zone) {
				const double capacity = zone_capacity(land, zone);
				const double used = usage[index * ZONE_COUNT + zone];
				const double occupancy = capacity > 0.0
					? used / capacity : (used > 0.0 ? 1.0e9 : 0.0);
				// Curva convexa perto de 1, com teto: ocupação alta pressiona o
				// preço muito mais que proporcionalmente.
				const double pressure = std::min(kDemoUrbanLandMaxPressure,
					occupancy * occupancy / std::max(0.05, 1.0 - occupancy));
				const double target = price_per_m2 * income_factor * (1.0 + pressure);
				land.land_price[zone] += (target - land.land_price[zone])
					* kDemoUrbanLandPriceSmooth;
				if (!std::isfinite(land.land_price[zone])
						|| land.land_price[zone] < 0.0) {
					land.land_price[zone] = 0.0;
				}
			}
		}
		// Sinal antigo perde força: a prefeitura reage à demanda corrente.
		for (int zone = 0; zone < ZONE_COUNT; ++zone) {
			land.zone_demand[zone] *= kDemoUrbanZoneDemandDecay;
		}
	}
}

bool start_project(WorldState& ws, SimCore& core, std::size_t city_index, int zone,
		int action, double km2, const std::string& owner, std::string* error,
		std::string* project_id) {
	if (error != nullptr) {
		error->clear();
	}
	if (project_id != nullptr) {
		project_id->clear();
	}
	if (city_index >= ws.cities.size() || city_index >= ws.land.size()) {
		if (error != nullptr) {
			*error = "cidade sem solo urbano";
		}
		return false;
	}
	if (zone < 0 || zone >= ZONE_COUNT) {
		if (error != nullptr) {
			*error = "zona inválida";
		}
		return false;
	}
	CityLand& land = ws.land[city_index];
	DemoCity& city = ws.cities[city_index];
	double cost = 0.0;
	double area_km2 = 0.0;
	if (action == URBAN_EXPAND) {
		if (!std::isfinite(km2) || km2 <= 0.0) {
			if (error != nullptr) {
				*error = "área inválida";
			}
			return false;
		}
		if (km2 > land.reserve_km2 + 1e-9) {
			if (error != nullptr) {
				*error = "reserva insuficiente";
			}
			return false;
		}
		area_km2 = km2;
		cost = km2 * kDemoUrbanInfraCostPerKm2;
	} else if (action == URBAN_DENSIFY) {
		const int density = std::clamp(
			static_cast<int>(land.max_density[zone]), 0, DENSITY_COUNT - 1);
		if (density >= DENSITY_HIGH) {
			if (error != nullptr) {
				*error = "zona já está na densidade máxima";
			}
			return false;
		}
		// Adensar exige infraestrutura que atenda a população atual da cidade.
		if (land.infra_capacity + 1e-6 < city.simulation_population) {
			if (error != nullptr) {
				*error = "infraestrutura insuficiente";
			}
			return false;
		}
		cost = std::max(0.0, land.area_km2[zone]) * kDemoUrbanInfraCostPerKm2
			* kDemoUrbanDensifyCostFactor;
	} else {
		if (error != nullptr) {
			*error = "tipo de obra urbana inválido";
		}
		return false;
	}
	if (!std::isfinite(cost) || cost <= 0.0) {
		if (error != nullptr) {
			*error = "custo de obra inválido";
		}
		return false;
	}
	// Anti-duplicação: uma obra urbana por zona (a prefeitura não empilha a
	// mesma expansão todo mês enquanto a anterior está em andamento).
	for (const DemoConstructionProject& project : ws.construction_projects) {
		if (project.city == city.id && project.urban_action != URBAN_NONE
				&& project.urban_zone == zone) {
			if (error != nullptr) {
				*error = "obra urbana em andamento";
			}
			return false;
		}
	}
	const std::string commit_owner = owner == "private" ? "private" : "state";
	const double committed = committed_cost(ws, city.id, commit_owner);
	double budget = 0.0;
	if (commit_owner == "state") {
		budget = std::max(0.0, city.government_budget);
	} else {
		budget = std::max(0.0, city.firm_cash);
	}
	if (committed + cost > budget) {
		if (error != nullptr) {
			*error = commit_owner == "state" ? "orçamento insuficiente"
				: "capital insuficiente";
		}
		return false;
	}

	DemoConstructionProject project;
	// Id estável por cidade/zona/tipo/dia: reprodutível no save/load e único
	// (a anti-duplicação impede repetir a mesma zona no mesmo dia).
	project.id = "URB-" + city.id + "-Z" + std::to_string(zone) + "-"
		+ std::to_string(action) + "-" + std::to_string(ws.day);
	project.building_id = project.id;
	project.city = city.id;
	project.owner = commit_owner;
	project.resource.clear();
	project.output_product = -1;
	project.recipe = -1;
	project.input_product = -1;
	// `capacity` aqui é só a RÉGUA de tamanho do §15 (escala o prazo); a obra
	// urbana não cria prédio. Adensar equivale a um lote pequeno.
	project.capacity = action == URBAN_EXPAND
		? area_km2 * kDemoUrbanKm2ToConstructionUnits : 1.0;
	project.total_cost = cost;
	project.reserved = cost;
	project.urban_action = action;
	project.urban_zone = zone;
	project.urban_km2 = area_km2;
	if (commit_owner == "state") {
		city.government_budget -= cost;
	} else {
		city.firm_cash -= cost;
	}
	core.demo_add_construction_project(project);
	if (project_id != nullptr) {
		*project_id = project.id;
	}
	return true;
}

void apply_completed_project(WorldState& ws,
		const DemoConstructionProject& project) {
	if (project.urban_action == URBAN_NONE) {
		return;
	}
	const int index = city_index_of(ws, project.city);
	if (index < 0 || static_cast<std::size_t>(index) >= ws.land.size()) {
		return;  // cidade sumiu: a obra se perde, o dinheiro já foi pago
	}
	CityLand& land = ws.land[static_cast<std::size_t>(index)];
	const int zone = std::clamp(project.urban_zone, 0, ZONE_COUNT - 1);
	if (project.urban_action == URBAN_EXPAND) {
		const double km2 = std::max(0.0, project.urban_km2);
		land.area_km2[zone] += km2;
		land.reserve_km2 = std::max(0.0, land.reserve_km2 - km2);
		// Terra nova vem com a infraestrutura da obra: mais gente atendida.
		land.infra_capacity += km2 * kDemoUrbanInfraPopPerKm2;
	} else if (project.urban_action == URBAN_DENSIFY) {
		const int density = std::clamp(
			static_cast<int>(land.max_density[zone]), 0, DENSITY_COUNT - 1);
		if (density < DENSITY_HIGH) {
			land.max_density[zone] = static_cast<Density>(density + 1);
		}
	}
}

} // namespace urban_land
} // namespace ogs
