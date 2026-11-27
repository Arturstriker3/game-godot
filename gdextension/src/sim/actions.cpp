// A5 — ações do mercado de construção (investir, construir, construir por grupo).
// SimCore/fachada: lógica movida de economy_sim.cpp — realocação byte-idêntica.
// C++ puro (STL): nada de godot_cpp aqui.
#include "actions.h"

#include "efficiency.h"

#include "sim_constants.h"
#include "sim_core.h"
#include "storage.h"
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
	if (city->firm_liquidity() < amount) {
		result.error = "capital insuficiente";
		return result;
	}
	if (building->input_product < 0
			&& city->suitability[building->output_product] <= 0.0) {
		result.error = "sem aptidão territorial";
		return result;
	}

	const double capacity_added = amount / std::max(1.0, building->capital_per_capacity);

	if (instant_construction) {
		const double invested = city->firm_pay(amount, City::kR1Invest);
		// A expansão é construída por serviços domésticos: o dinheiro vira
		// receita de construção (contada no PIB do próximo tick) e não
		// desaparece em um caixa inerte. Credita o que foi efetivamente
		// debitado (autorização pelo sub-saldo das firmas).
		city->service_receive(invested);
		city->construction_revenue_pending += invested;
		building->capacity += capacity_added;
		result.invested = invested;
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
	if (committed + amount > city->firm_liquidity()) {
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
	city->firm_pay(amount, City::kR1Invest);
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
	const bool state_owned = owner_name == "state";

	if (instant_construction || bootstrap_construction) {
		// Hook de teste (construção instantânea): comportamento anterior —
		// prédio pronto na hora e custo integral debitado do dono.
		double paid_cost = cost;
		if (state_owned) {
			if (city->government_budget < cost) {
				result.error = "orçamento insuficiente";
				return result;
			}
			city->government_budget -= cost;
		} else {
			if (city->firm_liquidity() < cost) {
				result.error = "capital insuficiente";
				return result;
			}
			paid_cost = city->firm_pay(cost, City::kR1Invest);
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
		// D-125: eficiência de produção por construção. Planta de BOOTSTRAP é
		// estoque pré-existente do ano-base (nasce madura, com equipamento);
		// construção instantânea de teste/obra nova nasce com a curva baixa da
		// classe. `output_scale` deixa de começar em 0,25: a rampa agora é a
		// eficiência (uma única curva, não duas penalidades somadas).
		if (bootstrap_construction) {
			eff::mature(building, true);
		} else if (city != nullptr) {
			eff::init_new(ws, *city, building);
		} else {
			building.production_efficiency = 0.30;
			building.efficiency_days = 0;
		}
		ws.buildings.push_back(building);
		// D-086: mina nova abre o estágio 0 reivindicando uma fatia da reserva da
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
		city->service_receive(paid_cost);
		city->construction_revenue_pending += paid_cost;
		result.created = true;
		result.cost = paid_cost;
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
		: city->firm_liquidity();
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
		city->firm_pay(cost, City::kR1Invest);
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
		group->cash = std::max(0.0, group->cash - owner_cost);
		group->capex_total += owner_cost;
		if (partner != nullptr && partner_cost > 0.0) {
			partner->cash = std::max(0.0, partner->cash - partner_cost);
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
		// D-125: obra nova nasce na eficiência inicial da classe (a rampa é a
		// curva de eficiência; `output_scale` fica 1,0).
		eff::init_new(ws, *city, building);
		ws.buildings.push_back(building);
		// D-086: mina nova abre o estágio 0 (mesma regra do build do Estado).
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
		city->service_receive(cost);
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
		city->service_receive(partner_cost);
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
	group->cash = std::max(0.0, group->cash - owner_cost);
	core.add_construction_project(project);
	result.created = true;
	result.cost = cost;
	result.owner = project.owner;
	result.local_share = local_share;
	result.project_id = id;
	return result;
}

namespace {
// D-086: caixa de quem paga o aprofundamento, pelo dono da MINA -
// Estado -> orcamento da cidade; grupo F9 -> caixa do grupo; "private" (legado)
// -> caixa da firma. `commit_key` e a chave do limite de comprometimento de obra
// (`committed_construction`). nullptr = dono sem caixa localizavel.
//
// R1a: com o caixa unico ligado, o "private" paga do POOL (`private_cash`) e o
// espelho da firma (`firm_cash`) e debitado a parte (`stat`); o sub-saldo das
// firmas (`firm_sub`) e o que autoriza o gasto. A conta e a mesma de um
// `firm_pay`, para a estatistica nao divergir do pool.
struct OwnerCash {
	double* cash = nullptr;
	double* stat = nullptr;
	double* sub = nullptr;
};
OwnerCash mine_owner_cash(SimCore& core, City& city, const Building& building,
		std::string& commit_key) {
	std::string group_id;
	std::string origin_nation;
	const int kind = core.owner_kind(building.owner, &group_id, &origin_nation);
	if (kind == 0) {
		commit_key = "state";
		return { &city.government_budget, nullptr, nullptr };
	}
	if (kind == 1 || kind == 2) {
		commit_key = group_id;
		Group* group = core.find_group(group_id);
		return group == nullptr ? OwnerCash{}
			: OwnerCash{ &group->cash, nullptr, nullptr };
	}
	commit_key = "private";
	if (city.unified_cash) {
		return { &city.private_cash, &city.firm_cash, &city.firm_sub };
	}
	return { &city.firm_cash, nullptr, nullptr };
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
	// D-086 (decisão do dono): a reserva NUNCA bloqueia o aprofundamento — ela é
	// só referência de profundidade. O gate de "mina encerrada" é apenas a
	// falência (`bankrupt`) ou o prejuízo persistente (`loss_days`).
	if (building->bankrupt || building->loss_days >= kLossDaysBeforeClose) {
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
	OwnerCash owner = mine_owner_cash(core, *city, *building, commit_key);
	double* cash = owner.cash;
	if (cash == nullptr) {
		result.error = "dono sem caixa";
		return result;
	}
	const double committed = committed_construction(ws, city->id, commit_key);
	// Com sub-saldos, o gasto das firmas e autorizado pelo `firm_sub`, nao
	// pelo total armazenado.
	const double budget = owner.sub != nullptr ? *owner.sub : *cash;
	if (committed + cost > budget) {
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
		if (owner.stat != nullptr) {
			*owner.stat -= cost;
		}
		if (owner.sub != nullptr) {
			*owner.sub -= cost;
		}
		city->service_receive(cost);
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
	if (owner.stat != nullptr) {
		*owner.stat -= cost;
	}
	if (owner.sub != nullptr) {
		*owner.sub -= cost;
	}
	core.add_construction_project(project);
	result.started = true;
	result.project_id = project.id;
	return result;
}

// ---------------------------------------------------------------------------
// B5 — obra de infraestrutura da região/estado (estrada/energia/água).
// ---------------------------------------------------------------------------
InfraResult action_infra_build(WorldState& ws, SimCore& core,
		const std::string& city_id, int infra_type, bool instant_construction) {
	InfraResult result;
	result.infra_type = infra_type;
	City* city = core.find_city(city_id);
	if (city == nullptr) {
		result.error = "cidade desconhecida";
		return result;
	}
	if (!city->region.ready) {
		result.error = "região sem dado";
		return result;
	}
	if (infra_type < 0 || infra_type >= kInfraTypeCount) {
		result.error = "tipo de infra inválido";
		return result;
	}
	const int current = SimCore::infra_level(city->region, infra_type);
	if (current < 0 || current >= kInfraMaxLevel) {
		result.error = "nível máximo atingido";
		return result;
	}
	// Custo e prazo: modelo declarado, por habitante da região (obra nacional).
	const double region_pop = std::max(0.0, city->region.region_pop);
	const double cost = kInfraCostPerCapitaPerLevel * region_pop;
	if (cost <= 0.0) {
		result.error = "região sem população";
		return result;
	}
	result.cost = cost;
	result.next_level = current + 1;
	result.duration_days = kInfraBuildDays;
	// Anti-duplicação: uma obra de infra do MESMO tipo por cidade em voo.
	for (const ConstructionProject& in_flight : ws.construction_projects) {
		if (in_flight.kind == CONSTRUCTION_REGION_INFRA
				&& in_flight.city == city->id
				&& in_flight.infra_type == infra_type) {
			result.error = "obra em andamento";
			return result;
		}
	}
	// Paga pelo GOVERNO NACIONAL (a cota equalizada do tesouro nacional vive
	// em city->government_budget): limite de comprometimento como as demais
	// obras do Estado.
	const double committed = committed_construction(ws, city->id, "state");
	if (committed + cost > city->government_budget) {
		result.error = "orçamento insuficiente";
		return result;
	}

	const std::string target = "INFRA-" + city->id + "-" + std::to_string(infra_type);

	if (instant_construction) {
		// Hook de teste/laboratório: sobe o nível na hora, custo integral
		// debitado e pago ao setor de construção (dinheiro conservado).
		city->government_budget -= cost;
		city->service_receive(cost);
		city->construction_revenue_pending += cost;
		SimCore::infra_level_up(city->region, infra_type);
		result.started = true;
		return result;
	}

	ConstructionProject project;
	project.id = target;
	project.building_id = target;
	project.city = city->id;
	project.owner = "state";
	project.kind = CONSTRUCTION_REGION_INFRA;
	project.infra_type = infra_type;
	project.output_product = -1;
	project.total_cost = cost;
	project.reserved = cost;
	city->government_budget -= cost;
	core.add_construction_project(project);
	result.started = true;
	result.project_id = project.id;
	return result;
}

// ---------------------------------------------------------------------------
// B5 — concessão de mina/fazenda estatal a grupo (F9) com royalty.
// ---------------------------------------------------------------------------
ConcedeResult action_concede(WorldState& ws, SimCore& core,
		const std::string& city_id, const std::string& building_id,
		const std::string& group_id, double royalty_rate) {
	ConcedeResult result;
	City* city = core.find_city(city_id);
	if (city == nullptr) {
		result.error = "cidade desconhecida";
		return result;
	}
	Building* building = find_building(ws, city->id, building_id);
	if (building == nullptr) {
		result.error = "ativo desconhecido";
		return result;
	}
	if (building->owner != "state") {
		result.error = "ativo não é estatal";
		return result;
	}
	// Concessão vale para extração PRIMÁRIA (mina/fazenda, sem receita fabril).
	if (building->recipe >= 0 || building->input_product >= 0) {
		result.error = "ativo não é mina/fazenda";
		return result;
	}
	Group* buyer = core.find_group(group_id);
	if (buyer == nullptr) {
		result.error = "concessionário desconhecido";
		return result;
	}
	const bool foreign = buyer->origin_nation != city->nation;
	if (foreign) {
		const std::string sector = building->output_product >= 0
			&& building->output_product < static_cast<int>(ws.product_sectors.size())
			? ws.product_sectors[static_cast<std::size_t>(building->output_product)]
			: std::string();
		if (core.fdi_mode(city->nation, sector) == 3) {
			result.error = "setor fechado ao capital estrangeiro";
			return result;
		}
	}
	const double price = building->capacity * building->capital_per_capacity;
	if (buyer->cash + 1e-9 < price) {
		result.error = "caixa insuficiente do concessionário";
		return result;
	}
	const double rate = std::isfinite(royalty_rate) && royalty_rate > 0.0
		? std::clamp(royalty_rate, 0.0, 1.0)
		: kConcessionRoyaltyRate;
	buyer->cash -= price;
	buyer->capex_total += price;
	city->government_budget += price;
	building->owner = foreign
		? "foreign:" + buyer->id + "@" + buyer->origin_nation
		: "national:" + buyer->id;
	building->local_share = 0.0;
	building->partner_group.clear();
	building->royalty = rate;
	result.ok = true;
	result.price = price;
	result.royalty = rate;
	result.owner = building->owner;
	return result;
}

// ---------------------------------------------------------------------------
// G2 — armazenagem
// ---------------------------------------------------------------------------

StorageBuildResult action_storage_build(WorldState& ws, SimCore& core,
		const std::string& city_id, int type, int scope, double tons,
		bool instant_construction) {
	StorageBuildResult result;
	result.type = type;
	result.scope = scope;
	City* city = core.find_city(city_id);
	if (city == nullptr) {
		result.error = "cidade desconhecida";
		return result;
	}
	if (type < 0 || type >= kStorageTypeCount) {
		result.error = "tipo de armazém inválido";
		return result;
	}
	if (scope < 0 || scope >= kStorageScopeCount) {
		result.error = "dono inválido";
		return result;
	}
	if (!(tons > 0.0)) {
		result.error = "capacidade inválida";
		return result;
	}
	const bool government = scope == STORAGE_GOVERNMENT;
	const char* owner = government ? "state" : "private";
	const double cost = tons * kStorageCostPerTon[type];
	const double cash = government ? std::max(0.0, city->government_budget)
		: std::max(0.0, city->firm_liquidity());
	const double committed = committed_construction(ws, city->id, owner);
	if (cost > cash - committed + 1e-9) {
		result.error = "capital insuficiente";
		return result;
	}
	if (instant_construction) {
		double paid_cost = cost;
		if (government) {
			city->government_budget -= cost;
		} else {
			paid_cost = city->firm_pay(cost, City::kR1Invest);
		}
		// Construção doméstica: o dinheiro vira receita do setor de construção
		// (contabilizada no próximo tick), como nas demais obras instantâneas.
		city->service_receive(paid_cost);
		city->construction_revenue_pending += paid_cost;
		const int city_index = static_cast<int>(city - ws.cities.data());
		add_storage_capacity(ws, city_index, type, scope, tons);
		result.started = true;
		result.cost = cost;
		result.capacity_added = tons;
		result.duration_days = 0;
		result.project_id = "STORAGE-" + std::string(
			storage_type_name(type) ? storage_type_name(type) : "x")
			+ "-" + std::to_string(ws.day);
		return result;
	}
	ConstructionProject project;
	project.id = "STORAGE-" + std::string(
		storage_type_name(type) ? storage_type_name(type) : "x")
		+ "-" + sane_token(city->id) + "-" + std::to_string(ws.day)
		+ "-" + std::to_string(ws.construction_projects.size());
	project.building_id = project.id;
	project.city = city->id;
	project.owner = owner;
	project.kind = CONSTRUCTION_STORAGE;
	project.storage_type = type;
	project.storage_capacity_t = tons;
	project.total_cost = cost;
	project.reserved = cost;
	project.created_day = ws.day;
	if (government) {
		city->government_budget -= cost;
	} else {
		city->firm_pay(cost, City::kR1Invest);
	}
	core.add_construction_project(project);
	result.started = true;
	result.cost = cost;
	result.capacity_added = tons;
	result.duration_days = project.duration_days;
	result.project_id = project.id;
	return result;
}

StorageTradeResult action_storage_trade(WorldState& ws, SimCore& core,
		const std::string& city_id, int type, int scope, int product,
		double tons, bool buy, bool relief) {
	StorageTradeResult result;
	City* city = core.find_city(city_id);
	if (city == nullptr) {
		result.error = "cidade desconhecida";
		return result;
	}
	if (type < 0 || type >= kStorageTypeCount || scope < 0
			|| scope >= kStorageScopeCount) {
		result.error = "armazém inválido";
		return result;
	}
	if (storage_type_is_structure_only(type)) {
		result.error = "tipo só de estrutura nesta parte";
		return result;
	}
	if (product < 0 || product >= ws.products) {
		result.error = "produto inválido";
		return result;
	}
	if (static_cast<std::size_t>(product) >= ws.storage_product_type.size()) {
		rebuild_storage_indexes(ws);
	}
	const int product_type =
		static_cast<std::size_t>(product) < ws.storage_product_type.size()
		? ws.storage_product_type[static_cast<std::size_t>(product)] : -1;
	if (!storage_type_accepts(type, product_type)) {
		result.error = "produto não é guardado neste tipo";
		return result;
	}
	const int city_index = static_cast<int>(city - ws.cities.data());
	const int site_index = find_storage_site(ws, city_index, type, scope);
	if (site_index < 0) {
		result.error = "armazém não existe";
		return result;
	}
	StorageSite& site = ws.storage_sites[static_cast<std::size_t>(site_index)];
	if (site.stock.size() != static_cast<std::size_t>(ws.products)) {
		site.stock.resize(static_cast<std::size_t>(ws.products), 0.0);
		site.inflow_stock.resize(static_cast<std::size_t>(ws.products), 0.0);
	}
	Market& market = city->markets[product];
	const double price = market.price > 0.0 ? market.price
		: core.base_price(product);
	if (!(price > 0.0)) {
		result.error = "preço inválido";
		return result;
	}
	if (!(tons > 0.0)) {
		result.error = "quantidade inválida";
		return result;
	}
	const bool government = scope == STORAGE_GOVERNMENT;
	if (buy) {
		const double free_capacity = storage_free_capacity(ws, site);
		double qty = std::min(tons, std::min(std::max(0.0, market.stock),
			free_capacity));
		const double cash = government ? std::max(0.0, city->government_budget)
			: std::max(0.0, city->firm_liquidity());
		qty = std::min(qty, cash / price);
		if (qty <= 1e-9) {
			result.error = "sem estoque/capacidade/caixa para comprar";
			return result;
		}
		const double value = qty * price;
		double paid_value = value;
		if (government) {
			city->government_budget -= value;
		} else {
			paid_value = city->firm_pay(value);
		}
		// O vendedor é a firma local: o dinheiro só muda de caixa. Credita o
		// valor efetivamente debitado (autorização pelo sub-saldo das firmas).
		city->firm_receive(paid_value);
		city->revenue_by_product[product] += value;
		market.stock -= qty;
		market.demand += qty;
		market.sold += qty;
		market.effective += qty;
		site.stock[static_cast<std::size_t>(product)] += qty;
		site.bought_today_t += qty;
		result.ok = true;
		result.tons = qty;
		result.value = value;
		return result;
	}
	// Liberação: só o estoque RETIDO sai (o pulmão da colheita já escoa pela
	// fase). Sem `relief`, o comprador local paga; em `relief` a tonelada vai
	// ao mercado sem pagamento (socorro) — bens não são dinheiro.
	const double inflow = static_cast<std::size_t>(product)
		< site.inflow_stock.size()
		? std::max(0.0, site.inflow_stock[
			static_cast<std::size_t>(product)]) : 0.0;
	const double held = std::max(0.0,
		site.stock[static_cast<std::size_t>(product)] - inflow);
	double qty = std::min(tons, held);
	if (qty <= 1e-9) {
		result.error = "sem estoque retido para liberar";
		return result;
	}
	double paid_qty = 0.0;
	if (!relief) {
		const double payer_cash = std::max(0.0, city->firm_liquidity());
		paid_qty = std::min(qty, payer_cash / price);
		qty = paid_qty;
	}
	if (qty <= 1e-9) {
		result.error = "comprador sem caixa";
		return result;
	}
	const double value = paid_qty * price;
	if (paid_qty > 0.0) {
		const double paid_value = city->firm_pay(value);
		if (government) {
			city->government_budget += paid_value;
		} else {
			// Privado: o comprador e o dono são a mesma firma agregada — o
			// dinheiro não muda de caixa; o lucro aparece no preço do mercado.
			city->firm_receive(paid_value);
		}
	}
	site.stock[static_cast<std::size_t>(product)] -= qty;
	market.stock += qty;
	market.supply += qty;
	site.released_today_t += qty;
	result.ok = true;
	result.tons = qty;
	result.value = value;
	result.free_tons = relief ? qty - paid_qty : 0.0;
	return result;
}

// ---------------------------------------------------------------------------
// D-125 — eficiência de produção (estilo HOI4). A planta começa baixa e cresce
// com os dias de operação; trocar de produto abre uma transição sem produção e
// aplica a retenção (60% na mesma família, reinício na outra). Os números são
// os defaults declarados em data/definitions/production/efficiency.json
// (espelho C++ em EfficiencyParams); nada aqui é dado observado.
// ---------------------------------------------------------------------------
namespace {

// Cidade dona da construção (-1 quando não existe).
int city_index_of(const WorldState& ws, const std::string& city_id) {
	const auto found = ws.city_lookup.find(city_id);
	return found == ws.city_lookup.end() ? -1 : found->second;
}

// Primeira receita (ordem determinística de `ws.recipes`) cujo produto
// principal é `product`; -1 quando não existe.
int first_recipe_for(const WorldState& ws, int product) {
	for (std::size_t index = 0; index < ws.recipes.size(); ++index) {
		if (ws.recipes[index].output == product) {
			return static_cast<int>(index);
		}
	}
	return -1;
}

} // namespace

ChangeProductResult action_change_product(WorldState& ws, SimCore& core,
		const std::string& city_id, const std::string& building_id,
		const std::string& product, const std::string& recipe_id) {
	ChangeProductResult result;
	Building* building = find_building(ws, city_id, building_id);
	if (building == nullptr) {
		result.error = "construção inexistente";
		return result;
	}
	const int city_index = city_index_of(ws, city_id);
	if (city_index < 0 || city_index >= static_cast<int>(ws.cities.size())) {
		result.error = "cidade inexistente";
		return result;
	}
	City& city = ws.cities[static_cast<std::size_t>(city_index)];
	const int target = core.product_index(product);
	if (target < 0) {
		result.error = "produto desconhecido";
		return result;
	}
	const EfficiencyClass cls = eff::classify(ws, city, *building);
	if (cls == EfficiencyClass::kMine) {
		result.error = "mina não troca de produto (a jazida manda)";
		return result;
	}
	const int days = eff::transition_days(ws, city, *building, target);
	if (days < 0) {
		result.error = "sem janela de plantio para o produto-alvo";
		return result;
	}
	// Receita nova: explícita quando veio; senão a primeira da ordem do
	// catálogo que produz o bem. Fazenda/pecuária não têm receita.
	int recipe = -1;
	if (!recipe_id.empty()) {
		recipe = core.recipe_index(recipe_id);
		if (recipe < 0) {
			result.error = "receita desconhecida";
			return result;
		}
		if (ws.recipes[static_cast<std::size_t>(recipe)].output != target) {
			result.error = "receita não produz o produto pedido";
			return result;
		}
	} else if (building->recipe >= 0) {
		recipe = first_recipe_for(ws, target);
		if (recipe < 0) {
			result.error = "sem receita para o produto pedido";
			return result;
		}
	}
	// Retenção: mesma família (goods.json.sector) guarda 60%; família
	// diferente zera e recomeça na eficiência inicial da classe.
	const bool same = eff::same_family(ws, building->output_product, target);
	const double retention = same ? ws.efficiency.retention_same_family
		: ws.efficiency.retention_other;
	const double start = std::clamp(
		eff::curve(ws, cls).start, 0.0, 1.0);
	const double kept = building->production_efficiency * retention;
	building->production_efficiency = std::clamp(
		retention > 0.0 ? std::max(start, kept) : start, 0.0, 1.0);
	building->output_product = target;
	if (recipe >= 0 || building->recipe >= 0) {
		building->recipe = recipe;
	}
	building->transition_days = days;
	building->efficiency_days = 0;
	building->idle_today = false;
	result.changed = true;
	result.efficiency = building->production_efficiency;
	result.transition_days = days;
	return result;
}

RefineryModeResult action_set_refinery_mode(WorldState& ws, SimCore& core,
		const std::string& city_id, const std::string& building_id,
		const std::string& mode) {
	(void)core;
	RefineryModeResult result;
	Building* building = find_building(ws, city_id, building_id);
	if (building == nullptr) {
		result.error = "construção inexistente";
		return result;
	}
	if (!eff::is_refinery(ws, *building)) {
		result.error = "construção não é refinaria";
		return result;
	}
	int value = -1;
	if (mode == "standard" || mode == "0") {
		value = 0;
	} else if (mode == "diesel" || mode == "1") {
		value = 1;
	} else if (mode == "jet" || mode == "2") {
		value = 2;
	}
	if (value < 0) {
		result.error = "modo desconhecido (standard|diesel|jet)";
		return result;
	}
	result.mode = value;
	if (building->refinery_mode == value) {
		result.changed = false;
		return result;
	}
	building->refinery_mode = value;
	// A troca de modo leva `transition_mode_days` sem produção; a eficiência
	// conquistada é preservada (não é troca de produto).
	building->transition_days = std::max(0, ws.efficiency.transition_mode_days);
	building->idle_today = false;
	result.changed = true;
	result.transition_days = building->transition_days;
	return result;
}

EquipmentResult action_set_building_equipment(WorldState& ws, SimCore& core,
		const std::string& city_id, const std::string& building_id,
		double level) {
	(void)core;
	EquipmentResult result;
	Building* building = find_building(ws, city_id, building_id);
	if (building == nullptr) {
		result.error = "construção inexistente";
		return result;
	}
	const int city_index = city_index_of(ws, city_id);
	if (city_index < 0) {
		result.error = "cidade inexistente";
		return result;
	}
	const City& city = ws.cities[static_cast<std::size_t>(city_index)];
	const EfficiencyClass cls = eff::classify(ws, city, *building);
	if (cls != EfficiencyClass::kMine && cls != EfficiencyClass::kFarm
			&& cls != EfficiencyClass::kRanch) {
		result.error = "equipamento só existe em lavoura, pecuária e mina";
		return result;
	}
	building->equipment = std::clamp(level, 0.0, 1.0);
	building->equipment_active = building->equipment > 0.0;
	result.set = true;
	result.level = building->equipment;
	return result;
}

} // namespace ogs
