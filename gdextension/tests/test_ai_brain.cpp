// D-131 (fase A): cerebro economico da IA — previsao de caixa mensal (R1),
// preco esperado (R3), media da rampa e a garantia de que `enabled=false` nao
// altera nenhuma decisao.
//
// Mundo de bolso (`pocket_world`): os numeros sao de teste, nao de simulacao.
#include "doctest/doctest.h"
#include "pocket_world.h"

#include "sim/ai/ai_phase.h"
#include "sim/ai/ai_private.h"
#include "sim/ai/ai_templates.h"
#include "sim/ai/ai_view.h"
#include "sim/ai_brain.h"
#include "sim/commands.h"
#include "sim/efficiency.h"
#include "sim/save_definitions.h"
#include "sim/save_value.h"
#include "sim/sim_constants.h"
#include "sim/sim_core.h"
#include "sim/sim_day.h"

#include <cmath>
#include <map>
#include <string>
#include <vector>

using doctest::Approx;

namespace {

ogs::ai_brain::ForecastInput factory_input(const ogs::test::PocketWorld& world,
		double capacity) {
	ogs::ai_brain::ForecastInput in;
	in.candidate_capacity = capacity;
	in.efficiency_class = ogs::EfficiencyClass::kFactory;
	in.from_efficiency = ogs::eff::curve(world.ws, in.efficiency_class).start;
	in.construction_days = 0;
	in.aptitude = 1.0;
	in.stage_factor = 1.0;
	in.area_scale = 1.0;
	in.jobs_per_capacity = 0.0;
	in.wage = 0.0;
	in.opex_per_ton = 0.0;
	in.input_ratio = 0.0;
	in.input_price = 0.0;
	in.price_expected = 100.0;
	in.supply_future = 0.0;
	in.demand_future = 100.0;
	in.loan_rate_annual = 0.0;
	in.margin_per_ton = 100.0;
	in.expected_efficiency = 0.5;
	in.payback_limit_days = 150.0;
	in.min_capacity = 2.0;
	return in;
}

// Log de comandos em forma comparavel (tipo + strings + doubles).
std::vector<std::string> command_lines(const ogs::CommandQueue& queue) {
	std::vector<std::string> lines;
	for (const ogs::Command& cmd : queue.log_entries()) {
		std::string line = ogs::command_type_name(cmd.type);
		for (const std::string& text : cmd.s) {
			line += "|" + text;
		}
		for (double value : cmd.v) {
			line += "|" + std::to_string(value);
		}
		lines.push_back(line);
	}
	return lines;
}

} // namespace

TEST_CASE("D-131: mean_between e o mesmo laco diario de mean_efficiency") {
	ogs::test::PocketWorld world;
	const auto cls = ogs::EfficiencyClass::kFactory;
	const double from = ogs::eff::curve(world.ws, cls).start;
	CHECK(ogs::eff::mean_between(world.ws, cls, from, false, 0, 89)
		== Approx(ogs::eff::mean_efficiency(world.ws, cls, from, false, 90)));
	CHECK(ogs::eff::mean_between(world.ws, cls, from, false, 30, 59)
		== Approx(ogs::eff::mean_between(world.ws, cls, from, false, 0, 59)
			* 2.0
			- ogs::eff::mean_between(world.ws, cls, from, false, 0, 29)));
	// Intervalo invertido = ausencia declarada (a eficiencia de partida).
	CHECK(ogs::eff::mean_between(world.ws, cls, from, false, 10, 5)
		== Approx(from));
}

TEST_CASE("D-131: vale de caixa rejeita investimento que quebraria") {
	ogs::test::PocketWorld world;
	ogs::AiBrainParams params;
	// Isola o gate (b): a alca (a) nao limita antes.
	params.capital_share = 1.0;
	ogs::ai_brain::ForecastInput in = factory_input(world, 4.0);
	in.cash = 5000.0;
	in.credit_approved = 0.0;
	in.unit_capex = 2000.0;
	in.construction_days = 30;
	in.price_expected = 0.0;
	in.margin_per_ton = 0.0;
	in.expected_efficiency = 0.0;
	in.payback_limit_days = 1000000.0;  // nao deixa o gate (e) decidir
	// Folha fixa ~500 por unidade de capacidade/mes (sem opex/insumo).
	in.jobs_per_capacity = 1.0;
	in.wage = 500.0 / 30.0;
	const ogs::ai_brain::ForecastResult result =
		ogs::ai_brain::forecast_monthly(params, world.ws, in);
	CHECK_FALSE(result.pass);
	CHECK(result.capacity >= 2.0);
	CHECK(result.capex <= 5000.0 + 1e-6);
	CHECK(result.min_balance < 0.0);
}

TEST_CASE("D-131: dimensionamento reduz a capacidade para caber no caixa") {
	ogs::test::PocketWorld world;
	ogs::AiBrainParams params;  // capital_share = 0.35
	ogs::ai_brain::ForecastInput in = factory_input(world, 20.0);
	in.cash = 10000.0;
	in.credit_approved = 0.0;
	in.unit_capex = 1000.0;
	in.construction_days = 0;
	const ogs::ai_brain::ForecastResult result =
		ogs::ai_brain::forecast_monthly(params, world.ws, in);
	CHECK(result.pass);
	CHECK(result.constrained);
	CHECK(result.capacity < 20.0);
	CHECK(result.capacity == Approx(3.5));
	CHECK(result.capex <= 10000.0 * 0.35 + 1e-6);
	CHECK(result.final_balance >= 0.0);
}

TEST_CASE("D-131: preco esperado cai com excesso previsto") {
	ogs::AiBrainParams params;
	const double balanced =
		ogs::ai_brain::expected_price(params, 10.0, 1.0, 10.0, 10.0);
	const double excess =
		ogs::ai_brain::expected_price(params, 10.0, 1.0, 20.0, 10.0);
	CHECK(excess < balanced);
	CHECK(excess == Approx(10.0 * (1.0 - params.elasticity)));
	// Piso declarado: excesso gigante nao leva o preco abaixo do piso.
	const double floored =
		ogs::ai_brain::expected_price(params, 10.0, 1.0, 1000.0, 10.0);
	CHECK(floored == Approx(10.0 * params.price_floor));
}

TEST_CASE("D-131: tendencia de preco limitada a [0,5; 2,0]") {
	ogs::AiBrainParams params;
	std::vector<double> rising;
	for (int index = 1; index <= 60; ++index) {
		rising.push_back(static_cast<double>(index));
	}
	std::vector<double> falling(rising.rbegin(), rising.rend());
	CHECK(ogs::ai_brain::price_trend(params, rising)
		== Approx(params.trend_max));
	CHECK(ogs::ai_brain::price_trend(params, falling)
		== Approx(params.trend_min));
	// Menos de 5 observacoes: ausencia declarada (1.0).
	std::vector<double> short_series{1.0, 2.0, 3.0, 4.0};
	CHECK(ogs::ai_brain::price_trend(params, short_series) == Approx(1.0));
}

TEST_CASE("D-131: save v42 aditivo preserva ai_brain e v41 carrega defaults") {
	ogs::test::PocketWorld source;
	std::map<std::string, double> changed;
	changed["enabled"] = 1.0;
	changed["price.elasticity"] = 0.50;
	changed["forecast.month_days"] = 30.0;
	ogs::ai_brain::apply_params(source.ws, changed);
	const ogs::save::Value definitions = ogs::save::definitions_to(source.ws);
	CHECK(ogs::kSaveVersion == 42);

	ogs::test::PocketWorld loaded;
	ogs::save::definitions_from(loaded.ws, definitions);
	const auto source_params = ogs::ai_brain::params_map(source.ws);
	const auto loaded_params = ogs::ai_brain::params_map(loaded.ws);
	CHECK(loaded.ws.ai_brain.enabled);
	CHECK(loaded_params.at("price.elasticity")
		== Approx(source_params.at("price.elasticity")));
	CHECK(loaded_params.at("forecast.month_days")
		== Approx(source_params.at("forecast.month_days")));

	// Save v41 (sem o bloco) mantem o default do struct: desligado.
	ogs::test::PocketWorld old;
	ogs::save::Value old_definitions = ogs::save::Value::make_object();
	ogs::save::definitions_from(old.ws, old_definitions);
	CHECK_FALSE(old.ws.ai_brain.enabled);
}

TEST_CASE("D-131: enabled=false nao altera as decisoes da IA") {
	ogs::test::PocketWorld first;
	ogs::test::PocketWorld second;
	// Parametros extremos no segundo mundo: se o gate/preco rodassem com a
	// flag desligada, as decisoes mudariam.
	std::map<std::string, double> extreme;
	extreme["enabled"] = 0.0;
	extreme["forecast.capital_share"] = 0.0;
	extreme["price.elasticity"] = 100.0;
	extreme["price.price_floor"] = 0.01;
	ogs::ai_brain::apply_params(second.ws, extreme);

	const auto run = [](ogs::test::PocketWorld& world) {
		// Semeia uma escassez de tabuas em AGRA (o mesmo estado nos dois
		// mundos) para a IA ter o que decidir: demanda, insumo em estoque e
		// caixa da firma.
		const int planks = world.product("planks");
		const int timber = world.product("timber");
		ogs::City& agra = world.ws.cities.front();
		agra.firm_cash = 200000.0;
		agra.markets[planks].price = 60.0;
		agra.markets[planks].desired = 10.0;
		agra.markets[planks].shortage = 10.0;
		agra.markets[timber].stock = 100.0;
		ogs::AiTemplates templates;
		ogs::build_ai_templates(world.ws, templates);
		ogs::AiView view;
		ogs::snapshot_ai_view(world.ws, world.core, view);
		ogs::CommandQueue queue;
		ogs::AiState state;
		ogs::AiContext ctx{world.ws, world.core, queue, templates, view, state};
		ogs::AiPrivateInvestor investor;
		investor.prepare(ctx);
		for (int city = 0; city < view.cities; ++city) {
			investor.run_city(ctx, city);
		}
		return command_lines(queue);
	};
	CHECK_FALSE(second.ws.ai_brain.enabled);
	const std::vector<std::string> first_lines = run(first);
	const std::vector<std::string> second_lines = run(second);
	CHECK_FALSE(first_lines.empty());
	CHECK(first_lines == second_lines);
}
