#pragma once
// C4 (D-110) — BANCOS COM DONO: público, privado nacional e estrangeiro.
// Desenho e contrato: docs/economy/C4_banks.md.
//
// A C3 (D-107) tinha "1 banco agregado por nação" como credor mínimo, sem
// depósito e sem capital. A C4 o substitui por até três bancos com dono:
//
//   * PÚBLICO   — dono o Estado/Tesouro da nação (`owner_kind = state`);
//   * PRIVADO   — dono as famílias da nação (`owner_kind = national`);
//   * ESTRANGEIRO — dono a nação de origem (`owner_kind = foreign`), só existe
//     quando banks.json declara a presença (nunca inventado).
//
// Dinheiro bancário: o empréstimo cria DEPÓSITO (passivo do banco, caixa do
// tomador), não M0 — `money_total` sobe e `M0` não (M1 = M0 + depósitos). O
// pagamento do principal e o juro DESTROEM depósito; o juro vira patrimônio do
// banco; o calote vira perda no patrimônio (C6 resolve, não esta fase).
//
// Cada grupo tem UM banco credor (regra declarada no §3 do doc): grupo estatal
// → banco público (crédito dirigido); os demais → papel sorteado por hash
// estável do id contra as participações declaradas. Isso mantém a matemática
// da C3 por par banco↔grupo e `Σ group.debt = Σ bank.loans`.
//
// Default do jogo: crédito DESLIGADO (`enabled = false`) — o mundo default não
// muda. Os bancos existem com posição zero; nada é inventado.

#include "world_state.h"

#include <string>
#include <vector>

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

// --- C4: papéis, participações e dividendo (default de jogo declarado, §3/§4)
constexpr int kRolePublic = 0;
constexpr int kRolePrivate = 1;
constexpr int kRoleForeign = 2;
constexpr double kSharePublic = 0.45;   // com banco estrangeiro
constexpr double kSharePrivate = 0.40;
constexpr double kShareForeign = 0.15;
constexpr double kDividendPayoutDefault = 0.5;  // metade do lucro retido
constexpr int kDividendPeriodDays = 30;         // fechamento mensal

struct Policy {
	bool enabled = false;
	double spread = kSpreadDefault;
	double leverage_cap = kLeverageCapDefault;
	double interest_cover = kInterestCoverDefault;
	int default_days = kDefaultDaysDefault;
	double repay_share = kRepayShareDefault;
	double dividend_payout = kDividendPayoutDefault;
};

// Política efetiva: estado salvo (ligado/desligado) + parâmetros de regra
// declarados (0 = default de sim/bank.h).
Policy policy(const WorldState& ws);

// Moeda da nação (vazio quando a nação não aparece em nenhuma moeda).
const std::string& currency_of_nation(const WorldState& ws,
	const std::string& nation);

// Taxa ANUAL do empréstimo = taxa básica efetiva da moeda (C2b1) + spread.
double loan_rate(const WorldState& ws, const std::string& nation);

// --- Identidade e papéis
const char* role_key(int role);                    // "public" | "private" | "foreign"
int role_from_key(const std::string& key);         // -1 = inválido
std::string bank_id(const std::string& nation, int role);

// Banco do papel; cria a linha zerada na primeira chamada (nome vazio =
// `no_data`, participação do default declarado).
Bank& ensure(WorldState& ws, const std::string& nation, int role);
Bank& ensure(WorldState& ws, const std::string& nation);  // papel público
const Bank* find(const WorldState& ws, const std::string& nation, int role);
const Bank* find(const WorldState& ws, const std::string& nation);
const Bank* find_id(const WorldState& ws, const std::string& id);

// Participação declarada de cada papel quando os três existem (renormaliza
// entre os existentes; 0 para papel ausente).
double declared_share(const WorldState& ws, const std::string& nation,
	int role);

// --- Definições (banks.json → nomes/estrangeiro). O parsing do JSON fica na
// fachada (`EconomySim::set_bank_definitions`); aqui só a regra.
struct NameData {
	std::string nation;
	std::string public_name;
	bool public_present = false;
	std::string private_name;
	bool private_present = false;
	std::string foreign_name;
	bool foreign_present = false;
	std::string foreign_origin;  // ISO3 do país de origem
};
// Cria/renomeia os bancos de cada nação do dado, renormaliza as participações
// e — quando o dado NÃO declara banco estrangeiro — funde o papel estrangeiro
// no privado CONSERVANDO todos os valores. Nunca inventa banco estrangeiro.
void apply_definitions(WorldState& ws, const std::vector<NameData>& defs);

// Patrimônio do banco: caixa + empréstimos + juros a receber − dívida com o
// BC − depósitos. Pode ser negativo: é a perda medida (§6 do doc).
double equity(const Bank& bank);
// Lucro retido = patrimônio − capital pago pelo dono (capital não é lucro).
double retained(const Bank& bank);

// Limite de crédito do grupo = min(alavancagem sobre o patrimônio,
// cobertura de juros do lucro anual). `sheet` pode vir pré-calculado.
double credit_limit(const WorldState& ws, const balance::GroupSheet& sheet,
	double rate = -1.0);
double credit_limit(const WorldState& ws, const Group& group,
	double rate = -1.0);

// Banco credor do grupo (regra declarada do §3). nullptr = nação sem banco.
const Bank* lender(const WorldState& ws, const Group& group);
Bank* lender(WorldState& ws, const Group& group);

// Empréstimo ao grupo, limitado pelo crédito disponível. Cria depósito
// (dinheiro bancário) e não M0. Devolve `false` com `error` quando recusado;
// `out_amount` é o concedido (pode ser menor).
bool lend(WorldState& ws, const std::string& group_id, double amount,
	double* out_amount, std::string* error);

// D-126: crédito de capital de giro das firmas privadas e dos serviços de uma
// cidade. O tomador é o caixa agregado da cidade (`firm_cash`); o passivo fica
// em `City::firm_debt` e a contraparte no banco PÚBLICO da nação (o mesmo
// credor da regra C4). Cria depósito (dinheiro bancário), nunca M0. O chamador
// já limita o pedido; aqui só se aplica o aperto de capital da C6.
bool lend_city(WorldState& ws, const std::string& city_id, double amount,
	double* out_amount, std::string* error);

// Capital pago pelo dono do banco (Tesouro, famílias ou exterior). Conservado:
// não cria dinheiro. `out_paid` = efetivamente pago.
double inject_capital(WorldState& ws, const std::string& nation, int role,
	double amount, double* out_paid, std::string* error);
// Compatibilidade C3: capital do banco público (dono Estado).
double inject_capital(WorldState& ws, const std::string& nation, double amount,
	double* out_paid, std::string* error);

// Juro do envelope de crédito da safra (D-105): o caixa do tomador já foi
// debitado pelo chamador; credita o banco público da nação. Sem banco público
// devolve 0 (o valor NÃO é cobrado; ausência declarada).
double receive_harvest_interest(WorldState& ws, const std::string& nation,
	double amount);

// Fase do dia: juros, atraso, amortização, calote e dividendo mensal.
// Devolve o nº de calotes. `bank_money_delta` (opcional) recebe a criação/
// destruição de dinheiro bancário que o invariante soma ao `money_total`:
// juro pago e amortização (−) e dividendo (+). O caixa tomado na execução
// apenas troca depósito por caixa do banco e não entra nesse fluxo.
int phase(WorldState& ws, double* bank_money_delta = nullptr);

struct Totals {
	double loans = 0.0;
	double arrears = 0.0;
	double deposits = 0.0;
	double bc_borrowing = 0.0;
	double cash = 0.0;
	double capital = 0.0;
	double equity = 0.0;
	double retained = 0.0;
	double writeoffs = 0.0;
	double interest_income = 0.0;
	double interest_expense = 0.0;
	double dividends = 0.0;
	double remittance = 0.0;
	double remittance_origin = 0.0;
	double harvest_interest = 0.0;
	int defaults = 0;
	int loans_count = 0;
	int banks = 0;
};
Totals totals(const WorldState& ws);
double total_group_debt(const WorldState& ws);
double total_group_arrears(const WorldState& ws);
double deposits_total(const WorldState& ws);

} // namespace bank
} // namespace ogs
