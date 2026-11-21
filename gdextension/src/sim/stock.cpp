// C5 (D-112) — BOLSA DE VALORES POR PAÍS, CONFIANÇA E EXPECTATIVAS.
// Desenho e contrato: docs/economy/C5_stock_exchange.md.
//
// Nada aqui inventa número: o preço nasce do patrimônio MEDIDO do grupo e o
// lucro esperado é a média adaptativa do lucro medido das construções do grupo.
// A confiança lê sinais já medidos (inflação, juros, produção, falências,
// desvalorizações e desemprego) e persegue o sinal bruto (expectativa
// adaptativa). Valorização não move caixa; o dividendo move (soma zero).

#include "stock.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <unordered_map>

#include "balance.h"
#include "bank.h"
#include "central_bank.h"
#include "exchange.h"
#include "monetary.h"

namespace ogs {
namespace stock {

namespace {

const std::string& currency_of(const WorldState& ws, const std::string& nation) {
	static const std::string kEmpty;
	const auto found = ws.nation_currency.find(nation);
	if (found == ws.nation_currency.end() || found->second < 0
			|| found->second >= static_cast<int>(ws.currencies.size())) {
		return kEmpty;
	}
	return ws.currencies[static_cast<std::size_t>(found->second)].code;
}

const Group* group_by_id(const WorldState& ws, const std::string& id) {
	const auto found = ws.group_lookup.find(id);
	if (found == ws.group_lookup.end() || found->second < 0
			|| found->second >= static_cast<int>(ws.groups.size())) {
		return nullptr;
	}
	return &ws.groups[static_cast<std::size_t>(found->second)];
}

Group* group_by_id(WorldState& ws, const std::string& id) {
	const auto found = ws.group_lookup.find(id);
	if (found == ws.group_lookup.end() || found->second < 0
			|| found->second >= static_cast<int>(ws.groups.size())) {
		return nullptr;
	}
	return &ws.groups[static_cast<std::size_t>(found->second)];
}

// Medição do dia por nação: valor de produção das construções (receita medida
// do último dia), construções falidas e desemprego das famílias.
struct NationMeasure {
	double production = 0.0;
	double population = 0.0;
	double unemployed = 0.0;
	int buildings = 0;
	int bankrupt = 0;
};

std::map<std::string, NationMeasure> measure_nations(const WorldState& ws) {
	std::map<std::string, NationMeasure> out;
	std::unordered_map<std::string, const std::string*> city_nation;
	city_nation.reserve(ws.cities.size() * 2 + 1);
	for (const City& city : ws.cities) {
		city_nation[city.id] = &city.nation;
		NationMeasure& row = out[city.nation];
		row.population += std::max(0.0, city.pop.population);
		row.unemployed += std::max(0.0, city.pop.unemployed);
	}
	for (const Building& building : ws.buildings) {
		const auto found = city_nation.find(building.city);
		if (found == city_nation.end()) {
			continue;
		}
		NationMeasure& row = out[*found->second];
		row.buildings += 1;
		if (building.bankrupt) {
			row.bankrupt += 1;
			continue;
		}
		row.production += std::max(0.0, building.revenue);
	}
	return out;
}

int devaluations_12m(const WorldState& ws, const std::string& currency) {
	if (currency.empty()) {
		return 0;
	}
	int count = 0;
	for (const FxEvent& event : ws.fx_events) {
		if (event.type != "devaluation" || event.currency != currency) {
			continue;
		}
		if (event.day < 0 || event.day < ws.day - 365) {
			continue;
		}
		count += 1;
	}
	return count;
}

// Sinal em [-1, +1]: `value` na escala `scale` (0 = sem dado).
double clamp_signal(double value, double scale) {
	if (!(scale > 0.0) || !std::isfinite(value)) {
		return 0.0;
	}
	return std::clamp(value / scale, -1.0, 1.0);
}

Signals compute_signals(const WorldState& ws, const StockExchange& exchange,
		const NationMeasure& measure) {
	Signals out;
	double weighted = 0.0;
	double weights = 0.0;
	// 1. Inflação 12 m contra a meta da C2b1.
	const auto cpi = ws.cpi_history.find(exchange.nation);
	if (cpi != ws.cpi_history.end()) {
		const central_bank::Inflation inflation =
			central_bank::inflation_of(cpi->second);
		if (inflation.has_12m) {
			out.has_inflation = true;
			out.inflation = inflation.rate_12m;
		} else if (inflation.has_annualized) {
			out.has_inflation = true;
			out.inflation = inflation.annualized;
		}
	}
	if (out.has_inflation) {
		// Inflação acima da meta reduz a confiança (sinal negativo).
		const double signal = -clamp_signal(out.inflation
			- monetary::kTargetInflation, kInflationScale);
		weighted += kWeightInflation * signal;
		weights += kWeightInflation;
	} else {
		out.missing.push_back("inflation");
	}
	// 2. Juro básico contra o neutro (acima do neutro = aperto = menos
	// confiança).
	if (!exchange.currency.empty()) {
		out.has_rate = true;
		out.rate = monetary::rate_for_nation(ws, exchange.nation);
		const double signal = -clamp_signal(out.rate - monetary::kNeutralRate,
			kRateScale);
		weighted += kWeightRate * signal;
		weights += kWeightRate;
	} else {
		out.missing.push_back("rate");
	}
	// 3. Crescimento da produção da nação (EMA do valor medido).
	if (exchange.production_seeded && exchange.production_ema > 0.0) {
		out.has_production = true;
		out.production_growth = (measure.production - exchange.production_ema)
			/ exchange.production_ema;
		const double signal = clamp_signal(out.production_growth * 365.0,
			kProductionScale);
		weighted += kWeightProduction * signal;
		weights += kWeightProduction;
	} else {
		out.missing.push_back("production");
	}
	// 4. Falências sobre as construções da nação.
	if (measure.buildings > 0) {
		out.has_bankruptcies = true;
		out.bankruptcy_share = static_cast<double>(measure.bankrupt)
			/ static_cast<double>(measure.buildings);
		const double signal = -std::clamp(out.bankruptcy_share
			/ kBankruptcyScale, 0.0, 1.0);
		weighted += kWeightBankruptcies * signal;
		weights += kWeightBankruptcies;
	} else {
		out.missing.push_back("bankruptcies");
	}
	// 5. Desvalorizações da moeda nos últimos 365 d.
	if (!exchange.currency.empty()) {
		out.has_devaluations = true;
		out.devaluations = devaluations_12m(ws, exchange.currency);
		const double signal = -std::clamp(
			static_cast<double>(out.devaluations) / kDevaluationScale, 0.0, 1.0);
		weighted += kWeightDevaluations * signal;
		weights += kWeightDevaluations;
	} else {
		out.missing.push_back("devaluations");
	}
	// 6. Desemprego medido contra o natural declarado.
	if (measure.population > 0.0) {
		out.has_unemployment = true;
		out.unemployment = measure.unemployed / measure.population;
		const double signal = clamp_signal(
			ws.natural_unemployment - out.unemployment, kUnemploymentScale);
		weighted += kWeightUnemployment * signal;
		weights += kWeightUnemployment;
	} else {
		out.missing.push_back("unemployment");
	}
	// Pesos renormalizados quando falta sinal: a confiança começa em 1,0.
	const double declared = kWeightInflation + kWeightRate + kWeightProduction
		+ kWeightBankruptcies + kWeightDevaluations + kWeightUnemployment;
	if (weights > 0.0) {
		out.raw = 1.0 + (weighted / weights) * declared;
	} else {
		out.raw = kExpectationDefault;
	}
	return out;
}

void credit_households(WorldState& ws, const std::string& nation,
		double amount) {
	if (!(amount > 0.0)) {
		return;
	}
	double population = 0.0;
	for (const City& city : ws.cities) {
		if (city.nation == nation) {
			population += std::max(0.0, city.pop.population);
		}
	}
	if (!(population > 0.0)) {
		return;
	}
	const City* last = nullptr;
	for (const City& city : ws.cities) {
		if (city.nation == nation) {
			last = &city;
		}
	}
	if (last == nullptr) {
		return;
	}
	double left = amount;
	for (City& city : ws.cities) {
		if (city.nation != nation) {
			continue;
		}
		const double share = std::max(0.0, city.pop.population) / population;
		double credit = (&city == last) ? left : amount * share;
		credit = std::min(credit, left);
		if (!(credit > 0.0)) {
			continue;
		}
		// A poupança tem duas camadas (escalar e por estrato): as duas andam
		// juntas, como no crédito/dividendo do banco privado (C4).
		city.pop.stratum_savings[kIncomeStrata - 1] += credit;
		city.pop.savings = 0.0;
		for (int stratum = 0; stratum < kIncomeStrata; ++stratum) {
			city.pop.savings += std::max(0.0,
				city.pop.stratum_savings[stratum]);
		}
		left -= credit;
	}
}

// Uma passada por dia: a produção medida alimenta a EMA ANTES do sinal (o
// sinal compara o nível medido com a média já formada; no dia 0 não há sinal).
double update_confidence_with(WorldState& ws, StockExchange& exchange,
		const NationMeasure& measure, const Policy& pol) {
	if (!exchange.production_seeded) {
		exchange.production_ema = measure.production;
		exchange.production_seeded = true;
	} else {
		exchange.production_ema +=
			(measure.production - exchange.production_ema) * 0.05;
	}
	const Signals values = compute_signals(ws, exchange, measure);
	exchange.confidence = std::clamp(exchange.confidence
		+ (values.raw - exchange.confidence) * pol.expectation_k, 0.65, 1.35);
	return values.raw;
}

bool nation_has_cities(const WorldState& ws, const std::string& nation) {
	for (const City& city : ws.cities) {
		if (city.nation == nation && city.pop.population > 0.0) {
			return true;
		}
	}
	return false;
}

// Divisão da carteira inicial (§4 do doc): 55 % famílias do estrato alto,
// 20 % bancos (pela participação da C4), 15 % grupos da nação e 10 % exterior
// (só com banco estrangeiro; sem ele a fatia vai para as famílias).
StockHoldings default_holdings(const WorldState& ws, const Group& group,
		double shares) {
	StockHoldings out;
	double households = kHoldHouseholds;
	double foreign_share = kHoldForeign;
	std::vector<const Bank*> banks;
	for (int role = 0; role < 3; ++role) {
		const Bank* bank = bank::find(ws, group.origin_nation, role);
		if (bank != nullptr) {
			banks.push_back(bank);
		}
	}
	if (banks.empty()) {
		households += kHoldBanks;
	} else {
		for (const Bank* bank : banks) {
			const double share = bank::declared_share(ws, group.origin_nation,
				bank->role);
			if (share > 0.0) {
				out.banks[bank->id] = shares * kHoldBanks * share;
			}
		}
	}
	std::vector<const Group*> locals;
	for (const Group& other : ws.groups) {
		if (&other == &group || other.origin_nation != group.origin_nation) {
			continue;
		}
		if (!eligible(other, nullptr)) {
			continue;
		}
		locals.push_back(&other);
	}
	if (locals.empty()) {
		households += kHoldGroups;
	} else {
		const double slice = shares * kHoldGroups
			/ static_cast<double>(locals.size());
		for (const Group* other : locals) {
			out.groups[other->id] = slice;
		}
	}
	if (bank::find(ws, group.origin_nation, bank::kRoleForeign) == nullptr) {
		households += kHoldForeign;
		foreign_share = 0.0;
	}
	out.foreign = shares * foreign_share;
	out.households_high = shares * households;
	return out;
}

void form_index(WorldState& ws, StockExchange& exchange) {
	if (exchange.index_base > 0.0) {
		return;
	}
	double cap = 0.0;
	for (const auto& row : ws.stock_tickers) {
		if (row.second.exchange == exchange.nation) {
			cap += market_cap(row.second);
		}
	}
	exchange.index_base = cap;
}

} // namespace

Policy policy(const WorldState& ws) {
	Policy out;
	out.enabled = ws.stock_enabled;
	out.investment_channel = ws.stock_investment_channel;
	if (ws.stock_risk_premium > 0.0) {
		out.risk_premium = ws.stock_risk_premium;
	}
	if (ws.stock_payout_ratio > 0.0) {
		out.payout_ratio = ws.stock_payout_ratio;
	}
	if (ws.stock_step > 0.0) {
		out.step = std::min(1.0, ws.stock_step);
	}
	if (ws.stock_expectation_k > 0.0) {
		out.expectation_k = std::min(1.0, ws.stock_expectation_k);
	}
	return out;
}

bool eligible(const Group& group, std::string* reason) {
	if (group.origin_nation.empty()) {
		if (reason != nullptr) {
			*reason = "no_origin";
		}
		return false;
	}
	if (group.state_owned) {
		if (reason != nullptr) {
			*reason = "state_owned";
		}
		return false;
	}
	if (!(group.revenue_total > 0.0)) {
		if (reason != nullptr) {
			*reason = "no_measured_earnings";
		}
		return false;
	}
	if (reason != nullptr) {
		*reason = "listed";
	}
	return true;
}

StockExchange& ensure_exchange(WorldState& ws, const std::string& nation) {
	StockExchange& exchange = ws.stock_exchanges[nation];
	if (exchange.nation.empty()) {
		exchange.nation = nation;
		// Nome fictício/genérico: nenhuma bolsa real (sigla ou marca) é usada.
		exchange.name = nation + " Exchange";
		exchange.name_status = "game_default";
	}
	exchange.currency = currency_of(ws, nation);
	return exchange;
}

StockTicker* find_ticker(WorldState& ws, const std::string& group_id) {
	const auto found = ws.stock_tickers.find(group_id);
	return found == ws.stock_tickers.end() ? nullptr : &found->second;
}

const StockTicker* find_ticker(const WorldState& ws,
		const std::string& group_id) {
	const auto found = ws.stock_tickers.find(group_id);
	return found == ws.stock_tickers.end() ? nullptr : &found->second;
}

double book_equity(const balance::GroupSheet& sheet) {
	return sheet.net_worth();
}

double market_cap(const StockTicker& ticker) {
	return std::max(0.0, ticker.price) * std::max(0.0, ticker.shares);
}

double index_of(const WorldState& ws, const StockExchange& exchange) {
	if (!(exchange.index_base > 0.0)) {
		return 100.0;
	}
	double cap = 0.0;
	for (const auto& row : ws.stock_tickers) {
		if (row.second.exchange == exchange.nation) {
			cap += market_cap(row.second);
		}
	}
	return cap / exchange.index_base * 100.0;
}

void seed(WorldState& ws) {
	const Policy pol = policy(ws);
	bool missing = false;
	for (const Group& group : ws.groups) {
		if (eligible(group, nullptr)
				&& ws.stock_tickers.count(group.id) == 0) {
			missing = true;
			break;
		}
	}
	if (!missing) {
		return;
	}
	const std::vector<balance::GroupSheet> sheets = balance::group_sheets(ws);
	for (std::size_t index = 0; index < ws.groups.size(); ++index) {
		const Group& group = ws.groups[index];
		if (!eligible(group, nullptr)) {
			continue;
		}
		if (ws.stock_tickers.count(group.id) != 0) {
			continue;
		}
		StockTicker ticker;
		ticker.group_id = group.id;
		ticker.nation = group.origin_nation;
		ticker.exchange = group.origin_nation;
		ticker.shares = pol.shares > 0.0 ? pol.shares : kSharesDefault;
		const double equity = index < sheets.size()
			? book_equity(sheets[index]) : 0.0;
		ticker.book_equity = std::max(0.0, equity);
		ticker.price = ticker.shares > 0.0
			? ticker.book_equity / ticker.shares : 0.0;
		ticker.profit_ema = index < sheets.size()
			? std::max(0.0, sheets[index].daily_profit) : 0.0;
		ticker.profit_at_last_dividend = group.profit_total;
		ticker.listed = true;
		ticker.reason = "listed";
		ticker.listed_day = ws.day;
		ticker.holdings = default_holdings(ws, group, ticker.shares);
		StockExchange& exchange = ensure_exchange(ws, ticker.nation);
		form_index(ws, exchange);
		ws.stock_tickers[ticker.group_id] = ticker;
	}
	for (auto& row : ws.stock_exchanges) {
		form_index(ws, row.second);
	}
}

double update_confidence(WorldState& ws, StockExchange& exchange) {
	const std::map<std::string, NationMeasure> measures = measure_nations(ws);
	const auto found = measures.find(exchange.nation);
	NationMeasure measure;
	if (found != measures.end()) {
		measure = found->second;
	}
	return update_confidence_with(ws, exchange, measure, policy(ws));
}

Signals signals(const WorldState& ws, const StockExchange& exchange) {
	const std::map<std::string, NationMeasure> measures = measure_nations(ws);
	const auto found = measures.find(exchange.nation);
	NationMeasure measure;
	if (found != measures.end()) {
		measure = found->second;
	}
	return compute_signals(ws, exchange, measure);
}

double pay_dividends(WorldState& ws) {
	const Policy pol = policy(ws);
	double paid_total = 0.0;
	for (auto& row : ws.stock_tickers) {
		StockTicker& ticker = row.second;
		if (!ticker.listed) {
			continue;
		}
		Group* group = group_by_id(ws, ticker.group_id);
		if (group == nullptr) {
			continue;
		}
		StockExchange& exchange = ensure_exchange(ws, ticker.nation);
		if (!nation_has_cities(ws, ticker.nation)) {
			continue;  // sem cidade, não há onde creditar: nada é destruído
		}
		const double period_profit = group->profit_total
			- ticker.profit_at_last_dividend;
		ticker.profit_at_last_dividend = group->profit_total;
		const double requested = pol.payout_ratio
			* std::max(0.0, period_profit);
		const double paid = std::min(requested, std::max(0.0, group->cash));
		ticker.dividend_last = paid;
		ticker.dividend_day = ws.day;
		ticker.dividend_limited += std::max(0.0, requested - paid);
		if (!(paid > 0.0)) {
			continue;
		}
		const double shares = std::max(ticker.shares, 1e-9);
		double credited = 0.0;
		// Famílias do estrato alto.
		const double households = paid * std::clamp(
			ticker.holdings.households_high / shares, 0.0, 1.0);
		if (households > 0.0) {
			credit_households(ws, ticker.nation, households);
			credited += households;
		}
		// Bancos.
		for (const auto& holder : ticker.holdings.banks) {
			const auto bank_row = ws.banks.find(holder.first);
			if (bank_row == ws.banks.end()) {
				continue;
			}
			const double amount = paid * std::clamp(holder.second / shares,
				0.0, 1.0);
			if (amount > 0.0) {
				bank_row->second.cash += amount;
				credited += amount;
			}
		}
		// Grupos.
		for (const auto& holder : ticker.holdings.groups) {
			Group* owner = group_by_id(ws, holder.first);
			if (owner == nullptr) {
				continue;
			}
			const double amount = paid * std::clamp(holder.second / shares,
				0.0, 1.0);
			if (amount > 0.0) {
				owner->cash += amount;
				credited += amount;
			}
		}
		// Exterior: a sobra exata fecha a soma (conservação bit-exata).
		const double foreign = std::max(0.0, paid - credited);
		if (foreign > 0.0 && ticker.holdings.foreign > 0.0) {
			ws.foreign_cash += foreign;
			exchange.remittance += foreign;
			const Bank* foreign_bank = bank::find(ws, ticker.nation,
				bank::kRoleForeign);
			const std::string& origin = foreign_bank == nullptr
				? std::string() : foreign_bank->owner_nation;
			const std::string& origin_currency = currency_of(ws, origin);
			const auto code = ws.currency_by_code.find(origin_currency);
			if (code != ws.currency_by_code.end()) {
				const double parity = exchange::parity_of(ws,
					ws.currencies[static_cast<std::size_t>(code->second)]);
				exchange.remittance_origin += foreign * parity;
			}
			credited += foreign;
		}
		// O emissor paga o que foi efetivamente distribuído.
		group->cash = std::max(0.0, group->cash - credited);
		exchange.dividends_paid += credited;
		paid_total += credited;
	}
	return paid_total;
}

void phase(WorldState& ws) {
	const Policy pol = policy(ws);
	ws.stock_enabled = pol.enabled;
	if (!pol.enabled) {
		return;
	}
	seed(ws);
	const std::vector<balance::GroupSheet> sheets = balance::group_sheets(ws);
	std::unordered_map<std::string, std::size_t> sheet_index;
	sheet_index.reserve(ws.groups.size() * 2 + 1);
	for (std::size_t index = 0; index < ws.groups.size(); ++index) {
		sheet_index[ws.groups[index].id] = index;
	}
	// 1. Lucro esperado (EMA do lucro medido) e confiança (diários).
	for (auto& row : ws.stock_tickers) {
		StockTicker& ticker = row.second;
		const auto found = sheet_index.find(ticker.group_id);
		if (found != sheet_index.end() && found->second < sheets.size()) {
			const double profit = sheets[found->second].daily_profit;
			ticker.profit_ema += (profit - ticker.profit_ema) * 0.05;
		}
	}
	// Uma passada de medição para todas as praças do dia.
	const std::map<std::string, NationMeasure> measures = measure_nations(ws);
	for (auto& row : ws.stock_exchanges) {
		const auto found = measures.find(row.first);
		NationMeasure measure;
		if (found != measures.end()) {
			measure = found->second;
		}
		update_confidence_with(ws, row.second, measure, pol);
	}
	// 2. Fechamento SEMANAL: elegibilidade e preço convergente.
	if (ws.day > 0 && ws.day % kPricePeriodDays == 0) {
		for (auto& row : ws.stock_tickers) {
			StockTicker& ticker = row.second;
			const Group* group = group_by_id(ws, ticker.group_id);
			std::string reason = "no_group";
			if (group != nullptr) {
				eligible(*group, &reason);
			}
			if (group == nullptr || reason != "listed") {
				if (ticker.listed) {
					ticker.listed = false;
					ticker.reason = reason;
					ticker.delisted_day = ws.day;
					// Papel retirado: o valor de mercado vai a zero de forma
					// declarada (não fica um preço velho no balanço).
					ticker.price = 0.0;
				}
				continue;
			}
			if (!ticker.listed) {
				ticker.listed = true;
				ticker.reason = "listed";
				ticker.listed_day = ws.day;
				ticker.delisted_day = -1;
			}
			const StockExchange& exchange = ensure_exchange(ws, ticker.nation);
			// Patrimônio medido do grupo no fechamento (base do q de Tobin).
			const auto sheet_found = sheet_index.find(ticker.group_id);
			if (sheet_found != sheet_index.end()
					&& sheet_found->second < sheets.size()) {
				ticker.book_equity = std::max(0.0,
					book_equity(sheets[sheet_found->second]));
			}
			const double rate = monetary::rate_for_nation(ws, ticker.nation);
			const double discount = std::max(kPriceFloorRate,
				rate + pol.risk_premium);
			const double dividend = pol.payout_ratio
				* std::max(0.0, ticker.profit_ema) * 365.0;
			const double shares = std::max(ticker.shares, 1e-9);
			const double fundamental = dividend / discount / shares;
			const double target = fundamental * exchange.confidence;
			ticker.price = std::max(0.0,
				ticker.price + (target - ticker.price) * pol.step);
		}
		for (auto& row : ws.stock_exchanges) {
			row.second.last_price_day = ws.day;
		}
	}
	// 3. Fechamento MENSAL: dividendo do lucro medido do período.
	if (ws.day > 0 && ws.day % pol.dividend_period_days == 0) {
		pay_dividends(ws);
	}
}

double investment_factor(const WorldState& ws, const std::string& group_id) {
	const Policy pol = policy(ws);
	if (!pol.enabled || !pol.investment_channel) {
		return 1.0;
	}
	const StockTicker* ticker = find_ticker(ws, group_id);
	if (ticker == nullptr || !ticker->listed) {
		return 1.0;
	}
	const double equity = ticker->book_equity;
	if (!(equity > 0.0)) {
		return 1.0;
	}
	const double q = market_cap(*ticker) / equity;
	return std::clamp(0.5 + 0.5 * q, kQFactorFloor, kQFactorCeiling);
}

Totals totals(const WorldState& ws) {
	Totals out;
	double base_cap = 0.0;
	for (const auto& row : ws.stock_exchanges) {
		out.exchanges += 1;
		out.dividends_paid += row.second.dividends_paid;
		out.remittance += row.second.remittance;
		out.remittance_origin += row.second.remittance_origin;
		base_cap += row.second.index_base;
	}
	for (const auto& row : ws.stock_tickers) {
		const StockTicker& ticker = row.second;
		if (ticker.listed) {
			out.listed += 1;
			out.market_cap += market_cap(ticker);
			out.holdings += market_cap(ticker);
		} else {
			out.unlisted += 1;
		}
		out.book_equity += std::max(0.0, ticker.book_equity);
	}
	out.index_global = base_cap > 0.0 ? out.market_cap / base_cap * 100.0 : 100.0;
	return out;
}

void after_load(WorldState& ws, int version) {
	// v32 e anteriores não têm bolsa: a migração cria com preço = patrimônio
	// medido e a carteira do default declarado. v33+ carrega exatamente o que
	// está no save (a bolsa de grupos novos nasce no tick, não no load).
	if (version >= 33) {
		return;
	}
	seed(ws);
}

void reset(WorldState& ws) {
	ws.stock_exchanges.clear();
	ws.stock_tickers.clear();
}

} // namespace stock
} // namespace ogs

// `StockHoldings` é um tipo de `ogs` (sim_types.h), não de `ogs::stock`.
double ogs::StockHoldings::total() const {
	double sum = households_high + foreign;
	for (const auto& row : banks) {
		sum += row.second;
	}
	for (const auto& row : groups) {
		sum += row.second;
	}
	return sum;
}
