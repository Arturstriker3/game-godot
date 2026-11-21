#pragma once
// C2b1 — EMISSÃO DE MOEDA E JUROS DO BANCO CENTRAL (D-104).
// Doc: docs/economy/C2b1_monetary_policy.md.
//
// Banco central POR MOEDA (a mesma entidade da C2a/D-103). Nesta fase ele:
//   1. fixa a TAXA BÁSICA — independente: regra tipo Taylor sobre a inflação
//      de 12 meses do CPI da C2a (ou a anualizada antes dos 12 meses) e o
//      hiato medido pelo desemprego; controlado: juro abaixo do neutro;
//   2. EMITE M0 apenas por operação contabilizada no balanço da C1: compra de
//      títulos do governo (financiamento do déficit medido). O dinheiro criado
//      entra no orçamento público e a dívida pública (títulos do BC) nasce do
//      mesmo valor — a identidade financeira continua fechando;
//   3. alimenta o canal de transmissão MÍNIMO: juro no crédito da safra
//      (D-099, em labor.cpp) e custo de capital no silo privado (G2, ai_storage).
//
// O que NÃO existe nesta fase (declarado): bancos e redesconto (C4), venda de
// títulos e dívida pública anterior (C3), intervenção cambial e banda, padrão
// ouro/Bretton Woods agindo, ouro físico negociado.
//
// Nenhum número de país é inventado: os parâmetros abaixo são DEFAULT DE JOGO,
// declarados em data/definitions/economy/monetary_policy.json (fonte
// "default de jogo") e conferidos pelo teste `c2b1_monetary_test`.

#include "world_state.h"

#include <map>
#include <string>
#include <vector>

namespace ogs {
namespace central_bank {
struct CpiRow;
}

namespace monetary {

// --- Defaults de jogo (espelho de data/definitions/economy/monetary_policy.json)
// D-113 (calibração pré-C6): a meta e o neutro estavam INCOMPATÍVEIS com o
// regime de preços deste modelo. O CPI da C2a é um índice de NÍVEL relativo aos
// preços-base (nenhum bem carrega tendência inflacionária própria): medido em
// 365 d no mundo default, a inflação de 12 meses tem mediana 1,55 %
// (p10 −0,23 %, p90 4,85 %; docs/economy/perf/c5_gate_default.json). Com meta
// 3 % o termo de inflação ficava permanentemente negativo (−2,2 pp) e a política
// vivia em viés expansionista: a taxa mediana do 1º ano caía de ~10 % para 0,3 %.
// A meta passa a ser 1,5 % (declarada, calibrada à tendência MEDIDA do CPI; não
// é meta de banco central real — o repositório não tem essa fonte, ver C2a) e o
// neutro 1 % real + meta = 2,5 %.
constexpr double kTargetInflation = 0.015;      // meta: 1,5 % a.a. (D-113)
constexpr double kNeutralRate = 0.025;          // taxa real neutra 1 % + meta
constexpr double kTaylorInflationWeight = 1.5;  // resposta à inflação
constexpr double kTaylorGapWeight = 0.5;        // resposta ao hiato (pp)
constexpr double kMaxRateStepPerDay = 0.0025;   // 25 pb por dia
// D-113: `kRateSmoothing` é por DECISÃO (cadência da leitura do CPI = 1 mês).
// Aplicado por dia, 0,5 dava meia-vida de 1 dia e a taxa teleportava para o
// alvo em ~4 dias; `next_rate` converte para o equivalente diário
// (1 − (1 − 0,5)^(1/30) ≈ 2,28 %/dia), declarado em sim/central_bank.h.
constexpr double kRateSmoothing = 0.5;          // metade do caminho por mês
constexpr double kRateFloor = 0.0;
constexpr double kRateCap = 0.30;
constexpr double kControlledSpread = 0.02;      // controlado: neutro − 2 pp
constexpr double kControlledEmissionShareOfM0 = 0.01;  // senhoriagem ≤1 %/dia
// D-113: o hiato do produto NÃO entra antes do baseline do mercado de trabalho.
// No dia 0 o modelo ainda está alocando a força de trabalho (desemprego medido
// 21,5 % no dia 7 contra 6,1 % no regime): o hiato saía −17,5 pp e a taxa caía
// de 4 % para 0,25 % em 15 dias por um artefato de inicialização. Até o
// primeiro fechamento mensal a leitura é AUSENTE (declarada, nada é completado).
constexpr int kGapBaselineDay = 30;
constexpr int kIndependenceDefault =
	1;  // central_bank::kIndependenceIndependent (default declarado)

// Independência EFETIVA de uma moeda: o valor salvo quando há política
// escolhida; `no_data` (a ausência declarada da C2a) resolve no default de jogo.
int effective_independence(const WorldState& ws, const std::string& code);

// Taxa básica EFETIVA (anual, fração). Sem decisão ainda (`rate < 0`) ou sem
// entrada do estado, devolve o neutro declarado — nunca um número inventado.
double effective_rate(const WorldState& ws, const std::string& code);
double rate_for_nation(const WorldState& ws, const std::string& nation);

// Leitura que a regra usa. Ausência de inflação é declarada (`has_inflation`).
struct Reading {
	bool has_inflation = false;
	double inflation = 0.0;  // variação de 12 meses (ou anualizada)
	bool annualized = false; // true = menos de 12 meses de série
	double gap = 0.0;        // (desemprego natural − medido) em pp
	double weight = 0.0;     // população de simulação que sustentou a leitura
};

Reading reading(const WorldState& ws, const std::string& code,
	const std::map<std::string, central_bank::CpiRow>& cpi);
// D-113: existe leitura de hiato para esta moeda? Falso antes do baseline do
// mercado de trabalho (`kGapBaselineDay`) ou quando nenhuma cidade da moeda tem
// força de trabalho. O status publica `has_gap` para separar "hiato ausente" de
// "hiato zero" — a regra não completa o número.
bool has_gap_reading(const WorldState& ws, const std::string& code);

// Regra tipo Taylor (fração anual). Sem inflação o termo não entra.
double taylor_target(const Reading& in);
// Passo limitado + suavização + piso/teto.
double next_rate(double current, double target);

// Dia da política monetária: atualiza a taxa de cada moeda e, para moeda
// controlada, monetiza o déficit MEDIDO do dia (variação negativa do caixa do
// governo por nação) até o limite de senhoriagem. `money_total` é a contagem
// canônica do dia ANTES da emissão (base do limite de 1 % do M0).
void tick(WorldState& ws, double money_total,
	const std::vector<double>& base_prices);

} // namespace monetary
} // namespace ogs
