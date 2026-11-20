// C3 (D-107) — crédito de empresa pelo banco agregado. Contrato em bank.h e
// desenho em docs/economy/C3_group_balance.md. Nada aqui inventa número: as
// taxas vêm da política monetária da moeda (C2b1) e os parâmetros de regra do
// JSON declarado (default de jogo).

#include "bank.h"

#include "balance.h"
#include "monetary.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace ogs {
namespace bank {

namespace {

Policy effective_policy(const WorldState& ws) {
	Policy out;
	out.enabled = ws.bank_credit_enabled;
	out.spread = ws.bank_spread > 0.0 ? ws.bank_spread : kSpreadDefault;
	out.leverage_cap = ws.bank_leverage_cap > 0.0 ? ws.bank_leverage_cap
		: kLeverageCapDefault;
	out.interest_cover = ws.bank_interest_cover > 0.0
		? ws.bank_interest_cover : kInterestCoverDefault;
	out.default_days = ws.bank_default_days > 0 ? ws.bank_default_days
		: kDefaultDaysDefault;
	out.repay_share = ws.bank_repay_share > 0.0 ? ws.bank_repay_share
		: kRepayShareDefault;
	return out;
}

// Maior cidade da nação (o "tesouro nacional" do modelo): o orçamento dela
// paga o capital do banco, como a cidade do cofre paga a compra de ouro.
City* treasury_city(WorldState& ws, const std::string& nation) {
	City* best = nullptr;
	for (City& city : ws.cities) {
		if (city.nation != nation) {
			continue;
		}
		if (best == nullptr
				|| city.simulation_population > best->simulation_population) {
			best = &city;
		}
	}
	return best;
}

} // namespace

Policy policy(const WorldState& ws) { return effective_policy(ws); }

const std::string& currency_of_nation(const WorldState& ws,
		const std::string& nation) {
	static const std::string kEmpty;
	for (const Currency& currency : ws.currencies) {
		for (const std::string& member : currency.nations) {
			if (member == nation) {
				return currency.code;
			}
		}
	}
	return kEmpty;
}

double loan_rate(const WorldState& ws, const std::string& nation) {
	const Policy pol = effective_policy(ws);
	return monetary::rate_for_nation(ws, nation) + pol.spread;
}

Bank& ensure(WorldState& ws, const std::string& nation) {
	Bank& bank = ws.banks[nation];
	if (bank.nation.empty()) {
		bank.nation = nation;
		bank.currency = currency_of_nation(ws, nation);
	}
	return bank;
}

const Bank* find(const WorldState& ws, const std::string& nation) {
	const auto found = ws.banks.find(nation);
	return found == ws.banks.end() ? nullptr : &found->second;
}

double equity(const Bank& bank) {
	return bank.cash + bank.loans + bank.arrears - bank.bc_borrowing
		+ bank.capital;
}

double credit_limit(const WorldState& ws, const balance::GroupSheet& sheet,
		double rate) {
	const Policy pol = effective_policy(ws);
	if (!pol.enabled) {
		return 0.0;
	}
	const double loan = rate > 0.0 ? rate
		: loan_rate(ws, sheet.origin_nation);
	// 1. Alavancagem: dívida ≤ leverage_cap × patrimônio líquido do grupo.
	const double by_leverage = pol.leverage_cap
		* std::max(0.0, sheet.net_worth());
	// 2. Cobertura de juros: o lucro anual medido tem de cobrir o juro anual
	// por `interest_cover`. Juro maior encurta o crédito — é o canal pelo qual
	// "alavancagem responde ao juro".
	const double annual_flow = std::max(0.0, sheet.daily_profit)
		* kDaysPerYear;
	const double by_cover = loan > 1e-9
		? annual_flow / (loan * pol.interest_cover) : by_leverage;
	const double limit = std::min(by_leverage, by_cover) - sheet.debt
		- sheet.payables;
	return std::max(0.0, limit);
}

double credit_limit(const WorldState& ws, const Group& group, double rate) {
	if (!effective_policy(ws).enabled) {
		return 0.0;
	}
	return credit_limit(ws, balance::group_sheet(ws, group), rate);
}

bool lend(WorldState& ws, const std::string& group_id, double amount,
		double* out_amount, std::string* error) {
	if (out_amount != nullptr) {
		*out_amount = 0.0;
	}
	const Policy pol = effective_policy(ws);
	if (!pol.enabled) {
		if (error != nullptr) {
			*error = "credito desligado";
		}
		return false;
	}
	if (!(amount > 0.0) || !std::isfinite(amount)) {
		if (error != nullptr) {
			*error = "valor invalido";
		}
		return false;
	}
	Group* group = nullptr;
	for (Group& row : ws.groups) {
		if (row.id == group_id) {
			group = &row;
			break;
		}
	}
	if (group == nullptr) {
		if (error != nullptr) {
			*error = "grupo desconhecido";
		}
		return false;
	}
	const double limit = credit_limit(ws, *group);
	if (limit <= 1e-9) {
		if (error != nullptr) {
			*error = "sem limite de credito";
		}
		return false;
	}
	const double granted = std::min(amount, limit);
	Bank& bank = ensure(ws, group->origin_nation);
	// M0 criado pelo BC contra o banco: os dois lados do balanço da C1.
	bank.loans += granted;
	bank.bc_borrowing += granted;
	bank.loans_count += 1;
	group->cash += granted;
	group->debt += granted;
	group->credit_total += granted;
	ws.emission_today += granted;
	if (out_amount != nullptr) {
		*out_amount = granted;
	}
	return true;
}

double inject_capital(WorldState& ws, const std::string& nation, double amount,
		double* out_paid, std::string* error) {
	if (out_paid != nullptr) {
		*out_paid = 0.0;
	}
	if (!(amount > 0.0) || !std::isfinite(amount)) {
		if (error != nullptr) {
			*error = "valor invalido";
		}
		return 0.0;
	}
	City* city = treasury_city(ws, nation);
	if (city == nullptr) {
		if (error != nullptr) {
			*error = "nacao sem cidade";
		}
		return 0.0;
	}
	const double paid = std::min(amount, std::max(0.0, city->government_budget));
	if (paid <= 0.0) {
		if (error != nullptr) {
			*error = "orcamento insuficiente";
		}
		return 0.0;
	}
	// Transferência conservada: o Tesouro paga e vira dono do capital.
	city->government_budget -= paid;
	Bank& bank = ensure(ws, nation);
	bank.cash += paid;
	bank.capital += paid;
	if (out_paid != nullptr) {
		*out_paid = paid;
	}
	return paid;
}

int phase(WorldState& ws) {
	// Sem crédito e sem dívida o dia não muda: o mundo default é o mesmo.
	bool any = false;
	for (const auto& row : ws.banks) {
		if (row.second.loans > 0.0 || row.second.arrears > 0.0
				|| row.second.bc_borrowing > 0.0
				|| std::abs(row.second.cash) > 1e-9) {
			any = true;
			break;
		}
	}
	if (!any) {
		return 0;
	}
	const Policy pol = effective_policy(ws);
	const std::vector<balance::GroupSheet> sheets = balance::group_sheets(ws);
	int defaults = 0;
	for (auto& row : ws.banks) {
		Bank& bank = row.second;
		if (bank.loans <= 0.0 && bank.arrears <= 0.0
				&& bank.bc_borrowing <= 0.0) {
			continue;
		}
		const double rate = loan_rate(ws, bank.nation);
		const double base_rate = monetary::rate_for_nation(ws, bank.nation);
		for (std::size_t index = 0; index < ws.groups.size(); ++index) {
			Group& group = ws.groups[index];
			if (group.origin_nation != bank.nation) {
				continue;
			}
			if (group.debt <= 0.0 && group.credit_arrears <= 0.0) {
				group.credit_default_days = 0;
				continue;
			}
			// 1. Juro do dia. O que o caixa não cobre vira juro VENCIDO dos
			// dois lados (`credit_arrears`/`arrears`) — não é capitalizado.
			const double interest = group.debt * rate / kDaysPerYear;
			const double paid_interest = std::min(std::max(0.0, group.cash),
				std::max(0.0, interest));
			if (paid_interest > 0.0) {
				group.cash -= paid_interest;
				bank.interest_income += paid_interest;
				// O banco repassa a parcela da taxa básica ao BC, abatendo as
				// reservas: esse M0 é destruído (registrado na emissão do dia).
				const double base_part = std::min(paid_interest,
					bank.bc_borrowing * base_rate / kDaysPerYear);
				if (base_part > 0.0) {
					bank.bc_borrowing -= base_part;
					bank.interest_expense += base_part;
					ws.emission_today -= base_part;
				}
				// O resto (o spread) fica no caixa do banco.
				bank.cash += paid_interest - base_part;
			}
			const double unpaid = std::max(0.0, interest - paid_interest);
			if (unpaid > 0.0) {
				group.credit_arrears += unpaid;
				bank.arrears += unpaid;
			}
			// 2. Amortização: só o excedente ao capital de giro (30 dias de
			// custo efetivamente pago) e na fração declarada.
			const double working = kWorkingCapitalDays
				* std::max(0.0, sheets[index].daily_cost);
			const double excess = std::max(0.0, group.cash - working);
			double repay = std::min(group.debt, pol.repay_share * excess);
			repay = std::min(repay, bank.bc_borrowing);
			if (repay > 0.0) {
				group.cash -= repay;
				group.debt -= repay;
				group.credit_repaid += repay;
				bank.loans -= repay;
				bank.bc_borrowing -= repay;
				// Pagar o principal DESTRÓI o M0 que o empréstimo criou.
				ws.emission_today -= repay;
			}
			// 3. Insolvência: ativos reais + caixa não cobrem a dívida. Conta
			// dias e, passado o prazo, o credor executa.
			if (sheets[index].net_worth() < 0.0) {
				group.credit_default_days += 1;
			} else {
				group.credit_default_days = 0;
			}
			if (group.credit_default_days < pol.default_days) {
				continue;
			}
			const double claim = group.debt + group.credit_arrears;
			const double seized = std::min(std::max(0.0, group.cash), claim);
			if (seized > 0.0) {
				group.cash -= seized;
				// O caixa tomado abate a dívida do banco com o BC (M0
				// destruído, registrado na emissão do dia).
				const double applied = std::min(seized, bank.bc_borrowing);
				bank.bc_borrowing -= applied;
				ws.emission_today -= applied;
				bank.cash += seized - applied;
			}
			const double writeoff = std::max(0.0, claim - seized);
			bank.loans = std::max(0.0, bank.loans - group.debt);
			bank.arrears = std::max(0.0, bank.arrears - group.credit_arrears);
			bank.writeoffs += writeoff;
			bank.defaults += 1;
			bank.last_default_day = ws.day;
			group.debt = 0.0;
			group.credit_arrears = 0.0;
			group.credit_default_days = 0;
			group.credit_defaults += 1;
			// Execução: as construções do grupo fecham (a mesma regra de
			// falência das firmas; o atrasado delas é baixado no caminho
			// existente, dos dois lados, como a C1 já faz).
			for (Building& building : ws.buildings) {
				if (building.bankrupt) {
					continue;
				}
				const std::string national = "national:" + group.id;
				const std::string foreign = "foreign:" + group.id + "@";
				if (building.owner == national
						|| building.owner.rfind(foreign, 0) == 0) {
					building.bankrupt = true;
				}
			}
			defaults += 1;
		}
	}
	return defaults;
}

Totals totals(const WorldState& ws) {
	Totals out;
	for (const auto& row : ws.banks) {
		const Bank& bank = row.second;
		out.loans += bank.loans;
		out.arrears += bank.arrears;
		out.bc_borrowing += bank.bc_borrowing;
		out.cash += bank.cash;
		out.capital += bank.capital;
		out.writeoffs += bank.writeoffs;
		out.interest_income += bank.interest_income;
		out.interest_expense += bank.interest_expense;
		out.defaults += bank.defaults;
		out.loans_count += bank.loans_count;
		out.equity += equity(bank);
	}
	return out;
}

double total_group_debt(const WorldState& ws) {
	double total = 0.0;
	for (const Group& group : ws.groups) {
		total += group.debt;
	}
	return total;
}

double total_group_arrears(const WorldState& ws) {
	double total = 0.0;
	for (const Group& group : ws.groups) {
		total += group.credit_arrears;
	}
	return total;
}

} // namespace bank
} // namespace ogs
