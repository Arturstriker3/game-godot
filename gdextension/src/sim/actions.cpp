// A5 — ações do mercado de construção (investir, construir, construir por grupo).
// SimCore/fachada: lógica movida de economy_sim.cpp — realocação byte-idêntica.
// C++ puro (STL): nada de godot_cpp aqui.
#include "actions.h"

#include "sim_constants.h"
#include "sim_core.h"
#include "urban_land.h"

#include <algorithm>
#include <cstddef>
#include <string>

namespace ogs {

Building* find_building(WorldState& ws, const std::string& city_id,
		const std::string& id) {
	for (Building& building : ws.buildings) {
		if (building.city == city_id && building.id == id) {
			return &building;
		}
	}
	return nullptr;
}

// §15: custo ainda não pago das obras de uma cidade para um dono ("state",
// "private" ou o id de um grupo). Base do limite de comprometimento: o dono não
// inicia obra nova cujo custo, somado ao já comprometido, estoura o caixa atual.
double committed_construction(const WorldState& ws, const std::string& city_id,
		const std::string& owner) {
	double committed = 0.0;
	for (const ConstructionProject& project : ws.construction_projects) {
		if (project.city != city_id) {
			continue;
		}
		const bool match = owner == "state" ? project.owner == "state"
			: owner == "private" ? project.owner == "private"
			: !project.group_id.empty() && project.group_id == owner;
		if (match) {
			committed += std::max(0.0, project.total_cost - project.paid);
		}
	}
	return committed;
}

InvestResult action_invest(WorldState& ws, SimCore& core,
		const std::string& city_id, const std::string& building_id,
		double amount, bool instant_construction) {
	InvestResult result;
	result.invested = 0.0;
	result.capacity_added = 0.0;
	result.error = "";

	City* city = core.find_city(city_id);
	if (city == nullptr) {
		result.error = "cidade desconhecida";
		return result;
	}
	Building* building = find_building(ws, city->id, building_id);
	if (building == nullptr) {
		result.error = "construção desconhecida";
		return result;
	}
	if (amount <= 0.0) {
		result.error = "valor inválido";
		return result;
	}
	if (city->firm_cash < amount) {
		result.error = "capital insuficiente";
		return result;
	}
	if (building->input_product < 0
			&& city->suitability[building->output_product] <= 0.0) {
		result.error = "sem aptidão territorial";
		return result;
	}
	if (building->input_product < 0) {
		const Deposit& deposit = city->deposits[building->output_product];
		if (deposit.tracked && deposit.remaining_kt <= 0.0) {
			result.error = "reserva esgotada";
			return result;
		}
	}

	const double capacity_added = amount / std::max(1.0, building->capital_per_capacity);

	if (instant_construction) {
		city->firm_cash -= amount;
		// A expansão é construída por serviços domésticos: o dinheiro vira
		// receita de construção (contada no PIB do próximo tick) e não
		// desaparece em um caixa inerte.
		city->service_cash += amount;
		city->construction_revenue_pending += amount;
		building->capacity += capacity_added;
		result.invested = amount;
		result.capacity_added = capacity_added;
		return result;
	}

	// §15: expansão vira projeto físico (mesma regra de obra nova). O dono
	// compromete o custo (limite do surto) e paga conforme a obra avança; a
	// capacidade entra ao concluir. Anti-duplicação: uma expansão em andamento
	// por prédio (a IA expandiria o mesmo prédio todo dia durante a obra).
	for (const ConstructionProject& in_flight : ws.construction_projects) {
		if (in_flight.expand_existing
				&& in_flight.building_id == building->id
				&& in_flight.city == city->id) {
			result.error = "obra em andamento";
			return result;
		}
	}
	const double committed =
		committed_construction(ws, city->id, "private");
	if (committed + amount > city->firm_cash) {
		result.error = "capital insuficiente";
		return result;
	}

	ConstructionProject project;
	project.id = building->id + "-EXP";
	project.building_id = building->id;
	project.city = city->id;
	project.owner = "private";
	project.resource = building->resource;
	project.input_product = building->input_product;
	project.output_product = building->output_product;
	project.recipe = building->recipe;
	project.capacity = capacity_added;
	project.input_per_output = building->input_per_output;
	for (int level = 0; level < kEducationLevels; ++level) {
		project.education_mix[level] = building->education_mix[level];
	}
	project.jobs = building->jobs * (capacity_added / std::max(1e-9, building->capacity));
	project.wage = building->wage;
	project.opex = building->opex * (capacity_added / std::max(1e-9, building->capacity));
	project.expand_existing = true;
	project.total_cost = amount;
	// §15 calibração: reserva o capital da obra no início (investimento).
	project.reserved = amount;
	city->firm_cash -= amount;
	core.add_construction_project(project);
	result.invested = amount;
	result.capacity_added = capacity_added;
	result.project_id = project.id;
	return result;
}

BuildResult action_build(WorldState& ws, SimCore& core,
		const std::string& city_id, const std::string& building_id,
		const std::string& resource, int input_product, int output_product,
		double capacity, double jobs, double wage, double opex,
		double input_per_output, const std::string& owner, double cost,
		const std::string& recipe_id, bool instant_construction,
		bool bootstrap_construction) {
	BuildResult result;
	result.created = false;
	result.cost = 0.0;
	result.error = "";

	City* city = core.find_city(city_id);
	if (city == nullptr) {
		result.error = "cidade desconhecida";
		return result;
	}
	const std::string id(building_id);
	const std::string owner_name(owner);
	if (id.empty()) {
		result.error = "identificador vazio";
		return result;
	}
	if (find_building(ws, city->id, id) != nullptr) {
		result.error = "construção já existe";
		return result;
	}
	if (output_product < 0 || output_product >= ws.products) {
		result.error = "produto inválido";
		return result;
	}
	if (input_product < -1 || input_product >= ws.products) {
		result.error = "insumo inválido";
		return result;
	}
	int recipe = -1;
	if (!recipe_id.empty()) {
		recipe = core.recipe_index(recipe_id);
		if (recipe < 0) {
			result.error = "receita desconhecida";
			return result;
		}
		const Recipe& chosen = ws.recipes[static_cast<std::size_t>(recipe)];
		if (chosen.output != output_product) {
			result.error = "receita não produz o bem";
			return result;
		}
		// O primeiro insumo da receita alimenta os campos legados de exibição
		// (input_product/input_per_output); a fase 4 usa a receita completa.
		input_product = chosen.inputs[0];
		input_per_output = chosen.input_qty[0] / chosen.yield;
		if (jobs <= 0.0) {
			jobs = capacity * chosen.jobs_per_ton;
		}
		if (wage <= 0.0) {
			wage = chosen.wage;
		}
		if (opex <= 0.0) {
			opex = capacity * chosen.opex_per_ton;
		}
	}
	double education_mix[kEducationLevels] = {0.90, 0.09, 0.01};
	if (recipe >= 0) {
		const Recipe& chosen = ws.recipes[static_cast<std::size_t>(recipe)];
		for (int level = 0; level < kEducationLevels; ++level) {
			education_mix[level] = chosen.education_mix[level];
		}
	} else if (input_product >= 0) {
		// Fábrica legada (sem receita): mistura industrial padrão.
		education_mix[0] = 0.60;
		education_mix[1] = 0.30;
		education_mix[2] = 0.10;
	}
	if (capacity <= 0.0 || jobs < 0.0 || wage < 0.0 || opex < 0.0 || cost < 0.0) {
		result.error = "parâmetros inválidos";
		return result;
	}
	if (recipe < 0 && input_product < 0
			&& city->suitability[output_product] <= 0.0) {
		result.error = "sem aptidão territorial";
		return result;
	}
	if (recipe < 0 && input_product < 0) {
		const Deposit& deposit = city->deposits[output_product];
		if (deposit.tracked && deposit.remaining_kt <= 0.0) {
			result.error = "reserva esgotada";
			return result;
		}
	}
	const bool state_owned = owner_name == "state";

	if (instant_construction || bootstrap_construction) {
		// Hook de teste (construção instantânea): comportamento anterior —
		// prédio pronto na hora e custo integral debitado do dono.
		if (state_owned) {
			if (city->government_budget < cost) {
				result.error = "orçamento insuficiente";
				return result;
			}
			city->government_budget -= cost;
		} else {
			if (city->firm_cash < cost) {
				result.error = "capital insuficiente";
				return result;
			}
			city->firm_cash -= cost;
		}

		Building building;
		building.id = id;
		building.city = city->id;
		building.kind = 0;  // tipo (0=fazenda); o produto fica em output_product
		building.resource = resource;
		building.owner = state_owned ? "state" : "private";
		building.input_product = input_product;
		building.output_product = output_product;
		building.recipe = recipe;
		building.capacity = capacity;
		building.input_per_output = input_per_output;
		for (int level = 0; level < kEducationLevels; ++level) {
			building.education_mix[level] = education_mix[level];
		}
		building.capital_per_capacity = capacity > 0.0
			? std::max(1.0, cost / capacity) : kCapitalPerCapacity;
		building.jobs = jobs;
		building.wage = wage;
		building.opex = opex;
		// Fábrica nova começa em rampa (25% da capacidade): evita o pico de
		// produção/oferta no primeiro dia e deixa a curva de preço reagir.
		if (recipe >= 0 || input_product >= 0) {
			building.output_scale = 0.25;
			building.last_utilization = 0.25;
		}
		ws.buildings.push_back(building);
		// D-083: mina nova abre o estágio 0 reivindicando uma fatia da reserva da
		// jazida (produção cheia no primeiro estágio). Produto sem jazida
		// declarada não tem estágio e mantém a extração ilimitada legada.
		if (SimCore::is_mine_building(*city, ws.buildings.back())) {
			SimCore::set_mine_stage(*city, ws.buildings.back(), 0);
		}
		// Índice por cidade: insere SÓ a construção nova. O tick reconstrói o
		// índice no início, após remover falidas.
		const std::size_t city_index =
			static_cast<std::size_t>(city - ws.cities.data());
		if (city_index < ws.buildings_by_city.size()) {
			ws.buildings_by_city[city_index].push_back(
				static_cast<int>(ws.buildings.size() - 1));
		} else {
			core.rebuild_indexes();
		}

		// O investimento é pago ao setor de construção doméstico e entra no PIB
		// do próximo tick como receita de construção.
		city->service_cash += cost;
		city->construction_revenue_pending += cost;
		result.created = true;
		result.cost = cost;
		return result;
	}

	// §19 (W1): nova fábrica/planta URBANA exige espaço na zona INDUSTRIAL.
	// Minas e fazendas (extração primária) ficam fora da área urbana e não
	// consomem zona. Sem espaço a decisão NÃO falha: fica pendente (a IA tenta
	// de novo) e sinaliza demanda de zona à prefeitura (W2). O hook de
	// construção instantânea acima é caminho de teste e não passa pelo limite.
	const bool urban_factory = recipe >= 0 || input_product >= 0;
	if (urban_factory && !ws.land.empty() && ws.land.size() == ws.cities.size()) {
		const std::size_t urban_index =
			static_cast<std::size_t>(city - ws.cities.data());
		// A zona industrial é medida em TRABALHADORES (m² por trabalhador): a
		// planta nova ocupa os empregos que ela vai criar.
		if (!urban_land::zone_has_room(ws, urban_index, ZONE_IND,
				std::max(1.0, jobs))) {
			urban_land::signal_zone_demand(ws.land[urban_index], ZONE_IND);
			result.zone_pending = true;
			result.error = "sem espaço na zona industrial";
			return result;
		}
	}

	// §15: obra vira PROJETO físico. O dono COMPROMETE o custo (limite do surto
	// de obras: o total comprometido não pode estourar o caixa atual), mas paga
	// conforme a obra avança; sem material/sem dinheiro a obra PARA (não falha).
	// Anti-duplicação: não abre obra nova do MESMO produto enquanto outra está
	// em andamento na cidade (a IA não enxerga obra em voo no status — sem o
	// guarda ela empilhava uma mina/fazenda por dia e, ao concluírem, a
	// sobreoferta falia a coorte inteira).
	for (const ConstructionProject& in_flight : ws.construction_projects) {
		if (!in_flight.expand_existing && in_flight.city == city->id
				&& in_flight.output_product == output_product) {
			result.error = "obra em andamento";
			return result;
		}
	}
	const std::string commit_owner = state_owned ? "state" : "private";
	const double committed = committed_construction(ws, city->id, commit_owner);
	const double commit_budget = state_owned ? city->government_budget
		: city->firm_cash;
	if (committed + cost > commit_budget) {
		result.error = state_owned ? "orçamento insuficiente"
			: "capital insuficiente";
		return result;
	}

	ConstructionProject project;
	project.id = id;
	project.building_id = id;
	project.city = city->id;
	project.owner = state_owned ? "state" : "private";
	project.resource = resource;
	project.input_product = input_product;
	project.output_product = output_product;
	project.recipe = recipe;
	project.capacity = capacity;
	project.input_per_output = input_per_output;
	for (int level = 0; level < kEducationLevels; ++level) {
		project.education_mix[level] = education_mix[level];
	}
	project.jobs = jobs;
	project.wage = wage;
	project.opex = opex;
	project.total_cost = cost;
	// §15 calibração: reserva o capital da obra no início (investimento, não
	// caixa de giro). O dono debita o custo total e a obra paga material/serviço
	// do fundo reservado — não pausa por liquidez temporária (a folha/opex da
	// fazenda deficitária não consome o capital já comprometido com a obra).
	// O `reserved` entra em money_total, então a conservação vale.
	project.reserved = cost;
	if (state_owned) {
		city->government_budget -= cost;
	} else {
		city->firm_cash -= cost;
	}
	core.add_construction_project(project);
	result.created = true;
	result.cost = cost;
	result.project_id = id;
	return result;
}

GroupInvestResult action_group_invest(WorldState& ws, SimCore& core,
		const std::string& group_id, const std::string& city_id,
		const std::string& building_id, const std::string& resource,
		int output_product, double capacity, const std::string& recipe_id,
		const std::string& partner_group, bool instant_construction) {
	GroupInvestResult result;
	result.created = false;
	result.cost = 0.0;
	result.error = "";
	const std::string gid(group_id);
	Group* group = core.find_group(gid);
	if (group == nullptr) {
		result.error = "grupo desconhecido";
		return result;
	}
	City* city = core.find_city(city_id);
	if (city == nullptr) {
		result.error = "cidade desconhecida";
		return result;
	}
	const std::string id(building_id);
	if (id.empty()) {
		result.error = "identificador vazio";
		return result;
	}
	if (find_building(ws, city->id, id) != nullptr) {
		result.error = "construção já existe";
		return result;
	}
	if (output_product < 0 || output_product >= ws.products) {
		result.error = "produto inválido";
		return result;
	}
	if (capacity <= 0.0) {
		result.error = "capacidade inválida";
		return result;
	}
	const std::string sector = output_product < static_cast<int>(ws.product_sectors.size())
		? ws.product_sectors[static_cast<std::size_t>(output_product)]
		: std::string();
	int recipe = -1;
	double capital_per_capacity = kCapitalPerCapacity;
	double jobs = 0.0;
	double wage = 0.0;
	double opex = 0.0;
	double education_mix[kEducationLevels] = {0.90, 0.09, 0.01};
	if (!recipe_id.empty()) {
		recipe = core.recipe_index(recipe_id);
		if (recipe < 0) {
			result.error = "receita desconhecida";
			return result;
		}
		const Recipe& chosen = ws.recipes[static_cast<std::size_t>(recipe)];
		if (chosen.output != output_product) {
			result.error = "receita não produz o bem";
			return result;
		}
		capital_per_capacity = std::max(1.0, chosen.capital_per_capacity);
		jobs = capacity * chosen.jobs_per_ton;
		wage = chosen.wage;
		opex = capacity * chosen.opex_per_ton;
		for (int level = 0; level < kEducationLevels; ++level) {
			education_mix[level] = chosen.education_mix[level];
		}
	} else if (city->suitability[output_product] <= 0.0) {
		result.error = "sem aptidão territorial";
		return result;
	} else {
		// Produção primária por grupo: mesmos coeficientes da fazenda inicial
		// (estimativa declarada; o núcleo não tem tabela por produto). Sem
		// folha/opex o lucro ficaria inflado e o emprego zerado.
		jobs = capacity * kJobsPerTon;
		wage = kFarmWage;
		opex = capacity * kOpexPerTon;
	}
	const bool foreign = group->origin_nation != city->nation;
	double local_share = 0.0;
	std::string partner_id;
	Group* partner = nullptr;
	if (foreign) {
		const int mode = core.fdi_mode(city->nation, sector);
		if (mode == 3) {
			result.error = "setor fechado ao capital estrangeiro";
			return result;
		}
		if (core.nation_confidence(city->nation) < 0.5) {
			result.error = "confiança baixa após nacionalização";
			return result;
		}
		if (mode == 2) {
			partner_id = partner_group;
			if (partner_id.empty()) {
				result.error = "joint venture exige parceiro local";
				return result;
			}
			partner = core.find_group(partner_id);
			if (partner == nullptr || partner->origin_nation != city->nation
					|| partner->state_owned) {
				result.error = "parceiro local inválido";
				return result;
			}
			const auto policy = ws.fdi_lookup.find(city->nation + "|" + sector);
			local_share = policy == ws.fdi_lookup.end()
				? 0.30 : std::clamp(
					ws.fdi_policies[static_cast<std::size_t>(policy->second)].min_local,
					0.0, 1.0);
			if (local_share <= 0.0) {
				local_share = 0.30;
			}
		} else if (mode == 1) {
			// Restrito: só quem já opera no setor do anfitrião.
			bool incumbent = false;
			for (const Building& building : ws.buildings) {
				if (building.owner.rfind("foreign:", 0) != 0
						|| building.output_product != output_product) {
					continue;
				}
				std::string owner_group;
				std::string origin;
				core.owner_kind(building.owner, &owner_group, &origin);
				const City* host = core.find_city(building.city);
				if (host != nullptr && host->nation == city->nation
						&& owner_group == gid) {
					incumbent = true;
					break;
				}
			}
			if (!incumbent) {
				result.error = "setor restrito a incumbentes";
				return result;
			}
		}
		if (recipe < 0) {
			const Deposit& deposit = city->deposits[output_product];
			if (deposit.tracked && deposit.remaining_kt <= 0.0) {
				result.error = "reserva esgotada";
				return result;
			}
		}
	}
	const double cost = capacity * capital_per_capacity;
	const double partner_cost = cost * local_share;
	const double owner_cost = cost - partner_cost;

	if (instant_construction) {
		if (group->cash + 1e-9 < owner_cost) {
			result.error = "capital insuficiente";
			result.cash = group->cash;
			result.cost = cost;
			return result;
		}
		if (partner != nullptr && partner->cash + 1e-9 < partner_cost) {
			result.error = "capital insuficiente do parceiro local";
			result.cost = cost;
			return result;
		}
		group->cash -= owner_cost;
		group->capex_total += owner_cost;
		if (partner != nullptr && partner_cost > 0.0) {
			partner->cash -= partner_cost;
			partner->capex_total += partner_cost;
		}

		Building building;
		building.id = id;
		building.city = city->id;
		building.kind = 0;
		building.resource = resource;
		building.owner = foreign
			? "foreign:" + gid + "@" + group->origin_nation
			: "national:" + gid;
		building.input_product = recipe >= 0
			? ws.recipes[static_cast<std::size_t>(recipe)].inputs[0] : -1;
		building.output_product = output_product;
		building.recipe = recipe;
		building.capacity = capacity;
		building.input_per_output = recipe >= 0
			? ws.recipes[static_cast<std::size_t>(recipe)].input_qty[0]
				/ std::max(1e-9,
					ws.recipes[static_cast<std::size_t>(recipe)].yield)
			: 0.0;
		building.capital_per_capacity = capital_per_capacity;
		building.jobs = jobs;
		building.wage = wage;
		building.opex = opex;
		for (int level = 0; level < kEducationLevels; ++level) {
			building.education_mix[level] = education_mix[level];
		}
		building.local_share = local_share;
		building.partner_group = partner != nullptr ? partner->id : std::string();
		if (recipe >= 0) {
			building.output_scale = 0.25;
			building.last_utilization = 0.25;
		}
		ws.buildings.push_back(building);
		// D-083: mina nova abre o estágio 0 (mesma regra do build do Estado).
		if (SimCore::is_mine_building(*city, ws.buildings.back())) {
			SimCore::set_mine_stage(*city, ws.buildings.back(), 0);
		}
		const std::size_t city_index =
			static_cast<std::size_t>(city - ws.cities.data());
		if (city_index < ws.buildings_by_city.size()) {
			ws.buildings_by_city[city_index].push_back(
				static_cast<int>(ws.buildings.size() - 1));
		} else {
			core.rebuild_indexes();
		}
		city->service_cash += cost;
		city->construction_revenue_pending += cost;
		result.created = true;
		result.cost = cost;
		result.owner = building.owner;
		result.local_share = local_share;
		return result;
	}

	// §15: obra vira projeto físico. O grupo financia a sua fatia conforme a
	// obra avança; o parceiro de JV paga a fatia local na hora (contribuição
	// adiantada ao serviço de construção). Sem material/sem dinheiro, a obra
	// PARA.
	// Anti-duplicação: não abre obra nova do MESMO produto enquanto outra está
	// em andamento na cidade (mesma regra do build do governo).
	for (const ConstructionProject& in_flight : ws.construction_projects) {
		if (!in_flight.expand_existing && in_flight.city == city->id
				&& in_flight.output_product == output_product) {
			result.error = "obra em andamento";
			return result;
		}
	}
	const double committed = committed_construction(ws, city->id, gid);
	if (committed + owner_cost > group->cash) {
		result.error = "capital insuficiente";
		result.cash = group->cash;
		result.cost = cost;
		return result;
	}
	if (partner != nullptr) {
		const double pcommitted =
			committed_construction(ws, city->id, partner->id);
		if (pcommitted + partner_cost > partner->cash) {
			result.error = "capital insuficiente do parceiro local";
			result.cost = cost;
			return result;
		}
		partner->cash -= partner_cost;
		partner->capex_total += partner_cost;
		city->service_cash += partner_cost;
		city->construction_revenue_pending += partner_cost;
	}
	group->capex_total += owner_cost;

	ConstructionProject project;
	project.id = id;
	project.building_id = id;
	project.city = city->id;
	project.owner = foreign
		? "foreign:" + gid + "@" + group->origin_nation
		: "national:" + gid;
	project.group_id = gid;
	project.resource = resource;
	project.input_product = recipe >= 0
		? ws.recipes[static_cast<std::size_t>(recipe)].inputs[0] : -1;
	project.output_product = output_product;
	project.recipe = recipe;
	project.capacity = capacity;
	project.input_per_output = recipe >= 0
		? ws.recipes[static_cast<std::size_t>(recipe)].input_qty[0]
			/ std::max(1e-9,
				ws.recipes[static_cast<std::size_t>(recipe)].yield)
		: 0.0;
	project.jobs = jobs;
	project.wage = wage;
	project.opex = opex;
	for (int level = 0; level < kEducationLevels; ++level) {
		project.education_mix[level] = education_mix[level];
	}
	project.local_share = local_share;
	project.partner_group = partner != nullptr ? partner->id : std::string();
	project.total_cost = owner_cost;
	// §15 calibração: reserva o capital da obra no início (investimento).
	project.reserved = owner_cost;
	group->cash -= owner_cost;
	core.add_construction_project(project);
	result.created = true;
	result.cost = cost;
	result.owner = project.owner;
	result.local_share = local_share;
	result.project_id = id;
	return result;
}

namespace {
// D-083: caixa de quem paga o aprofundamento, pelo dono da MINA —
// Estado → orçamento da cidade; grupo F9 → caixa do grupo; "private" (legado)
// → caixa da firma. `commit_key` é a chave do limite de comprometimento de obra
// (`committed_construction`). nullptr = dono sem caixa localizável.
double* mine_owner_cash(SimCore& core, City& city, const Building& building,
		std::string& commit_key) {
	std::string group_id;
	std::string origin_nation;
	const int kind = core.owner_kind(building.owner, &group_id, &origin_nation);
	if (kind == 0) {
		commit_key = "state";
		return &city.government_budget;
	}
	if (kind == 1 || kind == 2) {
		commit_key = group_id;
		Group* group = core.find_group(group_id);
		return group == nullptr ? nullptr : &group->cash;
	}
	commit_key = "private";
	return &city.firm_cash;
}
} // namespace

DeepenResult action_deepen_mine(WorldState& ws, SimCore& core,
		const std::string& city_id, const std::string& building_id,
		bool instant_construction) {
	DeepenResult result;
	City* city = core.find_city(city_id);
	if (city == nullptr) {
		result.error = "cidade desconhecida";
		return result;
	}
	Building* building = find_building(ws, city->id, building_id);
	if (building == nullptr) {
		result.error = "construção desconhecida";
		return result;
	}
	if (!SimCore::is_mine_building(*city, *building)) {
		result.error = "construção não é mina de estágio";
		return result;
	}
	// A RESERVA é a condição física: sem ela não há estágio novo, mesmo que a
	// mina ainda não tenha sido marcada como encerrada pelo tick.
	const Deposit& deposit = city->deposits[building->output_product];
	if (deposit.remaining_kt <= 0.0) {
		result.error = "reserva esgotada";
		return result;
	}
	if (building->bankrupt || building->depleted
			|| building->loss_days >= kLossDaysBeforeClose) {
		result.error = "mina encerrada";
		return result;
	}
	// Anti-duplicação: uma obra de aprofundamento por mina (a IA tentaria todo
	// dia durante os meses de obra).
	for (const ConstructionProject& in_flight : ws.construction_projects) {
		if (in_flight.deepen_stage >= 0 && in_flight.city == city->id
				&& in_flight.building_id == building->id) {
			result.error = "obra em andamento";
			return result;
		}
	}
	const double cost = core.mine_deepen_cost(*city, *building);
	const double capacity_added = SimCore::mine_deepen_capacity(*building);
	const int stage = building->stage + 1;
	if (cost <= 0.0 || capacity_added <= 0.0) {
		result.error = "mina sem capacidade a repor";
		return result;
	}
	std::string commit_key;
	double* cash = mine_owner_cash(core, *city, *building, commit_key);
	if (cash == nullptr) {
		result.error = "dono sem caixa";
		return result;
	}
	const double committed = committed_construction(ws, city->id, commit_key);
	if (committed + cost > *cash) {
		result.error = "capital insuficiente";
		result.cost = cost;
		return result;
	}

	result.cost = cost;
	result.capacity_added = capacity_added;
	result.stage = stage;
	result.duration_days = kMineDeepenBaseDays
		+ kMineDeepenDaysPerStage * stage;

	if (instant_construction) {
		// Hook de teste/laboratório: obra instantânea, custo integral debitado do
		// dono e pago ao setor de construção doméstico (dinheiro conservado).
		*cash -= cost;
		city->service_cash += cost;
		city->construction_revenue_pending += cost;
		building->capacity += capacity_added;
		core.set_mine_stage(*city, *building, stage);
		result.started = true;
		return result;
	}

	ConstructionProject project;
	project.id = building->id + "-DEEP" + std::to_string(stage);
	project.building_id = building->id;
	project.city = city->id;
	project.owner = building->owner;
	std::string owner_group;
	std::string owner_nation;
	if (core.owner_kind(building->owner, &owner_group, &owner_nation) != 0) {
		project.group_id = owner_group;
	}
	project.resource = building->resource;
	project.input_product = building->input_product;
	project.output_product = building->output_product;
	project.recipe = building->recipe;
	project.capacity = capacity_added;
	project.input_per_output = building->input_per_output;
	for (int level = 0; level < kEducationLevels; ++level) {
		project.education_mix[level] = building->education_mix[level];
	}
	project.jobs = building->jobs * (capacity_added
		/ std::max(1e-9, building->capacity));
	project.wage = building->wage;
	project.opex = building->opex * (capacity_added
		/ std::max(1e-9, building->capacity));
	project.expand_existing = true;
	project.deepen_stage = stage;
	project.total_cost = cost;
	// Mesma regra do §15: o capital da obra é reservado no início.
	project.reserved = cost;
	*cash -= cost;
	core.add_construction_project(project);
	result.started = true;
	result.project_id = project.id;
	return result;
}

} // namespace ogs
