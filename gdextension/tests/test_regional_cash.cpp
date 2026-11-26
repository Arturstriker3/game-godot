// R1a - caixa unico regional (flag `rework.regional_cash`).
//
// Tres garantias, com o fixture `pocket_world` e o MESMO caminho do save real:
//   a) a fusao dos dois caixas conserva o total (money_total nao muda);
//   b) uma transferencia interna (firmas -> servicos) e NO-OP monetario e
//      continua registrada na estatistica por setor;
//   c) o save/load com a flag ON preserva bit-a-bit o documento, e com a flag
//      OFF o documento nao ganha a chave `private_cash` (save anterior).
#include "doctest/doctest.h"
#include "pocket_world.h"

#include "sim/save_state.h"
#include "sim/sim_core.h"

#include <string>

using doctest::Approx;

namespace {

// Preenche os dois caixas de todas as cidades do fixture (o fixture nao tem
// caixa inicial: o mundo de bolso so precisa de um valor coerente).
void seed_cash(ogs::test::PocketWorld& world, double firm, double service) {
	for (ogs::City& city : world.ws.cities) {
		city.firm_cash = firm;
		city.service_cash = service;
	}
}

} // namespace

TEST_CASE("R1a: fusao do caixa conserva o total") {
	ogs::test::PocketWorld world;
	seed_cash(world, 50000.0, 30000.0);
	const double before = world.core.money_total();

	world.core.set_regional_cash(true);

	CHECK(world.ws.regional_cash);
	CHECK(world.core.money_total() == Approx(before));
	for (const ogs::City& city : world.ws.cities) {
		CHECK(city.unified_cash);
		CHECK(city.private_cash == Approx(80000.0));
		// A soma dos espelhos continua igual ao pool (invariante do status).
		CHECK(city.firm_cash + city.service_cash == Approx(city.private_cash));
	}

	// Desligar devolve o estado anterior: os dois caixas intactos e o pool 0.
	world.core.set_regional_cash(false);
	CHECK_FALSE(world.ws.regional_cash);
	CHECK(world.core.money_total() == Approx(before));
	for (const ogs::City& city : world.ws.cities) {
		CHECK_FALSE(city.unified_cash);
		CHECK(city.private_cash == Approx(0.0));
	}
}

TEST_CASE("R1a: transferencia interna e no-op monetario") {
	ogs::test::PocketWorld world;
	seed_cash(world, 50000.0, 30000.0);
	world.core.set_regional_cash(true);
	ogs::City& city = world.ws.cities[0];
	const double total_before = world.core.money_total();
	const double pool_before = city.private_cash;
	const double firm_before = city.firm_cash;
	const double service_before = city.service_cash;

	// Transferencia interna artificial (o mesmo par pay/receive usado por opex,
	// insumos de servico, frete e construcao): firmas pagam servicos.
	const double amount = 12000.0;
	city.firm_pay(amount);
	city.service_receive(amount);

	CHECK(city.private_cash == Approx(pool_before));
	CHECK(world.core.money_total() == Approx(total_before));
	// A estatistica por setor registra o fluxo (pode ate ficar negativa).
	CHECK(city.firm_cash == Approx(firm_before - amount));
	CHECK(city.service_cash == Approx(service_before + amount));

	// Desligado, a mesma chamada NAO toca no pool e cada setor paga o seu.
	world.core.set_regional_cash(false);
	seed_cash(world, 50000.0, 30000.0);
	const double off_total = world.core.money_total();
	ogs::City& off_city = world.ws.cities[0];
	off_city.firm_pay(amount);
	off_city.service_receive(amount);
	CHECK(off_city.private_cash == Approx(0.0));
	CHECK(off_city.firm_cash == Approx(50000.0 - amount));
	CHECK(off_city.service_cash == Approx(30000.0 + amount));
	CHECK(world.core.money_total() == Approx(off_total));
}

TEST_CASE("R1a: save/load com a flag ON e OFF e bit-exato") {
	// OFF: o documento NAO ganha a chave (save byte-identico ao anterior) e o
	// roundtrip devolve o mesmo dump.
	{
		ogs::test::PocketWorld world;
		seed_cash(world, 50000.0, 30000.0);
		const ogs::save::Value saved = ogs::save::to_value(world.ws, world.core);
		CHECK_FALSE(saved.has("regional_cash"));
		const std::string before = ogs::save::dump_json(saved);

		ogs::test::PocketWorld target;
		ogs::save::from_value(target.ws, target.core, saved);
		CHECK_FALSE(target.ws.regional_cash);
		for (const ogs::City& city : target.ws.cities) {
			CHECK_FALSE(city.unified_cash);
			CHECK(city.private_cash == Approx(0.0));
		}
		const std::string after = ogs::save::dump_json(
			ogs::save::to_value(target.ws, target.core));
		CHECK(after == before);
	}

	// ON: o documento carrega a flag e o pool; o roundtrip e bit-exato.
	{
		ogs::test::PocketWorld world;
		seed_cash(world, 50000.0, 30000.0);
		world.core.set_regional_cash(true);
		const double total = world.core.money_total();
		const ogs::save::Value saved = ogs::save::to_value(world.ws, world.core);
		REQUIRE(saved.has("regional_cash"));
		CHECK(bool(saved.get("regional_cash", false)));
		const std::string before = ogs::save::dump_json(saved);

		ogs::test::PocketWorld target;
		ogs::save::from_value(target.ws, target.core, saved);
		CHECK(target.ws.regional_cash);
		CHECK(target.core.money_total() == Approx(total));
		for (const ogs::City& city : target.ws.cities) {
			CHECK(city.unified_cash);
			CHECK(city.private_cash == Approx(80000.0));
		}
		const std::string after = ogs::save::dump_json(
			ogs::save::to_value(target.ws, target.core));
		CHECK(after == before);
	}
}
