// Módulo de domínio: logística — chegada, frota, portos e demanda de rota
// (S8 do docs/economy/core_refactor_plan.md).
// SimCore:: métodos movidos de sim_core.cpp — realocação byte-idêntica.
#include "sim_core.h"

#include "profiling.h"
#include "sim_constants.h"
#include "sim_labor.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace {
// Segurança numérica: nada de NaN/Inf no estado nem nos outputs.
inline double safe_nonneg(double v) {
	return std::isfinite(v) ? std::max(0.0, v) : 0.0;
}
} // namespace

namespace ogs {
void SimCore::demo_tick_port_expansion() {
	for (Port& port : ws_.ports) {
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
void SimCore::demo_fleet_release(const std::string& nation,
		const DemoTransportDemand& demand) {
	const auto found = ws_.fleets.find(nation);
	if (found == ws_.fleets.end()) {
		return;
	}
	DemoFleet& fleet = found->second;
	fleet.trucks_busy = std::max(0.0, fleet.trucks_busy - demand.trucks);
	fleet.ships_busy = std::max(0.0, fleet.ships_busy - demand.ships);
	fleet.planes_busy = std::max(0.0, fleet.planes_busy - demand.planes);
}
const Port* SimCore::demo_nearest_port(const std::string& nation,
		const std::string& landmass, const MapPoint& from) const {
	// Porto mais próximo da mesma nação e da mesma massa de terra. Dinâmico:
	// adicionar um porto em ports.json já o torna disponível para o roteamento.
	const Port* best = nullptr;
	double best_distance = 0.0;
	for (const Port& port : ws_.ports) {
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
Port* SimCore::find_demo_port(const std::string& id) {
	for (Port& port : ws_.ports) {
		if (port.id == id) {
			return &port;
		}
	}
	return nullptr;
}
int SimCore::demo_city_index_near(const std::string& nation,
		const std::string& landmass, const MapPoint& from) const {
	int best = -1;
	double best_distance = 0.0;
	for (std::size_t index = 0; index < ws_.cities.size(); ++index) {
		const DemoCity& city = ws_.cities[index];
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
void SimCore::demo_init_port_capacities() {
	// Capacidade de nível 1 pela população de simulação da cidade-sede. Sem
	// `port_of` válido (bench sintético), usa a cidade da nação/massa mais
	// próxima do cais; sem nenhuma, o piso de referência.
	for (Port& port : ws_.ports) {
		double population = 0.0;
		const DemoCity* host = port.host_city.empty()
			? nullptr : find_demo_city(port.host_city);
		if (host != nullptr) {
			population = host->simulation_population;
		} else {
			const int near = demo_city_index_near(port.nation, port.landmass, port.center);
			if (near >= 0) {
				port.host_city = ws_.cities[static_cast<std::size_t>(near)].id;
				population =
					ws_.cities[static_cast<std::size_t>(near)].simulation_population;
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
void SimCore::demo_reset_port_usage() {
	for (Port& port : ws_.ports) {
		port.used_today_t = 0.0;
		port.queue_t = 0.0;
		port.delay_hours = 0.0;
		port.demurrage = 0.0;
	}
}
double SimCore::demo_port_charge(Port& port, double tons, double& demurrage_out) {
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
double SimCore::demo_nation_sim_population(const std::string& nation) const {
	const auto found = ws_.cities_by_nation.find(nation);
	if (found != ws_.cities_by_nation.end()) {
		double total = 0.0;
		for (const int index : found->second) {
			total += ws_.cities[static_cast<std::size_t>(index)].simulation_population;
		}
		return total;
	}
	double total = 0.0;
	for (const DemoCity& city : ws_.cities) {
		if (city.nation == nation) {
			total += city.simulation_population;
		}
	}
	return total;
}
double SimCore::demo_nation_income_index(const std::string& nation) const {
	// PIB PPP per capita já carregado por cidade (external_income_index),
	// ponderado por população: nações ricas têm frota maior por habitante.
	double weighted = 0.0;
	double population = 0.0;
	for (const DemoCity& city : ws_.cities) {
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
bool SimCore::demo_nation_food_secure(const std::string& nation) const {
	const auto found = ws_.cities_by_nation.find(nation);
	if (found == ws_.cities_by_nation.end()) {
		return true;
	}
	double stock_ge = 0.0;
	double need = 0.0;
	for (const int index : found->second) {
		const DemoCity& city = ws_.cities[static_cast<std::size_t>(index)];
		// `pop.need` é zerado no reset do dia antes deste tick; a fórmula
		// abaixo é a mesma usada em demo_consume_population.
		need += std::max(0.0, city.simulation_population * kDemoNeedPerCapita);
		for (int slot = 0; slot < static_cast<int>(ws_.food_products.size()); ++slot) {
			const int product = demo_food_product(slot);
			const double factor = demo_food_calories(product);
			if (product >= 0 && product < ws_.products && factor > 0.0) {
				stock_ge += std::max(0.0, city.markets[product].stock) * factor;
			}
		}
	}
	if (need <= 1e-9) {
		return true;
	}
	return stock_ge >= need * kDemoFleetFoodSecureDays;
}
void SimCore::demo_init_fleets() {
	ws_.fleet_product_fuel = demo_product_index(std::string("fuel"));
	ws_.fleet_product_truck = demo_product_index(std::string("truck"));
	ws_.fleet_product_ship = demo_product_index(std::string("cargo_ship"));
	ws_.fleet_product_plane = demo_product_index(std::string("cargo_plane"));
	ws_.fleet_product_parts = demo_product_index(std::string("spare_parts"));
	ws_.fleets.clear();
	if (ws_.cities.empty()) {
		return;
	}
	// Ordem determinística por primeira aparição da nação (ordem das cidades).
	std::vector<std::string> nations;
	std::unordered_map<std::string, char> seen;
	seen.reserve(ws_.cities.size() * 2);
	for (const DemoCity& city : ws_.cities) {
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
		ws_.fleets.emplace(nation, fleet);
	}
	ws_.fleet_order = nations;
}
DemoTransportDemand SimCore::demo_route_demand(const DemoCity& seller,
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
			|| seller_port >= static_cast<int>(ws_.ports.size())
			|| buyer_port >= static_cast<int>(ws_.ports.size())) {
		return demand;
	}
	const Port& seller_city_port = ws_.ports[static_cast<std::size_t>(seller_port)];
	const Port& buyer_city_port = ws_.ports[static_cast<std::size_t>(buyer_port)];
	add_leg(TransportMode::Truck,
		distance_km(seller.center, seller_city_port.center));
	add_leg(TransportMode::Ship,
		distance_km(seller_city_port.center, buyer_city_port.center));
	add_leg(TransportMode::Truck,
		distance_km(buyer_city_port.center, buyer.center));
	return demand;
}
double SimCore::demo_fleet_capacity_scale(const std::string& nation,
		const DemoTransportDemand& demand, bool& fuel_limited) const {
	fuel_limited = false;
	if (demand.trucks <= 1e-9 && demand.ships <= 1e-9
			&& demand.planes <= 1e-9 && demand.fuel <= 1e-9) {
		return 1.0;
	}
	const auto found = ws_.fleets.find(nation);
	if (found == ws_.fleets.end()) {
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
void SimCore::demo_fleet_reserve(const std::string& nation,
		const DemoTransportDemand& demand) {
	const auto found = ws_.fleets.find(nation);
	if (found == ws_.fleets.end()) {
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
double SimCore::demo_fleet_fuel_price() const {
	if (ws_.fleet_product_fuel >= 0
			&& ws_.fleet_product_fuel < ws_.products) {
		return demo_base_price(ws_.fleet_product_fuel);
	}
	// goods.json: base_price do fuel (fallback antes de demo_set_goods).
	return 43.5;
}
double SimCore::demo_fleet_buy_core(const std::string& nation, int product_index,
		double amount, double budget_share, double& spent_out,
		double& unfilled_out) {
	spent_out = 0.0;
	unfilled_out = amount;
	const auto fleet_found = ws_.fleets.find(nation);
	if (fleet_found == ws_.fleets.end()) {
		return 0.0;
	}
	const auto cities_found = ws_.cities_by_nation.find(nation);
	if (cities_found == ws_.cities_by_nation.end() || cities_found->second.empty()) {
		return 0.0;
	}
	// Cidade-sede: a maior população de simulação paga a compra do governo.
	int hq = -1;
	double best_population = -1.0;
	for (const int index : cities_found->second) {
		const double population = ws_.cities[static_cast<std::size_t>(index)]
			.simulation_population;
		if (hq < 0 || population > best_population) {
			hq = index;
			best_population = population;
		}
	}
	DemoCity& hq_city = ws_.cities[static_cast<std::size_t>(hq)];
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
		const DemoCity& city = ws_.cities[static_cast<std::size_t>(index)];
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
		DemoCity& seller = ws_.cities[static_cast<std::size_t>(seller_entry.city)];
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
	if (product_index == ws_.fleet_product_fuel) {
		fleet.fuel += bought;
		fleet.fuel_bought_total += bought;
	} else if (product_index == ws_.fleet_product_parts) {
		fleet.spare_parts += bought;
	} else if (product_index == ws_.fleet_product_truck) {
		fleet.trucks += bought;
		fleet.vehicles_bought_total += bought;
	} else if (product_index == ws_.fleet_product_ship) {
		fleet.ships += bought;
		fleet.vehicles_bought_total += bought;
	} else if (product_index == ws_.fleet_product_plane) {
		fleet.planes += bought;
		fleet.vehicles_bought_total += bought;
	}
	spent_out = spent;
	unfilled_out = remaining;
	return bought;
}
void SimCore::phase_arrival() {
	// 1. Chegada das cargas pedidas no dia anterior.
	for (auto shipment = ws_.in_transit.begin(); shipment != ws_.in_transit.end();) {
		if (shipment->arrival_day <= ws_.day) {
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
			shipment = ws_.in_transit.erase(shipment);
			continue;
		}
		++shipment;
	}

	OGS_SIM_PHASE_LAP(kDemoPhaseArrival);
}
void SimCore::phase_fleet() {
	// 2b. F4: manutenção/compra mensal da frota e zera a fila do dia.
	if (ws_.fleets.empty() && !ws_.cities.empty()) {
		demo_init_fleets();
	}
	if (ws_.fleets.empty()) {
		return;
	}
	if (ws_.day % 30 == 0) {
		// Manutenção mensal: peças, saúde e atrito; depois a reposição do
		// governo. Usa a fila do dia anterior (antes do reset abaixo).
		for (const std::string& nation : ws_.fleet_order) {
			const auto found = ws_.fleets.find(nation);
			if (found == ws_.fleets.end()) {
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
			double spent = 0.0;
			double unfilled = 0.0;
			// Sem folga alimentar, o orçamento público fica com a comida: só
			// repõe em emergência (fuel abaixo de 20% do alvo, peças abaixo de
			// um mês). Com folga, a reposição segue o alvo normal.
			const bool fuel_emergency = fleet.fuel < fleet.fuel_target * 0.20;
			const bool parts_emergency = fleet.spare_parts < parts_need;
			if (ws_.fleet_product_fuel >= 0 && fleet.fuel < fleet.fuel_target
					&& (food_secure || fuel_emergency)) {
				demo_fleet_buy_core(nation, ws_.fleet_product_fuel,
					fleet.fuel_target - fleet.fuel, kDemoFleetBudgetShare,
					spent, unfilled);
			}
			if (ws_.fleet_product_parts >= 0 && fleet.spare_parts < parts_target
					&& (food_secure || parts_emergency)) {
				demo_fleet_buy_core(nation, ws_.fleet_product_parts,
					parts_target - fleet.spare_parts, kDemoFleetBudgetShare,
					spent, unfilled);
			}
			if (!food_secure) {
				continue;
			}
			if (ws_.fleet_product_truck >= 0) {
				const double target = std::max(fleet.target_trucks, fleet.trucks)
					* urgency + queued_trucks;
				if (fleet.trucks < target) {
					demo_fleet_buy_core(nation, ws_.fleet_product_truck,
						target - fleet.trucks, kDemoFleetBudgetShare, spent, unfilled);
				}
			}
			if (ws_.fleet_product_ship >= 0) {
				const double target = std::max(fleet.target_ships, fleet.ships)
					* urgency + queued_ships;
				if (fleet.ships < target) {
					demo_fleet_buy_core(nation, ws_.fleet_product_ship,
						target - fleet.ships, kDemoFleetBudgetShare, spent, unfilled);
				}
			}
			if (ws_.fleet_product_plane >= 0) {
				const double target = std::max(fleet.target_planes, fleet.planes) * urgency;
				if (fleet.planes < target) {
					demo_fleet_buy_core(nation, ws_.fleet_product_plane,
						target - fleet.planes, kDemoFleetBudgetShare, spent, unfilled);
				}
			}
		}
	}
	// Métricas do dia: fila e combustível consumido são diários. Os caminhões
	// de coleta/entrega do cais também são liberados aqui (ver
	// demo_trade_food): eles voltam no mesmo dia, enquanto navios e aviões
	// seguem reservados até a chegada.
	for (auto& entry : ws_.fleets) {
		entry.second.fuel_used_today = 0.0;
		entry.second.queue_today = 0.0;
		entry.second.missed_no_fleet = 0.0;
		entry.second.missed_no_fuel = 0.0;
		entry.second.trucks_busy = 0.0;
	}
}
} // namespace ogs
