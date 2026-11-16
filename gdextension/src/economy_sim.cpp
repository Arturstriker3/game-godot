#include "economy_sim.h"
#include "sim/profiling.h"
#include "sim/sim_constants.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_float64_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <utility>

namespace godot {

using namespace ogs;

namespace {

// Segurança numérica: nada de NaN/Inf no estado nem nos outputs.
inline double safe_value(double v) {
	return std::isfinite(v) ? v : 0.0;
}
inline double safe_nonneg(double v) {
	return std::isfinite(v) ? std::max(0.0, v) : 0.0;
}
inline double safe_unit(double v) {
	return std::isfinite(v) ? std::clamp(v, 0.0, 1.0) : 0.0;
}

// População de SIMULAÇÃO da cidade. Desde a calibração de 2026, `population`
// no cenário é a população real exibida (nação inteira rateada) e a aglomeração
// da fonte viaja em `sim_population`. Sem o campo (save antigo, cenário
// sintético), a população histórica É a unidade de simulação — comportamento
// anterior preservado.
inline double read_sim_population(const Dictionary& data, double population) {
	if (!data.has("sim_population")) {
		return population;
	}
	const double value = safe_nonneg(static_cast<double>(data["sim_population"]));
	return value > 0.0 ? value : population;
}

double to_lon(double x) {
	return x / kMapWidth * 360.0 - 180.0;
}

double to_lat(double y) {
	return 90.0 - y / kMapHeight * 180.0;
}

double radians(double degrees) {
	return degrees * kPi / 180.0;
}

} // namespace

EconomySim::EconomySim() {
	// Produtos padrão (21 primários) e cesta alimentar validada. O autoload
	// sobrescreve ambos com goods.json/needs.json via demo_set_goods/needs.
	demo_products_ = 21;
	demo_product_names_.clear();
	demo_product_names_.reserve(21);
	static const char* const kDefaults[21] = {
		"wheat", "iron", "rice", "maize", "bauxite", "lead", "gold",
		"phosphate", "tungsten", "rare_earths", "sulfur", "diamond",
		"coal", "copper", "gas", "oil", "potato", "soy", "cotton", "wood", "uranium"};
	for (int index = 0; index < 21; ++index) {
		demo_product_names_.emplace_back(kDefaults[index]);
	}
	demo_product_lookup_.clear();
	for (int index = 0; index < demo_products_; ++index) {
		demo_product_lookup_[demo_product_names_[static_cast<std::size_t>(index)]] = index;
	}
	demo_food_products_.clear();
	demo_food_factors_.clear();
	for (int index = 0; index < 5; ++index) {
		demo_food_products_.push_back(kDemoFoodProductIds[index]);
		demo_food_factors_.push_back(kDemoCaloricFactor[index]);
	}
	// F3: sem goods.json carregado nenhum dos 21 primários é durável.
	demo_product_durable_.assign(kDemoMaxProducts, 0);
}

EconomySim::~EconomySim() {
	// Se o sim for liberado com um dia AINDA rodando na thread do pool (o jogo
	// encerrando, por exemplo), espera o tick sair antes de destruir os
	// contêineres do status: destruí-los debaixo de quem os escreve era um
	// access violation (0xC0000005) intermitente no fim do processo. O mutex é
	// recursivo, então a mesma thread do tick não bloqueia aqui.
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
}

void EconomySim::_bind_methods() {
	ClassDB::bind_method(D_METHOD("initialize_demo"), &EconomySim::initialize_demo);
	ClassDB::bind_method(D_METHOD("initialize_demo_world", "cities", "ports", "buildings"),
	                     &EconomySim::initialize_demo_world);
	ClassDB::bind_method(D_METHOD("tick_demo"), &EconomySim::tick_demo);
	ClassDB::bind_method(D_METHOD("demo_tick_and_publish"), &EconomySim::demo_tick_and_publish);
	ClassDB::bind_method(D_METHOD("demo_set_profiling", "enabled"),
	                     &EconomySim::demo_set_profiling);
	ClassDB::bind_method(D_METHOD("demo_get_profile"), &EconomySim::demo_get_profile);
	ClassDB::bind_method(D_METHOD("demo_warm_status"), &EconomySim::demo_warm_status);
	ClassDB::bind_method(D_METHOD("get_demo_status"), &EconomySim::get_demo_status);
	ClassDB::bind_method(D_METHOD("demo_price_shock", "product", "multiplier"),
	                     &EconomySim::demo_price_shock);
	ClassDB::bind_method(D_METHOD("demo_set_suitability", "city_id", "product", "value"),
	                     &EconomySim::demo_set_suitability);
	ClassDB::bind_method(D_METHOD("demo_set_deposit", "city_id", "product", "reserve_kt",
	                     "quality", "depth_m"),
	                     &EconomySim::demo_set_deposit);
	ClassDB::bind_method(D_METHOD("demo_set_external_demand", "city_id", "product",
	                     "ceiling_t_per_day"),
	                     &EconomySim::demo_set_external_demand);
	ClassDB::bind_method(D_METHOD("demo_set_external_income_index", "city_id", "index"),
	                     &EconomySim::demo_set_external_income_index);
	ClassDB::bind_method(D_METHOD("demo_set_productivity_index", "city_id", "index"),
	                     &EconomySim::demo_set_productivity_index);
	ClassDB::bind_method(D_METHOD("demo_set_income_gini", "city_id", "gini"),
	                     &EconomySim::demo_set_income_gini);
	ClassDB::bind_method(D_METHOD("demo_set_labor_profile", "city_id", "profile"),
	                     &EconomySim::demo_set_labor_profile);
	ClassDB::bind_method(D_METHOD("demo_set_minimum_wage", "nation", "value"),
	                     &EconomySim::demo_set_minimum_wage);
	ClassDB::bind_method(D_METHOD("demo_set_wage_rigidity", "value"),
	                     &EconomySim::demo_set_wage_rigidity);
	ClassDB::bind_method(D_METHOD("demo_set_goods", "goods"),
	                     &EconomySim::demo_set_goods);
	ClassDB::bind_method(D_METHOD("demo_set_recipes", "recipes"),
	                     &EconomySim::demo_set_recipes);
	ClassDB::bind_method(D_METHOD("demo_set_needs", "needs"),
	                     &EconomySim::demo_set_needs);
	ClassDB::bind_method(D_METHOD("demo_set_services", "services"),
	                     &EconomySim::demo_set_services);
	ClassDB::bind_method(D_METHOD("demo_set_sector_employment", "data"),
	                     &EconomySim::demo_set_sector_employment);
	ClassDB::bind_method(D_METHOD("demo_get_service_sectors"),
	                     &EconomySim::demo_get_service_sectors);
	ClassDB::bind_method(D_METHOD("demo_get_services_detail", "city_id"),
	                     &EconomySim::demo_get_services_detail);
	ClassDB::bind_method(D_METHOD("demo_set_service_capacity", "city_id", "sector",
	                     "units_per_day"),
	                     &EconomySim::demo_set_service_capacity);
	ClassDB::bind_method(D_METHOD("demo_set_service_business_state", "business_id",
	                     "loss_days", "profit"),
	                     &EconomySim::demo_set_service_business_state);
	ClassDB::bind_method(D_METHOD("demo_get_service_business_count"),
	                     &EconomySim::demo_get_service_business_count);
	ClassDB::bind_method(D_METHOD("demo_get_service_bankruptcies"),
	                     &EconomySim::demo_get_service_bankruptcies);
	ClassDB::bind_method(D_METHOD("demo_seed_industry"),
	                     &EconomySim::demo_seed_industry);
	ClassDB::bind_method(D_METHOD("demo_invest", "city_id", "building_id", "amount"),
	                     &EconomySim::demo_invest);
	ClassDB::bind_method(D_METHOD("demo_build", "city_id", "building_id", "resource",
	                     "input_product", "output_product", "capacity", "jobs", "wage",
	                     "opex", "input_per_output", "owner", "cost", "recipe_id"),
	                     &EconomySim::demo_build, DEFVAL(""));
	ClassDB::bind_method(D_METHOD("demo_spend_government", "city_id", "amount"),
	                     &EconomySim::demo_spend_government);
	ClassDB::bind_method(D_METHOD("demo_expand_port", "port_id", "t_per_day"),
	                     &EconomySim::demo_expand_port);
	ClassDB::bind_method(D_METHOD("demo_set_port_capacity", "port_id", "t_per_day"),
	                     &EconomySim::demo_set_port_capacity);
	ClassDB::bind_method(D_METHOD("demo_to_dict"), &EconomySim::demo_to_dict);
	ClassDB::bind_method(D_METHOD("demo_from_dict", "data"), &EconomySim::demo_from_dict);
	ClassDB::bind_method(D_METHOD("demo_set_balance", "values"),
	                     &EconomySim::demo_set_balance);
	ClassDB::bind_method(D_METHOD("demo_set_fleet", "nation", "trucks", "ships",
	                     "planes", "fuel", "spare_parts"),
	                     &EconomySim::demo_set_fleet);
	ClassDB::bind_method(D_METHOD("demo_get_fleet", "nation"),
	                     &EconomySim::demo_get_fleet);
	ClassDB::bind_method(D_METHOD("demo_fleet_buy", "nation", "product", "amount",
	                     "budget_share"),
	                     &EconomySim::demo_fleet_buy, DEFVAL(0.1));
	ClassDB::bind_method(D_METHOD("demo_set_market_stock", "city_id", "product", "stock"),
	                     &EconomySim::demo_set_market_stock);
	ClassDB::bind_method(D_METHOD("demo_set_market_price", "city_id", "product", "price"),
	                     &EconomySim::demo_set_market_price);
	ClassDB::bind_method(D_METHOD("demo_set_currencies", "currencies"),
	                     &EconomySim::demo_set_currencies);
	ClassDB::bind_method(D_METHOD("demo_get_currency", "nation"),
	                     &EconomySim::demo_get_currency);
	ClassDB::bind_method(D_METHOD("demo_get_currencies"),
	                     &EconomySim::demo_get_currencies);
	ClassDB::bind_method(D_METHOD("demo_set_currency_rate", "code", "rate", "floating",
	                     "base_rate"),
	                     &EconomySim::demo_set_currency_rate, DEFVAL(true), DEFVAL(-1.0));
	ClassDB::bind_method(D_METHOD("demo_set_tariff", "buyer_nation", "seller_nation",
	                     "product", "rate"),
	                     &EconomySim::demo_set_tariff);
	ClassDB::bind_method(D_METHOD("demo_get_tariff", "buyer_nation", "seller_nation",
	                     "product"),
	                     &EconomySim::demo_get_tariff);
	ClassDB::bind_method(D_METHOD("demo_get_tariffs"),
	                     &EconomySim::demo_get_tariffs);
	ClassDB::bind_method(D_METHOD("demo_seed_groups"), &EconomySim::demo_seed_groups);
	ClassDB::bind_method(D_METHOD("demo_set_group_names", "names"),
	                     &EconomySim::demo_set_group_names);
	ClassDB::bind_method(D_METHOD("demo_get_groups"), &EconomySim::demo_get_groups);
	ClassDB::bind_method(D_METHOD("demo_get_group_ranking"),
	                     &EconomySim::demo_get_group_ranking);
	ClassDB::bind_method(D_METHOD("demo_get_group", "group_id"),
	                     &EconomySim::demo_get_group);
	ClassDB::bind_method(D_METHOD("demo_fund_group", "group_id", "amount"),
	                     &EconomySim::demo_fund_group);
	ClassDB::bind_method(D_METHOD("demo_group_invest", "group_id", "city_id",
	                     "building_id", "resource", "output_product", "capacity",
	                     "recipe_id", "partner_group"),
	                     &EconomySim::demo_group_invest, DEFVAL(""));
	ClassDB::bind_method(D_METHOD("demo_set_fdi_policy", "nation", "sector", "mode",
	                     "min_local"),
	                     &EconomySim::demo_set_fdi_policy, DEFVAL(0.0));
	ClassDB::bind_method(D_METHOD("demo_set_sector_incentive", "nation", "sector",
	                     "tax_holiday", "subsidy"),
	                     &EconomySim::demo_set_sector_incentive);
	ClassDB::bind_method(D_METHOD("demo_create_state_company", "nation", "name",
	                     "capital"),
	                     &EconomySim::demo_create_state_company);
	ClassDB::bind_method(D_METHOD("demo_privatize", "city_id", "building_id",
	                     "buyer_group"),
	                     &EconomySim::demo_privatize);
	ClassDB::bind_method(D_METHOD("demo_nationalize", "city_id", "building_id"),
	                     &EconomySim::demo_nationalize);
	ClassDB::bind_method(D_METHOD("demo_get_investment_status"),
	                     &EconomySim::demo_get_investment_status);
	ClassDB::bind_method(D_METHOD("demo_get_investment_signals"),
	                     &EconomySim::demo_get_investment_signals);
	ClassDB::bind_method(D_METHOD("demo_groups_seeded"),
	                     &EconomySim::demo_groups_seeded);
}

const EconomySim::ModeSpec& EconomySim::spec(TransportMode mode) {
	// capacidade de lote = carga útil de uma composição/frota padrão.
	// custo em unidades monetárias por tonelada-quilômetro.
	// Calibragem do porto (port_design.md §3): um caminhão leva 25 t, um navio
	// 2.000 t (80× mais) a 1/5 do custo por t·km; o avião é a ponte de
	// emergência, com lote pequeno.
	// F4: além da tarifa, o frete carrega o combustível físico por t·km e o
	// uso da frota (depreciação/crew), na ordem física: caminhão 0,00004 t/t·km
	// (25 t, retorno vazio), navio 0,000003, avião 0,0002. O uso da frota é
	// ~2–5% da tarifa e existe para o veículo ser um custo real, não só um
	// contador de capacidade. A soma (combustível + frota) fica em 3–6% do
	// frete: o bastante para o preço refletir o veículo, sem deslocar a
	// segurança alimentar dos importadores (medido em economy_world_test).
	// Tempo de calendário (2026-11): caminhão a 62 km/h mas só 10 h/dia de
	// jornada (~600 km/dia com o desvio de estrada); navio 33 km/h 24 h
	// (~790 km/dia); avião 800 km/h com ~3 h de solo. Sem a jornada do
	// caminhão, um trecho de 700 km "chegava" em 11 h e o veículo parecia um
	// trem no mapa.
	static const ModeSpec kTruck{
		TransportMode::Truck, "truck", 62.0, 25.0, 0.10, 6.0, 0.00004, 0.002,
		10.0, 1.35};
	static const ModeSpec kShip{
		TransportMode::Ship, "ship", 33.0, 2000.0, 0.02, 24.0, 0.000003, 0.001,
		24.0, 1.15};
	static const ModeSpec kPlane{
		TransportMode::Plane, "plane", 800.0, 20.0, 0.80, 3.0, 0.0002, 0.015,
		24.0, 1.0};

	switch (mode) {
		case TransportMode::Ship:
			return kShip;
		case TransportMode::Plane:
			return kPlane;
		case TransportMode::Truck:
		default:
			return kTruck;
	}
}

const char* EconomySim::scope_name(MarketScope scope) {
	switch (scope) {
		case MarketScope::National:
			return "nacional";
		case MarketScope::International:
			return "internacional";
		case MarketScope::Regional:
		default:
			return "regional";
	}
}

EconomySim::Leg EconomySim::make_leg(TransportMode mode, const std::string& from, const std::string& to,
                                     const MapPoint& from_point, const MapPoint& to_point,
                                     double load_tons) const {
	const ModeSpec& mode_spec = spec(mode);
	const double distance = distance_km(from_point, to_point);

	// Viagens necessárias para escoar o lote (cada lote = um veículo). A frota
	// parte em comboio: o prazo é o do veículo mais o embarque dos lotes, não
	// `tempo × nº de veículos`. O transbordo soma a sobrecarga de carga/descarga.
	const int trips = std::max(1, static_cast<int>(std::ceil(load_tons / mode_spec.batch_capacity_tons)));
	// Velocidade de calendário: a velocidade de cruzeiro vale só durante a
	// jornada diária (caminhão 10 h; navio/avião 24 h). O `route_factor` cobre
	// o desvio da rota real em relação à linha reta.
	const double effective_speed =
		mode_spec.speed_kmh * mode_spec.duty_hours_per_day / kHoursPerDay;
	const double routed_distance = distance * mode_spec.route_factor;
	const double hours = routed_distance / effective_speed + mode_spec.turnaround_hours
	                     + static_cast<double>(trips) * kDemoLoadHoursPerBatch;

	Leg leg;
	leg.mode = mode;
	leg.from = from;
	leg.to = to;
	leg.from_point = from_point;
	leg.to_point = to_point;
	leg.distance_km = distance;
	leg.load_tons = load_tons;
	leg.trips = trips;
	leg.transit_hours = hours;
	leg.cost = load_tons * distance * mode_spec.cost_per_ton_km;
	return leg;
}

double EconomySim::distance_km(const MapPoint& a, const MapPoint& b) const {
	const double lat1 = radians(to_lat(a.y));
	const double lat2 = radians(to_lat(b.y));
	const double dlat = radians(to_lat(b.y) - to_lat(a.y));

	// Mundo cilíndrico: a longitude dá a volta em ±180°, então a rota sempre usa
	// o caminho mais curto (ex.: Havaí -> Japão cruza o Pacífico, não o Atlântico).
	double dlon_degrees = to_lon(b.x) - to_lon(a.x);
	if (dlon_degrees > 180.0) {
		dlon_degrees -= 360.0;
	} else if (dlon_degrees < -180.0) {
		dlon_degrees += 360.0;
	}
	const double dlon = radians(dlon_degrees);

	const double s1 = std::sin(dlat / 2.0);
	const double s2 = std::sin(dlon / 2.0);
	const double h = s1 * s1 + std::cos(lat1) * std::cos(lat2) * s2 * s2;
	return 2.0 * kEarthRadiusKm * std::asin(std::sqrt(h));
}

void EconomySim::demo_fill_leg_status(Dictionary& dict, const Leg& leg) const {
	const ModeSpec& mode_spec = spec(leg.mode);
	const DemoStatusKeys& k = demo_status_keys_;
	dict[k.mode] = String(mode_spec.name);
	dict[k.from] = String(leg.from.c_str());
	dict[k.to] = String(leg.to.c_str());
	dict[k.from_px] = Vector2(leg.from_point.x, leg.from_point.y);
	dict[k.to_px] = Vector2(leg.to_point.x, leg.to_point.y);
	dict[k.distance_km] = leg.distance_km;
	dict[k.load_tons] = leg.load_tons;
	dict[k.trips] = leg.trips;
	dict[k.transit_hours] = leg.transit_hours;
	dict[k.cost] = leg.cost;
	dict[k.speed_kmh] = mode_spec.speed_kmh;
	dict[k.batch_capacity_tons] = mode_spec.batch_capacity_tons;
}

// Cenário de validação: duas cidades, POPs agregados, trigo consumido direto.
// ---------------------------------------------------------------------------

namespace {

constexpr double kDemoNeedPerCapita = 0.0003;
// F3: reserva de alimento (dias) que a POP mantém em poupança antes de gastar
// em life/luxury. Sem ela, a compra de conforto drenava a poupança usada para
// importar comida em nações sem terra (EGY/SAH em 365 d). É uma regra
// prudencial declarada, não um número observado: o colchão cobre uma década de
// cesta, então o conforto é pago essencialmente pela renda corrente, não pela
// poupança que financia a importação de alimento (EGY tinha poupança acima de
// um ano e ainda perdia comida em 365 d).
constexpr double kDemoFoodReserveDays = 3650.0;
// F7: margem de poupança do gate de renda → consumo. A cidade só declara o
// nível de consumo life/luxury quando a renda corrente cobre a cesta acumulada
// dos níveis com esta folga. Regra prudencial declarada (não é dado observado):
// a cesta de `needs.json` cobre os bens comerciáveis do modelo; moradia,
// energia, transporte urbano e serviços não têm preço próprio no núcleo (o
// setor de serviços é um salário médio, não uma cesta). A folga representa esse
// custo de vida não modelado — sem ela, qualquer renda acima da cesta de bens
// liberava luxury em todo o mundo (F7: 832/834 cidades no nível 3). O valor
// mora em demo_income_basket_margin_ (calibrado no economy_world_test; ver
// balance_review §23).
// F3: teto do gasto institucional (paper/computer) como fração do valor da
// cesta alimentar diária da cidade. A compra do Estado não pode competir com a
// importação de comida (EGY caía a 0,708 em 365 d quando o computer drenava o
// orçamento). Estimativa declarada, calibrável no laboratório.
constexpr double kDemoGovernmentGoodsFoodShare = 0.05;
// Renda externa das famílias: serviços urbanos, pesca e turismo vendidos ao
// resto do mundo. Dimensão derivada da própria cesta de subsistência — a
// necessidade alimentar per capita (0,0003 t/dia) paga ao preço-base do trigo
// (40/t) = 0,012 por pessoa/dia. É o que uma cidade SEM terra produtiva
// (deserto, gelo, ilha de coral) precisa ganhar para importar o alimento que
// não planta; sem isso o caixa zera, a IA não investe e a nação passa fome
// mesmo com dinheiro parado no resto do mundo (§20). Sai do setor externo
// (foreign_cash), então `money_total` continua conservado.
//
// A renda NÃO é uniforme: é proporcional ao PIB PPP per capita REAL da nação
// (índice por cidade, ver demo_set_external_income_index), normalizado para que
// a média mundial ponderada por população continue nesta base.
//
// F7 calibração: acima da média mundial (índice > 1) a transferência NÃO cresce
// mais. Ela é um PISO DE SUBSISTÊNCIA — o alimento que uma cidade sem terra
// produtiva precisa importar —, não uma transferência de riqueza: escalada até
// o topo, ela sozinha pagava 3× a cesta completa das famílias dos EUA/DEU e
// levava o mundo inteiro ao nível luxury, independentemente de salário/lucro.
// A renda acima da subsistência tem de vir do valor adicionado (salário +
// lucro), que é medido. Países pobres continuam abaixo de 1,0 e mantêm o piso
// proporcional ao PIB real.
constexpr double kDemoExternalIncomePerCapita = 0.012;
constexpr double kDemoExternalIncomeIndexCap = 1.0;
// F7 calibração — ANCORAGEM DA DISTRIBUIÇÃO DE RENDA (head, F7 final).
//
// A renda média por pessoa de cada nação é ancorada no PIB PPP per capita real
// (índice normalizado, média mundial = 1,0): a média da distribuição é
// `kIncomeAnchorWorldMean` cestas básicas × índice. A cesta básica (`basic`)
// custa 1×, a `life` acumulada deve custar 2,5–3× e a `luxury` acumulada 6–10×
// (limiares de classe no núcleo, multiplicadores abaixo). O sigma da lognormal
// vem do Gini da nação (GeographyService; `estimated` quando regional).
//
// Calibração verificada em 60 d com IA (economy_income_distribution_test):
// EUA/DEU basic ≤ 15% e maioria life; BRA basic ~41%; IND ~68%; NGA ~78%.
constexpr double kIncomeAnchorWorldMean = 2.0;   // cestas básicas/pessoa/dia
constexpr double kIncomeLuxuryMultiple = 7.0;    // luxo = 7× cesta básica
constexpr double kDemoLaborShare = 0.55;
constexpr double kDemoLaborShareLow = 0.50;
constexpr double kDemoLaborShareHigh = 0.65;
constexpr double kDemoServiceShare = 0.30;
constexpr double kDemoServiceWage = 0.15;
// F8 v2 — ÂNCORA DE PREÇO DE SERVIÇO LIGADA AO SALÁRIO DO PAÍS. O preço-âncora
// por setor (`definition.price`) escala com o salário de serviço local relativo
// ao salário-âncora da F7 (0,15 XGS/posto/dia): price/jobs_per_unit ≈ 0,22 na
// âncora, então a receita por posto passa a ser ≈ 1,47× o salário LOCAL — a
// margem bruta (~32% antes de insumos) fica estável em país rico e pobre, sem o
// teto `preço÷jobs_per_unit×0,68` que mascarava o preço sub-calibrado (Q-004).
// Os limites evitam preço zero/explosivo quando o salário foge da faixa típica.
// O piso de 0,10 cobre salários de subsistência (~0,015 XGS/posto/dia) sem
// travar a escala em país muito pobre — era o piso de 0,50 que deixava a
// margem de nações pobres disparar (a receita por posto ficava 3× acima do
// salário real).
constexpr double kDemoServiceAnchorScaleMin = 0.10;
constexpr double kDemoServiceAnchorScaleMax = 4.0;
// F8 v2 — MARGEM-ALVO: reduz a receita por posto de ~1,47× para ~1,25× o
// salário local (margem bruta ~20% antes de insumos). Combinado com a banda
// apertada [0,70; 1,40], a margem líquida mediana cai para ~5–20% conforme o
// setor (varejo baixo, finanças/TI mais alto), sem o prejuízo estrutural do
// preço sub-calibrado que o teto 0,68 mascarava (Q-004).
constexpr double kDemoServiceMarginCalibration = 0.85;
constexpr double kDemoServiceSpendShare = 0.45;
constexpr double kDemoTaxRate = 0.15;
constexpr double kDemoTransferShare = 1.0;
constexpr double kDemoCapitalDistribution = 1.0;
// F9 — investimento e propriedade.
// Imposto corporativo sobre o lucro do ativo do grupo, pago ao orçamento do
// anfitrião. Retenção: fração do lucro líquido que fica no caixa do grupo
// (investimento seguinte); o resto é dividendo ao estrato dono no fim do dia.
constexpr double kDemoCorporateTaxRate = 0.10;
constexpr double kDemoGroupDividendShare = 0.80;
// Lote de ativos por grupo na semeadura (nação × setor). Mantém "algumas
// centenas" de grupos no mundo real em vez de um grupo por cidade.
constexpr int kDemoGroupChunkAssets = 12;
// Nacionalização: confiança cai a 35% e só se recupera depois de 180 dias.
constexpr int kDemoNationalizationDays = 180;
constexpr double kDemoNationalizationConfidence = 0.35;
constexpr double kDemoNationalizationCompensation = 0.5;
constexpr double kDemoFreightPerTonKm = 0.10;
// Frete marítimo (entre massas de terra), espelhando a taxa de navio do
// mercado legado: 5x mais barato que o caminhão por tonelada-km.
constexpr double kDemoFreightShipPerTonKm = 0.02;

// ---------------------------------------------------------------------------
// F4 — frota e combustível.
//
// A frota inicial é DERIVADA de população/PIB (não é um número mágico):
// caminhão por 12,5 mil habitantes de simulação, navio por 250 mil, avião por
// 2,5 milhões, multiplicados por um fator de renda (PIB PPP) de 0,6 a 2,0.
// Cobertura calibrada no cenário real para ~6× o uso diário medido (~500
// pedidos/dia), de modo que a frota NÃO seja um gargalo no mundo baseline: ela
// existe, ocupa veículos e consome combustível, e passa a limitar quando há
// choque/expansão de comércio (a fila aciona a reposição do governo).
// Calibração F4 (economy_world_test --ai=1): a frota derivada original (1/2
// desta) fazia EGY cair de 1,000 (F3, 60 d) para 0,846 — os pequenos
// exportadores marítimos ficavam presos na coleta do cais e a comida estagnava
// em quem já estava abastecido. Com 2× e os pisos abaixo, 60 d e 365 d com IA
// ficam dentro do baseline F3 (365 d: 0 nações pioradas, 26 falências = base).
// Estoque estratégico inicial de combustível: 400 dias de uso estimado pela
// frota derivada (reserva declarada, `source: derived`). A reserva é o
// endowment de transição da F4: a reposição mensal do governo compra fuel no
// mercado interno (a compra entra em `desired` mesmo sem estoque) e a produção
// das fábricas de fuel da F2 substitui a reserva com o tempo. O teto global de
// 512 embarques/dia limita a variação do consumo, então 400 dias cobrem 1 ano
// de operação no cenário real sem transformar combustível em gargalo imediato —
// o portão "sem fuel não embarca" é real e testado em cenário controlado.
//
// Pisos de 16 caminhões / 4 navios: o piso antigo (8/2) deixava nações
// pequenas sem frota para a coleta no cais; com 2× na razão per capita o
// mundo fica dentro do baseline F3 em 365 d (0 nações pioradas, 26 falências =
// base) e o resíduo de 60 d é 0,021 em EGY — oscilação de chegada de comida,
// não regressão de balanço (ver GAME_STATE §10).
constexpr double kDemoFleetTrucksPerCapita = 1.0 / 12500.0;
constexpr double kDemoFleetShipsPerCapita = 1.0 / 250000.0;
constexpr double kDemoFleetPlanesPerCapita = 1.0 / 2500000.0;
constexpr double kDemoFleetMinTrucks = 16.0;
constexpr double kDemoFleetMinShips = 4.0;
constexpr double kDemoFleetFuelDays = 400.0;
// Reserva estratégica mínima por habitante de simulação (t/capita). Ordem de
// grandeza de um estoque nacional de combustível (não é medição): cobre a
// expansão industrial que a IA cria em 60–365 d, quando o volume de carga
// cresce ~7× sobre a estimativa da frota. O piso de 400 dias da frota continua
// valendo para nações pequenas.
constexpr double kDemoFleetFuelPerCapita = 0.01;
// Consumo diário estimado por veículo (t/dia), usado para dimensionar o
// estoque-alvo de combustível. Derivado do uso medido (~0,09 t/caminhão/dia;
// navio e avião têm tanques/passagens maiores).
constexpr double kDemoFleetFuelPerTruckDay = 0.12;
constexpr double kDemoFleetFuelPerShipDay = 0.80;
constexpr double kDemoFleetFuelPerPlaneDay = 1.50;
// Manutenção mensal: peças por veículo/mês (t). Um caminhão ~20 kg de
// componentes por mês; navio/avião uma ordem de grandeza acima.
constexpr double kDemoFleetPartsPerTruckMonth = 0.02;
constexpr double kDemoFleetPartsPerShipMonth = 0.40;
constexpr double kDemoFleetPartsPerPlaneMonth = 0.15;
// Reserva inicial de peças, em meses de manutenção. Transição da F4: o mundo
// começa sem fábricas de `spare_parts`; 18 meses cobrem o horizonte de 365 d
// com folga (como os 400 dias de fuel) e evitam que a saúde da frota caia ao
// piso de 0,5 no meio do ano — o que recriava o gargalo de comida da F3.
// A reposição mensal continua mirando 3 meses, não a reserva cheia.
constexpr double kDemoFleetPartsReserveMonths = 18.0;
constexpr double kDemoFleetPartsTargetMonths = 3.0;
// Atrito mensal (acidentes/obsolescência) e saúde da frota.
constexpr double kDemoFleetAttritionMonth = 0.002;
constexpr double kDemoFleetHealthFloor = 0.50;
constexpr double kDemoFleetHealthLossMonth = 0.10;
constexpr double kDemoFleetHealthRecoveryMonth = 0.05;
// Fração do orçamento da cidade-sede que o governo pode gastar por compra.
constexpr double kDemoFleetBudgetShare = 0.10;
// Cobertura de comida (dias de necessidade) exigida para o governo investir
// em frota. A comida tem prioridade no orçamento público: sem esta folga, o
// procurement mensal competia com a importação de alimento (AFG/LBY caíam em
// 365 d de IA). Abaixo do limiar, só há compra de emergência de fuel/peças.
constexpr double kDemoFleetFoodSecureDays = 3.0;

// ---------------------------------------------------------------------------
// F5 — câmbio, tarifa e preço entregue.
//
// `rate` = valor em XGS de 1 unidade local. O preço de mercado fica na ÂNCORA
// XGS (fonte de verdade do núcleo e da IA); a moeda local é só EXIBIÇÃO
// (`price_local = price_anchor ÷ taxa`, calculado ao publicar o status) e nunca
// volta para a simulação. Isso remove o round-trip `(base ÷ taxa) × taxa` que
// desviava 1 ulp nas decisões discretas (§15, F5).
//
// Drift diário máximo de 0,05% e banda de ±10% em torno da taxa inicial. A
// regra é conservadora de propósito: a calibração da F4 é sensível à chegada
// de comida e uma desvalorização forte realimentaria a fome. A balança é
// suavizada em 30 dias (EMA) e normalizada pelo volume do comércio; déficit
// persistente desvaloriza, superávit valoriza (§4.4/§5.6).
constexpr double kDemoFxDailyStep = 0.0005;
constexpr double kDemoFxBandLow = 0.90;
constexpr double kDemoFxBandHigh = 1.10;
constexpr double kDemoFxBalanceEmaDays = 30.0;
constexpr double kDemoFxBalanceScale = 0.10;

// ---------------------------------------------------------------------------
// Portos (ver docs/economy/port_design.md).
//
// Capacidade de nível 1 pela população de simulação da cidade-sede: piso de
// 1.000 t/dia (~0,4 Mt/ano, porto pequeno) e teto de 50.000 t/dia (~18 Mt/ano,
// calado de metrópole). O nível multiplica a base (1..N).
// ---------------------------------------------------------------------------
constexpr double kDemoPortBaseTons = 1000.0;
constexpr double kDemoPortTonsPerCapita = 0.002;  // 2 kg/habitante/dia
constexpr double kDemoPortMaxTons = 50000.0;
// Capital público por t/dia de capacidade nova e prazo da obra.
constexpr double kDemoPortCapitalPerTonCapacity = 500.0;
constexpr int kDemoPortBuildDays = 30;
// Tempo de transbordo (carga/descarga) por operação no cais.
constexpr double kDemoPortHandlingHours = 8.0;
// Fila: custo de demurrage por tonelada e por dia de atraso fracionário.
constexpr double kDemoPortDemurragePerTonDay = 0.5;
constexpr double kDemoPriceSmoothing = 0.20;

// Produção segue as vendas do dia anterior, com pequena folga de estoque.
constexpr double kDemoProductionBuffer = 1.10;
// Estoque-alvo em dias de venda. A produção repõe metade da diferença por dia:
// sem estoque-alvo, produzir sempre 10% acima das vendas faria o estoque
// crescer para sempre (minas e fazendas com demanda externa fixa).
// Prejuízo persistente reduz a produção; lucro com demanda alta pode expandir.
constexpr int kDemoLossDaysBeforeCut = 10;
constexpr double kDemoLossCut = 0.95;
constexpr double kDemoMinOutputScale = 0.20;
// Dias seguidos de prejuízo até a firma fechar a construção (falência). Depois
// de kDemoLossDaysBeforeCut a produção já foi cortada ao piso; se o prejuízo
// persistir até aqui, a firma encerra a atividade em vez de ficar no vermelho
// para sempre.
constexpr int kDemoLossDaysBeforeClose = 60;
// Capital de giro: as empresas retêm caixa suficiente para importar e pagar
// salários antes de distribuir lucros.
constexpr double kDemoFirmCashPerCapita = 0.10;
constexpr double kDemoServiceCashPerCapita = 0.05;
// F8: caixa do setor de serviços = capital de giro em dias de folha + insumos.
// O excedente do lucro é renda da POP dona (ver demo_tick / distribuição).
constexpr double kDemoServiceCashBufferDays = 8.0;
// F8: a fração de serviços respeita a cesta do dia — em cidade que importa
// comida o serviço não pode consumir o que compra o alimento. 0 desliga o
// guarda (comportamento F7: 45% da renda doméstica, cesta depois).
constexpr double kDemoServiceFoodGuardDays = 1.0;
// Serviços ajustam o emprego para cobrir a folha com a própria receita.
constexpr double kDemoServiceAdjust = 0.10;
// Limites usados pelo benchmark sintético (o cenário demo usa o equilíbrio
// do mercado de trabalho residual).
constexpr double kDemoServiceMinShare = 0.05;
constexpr double kDemoServiceMaxShare = 0.30;
// O salário de serviços busca o equilíbrio do mercado de trabalho residual,
// com piso no custo de subsistência (uma cesta alimentar por pessoa). Sem
// isso o salário fixo de 0,15/dia (3x o PIB per capita) só financiava ~10%
// da população e o desemprego aparecia em 80-90% por construção.
// Desemprego friccional: parte da força de trabalho residual está sempre
// procurando vaga. Sem isso o mercado de trabalho perfeitamente flexível
// produziria 0,0% de desemprego, o que também não é plausível.
// Demanda externa de ferro (fora do consumo alimentar): o ferro flui pelo
// mesmo mecanismo de teto data-driven dos demais bens brutos (fase 4d,
// external_demand declarado pelo GeographyService), não por um caso especial.
// Primeiro produto "bruto" (sem consumo local) na ordem de goods.json: 1=ferro.
// A fase 4d pula os alimentos da cesta (B3) via demo_food_calories, então o
// valor aqui só delimita onde começa a venda externa de matérias-primas
// (minerais, algodão, madeira).
constexpr int kDemoFirstRawProduct = 1;

// Extração mineral. 1 kt = 1000 t.
constexpr double kDemoTonsPerKiloton = 1000.0;
// Qualidade -> eficiência da extração: a produção efetiva é multiplicada por
// piso + (1 - piso) × qualidade (minério pobre rende menos por dia de operação).
constexpr double kDemoQualityEfficiencyFloor = 0.5;
// Profundidade -> custo: opex × (1 + profundidade / escala), com teto.
constexpr double kDemoDepthOpexScaleM = 2000.0;
constexpr double kDemoDepthOpexMaxFactor = 3.0;


// Aptidão de referência do território de AGRIA (trigo, dado real de
// agriculture.json). Mantém a produção validada de 20 t/dia e serve de valor
// inicial até o GeographyService carregar o dado em tempo de execução.
constexpr double kDemoReferenceWheatSuitability = 0.702;
constexpr double kDemoFarmCapacityTons = 20.0;
// Capital necessário para adicionar 1 t/dia de capacidade produtiva.
constexpr double kDemoCapitalPerCapacity = 2000.0;

// Inicialização do mundo real (initialize_demo_world). Valores derivados do
// cenário de validação AGRIA/PORTUS (fazenda de 20 t/dia, 150 empregos,
// salário 2.0 e opex 50), escalados para a população/construções reais.
constexpr double kDemoSavingsPerCapita = 0.50;
constexpr double kDemoGovernmentPerCapita = 1.0;
constexpr double kDemoJobsPerTon = 7.5;   // 150 empregos / 20 t
constexpr double kDemoFarmWage = 2.0;     // igual à fazenda demo
constexpr double kDemoOpexPerTon = 2.5;   // 50 opex / 20 t
// Fração da necessidade diária mantida como estoque antes de exportar. O
// vendedor já consumiu a própria necessidade na fase 5; a produção da fase 4
// repõe no dia seguinte. Buffer alto aqui espreme as cidades pequenas (a oferta
// vendável fica menor que a demanda e a última compradora — ex.: Cayenne — não
// recebe nada).
constexpr double kDemoTradeBufferDays = 0.0;
// Teto de cargas de comércio por dia (proteção contra loop). Com 40 cidades,
// 64 bastava; com 252 e 66 nações, 64 embarques/dia deixavam as cidades de
// índice alto (SAU, CHN...) permanentemente fora da fila — todas as
// compradoras com estoque zero empatam em cobertura relativa 3,0 e as 64
// primeiras do índice consumiam o teto todos os dias (§19). 512 cobre o pico
// de reabastecimento mundial e cai sozinho quando os estoques enchem.
constexpr std::size_t kDemoMaxShipmentsPerDay = 512;
// Capacidade de transporte diária de uma cidade, por habitante (t/dia). 1 kg
// por habitante/dia (~3x a necessidade alimentar de 0,3 kg) — um teto físico
// para o comércio não mover carga "infinita" num único dia, sem apertar o fluxo
// normal de comida.
constexpr double kDemoTradeCapacityPerCapita = 0.001;

	constexpr std::size_t kDemoTradeExhaustiveLimit = 24;
	// Célula do índice espacial do comércio, em unidades do mapa (~8 km cada):
	// 32 unidades ≈ 250 km. O comprador varre a grade em anéis até juntar K
	// vendedores, então a célula só afeta a constante do custo de busca.
	constexpr double kDemoTradeCellMapUnits = 32.0;

// Ordem idêntica à de data/definitions/economy/goods.json (posições 0..20).
// A lista padrão vive no construtor (EconomySim::EconomySim).

// ---------------------------------------------------------------------------
// Migração de índice de produto (save v1/v2 -> v3).
//
// Saves antigos tinham farinha=1 e pão=2 (entre trigo e ferro). Ao removê-los,
// o enum deslocou -2 a partir do ferro (iron 3->1, rice 4->2, ...). A migração
// reverte esse deslocamento: trigo (0) fica, farinha/pão (1/2) são descartados
// e os demais (>=3) deslocam -2.
// ---------------------------------------------------------------------------

// Índice antigo -> novo. Retorna -1 para farinha/pão (produto removido).
static int demo_migrate_product_index(int old_index) {
	if (old_index < 0) {
		return -1;  // sem produto/insumo
	}
	if (old_index == 0) {
		return 0;  // trigo inalterado
	}
	if (old_index == 1 || old_index == 2) {
		return -1;  // farinha/pão removidos
	}
	return old_index - 2;
}

// Remapeia um array por-produto do layout antigo (com farinha/pão) para o
// atual (products posições). new_index 0 <- old_index 0; new_index >= 1 <-
// old_index (new_index + 2). Posições sem correspondência ficam 0.0.
static Array demo_migrate_product_array(const Array& old, int products) {
	Array out;
	out.resize(products);
	for (int new_index = 0; new_index < products; ++new_index) {
		const int old_index = (new_index == 0) ? 0 : (new_index + 2);
		if (old_index >= 0 && old_index < old.size()) {
			out[new_index] = old[old_index];
		} else {
			out[new_index] = 0.0;
		}
	}
	return out;
}

// Remapeia um array por-produto usando o vetor old_index -> new_index (-1
// descarta o produto). Posições sem correspondência ficam 0.0.
static Array demo_remap_product_array(const Array& old,
		const std::vector<int>& remap, int products) {
	Array out;
	out.resize(products);
	for (int index = 0; index < products; ++index) {
		out[index] = 0.0;
	}
	const int limit = static_cast<int>(
		std::min<std::size_t>(static_cast<std::size_t>(old.size()), remap.size()));
	for (int old_index = 0; old_index < limit; ++old_index) {
		const int new_index = remap[static_cast<std::size_t>(old_index)];
		if (new_index >= 0 && new_index < products) {
			out[new_index] = old[old_index];
		}
	}
	return out;
}

} // namespace

// Eficiência da extração (0.5..1). Sem reserva declarada não há qualidade
// conhecida: eficiência 1 (comportamento legado).
double EconomySim::demo_extraction_efficiency(const DemoDeposit& deposit) {
	if (!deposit.tracked) {
		return 1.0;
	}
	return kDemoQualityEfficiencyFloor
		+ (1.0 - kDemoQualityEfficiencyFloor) * std::clamp(deposit.quality, 0.0, 1.0);
}

// Multiplicador do opex pela profundidade da jazida (1.0 sem reserva).
double EconomySim::demo_depth_opex_factor(const DemoDeposit& deposit) {
	if (!deposit.tracked) {
		return 1.0;
	}
	return std::min(kDemoDepthOpexMaxFactor,
		1.0 + std::max(0.0, deposit.depth_m) / kDemoDepthOpexScaleM);
}

double EconomySim::demo_base_price(int product) const {
	if (product >= 0 && product < demo_products_) {
		return demo_base_prices_[product];
	}
	return 40.0;
}

const EconomySim::DemoCurrency* EconomySim::demo_currency_for(
		const std::string& nation) const {
	const auto found = demo_nation_currency_.find(nation);
	if (found == demo_nation_currency_.end()
			|| found->second < 0
			|| found->second >= static_cast<int>(demo_currencies_.size())) {
		return nullptr;
	}
	return &demo_currencies_[static_cast<std::size_t>(found->second)];
}

EconomySim::DemoCurrency* EconomySim::demo_currency_for(
		const std::string& nation) {
	const auto found = demo_nation_currency_.find(nation);
	if (found == demo_nation_currency_.end()
			|| found->second < 0
			|| found->second >= static_cast<int>(demo_currencies_.size())) {
		return nullptr;
	}
	return &demo_currencies_[static_cast<std::size_t>(found->second)];
}

double EconomySim::demo_fx_rate(const std::string& nation) const {
	const DemoCurrency* currency = demo_currency_for(nation);
	if (currency == nullptr || !std::isfinite(currency->rate)
			|| currency->rate <= 0.0) {
		return 1.0;
	}
	return currency->rate;
}

const char* EconomySim::demo_currency_code(const std::string& nation) const {
	const DemoCurrency* currency = demo_currency_for(nation);
	return currency != nullptr ? currency->code.c_str() : "XGS";
}

double EconomySim::demo_competitiveness(const std::string& nation) const {
	// Fator de competitividade aplicado à âncora (F5, opção c). Com a taxa
	// FIXA (peg) o fator é 1: o valor interno não muda. Com regime float, a
	// taxa pode desviar ±10% da referência; o fator `taxa ÷ taxa-base`
	// reproduz o efeito de preço rígido na moeda local (desvalorização barateia
	// a oferta externa do país sem reescrever o preço interno), documentado em
	// GAME_STATE §15/D-027.
	const DemoCurrency* currency = demo_currency_for(nation);
	if (currency == nullptr || !std::isfinite(currency->rate)
			|| !std::isfinite(currency->base_rate)
			|| currency->rate <= 0.0 || currency->base_rate <= 0.0) {
		return 1.0;
	}
	if (!currency->floating) {
		return 1.0;
	}
	return std::clamp(currency->rate / currency->base_rate,
		kDemoFxBandLow, kDemoFxBandHigh);
}

void EconomySim::demo_rebuild_currency_indexes() {
	demo_currency_by_code_.clear();
	demo_nation_currency_.clear();
	demo_currency_by_code_.reserve(demo_currencies_.size() * 2);
	for (std::size_t index = 0; index < demo_currencies_.size(); ++index) {
		DemoCurrency& currency = demo_currencies_[index];
		if (currency.code.empty()) {
			continue;
		}
		demo_currency_by_code_[currency.code] = static_cast<int>(index);
		for (const std::string& nation : currency.nations) {
			if (!nation.empty()) {
				demo_nation_currency_[nation] = static_cast<int>(index);
			}
		}
	}
}

void EconomySim::demo_rebuild_tariff_lookup() {
	demo_tariff_lookup_.clear();
	demo_tariff_lookup_.reserve(demo_tariffs_.size() * 2);
	for (const DemoTariff& tariff : demo_tariffs_) {
		demo_tariff_lookup_[tariff.buyer + '|' + tariff.seller + '|'
			+ tariff.product] = std::clamp(tariff.rate, 0.0, 1.0);
	}
}

double EconomySim::demo_tariff_rate(const std::string& buyer_nation,
		const std::string& seller_nation, int product) const {
	if (demo_tariff_lookup_.empty() || product < 0 || product >= demo_products_) {
		return 0.0;
	}
	const std::string good = demo_product_name(product);
	const std::string keys[4] = {
		buyer_nation + '|' + seller_nation + '|' + good,
		buyer_nation + "|*|" + good,
		buyer_nation + '|' + seller_nation + "|*",
		buyer_nation + "|*|*",
	};
	for (const std::string& key : keys) {
		const auto found = demo_tariff_lookup_.find(key);
		if (found != demo_tariff_lookup_.end()) {
			return std::clamp(found->second, 0.0, 1.0);
		}
	}
	return 0.0;
}

Array EconomySim::demo_get_tariffs() const {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	Array out;
	out.resize(static_cast<int>(demo_tariffs_.size()));
	for (std::size_t index = 0; index < demo_tariffs_.size(); ++index) {
		const DemoTariff& tariff = demo_tariffs_[index];
		Dictionary entry;
		entry["buyer"] = String(tariff.buyer.c_str());
		entry["seller"] = String(tariff.seller.c_str());
		entry["product"] = String(tariff.product.c_str());
		entry["rate"] = tariff.rate;
		out[static_cast<int>(index)] = entry;
	}
	return out;
}

double EconomySim::demo_get_tariff(const String& buyer_nation,
		const String& seller_nation, const String& product) const {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	const int index = product == "*" ? -1 : demo_product_index(product);
	if (product != "*" && index < 0) {
		return 0.0;
	}
	if (index < 0) {
		const std::string buyer(buyer_nation.utf8().get_data());
		const std::string seller(seller_nation.utf8().get_data());
		const std::string keys[2] = {
			buyer + '|' + seller + "|*",
			buyer + "|*|*",
		};
		for (const std::string& key : keys) {
			const auto found = demo_tariff_lookup_.find(key);
			if (found != demo_tariff_lookup_.end()) {
				return std::clamp(found->second, 0.0, 1.0);
			}
		}
		return 0.0;
	}
	return demo_tariff_rate(std::string(buyer_nation.utf8().get_data()),
		std::string(seller_nation.utf8().get_data()), index);
}

void EconomySim::demo_set_tariff(const String& buyer_nation,
		const String& seller_nation, const String& product, double rate) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	DemoTariff tariff;
	tariff.buyer = std::string(buyer_nation.utf8().get_data());
	tariff.seller = seller_nation.is_empty() ? "*"
		: std::string(seller_nation.utf8().get_data());
	tariff.product = product.is_empty() ? "*"
		: std::string(product.utf8().get_data());
	tariff.rate = std::clamp(std::isfinite(rate) ? rate : 0.0, 0.0, 1.0);
	const std::string key = tariff.buyer + '|' + tariff.seller + '|'
		+ tariff.product;
	for (DemoTariff& existing : demo_tariffs_) {
		if (existing.buyer == tariff.buyer && existing.seller == tariff.seller
				&& existing.product == tariff.product) {
			existing.rate = tariff.rate;
			demo_tariff_lookup_[key] = tariff.rate;
			demo_status_invalidate();
			return;
		}
	}
	demo_tariffs_.push_back(tariff);
	demo_tariff_lookup_[key] = tariff.rate;
	demo_status_invalidate();
}

Dictionary EconomySim::demo_get_currency(const String& nation) const {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	Dictionary out;
	const std::string tag(nation.utf8().get_data());
	const DemoCurrency* currency = demo_currency_for(tag);
	if (currency == nullptr) {
		out["nation"] = nation;
		out["code"] = "XGS";
		out["rate"] = 1.0;
		out["base_rate"] = 1.0;
		out["source"] = "default:anchor";
		out["regime"] = "float";
		out["floating"] = true;
		out["fallback"] = false;
		out["trade_balance"] = 0.0;
		return out;
	}
	out["nation"] = nation;
	out["code"] = String(currency->code.c_str());
	out["rate"] = currency->rate;
	out["base_rate"] = currency->base_rate;
	out["source"] = String(currency->source.c_str());
	out["regime"] = currency->floating ? "float" : "peg";
	out["floating"] = currency->floating;
	out["fallback"] = currency->fallback;
	out["trade_balance"] = currency->balance_ema;
	out["gross_trade"] = currency->gross_ema;
	return out;
}

Array EconomySim::demo_get_currencies() const {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	Array out;
	out.resize(static_cast<int>(demo_currencies_.size()));
	for (std::size_t index = 0; index < demo_currencies_.size(); ++index) {
		const DemoCurrency& currency = demo_currencies_[index];
		Dictionary entry;
		entry["code"] = String(currency.code.c_str());
		entry["rate"] = currency.rate;
		entry["base_rate"] = currency.base_rate;
		entry["regime"] = currency.floating ? "float" : "peg";
		entry["fallback"] = currency.fallback;
		entry["source"] = String(currency.source.c_str());
		entry["trade_balance"] = currency.balance_ema;
		entry["gross_trade"] = currency.gross_ema;
		Array nations;
		nations.resize(static_cast<int>(currency.nations.size()));
		for (std::size_t n = 0; n < currency.nations.size(); ++n) {
			nations[static_cast<int>(n)] = String(currency.nations[n].c_str());
		}
		entry["nations"] = nations;
		out[static_cast<int>(index)] = entry;
	}
	return out;
}

void EconomySim::demo_set_currency_rate(const String& code, double rate,
		bool floating, double base_rate) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	const auto found = demo_currency_by_code_.find(std::string(code.utf8().get_data()));
	if (found == demo_currency_by_code_.end() || !std::isfinite(rate) || rate <= 0.0) {
		return;
	}
	DemoCurrency& currency = demo_currencies_[static_cast<std::size_t>(found->second)];
	currency.rate = rate;
	// `base_rate` fixa a referência da banda e da competitividade float. Sem
	// ele, a taxa informada vira a própria referência (comportamento anterior).
	currency.base_rate = (std::isfinite(base_rate) && base_rate > 0.0)
		? base_rate : rate;
	currency.floating = floating;
	demo_status_invalidate();
}

void EconomySim::demo_tick_fx() {
	if (demo_currencies_.empty() || demo_cities_.empty()) {
		return;
	}
	std::vector<double> exports(demo_currencies_.size(), 0.0);
	std::vector<double> imports(demo_currencies_.size(), 0.0);
	for (const DemoCity& city : demo_cities_) {
		const auto found = demo_nation_currency_.find(city.nation);
		if (found == demo_nation_currency_.end()) {
			continue;
		}
		const std::size_t index = static_cast<std::size_t>(found->second);
		for (int product = 0; product < demo_products_; ++product) {
			exports[index] += city.export_value[product];
			imports[index] += city.import_value[product];
		}
	}
	const double alpha = 1.0 / kDemoFxBalanceEmaDays;
	for (std::size_t index = 0; index < demo_currencies_.size(); ++index) {
		DemoCurrency& currency = demo_currencies_[index];
		const double flow = exports[index] - imports[index];
		const double gross = exports[index] + imports[index];
		currency.balance_ema += (flow - currency.balance_ema) * alpha;
		currency.gross_ema += (gross - currency.gross_ema) * alpha;
		if (!currency.floating) {
			currency.rate = currency.base_rate;
			continue;
		}
		// Pressão normalizada pelo volume: um déficit que representa 10% do
		// comércio (kDemoFxBalanceScale) satura em tanh(1) ~ 0,76. O passo
		// diário é pequeno (0,05%), então o câmbio reage à persistência da
		// balança, não a um dia isolado.
		const double scale = std::max(1.0,
			currency.gross_ema * kDemoFxBalanceScale);
		const double pressure = std::tanh(currency.balance_ema / scale);
		const double next = currency.rate * (1.0 + kDemoFxDailyStep * pressure);
		currency.rate = std::clamp(next,
			currency.base_rate * kDemoFxBandLow,
			currency.base_rate * kDemoFxBandHigh);
	}

	// O preço interno está na âncora: o passo cambial NÃO reescala preços. Só
	// a exibição (`price_local = preço ÷ taxa`) e a competitividade das
	// ofertas float (taxa ÷ taxa-base) leem a taxa corrente.
}

void EconomySim::demo_set_currencies(const Variant& currencies) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	Array list;
	// Sem `regime` declarado no dado a taxa fica FIXA (`peg`). O dado é do F1 e
	// não traz a fonte do regime cambial (currencies.json: `regime: null`), então
	// inventar flutuação violaria a regra "não invente dados" — e o drift diário
	// move preços/competitividade sem fonte. `regime: "float"` no JSON ativa o
	// drift por balança.
	std::string default_regime = "peg";
	if (currencies.get_type() == Variant::DICTIONARY) {
		const Dictionary data = currencies;
		const Variant entries = data.get("currencies", Array());
		if (entries.get_type() == Variant::ARRAY) {
			list = entries;
		}
		// `regime: null` no dado (currencies.json não tem fonte de regime) NÃO
		// é flutuação: só o valor explícito "float" ativa o drift e o fator de
		// competitividade. Qualquer outro valor mantém a taxa FIXA.
		const Variant regime_var = data.get("regime", Variant());
		if (regime_var.get_type() == Variant::STRING) {
			const String regime = regime_var;
			if (!regime.is_empty()) {
				default_regime = std::string(regime.utf8().get_data());
			}
		}
	} else if (currencies.get_type() == Variant::ARRAY) {
		list = currencies;
	}
	if (list.is_empty()) {
		return;
	}

	// Fallback documentado para moedas sem PIB PPP (currencies.json:
	// `rate_per_anchor: null`, `source: unknown:no_gdp`): usa a mediana das
	// taxas conhecidas e marca source `fallback:median`. A taxa 1,0 fica como
	// último recurso (nunca inventa um PIB).
	std::vector<double> known_rates;
	known_rates.reserve(static_cast<std::size_t>(list.size()));
	for (int index = 0; index < list.size(); ++index) {
		const Dictionary entry = list[index];
		if (!entry.has("rate_per_anchor")) {
			continue;
		}
		const Variant raw = entry.get("rate_per_anchor", Variant());
		if (raw.get_type() == Variant::NIL) {
			continue;
		}
		const double rate = static_cast<double>(raw);
		if (std::isfinite(rate) && rate > 0.0) {
			known_rates.push_back(rate);
		}
	}
	double fallback_rate = 1.0;
	if (!known_rates.empty()) {
		std::sort(known_rates.begin(), known_rates.end());
		fallback_rate = known_rates[known_rates.size() / 2];
	}

	std::vector<DemoCurrency> next;
	next.reserve(static_cast<std::size_t>(list.size()));
	for (int index = 0; index < list.size(); ++index) {
		const Dictionary entry = list[index];
		const String code = String(entry.get("code", ""));
		if (code.is_empty()) {
			continue;
		}
		DemoCurrency currency;
		currency.code = std::string(code.utf8().get_data());
		currency.source = std::string(String(entry.get("source", "default")).utf8().get_data());
		const String regime = String(entry.get("regime", String(default_regime.c_str())));
		currency.floating = regime == "float";
		bool valid = false;
		if (entry.has("rate_per_anchor")) {
			const Variant raw = entry.get("rate_per_anchor", Variant());
			if (raw.get_type() != Variant::NIL) {
				const double rate = static_cast<double>(raw);
				if (std::isfinite(rate) && rate > 0.0) {
					currency.rate = rate;
					valid = true;
				}
			}
		}
		if (!valid) {
			currency.rate = fallback_rate;
			currency.fallback = true;
			currency.source = "fallback:median";
		}
		currency.base_rate = currency.rate;
		const Variant nations_variant = entry.get("nations", Array());
		if (nations_variant.get_type() == Variant::ARRAY) {
			const Array nations = nations_variant;
			for (int n = 0; n < nations.size(); ++n) {
				const std::string nation(nations[n].operator String().utf8().get_data());
				if (!nation.empty()) {
					currency.nations.push_back(nation);
				}
			}
		}
		next.push_back(std::move(currency));
	}
	if (next.empty()) {
		return;
	}
	demo_currencies_ = std::move(next);
	demo_rebuild_currency_indexes();

	// O preço interno está na âncora: carregar moedas NÃO reescala preços. A
	// taxa só entra na exibição local e na competitividade float.
	if (demo_active_ && !demo_cities_.empty()) {
		++demo_status_static_version_;
		demo_status_invalidate();
	}
}


void EconomySim::demo_set_goods(const Array& goods) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	// Tabela de produtos: 21 primários nas posições 0..20 (ordem do goods.json
	// legado) e os bens novos em `order` (21..112). O núcleo não lê arquivos:
	// todo o conteúdo entra por aqui, no padrão do demo_set_goods antigo.
	const int previous_products = demo_products_;
	std::vector<std::string> names = demo_product_names_;
	std::vector<double> prices(kDemoMaxProducts, 0.0);
	std::vector<char> durable(kDemoMaxProducts, 0);
	std::vector<std::string> sectors = demo_product_sectors_;
	sectors.resize(kDemoMaxProducts);
	for (int index = 0; index < previous_products; ++index) {
		if (index < static_cast<int>(names.size())) {
			prices[static_cast<std::size_t>(index)] = demo_base_prices_[index];
		}
	}
	for (int index = 0; index < static_cast<int>(demo_product_durable_.size()); ++index) {
		durable[static_cast<std::size_t>(index)] = demo_product_durable_[static_cast<std::size_t>(index)];
	}
	for (int index = 0; index < goods.size(); ++index) {
		const Dictionary entry = goods[index];
		const String id = String(entry.get("id", ""));
		if (id.is_empty()) {
			continue;
		}
		int order = entry.has("order") ? int(entry.get("order", index)) : index;
		order = std::clamp(order, 0, kDemoMaxProducts - 1);
		if (static_cast<int>(names.size()) <= order) {
			names.resize(static_cast<std::size_t>(order) + 1);
		}
		names[static_cast<std::size_t>(order)] = std::string(id.utf8().get_data());
		const double price = safe_nonneg(static_cast<double>(entry.get("base_price", 0.0)));
		if (price > 0.0) {
			prices[static_cast<std::size_t>(order)] = price;
		}
		durable[static_cast<std::size_t>(order)] =
			bool(entry.get("durable", false)) ? 1 : 0;
		const String sector = String(entry.get("sector", ""));
		sectors[static_cast<std::size_t>(order)] =
			sector.is_empty() ? std::string() : std::string(sector.utf8().get_data());
	}
	int product_count = 21;
	for (int index = 0; index < static_cast<int>(names.size()); ++index) {
		if (!names[static_cast<std::size_t>(index)].empty()) {
			product_count = std::max(product_count, index + 1);
		}
	}
	product_count = std::min(product_count, kDemoMaxProducts);
	names.resize(static_cast<std::size_t>(product_count));
	demo_product_names_ = std::move(names);
	demo_product_lookup_.clear();
	demo_product_lookup_.reserve(static_cast<std::size_t>(product_count) * 2);
	for (int index = 0; index < product_count; ++index) {
		const std::string& name = demo_product_names_[static_cast<std::size_t>(index)];
		if (!name.empty()) {
			demo_product_lookup_[name] = index;
		}
		const double price = prices[static_cast<std::size_t>(index)];
		demo_base_prices_[index] = price > 0.0 ? price : 40.0;
	}
	demo_products_ = product_count;
	demo_product_durable_ = std::move(durable);
	demo_product_sectors_ = std::move(sectors);
	demo_product_sectors_.resize(static_cast<std::size_t>(product_count));

	// Mundo já inicializado: os produtos novos entram com o preço-base e sem
	// estoque. Os índices existentes (0..previous_products-1) não mudam.
	if (demo_active_ && product_count > previous_products) {
		for (DemoCity& city : demo_cities_) {
			for (int product = previous_products; product < product_count; ++product) {
				city.markets[product].price = demo_base_price(product);
			}
		}
		++demo_status_static_version_;
		++demo_status_deposit_version_;
		demo_status_invalidate();
	}
	// F4: os índices dos bens da frota dependem dos ids carregados. Se o mundo
	// já existe e ainda não tem frota (demo_set_goods chamado depois da
	// inicialização, como no autoload), deriva agora.
	demo_fleet_product_fuel_ = demo_product_index(String("fuel"));
	demo_fleet_product_truck_ = demo_product_index(String("truck"));
	demo_fleet_product_ship_ = demo_product_index(String("cargo_ship"));
	demo_fleet_product_plane_ = demo_product_index(String("cargo_plane"));
	demo_fleet_product_parts_ = demo_product_index(String("spare_parts"));
	if (demo_active_ && demo_fleets_.empty()) {
		demo_init_fleets();
	}
}

void EconomySim::initialize_demo() {
	// Novo mundo: o cache de status pertence ao mundo anterior.
	demo_status_slots_[0] = DemoStatusCache{};
	demo_status_slots_[1] = DemoStatusCache{};
	demo_status_live_ = -1;
	demo_status_built_day_ = -1;
	++demo_status_static_version_;
	++demo_status_deposit_version_;
	demo_active_ = true;
	demo_world_mode_ = false;
	day_ = 0;
	demo_cities_.clear();
	demo_buildings_.clear();
	demo_in_transit_.clear();
	demo_ports_.clear();
	demo_last_orders_.clear();
	demo_bankruptcies_ = 0;
	demo_depleted_mines_ = 0;
	demo_transport_cash_ = 10000.0;
	demo_foreign_cash_ = 0.0;
	demo_construction_cash_ = 0.0;
	demo_industry_seeded_ = false;
	for (int index = 0; index < kDemoMaxProducts; ++index) {
		demo_produced_[index] = 0.0;
		demo_consumed_total_[index] = 0.0;
		demo_produced_today_[index] = 0.0;
		demo_consumed_today_[index] = 0.0;
	}

	DemoCity agria;
	agria.id = "AGRIA";
	agria.name = "Agria";
	agria.nation = "DEMO";
	agria.center = {2532.6, 571.4};
	agria.population = 100000.0;
	agria.simulation_population = agria.population;
	agria.firm_cash = 50000.0;
	agria.service_cash = 30000.0;
	agria.government_budget = 100000.0;
	agria.service_jobs = agria.simulation_population * kDemoServiceShare;
	agria.service_wage = kDemoServiceWage;
	agria.pop.population = agria.population;
	agria.pop.savings = 50000.0;
	agria.suitability[0] = kDemoReferenceWheatSuitability;
	agria.markets[0] = {100.0, demo_base_price(0), 0.0, 0.0};
	for (int product = 1; product < demo_products_; ++product) {
		agria.markets[product].price = demo_base_price(product);
	}
	demo_cities_.push_back(agria);

	DemoCity portus;
	portus.id = "PORTUS";
	portus.name = "Portus";
	portus.nation = "DEMO";
	portus.center = {2560.4, 543.8};
	portus.population = 200000.0;
	portus.simulation_population = portus.population;
	portus.firm_cash = 80000.0;
	portus.service_cash = 60000.0;
	portus.government_budget = 150000.0;
	portus.service_jobs = portus.simulation_population * kDemoServiceShare;
	portus.service_wage = kDemoServiceWage;
	portus.pop.population = portus.population;
	portus.pop.savings = 100000.0;
	portus.suitability[0] = 0.625;
	portus.markets[0] = {0.0, demo_base_price(0), 0.0, 0.0};
	for (int product = 1; product < demo_products_; ++product) {
		portus.markets[product].price = demo_base_price(product);
	}
	demo_cities_.push_back(portus);

	// Capacidade é o potencial da terra; a produção efetiva é multiplicada
	// pela aptidão territorial (wheat). A comida é consumida direto do trigo —
	// farinha/pão foram removidos da cadeia (subprodutos ficam para depois).
	const double agria_farm_capacity =
		kDemoFarmCapacityTons / kDemoReferenceWheatSuitability;
	for (int index = 0; index < 3; ++index) {
		DemoBuilding farm;
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
		demo_buildings_.push_back(farm);
	}

	// Portus também produz trigo: reduz a dependência de importação.
	const double portus_farm_capacity = kDemoFarmCapacityTons / 0.625;
	for (int index = 0; index < 2; ++index) {
		DemoBuilding farm;
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
		demo_buildings_.push_back(farm);
	}
	for (DemoCity& city : demo_cities_) {
		city.pop.desired_by_category.assign(demo_food_products_.size(), 0.0);
		city.pop.consumed_by_category.assign(demo_food_products_.size(), 0.0);
		demo_init_pop_needs(city.pop);
		// F7: cenário de validação também tem mercado de trabalho (índice 1,0).
		demo_apply_labor_profile(city);
	}
	demo_rebuild_indexes();
	// F4: o cenário mínimo também tem frota (nação DEMO).
	demo_init_fleets();
	// F8: negócios de serviço iniciais (só quando os dados foram carregados).
	demo_seed_services();
	// F9: grupos a partir dos ativos privados (id determinístico).
	demo_seed_groups();
}

void EconomySim::initialize_demo_world(const Array& cities, const Array& ports,
		const Array& buildings) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	// Novo mundo: o cache de status pertence ao mundo anterior.
	demo_status_slots_[0] = DemoStatusCache{};
	demo_status_slots_[1] = DemoStatusCache{};
	demo_status_live_ = -1;
	demo_status_built_day_ = -1;
	++demo_status_static_version_;
	++demo_status_deposit_version_;
	demo_active_ = true;
	demo_world_mode_ = true;
	day_ = 0;
	demo_cities_.clear();
	demo_buildings_.clear();
	demo_in_transit_.clear();
	demo_invariant_violations_.clear();
	demo_ports_.clear();
	demo_last_orders_.clear();
	demo_bankruptcies_ = 0;
	demo_depleted_mines_ = 0;
	demo_transport_cash_ = 0.0;
	demo_foreign_cash_ = 0.0;
	demo_construction_cash_ = 0.0;
	demo_industry_seeded_ = false;
	// F9: mundo novo, grupos/política novos (a semeadura roda no fim).
	demo_groups_.clear();
	demo_group_lookup_.clear();
	demo_groups_seeded_ = false;
	demo_fdi_policies_.clear();
	demo_fdi_lookup_.clear();
	demo_incentives_.clear();
	demo_incentive_lookup_.clear();
	demo_confidence_.clear();
	demo_confidence_penalty_day_.clear();
	demo_group_flows_.clear();
	for (int index = 0; index < kDemoMaxProducts; ++index) {
		demo_produced_[index] = 0.0;
		demo_consumed_total_[index] = 0.0;
		demo_produced_today_[index] = 0.0;
		demo_consumed_today_[index] = 0.0;
	}

	// Cidades reais do cenário: identidade geográfica + caixas iniciais.
	for (int index = 0; index < cities.size(); ++index) {
		const Dictionary d = cities[index];
		DemoCity city;
		city.id = std::string(String(d["id"]).utf8().get_data());
		city.name = std::string(String(d["name"]).utf8().get_data());
		city.nation = std::string(String(d["nation"]).utf8().get_data());
		city.home_nation =
			std::string(String(d.get("home_nation", d["nation"])).utf8().get_data());
		city.landmass = std::string(String(d.get("landmass", "")).utf8().get_data());
		city.population = safe_nonneg(static_cast<double>(d["population"]));
		city.simulation_population = read_sim_population(d, city.population);
		city.center = {static_cast<double>(d["x"]), static_cast<double>(d["y"])};

		// Caixas iniciais por habitante (constantes do modelo demo). A escala é
		// a população de SIMULAÇÃO: a população real exibida não muda o balanço.
		city.firm_cash = city.simulation_population * kDemoFirmCashPerCapita;
		city.service_cash = city.simulation_population * kDemoServiceCashPerCapita;
		city.government_budget =
			city.simulation_population * kDemoGovernmentPerCapita;
		city.service_jobs = city.simulation_population * kDemoServiceShare;
		city.service_wage = kDemoServiceWage;
		city.pop.population = city.simulation_population;
		city.pop.savings = city.simulation_population * kDemoSavingsPerCapita;
		city.pop.desired_by_category.assign(demo_food_products_.size(), 0.0);
		city.pop.consumed_by_category.assign(demo_food_products_.size(), 0.0);
		demo_init_pop_needs(city.pop);
		// F7: perfil inicial de trabalho/educação a partir do índice de renda
		// (1,0 por padrão; o GeographyService reaplica quando tem o PIB real).
		demo_apply_labor_profile(city);
		// Aptidão padrão (inicializador do struct: suitability = {1.0, 1.0, 1.0,
		// 0, ...} — trigo/ferro/arroz em 1.0). O GeographyService sobrescreve com
		// o dado real em tempo de execução.
		city.suitability[0] = 1.0;
		for (int product = 0; product < demo_products_; ++product) {
			city.markets[product] = {0.0, demo_base_price(product),
				0.0, 0.0};
		}
		demo_cities_.push_back(std::move(city));
	}

	// Construções reais (fazendas primárias): o produto vem do campo "resource"
	// (resolvido pelo GeographyService para o alimento básico de maior aptidão ×
	// caloria da nação da cidade); ausente, mantém trigo (comportamento legado).
	// A produção efetiva segue multiplicando a capacidade pela aptidão do produto
	// escolhido (fase 4b usa city.suitability[output_product]).
	for (int index = 0; index < buildings.size(); ++index) {
		const Dictionary d = buildings[index];
		DemoBuilding building;
		building.id = std::string(
			String(d.get("id", d.get("building_id", ""))).utf8().get_data());
		building.city = std::string(String(d["city_id"]).utf8().get_data());
		building.resource = std::string(
			String(d.get("resource", "wheat")).utf8().get_data());
		const int output_product = demo_product_index(
			String(building.resource.c_str()));
		building.output_product = output_product >= 0 ? output_product : 0;
		building.kind = 0;  // tipo (0=fazenda); o produto fica em output_product
		building.owner = "private";
		building.input_product = -1;
		const double capacity =
			safe_nonneg(static_cast<double>(d["output_tons_per_day"]));
		// Aptidão da cultura escolhida na bacia da cidade (opcional; 1.0 no
		// caminho legado). A capacidade nominal é o teto de terra; a produção
		// efetiva da fase 4b é capacidade × aptidão. Folha e opex acompanham a
		// FRAÇÃO APTA da terra — antes seguiam a capacidade nominal inteira e
		// uma fazenda de aptidão 4% pagava 100% dos custos para produzir 4%,
		// falindo em 60 dias (§19). Sem essa correção o mundo perdia 83 das 252
		// fazendas iniciais no dia 61.
		const double aptitude = std::clamp(
			safe_nonneg(static_cast<double>(d.get("aptitude", 1.0))), 0.0, 1.0);
		building.capacity = capacity;
		building.input_per_output = 0.0;
		building.jobs = capacity * aptitude * kDemoJobsPerTon;
		building.wage = kDemoFarmWage;
		building.opex = capacity * aptitude * kDemoOpexPerTon;
		demo_buildings_.push_back(std::move(building));
	}

	// Portos do cenário (identidade geográfica, para o minimapa e os comboios).
	for (int index = 0; index < ports.size(); ++index) {
		const Dictionary d = ports[index];
		Port port;
		port.id = std::string(String(d["id"]).utf8().get_data());
		port.name = std::string(String(d["name"]).utf8().get_data());
		port.nation = std::string(String(d["nation"]).utf8().get_data());
		port.center = {static_cast<double>(d["x"]), static_cast<double>(d["y"])};
		port.landmass = std::string(String(d.get("landmass", "")).utf8().get_data());
		// Cidade-sede: só a âncora do porto (não é dono exclusivo do tráfego).
		port.host_city = std::string(String(d.get("port_of", d.get("host_city", "")))
			.utf8().get_data());
		demo_ports_.push_back(std::move(port));
	}

	demo_rebuild_indexes();
	// Capacidade de nível 1 depende da população da cidade-sede: só depois do
	// índice de cidades (e de os portos existirem).
	demo_init_port_capacities();
	// F4: frota inicial derivada de população/PIB (source: derived). Os bens da
	// frota só resolvem depois de demo_set_goods; aqui as contagens/estoques já
	// existem e os índices são atualizados quando os bens chegam.
	demo_init_fleets();
	// F8: serviços — catálogo já carregado pelo autoload/teste; os negócios
	// iniciais saem do caixa do próprio setor (capital fica em service_cash).
	demo_seed_services();
	// F9: grupos/empresas a partir dos ativos privados existentes.
	demo_seed_groups();
}

EconomySim::DemoCity* EconomySim::find_demo_city(const std::string& id) {
	const auto found = demo_city_lookup_.find(id);
	if (found == demo_city_lookup_.end()) {
		return nullptr;
	}
	return &demo_cities_[static_cast<std::size_t>(found->second)];
}

const EconomySim::DemoCity* EconomySim::find_demo_city(const std::string& id) const {
	const auto found = demo_city_lookup_.find(id);
	if (found == demo_city_lookup_.end()) {
		return nullptr;
	}
	return &demo_cities_[static_cast<std::size_t>(found->second)];
}

void EconomySim::demo_rebuild_indexes() {
	demo_city_lookup_.clear();
	demo_city_lookup_.reserve(demo_cities_.size() * 2);
	for (std::size_t index = 0; index < demo_cities_.size(); ++index) {
		demo_city_lookup_.emplace(demo_cities_[index].id, static_cast<int>(index));
	}
	// Uma lista de construções por cidade: as fases do tick e o resumo passam a
	// visitar só as construções locais, em vez de varrer o mundo inteiro.
	demo_buildings_by_city_.assign(demo_cities_.size(), {});
	for (std::size_t index = 0; index < demo_buildings_.size(); ++index) {
		const auto found = demo_city_lookup_.find(demo_buildings_[index].city);
		if (found == demo_city_lookup_.end()) {
			continue;
		}
		demo_buildings_by_city_[static_cast<std::size_t>(found->second)]
			.push_back(static_cast<int>(index));
	}
	// F4: cidades por nação, na ordem estável dos índices de cidade. A frota é
	// nacional (o governo compra combustível/peças/veículos no mercado interno),
	// então a compra visita só as cidades da nação, nunca o mundo.
	demo_cities_by_nation_.clear();
	demo_cities_by_nation_.reserve(demo_cities_.size() / 4 + 8);
	demo_fleet_order_.clear();
	demo_fleet_order_.reserve(demo_cities_.size() / 4 + 8);
	for (std::size_t index = 0; index < demo_cities_.size(); ++index) {
		const std::string& nation = demo_cities_[index].nation;
		auto found = demo_cities_by_nation_.find(nation);
		if (found == demo_cities_by_nation_.end()) {
			found = demo_cities_by_nation_.emplace(nation, std::vector<int>{}).first;
			demo_fleet_order_.push_back(nation);
		}
		found->second.push_back(static_cast<int>(index));
	}
}

double EconomySim::demo_money_total() const {
	double total = demo_transport_cash_ + demo_foreign_cash_ + demo_construction_cash_;
	for (const DemoCity& city : demo_cities_) {
		// capital_income_next é um crédito pendente às famílias: entra no total
		// para a conservação valer em qualquer ponto do tick.
		total += city.pop.savings + city.firm_cash + city.service_cash
		       + city.government_budget + city.capital_income_next;
	}
	// F9: o caixa dos grupos é dinheiro do mundo (capital retido/dividendos a
	// pagar). Sem ele, mover lucro da firma para o grupo pareceria destruição.
	for (const DemoGroup& group : demo_groups_) {
		total += group.cash;
	}
	return total;
}

void EconomySim::demo_snapshot(DemoSnapshot& out) const {
	out.cities = demo_cities_;
	out.buildings = demo_buildings_;
	out.service_businesses = demo_service_businesses_;
	out.next_service_business_id = demo_next_service_business_id_;
	out.service_bankruptcies = demo_service_bankruptcies_;
	out.transit = demo_in_transit_;
	out.fleets = demo_fleets_;
	out.groups = demo_groups_;
	out.confidence = demo_confidence_;
	out.group_flows = demo_group_flows_;
	out.currencies = demo_currencies_;
	out.transport_cash = demo_transport_cash_;
	out.foreign_cash = demo_foreign_cash_;
	out.construction_cash = demo_construction_cash_;
	for (int index = 0; index < demo_products_; ++index) {
		out.produced[index] = demo_produced_[index];
		out.consumed_total[index] = demo_consumed_total_[index];
		out.produced_today[index] = demo_produced_today_[index];
		out.consumed_today[index] = demo_consumed_today_[index];
	}
	out.day = day_;
}

void EconomySim::demo_restore(const DemoSnapshot& in) {
	demo_cities_ = in.cities;
	demo_buildings_ = in.buildings;
	demo_service_businesses_ = in.service_businesses;
	demo_next_service_business_id_ = in.next_service_business_id;
	demo_service_bankruptcies_ = in.service_bankruptcies;
	demo_in_transit_ = in.transit;
	demo_fleets_ = in.fleets;
	demo_groups_ = in.groups;
	demo_confidence_ = in.confidence;
	demo_group_flows_ = in.group_flows;
	// F5: o drift cambial roda antes do invariante; desfazer um tick
	// corrompido também desfaz a taxa do dia.
	demo_currencies_ = in.currencies;
	demo_rebuild_currency_indexes();
	demo_transport_cash_ = in.transport_cash;
	demo_foreign_cash_ = in.foreign_cash;
	demo_construction_cash_ = in.construction_cash;
	for (int index = 0; index < demo_products_; ++index) {
		demo_produced_[index] = in.produced[index];
		demo_consumed_total_[index] = in.consumed_total[index];
		demo_produced_today_[index] = in.produced_today[index];
		demo_consumed_today_[index] = in.consumed_today[index];
	}
	day_ = in.day;
	demo_rebuild_indexes();
	demo_rebuild_service_indexes();
	// F9: o índice de grupos acompanha o vetor restaurado.
	demo_group_lookup_.clear();
	for (std::size_t index = 0; index < demo_groups_.size(); ++index) {
		demo_group_lookup_[demo_groups_[index].id] = static_cast<int>(index);
	}
}

std::vector<std::string> EconomySim::demo_check_invariants(double money_before) const {
	std::vector<std::string> violations;

	// 1. Conservação de dinheiro: toda transação move dinheiro entre caixas
	// (firma, serviço, governo, poupança, transporte, exterior, construção).
	// O resto do mundo (foreign_cash) pode ficar negativo por design.
	const double money_after = demo_money_total();
	const double drift = std::abs(money_after - money_before);
	const double tolerance = std::max(1e-3, std::abs(money_before) * 1e-12);
	if (!std::isfinite(money_after) || drift > tolerance) {
		violations.push_back("dinheiro não conservado: antes="
			+ std::to_string(money_before) + " depois="
			+ std::to_string(money_after) + " drift=" + std::to_string(drift));
	}

	for (const DemoCity& city : demo_cities_) {
		auto check_nonneg = [&violations, &city](const char* label, double value) {
			if (!std::isfinite(value) || value < -1e-6) {
				violations.push_back(std::string("caixa negativo (") + label + ") em "
					+ city.id + ": " + std::to_string(value));
			}
		};
		check_nonneg("firm", city.firm_cash);
		check_nonneg("service", city.service_cash);
		check_nonneg("government", city.government_budget);
		check_nonneg("savings", city.pop.savings);

		// Preço na âncora: o câmbio não move mais o preço interno (só a
		// competitividade das ofertas float e a exibição local), então a banda
		// pode ser comparada sem folga.
		for (int product = 0; product < demo_products_; ++product) {
			const DemoMarket& market = city.markets[product];
			const double base = demo_base_price(product);
			const double floor_price = base * 0.5;
			const double ceiling = base * 3.0;
			if (!std::isfinite(market.price)
					|| market.price < floor_price
					|| market.price > ceiling) {
				violations.push_back("preço fora da banda em " + city.id + " "
					+ demo_product_name(product) + ": " + std::to_string(market.price)
					+ " (base âncora " + std::to_string(base) + ", moeda "
					+ demo_currency_code(city.nation) + " taxa "
					+ std::to_string(demo_fx_rate(city.nation)) + ")");
			}
			if (!std::isfinite(market.stock) || market.stock < -1e-6) {
				violations.push_back("estoque negativo em " + city.id + " "
					+ demo_product_name(product) + ": " + std::to_string(market.stock));
			}
		}
	}

	auto check_global = [&violations](const char* label, double value) {
		if (!std::isfinite(value) || value < -1e-6) {
			violations.push_back(std::string("caixa global negativo (") + label
				+ "): " + std::to_string(value));
		}
	};
	check_global("transport", demo_transport_cash_);
	check_global("construction", demo_construction_cash_);
	// F9: caixa de grupo é dinheiro do mundo; não pode ser negativo nem NaN.
	for (const DemoGroup& group : demo_groups_) {
		if (!std::isfinite(group.cash) || group.cash < -1e-6) {
			violations.push_back("caixa de grupo inválido (" + group.id + "): "
				+ std::to_string(group.cash));
		}
	}

	return violations;
}

void EconomySim::demo_trade_food() {
	// Comércio genérico de alimento (cesta de calorias, B3) entre cidades do
	// mundo real: cidades com déficit compram das que têm excedente, escolhendo
	// o alimento que entregar a caloria mais barata (preço + frete por
	// tonelada-equivalente de grão), mais perto primeiro quando há empate. O
	// frete é serviço doméstico do comprador (conserva o dinheiro).
	//
	// Porto (docs/economy/port_design.md): a travessia entre massas de terra
	// passa obrigatoriamente por um porto da nação em cada ponta; a capacidade
	// diária do cais é consumida aqui e o excesso vira fila (atraso + demurrage).
	demo_reset_port_usage();

	struct Buyer {
		int index;
		DemoCity* city;
		double gap;  // toneladas-equivalentes de grão
	};
	std::vector<Buyer> buyers;

	// Posição alimentar de cada cidade em toneladas-equivalentes de grão:
	// (estoque local + em trânsito) × fator calórico.
	std::vector<double> food_ge(demo_cities_.size(), 0.0);
	for (std::size_t i = 0; i < demo_cities_.size(); ++i) {
		const DemoCity& city = demo_cities_[i];
		double ge = 0.0;
		for (int slot = 0; slot < static_cast<int>(demo_food_products_.size()); ++slot) {
			const int product = demo_food_product(slot);
			ge += city.markets[product].stock * demo_food_calories(product);
		}
		food_ge[i] = ge;
	}
	// Chegada em trânsito por cidade: uma passada única com o índice de
	// cidades. Antes, cada carga varria TODAS as cidades comparando strings
	// (O(cargas × cidades) por dia, ~440 mil comparações com 512 cargas e 858
	// cidades no cenário real).
	for (const DemoShipment& shipment : demo_in_transit_) {
		const auto found = demo_city_lookup_.find(shipment.to_city);
		if (found == demo_city_lookup_.end()) {
			continue;
		}
		const std::size_t i = static_cast<std::size_t>(found->second);
		food_ge[i] += shipment.amount * demo_food_calories(shipment.product);
	}

	// Excedente vendável por cidade: o vendedor exporta o que sobra ACIMA da
	// própria necessidade de UM dia (não do buffer de cobertura). A fase 4 já
	// consome a refeição do dia; exigir a cobertura de 3 dias inteiros trava a
	// exportação — a produção visa exatamente `sold_prev × cover_days`, então o
	// estoque estaciona na cobertura e nada fica vendável (nenhuma cidade
	// exporta, e as deficitárias morrem de fome). Manter 1 dia preserva o buffer
	// mínimo e deixa o excedente fluir para quem tem déficit.
	std::vector<double> sellable_ge(demo_cities_.size(), 0.0);
	for (std::size_t i = 0; i < demo_cities_.size(); ++i) {
		sellable_ge[i] = std::max(0.0, food_ge[i] - demo_cities_[i].pop.need);
	}

	for (std::size_t i = 0; i < demo_cities_.size(); ++i) {
		// Cobertura-alvo em dias (como o mercado legado usa kCoverDays): a cidade
		// importa para manter um estoque de alguns dias, não só o déficit do dia.
		const double target = demo_cities_[i].pop.need * demo_inventory_cover_days_;
		const double gap = target - food_ge[i];
		if (gap > 1e-9) {
			buyers.push_back({static_cast<int>(i), &demo_cities_[i], gap});
		}
	}
	if (buyers.empty()) {
		return;
	}

	// Quantos vendedores existem no mundo hoje. Sem nenhum, nenhum comprador
	// pode ser atendido: sair aqui evita varrer a grade inteira por comprador
	// (medido em 650 cidades, dia 1: 1,4 s só provando que não havia oferta).
	int total_sellers = 0;
	for (std::size_t i = 0; i < sellable_ge.size(); ++i) {
		if (sellable_ge[i] > 1e-9) {
			++total_sellers;
		}
	}
	if (total_sellers == 0) {
		return;
	}

	// Compradores com MENOR COBERTURA RELATIVA primeiro (gap / necessidade):
	// sob escassez, todas as cidades convergem para a mesma fração atendida.
	// A ordem anterior era gap ABSOLUTO ascendente — ou seja, completava o
	// buffer de quem já estava quase cheio e deixava as megacidades (Baku,
	// Xangai, São Paulo) no fim da fila todos os dias; com a oferta mundial
	// abaixo da necessidade, elas nunca eram alcançadas (§19).
	std::stable_sort(buyers.begin(), buyers.end(),
		[](const Buyer& left, const Buyer& right) {
			const double left_need = std::max(1e-9, left.city->pop.need);
			const double right_need = std::max(1e-9, right.city->pop.need);
			return left.gap / left_need > right.gap / right_need;
		});

	// Índice espacial e ofertas por produto, montados UMA vez por dia.
	//
	// Antes, cada comprador varria TODAS as cidades e ordenava todas as ofertas
	// do mundo (frete por par) — O(cidades²), ~500 ms/dia com 650 cidades.
	// Agora o comprador avalia só os K vendedores mais próximos (grade) mais as
	// M ofertas mais baratas por caloria do globo, que representam o mercado
	// externo. São vendedores REAIS: o dinheiro continua indo para uma firma
	// existente, sem mercado agregado artificial.
	const std::size_t city_count = demo_cities_.size();
	// Porto nacional de cada cidade (mesma nação e massa de terra), escolhido
	// uma vez por dia: o mais próximo é o de menor frete terrestre. É o porto
	// que a cidade usa para exportar/importar — `port_of` só aponta a sede.
	std::vector<int> city_port(city_count, -1);
	if (!demo_ports_.empty()) {
		// Baldes por (nação, massa de terra): a cidade só compara com os portos
		// que pode usar (porto nacional na própria massa), não com os ~138 do
		// mundo. Custa uma fração de uma passada cidades × portos.
		std::unordered_map<std::string, std::vector<int>> ports_by_region;
		ports_by_region.reserve(demo_ports_.size() * 2);
		for (std::size_t pi = 0; pi < demo_ports_.size(); ++pi) {
			const Port& port = demo_ports_[pi];
			ports_by_region[port.nation + '\n' + port.landmass]
				.push_back(static_cast<int>(pi));
		}
		for (std::size_t ci = 0; ci < city_count; ++ci) {
			const DemoCity& city = demo_cities_[ci];
			const auto found =
				ports_by_region.find(city.nation + '\n' + city.landmass);
			if (found == ports_by_region.end()) {
				continue;
			}
			int best_port = -1;
			double best_distance = 0.0;
			for (const int pi : found->second) {
				const double distance =
					distance_km(city.center, demo_ports_[static_cast<std::size_t>(pi)].center);
				if (best_port < 0 || distance < best_distance) {
					best_port = pi;
					best_distance = distance;
				}
			}
			city_port[ci] = best_port;
		}
	}
	const bool exhaustive = city_count <= kDemoTradeExhaustiveLimit;
	const double cell = kDemoTradeCellMapUnits;
	const int grid_w = std::max(1, static_cast<int>(std::ceil(kMapWidth / cell)));
	const int grid_h = std::max(1, static_cast<int>(std::ceil(kMapHeight / cell)));
	std::vector<std::vector<int>> grid(
		static_cast<std::size_t>(grid_w * grid_h));
	// Bitmask de células com pelo menos um vendedor: quando a oferta do dia é
	// escassa (regra do porto deixa parte do mundo sem rota marítima), o anel
	// varria a grade inteira por comprador. A máscara evita tocar nas ~12 mil
	// listas vazias (cache miss por célula) sem mudar QUEM é candidato: o
	// filtro de `sellable_ge` dentro de scan_cell continua valendo.
	std::vector<char> cell_has_seller(static_cast<std::size_t>(grid_w * grid_h), 0);
	auto cell_index = [cell, grid_w, grid_h](const MapPoint& point) {
		const int ix = std::clamp(static_cast<int>(point.x / cell), 0, grid_w - 1);
		const int iy = std::clamp(static_cast<int>(point.y / cell), 0, grid_h - 1);
		return iy * grid_w + ix;
	};
	for (std::size_t si = 0; si < city_count; ++si) {
		const int index = cell_index(demo_cities_[si].center);
		grid[static_cast<std::size_t>(index)].push_back(static_cast<int>(si));
		if (sellable_ge[si] > 1e-9) {
			cell_has_seller[static_cast<std::size_t>(index)] = 1;
		}
	}

	// Ofertas por slot da cesta, ordenadas pelo custo por caloria do preço
	// (preço/fator, sem frete): as M primeiras formam o mercado externo.
	std::vector<std::vector<int>> offers_by_slot(demo_food_products_.size());
	for (std::size_t si = 0; si < city_count; ++si) {
		if (sellable_ge[si] <= 1e-9) {
			continue;
		}
		for (int slot = 0; slot < static_cast<int>(demo_food_products_.size()); ++slot) {
			const int product = demo_food_product(slot);
			const double factor = demo_food_calories(product);
			if (factor <= 0.0) {
				continue;
			}
			const double price = demo_cities_[si].markets[product].price
				* demo_competitiveness(demo_cities_[si].nation);
			const double sellable_tons = std::min(
				demo_cities_[si].markets[product].stock, sellable_ge[si] / factor);
			if (sellable_tons > 1e-9 && price > 0.0) {
				offers_by_slot[slot].push_back(static_cast<int>(si));
			}
		}
	}
	for (int slot = 0; slot < static_cast<int>(demo_food_products_.size()); ++slot) {
		const int product = demo_food_product(slot);
		std::vector<int>& list = offers_by_slot[slot];
		// Dentro de um slot o fator calórico é o mesmo para todos: ordenar pelo
		// preço equivale a ordenar pelo custo por caloria (sem frete).
		std::stable_sort(list.begin(), list.end(), [this, product](int l, int r) {
			return demo_cities_[static_cast<std::size_t>(l)].markets[product].price
				* demo_competitiveness(demo_cities_[static_cast<std::size_t>(l)].nation)
				< demo_cities_[static_cast<std::size_t>(r)].markets[product].price
				* demo_competitiveness(demo_cities_[static_cast<std::size_t>(r)].nation);
		});
	}

	const int k_neighbors = std::max(1, demo_trade_neighbors_);
	const int k_external = std::max(0, demo_trade_external_);
	// Nunca vale a pena procurar mais vizinhos do que existem vendedores no mundo:
	// quando há menos ofertas que K, o anel para de expandir assim que todas
	// foram vistas (antes ele varria a grade inteira para provar isso, o que
	// custava ~1,4 s por dia com 650 cidades). `sellers_left` cai quando um
	// vendedor se esgota no meio do dia.
	int sellers_left = total_sellers;

	// Vendedores alcançáveis por um comprador (regra do porto): na mesma massa
	// de terra sempre; entre massas, só se AS DUAS pontas têm porto. Contagens
	// iniciais do dia são um teto (só diminuem quando um vendedor esgota), então
	// nunca descartam um comprador que ainda poderia comprar. Sem isso, um
	// comprador sem rota possível varria a grade inteira (12 mil células) todos
	// os dias atrás de vendedores que nunca poderiam servi-lo.
	std::unordered_map<std::string, int> sellers_in_landmass;
	std::unordered_map<std::string, int> port_sellers_in_landmass;
	int port_sellers_total = 0;
	for (std::size_t si = 0; si < city_count; ++si) {
		if (sellable_ge[si] <= 1e-9) {
			continue;
		}
		++sellers_in_landmass[demo_cities_[si].landmass];
		if (city_port[si] >= 0) {
			++port_sellers_in_landmass[demo_cities_[si].landmass];
			++port_sellers_total;
		}
	}
	std::size_t shipments = 0;
	for (Buyer& buyer : buyers) {
		if (sellers_left <= 0) {
			break;  // todo o excedente do dia já foi vendido
		}
		double remaining = buyer.gap;  // toneladas-equivalentes de grão

		// Ofertas: cada (vendedor, alimento) com excedente vendável, ordenadas
		// pelo custo calórico entregue (preço + frete) / fator calórico.
		struct Offer {
			int seller_index;
			int product;
			double factor;
			double price;          // preço na âncora XGS (mercado do vendedor)
			double origin_anchor;  // preço × competitividade float do vendedor
			double unit_freight;   // frete em âncora
			double unit_tariff;    // tarifa em âncora
			double delivered;      // preço entregue na moeda do comprador (exibição)
			double unit_cost_per_ge;  // âncora por tonelada-equivalente de grão
		};

		// Candidatos do comprador: K vendedores mais próximos + mercado externo.
		// Rota possível? Na mesma massa de terra, caminhão direto; entre massas,
		// só com porto nas duas pontas. Comprador sem nenhum vendedor alcançável
		// não gera oferta: sair aqui evita varrer a grade inteira por dia.
		const int buyer_port = city_port[static_cast<std::size_t>(buyer.index)];
		const int local_sellers = sellers_in_landmass[buyer.city->landmass];
		const int local_port_sellers = port_sellers_in_landmass[buyer.city->landmass];
		const int reachable_sellers = local_sellers + (buyer_port >= 0
			? port_sellers_total - local_port_sellers : 0);
		if (reachable_sellers <= 0) {
			continue;
		}
		std::vector<int> candidates;
		if (exhaustive) {
			candidates.reserve(city_count);
			for (std::size_t si = 0; si < city_count; ++si) {
				if (static_cast<int>(si) != buyer.index && sellable_ge[si] > 1e-9) {
					candidates.push_back(static_cast<int>(si));
				}
			}
		} else {
			const int bx = std::clamp(
				static_cast<int>(buyer.city->center.x / cell), 0, grid_w - 1);
			const int by = std::clamp(
				static_cast<int>(buyer.city->center.y / cell), 0, grid_h - 1);
			// Raio máximo ÚTIL: distância de Chebyshev até o canto mais distante
			// da grade. Antes era max(grid_w, grid_h), ou seja, o anel continuava
			// varrendo células vazias muito além do mapa: com grade 157×79 e
			// menos de K vendedores no mundo, cada comprador percorria ~5,2 M
			// células (O(raio³) acumulado) — o bench ficava minutos parado.
			//
			// Teto ECONÔMICO (regra do porto): sem porto só há caminhão, cujo
			// frete já supera o preço do alimento bem antes de 2.000 km (~8
			// células de 250 km). Com porto, o navio é 5× mais barato e o anel
			// cobre o globo. Sem esse teto, um comprador sem rota marítima
			// varria a grade inteira todos os dias atrás de vendedores que nunca
			// seriam competitivos.
			const int ring_radius_limit = buyer_port >= 0 ? 48 : 8;
			const int max_radius = std::min(std::max(
				std::max(bx, grid_w - 1 - bx), std::max(by, grid_h - 1 - by)),
				ring_radius_limit);
			const int wanted = std::min(k_neighbors,
				std::min(sellers_left, reachable_sellers));
			// Limite de segurança: nenhum comprador visita mais células que a
			// grade inteira. Com oferta escassa (fim do dia, porto bloqueando
			// rotas), o anel tentava provar que não havia mais ninguém até o
			// fim do mapa para cada comprador; 1.024 células (raio ~16, ~4.000
			// km) já cobrem o alcance econômico — além dele entra o mercado
			// externo (M ofertas mais baratas do globo), que é anexado sempre.
			int cells_budget = std::min(grid_w * grid_h, 1024);
			auto scan_cell = [&](int ix, int iy) {
				if (ix < 0 || ix >= grid_w || iy < 0 || iy >= grid_h || cells_budget <= 0) {
					return;
				}
				--cells_budget;
				const std::size_t cell_offset =
					static_cast<std::size_t>(iy * grid_w + ix);
				if (cell_has_seller[cell_offset] == 0) {
					return;
				}
				for (const int si : grid[cell_offset]) {
					if (si == buyer.index || sellable_ge[static_cast<std::size_t>(si)] <= 1e-9) {
						continue;
					}
					// Sem porto, a cidade só compra dentro da própria massa de
					// terra: vendedores do outro lado do mar não são candidatos.
					if (buyer_port < 0
							&& demo_cities_[static_cast<std::size_t>(si)].landmass
								!= buyer.city->landmass) {
						continue;
					}
					candidates.push_back(si);
				}
			};
			for (int radius = 0; radius <= max_radius && cells_budget > 0; ++radius) {
				if (static_cast<int>(candidates.size()) >= wanted) {
					break;
				}
				// Só o PERÍMETRO do anel (4 lados, cantos uma vez só): O(raio)
				// por anel em vez de O(raio²) com `continue` nas células internas.
				for (int ix = bx - radius; ix <= bx + radius; ++ix) {
					scan_cell(ix, by - radius);
					if (radius > 0) {
						scan_cell(ix, by + radius);
					}
				}
				for (int iy = by - radius + 1; iy <= by + radius - 1; ++iy) {
					scan_cell(bx - radius, iy);
					if (radius > 0) {
						scan_cell(bx + radius, iy);
					}
				}
			}
			if (static_cast<int>(candidates.size()) > k_neighbors) {
				std::partial_sort(candidates.begin(),
					candidates.begin() + k_neighbors, candidates.end(),
					[this, &buyer](int l, int r) {
						return distance_km(demo_cities_[static_cast<std::size_t>(l)].center,
								buyer.city->center)
							< distance_km(demo_cities_[static_cast<std::size_t>(r)].center,
								buyer.city->center);
					});
				candidates.resize(static_cast<std::size_t>(k_neighbors));
			}
			// Mercado externo: as M ofertas mais baratas por caloria de cada
			// alimento da cesta (o resto do mundo, do ponto de vista do comprador).
			for (int slot = 0; slot < static_cast<int>(demo_food_products_.size()); ++slot) {
				const std::vector<int>& list = offers_by_slot[slot];
				const int limit = std::min(k_external, static_cast<int>(list.size()));
				for (int i = 0; i < limit; ++i) {
					if (list[static_cast<std::size_t>(i)] != buyer.index) {
						candidates.push_back(list[static_cast<std::size_t>(i)]);
					}
				}
			}
			std::sort(candidates.begin(), candidates.end());
			candidates.erase(std::unique(candidates.begin(), candidates.end()),
				candidates.end());
		}

		std::vector<Offer> offers;
		offers.reserve(candidates.size() * demo_food_products_.size());
		for (const int si : candidates) {
			DemoCity& seller = demo_cities_[static_cast<std::size_t>(si)];
			if (&seller == buyer.city || sellable_ge[static_cast<std::size_t>(si)] <= 1e-9) {
				continue;
			}
			const bool same_landmass = seller.landmass == buyer.city->landmass;
			double unit_freight = 0.0;
			if (same_landmass) {
				unit_freight = demo_leg_unit_freight_food(TransportMode::Truck,
					distance_km(seller.center, buyer.city->center));
			} else {
				// Travessia marítima só entre nações COM porto na massa de terra
				// de cada cidade: cidade→porto (caminhão) + porto→porto (navio) +
				// porto→cidade (caminhão). Sem porto de um dos lados, não há
				// oferta: a nação só comercia por terra com vizinhos.
				const int seller_port = city_port[static_cast<std::size_t>(si)];
				const int buyer_port = city_port[buyer.index];
				if (seller_port < 0 || buyer_port < 0) {
					continue;
				}
				const Port& sp = demo_ports_[static_cast<std::size_t>(seller_port)];
				const Port& bp = demo_ports_[static_cast<std::size_t>(buyer_port)];
				unit_freight =
					demo_leg_unit_freight_food(TransportMode::Truck,
						distance_km(seller.center, sp.center))
					+ demo_leg_unit_freight_food(TransportMode::Ship,
						distance_km(sp.center, bp.center))
					+ demo_leg_unit_freight_food(TransportMode::Truck,
						distance_km(bp.center, buyer.city->center));
			}
			for (int slot = 0; slot < static_cast<int>(demo_food_products_.size()); ++slot) {
				const int product = demo_food_product(slot);
				const double factor = demo_food_calories(product);
				if (factor <= 0.0) {
					continue;
				}
				// Vendável deste produto, limitado pelo excedente total da cidade
				// (acima da cobertura) e pelo estoque físico.
				const double sellable_tons = std::min(
					seller.markets[product].stock,
					sellable_ge[static_cast<std::size_t>(si)] / factor);
				if (sellable_tons <= 1e-9) {
					continue;
				}
				const double price = seller.markets[product].price;
				if (price <= 0.0) {
					continue;
				}
				const double origin_anchor =
					price * demo_competitiveness(seller.nation);
				const double tariff_rate = demo_tariff_rate(
					buyer.city->nation, seller.nation, product);
				const double unit_tariff =
					(origin_anchor + unit_freight) * tariff_rate;
				// Valor entregue em âncora: é ele que ordena a oferta (sem
				// round-trip de câmbio). A moeda do comprador só divide para
				// exibição — é fator comum, não muda a ordem.
				const double delivered_anchor =
					origin_anchor + unit_freight + unit_tariff;
				const double buyer_fx = demo_fx_rate(buyer.city->nation);
				const double delivered =
					delivered_anchor / std::max(1e-9, buyer_fx);
				const double unit_cost_per_ge = delivered_anchor / factor;
				offers.push_back({si, product, factor, price, origin_anchor,
					unit_freight, unit_tariff, delivered, unit_cost_per_ge});
			}
		}
		std::stable_sort(offers.begin(), offers.end(),
			[](const Offer& left, const Offer& right) {
				return left.unit_cost_per_ge < right.unit_cost_per_ge;
			});

		for (const Offer& offer : offers) {
			if (remaining <= 1e-9 || shipments >= kDemoMaxShipmentsPerDay) {
				break;
			}
			const int si = offer.seller_index;
			DemoCity* seller = &demo_cities_[si];
			const int product = offer.product;
			const double factor = offer.factor;
			const double origin_anchor = offer.origin_anchor;
			const double unit_freight = offer.unit_freight;
			const double unit_tariff = offer.unit_tariff;
			const double unit_cost = origin_anchor + unit_freight + unit_tariff;
			// Vendável recalculado (o excedente total cai a cada embarque).
			const double sellable_tons = std::min(
				seller->markets[product].stock, sellable_ge[si] / factor);
			if (sellable_tons <= 1e-9) {
				continue;
			}

			// Teto de carga diária (capacidade de transporte), em ton-equiv.
			const double capacity_ge =
				buyer.city->simulation_population * kDemoTradeCapacityPerCapita;
			double imported_ge = 0.0;
			for (int slot = 0; slot < static_cast<int>(demo_food_products_.size()); ++slot) {
				const int p = demo_food_product(slot);
				imported_ge += buyer.city->imported[p] * demo_food_calories(p);
			}
			const double room_ge = std::max(0.0, capacity_ge - imported_ge);
			const double amount_wanted_ge = std::min(
				remaining, std::min(sellable_tons * factor, room_ge));
			if (amount_wanted_ge <= 1e-9) {
				continue;
			}

			// Segurança alimentar: o governo subsidia a parte da importação que a
			// firma não consegue pagar (cidade sem produção/caixa). Conserva
			// dinheiro — firma + governo pagam exatamente o custo (carga + frete).
			// As FAMÍLIAS são as compradoras finais: quando o capital de giro da
			// firma e o orçamento público não cobrem a carga, a poupança das
			// famílias entra como pagamento. O alimento chega ao mercado e é
			// comprado por elas no dia seguinte, devolvendo o caixa à firma — é
			// o mesmo dinheiro, não uma transferência nova. Sem isso, uma cidade
			// com poupança alta e firma sem caixa (São Paulo, 28 M) ficava com
			// fome enquanto o dinheiro dormia na conta das famílias (§19).
			const double firm_cash = std::max(0.0, buyer.city->firm_cash);
			const double gov_budget = std::max(0.0, buyer.city->government_budget);
			const double savings = std::max(0.0, buyer.city->pop.savings);
			const double payable = firm_cash + gov_budget + savings;
			double amount_tons = amount_wanted_ge / factor;
			if (amount_tons * unit_cost > payable) {
				amount_tons = unit_cost > 0.0 ? payable / unit_cost : 0.0;
			}

			if (amount_tons <= 1e-9) {
				continue;
			}
			double ge = std::min(amount_tons * factor, remaining);
			amount_tons = ge / factor;

			// F4: frota livre + combustível limitam o embarque. Sem um dos dois,
			// a carga não sai (fica na fila da nação vendedora, que é a dona do
			// veículo); com frota parcial, sai proporcional. Portos/fila/
			// transbordo continuam sendo os existentes.
			const bool fleet_same_landmass =
				seller->landmass == buyer.city->landmass;
			DemoTransportDemand transport = demo_route_demand(*seller, *buyer.city,
				fleet_same_landmass ? -1 : city_port[static_cast<std::size_t>(si)],
				fleet_same_landmass ? -1 : city_port[buyer.index], amount_tons);
			bool fuel_limited = false;
			const double fleet_scale = demo_fleet_capacity_scale(
				seller->nation, transport, fuel_limited);
			if (fleet_scale < 1.0) {
				const double queued_tons = amount_tons * (1.0 - fleet_scale);
				const auto fleet_found = demo_fleets_.find(seller->nation);
				if (fleet_found != demo_fleets_.end()) {
					fleet_found->second.queue_today += queued_tons;
					if (fuel_limited) {
						fleet_found->second.missed_no_fuel += queued_tons;
					} else {
						fleet_found->second.missed_no_fleet += queued_tons;
					}
				}
				if (fleet_scale <= 1e-9) {
					continue;
				}
				amount_tons *= fleet_scale;
				ge = amount_tons * factor;
				transport = demo_route_demand(*seller, *buyer.city,
					fleet_same_landmass ? -1 : city_port[static_cast<std::size_t>(si)],
					fleet_same_landmass ? -1 : city_port[buyer.index], amount_tons);
			}

			const double total_cost = amount_tons * unit_cost;
			const double firm_share = std::min(total_cost, firm_cash);
			const double gov_share = std::min(total_cost - firm_share, gov_budget);
			const double savings_share = std::max(
				0.0, total_cost - firm_share - gov_share);
			const double cost = amount_tons * origin_anchor;
			const double freight = unit_freight * amount_tons;
			const double tariff_paid = unit_tariff * amount_tons;

			buyer.city->firm_cash -= firm_share;
			buyer.city->government_budget -= gov_share;
			buyer.city->pop.savings = std::max(
				0.0, buyer.city->pop.savings - savings_share);
			seller->firm_cash += cost;
			buyer.city->service_cash += freight;
			buyer.city->freight_revenue += freight;
			// Tarifa: paga pelo comprador e recebida pelo próprio governo
			// (dinheiro conservado; receita publicada no status).
			buyer.city->government_budget += tariff_paid;
			buyer.city->tariff_revenue += tariff_paid;

			seller->markets[product].stock -= amount_tons;
			seller->markets[product].desired += amount_tons;
			seller->markets[product].demand += amount_tons;
			seller->markets[product].sold += amount_tons;
			seller->revenue_by_product[product] += cost;
			seller->exported[product] += amount_tons;
			seller->export_value[product] += cost;

			buyer.city->imported[product] += amount_tons;
			buyer.city->import_value[product] += cost + freight;

			const bool same_landmass = seller->landmass == buyer.city->landmass;

			// Pedido de comércio para os comboios 3D e o HUD. Na mesma massa de
			// terra vai de caminhão em linha reta; entre massas, o caminhão
			// termina no porto nacional, o navio liga os dois cais e outro
			// caminhão entrega na cidade (infraestrutura nacional: o porto serve
			// a nação toda, não só a cidade-sede).
			TradeOrder order;
			order.from_city = seller->id;
			order.to_city = buyer.city->id;
			order.from_nation = seller->nation;
			order.to_nation = buyer.city->nation;
			const double lane_km = distance_km(seller->center, buyer.city->center);
			order.scope = (seller->nation == buyer.city->nation
				|| lane_km <= kRegionalRadiusKm)
				? MarketScope::Regional : MarketScope::International;
			order.product = product;
			order.amount = amount_tons;
			order.origin_price = origin_anchor
				/ std::max(1e-9, demo_fx_rate(seller->nation));
			order.origin_price_anchor = origin_anchor;
			order.tariff = tariff_paid;
			order.delivered_price = offer.delivered;
			order.fx_rate = demo_fx_rate(buyer.city->nation);
			order.seller_fx_rate = demo_fx_rate(seller->nation);
			order.buyer_currency = demo_currency_code(buyer.city->nation);
			order.seller_currency = demo_currency_code(seller->nation);
			// Cotação que forma o preço interno na ÂNCORA: valor CIF do BEM
			// (origem efetiva + tarifa), sem o frete-serviço. O frete fica de
			// fora de propósito — ele é um serviço doméstico do comprador (pago
			// ao `service_cash` dele, contado à parte em `import_value`), não
			// valor de mercado do bem. Incluí-lo levava qualquer importador
			// remoto ao teto de 3× o preço-base e triplicava o custo alimentar
			// em EGY/MRT/SDN (365 d, `economy_world_test`).
			buyer.city->import_price[product] = origin_anchor + unit_tariff;

			double total_transit = 0.0;
			double total_freight_cost = 0.0;
			double port_delay_days = 0.0;
			if (same_landmass) {
				Leg leg = make_leg(TransportMode::Truck, seller->id, buyer.city->id,
					seller->center, buyer.city->center, amount_tons);
				total_transit = leg.transit_hours;
				total_freight_cost = leg.cost;
				order.legs.push_back(std::move(leg));
			} else {
				const int seller_port_index = city_port[static_cast<std::size_t>(si)];
				const int buyer_port_index = city_port[buyer.index];
				// A oferta só entra na lista com os dois portos (ver o bloco de
				// ofertas); checar aqui é rede de segurança, não caminho normal.
				if (seller_port_index >= 0 && buyer_port_index >= 0) {
					Port& seller_port =
						demo_ports_[static_cast<std::size_t>(seller_port_index)];
					Port& buyer_port =
						demo_ports_[static_cast<std::size_t>(buyer_port_index)];
					// Cidade -> porto do vendedor (caminhão).
					if (distance_km(seller->center, seller_port.center) > 1.0) {
						Leg leg = make_leg(TransportMode::Truck, seller->id,
							seller_port.id, seller->center, seller_port.center, amount_tons);
						total_transit += leg.transit_hours;
						total_freight_cost += leg.cost;
						order.legs.push_back(std::move(leg));
					}
					// Porto -> porto (navio): a única travessia marítima.
					{
						Leg leg = make_leg(TransportMode::Ship, seller_port.id,
							buyer_port.id, seller_port.center, buyer_port.center, amount_tons);
						total_transit += leg.transit_hours;
						total_freight_cost += leg.cost;
						order.legs.push_back(std::move(leg));
					}
					// Porto do comprador -> cidade (caminhão).
					if (distance_km(buyer_port.center, buyer.city->center) > 1.0) {
						Leg leg = make_leg(TransportMode::Truck, buyer_port.id,
							buyer.city->id, buyer_port.center, buyer.city->center, amount_tons);
						total_transit += leg.transit_hours;
						total_freight_cost += leg.cost;
						order.legs.push_back(std::move(leg));
					}
					// Transbordo nos dois cais + fila do dia. O atraso de fila
					// atrasa a CHEGADA em dias inteiros; o demurrage é pago pelo
					// orçamento público do comprador ao setor de frete dele
					// (dinheiro conservado; sem verba, a fila só atrasa).
					total_transit += 2.0 * kDemoPortHandlingHours;
					double demurrage = 0.0;
					port_delay_days +=
						demo_port_charge(seller_port, amount_tons, demurrage);
					port_delay_days +=
						demo_port_charge(buyer_port, amount_tons, demurrage);
					const double demurrage_paid = std::min(
						demurrage, std::max(0.0, buyer.city->government_budget));
					buyer.city->government_budget -= demurrage_paid;
					buyer.city->service_cash += demurrage_paid;
					buyer.city->freight_revenue += demurrage_paid;
					total_freight_cost += demurrage_paid;
					total_transit += port_delay_days * kHoursPerDay;
				}
			}
			order.transit_hours = total_transit;
			order.freight_cost = total_freight_cost;
			demo_last_orders_.push_back(std::move(order));

			const int arrival_day =
				day_ + 1 + static_cast<int>(std::ceil(port_delay_days));
			// F4: reserva os veículos até a chegada e debita o combustível.
			transport = demo_route_demand(*seller, *buyer.city,
				fleet_same_landmass ? -1 : city_port[static_cast<std::size_t>(si)],
				fleet_same_landmass ? -1 : city_port[buyer.index], amount_tons);
			demo_fleet_reserve(seller->nation, transport);
			DemoShipment shipment;
			shipment.to_city = buyer.city->id;
			shipment.product = product;
			shipment.amount = amount_tons;
			shipment.arrival_day = arrival_day;
			shipment.fleet_nation = seller->nation;
			// Caminhões fazem só a coleta/entrega no cais e voltam no mesmo
			// dia; a travessia marítima fica com o navio. Sem isso, o caminhão
			// ficava preso até a chegada e um navio de 2.000 t exigia 80
			// caminhões ocupados por toda a viagem — nenhuma nação pequena
			// conseguia embarcar (ver demo_tick_fleet).
			shipment.fleet_trucks = fleet_same_landmass ? transport.trucks : 0.0;
			shipment.fleet_ships = transport.ships;
			shipment.fleet_planes = transport.planes;
			shipment.fuel = transport.fuel;
			demo_in_transit_.push_back(std::move(shipment));

			sellable_ge[si] -= ge;
			if (sellable_ge[si] <= 1e-9) {
				// Esgotou no meio do dia: sai da contagem usada para limitar a
				// expansão do anel dos próximos compradores.
				--sellers_left;
			}
			remaining -= ge;
			++shipments;
		}
	}
}

void EconomySim::demo_trade_goods() {
	// Comércio inter-cidades de TODOS os bens não alimentares (insumos e bens
	// industriais). A cesta alimentar continua no demo_trade_food (validado);
	// aqui a demanda é a necessidade DIÁRIA das construções locais (receitas ou
	// insumo legado). Reaproveita a mesma malha de rotas/frete/portos do
	// comércio de alimento: caminhão na mesma massa de terra, navio entre
	// massas com porto nacional nas duas pontas.
	if (demo_cities_.empty() || demo_products_ <= 0) {
		return;
	}
	const std::size_t city_count = demo_cities_.size();
	// Necessidade diária de insumo por cidade/produto (capacidade nominal,
	// não a produção do dia: o estoque-alvo é o que mantém a fábrica operando).
	std::vector<double> city_demand(city_count * static_cast<std::size_t>(demo_products_), 0.0);
	for (std::size_t ci = 0; ci < city_count; ++ci) {
		const DemoCity& city = demo_cities_[ci];
		for (const int bi : demo_buildings_by_city_[ci]) {
			const DemoBuilding& building = demo_buildings_[static_cast<std::size_t>(bi)];
			const double capacity = building.capacity * building.output_scale;
			if (capacity <= 0.0) {
				continue;
			}
			if (building.recipe >= 0
					&& building.recipe < static_cast<int>(demo_recipes_.size())) {
				const DemoRecipe& recipe =
					demo_recipes_[static_cast<std::size_t>(building.recipe)];
				for (int slot = 0; slot < recipe.input_count; ++slot) {
					const int product = recipe.inputs[slot];
					if (product < 0 || product >= demo_products_) {
						continue;
					}
					city_demand[ci * static_cast<std::size_t>(demo_products_)
						+ static_cast<std::size_t>(product)] +=
						capacity * recipe.input_qty[slot] / recipe.yield;
				}
			} else if (building.input_product >= 0
					&& building.input_product < demo_products_) {
				city_demand[ci * static_cast<std::size_t>(demo_products_)
					+ static_cast<std::size_t>(building.input_product)] +=
					capacity * building.input_per_output;
			}
		}
		(void)city;
	}
	// Trânsito a caminho da cidade, por produto.
	std::vector<double> in_transit(city_demand.size(), 0.0);
	for (const DemoShipment& shipment : demo_in_transit_) {
		const auto found = demo_city_lookup_.find(shipment.to_city);
		if (found == demo_city_lookup_.end()) {
			continue;
		}
		const std::size_t offset =
			static_cast<std::size_t>(found->second) * static_cast<std::size_t>(demo_products_)
			+ static_cast<std::size_t>(shipment.product);
		if (offset < in_transit.size()) {
			in_transit[offset] += shipment.amount;
		}
	}
	// Porto nacional de cada cidade (mesma nação + massa de terra). Sem porto,
	// a cidade só comercia por caminhão dentro da própria massa.
	std::vector<int> city_port(city_count, -1);
	if (!demo_ports_.empty()) {
		std::unordered_map<std::string, std::vector<int>> ports_by_region;
		ports_by_region.reserve(demo_ports_.size() * 2);
		for (std::size_t pi = 0; pi < demo_ports_.size(); ++pi) {
			const Port& port = demo_ports_[pi];
			ports_by_region[port.nation + '\n' + port.landmass]
				.push_back(static_cast<int>(pi));
		}
		for (std::size_t ci = 0; ci < city_count; ++ci) {
			const DemoCity& city = demo_cities_[ci];
			const auto found = ports_by_region.find(city.nation + '\n' + city.landmass);
			if (found == ports_by_region.end()) {
				continue;
			}
			int best_port = -1;
			double best_distance = 0.0;
			for (const int pi : found->second) {
				const double distance = distance_km(
					city.center, demo_ports_[static_cast<std::size_t>(pi)].center);
				if (best_port < 0 || distance < best_distance) {
					best_port = pi;
					best_distance = distance;
				}
			}
			city_port[ci] = best_port;
		}
	}
	// Grade espacial (~250 km por célula) para os K vizinhos.
	const double cell = kDemoTradeCellMapUnits;
	const int grid_w = std::max(1, static_cast<int>(std::ceil(kMapWidth / cell)));
	const int grid_h = std::max(1, static_cast<int>(std::ceil(kMapHeight / cell)));
	std::vector<std::vector<int>> grid(static_cast<std::size_t>(grid_w * grid_h));
	auto cell_index = [cell, grid_w, grid_h](const MapPoint& point) {
		const int ix = std::clamp(static_cast<int>(point.x / cell), 0, grid_w - 1);
		const int iy = std::clamp(static_cast<int>(point.y / cell), 0, grid_h - 1);
		return iy * grid_w + ix;
	};
	for (std::size_t ci = 0; ci < city_count; ++ci) {
		grid[static_cast<std::size_t>(cell_index(demo_cities_[ci].center))]
			.push_back(static_cast<int>(ci));
	}
	const int k_neighbors = std::max(1, demo_trade_neighbors_);
	const int k_external = std::max(0, demo_trade_external_);
	std::size_t shipments = 0;
	// Itera só os produtos com demanda E oferta no dia (nunca 114 × cidades).
	for (int product = 0; product < demo_products_ && shipments < kDemoMaxShipmentsPerDay;
			++product) {
		if (demo_food_calories(product) > 0.0) {
			continue;  // cesta alimentar: demo_trade_food
		}
		struct Buyer {
			int city_index;
			double gap;
			double daily;
		};
		struct Seller {
			int city_index;
			double sellable;
			double price;
		};
		std::vector<Buyer> buyers;
		std::vector<Seller> sellers;
		for (std::size_t ci = 0; ci < city_count; ++ci) {
			const std::size_t offset = ci * static_cast<std::size_t>(demo_products_)
				+ static_cast<std::size_t>(product);
			const double daily = city_demand[offset];
			const DemoMarket& market = demo_cities_[ci].markets[product];
			const double stock = std::max(0.0, market.stock);
			if (daily > 1e-9) {
				const double target = daily * demo_inventory_cover_days_;
				const double gap = target - (stock + in_transit[offset]);
				if (gap > 1e-9) {
					buyers.push_back({static_cast<int>(ci), gap, daily});
				}
			}
			// Vendedor: estoque acima da própria necessidade de 1 dia.
			const double own_need = daily;
			const double sellable = stock - own_need;
			if (sellable > 1e-9 && market.price > 0.0) {
				sellers.push_back({static_cast<int>(ci), sellable,
					market.price * demo_competitiveness(demo_cities_[ci].nation)});
			}
		}
		if (buyers.empty() || sellers.empty()) {
			continue;
		}
		std::stable_sort(buyers.begin(), buyers.end(),
			[](const Buyer& left, const Buyer& right) {
				return left.gap / std::max(1e-9, left.daily)
					> right.gap / std::max(1e-9, right.daily);
			});
		// Ofertas globais mais baratas: o "mercado externo" do comprador.
		std::vector<int> cheapest(sellers.size());
		for (std::size_t i = 0; i < cheapest.size(); ++i) {
			cheapest[i] = static_cast<int>(i);
		}
		std::stable_sort(cheapest.begin(), cheapest.end(),
			[&sellers](int left, int right) {
				return sellers[static_cast<std::size_t>(left)].price
					< sellers[static_cast<std::size_t>(right)].price;
			});
		for (Buyer& buyer : buyers) {
			if (shipments >= kDemoMaxShipmentsPerDay) {
				break;
			}
			DemoCity& buyer_city = demo_cities_[static_cast<std::size_t>(buyer.city_index)];
			const std::size_t buyer_offset =
				static_cast<std::size_t>(buyer.city_index) * static_cast<std::size_t>(demo_products_)
				+ static_cast<std::size_t>(product);
			double remaining = std::max(0.0, buyer.gap);
			if (remaining <= 1e-9) {
				continue;
			}
			// Candidatos: K vizinhos vendedores + M ofertas mais baratas.
			std::vector<int> candidates;
			const int bx = std::clamp(
				static_cast<int>(buyer_city.center.x / cell), 0, grid_w - 1);
			const int by = std::clamp(
				static_cast<int>(buyer_city.center.y / cell), 0, grid_h - 1);
			const int max_radius = std::min(8,
				std::max(std::max(bx, grid_w - 1 - bx), std::max(by, grid_h - 1 - by)));
			int cells_budget = std::min(grid_w * grid_h, 512);
			for (int radius = 0; radius <= max_radius && cells_budget > 0; ++radius) {
				if (static_cast<int>(candidates.size()) >= k_neighbors) {
					break;
				}
				for (int ix = bx - radius; ix <= bx + radius; ++ix) {
					for (int iy = by - radius; iy <= by + radius; ++iy) {
						if (ix != bx - radius && ix != bx + radius
								&& iy != by - radius && iy != by + radius) {
							continue;
						}
						if (ix < 0 || ix >= grid_w || iy < 0 || iy >= grid_h
								|| cells_budget <= 0) {
							continue;
						}
						--cells_budget;
						for (const int si : grid[static_cast<std::size_t>(iy * grid_w + ix)]) {
							if (si == buyer.city_index) {
								continue;
							}
							const std::size_t offset =
								static_cast<std::size_t>(si) * static_cast<std::size_t>(demo_products_)
								+ static_cast<std::size_t>(product);
							if (city_demand[offset] >= 0.0
									&& demo_cities_[static_cast<std::size_t>(si)]
										.markets[product].stock
										- city_demand[offset] > 1e-9) {
								candidates.push_back(si);
							}
						}
					}
				}
			}
			const int external_limit = std::min(k_external,
				static_cast<int>(cheapest.size()));
			for (int i = 0; i < external_limit; ++i) {
				const int si = sellers[static_cast<std::size_t>(
					cheapest[static_cast<std::size_t>(i)])].city_index;
				if (si != buyer.city_index) {
					candidates.push_back(si);
				}
			}
			std::sort(candidates.begin(), candidates.end());
			candidates.erase(std::unique(candidates.begin(), candidates.end()),
				candidates.end());
			struct Offer {
				int seller_index;
				double unit_cost;      // âncora: origem + frete + tarifa (ordena)
				double unit_freight;
				double origin_anchor;  // origem efetiva na âncora
				double unit_tariff;
				double delivered;      // moeda do comprador (exibição)
			};
			std::vector<Offer> offers;
			offers.reserve(candidates.size());
			for (const int si : candidates) {
				DemoCity& seller = demo_cities_[static_cast<std::size_t>(si)];
				const std::size_t offset =
					static_cast<std::size_t>(si) * static_cast<std::size_t>(demo_products_)
					+ static_cast<std::size_t>(product);
				if (seller.markets[product].stock - city_demand[offset] <= 1e-9) {
					continue;
				}
				double unit_freight = 0.0;
				if (seller.landmass == buyer_city.landmass) {
					unit_freight = demo_leg_unit_freight(TransportMode::Truck,
						distance_km(seller.center, buyer_city.center));
				} else {
					const int seller_port = city_port[static_cast<std::size_t>(si)];
					const int buyer_port = city_port[static_cast<std::size_t>(buyer.city_index)];
					if (seller_port < 0 || buyer_port < 0) {
						continue;
					}
					const Port& sp = demo_ports_[static_cast<std::size_t>(seller_port)];
					const Port& bp = demo_ports_[static_cast<std::size_t>(buyer_port)];
					unit_freight =
						demo_leg_unit_freight(TransportMode::Truck,
							distance_km(seller.center, sp.center))
						+ demo_leg_unit_freight(TransportMode::Ship,
							distance_km(sp.center, bp.center))
						+ demo_leg_unit_freight(TransportMode::Truck,
							distance_km(bp.center, buyer_city.center));
				}
				const double origin_anchor = seller.markets[product].price
					* demo_competitiveness(seller.nation);
				const double tariff_rate = demo_tariff_rate(
					buyer_city.nation, seller.nation, product);
				const double unit_tariff =
					(origin_anchor + unit_freight) * tariff_rate;
				const double delivered_anchor =
					origin_anchor + unit_freight + unit_tariff;
				const double buyer_fx = demo_fx_rate(buyer_city.nation);
				const double delivered =
					delivered_anchor / std::max(1e-9, buyer_fx);
				offers.push_back({si, delivered_anchor,
					unit_freight, origin_anchor, unit_tariff,
					delivered});
			}
			std::stable_sort(offers.begin(), offers.end(),
				[](const Offer& left, const Offer& right) {
					return left.unit_cost < right.unit_cost;
				});
			for (const Offer& offer : offers) {
				if (remaining <= 1e-9 || shipments >= kDemoMaxShipmentsPerDay) {
					break;
				}
				DemoCity& seller = demo_cities_[static_cast<std::size_t>(offer.seller_index)];
				const std::size_t seller_offset =
					static_cast<std::size_t>(offer.seller_index) * static_cast<std::size_t>(demo_products_)
					+ static_cast<std::size_t>(product);
				const double sellable = std::max(0.0,
					seller.markets[product].stock - city_demand[seller_offset]);
				if (sellable <= 1e-9) {
					continue;
				}
				// Capacidade de transporte do comprador (mesmo teto do alimento).
				double imported_total = 0.0;
				for (int p = 0; p < demo_products_; ++p) {
					imported_total += buyer_city.imported[p];
				}
				const double room = std::max(0.0,
					buyer_city.simulation_population * kDemoTradeCapacityPerCapita
					- imported_total);
				double amount = std::min(remaining, std::min(sellable, room));
				if (amount <= 1e-9) {
					continue;
				}
				// Pagamento: firma doméstica (comprador) -> firma vendedora +
				// frete ao serviço do comprador. Governo cobre o que a firma não
				// puder (insumo essencial), conservando o dinheiro.
				const double unit_cost = offer.unit_cost;
				const double payable = std::max(0.0, buyer_city.firm_cash)
					+ std::max(0.0, buyer_city.government_budget);
				if (amount * unit_cost > payable) {
					amount = unit_cost > 0.0 ? payable / unit_cost : 0.0;
				}
				if (amount <= 1e-9) {
					continue;
				}
				// F4: frota livre + combustível da nação vendedora. Sem veículo
				// ou sem fuel a carga não sai (a fila do dia alimenta a reposição
				// do governo); com capacidade parcial, embarca proporcional.
				const bool fleet_same_landmass =
					seller.landmass == buyer_city.landmass;
				DemoTransportDemand transport = demo_route_demand(seller, buyer_city,
					fleet_same_landmass ? -1 : city_port[static_cast<std::size_t>(
						offer.seller_index)],
					fleet_same_landmass ? -1 : city_port[static_cast<std::size_t>(
						buyer.city_index)], amount);
				bool fuel_limited = false;
				const double fleet_scale = demo_fleet_capacity_scale(
					seller.nation, transport, fuel_limited);
				if (fleet_scale < 1.0) {
					const double queued_tons = amount * (1.0 - fleet_scale);
					const auto fleet_found = demo_fleets_.find(seller.nation);
					if (fleet_found != demo_fleets_.end()) {
						fleet_found->second.queue_today += queued_tons;
						if (fuel_limited) {
							fleet_found->second.missed_no_fuel += queued_tons;
						} else {
							fleet_found->second.missed_no_fleet += queued_tons;
						}
					}
					if (fleet_scale <= 1e-9) {
						continue;
					}
					amount *= fleet_scale;
					transport = demo_route_demand(seller, buyer_city,
						fleet_same_landmass ? -1 : city_port[static_cast<std::size_t>(
							offer.seller_index)],
						fleet_same_landmass ? -1 : city_port[static_cast<std::size_t>(
							buyer.city_index)], amount);
				}
				const double total_cost = amount * unit_cost;
				const double firm_share = std::min(total_cost,
					std::max(0.0, buyer_city.firm_cash));
				const double gov_share = std::min(total_cost - firm_share,
					std::max(0.0, buyer_city.government_budget));
				const double paid = firm_share + gov_share;
				if (paid <= 1e-9) {
					continue;
				}
				const double goods_value = amount * offer.origin_anchor;
				const double freight = amount * offer.unit_freight;
				const double tariff_paid = amount * offer.unit_tariff;
				buyer_city.firm_cash -= firm_share;
				buyer_city.government_budget -= gov_share;
				seller.firm_cash += goods_value;
				buyer_city.service_cash += freight;
				buyer_city.freight_revenue += freight;
				buyer_city.government_budget += tariff_paid;
				buyer_city.tariff_revenue += tariff_paid;

				seller.markets[product].stock -= amount;
				seller.markets[product].desired += amount;
				seller.markets[product].demand += amount;
				seller.markets[product].sold += amount;
				seller.revenue_by_product[product] += goods_value;
				seller.exported[product] += amount;
				seller.export_value[product] += goods_value;
				buyer_city.imported[product] += amount;
				buyer_city.import_value[product] += goods_value + freight;

				TradeOrder order;
				order.from_city = seller.id;
				order.to_city = buyer_city.id;
				order.from_nation = seller.nation;
				order.to_nation = buyer_city.nation;
				const double lane_km = distance_km(seller.center, buyer_city.center);
				order.scope = (seller.nation == buyer_city.nation
					|| lane_km <= kRegionalRadiusKm)
					? MarketScope::Regional : MarketScope::International;
				order.product = product;
				order.amount = amount;
				order.origin_price = offer.origin_anchor
					/ std::max(1e-9, demo_fx_rate(seller.nation));
				order.origin_price_anchor = offer.origin_anchor;
				order.tariff = tariff_paid;
				order.delivered_price = offer.delivered;
				order.fx_rate = demo_fx_rate(buyer_city.nation);
				order.seller_fx_rate = demo_fx_rate(seller.nation);
				order.buyer_currency = demo_currency_code(buyer_city.nation);
				order.seller_currency = demo_currency_code(seller.nation);
				// F5: a cotação que forma o preço interno é o CIF do bem na
				// âncora (origem efetiva + tarifa), sem o frete-serviço — mesma
				// regra do comércio de comida.
				buyer_city.import_price[product] = offer.origin_anchor
					+ offer.unit_tariff;
				double total_transit = 0.0;
				double total_freight_cost = 0.0;
				double port_delay_days = 0.0;
				if (seller.landmass == buyer_city.landmass) {
					Leg leg = make_leg(TransportMode::Truck, seller.id, buyer_city.id,
						seller.center, buyer_city.center, amount);
					total_transit = leg.transit_hours;
					total_freight_cost = leg.cost;
					order.legs.push_back(std::move(leg));
				} else {
					const int seller_port = city_port[static_cast<std::size_t>(
						offer.seller_index)];
					const int buyer_port = city_port[static_cast<std::size_t>(buyer.city_index)];
					if (seller_port >= 0 && buyer_port >= 0) {
						Port& sp = demo_ports_[static_cast<std::size_t>(seller_port)];
						Port& bp = demo_ports_[static_cast<std::size_t>(buyer_port)];
						if (distance_km(seller.center, sp.center) > 1.0) {
							Leg leg = make_leg(TransportMode::Truck, seller.id, sp.id,
								seller.center, sp.center, amount);
							total_transit += leg.transit_hours;
							total_freight_cost += leg.cost;
							order.legs.push_back(std::move(leg));
						}
						{
							Leg leg = make_leg(TransportMode::Ship, sp.id, bp.id,
								sp.center, bp.center, amount);
							total_transit += leg.transit_hours;
							total_freight_cost += leg.cost;
							order.legs.push_back(std::move(leg));
						}
						if (distance_km(bp.center, buyer_city.center) > 1.0) {
							Leg leg = make_leg(TransportMode::Truck, bp.id, buyer_city.id,
								bp.center, buyer_city.center, amount);
							total_transit += leg.transit_hours;
							total_freight_cost += leg.cost;
							order.legs.push_back(std::move(leg));
						}
						total_transit += 2.0 * kDemoPortHandlingHours;
						double demurrage = 0.0;
						port_delay_days += demo_port_charge(sp, amount, demurrage);
						port_delay_days += demo_port_charge(bp, amount, demurrage);
						const double demurrage_paid = std::min(demurrage,
							std::max(0.0, buyer_city.government_budget));
						buyer_city.government_budget -= demurrage_paid;
						buyer_city.service_cash += demurrage_paid;
						buyer_city.freight_revenue += demurrage_paid;
						total_freight_cost += demurrage_paid;
						total_transit += port_delay_days * kHoursPerDay;
					}
				}
				order.transit_hours = total_transit;
				order.freight_cost = total_freight_cost;
				demo_last_orders_.push_back(std::move(order));
				const int arrival_day =
					day_ + 1 + static_cast<int>(std::ceil(port_delay_days));
				// F4: reserva os veículos até a chegada e debita o combustível.
				transport = demo_route_demand(seller, buyer_city,
					fleet_same_landmass ? -1 : city_port[static_cast<std::size_t>(
						offer.seller_index)],
					fleet_same_landmass ? -1 : city_port[static_cast<std::size_t>(
						buyer.city_index)], amount);
				demo_fleet_reserve(seller.nation, transport);
				DemoShipment shipment;
				shipment.to_city = buyer_city.id;
				shipment.product = product;
				shipment.amount = amount;
				shipment.arrival_day = arrival_day;
				shipment.fleet_nation = seller.nation;
				// Caminhões de coleta/entrega voltam no mesmo dia (ver
				// demo_trade_food); o navio fica reservado até a chegada.
				shipment.fleet_trucks = fleet_same_landmass ? transport.trucks : 0.0;
				shipment.fleet_ships = transport.ships;
				shipment.fleet_planes = transport.planes;
				shipment.fuel = transport.fuel;
				demo_in_transit_.push_back(std::move(shipment));
				in_transit[buyer_offset] += amount;
				remaining -= amount;
				++shipments;
			}
		}
	}
}

// ---------------------------------------------------------------------------
// F4 — frota nacional e combustível.
//
// Contrato: cada trecho de um pedido exige (a) veículo livre da nação
// VENDEDORA na capacidade do ModeSpec (caminhão 25 t, navio 2.000 t, avião 20 t)
// e (b) combustível no estoque nacional, consumido por t·km. Sem os dois, a
// carga não embarca; a diferença entra em `queue_today` e o governo repõe frota
// e combustível na manutenção mensal. Não há número fora do dado: a frota
// inicial é derivada de população/PIB (`source: derived`), o combustível é um
// bem físico (fuel, order 47) e os veículos são os bens finais da F2
// (truck/cargo_ship/cargo_plane).
// ---------------------------------------------------------------------------

double EconomySim::demo_nation_sim_population(const std::string& nation) const {
	const auto found = demo_cities_by_nation_.find(nation);
	if (found != demo_cities_by_nation_.end()) {
		double total = 0.0;
		for (const int index : found->second) {
			total += demo_cities_[static_cast<std::size_t>(index)].simulation_population;
		}
		return total;
	}
	double total = 0.0;
	for (const DemoCity& city : demo_cities_) {
		if (city.nation == nation) {
			total += city.simulation_population;
		}
	}
	return total;
}

double EconomySim::demo_nation_income_index(const std::string& nation) const {
	// PIB PPP per capita já carregado por cidade (external_income_index),
	// ponderado por população: nações ricas têm frota maior por habitante.
	double weighted = 0.0;
	double population = 0.0;
	for (const DemoCity& city : demo_cities_) {
		if (city.nation != nation || city.simulation_population <= 0.0) {
			continue;
		}
		weighted += city.external_income_index * city.simulation_population;
		population += city.simulation_population;
	}
	if (population <= 0.0) {
		return 1.0;
	}
	return std::clamp(weighted / population, 0.10, 8.0);
}

bool EconomySim::demo_nation_food_secure(const std::string& nation) const {
	const auto found = demo_cities_by_nation_.find(nation);
	if (found == demo_cities_by_nation_.end()) {
		return true;
	}
	double stock_ge = 0.0;
	double need = 0.0;
	for (const int index : found->second) {
		const DemoCity& city = demo_cities_[static_cast<std::size_t>(index)];
		// `pop.need` é zerado no reset do dia antes deste tick; a fórmula
		// abaixo é a mesma usada em demo_consume_population.
		need += std::max(0.0, city.simulation_population * kDemoNeedPerCapita);
		for (int slot = 0; slot < static_cast<int>(demo_food_products_.size()); ++slot) {
			const int product = demo_food_product(slot);
			const double factor = demo_food_calories(product);
			if (product >= 0 && product < demo_products_ && factor > 0.0) {
				stock_ge += std::max(0.0, city.markets[product].stock) * factor;
			}
		}
	}
	if (need <= 1e-9) {
		return true;
	}
	return stock_ge >= need * kDemoFleetFoodSecureDays;
}

void EconomySim::demo_init_fleets() {	demo_fleet_product_fuel_ = demo_product_index(String("fuel"));
	demo_fleet_product_truck_ = demo_product_index(String("truck"));
	demo_fleet_product_ship_ = demo_product_index(String("cargo_ship"));
	demo_fleet_product_plane_ = demo_product_index(String("cargo_plane"));
	demo_fleet_product_parts_ = demo_product_index(String("spare_parts"));
	demo_fleets_.clear();
	if (demo_cities_.empty()) {
		return;
	}
	// Ordem determinística por primeira aparição da nação (ordem das cidades).
	std::vector<std::string> nations;
	std::unordered_map<std::string, char> seen;
	seen.reserve(demo_cities_.size() * 2);
	for (const DemoCity& city : demo_cities_) {
		if (seen.emplace(city.nation, 1).second) {
			nations.push_back(city.nation);
		}
	}
	for (const std::string& nation : nations) {
		const double population = demo_nation_sim_population(nation);
		const double income = demo_nation_income_index(nation);
		const double factor = std::clamp(0.6 + 0.4 * income, 0.6, 2.0);
		DemoFleet fleet;
		fleet.trucks = std::max(kDemoFleetMinTrucks,
			std::ceil(population * kDemoFleetTrucksPerCapita * factor));
		fleet.ships = std::max(kDemoFleetMinShips,
			std::ceil(population * kDemoFleetShipsPerCapita * factor));
		fleet.planes = std::ceil(population * kDemoFleetPlanesPerCapita * factor);
		fleet.target_trucks = fleet.trucks;
		fleet.target_ships = fleet.ships;
		fleet.target_planes = fleet.planes;
		const double fuel_day = fleet.trucks * kDemoFleetFuelPerTruckDay
			+ fleet.ships * kDemoFleetFuelPerShipDay
			+ fleet.planes * kDemoFleetFuelPerPlaneDay;
		fleet.fuel_target = std::max(fuel_day * kDemoFleetFuelDays,
			population * kDemoFleetFuelPerCapita);
		fleet.fuel = fleet.fuel_target;
		fleet.spare_parts = kDemoFleetPartsReserveMonths * (fleet.trucks * kDemoFleetPartsPerTruckMonth
			+ fleet.ships * kDemoFleetPartsPerShipMonth
			+ fleet.planes * kDemoFleetPartsPerPlaneMonth);
		fleet.derived = true;
		demo_fleets_.emplace(nation, fleet);
	}
	demo_fleet_order_ = nations;
}

EconomySim::DemoTransportDemand EconomySim::demo_route_demand(const DemoCity& seller,
		const DemoCity& buyer, int seller_port, int buyer_port, double tons) const {
	DemoTransportDemand demand;
	if (tons <= 1e-9) {
		return demand;
	}
	auto add_leg = [&demand, tons](TransportMode mode, double km) {
		if (km <= 1e-6) {
			return;
		}
		const ModeSpec& mode_spec = spec(mode);
		const double vehicles = std::ceil(tons
			/ std::max(1e-9, mode_spec.batch_capacity_tons));
		demand.fuel += tons * km * mode_spec.fuel_per_ton_km;
		switch (mode) {
			case TransportMode::Ship:
				demand.ships += vehicles;
				demand.distance_ship_km += km;
				break;
			case TransportMode::Plane:
				demand.planes += vehicles;
				break;
			case TransportMode::Truck:
			default:
				demand.trucks += vehicles;
				demand.distance_truck_km += km;
				break;
		}
	};
	if (seller.landmass == buyer.landmass) {
		add_leg(TransportMode::Truck, distance_km(seller.center, buyer.center));
		return demand;
	}
	if (seller_port < 0 || buyer_port < 0
			|| seller_port >= static_cast<int>(demo_ports_.size())
			|| buyer_port >= static_cast<int>(demo_ports_.size())) {
		return demand;
	}
	const Port& seller_city_port = demo_ports_[static_cast<std::size_t>(seller_port)];
	const Port& buyer_city_port = demo_ports_[static_cast<std::size_t>(buyer_port)];
	add_leg(TransportMode::Truck,
		distance_km(seller.center, seller_city_port.center));
	add_leg(TransportMode::Ship,
		distance_km(seller_city_port.center, buyer_city_port.center));
	add_leg(TransportMode::Truck,
		distance_km(buyer_city_port.center, buyer.center));
	return demand;
}

double EconomySim::demo_fleet_capacity_scale(const std::string& nation,
		const DemoTransportDemand& demand, bool& fuel_limited) const {
	fuel_limited = false;
	if (demand.trucks <= 1e-9 && demand.ships <= 1e-9
			&& demand.planes <= 1e-9 && demand.fuel <= 1e-9) {
		return 1.0;
	}
	const auto found = demo_fleets_.find(nation);
	if (found == demo_fleets_.end()) {
		// Mundo sem frota declarada (save antigo/benchmark): não bloqueia.
		return 1.0;
	}
	const DemoFleet& fleet = found->second;
	double scale = 1.0;
	auto apply = [&scale](double free_units, double need) {
		if (need > 1e-9) {
			scale = std::min(scale, std::max(0.0, free_units) / need);
		}
	};
	// Capacidade efetiva = veículos × saúde − ocupados em trânsito.
	apply(fleet.trucks * fleet.health[0] - fleet.trucks_busy, demand.trucks);
	apply(fleet.ships * fleet.health[1] - fleet.ships_busy, demand.ships);
	apply(fleet.planes * fleet.health[2] - fleet.planes_busy, demand.planes);
	if (demand.fuel > 1e-9) {
		const double fuel_scale = std::max(0.0, fleet.fuel) / demand.fuel;
		if (fuel_scale < scale) {
			scale = fuel_scale;
			fuel_limited = true;
		}
	}
	return std::clamp(scale, 0.0, 1.0);
}

void EconomySim::demo_fleet_reserve(const std::string& nation,
		const DemoTransportDemand& demand) {
	const auto found = demo_fleets_.find(nation);
	if (found == demo_fleets_.end()) {
		return;
	}
	DemoFleet& fleet = found->second;
	fleet.trucks_busy += demand.trucks;
	fleet.ships_busy += demand.ships;
	fleet.planes_busy += demand.planes;
	fleet.fuel = std::max(0.0, fleet.fuel - demand.fuel);
	fleet.fuel_used_today += demand.fuel;
	fleet.fuel_used_total += demand.fuel;
}

void EconomySim::demo_fleet_release(const std::string& nation,
		const DemoTransportDemand& demand) {
	const auto found = demo_fleets_.find(nation);
	if (found == demo_fleets_.end()) {
		return;
	}
	DemoFleet& fleet = found->second;
	fleet.trucks_busy = std::max(0.0, fleet.trucks_busy - demand.trucks);
	fleet.ships_busy = std::max(0.0, fleet.ships_busy - demand.ships);
	fleet.planes_busy = std::max(0.0, fleet.planes_busy - demand.planes);
}

double EconomySim::demo_fleet_fuel_price() const {
	if (demo_fleet_product_fuel_ >= 0
			&& demo_fleet_product_fuel_ < demo_products_) {
		return demo_base_price(demo_fleet_product_fuel_);
	}
	// goods.json: base_price do fuel (fallback antes de demo_set_goods).
	return 43.5;
}

double EconomySim::demo_leg_unit_freight(TransportMode mode,
		double distance_km_value) const {
	const ModeSpec& mode_spec = spec(mode);
	return distance_km_value * (mode_spec.cost_per_ton_km
		+ mode_spec.fuel_per_ton_km * demo_fleet_fuel_price()
		+ mode_spec.fleet_cost_per_ton_km);
}

double EconomySim::demo_leg_unit_freight_food(TransportMode mode,
		double distance_km_value) const {
	// Só a tarifa: comida não paga o sobrepreço de fuel/frota. O veículo e o
	// combustível continuam exigidos no embarque (demo_route_demand), então a
	// frota segue sendo um gargalo físico — o que muda é quem absorve o custo.
	return distance_km_value * spec(mode).cost_per_ton_km;
}

void EconomySim::demo_tick_fleet() {
	if (demo_fleets_.empty() && !demo_cities_.empty()) {
		demo_init_fleets();
	}
	if (demo_fleets_.empty()) {
		return;
	}
	if (day_ % 30 == 0) {
		// Manutenção mensal: peças, saúde e atrito; depois a reposição do
		// governo. Usa a fila do dia anterior (antes do reset abaixo).
		for (const std::string& nation : demo_fleet_order_) {
			const auto found = demo_fleets_.find(nation);
			if (found == demo_fleets_.end()) {
				continue;
			}
			DemoFleet& fleet = found->second;
			const double parts_need = fleet.trucks * kDemoFleetPartsPerTruckMonth
				+ fleet.ships * kDemoFleetPartsPerShipMonth
				+ fleet.planes * kDemoFleetPartsPerPlaneMonth;
			if (parts_need > 1e-9) {
				const double available = std::max(0.0, fleet.spare_parts);
				const double used = std::min(available, parts_need);
				fleet.spare_parts = available - used;
				fleet.parts_used_total += used;
				const double ratio = used / parts_need;
				for (int mode = 0; mode < 3; ++mode) {
					if (ratio >= 0.999) {
						fleet.health[mode] = std::min(1.0,
							fleet.health[mode] + kDemoFleetHealthRecoveryMonth);
					} else {
						fleet.health[mode] = std::max(kDemoFleetHealthFloor,
							fleet.health[mode]
							- kDemoFleetHealthLossMonth * (1.0 - ratio));
					}
				}
			}
			// Atrito: acidentes/obsolescência consomem 0,2% da frota por mês.
			const double keep = 1.0 - kDemoFleetAttritionMonth;
			fleet.trucks *= keep;
			fleet.ships *= keep;
			fleet.planes *= keep;

			// Reposição do governo: combustível, peças e veículos. A fila do mês
			// acelera a reposição: +50% no alvo e os veículos necessários para
			// escoar a carga barrada em ~10 dias.
			const bool food_secure = demo_nation_food_secure(nation);
			const double fuel_day = fleet.trucks * kDemoFleetFuelPerTruckDay
				+ fleet.ships * kDemoFleetFuelPerShipDay
				+ fleet.planes * kDemoFleetFuelPerPlaneDay;
			fleet.fuel_target = std::max(fleet.fuel_target,
				std::max(fuel_day * kDemoFleetFuelDays,
					demo_nation_sim_population(nation) * kDemoFleetFuelPerCapita));
			const double parts_target = kDemoFleetPartsTargetMonths * parts_need;
			const double urgency = fleet.queue_today > 1e-9 ? 1.5 : 1.0;
			const double queued_trucks = fleet.queue_today / (25.0 * 10.0);
			const double queued_ships = fleet.queue_today / (2000.0 * 10.0);
			const String nation_id(nation.c_str());
			// Sem folga alimentar, o orçamento público fica com a comida: só
			// repõe em emergência (fuel abaixo de 20% do alvo, peças abaixo de
			// um mês). Com folga, a reposição segue o alvo normal.
			const bool fuel_emergency = fleet.fuel < fleet.fuel_target * 0.20;
			const bool parts_emergency = fleet.spare_parts < parts_need;
			if (demo_fleet_product_fuel_ >= 0 && fleet.fuel < fleet.fuel_target
					&& (food_secure || fuel_emergency)) {
				demo_fleet_buy(nation_id, String("fuel"),
					fleet.fuel_target - fleet.fuel, kDemoFleetBudgetShare);
			}
			if (demo_fleet_product_parts_ >= 0 && fleet.spare_parts < parts_target
					&& (food_secure || parts_emergency)) {
				demo_fleet_buy(nation_id, String("spare_parts"),
					parts_target - fleet.spare_parts, kDemoFleetBudgetShare);
			}
			if (!food_secure) {
				continue;
			}
			if (demo_fleet_product_truck_ >= 0) {
				const double target = std::max(fleet.target_trucks, fleet.trucks)
					* urgency + queued_trucks;
				if (fleet.trucks < target) {
					demo_fleet_buy(nation_id, String("truck"), target - fleet.trucks,
						kDemoFleetBudgetShare);
				}
			}
			if (demo_fleet_product_ship_ >= 0) {
				const double target = std::max(fleet.target_ships, fleet.ships)
					* urgency + queued_ships;
				if (fleet.ships < target) {
					demo_fleet_buy(nation_id, String("cargo_ship"), target - fleet.ships,
						kDemoFleetBudgetShare);
				}
			}
			if (demo_fleet_product_plane_ >= 0) {
				const double target = std::max(fleet.target_planes, fleet.planes) * urgency;
				if (fleet.planes < target) {
					demo_fleet_buy(nation_id, String("cargo_plane"), target - fleet.planes,
						kDemoFleetBudgetShare);
				}
			}
		}
	}
	// Métricas do dia: fila e combustível consumido são diários. Os caminhões
	// de coleta/entrega do cais também são liberados aqui (ver
	// demo_trade_food): eles voltam no mesmo dia, enquanto navios e aviões
	// seguem reservados até a chegada.
	for (auto& entry : demo_fleets_) {
		entry.second.fuel_used_today = 0.0;
		entry.second.queue_today = 0.0;
		entry.second.missed_no_fleet = 0.0;
		entry.second.missed_no_fuel = 0.0;
		entry.second.trucks_busy = 0.0;
	}
}

Dictionary EconomySim::demo_get_fleet(const String& nation) const {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	Dictionary out;
	const std::string tag(nation.utf8().get_data());
	const auto found = demo_fleets_.find(tag);
	if (found == demo_fleets_.end()) {
		out["error"] = "nacao sem frota";
		return out;
	}
	const DemoFleet& fleet = found->second;
	out["nation"] = nation;
	out["trucks"] = fleet.trucks;
	out["ships"] = fleet.ships;
	out["planes"] = fleet.planes;
	out["trucks_busy"] = fleet.trucks_busy;
	out["ships_busy"] = fleet.ships_busy;
	out["planes_busy"] = fleet.planes_busy;
	out["health"] = Array{fleet.health[0], fleet.health[1], fleet.health[2]};
	// Capacidade efetiva de um lote por modal (t) — veículos × saúde × carga.
	out["capacity_tons"] = fleet.trucks * fleet.health[0] * 25.0
		+ fleet.ships * fleet.health[1] * 2000.0
		+ fleet.planes * fleet.health[2] * 20.0;
	out["fuel"] = fleet.fuel;
	out["fuel_target"] = fleet.fuel_target;
	out["spare_parts"] = fleet.spare_parts;
	out["fuel_used_today"] = fleet.fuel_used_today;
	out["fuel_used_total"] = fleet.fuel_used_total;
	out["parts_used_total"] = fleet.parts_used_total;
	out["vehicles_bought_total"] = fleet.vehicles_bought_total;
	out["queue_today"] = fleet.queue_today;
	out["missed_no_fleet"] = fleet.missed_no_fleet;
	out["missed_no_fuel"] = fleet.missed_no_fuel;
	out["derived"] = fleet.derived;
	out["source"] = fleet.derived ? "derived" : "declared";
	out["target_trucks"] = fleet.target_trucks;
	out["target_ships"] = fleet.target_ships;
	out["target_planes"] = fleet.target_planes;
	return out;
}

void EconomySim::demo_set_fleet(const String& nation, double trucks, double ships,
		double planes, double fuel, double spare_parts) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	const std::string tag(nation.utf8().get_data());
	DemoFleet fleet = DemoFleet{};
	fleet.trucks = std::max(0.0, trucks);
	fleet.ships = std::max(0.0, ships);
	fleet.planes = std::max(0.0, planes);
	fleet.target_trucks = fleet.trucks;
	fleet.target_ships = fleet.ships;
	fleet.target_planes = fleet.planes;
	fleet.health[0] = 1.0;
	fleet.health[1] = 1.0;
	fleet.health[2] = 1.0;
	fleet.fuel = std::max(0.0, fuel);
	fleet.fuel_target = fleet.fuel;
	fleet.spare_parts = std::max(0.0, spare_parts);
	fleet.derived = false;
	demo_fleets_[tag] = fleet;
	bool known = false;
	for (const std::string& entry : demo_fleet_order_) {
		if (entry == tag) {
			known = true;
			break;
		}
	}
	if (!known) {
		demo_fleet_order_.push_back(tag);
	}
	demo_status_invalidate();
}

Dictionary EconomySim::demo_fleet_buy(const String& nation, const String& product,
		double amount, double budget_share) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	Dictionary result;
	result["bought"] = 0.0;
	result["spent"] = 0.0;
	result["unfilled"] = 0.0;
	result["error"] = "";
	const std::string tag(nation.utf8().get_data());
	const int product_index = demo_product_index(product);
	if (product_index < 0) {
		result["error"] = "produto desconhecido";
		return result;
	}
	if (amount <= 1e-9) {
		result["error"] = "quantidade invalida";
		return result;
	}
	const auto fleet_found = demo_fleets_.find(tag);
	if (fleet_found == demo_fleets_.end()) {
		result["error"] = "nacao sem frota";
		return result;
	}
	const auto cities_found = demo_cities_by_nation_.find(tag);
	if (cities_found == demo_cities_by_nation_.end() || cities_found->second.empty()) {
		result["error"] = "nacao sem cidade";
		return result;
	}
	// Cidade-sede: a maior população de simulação paga a compra do governo.
	int hq = -1;
	double best_population = -1.0;
	for (const int index : cities_found->second) {
		const double population = demo_cities_[static_cast<std::size_t>(index)]
			.simulation_population;
		if (hq < 0 || population > best_population) {
			hq = index;
			best_population = population;
		}
	}
	DemoCity& hq_city = demo_cities_[static_cast<std::size_t>(hq)];
	DemoFleet& fleet = fleet_found->second;
	// O pedido inteiro entra como demanda desejada da sede, mesmo sem estoque:
	// é o sinal que a IA lê para produzir/importar (estado vazio honesto).
	hq_city.markets[product_index].desired += amount;
	const double budget = std::max(0.0, hq_city.government_budget)
		* std::clamp(budget_share, 0.0, 1.0);
	struct Seller {
		int city = 0;
		// Preço na ÂNCORA: os caixas (orçamento do governo) estão em XGS, não
		// na moeda local (plano §5.6). Todas as cidades aqui são da mesma
		// nação, então a conversão preserva a ordem de escolha.
		double price = 0.0;
		double stock = 0.0;
	};
	std::vector<Seller> sellers;
	sellers.reserve(cities_found->second.size());
	for (const int index : cities_found->second) {
		const DemoCity& city = demo_cities_[static_cast<std::size_t>(index)];
		const DemoMarket& market = city.markets[product_index];
			if (market.stock > 1e-9 && market.price > 0.0) {
				sellers.push_back({index, market.price
					* demo_competitiveness(city.nation), market.stock});
			}
	}
	std::stable_sort(sellers.begin(), sellers.end(),
		[](const Seller& left, const Seller& right) {
			if (left.price != right.price) {
				return left.price < right.price;
			}
			return left.city < right.city;
		});
	double remaining = amount;
	double spent = 0.0;
	for (const Seller& seller_entry : sellers) {
		if (remaining <= 1e-9 || spent >= budget - 1e-9) {
			break;
		}
		DemoCity& seller = demo_cities_[static_cast<std::size_t>(seller_entry.city)];
		DemoMarket& market = seller.markets[product_index];
		double quantity = std::min(remaining, market.stock);
		if (quantity * seller_entry.price > budget - spent) {
			quantity = (budget - spent) / seller_entry.price;
		}
		if (quantity <= 1e-9) {
			continue;
		}
		const double cost = quantity * seller_entry.price;
		hq_city.government_budget -= cost;
		seller.firm_cash += cost;
		market.stock -= quantity;
		market.demand += quantity;
		market.sold += quantity;
		seller.revenue_by_product[product_index] += cost;
		spent += cost;
		remaining -= quantity;
	}
	const double bought = amount - remaining;
	if (product_index == demo_fleet_product_fuel_) {
		fleet.fuel += bought;
		fleet.fuel_bought_total += bought;
	} else if (product_index == demo_fleet_product_parts_) {
		fleet.spare_parts += bought;
	} else if (product_index == demo_fleet_product_truck_) {
		fleet.trucks += bought;
		fleet.vehicles_bought_total += bought;
	} else if (product_index == demo_fleet_product_ship_) {
		fleet.ships += bought;
		fleet.vehicles_bought_total += bought;
	} else if (product_index == demo_fleet_product_plane_) {
		fleet.planes += bought;
		fleet.vehicles_bought_total += bought;
	}
	result["bought"] = bought;
	result["spent"] = spent;
	result["unfilled"] = remaining;
	demo_status_invalidate();
	return result;
}

void EconomySim::demo_set_market_stock(const String& city_id, const String& product,
		double stock) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	DemoCity* city = find_demo_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr) {
		return;
	}
	const int product_index = demo_product_index(product);
	if (product_index < 0) {
		return;
	}
	city->markets[product_index].stock = std::max(0.0, stock);
	++demo_status_static_version_;
	demo_status_invalidate();
}

void EconomySim::demo_set_market_price(const String& city_id,
		const String& product, double price) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	DemoCity* city = find_demo_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr || !std::isfinite(price)) {
		return;
	}
	const int product_index = demo_product_index(product);
	if (product_index < 0) {
		return;
	}
	const double base = demo_base_price(product_index);
	city->markets[product_index].price = std::clamp(
		std::max(1e-6, price), base * 0.1, base * 10.0);
	demo_status_invalidate();
}

void EconomySim::demo_national_fiscal_transfer() {	// Equalização fiscal per capita dentro de cada nação: o orçamento público é
	// repartido entre as cidades proporcionalmente à população. Uma cidade rica
	// financia a segurança alimentar de uma cidade pobre da mesma nação (ex.:
	// Paris sustenta Cayenne). Conserva dinheiro — só move government_budget
	// entre cidades da MESMA nação.
	std::unordered_map<std::string, std::vector<DemoCity*>> by_nation;
	by_nation.reserve(demo_cities_.size() / 4 + 8);
	for (DemoCity& city : demo_cities_) {
		by_nation[city.nation].push_back(&city);
	}
	// Cada nação é independente: a ordem de varredura não muda o resultado.
	for (auto& entry : by_nation) {
		std::vector<DemoCity*>& cities = entry.second;
		double total_budget = 0.0;
		double total_population = 0.0;
		for (DemoCity* city : cities) {
			total_budget += city->government_budget;
			total_population += city->simulation_population;
		}
		if (cities.size() < 2 || total_population <= 0.0) {
			continue;
		}
		for (DemoCity* city : cities) {
			city->government_budget =
				total_budget * (city->simulation_population / total_population);
		}
	}
}

const EconomySim::Port* EconomySim::demo_nearest_port(const std::string& nation,
		const std::string& landmass, const MapPoint& from) const {
	// Porto mais próximo da mesma nação e da mesma massa de terra. Dinâmico:
	// adicionar um porto em ports.json já o torna disponível para o roteamento.
	const Port* best = nullptr;
	double best_distance = 0.0;
	for (const Port& port : demo_ports_) {
		if (port.nation != nation || port.landmass != landmass) {
			continue;
		}
		const double distance = distance_km(from, port.center);
		if (best == nullptr || distance < best_distance) {
			best = &port;
			best_distance = distance;
		}
	}
	return best;
}

EconomySim::Port* EconomySim::find_demo_port(const std::string& id) {
	for (Port& port : demo_ports_) {
		if (port.id == id) {
			return &port;
		}
	}
	return nullptr;
}

int EconomySim::demo_city_index_near(const std::string& nation,
		const std::string& landmass, const MapPoint& from) const {
	int best = -1;
	double best_distance = 0.0;
	for (std::size_t index = 0; index < demo_cities_.size(); ++index) {
		const DemoCity& city = demo_cities_[index];
		if (city.nation != nation || city.landmass != landmass) {
			continue;
		}
		const double distance = distance_km(from, city.center);
		if (best < 0 || distance < best_distance) {
			best = static_cast<int>(index);
			best_distance = distance;
		}
	}
	return best;
}

void EconomySim::demo_init_port_capacities() {
	// Capacidade de nível 1 pela população de simulação da cidade-sede. Sem
	// `port_of` válido (bench sintético), usa a cidade da nação/massa mais
	// próxima do cais; sem nenhuma, o piso de referência.
	for (Port& port : demo_ports_) {
		double population = 0.0;
		const DemoCity* host = port.host_city.empty()
			? nullptr : find_demo_city(port.host_city);
		if (host != nullptr) {
			population = host->simulation_population;
		} else {
			const int near = demo_city_index_near(port.nation, port.landmass, port.center);
			if (near >= 0) {
				port.host_city = demo_cities_[static_cast<std::size_t>(near)].id;
				population =
					demo_cities_[static_cast<std::size_t>(near)].simulation_population;
			}
		}
		port.base_capacity_t_per_day = std::clamp(
			kDemoPortBaseTons + kDemoPortTonsPerCapita * safe_nonneg(population),
			kDemoPortBaseTons, kDemoPortMaxTons);
		port.capacity_t_per_day = port.base_capacity_t_per_day;
		port.used_today_t = 0.0;
		port.queue_t = 0.0;
		port.delay_hours = 0.0;
		port.demurrage = 0.0;
		port.pending_capacity_t_per_day = 0.0;
		port.pending_days = 0;
	}
}

void EconomySim::demo_reset_port_usage() {
	for (Port& port : demo_ports_) {
		port.used_today_t = 0.0;
		port.queue_t = 0.0;
		port.delay_hours = 0.0;
		port.demurrage = 0.0;
	}
}

void EconomySim::demo_tick_port_expansion() {
	for (Port& port : demo_ports_) {
		if (port.pending_capacity_t_per_day <= 1e-9) {
			continue;
		}
		if (port.pending_days > 0) {
			--port.pending_days;
		}
		if (port.pending_days <= 0) {
			port.capacity_t_per_day += port.pending_capacity_t_per_day;
			port.pending_capacity_t_per_day = 0.0;
		}
	}
}

double EconomySim::demo_port_charge(Port& port, double tons, double& demurrage_out) {
	const double load = safe_nonneg(tons);
	const double capacity = std::max(1.0, port.capacity_t_per_day);
	const double overflow = std::max(0.0, port.used_today_t + load - capacity);
	// Fila medida em dias de capacidade: 1.000 t acima de um cais de 1.000 t/dia
	// custa um dia de atraso; um excesso pequeno atrasa só uma fração do dia.
	const double delay_days = overflow / capacity;
	const double demurrage = overflow * delay_days * kDemoPortDemurragePerTonDay;
	port.used_today_t += load;
	port.queue_t = std::max(port.queue_t, overflow);
	port.delay_hours += delay_days * kHoursPerDay;
	port.demurrage += demurrage;
	demurrage_out += demurrage;
	return delay_days;
}

Dictionary EconomySim::demo_expand_port(const String& port_id, double t_per_day) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	Dictionary result;
	result["expanded"] = false;
	result["cost"] = 0.0;
	result["days"] = 0;
	result["capacity"] = 0.0;
	result["error"] = "";

	Port* port = find_demo_port(std::string(port_id.utf8().get_data()));
	if (port == nullptr) {
		result["error"] = "porto desconhecido";
		return result;
	}
	result["capacity"] = port->capacity_t_per_day;
	const double add = safe_nonneg(t_per_day);
	if (add <= 1e-9) {
		result["error"] = "capacidade invalida";
		return result;
	}

	// Obra pública: paga o orçamento da cidade-sede e credita o setor de
	// construção (dinheiro conservado).
	int payer_index = -1;
	if (!port->host_city.empty()) {
		const auto found = demo_city_lookup_.find(port->host_city);
		if (found != demo_city_lookup_.end()) {
			payer_index = found->second;
		}
	}
	if (payer_index < 0) {
		payer_index = demo_city_index_near(port->nation, port->landmass, port->center);
	}
	if (payer_index < 0) {
		result["error"] = "cidade-sede ausente";
		return result;
	}
	DemoCity& payer = demo_cities_[static_cast<std::size_t>(payer_index)];
	const double cost = add * kDemoPortCapitalPerTonCapacity;
	if (payer.government_budget + 1e-9 < cost) {
		result["error"] = "orcamento insuficiente";
		return result;
	}
	payer.government_budget -= cost;
	demo_construction_cash_ += cost;
	port->pending_capacity_t_per_day += add;
	port->pending_days = std::max(port->pending_days, kDemoPortBuildDays);
	demo_status_invalidate();
	result["expanded"] = true;
	result["cost"] = cost;
	result["days"] = port->pending_days;
	return result;
}

void EconomySim::demo_set_port_capacity(const String& port_id, double t_per_day) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	Port* port = find_demo_port(std::string(port_id.utf8().get_data()));
	if (port == nullptr) {
		return;
	}
	port->base_capacity_t_per_day = safe_nonneg(t_per_day);
	port->capacity_t_per_day = port->base_capacity_t_per_day;
	demo_status_invalidate();
}

Dictionary EconomySim::tick_demo() {
	demo_tick_and_publish();
	// Cache hit: demo_tick_and_publish() já montou e publicou o dia em curso.
	return get_demo_status();
}

void EconomySim::demo_tick_and_publish() {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
#if defined(OGS_PROFILING)
	// Sink das fases do tick: os 15 slots de `demo_profile_us_`, com o toggle
	// `demo_profile_` como interruptor (desligado, nenhum relógio é lido).
	ogs::profile::Sink phase_sink{demo_profile_us_, kDemoPhaseCount, &demo_profile_};
	const ogs::profile::SinkScope phase_scope(&phase_sink);
#endif
	demo_tick_core();
	{
		// Fase `status` (RAII): fecha ao sair do bloco, depois do tick.
		OGS_SIM_ZONE(kDemoPhaseStatus);
		get_demo_status();
	}
}

void EconomySim::demo_status_invalidate() {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	demo_status_built_day_ = -1;
}

void EconomySim::demo_set_profiling(bool enabled) {
	demo_profile_ = enabled;
	if (!enabled) {
		for (double& value : demo_profile_us_) {
			value = 0.0;
		}
	}
}

Dictionary EconomySim::demo_get_profile() const {
	static const char* kNames[kDemoPhaseCount] = {
		"arrival", "reset", "labor", "production", "raw", "income", "services",
		"trade", "accounting", "prices", "clamp", "fiscal", "fx", "invariants",
		"status",
	};
	Dictionary profile;
	if (!demo_profile_) {
		return profile;
	}
	for (int phase = 0; phase < kDemoPhaseCount; ++phase) {
		profile[kNames[phase]] = demo_profile_us_[phase];
	}
	static const char* kStatusNames[4] = {
		"status_cities", "status_orders", "status_nations", "status_misc",
	};
	for (int part = 0; part < 4; ++part) {
		profile[kStatusNames[part]] = demo_status_sub_us_[part];
	}
	return profile;
}

// Pré-aquece os dois slots do status. O primeiro dia publicado em um slot
// frio paga a criação de TODOS os dicionários por cidade (medido no cenário
// real: 60 ms contra 17 ms num dia quente). Aqui isso vira custo de carga, e
// nenhum tick paga a alocação. Só roda com o mundo já preenchido (aptidão,
// demanda externa e índice de renda aplicados), senão o bloco estático do
// status sairia com os valores default.
void EconomySim::demo_warm_status() {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	if (!demo_active_) {
		return;
	}
	get_demo_status();  // preenche o slot corrente
	if (demo_status_live_ < 0) {
		return;
	}
	// `built_day_ = -1` faz a próxima chamada construir no OUTRO slot.
	demo_status_built_day_ = -1;
	get_demo_status();
}

void EconomySim::demo_tick_core() {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);

	if (!demo_active_) {
		initialize_demo();
	}
	// §12.3: se o mundo real ainda não tem indústria, semeia poucas plantas de
	// referência (moinho + padaria) antes de contar dinheiro/invariantes.
	demo_seed_industry();

	// Retrato para reverter o tick se um invariante rígido for violado.
	// Buffer PERSISTENTE: os vetores são reutilizados entre dias (com 114 bens
	// uma cidade custa ~27 KB; realocar 834 cópias por tick custava ~5 ms).
	DemoSnapshot& before = demo_snapshot_buffer_;
	demo_snapshot(before);
	const double money_before = demo_money_total();

	++day_;
	demo_last_orders_.clear();
	// Obras de porto com prazo vencido entram em operação neste dia.
	demo_tick_port_expansion();

	// Encerra as construções falidas no início do tick: assim o PIB do dia já
	// não as inclui e o status fica consistente (a falência foi marcada no tick
	// anterior, no fim da fase 7).
	for (auto it = demo_buildings_.begin(); it != demo_buildings_.end();) {
		if (it->bankrupt) {
			++demo_bankruptcies_;
			it = demo_buildings_.erase(it);
			continue;
		}
		// Mina com jazida esgotada encerra a operação no dia seguinte: o fim da
		// reserva é o fim econômico planejado, não uma insolvência. Contá-la
		// como falência inflava o número (99 minas de ouro/diamante de jazidas
		// de poucos dias em 730 d) e escondia as falências de verdade (§19).
		if (it->depleted) {
			++demo_depleted_mines_;
			it = demo_buildings_.erase(it);
			continue;
		}
		++it;
	}
	// O tick apaga construções falidas: os índices por cidade são refeitos
	// antes de qualquer fase usá-los.
	demo_rebuild_indexes();

	// Preço de abertura do dia por cidade/produto. O limite de variação é
	// aplicado UMA vez, no fim do tick: importação e reação de mercado não
	// podem somar dois passos de 5% e produzir um salto de 10% no mesmo dia.
	std::vector<std::vector<double>> open_prices(demo_cities_.size());
	for (std::size_t city_index = 0; city_index < demo_cities_.size(); ++city_index) {
		open_prices[city_index].resize(demo_products_);
		for (int product = 0; product < demo_products_; ++product) {
			open_prices[city_index][product] =
				demo_cities_[city_index].markets[product].price;
		}
	}

	// 1. Chegada das cargas pedidas no dia anterior.
	for (auto shipment = demo_in_transit_.begin(); shipment != demo_in_transit_.end();) {
		if (shipment->arrival_day <= day_) {
			if (DemoCity* city = find_demo_city(shipment->to_city)) {
				city->markets[shipment->product].stock += shipment->amount;
			}
			// F4: libera os veículos reservados pelo pedido (o combustível já
			// foi consumido no embarque).
			DemoTransportDemand done;
			done.trucks = shipment->fleet_trucks;
			done.ships = shipment->fleet_ships;
			done.planes = shipment->fleet_planes;
			demo_fleet_release(shipment->fleet_nation, done);
			shipment = demo_in_transit_.erase(shipment);
			continue;
		}
		++shipment;
	}

	OGS_SIM_PHASE_LAP(kDemoPhaseArrival);

	// 2. Zera os fluxos do dia.
	for (int index = 0; index < demo_products_; ++index) {
		demo_produced_today_[index] = 0.0;
		demo_consumed_today_[index] = 0.0;
	}
	for (DemoCity& city : demo_cities_) {
		city.pop.wage_income = 0.0;
		city.pop.capital_income = 0.0;
		city.pop.transfers = 0.0;
		city.pop.taxes = 0.0;
		city.pop.service_spend = 0.0;
		city.pop.disposable = 0.0;
		city.pop.external_income = 0.0;
		city.pop.food_spend = 0.0;
		city.pop.consumed = 0.0;
		city.pop.need = 0.0;
		city.pop.satisfaction = 0.0;
		city.pop.needs_spend = 0.0;
		city.pop.durable_spend = 0.0;
		city.pop.needs_satisfaction = 0.0;
		city.pop.needs_level = 0;
		// F3: níveis e duráveis (redimensiona só quando o mundo mudou).
		demo_init_pop_needs(city.pop);
		for (int need_level = 0; need_level < kDemoMaxNeedsLevels; ++need_level) {
			city.pop.level_want[need_level] = 0.0;
			city.pop.level_consumed[need_level] = 0.0;
			city.pop.level_satisfaction[need_level] = 0.0;
		}
		city.government_goods_spend = 0.0;
		city.tariff_revenue = 0.0;
		city.service_wage_bill = 0.0;
		city.service_revenue = 0.0;
		city.service_profit = 0.0;
		city.opex_revenue = 0.0;
		city.freight_revenue = 0.0;
		city.construction_revenue = city.construction_revenue_pending;
		city.construction_revenue_pending = 0.0;
		city.gdp = 0.0;
		// F8: fluxos do dia dos serviços (preço/base persistidos).
		city.service_export_revenue = 0.0;
		city.service_government_revenue = 0.0;
		city.service_input_cost = 0.0;
		city.service_business_profit = 0.0;
		for (int sector = 0; sector < kDemoMaxServiceSectors; ++sector) {
			DemoServiceMarket& service = city.service_markets[sector];
			service.demand = 0.0;
			service.supply = 0.0;
			service.unmet = 0.0;
			service.revenue = 0.0;
			service.input_cost = 0.0;
			service.service_cost = 0.0;
			service.jobs = 0.0;
			service.businesses = 0.0;
			service.exports = 0.0;
			service.export_revenue = 0.0;
			service.coverage = 0.0;
			for (int level = 0; level < kDemoMaxNeedsLevels; ++level) {
				service.pop_demand[level] = 0.0;
			}
		}
		for (int product = 0; product < demo_products_; ++product) {
			city.markets[product].demand = 0.0;
			city.markets[product].supply = 0.0;
			city.markets[product].desired = 0.0;
			city.markets[product].latent = 0.0;
			city.markets[product].sold_prev = city.markets[product].sold;
			city.markets[product].sold = 0.0;
			city.markets[product].shortage = 0.0;
			city.markets[product].surplus = 0.0;
			city.revenue_by_product[product] = 0.0;
			city.imported[product] = 0.0;
			city.exported[product] = 0.0;
			city.import_value[product] = 0.0;
			city.export_value[product] = 0.0;
		}
		if (city.pop.desired_by_category.size() != demo_food_products_.size()) {
			city.pop.desired_by_category.assign(demo_food_products_.size(), 0.0);
			city.pop.consumed_by_category.assign(demo_food_products_.size(), 0.0);
		}
		for (int product = 0; product < static_cast<int>(demo_food_products_.size()); ++product) {
			city.pop.desired_by_category[product] = 0.0;
			city.pop.consumed_by_category[product] = 0.0;
		}
	}
	for (DemoBuilding& building : demo_buildings_) {
		building.workers = 0.0;
		building.utilization = 0.0;
		building.revenue = 0.0;
		building.input_value = 0.0;
		building.costs = 0.0;
		building.profit = 0.0;
		building.paid_wages = 0.0;
		building.paid_opex = 0.0;
	}

	OGS_SIM_PHASE_LAP(kDemoPhaseReset);

	// 2b. F4: manutenção/compra mensal da frota e zera a fila do dia. Roda
	// antes do comércio para combustível/veículos comprados já valerem hoje.
	demo_tick_fleet();

	// 3. Mercado de trabalho e salários.
	// Minas mais fundas custam mais para operar: opex × fator de profundidade
	// (1.0 para tudo que não é extração com jazida declarada).
	auto opex_factor = [this](const DemoCity& owner_city, const DemoBuilding& building) {
		if (building.input_product >= 0 || building.output_product < 0
				|| building.output_product >= demo_products_) {
			return 1.0;
		}
		return demo_depth_opex_factor(owner_city.deposits[building.output_product]);
	};
	for (std::size_t city_index = 0; city_index < demo_cities_.size(); ++city_index) {
		DemoCity& city = demo_cities_[city_index];
		const std::vector<int>& local_buildings =
			demo_buildings_by_city_[city_index];
		// F7: o dia do mercado de trabalho resolve qualificação, força de
		// trabalho por nível, vagas, emprego e folha. O opex continua sendo o
		// custo de operação das plantas (pago em seguida, também limitado ao
		// caixa da firma).
		double opex_total = 0.0;
		for (const int bi : local_buildings) {
			DemoBuilding& building = demo_buildings_[static_cast<std::size_t>(bi)];
			const bool secondary = building.recipe >= 0 || building.input_product >= 0;
			// Planta secundária ociosa não paga o opex cheio: o custo acompanha
			// a operação do dia anterior (piso de 5% de stand-by).
			const double operating = secondary
				? std::clamp(building.last_utilization, 0.05, 1.0) : 1.0;
			opex_total += building.opex * opex_factor(city, building) * operating;
		}
		demo_labor_day(city);
		// Opex é comprado de serviços domésticos (manutenção, energia), não é
		// uma importação: mantém o dinheiro dentro da economia. A liquidez do
		// opex é a que sobrou do caixa depois da folha.
		const double opex_available = std::max(0.0, city.firm_cash);
		const double opex_scale = opex_total > opex_available
			? (opex_total > 0.0 ? opex_available / opex_total : 1.0) : 1.0;
		for (const int bi : local_buildings) {
			DemoBuilding& building = demo_buildings_[static_cast<std::size_t>(bi)];
			const bool secondary = building.recipe >= 0 || building.input_product >= 0;
			const double operating = secondary
				? std::clamp(building.last_utilization, 0.05, 1.0) : 1.0;
			building.paid_opex = building.opex * opex_factor(city, building)
				* operating * opex_scale;
		}
		const double paid_opex = opex_total * opex_scale;
		city.firm_cash -= paid_opex;
		city.service_cash += paid_opex;
		city.opex_revenue = paid_opex;
	}

	OGS_SIM_PHASE_LAP(kDemoPhaseLabor);

	// 4. Produção: cada construção declara insumo e produto. A produção segue
	// as vendas do dia anterior (com folga de estoque), limitada pela
	// capacidade, pela aptidão territorial e pelos insumos disponíveis.
	// Capacidade por cidade/produto, calculada uma vez por dia. Antes, cada
	// construção varria TODAS as construções do mundo para achar a capacidade
	// local do seu produto — O(construções²), inviável em escala mundial.
	std::vector<std::array<double, kDemoMaxProducts>> capacity_by_city_product(
		demo_cities_.size());
	for (auto& slots : capacity_by_city_product) {
		slots.fill(0.0);
	}
	for (const DemoBuilding& building : demo_buildings_) {
		if (building.output_product < 0 || building.output_product >= demo_products_) {
			continue;
		}
		const auto found = demo_city_lookup_.find(building.city);
		if (found == demo_city_lookup_.end()) {
			continue;
		}
		capacity_by_city_product[static_cast<std::size_t>(found->second)]
			[building.output_product] += building.capacity * building.output_scale;
	}

	for (DemoBuilding& building : demo_buildings_) {
		DemoCity* city = find_demo_city(building.city);
		if (city == nullptr || building.output_product < 0
				|| building.output_product >= demo_products_) {
			continue;
		}

		const std::size_t city_slot =
			static_cast<std::size_t>(city - demo_cities_.data());
		const double city_capacity =
			capacity_by_city_product[city_slot][building.output_product];
		const double capacity_share = city_capacity > 0.0
			? building.capacity * building.output_scale / city_capacity : 0.0;
		const double sold_prev = city->markets[building.output_product].sold_prev;
		const double labor = building.jobs > 0.0
			? building.workers / building.jobs : 1.0;
		const bool primary = building.recipe < 0 && building.input_product < 0;
		// Extração: aptidão territorial × eficiência pela qualidade do minério
		// (1.0 quando não há jazida declarada).
		DemoDeposit& deposit = city->deposits[building.output_product];
		const double suitability = primary
			? std::clamp(city->suitability[building.output_product], 0.0, 1.0)
				* demo_extraction_efficiency(deposit)
			: 1.0;

		// Capacidade e vendas na mesma unidade (toneladas efetivas).
		const double effective_capacity = city_capacity * suitability;
		// Estoque disponível antes da produção de hoje (stock já inclui o que
		// foi produzido nesta passagem; supply é exatamente essa produção).
		const double stock_before = std::max(0.0,
			city->markets[building.output_product].stock
			- city->markets[building.output_product].supply);
		// Repõe metade da distância até o estoque-alvo, para cima ou para baixo.
		// Isso evita tanto a produção infinita (excedente acumulado) quanto a
		// parada brusca quando o estoque está acima do necessário.
		double target = sold_prev;
		if (sold_prev <= 0.0) {
			// Produtor novo (ou sem vendas) enche o mercado com a capacidade
			// EFETIVA (output_scale), o que dá ao produto a primeira oferta
			// observável — é o que permite o comércio/consumo começar.
			target = effective_capacity;
		} else {
			const double desired_stock = sold_prev * demo_inventory_cover_days_;
			target = sold_prev
				+ (desired_stock - stock_before) * demo_inventory_recovery_;
		}
		target = std::clamp(target, 0.0, effective_capacity);
		const double desired_output = target * capacity_share * labor;
		double input_used = 0.0;
		double output = desired_output;
		if (building.recipe >= 0) {
			// Receita industrial (1..6 insumos, yield e subprodutos). A
			// alternativa de menor custo corrente entre as disponíveis na
			// cidade é escolhida a cada dia (determinístico; empate = primeira).
			const int chosen = demo_pick_recipe(*city, building.output_product);
			if (chosen >= 0) {
				building.recipe = chosen;
			}
			const DemoRecipe& recipe =
				demo_recipes_[static_cast<std::size_t>(building.recipe)];
			const double batch = recipe.yield > 0.0
				? desired_output / recipe.yield : 0.0;
			double scale = 1.0;
			for (int slot = 0; slot < recipe.input_count; ++slot) {
				const int input = recipe.inputs[slot];
				const double needed = recipe.input_qty[slot] * batch;
				DemoMarket& input_market = city->markets[input];
				input_market.desired += needed;
				if (needed > 1e-12) {
					scale = std::min(scale, input_market.stock / needed);
				}
			}
			scale = std::clamp(scale, 0.0, 1.0);
			for (int slot = 0; slot < recipe.input_count; ++slot) {
				const int input = recipe.inputs[slot];
				const double used = recipe.input_qty[slot] * batch * scale;
				DemoMarket& input_market = city->markets[input];
				input_market.stock = std::max(0.0, input_market.stock - used);
				input_market.demand += used;
				input_market.sold += used;
				city->revenue_by_product[input] += used * input_market.price;
				building.input_value += used * input_market.price;
				input_used += used;
			}
			output = batch * scale * recipe.yield;
			for (int slot = 0; slot < recipe.byproduct_count; ++slot) {
				const int product = recipe.byproducts[slot];
				const double amount = recipe.byproduct_qty[slot] * batch * scale;
				if (amount <= 0.0) {
					continue;
				}
				city->markets[product].stock += amount;
				city->markets[product].supply += amount;
				demo_produced_[product] += amount;
				demo_produced_today_[product] += amount;
			}
		} else if (building.input_product >= 0) {
			const double needed_input = desired_output * building.input_per_output;
			DemoMarket& input_market = city->markets[building.input_product];
			input_market.desired += needed_input;
			input_used = std::min(needed_input, input_market.stock);
			output = building.input_per_output > 0.0
				? input_used / building.input_per_output : 0.0;

			input_market.stock -= input_used;
			input_market.demand += input_used;
			input_market.sold += input_used;
			// A receita do setor fornecedor inclui a transferência interna entre
			// construções (quando houver cadeia de refino), senão o valor
			// adicionado do fornecedor ficaria negativo.
			city->revenue_by_product[building.input_product] += input_used
				* input_market.price;
			building.input_value = input_used * input_market.price;
		} else if (deposit.tracked) {
			// Esgotamento: a mina só extrai o que resta da reserva da jazida
			// (compartilhada pelas minas da cidade) e a debita na mesma medida.
			const double available_tons = deposit.remaining_kt * kDemoTonsPerKiloton;
			output = std::clamp(output, 0.0, available_tons);
			const double extracted_kt = output / kDemoTonsPerKiloton;
			deposit.remaining_kt = std::max(0.0, deposit.remaining_kt - extracted_kt);
			deposit.extracted_kt += extracted_kt;
			// Reserva zerada: a mina encerra no próximo dia SEM virar falência.
			if (deposit.remaining_kt <= 1e-9) {
				building.depleted = true;
			}
		}

		city->markets[building.output_product].stock += output;
		city->markets[building.output_product].supply += output;
		building.utilization = building.capacity > 0.0
			? output / building.capacity : 0.0;
		building.last_utilization = building.utilization;
		demo_produced_[building.output_product] += output;
		demo_produced_today_[building.output_product] += output;
	}

	OGS_SIM_PHASE_LAP(kDemoPhaseProduction);

	// 4c. Recursos brutos (minerais, soja, algodão e madeira): o resto do mundo
	// compra ao preço do mercado até um TETO de demanda externa por cidade
	// (external_demand), declarado pelo GeographyService como a participação da
	// nação no mercado mundial (reserva esgotada em ~25 anos — o mesmo horizonte
	// de max_rate_kt_day do deposits.json). Com o teto, a produção acima dele
	// vira estoque e a fase 8 derruba o preço. §5.4: só é exportado o bem com
	// `external_demand > 0` que NÃO é insumo de nenhuma construção ativa no
	// mundo (regra explícita, no lugar do antigo filtro "não é comida").
	std::vector<char> product_is_input(static_cast<std::size_t>(demo_products_), 0);
	for (const DemoBuilding& building : demo_buildings_) {
		if (building.recipe >= 0
				&& building.recipe < static_cast<int>(demo_recipes_.size())) {
			const DemoRecipe& recipe = demo_recipes_[static_cast<std::size_t>(building.recipe)];
			for (int slot = 0; slot < recipe.input_count; ++slot) {
				if (recipe.inputs[slot] >= 0 && recipe.inputs[slot] < demo_products_) {
					product_is_input[static_cast<std::size_t>(recipe.inputs[slot])] = 1;
				}
			}
		} else if (building.input_product >= 0 && building.input_product < demo_products_) {
			product_is_input[static_cast<std::size_t>(building.input_product)] = 1;
		}
	}
	for (DemoCity& city : demo_cities_) {
		for (int product = kDemoFirstRawProduct; product < demo_products_; ++product) {
			if (demo_food_calories(product) > 0.0) {
				continue;
			}
			if (product_is_input[static_cast<std::size_t>(product)] != 0) {
				continue;  // insumo de cadeia ativa: fica no mercado interno
			}
			const double ceiling = city.external_demand[product];
			if (ceiling <= 0.0) {
				continue;  // sem demanda externa declarada (sem depósito/aptidão)
			}
			DemoMarket& market = city.markets[product];
			market.desired += ceiling;
			const double sold = std::min(market.stock, ceiling);
			if (sold <= 0.0) {
				continue;
			}
			const double revenue = sold * market.price;
			market.stock -= sold;
			market.demand += sold;
			market.sold += sold;
			city.firm_cash += revenue;
			city.revenue_by_product[product] += revenue;
			city.exported[product] += sold;
			city.export_value[product] += revenue;
			demo_foreign_cash_ -= revenue;
			demo_consumed_total_[product] += sold;
			demo_consumed_today_[product] += sold;
		}
	}

	OGS_SIM_PHASE_LAP(kDemoPhaseRaw);

	// 5. Renda das famílias, impostos, transferências e consumo alimentar.
	for (DemoCity& city : demo_cities_) {
		DemoPop& pop = city.pop;
		pop.capital_income = city.capital_income_next;
		city.capital_income_next = 0.0;
		pop.taxes = (pop.wage_income + pop.capital_income) * kDemoTaxRate;
		// Orçamento público equilibrado: tudo o que é arrecadado volta como
		// transferência. Sem isso, o governo acumula receita e drena demanda.
		pop.transfers = pop.taxes * kDemoTransferShare;
		pop.disposable = std::max(0.0,
			pop.wage_income + pop.capital_income + pop.transfers - pop.taxes);
		// Renda externa (serviços urbanos/pesca/turismo): o resto do mundo paga
		// por serviços da cidade. Entra no orçamento das famílias e sai do setor
		// externo — é a renda que financia a importação de alimento onde não há
		// terra produtiva. Não é tributada nem infla a folha doméstica (§20).
		// O índice por cidade vem do PIB PPP per capita da nação (GeographyService,
		// nations.csv), normalizado para média mundial = 1,0; cidade sem índice
		// declarado (cenário sintético, save antigo) mantém a média (1,0).
		//
		// F7 calibração: é um PISO DE SUBSISTÊNCIA, então o índice é limitado à
		// média mundial (kDemoExternalIncomeIndexCap). Escalado até o topo, esse
		// bloco sozinho pagava 3× a cesta completa das famílias dos EUA/DEU e
		// punha o mundo inteiro no nível luxury. A renda acima da subsistência
		// vem do valor adicionado (salário + lucro), medido na cidade.
		pop.external_income = city.simulation_population
			* kDemoExternalIncomePerCapita
			* std::min(city.external_income_index, kDemoExternalIncomeIndexCap);
		pop.disposable += pop.external_income;
		demo_foreign_cash_ -= pop.external_income;

		// Necessidade alimentar do dia em toneladas-equivalentes de grão (B3) e
		// custo EFETIVO da cesta: os preços dos alimentos que a cidade REALMENTE
		// tem em estoque (na proporção de disponibilidade usada na compra) mais
		// o resíduo importado ao CIF conhecido. Antes, o subsídio usava o
		// alimento mais barato do MUNDO — pão/processado sem estoque local — e
		// subestimava em 3-4× o custo do grão da prateleira; a cidade comprava
		// só uma fração da necessidade mesmo gastando todo o orçamento
		// (NGA-LAG, SYR-DAM, SOL-HAR em 365 d).
		pop.need = city.simulation_population * kDemoNeedPerCapita;
		double food_local_cost = 0.0;
		double food_import_cost = 0.0;
		double food_unit_cost = 0.0;
		demo_food_basket_cost(city, pop.need, food_local_cost, food_import_cost,
			food_unit_cost);

		// A renda externa é renda de subsistência: o gasto discricionário em
		// serviços incide só sobre a renda DOMÉSTICA. Sem isso a cidade sem
		// terra gastava 45% da renda externa em serviços e sobrava menos que a
		// cesta (TKM ficava em 64% em 730 d, §20). A comida tem prioridade de
		// ORDENAÇÃO (é comprada antes de life/luxury e o governo cobre o que
		// faltar); o setor de serviços continua sendo a fonte de renda dos
		// empregos residuais, por isso não é zerado pela cesta.
		// F8 — FREIO DE FOME: quando a cidade não consegue cobrir a cesta nem
		// com a poupança mais a renda doméstica do dia, o gasto em serviços só
		// consome o que passar da cesta; sem essa situação, vale a fração de 45%
		// da renda doméstica (comportamento F7), que financia o setor cuja folha
		// e lucro voltam para a POP. A condição é de AFORDABILIDADE, não a
		// satisfação de ontem: cidade nova (sem comida comprada ainda) não pode
		// cair num laço de "sem serviço → sem renda → sem comida".
		const double basket_cost = pop.need * food_unit_cost
			* kDemoServiceFoodGuardDays;
		const double domestic_income =
			std::max(0.0, pop.disposable - pop.external_income);
		double service_base = domestic_income;
		if (basket_cost > 0.0 && domestic_income + std::max(0.0, pop.savings)
				< basket_cost) {
			service_base = std::max(0.0, domestic_income - basket_cost);
		}
		pop.service_spend = service_base * kDemoServiceSpendShare;
		city.service_cash += pop.service_spend;
		city.government_budget += pop.taxes - pop.transfers;

		// F7 calibração — RENDA POR ESTRATO. A folha por escolaridade vem do
		// mercado de trabalho (já paga e escalada pela liquidez); o lucro vai só
		// para os DONOS (estrato superior); a renda externa de subsistência é
		// uniforme per capita. O orçamento de compra continua sendo o da cidade
		// (o dinheiro é conservado); o que muda por estrato é QUEM pode comprar
		// cada nível de cesta.
		double stratum_pop_sum = 0.0;
		for (int level = 0; level < kDemoEducationLevels; ++level) {
			stratum_pop_sum += std::max(0.0, pop.stratum_population[level]);
		}
		for (int level = 0; level < kDemoEducationLevels; ++level) {
			const double share = stratum_pop_sum > 1e-9
				? std::max(0.0, pop.stratum_population[level]) / stratum_pop_sum
				: 1.0 / double(kDemoEducationLevels);
			pop.stratum_wage_income[level] = pop.wage_income_by_level[level];
			pop.stratum_external_income[level] = pop.external_income * share;
			pop.stratum_capital_income[level] = 0.0;
			pop.stratum_disposable[level] =
				pop.stratum_wage_income[level]
				+ pop.stratum_external_income[level];
		}
		// Lucro (renda de capital) só para os donos: o estrato de escolaridade
		// superior. Dividi-lo igualmente entre toda a POP é o que fazia
		// 832/834 cidades caírem no nível máximo de consumo.
		pop.stratum_capital_income[kDemoEducationLevels - 1] =
			std::max(0.0, pop.capital_income);
		pop.stratum_disposable[kDemoEducationLevels - 1] +=
			std::max(0.0, pop.capital_income);

		// F7 calibração — classes de consumo do dia (Gini declarado) ou espelho
		// dos estratos de escolaridade (legado). O teto de consumo de hoje usa
		// a classe; no caminho legado, o nível do estrato veio de ontem.
		demo_refresh_income_classes(city);

		double budget = pop.savings + pop.disposable - pop.service_spend;
		// Subsídio de subsistência: o governo garante a cesta alimentar mínima
		// quando o orçamento de comida não cobre (ex.: cidade sem produção nem
		// renda própria). Primeiro a parcela que está na prateleira (estoque
		// local); depois o resíduo que terá de ser importado, para que a fase 6
		// (comércio) encontre caixa. Sai do orçamento público — conserva o
		// dinheiro.
		double food_topup_local = 0.0;
		if (food_local_cost > 0.0 && budget < food_local_cost) {
			food_topup_local = std::min(food_local_cost - budget,
				std::max(0.0, city.government_budget));
			if (food_topup_local > 0.0) {
				budget += food_topup_local;
				city.government_budget -= food_topup_local;
				pop.transfers += food_topup_local;
			}
		}
		double food_topup_import = 0.0;
		if (food_import_cost > 0.0) {
			const double own_left = std::max(0.0, budget - food_local_cost);
			const double shortfall = std::max(0.0, food_import_cost - own_left);
			food_topup_import = std::min(shortfall,
				std::max(0.0, city.government_budget));
			if (food_topup_import > 0.0) {
				// Fica com as famílias: a importação é paga na fase 6 com
				// `pop.savings` quando firma e governo não cobrem.
				budget += food_topup_import;
				city.government_budget -= food_topup_import;
				pop.transfers += food_topup_import;
			}
		}

		// Consumo (B3): primeiro o estoque local, proporcional à disponibilidade
		// (toneladas-equivalentes de grão); o que faltar é comprado na fase 6
		// (comércio), sempre pelo alimento mais barato por caloria. A demanda
		// desejada é registrada mesmo sem estoque (sinal de escassez para a IA e
		// para os preços); a compra realizada é limitada por estoque e orçamento.
		double consumed = 0.0;      // toneladas-equivalentes de grão
		double food_spend = 0.0;
		double total_avail = 0.0;
		std::vector<double> avail_ge(demo_food_products_.size(), 0.0);
		for (int slot = 0; slot < static_cast<int>(demo_food_products_.size()); ++slot) {
			const int product = demo_food_product(slot);
			avail_ge[slot] = city.markets[product].stock * demo_food_calories(product);
			total_avail += avail_ge[slot];
		}
		double remaining_need = pop.need;
		for (int slot = 0; slot < static_cast<int>(demo_food_products_.size()) && remaining_need > 1e-12; ++slot) {
			const int product = demo_food_product(slot);
			const double factor = demo_food_calories(product);
			if (factor <= 0.0) {
				continue;
			}
			DemoMarket& market = city.markets[product];
			const double price = market.price;
			if (price <= 0.0) {
				continue;
			}
			// Desejo proporcional à disponibilidade; sem estoque nenhum, todo o
			// desejo recai sobre o primeiro alimento da cesta (trigo — o mais
			// barato por caloria na base), para manter o sinal de escassez.
			double want_ge = total_avail > 0.0
				? pop.need * (avail_ge[slot] / total_avail)
				: (slot == 0 ? pop.need : 0.0);
			want_ge = std::min(want_ge, remaining_need);
			double desired_tons = want_ge / factor;
			if (desired_tons * price > budget) {
				desired_tons = std::max(0.0, budget) / price;
			}
			if (desired_tons <= 1e-12) {
				continue;
			}
			market.desired += desired_tons;
			pop.desired_by_category[slot] += desired_tons;
			const double quantity = std::min(desired_tons, market.stock);
			if (quantity <= 0.0) {
				continue;
			}
			const double paid = quantity * price;
			market.stock -= quantity;
			market.demand += quantity;
			market.sold += quantity;
			budget -= paid;
			food_spend += paid;
			consumed += quantity * factor;
			pop.consumed_by_category[slot] += quantity;
			city.revenue_by_product[product] += paid;
			city.firm_cash += paid;
			demo_consumed_total_[product] += quantity;
			demo_consumed_today_[product] += quantity;
			remaining_need -= quantity * factor;
		}
		// F9 — PISO DE SUBSISTÊNCIA AGRÍCOLA (autoconsumo em espécie). Só age
		// quando a cidade NÃO conseguiu cobrir a cesta com o que comprou: as
		// famílias agrícolas comem da própria produção (agricultura familiar),
		// limitado pela necessidade dessas famílias e por uma fração da produção
		// local. O alimento sai do ESTOQUE (conservação): não é comprado, então
		// não há pagamento nem criação de comida. Sem déficit, não faz nada.
		double in_kind = 0.0;
		if (remaining_need > 1e-9 && city.agri_output_tons > 0.0
				&& city.sector_workers[0] > 0.0) {
			const double farm_household_size = std::clamp(
				city.simulation_population
					/ std::max(city.pop.labor_force, 1e-9), 1.0, 6.0);
			const double household_need = city.sector_workers[0]
				* farm_household_size * kDemoNeedPerCapita;
			const double own_production = city.agri_output_tons
				* demo_agri_own_consumption_share_;
			in_kind = std::min(
				std::min(remaining_need, household_need), own_production);
			double left = in_kind;
			for (int slot = 0; slot < static_cast<int>(demo_food_products_.size())
					&& left > 1e-12; ++slot) {
				const int product = demo_food_product(slot);
				const double factor = demo_food_calories(product);
				if (factor <= 0.0) {
					continue;
				}
				DemoMarket& market = city.markets[product];
				const double available = std::max(0.0, market.stock * factor);
				const double take = std::min(left, available);
				if (take <= 1e-12) {
					continue;
				}
				const double tons = take / factor;
				market.stock = std::max(0.0, market.stock - tons);
				pop.consumed_by_category[slot] += tons;
				demo_consumed_total_[product] += tons;
				demo_consumed_today_[product] += tons;
				consumed += take;
				left -= take;
			}
			in_kind -= left;
		}
		city.in_kind_food = in_kind;
		pop.consumed = consumed;
		pop.food_spend = food_spend;
		pop.satisfaction = pop.need > 0.0
			? std::clamp(consumed / pop.need, 0.0, 1.0) : 1.0;

		// Subsídio de subsistência LOCAL não gasto volta ao governo: sem isso, a
		// cidade sem estoque de alimento convertia o subsídio em poupança e ele
		// acabava financiando life/luxury (dinheiro conservado, mas fora do
		// destino). A parcela de IMPORTAÇÃO não volta: ela precisa continuar com
		// as famílias até a fase 6 pagar a carga.
		if (food_topup_local > 0.0) {
			const double unused = std::min(food_topup_local, std::max(0.0, budget));
			if (unused > 0.0) {
				budget -= unused;
				city.government_budget += unused;
				pop.transfers -= unused;
			}
		}

		// F3: necessidades das POPs em três níveis (basic/life/luxury). O
		// orçamento é sequencial: o alimento básico acima já foi pago; aqui
		// entram as demais categorias do basic, depois life e por último luxury
		// — o nível alcançado é consequência da renda disponível. Faltar
		// basic/food continua derrubando `pop.satisfaction` (fome); faltar
		// life/luxury derruba a satisfação do nível e a agregada.
		if (demo_needs_loaded_) {
			// Duráveis: descarte com perda de durable_replacement_per_year, na
			// taxa diária que perde exatamente r em 365 dias.
			for (int product = 0; product < demo_products_; ++product) {
				if (product < static_cast<int>(pop.durable_stock.size())
						&& product < static_cast<int>(demo_product_durable_.size())
						&& demo_product_durable_[static_cast<std::size_t>(product)] != 0) {
					pop.durable_stock[static_cast<std::size_t>(product)] =
						std::max(0.0,
							pop.durable_stock[static_cast<std::size_t>(product)]
							* (1.0 - demo_durable_decay_));
				}
			}
			// Reserva de alimento: a POP só compra life/luxury com a poupança
			// acima de kDemoFoodReserveDays de cesta. Isso preserva a capacidade
			// de importar comida em nações sem terra (o alimento continua sendo
			// a primeira prioridade do orçamento).
			double needs_budget = budget;
			if (food_unit_cost > 0.0) {
				const double reserve = pop.need * kDemoFoodReserveDays * food_unit_cost;
				// Protege no máximo a poupança existente: se ela já é menor que
				// o colchão, a renda corrente do dia continua disponível.
				const double protected_savings =
					std::min(std::max(0.0, pop.savings), reserve);
				needs_budget = std::max(0.0, budget - protected_savings);
			}
			double needs_spent = 0.0;
			double weighted = 0.0;
			double weight_sum = 0.0;
			for (double& level_spend : pop.level_spend) {
				level_spend = 0.0;
			}
			// População por CLASSE de consumo (basic/life/luxury; soma =
			// população da cidade). Com Gini declarado a classe vem da
			// distribuição de renda; sem Gini, espelha o estrato de
			// escolaridade (legado). É ela que define o desejo e o teto de
			// compra de cada nível.
			double class_total = 0.0;
			for (int cls = 0; cls < kDemoEducationLevels; ++cls) {
				class_total += std::max(0.0, pop.class_population[cls]);
			}
			for (int level_index = 0;
					level_index < static_cast<int>(demo_needs_levels_.size())
					&& level_index < kDemoMaxNeedsLevels; ++level_index) {
				const DemoNeedsLevel& level =
					demo_needs_levels_[static_cast<std::size_t>(level_index)];
				// F7: a renda define o nível de consumo POR ESTRATO. O básico é
				// sempre prioridade de todos; life/luxury só são comprados pelo
				// estrato cuja renda corrente cobre a cesta acumulada. Sem
				// renda, o desejo fica registrado e a satisfação cai — não se
				// inventa consumo com poupança.
				double level_want = 0.0;
				double level_got = 0.0;
				const double spend_before = needs_spent;
				if (level_index == 0) {
					pop.level_spend[0] += food_spend;
				}
				for (int cls = 0; cls < kDemoEducationLevels; ++cls) {
					const double class_pop =
						std::max(0.0, pop.class_population[cls]);
					if (class_pop <= 1e-9) {
						continue;
					}
					const double class_share = class_total > 1e-9
						? class_pop / class_total
						: 1.0 / double(kDemoEducationLevels);
					const bool affordable = level_index == 0
						|| level_index + 1 <= pop.class_income_level[cls];
					for (const DemoNeedsCategory& category : level.categories) {
						// A cesta alimentar básica tem laço próprio (validado) e
						// já foi consumida acima; reaproveita `pop.need/consumed`
						// para não cobrar duas vezes a mesma refeição.
						if (level_index == demo_needs_food_level_
								&& category.id == std::string("food")) {
							level_want += pop.need * class_share;
							level_got += pop.consumed * class_share;
							continue;
						}
						const double want_ge = category.per_capita_day
							* class_pop;
						level_want += want_ge;
						if (!affordable) {
							// Renda insuficiente: não compra (o consumo efetivo
							// respeita a renda), mas a NECESSIDADE entra no
							// mercado como desejo — o sinal de escassez da IA.
							demo_register_category_demand(city, category, want_ge);
							continue;
						}
						level_got += demo_consume_category(
							city, category, want_ge, needs_budget, needs_spent);
					}
				}
				pop.level_spend[level_index] += needs_spent - spend_before;
				pop.level_want[level_index] = level_want;
				pop.level_consumed[level_index] = level_got;
				const double satisfaction = level_want > 0.0
					? std::clamp(level_got / level_want, 0.0, 1.0) : 0.0;
				pop.level_satisfaction[level_index] = satisfaction;
				if (level_want > 0.0 && level.weight > 0.0) {
					weighted += satisfaction * level.weight;
					weight_sum += level.weight;
				}
			}
			pop.needs_satisfaction = weight_sum > 0.0
				? std::clamp(weighted / weight_sum, 0.0, 1.0) : 0.0;
			// F8: gancho de saúde privada. A cobertura (0..1) soma um pequeno
			// bônus de satisfação; o serviço público fica para a F9.
			pop.needs_satisfaction = std::clamp(pop.needs_satisfaction
				+ kDemoServiceHealthSatisfactionBonus * city.private_health_cover,
				0.0, 1.0);
			// Nível alcançado = maior nível com pelo menos metade da cesta,
			// exigindo os níveis anteriores (basic → life → luxury).
			pop.needs_level = 0;
			for (int level_index = 0;
					level_index < static_cast<int>(demo_needs_levels_.size())
					&& level_index < kDemoMaxNeedsLevels; ++level_index) {
				if (pop.level_satisfaction[level_index] >= 0.5) {
					pop.needs_level = level_index + 1;
				} else {
					break;
				}
			}
			pop.needs_spend = needs_spent;
			budget -= needs_spent;
			// F7: teto de consumo pela RENDA REAL de CADA ESTRATO — quanto da
			// cesta completa de cada nível a renda corrente do estrato cobre,
			// aos PREÇOS DE MERCADO de hoje. O nível vale a partir do dia
			// seguinte; é ele que libera life/luxury no orçamento do estrato.
			// Antes o custo vinha do gasto observado, que é zero quando o nível
			// já estava bloqueado: a cidade nunca saía do nível 1 (auto-bloqueio).
			// Preço de mercado existe mesmo sem compra e quebra o laço.
			// F7 calibração — GATE LEGADO por estrato. Continua sendo calculado
			// sempre (o campo `income_level_by_stratum` é lido pela UI/painel);
			// com Gini declarado ele NÃO define o consumo — a classe ancorada no
			// PIB PPP define (`demo_refresh_income_classes`).
			{
				double cumulative_cost[kDemoMaxNeedsLevels] = {};
				double running_cost = 0.0;
				// `demo_level_basket_cost` devolve o custo da CIDADE INTEIRA; o gate
				// de renda compara com a renda PER CAPITA do estrato, então o custo
				// é normalizado pela população.
				const double basket_population =
					std::max(1e-9, city.simulation_population);
				for (int level_index = 0;
						level_index < static_cast<int>(demo_needs_levels_.size())
						&& level_index < kDemoMaxNeedsLevels; ++level_index) {
					running_cost += demo_level_basket_cost(city, level_index);
					cumulative_cost[level_index] = running_cost / basket_population;
				}
				for (int stratum = 0; stratum < kDemoEducationLevels; ++stratum) {
					const double stratum_pop =
						std::max(0.0, pop.stratum_population[stratum]);
					const double per_capita = stratum_pop > 1e-9
						? std::max(0.0, pop.stratum_disposable[stratum]) / stratum_pop
						: 0.0;
					int income_level = 0;
					for (int level_index = 0;
							level_index < static_cast<int>(demo_needs_levels_.size())
							&& level_index < kDemoMaxNeedsLevels; ++level_index) {
						if (per_capita + 1e-12
								>= cumulative_cost[level_index]
									* demo_income_basket_margin_) {
							income_level = level_index + 1;
						} else {
							break;
						}
					}
					pop.stratum_income_level[stratum] = income_level;
				}
				// Sem Gini (legado), o gate de amanhã é o cap de classe; com
				// Gini, os caps vêm da distribuição e não são sobrescritos.
				if (city.income_gini <= 0.0) {
					demo_refresh_income_classes(city);
				}
			}
			// Agregado da cidade: o nível mediano (a maior parte da POP) — é o
			// que o mapa/UI e os testes legados leem — mais a fatia da POP em
			// cada nível, que é o número que separa os países no relatório.
			double population_seen = 0.0;
			int median_level = 0;
			bool median_found = false;
			const double city_population = city.simulation_population;
			for (int level_index = 0; level_index < kDemoIncomeLevels;
					++level_index) {
				pop.income_level_share[level_index] = 0.0;
			}
			for (int cls = 0; cls < kDemoEducationLevels; ++cls) {
				const int reached = pop.class_income_level[cls];
				if (reached >= 0 && reached < kDemoIncomeLevels) {
					pop.income_level_share[reached] +=
						std::max(0.0, pop.class_population[cls]);
				}
			}
			for (int level_index = 0; level_index < kDemoIncomeLevels;
					++level_index) {
				const double share = city_population > 1e-9
					? pop.income_level_share[level_index] / city_population : 0.0;
				pop.income_level_share[level_index] = std::clamp(share, 0.0, 1.0);
				if (!median_found) {
					population_seen += pop.income_level_share[level_index]
						* city_population;
					if (population_seen >= city_population * 0.5) {
						median_level = level_index;
						median_found = true;
					}
				}
			}
			pop.income_level = median_level;
		} else {
			// Sem needs.json (AGRIA/PORTUS e saves antigos): um único nível,
			// exatamente o comportamento validado.
			pop.needs_satisfaction = pop.satisfaction;
			pop.level_want[0] = pop.need;
			pop.level_consumed[0] = pop.consumed;
			pop.level_satisfaction[0] = pop.satisfaction;
			pop.needs_level = pop.satisfaction >= 0.5 ? 1 : 0;
			for (int level_index = 0; level_index < kDemoIncomeLevels;
					++level_index) {
				pop.income_level_share[level_index] = 0.0;
			}
			pop.income_level_share[pop.income_level] = 1.0;
		}
		// Consumo institucional do Estado (paper/computer) — decisão §12.10.
		demo_consume_government(city);
		pop.savings = budget;
	}

	OGS_SIM_PHASE_LAP(kDemoPhaseIncome);

	// F8: serviços. Roda DEPOIS do orçamento das famílias (a poupança do dia já
	// está fechada) e ANTES do comércio: a demanda das classes, o B2B e a
	// exportação de serviços não dependem do frete do dia. Entrada/saída de
	// negócios move capital dentro de `service_cash`/`pop.savings`.
	for (DemoCity& city : demo_cities_) {
		demo_services_day(city);
	}

	OGS_SIM_PHASE_LAP(kDemoPhaseServices);

	// 6. Comércio de alimento (cesta de calorias) entre cidades: cidades com
	// déficit compram das que têm excedente, mais barato por caloria. Cobre tanto
	// o cenário de validação (AGRIA/PORTUS) quanto o mundo real.
	demo_trade_food();
	// 6b. Comércio dos demais bens (insumos industriais), pela mesma malha.
	demo_trade_goods();

	OGS_SIM_PHASE_LAP(kDemoPhaseTrade);

	// 7. Receita por construção, lucro, distribuição de capital e PIB.
	for (std::size_t city_index = 0; city_index < demo_cities_.size(); ++city_index) {
		DemoCity& city = demo_cities_[city_index];
		const std::vector<int>& local_buildings =
			demo_buildings_by_city_[city_index];
		std::array<double, kDemoMaxProducts> capacity_by_output{};
		for (const int bi : local_buildings) {
			const DemoBuilding& building =
				demo_buildings_[static_cast<std::size_t>(bi)];
			capacity_by_output[building.output_product] +=
				building.capacity * building.output_scale;
		}
		for (const int bi : local_buildings) {
			DemoBuilding& building =
				demo_buildings_[static_cast<std::size_t>(bi)];
			const double effective = building.capacity * building.output_scale;
			const double share = capacity_by_output[building.output_product] > 0.0
				? effective / capacity_by_output[building.output_product] : 0.0;
			building.revenue = city.revenue_by_product[building.output_product] * share;
			building.costs = building.paid_wages + building.paid_opex
				+ building.input_value;
			building.profit = building.revenue - building.costs;
		}
		// Planta estratégica (bootstrap industrial §12.3): enquanto a demanda
		// final não existe (F3), o governo cobre o prejuízo da planta de
		// referência em vez de deixá-la fechar. Transferência explícita
		// governo -> firma: conserva o dinheiro e não maquia o resultado do
		// setor (a perda continua visível em `profit` antes do subsídio).
		for (const int bi : local_buildings) {
			DemoBuilding& building =
				demo_buildings_[static_cast<std::size_t>(bi)];
			if (!building.strategic || building.profit >= 0.0) {
				continue;
			}
			const double subsidy = std::min(-building.profit,
				std::max(0.0, city.government_budget));
			if (subsidy > 0.0) {
				city.government_budget -= subsidy;
				city.firm_cash += subsidy;
				building.profit += subsidy;
			}
		}

		// A receita de serviços inclui consumo das famílias, opex empresarial,
		// frete e construção: são serviços domésticos, não importações. F8 soma
		// a exportação de serviços (paga pelo setor externo) e a compra do
		// governo (transferência do orçamento); os insumos comprados pelos
		// negócios reduzem o lucro do setor (o dinheiro foi para as firmas).
		city.service_revenue = city.pop.service_spend + city.opex_revenue
			+ city.freight_revenue + city.construction_revenue
			+ city.service_export_revenue + city.service_government_revenue;
		city.service_profit = city.service_revenue - city.service_wage_bill
			- city.service_input_cost;

		double private_profit = 0.0;
		double state_profit = 0.0;
		double value_added = 0.0;
		// F9: lucro e faturamento por grupo presente na cidade. O JV divide a
		// linha do ativo entre o grupo estrangeiro e o parceiro nacional.
		std::vector<std::array<double, 3>> group_rows;  // índice, lucro, receita
		auto add_group_row = [&group_rows](int group_index, double profit,
				double revenue) {
			for (std::array<double, 3>& row : group_rows) {
				if (static_cast<int>(row[0]) == group_index) {
					row[1] += profit;
					row[2] += revenue;
					return;
				}
			}
			group_rows.push_back({static_cast<double>(group_index), profit, revenue});
		};
		for (const int bi : local_buildings) {
			const DemoBuilding& building =
				demo_buildings_[static_cast<std::size_t>(bi)];
			if (building.owner == "state") {
				state_profit += building.profit;
			} else {
				std::string group_id;
				std::string origin;
				const int owner_kind = demo_owner_kind(building.owner, &group_id,
					&origin);
				const int group_index = owner_kind == 1 || owner_kind == 2
					? demo_group_index(group_id) : -1;
				if (group_index >= 0) {
					const double local_share = std::clamp(building.local_share, 0.0, 1.0);
					const int partner_index = building.partner_group.empty()
						? -1 : demo_group_index(building.partner_group);
					if (local_share > 0.0 && partner_index >= 0) {
						add_group_row(group_index, building.profit * (1.0 - local_share),
							building.revenue * (1.0 - local_share));
						add_group_row(partner_index, building.profit * local_share,
							building.revenue * local_share);
					} else {
						add_group_row(group_index, building.profit, building.revenue);
					}
				} else {
					// Dono de grupo desconhecido (save antigo/editado): cai no
					// privado agregado, sem criar dinheiro.
					private_profit += building.profit;
				}
			}
			value_added += building.revenue - building.input_value;
		}

		// Dividendos só depois de reconstituir o capital de giro. Lucros de
		// estatais vão para o orçamento público; privados, para as famílias.
		const double firm_target =
			city.simulation_population * kDemoFirmCashPerCapita;
		double firm_surplus = std::max(0.0, city.firm_cash - firm_target);
		// F9: o lucro dos ativos de grupo sai da firma para o grupo (imposto
		// corporativo fica no anfitrião; o resto vira caixa do grupo e, no
		// fechamento do dia, dividendo ao estrato dono). A retenção é o que
		// financia o investimento seguinte; nada é criado ou destruído.
		for (const std::array<double, 3>& row : group_rows) {
			demo_route_group_profit(static_cast<int>(row[0]), city.nation,
				row[1], row[2], city, firm_surplus);
		}
		firm_surplus = std::max(0.0, city.firm_cash - firm_target);
		const double positive_total = std::max(0.0, private_profit)
			+ std::max(0.0, state_profit);
		const double distributable = std::min(
			positive_total * kDemoCapitalDistribution, firm_surplus);
		const double private_share = positive_total > 0.0
			? std::max(0.0, private_profit) / positive_total : 0.0;
		const double modeled_distribution = distributable * private_share;
		const double state_distribution = distributable - modeled_distribution;
		const double service_target =
			(city.service_wage_bill + city.service_input_cost)
			* kDemoServiceCashBufferDays;
		const double service_surplus = std::max(0.0,
			city.service_cash - service_target);
		// F8: o lucro do setor é renda da POP dona (plano §13). O caixa guarda
		// só o capital de giro (dias de folha + insumos); o resto do lucro é
		// distribuído no dia seguinte. O alvo antigo por habitante (0,05/pessoa)
		// quase nunca era alcançado e retinha o lucro para sempre: a cidade
		// pagava serviços sem receber o lucro de volta e esgotava a poupança
		// que compra comida (SDN-KHA, SOL-HAR em 365 d, F8).
		const double service_distribution = std::min(
			std::max(0.0, city.service_profit * kDemoCapitalDistribution),
			service_surplus);
		city.firm_cash -= modeled_distribution + state_distribution;
		city.service_cash -= service_distribution;
		city.government_budget += state_distribution;
		city.capital_income_next = modeled_distribution + service_distribution;
		city.gdp = value_added + city.service_revenue;

		// Comportamento das empresas: prejuízo persistente reduz a produção;
		// demanda acima da capacidade com lucro permite expansão.
		for (const int bi : local_buildings) {
			DemoBuilding& building =
				demo_buildings_[static_cast<std::size_t>(bi)];
			// Planta secundária ociosa não é insolvente: sem operação no dia,
			// não acumula dias de prejuízo (o custo de stand-by já é pequeno).
			const bool operating = building.recipe < 0 && building.input_product < 0
				? true : building.utilization > 1e-9;
			if (building.profit < 0.0 && operating) {
				++building.loss_days;
			} else {
				building.loss_days = 0;
			}
			// Falência: prejuízo persistente além do corte fecha a firma. A
			// regra é explícita — a construção some (capacidade e empregos), em
			// vez de ficar produzindo no piso no vermelho para sempre. Planta
			// estratégica é mantida pelo governo (ver subsídio acima).
			if (building.loss_days >= kDemoLossDaysBeforeClose && !building.strategic) {
				building.bankrupt = true;
			}
			if (building.loss_days >= kDemoLossDaysBeforeCut) {
				building.output_scale = std::max(kDemoMinOutputScale,
					building.output_scale * kDemoLossCut);
			} else if (building.profit > 0.0) {
				// Recuperação gradual após um período de prejuízo.
				building.output_scale = std::min(1.0, building.output_scale * 1.02);
			}
		}

		// F7: salários por nível reagem ao fechamento do dia — aperto do
		// mercado (vagas × trabalhadores), margem das firmas e âncora de renda
		// do país, com rigidez. O prêmio por educação emerge da escassez.
		double building_revenue = 0.0;
		double building_profit = 0.0;
		for (const int bi : local_buildings) {
			const DemoBuilding& building =
				demo_buildings_[static_cast<std::size_t>(bi)];
			building_revenue += building.revenue;
			building_profit += building.profit;
		}
		const double firm_margin = building_revenue > 1e-9
			? std::clamp(building_profit / building_revenue, -0.25, 0.25) : 0.0;
		demo_update_wages(city, firm_margin);

		// O salário de serviços se ajusta ao mercado de trabalho residual e o
		// emprego segue a receita do setor ao novo salário. Se a receita não
		// sustenta nem o salário de subsistência, sobra desemprego de verdade —
		// não um piso artificial de 5% de vagas.
		double cheapest_food = 0.0;
		for (int slot = 0; slot < static_cast<int>(demo_food_products_.size()); ++slot) {
			const int product = demo_food_product(slot);
			const double factor = demo_food_calories(product);
			if (factor <= 0.0) {
				continue;
			}
			const double per_calorie = city.markets[product].price / factor;
			if (per_calorie > 0.0
					&& (cheapest_food <= 0.0 || per_calorie < cheapest_food)) {
				cheapest_food = per_calorie;
			}
		}
		const double subsistence_wage = kDemoNeedPerCapita * cheapest_food;
		double modeled_jobs_total = 0.0;
		for (const int bi : local_buildings) {
			modeled_jobs_total += demo_buildings_[static_cast<std::size_t>(bi)].jobs;
		}
		const double residual_labor = std::max(0.0,
			city.pop.labor_force - modeled_jobs_total);
		const double service_supply = residual_labor
			* (1.0 - demo_natural_unemployment_);
		// F7: quem define o salário do setor de serviços é o MERCADO DE TRABALHO
		// (demo_labor_day grava aqui a média ponderada por nível/mistura). A
		// antiga regra de "receita ÷ força residual" brigava com ela e travava o
		// mundo em ~15% de desemprego. A receita continua limitando as VAGAS: se
		// ela não paga nem o salário de subsistência, sobra desemprego de
		// verdade — não um piso artificial de vagas.
		// F7 calibração: o setor de serviços é o empregador RESIDUAL do modelo
		// (serviços urbanos absorvem quem não está nas construções). Quem limita
		// a FOLHA é o caixa do setor (liquidez, em demo_labor_day), não o número
		// de vagas: cortar vagas pela receita transformava um salário-alvo alto
		// em desemprego em massa (1ª medição da calibração: EUA 41,8%, DEU
		// 48,8%), porque a receita de serviços é uma fração da renda das
		// famílias e não acompanha a parcela do trabalho no valor adicionado.
		(void)subsistence_wage;
		city.service_jobs += (service_supply - city.service_jobs)
			* kDemoServiceAdjust;
	}

	OGS_SIM_PHASE_LAP(kDemoPhaseAccounting);

	// 8. Preços reagem a estoque, oferta, demanda e custo de produção.
	for (std::size_t city_index = 0; city_index < demo_cities_.size(); ++city_index) {
		DemoCity& city = demo_cities_[city_index];
		const std::vector<int>& local_buildings =
			demo_buildings_by_city_[city_index];
		std::vector<char> has_producer(static_cast<std::size_t>(demo_products_), 0);
		for (const int bi : local_buildings) {
			const DemoBuilding& building =
				demo_buildings_[static_cast<std::size_t>(bi)];
			if (building.output_product >= 0
					&& building.output_product < demo_products_) {
				has_producer[building.output_product] = true;
			}
		}
		for (int product = 0; product < demo_products_; ++product) {
			const double base = demo_base_price(product);
			DemoMarket& market = city.markets[product];
			if (!has_producer[product]) {
				// §2.7: cidade sem produtor tem o preço disciplinado pelo CIF da
				// importação (na âncora), com a mesma suavização e o mesmo
				// limite diário dos produtores. Sem importação registrada, o preço
				// permanece (não se inventa cotação).
				//
				// A paridade age como TETO de concorrência: sem produtor local, o
				// consumidor pode importar, então o preço não fica ACIMA do CIF —
				// mas a importação não puxa o preço para cima. Motivo (calibração
				// F5, `economy_world_test` 365 d): puxar até o CIF levava EGY/MRT/
				// SDN ao teto de 3× o preço-base e derrubava a comida (EGY 1,000 →
				// 0,594 sem moedas); o frete de comida já é um serviço do comprador
				// e o piso calibrado da cesta (F4) não absorve 3× base.
				if (city.import_price[product] <= 0.0) {
					continue;
				}
				const double target = std::clamp(city.import_price[product],
					base * 0.5, base * 3.0);
				const double capped = std::min(market.price, target);
				const double max_step = base * 0.05;
				double next_import = market.price
					+ (capped - market.price) * kDemoPriceSmoothing;
				next_import = std::clamp(next_import,
					market.price - max_step, market.price + max_step);
				market.price = std::clamp(next_import,
					base * 0.5, base * 3.0);
				continue;
			}
			// Pressão usa a demanda desejada FINANCIADA: compras não atendidas
			// por falta de estoque continuam pressionando o preço, mas o desejo
			// sem renda (`latent`, sinal da IA) não infla o preço de um bem que
			// ninguém no mercado pode pagar.
			const double financed_demand = std::max(0.0,
				market.desired - market.latent);
			const double scarcity = financed_demand
				/ std::max(market.stock + market.supply, 0.001);

			double kind_capacity = 0.0;
			double kind_labor_opex = 0.0;
			for (const int bi : local_buildings) {
				const DemoBuilding& building =
					demo_buildings_[static_cast<std::size_t>(bi)];
				if (building.output_product == product) {
					kind_capacity += building.capacity;
					kind_labor_opex += building.workers * building.wage
						+ building.opex;
				}
			}
			const double unit_labor_opex = kind_capacity > 0.0
				? kind_labor_opex / kind_capacity : 0.0;

			// Piso de custo: insumos da receita + mão de obra/opex por unidade
			// de capacidade. Preços, salário e opex estão todos na âncora (F5),
			// então não há conversão de moeda aqui.
			double cost_floor = base;
			for (const int bi : local_buildings) {
				const DemoBuilding& building =
					demo_buildings_[static_cast<std::size_t>(bi)];
				if (building.output_product != product) {
					continue;
				}
				if (building.recipe >= 0
						&& building.recipe < static_cast<int>(demo_recipes_.size())) {
					const DemoRecipe& recipe =
						demo_recipes_[static_cast<std::size_t>(building.recipe)];
					double input_cost = 0.0;
					for (int slot = 0; slot < recipe.input_count; ++slot) {
						input_cost += recipe.input_qty[slot] / recipe.yield
							* city.markets[recipe.inputs[slot]].price;
					}
					cost_floor = input_cost + unit_labor_opex;
					break;
				}
				if (building.input_product >= 0) {
					cost_floor = city.markets[building.input_product].price
						* building.input_per_output + unit_labor_opex;
					break;
				}
			}
			const double target = std::max(
				base * (1.0 + std::clamp(scarcity - 1.0, -0.5, 2.0)),
				cost_floor * 1.05);
			// Limite de variação diária evita oscilações explosivas.
			const double max_step = base * 0.05;
			double next = market.price
				+ (target - market.price) * kDemoPriceSmoothing;
			next = std::clamp(next, market.price - max_step,
				market.price + max_step);
			market.price = std::clamp(next, base * 0.5, base * 3.0);
		}

		// Registro de escassez e excesso para diagnóstico e visualização.
		for (int product = 0; product < demo_products_; ++product) {
			DemoMarket& market = city.markets[product];
			market.shortage = std::max(0.0, market.desired - market.demand);
			market.surplus = std::max(0.0, market.stock
				- std::max(market.desired, market.sold) * 3.0);
		}
	}

	OGS_SIM_PHASE_LAP(kDemoPhasePrices);

	// 9. Limite diário único por mercado, aplicado depois de todas as regras
	// de preço (importação, custo e escassez). O piso e o teto continuam
	// valendo mesmo que o preço de abertura esteja fora da faixa.
	for (std::size_t city_index = 0; city_index < demo_cities_.size(); ++city_index) {
		for (int product = 0; product < demo_products_; ++product) {
			DemoMarket& market = demo_cities_[city_index].markets[product];
			const double base = demo_base_price(product);
			const double step = base * 0.05;
			const double floor = base * 0.5;
			const double ceiling = base * 3.0;
			// O preço de abertura pode ter saído da banda por um choque
			// (demo_price_shock). Clampa primeiro para dentro da banda, senão
			// `open - step` fica acima do teto e `lower > upper` desfaz o clamp
			// da fase 8, prendendo o preço fora da faixa (bug de preço pós-choque).
			const double open = std::clamp(open_prices[city_index][product], floor, ceiling);
			const double lower = std::max(floor, open - step);
			const double upper = std::min(ceiling, open + step);
			market.price = std::clamp(market.price, lower, upper);
		}
	}

	OGS_SIM_PHASE_LAP(kDemoPhaseClamp);

	// 9b. Redistribuição fiscal nacional (federalismo fiscal).
	demo_national_fiscal_transfer();

	// 9b2. F9: dividendos/repatriação dos grupos. Roda depois da contabilidade
	// das cidades (lucro já roteado) e antes da balança/câmbio, para o fluxo de
	// repatriação do dia entrar no relatório na mesma data.
	demo_settle_groups();

	OGS_SIM_PHASE_LAP(kDemoPhaseFiscal);

	// 9c. Câmbio dinâmico: a balança comercial do dia move as moedas float
	// dentro da banda (déficit desvaloriza, superávit valoriza). Não cria nem
	// destrói dinheiro: só muda a taxa usada na próxima decisão de compra.
	demo_tick_fx();

	OGS_SIM_PHASE_LAP(kDemoPhaseFx);

	// 10. Invariantes: se o tick corrompeu o estado (dinheiro não conservado,
	// caixa/estoque negativo ou preço fora da banda), registra e reverte.
	demo_invariant_violations_ = demo_check_invariants(money_before);
	if (!demo_invariant_violations_.empty()) {
		for (const std::string& violation : demo_invariant_violations_) {
			UtilityFunctions::push_error(
				String(("economia.invariante: " + violation).c_str()));
		}
		demo_restore(before);
	}
	OGS_SIM_PHASE_LAP(kDemoPhaseInvariants);
}

EconomySim::DemoStatusKeys::DemoStatusKeys() :
		amount("amount"),
		bankruptcies("bankruptcies"),
		buildings("buildings"),
		calendar_day("calendar_day"),
		capacity("capacity"),
		capital_cost_per_capacity("capital_cost_per_capacity"),
		capital_income("capital_income"),
		center("center"),
		cities("cities"),
		city_count("city_count"),
		construction_cash("construction_cash"),
		construction_revenue("construction_revenue"),
		consumed("consumed"),
		consumed_by_category("consumed_by_category"),
		consumed_today("consumed_today"),
		consumed_total("consumed_total"),
		costs("costs"),
		day("day"),
		deficit("deficit"),
		demand("demand"),
		dependents("dependents"),
		depleted("depleted"),
		depleted_mines("depleted_mines"),
		deposits("deposits"),
		depth_m("depth_m"),
		desired("desired"),
		desired_by_category("desired_by_category"),
		disposable("disposable"),
		disposable_per_capita("disposable_per_capita"),
		education("education"),
		education_share("education_share"),
		efficiency("efficiency"),
		employed("employed"),
		employed_by_level("employed_by_level"),
		export_value("export_value"),
		exported("exported"),
		external_demand("external_demand"),
		external_income("external_income"),
		external_income_index("external_income_index"),
		extracted_kt("extracted_kt"),
		firm_cash("firm_cash"),
		food_spend("food_spend"),
		foreign_cash("foreign_cash"),
		freight_cost("freight_cost"),
		freight_revenue("freight_revenue"),
		from_city("from_city"),
		from_nation("from_nation"),
		gdp("gdp"),
		gdp_per_capita("gdp_per_capita"),
		good("good"),
		government_budget("government_budget"),
		home_nation("home_nation"),
		id("id"),
		import_value("import_value"),
		imported("imported"),
		in_transit("in_transit"),
		income("income"),
		income_level("income_level"),
		income_level_share("income_level_share"),
		income_level_by_stratum("income_level_by_stratum"),
		stratum_disposable("stratum_disposable"),
		stratum_population("stratum_population"),
		stratum_wage_income("stratum_wage_income"),
		industrialization("industrialization"),
		initial_kt("initial_kt"),
		input_product("input_product"),
		input_value("input_value"),
		invariant_violations("invariant_violations"),
		jobs("jobs"),
		kind("kind"),
		labor("labor"),
		labor_force("labor_force"),
		labor_force_by_level("labor_force_by_level"),
		landmass("landmass"),
		legs("legs"),
		living_standard("living_standard"),
		loss_days("loss_days"),
		market_totals("market_totals"),
		markets("markets"),
		minimum_wage("minimum_wage"),
		mode("mode"),
		money_total("money_total"),
		name("name"),
		nation("nation"),
		nations("nations"),
		need("need"),
		opex("opex"),
		opex_factor("opex_factor"),
		opex_revenue("opex_revenue"),
		output_product("output_product"),
		output_scale("output_scale"),
		owner("owner"),
		pop("pop"),
		population("population"),
		ports("ports"),
		price("price"),
		price_local("price_local"),
		produced_today("produced_today"),
		produced_total("produced_total"),
		productivity_index("productivity_index"),
		product("product"),
		production("production"),
		products("products"),
		profit("profit"),
		quality("quality"),
		recruit_reserve("recruit_reserve"),
		remaining_kt("remaining_kt"),
		resource("resource"),
		retirees("retirees"),
		revenue("revenue"),
		satisfaction("satisfaction"),
		savings("savings"),
		scope("scope"),
		service_cash("service_cash"),
		service_jobs("service_jobs"),
		service_sectors("service_sectors"),
		service_businesses("service_businesses"),
		service_bankruptcies("service_bankruptcies"),
		service_exports("service_exports"),
		service_profit("service_profit"),
		service_revenue("service_revenue"),
		service_spend("service_spend"),
		service_wage("service_wage"),
		service_workers("service_workers"),
		service_jobs_filled("service_jobs_filled"),
		sector_workers("sector_workers"),
		sector_share("sector_share"),
		in_kind_food("in_kind_food"),
		shortage("shortage"),
		simulation_population("simulation_population"),
		site_suitability("site_suitability"),
		sold("sold"),
		stock("stock"),
		stockpile("stockpile"),
		students("students"),
		studying("studying"),
		suitability("suitability"),
		supply("supply"),
		surplus("surplus"),
		tag("tag"),
		taxes("taxes"),
		to_city("to_city"),
		to_nation("to_nation"),
		trade_orders("trade_orders"),
		transfers("transfers"),
		transit_hours("transit_hours"),
		transport_cash("transport_cash"),
		unemployed("unemployed"),
		unemployed_by_level("unemployed_by_level"),
		unemployment_by_level("unemployment_by_level"),
		utilization("utilization"),
		vacancies_by_level("vacancies_by_level"),
		wage("wage"),
		wage_by_level("wage_by_level"),
		wage_income("wage_income"),
		wage_income_by_level("wage_income_by_level"),
		wage_rigidity("wage_rigidity"),
		workers("workers"),
		working_age("working_age"),
		x("x"),
		y("y"),
		host_city("host_city"),
		capacity_t_per_day("capacity_t_per_day"),
		delay_hours("delay_hours"),
		level("level"),
		queue("queue"),
		throughput("throughput"),
		recipe("recipe"),
		inputs("inputs"),
		byproducts("byproducts"),
		qty("qty"),
		capital_per_capacity("capital_per_capacity"),
		last_utilization("last_utilization"),
		strategic("strategic"),
		yield("yield"),
		from("from"),
		to("to"),
		from_px("from_px"),
		to_px("to_px"),
		distance_km("distance_km"),
		load_tons("load_tons"),
		trips("trips"),
		speed_kmh("speed_kmh"),
		batch_capacity_tons("batch_capacity_tons"),
		dispatched("dispatched"),
		cost("cost"),
		needs_level("needs_level"),
		needs_satisfaction("needs_satisfaction"),
		basic_satisfaction("basic_satisfaction"),
		life_satisfaction("life_satisfaction"),
		luxury_satisfaction("luxury_satisfaction"),
		needs_spend("needs_spend"),
		durable_spend("durable_spend"),
		durable_stock("durable_stock"),
		government_goods_spend("government_goods_spend"),
		trucks("trucks"),
		ships("ships"),
		planes("planes"),
		fleet("fleet"),
		fuel("fuel"),
		spare_parts("spare_parts"),
		health("health"),
		fuel_used("fuel_used"),
		missed_no_fleet("missed_no_fleet"),
		missed_no_fuel("missed_no_fuel"),
		source("source"),
		target_trucks("target_trucks"),
		target_ships("target_ships"),
		target_planes("target_planes"),
		currency("currency"),
		currencies("currencies"),
		code("code"),
		rate("rate"),
		regime("regime"),
		trade_balance("trade_balance"),
		fallback("fallback"),
		tariff("tariff"),
		tariffs("tariffs"),
		seller("seller"),
		buyer("buyer"),
		origin_price("origin_price"),
		origin_price_anchor("origin_price_anchor"),
		delivered_price("delivered_price"),
		fx_rate("fx_rate"),
		seller_currency("seller_currency"),
		buyer_currency("buyer_currency"),
		seller_fx_rate("seller_fx_rate"),
		tariff_revenue("tariff_revenue"),
		owner_kind("owner_kind"),
		owner_group("owner_group"),
		owner_nation("owner_nation"),
		local_share("local_share"),
		groups("groups")
{}

void EconomySim::demo_fill_city_status(std::size_t city_index,
		DemoStatusCitySlot& slot, double in_transit) const {
	const DemoCity& city = demo_cities_[city_index];
	const DemoStatusKeys& k = demo_status_keys_;
	Dictionary& dict = slot.dict;
	// Taxa de câmbio da nação, resolvida UMA vez por cidade (antes eram 2
	// lookups de moeda por cidade: aqui e no bloco de mercados).
	const double fx_rate = demo_fx_rate(city.nation);

	// Strings estáveis: comparam a origem (memcmp) antes de realocar a String.
	// `nation`/`home_nation` entram aqui porque o território pode mudar com a
	// política; id, nome e massa de terra são imutáveis após a inicialização.
	auto sync_string = [&dict](std::string& src, String& stored,
			const std::string& value, const String& key) {
		if (src != value) {
			src = value;
			stored = String(value.c_str());
			dict[key] = stored;
		}
	};
	sync_string(slot.id_src, slot.id, city.id, k.id);
	sync_string(slot.name_src, slot.name, city.name, k.name);
	sync_string(slot.nation_src, slot.nation, city.nation, k.nation);
	sync_string(slot.home_src, slot.home_nation, city.home_nation, k.home_nation);
	sync_string(slot.landmass_src, slot.landmass, city.landmass, k.landmass);
	sync_string(slot.currency_src, slot.currency,
		std::string(demo_currency_code(city.nation)), k.currency);

	// Campos que só mudam quando um dado do território muda (aptidão, demanda
	// externa, posição): reescritos apenas quando a versão estática sobe.
	if (!slot.static_filled || slot.static_version != demo_status_static_version_) {
		dict[k.x] = city.center.x;
		dict[k.y] = city.center.y;
		slot.center.resize(2);
		slot.center[0] = city.center.x;
		slot.center[1] = city.center.y;
		dict[k.center] = slot.center;

		slot.suitability.resize(demo_products_);
		slot.external_demand.resize(demo_products_);
		for (int product = 0; product < demo_products_; ++product) {
			slot.suitability[product] = city.suitability[product];
			slot.external_demand[product] = city.external_demand[product];
		}
		dict[k.suitability] = slot.suitability;
		dict[k.external_demand] = slot.external_demand;
		slot.static_filled = true;
		slot.static_version = demo_status_static_version_;
	}

	// `population` = populacao real exibida; `sim_population` = unidade de
	// simulacao. As divisoes per-capita usam a segunda, para nao mudar com o
	// rateio nacional (o PIB nasce da populacao de simulacao).
	//
	// Cada escrita é uma chamada à ponte GDExtension com hash da chave
	// (~200 ns): 25 campos × 834 cidades custavam ~4 ms/dia. População,
	// população de simulação e os indicadores derivados ficam parados em boa
	// parte das cidades — comparar double antes evita reescrevê-los.
	double* scalar_mirror = slot.scalar_written;
	bool& scalar_filled = slot.scalar_written_filled;
	int scalar_index = 0;
	auto sync_scalar = [&dict, &k, scalar_mirror, &scalar_filled, &scalar_index](
			StringName key, double value) {
		if (!scalar_filled || scalar_mirror[scalar_index] != value) {
			dict[key] = value;
			scalar_mirror[scalar_index] = value;
		}
		++scalar_index;
	};
	sync_scalar(k.population, city.population);
	sync_scalar(k.simulation_population, city.simulation_population);
	sync_scalar(k.gdp, city.gdp);
	sync_scalar(k.gdp_per_capita, city.simulation_population > 0.0
		? city.gdp / city.simulation_population : 0.0);
	sync_scalar(k.external_income_index, city.external_income_index);
	sync_scalar(k.firm_cash, city.firm_cash);
	sync_scalar(k.service_cash, city.service_cash);
	sync_scalar(k.government_budget, city.government_budget);
	sync_scalar(k.government_goods_spend, city.government_goods_spend);
	sync_scalar(k.service_revenue, city.service_revenue);
	sync_scalar(k.service_jobs, city.service_jobs);
	sync_scalar(k.service_workers, city.service_workers);
	sync_scalar(k.service_jobs_filled, city.service_jobs_filled);
	sync_scalar(k.in_kind_food, city.in_kind_food);
	sync_scalar(k.service_profit, city.service_profit);
	sync_scalar(k.opex_revenue, city.opex_revenue);
	sync_scalar(k.freight_revenue, city.freight_revenue);
	sync_scalar(k.construction_revenue, city.construction_revenue);
	sync_scalar(k.service_wage, city.service_jobs > 0.0
		? city.service_wage_bill / city.service_jobs : 0.0);
	sync_scalar(k.disposable_per_capita, city.simulation_population > 0.0
		? city.pop.disposable / city.simulation_population : 0.0);

	// Padrão de vida: quantas cestas alimentares diárias a renda disponível
	// por pessoa compra. Diferente do PIB per capita, usa renda das famílias.
	double cheapest_price = 0.0;
	for (int food = 0; food < static_cast<int>(demo_food_products_.size()); ++food) {
		const int product = demo_food_product(food);
		const double factor = demo_food_calories(product);
		if (factor <= 0.0) {
			continue;
		}
		const double per_calorie = city.markets[product].price / factor;
		if (per_calorie > 0.0
				&& (cheapest_price <= 0.0 || per_calorie < cheapest_price)) {
			cheapest_price = per_calorie;
		}
	}
	const double basket_cost = kDemoNeedPerCapita * cheapest_price;
	const double living_standard = basket_cost > 0.0
		? (city.pop.disposable / city.simulation_population) / basket_cost : 0.0;
	sync_scalar(k.living_standard, living_standard);

	// Indicadores derivados, no espírito do formato legado, para a camada
	// visual: produção primária (t/dia), empregos, renda relativa, grau de
	// industrialização, estoque de trigo e necessidade diária.
	double production = 0.0;
	double secondary_capacity = 0.0;
	double total_capacity = 0.0;
	const std::vector<int>& local_buildings = demo_buildings_by_city_[city_index];
	for (const int bi : local_buildings) {
		const DemoBuilding& building = demo_buildings_[static_cast<std::size_t>(bi)];
		total_capacity += building.capacity * building.output_scale;
		if (building.input_product < 0) {
			production += building.capacity * building.output_scale;
		} else {
			secondary_capacity += building.capacity * building.output_scale;
		}
	}
	sync_scalar(k.production, production);
	sync_scalar(k.jobs, city.pop.employed);
	sync_scalar(k.income, std::clamp(living_standard, 0.0, 1.0));
	sync_scalar(k.industrialization, total_capacity > 0.0
		? std::clamp(secondary_capacity / total_capacity, 0.0, 1.0) : 0.0);
	sync_scalar(k.stockpile, city.markets[0].stock);
	sync_scalar(k.need, city.pop.need);
	sync_scalar(k.deficit, std::max(0.0, city.pop.need - city.pop.consumed));
	sync_scalar(k.consumed, city.pop.consumed);
	sync_scalar(k.in_transit, in_transit);
	sync_scalar(k.fx_rate, fx_rate);
	sync_scalar(k.tariff_revenue, city.tariff_revenue);
	sync_scalar(k.productivity_index, demo_effective_productivity(city));
	slot.scalar_written_filled = true;

	// F9: emprego setorial (4 posições). Espelho de 8 doubles: só reescreve o
	// PackedFloat64Array quando algum valor muda (o custo por escrita é a
	// ponte GDExtension, não o cálculo).
	{
		bool changed = !slot.sector_written_filled
			|| slot.sector_workers.size() != 4;
		if (!changed) {
			for (int i = 0; i < 4; ++i) {
				if (slot.sector_written[i] != city.sector_workers[i]
						|| slot.sector_written[4 + i] != city.sector_share[i]) {
					changed = true;
					break;
				}
			}
		}
		if (changed) {
			slot.sector_workers.resize(4);
			slot.sector_share.resize(4);
			for (int i = 0; i < 4; ++i) {
				slot.sector_workers[i] = city.sector_workers[i];
				slot.sector_share[i] = city.sector_share[i];
				slot.sector_written[i] = city.sector_workers[i];
				slot.sector_written[4 + i] = city.sector_share[i];
			}
			dict[k.sector_workers] = slot.sector_workers;
			dict[k.sector_share] = slot.sector_share;
			slot.sector_written_filled = true;
		}
	}

	// Reservas minerais declaradas (só produtos com jazida conhecida; ausência
	// da chave = sem dado, não "zero"). O bloco ESTÁTICO (reserva inicial,
	// qualidade, profundidade, eficiência, opex) é escrito uma vez; por dia só
	// mudam reserva restante, extraído e esgotado. Antes o dicionário inteiro
	// era reconstruído a cada dia — custo que crescia com as cidades mineiras
	// do cenário real (o mundo sintético do bench não tem depósito nenhum).
	if (slot.deposit_entry.size() < demo_products_) {
		slot.deposit_entry.resize(static_cast<std::size_t>(demo_products_));
		slot.deposit_present.assign(static_cast<std::size_t>(demo_products_), 0);
		slot.deposit_depleted.assign(static_cast<std::size_t>(demo_products_), 0);
		slot.deposit_written.assign(static_cast<std::size_t>(demo_products_),
			DemoDeposit{});
		slot.deposit_key.clear();
		slot.deposit_key.reserve(static_cast<std::size_t>(demo_products_));
		for (int product = 0; product < demo_products_; ++product) {
			slot.deposit_key.emplace_back(demo_product_name(product));
		}
	}
	// Jazida descoberta depois do primeiro dia (a IA/geografia chama
	// `demo_set_deposit`): a lista de jazidas rastreadas é recalculada uma vez e
	// só as novas entram no dicionário (as antigas mantêm o bloco estático).
	if (slot.deposit_tracked_version != demo_status_deposit_version_) {
		slot.deposit_tracked.clear();
		for (int product = 0; product < demo_products_; ++product) {
			if (city.deposits[product].tracked) {
				slot.deposit_tracked.push_back(product);
			}
		}
		slot.deposit_tracked_version = demo_status_deposit_version_;
	}
	// `deposits` só é remontado quando a cidade tem jazida rastreada. Cidade
	// sem jazida publica o dicionário vazio UMA vez (a chave sempre existe:
	// ausência seria "sem dado", não "sem reserva"); sem isso o bloco custava
	// ~4,5 ms/dia só de referenciar 834 dicionários.
	if (slot.deposit_tracked.empty()) {
		if (!slot.deposits_published) {
			dict[k.deposits] = slot.deposits;
			slot.deposits_published = true;
		}
	} else {
		for (const int product : slot.deposit_tracked) {
			const DemoDeposit& deposit = city.deposits[product];
			Dictionary& entry = slot.deposit_entry[static_cast<std::size_t>(product)];
			DemoDeposit& previous = slot.deposit_written[static_cast<std::size_t>(product)];
			const bool first = slot.deposit_present[static_cast<std::size_t>(product)] == 0;
			if (first) {
				entry[k.initial_kt] = deposit.initial_kt;
				entry[k.quality] = deposit.quality;
				entry[k.depth_m] = deposit.depth_m;
				entry[k.efficiency] = demo_extraction_efficiency(deposit);
				entry[k.opex_factor] = demo_depth_opex_factor(deposit);
				slot.deposits[slot.deposit_key[static_cast<std::size_t>(product)]] = entry;
				slot.deposit_present[static_cast<std::size_t>(product)] = 1;
			}
			if (first || previous.remaining_kt != deposit.remaining_kt) {
				entry[k.remaining_kt] = deposit.remaining_kt;
				previous.remaining_kt = deposit.remaining_kt;
			}
			if (first || previous.extracted_kt != deposit.extracted_kt) {
				entry[k.extracted_kt] = deposit.extracted_kt;
				previous.extracted_kt = deposit.extracted_kt;
			}
			const bool depleted = deposit.remaining_kt <= 0.0;
			if (first || slot.deposit_depleted[static_cast<std::size_t>(product)] != (depleted ? 1 : 0)) {
				entry[k.depleted] = depleted;
				slot.deposit_depleted[static_cast<std::size_t>(product)] = depleted ? 1 : 0;
			}
		}
		// Dictionary é por referência no Godot 4: uma única atribuição basta —
		// as entradas novas e os valores atualizados aparecem no publicado.
		// (Reatribuir todo dia custava ~1,4 µs × 662 cidades ≈ 0,9 ms.)
		if (!slot.deposits_published) {
			dict[k.deposits] = slot.deposits;
			slot.deposits_published = true;
		}
	}

	Dictionary& pop = slot.pop;
	// Valores diários: só reescreve o que mudou. Cada `pop[chave] = valor` é
	// uma chamada à ponte GDExtension com hash da chave (~190 ns); com 20
	// campos × 834 cidades isso custava ~3 ms/dia. `population`,
	// `labor_force` e `dependents` quase nunca mudam.
	{
		double* mirror = slot.pop_written;
		bool& filled = slot.pop_written_filled;
		int index = 0;
		auto sync_pop = [&pop, &k, mirror, &filled, &index](StringName key, double value) {
			if (!filled || mirror[index] != value) {
				pop[key] = value;
				mirror[index] = value;
			}
			++index;
		};
		sync_pop(k.population, city.pop.population);
		sync_pop(k.labor_force, city.pop.labor_force);
		sync_pop(k.employed, city.pop.employed);
		sync_pop(k.unemployed, city.pop.unemployed);
		sync_pop(k.dependents, city.pop.dependents);
		sync_pop(k.wage_income, city.pop.wage_income);
		sync_pop(k.capital_income, city.pop.capital_income);
		sync_pop(k.transfers, city.pop.transfers);
		sync_pop(k.taxes, city.pop.taxes);
		sync_pop(k.service_spend, city.pop.service_spend);
		sync_pop(k.disposable, city.pop.disposable);
		sync_pop(k.external_income, city.pop.external_income);
		sync_pop(k.food_spend, city.pop.food_spend);
		sync_pop(k.consumed, city.pop.consumed);
		sync_pop(k.need, city.pop.need);
		sync_pop(k.satisfaction, city.pop.satisfaction);
		sync_pop(k.savings, city.pop.savings);
		sync_pop(k.needs_level, double(city.pop.needs_level));
		sync_pop(k.needs_satisfaction, city.pop.needs_satisfaction);
		sync_pop(k.basic_satisfaction, city.pop.level_satisfaction.size() > 0
			? city.pop.level_satisfaction[0] : 0.0);
		sync_pop(k.life_satisfaction, city.pop.level_satisfaction.size() > 1
			? city.pop.level_satisfaction[1] : 0.0);
		sync_pop(k.luxury_satisfaction, city.pop.level_satisfaction.size() > 2
			? city.pop.level_satisfaction[2] : 0.0);
		sync_pop(k.needs_spend, city.pop.needs_spend);
		sync_pop(k.durable_spend, city.pop.durable_spend);
		double durable_total = 0.0;
		for (const double tons : city.pop.durable_stock) {
			durable_total += tons;
		}
		sync_pop(k.durable_stock, durable_total);
		// F7: população ativa, fora da força e mercado de trabalho.
		sync_pop(k.working_age, city.pop.working_age);
		sync_pop(k.retirees, city.pop.retirees);
		sync_pop(k.students, city.pop.students);
		sync_pop(k.recruit_reserve, city.pop.recruit_reserve);
		sync_pop(k.income_level, double(city.pop.income_level));
		const auto minimum_wage = demo_minimum_wages_.find(city.nation);
		sync_pop(k.minimum_wage, minimum_wage != demo_minimum_wages_.end()
			? std::max(0.0, minimum_wage->second) : 0.0);
		sync_pop(k.wage_rigidity, demo_wage_rigidity_);
		if (!filled) {
			slot.education.resize(kDemoEducationLevels);
			slot.education_share.resize(kDemoEducationLevels);
			slot.labor_force_by_level.resize(kDemoEducationLevels);
			slot.employed_by_level.resize(kDemoEducationLevels);
			slot.unemployed_by_level.resize(kDemoEducationLevels);
			slot.unemployment_by_level.resize(kDemoEducationLevels);
			slot.vacancies_by_level.resize(kDemoEducationLevels);
			slot.wage_by_level.resize(kDemoEducationLevels);
			slot.wage_income_by_level.resize(kDemoEducationLevels);
			slot.studying.resize(kDemoEducationLevels - 1);
			slot.income_level_share.resize(kDemoIncomeLevels);
			slot.income_level_by_stratum.resize(kDemoEducationLevels);
			slot.stratum_disposable.resize(kDemoEducationLevels);
			slot.stratum_population.resize(kDemoEducationLevels);
			slot.stratum_wage_income.resize(kDemoEducationLevels);
		}
		// Grupo "raro": educação, força por nível, estudo, fatias de renda e o
		// nível de consumo de cada estrato. Compara com o último publicado e só
		// toca nos Arrays quando algo muda (mudam em escala de semanas).
		{
			double rare[16];
			int ri = 0;
			for (int level = 0; level < kDemoEducationLevels; ++level) {
				rare[ri++] = city.pop.education[level];
			}
			for (int level = 0; level < kDemoEducationLevels; ++level) {
				rare[ri++] = city.pop.labor_force_by_level[level];
			}
			for (int track = 0; track < kDemoEducationLevels - 1; ++track) {
				rare[ri++] = city.pop.studying[track];
			}
			for (int level = 0; level < kDemoIncomeLevels; ++level) {
				rare[ri++] = city.pop.income_level_share[level];
			}
			for (int stratum = 0; stratum < kDemoEducationLevels; ++stratum) {
				rare[ri++] = double(city.pop.stratum_income_level[stratum]);
			}
			if (!slot.labor_rare_filled
					|| std::memcmp(rare, slot.labor_rare_written,
						sizeof(double) * static_cast<std::size_t>(ri)) != 0) {
				for (int level = 0; level < kDemoEducationLevels; ++level) {
					slot.education[level] = city.pop.education[level];
					slot.labor_force_by_level[level] =
						city.pop.labor_force_by_level[level];
				}
				for (int track = 0; track < kDemoEducationLevels - 1; ++track) {
					slot.studying[track] = city.pop.studying[track];
				}
				for (int level = 0; level < kDemoIncomeLevels; ++level) {
					slot.income_level_share[level] =
						city.pop.income_level_share[level];
				}
				for (int stratum = 0; stratum < kDemoEducationLevels; ++stratum) {
					slot.income_level_by_stratum[stratum] =
						city.pop.stratum_income_level[stratum];
				}
				std::memcpy(slot.labor_rare_written, rare,
					sizeof(double) * static_cast<std::size_t>(ri));
				slot.labor_rare_filled = true;
			}
		}
		// Grupo "diário": salários, emprego, vagas e folha por escolaridade.
		{
			double daily[24];
			int di = 0;
			for (int level = 0; level < kDemoEducationLevels; ++level) {
				daily[di++] = city.pop.working_age > 1e-9
					? city.pop.education[level] / city.pop.working_age : 0.0;
				daily[di++] = city.pop.employed_by_level[level];
				daily[di++] = city.pop.unemployed_by_level[level];
				daily[di++] = city.pop.labor_force_by_level[level] > 1e-9
					? city.pop.unemployed_by_level[level]
						/ city.pop.labor_force_by_level[level] : 0.0;
				daily[di++] = city.pop.vacancies_by_level[level];
				daily[di++] = city.pop.wage_by_level[level];
				daily[di++] = city.pop.wage_income_by_level[level];
			}
			if (!slot.labor_daily_filled
					|| std::memcmp(daily, slot.labor_daily_written,
						sizeof(double) * static_cast<std::size_t>(di)) != 0) {
				for (int level = 0; level < kDemoEducationLevels; ++level) {
					slot.education_share[level] = city.pop.working_age > 1e-9
						? city.pop.education[level] / city.pop.working_age : 0.0;
					slot.employed_by_level[level] =
						city.pop.employed_by_level[level];
					slot.unemployed_by_level[level] =
						city.pop.unemployed_by_level[level];
					slot.unemployment_by_level[level] =
						city.pop.labor_force_by_level[level] > 1e-9
						? city.pop.unemployed_by_level[level]
							/ city.pop.labor_force_by_level[level] : 0.0;
					slot.vacancies_by_level[level] =
						city.pop.vacancies_by_level[level];
					slot.wage_by_level[level] = city.pop.wage_by_level[level];
					slot.wage_income_by_level[level] =
						city.pop.wage_income_by_level[level];
				}
				std::memcpy(slot.labor_daily_written, daily,
					sizeof(double) * static_cast<std::size_t>(di));
				slot.labor_daily_filled = true;
			}
		}
		// Renda por estrato: o memcmp evita reescrever os três vetores quando
		// os valores não mudaram (população e folha do estrato variam devagar).
		{
			double strata[12];
			int si = 0;
			for (int stratum = 0; stratum < kDemoEducationLevels; ++stratum) {
				strata[si++] = city.pop.stratum_disposable[stratum];
				strata[si++] = city.pop.stratum_population[stratum];
				strata[si++] = city.pop.stratum_wage_income[stratum];
			}
			if (!slot.stratum_written_filled
					|| std::memcmp(strata, slot.stratum_written,
						sizeof(double) * static_cast<std::size_t>(si)) != 0) {
				for (int stratum = 0; stratum < kDemoEducationLevels; ++stratum) {
					slot.stratum_disposable[stratum] =
						city.pop.stratum_disposable[stratum];
					slot.stratum_population[stratum] =
						city.pop.stratum_population[stratum];
					slot.stratum_wage_income[stratum] =
						city.pop.stratum_wage_income[stratum];
				}
				std::memcpy(slot.stratum_written, strata,
					sizeof(double) * static_cast<std::size_t>(si));
				slot.stratum_written_filled = true;
			}
		}
		if (!filled) {
			pop[k.income_level_share] = slot.income_level_share;
			pop[k.income_level_by_stratum] = slot.income_level_by_stratum;
			pop[k.stratum_disposable] = slot.stratum_disposable;
			pop[k.stratum_population] = slot.stratum_population;
			pop[k.stratum_wage_income] = slot.stratum_wage_income;
			pop[k.education] = slot.education;
			pop[k.education_share] = slot.education_share;
			pop[k.labor_force_by_level] = slot.labor_force_by_level;
			pop[k.employed_by_level] = slot.employed_by_level;
			pop[k.unemployed_by_level] = slot.unemployed_by_level;
			pop[k.unemployment_by_level] = slot.unemployment_by_level;
			pop[k.vacancies_by_level] = slot.vacancies_by_level;
			pop[k.wage_by_level] = slot.wage_by_level;
			pop[k.wage_income_by_level] = slot.wage_income_by_level;
			pop[k.studying] = slot.studying;
		}
		if (!filled) {
			slot.desired_by_category.resize(demo_food_products_.size());
			slot.consumed_by_category.resize(demo_food_products_.size());
		}
		for (int food = 0; food < static_cast<int>(demo_food_products_.size()); ++food) {
			slot.desired_by_category[food] = city.pop.desired_by_category[food];
			slot.consumed_by_category[food] = city.pop.consumed_by_category[food];
		}
		if (!filled) {
			pop[k.desired_by_category] = slot.desired_by_category;
			pop[k.consumed_by_category] = slot.consumed_by_category;
		} else {
			bool changed = false;
			for (int food = 0; food < static_cast<int>(demo_food_products_.size()); ++food) {
				if (slot.pop_food_written[food] != city.pop.desired_by_category[food]
						|| slot.pop_food_written[demo_food_products_.size() + food]
							!= city.pop.consumed_by_category[food]) {
					changed = true;
					break;
				}
			}
			if (changed) {
				pop[k.desired_by_category] = slot.desired_by_category;
				pop[k.consumed_by_category] = slot.consumed_by_category;
			}
		}
		for (int food = 0; food < static_cast<int>(demo_food_products_.size()); ++food) {
			slot.pop_food_written[food] = city.pop.desired_by_category[food];
			slot.pop_food_written[demo_food_products_.size() + food] =
				city.pop.consumed_by_category[food];
		}
		filled = true;
	}
	if (!slot.pop_published) {
		dict[k.pop] = pop;
		slot.pop_published = true;
	}

	// `resize` com o mesmo tamanho já provoca copy-on-write no Array
	// compartilhado com o dicionário publicado (medido: ~4,5 ms/dia, 114
	// Variants copiados por cidade). Só redimensiona quando muda de fato.
	if (slot.markets.size() != demo_products_) {
		slot.markets.resize(demo_products_);
	}
	if (slot.market_entry.size() < demo_products_) {
		slot.market_entry.resize(static_cast<std::size_t>(demo_products_));
		slot.market_written.resize(static_cast<std::size_t>(demo_products_));
		slot.market_written_filled.assign(static_cast<std::size_t>(demo_products_), 0);
		slot.market_local_written.assign(static_cast<std::size_t>(demo_products_), 0.0);
		slot.io_written.assign(static_cast<std::size_t>(4 * demo_products_), 0.0);
		slot.io_filled.assign(4, 0);
	}
	// Preço local é EXIBIÇÃO: âncora ÷ taxa da moeda da nação (F5, opção c).
	// Não volta para a simulação; o valor na âncora é a fonte de verdade.
	const double local_rate = std::max(1e-9, fx_rate);
	bool markets_dirty = false;
	for (int product = 0; product < demo_products_; ++product) {
		const DemoMarket& source = city.markets[product];
		DemoMarket& previous = slot.market_written[static_cast<std::size_t>(product)];
		const double local = source.price / local_rate;
		if (slot.market_written_filled[static_cast<std::size_t>(product)] == 0) {
			// Primeira gravação desta cidade: cria as chaves do mercado.
			Dictionary& market = slot.market_entry[static_cast<std::size_t>(product)];
			market[k.product] = product;
			market[k.stock] = source.stock;
			market[k.price] = source.price;
			market[k.price_local] = local;
			market[k.demand] = source.demand;
			market[k.supply] = source.supply;
			market[k.desired] = source.desired;
			market[k.sold] = source.sold;
			market[k.shortage] = source.shortage;
			market[k.surplus] = source.surplus;
			previous = source;
			slot.market_local_written[static_cast<std::size_t>(product)] = local;
			slot.market_written_filled[static_cast<std::size_t>(product)] = 1;
			slot.markets[product] = market;
			markets_dirty = true;
			continue;
		}
		// Caminho comum (mercado publicado igual ao dia anterior): NENHUM
		// acesso ao Dictionary. A comparação cobre os 8 campos publicados, que
		// são contíguos (ver DemoMarket): um memcmp de 64 bytes por produto.
		// `sold_prev` é interno e muda TODO dia (`sold_prev = sold` no reset);
		// por isso fica no fim do struct, fora do bloco comparado.
		const bool local_changed = slot.market_local_written[
			static_cast<std::size_t>(product)] != local;
		if (!local_changed && std::memcmp(&previous, &source, sizeof(double) * 8) == 0) {
			continue;
		}
		markets_dirty = true;
		Dictionary& market = slot.market_entry[static_cast<std::size_t>(product)];
		if (local_changed) {
			market[k.price_local] = local;
			slot.market_local_written[static_cast<std::size_t>(product)] = local;
		}
		if (previous.stock != source.stock) {
			market[k.stock] = source.stock;
			previous.stock = source.stock;
		}
		if (previous.price != source.price) {
			market[k.price] = source.price;
			previous.price = source.price;
		}
		if (previous.demand != source.demand) {
			market[k.demand] = source.demand;
			previous.demand = source.demand;
		}
		if (previous.supply != source.supply) {
			market[k.supply] = source.supply;
			previous.supply = source.supply;
		}
		if (previous.desired != source.desired) {
			market[k.desired] = source.desired;
			previous.desired = source.desired;
		}
		if (previous.sold != source.sold) {
			market[k.sold] = source.sold;
			previous.sold = source.sold;
		}
		if (previous.shortage != source.shortage) {
			market[k.shortage] = source.shortage;
			previous.shortage = source.shortage;
		}
		if (previous.surplus != source.surplus) {
			market[k.surplus] = source.surplus;
			previous.surplus = source.surplus;
		}
	}
	// Sem nenhuma mudança o Array publicado continua o mesmo objeto: reatribuir
	// só custaria uma chamada à ponte GDExtension por cidade (~0,3 ms/dia).
	if (markets_dirty) {
		dict[k.markets] = slot.markets;
	}

	// F8: bloco plano de serviços (20 × 12). Só reescreve quando algum valor
	// muda; o Array publicado é reutilizado pela UI (ordem de service_sectors).
	if (demo_services_loaded_) {
		PackedFloat64Array& services = slot.services;
		services.resize(kDemoMaxServiceSectors * kDemoServiceFields);
		double* out = services.ptrw();
		for (int sector = 0; sector < kDemoMaxServiceSectors; ++sector) {
			const DemoServiceMarket& market = city.service_markets[sector];
			double* base = out + sector * kDemoServiceFields;
			base[0] = market.demand;
			base[1] = market.supply;
			base[2] = market.price;
			base[3] = market.revenue;
			base[4] = market.jobs;
			base[5] = market.businesses;
			base[6] = market.unmet;
			base[7] = market.exports;
			base[8] = market.export_revenue;
			base[9] = market.coverage;
			base[10] = market.input_cost;
			base[11] = market.service_cost;
		}
		bool dirty = !slot.services_filled;
		if (!dirty) {
			const double* previous = slot.services_written.ptr();
			for (int index = 0; index < services.size(); ++index) {
				if (previous[index] != out[index]) {
					dirty = true;
					break;
				}
			}
		}
		if (dirty) {
			slot.services_written = services;
			dict[k.services] = services;
			slot.services_filled = true;
		}
	}

	// `Array` é tipo por referência no Godot 4 (validado em teste): mutar
	// `slot.imported` já aparece no dicionário publicado. Redimensionar para o
	// mesmo tamanho era uma chamada à ponte por array/cidade/dia sem efeito.
	if (slot.imported.size() != demo_products_) {
		slot.imported.resize(demo_products_);
		slot.exported.resize(demo_products_);
		slot.import_value.resize(demo_products_);
		slot.export_value.resize(demo_products_);
	}
	{
		// Quatro arrays de fluxo, um por produto (demo_products_): idem
		// mercados, só produtos com comércio mudam (o resto fica em 0 dia após
		// dia) e a linha inteira é comparada com um memcmp antes do hash.
		const double* source_values[4] = {
			city.imported, city.exported, city.import_value, city.export_value};
		Array* targets[4] = {
			&slot.imported, &slot.exported, &slot.import_value, &slot.export_value};
		const StringName* keys[4] = {&k.imported, &k.exported, &k.import_value, &k.export_value};
		for (int array_index = 0; array_index < 4; ++array_index) {
			Array& target = *targets[array_index];
			double* previous = &slot.io_written[static_cast<std::size_t>(
				array_index * demo_products_)];
			const bool first = slot.io_filled[static_cast<std::size_t>(array_index)] == 0;
			// Linha inteira igual ao status anterior: nenhum hash a pagar.
			if (!first && std::memcmp(previous, source_values[array_index],
					sizeof(double) * static_cast<std::size_t>(demo_products_)) == 0) {
				continue;
			}
			for (int product = 0; product < demo_products_; ++product) {
				const double value = source_values[array_index][product];
				if (first || previous[product] != value) {
					target[product] = value;
					previous[product] = value;
				}
			}
			if (first) {
				dict[*keys[array_index]] = target;
				slot.io_filled[static_cast<std::size_t>(array_index)] = 1;
			}
		}
	}

	if (slot.buildings.size() != local_buildings.size()) {
		slot.buildings.resize(local_buildings.size());
		slot.buildings_bound = false;
	}
	if (slot.building_entry.size() < local_buildings.size()) {
		slot.building_entry.resize(local_buildings.size());
		slot.building_cache.resize(local_buildings.size());
	}
	for (std::size_t index = 0; index < local_buildings.size(); ++index) {
		const DemoBuilding& building =
			demo_buildings_[static_cast<std::size_t>(local_buildings[index])];
		Dictionary& entry = slot.building_entry[index];
		DemoStatusCitySlot::BuildingCache& cached = slot.building_cache[index];
		const bool identity_changed = !cached.filled || cached.id_src != building.id;
		// F9: privatização/nacionalização muda o dono sem mudar a identidade.
		const bool owner_changed = cached.owner_src != building.owner;
		// Bloco estático: só quando a construção nasce (ou muda de identidade).
		if (identity_changed || owner_changed) {
			cached.id_src = building.id;
			cached.id = String(building.id.c_str());
			entry[k.id] = cached.id;
			entry[k.kind] = building.kind;
			cached.resource_src = building.resource;
			cached.resource = String(building.resource.c_str());
			entry[k.resource] = cached.resource;
			cached.owner_src = building.owner;
			cached.owner = String(building.owner.c_str());
			entry[k.owner] = cached.owner;
			// F9: decomposição do dono para a UI/status (sem parse no cliente).
			std::string group_id;
			std::string origin_nation;
			const int owner_kind = demo_owner_kind(building.owner, &group_id,
				&origin_nation);
			entry[k.owner_kind] = owner_kind == 0 ? String("state")
				: owner_kind == 1 ? String("national")
				: owner_kind == 2 ? String("foreign") : String("private");
			entry[k.owner_group] = String(group_id.c_str());
			entry[k.owner_nation] = origin_nation.empty()
				? String() : String(origin_nation.c_str());
			entry[k.local_share] = building.local_share;
			entry[k.input_product] = building.input_product;
			entry[k.output_product] = building.output_product;
			entry[k.capital_per_capacity] = building.capital_per_capacity;
			entry[k.strategic] = building.strategic;
			cached.recipe = -2;
			cached.filled = true;
		}
		// Reinsere o `entry` no Array publicado só quando ele nasce/muda de
		// índice; mutações do Dictionary já são visíveis pela referência.
		if (!slot.buildings_bound || identity_changed) {
			slot.buildings[index] = entry;
		}
		// Receita (insumos/subprodutos): as Arrays/Dictionaries só são
		// reconstruídos quando a receita da construção muda — nunca por dia.
		if (cached.recipe != building.recipe) {
			cached.recipe = building.recipe;
			if (building.recipe >= 0
					&& building.recipe < static_cast<int>(demo_recipes_.size())) {
				const DemoRecipe& recipe =
					demo_recipes_[static_cast<std::size_t>(building.recipe)];
				entry[k.recipe] = String(recipe.id.c_str());
				Array inputs;
				Array byproducts;
				inputs.resize(recipe.input_count);
				byproducts.resize(recipe.byproduct_count);
				for (int slot = 0; slot < recipe.input_count; ++slot) {
					Dictionary input;
					input[k.good] = String(demo_product_name(recipe.inputs[slot]));
					input[k.qty] = recipe.input_qty[slot];
					inputs[slot] = input;
				}
				for (int slot = 0; slot < recipe.byproduct_count; ++slot) {
					Dictionary byproduct;
					byproduct[k.good] = String(demo_product_name(recipe.byproducts[slot]));
					byproduct[k.qty] = recipe.byproduct_qty[slot];
					byproducts[slot] = byproduct;
				}
				entry[k.inputs] = inputs;
				entry[k.byproducts] = byproducts;
				entry[k.yield] = recipe.yield;
			} else {
				entry[k.recipe] = String();
				entry[k.inputs] = Array();
				entry[k.byproducts] = Array();
			}
		}
		// Valores diários: só reescreve o que mudou (comparar double é ~20×
		// mais barato que um Dictionary::operator[] com hash de StringName).
		auto sync_double = [&entry, &cached](StringName key, double& mirror,
				double value) {
			if (mirror != value) {
				entry[key] = value;
				mirror = value;
			}
		};
		sync_double(k.last_utilization, cached.last_utilization, building.last_utilization);
		sync_double(k.capacity, cached.capacity, building.capacity);
		sync_double(k.jobs, cached.jobs, building.jobs);
		sync_double(k.wage, cached.wage, building.wage);
		sync_double(k.opex, cached.opex, building.opex);
		sync_double(k.workers, cached.workers, building.workers);
		sync_double(k.utilization, cached.utilization, building.utilization);
		sync_double(k.output_scale, cached.output_scale, building.output_scale);
		if (cached.loss_days != static_cast<double>(building.loss_days)) {
			entry[k.loss_days] = building.loss_days;
			cached.loss_days = static_cast<double>(building.loss_days);
		}
		sync_double(k.revenue, cached.revenue, building.revenue);
		sync_double(k.input_value, cached.input_value, building.input_value);
		sync_double(k.costs, cached.costs, building.costs);
		sync_double(k.profit, cached.profit, building.profit);
		sync_double(k.site_suitability, cached.site,
			city.suitability[building.output_product]);
	}
	if (!slot.buildings_published) {
		dict[k.buildings] = slot.buildings;
		slot.buildings_published = true;
	}
	slot.buildings_bound = true;
}

Dictionary EconomySim::get_demo_status() {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	// Snapshot do dia já publicado: devolve o mesmo dicionário (a UI lê o dia
	// inteiro; mutações entre ticks só valem no próximo status).
	if (demo_status_live_ >= 0 && demo_status_built_day_ == day_) {
		return demo_status_slots_[demo_status_live_].root;
	}
	const int slot_index = demo_status_live_ < 0 ? 0 : 1 - demo_status_live_;
	DemoStatusCache& cache = demo_status_slots_[slot_index];
	const DemoStatusKeys& k = demo_status_keys_;
	Dictionary& root = cache.root;

	// Subdivisao da fase status no benchmark (sem custo no jogo): as 4 marcas
	// `OGS_SIM_PHASE_LAP(part)` abaixo gravam em `demo_status_sub_us_`.
#if defined(OGS_PROFILING)
	ogs::profile::Sink status_sink{demo_status_sub_us_, 4, &demo_profile_};
	const ogs::profile::SinkScope status_scope(&status_sink);
#endif

	root[k.mode] = "demo";
	root[k.day] = day_;
	root[k.calendar_day] = day_;
	root[k.money_total] = demo_money_total();
	root[k.transport_cash] = demo_transport_cash_;
	root[k.foreign_cash] = demo_foreign_cash_;
	root[k.construction_cash] = demo_construction_cash_;
	root[k.bankruptcies] = demo_bankruptcies_;
	root[k.depleted_mines] = demo_depleted_mines_;
	// F8: catálogo de serviços (uma vez) + contadores de negócios do dia.
	if (demo_services_loaded_ && !cache.service_sectors_filled) {
		cache.service_sectors = demo_get_service_sectors();
		root[k.service_sectors] = cache.service_sectors;
		cache.service_sectors_filled = true;
	}
	root[k.service_businesses] = demo_get_service_business_count();
	root[k.service_bankruptcies] = demo_service_bankruptcies_;

	// Cabeçalho: os contêineres são pendurados no dicionário UMA vez; a partir
	// daí só são redimensionados/reescritos no lugar.
	if (!cache.header_filled) {
		cache.products.resize(demo_products_);
		for (int index = 0; index < demo_products_; ++index) {
			cache.products[index] = String(demo_product_name(index));
		}
		cache.tot_produced.resize(demo_products_);
		cache.tot_consumed.resize(demo_products_);
		cache.tot_produced_today.resize(demo_products_);
		cache.tot_consumed_today.resize(demo_products_);
		root[k.capital_cost_per_capacity] = kDemoCapitalPerCapacity;
		root[k.products] = cache.products;
		root[k.cities] = cache.cities;
		root[k.ports] = cache.ports;
		root[k.trade_orders] = cache.orders;
		root[k.nations] = cache.nations;
		root[k.currencies] = cache.currencies;
		root[k.tariffs] = cache.tariffs;
		root[k.invariant_violations] = cache.violations;
		root[k.market_totals] = cache.market_totals;
		root[k.produced_total] = cache.tot_produced;
		root[k.consumed_total] = cache.tot_consumed;
		root[k.produced_today] = cache.tot_produced_today;
		root[k.consumed_today] = cache.tot_consumed_today;
		cache.header_filled = true;
	}

	cache.violations.resize(demo_invariant_violations_.size());
	for (std::size_t index = 0; index < demo_invariant_violations_.size(); ++index) {
		cache.violations[index] = String(demo_invariant_violations_[index].c_str());
	}

	// Carga em trânsito por cidade numa passada única. Antes, cada cidade
	// varria TODAS as cargas do mundo (O(cidades × cargas) por dia).
	cache.in_transit_by_city.assign(demo_cities_.size(), 0.0);
	for (const DemoShipment& shipment : demo_in_transit_) {
		const auto found = demo_city_lookup_.find(shipment.to_city);
		if (found != demo_city_lookup_.end()) {
			cache.in_transit_by_city[static_cast<std::size_t>(found->second)]
				+= shipment.amount;
		}
	}
	OGS_SIM_PHASE_LAP(3);

	// Totais do sistema por produto — a mesma soma que a IA fazia em GDScript
	// sobre `cities[]` (858 cidades × 21 mercados, ~13 ms no frame da virada).
	// Aqui é uma passada de doubles na memória do núcleo, na thread do tick.
	cache.tot_surplus.resize(demo_products_);
	cache.tot_sold.resize(demo_products_);
	cache.tot_demand.resize(demo_products_);
	for (int product = 0; product < demo_products_; ++product) {
		cache.tot_surplus[product] = 0.0;
		cache.tot_sold[product] = 0.0;
		cache.tot_demand[product] = 0.0;
	}
	// Soma só os valores positivos, na ordem das cidades: idêntico (bit a bit)
	// ao laço que o `PrivateInvestor.prepare_totals` fazia.
	for (const DemoCity& city : demo_cities_) {
		for (int product = 0; product < demo_products_; ++product) {
			const DemoMarket& market = city.markets[product];
			if (market.surplus > 0.0) {
				cache.tot_surplus[product] += market.surplus;
			}
			if (market.sold > 0.0) {
				cache.tot_sold[product] += market.sold;
			}
			// F3: a IA usa este agregado como "existe demanda corrente no
			// sistema". Com bens finais ainda sem produtor, `demand` (compras
			// realizadas) é zero e só `desired` carrega a necessidade das POPs —
			// publicar `desired` permite a fábrica nascer onde há demanda real.
			if (market.desired > 0.0) {
				cache.tot_demand[product] += market.desired;
			}
		}
	}
	// Packed arrays são COW: depois de mutar o buffer da cache, o dicionário
	// precisa receber a referência de novo (senão o consumidor lê o dia anterior).
	cache.market_totals[k.surplus] = cache.tot_surplus;
	cache.market_totals[k.sold] = cache.tot_sold;
	cache.market_totals[k.demand] = cache.tot_demand;

	cache.cities.resize(demo_cities_.size());
	cache.city_slots.resize(demo_cities_.size());
	for (std::size_t city_index = 0; city_index < demo_cities_.size(); ++city_index) {
		DemoStatusCitySlot& slot = cache.city_slots[city_index];
		demo_fill_city_status(city_index, slot,
			cache.in_transit_by_city[city_index]);
		cache.cities[city_index] = slot.dict;
	}
	OGS_SIM_PHASE_LAP(0);

	// Portos (modo mundo) para o minimapa e o mundo 3D.
	cache.ports.resize(demo_ports_.size());
	cache.port_slots.resize(demo_ports_.size());
	for (std::size_t index = 0; index < demo_ports_.size(); ++index) {
		const Port& port = demo_ports_[index];
		Dictionary& dict = cache.port_slots[index];
		dict[k.id] = String(port.id.c_str());
		dict[k.name] = String(port.name.c_str());
		dict[k.nation] = String(port.nation.c_str());
		dict[k.x] = port.center.x;
		dict[k.y] = port.center.y;
		dict[k.landmass] = String(port.landmass.c_str());
		// Capacidade/fila (port_design.md §2): o porto serve a nação toda;
		// `host_city` é só a sede (âncora e base da capacidade).
		dict[k.host_city] = String(port.host_city.c_str());
		const double base = std::max(1.0, port.base_capacity_t_per_day);
		dict[k.level] = std::max(1, static_cast<int>(std::ceil(
			port.capacity_t_per_day / base)));
		dict[k.capacity_t_per_day] = port.capacity_t_per_day;
		dict[k.throughput] = port.used_today_t;
		dict[k.queue] = port.queue_t;
		dict[k.delay_hours] = port.delay_hours;
		dict[k.utilization] = port.capacity_t_per_day > 0.0
			? port.used_today_t / port.capacity_t_per_day : 0.0;
		cache.ports[index] = dict;
	}

	// Pedidos de comércio do último tick (comboios 3D e HUD).
	cache.orders.resize(demo_last_orders_.size());
	cache.order_slots.resize(demo_last_orders_.size());
	cache.order_leg_slots.resize(demo_last_orders_.size());
	if (cache.order_leg_entry.size() < cache.order_slots.size()) {
		cache.order_leg_entry.resize(cache.order_slots.size());
	}
	for (std::size_t index = 0; index < demo_last_orders_.size(); ++index) {
		const TradeOrder& order = demo_last_orders_[index];
		Dictionary& dict = cache.order_slots[index];
		dict[k.from_city] = String(order.from_city.c_str());
		dict[k.to_city] = String(order.to_city.c_str());
		dict[k.from_nation] = String(order.from_nation.c_str());
		dict[k.to_nation] = String(order.to_nation.c_str());
		dict[k.scope] = String(scope_name(order.scope));
		dict[k.origin_price] = order.origin_price;
		dict[k.origin_price_anchor] = order.origin_price_anchor;
		dict[k.tariff] = order.tariff;
		dict[k.delivered_price] = order.delivered_price;
		dict[k.fx_rate] = order.fx_rate;
		dict[k.seller_fx_rate] = order.seller_fx_rate;
		dict[k.buyer_currency] = String(order.buyer_currency.c_str());
		dict[k.seller_currency] = String(order.seller_currency.c_str());
		dict[k.good] = order.product >= 0 && order.product < demo_products_
			? String(demo_product_name(order.product)) : String("wheat");
		dict[k.amount] = order.amount;
		dict[k.transit_hours] = order.transit_hours;
		dict[k.freight_cost] = order.freight_cost;
		Array& legs = cache.order_leg_slots[index];
		legs.resize(order.legs.size());
		std::vector<Dictionary>& leg_entries = cache.order_leg_entry[index];
		if (leg_entries.size() < order.legs.size()) {
			leg_entries.resize(order.legs.size());
		}
		for (std::size_t leg_index = 0; leg_index < order.legs.size(); ++leg_index) {
			Dictionary& leg_dict = leg_entries[leg_index];
			demo_fill_leg_status(leg_dict, order.legs[leg_index]);
			legs[leg_index] = leg_dict;
		}
		dict[k.legs] = legs;
		cache.orders[index] = dict;
	}
	OGS_SIM_PHASE_LAP(1);

	// Nações agregadas (telas de política/economia), na ordem de aparição.
	// Uma única passada sobre as cidades (antes: nação × cidade × construção).
	struct NationTotals {
		double population = 0.0;
		double production = 0.0;
		double stockpile = 0.0;
		double need = 0.0;
		double deficit = 0.0;
		double in_transit = 0.0;
		int city_count = 0;
		// F7: mercado de trabalho agregado (soma das cidades da nação).
		double working_age = 0.0;
		double labor_force = 0.0;
		double employed = 0.0;
		double unemployed = 0.0;
		double students = 0.0;
		double recruit_reserve = 0.0;
		double education[kDemoEducationLevels] = {};
		double labor_force_by_level[kDemoEducationLevels] = {};
		double employed_by_level[kDemoEducationLevels] = {};
		double unemployed_by_level[kDemoEducationLevels] = {};
		double vacancies_by_level[kDemoEducationLevels] = {};
		double wage_weighted[kDemoEducationLevels] = {};
		double wage_weight[kDemoEducationLevels] = {};
		// F7 calibração: fatia da POP da nação por nível de consumo
		// (0 = nenhum, 1 = basic, 2 = life, 3 = luxury).
		double income_share[kDemoIncomeLevels] = {};
		double income_population = 0.0;
		// F7 calibração: TFP nacional ponderada pela população de simulação.
		double productivity_weighted = 0.0;
	};
	std::vector<std::string> nation_order;
	std::vector<NationTotals> nation_totals;
	std::unordered_map<std::string, int> nation_slot;
	nation_slot.reserve(demo_cities_.size() * 2);
	for (std::size_t city_index = 0; city_index < demo_cities_.size(); ++city_index) {
		const DemoCity& city = demo_cities_[city_index];
		auto found = nation_slot.find(city.nation);
		if (found == nation_slot.end()) {
			found = nation_slot.emplace(city.nation,
				static_cast<int>(nation_order.size())).first;
			nation_order.push_back(city.nation);
			nation_totals.push_back(NationTotals{});
		}
		NationTotals& totals = nation_totals[static_cast<std::size_t>(found->second)];
		++totals.city_count;
		totals.population += city.population;
		totals.stockpile += city.markets[0].stock;
		totals.need += city.pop.need;
		totals.deficit += std::max(0.0, city.pop.need - city.pop.consumed);
		totals.working_age += city.pop.working_age;
		totals.labor_force += city.pop.labor_force;
		totals.employed += city.pop.employed;
		totals.unemployed += city.pop.unemployed;
		totals.students += city.pop.students;
		totals.recruit_reserve += city.pop.recruit_reserve;
		totals.income_population += std::max(0.0, city.simulation_population);
		totals.productivity_weighted += demo_effective_productivity(city)
			* std::max(0.0, city.simulation_population);
		for (int level_index = 0; level_index < kDemoIncomeLevels; ++level_index) {
			totals.income_share[level_index] +=
				city.pop.income_level_share[level_index]
				* std::max(0.0, city.simulation_population);
		}
		for (int level = 0; level < kDemoEducationLevels; ++level) {
			totals.education[level] += city.pop.education[level];
			totals.labor_force_by_level[level] +=
				city.pop.labor_force_by_level[level];
			totals.employed_by_level[level] += city.pop.employed_by_level[level];
			totals.unemployed_by_level[level] +=
				city.pop.unemployed_by_level[level];
			totals.vacancies_by_level[level] +=
				city.pop.vacancies_by_level[level];
			totals.wage_weighted[level] += city.pop.wage_by_level[level]
				* city.pop.labor_force_by_level[level];
			totals.wage_weight[level] += city.pop.labor_force_by_level[level];
		}
		for (const int bi : demo_buildings_by_city_[city_index]) {
			const DemoBuilding& building =
				demo_buildings_[static_cast<std::size_t>(bi)];
			if (building.input_product < 0) {
				totals.production += building.capacity * building.output_scale;
			}
		}
	}
	for (std::size_t index = 0; index < cache.in_transit_by_city.size(); ++index) {
		const double amount = cache.in_transit_by_city[index];
		if (amount == 0.0) {
			continue;
		}
		const auto slot = nation_slot.find(demo_cities_[index].nation);
		if (slot != nation_slot.end()) {
			nation_totals[static_cast<std::size_t>(slot->second)].in_transit += amount;
		}
	}
	static const std::pair<const char*, const char*> kNames[] = {
		{"GBR", "England"}, {"FRA", "France"}, {"DEU", "Germany"},
		{"NED", "Netherlands"}, {"BEL", "Belgium"},
	};
	cache.nations.resize(nation_order.size());
	cache.nation_slots.resize(nation_order.size());
	for (std::size_t index = 0; index < nation_order.size(); ++index) {
		const std::string& tag = nation_order[index];
		const NationTotals& totals = nation_totals[index];
		Dictionary& nation = cache.nation_slots[index];
		nation[k.tag] = String(tag.c_str());
		const char* display = tag.c_str();
		for (const auto& [code, name] : kNames) {
			if (tag == code) {
				display = name;
			}
		}
		nation[k.name] = String(display);
		nation[k.city_count] = totals.city_count;
		nation[k.population] = totals.population;
		nation[k.production] = totals.production;
		nation[k.stockpile] = totals.stockpile;
		nation[k.need] = totals.need;
		nation[k.deficit] = totals.deficit;
		nation[k.in_transit] = totals.in_transit;
		// F4: frota/combustível da nação (a UI e a IA leem daqui; a fila do dia
		// é o sinal de compra de veículos/combustível).
		const auto fleet_found = demo_fleets_.find(tag);
		if (fleet_found != demo_fleets_.end()) {
			const DemoFleet& fleet = fleet_found->second;
			Dictionary fleet_dict;
			fleet_dict[k.trucks] = fleet.trucks;
			fleet_dict[k.ships] = fleet.ships;
			fleet_dict[k.planes] = fleet.planes;
			fleet_dict[k.fuel] = fleet.fuel;
			fleet_dict[k.spare_parts] = fleet.spare_parts;
			fleet_dict[k.health] = Array{fleet.health[0], fleet.health[1],
				fleet.health[2]};
			fleet_dict[k.fuel_used] = fleet.fuel_used_today;
			fleet_dict[k.queue] = fleet.queue_today;
			fleet_dict[k.missed_no_fleet] = fleet.missed_no_fleet;
			fleet_dict[k.missed_no_fuel] = fleet.missed_no_fuel;
			fleet_dict[k.source] = fleet.derived ? "derived" : "declared";
			nation[k.fleet] = fleet_dict;
		}
		// F7: mercado de trabalho da nação (telas de política/trabalho).
		Dictionary labor;
		labor[k.working_age] = totals.working_age;
		labor[k.labor_force] = totals.labor_force;
		labor[k.employed] = totals.employed;
		labor[k.unemployed] = totals.unemployed;
		labor[k.students] = totals.students;
		labor[k.recruit_reserve] = totals.recruit_reserve;
		labor[k.education] = Array{
			totals.education[0], totals.education[1], totals.education[2]};
		labor[k.labor_force_by_level] = Array{
			totals.labor_force_by_level[0], totals.labor_force_by_level[1],
			totals.labor_force_by_level[2]};
		labor[k.employed_by_level] = Array{
			totals.employed_by_level[0], totals.employed_by_level[1],
			totals.employed_by_level[2]};
		labor[k.unemployed_by_level] = Array{
			totals.unemployed_by_level[0], totals.unemployed_by_level[1],
			totals.unemployed_by_level[2]};
		labor[k.vacancies_by_level] = Array{
			totals.vacancies_by_level[0], totals.vacancies_by_level[1],
			totals.vacancies_by_level[2]};
		Array unemployment_by_level;
		Array wage_by_level;
		for (int level = 0; level < kDemoEducationLevels; ++level) {
			unemployment_by_level.append(totals.labor_force_by_level[level] > 1e-9
				? totals.unemployed_by_level[level]
					/ totals.labor_force_by_level[level] : 0.0);
			wage_by_level.append(totals.wage_weight[level] > 1e-9
				? totals.wage_weighted[level] / totals.wage_weight[level]
				: (demo_minimum_wages_.find(tag) != demo_minimum_wages_.end()
					? std::max(0.0, demo_minimum_wages_.at(tag)) : 0.0));
		}
		labor[k.unemployment_by_level] = unemployment_by_level;
		labor[k.wage_by_level] = wage_by_level;
		const auto minimum = demo_minimum_wages_.find(tag);
		labor[k.minimum_wage] = minimum != demo_minimum_wages_.end()
			? std::max(0.0, minimum->second) : 0.0;
		labor[k.wage_rigidity] = demo_wage_rigidity_;
		// F7 calibração: fatia da POP por nível de consumo (a soma é 1).
		Array income_share;
		for (int level_index = 0; level_index < kDemoIncomeLevels; ++level_index) {
			income_share.append(totals.income_population > 1e-9
				? totals.income_share[level_index] / totals.income_population : 0.0);
		}
		labor[k.income_level_share] = income_share;
		// F7 calibração: fator de produtividade nacional (`derived:gdp_ppp`,
		// 1,0 = média mundial), ponderado pela população de simulação.
		labor[k.productivity_index] = totals.income_population > 1e-9
			? totals.productivity_weighted / totals.income_population : 1.0;
		nation[k.labor] = labor;
		cache.nations[index] = nation;
	}

	for (int index = 0; index < demo_products_; ++index) {
		cache.tot_produced[index] = demo_produced_[index];
		cache.tot_consumed[index] = demo_consumed_total_[index];
		cache.tot_produced_today[index] = demo_produced_today_[index];
		cache.tot_consumed_today[index] = demo_consumed_today_[index];
	}
	// F5: moedas e tarifas para a UI da F6. Reescritas em lugar (o Array é
	// referência): só cresce quando o conjunto muda.
	cache.currencies.resize(static_cast<int>(demo_currencies_.size()));
	for (std::size_t index = 0; index < demo_currencies_.size(); ++index) {
		const DemoCurrency& currency = demo_currencies_[index];
		Dictionary entry;
		entry[k.code] = String(currency.code.c_str());
		entry[k.rate] = currency.rate;
		entry[k.regime] = currency.floating ? "float" : "peg";
		entry[k.fallback] = currency.fallback;
		entry[k.trade_balance] = currency.balance_ema;
		entry[k.source] = String(currency.source.c_str());
		cache.currencies[static_cast<int>(index)] = entry;
	}
	cache.tariffs.resize(static_cast<int>(demo_tariffs_.size()));
	for (std::size_t index = 0; index < demo_tariffs_.size(); ++index) {
		const DemoTariff& tariff = demo_tariffs_[index];
		Dictionary entry;
		entry[k.buyer] = String(tariff.buyer.c_str());
		entry[k.seller] = String(tariff.seller.c_str());
		entry[k.product] = String(tariff.product.c_str());
		entry[k.rate] = tariff.rate;
		cache.tariffs[static_cast<int>(index)] = entry;
	}
	OGS_SIM_PHASE_LAP(2);
	demo_status_live_ = slot_index;
	demo_status_built_day_ = day_;
	return root;
}



void EconomySim::demo_price_shock(const String& product, double multiplier) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	int index = demo_product_index(product);
	if (index < 0) {
		return;
	}
	for (DemoCity& city : demo_cities_) {
		const double base = demo_base_price(index);
		city.markets[index].price = std::clamp(
			city.markets[index].price * multiplier, base * 0.1, base * 10.0);
	}
	demo_status_invalidate();
}

int EconomySim::demo_product_index(const String& product) const {
	const auto found = demo_product_lookup_.find(std::string(product.utf8().get_data()));
	if (found == demo_product_lookup_.end()) {
		return -1;
	}
	return found->second;
}

int EconomySim::demo_food_product(int slot) const {
	return (slot >= 0 && slot < static_cast<int>(demo_food_products_.size()))
		? demo_food_products_[static_cast<std::size_t>(slot)] : -1;
}

double EconomySim::demo_food_calories(int product) const {
	for (std::size_t slot = 0; slot < demo_food_products_.size(); ++slot) {
		if (demo_food_products_[slot] == product) {
			return demo_food_factors_[slot];
		}
	}
	return 0.0;
}

const char* EconomySim::demo_product_name(int product) const {
	if (product >= 0 && product < demo_products_
			&& product < static_cast<int>(demo_product_names_.size())) {
		return demo_product_names_[static_cast<std::size_t>(product)].c_str();
	}
	return "";
}

int EconomySim::demo_recipe_index(const String& recipe_id) const {
	const auto found = demo_recipe_lookup_.find(std::string(recipe_id.utf8().get_data()));
	if (found == demo_recipe_lookup_.end()) {
		return -1;
	}
	return found->second;
}

int EconomySim::demo_pick_recipe(const DemoCity& city, int output_product) const {
	int best = -1;
	double best_cost = 0.0;
	for (std::size_t index = 0; index < demo_recipes_.size(); ++index) {
		const DemoRecipe& recipe = demo_recipes_[index];
		if (recipe.output != output_product || recipe.input_count <= 0
				|| recipe.yield <= 0.0) {
			continue;
		}
		bool available = true;
		double cost = 0.0;
		for (int slot = 0; slot < recipe.input_count; ++slot) {
			const int input = recipe.inputs[slot];
			const double qty = recipe.input_qty[slot];
			if (input < 0 || input >= demo_products_ || qty <= 0.0) {
				available = false;
				break;
			}
			const DemoMarket& market = city.markets[input];
			// Insumo só conta como disponível quando há estoque físico: uma
			// receita sem insumo produziria zero e venceria a comparação de custo.
			if (market.stock <= 1e-9) {
				available = false;
				break;
			}
			const double unit_price = market.price > 0.0
				? market.price : demo_base_price(input);
			cost += qty * unit_price / recipe.yield;
		}
		if (!available) {
			continue;
		}
		if (best < 0 || cost < best_cost) {
			best = static_cast<int>(index);
			best_cost = cost;
		}
	}
	return best;
}

void EconomySim::demo_set_recipes(const Array& recipes) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	demo_recipes_.clear();
	demo_recipe_lookup_.clear();
	demo_recipes_.reserve(static_cast<std::size_t>(recipes.size()));
	for (int index = 0; index < recipes.size(); ++index) {
		const Dictionary entry = recipes[index];
		const String output_id = String(entry.get("output", ""));
		const int output = demo_product_index(output_id);
		if (output < 0) {
			continue;  // receita de bem que não existe no goods.json carregado
		}
		DemoRecipe recipe;
		recipe.id = std::string(String(entry.get("id", "")).utf8().get_data());
		recipe.output = output;
		recipe.yield = std::max(1e-9, safe_nonneg(
			static_cast<double>(entry.get("yield", 1.0))));
		recipe.capital_per_capacity = std::max(1.0, safe_nonneg(
			static_cast<double>(entry.get("capital_per_capacity", 2000.0))));
		recipe.jobs_per_ton = safe_nonneg(
			static_cast<double>(entry.get("jobs_per_ton", 7.5)));
		recipe.wage = safe_nonneg(static_cast<double>(entry.get("wage", 2.0)));
		recipe.opex_per_ton = safe_nonneg(
			static_cast<double>(entry.get("opex_per_ton", 2.5)));
		// F7: escolaridade das vagas. `education_mix` explícito vence a tabela
		// por setor (dado do Game Systems Designer); sem ele, o setor decide.
		const std::string sector(
			String(entry.get("sector", "")).utf8().get_data());
		demo_education_mix_for_sector(sector, recipe.education_mix);
		if (entry.has("education_mix")) {
			double mix[3] = {-1.0, -1.0, -1.0};
			const Variant mix_value = entry["education_mix"];
			if (mix_value.get_type() == Variant::ARRAY) {
				const Array values = mix_value;
				for (int level = 0;
						level < kDemoEducationLevels && level < values.size();
						++level) {
					mix[level] = std::max(0.0, double(values[level]));
				}
			} else if (mix_value.get_type() == Variant::DICTIONARY) {
				const Dictionary values = mix_value;
				static const char* kNames[3] = {
					"basic", "intermediate", "higher"};
				for (int level = 0; level < kDemoEducationLevels; ++level) {
					if (values.has(kNames[level])) {
						mix[level] = std::max(0.0,
							double(values[kNames[level]]));
					}
				}
			}
			const double total = std::max(0.0, mix[0]) + std::max(0.0, mix[1])
				+ std::max(0.0, mix[2]);
			if (total > 1e-9) {
				for (int level = 0; level < kDemoEducationLevels; ++level) {
					recipe.education_mix[level] = std::max(0.0, mix[level]) / total;
				}
			}
		}
		Array inputs = entry.get("inputs", Array());
		for (int slot = 0; slot < inputs.size() && slot < kDemoMaxRecipeInputs; ++slot) {
			const Dictionary input = inputs[slot];
			const int product = demo_product_index(String(input.get("good", "")));
			const double qty = safe_nonneg(static_cast<double>(input.get("qty", 1.0)));
			if (product < 0 || qty <= 0.0) {
				continue;
			}
			recipe.inputs[recipe.input_count] = product;
			recipe.input_qty[recipe.input_count] = qty;
			++recipe.input_count;
		}
		if (recipe.input_count <= 0) {
			continue;
		}
		Array byproducts = entry.get("byproducts", Array());
		for (int slot = 0; slot < byproducts.size() && slot < kDemoMaxRecipeInputs; ++slot) {
			const Dictionary item = byproducts[slot];
			const int product = demo_product_index(String(item.get("good", "")));
			const double qty = safe_nonneg(static_cast<double>(item.get("qty", 1.0)));
			if (product < 0 || qty <= 0.0) {
				continue;
			}
			recipe.byproducts[recipe.byproduct_count] = product;
			recipe.byproduct_qty[recipe.byproduct_count] = qty;
			++recipe.byproduct_count;
		}
		demo_recipe_lookup_[recipe.id] = static_cast<int>(demo_recipes_.size());
		demo_recipes_.push_back(recipe);
	}
}

void EconomySim::demo_seed_industry() {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	if (!demo_world_mode_ || demo_industry_seeded_ || demo_recipes_.empty()
			|| demo_cities_.empty()) {
		return;
	}
	for (const DemoBuilding& building : demo_buildings_) {
		if (building.recipe >= 0 || building.input_product >= 0) {
			demo_industry_seeded_ = true;  // o mundo já tem indústria
			return;
		}
	}
	const int flour_product = demo_product_index(String("flour"));
	const int bread_product = demo_product_index(String("bread"));
	int flour_recipe = -1;
	int bread_recipe = -1;
	for (std::size_t index = 0; index < demo_recipes_.size(); ++index) {
		if (demo_recipes_[index].output == flour_product && flour_recipe < 0) {
			flour_recipe = static_cast<int>(index);
		}
		if (demo_recipes_[index].output == bread_product && bread_recipe < 0) {
			bread_recipe = static_cast<int>(index);
		}
	}
	if (flour_recipe < 0) {
		return;  // recipes.json ainda não carregou
	}
	// Maiores cidades (capitais/megacidades) por população de simulação.
	std::vector<int> order(demo_cities_.size());
	for (std::size_t index = 0; index < order.size(); ++index) {
		order[index] = static_cast<int>(index);
	}
	std::stable_sort(order.begin(), order.end(), [this](int left, int right) {
		return demo_cities_[static_cast<std::size_t>(left)].simulation_population
			> demo_cities_[static_cast<std::size_t>(right)].simulation_population;
	});
	const int seed_cities = std::min<int>(6, static_cast<int>(order.size()));
	int created = 0;
	for (int slot = 0; slot < seed_cities; ++slot) {
		DemoCity& city = demo_cities_[static_cast<std::size_t>(order[slot])];
		// Capacidade de referência = 5% da necessidade alimentar diária da
		// cidade, em toneladas de farinha/pão. É uma ESTIMATIVA de transição
		// (§12.3), não um dado observado; calibrada no laboratório em F3.
		const double capacity = std::clamp(
			city.simulation_population * kDemoNeedPerCapita * 0.05, 2.0, 20.0);
		const DemoRecipe& flour = demo_recipes_[static_cast<std::size_t>(flour_recipe)];
		const double cost = capacity * flour.capital_per_capacity;
		if (city.firm_cash + city.government_budget < cost * 2.0) {
			continue;
		}
		const String city_id(city.id.c_str());
		const String mill_id(("SEED-FLOUR-" + city.id).c_str());
		Dictionary mill = demo_build(city_id, mill_id, "flour", -1, flour_product,
			capacity, 0.0, 0.0, 0.0, 1.0, "state", cost, String(flour.id.c_str()));
		if (!bool(mill.get("created", false))) {
			continue;
		}
		if (DemoBuilding* built = find_demo_building(city.id,
				std::string(mill_id.utf8().get_data()))) {
			built->strategic = true;
		}
		++created;
		if (bread_recipe >= 0) {
			const DemoRecipe& bread =
				demo_recipes_[static_cast<std::size_t>(bread_recipe)];
			const double bread_capacity = capacity * 0.8;
			const double bread_cost = bread_capacity * bread.capital_per_capacity;
			const String bakery_id(("SEED-BREAD-" + city.id).c_str());
			Dictionary bakery = demo_build(city_id, bakery_id, "bread", -1,
				bread_product, bread_capacity, 0.0, 0.0, 0.0, 1.0, "state",
				bread_cost, String(bread.id.c_str()));
			if (bool(bakery.get("created", false))) {
				if (DemoBuilding* built = find_demo_building(city.id,
						std::string(bakery_id.utf8().get_data()))) {
					built->strategic = true;
				}
				++created;
			}
		}
	}
	// F3: corrente final representativa (bem de POP) para a demanda final não
	// ficar só no papel. A IA ainda não atravessa a cadeia inteira quando o
	// insumo não existe (gate de `_input_available` + uma construção por
	// visita), então as 3 maiores cidades com aptidão para a matéria-prima
	// recebem uma cadeia de referência cotton→fabric→clothes e wood→cellulose→
	// paper. É ESTIMATIVA de transição (§12.3), marcada `strategic`, não dado
	// observado; as capacidades derivam da necessidade per capita do nível.
	auto recipe_for = [this](const char* output_id) {
		const int product = demo_product_index(String(output_id));
		for (std::size_t index = 0; index < demo_recipes_.size(); ++index) {
			if (demo_recipes_[index].output == product) {
				return static_cast<int>(index);
			}
		}
		return -1;
	};
	auto seed_final_chain = [this, &recipe_for](DemoCity& city, const char* raw_id,
			const char* mid_id, const char* final_id, double per_capita_day,
			double max_capacity) -> int {
		const int raw_product = demo_product_index(String(raw_id));
		const int mid_recipe = recipe_for(mid_id);
		const int final_recipe = recipe_for(final_id);
		if (raw_product < 0 || mid_recipe < 0 || final_recipe < 0) {
			return 0;
		}
		// Sem aptidão para a matéria-prima não há cadeia local (nada inventado).
		if (city.suitability[raw_product] < 0.30) {
			return 0;
		}
		const double final_capacity = std::clamp(
			city.simulation_population * per_capita_day, 2.0, max_capacity);
		for (const auto& existing : demo_buildings_) {
			if (existing.city == city.id
					&& existing.id.find("SEED-" + std::string(final_id)) != std::string::npos) {
				return 0;  // já semeada
			}
		}
		const DemoRecipe& mid = demo_recipes_[static_cast<std::size_t>(mid_recipe)];
		const DemoRecipe& final = demo_recipes_[static_cast<std::size_t>(final_recipe)];
		// Só as fábricas são semeadas: a matéria-prima entra por comércio (o
		// `demo_trade_goods` já trata o insumo da receita) e a IA constrói as
		// fazendas de algodão/madeira quando o shortage aparece. Semear a
		// fazenda aqui tirava mão de obra primária das fazendas de alimento.
		const double total_cost =
			final_capacity * mid.capital_per_capacity
			+ final_capacity * final.capital_per_capacity;
		if (city.government_budget < total_cost) {
			return 0;
		}
		const String city_id(city.id.c_str());
		int built = 0;
		const String mid_building_id(("SEED-" + std::string(mid_id) + "-" + city.id).c_str());
		Dictionary mid_build = demo_build(city_id, mid_building_id, String(mid_id),
			-1, demo_recipes_[static_cast<std::size_t>(mid_recipe)].output,
			final_capacity, 0.0, 0.0, 0.0, 1.0, "state",
			final_capacity * mid.capital_per_capacity,
			String(mid.id.c_str()));
		if (bool(mid_build.get("created", false))) {
			if (DemoBuilding* node = find_demo_building(city.id,
					std::string(mid_building_id.utf8().get_data()))) {
				node->strategic = true;
			}
			++built;
		}
		const String final_building_id(("SEED-" + std::string(final_id) + "-" + city.id).c_str());
		Dictionary final_build = demo_build(city_id, final_building_id, String(final_id),
			-1, demo_recipes_[static_cast<std::size_t>(final_recipe)].output,
			final_capacity, 0.0, 0.0, 0.0, 1.0, "state",
			final_capacity * final.capital_per_capacity,
			String(final.id.c_str()));
		if (bool(final_build.get("created", false))) {
			if (DemoBuilding* node = find_demo_building(city.id,
					std::string(final_building_id.utf8().get_data()))) {
				node->strategic = true;
			}
			++built;
		}
		return built;
	};
	if (demo_needs_loaded_) {
		// Só capital com folga alimentar: a fábrica de referência não pode
		// competir com a comida de uma nação importadora (EGY/SAH). A folga é
		// medida pela capacidade primária de alimento já instalada × aptidão.
		int chains_seeded = 0;
		const int scan = std::min<int>(24, static_cast<int>(order.size()));
		for (int slot = 0; slot < scan && chains_seeded < 3; ++slot) {
			const int city_index = order[static_cast<std::size_t>(slot)];
			DemoCity& city = demo_cities_[static_cast<std::size_t>(city_index)];
			double food_capacity = 0.0;
			for (const int bi : demo_buildings_by_city_[static_cast<std::size_t>(city_index)]) {
				const DemoBuilding& building = demo_buildings_[static_cast<std::size_t>(bi)];
				if (demo_food_calories(building.output_product) > 0.0) {
					food_capacity += building.capacity
						* city.suitability[building.output_product];
				}
			}
			const double food_need = city.simulation_population * kDemoNeedPerCapita;
			if (food_capacity < food_need * 1.10) {
				continue;
			}
			int built = 0;
			// basic/clothing: 0,0004 t/mês por habitante.
			built += seed_final_chain(city, "cotton", "fabric", "clothes",
				0.0004 / 30.0, 20.0);
			// institutional/government paper: 0,005 × âncora alimentar/dia.
			built += seed_final_chain(city, "wood", "cellulose", "paper",
				kDemoNeedPerCapita * 0.005, 10.0);
			if (built > 0) {
				++chains_seeded;
				created += built;
			}
		}
	}
	demo_industry_seeded_ = true;
	if (created > 0) {
		UtilityFunctions::print(String(("economia: bootstrap industrial (estimativa) — "
			+ std::to_string(created) + " plantas de referência semeadas").c_str()));
	}
}

void EconomySim::demo_set_needs(const Dictionary& needs) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	// Cesta alimentar a partir do nível básico de needs.json. Só a categoria
	// `food` tem âncora física no núcleo hoje (kDemoNeedPerCapita); as demais
	// categorias ficam para F3.
	const Dictionary levels = needs.get("levels", Dictionary());
	const Dictionary basic = levels.get("basic", Dictionary());
	const Dictionary categories = basic.get("categories", Dictionary());
	const Dictionary food = categories.get("food", Dictionary());
	const Array substitutes = food.get("substitutes", Array());
	if (substitutes.is_empty()) {
		return;  // mantém a cesta validada (5 culturas)
	}
	const Dictionary equivalence = needs.get("equivalence", Dictionary());
	const Dictionary calories = equivalence.get("calories", Dictionary());
	std::vector<int> products;
	std::vector<double> factors;
	for (int index = 0; index < substitutes.size()
			&& static_cast<int>(products.size()) < kDemoMaxFoodProducts; ++index) {
		const String id = String(substitutes[index]);
		const int product = demo_product_index(id);
		if (product < 0) {
			continue;
		}
		double factor = 1.0;
		if (calories.has(id)) {
			factor = static_cast<double>(calories[id]);
		}
		factor = std::clamp(factor, 0.05, 4.0);
		// Mantém a ordem data-driven, mas garante os 5 grãos validados no
		// início quando estão entre os substitutos (a ordem de consumo importa
		// sob orçamento apertado: os mais baratos/legados primeiro).
		products.push_back(product);
		factors.push_back(factor);
	}
	if (products.size() < 2) {
		return;
	}
	// Ordem de consumo: os 5 grãos/raízes validados primeiro (a ordem importa
	// sob orçamento apertado e no fallback "sem estoque"); os processados
	// (bread/meat/canned_food/bagged_rice) entram depois.
	static const char* const kLegacyFood[] = {
		"wheat", "rice", "maize", "potato", "soy"};
	std::vector<int> ordered_products;
	std::vector<double> ordered_factors;
	for (const char* legacy : kLegacyFood) {
		const int index = demo_product_index(String(legacy));
		for (std::size_t slot = 0; slot < products.size(); ++slot) {
			if (products[slot] == index) {
				ordered_products.push_back(products[slot]);
				ordered_factors.push_back(factors[slot]);
				break;
			}
		}
	}
	for (std::size_t slot = 0; slot < products.size(); ++slot) {
		bool duplicate = false;
		for (const int existing : ordered_products) {
			if (existing == products[slot]) {
				duplicate = true;
				break;
			}
		}
		if (!duplicate) {
			ordered_products.push_back(products[slot]);
			ordered_factors.push_back(factors[slot]);
		}
	}
	demo_food_products_ = std::move(ordered_products);
	demo_food_factors_ = std::move(ordered_factors);

	// F3: níveis completos (basic/life/luxury). A cesta alimentar acima é o
	// contrato validado e continua com o laço próprio no tick; aqui entram os
	// níveis e as categorias não alimentares, com a ordem dos substitutos
	// preservada (do melhor ao último caso).
	demo_needs_levels_.clear();
	demo_needs_food_level_ = -1;
	demo_needs_loaded_ = false;
	demo_government_products_.clear();
	demo_government_per_capita_day_.clear();
	const Dictionary levels_dict = needs.get("levels", Dictionary());
	const Array level_names = levels_dict.keys();
	struct ParsedLevel {
		int priority = 1;
		String name;
		Dictionary data;
	};
	std::vector<ParsedLevel> parsed_levels;
	parsed_levels.reserve(static_cast<std::size_t>(level_names.size()));
	for (int index = 0; index < level_names.size(); ++index) {
		const String name = String(level_names[index]);
		const Dictionary data = levels_dict.get(name, Dictionary());
		if (data.is_empty()) {
			continue;
		}
		ParsedLevel entry;
		entry.priority = int(data.get("priority", index + 1));
		entry.name = name;
		entry.data = data;
		parsed_levels.push_back(std::move(entry));
	}
	std::stable_sort(parsed_levels.begin(), parsed_levels.end(),
		[](const ParsedLevel& left, const ParsedLevel& right) {
			return left.priority < right.priority;
		});
	for (const ParsedLevel& parsed : parsed_levels) {
		if (static_cast<int>(demo_needs_levels_.size()) >= kDemoMaxNeedsLevels) {
			break;
		}
		DemoNeedsLevel level;
		level.id = std::string(parsed.name.utf8().get_data());
		level.priority = parsed.priority;
		level.weight = std::clamp(
			static_cast<double>(parsed.data.get("satisfaction_weight", 0.0)), 0.0, 1.0);
		const int level_index = static_cast<int>(demo_needs_levels_.size());
		const Dictionary categories = parsed.data.get("categories", Dictionary());
		const Array category_names = categories.keys();
		for (int ci = 0; ci < category_names.size(); ++ci) {
			const String category_name = String(category_names[ci]);
			const Dictionary category_data = categories.get(category_name, Dictionary());
			const Array substitutes = category_data.get("substitutes", Array());
			if (substitutes.is_empty()) {
				continue;
			}
			DemoNeedsCategory category;
			category.id = std::string(category_name.utf8().get_data());
			category.per_capita_day = std::max(0.0,
				static_cast<double>(category_data.get("per_capita_t_month", 0.0)))
				/ 30.0;
			// Cada categoria declara a tabela de equivalência que usa
			// (ex.: basic/food → equivalence.calories).
			const String eq_key = String(category_data.get("equivalence_key", ""));
			Dictionary eq_table;
			if (!eq_key.is_empty()) {
				eq_table = equivalence.get(eq_key, Dictionary());
			}
			for (int si = 0; si < substitutes.size(); ++si) {
				const String id = String(substitutes[si]);
				const int product = demo_product_index(id);
				if (product < 0) {
					continue;
				}
				double factor = 1.0;
				if (eq_table.has(id)) {
					factor = static_cast<double>(eq_table[id]);
				}
				factor = std::clamp(factor, 0.05, 8.0);
				category.substitutes.push_back(product);
				category.equivalence.push_back(factor);
			}
			if (category.substitutes.empty()) {
				continue;
			}
			if (category_name == String("food") && demo_needs_food_level_ < 0) {
				demo_needs_food_level_ = level_index;
			}
			level.categories.push_back(std::move(category));
		}
		if (level.categories.empty()) {
			continue;
		}
		demo_needs_levels_.push_back(std::move(level));
	}
	if (demo_needs_levels_.empty()) {
		++demo_status_static_version_;
		demo_status_invalidate();
		return;
	}
	demo_needs_loaded_ = true;
	const double replacement_per_year = std::clamp(
		static_cast<double>(needs.get("durable_replacement_per_year", 0.1)), 0.0, 0.99);
	demo_durable_decay_ = replacement_per_year > 0.0
		? 1.0 - std::pow(1.0 - replacement_per_year, 1.0 / 365.0) : 0.0;
	// Consumo institucional (decisão §12.10): o Estado compra paper/computer.
	// O dado de needs.json lista os BENS (`institutional.government`); a
	// quantidade não existe na spec, então usa-se uma fração ESTIMADA da âncora
	// alimentar (kDemoNeedPerCapita), declarada na tabela abaixo e registrada em
	// GAME_STATE/perf como estimativa — nunca como medição. Se um needs.json
	// futuro trouxer `institutional_per_capita_t_month.government.<bem>`, ele
	// vence a estimativa.
	const Dictionary institutional = needs.get("institutional", Dictionary());
	const Array government = institutional.get("government", Array());
	const Dictionary quantities_root =
		needs.get("institutional_per_capita_t_month", Dictionary());
	const Dictionary quantities = quantities_root.get("government", Dictionary());
	static const struct {
		const char* id;
		double anchor_share;  // fração de kDemoNeedPerCapita por dia
	} kEstimatedGovernmentNeeds[] = {
		{"paper", 0.005},
		{"computer", 0.0001},
	};
	for (int index = 0; index < government.size(); ++index) {
		const String id = String(government[index]);
		const int product = demo_product_index(id);
		if (product < 0) {
			continue;
		}
		double per_day = 0.0;
		if (quantities.has(id)) {
			per_day = std::max(0.0, static_cast<double>(quantities[id])) / 30.0;
		} else {
			for (const auto& estimate : kEstimatedGovernmentNeeds) {
				if (id == String(estimate.id)) {
					per_day = kDemoNeedPerCapita * estimate.anchor_share;
					break;
				}
			}
		}
		if (per_day <= 0.0) {
			continue;
		}
		demo_government_products_.push_back(product);
		demo_government_per_capita_day_.push_back(per_day);
	}
	// Necessidades carregadas depois do mundo: o estado por cidade acompanha.
	for (DemoCity& city : demo_cities_) {
		demo_init_pop_needs(city.pop);
	}
	++demo_status_static_version_;
	demo_status_invalidate();
}

double EconomySim::demo_level_basket_cost(const DemoCity& city,
		int level_index) const {
	if (level_index < 0
			|| level_index >= static_cast<int>(demo_needs_levels_.size())) {
		return 0.0;
	}
	const DemoNeedsLevel& level =
		demo_needs_levels_[static_cast<std::size_t>(level_index)];
	const double population = std::max(0.0, city.simulation_population);
	double cost = 0.0;
	for (const DemoNeedsCategory& category : level.categories) {
		double want = category.per_capita_day * population;
		if (want <= 0.0) {
			continue;
		}
		// Duráveis comprados com poupança: a quantidade declarada é a posse e o
		// fluxo diário de renda precisa cobrir apenas a REPOSIÇÃO anual
		// (`durable_replacement_per_year`), não uma compra por mês. Sem isso o
		// telefone (0,000225 t/mês) custava ~1/3 de toda a renda diária e
		// travava life/luxury em qualquer nação.
		const double replacement = std::clamp(demo_durable_decay_ * 365.0, 0.0, 0.99);
		double cheapest = 0.0;  // preço por tonelada-equivalente
		bool has_durable = false;
		double cheapest_durable = 0.0;
		for (std::size_t slot = 0; slot < category.substitutes.size(); ++slot) {
			const int product = category.substitutes[slot];
			if (product < 0 || product >= demo_products_) {
				continue;
			}
			const double factor = category.equivalence[slot];
			if (factor <= 0.0) {
				continue;
			}
			const double price = city.markets[product].price;
			if (price <= 0.0) {
				continue;
			}
			const double per_ge = price / factor;
			const bool durable =
				product < static_cast<int>(demo_product_durable_.size())
				&& demo_product_durable_[static_cast<std::size_t>(product)] != 0;
			if (durable) {
				if (!has_durable || per_ge < cheapest_durable) {
					has_durable = true;
					cheapest_durable = per_ge;
				}
				continue;
			}
			if (cheapest <= 0.0 || per_ge < cheapest) {
				cheapest = per_ge;
			}
		}
		if (cheapest > 0.0) {
			cost += want * cheapest;
		} else if (has_durable) {
			// Só há substituto durável: cobra a reposição, não a posse inteira.
			cost += want * (replacement / 12.0) * cheapest_durable;
		}
	}
	return cost;
}

void EconomySim::demo_food_basket_cost(const DemoCity& city, double need,
		double& local_cost, double& import_cost, double& per_ge) const {
	local_cost = 0.0;
	import_cost = 0.0;
	per_ge = 0.0;
	const int slots = std::min(
		static_cast<int>(demo_food_products_.size()), kDemoMaxFoodProducts);
	if (slots <= 0) {
		return;
	}
	// Uma passada: guarda a disponibilidade e o preço por tonelada-equivalente
	// de cada slot (evita repetir a busca de fator calórico, que é O(slots)).
	double stock_ge[kDemoMaxFoodProducts];
	double unit_price[kDemoMaxFoodProducts];
	double total_avail = 0.0;
	double cheapest = 0.0;
	double cheapest_import = 0.0;
	for (int slot = 0; slot < slots; ++slot) {
		const int product = demo_food_product(slot);
		const double factor = demo_food_calories(product);
		stock_ge[slot] = 0.0;
		unit_price[slot] = 0.0;
		if (factor <= 0.0) {
			continue;
		}
		stock_ge[slot] = std::max(0.0, city.markets[product].stock) * factor;
		total_avail += stock_ge[slot];
		const double price = city.markets[product].price;
		if (price > 0.0) {
			unit_price[slot] = price / factor;
			if (cheapest <= 0.0 || unit_price[slot] < cheapest) {
				cheapest = unit_price[slot];
			}
		}
		// CIF conhecido: origem efetiva + tarifa (sem o frete, que é serviço
		// doméstico do comprador). É o preço que o importador realmente paga.
		const double cif = city.import_price[product];
		if (cif > 0.0) {
			const double unit = cif / factor;
			if (cheapest_import <= 0.0 || unit < cheapest_import) {
				cheapest_import = unit;
			}
		}
	}
	if (cheapest_import <= 0.0) {
		cheapest_import = cheapest;
	}
	if (cheapest <= 0.0) {
		cheapest = cheapest_import;
	}
	if (cheapest <= 0.0) {
		return;
	}
	if (total_avail <= 0.0 || need <= 0.0) {
		import_cost = std::max(0.0, need) * cheapest_import;
		per_ge = cheapest_import;
		return;
	}
	// Mesma proporção de disponibilidade usada na compra (B3). O custo é o da
	// cesta que a cidade consegue montar hoje; o que faltar para a necessidade
	// é importado ao CIF conhecido.
	double covered = 0.0;
	for (int slot = 0; slot < slots; ++slot) {
		if (stock_ge[slot] <= 0.0 || unit_price[slot] <= 0.0) {
			continue;
		}
		const double want = std::min(need, need * (stock_ge[slot] / total_avail));
		local_cost += want * unit_price[slot];
		covered += want;
	}
	const double residual = std::max(0.0, need - covered);
	import_cost = residual * cheapest_import;
	per_ge = (local_cost + import_cost) / need;
}

bool EconomySim::demo_category_top_market(DemoCity& city,
		const DemoNeedsCategory& category, DemoMarket*& market, double& factor,
		double& durable_scale) {
	// Substituto PREFERIDO da categoria: o primeiro com preço de mercado e
	// equivalência válidas. É nele que o desejo da categoria é registrado como
	// sinal de mercado (a cesta tenta os substitutos nesta ordem).
	for (std::size_t slot = 0; slot < category.substitutes.size(); ++slot) {
		const int product = category.substitutes[slot];
		if (product < 0 || product >= demo_products_) {
			continue;
		}
		const double slot_factor = category.equivalence[slot];
		if (slot_factor <= 0.0 || city.markets[product].price <= 0.0) {
			continue;
		}
		const bool durable =
			product < static_cast<int>(demo_product_durable_.size())
			&& demo_product_durable_[static_cast<std::size_t>(product)] != 0;
		market = &city.markets[product];
		factor = slot_factor;
		durable_scale = durable
			? std::clamp(demo_durable_decay_ * 365.0 / 12.0, 0.0, 1.0) : 1.0;
		return true;
	}
	return false;
}

void EconomySim::demo_register_category_demand(DemoCity& city,
		const DemoNeedsCategory& category, double want_ge) {
	if (want_ge <= 0.0) {
		return;
	}
	DemoMarket* market = nullptr;
	double factor = 0.0;
	double durable_scale = 1.0;
	if (!demo_category_top_market(city, category, market, factor, durable_scale)) {
		return;
	}
	if (durable_scale <= 0.0) {
		return;
	}
	// Desejo declarado do tamanho da NECESSIDADE, independentemente de renda
	// ou estoque: é o sinal que a IA lê (`market.shortage`) para abrir a
	// cadeia do bem final. A compra efetiva fica na demo_consume_category e
	// respeita orçamento e estoque; nada é comprado aqui. Como não há renda
	// por trás, o valor entra também em `latent`: a IA vê a escassez, mas o
	// preço não sobe por desejo sem poder de compra.
	const double desired_tons = want_ge / factor * durable_scale;
	market->desired += desired_tons;
	market->latent += desired_tons;
}

double EconomySim::demo_consume_category(DemoCity& city,
		const DemoNeedsCategory& category, double want_ge, double& budget,
		double& spend) {
	if (want_ge <= 0.0 || category.substitutes.empty()) {
		return 0.0;
	}
	// Demanda DESEJADA da categoria: registrada pelo tamanho do desejo no
	// substituto preferido, não pelo estoque. É o sinal de escassez que a IA
	// lê; a compra abaixo é que respeita caixa e estoque. A parcela que o
	// orçamento do dia não cobre entra em `latent` e não pressiona o preço.
	{
		DemoMarket* signal_market = nullptr;
		double signal_factor = 0.0;
		double signal_scale = 1.0;
		if (demo_category_top_market(city, category, signal_market,
				signal_factor, signal_scale) && signal_scale > 0.0) {
			const double full_tons = want_ge / signal_factor * signal_scale;
			const double backed_tons = std::min(full_tons,
				signal_market->price > 0.0
					? std::max(0.0, budget) / signal_market->price : 0.0);
			signal_market->desired += full_tons;
			signal_market->latent += std::max(0.0, full_tons - backed_tons);
		}
	}
	double remaining = want_ge;
	double consumed_ge = 0.0;
	for (std::size_t slot = 0;
			slot < category.substitutes.size() && remaining > 1e-12; ++slot) {
		const int product = category.substitutes[slot];
		if (product < 0 || product >= demo_products_) {
			continue;
		}
		const double factor = category.equivalence[slot];
		if (factor <= 0.0) {
			continue;
		}
		DemoMarket& market = city.markets[product];
		const double price = market.price;
		if (price <= 0.0) {
			continue;
		}
		// Durável: a posse declarada custa um fluxo de REPOSIÇÃO anual, não uma
		// compra por mês (um telefone por pessoa/mês drenava toda a renda).
		const bool durable =
			product < static_cast<int>(demo_product_durable_.size())
			&& demo_product_durable_[static_cast<std::size_t>(product)] != 0;
		const double durable_scale = durable
			? std::clamp(demo_durable_decay_ * 365.0 / 12.0, 0.0, 1.0) : 1.0;
		double desired_tons = durable_scale > 0.0
			? remaining / factor * durable_scale : 0.0;
		// Compra efetiva: o desejo limitado pelo estoque e pelo orçamento do dia.
		double quantity = std::min(desired_tons, std::max(0.0, market.stock));
		if (quantity * price > budget) {
			quantity = std::max(0.0, budget) / price;
		}
		if (quantity <= 1e-12) {
			continue;
		}
		const double paid = quantity * price;
		market.stock -= quantity;
		market.demand += quantity;
		market.sold += quantity;
		budget -= paid;
		spend += paid;
		city.revenue_by_product[product] += paid;
		city.firm_cash += paid;
		demo_consumed_total_[product] += quantity;
		demo_consumed_today_[product] += quantity;
		const double got_ge = durable_scale > 0.0
			? quantity * factor / durable_scale : 0.0;
		consumed_ge += got_ge;
		remaining = std::max(0.0, remaining - got_ge);
		if (durable) {
			if (product < static_cast<int>(city.pop.durable_stock.size())) {
				city.pop.durable_stock[static_cast<std::size_t>(product)] += quantity;
			}
			city.pop.durable_spend += paid;
		}
	}
	return consumed_ge;
}

void EconomySim::demo_consume_government(DemoCity& city) {
	if (!demo_needs_loaded_ || demo_government_products_.empty()) {
		return;
	}
	// Teto prudencial: a compra institucional não pode consumir mais que uma
	// fração do valor da cesta alimentar diária da cidade. Sem o teto, o
	// computer (preço alto) drenava o orçamento que importa comida em nações
	// sem terra (EGY 1,000 → 0,708 em 365 d).
	double cheapest_food = 0.0;
	for (int slot = 0; slot < static_cast<int>(demo_food_products_.size()); ++slot) {
		const int product = demo_food_product(slot);
		const double factor = demo_food_calories(product);
		if (factor <= 0.0) {
			continue;
		}
		const double per_calorie = city.markets[product].price / factor;
		if (per_calorie > 0.0
				&& (cheapest_food <= 0.0 || per_calorie < cheapest_food)) {
			cheapest_food = per_calorie;
		}
	}
	double budget = std::max(0.0, city.government_budget);
	if (cheapest_food > 0.0) {
		const double food_value = city.simulation_population
			* kDemoNeedPerCapita * cheapest_food;
		budget = std::min(budget,
			food_value * kDemoGovernmentGoodsFoodShare);
	} else {
		budget = std::min(budget, budget * 0.01);
	}
	// Divisão igual entre os bens institucionais: o computer (caro) não pode
	// consumir a cota inteira e deixar o paper sem compra.
	const double per_product_budget = budget / std::max<std::size_t>(
		1, demo_government_products_.size());
	for (std::size_t index = 0; index < demo_government_products_.size(); ++index) {
		const int product = demo_government_products_[index];
		if (product < 0 || product >= demo_products_) {
			continue;
		}
		const double want = demo_government_per_capita_day_[index]
			* city.simulation_population;
		if (want <= 1e-12) {
			continue;
		}
		DemoMarket& market = city.markets[product];
		const double price = market.price;
		if (price <= 0.0) {
			continue;
		}
		double desired_tons = want;
		if (desired_tons * price > per_product_budget) {
			desired_tons = std::max(0.0, per_product_budget) / price;
		}
		if (desired_tons <= 1e-12) {
			continue;
		}
		market.desired += desired_tons;
		const double quantity = std::min(desired_tons,
			std::max(0.0, market.stock));
		if (quantity <= 0.0) {
			continue;
		}
		const double paid = quantity * price;
		market.stock -= quantity;
		market.demand += quantity;
		market.sold += quantity;
		city.government_budget -= paid;
		city.government_goods_spend += paid;
		city.firm_cash += paid;
		city.revenue_by_product[product] += paid;
		demo_consumed_total_[product] += quantity;
		demo_consumed_today_[product] += quantity;
	}
}

void EconomySim::demo_init_pop_needs(DemoPop& pop) const {
	if (pop.level_want.size() != kDemoMaxNeedsLevels) {
		pop.level_want.assign(kDemoMaxNeedsLevels, 0.0);
	}
	if (pop.level_consumed.size() != kDemoMaxNeedsLevels) {
		pop.level_consumed.assign(kDemoMaxNeedsLevels, 0.0);
	}
	if (pop.level_satisfaction.size() != kDemoMaxNeedsLevels) {
		pop.level_satisfaction.assign(kDemoMaxNeedsLevels, 0.0);
	}
	// F7: gasto por nível (custo unitário da cesta para o teto de renda).
	if (pop.level_spend.size() != kDemoMaxNeedsLevels) {
		pop.level_spend.assign(kDemoMaxNeedsLevels, 0.0);
	}
	// `resize` preserva o prefixo: um bem novo não zera o estoque durável.
	if (pop.durable_stock.size() != static_cast<std::size_t>(std::max(0, demo_products_))) {
		pop.durable_stock.resize(static_cast<std::size_t>(std::max(0, demo_products_)), 0.0);
	}
}

EconomySim::DemoBuilding* EconomySim::find_demo_building(const std::string& city_id,
		const std::string& id) {
	for (DemoBuilding& building : demo_buildings_) {
		if (building.city == city_id && building.id == id) {
			return &building;
		}
	}
	return nullptr;
}

void EconomySim::demo_set_balance(const Dictionary& values) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	auto number = [&values](const char* key, double fallback) {
		return values.has(key) ? double(values[key]) : fallback;
	};
	demo_natural_unemployment_ = std::clamp(
		number("natural_unemployment", demo_natural_unemployment_), 0.0, 0.5);
	demo_service_wage_adjust_ = std::clamp(
		number("service_wage_adjust", demo_service_wage_adjust_), 0.0, 1.0);
	demo_service_wage_max_ = std::clamp(
		number("service_wage_max", demo_service_wage_max_), 0.01, 10.0);
	demo_inventory_cover_days_ = std::clamp(
		number("inventory_cover_days", demo_inventory_cover_days_), 1.0, 30.0);
	demo_inventory_recovery_ = std::clamp(
		number("inventory_recovery", demo_inventory_recovery_), 0.05, 1.0);
	// Busca do comércio de alimento (K vizinhos + M ofertas externas).
	demo_trade_neighbors_ = std::clamp(
		static_cast<int>(number("trade_neighbors", demo_trade_neighbors_)), 1, 256);
	demo_trade_external_ = std::clamp(
		static_cast<int>(number("trade_external", demo_trade_external_)), 0, 256);
	// F7 calibração: folga de custo de vida do gate de renda → nível de
	// consumo. Padrão = kDemoIncomeBasketMargin; o laboratório pode isolá-la
	// (1.0 = sem folga) para testar a regra do nível sem depender da calibração.
	demo_income_basket_margin_ = std::clamp(
		number("income_basket_margin", demo_income_basket_margin_), 1.0, 8.0);
}

void EconomySim::demo_set_suitability(const String& city_id, const String& product,
		double value) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	const int index = demo_product_index(product);
	if (index < 0) {
		return;
	}
	DemoCity* city = find_demo_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr) {
		return;
	}
	city->suitability[index] = std::clamp(value, 0.0, 1.0);
	// O status reuse arrays de aptidão: invalida só quando o dado muda.
	++demo_status_static_version_;
	demo_status_invalidate();
}

bool EconomySim::demo_set_deposit(const String& city_id, const String& product,
		double reserve_kt, double quality, double depth_m) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	const int index = demo_product_index(product);
	if (index < 0) {
		return false;
	}
	DemoCity* city = find_demo_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr) {
		return false;
	}
	DemoDeposit& deposit = city->deposits[index];
	deposit.quality = std::clamp(safe_nonneg(quality), 0.0, 1.0);
	deposit.depth_m = safe_nonneg(depth_m);
	if (deposit.tracked) {
		// Idempotente: não reabastece uma jazida já em exploração (load/save).
		return false;
	}
	deposit.tracked = true;
	deposit.initial_kt = safe_nonneg(reserve_kt);
	deposit.remaining_kt = deposit.initial_kt;
	deposit.extracted_kt = 0.0;
	++demo_status_deposit_version_;
	demo_status_invalidate();
	return true;
}

void EconomySim::demo_set_external_demand(const String& city_id, const String& product,
		double ceiling_t_per_day) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	const int index = demo_product_index(product);
	if (index < 0) {
		return;
	}
	DemoCity* city = find_demo_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr) {
		return;
	}
	city->external_demand[index] = safe_nonneg(ceiling_t_per_day);
	++demo_status_static_version_;
	demo_status_invalidate();
}

void EconomySim::demo_set_external_income_index(const String& city_id, double index) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	DemoCity* city = find_demo_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr) {
		return;
	}
	// [0, 8]: 0 desliga a renda (calibração/teste), 8 cobre com folga a maior
	// razão PIB PPP per capita do cenário (Singapura ≈ 6,3× a média mundial) e
	// corta dados corrompidos sem virar um teto econômico silencioso.
	city->external_income_index = std::clamp(index, 0.0, 8.0);
	// F7: a estrutura etária, a escolaridade e a participação derivam do mesmo
	// índice de renda real (único dado por país do cenário). Reaplica o perfil
	// quando o índice chega do GeographyService, logo após criar o mundo.
	demo_apply_labor_profile(*city);
	++demo_status_static_version_;
	demo_status_invalidate();
}

void EconomySim::demo_set_productivity_index(const String& city_id, double index) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	DemoCity* city = find_demo_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr) {
		return;
	}
	// 0 (ou negativo) = DERIVAR do índice de renda real: o dado do PIB PPP per
	// capita chega por demo_set_external_income_index e a TFP acompanha. Um
	// valor positivo é fator explícito do laboratório. Os limites só contêm
	// dado corrompido: o cenário real fica em ~0,38 (NGA) a ~3,57 (USA).
	city->productivity_index = index > 0.0
		? std::clamp(index, 0.05, 8.0) : 0.0;
	++demo_status_static_version_;
	demo_status_invalidate();
}

double EconomySim::demo_effective_productivity(const DemoCity& city) const {
	// F7 calibração — TFP nacional. O cenário não traz valor adicionado por
	// trabalhador; o índice de renda real (PIB PPP per capita / média mundial,
	// `derived:gdp_ppp`) é a única medida de nível de produtividade já carregada
	// e entra como FATOR COMPLETO, normalizado para a média mundial = 1,0:
	// EUA ~3,5, DEU ~2,9, BRA ~0,9, IND ~0,46, NGA ~0,37. Aplicar só um prêmio
	// `max(1, índice)` deixava a Nigéria produzindo como a média mundial e
	// invertia a distribuição (menos gente no basic do que a renda real
	// permite). O limite [0,25, 4,0] contém os extremos reais (QAT ~5,0) sem
	// inverter a ordem entre as nações jogáveis; o piso de subsistência da
	// âncora salarial (ver demo_update_wages) impede que o fator < 1 derrube o
	// trabalhador abaixo da cesta alimentar.
	if (city.productivity_index > 0.0) {
		return std::clamp(city.productivity_index, 0.25, 4.0);
	}
	return std::clamp(city.external_income_index, 0.25, 4.0);
}

void EconomySim::demo_set_income_gini(const String& city_id, double gini) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	DemoCity* city = find_demo_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr) {
		return;
	}
	// [0, 1]: 0 desliga (legado por escolaridade); dados corrompidos não viram
	// sigma negativo. A faixa real estimada por região fica em 0,33–0,47.
	city->income_gini = std::clamp(gini, 0.0, 1.0);
	// Recalcula as classes na hora: o status lido antes do primeiro tick já
	// publica a repartição ancorada (se o mundo ainda não tem preços, cai no
	// espelho dos estratos, sem inventar Gini).
	demo_refresh_income_classes(*city);
	++demo_status_static_version_;
	demo_status_invalidate();
}

// ---------------------------------------------------------------------------
// F7 — trabalho, educação e renda.
// ---------------------------------------------------------------------------

namespace {

// Perfil derivado do índice de renda externa (PIB PPP per capita normalizado
// para a média mundial = 1,0). O cenário NÃO traz estrutura etária nem
// escolaridade por nação: estes são parâmetros declarados, na ordem de
// grandeza de ILO/World Bank (participação e idade ativa) e Barro-Lee/UNDP
// (escolaridade). São calibráveis no laboratório; não são dados observados.
constexpr double kLaborIncomeSaturation = 3.0;
constexpr double kLaborWorkingAgeLow = 0.57;
constexpr double kLaborWorkingAgeHigh = 0.66;
constexpr double kLaborRetireeLow = 0.04;
constexpr double kLaborRetireeHigh = 0.17;
constexpr double kLaborBasicEduLow = 0.80;
constexpr double kLaborBasicEduHigh = 0.25;
constexpr double kLaborHigherEduLow = 0.03;
constexpr double kLaborHigherEduHigh = 0.30;
constexpr double kLaborParticipationLow = 0.74;
constexpr double kLaborParticipationHigh = 0.65;
constexpr double kLaborParticipationByLevel[3] = {0.70, 0.80, 0.88};
constexpr double kLaborStudentShareLow = 0.02;
constexpr double kLaborStudentShareHigh = 0.11;
constexpr double kLaborRecruitShare = 0.55;
// F7 calibração — SALÁRIO COMO PARCELA DO VALOR ADICIONADO.
//
// A âncora absoluta antiga (0,030/0,044/0,060 XGS/dia) foi herdada da escala do
// setor de serviços e ficou ~60× abaixo da receita por trabalhador da produção
// primária (20 t/dia ÷ 7,5 empregos/t × 40/t ≈ 5,3 por trabalhador/dia). O
// resultado era um mercado de trabalho dual: a folha pagava uma fração mínima
// do valor adicionado e o resto virava lucro distribuído igualmente a toda a
// POP — 832/834 cidades no nível de consumo máximo.
//
// Agora o NÍVEL do salário é a parcela do trabalho no valor adicionado do dia
// (`city.gdp` = valor adicionado das construções + receita de serviços), entre
// 50% e 65% conforme a renda real do país, e a folha é normalizada por essas
// razões: Σ(vagas preenchidas × salário) = participação × VA. A riqueza que
// sobra é LUCRO e vai para os DONOS (estrato superior), não para toda a POP.
//
// Só as razões entre níveis são fixas (básico 1,00 / intermediário 1,15 /
// superior 1,30) e pequenas de propósito: o PRÊMIO por educação emerge do aperto
// de cada nível (vagas sem candidato × desempregados), como na F7.
constexpr double kLaborWageRatio[3] = {1.00, 1.15, 1.30};
// Salário inicial de criação do mundo (XGS/dia por vaga): um palpite de ordem
// de grandeza do valor adicionado por vaga pago ao trabalho — NÃO é o salário
// de equilíbrio. O primeiro dia com valor adicionado conhecido ancora o salário
// na parcela do trabalho (demo_update_wages) sem inércia.
constexpr double kLaborWageInitial = 0.055;
// Mistura de escolaridade das vagas de serviços urbanos (varejo, reparos,
// transporte local): maioria básico, minoria com superior.
constexpr double kLaborServiceMix[3] = {0.62, 0.30, 0.08};
// Qualificação: duração média da formação (3 e 4 anos), taxa anual de matrícula
// em função do prêmio salarial, fração jovem do nível de origem e teto de
// retirada do desemprego do nível (a escola não pode esvaziar a produção).
constexpr double kLaborStudyDurationDays[2] = {1095.0, 1460.0};
constexpr double kLaborStudyPremiumThreshold = 1.15;
constexpr double kLaborStudyElasticity = 0.12;
constexpr double kLaborStudyMaxAnnual = 0.08;
constexpr double kLaborStudyYouthShare = 0.10;
constexpr double kLaborStudyJoblessShare = 0.25;
constexpr double kLaborStudyMaxDailyShare = 0.002;
// Ajuste salarial gradual: sensibilidade ao aperto e à margem das firmas.
constexpr double kLaborWageTightnessGain = 1.20;
constexpr double kLaborWageMarginGain = 0.60;
constexpr double kLaborIncomeElasticity = 0.30;

// ---------------------------------------------------------------------------
// F7 calibração — distribuição de renda por Gini.
//
// A repartição da POP em classes usa uma lognormal: para um Gini G, o sigma é
// `sqrt(2) * Φ⁻¹((1+G)/2)` e a média é ancorada no PIB PPP per capita. As
// funções abaixo são as aproximações padrão (Acklam para Φ⁻¹ e erf para Φ);
// nenhum número de nação é inventado aqui — Gini e índice vêm dos dados.
// ---------------------------------------------------------------------------
double normal_cdf(double z) {
	return 0.5 * (1.0 + std::erf(z / std::sqrt(2.0)));
}

// Φ⁻¹(x) — Peter Acklam, erro absoluto < 1,15e-9 (domínio (0,1)).
double normal_quantile(double p) {
	if (p <= 1e-12) {
		return -8.0;
	}
	if (p >= 1.0 - 1e-12) {
		return 8.0;
	}
	static const double a[6] = {
		-3.969683028665376e+01, 2.209460984245205e+02,
		-2.759285104469687e+02, 1.383577518672690e+02,
		-3.066479806614716e+01, 2.506628277459239e+00};
	static const double b[5] = {
		-5.447609879822406e+01, 1.615858368580409e+02,
		-1.556989798598866e+02, 6.680131188771972e+01,
		-1.328068155288572e+01};
	static const double c[6] = {
		-7.784894002430293e-03, -3.223964580411365e-01,
		-2.400758277161838e+00, -2.549732539343734e+00,
		4.374664141464968e+00, 2.938163982698783e+00};
	static const double d[4] = {
		7.784695709041462e-03, 3.224671290700398e-01,
		2.445134137142996e+00, 3.754408661907416e+00};
	const double low = 0.02425;
	const double high = 1.0 - low;
	if (p < low) {
		const double q = std::sqrt(-2.0 * std::log(p));
		return (((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q
			+ c[5]) / ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
	}
	if (p > high) {
		const double q = std::sqrt(-2.0 * std::log(1.0 - p));
		return -(((((c[0] * q + c[1]) * q + c[2]) * q + c[3]) * q + c[4]) * q
			+ c[5]) / ((((d[0] * q + d[1]) * q + d[2]) * q + d[3]) * q + 1.0);
	}
	const double q = p - 0.5;
	const double r = q * q;
	return (((((a[0] * r + a[1]) * r + a[2]) * r + a[3]) * r + a[4]) * r
		+ a[5]) * q / (((((b[0] * r + b[1]) * r + b[2]) * r + b[3]) * r
		+ b[4]) * r + 1.0);
}

double labor_income_factor(double income_index) {
	// Riqueza do país: PIB per capita elevado paga salários maiores na âncora.
	// Expoente 0,30 (renda não é repassada 1:1, pois o nível de preços do modelo
	// é comum) entre 0,55 e 1,9 — Nigéria ~0,66; EUA ~1,46.
	const double income = std::clamp(income_index, 0.0, 8.0);
	return std::clamp(std::pow(std::max(income, 1e-6), kLaborIncomeElasticity),
		0.55, 1.9);
}

}  // namespace

void EconomySim::demo_refresh_strata(DemoPop& pop) const {
	// Estrato = escolaridade/ocupação (básico, intermediário, superior). A
	// população do estrato inclui os dependentes: a escolaridade é medida na
	// idade ativa, mas a renda do estrato sustenta toda a sua população.
	double education_total = 0.0;
	for (int level = 0; level < kDemoEducationLevels; ++level) {
		education_total += std::max(0.0, pop.education[level]);
	}
	for (int level = 0; level < kDemoEducationLevels; ++level) {
		pop.stratum_population[level] = education_total > 1e-9
			? pop.population * std::max(0.0, pop.education[level]) / education_total
			: pop.population / double(kDemoEducationLevels);
	}
}

void EconomySim::demo_refresh_income_classes(DemoCity& city) const {
	DemoPop& pop = city.pop;
	const double population = std::max(0.0, city.simulation_population);
	const double gini = std::clamp(city.income_gini, 0.0, 1.0);
	double basic_cost = 0.0;
	if (population > 1e-9) {
		basic_cost = demo_level_basket_cost(city, 0) / population;
	}
	// Sem Gini declarado (cidade sintética/save antigo) ou sem preço de cesta
	// (mundo sem needs.json): espelha os estratos de escolaridade. É o
	// comportamento anterior, preservado fora do cenário real.
	if (gini <= 0.0 || basic_cost <= 1e-12) {
		for (int level = 0; level < kDemoEducationLevels; ++level) {
			pop.class_population[level] = std::max(0.0, pop.stratum_population[level]);
			pop.class_income_level[level] = pop.stratum_income_level[level];
		}
		return;
	}
	// Log-normal: sigma do Gini (2Φ(σ/√2)-1 = G) e média ancorada no PIB PPP
	// per capita real, em cestas básicas (média mundial = kIncomeAnchorWorldMean).
	double sigma = std::sqrt(2.0) * normal_quantile(0.5 * (1.0 + gini));
	sigma = std::clamp(sigma, 0.15, 1.50);
	const double index = std::clamp(city.external_income_index, 0.01, 8.0);
	const double mean_units = kIncomeAnchorWorldMean * index;
	const double mu = std::log(mean_units) - 0.5 * sigma * sigma;
	// Limiares de classe em cestas básicas: pobreza = 1×; luxo = 7×.
	const double basic_share = std::clamp(
		normal_cdf((0.0 - mu) / sigma), 0.0, 1.0);
	const double luxury_share = std::clamp(
		1.0 - normal_cdf((std::log(kIncomeLuxuryMultiple) - mu) / sigma),
		0.0, 1.0);
	const double life_share = std::max(
		0.0, 1.0 - basic_share - luxury_share);
	pop.class_population[0] = population * basic_share;
	pop.class_population[1] = population * life_share;
	pop.class_population[2] = population * luxury_share;
	// basic compra só o nível 0; life, 0-1; luxury, todos (caps em 1-based).
	pop.class_income_level[0] = 1;
	pop.class_income_level[1] = 2;
	pop.class_income_level[2] = kDemoMaxNeedsLevels;
}

void EconomySim::demo_education_mix_for_sector(const std::string& sector,
		double* mix) {	// Escolaridade das vagas por setor (recipes.json `sector`). Semicondutores/
	// eletrônicos e máquinas pedem mais superior; alimentos, têxteis, madeira e
	// embalagem são intensivos em básico. Tabela de engenharia do modelo.
	struct Row {
		const char* sector;
		double mix[3];
	};
	static const Row kRows[] = {
		{"electronics", {0.22, 0.40, 0.38}},
		{"machinery", {0.30, 0.45, 0.25}},
		{"chemical", {0.40, 0.42, 0.18}},
		{"military", {0.35, 0.45, 0.20}},
		{"luxury", {0.45, 0.38, 0.17}},
		{"metal", {0.50, 0.38, 0.12}},
		{"transport", {0.55, 0.35, 0.10}},
		{"construction", {0.65, 0.30, 0.05}},
		{"food", {0.70, 0.25, 0.05}},
		{"packaging", {0.70, 0.25, 0.05}},
		{"textile", {0.72, 0.24, 0.04}},
		{"wood", {0.72, 0.24, 0.04}},
		{"agro", {0.88, 0.10, 0.02}},
	};
	for (const Row& row : kRows) {
		if (sector == row.sector) {
			for (int level = 0; level < kDemoEducationLevels; ++level) {
				mix[level] = row.mix[level];
			}
			return;
		}
	}
	const double fallback[3] = {0.60, 0.30, 0.10};
	for (int level = 0; level < kDemoEducationLevels; ++level) {
		mix[level] = fallback[level];
	}
}

void EconomySim::demo_apply_labor_profile(DemoCity& city) {
	DemoPop& pop = city.pop;
	const double population = std::max(0.0, city.simulation_population);
	pop.population = population;
	const double t = std::clamp(
		std::clamp(city.external_income_index, 0.0, 8.0) / kLaborIncomeSaturation,
		0.0, 1.0);
	pop.working_age = population * (kLaborWorkingAgeLow
		+ (kLaborWorkingAgeHigh - kLaborWorkingAgeLow) * t);
	pop.retirees = population * (kLaborRetireeLow
		+ (kLaborRetireeHigh - kLaborRetireeLow) * t);
	pop.recruit_reserve = pop.working_age * kLaborRecruitShare;
	const double basic = kLaborBasicEduLow
		+ (kLaborBasicEduHigh - kLaborBasicEduLow) * t;
	double higher = kLaborHigherEduLow
		+ (kLaborHigherEduHigh - kLaborHigherEduLow) * t;
	higher = std::min(higher, std::max(0.0, 1.0 - basic - 0.02));
	const double intermediate = std::max(0.0, 1.0 - basic - higher);
	pop.education[0] = pop.working_age * basic;
	pop.education[1] = pop.working_age * intermediate;
	pop.education[2] = pop.working_age * higher;
	const double country_participation = kLaborParticipationLow
		+ (kLaborParticipationHigh - kLaborParticipationLow) * t;
	const double scale = country_participation / kLaborParticipationByLevel[0];
	for (int level = 0; level < kDemoEducationLevels; ++level) {
		pop.participation[level] = std::clamp(
			kLaborParticipationByLevel[level] * scale, 0.35, 0.92);
	}
	const double student_share = kLaborStudentShareLow
		+ (kLaborStudentShareHigh - kLaborStudentShareLow) * t;
	const double students = pop.working_age * student_share;
	pop.studying[0] = students * 0.60;
	pop.studying[1] = students * 0.40;
	pop.students = pop.studying[0] + pop.studying[1];
	const double factor = labor_income_factor(city.external_income_index);
	const auto minimum = demo_minimum_wages_.find(city.nation);
	const double floor = minimum != demo_minimum_wages_.end()
		? std::max(0.0, minimum->second) : 0.0;
	pop.labor_force = 0.0;
	for (int level = 0; level < kDemoEducationLevels; ++level) {
		const double studying = level < kDemoEducationLevels - 1
			? pop.studying[level] : 0.0;
		pop.labor_force_by_level[level] = std::max(0.0,
			pop.education[level] * pop.participation[level] - studying);
		pop.employed_by_level[level] = 0.0;
		pop.unemployed_by_level[level] = pop.labor_force_by_level[level];
		pop.vacancies_by_level[level] = 0.0;
		pop.unfilled_by_level[level] = 0.0;
		pop.wage_income_by_level[level] = 0.0;
		pop.wage_by_level[level] = std::max(floor,
			kLaborWageInitial * kLaborWageRatio[level] * factor);
		pop.labor_force += pop.labor_force_by_level[level];
	}
	pop.wage_anchored = false;
	pop.employed = 0.0;
	pop.unemployed = pop.labor_force;
	pop.dependents = std::max(0.0, population - pop.labor_force);
	pop.wage_income = 0.0;
	// A renda básica é sempre permitida; life/luxury dependem da renda real
	// (income_level calculado no fim da fase de renda).
	pop.income_level = 1;
	if (pop.level_spend.size() != demo_needs_levels_.size()) {
		pop.level_spend.assign(demo_needs_levels_.size(), 0.0);
	}
	demo_refresh_strata(pop);
	demo_refresh_income_classes(city);
}

void EconomySim::demo_set_labor_profile(const String& city_id,
		const Dictionary& profile) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	DemoCity* city = find_demo_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr) {
		return;
	}
	demo_apply_labor_profile(*city);
	DemoPop& pop = city->pop;
	const double population = std::max(0.0, city->simulation_population);
	if (profile.has("working_age_share")) {
		const double share = std::clamp(
			double(profile["working_age_share"]), 0.0, 1.0);
		pop.working_age = population * share;
		pop.recruit_reserve = pop.working_age * kLaborRecruitShare;
	}
	if (profile.has("retiree_share")) {
		pop.retirees = population * std::clamp(
			double(profile["retiree_share"]), 0.0, 1.0);
	}
	if (profile.has("education")) {
		const Variant value = profile["education"];
		if (value.get_type() == Variant::ARRAY) {
			const Array shares = value;
			if (shares.size() >= kDemoEducationLevels) {
				double basic = std::max(0.0, double(shares[0]));
				double intermediate = std::max(0.0, double(shares[1]));
				double higher = std::max(0.0, double(shares[2]));
				const double sum = basic + intermediate + higher;
				if (sum > 0.0) {
					basic /= sum;
					intermediate /= sum;
					higher /= sum;
					pop.education[0] = pop.working_age * basic;
					pop.education[1] = pop.working_age * intermediate;
					pop.education[2] = pop.working_age * higher;
					// Perfil explícito substitui a estrutura etária: as coortes
					// em formação do perfil derivado da renda não podem vazar
					// para a nova distribuição (senão "todo mundo básico" ainda
					// formava superiores todos os dias).
					pop.studying[0] = 0.0;
					pop.studying[1] = 0.0;
				}
			}
		}
	}
	if (profile.has("participation")) {
		const Variant value = profile["participation"];
		if (value.get_type() == Variant::ARRAY) {
			const Array rates = value;
			for (int level = 0;
					level < kDemoEducationLevels && level < rates.size(); ++level) {
				pop.participation[level] = std::clamp(
					double(rates[level]), 0.0, 1.0);
			}
		} else {
			const double rate = std::clamp(double(value), 0.0, 1.0);
			for (int level = 0; level < kDemoEducationLevels; ++level) {
				pop.participation[level] = rate;
			}
		}
	}
	if (profile.has("wage_by_level")) {
		const Variant value = profile["wage_by_level"];
		if (value.get_type() == Variant::ARRAY) {
			const Array wages = value;
			for (int level = 0;
					level < kDemoEducationLevels && level < wages.size(); ++level) {
				pop.wage_by_level[level] = std::max(0.0, double(wages[level]));
			}
		}
	}
	pop.labor_force = 0.0;
	for (int level = 0; level < kDemoEducationLevels; ++level) {
		const double studying = level < kDemoEducationLevels - 1
			? pop.studying[level] : 0.0;
		pop.labor_force_by_level[level] = std::max(0.0,
			pop.education[level] * pop.participation[level] - studying);
		pop.unemployed_by_level[level] = pop.labor_force_by_level[level];
		pop.employed_by_level[level] = 0.0;
		pop.unfilled_by_level[level] = 0.0;
		pop.labor_force += pop.labor_force_by_level[level];
	}
	pop.unemployed = pop.labor_force;
	pop.employed = 0.0;
	pop.students = pop.studying[0] + pop.studying[1];
	pop.dependents = std::max(0.0, population - pop.labor_force);
	demo_refresh_strata(pop);
	demo_status_invalidate();
}

void EconomySim::demo_set_minimum_wage(const String& nation, double value) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	const std::string tag(nation.utf8().get_data());
	if (tag.empty()) {
		return;
	}
	if (value <= 0.0) {
		demo_minimum_wages_.erase(tag);
	} else {
		demo_minimum_wages_[tag] = std::min(value, 100.0);
	}
	// O piso vale a partir do próximo ajuste salarial; cidades sem histórico de
	// salário recebem o âncora com o piso imediatamente.
	const double floor = value > 0.0 ? std::min(value, 100.0) : 0.0;
	if (floor > 0.0) {
		for (DemoCity& city : demo_cities_) {
			if (city.nation != tag) {
				continue;
			}
			for (int level = 0; level < kDemoEducationLevels; ++level) {
				city.pop.wage_by_level[level] = std::max(
					city.pop.wage_by_level[level], floor);
			}
		}
	}
	demo_status_invalidate();
}

void EconomySim::demo_set_wage_rigidity(double value) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	demo_wage_rigidity_ = std::clamp(value, 0.0, 0.25);
	demo_status_invalidate();
}

// ---------------------------------------------------------------------------
// F9 — EMPREGO SETORIAL (docs/economy/sector_employment_report.md)
//
// O mercado de trabalho da F7 continua fechando emprego/desemprego como antes
// (o setor de serviços é o empregador residual). A F9 decompõe esse emprego em
// setores com estrutura ancorada em dados observados:
//   0 agropecuária — da produção local de alimentos (t/dia) e da TFP;
//   1 indústria/mineração/construção — da âncora ILO por renda e do valor
//     adicionado simulado (quando ele passa da âncora);
//   2 serviços — residual (formal + informal de serviços);
//   3 informal — SOBREPOSIÇÃO (fração do emprego total), não soma com 0..2.
// Não cria nem destrói dinheiro: a renda domiciliar continua agregada por
// cidade. O único fluxo novo é o autoconsumo agrícola em espécie (ver o piso de
// subsistência no consumo), que vem da produção local e é conservado no estoque.
// ---------------------------------------------------------------------------

namespace {
// Espelho compilado de data/definitions/economy/sector_employment.json. A fonte
// de verdade é o JSON (carregado por demo_set_sector_employment); este espelho
// existe para testes/benchmark rodarem sem o arquivo.
constexpr int kDemoSectorAnchorCount = 5;
struct DemoSectorAnchorDefault {
	double income;
	double agriculture;
	double industry;
	double informal;
};
constexpr DemoSectorAnchorDefault kDemoSectorAnchorDefaults[kDemoSectorAnchorCount] = {
	{0.371, 0.35, 0.12, 0.92},   // NGA
	{0.457, 0.43, 0.25, 0.90},   // IND
	{0.912, 0.09, 0.20, 0.37},   // BRA
	{2.935, 0.013, 0.27, 0.08},  // DEU
	{3.503, 0.015, 0.19, 0.06},  // USA
};
}  // namespace

void EconomySim::demo_set_sector_employment(const Variant& data) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	if (data.get_type() != Variant::DICTIONARY) {
		return;
	}
	const Dictionary root = data;
	const Array anchors = root.get("anchors", Array());
	std::vector<DemoSectorAnchor> parsed;
	parsed.reserve(anchors.size());
	for (int index = 0; index < anchors.size(); ++index) {
		if (anchors[index].get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary entry = anchors[index];
		DemoSectorAnchor anchor;
		anchor.income = std::clamp(
			static_cast<double>(entry.get("income_index", 1.0)), 0.05, 8.0);
		anchor.agriculture = std::clamp(
			static_cast<double>(entry.get("agriculture", 0.0)), 0.0, 0.85);
		anchor.industry = std::clamp(
			static_cast<double>(entry.get("industry", 0.0)), 0.0, 0.70);
		anchor.informal = std::clamp(
			static_cast<double>(entry.get("informal", 0.0)), 0.0, 1.0);
		parsed.push_back(anchor);
	}
	if (parsed.size() < 2) {
		return;
	}
	std::sort(parsed.begin(), parsed.end(),
		[](const DemoSectorAnchor& a, const DemoSectorAnchor& b) {
			return a.income < b.income;
		});
	const Dictionary agri = root.get("agriculture", Dictionary());
	if (!agri.is_empty()) {
		demo_agri_labor_per_ton_ = std::clamp(
			static_cast<double>(agri.get("labor_per_ton_base",
				demo_agri_labor_per_ton_)), 0.1, 5000.0);
		demo_agri_labor_exponent_ = std::clamp(
			static_cast<double>(agri.get("productivity_exponent",
				demo_agri_labor_exponent_)), 0.0, 4.0);
		demo_agri_production_band_min_ = std::clamp(
			static_cast<double>(agri.get("production_band_min",
				demo_agri_production_band_min_)), 0.05, 1.0);
		demo_agri_production_band_max_ = std::clamp(
			static_cast<double>(agri.get("production_band_max",
				demo_agri_production_band_max_)), 1.0, 4.0);
		demo_agri_own_consumption_share_ = std::clamp(
			static_cast<double>(agri.get("own_consumption_share",
				demo_agri_own_consumption_share_)), 0.0, 1.0);
	}
	const Dictionary industry = root.get("industry", Dictionary());
	if (!industry.is_empty()) {
		demo_industry_labor_per_va_ = std::clamp(
			static_cast<double>(industry.get("labor_per_value_added",
				demo_industry_labor_per_va_)), 0.0, 1000.0);
		demo_industry_labor_exponent_ = std::clamp(
			static_cast<double>(industry.get("productivity_exponent",
				demo_industry_labor_exponent_)), 0.0, 4.0);
	}
	demo_sector_anchors_ = std::move(parsed);
	demo_sector_loaded_ = true;
	demo_status_invalidate();
}

void EconomySim::demo_sector_anchor_values(double income, double* out) const {
	// Interpolação linear no log da renda entre as âncoras observadas (ILO).
	// Fora da faixa, vale o extremo (nação mais pobre/rica do conjunto).
	double incomes[kDemoSectorAnchorCount];
	double agri[kDemoSectorAnchorCount];
	double industry[kDemoSectorAnchorCount];
	double informal[kDemoSectorAnchorCount];
	int count = 0;
	if (!demo_sector_anchors_.empty()) {
		for (const DemoSectorAnchor& anchor : demo_sector_anchors_) {
			incomes[count] = anchor.income;
			agri[count] = anchor.agriculture;
			industry[count] = anchor.industry;
			informal[count] = anchor.informal;
			++count;
		}
	} else {
		for (int i = 0; i < kDemoSectorAnchorCount; ++i) {
			incomes[i] = kDemoSectorAnchorDefaults[i].income;
			agri[i] = kDemoSectorAnchorDefaults[i].agriculture;
			industry[i] = kDemoSectorAnchorDefaults[i].industry;
			informal[i] = kDemoSectorAnchorDefaults[i].informal;
		}
		count = kDemoSectorAnchorCount;
	}
	const double x = std::log(std::clamp(income, 0.02, 10.0));
	if (x <= std::log(incomes[0])) {
		out[0] = agri[0];
		out[1] = industry[0];
		out[2] = informal[0];
		return;
	}
	if (x >= std::log(incomes[count - 1])) {
		out[0] = agri[count - 1];
		out[1] = industry[count - 1];
		out[2] = informal[count - 1];
		return;
	}
	for (int i = 1; i < count; ++i) {
		const double x0 = std::log(incomes[i - 1]);
		const double x1 = std::log(incomes[i]);
		if (x > x1) {
			continue;
		}
		const double t = x1 > x0 ? std::clamp((x - x0) / (x1 - x0), 0.0, 1.0) : 0.0;
		out[0] = agri[i - 1] + (agri[i] - agri[i - 1]) * t;
		out[1] = industry[i - 1] + (industry[i] - industry[i - 1]) * t;
		out[2] = informal[i - 1] + (informal[i] - informal[i - 1]) * t;
		return;
	}
	out[0] = agri[count - 1];
	out[1] = industry[count - 1];
	out[2] = informal[count - 1];
}

void EconomySim::demo_update_sector_employment(DemoCity& city) {
	DemoPop& pop = city.pop;
	const double tfp = std::clamp(demo_effective_productivity(city), 0.25, 4.0);
	double anchors[3] = {0.0, 0.0, 0.0};
	demo_sector_anchor_values(tfp, anchors);
	// 1. Produção agrícola local (t/dia): construções PRIMÁRIAS cujo produto
	// está na cesta alimentar. É a capacidade declarada — a mesma que o status
	// publica em `production` —, não o vendido do dia (preço/caixa não muda a
	// necessidade de mão de obra da lavoura).
	city.agri_output_tons = 0.0;
	double industry_va = 0.0;
	const std::size_t city_index =
		static_cast<std::size_t>(&city - demo_cities_.data());
	if (city_index < demo_buildings_by_city_.size()) {
		for (const int bi : demo_buildings_by_city_[city_index]) {
			const DemoBuilding& building =
				demo_buildings_[static_cast<std::size_t>(bi)];
			if (building.output_product < 0) {
				continue;
			}
			if (building.input_product >= 0 || building.recipe >= 0) {
				// Valor adicionado do dia da planta secundária (receita menos
				// insumos, como no PIB da cidade).
				industry_va += std::max(0.0,
					building.revenue - building.input_value);
				continue;
			}
			for (const int food : demo_food_products_) {
				if (food == building.output_product) {
					city.agri_output_tons +=
						std::max(0.0, building.capacity * building.output_scale);
					break;
				}
			}
		}
	}
	const double employed = std::max(0.0, pop.employed);
	if (employed <= 0.0) {
		for (int i = 0; i < 4; ++i) {
			city.sector_workers[i] = 0.0;
			city.sector_share[i] = 0.0;
		}
		return;
	}
	const double labor_force = std::max(pop.labor_force, 1e-9);
	// 2. Demanda de trabalho agrícola: t/dia × base / TFP^expoente. A âncora
	// ILO dá a ESTRUTURA (share of total employment); a produção local modula
	// em [band_min, band_max] — a capacidade de fazenda do cenário (1 por
	// cidade) não é uma função de produção calibrada, então ela não pode
	// substituir a estrutura observada (ver o relatório F9).
	const double production_share = city.agri_output_tons
		* demo_agri_labor_per_ton_ / std::pow(tfp, demo_agri_labor_exponent_)
		/ labor_force;
	const double production_factor = anchors[0] > 1e-9
		? std::clamp(production_share / anchors[0],
			demo_agri_production_band_min_, demo_agri_production_band_max_)
		: 1.0;
	double agri = std::clamp(anchors[0] * production_factor, 0.0, 0.65)
		* employed;
	// 3. Indústria/mineração: âncora de renda (ILO) e, como piso adicional, o
	// valor adicionado simulado por trabalhador (1/TFP). No cenário atual a
	// indústria é um esqueleto (poucas plantas), então a âncora domina.
	const double va_jobs = demo_industry_labor_per_va_ > 0.0
		? industry_va * demo_industry_labor_per_va_
			/ std::pow(tfp, demo_industry_labor_exponent_)
		: 0.0;
	double industry = std::clamp(
		std::max(anchors[1] * employed, va_jobs), 0.0, 0.60 * employed);
	// 4. Serviços é o residual. A soma nunca passa do emprego; a informalidade
	// é uma sobreposição (parte dos três setores), não um quarto setor.
	if (agri + industry > employed) {
		const double scale = employed / std::max(agri + industry, 1e-9);
		agri *= scale;
		industry *= scale;
	}
	const double services = std::max(0.0, employed - agri - industry);
	city.sector_workers[0] = agri;
	city.sector_workers[1] = industry;
	city.sector_workers[2] = services;
	city.sector_workers[3] = std::clamp(anchors[2], 0.0, 1.0) * employed;
	for (int i = 0; i < 4; ++i) {
		city.sector_share[i] = employed > 1e-9
			? city.sector_workers[i] / employed : 0.0;
	}
	// F8 → F9: `service_workers` publica o EMPREGO do setor de serviços (a
	// fatia estrutural), não mais as vagas pagas pelo setor. As vagas pagas
	// continuam em `service_jobs_filled` — salário e folha seguem por elas.
	city.service_workers = services;
}

double EconomySim::demo_labor_day(DemoCity& city) {
	DemoPop& pop = city.pop;
	// 1. QUALIFICAÇÃO — conclusão. A coorte em formação conclui a uma taxa
	// constante (coorte exponencial com duração média de 3/4 anos): quem
	// conclui muda de nível de escolaridade e passa a disputar as vagas do
	// nível acima. A população não muda; só a distribuição de educação.
	for (int dst = 1; dst < kDemoEducationLevels; ++dst) {
		double& cohort = pop.studying[dst - 1];
		const double duration = kLaborStudyDurationDays[dst - 1];
		if (cohort <= 0.0 || duration <= 0.0) {
			continue;
		}
		const double completed = cohort / duration
			* (1.0 + kDemoServiceEducationRateBonus * city.private_education_cover);
		const int src = dst - 1;
		const double moved = std::min(completed, pop.education[src]);
		cohort = std::max(0.0, cohort - moved);
		pop.education[src] = std::max(0.0, pop.education[src] - moved);
		pop.education[dst] += moved;
	}
	// 2. QUALIFICAÇÃO — matrícula. O prêmio salarial do nível acima (que
	// EMERGE do aperto de cada mercado) atrai jovens do nível de origem. Sem
	// vagas no destino (desemprego alto), a escola não cresce: não se forma
	// gente para ficar desempregada. Não há custo de escola nesta fase.
	for (int dst = 1; dst < kDemoEducationLevels; ++dst) {
		const int src = dst - 1;
		const double premium = pop.wage_by_level[dst]
			/ std::max(pop.wage_by_level[src], 1e-9);
		if (premium <= kLaborStudyPremiumThreshold) {
			continue;
		}
		const double target_supply = pop.labor_force_by_level[dst];
		const double target_unemployment = target_supply > 0.0
			? pop.unemployed_by_level[dst] / target_supply : 0.0;
		const double scarcity = std::clamp(
			1.0 - target_unemployment / 0.08, 0.0, 1.0);
		const double annual = std::clamp(
			(premium - kLaborStudyPremiumThreshold) * kLaborStudyElasticity,
			0.0, kLaborStudyMaxAnnual) * scarcity;
		double& cohort = pop.studying[dst - 1];
		double flow = pop.education[src] * kLaborStudyYouthShare * annual / 365.0;
		flow = std::min(flow,
			std::max(0.0, pop.unemployed_by_level[src])
				* kLaborStudyJoblessShare);
		flow = std::min(flow,
			std::max(0.0, pop.education[src] - cohort)
				* kLaborStudyMaxDailyShare);
		if (flow > 0.0) {
			cohort += flow;
		}
	}
	// 3. FORÇA DE TRABALHO por nível (estudantes ficam fora; educação de
	// origem permanece até a conclusão).
	pop.labor_force = 0.0;
	pop.students = 0.0;
	for (int level = 0; level < kDemoEducationLevels; ++level) {
		const double studying = level < kDemoEducationLevels - 1
			? pop.studying[level] : 0.0;
		pop.students += studying;
		pop.labor_force_by_level[level] = std::max(0.0,
			pop.education[level] * pop.participation[level] - studying);
		pop.labor_force += pop.labor_force_by_level[level];
	}
	// 4. VAGAS por nível: serviços urbanos + cada construção (a mistura da
	// receita/setor). É a DEMANDA que o mercado de trabalho precisa preencher.
	double vacancy[kDemoEducationLevels] = {};
	for (int level = 0; level < kDemoEducationLevels; ++level) {
		vacancy[level] += city.service_jobs * kLaborServiceMix[level];
	}
	const std::size_t city_index =
		static_cast<std::size_t>(&city - demo_cities_.data());
	if (city_index < demo_buildings_by_city_.size()) {
		for (const int bi : demo_buildings_by_city_[city_index]) {
			const DemoBuilding& building =
				demo_buildings_[static_cast<std::size_t>(bi)];
			if (building.jobs <= 0.0) {
				continue;
			}
			for (int level = 0; level < kDemoEducationLevels; ++level) {
				vacancy[level] += building.jobs * building.education_mix[level];
			}
		}
	}
	// 5. ALOCAÇÃO: cada trabalhador procura a vaga do SEU nível e, se não
	// houver, aceita uma vaga ABAIXO (sobrequalificado); nunca o contrário.
	// `remaining` guarda as vagas não preenchidas por nível (escassez).
	double remaining[kDemoEducationLevels];
	double assigned[kDemoEducationLevels][kDemoEducationLevels] = {};
	for (int level = 0; level < kDemoEducationLevels; ++level) {
		remaining[level] = vacancy[level];
		pop.vacancies_by_level[level] = vacancy[level];
		pop.unfilled_by_level[level] = 0.0;
	}
	for (int edu = kDemoEducationLevels - 1; edu >= 0; --edu) {
		double supply = pop.labor_force_by_level[edu];
		for (int slot = edu; slot >= 0 && supply > 1e-12; --slot) {
			const double take = std::min(supply, remaining[slot]);
			if (take <= 0.0) {
				continue;
			}
			assigned[edu][slot] += take;
			remaining[slot] -= take;
			supply -= take;
		}
		pop.employed_by_level[edu] = 0.0;
		for (int slot = 0; slot < kDemoEducationLevels; ++slot) {
			pop.employed_by_level[edu] += assigned[edu][slot];
		}
		pop.unemployed_by_level[edu] =
			std::max(0.0, pop.labor_force_by_level[edu]
				- pop.employed_by_level[edu]);
	}
	for (int level = 0; level < kDemoEducationLevels; ++level) {
		pop.unfilled_by_level[level] = std::max(0.0, remaining[level]);
	}
	double fill[kDemoEducationLevels];
	for (int level = 0; level < kDemoEducationLevels; ++level) {
		fill[level] = vacancy[level] > 1e-12
			? std::clamp((vacancy[level] - remaining[level]) / vacancy[level],
				0.0, 1.0)
			: 1.0;
		// Vagas efetivamente preenchidas: peso da folha no salário ancorado no
		// valor adicionado (inclui o sobrequalificado que ocupa a vaga de baixo).
		pop.filled_by_slot[level] = std::max(0.0, vacancy[level] - remaining[level]);
	}
	// 6. FOLHA: cada construção emprega `jobs × Σ(mix × preenchimento)` e paga
	// o salário do NÍVEL DA VAGA. Vaga qualificada sem trabalhador qualificado
	// reduz o emprego (e a produção, via `workers/jobs`) proporcionalmente.
	double buildings_payroll = 0.0;
	if (city_index < demo_buildings_by_city_.size()) {
		for (const int bi : demo_buildings_by_city_[city_index]) {
			DemoBuilding& building = demo_buildings_[static_cast<std::size_t>(bi)];
			double workers_fraction = 0.0;
			double contract_wage = 0.0;
			double plant_payroll = 0.0;
			for (int level = 0; level < kDemoEducationLevels; ++level) {
				workers_fraction += building.education_mix[level] * fill[level];
				contract_wage += building.education_mix[level]
					* pop.wage_by_level[level];
				plant_payroll += building.education_mix[level] * fill[level]
					* pop.wage_by_level[level];
			}
			building.workers = building.jobs * workers_fraction;
			building.wage = contract_wage;
			const bool secondary =
				building.recipe >= 0 || building.input_product >= 0;
			const double operating = secondary
				? std::clamp(building.last_utilization, 0.05, 1.0) : 1.0;
			buildings_payroll += building.jobs * plant_payroll * operating;
		}
	}
	double service_fraction = 0.0;
	double service_contract_wage = 0.0;
	double service_payroll_per_job = 0.0;
	for (int level = 0; level < kDemoEducationLevels; ++level) {
		service_fraction += kLaborServiceMix[level] * fill[level];
		service_contract_wage += kLaborServiceMix[level]
			* pop.wage_by_level[level];
		service_payroll_per_job += kLaborServiceMix[level] * fill[level]
			* pop.wage_by_level[level];
	}
	// F8: vagas de serviço efetivamente preenchidas (peso da mistura de
	// escolaridade × preenchimento). É o emprego real do setor, não a meta.
	city.service_jobs_filled = city.service_jobs * service_fraction;
	city.service_workers = city.service_jobs_filled;
	pop.employed = 0.0;
	pop.unemployed = 0.0;
	for (int level = 0; level < kDemoEducationLevels; ++level) {
		pop.employed += pop.employed_by_level[level];
		pop.unemployed += pop.unemployed_by_level[level];
		pop.wage_income_by_level[level] = 0.0;
		for (int slot = 0; slot < kDemoEducationLevels; ++slot) {
			pop.wage_income_by_level[level] += assigned[level][slot]
				* pop.wage_by_level[slot];
		}
	}
	pop.dependents = std::max(0.0, pop.population - pop.labor_force);
	// F9: decompõe o emprego do dia em agro/indústria/serviços + informalidade.
	demo_update_sector_employment(city);
	// F7 calibração: estratos de renda = escolaridade/ocupação (ver
	// demo_refresh_strata). A população do estrato inclui os dependentes.
	demo_refresh_strata(pop);
	// 7. Caixa da firma e do setor de serviços: a folha total é paga só até o
	// caixa disponível (restrição de liquidez do modelo, preservada). O setor de
	// serviços também só paga até o próprio caixa.
	const double buildings_full_payroll = buildings_payroll;
	const double firm_available = std::max(0.0, city.firm_cash);
	const double committed = buildings_full_payroll;
	const double pay_scale = committed > firm_available
		? firm_available / committed : 1.0;
	if (city_index < demo_buildings_by_city_.size()) {
		for (const int bi : demo_buildings_by_city_[city_index]) {
			DemoBuilding& building = demo_buildings_[static_cast<std::size_t>(bi)];
			const bool secondary =
				building.recipe >= 0 || building.input_product >= 0;
			const double operating = secondary
				? std::clamp(building.last_utilization, 0.05, 1.0) : 1.0;
			double plant_payroll = 0.0;
			for (int level = 0; level < kDemoEducationLevels; ++level) {
				plant_payroll += building.education_mix[level] * fill[level]
					* pop.wage_by_level[level];
			}
			building.paid_wages =
				building.jobs * plant_payroll * operating * pay_scale;
		}
	}
	const double full_service_bill = city.service_jobs * service_payroll_per_job;
	const double paid_service_bill = std::min(full_service_bill,
		std::max(0.0, city.service_cash));
	// Ganchos do laboratório mantidos: `service_wage_adjust` é a velocidade com
	// que o salário pago do setor converge para a média do mercado de trabalho
	// e `service_wage_max` limita o valor (parâmetros de balanceamento, não
	// política). Com os padrões (0,10 e 1,0) o salário acompanha o mercado.
	const double market_service_wage = service_fraction > 1e-12
		? service_contract_wage / std::max(service_fraction, 1e-12) : 0.0;
	if (market_service_wage > 0.0) {
		city.service_wage += (market_service_wage - city.service_wage)
			* std::clamp(demo_service_wage_adjust_, 0.0, 1.0);
	}
	city.service_wage = std::min(
		std::max(0.0, city.service_wage), demo_service_wage_max_);
	city.service_wage_bill = paid_service_bill;
	const double paid_total = buildings_full_payroll * pay_scale
		+ paid_service_bill;
	// A renda por nível é a folha CHEIA por nível, escalada pela mesma fração
	// efetivamente paga (liquidez + teto do caixa de serviços).
	const double full_level_total = pop.wage_income_by_level[0]
		+ pop.wage_income_by_level[1] + pop.wage_income_by_level[2];
	const double paid_ratio = full_level_total > 1e-12
		? std::clamp(paid_total / full_level_total, 0.0, 1.0) : 0.0;
	for (int level = 0; level < kDemoEducationLevels; ++level) {
		pop.wage_income_by_level[level] *= paid_ratio;
	}
	pop.wage_income = pop.wage_income_by_level[0]
		+ pop.wage_income_by_level[1] + pop.wage_income_by_level[2];
	city.firm_cash -= buildings_payroll * pay_scale;
	city.service_cash -= paid_service_bill;
	return pop.wage_income;
}

// ---------------------------------------------------------------------------
// F8 — SERVIÇOS
// ---------------------------------------------------------------------------

int EconomySim::demo_service_sector_index(const std::string& sector) const {
	for (std::size_t index = 0; index < demo_service_sectors_.size(); ++index) {
		if (demo_service_sectors_[index].id == sector) {
			return static_cast<int>(index);
		}
	}
	return -1;
}

void EconomySim::demo_set_services(const Variant& services) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	Array sectors;
	if (services.get_type() == Variant::DICTIONARY) {
		const Dictionary data = services;
		sectors = data.get("sectors", Array());
		demo_service_government_share_ = std::clamp(
			safe_nonneg(static_cast<double>(data.get("government_spend_share",
				demo_service_government_share_))), 0.0, 0.05);
	} else if (services.get_type() == Variant::ARRAY) {
		sectors = services;
	}
	if (sectors.size() != kDemoMaxServiceSectors) {
		return;
	}
	std::vector<DemoServiceSector> parsed;
	std::vector<std::vector<std::string>> service_ids;
	parsed.reserve(kDemoMaxServiceSectors);
	service_ids.reserve(kDemoMaxServiceSectors);
	for (int index = 0; index < sectors.size(); ++index) {
		if (sectors[index].get_type() != Variant::DICTIONARY) {
			return;
		}
		const Dictionary d = sectors[index];
		DemoServiceSector sector;
		sector.id = std::string(String(d.get("id", "")).utf8().get_data());
		if (sector.id.empty()) {
			return;
		}
		sector.name_pt = std::string(String(d.get("name_pt", sector.id.c_str())).utf8().get_data());
		sector.name_en = std::string(String(d.get("name_en", sector.id.c_str())).utf8().get_data());
		sector.name_es = std::string(String(d.get("name_es", sector.id.c_str())).utf8().get_data());
		sector.icon = std::string(String(d.get("icon", sector.id.c_str())).utf8().get_data());
		sector.price = std::clamp(
			safe_nonneg(static_cast<double>(d.get("price", 1.0))), 0.01, 100000.0);
		const Dictionary pop_share = d.get("pop_share", Dictionary());
		for (int level = 0; level < kDemoMaxNeedsLevels; ++level) {
			static const char* const kLevelKeys[3] = {"basic", "life", "luxury"};
			sector.pop_share[level] = std::max(0.0,
				static_cast<double>(pop_share.get(kLevelKeys[level], 0.0)));
		}
		sector.business_share = std::max(0.0,
			static_cast<double>(d.get("business_share", 0.0)));
		sector.freight_share = std::max(0.0,
			static_cast<double>(d.get("freight_share", 0.0)));
		sector.government_share = std::max(0.0,
			static_cast<double>(d.get("government_share", 0.0)));
		const Array inputs = d.get("input_goods", Array());
		for (int slot = 0; slot < inputs.size(); ++slot) {
			const Dictionary input = inputs[slot];
			const int product = demo_product_index(
				String(input.get("good", "")));
			const double qty = safe_nonneg(
				static_cast<double>(input.get("per_unit", 0.0)));
			if (product < 0 || qty <= 0.0) {
				continue;
			}
			sector.inputs.push_back({product, qty});
		}
		std::vector<std::string> names;
		const Array service_inputs = d.get("input_services", Array());
		for (int slot = 0; slot < service_inputs.size() && slot < 2; ++slot) {
			const Dictionary input = service_inputs[slot];
			const std::string id = std::string(
				String(input.get("sector", "")).utf8().get_data());
			const double qty = safe_nonneg(
				static_cast<double>(input.get("per_unit", 0.0)));
			if (id.empty() || qty <= 0.0) {
				continue;
			}
			names.push_back(id);
			sector.service_input_qty.push_back(qty);
		}
		const Array mix = d.get("education_mix", Array());
		double mix_sum = 0.0;
		for (int level = 0; level < kDemoEducationLevels; ++level) {
			sector.education_mix[level] = level < mix.size()
				? std::max(0.0, static_cast<double>(mix[level])) : 0.0;
			mix_sum += sector.education_mix[level];
		}
		if (mix_sum <= 1e-9) {
			sector.education_mix[0] = 1.0;
		} else {
			for (int level = 0; level < kDemoEducationLevels; ++level) {
				sector.education_mix[level] /= mix_sum;
			}
		}
		sector.jobs_per_unit = std::clamp(
			safe_nonneg(static_cast<double>(d.get("jobs_per_unit", 3.0))), 0.001, 1000.0);
		sector.capital_per_unit = std::clamp(
			safe_nonneg(static_cast<double>(d.get("capital_per_unit", 96.0))), 0.01, 1.0e7);
		sector.min_capacity = std::clamp(
			safe_nonneg(static_cast<double>(d.get("min_capacity", 5.0))), 0.01, 1.0e7);
		sector.exportable = bool(d.get("exportable", false));
		sector.export_share = std::clamp(
			safe_nonneg(static_cast<double>(d.get("export_share", 0.0))), 0.0, 0.5);
		sector.health = bool(d.get("health", false));
		sector.education = bool(d.get("education", false));
		sector.source = std::string(
			String(d.get("source", "estimated:structure")).utf8().get_data());
		parsed.push_back(std::move(sector));
		service_ids.push_back(std::move(names));
	}
	// Resolve os serviços intermediários depois de todos os ids existirem.
	for (std::size_t index = 0; index < parsed.size(); ++index) {
		for (const std::string& id : service_ids[index]) {
			int found = -1;
			for (std::size_t other = 0; other < parsed.size(); ++other) {
				if (parsed[other].id == id) {
					found = static_cast<int>(other);
					break;
				}
			}
			if (found >= 0 && found != static_cast<int>(index)) {
				parsed[index].service_inputs.push_back(found);
			} else if (found == static_cast<int>(index)) {
				parsed[index].service_input_qty.pop_back();
			}
		}
		if (parsed[index].service_inputs.size() != parsed[index].service_input_qty.size()) {
			parsed[index].service_inputs.clear();
			parsed[index].service_input_qty.clear();
		}
	}
	demo_service_sectors_ = std::move(parsed);
	demo_services_loaded_ = true;
	for (DemoCity& city : demo_cities_) {
		for (int sector = 0; sector < kDemoMaxServiceSectors; ++sector) {
			DemoServiceMarket& market = city.service_markets[sector];
			market.base_price = demo_service_sectors_[static_cast<std::size_t>(sector)].price;
			if (market.price <= 0.0 || !std::isfinite(market.price)) {
				market.price = market.base_price;
			}
		}
	}
	if (!demo_cities_.empty() && demo_service_businesses_.empty()) {
		demo_seed_services();
	}
}

Array EconomySim::demo_get_service_sectors() const {
	Array out;
	for (const DemoServiceSector& sector : demo_service_sectors_) {
		Dictionary entry;
		entry["id"] = String(sector.id.c_str());
		entry["name_pt"] = String(sector.name_pt.c_str());
		entry["name_en"] = String(sector.name_en.c_str());
		entry["name_es"] = String(sector.name_es.c_str());
		entry["icon"] = String(sector.icon.c_str());
		entry["price"] = sector.price;
		entry["exportable"] = sector.exportable;
		entry["health"] = sector.health;
		entry["education"] = sector.education;
		entry["jobs_per_unit"] = sector.jobs_per_unit;
		Array mix;
		for (int level = 0; level < kDemoEducationLevels; ++level) {
			mix.append(sector.education_mix[level]);
		}
		entry["education_mix"] = mix;
		entry["source"] = String(sector.source.c_str());
		out.append(entry);
	}
	return out;
}

void EconomySim::demo_rebuild_service_indexes() {
	demo_service_by_city_.assign(demo_cities_.size(), {});
	demo_service_by_sector_.assign(demo_service_sectors_.size(), {});
	for (int index = 0; index < static_cast<int>(demo_service_businesses_.size()); ++index) {
		const DemoServiceBusiness& business =
			demo_service_businesses_[static_cast<std::size_t>(index)];
		if (business.bankrupt) {
			continue;
		}
		const auto found = demo_city_lookup_.find(business.city);
		if (found != demo_city_lookup_.end()) {
			demo_service_by_city_[static_cast<std::size_t>(found->second)].push_back(index);
		}
		if (business.sector >= 0
				&& business.sector < static_cast<int>(demo_service_by_sector_.size())) {
			demo_service_by_sector_[static_cast<std::size_t>(business.sector)].push_back(index);
		}
	}
}

double EconomySim::demo_city_food_reserve(const DemoCity& city) const {
	// F8 v2: reserva alimentar robusta (XGS) que a poupança deve preservar antes
	// de financiar abertura de negócio. Usa a MESMA regra prudencial da F3
	// (kDemoFoodReserveDays de cesta) — a poupança que importa comida em nação
	// sem terra fica intocada; só o excedente financia serviço.
	const double need = std::max(0.0,
		city.simulation_population * kDemoNeedPerCapita);
	double local = 0.0;
	double import = 0.0;
	double per_ge = 0.0;
	demo_food_basket_cost(city, need, local, import, per_ge);
	return need * kDemoFoodReserveDays * per_ge;
}

bool EconomySim::demo_service_open(DemoCity& city, int sector, double capacity,
		double capital) {
	if (sector < 0 || sector >= static_cast<int>(demo_service_sectors_.size())) {
		return false;
	}
	const DemoServiceSector& definition =
		demo_service_sectors_[static_cast<std::size_t>(sector)];
	capacity = std::max(0.0, capacity);
	capital = std::max(0.0, capital);
	if (capacity <= 0.0) {
		return false;
	}
	// F8 v2: o capital de abertura NÃO pode sair da poupança que paga comida
	// (F3 soma savings + renda para importar alimento — DJI regredia 0,472→0,355
	// quando o capital drenava essa poupança, Q-004). Ordem de financiamento:
	//   1º caixa do PRÓPRIO SETOR (lucros retidos em service_cash) — já está no
	//      setor, vira equity do negócio sem movimento de dinheiro;
	//   2º poupança das famílias ACIMA da reserva alimentar robusta — move
	//      savings→service_cash (e é devolvida no fechamento).
	// Se nem a soma cobre o capital sem tocar a reserva de comida, não abre.
	const double food_reserve = demo_city_food_reserve(city);
	const double savings_surplus = std::max(0.0, city.pop.savings - food_reserve);
	const double from_sector = std::min(capital, std::max(0.0, city.service_cash));
	const double from_savings = std::min(capital - from_sector, savings_surplus);
	if (from_sector + from_savings < capital) {
		return false;
	}
	if (from_savings > 0.0) {
		city.pop.savings -= from_savings;
		city.service_cash += from_savings;
	}
	DemoServiceBusiness business;
	business.id = demo_next_service_business_id_++;
	business.sector = sector;
	business.city = city.id;
	business.owner_stratum = kDemoEducationLevels - 1;
	business.capacity = capacity;
	business.jobs = capacity * definition.jobs_per_unit;
	business.workers = business.jobs;
	// `capital` guarda só a parcela da POP (devolvida no fechamento); a equity
	// total (capital_left) soma a parcela do setor.
	business.capital = from_savings;
	business.capital_left = from_sector + from_savings;
	business.opened_day = day_;
	unsigned long long seed = 1469598103934665603ULL;
	auto mix = [&seed](const std::string& text) {
		for (const char c : text) {
			seed ^= static_cast<unsigned char>(c);
			seed *= 1099511628211ULL;
		}
		seed ^= 0xffULL;
		seed *= 1099511628211ULL;
	};
	mix(city.id);
	mix(definition.id);
	mix(std::to_string(business.id));
	business.name_seed = seed;
	const int index = static_cast<int>(demo_service_businesses_.size());
	demo_service_businesses_.push_back(std::move(business));
	const auto found = demo_city_lookup_.find(city.id);
	if (found != demo_city_lookup_.end()) {
		if (demo_service_by_city_.size() != demo_cities_.size()) {
			demo_service_by_city_.assign(demo_cities_.size(), {});
		}
		demo_service_by_city_[static_cast<std::size_t>(found->second)].push_back(index);
	}
	if (demo_service_by_sector_.size() != demo_service_sectors_.size()) {
		demo_service_by_sector_.assign(demo_service_sectors_.size(), {});
	}
	demo_service_by_sector_[static_cast<std::size_t>(sector)].push_back(index);
	city.service_business_count += 1;
	return true;
}

void EconomySim::demo_service_close(DemoServiceBusiness& business) {
	if (business.bankrupt) {
		return;
	}
	business.bankrupt = true;
	DemoCity* city = find_demo_city(business.city);
	if (city != nullptr) {
		// F8 v2: devolve à POP no máximo a parcela que ELA aportou
		// (`business.capital` = from_savings); a parcela financiada pelo setor
		// fica no setor. `capital_left` pode exceder o aporte quando o negócio
		// lucra — o lucro já foi distribuído no agregado do setor, então só o
		// aporte da POP volta (o teto pelo service_cash evita drenar o giro).
		const double returned = std::min(
			std::max(0.0, business.capital_left),
			std::max(0.0, business.capital));
		const double paid_back = std::min(returned, std::max(0.0, city->service_cash));
		city->service_cash -= paid_back;
		city->pop.savings += paid_back;
		city->service_business_count = std::max(0, city->service_business_count - 1);
	}
	business.capital_left = 0.0;
	demo_service_bankruptcies_ += 1;
}

void EconomySim::demo_seed_services() {
	demo_service_businesses_.clear();
	demo_service_businesses_.shrink_to_fit();
	demo_service_bankruptcies_ = 0;
	demo_next_service_business_id_ = 1;
	demo_service_by_city_.assign(demo_cities_.size(), {});
	demo_service_by_sector_.assign(demo_service_sectors_.size(), {});
	if (!demo_services_loaded_ || demo_service_sectors_.empty()) {
		return;
	}
	for (DemoCity& city : demo_cities_) {
		city.service_business_count = 0;
		for (int sector = 0; sector < kDemoMaxServiceSectors; ++sector) {
			DemoServiceMarket& market = city.service_markets[sector];
			market.base_price =
				demo_service_sectors_[static_cast<std::size_t>(sector)].price;
			if (market.price <= 0.0 || !std::isfinite(market.price)) {
				market.price = market.base_price;
			}
		}
		const double population = std::max(1.0, city.simulation_population);
		int count = static_cast<int>(std::clamp(
			4.0 + std::log10(std::max(1000.0, population)) * 2.0, 4.0, 16.0));
		// Escolhe os setores com maior peso de consumo das famílias; setores
		// exportáveis entram quando o país tem renda acima da média.
		std::vector<int> order(demo_service_sectors_.size());
		std::vector<double> weight(demo_service_sectors_.size(), 0.0);
		for (std::size_t sector = 0; sector < demo_service_sectors_.size(); ++sector) {
			order[sector] = static_cast<int>(sector);
			const DemoServiceSector& definition = demo_service_sectors_[sector];
			weight[sector] = definition.pop_share[0] + definition.pop_share[1] * 1.4
				+ definition.pop_share[2] * 1.8 + definition.business_share
				+ definition.government_share * 0.5;
		}
		std::stable_sort(order.begin(), order.end(), [&weight](int a, int b) {
			return weight[static_cast<std::size_t>(a)]
				> weight[static_cast<std::size_t>(b)];
		});
		for (int slot = 0; slot < count && slot < static_cast<int>(order.size()); ++slot) {
			const int sector = order[static_cast<std::size_t>(slot)];
			const DemoServiceSector& definition =
				demo_service_sectors_[static_cast<std::size_t>(sector)];
			// F8 fix: sem o teto arbitrário de 40 postos. A semente parte do
			// mínimo viável do setor; a ENTRADA (passo 9) cresce a oferta até a
			// demanda real. A folha sobre efetivos torna o negócio ocioso
			// não-letivo, então superdimensionar não é mais fatal.
			const double capacity = definition.min_capacity;
			const double capital = capacity * definition.capital_per_unit;
			// F8 v2: o capital de abertura vem do caixa do próprio setor e, se
			// faltar, da poupança das famílias ACIMA da reserva de comida — o
			// gate interno de demo_service_open decide. Cidade pequena/pobre
			// semeia menos negócios e abre os demais quando houver caixa.
			demo_service_open(city, sector, capacity, capital);
		}
		if (city.external_income_index > 1.2) {
			for (int sector = 0; sector < static_cast<int>(demo_service_sectors_.size()); ++sector) {
				const DemoServiceSector& definition =
					demo_service_sectors_[static_cast<std::size_t>(sector)];
				if (!definition.exportable) {
					continue;
				}
				bool present = false;
				for (const int index : demo_service_by_city_[static_cast<std::size_t>(
						&city - demo_cities_.data())]) {
					if (demo_service_businesses_[static_cast<std::size_t>(index)].sector
							== sector) {
						present = true;
						break;
					}
				}
				if (present) {
					continue;
				}
				const double capacity = definition.min_capacity;
				const double capital = capacity * definition.capital_per_unit;
				demo_service_open(city, sector, capacity, capital);
			}
		}
	}
}

void EconomySim::demo_services_day(DemoCity& city) {
	if (!demo_services_loaded_ || demo_service_sectors_.empty()) {
		return;
	}
	const std::size_t city_index =
		static_cast<std::size_t>(&city - demo_cities_.data());
	if (city_index >= demo_service_by_city_.size()) {
		demo_service_by_city_.assign(demo_cities_.size(), {});
	}
	if (demo_service_by_sector_.size() != demo_service_sectors_.size()) {
		demo_service_by_sector_.assign(demo_service_sectors_.size(), {});
	}
	const DemoPop& pop = city.pop;
	const double wage = std::max(0.0, city.service_wage);
	// F8 v2 (calibração, Q-004): o custo de trabalho do negócio é o salário de
	// mercado INTEGRAL — sem o teto `preço÷jobs_per_unit×0,68`, que era a muleta
	// que mascarava o preço sub-calibrado. A margem realista vem da ÂNCORA DE
	// PREÇO escalada pelo salário local (passo 8): price/jobs_per_unit acompanha
	// o salário, então o negócio não fica estruturalmente no prejuízo em país
	// rico — o teto contábil deixa de ser necessário.
	// Reserva alimentar robusta (calculada UMA vez por cidade/dia): a poupança
	// acima dela é o que pode financiar a entrada de negócio, sem tocar a
	// poupança que importa comida (DJI, Q-004).
	const double food_reserve = demo_city_food_reserve(city);
	const double savings_surplus = std::max(0.0, city.pop.savings - food_reserve);

	// 1. Pesos da demanda das famílias por classe de consumo (basic/life/
	// luxury). Com Gini, `class_population` é a repartição real; sem Gini,
	// espelha os estratos (mesma regra da F7). O dinheiro é o
	// `pop.service_spend` já debitado das famílias; aqui ele é DISTRIBUÍDO.
	double class_pop[kDemoEducationLevels] = {
		std::max(0.0, pop.class_population[0]),
		std::max(0.0, pop.class_population[1]),
		std::max(0.0, pop.class_population[2])};
	if (class_pop[0] + class_pop[1] + class_pop[2] <= 1e-9) {
		for (int level = 0; level < kDemoEducationLevels; ++level) {
			class_pop[level] = std::max(0.0, pop.stratum_population[level]);
		}
	}
	double weight[kDemoMaxServiceSectors] = {};
	double weight_total = 0.0;
	for (int sector = 0; sector < kDemoMaxServiceSectors; ++sector) {
		const DemoServiceSector& definition =
			demo_service_sectors_[static_cast<std::size_t>(sector)];
		double value = 0.0;
		for (int level = 0; level < kDemoMaxNeedsLevels; ++level) {
			value += class_pop[level] * definition.pop_share[level];
		}
		weight[sector] = value;
		weight_total += value;
	}
	const double household_pool = std::max(0.0, pop.service_spend);

	// 2. B2B: o opex pago pelas empresas é receita de serviços. Finanças,
	// profissionais, TI, telecom, manutenção e limpeza entram por participação
	// declarada; logística entra pela TONELAGEM movimentada ontem (sold_prev da
	// cesta + insumos), como pede o plano.
	double tonnage = 0.0;
	for (int product = 0; product < demo_products_; ++product) {
		tonnage += std::max(0.0, city.markets[product].sold_prev);
	}
	city.service_tonnage_prev = tonnage;
	const double b2b_pool = std::max(0.0, city.opex_revenue);
	double b2b_weight[kDemoMaxServiceSectors] = {};
	double b2b_total = 0.0;
	for (int sector = 0; sector < kDemoMaxServiceSectors; ++sector) {
		const DemoServiceSector& definition =
			demo_service_sectors_[static_cast<std::size_t>(sector)];
		double value = definition.business_share;
		if (definition.freight_share > 0.0) {
			value += definition.freight_share
				* std::clamp(tonnage / 2000.0, 0.05, 4.0);
		}
		b2b_weight[sector] = value;
		b2b_total += value;
	}

	// 3. Governo: compra uma fração pequena do orçamento em serviços
	// (limpeza/segurança/resíduos, construção, profissionais, saúde/educação).
	// É transferência explícita governo → service_cash (dinheiro conservado).
	double government_pool = 0.0;
	double gov_weight[kDemoMaxServiceSectors] = {};
	double gov_total = 0.0;
	for (int sector = 0; sector < kDemoMaxServiceSectors; ++sector) {
		gov_weight[sector] =
			demo_service_sectors_[static_cast<std::size_t>(sector)].government_share;
		gov_total += gov_weight[sector];
	}
	if (gov_total > 0.0) {
		// A fração é da ARRECADAÇÃO DO DIA (fluxo), não do caixa acumulado:
		// cobrar a fração sobre o estoque todo dia drenava o tesouro que
		// financia a importação de comida (AFG-KAN/KAB em 365 d, F8). O caixa
		// do dia continua sendo o teto.
		const double daily_flow = std::max(0.0, pop.taxes);
		government_pool = std::min(daily_flow * demo_service_government_share_,
			std::max(0.0, city.government_budget));
		city.government_budget -= government_pool;
		city.service_cash += government_pool;
		city.service_government_revenue += government_pool;
	}

	// 4. Capacidade e exportação. A exportação é atendida primeiro; o resto da
	// oferta fica para o mercado doméstico. O preço de exportação usa a
	// competitividade cambial da F5 (a tarifa declarada para "services" reduz a
	// competitividade, sem criar/destruir dinheiro).
	const double competitiveness = std::clamp(
		demo_competitiveness(city.nation), 0.5, 1.5);
	const int service_tariff_product = demo_product_index(String("services"));
	const double service_tariff = service_tariff_product >= 0
		? demo_tariff_rate(city.nation, "*", service_tariff_product) : 0.0;
	const double export_factor = competitiveness * (1.0 - std::clamp(service_tariff, 0.0, 1.0));
	double sector_supply[kDemoMaxServiceSectors] = {};
	double sector_exports[kDemoMaxServiceSectors] = {};
	double sector_export_revenue[kDemoMaxServiceSectors] = {};
	for (const int index : demo_service_by_city_[city_index]) {
		DemoServiceBusiness& business =
			demo_service_businesses_[static_cast<std::size_t>(index)];
		if (business.bankrupt || business.sector < 0
				|| business.sector >= kDemoMaxServiceSectors) {
			continue;
		}
		const double capacity = std::max(0.0,
			business.capacity * std::clamp(business.utilization > 0.0
				? business.utilization : 1.0, 0.05, 1.0));
		sector_supply[business.sector] += capacity;
	}
	const double external_factor = std::clamp(city.external_income_index, 0.2, 3.0);
	for (int sector = 0; sector < kDemoMaxServiceSectors; ++sector) {
		const DemoServiceSector& definition =
			demo_service_sectors_[static_cast<std::size_t>(sector)];
		if (!definition.exportable || definition.export_share <= 0.0) {
			continue;
		}
		const double units = sector_supply[sector] * definition.export_share
			* external_factor;
		if (units <= 0.0) {
			continue;
		}
		const double revenue = units
			* city.service_markets[sector].price * export_factor;
		sector_exports[sector] = units;
		sector_export_revenue[sector] = revenue;
		city.service_export_revenue += revenue;
		city.service_cash += revenue;
		demo_foreign_cash_ -= revenue;
	}

	// 5. Mercados: demanda em unidades = dinheiro / preço. A receita só é
	// realizada na proporção atendida (demanda sem oferta não vira dinheiro).
	double active_businesses_total = 0.0;
	for (const int index : demo_service_by_city_[city_index]) {
		const DemoServiceBusiness& business =
			demo_service_businesses_[static_cast<std::size_t>(index)];
		if (!business.bankrupt) {
			active_businesses_total += 1.0;
		}
	}
	double total_units_served = 0.0;
	double total_service_cost = 0.0;
	double total_input_cost = 0.0;
	for (int sector = 0; sector < kDemoMaxServiceSectors; ++sector) {
		DemoServiceMarket& market = city.service_markets[sector];
		market.demand = 0.0;
		market.supply = sector_supply[sector];
		market.unmet = 0.0;
		market.revenue = 0.0;
		market.input_cost = 0.0;
		market.service_cost = 0.0;
		market.jobs = 0.0;
		market.businesses = 0.0;
		market.exports = sector_exports[sector];
		market.export_revenue = sector_export_revenue[sector];
		market.coverage = 0.0;
		double money = 0.0;
		if (weight_total > 0.0) {
			money += household_pool * weight[sector] / weight_total;
		}
		if (b2b_total > 0.0) {
			money += b2b_pool * b2b_weight[sector] / b2b_total;
		}
		if (gov_total > 0.0) {
			money += government_pool * gov_weight[sector] / gov_total;
		}
		if (market.price <= 0.0) {
			market.price = std::max(0.01,
				demo_service_sectors_[static_cast<std::size_t>(sector)].price);
		}
		const double domestic_demand = money / market.price;
		const double total_demand = domestic_demand + market.exports;
		market.demand = total_demand;
		for (int level = 0; level < kDemoMaxNeedsLevels; ++level) {
			market.pop_demand[level] = weight_total > 0.0 && market.price > 1e-12
				? household_pool * class_pop[level]
					* demo_service_sectors_[static_cast<std::size_t>(sector)].pop_share[level]
					/ weight_total / market.price
				: 0.0;
		}
		const double served = std::min(market.supply, total_demand);
		const double domestic_served = std::max(0.0,
			std::min(domestic_demand, served - market.exports));
		const double export_served = std::min(market.exports, served);
		// Dinheiro realizado: fração doméstica atendida do pool + exportação
		// efetivamente entregue. O pool não gasto fica em service_cash.
		const double realized = domestic_demand > 1e-12
			? money * domestic_served / domestic_demand : 0.0;
		market.revenue = realized + export_served * market.price * export_factor;
		if (market.demand > 0.0) {
			market.unmet = std::max(0.0, market.demand - served);
		}
		total_units_served += served;
		// Cobertura privada de saúde/educação: oferta por habitante contra a
		// necessidade de referência (gancho documentado, não serviço público).
		if (demo_service_sectors_[static_cast<std::size_t>(sector)].health) {
			const double need = std::max(1e-9,
				city.simulation_population * kDemoServiceHealthNeedPerCapita);
			market.coverage = std::clamp(market.supply / need, 0.0, 1.0);
			city.private_health_cover = market.coverage;
		}
		if (demo_service_sectors_[static_cast<std::size_t>(sector)].education) {
			const double need = std::max(1e-9,
				city.simulation_population * kDemoServiceEducationNeedPerCapita);
			market.coverage = std::clamp(market.supply / need, 0.0, 1.0);
			city.private_education_cover = market.coverage;
		}
	}

	// 6. Negócios: produzem a sua fatia da oferta, compram insumos (bens e
	// serviços) e pagam a folha (fatia de city.service_wage_bill). O lucro é
	// informativo para entrada/saída; a distribuição aos donos continua no
	// agregado do setor de serviços (service_profit).
	std::vector<double> sector_revenue_gain(kDemoMaxServiceSectors, 0.0);
	for (const int index : demo_service_by_city_[city_index]) {
		DemoServiceBusiness& business =
			demo_service_businesses_[static_cast<std::size_t>(index)];
		if (business.bankrupt || business.sector < 0
				|| business.sector >= kDemoMaxServiceSectors) {
			continue;
		}
		DemoServiceMarket& market = city.service_markets[business.sector];
		const DemoServiceSector& definition =
			demo_service_sectors_[static_cast<std::size_t>(business.sector)];
		market.businesses += 1.0;
		const double capacity = std::max(0.0, business.capacity);
		double units = 0.0;
		if (market.supply > 1e-12) {
			units = std::min(capacity, market.supply > 0.0
				? std::max(0.0, market.demand) * capacity / market.supply : 0.0);
			units = std::min(units, capacity);
		}
		business.utilization = capacity > 1e-12 ? units / capacity : 0.0;
		business.workers = business.jobs * std::clamp(business.utilization, 0.05, 1.0);
		const double share = market.supply > 1e-12 ? capacity / market.supply : 0.0;
		business.revenue = market.revenue * share;
		// Insumos de bens: compra no mercado da cidade ao preço corrente, até o
		// estoque disponível. O dinheiro sai de service_cash e entra em
		// firm_cash (produtor), conservando o total.
		double input_cost = 0.0;
		for (const DemoServiceSector::Input& input : definition.inputs) {
			if (input.product < 0 || input.product >= demo_products_) {
				continue;
			}
			DemoMarket& goods = city.markets[input.product];
			const double needed = input.qty * units;
			const double bought = std::min(needed, std::max(0.0, goods.stock));
			if (bought <= 0.0) {
				continue;
			}
			const double paid = bought * goods.price;
			goods.stock -= bought;
			goods.demand += bought;
			goods.sold += bought;
			goods.desired += needed;
			city.revenue_by_product[input.product] += paid;
			input_cost += paid;
		}
		business.input_cost = input_cost;
		// Insumos de serviço: transferência entre pools setoriais (não sai do
		// service_cash; o lucro do fornecedor sobe e o do comprador cai).
		double service_cost = 0.0;
		for (std::size_t slot = 0; slot < definition.service_inputs.size(); ++slot) {
			const int supplier = definition.service_inputs[slot];
			if (supplier < 0 || supplier >= kDemoMaxServiceSectors) {
				continue;
			}
			const double qty = definition.service_input_qty[slot] * units;
			const double price = city.service_markets[supplier].price;
			const double paid = qty * price;
			service_cost += paid;
			sector_revenue_gain[static_cast<std::size_t>(supplier)] += paid;
		}
		business.service_cost = service_cost;
		// F8 fix: folha sobre EFETIVOS (workers), não sobre POSTOS (jobs). O
		// negócio ocioso não paga a folha cheia — o custo acompanha a
		// utilização. A folha agregada do setor (demo_labor_day) continua sendo
		// a do empregador residual da F7; aqui só se atribui o custo marginal.
		business.wages = business.workers * wage;
		business.profit = business.revenue - business.wages
			- business.input_cost - business.service_cost;
		business.capital_left = std::max(0.0,
			business.capital_left + business.profit);
		if (business.profit < 0.0 && business.utilization > 1e-9) {
			++business.loss_days;
		} else if (business.profit > 0.0) {
			business.loss_days = 0;
		}
		total_input_cost += business.input_cost;
		total_service_cost += business.service_cost;
	}
	// Receita dos fornecedores de serviços: aumenta o pool publicado do setor.
	for (int sector = 0; sector < kDemoMaxServiceSectors; ++sector) {
		if (sector_revenue_gain[static_cast<std::size_t>(sector)] > 0.0) {
			city.service_markets[sector].revenue +=
				sector_revenue_gain[static_cast<std::size_t>(sector)];
		}
	}
	// E chega aos negócios fornecedores (mesma proporção de capacidade): o
	// lucro do comprador já pagou o serviço, então o efeito agregado é nulo.
	for (const int index : demo_service_by_city_[city_index]) {
		DemoServiceBusiness& business =
			demo_service_businesses_[static_cast<std::size_t>(index)];
		if (business.bankrupt || business.sector < 0
				|| business.sector >= kDemoMaxServiceSectors) {
			continue;
		}
		const double gain = sector_revenue_gain[static_cast<std::size_t>(business.sector)];
		if (gain <= 0.0) {
			continue;
		}
		const double supply = city.service_markets[business.sector].supply;
		const double share = supply > 1e-12 ? business.capacity / supply : 0.0;
		business.revenue += gain * share;
		business.profit += gain * share;
		business.capital_left += gain * share;
	}
	// Paga os insumos de bens: service_cash → firm_cash.
	const double paid_inputs = std::min(total_input_cost,
		std::max(0.0, city.service_cash));
	if (paid_inputs > 0.0) {
		city.service_cash -= paid_inputs;
		city.firm_cash += paid_inputs;
	}
	city.service_input_cost = total_input_cost;

	// 7. Emprego por setor: divide o emprego AGREGADO de serviços da F7 (o
	// empregador residual) pela participação dos negócios de cada setor. Sem
	// negócios, a divisão segue os pesos de demanda — a soma continua igual a
	// city.service_jobs (nenhum emprego é criado ou perdido aqui).
	double jobs_weight[kDemoMaxServiceSectors] = {};
	double jobs_total = 0.0;
	for (const int index : demo_service_by_city_[city_index]) {
		const DemoServiceBusiness& business =
			demo_service_businesses_[static_cast<std::size_t>(index)];
		if (business.bankrupt || business.sector < 0
				|| business.sector >= kDemoMaxServiceSectors) {
			continue;
		}
		jobs_weight[business.sector] += std::max(0.0, business.jobs);
		jobs_total += std::max(0.0, business.jobs);
	}
	if (jobs_total <= 1e-12) {
		for (int sector = 0; sector < kDemoMaxServiceSectors; ++sector) {
			jobs_weight[sector] = weight[sector];
			jobs_total += weight[sector];
		}
	}
	for (int sector = 0; sector < kDemoMaxServiceSectors; ++sector) {
		city.service_markets[sector].jobs = jobs_total > 1e-12
			? city.service_jobs * jobs_weight[sector] / jobs_total : 0.0;
	}

	// 8. Preço: reage à razão demanda financiada/oferta, com suavização e
	// banda em torno da âncora. Oferta zero com demanda > 0 sobe ao teto.
	for (int sector = 0; sector < kDemoMaxServiceSectors; ++sector) {
		DemoServiceMarket& market = city.service_markets[sector];
		const DemoServiceSector& definition =
			demo_service_sectors_[static_cast<std::size_t>(sector)];
		// F8 v2 — ÂNCORA LIGADA AO SALÁRIO DO PAÍS: o preço-âncora do setor
		// escala com o salário de serviço local (kDemoServiceWage = 0,15, a
		// âncora salarial da F7). Sem isso price/jobs_per_unit ficava fixo em
		// ~0,22 e, em país rico, abaixo do salário — o negócio formal ficava no
		// prejuízo estrutural (o teto 0,68 mascarava). Com a âncora escalando, a
		// receita por posto passa a ≈ 1,47× o salário local, então a margem fica
		// estável em país rico e pobre; os limites evitam preço zero/explosivo.
		const double anchor_scale = std::clamp(
			wage / kDemoServiceWage, kDemoServiceAnchorScaleMin,
			kDemoServiceAnchorScaleMax);
		market.base_price = definition.price * anchor_scale
			* kDemoServiceMarginCalibration;
		const double ratio = market.supply > 1e-9
			? std::max(0.0, market.demand) / market.supply
			: (market.demand > 1e-9 ? 4.0 : 1.0);
		const double target = market.base_price * std::clamp(
			std::pow(ratio, 0.35), kDemoServicePriceBandLow,
			kDemoServicePriceBandHigh);
		market.price += (target - market.price) * kDemoServicePriceSmooth;
		market.price = std::clamp(market.price,
			market.base_price * kDemoServicePriceBandLow,
			market.base_price * kDemoServicePriceBandHigh);
	}

	// 9. Entrada e saída. Entrada só com demanda não atendida, margem esperada
	// acima do limiar e capital financiável pelo caixa do setor + poupança acima
	// da reserva de comida. Até kDemoServiceOpensPerDay aberturas por cidade/dia
	// (teto, não alvo: sem demanda não atendida, não abre nada).
	int opens = 0;
	for (int sector = 0; sector < kDemoMaxServiceSectors && opens < kDemoServiceOpensPerDay; ++sector) {
		DemoServiceMarket& market = city.service_markets[sector];
		const DemoServiceSector& definition =
			demo_service_sectors_[static_cast<std::size_t>(sector)];
		if (market.unmet < definition.min_capacity * 0.5) {
			continue;
		}
		int existing = 0;
		for (const int index : demo_service_by_city_[city_index]) {
			const DemoServiceBusiness& business =
				demo_service_businesses_[static_cast<std::size_t>(index)];
			if (!business.bankrupt && business.sector == sector) {
				++existing;
			}
		}
		if (existing >= kDemoServiceMaxPerSector) {
			continue;
		}
		// F8 v2: capacidade pela DEMANDA não atendida, com folga de 25% para a
		// oferta se manter à frente do crescimento da renda. Financiada pelo
		// caixa do setor + poupança das famílias ACIMA da reserva de comida
		// (nunca pela poupança que paga comida — DJI, Q-004).
		const double finance_base = std::max(0.0, city.service_cash) + savings_surplus;
		const double affordable = finance_base > 0.0
			? finance_base
				/ (definition.capital_per_unit * kDemoServiceCapitalReserve) : 0.0;
		if (affordable < definition.min_capacity) {
			continue;
		}
		const double capacity = std::max(definition.min_capacity,
			std::min(market.unmet * 1.25, affordable));
		const double revenue = capacity * market.price;
		double expected_cost = capacity * definition.jobs_per_unit * wage;
		for (const DemoServiceSector::Input& input : definition.inputs) {
			if (input.product >= 0 && input.product < demo_products_) {
				expected_cost += capacity * input.qty * city.markets[input.product].price;
			}
		}
		for (std::size_t slot = 0; slot < definition.service_inputs.size(); ++slot) {
			const int supplier = definition.service_inputs[slot];
			if (supplier >= 0 && supplier < kDemoMaxServiceSectors) {
				expected_cost += capacity * definition.service_input_qty[slot]
					* city.service_markets[supplier].price;
			}
		}
		const double margin = revenue > 1e-9
			? (revenue - expected_cost) / revenue : -1.0;
		if (margin < kDemoServiceEntryMargin) {
			continue;
		}
		const double capital = capacity * definition.capital_per_unit;
		if (demo_service_open(city, sector, capacity, capital)) {
			++opens;
		}
	}
	for (const int index : demo_service_by_city_[city_index]) {
		DemoServiceBusiness& business =
			demo_service_businesses_[static_cast<std::size_t>(index)];
		if (business.bankrupt) {
			continue;
		}
		if (business.loss_days >= kDemoServiceLossDays) {
			demo_service_close(business);
		}
	}
	city.service_business_count = 0;
	for (const int index : demo_service_by_city_[city_index]) {
		if (!demo_service_businesses_[static_cast<std::size_t>(index)].bankrupt) {
			++city.service_business_count;
		}
	}
	(void)active_businesses_total;
	(void)total_units_served;
	(void)total_service_cost;
}

Dictionary EconomySim::demo_get_services_detail(const String& city_id) const {
	Dictionary out;
	const std::string id = std::string(city_id.utf8().get_data());
	const auto found = demo_city_lookup_.find(id);
	if (found == demo_city_lookup_.end()) {
		out["error"] = "cidade inexistente";
		return out;
	}
	const DemoCity& city = demo_cities_[static_cast<std::size_t>(found->second)];
	out["city"] = city_id;
	out["nation"] = String(city.nation.c_str());
	out["name"] = String(city.name.c_str());
	Array sectors;
	for (int sector = 0; sector < static_cast<int>(demo_service_sectors_.size()); ++sector) {
		const DemoServiceSector& definition =
			demo_service_sectors_[static_cast<std::size_t>(sector)];
		const DemoServiceMarket& market = city.service_markets[sector];
		Dictionary entry;
		entry["id"] = String(definition.id.c_str());
		entry["name_pt"] = String(definition.name_pt.c_str());
		entry["name_en"] = String(definition.name_en.c_str());
		entry["name_es"] = String(definition.name_es.c_str());
		entry["icon"] = String(definition.icon.c_str());
		entry["demand"] = market.demand;
		entry["supply"] = market.supply;
		entry["unmet"] = market.unmet;
		entry["price"] = market.price;
		entry["revenue"] = market.revenue;
		entry["jobs"] = market.jobs;
		entry["businesses"] = market.businesses;
		entry["exports"] = market.exports;
		entry["export_revenue"] = market.export_revenue;
		entry["coverage"] = market.coverage;
		entry["input_cost"] = market.input_cost;
		entry["service_cost"] = market.service_cost;
		Array pop_demand;
		for (int level = 0; level < kDemoMaxNeedsLevels; ++level) {
			pop_demand.append(market.pop_demand[level]);
		}
		entry["pop_demand"] = pop_demand;
		sectors.append(entry);
	}
	out["sectors"] = sectors;
	Array businesses;
	for (const int index : demo_service_by_city_[static_cast<std::size_t>(found->second)]) {
		const DemoServiceBusiness& business =
			demo_service_businesses_[static_cast<std::size_t>(index)];
		if (business.bankrupt) {
			continue;
		}
		Dictionary entry;
		entry["id"] = business.id;
		entry["sector"] = String(
			demo_service_sectors_[static_cast<std::size_t>(business.sector)].id.c_str());
		entry["owner_stratum"] = business.owner_stratum;
		entry["capacity"] = business.capacity;
		entry["jobs"] = business.jobs;
		entry["workers"] = business.workers;
		entry["capital"] = business.capital;
		entry["capital_left"] = business.capital_left;
		entry["revenue"] = business.revenue;
		entry["wages"] = business.wages;
		entry["input_cost"] = business.input_cost;
		entry["service_cost"] = business.service_cost;
		entry["profit"] = business.profit;
		entry["loss_days"] = business.loss_days;
		entry["opened_day"] = business.opened_day;
		entry["name_seed"] = static_cast<double>(
			business.name_seed % 9007199254740991ULL);
		businesses.append(entry);
	}
	out["businesses"] = businesses;
	out["private_health_cover"] = city.private_health_cover;
	out["private_education_cover"] = city.private_education_cover;
	out["service_jobs"] = city.service_jobs;
	out["service_businesses"] = city.service_business_count;
	Array class_population;
	Array income_level_share;
	for (int level = 0; level < kDemoEducationLevels; ++level) {
		class_population.append(city.pop.class_population[level]);
	}
	for (int level = 0; level < kDemoIncomeLevels; ++level) {
		income_level_share.append(city.pop.income_level_share[level]);
	}
	out["class_population"] = class_population;
	out["income_level_share"] = income_level_share;
	out["service_tonnage_prev"] = city.service_tonnage_prev;
	out["service_export_revenue"] = city.service_export_revenue;
	out["service_government_revenue"] = city.service_government_revenue;
	out["service_input_cost"] = city.service_input_cost;
	return out;
}

void EconomySim::demo_set_service_capacity(const String& city_id,
		const String& sector, double units_per_day) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	DemoCity* city = find_demo_city(std::string(city_id.utf8().get_data()));
	const int sector_index = demo_service_sector_index(
		std::string(sector.utf8().get_data()));
	if (city == nullptr || sector_index < 0) {
		return;
	}
	units_per_day = std::max(0.0, units_per_day);
	std::vector<int> local;
	for (int index = 0; index < static_cast<int>(demo_service_businesses_.size()); ++index) {
		if (demo_service_businesses_[static_cast<std::size_t>(index)].city == city->id
				&& demo_service_businesses_[static_cast<std::size_t>(index)].sector == sector_index
				&& !demo_service_businesses_[static_cast<std::size_t>(index)].bankrupt) {
			local.push_back(index);
		}
	}
	if (local.empty()) {
		if (units_per_day > 0.0) {
			const DemoServiceSector& definition =
				demo_service_sectors_[static_cast<std::size_t>(sector_index)];
			demo_service_open(*city, sector_index, units_per_day,
				units_per_day * definition.capital_per_unit);
		}
		return;
	}
	const double each = units_per_day / static_cast<double>(local.size());
	for (const int index : local) {
		DemoServiceBusiness& business =
			demo_service_businesses_[static_cast<std::size_t>(index)];
		business.capacity = each;
		business.jobs = each
			* demo_service_sectors_[static_cast<std::size_t>(sector_index)].jobs_per_unit;
		business.workers = business.jobs;
		business.loss_days = 0;
	}
}

void EconomySim::demo_set_service_business_state(int business_id, int loss_days,
		double profit) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	for (DemoServiceBusiness& business : demo_service_businesses_) {
		if (business.id != business_id) {
			continue;
		}
		business.loss_days = std::max(0, loss_days);
		business.profit = profit;
		return;
	}
}

int EconomySim::demo_get_service_business_count() const {
	int count = 0;
	for (const DemoServiceBusiness& business : demo_service_businesses_) {
		if (!business.bankrupt) {
			++count;
		}
	}
	return count;
}

int EconomySim::demo_get_service_bankruptcies() const {
	return demo_service_bankruptcies_;
}


void EconomySim::demo_update_wages(DemoCity& city, double building_margin) {
	DemoPop& pop = city.pop;
	const auto minimum = demo_minimum_wages_.find(city.nation);
	double floor = minimum != demo_minimum_wages_.end()
		? std::max(0.0, minimum->second) : 0.0;
	const double margin = std::clamp(building_margin, -0.25, 0.25);
	// 1. Parcela do trabalho no valor adicionado do dia. `city.gdp` é o valor
	// adicionado das construções (receita − insumos) + a receita de serviços,
	// já calculado nesta fase. A faixa 50–65% acompanha a renda real do país
	// (países ricos têm setor de serviços maior e participação do trabalho
	// maior); não é um multiplicador de preço.
	const double t = std::clamp(
		std::clamp(city.external_income_index, 0.0, 8.0) / kLaborIncomeSaturation,
		0.0, 1.0);
	const double labor_share = kDemoLaborShareLow
		+ (kDemoLaborShareHigh - kDemoLaborShareLow) * t;
	double filled_total = 0.0;
	for (int level = 0; level < kDemoEducationLevels; ++level) {
		filled_total += std::max(0.0, pop.filled_by_slot[level]);
	}
	// F7 calibração — PISO DE SUBSISTÊNCIA. Com a TFP como FATOR COMPLETO
	// (nível do país, não prêmio), um país de PIB PPP per capita 0,37 teria
	// alvo salarial abaixo da cesta alimentar. O piso garante que um emprego
	// pague a comida de um trabalhador e seus dependentes (população ÷ vagas
	// preenchidas) pelo alimento mais barato por caloria. Não cria dinheiro: o
	// pagamento continua limitado ao caixa da firma (pay_scale).
	if (filled_total > 1e-9 && city.simulation_population > 0.0) {
		double cheapest_food = 0.0;
		for (int slot = 0;
				slot < static_cast<int>(demo_food_products_.size()); ++slot) {
			const int product = demo_food_product(slot);
			const double factor = demo_food_calories(product);
			if (factor <= 0.0) {
				continue;
			}
			const double per_calorie = city.markets[product].price / factor;
			if (per_calorie > 0.0
					&& (cheapest_food <= 0.0 || per_calorie < cheapest_food)) {
				cheapest_food = per_calorie;
			}
		}
		if (cheapest_food > 0.0) {
			floor = std::max(floor,
				cheapest_food * kDemoNeedPerCapita
					* city.simulation_population / filled_total);
		}
	}
	const double productivity = demo_effective_productivity(city);
	const double value_added = std::max(0.0, city.gdp) * productivity;
	// TFP nacional (`derived:gdp_ppp`): o valor adicionado POR TRABALHADOR
	// usado na âncora é o do país relativo à média mundial. O pagamento
	// continua limitado ao caixa da firma (pay_scale), então a TFP muda a
	// divisão salário/lucro — não a quantidade de dinheiro em circulação.
	if (filled_total <= 1e-9 || value_added <= 1e-9) {
		// Sem emprego ou sem valor adicionado no dia: não há âncora nova. Os
		// salários ficam onde estavam (o piso continua valendo).
		for (int level = 0; level < kDemoEducationLevels; ++level) {
			pop.wage_by_level[level] = std::max(
				floor, pop.wage_by_level[level]);
		}
		return;
	}
	const double wage_bill_target = labor_share * value_added;
	const double average_target = wage_bill_target / filled_total;
	// 2. Prêmio RELATIVO por nível: razão fixa pequena × aperto do mercado do
	// nível (escassez sobe, sobra derruba) × margem das firmas. A normalização
	// por Σ(vagas × razão) mantém a folha total = parcela × VA: o aperto muda a
	// distribuição entre os níveis, não o tamanho da folha.
	double ratio[kDemoEducationLevels];
	double ratio_weighted = 0.0;
	for (int level = 0; level < kDemoEducationLevels; ++level) {
		const double supply = std::max(pop.labor_force_by_level[level], 1e-9);
		const double demand = std::max(0.0, pop.vacancies_by_level[level]);
		const double unfilled = std::max(0.0, pop.unfilled_by_level[level]);
		const double jobless = std::max(0.0, pop.unemployed_by_level[level]);
		const double scale = std::max(std::max(demand, supply), 1e-9);
		const double tightness = std::clamp(
			(unfilled - jobless) / scale, -1.0, 1.0);
		double margin_factor = 1.0;
		if (margin > 0.0 && tightness > 0.0) {
			margin_factor = 1.0 + kLaborWageMarginGain * margin;
		} else if (margin < 0.0) {
			margin_factor = std::clamp(
				1.0 + kLaborWageMarginGain * margin, 0.85, 1.0);
		}
		ratio[level] = kLaborWageRatio[level]
			* (1.0 + kLaborWageTightnessGain * tightness) * margin_factor;
		ratio[level] = std::max(ratio[level], 0.10);
		ratio_weighted += pop.filled_by_slot[level] * ratio[level];
	}
	if (ratio_weighted <= 1e-12) {
		return;
	}
	// 3. Alvo por nível e passo diário limitado pela rigidez. O primeiro ajuste
	// de uma cidade sem histórico (âncora inicial = palpite de criação do
	// mundo) entra direto: não é um salário observado que precise de inércia.
	const bool anchored = pop.wage_anchored;
	for (int level = 0; level < kDemoEducationLevels; ++level) {
		// Σ(vagas × salário) = parcela × VA: o salário do nível é o salário
		// médio e a razão normalizada redistribui a folha entre os níveis.
		const double target = std::max(floor,
			average_target * filled_total * ratio[level] / ratio_weighted);
		const double current = pop.wage_by_level[level];
		double next = target;
		if (anchored) {
			const double step = std::max(current, target) * demo_wage_rigidity_;
			next = current + std::clamp(target - current, -step, step);
		}
		pop.wage_by_level[level] = std::clamp(next, floor, 100.0);
	}
	pop.wage_anchored = true;
}

Dictionary EconomySim::demo_invest(const String& city_id, const String& building_id,
		double amount) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	Dictionary result;
	result["invested"] = 0.0;
	result["capacity_added"] = 0.0;
	result["error"] = "";

	DemoCity* city = find_demo_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr) {
		result["error"] = "cidade desconhecida";
		return result;
	}
	DemoBuilding* building = find_demo_building(city->id,
		std::string(building_id.utf8().get_data()));
	if (building == nullptr) {
		result["error"] = "construção desconhecida";
		return result;
	}
	if (amount <= 0.0) {
		result["error"] = "valor inválido";
		return result;
	}
	if (city->firm_cash < amount) {
		result["error"] = "capital insuficiente";
		return result;
	}
	if (building->input_product < 0
			&& city->suitability[building->output_product] <= 0.0) {
		result["error"] = "sem aptidão territorial";
		return result;
	}
	if (building->input_product < 0) {
		const DemoDeposit& deposit = city->deposits[building->output_product];
		if (deposit.tracked && deposit.remaining_kt <= 0.0) {
			result["error"] = "reserva esgotada";
			return result;
		}
	}

	const double capacity_added = amount / std::max(1.0, building->capital_per_capacity);
	city->firm_cash -= amount;
	// A expansão é construída por serviços domésticos: o dinheiro vira
	// receita de construção (contada no PIB do próximo tick) e não desaparece
	// em um caixa inerte.
	city->service_cash += amount;
	city->construction_revenue_pending += amount;
	building->capacity += capacity_added;
	result["invested"] = amount;
	result["capacity_added"] = capacity_added;
	demo_status_invalidate();
	return result;
}

Dictionary EconomySim::demo_build(const String& city_id, const String& building_id,
		const String& resource, int input_product, int output_product,
		double capacity, double jobs, double wage, double opex,
		double input_per_output, const String& owner, double cost,
		const String& recipe_id) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	Dictionary result;
	result["created"] = false;
	result["cost"] = 0.0;
	result["error"] = "";

	DemoCity* city = find_demo_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr) {
		result["error"] = "cidade desconhecida";
		return result;
	}
	const std::string id(building_id.utf8().get_data());
	const std::string owner_name(owner.utf8().get_data());
	if (id.empty()) {
		result["error"] = "identificador vazio";
		return result;
	}
	if (find_demo_building(city->id, id) != nullptr) {
		result["error"] = "construção já existe";
		return result;
	}
	if (output_product < 0 || output_product >= demo_products_) {
		result["error"] = "produto inválido";
		return result;
	}
	if (input_product < -1 || input_product >= demo_products_) {
		result["error"] = "insumo inválido";
		return result;
	}
	int recipe = -1;
	if (!recipe_id.is_empty()) {
		recipe = demo_recipe_index(recipe_id);
		if (recipe < 0) {
			result["error"] = "receita desconhecida";
			return result;
		}
		const DemoRecipe& chosen = demo_recipes_[static_cast<std::size_t>(recipe)];
		if (chosen.output != output_product) {
			result["error"] = "receita não produz o bem";
			return result;
		}
		// O primeiro insumo da receita alimenta os campos legados de exibição
		// (input_product/input_per_output); a fase 4 usa a receita completa.
		input_product = chosen.inputs[0];
		input_per_output = chosen.input_qty[0] / chosen.yield;
		if (jobs <= 0.0) {
			jobs = capacity * chosen.jobs_per_ton;
		}
		if (wage <= 0.0) {
			wage = chosen.wage;
		}
		if (opex <= 0.0) {
			opex = capacity * chosen.opex_per_ton;
		}
	}
	double education_mix[kDemoEducationLevels] = {0.90, 0.09, 0.01};
	if (recipe >= 0) {
		const DemoRecipe& chosen = demo_recipes_[static_cast<std::size_t>(recipe)];
		for (int level = 0; level < kDemoEducationLevels; ++level) {
			education_mix[level] = chosen.education_mix[level];
		}
	} else if (input_product >= 0) {
		// Fábrica legada (sem receita): mistura industrial padrão.
		education_mix[0] = 0.60;
		education_mix[1] = 0.30;
		education_mix[2] = 0.10;
	}
	if (capacity <= 0.0 || jobs < 0.0 || wage < 0.0 || opex < 0.0 || cost < 0.0) {
		result["error"] = "parâmetros inválidos";
		return result;
	}
	if (recipe < 0 && input_product < 0
			&& city->suitability[output_product] <= 0.0) {
		result["error"] = "sem aptidão territorial";
		return result;
	}
	if (recipe < 0 && input_product < 0) {
		const DemoDeposit& deposit = city->deposits[output_product];
		if (deposit.tracked && deposit.remaining_kt <= 0.0) {
			result["error"] = "reserva esgotada";
			return result;
		}
	}
	const bool state_owned = owner_name == "state";
	if (state_owned) {
		if (city->government_budget < cost) {
			result["error"] = "orçamento insuficiente";
			return result;
		}
		city->government_budget -= cost;
	} else {
		if (city->firm_cash < cost) {
			result["error"] = "capital insuficiente";
			return result;
		}
		city->firm_cash -= cost;
	}

	DemoBuilding building;
	building.id = id;
	building.city = city->id;
	building.kind = 0;  // tipo (0=fazenda); o produto fica em output_product
	building.resource = std::string(resource.utf8().get_data());
	building.owner = state_owned ? "state" : "private";
	building.input_product = input_product;
	building.output_product = output_product;
	building.recipe = recipe;
	building.capacity = capacity;
	building.input_per_output = input_per_output;
	for (int level = 0; level < kDemoEducationLevels; ++level) {
		building.education_mix[level] = education_mix[level];
	}
	building.capital_per_capacity = capacity > 0.0
		? std::max(1.0, cost / capacity) : kDemoCapitalPerCapacity;
	building.jobs = jobs;
	building.wage = wage;
	building.opex = opex;
	// Fábrica nova começa em rampa (25% da capacidade): evita o pico de
	// produção/oferta no primeiro dia e deixa a curva de preço reagir.
	if (recipe >= 0 || input_product >= 0) {
		building.output_scale = 0.25;
		building.last_utilization = 0.25;
	}
	demo_buildings_.push_back(building);
	// Índice por cidade: insere SÓ a construção nova. Reconstruir o índice
	// inteiro (834 cidades) a cada build custava ~0,1 ms; a IA faz ~160
	// builds/dia, o que somava ~15 ms/dia no trabalho fatiado (medido).
	// O tick já reconstrói o índice no início, após remover falidas.
	const std::size_t city_index = static_cast<std::size_t>(city - demo_cities_.data());
	if (city_index < demo_buildings_by_city_.size()) {
		demo_buildings_by_city_[city_index].push_back(
			static_cast<int>(demo_buildings_.size() - 1));
	} else {
		demo_rebuild_indexes();
	}

	// O investimento é pago ao setor de construção doméstico e entra no PIB
	// do próximo tick como receita de construção.
	city->service_cash += cost;
	city->construction_revenue_pending += cost;
	result["created"] = true;
	result["cost"] = cost;
	demo_status_invalidate();
	return result;
}

// =====================================================================
// F9 — investimento e propriedade
// =====================================================================

namespace {
// Token seguro para id determinístico (letras/números; o resto vira '-').
std::string demo_sane_token(const std::string& text) {
	std::string out;
	out.reserve(text.size());
	for (const char c : text) {
		const unsigned char u = static_cast<unsigned char>(c);
		const bool ok = (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z')
			|| (u >= '0' && u <= '9');
		out.push_back(ok ? c : '-');
	}
	while (!out.empty() && out.back() == '-') {
		out.pop_back();
	}
	return out.empty() ? std::string("X") : out;
}
}  // namespace

int EconomySim::demo_owner_kind(const std::string& owner, std::string* group_id,
		std::string* origin_nation) const {
	if (group_id != nullptr) {
		group_id->clear();
	}
	if (origin_nation != nullptr) {
		origin_nation->clear();
	}
	if (owner == "state") {
		return 0;
	}
	const std::string national_prefix = "national:";
	if (owner.rfind(national_prefix, 0) == 0) {
		if (group_id != nullptr) {
			*group_id = owner.substr(national_prefix.size());
		}
		return 1;
	}
	const std::string foreign_prefix = "foreign:";
	if (owner.rfind(foreign_prefix, 0) == 0) {
		const std::string rest = owner.substr(foreign_prefix.size());
		const std::size_t at = rest.find('@');
		if (group_id != nullptr) {
			*group_id = at == std::string::npos ? rest : rest.substr(0, at);
		}
		if (origin_nation != nullptr) {
			*origin_nation = at == std::string::npos
				? std::string() : rest.substr(at + 1);
		}
		return 2;
	}
	return 3;  // "private" e qualquer dono desconhecido
}

int EconomySim::demo_group_index(const std::string& id) const {
	const auto found = demo_group_lookup_.find(id);
	return found == demo_group_lookup_.end() ? -1 : found->second;
}

EconomySim::DemoGroup* EconomySim::find_demo_group(const std::string& id) {
	const int index = demo_group_index(id);
	return index < 0 ? nullptr : &demo_groups_[static_cast<std::size_t>(index)];
}

const EconomySim::DemoGroup* EconomySim::find_demo_group(
		const std::string& id) const {
	const int index = demo_group_index(id);
	return index < 0 ? nullptr : &demo_groups_[static_cast<std::size_t>(index)];
}

int EconomySim::demo_fdi_mode(const std::string& nation,
		const std::string& sector) const {
	const auto found = demo_fdi_lookup_.find(nation + "|" + sector);
	return found == demo_fdi_lookup_.end()
		? 0 : demo_fdi_policies_[static_cast<std::size_t>(found->second)].mode;
}

double EconomySim::demo_sector_tax_holiday(const std::string& nation,
		const std::string& sector) const {
	const auto found = demo_incentive_lookup_.find(nation + "|" + sector);
	if (found == demo_incentive_lookup_.end()) {
		return 0.0;
	}
	return std::clamp(
		demo_incentives_[static_cast<std::size_t>(found->second)].tax_holiday,
		0.0, 1.0);
}

double EconomySim::demo_sector_subsidy(const std::string& nation,
		const std::string& sector) const {
	const auto found = demo_incentive_lookup_.find(nation + "|" + sector);
	if (found == demo_incentive_lookup_.end()) {
		return 0.0;
	}
	return std::max(0.0,
		demo_incentives_[static_cast<std::size_t>(found->second)].subsidy);
}

double EconomySim::demo_nation_confidence(const std::string& nation) const {
	const auto found = demo_confidence_.find(nation);
	return found == demo_confidence_.end() ? 1.0
		: std::clamp(found->second, 0.0, 1.0);
}

void EconomySim::demo_seed_groups() {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	if (demo_groups_seeded_ || !demo_groups_.empty()) {
		return;
	}
	// Agrupa por (nação, setor do produto) — a regra documentada no
	// GAME_STATE/docs. `std::map` mantém a ordem determinística.
	std::map<std::string, std::vector<int>> buckets;
	for (std::size_t index = 0; index < demo_buildings_.size(); ++index) {
		const DemoBuilding& building = demo_buildings_[index];
		if (building.owner != "private") {
			continue;
		}
		const DemoCity* city = find_demo_city(building.city);
		if (city == nullptr) {
			continue;
		}
		std::string sector;
		if (building.output_product >= 0
				&& building.output_product < static_cast<int>(demo_product_sectors_.size())) {
			sector = demo_product_sectors_[static_cast<std::size_t>(building.output_product)];
		}
		if (sector.empty()) {
			sector = (building.recipe >= 0 || building.input_product >= 0)
				? "industry" : "primary";
		}
		buckets[city->nation + "|" + sector].push_back(static_cast<int>(index));
	}
	int chunk_serial = 0;
	for (auto& entry : buckets) {
		std::vector<int>& indices = entry.second;
		const std::size_t separator = entry.first.find('|');
		const std::string nation = entry.first.substr(0, separator);
		const std::string sector = separator == std::string::npos
			? std::string("primary") : entry.first.substr(separator + 1);
		std::sort(indices.begin(), indices.end(), [this](int a, int b) {
			return demo_buildings_[static_cast<std::size_t>(a)].id
				< demo_buildings_[static_cast<std::size_t>(b)].id;
		});
		for (std::size_t start = 0; start < indices.size();
				start += static_cast<std::size_t>(kDemoGroupChunkAssets)) {
			DemoGroup group;
			group.id = "G-" + demo_sane_token(nation) + "-"
				+ demo_sane_token(sector) + "-" + std::to_string(chunk_serial);
			group.origin_nation = nation;
			group.sector = sector;
			const int group_index = static_cast<int>(demo_groups_.size());
			demo_group_lookup_[group.id] = group_index;
			demo_groups_.push_back(group);
			const std::size_t end = std::min(indices.size(),
				start + static_cast<std::size_t>(kDemoGroupChunkAssets));
			for (std::size_t i = start; i < end; ++i) {
				DemoBuilding& building =
					demo_buildings_[static_cast<std::size_t>(indices[i])];
				building.owner = "national:" + group.id;
			}
			++chunk_serial;
		}
	}
	demo_groups_seeded_ = true;
	++demo_status_static_version_;
	demo_status_invalidate();
}

int EconomySim::demo_set_group_names(const Dictionary& names) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	int applied = 0;
	for (const Variant& key : names.keys()) {
		const std::string id(String(key).utf8().get_data());
		DemoGroup* group = find_demo_group(id);
		if (group == nullptr) {
			continue;
		}
		const String value = names[key];
		if (value.is_empty()) {
			continue;
		}
		group->name = std::string(value.utf8().get_data());
		++applied;
	}
	if (applied > 0) {
		demo_status_invalidate();
	}
	return applied;
}

Array EconomySim::demo_get_groups() const {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	struct GroupStats {
		int assets = 0;
		double capacity = 0.0;
		double employed = 0.0;
		std::set<std::string> countries;
	};
	std::vector<GroupStats> stats(demo_groups_.size());
	for (const DemoBuilding& building : demo_buildings_) {
		std::string group_id;
		std::string origin;
		const int kind = demo_owner_kind(building.owner, &group_id, &origin);
		if (kind != 1 && kind != 2) {
			continue;
		}
		const int index = demo_group_index(group_id);
		if (index < 0) {
			continue;
		}
		GroupStats& row = stats[static_cast<std::size_t>(index)];
		row.assets += 1;
		row.capacity += building.capacity * building.output_scale;
		row.employed += building.workers;
		if (const DemoCity* city = find_demo_city(building.city)) {
			row.countries.insert(city->nation);
		}
	}
	std::vector<int> order(demo_groups_.size());
	for (std::size_t index = 0; index < order.size(); ++index) {
		order[index] = static_cast<int>(index);
	}
	std::sort(order.begin(), order.end(), [this](int a, int b) {
		const DemoGroup& ga = demo_groups_[static_cast<std::size_t>(a)];
		const DemoGroup& gb = demo_groups_[static_cast<std::size_t>(b)];
		if (ga.revenue_total != gb.revenue_total) {
			return ga.revenue_total > gb.revenue_total;
		}
		if (ga.cash != gb.cash) {
			return ga.cash > gb.cash;
		}
		return ga.id < gb.id;
	});
	Array result;
	for (const int index : order) {
		const DemoGroup& group = demo_groups_[static_cast<std::size_t>(index)];
		const GroupStats& row = stats[static_cast<std::size_t>(index)];
		Dictionary entry;
		entry["id"] = String(group.id.c_str());
		entry["name"] = group.name.empty()
			? String(group.id.c_str()) : String(group.name.c_str());
		entry["origin_nation"] = String(group.origin_nation.c_str());
		entry["sector"] = String(group.sector.c_str());
		entry["state_owned"] = group.state_owned;
		entry["cash"] = group.cash;
		entry["distributable"] = group.distributable;
		entry["revenue_total"] = group.revenue_total;
		entry["profit_total"] = group.profit_total;
		entry["dividends_total"] = group.dividends_total;
		entry["taxes_total"] = group.taxes_total;
		entry["capex_total"] = group.capex_total;
		entry["assets"] = row.assets;
		entry["capacity"] = row.capacity;
		entry["employed"] = row.employed;
		entry["countries"] = static_cast<int>(row.countries.size());
		result.append(entry);
	}
	return result;
}

Array EconomySim::demo_get_group_ranking() const {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	// Ordem idêntica à de demo_get_groups (faturamento, caixa, id), sem a
	// varredura de construções: a IA precisa do caixa do dia e da vez de cada
	// grupo, não dos ativos/países. Evita 1 std::set + 1 entrada de ranking por
	// grupo a cada dia de decisão.
	std::vector<int> order(demo_groups_.size());
	for (std::size_t index = 0; index < order.size(); ++index) {
		order[index] = static_cast<int>(index);
	}
	std::sort(order.begin(), order.end(), [this](int a, int b) {
		const DemoGroup& ga = demo_groups_[static_cast<std::size_t>(a)];
		const DemoGroup& gb = demo_groups_[static_cast<std::size_t>(b)];
		if (ga.revenue_total != gb.revenue_total) {
			return ga.revenue_total > gb.revenue_total;
		}
		if (ga.cash != gb.cash) {
			return ga.cash > gb.cash;
		}
		return ga.id < gb.id;
	});
	Array result;
	result.resize(static_cast<int>(order.size()));
	int slot = 0;
	for (const int index : order) {
		const DemoGroup& group = demo_groups_[static_cast<std::size_t>(index)];
		Dictionary entry;
		entry["id"] = String(group.id.c_str());
		entry["origin_nation"] = String(group.origin_nation.c_str());
		entry["sector"] = String(group.sector.c_str());
		entry["state_owned"] = group.state_owned;
		entry["cash"] = group.cash;
		entry["distributable"] = group.distributable;
		entry["revenue_total"] = group.revenue_total;
		result[slot++] = entry;
	}
	return result;
}

Dictionary EconomySim::demo_get_group(const String& group_id) const {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	const std::string id(group_id.utf8().get_data());
	const int index = demo_group_index(id);
	Dictionary entry;
	if (index < 0) {
		entry["error"] = "grupo desconhecido";
		return entry;
	}
	const DemoGroup& group = demo_groups_[static_cast<std::size_t>(index)];
	entry["id"] = String(group.id.c_str());
	entry["name"] = group.name.empty()
		? String(group.id.c_str()) : String(group.name.c_str());
	entry["origin_nation"] = String(group.origin_nation.c_str());
	entry["sector"] = String(group.sector.c_str());
	entry["state_owned"] = group.state_owned;
	entry["cash"] = group.cash;
	entry["distributable"] = group.distributable;
	entry["revenue_total"] = group.revenue_total;
	entry["profit_total"] = group.profit_total;
	entry["dividends_total"] = group.dividends_total;
	entry["taxes_total"] = group.taxes_total;
	entry["capex_total"] = group.capex_total;
	Dictionary by_host;
	for (const auto& row : group.profit_by_host) {
		by_host[String(row.first.c_str())] = row.second;
	}
	entry["profit_by_host"] = by_host;
	return entry;
}

Dictionary EconomySim::demo_fund_group(const String& group_id, double amount) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	Dictionary result;
	result["funded"] = false;
	result["moved"] = 0.0;
	result["error"] = "";
	DemoGroup* group = find_demo_group(std::string(group_id.utf8().get_data()));
	if (group == nullptr) {
		result["error"] = "grupo desconhecido";
		return result;
	}
	if (amount <= 0.0) {
		result["error"] = "valor inválido";
		return result;
	}
	// Retira do orçamento das cidades da nação de origem, na ordem dos índices
	// (determinística). Não cria dinheiro.
	double remaining = amount;
	for (DemoCity& city : demo_cities_) {
		if (remaining <= 0.0) {
			break;
		}
		if (city.nation != group->origin_nation) {
			continue;
		}
		const double take = std::min(remaining, std::max(0.0, city.government_budget));
		if (take <= 0.0) {
			continue;
		}
		city.government_budget -= take;
		group->cash += take;
		remaining -= take;
	}
	const double moved = amount - remaining;
	result["moved"] = moved;
	result["funded"] = moved > 0.0;
	if (moved <= 0.0) {
		result["error"] = "sem orçamento na nação de origem";
	}
	demo_status_invalidate();
	return result;
}

double EconomySim::demo_credit_nation_households(const std::string& nation,
		double amount) {
	if (amount <= 0.0) {
		return 0.0;
	}
	// F9-perf: só as cidades da nação (índice demo_cities_by_nation_), não o
	// mundo. Antes eram duas varreduras de 834 cidades por grupo com dividendo
	// a pagar — a fase `fiscal` do tick pagava isso todo dia.
	const auto found = demo_cities_by_nation_.find(nation);
	if (found == demo_cities_by_nation_.end() || found->second.empty()) {
		return 0.0;
	}
	const std::vector<int>& indices = found->second;
	// O dono do capital é aproximado pelo estrato de maior escolaridade (a
	// proxy disponível no modelo para os proprietários); sem ele, população.
	double weight_total = 0.0;
	for (const int index : indices) {
		const DemoCity& city = demo_cities_[static_cast<std::size_t>(index)];
		double weight = city.pop.class_population[kDemoEducationLevels - 1];
		if (!(weight > 0.0)) {
			weight = city.simulation_population;
		}
		weight_total += weight;
	}
	if (!(weight_total > 0.0)) {
		return 0.0;
	}
	for (const int index : indices) {
		DemoCity& city = demo_cities_[static_cast<std::size_t>(index)];
		double weight = city.pop.class_population[kDemoEducationLevels - 1];
		if (!(weight > 0.0)) {
			weight = city.simulation_population;
		}
		city.capital_income_next += amount * weight / weight_total;
	}
	return amount;
}

void EconomySim::demo_route_group_profit(int group_index,
		const std::string& host_nation, double profit, double revenue,
		DemoCity& city, double& firm_surplus) {
	if (group_index < 0 || group_index >= static_cast<int>(demo_groups_.size())) {
		return;
	}
	DemoGroup& group = demo_groups_[static_cast<std::size_t>(group_index)];
	group.revenue_total += revenue;
	group.profit_total += profit;
	if (profit > 0.0) {
		if (group.state_owned) {
			// Lucro de estatal -> orçamento do anfitrião (dinheiro conservado).
			const double moved = std::min(profit, firm_surplus);
			city.firm_cash -= moved;
			city.government_budget += moved;
			firm_surplus -= moved;
			return;
		}
		// Imposto corporativo: fica no anfitrião. A isenção do incentivo
		// setorial reduz a alíquota.
		const double holiday = demo_sector_tax_holiday(host_nation, group.sector);
		const double tax_rate = std::clamp(
			kDemoCorporateTaxRate * (1.0 - holiday), 0.0, 1.0);
		const double tax = std::min(profit * tax_rate, firm_surplus);
		city.firm_cash -= tax;
		city.government_budget += tax;
		firm_surplus -= tax;
		group.taxes_total += tax;
		demo_group_flows_[host_nation].taxes += tax;
		const double net = profit - tax;
		const double moved = std::min(net, firm_surplus);
		city.firm_cash -= moved;
		group.cash += moved;
		group.distributable += moved;
		firm_surplus -= moved;
		group.profit_by_host[host_nation] += moved;
		// Incentivo: subsídio ad valorem sobre a receita do ativo, pago pelo
		// governo do anfitrião (política explícita; 0 por padrão).
		const double subsidy_rate = demo_sector_subsidy(host_nation, group.sector);
		if (subsidy_rate > 0.0 && revenue > 0.0) {
			const double subsidy = std::min(
				std::max(0.0, city.government_budget), revenue * subsidy_rate);
			if (subsidy > 0.0) {
				city.government_budget -= subsidy;
				group.cash += subsidy;
				group.distributable += subsidy;
				demo_group_flows_[host_nation].subsidy += subsidy;
			}
		}
	} else if (profit < 0.0) {
		// O grupo recapitaliza o ativo até o limite do caixa; o resto da perda
		// continua na firma (pode levar a falência, como antes).
		const double inject = std::min(-profit, group.cash);
		if (inject > 0.0) {
			group.cash -= inject;
			city.firm_cash += inject;
		}
	}
}

void EconomySim::demo_settle_groups() {
	if (demo_groups_.empty()) {
		return;
	}
	// Recuperação lenta da confiança depois do prazo da nacionalização.
	for (auto it = demo_confidence_penalty_day_.begin();
			it != demo_confidence_penalty_day_.end();) {
		if (day_ >= it->second) {
			auto confidence = demo_confidence_.find(it->first);
			if (confidence != demo_confidence_.end()) {
				confidence->second = std::min(1.0, confidence->second + 0.005);
			}
			if (confidence == demo_confidence_.end()
					|| confidence->second >= 1.0) {
				it = demo_confidence_penalty_day_.erase(it);
				continue;
			}
		}
		++it;
	}
	for (std::size_t index = 0; index < demo_groups_.size(); ++index) {
		DemoGroup& group = demo_groups_[index];
		if (group.state_owned || group.distributable <= 1e-9) {
			continue;
		}
		// Dividendo ao estrato dono: fração do lucro acumulado. O que sobra
		// fica como capital para o próximo investimento.
		double payable = group.distributable * kDemoGroupDividendShare;
		payable = std::min(payable, group.cash);
		if (payable <= 1e-9) {
			continue;
		}
		double host_total = 0.0;
		for (const auto& row : group.profit_by_host) {
			host_total += std::max(0.0, row.second);
		}
		if (host_total <= 1e-9) {
			const double credited = demo_credit_nation_households(
				group.origin_nation, payable);
			if (credited > 0.0) {
				group.cash -= credited;
				group.distributable = std::max(0.0, group.distributable - credited);
				group.dividends_total += credited;
				demo_group_flows_[group.origin_nation].dividends += credited;
			}
			continue;
		}
		double credited_total = 0.0;
		std::vector<std::string> exhausted;
		for (auto& row : group.profit_by_host) {
			const double share = payable * std::max(0.0, row.second) / host_total;
			if (share <= 1e-9) {
				continue;
			}
			const bool domestic = row.first == group.origin_nation;
			const double credited = demo_credit_nation_households(
				group.origin_nation, share);
			if (credited <= 0.0) {
				continue;
			}
			credited_total += credited;
			if (domestic) {
				demo_group_flows_[row.first].dividends += credited;
			} else {
				// Repatriação: sai do anfitrião, entra na origem.
				demo_group_flows_[row.first].repatriated_out += credited;
				demo_group_flows_[group.origin_nation].repatriated_in += credited;
			}
			row.second -= share;
			if (row.second <= 1e-9) {
				exhausted.push_back(row.first);
			}
		}
		for (const std::string& host : exhausted) {
			group.profit_by_host.erase(host);
		}
		if (credited_total > 0.0) {
			group.cash -= credited_total;
			group.distributable = std::max(0.0,
				group.distributable - credited_total);
			group.dividends_total += credited_total;
		}
	}
}

Dictionary EconomySim::demo_group_invest(const String& group_id,
		const String& city_id, const String& building_id, const String& resource,
		int output_product, double capacity, const String& recipe_id,
		const String& partner_group) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	Dictionary result;
	result["created"] = false;
	result["cost"] = 0.0;
	result["error"] = "";
	const std::string gid(group_id.utf8().get_data());
	DemoGroup* group = find_demo_group(gid);
	if (group == nullptr) {
		result["error"] = "grupo desconhecido";
		return result;
	}
	DemoCity* city = find_demo_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr) {
		result["error"] = "cidade desconhecida";
		return result;
	}
	const std::string id(building_id.utf8().get_data());
	if (id.empty()) {
		result["error"] = "identificador vazio";
		return result;
	}
	if (find_demo_building(city->id, id) != nullptr) {
		result["error"] = "construção já existe";
		return result;
	}
	if (output_product < 0 || output_product >= demo_products_) {
		result["error"] = "produto inválido";
		return result;
	}
	if (capacity <= 0.0) {
		result["error"] = "capacidade inválida";
		return result;
	}
	const std::string sector = output_product < static_cast<int>(demo_product_sectors_.size())
		? demo_product_sectors_[static_cast<std::size_t>(output_product)]
		: std::string();
	int recipe = -1;
	double capital_per_capacity = kDemoCapitalPerCapacity;
	double jobs = 0.0;
	double wage = 0.0;
	double opex = 0.0;
	double education_mix[kDemoEducationLevels] = {0.90, 0.09, 0.01};
	if (!recipe_id.is_empty()) {
		recipe = demo_recipe_index(recipe_id);
		if (recipe < 0) {
			result["error"] = "receita desconhecida";
			return result;
		}
		const DemoRecipe& chosen = demo_recipes_[static_cast<std::size_t>(recipe)];
		if (chosen.output != output_product) {
			result["error"] = "receita não produz o bem";
			return result;
		}
		capital_per_capacity = std::max(1.0, chosen.capital_per_capacity);
		jobs = capacity * chosen.jobs_per_ton;
		wage = chosen.wage;
		opex = capacity * chosen.opex_per_ton;
		for (int level = 0; level < kDemoEducationLevels; ++level) {
			education_mix[level] = chosen.education_mix[level];
		}
	} else if (city->suitability[output_product] <= 0.0) {
		result["error"] = "sem aptidão territorial";
		return result;
	} else {
		// Produção primária por grupo: mesmos coeficientes da fazenda inicial
		// (estimativa declarada; o núcleo não tem tabela por produto). Sem
		// folha/opex o lucro ficaria inflado e o emprego zerado.
		jobs = capacity * kDemoJobsPerTon;
		wage = kDemoFarmWage;
		opex = capacity * kDemoOpexPerTon;
	}
	const bool foreign = group->origin_nation != city->nation;
	double local_share = 0.0;
	std::string partner_id;
	DemoGroup* partner = nullptr;
	if (foreign) {
		const int mode = demo_fdi_mode(city->nation, sector);
		if (mode == 3) {
			result["error"] = "setor fechado ao capital estrangeiro";
			return result;
		}
		if (demo_nation_confidence(city->nation) < 0.5) {
			result["error"] = "confiança baixa após nacionalização";
			return result;
		}
		if (mode == 2) {
			partner_id = std::string(partner_group.utf8().get_data());
			if (partner_id.empty()) {
				result["error"] = "joint venture exige parceiro local";
				return result;
			}
			partner = find_demo_group(partner_id);
			if (partner == nullptr || partner->origin_nation != city->nation
					|| partner->state_owned) {
				result["error"] = "parceiro local inválido";
				return result;
			}
			const auto policy = demo_fdi_lookup_.find(city->nation + "|" + sector);
			local_share = policy == demo_fdi_lookup_.end()
				? 0.30 : std::clamp(
					demo_fdi_policies_[static_cast<std::size_t>(policy->second)].min_local,
					0.0, 1.0);
			if (local_share <= 0.0) {
				local_share = 0.30;
			}
		} else if (mode == 1) {
			// Restrito: só quem já opera no setor do anfitrião.
			bool incumbent = false;
			for (const DemoBuilding& building : demo_buildings_) {
				if (building.owner.rfind("foreign:", 0) != 0
						|| building.output_product != output_product) {
					continue;
				}
				std::string owner_group;
				std::string origin;
				demo_owner_kind(building.owner, &owner_group, &origin);
				const DemoCity* host = find_demo_city(building.city);
				if (host != nullptr && host->nation == city->nation
						&& owner_group == gid) {
					incumbent = true;
					break;
				}
			}
			if (!incumbent) {
				result["error"] = "setor restrito a incumbentes";
				return result;
			}
		}
		if (recipe < 0) {
			const DemoDeposit& deposit = city->deposits[output_product];
			if (deposit.tracked && deposit.remaining_kt <= 0.0) {
				result["error"] = "reserva esgotada";
				return result;
			}
		}
	}
	const double cost = capacity * capital_per_capacity;
	const double partner_cost = cost * local_share;
	const double owner_cost = cost - partner_cost;
	if (group->cash + 1e-9 < owner_cost) {
		result["error"] = "capital insuficiente";
		result["cash"] = group->cash;
		result["cost"] = cost;
		return result;
	}
	if (partner != nullptr && partner->cash + 1e-9 < partner_cost) {
		result["error"] = "capital insuficiente do parceiro local";
		result["cost"] = cost;
		return result;
	}
	group->cash -= owner_cost;
	group->capex_total += owner_cost;
	if (partner != nullptr && partner_cost > 0.0) {
		partner->cash -= partner_cost;
		partner->capex_total += partner_cost;
	}

	DemoBuilding building;
	building.id = id;
	building.city = city->id;
	building.kind = 0;
	building.resource = std::string(resource.utf8().get_data());
	building.owner = foreign
		? "foreign:" + gid + "@" + group->origin_nation
		: "national:" + gid;
	building.input_product = recipe >= 0
		? demo_recipes_[static_cast<std::size_t>(recipe)].inputs[0] : -1;
	building.output_product = output_product;
	building.recipe = recipe;
	building.capacity = capacity;
	building.input_per_output = recipe >= 0
		? demo_recipes_[static_cast<std::size_t>(recipe)].input_qty[0]
			/ std::max(1e-9,
				demo_recipes_[static_cast<std::size_t>(recipe)].yield)
		: 0.0;
	building.capital_per_capacity = capital_per_capacity;
	building.jobs = jobs;
	building.wage = wage;
	building.opex = opex;
	for (int level = 0; level < kDemoEducationLevels; ++level) {
		building.education_mix[level] = education_mix[level];
	}
	building.local_share = local_share;
	building.partner_group = partner != nullptr ? partner->id : std::string();
	if (recipe >= 0) {
		building.output_scale = 0.25;
		building.last_utilization = 0.25;
	}
	demo_buildings_.push_back(building);
	const std::size_t city_index =
		static_cast<std::size_t>(city - demo_cities_.data());
	if (city_index < demo_buildings_by_city_.size()) {
		demo_buildings_by_city_[city_index].push_back(
			static_cast<int>(demo_buildings_.size() - 1));
	} else {
		demo_rebuild_indexes();
	}
	city->service_cash += cost;
	city->construction_revenue_pending += cost;
	result["created"] = true;
	result["cost"] = cost;
	result["owner"] = String(building.owner.c_str());
	result["local_share"] = local_share;
	demo_status_invalidate();
	return result;
}

Dictionary EconomySim::demo_set_fdi_policy(const String& nation,
		const String& sector, const String& mode, double min_local) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	Dictionary result;
	result["ok"] = false;
	result["error"] = "";
	const std::string tag(nation.utf8().get_data());
	const std::string sector_id(sector.utf8().get_data());
	const String mode_name = mode.to_lower();
	int mode_value = -1;
	if (mode_name == "open" || mode_name == "aberto") {
		mode_value = 0;
	} else if (mode_name == "restricted" || mode_name == "restrito") {
		mode_value = 1;
	} else if (mode_name == "joint_venture" || mode_name == "joint") {
		mode_value = 2;
	} else if (mode_name == "closed" || mode_name == "fechado") {
		mode_value = 3;
	}
	if (mode_value < 0) {
		result["error"] = "modo inválido";
		return result;
	}
	const std::string key = tag + "|" + sector_id;
	const auto found = demo_fdi_lookup_.find(key);
	DemoFdiPolicy policy;
	policy.nation = tag;
	policy.sector = sector_id;
	policy.mode = mode_value;
	policy.min_local = std::clamp(min_local, 0.0, 1.0);
	if (found == demo_fdi_lookup_.end()) {
		demo_fdi_lookup_[key] = static_cast<int>(demo_fdi_policies_.size());
		demo_fdi_policies_.push_back(policy);
	} else {
		demo_fdi_policies_[static_cast<std::size_t>(found->second)] = policy;
	}
	result["ok"] = true;
	result["nation"] = String(tag.c_str());
	result["sector"] = String(sector_id.c_str());
	result["mode"] = mode_name;
	result["min_local"] = policy.min_local;
	return result;
}

Dictionary EconomySim::demo_set_sector_incentive(const String& nation,
		const String& sector, double tax_holiday, double subsidy) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	Dictionary result;
	result["ok"] = true;
	const std::string tag(nation.utf8().get_data());
	const std::string sector_id(sector.utf8().get_data());
	const std::string key = tag + "|" + sector_id;
	DemoSectorIncentive incentive;
	incentive.nation = tag;
	incentive.sector = sector_id;
	incentive.tax_holiday = std::clamp(tax_holiday, 0.0, 1.0);
	incentive.subsidy = std::max(0.0, subsidy);
	const auto found = demo_incentive_lookup_.find(key);
	if (found == demo_incentive_lookup_.end()) {
		demo_incentive_lookup_[key] = static_cast<int>(demo_incentives_.size());
		demo_incentives_.push_back(incentive);
	} else {
		demo_incentives_[static_cast<std::size_t>(found->second)] = incentive;
	}
	result["nation"] = String(tag.c_str());
	result["sector"] = String(sector_id.c_str());
	result["tax_holiday"] = incentive.tax_holiday;
	result["subsidy"] = incentive.subsidy;
	return result;
}

Dictionary EconomySim::demo_create_state_company(const String& nation,
		const String& name, double capital) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	Dictionary result;
	result["created"] = false;
	result["error"] = "";
	const std::string tag(nation.utf8().get_data());
	if (tag.empty()) {
		result["error"] = "nação vazia";
		return result;
	}
	DemoGroup group;
	group.state_owned = true;
	group.origin_nation = tag;
	group.sector = "state";
	const String company_name = name.strip_edges();
	group.name = company_name.is_empty()
		? std::string() : std::string(company_name.utf8().get_data());
	int serial = 0;
	do {
		group.id = "S-" + demo_sane_token(tag) + "-" + std::to_string(serial);
		++serial;
	} while (demo_group_lookup_.find(group.id) != demo_group_lookup_.end());
	demo_group_lookup_[group.id] = static_cast<int>(demo_groups_.size());
	demo_groups_.push_back(group);
	DemoGroup* created = &demo_groups_.back();
	double remaining = std::max(0.0, capital);
	for (DemoCity& city : demo_cities_) {
		if (remaining <= 0.0) {
			break;
		}
		if (city.nation != tag) {
			continue;
		}
		const double take = std::min(remaining, std::max(0.0, city.government_budget));
		city.government_budget -= take;
		created->cash += take;
		remaining -= take;
	}
	result["created"] = true;
	result["id"] = String(created->id.c_str());
	result["capital"] = created->cash;
	if (created->cash <= 0.0) {
		result["warning"] = "nenhum orçamento disponível na nação";
	}
	demo_status_invalidate();
	return result;
}

Dictionary EconomySim::demo_privatize(const String& city_id,
		const String& building_id, const String& buyer_group) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	Dictionary result;
	result["ok"] = false;
	result["error"] = "";
	DemoCity* city = find_demo_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr) {
		result["error"] = "cidade desconhecida";
		return result;
	}
	DemoBuilding* building = find_demo_building(city->id,
		std::string(building_id.utf8().get_data()));
	if (building == nullptr) {
		result["error"] = "ativo desconhecido";
		return result;
	}
	if (building->owner != "state") {
		result["error"] = "ativo não é estatal";
		return result;
	}
	DemoGroup* buyer = find_demo_group(std::string(buyer_group.utf8().get_data()));
	if (buyer == nullptr) {
		result["error"] = "comprador desconhecido";
		return result;
	}
	const bool foreign = buyer->origin_nation != city->nation;
	if (foreign) {
		const std::string sector = building->output_product >= 0
			&& building->output_product < static_cast<int>(demo_product_sectors_.size())
			? demo_product_sectors_[static_cast<std::size_t>(building->output_product)]
			: std::string();
		if (demo_fdi_mode(city->nation, sector) == 3) {
			result["error"] = "setor fechado ao capital estrangeiro";
			return result;
		}
	}
	const double price = building->capacity * building->capital_per_capacity;
	if (buyer->cash + 1e-9 < price) {
		result["error"] = "caixa insuficiente do comprador";
		return result;
	}
	buyer->cash -= price;
	buyer->capex_total += price;
	city->government_budget += price;
	building->owner = foreign
		? "foreign:" + buyer->id + "@" + buyer->origin_nation
		: "national:" + buyer->id;
	building->local_share = 0.0;
	building->partner_group.clear();
	result["ok"] = true;
	result["price"] = price;
	result["owner"] = String(building->owner.c_str());
	demo_status_invalidate();
	return result;
}

Dictionary EconomySim::demo_nationalize(const String& city_id,
		const String& building_id) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	Dictionary result;
	result["ok"] = false;
	result["error"] = "";
	DemoCity* city = find_demo_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr) {
		result["error"] = "cidade desconhecida";
		return result;
	}
	DemoBuilding* building = find_demo_building(city->id,
		std::string(building_id.utf8().get_data()));
	if (building == nullptr) {
		result["error"] = "ativo desconhecido";
		return result;
	}
	std::string group_id;
	std::string origin;
	const int kind = demo_owner_kind(building->owner, &group_id, &origin);
	if (kind == 0) {
		result["error"] = "ativo já é estatal";
		return result;
	}
	const double book = building->capacity * building->capital_per_capacity;
	const std::string previous_owner = building->owner;
	const double compensation = std::min(book * kDemoNationalizationCompensation,
		std::max(0.0, city->government_budget));
	if (compensation > 0.0) {
		city->government_budget -= compensation;
		if (DemoGroup* owner = find_demo_group(group_id)) {
			owner->cash += compensation;
		} else {
			city->firm_cash += compensation;
		}
	}
	building->owner = "state";
	building->local_share = 0.0;
	building->partner_group.clear();
	// Derruba a confiança do anfitrião por um período: o IDE novo fica
	// bloqueado enquanto a confiança estiver abaixo de 0,5.
	demo_confidence_[city->nation] = kDemoNationalizationConfidence;
	demo_confidence_penalty_day_[city->nation] = day_ + kDemoNationalizationDays;
	result["ok"] = true;
	result["compensation"] = compensation;
	result["confidence"] = demo_nation_confidence(city->nation);
	result["previous_owner"] = String(previous_owner.c_str());
	demo_status_invalidate();
	return result;
}

Dictionary EconomySim::demo_get_investment_status() const {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	Dictionary out;
	out["groups"] = demo_get_groups();
	struct NationOwnership {
		double production[3] = {};
		double employment[3] = {};
		double capacity[3] = {};
		int assets[3] = {};
	};
	std::map<std::string, NationOwnership> nations;
	for (const DemoBuilding& building : demo_buildings_) {
		const DemoCity* city = find_demo_city(building.city);
		if (city == nullptr) {
			continue;
		}
		std::string group_id;
		std::string origin;
		const int kind = demo_owner_kind(building.owner, &group_id, &origin);
		const int slot = kind == 0 ? 0 : (kind == 2 ? 2 : 1);
		NationOwnership& row = nations[city->nation];
		row.production[slot] += building.revenue;
		row.employment[slot] += building.workers;
		row.capacity[slot] += building.capacity * building.output_scale;
		row.assets[slot] += 1;
	}
	Dictionary ownership;
	for (const auto& entry : nations) {
		const NationOwnership& row = entry.second;
		double production_total = 0.0;
		double employment_total = 0.0;
		double capacity_total = 0.0;
		int assets_total = 0;
		for (int slot = 0; slot < 3; ++slot) {
			production_total += row.production[slot];
			employment_total += row.employment[slot];
			capacity_total += row.capacity[slot];
			assets_total += row.assets[slot];
		}
		Dictionary record;
		Array production;
		Array employment;
		Array capacity;
		Array assets;
		for (int slot = 0; slot < 3; ++slot) {
			production.append(production_total > 0.0
				? row.production[slot] / production_total : 0.0);
			employment.append(employment_total > 0.0
				? row.employment[slot] / employment_total : 0.0);
			capacity.append(capacity_total > 0.0
				? row.capacity[slot] / capacity_total : 0.0);
			assets.append(row.assets[slot]);
		}
		record["production_share"] = production;   // [state, national, foreign]
		record["employment_share"] = employment;
		record["capacity_share"] = capacity;
		record["assets"] = assets;
		record["production_total"] = production_total;
		record["employment_total"] = employment_total;
		record["assets_total"] = assets_total;
		record["confidence"] = demo_nation_confidence(entry.first);
		ownership[String(entry.first.c_str())] = record;
	}
	out["nations"] = ownership;

	Array flows;
	std::set<std::string> flow_nations;
	for (const auto& entry : demo_group_flows_) {
		flow_nations.insert(entry.first);
	}
	for (const std::string& nation : flow_nations) {
		const DemoGroupFlow flow = demo_group_flows_.at(nation);
		Dictionary record;
		record["nation"] = String(nation.c_str());
		record["repatriated_out"] = flow.repatriated_out;
		record["repatriated_in"] = flow.repatriated_in;
		record["dividends"] = flow.dividends;
		record["taxes"] = flow.taxes;
		record["subsidy"] = flow.subsidy;
		flows.append(record);
	}
	out["flows"] = flows;

	Array policies;
	for (const DemoFdiPolicy& policy : demo_fdi_policies_) {
		Dictionary record;
		record["nation"] = String(policy.nation.c_str());
		record["sector"] = String(policy.sector.c_str());
		record["mode"] = policy.mode;
		record["min_local"] = policy.min_local;
		policies.append(record);
	}
	out["fdi_policies"] = policies;
	Array incentives;
	for (const DemoSectorIncentive& incentive : demo_incentives_) {
		Dictionary record;
		record["nation"] = String(incentive.nation.c_str());
		record["sector"] = String(incentive.sector.c_str());
		record["tax_holiday"] = incentive.tax_holiday;
		record["subsidy"] = incentive.subsidy;
		incentives.append(record);
	}
	out["sector_incentives"] = incentives;
	out["group_count"] = static_cast<int>(demo_groups_.size());
	return out;
}

Dictionary EconomySim::demo_get_investment_signals() const {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	Dictionary out;
	// Confiança: só as nações com penalidade registrada; as demais valem 1,0
	// (o consumidor usa o default). Sem varredura de construções.
	Dictionary nations;
	for (const auto& row : demo_confidence_) {
		Dictionary entry;
		entry["confidence"] = row.second;
		nations[String(row.first.c_str())] = entry;
	}
	out["nations"] = nations;
	Array policies;
	policies.resize(static_cast<int>(demo_fdi_policies_.size()));
	int slot = 0;
	for (const DemoFdiPolicy& policy : demo_fdi_policies_) {
		Dictionary record;
		record["nation"] = String(policy.nation.c_str());
		record["sector"] = String(policy.sector.c_str());
		record["mode"] = policy.mode;
		record["min_local"] = policy.min_local;
		policies[slot++] = record;
	}
	out["fdi_policies"] = policies;
	Array incentives;
	incentives.resize(static_cast<int>(demo_incentives_.size()));
	slot = 0;
	for (const DemoSectorIncentive& incentive : demo_incentives_) {
		Dictionary record;
		record["nation"] = String(incentive.nation.c_str());
		record["sector"] = String(incentive.sector.c_str());
		record["tax_holiday"] = incentive.tax_holiday;
		record["subsidy"] = incentive.subsidy;
		incentives[slot++] = record;
	}
	out["sector_incentives"] = incentives;
	return out;
}

bool EconomySim::demo_spend_government(const String& city_id, double amount) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	DemoCity* city = find_demo_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr || amount <= 0.0 || city->government_budget < amount) {
		return false;
	}
	city->government_budget -= amount;
	// Serviço doméstico (prospecção, estudos, licenciamento).
	city->service_cash += amount;
	city->service_revenue += amount;
	city->service_profit += amount;
	city->gdp += amount;
	demo_status_invalidate();
	return true;
}

Dictionary EconomySim::demo_to_dict() const {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	Dictionary data;
	data["version"] = kDemoSaveVersion;
	data["active"] = demo_active_;
	data["world_mode"] = demo_world_mode_;
	data["day"] = day_;
	data["transport_cash"] = demo_transport_cash_;
	data["foreign_cash"] = demo_foreign_cash_;
	data["construction_cash"] = demo_construction_cash_;

	// Lista canônica de produtos (id na posição do índice). O contrato é por
	// `order`; os 21 primeiros são os mesmos do save v3.
	Array product_names;
	for (int index = 0; index < demo_products_; ++index) {
		product_names.append(String(demo_product_name(index)));
	}
	data["products"] = product_names;

	Array produced;
	Array consumed;
	for (int index = 0; index < demo_products_; ++index) {
		produced.append(demo_produced_[index]);
		consumed.append(demo_consumed_total_[index]);
	}
	data["produced_total"] = produced;
	data["consumed_total"] = consumed;

	Array cities;
	for (const DemoCity& city : demo_cities_) {
		Dictionary entry;
		entry["id"] = String(city.id.c_str());
		entry["name"] = String(city.name.c_str());
		entry["nation"] = String(city.nation.c_str());
		entry["home_nation"] = String(city.home_nation.c_str());
		entry["landmass"] = String(city.landmass.c_str());
		Array center;
		center.append(city.center.x);
		center.append(city.center.y);
		entry["center"] = center;
		entry["population"] = city.population;
		entry["sim_population"] = city.simulation_population;
		entry["firm_cash"] = city.firm_cash;
		entry["service_cash"] = city.service_cash;
		entry["government_budget"] = city.government_budget;
		entry["service_jobs"] = city.service_jobs;
		entry["service_wage"] = city.service_wage;
		entry["service_wage_bill"] = city.service_wage_bill;
		entry["service_revenue"] = city.service_revenue;
		entry["service_profit"] = city.service_profit;
		entry["opex_revenue"] = city.opex_revenue;
		entry["freight_revenue"] = city.freight_revenue;
		entry["construction_revenue"] = city.construction_revenue;
		entry["construction_revenue_pending"] = city.construction_revenue_pending;
		entry["capital_income_next"] = city.capital_income_next;
		entry["gdp"] = city.gdp;
		entry["external_income_index"] = city.external_income_index;
		// TFP: 0 = derivada do índice de renda (não duplica o dado no save).
		entry["productivity_index"] = city.productivity_index;
		// Gini: 0 = não declarado (legado por escolaridade).
		entry["income_gini"] = city.income_gini;
		// F8: serviços (save v10). Mercados dos 20 setores em arrays de 12
		// campos (ordem fixa) + fluxos/caixa do dia e cobertura privada.
		entry["service_export_revenue"] = city.service_export_revenue;
		entry["service_government_revenue"] = city.service_government_revenue;
		entry["service_input_cost"] = city.service_input_cost;
		entry["service_business_profit"] = city.service_business_profit;
		entry["service_business_count"] = city.service_business_count;
		entry["private_health_cover"] = city.private_health_cover;
		entry["private_education_cover"] = city.private_education_cover;
		entry["service_tonnage_prev"] = city.service_tonnage_prev;
		Array service_markets;
		for (int sector = 0; sector < kDemoMaxServiceSectors; ++sector) {
			const DemoServiceMarket& market = city.service_markets[sector];
			Array fields;
			fields.append(market.demand);
			fields.append(market.supply);
			fields.append(market.unmet);
			fields.append(market.price);
			fields.append(market.base_price);
			fields.append(market.revenue);
			fields.append(market.input_cost);
			fields.append(market.service_cost);
			fields.append(market.jobs);
			fields.append(market.businesses);
			fields.append(market.exports);
			fields.append(market.export_revenue);
			fields.append(market.coverage);
			service_markets.append(fields);
		}
		entry["services"] = service_markets;

		Array suitability;
		Array revenue;
		Array imported;
		Array exported;
		Array import_value;
		Array export_value;
		Array external_demand;
		Array import_price;
		for (int product = 0; product < demo_products_; ++product) {
			suitability.append(city.suitability[product]);
			revenue.append(city.revenue_by_product[product]);
			imported.append(city.imported[product]);
			exported.append(city.exported[product]);
			import_value.append(city.import_value[product]);
			export_value.append(city.export_value[product]);
			external_demand.append(city.external_demand[product]);
			import_price.append(city.import_price[product]);
		}
		entry["suitability"] = suitability;
		entry["revenue_by_product"] = revenue;
		entry["imported"] = imported;
		entry["exported"] = exported;
		entry["import_value"] = import_value;
		entry["export_value"] = export_value;
		entry["external_demand"] = external_demand;
		entry["import_price"] = import_price;

		// Reservas minerais (só jazidas declaradas): o saldo restante persiste.
		Dictionary deposits;
		for (int product = 0; product < demo_products_; ++product) {
			const DemoDeposit& deposit = city.deposits[product];
			if (!deposit.tracked) {
				continue;
			}
			Dictionary deposit_entry;
			deposit_entry["initial_kt"] = deposit.initial_kt;
			deposit_entry["remaining_kt"] = deposit.remaining_kt;
			deposit_entry["extracted_kt"] = deposit.extracted_kt;
			deposit_entry["quality"] = deposit.quality;
			deposit_entry["depth_m"] = deposit.depth_m;
			deposits[String(demo_product_name(product))] = deposit_entry;
		}
		entry["deposits"] = deposits;

		Dictionary pop;
		pop["population"] = city.pop.population;
		pop["labor_force"] = city.pop.labor_force;
		pop["employed"] = city.pop.employed;
		pop["unemployed"] = city.pop.unemployed;
		pop["dependents"] = city.pop.dependents;
		pop["wage_income"] = city.pop.wage_income;
		pop["capital_income"] = city.pop.capital_income;
		pop["transfers"] = city.pop.transfers;
		pop["taxes"] = city.pop.taxes;
		pop["service_spend"] = city.pop.service_spend;
		pop["disposable"] = city.pop.disposable;
		pop["external_income"] = city.pop.external_income;
		pop["food_spend"] = city.pop.food_spend;
		pop["consumed"] = city.pop.consumed;
		pop["need"] = city.pop.need;
		pop["satisfaction"] = city.pop.satisfaction;
		pop["savings"] = city.pop.savings;
		Array desired_by_category;
		Array consumed_by_category;
		for (int product = 0; product < static_cast<int>(demo_food_products_.size()); ++product) {
			desired_by_category.append(city.pop.desired_by_category[product]);
			consumed_by_category.append(city.pop.consumed_by_category[product]);
		}
		pop["desired_by_category"] = desired_by_category;
		pop["consumed_by_category"] = consumed_by_category;
		// F3: estado das necessidades (níveis, duráveis e gasto próprio). Campos
		// aditivos: save v4 anterior continua carregando (default zero).
		Dictionary needs_state;
		needs_state["level"] = city.pop.needs_level;
		needs_state["satisfaction"] = city.pop.needs_satisfaction;
		needs_state["spend"] = city.pop.needs_spend;
		needs_state["durable_spend"] = city.pop.durable_spend;
		Array level_satisfaction;
		Array level_want;
		Array level_consumed;
		for (int level = 0; level < kDemoMaxNeedsLevels; ++level) {
			level_satisfaction.append(level < static_cast<int>(
				city.pop.level_satisfaction.size()) ? city.pop.level_satisfaction[level] : 0.0);
			level_want.append(level < static_cast<int>(
				city.pop.level_want.size()) ? city.pop.level_want[level] : 0.0);
			level_consumed.append(level < static_cast<int>(
				city.pop.level_consumed.size()) ? city.pop.level_consumed[level] : 0.0);
		}
		needs_state["level_satisfaction"] = level_satisfaction;
		needs_state["level_want"] = level_want;
		needs_state["level_consumed"] = level_consumed;
		Dictionary durable_stock;
		for (int product = 0; product < demo_products_
				&& product < static_cast<int>(city.pop.durable_stock.size()); ++product) {
			if (city.pop.durable_stock[static_cast<std::size_t>(product)] > 0.0) {
				durable_stock[String(demo_product_name(product))] =
					city.pop.durable_stock[static_cast<std::size_t>(product)];
			}
		}
		needs_state["durable_stock"] = durable_stock;
		pop["needs"] = needs_state;
		// F7: mercado de trabalho e educação (save v9). Bloco aditivo: os campos
		// de compatibilidade acima continuam iguais.
		Dictionary labor;
		labor["working_age"] = city.pop.working_age;
		labor["retirees"] = city.pop.retirees;
		labor["students"] = city.pop.students;
		labor["recruit_reserve"] = city.pop.recruit_reserve;
		labor["education"] = Array{city.pop.education[0],
			city.pop.education[1], city.pop.education[2]};
		labor["participation"] = Array{city.pop.participation[0],
			city.pop.participation[1], city.pop.participation[2]};
		labor["labor_force_by_level"] = Array{
			city.pop.labor_force_by_level[0],
			city.pop.labor_force_by_level[1],
			city.pop.labor_force_by_level[2]};
		labor["employed_by_level"] = Array{city.pop.employed_by_level[0],
			city.pop.employed_by_level[1], city.pop.employed_by_level[2]};
		labor["unemployed_by_level"] = Array{
			city.pop.unemployed_by_level[0],
			city.pop.unemployed_by_level[1],
			city.pop.unemployed_by_level[2]};
		labor["vacancies_by_level"] = Array{
			city.pop.vacancies_by_level[0],
			city.pop.vacancies_by_level[1],
			city.pop.vacancies_by_level[2]};
		labor["unfilled_by_level"] = Array{
			city.pop.unfilled_by_level[0],
			city.pop.unfilled_by_level[1],
			city.pop.unfilled_by_level[2]};
		labor["wage_by_level"] = Array{city.pop.wage_by_level[0],
			city.pop.wage_by_level[1], city.pop.wage_by_level[2]};
		labor["wage_income_by_level"] = Array{
			city.pop.wage_income_by_level[0],
			city.pop.wage_income_by_level[1],
			city.pop.wage_income_by_level[2]};
		labor["studying"] = Array{city.pop.studying[0], city.pop.studying[1]};
		labor["income_level"] = city.pop.income_level;
		labor["wage_anchored"] = city.pop.wage_anchored;
		labor["income_level_share"] = Array{city.pop.income_level_share[0],
			city.pop.income_level_share[1], city.pop.income_level_share[2],
			city.pop.income_level_share[3]};
		labor["income_level_by_stratum"] = Array{
			city.pop.stratum_income_level[0],
			city.pop.stratum_income_level[1],
			city.pop.stratum_income_level[2]};
		pop["labor"] = labor;
		entry["pop"] = pop;
		entry["government_goods_spend"] = city.government_goods_spend;

		Array markets;
		for (int product = 0; product < demo_products_; ++product) {
			Dictionary market;
			market["stock"] = city.markets[product].stock;
			market["price"] = city.markets[product].price;
			market["demand"] = city.markets[product].demand;
			market["supply"] = city.markets[product].supply;
			market["desired"] = city.markets[product].desired;
			market["sold"] = city.markets[product].sold;
			market["sold_prev"] = city.markets[product].sold_prev;
			market["shortage"] = city.markets[product].shortage;
			market["surplus"] = city.markets[product].surplus;
			markets.append(market);
		}
		entry["markets"] = markets;
		cities.append(entry);
	}
	data["cities"] = cities;

	Array buildings;
	for (const DemoBuilding& building : demo_buildings_) {
		Dictionary entry;
		entry["id"] = String(building.id.c_str());
		entry["city"] = String(building.city.c_str());
		entry["kind"] = building.kind;
		entry["resource"] = String(building.resource.c_str());
		entry["owner"] = String(building.owner.c_str());
		entry["local_share"] = building.local_share;
		entry["partner_group"] = String(building.partner_group.c_str());
		entry["input_product"] = building.input_product;
		entry["output_product"] = building.output_product;
		entry["recipe"] = building.recipe >= 0
			&& building.recipe < static_cast<int>(demo_recipes_.size())
			? String(demo_recipes_[static_cast<std::size_t>(building.recipe)].id.c_str())
			: String("");
		entry["capacity"] = building.capacity;
		entry["input_per_output"] = building.input_per_output;
		entry["capital_per_capacity"] = building.capital_per_capacity;
		entry["jobs"] = building.jobs;
		entry["wage"] = building.wage;
		entry["opex"] = building.opex;
		// F7: mistura de escolaridade das vagas (save v9).
		entry["education_mix"] = Array{building.education_mix[0],
			building.education_mix[1], building.education_mix[2]};
		entry["output_scale"] = building.output_scale;
		entry["last_utilization"] = building.last_utilization;
		entry["strategic"] = building.strategic;
		entry["loss_days"] = building.loss_days;
		buildings.append(entry);
	}
	data["buildings"] = buildings;

	// F8: negócios de serviço (catálogo global). O nome NÃO é gravado: é
	// derivado da semente + cultura do dono no GDScript (rótulo), então o save
	// guarda só `name_seed` e o vínculo cidade/setor.
	Array service_businesses;
	for (const DemoServiceBusiness& business : demo_service_businesses_) {
		Dictionary entry;
		entry["id"] = business.id;
		entry["sector"] = business.sector;
		entry["city"] = String(business.city.c_str());
		entry["owner_stratum"] = business.owner_stratum;
		entry["capacity"] = business.capacity;
		entry["jobs"] = business.jobs;
		entry["workers"] = business.workers;
		entry["capital"] = business.capital;
		entry["capital_left"] = business.capital_left;
		entry["utilization"] = business.utilization;
		entry["revenue"] = business.revenue;
		entry["wages"] = business.wages;
		entry["input_cost"] = business.input_cost;
		entry["service_cost"] = business.service_cost;
		entry["profit"] = business.profit;
		entry["loss_days"] = business.loss_days;
		entry["bankrupt"] = business.bankrupt;
		entry["opened_day"] = business.opened_day;
		entry["name_seed"] = static_cast<double>(business.name_seed % 9007199254740991ULL);
		service_businesses.append(entry);
	}
	data["service_businesses"] = service_businesses;
	data["service_bankruptcies"] = demo_service_bankruptcies_;

	Array transit;
	for (const DemoShipment& shipment : demo_in_transit_) {
		Dictionary entry;
		entry["to_city"] = String(shipment.to_city.c_str());
		entry["product"] = shipment.product;
		entry["amount"] = shipment.amount;
		entry["arrival_day"] = shipment.arrival_day;
		// F4: reserva de frota do pedido (liberada na chegada).
		entry["fleet_nation"] = String(shipment.fleet_nation.c_str());
		entry["fleet_trucks"] = shipment.fleet_trucks;
		entry["fleet_ships"] = shipment.fleet_ships;
		entry["fleet_planes"] = shipment.fleet_planes;
		entry["fuel"] = shipment.fuel;
		transit.append(entry);
	}
	data["in_transit"] = transit;

	// F4: frota nacional. Uma entrada por nação, com a saúde por modal.
	Array fleets;
	for (const std::string& nation : demo_fleet_order_) {
		const auto found = demo_fleets_.find(nation);
		if (found == demo_fleets_.end()) {
			continue;
		}
		const DemoFleet& fleet = found->second;
		Dictionary entry;
		entry["nation"] = String(nation.c_str());
		entry["trucks"] = fleet.trucks;
		entry["ships"] = fleet.ships;
		entry["planes"] = fleet.planes;
		entry["target_trucks"] = fleet.target_trucks;
		entry["target_ships"] = fleet.target_ships;
		entry["target_planes"] = fleet.target_planes;
		entry["fuel"] = fleet.fuel;
		entry["fuel_target"] = fleet.fuel_target;
		entry["spare_parts"] = fleet.spare_parts;
		entry["health"] = Array{fleet.health[0], fleet.health[1], fleet.health[2]};
		entry["fuel_used_total"] = fleet.fuel_used_total;
		entry["fuel_bought_total"] = fleet.fuel_bought_total;
		entry["parts_used_total"] = fleet.parts_used_total;
		entry["vehicles_bought_total"] = fleet.vehicles_bought_total;
		entry["derived"] = fleet.derived;
		fleets.append(entry);
	}
	data["fleets"] = fleets;

	// F5: moedas (taxa, base, regime, balança) e tarifas declaradas.
	Array currencies;
	for (const DemoCurrency& currency : demo_currencies_) {
		Dictionary entry;
		entry["code"] = String(currency.code.c_str());
		entry["rate_per_anchor"] = currency.rate;
		entry["base_rate"] = currency.base_rate;
		entry["floating"] = currency.floating;
		entry["fallback"] = currency.fallback;
		entry["source"] = String(currency.source.c_str());
		entry["balance_ema"] = currency.balance_ema;
		entry["gross_ema"] = currency.gross_ema;
		Array nations;
		for (const std::string& nation : currency.nations) {
			nations.append(String(nation.c_str()));
		}
		entry["nations"] = nations;
		currencies.append(entry);
	}
	data["currencies"] = currencies;
	Array tariffs;
	for (const DemoTariff& tariff : demo_tariffs_) {
		Dictionary entry;
		entry["buyer"] = String(tariff.buyer.c_str());
		entry["seller"] = String(tariff.seller.c_str());
		entry["product"] = String(tariff.product.c_str());
		entry["rate"] = tariff.rate;
		tariffs.append(entry);
	}
	data["tariffs"] = tariffs;
	// F9 (save v11): grupos, fluxos, política de IDE, incentivos e confiança.
	Array groups;
	for (const DemoGroup& group : demo_groups_) {
		Dictionary entry;
		entry["id"] = String(group.id.c_str());
		entry["name"] = String(group.name.c_str());
		entry["origin_nation"] = String(group.origin_nation.c_str());
		entry["sector"] = String(group.sector.c_str());
		entry["cash"] = group.cash;
		entry["distributable"] = group.distributable;
		entry["revenue_total"] = group.revenue_total;
		entry["profit_total"] = group.profit_total;
		entry["dividends_total"] = group.dividends_total;
		entry["taxes_total"] = group.taxes_total;
		entry["capex_total"] = group.capex_total;
		entry["state_owned"] = group.state_owned;
		Array by_host;
		for (const auto& row : group.profit_by_host) {
			Dictionary host;
			host["nation"] = String(row.first.c_str());
			host["profit"] = row.second;
			by_host.append(host);
		}
		entry["profit_by_host"] = by_host;
		groups.append(entry);
	}
	data["groups"] = groups;
	Array group_flows;
	for (const auto& row : demo_group_flows_) {
		Dictionary entry;
		entry["nation"] = String(row.first.c_str());
		entry["repatriated_out"] = row.second.repatriated_out;
		entry["repatriated_in"] = row.second.repatriated_in;
		entry["dividends"] = row.second.dividends;
		entry["taxes"] = row.second.taxes;
		entry["subsidy"] = row.second.subsidy;
		group_flows.append(entry);
	}
	data["group_flows"] = group_flows;
	Array fdi_policies;
	for (const DemoFdiPolicy& policy : demo_fdi_policies_) {
		Dictionary entry;
		entry["nation"] = String(policy.nation.c_str());
		entry["sector"] = String(policy.sector.c_str());
		entry["mode"] = policy.mode;
		entry["min_local"] = policy.min_local;
		fdi_policies.append(entry);
	}
	data["fdi_policies"] = fdi_policies;
	Array incentives;
	for (const DemoSectorIncentive& incentive : demo_incentives_) {
		Dictionary entry;
		entry["nation"] = String(incentive.nation.c_str());
		entry["sector"] = String(incentive.sector.c_str());
		entry["tax_holiday"] = incentive.tax_holiday;
		entry["subsidy"] = incentive.subsidy;
		incentives.append(entry);
	}
	data["sector_incentives"] = incentives;
	Array confidence;
	for (const auto& row : demo_confidence_) {
		Dictionary entry;
		entry["nation"] = String(row.first.c_str());
		entry["value"] = row.second;
		const auto penalty = demo_confidence_penalty_day_.find(row.first);
		entry["penalty_day"] = penalty == demo_confidence_penalty_day_.end()
			? 0 : penalty->second;
		confidence.append(entry);
	}
	data["confidence"] = confidence;
	// F7: política salarial (save v9). `minimum_wages` por nação (ausente/0 =
	// piso desligado) e a rigidez diária configurável.
	Array minimum_wages;
	for (const auto& [nation_tag, value] : demo_minimum_wages_) {
		if (value <= 0.0) {
			continue;
		}
		Dictionary entry;
		entry["nation"] = String(nation_tag.c_str());
		entry["value"] = value;
		minimum_wages.append(entry);
	}
	data["minimum_wages"] = minimum_wages;
	data["wage_rigidity"] = demo_wage_rigidity_;

	// Portos: identidade + capacidade/fila/obra. Campos aditivos (v3 continua
	// válido; save anterior não tem a chave e o loader mantém os portos do
	// mundo, recalculando a capacidade).
	Array ports;
	for (const Port& port : demo_ports_) {
		Dictionary entry;
		entry["id"] = String(port.id.c_str());
		entry["name"] = String(port.name.c_str());
		entry["nation"] = String(port.nation.c_str());
		entry["landmass"] = String(port.landmass.c_str());
		entry["host_city"] = String(port.host_city.c_str());
		entry["x"] = port.center.x;
		entry["y"] = port.center.y;
		entry["base_capacity_t_per_day"] = port.base_capacity_t_per_day;
		entry["capacity_t_per_day"] = port.capacity_t_per_day;
		entry["pending_capacity_t_per_day"] = port.pending_capacity_t_per_day;
		entry["pending_days"] = port.pending_days;
		ports.append(entry);
	}
	data["ports"] = ports;
	return data;
}

void EconomySim::demo_from_dict(const Dictionary& data) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	// O save substitui o mundo: o cache de status (dicionários reaproveitados)
	// não pode sobreviver a ele.
	demo_status_slots_[0] = DemoStatusCache{};
	demo_status_slots_[1] = DemoStatusCache{};
	demo_status_live_ = -1;
	demo_status_built_day_ = -1;
	++demo_status_static_version_;
	++demo_status_deposit_version_;
	// Recusa save de versão futura (maior que a que este binário conhece).
	const int version = int(data.get("version", 0));
	if (version > kDemoSaveVersion) {
		demo_active_ = false;
		return;
	}
	// Saves v1/v2 (antes da remoção de farinha/pão) trazem índices de produto
	// no layout antigo (farinha=1, pão=2, ferro=3...): remapeia para o layout
	// atual (ferro=1, arroz=2...), descartando farinha/pão.
	//
	// Saves v7 trazem a lista `products` com 114 ids (inclui `body_armor`).
	// Quando ela existe, a migração é POR ID (o nome do produto vence o
	// índice), então remoções futuras não deslocam estoques, mercados,
	// construções ou cargas em trânsito.
	const Array saved_products = data.get("products", Array());
	const bool has_saved_products = saved_products.size() > 0;
	const bool needs_legacy_migration = !has_saved_products && version < 3;
	std::vector<int> product_remap;
	if (has_saved_products) {
		product_remap.resize(
			static_cast<std::size_t>(saved_products.size()), -1);
		for (int index = 0; index < saved_products.size(); ++index) {
			product_remap[static_cast<std::size_t>(index)] =
				demo_product_index(String(saved_products[index]));
		}
	}
	const auto migrate_array = [&](const Array& old) -> Array {
		if (has_saved_products) {
			return demo_remap_product_array(old, product_remap, demo_products_);
		}
		if (needs_legacy_migration) {
			return demo_migrate_product_array(old, demo_products_);
		}
		return old;
	};
	const auto migrate_index = [&](int old_index) -> int {
		if (has_saved_products) {
			if (old_index < 0
					|| old_index >= static_cast<int>(product_remap.size())) {
				return -1;
			}
			return product_remap[static_cast<std::size_t>(old_index)];
		}
		if (needs_legacy_migration) {
			return demo_migrate_product_index(old_index);
		}
		return old_index;
	};
	demo_active_ = bool(data.get("active", true));
	demo_world_mode_ = bool(data.get("world_mode", false));
	day_ = int(data.get("day", 0));
	demo_transport_cash_ = double(data.get("transport_cash", 0.0));
	demo_foreign_cash_ = double(data.get("foreign_cash", 0.0));
	demo_construction_cash_ = double(data.get("construction_cash", 0.0));

	// F5: moedas e tarifas (save v6). Carregadas ANTES das cidades porque o
	// preço-base local (moeda da nação) depende da taxa. Save v5 sem as chaves
	// mantém o que já está carregado pelo autoload (mundo em XGS se não houver).
	if (data.has("currencies")) {
		const Array saved_currencies = data.get("currencies", Array());
		demo_currencies_.clear();
		demo_currencies_.reserve(static_cast<std::size_t>(saved_currencies.size()));
		for (int index = 0; index < saved_currencies.size(); ++index) {
			const Dictionary entry = saved_currencies[index];
			const String code = String(entry.get("code", ""));
			if (code.is_empty()) {
				continue;
			}
			DemoCurrency currency;
			currency.code = std::string(code.utf8().get_data());
			currency.rate = double(entry.get("rate_per_anchor", 1.0));
			if (!std::isfinite(currency.rate) || currency.rate <= 0.0) {
				currency.rate = 1.0;
			}
			currency.base_rate = double(entry.get("base_rate", currency.rate));
			if (!std::isfinite(currency.base_rate) || currency.base_rate <= 0.0) {
				currency.base_rate = currency.rate;
			}
			currency.floating = bool(entry.get("floating", true));
			currency.fallback = bool(entry.get("fallback", false));
			currency.source = std::string(
				String(entry.get("source", "default")).utf8().get_data());
			currency.balance_ema = double(entry.get("balance_ema", 0.0));
			currency.gross_ema = double(entry.get("gross_ema", 0.0));
			const Variant nations_variant = entry.get("nations", Array());
			if (nations_variant.get_type() == Variant::ARRAY) {
				const Array nations = nations_variant;
				for (int n = 0; n < nations.size(); ++n) {
					const std::string nation(
						nations[n].operator String().utf8().get_data());
					if (!nation.empty()) {
						currency.nations.push_back(nation);
					}
				}
			}
			demo_currencies_.push_back(std::move(currency));
		}
		demo_rebuild_currency_indexes();
	}
	if (data.has("tariffs")) {
		const Array saved_tariffs = data.get("tariffs", Array());
		demo_tariffs_.clear();
		demo_tariffs_.reserve(static_cast<std::size_t>(saved_tariffs.size()));
		for (int index = 0; index < saved_tariffs.size(); ++index) {
			const Dictionary entry = saved_tariffs[index];
			DemoTariff tariff;
			tariff.buyer = std::string(
				String(entry.get("buyer", "")).utf8().get_data());
			if (tariff.buyer.empty()) {
				continue;
			}
			tariff.seller = std::string(
				String(entry.get("seller", "*")).utf8().get_data());
			tariff.product = std::string(
				String(entry.get("product", "*")).utf8().get_data());
			if (tariff.seller.empty()) {
				tariff.seller = "*";
			}
			if (tariff.product.empty()) {
				tariff.product = "*";
			}
			tariff.rate = std::clamp(
				double(entry.get("rate", 0.0)), 0.0, 1.0);
			demo_tariffs_.push_back(std::move(tariff));
		}
		demo_rebuild_tariff_lookup();
	}
	// F9 (save v11): grupos e política. Save v10 e anteriores não têm as
	// chaves: os grupos são semeados a partir dos ativos privados no fim do
	// load (regra determinística de demo_seed_groups).
	demo_groups_.clear();
	demo_group_lookup_.clear();
	const Array saved_groups = data.get("groups", Array());
	for (int index = 0; index < saved_groups.size(); ++index) {
		const Dictionary entry = saved_groups[index];
		DemoGroup group;
		group.id = std::string(String(entry.get("id", "")).utf8().get_data());
		if (group.id.empty()) {
			continue;
		}
		group.name = std::string(String(entry.get("name", "")).utf8().get_data());
		group.origin_nation = std::string(
			String(entry.get("origin_nation", "")).utf8().get_data());
		group.sector = std::string(String(entry.get("sector", "")).utf8().get_data());
		group.cash = std::max(0.0, double(entry.get("cash", 0.0)));
		group.distributable = std::max(0.0,
			double(entry.get("distributable", 0.0)));
		group.revenue_total = std::max(0.0,
			double(entry.get("revenue_total", 0.0)));
		group.profit_total = double(entry.get("profit_total", 0.0));
		group.dividends_total = std::max(0.0,
			double(entry.get("dividends_total", 0.0)));
		group.taxes_total = std::max(0.0, double(entry.get("taxes_total", 0.0)));
		group.capex_total = std::max(0.0, double(entry.get("capex_total", 0.0)));
		group.state_owned = bool(entry.get("state_owned", false));
		const Array by_host = entry.get("profit_by_host", Array());
		for (int host_index = 0; host_index < by_host.size(); ++host_index) {
			const Dictionary host = by_host[host_index];
			const std::string nation = std::string(
				String(host.get("nation", "")).utf8().get_data());
			if (!nation.empty()) {
				group.profit_by_host[nation] = double(host.get("profit", 0.0));
			}
		}
		demo_group_lookup_[group.id] = static_cast<int>(demo_groups_.size());
		demo_groups_.push_back(std::move(group));
	}
	demo_fdi_policies_.clear();
	demo_fdi_lookup_.clear();
	const Array saved_policies = data.get("fdi_policies", Array());
	for (int index = 0; index < saved_policies.size(); ++index) {
		const Dictionary entry = saved_policies[index];
		DemoFdiPolicy policy;
		policy.nation = std::string(
			String(entry.get("nation", "")).utf8().get_data());
		policy.sector = std::string(
			String(entry.get("sector", "")).utf8().get_data());
		policy.mode = std::clamp(int(entry.get("mode", 0)), 0, 3);
		policy.min_local = std::clamp(double(entry.get("min_local", 0.0)), 0.0, 1.0);
		if (policy.nation.empty()) {
			continue;
		}
		demo_fdi_lookup_[policy.nation + "|" + policy.sector] =
			static_cast<int>(demo_fdi_policies_.size());
		demo_fdi_policies_.push_back(std::move(policy));
	}
	demo_incentives_.clear();
	demo_incentive_lookup_.clear();
	const Array saved_incentives = data.get("sector_incentives", Array());
	for (int index = 0; index < saved_incentives.size(); ++index) {
		const Dictionary entry = saved_incentives[index];
		DemoSectorIncentive incentive;
		incentive.nation = std::string(
			String(entry.get("nation", "")).utf8().get_data());
		incentive.sector = std::string(
			String(entry.get("sector", "")).utf8().get_data());
		incentive.tax_holiday = std::clamp(
			double(entry.get("tax_holiday", 0.0)), 0.0, 1.0);
		incentive.subsidy = std::max(0.0, double(entry.get("subsidy", 0.0)));
		if (incentive.nation.empty()) {
			continue;
		}
		demo_incentive_lookup_[incentive.nation + "|" + incentive.sector] =
			static_cast<int>(demo_incentives_.size());
		demo_incentives_.push_back(std::move(incentive));
	}
	demo_confidence_.clear();
	demo_confidence_penalty_day_.clear();
	const Array saved_confidence = data.get("confidence", Array());
	for (int index = 0; index < saved_confidence.size(); ++index) {
		const Dictionary entry = saved_confidence[index];
		const std::string nation = std::string(
			String(entry.get("nation", "")).utf8().get_data());
		if (nation.empty()) {
			continue;
		}
		demo_confidence_[nation] = std::clamp(
			double(entry.get("value", 1.0)), 0.0, 1.0);
		const int penalty = int(entry.get("penalty_day", 0));
		if (penalty > 0) {
			demo_confidence_penalty_day_[nation] = penalty;
		}
	}
	demo_group_flows_.clear();
	const Array saved_flows = data.get("group_flows", Array());
	for (int index = 0; index < saved_flows.size(); ++index) {
		const Dictionary entry = saved_flows[index];
		const std::string nation = std::string(
			String(entry.get("nation", "")).utf8().get_data());
		if (nation.empty()) {
			continue;
		}
		DemoGroupFlow flow;
		flow.repatriated_out = std::max(0.0,
			double(entry.get("repatriated_out", 0.0)));
		flow.repatriated_in = std::max(0.0,
			double(entry.get("repatriated_in", 0.0)));
		flow.dividends = std::max(0.0, double(entry.get("dividends", 0.0)));
		flow.taxes = std::max(0.0, double(entry.get("taxes", 0.0)));
		flow.subsidy = std::max(0.0, double(entry.get("subsidy", 0.0)));
		demo_group_flows_[nation] = flow;
	}
	// F7: política salarial. Save v8 e anteriores não têm as chaves: piso
	// desligado e rigidez padrão (1%/dia).
	demo_minimum_wages_.clear();
	Array saved_minimum_wages = data.get("minimum_wages", Array());
	for (int index = 0; index < saved_minimum_wages.size(); ++index) {
		const Dictionary entry = saved_minimum_wages[index];
		const String tag = String(entry.get("nation", ""));
		const double value = std::clamp(double(entry.get("value", 0.0)),
			0.0, 100.0);
		if (!tag.is_empty() && value > 0.0) {
			demo_minimum_wages_[std::string(tag.utf8().get_data())] = value;
		}
	}
	demo_wage_rigidity_ = std::clamp(
		double(data.get("wage_rigidity", demo_wage_rigidity_)), 0.0, 0.25);

	Array produced = migrate_array(data.get("produced_total", Array()));
	Array consumed = migrate_array(data.get("consumed_total", Array()));
	for (int index = 0; index < demo_products_; ++index) {
		demo_produced_[index] = index < produced.size() ? double(produced[index]) : 0.0;
		demo_consumed_total_[index] =
			index < consumed.size() ? double(consumed[index]) : 0.0;
		demo_produced_today_[index] = 0.0;
		demo_consumed_today_[index] = 0.0;
	}

	demo_cities_.clear();
	Array cities = data.get("cities", Array());
	for (int index = 0; index < cities.size(); ++index) {
		Dictionary entry = cities[index];
		DemoCity city;
		city.id = std::string(String(entry.get("id", "")).utf8().get_data());
		city.name = std::string(String(entry.get("name", "")).utf8().get_data());
		city.nation = std::string(String(entry.get("nation", "")).utf8().get_data());
		// F5 v7: preço de mercado passou a ficar na ÂNCORA. Saves v6 e
		// anteriores guardavam preço local; converte uma vez pela taxa da nação
		// (as moedas foram carregadas antes das cidades).
		const double save_fx = version >= 7
			? 1.0 : std::max(1e-9, demo_fx_rate(city.nation));
		city.home_nation = std::string(
			String(entry.get("home_nation", String(city.nation.c_str()))).utf8().get_data());
		city.landmass =
			std::string(String(entry.get("landmass", "")).utf8().get_data());
		Array center = entry.get("center", Array());
		if (center.size() >= 2) {
			city.center = {double(center[0]), double(center[1])};
		}
		city.population = double(entry.get("population", 0.0));
		// Save antigo (ou cenário sintético) sem o campo: a população real
		// exibida é a própria unidade de simulação (comportamento anterior).
		city.simulation_population = read_sim_population(entry, city.population);
		city.firm_cash = double(entry.get("firm_cash", 0.0));
		city.service_cash = double(entry.get("service_cash", 0.0));
		city.government_budget = double(entry.get("government_budget", 0.0));
		// Save antigo não tem o índice: mantém a média mundial (1,0).
		city.external_income_index = std::clamp(
			double(entry.get("external_income_index", 1.0)), 0.0, 8.0);
		// F7 calibração: TFP derivada do PIB PPP per capita. Save v9 e
		// anteriores não têm o campo: 0 = DERIVAR do índice de renda já gravado
		// (o mesmo `derived:gdp_ppp`), sem inventar produtividade.
		city.productivity_index = std::clamp(
			double(entry.get("productivity_index", 0.0)), 0.0, 8.0);
		// F7 calibração: save anterior à distribuição por Gini não tem o campo;
		// 0 mantém o gate legado por estrato até o GeographyService declarar.
		city.income_gini = std::clamp(
			double(entry.get("income_gini", 0.0)), 0.0, 1.0);
		city.service_jobs = double(entry.get("service_jobs", 0.0));
		city.service_wage = double(entry.get("service_wage", 0.0));
		city.service_wage_bill = double(entry.get("service_wage_bill", 0.0));
		city.service_revenue = double(entry.get("service_revenue", 0.0));
		city.service_profit = double(entry.get("service_profit", 0.0));
		city.opex_revenue = double(entry.get("opex_revenue", 0.0));
		city.freight_revenue = double(entry.get("freight_revenue", 0.0));
		city.construction_revenue = double(entry.get("construction_revenue", 0.0));
		city.construction_revenue_pending =
			double(entry.get("construction_revenue_pending", 0.0));
		city.capital_income_next = double(entry.get("capital_income_next", 0.0));
		city.gdp = double(entry.get("gdp", 0.0));
		// F8: serviços (save v10). Sem o bloco (v9 e anteriores), os mercados
		// ficam no preço-âncora do catálogo e os negócios são semeados no fim
		// do load (`demo_seed_services`).
		city.service_export_revenue = double(entry.get("service_export_revenue", 0.0));
		city.service_government_revenue =
			double(entry.get("service_government_revenue", 0.0));
		city.service_input_cost = double(entry.get("service_input_cost", 0.0));
		city.service_business_profit =
			double(entry.get("service_business_profit", 0.0));
		city.service_business_count =
			int(entry.get("service_business_count", 0));
		city.private_health_cover = std::clamp(
			double(entry.get("private_health_cover", 0.0)), 0.0, 1.0);
		city.private_education_cover = std::clamp(
			double(entry.get("private_education_cover", 0.0)), 0.0, 1.0);
		city.service_tonnage_prev = double(entry.get("service_tonnage_prev", 0.0));
		const Array saved_services = entry.get("services", Array());
		for (int sector = 0; sector < kDemoMaxServiceSectors; ++sector) {
			DemoServiceMarket& market = city.service_markets[sector];
			if (sector < static_cast<int>(demo_service_sectors_.size())) {
				market.base_price =
					demo_service_sectors_[static_cast<std::size_t>(sector)].price;
			}
			market.price = market.base_price;
			if (sector >= saved_services.size()
					|| saved_services[sector].get_type() != Variant::ARRAY) {
				continue;
			}
			const Array fields = saved_services[sector];
			if (fields.size() < 13) {
				continue;
			}
			market.demand = double(fields[0]);
			market.supply = double(fields[1]);
			market.unmet = double(fields[2]);
			market.price = double(fields[3]) > 0.0
				? double(fields[3]) : market.base_price;
			market.base_price = double(fields[4]) > 0.0
				? double(fields[4]) : market.base_price;
			market.revenue = double(fields[5]);
			market.input_cost = double(fields[6]);
			market.service_cost = double(fields[7]);
			market.jobs = double(fields[8]);
			market.businesses = double(fields[9]);
			market.exports = double(fields[10]);
			market.export_revenue = double(fields[11]);
			market.coverage = std::clamp(double(fields[12]), 0.0, 1.0);
		}

		Array suitability = entry.get("suitability", Array());
		Array revenue = entry.get("revenue_by_product", Array());
		Array imported = entry.get("imported", Array());
		Array exported = entry.get("exported", Array());
		Array import_value = entry.get("import_value", Array());
		Array export_value = entry.get("export_value", Array());
		Array external_demand = entry.get("external_demand", Array());
		Array import_price = entry.get("import_price", Array());
		suitability = migrate_array(suitability);
		revenue = migrate_array(revenue);
		imported = migrate_array(imported);
		exported = migrate_array(exported);
		import_value = migrate_array(import_value);
		export_value = migrate_array(export_value);
		external_demand = migrate_array(external_demand);
		import_price = migrate_array(import_price);
		for (int product = 0; product < demo_products_; ++product) {
			city.suitability[product] = product < suitability.size()
				? double(suitability[product]) : (product >= 1 ? 0.0 : 1.0);
			city.revenue_by_product[product] = product < revenue.size()
				? double(revenue[product]) : 0.0;
			city.imported[product] = product < imported.size()
				? double(imported[product]) : 0.0;
			city.exported[product] = product < exported.size()
				? double(exported[product]) : 0.0;
			city.import_value[product] = product < import_value.size()
				? double(import_value[product]) : 0.0;
			city.export_value[product] = product < export_value.size()
				? double(export_value[product]) : 0.0;
			city.external_demand[product] = product < external_demand.size()
				? double(external_demand[product]) : 0.0;
			// F5: cotação de importação (na âncora desde o v7). Save anterior à
			// F5 não tem o array: sem cotação até a primeira compra externa.
			city.import_price[product] = product < import_price.size()
				? safe_nonneg(double(import_price[product])) * save_fx : 0.0;
		}

		Dictionary saved_deposits = entry.get("deposits", Dictionary());
		for (int product = 0; product < demo_products_; ++product) {
			const String key = String(demo_product_name(product));
			if (!saved_deposits.has(key)) {
				continue;
			}
			Dictionary deposit_entry = saved_deposits[key];
			DemoDeposit& deposit = city.deposits[product];
			deposit.tracked = true;
			deposit.initial_kt = double(deposit_entry.get("initial_kt", 0.0));
			deposit.remaining_kt = double(deposit_entry.get("remaining_kt", 0.0));
			deposit.extracted_kt = double(deposit_entry.get("extracted_kt", 0.0));
			deposit.quality = double(deposit_entry.get("quality", 1.0));
			deposit.depth_m = double(deposit_entry.get("depth_m", 0.0));
		}

		Dictionary pop = entry.get("pop", Dictionary());
		city.pop.population = double(pop.get("population", city.simulation_population));
		city.pop.labor_force = double(pop.get("labor_force", 0.0));
		city.pop.employed = double(pop.get("employed", 0.0));
		city.pop.unemployed = double(pop.get("unemployed", 0.0));
		city.pop.dependents = double(pop.get("dependents", 0.0));
		city.pop.wage_income = double(pop.get("wage_income", 0.0));
		city.pop.capital_income = double(pop.get("capital_income", 0.0));
		city.pop.transfers = double(pop.get("transfers", 0.0));
		city.pop.taxes = double(pop.get("taxes", 0.0));
		city.pop.service_spend = double(pop.get("service_spend", 0.0));
		city.pop.disposable = double(pop.get("disposable", 0.0));
		city.pop.external_income = double(pop.get("external_income", 0.0));
		city.pop.food_spend = double(pop.get("food_spend", 0.0));
		city.pop.consumed = double(pop.get("consumed", 0.0));
		city.pop.need = double(pop.get("need", 0.0));
		city.pop.satisfaction = double(pop.get("satisfaction", 0.0));
		city.pop.savings = double(pop.get("savings", 0.0));
		// F3: estado das necessidades (aditivo; save anterior = zero).
		demo_init_pop_needs(city.pop);
		Dictionary needs_state = pop.get("needs", Dictionary());
		city.pop.needs_level = int(needs_state.get("level", 0));
		city.pop.needs_satisfaction = double(needs_state.get("satisfaction",
			city.pop.satisfaction));
		city.pop.needs_spend = double(needs_state.get("spend", 0.0));
		city.pop.durable_spend = double(needs_state.get("durable_spend", 0.0));
		Array saved_level_sat = needs_state.get("level_satisfaction", Array());
		Array saved_level_want = needs_state.get("level_want", Array());
		Array saved_level_cons = needs_state.get("level_consumed", Array());
		for (int level = 0; level < kDemoMaxNeedsLevels; ++level) {
			city.pop.level_satisfaction[level] = level < saved_level_sat.size()
				? double(saved_level_sat[level]) : 0.0;
			city.pop.level_want[level] = level < saved_level_want.size()
				? double(saved_level_want[level]) : 0.0;
			city.pop.level_consumed[level] = level < saved_level_cons.size()
				? double(saved_level_cons[level]) : 0.0;
		}
		Dictionary saved_durable = needs_state.get("durable_stock", Dictionary());
		for (int product = 0; product < demo_products_
				&& product < static_cast<int>(city.pop.durable_stock.size()); ++product) {
			const String key = String(demo_product_name(product));
			if (saved_durable.has(key)) {
				city.pop.durable_stock[static_cast<std::size_t>(product)] =
					std::max(0.0, double(saved_durable[key]));
			}
		}
		// F7: mercado de trabalho e educação (save v9). Save v8 e anteriores não
		// têm o bloco: o perfil é derivado do índice de renda já gravado. A
		// força de trabalho, o emprego e os salários são recalculados pelo
		// mesmo modelo do primeiro dia (migração documentada).
		Dictionary saved_labor = pop.get("labor", Dictionary());
		const bool has_labor = version >= 9 && !saved_labor.is_empty();
		if (!has_labor) {
			demo_apply_labor_profile(city);
		} else {
			DemoPop& labor_pop = city.pop;
			labor_pop.working_age = std::max(0.0,
				double(saved_labor.get("working_age", labor_pop.working_age)));
			labor_pop.retirees = std::max(0.0,
				double(saved_labor.get("retirees", 0.0)));
			labor_pop.students = std::max(0.0,
				double(saved_labor.get("students", 0.0)));
			labor_pop.recruit_reserve = std::max(0.0,
				double(saved_labor.get("recruit_reserve", 0.0)));
			const auto read_levels = [&saved_labor](const char* key,
					double* out, int count) {
				const Array values = saved_labor.get(key, Array());
				for (int level = 0; level < count; ++level) {
					out[level] = level < values.size()
						? std::max(0.0, double(values[level])) : out[level];
				}
			};
			read_levels("education", labor_pop.education,
				kDemoEducationLevels);
			read_levels("participation", labor_pop.participation,
				kDemoEducationLevels);
			read_levels("labor_force_by_level", labor_pop.labor_force_by_level,
				kDemoEducationLevels);
			read_levels("employed_by_level", labor_pop.employed_by_level,
				kDemoEducationLevels);
			read_levels("unemployed_by_level", labor_pop.unemployed_by_level,
				kDemoEducationLevels);
			read_levels("vacancies_by_level", labor_pop.vacancies_by_level,
				kDemoEducationLevels);
			read_levels("wage_by_level", labor_pop.wage_by_level,
				kDemoEducationLevels);
			read_levels("wage_income_by_level",
				labor_pop.wage_income_by_level, kDemoEducationLevels);
			read_levels("studying", labor_pop.studying,
				kDemoEducationLevels - 1);
			labor_pop.students = labor_pop.studying[0] + labor_pop.studying[1];
			labor_pop.income_level = std::clamp(
				int(saved_labor.get("income_level", 1)), 0, 3);
			// F7 calibração: blocos aditivos. Save anterior ao calibração (v9
			// sem eles) cai no nível único e mantém o salário carregado como
			// histórico (wage_anchored = true, com inércia).
			const bool has_strata = saved_labor.has("income_level_by_stratum");
			if (has_strata) {
				const Array strata = saved_labor.get("income_level_by_stratum", Array());
				for (int stratum = 0; stratum < kDemoEducationLevels; ++stratum) {
					labor_pop.stratum_income_level[stratum] = stratum < strata.size()
						? std::clamp(int(strata[stratum]), 0, 3)
						: labor_pop.income_level;
				}
			} else {
				for (int stratum = 0; stratum < kDemoEducationLevels; ++stratum) {
					labor_pop.stratum_income_level[stratum] = labor_pop.income_level;
				}
			}
			const Array share = saved_labor.get("income_level_share", Array());
			for (int level_index = 0; level_index < kDemoIncomeLevels; ++level_index) {
				labor_pop.income_level_share[level_index] = level_index < share.size()
					? std::clamp(double(share[level_index]), 0.0, 1.0) : 0.0;
			}
			if (share.is_empty()) {
				labor_pop.income_level_share[labor_pop.income_level] = 1.0;
			}
			labor_pop.wage_anchored = bool(saved_labor.get("wage_anchored", true));
			labor_pop.labor_force = 0.0;
			for (int level = 0; level < kDemoEducationLevels; ++level) {
				labor_pop.labor_force +=
					labor_pop.labor_force_by_level[level];
			}
			labor_pop.employed = 0.0;
			labor_pop.unemployed = 0.0;
			for (int level = 0; level < kDemoEducationLevels; ++level) {
				labor_pop.employed += labor_pop.employed_by_level[level];
				labor_pop.unemployed += labor_pop.unemployed_by_level[level];
			}
			labor_pop.wage_income = 0.0;
			for (int level = 0; level < kDemoEducationLevels; ++level) {
				labor_pop.wage_income += labor_pop.wage_income_by_level[level];
			}
			labor_pop.dependents = std::max(0.0,
				city.pop.population - labor_pop.labor_force);
		}
		city.government_goods_spend = double(entry.get("government_goods_spend", 0.0));
		Array desired_by_category = pop.get("desired_by_category", Array());
		Array consumed_by_category = pop.get("consumed_by_category", Array());
		city.pop.desired_by_category.assign(demo_food_products_.size(), 0.0);
		city.pop.consumed_by_category.assign(demo_food_products_.size(), 0.0);
		for (int product = 0; product < static_cast<int>(demo_food_products_.size()); ++product) {
			city.pop.desired_by_category[product] =
				product < desired_by_category.size()
					? double(desired_by_category[product]) : 0.0;
			city.pop.consumed_by_category[product] =
				product < consumed_by_category.size()
					? double(consumed_by_category[product]) : 0.0;
		}

		Array markets = entry.get("markets", Array());
		if (has_saved_products || needs_legacy_migration) {
			// Remapeia os mercados antigos por id (save v7 com 114 produtos,
			// descartando `body_armor`) ou pelo deslocamento -2 (save v1/v2 com
			// farinha/pão). Mercado sem correspondência recebe o preço-base.
			Array remapped;
			remapped.resize(demo_products_);
			for (int new_index = 0; new_index < demo_products_; ++new_index) {
				Dictionary m;
				m["stock"] = 0.0;
				m["price"] = demo_base_price(new_index);
				m["demand"] = 0.0;
				m["supply"] = 0.0;
				m["desired"] = 0.0;
				m["sold"] = 0.0;
				m["sold_prev"] = 0.0;
				m["shortage"] = 0.0;
				m["surplus"] = 0.0;
				remapped[new_index] = m;
			}
			for (int old_index = 0; old_index < markets.size(); ++old_index) {
				const int new_index = migrate_index(old_index);
				if (new_index >= 0 && new_index < demo_products_) {
					remapped[new_index] = markets[old_index];
				}
			}
			markets = remapped;
		}
		// Saves antigos (4 produtos) não trazem mercados dos novos bens: preço-base.
		for (int product = markets.size(); product < demo_products_; ++product) {
			if (product >= 0) {
				city.markets[product].price = demo_base_price(product);
			}
		}
		for (int product = 0; product < demo_products_ && product < markets.size(); ++product) {
			Dictionary market = markets[product];
			city.markets[product].stock = double(market.get("stock", 0.0));
			city.markets[product].price = double(market.get("price", 1.0)) * save_fx;
			city.markets[product].demand = double(market.get("demand", 0.0));
			city.markets[product].supply = double(market.get("supply", 0.0));
			city.markets[product].desired = double(market.get("desired", 0.0));
			city.markets[product].sold = double(market.get("sold", 0.0));
			city.markets[product].sold_prev = double(market.get("sold_prev", 0.0));
			city.markets[product].shortage = double(market.get("shortage", 0.0));
			city.markets[product].surplus = double(market.get("surplus", 0.0));
		}
		demo_cities_.push_back(city);
	}

	demo_buildings_.clear();
	Array buildings = data.get("buildings", Array());
	for (int index = 0; index < buildings.size(); ++index) {
		Dictionary entry = buildings[index];
		DemoBuilding building;
		building.id = std::string(String(entry.get("id", "")).utf8().get_data());
		building.city = std::string(String(entry.get("city", "")).utf8().get_data());
		building.kind = int(entry.get("kind", 0));
		building.resource =
			std::string(String(entry.get("resource", "")).utf8().get_data());
		building.owner =
			std::string(String(entry.get("owner", "private")).utf8().get_data());
		building.local_share = std::clamp(
			double(entry.get("local_share", 0.0)), 0.0, 1.0);
		building.partner_group = std::string(
			String(entry.get("partner_group", "")).utf8().get_data());
		building.input_product = int(entry.get("input_product", -1));
		building.output_product = int(entry.get("output_product", 0));
		if (has_saved_products || needs_legacy_migration) {
			building.input_product = migrate_index(building.input_product);
			building.output_product = migrate_index(building.output_product);
			if (building.output_product < 0) {
				// Produto removido da cadeia (farinha/pão ou body_armor):
				// construção descartada.
				continue;
			}
		}
		building.capacity = double(entry.get("capacity", 0.0));
		building.input_per_output = double(entry.get("input_per_output", 0.0));
		// Receita industrial: o id é resolvido na tabela carregada por
		// demo_set_recipes (v4). Save v3 não tem a chave -> produtor legado.
		const String recipe_id = String(entry.get("recipe", ""));
		building.recipe = recipe_id.is_empty() ? -1 : demo_recipe_index(recipe_id);
		building.capital_per_capacity = std::max(1.0, safe_nonneg(
			double(entry.get("capital_per_capacity", kDemoCapitalPerCapacity))));
		building.jobs = double(entry.get("jobs", 0.0));
		building.wage = double(entry.get("wage", 0.0));
		building.opex = double(entry.get("opex", 0.0));
		// F7: mistura de escolaridade. Save v8 sem a chave: usa a da receita
		// resolvida; primária segue o padrão do struct (quase só básico).
		if (entry.has("education_mix")) {
			const Array mix = entry.get("education_mix", Array());
			double total = 0.0;
			for (int level = 0;
					level < kDemoEducationLevels && level < mix.size(); ++level) {
				building.education_mix[level] = std::max(0.0, double(mix[level]));
				total += building.education_mix[level];
			}
			if (total > 1e-9) {
				for (int level = 0; level < kDemoEducationLevels; ++level) {
					building.education_mix[level] /= total;
				}
			}
		} else if (building.recipe >= 0
				&& building.recipe < static_cast<int>(demo_recipes_.size())) {
			const DemoRecipe& recipe =
				demo_recipes_[static_cast<std::size_t>(building.recipe)];
			for (int level = 0; level < kDemoEducationLevels; ++level) {
				building.education_mix[level] = recipe.education_mix[level];
			}
		}
		building.output_scale = double(entry.get("output_scale", 1.0));
		building.last_utilization = double(entry.get("last_utilization", 1.0));
		building.strategic = bool(entry.get("strategic", false));
		building.loss_days = int(entry.get("loss_days", 0));
		demo_buildings_.push_back(building);
	}

	demo_in_transit_.clear();
	Array transit = data.get("in_transit", Array());
	for (int index = 0; index < transit.size(); ++index) {
		Dictionary entry = transit[index];
		DemoShipment shipment;
		shipment.to_city =
			std::string(String(entry.get("to_city", "")).utf8().get_data());
		shipment.product = int(entry.get("product", 0));
		if (has_saved_products || needs_legacy_migration) {
			const int migrated = migrate_index(shipment.product);
			if (migrated < 0) {
				// Produto removido da cadeia em trânsito (farinha/pão ou
				// body_armor): carga descartada.
				continue;
			}
			shipment.product = migrated;
		}
		shipment.amount = double(entry.get("amount", 0.0));
		shipment.arrival_day = int(entry.get("arrival_day", 0));
		// F4: reserva da frota (save v5). Save anterior: frota livre ao carregar.
		shipment.fleet_nation =
			std::string(String(entry.get("fleet_nation", "")).utf8().get_data());
		shipment.fleet_trucks = double(entry.get("fleet_trucks", 0.0));
		shipment.fleet_ships = double(entry.get("fleet_ships", 0.0));
		shipment.fleet_planes = double(entry.get("fleet_planes", 0.0));
		shipment.fuel = double(entry.get("fuel", 0.0));
		demo_in_transit_.push_back(shipment);
	}

	// Portos (campo aditivo, ausente em saves anteriores ao porto): o save
	// preserva capacidade, expansão em obra e fila do dia. Sem "ports", mantém
	// os portos do mundo já carregado e recalcula a capacidade pela população
	// que veio no save (save antigo é compatível: comportamento anterior).
	const bool has_saved_ports = data.has("ports");
	if (has_saved_ports) {
		demo_ports_.clear();
		Array ports = data.get("ports", Array());
		for (int index = 0; index < ports.size(); ++index) {
			Dictionary entry = ports[index];
			Port port;
			port.id = std::string(String(entry.get("id", "")).utf8().get_data());
			port.name = std::string(String(entry.get("name", "")).utf8().get_data());
			port.nation = std::string(String(entry.get("nation", "")).utf8().get_data());
			port.landmass =
				std::string(String(entry.get("landmass", "")).utf8().get_data());
			port.host_city =
				std::string(String(entry.get("host_city", "")).utf8().get_data());
			port.center = {double(entry.get("x", 0.0)), double(entry.get("y", 0.0))};
			port.base_capacity_t_per_day =
				safe_nonneg(double(entry.get("base_capacity_t_per_day", 0.0)));
			port.capacity_t_per_day =
				safe_nonneg(double(entry.get("capacity_t_per_day",
					port.base_capacity_t_per_day)));
			port.pending_capacity_t_per_day =
				safe_nonneg(double(entry.get("pending_capacity_t_per_day", 0.0)));
			port.pending_days = std::max(0, int(entry.get("pending_days", 0)));
			demo_ports_.push_back(std::move(port));
		}
	}

	demo_rebuild_indexes();
	if (!has_saved_ports && !demo_ports_.empty()) {
		demo_init_port_capacities();
	}
	// F4: frota (save v5). Save anterior não tem o bloco: deriva de
	// população/PIB, documentado como `source: derived`.
	demo_fleets_.clear();
	demo_fleet_order_.clear();
	const Array saved_fleets = data.get("fleets", Array());
	for (int index = 0; index < saved_fleets.size(); ++index) {
		Dictionary entry = saved_fleets[index];
		const std::string nation =
			std::string(String(entry.get("nation", "")).utf8().get_data());
		if (nation.empty()) {
			continue;
		}
		DemoFleet fleet;
		fleet.trucks = safe_nonneg(double(entry.get("trucks", 0.0)));
		fleet.ships = safe_nonneg(double(entry.get("ships", 0.0)));
		fleet.planes = safe_nonneg(double(entry.get("planes", 0.0)));
		fleet.target_trucks = safe_nonneg(double(entry.get("target_trucks", 0.0)));
		fleet.target_ships = safe_nonneg(double(entry.get("target_ships", 0.0)));
		fleet.target_planes = safe_nonneg(double(entry.get("target_planes", 0.0)));
		fleet.fuel = safe_nonneg(double(entry.get("fuel", 0.0)));
		fleet.fuel_target = safe_nonneg(double(entry.get("fuel_target", 0.0)));
		fleet.spare_parts = safe_nonneg(double(entry.get("spare_parts", 0.0)));
		Array health = entry.get("health", Array());
		for (int mode = 0; mode < 3; ++mode) {
			fleet.health[mode] = mode < health.size()
				? std::clamp(double(health[mode]), kDemoFleetHealthFloor, 1.0) : 1.0;
		}
		fleet.fuel_used_total = double(entry.get("fuel_used_total", 0.0));
		fleet.fuel_bought_total = double(entry.get("fuel_bought_total", 0.0));
		fleet.parts_used_total = double(entry.get("parts_used_total", 0.0));
		fleet.vehicles_bought_total =
			double(entry.get("vehicles_bought_total", 0.0));
		fleet.derived = bool(entry.get("derived", false));
		demo_fleets_.emplace(nation, fleet);
		demo_fleet_order_.push_back(nation);
	}
	// Reserva das cargas em trânsito: reconstrói os veículos ocupados.
	for (const DemoShipment& shipment : demo_in_transit_) {
		if (shipment.fleet_nation.empty()) {
			continue;
		}
		DemoTransportDemand busy;
		busy.trucks = shipment.fleet_trucks;
		busy.ships = shipment.fleet_ships;
		busy.planes = shipment.fleet_planes;
		const auto found = demo_fleets_.find(shipment.fleet_nation);
		if (found == demo_fleets_.end()) {
			continue;
		}
		found->second.trucks_busy += busy.trucks;
		found->second.ships_busy += busy.ships;
		found->second.planes_busy += busy.planes;
	}
	if (demo_fleets_.empty()) {
		demo_init_fleets();
	} else {
		// Índices dos bens da frota (products já carregados antes do load).
		demo_fleet_product_fuel_ = demo_product_index(String("fuel"));
		demo_fleet_product_truck_ = demo_product_index(String("truck"));
		demo_fleet_product_ship_ = demo_product_index(String("cargo_ship"));
		demo_fleet_product_plane_ = demo_product_index(String("cargo_plane"));
		demo_fleet_product_parts_ = demo_product_index(String("spare_parts"));
	}

	// F8: negócios de serviço (save v10). Save v9 e anteriores não têm o bloco:
	// semeia os negócios como num mundo novo (migração declarada).
	demo_service_businesses_.clear();
	demo_next_service_business_id_ = 1;
	demo_service_bankruptcies_ = int(data.get("service_bankruptcies", 0));
	const Array saved_businesses = data.get("service_businesses", Array());
	for (int index = 0; index < saved_businesses.size(); ++index) {
		if (saved_businesses[index].get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary entry = saved_businesses[index];
		DemoServiceBusiness business;
		business.id = int(entry.get("id", 0));
		business.sector = int(entry.get("sector", -1));
		business.city = std::string(
			String(entry.get("city", "")).utf8().get_data());
		business.owner_stratum = std::clamp(
			int(entry.get("owner_stratum", kDemoEducationLevels - 1)),
			0, kDemoEducationLevels - 1);
		business.capacity = safe_nonneg(double(entry.get("capacity", 0.0)));
		business.jobs = safe_nonneg(double(entry.get("jobs", 0.0)));
		business.workers = safe_nonneg(double(entry.get("workers", 0.0)));
		business.capital = safe_nonneg(double(entry.get("capital", 0.0)));
		business.capital_left = safe_nonneg(double(entry.get("capital_left", 0.0)));
		business.utilization = std::clamp(
			double(entry.get("utilization", 0.0)), 0.0, 1.0);
		business.revenue = double(entry.get("revenue", 0.0));
		business.wages = safe_nonneg(double(entry.get("wages", 0.0)));
		business.input_cost = safe_nonneg(double(entry.get("input_cost", 0.0)));
		business.service_cost = safe_nonneg(double(entry.get("service_cost", 0.0)));
		business.profit = double(entry.get("profit", 0.0));
		business.loss_days = std::max(0, int(entry.get("loss_days", 0)));
		business.bankrupt = bool(entry.get("bankrupt", false));
		business.opened_day = int(entry.get("opened_day", 0));
		const double seed = double(entry.get("name_seed", 0.0));
		business.name_seed = seed > 0.0
			? static_cast<unsigned long long>(seed) : 0ULL;
		if (business.name_seed == 0ULL) {
			unsigned long long hash = 1469598103934665603ULL;
			for (const char c : business.city + "|" + std::to_string(business.id)) {
				hash ^= static_cast<unsigned char>(c);
				hash *= 1099511628211ULL;
			}
			business.name_seed = hash;
		}
		demo_next_service_business_id_ = std::max(
			demo_next_service_business_id_, business.id + 1);
		demo_service_businesses_.push_back(std::move(business));
	}
	if (demo_service_businesses_.empty() && demo_services_loaded_) {
		demo_seed_services();
	} else {
		demo_rebuild_service_indexes();
	}
	// F9: save v10 e anteriores não têm grupos. Semeia a partir dos ativos
	// privados (id determinístico); os nomes entram pelo cliente no próximo
	// boot, como no mundo recém-criado.
	if (demo_groups_.empty()) {
		demo_groups_seeded_ = false;
		demo_seed_groups();
	} else {
		demo_groups_seeded_ = true;
	}
}

} // namespace godot
