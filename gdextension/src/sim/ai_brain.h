#pragma once
// D-131 — CÉREBRO ECONÔMICO DA IA (fase A).
//
// Regras puras sobre WorldState, sem godot-cpp:
//   * R1 — previsão de fluxo de caixa mensal de um candidato de investimento
//     (capex, dias de obra, rampa D-125, folha cheia, opex/insumo, juros) com
//     o gate de caixa (a)-(e) e o dimensionamento fechado em `cap_max`;
//   * R3 — preço esperado = preço de hoje × tendência (média móvel) × castigo
//     por excesso de oferta previsto.
//
// A concorrência (R2), a exposição/anti-manada (R4) e o vínculo com a D-126
// (R5) ficam para as fases B/C: aqui a oferta futura é a oferta existente de
// hoje, como na IA atual.
//
// Defaults declarados em `data/definitions/economy/ai_brain.json` (NÃO são
// dado observado). `enabled=false` mantém as decisões da IA como antes.
#include <map>
#include <string>
#include <vector>

#include "efficiency.h"
#include "world_state.h"

namespace ogs {
namespace ai_brain {

// Aplica um mapa chave->valor (chaves pontuadas: "forecast.month_days") sobre
// os parâmetros, sem tocar em chaves ausentes. Valores inválidos são
// ignorados (nunca lança).
void apply_params(WorldState& ws, const std::map<std::string, double>& values);

// Mapa completo dos parâmetros (chaves pontuadas, ordem estável) para o
// save/status.
std::map<std::string, double> params_map(const WorldState& ws);

// R3 — tendência da série de preços (média dos últimos `trend_days` dividida
// pela média da janela anterior), limitada a [trend_min, trend_max]. Menos de
// 5 observações devolve 1.0 (ausência declarada). Com histórico menor que
// `2 × trend_days`, a janela corrente usa o máximo disponível que ainda deixe
// ao menos 5 observações para a janela anterior (ex.: 40 observações e
// trend_days=30 → 30 correntes / 10 anteriores).
double price_trend(const AiBrainParams& params,
	const std::vector<double>& history);

// R3 — preço esperado = preço de hoje × clamp(trend) ×
// clamp(1 − elasticidade × excesso, price_floor, 1).
// `excesso = max(0, (oferta − demanda) / max(demanda, 0.001))`.
double expected_price(const AiBrainParams& params, double price_today,
	double trend, double supply_future, double demand_future);

// R1 — entradas do forecast de um candidato (uma planta de `candidate_capacity`
// t/dia). Todos os fluxos mensais são lineares na capacidade, por isso o
// dimensionamento é fechado.
struct ForecastInput {
	double cash = 0.0;
	double credit_approved = 0.0;
	double candidate_capacity = 0.0;
	double unit_capex = 2000.0;
	// Curva D-125: classe da planta nova/expansão, eficiência de partida e se o
	// equipamento (trator/máquina) estará ativo.
	EfficiencyClass efficiency_class = EfficiencyClass::kFactory;
	double from_efficiency = 0.0;
	bool equipment_active = false;
	// Dias de obra (cheios, sem receita/folha) e mês do forecast.
	int construction_days = 0;
	// Produção por unidade de capacidade por dia de operação.
	double aptitude = 1.0;
	double stage_factor = 1.0;
	double area_scale = 1.0;
	// Custo mensal por unidade de capacidade.
	double jobs_per_capacity = 0.0;
	double wage = 0.0;
	double opex_per_ton = 0.0;
	double input_ratio = 0.0;
	double input_price = 0.0;
	// Receita: preço esperado (o excesso de oferta já entra nele, R3).
	double price_expected = 0.0;
	// Juro anual do crédito aprovado (0 = forecast sem dívida).
	double loan_rate_annual = 0.0;
	// Gate (e): margem por tonelada com o preço esperado e a eficiência média
	// usada pelo payback D-125.
	double margin_per_ton = 0.0;
	double expected_efficiency = 1.0;
	double payback_limit_days = 150.0;
	double min_capacity = 2.0;
};

// R1 — resultado do forecast e do dimensionamento.
struct ForecastResult {
	// Gate (a)-(e) passou com a capacidade final.
	bool pass = false;
	// `true` quando o `cap_max` reduziu a capacidade candidata.
	bool constrained = false;
	// Capacidade final (0 quando abaixo de `min_capacity`).
	double capacity = 0.0;
	double cap_max = 0.0;
	double capex = 0.0;
	// Menor saldo mensal e o saldo no fim do horizonte (H meses).
	double min_balance = 0.0;
	double final_balance = 0.0;
	// Menor lucro operacional ACUMULADO (sem capex) nos primeiros
	// `trough_months_max` meses.
	double trough_profit = 0.0;
	// Fase C (R5): maior sequência de dias operacionais negativos dentro do
	// horizonte. O gate (d) antecipa os degraus da escada D-126: com a crise
	// ligada, uma sequência >= `crisis.mothball_days` reprova (a planta nasceria
	// para hibernar). 0 quando a crise está desligada.
	int max_loss_days = 0;
	double payback_days = 0.0;
	// Eficiência média da rampa no horizonte do forecast.
	double mean_efficiency = 0.0;
	// Horizonte do forecast em meses (H).
	int months = 0;
	// D-131 fase C (diagnóstico): primeiro gate reprovado.
	// 0 = passou; 1 = (a) capex > caixa × capital_share + crédito; 2 = (b)
	// saldo mínimo < −crédito; 3 = (c) saldo final < 0; 4 = (d) trough
	// operacional negativo (ou escada D-126); 5 = (e) payback; 6 = o
	// dimensionamento fechado ficou abaixo de `min_capacity` (cap_max).
	int failed_gate = 0;
	// Valores do forecast para o diagnóstico por gate.
	double spendable = 0.0;     // caixa × capital_share + crédito
	double revenue_total = 0.0; // receita somada no horizonte
	double cost_total = 0.0;    // folha + insumo + juro no horizonte
};

// R1 — previsão mensal + dimensionamento fechado + gates (a)-(e).
ForecastResult forecast_monthly(const AiBrainParams& params,
	const WorldState& ws, const ForecastInput& in);

} // namespace ai_brain
} // namespace ogs
