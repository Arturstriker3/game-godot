#pragma once
// C6 (D-114) — CRISE BANCARIA, CORRIDA E RESOLUCAO.
// Contrato e medição: docs/economy/C6_bank_crisis.md.
// Parametros: data/definitions/economy/bank_crisis.json (default de jogo).
//
// A C4 deu dono aos bancos e dinheiro bancario (deposito), mas declarou
// `resolution: "C6"`: o patrimonio podia ficar negativo e ninguem socorria o
// banco. A C6 fecha isso com cinco pecas, todas conservando dinheiro:
//
//   1. INDICADORES por banco/dia: patrimonio/ativos (indice de capital),
//      caixa/depositos (liquidez) e inadimplencia da carteira (atrasados /
//      carteira). Medidos e publicados; os minimos sao default de jogo.
//   2. CONFIANCA do depositante (0..1), suavizada por dia, funcao de
//      solvencia, liquidez, perdas recentes (EMA), contagio dos OUTROS bancos
//      da mesma nacao e confianca da bolsa (C5), quando existir.
//   3. CORRIDA: abaixo do limiar, uma fracao declarada dos depositos sai por
//      dia e vira papel-moeda (reserva liberada: M0 tem a mesma soma, muda a
//      composicao), deposito do banco publico (fuga para qualidade) ou remessa
//      da matriz estrangeira (fuga de capital pela posicao do exterior).
//   4. EMPRESTADOR DE ULTIMA INSTANCIA: o BC da moeda redesconta contra o
//      colateral com taxa basica + spread, criando M0 e registrando a divida
//      do banco (`bc_borrowing`). Moeda de uniao usa o BC da uniao;
//      dolarizado/sem moeda propria NAO tem emprestador (so o governo).
//   5. APERTO E RESOLUCAO: abaixo do capital minimo o banco corta credito novo
//      proporcionalmente; patrimonio < 0 entra na hierarquia acionista ->
//      deposito nao segurado -> seguro de deposito -> Tesouro/divida publica ->
//      incorporacao pelo banco publico. Cada custo fica registrado por parte e
//      os custos que fecham o rombo somam exatamente o rombo.
//
// Nenhuma valoracao cria dinheiro: o fundo de seguro entra no `money_total`,
// os premios saem do caixa do banco, o resgate do Tesouro e' transferencia (ou
// emissao registrada no BC) e o corte de deposito destroi dinheiro bancario
// junto com o caixa do depositante (a variacao dos depositos entra no
// `bank_money_today`, como o credito da C4).

#include "world_state.h"

#include <string>
#include <vector>

namespace ogs {

namespace bank_crisis {

// --- Defaults de jogo (espelho de data/definitions/economy/bank_crisis.json).
// O mundo base da C4 tem patrimonio ~0,4 % dos ativos e caixa ~0; por isso o
// piso de capital e' 0,1 % (do MODELO, nao Basileia) e o minimo de liquidez
// nasce 0 (o banco da C4 nao guarda reserva). Ver o `source_note` do JSON.
constexpr double kCapitalMinDefault = 0.001;
constexpr double kCapitalCutFloorDefault = 0.0;
// RESERVADO: nenhuma regra consome o mínimo de liquidez (o sinal de confiança
// usa a referência do próprio banco). Publicado no JSON/política para não
// prometer um efeito que o núcleo não tem.
constexpr double kLiquidityMinDefault = 0.0;
constexpr double kLossScaleDefault = 0.02;
constexpr double kLossEmaDaysDefault = 10.0;
constexpr double kConfidenceStartDefault = 1.0;
constexpr double kConfidenceSmoothingDefault = 0.12;
constexpr double kWeightSolvencyDefault = 0.30;
constexpr double kWeightLiquidityDefault = 0.10;
constexpr double kWeightLossesDefault = 0.40;
constexpr double kWeightContagionDefault = 0.15;
constexpr double kWeightMarketDefault = 0.05;
constexpr double kRunThresholdDefault = 0.35;
constexpr double kRunShareDefault = 0.08;
constexpr double kFlightToQualityDefault = 0.45;
constexpr double kLolrSpreadDefault = 0.03;
constexpr double kLolrHaircutDefault = 0.70;
constexpr double kLolrMaxAssetsDefault = 0.50;
constexpr double kInsuredShareDefault = 0.60;
constexpr double kPremiumDailyDefault = 0.00005;
constexpr double kFundTargetRatioDefault = 0.01;
constexpr double kCapitalFlightShareDefault = 0.25;
// Materialidade da insolvencia (D-114): um patrimonio negativo de fracao de
// unidade e' arredondamento do juro/dividendo, nao um rombo. Abaixo desta
// tolerancia o banco nao entra na hierarquia de resolucao (fica calmo e
// monitorado). Declarado como default de jogo; nao e' dado observado.
constexpr double kInsolvencyToleranceDefault = 1.0;
// Piso RELATIVO aos ativos (1 parte por milhao). A tolerancia efetiva e'
// max(absoluta, ativos x razao): o ruido de juro acompanha o tamanho da
// carteira, e o rombo medido no mundo base ia a 1,68 XGS num banco de milhoes.
constexpr double kInsolvencyToleranceRatioDefault = 0.000001;
// Anel de eventos publicado no status (o total fica no WorldState).
constexpr int kEventRing = 64;
constexpr double kDaysPerYear = 365.0;

// --- Politica efetiva (estado salvo + regras; 0 = default acima).
struct Policy {
	bool enabled = true;
	double capital_min = kCapitalMinDefault;
	double capital_cut_floor = kCapitalCutFloorDefault;
	double liquidity_min = kLiquidityMinDefault;  // reservado (sem regra)
	double loss_scale = kLossScaleDefault;
	double loss_ema_days = kLossEmaDaysDefault;
	double confidence_start = kConfidenceStartDefault;
	double smoothing = kConfidenceSmoothingDefault;
	double w_solvency = kWeightSolvencyDefault;
	double w_liquidity = kWeightLiquidityDefault;
	double w_losses = kWeightLossesDefault;
	double w_contagion = kWeightContagionDefault;
	double w_market = kWeightMarketDefault;
	double run_threshold = kRunThresholdDefault;
	double run_share = kRunShareDefault;
	double flight_to_quality = kFlightToQualityDefault;
	double lolr_spread = kLolrSpreadDefault;
	double lolr_haircut = kLolrHaircutDefault;
	double lolr_max_assets = kLolrMaxAssetsDefault;
	double insured_share = kInsuredShareDefault;
	double premium_daily = kPremiumDailyDefault;
	double fund_target_ratio = kFundTargetRatioDefault;
	double capital_flight_share = kCapitalFlightShareDefault;
	double insolvency_tolerance = kInsolvencyToleranceDefault;
	double insolvency_tolerance_ratio = kInsolvencyToleranceRatioDefault;
	bool treasury_debt_rescue = true;
	bool takeover_public = true;
};
Policy policy(const WorldState& ws);

// --- Indicadores medidos de um banco (0 quando a base e' 0).
struct Indicators {
	double assets = 0.0;       // caixa + carteira
	double loans = 0.0;
	double arrears = 0.0;
	double deposits = 0.0;
	double cash = 0.0;
	double capital = 0.0;
	double equity = 0.0;
	double capital_ratio = 0.0;   // patrimonio / ativos
	double liquidity_ratio = 0.0; // caixa / depositos
	double npl_ratio = 0.0;       // atrasados / carteira
};
Indicators indicators(const Bank& bank);

// Fator de aperto do credito novo: 1 acima do capital minimo, cai
// proporcionalmente ate `capital_cut_floor` abaixo dele (0 = sem credito).
double credit_factor(const WorldState& ws, const Bank& bank);

// Existe banco central para a moeda da nacao? Dolarizado/sem moeda propria
// (`fx_no_policy_nations`, IMF AREAER) NAO tem emprestador de ultima instancia;
// moeda de uniao (EUR, XOF...) usa o BC da uniao. Sem moeda no estado = nao.
bool has_lender(const WorldState& ws, const std::string& nation);

// --- Choque de perda de credito (forcado em teste/estresse): baixa `amount`
// da carteira do banco e da divida dos grupos tomadores na mesma proporcao,
// SEM mover dinheiro (e' perda de ativo reconhecida). A perda entra na EMA de
// confianca do banco e em `writeoffs`. Devolve o valor efetivamente baixado.
double apply_loss(WorldState& ws, const std::string& bank_id, double amount);
// Mesmo choque com valor = `share` da carteira do banco (ex.: 0.30 = 30 %).
double apply_loss_share(WorldState& ws, const std::string& bank_id,
	double share);

// --- Fase do dia: premio do seguro, indicadores, confianca, corrida,
// redesconto, aperto e resolucao. Roda DEPOIS da bolsa (a confianca da C5
// entra no sinal) e ANTES dos invariantes. `m0_total` (opcional) e' a base
// monetaria do dia para a valvula de 1 %/dia do R2c; negativo = ausente
// (valvula sem teto lido nao emite).
void phase(WorldState& ws, double m0_total = -1.0);

// Resolucao de UM banco insolvente (chamada pela fase; exposta para teste).
// Devolve o rombo reconhecido (0 = nao estava insolvente). Com
// `banks.resolution` o caminho e' o do R2c (leilao/bail-in/fundo/charter ou
// cascata do publico); sem ele, o C6 legado bit a bit.
double resolve(WorldState& ws, const std::string& bank_id,
	const std::string& cause = "insolvency", double m0_total = -1.0);

// Evento no anel (o total sempre incrementa).
void register_event(WorldState& ws, const BankCrisisEvent& event);

// `initialize_world`: zera o estado de crise (fundos e eventos) sem inventar.
void reset(WorldState& ws);
// Save v33 e anteriores: bancos sem estado de crise iniciam calmos (confianca
// 1,0, resolucao nenhuma, fundo 0). v34+ carrega o estado salvo.
void after_load(WorldState& ws, int version);

// Totais publicados no status.
struct Totals {
	double confidence_sum = 0.0;
	double confidence_min = 1.0;
	double confidence_max = 0.0;
	// A remessa da matriz estrangeira é medida em `Bank::capital_flight` (não
	// há cópia separada): `run_foreign` foi removido para não apelidar o mesmo
	// valor duas vezes no contrato.
	double capital_flight = 0.0;
	double run_withdrawn = 0.0;
	double run_cash = 0.0;
	double run_public = 0.0;
	double lolr_borrowed = 0.0;
	double lolr_interest = 0.0;
	double insured_payout = 0.0;
	double bailin_cut = 0.0;
	double treasury_rescue = 0.0;
	double fund_total = 0.0;
	double premiums = 0.0;
	double payouts = 0.0;
	int runs = 0;          // bancos em corrida hoje
	int run_days = 0;      // dias de corrida CORRENTES somados (zera ao sair)
	// Conta o dia da resolução de TODO banco, inclusive a casca absorvida pelo
	// banco público (a casca não tem saldo, mas a resolução aconteceu).
	int resolutions = 0;
	int resolved_markets = 0;  // resolvidos que ainda têm linha com posição
	int events_total = 0;
	int banks = 0;
};
Totals totals(const WorldState& ws);

} // namespace bank_crisis
} // namespace ogs
