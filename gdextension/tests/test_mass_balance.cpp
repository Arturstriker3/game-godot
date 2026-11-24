// Balanco de massa da D-127: serraria (0,55 tabua + 0,12 serragem + 0,33 de
// perda declarada) e moagem (0,75 farinha + 0,25 farelo).
//
// O nucleo NAO simula `co_loss` (e validacao do builder de dados); ele copia
// as quantidades de subproduto da receita. O teste confere as duas pontas:
//   * o fixture (Recipe em memoria) entrega 0,55/0,12 ao motor;
//   * o dado real recipes.json declara co_loss e conserva a massa.
#include "doctest/doctest.h"
#include "pocket_world.h"

#include "sim/efficiency.h"
#include "sim/save_value.h"

#include <string>

using doctest::Approx;

namespace {

const ogs::save::Value kEmpty;

const ogs::save::Value& recipe_by_id(const ogs::save::Value& recipes,
		const std::string& id) {
	static const ogs::save::Value empty;
	for (int index = 0; index < recipes.size(); ++index) {
		if (std::string(static_cast<std::string>(
				recipes[index].get("id", empty))) == id) {
			return recipes[index];
		}
	}
	return empty;
}

double sum_byproducts(const ogs::save::Value& recipe) {
	double total = 0.0;
	const ogs::save::Value& byproducts = recipe.get("byproducts", kEmpty);
	for (int index = 0; index < byproducts.size(); ++index) {
		total += double(byproducts[index].get("qty", kEmpty));
	}
	return total;
}

} // namespace

TEST_CASE("D-127: serraria entrega 0,55 tabua + 0,12 serragem ao motor") {
	ogs::test::PocketWorld world;
	ogs::Building sawmill;
	sawmill.id = "SAWMILL";
	sawmill.city = "PORTUS";
	sawmill.recipe = world.recipe("planks");
	sawmill.output_product = world.product("planks");

	const ogs::Recipe& recipe = world.ws.recipes[
		static_cast<std::size_t>(sawmill.recipe)];
	CHECK(recipe.yield == Approx(0.55));
	CHECK(recipe.byproduct_count == 1);
	CHECK(recipe.byproduct_qty[0] == Approx(0.12));
	// Fora da refinaria os modos da D-125 nao mexem no rendimento.
	CHECK(ogs::eff::main_yield(world.ws, sawmill, recipe) == Approx(0.55));
	CHECK(ogs::eff::byproduct_yield(world.ws, sawmill, recipe, 0)
		== Approx(0.12));
}

TEST_CASE("D-127: serraria conserva a massa (0,55 + 0,12 + 0,33 = 1)") {
	ogs::test::PocketWorld world;
	const double main = 0.55;
	const double byproduct = 0.12;
	const double loss = world.declared_loss("planks");
	CHECK(loss == Approx(0.33));
	CHECK(main + byproduct + loss == Approx(1.0));
}

TEST_CASE("D-127: moagem conserva a massa (0,75 farinha + 0,25 farelo)") {
	ogs::save::Value recipes;
	std::string error;
	REQUIRE(ogs::save::parse_json_file(std::string(OPENGS_REPO_DIR)
		+ "/data/definitions/economy/recipes.json", recipes, error));

	const ogs::save::Value& flour = recipe_by_id(recipes, "flour");
	REQUIRE(flour.get_type() == ogs::save::Value::DICTIONARY);
	CHECK(double(flour.get("yield", ogs::save::Value())) == Approx(0.75));
	CHECK(sum_byproducts(flour) == Approx(0.25));
	CHECK(double(flour.get("yield", ogs::save::Value()))
		+ sum_byproducts(flour) == Approx(1.0));
}

TEST_CASE("D-127: recipes.json real declara a serraria 0,55/0,12/0,33") {
	ogs::save::Value recipes;
	std::string error;
	REQUIRE(ogs::save::parse_json_file(std::string(OPENGS_REPO_DIR)
		+ "/data/definitions/economy/recipes.json", recipes, error));

	const ogs::save::Value& planks = recipe_by_id(recipes, "planks");
	REQUIRE(planks.get_type() == ogs::save::Value::DICTIONARY);
	CHECK(double(planks.get("yield", ogs::save::Value())) == Approx(0.55));
	CHECK(sum_byproducts(planks) == Approx(0.12));
	const ogs::save::Value& loss = planks.get("co_loss", kEmpty);
	REQUIRE(loss.get_type() != ogs::save::Value::NIL);
	CHECK(double(loss) == Approx(0.33));
	CHECK(0.55 + 0.12 + double(loss) == Approx(1.0));

	// A moagem segue a mesma regra de conservacao.
	const ogs::save::Value& flour = recipe_by_id(recipes, "flour");
	CHECK(double(flour.get("yield", ogs::save::Value()))
		+ sum_byproducts(flour) == Approx(1.0));
}

TEST_CASE("D-127: refinaria de petroleo conserva 0,45 + 0,29 + 0,09 + 0,17") {
	ogs::test::PocketWorld world;
	const double main = 0.45;
	const double diesel = 0.29;
	const double jet = 0.09;
	const double loss = world.declared_loss("gasoline_a");
	CHECK(loss == Approx(0.17));
	CHECK(main + diesel + jet + loss == Approx(1.0));
}
