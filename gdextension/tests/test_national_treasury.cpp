// R1b - Tesouro Nacional + envelopes (flag `rework.national_treasury`).
//
// Garantias, com o fixture `pocket_world` e o MESMO caminho do save real:
//   a) configuracao invalida (Tesouro ON sem `regional_cash`) e tratada como
//      desligada e deixa o motivo em `treasury_config_error`;
//   b) a arrecadacao no Tesouro e a distribuicao do envelope conservam o
//      dinheiro do mundo: imposto arrecadado == distribuido + saldo do Tesouro;
//   c) o envelope e proporcional a populacao com piso minimo por regiao;
//   d) o envelope nao toca os sub-saldos privados do R1a;
//   e) save/load ON preserva flag e saldos; OFF nao ganha chave nova.
#include "doctest/doctest.h"
#include "pocket_world.h"

#include "sim/save_state.h"
#include "sim/sim_core.h"
#include "sim/treasury.h"

#include <map>
#include <string>

using doctest::Approx;

namespace {

void seed_cash(ogs::test::PocketWorld& world, double firm, double service) {
	for (ogs::City& city : world.ws.cities) {
		city.firm_cash = firm;
		city.service_cash = service;
	}
}

// Liga as duas flags (R1b so existe com R1a) com parametros explicitos.
void enable_treasury(ogs::test::PocketWorld& world, double floor_fraction,
		double works_share = 0.0) {
	world.core.set_regional_cash(true);
	world.core.set_national_treasury(true, 1.0, floor_fraction, works_share, 0.5);
}

} // namespace

TEST_CASE("R1b: configuracao invalida (Tesouro sem caixa regional) desliga") {
	ogs::test::PocketWorld world;
	world.core.set_regional_cash(false);
	world.core.set_national_treasury(true, 1.0, 0.5, 0.35, 0.5);
	CHECK_FALSE(world.ws.national_treasury);
	CHECK_FALSE(world.ws.treasury_config_error.empty());

	// Com o caixa regional ligado a mesma chamada vale.
	ogs::test::PocketWorld valid;
	valid.core.set_regional_cash(true);
	valid.core.set_national_treasury(true, 1.0, 0.5, 0.35, 0.5);
	CHECK(valid.ws.national_treasury);
	CHECK(valid.ws.treasury_config_error.empty());

	// Desligar nao destroi dinheiro: o saldo volta ao orcamento publico da
	// primeira cidade da nacao (conservacao do total).
	ogs::treasury::collect(valid.ws, "AAA", 123.0);
	const double total_before = valid.core.money_total();
	valid.core.set_national_treasury(false);
	CHECK_FALSE(valid.ws.national_treasury);
	CHECK(ogs::treasury::total_balance(valid.ws) == Approx(0.0));
	CHECK(valid.core.money_total() == Approx(total_before));
}

TEST_CASE("R1b: arrecadacao e envelope conservam (arrecadado == distribuido + "
		"saldo)") {
	ogs::test::PocketWorld world;
	seed_cash(world, 50000.0, 30000.0);
	enable_treasury(world, 0.5);
	const double money_before = world.core.money_total();

	// O debito do pagador ja aconteceu; aqui so a contraparte no Tesouro.
	ogs::treasury::collect(world.ws, "AAA", 900.0);
	CHECK(world.core.money_total() == Approx(money_before + 900.0));
	CHECK(ogs::treasury::balance(world.ws, "AAA") == Approx(900.0));

	const double distributed = ogs::treasury::distribute_envelopes(world.ws);
	CHECK(distributed == Approx(900.0));
	CHECK(ogs::treasury::balance(world.ws, "AAA") == Approx(0.0));
	CHECK(world.ws.treasury_collected_total
		== Approx(world.ws.treasury_distributed_total
			+ ogs::treasury::total_balance(world.ws)));
	CHECK(world.core.money_total() == Approx(money_before + 900.0));
}

TEST_CASE("R1b: envelope e proporcional a populacao com piso por regiao") {
	// Sem piso: rateio puro pela populacao (2/3 e 1/3 no fixture 1M/500k).
	{
		ogs::test::PocketWorld world;
		enable_treasury(world, 0.0);
		ogs::treasury::collect(world.ws, "AAA", 900.0);
		ogs::treasury::distribute_envelopes(world.ws);
		CHECK(world.ws.cities[0].envelope_received_month == Approx(600.0));
		CHECK(world.ws.cities[1].envelope_received_month == Approx(300.0));
	}
	// Piso: uma regiao minuscula (1.000 de 1.001.000) recebe ao menos metade do
	// rateio igualitario (floor/n = 0,25 => ~18% do envelope, nao 0,1%).
	{
		ogs::test::PocketWorld world;
		enable_treasury(world, 0.5);
		world.ws.cities[0].simulation_population = 1000000.0;
		world.ws.cities[1].simulation_population = 1000.0;
		ogs::treasury::collect(world.ws, "AAA", 900.0);
		ogs::treasury::distribute_envelopes(world.ws);
		const double agra = world.ws.cities[0].envelope_received_month;
		const double portus = world.ws.cities[1].envelope_received_month;
		CHECK(agra + portus == Approx(900.0));
		CHECK(portus > 179.0);       // muito acima dos 0,9 do rateio puro
		CHECK(portus < 181.0);       // e abaixo do rateio igualitario (450)
		CHECK(portus == Approx(900.0 * 0.25 / 1.249000999).epsilon(1e-6));
	}
	// works_share: a fatia de obras vai para `CityGov::cash`; o resto para o
	// orcamento publico (a soma e o envelope recebido).
	{
		ogs::test::PocketWorld world;
		world.ws.city_gov.resize(world.ws.cities.size());
		enable_treasury(world, 0.0, 0.25);
		ogs::treasury::collect(world.ws, "AAA", 800.0);
		ogs::treasury::distribute_envelopes(world.ws);
		const double services =
			world.ws.cities[0].government_budget
			+ world.ws.cities[1].government_budget;
		const double works = world.ws.city_gov[0].cash
			+ world.ws.city_gov[1].cash;
		CHECK(works == Approx(200.0));
		CHECK(services == Approx(600.0));
		CHECK(works + services == Approx(800.0));
	}
}

TEST_CASE("R1b: envelope nao toca os sub-saldos privados (R1a)") {
	ogs::test::PocketWorld world;
	seed_cash(world, 50000.0, 30000.0);
	enable_treasury(world, 0.5);
	ogs::City& city = world.ws.cities[0];
	const double firm_sub = city.firm_sub;
	const double service_sub = city.service_sub;
	const double private_cash = city.private_cash;
	const double firm_cash = city.firm_cash;
	const double service_cash = city.service_cash;

	ogs::treasury::collect(world.ws, "AAA", 500.0);
	ogs::treasury::distribute_envelopes(world.ws);

	CHECK(city.firm_sub == Approx(firm_sub));
	CHECK(city.service_sub == Approx(service_sub));
	CHECK(city.private_cash == Approx(private_cash));
	CHECK(city.firm_cash == Approx(firm_cash));
	CHECK(city.service_cash == Approx(service_cash));
}

TEST_CASE("R1b: save ON preserva o Tesouro; OFF nao ganha chave") {
	// OFF: sem chave nova e documento igual ao de antes.
	{
		ogs::test::PocketWorld world;
		seed_cash(world, 50000.0, 30000.0);
		const ogs::save::Value saved =
			ogs::save::to_value(world.ws, world.core);
		CHECK_FALSE(saved.has("national_treasury"));
		CHECK_FALSE(saved.has("treasury"));
		const std::string before = ogs::save::dump_json(saved);
		ogs::test::PocketWorld target;
		ogs::save::from_value(target.ws, target.core, saved);
		CHECK_FALSE(target.ws.national_treasury);
		CHECK(ogs::treasury::total_balance(target.ws) == Approx(0.0));
		CHECK(ogs::save::dump_json(
			ogs::save::to_value(target.ws, target.core)) == before);
	}
	// ON: flag, saldos por nacao e acumulados sobrevivem ao round-trip.
	{
		ogs::test::PocketWorld world;
		seed_cash(world, 50000.0, 30000.0);
		enable_treasury(world, 0.5);
		ogs::treasury::collect(world.ws, "AAA", 700.0);
		ogs::treasury::distribute_envelopes(world.ws);
		ogs::treasury::collect(world.ws, "AAA", 40.0);
		CHECK(ogs::treasury::balance(world.ws, "AAA") == Approx(40.0));

		const ogs::save::Value saved =
			ogs::save::to_value(world.ws, world.core);
		CHECK(saved.has("national_treasury"));
		CHECK(saved.has("treasury"));
		const std::string before = ogs::save::dump_json(saved);
		ogs::test::PocketWorld target;
		ogs::save::from_value(target.ws, target.core, saved);
		CHECK(target.ws.national_treasury);
		CHECK(ogs::treasury::balance(target.ws, "AAA") == Approx(40.0));
		CHECK(target.ws.treasury_collected_total == Approx(740.0));
		CHECK(target.ws.treasury_distributed_total == Approx(700.0));
		CHECK(ogs::save::dump_json(
			ogs::save::to_value(target.ws, target.core)) == before);
	}
	// Flag do save ON aplicada sem o caixa regional: config invalida =>
	// desligado e saldo devolvido as regioes (nao se destroi dinheiro).
	{
		ogs::test::PocketWorld target;
		target.core.set_regional_cash(false);
		target.ws.cities[0].government_budget = 10.0;
		const double money_before = target.core.money_total();
		std::map<std::string, double> balances;
		balances["AAA"] = 25.0;
		target.core.apply_national_treasury_after_load(
			true, true, balances, 25.0, 0.0);
		CHECK_FALSE(target.ws.national_treasury);
		CHECK_FALSE(target.ws.treasury_config_error.empty());
		CHECK(ogs::treasury::total_balance(target.ws) == Approx(0.0));
		CHECK(target.core.money_total() == Approx(money_before));
	}
}
