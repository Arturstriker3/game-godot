#pragma once
// Estado da simulação econômica (S5a-2 do docs/economy/core_refactor_plan.md).
// C++ puro (só STL, via sim_types.h) — nada de godot_cpp. É o "WorldState" que
// o SimCore (sim_core.*) recebe e muta a cada tick; a fachada EconomySim
// (economy_sim.*) guarda UM `WorldState ws_` e converte Godot <-> sim.

#include "sim_types.h"

namespace ogs {

struct WorldState {
	int day = 0;

	// Estado do cenário de validação de duas cidades.
	bool active = false;
	// true quando o demo foi inicializado a partir do mundo real
	// (initialize_demo_world), em vez do cenário AGRIA/PORTUS. Ativa o comércio
	// genérico entre cidades no lugar dos blocos específicos de validação.
	bool world_mode = false;
	// Parâmetros de balanceamento do cenário demo (configuráveis pelo
	// laboratório; os padrões preservam o comportamento do jogo).
	double natural_unemployment = 0.04;
	double service_wage_adjust = 0.10;
	double service_wage_max = 1.0;
	// F7: rigidez salarial (fração máxima de ajuste por dia) e piso de salário
	// mínimo por nação (0 = desligado). Ganchos de política.
	double wage_rigidity = 0.01;
	// F7 calibração: folga de custo de vida não modelado no gate de renda →
	// nível de consumo. A cesta de needs.json cobre só os bens comerciáveis;
	// moradia/energia/serviços não têm preço no núcleo, então o nível exige uma
	// folga. Sem ela qualquer renda acima da cesta liberava luxury no mundo
	// inteiro (832/834 cidades). Calibrada no economy_world_test.
	double income_basket_margin = 2.30;
	std::unordered_map<std::string, double> minimum_wages;
	double inventory_cover_days = 3.0;
	double inventory_recovery = 0.5;
	// Índice espacial do comércio de alimento: por comprador, só os K
	// vendedores mais próximos entram na disputa, além de M ofertas
	// representativas do mercado externo (as mais baratas por caloria no globo).
	// Mundos pequenos (<= kDemoTradeExhaustiveLimit) mantêm a varredura completa
	// para preservar exatamente o comportamento validado no cenário real.
	int trade_neighbors = 12;
	int trade_external = 8;
	std::vector<DemoCity> cities;
	std::vector<DemoBuilding> buildings;
	// F8: catálogo de serviços e negócios (um vetor global; índices por cidade
	// e por setor são reconstruídos quando o conjunto muda).
	bool services_loaded = false;
	double service_government_share = 0.002;
	bool sector_loaded = false;
	std::vector<DemoSectorAnchor> sector_anchors;
	// Demanda de trabalho agrícola: t/dia × base / TFP^expoente.
	double agri_labor_per_ton = 55.0;
	double agri_labor_exponent = 1.4486;
	double agri_production_band_min = 0.9;
	double agri_production_band_max = 1.1;
	// Autoconsumo máximo das famílias agrícolas, como fração da produção
	// local disponível — o resto da produção vai ao mercado.
	double agri_own_consumption_share = 0.5;
	// Indústria: trabalhadores por unidade de valor adicionado (XGS/dia).
	double industry_labor_per_va = 3.0;
	double industry_labor_exponent = 1.4486;
	std::vector<DemoServiceSector> service_sectors;
	std::vector<DemoServiceBusiness> service_businesses;
	std::vector<std::vector<int>> service_by_city;
	std::vector<std::vector<int>> service_by_sector;
	int next_service_business_id = 1;
	int service_bankruptcies = 0;
	// Índices derivados do mundo demo (ver demo_rebuild_indexes).
	std::unordered_map<std::string, int> city_lookup;
	std::vector<std::vector<int>> buildings_by_city;
	// F4: cidades por nação (ordem de índice) para a frota/compra do governo.
	std::unordered_map<std::string, std::vector<int>> cities_by_nation;
	std::vector<DemoShipment> in_transit;
	std::vector<std::string> invariant_violations;
	// Portos do cenário real (modo mundo) e pedidos de comércio do último tick,
	// para a camada visual (minimapa e comboios 3D).
	std::vector<Port> ports;
	std::vector<TradeOrder> last_orders;
	int bankruptcies = 0;
	int depleted_mines = 0;
	double transport_cash = 0.0;
	double foreign_cash = 0.0;
	// Setor de construção: recebe o capital investido em expansão.
	double construction_cash = 0.0;
	double produced[kDemoMaxProducts] = {};
	double consumed_total[kDemoMaxProducts] = {};
	double produced_today[kDemoMaxProducts] = {};
	double consumed_today[kDemoMaxProducts] = {};
	// Produtos ativos (demo_set_goods). 21 por padrão (cenário de validação).
	int products = 21;
	std::vector<std::string> product_names;
	std::unordered_map<std::string, int> product_lookup;
	// Receitas industriais e índice por id (recipes.json).
	std::vector<DemoRecipe> recipes;
	std::unordered_map<std::string, int> recipe_lookup;
	// Cesta alimentar data-driven (needs.json); default = 5 culturas validadas.
	std::vector<int> food_products;
	std::vector<double> food_factors;
	// F3: tabela de necessidades por nível (needs.json), substitutos,
	// equivalência e consumo institucional do governo. `needs_loaded`
	// falso = comportamento de 1 nível (cenário AGRIA/PORTUS e saves antigos).
	std::vector<DemoNeedsLevel> needs_levels;
	bool needs_loaded = false;
	int needs_food_level = -1;
	// Decaimento diário que perde exatamente durable_replacement_per_year em
	// 365 dias: 1 - (1 - r)^(1/365).
	double durable_decay = 0.0;
	std::vector<char> product_durable;  // por produto (goods.json durable)
	std::vector<int> government_products;
	std::vector<double> government_per_capita_day;
	// Bootstrap industrial já executado neste mundo.
	bool industry_seeded = false;
	// F4: frota por nação (ordem determinística em fleet_order) e índices
	// dos bens da frota (fuel/truck/cargo_ship/cargo_plane/spare_parts).
	std::unordered_map<std::string, DemoFleet> fleets;
	std::vector<std::string> fleet_order;
	int fleet_product_fuel = -1;
	int fleet_product_truck = -1;
	int fleet_product_ship = -1;
	int fleet_product_plane = -1;
	int fleet_product_parts = -1;
	// F5: moedas por code/nacao, ordem determinística e tarifas declaradas.
	std::vector<DemoCurrency> currencies;
	std::unordered_map<std::string, int> currency_by_code;
	std::unordered_map<std::string, int> nation_currency;
	std::vector<DemoTariff> tariffs;
	std::unordered_map<std::string, double> tariff_lookup;
	// F9: grupos/empresas, política de IDE, incentivos, confiança e fluxos.
	std::vector<DemoGroup> groups;
	std::unordered_map<std::string, int> group_lookup;
	bool groups_seeded = false;
	std::vector<DemoFdiPolicy> fdi_policies;
	std::unordered_map<std::string, int> fdi_lookup;
	std::vector<DemoSectorIncentive> incentives;
	std::unordered_map<std::string, int> incentive_lookup;
	std::unordered_map<std::string, double> confidence;
	std::unordered_map<std::string, int> confidence_penalty_day;
	std::unordered_map<std::string, DemoGroupFlow> group_flows;
	// Setor de cada produto (goods.json → sector). Vazio = "primary".
	std::vector<std::string> product_sectors;
	// Preços-base dos bens (data-driven: demo_set_goods/initialize_demo_world).
	double base_prices[kDemoMaxProducts] = {
		40.0, 60.0, 55.0, 42.0, 15.0, 25.0, 3000.0, 10.0, 200.0, 400.0,
		15.0, 5000.0, 6.0, 60.0, 30.0, 45.0, 35.0, 45.0, 70.0, 25.0, 500.0};
	// Retrato do tick reaproveitado entre dias (ver demo_tick_core).
	DemoSnapshot snapshot_buffer;
};

} // namespace ogs
