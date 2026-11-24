// Testes do banco: conservacao de dinheiro (D-128), identidade do banco e
// credor declarado. So C++ puro: nenhuma API do Godot.
#include "doctest/doctest.h"
#include "pocket_world.h"

#include "sim/bank.h"
#include "sim/sim_core.h"

using doctest::Approx;

namespace {

// Grupo estatal sem predios: para o banco publico, com o passivo que o teste
// quiser. `cash` e o deposito do grupo no banco (dinheiro do mundo).
ogs::Group make_group(const std::string& id, double cash) {
	ogs::Group group;
	group.id = id;
	group.name = id;
	group.origin_nation = "AAA";
	group.state_owned = true;
	group.cash = cash;
	return group;
}

} // namespace

TEST_CASE("bank: posicao vazia nao cria nem destroi dinheiro") {
	ogs::test::PocketWorld world;
	// O fixture cria o banco publico com saldo zero: a fase sai cedo.
	double delta = 123.0;
	const int defaults = ogs::bank::phase(world.ws, &delta);
	CHECK(defaults == 0);
	CHECK(delta == Approx(0.0));
}

TEST_CASE("bank: calote (seizure) conserva o dinheiro - D-128") {
	ogs::test::PocketWorld world;
	world.ws.bank_default_days = 1;  // insolvente hoje -> executa hoje

	ogs::Bank& bank = ogs::bank::ensure(world.ws, "AAA",
		ogs::bank::kRolePublic);
	bank.cash = 0.0;
	bank.deposits = 500.0;  // o caixa do grupo e este deposito
	bank.loans = 0.0;
	bank.arrears = 1000.0;

	ogs::Group group = make_group("AAA_STATE", 500.0);
	group.credit_arrears = 1000.0;  // passivo -> patrimonio negativo
	world.ws.groups.push_back(group);
	world.core.rebuild_indexes();

	const double money_before = world.core.money_total();
	double delta = 0.0;
	const int defaults = ogs::bank::phase(world.ws, &delta);

	CHECK(defaults == 1);
	// A tomada troca deposito por caixa do banco: o fluxo BANCARIO do dia e 0.
	CHECK(delta == Approx(0.0));
	CHECK(world.core.money_total() == Approx(money_before));
	CHECK(world.ws.groups.back().cash == Approx(0.0));
	CHECK(world.ws.groups.back().credit_arrears == Approx(0.0));
	CHECK(bank.deposits == Approx(0.0));
	CHECK(bank.cash == Approx(500.0));
	CHECK(bank.arrears == Approx(0.0));
	CHECK(bank.writeoffs == Approx(500.0));
	CHECK(bank.defaults == 1);
}

TEST_CASE("bank: amortizacao destroi deposito e o delta fecha a identidade - D-128") {
	ogs::test::PocketWorld world;
	ogs::Bank& bank = ogs::bank::ensure(world.ws, "AAA",
		ogs::bank::kRolePublic);
	bank.cash = 0.0;
	bank.deposits = 1000.0;
	bank.loans = 400.0;

	ogs::Group group = make_group("AAA_AMORT", 1000.0);
	group.debt = 400.0;  // abaixo de metade do caixa: amortiza inteiro
	world.ws.groups.push_back(group);
	world.core.rebuild_indexes();

	// Juro do dia (taxa basica 0 sem moeda no fixture + spread 2 p.p.).
	const double interest = 400.0 * ogs::bank::loan_rate(world.ws, "AAA")
		/ ogs::bank::kDaysPerYear;
	const double money_before = world.core.money_total();
	double delta = 0.0;
	const int defaults = ogs::bank::phase(world.ws, &delta);

	CHECK(defaults == 0);
	// Sem caixa de giro (daily_cost 0), metade do caixa amortiza a divida.
	CHECK(world.ws.groups.back().debt == Approx(0.0));
	CHECK(bank.loans == Approx(0.0));
	CHECK(bank.deposits == Approx(1000.0 - interest - 400.0));
	CHECK(delta == Approx(-(400.0 + interest)));
	// money_after = money_before + bank_money_delta (o invariante da D-128).
	CHECK(world.core.money_total() == Approx(money_before + delta));
}

TEST_CASE("bank: patrimonio e lucro retido seguem ativos menos passivos") {
	ogs::Bank bank;
	bank.cash = 100.0;
	bank.loans = 200.0;
	bank.arrears = 50.0;
	bank.bc_borrowing = 20.0;
	bank.deposits = 150.0;
	bank.capital = 40.0;

	CHECK(ogs::bank::equity(bank) == Approx(180.0));
	CHECK(ogs::bank::retained(bank) == Approx(140.0));
}

TEST_CASE("bank: grupo estatal toma do banco publico") {
	ogs::test::PocketWorld world;
	ogs::bank::ensure(world.ws, "AAA", ogs::bank::kRolePublic);
	ogs::bank::ensure(world.ws, "AAA", ogs::bank::kRolePrivate);
	const ogs::Group group = make_group("AAA_STATE", 10.0);

	const ogs::Bank* lender = ogs::bank::lender(world.ws, group);
	REQUIRE(lender != nullptr);
	CHECK(lender->role == ogs::bank::kRolePublic);
}

TEST_CASE("bank: totais somam depositos e divida dos grupos") {
	ogs::test::PocketWorld world;
	ogs::Bank& bank = ogs::bank::ensure(world.ws, "AAA",
		ogs::bank::kRolePublic);
	bank.deposits = 250.0;

	ogs::Group first = make_group("AAA_ONE", 0.0);
	first.debt = 100.0;
	ogs::Group second = make_group("AAA_TWO", 0.0);
	second.debt = 50.0;
	world.ws.groups.push_back(first);
	world.ws.groups.push_back(second);

	CHECK(ogs::bank::deposits_total(world.ws) == Approx(250.0));
	CHECK(ogs::bank::total_group_debt(world.ws) == Approx(150.0));
}
