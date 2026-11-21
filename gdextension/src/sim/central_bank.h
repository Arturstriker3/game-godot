#pragma once
// C2a — BANCO CENTRAL POR MOEDA (estado + medição). Decisão D-103.
// Plano: docs/plans/perf_pop_financas_plano.md, PARTE C / C2; doc desta fase:
// docs/economy/C2_central_bank.md.
//
// Esta fase é MEDIÇÃO. O banco central existe como ENTIDADE por moeda
// (`Currency`), com balanço que fecha sobre o balanço da C1:
//
//     passivo = base monetária M0
//               (moeda em circulação + reservas dos bancos)
//     ativo   = reservas internacionais (ouro do cofre + moeda de reserva)
//               e títulos
//
// O que a C2a NÃO faz: emitir, fixar juros, intervir no câmbio, converter em
// ouro ou negociar o ouro como bem físico. Isso é a C2b. Aqui nada é criado:
//   * M0 é a moeda que já existe, atribuída à moeda do país que a detém;
//   * reservas dos bancos = 0 (não há banco; é a C4);
//   * títulos = 0 (não há dívida pública no estado; é a C3/C4);
//   * moeda de reserva em carteira = 0 (o exterior do núcleo é um único
//     `foreign_cash` global, não decomposto — ausência declarada);
//   * ouro = dado REAL da G2/D-101 (World Gold Council via
//     `strategic_reserves.json`), declarado no núcleo por
//     `set_central_bank_gold()`. É ativo REAL: entra no ativo do BC e no
//     patrimônio, mas NÃO na identidade financeira (a soma das posições
//     financeiras do mundo continua 0, como na C1).
//
// A política (regime cambial, independência, padrão monetário, moeda de
// reserva) é ESTADO SALVO com o default atual e não age nesta fase.

#include "world_state.h"

#include <map>
#include <string>
#include <vector>

namespace ogs {
namespace central_bank {

// ---------------------------------------------------------------------------
// Política (salva no v27; nenhuma mecânica na C2a).
// ---------------------------------------------------------------------------
// Regime cambial da moeda. `kRegimePeg` é o default do dado atual
// (currencies.json não tem fonte de regime; `set_currencies` já cai em peg) e
// `kRegimeFloating` reproduz o drift dentro da banda que o fx.cpp já faz. A
// BANDA administrada (`kRegimeBand`) é um DEGRAU do mesmo drift — a mecânica
// própria (intervenção, largura, custo de defesa) é C2b.
enum ExchangeRegime {
	kRegimeFloating = 0,
	kRegimePeg = 1,
	kRegimeBand = 2,
};

// Independência do BC. `kIndependenceNoData` é a AUSÊNCIA declarada: o
// repositório não tem fonte de independência de banco central para 179 nações
// e a C2a não age sobre ela. `independent` (meta própria) e `controlled`
// (governo decide juro/emissão) são C2b.
enum Independence {
	kIndependenceNoData = 0,
	kIndependenceIndependent = 1,
	kIndependenceControlled = 2,
};

// Padrão monetário internacional (estado do MUNDO). Fiduciário é o início
// declarado pelo plano; Bretton Woods (âncora presa ao ouro) e padrão ouro
// (emissão limitada às reservas) são C2b.
enum Standard {
	kStandardFiat = 0,
	kStandardBrettonWoods = 1,
	kStandardGold = 2,
};

const char* regime_name(int value);        // "floating" | "peg" | "band"
const char* independence_name(int value);  // "no_data" | "independent" | "controlled"
const char* standard_name(int value);      // "fiat" | "bretton_woods" | "gold"
int regime_from_name(const std::string& name, int fallback);
int independence_from_name(const std::string& name, int fallback);
int standard_from_name(const std::string& name, int fallback);

// Modelo de calendário da série do CPI (12 meses de 30 dias, como o W2).
constexpr int kCpiSampleDays = 30;
constexpr int kCpiSampleSlots = 13;  // 12 meses de intervalo + a amostra atual
constexpr int kModelYearDays = 360;
// D-113 (calibração pré-C6): a anualização de uma janela CURTA não é leitura de
// inflação. Com 2 amostras (30 dias) o expoente 360/30 = 12 multiplicava a
// variação do 1º mês pelo fator 12: no dia 45 a anualizada mediana era 8,9 %
// (p90 24,1 %) e a regra de Taylor mirava 12,5 %, levando a taxa de ~0 % a ~10 %
// em 2 semanas. Antes de um trimestre de série a inflação é AUSENTE (declarada)
// e a regra usa só o neutro (e o hiato, depois do baseline).
constexpr int kAnnualizeMinSpanDays = 90;

// ---------------------------------------------------------------------------
// Dinheiro do dia atribuído às moedas (leitura).
// ---------------------------------------------------------------------------
// A moeda em circulação é atribuída pela nação de quem a detém. O que NÃO tem
// nação atribuível fica fora da soma por moeda e é DECLARADO em buckets
// próprios — nada é rateado por palpite. A soma de `allocated` + `unallocated`
// reproduz o `money_total` canônico (checagem cruzada do invariante).
struct MoneyAllocation {
	std::vector<double> by_currency;  // paralelo a `ws.currencies`
	double allocated = 0.0;           // caixa doméstico com moeda identificada
	// Sem moeda identificável:
	double sector_cash = 0.0;    // transporte + construção (caixas de setor)
	double group_cash = 0.0;     // grupos sem nação de origem
	double project_cash = 0.0;   // obra cuja cidade não resolve
	double external = 0.0;       // `foreign_cash` (posição do exterior, global)
	double no_currency = 0.0;    // nação sem moeda na tabela
	double total = 0.0;          // `money_total` do dia

	double unallocated() const {
		return sector_cash + group_cash + project_cash + external + no_currency;
	}
	double allocated_total() const { return allocated + unallocated(); }
	double residual() const { return allocated_total() - total; }
};

MoneyAllocation allocate_money(const WorldState& ws, double money_total);

// ---------------------------------------------------------------------------
// CPI por país (leitura) — a MESMA cesta do IPC do G2 (D-101):
// alimentos do `needs.json` ponderados pelo fator calórico, razão preço/base
// ponderada pela necessidade de cada quintil da cidade (sem quintis, a
// necessidade da cidade; sem ela, população de simulação × necessidade per
// capita). Os números são idênticos aos de `get_storage_status()` — o teste
// `c2_central_bank_test` prova a igualdade.
// ---------------------------------------------------------------------------
struct CpiRow {
	double index = 0.0;   // razão média preço/base (1,0 = base do catálogo)
	double weight = 0.0;  // soma dos pesos das cidades no índice
	bool ok = false;      // false = nenhuma cidade com preço de cesta
};

std::map<std::string, CpiRow> cpi_by_nation(const WorldState& ws,
	const std::vector<double>& base_prices);

// Mantém a SÉRIE MENSAL do CPI por nação (amostra a cada 30 dias). Só
// medição: escreve apenas `ws.cpi_history` / `ws.cpi_last_observe_day`. Deve
// ser chamado depois dos invariantes do dia (um tick revertido não amostra).
bool cpi_sample_due(const WorldState& ws);
void observe_cpi(WorldState& ws, const std::vector<double>& base_prices);

// Inflação lida da série. Ausência é declarada (`has_*` = false), nunca
// completada com número inventado.
struct Inflation {
	bool has_index = false;
	double index = 0.0;      // CPI da amostra mais recente
	int observations = 0;    // amostras na série (0..13)
	int span_days = 0;       // dias entre a 1ª e a última amostra
	bool has_12m = false;    // 13 amostras = 12 meses do modelo (360 d)
	double rate_12m = 0.0;   // variação em 12 meses
	bool has_annualized = false;
	double annualized = 0.0; // (última/1ª)^(360/dias) − 1
};

Inflation inflation_of(const CpiHistory& history);

// ---------------------------------------------------------------------------
// Preço do ouro usado para valorar a reserva.
// ---------------------------------------------------------------------------
// Média do preço de mercado do bem `gold` entre as cidades com preço > 0;
// sem mercado com cotação, usa o preço de catálogo (`base_prices`). O método
// vai em `basis` (nunca fica implícito).
double gold_price(const WorldState& ws, const std::vector<double>& base_prices,
	std::string* basis = nullptr);

// ---------------------------------------------------------------------------
// Banco central de uma moeda (balanço medido do dia).
// ---------------------------------------------------------------------------
struct CurrencyBank {
	std::string code;
	int regime = kRegimePeg;
	int independence = kIndependenceNoData;
	std::vector<std::string> nations;

	// Passivo: base monetária.
	double currency_in_circulation = 0.0;  // caixa total da moeda (público + bancos)
	double bank_reserves = 0.0;            // reservas dos bancos (0: sem banco)
	double m0 = 0.0;                       // caixa − depósitos (público + reservas)
	// Agregados (medidos do balanço da C1; sem emissão nova).
	double m1 = 0.0;  // caixa − reservas (público + depósitos à vista; C4)
	double m2 = 0.0;  // M1 + depósitos de poupança/prazo (C4: = M1)

	// Ativo: reservas internacionais e títulos.
	double gold_tonnes = 0.0;               // ouro fino (dado real da G2)
	double gold_value = 0.0;                // ouro × preço de conversão/mercado
	double reserve_currency_holdings = 0.0; // reservas em âncora (C2b2)
	double securities = 0.0;                // 0: sem dívida pública (C3/C4)

	// C2b2 (D-106) — regime efetivo e defesa (leitura do status).
	int regime_saved = kRegimePeg;
	int regime_effective = kRegimePeg;
	double parity = 1.0;
	double band_width = 0.0;
	bool funded = false;
	double intervention_today = 0.0;
	int devaluations = 0;
	int last_event_day = -1;
	double coverage_ratio = 0.0;  // ouro (preço fixo) ÷ M0 em âncora
	double conversion_price = 0.0;

	double financial_assets() const {
		return reserve_currency_holdings + securities;
	}
	double liabilities() const { return m0; }
	// Patrimônio FINANCEIRO: ativo financeiro − passivo. Fica −M0 porque a
	// contrapartida da moeda em circulação é a dívida do emissor (a mesma
	// identidade da C1). O ouro é ativo REAL e não entra aqui.
	double net_financial_worth() const { return financial_assets() - liabilities(); }
	double total_assets() const { return financial_assets() + gold_value; }
};

std::vector<CurrencyBank> banks(const WorldState& ws,
	const MoneyAllocation& money, const std::vector<double>& base_prices);

} // namespace central_bank
} // namespace ogs
