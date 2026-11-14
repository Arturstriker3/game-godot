#include "economy_sim.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <utility>

namespace godot {

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

constexpr double kPi = 3.14159265358979323846;
constexpr double kEarthRadiusKm = 6371.0;
constexpr double kMapWidth = 5000.0;
constexpr double kMapHeight = 2500.0;
constexpr double kHoursPerDay = 24.0;

// Sobrecarga de embarque por lote (a frota sai em comboio, não em série).
// Usada por make_leg (tempo do trecho), por isso vive no primeiro bloco.
constexpr double kDemoLoadHoursPerBatch = 0.5;

// 0,3 kg de trigo por pessoa por dia, em toneladas.
constexpr double kNeedPerCapita = 0.0003;

// Indicadores urbanos derivados (não simulados): empregos industriais, renda
// relativa e grau de industrialização usados pelo crescimento visual da cidade.
constexpr double kTonsPerJob = 0.0025;
constexpr double kTonsPerCapitaSaturation = 0.0006;
constexpr double kIncomeFloor = 0.35;
constexpr double kIncomeIndustrialWeight = 0.5;
constexpr double kIncomeDeficitWeight = 0.3;

// Abaixo disso a cidade está em emergência e paga o frete aéreo.
constexpr double kEmergencyCoverDays = 2.0;
constexpr double kEmergencyAirShare = 0.35;

// Dias de cobertura (estoque + em trânsito) que uma cidade tenta manter. Sem
// isso, uma cidade a ~9 dias de navio só pedia o déficit diário e recebia
// ~1/9 da necessidade.
constexpr double kCoverDays = 10.0;

// Limite de pedidos por dia (proteção); os comboios exibidos têm limite próprio.
constexpr std::size_t kMaxOrdersPerDay = 24;

// Versão do schema do save do modo demo. Aumente ao mudar a estrutura do
// dicionário; demo_from_dict recusa versões futuras (maiores) e migra as
// anteriores.
// v3: farinha/pão removidos da cadeia (enum DemoProduct deslocou -2 a partir do
// ferro) e 21 produtos. demo_from_dict remapeia saves v1/v2 (com farinha/pão).
// v5: frota nacional (F4) — `fleets` e a reserva de veículos de cada carga em
// `in_transit`. Save v4 sem a chave deriva a frota de população/PIB.
constexpr int kDemoSaveVersion = 5;

// Até esta distância, o vendedor do mesmo país é considerado mercado regional.
constexpr double kRegionalRadiusKm = 500.0;

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
	ClassDB::bind_method(D_METHOD("initialize", "cities", "ports"), &EconomySim::initialize);
	ClassDB::bind_method(D_METHOD("tick"), &EconomySim::tick);
	ClassDB::bind_method(D_METHOD("get_status"), &EconomySim::get_status);
	ClassDB::bind_method(D_METHOD("initialize_demo"), &EconomySim::initialize_demo);
	ClassDB::bind_method(D_METHOD("initialize_demo_world", "cities", "ports", "buildings"),
	                     &EconomySim::initialize_demo_world);
	ClassDB::bind_method(D_METHOD("tick_demo"), &EconomySim::tick_demo);
	ClassDB::bind_method(D_METHOD("demo_tick_core"), &EconomySim::demo_tick_core);
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
	ClassDB::bind_method(D_METHOD("demo_set_goods", "goods"),
	                     &EconomySim::demo_set_goods);
	ClassDB::bind_method(D_METHOD("demo_set_recipes", "recipes"),
	                     &EconomySim::demo_set_recipes);
	ClassDB::bind_method(D_METHOD("demo_set_needs", "needs"),
	                     &EconomySim::demo_set_needs);
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
	ClassDB::bind_method(D_METHOD("initialize_benchmark", "city_count",
	                     "buildings_per_city", "seed"),
	                     &EconomySim::initialize_benchmark);
	ClassDB::bind_method(D_METHOD("run_benchmark", "days", "warmup_days"),
	                     &EconomySim::run_benchmark);
	ClassDB::bind_method(D_METHOD("get_benchmark_status"),
	                     &EconomySim::get_benchmark_status);
	ClassDB::bind_method(D_METHOD("get_benchmark_city_count"),
	                     &EconomySim::get_benchmark_city_count);
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
	static const ModeSpec kTruck{
		TransportMode::Truck, "truck", 65.0, 25.0, 0.10, 6.0, 0.00004, 0.002};
	static const ModeSpec kShip{
		TransportMode::Ship, "ship", 35.0, 2000.0, 0.02, 24.0, 0.000003, 0.001};
	static const ModeSpec kPlane{
		TransportMode::Plane, "plane", 750.0, 20.0, 0.80, 24.0, 0.0002, 0.015};

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

void EconomySim::initialize(const Array& cities, const Array& ports) {
	in_transit_.clear();
	last_orders_.clear();
	day_ = 0;
	clock_hours_ = 0.0;
	cities_.clear();
	ports_.clear();

	for (int i = 0; i < cities.size(); ++i) {
		const Dictionary d = cities[i];
		City city;
		city.id = String(d["id"]).utf8().get_data();
		city.nation = String(d["nation"]).utf8().get_data();
		city.home_nation = String(d["home_nation"]).utf8().get_data();
		city.name = String(d["name"]).utf8().get_data();
		city.population = safe_nonneg(static_cast<double>(d["population"]));
		city.simulation_population = read_sim_population(d, city.population);
		city.production = safe_nonneg(static_cast<double>(d["production"]));
		city.stockpile = 0.0;
		city.center = MapPoint{static_cast<double>(d["x"]), static_cast<double>(d["y"])};
		city.landmass = String(d["landmass"]).utf8().get_data();
		cities_.push_back(std::move(city));
	}

	for (int i = 0; i < ports.size(); ++i) {
		const Dictionary d = ports[i];
		Port port;
		port.id = String(d["id"]).utf8().get_data();
		port.name = String(d["name"]).utf8().get_data();
		port.nation = String(d["nation"]).utf8().get_data();
		port.center = MapPoint{static_cast<double>(d["x"]), static_cast<double>(d["y"])};
		port.landmass = String(d["landmass"]).utf8().get_data();
		ports_.push_back(std::move(port));
	}
}

Dictionary EconomySim::tick() {
	++day_;
	clock_hours_ += kHoursPerDay;
	last_orders_.clear();

	// 1. Descarrega o que chegou à cidade de destino neste dia.
	for (auto shipment = in_transit_.begin(); shipment != in_transit_.end();) {
		if (shipment->arrival_hour <= clock_hours_) {
			if (City* city = find_city(shipment->to_city)) {
				city->stockpile += shipment->amount;
			}
			shipment = in_transit_.erase(shipment);
			continue;
		}
		++shipment;
	}

	// 2. Produção local e consumo de cada cidade.
	for (City& city : cities_) {
		city.stockpile += city.production;
		city.need = city.simulation_population * kNeedPerCapita;
		city.consumed = std::min(city.stockpile, city.need);
		city.stockpile -= city.consumed;
		city.deficit = city.need - city.consumed;
		city.dispatched = 0.0;

		// Indicadores derivados: quanto a cidade industrializou, quantos
		// empregos isso sustenta e a renda relativa (0..1) de quem vive nela.
		const double saturation = city.simulation_population * kTonsPerCapitaSaturation;
		city.industrialization = saturation > 0.0
			? std::clamp(city.production / saturation, 0.0, 1.0)
			: 0.0;
		city.jobs = city.production / kTonsPerJob;
		const double deficit_ratio = city.need > 0.0
			? std::clamp(city.deficit / city.need, 0.0, 1.0)
			: 0.0;
		city.income = safe_unit(
			kIncomeFloor + kIncomeIndustrialWeight * city.industrialization
				- kIncomeDeficitWeight * deficit_ratio);

		// Segurança numérica: o estado nunca carrega valor não finito.
		city.stockpile = safe_nonneg(city.stockpile);
		city.need = safe_nonneg(city.need);
		city.consumed = safe_nonneg(city.consumed);
		city.deficit = safe_nonneg(city.deficit);
		city.jobs = safe_nonneg(city.jobs);
		city.income = safe_unit(city.income);
		city.industrialization = safe_unit(city.industrialization);
		city.dispatched = safe_nonneg(city.dispatched);
	}

	// 3. Cidades com excedente vendem para cidades com déficit. O que já está
	//    em trânsito conta como cobertura.
	std::vector<City*> sellers;
	std::vector<std::pair<City*, double>> buyers;
	for (City& city : cities_) {
		if (city.stockpile > 1e-9) {
			sellers.push_back(&city);
		}
		const double pending = in_transit_total(city.id);
		// A cidade tenta manter kCoverDays de cobertura (estoque + em trânsito),
		// em vez de cobrir apenas o déficit diário.
		const double target = city.need * kCoverDays;
		const double open_deficit = target - (city.stockpile + pending);
		if (open_deficit > 1e-9) {
			buyers.emplace_back(&city, open_deficit);
		}
	}

	// Compradores mais urgentes primeiro; vendedores com mais trigo primeiro.
	// stable_sort garante ordem determinística entre empates (mesmo binário e
	// entre plataformas), essencial para replay/save idênticos.
	std::stable_sort(buyers.begin(), buyers.end(),
	          [](const auto& left, const auto& right) { return left.second > right.second; });
	std::stable_sort(sellers.begin(), sellers.end(),
	          [](const City* left, const City* right) { return left->stockpile > right->stockpile; });

	// Fase 1: mercado regional (cidades vizinhas do mesmo país).
	// Fase 2: mercado nacional (resto do país).
	// Fase 3: mercado internacional.
	const MarketScope scope_order[] = {
		MarketScope::Regional,
		MarketScope::National,
		MarketScope::International,
	};

	for (MarketScope scope : scope_order) {
		for (auto& [buyer, open_deficit] : buyers) {
			double remaining = open_deficit - buyer->dispatched;
			while (remaining > 1e-9 && last_orders_.size() < kMaxOrdersPerDay) {
				if (!dispatch_trade(*buyer, remaining, sellers, scope)) {
					break;
				}
			}
		}
	}

	Dictionary result;
	result["day"] = day_;

	Array cities_array;
	for (const City& city : cities_) {
		cities_array.append(city_to_dict(city));
	}
	result["cities"] = cities_array;

	Array ports_array;
	for (const Port& port : ports_) {
		ports_array.append(port_to_dict(port));
	}
	result["ports"] = ports_array;

	Array nations_array;
	for (const char* tag : {"GBR", "FRA", "DEU", "NED", "BEL"}) {
		nations_array.append(nation_to_dict(tag));
	}
	result["nations"] = nations_array;

	Array orders_array;
	for (const TradeOrder& order : last_orders_) {
		orders_array.append(order_to_dict(order));
	}
	result["trade_orders"] = orders_array;

	return result;
}

Dictionary EconomySim::get_status() const {
	Dictionary result;
	result["mode"] = "legacy";
	result["day"] = day_;
	result["calendar_day"] = day_;

	Array cities_array;
	for (const City& city : cities_) {
		cities_array.append(city_to_dict(city));
	}
	result["cities"] = cities_array;

	Array ports_array;
	for (const Port& port : ports_) {
		ports_array.append(port_to_dict(port));
	}
	result["ports"] = ports_array;

	Array nations_array;
	for (const char* tag : {"GBR", "FRA", "DEU", "NED", "BEL"}) {
		nations_array.append(nation_to_dict(tag));
	}
	result["nations"] = nations_array;

	return result;
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

bool EconomySim::in_scope(const City& buyer, const City& seller, MarketScope scope) const {
	const bool same_nation = buyer.nation == seller.nation;

	switch (scope) {
		case MarketScope::International:
			return !same_nation;
		case MarketScope::Regional:
			return same_nation && distance_km(buyer.center, seller.center) <= kRegionalRadiusKm;
		case MarketScope::National:
			return same_nation && distance_km(buyer.center, seller.center) > kRegionalRadiusKm;
	}
	return false;
}

bool EconomySim::dispatch_trade(City& buyer, double& remaining, const std::vector<City*>& sellers,
                                MarketScope scope) {
	// Dentro da camada, o frete mais barato é o do vendedor mais próximo.
	City* seller = nullptr;
	double best_distance = 0.0;
	for (City* candidate : sellers) {
		if (candidate->stockpile <= 1e-9 || !in_scope(buyer, *candidate, scope)) {
			continue;
		}
		const double candidate_distance = distance_km(buyer.center, candidate->center);
		if (seller == nullptr || candidate_distance < best_distance) {
			seller = candidate;
			best_distance = candidate_distance;
		}
	}
	if (seller == nullptr) {
		return false;
	}

	const double amount = std::min(remaining, seller->stockpile);
	if (amount <= 1e-9) {
		return false;
	}

	const double cover_days = buyer.stockpile / std::max(buyer.need, 1.0);
	const double air_share = (cover_days < kEmergencyCoverDays) ? kEmergencyAirShare : 0.0;

	TradeOrder order;
	order.from_city = seller->id;
	order.to_city = buyer.id;
	order.from_nation = seller->nation;
	order.to_nation = buyer.nation;
	order.scope = scope;
	order.amount = amount;
	order.legs = build_route(*seller, buyer, amount, air_share);

	for (const Leg& leg : order.legs) {
		order.freight_cost += leg.cost;
		order.transit_hours += leg.transit_hours;
	}

	seller->stockpile -= amount;
	buyer.dispatched += amount;
	remaining -= amount;

	in_transit_.push_back({buyer.id, amount, clock_hours_ + order.transit_hours});
	last_orders_.push_back(std::move(order));
	return true;
}

std::vector<EconomySim::Leg> EconomySim::build_route(const City& seller, const City& buyer,
                                                     double amount, double air_share) const {
	const bool crosses_sea = seller.landmass != buyer.landmass;

	const double air_tons = amount * std::clamp(air_share, 0.0, 1.0);
	const double surface_tons = amount - air_tons;

	std::vector<Leg> legs;
	if (crosses_sea) {
		legs = build_overseas_route(seller, buyer, surface_tons);
	} else {
		legs.push_back(make_leg(TransportMode::Truck, seller.name, buyer.name,
		                        seller.center, buyer.center, surface_tons));
	}

	if (air_tons > 1e-9) {
		// A ponte aérea vai direto da origem ao destino, sem passar por porto.
		legs.push_back(make_leg(TransportMode::Plane, seller.name, buyer.name,
		                        seller.center, buyer.center, air_tons));
	}

	return legs;
}

std::vector<EconomySim::Leg> EconomySim::build_overseas_route(const City& seller, const City& buyer,
                                                             double amount) const {
	// Vendedor -> porto dele (mesma nação/massa) -> navio -> porto do comprador
	// -> comprador. Sem porto da nação, não há travessia marítima (estratégico).
	const Port* seller_port = nearest_port(seller.nation, seller.landmass, seller.center);
	const Port* buyer_port = nearest_port(buyer.nation, buyer.landmass, buyer.center);
	if (seller_port == nullptr || buyer_port == nullptr) {
		return {};
	}

	std::vector<Leg> legs;
	const double kSamePointKm = 1.0;
	if (distance_km(seller.center, seller_port->center) > kSamePointKm) {
		legs.push_back(make_leg(TransportMode::Truck, seller.name, seller_port->name,
		                        seller.center, seller_port->center, amount));
	}
	legs.push_back(make_leg(TransportMode::Ship, seller_port->name, buyer_port->name,
	                        seller_port->center, buyer_port->center, amount));
	if (distance_km(buyer_port->center, buyer.center) > kSamePointKm) {
		legs.push_back(make_leg(TransportMode::Truck, buyer_port->name, buyer.name,
		                        buyer_port->center, buyer.center, amount));
	}
	return legs;
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
	const double hours = distance / mode_spec.speed_kmh + mode_spec.turnaround_hours
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

double EconomySim::in_transit_total(const std::string& city_id) const {
	double total = 0.0;
	for (const Shipment& shipment : in_transit_) {
		if (shipment.to_city == city_id) {
			total += shipment.amount;
		}
	}
	return total;
}

const EconomySim::Port* EconomySim::nearest_port(const std::string& nation,
                                                 const std::string& landmass,
                                                 const MapPoint& from) const {
	const Port* best = nullptr;
	double best_d = 0.0;
	for (const Port& port : ports_) {
		if (port.nation != nation || port.landmass != landmass) {
			continue;
		}
		const double d = distance_km(from, port.center);
		if (best == nullptr || d < best_d) {
			best = &port;
			best_d = d;
		}
	}
	return best;
}

EconomySim::City* EconomySim::find_city(const std::string& id) {
	for (City& city : cities_) {
		if (city.id == id) {
			return &city;
		}
	}
	return nullptr;
}

const EconomySim::City* EconomySim::find_city(const std::string& id) const {
	for (const City& city : cities_) {
		if (city.id == id) {
			return &city;
		}
	}
	return nullptr;
}

Dictionary EconomySim::city_to_dict(const City& city) const {
	Dictionary dict;
	dict["id"] = String(city.id.c_str());
	dict["nation"] = String(city.nation.c_str());
	dict["home_nation"] = String(city.home_nation.c_str());
	dict["name"] = String(city.name.c_str());
	dict["population"] = safe_value(city.population);
	dict["sim_population"] = safe_value(city.simulation_population);
	dict["need"] = safe_value(city.need);
	dict["production"] = safe_value(city.production);
	dict["consumed"] = safe_value(city.consumed);
	dict["deficit"] = safe_value(city.deficit);
	dict["dispatched"] = safe_value(city.dispatched);
	dict["in_transit"] = safe_value(in_transit_total(city.id));
	dict["stockpile"] = safe_value(city.stockpile);
	dict["jobs"] = safe_value(city.jobs);
	dict["income"] = safe_value(city.income);
	dict["industrialization"] = safe_value(city.industrialization);
	dict["x"] = safe_value(city.center.x);
	dict["y"] = safe_value(city.center.y);
	return dict;
}

Dictionary EconomySim::port_to_dict(const Port& port) const {
	Dictionary dict;
	dict["id"] = String(port.id.c_str());
	dict["name"] = String(port.name.c_str());
	dict["nation"] = String(port.nation.c_str());
	dict["x"] = port.center.x;
	dict["y"] = port.center.y;
	dict["landmass"] = String(port.landmass.c_str());
	return dict;
}

Dictionary EconomySim::nation_to_dict(const std::string& tag) const {
	static const std::pair<const char*, const char*> kNames[] = {
		{"GBR", "England"}, {"FRA", "France"}, {"DEU", "Germany"},
		{"NED", "Netherlands"}, {"BEL", "Belgium"},
	};

	Dictionary dict;
	dict["tag"] = String(tag.c_str());

	const char* display = tag.c_str();
	for (const auto& [code, name] : kNames) {
		if (tag == code) {
			display = name;
		}
	}
	dict["name"] = String(display);

	double population = 0.0;
	double production = 0.0;
	double stockpile = 0.0;
	double need = 0.0;
	double deficit = 0.0;
	double dispatched = 0.0;
	double in_transit = 0.0;
	int city_count = 0;

	for (const City& city : cities_) {
		if (city.nation != tag) {
			continue;
		}
		++city_count;
		population += city.population;
		production += city.production;
		stockpile += city.stockpile;
		need += city.need;
		deficit += city.deficit;
		dispatched += city.dispatched;
		in_transit += in_transit_total(city.id);
	}

	dict["city_count"] = city_count;
	dict["population"] = population;
	dict["production"] = production;
	dict["stockpile"] = stockpile;
	dict["need"] = need;
	dict["deficit"] = deficit;
	dict["dispatched"] = dispatched;
	dict["in_transit"] = in_transit;
	return dict;
}

Dictionary EconomySim::leg_to_dict(const Leg& leg) const {
	const ModeSpec& mode_spec = spec(leg.mode);

	Dictionary dict;
	dict["mode"] = String(mode_spec.name);
	dict["from"] = String(leg.from.c_str());
	dict["to"] = String(leg.to.c_str());
	dict["from_px"] = Vector2(leg.from_point.x, leg.from_point.y);
	dict["to_px"] = Vector2(leg.to_point.x, leg.to_point.y);
	dict["distance_km"] = leg.distance_km;
	dict["load_tons"] = leg.load_tons;
	dict["trips"] = leg.trips;
	dict["transit_hours"] = leg.transit_hours;
	dict["cost"] = leg.cost;
	dict["speed_kmh"] = mode_spec.speed_kmh;
	dict["batch_capacity_tons"] = mode_spec.batch_capacity_tons;
	return dict;
}

// Versão do status: mesmo conteúdo de leg_to_dict, mas escrevendo em um
// Dictionary reaproveitado e com chaves StringName (sem hash por escrita).
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

Dictionary EconomySim::order_to_dict(const TradeOrder& order) const {
	Dictionary dict;
	dict["from_city"] = String(order.from_city.c_str());
	dict["to_city"] = String(order.to_city.c_str());
	dict["from_nation"] = String(order.from_nation.c_str());
	dict["to_nation"] = String(order.to_nation.c_str());
	dict["scope"] = String(scope_name(order.scope));
	dict["good"] = "wheat";
	dict["amount"] = order.amount;
	dict["transit_hours"] = order.transit_hours;
	dict["freight_cost"] = order.freight_cost;

	Array legs;
	for (const Leg& leg : order.legs) {
		legs.append(leg_to_dict(leg));
	}
	dict["legs"] = legs;
	return dict;
}

// ---------------------------------------------------------------------------
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
// a média mundial ponderada por população continue nesta base. Um subsídio
// igual para todos seria o resto do mundo pagando o mesmo a Noruega e ao
// Burundi — nem é renda de serviços, nem é exportação.
constexpr double kDemoExternalIncomePerCapita = 0.012;
constexpr double kDemoLaborShare = 0.55;
constexpr double kDemoServiceShare = 0.30;
constexpr double kDemoServiceWage = 0.15;
constexpr double kDemoServiceSpendShare = 0.45;
constexpr double kDemoTaxRate = 0.15;
constexpr double kDemoTransferShare = 1.0;
constexpr double kDemoCapitalDistribution = 1.0;
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

void EconomySim::demo_set_goods(const Array& goods) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	// Tabela de produtos: 21 primários nas posições 0..20 (ordem do goods.json
	// legado) e os bens novos em `order` (21..113). O núcleo não lê arquivos:
	// todo o conteúdo entra por aqui, no padrão do demo_set_goods antigo.
	const int previous_products = demo_products_;
	std::vector<std::string> names = demo_product_names_;
	std::vector<double> prices(kDemoMaxProducts, 0.0);
	std::vector<char> durable(kDemoMaxProducts, 0);
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

	// Mundo já inicializado: os produtos novos entram com o preço-base e sem
	// estoque. Os índices existentes (0..previous_products-1) não mudam.
	if (demo_active_ && product_count > previous_products) {
		for (DemoCity& city : demo_cities_) {
			for (int product = previous_products; product < product_count; ++product) {
				city.markets[product].price = demo_base_prices_[product];
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
	}
	demo_rebuild_indexes();
	// F4: o cenário mínimo também tem frota (nação DEMO).
	demo_init_fleets();
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
		// Aptidão padrão (inicializador do struct: suitability = {1.0, 1.0, 1.0,
		// 0, ...} — trigo/ferro/arroz em 1.0). O GeographyService sobrescreve com
		// o dado real em tempo de execução.
		city.suitability[0] = 1.0;
		for (int product = 0; product < demo_products_; ++product) {
			city.markets[product] = {0.0, demo_base_price(product), 0.0, 0.0};
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
	return total;
}

void EconomySim::demo_snapshot(DemoSnapshot& out) const {
	out.cities = demo_cities_;
	out.buildings = demo_buildings_;
	out.transit = demo_in_transit_;
	out.fleets = demo_fleets_;
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
	demo_in_transit_ = in.transit;
	demo_fleets_ = in.fleets;
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

		for (int product = 0; product < demo_products_; ++product) {
			const DemoMarket& market = city.markets[product];
			const double base = demo_base_price(product);
			if (!std::isfinite(market.price) || market.price < base * 0.5 - 1e-6
					|| market.price > base * 3.0 + 1e-6) {
				violations.push_back("preço fora da banda em " + city.id + " "
					+ demo_product_name(product) + ": " + std::to_string(market.price));
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
			const double price = demo_cities_[si].markets[product].price;
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
				< demo_cities_[static_cast<std::size_t>(r)].markets[product].price;
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
			double price;
			double unit_freight;
			double unit_cost_per_ge;
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
				const double unit_cost_per_ge = (price + unit_freight) / factor;
				offers.push_back({si, product, factor, price,
					unit_freight, unit_cost_per_ge});
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
			const double price = offer.price;
			const double unit_freight = offer.unit_freight;
			const double unit_cost = price + unit_freight;
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
			const double cost = amount_tons * price;
			const double freight = unit_freight * amount_tons;

			buyer.city->firm_cash -= firm_share;
			buyer.city->government_budget -= gov_share;
			buyer.city->pop.savings = std::max(
				0.0, buyer.city->pop.savings - savings_share);
			seller->firm_cash += cost;
			buyer.city->service_cash += freight;
			buyer.city->freight_revenue += freight;

			seller->markets[product].stock -= amount_tons;
			seller->markets[product].desired += amount_tons;
			seller->markets[product].demand += amount_tons;
			seller->markets[product].sold += amount_tons;
			seller->revenue_by_product[product] += cost;
			seller->exported[product] += amount_tons;
			seller->export_value[product] += cost;

			buyer.city->imported[product] += amount_tons;
			buyer.city->import_value[product] += cost;

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
			order.scope = MarketScope::International;
			order.product = product;
			order.amount = amount_tons;

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
				sellers.push_back({static_cast<int>(ci), sellable, market.price});
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
				double unit_cost;
				double unit_freight;
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
				offers.push_back({si, seller.markets[product].price + unit_freight,
					unit_freight});
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
				const double unit_price = seller.markets[product].price;
				const double unit_cost = unit_price + offer.unit_freight;
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
				const double goods_value = amount * unit_price;
				const double freight = amount * offer.unit_freight;
				buyer_city.firm_cash -= firm_share;
				buyer_city.government_budget -= gov_share;
				seller.firm_cash += goods_value;
				buyer_city.service_cash += freight;
				buyer_city.freight_revenue += freight;

				seller.markets[product].stock -= amount;
				seller.markets[product].desired += amount;
				seller.markets[product].demand += amount;
				seller.markets[product].sold += amount;
				seller.revenue_by_product[product] += goods_value;
				seller.exported[product] += amount;
				seller.export_value[product] += goods_value;
				buyer_city.imported[product] += amount;
				buyer_city.import_value[product] += goods_value;

				TradeOrder order;
				order.from_city = seller.id;
				order.to_city = buyer_city.id;
				order.from_nation = seller.nation;
				order.to_nation = buyer_city.nation;
				order.scope = seller.nation == buyer_city.nation
					? MarketScope::Regional : MarketScope::International;
				order.product = product;
				order.amount = amount;
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
		double price = 0.0;
		double stock = 0.0;
	};
	std::vector<Seller> sellers;
	sellers.reserve(cities_found->second.size());
	for (const int index : cities_found->second) {
		const DemoCity& city = demo_cities_[static_cast<std::size_t>(index)];
		const DemoMarket& market = city.markets[product_index];
		if (market.stock > 1e-9 && market.price > 0.0) {
			sellers.push_back({index, market.price, market.stock});
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
		if (quantity * market.price > budget - spent) {
			quantity = (budget - spent) / market.price;
		}
		if (quantity <= 1e-9) {
			continue;
		}
		const double cost = quantity * market.price;
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
	demo_tick_core();
	using Clock = std::chrono::steady_clock;
	const Clock::time_point start = demo_profile_ ? Clock::now() : Clock::time_point{};
	get_demo_status();
	if (demo_profile_) {
		const Clock::time_point end = Clock::now();
		demo_profile_us_[kDemoPhaseStatus] =
			std::chrono::duration<double, std::micro>(end - start).count();
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
		"arrival", "reset", "labor", "production", "raw", "income", "trade",
		"accounting", "prices", "clamp", "fiscal", "invariants", "status",
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
	using Clock = std::chrono::steady_clock;
	Clock::time_point lap_mark = demo_profile_ ? Clock::now() : Clock::time_point{};
	auto lap = [this, &lap_mark](int phase) {
		if (!demo_profile_) {
			return;
		}
		const Clock::time_point now = Clock::now();
		demo_profile_us_[phase] =
			std::chrono::duration<double, std::micro>(now - lap_mark).count();
		lap_mark = now;
	};

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

	lap(kDemoPhaseArrival);

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
		city.service_wage_bill = 0.0;
		city.service_revenue = 0.0;
		city.service_profit = 0.0;
		city.opex_revenue = 0.0;
		city.freight_revenue = 0.0;
		city.construction_revenue = city.construction_revenue_pending;
		city.construction_revenue_pending = 0.0;
		city.gdp = 0.0;
		for (int product = 0; product < demo_products_; ++product) {
			city.markets[product].demand = 0.0;
			city.markets[product].supply = 0.0;
			city.markets[product].desired = 0.0;
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

	lap(kDemoPhaseReset);

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
		double primary_jobs = 0.0;
		double secondary_jobs = 0.0;
		for (const int bi : local_buildings) {
			const DemoBuilding& building = demo_buildings_[static_cast<std::size_t>(bi)];
			// Primária = extração/fazenda (sem receita industrial). Fábricas
			// (receita ou insumo legado) são secundárias.
			if (building.recipe < 0 && building.input_product < 0) {
				primary_jobs += building.jobs;
			} else {
				secondary_jobs += building.jobs;
			}
		}
		// O emprego de serviços é persistente: cresce ou recua conforme a
		// receita do setor, em vez de ser recalculado a cada dia.
		const double labor_force = city.simulation_population * kDemoLaborShare;
		city.pop.labor_force = labor_force;

		// §5.3 — alocação de trabalho em duas classes. A fazenda/extaração
		// (necessidade básica) tem prioridade; a fábrica só usa a mão de obra
		// que sobra. Antes um `fill` único diluía as fazendas a cada fábrica
		// nova e a comida caía junto com a industrialização.
		const double available = std::max(0.0, labor_force - city.service_jobs);
		double primary_fill = 1.0;
		double secondary_fill = 0.0;
		if (primary_jobs > 0.0) {
			primary_fill = std::clamp(available / primary_jobs, 0.0, 1.0);
			secondary_fill = available > primary_jobs && secondary_jobs > 0.0
				? std::clamp((available - primary_jobs) / secondary_jobs, 0.0, 1.0)
				: 0.0;
		} else if (secondary_jobs > 0.0) {
			secondary_fill = std::clamp(available / secondary_jobs, 0.0, 1.0);
		}

		double modeled_wages = 0.0;
		double opex_total = 0.0;
		for (const int bi : local_buildings) {
			DemoBuilding& building = demo_buildings_[static_cast<std::size_t>(bi)];
			const bool secondary = building.recipe >= 0 || building.input_product >= 0;
			const double fill = secondary ? secondary_fill : primary_fill;
			building.workers = building.jobs * fill;
			// Planta secundária ociosa não paga a folha/opex cheios: os custos
			// acompanham a operação do dia anterior (piso de 5% de stand-by).
			const double operating = secondary
				? std::clamp(building.last_utilization, 0.05, 1.0) : 1.0;
			modeled_wages += building.workers * building.wage * operating;
			opex_total += building.opex * opex_factor(city, building) * operating;
		}

		// Restrição de liquidez: a empresa não paga folha nem opex com dinheiro
		// que não tem. Sem isso, cenários deficitários acumulavam caixa negativo
		// para sempre e a contabilidade de renda deixava de ter lastro.
		const double firm_available = std::max(0.0, city.firm_cash);
		const double committed = modeled_wages + opex_total;
		const double pay_scale = committed > firm_available && committed > 0.0
			? firm_available / committed : 1.0;
		modeled_wages *= pay_scale;
		opex_total *= pay_scale;
		for (const int bi : local_buildings) {
			DemoBuilding& building = demo_buildings_[static_cast<std::size_t>(bi)];
			const bool secondary = building.recipe >= 0 || building.input_product >= 0;
			const double operating = secondary
				? std::clamp(building.last_utilization, 0.05, 1.0) : 1.0;
			building.paid_wages = building.workers * building.wage * operating * pay_scale;
			building.paid_opex = building.opex * opex_factor(city, building)
				* operating * pay_scale;
		}

		city.service_wage_bill = city.service_jobs * city.service_wage;
		// O setor de serviços não paga folha sem caixa: a folha é limitada ao
		// saldo disponível no início do dia, sem crédito implícito.
		city.service_wage_bill = std::min(city.service_wage_bill,
				std::max(0.0, city.service_cash));
		city.pop.employed = city.service_jobs
			+ primary_jobs * primary_fill + secondary_jobs * secondary_fill;
		city.pop.unemployed = std::max(0.0, labor_force - city.pop.employed);
		city.pop.dependents =
			std::max(0.0, city.simulation_population - labor_force);
		city.pop.wage_income = modeled_wages + city.service_wage_bill;

		city.firm_cash -= modeled_wages + opex_total;
		city.service_cash -= city.service_wage_bill;
		// Opex é comprado de serviços domésticos (manutenção, energia),
		// não é uma importação: mantém o dinheiro dentro da economia.
		city.service_cash += opex_total;
		city.opex_revenue = opex_total;
	}

	lap(kDemoPhaseLabor);

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

	lap(kDemoPhaseProduction);

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

	lap(kDemoPhaseRaw);

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
		pop.external_income = city.simulation_population
			* kDemoExternalIncomePerCapita * city.external_income_index;
		pop.disposable += pop.external_income;
		demo_foreign_cash_ -= pop.external_income;
		// A renda externa é renda de subsistência: o gasto discricionário em
		// serviços incide só sobre a renda DOMÉSTICA. Sem isso a cidade sem
		// terra gastava 45% da renda externa em serviços e sobrava menos que a
		// cesta (TKM ficava em 64% em 730 d, §20).
		pop.service_spend = std::max(0.0,
			pop.disposable - pop.external_income) * kDemoServiceSpendShare;
		city.service_cash += pop.service_spend;
		city.government_budget += pop.taxes - pop.transfers;

		double budget = pop.savings + pop.disposable - pop.service_spend;
		// Necessidade alimentar em toneladas-equivalentes de grão (B3).
		pop.need = city.simulation_population * kDemoNeedPerCapita;
		// Subsídio de subsistência: o governo garante a cesta alimentar mínima
		// quando o orçamento de comida não cobre (ex.: cidade sem produção nem
		// renda própria). A cesta usa o alimento mais barato por caloria
		// (preço / fator calórico); com os preços-base, o trigo segue o mais
		// barato. Sai do orçamento público — conserva o dinheiro.
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
		const double food_cost = pop.need * cheapest_food;
		double food_topup = 0.0;
		if (cheapest_food > 0.0 && budget < food_cost) {
			food_topup = std::min(food_cost - budget,
				std::max(0.0, city.government_budget));
			if (food_topup > 0.0) {
				budget += food_topup;
				city.government_budget -= food_topup;
				pop.transfers += food_topup;
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
		pop.consumed = consumed;
		pop.food_spend = food_spend;
		pop.satisfaction = pop.need > 0.0
			? std::clamp(consumed / pop.need, 0.0, 1.0) : 1.0;

		// Subsídio de subsistência não gasto volta ao governo: sem isso, a
		// cidade sem estoque de alimento convertia o subsídio em poupança e ele
		// acabava financiando life/luxury (dinheiro conservado, mas fora do
		// destino). Devolver mantém o piso alimentar e a prioridade basic.
		if (food_topup > 0.0) {
			const double unused = std::min(food_topup, std::max(0.0, budget));
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
			if (cheapest_food > 0.0) {
				const double reserve = pop.need * kDemoFoodReserveDays * cheapest_food;
				// Protege no máximo a poupança existente: se ela já é menor que
				// o colchão, a renda corrente do dia continua disponível.
				const double protected_savings =
					std::min(std::max(0.0, pop.savings), reserve);
				needs_budget = std::max(0.0, budget - protected_savings);
			}
			double needs_spent = 0.0;
			double weighted = 0.0;
			double weight_sum = 0.0;
			for (int level_index = 0;
					level_index < static_cast<int>(demo_needs_levels_.size())
					&& level_index < kDemoMaxNeedsLevels; ++level_index) {
				const DemoNeedsLevel& level =
					demo_needs_levels_[static_cast<std::size_t>(level_index)];
				double level_want = 0.0;
				double level_got = 0.0;
				for (const DemoNeedsCategory& category : level.categories) {
					// A cesta alimentar básica tem laço próprio (validado) e já
					// foi consumida acima; reaproveita `pop.need/consumed` para
					// não cobrar duas vezes a mesma refeição.
					if (level_index == demo_needs_food_level_
							&& category.id == std::string("food")) {
						level_want += pop.need;
						level_got += pop.consumed;
						continue;
					}
					const double want_ge = category.per_capita_day
						* city.simulation_population;
					level_want += want_ge;
					level_got += demo_consume_category(
						city, category, want_ge, needs_budget, needs_spent);
				}
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
		} else {
			// Sem needs.json (AGRIA/PORTUS e saves antigos): um único nível,
			// exatamente o comportamento validado.
			pop.needs_satisfaction = pop.satisfaction;
			pop.level_want[0] = pop.need;
			pop.level_consumed[0] = pop.consumed;
			pop.level_satisfaction[0] = pop.satisfaction;
			pop.needs_level = pop.satisfaction >= 0.5 ? 1 : 0;
		}
		// Consumo institucional do Estado (paper/computer) — decisão §12.10.
		demo_consume_government(city);
		pop.savings = budget;
	}

	lap(kDemoPhaseIncome);

	// 6. Comércio de alimento (cesta de calorias) entre cidades: cidades com
	// déficit compram das que têm excedente, mais barato por caloria. Cobre tanto
	// o cenário de validação (AGRIA/PORTUS) quanto o mundo real.
	demo_trade_food();
	// 6b. Comércio dos demais bens (insumos industriais), pela mesma malha.
	demo_trade_goods();

	lap(kDemoPhaseTrade);

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
		// frete e construção: são serviços domésticos, não importações.
		city.service_revenue = city.pop.service_spend + city.opex_revenue
			+ city.freight_revenue + city.construction_revenue;
		city.service_profit = city.service_revenue - city.service_wage_bill;

		double private_profit = 0.0;
		double state_profit = 0.0;
		double value_added = 0.0;
		for (const int bi : local_buildings) {
			const DemoBuilding& building =
				demo_buildings_[static_cast<std::size_t>(bi)];
			if (building.owner == "state") {
				state_profit += building.profit;
			} else {
				private_profit += building.profit;
			}
			value_added += building.revenue - building.input_value;
		}

		// Dividendos só depois de reconstituir o capital de giro. Lucros de
		// estatais vão para o orçamento público; privados, para as famílias.
		const double firm_target =
			city.simulation_population * kDemoFirmCashPerCapita;
		const double firm_surplus = std::max(0.0, city.firm_cash - firm_target);
		const double positive_total = std::max(0.0, private_profit)
			+ std::max(0.0, state_profit);
		const double distributable = std::min(
			positive_total * kDemoCapitalDistribution, firm_surplus);
		const double private_share = positive_total > 0.0
			? std::max(0.0, private_profit) / positive_total : 0.0;
		const double modeled_distribution = distributable * private_share;
		const double state_distribution = distributable - modeled_distribution;
		const double service_target =
			city.simulation_population * kDemoServiceCashPerCapita;
		const double service_surplus = std::max(0.0,
			city.service_cash - service_target);
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
		if (service_supply > 0.0) {
			const double wage_target = city.service_revenue / service_supply;
			const double bounded_wage = std::clamp(
				std::max(wage_target, subsistence_wage),
				0.0, demo_service_wage_max_);
			city.service_wage += (bounded_wage - city.service_wage)
				* demo_service_wage_adjust_;
		}
		if (city.service_wage > 0.0) {
			const double affordable_jobs = city.service_revenue / city.service_wage;
			const double bounded = std::min(service_supply, affordable_jobs);
			city.service_jobs += (bounded - city.service_jobs) * kDemoServiceAdjust;
		}
	}

	lap(kDemoPhaseAccounting);

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
			if (!has_producer[product]) {
				// Sem produção local o preço vem da importação (trigo em Portus).
				continue;
			}
			DemoMarket& market = city.markets[product];
			const double base = demo_base_price(product);
			// Pressão usa a demanda desejada: compras não atendidas por falta
			// de estoque continuam pressionando o preço.
			const double scarcity = market.desired
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
			// de capacidade. Receitas alternativas usam o custo dos insumos da
			// receita escolhida (a de menor custo corrente na cidade).
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

	lap(kDemoPhasePrices);

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

	lap(kDemoPhaseClamp);

	// 9b. Redistribuição fiscal nacional (federalismo fiscal).
	demo_national_fiscal_transfer();

	lap(kDemoPhaseFiscal);

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
	lap(kDemoPhaseInvariants);
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
		efficiency("efficiency"),
		employed("employed"),
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
		industrialization("industrialization"),
		initial_kt("initial_kt"),
		input_product("input_product"),
		input_value("input_value"),
		invariant_violations("invariant_violations"),
		jobs("jobs"),
		kind("kind"),
		labor_force("labor_force"),
		landmass("landmass"),
		legs("legs"),
		living_standard("living_standard"),
		loss_days("loss_days"),
		market_totals("market_totals"),
		markets("markets"),
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
		produced_today("produced_today"),
		produced_total("produced_total"),
		product("product"),
		production("production"),
		products("products"),
		profit("profit"),
		quality("quality"),
		remaining_kt("remaining_kt"),
		resource("resource"),
		revenue("revenue"),
		satisfaction("satisfaction"),
		savings("savings"),
		scope("scope"),
		service_cash("service_cash"),
		service_profit("service_profit"),
		service_revenue("service_revenue"),
		service_spend("service_spend"),
		service_wage("service_wage"),
		shortage("shortage"),
		simulation_population("simulation_population"),
		site_suitability("site_suitability"),
		sold("sold"),
		stock("stock"),
		stockpile("stockpile"),
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
		utilization("utilization"),
		wage("wage"),
		wage_income("wage_income"),
		workers("workers"),
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
		target_planes("target_planes")
{}

void EconomySim::demo_fill_city_status(std::size_t city_index,
		DemoStatusCitySlot& slot, double in_transit) const {
	const DemoCity& city = demo_cities_[city_index];
	const DemoStatusKeys& k = demo_status_keys_;
	Dictionary& dict = slot.dict;

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
	slot.scalar_written_filled = true;

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
		slot.io_written.assign(static_cast<std::size_t>(4 * demo_products_), 0.0);
		slot.io_filled.assign(4, 0);
	}
	bool markets_dirty = false;
	for (int product = 0; product < demo_products_; ++product) {
		const DemoMarket& source = city.markets[product];
		DemoMarket& previous = slot.market_written[static_cast<std::size_t>(product)];
		if (slot.market_written_filled[static_cast<std::size_t>(product)] == 0) {
			// Primeira gravação desta cidade: cria as chaves do mercado.
			Dictionary& market = slot.market_entry[static_cast<std::size_t>(product)];
			market[k.product] = product;
			market[k.stock] = source.stock;
			market[k.price] = source.price;
			market[k.demand] = source.demand;
			market[k.supply] = source.supply;
			market[k.desired] = source.desired;
			market[k.sold] = source.sold;
			market[k.shortage] = source.shortage;
			market[k.surplus] = source.surplus;
			previous = source;
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
		if (std::memcmp(&previous, &source, sizeof(double) * 8) == 0) {
			continue;
		}
		markets_dirty = true;
		Dictionary& market = slot.market_entry[static_cast<std::size_t>(product)];
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
		// Bloco estático: só quando a construção nasce (ou muda de identidade).
		if (identity_changed) {
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

	// Subdivisao da fase status no benchmark (sem custo no jogo).
	using StatusClock = std::chrono::steady_clock;
	StatusClock::time_point status_mark =
		demo_profile_ ? StatusClock::now() : StatusClock::time_point{};
	auto status_lap = [this, &status_mark](int part) {
		if (!demo_profile_) {
			return;
		}
		const StatusClock::time_point now = StatusClock::now();
		demo_status_sub_us_[part] =
			std::chrono::duration<double, std::micro>(now - status_mark).count();
		status_mark = now;
	};

	root[k.mode] = "demo";
	root[k.day] = day_;
	root[k.calendar_day] = day_;
	root[k.money_total] = demo_money_total();
	root[k.transport_cash] = demo_transport_cash_;
	root[k.foreign_cash] = demo_foreign_cash_;
	root[k.construction_cash] = demo_construction_cash_;
	root[k.bankruptcies] = demo_bankruptcies_;
	root[k.depleted_mines] = demo_depleted_mines_;

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
	status_lap(3);

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
	status_lap(0);

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
	status_lap(1);

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
		cache.nations[index] = nation;
	}

	for (int index = 0; index < demo_products_; ++index) {
		cache.tot_produced[index] = demo_produced_[index];
		cache.tot_consumed[index] = demo_consumed_total_[index];
		cache.tot_produced_today[index] = demo_produced_today_[index];
		cache.tot_consumed_today[index] = demo_consumed_today_[index];
	}
	status_lap(2);
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

double EconomySim::demo_consume_category(DemoCity& city,
		const DemoNeedsCategory& category, double want_ge, double& budget,
		double& spend) {
	if (want_ge <= 0.0 || category.substitutes.empty()) {
		return 0.0;
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
		// Demanda desejada registrada mesmo sem estoque: é o sinal de escassez
		// que a IA lê (`market.shortage`) para abrir a fábrica do bem final.
		double desired_tons = remaining / factor;
		if (desired_tons * price > budget) {
			desired_tons = std::max(0.0, budget) / price;
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
		budget -= paid;
		spend += paid;
		city.revenue_by_product[product] += paid;
		city.firm_cash += paid;
		demo_consumed_total_[product] += quantity;
		demo_consumed_today_[product] += quantity;
		const double got_ge = quantity * factor;
		consumed_ge += got_ge;
		remaining = std::max(0.0, remaining - got_ge);
		const bool durable = product < static_cast<int>(demo_product_durable_.size())
			&& demo_product_durable_[static_cast<std::size_t>(product)] != 0;
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
	++demo_status_static_version_;
	demo_status_invalidate();
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

		Array suitability;
		Array revenue;
		Array imported;
		Array exported;
		Array import_value;
		Array export_value;
		Array external_demand;
		for (int product = 0; product < demo_products_; ++product) {
			suitability.append(city.suitability[product]);
			revenue.append(city.revenue_by_product[product]);
			imported.append(city.imported[product]);
			exported.append(city.exported[product]);
			import_value.append(city.import_value[product]);
			export_value.append(city.export_value[product]);
			external_demand.append(city.external_demand[product]);
		}
		entry["suitability"] = suitability;
		entry["revenue_by_product"] = revenue;
		entry["imported"] = imported;
		entry["exported"] = exported;
		entry["import_value"] = import_value;
		entry["export_value"] = export_value;
		entry["external_demand"] = external_demand;

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
		entry["output_scale"] = building.output_scale;
		entry["last_utilization"] = building.last_utilization;
		entry["strategic"] = building.strategic;
		entry["loss_days"] = building.loss_days;
		buildings.append(entry);
	}
	data["buildings"] = buildings;

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
	const bool needs_migration = version < 3;
	demo_active_ = bool(data.get("active", true));
	demo_world_mode_ = bool(data.get("world_mode", false));
	day_ = int(data.get("day", 0));
	demo_transport_cash_ = double(data.get("transport_cash", 0.0));
	demo_foreign_cash_ = double(data.get("foreign_cash", 0.0));
	demo_construction_cash_ = double(data.get("construction_cash", 0.0));

	Array produced = data.get("produced_total", Array());
	Array consumed = data.get("consumed_total", Array());
	if (needs_migration) {
		produced = demo_migrate_product_array(produced, demo_products_);
		consumed = demo_migrate_product_array(consumed, demo_products_);
	}
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

		Array suitability = entry.get("suitability", Array());
		Array revenue = entry.get("revenue_by_product", Array());
		Array imported = entry.get("imported", Array());
		Array exported = entry.get("exported", Array());
		Array import_value = entry.get("import_value", Array());
		Array export_value = entry.get("export_value", Array());
		Array external_demand = entry.get("external_demand", Array());
		if (needs_migration) {
			suitability = demo_migrate_product_array(suitability, demo_products_);
			revenue = demo_migrate_product_array(revenue, demo_products_);
			imported = demo_migrate_product_array(imported, demo_products_);
			exported = demo_migrate_product_array(exported, demo_products_);
			import_value = demo_migrate_product_array(import_value, demo_products_);
			export_value = demo_migrate_product_array(export_value, demo_products_);
			external_demand = demo_migrate_product_array(external_demand, demo_products_);
		}
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
		if (needs_migration) {
			// Remapeia os mercados antigos (farinha/pão descartados; ferro etc.
			// deslocados -2). Mercados sem correspondência no layout antigo
			// recebem o preço-base.
			Array remapped;
			remapped.resize(demo_products_);
			for (int new_index = 0; new_index < demo_products_; ++new_index) {
				const int old_index = (new_index == 0) ? 0 : (new_index + 2);
				if (old_index >= 0 && old_index < markets.size()) {
					remapped[new_index] = markets[old_index];
				} else {
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
			city.markets[product].price = double(market.get("price", 1.0));
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
		building.input_product = int(entry.get("input_product", -1));
		building.output_product = int(entry.get("output_product", 0));
		if (needs_migration) {
			building.input_product = demo_migrate_product_index(building.input_product);
			building.output_product = demo_migrate_product_index(building.output_product);
			if (building.output_product < 0) {
				continue;  // moinho/padaria (farinha/pão): construção removida da cadeia
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
		if (needs_migration) {
			const int migrated = demo_migrate_product_index(shipment.product);
			if (migrated < 0) {
				continue;  // farinha/pão em trânsito: descartado
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
}

// ---------------------------------------------------------------------------
// Benchmark sintético: estruturas planas, sem strings e sem alocação por tick.
// O modo demo/legado e o save/load existentes não são alterados.
// ---------------------------------------------------------------------------

namespace {

constexpr double kBenchFreightPerTon = 0.50;
constexpr double kBenchInvestmentCost = 1000.0;
constexpr double kBenchInvestmentCapacity = 0.5;
constexpr int kBenchInvestmentInterval = 10;

double bench_elapsed_us(const std::chrono::steady_clock::time_point& start,
		const std::chrono::steady_clock::time_point& end) {
	return std::chrono::duration<double, std::micro>(end - start).count();
}

long long bench_positive_mod(long long value, long long modulus) {
	const long long remainder = value % modulus;
	return remainder < 0 ? remainder + modulus : remainder;
}

} // namespace

void EconomySim::initialize_benchmark(int city_count, int buildings_per_city,
		int seed) {
	bench_cities_count_ = std::max(1, city_count);
	bench_buildings_per_city_ = std::max(3, buildings_per_city);
	bench_seed_ = seed;
	bench_day_ = 0;
	bench_transport_cash_ = 0.0;
	bench_active_ = true;

	const int market_count = bench_cities_count_ * kBenchProducts;
	bench_cities_.assign(bench_cities_count_, BenchCity{});
	bench_markets_.assign(market_count, BenchMarket{});
	bench_buildings_.clear();
	bench_buildings_.reserve(
		static_cast<std::size_t>(bench_cities_count_) * bench_buildings_per_city_);
	bench_links_.clear();
	bench_links_.reserve(static_cast<std::size_t>(bench_cities_count_) * 2);
	bench_capacity_total_.assign(market_count, 0.0);
	bench_unit_labor_opex_.assign(market_count, 0.0);
	bench_revenue_.assign(market_count, 0.0);
	bench_produced_total_.assign(kBenchProducts, 0.0);
	bench_consumed_total_.assign(kBenchProducts, 0.0);
	bench_tick_us_.clear();
	bench_tick_us_.reserve(256);
	bench_phase_reset_us_ = 0.0;
	bench_phase_market_us_ = 0.0;
	bench_phase_pop_us_ = 0.0;
	bench_phase_production_us_ = 0.0;
	bench_phase_trade_us_ = 0.0;
	bench_phase_accounting_us_ = 0.0;
	bench_phase_prices_us_ = 0.0;
	bench_phase_investment_us_ = 0.0;

	for (int city = 0; city < bench_cities_count_; ++city) {
		BenchCity& entry = bench_cities_[city];
		const long long mixed = static_cast<long long>(city) * 7919LL
			+ static_cast<long long>(bench_seed_) * 104729LL;
		entry.population = 50000.0
			+ static_cast<double>(bench_positive_mod(mixed, 150000));
		entry.simulation_population = entry.population;
		entry.firm_cash = 50000.0;
		entry.service_cash = 30000.0;
		entry.government_budget = 100000.0;
		entry.service_jobs = entry.simulation_population * kDemoServiceShare;
		entry.service_wage = kDemoServiceWage;
		entry.savings = 50000.0;

		BenchMarket& wheat = bench_markets_[bench_market(city, 0)];
		wheat.stock = 100.0;
		wheat.price = demo_base_price(0);

		for (int slot = 0; slot < bench_buildings_per_city_; ++slot) {
			BenchBuilding building;
			building.city = city;
			building.output = 0;
			building.input = -1;
			building.capacity = 20.0;
			building.input_per_output = 0.0;
			building.jobs = 150.0;
			building.wage = 2.0;
			building.opex = 50.0;
			building.output_market = bench_market(city, building.output);
			building.input_market = building.input >= 0
				? bench_market(city, building.input) : -1;
			bench_buildings_.push_back(building);
		}
	}

	if (bench_cities_count_ > 1) {
		for (int city = 0; city < bench_cities_count_; ++city) {
			BenchTradeLink forward;
			forward.from = city;
			forward.to = (city + 1) % bench_cities_count_;
			forward.product = 0;
			forward.from_market = bench_market(forward.from, 0);
			forward.to_market = bench_market(forward.to, 0);
			forward.capacity = 60.0;
			bench_links_.push_back(forward);
			if (bench_cities_count_ > 2) {
				BenchTradeLink backward;
				backward.from = city;
				backward.to = (city + bench_cities_count_ - 1) % bench_cities_count_;
				backward.product = 0;
				backward.from_market = bench_market(backward.from, 0);
				backward.to_market = bench_market(backward.to, 0);
				backward.capacity = 60.0;
				bench_links_.push_back(backward);
			}
		}
	}
}

void EconomySim::benchmark_reset() {
	for (BenchMarket& market : bench_markets_) {
		market.sold_prev = market.sold;
		market.demand = 0.0;
		market.supply = 0.0;
		market.desired = 0.0;
		market.sold = 0.0;
	}
	for (BenchCity& city : bench_cities_) {
		city.service_wage_bill = 0.0;
		city.service_revenue = 0.0;
		city.service_profit = 0.0;
		city.opex_revenue = 0.0;
		city.need = 0.0;
		city.disposable = 0.0;
		city.consumed = 0.0;
		city.food_spend = 0.0;
		city.satisfaction = 0.0;
		city.labor_force = 0.0;
		city.employed = 0.0;
		city.unemployed = 0.0;
		city.wage_income = 0.0;
		city.capital_income = 0.0;
		city.taxes = 0.0;
		city.transfers = 0.0;
		city.service_spend = 0.0;
		city.gdp = 0.0;
	}
	std::fill(bench_capacity_total_.begin(), bench_capacity_total_.end(), 0.0);
	for (BenchBuilding& building : bench_buildings_) {
		building.workers = 0.0;
		building.utilization = 0.0;
		building.revenue = 0.0;
		building.input_value = 0.0;
		building.costs = 0.0;
		building.profit = 0.0;
		// A capacidade efetiva já fica pronta para produção e preços.
		bench_capacity_total_[building.output_market] +=
			building.capacity * building.output_scale;
	}
	std::fill(bench_unit_labor_opex_.begin(), bench_unit_labor_opex_.end(), 0.0);
	std::fill(bench_revenue_.begin(), bench_revenue_.end(), 0.0);
	std::fill(bench_produced_total_.begin(), bench_produced_total_.end(), 0.0);
	std::fill(bench_consumed_total_.begin(), bench_consumed_total_.end(), 0.0);
}

void EconomySim::benchmark_labor() {
	const int per_city = bench_buildings_per_city_;
	for (int city = 0; city < bench_cities_count_; ++city) {
		BenchCity& entry = bench_cities_[city];
		const int base = city * per_city;
		double modeled_jobs = 0.0;
		for (int slot = 0; slot < per_city; ++slot) {
			modeled_jobs += bench_buildings_[base + slot].jobs;
		}
		const double labor_force = entry.simulation_population * kDemoLaborShare;
		const double available = std::max(0.0, labor_force - entry.service_jobs);
		const double fill = modeled_jobs > 0.0
			? std::clamp(available / modeled_jobs, 0.0, 1.0) : 0.0;
		double modeled_wages = 0.0;
		double opex_total = 0.0;
		for (int slot = 0; slot < per_city; ++slot) {
			BenchBuilding& building = bench_buildings_[base + slot];
			building.workers = building.jobs * fill;
			modeled_wages += building.workers * building.wage;
			opex_total += building.opex;
		}
		entry.labor_force = labor_force;
		entry.service_wage_bill = entry.service_jobs * entry.service_wage;
		entry.employed = entry.service_jobs + modeled_jobs * fill;
		entry.unemployed = std::max(0.0, labor_force - entry.employed);
		entry.wage_income = modeled_wages + entry.service_wage_bill;
		entry.opex_revenue = opex_total;
		entry.firm_cash -= modeled_wages + opex_total;
		entry.service_cash -= entry.service_wage_bill;
		entry.service_cash += opex_total;
	}
}

void EconomySim::benchmark_production() {
	for (BenchBuilding& building : bench_buildings_) {
		const int output_index = building.output_market;
		const double city_capacity = bench_capacity_total_[output_index];
		const double effective = building.capacity * building.output_scale;
		const double share = city_capacity > 0.0 ? effective / city_capacity : 0.0;
		building.output_share = share;
		const double sold_prev = bench_markets_[output_index].sold_prev;
		const double target = sold_prev <= 0.0
			? city_capacity
			: std::min(city_capacity, sold_prev * kDemoProductionBuffer);
		const double labor = building.jobs > 0.0
			? building.workers / building.jobs : 1.0;
		const double desired_output = target * share * labor;
		double input_used = 0.0;
		double output = desired_output;
		if (building.input >= 0) {
			BenchMarket& input_market = bench_markets_[building.input_market];
			const double needed = desired_output * building.input_per_output;
			input_market.desired += needed;
			input_used = std::min(needed, input_market.stock);
			output = building.input_per_output > 0.0
				? input_used / building.input_per_output : 0.0;
			input_market.stock -= input_used;
			input_market.demand += input_used;
			input_market.sold += input_used;
			bench_revenue_[building.input_market] +=
				input_used * input_market.price;
			building.input_value = input_used * input_market.price;
		}
		BenchMarket& output_market = bench_markets_[output_index];
		output_market.stock += output;
		output_market.supply += output;
		building.utilization = building.capacity > 0.0
			? output / building.capacity : 0.0;
		bench_produced_total_[building.output] += output;
	}
}

void EconomySim::benchmark_consumption() {
	const int preference[1] = {0};
	for (int city = 0; city < bench_cities_count_; ++city) {
		BenchCity& entry = bench_cities_[city];
		entry.capital_income = entry.capital_income_next;
		entry.capital_income_next = 0.0;
		entry.taxes = (entry.wage_income + entry.capital_income) * kDemoTaxRate;
		// Orçamento público equilibrado: transferências iguais aos impostos.
		entry.transfers = entry.taxes;
		entry.disposable = std::max(0.0,
			entry.wage_income + entry.capital_income + entry.transfers - entry.taxes);
		entry.service_spend = entry.disposable * kDemoServiceSpendShare;
		entry.service_cash += entry.service_spend;
		entry.government_budget += entry.taxes - entry.transfers;

		double budget = entry.savings + entry.disposable - entry.service_spend;
		entry.need = entry.simulation_population * kDemoNeedPerCapita;
		double consumed = 0.0;
		double food_spend = 0.0;
		for (int slot = 0; slot < kBenchProducts && consumed < entry.need; ++slot) {
			const int product = preference[slot];
			BenchMarket& market = bench_markets_[bench_market(city, product)];
			const double price = market.price;
			if (price <= 0.0) {
				continue;
			}
			const double desired = std::min(
				entry.need - consumed, std::max(0.0, budget) / price);
			if (desired <= 0.0) {
				continue;
			}
			market.desired += desired;
			const double realized = std::min(desired, market.stock);
			if (realized <= 0.0) {
				continue;
			}
			market.stock -= realized;
			market.demand += realized;
			market.sold += realized;
			budget -= realized * price;
			food_spend += realized * price;
			consumed += realized;
			bench_revenue_[bench_market(city, product)] += realized * price;
			entry.firm_cash += realized * price;
			bench_consumed_total_[product] += realized;
		}
		entry.consumed = consumed;
		entry.food_spend = food_spend;
		entry.satisfaction = entry.need > 0.0
			? std::clamp(consumed / entry.need, 0.0, 1.0) : 1.0;
		entry.savings = budget;
	}
}

void EconomySim::benchmark_trade() {
	for (const BenchTradeLink& link : bench_links_) {
		BenchMarket& seller = bench_markets_[link.from_market];
		BenchMarket& buyer = bench_markets_[link.to_market];
		const double seller_target =
			std::max(seller.desired, seller.sold_prev) * kDemoProductionBuffer;
		const double seller_surplus = std::max(0.0, seller.stock - seller_target);
		const double buyer_target =
			std::max(buyer.desired, buyer.sold_prev) * kDemoProductionBuffer;
		const double buyer_missing = std::max(0.0, buyer_target - buyer.stock);
		const double amount = std::min({seller_surplus, buyer_missing, link.capacity});
		if (amount <= 0.0) {
			continue;
		}
		const double unit = seller.price;
		const double freight = amount * kBenchFreightPerTon;
		seller.stock -= amount;
		seller.demand += amount;
		seller.sold += amount;
		buyer.stock += amount;
		buyer.demand += amount;
		buyer.price = unit + kBenchFreightPerTon;
		bench_cities_[link.from].firm_cash += amount * unit;
		bench_cities_[link.to].firm_cash -= amount * unit + freight;
		bench_transport_cash_ += freight;
		bench_revenue_[link.from_market] += amount * unit;
	}
}

void EconomySim::benchmark_accounting() {
	const int per_city = bench_buildings_per_city_;
	for (int city = 0; city < bench_cities_count_; ++city) {
		BenchCity& entry = bench_cities_[city];
		const int base = city * per_city;
		double private_profit = 0.0;
		double value_added = 0.0;
		for (int slot = 0; slot < per_city; ++slot) {
			BenchBuilding& building = bench_buildings_[base + slot];
			const int output_index = building.output_market;
			building.revenue = bench_revenue_[output_index] * building.output_share;
			const double labor_opex = building.workers * building.wage
				+ building.opex;
			building.costs = labor_opex + building.input_value;
			building.profit = building.revenue - building.costs;
			private_profit += building.profit;
			value_added += building.revenue - building.input_value;
			bench_unit_labor_opex_[output_index] += labor_opex;

			// O ajuste de produção usa o lucro do mesmo tick.
			if (building.profit < 0.0) {
				++building.loss_days;
			} else {
				building.loss_days = 0;
			}
			if (building.loss_days >= kDemoLossDaysBeforeCut) {
				building.output_scale = std::max(kDemoMinOutputScale,
					building.output_scale * kDemoLossCut);
			} else if (building.profit > 0.0) {
				building.output_scale = std::min(1.0,
					building.output_scale * 1.02);
			}
		}

		entry.service_revenue = entry.service_spend + entry.opex_revenue;
		entry.service_profit = entry.service_revenue - entry.service_wage_bill;

		const double firm_target =
			entry.simulation_population * kDemoFirmCashPerCapita;
		const double firm_surplus = std::max(0.0, entry.firm_cash - firm_target);
		const double modeled_distribution =
			std::min(std::max(0.0, private_profit), firm_surplus);
		const double service_target =
			entry.simulation_population * kDemoServiceCashPerCapita;
		const double service_surplus = std::max(0.0,
			entry.service_cash - service_target);
		const double service_distribution =
			std::min(std::max(0.0, entry.service_profit), service_surplus);
		entry.firm_cash -= modeled_distribution;
		entry.service_cash -= service_distribution;
		entry.capital_income_next = modeled_distribution + service_distribution;
		entry.gdp = value_added + entry.service_revenue;

		if (entry.service_wage > 0.0) {
			const double target_jobs = entry.service_revenue / entry.service_wage;
			const double lower = entry.simulation_population * kDemoServiceMinShare;
			const double upper = entry.simulation_population * kDemoServiceMaxShare;
			const double bounded = std::clamp(target_jobs, lower, upper);
			entry.service_jobs += (bounded - entry.service_jobs)
				* kDemoServiceAdjust;
		}
	}
}

void EconomySim::benchmark_prices() {
	for (int city = 0; city < bench_cities_count_; ++city) {
		for (int product = 0; product < kBenchProducts; ++product) {
			const int market_index = bench_market(city, product);
			BenchMarket& market = bench_markets_[market_index];
			const double base = demo_base_price(product);
			const double scarcity = market.desired
				/ std::max(market.stock + market.supply, 0.001);
			// Capacidade e custo de mão de obra/opex já foram acumulados na
			// contabilidade: evita varrer as construções outra vez.
			const double kind_capacity = bench_capacity_total_[market_index];
			const double unit_labor_opex = kind_capacity > 0.0
				? bench_unit_labor_opex_[market_index] / kind_capacity : 0.0;
			double cost_floor = base;
			// (sem cadeia de refino: trigo é extração primária)
			const double target = std::max(
				base * (1.0 + std::clamp(scarcity - 1.0, -0.5, 2.0)),
				cost_floor * 1.05);
			const double max_step = base * 0.05;
			double next = market.price
				+ (target - market.price) * kDemoPriceSmoothing;
			next = std::clamp(next, market.price - max_step,
				market.price + max_step);
			market.price = std::clamp(next, base * 0.5, base * 3.0);
		}
	}
}

void EconomySim::benchmark_investment() {
	if (bench_day_ % kBenchInvestmentInterval != 0) {
		return;
	}
	const int per_city = bench_buildings_per_city_;
	for (int city = 0; city < bench_cities_count_; ++city) {
		BenchCity& entry = bench_cities_[city];
		const double target_cash =
			entry.simulation_population * kDemoFirmCashPerCapita;
		if (entry.firm_cash <= target_cash) {
			continue;
		}
		const int base = city * per_city;
		for (int slot = 0; slot < per_city; ++slot) {
			BenchBuilding& building = bench_buildings_[base + slot];
			const BenchMarket& market =
				bench_markets_[building.output_market];
			if (building.profit <= 0.0 || building.capacity >= 200.0
					|| market.sold_prev < building.capacity * 0.98) {
				continue;
			}
			if (entry.firm_cash < kBenchInvestmentCost) {
				break;
			}
			building.capacity += kBenchInvestmentCapacity;
			entry.firm_cash -= kBenchInvestmentCost;
			entry.service_cash += kBenchInvestmentCost;
			break;
		}
	}
}

void EconomySim::tick_benchmark_internal() {
	++bench_day_;
	auto start = std::chrono::steady_clock::now();
	benchmark_reset();
	auto end = std::chrono::steady_clock::now();
	bench_phase_reset_us_ += bench_elapsed_us(start, end);

	start = std::chrono::steady_clock::now();
	benchmark_labor();
	end = std::chrono::steady_clock::now();
	bench_phase_pop_us_ += bench_elapsed_us(start, end);

	start = std::chrono::steady_clock::now();
	benchmark_production();
	end = std::chrono::steady_clock::now();
	bench_phase_production_us_ += bench_elapsed_us(start, end);

	start = std::chrono::steady_clock::now();
	benchmark_trade();
	end = std::chrono::steady_clock::now();
	bench_phase_trade_us_ += bench_elapsed_us(start, end);

	start = std::chrono::steady_clock::now();
	benchmark_consumption();
	end = std::chrono::steady_clock::now();
	bench_phase_market_us_ += bench_elapsed_us(start, end);

	start = std::chrono::steady_clock::now();
	benchmark_accounting();
	end = std::chrono::steady_clock::now();
	bench_phase_accounting_us_ += bench_elapsed_us(start, end);

	start = std::chrono::steady_clock::now();
	benchmark_prices();
	end = std::chrono::steady_clock::now();
	bench_phase_prices_us_ += bench_elapsed_us(start, end);

	start = std::chrono::steady_clock::now();
	benchmark_investment();
	end = std::chrono::steady_clock::now();
	bench_phase_investment_us_ += bench_elapsed_us(start, end);
}

Dictionary EconomySim::run_benchmark(int days, int warmup_days) {
	if (!bench_active_) {
		initialize_benchmark(100, 4, 1);
	}
	const int measured_days = std::max(1, days);
	const int warmup = std::max(0, warmup_days);
	for (int index = 0; index < warmup; ++index) {
		tick_benchmark_internal();
	}

	bench_tick_us_.clear();
	if (bench_tick_us_.capacity() < static_cast<std::size_t>(measured_days)) {
		bench_tick_us_.reserve(measured_days);
	}
	bench_phase_reset_us_ = 0.0;
	bench_phase_market_us_ = 0.0;
	bench_phase_pop_us_ = 0.0;
	bench_phase_production_us_ = 0.0;
	bench_phase_trade_us_ = 0.0;
	bench_phase_accounting_us_ = 0.0;
	bench_phase_prices_us_ = 0.0;
	bench_phase_investment_us_ = 0.0;

	const auto total_start = std::chrono::steady_clock::now();
	for (int index = 0; index < measured_days; ++index) {
		const auto start = std::chrono::steady_clock::now();
		tick_benchmark_internal();
		const auto end = std::chrono::steady_clock::now();
		bench_tick_us_.push_back(bench_elapsed_us(start, end));
	}
	const auto total_end = std::chrono::steady_clock::now();

	std::vector<double> sorted = bench_tick_us_;
	std::sort(sorted.begin(), sorted.end());
	double sum = 0.0;
	for (double value : bench_tick_us_) {
		sum += value;
	}
	const double average = sum / measured_days;
	const int p95_index = std::min(measured_days - 1, std::max(0,
		static_cast<int>(std::ceil(measured_days * 0.95)) - 1));
	const double p95 = sorted[static_cast<std::size_t>(p95_index)];
	const double maximum = sorted.empty() ? 0.0 : sorted.back();
	const double total_us = bench_elapsed_us(total_start, total_end);

	Dictionary result;
	result["city_count"] = bench_cities_count_;
	result["building_count"] = static_cast<int>(bench_buildings_.size());
	result["link_count"] = static_cast<int>(bench_links_.size());
	result["days"] = measured_days;
	result["warmup_days"] = warmup;
	result["avg_tick_us"] = average;
	result["p95_tick_us"] = p95;
	result["max_tick_us"] = maximum;
	result["avg_tick_ms"] = average / 1000.0;
	result["p95_tick_ms"] = p95 / 1000.0;
	result["max_tick_ms"] = maximum / 1000.0;
	result["total_ms"] = total_us / 1000.0;
	result["reset_us"] = bench_phase_reset_us_ / measured_days;
	result["market_us"] = bench_phase_market_us_ / measured_days;
	result["pop_us"] = bench_phase_pop_us_ / measured_days;
	result["production_us"] = bench_phase_production_us_ / measured_days;
	result["trade_us"] = bench_phase_trade_us_ / measured_days;
	result["accounting_us"] = bench_phase_accounting_us_ / measured_days;
	result["prices_us"] = bench_phase_prices_us_ / measured_days;
	result["investment_us"] = bench_phase_investment_us_ / measured_days;
	result["memory_bytes"] = static_cast<double>(benchmark_memory_bytes());
	result["checksum"] = benchmark_checksum();
	return result;
}

Dictionary EconomySim::get_benchmark_status() const {
	Dictionary status;
	status["active"] = bench_active_;
	status["day"] = bench_day_;
	status["city_count"] = bench_cities_count_;
	status["building_count"] = static_cast<int>(bench_buildings_.size());
	status["link_count"] = static_cast<int>(bench_links_.size());
	status["memory_bytes"] = static_cast<double>(benchmark_memory_bytes());
	status["checksum"] = benchmark_checksum();

	Array products;
	products.append("wheat");
	status["products"] = products;

	if (bench_cities_count_ > 0) {
		const BenchCity& city = bench_cities_[0];
		Dictionary sample;
		sample["population"] = city.population;
		sample["firm_cash"] = city.firm_cash;
		sample["service_cash"] = city.service_cash;
		sample["government_budget"] = city.government_budget;
		sample["savings"] = city.savings;
		sample["employed"] = city.employed;
		sample["unemployed"] = city.unemployed;
		sample["satisfaction"] = city.satisfaction;
		sample["gdp"] = city.gdp;
		Array markets;
		for (int product = 0; product < kBenchProducts; ++product) {
			const BenchMarket& market = bench_markets_[bench_market(0, product)];
			Dictionary entry;
			entry["stock"] = market.stock;
			entry["price"] = market.price;
			entry["desired"] = market.desired;
			entry["sold"] = market.sold;
			markets.append(entry);
		}
		sample["markets"] = markets;
		status["sample_city"] = sample;
	}
	return status;
}

int EconomySim::get_benchmark_city_count() const {
	return bench_cities_count_;
}

std::size_t EconomySim::benchmark_memory_bytes() const {
	std::size_t bytes = sizeof(EconomySim);
	const auto add = [&bytes](std::size_t count, std::size_t element) {
		bytes += count * element;
	};
	add(bench_cities_.capacity(), sizeof(BenchCity));
	add(bench_markets_.capacity(), sizeof(BenchMarket));
	add(bench_buildings_.capacity(), sizeof(BenchBuilding));
	add(bench_links_.capacity(), sizeof(BenchTradeLink));
	add(bench_capacity_total_.capacity(), sizeof(double));
	add(bench_unit_labor_opex_.capacity(), sizeof(double));
	add(bench_revenue_.capacity(), sizeof(double));
	add(bench_produced_total_.capacity(), sizeof(double));
	add(bench_consumed_total_.capacity(), sizeof(double));
	add(bench_tick_us_.capacity(), sizeof(double));
	return bytes;
}

double EconomySim::benchmark_checksum() const {
	double checksum = bench_transport_cash_;
	for (const BenchMarket& market : bench_markets_) {
		checksum += market.stock + market.price * 1000.0;
	}
	for (const BenchCity& city : bench_cities_) {
		checksum += city.savings + city.firm_cash + city.service_cash
			+ city.government_budget;
	}
	return checksum;
}

} // namespace godot
