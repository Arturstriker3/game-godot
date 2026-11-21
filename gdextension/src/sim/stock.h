#pragma once
// C5 (D-112) — BOLSA DE VALORES POR PAÍS, CONFIANÇA E EXPECTATIVAS.
// Desenho e contrato: docs/economy/C5_stock_exchange.md.
//
// Uma bolsa por nação (nome genérico/fictício, `name_status = game_default`),
// sem livro de ordens. A ação é um ativo financeiro do detentor e um claim do
// emissor (o grupo): `Σ carteiras = Σ market cap`. Valorização NÃO cria
// dinheiro — é preço de ativo; só o dividendo move caixa (soma zero).
//
//   preço     = fundamental × confiança, com passo convergente semanal
//   fundamental = payout × max(0, E[lucro]) / (taxa básica + prêmio) / ações
//   confiança = expectativa adaptativa dos sinais MEDIDOS da nação
//
// Esta fase não tem crash/corrida (C6), IPO nem regulação (depois da C5) e a
// carteira só muda por dividendo — tudo declarado no status.

#include "world_state.h"

#include <string>
#include <vector>

namespace ogs {

namespace balance {
struct GroupSheet;
}

namespace stock {

// --- Defaults de jogo declarados (§5/§6 do doc; nenhum número observado).
constexpr double kRiskPremiumDefault = 0.04;   // prêmio de risco sobre a básica
constexpr double kPayoutRatioDefault = 0.5;    // metade do lucro medido
constexpr double kStepDefault = 0.35;          // passo semanal convergente
constexpr double kExpectationKDefault = 0.05;  // expectativa adaptativa diária
constexpr double kPriceFloorRate = 0.01;       // piso do denominador
constexpr double kSharesDefault = 1000000.0;   // granularidade do papel
constexpr int kPricePeriodDays = 7;            // fechamento semanal
constexpr int kDividendPeriodDays = 30;        // fechamento mensal
constexpr double kExpectationDefault = 1.0;
// C5-fix (D-112) - AQUECIMENTO E REFERENCIA DE LUCRO.
// O lucro de referência do papel NÃO nasce de um dia: é semeado com a MÉDIA
// MEDIDA desde o início do mundo e, nos primeiros 63 dias, converge ao nível
// recente por EMA (`kProfitWarmupK` = 0,05, meia-vida ~14 d); o preço não reage
// nesse aquecimento e o índice da praça só é publicado (base 100) no primeiro
// fechamento semanal depois dele. Depois disso a referência vira janela anual
// (`kProfitEmaK` = 1/365 por dia).
constexpr int kIndexWarmupDays = 63;           // 9 semanas de aquecimento
// Durante o aquecimento a referência converge ao NÍVEL RECENTE do lucro
// (EMA de ~20 dias, partindo da média medida) e o preço não reage: no fim
// dela a referência não é mais um dia nem a média de um período de rampa.
constexpr double kProfitWarmupK = 0.05;
// Depois do aquecimento a referência é uma EMA de janela ANUAL (1/365 por dia
// = TTM): expectativa de lucro não é o último mês. Uma mudança permanente de
// nível é reconhecida com constante de tempo de ~1 ano (declarado).
constexpr double kProfitEmaK = 1.0 / 365.0;
// Banda de P/L declarada (default de jogo). Fora dela a valoração não é
// travada pelo arredondamento da taxa: o múltiplo implícito é
// `payout / (taxa básica + prêmio)` e fica limitado a esta faixa.
constexpr double kPeFloor = 3.0;
constexpr double kPeCeiling = 30.0;
// C5-fix (D-112) - TAXA DESCONTADA. O preço desconta a taxa ESPERADA, não a
// taxa do dia: a expectativa fica no neutro (`monetary::kNeutralRate`) e anda
// um quarto do desvio da taxa básica corrente. Sem isso, a normalização da
// taxa básica no 1º ano (10 % -> neutro) re-avaliava o mercado inteiro em 3x
// sem nenhuma mudança de lucro (medição em docs/economy/C5_stock_exchange.md
// §12). Default de jogo, declarado.
constexpr double kRatePassThrough = 0.25;
// Carteira inicial declarada (§4): 55 % famílias do estrato alto, 20 % bancos,
// 15 % grupos da mesma nação, 10 % exterior (sem banco estrangeiro, a fatia do
// exterior vai para as famílias e a soma continua 100 %).
constexpr double kHoldHouseholds = 0.55;
constexpr double kHoldBanks = 0.20;
constexpr double kHoldGroups = 0.15;
constexpr double kHoldForeign = 0.10;
// Pesos do índice de confiança (§6), soma 0,35.
constexpr double kWeightInflation = 0.10;
constexpr double kWeightRate = 0.05;
constexpr double kWeightProduction = 0.08;
constexpr double kWeightBankruptcies = 0.05;
constexpr double kWeightDevaluations = 0.04;
constexpr double kWeightUnemployment = 0.03;
// Escalas declaradas dos sinais.
constexpr double kInflationScale = 0.10;     // ±10 p.p. em torno da meta
constexpr double kRateScale = 0.05;          // ±5 p.p. em torno do neutro
constexpr double kProductionScale = 0.10;    // ±10 % ao ano
constexpr double kBankruptcyScale = 0.02;    // 2 % das construções falidas
constexpr double kDevaluationScale = 3.0;    // 3 eventos em 365 d
constexpr double kUnemploymentScale = 0.04;  // 4 p.p. em torno do natural
// Canal de investimento (q de Tobin), limitado para não virar multiplicador
// de investimento sem calibração (§7).
constexpr double kQFactorFloor = 0.8;
constexpr double kQFactorCeiling = 1.2;

struct Policy {
	bool enabled = true;
	bool investment_channel = true;
	double risk_premium = kRiskPremiumDefault;
	double payout_ratio = kPayoutRatioDefault;
	double step = kStepDefault;
	double expectation_k = kExpectationKDefault;
	double shares = kSharesDefault;
	int dividend_period_days = kDividendPeriodDays;
};
// Política efetiva: estado salvo (ligado/desligado) + parâmetros de regra
// (0 = default declarado aqui).
Policy policy(const WorldState& ws);

// --- Elegibilidade (§3): grupo não estatal, com origem e receita medida.
bool eligible(const Group& group, std::string* reason);

// --- Garantia das linhas (idempotente): cria bolsa da nação e o papel do
// grupo. `seed` usa o patrimônio MEDIDO do grupo por ação (nunca preço
// inventado) e a carteira do default declarado.
void seed(WorldState& ws);
StockExchange& ensure_exchange(WorldState& ws, const std::string& nation);
StockTicker* find_ticker(WorldState& ws, const std::string& group_id);
const StockTicker* find_ticker(const WorldState& ws, const std::string& group_id);

// Patrimônio medido do grupo (mesma conta da C3: `GroupSheet::net_worth`).
double book_equity(const balance::GroupSheet& sheet);

// Preço fundamental do papel (uma única fórmula para o núcleo e o status):
//   E[lucro anual] × clamp(payout / (taxa + prêmio), kPeFloor, kPeCeiling) / ações
double fundamental_price(const WorldState& ws, const StockTicker& ticker,
	const Policy& pol);

// Market cap e índice (base 100 no dia de formação da bolsa).
double market_cap(const StockTicker& ticker);
double index_of(const WorldState& ws, const StockExchange& exchange);

// Sinais medidos da nação + confiança (expectativa adaptativa).
struct Signals {
	bool has_inflation = false;
	double inflation = 0.0;
	bool has_rate = false;
	double rate = 0.0;
	bool has_production = false;
	double production_growth = 0.0;
	bool has_bankruptcies = false;
	double bankruptcy_share = 0.0;
	bool has_devaluations = false;
	int devaluations = 0;
	bool has_unemployment = false;
	double unemployment = 0.0;
	std::vector<std::string> missing;
	double raw = kExpectationDefault;
};
Signals signals(const WorldState& ws, const StockExchange& exchange);
// Atualiza `confidence` (uma passada por dia). Devolve o sinal bruto.
double update_confidence(WorldState& ws, StockExchange& exchange);

// Fase do dia: lucro esperado, confiança, preço semanal e dividendo mensal.
void phase(WorldState& ws);
// Divisão do lucro do período entre os detentores (conservada). Devolve o
// total pago na moeda da bolsa.
double pay_dividends(WorldState& ws);

// Canal de investimento: fator do score do grupo (1 fora da bolsa).
double investment_factor(const WorldState& ws, const std::string& group_id);

struct Totals {
	double market_cap = 0.0;
	double book_equity = 0.0;
	double dividends_paid = 0.0;
	double remittance = 0.0;
	double remittance_origin = 0.0;
	double holdings = 0.0;
	double index_global = 100.0;
	int listed = 0;
	int unlisted = 0;
	int exchanges = 0;
};
Totals totals(const WorldState& ws);

// Save v33: reforço depois da carga. v32 e anteriores recebem a bolsa criada
// com preço = patrimônio medido (migração); v33+ só garante novas linhas.
void after_load(WorldState& ws, int version);
// `initialize_world` troca os grupos: a bolsa é zerada e nasce no dia 1.
void reset(WorldState& ws);

} // namespace stock
} // namespace ogs
