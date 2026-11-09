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

// 0.3 kg de trigo por pessoa por dia, em toneladas.
constexpr double kNeedPerCapita = 0.0003;

// Abaixo disso o comprador está em emergência e paga o frete aéreo.
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
	define_states();
}

void EconomySim::define_states() {
	states_.clear();

	// França: maior produtor de trigo, com destaque para a Beauce.
	// Coordenadas = centro real da regiao projetado no mapa (5000x2500 px).
	states_ = {
		{"FRA-IDF", "FRA", "Ile-de-France", 16.0e6, 1500.0, 0.0, {2532.6, 571.5}},
		{"FRA-BCE", "FRA", "Beauce", 2.0e6, 16000.0, 0.0, {2520.7, 577.1}},
		{"FRA-NOR", "FRA", "Normandie", 3.5e6, 8000.0, 0.0, {2494.9, 566.9}},
		{"FRA-EST", "FRA", "Grand Est", 8.5e6, 2000.0, 0.0, {2585.8, 573.7}},
		{"FRA-BRE", "FRA", "Bretagne", 6.0e6, 1000.0, 0.0, {2476.7, 581.8}},
		{"FRA-AQU", "FRA", "Nouvelle-Aquitaine", 8.0e6, 1500.0, 0.0, {2491.9, 627.2}},
		{"FRA-ARA", "FRA", "Auvergne-Rhone-Alpes", 12.0e6, 0.0, 0.0, {2567.2, 615.0}},
		{"FRA-HDF", "FRA", "Hauts-de-France", 8.0e6, 0.0, 0.0, {2542.5, 546.8}},
		{"FRA-CVL", "FRA", "Centre-Val de Loire", 4.0e6, 0.0, 0.0, {2526.4, 585.0}},

		// Alemanha: produção relevante, mas insuficiente para a demanda interna.
		{"DEU-NRW", "DEU", "Nordrhein-Westfalen", 20.0e6, 4000.0, 0.0, {2594.0, 538.4}},
		{"DEU-BAY", "DEU", "Bayern", 15.0e6, 7000.0, 0.0, {2660.8, 581.4}},
		{"DEU-BW", "DEU", "Baden-Wurttemberg", 13.0e6, 3000.0, 0.0, {2627.5, 572.5}},
		{"DEU-NDS", "DEU", "Niedersachsen", 12.0e6, 4000.0, 0.0, {2635.1, 523.0}},
		{"DEU-SN", "DEU", "Sachsen", 10.0e6, 2000.0, 0.0, {2690.8, 540.9}},
		{"DEU-BE", "DEU", "Berlin-Brandenburg", 13.2e6, 0.0, 0.0, {2686.2, 521.0}},

		// Inglaterra: depende de importação, sobretudo no Sudeste.
		{"GBR-LDN", "GBR", "Greater London", 18.0e6, 0.0, 0.0, {2498.2, 534.0}},
		{"GBR-MID", "GBR", "Midlands", 11.0e6, 4000.0, 0.0, {2473.6, 520.5}},
		{"GBR-NW", "GBR", "North West", 13.0e6, 2000.0, 0.0, {2468.9, 507.6}},
		{"GBR-ECO", "GBR", "East of England", 6.0e6, 4000.0, 0.0, {2501.7, 525.0}},
		{"GBR-SCO", "GBR", "Scotland", 5.5e6, 2000.0, 0.0, {2441.0, 474.1}},
		{"GBR-WAL", "GBR", "Wales", 3.1e6, 0.0, 0.0, {2455.8, 534.4}},
	};
}

Dictionary EconomySim::tick() {
	++day_;
	clock_hours_ += kHoursPerDay;
	last_orders_.clear();

	// 1. Descarrega o que chegou ao estado de destino neste dia.
	for (auto shipment = in_transit_.begin(); shipment != in_transit_.end();) {
		if (shipment->arrival_hour <= clock_hours_) {
			if (State* state = find_state(shipment->to_state)) {
				state->stockpile += shipment->amount;
			}
			shipment = in_transit_.erase(shipment);
			continue;
		}
		++shipment;
	}

	// 2. Produção local e consumo regional de cada estado.
	for (State& state : states_) {
		state.stockpile += state.production;
		state.need = state.population * kNeedPerCapita;
		state.consumed = std::min(state.stockpile, state.need);
		state.stockpile -= state.consumed;
		state.deficit = state.need - state.consumed;
		state.dispatched = 0.0;
	}

	// 3. Estados com excedente vendem para estados com déficit, inclusive
	//    dentro do mesmo país. O que já está em trânsito conta como cobertura.
	std::vector<State*> sellers;
	std::vector<std::pair<State*, double>> buyers;
	for (State& state : states_) {
		if (state.stockpile > 1e-9) {
			sellers.push_back(&state);
		}
		const double pending = in_transit_total(state.id);
		const double open_deficit = state.deficit - pending;
		if (open_deficit > 1e-9) {
			buyers.emplace_back(&state, open_deficit);
		}
	}

	// Compradores mais urgentes primeiro; vendedores com mais trigo primeiro.
	std::sort(buyers.begin(), buyers.end(),
	          [](const auto& left, const auto& right) { return left.second > right.second; });
	std::sort(sellers.begin(), sellers.end(),
	          [](const State* left, const State* right) { return left->stockpile > right->stockpile; });

	// Fase 1: mercado regional (vizinhos do mesmo país).
	// Fase 2: mercado nacional (resto do país).
	// Fase 3: mercado internacional.
	// Em todas as fases a carga só chega ao comprador por transporte físico.
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

	Array states_array;
	for (const State& state : states_) {
		states_array.append(state_to_dict(state));
	}
	result["states"] = states_array;

	Array nations_array;
	for (const char* tag : {"GBR", "FRA", "DEU"}) {
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

	Array states_array;
	for (const State& state : states_) {
		states_array.append(state_to_dict(state));
	}
	result["states"] = states_array;

	Array nations_array;
	for (const char* tag : {"GBR", "FRA", "DEU"}) {
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

bool EconomySim::in_scope(const State& buyer, const State& seller, MarketScope scope) const {
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

bool EconomySim::dispatch_trade(State& buyer, double& remaining, const std::vector<State*>& sellers,
                                MarketScope scope) {
	// Dentro da camada, o frete mais barato é o do vendedor mais próximo.
	State* seller = nullptr;
	double best_distance = 0.0;
	for (State* candidate : sellers) {
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
	order.from_state = seller->id;
	order.to_state = buyer.id;
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

std::vector<EconomySim::Leg> EconomySim::build_route(const State& seller, const State& buyer,
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

std::vector<EconomySim::Leg> EconomySim::build_overseas_route(const State& seller, const State& buyer,
                                                             double amount) const {
	// Garante que a rota sempre comece no continente e termine na ilha.
	const State& origin = seller.nation == "GBR" ? buyer : seller;
	const State& destination = seller.nation == "GBR" ? seller : buyer;

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
	const double dlon = radians(to_lon(b.x) - to_lon(a.x));

	const double s1 = std::sin(dlat / 2.0);
	const double s2 = std::sin(dlon / 2.0);
	const double h = s1 * s1 + std::cos(lat1) * std::cos(lat2) * s2 * s2;
	return 2.0 * kEarthRadiusKm * std::asin(std::sqrt(h));
}

double EconomySim::in_transit_total(const std::string& state_id) const {
	double total = 0.0;
	for (const Shipment& shipment : in_transit_) {
		if (shipment.to_state == state_id) {
			total += shipment.amount;
		}
	}
	return total;
}

EconomySim::State* EconomySim::find_state(const std::string& id) {
	for (State& state : states_) {
		if (state.id == id) {
			return &state;
		}
	}
	return nullptr;
}

const EconomySim::State* EconomySim::find_state(const std::string& id) const {
	for (const State& state : states_) {
		if (state.id == id) {
			return &state;
		}
	}
	return nullptr;
}

Dictionary EconomySim::state_to_dict(const State& state) const {
	Dictionary dict;
	dict["id"] = String(state.id.c_str());
	dict["nation"] = String(state.nation.c_str());
	dict["name"] = String(state.name.c_str());
	dict["population"] = state.population;
	dict["need"] = state.need;
	dict["production"] = state.production;
	dict["consumed"] = state.consumed;
	dict["deficit"] = state.deficit;
	dict["dispatched"] = state.dispatched;
	dict["in_transit"] = in_transit_total(state.id);
	dict["stockpile"] = state.stockpile;
	return dict;
}

Dictionary EconomySim::nation_to_dict(const std::string& tag) const {
	static const std::pair<const char*, const char*> kNames[] = {
		{"GBR", "England"}, {"FRA", "France"}, {"DEU", "Germany"},
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
	int state_count = 0;

	for (const State& state : states_) {
		if (state.nation != tag) {
			continue;
		}
		++state_count;
		population += state.population;
		production += state.production;
		stockpile += state.stockpile;
		need += state.need;
		deficit += state.deficit;
		dispatched += state.dispatched;
		in_transit += in_transit_total(state.id);
	}

	dict["state_count"] = state_count;
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
	dict["from_state"] = String(order.from_state.c_str());
	dict["to_state"] = String(order.to_state.c_str());
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
