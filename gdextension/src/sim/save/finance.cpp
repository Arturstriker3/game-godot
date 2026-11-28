// H1g — save/load por domínio: crédito, bancos, moeda, câmbio e bolsa.
//
// Blocos movidos VERBATIM de save_state.cpp (mesmas linhas, mesma ordem de
// escrita de chaves). Qualquer reordenação muda os bytes do save.
#include "save_internal.h"

#include "../bank.h"
#include "../bank_crisis.h"
#include "../central_bank.h"
#include "../exchange.h"
#include "../sim_constants.h"
#include "../sim_core.h"
#include "../stock.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace ogs::save {
using namespace detail;

namespace finance {

void write(Dictionary& data, const WorldState& ws) {
	// F5: moedas (taxa, base, regime, balança) e tarifas declaradas.
	Array currencies = make_array();
	for (const Currency& currency : ws.currencies) {
		Dictionary entry = make_object();
		entry["code"] = sim_str(currency.code.c_str());
		entry["rate_per_anchor"] = currency.rate;
		entry["base_rate"] = currency.base_rate;
		entry["floating"] = currency.floating;
		entry["fallback"] = currency.fallback;
		entry["source"] = sim_str(currency.source.c_str());
		entry["balance_ema"] = currency.balance_ema;
		entry["gross_ema"] = currency.gross_ema;
		// C2a (v27/D-103): política da moeda. Salva, sem mecânica nesta fase.
		entry["exchange_regime"] = sim_str(
			central_bank::regime_name(currency.exchange_regime));
		entry["bank_independence"] = sim_str(
			central_bank::independence_name(currency.bank_independence));
		Array nations = make_array();
		for (const std::string& nation : currency.nations) {
			nations.append(sim_str(nation.c_str()));
		}
		entry["nations"] = nations;
		currencies.append(entry);
	}
	data["currencies"] = currencies;
	Array tariffs = make_array();
	for (const Tariff& tariff : ws.tariffs) {
		Dictionary entry = make_object();
		entry["buyer"] = sim_str(tariff.buyer.c_str());
		entry["seller"] = sim_str(tariff.seller.c_str());
		entry["product"] = sim_str(tariff.product.c_str());
		entry["rate"] = tariff.rate;
		tariffs.append(entry);
	}
	data["tariffs"] = tariffs;
	// C2a (v27/D-103) — BANCO CENTRAL POR MOEDA: política do mundo, reserva de
	// ouro do BC por moeda (dado real da G2) e a série mensal do CPI. Tudo é
	// ADITIVO: save v26 carrega com fiduciário/USD (defaults declarados pelo
	// plano), ouro 0 sem declaração e série vazia — nada inventado para trás.
	{
		Dictionary policy = make_object();
		policy["standard"] = sim_str(
			central_bank::standard_name(ws.monetary_standard));
		policy["reserve_currency"] = sim_str(ws.reserve_currency.c_str());
		data["monetary_policy"] = policy;
	}
	{
		Dictionary bank = make_object();
		bank["gold_declared"] = ws.reserve_gold_declared;
		Dictionary gold = make_object();
		for (const auto& row : ws.reserve_gold_tonnes) {
			gold[sim_str(row.first.c_str())] = row.second;
		}
		bank["gold_tonnes_by_currency"] = gold;
		bank["gold_unallocated_tonnes"] = ws.reserve_gold_unallocated;
		bank["cpi_last_day"] = ws.cpi_last_observe_day;
		Dictionary cpi = make_object();
		for (const auto& row : ws.cpi_history) {
			if (row.second.count <= 0) {
				continue;
			}
			Dictionary entry = make_object();
			Array values = make_array();
			Array days = make_array();
			const int count = std::clamp(row.second.count, 0,
				central_bank::kCpiSampleSlots);
			for (int index = 0; index < count; ++index) {
				values.append(row.second.samples[index]);
				days.append(row.second.sample_day[index]);
			}
			entry["index"] = values;
			entry["days"] = days;
			cpi[sim_str(row.first.c_str())] = entry;
		}
		bank["cpi"] = cpi;
		data["central_bank"] = bank;
	}
	// C2b1 (v28/D-104) — POLÍTICA MONETÁRIA ATIVA: taxa básica por moeda, M0
	// emitido por operação contabilizada, carteira de títulos do BC e dívida
	// pública por nação. ADITIVO: v27 carrega com taxa sem decisão (resolve no
	// neutro do jogo), emissão 0 e dívida 0 — nada inventado para trás.
	{
		Dictionary block = make_object();
		Dictionary policies = make_object();
		for (const auto& row : ws.monetary) {
			Dictionary entry = make_object();
			entry["rate"] = row.second.rate;
			entry["emission"] = row.second.emission;
			entry["securities"] = row.second.securities;
			entry["last_day"] = row.second.last_day;
			policies[sim_str(row.first.c_str())] = entry;
		}
		block["policies"] = policies;
		Dictionary bonds = make_object();
		for (const auto& row : ws.gov_bonds) {
			bonds[sim_str(row.first.c_str())] = row.second;
		}
		block["gov_bonds"] = bonds;
		// C6 (D-114): títulos do resgate sem BC em poder do exterior (claim do
		// credor; pago pela posição externa). Aditivo no v34 — ausente = 0.
		Dictionary external_bonds = make_object();
		for (const auto& row : ws.external_bonds) {
			external_bonds[sim_str(row.first.c_str())] = row.second;
		}
		block["external_bonds"] = external_bonds;
		// Janela do déficit (medição que alimenta a decisão do dia seguinte):
		// sem ela, um load recomeçaria a janela no caixa do load e a
		// monetização divergiria do caminho direto.
		Dictionary cash_open = make_object();
		for (const auto& row : ws.gov_cash_open) {
			cash_open[sim_str(row.first.c_str())] = row.second;
		}
		block["gov_cash_open"] = cash_open;
		data["monetary"] = block;
	}
	// C2b2 (v29/D-106) — CÂMBIO: paridade, largura da banda, reservas em
	// moeda de reserva, preço fixo de conversão do ouro, cobertura e o anel de
	// eventos. ADITIVO: v28 carrega com paridade 0 (o `base_rate` do F5),
	// banda no default, reservas 0 (fixo administrativo) e nenhum evento.
	{
		Dictionary block = make_object();
		Dictionary currencies = make_object();
		for (const auto& row : ws.fx) {
			Dictionary entry = make_object();
			entry["parity"] = row.second.parity;
			entry["band_width"] = row.second.band_width;
			entry["reserves"] = row.second.reserves;
			entry["conversion_price"] = row.second.conversion_price;
			entry["funded"] = row.second.funded;
			entry["devaluations"] = row.second.devaluations;
			entry["last_event_day"] = row.second.last_event_day;
			// C2c (v31/D-108): composição da cesta resolvida no mundo.
			Dictionary basket = make_object();
			for (const auto& unit : row.second.basket) {
				basket[sim_str(unit.first.c_str())] = unit.second;
			}
			entry["basket"] = basket;
			entry["basket_source"] =
				sim_str(row.second.basket_source.c_str());
			currencies[sim_str(row.first.c_str())] = entry;
		}
		block["currencies"] = currencies;
		Array events = make_array();
		for (const FxEvent& row : ws.fx_events) {
			Dictionary entry = make_object();
			entry["day"] = row.day;
			entry["currency"] = sim_str(row.currency.c_str());
			entry["type"] = sim_str(row.type.c_str());
			entry["old_parity"] = row.old_parity;
			entry["new_parity"] = row.new_parity;
			entry["reserves_used"] = row.reserves_used;
			events.append(entry);
		}
		block["events"] = events;
		Dictionary params = make_object();
		params["band_width"] = ws.fx_band_width;
		params["defence_share"] = ws.fx_defence_share;
		params["gold_reserve_ratio"] = ws.fx_gold_reserve_ratio;
		block["parameters"] = params;
		data["fx"] = block;
	}
	// C2c (v31/D-108) — REGIME REAL por moeda (IMF AREAER 30/04/2026): regime,
	// âncora (moeda ou cesta), fonte, emissor e a trava de união monetária.
	// ADITIVO: v30 carrega os regimes do arquivo pelo `set_fx_regimes`.
	{
		Dictionary block = make_object();
		block["source"] = sim_str(ws.fx_regime_source.c_str());
		Array no_policy = make_array();
		for (const std::string& nation : ws.fx_no_policy_nations) {
			no_policy.append(sim_str(nation.c_str()));
		}
		block["no_policy"] = no_policy;
		Dictionary countries = make_object();
		for (const auto& row : ws.fx_regime) {
			Dictionary entry = make_object();
			entry["regime"] =
				sim_str(central_bank::regime_name(row.second.regime));
			entry["anchor"] = sim_str(row.second.anchor.c_str());
			entry["anchor_type"] = sim_str(row.second.anchor_type.c_str());
			entry["anchor_source"] =
				sim_str(row.second.anchor_source.c_str());
			entry["issuer"] = sim_str(row.second.issuer.c_str());
			entry["locked"] = row.second.locked;
			entry["source"] = sim_str(row.second.source.c_str());
			Dictionary basket = make_object();
			for (const auto& unit : row.second.basket) {
				basket[sim_str(unit.first.c_str())] = unit.second;
			}
			entry["basket"] = basket;
			countries[sim_str(row.first.c_str())] = entry;
		}
		block["currencies"] = countries;
		data["fx_regimes"] = block;
	}
	// C3 (v30/D-107) — CRÉDITO DE EMPRESA: banco agregado por nação e o
	// estado do crédito. ADITIVO: v29 carrega desligado, sem banco e com
	// dívida 0 (nada inventado para trás).
	{
		Dictionary block = make_object();
		block["enabled"] = ws.bank_credit_enabled;
		// R2a: bloco do rework (só no ON; OFF não ganha chave).
		if (ws.banks_rework.enabled) {
			Dictionary rework = make_object();
			rework["capital"] = ws.banks_rework.capital;
			rework["deposits"] = ws.banks_rework.deposits;
			rework["resolution"] = ws.banks_rework.resolution;
			rework["dividend_target_ratio"] =
				ws.banks_rework.dividend_target_ratio;
			rework["dividend_cash_floor_ratio"] =
				ws.banks_rework.dividend_cash_floor_ratio;
			rework["seed_enabled"] = ws.banks_rework.seed_enabled;
			rework["seed_floor_days_treasury"] =
				ws.banks_rework.seed_floor_days_treasury;
			rework["seed_public_month_frac"] =
				ws.banks_rework.seed_public_month_frac;
			rework["seed_public_min_ticket_frac"] =
				ws.banks_rework.seed_public_min_ticket_frac;
			rework["protected_food_months"] =
				ws.banks_rework.protected_food_months;
			rework["alloc_min_capital_ratio"] =
				ws.banks_rework.alloc_min_capital_ratio;
			rework["alloc_pendulum_month"] =
				ws.banks_rework.alloc_pendulum_month;
			rework["alloc_w_rel"] = ws.banks_rework.alloc_w_rel;
			rework["alloc_w_sol"] = ws.banks_rework.alloc_w_sol;
			rework["alloc_w_pend"] = ws.banks_rework.alloc_w_pend;
			rework["liquidity_min_reserve_ratio"] =
				ws.banks_rework.liquidity_min_reserve_ratio;
			// R2c: parametros do leilao e do charter publico emergencial.
			rework["auction_discount_private"] =
				ws.banks_rework.auction_discount_private;
			rework["auction_discount_public"] =
				ws.banks_rework.auction_discount_public;
			rework["charter_max_pct_gdp"] =
				ws.banks_rework.charter_max_pct_gdp;
			rework["charter_max_treasury_months"] =
				ws.banks_rework.charter_max_treasury_months;
			rework["seed_source"] =
				sim_str(ws.banks_rework.seed_source.c_str());
			block["rework"] = rework;
		}
		Dictionary rows = make_object();
		for (const auto& row : ws.banks) {
			const Bank& bank = row.second;
			Dictionary entry = make_object();
			entry["id"] = sim_str(bank.id.c_str());
			entry["nation"] = sim_str(bank.nation.c_str());
			entry["role"] = sim_str(bank::role_key(bank.role));
			entry["name"] = sim_str(bank.name.c_str());
			entry["name_status"] = sim_str(bank.name_status.c_str());
			entry["owner_kind"] = sim_str(bank.owner_kind.c_str());
			entry["owner_nation"] = sim_str(bank.owner_nation.c_str());
			entry["share"] = bank.share;
			entry["currency"] = sim_str(bank.currency.c_str());
			entry["loans"] = bank.loans;
			entry["arrears"] = bank.arrears;
			entry["deposits"] = bank.deposits;
			entry["bc_borrowing"] = bank.bc_borrowing;
			entry["cash"] = bank.cash;
			entry["capital"] = bank.capital;
			entry["interest_income"] = bank.interest_income;
			entry["interest_expense"] = bank.interest_expense;
			entry["writeoffs"] = bank.writeoffs;
			entry["dividends"] = bank.dividends;
			entry["remittance"] = bank.remittance;
			entry["remittance_origin"] = bank.remittance_origin;
			entry["harvest_interest"] = bank.harvest_interest;
			entry["defaults"] = bank.defaults;
			entry["loans_count"] = bank.loans_count;
			entry["last_default_day"] = bank.last_default_day;
			entry["last_dividend_day"] = bank.last_dividend_day;
			// C6 (D-114): estado de crise por banco (confianca, corrida,
			// redesconto, seguro, bail-in, resolucao).
			entry["confidence"] = bank.confidence;
			entry["loss_seeded"] = bank.loss_seeded;
			entry["loss_ema"] = bank.loss_ema;
			// Perda reconhecida HOJE: sem ela, um load no meio do dia perdia o
			// choque que ainda não entrou na EMA e a continuação divergia.
			entry["loss_today"] = bank.loss_today;
			entry["run_days"] = bank.run_days;
			entry["last_run_day"] = bank.last_run_day;
			entry["run_withdrawn"] = bank.run_withdrawn;
			entry["run_cash"] = bank.run_cash;
			entry["run_public"] = bank.run_public;
			entry["lolr_borrowed"] = bank.lolr_borrowed;
			entry["lolr_interest"] = bank.lolr_interest;
			entry["lolr_days"] = bank.lolr_days;
			entry["insured_payout"] = bank.insured_payout;
			entry["bailin_cut"] = bank.bailin_cut;
			entry["treasury_rescue"] = bank.treasury_rescue;
			entry["capital_flight"] = bank.capital_flight;
			entry["resolution_day"] = bank.resolution_day;
			entry["resolved"] = bank.resolved;
			entry["resolution"] = sim_str(bank.resolution.c_str());
			entry["absorbed_into"] = sim_str(bank.absorbed_into.c_str());
			entry["baseline_seeded"] = bank.baseline_seeded;
			entry["open_capital_ratio"] = bank.open_capital_ratio;
			entry["open_liquidity_ratio"] = bank.open_liquidity_ratio;
			entry["last_capital_ratio"] = bank.last_capital_ratio;
			entry["last_liquidity_ratio"] = bank.last_liquidity_ratio;
			entry["last_npl_ratio"] = bank.last_npl_ratio;
			if (ws.banks_rework.enabled) {
				// R2a: só no ON (save OFF byte-idêntico ao anterior).
				entry["deposits_funding"] = bank.deposits_funding;
				// R2b1 (fix A3): sinal de liquidez acendido pelo saque que
				// deixa o caixa abaixo do piso (registro, nao bloqueio).
				entry["liquidity_floor_breaches"] =
					bank.liquidity_floor_breaches;
				entry["last_liquidity_floor_breach_day"] =
					bank.last_liquidity_floor_breach_day;
				entry["capital_ratio_target"] = bank.capital_ratio_target;
				entry["capital_target"] = bank.capital_target;
				entry["seed_scale"] = bank.seed_scale;
				entry["reference_scale"] = bank.reference_scale;
				entry["gov_bonds"] = bank.gov_bonds;
				entry["charter"] = sim_str(bank.charter.c_str());
				entry["public_cap_deferred"] =
					sim_str(bank.public_cap_deferred.c_str());
			}
			rows[sim_str(row.first.c_str())] = entry;
		}
		block["banks"] = rows;
		data["banks"] = block;
	}
	// C6 (v34/D-114) - CRISE BANCARIA: regra, fundo de seguro por nacao e o
	// anel de eventos. ADITIVO: v33 e anteriores chegam sem o bloco e a
	// migracao (`bank_crisis::after_load`) inicia os bancos calmos.
	{
		Dictionary block = make_object();
		block["enabled"] = ws.bank_crisis.enabled;
		Dictionary policy = make_object();
		policy["capital_min"] = ws.bank_crisis.capital_min;
		policy["capital_cut_floor"] = ws.bank_crisis.capital_cut_floor;
		policy["liquidity_min"] = ws.bank_crisis.liquidity_min;
		policy["loss_scale"] = ws.bank_crisis.loss_scale;
		policy["loss_ema_days"] = ws.bank_crisis.loss_ema_days;
		policy["confidence_start"] = ws.bank_crisis.confidence_start;
		policy["smoothing"] = ws.bank_crisis.smoothing;
		policy["w_solvency"] = ws.bank_crisis.w_solvency;
		policy["w_liquidity"] = ws.bank_crisis.w_liquidity;
		policy["w_losses"] = ws.bank_crisis.w_losses;
		policy["w_contagion"] = ws.bank_crisis.w_contagion;
		policy["w_market"] = ws.bank_crisis.w_market;
		policy["run_threshold"] = ws.bank_crisis.run_threshold;
		policy["run_share"] = ws.bank_crisis.run_share;
		policy["flight_to_quality"] = ws.bank_crisis.flight_to_quality;
		policy["lolr_spread"] = ws.bank_crisis.lolr_spread;
		policy["lolr_haircut"] = ws.bank_crisis.lolr_haircut;
		policy["lolr_max_assets"] = ws.bank_crisis.lolr_max_assets;
		policy["insured_share"] = ws.bank_crisis.insured_share;
		policy["premium_daily"] = ws.bank_crisis.premium_daily;
		policy["fund_target_ratio"] = ws.bank_crisis.fund_target_ratio;
		policy["capital_flight_share"] =
			ws.bank_crisis.capital_flight_share;
		policy["insolvency_tolerance"] =
			ws.bank_crisis.insolvency_tolerance;
		policy["insolvency_tolerance_ratio"] =
			ws.bank_crisis.insolvency_tolerance_ratio;
		policy["treasury_debt_rescue"] =
			ws.bank_crisis.treasury_debt_rescue;
		policy["takeover_public"] = ws.bank_crisis.takeover_public;
		block["policy"] = policy;
		Dictionary funds = make_object();
		for (const auto& fund_row : ws.deposit_insurance) {
			Dictionary entry = make_object();
			entry["fund"] = fund_row.second.fund;
			entry["premiums"] = fund_row.second.premiums;
			entry["payouts"] = fund_row.second.payouts;
			entry["last_payout_day"] = fund_row.second.last_payout_day;
			funds[sim_str(fund_row.first.c_str())] = entry;
		}
		block["funds"] = funds;
		Array events = make_array();
		for (const BankCrisisEvent& event : ws.bank_crisis_events) {
			Dictionary entry = make_object();
			entry["day"] = event.day;
			entry["bank"] = sim_str(event.bank.c_str());
			entry["nation"] = sim_str(event.nation.c_str());
			entry["kind"] = sim_str(event.kind.c_str());
			entry["cause"] = sim_str(event.cause.c_str());
			entry["gap"] = event.gap;
			entry["cost_shareholders"] = event.cost_shareholders;
			entry["cost_depositors"] = event.cost_depositors;
			entry["cost_fund"] = event.cost_fund;
			entry["cost_treasury"] = event.cost_treasury;
			entry["cost_takeover"] = event.cost_takeover;
			entry["capital_flight"] = event.capital_flight;
			entry["lolr"] = event.lolr;
			entry["emission"] = event.emission;
			// R2c: episodio de resolucao (leilao/charter/austeridade/deserto).
			entry["buyer"] = sim_str(event.buyer.c_str());
			entry["auction_book"] = event.auction_book;
			entry["auction_price"] = event.auction_price;
			entry["charter_cost"] = event.charter_cost;
			entry["charter_cap"] = event.charter_cap;
			entry["austerity"] = event.austerity;
			entry["desert"] = event.desert;
			entry["recapitalized"] = event.recapitalized;
			events.append(entry);
		}
		block["events"] = events;
		block["events_total"] = ws.bank_crisis_events_total;
		data["bank_crisis"] = block;
	}
	// C5 (v33/D-112) - BOLSA: uma praça por nação e um papel por grupo
	// listado. ADITIVO: v32 e anteriores chegam sem o bloco e recebem a bolsa
	// criada na migração (preço = patrimônio medido; nada inventado).
	{
		Dictionary block = make_object();
		block["enabled"] = ws.stock_enabled;
		Dictionary policy = make_object();
		policy["investment_channel"] = ws.stock_investment_channel;
		policy["risk_premium"] = ws.stock_risk_premium;
		policy["payout_ratio"] = ws.stock_payout_ratio;
		policy["step"] = ws.stock_step;
		policy["expectation_k"] = ws.stock_expectation_k;
		block["policy"] = policy;
		Dictionary exchanges = make_object();
		for (const auto& row : ws.stock_exchanges) {
			const StockExchange& exchange = row.second;
			Dictionary entry = make_object();
			entry["nation"] = sim_str(exchange.nation.c_str());
			entry["name"] = sim_str(exchange.name.c_str());
			entry["name_status"] = sim_str(exchange.name_status.c_str());
			entry["currency"] = sim_str(exchange.currency.c_str());
			entry["confidence"] = exchange.confidence;
			entry["index_base"] = exchange.index_base;
			entry["last_price_day"] = exchange.last_price_day;
			entry["production_ema"] = exchange.production_ema;
			entry["production_seeded"] = exchange.production_seeded;
			entry["dividends_paid"] = exchange.dividends_paid;
			entry["remittance"] = exchange.remittance;
			entry["remittance_origin"] = exchange.remittance_origin;
			exchanges[sim_str(row.first.c_str())] = entry;
		}
		block["exchanges"] = exchanges;
		Dictionary tickers = make_object();
		for (const auto& row : ws.stock_tickers) {
			const StockTicker& ticker = row.second;
			Dictionary entry = make_object();
			entry["group_id"] = sim_str(ticker.group_id.c_str());
			entry["nation"] = sim_str(ticker.nation.c_str());
			entry["exchange"] = sim_str(ticker.exchange.c_str());
			entry["shares"] = ticker.shares;
			entry["price"] = ticker.price;
			entry["book_equity"] = ticker.book_equity;
			entry["profit_ema"] = ticker.profit_ema;
			entry["profit_at_last_dividend"] =
				ticker.profit_at_last_dividend;
			entry["dividend_last"] = ticker.dividend_last;
			entry["dividend_limited"] = ticker.dividend_limited;
			entry["dividend_day"] = ticker.dividend_day;
			entry["listed_day"] = ticker.listed_day;
			entry["delisted_day"] = ticker.delisted_day;
			entry["listed"] = ticker.listed;
			entry["reason"] = sim_str(ticker.reason.c_str());
			Dictionary holdings = make_object();
			holdings["households_high"] = ticker.holdings.households_high;
			Dictionary banks = make_object();
			for (const auto& bank_row : ticker.holdings.banks) {
				banks[sim_str(bank_row.first.c_str())] = bank_row.second;
			}
			holdings["banks"] = banks;
			Dictionary groups = make_object();
			for (const auto& group_row : ticker.holdings.groups) {
				groups[sim_str(group_row.first.c_str())] = group_row.second;
			}
			holdings["groups"] = groups;
			holdings["foreign"] = ticker.holdings.foreign;
			entry["holdings"] = holdings;
			tickers[sim_str(row.first.c_str())] = entry;
		}
		block["tickers"] = tickers;
		data["stock"] = block;
	}
}

void read_money(WorldState& ws, SimCore& core, const Dictionary& data) {
	// F5: moedas e tarifas (save v6). Carregadas ANTES das cidades porque o
	// preço-base local (moeda da nação) depende da taxa. Save v5 sem as chaves
	// mantém o que já está carregado pelo autoload (mundo em XGS se não houver).
	if (data.has("currencies")) {
		const Array saved_currencies = data.get("currencies", Array());
		ws.currencies.clear();
		ws.currencies.reserve(static_cast<std::size_t>(saved_currencies.size()));
		for (int index = 0; index < saved_currencies.size(); ++index) {
			const Dictionary entry = saved_currencies[index];
			const String code = String(entry.get("code", ""));
			if (code.is_empty()) {
				continue;
			}
			Currency currency;
			currency.code = std::string(code.utf8().get_data());
			currency.rate = double(entry.get("rate_per_anchor", 1.0));
			if (!std::isfinite(currency.rate) || currency.rate <= 0.0) {
				currency.rate = 1.0;
			}
			currency.base_rate = double(entry.get("base_rate", currency.rate));
			if (!std::isfinite(currency.base_rate) || currency.base_rate <= 0.0) {
				currency.base_rate = currency.rate;
			}
			currency.floating = bool(entry.get("floating", true));
			currency.fallback = bool(entry.get("fallback", false));
			currency.source = std::string(
				String(entry.get("source", "default")).utf8().get_data());
			currency.balance_ema = double(entry.get("balance_ema", 0.0));
			currency.gross_ema = double(entry.get("gross_ema", 0.0));
			// C2a (v27): política da moeda. Save v26 e anteriores NÃO têm as
			// chaves — o regime nasce do `floating` que já existia (peg é o
			// default do dado) e a independência nasce `no_data` (ausência
			// declarada; nada é inventado para trás).
			currency.exchange_regime = central_bank::regime_from_name(
				std::string(String(entry.get("exchange_regime", ""))
					.utf8().get_data()),
				currency.floating ? central_bank::kRegimeFloating
					: central_bank::kRegimePeg);
			currency.bank_independence = central_bank::independence_from_name(
				std::string(String(entry.get("bank_independence", ""))
					.utf8().get_data()),
				central_bank::kIndependenceNoData);
			const Variant nations_variant = entry.get("nations", Array());
			if (nations_variant.get_type() == Variant::ARRAY) {
				const Array nations = nations_variant;
				for (int n = 0; n < nations.size(); ++n) {
					const std::string nation(
						nations[n].operator String().utf8().get_data());
					if (!nation.empty()) {
						currency.nations.push_back(nation);
					}
				}
			}
			ws.currencies.push_back(std::move(currency));
		}
		core.rebuild_currency_indexes();
	}
	// C2a (v27/D-103) — banco central por moeda: política do mundo, reserva de
	// ouro e série do CPI. Save v26 e anteriores chegam SEM os blocos: o
	// estado volta aos defaults DECLARADOS (fiduciário, moeda de reserva
	// americana — padrões do plano) com ouro 0/sem declaração e série vazia.
	ws.monetary_standard = central_bank::kStandardFiat;
	ws.reserve_currency = "USD";
	ws.reserve_gold_tonnes.clear();
	ws.reserve_gold_declared = false;
	ws.reserve_gold_unallocated = 0.0;
	ws.cpi_history.clear();
	ws.cpi_last_observe_day = -1;
	if (data.has("monetary_policy")) {
		const Dictionary policy = data.get("monetary_policy", Dictionary());
		ws.monetary_standard = central_bank::standard_from_name(
			std::string(String(policy.get("standard", "fiat"))
				.utf8().get_data()),
			central_bank::kStandardFiat);
		const String reserve = String(policy.get("reserve_currency", "USD"));
		if (!reserve.is_empty()) {
			ws.reserve_currency = std::string(reserve.utf8().get_data());
		}
	}
	if (data.has("central_bank")) {
		const Dictionary bank = data.get("central_bank", Dictionary());
		ws.reserve_gold_declared = bool(bank.get("gold_declared", false));
		const Variant gold_variant = bank.get("gold_tonnes_by_currency",
			Dictionary());
		if (gold_variant.get_type() == Variant::DICTIONARY) {
			const Dictionary& gold = gold_variant;
			for (const auto& row : gold.entries()) {
				if (row.first.empty()) {
					continue;
				}
				const double tonnes = double(row.second);
				if (!std::isfinite(tonnes) || tonnes <= 0.0) {
					continue;
				}
				ws.reserve_gold_tonnes[row.first] = tonnes;
			}
		}
		ws.reserve_gold_unallocated = std::max(0.0,
			double(bank.get("gold_unallocated_tonnes", 0.0)));
		ws.cpi_last_observe_day = int(bank.get("cpi_last_day", -1));
		const Variant cpi_variant = bank.get("cpi", Dictionary());
		if (cpi_variant.get_type() == Variant::DICTIONARY) {
			const Dictionary& cpi = cpi_variant;
			for (const auto& row_entry : cpi.entries()) {
				if (row_entry.first.empty()
						|| row_entry.second.get_type()
							!= Variant::DICTIONARY) {
					continue;
				}
				const Dictionary& row = row_entry.second;
				const Variant values_variant = row.get("index", Array());
				const Variant days_variant = row.get("days", Array());
				if (values_variant.get_type() != Variant::ARRAY) {
					continue;
				}
				const Array& values = values_variant;
				const Array days = days_variant.get_type() == Variant::ARRAY
					? Array(days_variant) : Value::make_array();
				CpiHistory history;
				for (int index = 0;
						index < values.size()
						&& index < central_bank::kCpiSampleSlots; ++index) {
					const double value = double(values[index]);
					if (!std::isfinite(value) || value <= 0.0) {
						continue;
					}
					const int slot = history.count;
					history.samples[slot] = value;
					history.sample_day[slot] = index < days.size()
						? int(days[index]) : 0;
					history.count = slot + 1;
				}
				if (history.count > 0) {
					ws.cpi_history[row_entry.first] = history;
				}
			}
		}
	}
	// C2b1 (v28/D-104) — política monetária ativa. Save v27 e anteriores
	// chegam SEM o bloco: taxa sem decisão (`rate < 0` → neutro do jogo),
	// emissão/carteira/dívida 0 e a emissão do dia 0 (só o dia conta).
	ws.monetary.clear();
	ws.gov_bonds.clear();
	ws.external_bonds.clear();
	ws.gov_cash_open.clear();
	ws.emission_today = 0.0;
	if (data.has("monetary")) {
		const Dictionary block = data.get("monetary", Dictionary());
		const Variant policies_variant = block.get("policies", Dictionary());
		if (policies_variant.get_type() == Variant::DICTIONARY) {
			const Dictionary& policies = policies_variant;
			for (const auto& row_entry : policies.entries()) {
				if (row_entry.first.empty()
						|| row_entry.second.get_type()
							!= Variant::DICTIONARY) {
					continue;
				}
				const Dictionary& row = row_entry.second;
				MonetaryState state;
				const double rate = double(row.get("rate", -1.0));
				state.rate = std::isfinite(rate) ? rate : -1.0;
				const double emission = double(row.get("emission", 0.0));
				state.emission = std::isfinite(emission)
					? std::max(0.0, emission) : 0.0;
				const double securities = double(row.get("securities", 0.0));
				state.securities = std::isfinite(securities)
					? std::max(0.0, securities) : 0.0;
				state.last_day = int(row.get("last_day", -1));
				ws.monetary[row_entry.first] = state;
			}
		}
		const Variant bonds_variant = block.get("gov_bonds", Dictionary());
		if (bonds_variant.get_type() == Variant::DICTIONARY) {
			const Dictionary& bonds = bonds_variant;
			for (const auto& row : bonds.entries()) {
				if (row.first.empty()) {
					continue;
				}
				const double value = double(row.second);
				if (std::isfinite(value) && value > 0.0) {
					ws.gov_bonds[row.first] = value;
				}
			}
		}
		const Variant open_variant = block.get("gov_cash_open", Dictionary());
		if (open_variant.get_type() == Variant::DICTIONARY) {
			const Dictionary& cash = open_variant;
			for (const auto& row : cash.entries()) {
				if (row.first.empty()) {
					continue;
				}
				const double value = double(row.second);
				if (std::isfinite(value)) {
					ws.gov_cash_open[row.first] = value;
				}
			}
		}
		// C6 (D-114): títulos do resgate sem BC em poder do exterior. v33 e
		// anteriores chegam sem a chave: mapa vazio (ausência declarada).
		const Variant external_variant =
			block.get("external_bonds", Dictionary());
		if (external_variant.get_type() == Variant::DICTIONARY) {
			const Dictionary& external = external_variant;
			for (const auto& row : external.entries()) {
				if (row.first.empty()) {
					continue;
				}
				const double value = double(row.second);
				if (std::isfinite(value) && value > 0.0) {
					ws.external_bonds[row.first] = value;
				}
			}
		}
	}
	// C2b2 (v29/D-106) — CÂMBIO. Save v28 e anteriores chegam SEM o bloco:
	// paridade 0 (o `base_rate` do F5), banda no default de jogo, reservas 0
	// (fixo administrativo, como o F5), sem eventos e sem desvalorização.
	ws.fx.clear();
	ws.fx_events.clear();
	ws.fx_band_width = 0.0;
	ws.fx_defence_share = 0.0;
	ws.fx_gold_reserve_ratio = 0.0;
	if (data.has("fx")) {
		const Dictionary block = data.get("fx", Dictionary());
		const Variant currencies_variant = block.get("currencies", Dictionary());
		if (currencies_variant.get_type() == Variant::DICTIONARY) {
			const Dictionary& currencies = currencies_variant;
			for (const auto& row_entry : currencies.entries()) {
				if (row_entry.first.empty()
						|| row_entry.second.get_type()
							!= Variant::DICTIONARY) {
					continue;
				}
				const Dictionary& row = row_entry.second;
				FxState state;
				state.parity = std::max(0.0,
					double(row.get("parity", 0.0)));
				state.band_width = std::clamp(
					double(row.get("band_width", -1.0)), -1.0, 1.0);
				state.reserves = std::max(0.0,
					double(row.get("reserves", 0.0)));
				state.conversion_price = std::max(0.0,
					double(row.get("conversion_price", 0.0)));
				state.funded = bool(row.get("funded", false));
				state.devaluations = std::max(0,
					int(row.get("devaluations", 0)));
				state.last_event_day = int(row.get("last_event_day", -1));
				// C2c (v31/D-108): cesta resolvida (v30 chega vazia e o
				// `resolve_baskets` a preenche pelo comércio medido).
				const Variant basket_variant = row.get("basket", Variant());
				if (basket_variant.get_type() == Variant::DICTIONARY) {
					const Dictionary basket = basket_variant;
					for (const auto& unit : basket.entries()) {
						const std::string code(
							String(unit.first).utf8().get_data());
						const double weight = double(unit.second);
						if (!code.empty() && std::isfinite(weight)
								&& weight > 0.0) {
							state.basket[code] = weight;
						}
					}
				}
				state.basket_source = std::string(
					String(row.get("basket_source", "")).utf8().get_data());
				ws.fx[row_entry.first] = state;
			}
		}
		const Variant events_variant = block.get("events", Array());
		if (events_variant.get_type() == Variant::ARRAY) {
			const Array& events = events_variant;
			for (int index = 0; index < events.size(); ++index) {
				if (events[index].get_type() != Variant::DICTIONARY) {
					continue;
				}
				const Dictionary entry = events[index];
				FxEvent row;
				row.day = int(entry.get("day", -1));
				row.currency = std::string(
					String(entry.get("currency", "")).utf8().get_data());
				row.type = std::string(
					String(entry.get("type", "")).utf8().get_data());
				row.old_parity = double(entry.get("old_parity", 0.0));
				row.new_parity = double(entry.get("new_parity", 0.0));
				row.reserves_used = double(entry.get("reserves_used", 0.0));
				if (row.currency.empty()) {
					continue;
				}
				ws.fx_events.push_back(std::move(row));
			}
			if (static_cast<int>(ws.fx_events.size()) > 64) {
				ws.fx_events.erase(ws.fx_events.begin(),
					ws.fx_events.end() - 64);
			}
		}
		const Variant params_variant = block.get("parameters", Dictionary());
		if (params_variant.get_type() == Variant::DICTIONARY) {
			const Dictionary& params = params_variant;
			ws.fx_band_width = std::max(0.0,
				double(params.get("band_width", 0.0)));
			ws.fx_defence_share = std::max(0.0,
				double(params.get("defence_share", 0.0)));
			ws.fx_gold_reserve_ratio = std::max(0.0,
				double(params.get("gold_reserve_ratio", 0.0)));
		}
	}
	// C3 (v30/D-107) — CRÉDITO. Save v29 e anteriores chegam SEM o bloco:
	// desligado, sem banco e com dívida 0 (a migração não inventa credor).
	ws.banks.clear();
	ws.bank_credit_enabled = false;
	if (data.has("banks")) {
		const Dictionary block = data.get("banks", Dictionary());
		ws.bank_credit_enabled = bool(block.get("enabled", false));
		// R2a (v44): bloco de politica dos bancos. Save anterior não tem a
		// chave: a configuracao do rework.json continua valendo e o load não
		// capitaliza retroativamente (`seed_source = loaded_save` na validação).
		const Variant rework_variant = block.get("rework", Variant());
		if (rework_variant.get_type() == Variant::DICTIONARY) {
			const Dictionary rework = rework_variant;
			BanksRework& br = ws.banks_rework;
			br.enabled = true;
			br.capital = bool(rework.get("capital", true));
			br.deposits = bool(rework.get("deposits", false));
			// R2c (QA t206): a config trata `resolution` ausente como
			// "segue deposits" (definitions_loader); o load tem de fazer o
			// mesmo, senao um save v44 sem a chave perde o resolvedor que a
			// config ligou. Com a chave presente vale o valor gravado.
			br.resolution = bool(rework.get("resolution", br.deposits));
			br.dividend_target_ratio = double(
				rework.get("dividend_target_ratio", 0.10));
			br.dividend_cash_floor_ratio = double(
				rework.get("dividend_cash_floor_ratio", 0.10));
			br.seed_enabled = bool(rework.get("seed_enabled", true));
			br.seed_floor_days_treasury = double(
				rework.get("seed_floor_days_treasury", 3.0));
			br.seed_public_month_frac = double(
				rework.get("seed_public_month_frac", 0.10));
			br.seed_public_min_ticket_frac = double(
				rework.get("seed_public_min_ticket_frac", 0.25));
			br.protected_food_months = double(
				rework.get("protected_food_months", 6.0));
			br.alloc_min_capital_ratio = double(
				rework.get("alloc_min_capital_ratio", 0.08));
			br.alloc_pendulum_month = double(
				rework.get("alloc_pendulum_month", 0.10));
			br.alloc_w_rel = double(rework.get("alloc_w_rel", 0.50));
			br.alloc_w_sol = double(rework.get("alloc_w_sol", 0.35));
			br.alloc_w_pend = double(rework.get("alloc_w_pend", 0.15));
			br.liquidity_min_reserve_ratio = double(
				rework.get("liquidity_min_reserve_ratio", 0.10));
			// R2c: save anterior (v44 sem as chaves) herda os defaults do
			// bloco, nao desliga o leilao/charter por ausencia.
			br.auction_discount_private = double(
				rework.get("auction_discount_private", 0.30));
			br.auction_discount_public = double(
				rework.get("auction_discount_public", 0.45));
			br.charter_max_pct_gdp = double(
				rework.get("charter_max_pct_gdp", 0.005));
			br.charter_max_treasury_months = double(
				rework.get("charter_max_treasury_months", 3.0));
			br.seed_source = std::string(
				String(rework.get("seed_source", "loaded_save"))
					.utf8().get_data());
		} else if (!ws.banks_rework.enabled) {
			ws.banks_rework.seed_source = "loaded_save";
		}
		// v32 (C4/D-110): um registro por banco (`banks`), com papel e dono.
		const Value& rows_variant = block.get("banks", Value());
		const Value& nations_variant = block.get("nations", Value());
		if (rows_variant.get_type() == Variant::DICTIONARY
				&& !Dictionary(rows_variant).is_empty()) {
			const Dictionary& rows = rows_variant;
			for (const auto& row_entry : rows.entries()) {
				if (row_entry.first.empty()
						|| row_entry.second.get_type()
							!= Variant::DICTIONARY) {
					continue;
				}
				const Dictionary& row = row_entry.second;
				Bank bank;
				bank.id = row_entry.first;
				bank.nation = std::string(String(row.get("nation", ""))
					.utf8().get_data());
				if (bank.nation.empty()) {
					const std::size_t slash = bank.id.find('/');
					bank.nation = slash == std::string::npos ? bank.id
						: bank.id.substr(0, slash);
				}
				int role = bank::role_from_key(std::string(
					String(row.get("role", "")).utf8().get_data()));
				if (role < 0) {
					const std::size_t slash = bank.id.find('/');
					role = slash == std::string::npos ? -1
						: bank::role_from_key(bank.id.substr(slash + 1));
				}
				bank.role = role < 0 ? 1 : role;
				bank.name = std::string(String(row.get("name", ""))
					.utf8().get_data());
				bank.name_status = std::string(
					String(row.get("name_status", "no_data"))
						.utf8().get_data());
				bank.owner_kind = std::string(
					String(row.get("owner_kind", "national"))
						.utf8().get_data());
				bank.owner_nation = std::string(
					String(row.get("owner_nation", "")).utf8().get_data());
				bank.share = std::max(0.0, double(row.get("share", 0.0)));
				bank.currency = std::string(
					String(row.get("currency", "")).utf8().get_data());
				bank.loans = std::max(0.0, double(row.get("loans", 0.0)));
				bank.arrears = std::max(0.0, double(row.get("arrears", 0.0)));
				bank.deposits = std::max(0.0,
					double(row.get("deposits", 0.0)));
				bank.bc_borrowing = std::max(0.0,
					double(row.get("bc_borrowing", 0.0)));
				bank.cash = std::max(0.0, double(row.get("cash", 0.0)));
				bank.capital = std::max(0.0, double(row.get("capital", 0.0)));
				bank.interest_income = std::max(0.0,
					double(row.get("interest_income", 0.0)));
				bank.interest_expense = std::max(0.0,
					double(row.get("interest_expense", 0.0)));
				bank.writeoffs = std::max(0.0,
					double(row.get("writeoffs", 0.0)));
				bank.dividends = std::max(0.0,
					double(row.get("dividends", 0.0)));
				bank.remittance = std::max(0.0,
					double(row.get("remittance", 0.0)));
				bank.remittance_origin = std::max(0.0,
					double(row.get("remittance_origin", 0.0)));
				bank.harvest_interest = std::max(0.0,
					double(row.get("harvest_interest", 0.0)));
				bank.defaults = std::max(0, int(row.get("defaults", 0)));
				bank.loans_count = std::max(0,
					int(row.get("loans_count", 0)));
				bank.last_default_day = int(
					row.get("last_default_day", -1));
				bank.last_dividend_day = int(
					row.get("last_dividend_day", -1));
				// C6 (D-114): estado de crise. v33 e anteriores nao tem as
				// chaves: o banco carrega CALMO (confianca 1,0, sem corrida,
				// sem resolucao) - nada inventado para tras.
				bank.confidence = std::clamp(
					double(row.get("confidence", 1.0)), 0.0, 1.0);
				bank.loss_seeded = bool(row.get("loss_seeded", false));
				bank.loss_ema = std::max(0.0,
					double(row.get("loss_ema", 0.0)));
				bank.loss_today = std::max(0.0,
					double(row.get("loss_today", 0.0)));
				bank.run_days = std::max(0, int(row.get("run_days", 0)));
				bank.last_run_day = int(row.get("last_run_day", -1));
				bank.run_withdrawn = std::max(0.0,
					double(row.get("run_withdrawn", 0.0)));
				bank.run_cash = std::max(0.0,
					double(row.get("run_cash", 0.0)));
				bank.run_public = std::max(0.0,
					double(row.get("run_public", 0.0)));
				bank.lolr_borrowed = std::max(0.0,
					double(row.get("lolr_borrowed", 0.0)));
				bank.lolr_interest = std::max(0.0,
					double(row.get("lolr_interest", 0.0)));
				bank.lolr_days = std::max(0,
					int(row.get("lolr_days", 0)));
				bank.insured_payout = std::max(0.0,
					double(row.get("insured_payout", 0.0)));
				bank.bailin_cut = std::max(0.0,
					double(row.get("bailin_cut", 0.0)));
				bank.treasury_rescue = std::max(0.0,
					double(row.get("treasury_rescue", 0.0)));
				bank.capital_flight = std::max(0.0,
					double(row.get("capital_flight", 0.0)));
				bank.resolution_day = int(
					row.get("resolution_day", -1));
				bank.resolved = bool(row.get("resolved", false));
				bank.resolution = std::string(String(
					row.get("resolution", "none")).utf8().get_data());
				bank.absorbed_into = std::string(String(
					row.get("absorbed_into", "")).utf8().get_data());
				bank.baseline_seeded = bool(
					row.get("baseline_seeded", false));
				bank.open_capital_ratio = double(
					row.get("open_capital_ratio", 0.0));
				bank.open_liquidity_ratio = double(
					row.get("open_liquidity_ratio", 0.0));
				bank.last_capital_ratio = double(
					row.get("last_capital_ratio", 0.0));
				bank.last_liquidity_ratio = double(
					row.get("last_liquidity_ratio", 0.0));
				bank.last_npl_ratio = double(
					row.get("last_npl_ratio", 0.0));
				// R2a (v44): campos do rework, só quando o bloco existe. Save
				// anterior chega com 0/"" (ausência declarada, sem seed
				// retroativo).
				bank.deposits_funding = std::max(0.0,
					double(row.get("deposits_funding", 0.0)));
				// R2b1 (fix A3): sinal de liquidez (ON; ausente = 0).
				bank.liquidity_floor_breaches = std::max(0,
					int(row.get("liquidity_floor_breaches", 0)));
				bank.last_liquidity_floor_breach_day = int(
					row.get("last_liquidity_floor_breach_day", -1));
				bank.capital_ratio_target = std::max(0.0,
					double(row.get("capital_ratio_target", 0.0)));
				bank.capital_target = std::max(0.0,
					double(row.get("capital_target", 0.0)));
				bank.seed_scale = std::max(0.0,
					double(row.get("seed_scale", 0.0)));
				bank.reference_scale = std::max(0.0,
					double(row.get("reference_scale", 0.0)));
				bank.gov_bonds = std::max(0.0,
					double(row.get("gov_bonds", 0.0)));
				bank.charter = std::string(
					String(row.get("charter", "declared")).utf8().get_data());
				bank.public_cap_deferred = std::string(
					String(row.get("public_cap_deferred", ""))
						.utf8().get_data());
				ws.banks[bank.id] = bank;
			}
		} else if (nations_variant.get_type() == Variant::DICTIONARY) {
			// MIGRAÇÃO v31 -> v32: o banco AGREGADO por nação da C3 é dividido
			// pelos três papéis com as participações declaradas (0,45 / 0,40 /
			// 0,15) e o resíduo de arredondamento fica no estrangeiro, de modo
			// que a soma por nação é bit-exata. `deposits = 0` (a C3 não tinha
			// dinheiro bancário). Nomes entram por `set_bank_definitions`; aqui
			// só a conservação.
			const Dictionary& nations = nations_variant;
			for (const auto& row_entry : nations.entries()) {
				if (row_entry.first.empty()
						|| row_entry.second.get_type()
							!= Variant::DICTIONARY) {
					continue;
				}
				const Dictionary& row = row_entry.second;
				const double loans =
					std::max(0.0, double(row.get("loans", 0.0)));
				const double arrears =
					std::max(0.0, double(row.get("arrears", 0.0)));
				const double borrowing = std::max(0.0,
					double(row.get("bc_borrowing", 0.0)));
				const double cash =
					std::max(0.0, double(row.get("cash", 0.0)));
				const double capital =
					std::max(0.0, double(row.get("capital", 0.0)));
				const double income = std::max(0.0,
					double(row.get("interest_income", 0.0)));
				const double expense = std::max(0.0,
					double(row.get("interest_expense", 0.0)));
				const double writeoffs = std::max(0.0,
					double(row.get("writeoffs", 0.0)));
				const int defaults =
					std::max(0, int(row.get("defaults", 0)));
				const int count = std::max(0,
					int(row.get("loans_count", 0)));
				const int last_default = int(
					row.get("last_default_day", -1));
				const std::string currency = std::string(
					String(row.get("currency", "")).utf8().get_data());
				static const double kShares[3] = { 0.45, 0.40, 0.15 };
				double loan_part = 0.0;
				double arrears_part = 0.0;
				double borrowing_part = 0.0;
				double cash_part = 0.0;
				double capital_part = 0.0;
				double income_part = 0.0;
				double expense_part = 0.0;
				double writeoff_part = 0.0;
				int count_part = 0;
				for (int part = 0; part < 3; ++part) {
					const bool last = part == 2;
					const double l = last ? loans - loan_part
						: loans * kShares[part];
					const double a = last ? arrears - arrears_part
						: arrears * kShares[part];
					const double b = last ? borrowing - borrowing_part
						: borrowing * kShares[part];
					const double c = last ? cash - cash_part
						: cash * kShares[part];
					const double k = last ? capital - capital_part
						: capital * kShares[part];
					const double i = last ? income - income_part
						: income * kShares[part];
					const double e = last ? expense - expense_part
						: expense * kShares[part];
					const double w = last ? writeoffs - writeoff_part
						: writeoffs * kShares[part];
					const int n = last ? count - count_part
						: static_cast<int>(count * kShares[part]);
					loan_part += l;
					arrears_part += a;
					borrowing_part += b;
					cash_part += c;
					capital_part += k;
					income_part += i;
					expense_part += e;
					writeoff_part += w;
					count_part += n;
					Bank bank;
					bank.nation = row_entry.first;
					bank.role = part;
					bank.id = bank::bank_id(bank.nation, part);
					bank.currency = currency;
					bank.owner_kind = part == 0 ? "state"
						: (part == 1 ? "national" : "foreign");
					bank.name_status = "no_data";
					bank.share = kShares[part];
					bank.loans = l;
					bank.arrears = a;
					bank.bc_borrowing = b;
					bank.cash = c;
					bank.capital = k;
					bank.interest_income = i;
					bank.interest_expense = e;
					bank.writeoffs = w;
					bank.loans_count = n;
					bank.defaults = part == 0 ? defaults : 0;
					bank.last_default_day = part == 0 ? last_default : -1;
					ws.banks[bank.id] = bank;
				}
			}
		}
	}
}

void read_tariffs(WorldState& ws, SimCore& core, const Dictionary& data) {
	if (data.has("tariffs")) {
		const Array saved_tariffs = data.get("tariffs", Array());
		ws.tariffs.clear();
		ws.tariffs.reserve(static_cast<std::size_t>(saved_tariffs.size()));
		for (int index = 0; index < saved_tariffs.size(); ++index) {
			const Dictionary entry = saved_tariffs[index];
			Tariff tariff;
			tariff.buyer = std::string(
				String(entry.get("buyer", "")).utf8().get_data());
			if (tariff.buyer.empty()) {
				continue;
			}
			tariff.seller = std::string(
				String(entry.get("seller", "*")).utf8().get_data());
			tariff.product = std::string(
				String(entry.get("product", "*")).utf8().get_data());
			if (tariff.seller.empty()) {
				tariff.seller = "*";
			}
			if (tariff.product.empty()) {
				tariff.product = "*";
			}
			tariff.rate = std::clamp(
				double(entry.get("rate", 0.0)), 0.0, 1.0);
			ws.tariffs.push_back(std::move(tariff));
		}
		core.rebuild_tariff_lookup();
	}
}

void read_stock(WorldState& ws, const Dictionary& data) {
	// C5 (v33/D-112) - BOLSA. v32 e anteriores chegam SEM o bloco: a bolsa é
	// criada no fim da carga (`stock::after_load`), com preço = patrimônio
	// medido e a carteira do default declarado. v33 carrega o estado salvo.
	ws.stock_exchanges.clear();
	ws.stock_tickers.clear();
	ws.stock_enabled = true;
	ws.stock_investment_channel = true;
	ws.stock_risk_premium = 0.0;
	ws.stock_payout_ratio = 0.0;
	ws.stock_step = 0.0;
	ws.stock_expectation_k = 0.0;
	if (data.has("stock")) {
		const Dictionary block = data.get("stock", Dictionary());
		ws.stock_enabled = bool(block.get("enabled", true));
		const Value& policy_variant = block.get("policy", Value());
		if (policy_variant.get_type() == Variant::DICTIONARY) {
			const Dictionary params = policy_variant;
			ws.stock_investment_channel = bool(
				params.get("investment_channel", true));
			ws.stock_risk_premium = std::max(0.0,
				double(params.get("risk_premium", 0.0)));
			ws.stock_payout_ratio = std::max(0.0,
				double(params.get("payout_ratio", 0.0)));
			ws.stock_step = std::max(0.0,
				double(params.get("step", 0.0)));
			ws.stock_expectation_k = std::max(0.0,
				double(params.get("expectation_k", 0.0)));
		}
		const Value& exchanges_variant = block.get("exchanges", Value());
		if (exchanges_variant.get_type() == Variant::DICTIONARY) {
			const Dictionary& exchanges = exchanges_variant;
			for (const auto& row_entry : exchanges.entries()) {
				if (row_entry.first.empty() || row_entry.second.get_type()
						!= Variant::DICTIONARY) {
					continue;
				}
				const Dictionary& row = row_entry.second;
				StockExchange exchange;
				exchange.nation = std::string(
					String(row.get("nation", "")).utf8().get_data());
				if (exchange.nation.empty()) {
					exchange.nation = row_entry.first;
				}
				exchange.name = std::string(
					String(row.get("name", "")).utf8().get_data());
				exchange.name_status = std::string(
					String(row.get("name_status", "game_default"))
						.utf8().get_data());
				exchange.currency = std::string(
					String(row.get("currency", "")).utf8().get_data());
				exchange.confidence = std::clamp(
					double(row.get("confidence", 1.0)), 0.65, 1.35);
				exchange.index_base = std::max(0.0,
					double(row.get("index_base", 0.0)));
				exchange.last_price_day = int(
					row.get("last_price_day", -1));
				// Medição que afeta a trajetória (EMA da produção da nação):
				// vai ao save para a continuação ser idêntica ao caminho direto.
				exchange.production_ema = std::max(0.0,
					double(row.get("production_ema", 0.0)));
				exchange.production_seeded = bool(
					row.get("production_seeded", false));
				exchange.dividends_paid = std::max(0.0,
					double(row.get("dividends_paid", 0.0)));
				exchange.remittance = std::max(0.0,
					double(row.get("remittance", 0.0)));
				exchange.remittance_origin = std::max(0.0,
					double(row.get("remittance_origin", 0.0)));
				ws.stock_exchanges[exchange.nation] = exchange;
			}
		}
		const Value& tickers_variant = block.get("tickers", Value());
		if (tickers_variant.get_type() == Variant::DICTIONARY) {
			const Dictionary& tickers = tickers_variant;
			for (const auto& row_entry : tickers.entries()) {
				if (row_entry.first.empty() || row_entry.second.get_type()
						!= Variant::DICTIONARY) {
					continue;
				}
				const Dictionary& row = row_entry.second;
				StockTicker ticker;
				ticker.group_id = std::string(
					String(row.get("group_id", "")).utf8().get_data());
				if (ticker.group_id.empty()) {
					ticker.group_id = row_entry.first;
				}
				ticker.nation = std::string(
					String(row.get("nation", "")).utf8().get_data());
				ticker.exchange = std::string(
					String(row.get("exchange", "")).utf8().get_data());
				if (ticker.exchange.empty()) {
					ticker.exchange = ticker.nation;
				}
				ticker.shares = std::max(0.0,
					double(row.get("shares", 0.0)));
				ticker.price = std::max(0.0, double(row.get("price", 0.0)));
				ticker.book_equity = std::max(0.0,
					double(row.get("book_equity", 0.0)));
				ticker.profit_ema = double(row.get("profit_ema", 0.0));
				ticker.profit_at_last_dividend = double(
					row.get("profit_at_last_dividend", 0.0));
				ticker.dividend_last = std::max(0.0,
					double(row.get("dividend_last", 0.0)));
				ticker.dividend_limited = std::max(0.0,
					double(row.get("dividend_limited", 0.0)));
				ticker.dividend_day = int(row.get("dividend_day", -1));
				ticker.listed_day = int(row.get("listed_day", 0));
				ticker.delisted_day = int(row.get("delisted_day", -1));
				ticker.listed = bool(row.get("listed", false));
				ticker.reason = std::string(
					String(row.get("reason", "listed")).utf8().get_data());
				const Value& holdings_variant = row.get("holdings", Value());
				if (holdings_variant.get_type() == Variant::DICTIONARY) {
					const Dictionary& holdings = holdings_variant;
					ticker.holdings.households_high = std::max(0.0,
						double(holdings.get("households_high", 0.0)));
					const Value& banks_variant = holdings.get("banks", Value());
					if (banks_variant.get_type() == Variant::DICTIONARY) {
						const Dictionary& bank_map = banks_variant;
						for (const auto& bank_row : bank_map.entries()) {
							ticker.holdings.banks[bank_row.first] =
								std::max(0.0, double(bank_row.second));
						}
					}
					const Value& groups_variant = holdings.get("groups",
						Value());
					if (groups_variant.get_type() == Variant::DICTIONARY) {
						const Dictionary& group_map = groups_variant;
						for (const auto& group_row : group_map.entries()) {
							ticker.holdings.groups[group_row.first] =
								std::max(0.0, double(group_row.second));
						}
					}
					ticker.holdings.foreign = std::max(0.0,
						double(holdings.get("foreign", 0.0)));
				}
				ws.stock_tickers[ticker.group_id] = ticker;
			}
		}
	}
}

void read_bank_crisis(WorldState& ws, const Dictionary& data) {
	// C6 (v34/D-114) - CRISE BANCARIA. v33 e anteriores chegam SEM o bloco:
	// `bank_crisis::after_load` inicia os bancos calmos (confianca 1,0), fundo
	// 0 e nenhum evento. O policy salvo e' regra (0 = default declarado).
	ws.bank_crisis = BankCrisisPolicy{};
	ws.deposit_insurance.clear();
	ws.bank_crisis_events.clear();
	ws.bank_crisis_events_total = 0;
	if (data.has("bank_crisis")) {
		const Dictionary block = data.get("bank_crisis", Dictionary());
		ws.bank_crisis.enabled = bool(block.get("enabled", true));
		const Value& policy_variant = block.get("policy", Value());
		if (policy_variant.get_type() == Variant::DICTIONARY) {
			const Dictionary params = policy_variant;
			BankCrisisPolicy& pol = ws.bank_crisis;
			pol.capital_min = std::max(0.0,
				double(params.get("capital_min", 0.0)));
			pol.capital_cut_floor = std::clamp(
				double(params.get("capital_cut_floor", 0.0)), 0.0, 1.0);
			pol.liquidity_min = std::max(0.0,
				double(params.get("liquidity_min", 0.0)));
			pol.loss_scale = std::max(0.0,
				double(params.get("loss_scale", 0.0)));
			pol.loss_ema_days = std::max(0.0,
				double(params.get("loss_ema_days", 0.0)));
			pol.confidence_start = std::max(0.0,
				double(params.get("confidence_start", 0.0)));
			pol.smoothing = std::clamp(
				double(params.get("smoothing", 0.0)), 0.0, 1.0);
			pol.w_solvency = std::max(0.0,
				double(params.get("w_solvency", 0.0)));
			pol.w_liquidity = std::max(0.0,
				double(params.get("w_liquidity", 0.0)));
			pol.w_losses = std::max(0.0,
				double(params.get("w_losses", 0.0)));
			pol.w_contagion = std::max(0.0,
				double(params.get("w_contagion", 0.0)));
			pol.w_market = std::max(0.0,
				double(params.get("w_market", 0.0)));
			pol.run_threshold = std::max(0.0,
				double(params.get("run_threshold", 0.0)));
			pol.run_share = std::clamp(
				double(params.get("run_share", 0.0)), 0.0, 1.0);
			pol.flight_to_quality = std::clamp(
				double(params.get("flight_to_quality", 0.0)), 0.0, 1.0);
			pol.lolr_spread = std::max(0.0,
				double(params.get("lolr_spread", 0.0)));
			pol.lolr_haircut = std::clamp(
				double(params.get("lolr_haircut", 0.0)), 0.0, 1.0);
			pol.lolr_max_assets = std::clamp(
				double(params.get("lolr_max_assets", 0.0)), 0.0, 1.0);
			pol.insured_share = std::clamp(
				double(params.get("insured_share", 0.0)), 0.0, 1.0);
			pol.premium_daily = std::max(0.0,
				double(params.get("premium_daily", 0.0)));
			pol.fund_target_ratio = std::max(0.0,
				double(params.get("fund_target_ratio", 0.0)));
			pol.capital_flight_share = std::clamp(
				double(params.get("capital_flight_share", 0.0)),
				0.0, 1.0);
			pol.insolvency_tolerance = std::max(0.0,
				double(params.get("insolvency_tolerance", 0.0)));
			pol.insolvency_tolerance_ratio = std::max(0.0,
				double(params.get("insolvency_tolerance_ratio", 0.0)));
			pol.treasury_debt_rescue = bool(
				params.get("treasury_debt_rescue", false));
			pol.takeover_public = bool(
				params.get("takeover_public", true));
		}
		const Value& funds_variant = block.get("funds", Value());
		if (funds_variant.get_type() == Variant::DICTIONARY) {
			const Dictionary& funds = funds_variant;
			for (const auto& row_entry : funds.entries()) {
				if (row_entry.first.empty()
						|| row_entry.second.get_type()
							!= Variant::DICTIONARY) {
					continue;
				}
				const Dictionary& row = row_entry.second;
				DepositInsurance fund;
				fund.fund = std::max(0.0,
					double(row.get("fund", 0.0)));
				fund.premiums = std::max(0.0,
					double(row.get("premiums", 0.0)));
				fund.payouts = std::max(0.0,
					double(row.get("payouts", 0.0)));
				fund.last_payout_day = int(
					row.get("last_payout_day", -1));
				ws.deposit_insurance[std::string(row_entry.first)] = fund;
			}
		}
		const Array events = block.get("events", Array());
		for (int index = 0; index < events.size(); ++index) {
			const Dictionary row = events[index];
			BankCrisisEvent event;
			event.day = int(row.get("day", -1));
			event.bank = std::string(
				String(row.get("bank", "")).utf8().get_data());
			event.nation = std::string(
				String(row.get("nation", "")).utf8().get_data());
			event.kind = std::string(
				String(row.get("kind", "")).utf8().get_data());
			event.cause = std::string(
				String(row.get("cause", "")).utf8().get_data());
			event.gap = double(row.get("gap", 0.0));
			event.cost_shareholders = double(
				row.get("cost_shareholders", 0.0));
			event.cost_depositors = double(
				row.get("cost_depositors", 0.0));
			event.cost_fund = double(row.get("cost_fund", 0.0));
			event.cost_treasury = double(
				row.get("cost_treasury", 0.0));
			event.cost_takeover = double(
				row.get("cost_takeover", 0.0));
			event.capital_flight = double(
				row.get("capital_flight", 0.0));
			event.lolr = double(row.get("lolr", 0.0));
			event.emission = double(row.get("emission", 0.0));
			event.buyer = std::string(
				String(row.get("buyer", "")).utf8().get_data());
			event.auction_book = double(
				row.get("auction_book", 0.0));
			event.auction_price = double(
				row.get("auction_price", 0.0));
			event.charter_cost = double(
				row.get("charter_cost", 0.0));
			event.charter_cap = double(
				row.get("charter_cap", 0.0));
			event.austerity = double(row.get("austerity", 0.0));
			event.desert = bool(row.get("desert", false));
			event.recapitalized = bool(
				row.get("recapitalized", false));
			ws.bank_crisis_events.push_back(event);
		}
		// O anel publicado tem tamanho fixo: um save adulterado (ou com mais
		// de kEventRing eventos) não pode inchar o status. Fica o mais recente,
		// como em `register_event`; `events_total` guarda o total.
		if (static_cast<int>(ws.bank_crisis_events.size())
				> bank_crisis::kEventRing) {
			const auto first = ws.bank_crisis_events.end()
				- bank_crisis::kEventRing;
			ws.bank_crisis_events.erase(
				ws.bank_crisis_events.begin(), first);
		}
		ws.bank_crisis_events_total = int(
			block.get("events_total", 0));
	}
}

} // namespace finance

} // namespace ogs::save
