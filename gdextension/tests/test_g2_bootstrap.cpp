// G2 — bootstrap industrial (spec G2_bootstrap_industrial.md §2) e demanda
// potencial no cerebro (SS3). Numeros de teste; nenhum dado de simulacao.
#include "doctest/doctest.h"
#include "pocket_world.h"

#include "sim/ai/ai_view.h"
#include "sim/ai_brain.h"
#include "sim/bootstrap.h"
#include "sim/potential_demand.h"

#include <cmath>
#include <string>
#include <vector>

using doctest::Approx;

namespace {

int add_recipe(ogs::WorldState& ws, const std::string& id, int output,
		double yield, const std::vector<int>& inputs) {
	ogs::Recipe recipe;
	recipe.id = id;
	recipe.output = output;
	recipe.yield = yield;
	recipe.input_count = static_cast<int>(inputs.size());
	for (std::size_t slot = 0; slot < inputs.size(); ++slot) {
		recipe.inputs[slot] = inputs[slot];
		recipe.input_qty[slot] = 1.0;
	}
	recipe.capital_per_capacity = 1000.0;
	recipe.jobs_per_ton = 1.0;
	ws.recipes.push_back(recipe);
	return static_cast<int>(ws.recipes.size()) - 1;
}

void add_category(ogs::WorldState& ws, double per_capita_day,
		int substitute) {
	ogs::NeedsLevel level;
	level.id = "basic";
	ogs::NeedsCategory category;
	category.id = "test";
	category.per_capita_day = per_capita_day;
	category.substitutes.push_back(substitute);
	category.equivalence.push_back(1.0);
	level.categories.push_back(category);
	ws.needs_levels.push_back(level);
}

// Mundo sintetico pequeno para a propagacao pura.
ogs::WorldState demand_world() {
	ogs::WorldState ws;
	ws.products = 5;
	ws.product_names = {"mid", "final", "raw", "other_final", "other_mid"};
	add_recipe(ws, "final", 1, 1.0, {0});
	add_recipe(ws, "mid", 0, 1.0, {2});
	add_recipe(ws, "other_final", 3, 1.0, {4, 4, 4, 4});
	add_recipe(ws, "other_mid", 4, 1.0, {2});
	add_category(ws, 1e-6, 1);
	add_category(ws, 1e-6, 3);
	ws.needs_loaded = true;
	ws.durable_decay = 0.0;
	ws.cities.resize(1);
	ws.cities[0].id = "C1";
	ws.cities[0].nation = "AAA";
	ws.cities[0].simulation_population = 1000000.0;
	return ws;
}

} // namespace

TEST_CASE("G2: potencial propaga com desconto 0,5 por nivel") {
	ogs::WorldState ws = demand_world();
	ogs::AiBrainParams params;
	params.potential_discount = 0.5;
	params.potential_cap = 1.0;
	ogs::PotentialDemandIndex index;
	ogs::build_potential_demand_index(ws, index);
	std::vector<double> potential;
	ogs::potential_demand_for_city(ws, index, 0, params, potential);
	CHECK(potential[1] == Approx(1.0));   // final = ancora
	CHECK(potential[0] == Approx(0.5));   // um nivel abaixo x 0,5
	CHECK(potential[2] == Approx(0.0));   // sem receita: nao recebe potencial
}

TEST_CASE("G2: potencial respeita cap como piso da ancora final") {
	ogs::WorldState ws = demand_world();
	// other_mid tambem tem demanda final propria (1,0): o cap usa a ancora
	// DELE, nao a do bem a jusante.
	add_category(ws, 1e-6, 4);
	ogs::AiBrainParams params;
	params.potential_discount = 0.5;
	params.potential_cap = 1.0;
	ogs::PotentialDemandIndex index;
	ogs::build_potential_demand_index(ws, index);
	std::vector<double> potential;
	ogs::potential_demand_for_city(ws, index, 0, params, potential);
	// other_mid receberia 1 x 4 x 0,5 = 2, mas cap=1,0 x ancora(1) = 1.
	CHECK(potential[4] == Approx(1.0));
}

TEST_CASE("G2: desconto 0 zera a propagacao para tras") {
	ogs::WorldState ws = demand_world();
	ogs::AiBrainParams params;
	params.potential_discount = 0.0;
	ogs::PotentialDemandIndex index;
	ogs::build_potential_demand_index(ws, index);
	std::vector<double> potential;
	ogs::potential_demand_for_city(ws, index, 0, params, potential);
	CHECK(potential[1] == Approx(1.0));
	CHECK(potential[0] == Approx(0.0));
}

TEST_CASE("G2: piso nao soma sobre o desejo vivo e nao toca o mercado real") {
	ogs::test::PocketWorld world;
	const int chip = world.product("chip");
	REQUIRE(chip >= 0);
	world.ws.ai_brain.enabled = true;
	world.ws.ai_brain.potential_enabled = true;
	world.ws.ai_brain.potential_discount = 0.5;
	world.ws.ai_brain.potential_cap = 1.0;
	add_category(world.ws, 1e-6, chip);
	world.ws.needs_loaded = true;
	const double live_before =
		world.ws.cities[0].markets[chip].desired;
	ogs::AiView view;
	ogs::snapshot_ai_view(world.ws, world.core, view);
	// O mercado real nao muda; a view recebe o piso (potencial > desejo vivo).
	CHECK(world.ws.cities[0].markets[chip].desired == live_before);
	const double view_desired = view.markets[chip].desired;
	CHECK(view_desired >= live_before);
	CHECK(view.markets[chip].shortage
		>= world.ws.cities[0].markets[chip].shortage);
	// Piso, nao soma: desejado vivo acima do potencial nao ganha nada.
	world.ws.cities[0].markets[chip].desired = view_desired + 1000.0;
	ogs::AiView second;
	ogs::snapshot_ai_view(world.ws, world.core, second);
	CHECK(second.markets[chip].desired
		== world.ws.cities[0].markets[chip].desired);
}

TEST_CASE("G2: piso desligado nao altera a view nem os totais") {
	ogs::test::PocketWorld off_brain;
	off_brain.ws.ai_brain.enabled = false;
	off_brain.ws.ai_brain.potential_enabled = true;
	add_category(off_brain.ws, 1e-6, off_brain.product("chip"));
	off_brain.ws.needs_loaded = true;
	ogs::AiView view_a;
	ogs::snapshot_ai_view(off_brain.ws, off_brain.core, view_a);

	ogs::test::PocketWorld off_potential;
	off_potential.ws.ai_brain.enabled = true;
	off_potential.ws.ai_brain.potential_enabled = false;
	add_category(off_potential.ws, 1e-6, off_potential.product("chip"));
	off_potential.ws.needs_loaded = true;
	ogs::AiView view_b;
	ogs::snapshot_ai_view(off_potential.ws, off_potential.core, view_b);

	REQUIRE(view_a.tot_desired.size() == view_b.tot_desired.size());
	REQUIRE(view_a.markets.size() == view_b.markets.size());
	for (std::size_t at = 0; at < view_a.markets.size(); ++at) {
		CHECK(view_a.markets[at].desired
			== doctest::Approx(view_b.markets[at].desired));
		CHECK(view_a.markets[at].shortage
			== doctest::Approx(view_b.markets[at].shortage));
	}
	for (std::size_t product = 0; product < view_a.tot_desired.size();
			++product) {
		CHECK(view_a.tot_desired[product]
			== doctest::Approx(view_b.tot_desired[product]));
	}
}

TEST_CASE("G2: semeadura deterministica (duas cargas = mesmo plano)") {
	ogs::WorldState ws = demand_world();
	ws.bootstrap.enabled = true;
	ws.bootstrap.seed_fraction = 0.10;
	ws.bootstrap.capacity_min = 0.05;
	ws.bootstrap.capacity_max = 20.0;
	ws.bootstrap.budget_share = 0.5;
	ws.bootstrap.score_inputs = {"raw"};
	ogs::BootstrapGood good;
	good.id = "mid";
	good.producers.push_back({"AAA", 0.60});
	good.producers.push_back({"BBB", 0.40});
	ws.bootstrap.goods.push_back(good);
	ws.cities[0].government_budget = 1000000.0;
	ws.cities[0].suitability[2] = 1.0;
	// Ancora pequena para nao estourar o teto da planta.
	ws.needs_levels[0].categories[0].per_capita_day = 1e-9;

	const ogs::bootstrap::Plan first = ogs::bootstrap::plan(ws);
	const ogs::bootstrap::Plan second = ogs::bootstrap::plan(ws);
	REQUIRE(first.rows.size() == second.rows.size());
	CHECK(first.seeded == second.seeded);
	CHECK(first.shortfall == Approx(second.shortfall));
	CHECK(first.unseeded_share == Approx(second.unseeded_share));
	for (std::size_t index = 0; index < first.rows.size(); ++index) {
		CHECK(first.rows[index].good == second.rows[index].good);
		CHECK(first.rows[index].nation == second.rows[index].nation);
		CHECK(first.rows[index].city == second.rows[index].city);
		CHECK(first.rows[index].capacity
			== doctest::Approx(second.rows[index].capacity));
		CHECK(first.rows[index].recipe == second.rows[index].recipe);
		CHECK(first.rows[index].reason == second.rows[index].reason);
	}
}
