// Determinismo: mesma semente (mesmo fixture) -> mesmo digest do save. Sem
// tick e com tick (o dia e deterministico: nenhuma decisao fora da ordem).
//
// D-126: os casos da bateria de crise foram portados em tests/test_crisis.cpp
// (mundo de bolso; sem godot-cpp).
#include "doctest/doctest.h"
#include "pocket_world.h"

#include "sim/sim_day.h"

#include <string>

TEST_CASE("determinismo: mesma semente gera o mesmo digest (sem tick)") {
	ogs::test::PocketWorld first;
	ogs::test::PocketWorld second;
	CHECK(ogs::test::digest(first.ws, first.core)
		== ogs::test::digest(second.ws, second.core));
}

TEST_CASE("determinismo: dois mundos com a mesma semente andam dias iguais") {
	ogs::test::PocketWorld first;
	ogs::test::PocketWorld second;
	for (int day = 0; day < 2; ++day) {
		ogs::tick_day(first.ws, first.core);
		ogs::tick_day(second.ws, second.core);
		CHECK(first.ws.day == second.ws.day);
	}
	CHECK(ogs::test::digest(first.ws, first.core)
		== ogs::test::digest(second.ws, second.core));
}

TEST_CASE("determinismo: o digest muda quando o mundo anda") {
	ogs::test::PocketWorld world;
	const auto initial = ogs::test::digest(world.ws, world.core);
	ogs::tick_day(world.ws, world.core);
	const auto after_one = ogs::test::digest(world.ws, world.core);
	ogs::tick_day(world.ws, world.core);
	const auto after_two = ogs::test::digest(world.ws, world.core);

	CHECK(after_one != initial);
	CHECK(after_two != after_one);
}
