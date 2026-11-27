// R1c (QA t177) - alavancas A3 explicitas sob `firms_own_cash`: o dinheiro sai
// do Tesouro Nacional, nunca do orcamento regional (limite, nao caixa). Ver o
// contrato em state_levers.h e a doc em docs/economy/rework/R1.md (R1c-D3).
#include "state_levers.h"

#include "sim_core.h"
#include "treasury.h"
#include "world_state.h"

#include <algorithm>
#include <cstddef>
#include <string>

namespace ogs {

namespace {

constexpr double kBalanceEpsilon = 1e-9;

} // namespace

bool treasury_levers_enabled(const WorldState& ws) {
	return ws.firms_own_cash && treasury::enabled(ws);
}

TreasuryFundResult action_fund_group_from_treasury(WorldState& ws,
		const std::string& group_id, double amount) {
	TreasuryFundResult result;
	if (!treasury_levers_enabled(ws)) {
		result.error = "Tesouro indisponivel (flags do rework invalidas)";
		return result;
	}
	const auto found = ws.group_lookup.find(group_id);
	if (found == ws.group_lookup.end() || found->second < 0
			|| found->second >= static_cast<int>(ws.groups.size())) {
		result.error = "grupo desconhecido";
		return result;
	}
	if (amount <= 0.0) {
		result.error = "valor invalido";
		return result;
	}
	Group& group = ws.groups[static_cast<std::size_t>(found->second)];
	// Sem debito parcial: ou o Tesouro cobre o valor inteiro ou o comando falha
	// (o chamador nao deve ver um grupo financiado pela metade).
	if (treasury::balance(ws, group.origin_nation) + kBalanceEpsilon
			< amount) {
		result.error = "saldo insuficiente do Tesouro";
		return result;
	}
	const double paid = treasury::spend(ws, group.origin_nation, amount);
	if (paid + kBalanceEpsilon < amount) {
		// Defensivo: `spend` devolve o que o saldo permitia. Nao credita
		// parcial para o comando falhar limpo como prometido.
		result.error = "saldo insuficiente do Tesouro";
		return result;
	}
	group.cash += paid;
	result.moved = paid;
	result.funded = true;
	return result;
}

TreasuryCompanyResult action_create_state_company_from_treasury(
		WorldState& ws, const std::string& nation, const std::string& name,
		double capital) {
	TreasuryCompanyResult result;
	if (!treasury_levers_enabled(ws)) {
		result.error = "Tesouro indisponivel (flags do rework invalidas)";
		return result;
	}
	if (nation.empty()) {
		result.error = "nacao vazia";
		return result;
	}
	const double requested = std::max(0.0, capital);
	// Valida ANTES de criar o grupo: falha sem efeito colateral.
	if (treasury::balance(ws, nation) + kBalanceEpsilon < requested) {
		result.error = "saldo insuficiente do Tesouro";
		return result;
	}
	Group group;
	group.state_owned = true;
	group.origin_nation = nation;
	group.sector = "state";
	group.name = name;
	int serial = 0;
	do {
		group.id = "S-" + sane_token(nation) + "-" + std::to_string(serial);
		++serial;
	} while (ws.group_lookup.find(group.id) != ws.group_lookup.end());
	ws.group_lookup[group.id] = static_cast<int>(ws.groups.size());
	ws.groups.push_back(group);
	Group* created = &ws.groups.back();
	const double paid = treasury::spend(ws, nation, requested);
	created->cash += paid;
	result.created = true;
	result.id = created->id;
	result.capital = created->cash;
	if (created->cash <= 0.0) {
		result.warning = "nenhum orcamento disponivel na nacao";
	}
	return result;
}

} // namespace ogs
