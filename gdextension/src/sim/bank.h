#pragma once
// C3 (D-107) — CRÉDITO DE EMPRESA: O BANCO AGREGADO COMO CREDOR MÍNIMO.
// Desenho e contrato: docs/economy/C3_group_balance.md.
//
// O plano da PARTE C pede "DemoGroup ganha dívida, estoque, capital fixo e
// patrimônio líquido; falência atinge credores". Para a dívida ter
// contraparte (o invariante da C1: todo ativo é passivo de alguém) esta fase
// traz o "1 banco agregado por nação" da C2 como **credor**, sem depósitos e
// sem capital próprio — as duas coisas são C4 e estão declaradas como
// ausência, não inventadas.
//
// Como o dinheiro se move (tudo contabilizado no balanço da C1):
//   * EMPRÉSTIMO (A): o BC cria A de M0 em favor do banco (`bc_borrowing`), o
//     banco entrega A ao grupo (`loans`, `group.debt`, `group.cash`).
//   * PAGAMENTO (P): o grupo paga P, o banco abate a dívida com o BC — o M0
//     correspondente é destruído (`emission_today` fica negativo).
//   * JURO: o grupo paga em caixa; o banco repassa a parcela da taxa básica
//     ao BC (abate reservas) e retém o spread como caixa. Sem caixa, o juro
//     vencido vai para `credit_arrears`/`arrears` — nunca é capitalizado em
//     silêncio.
//   * CALOTE: o banco toma o caixa do grupo, fecha as construções dele (a
//     mesma regra de falência das firmas) e lança a diferença em `writeoffs`;
//     a perda cai no patrimônio do banco e o dinheiro em circulação não muda
//     além do caixa tomado. Resolver o banco insolvente é C6.
//
// Default do jogo: DESLIGADO (`enabled = false`) — um banco sem capital fica
// insolvente no primeiro calote e socorrê-lo é C6. Ver §4 do doc.

#include "world_state.h"

#include <string>

namespace ogs {

namespace balance {
struct GroupSheet;
}

namespace bank {

// --- Defaults de jogo (espelho de data/definitions/economy/bank_credit.json)
constexpr double kSpreadDefault = 0.02;          // 2 p.p. sobre a taxa básica
constexpr double kLeverageCapDefault = 1.0;      // dívida ≤ 1 × patrimônio
constexpr double kInterestCoverDefault = 2.0;    // lucro anual ≥ 2 × juro anual
constexpr int kDefaultDaysDefault = 30;          // dias insolvente até o calote
constexpr double kRepayShareDefault = 0.5;       // metade do excedente por dia
constexpr double kDaysPerYear = 365.0;
constexpr double kWorkingCapitalDays = 30.0;     // caixa de giro protegido

struct Policy {
	bool enabled = false;
	double spread = kSpreadDefault;
	double leverage_cap = kLeverageCapDefault;
	double interest_cover = kInterestCoverDefault;
	int default_days = kDefaultDaysDefault;
	double repay_share = kRepayShareDefault;
};

// Política efetiva: estado salvo (ligado/desligado) + parâmetros de regra
// declarados (0 = default de sim/bank.h).
Policy policy(const WorldState& ws);

// Moeda da nação (vazio quando a nação não aparece em nenhuma moeda).
const std::string& currency_of_nation(const WorldState& ws,
	const std::string& nation);

// Taxa ANUAL do empréstimo = taxa básica efetiva da moeda (C2b1) + spread.
double loan_rate(const WorldState& ws, const std::string& nation);

// Banco agregado da nação; cria a linha zerada na primeira chamada.
Bank& ensure(WorldState& ws, const std::string& nation);
const Bank* find(const WorldState& ws, const std::string& nation);

// Patrimônio do banco: caixa + empréstimos + juros a receber − dívida com o
// BC + capital. Pode ser negativo: é a perda medida (§6 do doc).
double equity(const Bank& bank);

// Limite de crédito do grupo = min(alavancagem sobre o patrimônio,
// cobertura de juros do lucro anual). `sheet` pode vir pré-calculado.
double credit_limit(const WorldState& ws, const balance::GroupSheet& sheet,
	double rate = -1.0);
double credit_limit(const WorldState& ws, const Group& group,
	double rate = -1.0);

// Empréstimo ao grupo, limitado pelo crédito disponível. Devolve `false` com
// `error` quando recusado; `out_amount` é o concedido (pode ser menor).
bool lend(WorldState& ws, const std::string& group_id, double amount,
	double* out_amount, std::string* error);

// Capital pago pelo Tesouro da nação ao banco (dono estado, até a C4). O
// dinheiro sai do orçamento da maior cidade da nação (o "tesouro nacional" do
// modelo): não cria dinheiro. `out_paid` = efetivamente pago.
double inject_capital(WorldState& ws, const std::string& nation, double amount,
	double* out_paid, std::string* error);

// Fase do dia: juros, atraso, amortização e calote. Devolve o nº de calotes.
int phase(WorldState& ws);

struct Totals {
	double loans = 0.0;
	double arrears = 0.0;
	double bc_borrowing = 0.0;
	double cash = 0.0;
	double capital = 0.0;
	double equity = 0.0;
	double writeoffs = 0.0;
	double interest_income = 0.0;
	double interest_expense = 0.0;
	int defaults = 0;
	int loans_count = 0;
};
Totals totals(const WorldState& ws);
double total_group_debt(const WorldState& ws);
double total_group_arrears(const WorldState& ws);

} // namespace bank
} // namespace ogs
