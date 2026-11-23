// C4 (D-110) — bancos com dono (público, privado nacional, estrangeiro).
// Contrato em bank.h e desenho em docs/economy/C4_banks.md. Nada aqui inventa
// número: taxas vêm da política monetária da moeda (C2b1), nomes de banks.json
// e todo parâmetro de regra é default de jogo declarado.

#include "bank.h"

#include "balance.h"
#include "bank_crisis.h"
#include "exchange.h"
#include "monetary.h"
#include "sim_constants.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
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
	out.dividend_payout = kDividendPayoutDefault;
	return out;
}

// Maior cidade da nação (o "tesouro nacional" do modelo): o orçamento dela
// paga o capital do banco público, como a cidade do cofre paga o ouro.
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

Bank* find_mut(WorldState& ws, const std::string& nation, int role) {
	const auto found = ws.banks.find(bank_id(nation, role));
	return found == ws.banks.end() ? nullptr : &found->second;
}

// FNV-1a estável: o tomador de cada grupo não pode mudar entre processos nem
// entre save/load (o hash da libc não é estável por implementação).
std::uint64_t stable_hash(const std::string& text) {
	std::uint64_t hash = 1469598103934665603ULL;
	for (const char c : text) {
		hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
		hash *= 1099511628211ULL;
	}
	return hash;
}

// Participações declaradas: com estrangeiro 0,45/0,40/0,15; sem estrangeiro
// 0,55/0,45 (0,55/0,45 do doc §3). Renormaliza entre os papéis EXISTENTES.
void renormalize(WorldState& ws, const std::string& nation) {
	Bank* pub = find_mut(ws, nation, kRolePublic);
	Bank* pri = find_mut(ws, nation, kRolePrivate);
	Bank* fgn = find_mut(ws, nation, kRoleForeign);
	// C6 (D-114): papel absorvido pelo banco publico fica como CASCA (share 0)
	// e nao volta a participar do sorteio de tomador quando as definicoes sao
	// reaplicadas depois de um save.
	const auto active = [](Bank* bank) {
		return bank != nullptr && bank->absorbed_into.empty();
	};
	if (pub != nullptr && !pub->absorbed_into.empty()) {
		pub->share = 0.0;
	}
	if (pri != nullptr && !pri->absorbed_into.empty()) {
		pri->share = 0.0;
	}
	if (fgn != nullptr && !fgn->absorbed_into.empty()) {
		fgn->share = 0.0;
	}
	const int count = (active(pub) ? 1 : 0) + (active(pri) ? 1 : 0)
		+ (active(fgn) ? 1 : 0);
	if (count == 0) {
		return;
	}
	if (count == 3) {
		pub->share = kSharePublic;
		pri->share = kSharePrivate;
		fgn->share = kShareForeign;
		return;
	}
	if (count == 1) {
		Bank* only = active(pub) ? pub : (active(pri) ? pri : fgn);
		only->share = 1.0;
		return;
	}
	if (active(pub) && active(pri)) {
		pub->share = 0.55;
		pri->share = 0.45;
		return;
	}
	// Par público+estrangeiro (não acontece na prática; private sempre existe
	// quando o par é criado): divide pelo peso declarado dos dois papéis.
	const double wp = active(pub) ? kSharePublic : 0.0;
	const double wf = active(fgn) ? kShareForeign : 0.0;
	const double total = wp + wf;
	if (total <= 0.0) {
		return;
	}
	if (active(pub)) {
		pub->share = wp / total;
	}
	if (active(fgn)) {
		fgn->share = wf / total;
	}
}

// Funde um banco no outro conservando TODOS os valores (nada some): usado
// quando o dado diz que não há banco estrangeiro na nação.
void merge_into(Bank& into, const Bank& from) {
	into.loans += from.loans;
	into.arrears += from.arrears;
	into.deposits += from.deposits;
	into.bc_borrowing += from.bc_borrowing;
	into.cash += from.cash;
	into.capital += from.capital;
	into.interest_income += from.interest_income;
	into.interest_expense += from.interest_expense;
	into.writeoffs += from.writeoffs;
	into.dividends += from.dividends;
	into.remittance += from.remittance;
	into.remittance_origin += from.remittance_origin;
	into.harvest_interest += from.harvest_interest;
	into.defaults += from.defaults;
	into.loans_count += from.loans_count;
	into.last_default_day = std::max(into.last_default_day,
		from.last_default_day);
	into.last_dividend_day = std::max(into.last_dividend_day,
		from.last_dividend_day);
	// C6 (D-114): estado de crise tambem e' conservado na fusao por definicao
	// (0 no mundo novo; so importa se o dado mudar no meio de uma crise).
	into.confidence = std::min(into.confidence, from.confidence);
	into.run_withdrawn += from.run_withdrawn;
	into.run_cash += from.run_cash;
	into.run_public += from.run_public;
	into.lolr_borrowed += from.lolr_borrowed;
	into.lolr_interest += from.lolr_interest;
	into.insured_payout += from.insured_payout;
	into.bailin_cut += from.bailin_cut;
	into.treasury_rescue += from.treasury_rescue;
	into.capital_flight += from.capital_flight;
}

} // namespace

const char* role_key(int role) {
	switch (role) {
	case kRolePublic:
		return "public";
	case kRolePrivate:
		return "private";
	case kRoleForeign:
		return "foreign";
	default:
		return "";
	}
}

int role_from_key(const std::string& key) {
	if (key == "public") {
		return kRolePublic;
	}
	if (key == "private") {
		return kRolePrivate;
	}
	if (key == "foreign") {
		return kRoleForeign;
	}
	return -1;
}

std::string bank_id(const std::string& nation, int role) {
	return nation + "/" + std::string(role_key(role));
}

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

Bank& ensure(WorldState& ws, const std::string& nation, int role) {
	const std::string id = bank_id(nation, role);
	Bank& bank = ws.banks[id];
	if (bank.id.empty()) {
		bank.id = id;
		bank.nation = nation;
		bank.currency = currency_of_nation(ws, nation);
		bank.role = role;
		bank.owner_kind = role == kRolePublic ? "state"
			: (role == kRolePrivate ? "national" : "foreign");
		bank.name_status = "no_data";
		renormalize(ws, nation);
	}
	return bank;
}

Bank& ensure(WorldState& ws, const std::string& nation) {
	return ensure(ws, nation, kRolePublic);
}

const Bank* find(const WorldState& ws, const std::string& nation, int role) {
	const auto found = ws.banks.find(bank_id(nation, role));
	return found == ws.banks.end() ? nullptr : &found->second;
}

const Bank* find(const WorldState& ws, const std::string& nation) {
	if (const Bank* bank = find(ws, nation, kRolePublic)) {
		return bank;
	}
	for (const auto& row : ws.banks) {
		if (row.second.nation == nation) {
			return &row.second;
		}
	}
	return nullptr;
}

const Bank* find_id(const WorldState& ws, const std::string& id) {
	const auto found = ws.banks.find(id);
	return found == ws.banks.end() ? nullptr : &found->second;
}

double declared_share(const WorldState& ws, const std::string& nation,
		int role) {
	const Bank* bank = find(ws, nation, role);
	return bank == nullptr ? 0.0 : bank->share;
}

void apply_definitions(WorldState& ws, const std::vector<NameData>& defs) {
	for (const NameData& data : defs) {
		if (data.nation.empty()) {
			continue;
		}
		Bank& pub = ensure(ws, data.nation, kRolePublic);
		pub.name = data.public_name;
		pub.name_status = data.public_present && !data.public_name.empty()
			? "present" : "no_data";
		Bank& pri = ensure(ws, data.nation, kRolePrivate);
		pri.name = data.private_name;
		pri.name_status = data.private_present && !data.private_name.empty()
			? "present" : "no_data";
		const bool wants_foreign = data.foreign_present
			&& !data.foreign_origin.empty();
		const auto existing = ws.banks.find(bank_id(data.nation, kRoleForeign));
		if (wants_foreign) {
			Bank& fgn = ensure(ws, data.nation, kRoleForeign);
			fgn.name = data.foreign_name;
			fgn.name_status = data.foreign_present
				&& !data.foreign_name.empty() ? "present" : "no_data";
			fgn.owner_nation = data.foreign_origin;
		} else if (existing != ws.banks.end()) {
			// Sem estrangeiro no dado: funde no privado conservando valores.
			merge_into(pri, existing->second);
			ws.banks.erase(existing);
		}
		renormalize(ws, data.nation);
	}
}

double equity(const Bank& bank) {
	return bank.cash + bank.loans + bank.arrears - bank.bc_borrowing
		- bank.deposits;
}

double retained(const Bank& bank) { return equity(bank) - bank.capital; }

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

const Bank* lender(const WorldState& ws, const Group& group) {
	// Crédito dirigido: grupo estatal toma do banco público.
	if (group.state_owned) {
		if (const Bank* bank = find(ws, group.origin_nation, kRolePublic)) {
			return bank;
		}
	}
	// Demais: sorteio determinístico do papel contra as participações.
	const std::uint64_t roll = stable_hash(group.id) % 10000ULL;
	double cumulative = 0.0;
	const Bank* last = nullptr;
	for (const int role : {kRolePublic, kRolePrivate, kRoleForeign}) {
		const Bank* bank = find(ws, group.origin_nation, role);
		if (bank == nullptr || !bank->absorbed_into.empty()) {
			continue;
		}
		last = bank;
		cumulative += bank->share * 10000.0;
		if (static_cast<double>(roll) < cumulative) {
			return bank;
		}
	}
	return last;
}

Bank* lender(WorldState& ws, const Group& group) {
	// Garante os dois bancos institucionais antes de escolher (o estrangeiro
	// só existe se o dado o declarou).
	ensure(ws, group.origin_nation, kRolePublic);
	ensure(ws, group.origin_nation, kRolePrivate);
	const Bank* chosen = lender(static_cast<const WorldState&>(ws), group);
	if (chosen == nullptr) {
		return nullptr;
	}
	const auto found = ws.banks.find(chosen->id);
	return found == ws.banks.end() ? nullptr : &found->second;
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
	Bank* bank = lender(ws, *group);
	if (bank == nullptr) {
		if (error != nullptr) {
			*error = "nacao sem banco";
		}
		return false;
	}
	// C6 (D-114): banco abaixo do indice de capital minimo corta credito novo
	// proporcionalmente (1,0 acima do minimo; 0 sem credito). No mundo base a
	// razao esta acima do piso declarado e o fator e' 1.
	const double tightening = bank_crisis::credit_factor(ws, *bank);
	const double granted = std::min(amount, limit * tightening);
	if (granted <= 1e-9) {
		if (error != nullptr) {
			*error = "credito cortado pelo capital do banco";
		}
		return false;
	}
	// Dinheiro BANCÁRIO: o banco cria o depósito do tomador (M1), não M0.
	bank->loans += granted;
	bank->deposits += granted;
	bank->loans_count += 1;
	group->cash += granted;
	group->debt += granted;
	group->credit_total += granted;
	if (out_amount != nullptr) {
		*out_amount = granted;
	}
	return true;
}

namespace {

// Capital pago pelas famílias da nação: sai da poupança das cidades na ordem
// de tamanho, CONSERVADO. A poupança tem duas camadas (`pop.savings` é
// reconstruída de `pop.stratum_savings` na renda do dia): debitar só o escalar
// faria o dinheiro voltar — as duas andam juntas.
double debit_households(Pop& pop, double amount) {
	double total = 0.0;
	for (int s = 0; s < kIncomeStrata; ++s) {
		total += std::max(0.0, pop.stratum_savings[s]);
	}
	if (total <= 0.0 || amount <= 0.0) {
		return 0.0;
	}
	const double take = std::min(amount, total);
	for (int s = 0; s < kIncomeStrata; ++s) {
		const double share = std::max(0.0, pop.stratum_savings[s]) / total;
		pop.stratum_savings[s] = std::max(0.0,
			pop.stratum_savings[s] - take * share);
	}
	pop.savings = 0.0;
	for (int s = 0; s < kIncomeStrata; ++s) {
		pop.savings += std::max(0.0, pop.stratum_savings[s]);
	}
	return take;
}

// Credita as famílias na mesma camada (dividendo do banco privado): entra no
// estrato mais alto (são os donos do banco no modelo) e a escalar acompanha.
void credit_households(Pop& pop, double amount) {
	if (!(amount > 0.0)) {
		return;
	}
	pop.stratum_savings[kIncomeStrata - 1] += amount;
	pop.savings = 0.0;
	for (int s = 0; s < kIncomeStrata; ++s) {
		pop.savings += std::max(0.0, pop.stratum_savings[s]);
	}
}

double pay_from_households(WorldState& ws, const std::string& nation,
		double amount) {
	std::vector<City*> cities;
	for (City& city : ws.cities) {
		if (city.nation == nation) {
			cities.push_back(&city);
		}
	}
	std::sort(cities.begin(), cities.end(), [](const City* a, const City* b) {
		if (a->simulation_population != b->simulation_population) {
			return a->simulation_population > b->simulation_population;
		}
		return a->id < b->id;
	});
	double paid = 0.0;
	for (City* city : cities) {
		if (paid >= amount) {
			break;
		}
		paid += debit_households(city->pop, amount - paid);
	}
	return paid;
}

} // namespace

double inject_capital(WorldState& ws, const std::string& nation, int role,
		double amount, double* out_paid, std::string* error) {
	if (out_paid != nullptr) {
		*out_paid = 0.0;
	}
	if (!(amount > 0.0) || !std::isfinite(amount)) {
		if (error != nullptr) {
			*error = "valor invalido";
		}
		return 0.0;
	}
	if (role < kRolePublic || role > kRoleForeign) {
		if (error != nullptr) {
			*error = "papel de banco desconhecido";
		}
		return 0.0;
	}
	double paid = 0.0;
	if (role == kRolePublic) {
		City* city = treasury_city(ws, nation);
		if (city == nullptr) {
			if (error != nullptr) {
				*error = "nacao sem cidade";
			}
			return 0.0;
		}
		paid = std::min(amount, std::max(0.0, city->government_budget));
		if (paid <= 0.0) {
			if (error != nullptr) {
				*error = "orcamento insuficiente";
			}
			return 0.0;
		}
		city->government_budget -= paid;
	} else if (role == kRolePrivate) {
		paid = pay_from_households(ws, nation, amount);
		if (paid <= 0.0) {
			if (error != nullptr) {
				*error = "poupanca das familias insuficiente";
			}
			return 0.0;
		}
	} else {
		// Capital estrangeiro: sai da posição do exterior (pode ser negativa
		// por desenho; a C1 já publica `external` assim).
		paid = amount;
		ws.foreign_cash -= paid;
	}
	Bank& bank = ensure(ws, nation, role);
	bank.cash += paid;
	bank.capital += paid;
	if (out_paid != nullptr) {
		*out_paid = paid;
	}
	return paid;
}

double inject_capital(WorldState& ws, const std::string& nation, double amount,
		double* out_paid, std::string* error) {
	return inject_capital(ws, nation, kRolePublic, amount, out_paid, error);
}

double receive_harvest_interest(WorldState& ws, const std::string& nation,
		double amount) {
	if (!(amount > 0.0) || !std::isfinite(amount)) {
		return 0.0;
	}
	Bank* bank = find_mut(ws, nation, kRolePublic);
	if (bank == nullptr) {
		return 0.0;
	}
	bank->cash += amount;
	bank->harvest_interest += amount;
	return amount;
}

namespace {

// Converte um valor em âncora para a moeda do país de origem do banco
// estrangeiro pela paridade da C2c. 0 = moeda de origem ausente (declarado).
double origin_amount(const WorldState& ws, const Bank& bank, double anchor) {
	const std::string& code = currency_of_nation(ws, bank.owner_nation);
	if (code.empty()) {
		return 0.0;
	}
	for (const Currency& currency : ws.currencies) {
		if (currency.code == code) {
			return anchor * exchange::parity_of(ws, currency);
		}
	}
	return 0.0;
}

void distribute_dividends(WorldState& ws, const Policy& pol) {
	for (auto& row : ws.banks) {
		Bank& bank = row.second;
		const double profit = retained(bank);
		if (profit <= 1e-9) {
			continue;
		}
		double payout = pol.dividend_payout * profit;
		if (payout <= 1e-9) {
			continue;
		}
		// O dividendo cria o depósito do dono e reduz o patrimônio do banco:
		// o lucro retido vira dinheiro do dono, sem destruir a identidade.
		bank.deposits += payout;
		if (bank.owner_kind == "state") {
			City* city = treasury_city(ws, bank.nation);
			if (city == nullptr) {
				bank.deposits -= payout;
				continue;
			}
			city->government_budget += payout;
		} else if (bank.owner_kind == "national") {
			City* city = treasury_city(ws, bank.nation);
			if (city == nullptr) {
				bank.deposits -= payout;
				continue;
			}
			credit_households(city->pop, payout);
		} else {
			ws.foreign_cash += payout;
			bank.remittance += payout;
			bank.remittance_origin += origin_amount(ws, bank, payout);
		}
		bank.dividends += payout;
		bank.last_dividend_day = ws.day;
	}
}

} // namespace

int phase(WorldState& ws, double* bank_money_delta) {
	if (bank_money_delta != nullptr) {
		*bank_money_delta = 0.0;
	}
	// Fluxo de dinheiro BANCÁRIO do dia (o que o invariante soma ao
	// `money_total`): juro pago e amortização destroem depósito e caixa do
	// tomador; o dividendo converte patrimônio do banco em depósito do dono.
	// A execução do calote apenas troca depósito do grupo por caixa do banco
	// (os dois já estão em `money_total`): não cria nem destrói dinheiro e
	// fica FORA deste fluxo.
	double money_delta = 0.0;
	// Sem crédito e sem posição o dia não muda: o mundo default é o mesmo.
	bool any = false;
	for (const auto& row : ws.banks) {
		if (row.second.loans > 0.0 || row.second.arrears > 0.0
				|| row.second.deposits > 0.0
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
	// Credor de cada grupo (regra declarada do §3), calculado uma vez.
	std::vector<std::string> lenders(ws.groups.size());
	for (std::size_t index = 0; index < ws.groups.size(); ++index) {
		const Bank* bank = lender(ws, ws.groups[index]);
		lenders[index] = bank == nullptr ? std::string() : bank->id;
	}
	int defaults = 0;
	for (auto& row : ws.banks) {
		Bank& bank = row.second;
		if (bank.loans <= 0.0 && bank.arrears <= 0.0
				&& bank.deposits <= 0.0 && bank.bc_borrowing <= 0.0) {
			continue;
		}
		const double rate = loan_rate(ws, bank.nation);
		const double base_rate = monetary::rate_for_nation(ws, bank.nation);
		for (std::size_t index = 0; index < ws.groups.size(); ++index) {
			Group& group = ws.groups[index];
			if (lenders[index] != bank.id) {
				continue;
			}
			if (group.debt <= 0.0 && group.credit_arrears <= 0.0) {
				group.credit_default_days = 0;
				continue;
			}
			// 1. Juro do dia. O caixa do grupo É o depósito dele no banco:
			// pagar juro destrói depósito e vira patrimônio do banco. O que o
			// caixa não cobre vira juro VENCIDO dos dois lados — nunca
			// capitalizado em silêncio.
			const double interest = group.debt * rate / kDaysPerYear;
			const double paid_interest = std::min({
				std::max(0.0, group.cash), std::max(0.0, interest),
				std::max(0.0, bank.deposits) });
			if (paid_interest > 0.0) {
				group.cash -= paid_interest;
				bank.deposits -= paid_interest;
				bank.interest_income += paid_interest;
				money_delta -= paid_interest;
				// Reserva herdada da C3 (save migrado): se o banco ainda deve
				// ao BC, paga a parcela da taxa básica em caixa — o M0
				// correspondente é destruído e registrado na emissão do dia.
				const double base_part = std::min({
					bank.cash,
					bank.bc_borrowing * base_rate / kDaysPerYear,
					std::max(0.0, paid_interest) });
				if (base_part > 0.0) {
					bank.cash -= base_part;
					bank.bc_borrowing -= base_part;
					bank.interest_expense += base_part;
					ws.emission_today -= base_part;
				}
			}
			const double unpaid = std::max(0.0, interest - paid_interest);
			if (unpaid > 0.0) {
				group.credit_arrears += unpaid;
				bank.arrears += unpaid;
			}
			// 2. Amortização: só o excedente ao capital de giro (30 dias de
			// custo efetivamente pago) e na fração declarada. Destrói depósito.
			const double working = kWorkingCapitalDays
				* std::max(0.0, sheets[index].daily_cost);
			const double excess = std::max(0.0, group.cash - working);
			double repay = std::min({ group.debt,
				pol.repay_share * excess, std::max(0.0, bank.deposits) });
			if (repay > 0.0) {
				group.cash -= repay;
				group.debt -= repay;
				group.credit_repaid += repay;
				bank.loans -= repay;
				bank.deposits -= repay;
				money_delta -= repay;
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
			const double seized = std::min({ std::max(0.0, group.cash),
				claim, std::max(0.0, bank.deposits) });
			if (seized > 0.0) {
				group.cash -= seized;
				// O caixa tomado sai do depósito e vira caixa do banco: os dois
				// lados já estão em `money_total`, então não há criação nem
				// destruição de dinheiro (por isso `money_delta` não muda).
				bank.deposits -= seized;
				bank.cash += seized;
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
	// 4. Dividendo mensal ao dono (lucro retido × payout declarado).
	if (ws.day > 0 && ws.day % kDividendPeriodDays == 0) {
		double deposits_before = 0.0;
		for (const auto& row : ws.banks) {
			deposits_before += row.second.deposits;
		}
		distribute_dividends(ws, pol);
		double deposits_after = 0.0;
		for (const auto& row : ws.banks) {
			deposits_after += row.second.deposits;
		}
		// O dividendo credita o dono (governo, famílias ou exterior) e cria o
		// depósito correspondente: é criação de dinheiro bancário no dia.
		money_delta += deposits_after - deposits_before;
	}
	if (bank_money_delta != nullptr) {
		*bank_money_delta = money_delta;
	}
	return defaults;
}

Totals totals(const WorldState& ws) {
	Totals out;
	for (const auto& row : ws.banks) {
		const Bank& bank = row.second;
		out.loans += bank.loans;
		out.arrears += bank.arrears;
		out.deposits += bank.deposits;
		out.bc_borrowing += bank.bc_borrowing;
		out.cash += bank.cash;
		out.capital += bank.capital;
		out.writeoffs += bank.writeoffs;
		out.interest_income += bank.interest_income;
		out.interest_expense += bank.interest_expense;
		out.dividends += bank.dividends;
		out.remittance += bank.remittance;
		out.remittance_origin += bank.remittance_origin;
		out.harvest_interest += bank.harvest_interest;
		out.defaults += bank.defaults;
		out.loans_count += bank.loans_count;
		out.equity += equity(bank);
		out.retained += retained(bank);
		out.banks += 1;
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

double deposits_total(const WorldState& ws) {
	double total = 0.0;
	for (const auto& row : ws.banks) {
		total += std::max(0.0, row.second.deposits);
	}
	return total;
}

} // namespace bank
} // namespace ogs
