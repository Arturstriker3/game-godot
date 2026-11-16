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

} // namespace ogs
