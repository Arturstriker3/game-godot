#include "economy_sim.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace godot {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kEarthRadiusKm = 6371.0;
constexpr double kMapWidth = 5000.0;
constexpr double kMapHeight = 2500.0;
constexpr double kHoursPerDay = 24.0;

// 0,3 kg de trigo por pessoa por dia, em toneladas.
constexpr double kNeedPerCapita = 0.0003;

// Abaixo disso a cidade está em emergência e paga o frete aéreo.
constexpr double kEmergencyCoverDays = 2.0;
constexpr double kEmergencyAirShare = 0.35;

// Limite de pedidos por dia (proteção); os comboios exibidos têm limite próprio.
constexpr std::size_t kMaxOrdersPerDay = 24;

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
	ClassDB::bind_method(D_METHOD("initialize"), &EconomySim::initialize);
	ClassDB::bind_method(D_METHOD("tick"), &EconomySim::tick);
	ClassDB::bind_method(D_METHOD("get_status"), &EconomySim::get_status);
}

const EconomySim::ModeSpec& EconomySim::spec(TransportMode mode) {
	// capacidade de lote = carga útil de uma composição/frota padrão.
	// custo em unidades monetárias por tonelada-quilômetro.
	static const ModeSpec kTruck{TransportMode::Truck, "truck", 65.0, 1200.0, 0.10};
	static const ModeSpec kShip{TransportMode::Ship, "ship", 35.0, 12000.0, 0.02};
	static const ModeSpec kPlane{TransportMode::Plane, "plane", 750.0, 400.0, 0.80};

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

void EconomySim::initialize() {
	in_transit_.clear();
	last_orders_.clear();
	day_ = 0;
	clock_hours_ = 0.0;
	define_cities();
}

void EconomySim::define_cities() {
	cities_.clear();

	// Uma cidade por nação. Coordenadas = centro real da cidade projetado no
	// mapa (5000x2500 px) na projeção equiretangular do ne2.png.
	// Produção é a capacidade diária de trigo em toneladas.
	cities_ = {
		{"GBR-LDN", "GBR", "London", 9.0e6, 1300.0, 0.0, {2498.2, 534.6}},
		{"FRA-PAR", "FRA", "Paris", 12.0e6, 6000.0, 0.0, {2532.6, 571.4}},
		{"DEU-BER", "DEU", "Berlin", 6.0e6, 2500.0, 0.0, {2686.2, 520.6}},
		{"NED-AMS", "NED", "Amsterdam", 2.5e6, 300.0, 0.0, {2568.0, 522.7}},
		{"BEL-BRU", "BEL", "Brussels", 2.5e6, 400.0, 0.0, {2560.4, 543.8}},
	};
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
		const double open_deficit = city.deficit - pending;
		if (open_deficit > 1e-9) {
			buyers.emplace_back(&city, open_deficit);
		}
	}

	// Compradores mais urgentes primeiro; vendedores com mais trigo primeiro.
	std::sort(buyers.begin(), buyers.end(),
	          [](const auto& left, const auto& right) { return left.second > right.second; });
	std::sort(sellers.begin(), sellers.end(),
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
	result["day"] = day_;

	Array cities_array;
	for (const City& city : cities_) {
		cities_array.append(city_to_dict(city));
	}
	result["cities"] = cities_array;

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
	const bool crosses_sea = (seller.nation != "GBR") != (buyer.nation != "GBR");

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
	// Garante que a rota sempre comece no continente e termine na ilha.
	const City& origin = seller.nation == "GBR" ? buyer : seller;
	const City& destination = seller.nation == "GBR" ? seller : buyer;

	return {
		make_leg(TransportMode::Truck, origin.name, "Canal port", origin.center, france_port_, amount),
		make_leg(TransportMode::Ship, "Canal port", "Channel crossing", france_port_, england_port_, amount),
		make_leg(TransportMode::Truck, "Channel crossing", destination.name, england_port_,
		         destination.center, amount),
	};
}

EconomySim::Leg EconomySim::make_leg(TransportMode mode, const std::string& from, const std::string& to,
                                     const MapPoint& from_point, const MapPoint& to_point,
                                     double load_tons) const {
	const ModeSpec& mode_spec = spec(mode);
	const double distance = distance_km(from_point, to_point);

	// Viagens necessárias para escoar o lote; o prazo considera a última delas.
	const int trips = std::max(1, static_cast<int>(std::ceil(load_tons / mode_spec.batch_capacity_tons)));
	const double hours = distance / mode_spec.speed_kmh * static_cast<double>(trips);

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
	dict["name"] = String(city.name.c_str());
	dict["population"] = city.population;
	dict["need"] = city.need;
	dict["production"] = city.production;
	dict["consumed"] = city.consumed;
	dict["deficit"] = city.deficit;
	dict["dispatched"] = city.dispatched;
	dict["in_transit"] = in_transit_total(city.id);
	dict["stockpile"] = city.stockpile;
	dict["x"] = city.center.x;
	dict["y"] = city.center.y;
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

} // namespace godot
