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
#include "sim/balance.h"
#include "sim/crisis.h"
#include "sim/deposit.h"
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

// R2b1 (caso exato do 365 d ON): o comprador do leilao e' autorizado por
// `liquid_cash()` (caixa + deposito sacavel) e pagava so' com `cash`, deixando
// o caixa negativo (G-AUS-mining-11 no dia 118). O pagamento agora segue a
// ordem caixa -> deposito do `Group::pay_cash`; aqui o preco cabe no deposito
// e NAO no caixa.
TEST_CASE("R2b1: leilao autorizado por liquid_cash nao deixa cash negativo") {
	ogs::test::PocketWorld world;
	world.ws.regional_cash = true;
	ogs::BanksRework cfg;
	cfg.enabled = true;
	cfg.capital = true;
	cfg.deposits = true;
	world.core.set_banks_rework(cfg);
	ogs::Bank& bank = ogs::bank::ensure(world.ws, "AAA",
		ogs::bank::kRolePublic);
	bank.name = "AAA bank";
	bank.name_status = "present";
	ogs::Building* mill = world.building("PORTUS_SAWMILL");
	REQUIRE(mill != nullptr);
	ogs::Group& seller = group_by_id(world, "AAA_WOOD");
	ogs::Group& buyer = group_by_id(world, "AAA_FUEL");

	// Unico candidato: o leilao escolhe o primeiro grupo com liquidez.
	for (ogs::Group& group : world.ws.groups) {
		group.cash = 0.0;
		group.bank_deposit = 0.0;
		group.deposit_bank_id.clear();
	}
	seller.debt = 0.0;
	seller.credit_arrears = 0.0;
	const double book = std::max(1.0,
		std::max(0.0, mill->capital_per_capacity)
			* std::max(0.0, mill->capacity));
	const double price = book * (1.0 - std::clamp(
		world.ws.crisis.auction_discount, 0.0, 1.0));
	REQUIRE(price > 1.0);
	buyer.cash = price * 0.25;           // so' um quarto em caixa
	buyer.bank_deposit = price;          // o resto esta' no deposito
	buyer.deposit_bank_id = bank.id;
	bank.cash = price;                   // lastro do claim
	bank.deposits_funding = price;
	ogs::deposit::link(world.ws);
	REQUIRE(buyer.liquid_cash() >= price);
	REQUIRE(buyer.cash < price);

	const double money_before = world.core.money_total();
	std::string error;
	const bool ok = ogs::crisis::auction(world.ws, world.core, "PORTUS",
		"PORTUS_SAWMILL", &error);
	CHECK_MESSAGE(ok, error);

	// Caixa nunca negativo: a parcela que o caixa nao cobre sai do claim.
	CHECK(buyer.cash >= 0.0);
	CHECK(buyer.cash == Approx(0.0));
	CHECK(buyer.bank_deposit == Approx(price * 0.25));
	CHECK(bank.cash == Approx(price * 0.25));
	CHECK(bank.deposits_funding == Approx(price * 0.25));
	CHECK(seller.cash == Approx(price));
	CHECK(world.core.money_total() == Approx(money_before));
}

// R2b1 (segundo defeito do dia 118): o produto do leilao reduzia o AGREGADO
// do vendedor no `lender()` e deixava os claims de outro banco em aberto
// (juros gap no invariante). Agora paga os claims POR CREDOR, na ordem do
// vetor, e o par agregado<->claim<->banco fica exato.
TEST_CASE("R2b1: leilao paga os claims do vendedor por credor") {
	ogs::test::PocketWorld world;
	world.ws.regional_cash = true;
	ogs::BanksRework cfg;
	cfg.enabled = true;
	cfg.capital = true;
	cfg.deposits = true;
	world.core.set_banks_rework(cfg);
	ogs::Bank& pub = ogs::bank::ensure(world.ws, "AAA",
		ogs::bank::kRolePublic);
	pub.name = "AAA public";
	pub.name_status = "present";
	ogs::Bank& pri = ogs::bank::ensure(world.ws, "AAA",
		ogs::bank::kRolePrivate);
	pri.name = "AAA private";
	pri.name_status = "present";
	for (auto& row : world.ws.banks) {
		row.second.loans = 0.0;
		row.second.arrears = 0.0;
		row.second.deposits = 0.0;
		row.second.deposits_funding = 0.0;
		row.second.cash = 0.0;
	}
	ogs::Building* mill = world.building("PORTUS_SAWMILL");
	REQUIRE(mill != nullptr);
	ogs::Group& seller = group_by_id(world, "AAA_WOOD");
	ogs::Group& buyer = group_by_id(world, "AAA_FUEL");
	for (ogs::Group& group : world.ws.groups) {
		group.cash = 0.0;
		group.debt = 0.0;
		group.credit_arrears = 0.0;
		group.debt_claims.clear();
		group.bank_deposit = 0.0;
		group.deposit_bank_id.clear();
	}
	// Vendedor com DOIS credores: o agregado precisa espelhar os claims.
	seller.debt_claims = {
		ogs::BankLoan{pub.id, 800.0, 200.0, 0.0, 0},
		ogs::BankLoan{pri.id, 500.0, 500.0, 0.0, 0},
	};
	std::sort(seller.debt_claims.begin(), seller.debt_claims.end(),
		[](const ogs::BankLoan& a, const ogs::BankLoan& b) {
			return a.bank_id < b.bank_id;
		});
	seller.debt = 1300.0;
	seller.credit_arrears = 700.0;
	pub.loans = 800.0;
	pub.arrears = 200.0;
	pub.deposits = 1000.0;
	pri.loans = 500.0;
	pri.arrears = 500.0;
	pri.deposits = 1000.0;
	const double book = std::max(1.0,
		std::max(0.0, mill->capital_per_capacity)
			* std::max(0.0, mill->capacity));
	const double price = book * (1.0 - std::clamp(
		world.ws.crisis.auction_discount, 0.0, 1.0));
	REQUIRE(price > 2000.0);
	buyer.cash = price;
	ogs::deposit::link(world.ws);
	REQUIRE(std::abs(ogs::balance::ledger(world.ws,
		world.core.money_total()).financial_sum()) < 1e-6);

	double bank_delta = 0.0;
	std::string error;
	const double money_before = world.core.money_total();
	const bool ok = ogs::crisis::auction(world.ws, world.core, "PORTUS",
		"PORTUS_SAWMILL", &error);
	CHECK_MESSAGE(ok, error);

	CHECK(seller.debt == Approx(0.0));
	CHECK(seller.credit_arrears == Approx(0.0));
	for (const ogs::BankLoan& claim : seller.debt_claims) {
		CHECK(claim.principal + claim.arrears == Approx(0.0));
	}
	CHECK(pub.loans == Approx(0.0));
	CHECK(pub.arrears == Approx(0.0));
	CHECK(pub.deposits == Approx(0.0));
	CHECK(pri.loans == Approx(0.0));
	CHECK(pri.arrears == Approx(0.0));
	CHECK(pri.deposits == Approx(0.0));
	CHECK(seller.cash == Approx(price - 2000.0));
	// A dívida paga destrói depósito (fora do money_total): o caixa do mundo
	// cai exatamente pelo valor dos claims quitados.
	CHECK(world.core.money_total() == Approx(money_before - 2000.0));
	// O livro do mundo continua fechando: sem claim órfão no outro banco.
	CHECK(std::abs(ogs::balance::ledger(world.ws,
		world.core.money_total()).financial_sum()) < 1e-6);
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
