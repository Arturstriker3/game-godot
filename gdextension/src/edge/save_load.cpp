#include "save_load.h"

#include "../sim/sim_constants.h"

#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <algorithm>
#include <string>
#include <vector>

using namespace godot;

namespace {

// Constrói um String Godot a partir de UTF-8. O construtor `String(const char*)`
// desta godot-cpp interpreta os bytes como Latin-1, então `String(x.c_str())`
// corrompia os nomes não-ASCII a cada round-trip save/load (Córdoba -> CÃ³rdoba
// -> CÃÂ³rdoba...). `String::utf8` faz a leitura correta dos bytes UTF-8.
inline String sim_str(const char* utf8) {
	return String::utf8(utf8);
}

} // namespace

namespace ogs {
namespace edge {

godot::Dictionary to_dict(const WorldState& ws, const SimCore& core) {
	Dictionary data;
	data["version"] = kDemoSaveVersion;
	data["active"] = ws.active;
	data["world_mode"] = ws.world_mode;
	data["day"] = ws.day;
	data["transport_cash"] = ws.transport_cash;
	data["foreign_cash"] = ws.foreign_cash;
	data["construction_cash"] = ws.construction_cash;
	// Contadores de encerramento de construções (falidas/esgotadas) e a marca de
	// bootstrap industrial. São ESTADO VERDADEIRO (não cache): sem eles, o load
	// re-semeava a indústria (24 plantas extras) e perdia o histórico de
	// falências/jazidas esgotadas, divergindo do caminho direto já no 1º tick.
	data["bankruptcies"] = ws.bankruptcies;
	data["depleted_mines"] = ws.depleted_mines;
	data["industry_seeded"] = ws.industry_seeded;

	// Lista canônica de produtos (id na posição do índice). O contrato é por
	// `order`; os 21 primeiros são os mesmos do save v3.
	Array product_names;
	for (int index = 0; index < ws.products; ++index) {
		product_names.append(String(core.demo_product_name(index)));
	}
	data["products"] = product_names;

	Array produced;
	Array consumed;
	for (int index = 0; index < ws.products; ++index) {
		produced.append(ws.produced[index]);
		consumed.append(ws.consumed_total[index]);
	}
	data["produced_total"] = produced;
	data["consumed_total"] = consumed;

	Array cities;
	for (const DemoCity& city : ws.cities) {
		Dictionary entry;
		entry["id"] = sim_str(city.id.c_str());
		entry["name"] = sim_str(city.name.c_str());
		entry["nation"] = sim_str(city.nation.c_str());
		entry["home_nation"] = sim_str(city.home_nation.c_str());
		entry["landmass"] = sim_str(city.landmass.c_str());
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
		// TFP: 0 = derivada do índice de renda (não duplica o dado no save).
		entry["productivity_index"] = city.productivity_index;
		// Gini: 0 = não declarado (legado por escolaridade).
		entry["income_gini"] = city.income_gini;
		// F8: serviços (save v10). Mercados dos 20 setores em arrays de 12
		// campos (ordem fixa) + fluxos/caixa do dia e cobertura privada.
		entry["service_export_revenue"] = city.service_export_revenue;
		entry["service_government_revenue"] = city.service_government_revenue;
		entry["service_input_cost"] = city.service_input_cost;
		entry["service_business_profit"] = city.service_business_profit;
		entry["service_business_count"] = city.service_business_count;
		entry["private_health_cover"] = city.private_health_cover;
		entry["private_education_cover"] = city.private_education_cover;
		entry["service_tonnage_prev"] = city.service_tonnage_prev;
		Array service_markets;
		for (int sector = 0; sector < kDemoMaxServiceSectors; ++sector) {
			const DemoServiceMarket& market = city.service_markets[sector];
			Array fields;
			fields.append(market.demand);
			fields.append(market.supply);
			fields.append(market.unmet);
			fields.append(market.price);
			fields.append(market.base_price);
			fields.append(market.revenue);
			fields.append(market.input_cost);
			fields.append(market.service_cost);
			fields.append(market.jobs);
			fields.append(market.businesses);
			fields.append(market.exports);
			fields.append(market.export_revenue);
			fields.append(market.coverage);
			service_markets.append(fields);
		}
		entry["services"] = service_markets;

		Array suitability;
		Array revenue;
		Array imported;
		Array exported;
		Array import_value;
		Array export_value;
		Array external_demand;
		Array import_price;
		for (int product = 0; product < ws.products; ++product) {
			suitability.append(city.suitability[product]);
			revenue.append(city.revenue_by_product[product]);
			imported.append(city.imported[product]);
			exported.append(city.exported[product]);
			import_value.append(city.import_value[product]);
			export_value.append(city.export_value[product]);
			external_demand.append(city.external_demand[product]);
			import_price.append(city.import_price[product]);
		}
		entry["suitability"] = suitability;
		entry["revenue_by_product"] = revenue;
		entry["imported"] = imported;
		entry["exported"] = exported;
		entry["import_value"] = import_value;
		entry["export_value"] = export_value;
		entry["external_demand"] = external_demand;
		entry["import_price"] = import_price;

		// Reservas minerais (só jazidas declaradas): o saldo restante persiste.
		Dictionary deposits;
		for (int product = 0; product < ws.products; ++product) {
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
			deposits[String(core.demo_product_name(product))] = deposit_entry;
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
		for (int product = 0; product < static_cast<int>(ws.food_products.size()); ++product) {
			desired_by_category.append(city.pop.desired_by_category[product]);
			consumed_by_category.append(city.pop.consumed_by_category[product]);
		}
		pop["desired_by_category"] = desired_by_category;
		pop["consumed_by_category"] = consumed_by_category;
		// F3: estado das necessidades (níveis, duráveis e gasto próprio). Campos
		// aditivos: save v4 anterior continua carregando (default zero).
		Dictionary needs_state;
		needs_state["level"] = city.pop.needs_level;
		needs_state["satisfaction"] = city.pop.needs_satisfaction;
		needs_state["spend"] = city.pop.needs_spend;
		needs_state["durable_spend"] = city.pop.durable_spend;
		Array level_satisfaction;
		Array level_want;
		Array level_consumed;
		for (int level = 0; level < kDemoMaxNeedsLevels; ++level) {
			level_satisfaction.append(level < static_cast<int>(
				city.pop.level_satisfaction.size()) ? city.pop.level_satisfaction[level] : 0.0);
			level_want.append(level < static_cast<int>(
				city.pop.level_want.size()) ? city.pop.level_want[level] : 0.0);
			level_consumed.append(level < static_cast<int>(
				city.pop.level_consumed.size()) ? city.pop.level_consumed[level] : 0.0);
		}
		needs_state["level_satisfaction"] = level_satisfaction;
		needs_state["level_want"] = level_want;
		needs_state["level_consumed"] = level_consumed;
		Dictionary durable_stock;
		for (int product = 0; product < ws.products
				&& product < static_cast<int>(city.pop.durable_stock.size()); ++product) {
			if (city.pop.durable_stock[static_cast<std::size_t>(product)] > 0.0) {
				durable_stock[String(core.demo_product_name(product))] =
					city.pop.durable_stock[static_cast<std::size_t>(product)];
			}
		}
		needs_state["durable_stock"] = durable_stock;
		pop["needs"] = needs_state;
		// F7: mercado de trabalho e educação (save v9). Bloco aditivo: os campos
		// de compatibilidade acima continuam iguais.
		Dictionary labor;
		labor["working_age"] = city.pop.working_age;
		labor["retirees"] = city.pop.retirees;
		labor["students"] = city.pop.students;
		labor["recruit_reserve"] = city.pop.recruit_reserve;
		labor["education"] = Array{city.pop.education[0],
			city.pop.education[1], city.pop.education[2]};
		labor["participation"] = Array{city.pop.participation[0],
			city.pop.participation[1], city.pop.participation[2]};
		labor["labor_force_by_level"] = Array{
			city.pop.labor_force_by_level[0],
			city.pop.labor_force_by_level[1],
			city.pop.labor_force_by_level[2]};
		labor["employed_by_level"] = Array{city.pop.employed_by_level[0],
			city.pop.employed_by_level[1], city.pop.employed_by_level[2]};
		labor["unemployed_by_level"] = Array{
			city.pop.unemployed_by_level[0],
			city.pop.unemployed_by_level[1],
			city.pop.unemployed_by_level[2]};
		labor["vacancies_by_level"] = Array{
			city.pop.vacancies_by_level[0],
			city.pop.vacancies_by_level[1],
			city.pop.vacancies_by_level[2]};
		labor["unfilled_by_level"] = Array{
			city.pop.unfilled_by_level[0],
			city.pop.unfilled_by_level[1],
			city.pop.unfilled_by_level[2]};
		labor["wage_by_level"] = Array{city.pop.wage_by_level[0],
			city.pop.wage_by_level[1], city.pop.wage_by_level[2]};
		labor["wage_income_by_level"] = Array{
			city.pop.wage_income_by_level[0],
			city.pop.wage_income_by_level[1],
			city.pop.wage_income_by_level[2]};
		labor["studying"] = Array{city.pop.studying[0], city.pop.studying[1]};
		labor["income_level"] = city.pop.income_level;
		labor["wage_anchored"] = city.pop.wage_anchored;
		labor["income_level_share"] = Array{city.pop.income_level_share[0],
			city.pop.income_level_share[1], city.pop.income_level_share[2],
			city.pop.income_level_share[3]};
		labor["income_level_by_stratum"] = Array{
			city.pop.stratum_income_level[0],
			city.pop.stratum_income_level[1],
			city.pop.stratum_income_level[2]};
		pop["labor"] = labor;
		entry["pop"] = pop;
		entry["government_goods_spend"] = city.government_goods_spend;

		Array markets;
		for (int product = 0; product < ws.products; ++product) {
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
	for (const DemoBuilding& building : ws.buildings) {
		Dictionary entry;
		entry["id"] = sim_str(building.id.c_str());
		entry["city"] = sim_str(building.city.c_str());
		entry["kind"] = building.kind;
		entry["resource"] = sim_str(building.resource.c_str());
		entry["owner"] = sim_str(building.owner.c_str());
		entry["local_share"] = building.local_share;
		entry["partner_group"] = sim_str(building.partner_group.c_str());
		entry["input_product"] = building.input_product;
		entry["output_product"] = building.output_product;
		entry["recipe"] = building.recipe >= 0
			&& building.recipe < static_cast<int>(ws.recipes.size())
			? sim_str(ws.recipes[static_cast<std::size_t>(building.recipe)].id.c_str())
			: String("");
		entry["capacity"] = building.capacity;
		entry["input_per_output"] = building.input_per_output;
		entry["capital_per_capacity"] = building.capital_per_capacity;
		entry["jobs"] = building.jobs;
		entry["wage"] = building.wage;
		entry["opex"] = building.opex;
		// F7: mistura de escolaridade das vagas (save v9).
		entry["education_mix"] = Array{building.education_mix[0],
			building.education_mix[1], building.education_mix[2]};
		entry["output_scale"] = building.output_scale;
		entry["last_utilization"] = building.last_utilization;
		entry["strategic"] = building.strategic;
		entry["loss_days"] = building.loss_days;
		// Estado de encerramento da construção: falida por insolvência ou mina
		// com jazida esgotada. A remoção acontece no prólogo do tick seguinte;
		// sem persistir a marca, o load "ressuscita" a construção por um dia.
		entry["bankrupt"] = building.bankrupt;
		entry["depleted"] = building.depleted;
		buildings.append(entry);
	}
	data["buildings"] = buildings;

	// F8: negócios de serviço (catálogo global). O nome NÃO é gravado: é
	// derivado da semente + cultura do dono no GDScript (rótulo), então o save
	// guarda só `name_seed` e o vínculo cidade/setor.
	Array service_businesses;
	for (const DemoServiceBusiness& business : ws.service_businesses) {
		Dictionary entry;
		entry["id"] = business.id;
		entry["sector"] = business.sector;
		entry["city"] = sim_str(business.city.c_str());
		entry["owner_stratum"] = business.owner_stratum;
		entry["capacity"] = business.capacity;
		entry["jobs"] = business.jobs;
		entry["workers"] = business.workers;
		entry["capital"] = business.capital;
		entry["capital_left"] = business.capital_left;
		entry["utilization"] = business.utilization;
		entry["revenue"] = business.revenue;
		entry["wages"] = business.wages;
		entry["input_cost"] = business.input_cost;
		entry["service_cost"] = business.service_cost;
		entry["profit"] = business.profit;
		entry["loss_days"] = business.loss_days;
		entry["bankrupt"] = business.bankrupt;
		entry["opened_day"] = business.opened_day;
		entry["name_seed"] = static_cast<double>(business.name_seed % 9007199254740991ULL);
		service_businesses.append(entry);
	}
	data["service_businesses"] = service_businesses;
	data["service_bankruptcies"] = ws.service_bankruptcies;

	Array transit;
	for (const DemoShipment& shipment : ws.in_transit) {
		Dictionary entry;
		entry["to_city"] = sim_str(shipment.to_city.c_str());
		entry["product"] = shipment.product;
		entry["amount"] = shipment.amount;
		entry["arrival_day"] = shipment.arrival_day;
		// F4: reserva de frota do pedido (liberada na chegada).
		entry["fleet_nation"] = sim_str(shipment.fleet_nation.c_str());
		entry["fleet_trucks"] = shipment.fleet_trucks;
		entry["fleet_ships"] = shipment.fleet_ships;
		entry["fleet_planes"] = shipment.fleet_planes;
		entry["fuel"] = shipment.fuel;
		transit.append(entry);
	}
	data["in_transit"] = transit;

	// F4: frota nacional. Uma entrada por nação, com a saúde por modal.
	Array fleets;
	for (const std::string& nation : ws.fleet_order) {
		const auto found = ws.fleets.find(nation);
		if (found == ws.fleets.end()) {
			continue;
		}
		const DemoFleet& fleet = found->second;
		Dictionary entry;
		entry["nation"] = sim_str(nation.c_str());
		entry["trucks"] = fleet.trucks;
		entry["ships"] = fleet.ships;
		entry["planes"] = fleet.planes;
		entry["target_trucks"] = fleet.target_trucks;
		entry["target_ships"] = fleet.target_ships;
		entry["target_planes"] = fleet.target_planes;
		entry["fuel"] = fleet.fuel;
		entry["fuel_target"] = fleet.fuel_target;
		entry["spare_parts"] = fleet.spare_parts;
		entry["health"] = Array{fleet.health[0], fleet.health[1], fleet.health[2]};
		entry["fuel_used_total"] = fleet.fuel_used_total;
		entry["fuel_bought_total"] = fleet.fuel_bought_total;
		entry["parts_used_total"] = fleet.parts_used_total;
		entry["vehicles_bought_total"] = fleet.vehicles_bought_total;
		entry["derived"] = fleet.derived;
		fleets.append(entry);
	}
	data["fleets"] = fleets;

	// F5: moedas (taxa, base, regime, balança) e tarifas declaradas.
	Array currencies;
	for (const DemoCurrency& currency : ws.currencies) {
		Dictionary entry;
		entry["code"] = sim_str(currency.code.c_str());
		entry["rate_per_anchor"] = currency.rate;
		entry["base_rate"] = currency.base_rate;
		entry["floating"] = currency.floating;
		entry["fallback"] = currency.fallback;
		entry["source"] = sim_str(currency.source.c_str());
		entry["balance_ema"] = currency.balance_ema;
		entry["gross_ema"] = currency.gross_ema;
		Array nations;
		for (const std::string& nation : currency.nations) {
			nations.append(sim_str(nation.c_str()));
		}
		entry["nations"] = nations;
		currencies.append(entry);
	}
	data["currencies"] = currencies;
	Array tariffs;
	for (const DemoTariff& tariff : ws.tariffs) {
		Dictionary entry;
		entry["buyer"] = sim_str(tariff.buyer.c_str());
		entry["seller"] = sim_str(tariff.seller.c_str());
		entry["product"] = sim_str(tariff.product.c_str());
		entry["rate"] = tariff.rate;
		tariffs.append(entry);
	}
	data["tariffs"] = tariffs;
	// F9 (save v11): grupos, fluxos, política de IDE, incentivos e confiança.
	Array groups;
	for (const DemoGroup& group : ws.groups) {
		Dictionary entry;
		entry["id"] = sim_str(group.id.c_str());
		entry["name"] = sim_str(group.name.c_str());
		entry["origin_nation"] = sim_str(group.origin_nation.c_str());
		entry["sector"] = sim_str(group.sector.c_str());
		entry["cash"] = group.cash;
		entry["distributable"] = group.distributable;
		entry["revenue_total"] = group.revenue_total;
		entry["profit_total"] = group.profit_total;
		entry["dividends_total"] = group.dividends_total;
		entry["taxes_total"] = group.taxes_total;
		entry["capex_total"] = group.capex_total;
		entry["state_owned"] = group.state_owned;
		Array by_host;
		for (const auto& row : group.profit_by_host) {
			Dictionary host;
			host["nation"] = sim_str(row.first.c_str());
			host["profit"] = row.second;
			by_host.append(host);
		}
		entry["profit_by_host"] = by_host;
		groups.append(entry);
	}
	data["groups"] = groups;
	// Ordem determinística por nação (o mapa é unordered; iterar direto mudaria
	// a ordem do array a cada load e quebraria o round-trip byte a byte).
	Array group_flows;
	std::vector<std::string> group_flow_keys;
	group_flow_keys.reserve(ws.group_flows.size());
	for (const auto& row : ws.group_flows) {
		group_flow_keys.push_back(row.first);
	}
	std::sort(group_flow_keys.begin(), group_flow_keys.end());
	for (const std::string& nation : group_flow_keys) {
		const DemoGroupFlow& flow = ws.group_flows.at(nation);
		Dictionary entry;
		entry["nation"] = sim_str(nation.c_str());
		entry["repatriated_out"] = flow.repatriated_out;
		entry["repatriated_in"] = flow.repatriated_in;
		entry["dividends"] = flow.dividends;
		entry["taxes"] = flow.taxes;
		entry["subsidy"] = flow.subsidy;
		group_flows.append(entry);
	}
	data["group_flows"] = group_flows;
	Array fdi_policies;
	for (const DemoFdiPolicy& policy : ws.fdi_policies) {
		Dictionary entry;
		entry["nation"] = sim_str(policy.nation.c_str());
		entry["sector"] = sim_str(policy.sector.c_str());
		entry["mode"] = policy.mode;
		entry["min_local"] = policy.min_local;
		fdi_policies.append(entry);
	}
	data["fdi_policies"] = fdi_policies;
	Array incentives;
	for (const DemoSectorIncentive& incentive : ws.incentives) {
		Dictionary entry;
		entry["nation"] = sim_str(incentive.nation.c_str());
		entry["sector"] = sim_str(incentive.sector.c_str());
		entry["tax_holiday"] = incentive.tax_holiday;
		entry["subsidy"] = incentive.subsidy;
		incentives.append(entry);
	}
	data["sector_incentives"] = incentives;
	// Ordem determinística por nação (unordered_map -> array estável).
	Array confidence;
	std::vector<std::string> confidence_keys;
	confidence_keys.reserve(ws.confidence.size());
	for (const auto& row : ws.confidence) {
		confidence_keys.push_back(row.first);
	}
	std::sort(confidence_keys.begin(), confidence_keys.end());
	for (const std::string& nation : confidence_keys) {
		Dictionary entry;
		entry["nation"] = sim_str(nation.c_str());
		entry["value"] = ws.confidence.at(nation);
		const auto penalty = ws.confidence_penalty_day.find(nation);
		entry["penalty_day"] = penalty == ws.confidence_penalty_day.end()
			? 0 : penalty->second;
		confidence.append(entry);
	}
	data["confidence"] = confidence;
	// F7: política salarial (save v9). `minimum_wages` por nação (ausente/0 =
	// piso desligado) e a rigidez diária configurável. Ordem por nação.
	Array minimum_wages;
	std::vector<std::string> wage_keys;
	wage_keys.reserve(ws.minimum_wages.size());
	for (const auto& row : ws.minimum_wages) {
		if (row.second > 0.0) {
			wage_keys.push_back(row.first);
		}
	}
	std::sort(wage_keys.begin(), wage_keys.end());
	for (const std::string& nation_tag : wage_keys) {
		Dictionary entry;
		entry["nation"] = sim_str(nation_tag.c_str());
		entry["value"] = ws.minimum_wages.at(nation_tag);
		minimum_wages.append(entry);
	}
	data["minimum_wages"] = minimum_wages;
	data["wage_rigidity"] = ws.wage_rigidity;

	// Portos: identidade + capacidade/fila/obra. Campos aditivos (v3 continua
	// válido; save anterior não tem a chave e o loader mantém os portos do
	// mundo, recalculando a capacidade).
	Array ports;
	for (const Port& port : ws.ports) {
		Dictionary entry;
		entry["id"] = sim_str(port.id.c_str());
		entry["name"] = sim_str(port.name.c_str());
		entry["nation"] = sim_str(port.nation.c_str());
		entry["landmass"] = sim_str(port.landmass.c_str());
		entry["host_city"] = sim_str(port.host_city.c_str());
		entry["x"] = port.center.x;
		entry["y"] = port.center.y;
		entry["base_capacity_t_per_day"] = port.base_capacity_t_per_day;
		entry["capacity_t_per_day"] = port.capacity_t_per_day;
		entry["pending_capacity_t_per_day"] = port.pending_capacity_t_per_day;
		entry["pending_days"] = port.pending_days;
		ports.append(entry);
	}
	data["ports"] = ports;
	return data;
}

} // namespace edge
} // namespace ogs