// C6 (D-114) — crise bancaria, corrida e resolucao.
// Contrato em bank_crisis.h e docs/economy/C6_bank_crisis.md. Todo parametro
// vem de data/definitions/economy/bank_crisis.json (default de jogo) e as
// taxas vem da politica monetaria da moeda (C2b1). Nada aqui inventa numero.

#include "bank_crisis.h"

#include "bank.h"
#include "monetary.h"
#include "stock.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

namespace ogs {
namespace bank_crisis {

namespace {

constexpr double kEps = 1e-9;

double pick(double value, double fallback) {
	return value > 0.0 ? value : fallback;
}

double clamp01(double value) {
	if (!std::isfinite(value)) {
		return 0.0;
	}
	return std::clamp(value, 0.0, 1.0);
}

// Codigo da moeda da nacao (vazio = sem moeda no estado: ausencia declarada).
std::string currency_of(const WorldState& ws, const std::string& nation) {
	return bank::currency_of_nation(ws, nation);
}

bool nation_in(const Currency& currency, const std::string& nation) {
	return std::find(currency.nations.begin(), currency.nations.end(), nation)
		!= currency.nations.end();
}

MonetaryState* monetary_of(WorldState& ws, const std::string& currency) {
	const auto found = ws.monetary.find(currency);
	return found == ws.monetary.end() ? nullptr : &found->second;
}

const StockExchange* exchange_of(const WorldState& ws,
		const std::string& nation) {
	const auto found = ws.stock_exchanges.find(nation);
	return found == ws.stock_exchanges.end() ? nullptr : &found->second;
}

// Tesouro do modelo: o orcamento das cidades da nacao (a maior primeiro,
// ordem deterministica). Devolve o efetivamente pago.
double pay_from_government(WorldState& ws, const std::string& nation,
		double amount) {
	if (!(amount > 0.0)) {
		return 0.0;
	}
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
		const double take = std::min(amount - paid,
			std::max(0.0, city->government_budget));
		city->government_budget -= take;
		paid += take;
	}
	return paid;
}

// Dívida pública carregada pelo BC da moeda (ou pelo exterior quando a nação
// não tem BC: dolarizado/sem moeda própria). Conserva dinheiro: com BC, ele
// emite o valor creditado ao banco e o título entra na carteira dele; sem BC,
// o título fica com o SETOR EXTERIOR, pago da posição externa (`foreign_cash`,
// que pode ficar negativa por desenho, como na C4) e registrado em
// `external_bonds` (claim do credor em `Ledger.external_claims`). A ponta do
// credor é obrigatória: sem ela `financial_sum = −valor` e o tick é revertido.
double rescue_with_debt(WorldState& ws, const std::string& nation,
		double amount, bool* out_emitted) {
	if (out_emitted != nullptr) {
		*out_emitted = false;
	}
	if (!(amount > 0.0)) {
		return 0.0;
	}
	ws.gov_bonds[nation] += amount;
	const std::string code = currency_of(ws, nation);
	const bool lender = !code.empty()
		&& std::find(ws.fx_no_policy_nations.begin(),
			ws.fx_no_policy_nations.end(), nation)
			== ws.fx_no_policy_nations.end();
	// Moeda com BC, mas SEM estado monetário salvo: não há carteira do BC nem
	// emissão registrada possível. O título vai para o exterior (nunca um
	// `emission_today` sem o título correspondente).
	MonetaryState* state = lender ? monetary_of(ws, code) : nullptr;
	if (state != nullptr) {
		state->emission += amount;
		state->emission_today += amount;
		state->securities += amount;
		ws.emission_today += amount;
		if (out_emitted != nullptr) {
			*out_emitted = true;
		}
		return amount;
	}
	// Sem BC: o Tesouro toma do exterior (posição externa). O dinheiro sai da
	// posição do exterior e entra no banco (soma zero); o título é o claim do
	// credor.
	ws.external_bonds[nation] += amount;
	ws.foreign_cash -= amount;
	return amount;
}

std::string join_kind(const std::vector<std::string>& parts) {
	std::string out;
	for (const std::string& part : parts) {
		if (part.empty()) {
			continue;
		}
		if (!out.empty()) {
			out += "+";
		}
		out += part;
	}
	return out.empty() ? "none" : out;
}

} // namespace

Policy policy(const WorldState& ws) {
	const BankCrisisPolicy& saved = ws.bank_crisis;
	Policy out;
	out.enabled = saved.enabled;
	out.capital_min = pick(saved.capital_min, kCapitalMinDefault);
	out.capital_cut_floor = saved.capital_cut_floor;
	out.liquidity_min = saved.liquidity_min;
	out.loss_scale = pick(saved.loss_scale, kLossScaleDefault);
	out.loss_ema_days = pick(saved.loss_ema_days, kLossEmaDaysDefault);
	out.confidence_start = pick(saved.confidence_start,
		kConfidenceStartDefault);
	out.smoothing = pick(saved.smoothing, kConfidenceSmoothingDefault);
	out.w_solvency = pick(saved.w_solvency, kWeightSolvencyDefault);
	out.w_liquidity = pick(saved.w_liquidity, kWeightLiquidityDefault);
	out.w_losses = pick(saved.w_losses, kWeightLossesDefault);
	out.w_contagion = pick(saved.w_contagion, kWeightContagionDefault);
	out.w_market = pick(saved.w_market, kWeightMarketDefault);
	out.run_threshold = pick(saved.run_threshold, kRunThresholdDefault);
	out.run_share = pick(saved.run_share, kRunShareDefault);
	out.flight_to_quality = saved.flight_to_quality > 0.0
		? saved.flight_to_quality : kFlightToQualityDefault;
	out.lolr_spread = pick(saved.lolr_spread, kLolrSpreadDefault);
	out.lolr_haircut = pick(saved.lolr_haircut, kLolrHaircutDefault);
	out.lolr_max_assets = pick(saved.lolr_max_assets,
		kLolrMaxAssetsDefault);
	out.insured_share = pick(saved.insured_share, kInsuredShareDefault);
	out.premium_daily = pick(saved.premium_daily, kPremiumDailyDefault);
	out.fund_target_ratio = pick(saved.fund_target_ratio,
		kFundTargetRatioDefault);
	out.capital_flight_share = saved.capital_flight_share > 0.0
		? saved.capital_flight_share : kCapitalFlightShareDefault;
	out.insolvency_tolerance = saved.insolvency_tolerance > 0.0
		? saved.insolvency_tolerance : kInsolvencyToleranceDefault;
	out.insolvency_tolerance_ratio =
		saved.insolvency_tolerance_ratio > 0.0
		? saved.insolvency_tolerance_ratio
		: kInsolvencyToleranceRatioDefault;
	out.treasury_debt_rescue = saved.treasury_debt_rescue;
	out.takeover_public = saved.takeover_public;
	return out;
}

Indicators indicators(const Bank& bank) {
	Indicators out;
	out.loans = std::max(0.0, bank.loans);
	out.arrears = std::max(0.0, bank.arrears);
	out.cash = std::max(0.0, bank.cash);
	out.deposits = bank::total_deposits(bank);
	out.capital = std::max(0.0, bank.capital);
	out.assets = out.cash + out.loans + out.arrears;
	out.equity = bank::equity(bank);
	out.capital_ratio = out.assets > kEps ? out.equity / out.assets : 0.0;
	out.liquidity_ratio = out.deposits > kEps
		? out.cash / out.deposits : 0.0;
	const double book = out.loans + out.arrears;
	out.npl_ratio = book > kEps ? out.arrears / book : 0.0;
	return out;
}

double credit_factor(const WorldState& ws, const Bank& bank) {
	const Policy pol = policy(ws);
	const Indicators ind = indicators(bank);
	if (ind.assets <= kEps) {
		return 1.0;
	}
	if (ind.capital_ratio >= pol.capital_min) {
		return 1.0;
	}
	if (pol.capital_min <= kEps) {
		return 1.0;
	}
	const double factor = ind.capital_ratio / pol.capital_min;
	return std::clamp(factor, clamp01(pol.capital_cut_floor), 1.0);
}

bool has_lender(const WorldState& ws, const std::string& nation) {
	if (nation.empty()) {
		return false;
	}
	if (std::find(ws.fx_no_policy_nations.begin(),
			ws.fx_no_policy_nations.end(), nation)
			!= ws.fx_no_policy_nations.end()) {
		return false;
	}
	// Sem moeda no estado nao ha banco central (ausencia declarada).
	return !currency_of(ws, nation).empty();
}

void register_event(WorldState& ws, const BankCrisisEvent& event) {
	ws.bank_crisis_events.push_back(event);
	ws.bank_crisis_events_total += 1;
	if (static_cast<int>(ws.bank_crisis_events.size()) > kEventRing) {
		ws.bank_crisis_events.erase(ws.bank_crisis_events.begin());
	}
}

// --- Choque de perda -------------------------------------------------------
namespace {

// Charge o corte de deposito nao segurado nos grupos tomadores do banco,
// proporcional a divida e limitado pelo caixa de cada um. Devolve o total
// efetivamente cobrado (o que nao couber fica para o seguro/Tesouro).
double charge_depositors(WorldState& ws, const Bank& bank, double target) {
	if (!(target > 0.0)) {
		return 0.0;
	}
	struct Row {
		int index = -1;
		double debt = 0.0;
		double arrears = 0.0;
	};
	std::vector<Row> rows;
	double total = 0.0;
	for (int index = 0; index < static_cast<int>(ws.groups.size());
			++index) {
		const Group& group = ws.groups[static_cast<std::size_t>(index)];
		const Bank* lender = bank::lender(ws, group);
		if (lender == nullptr || lender->id != bank.id) {
			continue;
		}
		const double claim = std::max(0.0, group.debt)
			+ std::max(0.0, group.credit_arrears);
		if (claim <= kEps) {
			continue;
		}
		rows.push_back({index, group.debt, group.credit_arrears});
		total += claim;
	}
	if (rows.empty() || total <= kEps) {
		return 0.0;
	}
	double charged = 0.0;
	// Duas passadas: a 1a cobra proporcional; a 2a redistribui o que sobrou
	// entre quem ainda tem caixa (deterministico pela ordem dos grupos).
	for (int pass = 0; pass < 2 && charged < target - kEps; ++pass) {
		double remaining = target - charged;
		double weight = 0.0;
		for (const Row& row : rows) {
			const Group& group = ws.groups[static_cast<std::size_t>(
				row.index)];
			if (group.cash > kEps) {
				weight += row.debt + row.arrears;
			}
		}
		if (weight <= kEps) {
			break;
		}
		for (const Row& row : rows) {
			if (remaining <= kEps) {
				break;
			}
			Group& group = ws.groups[static_cast<std::size_t>(row.index)];
			if (group.cash <= kEps) {
				continue;
			}
			const double share = (row.debt + row.arrears) / weight;
			const double take = std::min({ remaining * share,
				std::max(0.0, group.cash) });
			if (take <= kEps) {
				continue;
			}
			group.cash -= take;
			charged += take;
			remaining -= take;
		}
	}
	return charged;
}

} // namespace

double apply_loss(WorldState& ws, const std::string& bank_id, double amount) {
	const auto found = ws.banks.find(bank_id);
	if (found == ws.banks.end() || !(amount > 0.0)
			|| !std::isfinite(amount)) {
		return 0.0;
	}
	Bank& bank = found->second;
	double book = 0.0;
	for (Group& group : ws.groups) {
		const Bank* lender = bank::lender(ws, group);
		if (lender != nullptr && lender->id == bank.id) {
			book += std::max(0.0, group.debt)
				+ std::max(0.0, group.credit_arrears);
		}
	}
	const double loss = std::min({ amount, book,
		std::max(0.0, bank.loans) + std::max(0.0, bank.arrears) });
	if (loss <= kEps) {
		return 0.0;
	}
	double written = 0.0;
	for (Group& group : ws.groups) {
		if (written >= loss - kEps) {
			break;
		}
		const Bank* lender = bank::lender(ws, group);
		if (lender == nullptr || lender->id != bank.id) {
			continue;
		}
		const double claim = std::max(0.0, group.debt)
			+ std::max(0.0, group.credit_arrears);
		if (claim <= kEps) {
			continue;
		}
		const double share = claim / book;
		double take = std::min(claim, loss * share);
		// Segunda passada cobre o arredondamento: o que sobrar vai para o
		// ultimo tomador com divida (a soma fecha por construcao).
		if (written + take > loss) {
			take = loss - written;
		}
		const double share_arrears = group.credit_arrears / claim;
		const double take_arrears = std::min(group.credit_arrears,
			take * share_arrears);
		const double take_debt = std::min(group.debt,
			take - take_arrears);
		group.credit_arrears -= take_arrears;
		group.debt -= take_debt;
		bank.arrears = std::max(0.0, bank.arrears - take_arrears);
		bank.loans = std::max(0.0, bank.loans - take_debt);
		bank.writeoffs += take_arrears + take_debt;
		bank.loss_today += take_arrears + take_debt;
		written += take_arrears + take_debt;
	}
	return written;
}

double apply_loss_share(WorldState& ws, const std::string& bank_id,
		double share) {
	const auto found = ws.banks.find(bank_id);
	if (found == ws.banks.end() || !(share > 0.0)) {
		return 0.0;
	}
	const double book = std::max(0.0, found->second.loans)
		+ std::max(0.0, found->second.arrears);
	return apply_loss(ws, bank_id, book * std::min(1.0, share));
}

// --- Resolucao -------------------------------------------------------------
double resolve(WorldState& ws, const std::string& bank_id,
		const std::string& cause) {
	const auto found = ws.banks.find(bank_id);
	if (found == ws.banks.end()) {
		return 0.0;
	}
	Bank& bank = found->second;
	if (!bank.absorbed_into.empty()) {
		return 0.0;
	}
	// Uma liquidacao sem lastro nao se repete (nao ha o que cortar). Bail-in,
	// fundo ou Tesouro deixam o banco monitorado: se voltar a ficar insolvente
	// por nova perda, passa pela hierarquia de novo.
	if (bank.resolved && bank.resolution == "liquidation") {
		return 0.0;
	}
	const Policy pol = policy(ws);
	const Indicators ind = indicators(bank);
	const double tolerance = std::max(pol.insolvency_tolerance,
		ind.assets * pol.insolvency_tolerance_ratio);
	if (ind.equity >= -tolerance) {
		return 0.0;
	}
	const double gap0 = -ind.equity;
	double gap = gap0;
	BankCrisisEvent event;
	event.day = ws.day;
	event.bank = bank.id;
	event.nation = bank.nation;
	event.kind = "resolution";
	event.cause = cause;
	event.gap = gap0;
	std::vector<std::string> kinds;

	// 1. Acionistas: o capital e' um claim do dono; com patrimonio negativo o
	// claim ja vale zero. A baixa NAO fecha o rombo (o buraco e' contra os
	// depositos); o custo fica registrado.
	event.cost_shareholders = std::max(0.0, bank.capital);
	bank.capital = 0.0;
	// Hook C5: se o banco tiver papel listado, ele e' baixado a zero (a C5
	// lista grupos, entao hoje isso nao acontece; a regra fica pronta).
	StockTicker* ticker = stock::find_ticker(ws, bank.id);
	if (ticker != nullptr && ticker->listed) {
		ticker->listed = false;
		ticker->price = 0.0;
		ticker->reason = "bank_resolution";
		ticker->delisted_day = ws.day;
	}

	// 2. Bail-in: deposito NAO segurado acima do teto sofre corte. O corte e'
	// cobrado do caixa dos tomadores (o dinheiro deles) e destroi o deposito:
	// conserva a soma e a variacao entra em `bank_money_today`.
	const double uninsured = ind.deposits
		* clamp01(1.0 - pol.insured_share);
	if (uninsured > kEps && gap > kEps) {
		double cut_total = 0.0;
		// R2a: o funding (D_f) corta o CLAIM do depositante, sem tocar em
		// caixa; o depósito de crédito (D_l) mantém o caminho legado (corta o
		// caixa do tomador). A soma dos dois respeita o alvo não segurado.
		if (bank.deposits_funding > kEps) {
			const double funding_cut = bank::cut_funding_claims(ws, bank.id,
				std::min(uninsured, gap));
			cut_total += funding_cut;
			gap -= funding_cut;
		}
		const double legacy = std::min(
			std::max(0.0, uninsured - cut_total), gap);
		if (legacy > kEps) {
			const double cut = charge_depositors(ws, bank, legacy);
			if (cut > kEps) {
				bank.deposits = std::max(0.0, bank.deposits - cut);
				cut_total += cut;
				gap -= cut;
			}
		}
		if (cut_total > kEps) {
			bank.bailin_cut += cut_total;
			event.cost_depositors = cut_total;
			kinds.push_back("bailin");
		}
	}

	// 3. Seguro de deposito: o fundo da nacao paga ate o saldo (e ate o teto
	// declarado). O pagamento ENTRA no banco (cobre o buraco), sem criar
	// dinheiro.
	if (gap > kEps) {
		DepositInsurance& fund = ws.deposit_insurance[bank.nation];
		const double insured = ind.deposits * clamp01(pol.insured_share);
		const double pay = std::min({ fund.fund, gap, insured });
		if (pay > kEps) {
			fund.fund -= pay;
			fund.payouts += pay;
			fund.last_payout_day = ws.day;
			bank.cash += pay;
			bank.insured_payout += pay;
			event.cost_fund = pay;
			gap -= pay;
			kinds.push_back("insurance");
		}
	}

	// 4. Tesouro: primeiro o caixa do orcamento publico (transferencia).
	if (gap > kEps) {
		const double paid = pay_from_government(ws, bank.nation, gap);
		if (paid > kEps) {
			bank.cash += paid;
			bank.treasury_rescue += paid;
			event.cost_treasury += paid;
			gap -= paid;
			kinds.push_back("treasury");
		}
	}

	// 5. Divida publica carregada pelo BC (ou pelo exterior, sem BC). So com
	// a alavanca declarada; e' a monetizacao registrada do resgate.
	if (gap > kEps && pol.treasury_debt_rescue) {
		bool emitted = false;
		const double paid = rescue_with_debt(ws, bank.nation, gap, &emitted);
		if (paid > kEps) {
			bank.cash += paid;
			bank.treasury_rescue += paid;
			event.cost_treasury += paid;
			event.emission += emitted ? paid : 0.0;
			gap -= paid;
			// `treasury` ja entrou; nao duplica no rotulo.
		}
	}

	// 6. Incorporacao pelo banco publico: o residual vira perda do publico.
	if (gap > kEps && pol.takeover_public) {
		Bank* pub = nullptr;
		const auto pub_found = ws.banks.find(
			bank::bank_id(bank.nation, bank::kRolePublic));
		if (pub_found != ws.banks.end()
				&& pub_found->second.id != bank.id
				&& pub_found->second.absorbed_into.empty()) {
			pub = &pub_found->second;
		}
		if (pub != nullptr) {
			// Tomadores do banco quebrado (antes de a casca mudar de share).
			std::vector<int> borrowers;
			for (int index = 0; index < static_cast<int>(ws.groups.size());
					++index) {
				const Bank* lender = bank::lender(ws,
					ws.groups[static_cast<std::size_t>(index)]);
				if (lender != nullptr && lender->id == bank.id) {
					borrowers.push_back(index);
				}
			}
			pub->cash += bank.cash;
			pub->loans += bank.loans;
			pub->arrears += bank.arrears;
			pub->deposits += bank.deposits;
			pub->deposits_funding += bank.deposits_funding;
			pub->capital_target += bank.capital_target;
			if (bank.deposits_funding > kEps) {
				// O claim do depositante acompanha o passivo: o dono passa a
				// ser o banco público (Σ claims == Σ D_f continua valendo).
				bank::reassign_claims(ws, bank.id, pub->id);
			}
			pub->bc_borrowing += bank.bc_borrowing;
			pub->writeoffs += bank.writeoffs;
			pub->defaults += bank.defaults;
			pub->loans_count += bank.loans_count;
			pub->interest_income += bank.interest_income;
			pub->interest_expense += bank.interest_expense;
			pub->dividends += bank.dividends;
			pub->harvest_interest += bank.harvest_interest;
			pub->last_default_day = std::max(pub->last_default_day,
				bank.last_default_day);
			pub->last_dividend_day = std::max(pub->last_dividend_day,
				bank.last_dividend_day);
			const std::string pub_id = pub->id;
			// Casca: saldos zerados, share 0 (nao entra no sorteio de tomador).
			bank.cash = 0.0;
			bank.loans = 0.0;
			bank.arrears = 0.0;
			bank.deposits = 0.0;
			bank.deposits_funding = 0.0;
			bank.capital_target = 0.0;
			bank.bc_borrowing = 0.0;
			bank.share = 0.0;
			// Resolvido e absorvido: não está mais "em corrida hoje". O
			// acumulado de saques fica para a medição.
			bank.run_days = 0;
			// Acumulados JA copiados para o banco publico: a casca zera para
			// `bank::totals` nao contar duas vezes (writeoffs/defaults/
			// loans_count/juros/dividendos/juro da safra). A ANCORA da
			// resolucao continua na casca (`resolution`, `resolution_day`).
			bank.writeoffs = 0.0;
			bank.defaults = 0;
			bank.loans_count = 0;
			bank.interest_income = 0.0;
			bank.interest_expense = 0.0;
			bank.dividends = 0.0;
			bank.harvest_interest = 0.0;
			bank.absorbed_into = pub_id;
			// Redistribui a participacao entre os bancos que sobraram na nacao.
			double share_sum = 0.0;
			for (auto& row : ws.banks) {
				if (row.second.nation == bank.nation
						&& row.second.absorbed_into.empty()
						&& row.first != bank.id) {
					share_sum += row.second.share;
				}
			}
			for (auto& row : ws.banks) {
				if (row.second.nation == bank.nation
						&& row.second.absorbed_into.empty()
						&& row.first != bank.id) {
					row.second.share = share_sum > kEps
						? row.second.share / share_sum
						: 1.0;
				}
			}
			// O tomador sortudo pode cair em outro banco depois da saida do
			// papel: move a divida para o novo credor para o par
			// banco<->grupo continuar batendo.
			for (int index : borrowers) {
				const Bank* chosen = bank::lender(ws,
					ws.groups[static_cast<std::size_t>(index)]);
				if (chosen == nullptr || chosen->id == pub_id
						|| chosen->id == bank.id) {
					continue;
				}
				Group& group = ws.groups[static_cast<std::size_t>(index)];
				const double move_debt = std::min(group.debt,
					std::max(0.0, pub->loans));
				const double move_arrears = std::min(group.credit_arrears,
					std::max(0.0, pub->arrears));
				pub->loans -= move_debt;
				pub->arrears -= move_arrears;
				auto new_found = ws.banks.find(chosen->id);
				if (new_found == ws.banks.end()) {
					continue;
				}
				new_found->second.loans += move_debt;
				new_found->second.arrears += move_arrears;
			}
			event.cost_takeover = gap;
			gap = 0.0;
			kinds.push_back("takeover");
		}
	}

	// 7. Liquidacao (sem Tesouro/BC/banco publico): o residual fica declarado
	// no patrimonio negativo; nenhuma soma quebra porque os depositos nao sao
	// ativos de ninguem no livro (o dinheiro real esta com os depositantes).
	bank.resolved = true;
	bank.resolution_day = ws.day;
	bank.resolution = join_kind(kinds);
	if (kinds.empty()) {
		bank.resolution = "liquidation";
	}
	register_event(ws, event);
	return gap0;
}

// --- Fase do dia -----------------------------------------------------------
void phase(WorldState& ws) {
	const Policy pol = policy(ws);
	if (!pol.enabled) {
		return;
	}
	double deposits_before = 0.0;
	std::map<std::string, double> nation_deposits;
	for (const auto& row : ws.banks) {
		const Bank& bank = row.second;
		deposits_before += bank.deposits;
		nation_deposits[bank.nation] += bank::total_deposits(bank);
	}

	// 1. Premio do seguro de deposito: sai do caixa do banco, entra no fundo
	// da nacao (conserva dinheiro). O fundo tem alvo declarado por nacao; sem
	// caixa o banco nao paga (ausencia declarada).
	for (auto& row : ws.banks) {
		Bank& bank = row.second;
		if (bank.resolved || !bank.absorbed_into.empty()
				|| bank::total_deposits(bank) <= kEps) {
			continue;
		}
		DepositInsurance& fund = ws.deposit_insurance[bank.nation];
		const double target = nation_deposits[bank.nation]
			* clamp01(pol.insured_share) * pol.fund_target_ratio;
		const double room = std::max(0.0, target - fund.fund);
		// O premio nao pode levar o patrimonio a negativo: um banco sem
		// capital (o mundo base da C4) pagaria o premio com o proprio buraco
		// e entraria em resolucao por centavos. Sem patrimonio nao ha premio
		// (ausencia declarada) — o fundo so acumula do que o banco ganhou.
		const double premium = std::min({ bank::total_deposits(bank)
				* pol.premium_daily,
			room, std::max(0.0, bank.cash),
			std::max(0.0, bank::equity(bank)) });
		if (premium <= kEps) {
			continue;
		}
		bank.cash -= premium;
		fund.fund += premium;
		fund.premiums += premium;
	}

	// 2. Indicadores, perdas recentes e confianca. Contagio usa a confianca
	// do fecho ANTERIOR (duas passadas: leitura e atualizacao), para a ordem
	// do mapa nao mudar o resultado.
	std::map<std::string, double> confidence_before;
	std::map<std::string, int> nation_banks;
	std::map<std::string, double> nation_conf_sum;
	for (const auto& row : ws.banks) {
		const Bank& bank = row.second;
		if (bank.resolved || !bank.absorbed_into.empty()) {
			continue;
		}
		if (bank.loans <= kEps && bank.arrears <= kEps
				&& bank::total_deposits(bank) <= kEps
				&& bank.cash <= kEps) {
			continue;
		}
		confidence_before[bank.id] = bank.confidence;
		nation_banks[bank.nation] += 1;
		nation_conf_sum[bank.nation] += bank.confidence;
	}
	for (auto& row : ws.banks) {
		Bank& bank = row.second;
		if (!bank.absorbed_into.empty()) {
			continue;
		}
		const Indicators ind = indicators(bank);
		const bool any = ind.assets > kEps || ind.deposits > kEps;
		if (!any) {
			continue;
		}
		bank.last_capital_ratio = ind.capital_ratio;
		bank.last_liquidity_ratio = ind.liquidity_ratio;
		bank.last_npl_ratio = ind.npl_ratio;
		// Referencia do depositante: o estado do PRIMEIRO fecho com posicao,
		// seguido para CIMA quando o banco melhora (capital novo, caixa que
		// entra). A confianca mede DETERIORACAO contra o melhor estado
		// conhecido, nao um nivel absoluto (ver `baseline_note`).
		if (!bank.baseline_seeded) {
			bank.open_capital_ratio = ind.capital_ratio;
			bank.open_liquidity_ratio = ind.liquidity_ratio;
			bank.baseline_seeded = true;
		} else {
			bank.open_capital_ratio = std::max(bank.open_capital_ratio,
				ind.capital_ratio);
			bank.open_liquidity_ratio = std::max(
				bank.open_liquidity_ratio, ind.liquidity_ratio);
		}
		// EMA da perda: a 1a perda entra inteira; depois k = 1/dias.
		const double k = pol.loss_ema_days > kEps
			? 1.0 / pol.loss_ema_days : 1.0;
		if (bank.loss_today > kEps) {
			if (!bank.loss_seeded) {
				bank.loss_ema = bank.loss_today;
				bank.loss_seeded = true;
			} else {
				bank.loss_ema += k * (bank.loss_today - bank.loss_ema);
			}
		} else {
			bank.loss_ema += k * (0.0 - bank.loss_ema);
		}
		bank.loss_today = 0.0;

		// Solvencia e liquidez RELATIVAS a referencia do banco: 1 = sem
		// deterioracao. Um banco que sempre teve patrimonio fino e caixa zero
		// (o mundo base da C4) nao perde confianca por isso; um choque que
		// derruba o indice contra a referencia perde.
		const double solvency = bank.open_capital_ratio > kEps
			? clamp01(ind.capital_ratio / bank.open_capital_ratio) : 1.0;
		const double liquidity = bank.open_liquidity_ratio > kEps
			? clamp01(ind.liquidity_ratio / bank.open_liquidity_ratio)
			: 1.0;
		const double loss_share = ind.assets > kEps
			? bank.loss_ema / ind.assets : 0.0;
		const double losses = pol.loss_scale > kEps
			? clamp01(1.0 - loss_share / pol.loss_scale) : 1.0;
		double contagion = 1.0;
		const auto count = nation_banks.find(bank.nation);
		if (count != nation_banks.end() && count->second > 1) {
			const double others = nation_conf_sum[bank.nation]
				- confidence_before[bank.id];
			contagion = clamp01(others / (count->second - 1));
		}
		double market = 1.0;
		const StockExchange* exchange = exchange_of(ws, bank.nation);
		if (exchange != nullptr) {
			market = clamp01((exchange->confidence - 0.65) / 0.70);
		}
		const double weight = pol.w_solvency + pol.w_liquidity + pol.w_losses
			+ pol.w_contagion + pol.w_market;
		double raw = 1.0;
		if (weight > kEps) {
			raw = (pol.w_solvency * solvency
				+ pol.w_liquidity * liquidity
				+ pol.w_losses * losses
				+ pol.w_contagion * contagion
				+ pol.w_market * market) / weight;
		}
		const double next = bank.confidence
			+ (raw - bank.confidence) * clamp01(pol.smoothing);
		bank.confidence = clamp01(next);
	}

	// 3. Corrida + emprestador de ultima instancia + fuga de capital.
	std::vector<std::string> ids;
	for (const auto& row : ws.banks) {
		ids.push_back(row.first);
	}
	for (const std::string& id : ids) {
		auto found = ws.banks.find(id);
		if (found == ws.banks.end()) {
			continue;
		}
		Bank& bank = found->second;
		if (!bank.absorbed_into.empty()) {
			continue;
		}
		if (bank.deposits <= kEps
				|| bank.confidence >= pol.run_threshold) {
			bank.run_days = 0;
			continue;
		}
		const bool first = bank.run_days == 0;
		bank.run_days += 1;
		bank.last_run_day = ws.day;
		const double requested = std::min(bank.deposits,
			bank.deposits * clamp01(pol.run_share));
		// Fuga de capital da matriz estrangeira: sai do caixa do proprio
		// banco e vai para a posicao do exterior (soma zero, nao mexe em
		// deposito). Antes do saque, para o redesconto cobrir a falta.
		double remitted = 0.0;
		if (bank.role == bank::kRoleForeign && bank.cash > kEps
				&& pol.capital_flight_share > kEps) {
			remitted = std::min(bank.cash,
				requested * clamp01(pol.capital_flight_share));
			if (remitted > kEps) {
				bank.cash -= remitted;
				ws.foreign_cash += remitted;
				bank.capital_flight += remitted;
			}
		}
		// Redesconto: cobre o que falta de caixa para o saque, contra o
		// colateral da carteira, com o BC da moeda (dolarizado nao tem).
		double borrowed = 0.0;
		if (has_lender(ws, bank.nation)) {
			const double need = std::max(0.0, requested - bank.cash);
			if (need > kEps) {
				const Indicators ind = indicators(bank);
				const double collateral = (ind.loans + ind.arrears)
					* clamp01(pol.lolr_haircut);
				const double cap = std::min(collateral,
					ind.assets * clamp01(pol.lolr_max_assets));
				const double headroom = std::max(0.0,
					cap - std::max(0.0, bank.bc_borrowing));
				borrowed = std::min(need, headroom);
				if (borrowed > kEps) {
					bank.cash += borrowed;
					bank.bc_borrowing += borrowed;
					bank.lolr_borrowed += borrowed;
					bank.lolr_days += 1;
					ws.emission_today += borrowed;
					MonetaryState* state = monetary_of(ws,
						currency_of(ws, bank.nation));
					if (state != nullptr) {
						state->emission += borrowed;
						state->emission_today += borrowed;
					}
				}
			}
		}
		const double paid = std::min(requested, std::max(0.0, bank.cash));
		if (paid > kEps) {
			// Fuga para qualidade: parte vai ao banco publico da nacao (se
			// existir e nao for o proprio banco); o resto vira papel-moeda
			// (a reserva e' liberada e o M0 mantem a soma — muda a
			// composicao, como uma corrida de verdade).
			Bank* pub = nullptr;
			const auto pub_found = ws.banks.find(
				bank::bank_id(bank.nation, bank::kRolePublic));
			if (pub_found != ws.banks.end()
					&& pub_found->second.id != bank.id
					&& pub_found->second.absorbed_into.empty()) {
				pub = &pub_found->second;
			}
			const double to_public = pub != nullptr
				? paid * clamp01(pol.flight_to_quality) : 0.0;
			const double to_cash = paid - to_public;
			bank.deposits = std::max(0.0, bank.deposits - paid);
			bank.cash -= paid;
			if (to_public > kEps && pub != nullptr) {
				pub->deposits += to_public;
				pub->cash += to_public;
			}
			bank.run_withdrawn += paid;
			bank.run_cash += to_cash;
			bank.run_public += to_public;
			if (first) {
				BankCrisisEvent event;
				event.day = ws.day;
				event.bank = bank.id;
				event.nation = bank.nation;
				event.kind = "run";
				event.cause = "confidence_below_threshold";
				event.lolr = borrowed;
				event.capital_flight = remitted;
				register_event(ws, event);
			}
		}
	}

	// 3b. Juro de penalidade do redesconto (SO o spread: a taxa basica ja e'
	// cobrada pela fase de credito da C3/C4). Mesma convencao do C3: pagar o
	// BC destroi reserva e reduz a divida registrada no dia.
	for (auto& row : ws.banks) {
		Bank& bank = row.second;
		if (bank.bc_borrowing <= kEps || bank.cash <= kEps) {
			continue;
		}
		const double due = bank.bc_borrowing * pol.lolr_spread / kDaysPerYear;
		const double paid = std::min(std::max(0.0, bank.cash),
			std::max(0.0, due));
		if (paid <= kEps) {
			continue;
		}
		bank.cash -= paid;
		bank.bc_borrowing -= paid;
		bank.interest_expense += paid;
		bank.lolr_interest += paid;
		ws.emission_today -= paid;
	}

	// 4. Resolucao dos insolventes (patrimonio < 0). A casca absorvida nao
	// volta para a fila; um banco por vez, ordem do mapa (deterministica).
	for (const std::string& id : ids) {
		auto found = ws.banks.find(id);
		if (found == ws.banks.end()) {
			continue;
		}
		if (!found->second.absorbed_into.empty()) {
			continue;
		}
		// Uma liquidacao sem lastro nao se repete todo dia (nao ha o que
		// cortar); os demais casos podem voltar se houver nova perda.
		if (found->second.resolved
				&& found->second.resolution == "liquidation") {
			continue;
		}
		// `resolve` aplica a tolerancia de materialidade (absoluta + razao
		// dos ativos); aqui so os negativos entram na fila.
		if (indicators(found->second).equity < 0.0) {
			resolve(ws, id, "equity_negative");
		}
	}

	// 5. Variacao de deposito da fase de crise entra no dinheiro bancario do
	// dia (a conservacao do dia soma `bank_money_today`, como na C4).
	double deposits_after = 0.0;
	for (const auto& row : ws.banks) {
		deposits_after += row.second.deposits;
	}
	ws.bank_money_today += deposits_after - deposits_before;
}

Totals totals(const WorldState& ws) {
	Totals out;
	for (const auto& row : ws.banks) {
		const Bank& bank = row.second;
		if (!bank.absorbed_into.empty()) {
			// Casca absorvida: sem saldo, mas os acumuladores de medição que
			// NÃO foram copiados ao banco público continuam aqui e entram UMA
			// vez no total (corrida, redesconto, seguro, bail-in, Tesouro e
			// fuga). Os copiados foram zerados na incorporação.
			out.capital_flight += bank.capital_flight;
			out.run_withdrawn += bank.run_withdrawn;
			out.run_cash += bank.run_cash;
			out.run_public += bank.run_public;
			out.lolr_borrowed += bank.lolr_borrowed;
			out.lolr_interest += bank.lolr_interest;
			out.insured_payout += bank.insured_payout;
			out.bailin_cut += bank.bailin_cut;
			out.treasury_rescue += bank.treasury_rescue;
			continue;
		}
		const bool any = bank.loans > kEps || bank.arrears > kEps
			|| bank::total_deposits(bank) > kEps || bank.cash > kEps;
		if (!any) {
			continue;
		}
		out.banks += 1;
		out.confidence_sum += bank.confidence;
		out.confidence_min = std::min(out.confidence_min, bank.confidence);
		out.confidence_max = std::max(out.confidence_max, bank.confidence);
		out.capital_flight += bank.capital_flight;
		out.run_withdrawn += bank.run_withdrawn;
		out.run_cash += bank.run_cash;
		out.run_public += bank.run_public;
		out.lolr_borrowed += bank.lolr_borrowed;
		out.lolr_interest += bank.lolr_interest;
		out.insured_payout += bank.insured_payout;
		out.bailin_cut += bank.bailin_cut;
		out.treasury_rescue += bank.treasury_rescue;
		if (bank.run_days > 0) {
			out.runs += 1;
		}
		out.run_days += bank.run_days;
		if (bank.resolution_day >= 0) {
			out.resolved_markets += 1;
		}
	}
	// Resoluções: conta o dia de TODO banco, inclusive a casca absorvida pelo
	// banco público (a linha não tem saldo, mas a resolução aconteceu). Contar
	// no laço acima perdia a resolução no momento da incorporação.
	for (const auto& row : ws.banks) {
		if (row.second.resolution_day >= 0) {
			out.resolutions += 1;
		}
	}
	for (const auto& row : ws.deposit_insurance) {
		out.fund_total += row.second.fund;
		out.premiums += row.second.premiums;
		out.payouts += row.second.payouts;
	}
	out.events_total = ws.bank_crisis_events_total;
	return out;
}

void reset(WorldState& ws) {
	ws.deposit_insurance.clear();
	ws.bank_crisis_events.clear();
	ws.bank_crisis_events_total = 0;
	for (auto& row : ws.banks) {
		Bank& bank = row.second;
		bank.confidence = policy(ws).confidence_start;
		bank.loss_seeded = false;
		bank.loss_ema = 0.0;
		bank.loss_today = 0.0;
		bank.run_days = 0;
		bank.last_run_day = -1;
		bank.run_withdrawn = 0.0;
		bank.run_cash = 0.0;
		bank.run_public = 0.0;
		bank.lolr_borrowed = 0.0;
		bank.lolr_interest = 0.0;
		bank.lolr_days = 0;
		bank.insured_payout = 0.0;
		bank.bailin_cut = 0.0;
		bank.treasury_rescue = 0.0;
		bank.capital_flight = 0.0;
		bank.resolution_day = -1;
		bank.resolved = false;
		bank.resolution = "none";
		bank.absorbed_into.clear();
		bank.last_capital_ratio = 0.0;
		bank.last_liquidity_ratio = 0.0;
		bank.last_npl_ratio = 0.0;
		bank.baseline_seeded = false;
		bank.open_capital_ratio = 0.0;
		bank.open_liquidity_ratio = 0.0;
	}
}

void after_load(WorldState& ws, int version) {
	if (version < 34) {
		// v33 e anteriores nao tem estado de crise: bancos iniciam calmos
		// (confianca 1,0), sem fundo e sem eventos. Nada e inventado.
		ws.deposit_insurance.clear();
		ws.bank_crisis_events.clear();
		ws.bank_crisis_events_total = 0;
		for (auto& row : ws.banks) {
			Bank& bank = row.second;
			bank.confidence = policy(ws).confidence_start;
			bank.loss_seeded = false;
			bank.loss_ema = 0.0;
			bank.loss_today = 0.0;
			bank.run_days = 0;
			bank.last_run_day = -1;
			bank.resolved = false;
			bank.resolution = "none";
			bank.absorbed_into.clear();
			bank.resolution_day = -1;
			bank.baseline_seeded = false;
			bank.open_capital_ratio = 0.0;
			bank.open_liquidity_ratio = 0.0;
		}
	}
}

} // namespace bank_crisis
} // namespace ogs
