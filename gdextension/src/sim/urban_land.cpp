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
inline bool is_urban_factory(const Building& building) {
	return building.recipe >= 0 || building.input_product >= 0;
}

inline bool is_urban_factory(const ConstructionProject& project) {
	return !project.expand_existing
		&& (project.recipe >= 0 || project.input_product >= 0);
}

// Custo comprometido e ainda não pago de um dono numa cidade (mesma regra de
// sim/construction.cpp: o dono não inicia obra que estoure o caixa atual).
inline double committed_cost(const WorldState& ws, const std::string& city_id,
		const std::string& owner) {
	double committed = 0.0;
	for (const ConstructionProject& project : ws.construction_projects) {
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

// Raio equivalente da cidade (km) a partir da área urbana total: um disco de
// mesma área. É a régua do custo crescente — redes mais longas até o centro.
double city_radius_km(double area_km2) {
	if (area_km2 <= 0.0) {
		return 0.0;
	}
	return std::sqrt(area_km2 / 3.14159265358979323846);
}

// Área da MALHA FORMAL (zonas RES/COM/IND): a régua do raio é o tecido urbano
// zoneado. A área informal (favela) fica de fora porque nasce fora do
// zoneamento e não é o que o município expande com infraestrutura.
double urban_area_km2(const CityLand& land) {
	double area = 0.0;
	for (int zone = 0; zone < ZONE_COUNT; ++zone) {
		area += std::max(0.0, land.area_km2[zone]);
	}
	return area;
}

// Fator de distância: 1,0 no tamanho do carregamento e linear no raio.
double distance_factor(const CityLand& land) {
	if (land.initial_area_km2 <= 0.0) {
		return 1.0;
	}
	const double initial_radius = city_radius_km(land.initial_area_km2);
	const double radius = city_radius_km(urban_area_km2(land));
	if (initial_radius <= 0.0 || radius <= 0.0) {
		return 1.0;
	}
	return 1.0 + kUrbanExpandDistanceK
		* std::max(0.0, radius / initial_radius - 1.0);
}

// Fator de relevo: 1,0 com a reserva intacta e 1+k quando ela foi toda usada —
// o que sobra na reserva é o terreno pior (encosta, várzea).
double relief_factor(const CityLand& land) {
	if (land.initial_reserve_km2 <= 0.0) {
		return 1.0;
	}
	const double used = std::clamp(
		1.0 - std::max(0.0, land.reserve_km2) / land.initial_reserve_km2,
		0.0, 1.0);
	return 1.0 + kUrbanExpandReliefK * used;
}

// Fração do custo de infraestrutura por km² que o passo de adensamento custa,
// conforme o nível de destino (LOW→MID barato, MID→HIGH pesado).
double densify_factor(int density) {
	return density <= DENSITY_LOW ? kUrbanDensifyCostFactorLowMid
		: kUrbanDensifyCostFactorMidHigh;
}

// Escala monetária local da obra: alvo de receita municipal do MÊS (fonte ×
// PIB do mês). Antes do primeiro fechamento o alvo ainda é 0: estima pela
// mesma conta da fase fiscal (PIB do dia × 30 × % do PIB da fonte). Sem dado
// municipal (cenário sintético), devolve 0 e o piso declarado entra.
double municipal_month_scale(const WorldState& ws, std::size_t city_index) {
	if (city_index >= ws.cities.size()) {
		return 0.0;
	}
	if (city_index < ws.city_gov.size()) {
		const CityGov& gov = ws.city_gov[city_index];
		if (gov.target_revenue_month > 0.0) {
			return gov.target_revenue_month;
		}
		const double monthly_gdp =
			std::max(0.0, ws.cities[city_index].gdp) * 30.0;
		return std::max(0.0, gov.revenue_pct_gdp) * monthly_gdp;
	}
	return 0.0;
}

// Custo base de 1 km² de infraestrutura (XGS): meses de receita municipal,
// com piso declarado para cidade sem dado municipal.
double base_infra_cost_per_km2(const WorldState& ws, std::size_t city_index) {
	const double scale = municipal_month_scale(ws, city_index);
	return std::max(kUrbanInfraFloorPerKm2,
		kUrbanInfraCostMonths * scale);
}

// Troca a receita de material do adensamento MID→HIGH: mais aço e elétrica,
// mantendo a mesma fração do custo total (o contrato do §15 não muda).
void reshape_high_density_materials(WorldState& ws, SimCore& core,
		const std::string& project_id) {
	if (ws.construction_projects.empty()) {
		return;
	}
	ConstructionProject& project = ws.construction_projects.back();
	if (project.id != project_id || project.material_count <= 0) {
		return;
	}
	for (int m = 0; m < project.material_count; ++m) {
		const int product = project.materials[m];
		const std::string name = core.product_name(product);
		if (name == "steel_structure") {
			project.material_qty[m] *= kUrbanDensifyHighSteelWeight;
		} else if (name == "electrical_material") {
			project.material_qty[m] *= kUrbanDensifyHighElectricalWeight;
		}
	}
	double value_after = 0.0;
	for (int m = 0; m < project.material_count; ++m) {
		const int product = project.materials[m];
		value_after += project.material_qty[m]
			* std::max(0.0, core.base_price(product));
	}
	if (value_after <= 0.0) {
		return;
	}
	const double target = kConstructionMaterialShare
		* std::max(0.0, project.total_cost);
	const double scale = target / value_after;
	for (int m = 0; m < project.material_count; ++m) {
		project.material_qty[m] *= scale;
	}
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
	const double far = kUrbanFar[density];
	if (far <= 0.0) {
		return 0.0;
	}
	// RES: unidades de moradia. COM/IND: TRABALHADORES que o piso comporta
	// (m² úteis por trabalhador, ver sim_constants.h). A zona limita pelo
	// emprego, então uma metrópole cheia encosta na capacidade de verdade.
	const double per_unit_m2 = zone == ZONE_RES
		? kUrbanUnitM2[density][TIER_MEDIO]
		: zone == ZONE_COM ? kUrbanServiceM2PerWorker
		: kUrbanFactoryM2PerWorker;
	if (per_unit_m2 <= 0.0) {
		return 0.0;
	}
	// Q-015-C: o envelope (área × FAR ÷ m²) é o limite do LOTE. Na escala da
	// cidade só uma fração dele está construída — `zone_fill` guarda essa
	// fração (calibrada no dia 0 pela faixa de população urbana). Sem ela a
	// cidade nasce com 5-15× de folga e o preço da terra nunca reage.
	const double fill = std::max(0.0, land.zone_fill[zone]);
	return area_m2 * far * fill / per_unit_m2;
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
		// Q-015-C (§19, objetivo 4): a unidade INFORMAL não ocupa a zona
		// formal — ela mora em `informal_km2`, fora do envelope zoneado. O
		// estoque carregado inclui as unidades de posse informal (subset de
		// `units`), então elas saem do uso da zona para a ocupação medir só a
		// moradia formal.
		return std::max(0.0, usage - std::max(0.0, house.informal_units));
	}
	if (zone == ZONE_COM) {
		// Empregos dos negócios de serviço ativos (cada negócio declara `jobs`).
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
					usage += std::max(0.0, ws.service_businesses[
						static_cast<std::size_t>(business_index)].jobs);
				}
			}
			return usage;
		}
		for (const ServiceBusiness& business : ws.service_businesses) {
			if (!business.bankrupt && business.city == city_id) {
				usage += std::max(0.0, business.jobs);
			}
		}
		return usage;
	}
	// IND: empregos das plantas em operação + empregos contratados nas obras de
	// fábrica em andamento na cidade.
	if (city_index < ws.buildings_by_city.size()) {
		for (const int building_index : ws.buildings_by_city[city_index]) {
			if (building_index >= 0
					&& static_cast<std::size_t>(building_index) < ws.buildings.size()) {
				const Building& building =
					ws.buildings[static_cast<std::size_t>(building_index)];
				if (is_urban_factory(building)) {
					usage += std::max(0.0, building.jobs);
				}
			}
		}
	} else {
		for (const Building& building : ws.buildings) {
			if (building.city == city_id && is_urban_factory(building)) {
				usage += std::max(0.0, building.jobs);
			}
		}
	}
	for (const ConstructionProject& project : ws.construction_projects) {
		if (project.city == city_id && is_urban_factory(project)) {
			usage += std::max(0.0, project.jobs);
		}
	}
	return usage;
}

double zone_occupancy(const WorldState& ws, std::size_t city_index, int zone) {
	if (city_index >= ws.land.size() || zone < 0 || zone >= ZONE_COUNT) {
		return 0.0;
	}
	const double capacity = zone_capacity_for(ws, city_index, zone);
	const double usage = zone_usage(ws, city_index, zone);
	if (capacity <= 0.0) {
		return usage > 0.0 ? 1.0e9 : 0.0;
	}
	return usage / capacity;
}

double zone_capacity_for(const WorldState& ws, std::size_t city_index, int zone) {
	if (city_index >= ws.land.size() || zone < 0 || zone >= ZONE_COUNT) {
		return 0.0;
	}
	const double capacity = zone_capacity(ws.land[city_index], zone);
	if (zone != ZONE_RES) {
		// COM/IND: a capacidade é o piso que cabe no envelope. Uso acima dela
		// é sinal legítimo de zona cheia (o gate fecha e a prefeitura expande)
		// — não se aplica o piso do estoque.
		return capacity;
	}
	// Q-015-C: a capacidade RES nunca fica abaixo do estoque JÁ existente
	// dividido pelo teto de ocupação (0,95). Estoque acima do envelope
	// calibrado é inconsistência de fonte (ex.: NPL-POK, área do Atlas 2014 ×
	// população 2026), não cidade com 137% de ocupação: a capacidade efetiva
	// tem piso em estoque ÷ teto, sem inventar área. Cidade no teto está cheia
	// — moradia nova exige expandir/adensar (o envelope cresce).
	const double usage = zone_usage(ws, city_index, zone);
	return std::max(capacity, usage / kUrbanOccupancyCap);
}

bool zone_has_room(const WorldState& ws, std::size_t city_index, int zone,
		double slots) {
	if (city_index >= ws.land.size() || zone < 0 || zone >= ZONE_COUNT) {
		// Sem solo carregado não há limite de zona (cenário sintético/save
		// antigo): ausência explícita, o comportamento anterior é preservado.
		return true;
	}
	const double capacity = zone_capacity_for(ws, city_index, zone);
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
	const std::size_t count = std::min(
		{ws.land.size(), ws.housing.size(), ws.cities.size()});
	if (count == 0) {
		return;
	}
	// A6: cada cidade fecha o mês no seu dia (`(dia - índice) % 30 == 0`), em
	// vez de todas no dia múltiplo de 30. O trabalho por dia cai a ~1/30 e o
	// resultado ao longo do mês é o mesmo.
	const int period = ws.cadence.monthly_period;
	bool any_due = false;
	for (std::size_t index = 0; index < count && !any_due; ++index) {
		any_due = cadence_due(ws.day, index, period);
	}
	if (!any_due) {
		return;
	}
	// Uso por cidade — SÓ para as cidades do lote do dia (A6). Calcular as 834
	// todo dia devolveria o custo ao caminho diário (o índice por cidade evita
	// 42 mil buscas por id a cada tick). Os negócios vêm do índice
	// `service_by_city` — a mesma fonte que `zone_usage` já usa.
	std::vector<char> due_mask(count, 0);
	for (std::size_t index = 0; index < count; ++index) {
		due_mask[index] = cadence_due(ws.day, index, period) ? 1 : 0;
	}
	std::vector<double> usage(count * ZONE_COUNT, 0.0);
	for (std::size_t index = 0; index < count; ++index) {
		if (!due_mask[index]) {
			continue;
		}
		const CityHousing& house = ws.housing[index];
		double units = 0.0;
		for (int density = 0; density < DENSITY_COUNT; ++density) {
			for (int tier = 0; tier < TIER_COUNT; ++tier) {
				units += std::max(0.0, house.units[density][tier]);
			}
		}
		usage[index * ZONE_COUNT + ZONE_RES] = units;
	}
	const bool have_service_index = ws.service_by_city.size() >= count;
	for (std::size_t index = 0; index < count; ++index) {
		if (!due_mask[index]) {
			continue;
		}
		if (have_service_index) {
			for (const int business_index : ws.service_by_city[index]) {
				if (business_index < 0 || static_cast<std::size_t>(business_index)
						>= ws.service_businesses.size()) {
					continue;
				}
				const ServiceBusiness& business =
					ws.service_businesses[static_cast<std::size_t>(business_index)];
				if (business.bankrupt) {
					continue;
				}
				usage[index * ZONE_COUNT + ZONE_COM]
					+= std::max(0.0, business.jobs);
			}
			continue;
		}
		// Índice indisponível (mundo recém-carregado sem o catálogo): varredura
		// global, o caminho anterior — ausência explícita, sem inventar uso.
		for (const ServiceBusiness& business : ws.service_businesses) {
			if (business.bankrupt) {
				continue;
			}
			const int found = city_index_of(ws, business.city);
			if (found >= 0 && static_cast<std::size_t>(found) == index) {
				usage[index * ZONE_COUNT + ZONE_COM]
					+= std::max(0.0, business.jobs);
			}
		}
	}
	for (const Building& building : ws.buildings) {
		if (!is_urban_factory(building)) {
			continue;
		}
		const int index = city_index_of(ws, building.city);
		if (index >= 0 && static_cast<std::size_t>(index) < count
				&& due_mask[static_cast<std::size_t>(index)] != 0) {
			usage[static_cast<std::size_t>(index) * ZONE_COUNT + ZONE_IND]
				+= std::max(0.0, building.jobs);
		}
	}
	for (const ConstructionProject& project : ws.construction_projects) {
		if (!is_urban_factory(project)) {
			continue;
		}
		const int index = city_index_of(ws, project.city);
		if (index >= 0 && static_cast<std::size_t>(index) < count
				&& due_mask[static_cast<std::size_t>(index)] != 0) {
			usage[static_cast<std::size_t>(index) * ZONE_COUNT + ZONE_IND]
				+= std::max(0.0, project.jobs);
		}
	}

	for (std::size_t index = 0; index < count; ++index) {
		if (!cadence_due(ws.day, index, period)) {
			continue;  // cidade fora do lote do dia (A6)
		}
		CityLand& land = ws.land[index];
		const City& city = ws.cities[index];
		const CityHousing& house = ws.housing[index];
		// Base do preço da terra: parcela do terreno no valor do imóvel médio
		// (XGS/m²) na densidade permitida. Sem preço na fonte (0) o preço da
		// terra NÃO é inventado: fica como está (ausência explícita).
		const int density = std::clamp(
			static_cast<int>(land.max_density[ZONE_RES]), 0, DENSITY_COUNT - 1);
		const double unit_m2 = kUrbanUnitM2[density][TIER_MEDIO];
		const double price_per_m2 = unit_m2 > 0.0
			? std::max(0.0, house.price[TIER_MEDIO]) * kUrbanLandShareOfHousePrice
				/ unit_m2 : 0.0;
		if (price_per_m2 > 0.0) {
			// Renda média local: índice da nação relativo à média mundial
			// (1,0), linear e limitado — renda alta encarece o solo.
			const double income = std::isfinite(city.external_income_index)
				? city.external_income_index : 1.0;
			const double income_factor = 1.0 + kUrbanLandIncomeGain
				* std::clamp(income - 1.0, -0.5, 1.0);
			for (int zone = 0; zone < ZONE_COUNT; ++zone) {
				const double capacity = zone_capacity(land, zone);
				const double used = usage[index * ZONE_COUNT + zone];
				const double occupancy = capacity > 0.0
					? used / capacity : (used > 0.0 ? 1.0e9 : 0.0);
				// Curva convexa perto de 1, com teto: ocupação alta pressiona o
				// preço muito mais que proporcionalmente.
				const double pressure = std::min(kUrbanLandMaxPressure,
					occupancy * occupancy / std::max(0.05, 1.0 - occupancy));
				const double target = price_per_m2 * income_factor * (1.0 + pressure);
				land.land_price[zone] += (target - land.land_price[zone])
					* kUrbanLandPriceSmooth;
				if (!std::isfinite(land.land_price[zone])
						|| land.land_price[zone] < 0.0) {
					land.land_price[zone] = 0.0;
				}
			}
		}
		// Sinal antigo perde força: a prefeitura reage à demanda corrente.
		for (int zone = 0; zone < ZONE_COUNT; ++zone) {
			land.zone_demand[zone] *= kUrbanZoneDemandDecay;
		}
	}
}

double expansion_cost_per_km2(const WorldState& ws, std::size_t city_index) {
	if (city_index >= ws.land.size()) {
		return 0.0;
	}
	const CityLand& land = ws.land[city_index];
	return base_infra_cost_per_km2(ws, city_index) * distance_factor(land)
		* relief_factor(land);
}

double densify_cost(const WorldState& ws, std::size_t city_index, int zone) {
	if (city_index >= ws.land.size() || zone < 0 || zone >= ZONE_COUNT) {
		return 0.0;
	}
	const CityLand& land = ws.land[city_index];
	const int density = std::clamp(
		static_cast<int>(land.max_density[zone]), 0, DENSITY_COUNT - 1);
	if (density >= DENSITY_HIGH) {
		return 0.0;
	}
	return std::max(0.0, land.area_km2[zone])
		* base_infra_cost_per_km2(ws, city_index)
		* densify_factor(density) * distance_factor(land);
}

bool start_project(WorldState& ws, SimCore& core, std::size_t city_index, int zone,
		int action, double km2, const std::string& owner, std::string* error,
		std::string* project_id, CityGov* payer) {
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
	City& city = ws.cities[city_index];
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
		cost = km2 * expansion_cost_per_km2(ws, city_index);
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
		// Custo do nível: LOW→MID reforço de redes; MID→HIGH aço/elevadores
		// (spec "Custo crescente"), mais o fator de distância da cidade.
		cost = densify_cost(ws, city_index, zone);
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
	for (const ConstructionProject& project : ws.construction_projects) {
		if (project.city == city.id && project.urban_action != URBAN_NONE
				&& project.urban_zone == zone) {
			if (error != nullptr) {
				*error = "obra urbana em andamento";
			}
			return false;
		}
	}
	const std::string commit_owner = owner == "private" ? "private" : "state";
	// Gate do caixa: CADA CONTA responde só pelo que ela paga.
	//
	// INTEGRAÇÃO W2: a prefeitura paga do PRÓPRIO caixa (`payer`), e o custo
	// inteiro é debitado no ato (abaixo). O caixa corrente já está líquido de
	// todas as obras do município, então somar `committed_cost("state")` aqui
	// misturava contas e cobrava a obra duas vezes:
	//   1. as obras §15 de PRÉDIO do Estado saem de `city.government_budget`
	//      (economy_sim.cpp, `build`), nunca do caixa municipal — mesmo que
	//      o campo se chame "state", é o tesouro nacional;
	//   2. as obras que o município paga (a urbana aqui e a moradia do governo,
	//      `housing::start_project`) já tiveram `total_cost` debitado do caixa
	//      no ato, e `committed_cost` conta de novo o que ainda não foi pago.
	// Esse duplo cômputo era o gate que barrava a prefeitura em cidade cheia
	// com caixa (Q-015, B3). O orçamento público e o caixa da firma mantêm a
	// regra anterior (`committed + cost`), porque lá o compromisso do dono é a
	// régua declarada do §15 e mexer nela mudaria a economia privada.
	double committed = 0.0;
	double budget = 0.0;
	if (payer != nullptr) {
		budget = std::max(0.0, payer->cash);
	} else if (commit_owner == "state") {
		committed = committed_cost(ws, city.id, commit_owner);
		budget = std::max(0.0, city.government_budget);
	} else {
		committed = committed_cost(ws, city.id, commit_owner);
		budget = std::max(0.0, city.firm_cash);
	}
	if (committed + cost > budget) {
		if (error != nullptr) {
			*error = payer != nullptr ? "caixa da prefeitura insuficiente"
				: commit_owner == "state" ? "orçamento insuficiente"
				: "capital insuficiente";
		}
		return false;
	}

	ConstructionProject project;
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
	// urbana não cria prédio. Adensar equivale a um lote pequeno. O prazo
	// CRESCE com a distância (redes mais longas, spec "Custo crescente").
	const double size_scale = distance_factor(land);
	project.capacity = (action == URBAN_EXPAND
		? area_km2 * kUrbanKm2ToConstructionUnits : 1.0) * size_scale;
	project.total_cost = cost;
	project.reserved = cost;
	project.urban_action = action;
	project.kind = CONSTRUCTION_URBAN_INFRA;
	project.urban_zone = zone;
	project.urban_km2 = area_km2;
	if (payer != nullptr) {
		payer->cash = std::max(0.0, payer->cash) - cost;
	} else if (commit_owner == "state") {
		city.government_budget -= cost;
	} else {
		city.firm_cash -= cost;
	}
	core.add_construction_project(project);
	// MID→HIGH: a receita de obra troca para aço/elétrica (mesma fração do
	// custo total), depois que o §15 montou a receita padrão.
	if (action == URBAN_DENSIFY
			&& static_cast<int>(land.max_density[zone]) == DENSITY_MID) {
		reshape_high_density_materials(ws, core, project.id);
	}
	if (project_id != nullptr) {
		*project_id = project.id;
	}
	return true;
}

void apply_completed_project(WorldState& ws,
		const ConstructionProject& project) {
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
		land.infra_capacity += km2 * kUrbanInfraPopPerKm2;
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
