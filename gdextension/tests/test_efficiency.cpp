// Testes da D-125 (eficiencia de producao): rampa, teto, equipamento,
// transicao, retencao por familia e modos da refinaria (so petroleo).
#include "doctest/doctest.h"
#include "pocket_world.h"

#include "sim/efficiency.h"
#include "sim/save_value.h"

#include <algorithm>
#include <string>

using doctest::Approx;

namespace {

ogs::Building building_with_recipe(const ogs::test::PocketWorld& world,
		const std::string& recipe_id, double efficiency) {
	ogs::Building building;
	building.id = "B_TEST";
	building.city = "PORTUS";
	building.recipe = world.recipe(recipe_id);
	building.output_product = world.ws.recipes[
		static_cast<std::size_t>(building.recipe)].output;
	building.input_product = -1;
	building.capacity = 1.0;
	building.production_efficiency = efficiency;
	building.efficiency_days = 0;
	return building;
}

// Roda `days` passos diarios e devolve a eficiencia final (mesma matematica da
// fase: passo + teto; nada e arredondado entre dias).
double run_days(const ogs::WorldState& ws, ogs::Building building,
		ogs::EfficiencyClass cls, int days, bool equipment = false) {
	const bool powered = equipment && building.equipment > 0.0;
	building.equipment_active = powered;
	const double ceiling = ogs::eff::cap(ws, building, cls);
	for (int day = 0; day < days; ++day) {
		if (building.production_efficiency < ceiling) {
			building.production_efficiency = std::min(ceiling,
				building.production_efficiency
					+ ogs::eff::step(ws, building, cls, powered));
		}
	}
	return building.production_efficiency;
}

int class_id(ogs::EfficiencyClass cls) {
	return static_cast<int>(cls);
}

} // namespace

TEST_CASE("D-125: fabrica comeca em 30% e sobe 0,6 p.p. por dia ate 90 dias") {
	ogs::test::PocketWorld world;
	ogs::Building building = building_with_recipe(world, "planks", 0.30);
	const auto cls = ogs::eff::classify(world.ws, world.city("PORTUS"), building);

	CHECK(class_id(cls) == class_id(ogs::EfficiencyClass::kFactory));
	CHECK(ogs::eff::step(world.ws, building, cls, false)
		== Approx(0.60 / 90.0));

	CHECK(ogs::eff::curve(world.ws, cls).start == Approx(0.30));
	CHECK(ogs::eff::curve(world.ws, cls).mid == Approx(0.90));
	CHECK(ogs::eff::curve(world.ws, cls).mid_days == Approx(90.0));
	CHECK(ogs::eff::curve(world.ws, cls).full_days == Approx(180.0));

	// A curva e linear em dias de operacao: 90 -> 0,90 e 180 -> 1,00.
	CHECK(run_days(world.ws, building, cls, 90) == Approx(0.90));
	CHECK(run_days(world.ws, building, cls, 180) == Approx(1.0));
	CHECK(building.production_efficiency == Approx(0.30));  // nao mutou
}

TEST_CASE("D-125: mina 25% -> 100% em 240 dias (com maquina)") {
	ogs::test::PocketWorld world;
	const int oil = world.product("oil");

	ogs::City mine_city = world.city("PORTUS");
	mine_city.deposits[oil].tracked = true;
	ogs::Building mine;
	mine.id = "MINE";
	mine.output_product = oil;
	mine.capacity = 1.0;
	mine.production_efficiency = 0.25;
	mine.equipment = 1.0;
	const auto cls = ogs::eff::classify(world.ws, mine_city, mine);

	CHECK(class_id(cls) == class_id(ogs::EfficiencyClass::kMine));
	CHECK(ogs::eff::step(world.ws, mine, cls, false)
		== Approx(0.75 / 240.0));
	CHECK(ogs::eff::step(world.ws, mine, cls, true)
		== Approx(0.75 / 240.0 * 1.5));
	CHECK(run_days(world.ws, mine, cls, 240, true) == Approx(1.0));
}

TEST_CASE("D-125: fazenda 20% -> 100% em 365 dias (com trator)") {
	ogs::test::PocketWorld world;
	ogs::Building farm;
	farm.id = "FARM";
	farm.output_product = world.product("wheat");
	farm.capacity = 1.0;
	farm.production_efficiency = 0.20;
	const auto cls = ogs::eff::classify(world.ws, world.city("AGRA"), farm);

	CHECK(class_id(cls) == class_id(ogs::EfficiencyClass::kFarm));
	CHECK(ogs::eff::step(world.ws, farm, cls, false)
		== Approx(0.80 / 365.0));
	// Sem equipamento o teto da lavoura e `cap_without` (85%).
	CHECK(run_days(world.ws, farm, cls, 365, false) == Approx(0.85));
	// Com trator, a curva inteira: 100% em 365 dias.
	farm.equipment = 1.0;
	CHECK(run_days(world.ws, farm, cls, 365, true) == Approx(1.0));
}

TEST_CASE("D-125: pecuaria (carne) tem a rampa mais lenta, 540 dias") {
	ogs::test::PocketWorld world;
	ogs::Building ranch;
	ranch.id = "RANCH";
	ranch.output_product = world.product("meat");
	ranch.capacity = 1.0;
	ranch.production_efficiency = 0.20;
	const auto cls = ogs::eff::classify(world.ws, world.city("AGRA"), ranch);

	CHECK(class_id(cls) == class_id(ogs::EfficiencyClass::kRanch));
	CHECK(ogs::eff::curve(world.ws, cls).full_days == Approx(540.0));
	CHECK(ogs::eff::step(world.ws, ranch, cls, false)
		== Approx(0.80 / 540.0));
	CHECK(ogs::eff::cap(world.ws, ranch, cls) == Approx(0.85));
}

TEST_CASE("D-125: teto sem equipamento (85%) e com equipamento (100%)") {
	ogs::test::PocketWorld world;
	ogs::Building factory = building_with_recipe(world, "planks", 0.9);
	CHECK(ogs::eff::cap(world.ws, factory,
		ogs::EfficiencyClass::kFactory) == Approx(1.0));

	ogs::Building farm;
	farm.output_product = world.product("wheat");
	const auto cls = ogs::EfficiencyClass::kFarm;
	farm.equipment = 1.0;
	farm.equipment_active = false;
	CHECK(ogs::eff::cap(world.ws, farm, cls) == Approx(0.85));
	farm.equipment_active = true;
	CHECK(ogs::eff::cap(world.ws, farm, cls) == Approx(1.0));
}

TEST_CASE("D-125: equipamento acelera o passo em 1,5x") {
	ogs::test::PocketWorld world;
	ogs::Building farm;
	farm.output_product = world.product("wheat");
	farm.production_efficiency = 0.20;
	const auto cls = ogs::EfficiencyClass::kFarm;

	const double base = ogs::eff::step(world.ws, farm, cls, false);
	farm.equipment = 1.0;
	const double powered = ogs::eff::step(world.ws, farm, cls, true);
	CHECK(powered == Approx(base * 1.5));
	farm.equipment_active = false;
	CHECK(ogs::eff::step(world.ws, farm, cls, false) == Approx(base));
}

TEST_CASE("D-125: eficiencia media esperada de uma fabrica nova em 180 dias") {
	ogs::test::PocketWorld world;
	const auto cls = ogs::EfficiencyClass::kFactory;
	// Media da rampa 0,30->0,90->1,00, contando a eficiencia DEPOIS do passo.
	const double expected = (90.0 * (0.30 + 0.60 / 90.0 + 0.90) / 2.0
		+ 90.0 * (0.90 + 0.10 / 90.0 + 1.0) / 2.0) / 180.0;
	CHECK(ogs::eff::mean_efficiency(world.ws, cls, 0.30, false, 180)
		== Approx(expected));
	CHECK(ogs::eff::mean_efficiency(world.ws, cls, 0.0, false, 0)
		== Approx(0.0));
}

TEST_CASE("D-125: dias de transicao nao produzem nem maturam") {
	ogs::test::PocketWorld world;
	const auto cls = ogs::EfficiencyClass::kFactory;
	const double running = ogs::eff::mean_efficiency(world.ws, cls, 0.30,
		false, 180);
	const double idle = ogs::eff::mean_efficiency(world.ws, cls, 0.30,
		false, 180, 7);
	CHECK(idle < running);
	CHECK(idle > 0.0);
	// 7 dias parados no total de 7: media zero.
	CHECK(ogs::eff::mean_efficiency(world.ws, cls, 0.30, false, 7, 7)
		== Approx(0.0));
}

TEST_CASE("D-125: dias de transicao por setor e refinaria") {
	ogs::test::PocketWorld world;
	ogs::Building factory = building_with_recipe(world, "planks", 0.9);
	ogs::Building refinery = building_with_recipe(world, "gasoline_a", 0.9);

	const ogs::City& portus = world.city("PORTUS");
	CHECK(ogs::eff::transition_days(world.ws, portus, factory,
		world.product("planks")) == 7);
	CHECK(ogs::eff::transition_days(world.ws, portus, factory,
		world.product("ammo")) == 14);
	CHECK(ogs::eff::transition_days(world.ws, portus, factory,
		world.product("chip")) == 30);
	CHECK(ogs::eff::transition_days(world.ws, portus, refinery,
		world.product("planks")) == 7);

	ogs::City mine_city = portus;
	const int oil = world.product("oil");
	mine_city.deposits[oil].tracked = true;
	ogs::Building mine;
	mine.output_product = oil;
	// Mina nao troca de produto: a jazida manda.
	CHECK(ogs::eff::transition_days(world.ws, mine_city, mine, oil) == -1);
}

TEST_CASE("D-125: retencao usa a familia (goods.json.sector)") {
	ogs::test::PocketWorld world;
	CHECK(ogs::eff::same_family(world.ws, world.product("planks"),
		world.product("sawdust")));
	CHECK_FALSE(ogs::eff::same_family(world.ws, world.product("planks"),
		world.product("gasoline")));
	CHECK_FALSE(ogs::eff::same_family(world.ws, -1, world.product("planks")));
	CHECK(world.ws.efficiency.retention_same_family == Approx(0.60));
	CHECK(world.ws.efficiency.retention_other == Approx(0.0));
}

TEST_CASE("D-125: refinaria e so o refino de petroleo (gasolina + insumo oil)") {
	ogs::test::PocketWorld world;
	const ogs::City& portus = world.city("PORTUS");

	ogs::Building petroleum = building_with_recipe(world, "gasoline_a", 0.9);
	ogs::Building ethanol = building_with_recipe(world, "gasoline_b", 0.9);
	ogs::Building factory = building_with_recipe(world, "chip", 0.9);

	CHECK(ogs::eff::is_refinery(world.ws, petroleum));
	CHECK_FALSE(ogs::eff::is_refinery(world.ws, ethanol));
	CHECK_FALSE(ogs::eff::is_refinery(world.ws, factory));
	CHECK(class_id(ogs::eff::classify(world.ws, portus, petroleum))
		== class_id(ogs::EfficiencyClass::kRefinery));
	CHECK(class_id(ogs::eff::classify(world.ws, portus, ethanol))
		== class_id(ogs::EfficiencyClass::kFactory));
}

TEST_CASE("D-125: modos da refinaria trocam as fracoes do lote de petroleo") {
	ogs::test::PocketWorld world;
	const ogs::City& portus = world.city("PORTUS");
	ogs::Building building = building_with_recipe(world, "gasoline_a", 0.9);
	const ogs::Recipe& recipe = world.ws.recipes[
		static_cast<std::size_t>(building.recipe)];

	// Modo padrao (0): o dado da receita vale (45/29/9).
	building.refinery_mode = 0;
	CHECK(ogs::eff::main_yield(world.ws, building, recipe) == Approx(0.45));
	CHECK(ogs::eff::byproduct_yield(world.ws, building, recipe, 0)
		== Approx(0.29));
	CHECK(ogs::eff::byproduct_yield(world.ws, building, recipe, 1)
		== Approx(0.09));

	// Foco diesel (1): 38/36/9.
	building.refinery_mode = 1;
	CHECK(ogs::eff::main_yield(world.ws, building, recipe) == Approx(0.38));
	CHECK(ogs::eff::byproduct_yield(world.ws, building, recipe, 0)
		== Approx(0.36));
	CHECK(ogs::eff::byproduct_yield(world.ws, building, recipe, 1)
		== Approx(0.09));

	// Foco querosene (2): 40/28/15.
	building.refinery_mode = 2;
	CHECK(ogs::eff::main_yield(world.ws, building, recipe) == Approx(0.40));
	CHECK(ogs::eff::byproduct_yield(world.ws, building, recipe, 0)
		== Approx(0.28));
	CHECK(ogs::eff::byproduct_yield(world.ws, building, recipe, 1)
		== Approx(0.15));
	CHECK(class_id(ogs::eff::classify(world.ws, portus, building))
		== class_id(ogs::EfficiencyClass::kRefinery));

	// Receita que NAO e refino de petroleo recusa o modo (D-125 pos-QA).
	ogs::Building ethanol = building_with_recipe(world, "gasoline_b", 0.9);
	ethanol.refinery_mode = 2;
	const ogs::Recipe& ethanol_recipe = world.ws.recipes[
		static_cast<std::size_t>(ethanol.recipe)];
	CHECK(ogs::eff::main_yield(world.ws, ethanol, ethanol_recipe)
		== Approx(0.75));
}

TEST_CASE("D-125: efficiency.json do dado espelha os defaults C++") {
	ogs::save::Value definitions;
	std::string error;
	const std::string path = std::string(OPENGS_REPO_DIR)
		+ "/data/definitions/production/efficiency.json";
	REQUIRE(ogs::save::parse_json_file(path, definitions, error));

	ogs::WorldState defaults;  // parametros default do nucleo
	const auto read = [&definitions](const char* block, const char* key) {
		return double(definitions.get(block, ogs::save::Value())
			.get(key, ogs::save::Value()));
	};
	CHECK(read("factory", "start") == Approx(defaults.efficiency.factory.start));
	CHECK(read("factory", "mid") == Approx(defaults.efficiency.factory.mid));
	CHECK(read("factory", "mid_days")
		== Approx(defaults.efficiency.factory.mid_days));
	CHECK(read("factory", "full_days")
		== Approx(defaults.efficiency.factory.full_days));
	CHECK(read("mine", "start") == Approx(defaults.efficiency.mine.start));
	CHECK(read("mine", "full_days")
		== Approx(defaults.efficiency.mine.full_days));
	CHECK(read("farm", "start") == Approx(defaults.efficiency.farm.start));
	CHECK(read("farm", "full_days")
		== Approx(defaults.efficiency.farm.full_days));
	CHECK(read("ranch", "full_days")
		== Approx(defaults.efficiency.ranch.full_days));
	CHECK(read("equipment", "cap_without")
		== Approx(defaults.efficiency.equipment_cap_without));
	CHECK(read("equipment", "rate_boost")
		== Approx(defaults.efficiency.equipment_rate_boost));
	CHECK(read("retention", "same_family")
		== Approx(defaults.efficiency.retention_same_family));
	CHECK(read("retention", "other")
		== Approx(defaults.efficiency.retention_other));
}
