#pragma once
// C1 — CONTABILIDADE POR BALANÇO (docs/plans/perf_pop_financas_plano.md, PARTE C).
//
// Camada de LEITURA sobre o `WorldState`: não move dinheiro, não cria número.
// Ela organiza o que já existe em ativos/passivos por agente e fecha a
// identidade financeira do mundo:
//
//     soma das posições financeiras (famílias + firmas + governo + exterior)
//         − passivo da autoridade monetária  =  0
//
// Enquanto não há banco (C2), a autoridade monetária é um PASSIVO PLACEHOLDER
// igual ao `money_total()` do dia — a moeda em circulação é dívida de quem a
// emitiu, exatamente como o banco central da C2 vai registrar. Quando a C2
// criar a emissão, este passivo passa a ser o do BC (mesma identidade).
//
// Ativos REAIS (estoque a preço de mercado, capital fixo, terra/moradia da B6)
// não têm contrapartida financeira e entram só no patrimônio líquido de quem
// os detém. Os únicos instrumentos FINANCEIROS hoje são o caixa e o salário
// atrasado (D-099): o atrasado é ativo das famílias e passivo da firma que
// devia — por isso ele aparece duas vezes no `Ledger`.
//
// `Sheet` é o balanço agregado de um agente (ou de todos, `nation` vazio).
// Nada aqui é inventado: campo sem dado no `WorldState` fica 0 e a ausência é
// declarada em `docs/economy/C1_balance_sheet.md`.

#include "world_state.h"

#include <string>
#include <vector>

namespace ogs {
namespace balance {

// Balanço agregado de um agente (ou do mundo inteiro).
struct Sheet {
	// Financeiro.
	double cash = 0.0;         // dinheiro (caixa) do agente
	double receivables = 0.0;  // contas a receber (salário atrasado a receber)
	double payables = 0.0;     // contas a pagar (salário atrasado a pagar)
	double debt = 0.0;         // dívida (C2/C4; zero na C1)
	// Real.
	double inventory = 0.0;      // estoque a preço de mercado (t × preço)
	double fixed_capital = 0.0;  // capital fixo a valor contábil (plantas)
	double property = 0.0;       // terra/moradia/anel da B6 (valor de mercado)
	// C3 (D-107): empréstimos a receber (setor bancário) e claim de patrimônio
	// do Tesouro sobre o banco (capital pago). Financeiros.
	double loans = 0.0;
	double equity_claims = 0.0;
	// C4 (D-110): depósitos à vista do setor bancário (passivo).
	double deposits = 0.0;

	double financial_assets() const {
		return cash + receivables + loans + equity_claims;
	}
	double financial_liabilities() const { return payables + debt; }
	// Ativos − passivos, com os ativos reais dentro (patrimônio líquido).
	double net_worth() const {
		return financial_assets() + inventory + fixed_capital + property
			- financial_liabilities();
	}
};

// Livro do dia: posição financeira líquida de cada setor + a contrapartida
// monetária. `families`/`firms` já incluem os recebíveis/pagáveis.
struct Ledger {
	double families = 0.0;    // caixa + a receber
	double firms = 0.0;       // caixa − a pagar
	double government = 0.0;  // caixa (governo local + prefeituras)
	double external = 0.0;    // foreign_cash (negativo = crédito externo usado)
	double families_cash = 0.0;  // caixa bruto das famílias (sem recebíveis)
	double firms_cash = 0.0;     // caixa bruto das firmas (sem pagáveis)
	double receivables = 0.0;
	double payables = 0.0;
	// Composição do atrasado (leitura; a soma é `payables`).
	double arrears_plants = 0.0;    // folha das plantas (D-099)
	double arrears_service = 0.0;   // folha do setor de serviços
	double arrears_rural = 0.0;     // folha rural (paga pelo caixa da cidade)
	// C2b1 (D-104): passivo LÍQUIDO do emissor = M0 − títulos na carteira do
	// BC. A emissão de M0 contra títulos do governo aumenta os dois lados: o
	// caixa público e a dívida pública (que fecha com os títulos do BC).
	double monetary_base = 0.0;
	double monetary_claims = 0.0;    // ativos do BC: títulos + empréstimos a bancos
	double government_debt = 0.0;    // dívida pública emitida (passivo)
	double money_total = 0.0;        // contagem canônica (SimCore::money_total)
	// C3 (D-107): setor bancário (banco agregado por nação) e a dívida dos
	// grupos. `banking` é a posição líquida do setor: caixa + empréstimos +
	// juros a receber − dívida com o BC − capital do Tesouro. O capital aparece
	// como claim (`government`), porque o Tesouro é o dono declarado.
	double banking = 0.0;
	double bank_cash = 0.0;
	double bank_loans = 0.0;
	double bank_arrears = 0.0;
	double bank_borrowing = 0.0;
	double bank_capital = 0.0;
	// C4 (D-110): depósitos à vista (passivo do banco, dinheiro bancário) e os
	// claims de capital dos donos não-estatais (famílias e exterior).
	double bank_deposits = 0.0;
	double household_claims = 0.0;
	double external_claims = 0.0;
	// Claim de patrimônio do governo sobre o banco (capital pago). É ativo
	// financeiro, NÃO caixa: por isso fica fora de `cash_total`.
	double government_claims = 0.0;
	double group_debt = 0.0;
	double group_arrears = 0.0;
	// C5 (D-112): ações a valor de mercado. `stock_assets` é a carteira dos
	// detentores e `stock_claims` o MESMO valor do lado do emissor; iguais por
	// construção, entram com sinais opostos — valorização não cria dinheiro.
	double stock_assets = 0.0;
	double stock_claims = 0.0;
	// C6 (D-114): caixa do fundo de seguro de depósito. O prêmio sai do caixa
	// do banco (reduz `banking`) e entra aqui; o pagamento faz o caminho
	// inverso. Sem o fundo, a enumeração de caixa ficaria menor que o
	// `money_total` pelo saldo acumulado.
	double insurance_fund = 0.0;

	// Enumeração INDEPENDENTE do caixa: se divergir de `money_total`, algum
	// caixa ficou de fora de um dos dois lados (é o que o invariante pega).
	double cash_total() const {
		return families_cash + firms_cash + government + external + bank_cash
			+ insurance_fund;
	}
	// Soma mundial das posições financeiras (deve ser 0). C4 (D-110): entram os
	// claims de capital das famílias e do exterior e o M0 líquido de depósitos.
	// C5 (D-112): a ação aparece dos dois lados pelo mesmo valor.
	double financial_sum() const {
		return families + firms + government + government_claims
			+ household_claims + external + external_claims + banking
			+ stock_assets - stock_claims + insurance_fund
			- government_debt - monetary_base;
	}
	double cash_residual() const { return cash_total() - money_total; }
};

// Livro do dia. `money_total` vem do SimCore (contagem canônica).
Ledger ledger(const WorldState& ws, double money_total);

// Balanço agregado por agente. `nation` vazio = mundo inteiro.
Sheet families(const WorldState& ws, const std::string& nation = std::string());
Sheet firms(const WorldState& ws, const std::string& nation = std::string());
Sheet government(const WorldState& ws, const std::string& nation = std::string());
// Setor externo: a posição NÃO é decomposta por nação (o núcleo guarda um
// `foreign_cash` global). Devolve o caixa do exterior inteiro; `nation` não
// altera o resultado — a ausência é declarada no status.
Sheet external(const WorldState& ws);

// C3 (D-107) — BALANÇO DO GRUPO (leitura). `inventory` é o rateio da
// capacidade do par cidade×produto — a mesma regra da receita — sobre o
// estoque de mercado das cidades onde o grupo produz; `fixed_capital` é o
// capital contábil das construções do grupo na fatia dele (1 − local_share em
// joint venture). `debt`/`arrears` vêm do crédito (zero sem crédito).
struct GroupSheet : Sheet {
	std::string id;
	std::string name;
	std::string origin_nation;
	std::string sector;
	double daily_cost = 0.0;    // folha + opex pagos no último dia
	double daily_profit = 0.0;  // lucro do último dia das construções do grupo
	int building_count = 0;
};
GroupSheet group_sheet(const WorldState& ws, const Group& group);
// Os mesmos balanços para todos os grupos, em UMA passagem pelo estado
// (índice alinhado com `ws.groups`).
std::vector<GroupSheet> group_sheets(const WorldState& ws);

// C3 (D-107) — SETOR BANCÁRIO: caixa + empréstimos + juros a receber (ativos)
// contra dívida com o BC + capital do Tesouro (passivos). `debt` é a soma dos
// dois passivos; `equity_claims` NÃO entra aqui (é do governo).
Sheet banking(const WorldState& ws);
Sheet banking_nation(const WorldState& ws, const std::string& nation);

// Diagnóstico de liquidez × solvência das plantas (leitura; não decide nada).
// Insolvente = patrimônio da planta < 0; ilíquida = patrimônio >= 0 mas menor
// que 30 dias de custo efetivamente pago (folha + opex).
struct FirmSolvency {
	int count = 0;
	int insolvent = 0;
	int illiquid = 0;
	int liquid = 0;
};
FirmSolvency firm_solvency(const WorldState& ws);

// Ativo de mercado de uma planta primária COM plano de safra (regra da D-099):
// `(estoque do mercado + safra em pé) × preço × fatia da firma`. A safra em pé
// é calculada pelo SimCore (precisa do calendário) e entra por parâmetro.
// Devolve 0 sem plano/capacidade. Mesma matemática da D-099 — a C1 só passou a
// chamá-la do balanço.
double marketable_assets(const WorldState& ws, std::size_t city_index,
	const Building& building, double price, double standing_value);

// Fatia da planta na capacidade do par cidade×produto (0..1). 0 = par sem
// capacidade primária (o chamador da D-099 devolve 1.0 nesse caso).
double firm_production_share(const WorldState& ws, std::size_t city_index,
	const Building& building);

} // namespace balance
} // namespace ogs
