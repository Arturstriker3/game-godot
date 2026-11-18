// Implementação de ai_view.h (A5): o retrato do dia que a IA lê.
//
// Cada laço aqui espelha um laço de `EconomySim::fill_light_signals` ou o
// cálculo de `market_totals` em economy_sim.cpp — MESMOS valores, MESMA ordem.
// O snapshot é o único ponto do caminho da IA que toca o WorldState vivo; todos
// os helpers abaixo trabalham só nos vetores de `AiView`.

#include "ai_view.h"

#include "../sim_core.h"
#include "../sim_constants.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace ogs {
namespace {

// Faixa [begin, end) da cidade numa tabela plana com offsets cities + 1.
// Cidade fora do retrato devolve faixa vazia (ausência explícita).
void city_range(const std::vector<int>& offset, int city, int& begin, int& end) {
	begin = 0;
	end = 0;
	if (city < 0 || city + 1 >= static_cast<int>(offset.size())) {
		return;
	}
	begin = offset[static_cast<std::size_t>(city)];
	end = offset[static_cast<std::size_t>(city) + 1];
}

} // namespace

void snapshot_ai_view(const WorldState& ws, const SimCore& core, AiView& view) {
	const std::size_t cities = ws.cities.size();
	const std::size_t products =
		static_cast<std::size_t>(std::max(ws.products, 0));
	view.cities = static_cast<int>(cities);
	view.products = static_cast<int>(products);

	// 1. Cidade, mercados e POP — os 7 campos do `light` (m_*) e os 5 da POP
	// (p_*), na ordem cidade → produto.
	view.city_rows.resize(cities);
	view.markets.resize(cities * products);
	view.c_land_capacity.assign(cities * products, 0.0);
	view.pop.resize(cities);
	for (std::size_t city_index = 0; city_index < cities; ++city_index) {
		const City& city = ws.cities[city_index];
		AiCityRow& row = view.city_rows[city_index];
		row.id = city.id;
		row.nation = city.nation;
		row.landmass = city.landmass;
		row.firm_cash = city.firm_cash;
		row.government_budget = city.government_budget;
		// B5: infra da região + população do interior + déficit alimentar da
		// região + êxodo acumulado (o governo decide obra de infra com isso).
		row.infra_road = city.region.infra_road;
		row.infra_power = city.region.infra_power;
		row.infra_water = city.region.infra_water;
		row.region_pop = std::max(0.0, city.region.region_pop);
		row.region_food_deficit = std::max(0.0,
			city.region.food_need - city.region.food_consumed);
		row.migration_rural_in_total =
			std::max(0.0, city.region.migration_rural_in_total);
		row.suitability.assign(products, 0.0);
		row.external_demand.assign(products, 0.0);
		for (std::size_t product = 0; product < products; ++product) {
			row.suitability[product] = city.suitability[product];
			row.external_demand[product] = city.external_demand[product];
			const Market& market = city.markets[product];
			AiMarketRow& target =
				view.markets[city_index * products + product];
			target.price = market.price;
			target.stock = market.stock;
			target.supply = market.supply;
			target.surplus = market.surplus;
			target.sold = market.sold;
			target.shortage = market.shortage;
			target.desired = market.desired;
			view.c_land_capacity[city_index * products + product] =
				city.land_capacity[product];
		}
		AiPopRow& pop = view.pop[city_index];
		pop.need = city.pop.need;
		pop.consumed = city.pop.consumed;
		pop.satisfaction = city.pop.satisfaction;
		pop.labor_force = city.pop.labor_force;
		pop.unemployed = city.pop.unemployed;
	}

	// 2. Construções (CSR): ordem cidade → construção de `buildings_by_city`,
	// com `site_suitability` = aptidão da cidade para o produto de saída.
	view.b_offset.assign(cities + 1, 0);
	view.b_id.clear();
	view.b_owner.clear();
	view.b_output.clear();
	view.b_capacity.clear();
	view.b_utilization.clear();
	view.b_costs.clear();
	view.b_site_suitability.clear();
	view.b_stage.clear();
	view.b_stage_factor.clear();
	view.b_deepen_cost.clear();
	for (std::size_t city_index = 0; city_index < cities; ++city_index) {
		const City& bcity = ws.cities[city_index];
		if (city_index < ws.buildings_by_city.size()) {
			for (const int bi : ws.buildings_by_city[city_index]) {
				if (bi < 0 || bi >= static_cast<int>(ws.buildings.size())) {
					continue;
				}
				const Building& building =
					ws.buildings[static_cast<std::size_t>(bi)];
				view.b_id.push_back(building.id);
				view.b_output.push_back(building.output_product);
				view.b_owner.push_back(building.owner);
				view.b_capacity.push_back(building.capacity);
				view.b_utilization.push_back(building.utilization);
				view.b_costs.push_back(building.costs);
				const int output = building.output_product;
				view.b_site_suitability.push_back(
					output >= 0 && output < ws.products
						? bcity.suitability[output] : 0.0);
				// D-086: estágio da mina (0/1,0 nas construções sem estágio).
				const bool mine = output >= 0 && output < ws.products
					&& SimCore::is_mine_building(bcity, building);
				view.b_stage.push_back(mine ? building.stage : 0);
				view.b_stage_factor.push_back(mine
					? SimCore::mine_stage_factor(bcity, building) : 1.0);
				// D-086: custo do próximo aprofundamento vem do núcleo (a IA não
				// duplica a fórmula); 0.0 fora de mina.
				view.b_deepen_cost.push_back(mine
					? core.mine_deepen_cost(bcity, building) : 0.0);
			}
		}
		view.b_offset[city_index + 1] =
			static_cast<int>(view.b_output.size());
	}

	// 3. Jazidas rastreadas: reserva restante e o fator de profundidade que o
	// núcleo aplica ao opex (ordem cidade → produto, como no `light`).
	view.d_offset.assign(cities + 1, 0);
	view.d_product.clear();
	view.d_remaining_kt.clear();
	view.d_opex_factor.clear();
	view.d_quality.clear();
	for (std::size_t city_index = 0; city_index < cities; ++city_index) {
		const City& dcity = ws.cities[city_index];
		for (std::size_t product = 0; product < products; ++product) {
			const Deposit& deposit = dcity.deposits[product];
			if (!deposit.tracked) {
				continue;
			}
			view.d_product.push_back(static_cast<int>(product));
			view.d_remaining_kt.push_back(deposit.remaining_kt);
			view.d_opex_factor.push_back(SimCore::depth_opex_factor(deposit));
			view.d_quality.push_back(deposit.quality);
		}
		view.d_offset[city_index + 1] =
			static_cast<int>(view.d_product.size());
	}

	// 4. Obras em andamento por cidade: produto de saída, capacidade e se é
	// expansão de prédio existente. Ordem cidade → obra (ordem do vetor do
	// núcleo), determinística.
	view.c_offset.assign(cities + 1, 0);
	view.c_output.clear();
	view.c_capacity.clear();
	view.c_expand.clear();
	for (std::size_t city_index = 0; city_index < cities; ++city_index) {
		const std::string& city_id = ws.cities[city_index].id;
		for (const ConstructionProject& project : ws.construction_projects) {
			if (project.city != city_id) {
				continue;
			}
			view.c_output.push_back(project.output_product);
			view.c_capacity.push_back(project.capacity);
			view.c_expand.push_back(project.expand_existing ? 1 : 0);
		}
		view.c_offset[city_index + 1] =
			static_cast<int>(view.c_output.size());
	}

	// 5. Incumbentes (F9): pares (cidade, recurso) com dono-grupo — o mesmo
	// filtro do `light` (`owner_kind` com grupo e recurso não vazios) e a mesma
	// ordem (cidade → construção).
	view.i_city.clear();
	view.i_resource.clear();
	view.i_owner_group.clear();
	for (std::size_t city_index = 0; city_index < cities; ++city_index) {
		if (city_index >= ws.buildings_by_city.size()) {
			continue;
		}
		for (const int bi : ws.buildings_by_city[city_index]) {
			if (bi < 0 || bi >= static_cast<int>(ws.buildings.size())) {
				continue;
			}
			const Building& building =
				ws.buildings[static_cast<std::size_t>(bi)];
			std::string group_id;
			std::string origin_nation;
			core.owner_kind(building.owner, &group_id, &origin_nation);
			if (group_id.empty() || building.resource.empty()) {
				continue;
			}
			view.i_city.push_back(static_cast<int>(city_index));
			view.i_resource.push_back(building.resource);
			view.i_owner_group.push_back(group_id);
		}
	}

	// 6. Totais por produto: soma SÓ dos valores positivos, na ordem das
	// cidades (bit a bit igual a `market_totals` do status).
	view.tot_surplus.assign(products, 0.0);
	view.tot_sold.assign(products, 0.0);
	view.tot_desired.assign(products, 0.0);
	for (const City& city : ws.cities) {
		for (std::size_t product = 0; product < products; ++product) {
			const Market& market = city.markets[product];
			if (market.surplus > 0.0) {
				view.tot_surplus[product] += market.surplus;
			}
			if (market.sold > 0.0) {
				view.tot_sold[product] += market.sold;
			}
			if (market.desired > 0.0) {
				view.tot_desired[product] += market.desired;
			}
		}
	}

	// 7. Ranking de grupos: ordem de `EconomySim::get_group_ranking` (receita
	// total desc, caixa desc, id asc). É a ordem das vezes dos grupos.
	std::vector<int> order(ws.groups.size());
	for (std::size_t index = 0; index < order.size(); ++index) {
		order[index] = static_cast<int>(index);
	}
	std::sort(order.begin(), order.end(), [&ws](int a, int b) {
		const Group& ga = ws.groups[static_cast<std::size_t>(a)];
		const Group& gb = ws.groups[static_cast<std::size_t>(b)];
		if (ga.revenue_total != gb.revenue_total) {
			return ga.revenue_total > gb.revenue_total;
		}
		if (ga.cash != gb.cash) {
			return ga.cash > gb.cash;
		}
		return ga.id < gb.id;
	});
	view.groups.resize(order.size());
	for (std::size_t slot = 0; slot < order.size(); ++slot) {
		const Group& group =
			ws.groups[static_cast<std::size_t>(order[slot])];
		AiGroupRow& row = view.groups[slot];
		row.id = group.id;
		row.origin_nation = group.origin_nation;
		row.sector = group.sector;
		row.state_owned = group.state_owned;
		row.cash = group.cash;
		row.distributable = group.distributable;
		row.revenue_total = group.revenue_total;
	}

	// 8. Catálogo de receitas: cópia posicional (os índices de
	// `AiTemplate::recipes` valem aqui). Receitas não mudam durante o tick.
	view.recipes = ws.recipes;
}

double ai_market(const AiView& view, int city, int product, AiMarketField field) {
	if (city < 0 || city >= view.cities || product < 0
			|| product >= view.products) {
		return 0.0;
	}
	const AiMarketRow& row = view.markets[
		static_cast<std::size_t>(city) * static_cast<std::size_t>(view.products)
		+ static_cast<std::size_t>(product)];
	switch (field) {
	case kAiPrice: return row.price;
	case kAiStock: return row.stock;
	case kAiSupply: return row.supply;
	case kAiSurplus: return row.surplus;
	case kAiSold: return row.sold;
	case kAiShortage: return row.shortage;
	case kAiDesired: return row.desired;
	}
	return 0.0;
}

double ai_pop(const AiView& view, int city, AiPopField field) {
	if (city < 0 || city >= view.cities) {
		return 0.0;
	}
	const AiPopRow& row = view.pop[static_cast<std::size_t>(city)];
	switch (field) {
	case kAiNeed: return row.need;
	case kAiConsumed: return row.consumed;
	case kAiSatisfaction: return row.satisfaction;
	case kAiLaborForce: return row.labor_force;
	case kAiUnemployed: return row.unemployed;
	}
	return 0.0;
}

int ai_b_begin(const AiView& view, int city) {
	int begin = 0;
	int end = 0;
	city_range(view.b_offset, city, begin, end);
	return begin;
}

int ai_b_end(const AiView& view, int city) {
	int begin = 0;
	int end = 0;
	city_range(view.b_offset, city, begin, end);
	return end;
}

int ai_d_begin(const AiView& view, int city) {
	int begin = 0;
	int end = 0;
	city_range(view.d_offset, city, begin, end);
	return begin;
}

int ai_d_end(const AiView& view, int city) {
	int begin = 0;
	int end = 0;
	city_range(view.d_offset, city, begin, end);
	return end;
}

int ai_c_begin(const AiView& view, int city) {
	int begin = 0;
	int end = 0;
	city_range(view.c_offset, city, begin, end);
	return begin;
}

int ai_c_end(const AiView& view, int city) {
	int begin = 0;
	int end = 0;
	city_range(view.c_offset, city, begin, end);
	return end;
}

bool ai_has_deposit(const AiView& view, int city, int product) {
	const int begin = ai_d_begin(view, city);
	const int end = ai_d_end(view, city);
	for (int index = begin; index < end; ++index) {
		const std::size_t at = static_cast<std::size_t>(index);
		if (view.d_product[at] == product) {
			// D-086: basta a jazida ser RASTREADA — o recurso nunca acaba, então
			// não se exige `remaining_kt > 0`.
			return true;
		}
	}
	return false;
}

double ai_deposit_opex_factor(const AiView& view, int city, int product) {
	const int begin = ai_d_begin(view, city);
	const int end = ai_d_end(view, city);
	for (int index = begin; index < end; ++index) {
		const std::size_t at = static_cast<std::size_t>(index);
		if (view.d_product[at] == product) {
			return view.d_opex_factor[at];
		}
	}
	// Sem jazida declarada o fator é 1.0 (comportamento legado).
	return 1.0;
}

double ai_deposit_quality(const AiView& view, int city, int product) {
	const int begin = ai_d_begin(view, city);
	const int end = ai_d_end(view, city);
	for (int index = begin; index < end; ++index) {
		const std::size_t at = static_cast<std::size_t>(index);
		if (view.d_product[at] == product) {
			return view.d_quality[at];
		}
	}
	return 0.0;  // sem jazida declarada: ausência explícita
}

double ai_pending_capacity(const AiView& view, int city, int product) {
	const int begin = ai_c_begin(view, city);
	const int end = ai_c_end(view, city);
	double total = 0.0;
	for (int index = begin; index < end; ++index) {
		const std::size_t at = static_cast<std::size_t>(index);
		if (view.c_output[at] == product) {
			total += view.c_capacity[at];
		}
	}
	return total;
}

bool ai_has_pending_build(const AiView& view, int city, int product) {
	const int begin = ai_c_begin(view, city);
	const int end = ai_c_end(view, city);
	for (int index = begin; index < end; ++index) {
		const std::size_t at = static_cast<std::size_t>(index);
		if (view.c_output[at] == product && view.c_expand[at] == 0) {
			return true;
		}
	}
	return false;
}

double ai_existing_capacity(const AiView& view, int city, int product) {
	const int begin = ai_b_begin(view, city);
	const int end = ai_b_end(view, city);
	double total = 0.0;
	for (int index = begin; index < end; ++index) {
		const std::size_t at = static_cast<std::size_t>(index);
		if (view.b_output[at] == product) {
			total += view.b_capacity[at];
		}
	}
	return total;
}

std::string ai_producer_id(const AiView& view, int city, int product) {
	const int begin = ai_b_begin(view, city);
	const int end = ai_b_end(view, city);
	for (int index = begin; index < end; ++index) {
		const std::size_t at = static_cast<std::size_t>(index);
		if (view.b_output[at] == product && view.b_owner[at] != "state") {
			return view.b_id[at];
		}
	}
	return std::string();
}

bool ai_local_has(const AiView& view, int city, int product) {
	const int begin = ai_b_begin(view, city);
	const int end = ai_b_end(view, city);
	for (int index = begin; index < end; ++index) {
		if (view.b_output[static_cast<std::size_t>(index)] == product) {
			return true;
		}
	}
	return false;
}

int ai_mine_index(const AiView& view, int city, int product, bool state_only) {
	// Só faz sentido para produto com jazida declarada na cidade (senão não há
	// estágio — é extração ilimitada legada).
	if (!ai_has_deposit(view, city, product)) {
		return -1;
	}
	const int begin = ai_b_begin(view, city);
	const int end = ai_b_end(view, city);
	// D-086: entre as construções do produto com o dono pedido, devolve a de
	// MENOR custo de próximo estágio (empate = primeira, comparação estrita).
	// A ordem das construções no mundo é adversa de propósito (teste mine_stage).
	int best = -1;
	double best_cost = 0.0;
	for (int index = begin; index < end; ++index) {
		const std::size_t at = static_cast<std::size_t>(index);
		if (view.b_output[at] != product) {
			continue;
		}
		const bool is_state = view.b_owner[at] == "state";
		if (state_only != is_state) {
			continue;
		}
		const double cost = ai_mine_deepen_cost(view, city, index);
		if (best < 0 || cost < best_cost) {
			best = index;
			best_cost = cost;
		}
	}
	return best;
}

double ai_mine_deepen_capacity(const AiView& view, int index) {
	if (index < 0 || index >= static_cast<int>(view.b_capacity.size())) {
		return 0.0;
	}
	return std::max(0.0, view.b_capacity[static_cast<std::size_t>(index)])
		* (kMineDeepenCapacityFactor - 1.0);
}

double ai_mine_deepen_cost(const AiView& view, int city, int index) {
	(void)city;  // a view já é por cidade (o custo vem do retrato do dia)
	if (index < 0 || index >= static_cast<int>(view.b_deepen_cost.size())) {
		return 0.0;
	}
	return view.b_deepen_cost[static_cast<std::size_t>(index)];
}

double ai_observed_cost(const AiView& view, int city, int product) {
	const int begin = ai_b_begin(view, city);
	const int end = ai_b_end(view, city);
	double total_cost = 0.0;
	double total_output = 0.0;
	for (int index = begin; index < end; ++index) {
		const std::size_t at = static_cast<std::size_t>(index);
		if (view.b_output[at] != product) {
			continue;
		}
		const double output = view.b_capacity[at] * view.b_utilization[at];
		if (output <= 0.0) {
			continue;
		}
		total_cost += view.b_costs[at];
		total_output += output;
	}
	return total_output > 0.0 ? total_cost / total_output : -1.0;
}

double ai_land_capacity(const AiView& view, int city, int product) {
	if (city < 0 || city >= view.cities || product < 0
			|| product >= view.products) {
		return 0.0;
	}
	return view.c_land_capacity[
		static_cast<std::size_t>(city) * static_cast<std::size_t>(view.products)
		+ static_cast<std::size_t>(product)];
}

AiBuildingIndex ai_build_index(const AiView& view, int city) {
	AiBuildingIndex index;
	index.counts.assign(static_cast<std::size_t>(std::max(view.products, 0)), 0);
	index.util_sum.assign(index.counts.size(), 0.0);
	index.util_n.assign(index.counts.size(), 0);
	const int begin = ai_b_begin(view, city);
	const int end = ai_b_end(view, city);
	for (int slot = begin; slot < end; ++slot) {
		const std::size_t at = static_cast<std::size_t>(slot);
		const int product = view.b_output[at];
		// O índice é indexado por produto: bem fora do catálogo não entra
		// (o GDScript guardaria uma chave impossível no dicionário).
		if (product < 0 || product >= view.products) {
			continue;
		}
		const std::size_t key = static_cast<std::size_t>(product);
		index.counts[key] += 1;
		// Utilização efetiva: utilização ÷ aptidão do sítio (piso 0.01), 0..1.
		const double site = std::max(view.b_site_suitability[at], 0.01);
		const double effective =
			std::clamp(view.b_utilization[at] / site, 0.0, 1.0);
		index.util_sum[key] += effective;
		index.util_n[key] += 1;
	}
	return index;
}

int ai_buildings_of_type(const AiView& view, int city, int product,
		const AiBuildingIndex& index) {
	(void)view;
	(void)city;
	if (product < 0 || product >= static_cast<int>(index.counts.size())) {
		return 0;
	}
	return index.counts[static_cast<std::size_t>(product)];
}

double ai_avg_utilization(const AiView& view, int city, int product,
		const AiBuildingIndex& index) {
	(void)view;
	(void)city;
	if (product < 0 || product >= static_cast<int>(index.util_n.size())) {
		return 1.0;
	}
	const std::size_t key = static_cast<std::size_t>(product);
	const int count = index.util_n[key];
	if (count == 0) {
		return 1.0;
	}
	return index.util_sum[key] / static_cast<double>(count);
}

double ai_derived_input_demand(const AiView& view, const AiTemplates& templates,
		int city, int product) {
	if (product < 0) {
		return 0.0;
	}
	double total = 0.0;
	for (const AiTemplate& tmpl : templates.recipe_templates) {
		// Uma alternativa por saída: o insumo entra pelo consumo da receita que
		// MAIS o usa (evita contar duas rotas do mesmo bem).
		double ratio = 0.0;
		for (const int recipe_index : tmpl.recipes) {
			if (recipe_index < 0
					|| recipe_index >= static_cast<int>(view.recipes.size())) {
				continue;
			}
			const Recipe& recipe =
				view.recipes[static_cast<std::size_t>(recipe_index)];
			double used = 0.0;
			for (int slot = 0; slot < recipe.input_count; ++slot) {
				if (recipe.inputs[slot] == product) {
					used += recipe.input_qty[slot] / recipe.yield;
				}
			}
			ratio = std::max(ratio, used);
		}
		if (ratio <= 0.0) {
			continue;
		}
		total += ai_existing_capacity(view, city, tmpl.product) * ratio;
	}
	return total;
}

int ai_best_recipe(const AiView& view, const AiTemplates& templates, int city,
		const AiTemplate& tmpl) {
	(void)templates;
	int best = -1;
	double best_cost = 0.0;
	for (const int recipe_index : tmpl.recipes) {
		if (recipe_index < 0
				|| recipe_index >= static_cast<int>(view.recipes.size())) {
			continue;
		}
		const Recipe& recipe =
			view.recipes[static_cast<std::size_t>(recipe_index)];
		double cost = 0.0;
		bool available = true;
		for (int slot = 0; slot < recipe.input_count; ++slot) {
			const int input_product = recipe.inputs[slot];
			if (input_product < 0 || input_product >= view.products) {
				available = false;
				break;
			}
			double price = ai_market(view, city, input_product, kAiPrice);
			if (price <= 0.0) {
				price = 1.0;
			}
			cost += recipe.input_qty[slot] * price / recipe.yield;
		}
		// Empate fica com a PRIMEIRA receita da ordem do arquivo (cost strict).
		if (available && (best < 0 || cost < best_cost)) {
			best_cost = cost;
			best = recipe_index;
		}
	}
	return best;
}

double ai_variable_cost(const AiView& view, const AiTemplates& templates,
		int city, const AiTemplate& tmpl) {
	double unit = tmpl.jobs_per_ton * tmpl.wage + tmpl.opex_per_ton;
	if (tmpl.is_recipe) {
		const int recipe_index = ai_best_recipe(view, templates, city, tmpl);
		if (recipe_index < 0) {
			return unit;
		}
		const Recipe& recipe =
			view.recipes[static_cast<std::size_t>(recipe_index)];
		for (int slot = 0; slot < recipe.input_count; ++slot) {
			unit += recipe.input_qty[slot] / recipe.yield
				* ai_market(view, city, recipe.inputs[slot], kAiPrice);
		}
		return unit;
	}
	if (tmpl.input >= 0) {
		unit += tmpl.input_ratio
			* ai_market(view, city, tmpl.input, kAiPrice);
	}
	return unit;
}

double ai_variable_cost_mined(const AiView& view, const AiTemplates& templates,
		int city, const AiTemplate& tmpl) {
	(void)templates;
	const double opex_factor =
		ai_deposit_opex_factor(view, city, tmpl.product);
	return tmpl.jobs_per_ton * tmpl.wage + tmpl.opex_per_ton * opex_factor;
}

} // namespace ogs
