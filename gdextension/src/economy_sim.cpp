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
constexpr int kDemoSaveVersion = 3;

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

EconomySim::EconomySim() = default;

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
	ClassDB::bind_method(D_METHOD("demo_invest", "city_id", "building_id", "amount"),
	                     &EconomySim::demo_invest);
	ClassDB::bind_method(D_METHOD("demo_build", "city_id", "building_id", "resource",
	                     "input_product", "output_product", "capacity", "jobs", "wage",
	                     "opex", "input_per_output", "owner", "cost"),
	                     &EconomySim::demo_build);
	ClassDB::bind_method(D_METHOD("demo_spend_government", "city_id", "amount"),
	                     &EconomySim::demo_spend_government);
	ClassDB::bind_method(D_METHOD("demo_to_dict"), &EconomySim::demo_to_dict);
	ClassDB::bind_method(D_METHOD("demo_from_dict", "data"), &EconomySim::demo_from_dict);
	ClassDB::bind_method(D_METHOD("demo_set_balance", "values"),
	                     &EconomySim::demo_set_balance);
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
	static const ModeSpec kTruck{TransportMode::Truck, "truck", 65.0, 1200.0, 0.10, 6.0};
	static const ModeSpec kShip{TransportMode::Ship, "ship", 35.0, 12000.0, 0.02, 24.0};
	static const ModeSpec kPlane{TransportMode::Plane, "plane", 750.0, 400.0, 0.80, 24.0};

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

	// Viagens necessárias para escoar o lote; o prazo considera a última delas.
	// O tempo de viagem soma a sobrecarga de carga/descarga (turnaround).
	const int trips = std::max(1, static_cast<int>(std::ceil(load_tons / mode_spec.batch_capacity_tons)));
	const double hours = distance / mode_spec.speed_kmh * static_cast<double>(trips)
	                     + mode_spec.turnaround_hours;

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

// Ordem idêntica à de data/definitions/economy/goods.json.
constexpr const char* kDemoProductNames[21] = {
	"wheat", "iron", "rice", "maize", "bauxite", "lead", "gold",
	"phosphate", "tungsten", "rare_earths", "sulfur", "diamond",
	"coal", "copper", "gas", "oil", "potato", "soy", "cotton", "wood", "uranium",
};

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
	if (product >= 0 && product < kDemoProducts) {
		return demo_base_prices_[product];
	}
	return 40.0;
}

void EconomySim::demo_set_goods(const Array& goods) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	// Atualiza os preços-base a partir de dados externos (goods.json). Mantém o
	// preço padrão para bens ausentes ou inválidos.
	for (int index = 0; index < goods.size(); ++index) {
		const Dictionary entry = goods[index];
		const int product = demo_product_index(String(entry.get("id", "")));
		if (product < 0) {
			continue;
		}
		const double price = safe_nonneg(static_cast<double>(entry.get("base_price", 0.0)));
		if (price > 0.0) {
			demo_base_prices_[product] = price;
		}
	}
}

void EconomySim::initialize_demo() {
	// Novo mundo: o cache de status pertence ao mundo anterior.
	demo_status_slots_[0] = DemoStatusCache{};
	demo_status_slots_[1] = DemoStatusCache{};
	demo_status_live_ = -1;
	demo_status_built_day_ = -1;
	++demo_status_static_version_;
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
	for (int index = 0; index < kDemoProducts; ++index) {
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
	for (int product = 1; product < kDemoProducts; ++product) {
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
	for (int product = 1; product < kDemoProducts; ++product) {
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
	demo_rebuild_indexes();
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
	for (int index = 0; index < kDemoProducts; ++index) {
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
		// Aptidão padrão (inicializador do struct: suitability = {1.0, 1.0, 1.0,
		// 0, ...} — trigo/ferro/arroz em 1.0). O GeographyService sobrescreve com
		// o dado real em tempo de execução.
		city.suitability[0] = 1.0;
		for (int product = 0; product < kDemoProducts; ++product) {
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
		demo_ports_.push_back(std::move(port));
	}

	demo_rebuild_indexes();
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
	out.transport_cash = demo_transport_cash_;
	out.foreign_cash = demo_foreign_cash_;
	out.construction_cash = demo_construction_cash_;
	for (int index = 0; index < kDemoProducts; ++index) {
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
	demo_transport_cash_ = in.transport_cash;
	demo_foreign_cash_ = in.foreign_cash;
	demo_construction_cash_ = in.construction_cash;
	for (int index = 0; index < kDemoProducts; ++index) {
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

		for (int product = 0; product < kDemoProducts; ++product) {
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
		for (int slot = 0; slot < kDemoFoodProducts; ++slot) {
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
	const bool exhaustive = city_count <= kDemoTradeExhaustiveLimit;
	const double cell = kDemoTradeCellMapUnits;
	const int grid_w = std::max(1, static_cast<int>(std::ceil(kMapWidth / cell)));
	const int grid_h = std::max(1, static_cast<int>(std::ceil(kMapHeight / cell)));
	std::vector<std::vector<int>> grid(
		static_cast<std::size_t>(grid_w * grid_h));
	auto cell_index = [cell, grid_w, grid_h](const MapPoint& point) {
		const int ix = std::clamp(static_cast<int>(point.x / cell), 0, grid_w - 1);
		const int iy = std::clamp(static_cast<int>(point.y / cell), 0, grid_h - 1);
		return iy * grid_w + ix;
	};
	for (std::size_t si = 0; si < city_count; ++si) {
		grid[static_cast<std::size_t>(cell_index(demo_cities_[si].center))]
			.push_back(static_cast<int>(si));
	}

	// Ofertas por slot da cesta, ordenadas pelo custo por caloria do preço
	// (preço/fator, sem frete): as M primeiras formam o mercado externo.
	std::vector<std::vector<int>> offers_by_slot(kDemoFoodProducts);
	for (std::size_t si = 0; si < city_count; ++si) {
		if (sellable_ge[si] <= 1e-9) {
			continue;
		}
		for (int slot = 0; slot < kDemoFoodProducts; ++slot) {
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
	for (int slot = 0; slot < kDemoFoodProducts; ++slot) {
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
			const int max_radius = std::max(
				std::max(bx, grid_w - 1 - bx), std::max(by, grid_h - 1 - by));
			const int wanted = std::min(k_neighbors, sellers_left);
			// Limite de segurança: nenhum comprador visita mais células que a
			// grade inteira, mesmo que as condições acima mudem.
			int cells_budget = grid_w * grid_h;
			auto scan_cell = [&](int ix, int iy) {
				if (ix < 0 || ix >= grid_w || iy < 0 || iy >= grid_h || cells_budget <= 0) {
					return;
				}
				--cells_budget;
				for (const int si : grid[static_cast<std::size_t>(iy * grid_w + ix)]) {
					if (si == buyer.index || sellable_ge[static_cast<std::size_t>(si)] <= 1e-9) {
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
			for (int slot = 0; slot < kDemoFoodProducts; ++slot) {
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
		offers.reserve(candidates.size() * kDemoFoodProducts);
		for (const int si : candidates) {
			DemoCity& seller = demo_cities_[static_cast<std::size_t>(si)];
			if (&seller == buyer.city || sellable_ge[static_cast<std::size_t>(si)] <= 1e-9) {
				continue;
			}
			const bool same_landmass = seller.landmass == buyer.city->landmass;
			const double freight_rate = same_landmass
				? kDemoFreightPerTonKm : kDemoFreightShipPerTonKm;
			const double unit_freight =
				distance_km(seller.center, buyer.city->center) * freight_rate;
			for (int slot = 0; slot < kDemoFoodProducts; ++slot) {
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
			for (int slot = 0; slot < kDemoFoodProducts; ++slot) {
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
			const double ge = std::min(amount_tons * factor, remaining);
			amount_tons = ge / factor;

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

			demo_in_transit_.push_back({buyer.city->id, product, amount_tons, day_ + 1});

			const bool same_landmass = seller->landmass == buyer.city->landmass;

			// Pedido de comércio para os comboios 3D e o HUD. Na mesma massa de
			// terra vai de caminhão em linha reta; entre massas, o navio entra
			// pelo porto do vendedor e sai pelo porto do comprador (infraestrutura
			// nacional: sem porto, sem travessia marítima — cai no direto).
			TradeOrder order;
			order.from_city = seller->id;
			order.to_city = buyer.city->id;
			order.from_nation = seller->nation;
			order.to_nation = buyer.city->nation;
			order.scope = MarketScope::International;
			order.amount = amount_tons;

			double total_transit = 0.0;
			double total_freight_cost = 0.0;
			if (same_landmass) {
				Leg leg = make_leg(TransportMode::Truck, seller->id, buyer.city->id,
					seller->center, buyer.city->center, amount_tons);
				total_transit = leg.transit_hours;
				total_freight_cost = leg.cost;
				order.legs.push_back(std::move(leg));
			} else {
				const Port* seller_port = demo_nearest_port(
					seller->nation, seller->landmass, seller->center);
				const Port* buyer_port = demo_nearest_port(
					buyer.city->nation, buyer.city->landmass, buyer.city->center);
				if (seller_port != nullptr && buyer_port != nullptr) {
					// Cidade -> porto do vendedor (caminhão).
					if (distance_km(seller->center, seller_port->center) > 1.0) {
						Leg leg = make_leg(TransportMode::Truck, seller->id,
							seller_port->name, seller->center, seller_port->center, amount_tons);
						total_transit += leg.transit_hours;
						total_freight_cost += leg.cost;
						order.legs.push_back(std::move(leg));
					}
					// Porto -> porto (navio).
					{
						Leg leg = make_leg(TransportMode::Ship, seller_port->name,
							buyer_port->name, seller_port->center, buyer_port->center, amount_tons);
						total_transit += leg.transit_hours;
						total_freight_cost += leg.cost;
						order.legs.push_back(std::move(leg));
					}
					// Porto do comprador -> cidade (caminhão).
					if (distance_km(buyer_port->center, buyer.city->center) > 1.0) {
						Leg leg = make_leg(TransportMode::Truck, buyer_port->name,
							buyer.city->id, buyer_port->center, buyer.city->center, amount_tons);
						total_transit += leg.transit_hours;
						total_freight_cost += leg.cost;
						order.legs.push_back(std::move(leg));
					}
				} else {
					// Sem porto na rota: navio direto entre cidades.
					Leg leg = make_leg(TransportMode::Ship, seller->id, buyer.city->id,
						seller->center, buyer.city->center, amount_tons);
					total_transit = leg.transit_hours;
					total_freight_cost = leg.cost;
					order.legs.push_back(std::move(leg));
				}
			}
			order.transit_hours = total_transit;
			order.freight_cost = total_freight_cost;
			demo_last_orders_.push_back(std::move(order));

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

void EconomySim::demo_national_fiscal_transfer() {
	// Equalização fiscal per capita dentro de cada nação: o orçamento público é
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

	// Retrato para reverter o tick se um invariante rígido for violado.
	DemoSnapshot before;
	demo_snapshot(before);
	const double money_before = demo_money_total();

	++day_;
	demo_last_orders_.clear();

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
		open_prices[city_index].resize(kDemoProducts);
		for (int product = 0; product < kDemoProducts; ++product) {
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
			shipment = demo_in_transit_.erase(shipment);
			continue;
		}
		++shipment;
	}

	lap(kDemoPhaseArrival);

	// 2. Zera os fluxos do dia.
	for (int index = 0; index < kDemoProducts; ++index) {
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
		city.service_wage_bill = 0.0;
		city.service_revenue = 0.0;
		city.service_profit = 0.0;
		city.opex_revenue = 0.0;
		city.freight_revenue = 0.0;
		city.construction_revenue = city.construction_revenue_pending;
		city.construction_revenue_pending = 0.0;
		city.gdp = 0.0;
		for (int product = 0; product < kDemoProducts; ++product) {
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
		for (int product = 0; product < kDemoFoodProducts; ++product) {
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

	// 3. Mercado de trabalho e salários.
	// Minas mais fundas custam mais para operar: opex × fator de profundidade
	// (1.0 para tudo que não é extração com jazida declarada).
	auto opex_factor = [](const DemoCity& owner_city, const DemoBuilding& building) {
		if (building.input_product >= 0 || building.output_product < 0
				|| building.output_product >= kDemoProducts) {
			return 1.0;
		}
		return demo_depth_opex_factor(owner_city.deposits[building.output_product]);
	};
	for (std::size_t city_index = 0; city_index < demo_cities_.size(); ++city_index) {
		DemoCity& city = demo_cities_[city_index];
		const std::vector<int>& local_buildings =
			demo_buildings_by_city_[city_index];
		double modeled_jobs = 0.0;
		for (const int bi : local_buildings) {
			modeled_jobs += demo_buildings_[static_cast<std::size_t>(bi)].jobs;
		}
		// O emprego de serviços é persistente: cresce ou recua conforme a
		// receita do setor, em vez de ser recalculado a cada dia.
		const double labor_force = city.simulation_population * kDemoLaborShare;
		city.pop.labor_force = labor_force;

		const double available = std::max(0.0, labor_force - city.service_jobs);
		const double fill = modeled_jobs > 0.0
			? std::clamp(available / modeled_jobs, 0.0, 1.0) : 0.0;

		double modeled_wages = 0.0;
		double opex_total = 0.0;
		for (const int bi : local_buildings) {
			DemoBuilding& building = demo_buildings_[static_cast<std::size_t>(bi)];
			building.workers = building.jobs * fill;
			modeled_wages += building.workers * building.wage;
			opex_total += building.opex * opex_factor(city, building);
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
			building.paid_wages = building.workers * building.wage * pay_scale;
			building.paid_opex = building.opex * opex_factor(city, building) * pay_scale;
		}

		city.service_wage_bill = city.service_jobs * city.service_wage;
		// O setor de serviços não paga folha sem caixa: a folha é limitada ao
		// saldo disponível no início do dia, sem crédito implícito.
		city.service_wage_bill = std::min(city.service_wage_bill,
				std::max(0.0, city.service_cash));
		city.pop.employed = city.service_jobs + modeled_jobs * fill;
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
	std::vector<std::array<double, kDemoProducts>> capacity_by_city_product(
		demo_cities_.size());
	for (auto& slots : capacity_by_city_product) {
		slots.fill(0.0);
	}
	for (const DemoBuilding& building : demo_buildings_) {
		if (building.output_product < 0 || building.output_product >= kDemoProducts) {
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
				|| building.output_product >= kDemoProducts) {
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
		// Extração: aptidão territorial × eficiência pela qualidade do minério
		// (1.0 quando não há jazida declarada).
		DemoDeposit& deposit = city->deposits[building.output_product];
		const double suitability = building.input_product < 0
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
		if (building.input_product >= 0) {
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
		demo_produced_[building.output_product] += output;
		demo_produced_today_[building.output_product] += output;
	}

	lap(kDemoPhaseProduction);

	// 4c. Recursos brutos (minerais, soja, algodão e madeira): sem consumo local
	// nem cadeia de refino nesta etapa. O ferro entra aqui (não há mais um caso
	// especial 4c com 20 t/dia fixo): usa o mesmo teto data-driven dos demais.
	// O resto do mundo compra ao preço do
	// mercado até um TETO de demanda externa por cidade (external_demand),
	// declarado pelo GeographyService como a participação da nação no mercado
	// mundial (reserva esgotada em ~25 anos — o mesmo horizonte de
	// max_rate_kt_day do deposits.json). Com o teto, a produção acima dele vira
	// estoque e a fase 8 derruba o preço (escassez < 1) — é a "curva" de demanda
	// limitada, que evita sobra absurda e não inventa consumo infinito. A
	// extração continua limitada pela reserva (fase 4), então o esgotamento da
	// jazida encerra a receita. Os alimentos da cesta (B3: trigo, arroz, milho,
	// batata e soja) ficam FORA desta venda externa — são consumidos pelas
	// famílias (fase 5) e negociados entre cidades (fase 6); vendê-los aqui
	// deixava as cidades produtoras de arroz/milho sem nada para comer.
	for (DemoCity& city : demo_cities_) {
		for (int product = kDemoFirstRawProduct; product < kDemoProducts; ++product) {
			if (demo_food_calories(product) > 0.0) {
				continue;
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
		for (int slot = 0; slot < kDemoFoodProducts; ++slot) {
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
		if (cheapest_food > 0.0 && budget < food_cost) {
			const double topup = std::min(food_cost - budget,
				std::max(0.0, city.government_budget));
			if (topup > 0.0) {
				budget += topup;
				city.government_budget -= topup;
				pop.transfers += topup;
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
		double avail_ge[kDemoFoodProducts] = {};
		for (int slot = 0; slot < kDemoFoodProducts; ++slot) {
			const int product = demo_food_product(slot);
			avail_ge[slot] = city.markets[product].stock * demo_food_calories(product);
			total_avail += avail_ge[slot];
		}
		double remaining_need = pop.need;
		for (int slot = 0; slot < kDemoFoodProducts && remaining_need > 1e-12; ++slot) {
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
		pop.savings = budget;
	}

	lap(kDemoPhaseIncome);

	// 6. Comércio de alimento (cesta de calorias) entre cidades: cidades com
	// déficit compram das que têm excedente, mais barato por caloria. Cobre tanto
	// o cenário de validação (AGRIA/PORTUS) quanto o mundo real.
	demo_trade_food();

	lap(kDemoPhaseTrade);

	// 7. Receita por construção, lucro, distribuição de capital e PIB.
	for (std::size_t city_index = 0; city_index < demo_cities_.size(); ++city_index) {
		DemoCity& city = demo_cities_[city_index];
		const std::vector<int>& local_buildings =
			demo_buildings_by_city_[city_index];
		double capacity_by_output[kDemoProducts] = {};
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
			if (building.profit < 0.0) {
				++building.loss_days;
			} else {
				building.loss_days = 0;
			}
			// Falência: prejuízo persistente além do corte fecha a firma. A
			// regra é explícita — a construção some (capacidade e empregos), em
			// vez de ficar produzindo no piso no vermelho para sempre.
			if (building.loss_days >= kDemoLossDaysBeforeClose) {
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
		for (int slot = 0; slot < kDemoFoodProducts; ++slot) {
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
		bool has_producer[kDemoProducts] = {};
		for (const int bi : local_buildings) {
			const DemoBuilding& building =
				demo_buildings_[static_cast<std::size_t>(bi)];
			if (building.output_product >= 0
					&& building.output_product < kDemoProducts) {
				has_producer[building.output_product] = true;
			}
		}
		for (int product = 0; product < kDemoProducts; ++product) {
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

			// Piso de custo: insumo declarado pela receita + mão de obra/opex
			// por unidade de capacidade.
			double cost_floor = base;
			for (const int bi : local_buildings) {
				const DemoBuilding& building =
					demo_buildings_[static_cast<std::size_t>(bi)];
				if (building.output_product == product
						&& building.input_product >= 0) {
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
		for (int product = 0; product < kDemoProducts; ++product) {
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
		for (int product = 0; product < kDemoProducts; ++product) {
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
		y("y")
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

		slot.suitability.resize(kDemoProducts);
		slot.external_demand.resize(kDemoProducts);
		for (int product = 0; product < kDemoProducts; ++product) {
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
	dict[k.population] = city.population;
	dict[k.simulation_population] = city.simulation_population;
	dict[k.gdp] = city.gdp;
	dict[k.gdp_per_capita] = city.simulation_population > 0.0
		? city.gdp / city.simulation_population : 0.0;
	dict[k.external_income_index] = city.external_income_index;
	dict[k.firm_cash] = city.firm_cash;
	dict[k.service_cash] = city.service_cash;
	dict[k.government_budget] = city.government_budget;
	dict[k.service_revenue] = city.service_revenue;
	dict[k.service_profit] = city.service_profit;
	dict[k.opex_revenue] = city.opex_revenue;
	dict[k.freight_revenue] = city.freight_revenue;
	dict[k.construction_revenue] = city.construction_revenue;
	dict[k.service_wage] = city.service_jobs > 0.0
		? city.service_wage_bill / city.service_jobs : 0.0;
	dict[k.disposable_per_capita] = city.simulation_population > 0.0
		? city.pop.disposable / city.simulation_population : 0.0;

	// Padrão de vida: quantas cestas alimentares diárias a renda disponível
	// por pessoa compra. Diferente do PIB per capita, usa renda das famílias.
	double cheapest_price = 0.0;
	for (int food = 0; food < kDemoFoodProducts; ++food) {
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
	dict[k.living_standard] = living_standard;

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
	dict[k.production] = production;
	dict[k.jobs] = city.pop.employed;
	dict[k.income] = std::clamp(living_standard, 0.0, 1.0);
	dict[k.industrialization] = total_capacity > 0.0
		? std::clamp(secondary_capacity / total_capacity, 0.0, 1.0) : 0.0;
	dict[k.stockpile] = city.markets[0].stock;
	dict[k.need] = city.pop.need;
	dict[k.deficit] = std::max(0.0, city.pop.need - city.pop.consumed);
	dict[k.consumed] = city.pop.consumed;
	dict[k.in_transit] = in_transit;

	// Reservas minerais declaradas (só produtos com jazida conhecida; ausência
	// da chave = sem dado, não "zero"). O bloco ESTÁTICO (reserva inicial,
	// qualidade, profundidade, eficiência, opex) é escrito uma vez; por dia só
	// mudam reserva restante, extraído e esgotado. Antes o dicionário inteiro
	// era reconstruído a cada dia — custo que crescia com as cidades mineiras
	// do cenário real (o mundo sintético do bench não tem depósito nenhum).
	if (slot.deposit_entry.size() < kDemoProducts) {
		slot.deposit_entry.resize(static_cast<std::size_t>(kDemoProducts));
		slot.deposit_present.assign(static_cast<std::size_t>(kDemoProducts), 0);
		slot.deposit_depleted.assign(static_cast<std::size_t>(kDemoProducts), 0);
		slot.deposit_written.assign(static_cast<std::size_t>(kDemoProducts),
			DemoDeposit{});
		slot.deposit_key.clear();
		slot.deposit_key.reserve(static_cast<std::size_t>(kDemoProducts));
		for (int product = 0; product < kDemoProducts; ++product) {
			slot.deposit_key.emplace_back(kDemoProductNames[product]);
		}
	}
	// Jazida descoberta depois do primeiro dia (a IA chama `demo_set_deposit`
	// ao revelar/conquistar) sobe a versão estática: o bloco de reserva precisa
	// ser reescrito, senão o status mostraria a jazida antiga para sempre.
	if (slot.deposit_static_version != demo_status_static_version_) {
		slot.deposit_present.assign(static_cast<std::size_t>(kDemoProducts), 0);
		slot.deposit_static_version = demo_status_static_version_;
	}
	for (int product = 0; product < kDemoProducts; ++product) {
		const DemoDeposit& deposit = city.deposits[product];
		if (!deposit.tracked) {
			continue;
		}
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
	dict[k.deposits] = slot.deposits;

	Dictionary& pop = slot.pop;
	pop[k.population] = city.pop.population;
	pop[k.labor_force] = city.pop.labor_force;
	pop[k.employed] = city.pop.employed;
	pop[k.unemployed] = city.pop.unemployed;
	pop[k.dependents] = city.pop.dependents;
	pop[k.wage_income] = city.pop.wage_income;
	pop[k.capital_income] = city.pop.capital_income;
	pop[k.transfers] = city.pop.transfers;
	pop[k.taxes] = city.pop.taxes;
	pop[k.service_spend] = city.pop.service_spend;
	pop[k.disposable] = city.pop.disposable;
	pop[k.external_income] = city.pop.external_income;
	pop[k.food_spend] = city.pop.food_spend;
	pop[k.consumed] = city.pop.consumed;
	pop[k.need] = city.pop.need;
	pop[k.satisfaction] = city.pop.satisfaction;
	pop[k.savings] = city.pop.savings;
	slot.desired_by_category.resize(kDemoFoodProducts);
	slot.consumed_by_category.resize(kDemoFoodProducts);
	for (int food = 0; food < kDemoFoodProducts; ++food) {
		slot.desired_by_category[food] = city.pop.desired_by_category[food];
		slot.consumed_by_category[food] = city.pop.consumed_by_category[food];
	}
	pop[k.desired_by_category] = slot.desired_by_category;
	pop[k.consumed_by_category] = slot.consumed_by_category;
	dict[k.pop] = pop;

	slot.markets.resize(kDemoProducts);
	if (slot.market_entry.size() < kDemoProducts) {
		slot.market_entry.resize(static_cast<std::size_t>(kDemoProducts));
		slot.market_written.resize(static_cast<std::size_t>(kDemoProducts));
		slot.market_written_filled.assign(static_cast<std::size_t>(kDemoProducts), 0);
		slot.io_written.assign(static_cast<std::size_t>(4 * kDemoProducts), 0.0);
		slot.io_filled.assign(4, 0);
	}
	for (int product = 0; product < kDemoProducts; ++product) {
		const DemoMarket& source = city.markets[product];
		Dictionary& market = slot.market_entry[static_cast<std::size_t>(product)];
		DemoMarket& previous = slot.market_written[static_cast<std::size_t>(product)];
		if (slot.market_written_filled[static_cast<std::size_t>(product)] == 0) {
			// Primeira gravação desta cidade: cria as chaves do mercado.
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
			continue;
		}
		// Depois disso o Dictionary é mutado no lugar: só valores diferentes do
		// último status publicado pagam o custo de hash.
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
	dict[k.markets] = slot.markets;

	slot.imported.resize(kDemoProducts);
	slot.exported.resize(kDemoProducts);
	slot.import_value.resize(kDemoProducts);
	slot.export_value.resize(kDemoProducts);
	{
		// Quatro arrays de fluxo, cada um com 21 produtos: idem mercados, só
		// produtos com comércio mudam (o resto fica em 0 dia após dia).
		const double* source_values[4] = {
			city.imported, city.exported, city.import_value, city.export_value};
		Array* targets[4] = {
			&slot.imported, &slot.exported, &slot.import_value, &slot.export_value};
		const StringName* keys[4] = {&k.imported, &k.exported, &k.import_value, &k.export_value};
		for (int array_index = 0; array_index < 4; ++array_index) {
			Array& target = *targets[array_index];
			double* previous = &slot.io_written[static_cast<std::size_t>(
				array_index * kDemoProducts)];
			const bool first = slot.io_filled[static_cast<std::size_t>(array_index)] == 0;
			for (int product = 0; product < kDemoProducts; ++product) {
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
	dict[k.imported] = slot.imported;
	dict[k.exported] = slot.exported;
	dict[k.import_value] = slot.import_value;
	dict[k.export_value] = slot.export_value;

	slot.buildings.resize(local_buildings.size());
	if (slot.building_entry.size() < local_buildings.size()) {
		slot.building_entry.resize(local_buildings.size());
	}
	for (std::size_t index = 0; index < local_buildings.size(); ++index) {
		const DemoBuilding& building =
			demo_buildings_[static_cast<std::size_t>(local_buildings[index])];
		Dictionary& entry = slot.building_entry[index];
		entry[k.id] = String(building.id.c_str());
		entry[k.kind] = building.kind;
		entry[k.resource] = String(building.resource.c_str());
		entry[k.owner] = String(building.owner.c_str());
		entry[k.input_product] = building.input_product;
		entry[k.output_product] = building.output_product;
		entry[k.capacity] = building.capacity;
		entry[k.jobs] = building.jobs;
		entry[k.wage] = building.wage;
		entry[k.opex] = building.opex;
		entry[k.workers] = building.workers;
		entry[k.utilization] = building.utilization;
		entry[k.output_scale] = building.output_scale;
		entry[k.loss_days] = building.loss_days;
		entry[k.revenue] = building.revenue;
		entry[k.input_value] = building.input_value;
		entry[k.costs] = building.costs;
		entry[k.profit] = building.profit;
		entry[k.site_suitability] = city.suitability[building.output_product];
		slot.buildings[index] = entry;
	}
	dict[k.buildings] = slot.buildings;
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
		cache.products.resize(kDemoProducts);
		for (int index = 0; index < kDemoProducts; ++index) {
			cache.products[index] = String(kDemoProductNames[index]);
		}
		cache.tot_produced.resize(kDemoProducts);
		cache.tot_consumed.resize(kDemoProducts);
		cache.tot_produced_today.resize(kDemoProducts);
		cache.tot_consumed_today.resize(kDemoProducts);
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
	cache.tot_surplus.resize(kDemoProducts);
	cache.tot_sold.resize(kDemoProducts);
	cache.tot_demand.resize(kDemoProducts);
	for (int product = 0; product < kDemoProducts; ++product) {
		cache.tot_surplus[product] = 0.0;
		cache.tot_sold[product] = 0.0;
		cache.tot_demand[product] = 0.0;
	}
	// Soma só os valores positivos, na ordem das cidades: idêntico (bit a bit)
	// ao laço que o `PrivateInvestor.prepare_totals` fazia.
	for (const DemoCity& city : demo_cities_) {
		for (int product = 0; product < kDemoProducts; ++product) {
			const DemoMarket& market = city.markets[product];
			if (market.surplus > 0.0) {
				cache.tot_surplus[product] += market.surplus;
			}
			if (market.sold > 0.0) {
				cache.tot_sold[product] += market.sold;
			}
			if (market.demand > 0.0) {
				cache.tot_demand[product] += market.demand;
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
		cache.ports[index] = dict;
	}

	// Pedidos de comércio do último tick (comboios 3D e HUD).
	cache.orders.resize(demo_last_orders_.size());
	cache.order_slots.resize(demo_last_orders_.size());
	cache.order_leg_slots.resize(demo_last_orders_.size());
	for (std::size_t index = 0; index < demo_last_orders_.size(); ++index) {
		const TradeOrder& order = demo_last_orders_[index];
		Dictionary& dict = cache.order_slots[index];
		dict[k.from_city] = String(order.from_city.c_str());
		dict[k.to_city] = String(order.to_city.c_str());
		dict[k.from_nation] = String(order.from_nation.c_str());
		dict[k.to_nation] = String(order.to_nation.c_str());
		dict[k.scope] = String(scope_name(order.scope));
		dict[k.good] = "wheat";
		dict[k.amount] = order.amount;
		dict[k.transit_hours] = order.transit_hours;
		dict[k.freight_cost] = order.freight_cost;
		Array& legs = cache.order_leg_slots[index];
		legs.resize(order.legs.size());
		for (std::size_t leg_index = 0; leg_index < order.legs.size(); ++leg_index) {
			legs[leg_index] = leg_to_dict(order.legs[leg_index]);
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
		cache.nations[index] = nation;
	}

	for (int index = 0; index < kDemoProducts; ++index) {
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

int EconomySim::demo_product_index(const String& product) {
	for (int index = 0; index < kDemoProducts; ++index) {
		if (product == kDemoProductNames[index]) {
			return index;
		}
	}
	return -1;
}

int EconomySim::demo_food_product(int slot) {
	return (slot >= 0 && slot < kDemoFoodProducts) ? kDemoFoodProductIds[slot] : -1;
}

double EconomySim::demo_food_calories(int product) {
	for (int slot = 0; slot < kDemoFoodProducts; ++slot) {
		if (kDemoFoodProductIds[slot] == product) {
			return kDemoCaloricFactor[slot];
		}
	}
	return 0.0;
}

const char* EconomySim::demo_product_name(int product) {
	if (product >= 0 && product < kDemoProducts) {
		return kDemoProductNames[product];
	}
	return kDemoProductNames[0];
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
	++demo_status_static_version_;
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

	const double capacity_added = amount / kDemoCapitalPerCapacity;
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
		double input_per_output, const String& owner, double cost) {
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
	if (output_product < 0 || output_product >= kDemoProducts) {
		result["error"] = "produto inválido";
		return result;
	}
	if (input_product < -1 || input_product >= kDemoProducts) {
		result["error"] = "insumo inválido";
		return result;
	}
	if (capacity <= 0.0 || jobs < 0.0 || wage < 0.0 || opex < 0.0 || cost < 0.0) {
		result["error"] = "parâmetros inválidos";
		return result;
	}
	if (input_product < 0
			&& city->suitability[output_product] <= 0.0) {
		result["error"] = "sem aptidão territorial";
		return result;
	}
	if (input_product < 0) {
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
	building.capacity = capacity;
	building.input_per_output = input_per_output;
	building.jobs = jobs;
	building.wage = wage;
	building.opex = opex;
	demo_buildings_.push_back(building);

	// O investimento é pago ao setor de construção doméstico e entra no PIB
	// do próximo tick como receita de construção.
	demo_rebuild_indexes();
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

	Array produced;
	Array consumed;
	for (int index = 0; index < kDemoProducts; ++index) {
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
		for (int product = 0; product < kDemoProducts; ++product) {
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
		for (int product = 0; product < kDemoProducts; ++product) {
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
			deposits[String(kDemoProductNames[product])] = deposit_entry;
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
		for (int product = 0; product < kDemoFoodProducts; ++product) {
			desired_by_category.append(city.pop.desired_by_category[product]);
			consumed_by_category.append(city.pop.consumed_by_category[product]);
		}
		pop["desired_by_category"] = desired_by_category;
		pop["consumed_by_category"] = consumed_by_category;
		entry["pop"] = pop;

		Array markets;
		for (int product = 0; product < kDemoProducts; ++product) {
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
		entry["capacity"] = building.capacity;
		entry["input_per_output"] = building.input_per_output;
		entry["jobs"] = building.jobs;
		entry["wage"] = building.wage;
		entry["opex"] = building.opex;
		entry["output_scale"] = building.output_scale;
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
		transit.append(entry);
	}
	data["in_transit"] = transit;
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
		produced = demo_migrate_product_array(produced, kDemoProducts);
		consumed = demo_migrate_product_array(consumed, kDemoProducts);
	}
	for (int index = 0; index < kDemoProducts; ++index) {
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
			suitability = demo_migrate_product_array(suitability, kDemoProducts);
			revenue = demo_migrate_product_array(revenue, kDemoProducts);
			imported = demo_migrate_product_array(imported, kDemoProducts);
			exported = demo_migrate_product_array(exported, kDemoProducts);
			import_value = demo_migrate_product_array(import_value, kDemoProducts);
			export_value = demo_migrate_product_array(export_value, kDemoProducts);
			external_demand = demo_migrate_product_array(external_demand, kDemoProducts);
		}
		for (int product = 0; product < kDemoProducts; ++product) {
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
		for (int product = 0; product < kDemoProducts; ++product) {
			const String key = String(kDemoProductNames[product]);
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
		Array desired_by_category = pop.get("desired_by_category", Array());
		Array consumed_by_category = pop.get("consumed_by_category", Array());
		for (int product = 0; product < kDemoFoodProducts; ++product) {
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
			remapped.resize(kDemoProducts);
			for (int new_index = 0; new_index < kDemoProducts; ++new_index) {
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
		for (int product = markets.size(); product < kDemoProducts; ++product) {
			if (product >= 0) {
				city.markets[product].price = demo_base_price(product);
			}
		}
		for (int product = 0; product < kDemoProducts && product < markets.size(); ++product) {
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
		building.jobs = double(entry.get("jobs", 0.0));
		building.wage = double(entry.get("wage", 0.0));
		building.opex = double(entry.get("opex", 0.0));
		building.output_scale = double(entry.get("output_scale", 1.0));
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
		demo_in_transit_.push_back(shipment);
	}
	demo_rebuild_indexes();
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
