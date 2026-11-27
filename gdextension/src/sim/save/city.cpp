// H1g — save/load por domínio: cidades, edificações, obras e blocos urbanos.
//
// Blocos movidos VERBATIM de save_state.cpp (mesmas linhas, mesma ordem de
// escrita de chaves). Qualquer reordenação muda os bytes do save.
#include "save_internal.h"

#include "../sim_constants.h"
#include "../sim_core.h"
#include "../storage.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace ogs::save {
using namespace detail;

namespace city {

// Línha 475–1322 de save_state.cpp: c78191571b2711b3.
void write(Dictionary& data, const WorldState& ws, const SimCore& core) {
	Array cities = make_array();
	for (const City& city : ws.cities) {
		Dictionary entry = make_object();
		entry["id"] = sim_str(city.id.c_str());
		entry["name"] = sim_str(city.name.c_str());
		entry["nation"] = sim_str(city.nation.c_str());
		entry["home_nation"] = sim_str(city.home_nation.c_str());
		entry["landmass"] = sim_str(city.landmass.c_str());
		Array center = make_array();
		center.append(city.center.x);
		center.append(city.center.y);
		entry["center"] = center;
		entry["population"] = city.population;
		entry["sim_population"] = city.simulation_population;
		entry["firm_cash"] = city.firm_cash;
		entry["service_cash"] = city.service_cash;
		// R1a: com o caixa unico regional ligado, `private_cash` e o dinheiro
		// real do setor privado. Desligado a chave NAO entra (save byte-identico
		// ao anterior).
		if (city.unified_cash) {
			entry["private_cash"] = city.private_cash;
			// R1a (correcao): sub-saldos virtuais. A soma dos dois e o pool;
			// o save OFF nao ganha nenhuma das chaves.
			entry["firm_sub"] = city.firm_sub;
			entry["service_sub"] = city.service_sub;
			// R1a (continuacao): estado do sub-teto externo e da fatia medida.
			// So no ON (save OFF continua byte-identico ao anterior).
			entry["firm_revenue_share"] =
				city.measured_firm_revenue_share();
			entry["firm_external_paid_today"] =
				city.firm_external_paid_today;
			entry["service_external_paid_today"] =
				city.service_external_paid_today;
		}
		// D-126 (v41): crédito de capital de giro da cidade e o prêmio de crise.
		entry["firm_debt"] = city.firm_debt;
		entry["firm_credit_arrears"] = city.firm_credit_arrears;
		entry["firm_credit_total"] = city.firm_credit_total;
		entry["firm_credit_rate_add"] = city.firm_credit_rate_add;
		entry["firm_credit_default_days"] = city.firm_credit_default_days;
		entry["firm_credit_defaults"] = city.firm_credit_defaults;
		entry["firm_credit_renegotiations"] =
			city.firm_credit_renegotiations;
		// C1 (save v26): salário atrasado das firmas da cidade (passivo).
		entry["service_wage_arrears"] = city.service_wage_arrears;
		entry["rural_wage_arrears"] = city.rural_wage_arrears;
		entry["government_budget"] = city.government_budget;
		// R1b (save v43): orçamento regional. Só entra com o Tesouro ligado;
		// save OFF continua byte-idêntico. O gasto acumulado é diagnóstico.
		if (ws.national_treasury) {
			entry["regional_budget_limit_month"] =
				city.regional_budget_limit_month;
			entry["regional_budget_spent_month"] =
				city.regional_budget_spent_month;
			entry["regional_budget_spent_total"] =
				city.regional_budget_spent_total;
			entry["regional_budget_deferred_month"] =
				city.regional_budget_deferred_month;
		}
		// R2a (v44): conta do setor privado no banco (claim de funding). Só
		// entra com a sub-flag `deposits`; OFF não ganha a chave.
		if (ws.banks_rework.enabled && ws.banks_rework.deposits) {
			entry["protected_cash"] = city.protected_cash;
			Array bank_accounts = make_array();
			for (const City::BankAccount& account : city.bank_accounts) {
				Dictionary row = make_object();
				row["bank_id"] = sim_str(account.bank_id.c_str());
				row["firm"] = account.firm;
				row["service"] = account.service;
				bank_accounts.append(row);
			}
			entry["bank_accounts"] = bank_accounts;
		}
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
		// B2: aluguel pendente às famílias donas (pago no dia seguinte, como o
		// capital). Sem persistir, o load perde a fatura e a conservação muda.
		entry["rent_income_next"] = city.rent_income_next;
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
		Array service_markets = make_array();
		for (int sector = 0; sector < kMaxServiceSectors; ++sector) {
			const ServiceMarket& market = city.service_markets[sector];
			Array fields = make_array();
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

		Array suitability = make_array();
		Array revenue = make_array();
		Array imported = make_array();
		Array exported = make_array();
		Array import_value = make_array();
		Array export_value = make_array();
		Array external_demand = make_array();
		Array import_price = make_array();
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
		Dictionary deposits = make_object();
		for (int product = 0; product < ws.products; ++product) {
			const Deposit& deposit = city.deposits[product];
			if (!deposit.tracked) {
				continue;
			}
			Dictionary deposit_entry = make_object();
			deposit_entry["initial_kt"] = deposit.initial_kt;
			deposit_entry["remaining_kt"] = deposit.remaining_kt;
			deposit_entry["extracted_kt"] = deposit.extracted_kt;
			deposit_entry["quality"] = deposit.quality;
			deposit_entry["depth_m"] = deposit.depth_m;
			deposits[String(core.product_name(product))] = deposit_entry;
		}
		entry["deposits"] = deposits;

		pop::write(entry, city, ws, core);
		// B3 (save v19): região da cidade (estados inteiros). Estado persistente
		// (população, caixa, coortes e quintis do interior); os fluxos diários
		// (emprego, renda, comida, satisfação) são derivados e recalculados.
		region::write(entry, city);
		entry["government_goods_spend"] = city.government_goods_spend;
		// §15 (save v12): emprego setorial (agro/indústria/serviços + informal).
		// É estado DERIVADO, mas o prólogo usa `sector_workers[1]` como base da
		// capacidade de construção do dia — sem persistir, o load zera o vetor e
		// a capacidade cai para o piso no primeiro tick (divergência de round-trip).
		entry["sector_workers"] = Array{city.sector_workers[0],
			city.sector_workers[1], city.sector_workers[2], city.sector_workers[3]};
		entry["sector_share"] = Array{city.sector_share[0],
			city.sector_share[1], city.sector_share[2], city.sector_share[3]};

		Array markets = make_array();
		for (int product = 0; product < ws.products; ++product) {
			Dictionary market = make_object();
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

	Array buildings = make_array();
	for (const Building& building : ws.buildings) {
		Dictionary entry = make_object();
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
		// D-086 (save v18): estágio de mina e a fatia de reserva que ele trabalha.
		entry["stage"] = building.stage;
		entry["stage_reserve_kt"] = building.stage_reserve_kt;
		entry["stage_mined_kt"] = building.stage_mined_kt;
		// B5 (save v21): royalty/imposto de concessão (fração da receita paga
		// ao Estado). 0 fora de concessão.
		entry["royalty"] = building.royalty;
		// C1 (save v26): salário atrasado da planta (passivo da firma).
		entry["wage_arrears"] = building.wage_arrears;
		// D-125 (save v40): eficiência de produção, transição, equipamento e
		// modo da refinaria. Mundo semeado nasce maduro (1.0/9999/equipamento
		// ligado); construção nova carrega a curva baixa da classe.
		entry["production_efficiency"] = building.production_efficiency;
		entry["efficiency_days"] = building.efficiency_days;
		entry["transition_days"] = building.transition_days;
		entry["equipment"] = building.equipment;
		entry["refinery_mode"] = building.refinery_mode;
		// D-126 (save v41): estado da sobrevivência em crise.
		entry["crisis_stage"] = building.crisis_stage;
		entry["last_layoff_day"] = building.last_layoff_day;
		entry["last_credit_day"] = building.last_credit_day;
		entry["last_liquidation_day"] = building.last_liquidation_day;
		entry["mothballed"] = building.mothballed;
		entry["mothball_day"] = building.mothball_day;
		entry["mothball_unpaid_days"] = building.mothball_unpaid_days;
		entry["reopen_signal_days"] = building.reopen_signal_days;
		entry["farm_area_reduced"] = building.farm_area_reduced;
		buildings.append(entry);
	}
	data["buildings"] = buildings;

	// §15 (save v12): obras de construção em andamento. Save v11 e anteriores
	// não têm a chave: não há projeto em voo (migração trivial).
	Array construction_projects = make_array();
	for (const ConstructionProject& project : ws.construction_projects) {
		Dictionary entry = make_object();
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
		// Save v14: kind da obra na fila única (§15 unificado) + os campos da
		// moradia (§19 W3) e a parcela de TERRA paga à prefeitura.
		entry["kind"] = project.kind;
		entry["housing_density"] = project.housing_density;
		entry["housing_tier"] = project.housing_tier;
		entry["housing_owner"] = project.housing_owner;
		entry["housing_units"] = project.housing_units;
		entry["land_cost"] = project.land_cost;
		// D-086 (save v18): obra de APROFUNDAMENTO de mina (-1 = obra normal).
		entry["deepen_stage"] = project.deepen_stage;
		// B5 (save v21): tipo de infra da região (0 estrada, 1 energia, 2 água)
		// quando kind == CONSTRUCTION_REGION_INFRA; -1 = não se aplica.
		entry["infra_type"] = project.infra_type;
		// G2 (save v25): obra de armazém (tipo e capacidade a entregar).
		entry["storage_type"] = project.storage_type;
		entry["storage_capacity_t"] = project.storage_capacity_t;
		// PARTE E (save v35): obra de energia — fonte (usina) e potência (MW).
		entry["power_fuel"] = project.power_fuel;
		entry["power_mw"] = project.power_mw;
		// E5 (save v36): interconexão nova — par canônico de nações.
		entry["power_nation_a"] = sim_str(project.power_nation_a.c_str());
		entry["power_nation_b"] = sim_str(project.power_nation_b.c_str());
		Array materials = make_array();
		Array material_qty = make_array();
		Array material_delivered = make_array();
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
	// padrões, posse). Sem set_urban (cenário sintético) a lista é vazia.
	Array urban = make_array();
	const std::size_t urban_count = std::min(
		{ws.land.size(), ws.housing.size(), ws.city_gov.size(),
			ws.cities.size()});
	for (std::size_t index = 0; index < urban_count; ++index) {
		const CityLand& land = ws.land[index];
		const CityHousing& house = ws.housing[index];
		const CityGov& gov = ws.city_gov[index];
		Dictionary entry = make_object();
		entry["city"] = sim_str(ws.cities[index].id.c_str());
		Array area = make_array();
		Array max_density = make_array();
		Array land_price = make_array();
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
		Array zone_demand = make_array();
		for (int zone = 0; zone < ZONE_COUNT; ++zone) {
			zone_demand.append(land.zone_demand[zone]);
		}
		entry["zone_demand"] = zone_demand;
		// Q-015-C: fração do envelope construída na escala da cidade. Save
		// anterior não tem a chave e volta ao comportamento antigo (1,0).
		Array zone_fill = make_array();
		for (int zone = 0; zone < ZONE_COUNT; ++zone) {
			zone_fill.append(land.zone_fill[zone]);
		}
		entry["zone_fill"] = zone_fill;
		// §19 (integração): base do custo crescente (raio e reserva no
		// carregamento). Save anterior não tem a chave: cai no campo do loader
		// (ausência explícita, fator 1 sem inventar aumento).
		entry["initial_area_km2"] = land.initial_area_km2;
		entry["initial_reserve_km2"] = land.initial_reserve_km2;
		Array units = make_array();
		for (int density = 0; density < DENSITY_COUNT; ++density) {
			Array row = make_array();
			for (int tier = 0; tier < TIER_COUNT; ++tier) {
				row.append(house.units[density][tier]);
			}
			units.append(row);
		}
		entry["units"] = units;
		entry["condition"] = house.condition;
		Array owner_share = make_array();
		for (int owner = 0; owner < OWNER_COUNT; ++owner) {
			owner_share.append(house.owner_share[owner]);
		}
		entry["owner_share"] = owner_share;
		Array tenure = make_array();
		for (int tier = 0; tier < TIER_COUNT; ++tier) {
			Array row = make_array();
			for (int kind = 0; kind < TENURE_COUNT; ++kind) {
				row.append(house.tenure[tier][kind]);
			}
			tenure.append(row);
		}
		entry["tenure"] = tenure;
		Array rent = make_array();
		Array price = make_array();
		Array vacancy = make_array();
		for (int tier = 0; tier < TIER_COUNT; ++tier) {
			rent.append(house.rent[tier]);
			price.append(house.price[tier]);
			vacancy.append(house.vacancy[tier]);
		}
		entry["rent"] = rent;
		entry["price"] = price;
		entry["vacancy"] = vacancy;
		entry["informal_units"] = house.informal_units;
		Dictionary gov_entry = make_object();
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
	Array city_gov_save = make_array();
	const std::size_t city_gov_count =
		std::min(ws.city_gov.size(), ws.cities.size());
	for (std::size_t index = 0; index < city_gov_count; ++index) {
		const CityGov& gov = ws.city_gov[index];
		Dictionary entry = make_object();
		entry["city"] = sim_str(ws.cities[index].id.c_str());
		entry["revenue_pct_gdp"] = gov.revenue_pct_gdp;
		entry["target_revenue_month"] = gov.target_revenue_month;
		entry["month_transfer_paid"] = gov.month_transfer_paid;
		// A6: janela do fechamento mensal desta cidade (escalonamento).
		entry["month_marker"] = gov.month_marker;
		Array projects = make_array();
		for (int slot = 0; slot < gov.project_count; ++slot) {
			const CityGovProject& project = gov.projects[slot];
			Dictionary row = make_object();
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
	// domicílios, o sinal de aperto e os fluxos do mês. Desde o save v14 as
	// obras em andamento NÃO moram aqui: elas são `construction_projects` com
	// kind CONSTRUCTION_HOUSING (fila única §15).
	Array housing_state = make_array();
	const std::size_t housing_count = std::min(
		ws.housing.size(), ws.cities.size());
	for (std::size_t index = 0; index < housing_count; ++index) {
		const CityHousing& house = ws.housing[index];
		Dictionary entry = make_object();
		entry["city"] = sim_str(ws.cities[index].id.c_str());
		entry["households"] = house.households;
		Array households_by_tier = make_array();
		for (int tier = 0; tier < TIER_COUNT; ++tier) {
			households_by_tier.append(house.households_by_tier[tier]);
		}
		entry["households_by_tier"] = households_by_tier;
		entry["pop_anchor"] = house.pop_anchor;
		entry["rent_anchored"] = house.rent_anchored;
		Array rent_cap_ratio = make_array();
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
		// save v14: as obras de moradia vivem em `construction_projects`
		// (kind CONSTRUCTION_HOUSING); não há mais lista própria aqui.
		housing_state.append(entry);
	}
	data["housing"] = housing_state;

	// F8: negócios de serviço (catálogo global). O nome NÃO é gravado: é
	// derivado da semente + cultura do dono no GDScript (rótulo), então o save
	// guarda só `name_seed` e o vínculo cidade/setor.
	Array service_businesses = make_array();
	for (const ServiceBusiness& business : ws.service_businesses) {
		Dictionary entry = make_object();
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
}

// Linhas 3348–4459 (cidades, edificações e obras): dacdcf7495213a5e.
void read(WorldState& ws, SimCore& core, const Dictionary& data, int version,
		const std::function<Array(const Array&)>& migrate_array,
		const std::function<int(int)>& migrate_index,
		const std::function<int(int)>& legacy_substitute,
		bool has_saved_products, bool needs_legacy_migration,
		int legacy_fuel_index, int legacy_gasoline, int legacy_diesel,
		int legacy_jet) {
	ws.cities.clear();
	Array cities = data.get("cities", Array());
	for (int index = 0; index < cities.size(); ++index) {
		Dictionary entry = cities[index];
		City city;
		city.id = std::string(String(entry.get("id", "")).utf8().get_data());
		city.name = std::string(String(entry.get("name", "")).utf8().get_data());
		city.nation = std::string(String(entry.get("nation", "")).utf8().get_data());
		// F5 v7: preço de mercado passou a ficar na ÂNCORA. Saves v6 e
		// anteriores guardavam preço local; converte uma vez pela taxa da nação
		// (as moedas foram carregadas antes das cidades).
		const double save_fx = version >= 7
			? 1.0 : std::max(1e-9, core.fx_rate(city.nation));
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
		// R1a: chave aditiva do caixa unico regional. Sem ela (save anterior ou
		// flag desligada) o modo vem da configuracao e o pool nasce da soma.
		if (entry.has("private_cash")) {
			city.private_cash = double(entry.get("private_cash", 0.0));
			city.unified_cash = true;
		}
		// R1a (correcao): sub-saldos virtuais. Save novo traz os dois; save
		// legado (ON anterior a correcao, ou sem as chaves) e semeado dos
		// espelhos antigos, preservando o total. `normalize_subbalances`
		// fecha a invariante e impede sub-saldo negativo.
		if (city.unified_cash) {
			if (entry.has("firm_sub") && entry.has("service_sub")) {
				city.firm_sub =
					std::max(0.0, double(entry.get("firm_sub", 0.0)));
				city.service_sub =
					std::max(0.0, double(entry.get("service_sub", 0.0)));
				city.firm_cash = city.firm_sub;
				city.service_cash = city.service_sub;
				city.normalize_subbalances();
			} else {
				city.seed_subbalances_from_mirrors();
			}
		}
		// R1a (continuacao): fatia medida e pagamentos externos do dia. Sem as
		// chaves (save OFF ou anterior a correcao), a fatia e derivada dos
		// espelhos em `apply_regional_cash_after_load` e o dia comeca zerado.
		if (entry.has("firm_revenue_share")) {
			city.firm_revenue_share =
				double(entry.get("firm_revenue_share", -1.0));
		}
		city.firm_external_paid_today = entry.has("firm_external_paid_today")
			? std::max(0.0, double(entry.get("firm_external_paid_today", 0.0)))
			: 0.0;
		city.service_external_paid_today =
			entry.has("service_external_paid_today")
			? std::max(0.0,
				double(entry.get("service_external_paid_today", 0.0)))
			: 0.0;
		// R2a (v44): conta bancária do setor privado e piso do sweep. Sem as
		// chaves (save anterior/OFF) a conta nasce vazia.
		city.bank_accounts.clear();
		city.protected_cash = std::max(0.0,
			double(entry.get("protected_cash", 0.0)));
		const Array bank_accounts = entry.get("bank_accounts", Array());
		for (int index = 0; index < bank_accounts.size(); ++index) {
			if (bank_accounts[index].get_type() != Variant::DICTIONARY) {
				continue;
			}
			const Dictionary row = bank_accounts[index];
			const std::string bank_id(
				String(row.get("bank_id", "")).utf8().get_data());
			if (bank_id.empty()) {
				continue;
			}
			City::BankAccount account;
			account.bank_id = bank_id;
			account.firm = safe_nonneg(double(row.get("firm", 0.0)));
			account.service = safe_nonneg(double(row.get("service", 0.0)));
			city.bank_accounts.push_back(account);
		}
		std::sort(city.bank_accounts.begin(), city.bank_accounts.end(),
			[](const City::BankAccount& a, const City::BankAccount& b) {
				return a.bank_id < b.bank_id;
			});
		// D-126 (v41): crédito de giro da cidade. Lido pela PRESENÇA da chave,
		// não pela versão: `banks[].loans` é lido em qualquer versão, e zerar a
		// dívida de um save reconstruído/rebaixado abriria o gap crédito ×
		// passivo que o invariante financeiro acusa. Sem a chave (save v40 real)
		// o estado é neutro: dívida zero, como antes da D-126.
		city.firm_debt = entry.has("firm_debt")
			? safe_nonneg(double(entry.get("firm_debt", 0.0))) : 0.0;
		city.firm_credit_arrears = entry.has("firm_credit_arrears")
			? safe_nonneg(double(entry.get("firm_credit_arrears", 0.0))) : 0.0;
		city.firm_credit_total = entry.has("firm_credit_total")
			? safe_nonneg(double(entry.get("firm_credit_total", 0.0))) : 0.0;
		city.firm_credit_rate_add = entry.has("firm_credit_rate_add")
			? std::max(0.0, double(entry.get("firm_credit_rate_add", 0.0)))
			: 0.0;
		city.firm_credit_default_days = entry.has("firm_credit_default_days")
			? std::max(0, int(entry.get("firm_credit_default_days", 0))) : 0;
		city.firm_credit_defaults = entry.has("firm_credit_defaults")
			? std::max(0, int(entry.get("firm_credit_defaults", 0))) : 0;
		city.firm_credit_renegotiations = entry.has("firm_credit_renegotiations")
			? std::max(0, int(entry.get("firm_credit_renegotiations", 0)))
			: 0;
		// C1 (save v26): salário atrasado das firmas da cidade. v25 e
		// anteriores não têm a chave — nasce 0.
		city.service_wage_arrears = version >= 26
			? std::max(0.0, double(entry.get("service_wage_arrears", 0.0))) : 0.0;
		city.rural_wage_arrears = version >= 26
			? std::max(0.0, double(entry.get("rural_wage_arrears", 0.0))) : 0.0;
		city.government_budget = double(entry.get("government_budget", 0.0));
		// R1b (save v43): orçamento regional. Save anterior/ausente nasce 0;
		// `apply_national_treasury_after_load` deriva o limite do restante.
		city.regional_budget_limit_month = std::max(0.0,
			double(entry.get("regional_budget_limit_month", 0.0)));
		city.regional_budget_spent_month = std::max(0.0,
			double(entry.get("regional_budget_spent_month", 0.0)));
		city.regional_budget_spent_total = std::max(0.0,
			double(entry.get("regional_budget_spent_total", 0.0)));
		city.regional_budget_deferred_month = std::max(0.0,
			double(entry.get("regional_budget_deferred_month", 0.0)));
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
		city.rent_income_next = std::max(0.0,
			double(entry.get("rent_income_next", 0.0)));
		city.gdp = double(entry.get("gdp", 0.0));
		// F8: serviços (save v10). Sem o bloco (v9 e anteriores), os mercados
		// ficam no preço-âncora do catálogo e os negócios são semeados no fim
		// do load (`seed_services`).
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
		for (int sector = 0; sector < kMaxServiceSectors; ++sector) {
			ServiceMarket& market = city.service_markets[sector];
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
			const String key = String(core.product_name(product));
			if (!saved_deposits.has(key)) {
				continue;
			}
			Dictionary deposit_entry = saved_deposits[key];
			Deposit& deposit = city.deposits[product];
			deposit.tracked = true;
			deposit.initial_kt = double(deposit_entry.get("initial_kt", 0.0));
			deposit.remaining_kt = double(deposit_entry.get("remaining_kt", 0.0));
			deposit.extracted_kt = double(deposit_entry.get("extracted_kt", 0.0));
			deposit.quality = double(deposit_entry.get("quality", 1.0));
			deposit.depth_m = double(deposit_entry.get("depth_m", 0.0));
		}

		pop::read(entry, city, ws, core, version);
		region::read(entry, city, ws, core, version);
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
		pop::read_categories(entry, city, ws);

		Array markets = entry.get("markets", Array());
		// F0: estoque do combustivel generico no save antigo (indice antigo de
		// `fuel` na lista salva). Rateado nas tres grades logo abaixo.
		double legacy_fuel_stock = 0.0;
		if (legacy_fuel_index >= 0 && legacy_fuel_index < markets.size()) {
			const Dictionary legacy_market = markets[legacy_fuel_index];
			legacy_fuel_stock = safe_nonneg(
				double(legacy_market.get("stock", 0.0)));
		}
		if (has_saved_products || needs_legacy_migration) {
			// Remapeia os mercados antigos por id (save v7 com 114 produtos,
			// descartando `body_armor`) ou pelo deslocamento -2 (save v1/v2 com
			// farinha/pão). Mercado sem correspondência recebe o preço-base.
			Array remapped;
			remapped.resize(ws.products);
			for (int new_index = 0; new_index < ws.products; ++new_index) {
				Dictionary m;
				m["stock"] = 0.0;
				m["price"] = core.base_price(new_index);
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
				city.markets[product].price = core.base_price(product);
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
		// F0: o estoque generico de `fuel` do save antigo entra nas tres
		// grades pelas fracoes da refinaria. O preco local de cada grade comeca
		// no preco-base declarado em goods.json (nao ha preco generico a
		// herdar sem inventar conversao entre bens diferentes).
		if (legacy_fuel_stock > 0.0) {
			city.markets[legacy_gasoline].stock +=
				legacy_fuel_stock * kLegacyFuelGasolineShare;
			city.markets[legacy_diesel].stock +=
				legacy_fuel_stock * kLegacyFuelDieselShare;
			city.markets[legacy_jet].stock +=
				legacy_fuel_stock * kLegacyFuelJetShare;
		}
		ws.cities.push_back(city);
	}

	ws.buildings.clear();
	Array buildings = data.get("buildings", Array());
	for (int index = 0; index < buildings.size(); ++index) {
		Dictionary entry = buildings[index];
		Building building;
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
			const int legacy_output = legacy_substitute(building.output_product);
			building.input_product = migrate_index(building.input_product);
			building.output_product = migrate_index(building.output_product);
			if (building.output_product < 0 && legacy_output >= 0) {
				// F0: esta construcao produzia o `fuel` generico; passa a
				// produzir gasoline (a refinaria entrega diesel e querosene
				// por co-producao).
				building.output_product = legacy_output;
			}
			if (building.output_product < 0) {
				// Produto removido da cadeia (farinha/pão ou body_armor):
				// construção descartada.
				continue;
			}
		}
		building.capacity = double(entry.get("capacity", 0.0));
		building.input_per_output = double(entry.get("input_per_output", 0.0));
		// Receita industrial: o id é resolvido na tabela carregada por
		// set_recipes (v4). Save v3 não tem a chave -> produtor legado.
		const std::string building_recipe_id =
			std::string(String(entry.get("recipe", "")).utf8().get_data());
		building.recipe = building_recipe_id.empty()
			? -1 : core.recipe_index(building_recipe_id);
		if (building.recipe < 0) {
			// F0: receita do combustivel generico (fuel_a..fuel_d) de um save
			// antigo -> a receita que ocupa o lugar hoje.
			const char* alias = legacy_recipe_alias(building_recipe_id);
			if (alias != nullptr) {
				building.recipe = core.recipe_index(std::string(alias));
			}
		}
		building.capital_per_capacity = std::max(1.0, safe_nonneg(
			double(entry.get("capital_per_capacity", kCapitalPerCapacity))));
		building.jobs = double(entry.get("jobs", 0.0));
		building.wage = double(entry.get("wage", 0.0));
		building.opex = double(entry.get("opex", 0.0));
		// F7: mistura de escolaridade. Save v8 sem a chave: usa a da receita
		// resolvida; primária segue o padrão do struct (quase só básico).
		if (entry.has("education_mix")) {
			const Array mix = entry.get("education_mix", Array());
			double total = 0.0;
			for (int level = 0;
					level < kEducationLevels && level < mix.size(); ++level) {
				building.education_mix[level] = std::max(0.0, double(mix[level]));
				total += building.education_mix[level];
			}
			if (total > 1e-9) {
				for (int level = 0; level < kEducationLevels; ++level) {
					building.education_mix[level] /= total;
				}
			}
		} else if (building.recipe >= 0
				&& building.recipe < static_cast<int>(ws.recipes.size())) {
			const Recipe& recipe =
				ws.recipes[static_cast<std::size_t>(building.recipe)];
			for (int level = 0; level < kEducationLevels; ++level) {
				building.education_mix[level] = recipe.education_mix[level];
			}
		}
		building.output_scale = double(entry.get("output_scale", 1.0));
		building.last_utilization = double(entry.get("last_utilization", 1.0));
		building.ramp_days = std::max(0, int(entry.get("ramp_days", 0)));
		building.strategic = bool(entry.get("strategic", false));
		building.loss_days = int(entry.get("loss_days", 0));
		building.bankrupt = bool(entry.get("bankrupt", false));
		// D-125 (save v40): eficiência de produção. Save v39 e anteriores não
		// têm as chaves: a planta é estoque PRÉ-EXISTENTE do ano-base, então
		// carrega MADURA (eficiência 1, trator/máquina ligados) — nada
		// inventado para trás e o comportamento anterior preservado.
		building.production_efficiency = std::clamp(
			version >= 40 ? double(entry.get("production_efficiency", 1.0)) : 1.0,
			0.0, 1.0);
		building.efficiency_days = version >= 40
			? std::max(0, int(entry.get("efficiency_days", 9999))) : 9999;
		building.transition_days = version >= 40
			? std::max(0, int(entry.get("transition_days", 0))) : 0;
		building.equipment = std::clamp(version >= 40
			? double(entry.get("equipment", 0.0)) : 1.0, 0.0, 1.0);
		building.refinery_mode = version >= 40
			? std::clamp(int(entry.get("refinery_mode", 0)), 0, 2) : 0;
		// D-126 (save v41): sobrevivência em crise. Save v40 e anteriores
		// carrega neutro (sem hibernação, sem demissão/liquidação em curso).
		if (version >= 41) {
			building.crisis_stage = std::clamp(
				int(entry.get("crisis_stage", 0)), 0, 6);
			building.last_layoff_day = std::max(-1000000,
				int(entry.get("last_layoff_day", -1000000)));
			building.last_credit_day = std::max(-1000000,
				int(entry.get("last_credit_day", -1000000)));
			building.last_liquidation_day = std::max(-1000000,
				int(entry.get("last_liquidation_day", -1000000)));
			building.mothballed = bool(entry.get("mothballed", false));
			building.mothball_day = int(entry.get("mothball_day", -1));
			building.mothball_unpaid_days = std::max(0,
				int(entry.get("mothball_unpaid_days", 0)));
			building.reopen_signal_days = std::max(0,
				int(entry.get("reopen_signal_days", 0)));
			building.farm_area_reduced = bool(
				entry.get("farm_area_reduced", false));
		}
		building.idle_today = false;
		building.equipment_active = building.equipment > 0.0;
		// D-086 (decisão do dono): o recurso nunca acaba — ignora o `depleted`
		// salvo (que antes marcava a jazida esgotada) e força `false`. O formato
		// e a versão (v18) NÃO mudam.
		building.depleted = false;
		// D-086 (save v18): estágio de mina. Save v17 e anteriores (sem `stage`)
		// entra no ÚLTIMO estágio com o estágio esgotado: é a mina histórica que
		// produz quase nada e só volta pagando o aprofundamento.
		if (entry.has("stage")) {
			building.stage = std::max(0, int(entry.get("stage", 0)));
			building.stage_reserve_kt = std::max(0.0,
				double(entry.get("stage_reserve_kt", 0.0)));
			building.stage_mined_kt = std::clamp(
				double(entry.get("stage_mined_kt", 0.0)), 0.0,
				building.stage_reserve_kt);
		} else {
			building.stage = kMineInitialStage;
			building.stage_reserve_kt = 0.0;
			building.stage_mined_kt = 0.0;
		}
		// B5 (save v21): royalty/imposto de concessão. Save v20 e anteriores
		// não têm a chave: 0 (sem concessão — nada inventado para trás).
		building.royalty = std::clamp(
			double(entry.get("royalty", 0.0)), 0.0, 1.0);
		// C1 (save v26): salário atrasado da planta. v25 e anteriores não têm
		// a chave — nasce 0 (nada inventado para trás).
		building.wage_arrears = version >= 26
			? std::max(0.0, double(entry.get("wage_arrears", 0.0))) : 0.0;
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
		ConstructionProject project;
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
		// Save v14: kind explícito na fila única (0 prédio, 1 moradia, 2 infra).
		// Save v13 e anteriores não têm a chave: a derivação por `urban_action`
		// mantém a obra urbana como infra (2) e o resto como prédio (0).
		project.kind = std::clamp(
			int(entry.get("kind", project.urban_action != 0 ? 2 : 0)), 0,
			static_cast<int>(CONSTRUCTION_POWER_LINE));
		// Moradia (§19 W3): densidade, padrão, dono e unidades da obra, mais a
		// parcela de terra já paga à prefeitura na abertura.
		project.housing_density = std::clamp(
			int(entry.get("housing_density", 0)), 0, DENSITY_COUNT - 1);
		project.housing_tier = std::clamp(
			int(entry.get("housing_tier", 0)), 0, TIER_COUNT - 1);
		project.housing_owner = std::clamp(
			int(entry.get("housing_owner", 0)), 0,
			static_cast<int>(HOUSING_PROJECT_GOVERNMENT));
		project.housing_units = safe_nonneg(
			double(entry.get("housing_units", 0.0)));
		project.land_cost = safe_nonneg(double(entry.get("land_cost", 0.0)));
		// Obra urbana e moradia não têm produto: a migração de índice de produto
		// abaixo não se aplica a elas.
		const bool urban_project = project.urban_action != 0
			|| project.kind == CONSTRUCTION_HOUSING;
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
			const int legacy_output = legacy_substitute(project.output_product);
			project.output_product = migrate_index(project.output_product);
			if (project.output_product < 0 && legacy_output >= 0) {
				// F0: obra do `fuel` generico -> gasoline (ver save v39).
				project.output_product = legacy_output;
			}
			if (project.output_product < 0) {
				continue;  // produto removido: obra descartada
			}
		}
		const std::string project_recipe_id =
			std::string(String(entry.get("recipe", "")).utf8().get_data());
		project.recipe = project_recipe_id.empty()
			? -1 : core.recipe_index(project_recipe_id);
		if (project.recipe < 0) {
			const char* alias = legacy_recipe_alias(project_recipe_id);
			if (alias != nullptr) {
				project.recipe = core.recipe_index(std::string(alias));
			}
		}
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
		if (mix.size() >= kEducationLevels) {
			double total = 0.0;
			for (int level = 0; level < kEducationLevels; ++level) {
				project.education_mix[level] = std::max(0.0, double(mix[level]));
				total += project.education_mix[level];
			}
			if (total > 1e-9) {
				for (int level = 0; level < kEducationLevels; ++level) {
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
		// D-086: obra de aprofundamento de mina. Save v17 e anteriores sem a chave: -1.
		project.deepen_stage = std::max(-1, int(entry.get("deepen_stage", -1)));
		// B5: tipo de infra da região. Save v20 e anteriores sem a chave: -1.
		project.infra_type = std::max(-1,
			std::min(int(entry.get("infra_type", -1)), kInfraTypeCount - 1));
	// G2 (v25): obra de armazém. Save v24 e anteriores não têm as chaves:
	// -1 = não é armazém (nada inventado para trás).
	project.storage_type = std::max(-1,
		std::min(int(entry.get("storage_type", -1)), kStorageTypeCount - 1));
	project.storage_capacity_t = safe_nonneg(
		double(entry.get("storage_capacity_t", 0.0)));
	// PARTE E (v35): obra de energia. Save v34 e anteriores não têm as chaves:
	// -1/0 = não é obra de energia (nada inventado para trás).
	project.power_fuel = std::max(-1,
		std::min(int(entry.get("power_fuel", -1)), kPowerFuelCount - 1));
	project.power_mw = safe_nonneg(double(entry.get("power_mw", 0.0)));
	project.power_nation_a = std::string(
		String(entry.get("power_nation_a", "")).utf8().get_data());
	project.power_nation_b = std::string(
		String(entry.get("power_nation_b", "")).utf8().get_data());
		const Array materials = entry.get("materials", Array());
		const Array material_qty = entry.get("material_qty", Array());
		const Array material_delivered = entry.get("material_delivered", Array());
		project.material_count = std::min(
			static_cast<int>(materials.size()), kMaxConstructionMaterials);
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
}

void read_services(WorldState& ws, SimCore& core, const Dictionary& data) {
	ws.service_businesses.clear();
	ws.next_service_business_id = 1;
	ws.service_bankruptcies = int(data.get("service_bankruptcies", 0));
	const Array saved_businesses = data.get("service_businesses", Array());
	for (int index = 0; index < saved_businesses.size(); ++index) {
		if (saved_businesses[index].get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary entry = saved_businesses[index];
		ServiceBusiness business;
		business.id = int(entry.get("id", 0));
		business.sector = int(entry.get("sector", -1));
		business.city = std::string(
			String(entry.get("city", "")).utf8().get_data());
		business.owner_stratum = std::clamp(
			int(entry.get("owner_stratum", kEducationLevels - 1)),
			0, kEducationLevels - 1);
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
		core.seed_services();
	} else {
		core.rebuild_service_indexes();
	}
}

void read_urban(WorldState& ws, const Dictionary& data) {
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
			// Q-015-C: fração do envelope construída. Save anterior ao campo
			// (ou cenário sintético) = 1,0: comportamento anterior, sem
			// recalibrar capacidade de mundo já em andamento.
			const Array zone_fill = entry.get("zone_fill", Array());
			for (int zone = 0; zone < ZONE_COUNT; ++zone) {
				const double fill = zone < zone_fill.size()
					? safe_nonneg(double(zone_fill[zone])) : 1.0;
				land.zone_fill[zone] = fill > 0.0 ? fill : 1.0;
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
}

void read_city_gov(WorldState& ws, const Dictionary& data) {
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
			// A6: janela do fechamento mensal (save v14 não tem: 0 = primeira
			// janela, que a fase escala pelos dias decorridos desde o dia 0).
			gov.month_marker = std::max(
				0, int(entry.get("month_marker", 0)));
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
}

void read_housing(WorldState& ws, const Dictionary& data, int version) {
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
			// save v14: a obra de moradia vive em `construction_projects`
			// (kind CONSTRUCTION_HOUSING). Save v13 e anteriores guardavam a
			// lista em `housing[].projects`: cada obra antiga é MIGRADA para a
			// fila única com o prazo RESTANTE e custo 0 — o capital já foi pago
			// na versão antiga, então a obra só cumpre o prazo, sem consumir
			// caixa nem capacidade (total_out = 0). Ordem determinística: índice
			// da entrada `housing` e depois o slot.
			if (version < 14) {
				const Array projects = entry.get("projects", Array());
				const int max_slots = std::min(
					static_cast<int>(projects.size()), kHousingMaxProjects);
				for (int slot = 0; slot < max_slots; ++slot) {
					if (projects[slot].get_type() != Variant::DICTIONARY) {
						continue;
					}
					const Dictionary p = projects[slot];
					const int state = std::clamp(
						int(p.get("state", 0)), 0,
						static_cast<int>(HOUSING_PROJECT_GOVERNMENT));
					if (state == HOUSING_PROJECT_IDLE) {
						continue;  // slot vazio no save antigo
					}
					ConstructionProject project;
					project.id = "HSG-V13-" + city_id + "-"
						+ std::to_string(slot);
					project.building_id = project.id;
					project.city = city_id;
					project.owner = state == HOUSING_PROJECT_GOVERNMENT
						? "state" : "private";
					project.output_product = -1;
					project.recipe = -1;
					project.input_product = -1;
					project.kind = CONSTRUCTION_HOUSING;
					project.housing_owner = state;
					project.housing_density = std::clamp(
						int(p.get("density", 0)), 0, DENSITY_COUNT - 1);
					project.housing_tier = std::clamp(
						int(p.get("tier", 0)), 0, TIER_COUNT - 1);
					project.housing_units = safe_nonneg(
						double(p.get("units", 0.0)));
					project.capacity = project.housing_units;
					project.total_cost = 0.0;
					project.reserved = 0.0;
					project.labor_cost_total = 0.0;
					project.material_count = 0;
					project.duration_days = std::max(
						1, int(p.get("days_left", 0)));
					project.days_elapsed = 0;
					project.created_day = ws.day;
					project.paused = false;
					ws.construction_projects.push_back(std::move(project));
				}
			}
		}
	}
}

} // namespace city

} // namespace ogs::save
