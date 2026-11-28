#pragma once
// R1c (QA t177) - ALAVANCAS EXPLICITAS do Estado sob `rework.firms_own_cash`.
//
// `apply_fund_group` e `apply_create_state_company` sao ATOS DELIBERADOS do
// jogador, nao socorro automatico: a spec do rework declara que estatais
// existem e que a nacionalizacao e uma transferencia. Com a flag ON, porem, o
// orcamento municipal e apenas um LIMITE do mes (nao caixa), entao esses dois
// comandos passam a debitar o TESOURO NACIONAL e respeitar o saldo:
//   * saldo insuficiente -> o comando falha limpo (nada e movido, nenhuma
//     estatal e criada), sem debito parcial;
//   * o valor debitado e exatamente o creditado na contraparte -> `money_total`
//     invariante (nenhum dinheiro criado ou destruido);
//   * sem a flag ON (ou sem o Tesouro valido) o caminho antigo continua: o
//     orcamento das cidades e dinheiro real, como no mundo anterior ao R1.
//
// Regras de quando/como usar essas alavancas entram na fase politica (R3); o
// nucleo so garante a contabilidade.
#include <string>

namespace ogs {

struct WorldState;

struct TreasuryFundResult {
	bool funded = false;
	double moved = 0.0;
	std::string error;
};

struct TreasuryCompanyResult {
	bool created = false;
	std::string id;
	double capital = 0.0;
	std::string error;
	std::string warning;
};

// R2c: cria/reabre o banco PUBLICO da nacao (charter) com o capital debitado
// do Tesouro. Falha limpa sem saldo (nada e criado/movido). E' a mesma
// alavanca explicita da `create_state_company`, aplicada ao papel publico:
// funciona em qualquer nacao, inclusive nas que nasceram sem banco publico.
struct TreasuryBankResult {
	bool created = false;
	std::string id;
	double capital = 0.0;
	std::string error;
	std::string warning;
};

// A flag das empresas-do-caixa-proprio esta ligada E o Tesouro e valido?
bool treasury_levers_enabled(const WorldState& ws);

// Aporte de capital a um grupo existente, debitado do Tesouro da nacao de
// origem do grupo. Falha inteiro se o saldo nao cobre.
TreasuryFundResult action_fund_group_from_treasury(WorldState& ws,
		const std::string& group_id, double amount);

// Cria uma estatal com o capital debitado do Tesouro da nacao. Falha antes de
// criar o grupo se o saldo nao cobre o capital pedido (capital <= 0 cria sem
// debito, como no caminho antigo).
TreasuryCompanyResult action_create_state_company_from_treasury(
		WorldState& ws, const std::string& nation, const std::string& name,
		double capital);

TreasuryBankResult action_charter_public_bank_from_treasury(WorldState& ws,
		const std::string& nation, double capital);

} // namespace ogs
