// Módulo de domínio: geometria e frete (S8 do docs/economy/core_refactor_plan.md).
// SimCore:: métodos movidos de sim_core.cpp — realocação byte-idêntica.
#include "sim_core.h"

#include "profiling.h"
#include "sim_constants.h"
#include "sim_labor.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace {
// Coordenadas do mapa (px -> graus) e trigonometria de `distance_km`.
double to_lon(double x) {
	return x / ogs::kMapWidth * 360.0 - 180.0;
}

double to_lat(double y) {
	return 90.0 - y / ogs::kMapHeight * 180.0;
}

double radians(double degrees) {
	return degrees * ogs::kPi / 180.0;
}
} // namespace

namespace ogs {
const ModeSpec& SimCore::spec(TransportMode mode) {
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
const char* SimCore::scope_name(MarketScope scope) {
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
Leg SimCore::make_leg(TransportMode mode, const std::string& from,
		const std::string& to, const MapPoint& from_point, const MapPoint& to_point,
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
	                     + static_cast<double>(trips) * kLoadHoursPerBatch;

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
double SimCore::distance_km(const MapPoint& a, const MapPoint& b) const {
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
double SimCore::leg_unit_freight(TransportMode mode,
		double distance_km_value) const {
	const ModeSpec& mode_spec = spec(mode);
	return distance_km_value * (mode_spec.cost_per_ton_km
		+ mode_spec.fuel_per_ton_km * fleet_fuel_price()
		+ mode_spec.fleet_cost_per_ton_km);
}
double SimCore::leg_unit_freight_food(TransportMode mode,
		double distance_km_value) const {
	// Só a tarifa: comida não paga o sobrepreço de fuel/frota. O veículo e o
	// combustível continuam exigidos no embarque (route_demand), então a
	// frota segue sendo um gargalo físico — o que muda é quem absorve o custo.
	return distance_km_value * spec(mode).cost_per_ton_km;
}
} // namespace ogs
