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
constexpr double kTargetInflation = 0.03;       // meta: 3 % a.a.
constexpr double kNeutralRate = 0.04;           // taxa real neutra 1 % + meta
constexpr double kTaylorInflationWeight = 1.5;  // resposta à inflação
constexpr double kTaylorGapWeight = 0.5;        // resposta ao hiato (pp)
constexpr double kMaxRateStepPerDay = 0.0025;   // 25 pb por dia
constexpr double kRateSmoothing = 0.5;          // metade do caminho por dia
constexpr double kRateFloor = 0.0;
constexpr double kRateCap = 0.30;
constexpr double kControlledSpread = 0.02;      // controlado: neutro − 2 pp
constexpr double kControlledEmissionShareOfM0 = 0.01;  // senhoriagem ≤1 %/dia
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
