#include "sim_core.h"

#include "balance.h"
#include "central_bank.h"
#include "city_gov.h"
#include "profiling.h"
#include "storage.h"
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
		// G2: mapeamento produto→tipo e o índice de cidade dos armazéns
		// (depende do city_lookup, criado acima).
		ogs::rebuild_storage_indexes(ws_);
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
		// para a conservação valer em qualquer ponto do tick. O caixa da região
		// (B3) é dinheiro do mundo, como o das famílias.
		total += city.pop.savings + city.firm_cash + city.service_cash
		       + city.government_budget + city.capital_income_next
		       + city.rent_income_next + city.region.cash;
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
	out.storage_sites = ws_.storage_sites;
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
			// C1: o salário atrasado da planta é baixado dos DOIS lados quando
			// ela fecha — a obligation morre com a firma (o ativo das famílias
			// deixa de existir; é perda do credor, não dinheiro que some).
			if (it->city_index >= 0
					&& it->city_index < static_cast<int>(ws_.cities.size())
					&& it->wage_arrears > 0.0) {
				City& city_ref = ws_.cities[static_cast<std::size_t>(
					it->city_index)];
				city_ref.pop.arrears_receivable = std::max(0.0,
					city_ref.pop.arrears_receivable - it->wage_arrears);
			}
			// D-099: composição para o aceite do 365 d (runtime, não vai ao
			// save). A primária COM plano de safra é o caso que a regra de
			// liquidez poupa; o contador mostra quem ainda fecha.
			if (it->output_product >= 0
					&& it->output_product < kMaxProducts) {
				++ws_.bankrupt_by_output[it->output_product];
			}
			if (it->recipe < 0 && it->input_product < 0
					&& it->city_index >= 0
					&& it->city_index < static_cast<int>(ws_.cities.size())
					&& crop_planned_at(static_cast<std::size_t>(it->city_index),
						it->output_product)) {
				++ws_.bankrupt_planned_primary;
			}
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
			// B2 (correção): `stratum_level` NÃO é fluxo do dia — é o teto de
			// consumo que a fase de consumo recalcula e que o save persiste
			// (`strata_level`). Zerá-lo aqui deixava `refresh_income_classes`
			// publicando 100% basic enquanto o dia não chegava ao consumo
			// (serviços liam a classe zerada). Mantém o valor do dia anterior.
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
			city.markets[product].effective = 0.0;
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
			city.agri_in_kind[product] = 0.0;
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
		building.royalty_paid = 0.0;
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

		// B5: royalty de concessão — fração da RECEITA da mina/fazenda concedida
		// paga ao Estado (governo nacional). Reduz o lucro do grupo
		// concessionário: o caixa da firma paga o royalty ao governo (dinheiro
		// conservado). Cap no lucro (não se paga royalty de prejuízo).
		for (const int bi : local_buildings) {
			Building& building =
				ws_.buildings[static_cast<std::size_t>(bi)];
			if (building.royalty <= 0.0 || building.owner == "state") {
				continue;
			}
			// O royalty é limitado TAMBÉM pelo caixa da firma (a fonte de onde
			// o lucro do concessionário sai): sem o teto, o clamp de
			// `max(0, ...)` criava dinheiro quando a firma tinha menos caixa
			// que o royalty (o governo recebia o cheio, a firma pagava menos).
			const double royalty_paid = std::min({
				building.revenue * building.royalty,
				std::max(0.0, building.profit),
				std::max(0.0, city.firm_cash)});
			if (royalty_paid <= 0.0) {
				continue;
			}
			building.profit -= royalty_paid;
			building.royalty_paid = royalty_paid;
			city.firm_cash -= royalty_paid;
			city.government_budget += royalty_paid;
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
			// D-099 (liquidez × solvência): fazenda PRIMÁRIA cujo par
			// (cidade, produto) tem plano de safra não está insolvente
			// enquanto a colheita não vendida está no estoque da cidade. O
			// teste antigo olhava só caixa/receita: a soja de AUS-DAR não tem
			// comprador local (`sold` = 0), então a folha da janela de colheita
			// não tinha receita e a fazenda fechava mesmo com o celeiro cheio.
			// O estoque do produto, na FATIA da firma (a mesma regra de
			// rateio da receita: capacidade efetiva ÷ capacidade do produto),
			// vale a preço de mercado da cidade e é ativo — não é isenção: se
			// o estoque não cobre o prejuízo do dia, `loss_days` volta a
			// acumular e a firma fecha. A lavoura em pé (safra ainda não
			// entregue do plano) entra como o mesmo ativo: é a carência até a
			// próxima colheita que a decisão pede — o custo de plantio é
			// capital de giro da safra, não prejuízo. Nada de dinheiro é criado
			// (estoque e lavoura são avaliados, não vendidos) e a folha
			// continua limitada ao caixa da cidade (pay_scale em `labor_day`),
			// então a folha devida é REDUZIDA quando falta liquidez; a dívida
			// com os trabalhadores e a venda com desconto ficam para a
			// contabilidade da C1/C3.
			bool covered_by_stock = false;
			if (!secondary && operating && building.output_product >= 0
					&& building.output_product < ws_.products
					&& crop_planned_at(city_index, building.output_product)) {
				const double product_capacity =
					capacity_by_output[building.output_product];
				const double effective = effective_capacity(building);
				const double share = product_capacity > 0.0
					? effective / product_capacity : 0.0;
				const Market& market =
					city.markets[building.output_product];
				const double price = std::max(0.0, market.price);
				// Estoque do produto na cidade (colheita já entregue e não
				// vendida) + safra ainda no campo/por entregar do plano. Os
				// dois são avaliados a preço de mercado e rateados pela
				// capacidade, como a receita.
				const double collateral = (std::max(0.0, market.stock)
					+ crop_standing_value(city.id,
						building.output_product, price))
					* price * share;
				covered_by_stock = collateral > 0.0
					&& collateral >= -building.profit;
			}
			if (building.profit < 0.0 && operating && !covered_by_stock) {
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
	// caixa/estoque negativo ou preço inválido — NaN/inf/≤0; sem banda), registra
	// e reverte.
	ws_.invariant_violations = check_invariants(money_before());
	if (!ws_.invariant_violations.empty()) {
		restore_state(ws_.snapshot_buffer);
	} else {
		// C2a (D-103): com o dia válido, a série mensal do CPI recebe a amostra
		// (a cada 30 dias por nação). Só medição — nenhum preço/caixa muda, e
		// um tick revertido não amostra (o dia não aconteceu).
		std::vector<double> base_list(
			static_cast<std::size_t>(ws_.products), 0.0);
		for (int product = 0; product < ws_.products; ++product) {
			base_list[static_cast<std::size_t>(product)] = base_price(product);
		}
		central_bank::observe_cpi(ws_, base_list);
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
	ws_.storage_sites = in.storage_sites;
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
		violations.push_back("dinheiro não conservado: dia="
			+ std::to_string(ws_.day) + " antes="
			+ std::to_string(money_before) + " depois="
			+ std::to_string(money_after) + " drift=" + std::to_string(drift));
	}

	// 2. C1 — IDENTIDADE FINANCEIRA DO MUNDO (novo invariante diário): todo
	// ativo financeiro é passivo de alguém. A soma das posições financeiras
	// (famílias + firmas + governo + exterior) menos o passivo da autoridade
	// monetária (a moeda em circulação, placeholder até a C2) tem de ser 0.
	// `cash_residual` é a checagem CRUZADA: o balanço enumera os caixas por
	// conta própria e tem de reproduzir o `money_total` canônico — se um caixa
	// ficar de fora de um dos dois lados, este teste acusa.
	{
		const balance::Ledger book = balance::ledger(ws_, money_after);
		const double book_tolerance = std::max(1e-3,
			std::abs(money_after) * 1e-12);
		const double financial_sum = book.financial_sum();
		if (!std::isfinite(financial_sum)
				|| std::abs(financial_sum) > book_tolerance) {
			violations.push_back("posições financeiras não fecham: dia="
				+ std::to_string(ws_.day) + " soma="
				+ std::to_string(financial_sum)
				+ " (famílias=" + std::to_string(book.families)
				+ " firmas=" + std::to_string(book.firms)
				+ " governo=" + std::to_string(book.government)
				+ " exterior=" + std::to_string(book.external)
				+ " base monetária=" + std::to_string(book.monetary_base)
				+ ")");
		}
		const double cash_residual = book.cash_residual();
		if (!std::isfinite(cash_residual)
				|| std::abs(cash_residual) > book_tolerance) {
			violations.push_back("enumeração de caixa diverge do money_total: dia="
				+ std::to_string(ws_.day) + " balanço="
				+ std::to_string(book.cash_total()) + " money_total="
				+ std::to_string(book.money_total) + " residual="
				+ std::to_string(cash_residual));
		}
		if (!std::isfinite(book.receivables) || book.receivables < -1e-6
				|| !std::isfinite(book.payables) || book.payables < -1e-6) {
			violations.push_back("salário atrasado negativo: dia="
				+ std::to_string(ws_.day) + " a receber="
				+ std::to_string(book.receivables) + " a pagar="
				+ std::to_string(book.payables));
		}
	}

	// 2b. C2a (D-103) — BANCO CENTRAL POR MOEDA: a moeda em circulação do dia
	// tem de ser integralmente atribuível às moedas do mundo (ou declarada como
	// não atribuível), e a base monetária medida (M0) tem de fechar com o
	// passivo do emissor da C1. Nenhuma emissão nova: a soma dos M0 por moeda +
	// os buckets sem nação = `money_total` do dia. Sem banco (C4) as reservas
	// bancárias são 0, então M0 por moeda é exatamente a moeda atribuída.
	{
		const central_bank::MoneyAllocation money =
			central_bank::allocate_money(ws_, money_after);
		const double bc_tolerance = std::max(1e-3,
			std::abs(money_after) * 1e-12);
		const double residual = money.residual();
		if (!std::isfinite(residual) || std::abs(residual) > bc_tolerance) {
			violations.push_back("moeda não atribuível às moedas do mundo: dia="
				+ std::to_string(ws_.day) + " atribuído="
				+ std::to_string(money.allocated_total()) + " money_total="
				+ std::to_string(money.total) + " residual="
				+ std::to_string(residual));
		}
		for (std::size_t index = 0; index < money.by_currency.size();
				++index) {
			const double m0 = money.by_currency[index];
			if (!std::isfinite(m0) || m0 < -1e-6) {
				violations.push_back("M0 inválido em "
					+ ws_.currencies[index].code + ": "
					+ std::to_string(m0));
			}
		}
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
		// competitividade das ofertas float e a exibição local). D-091: SEM
		// banda econômica fixa — o preço varia livremente. O único invariante
		// numérico é ser finito e > 0 (NaN/inf/negativo = corrupção, revertida).
		for (int product = 0; product < ws_.products; ++product) {
			const Market& market = city.markets[product];
			if (!std::isfinite(market.price) || market.price <= 0.0) {
				violations.push_back("preço inválido (NaN/inf/≤0) em " + city.id + " "
					+ product_name(product) + ": " + std::to_string(market.price)
					+ " (base âncora " + std::to_string(base_price(product))
					+ ", moeda " + currency_code(city.nation) + " taxa "
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

// ---------------------------------------------------------------------------
// B3 — REGIÃO (estados inteiros): semeadura do bloco do interior.
// ---------------------------------------------------------------------------
void SimCore::seed_region_demography(City& city) {
	Region& region = city.region;
	if (!region.ready || region.demography_ready) {
		return;
	}
	const auto found = ws_.age_structures.find(city.nation);
	if (found == ws_.age_structures.end() || !found->second.valid) {
		return;
	}
	const AgeStructure& structure = found->second;
	const double population = std::max(0.0, region.region_pop);
	double total = 0.0;
	for (int band = 0; band < kAgeBands; ++band) {
		region.age_band[band] = population * structure.band[band];
		total += region.age_band[band];
	}
	region.age_band[2] = std::max(0.0,
		region.age_band[2] + (population - total));
	region.demography_ready = true;
}

void SimCore::apply_region_demography(City& city) {
	Region& region = city.region;
	seed_region_demography(city);
	if (!region.demography_ready) {
		return;
	}
	double total = 0.0;
	for (int band = 0; band < kAgeBands; ++band) {
		region.age_band[band] = std::max(0.0, region.age_band[band]);
		total += region.age_band[band];
	}
	region.region_pop = total;
	reconcile_city_population(city);
}

void SimCore::seed_region_strata(City& city, const double* shares) {
	Region& region = city.region;
	if (!region.ready) {
		return;
	}
	double income_share[kIncomeStrata];
	if (shares != nullptr) {
		double sum = 0.0;
		for (int s = 0; s < kIncomeStrata; ++s) {
			income_share[s] = std::max(0.0, shares[s]);
			sum += income_share[s];
		}
		if (sum <= 1e-9) {
			for (int s = 0; s < kIncomeStrata; ++s) {
				income_share[s] = 1.0 / double(kIncomeStrata);
			}
		} else {
			for (int s = 0; s < kIncomeStrata; ++s) {
				income_share[s] /= sum;
			}
		}
	} else {
		for (int s = 0; s < kIncomeStrata; ++s) {
			income_share[s] = 1.0 / double(kIncomeStrata);
		}
	}
	for (int s = 0; s < kIncomeStrata; ++s) {
		region.strata_income_share[s] = income_share[s];
		region.stratum_people[s] = std::max(0.0, region.region_pop)
			/ double(kIncomeStrata);
		// Poupança inicial (bootstrap): o interior começa com caixa para
		// comprar comida antes do primeiro salário semanal (cadência A6),
		// repartida pela renda — mesma convenção da poupança urbana.
		region.stratum_savings[s] = std::max(0.0, region.region_pop)
			* kRegionSavingsPerCapita * income_share[s];
		region.stratum_net_income[s] = 0.0;
		region.stratum_food_spend[s] = 0.0;
		region.stratum_need[s] = 0.0;
		region.stratum_consumed[s] = 0.0;
	}
	// O caixa do interior É a soma dos quintis (round-trip exato).
	region.cash = 0.0;
	for (int s = 0; s < kIncomeStrata; ++s) {
		region.cash += region.stratum_savings[s];
	}
	region.strata_ready = true;
	// B7: semeia a informalidade (urbano + interior) e a referência de saúde.
	seed_informality(city);
}

void SimCore::recompute_region_mix(City& city) {
	Region& region = city.region;
	const double urban = region.urban_share >= 0.0
		? std::clamp(region.urban_share, 0.0, 1.0)
		: kRegionUrbanShareNeutral;
	const double agri = kRegionAgriAtRural
		+ (kRegionAgriAtUrban - kRegionAgriAtRural) * urban;
	const double industry = kRegionIndustryAtRural
		+ (kRegionIndustryAtUrban - kRegionIndustryAtRural) * urban;
	region.employment_mix[0] = agri;
	region.employment_mix[1] = industry;
	region.employment_mix[2] = std::max(0.0, 1.0 - agri - industry);
}

void SimCore::seed_region(City& city) {
	Region& region = city.region;
	if (!region.ready) {
		return;
	}
	recompute_region_mix(city);
	apply_region_demography(city);
	// Quintis do interior: usa os alvos do país quando a POP urbana já foi
	// semeada com eles (set_city_strata), senão 20% iguais (ausência explícita).
	if (city.pop.strata_ready) {
		seed_region_strata(city, city.pop.strata_income_share);
	} else {
		seed_region_strata(city, nullptr);
	}
}

void SimCore::reconcile_city_population(City& city) {
	// A população URBANA (unidade de simulação) acompanha a POP (soma das
	// coortes, já atualizada pela demografia B1); o total exibido é urbano +
	// interior (invariante B3). Sem esta passada o total não cresce com a B1.
	city.simulation_population = std::max(0.0, city.pop.population);
	city.population = city.simulation_population
		+ (city.region.ready ? std::max(0.0, city.region.region_pop) : 0.0);
}

} // namespace ogs
