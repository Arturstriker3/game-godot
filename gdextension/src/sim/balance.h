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

	double financial_assets() const { return cash + receivables; }
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
	double monetary_base = 0.0;  // passivo da autoridade monetária (money_total)
	double money_total = 0.0;    // contagem canônica (SimCore::money_total)

	// Enumeração INDEPENDENTE do caixa: se divergir de `money_total`, algum
	// caixa ficou de fora de um dos dois lados (é o que o invariante pega).
	double cash_total() const {
		return families_cash + firms_cash + government + external;
	}
	// Soma mundial das posições financeiras (deve ser 0).
	double financial_sum() const {
		return families + firms + government + external - monetary_base;
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
