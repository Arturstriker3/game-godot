// Implementação de ai_phase.h (A5): orquestração da IA em C++ puro.
//
// Nada aqui decide: o agendamento (grupos da vez, cidades da vez) e a emissão
// de comandos são infraestrutura; as regras vivem em `AiGroupInvestor::run`
// (ai_group.h), `AiPrivateInvestor::run_city` (ai_private.h) e
// `AiGovernment::run_city` (ai_government.h).
//
// A thread é a do dia: quem chama `run` já está sob a trava da fachada (o
// escritor único do `WorldState`). Por isso nenhuma função daqui trava nada
// além da própria fila de comandos.

#include "ai_phase.h"

#include "../actions.h"
#include "../balance.h"
#include "../bank.h"
#include "../crisis.h"
#include "../efficiency.h"
#include "../profiling.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

namespace ogs {
namespace {

// Cadência por cidade (== `interval_days` do economic_ai.gd, agora no estado
// `cadence` do mundo — A6): a cidade de índice `i` planeja quando
// `dia % período == i % período`. Período 0/1 = todas as cidades todo dia.

// Retira da fila SÓ o comando que a IA acabou de emitir. `false` = ele já não
// estava pendente (outro aplicador o levou); nesse caso a ação NÃO é aplicada
// aqui, para não duplicar o efeito.
bool take_own(CommandQueue& queue, uint64_t seq, Command& out) {
	return queue.take_by_seq(seq, out);
}

// Massa de terra da cidade no cache do dia (-1 = não resolveu). A obra entra
// no recorte regional do `pending_committed` (R3/Q-014: comida).
int city_region(const AiContext& ctx, const std::string& city_id) {
	for (const City& city : ctx.ws.cities) {
		if (city.id == city_id) {
			return ai_risk::region_slot(ctx.state.brain_supply, city.landmass);
		}
	}
	return -1;
}

// D-125 — MODOS DA REFINARIA. A IA só troca o modo quando o ganho de receita
// por lote cobre a produção perdida na transição (`ai.*` do
// efficiency.json): ganho relativo >= `mode_min_gain`, cooldown entre trocas e
// payback de `mode_payback_days` dias. Só leitura da view/preços do mercado da
// cidade; a mutação vai pelo comando A3 `set_refinery_mode`.
const char* refinery_mode_name(int mode) {
	return mode == 1 ? "diesel" : mode == 2 ? "jet" : "standard";
}

void run_refinery_modes(AiContext& ctx) {
	WorldState& ws = ctx.ws;
	const EfficiencyParams& params = ws.efficiency;
	const int oil = ctx.core.product_index(std::string("oil"));
	const int products[3] = {params.refinery_gasoline,
		params.refinery_diesel, params.refinery_jet};
	if (oil < 0 || products[0] < 0 || products[1] < 0 || products[2] < 0) {
		return;  // catálogo sem os três graus: a regra fica inerte
	}
	for (std::size_t ci = 0; ci < ws.cities.size(); ++ci) {
		City& city = ws.cities[ci];
		if (ci >= ws.buildings_by_city.size()) {
			continue;
		}
		for (const int bi : ws.buildings_by_city[ci]) {
			Building& building = ws.buildings[static_cast<std::size_t>(bi)];
			if (!eff::is_refinery(ws, building)
					|| building.transition_days > 0) {
				continue;
			}
			const auto seen = ctx.state.last_refinery_mode_day.find(building.id);
			if (seen != ctx.state.last_refinery_mode_day.end()
					&& ctx.day - seen->second < params.ai_mode_cooldown_days) {
				continue;
			}
			// Receita por tonelada de petróleo em cada modo, com os preços de
			// HOJE do mercado da cidade. Sem preço para um grau, não decide.
			double revenue[3] = {0.0, 0.0, 0.0};
			bool priced = true;
			for (int mode = 0; mode < 3 && priced; ++mode) {
				for (int slot = 0; slot < 3; ++slot) {
					const double price = city.markets[
						static_cast<std::size_t>(products[slot])].price;
					if (!(price > 0.0)) {
						priced = false;
						break;
					}
					revenue[mode] += params.mode_shares[mode][slot] * price;
				}
			}
			if (!priced) {
				continue;
			}
			int best = 0;
			for (int mode = 1; mode < 3; ++mode) {
				if (revenue[mode] > revenue[best]) {
					best = mode;
				}
			}
			const int current = std::clamp(building.refinery_mode, 0, 2);
			if (best == current) {
				continue;
			}
			const double gain = revenue[best] - revenue[current];
			const double base = std::max(revenue[current], 1e-9);
			if (gain <= 0.0 || gain / base < params.ai_mode_min_gain) {
				continue;
			}
			const double oil_price = city.markets[
				static_cast<std::size_t>(oil)].price;
			if (!(oil_price > 0.0)) {
				continue;
			}
			// Produção efetiva (t/dia do produto principal) convertida em lotes
			// de petróleo pela fração do modo corrente.
			const double main_share = std::max(0.01,
				params.mode_shares[current][0]);
			const double batch = std::max(0.0, building.capacity)
				* std::clamp(building.production_efficiency, 0.0, 1.0)
				/ main_share;
			const double daily_gain = gain * batch;
			const double current_margin = std::max(0.0,
				(revenue[current] - oil_price) * batch);
			const double lost = current_margin
				* std::max(0, params.transition_mode_days);
			if (daily_gain * params.ai_mode_payback_days < lost) {
				continue;
			}
			const AiActionResult result = ai_emit_refinery_mode(ctx, building.city,
				building.id, refinery_mode_name(best));
			if (result.created) {
				ctx.state.last_refinery_mode_day[building.id] = ctx.day;
			}
		}
	}
}

} // namespace

AiActionResult ai_emit_invest(AiContext& ctx, const std::string& city,
		const std::string& building, double amount) {
	Command cmd;
	cmd.type = CommandType::Invest;
	cmd.day = ctx.day;
	cmd.s[0] = city;
	cmd.s[1] = building;
	cmd.v[0] = amount;
	const uint64_t seq = ctx.queue.push(cmd);

	AiActionResult out;
	Command taken;
	if (!take_own(ctx.queue, seq, taken)) {
		out.error = "comando perdido na fila";
		return out;
	}
	const InvestResult result = action_invest(ctx.ws, ctx.core, city, building,
		amount, ctx.instant);
	taken.apply_day = ctx.day;
	ctx.queue.log(taken);

	out.created = result.error.empty();
	out.cost = result.invested;
	out.error = result.error;
	return out;
}

AiActionResult ai_emit_deepen(AiContext& ctx, const std::string& city,
		const std::string& building) {
	Command cmd;
	cmd.type = CommandType::DeepenMine;
	cmd.day = ctx.day;
	cmd.s[0] = city;
	cmd.s[1] = building;
	const uint64_t seq = ctx.queue.push(cmd);

	AiActionResult out;
	Command taken;
	if (!take_own(ctx.queue, seq, taken)) {
		out.error = "comando perdido na fila";
		return out;
	}
	const DeepenResult result = action_deepen_mine(ctx.ws, ctx.core, city,
		building, ctx.instant);
	taken.apply_day = ctx.day;
	ctx.queue.log(taken);

	out.created = result.started;
	out.cost = result.cost;
	out.error = result.error;
	return out;
}

AiActionResult ai_emit_infra(AiContext& ctx, const std::string& city,
		int infra_type) {
	Command cmd;
	cmd.type = CommandType::InfraBuild;
	cmd.day = ctx.day;
	cmd.s[0] = city;
	cmd.i[0] = infra_type;
	const uint64_t seq = ctx.queue.push(cmd);

	AiActionResult out;
	Command taken;
	if (!take_own(ctx.queue, seq, taken)) {
		out.error = "comando perdido na fila";
		return out;
	}
	const InfraResult result = action_infra_build(ctx.ws, ctx.core, city,
		infra_type, ctx.instant);
	taken.apply_day = ctx.day;
	ctx.queue.log(taken);

	out.created = result.started;
	out.cost = result.cost;
	out.error = result.error;
	return out;
}

AiActionResult ai_emit_concede(AiContext& ctx, const std::string& city,
		const std::string& building, const std::string& group, double royalty) {
	Command cmd;
	cmd.type = CommandType::Concede;
	cmd.day = ctx.day;
	cmd.s[0] = city;
	cmd.s[1] = building;
	cmd.s[2] = group;
	cmd.v[0] = royalty;
	const uint64_t seq = ctx.queue.push(cmd);

	AiActionResult out;
	Command taken;
	if (!take_own(ctx.queue, seq, taken)) {
		out.error = "comando perdido na fila";
		return out;
	}
	const ConcedeResult result = action_concede(ctx.ws, ctx.core, city,
		building, group, royalty);
	taken.apply_day = ctx.day;
	ctx.queue.log(taken);

	out.created = result.ok;
	out.cost = result.price;
	out.error = result.error;
	out.owner = result.owner;
	return out;
}

AiActionResult ai_emit_storage_build(AiContext& ctx, const std::string& city,
		int type, int scope, double tons) {
	Command cmd;
	cmd.type = CommandType::StorageBuild;
	cmd.day = ctx.day;
	cmd.s[0] = city;
	cmd.i[0] = type;
	cmd.i[1] = scope;
	cmd.v[0] = tons;
	const uint64_t seq = ctx.queue.push(cmd);

	AiActionResult out;
	Command taken;
	if (!take_own(ctx.queue, seq, taken)) {
		out.error = "comando perdido na fila";
		return out;
	}
	const StorageBuildResult result = action_storage_build(ctx.ws, ctx.core,
		city, type, scope, tons, ctx.instant);
	taken.apply_day = ctx.day;
	ctx.queue.log(taken);

	out.created = result.started;
	out.cost = result.cost;
	out.error = result.error;
	return out;
}

AiActionResult ai_emit_storage_trade(AiContext& ctx, const std::string& city,
		int type, int scope, int product, double tons, bool buy, bool relief) {
	Command cmd;
	cmd.type = CommandType::StorageTrade;
	cmd.day = ctx.day;
	cmd.s[0] = city;
	cmd.s[1] = relief ? "relief" : "paid";
	cmd.i[0] = type;
	cmd.i[1] = scope;
	cmd.v[0] = tons;
	cmd.v[1] = static_cast<double>(product);
	cmd.b = buy;
	const uint64_t seq = ctx.queue.push(cmd);

	AiActionResult out;
	Command taken;
	if (!take_own(ctx.queue, seq, taken)) {
		out.error = "comando perdido na fila";
		return out;
	}
	const StorageTradeResult result = action_storage_trade(ctx.ws, ctx.core,
		city, type, scope, product, tons, buy, relief);
	taken.apply_day = ctx.day;
	ctx.queue.log(taken);

	out.created = result.ok;
	out.cost = result.value;
	out.error = result.error;
	return out;
}

AiActionResult ai_emit_build(AiContext& ctx, const std::string& city,
		const std::string& building, const std::string& resource, int input,
		int output, double capacity, double jobs, double wage, double opex,
		double input_per_ratio, const std::string& owner, double cost,
		const std::string& recipe_id) {
	Command cmd;
	cmd.type = CommandType::Build;
	cmd.day = ctx.day;
	cmd.s[0] = city;
	cmd.s[1] = building;
	cmd.s[2] = resource;
	cmd.s[3] = owner;
	cmd.s[4] = recipe_id;
	cmd.i[0] = input;
	cmd.i[1] = output;
	cmd.v[0] = capacity;
	cmd.v[1] = jobs;
	cmd.v[2] = wage;
	cmd.v[3] = opex;
	cmd.v[4] = input_per_ratio;
	cmd.v[5] = cost;
	const uint64_t seq = ctx.queue.push(cmd);

	AiActionResult out;
	Command taken;
	if (!take_own(ctx.queue, seq, taken)) {
		out.error = "comando perdido na fila";
		return out;
	}
	const BuildResult result = action_build(ctx.ws, ctx.core, city, building,
		resource, input, output, capacity, jobs, wage, opex, input_per_ratio,
		owner, cost, recipe_id, ctx.instant, ctx.bootstrap);
	taken.apply_day = ctx.day;
	ctx.queue.log(taken);

	out.created = result.created;
	out.cost = result.cost;
	out.error = result.error;
	out.owner = owner;
	// D-131 fase B: a obra emitida nesta fase entra na concorrência
	// (`pending_committed`), na janela anti-manada e na exposição do dono.
	if (ctx.ws.ai_brain.enabled && result.created) {
		ai_risk::pending_add(ctx.state, city_region(ctx, city), output,
			std::max(0.0, capacity));
		ai_risk::herd_record(ctx.state, output, capacity, ctx.day);
		ai_risk::exposure_add(ctx.state.brain_exposure, city, owner, output,
			result.cost);
	}
	return out;
}

// C3 (D-107): pedido de crédito do grupo (comando `group_borrow`).
// s0 grupo, v0 valor pedido. `cost` = concedido.
AiActionResult ai_emit_group_borrow(AiContext& ctx, const std::string& group,
		double amount) {
	Command cmd;
	cmd.type = CommandType::GroupBorrow;
	cmd.day = ctx.day;
	cmd.s[0] = group;
	cmd.v[0] = amount;
	const uint64_t seq = ctx.queue.push(cmd);

	AiActionResult out;
	Command taken;
	if (!take_own(ctx.queue, seq, taken)) {
		out.error = "comando perdido na fila";
		return out;
	}
	double granted = 0.0;
	std::string error;
	out.created = bank::lend(ctx.ws, group, amount, &granted, &error);
	// D-131: a IA roda depois do tick; o M1 do empréstimo de grupo entra no
	// acumulado auditável (não passa por `bank_money_today`).
	if (granted > 0.0) {
		ctx.ws.bank_money_delta_total += granted;
	}
	taken.apply_day = ctx.day;
	ctx.queue.log(taken);
	out.cost = granted;
	out.cash = granted;
	out.error = error;
	return out;
}

AiActionResult ai_emit_city_borrow(AiContext& ctx, const std::string& city,
		double amount) {
	Command cmd;
	cmd.type = CommandType::CityBorrow;
	cmd.day = ctx.day;
	cmd.s[0] = city;
	cmd.v[0] = amount;
	const uint64_t seq = ctx.queue.push(cmd);

	AiActionResult out;
	Command taken;
	if (!take_own(ctx.queue, seq, taken)) {
		out.error = "comando perdido na fila";
		return out;
	}
	double granted = 0.0;
	std::string error;
	out.created = crisis::borrow_city(ctx.ws, city, amount, &granted, &error);
	taken.apply_day = ctx.day;
	ctx.queue.log(taken);
	out.cost = granted;
	out.cash = granted;
	out.error = error;
	return out;
}

AiActionResult ai_emit_group_invest(AiContext& ctx, const std::string& group,
		const std::string& city, const std::string& building,
		const std::string& resource, int output, double capacity,
		const std::string& recipe_id, const std::string& partner) {
	Command cmd;
	cmd.type = CommandType::GroupInvest;
	cmd.day = ctx.day;
	cmd.s[0] = group;
	cmd.s[1] = city;
	cmd.s[2] = building;
	cmd.s[3] = resource;
	cmd.s[4] = recipe_id;
	cmd.s[5] = partner;
	cmd.i[0] = output;
	cmd.v[0] = capacity;
	const uint64_t seq = ctx.queue.push(cmd);

	AiActionResult out;
	Command taken;
	if (!take_own(ctx.queue, seq, taken)) {
		out.error = "comando perdido na fila";
		return out;
	}
	const GroupInvestResult result = action_group_invest(ctx.ws, ctx.core,
		group, city, building, resource, output, capacity, recipe_id, partner,
		ctx.instant);
	taken.apply_day = ctx.day;
	ctx.queue.log(taken);

	out.created = result.created;
	out.cost = result.cost;
	out.cash = result.cash;
	out.local_share = result.local_share;
	out.error = result.error;
	out.owner = result.owner;
	if (ctx.ws.ai_brain.enabled && result.created) {
		ai_risk::pending_add(ctx.state, city_region(ctx, city), output,
			std::max(0.0, capacity));
		ai_risk::herd_record(ctx.state, output, capacity, ctx.day);
		ai_risk::exposure_add(ctx.state.brain_exposure, city,
			result.owner.empty() ? group : result.owner, output, result.cost);
	}
	return out;
}

// D-125: troca de modo da refinaria. A decisão é da IA, mas a mutação vem
// pelo mesmo caminho A3 das demais (emite, retira só o próprio comando e
// aplica no escritor único).
AiActionResult ai_emit_refinery_mode(AiContext& ctx, const std::string& city,
		const std::string& building, const std::string& mode) {
	Command cmd;
	cmd.type = CommandType::SetRefineryMode;
	cmd.day = ctx.day;
	cmd.s[0] = city;
	cmd.s[1] = building;
	cmd.s[2] = mode;
	const uint64_t seq = ctx.queue.push(cmd);

	AiActionResult out;
	Command taken;
	if (!take_own(ctx.queue, seq, taken)) {
		out.error = "comando perdido na fila";
		return out;
	}
	const RefineryModeResult result = action_set_refinery_mode(ctx.ws, ctx.core,
		city, building, mode);
	taken.apply_day = ctx.day;
	ctx.queue.log(taken);

	out.created = result.changed;
	out.error = result.error;
	return out;
}

void AiPhase::reset() {
	// `templates_products_ = -1` força `build_ai_templates` de novo no próximo
	// `run` (o catálogo de produtos do mundo novo pode ter outro tamanho).
	state_ = AiState{};
	view_ = AiView{};
	templates_ = AiTemplates{};
	templates_products_ = -1;
}

void AiPhase::run(WorldState& ws, SimCore& core, CommandQueue& queue,
		bool instant, bool bootstrap) {
	// `day` é o dia JÁ fechado (a fachada passa depois do tick): todo comando
	// emitido aqui carrega este dia.
	const int day = ws.day;

	// 1. Retrato do dia. A partir daqui as regras só enxergam `view_`.
	{
		OGS_SIM_ZONE(kPhaseAiView);
		snapshot_ai_view(ws, core, view_);
	}

	// 2. Templates de engenharia: construídos uma vez, refeitos só quando o
	// catálogo de produtos muda de tamanho.
	if (templates_products_ != view_.products) {
		build_ai_templates(ws, templates_);
		templates_products_ = view_.products;
	}

	// 3. Visões locais de caixa dos grupos (cada investidor debita a própria
	// cópia ao longo do dia; o mundo só muda na aplicação do comando).
	state_.private_group_cash.resize(view_.groups.size());
	state_.group_cash.resize(view_.groups.size());
	state_.group_credit.assign(view_.groups.size(), 0.0);
	for (std::size_t index = 0; index < view_.groups.size(); ++index) {
		state_.private_group_cash[index] = view_.groups[index].cash;
		state_.group_cash[index] = view_.groups[index].cash;
	}
	// C3 (D-107): limite de crédito do dia por grupo (uma passagem pelo
	// estado). Desligado, `credit_limit` devolve 0 e nada muda no mundo.
	if (ws.bank_credit_enabled) {
		const std::vector<ogs::balance::GroupSheet> sheets =
			ogs::balance::group_sheets(ws);
		for (std::size_t index = 0; index < view_.groups.size()
				&& index < sheets.size(); ++index) {
			state_.group_credit[index] = ogs::bank::credit_limit(ws,
				sheets[index]);
		}
	}

	// 4. Contexto do dia: tudo por referência, nada copiado.
	AiContext ctx{ws, core, queue, templates_, view_, state_, day, instant,
		bootstrap};

	// 4b. Medidas do dia que não mudam entre cidades (== `prepare_totals` no
	// `plan()` do GDScript): balanço alimentar mundo/região, capacidade em obras
	// por produto e o caixa local dos grupos. Uma vez por dia, ANTES das
	// cidades — `run_city` do privado não remede.
	private_investor_.prepare(ctx);

	// 4c. Grupos (F9): a decisão do dia vem ANTES das cidades e na ordem do
	// ranking, como no `plan()` do `economic_ai.gd` (grupos → cidades).
	group_investor_.run(ctx);

	// 4d. D-125: modo das refinarias existentes. Roda em toda cidade (as
	// refinarias do ano-base já estão no mundo) e só troca quando o ganho paga
	// a transição; o comando entra no mesmo log A3.
	run_refinery_modes(ctx);

	// 5. Agenda: `dia % período` escolhe as cidades da vez (A6: período no
	// estado `cadence` do mundo; antes era o literal 7). Cenário menor que um
	// ciclo usa os dias 0..n-1 e não faz nada nos demais
	// (== `phase >= mini(_city_count, interval_days)` do GDScript).
	const int interval = std::max(1, ws.cadence.weekly_period);
	const int phase = day % interval;
	if (view_.cities > 0 && phase >= std::min(view_.cities, interval)) {
		return;
	}
	for (int index = phase; index < view_.cities; index += interval) {
		// Ordem do dia por cidade: privado, governo e, por último, armazém
		// (compra/liberação e obra). O armazém vem depois para não competir
		// com as decisões de produção pelo caixa congelado da view.
		private_investor_.run_city(ctx, index);
		government_.run_city(ctx, index);
		storage_investor_.run_city(ctx, index);
	}
}

} // namespace ogs
