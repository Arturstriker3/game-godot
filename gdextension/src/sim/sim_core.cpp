#include "sim_core.h"

#include "balance.h"
#include "central_bank.h"
#include "monetary.h"
#include "city_gov.h"
#include "profiling.h"
#include "storage.h"
#include "sim_constants.h"
#include "sim_labor.h"
#include "treasury.h"

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

void SimCore::set_regional_cash(bool enabled, int external_subceiling) {
	ws_.regional_cash = enabled;
	ws_.external_subceiling = external_subceiling;
	for (City& city : ws_.cities) {
		// R1a (medicao): o modo do sub-teto vale em qualquer chamada - trocar
		// de modo nao mexe no dinheiro, so na leitura do teto.
		city.external_subceiling = external_subceiling;
		// Preserva o total: ao ligar, o pool nasce da soma dos dois caixas;
		// ao desligar, os dois campos seguem como o estado (private_cash sai
		// do save).
		if (enabled && !city.unified_cash) {
			city.unified_cash = true;
			// Semeadura ANTES da fatia: `measured_firm_revenue_share` le os
			// sub-saldos quando o pool esta ligado; sincronizar primeiro evita
			// medir 50/50 com os subs ainda zerados.
			city.sync_private_cash();
			// R1a (continuacao): a fatia inicial do experimento t160-t163 e a
			// razao dos dois caixas no instante da fusao; o pendulo mensal a
			// atualiza depois.
			if (city.firm_revenue_share < 0.0) {
				city.firm_revenue_share =
					city.measured_firm_revenue_share();
			}
		} else if (enabled) {
			// Save/cidade que ja veio com o pool: garante a invariante dos
			// sub-saldos (idempotente em save novo).
			city.normalize_subbalances();
		} else {
			city.unified_cash = false;
			city.private_cash = 0.0;
			city.firm_sub = 0.0;
			city.service_sub = 0.0;
			city.firm_revenue_share = -1.0;
			city.firm_external_paid_today = 0.0;
			city.service_external_paid_today = 0.0;
		}
	}
}

void SimCore::set_r1_diag(bool enabled) {
	// R1a (t163): instrumentacao de medicao. Nao decide nada; so habilita as
	// escritas do livro por cidade (com false, nenhum numero do mundo muda).
	for (City& city : ws_.cities) {
		city.r1_diag = enabled;
		city.r1_reset_day();
	}
}

void SimCore::apply_regional_cash_after_load(bool saved_flag,
		bool has_saved_flag) {
	const bool enabled = has_saved_flag ? saved_flag : ws_.regional_cash;
	ws_.regional_cash = enabled;
	for (City& city : ws_.cities) {
		// R1a (medicao): o modo vem da configuracao ja aplicada (rework.json).
		city.external_subceiling = ws_.external_subceiling;
		if (enabled) {
			if (!city.unified_cash) {
				// Save anterior a flag (ou sem a chave): o pool nasce da soma
				// e os sub-saldos sao semeados dos dois caixas antigos.
				city.unified_cash = true;
				city.sync_private_cash();
			} else {
				// Save ON: `normalize_subbalances` fecha a invariante e e
				// idempotente quando o save ja trouxe os dois sub-saldos.
				city.normalize_subbalances();
			}
			// Save sem a fatia medida (anterior a esta correcao): deriva da
			// razao dos espelhos, que o load preservou.
			if (city.firm_revenue_share < 0.0) {
				city.firm_revenue_share =
					city.measured_firm_revenue_share();
			}
		} else {
			city.unified_cash = false;
			city.private_cash = 0.0;
			city.firm_sub = 0.0;
			city.service_sub = 0.0;
			city.firm_revenue_share = -1.0;
			city.firm_external_paid_today = 0.0;
			city.service_external_paid_today = 0.0;
		}
	}
}

void SimCore::set_national_treasury(bool enabled, double budget_share,
		double floor_fraction, double works_share,
		double maintenance_share) {
	// R1b - configuracao invalida: o Tesouro so existe com o caixa unico
	// regional (R1a). A flag e tratada como desligada e o motivo fica no
	// `treasury_config_error` (a fachada imprime no log do Godot).
	if (enabled && !ws_.regional_cash) {
		ws_.treasury_config_error =
			"rework.national_treasury requer rework.regional_cash ligado; "
			"flag tratada como desligada";
		if (ws_.national_treasury) {
			treasury::reconcile(ws_);
			treasury::drain_to_regions(ws_);
		}
		ws_.national_treasury = false;
		return;
	}
	ws_.treasury_config_error.clear();
	ws_.regional_budget_share = std::isfinite(budget_share)
		? std::clamp(budget_share, 0.0, 1.0) : 1.0;
	ws_.regional_budget_floor_fraction = std::isfinite(floor_fraction)
		? std::clamp(floor_fraction, 0.0, 1.0) : 0.5;
	ws_.regional_budget_works_share = std::isfinite(works_share)
		? std::clamp(works_share, 0.0, 1.0) : 0.35;
	ws_.regional_budget_maintenance_share = std::isfinite(maintenance_share)
		? std::clamp(maintenance_share, 0.0, 1.0) : 0.5;
	if (enabled && !ws_.national_treasury) {
		ws_.national_treasury = true;
	}
	if (!enabled && ws_.national_treasury) {
		// Desligar nao destroi dinheiro: o Tesouro e os restantes virtuais
		// viram dinheiro real na primeira cidade da nacao (mundo OFF).
		treasury::reconcile(ws_);
		treasury::drain_to_regions(ws_);
		ws_.national_treasury = false;
		ws_.gov_cash_open.clear();
		return;
	}
	if (ws_.national_treasury && !ws_.regional_budget_started
			&& !ws_.cities.empty()) {
		// Primeira aplicacao com mundo montado: nao pode sobrar caixa
		// publico estacionado na regiao (vira saldo do Tesouro) e o mes
		// corrente ja abre com dotacao.
		treasury::absorb_public_cash(ws_);
		treasury::start_month(ws_);
		ws_.gov_cash_open.clear();
	}
}

void SimCore::apply_national_treasury_after_load(bool saved_flag,
		bool has_saved_flag, const std::map<std::string, double>& saved_balances,
		double collected_total, double spent_total) {
	const bool was_on = has_saved_flag && saved_flag;
	bool enabled = has_saved_flag ? saved_flag : ws_.national_treasury;
	if (enabled && !ws_.regional_cash) {
		ws_.treasury_config_error =
			"save com national_treasury sem regional_cash; flag tratada como "
			"desligada";
		enabled = false;
	}
	if (!enabled) {
		ws_.national_treasury = false;
		if (was_on) {
			// Os saldos/contadores do save eram virtuais (ON); no mundo OFF
			// eles viram dinheiro real, sem criar nem destruir total.
			ws_.nation_treasury = saved_balances;
			for (auto& row : ws_.nation_treasury) {
				row.second = std::isfinite(row.second)
					? std::max(0.0, row.second) : 0.0;
			}
			treasury::drain_to_regions(ws_);
		}
		return;
	}
	ws_.treasury_config_error.clear();
	ws_.national_treasury = true;
	ws_.nation_treasury = saved_balances;
	for (auto& row : ws_.nation_treasury) {
		row.second = std::isfinite(row.second) ? std::max(0.0, row.second) : 0.0;
	}
	ws_.treasury_collected_total = std::isfinite(collected_total)
		? std::max(0.0, collected_total) : 0.0;
	ws_.treasury_spent_total = std::isfinite(spent_total)
		? std::max(0.0, spent_total) : 0.0;
	ws_.regional_budget_started = true;
	const std::size_t gov_count = std::min(ws_.cities.size(),
		ws_.city_gov.size());
	for (std::size_t index = 0; index < ws_.cities.size(); ++index) {
		City& city = ws_.cities[index];
		// Save antigo/sem os contadores: deriva o limite do que ainda existe
		// nos restantes (nada e criado; o Tesouro continua a fonte).
		if (city.regional_budget_limit_month <= 0.0) {
			city.regional_budget_limit_month =
				std::max(0.0, city.government_budget)
				+ (index < gov_count
					? std::max(0.0, ws_.city_gov[index].cash) : 0.0);
		}
		city.regional_budget_open_services =
			std::max(0.0, city.government_budget);
		city.regional_budget_open_works = index < gov_count
			? std::max(0.0, ws_.city_gov[index].cash) : 0.0;
	}
}

void SimCore::reconcile_regional_budget() {
	treasury::reconcile(ws_);
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
	double total = ws_.transport_cash + ws_.foreign_cash + ws_.construction_cash
		+ ws_.power_cash;
	// R1b: o saldo do Tesouro Nacional por nacao e o UNICO caixa publico com a
	// flag ligada. Os restantes de orcamento regional de servicos
	// (`City::government_budget`) e de obras (`CityGov::cash`) sao apenas
	// contadores de limite do mes — nao sao dinheiro e nao entram no total.
	total += treasury::total_balance(ws_);
	for (const City& city : ws_.cities) {
		// capital_income_next é um crédito pendente às famílias: entra no total
		// para a conservação valer em qualquer ponto do tick. O caixa da região
		// (B3) é dinheiro do mundo, como o das famílias.
		// A expressão do OFF é a MESMA do mundo anterior, termo a termo: em
		// debug (sem otimização) reordenar a soma muda ulps e o digest.
		if (!ws_.national_treasury) {
			total += city.pop.savings + city.private_total()
			       + city.government_budget + city.capital_income_next
			       + city.rent_income_next + city.region.cash;
		} else {
			total += city.pop.savings + city.private_total()
			       + city.capital_income_next
			       + city.rent_income_next + city.region.cash;
		}
	}
	// F9: o caixa dos grupos é dinheiro do mundo (capital retido/dividendos a
	// pagar). Sem ele, mover lucro da firma para o grupo pareceria destruição.
	for (const Group& group : ws_.groups) {
		total += group.cash;
	}
	// C3 (D-107): o caixa do banco agregado também é dinheiro do mundo (lucro
	// retido do spread e o capital pago pelo Tesouro).
	for (const auto& row : ws_.banks) {
		total += row.second.cash;
	}
	// C6 (D-114): o fundo de seguro de depósito é dinheiro do mundo (o
	// prêmio sai do caixa do banco; o pagamento volta como caixa do banco
	// resolvido). Sem fundo (mundo base) o total é idêntico à C4.
	for (const auto& row : ws_.deposit_insurance) {
		total += row.second.fund;
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

// C4 (D-110): base monetária = dinheiro total − depósitos à vista (dinheiro
// bancário criado pelo crédito). Sem crédito os depósitos são 0 e `m0_total`
// é exatamente o `money_total` da C2a/C3.
double SimCore::m0_total() const {
	double deposits = 0.0;
	for (const auto& row : ws_.banks) {
		deposits += std::max(0.0, row.second.deposits);
	}
	return money_total() - deposits;
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
	// C2b1: política monetária, dívida pública e emissão do dia.
	out.monetary = ws_.monetary;
	out.gov_bonds = ws_.gov_bonds;
	out.external_bonds = ws_.external_bonds;
	out.gov_cash_open = ws_.gov_cash_open;
	// R1b: saldo do Tesouro por nacao (arrecadacao/gasto do dia) e os
	// acumulados diagnosticos. Um tick revertido devolve os dois.
	out.nation_treasury = ws_.nation_treasury;
	out.treasury_collected_total = ws_.treasury_collected_total;
	out.treasury_spent_total = ws_.treasury_spent_total;
	// C2b2 (D-106): a intervenção cambial e a desvalorização são estado do dia.
	out.fx = ws_.fx;
	out.fx_events = ws_.fx_events;
	// C3 (D-107): o banco agregado (crédito, juro, calote) e a emissão líquida.
	out.banks = ws_.banks;
	// C6 (D-114): fundo de seguro e eventos entram no retrato (a crise move
	// caixa e destrói depósito; reverter tem de desfazer os dois).
	out.deposit_insurance = ws_.deposit_insurance;
	out.bank_crisis_events = ws_.bank_crisis_events;
	out.bank_crisis_events_total = ws_.bank_crisis_events_total;
	// C5 (D-112): preço, confiança e dividendo da bolsa são estado do dia.
	out.stock_exchanges = ws_.stock_exchanges;
	out.stock_tickers = ws_.stock_tickers;
	out.power_cities = ws_.power_cities;
	out.power_plants = ws_.power_plants;
	out.power_grids = ws_.power_grids;
	// E5 (D-117): o comércio do dia gera energia e move a conta da rede;
	// reverter o tick precisa devolver as linhas e o preço/servido do dia.
	out.interconnects = ws_.interconnects;
	out.power_interconnects_ready = ws_.power_interconnects_ready;
	out.power_cash = ws_.power_cash;
	out.emission_today = ws_.emission_today;
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
	// R1b (t172): fecha o gasto publico ja realizado ANTES do retrato do dia
	// contra o Tesouro. O orcamento regional e um limite: gastar do contador
	// (capital publico do banco no setup, comando A3 entre dias, etc.) move
	// dinheiro para o destinatario, mas a contraparte no Tesouro so existe na
	// reconciliacao. Se ela ficar para a primeira fase do dia, o tick debita o
	// Tesouro por um gasto cuja criacao de dinheiro ficou ANTES do
	// `money_before` e o invariante ve um drift do tamanho exato desse gasto
	// (~31,36 M no dia 1; ~6,25 M no dia 2 com comandos). Reconciliar aqui poe
	// os dois lados antes do retrato; a reconciliacao intradiaria (fronteiras
	// de fase) continua igual. No OFF e no-op.
	if (ws_.national_treasury && ws_.regional_budget_started) {
		treasury::reconcile(ws_);
	}
	// Retrato para reverter o tick se um invariante rígido for violado.
	// Buffer PERSISTENTE: os vetores são reutilizados entre dias (com 114 bens
	// uma cidade custa ~27 KB; realocar 834 cópias por tick custava ~5 ms).
	Snapshot& before = ws_.snapshot_buffer;
	// C2b1 (D-104): o reset do dia vem ANTES do retrato — reverter um tick que
	// violou o invariante devolve o dia sem emissão (o dia não aconteceu) e sem
	// a medição de déficit dele.
	ws_.emission_today = 0.0;
	ws_.bank_money_today = 0.0;
	for (auto& row : ws_.monetary) {
		row.second.emission_today = 0.0;
		row.second.deficit_measured = 0.0;
		row.second.unfunded = 0.0;
	}
	{
		OGS_SIM_ZONE(kPhasePrologueSnapshot);
		snapshot_state(before);
	}
	money_before_ = money_total();
	// Janela do déficit: abertura do mundo (dia 1) e, depois, o fecho de cada
	// dia (`monetary::tick` refresca) — um gasto público entre dois dias conta.
	if (ws_.gov_cash_open.empty()) {
		const std::size_t gov_count = std::min(ws_.cities.size(),
			ws_.city_gov.size());
		for (std::size_t index = 0; index < ws_.cities.size(); ++index) {
			const City& city = ws_.cities[index];
			double cash = ws_.national_treasury ? 0.0 : city.government_budget;
			if (index < gov_count) {
				const CityGov& gov = ws_.city_gov[index];
				// O fundo das obras ja iniciadas e dinheiro publico reservado
				// nos dois modos; o caixa corrente da prefeitura so e dinheiro
				// no modo OFF.
				if (!ws_.national_treasury) {
					cash += std::max(0.0, gov.cash);
				}
				for (int slot = 0; slot < gov.project_count; ++slot) {
					cash += std::max(0.0, gov.projects[slot].reserved);
				}
			}
			ws_.gov_cash_open[city.nation] += cash;
		}
		// R1b: o Tesouro Nacional e o caixa publico da nacao na janela do
		// deficit (o orcamento regional e limite, nao saldo).
		for (const auto& row : ws_.nation_treasury) {
			ws_.gov_cash_open[row.first] += row.second;
		}
	}

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
		// R1a (continuacao): pendulo mensal da participacao por setor. A
		// receita do dia que acabou (vendas das firmas por produto + receita
		// de servicos) entra como amostra; a fatia anda 1/30 por dia e fica em
		// [5%, 95%]. So com o caixa unico (desligada nao muda nada).
		if (city.unified_cash) {
			double firm_gross = 0.0;
			for (int product = 0; product < ws_.products; ++product) {
				firm_gross += std::max(0.0, city.revenue_by_product[product]);
			}
			const double service_gross = std::max(0.0, city.service_revenue);
			const double gross = firm_gross + service_gross;
			if (gross > 1e-9) {
				const double current = city.measured_firm_revenue_share();
				const double sample = firm_gross / gross;
				city.firm_revenue_share = std::clamp(
					current + (sample - current)
						/ City::kRevenueSharePendulumDays,
					City::kExternalShareFloor,
					1.0 - City::kExternalShareFloor);
			}
		}
		// Sub-teto diario dos pagamentos externos: recomeca zerado todo dia.
		city.firm_external_paid_today = 0.0;
		city.service_external_paid_today = 0.0;
		// R1a (t163): o livro de medicao tambem e do dia.
		city.r1_reset_day();
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
		// D-124 (F0): a co-produção também OFERTA o produto extra, então a
		// receita do lote tem de ser rateada entre o produtor principal e o
		// co-produtor. A capacidade é declarada em toneladas do produto
		// PRINCIPAL; a capacidade equivalente da saída extra é a mesma fração
		// do lote (`byproduct_qty / yield`). Antes, diesel/querosene vendidos
		// ficavam sem dono e a refinaria aparecia com prejuízo de −39,39/t
		// (diagnóstico t141): ela fechava mesmo com os três graus vendáveis.
		std::array<double, kMaxProducts> revenue_capacity_by_output{};
		for (const int bi : local_buildings) {
			const Building& building =
				ws_.buildings[static_cast<std::size_t>(bi)];
			const double effective = effective_capacity(building);
			capacity_by_output[building.output_product] += effective;
			revenue_capacity_by_output[building.output_product] += effective;
			if (building.recipe < 0
					|| building.recipe >= static_cast<int>(ws_.recipes.size())) {
				continue;
			}
			const Recipe& recipe =
				ws_.recipes[static_cast<std::size_t>(building.recipe)];
			if (recipe.yield <= 0.0) {
				continue;
			}
			for (int slot = 0; slot < recipe.byproduct_count; ++slot) {
				const int product = recipe.byproducts[slot];
				if (product < 0 || product >= ws_.products
						|| product == building.output_product) {
					continue;
				}
				revenue_capacity_by_output[product] += effective
					* recipe.byproduct_qty[slot] / recipe.yield;
			}
		}
		for (const int bi : local_buildings) {
			Building& building =
				ws_.buildings[static_cast<std::size_t>(bi)];
			const double effective = effective_capacity(building);
			const double main_capacity =
				revenue_capacity_by_output[building.output_product];
			const double share = main_capacity > 0.0
				? effective / main_capacity : 0.0;
			building.revenue = city.revenue_by_product[building.output_product] * share;
			// D-124 (F0): a receita das saídas extras do MESMO lote pertence à
			// firma que produziu o lote, na proporção do que ela ofertou do
			// produto (a mesma regra de capacidade acima). É reatribuição de
			// receita já recebida pela cidade — nenhum dinheiro é criado.
			if (building.recipe >= 0
					&& building.recipe < static_cast<int>(ws_.recipes.size())) {
				const Recipe& recipe =
					ws_.recipes[static_cast<std::size_t>(building.recipe)];
				if (recipe.yield > 0.0) {
					for (int slot = 0; slot < recipe.byproduct_count; ++slot) {
						const int product = recipe.byproducts[slot];
						if (product < 0 || product >= ws_.products
								|| product == building.output_product) {
							continue;
						}
						const double product_capacity =
							revenue_capacity_by_output[product];
						if (product_capacity <= 0.0) {
							continue;
						}
						const double co_share = effective
							* recipe.byproduct_qty[slot] / recipe.yield
							/ product_capacity;
						building.revenue +=
							city.revenue_by_product[product] * co_share;
					}
				}
			}
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
				city.firm_receive(subsidy);
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
				city.firm_external_room_request(std::min(
					building.revenue * building.royalty,
					std::max(0.0, building.profit)))});
			if (royalty_paid <= 0.0) {
				continue;
			}
			building.profit -= royalty_paid;
			building.royalty_paid = royalty_paid;
			city.firm_external_pay(royalty_paid, City::kR1Taxes);
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
		double firm_surplus = std::max(0.0, city.firm_liquidity() - firm_target);
		// F9: o lucro dos ativos de grupo sai da firma para o grupo (imposto
		// corporativo fica no anfitrião; o resto vira caixa do grupo e, no
		// fechamento do dia, dividendo ao estrato dono). A retenção é o que
		// financia o investimento seguinte; nada é criado ou destruído.
		for (const std::array<double, 3>& row : group_rows) {
			route_group_profit(static_cast<int>(row[0]), city.nation,
				row[1], row[2], city, firm_surplus);
		}
		firm_surplus = std::max(0.0, city.firm_liquidity() - firm_target);
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
		// R1a: o debito das firmas acontece ANTES de ler a liquidez dos servicos;
		// com caixa unico os dois dividem o pool e o pagamento conjunto nao pode
		// exceder o que existe. Desligado, a ordem nao muda numero nenhum.
		city.firm_pay(modeled_distribution + state_distribution,
			City::kR1Dividends);
		const double service_surplus = std::max(0.0,
			city.service_liquidity() - service_target);
		// F8: o lucro do setor é renda da POP dona (plano §13). O caixa guarda
		// só o capital de giro (dias de folha + insumos); o resto do lucro é
		// distribuído no dia seguinte. O alvo antigo por habitante (0,05/pessoa)
		// quase nunca era alcançado e retinha o lucro para sempre: a cidade
		// pagava serviços sem receber o lucro de volta e esgotava a poupança
		// que compra comida (SDN-KHA, SOL-HAR em 365 d, F8).
		const double service_distribution = std::min(
			std::max(0.0, city.service_profit * kCapitalDistribution),
			service_surplus);
		city.service_pay(service_distribution, City::kR1Dividends);
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
			// D-126: com a sobrevivência em crise ligada, a falência passa pela
			// reação (hibernação e leilão) na fase `phase_crisis`; o prédio só
			// some se o leilão não tiver comprador.
			if (!ws_.crisis.enabled
					&& building.loss_days >= kLossDaysBeforeClose
					&& !building.strategic) {
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
		// D-126: dia válido - acumula a criação/destruição bancária registrada
		// (crédito de giro da crise + fase C3/C4). Um tick revertido não conta.
		ws_.bank_money_delta_total += ws_.bank_money_today;
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
	// C2b1: desfazer o tick desfaz a taxa, o M0 emitido e a dívida pública.
	ws_.monetary = in.monetary;
	ws_.gov_bonds = in.gov_bonds;
	ws_.external_bonds = in.external_bonds;
	ws_.gov_cash_open = in.gov_cash_open;
	ws_.nation_treasury = in.nation_treasury;
	ws_.treasury_collected_total = in.treasury_collected_total;
	ws_.treasury_spent_total = in.treasury_spent_total;
	ws_.fx = in.fx;
	ws_.fx_events = in.fx_events;
	ws_.banks = in.banks;
	// C6 (D-114): fundo de seguro, eventos e a resolução andam juntos.
	ws_.deposit_insurance = in.deposit_insurance;
	ws_.bank_crisis_events = in.bank_crisis_events;
	ws_.bank_crisis_events_total = in.bank_crisis_events_total;
	// C5 (D-112): desfazer o tick desfaz preço, confiança e dividendo pago.
	ws_.stock_exchanges = in.stock_exchanges;
	ws_.stock_tickers = in.stock_tickers;
	// PARTE E (D-116): parque, rede, racionamento e caixa do setor.
	ws_.power_cities = in.power_cities;
	ws_.power_plants = in.power_plants;
	ws_.power_grids = in.power_grids;
	ws_.interconnects = in.interconnects;
	ws_.power_interconnects_ready = in.power_interconnects_ready;
	ws_.power_cash = in.power_cash;
	ws_.emission_today = in.emission_today;
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
	//
	// C2b1 (D-104): a conservação passa a ser "dinheiro total = anterior +
	// emissão líquida REGISTRADA". Só o banco central cria M0, e só por uma
	// operação contabilizada no balanço da C1 (compra de títulos do governo);
	// `ws_.emission_today` é exatamente o que ele registrou no dia. Qualquer
	// outra diferença (inclusive emissão sem registro) continua violação.
	const double money_after = money_total();
	const double expected_after = money_before + ws_.emission_today
		+ ws_.bank_money_today;
	const double drift = std::abs(money_after - expected_after);
	const double tolerance = std::max(1e-3, std::abs(money_before) * 1e-12);
	if (!std::isfinite(money_after) || drift > tolerance) {
		violations.push_back("dinheiro não conservado: dia="
			+ std::to_string(ws_.day) + " antes="
			+ std::to_string(money_before) + " depois="
			+ std::to_string(money_after) + " emissão="
			+ std::to_string(ws_.emission_today) + " drift="
			+ std::to_string(drift));
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
		// C3 (D-107) - CRÉDITO DE EMPRESA: a dívida do grupo é o empréstimo do
		// banco agregado, e o juro vencido é o mesmo dos dois lados. Posição
		// negativa de qualquer um dos dois é erro de conta, não perda (a perda
		// é o patrimônio do banco, que pode ficar negativo).
		const double loans_gap = book.bank_loans - book.group_debt;
		const double arrears_gap = book.bank_arrears - book.group_arrears;
		if (!std::isfinite(loans_gap)
				|| std::abs(loans_gap) > book_tolerance
				|| !std::isfinite(arrears_gap)
				|| std::abs(arrears_gap) > book_tolerance) {
			violations.push_back("credito nao fecha dos dois lados: dia="
				+ std::to_string(ws_.day) + " banco="
				+ std::to_string(book.bank_loans) + " grupos="
				+ std::to_string(book.group_debt) + " gap="
				+ std::to_string(loans_gap) + " juros gap="
				+ std::to_string(arrears_gap));
		}
		if (book.bank_loans < -1e-6 || book.bank_arrears < -1e-6
				|| book.bank_borrowing < -1e-6 || book.bank_cash < -1e-6
				|| book.group_debt < -1e-6
				|| book.group_arrears < -1e-6
				|| book.bank_deposits < -1e-6) {
			violations.push_back("posicao de credito negativa: dia="
				+ std::to_string(ws_.day) + " banco="
				+ std::to_string(book.bank_loans) + " reservas="
				+ std::to_string(book.bank_borrowing) + " divida="
				+ std::to_string(book.group_debt) + " depositos="
				+ std::to_string(book.bank_deposits));
		}
		// C5 (D-112) - BOLSA: a carteira dos detentores tem de fechar com as
		// ações emitidas (senão haveria ação criada do nada) e o preço é um
		// ativo — finito e não negativo. Valorização não entra na conservação
		// de dinheiro: só o dividendo move caixa.
		const double stock_gap = book.stock_assets - book.stock_claims;
		if (!std::isfinite(stock_gap)
				|| std::abs(stock_gap) > book_tolerance) {
			violations.push_back("bolsa: acoes nao fecham dos dois lados: dia="
				+ std::to_string(ws_.day) + " carteiras="
				+ std::to_string(book.stock_assets) + " emissores="
				+ std::to_string(book.stock_claims) + " gap="
				+ std::to_string(stock_gap));
		}
		for (const auto& row : ws_.stock_tickers) {
			const StockTicker& ticker = row.second;
			const double claims = ticker.holdings.total() - ticker.shares;
			const double tolerance = std::max(1e-6,
				std::abs(ticker.shares) * 1e-9);
			if (!std::isfinite(ticker.price) || ticker.price < 0.0
					|| !std::isfinite(claims)
					|| std::abs(claims) > tolerance) {
				violations.push_back("bolsa: papel inconsistente: dia="
					+ std::to_string(ws_.day) + " grupo="
					+ ticker.group_id + " preco="
					+ std::to_string(ticker.price) + " carteira="
					+ std::to_string(ticker.holdings.total()) + " acoes="
					+ std::to_string(ticker.shares));
			}
		}
		for (const auto& row : ws_.banks) {
			const Bank& bank = row.second;
			if (!std::isfinite(bank.deposits) || bank.deposits < -1e-6
					|| !std::isfinite(bank.capital)
					|| bank.capital < -1e-6
					|| !std::isfinite(bank.cash) || bank.cash < -1e-6) {
				violations.push_back("banco com posicao negativa: dia="
					+ std::to_string(ws_.day) + " " + bank.id + " depositos="
					+ std::to_string(bank.deposits) + " capital="
					+ std::to_string(bank.capital) + " caixa="
					+ std::to_string(bank.cash));
			}
		}
	}

	// 2b. C2a (D-103) — BANCO CENTRAL POR MOEDA: o caixa do dia tem de ser
	// integralmente atribuível às moedas do mundo (ou declarado como não
	// atribuível). C4 (D-110): a atribuição é de CAIXA; o M0 por moeda é
	// `caixa − depósitos` e o M1 é `caixa − reservas`.
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
		if (city.unified_cash) {
			// R1a: com o pool unico, o dinheiro real e `private_cash` e o
			// direito de gastar de cada setor e o sub-saldo. Os dois sub-saldos
			// nunca ficam negativos e a soma TEM de fechar o total (invariante
			// dura dos sub-saldos virtuais).
			check_nonneg("private", city.private_cash);
			check_nonneg("firm_sub", city.firm_sub);
			check_nonneg("service_sub", city.service_sub);
			const double sub_sum = city.firm_sub + city.service_sub;
			// R1a (QA t167): tolerancia apertada. Antes 1e-6*(1+|pool|) dava
			// ~1.000 num pool de 1e9 e nao pegava deriva grande; os primitivos
			// mantem a soma a poucos ulps, entao 1e-10 relativo (piso de 1e-6
			// para pools pequenos) e folgado para o arredondamento e aperta o
			// guarda em 10.000x.
			const double tolerance = std::max(1e-6,
				1e-10 * (1.0 + std::fabs(city.private_cash)));
			if (!std::isfinite(sub_sum)
					|| std::fabs(sub_sum - city.private_cash) > tolerance) {
				violations.push_back("sub-saldos nao somam o pool em "
					+ city.id + ": firm_sub=" + std::to_string(city.firm_sub)
					+ " service_sub=" + std::to_string(city.service_sub)
					+ " private_cash=" + std::to_string(city.private_cash));
			}
		} else {
			check_nonneg("firm", city.firm_cash);
			check_nonneg("service", city.service_cash);
		}
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
