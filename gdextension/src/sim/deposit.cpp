// R2b1 - implementacao do deposito liquido (docs/economy/rework/R2_plano.md
// 4.3/4.4). Tudo aqui e' caixa <-> claim do MESMO banco: nenhuma soma muda
// `money_total` e `S claims == S D_f` continua exato.

#include "deposit.h"

#include "bank.h"
#include "world_state.h"

#include <algorithm>
#include <cmath>

namespace ogs {
namespace deposit {

namespace {

constexpr double kEps = 1e-9;

bool active(const WorldState& ws) {
	return ws.banks_rework.enabled && ws.banks_rework.deposits;
}

const Bank* find_bank(const WorldState& ws, const std::string& id) {
	if (id.empty()) {
		return nullptr;
	}
	const auto found = ws.banks.find(id);
	return found == ws.banks.end() ? nullptr : &found->second;
}

Bank* find_bank_mut(WorldState& ws, const std::string& id) {
	if (id.empty()) {
		return nullptr;
	}
	const auto found = ws.banks.find(id);
	return found == ws.banks.end() ? nullptr : &found->second;
}

// Parcela do claim coberta pelo caixa do banco, pro rata entre TODOS os
// depositantes de funding (o mesmo `bank.cash` lastreia D_f).
double covered(const Bank& bank, double claim) {
	if (!(claim > kEps)) {
		return 0.0;
	}
	const double funding = std::max(0.0, bank.deposits_funding);
	if (funding <= kEps) {
		return 0.0;
	}
	const double cash = std::max(0.0, bank.cash);
	return std::min(claim, cash * std::min(1.0, claim / funding));
}

} // namespace

void link(WorldState& ws) {
	for (City& city : ws.cities) {
		city.ws_link = &ws;
	}
	for (Group& group : ws.groups) {
		group.ws_link = &ws;
	}
}

double liquid_city(const WorldState& ws, const City& city, bool firm) {
	if (!active(ws)) {
		return 0.0;
	}
	double total = 0.0;
	for (const City::BankAccount& account : city.bank_accounts) {
		const Bank* bank = find_bank(ws, account.bank_id);
		if (bank == nullptr) {
			continue;
		}
		total += covered(*bank, firm ? account.firm : account.service);
	}
	return std::max(0.0, total);
}

double liquid_group(const WorldState& ws, const Group& group) {
	if (!active(ws) || !(group.bank_deposit > kEps)) {
		return 0.0;
	}
	const Bank* bank = find_bank(ws, group.deposit_bank_id);
	if (bank == nullptr) {
		return 0.0;
	}
	return covered(*bank, group.bank_deposit);
}

double withdraw_city(WorldState& ws, City& city, bool firm, double want) {
	if (!active(ws) || !(want > kEps)) {
		return 0.0;
	}
	double left = want;
	double paid = 0.0;
	for (City::BankAccount& account : city.bank_accounts) {
		if (left <= kEps) {
			break;
		}
		Bank* bank = find_bank_mut(ws, account.bank_id);
		if (bank == nullptr) {
			continue;
		}
		double& claim = firm ? account.firm : account.service;
		const double x = std::min({ left, std::max(0.0, claim),
			covered(*bank, claim) });
		if (!(x > kEps)) {
			continue;
		}
		claim -= x;
		if (claim < 1e-9) {
			claim = 0.0;
		}
		bank->cash = std::max(0.0, bank->cash - x);
		bank->deposits_funding = std::max(0.0,
			bank->deposits_funding - x);
		if (firm) {
			city.firm_sub += x;
			city.firm_cash = city.firm_sub;
		} else {
			city.service_sub += x;
			city.service_cash = city.service_sub;
		}
		city.private_cash += x;
		left -= x;
		paid += x;
	}
	return paid;
}

double withdraw_group(WorldState& ws, Group& group, double want) {
	if (!active(ws) || !(want > kEps) || !(group.bank_deposit > kEps)) {
		return 0.0;
	}
	Bank* bank = find_bank_mut(ws, group.deposit_bank_id);
	if (bank == nullptr) {
		return 0.0;
	}
	const double x = std::min({ std::max(0.0, want),
		std::max(0.0, group.bank_deposit), covered(*bank, group.bank_deposit) });
	if (!(x > kEps)) {
		return 0.0;
	}
	group.bank_deposit -= x;
	if (group.bank_deposit < 1e-9) {
		group.bank_deposit = 0.0;
	}
	bank->cash = std::max(0.0, bank->cash - x);
	bank->deposits_funding = std::max(0.0, bank->deposits_funding - x);
	group.cash += x;
	return x;
}

double pay_group(WorldState& ws, Group& group, double value,
		double* from_cash, double* from_claim) {
	if (from_cash != nullptr) {
		*from_cash = 0.0;
	}
	if (from_claim != nullptr) {
		*from_claim = 0.0;
	}
	if (!(value > 0.0)) {
		return 0.0;
	}
	const double cash_part = std::min(value, std::max(0.0, group.cash));
	group.cash -= cash_part;
	double claim_part = 0.0;
	if (value - cash_part > kEps) {
		claim_part = withdraw_group(ws, group, value - cash_part);
		// withdraw_group credita o caixa do pagador; o pagamento debita.
		group.cash = std::max(0.0, group.cash - claim_part);
	}
	if (from_cash != nullptr) {
		*from_cash = cash_part;
	}
	if (from_claim != nullptr) {
		*from_claim = claim_part;
	}
	return cash_part + claim_part;
}

} // namespace deposit
} // namespace ogs
