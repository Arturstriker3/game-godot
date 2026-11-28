// C4 (D-110) — bancos com dono (público, privado nacional, estrangeiro).
// Contrato em bank.h e desenho em docs/economy/C4_banks.md. Nada aqui inventa
// número: taxas vêm da política monetária da moeda (C2b1), nomes de banks.json
// e todo parâmetro de regra é default de jogo declarado.

#include "bank.h"

#include "balance.h"
#include "bank_crisis.h"
#include "deposit.h"
#include "exchange.h"
#include "monetary.h"
#include "sim_constants.h"
#include "treasury.h"

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
	into.deposits_funding += from.deposits_funding;
	into.capital_target += from.capital_target;
	into.capital_ratio_target = std::max(into.capital_ratio_target,
		from.capital_ratio_target);
	into.seed_scale = std::max(into.seed_scale, from.seed_scale);
	into.reference_scale = std::max(into.reference_scale,
		from.reference_scale);
	into.gov_bonds += from.gov_bonds;
	if (into.charter.empty() || into.charter == "no_data") {
		into.charter = from.charter;
	}
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

namespace {

bool credit_bank_present(const Bank& bank) {
	return bank.name_status == "present" && bank.absorbed_into.empty();
}

double solvency_ratio(const Bank& bank) {
	const double loans = std::max(0.0, bank.loans);
	if (loans <= 1e-9) {
		return 1.0;
	}
	return std::clamp(equity(bank) / loans, 0.0, 1.0);
}

// Claim de divida do tomador por credor (ordenado por bank_id).
BankLoan& loan_claim(std::vector<BankLoan>& claims, const std::string& id) {
	auto found = std::lower_bound(claims.begin(), claims.end(), id,
		[](const BankLoan& claim, const std::string& bank_id) {
			return claim.bank_id < bank_id;
		});
	if (found != claims.end() && found->bank_id == id) {
		return *found;
	}
	BankLoan claim;
	claim.bank_id = id;
	return *claims.insert(found, claim);
}

const BankLoan* find_loan(const std::vector<BankLoan>& claims,
		const std::string& id) {
	const auto found = std::lower_bound(claims.begin(), claims.end(), id,
		[](const BankLoan& claim, const std::string& bank_id) {
			return claim.bank_id < bank_id;
		});
	return found != claims.end() && found->bank_id == id ? &*found : nullptr;
}

BankLoan* find_loan_mut(std::vector<BankLoan>& claims,
		const std::string& id) {
	auto found = std::lower_bound(claims.begin(), claims.end(), id,
		[](const BankLoan& claim, const std::string& bank_id) {
			return claim.bank_id < bank_id;
		});
	return found != claims.end() && found->bank_id == id ? &*found : nullptr;
}

bool claims_mode(const WorldState& ws) {
	return ws.banks_rework.enabled && ws.banks_rework.deposits;
}

// Menor taxa ofertada com capacidade; desempate deterministico por
// capacidade e id (nunca pela ordem de insercao do mapa).
Bank* choose_credit_bank(WorldState& ws, const std::string& nation) {
	Bank* best = nullptr;
	double best_rate = 0.0;
	double best_cap = 0.0;
	for (auto& row : ws.banks) {
		Bank& bank = row.second;
		if (bank.nation != nation || !credit_bank_present(bank)) {
			continue;
		}
		const double rate = offer_rate(ws, bank);
		const double cap = capacity(ws, bank);
		// R2b2: so entra na escolha o banco com capacidade NOVA positiva; sem
		// nenhum, o pedido e' recusado (nao se cria banco nem se ignora a folga).
		if (!(cap > 1e-9)) {
			continue;
		}
		const bool better_rate = best == nullptr
			|| rate < best_rate - 1e-12;
		const bool same_rate = best != nullptr
			&& std::fabs(rate - best_rate) <= 1e-12;
		const bool better_cap = same_rate && (cap > best_cap + 1e-9
			|| (std::fabs(cap - best_cap) <= 1e-9 && bank.id < best->id));
		if (better_rate || better_cap) {
			best = &bank;
			best_rate = rate;
			best_cap = cap;
		}
	}
	return best;
}

void record_loan(WorldState& ws, std::vector<BankLoan>& claims, Bank& bank,
		double amount) {
	BankLoan& claim = loan_claim(claims, bank.id);
	claim.principal += amount;
	claim.rate_add = offer_rate(ws, bank) - loan_rate(ws, bank.nation);
}

} // namespace

double offer_rate(const WorldState& ws, const Bank& bank) {
	const Policy pol = effective_policy(ws);
	const double base = monetary::rate_for_nation(ws, bank.nation)
		+ pol.spread;
	if (!claims_mode(ws)) {
		return base;
	}
	// Spread ADITIVO por solidez, centrado na media dos bancos presentes da
	// nacao: o nivel medio de juros nao muda (a soma dos desvios e' 0).
	double sum = 0.0;
	int count = 0;
	for (const auto& row : ws.banks) {
		const Bank& peer = row.second;
		if (peer.nation != bank.nation || !credit_bank_present(peer)) {
			continue;
		}
		sum += solvency_ratio(peer);
		++count;
	}
	if (count < 2) {
		return base;
	}
	constexpr double kSolvencySpread = 0.02;  // 2 p.p. de amplitude
	const double mean = sum / static_cast<double>(count);
	const double delta = kSolvencySpread * (mean - solvency_ratio(bank));
	return std::max(0.0, base + delta);
}

double capacity(const WorldState& ws, const Bank& bank) {
	const BanksRework& br = ws.banks_rework;
	if (!br.enabled || !br.deposits) {
		return 1e18;
	}
	const double target = bank.capital_ratio_target > 0.0
		? bank.capital_ratio_target : br.dividend_target_ratio;
	const double capital_headroom = target > 1e-9
		? std::max(0.0, equity(bank) / target - std::max(0.0, bank.loans))
		: 1e18;
	const double min_reserve = std::max(0.0,
		br.liquidity_min_reserve_ratio);
	const double liquidity_headroom = min_reserve > 1e-9
		? std::max(0.0, std::max(0.0, bank.cash) / min_reserve
			- total_deposits(bank))
		: 1e18;
	return std::min(capital_headroom, liquidity_headroom);
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
		- bank.deposits - bank.deposits_funding;
}

double retained(const Bank& bank) { return equity(bank) - bank.capital; }

double total_deposits(const Bank& bank) {
	return std::max(0.0, bank.deposits)
		+ std::max(0.0, bank.deposits_funding);
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
	if (claims_mode(ws)) {
		// R2b2: com depositos o credor e' o banco de menor taxa COM capacidade.
		// Sem nenhum, recusa — nao se cai no lender() (que ignoraria a folga).
		bank = choose_credit_bank(ws, group->origin_nation);
		if (bank == nullptr) {
			if (error != nullptr) {
				*error = "sem capacidade de credito";
			}
			return false;
		}
	}
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
	double granted = std::min(amount, limit * tightening);
	if (claims_mode(ws)) {
		// R2b2: a capacidade NOVA do banco limita o desembolso.
		granted = std::min(granted, capacity(ws, *bank));
	}
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
	if (claims_mode(ws)) {
		// R2b1: grava o credor no claim do tomador (servicing por banco).
		record_loan(ws, group->debt_claims, *bank, granted);
	}
	group->cash += granted;
	group->debt += granted;
	group->credit_total += granted;
	if (out_amount != nullptr) {
		*out_amount = granted;
	}
	return true;
}

bool lend_city(WorldState& ws, const std::string& city_id, double amount,
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
	City* city = nullptr;
	for (City& row : ws.cities) {
		if (row.id == city_id) {
			city = &row;
			break;
		}
	}
	if (city == nullptr) {
		if (error != nullptr) {
			*error = "cidade desconhecida";
		}
		return false;
	}
	// R2b1: com depositos, o banco e' escolhido por taxa/capacidade entre os
	// papeis PRESENTES da nacao (nao ha mais monopolio publico). Se a cidade
	// ja tem divida com um credor, o novo desembolso continua nele para o par
	// tomador<->banco seguir unico no servico; sem divida, compete.
	Bank* bank = nullptr;
	if (claims_mode(ws)) {
		for (BankLoan& claim : city->firm_debt_claims) {
			if (claim.principal + claim.arrears > 1e-9) {
				const auto found = ws.banks.find(claim.bank_id);
				if (found != ws.banks.end()) {
					bank = &found->second;
				}
				break;
			}
		}
		if (bank == nullptr) {
			bank = choose_credit_bank(ws, city->nation);
			if (bank == nullptr) {
				if (error != nullptr) {
					*error = "sem capacidade de credito";
				}
				return false;
			}
		}
	}
	if (bank == nullptr) {
		// C4 (D-110): sem depositos, o credito de giro usa o banco PUBLICO
		// que JA existe (caminho de hoje, bit a bit).
		bank = find_mut(ws, city->nation, kRolePublic);
	}
	if (bank == nullptr) {
		if (error != nullptr) {
			*error = "nacao sem banco publico";
		}
		return false;
	}
	const double tightening = bank_crisis::credit_factor(ws, *bank);
	double granted = amount * std::clamp(tightening, 0.0, 1.0);
	if (claims_mode(ws)) {
		// R2b2: a capacidade NOVA do banco limita o desembolso.
		granted = std::min(granted, capacity(ws, *bank));
	}
	if (granted <= 1e-9) {
		if (error != nullptr) {
			*error = "credito cortado pelo capital do banco";
		}
		return false;
	}
	// Dinheiro BANCÁRIO: depósito do tomador (M1), não M0.
	bank->loans += granted;
	bank->deposits += granted;
	bank->loans_count += 1;
	if (claims_mode(ws)) {
		record_loan(ws, city->firm_debt_claims, *bank, granted);
	}
	city->firm_receive(granted);
	city->firm_debt += granted;
	city->firm_credit_total += granted;
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
		double amount, const std::string& bank_id) {
	if (!(amount > 0.0) || !std::isfinite(amount)) {
		return 0.0;
	}
	Bank* bank = nullptr;
	if (!bank_id.empty()) {
		const auto found = ws.banks.find(bank_id);
		if (found != ws.banks.end()) {
			bank = &found->second;
		}
	}
	if (bank == nullptr && claims_mode(ws)) {
		// R2b1: o juro da safra vai ao banco escolhido pela competicao (o
		// mesmo criterio de taxa/capacidade do credito), nao ao publico fixo.
		bank = choose_credit_bank(ws, nation);
	}
	if (bank == nullptr) {
		bank = find_mut(ws, nation, kRolePublic);
	}
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
	const BanksRework& br = ws.banks_rework;
	for (auto& row : ws.banks) {
		Bank& bank = row.second;
		const double profit = retained(bank);
		if (profit <= 1e-9) {
			continue;
		}
		if (br.enabled && br.capital) {
			// R2a: retém até a meta de adequação (com rampa de 24 meses) e
			// distribui só o EXCEDENTE DE CAIXA. O dividendo debita o caixa do
			// banco e credita o dono: transferência, nunca criação de depósito.
			const double assets = bank.loans + bank.arrears;
			const double current = assets > 1e-9
				? equity(bank) / assets : 1.0;
			const double target = bank.capital_ratio_target > 0.0
				? bank.capital_ratio_target : br.dividend_target_ratio;
			const double day_frac = std::clamp(
				static_cast<double>(ws.day) / 730.0, 0.0, 1.0);
			const double target_eff = std::min(target,
				current + (target - current) * day_frac);
			const double retain = std::max(0.0, target_eff * assets
				- equity(bank));
			// R2a (QA M2): o piso de caixa do dividendo e o knob declarado
			// `dividend.cash_floor_ratio` (fracao dos depositos totais, como o
			// `liquidity.min_reserve_ratio`). Default 0,10 nos dois -> o
			// caminho de jogo nao muda; o campo deixa de ser morto.
			const double floor = br.dividend_cash_floor_ratio
				* total_deposits(bank);
			const double payout = std::min(
				std::max(0.0, profit - retain),
				std::max(0.0, bank.cash - floor));
			if (payout <= 1e-9) {
				continue;
			}
			bool credited = true;
			if (bank.owner_kind == "state") {
				if (treasury::enabled(ws)) {
					treasury::collect(ws, bank.nation, payout);
				} else {
					City* city = treasury_city(ws, bank.nation);
					if (city == nullptr) {
						credited = false;
					} else {
						city->government_budget += payout;
					}
				}
			} else if (bank.owner_kind == "national") {
				City* city = treasury_city(ws, bank.nation);
				if (city == nullptr) {
					credited = false;
				} else {
					credit_households(city->pop, payout);
				}
			} else {
				ws.foreign_cash += payout;
				bank.remittance += payout;
				bank.remittance_origin += origin_amount(ws, bank, payout);
			}
			if (!credited) {
				continue;
			}
			bank.cash -= payout;
			bank.dividends += payout;
			bank.last_dividend_day = ws.day;
			continue;
		}
		// Caminho legado (banks.capital desligado): 50% do lucro retido vira
		// depósito do dono, como na C4. Nenhum número muda no OFF.
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

// D-126: custo diário efetivo das firmas privadas/estatais e dos serviços da
// cidade — base do capital de giro protegido e do teto do crédito de crise.
double city_business_daily_cost(const WorldState& ws, std::size_t city_index) {
	if (city_index >= ws.cities.size()) {
		return 0.0;
	}
	const City& city = ws.cities[city_index];
	double cost = std::max(0.0, city.service_wage_bill)
		+ std::max(0.0, city.service_input_cost);
	if (city_index < ws.buildings_by_city.size()) {
		for (const int bi : ws.buildings_by_city[city_index]) {
			const Building& building =
				ws.buildings[static_cast<std::size_t>(bi)];
			if (building.owner == "private" || building.owner == "state") {
				cost += std::max(0.0, building.paid_wages)
					+ std::max(0.0, building.paid_opex);
			}
		}
	}
	return cost;
}

// R2b1: paga o grupo ao banco com a ordem caixa -> deposito. A parcela de
// caixa e' limitada por `cash_cap` (o D_l do credor, como no caminho legado);
// a parcela de claim sai do banco de relacionamento e volta como caixa.
struct GroupPaid {
	double cash = 0.0;
	double claim = 0.0;
	double total() const { return cash + claim; }
};

GroupPaid pay_group_capped(WorldState& ws, Group& group, double value,
		double cash_cap) {
	GroupPaid out;
	if (!(value > 0.0)) {
		return out;
	}
	const double cash_room = std::min(std::max(0.0, group.cash),
		std::max(0.0, cash_cap));
	out.cash = std::min(value, cash_room);
	group.cash -= out.cash;
	const double need = value - out.cash;
	if (need > 1e-9) {
		out.claim = deposit::withdraw_group(ws, group, need);
		// withdraw_group credita o caixa do pagador; o pagamento debita.
		group.cash = std::max(0.0, group.cash - out.claim);
	}
	return out;
}

// Mesma ordem caixa -> deposito para um sub-saldo da cidade. `cash_cap` e' o
// D_l do credor (a parcela de caixa que pode ser destruida como deposito).
double pay_city_sub(WorldState& ws, City& city, bool firm, double value,
		double cash_cap, double* cash_paid, double* claim_paid) {
	if (cash_paid != nullptr) {
		*cash_paid = 0.0;
	}
	if (claim_paid != nullptr) {
		*claim_paid = 0.0;
	}
	if (!(value > 0.0)) {
		return 0.0;
	}
	const double sub = firm ? std::max(0.0, city.firm_sub)
		: std::max(0.0, city.service_sub);
	const double cash = std::min({ value, sub, std::max(0.0, cash_cap) });
	if (cash > 0.0) {
		if (firm) {
			city.firm_pay(cash, City::kR1Finance);
		} else {
			city.service_pay(cash, City::kR1Finance);
		}
	}
	double claim = 0.0;
	const double need = value - cash;
	if (need > 1e-9) {
		claim = deposit::withdraw_city(ws, city, firm, need);
		if (claim > 0.0) {
			if (firm) {
				city.firm_pay(claim, City::kR1Finance);
			} else {
				city.service_pay(claim, City::kR1Finance);
			}
		}
	}
	if (cash_paid != nullptr) {
		*cash_paid = cash;
	}
	if (claim_paid != nullptr) {
		*claim_paid = claim;
	}
	return cash + claim;
}

// Paga um valor ao banco usando firmas e depois servicos (a mesma ordem do
// caminho legado), devolvendo a ORIGEM do dinheiro.
double pay_city_liquid(WorldState& ws, City& city, double value,
		double cash_cap, double* cash_paid, double* claim_paid) {
	double cash_total = 0.0;
	double claim_total = 0.0;
	double left = value;
	double cap = std::max(0.0, cash_cap);
	if (left > 0.0) {
		const double firm_value = std::min(left,
			std::max(0.0, city.firm_liquidity()));
		if (firm_value > 0.0) {
			double c = 0.0;
			double k = 0.0;
			const double paid = pay_city_sub(ws, city, true, firm_value, cap,
				&c, &k);
			cash_total += c;
			claim_total += k;
			cap = std::max(0.0, cap - c);
			left -= paid;
		}
	}
	if (left > 0.0) {
		const double service_value = std::min(left,
			std::max(0.0, city.service_liquidity()));
		if (service_value > 0.0) {
			double c = 0.0;
			double k = 0.0;
			pay_city_sub(ws, city, false, service_value, cap, &c, &k);
			cash_total += c;
			claim_total += k;
			left -= c + k;
		}
	}
	if (cash_paid != nullptr) {
		*cash_paid = cash_total;
	}
	if (claim_paid != nullptr) {
		*claim_paid = claim_total;
	}
	return cash_total + claim_total;
}

// R2b1: claims com saldo em aberto (principal ou juros vencidos).
int outstanding_claims(const std::vector<BankLoan>& claims) {
	int count = 0;
	for (const BankLoan& claim : claims) {
		if (claim.principal + claim.arrears > 1e-9) {
			++count;
		}
	}
	return count;
}

BankLoan* first_outstanding_claim(std::vector<BankLoan>& claims) {
	for (BankLoan& claim : claims) {
		if (claim.principal + claim.arrears > 1e-9) {
			return &claim;
		}
	}
	return nullptr;
}

const BankLoan* first_outstanding_claim(const std::vector<BankLoan>& claims) {
	for (const BankLoan& claim : claims) {
		if (claim.principal + claim.arrears > 1e-9) {
			return &claim;
		}
	}
	return nullptr;
}

// R2b1: servicing da cidade POR CREDOR (`firm_debt_claims`). Cada claim paga
// juro/amortizacao ao proprio banco; juro nao pago vira atraso do claim. A
// renegociacao e a execucao SO rodam com UM credor em aberto: com varios, o
// caso e' de resolucao do R2c — executar por credor tomaria o mesmo caixa
// duas vezes. Os agregados `firm_debt`/`firm_credit_arrears` acompanham.
void service_city_claims(WorldState& ws, std::size_t city_index,
		const Policy& pol, double* money_delta) {
	City& city = ws.cities[city_index];
	const double add = std::max(0.0, city.firm_credit_rate_add);
	// 1. Juro por credor (a taxa e' a oferta do banco do claim + premio do dia).
	double unpaid_total = 0.0;
	for (BankLoan& claim : city.firm_debt_claims) {
		if (claim.principal + claim.arrears <= 1e-9) {
			continue;
		}
		const auto found = ws.banks.find(claim.bank_id);
		if (found == ws.banks.end()) {
			continue;
		}
		Bank& bank = found->second;
		const double rate = loan_rate(ws, bank.nation) + claim.rate_add + add;
		const double interest = std::max(0.0, claim.principal)
			* std::max(0.0, rate) / kDaysPerYear;
		if (interest <= 1e-12) {
			continue;
		}
		const double claim_room = std::max(0.0,
			city.firm_liquidity() - city.private_total());
		const double target = std::min({ interest,
			std::max(0.0, city.firm_liquidity()),
			std::max(0.0, bank.deposits) + claim_room });
		double cash_paid = 0.0;
		double claim_paid = 0.0;
		const double paid = pay_city_liquid(ws, city, target,
			std::max(0.0, bank.deposits), &cash_paid, &claim_paid);
		if (paid > 0.0) {
			bank.deposits = std::max(0.0, bank.deposits - cash_paid);
			bank.cash += claim_paid;
			bank.interest_income += paid;
			if (money_delta != nullptr) {
				*money_delta -= cash_paid;
			}
		}
		const double unpaid = std::max(0.0, interest - paid);
		if (unpaid > 1e-12) {
			claim.arrears += unpaid;
			city.firm_credit_arrears += unpaid;
			bank.arrears += unpaid;
			unpaid_total += unpaid;
		}
	}
	// O dia de atraso conta UMA vez, nao uma por credor.
	if (unpaid_total > 1e-12) {
		city.firm_credit_default_days += 1;
	} else {
		city.firm_credit_default_days = 0;
	}
	// 2. Amortizacao: so o excedente ao capital de giro, repartido entre os
	// claims em ordem de banco (determinista).
	const double working = kWorkingCapitalDays
		* city_business_daily_cost(ws, city_index);
	const double excess = std::max(0.0, city.firm_liquidity() - working);
	double budget = std::min(std::max(0.0, pol.repay_share) * excess,
		std::max(0.0, city.firm_debt));
	for (BankLoan& claim : city.firm_debt_claims) {
		if (budget <= 1e-9) {
			break;
		}
		if (claim.principal <= 1e-9) {
			continue;
		}
		const auto found = ws.banks.find(claim.bank_id);
		if (found == ws.banks.end()) {
			continue;
		}
		Bank& bank = found->second;
		const double claim_room = std::max(0.0,
			city.firm_liquidity() - city.private_total());
		const double want = std::min({ claim.principal, budget,
			std::max(0.0, bank.deposits) + claim_room });
		if (want <= 1e-9) {
			continue;
		}
		double cash_paid = 0.0;
		double claim_paid = 0.0;
		const double repay = pay_city_liquid(ws, city, want,
			std::max(0.0, bank.deposits), &cash_paid, &claim_paid);
		if (repay <= 1e-9) {
			continue;
		}
		claim.principal = std::max(0.0, claim.principal - repay);
		city.firm_debt = std::max(0.0, city.firm_debt - repay);
		bank.loans = std::max(0.0, bank.loans - repay);
		bank.deposits = std::max(0.0, bank.deposits - cash_paid);
		bank.cash += claim_paid;
		if (money_delta != nullptr) {
			*money_delta -= cash_paid;
		}
		budget -= repay;
	}
	// 3. Renegociacao (max. declarado) SO com um credor em aberto: com varios,
	// a multa/capitalizacao de um claim mudaria a divida dos demais.
	BankLoan* single = outstanding_claims(city.firm_debt_claims) == 1
		? first_outstanding_claim(city.firm_debt_claims) : nullptr;
	if (single != nullptr
			&& ws.crisis.enabled
			&& city.firm_credit_arrears > 1e-9
			&& city.firm_credit_renegotiations
				< std::max(0, ws.crisis.credit_max_renegotiations)
			&& city.firm_credit_default_days
				>= std::max(1, ws.crisis.credit_renegotiation_days)) {
		const auto found = ws.banks.find(single->bank_id);
		if (found != ws.banks.end()) {
			Bank& bank = found->second;
			const double fee = std::max(0.0, ws.crisis.credit_penalty
				* (single->principal + single->arrears));
			const double claim_room = std::max(0.0,
				city.firm_liquidity() - city.private_total());
			const double target = std::min({ fee,
				std::max(0.0, city.firm_liquidity()),
				std::max(0.0, bank.deposits) + claim_room });
			double cash_paid = 0.0;
			double claim_paid = 0.0;
			const double paid = pay_city_liquid(ws, city, target,
				std::max(0.0, bank.deposits), &cash_paid, &claim_paid);
			if (paid > 0.0) {
				bank.deposits = std::max(0.0, bank.deposits - cash_paid);
				bank.cash += claim_paid;
				bank.interest_income += paid;
				if (money_delta != nullptr) {
					*money_delta -= cash_paid;
				}
				ws.crisis_penalty_paid += paid;
			}
			const double capitalized = std::max(0.0, fee - paid);
			const double recap = single->arrears + capitalized;
			single->principal += recap;
			single->arrears = 0.0;
			single->default_days = 0;
			city.firm_debt += recap;
			bank.loans += recap;
			bank.arrears = std::max(0.0,
				bank.arrears - city.firm_credit_arrears);
			city.firm_credit_arrears = 0.0;
			city.firm_credit_default_days = 0;
			city.firm_credit_renegotiations += 1;
			ws.crisis_renegotiations += 1;
		}
	}
	// 4. Execucao/calote: SO com um credor em aberto (R2b1). O caixa tomado
	// vira caixa do banco (nele ou no banco de onde o claim foi sacado) e a
	// parcela do claim e' sacada no mesmo dia; o resto e' perda do credor.
	if (outstanding_claims(city.firm_debt_claims) != 1) {
		return;
	}
	BankLoan* only = first_outstanding_claim(city.firm_debt_claims);
	if (only == nullptr || city.firm_credit_default_days
			< std::max(1, pol.default_days)
			|| city.firm_credit_renegotiations
				< std::max(0, ws.crisis.credit_max_renegotiations)) {
		return;
	}
	const auto found = ws.banks.find(only->bank_id);
	if (found == ws.banks.end()) {
		return;
	}
	Bank& bank = found->second;
	const double claim = only->principal + only->arrears;
	const double claim_room = std::max(0.0,
		city.firm_liquidity() - city.private_total());
	const double target = std::min({ claim,
		std::max(0.0, city.firm_liquidity()),
		std::max(0.0, bank.deposits) + claim_room });
	double cash_paid = 0.0;
	double claim_paid = 0.0;
	const double seized = pay_city_liquid(ws, city, target,
		std::max(0.0, bank.deposits), &cash_paid, &claim_paid);
	if (seized > 0.0) {
		bank.deposits = std::max(0.0, bank.deposits - cash_paid);
		bank.cash += cash_paid + claim_paid;
	}
	bank.loans = std::max(0.0, bank.loans - only->principal);
	bank.arrears = std::max(0.0, bank.arrears - only->arrears);
	bank.writeoffs += std::max(0.0, claim - seized);
	bank.defaults += 1;
	bank.last_default_day = ws.day;
	city.firm_debt = std::max(0.0, city.firm_debt - only->principal);
	city.firm_credit_arrears = std::max(0.0,
		city.firm_credit_arrears - only->arrears);
	only->principal = 0.0;
	only->arrears = 0.0;
	only->default_days = 0;
	city.firm_credit_default_days = 0;
	city.firm_credit_defaults += 1;
}

} // namespace

// R2a: fecho mensal do dividendo pela MESMA regra que a fase usa. Exposto para
// o teste unitario provar a transferencia (caixa debitado, Δdepositos = 0 e
// `money_total` constante) sem depender dos outros passos do dia.
void pay_dividends(WorldState& ws) {
	distribute_dividends(ws, policy(ws));
}

int phase(WorldState& ws, double* bank_money_delta) {
	if (bank_money_delta != nullptr) {
		*bank_money_delta = 0.0;
	}
	// R2b1: servicing e pagamentos leem o claim sacavel do dia.
	deposit::link(ws);
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
				|| row.second.deposits_funding > 0.0
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
	const bool claims = claims_mode(ws);
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
				&& bank.deposits <= 0.0
				&& bank.deposits_funding <= 0.0
				&& bank.bc_borrowing <= 0.0) {
			continue;
		}
		const double rate = loan_rate(ws, bank.nation);
		const double base_rate = monetary::rate_for_nation(ws, bank.nation);
		for (std::size_t index = 0; index < ws.groups.size(); ++index) {
			Group& group = ws.groups[index];
			BankLoan* loan = claims
				? find_loan_mut(group.debt_claims, bank.id) : nullptr;
			if (claims) {
				if (loan == nullptr
						|| loan->principal + loan->arrears <= 1e-9) {
					continue;
				}
			} else if (lenders[index] != bank.id) {
				continue;
			}
			if (loan == nullptr && group.debt <= 0.0
					&& group.credit_arrears <= 0.0) {
				group.credit_default_days = 0;
				continue;
			}
			// R2b1: no modo claims o juro/amortizacao/calote recaem SOBRE a
			// divida do credor visitado (o grupo pode ter dois bancos).
			const double loan_principal = loan != nullptr
				? std::max(0.0, loan->principal)
				: std::max(0.0, group.debt);
			const double loan_arrears = loan != nullptr
				? std::max(0.0, loan->arrears)
				: std::max(0.0, group.credit_arrears);
			const double loan_add = loan != nullptr ? loan->rate_add : 0.0;
			// 1. Juro do dia. O caixa do grupo É o depósito dele no banco:
			// pagar juro destrói depósito e vira patrimônio do banco. O que o
			// caixa não cobre vira juro VENCIDO dos dois lados — nunca
			// capitalizado em silêncio. D-126: o capital de giro de crise paga
			// a taxa do banco + o prêmio declarado do grupo.
			const double group_rate = rate + loan_add
				+ std::max(0.0, group.credit_rate_add);
			const double interest = loan_principal * group_rate
				/ kDaysPerYear;
			double paid_interest = 0.0;
			if (claims) {
				const GroupPaid paid = pay_group_capped(ws, group, interest,
					std::max(0.0, bank.deposits));
				paid_interest = paid.total();
				bank.deposits = std::max(0.0, bank.deposits - paid.cash);
				bank.cash += paid.claim;
				bank.interest_income += paid_interest;
				money_delta -= paid.cash;
			} else {
				paid_interest = std::min({
					std::max(0.0, group.cash), std::max(0.0, interest),
					std::max(0.0, bank.deposits) });
				if (paid_interest > 0.0) {
					group.cash -= paid_interest;
					bank.deposits -= paid_interest;
					bank.interest_income += paid_interest;
					money_delta -= paid_interest;
				}
			}
			if (paid_interest > 0.0) {
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
				if (loan != nullptr) {
					loan->arrears += unpaid;
				}
				group.credit_arrears += unpaid;
				bank.arrears += unpaid;
				group.credit_arrears_days += 1;
			} else {
				group.credit_arrears_days = 0;
			}
			// D-126: UMA renegociação do capital de giro em atraso, com multa.
			// A multa é paga ao banco (destrói depósito) e o atraso é
			// reclassificado em principal — nenhum dinheiro é criado.
			if (ws.crisis.enabled
					&& group.credit_arrears > 1e-9
					&& (!claims || group.debt_claims.size() <= 1)
					&& group.credit_renegotiations
						< std::max(0, ws.crisis.credit_max_renegotiations)
					&& group.credit_arrears_days
						>= std::max(1, ws.crisis.credit_renegotiation_days)) {
				const double base = claims
					? loan_principal + loan_arrears
					: group.debt + group.credit_arrears;
				const double fee = std::max(0.0,
					ws.crisis.credit_penalty * base);
				double penalty = 0.0;
				if (claims) {
					const GroupPaid paid = pay_group_capped(ws, group, fee,
						std::max(0.0, bank.deposits));
					penalty = paid.total();
					bank.deposits = std::max(0.0,
						bank.deposits - paid.cash);
					bank.cash += paid.claim;
					bank.interest_income += penalty;
					money_delta -= paid.cash;
				} else {
					penalty = std::min({
						std::max(0.0, group.cash), fee,
						std::max(0.0, bank.deposits) });
					if (penalty > 0.0) {
						group.cash -= penalty;
						bank.deposits -= penalty;
						bank.interest_income += penalty;
						money_delta -= penalty;
					}
				}
				// D-126: a parte da multa que o caixa não cobre é capitalizada
				// no principal (o tomador deve os 5% a mais). Nenhum dinheiro é
				// criado: só a dívida e o ativo do banco crescem juntos.
				const double capitalized = std::max(0.0, fee - penalty);
				const double recap = group.credit_arrears + capitalized;
				group.debt += recap;
				bank.loans += recap;
				if (loan != nullptr) {
					loan->principal += recap;
					loan->arrears = 0.0;
					loan->default_days = 0;
				}
				bank.arrears = std::max(0.0,
					bank.arrears - group.credit_arrears);
				group.credit_arrears = 0.0;
				group.credit_arrears_days = 0;
				group.credit_renegotiations += 1;
				// D-126: o premio `credit_rate_add` entra UMA vez, quando o
				// credito de giro e' concedido (crisis::borrow_group). A
				// renegociacao capitaliza a multa, mas nao soma o premio de novo.
				ws.crisis_renegotiations += 1;
				ws.crisis_penalty_paid += fee;
			}
			// 2. Amortização: só o excedente ao capital de giro (30 dias de
			// custo efetivamente pago) e na fração declarada. Destrói depósito.
			const double working = kWorkingCapitalDays
				* std::max(0.0, sheets[index].daily_cost);
			const double excess = std::max(0.0,
				(claims ? group.liquid_cash() : group.cash) - working);
			if (claims) {
				const double repay = std::min({ loan_principal,
					pol.repay_share * excess,
					std::max(0.0, bank.deposits)
						+ (group.liquid_cash() - group.cash) });
				if (repay > 0.0) {
					const GroupPaid paid = pay_group_capped(ws, group, repay,
						std::max(0.0, bank.deposits));
					group.debt = std::max(0.0, group.debt - paid.total());
					group.credit_repaid += paid.total();
					bank.loans = std::max(0.0,
						bank.loans - paid.total());
					bank.deposits = std::max(0.0,
						bank.deposits - paid.cash);
					// A parcela do claim ja saiu do caixa e volta ao banco no
					// pagamento do principal (nao destroi D_l).
					bank.cash += paid.claim;
					money_delta -= paid.cash;
					if (loan != nullptr) {
						loan->principal = std::max(0.0,
							loan->principal - paid.total());
					}
				}
			} else {
				double repay = std::min({ group.debt,
					pol.repay_share * excess,
					std::max(0.0, bank.deposits) });
				if (repay > 0.0) {
					group.cash -= repay;
					group.debt -= repay;
					group.credit_repaid += repay;
					bank.loans -= repay;
					bank.deposits -= repay;
					money_delta -= repay;
				}
			}
			// 3. Insolvência: ativos reais + caixa não cobrem a dívida. Conta
			// dias e, passado o prazo, o credor executa. R2b1: com varios
			// credores o dia conta UMA vez (so no primeiro claim em aberto) e
			// a execucao e' bloqueada enquanto houver mais de um credor.
			const bool default_tick = !claims
				|| (first_outstanding_claim(group.debt_claims) != nullptr
					&& first_outstanding_claim(group.debt_claims)->bank_id
						== bank.id);
			if (default_tick) {
				if (sheets[index].net_worth() < 0.0) {
					group.credit_default_days += 1;
				} else {
					group.credit_default_days = 0;
				}
			}
			if (group.credit_default_days < pol.default_days) {
				continue;
			}
			if (claims && outstanding_claims(group.debt_claims) > 1) {
				continue;
			}
			const double claim = claims
				? loan_principal + loan_arrears
				: group.debt + group.credit_arrears;
			double seized = 0.0;
			if (claims) {
				GroupPaid paid = pay_group_capped(ws, group,
					std::min(claim, group.liquid_cash()),
					std::max(0.0, bank.deposits));
				seized = paid.total();
				// O caixa tomado sai do depósito e vira caixa do banco; a
				// parcela do claim já foi retirada e devolvida.
				bank.deposits = std::max(0.0, bank.deposits - paid.cash);
				bank.cash += paid.cash;
				bank.cash += paid.claim;
			} else {
				seized = std::min({ std::max(0.0, group.cash),
					claim, std::max(0.0, bank.deposits) });
				if (seized > 0.0) {
					group.cash -= seized;
					// O caixa tomado sai do depósito e vira caixa do banco: os dois
					// lados já estão em `money_total`, então não há criação nem
					// destruição de dinheiro (por isso `money_delta` não muda).
					bank.deposits -= seized;
					bank.cash += seized;
				}
			}
			const double writeoff = std::max(0.0, claim - seized);
			bank.loans = std::max(0.0,
				bank.loans - (claims ? loan_principal : group.debt));
			bank.arrears = std::max(0.0,
				bank.arrears - (claims ? loan_arrears : group.credit_arrears));
			bank.writeoffs += writeoff;
			bank.defaults += 1;
			bank.last_default_day = ws.day;
			group.debt = claims ? std::max(0.0, group.debt - loan_principal)
				: 0.0;
			group.credit_arrears = claims
				? std::max(0.0, group.credit_arrears - loan_arrears) : 0.0;
			if (loan != nullptr) {
				loan->principal = 0.0;
				loan->arrears = 0.0;
				loan->default_days = 0;
			}
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
					// D-126: a execução bancária entra na fila da crise como
					// falência (o prédio vai a leilão antes de sumir). Sem a
					// crise ligada, preserva a regra antiga.
					if (ws.crisis.enabled) {
						building.loss_days = std::max(building.loss_days,
							ws.crisis.bankrupt_days);
					} else {
						building.bankrupt = true;
					}
				}
			}
			defaults += 1;
		}
	}
	// 4. D-126: crédito de capital de giro das firmas privadas/serviços da
	// cidade (credor = banco público). Mesma matemática da C3: juro e
	// amortização destroem depósito; 1 renegociação com multa; calote depois
	// do prazo. O caixa agregado da cidade é o tomador.
	for (std::size_t city_index = 0; city_index < ws.cities.size();
			++city_index) {
		City& city = ws.cities[city_index];
		if (city.firm_debt <= 0.0 && city.firm_credit_arrears <= 0.0) {
			city.firm_credit_default_days = 0;
			continue;
		}
		// R2b1: com depositos o servicing e' por credor e a execucao
		// multi-credor fica bloqueada (ver `service_city_claims`).
		if (claims) {
			service_city_claims(ws, city_index, pol, &money_delta);
			continue;
		}
		Bank* city_bank = nullptr;
		if (claims) {
			for (const BankLoan& loan : city.firm_debt_claims) {
				if (loan.principal + loan.arrears > 1e-9) {
					const auto found = ws.banks.find(loan.bank_id);
					if (found != ws.banks.end()) {
						city_bank = &found->second;
					}
					break;
				}
			}
		}
		if (city_bank == nullptr) {
			city_bank = &ensure(ws, city.nation, kRolePublic);
		}
		if (city_bank->id.empty()) {
			city_bank = &ensure(ws, city.nation);
		}
		if (city_bank == nullptr) {
			continue;
		}
		const double city_rate = loan_rate(ws, city.nation)
			+ std::max(0.0, city.firm_credit_rate_add);
		double paid_interest = 0.0;
		double interest = 0.0;
		if (claims) {
			interest = city.firm_debt * city_rate / kDaysPerYear;
			double cash_paid = 0.0;
			double claim_paid = 0.0;
			paid_interest = pay_city_liquid(ws, city, interest,
				std::max(0.0, city_bank->deposits), &cash_paid, &claim_paid);
			city_bank->deposits = std::max(0.0,
				city_bank->deposits - cash_paid);
			city_bank->cash += claim_paid;
			city_bank->interest_income += paid_interest;
			money_delta -= cash_paid;
			if (interest - paid_interest > 1e-12) {
				city.firm_credit_arrears += interest - paid_interest;
				city_bank->arrears += interest - paid_interest;
				city.firm_credit_default_days += 1;
			} else {
				city.firm_credit_default_days = 0;
			}
		} else {
			interest = std::min(
				city.firm_debt * city_rate / kDaysPerYear,
				std::max(0.0, city_bank->deposits));
			double remaining = std::max(0.0, interest);
			const double from_firm = std::min(remaining,
				city.firm_external_room_request(remaining));
			if (from_firm > 0.0) {
				city.firm_external_pay(from_firm, City::kR1Finance);
				paid_interest += from_firm;
				remaining -= from_firm;
			}
			if (remaining > 0.0) {
				const double from_service = std::min(remaining,
					city.service_external_room_request(remaining));
				if (from_service > 0.0) {
					city.service_external_pay(from_service, City::kR1Finance);
					paid_interest += from_service;
					remaining -= from_service;
				}
			}
			if (paid_interest > 0.0) {
				city_bank->deposits -= paid_interest;
				city_bank->interest_income += paid_interest;
				money_delta -= paid_interest;
			}
			if (remaining > 1e-12) {
				city.firm_credit_arrears += remaining;
				city_bank->arrears += remaining;
				city.firm_credit_default_days += 1;
			} else {
				city.firm_credit_default_days = 0;
			}
		}
		// Amortização só do excedente ao capital de giro protegido.
		const double working = kWorkingCapitalDays
			* city_business_daily_cost(ws, city_index);
		const double excess = std::max(0.0, city.firm_liquidity() - working);
		if (claims) {
			const double claim_room = std::max(0.0,
				city.firm_liquidity() - city.private_total());
			const double want_repay = std::min({ city.firm_debt,
				pol.repay_share * excess,
				std::max(0.0, city_bank->deposits) + claim_room });
			double cash_paid = 0.0;
			double claim_paid = 0.0;
			const double repay = pay_city_liquid(ws, city, want_repay,
				std::max(0.0, city_bank->deposits), &cash_paid, &claim_paid);
			if (repay > 0.0) {
				city.firm_debt = std::max(0.0, city.firm_debt - repay);
				city_bank->loans = std::max(0.0,
					city_bank->loans - repay);
				city_bank->deposits = std::max(0.0,
					city_bank->deposits - cash_paid);
				city_bank->cash += claim_paid;
				money_delta -= cash_paid;
			}
		} else {
			const double want_repay = std::min({ city.firm_debt,
				pol.repay_share * excess,
				std::max(0.0, city_bank->deposits) });
			const double repay = std::min(want_repay,
				city.firm_external_room_request(want_repay));
			if (repay > 0.0) {
				city.firm_external_pay(repay, City::kR1Finance);
				city.firm_debt -= repay;
				city_bank->loans -= repay;
				city_bank->deposits -= repay;
				money_delta -= repay;
			}
		}
		// Renegociação (máx. declarado): multa + atraso capitalizado.
		if (ws.crisis.enabled
				&& city.firm_credit_arrears > 1e-9
				&& city.firm_credit_renegotiations
					< std::max(0, ws.crisis.credit_max_renegotiations)
				&& city.firm_credit_default_days
					>= std::max(1, ws.crisis.credit_renegotiation_days)) {
			const double base = city.firm_debt + city.firm_credit_arrears;
			const double fee = std::max(0.0,
				ws.crisis.credit_penalty * base);
			if (claims) {
				const double claim_room = std::max(0.0,
					city.firm_liquidity() - city.private_total());
				const double target = std::min({ fee,
					city.firm_liquidity(),
					std::max(0.0, city_bank->deposits) + claim_room });
				double cash_paid = 0.0;
				double claim_paid = 0.0;
				const double paid = pay_city_liquid(ws, city, target,
					std::max(0.0, city_bank->deposits), &cash_paid,
					&claim_paid);
				if (paid > 0.0) {
					city_bank->deposits = std::max(0.0,
						city_bank->deposits - cash_paid);
					city_bank->cash += claim_paid;
					city_bank->interest_income += paid;
					money_delta -= cash_paid;
					ws.crisis_penalty_paid += paid;
				}
				const double capitalized = std::max(0.0, fee - paid);
				city.firm_debt += capitalized;
				city_bank->loans += capitalized;
			} else {
				const double penalty = std::min({
					std::max(0.0, city.private_total()),
					fee,
					std::max(0.0, city_bank->deposits) });
				if (penalty > 0.0) {
					double left = penalty;
					const double p_firm = std::min(left,
						city.firm_external_room_request(left));
					city.firm_external_pay(p_firm, City::kR1Finance);
					left -= p_firm;
					if (left > 0.0) {
						const double p_service = std::min(left,
							city.service_external_room_request(left));
						city.service_external_pay(p_service, City::kR1Finance);
						left -= p_service;
					}
					const double paid = penalty - left;
					city_bank->deposits -= paid;
					city_bank->interest_income += paid;
					money_delta -= paid;
					ws.crisis_penalty_paid += paid;
				}
				// D-126: a parte da multa que o caixa não cobre é capitalizada
				// no principal — dívida e ativo do banco crescem juntos, sem
				// dinheiro.
				const double capitalized = std::max(0.0, fee - penalty);
				city.firm_debt += city.firm_credit_arrears + capitalized;
				city_bank->loans += city.firm_credit_arrears + capitalized;
			}
			city_bank->arrears = std::max(0.0,
				city_bank->arrears - city.firm_credit_arrears);
			city.firm_credit_arrears = 0.0;
			city.firm_credit_default_days = 0;
			city.firm_credit_renegotiations += 1;
			// D-126: o premio `credit_rate_add` entra UMA vez, quando o credito
			// de giro e' concedido (crisis::borrow_city). A renegociacao
			// capitaliza a multa, mas nao soma o premio de novo.
			ws.crisis_renegotiations += 1;
		}
		// Calote após a renegociação: o caixa é tomado e o resto é perda do
		// banco. Trocar depósito por caixa não muda o `money_total`.
		if (city.firm_credit_default_days
				>= std::max(1, pol.default_days)
				&& city.firm_credit_renegotiations
					>= std::max(0, ws.crisis.credit_max_renegotiations)) {
			const double claim = city.firm_debt + city.firm_credit_arrears;
			const double available = city.private_liquidity();
			const double seized = std::min({ available, claim,
				std::max(0.0, city_bank->deposits) });
			if (seized > 0.0) {
				double left = seized;
				const double s_firm = std::min(left,
					city.firm_external_room_request(left));
				city.firm_external_pay(s_firm, City::kR1Finance);
				left -= s_firm;
				if (left > 0.0) {
					const double s_service = std::min(left,
						city.service_external_room_request(left));
					city.service_external_pay(s_service, City::kR1Finance);
					left -= s_service;
				}
				city_bank->deposits -= seized - left;
				city_bank->cash += seized - left;
			}
			city_bank->loans = std::max(0.0,
				city_bank->loans - city.firm_debt);
			city_bank->arrears = std::max(0.0,
				city_bank->arrears - city.firm_credit_arrears);
			city_bank->writeoffs += std::max(0.0,
				claim - seized);
			city_bank->defaults += 1;
			city_bank->last_default_day = ws.day;
			city.firm_debt = 0.0;
			city.firm_credit_arrears = 0.0;
			city.firm_credit_default_days = 0;
			city.firm_credit_defaults += 1;
		}
	}
	// 4. Dividendo mensal ao dono (lucro retido × payout declarado).
	if (ws.day > 0 && ws.day % kDividendPeriodDays == 0) {
		double deposits_before = 0.0;
		if (!(ws.banks_rework.enabled && ws.banks_rework.capital)) {
			for (const auto& row : ws.banks) {
				deposits_before += row.second.deposits;
			}
		}
		distribute_dividends(ws, pol);
		if (!(ws.banks_rework.enabled && ws.banks_rework.capital)) {
			double deposits_after = 0.0;
			for (const auto& row : ws.banks) {
				deposits_after += row.second.deposits;
			}
			// Caminho legado: o dividendo credita o dono e cria o depósito
			// correspondente (dinheiro bancário do dia). No R2a o dividendo é
			// transferência de caixa e `money_delta` não muda.
			money_delta += deposits_after - deposits_before;
		}
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
		out.deposits_funding += bank.deposits_funding;
		out.capital_target += bank.capital_target;
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
	const double assets = out.loans + out.arrears;
	out.capital_ratio = assets > 1e-9 ? out.equity / assets : 0.0;
	out.paid_in_ratio = assets > 1e-9 ? out.capital / assets : 0.0;
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

namespace {

constexpr double kDaysPerMonth = 365.0 / 12.0;

bool bank_present(const Bank& bank) {
	return bank.name_status == "present" && bank.absorbed_into.empty();
}

City::BankAccount* account_of(City& city, const std::string& bank_id) {
	auto found = std::lower_bound(city.bank_accounts.begin(),
		city.bank_accounts.end(), bank_id,
		[](const City::BankAccount& account, const std::string& id) {
			return account.bank_id < id;
		});
	if (found != city.bank_accounts.end() && found->bank_id == bank_id) {
		return &*found;
	}
	City::BankAccount account;
	account.bank_id = bank_id;
	return &*city.bank_accounts.insert(found, account);
}

const City::BankAccount* find_account(const City& city,
		const std::string& bank_id) {
	auto found = std::lower_bound(city.bank_accounts.begin(),
		city.bank_accounts.end(), bank_id,
		[](const City::BankAccount& account, const std::string& id) {
			return account.bank_id < id;
		});
	return found != city.bank_accounts.end() && found->bank_id == bank_id
		? &*found : nullptr;
}

double claim_of(const WorldState& ws, const std::string& bank_id) {
	double total = 0.0;
	for (const City& city : ws.cities) {
		if (const City::BankAccount* account = find_account(city, bank_id)) {
			total += std::max(0.0, account->firm)
				+ std::max(0.0, account->service);
		}
	}
	for (const Group& group : ws.groups) {
		if (group.deposit_bank_id == bank_id) {
			total += std::max(0.0, group.bank_deposit);
		}
	}
	return total;
}

// Escolhe o banco do deposito por relacionamento + solidez + pendulo
// deterministico. Sem banco presente devolve nullptr (o caixa fica em casa).
Bank* choose_deposit_bank(WorldState& ws, const City& city, bool firm) {
	const BanksRework& br = ws.banks_rework;
	std::vector<Bank*> candidates;
	for (auto& row : ws.banks) {
		Bank& bank = row.second;
		if (bank.nation != city.nation || !bank_present(bank)) {
			continue;
		}
		candidates.push_back(&bank);
	}
	if (candidates.empty()) {
		return nullptr;
	}
	const auto solvency_of = [](const Bank& bank) {
		const double assets = bank.loans + bank.arrears;
		if (assets <= 1e-9) {
			return 0.0;
		}
		return std::clamp(equity(bank) / assets, 0.0, 1.0);
	};
	// Solidez minima: quando ha algum banco acima do piso, os abaixo ficam
	// fora da alocacao (o piso de 8% declarado no bloco).
	bool any_solid = false;
	for (const Bank* bank : candidates) {
		if (solvency_of(*bank) >= br.alloc_min_capital_ratio) {
			any_solid = true;
			break;
		}
	}
	const double total_claim = std::max(1e-9, city.claim_total());
	const std::uint64_t month = static_cast<std::uint64_t>(
		std::max(0, ws.day) / static_cast<int>(kDaysPerMonth));
	Bank* best = nullptr;
	double best_score = -1.0;
	for (Bank* bank : candidates) {
		const double sol = solvency_of(*bank);
		if (any_solid && sol < br.alloc_min_capital_ratio) {
			continue;
		}
		double relation = 0.0;
		if (const City::BankAccount* account =
				find_account(city, bank->id)) {
			relation = (std::max(0.0, account->firm)
				+ std::max(0.0, account->service)) / total_claim;
		}
		const std::uint64_t roll = stable_hash(city.id + "|"
			+ std::string(role_key(bank->role)) + "|"
			+ std::to_string(month) + (firm ? "|f" : "|s"));
		const double unit = static_cast<double>(roll % 10000ULL) / 10000.0;
		const double pendulum = 1.0 - br.alloc_pendulum_month
			+ 2.0 * br.alloc_pendulum_month * unit;
		const double score = br.alloc_w_rel * relation
			+ br.alloc_w_sol * sol + br.alloc_w_pend * pendulum;
		if (score > best_score
				|| (score == best_score
					&& (best == nullptr || bank->id < best->id))) {
			best = bank;
			best_score = score;
		}
	}
	return best;
}

// Move caixa do setor privado da cidade para o banco e cria o claim.
void sweep_city_sub(WorldState& ws, City& city, bool firm, double amount) {
	if (amount <= 1e-9) {
		return;
	}
	Bank* bank = choose_deposit_bank(ws, city, firm);
	if (bank == nullptr) {
		return;
	}
	const double value = std::min(amount,
		firm ? std::max(0.0, city.firm_sub)
			: std::max(0.0, city.service_sub));
	if (value <= 1e-9) {
		return;
	}
	City::BankAccount* account = account_of(city, bank->id);
	if (firm) {
		account->firm += value;
		city.firm_sub = std::max(0.0, city.firm_sub - value);
		city.firm_cash = city.firm_sub;
	} else {
		account->service += value;
		city.service_sub = std::max(0.0, city.service_sub - value);
		city.service_cash = city.service_sub;
	}
	city.private_cash = std::max(0.0, city.private_cash - value);
	bank->cash += value;
	bank->deposits_funding += value;
}

} // namespace

double deposit_claims_total(const WorldState& ws) {
	double total = 0.0;
	for (const City& city : ws.cities) {
		total += city.claim_total();
	}
	for (const Group& group : ws.groups) {
		total += std::max(0.0, group.bank_deposit);
	}
	return total;
}

double liquidity_floor(const WorldState& ws, const Bank& bank) {
	return std::max(0.0, ws.banks_rework.liquidity_min_reserve_ratio)
		* total_deposits(bank);
}

void seed_capital(WorldState& ws) {
	const BanksRework& br = ws.banks_rework;
	if (!br.enabled || !br.capital || !br.seed_enabled) {
		return;
	}
	if (br.seed_source == "loaded_save") {
		return;
	}
	if (br.seeded) {
		return;
	}
	if (!ws.bank_seed.loaded || ws.bank_seed.nations.empty()) {
		ws.banks_rework.seed_source = "no_model_gdp";
		return;
	}
	const double target_ratio = ws.bank_seed.capital_to_loans_target > 0.0
		? ws.bank_seed.capital_to_loans_target : 0.10;
	for (const auto& row : ws.bank_seed.nations) {
		const std::string& nation = row.first;
		const BankSeedRow& seed = row.second;
		const double model_target = target_ratio * std::max(0.0, seed.steady_loans);
		const double reference_target = seed.reference_target > 0.0
			? seed.reference_target
			: (seed.has_capital_pct_gdp
				? seed.capital_pct_gdp * std::max(0.0, seed.model_gdp_annual)
				: 0.0);
		// Papéis presentes SOMENTE (`present`); none/no_data nunca capitaliza.
		std::vector<Bank*> present;
		std::vector<double> present_weight;
		double weight_sum = 0.0;
		for (const int role : {kRolePublic, kRolePrivate, kRoleForeign}) {
			Bank* bank = find_mut(ws, nation, role);
			if (bank == nullptr || !bank_present(*bank)) {
				if (bank != nullptr) {
					bank->charter = bank->name_status == "none"
						? "none" : "no_data";
					bank->seed_scale = 0.0;
					bank->reference_scale = 0.0;
					bank->capital_target = 0.0;
				}
				continue;
			}
			// `bank.share` fica com a renormalizacao da C4/C5 (soma 1 sobre as
			// LINHAS de banco existentes, inclusive `no_data`): a C5 rateia as
			// acoes do grupo por ela. O seed usa o peso local so para o alvo.
			const double weight = role == kRolePublic ? kSharePublic
				: (role == kRolePrivate ? kSharePrivate : kShareForeign);
			weight_sum += weight;
			present.push_back(bank);
			present_weight.push_back(weight);
		}
		if (present.empty() || weight_sum <= 1e-9) {
			continue;
		}
		for (std::size_t slot = 0; slot < present.size(); ++slot) {
			Bank* bank = present[slot];
			bank->capital_target = model_target * present_weight[slot]
				/ weight_sum;
			bank->capital_ratio_target = br.dividend_target_ratio;
			bank->charter = "declared";
			bank->seed_scale = 0.0;
			bank->reference_scale = 0.0;
			bank->public_cap_deferred.clear();
		}
		// P1: privado no dia 0 (realocacao das familias); publico = 0 no dia 0
		// (o Tesouro nasce vazio e a capitalizacao entra no fecho mensal);
		// estrangeiro = 0.
		if (Bank* private_bank = find_mut(ws, nation, kRolePrivate)) {
			if (bank_present(*private_bank)
					&& private_bank->capital_target > 1e-9) {
				const double want = private_bank->capital_target;
				const double paid = pay_from_households(ws, nation, want);
				if (paid > 0.0) {
					private_bank->cash += paid;
					private_bank->capital += paid;
				}
				private_bank->seed_scale = paid / want;
			}
		}
		for (Bank* bank : present) {
			if (bank->role == kRolePrivate) {
				continue;
			}
			bank->seed_scale = 0.0;
		}
		if (reference_target > 0.0) {
			for (Bank* bank : present) {
				const double role_reference = reference_target * bank->share;
				if (role_reference > 1e-9) {
					bank->reference_scale = bank->capital / role_reference;
				}
			}
		}
	}
	ws.banks_rework.seed_source = "world_new";
	ws.banks_rework.seeded = true;
}

void capitalize_public_monthly(WorldState& ws) {
	const BanksRework& br = ws.banks_rework;
	if (!br.enabled || !br.capital || !br.seed_enabled) {
		return;
	}
	if (br.seed_source == "loaded_save") {
		return;
	}
	for (auto& row : ws.banks) {
		Bank& bank = row.second;
		if (bank.role != kRolePublic || !bank_present(bank)) {
			continue;
		}
		if (bank.capital_target <= 1e-9) {
			bank.public_cap_deferred = "no_model_target";
			continue;
		}
		const double remaining = std::max(0.0,
			bank.capital_target - bank.capital);
		if (remaining <= 1e-9) {
			bank.public_cap_deferred.clear();
			continue;
		}
		if (!treasury::enabled(ws)) {
			bank.public_cap_deferred = "treasury_disabled";
			continue;
		}
		const double balance = treasury::balance(ws, bank.nation);
		const double day = static_cast<double>(std::max(1, ws.day));
		const double daily_spend = std::max(0.0,
			ws.treasury_spent_total / day);
		const double floor = br.seed_floor_days_treasury * daily_spend;
		const double available = std::max(0.0, balance - floor);
		const double ticket = br.seed_public_month_frac * available;
		const double pay = std::min(remaining, ticket);
		const double min_ticket = br.seed_public_min_ticket_frac * remaining;
		if (pay <= 1e-9 || pay < min_ticket) {
			bank.public_cap_deferred = "ticket_below_min";
			continue;
		}
		const double paid = treasury::spend(ws, bank.nation, pay);
		if (paid <= 1e-9) {
			bank.public_cap_deferred = "no_treasury_cash";
			continue;
		}
		bank.cash += paid;
		bank.capital += paid;
		bank.public_cap_deferred.clear();
		if (bank.capital_target > 1e-9) {
			bank.seed_scale = bank.capital / bank.capital_target;
		}
	}
}

void sweep_deposits(WorldState& ws) {
	const BanksRework& br = ws.banks_rework;
	if (!br.enabled || !br.deposits) {
		return;
	}
	// R2b1: o claim e' sacavel no mesmo dia - liga cidades/grupos ao mundo
	// (o ponteiro e' derivado e nao entra no save).
	deposit::link(ws);
	// R2b1: no fecho mensal a custodia e' realocada pelo score declarado
	// antes do sweep do dia (o novo funding ja entra no banco escolhido).
	allocate_deposits_monthly(ws);
	for (City& city : ws.cities) {
		// R2a (QA B4): colchao pela media de 30 dias do gasto com comida, nao
		// pelo valor do dia. Um dia de escassez nao pode encolher o piso. A
		// ultima amostra e' substituida se o sweep rodar de novo no mesmo dia
		// (teste/chamada repetida nao duplica o dia).
		const double sample = std::max(0.0, city.pop.food_spend);
		if (city.food_spend_hist_day == ws.day && city.food_spend_hist_n > 0) {
			city.food_spend_hist[
				(city.food_spend_hist_pos + 29) % 30] = sample;
		} else {
			city.food_spend_hist[city.food_spend_hist_pos] = sample;
			city.food_spend_hist_pos =
				(city.food_spend_hist_pos + 1) % 30;
			city.food_spend_hist_n = std::min(city.food_spend_hist_n + 1, 30);
			city.food_spend_hist_day = ws.day;
		}
		double food_daily = 0.0;
		for (int slot = 0; slot < city.food_spend_hist_n; ++slot) {
			food_daily += city.food_spend_hist[slot];
		}
		food_daily = city.food_spend_hist_n > 0
			? food_daily / static_cast<double>(city.food_spend_hist_n)
			: 0.0;
		const double budget = std::max(0.0,
			city.regional_budget_limit_month);
		city.protected_cash = std::max(0.0, br.protected_food_months)
			* kDaysPerMonth * food_daily + budget;
		if (!city.unified_cash) {
			continue;
		}
		const double total = std::max(0.0, city.private_cash);
		if (total <= 1e-9) {
			continue;
		}
		const double firm_sub = std::max(0.0, city.firm_sub);
		const double service_sub = std::max(0.0, city.service_sub);
		const double firm_share = total > 1e-9 ? firm_sub / total : 0.0;
		const double firm_excess = std::max(0.0,
			firm_sub - city.protected_cash * firm_share);
		const double service_excess = std::max(0.0,
			service_sub - city.protected_cash * (1.0 - firm_share));
		sweep_city_sub(ws, city, true, firm_excess);
		sweep_city_sub(ws, city, false, service_excess);
	}
	// Empresas: o excesso do caixa do grupo acima do float (5 dias de custo
	// medido) vai ao banco de relacionamento (o mesmo `lender()`).
	const std::vector<balance::GroupSheet> sheets = balance::group_sheets(ws);
	for (std::size_t index = 0; index < ws.groups.size(); ++index) {
		Group& group = ws.groups[index];
		const double daily_cost = index < sheets.size()
			? std::max(0.0, sheets[index].daily_cost) : 0.0;
		if (daily_cost <= 1e-9) {
			continue;
		}
		const double float_cash = 5.0 * daily_cost;
		const double excess = std::max(0.0, group.cash - float_cash);
		if (excess <= 1e-9) {
			continue;
		}
		// O claim do grupo e' UM so (deposit_bank_id): se ele ja tem deposito num
		// banco presente, o sweep continua nele. Trocar de banco deixaria a
		// passividade D_f no antigo e o claim apontando para o novo (Σ claims
		// != Σ D_f). A competicao por banco e' do R2b, nao do sweep.
		Bank* bank = nullptr;
		if (group.bank_deposit > 1e-9 && !group.deposit_bank_id.empty()) {
			const auto current = ws.banks.find(group.deposit_bank_id);
			if (current != ws.banks.end() && bank_present(current->second)) {
				bank = &current->second;
			}
		}
		if (bank == nullptr) {
			bank = lender(ws, group);
		}
		if (bank == nullptr) {
			continue;
		}
		// O lender() ja devolve o banco mutavel; garante o papel presente.
		if (!bank_present(*bank)) {
			continue;
		}
		const double value = std::min(excess, std::max(0.0, group.cash));
		if (value <= 1e-9) {
			continue;
		}
		group.cash -= value;
		group.bank_deposit += value;
		group.deposit_bank_id = bank->id;
		bank->cash += value;
		bank->deposits_funding += value;
	}
}

void allocate_deposits_monthly(WorldState& ws) {
	const BanksRework& br = ws.banks_rework;
	if (!br.enabled || !br.deposits) {
		return;
	}
	if (ws.day <= 0 || ws.day % kDividendPeriodDays != 0) {
		return;
	}
	deposit::link(ws);
	for (City& city : ws.cities) {
		if (city.bank_accounts.empty()) {
			continue;
		}
		for (const bool firm : {true, false}) {
			Bank* target = choose_deposit_bank(ws, city, firm);
			if (target == nullptr) {
				continue;
			}
			// Snapshot dos bancos de origem: `account_of` pode inserir e
			// invalidar iteradores do vetor de contas durante a movimentacao.
			std::vector<std::string> ids;
			for (const City::BankAccount& account : city.bank_accounts) {
				const double claim = firm ? account.firm : account.service;
				if (claim > 1e-9 && account.bank_id != target->id) {
					ids.push_back(account.bank_id);
				}
			}
			std::sort(ids.begin(), ids.end());
			const std::string target_id = target->id;
			for (const std::string& id : ids) {
				const auto source = ws.banks.find(id);
				if (source == ws.banks.end()) {
					continue;
				}
				Bank& from = source->second;
				City::BankAccount* account = account_of(city, id);
				double& claim = firm ? account->firm : account->service;
				const double value = std::max(0.0, claim);
				// Move claim e caixa de lastro JUNTOS; sem caixa no banco de
				// origem, a parcela fica (nao se inventa reserva).
				const double move = std::min(value, std::max(0.0, from.cash));
				if (move <= 1e-9) {
					continue;
				}
				claim -= move;
				if (claim < 1e-9) {
					claim = 0.0;
				}
				City::BankAccount* dst = account_of(city, target_id);
				(firm ? dst->firm : dst->service) += move;
				from.cash = std::max(0.0, from.cash - move);
				from.deposits_funding = std::max(0.0,
					from.deposits_funding - move);
				target->cash += move;
				target->deposits_funding += move;
			}
		}
	}
}

void reassign_claims(WorldState& ws, const std::string& from_bank,
		const std::string& to_bank) {
	if (from_bank.empty() || to_bank.empty() || from_bank == to_bank) {
		return;
	}
	for (City& city : ws.cities) {
		auto found = std::lower_bound(city.bank_accounts.begin(),
			city.bank_accounts.end(), from_bank,
			[](const City::BankAccount& account, const std::string& id) {
				return account.bank_id < id;
			});
		if (found == city.bank_accounts.end()
				|| found->bank_id != from_bank) {
			continue;
		}
		const double firm = std::max(0.0, found->firm);
		const double service = std::max(0.0, found->service);
		city.bank_accounts.erase(found);
		if (firm > 0.0 || service > 0.0) {
			City::BankAccount* target = account_of(city, to_bank);
			target->firm += firm;
			target->service += service;
		}
	}
	for (Group& group : ws.groups) {
		if (group.deposit_bank_id == from_bank) {
			group.deposit_bank_id = to_bank;
		}
	}
}

double cut_funding_claims(WorldState& ws, const std::string& bank_id,
		double amount) {
	const auto found = ws.banks.find(bank_id);
	if (found == ws.banks.end()) {
		return 0.0;
	}
	Bank& bank = found->second;
	const double total = std::min(std::max(0.0, amount),
		std::max(0.0, bank.deposits_funding));
	if (total <= 1e-9) {
		return 0.0;
	}
	const double factor = 1.0 - total / std::max(1e-9, bank.deposits_funding);
	double cut = 0.0;
	for (City& city : ws.cities) {
		for (City::BankAccount& account : city.bank_accounts) {
			if (account.bank_id != bank_id) {
				continue;
			}
			const double firm = account.firm * factor;
			const double service = account.service * factor;
			cut += (account.firm - firm) + (account.service - service);
			account.firm = firm;
			account.service = service;
		}
	}
	for (Group& group : ws.groups) {
		if (group.deposit_bank_id != bank_id) {
			continue;
		}
		const double left = group.bank_deposit * factor;
		cut += group.bank_deposit - left;
		group.bank_deposit = left;
	}
	bank.deposits_funding = std::max(0.0,
		bank.deposits_funding - cut);
	return cut;
}

} // namespace bank
} // namespace ogs
