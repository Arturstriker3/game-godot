#include "sim_core.h"

#include "profiling.h"

#include <algorithm>

namespace ogs {

SimCore::SimCore(WorldState& ws) : ws_(ws) {}

// ------------------------------------------------------------------
// Helpers compartilhados.
// ------------------------------------------------------------------

DemoCity* SimCore::find_demo_city(const std::string& id) {
	const auto found = ws_.city_lookup.find(id);
	if (found == ws_.city_lookup.end()) {
		return nullptr;
	}
	return &ws_.cities[static_cast<std::size_t>(found->second)];
}

const DemoCity* SimCore::find_demo_city(const std::string& id) const {
	const auto found = ws_.city_lookup.find(id);
	if (found == ws_.city_lookup.end()) {
		return nullptr;
	}
	return &ws_.cities[static_cast<std::size_t>(found->second)];
}

void SimCore::demo_rebuild_indexes() {
	ws_.city_lookup.clear();
	ws_.city_lookup.reserve(ws_.cities.size() * 2);
	for (std::size_t index = 0; index < ws_.cities.size(); ++index) {
		ws_.city_lookup.emplace(ws_.cities[index].id, static_cast<int>(index));
	}
	// Uma lista de construções por cidade: as fases do tick e o resumo passam a
	// visitar só as construções locais, em vez de varrer o mundo inteiro.
	ws_.buildings_by_city.assign(ws_.cities.size(), {});
	for (std::size_t index = 0; index < ws_.buildings.size(); ++index) {
		const auto found = ws_.city_lookup.find(ws_.buildings[index].city);
		if (found == ws_.city_lookup.end()) {
			continue;
		}
		ws_.buildings_by_city[static_cast<std::size_t>(found->second)]
			.push_back(static_cast<int>(index));
	}
	// F4: cidades por nação, na ordem estável dos índices de cidade. A frota é
	// nacional (o governo compra combustível/peças/veículos no mercado interno),
	// então a compra visita só as cidades da nação, nunca o mundo.
	ws_.cities_by_nation.clear();
	ws_.cities_by_nation.reserve(ws_.cities.size() / 4 + 8);
	ws_.fleet_order.clear();
	ws_.fleet_order.reserve(ws_.cities.size() / 4 + 8);
	for (std::size_t index = 0; index < ws_.cities.size(); ++index) {
		const std::string& nation = ws_.cities[index].nation;
		auto found = ws_.cities_by_nation.find(nation);
		if (found == ws_.cities_by_nation.end()) {
			found = ws_.cities_by_nation.emplace(nation, std::vector<int>{}).first;
			ws_.fleet_order.push_back(nation);
		}
		found->second.push_back(static_cast<int>(index));
	}
}

double SimCore::demo_money_total() const {
	double total = ws_.transport_cash + ws_.foreign_cash + ws_.construction_cash;
	for (const DemoCity& city : ws_.cities) {
		// capital_income_next é um crédito pendente às famílias: entra no total
		// para a conservação valer em qualquer ponto do tick.
		total += city.pop.savings + city.firm_cash + city.service_cash
		       + city.government_budget + city.capital_income_next;
	}
	// F9: o caixa dos grupos é dinheiro do mundo (capital retido/dividendos a
	// pagar). Sem ele, mover lucro da firma para o grupo pareceria destruição.
	for (const DemoGroup& group : ws_.groups) {
		total += group.cash;
	}
	return total;
}

void SimCore::demo_snapshot(DemoSnapshot& out) const {
	out.cities = ws_.cities;
	out.buildings = ws_.buildings;
	out.service_businesses = ws_.service_businesses;
	out.next_service_business_id = ws_.next_service_business_id;
	out.service_bankruptcies = ws_.service_bankruptcies;
	out.transit = ws_.in_transit;
	out.fleets = ws_.fleets;
	out.groups = ws_.groups;
	out.confidence = ws_.confidence;
	out.group_flows = ws_.group_flows;
	out.currencies = ws_.currencies;
	out.transport_cash = ws_.transport_cash;
	out.foreign_cash = ws_.foreign_cash;
	out.construction_cash = ws_.construction_cash;
	for (int index = 0; index < ws_.products; ++index) {
		out.produced[index] = ws_.produced[index];
		out.consumed_total[index] = ws_.consumed_total[index];
		out.produced_today[index] = ws_.produced_today[index];
		out.consumed_today[index] = ws_.consumed_today[index];
	}
	out.day = ws_.day;
}

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

void SimCore::demo_init_pop_needs(DemoPop& pop) const {
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
	if (pop.durable_stock.size() != static_cast<std::size_t>(std::max(0, ws_.products))) {
		pop.durable_stock.resize(static_cast<std::size_t>(std::max(0, ws_.products)), 0.0);
	}
}

// ------------------------------------------------------------------
// Ordem do dia (fases).
// ------------------------------------------------------------------

void SimCore::prologue() {
	// Retrato para reverter o tick se um invariante rígido for violado.
	// Buffer PERSISTENTE: os vetores são reutilizados entre dias (com 114 bens
	// uma cidade custa ~27 KB; realocar 834 cópias por tick custava ~5 ms).
	DemoSnapshot& before = ws_.snapshot_buffer;
	demo_snapshot(before);
	money_before_ = demo_money_total();

	++ws_.day;
	ws_.last_orders.clear();
	// Obras de porto com prazo vencido entram em operação neste dia.
	demo_tick_port_expansion();

	// Encerra as construções falidas no início do tick: assim o PIB do dia já
	// não as inclui e o status fica consistente (a falência foi marcada no tick
	// anterior, no fim da fase 7).
	for (auto it = ws_.buildings.begin(); it != ws_.buildings.end();) {
		if (it->bankrupt) {
			++ws_.bankruptcies;
			it = ws_.buildings.erase(it);
			continue;
		}
		// Mina com jazida esgotada encerra a operação no dia seguinte: o fim da
		// reserva é o fim econômico planejado, não uma insolvência. Contá-la
		// como falência inflava o número (99 minas de ouro/diamante de jazidas
		// de poucos dias em 730 d) e escondia as falências de verdade (§19).
		if (it->depleted) {
			++ws_.depleted_mines;
			it = ws_.buildings.erase(it);
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
	open_prices_.assign(ws_.cities.size(), {});
	for (std::size_t city_index = 0; city_index < ws_.cities.size(); ++city_index) {
		open_prices_[city_index].resize(ws_.products);
		for (int product = 0; product < ws_.products; ++product) {
			open_prices_[city_index][product] =
				ws_.cities[city_index].markets[product].price;
		}
	}
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

void SimCore::phase_reset() {
	// 2. Zera os fluxos do dia.
	for (int index = 0; index < ws_.products; ++index) {
		ws_.produced_today[index] = 0.0;
		ws_.consumed_today[index] = 0.0;
	}
	for (DemoCity& city : ws_.cities) {
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
		for (int product = 0; product < ws_.products; ++product) {
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
		if (city.pop.desired_by_category.size() != ws_.food_products.size()) {
			city.pop.desired_by_category.assign(ws_.food_products.size(), 0.0);
			city.pop.consumed_by_category.assign(ws_.food_products.size(), 0.0);
		}
		for (int product = 0; product < static_cast<int>(ws_.food_products.size()); ++product) {
			city.pop.desired_by_category[product] = 0.0;
			city.pop.consumed_by_category[product] = 0.0;
		}
	}
	for (DemoBuilding& building : ws_.buildings) {
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
}

} // namespace ogs
