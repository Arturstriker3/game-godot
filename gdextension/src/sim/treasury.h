#pragma once
// R1b - Tesouro Nacional + ORCAMENTO REGIONAL (flag
// `rework.national_treasury`).
//
// Contrato da fase (decisao do dono, 27/11):
//   * TODA a arrecadacao de impostos que hoje entra no caixa da cidade/prefeitura
//     (IPTU + taxas locais; o imposto de renda do modelo e 100% rebatido) passa a
//     entrar no Tesouro da nacao (`WorldState::nation_treasury`) com a flag ON;
//   * a equalizacao fiscal per capita e o repasse municipal morrem;
//   * o dinheiro NAO e transferido para a regiao. Cada regiao recebe um LIMITE
//     mensal de gasto (`City::regional_budget_limit_month`), proporcional a
//     populacao simulada com piso minimo por regiao (`floor_fraction` do rateio
//     igualitario). A dotacao e dividida entre servicos
//     (`City::government_budget`, restante do mes) e obras
//     (`CityGov::cash`, restante do mes);
//   * toda despesa publica debita o TESOURO na reconciliacao do tick, ate o
//     limite; o que excede o limite nao e executado (adiado deterministicamente)
//     e o que nao foi gasto permanece no Tesouro;
//   * a conservacao fica simples: um saldo (Tesouro) + contadores de
//     limite/gasto por regiao. Nenhuma rubrica cria/destroi dinheiro.
//
// Desligada (default): `start_month`/`reconcile` nao mudam nada e o mundo
// continua bit-identico ao anterior.
#include "world_state.h"

#include <string>

namespace ogs {
namespace treasury {

// A flag esta ligada e a configuracao e valida?
bool enabled(const WorldState& ws);

// Credita a receita publica no Tesouro da nacao. O DEBITO ja aconteceu no
// caixa do pagador (familia/firma/servico); esta funcao so move a contraparte.
void collect(WorldState& ws, const std::string& nation, double amount);

// Saldo do Tesouro de uma nacao / do mundo.
double balance(const WorldState& ws, const std::string& nation);
double total_balance(const WorldState& ws);

// Peso de uma regiao no rateio: max(pop_share, floor_fraction / n_regioes).
// pop_share/finito sao saneados; n_regioes <= 0 devolve 0.
double budget_weight(double pop_share, int city_count, double floor_fraction);

// Abre o mes do orcamento regional: calcula a dotacao de cada regiao a partir
// de `regional_budget_share` do saldo de cada Tesouro (populacao + piso) e
// grava limite/restante de servicos e obras. NAO move dinheiro. Devolve a soma
// das dotacoes.
double start_month(WorldState& ws);

// Leva o caixa publico regional existente para o Tesouro e zera os contadores
// regionais (ao ligar a flag, para nao sobrar saldo estacionado na regiao).
void absorb_public_cash(WorldState& ws);

// Reconcilia o gasto/receita publica do intervalo desde a ultima chamada:
// consome o saldo do Tesouro pelo gasto dos contadores e credita receitas
// publicas. Idempotente quando nada mudou. Devolve o gasto debitado.
double reconcile(WorldState& ws);

// Zera os contadores regionais (ao desligar a flag). O Tesouro fica intacto:
// o dinheiro nao foi da regiao, entao nao ha o que devolver.
void clear_counters(WorldState& ws);

// Dissolve o Tesouro + os contadores virtuais (mundo ON) em dinheiro real,
// devolvendo o total de cada nacao ao orcamento da primeira cidade da nacao.
// Usado so ao DESLIGAR a flag/num save ON carregado sem `regional_cash`: o
// mundo OFF nao pode ficar com dinheiro em `nation_treasury` (senao o save
// OFF perde o saldo). Nao cria nem destroi dinheiro.
void drain_to_regions(WorldState& ws);

} // namespace treasury
} // namespace ogs
