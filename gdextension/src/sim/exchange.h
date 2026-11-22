#pragma once
// C2b2 — REGIMES DE CÂMBIO, PADRÃO MONETÁRIO E OURO FÍSICO (D-106).
// Doc: docs/economy/C2b2_fx_gold.md. Plano: perf_pop_financas_plano.md, C2.
//
// O banco central da C2a (entidade por moeda) e a emissão da C2b1 continuam
// como estão. Esta fase liga o que eles já sabiam declarar:
//
//   1. REGIME por moeda: `floating` (o drift do F5), `peg` (paridade contra a
//      moeda de reserva) e `band` (paridade ± largura). Fixo/banda são
//      defendidos com as RESERVAS do BC; reserva esgotada vira desvalorização
//      forçada, registrada como evento.
//   2. PADRÃO do mundo: `fiat` (default), `bretton_woods` (moedas presas à
//      reserva; a âncora conversível em ouro a preço fixo) e `gold` (idem +
//      M0 limitado pela cobertura). Troca só por comando A3.
//   3. OURO FÍSICO: o ouro do cofre (G2/D-101) é estoque real no armazém
//      `vault` do governo; a compra/venda do BC passa pelo mercado do produto
//      ouro, sem teto econômico de preço (D-091).
//
// NADA aqui inventa número de país: paridade default é o `base_rate` do dado
// (F5), reserva inicial é 0 (ausência declarada) e o preço fixo do ouro é o
// preço de catálogo do próprio jogo (goods.json). Os parâmetros de regra
// (largura da banda, fatia de defesa, cobertura) são DEFAULT DE JOGO,
// declarados em data/definitions/economy/exchange_policy.json.

#include "world_state.h"

#include <string>
#include <vector>

namespace ogs {
namespace exchange {

// Espelho de data/definitions/economy/exchange_policy.json (default de jogo).
constexpr double kBandWidthDefault = 0.05;       // ±5 %
constexpr double kDefenceShareDefault = 0.5;     // metade do excesso do dia
constexpr double kGoldReserveRatioDefault = 1.0; // cobertura de 100 %
// Anel de eventos do status (o mais antigo sai). Não é log de auditoria: o
// contador total (`devaluations`) é a fonte para "quantas".
constexpr int kMaxEvents = 64;

struct Parameters {
	double band_width = kBandWidthDefault;
	double defence_share = kDefenceShareDefault;
	double gold_reserve_ratio = kGoldReserveRatioDefault;
};

Parameters parameters(const WorldState& ws);
void set_parameters(WorldState& ws, double band_width, double defence_share,
	double gold_reserve_ratio);

// O padrão do mundo força o peg de TODA moeda (Bretton Woods e padrão ouro):
// o regime SALVO é preservado e publicado, o EFETIVO é `peg`.
bool standard_forces_peg(const WorldState& ws);
int effective_regime(const WorldState& ws, const Currency& currency);

// Paridade (unidades locais por 1 unidade da moeda de reserva). 0 salvo = o
// `base_rate` do dado do F5 (não é número novo). Para moeda COM âncora (C2c)
// a paridade é contra a ÂNCORA (moeda ou cesta), não contra o XGS.
double parity_of(const WorldState& ws, const Currency& currency);

// C2c (D-108) — ÂNCORA e CESTA.
// A moeda tem âncora declarada (moeda única ou cesta com composição)?
bool has_anchor(const WorldState& ws, const Currency& currency);
// Valor em XGS de 1 unidade da âncora: a taxa da moeda-âncora, ou a média
// GEOMÉTRICA PONDERADA das taxas cruzadas da cesta. 0 = âncora sem taxa ou
// cesta sem composição — ausência declarada, nunca um número inventado.
double anchor_value(const WorldState& ws, const Currency& currency);
// Resolve a composição de cesta que o FMI não publica: pesos = participação
// medida no comércio do próprio mundo (default de jogo, marcado como tal).
// Só age uma vez por moeda, quando há comércio medido; sem comércio, vazio.
void resolve_baskets(WorldState& ws);
// Paridade inicial cruzada: fixa `fx[moeda].parity` para a taxa de câmbio com
// que o mundo nasceu (a taxa do F5 é o ponto de partida, não um dado novo).
void initialize_anchors(WorldState& ws);
// Alvo (taxa em XGS por 1 unidade local) que o regime manda: `ancora ÷
// paridade`. Sem âncora, devolve `parity_of` (o caminho da C2b2, idêntico).
double target_rate(const WorldState& ws, const Currency& currency);

// D-108(e) — SUCESSÃO de moeda extinta (ex.: BGN -> EUR a 1 EUR = 1,95583 BGN,
// Decisão (UE) 2025/1407). Funde nação, paridade, reservas, ouro do BC, emissão
// e bancos na moeda que a substituiu. O dinheiro do núcleo é em XGS (âncora):
// a conservação fecha por construção; só a paridade LOCAL (unidades por 1
// âncora) é dividida por `units_per_target`. Idempotente.
// @returns a paridade local convertida (0 se a moeda não existia).
double migrate_currency(WorldState& ws, const std::string& from,
	const std::string& to, double units_per_target);
// Largura da banda (±fração). -1 salvo = default de jogo.
double band_of(const WorldState& ws, const Currency& currency);
// Preço fixo de conversão do ouro (unidades-âncora por tonelada). 0 = preço de
// catálogo do bem `gold`; congelado quando o padrão é ligado por comando.
double conversion_price(const WorldState& ws, const Currency& currency,
	const std::vector<double>& base_prices);

struct Event {
	int day = -1;
	std::string currency;
	std::string type;  // "devaluation" | "reserve_purchase" | "gold_conversion"
	double old_parity = 0.0;
	double new_parity = 0.0;
	double reserves_used = 0.0;  // unidades de âncora
};

void push_event(WorldState& ws, Event event);

// Estatística do dia de câmbio (status / teste).
struct DayStats {
	int defended = 0;          // moedas que o BC defendeu hoje
	int devaluations = 0;      // desvalorizações forçadas hoje
	double reserves_sold = 0.0;    // âncora vendida (defesa de depreciação)
	double reserves_bought = 0.0;  // âncora comprada (defesa de apreciação)
	double money_destroyed = 0.0;  // moeda local recolhida (venda de reserva)
	double money_created = 0.0;    // moeda local emitida (compra de reserva)
};

// A fase de câmbio do dia: EMA da balança, drift do flutuante e a defesa dos
// regimes fixo/banda com as reservas. Substitui o corpo antigo do `tick_fx`
// (o caminho flutuante e o peg administrativo sem reserva são idênticos).
DayStats tick(WorldState& ws, const std::vector<double>& base_prices);

// Cobertura de ouro de uma moeda (medida) e o teto de M0 sob o padrão ouro.
struct Coverage {
	double gold_tonnes = 0.0;
	double gold_value_anchor = 0.0;  // toneladas × preço fixo
	double m0 = 0.0;                 // moeda local
	double m0_anchor = 0.0;          // m0 ÷ paridade
	bool has_gold = false;
	double ratio = 0.0;              // valor ÷ M0 (0 sem M0)
};
Coverage coverage(const WorldState& ws, const Currency& currency, double m0,
	const std::vector<double>& base_prices);
// Espaço de emissão (moeda local) até a cobertura exigida. `inf` = sem teto
// (padrão fiduciário) — nunca um número inventado.
double emission_headroom(const WorldState& ws, const Currency& currency,
	double m0, const std::vector<double>& base_prices);

// Reservas: declaração explícita (lab/teste) e a regra do cenário de estresse
// (N dias de importação MEDIDA do próprio mundo; não é dado observado).
void set_reserves(WorldState& ws, const std::string& code, double anchor_units);
double seed_reserves_from_imports(WorldState& ws, int days);

// Ouro físico do cofre: toneladas do razão do BC e o estoque do armazém.
double vault_gold(const WorldState& ws, const Currency& currency);
struct GoldTrade {
	bool ok = false;
	double tons = 0.0;
	double value = 0.0;
	std::string city;
	std::string error;
};
// Compra/venda do BC contra o mercado da cidade do cofre. Ouro não é criado
// nem destruído: só muda de dono. `buy` = cofre compra do mercado.
GoldTrade gold_trade(WorldState& ws, const std::string& currency, double tonnes,
	bool buy);
// Conversão Bretton Woods/padrão ouro: a âncora entrega ouro do mercado ao
// cofre ao preço fixo e recebe moeda local (emitida contra a reserva gasta).
GoldTrade gold_convert(WorldState& ws, const std::string& currency,
	double tonnes, const std::vector<double>& base_prices);

} // namespace exchange
} // namespace ogs
