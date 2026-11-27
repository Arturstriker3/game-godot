#include "scenario_fixture.h"

#include "sim_constants.h"
#include "efficiency.h"

namespace ogs {

void initialize_scenario_fixture(WorldState& ws, SimCore& core) {
	ws.active = true;
	ws.world_mode = false;
	ws.day = 0;
	ws.cities.clear();
	ws.buildings.clear();
	ws.construction_projects.clear();
	// §19: o cenário sintético não tem dado urbano (vetores vazios).
	ws.land.clear();
	ws.housing.clear();
	ws.city_gov.clear();
	ws.in_transit.clear();
	ws.ports.clear();
	ws.last_orders.clear();
	ws.bankruptcies = 0;
	ws.depleted_mines = 0;
	ws.transport_cash = 10000.0;
	ws.foreign_cash = 0.0;
	ws.construction_cash = 0.0;
	ws.industry_seeded = false;
	for (int index = 0; index < kMaxProducts; ++index) {
		ws.produced[index] = 0.0;
		ws.consumed_total[index] = 0.0;
		ws.produced_today[index] = 0.0;
		ws.consumed_today[index] = 0.0;
	}

	City agria;
	agria.id = "AGRIA";
	agria.name = "Agria";
	agria.nation = "DEMO";
	agria.center = {2532.6, 571.4};
	agria.population = 100000.0;
	agria.simulation_population = agria.population;
	agria.firm_cash = 50000.0;
	agria.service_cash = 30000.0;
	agria.government_budget = 100000.0;
	agria.service_jobs = agria.simulation_population * kServiceShare;
	agria.service_wage = kServiceWage;
	agria.pop.population = agria.population;
	agria.pop.savings = 50000.0;
	agria.suitability[0] = kReferenceWheatSuitability;
	agria.markets[0] = {100.0, core.base_price(0), 0.0, 0.0};
	for (int product = 1; product < ws.products; ++product) {
		agria.markets[product].price = core.base_price(product);
	}
	ws.cities.push_back(agria);

	City portus;
	portus.id = "PORTUS";
	portus.name = "Portus";
	portus.nation = "DEMO";
	portus.center = {2560.4, 543.8};
	portus.population = 200000.0;
	portus.simulation_population = portus.population;
	portus.firm_cash = 80000.0;
	portus.service_cash = 60000.0;
	portus.government_budget = 150000.0;
	portus.service_jobs = portus.simulation_population * kServiceShare;
	portus.service_wage = kServiceWage;
	portus.pop.population = portus.population;
	portus.pop.savings = 100000.0;
	portus.suitability[0] = 0.625;
	portus.markets[0] = {0.0, core.base_price(0), 0.0, 0.0};
	for (int product = 1; product < ws.products; ++product) {
		portus.markets[product].price = core.base_price(product);
	}
	ws.cities.push_back(portus);

	// Capacidade é o potencial da terra; a produção efetiva é multiplicada
	// pela aptidão territorial (wheat). A comida é consumida direto do trigo —
	// farinha/pão foram removidos da cadeia (subprodutos ficam para depois).
	const double agria_farm_capacity =
		kFarmCapacityTons / kReferenceWheatSuitability;
	for (int index = 0; index < 3; ++index) {
		Building farm;
		farm.id = "FARM-A-" + std::to_string(index + 1);
		farm.city = "AGRIA";
		farm.kind = 0;
		farm.resource = "wheat";
		farm.input_product = -1;
		farm.output_product = 0;
		farm.capacity = agria_farm_capacity;
		farm.input_per_output = 0.0;
		farm.jobs = 150.0;
		farm.wage = 2.0;
		farm.opex = 50.0;
		// D-125: fazenda do cenário de validação é pré-existente (madura).
		eff::mature(farm, true);
		ws.buildings.push_back(farm);
	}

	// Portus também produz trigo: reduz a dependência de importação.
	const double portus_farm_capacity = kFarmCapacityTons / 0.625;
	for (int index = 0; index < 2; ++index) {
		Building farm;
		farm.id = "FARM-P-" + std::to_string(index + 1);
		farm.city = "PORTUS";
		farm.kind = 0;
		farm.resource = "wheat";
		farm.input_product = -1;
		farm.output_product = 0;
		farm.capacity = portus_farm_capacity;
		farm.input_per_output = 0.0;
		farm.jobs = 150.0;
		farm.wage = 2.0;
		farm.opex = 50.0;
		eff::mature(farm, true);
		ws.buildings.push_back(farm);
	}
	for (City& city : ws.cities) {
		city.pop.desired_by_category.assign(ws.food_products.size(), 0.0);
		city.pop.consumed_by_category.assign(ws.food_products.size(), 0.0);
		core.init_pop_needs(city.pop);
		// F7: cenário de validação também tem mercado de trabalho (índice 1,0).
		core.apply_labor_profile(city);
	}
	core.rebuild_indexes();
	// F4: o cenário mínimo também tem frota (nação DEMO).
	core.init_fleets();
	// F8: negócios de serviço iniciais (só quando os dados foram carregados).
	core.seed_services();
	// F9: grupos a partir dos ativos privados (id determinístico).
	core.seed_groups();
	// R1a (QA t167): o cenário mínimo também respeita `rework.regional_cash`.
	// Como o fixture preserva a flag no WorldState, ligar antes de criá-lo
	// converte os dois caixas em pool + sub-saldos (antes o demo/teste rodava
	// OFF em silêncio com a flag ligada). Desligada, não muda nada.
	core.set_regional_cash(ws.regional_cash, ws.external_subceiling);
	// R1b: o cenario minimo tambem respeita `rework.national_treasury`.
	core.set_national_treasury(ws.national_treasury,
		ws.treasury_envelope_share, ws.treasury_floor_fraction,
		ws.treasury_envelope_works_share, ws.treasury_maintenance_share);
}

} // namespace ogs
