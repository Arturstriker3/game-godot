#pragma once
// R1b - Tesouro Nacional + envelopes mensais (flag `rework.national_treasury`).
//
// Contrato da fase:
//   * TODA a arrecadacao de impostos que hoje entra no caixa da cidade/prefeitura
//     (IPTU + taxas locais; o imposto de renda do modelo e 100% rebatido) passa a
//     entrar no Tesouro da nacao (`WorldState::nation_treasury`) com a flag ON;
//   * a equalizacao fiscal per capita e o repasse municipal morrem;
//   * o Tesouro distribui por mes um envelope por regiao: proporcional a
//     populacao simulada, com piso minimo por regiao (`floor_fraction` do
//     rateio igualitario). O envelope e creditado no orcamento publico da regiao
//     (`City::government_budget`) e na conta de obras (`CityGov::cash`, fatia
//     `works_share`);
//   * todo movimento e transferencia entre caixas ja contados por
//     `SimCore::money_total` - nenhuma rubrica cria/destroi dinheiro.
//
// Desligada (default): `collect`/`distribute_envelopes` nao sao chamados e o
// mundo continua bit-identico ao anterior.
#include "world_state.h"

#include <string>

namespace ogs {
namespace treasury {

// A flag esta ligada e a configuracao e valida?
bool enabled(const WorldState& ws);

// Credita o imposto arrecadado no Tesouro da nacao. O DEBITO ja aconteceu no
// caixa do pagador (familia/firma/servico); esta funcao so move a contraparte.
void collect(WorldState& ws, const std::string& nation, double amount);

// Saldo do Tesouro de uma nacao / do mundo.
double balance(const WorldState& ws, const std::string& nation);
double total_balance(const WorldState& ws);

// Peso de uma regiao no rateio: max(pop_share, floor_fraction / n_regioes).
// pop_share/finito sao saneados; n_regioes <= 0 devolve 0.
double envelope_weight(double pop_share, int city_count, double floor_fraction);

// Distribui `ws.treasury_envelope_share` dos saldos de cada nacao em envelopes
// mensais. Devolve o total distribuido. Nao cria nem destroi dinheiro
// (Tesouro -> government_budget/CityGov::cash).
double distribute_envelopes(WorldState& ws);

// Desliga a flag sem destruir dinheiro: cada saldo volta ao orcamento publico
// da primeira cidade da nacao. No-op com saldo 0 (mundo OFF).
void drain_to_regions(WorldState& ws);

} // namespace treasury
} // namespace ogs
