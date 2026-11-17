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
#include "../profiling.h"

#include <algorithm>
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
	for (std::size_t index = 0; index < view_.groups.size(); ++index) {
		state_.private_group_cash[index] = view_.groups[index].cash;
		state_.group_cash[index] = view_.groups[index].cash;
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
		// Ordem do dia por cidade: privado e depois governo (economic_ai.gd).
		private_investor_.run_city(ctx, index);
		government_.run_city(ctx, index);
	}
}

} // namespace ogs
