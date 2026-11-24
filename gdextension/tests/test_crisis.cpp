// D-126: sobrevivencia em crise. Porte dos casos da bateria Godot
// (tools/d126_crisis_test.gd) para o mundo de bolso, sem godot-cpp:
// demissao/rescisao, credito de giro, renegociacao bancaria, leilao e
// hibernacao de usina pela necessidade de pico x reserva.
//
// Nenhum numero aqui e dado de simulacao: o fixture e o mundo de bolso e o
// estado extra (caixa do grupo, usinas) e montado no teste, como o save de
// stress da bateria Godot.
#include "doctest/doctest.h"
#include "pocket_world.h"

#include "sim/bank.h"
#include "sim/crisis.h"
#include "sim/sim_core.h"
#include "sim/sim_types.h"

#include <string>

using doctest::Approx;

namespace {

ogs::Group& group_by_id(ogs::test::PocketWorld& world, const std::string& id) {
	for (ogs::Group& group : world.ws.groups) {
		if (group.id == id) {
			return group;
		}
	}
	throw std::runtime_error("pocket_world: grupo ausente: " + id);
}

ogs::City& city_by_id(ogs::test::PocketWorld& world, const std::string& id) {
	for (ogs::City& city : world.ws.cities) {
		if (city.id == id) {
			return city;
		}
	}
	throw std::runtime_error("pocket_world: cidade ausente: " + id);
}

ogs::Bank& public_bank(ogs::test::PocketWorld& world) {
	return ogs::bank::ensure(world.ws, "AAA", ogs::bank::kRolePublic);
}

// Usina de teste: nao existe no fixture; o estado vem daqui (test state).
void add_plant(ogs::test::PocketWorld& world, const std::string& id,
		int fuel, double mw, double opex_per_mwh, const std::string& owner) {
	ogs::PowerPlant plant;
	plant.id = id;
	plant.city_index = 0;  // AGRA
	plant.fuel = fuel;
	plant.mw = mw;
	plant.owner_group = owner;
	plant.owner_nation = "AAA";
	plant.capex = 1000.0;
	plant.opex_per_mwh = opex_per_mwh;
	plant.jobs = 10.0;
	plant.wage = 10.0;
	plant.day_avail_mw = mw;
	plant.production_efficiency = 1.0;
	plant.loss_days = 55;  // acima de mothball_days (50)
	world.ws.power_plants.push_back(plant);
}

// Rede AAA com demanda medida e CF declarado (ausencia declarada nao hiberna).
void seed_grid(ogs::test::PocketWorld& world, double demand_mwh) {
	if (world.ws.power_cities.size() < world.ws.cities.size()) {
		world.ws.power_cities.resize(world.ws.cities.size());
	}
	ogs::PowerGridState& grid = world.ws.power_grids["AAA"];
	grid.nation = "AAA";
	grid.exists = true;
	grid.demand_mwh = demand_mwh;
	grid.price = 0.0;
	world.ws.crisis.power_reserve_margin = 0.15;
	world.ws.power_params.cf[ogs::POWER_COAL] = 1.0;
	world.ws.power_params.cf[ogs::POWER_OIL] = 1.0;
}

} // namespace

TEST_CASE("D-126: rescisao reduz caixa e trabalhadores com conservacao") {
	ogs::test::PocketWorld world;
	ogs::Building* mill = world.building("PORTUS_SAWMILL");
	REQUIRE(mill != nullptr);
	ogs::Group& owner = group_by_id(world, "AAA_WOOD");
	ogs::City& portus = city_by_id(world, "PORTUS");

	// Estado de teste: caixa do dono suficiente e trabalhadores preenchidos
	// (o fixture nasce sem operacao, como o mundo inicial).
	owner.cash = 100000.0;
	mill->workers = mill->jobs;
	REQUIRE(mill->jobs > 1.0);
	REQUIRE(mill->wage > 0.0);

	const double jobs_before = mill->jobs;
	const double cash_before = owner.cash;
	const double savings_before = portus.pop.savings;
	const double money_before = world.core.money_total();

	std::string error;
	const bool ok = ogs::crisis::layoff(world.ws, world.core, "PORTUS",
		"PORTUS_SAWMILL", 0.5, &error);
	CHECK_MESSAGE(ok, error);

	const double per_job = mill->wage * world.ws.crisis.severance_days;
	const double cut = jobs_before * 0.5;
	const double paid = cut * per_job;

	CHECK(mill->jobs == Approx(jobs_before - cut));
	CHECK(mill->workers == Approx(mill->jobs));
	CHECK(mill->last_layoff_day == world.ws.day);
	CHECK(owner.cash == Approx(cash_before - paid));
	// A rescisao vai as familias: nenhum dinheiro e criado nem destruido.
	CHECK(portus.pop.savings == Approx(savings_before + paid));
	CHECK(world.core.money_total() == Approx(money_before));
	CHECK(world.ws.crisis_layoffs == 1);
	CHECK(world.ws.crisis_jobs_cut == Approx(cut));
	CHECK(world.ws.crisis_severance_paid == Approx(paid));
}

TEST_CASE("D-126: credito de giro so com banco existente") {
	SUBCASE("com banco publico o credito entra no caixa e vira divida") {
		ogs::test::PocketWorld world;
		ogs::City& agra = city_by_id(world, "AGRA");
		agra.firm_cash = 10000.0;
		ogs::Bank& bank = public_bank(world);
		const double money_before = world.core.money_total();

		std::string error;
		const bool ok = ogs::crisis::borrow_working_capital(world.ws,
			world.core, "AGRA", 500.0, &error);
		CHECK_MESSAGE(ok, error);

		CHECK(agra.firm_debt == Approx(500.0));
		CHECK(agra.firm_cash == Approx(10500.0));
		CHECK(bank.loans == Approx(500.0));
		CHECK(bank.deposits == Approx(500.0));
		// Dinheiro bancario: o total sobe exatamente o concedido (M1).
		CHECK(world.core.money_total() == Approx(money_before + 500.0));
	}

	SUBCASE("sem banco na nacao nao ha credito (nunca se cria banco)") {
		ogs::test::PocketWorld world;
		ogs::City& agra = city_by_id(world, "AGRA");
		agra.firm_cash = 10000.0;
		world.ws.banks.clear();  // nacao sem banco publico
		const double money_before = world.core.money_total();

		std::string error;
		const bool ok = ogs::crisis::borrow_working_capital(world.ws,
			world.core, "AGRA", 500.0, &error);
		CHECK_FALSE(ok);
		CHECK(error.find("banco") != std::string::npos);
		CHECK(agra.firm_debt == Approx(0.0));
		CHECK(agra.firm_cash == Approx(10000.0));
		CHECK(world.ws.banks.empty());
		CHECK(world.ws.crisis_loans == Approx(0.0));
		CHECK(world.core.money_total() == Approx(money_before));
	}
}

TEST_CASE("D-126: renegociacao cobra a multa uma vez e capitaliza o resto") {
	ogs::test::PocketWorld world;
	ogs::Group group;
	group.id = "AAA_RENEG";
	group.name = group.id;
	group.origin_nation = "AAA";
	group.state_owned = true;  // credor declarado: banco publico
	group.debt = 100000.0;
	group.cash = 0.0;  // multa nao cabe no caixa: capitaliza
	group.credit_rate_add = world.ws.crisis.credit_rate_add;
	group.credit_arrears_days = world.ws.crisis.credit_renegotiation_days;
	world.ws.groups.push_back(group);
	world.core.rebuild_indexes();

	ogs::Bank& bank = public_bank(world);
	bank.loans = 100000.0;
	bank.cash = 0.0;
	bank.deposits = 0.0;
	bank.arrears = 0.0;

	const double rate = ogs::bank::loan_rate(world.ws, "AAA")
		+ world.ws.crisis.credit_rate_add;
	const double interest = 100000.0 * rate / ogs::bank::kDaysPerYear;
	const double fee = world.ws.crisis.credit_penalty * (100000.0 + interest);
	const double money_before = world.core.money_total();

	double delta = 123.0;
	CHECK(ogs::bank::phase(world.ws, &delta) == 0);

	ogs::Group& after = group_by_id(world, "AAA_RENEG");
	CHECK(after.credit_arrears == Approx(0.0));
	CHECK(after.debt == Approx(100000.0 + interest + fee));
	CHECK(after.credit_renegotiations == 1);
	CHECK(after.credit_rate_add
		== Approx(world.ws.crisis.credit_rate_add));
	CHECK(world.ws.crisis_renegotiations == 1);
	CHECK(world.ws.crisis_penalty_paid == Approx(fee));
	// A capitalizacao cresce divida e ativo do banco juntos; nenhum dinheiro
	// e criado nem destruido (o fluxo bancario do dia fica em zero).
	CHECK(bank.loans == Approx(100000.0 + interest + fee));
	CHECK(bank.deposits == Approx(0.0));
	CHECK(world.core.money_total() == Approx(money_before));
	CHECK(delta == Approx(0.0));

	// Dias seguintes: a multa NAO e aplicada de novo (premio unico).
	const double debt_after_renegotiation = after.debt;
	ogs::bank::phase(world.ws, nullptr);
	ogs::bank::phase(world.ws, nullptr);
	const double debt_later = after.debt;
	CHECK(world.ws.crisis_renegotiations == 1);
	CHECK(after.credit_rate_add
		== Approx(world.ws.crisis.credit_rate_add));
	CHECK(debt_later - debt_after_renegotiation
		<= debt_after_renegotiation * rate / ogs::bank::kDaysPerYear * 2.0
			+ 1e-6);
}

TEST_CASE("D-126: leilao transfere o predio com conservacao") {
	ogs::test::PocketWorld world;
	ogs::Building* mill = world.building("PORTUS_SAWMILL");
	REQUIRE(mill != nullptr);
	ogs::Group& seller = group_by_id(world, "AAA_WOOD");
	ogs::Group& buyer = group_by_id(world, "AAA_FUEL");
	// Comprador de teste: sem caixa no fixture, o predio iria a falencia.
	buyer.cash = 20000.0;
	const double seller_cash = seller.cash;
	const double money_before = world.core.money_total();
	const double price = 10000.0
		* (1.0 - world.ws.crisis.auction_discount);

	std::string error;
	const bool ok = ogs::crisis::auction(world.ws, world.core, "PORTUS",
		"PORTUS_SAWMILL", &error);
	CHECK_MESSAGE(ok, error);

	// O predio NAO e apagado: muda de dono com o desconto do leilao.
	ogs::Building* after = world.building("PORTUS_SAWMILL");
	REQUIRE(after != nullptr);
	CHECK(after->owner == "national:AAA_FUEL");
	CHECK_FALSE(after->bankrupt);
	CHECK(after->local_share == Approx(0.0));
	CHECK(buyer.cash == Approx(20000.0 - price));
	CHECK(seller.cash == Approx(seller_cash + price));
	CHECK(world.core.money_total() == Approx(money_before));
	CHECK(world.ws.crisis_auctions == 1);
	CHECK(world.ws.crisis_auction_value == Approx(price));
}

TEST_CASE("D-126: hibernacao de usina respeita o pico x reserva") {
	SUBCASE("necessaria nao hiberna, excedente hiberna") {
		// Demanda 100 MWh x 1,15 = 115 MWh de capacidade firme; a de carvao
		// entrega 120 MWh e cobre sozinha o prefixo de merito.
		ogs::test::PocketWorld world;
		seed_grid(world, 100.0);
		add_plant(world, "COAL", ogs::POWER_COAL, 5.0, 0.10, "AAA_FUEL");
		add_plant(world, "OIL", ogs::POWER_OIL, 5.0, 0.20, "AAA_FUEL");

		double delta = 0.0;
		ogs::crisis::phase(world.ws, world.core, &delta);

		CHECK_FALSE(world.ws.power_plants[0].mothballed);
		CHECK(world.ws.power_plants[1].mothballed);
		CHECK(world.ws.crisis_mothballs == 1);
	}

	SUBCASE("quando o ativo nao cobre o pico a hibernada e necessaria") {
		// Carvao entrega 105 MWh: cobre a demanda (100) mas nao o pico com
		// margem (115). O oleo deixa de ser excedente e nao hiberna.
		ogs::test::PocketWorld world;
		seed_grid(world, 100.0);
		add_plant(world, "COAL", ogs::POWER_COAL, 105.0 / 24.0, 0.10,
			"AAA_FUEL");
		add_plant(world, "OIL", ogs::POWER_OIL, 5.0, 0.20, "AAA_FUEL");

		double delta = 0.0;
		ogs::crisis::phase(world.ws, world.core, &delta);

		CHECK_FALSE(world.ws.power_plants[0].mothballed);
		CHECK_FALSE(world.ws.power_plants[1].mothballed);
		CHECK(world.ws.crisis_mothballs == 0);
	}
}
