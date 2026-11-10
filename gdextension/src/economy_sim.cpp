#include "economy_sim.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <algorithm>
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
// dicionário; demo_from_dict recusa versões futuras (maiores).
constexpr int kDemoSaveVersion = 1;

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

EconomySim::~EconomySim() = default;

void EconomySim::_bind_methods() {
	ClassDB::bind_method(D_METHOD("initialize", "cities", "ports"), &EconomySim::initialize);
	ClassDB::bind_method(D_METHOD("tick"), &EconomySim::tick);
	ClassDB::bind_method(D_METHOD("get_status"), &EconomySim::get_status);
	ClassDB::bind_method(D_METHOD("initialize_demo"), &EconomySim::initialize_demo);
	ClassDB::bind_method(D_METHOD("initialize_demo_world", "cities", "ports", "buildings"),
	                     &EconomySim::initialize_demo_world);
	ClassDB::bind_method(D_METHOD("tick_demo"), &EconomySim::tick_demo);
	ClassDB::bind_method(D_METHOD("get_demo_status"), &EconomySim::get_demo_status);
	ClassDB::bind_method(D_METHOD("demo_price_shock", "product", "multiplier"),
	                     &EconomySim::demo_price_shock);
	ClassDB::bind_method(D_METHOD("demo_set_suitability", "city_id", "product", "value"),
	                     &EconomySim::demo_set_suitability);
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
		city.need = city.population * kNeedPerCapita;
		city.consumed = std::min(city.stockpile, city.need);
		city.stockpile -= city.consumed;
		city.deficit = city.need - city.consumed;
		city.dispatched = 0.0;

		// Indicadores derivados: quanto a cidade industrializou, quantos
		// empregos isso sustenta e a renda relativa (0..1) de quem vive nela.
		const double saturation = city.population * kTonsPerCapitaSaturation;
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
// Cenário de validação: duas cidades, POPs agregados, trigo -> farinha -> pão.
// ---------------------------------------------------------------------------

namespace {

constexpr double kDemoNeedPerCapita = 0.0003;
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
// Volume de pão exportado a Agria, calibrado para equilibrar o trigo importado.
constexpr double kDemoBreadExportTons = 6.0;
// Demanda externa de ferro (fora do consumo alimentar).
constexpr double kDemoIronExportDemand = 20.0;

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
// Teto de cargas de comércio por dia (proteção contra loop).
constexpr std::size_t kDemoMaxShipmentsPerDay = 64;

} // namespace

double EconomySim::demo_base_price(int product) {
	switch (product) {
		case 1:
			return 70.0;
		case 2:
			return 80.0;
		case 3:
			return 60.0;
		default:
			return 40.0;
	}
}

void EconomySim::initialize_demo() {
	demo_active_ = true;
	day_ = 0;
	demo_cities_.clear();
	demo_buildings_.clear();
	demo_in_transit_.clear();
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
	agria.firm_cash = 50000.0;
	agria.service_cash = 30000.0;
	agria.government_budget = 100000.0;
	agria.service_jobs = agria.population * kDemoServiceShare;
	agria.service_wage = kDemoServiceWage;
	agria.pop.population = agria.population;
	agria.pop.savings = 50000.0;
	agria.suitability[0] = kDemoReferenceWheatSuitability;
	agria.markets[0] = {100.0, demo_base_price(0), 0.0, 0.0};
	agria.markets[1] = {0.0, demo_base_price(1), 0.0, 0.0};
	agria.markets[2] = {0.0, demo_base_price(2), 0.0, 0.0};
	agria.markets[3] = {0.0, demo_base_price(3), 0.0, 0.0};
	demo_cities_.push_back(agria);

	DemoCity portus;
	portus.id = "PORTUS";
	portus.name = "Portus";
	portus.nation = "DEMO";
	portus.center = {2560.4, 543.8};
	portus.population = 200000.0;
	portus.firm_cash = 80000.0;
	portus.service_cash = 60000.0;
	portus.government_budget = 150000.0;
	portus.service_jobs = portus.population * kDemoServiceShare;
	portus.service_wage = kDemoServiceWage;
	portus.pop.population = portus.population;
	portus.pop.savings = 100000.0;
	portus.suitability[0] = 0.625;
	portus.markets[0] = {0.0, demo_base_price(0), 0.0, 0.0};
	portus.markets[1] = {0.0, demo_base_price(1), 0.0, 0.0};
	portus.markets[2] = {50.0, demo_base_price(2), 0.0, 0.0};
	portus.markets[3] = {0.0, demo_base_price(3), 0.0, 0.0};
	demo_cities_.push_back(portus);

	// Capacidade é o potencial da terra; a produção efetiva é multiplicada
	// pela aptidão territorial (wheat). Assim o encadeamento trigo -> farinha
	// -> pão fica ligado ao recurso e à geografia.
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

	// Portus também produz parte do trigo que processa: reduz a dependência
	// de importação e mantém a balança comercial próxima do equilíbrio.
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

	DemoBuilding mill;
	mill.id = "MILL-B";
	mill.city = "PORTUS";
	mill.kind = 1;
	mill.resource = "flour";
	mill.input_product = 0;
	mill.output_product = 1;
	mill.capacity = 50.0;
	mill.input_per_output = 1.2;
	mill.jobs = 200.0;
	mill.wage = 2.0;
	mill.opex = 200.0;
	demo_buildings_.push_back(mill);

	for (int index = 0; index < 5; ++index) {
		DemoBuilding bakery;
		bakery.id = "BAKERY-B-" + std::to_string(index + 1);
		bakery.city = "PORTUS";
		bakery.kind = 2;
		bakery.resource = "bread";
		bakery.input_product = 1;
		bakery.output_product = 2;
		bakery.capacity = 15.0;
		bakery.input_per_output = 0.83;
		bakery.jobs = 100.0;
		bakery.wage = 1.5;
		bakery.opex = 100.0;
		demo_buildings_.push_back(bakery);
	}
}

void EconomySim::initialize_demo_world(const Array& cities, const Array& ports,
		const Array& buildings) {
	demo_active_ = true;
	demo_world_mode_ = true;
	day_ = 0;
	demo_cities_.clear();
	demo_buildings_.clear();
	demo_in_transit_.clear();
	demo_invariant_violations_.clear();
	demo_ports_.clear();
	demo_last_orders_.clear();
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
		city.center = {static_cast<double>(d["x"]), static_cast<double>(d["y"])};

		// Caixas iniciais por habitante (constantes do modelo demo).
		city.firm_cash = city.population * kDemoFirmCashPerCapita;
		city.service_cash = city.population * kDemoServiceCashPerCapita;
		city.government_budget = city.population * kDemoGovernmentPerCapita;
		city.service_jobs = city.population * kDemoServiceShare;
		city.service_wage = kDemoServiceWage;
		city.pop.population = city.population;
		city.pop.savings = city.population * kDemoSavingsPerCapita;
		// Aptidão padrão: trigo 1.0 (o GeographyService sobrescreve com o dado
		// real em tempo de execução); ferro 0.0 até haver mina conhecida.
		city.suitability[0] = 1.0;
		city.suitability[1] = 1.0;
		city.suitability[2] = 1.0;
		city.suitability[3] = 0.0;
		for (int product = 0; product < kDemoProducts; ++product) {
			city.markets[product] = {0.0, demo_base_price(product), 0.0, 0.0};
		}
		demo_cities_.push_back(std::move(city));
	}

	// Construções reais (trigo): cada linha vira uma fazenda primária.
	for (int index = 0; index < buildings.size(); ++index) {
		const Dictionary d = buildings[index];
		DemoBuilding building;
		building.id = std::string(
			String(d.get("id", d.get("building_id", ""))).utf8().get_data());
		building.city = std::string(String(d["city_id"]).utf8().get_data());
		building.kind = 0;
		building.resource = "wheat";
		building.owner = "private";
		building.input_product = -1;
		building.output_product = 0;
		const double capacity =
			safe_nonneg(static_cast<double>(d["output_tons_per_day"]));
		building.capacity = capacity;
		building.input_per_output = 0.0;
		building.jobs = capacity * kDemoJobsPerTon;
		building.wage = kDemoFarmWage;
		building.opex = capacity * kDemoOpexPerTon;
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
}

EconomySim::DemoCity* EconomySim::find_demo_city(const std::string& id) {
	for (DemoCity& city : demo_cities_) {
		if (city.id == id) {
			return &city;
		}
	}
	return nullptr;
}

const EconomySim::DemoCity* EconomySim::find_demo_city(const std::string& id) const {
	for (const DemoCity& city : demo_cities_) {
		if (city.id == id) {
			return &city;
		}
	}
	return nullptr;
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

void EconomySim::demo_trade_wheat() {
	// Comércio genérico de trigo entre cidades (mundo real): cidades com déficit
	// de alimento compram das que têm excedente, mais próximas primeiro, limitadas
	// por caixa e estoque. O frete é serviço doméstico do comprador (conserva o
	// dinheiro). Espelha a regra do mercado legado, no modelo de dinheiro do demo.
	const int wheat = 0;

	struct Buyer {
		DemoCity* city;
		double gap;
	};
	std::vector<Buyer> buyers;
	std::vector<DemoCity*> sellers;
	for (DemoCity& city : demo_cities_) {
		// Cobertura-alvo em dias (como o mercado legado usa kCoverDays): a cidade
		// importa para manter um estoque de alguns dias, não só o déficit do dia —
		// senão a compra oscila entre sobra e falta. O que já está em trânsito
		// conta como cobertura.
		double in_transit_to = 0.0;
		for (const DemoShipment& shipment : demo_in_transit_) {
			if (shipment.to_city == city.id && shipment.product == wheat) {
				in_transit_to += shipment.amount;
			}
		}
		const double target = city.pop.need * demo_inventory_cover_days_;
		const double gap = target - (city.markets[wheat].stock + in_transit_to);
		if (gap > 1e-9) {
			buyers.push_back({&city, gap});
		}
		const double surplus =
			city.markets[wheat].stock - city.pop.need * kDemoTradeBufferDays;
		if (surplus > 1e-9) {
			sellers.push_back(&city);
		}
	}
	if (buyers.empty() || sellers.empty()) {
		return;
	}

	// Compradores com menor déficit primeiro: o buffer de uma cidade pequena é
	// minúsculo (ex.: Cayenne = 135 t) e barato de completar; na ordem descrescente
	// a maior cidade monta o buffer de 3 dias e espreme as pequenas, que ficam
	// sem nada. Ascendente garante que todos comam antes de alguém estocar.
	std::stable_sort(buyers.begin(), buyers.end(),
		[](const Buyer& left, const Buyer& right) { return left.gap < right.gap; });

	std::size_t shipments = 0;
	for (Buyer& buyer : buyers) {
		double remaining = buyer.gap;
		// Vendedores mais próximos primeiro.
		std::stable_sort(sellers.begin(), sellers.end(),
			[this, buyer](const DemoCity* left, const DemoCity* right) {
				return distance_km(buyer.city->center, left->center)
					< distance_km(buyer.city->center, right->center);
			});
		for (DemoCity* seller : sellers) {
			if (remaining <= 1e-9 || shipments >= kDemoMaxShipmentsPerDay) {
				break;
			}
			if (seller == buyer.city) {
				continue;
			}
			const double surplus = seller->markets[wheat].stock
				- seller->pop.need * kDemoTradeBufferDays;
			if (surplus <= 1e-9) {
				continue;
			}
			const double price = seller->markets[wheat].price;
			// Travessia entre massas de terra usa navio (5x mais barato por
			// tonelada-km); dentro da mesma massa de terra, caminhão.
			const bool same_landmass = seller->landmass == buyer.city->landmass;
			const double freight_rate = same_landmass
				? kDemoFreightPerTonKm : kDemoFreightShipPerTonKm;
			const double unit_freight =
				distance_km(seller->center, buyer.city->center) * freight_rate;
			const double unit_cost = price + unit_freight;
			const double amount_wanted = std::min(remaining, surplus);
			if (amount_wanted <= 1e-9) {
				continue;
			}
			// Segurança alimentar: o governo subsidia a parte da importação que a
			// firma não consegue pagar (cidade sem produção/caixa). Conserva
			// dinheiro — firma + governo pagam exatamente o custo (carga + frete).
			const double firm_cash = std::max(0.0, buyer.city->firm_cash);
			const double gov_budget = std::max(0.0, buyer.city->government_budget);
			const double payable = firm_cash + gov_budget;
			const double amount = unit_cost > 0.0
				? std::min(amount_wanted, payable / unit_cost) : 0.0;
			if (amount <= 1e-9) {
				continue;
			}
			const double total_cost = amount * unit_cost;
			const double firm_share = std::min(total_cost, firm_cash);
			const double gov_share = total_cost - firm_share;
			const double cost = amount * price;
			const double freight = unit_freight * amount;

			buyer.city->firm_cash -= firm_share;
			buyer.city->government_budget -= gov_share;
			seller->firm_cash += cost;
			buyer.city->service_cash += freight;
			buyer.city->freight_revenue += freight;

			seller->markets[wheat].stock -= amount;
			seller->markets[wheat].desired += amount;
			seller->markets[wheat].demand += amount;
			seller->markets[wheat].sold += amount;
			seller->revenue_by_product[wheat] += cost;
			seller->exported[wheat] += amount;
			seller->export_value[wheat] += cost;

			buyer.city->imported[wheat] += amount;
			buyer.city->import_value[wheat] += cost;

			demo_in_transit_.push_back({buyer.city->id, wheat, amount, day_ + 1});

			// Pedido de comércio com uma rota em linha reta (caminhão na mesma
			// massa de terra, navio entre massas), para os comboios 3D e o HUD.
			TradeOrder order;
			order.from_city = seller->id;
			order.to_city = buyer.city->id;
			order.from_nation = seller->nation;
			order.to_nation = buyer.city->nation;
			order.scope = MarketScope::International;
			order.amount = amount;
			const TransportMode mode = same_landmass
				? TransportMode::Truck : TransportMode::Ship;
			Leg leg = make_leg(mode, seller->id, buyer.city->id,
				seller->center, buyer.city->center, amount);
			order.transit_hours = leg.transit_hours;
			order.freight_cost = leg.cost;
			order.legs.push_back(std::move(leg));
			demo_last_orders_.push_back(std::move(order));

			remaining -= amount;
			++shipments;
		}
	}
}

Dictionary EconomySim::tick_demo() {
	if (!demo_active_) {
		initialize_demo();
	}

	// Retrato para reverter o tick se um invariante rígido for violado.
	DemoSnapshot before;
	demo_snapshot(before);
	const double money_before = demo_money_total();

	++day_;
	demo_last_orders_.clear();

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

	// 3. Mercado de trabalho e salários.
	for (DemoCity& city : demo_cities_) {
		double modeled_jobs = 0.0;
		for (const DemoBuilding& building : demo_buildings_) {
			if (building.city == city.id) {
				modeled_jobs += building.jobs;
			}
		}
		// O emprego de serviços é persistente: cresce ou recua conforme a
		// receita do setor, em vez de ser recalculado a cada dia.
		const double labor_force = city.population * kDemoLaborShare;
		city.pop.labor_force = labor_force;

		const double available = std::max(0.0, labor_force - city.service_jobs);
		const double fill = modeled_jobs > 0.0
			? std::clamp(available / modeled_jobs, 0.0, 1.0) : 0.0;

		double modeled_wages = 0.0;
		double opex_total = 0.0;
		for (DemoBuilding& building : demo_buildings_) {
			if (building.city != city.id) {
				continue;
			}
			building.workers = building.jobs * fill;
			modeled_wages += building.workers * building.wage;
			opex_total += building.opex;
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
		for (DemoBuilding& building : demo_buildings_) {
			if (building.city != city.id) {
				continue;
			}
			building.paid_wages = building.workers * building.wage * pay_scale;
			building.paid_opex = building.opex * pay_scale;
		}

		city.service_wage_bill = city.service_jobs * city.service_wage;
		// O setor de serviços não paga folha sem caixa: a folha é limitada ao
		// saldo disponível no início do dia, sem crédito implícito.
		city.service_wage_bill = std::min(city.service_wage_bill,
				std::max(0.0, city.service_cash));
		city.pop.employed = city.service_jobs + modeled_jobs * fill;
		city.pop.unemployed = std::max(0.0, labor_force - city.pop.employed);
		city.pop.dependents = std::max(0.0, city.population - labor_force);
		city.pop.wage_income = modeled_wages + city.service_wage_bill;

		city.firm_cash -= modeled_wages + opex_total;
		city.service_cash -= city.service_wage_bill;
		// Opex é comprado de serviços domésticos (manutenção, energia),
		// não é uma importação: mantém o dinheiro dentro da economia.
		city.service_cash += opex_total;
		city.opex_revenue = opex_total;
	}

	// 4. Produção: cada construção declara insumo e produto. A produção segue
	// as vendas do dia anterior (com folga de estoque), limitada pela
	// capacidade, pela aptidão territorial e pelos insumos disponíveis.
	for (DemoBuilding& building : demo_buildings_) {
		DemoCity* city = find_demo_city(building.city);
		if (city == nullptr || building.output_product < 0
				|| building.output_product >= kDemoProducts) {
			continue;
		}

		double city_capacity = 0.0;
		for (const DemoBuilding& other : demo_buildings_) {
			if (other.city == city->id
					&& other.output_product == building.output_product) {
				city_capacity += other.capacity * other.output_scale;
			}
		}
		const double capacity_share = city_capacity > 0.0
			? building.capacity * building.output_scale / city_capacity : 0.0;
		const double sold_prev = city->markets[building.output_product].sold_prev;
		const double labor = building.jobs > 0.0
			? building.workers / building.jobs : 1.0;
		const double suitability = building.input_product < 0
			? std::clamp(city->suitability[building.output_product], 0.0, 1.0)
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
			// A receita do setor fornecedor inclui a transferência interna (ex.:
			// farinha do moinho consumida pela padaria), senão o valor adicionado
			// do moinho ficaria negativo.
			city->revenue_by_product[building.input_product] += input_used
				* input_market.price;
			building.input_value = input_used * input_market.price;
		}

		city->markets[building.output_product].stock += output;
		city->markets[building.output_product].supply += output;
		building.utilization = building.capacity > 0.0
			? output / building.capacity : 0.0;
		demo_produced_[building.output_product] += output;
		demo_produced_today_[building.output_product] += output;
	}

	// 4b. Agria importa pão de Portus antes do consumo local: fecha a balança
	// comercial e dá demanda à capacidade extra de panificação.
	{
		DemoCity* agria = find_demo_city("AGRIA");
		DemoCity* portus = find_demo_city("PORTUS");
		if (agria != nullptr && portus != nullptr) {
			const double agria_need = agria->population * kDemoNeedPerCapita;
			const double bread_gap = std::max(0.0,
				agria_need - agria->markets[2].stock);
			const double unit_freight = distance_km(agria->center, portus->center)
				* kDemoFreightPerTonKm;
			const double unit_cost = portus->markets[2].price + unit_freight;
			const double affordable = unit_cost > 0.0
				? std::max(0.0, agria->firm_cash) / unit_cost : 0.0;
			const double amount = std::min({bread_gap,
				portus->markets[2].stock * 0.8, affordable,
				kDemoBreadExportTons});
			if (amount > 0.0) {
				const double price = portus->markets[2].price;
				const double cost = amount * price;
				const double freight = unit_freight * amount;

				agria->firm_cash -= cost + freight;
				portus->firm_cash += cost;
				// O frete é um serviço doméstico prestado a Agria.
				agria->service_cash += freight;
				agria->freight_revenue += freight;
				portus->markets[2].stock -= amount;
				portus->markets[2].desired += amount;
				portus->markets[2].demand += amount;
				portus->markets[2].sold += amount;
				portus->revenue_by_product[2] += cost;
				// Portus exporta pão; Agria importa.
				portus->exported[2] += amount;
				portus->export_value[2] += cost;
				agria->imported[2] += amount;
				agria->import_value[2] += cost;
				demo_in_transit_.push_back({agria->id, 2, amount, day_ + 1});
			}
		}
	}

	// 4c. Demanda externa de ferro: o comprador externo absorve o estoque.
	for (DemoCity& city : demo_cities_) {
		DemoMarket& iron = city.markets[3];
		iron.desired += kDemoIronExportDemand;
		const double sold = std::min(iron.stock, kDemoIronExportDemand);
		if (sold <= 0.0) {
			continue;
		}
		const double revenue = sold * iron.price;
		iron.stock -= sold;
		iron.demand += sold;
		iron.sold += sold;
		city.firm_cash += revenue;
		city.revenue_by_product[3] += revenue;
		city.exported[3] += sold;
		city.export_value[3] += revenue;
		demo_foreign_cash_ -= revenue;
		demo_consumed_total_[3] += sold;
		demo_consumed_today_[3] += sold;
	}

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
		pop.service_spend = pop.disposable * kDemoServiceSpendShare;
		city.service_cash += pop.service_spend;
		city.government_budget += pop.taxes - pop.transfers;

		double budget = pop.savings + pop.disposable - pop.service_spend;
		pop.need = pop.population * kDemoNeedPerCapita;
		// Subsídio de subsistência: o governo garante a cesta alimentar mínima
		// quando o orçamento de comida não cobre (ex.: cidade sem produção nem
		// renda própria). Sai do orçamento público — conserva o dinheiro. Se as
		// reservas públicas esgotarem, o subsídio cessa (a redistribuição fiscal
		// nacional, que o tornaria sustentável, fica para o 1.8).
		double cheapest_food = 0.0;
		for (int product = 0; product < kDemoFoodProducts; ++product) {
			const double price = city.markets[product].price;
			if (price > 0.0 && (cheapest_food <= 0.0 || price < cheapest_food)) {
				cheapest_food = price;
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
		double consumed = 0.0;
		double food_spend = 0.0;
		// Preferência: pão, farinha, trigo. A demanda desejada é registrada
		// mesmo quando não há estoque; a compra realizada é o que cabe no
		// estoque e no orçamento.
		const int preference[3] = {2, 1, 0};
		for (int slot = 0; slot < 3 && consumed < pop.need; ++slot) {
			const int product = preference[slot];
			DemoMarket& market = city.markets[product];
			const double price = market.price;
			if (price <= 0.0) {
				continue;
			}
			const double desired_quantity = std::min(
				pop.need - consumed, std::max(0.0, budget) / price);
			if (desired_quantity <= 0.0) {
				continue;
			}
			market.desired += desired_quantity;
			pop.desired_by_category[product] += desired_quantity;
			const double quantity = std::min(desired_quantity, market.stock);
			if (quantity <= 0.0) {
				continue;
			}
			market.stock -= quantity;
			market.demand += quantity;
			market.sold += quantity;
			budget -= quantity * price;
			food_spend += quantity * price;
			consumed += quantity;
			pop.consumed_by_category[product] += quantity;
			city.revenue_by_product[product] += quantity * price;
			city.firm_cash += quantity * price;
			demo_consumed_total_[product] += quantity;
			demo_consumed_today_[product] += quantity;
		}
		pop.consumed = consumed;
		pop.food_spend = food_spend;
		pop.satisfaction = pop.need > 0.0
			? std::clamp(consumed / pop.need, 0.0, 1.0) : 1.0;
		pop.savings = budget;
	}

	// 6. Comércio: o moinho de Portus compra trigo de Agria para o dia seguinte.
	{
		DemoCity* agria = find_demo_city("AGRIA");
		DemoCity* portus = find_demo_city("PORTUS");
		if (agria != nullptr && portus != nullptr) {
			// O moinho repõe o trigo necessário para a produção de farinha,
			// mirando um estoque de alguns dias em vez de comprar 10% acima
			// das vendas para sempre (o que acumulava trigo sem limite).
			const double flour_sold = portus->markets[1].sold;
			const double mill_daily = (flour_sold > 0.0 ? flour_sold : 50.0) * 1.2;
			const double wheat_target = mill_daily * demo_inventory_cover_days_;
			const double missing = wheat_target - portus->markets[0].stock;
			if (missing > 0.0 && agria->markets[0].stock > 0.0) {
				const double price = agria->markets[0].price;
				const double unit_freight = distance_km(agria->center, portus->center)
					* kDemoFreightPerTonKm;
				const double unit_cost = price + unit_freight;
				// O comprador não pode gastar mais do que o caixa disponível:
				// sem este limite o comércio deficitário levava o caixa a
				// negativo e a economia seguia comprando a crédito infinito.
				const double affordable = unit_cost > 0.0
					? std::max(0.0, portus->firm_cash) / unit_cost : 0.0;
				const double amount = std::min({missing,
					agria->markets[0].stock, affordable});
				if (amount > 0.0) {
					const double cost = amount * price;
					const double freight = unit_freight * amount;

					portus->firm_cash -= cost + freight;
					agria->firm_cash += cost;
					// O frete é um serviço doméstico prestado a Portus.
					portus->service_cash += freight;
					portus->freight_revenue += freight;
					agria->markets[0].stock -= amount;
					agria->markets[0].desired += amount;
					agria->markets[0].demand += amount;
					agria->markets[0].sold += amount;
					agria->revenue_by_product[0] += cost;
					agria->exported[0] += amount;
					agria->export_value[0] += cost;
					portus->imported[0] += amount;
					portus->import_value[0] += cost;
					// Preço de Portus é a média ponderada entre estoque local e
					// custo posto (importação), em vez de sobrescrever o local.
					const double imported_unit = (cost + freight) / amount;
					const double local_stock = portus->markets[0].stock;
					const double total = local_stock + amount;
					const double previous_price = portus->markets[0].price;
					const double blended = total > 0.0
						? (local_stock * previous_price
							+ amount * imported_unit) / total
						: imported_unit;
					// O limite diário de 5% também vale para o preço posto
					// pela importação; o clamp final do tick repete a regra.
					const double wheat_base = demo_base_price(0);
					const double wheat_step = wheat_base * 0.05;
					const double smoothed = std::clamp(blended,
						previous_price - wheat_step, previous_price + wheat_step);
					portus->markets[0].price = std::clamp(smoothed,
						wheat_base * 0.5, wheat_base * 3.0);
					demo_in_transit_.push_back(
						{portus->id, 0, amount, day_ + 1});
				}
			}
		}
	}

	// 6b. Comércio genérico de trigo entre as cidades do mundo real. No cenário
	// de validação (AGRIA/PORTUS) o bloco 6 acima já resolve o trigo; aqui a
	// regra genérica cobre qualquer número de cidades.
	if (demo_world_mode_) {
		demo_trade_wheat();
	}

	// 7. Receita por construção, lucro, distribuição de capital e PIB.
	for (DemoCity& city : demo_cities_) {
		double capacity_by_output[kDemoProducts] = {0.0, 0.0, 0.0, 0.0};
		for (const DemoBuilding& building : demo_buildings_) {
			if (building.city == city.id) {
				capacity_by_output[building.output_product] +=
					building.capacity * building.output_scale;
			}
		}
		for (DemoBuilding& building : demo_buildings_) {
			if (building.city != city.id) {
				continue;
			}
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
		for (const DemoBuilding& building : demo_buildings_) {
			if (building.city == city.id) {
				if (building.owner == "state") {
					state_profit += building.profit;
				} else {
					private_profit += building.profit;
				}
				value_added += building.revenue - building.input_value;
			}
		}

		// Dividendos só depois de reconstituir o capital de giro. Lucros de
		// estatais vão para o orçamento público; privados, para as famílias.
		const double firm_target = city.population * kDemoFirmCashPerCapita;
		const double firm_surplus = std::max(0.0, city.firm_cash - firm_target);
		const double positive_total = std::max(0.0, private_profit)
			+ std::max(0.0, state_profit);
		const double distributable = std::min(
			positive_total * kDemoCapitalDistribution, firm_surplus);
		const double private_share = positive_total > 0.0
			? std::max(0.0, private_profit) / positive_total : 0.0;
		const double modeled_distribution = distributable * private_share;
		const double state_distribution = distributable - modeled_distribution;
		const double service_target = city.population * kDemoServiceCashPerCapita;
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
		for (DemoBuilding& building : demo_buildings_) {
			if (building.city != city.id) {
				continue;
			}
			if (building.profit < 0.0) {
				++building.loss_days;
			} else {
				building.loss_days = 0;
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
		for (int product = 0; product < kDemoFoodProducts; ++product) {
			const double price = city.markets[product].price;
			if (price > 0.0 && (cheapest_food <= 0.0 || price < cheapest_food)) {
				cheapest_food = price;
			}
		}
		const double subsistence_wage = kDemoNeedPerCapita * cheapest_food;
		double modeled_jobs_total = 0.0;
		for (const DemoBuilding& building : demo_buildings_) {
			if (building.city == city.id) {
				modeled_jobs_total += building.jobs;
			}
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

	// 8. Preços reagem a estoque, oferta, demanda e custo de produção.
	for (DemoCity& city : demo_cities_) {
		bool has_producer[kDemoProducts] = {false, false, false, false};
		for (const DemoBuilding& building : demo_buildings_) {
			if (building.city == city.id && building.output_product >= 0
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
			for (const DemoBuilding& building : demo_buildings_) {
				if (building.city == city.id && building.output_product == product) {
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
			for (const DemoBuilding& building : demo_buildings_) {
				if (building.city == city.id && building.output_product == product
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

	return get_demo_status();
}

Dictionary EconomySim::demo_city_to_dict(const DemoCity& city) const {
	Dictionary dict;
	dict["id"] = String(city.id.c_str());
	dict["name"] = String(city.name.c_str());
	// Campos de compatibilidade com o formato legado (city_to_dict), para a
	// camada visual (city_world, map_mode_layer, urban) ler o mesmo status.
	dict["nation"] = String(city.nation.c_str());
	dict["home_nation"] = String(city.home_nation.c_str());
	dict["landmass"] = String(city.landmass.c_str());
	dict["x"] = city.center.x;
	dict["y"] = city.center.y;
	Array center;
	center.append(city.center.x);
	center.append(city.center.y);
	dict["center"] = center;
	dict["population"] = city.population;
	dict["gdp"] = city.gdp;
	dict["gdp_per_capita"] = city.population > 0.0
		? city.gdp / city.population : 0.0;
	dict["firm_cash"] = city.firm_cash;
	dict["service_cash"] = city.service_cash;
	dict["government_budget"] = city.government_budget;
	dict["service_revenue"] = city.service_revenue;
	dict["service_profit"] = city.service_profit;
	dict["opex_revenue"] = city.opex_revenue;
	dict["freight_revenue"] = city.freight_revenue;
	dict["construction_revenue"] = city.construction_revenue;
	dict["service_wage"] = city.service_jobs > 0.0
		? city.service_wage_bill / city.service_jobs : 0.0;
	dict["disposable_per_capita"] = city.population > 0.0
		? city.pop.disposable / city.population : 0.0;

	// Padrão de vida: quantas cestas alimentares diárias a renda disponível
	// por pessoa compra. Diferente do PIB per capita, usa renda das famílias.
	double cheapest_price = 0.0;
	for (int product = 0; product < kDemoFoodProducts; ++product) {
		const double price = city.markets[product].price;
		if (price > 0.0 && (cheapest_price <= 0.0 || price < cheapest_price)) {
			cheapest_price = price;
		}
	}
	const double basket_cost = kDemoNeedPerCapita * cheapest_price;
	const double living_standard = basket_cost > 0.0
		? (city.pop.disposable / city.population) / basket_cost : 0.0;
	dict["living_standard"] = living_standard;

	// Indicadores derivados, no espírito do formato legado, para a camada
	// visual: produção primária (t/dia), empregos, renda relativa, grau de
	// industrialização, estoque de trigo e necessidade diária.
	double production = 0.0;
	double secondary_capacity = 0.0;
	double total_capacity = 0.0;
	for (const DemoBuilding& building : demo_buildings_) {
		if (building.city != city.id) {
			continue;
		}
		total_capacity += building.capacity * building.output_scale;
		if (building.input_product < 0) {
			production += building.capacity * building.output_scale;
		} else {
			secondary_capacity += building.capacity * building.output_scale;
		}
	}
	dict["production"] = production;
	dict["jobs"] = city.pop.employed;
	dict["income"] = std::clamp(living_standard, 0.0, 1.0);
	dict["industrialization"] = total_capacity > 0.0
		? std::clamp(secondary_capacity / total_capacity, 0.0, 1.0) : 0.0;
	dict["stockpile"] = city.markets[0].stock;
	dict["need"] = city.pop.need;
	dict["deficit"] = std::max(0.0, city.pop.need - city.pop.consumed);
	dict["consumed"] = city.pop.consumed;
	double in_transit = 0.0;
	for (const DemoShipment& shipment : demo_in_transit_) {
		if (shipment.to_city == city.id) {
			in_transit += shipment.amount;
		}
	}
	dict["in_transit"] = in_transit;

	Array suitability;
	for (int product = 0; product < kDemoProducts; ++product) {
		suitability.append(city.suitability[product]);
	}
	dict["suitability"] = suitability;

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
	dict["pop"] = pop;

	Array markets;
	for (int product = 0; product < kDemoProducts; ++product) {
		Dictionary market;
		market["product"] = product;
		market["stock"] = city.markets[product].stock;
		market["price"] = city.markets[product].price;
		market["demand"] = city.markets[product].demand;
		market["supply"] = city.markets[product].supply;
		market["desired"] = city.markets[product].desired;
		market["sold"] = city.markets[product].sold;
		market["shortage"] = city.markets[product].shortage;
		market["surplus"] = city.markets[product].surplus;
		markets.append(market);
	}
	dict["markets"] = markets;

	Array imported;
	Array exported;
	Array import_value;
	Array export_value;
	for (int product = 0; product < kDemoProducts; ++product) {
		imported.append(city.imported[product]);
		exported.append(city.exported[product]);
		import_value.append(city.import_value[product]);
		export_value.append(city.export_value[product]);
	}
	dict["imported"] = imported;
	dict["exported"] = exported;
	dict["import_value"] = import_value;
	dict["export_value"] = export_value;

	Array buildings;
	for (const DemoBuilding& building : demo_buildings_) {
		if (building.city != city.id) {
			continue;
		}
		Dictionary entry;
		entry["id"] = String(building.id.c_str());
		entry["kind"] = building.kind;
		entry["resource"] = String(building.resource.c_str());
		entry["owner"] = String(building.owner.c_str());
		entry["input_product"] = building.input_product;
		entry["output_product"] = building.output_product;
		entry["capacity"] = building.capacity;
		entry["jobs"] = building.jobs;
		entry["wage"] = building.wage;
		entry["opex"] = building.opex;
		entry["workers"] = building.workers;
		entry["utilization"] = building.utilization;
		entry["output_scale"] = building.output_scale;
		entry["loss_days"] = building.loss_days;
		entry["revenue"] = building.revenue;
		entry["input_value"] = building.input_value;
		entry["costs"] = building.costs;
		entry["profit"] = building.profit;
		entry["site_suitability"] = city.suitability[building.output_product];
		buildings.append(entry);
	}
	dict["buildings"] = buildings;
	return dict;
}

Dictionary EconomySim::get_demo_status() const {
	Dictionary result;
	result["mode"] = "demo";
	result["day"] = day_;
	result["calendar_day"] = day_;
	result["money_total"] = demo_money_total();
	result["transport_cash"] = demo_transport_cash_;
	result["foreign_cash"] = demo_foreign_cash_;
	result["construction_cash"] = demo_construction_cash_;

	Array products;
	products.append("wheat");
	products.append("flour");
	products.append("bread");
	products.append("iron");
	result["products"] = products;
	result["capital_cost_per_capacity"] = kDemoCapitalPerCapacity;

	Array violations;
	for (const std::string& violation : demo_invariant_violations_) {
		violations.append(String(violation.c_str()));
	}
	result["invariant_violations"] = violations;

	Array cities;
	for (const DemoCity& city : demo_cities_) {
		cities.append(demo_city_to_dict(city));
	}
	result["cities"] = cities;

	// Portos (modo mundo) para o minimapa e o mundo 3D.
	Array ports;
	for (const Port& port : demo_ports_) {
		ports.append(port_to_dict(port));
	}
	result["ports"] = ports;

	// Pedidos de comércio do último tick (comboios 3D e HUD).
	Array orders;
	for (const TradeOrder& order : demo_last_orders_) {
		orders.append(order_to_dict(order));
	}
	result["trade_orders"] = orders;

	// Nações agregadas (telas de política/economia), na ordem de aparição.
	Array nations;
	std::vector<std::string> nation_order;
	for (const DemoCity& city : demo_cities_) {
		if (std::find(nation_order.begin(), nation_order.end(), city.nation)
				== nation_order.end()) {
			nation_order.push_back(city.nation);
		}
	}
	static const std::pair<const char*, const char*> kNames[] = {
		{"GBR", "England"}, {"FRA", "France"}, {"DEU", "Germany"},
		{"NED", "Netherlands"}, {"BEL", "Belgium"},
	};
	for (const std::string& tag : nation_order) {
		Dictionary nation;
		nation["tag"] = String(tag.c_str());
		const char* display = tag.c_str();
		for (const auto& [code, name] : kNames) {
			if (tag == code) {
				display = name;
			}
		}
		nation["name"] = String(display);
		double population = 0.0;
		double production = 0.0;
		double stockpile = 0.0;
		double need = 0.0;
		double deficit = 0.0;
		double in_transit = 0.0;
		int city_count = 0;
		for (const DemoCity& city : demo_cities_) {
			if (city.nation != tag) {
				continue;
			}
			++city_count;
			population += city.population;
			stockpile += city.markets[0].stock;
			need += city.pop.need;
			deficit += std::max(0.0, city.pop.need - city.pop.consumed);
			for (const DemoBuilding& building : demo_buildings_) {
				if (building.city == city.id && building.input_product < 0) {
					production += building.capacity * building.output_scale;
				}
			}
		}
		for (const DemoShipment& shipment : demo_in_transit_) {
			const DemoCity* city = find_demo_city(shipment.to_city);
			if (city != nullptr && city->nation == tag) {
				in_transit += shipment.amount;
			}
		}
		nation["city_count"] = city_count;
		nation["population"] = population;
		nation["production"] = production;
		nation["stockpile"] = stockpile;
		nation["need"] = need;
		nation["deficit"] = deficit;
		nation["in_transit"] = in_transit;
		nations.append(nation);
	}
	result["nations"] = nations;

	Array produced;
	Array consumed;
	Array produced_today;
	Array consumed_today;
	for (int index = 0; index < kDemoProducts; ++index) {
		produced.append(demo_produced_[index]);
		consumed.append(demo_consumed_total_[index]);
		produced_today.append(demo_produced_today_[index]);
		consumed_today.append(demo_consumed_today_[index]);
	}
	result["produced_total"] = produced;
	result["consumed_total"] = consumed;
	result["produced_today"] = produced_today;
	result["consumed_today"] = consumed_today;
	return result;
}

void EconomySim::demo_price_shock(const String& product, double multiplier) {
	int index = demo_product_index(product);
	if (index < 0) {
		return;
	}
	for (DemoCity& city : demo_cities_) {
		const double base = demo_base_price(index);
		city.markets[index].price = std::clamp(
			city.markets[index].price * multiplier, base * 0.1, base * 10.0);
	}
}

int EconomySim::demo_product_index(const String& product) {
	if (product == "flour") {
		return 1;
	}
	if (product == "bread") {
		return 2;
	}
	if (product == "iron") {
		return 3;
	}
	if (product == "wheat") {
		return 0;
	}
	return -1;
}

const char* EconomySim::demo_product_name(int product) {
	switch (product) {
		case 1:
			return "flour";
		case 2:
			return "bread";
		case 3:
			return "iron";
		default:
			return "wheat";
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
}

void EconomySim::demo_set_suitability(const String& city_id, const String& product,
		double value) {
	const int index = demo_product_index(product);
	if (index < 0) {
		return;
	}
	DemoCity* city = find_demo_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr) {
		return;
	}
	city->suitability[index] = std::clamp(value, 0.0, 1.0);
}

Dictionary EconomySim::demo_invest(const String& city_id, const String& building_id,
		double amount) {
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
	return result;
}

Dictionary EconomySim::demo_build(const String& city_id, const String& building_id,
		const String& resource, int input_product, int output_product,
		double capacity, double jobs, double wage, double opex,
		double input_per_output, const String& owner, double cost) {
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
	building.kind = output_product;
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
	city->service_cash += cost;
	city->construction_revenue_pending += cost;
	result["created"] = true;
	result["cost"] = cost;
	return result;
}

bool EconomySim::demo_spend_government(const String& city_id, double amount) {
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
	return true;
}

Dictionary EconomySim::demo_to_dict() const {
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

		Array suitability;
		Array revenue;
		Array imported;
		Array exported;
		Array import_value;
		Array export_value;
		for (int product = 0; product < kDemoProducts; ++product) {
			suitability.append(city.suitability[product]);
			revenue.append(city.revenue_by_product[product]);
			imported.append(city.imported[product]);
			exported.append(city.exported[product]);
			import_value.append(city.import_value[product]);
			export_value.append(city.export_value[product]);
		}
		entry["suitability"] = suitability;
		entry["revenue_by_product"] = revenue;
		entry["imported"] = imported;
		entry["exported"] = exported;
		entry["import_value"] = import_value;
		entry["export_value"] = export_value;

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
	// Recusa save de versão futura (maior que a que este binário conhece).
	const int version = int(data.get("version", 0));
	if (version > kDemoSaveVersion) {
		demo_active_ = false;
		return;
	}
	demo_active_ = bool(data.get("active", true));
	demo_world_mode_ = bool(data.get("world_mode", false));
	day_ = int(data.get("day", 0));
	demo_transport_cash_ = double(data.get("transport_cash", 0.0));
	demo_foreign_cash_ = double(data.get("foreign_cash", 0.0));
	demo_construction_cash_ = double(data.get("construction_cash", 0.0));

	Array produced = data.get("produced_total", Array());
	Array consumed = data.get("consumed_total", Array());
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
		city.firm_cash = double(entry.get("firm_cash", 0.0));
		city.service_cash = double(entry.get("service_cash", 0.0));
		city.government_budget = double(entry.get("government_budget", 0.0));
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
		for (int product = 0; product < kDemoProducts; ++product) {
			city.suitability[product] = product < suitability.size()
				? double(suitability[product]) : (product == 3 ? 0.0 : 1.0);
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
		}

		Dictionary pop = entry.get("pop", Dictionary());
		city.pop.population = double(pop.get("population", city.population));
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
		shipment.amount = double(entry.get("amount", 0.0));
		shipment.arrival_day = int(entry.get("arrival_day", 0));
		demo_in_transit_.push_back(shipment);
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
		entry.firm_cash = 50000.0;
		entry.service_cash = 30000.0;
		entry.government_budget = 100000.0;
		entry.service_jobs = entry.population * kDemoServiceShare;
		entry.service_wage = kDemoServiceWage;
		entry.savings = 50000.0;

		BenchMarket& wheat = bench_markets_[bench_market(city, 0)];
		wheat.stock = 100.0;
		wheat.price = demo_base_price(0);
		BenchMarket& flour = bench_markets_[bench_market(city, 1)];
		flour.stock = 0.0;
		flour.price = demo_base_price(1);
		BenchMarket& bread = bench_markets_[bench_market(city, 2)];
		bread.stock = 50.0;
		bread.price = demo_base_price(2);

		for (int slot = 0; slot < bench_buildings_per_city_; ++slot) {
			BenchBuilding building;
			building.city = city;
			building.output = slot < 3 ? slot : (slot - 3) % kBenchProducts;
			if (building.output == 0) {
				building.input = -1;
				building.capacity = 20.0;
				building.input_per_output = 0.0;
				building.jobs = 150.0;
				building.wage = 2.0;
				building.opex = 50.0;
			} else if (building.output == 1) {
				building.input = 0;
				building.capacity = 50.0;
				building.input_per_output = 1.2;
				building.jobs = 200.0;
				building.wage = 2.0;
				building.opex = 200.0;
			} else {
				building.input = 1;
				building.capacity = 15.0;
				building.input_per_output = 0.83;
				building.jobs = 100.0;
				building.wage = 1.5;
				building.opex = 100.0;
			}
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
		const double labor_force = entry.population * kDemoLaborShare;
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
	const int preference[3] = {2, 1, 0};
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
		entry.need = entry.population * kDemoNeedPerCapita;
		double consumed = 0.0;
		double food_spend = 0.0;
		for (int slot = 0; slot < 3 && consumed < entry.need; ++slot) {
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

		const double firm_target = entry.population * kDemoFirmCashPerCapita;
		const double firm_surplus = std::max(0.0, entry.firm_cash - firm_target);
		const double modeled_distribution =
			std::min(std::max(0.0, private_profit), firm_surplus);
		const double service_target = entry.population * kDemoServiceCashPerCapita;
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
			const double lower = entry.population * kDemoServiceMinShare;
			const double upper = entry.population * kDemoServiceMaxShare;
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
			if (product == 1) {
				cost_floor = bench_markets_[bench_market(city, 0)].price * 1.2
					+ unit_labor_opex;
			} else if (product == 2) {
				cost_floor = bench_markets_[bench_market(city, 1)].price * 0.83
					+ unit_labor_opex;
			}
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
		const double target_cash = entry.population * kDemoFirmCashPerCapita;
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
	products.append("flour");
	products.append("bread");
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