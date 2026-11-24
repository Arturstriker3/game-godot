// D-130: produtos militares novos (aramida, placa balística, equipamento
// tático, comando, bloqueador de drones, lança-foguetes, foguete e antiaéreo).
//
// O núcleo não conhece a lista de bens: ele recebe products/recipes do JSON.
// Este teste confere o DADO real que o jogo carrega (goods.json/recipes.json):
//   * os 8 bens existem com setor/tier declarados;
//   * a receita de cada um tem exatamente os insumos aprovados, coeficiente
//     unitário (coef_source default_unit);
//   * a cadeia aramida → placa cerâmica → equipamento tático fecha por insumo;
//   * jobs/wage/opex/capital são iguais aos de small_arms/infantry_gear, com
//     params_source declarado.
#include "doctest/doctest.h"

#include "sim/save_value.h"

#include <algorithm>
#include <string>
#include <vector>

using doctest::Approx;

namespace {

const ogs::save::Value kEmpty;

const ogs::save::Value& by_id(const ogs::save::Value& rows,
		const std::string& id) {
	static const ogs::save::Value empty;
	for (int index = 0; index < rows.size(); ++index) {
		if (static_cast<std::string>(rows[index].get("id", kEmpty)) == id) {
			return rows[index];
		}
	}
	return empty;
}

std::vector<std::string> input_goods(const ogs::save::Value& recipe) {
	std::vector<std::string> out;
	const ogs::save::Value& inputs = recipe.get("inputs", kEmpty);
	for (int index = 0; index < inputs.size(); ++index) {
		out.push_back(static_cast<std::string>(inputs[index].get("good", kEmpty)));
	}
	std::sort(out.begin(), out.end());
	return out;
}

bool has_input(const ogs::save::Value& recipe, const std::string& good) {
	const std::vector<std::string> inputs = input_goods(recipe);
	return std::find(inputs.begin(), inputs.end(), good) != inputs.end();
}

struct GoodSpec {
	const char* id;
	const char* sector;
	const char* tier;
	bool durable;
};

constexpr GoodSpec kNewGoods[] = {
	{"aramid", "chemical", "semi", false},
	{"ballistic_ceramic_plate", "military", "semi", false},
	{"tactical_equipment", "military", "final", false},
	{"command_equipment", "military", "final", true},
	{"drone_jammer", "military", "final", true},
	{"at_launcher", "military", "final", true},
	{"at_rocket", "military", "final", false},
	{"aa_launcher", "military", "final", true},
};

} // namespace

TEST_CASE("D-130: catalogo declara os 8 bens militares novos") {
	ogs::save::Value goods;
	std::string error;
	REQUIRE(ogs::save::parse_json_file(std::string(OPENGS_REPO_DIR)
		+ "/data/definitions/economy/goods.json", goods, error));

	for (const GoodSpec& expected : kNewGoods) {
		const ogs::save::Value& good = by_id(goods, expected.id);
		REQUIRE(good.get_type() == ogs::save::Value::DICTIONARY);
		CHECK(static_cast<std::string>(good.get("sector", kEmpty))
			== expected.sector);
		CHECK(static_cast<std::string>(good.get("tier", kEmpty))
			== expected.tier);
		CHECK(bool(good.get("durable", kEmpty)) == expected.durable);
		CHECK(double(good.get("base_price", kEmpty)) > 0.0);
		const std::string source =
			static_cast<std::string>(good.get("base_price_source", kEmpty));
		CHECK(source.rfind("derived", 0) == 0);
	}
}

TEST_CASE("D-130: receitas novas declaram os insumos aprovados") {
	ogs::save::Value recipes;
	std::string error;
	REQUIRE(ogs::save::parse_json_file(std::string(OPENGS_REPO_DIR)
		+ "/data/definitions/economy/recipes.json", recipes, error));

	struct RecipeSpec {
		const char* id;
		std::vector<std::string> inputs;
	};
	const std::vector<RecipeSpec> expected = {
		{"aramid", {"chemicals", "refined_sulfur"}},
		{"ballistic_ceramic_plate", {"aramid", "bauxite"}},
		{"tactical_equipment",
			{"aramid", "ballistic_ceramic_plate", "rubber", "synthetic_fabric"}},
		{"command_equipment",
			{"batteries", "computer", "electronic_components", "wires"}},
		{"drone_jammer",
			{"batteries", "circuit_board", "electronic_components", "plastic"}},
		{"at_launcher", {"hardware", "planks", "steel"}},
		{"at_rocket", {"explosives", "gunpowder", "steel_sheet"}},
		{"aa_launcher",
			{"aluminum_ingot", "electronic_components", "explosives", "sensors"}},
	};
	for (const RecipeSpec& spec : expected) {
		const ogs::save::Value& recipe = by_id(recipes, spec.id);
		REQUIRE(recipe.get_type() == ogs::save::Value::DICTIONARY);
		CHECK(static_cast<std::string>(recipe.get("output", kEmpty))
			== spec.id);
		std::vector<std::string> inputs = spec.inputs;
		std::sort(inputs.begin(), inputs.end());
		CHECK(input_goods(recipe) == inputs);
		CHECK(static_cast<std::string>(recipe.get("coef_source", kEmpty))
			== "default_unit");
	}
}

TEST_CASE("D-130: cadeia aramida -> placa ceramica -> equipamento tatico") {
	ogs::save::Value recipes;
	std::string error;
	REQUIRE(ogs::save::parse_json_file(std::string(OPENGS_REPO_DIR)
		+ "/data/definitions/economy/recipes.json", recipes, error));

	const ogs::save::Value& aramid = by_id(recipes, "aramid");
	const ogs::save::Value& plate = by_id(recipes, "ballistic_ceramic_plate");
	const ogs::save::Value& tactical = by_id(recipes, "tactical_equipment");
	REQUIRE(aramid.get_type() == ogs::save::Value::DICTIONARY);
	REQUIRE(plate.get_type() == ogs::save::Value::DICTIONARY);
	REQUIRE(tactical.get_type() == ogs::save::Value::DICTIONARY);

	CHECK(has_input(aramid, "chemicals"));
	CHECK(has_input(aramid, "refined_sulfur"));
	CHECK(has_input(plate, "aramid"));
	CHECK(has_input(plate, "bauxite"));
	// A placa é consumida pela etapa seguinte (consumidor declarado no dado).
	CHECK(has_input(tactical, "aramid"));
	CHECK(has_input(tactical, "ballistic_ceramic_plate"));
	CHECK(has_input(tactical, "rubber"));
	CHECK(has_input(tactical, "synthetic_fabric"));
}

TEST_CASE("D-130: parametros iguais a small_arms/infantry_gear") {
	ogs::save::Value recipes;
	std::string error;
	REQUIRE(ogs::save::parse_json_file(std::string(OPENGS_REPO_DIR)
		+ "/data/definitions/economy/recipes.json", recipes, error));

	const ogs::save::Value& reference = by_id(recipes, "small_arms");
	const ogs::save::Value& reference_gear = by_id(recipes, "infantry_gear");
	REQUIRE(reference.get_type() == ogs::save::Value::DICTIONARY);
	REQUIRE(reference_gear.get_type() == ogs::save::Value::DICTIONARY);
	CHECK(double(reference.get("jobs_per_ton", kEmpty))
		== Approx(double(reference_gear.get("jobs_per_ton", kEmpty))));
	CHECK(double(reference.get("wage", kEmpty))
		== Approx(double(reference_gear.get("wage", kEmpty))));
	CHECK(double(reference.get("opex_per_ton", kEmpty))
		== Approx(double(reference_gear.get("opex_per_ton", kEmpty))));
	CHECK(double(reference.get("capital_per_capacity", kEmpty))
		== Approx(double(reference_gear.get("capital_per_capacity", kEmpty))));

	for (const GoodSpec& spec : kNewGoods) {
		const ogs::save::Value& recipe = by_id(recipes, spec.id);
		REQUIRE(recipe.get_type() == ogs::save::Value::DICTIONARY);
		CHECK(double(recipe.get("jobs_per_ton", kEmpty))
			== Approx(double(reference.get("jobs_per_ton", kEmpty))));
		CHECK(double(recipe.get("wage", kEmpty))
			== Approx(double(reference.get("wage", kEmpty))));
		CHECK(double(recipe.get("opex_per_ton", kEmpty))
			== Approx(double(reference.get("opex_per_ton", kEmpty))));
		CHECK(double(recipe.get("capital_per_capacity", kEmpty))
			== Approx(double(reference.get("capital_per_capacity", kEmpty))));
		const std::string params_source =
			static_cast<std::string>(recipe.get("params_source", kEmpty));
		CHECK(params_source.rfind("reuse:", 0) == 0);
	}
}
