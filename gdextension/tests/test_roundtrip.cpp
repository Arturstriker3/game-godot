// Save/load do WorldState minimo: o roundtrip tem de devolver exatamente o
// mesmo documento (o save_sha256 do jogo depende da ordem/tipos das chaves).
#include "doctest/doctest.h"
#include "pocket_world.h"

#include "sim/save_definitions.h"
#include "sim/potential_demand.h"
#include "sim/save_state.h"
#include "sim/sim_core.h"

#include <string>

using doctest::Approx;

namespace {

// Primeira linha diferente de dois dumps (mensagem curta; o dump inteiro e
// enorme e nao ajuda no log do runner).
std::string first_difference(const std::string& left,
		const std::string& right) {
	std::size_t line = 1;
	std::size_t begin_left = 0;
	std::size_t begin_right = 0;
	std::string previous;
	while (begin_left <= left.size() && begin_right <= right.size()) {
		std::size_t end_left = left.find('\n', begin_left);
		std::size_t end_right = right.find('\n', begin_right);
		if (end_left == std::string::npos) { end_left = left.size(); }
		if (end_right == std::string::npos) { end_right = right.size(); }
		const std::string left_line = left.substr(begin_left,
			end_left - begin_left);
		const std::string right_line = right.substr(begin_right,
			end_right - begin_right);
		if (left_line != right_line) {
			return "linha " + std::to_string(line) + "\n  contexto: "
				+ previous + "\n  antes: " + left_line
				+ "\n  depois: " + right_line;
		}
		previous = left_line;
		if (end_left == left.size() || end_right == right.size()) {
			break;
		}
		begin_left = end_left + 1;
		begin_right = end_right + 1;
		++line;
	}
	return "sem linha diferente (tamanho diverge)";
}

} // namespace

TEST_CASE("save/load: roundtrip do mundo de bolso preserva o documento") {
	ogs::test::PocketWorld source;
	const ogs::save::Value saved = ogs::save::to_value(source.ws, source.core);
	const std::string before = ogs::save::dump_json(saved);

	ogs::test::PocketWorld target;
	ogs::save::from_value(target.ws, target.core, saved);
	const std::string after = ogs::save::dump_json(
		ogs::save::to_value(target.ws, target.core));

	const bool same = after == before;
	CHECK_MESSAGE(same, first_difference(before, after));
}

TEST_CASE("save/load: roundtrip preserva dinheiro, dia e entidades") {
	ogs::test::PocketWorld source;
	source.ws.day = 7;
	const double money = source.core.money_total();
	const ogs::save::Value saved = ogs::save::to_value(source.ws, source.core);

	ogs::test::PocketWorld target;
	ogs::save::from_value(target.ws, target.core, saved);

	CHECK(target.ws.day == 7);
	CHECK(target.ws.active == source.ws.active);
	CHECK(target.ws.cities.size() == source.ws.cities.size());
	CHECK(target.ws.buildings.size() == source.ws.buildings.size());
	CHECK(target.ws.groups.size() == source.ws.groups.size());
	CHECK(target.core.money_total() == Approx(money));
	CHECK(target.ws.banks.size() == source.ws.banks.size());
}

TEST_CASE("save/load: catalogo e receitas continuam disponiveis apos o load") {
	ogs::test::PocketWorld target;
	const ogs::save::Value saved = ogs::save::to_value(target.ws, target.core);
	ogs::save::from_value(target.ws, target.core, saved);

	CHECK(target.product("planks") >= 0);
	CHECK(target.product("oil") >= 0);
	CHECK(target.recipe("planks") >= 0);
	CHECK(target.recipe("gasoline_a") >= 0);
	CHECK(target.ws.recipes[static_cast<std::size_t>(
		target.recipe("planks"))].yield == Approx(0.55));
	CHECK(target.ws.efficiency.refinery_gasoline == target.product("gasoline"));
}

TEST_CASE("save/load: roundtrip preserva construction_days e plant_class (G1)") {
	// G1: prazo de obra da receita e classe da planta sao aditivos no save
	// (sem bump de versao). O fixture tem a receita de eletronicos `chip`, que
	// aqui representa a planta de semicondutores.
	ogs::test::PocketWorld source;
	const int source_index = source.recipe("chip");
	REQUIRE(source_index >= 0);
	ogs::Recipe& recipe = source.ws.recipes[
		static_cast<std::size_t>(source_index)];
	recipe.construction_days = 180;
	recipe.plant_class = "semiconductor_fab";

	// Caminho real do save: as receitas vivem no bloco `definitions`
	// (to_snapshot/to_dict de economy_sim), nao no estado dinamico.
	const ogs::save::Value definitions = ogs::save::definitions_to(source.ws);
	const std::string before = ogs::save::dump_json(definitions);

	ogs::test::PocketWorld target;
	ogs::save::definitions_from(target.ws, definitions);

	const int loaded_index = target.recipe("chip");
	REQUIRE(loaded_index >= 0);
	const ogs::Recipe& loaded = target.ws.recipes[
		static_cast<std::size_t>(loaded_index)];
	CHECK(loaded.construction_days == 180);
	CHECK(loaded.plant_class == "semiconductor_fab");

	// O bloco inteiro volta igual (o save_sha256 do jogo depende disso).
	const std::string after = ogs::save::dump_json(
		ogs::save::definitions_to(target.ws));
	CHECK_MESSAGE(after == before, first_difference(before, after));
}

TEST_CASE("save/load: roundtrip preserva o cache mensal do piso (G2)") {
	// G2 QA: o cache do piso viaja como chave aditiva `potential_cache`
	// (esparsa, so celulas nao nulas) e volta bit a bit; save sem a chave
	// deixa o cache vazio (recalculado no proximo snapshot).
	ogs::test::PocketWorld source;
	ogs::PotentialDemandCache cache;
	cache.cities = 2;
	cache.products = 3;
	cache.day = 41;
	cache.recipes = 17;
	cache.needs_levels = 4;
	cache.government_products = 2;
	cache.durable_decay = 0.000289;
	cache.discount = 0.5;
	cache.cap = 1.0;
	cache.needs_loaded = true;
	cache.values.assign(6, 0.0);
	cache.values[0] = 12.5;   // cidade 0, produto 0
	cache.values[5] = 0.25;   // cidade 1, produto 2

	const ogs::save::Value saved =
		ogs::save::to_value(source.ws, source.core, &cache);

	ogs::test::PocketWorld target;
	ogs::PotentialDemandCache loaded;
	ogs::save::from_value(target.ws, target.core, saved, &loaded);
	CHECK(loaded.day == 41);
	CHECK(loaded.cities == 2);
	CHECK(loaded.products == 3);
	CHECK(loaded.recipes == 17u);
	CHECK(loaded.needs_levels == 4u);
	CHECK(loaded.government_products == 2u);
	CHECK(loaded.durable_decay == Approx(0.000289));
	CHECK(loaded.discount == Approx(0.5));
	CHECK(loaded.cap == Approx(1.0));
	CHECK(loaded.needs_loaded);
	REQUIRE(loaded.values.size() == cache.values.size());
	for (std::size_t cell = 0; cell < cache.values.size(); ++cell) {
		CHECK(loaded.values[cell] == Approx(cache.values[cell]));
	}

	// Save sem a chave (antigo, ou brain/piso desligado): cache vazio.
	ogs::test::PocketWorld plain;
	const ogs::save::Value no_cache =
		ogs::save::to_value(plain.ws, plain.core);
	CHECK_FALSE(no_cache.has("potential_cache"));
	ogs::PotentialDemandCache untouched;
	ogs::save::from_value(plain.ws, plain.core, no_cache, &untouched);
	CHECK(untouched.values.empty());
	CHECK(untouched.day == -1);
}

TEST_CASE("save/load: carregar duas vezes no mesmo mundo e idempotente") {
	ogs::test::PocketWorld world;
	const ogs::save::Value saved = ogs::save::to_value(world.ws, world.core);
	ogs::save::from_value(world.ws, world.core, saved);
	const std::string first = ogs::save::dump_json(
		ogs::save::to_value(world.ws, world.core));
	ogs::save::from_value(world.ws, world.core, saved);
	const std::string second = ogs::save::dump_json(
		ogs::save::to_value(world.ws, world.core));
	CHECK(second == first);
}
