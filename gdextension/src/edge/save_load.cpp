#include "save_load.h"

#include "../sim/sim_constants.h"

#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
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

inline double safe_nonneg(double v) {
	return std::isfinite(v) ? std::max(0.0, v) : 0.0;
}

// Fração 0..1 (taxas da prefeitura e condição do estoque, save v13).
inline double safe_unit(double v) {
	return std::isfinite(v) ? std::clamp(v, 0.0, 1.0) : 0.0;
}

// População de SIMULAÇÃO da cidade. Desde a calibração de 2026, `population`
// no cenário é a população real exibida (nação inteira rateada) e a aglomeração
// da fonte viaja em `sim_population`. Sem o campo (save antigo, cenário
// sintético), a população histórica É a unidade de simulação — comportamento
// anterior preservado.
inline double read_sim_population(const Dictionary& data, double population) {
	if (!data.has("sim_population")) {
		return population;
	}
	const double value = safe_nonneg(static_cast<double>(data["sim_population"]));
	return value > 0.0 ? value : population;
}

// ---------------------------------------------------------------------------
// Migração de índice de produto (save v1/v2 -> v3).
//
// Saves antigos tinham farinha=1 e pão=2 (entre trigo e ferro). Ao removê-los,
// o enum deslocou -2 a partir do ferro (iron 3->1, rice 4->2, ...). A migração
// reverte esse deslocamento: trigo (0) fica, farinha/pão (1/2) são descartados
// e os demais (>=3) deslocam -2.
// ---------------------------------------------------------------------------

// Índice antigo -> novo. Retorna -1 para farinha/pão (produto removido).
static int demo_migrate_product_index(int old_index) {
	if (old_index < 0) {
		return -1;  // sem produto/insumo
	}
	if (old_index == 0) {
		return 0;  // trigo inalterado
	}
	if (old_index == 1 || old_index == 2) {
		return -1;  // farinha/pão removidos
	}
	return old_index - 2;
}

// Remapeia um array por-produto do layout antigo (com farinha/pão) para o
// atual (products posições). new_index 0 <- old_index 0; new_index >= 1 <-
// old_index (new_index + 2). Posições sem correspondência ficam 0.0.
static Array demo_migrate_product_array(const Array& old, int products) {
	Array out;
	out.resize(products);
	for (int new_index = 0; new_index < products; ++new_index) {
		const int old_index = (new_index == 0) ? 0 : (new_index + 2);
		if (old_index >= 0 && old_index < old.size()) {
			out[new_index] = old[old_index];
		} else {
			out[new_index] = 0.0;
		}
	}
	return out;
}

// Remapeia um array por-produto usando o vetor old_index -> new_index (-1
// descarta o produto). Posições sem correspondência ficam 0.0.
static Array demo_remap_product_array(const Array& old,
		const std::vector<int>& remap, int products) {
	Array out;
	out.resize(products);
	for (int index = 0; index < products; ++index) {
		out[index] = 0.0;
	}
	const int limit = static_cast<int>(
		std::min<std::size_t>(static_cast<std::size_t>(old.size()), remap.size()));
	for (int old_index = 0; old_index < limit; ++old_index) {
		const int new_index = remap[static_cast<std::size_t>(old_index)];
		if (new_index >= 0 && new_index < products) {
			out[new_index] = old[old_index];
		}
	}
	return out;
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
		// §15 (save v12): emprego setorial (agro/indústria/serviços + informal).
		// É estado DERIVADO, mas o prólogo usa `sector_workers[1]` como base da
		// capacidade de construção do dia — sem persistir, o load zera o vetor e
		// a capacidade cai para o piso no primeiro tick (divergência de round-trip).
		entry["sector_workers"] = Array{city.sector_workers[0],
			city.sector_workers[1], city.sector_workers[2], city.sector_workers[3]};
		entry["sector_share"] = Array{city.sector_share[0],
			city.sector_share[1], city.sector_share[2], city.sector_share[3]};

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
		entry["ramp_days"] = building.ramp_days;
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

	// §15 (save v12): obras de construção em andamento. Save v11 e anteriores
	// não têm a chave: não há projeto em voo (migração trivial).
	Array construction_projects;
	for (const DemoConstructionProject& project : ws.construction_projects) {
		Dictionary entry;
		entry["id"] = sim_str(project.id.c_str());
		entry["building_id"] = sim_str(project.building_id.c_str());
		entry["city"] = sim_str(project.city.c_str());
		entry["owner"] = sim_str(project.owner.c_str());
		entry["group_id"] = sim_str(project.group_id.c_str());
		entry["resource"] = sim_str(project.resource.c_str());
		entry["output_product"] = project.output_product;
		entry["recipe"] = project.recipe >= 0
			&& project.recipe < static_cast<int>(ws.recipes.size())
			? sim_str(ws.recipes[static_cast<std::size_t>(project.recipe)].id.c_str())
			: String("");
		entry["input_product"] = project.input_product;
		entry["capacity"] = project.capacity;
		entry["jobs"] = project.jobs;
		entry["wage"] = project.wage;
		entry["opex"] = project.opex;
		entry["input_per_output"] = project.input_per_output;
		entry["education_mix"] = Array{project.education_mix[0],
			project.education_mix[1], project.education_mix[2]};
		entry["local_share"] = project.local_share;
		entry["partner_group"] = sim_str(project.partner_group.c_str());
		entry["strategic"] = project.strategic;
		entry["expand_existing"] = project.expand_existing;
		entry["total_cost"] = project.total_cost;
		entry["labor_cost_total"] = project.labor_cost_total;
		entry["paid"] = project.paid;
		entry["labor_paid"] = project.labor_paid;
		entry["reserved"] = project.reserved;
		entry["duration_days"] = project.duration_days;
		entry["days_elapsed"] = project.days_elapsed;
		entry["paused"] = project.paused;
		entry["pause_reason"] = sim_str(project.pause_reason.c_str());
		entry["created_day"] = project.created_day;
		// §19 (W1): obra de infraestrutura urbana (expansão/adensamento de
		// zona). Save anterior ao W1 não tem a chave: 0 = obra normal.
		entry["urban_action"] = project.urban_action;
		entry["urban_zone"] = project.urban_zone;
		entry["urban_km2"] = project.urban_km2;
		Array materials;
		Array material_qty;
		Array material_delivered;
		for (int m = 0; m < project.material_count; ++m) {
			materials.append(project.materials[m]);
			material_qty.append(project.material_qty[m]);
			material_delivered.append(project.material_delivered[m]);
		}
		entry["materials"] = materials;
		entry["material_qty"] = material_qty;
		entry["material_delivered"] = material_delivered;
		construction_projects.append(entry);
	}
	data["construction_projects"] = construction_projects;

	// §19 (save v13): solo urbano, moradia e prefeitura por cidade. Vetores
	// paralelos a `cities`; arrays na ordem fixa dos enums (zonas, densidades,
	// padrões, posse). Sem demo_set_urban (cenário sintético) a lista é vazia.
	Array urban;
	const std::size_t urban_count = std::min(
		{ws.land.size(), ws.housing.size(), ws.city_gov.size(),
			ws.cities.size()});
	for (std::size_t index = 0; index < urban_count; ++index) {
		const CityLand& land = ws.land[index];
		const CityHousing& house = ws.housing[index];
		const CityGov& gov = ws.city_gov[index];
		Dictionary entry;
		entry["city"] = sim_str(ws.cities[index].id.c_str());
		Array area;
		Array max_density;
		Array land_price;
		for (int zone = 0; zone < ZONE_COUNT; ++zone) {
			area.append(land.area_km2[zone]);
			max_density.append(static_cast<int>(land.max_density[zone]));
			land_price.append(land.land_price[zone]);
		}
		entry["area_km2"] = area;
		entry["max_density"] = max_density;
		entry["land_price"] = land_price;
		entry["reserve_km2"] = land.reserve_km2;
		entry["infra_capacity"] = land.infra_capacity;
		entry["informal_km2"] = land.informal_km2;
		// §19 (W1): demanda de zona barrada por falta de espaço (sinal para a
		// prefeitura). Save anterior ao W1 não tem a chave: zero.
		Array zone_demand;
		for (int zone = 0; zone < ZONE_COUNT; ++zone) {
			zone_demand.append(land.zone_demand[zone]);
		}
		entry["zone_demand"] = zone_demand;
		// §19 (integração): base do custo crescente (raio e reserva no
		// carregamento). Save anterior não tem a chave: cai no campo do loader
		// (ausência explícita, fator 1 sem inventar aumento).
		entry["initial_area_km2"] = land.initial_area_km2;
		entry["initial_reserve_km2"] = land.initial_reserve_km2;
		Array units;
		for (int density = 0; density < DENSITY_COUNT; ++density) {
			Array row;
			for (int tier = 0; tier < TIER_COUNT; ++tier) {
				row.append(house.units[density][tier]);
			}
			units.append(row);
		}
		entry["units"] = units;
		entry["condition"] = house.condition;
		Array owner_share;
		for (int owner = 0; owner < OWNER_COUNT; ++owner) {
			owner_share.append(house.owner_share[owner]);
		}
		entry["owner_share"] = owner_share;
		Array tenure;
		for (int tier = 0; tier < TIER_COUNT; ++tier) {
			Array row;
			for (int kind = 0; kind < TENURE_COUNT; ++kind) {
				row.append(house.tenure[tier][kind]);
			}
			tenure.append(row);
		}
		entry["tenure"] = tenure;
		Array rent;
		Array price;
		Array vacancy;
		for (int tier = 0; tier < TIER_COUNT; ++tier) {
			rent.append(house.rent[tier]);
			price.append(house.price[tier]);
			vacancy.append(house.vacancy[tier]);
		}
		entry["rent"] = rent;
		entry["price"] = price;
		entry["vacancy"] = vacancy;
		entry["informal_units"] = house.informal_units;
		Dictionary gov_entry;
		gov_entry["cash"] = gov.cash;
		gov_entry["iptu_rate"] = gov.iptu_rate;
		gov_entry["fee_rate"] = gov.fee_rate;
		gov_entry["transfer_share"] = gov.transfer_share;
		gov_entry["month_revenue_iptu"] = gov.month_revenue_iptu;
		gov_entry["month_revenue_fees"] = gov.month_revenue_fees;
		gov_entry["month_revenue_transfer"] = gov.month_revenue_transfer;
		gov_entry["month_expense_maintenance"] = gov.month_expense_maintenance;
		gov_entry["month_expense_infrastructure"] =
			gov.month_expense_infrastructure;
		gov_entry["month_expense_social_housing"] =
			gov.month_expense_social_housing;
		entry["gov"] = gov_entry;
		urban.append(entry);
	}
	data["urban"] = urban;

	// §19 (W2): prefeitura em operação — calibração do repasse e obra urbana
	// (decisão, orçamento reservado e prazo). Bloco PRÓPRIO para não disputar o
	// dicionário `gov` do bloco `urban` com os pacotes W1/W3.
	Array city_gov_save;
	const std::size_t city_gov_count =
		std::min(ws.city_gov.size(), ws.cities.size());
	for (std::size_t index = 0; index < city_gov_count; ++index) {
		const CityGov& gov = ws.city_gov[index];
		Dictionary entry;
		entry["city"] = sim_str(ws.cities[index].id.c_str());
		entry["revenue_pct_gdp"] = gov.revenue_pct_gdp;
		entry["target_revenue_month"] = gov.target_revenue_month;
		entry["month_transfer_paid"] = gov.month_transfer_paid;
		Array projects;
		for (int slot = 0; slot < gov.project_count; ++slot) {
			const CityGovProject& project = gov.projects[slot];
			Dictionary row;
			row["kind"] = static_cast<int>(project.kind);
			row["zone"] = project.zone;
			row["area_km2"] = project.area_km2;
			row["units"] = project.units;
			row["cost"] = project.cost;
			row["reserved"] = project.reserved;
			row["days_left"] = project.days_left;
			row["created_day"] = project.created_day;
			projects.append(row);
		}
		entry["projects"] = projects;
		city_gov_save.append(entry);
	}
	data["city_gov"] = city_gov_save;
	// §19 W3 — estado do MERCADO DE MORADIA por cidade (bloco próprio, para
	// não conflitar com o bloco `urban` do W0/W1/W2). Guarda a base de
	// domicílios, o sinal de aperto, os fluxos do mês e as obras em andamento.
	Array housing_state;
	const std::size_t housing_count = std::min(
		ws.housing.size(), ws.cities.size());
	for (std::size_t index = 0; index < housing_count; ++index) {
		const CityHousing& house = ws.housing[index];
		Dictionary entry;
		entry["city"] = sim_str(ws.cities[index].id.c_str());
		entry["households"] = house.households;
		Array households_by_tier;
		for (int tier = 0; tier < TIER_COUNT; ++tier) {
			households_by_tier.append(house.households_by_tier[tier]);
		}
		entry["households_by_tier"] = households_by_tier;
		entry["pop_anchor"] = house.pop_anchor;
		entry["rent_anchored"] = house.rent_anchored;
		Array rent_cap_ratio;
		for (int tier = 0; tier < TIER_COUNT; ++tier) {
			rent_cap_ratio.append(house.rent_cap_ratio[tier]);
		}
		entry["rent_cap_ratio"] = rent_cap_ratio;
		entry["housing_stress"] = house.housing_stress;
		entry["stress_days"] = house.stress_days;
		entry["rent_paid_month"] = house.rent_paid_month;
		// §19 (integração): o aluguel pago NO DIA também é estado (o teste de
		// round-trip do W1 compara o retrato urbano inteiro); sem a chave o
		// load zerava o dia e o save divergia.
		entry["rent_paid_today"] = house.rent_paid_today;
		entry["rent_households_month"] = house.rent_households_month;
		entry["rent_firms_month"] = house.rent_firms_month;
		entry["rent_government_month"] = house.rent_government_month;
		entry["rent_burden"] = house.rent_burden;
		entry["units_built_month"] = house.units_built_month;
		entry["month_marker"] = house.month_marker;
		Array projects;
		for (int slot = 0; slot < kDemoHousingMaxProjects; ++slot) {
			const HousingProject& project = house.projects[slot];
			if (project.state == HOUSING_PROJECT_IDLE) {
				continue;
			}
			Dictionary p;
			p["state"] = project.state;
			p["density"] = project.density;
			p["tier"] = project.tier;
			p["units"] = project.units;
			p["days_left"] = project.days_left;
			projects.append(p);
		}
		entry["projects"] = projects;
		housing_state.append(entry);
	}
	data["housing"] = housing_state;

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

// Restaura o estado do cenário a partir de to_dict() (load + migrações).
void from_dict(WorldState& ws, SimCore& core, const Dictionary& data) {
	// Recusa save de versão futura (maior que a que este binário conhece).
	const int version = int(data.get("version", 0));
	if (version > kDemoSaveVersion) {
		ws.active = false;
		return;
	}
	// Saves v1/v2 (antes da remoção de farinha/pão) trazem índices de produto
	// no layout antigo (farinha=1, pão=2, ferro=3...): remapeia para o layout
	// atual (ferro=1, arroz=2...), descartando farinha/pão.
	//
	// Saves v7 trazem a lista `products` com 114 ids (inclui `body_armor`).
	// Quando ela existe, a migração é POR ID (o nome do produto vence o
	// índice), então remoções futuras não deslocam estoques, mercados,
	// construções ou cargas em trânsito.
	const Array saved_products = data.get("products", Array());
	const bool has_saved_products = saved_products.size() > 0;
	const bool needs_legacy_migration = !has_saved_products && version < 3;
	std::vector<int> product_remap;
	if (has_saved_products) {
		product_remap.resize(
			static_cast<std::size_t>(saved_products.size()), -1);
		for (int index = 0; index < saved_products.size(); ++index) {
			product_remap[static_cast<std::size_t>(index)] =
				core.demo_product_index(std::string(String(saved_products[index]).utf8().get_data()));
		}
	}
	const auto migrate_array = [&](const Array& old) -> Array {
		if (has_saved_products) {
			return demo_remap_product_array(old, product_remap, ws.products);
		}
		if (needs_legacy_migration) {
			return demo_migrate_product_array(old, ws.products);
		}
		return old;
	};
	const auto migrate_index = [&](int old_index) -> int {
		if (has_saved_products) {
			if (old_index < 0
					|| old_index >= static_cast<int>(product_remap.size())) {
				return -1;
			}
			return product_remap[static_cast<std::size_t>(old_index)];
		}
		if (needs_legacy_migration) {
			return demo_migrate_product_index(old_index);
		}
		return old_index;
	};
	ws.active = bool(data.get("active", true));
	ws.world_mode = bool(data.get("world_mode", false));
	ws.day = int(data.get("day", 0));
	ws.transport_cash = double(data.get("transport_cash", 0.0));
	ws.foreign_cash = double(data.get("foreign_cash", 0.0));
	ws.construction_cash = double(data.get("construction_cash", 0.0));
	ws.bankruptcies = int(data.get("bankruptcies", 0));
	ws.depleted_mines = int(data.get("depleted_mines", 0));
	ws.industry_seeded = bool(data.get("industry_seeded", false));

	// F5: moedas e tarifas (save v6). Carregadas ANTES das cidades porque o
	// preço-base local (moeda da nação) depende da taxa. Save v5 sem as chaves
	// mantém o que já está carregado pelo autoload (mundo em XGS se não houver).
	if (data.has("currencies")) {
		const Array saved_currencies = data.get("currencies", Array());
		ws.currencies.clear();
		ws.currencies.reserve(static_cast<std::size_t>(saved_currencies.size()));
		for (int index = 0; index < saved_currencies.size(); ++index) {
			const Dictionary entry = saved_currencies[index];
			const String code = String(entry.get("code", ""));
			if (code.is_empty()) {
				continue;
			}
			DemoCurrency currency;
			currency.code = std::string(code.utf8().get_data());
			currency.rate = double(entry.get("rate_per_anchor", 1.0));
			if (!std::isfinite(currency.rate) || currency.rate <= 0.0) {
				currency.rate = 1.0;
			}
			currency.base_rate = double(entry.get("base_rate", currency.rate));
			if (!std::isfinite(currency.base_rate) || currency.base_rate <= 0.0) {
				currency.base_rate = currency.rate;
			}
			currency.floating = bool(entry.get("floating", true));
			currency.fallback = bool(entry.get("fallback", false));
			currency.source = std::string(
				String(entry.get("source", "default")).utf8().get_data());
			currency.balance_ema = double(entry.get("balance_ema", 0.0));
			currency.gross_ema = double(entry.get("gross_ema", 0.0));
			const Variant nations_variant = entry.get("nations", Array());
			if (nations_variant.get_type() == Variant::ARRAY) {
				const Array nations = nations_variant;
				for (int n = 0; n < nations.size(); ++n) {
					const std::string nation(
						nations[n].operator String().utf8().get_data());
					if (!nation.empty()) {
						currency.nations.push_back(nation);
					}
				}
			}
			ws.currencies.push_back(std::move(currency));
		}
		core.demo_rebuild_currency_indexes();
	}
	if (data.has("tariffs")) {
		const Array saved_tariffs = data.get("tariffs", Array());
		ws.tariffs.clear();
		ws.tariffs.reserve(static_cast<std::size_t>(saved_tariffs.size()));
		for (int index = 0; index < saved_tariffs.size(); ++index) {
			const Dictionary entry = saved_tariffs[index];
			DemoTariff tariff;
			tariff.buyer = std::string(
				String(entry.get("buyer", "")).utf8().get_data());
			if (tariff.buyer.empty()) {
				continue;
			}
			tariff.seller = std::string(
				String(entry.get("seller", "*")).utf8().get_data());
			tariff.product = std::string(
				String(entry.get("product", "*")).utf8().get_data());
			if (tariff.seller.empty()) {
				tariff.seller = "*";
			}
			if (tariff.product.empty()) {
				tariff.product = "*";
			}
			tariff.rate = std::clamp(
				double(entry.get("rate", 0.0)), 0.0, 1.0);
			ws.tariffs.push_back(std::move(tariff));
		}
		core.demo_rebuild_tariff_lookup();
	}
	// F9 (save v11): grupos e política. Save v10 e anteriores não têm as
	// chaves: os grupos são semeados a partir dos ativos privados no fim do
	// load (regra determinística de demo_seed_groups).
	ws.groups.clear();
	ws.group_lookup.clear();
	const Array saved_groups = data.get("groups", Array());
	for (int index = 0; index < saved_groups.size(); ++index) {
		const Dictionary entry = saved_groups[index];
		DemoGroup group;
		group.id = std::string(String(entry.get("id", "")).utf8().get_data());
		if (group.id.empty()) {
			continue;
		}
		group.name = std::string(String(entry.get("name", "")).utf8().get_data());
		group.origin_nation = std::string(
			String(entry.get("origin_nation", "")).utf8().get_data());
		group.sector = std::string(String(entry.get("sector", "")).utf8().get_data());
		group.cash = std::max(0.0, double(entry.get("cash", 0.0)));
		group.distributable = std::max(0.0,
			double(entry.get("distributable", 0.0)));
		group.revenue_total = std::max(0.0,
			double(entry.get("revenue_total", 0.0)));
		group.profit_total = double(entry.get("profit_total", 0.0));
		group.dividends_total = std::max(0.0,
			double(entry.get("dividends_total", 0.0)));
		group.taxes_total = std::max(0.0, double(entry.get("taxes_total", 0.0)));
		group.capex_total = std::max(0.0, double(entry.get("capex_total", 0.0)));
		group.state_owned = bool(entry.get("state_owned", false));
		const Array by_host = entry.get("profit_by_host", Array());
		for (int host_index = 0; host_index < by_host.size(); ++host_index) {
			const Dictionary host = by_host[host_index];
			const std::string nation = std::string(
				String(host.get("nation", "")).utf8().get_data());
			if (!nation.empty()) {
				group.profit_by_host[nation] = double(host.get("profit", 0.0));
			}
		}
		ws.group_lookup[group.id] = static_cast<int>(ws.groups.size());
		ws.groups.push_back(std::move(group));
	}
	ws.fdi_policies.clear();
	ws.fdi_lookup.clear();
	const Array saved_policies = data.get("fdi_policies", Array());
	for (int index = 0; index < saved_policies.size(); ++index) {
		const Dictionary entry = saved_policies[index];
		DemoFdiPolicy policy;
		policy.nation = std::string(
			String(entry.get("nation", "")).utf8().get_data());
		policy.sector = std::string(
			String(entry.get("sector", "")).utf8().get_data());
		policy.mode = std::clamp(int(entry.get("mode", 0)), 0, 3);
		policy.min_local = std::clamp(double(entry.get("min_local", 0.0)), 0.0, 1.0);
		if (policy.nation.empty()) {
			continue;
		}
		ws.fdi_lookup[policy.nation + "|" + policy.sector] =
			static_cast<int>(ws.fdi_policies.size());
		ws.fdi_policies.push_back(std::move(policy));
	}
	ws.incentives.clear();
	ws.incentive_lookup.clear();
	const Array saved_incentives = data.get("sector_incentives", Array());
	for (int index = 0; index < saved_incentives.size(); ++index) {
		const Dictionary entry = saved_incentives[index];
		DemoSectorIncentive incentive;
		incentive.nation = std::string(
			String(entry.get("nation", "")).utf8().get_data());
		incentive.sector = std::string(
			String(entry.get("sector", "")).utf8().get_data());
		incentive.tax_holiday = std::clamp(
			double(entry.get("tax_holiday", 0.0)), 0.0, 1.0);
		incentive.subsidy = std::max(0.0, double(entry.get("subsidy", 0.0)));
		if (incentive.nation.empty()) {
			continue;
		}
		ws.incentive_lookup[incentive.nation + "|" + incentive.sector] =
			static_cast<int>(ws.incentives.size());
		ws.incentives.push_back(std::move(incentive));
	}
	ws.confidence.clear();
	ws.confidence_penalty_day.clear();
	const Array saved_confidence = data.get("confidence", Array());
	for (int index = 0; index < saved_confidence.size(); ++index) {
		const Dictionary entry = saved_confidence[index];
		const std::string nation = std::string(
			String(entry.get("nation", "")).utf8().get_data());
		if (nation.empty()) {
			continue;
		}
		ws.confidence[nation] = std::clamp(
			double(entry.get("value", 1.0)), 0.0, 1.0);
		const int penalty = int(entry.get("penalty_day", 0));
		if (penalty > 0) {
			ws.confidence_penalty_day[nation] = penalty;
		}
	}
	ws.group_flows.clear();
	const Array saved_flows = data.get("group_flows", Array());
	for (int index = 0; index < saved_flows.size(); ++index) {
		const Dictionary entry = saved_flows[index];
		const std::string nation = std::string(
			String(entry.get("nation", "")).utf8().get_data());
		if (nation.empty()) {
			continue;
		}
		DemoGroupFlow flow;
		flow.repatriated_out = std::max(0.0,
			double(entry.get("repatriated_out", 0.0)));
		flow.repatriated_in = std::max(0.0,
			double(entry.get("repatriated_in", 0.0)));
		flow.dividends = std::max(0.0, double(entry.get("dividends", 0.0)));
		flow.taxes = std::max(0.0, double(entry.get("taxes", 0.0)));
		flow.subsidy = std::max(0.0, double(entry.get("subsidy", 0.0)));
		ws.group_flows[nation] = flow;
	}
	// F7: política salarial. Save v8 e anteriores não têm as chaves: piso
	// desligado e rigidez padrão (1%/dia).
	ws.minimum_wages.clear();
	Array saved_minimum_wages = data.get("minimum_wages", Array());
	for (int index = 0; index < saved_minimum_wages.size(); ++index) {
		const Dictionary entry = saved_minimum_wages[index];
		const String tag = String(entry.get("nation", ""));
		const double value = std::clamp(double(entry.get("value", 0.0)),
			0.0, 100.0);
		if (!tag.is_empty() && value > 0.0) {
			ws.minimum_wages[std::string(tag.utf8().get_data())] = value;
		}
	}
	ws.wage_rigidity = std::clamp(
		double(data.get("wage_rigidity", ws.wage_rigidity)), 0.0, 0.25);

	Array produced = migrate_array(data.get("produced_total", Array()));
	Array consumed = migrate_array(data.get("consumed_total", Array()));
	for (int index = 0; index < ws.products; ++index) {
		ws.produced[index] = index < produced.size() ? double(produced[index]) : 0.0;
		ws.consumed_total[index] =
			index < consumed.size() ? double(consumed[index]) : 0.0;
		ws.produced_today[index] = 0.0;
		ws.consumed_today[index] = 0.0;
	}

	ws.cities.clear();
	Array cities = data.get("cities", Array());
	for (int index = 0; index < cities.size(); ++index) {
		Dictionary entry = cities[index];
		DemoCity city;
		city.id = std::string(String(entry.get("id", "")).utf8().get_data());
		city.name = std::string(String(entry.get("name", "")).utf8().get_data());
		city.nation = std::string(String(entry.get("nation", "")).utf8().get_data());
		// F5 v7: preço de mercado passou a ficar na ÂNCORA. Saves v6 e
		// anteriores guardavam preço local; converte uma vez pela taxa da nação
		// (as moedas foram carregadas antes das cidades).
		const double save_fx = version >= 7
			? 1.0 : std::max(1e-9, core.demo_fx_rate(city.nation));
		city.home_nation = std::string(
			String(entry.get("home_nation", String(city.nation.c_str()))).utf8().get_data());
		city.landmass =
			std::string(String(entry.get("landmass", "")).utf8().get_data());
		Array center = entry.get("center", Array());
		if (center.size() >= 2) {
			city.center = {double(center[0]), double(center[1])};
		}
		city.population = double(entry.get("population", 0.0));
		// Save antigo (ou cenário sintético) sem o campo: a população real
		// exibida é a própria unidade de simulação (comportamento anterior).
		city.simulation_population = read_sim_population(entry, city.population);
		city.firm_cash = double(entry.get("firm_cash", 0.0));
		city.service_cash = double(entry.get("service_cash", 0.0));
		city.government_budget = double(entry.get("government_budget", 0.0));
		// Save antigo não tem o índice: mantém a média mundial (1,0).
		city.external_income_index = std::clamp(
			double(entry.get("external_income_index", 1.0)), 0.0, 8.0);
		// F7 calibração: TFP derivada do PIB PPP per capita. Save v9 e
		// anteriores não têm o campo: 0 = DERIVAR do índice de renda já gravado
		// (o mesmo `derived:gdp_ppp`), sem inventar produtividade.
		city.productivity_index = std::clamp(
			double(entry.get("productivity_index", 0.0)), 0.0, 8.0);
		// F7 calibração: save anterior à distribuição por Gini não tem o campo;
		// 0 mantém o gate legado por estrato até o GeographyService declarar.
		city.income_gini = std::clamp(
			double(entry.get("income_gini", 0.0)), 0.0, 1.0);
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
		// F8: serviços (save v10). Sem o bloco (v9 e anteriores), os mercados
		// ficam no preço-âncora do catálogo e os negócios são semeados no fim
		// do load (`demo_seed_services`).
		city.service_export_revenue = double(entry.get("service_export_revenue", 0.0));
		city.service_government_revenue =
			double(entry.get("service_government_revenue", 0.0));
		city.service_input_cost = double(entry.get("service_input_cost", 0.0));
		city.service_business_profit =
			double(entry.get("service_business_profit", 0.0));
		city.service_business_count =
			int(entry.get("service_business_count", 0));
		city.private_health_cover = std::clamp(
			double(entry.get("private_health_cover", 0.0)), 0.0, 1.0);
		city.private_education_cover = std::clamp(
			double(entry.get("private_education_cover", 0.0)), 0.0, 1.0);
		city.service_tonnage_prev = double(entry.get("service_tonnage_prev", 0.0));
		const Array saved_services = entry.get("services", Array());
		for (int sector = 0; sector < kDemoMaxServiceSectors; ++sector) {
			DemoServiceMarket& market = city.service_markets[sector];
			if (sector < static_cast<int>(ws.service_sectors.size())) {
				market.base_price =
					ws.service_sectors[static_cast<std::size_t>(sector)].price;
			}
			market.price = market.base_price;
			if (sector >= saved_services.size()
					|| saved_services[sector].get_type() != Variant::ARRAY) {
				continue;
			}
			const Array fields = saved_services[sector];
			if (fields.size() < 13) {
				continue;
			}
			market.demand = double(fields[0]);
			market.supply = double(fields[1]);
			market.unmet = double(fields[2]);
			market.price = double(fields[3]) > 0.0
				? double(fields[3]) : market.base_price;
			market.base_price = double(fields[4]) > 0.0
				? double(fields[4]) : market.base_price;
			market.revenue = double(fields[5]);
			market.input_cost = double(fields[6]);
			market.service_cost = double(fields[7]);
			market.jobs = double(fields[8]);
			market.businesses = double(fields[9]);
			market.exports = double(fields[10]);
			market.export_revenue = double(fields[11]);
			market.coverage = std::clamp(double(fields[12]), 0.0, 1.0);
		}

		Array suitability = entry.get("suitability", Array());
		Array revenue = entry.get("revenue_by_product", Array());
		Array imported = entry.get("imported", Array());
		Array exported = entry.get("exported", Array());
		Array import_value = entry.get("import_value", Array());
		Array export_value = entry.get("export_value", Array());
		Array external_demand = entry.get("external_demand", Array());
		Array import_price = entry.get("import_price", Array());
		suitability = migrate_array(suitability);
		revenue = migrate_array(revenue);
		imported = migrate_array(imported);
		exported = migrate_array(exported);
		import_value = migrate_array(import_value);
		export_value = migrate_array(export_value);
		external_demand = migrate_array(external_demand);
		import_price = migrate_array(import_price);
		for (int product = 0; product < ws.products; ++product) {
			city.suitability[product] = product < suitability.size()
				? double(suitability[product]) : (product >= 1 ? 0.0 : 1.0);
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
			city.external_demand[product] = product < external_demand.size()
				? double(external_demand[product]) : 0.0;
			// F5: cotação de importação (na âncora desde o v7). Save anterior à
			// F5 não tem o array: sem cotação até a primeira compra externa.
			city.import_price[product] = product < import_price.size()
				? safe_nonneg(double(import_price[product])) * save_fx : 0.0;
		}

		Dictionary saved_deposits = entry.get("deposits", Dictionary());
		for (int product = 0; product < ws.products; ++product) {
			const String key = String(core.demo_product_name(product));
			if (!saved_deposits.has(key)) {
				continue;
			}
			Dictionary deposit_entry = saved_deposits[key];
			DemoDeposit& deposit = city.deposits[product];
			deposit.tracked = true;
			deposit.initial_kt = double(deposit_entry.get("initial_kt", 0.0));
			deposit.remaining_kt = double(deposit_entry.get("remaining_kt", 0.0));
			deposit.extracted_kt = double(deposit_entry.get("extracted_kt", 0.0));
			deposit.quality = double(deposit_entry.get("quality", 1.0));
			deposit.depth_m = double(deposit_entry.get("depth_m", 0.0));
		}

		Dictionary pop = entry.get("pop", Dictionary());
		city.pop.population = double(pop.get("population", city.simulation_population));
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
		city.pop.external_income = double(pop.get("external_income", 0.0));
		city.pop.food_spend = double(pop.get("food_spend", 0.0));
		city.pop.consumed = double(pop.get("consumed", 0.0));
		city.pop.need = double(pop.get("need", 0.0));
		city.pop.satisfaction = double(pop.get("satisfaction", 0.0));
		city.pop.savings = double(pop.get("savings", 0.0));
		// F3: estado das necessidades (aditivo; save anterior = zero).
		core.demo_init_pop_needs(city.pop);
		Dictionary needs_state = pop.get("needs", Dictionary());
		city.pop.needs_level = int(needs_state.get("level", 0));
		city.pop.needs_satisfaction = double(needs_state.get("satisfaction",
			city.pop.satisfaction));
		city.pop.needs_spend = double(needs_state.get("spend", 0.0));
		city.pop.durable_spend = double(needs_state.get("durable_spend", 0.0));
		Array saved_level_sat = needs_state.get("level_satisfaction", Array());
		Array saved_level_want = needs_state.get("level_want", Array());
		Array saved_level_cons = needs_state.get("level_consumed", Array());
		for (int level = 0; level < kDemoMaxNeedsLevels; ++level) {
			city.pop.level_satisfaction[level] = level < saved_level_sat.size()
				? double(saved_level_sat[level]) : 0.0;
			city.pop.level_want[level] = level < saved_level_want.size()
				? double(saved_level_want[level]) : 0.0;
			city.pop.level_consumed[level] = level < saved_level_cons.size()
				? double(saved_level_cons[level]) : 0.0;
		}
		Dictionary saved_durable = needs_state.get("durable_stock", Dictionary());
		for (int product = 0; product < ws.products
				&& product < static_cast<int>(city.pop.durable_stock.size()); ++product) {
			const String key = String(core.demo_product_name(product));
			if (saved_durable.has(key)) {
				city.pop.durable_stock[static_cast<std::size_t>(product)] =
					std::max(0.0, double(saved_durable[key]));
			}
		}
		// F7: mercado de trabalho e educação (save v9). Save v8 e anteriores não
		// têm o bloco: o perfil é derivado do índice de renda já gravado. A
		// força de trabalho, o emprego e os salários são recalculados pelo
		// mesmo modelo do primeiro dia (migração documentada).
		Dictionary saved_labor = pop.get("labor", Dictionary());
		const bool has_labor = version >= 9 && !saved_labor.is_empty();
		if (!has_labor) {
			core.demo_apply_labor_profile(city);
		} else {
			DemoPop& labor_pop = city.pop;
			labor_pop.working_age = std::max(0.0,
				double(saved_labor.get("working_age", labor_pop.working_age)));
			labor_pop.retirees = std::max(0.0,
				double(saved_labor.get("retirees", 0.0)));
			labor_pop.students = std::max(0.0,
				double(saved_labor.get("students", 0.0)));
			labor_pop.recruit_reserve = std::max(0.0,
				double(saved_labor.get("recruit_reserve", 0.0)));
			const auto read_levels = [&saved_labor](const char* key,
					double* out, int count) {
				const Array values = saved_labor.get(key, Array());
				for (int level = 0; level < count; ++level) {
					out[level] = level < values.size()
						? std::max(0.0, double(values[level])) : out[level];
				}
			};
			read_levels("education", labor_pop.education,
				kDemoEducationLevels);
			read_levels("participation", labor_pop.participation,
				kDemoEducationLevels);
			read_levels("labor_force_by_level", labor_pop.labor_force_by_level,
				kDemoEducationLevels);
			read_levels("employed_by_level", labor_pop.employed_by_level,
				kDemoEducationLevels);
			read_levels("unemployed_by_level", labor_pop.unemployed_by_level,
				kDemoEducationLevels);
			read_levels("vacancies_by_level", labor_pop.vacancies_by_level,
				kDemoEducationLevels);
			read_levels("wage_by_level", labor_pop.wage_by_level,
				kDemoEducationLevels);
			read_levels("wage_income_by_level",
				labor_pop.wage_income_by_level, kDemoEducationLevels);
			read_levels("studying", labor_pop.studying,
				kDemoEducationLevels - 1);
			labor_pop.students = labor_pop.studying[0] + labor_pop.studying[1];
			labor_pop.income_level = std::clamp(
				int(saved_labor.get("income_level", 1)), 0, 3);
			// F7 calibração: blocos aditivos. Save anterior ao calibração (v9
			// sem eles) cai no nível único e mantém o salário carregado como
			// histórico (wage_anchored = true, com inércia).
			const bool has_strata = saved_labor.has("income_level_by_stratum");
			if (has_strata) {
				const Array strata = saved_labor.get("income_level_by_stratum", Array());
				for (int stratum = 0; stratum < kDemoEducationLevels; ++stratum) {
					labor_pop.stratum_income_level[stratum] = stratum < strata.size()
						? std::clamp(int(strata[stratum]), 0, 3)
						: labor_pop.income_level;
				}
			} else {
				for (int stratum = 0; stratum < kDemoEducationLevels; ++stratum) {
					labor_pop.stratum_income_level[stratum] = labor_pop.income_level;
				}
			}
			const Array share = saved_labor.get("income_level_share", Array());
			for (int level_index = 0; level_index < kDemoIncomeLevels; ++level_index) {
				labor_pop.income_level_share[level_index] = level_index < share.size()
					? std::clamp(double(share[level_index]), 0.0, 1.0) : 0.0;
			}
			if (share.is_empty()) {
				labor_pop.income_level_share[labor_pop.income_level] = 1.0;
			}
			labor_pop.wage_anchored = bool(saved_labor.get("wage_anchored", true));
			labor_pop.labor_force = 0.0;
			for (int level = 0; level < kDemoEducationLevels; ++level) {
				labor_pop.labor_force +=
					labor_pop.labor_force_by_level[level];
			}
			labor_pop.employed = 0.0;
			labor_pop.unemployed = 0.0;
			for (int level = 0; level < kDemoEducationLevels; ++level) {
				labor_pop.employed += labor_pop.employed_by_level[level];
				labor_pop.unemployed += labor_pop.unemployed_by_level[level];
			}
			labor_pop.wage_income = 0.0;
			for (int level = 0; level < kDemoEducationLevels; ++level) {
				labor_pop.wage_income += labor_pop.wage_income_by_level[level];
			}
			labor_pop.dependents = std::max(0.0,
				city.pop.population - labor_pop.labor_force);
		}
		city.government_goods_spend = double(entry.get("government_goods_spend", 0.0));
		// §15 (save v12): emprego setorial — base da capacidade de construção do
		// prólogo (ver to_dict). Save v11 não tem a chave: zeros, recalculado no
		// primeiro tick (comportamento anterior, só que agora o piso da obra cobre).
		const Array saved_sector_workers = entry.get("sector_workers", Array());
		const Array saved_sector_share = entry.get("sector_share", Array());
		for (int slot = 0; slot < 4; ++slot) {
			city.sector_workers[slot] = slot < saved_sector_workers.size()
				? std::max(0.0, double(saved_sector_workers[slot])) : 0.0;
			city.sector_share[slot] = slot < saved_sector_share.size()
				? std::clamp(double(saved_sector_share[slot]), 0.0, 1.0) : 0.0;
		}
		Array desired_by_category = pop.get("desired_by_category", Array());
		Array consumed_by_category = pop.get("consumed_by_category", Array());
		city.pop.desired_by_category.assign(ws.food_products.size(), 0.0);
		city.pop.consumed_by_category.assign(ws.food_products.size(), 0.0);
		for (int product = 0; product < static_cast<int>(ws.food_products.size()); ++product) {
			city.pop.desired_by_category[product] =
				product < desired_by_category.size()
					? double(desired_by_category[product]) : 0.0;
			city.pop.consumed_by_category[product] =
				product < consumed_by_category.size()
					? double(consumed_by_category[product]) : 0.0;
		}

		Array markets = entry.get("markets", Array());
		if (has_saved_products || needs_legacy_migration) {
			// Remapeia os mercados antigos por id (save v7 com 114 produtos,
			// descartando `body_armor`) ou pelo deslocamento -2 (save v1/v2 com
			// farinha/pão). Mercado sem correspondência recebe o preço-base.
			Array remapped;
			remapped.resize(ws.products);
			for (int new_index = 0; new_index < ws.products; ++new_index) {
				Dictionary m;
				m["stock"] = 0.0;
				m["price"] = core.demo_base_price(new_index);
				m["demand"] = 0.0;
				m["supply"] = 0.0;
				m["desired"] = 0.0;
				m["sold"] = 0.0;
				m["sold_prev"] = 0.0;
				m["shortage"] = 0.0;
				m["surplus"] = 0.0;
				remapped[new_index] = m;
			}
			for (int old_index = 0; old_index < markets.size(); ++old_index) {
				const int new_index = migrate_index(old_index);
				if (new_index >= 0 && new_index < ws.products) {
					remapped[new_index] = markets[old_index];
				}
			}
			markets = remapped;
		}
		// Saves antigos (4 produtos) não trazem mercados dos novos bens: preço-base.
		for (int product = markets.size(); product < ws.products; ++product) {
			if (product >= 0) {
				city.markets[product].price = core.demo_base_price(product);
			}
		}
		for (int product = 0; product < ws.products && product < markets.size(); ++product) {
			Dictionary market = markets[product];
			city.markets[product].stock = double(market.get("stock", 0.0));
			city.markets[product].price = double(market.get("price", 1.0)) * save_fx;
			city.markets[product].demand = double(market.get("demand", 0.0));
			city.markets[product].supply = double(market.get("supply", 0.0));
			city.markets[product].desired = double(market.get("desired", 0.0));
			city.markets[product].sold = double(market.get("sold", 0.0));
			city.markets[product].sold_prev = double(market.get("sold_prev", 0.0));
			city.markets[product].shortage = double(market.get("shortage", 0.0));
			city.markets[product].surplus = double(market.get("surplus", 0.0));
		}
		ws.cities.push_back(city);
	}

	ws.buildings.clear();
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
		building.local_share = std::clamp(
			double(entry.get("local_share", 0.0)), 0.0, 1.0);
		building.partner_group = std::string(
			String(entry.get("partner_group", "")).utf8().get_data());
		building.input_product = int(entry.get("input_product", -1));
		building.output_product = int(entry.get("output_product", 0));
		if (has_saved_products || needs_legacy_migration) {
			building.input_product = migrate_index(building.input_product);
			building.output_product = migrate_index(building.output_product);
			if (building.output_product < 0) {
				// Produto removido da cadeia (farinha/pão ou body_armor):
				// construção descartada.
				continue;
			}
		}
		building.capacity = double(entry.get("capacity", 0.0));
		building.input_per_output = double(entry.get("input_per_output", 0.0));
		// Receita industrial: o id é resolvido na tabela carregada por
		// demo_set_recipes (v4). Save v3 não tem a chave -> produtor legado.
		const String recipe_id = String(entry.get("recipe", ""));
		building.recipe = recipe_id.is_empty() ? -1 : core.demo_recipe_index(std::string(recipe_id.utf8().get_data()));
		building.capital_per_capacity = std::max(1.0, safe_nonneg(
			double(entry.get("capital_per_capacity", kDemoCapitalPerCapacity))));
		building.jobs = double(entry.get("jobs", 0.0));
		building.wage = double(entry.get("wage", 0.0));
		building.opex = double(entry.get("opex", 0.0));
		// F7: mistura de escolaridade. Save v8 sem a chave: usa a da receita
		// resolvida; primária segue o padrão do struct (quase só básico).
		if (entry.has("education_mix")) {
			const Array mix = entry.get("education_mix", Array());
			double total = 0.0;
			for (int level = 0;
					level < kDemoEducationLevels && level < mix.size(); ++level) {
				building.education_mix[level] = std::max(0.0, double(mix[level]));
				total += building.education_mix[level];
			}
			if (total > 1e-9) {
				for (int level = 0; level < kDemoEducationLevels; ++level) {
					building.education_mix[level] /= total;
				}
			}
		} else if (building.recipe >= 0
				&& building.recipe < static_cast<int>(ws.recipes.size())) {
			const DemoRecipe& recipe =
				ws.recipes[static_cast<std::size_t>(building.recipe)];
			for (int level = 0; level < kDemoEducationLevels; ++level) {
				building.education_mix[level] = recipe.education_mix[level];
			}
		}
		building.output_scale = double(entry.get("output_scale", 1.0));
		building.last_utilization = double(entry.get("last_utilization", 1.0));
		building.ramp_days = std::max(0, int(entry.get("ramp_days", 0)));
		building.strategic = bool(entry.get("strategic", false));
		building.loss_days = int(entry.get("loss_days", 0));
		building.bankrupt = bool(entry.get("bankrupt", false));
		building.depleted = bool(entry.get("depleted", false));
		ws.buildings.push_back(building);
	}

	// §15 (save v12): obras em andamento. Save v11 e anteriores não têm a chave:
	// lista vazia (nenhum projeto em voo no mundo antigo — migração trivial).
	ws.construction_projects.clear();
	const Array saved_projects = data.get("construction_projects", Array());
	for (int index = 0; index < saved_projects.size(); ++index) {
		if (saved_projects[index].get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary entry = saved_projects[index];
		DemoConstructionProject project;
		project.id = std::string(String(entry.get("id", "")).utf8().get_data());
		if (project.id.empty()) {
			continue;
		}
		// §19 (W1): obra de infraestrutura urbana. Ausente no save anterior ao
		// W1 = 0 (obra econômica normal). A obra urbana não tem produto, então
		// a migração de índice de produto abaixo não se aplica a ela.
		project.urban_action = std::clamp(
			int(entry.get("urban_action", 0)), 0, 2);
		project.urban_zone = std::clamp(
			int(entry.get("urban_zone", 0)), 0, ZONE_COUNT - 1);
		project.urban_km2 = safe_nonneg(double(entry.get("urban_km2", 0.0)));
		const bool urban_project = project.urban_action != 0;
		project.building_id = std::string(
			String(entry.get("building_id", "")).utf8().get_data());
		project.city = std::string(String(entry.get("city", "")).utf8().get_data());
		project.owner = std::string(
			String(entry.get("owner", "private")).utf8().get_data());
		project.group_id = std::string(
			String(entry.get("group_id", "")).utf8().get_data());
		project.resource = std::string(
			String(entry.get("resource", "")).utf8().get_data());
		project.output_product = int(entry.get("output_product", -1));
		if (!urban_project && (has_saved_products || needs_legacy_migration)) {
			project.output_product = migrate_index(project.output_product);
			if (project.output_product < 0) {
				continue;  // produto removido: obra descartada
			}
		}
		const String recipe_id = String(entry.get("recipe", ""));
		project.recipe = recipe_id.is_empty()
			? -1 : core.demo_recipe_index(std::string(recipe_id.utf8().get_data()));
		project.input_product = int(entry.get("input_product", -1));
		if (!urban_project && (has_saved_products || needs_legacy_migration)) {
			project.input_product = migrate_index(project.input_product);
		}
		project.capacity = std::max(0.0, double(entry.get("capacity", 0.0)));
		project.jobs = std::max(0.0, double(entry.get("jobs", 0.0)));
		project.wage = std::max(0.0, double(entry.get("wage", 0.0)));
		project.opex = std::max(0.0, double(entry.get("opex", 0.0)));
		project.input_per_output = double(entry.get("input_per_output", 0.0));
		const Array mix = entry.get("education_mix", Array());
		if (mix.size() >= kDemoEducationLevels) {
			double total = 0.0;
			for (int level = 0; level < kDemoEducationLevels; ++level) {
				project.education_mix[level] = std::max(0.0, double(mix[level]));
				total += project.education_mix[level];
			}
			if (total > 1e-9) {
				for (int level = 0; level < kDemoEducationLevels; ++level) {
					project.education_mix[level] /= total;
				}
			}
		}
		project.local_share = std::clamp(
			double(entry.get("local_share", 0.0)), 0.0, 1.0);
		project.partner_group = std::string(
			String(entry.get("partner_group", "")).utf8().get_data());
		project.strategic = bool(entry.get("strategic", false));
		project.expand_existing = bool(entry.get("expand_existing", false));
		project.total_cost = std::max(0.0, double(entry.get("total_cost", 0.0)));
		project.labor_cost_total = std::max(0.0,
			double(entry.get("labor_cost_total", 0.0)));
		project.paid = std::max(0.0, double(entry.get("paid", 0.0)));
		project.labor_paid = std::max(0.0, double(entry.get("labor_paid", 0.0)));
		project.reserved = std::max(0.0, double(entry.get("reserved", 0.0)));
		project.duration_days = std::max(1, int(entry.get("duration_days", 30)));
		project.days_elapsed = std::max(0, int(entry.get("days_elapsed", 0)));
		project.paused = bool(entry.get("paused", false));
		project.pause_reason = std::string(
			String(entry.get("pause_reason", "")).utf8().get_data());
		project.created_day = int(entry.get("created_day", 0));
		const Array materials = entry.get("materials", Array());
		const Array material_qty = entry.get("material_qty", Array());
		const Array material_delivered = entry.get("material_delivered", Array());
		project.material_count = std::min(
			static_cast<int>(materials.size()), kDemoMaxConstructionMaterials);
		for (int m = 0; m < project.material_count; ++m) {
			int product = int(materials[m]);
			if (has_saved_products || needs_legacy_migration) {
				product = migrate_index(product);
			}
			project.materials[m] = product;
			project.material_qty[m] = m < material_qty.size()
				? std::max(0.0, double(material_qty[m])) : 0.0;
			project.material_delivered[m] = m < material_delivered.size()
				? std::max(0.0, double(material_delivered[m])) : 0.0;
		}
		ws.construction_projects.push_back(std::move(project));
	}

	ws.in_transit.clear();
	Array transit = data.get("in_transit", Array());
	for (int index = 0; index < transit.size(); ++index) {
		Dictionary entry = transit[index];
		DemoShipment shipment;
		shipment.to_city =
			std::string(String(entry.get("to_city", "")).utf8().get_data());
		shipment.product = int(entry.get("product", 0));
		if (has_saved_products || needs_legacy_migration) {
			const int migrated = migrate_index(shipment.product);
			if (migrated < 0) {
				// Produto removido da cadeia em trânsito (farinha/pão ou
				// body_armor): carga descartada.
				continue;
			}
			shipment.product = migrated;
		}
		shipment.amount = double(entry.get("amount", 0.0));
		shipment.arrival_day = int(entry.get("arrival_day", 0));
		// F4: reserva da frota (save v5). Save anterior: frota livre ao carregar.
		shipment.fleet_nation =
			std::string(String(entry.get("fleet_nation", "")).utf8().get_data());
		shipment.fleet_trucks = double(entry.get("fleet_trucks", 0.0));
		shipment.fleet_ships = double(entry.get("fleet_ships", 0.0));
		shipment.fleet_planes = double(entry.get("fleet_planes", 0.0));
		shipment.fuel = double(entry.get("fuel", 0.0));
		ws.in_transit.push_back(shipment);
	}

	// Portos (campo aditivo, ausente em saves anteriores ao porto): o save
	// preserva capacidade, expansão em obra e fila do dia. Sem "ports", mantém
	// os portos do mundo já carregado e recalcula a capacidade pela população
	// que veio no save (save antigo é compatível: comportamento anterior).
	const bool has_saved_ports = data.has("ports");
	if (has_saved_ports) {
		ws.ports.clear();
		Array ports = data.get("ports", Array());
		for (int index = 0; index < ports.size(); ++index) {
			Dictionary entry = ports[index];
			Port port;
			port.id = std::string(String(entry.get("id", "")).utf8().get_data());
			port.name = std::string(String(entry.get("name", "")).utf8().get_data());
			port.nation = std::string(String(entry.get("nation", "")).utf8().get_data());
			port.landmass =
				std::string(String(entry.get("landmass", "")).utf8().get_data());
			port.host_city =
				std::string(String(entry.get("host_city", "")).utf8().get_data());
			port.center = {double(entry.get("x", 0.0)), double(entry.get("y", 0.0))};
			port.base_capacity_t_per_day =
				safe_nonneg(double(entry.get("base_capacity_t_per_day", 0.0)));
			port.capacity_t_per_day =
				safe_nonneg(double(entry.get("capacity_t_per_day",
					port.base_capacity_t_per_day)));
			port.pending_capacity_t_per_day =
				safe_nonneg(double(entry.get("pending_capacity_t_per_day", 0.0)));
			port.pending_days = std::max(0, int(entry.get("pending_days", 0)));
			ws.ports.push_back(std::move(port));
		}
	}

	core.demo_rebuild_indexes();
	if (!has_saved_ports && !ws.ports.empty()) {
		core.demo_init_port_capacities();
	}
	// F4: frota (save v5). Save anterior não tem o bloco: deriva de
	// população/PIB, documentado como `source: derived`.
	ws.fleets.clear();
	ws.fleet_order.clear();
	const Array saved_fleets = data.get("fleets", Array());
	for (int index = 0; index < saved_fleets.size(); ++index) {
		Dictionary entry = saved_fleets[index];
		const std::string nation =
			std::string(String(entry.get("nation", "")).utf8().get_data());
		if (nation.empty()) {
			continue;
		}
		DemoFleet fleet;
		fleet.trucks = safe_nonneg(double(entry.get("trucks", 0.0)));
		fleet.ships = safe_nonneg(double(entry.get("ships", 0.0)));
		fleet.planes = safe_nonneg(double(entry.get("planes", 0.0)));
		fleet.target_trucks = safe_nonneg(double(entry.get("target_trucks", 0.0)));
		fleet.target_ships = safe_nonneg(double(entry.get("target_ships", 0.0)));
		fleet.target_planes = safe_nonneg(double(entry.get("target_planes", 0.0)));
		fleet.fuel = safe_nonneg(double(entry.get("fuel", 0.0)));
		fleet.fuel_target = safe_nonneg(double(entry.get("fuel_target", 0.0)));
		fleet.spare_parts = safe_nonneg(double(entry.get("spare_parts", 0.0)));
		Array health = entry.get("health", Array());
		for (int mode = 0; mode < 3; ++mode) {
			fleet.health[mode] = mode < health.size()
				? std::clamp(double(health[mode]), kDemoFleetHealthFloor, 1.0) : 1.0;
		}
		fleet.fuel_used_total = double(entry.get("fuel_used_total", 0.0));
		fleet.fuel_bought_total = double(entry.get("fuel_bought_total", 0.0));
		fleet.parts_used_total = double(entry.get("parts_used_total", 0.0));
		fleet.vehicles_bought_total =
			double(entry.get("vehicles_bought_total", 0.0));
		fleet.derived = bool(entry.get("derived", false));
		ws.fleets.emplace(nation, fleet);
		ws.fleet_order.push_back(nation);
	}
	// Reserva das cargas em trânsito: reconstrói os veículos ocupados.
	for (const DemoShipment& shipment : ws.in_transit) {
		if (shipment.fleet_nation.empty()) {
			continue;
		}
		DemoTransportDemand busy;
		busy.trucks = shipment.fleet_trucks;
		busy.ships = shipment.fleet_ships;
		busy.planes = shipment.fleet_planes;
		const auto found = ws.fleets.find(shipment.fleet_nation);
		if (found == ws.fleets.end()) {
			continue;
		}
		found->second.trucks_busy += busy.trucks;
		found->second.ships_busy += busy.ships;
		found->second.planes_busy += busy.planes;
	}
	if (ws.fleets.empty()) {
		core.demo_init_fleets();
	} else {
		// Índices dos bens da frota (products já carregados antes do load).
		ws.fleet_product_fuel = core.demo_product_index(std::string(String("fuel").utf8().get_data()));
		ws.fleet_product_truck = core.demo_product_index(std::string(String("truck").utf8().get_data()));
		ws.fleet_product_ship = core.demo_product_index(std::string(String("cargo_ship").utf8().get_data()));
		ws.fleet_product_plane = core.demo_product_index(std::string(String("cargo_plane").utf8().get_data()));
		ws.fleet_product_parts = core.demo_product_index(std::string(String("spare_parts").utf8().get_data()));
	}

	// F8: negócios de serviço (save v10). Save v9 e anteriores não têm o bloco:
	// semeia os negócios como num mundo novo (migração declarada).
	ws.service_businesses.clear();
	ws.next_service_business_id = 1;
	ws.service_bankruptcies = int(data.get("service_bankruptcies", 0));
	const Array saved_businesses = data.get("service_businesses", Array());
	for (int index = 0; index < saved_businesses.size(); ++index) {
		if (saved_businesses[index].get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary entry = saved_businesses[index];
		DemoServiceBusiness business;
		business.id = int(entry.get("id", 0));
		business.sector = int(entry.get("sector", -1));
		business.city = std::string(
			String(entry.get("city", "")).utf8().get_data());
		business.owner_stratum = std::clamp(
			int(entry.get("owner_stratum", kDemoEducationLevels - 1)),
			0, kDemoEducationLevels - 1);
		business.capacity = safe_nonneg(double(entry.get("capacity", 0.0)));
		business.jobs = safe_nonneg(double(entry.get("jobs", 0.0)));
		business.workers = safe_nonneg(double(entry.get("workers", 0.0)));
		business.capital = safe_nonneg(double(entry.get("capital", 0.0)));
		business.capital_left = safe_nonneg(double(entry.get("capital_left", 0.0)));
		business.utilization = std::clamp(
			double(entry.get("utilization", 0.0)), 0.0, 1.0);
		business.revenue = double(entry.get("revenue", 0.0));
		business.wages = safe_nonneg(double(entry.get("wages", 0.0)));
		business.input_cost = safe_nonneg(double(entry.get("input_cost", 0.0)));
		business.service_cost = safe_nonneg(double(entry.get("service_cost", 0.0)));
		business.profit = double(entry.get("profit", 0.0));
		business.loss_days = std::max(0, int(entry.get("loss_days", 0)));
		business.bankrupt = bool(entry.get("bankrupt", false));
		business.opened_day = int(entry.get("opened_day", 0));
		const double seed = double(entry.get("name_seed", 0.0));
		business.name_seed = seed > 0.0
			? static_cast<unsigned long long>(seed) : 0ULL;
		if (business.name_seed == 0ULL) {
			unsigned long long hash = 1469598103934665603ULL;
			for (const char c : business.city + "|" + std::to_string(business.id)) {
				hash ^= static_cast<unsigned char>(c);
				hash *= 1099511628211ULL;
			}
			business.name_seed = hash;
		}
		ws.next_service_business_id = std::max(
			ws.next_service_business_id, business.id + 1);
		ws.service_businesses.push_back(std::move(business));
	}
	if (ws.service_businesses.empty() && ws.services_loaded) {
		core.demo_seed_services();
	} else {
		core.demo_rebuild_service_indexes();
	}
	// F9: save v10 e anteriores não têm grupos. Semeia a partir dos ativos
	// privados (id determinístico); os nomes entram pelo cliente no próximo
	// boot, como no mundo recém-criado.
	if (ws.groups.empty()) {
		ws.groups_seeded = false;
		core.demo_seed_groups();
	} else {
		ws.groups_seeded = true;
	}

	// §19 (save v13): solo urbano, moradia e prefeitura por cidade. O save v12
	// e anteriores não têm a chave: os vetores ficam como estão (vazios num load
	// frio ou com os padrões carregados pelo demo_set_urban), sem lógica diária
	// a reconstruir. O casamento é por id, não por posição.
	if (data.has("urban")) {
		const Array saved_urban = data.get("urban", Array());
		ws.land.clear();
		ws.housing.clear();
		ws.city_gov.clear();
		if (!saved_urban.is_empty()) {
			ws.land.assign(ws.cities.size(), CityLand{});
			ws.housing.assign(ws.cities.size(), CityHousing{});
			ws.city_gov.assign(ws.cities.size(), CityGov{});
		}
		for (int index = 0; index < saved_urban.size(); ++index) {
			if (saved_urban[index].get_type() != Variant::DICTIONARY) {
				continue;
			}
			const Dictionary entry = saved_urban[index];
			const std::string city_id = std::string(
				String(entry.get("city", "")).utf8().get_data());
			const auto found = ws.city_lookup.find(city_id);
			if (found == ws.city_lookup.end() || found->second < 0
					|| static_cast<std::size_t>(found->second)
						>= ws.cities.size()) {
				continue;
			}
			const std::size_t city_index =
				static_cast<std::size_t>(found->second);
			CityLand& land = ws.land[city_index];
			CityHousing& house = ws.housing[city_index];
			CityGov& gov = ws.city_gov[city_index];
			const Array area = entry.get("area_km2", Array());
			const Array max_density = entry.get("max_density", Array());
			const Array land_price = entry.get("land_price", Array());
			for (int zone = 0; zone < ZONE_COUNT; ++zone) {
				land.area_km2[zone] = zone < area.size()
					? safe_nonneg(double(area[zone])) : 0.0;
				land.max_density[zone] = zone < max_density.size()
					? static_cast<Density>(std::clamp(
						int(max_density[zone]), 0, DENSITY_COUNT - 1))
					: DENSITY_LOW;
				land.land_price[zone] = zone < land_price.size()
					? safe_nonneg(double(land_price[zone])) : 0.0;
			}
			land.reserve_km2 = safe_nonneg(
				double(entry.get("reserve_km2", 0.0)));
			land.infra_capacity = safe_nonneg(
				double(entry.get("infra_capacity", 0.0)));
			land.informal_km2 = safe_nonneg(
				double(entry.get("informal_km2", 0.0)));
			const Array zone_demand = entry.get("zone_demand", Array());
			for (int zone = 0; zone < ZONE_COUNT; ++zone) {
				land.zone_demand[zone] = zone < zone_demand.size()
					? safe_nonneg(double(zone_demand[zone])) : 0.0;
			}
			// §19 (integração): base do custo crescente. Save anterior ao campo
			// deriva do estado atual (área/reserva de hoje = base do próximo
			// carregamento): o custo volta a subir a partir de agora em vez de
			// inventar um raio original que ninguém mediu.
			land.initial_area_km2 = safe_nonneg(
				double(entry.get("initial_area_km2", 0.0)));
			land.initial_reserve_km2 = safe_nonneg(
				double(entry.get("initial_reserve_km2", 0.0)));
			if (land.initial_area_km2 <= 0.0) {
				double area_total = 0.0;
				for (int zone = 0; zone < ZONE_COUNT; ++zone) {
					area_total += safe_nonneg(land.area_km2[zone]);
				}
				land.initial_area_km2 = area_total;
			}
			if (land.initial_reserve_km2 <= 0.0) {
				land.initial_reserve_km2 = land.reserve_km2;
			}
			const Array units = entry.get("units", Array());
			for (int density = 0; density < DENSITY_COUNT; ++density) {
				if (density >= units.size()
						|| units[density].get_type() != Variant::ARRAY) {
					continue;
				}
				const Array row = units[density];
				for (int tier = 0; tier < TIER_COUNT; ++tier) {
					house.units[density][tier] = tier < row.size()
						? safe_nonneg(double(row[tier])) : 0.0;
				}
			}
			house.condition = safe_unit(
				double(entry.get("condition", 0.0)));
			const Array owner_share = entry.get("owner_share", Array());
			for (int owner = 0; owner < OWNER_COUNT; ++owner) {
				house.owner_share[owner] = owner < owner_share.size()
					? safe_nonneg(double(owner_share[owner])) : 0.0;
			}
			const Array tenure = entry.get("tenure", Array());
			for (int tier = 0; tier < TIER_COUNT; ++tier) {
				if (tier >= tenure.size()
						|| tenure[tier].get_type() != Variant::ARRAY) {
					continue;
				}
				const Array row = tenure[tier];
				for (int kind = 0; kind < TENURE_COUNT; ++kind) {
					house.tenure[tier][kind] = kind < row.size()
						? safe_nonneg(double(row[kind])) : 0.0;
				}
			}
			const Array rent = entry.get("rent", Array());
			const Array price = entry.get("price", Array());
			const Array vacancy = entry.get("vacancy", Array());
			for (int tier = 0; tier < TIER_COUNT; ++tier) {
				house.rent[tier] = tier < rent.size()
					? safe_nonneg(double(rent[tier])) : 0.0;
				house.price[tier] = tier < price.size()
					? safe_nonneg(double(price[tier])) : 0.0;
				house.vacancy[tier] = tier < vacancy.size()
					? safe_nonneg(double(vacancy[tier])) : 0.0;
			}
			house.informal_units = safe_nonneg(
				double(entry.get("informal_units", 0.0)));
			const Dictionary gov_entry = entry.get("gov", Dictionary());
			gov.cash = double(gov_entry.get("cash", 0.0));
			gov.iptu_rate = safe_unit(
				double(gov_entry.get("iptu_rate", 0.0)));
			gov.fee_rate = safe_unit(
				double(gov_entry.get("fee_rate", 0.0)));
			gov.transfer_share = safe_unit(
				double(gov_entry.get("transfer_share", 0.0)));
			gov.month_revenue_iptu = safe_nonneg(
				double(gov_entry.get("month_revenue_iptu", 0.0)));
			gov.month_revenue_fees = safe_nonneg(
				double(gov_entry.get("month_revenue_fees", 0.0)));
			gov.month_revenue_transfer = safe_nonneg(
				double(gov_entry.get("month_revenue_transfer", 0.0)));
			gov.month_expense_maintenance = safe_nonneg(
				double(gov_entry.get("month_expense_maintenance", 0.0)));
			gov.month_expense_infrastructure = safe_nonneg(
				double(gov_entry.get("month_expense_infrastructure", 0.0)));
			gov.month_expense_social_housing = safe_nonneg(
				double(gov_entry.get("month_expense_social_housing", 0.0)));
		}
	}

	// §19 (W2): prefeitura em operação (bloco próprio). Save v13 anterior ao W2
	// não tem a chave: fica sem repasse calibrado e sem obra, que é o estado
	// honesto (a próxima carga dos JSON preenche revenue_pct_gdp).
	if (data.has("city_gov")) {
		const Array saved_city_gov = data.get("city_gov", Array());
		for (int index = 0; index < saved_city_gov.size(); ++index) {
			if (saved_city_gov[index].get_type() != Variant::DICTIONARY) {
				continue;
			}
			const Dictionary entry = saved_city_gov[index];
			const std::string city_id = std::string(
				String(entry.get("city", "")).utf8().get_data());
			const auto found = ws.city_lookup.find(city_id);
			if (found == ws.city_lookup.end() || found->second < 0
					|| static_cast<std::size_t>(found->second)
						>= ws.city_gov.size()) {
				continue;
			}
			CityGov& gov = ws.city_gov[static_cast<std::size_t>(found->second)];
			gov.revenue_pct_gdp = safe_unit(
				double(entry.get("revenue_pct_gdp", 0.0)));
			gov.target_revenue_month = safe_nonneg(
				double(entry.get("target_revenue_month", 0.0)));
			gov.month_transfer_paid = safe_nonneg(
				double(entry.get("month_transfer_paid", 0.0)));
			const Array projects = entry.get("projects", Array());
			gov.project_count = 0;
			for (int slot = 0; slot < projects.size()
					&& gov.project_count < kCityGovMaxProjects; ++slot) {
				if (projects[slot].get_type() != Variant::DICTIONARY) {
					continue;
				}
				const Dictionary row = projects[slot];
				CityGovProject& project = gov.projects[gov.project_count];
				project = CityGovProject{};
				project.kind = static_cast<UrbanProjectKind>(std::clamp(
					int(row.get("kind", 0)), 0, URBAN_PROJECT_COUNT - 1));
				project.zone = std::clamp(int(row.get("zone", 0)), 0,
					ZONE_COUNT - 1);
				project.area_km2 = safe_nonneg(double(row.get("area_km2", 0.0)));
				project.units = safe_nonneg(double(row.get("units", 0.0)));
				project.cost = safe_nonneg(double(row.get("cost", 0.0)));
				project.reserved = safe_nonneg(double(row.get("reserved", 0.0)));
				project.days_left = std::max(0,
					int(row.get("days_left", 0)));
				project.created_day = int(row.get("created_day", 0));
				if (project.days_left <= 0) {
					continue;
				}
				++gov.project_count;
			}
		}
	}

	// §19 W3 — estado do mercado de moradia (bloco `housing`, aditivo). Save
	// anterior ao W3 não tem a chave: os campos ficam no padrão (0 = mercado
	// ainda não rodou) e a fase deriva a base de domicílios do estoque.
	if (data.has("housing")) {
		const Array saved_housing = data.get("housing", Array());
		for (int index = 0; index < saved_housing.size(); ++index) {
			if (saved_housing[index].get_type() != Variant::DICTIONARY) {
				continue;
			}
			const Dictionary entry = saved_housing[index];
			const std::string city_id = std::string(
				String(entry.get("city", "")).utf8().get_data());
			const auto found = ws.city_lookup.find(city_id);
			if (found == ws.city_lookup.end() || found->second < 0
					|| static_cast<std::size_t>(found->second)
						>= ws.housing.size()) {
				continue;
			}
			CityHousing& house = ws.housing[
				static_cast<std::size_t>(found->second)];
			house.households = safe_nonneg(
				double(entry.get("households", 0.0)));
			const Array hh_tier = entry.get("households_by_tier", Array());
			for (int tier = 0; tier < TIER_COUNT; ++tier) {
				house.households_by_tier[tier] = tier < hh_tier.size()
					? safe_nonneg(double(hh_tier[tier])) : 0.0;
			}
			house.pop_anchor = safe_nonneg(
				double(entry.get("pop_anchor", 0.0)));
			house.rent_anchored = bool(entry.get("rent_anchored", false));
			const Array cap_ratio = entry.get("rent_cap_ratio", Array());
			for (int tier = 0; tier < TIER_COUNT; ++tier) {
				house.rent_cap_ratio[tier] = tier < cap_ratio.size()
					? std::clamp(double(cap_ratio[tier]), 1.0, 2.5) : 1.0;
			}
			house.housing_stress = safe_unit(
				double(entry.get("housing_stress", 0.0)));
			house.stress_days = std::max(
				0, int(entry.get("stress_days", 0)));
			house.rent_paid_month = safe_nonneg(
				double(entry.get("rent_paid_month", 0.0)));
			house.rent_paid_today = safe_nonneg(
				double(entry.get("rent_paid_today", 0.0)));
			house.rent_households_month = safe_nonneg(
				double(entry.get("rent_households_month", 0.0)));
			house.rent_firms_month = safe_nonneg(
				double(entry.get("rent_firms_month", 0.0)));
			house.rent_government_month = safe_nonneg(
				double(entry.get("rent_government_month", 0.0)));
			house.rent_burden = safe_unit(
				double(entry.get("rent_burden", 0.0)));
			house.units_built_month = safe_nonneg(
				double(entry.get("units_built_month", 0.0)));
			house.month_marker = std::max(
				0, int(entry.get("month_marker", 0)));
			for (int slot = 0; slot < kDemoHousingMaxProjects; ++slot) {
				house.projects[slot] = HousingProject{};
			}
			const Array projects = entry.get("projects", Array());
			for (int slot = 0;
					slot < projects.size() && slot < kDemoHousingMaxProjects;
					++slot) {
				if (projects[slot].get_type() != Variant::DICTIONARY) {
					continue;
				}
				const Dictionary p = projects[slot];
				HousingProject& project = house.projects[slot];
				project.state = std::clamp(
					int(p.get("state", 0)), 0,
					static_cast<int>(HOUSING_PROJECT_GOVERNMENT));
				project.density = std::clamp(
					int(p.get("density", 0)), 0, DENSITY_COUNT - 1);
				project.tier = std::clamp(
					int(p.get("tier", 0)), 0, TIER_COUNT - 1);
				project.units = safe_nonneg(double(p.get("units", 0.0)));
				project.days_left = std::max(0, int(p.get("days_left", 0)));
			}
		}
	}
}

} // namespace edge
} // namespace ogs