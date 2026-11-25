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
	const int op_begin = std::max(month_begin, std::max(0, in.construction_days));
	if (month_end < op_begin) {
		return out;
	}
	out.op_days = month_end - op_begin + 1;
	out.mean_eff = eff::mean_between(ws, in.efficiency_class,
		in.from_efficiency, in.equipment_active, op_begin, month_end);
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
	const double tons = prod.prod_per_capacity;
	const double total_prod = capacity * tons;
	const double share = total_prod + in.supply_future > 1e-9
		? total_prod / (in.supply_future + total_prod) : 0.0;
	const double revenue = total_prod * share * std::max(0.0, in.price_expected);
	const double fixed = static_cast<double>(prod.op_days)
		* std::max(0.0, in.jobs_per_capacity) * std::max(0.0, in.wage);
	const double variable = static_cast<double>(prod.op_days)
			* std::max(0.0, in.opex_per_ton) * tons
		+ std::max(0.0, in.input_ratio) * tons
			* std::max(0.0, in.input_price);
	// Juro do capital emprestado (o crédito aprovado cobre o que o caixa não
	// paga). Taxa anual / 365 × dias do mês.
	const double borrowed = std::max(0.0, capacity * in.unit_capex - in.cash);
	out.burn = fixed + variable
		+ borrowed / std::max(1.0, capacity)
			* std::max(0.0, in.loan_rate_annual)
			* static_cast<double>(std::max(1, params.month_days)) / 365.0
		- revenue;
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
}

std::map<std::string, double> params_map(const WorldState& ws) {
	const AiBrainParams& p = ws.ai_brain;
	return {
		{"enabled", p.enabled ? 1.0 : 0.0},
		{"forecast.month_days", double(p.month_days)},
		{"forecast.months_max", double(p.months_max)},
		{"forecast.trough_months_max", double(p.trough_months_max)},
		{"forecast.capital_share", p.capital_share},
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
	};
}

double price_trend(const AiBrainParams& params,
		const std::vector<double>& history) {
	const std::size_t count = history.size();
	// Ausência declarada: sem 5 observações não há tendência (1.0).
	if (count < 5) {
		return 1.0;
	}
	const std::size_t window = std::min<std::size_t>(
		static_cast<std::size_t>(std::max(1, params.trend_days)), count / 2);
	if (window < 2) {
		return 1.0;
	}
	const double current = mean_of(history, count - window, count);
	const double previous = mean_of(history, count - 2 * window,
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
	for (int month = 1; month <= months; ++month) {
		const MonthProd prod = month_production(ws, params, in, month);
		const double tons = prod.prod_per_capacity;
		const double total_prod = capacity * tons;
		const double share = total_prod + in.supply_future > 1e-9
			? total_prod / (in.supply_future + total_prod) : 0.0;
		const double revenue = total_prod * share
			* std::max(0.0, in.price_expected);
		const double fixed = static_cast<double>(prod.op_days)
			* std::max(0.0, in.jobs_per_capacity) * std::max(0.0, in.wage)
			* capacity;
		const double variable = static_cast<double>(prod.op_days)
				* std::max(0.0, in.opex_per_ton) * total_prod
			+ std::max(0.0, in.input_ratio) * total_prod
				* std::max(0.0, in.input_price);
		const double borrowed = std::max(0.0, capex - std::max(0.0, in.cash));
		const double interest = borrowed * std::max(0.0, in.loan_rate_annual)
			* static_cast<double>(month_days) / 365.0;
		const double operating = revenue - fixed - variable - interest;
		balance += operating;
		out.final_balance = balance;
		out.min_balance = std::min(out.min_balance, balance);
		cumulative += operating;
		if (month <= params.trough_months_max) {
			out.trough_profit = std::min(out.trough_profit, cumulative);
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
	out.pass = capex <= spendable + eps
		&& out.min_balance >= -credit - eps
		&& out.final_balance >= -eps
		&& out.trough_profit >= -eps
		&& out.payback_days <= payback_limit + eps;
	return out;
}

} // namespace ai_brain
} // namespace ogs
