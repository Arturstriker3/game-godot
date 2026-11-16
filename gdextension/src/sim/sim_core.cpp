#include "sim_core.h"

#include "profiling.h"
#include "sim_constants.h"
#include "sim_labor.h"

#include <algorithm>
#include <array>

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

void SimCore::phase_labor() {
	// Minas mais fundas custam mais para operar: opex × fator de profundidade
	// (1.0 para tudo que não é extração com jazida declarada).
	auto opex_factor = [this](const DemoCity& owner_city, const DemoBuilding& building) {
		if (building.input_product >= 0 || building.output_product < 0
				|| building.output_product >= ws_.products) {
			return 1.0;
		}
		return demo_depth_opex_factor(owner_city.deposits[building.output_product]);
	};
	for (std::size_t city_index = 0; city_index < ws_.cities.size(); ++city_index) {
		DemoCity& city = ws_.cities[city_index];
		const std::vector<int>& local_buildings =
			ws_.buildings_by_city[city_index];
		// F7: o dia do mercado de trabalho resolve qualificação, força de
		// trabalho por nível, vagas, emprego e folha. O opex continua sendo o
		// custo de operação das plantas (pago em seguida, também limitado ao
		// caixa da firma).
		double opex_total = 0.0;
		for (const int bi : local_buildings) {
			DemoBuilding& building = ws_.buildings[static_cast<std::size_t>(bi)];
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
			DemoBuilding& building = ws_.buildings[static_cast<std::size_t>(bi)];
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
}

// Eficiência da extração (0.5..1). Sem reserva declarada não há qualidade
// conhecida: eficiência 1 (comportamento legado).
double SimCore::demo_extraction_efficiency(const DemoDeposit& deposit) {
	if (!deposit.tracked) {
		return 1.0;
	}
	return kDemoQualityEfficiencyFloor
		+ (1.0 - kDemoQualityEfficiencyFloor) * std::clamp(deposit.quality, 0.0, 1.0);
}

double SimCore::demo_base_price(int product) const {
	if (product >= 0 && product < ws_.products) {
		return ws_.base_prices[product];
	}
	return 40.0;
}

int SimCore::demo_food_product(int slot) const {
	return (slot >= 0 && slot < static_cast<int>(ws_.food_products.size()))
		? ws_.food_products[static_cast<std::size_t>(slot)] : -1;
}

double SimCore::demo_food_calories(int product) const {
	for (std::size_t slot = 0; slot < ws_.food_products.size(); ++slot) {
		if (ws_.food_products[slot] == product) {
			return ws_.food_factors[slot];
		}
	}
	return 0.0;
}

int SimCore::demo_pick_recipe(const DemoCity& city, int output_product) const {
	int best = -1;
	double best_cost = 0.0;
	for (std::size_t index = 0; index < ws_.recipes.size(); ++index) {
		const DemoRecipe& recipe = ws_.recipes[index];
		if (recipe.output != output_product || recipe.input_count <= 0
				|| recipe.yield <= 0.0) {
			continue;
		}
		bool available = true;
		double cost = 0.0;
		for (int slot = 0; slot < recipe.input_count; ++slot) {
			const int input = recipe.inputs[slot];
			const double qty = recipe.input_qty[slot];
			if (input < 0 || input >= ws_.products || qty <= 0.0) {
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

void SimCore::phase_production() {
	// 4. Produção: cada construção declara insumo e produto. A produção segue
	// as vendas do dia anterior (com folga de estoque), limitada pela
	// capacidade, pela aptidão territorial e pelos insumos disponíveis.
	// Capacidade por cidade/produto, calculada uma vez por dia. Antes, cada
	// construção varria TODAS as construções do mundo para achar a capacidade
	// local do seu produto — O(construções²), inviável em escala mundial.
	std::vector<std::array<double, kDemoMaxProducts>> capacity_by_city_product(
		ws_.cities.size());
	for (auto& slots : capacity_by_city_product) {
		slots.fill(0.0);
	}
	for (const DemoBuilding& building : ws_.buildings) {
		if (building.output_product < 0 || building.output_product >= ws_.products) {
			continue;
		}
		const auto found = ws_.city_lookup.find(building.city);
		if (found == ws_.city_lookup.end()) {
			continue;
		}
		capacity_by_city_product[static_cast<std::size_t>(found->second)]
			[building.output_product] += building.capacity * building.output_scale;
	}

	for (DemoBuilding& building : ws_.buildings) {
		DemoCity* city = find_demo_city(building.city);
		if (city == nullptr || building.output_product < 0
				|| building.output_product >= ws_.products) {
			continue;
		}

		const std::size_t city_slot =
			static_cast<std::size_t>(city - ws_.cities.data());
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
			const double desired_stock = sold_prev * ws_.inventory_cover_days;
			target = sold_prev
				+ (desired_stock - stock_before) * ws_.inventory_recovery;
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
				ws_.recipes[static_cast<std::size_t>(building.recipe)];
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
				ws_.produced[product] += amount;
				ws_.produced_today[product] += amount;
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
		ws_.produced[building.output_product] += output;
		ws_.produced_today[building.output_product] += output;
	}

	OGS_SIM_PHASE_LAP(kDemoPhaseProduction);
}

void SimCore::phase_raw() {
	// 4c. Recursos brutos (minerais, soja, algodão e madeira): o resto do mundo
	// compra ao preço do mercado até um TETO de demanda externa por cidade
	// (external_demand), declarado pelo GeographyService como a participação da
	// nação no mercado mundial (reserva esgotada em ~25 anos — o mesmo horizonte
	// de max_rate_kt_day do deposits.json). Com o teto, a produção acima dele
	// vira estoque e a fase 8 derruba o preço. §5.4: só é exportado o bem com
	// `external_demand > 0` que NÃO é insumo de nenhuma construção ativa no
	// mundo (regra explícita, no lugar do antigo filtro "não é comida").
	std::vector<char> product_is_input(static_cast<std::size_t>(ws_.products), 0);
	for (const DemoBuilding& building : ws_.buildings) {
		if (building.recipe >= 0
				&& building.recipe < static_cast<int>(ws_.recipes.size())) {
			const DemoRecipe& recipe = ws_.recipes[static_cast<std::size_t>(building.recipe)];
			for (int slot = 0; slot < recipe.input_count; ++slot) {
				if (recipe.inputs[slot] >= 0 && recipe.inputs[slot] < ws_.products) {
					product_is_input[static_cast<std::size_t>(recipe.inputs[slot])] = 1;
				}
			}
		} else if (building.input_product >= 0 && building.input_product < ws_.products) {
			product_is_input[static_cast<std::size_t>(building.input_product)] = 1;
		}
	}
	for (DemoCity& city : ws_.cities) {
		for (int product = kDemoFirstRawProduct; product < ws_.products; ++product) {
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
			ws_.foreign_cash -= revenue;
			ws_.consumed_total[product] += sold;
			ws_.consumed_today[product] += sold;
		}
	}

	OGS_SIM_PHASE_LAP(kDemoPhaseRaw);
}

// ------------------------------------------------------------------
// F7/F9 — trabalho, educação, renda e emprego setorial.
// ------------------------------------------------------------------

// Multiplicador do opex pela profundidade da jazida (1.0 sem reserva).
double SimCore::demo_depth_opex_factor(const DemoDeposit& deposit) {
	if (!deposit.tracked) {
		return 1.0;
	}
	return std::min(kDemoDepthOpexMaxFactor,
		1.0 + std::max(0.0, deposit.depth_m) / kDemoDepthOpexScaleM);
}

double SimCore::demo_effective_productivity(const DemoCity& city) const {
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

void SimCore::demo_refresh_strata(DemoPop& pop) const {
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

void SimCore::demo_education_mix_for_sector(const std::string& sector,
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

void SimCore::demo_sector_anchor_values(double income, double* out) const {
	// Interpolação linear no log da renda entre as âncoras observadas (ILO).
	// Fora da faixa, vale o extremo (nação mais pobre/rica do conjunto).
	double incomes[kDemoSectorAnchorCount];
	double agri[kDemoSectorAnchorCount];
	double industry[kDemoSectorAnchorCount];
	double informal[kDemoSectorAnchorCount];
	int count = 0;
	if (!ws_.sector_anchors.empty()) {
		for (const DemoSectorAnchor& anchor : ws_.sector_anchors) {
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

void SimCore::demo_update_sector_employment(DemoCity& city) {
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
		static_cast<std::size_t>(&city - ws_.cities.data());
	if (city_index < ws_.buildings_by_city.size()) {
		for (const int bi : ws_.buildings_by_city[city_index]) {
			const DemoBuilding& building =
				ws_.buildings[static_cast<std::size_t>(bi)];
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
			for (const int food : ws_.food_products) {
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
		* ws_.agri_labor_per_ton / std::pow(tfp, ws_.agri_labor_exponent)
		/ labor_force;
	const double production_factor = anchors[0] > 1e-9
		? std::clamp(production_share / anchors[0],
			ws_.agri_production_band_min, ws_.agri_production_band_max)
		: 1.0;
	double agri = std::clamp(anchors[0] * production_factor, 0.0, 0.65)
		* employed;
	// 3. Indústria/mineração: âncora de renda (ILO) e, como piso adicional, o
	// valor adicionado simulado por trabalhador (1/TFP). No cenário atual a
	// indústria é um esqueleto (poucas plantas), então a âncora domina.
	const double va_jobs = ws_.industry_labor_per_va > 0.0
		? industry_va * ws_.industry_labor_per_va
			/ std::pow(tfp, ws_.industry_labor_exponent)
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

double SimCore::demo_labor_day(DemoCity& city) {
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
		static_cast<std::size_t>(&city - ws_.cities.data());
	if (city_index < ws_.buildings_by_city.size()) {
		for (const int bi : ws_.buildings_by_city[city_index]) {
			const DemoBuilding& building =
				ws_.buildings[static_cast<std::size_t>(bi)];
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
	if (city_index < ws_.buildings_by_city.size()) {
		for (const int bi : ws_.buildings_by_city[city_index]) {
			DemoBuilding& building = ws_.buildings[static_cast<std::size_t>(bi)];
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
	if (city_index < ws_.buildings_by_city.size()) {
		for (const int bi : ws_.buildings_by_city[city_index]) {
			DemoBuilding& building = ws_.buildings[static_cast<std::size_t>(bi)];
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
			* std::clamp(ws_.service_wage_adjust, 0.0, 1.0);
	}
	city.service_wage = std::min(
		std::max(0.0, city.service_wage), ws_.service_wage_max);
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

// ------------------------------------------------------------------
// F7/F9 — renda das famílias, impostos, transferências e consumo (cesta
// alimentar, níveis de necessidade e classes de renda).
// ------------------------------------------------------------------

void SimCore::phase_income() {
	// 5. Renda das famílias, impostos, transferências e consumo alimentar.
	for (DemoCity& city : ws_.cities) {
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
		ws_.foreign_cash -= pop.external_income;

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
		std::vector<double> avail_ge(ws_.food_products.size(), 0.0);
		for (int slot = 0; slot < static_cast<int>(ws_.food_products.size()); ++slot) {
			const int product = demo_food_product(slot);
			avail_ge[slot] = city.markets[product].stock * demo_food_calories(product);
			total_avail += avail_ge[slot];
		}
		double remaining_need = pop.need;
		for (int slot = 0; slot < static_cast<int>(ws_.food_products.size()) && remaining_need > 1e-12; ++slot) {
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
			ws_.consumed_total[product] += quantity;
			ws_.consumed_today[product] += quantity;
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
				* ws_.agri_own_consumption_share;
			in_kind = std::min(
				std::min(remaining_need, household_need), own_production);
			double left = in_kind;
			for (int slot = 0; slot < static_cast<int>(ws_.food_products.size())
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
				ws_.consumed_total[product] += tons;
				ws_.consumed_today[product] += tons;
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
		if (ws_.needs_loaded) {
			// Duráveis: descarte com perda de durable_replacement_per_year, na
			// taxa diária que perde exatamente r em 365 dias.
			for (int product = 0; product < ws_.products; ++product) {
				if (product < static_cast<int>(pop.durable_stock.size())
						&& product < static_cast<int>(ws_.product_durable.size())
						&& ws_.product_durable[static_cast<std::size_t>(product)] != 0) {
					pop.durable_stock[static_cast<std::size_t>(product)] =
						std::max(0.0,
							pop.durable_stock[static_cast<std::size_t>(product)]
							* (1.0 - ws_.durable_decay));
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
					level_index < static_cast<int>(ws_.needs_levels.size())
					&& level_index < kDemoMaxNeedsLevels; ++level_index) {
				const DemoNeedsLevel& level =
					ws_.needs_levels[static_cast<std::size_t>(level_index)];
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
						if (level_index == ws_.needs_food_level
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
					level_index < static_cast<int>(ws_.needs_levels.size())
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
						level_index < static_cast<int>(ws_.needs_levels.size())
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
							level_index < static_cast<int>(ws_.needs_levels.size())
							&& level_index < kDemoMaxNeedsLevels; ++level_index) {
						if (per_capita + 1e-12
								>= cumulative_cost[level_index]
									* ws_.income_basket_margin) {
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
}

double SimCore::demo_level_basket_cost(const DemoCity& city,
		int level_index) const {
	if (level_index < 0
			|| level_index >= static_cast<int>(ws_.needs_levels.size())) {
		return 0.0;
	}
	const DemoNeedsLevel& level =
		ws_.needs_levels[static_cast<std::size_t>(level_index)];
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
		const double replacement = std::clamp(ws_.durable_decay * 365.0, 0.0, 0.99);
		double cheapest = 0.0;  // preço por tonelada-equivalente
		bool has_durable = false;
		double cheapest_durable = 0.0;
		for (std::size_t slot = 0; slot < category.substitutes.size(); ++slot) {
			const int product = category.substitutes[slot];
			if (product < 0 || product >= ws_.products) {
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
				product < static_cast<int>(ws_.product_durable.size())
				&& ws_.product_durable[static_cast<std::size_t>(product)] != 0;
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

void SimCore::demo_food_basket_cost(const DemoCity& city, double need,
		double& local_cost, double& import_cost, double& per_ge) const {
	local_cost = 0.0;
	import_cost = 0.0;
	per_ge = 0.0;
	const int slots = std::min(
		static_cast<int>(ws_.food_products.size()), kDemoMaxFoodProducts);
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

bool SimCore::demo_category_top_market(DemoCity& city,
		const DemoNeedsCategory& category, DemoMarket*& market, double& factor,
		double& durable_scale) {
	// Substituto PREFERIDO da categoria: o primeiro com preço de mercado e
	// equivalência válidas. É nele que o desejo da categoria é registrado como
	// sinal de mercado (a cesta tenta os substitutos nesta ordem).
	for (std::size_t slot = 0; slot < category.substitutes.size(); ++slot) {
		const int product = category.substitutes[slot];
		if (product < 0 || product >= ws_.products) {
			continue;
		}
		const double slot_factor = category.equivalence[slot];
		if (slot_factor <= 0.0 || city.markets[product].price <= 0.0) {
			continue;
		}
		const bool durable =
			product < static_cast<int>(ws_.product_durable.size())
			&& ws_.product_durable[static_cast<std::size_t>(product)] != 0;
		market = &city.markets[product];
		factor = slot_factor;
		durable_scale = durable
			? std::clamp(ws_.durable_decay * 365.0 / 12.0, 0.0, 1.0) : 1.0;
		return true;
	}
	return false;
}

void SimCore::demo_register_category_demand(DemoCity& city,
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

double SimCore::demo_consume_category(DemoCity& city,
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
		if (product < 0 || product >= ws_.products) {
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
			product < static_cast<int>(ws_.product_durable.size())
			&& ws_.product_durable[static_cast<std::size_t>(product)] != 0;
		const double durable_scale = durable
			? std::clamp(ws_.durable_decay * 365.0 / 12.0, 0.0, 1.0) : 1.0;
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
		ws_.consumed_total[product] += quantity;
		ws_.consumed_today[product] += quantity;
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

void SimCore::demo_consume_government(DemoCity& city) {
	if (!ws_.needs_loaded || ws_.government_products.empty()) {
		return;
	}
	// Teto prudencial: a compra institucional não pode consumir mais que uma
	// fração do valor da cesta alimentar diária da cidade. Sem o teto, o
	// computer (preço alto) drenava o orçamento que importa comida em nações
	// sem terra (EGY 1,000 → 0,708 em 365 d).
	double cheapest_food = 0.0;
	for (int slot = 0; slot < static_cast<int>(ws_.food_products.size()); ++slot) {
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
		1, ws_.government_products.size());
	for (std::size_t index = 0; index < ws_.government_products.size(); ++index) {
		const int product = ws_.government_products[index];
		if (product < 0 || product >= ws_.products) {
			continue;
		}
		const double want = ws_.government_per_capita_day[index]
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
		ws_.consumed_total[product] += quantity;
		ws_.consumed_today[product] += quantity;
	}
}

void SimCore::demo_refresh_income_classes(DemoCity& city) const {
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

} // namespace ogs
