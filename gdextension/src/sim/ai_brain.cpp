// D-131 — CÉREBRO ECONÔMICO DA IA (fase A). Ver docs/economy/D131_ai_brain.md,
// gdextension/src/sim/ai_brain.h e data/definitions/economy/ai_brain.json.
//
// Nada aqui lê arquivo nem toca o mundo: os parâmetros chegam pelo
// `AiBrainParams` do WorldState (defaults compilados = espelho do JSON) e as
// funções são puras/determinísticas. `enabled=false` faz o chamador não usar
// nada disto.
#include "ai_brain.h"

#include <algorithm>
#include <cmath>

namespace ogs {
namespace ai_brain {
namespace {

constexpr double kMinDemand = 0.001;

double mean_of(const std::vector<double>& values, std::size_t begin,
		std::size_t end) {
	if (begin >= end || end > values.size()) {
		return 0.0;
	}
	double total = 0.0;
	for (std::size_t index = begin; index < end; ++index) {
		total += values[index];
	}
	return total / static_cast<double>(end - begin);
}

// Produção por unidade de capacidade num intervalo de operação: soma diária da
// curva D-125 (mesma convenção de `eff::mean_efficiency`) × frações do sítio.
struct MonthProd {
	int op_days = 0;
	double mean_eff = 0.0;
	double prod_per_capacity = 0.0;  // t/dia de capacidade
};

MonthProd month_production(const WorldState& ws, const AiBrainParams& params,
		const ForecastInput& in, int month) {
	const int month_days = std::max(1, params.month_days);
	MonthProd out;
	const int month_begin = (month - 1) * month_days;
	const int month_end = month * month_days - 1;
	const int construction_days = std::max(0, in.construction_days);
	const int op_begin = std::max(month_begin, construction_days);
	if (month_end < op_begin) {
		return out;
	}
	out.op_days = month_end - op_begin + 1;
	// `mean_between` recebe dias de OPERAÇÃO (0 = primeiro dia de produção),
	// não dias de projeto: o índice precisa descontar a obra, senão a rampa
	// começa `construction_days` adiantada e o forecast fica otimista.
	out.mean_eff = eff::mean_between(ws, in.efficiency_class,
		in.from_efficiency, in.equipment_active,
		op_begin - construction_days, month_end - construction_days);
	const double site = std::max(0.0, in.aptitude)
		* std::max(0.0, in.stage_factor) * std::max(0.0, in.area_scale);
	out.prod_per_capacity = site * out.mean_eff
		* static_cast<double>(out.op_days);
	return out;
}

// Custo/receita MENSAIS por unidade de capacidade com uma capacidade de
// referência (a parcela de mercado depende da capacidade candidata).
struct UnitMonth {
	double burn = 0.0;  // custo + juro - receita, por unidade de capacidade
};

UnitMonth unit_month(const WorldState& ws, const AiBrainParams& params,
		const ForecastInput& in, int month, double ref_capacity) {
	UnitMonth out;
	const MonthProd prod = month_production(ws, params, in, month);
	const double capacity = std::max(0.0, ref_capacity);
	// `tons` é a produção do MÊS por unidade de capacidade (já inclui os
	// `op_days`). O opex NÃO usa `tons`: é custo por capacidade-dia (o núcleo
	// paga `capacity × opex_per_ton` sobre a operação).
	const double tons = prod.prod_per_capacity;
	const double total_prod = capacity * tons;
	// D-131 fase C (correção de dupla contagem): a planta vende a produção
	// pelo preço esperado. O excesso de oferta JÁ entra no preço (R3,
	// elasticidade com piso) — multiplicar de novo pela própria produção como
	// "parcela de mercado" contava a mesma coisa duas vezes e subcontava a
	// receita em ~100×.
	const double revenue = total_prod * std::max(0.0, in.price_expected);
	const double fixed = static_cast<double>(prod.op_days)
		* std::max(0.0, in.jobs_per_capacity) * std::max(0.0, in.wage);
	// Opex é custo por unidade de CAPACIDADE por dia (o núcleo grava
	// `capacity × opex_per_ton` e paga sobre a operação), não por tonelada
	// produzida. Por isso não escala com a aptidão nem com a eficiência.
	const double opex = static_cast<double>(prod.op_days)
		* std::max(0.0, in.opex_per_ton);
	const double inputs = std::max(0.0, in.input_ratio) * tons
		* std::max(0.0, in.input_price);
	// Juro do capital emprestado (o crédito aprovado cobre o que o caixa não
	// paga). Taxa anual / 365 × dias do mês.
	const double borrowed = std::max(0.0, capacity * in.unit_capex - in.cash);
	// Fase B (ressalva da fase A): receita e custos na MESMA base, por unidade
	// de capacidade. `revenue` acima é o total da capacidade de referência; o
	// dimensionamento fechado `cap_max = (caixa + crédito)/max_m(unit_capex +
	// burn_m)` só fecha se `burn` for o custo líquido POR capacidade.
	const double revenue_per_capacity =
		capacity > 1e-9 ? revenue / capacity : 0.0;
	out.burn = fixed + opex + inputs
		+ borrowed / std::max(1e-9, capacity)
			* std::max(0.0, in.loan_rate_annual)
			* static_cast<double>(std::max(1, params.month_days)) / 365.0
		- revenue_per_capacity;
	return out;
}

} // namespace

void apply_params(WorldState& ws, const std::map<std::string, double>& values) {
	AiBrainParams& p = ws.ai_brain;
	const auto get = [&values](const char* dotted, const char* bare,
			double fallback) {
		auto found = values.find(dotted);
		if (found != values.end()) {
			return found->second;
		}
		found = values.find(bare);
		return found == values.end() ? fallback : found->second;
	};
	p.enabled = get("enabled", "enabled", p.enabled ? 1.0 : 0.0) > 0.5;
	p.month_days = std::max(1,
		int(get("forecast.month_days", "month_days", p.month_days)));
	p.months_max = std::max(1,
		int(get("forecast.months_max", "months_max", p.months_max)));
	p.trough_months_max = std::max(0,
		int(get("forecast.trough_months_max", "trough_months_max",
			p.trough_months_max)));
	p.capital_share = std::clamp(
		get("forecast.capital_share", "capital_share", p.capital_share),
		0.0, 1.0);
	// Escopos: enum declarado (0 = região, 1 = mundo). O chamador converte os
	// nomes do JSON/save (`region`/`world`) para este código.
	p.scope_food = std::clamp(
		int(get("competition.scope_food", "scope_food", p.scope_food)), 0, 1);
	p.scope_other = std::clamp(
		int(get("competition.scope_other", "scope_other", p.scope_other)), 0, 1);
	p.demand_growth_days = std::max(1,
		int(get("competition.demand_growth_days", "demand_growth_days",
			p.demand_growth_days)));
	p.trend_days = std::max(1,
		int(get("price.trend_days", "trend_days", p.trend_days)));
	p.history_days = std::max(1,
		int(get("price.history_days", "history_days", p.history_days)));
	p.trend_min = std::max(0.0,
		get("price.trend_min", "trend_min", p.trend_min));
	p.trend_max = std::max(p.trend_min,
		get("price.trend_max", "trend_max", p.trend_max));
	p.elasticity = std::max(0.0,
		get("price.elasticity", "elasticity", p.elasticity));
	p.price_floor = std::clamp(
		get("price.price_floor", "price_floor", p.price_floor), 0.0, 1.0);
	p.max_good_share = std::clamp(
		get("risk.max_good_share", "max_good_share", p.max_good_share),
		0.0, 1.0);
	p.max_sector_share = std::clamp(
		get("risk.max_sector_share", "max_sector_share", p.max_sector_share),
		0.0, 1.0);
	p.max_goods_per_agent = std::max(0,
		int(get("risk.max_goods_per_agent", "max_goods_per_agent",
			p.max_goods_per_agent)));
	p.max_new_plants_per_good_30d = std::max(0,
		int(get("herd.max_new_plants_per_good_30d",
			"max_new_plants_per_good_30d",
			p.max_new_plants_per_good_30d)));
	p.max_new_capacity_share_30d = std::max(0.0,
		get("herd.max_new_capacity_share_30d", "max_new_capacity_share_30d",
			p.max_new_capacity_share_30d));
	p.food_multiplier = std::max(0.0,
		get("herd.food_multiplier", "food_multiplier", p.food_multiplier));
	p.block_loss_days = std::max(0,
		int(get("crisis_link.block_loss_days", "block_loss_days",
			p.block_loss_days)));
	// G2 — piso de demanda potencial (regra declarada; off não executa nada).
	p.potential_enabled = get("potential_demand.enabled",
		"potential_enabled", p.potential_enabled ? 1.0 : 0.0) > 0.5;
	p.potential_discount = std::clamp(get("potential_demand.discount",
		"potential_discount", p.potential_discount), 0.0, 1.0);
	p.potential_cap = std::max(0.0, get("potential_demand.cap",
		"potential_cap", p.potential_cap));
	// G2 (perf): periodo do recalculo do piso (1 = todo dia, o comportamento
	// antigo; 30 = cache mensal). Deterministico: dia 0 e multiplos de N.
	p.potential_recompute_days = std::max(1, int(get(
		"potential_demand.recompute_days", "potential_recompute_days",
		double(p.potential_recompute_days))));
	// G2 (perf): varredura mensal escalonada do investidor (cidade/grupo `i`
	// avalia quando `(dia - i) % N == 0`). 1 reproduz a varredura diaria.
	p.scan_interval_days = std::max(1, int(get(
		"scan_interval_days", "scan_interval_days",
		double(p.scan_interval_days))));
}

std::map<std::string, double> params_map(const WorldState& ws) {
	const AiBrainParams& p = ws.ai_brain;
	return {
		{"enabled", p.enabled ? 1.0 : 0.0},
		{"forecast.month_days", double(p.month_days)},
		{"forecast.months_max", double(p.months_max)},
		{"forecast.trough_months_max", double(p.trough_months_max)},
		{"forecast.capital_share", p.capital_share},
		{"competition.scope_food", double(p.scope_food)},
		{"competition.scope_other", double(p.scope_other)},
		{"competition.demand_growth_days", double(p.demand_growth_days)},
		{"price.trend_days", double(p.trend_days)},
		{"price.history_days", double(p.history_days)},
		{"price.trend_min", p.trend_min},
		{"price.trend_max", p.trend_max},
		{"price.elasticity", p.elasticity},
		{"price.price_floor", p.price_floor},
		{"risk.max_good_share", p.max_good_share},
		{"risk.max_sector_share", p.max_sector_share},
		{"risk.max_goods_per_agent", double(p.max_goods_per_agent)},
		{"herd.max_new_plants_per_good_30d",
			double(p.max_new_plants_per_good_30d)},
		{"herd.max_new_capacity_share_30d", p.max_new_capacity_share_30d},
		{"herd.food_multiplier", p.food_multiplier},
		{"crisis_link.block_loss_days", double(p.block_loss_days)},
		// G2: diagnóstico; o save só grava a seção quando difere do default.
		{"potential_demand.enabled", p.potential_enabled ? 1.0 : 0.0},
		{"potential_demand.discount", p.potential_discount},
		{"potential_demand.cap", p.potential_cap},
		{"potential_demand.recompute_days",
			double(p.potential_recompute_days)},
		// G2 (perf): cadencia da varredura do investidor (diagnostico/save
		// aditivo; so grava quando difere do default).
		{"scan_interval_days", double(p.scan_interval_days)},
	};
}

double price_trend(const AiBrainParams& params,
		const std::vector<double>& history) {
	const std::size_t count = history.size();
	// Ausência declarada: sem 5 observações não há tendência (1.0).
	if (count < 5) {
		return 1.0;
	}
	// Fase B (ressalva da fase A): com menos de `2 × trend_days` observações a
	// janela corrente usa TODO o histórico disponível até `trend_days`, desde
	// que sobre ao menos 5 observações para a janela anterior. O `count / 2`
	// antigo cortava a MA corrente pela metade mesmo quando ela caberia (ex.:
	// 40 observações com trend_days=30 usava 20/20 em vez de 30/10).
	const std::size_t trend = static_cast<std::size_t>(
		std::max(1, params.trend_days));
	std::size_t window = 0;
	if (count >= trend + 5) {
		window = trend;
	} else {
		// Série curta (5..trend+4): metade disponível, nunca menos de 2, para
		// que as duas médias existam.
		window = count / 2;
	}
	// A janela anterior usa o que sobrou (até `trend_days`); com 40
	// observações e trend=30 são 30 correntes e 10 anteriores.
	const std::size_t previous_window = std::min(trend, count - window);
	if (window < 2 || previous_window < 2 || count < window + previous_window) {
		return 1.0;
	}
	const double current = mean_of(history, count - window, count);
	const double previous = mean_of(history, count - window - previous_window,
		count - window);
	if (previous <= 1e-9) {
		return 1.0;
	}
	return std::clamp(current / previous, params.trend_min, params.trend_max);
}

double expected_price(const AiBrainParams& params, double price_today,
		double trend, double supply_future, double demand_future) {
	if (price_today <= 0.0) {
		return 0.0;
	}
	const double demand = std::max(demand_future, kMinDemand);
	const double excess = std::max(0.0,
		(supply_future - demand_future) / demand);
	const double penalty = std::clamp(
		1.0 - std::max(0.0, params.elasticity) * excess,
		params.price_floor, 1.0);
	const double clamped_trend = std::clamp(trend, params.trend_min,
		params.trend_max);
	return price_today * clamped_trend * penalty;
}

ForecastResult forecast_monthly(const AiBrainParams& params,
		const WorldState& ws, const ForecastInput& in) {
	ForecastResult out;
	const int month_days = std::max(1, params.month_days);
	const double payback_limit = std::max(1.0, in.payback_limit_days);
	int months = static_cast<int>(
		std::ceil(payback_limit / static_cast<double>(month_days)));
	months = std::clamp(months, 1, std::max(1, params.months_max));
	out.months = months;

	const double candidate = std::max(0.0, in.candidate_capacity);
	if (candidate <= 0.0) {
		return out;
	}

	// (a) capex ≤ caixa × capital_share + crédito aprovado.
	const double unit_capex = std::max(1e-9, in.unit_capex);
	const double spendable = std::max(0.0, in.cash) * params.capital_share
		+ std::max(0.0, in.credit_approved);
	out.spendable = spendable;
	const double cap_by_share = spendable / unit_capex;

	// Dimensionamento fechado: saldo_m = caixa − cap × (unit_capex + burn_m).
	// `burn_m` é avaliado na capacidade candidata (a parcela de mercado é a
	// única não linear e fica congelada no ponto de projeto).
	double max_burden = -1e300;
	for (int month = 1; month <= months; ++month) {
		const UnitMonth row = unit_month(ws, params, in, month, candidate);
		max_burden = std::max(max_burden, unit_capex + row.burn);
	}
	const double cap_by_cash = max_burden > 1e-9
		? (std::max(0.0, in.cash) + std::max(0.0, in.credit_approved))
			/ max_burden
		: candidate;
	double cap_max = std::min(candidate, std::min(cap_by_share, cap_by_cash));
	cap_max = std::max(0.0, cap_max);
	out.cap_max = cap_max;
	out.constrained = cap_max < candidate - 1e-9;
	if (cap_max < std::max(0.0, in.min_capacity)) {
		out.failed_gate = 6;
		return out;
	}

	// Forecast completo na capacidade final.
	const double capacity = cap_max;
	const double capex = capacity * unit_capex;
	out.capacity = capacity;
	out.capex = capex;
	out.min_balance = std::max(0.0, in.cash) - capex;
	out.final_balance = out.min_balance;
	double balance = out.min_balance;
	double cumulative = 0.0;
	out.trough_profit = 0.0;
	double weighted_eff = 0.0;
	int total_op_days = 0;
	double last_operating = 0.0;
	// Fase C (R5): "não nascer para hibernar". O forecast é mensal; um mês com
	// resultado operacional negativo vale todos os seus `op_days` de perda (a
	// escada D-126 conta dias CONSECUTIVOS de prejuízo). A sequência reinicia
	// no primeiro mês operacional positivo.
	int loss_days = 0;
	for (int month = 1; month <= months; ++month) {
		const MonthProd prod = month_production(ws, params, in, month);
		const double tons = prod.prod_per_capacity;
		const double total_prod = capacity * tons;
		// Mesma correção de dupla contagem de `unit_month`: o preço esperado
		// já castiga o excesso de oferta.
		const double revenue = total_prod
			* std::max(0.0, in.price_expected);
		const double fixed = static_cast<double>(prod.op_days)
			* std::max(0.0, in.jobs_per_capacity) * std::max(0.0, in.wage)
			* capacity;
		// Opex por capacidade-dia (não por tonelada produzida) + insumo sobre
		// o que foi efetivamente produzido.
		const double opex_fixed = static_cast<double>(prod.op_days)
			* std::max(0.0, in.opex_per_ton) * capacity;
		const double inputs = std::max(0.0, in.input_ratio) * total_prod
			* std::max(0.0, in.input_price);
		const double borrowed = std::max(0.0, capex - std::max(0.0, in.cash));
		const double interest = borrowed * std::max(0.0, in.loan_rate_annual)
			* static_cast<double>(month_days) / 365.0;
		const double operating =
			revenue - fixed - opex_fixed - inputs - interest;
		out.revenue_total += revenue;
		out.cost_total += fixed + opex_fixed + inputs + interest;
		balance += operating;
		out.final_balance = balance;
		out.min_balance = std::min(out.min_balance, balance);
		cumulative += operating;
		if (month <= params.trough_months_max) {
			out.trough_profit = std::min(out.trough_profit, cumulative);
		}
		if (prod.op_days > 0) {
			last_operating = operating;
			if (operating < 0.0) {
				loss_days += prod.op_days;
				out.max_loss_days = std::max(out.max_loss_days, loss_days);
			} else {
				loss_days = 0;
			}
		}
		weighted_eff += prod.mean_eff * static_cast<double>(prod.op_days);
		total_op_days += prod.op_days;
	}
	out.mean_efficiency = total_op_days > 0
		? weighted_eff / static_cast<double>(total_op_days)
		: std::clamp(in.from_efficiency, 0.0, 1.0);

	// (e) payback D-125 com o preço esperado.
	const double daily_profit = std::max(0.0, in.margin_per_ton) * capacity
		* std::max(0.0, in.expected_efficiency);
	out.payback_days = capex / std::max(daily_profit, 0.001);

	const double credit = std::max(0.0, in.credit_approved);
	const double eps = 1e-6;
	// (d) Fase C: com a crise ligada a planta não pode chegar ao degrau de
	// hibernação da escada D-126 (a reação do núcleo continua mandando; o
	// forecast só não cria a planta que já nasceria para hibernar). Com
	// `crisis.enabled=false` o degrau sai do caminho (equivalência com hoje).
	bool stairs_ok = true;
	if (ws.crisis.enabled) {
		const int hibernation = std::max(1, ws.crisis.mothball_days);
		stairs_ok = out.max_loss_days < hibernation;
	}
	const bool gate_a = capex <= spendable + eps;
	const bool gate_b = out.min_balance >= -credit - eps;
	// D-131 fase C (R5, correção): a linha de crédito de INVESTIMENTO financia
	// o vale da rampa D-125. Exigir saldo final >= 0 só com caixa tornava o
	// crédito inútil (era só teto do capex e o grupo ficava com 0 obras).
	// Agora o saldo final e o trough podem usar a linha (limite), mas o
	// projeto precisa estar em OPERAÇÃO >= 0 no último mês do horizonte: não
	// nasce para queimar crédito.
	const bool gate_c = out.final_balance >= -credit - eps
		&& last_operating >= -eps;
	const bool gate_d = out.trough_profit >= -credit - eps && stairs_ok;
	const bool gate_e = out.payback_days <= payback_limit + eps;
	if (!gate_a) {
		out.failed_gate = 1;
	} else if (!gate_b) {
		out.failed_gate = 2;
	} else if (!gate_c) {
		out.failed_gate = 3;
	} else if (!gate_d) {
		out.failed_gate = 4;
	} else if (!gate_e) {
		out.failed_gate = 5;
	}
	out.pass = out.failed_gate == 0;
	return out;
}

} // namespace ai_brain
} // namespace ogs
