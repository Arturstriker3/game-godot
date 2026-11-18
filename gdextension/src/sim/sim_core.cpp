#include "sim_core.h"

#include "city_gov.h"
#include "profiling.h"
#include "sim_constants.h"
#include "sim_labor.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace ogs {

SimCore::SimCore(WorldState& ws) : ws_(ws) {}

// ------------------------------------------------------------------
// S16 — paralelismo determinístico por cidade.
// ------------------------------------------------------------------

int SimCore::effective_threads() const {
	if (sim_threads_ <= 0) {
		const int hw = static_cast<int>(std::thread::hardware_concurrency());
		return std::clamp(hw - 2, 1, 8);
	}
	return sim_threads_;
}

ThreadPool& SimCore::pool() {
	const int n = effective_threads();
	if (!pool_ || pool_->thread_count() != n) {
		pool_.reset(new ThreadPool(n));
	}
	return *pool_;
}

void SimCore::set_sim_threads(int threads) {
	// Guarda o valor; o pool é (re)criado sob demanda na próxima fase paralela.
	sim_threads_ = std::max(0, threads);
}

// ------------------------------------------------------------------
// Helpers compartilhados.
// ------------------------------------------------------------------

City* SimCore::find_city(const std::string& id) {
	const auto found = ws_.city_lookup.find(id);
	if (found == ws_.city_lookup.end()) {
		return nullptr;
	}
	return &ws_.cities[static_cast<std::size_t>(found->second)];
}

const City* SimCore::find_city(const std::string& id) const {
	const auto found = ws_.city_lookup.find(id);
	if (found == ws_.city_lookup.end()) {
		return nullptr;
	}
	return &ws_.cities[static_cast<std::size_t>(found->second)];
}

void SimCore::set_land_capacity(const std::string& city_id, int product,
		double value) {
	if (product < 0 || product >= ws_.products) {
		return;
	}
	City* city = find_city(city_id);
	if (city == nullptr) {
		return;
	}
	city->land_capacity[product] = std::max(0.0, value);
}

void SimCore::rebuild_indexes(bool full) {
	// Caminho barato (todo tick): SÓ o índice de construções por cidade, que é
	// o único que muda dentro do jogo (falida removida no prólogo, obra
	// concluída no tick). Cidades, nações, massas de terra e o lookup por id só
	// mudam no carregamento — esses são refeitos com full=true (load, set_urban,
	// cenário de validação e rollback de invariante).
	if (full) {
		// A2: tabelas de nomes (ordem lexicográfica: reproduz a ordem das somas
		// que hoje itera std::map<std::string, ...>).
		std::vector<std::string> nations;
		std::vector<std::string> landmasses;
		nations.reserve(ws_.cities.size() / 4 + 8);
		landmasses.reserve(ws_.cities.size() / 8 + 8);
		for (const City& city : ws_.cities) {
			nations.push_back(city.nation);
			landmasses.push_back(city.landmass);
		}
		for (const Port& port : ws_.ports) {
			nations.push_back(port.nation);
			landmasses.push_back(port.landmass);
		}
		for (const std::string& nation : ws_.fleet_order) {
			nations.push_back(nation);
		}
		for (const Group& group : ws_.groups) {
			nations.push_back(group.origin_nation);
		}
		for (const Currency& currency : ws_.currencies) {
			for (const std::string& nation : currency.nations) {
				nations.push_back(nation);
			}
		}
		for (const Tariff& tariff : ws_.tariffs) {
			if (tariff.buyer != "*") {
				nations.push_back(tariff.buyer);
			}
			if (tariff.seller != "*") {
				nations.push_back(tariff.seller);
			}
		}
		for (const FdiPolicy& policy : ws_.fdi_policies) {
			nations.push_back(policy.nation);
		}
		for (const SectorIncentive& incentive : ws_.incentives) {
			nations.push_back(incentive.nation);
		}
		for (const auto& row : ws_.confidence) {
			nations.push_back(row.first);
		}
		for (const auto& row : ws_.group_flows) {
			nations.push_back(row.first);
		}
		for (const auto& row : ws_.minimum_wages) {
			nations.push_back(row.first);
		}
		ws_.nation_table.build(nations);
		ws_.landmass_table.build(landmasses);
		for (City& city : ws_.cities) {
			city.nation_index = ws_.nation_table.find(city.nation);
			city.landmass_index = ws_.landmass_table.find(city.landmass);
		}
		ws_.city_lookup.clear();
		ws_.city_lookup.reserve(ws_.cities.size() * 2);
		for (std::size_t index = 0; index < ws_.cities.size(); ++index) {
			ws_.city_lookup.emplace(ws_.cities[index].id, static_cast<int>(index));
		}
		// F4: cidades por nação, na ordem estável dos índices de cidade. A frota
		// é nacional (o governo compra combustível/peças/veículos no mercado
		// interno), então a compra visita só as cidades da nação, nunca o mundo.
		ws_.cities_by_nation.clear();
		ws_.cities_by_nation.reserve(ws_.nation_table.size() + 8);
		ws_.fleet_order.clear();
		ws_.fleet_order.reserve(ws_.nation_table.size() + 8);
		for (std::size_t index = 0; index < ws_.cities.size(); ++index) {
			const std::string& nation = ws_.cities[index].nation;
			auto found = ws_.cities_by_nation.find(nation);
			if (found == ws_.cities_by_nation.end()) {
				found = ws_.cities_by_nation.emplace(nation, std::vector<int>{}).first;
				ws_.fleet_order.push_back(nation);
			}
			found->second.push_back(static_cast<int>(index));
		}
		// A2: o índice de tarifas usa as tabelas de nação/produto; refaz aqui
		// (o load chama rebuild_tariff_lookup antes das cidades existirem).
		rebuild_tariff_lookup();
		// Caminho quente abaixo: o índice por cidade e o de construções.
	}
	// Uma lista de construções por cidade: as fases do tick e o resumo passam a
	// visitar só as construções locais, em vez de varrer o mundo inteiro.
	if (ws_.buildings_by_city.size() != ws_.cities.size()) {
		ws_.buildings_by_city.resize(ws_.cities.size());
	}
	for (std::vector<int>& list : ws_.buildings_by_city) {
		list.clear();  // preserva a capacidade: nada de 834 realocações por dia
	}
	for (std::size_t index = 0; index < ws_.buildings.size(); ++index) {
		Building& building = ws_.buildings[index];
		if (building.city_index < 0 && !building.city.empty()) {
			const auto found = ws_.city_lookup.find(building.city);
			building.city_index =
				found == ws_.city_lookup.end() ? -1 : found->second;
		}
		if (building.city_index < 0
				|| building.city_index >= static_cast<int>(ws_.buildings_by_city.size())) {
			continue;
		}
		ws_.buildings_by_city[static_cast<std::size_t>(building.city_index)]
			.push_back(static_cast<int>(index));
	}
}

double SimCore::money_total() const {
	double total = ws_.transport_cash + ws_.foreign_cash + ws_.construction_cash;
	for (const City& city : ws_.cities) {
		// capital_income_next é um crédito pendente às famílias: entra no total
		// para a conservação valer em qualquer ponto do tick.
		total += city.pop.savings + city.firm_cash + city.service_cash
		       + city.government_budget + city.capital_income_next
		       + city.rent_income_next;
	}
	// F9: o caixa dos grupos é dinheiro do mundo (capital retido/dividendos a
	// pagar). Sem ele, mover lucro da firma para o grupo pareceria destruição.
	for (const Group& group : ws_.groups) {
		total += group.cash;
	}
	// §15: o fundo reservado de cada obra (capital do investimento debitado no
	// início) é dinheiro do mundo — soma para a conservação valer entre o débito
	// (na decisão) e o pagamento incremental (no tick).
	for (const ConstructionProject& project : ws_.construction_projects) {
		total += project.reserved;
	}
	// §19 W2/W3: caixa da prefeitura (inclui aluguel pago a dono público) e
	// fundo das obras urbanas ainda não pagos — dinheiro do mundo.
	total += city_gov::money_total(ws_);
	return total;
}

void SimCore::snapshot_state(Snapshot& out) const {
	out.cities = ws_.cities;
	out.buildings = ws_.buildings;
	out.construction_projects = ws_.construction_projects;
	out.service_businesses = ws_.service_businesses;
	out.next_service_business_id = ws_.next_service_business_id;
	out.service_bankruptcies = ws_.service_bankruptcies;
	out.transit = ws_.in_transit;
	out.fleets = ws_.fleets;
	out.groups = ws_.groups;
	out.confidence = ws_.confidence;
	out.group_flows = ws_.group_flows;
	out.currencies = ws_.currencies;
	city_gov::snapshot(ws_, out);
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

// ------------------------------------------------------------------
// Ordem do dia (fases).
// ------------------------------------------------------------------

void SimCore::prologue() {
	// Retrato para reverter o tick se um invariante rígido for violado.
	// Buffer PERSISTENTE: os vetores são reutilizados entre dias (com 114 bens
	// uma cidade custa ~27 KB; realocar 834 cópias por tick custava ~5 ms).
	Snapshot& before = ws_.snapshot_buffer;
	{
		OGS_SIM_ZONE(kPhasePrologueSnapshot);
		snapshot_state(before);
	}
	money_before_ = money_total();

	// O "resto do prólogo" cobre esta faixa inteira; o bloco de índices abaixo é
	// uma zona ANINHADA (o tempo dele também aparece em `prologue_other`, porque
	// `rebuild_indexes()` cai no meio da faixa).
	{
	OGS_SIM_ZONE(kPhasePrologueOther);
	++ws_.day;
	ws_.last_orders.clear();
	// Obras de porto com prazo vencido entram em operação neste dia.
	tick_port_expansion();
	// §15: obras de construção (projeto físico) avançam por materiais +
	// capacidade do setor de construção; as concluídas entram em operação.
	tick_construction();

	// Encerra as construções falidas no início do tick: assim o PIB do dia já
	// não as inclui e o status fica consistente (a falência foi marcada no tick
	// anterior, no fim da fase 7).
	// D-086 (decisão do dono): o recurso NUNCA acaba — `depleted` nunca é marcado
	// em lugar nenhum do núcleo (a produção não zera e o aprofundamento não é
	// bloqueado por reserva), então não há o ramo de "mina com jazida esgotada"
	// aqui. `ws_.depleted_mines` permanece 0 e é mantido no estado/save/status
	// por contrato.
	for (auto it = ws_.buildings.begin(); it != ws_.buildings.end();) {
		if (it->bankrupt) {
			++ws_.bankruptcies;
			it = ws_.buildings.erase(it);
			continue;
		}
		++it;
	}
	// O tick apaga construções falidas: os índices por cidade são refeitos
	// antes de qualquer fase usá-los. A2: caminho barato — cidades, nações e
	// tarifas só mudam no carregamento, então só o índice de construções por
	// cidade é reconstruído aqui.
	{
		OGS_SIM_ZONE(kPhasePrologueIndexes);
		rebuild_indexes(false);
	}

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
}

void SimCore::phase_reset() {
	// 2. Zera os fluxos do dia.
	for (int index = 0; index < ws_.products; ++index) {
		ws_.produced_today[index] = 0.0;
		ws_.consumed_today[index] = 0.0;
	}
	for (City& city : ws_.cities) {
		city.pop.wage_income = 0.0;
		city.pop.capital_income = 0.0;
		city.pop.transfers = 0.0;
		city.pop.taxes = 0.0;
		city.pop.service_spend = 0.0;
		city.pop.disposable = 0.0;
		city.pop.external_income = 0.0;
		city.pop.rent_income = 0.0;
		for (int s = 0; s < kIncomeStrata; ++s) {
			// B2: fluxos do dia por quintil (poupança/patrimônio persistem).
			city.pop.stratum_wage[s] = 0.0;
			city.pop.stratum_capital[s] = 0.0;
			city.pop.stratum_rent[s] = 0.0;
			city.pop.stratum_transfer[s] = 0.0;
			city.pop.stratum_net_income[s] = 0.0;
			city.pop.stratum_service_spend[s] = 0.0;
			city.pop.stratum_rent_paid[s] = 0.0;
			city.pop.stratum_rent_bill[s] = 0.0;
			city.pop.stratum_food_spend[s] = 0.0;
			city.pop.stratum_need[s] = 0.0;
			city.pop.stratum_consumed[s] = 0.0;
			city.pop.stratum_level[s] = 0;
		}
		city.pop.food_spend = 0.0;
		city.pop.consumed = 0.0;
		city.pop.need = 0.0;
		city.pop.satisfaction = 0.0;
		city.pop.needs_spend = 0.0;
		city.pop.durable_spend = 0.0;
		city.pop.needs_satisfaction = 0.0;
		city.pop.needs_level = 0;
		// F3: níveis e duráveis (redimensiona só quando o mundo mudou).
		init_pop_needs(city.pop);
		for (int need_level = 0; need_level < kMaxNeedsLevels; ++need_level) {
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
		for (int sector = 0; sector < kMaxServiceSectors; ++sector) {
			ServiceMarket& service = city.service_markets[sector];
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
			for (int level = 0; level < kMaxNeedsLevels; ++level) {
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
	for (Building& building : ws_.buildings) {
		building.workers = 0.0;
		building.utilization = 0.0;
		building.revenue = 0.0;
		building.input_value = 0.0;
		building.costs = 0.0;
		building.profit = 0.0;
		building.paid_wages = 0.0;
		building.paid_opex = 0.0;
	}

	OGS_SIM_PHASE_LAP(kPhaseReset);
}

void SimCore::phase_accounting() {
	// 7. Receita por construção, lucro, distribuição de capital e PIB.
	for (std::size_t city_index = 0; city_index < ws_.cities.size(); ++city_index) {
		City& city = ws_.cities[city_index];
		const std::vector<int>& local_buildings =
			ws_.buildings_by_city[city_index];
		std::array<double, kMaxProducts> capacity_by_output{};
		// D-086: a fatia de receita segue a capacidade EFETIVA do dia — mina com
		// estágio esgotado produz 5% e não pode levar 100% da receita da cidade
		// (isso maquiava o lucro da mina parada e afundava a mina ativa).
		auto effective_capacity = [&city](const Building& building) {
			return building.capacity * building.output_scale
				* mine_stage_factor(city, building);
		};
		for (const int bi : local_buildings) {
			const Building& building =
				ws_.buildings[static_cast<std::size_t>(bi)];
			capacity_by_output[building.output_product] +=
				effective_capacity(building);
		}
		for (const int bi : local_buildings) {
			Building& building =
				ws_.buildings[static_cast<std::size_t>(bi)];
			const double effective = effective_capacity(building);
			const double share = capacity_by_output[building.output_product] > 0.0
				? effective / capacity_by_output[building.output_product] : 0.0;
			building.revenue = city.revenue_by_product[building.output_product] * share;
			building.costs = building.paid_wages + building.paid_opex
				+ building.input_value;
			building.profit = building.revenue - building.costs;
		}
		// Planta estratégica (bootstrap industrial §12.3): enquanto a demanda
		// final não existe (F3), o governo cobre o prejuízo da planta de
		// referência em vez de deixá-la fechar. Transferência explícita
		// governo -> firma: conserva o dinheiro e não maquia o resultado do
		// setor (a perda continua visível em `profit` antes do subsídio).
		for (const int bi : local_buildings) {
			Building& building =
				ws_.buildings[static_cast<std::size_t>(bi)];
			if (!building.strategic || building.profit >= 0.0) {
				continue;
			}
			const double subsidy = std::min(-building.profit,
				std::max(0.0, city.government_budget));
			if (subsidy > 0.0) {
				city.government_budget -= subsidy;
				city.firm_cash += subsidy;
				building.profit += subsidy;
			}
		}

		// A receita de serviços inclui consumo das famílias, opex empresarial,
		// frete e construção: são serviços domésticos, não importações. F8 soma
		// a exportação de serviços (paga pelo setor externo) e a compra do
		// governo (transferência do orçamento); os insumos comprados pelos
		// negócios reduzem o lucro do setor (o dinheiro foi para as firmas).
		city.service_revenue = city.pop.service_spend + city.opex_revenue
			+ city.freight_revenue + city.construction_revenue
			+ city.service_export_revenue + city.service_government_revenue;
		city.service_profit = city.service_revenue - city.service_wage_bill
			- city.service_input_cost;

		double private_profit = 0.0;
		double state_profit = 0.0;
		double value_added = 0.0;
		// F9: lucro e faturamento por grupo presente na cidade. O JV divide a
		// linha do ativo entre o grupo estrangeiro e o parceiro nacional.
		std::vector<std::array<double, 3>> group_rows;  // índice, lucro, receita
		auto add_group_row = [&group_rows](int group_index, double profit,
				double revenue) {
			for (std::array<double, 3>& row : group_rows) {
				if (static_cast<int>(row[0]) == group_index) {
					row[1] += profit;
					row[2] += revenue;
					return;
				}
			}
			group_rows.push_back({static_cast<double>(group_index), profit, revenue});
		};
		for (const int bi : local_buildings) {
			const Building& building =
				ws_.buildings[static_cast<std::size_t>(bi)];
			if (building.owner == "state") {
				state_profit += building.profit;
			} else {
				std::string group_id;
				std::string origin;
				const int owner_code = owner_kind(building.owner, &group_id,
					&origin);
				const int group_slot = owner_code == 1 || owner_code == 2
					? group_index(group_id) : -1;
				if (group_slot >= 0) {
					const double local_share = std::clamp(building.local_share, 0.0, 1.0);
					const int partner_index = building.partner_group.empty()
						? -1 : group_index(building.partner_group);
					if (local_share > 0.0 && partner_index >= 0) {
						add_group_row(group_slot, building.profit * (1.0 - local_share),
							building.revenue * (1.0 - local_share));
						add_group_row(partner_index, building.profit * local_share,
							building.revenue * local_share);
					} else {
						add_group_row(group_slot, building.profit, building.revenue);
					}
				} else {
					// Dono de grupo desconhecido (save antigo/editado): cai no
					// privado agregado, sem criar dinheiro.
					private_profit += building.profit;
				}
			}
			value_added += building.revenue - building.input_value;
		}

		// Dividendos só depois de reconstituir o capital de giro. Lucros de
		// estatais vão para o orçamento público; privados, para as famílias.
		const double firm_target =
			city.simulation_population * kFirmCashPerCapita;
		double firm_surplus = std::max(0.0, city.firm_cash - firm_target);
		// F9: o lucro dos ativos de grupo sai da firma para o grupo (imposto
		// corporativo fica no anfitrião; o resto vira caixa do grupo e, no
		// fechamento do dia, dividendo ao estrato dono). A retenção é o que
		// financia o investimento seguinte; nada é criado ou destruído.
		for (const std::array<double, 3>& row : group_rows) {
			route_group_profit(static_cast<int>(row[0]), city.nation,
				row[1], row[2], city, firm_surplus);
		}
		firm_surplus = std::max(0.0, city.firm_cash - firm_target);
		const double positive_total = std::max(0.0, private_profit)
			+ std::max(0.0, state_profit);
		const double distributable = std::min(
			positive_total * kCapitalDistribution, firm_surplus);
		const double private_share = positive_total > 0.0
			? std::max(0.0, private_profit) / positive_total : 0.0;
		const double modeled_distribution = distributable * private_share;
		const double state_distribution = distributable - modeled_distribution;
		const double service_target =
			(city.service_wage_bill + city.service_input_cost)
			* kServiceCashBufferDays;
		const double service_surplus = std::max(0.0,
			city.service_cash - service_target);
		// F8: o lucro do setor é renda da POP dona (plano §13). O caixa guarda
		// só o capital de giro (dias de folha + insumos); o resto do lucro é
		// distribuído no dia seguinte. O alvo antigo por habitante (0,05/pessoa)
		// quase nunca era alcançado e retinha o lucro para sempre: a cidade
		// pagava serviços sem receber o lucro de volta e esgotava a poupança
		// que compra comida (SDN-KHA, SOL-HAR em 365 d, F8).
		const double service_distribution = std::min(
			std::max(0.0, city.service_profit * kCapitalDistribution),
			service_surplus);
		city.firm_cash -= modeled_distribution + state_distribution;
		city.service_cash -= service_distribution;
		city.government_budget += state_distribution;
		// §19 W3: a renda de capital do dia SOMA ao crédito pendente em vez de
		// substituí-lo — o aluguel pago às famílias donas (fase 5b) já está
		// aqui. Sem o "+", o aluguel à família era anulado na contabilidade
		// (dinheiro não conservado).
		city.capital_income_next += modeled_distribution + service_distribution;
		city.gdp = value_added + city.service_revenue;

		// Comportamento das empresas: prejuízo persistente reduz a produção;
		// demanda acima da capacidade com lucro permite expansão.
		for (const int bi : local_buildings) {
			Building& building =
				ws_.buildings[static_cast<std::size_t>(bi)];
			// Carência de falência (planta nova, §15): a planta construída não é
			// avaliada por prejuízo durante os primeiros kRampDays de
			// operação — o capital da obra é investimento, não prejuízo
			// operacional; a planta precisa de tempo para formar carteira antes
			// de ser avaliada. O output_scale segue o valor de entrada (100%
			// para primária, 25% para fábrica) e as regras de corte abaixo.
			if (building.ramp_days > 0) {
				--building.ramp_days;
				building.loss_days = 0;
				continue;
			}
			// Planta secundária ociosa não é insolvente: sem operação no dia,
			// não acumula dias de prejuízo (o custo de stand-by já é pequeno).
			// D-086: mina com o estágio esgotado é o mesmo caso — o poço parado
			// aguarda aprofundamento; ele não "faliu" e não pode desaparecer.
			const bool secondary =
				building.recipe >= 0 || building.input_product >= 0;
			const bool mine_idle = !secondary && is_mine_building(city, building)
				&& mine_stage_factor(city, building) <= kMineStageFloor + 1e-12;
			const bool operating = mine_idle ? false
				: secondary ? building.utilization > 1e-9 : true;
			if (building.profit < 0.0 && operating) {
				++building.loss_days;
			} else {
				building.loss_days = 0;
			}
			// Falência: prejuízo persistente além do corte fecha a firma. A
			// regra é explícita — a construção some (capacidade e empregos), em
			// vez de ficar produzindo no piso no vermelho para sempre. Planta
			// estratégica é mantida pelo governo (ver subsídio acima).
			if (building.loss_days >= kLossDaysBeforeClose && !building.strategic) {
				building.bankrupt = true;
			}
			if (building.loss_days >= kLossDaysBeforeCut) {
				building.output_scale = std::max(kMinOutputScale,
					building.output_scale * kLossCut);
			} else if (building.profit > 0.0) {
				// Recuperação gradual após um período de prejuízo.
				building.output_scale = std::min(1.0, building.output_scale * 1.02);
			}
		}

		// F7: salários por nível reagem ao fechamento do dia — aperto do
		// mercado (vagas × trabalhadores), margem das firmas e âncora de renda
		// do país, com rigidez. O prêmio por educação emerge da escassez.
		double building_revenue = 0.0;
		double building_profit = 0.0;
		for (const int bi : local_buildings) {
			const Building& building =
				ws_.buildings[static_cast<std::size_t>(bi)];
			building_revenue += building.revenue;
			building_profit += building.profit;
		}
		const double firm_margin = building_revenue > 1e-9
			? std::clamp(building_profit / building_revenue, -0.25, 0.25) : 0.0;
		update_wages(city, firm_margin);

		// O salário de serviços se ajusta ao mercado de trabalho residual e o
		// emprego segue a receita do setor ao novo salário. Se a receita não
		// sustenta nem o salário de subsistência, sobra desemprego de verdade —
		// não um piso artificial de 5% de vagas.
		double cheapest_food = 0.0;
		for (int slot = 0; slot < static_cast<int>(ws_.food_products.size()); ++slot) {
			const int product = food_product(slot);
			const double factor = food_calories(product);
			if (factor <= 0.0) {
				continue;
			}
			const double per_calorie = city.markets[product].price / factor;
			if (per_calorie > 0.0
					&& (cheapest_food <= 0.0 || per_calorie < cheapest_food)) {
				cheapest_food = per_calorie;
			}
		}
		const double subsistence_wage = kNeedPerCapita * cheapest_food;
		double modeled_jobs_total = 0.0;
		for (const int bi : local_buildings) {
			modeled_jobs_total += ws_.buildings[static_cast<std::size_t>(bi)].jobs;
		}
		const double residual_labor = std::max(0.0,
			city.pop.labor_force - modeled_jobs_total);
		const double service_supply = residual_labor
			* (1.0 - ws_.natural_unemployment);
		// F7: quem define o salário do setor de serviços é o MERCADO DE TRABALHO
		// (labor_day grava aqui a média ponderada por nível/mistura). A
		// antiga regra de "receita ÷ força residual" brigava com ela e travava o
		// mundo em ~15% de desemprego. A receita continua limitando as VAGAS: se
		// ela não paga nem o salário de subsistência, sobra desemprego de
		// verdade — não um piso artificial de vagas.
		// F7 calibração: o setor de serviços é o empregador RESIDUAL do modelo
		// (serviços urbanos absorvem quem não está nas construções). Quem limita
		// a FOLHA é o caixa do setor (liquidez, em labor_day), não o número
		// de vagas: cortar vagas pela receita transformava um salário-alvo alto
		// em desemprego em massa (1ª medição da calibração: EUA 41,8%, DEU
		// 48,8%), porque a receita de serviços é uma fração da renda das
		// famílias e não acompanha a parcela do trabalho no valor adicionado.
		(void)subsistence_wage;
		city.service_jobs += (service_supply - city.service_jobs)
			* kServiceAdjust;
	}

	OGS_SIM_PHASE_LAP(kPhaseAccounting);
}

void SimCore::phase_invariants() {
	// 10. Invariantes: se o tick corrompeu o estado (dinheiro não conservado,
	// caixa/estoque negativo ou preço fora da banda), registra e reverte.
	ws_.invariant_violations = check_invariants(money_before());
	if (!ws_.invariant_violations.empty()) {
		restore_state(ws_.snapshot_buffer);
	}
	OGS_SIM_PHASE_LAP(kPhaseInvariants);
}

void SimCore::restore_state(const Snapshot& in) {
	ws_.cities = in.cities;
	ws_.buildings = in.buildings;
	ws_.construction_projects = in.construction_projects;
	ws_.service_businesses = in.service_businesses;
	ws_.next_service_business_id = in.next_service_business_id;
	ws_.service_bankruptcies = in.service_bankruptcies;
	ws_.in_transit = in.transit;
	ws_.fleets = in.fleets;
	ws_.groups = in.groups;
	ws_.confidence = in.confidence;
	ws_.group_flows = in.group_flows;
	// F5: o drift cambial roda antes do invariante; desfazer um tick
	// corrompido também desfaz a taxa do dia.
	ws_.currencies = in.currencies;
	city_gov::restore(ws_, in);
	rebuild_currency_indexes();
	ws_.transport_cash = in.transport_cash;
	ws_.foreign_cash = in.foreign_cash;
	ws_.construction_cash = in.construction_cash;
	for (int index = 0; index < ws_.products; ++index) {
		ws_.produced[index] = in.produced[index];
		ws_.consumed_total[index] = in.consumed_total[index];
		ws_.produced_today[index] = in.produced_today[index];
		ws_.consumed_today[index] = in.consumed_today[index];
	}
	ws_.day = in.day;
	rebuild_indexes();
	rebuild_service_indexes();
	// F9: o índice de grupos acompanha o vetor restaurado.
	ws_.group_lookup.clear();
	for (std::size_t index = 0; index < ws_.groups.size(); ++index) {
		ws_.group_lookup[ws_.groups[index].id] = static_cast<int>(index);
	}
}

std::vector<std::string> SimCore::check_invariants(double money_before) const {
	std::vector<std::string> violations;

	// 1. Conservação de dinheiro: toda transação move dinheiro entre caixas
	// (firma, serviço, governo, poupança, transporte, exterior, construção).
	// O resto do mundo (foreign_cash) pode ficar negativo por design.
	const double money_after = money_total();
	const double drift = std::abs(money_after - money_before);
	const double tolerance = std::max(1e-3, std::abs(money_before) * 1e-12);
	if (!std::isfinite(money_after) || drift > tolerance) {
		violations.push_back("dinheiro não conservado: antes="
			+ std::to_string(money_before) + " depois="
			+ std::to_string(money_after) + " drift=" + std::to_string(drift));
	}

	for (const City& city : ws_.cities) {
		auto check_nonneg = [&violations, &city](const char* label, double value) {
			if (!std::isfinite(value) || value < -1e-6) {
				violations.push_back(std::string("caixa negativo (") + label + ") em "
					+ city.id + ": " + std::to_string(value));
			}
		};
		check_nonneg("firm", city.firm_cash);
		check_nonneg("service", city.service_cash);
		check_nonneg("government", city.government_budget);
		check_nonneg("savings", city.pop.savings);
		// B2: a soma dos quintis é o escalar da cidade; negativo em um estrato
		// é corrupção de estado, não "dívida" (a dívida é campo próprio, zero
		// até a Parte C).
		for (int s = 0; s < kIncomeStrata; ++s) {
			check_nonneg("stratum_savings", city.pop.stratum_savings[s]);
		}

		// Preço na âncora: o câmbio não move mais o preço interno (só a
		// competitividade das ofertas float e a exibição local), então a banda
		// pode ser comparada sem folga.
		for (int product = 0; product < ws_.products; ++product) {
			const Market& market = city.markets[product];
			const double base = base_price(product);
			const double floor_price = base * 0.5;
			const double ceiling = base * 3.0;
			if (!std::isfinite(market.price)
					|| market.price < floor_price
					|| market.price > ceiling) {
				violations.push_back("preço fora da banda em " + city.id + " "
					+ product_name(product) + ": " + std::to_string(market.price)
					+ " (base âncora " + std::to_string(base) + ", moeda "
					+ currency_code(city.nation) + " taxa "
					+ std::to_string(fx_rate(city.nation)) + ")");
			}
			if (!std::isfinite(market.stock) || market.stock < -1e-6) {
				violations.push_back("estoque negativo em " + city.id + " "
					+ product_name(product) + ": " + std::to_string(market.stock));
			}
		}
	}

	auto check_global = [&violations](const char* label, double value) {
		if (!std::isfinite(value) || value < -1e-6) {
			violations.push_back(std::string("caixa global negativo (") + label
				+ "): " + std::to_string(value));
		}
	};
	check_global("transport", ws_.transport_cash);
	check_global("construction", ws_.construction_cash);
	// F9: caixa de grupo é dinheiro do mundo; não pode ser negativo nem NaN.
	for (const Group& group : ws_.groups) {
		if (!std::isfinite(group.cash) || group.cash < -1e-6) {
			violations.push_back("caixa de grupo inválido (" + group.id + "): "
				+ std::to_string(group.cash));
		}
	}

	return violations;
}

} // namespace ogs
