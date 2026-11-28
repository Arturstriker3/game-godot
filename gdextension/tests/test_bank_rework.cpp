// R2a (rework.banks) - testes do capital pago e dos depositos de funding.
// So C++ puro: nenhuma API do Godot, nenhum numero de jogo inventado (os
// valores aqui sao do fixture de bolso e existem para provar as identidades).
//
// Gates cobertos (§3.6 e §2.7 do docs/economy/rework/R2_plano.md):
//   * conservacao do ciclo (sweep/seed nao criam nem destroem dinheiro);
//   * claim de funding no ledger (Σ claims == Σ D_f, ao centavo);
//   * seed sem emissao (privado realocado das familias; publico = 0 no dia 0);
//   * colchao protegido (cesta + orcamento) preservado no sweep;
//   * papel `none`/`no_data` nunca capitalizado;
//   * dividendo: caixa debitado, Δdepositos = 0, `money_total` constante.
#include "doctest/doctest.h"
#include "pocket_world.h"

#include "sim/balance.h"
#include "sim/bank.h"
#include "sim/bank_crisis.h"
#include "sim/deposit.h"
#include "sim/save_state.h"
#include "sim/sim_core.h"
#include "sim/treasury.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

using doctest::Approx;

namespace {

void enable_banks(ogs::test::PocketWorld& world, bool capital, bool deposits) {
	world.ws.regional_cash = true;
	ogs::BanksRework cfg;
	cfg.enabled = true;
	cfg.capital = capital;
	cfg.deposits = deposits;
	world.core.set_banks_rework(cfg);
}

void enable_treasury(ogs::test::PocketWorld& world) {
	world.core.set_national_treasury(true, 1.0, 0.5, 0.35, 0.5);
}

ogs::Bank& present(ogs::test::PocketWorld& world, int role) {
	ogs::Bank& bank = ogs::bank::ensure(world.ws, "AAA", role);
	bank.name = "AAA bank";
	bank.name_status = "present";
	return bank;
}

double stratum_total(const ogs::Pop& pop) {
	double total = 0.0;
	for (int s = 0; s < ogs::kIncomeStrata; ++s) {
		total += std::max(0.0, pop.stratum_savings[s]);
	}
	return total;
}

// Soma dos claims de depositante (cidade + grupo) apontando para um banco.
double claims_to(const ogs::WorldState& ws, const std::string& bank_id) {
	double total = 0.0;
	for (const ogs::City& city : ws.cities) {
		for (const ogs::City::BankAccount& account : city.bank_accounts) {
			if (account.bank_id == bank_id) {
				total += std::max(0.0, account.firm)
					+ std::max(0.0, account.service);
			}
		}
	}
	for (const ogs::Group& group : ws.groups) {
		if (group.deposit_bank_id == bank_id) {
			total += std::max(0.0, group.bank_deposit);
		}
	}
	return total;
}

// R2b1: claim de divida por credor (o vetor e' ordenado por `bank_id`).
ogs::BankLoan claim_to(const std::string& bank_id, double principal,
		double arrears = 0.0) {
	ogs::BankLoan claim;
	claim.bank_id = bank_id;
	claim.principal = principal;
	claim.arrears = arrears;
	return claim;
}

void sort_claims(std::vector<ogs::BankLoan>& claims) {
	std::sort(claims.begin(), claims.end(),
		[](const ogs::BankLoan& a, const ogs::BankLoan& b) {
			return a.bank_id < b.bank_id;
		});
}

} // namespace

TEST_CASE("R2a: sweep conserva o dinheiro e cria claim igual ao funding") {
	ogs::test::PocketWorld world;
	enable_banks(world, true, true);
	// Sem piso de comida neste teste: todo o caixa privado e' excesso.
	world.ws.banks_rework.protected_food_months = 0.0;
	present(world, ogs::bank::kRolePrivate);

	ogs::City& city = world.ws.cities.front();
	city.unified_cash = true;
	city.regional_budget_limit_month = 0.0;
	city.pop.food_spend = 0.0;
	city.firm_sub = 300.0;
	city.service_sub = 200.0;
	city.firm_cash = 300.0;
	city.service_cash = 200.0;
	city.private_cash = 500.0;

	const double money_before = world.core.money_total();
	const double m0_before = world.core.m0_total();
	ogs::bank::sweep_deposits(world.ws);

	CHECK(world.core.money_total() == Approx(money_before));
	// O funding e' claim lastreado por caixa: nao entra no M0.
	CHECK(world.core.m0_total() == Approx(m0_before));
	CHECK(ogs::bank::deposit_claims_total(world.ws) == Approx(500.0));
	const ogs::bank::Totals totals = ogs::bank::totals(world.ws);
	CHECK(totals.deposits_funding == Approx(500.0));
	CHECK(totals.deposits == Approx(0.0));
	CHECK(city.private_cash == Approx(0.0));
	CHECK(city.firm_sub == Approx(0.0));
	CHECK(city.service_sub == Approx(0.0));
	CHECK(city.claim_total() == Approx(500.0));

	// Ledger: o claim do depositante aparece e fecha com Σ D_f.
	const ogs::balance::Ledger book = ogs::balance::ledger(world.ws,
		world.core.money_total());
	CHECK(book.deposit_claims == Approx(book.bank_deposits_funding));
	CHECK(book.bank_deposits_funding == Approx(500.0));
	CHECK(book.bank_deposits_loan == Approx(0.0));
}

TEST_CASE("R2a: colchao protegido (cesta + orcamento) nunca e' sweepado") {
	ogs::test::PocketWorld world;
	enable_banks(world, true, true);
	// 6 meses de cesta: 6 * (365/12) * 1,0 = 182,5 por dia de comida.
	world.ws.banks_rework.protected_food_months = 6.0;

	ogs::City& city = world.ws.cities.front();
	city.unified_cash = true;
	city.regional_budget_limit_month = 0.0;
	city.pop.food_spend = 1.0;
	const double floor = 6.0 * 365.0 / 12.0;
	city.firm_sub = 300.0;
	city.service_sub = 200.0;
	city.firm_cash = 300.0;
	city.service_cash = 200.0;
	city.private_cash = 500.0;

	ogs::bank::sweep_deposits(world.ws);

	CHECK(city.protected_cash == Approx(floor));
	CHECK(city.private_cash == Approx(floor));
	CHECK(city.firm_sub + city.service_sub == Approx(floor));
	// O sub que ficou abaixo da fatia protegida nao e' tocado a mais: o
	// excesso sai proporcional ao sub-saldo e o colchao fica inteiro.
	CHECK(city.firm_sub == Approx(floor * 0.6));
	CHECK(ogs::bank::deposit_claims_total(world.ws)
		== Approx(500.0 - floor));

	// Cidade abaixo do piso: nada sai.
	ogs::City& second = world.ws.cities.back();
	second.unified_cash = true;
	second.pop.food_spend = 10.0;
	second.firm_sub = 1.0;
	second.service_sub = 1.0;
	second.firm_cash = 1.0;
	second.service_cash = 1.0;
	second.private_cash = 2.0;
	ogs::bank::sweep_deposits(world.ws);
	CHECK(second.private_cash == Approx(2.0));
	CHECK(second.claim_total() == Approx(0.0));
}

TEST_CASE("R2a: seed capitaliza o privado das familias e o publico fica 0") {
	ogs::test::PocketWorld world;
	enable_banks(world, true, false);
	present(world, ogs::bank::kRolePrivate);

	// Alvo do modelo = 10% de 1000 de carteira steady = 100; so dois papeis
	// presentes (publico 0,55 / privado 0,45 renormalizados).
	world.ws.bank_seed.loaded = true;
	world.ws.bank_seed.capital_to_loans_target = 0.10;
	ogs::BankSeedRow row;
	row.steady_loans = 1000.0;
	row.confidence = "estimated";
	row.roles_present = {"public", "private"};
	world.ws.bank_seed.nations["AAA"] = row;

	// Familias com 1000 na camada mais alta (as duas camadas andam juntas).
	ogs::City& city = world.ws.cities.front();
	city.pop.stratum_savings[ogs::kIncomeStrata - 1] = 1000.0;
	city.pop.savings = stratum_total(city.pop);

	const double money_before = world.core.money_total();
	ogs::bank::seed_capital(world.ws);

	CHECK(world.ws.banks_rework.seed_source == "world_new");
	CHECK(world.core.money_total() == Approx(money_before));
	const ogs::Bank* public_bank = ogs::bank::find(world.ws, "AAA",
		ogs::bank::kRolePublic);
	const ogs::Bank* private_bank = ogs::bank::find(world.ws, "AAA",
		ogs::bank::kRolePrivate);
	REQUIRE(public_bank != nullptr);
	REQUIRE(private_bank != nullptr);
	CHECK(public_bank->capital == Approx(0.0));
	CHECK(public_bank->seed_scale == Approx(0.0));
	CHECK(public_bank->charter == "declared");
	CHECK(private_bank->capital > 0.0);
	// Shares C4 renormalizadas entre os papeis PRESENTES (0,55 publico e
	// 0,45 privado quando nao ha estrangeiro); aqui o peso declarado e
	// 0,45/0,40 -> privado = 0,40/0,85.
	CHECK(private_bank->capital_target
		== Approx(100.0 * 0.40 / (0.45 + 0.40)));
	CHECK(private_bank->seed_scale == Approx(1.0));
	CHECK(private_bank->capital == Approx(private_bank->cash));
	// Σ seed = Σ capital pago (so o privado).
	CHECK(ogs::bank::totals(world.ws).capital == Approx(private_bank->capital));
	// O seed sai das familias (realocacao), nunca de emissao.
	CHECK(city.pop.savings == Approx(1000.0 - private_bank->capital));

	// Idempotente: chamar de novo nao semeia outra vez.
	const double capital_once = private_bank->capital;
	ogs::bank::seed_capital(world.ws);
	CHECK(ogs::bank::find(world.ws, "AAA", ogs::bank::kRolePrivate)->capital
		== Approx(capital_once));
}

TEST_CASE("R2a: papel none/no_data nunca e' capitalizado") {
	ogs::test::PocketWorld world;
	enable_banks(world, true, false);

	ogs::Bank& private_bank = present(world, ogs::bank::kRolePrivate);
	private_bank.name_status = "none";
	private_bank.name.clear();
	world.ws.bank_seed.loaded = true;
	world.ws.bank_seed.capital_to_loans_target = 0.10;
	ogs::BankSeedRow row;
	row.steady_loans = 1000.0;
	row.roles_present = {"public", "private"};
	world.ws.bank_seed.nations["AAA"] = row;
	ogs::City& city = world.ws.cities.front();
	city.pop.stratum_savings[ogs::kIncomeStrata - 1] = 1000.0;
	city.pop.savings = stratum_total(city.pop);

	const double money_before = world.core.money_total();
	ogs::bank::seed_capital(world.ws);

	CHECK(world.core.money_total() == Approx(money_before));
	const ogs::Bank* none_bank = ogs::bank::find(world.ws, "AAA",
		ogs::bank::kRolePrivate);
	REQUIRE(none_bank != nullptr);
	CHECK(none_bank->capital == Approx(0.0));
	CHECK(none_bank->capital_target == Approx(0.0));
	CHECK(none_bank->seed_scale == Approx(0.0));
	CHECK(none_bank->charter == "none");
	// A share da C5 nao e' tocada pelo seed: a soma sobre as LINHAS de banco
	// existentes continua 1 (inclusive um papel `no_data`).
	const ogs::Bank* public_bank = ogs::bank::find(world.ws, "AAA",
		ogs::bank::kRolePublic);
	REQUIRE(public_bank != nullptr);
	CHECK(public_bank->share + none_bank->share == Approx(1.0));
}

TEST_CASE("R2a: dividendo debita caixa, nao cria deposito e conserva T") {
	ogs::test::PocketWorld world;
	enable_banks(world, true, false);
	world.ws.day = 30;  // fecho mensal
	world.ws.national_treasury = false;

	ogs::Bank& bank = present(world, ogs::bank::kRolePublic);
	bank.cash = 500.0;
	bank.capital = 100.0;
	bank.loans = 0.0;
	bank.arrears = 0.0;
	bank.deposits = 0.0;
	bank.deposits_funding = 0.0;
	bank.bc_borrowing = 0.0;

	const double money_before = world.core.money_total();
	ogs::bank::pay_dividends(world.ws);

	CHECK(bank.dividends == Approx(400.0));  // retained 400, sem piso
	CHECK(bank.cash == Approx(100.0));
	CHECK(bank.deposits == Approx(0.0));
	CHECK(bank.deposits_funding == Approx(0.0));
	CHECK(bank.capital == Approx(100.0));
	CHECK(world.core.money_total() == Approx(money_before));
}

TEST_CASE("R2a: seed.enabled=0 nao capitaliza nada") {
	ogs::test::PocketWorld world;
	enable_banks(world, true, false);
	world.ws.banks_rework.seed_enabled = false;
	present(world, ogs::bank::kRolePrivate);
	world.ws.bank_seed.loaded = true;
	world.ws.bank_seed.capital_to_loans_target = 0.10;
	ogs::BankSeedRow row;
	row.steady_loans = 1000.0;
	world.ws.bank_seed.nations["AAA"] = row;
	ogs::City& city = world.ws.cities.front();
	city.pop.stratum_savings[ogs::kIncomeStrata - 1] = 1000.0;
	city.pop.savings = stratum_total(city.pop);

	const double money_before = world.core.money_total();
	ogs::bank::seed_capital(world.ws);
	CHECK(ogs::bank::totals(world.ws).capital == Approx(0.0));
	CHECK(world.core.money_total() == Approx(money_before));
	CHECK(city.pop.savings == Approx(1000.0));
}

TEST_CASE("R2a: save v44 carrega sem capitalizar de novo") {
	ogs::test::PocketWorld source;
	enable_banks(source, true, false);
	present(source, ogs::bank::kRolePrivate);
	source.ws.bank_seed.loaded = true;
	source.ws.bank_seed.capital_to_loans_target = 0.10;
	ogs::BankSeedRow row;
	row.steady_loans = 1000.0;
	row.roles_present = {"public", "private"};
	source.ws.bank_seed.nations["AAA"] = row;
	ogs::City& city = source.ws.cities.front();
	city.pop.stratum_savings[ogs::kIncomeStrata - 1] = 1000.0;
	city.pop.savings = stratum_total(city.pop);
	ogs::bank::seed_capital(source.ws);
	const double capital_before = ogs::bank::find(source.ws, "AAA",
		ogs::bank::kRolePrivate)->capital;
	REQUIRE(capital_before > 0.0);

	const ogs::save::Value saved = ogs::save::to_value(source.ws,
		source.core);
	ogs::test::PocketWorld target;
	ogs::save::from_value(target.ws, target.core, saved);

	CHECK(target.ws.banks_rework.enabled);
	CHECK(target.ws.banks_rework.capital);
	CHECK(target.ws.banks_rework.seed_source == "loaded_save");
	const ogs::Bank* loaded = ogs::bank::find(target.ws, "AAA",
		ogs::bank::kRolePrivate);
	REQUIRE(loaded != nullptr);
	CHECK(loaded->capital == Approx(capital_before));
	CHECK(loaded->capital_target == Approx(source.ws.banks.find("AAA/private")
		->second.capital_target));
	// O load nao capitaliza retroativamente: chamar o gancho do dia 0 nao move
	// dinheiro nem capital de um mundo carregado.
	const double money_before = target.core.money_total();
	ogs::bank::seed_capital(target.ws);
	CHECK(ogs::bank::find(target.ws, "AAA", ogs::bank::kRolePrivate)->capital
		== Approx(capital_before));
	CHECK(target.core.money_total() == Approx(money_before));
}

TEST_CASE("R2a: funding do grupo entra no claim e o sweep e' conservativo") {
	ogs::test::PocketWorld world;
	enable_banks(world, true, true);
	present(world, ogs::bank::kRolePublic);

	// Grupo com caixa acima de 5 dias de custo. O `Sheet` vem do balance, que
	// le os predios do fixture; sem custo diario o sweep nao age — o teste
	// usa o proprio `daily_cost` medido para nao inventar numero.
	const std::vector<ogs::balance::GroupSheet> sheets =
		ogs::balance::group_sheets(world.ws);
	REQUIRE(!world.ws.groups.empty());
	REQUIRE(sheets.size() == world.ws.groups.size());
	ogs::Group& group = world.ws.groups.front();
	const double cost = std::max(0.0, sheets.front().daily_cost);
	group.cash = 5.0 * cost + 250.0;

	const double money_before = world.core.money_total();
	ogs::bank::sweep_deposits(world.ws);
	CHECK(world.core.money_total() == Approx(money_before));

	if (cost > 1e-9) {
		CHECK(group.bank_deposit == Approx(250.0));
		CHECK(group.cash == Approx(5.0 * cost));
		CHECK(!group.deposit_bank_id.empty());
		const ogs::balance::Ledger book = ogs::balance::ledger(world.ws,
			world.core.money_total());
		CHECK(book.deposit_claims == Approx(book.bank_deposits_funding));
	}
}

TEST_CASE("R2a: capitalizacao publica mensal (piso, ticket e deferral)") {
	SUBCASE("publico = 0 no dia 0 e alvo vem do seed") {
		ogs::test::PocketWorld world;
		enable_banks(world, true, false);
		enable_treasury(world);
		ogs::Bank& pub = present(world, ogs::bank::kRolePublic);
		world.ws.bank_seed.loaded = true;
		world.ws.bank_seed.capital_to_loans_target = 0.10;
		ogs::BankSeedRow row;
		row.steady_loans = 1000.0;
		row.roles_present = {"public"};
		world.ws.bank_seed.nations["AAA"] = row;

		const double money_before = world.core.money_total();
		ogs::bank::seed_capital(world.ws);
		CHECK(pub.capital_target == Approx(100.0));
		CHECK(pub.capital == Approx(0.0));
		CHECK(pub.cash == Approx(0.0));
		CHECK(world.core.money_total() == Approx(money_before));
		// O gancho e' mensal: no dia 0 nao paga nada.
		world.ws.day = 0;
		ogs::bank::capitalize_public_monthly(world.ws);
		CHECK(pub.capital == Approx(0.0));
	}

	SUBCASE("ticket = X * (saldo - piso 3 d), conservando T") {
		ogs::test::PocketWorld world;
		enable_banks(world, true, false);
		enable_treasury(world);
		ogs::Bank& pub = present(world, ogs::bank::kRolePublic);
		world.ws.bank_seed.loaded = true;
		world.ws.bank_seed.capital_to_loans_target = 0.10;
		ogs::BankSeedRow row;
		row.steady_loans = 1000.0;
		row.roles_present = {"public"};
		world.ws.bank_seed.nations["AAA"] = row;
		ogs::bank::seed_capital(world.ws);

		world.ws.day = 30;
		ogs::treasury::collect(world.ws, "AAA", 1000.0);
		// Gasto/dia = 300/30 = 10 -> piso de 3 d = 30; disponivel 970.
		world.ws.treasury_spent_total = 300.0;
		const double money_before = world.core.money_total();
		ogs::bank::capitalize_public_monthly(world.ws);
		CHECK(pub.capital == Approx(97.0));
		CHECK(pub.cash == Approx(97.0));
		CHECK(pub.public_cap_deferred.empty());
		CHECK(pub.seed_scale == Approx(0.97));
		CHECK(ogs::treasury::balance(world.ws, "AAA") == Approx(903.0));
		CHECK(world.core.money_total() == Approx(money_before));

		// O restante (3) cabe no ticket do mesmo fecho: completa o alvo.
		ogs::bank::capitalize_public_monthly(world.ws);
		CHECK(pub.capital == Approx(100.0));
		CHECK(pub.seed_scale == Approx(1.0));
	}

	SUBCASE("ticket abaixo do minimo posterga e declara") {
		ogs::test::PocketWorld world;
		enable_banks(world, true, false);
		enable_treasury(world);
		ogs::Bank& pub = present(world, ogs::bank::kRolePublic);
		world.ws.bank_seed.loaded = true;
		world.ws.bank_seed.capital_to_loans_target = 0.10;
		ogs::BankSeedRow row;
		row.steady_loans = 1000.0;
		row.roles_present = {"public"};
		world.ws.bank_seed.nations["AAA"] = row;
		ogs::bank::seed_capital(world.ws);

		world.ws.day = 30;
		ogs::treasury::collect(world.ws, "AAA", 100.0);
		world.ws.treasury_spent_total = 300.0;
		const double money_before = world.core.money_total();
		ogs::bank::capitalize_public_monthly(world.ws);
		// disponivel 70; ticket 7 < 25% de 100 -> posterga.
		CHECK(pub.capital == Approx(0.0));
		CHECK(pub.public_cap_deferred == "ticket_below_min");
		CHECK(ogs::treasury::balance(world.ws, "AAA") == Approx(100.0));
		CHECK(world.core.money_total() == Approx(money_before));
	}

	SUBCASE("Tesouro desligado e papel sem alvo declarados") {
		ogs::test::PocketWorld world;
		enable_banks(world, true, false);
		ogs::Bank& pub = present(world, ogs::bank::kRolePublic);
		world.ws.bank_seed.loaded = true;
		world.ws.bank_seed.capital_to_loans_target = 0.10;
		ogs::BankSeedRow row;
		row.steady_loans = 1000.0;
		row.roles_present = {"public"};
		world.ws.bank_seed.nations["AAA"] = row;
		ogs::bank::seed_capital(world.ws);

		world.ws.day = 30;
		ogs::bank::capitalize_public_monthly(world.ws);
		CHECK(pub.capital == Approx(0.0));
		CHECK(pub.public_cap_deferred == "treasury_disabled");

		ogs::test::PocketWorld empty;
		enable_banks(empty, true, false);
		ogs::Bank& no_target = present(empty, ogs::bank::kRolePublic);
		empty.ws.day = 30;
		ogs::bank::capitalize_public_monthly(empty.ws);
		CHECK(no_target.capital == Approx(0.0));
		CHECK(no_target.public_cap_deferred == "no_model_target");
	}
}

TEST_CASE("R2a: bail-in pro rata entre D_f e D_l e reassign conserva claims") {
	ogs::test::PocketWorld world;
	enable_banks(world, true, true);
	ogs::Bank& pub = present(world, ogs::bank::kRolePublic);
	ogs::Bank& failing = present(world, ogs::bank::kRolePrivate);
	// O sorteio de tomador tem de cair no banco que quebra.
	pub.share = 0.0;
	failing.share = 1.0;

	failing.capital = 0.0;
	failing.cash = 0.0;
	failing.loans = 0.0;
	failing.arrears = 0.0;
	failing.deposits = 1000.0;          // D_l
	failing.deposits_funding = 1000.0;  // D_f
	// Claim do depositante do funding, igual ao passivo.
	ogs::City& city = world.ws.cities.front();
	city.bank_accounts.clear();
	ogs::City::BankAccount account;
	account.bank_id = failing.id;
	account.firm = 1000.0;
	city.bank_accounts.push_back(account);
	// Tomador do banco com caixa para o corte de D_l.
	ogs::Group& group = world.ws.groups.front();
	group.state_owned = false;
	group.debt = 1000.0;
	group.credit_arrears = 0.0;
	group.cash = 400.0;
	// Sem fundo/Tesouro: o residual chega a incorporacao pelo publico.
	world.ws.bank_crisis.treasury_debt_rescue = false;
	world.ws.bank_crisis.takeover_public = true;
	world.ws.national_treasury = false;

	const double money_before = world.core.money_total();
	const double gap = ogs::bank_crisis::resolve(world.ws, failing.id);
	// 0,4 * 2000 de passivo nao segurado = 800, metade em cada tipo.
	CHECK(gap == Approx(2000.0));
	CHECK(failing.bailin_cut == Approx(800.0));
	CHECK(group.cash == Approx(0.0));
	// O corte de D_l destroi caixa do tomador (a variacao fica registrada).
	CHECK(world.core.money_total() == Approx(money_before - 400.0));

	// Incorporacao pelo publico: passivo e claim andam juntos (o banco quebrado
	// fica como casca zerada, com a ancora da resolucao).
	REQUIRE(failing.absorbed_into == pub.id);
	CHECK(failing.deposits == Approx(0.0));
	CHECK(pub.deposits == Approx(600.0));
	CHECK(pub.deposits_funding == Approx(600.0));
	CHECK(claims_to(world.ws, pub.id) == Approx(pub.deposits_funding));
	CHECK(claims_to(world.ws, failing.id) == Approx(0.0));
	CHECK(ogs::bank::deposit_claims_total(world.ws)
		== Approx(ogs::bank::totals(world.ws).deposits_funding));
}

TEST_CASE("R2a: sweep OFF (deposits=false) e' no-op") {
	ogs::test::PocketWorld world;
	enable_banks(world, true, false);
	present(world, ogs::bank::kRolePrivate);

	ogs::City& city = world.ws.cities.front();
	city.unified_cash = true;
	city.pop.food_spend = 0.0;
	city.firm_sub = 300.0;
	city.service_sub = 200.0;
	city.firm_cash = 300.0;
	city.service_cash = 200.0;
	city.private_cash = 500.0;

	const double money_before = world.core.money_total();
	const double m0_before = world.core.m0_total();
	const double broad_before = world.core.broad_money();
	ogs::bank::sweep_deposits(world.ws);

	CHECK(city.private_cash == Approx(500.0));
	CHECK(city.firm_sub == Approx(300.0));
	CHECK(city.service_sub == Approx(200.0));
	CHECK(city.claim_total() == Approx(0.0));
	CHECK(city.protected_cash == Approx(0.0));
	CHECK(ogs::bank::totals(world.ws).deposits_funding == Approx(0.0));
	CHECK(world.core.money_total() == Approx(money_before));
	CHECK(world.core.m0_total() == Approx(m0_before));
	CHECK(world.core.broad_money() == Approx(broad_before));
}

TEST_CASE("R2a: load de v43 (sem bloco de politica) com flag ON nao capitaliza") {
	ogs::test::PocketWorld source;
	source.ws.regional_cash = true;
	ogs::Bank& saved_bank = present(source, ogs::bank::kRolePrivate);
	saved_bank.capital = 0.0;
	saved_bank.cash = 0.0;
	const ogs::save::Value saved = ogs::save::to_value(source.ws,
		source.core);
	const ogs::save::Value& banks = saved.get("banks", ogs::save::Value());
	REQUIRE(banks.get_type() == ogs::save::Value::DICTIONARY);
	REQUIRE_FALSE(banks.has("rework"));

	ogs::test::PocketWorld target;
	target.core.set_regional_cash(true);
	ogs::BanksRework cfg;
	cfg.enabled = true;
	cfg.capital = true;
	target.core.set_banks_rework(cfg);
	ogs::save::from_value(target.ws, target.core, saved);

	CHECK(target.ws.banks_rework.enabled);
	CHECK(target.ws.banks_rework.capital);
	CHECK(target.ws.banks_rework.seed_source == "loaded_save");
	// O estado salvo (capital 0 do v43) nao e' re-semeado.
	target.ws.bank_seed.loaded = true;
	target.ws.bank_seed.capital_to_loans_target = 0.10;
	ogs::BankSeedRow row;
	row.steady_loans = 1000.0;
	row.roles_present = {"private"};
	target.ws.bank_seed.nations["AAA"] = row;
	ogs::City& city = target.ws.cities.front();
	city.pop.stratum_savings[ogs::kIncomeStrata - 1] = 1000.0;
	city.pop.savings = stratum_total(city.pop);
	const double money_before = target.core.money_total();
	ogs::bank::seed_capital(target.ws);
	const ogs::Bank* loaded = ogs::bank::find(target.ws, "AAA",
		ogs::bank::kRolePrivate);
	REQUIRE(loaded != nullptr);
	CHECK(loaded->capital == Approx(0.0));
	CHECK(target.core.money_total() == Approx(money_before));
}

TEST_CASE("R2a: reconfigurar depois do load nao reabilita a capitalizacao (M1)") {
	ogs::test::PocketWorld source;
	enable_banks(source, true, false);
	present(source, ogs::bank::kRolePublic);
	source.ws.bank_seed.loaded = true;
	source.ws.bank_seed.capital_to_loans_target = 0.10;
	ogs::BankSeedRow row;
	row.steady_loans = 1000.0;
	row.roles_present = {"public"};
	source.ws.bank_seed.nations["AAA"] = row;
	ogs::bank::seed_capital(source.ws);
	const ogs::save::Value saved = ogs::save::to_value(source.ws,
		source.core);

	ogs::test::PocketWorld target;
	ogs::save::from_value(target.ws, target.core, saved);
	REQUIRE(target.ws.banks_rework.seed_source == "loaded_save");
	const ogs::Bank* pub = ogs::bank::find(target.ws, "AAA",
		ogs::bank::kRolePublic);
	REQUIRE(pub != nullptr);
	const double capital_before = pub->capital;

	// O que o economy.gd faz ao reaplicar o rework.json: um cfg NOVO (sem
	// seed_source). O estado do mundo carregado nao pode ser apagado.
	ogs::BanksRework again;
	again.enabled = true;
	again.capital = true;
	again.dividend_target_ratio = 0.2;
	target.core.set_banks_rework(again);
	CHECK(target.ws.banks_rework.enabled);
	CHECK(target.ws.banks_rework.dividend_target_ratio == Approx(0.2));
	CHECK(target.ws.banks_rework.seed_source == "loaded_save");

	// Nem o gancho publico mensal nem o seed privado rodam no mundo carregado.
	enable_treasury(target);
	ogs::treasury::collect(target.ws, "AAA", 1000.0);
	target.ws.day = 30;
	target.ws.treasury_spent_total = 300.0;
	const double money_before = target.core.money_total();
	ogs::bank::capitalize_public_monthly(target.ws);
	ogs::bank::seed_capital(target.ws);
	CHECK(pub->capital == Approx(capital_before));
	CHECK(target.core.money_total() == Approx(money_before));
	CHECK(ogs::treasury::balance(target.ws, "AAA") == Approx(1000.0));
}

TEST_CASE("R2a: dividendo de familias/exterior e retencao pela rampa") {
	SUBCASE("familia: caixa debitado e savings creditadas, T constante") {
		ogs::test::PocketWorld world;
		enable_banks(world, true, false);
		world.ws.day = 0;
		ogs::Bank& bank = present(world, ogs::bank::kRolePrivate);
		bank.owner_kind = "national";
		bank.cash = 50.0;
		bank.loans = 1000.0;
		bank.deposits = 1000.0;
		bank.deposits_funding = 0.0;
		bank.capital = 0.0;
		world.ws.banks_rework.dividend_cash_floor_ratio = 0.0;
		ogs::City& city = world.ws.cities.front();
		const double savings_before = city.pop.savings;
		const double money_before = world.core.money_total();
		ogs::bank::pay_dividends(world.ws);
		// No dia 0 a rampa usa a razao atual (5%) -> nada a reter.
		CHECK(bank.dividends == Approx(50.0));
		CHECK(bank.cash == Approx(0.0));
		CHECK(city.pop.savings == Approx(savings_before + 50.0));
		CHECK(world.core.money_total() == Approx(money_before));
	}

	SUBCASE("exterior: credita a posicao externa, T constante") {
		ogs::test::PocketWorld world;
		enable_banks(world, true, false);
		world.ws.day = 0;
		ogs::Bank& bank = present(world, ogs::bank::kRoleForeign);
		bank.owner_kind = "foreign";
		bank.cash = 50.0;
		bank.loans = 1000.0;
		bank.deposits = 1000.0;
		bank.deposits_funding = 0.0;
		bank.capital = 0.0;
		world.ws.banks_rework.dividend_cash_floor_ratio = 0.0;
		const double foreign_before = world.ws.foreign_cash;
		const double money_before = world.core.money_total();
		ogs::bank::pay_dividends(world.ws);
		CHECK(bank.dividends == Approx(50.0));
		CHECK(world.ws.foreign_cash == Approx(foreign_before + 50.0));
		CHECK(bank.remittance == Approx(50.0));
		CHECK(world.core.money_total() == Approx(money_before));
	}

	SUBCASE("rampa de 24 meses: no dia 730 a meta retem o lucro") {
		ogs::test::PocketWorld world;
		enable_banks(world, true, false);
		world.ws.day = 730;
		ogs::Bank& bank = present(world, ogs::bank::kRolePrivate);
		bank.owner_kind = "national";
		bank.cash = 50.0;
		bank.loans = 1000.0;
		bank.deposits = 1000.0;
		bank.deposits_funding = 0.0;
		bank.capital = 0.0;
		world.ws.banks_rework.dividend_cash_floor_ratio = 0.0;
		const double money_before = world.core.money_total();
		ogs::bank::pay_dividends(world.ws);
		// equity = 50, carteira = 1000: 10% exige 100 de patrimonio > lucro 50.
		CHECK(bank.dividends == Approx(0.0));
		CHECK(bank.cash == Approx(50.0));
		CHECK(world.core.money_total() == Approx(money_before));
	}
}

TEST_CASE("R2a: broad constante e M1 caindo no sweep") {
	ogs::test::PocketWorld world;
	enable_banks(world, true, true);
	world.ws.banks_rework.protected_food_months = 0.0;
	present(world, ogs::bank::kRolePrivate);

	ogs::City& city = world.ws.cities.front();
	city.unified_cash = true;
	city.regional_budget_limit_month = 0.0;
	city.pop.food_spend = 0.0;
	city.firm_sub = 300.0;
	city.service_sub = 200.0;
	city.firm_cash = 300.0;
	city.service_cash = 200.0;
	city.private_cash = 500.0;

	const double money_before = world.core.money_total();
	const double m0_before = world.core.m0_gross();
	const double broad_before = world.core.broad_money();
	const double banks_cash_before = ogs::bank::totals(world.ws).cash;
	ogs::bank::sweep_deposits(world.ws);

	CHECK(world.core.money_total() == Approx(money_before));
	CHECK(world.core.m0_gross() == Approx(m0_before));
	CHECK(world.core.broad_money() == Approx(broad_before));
	// M1_legacy = T - S bank.cash: o sweep tira 500 do dinheiro fora dos bancos.
	const double m1_before = money_before - banks_cash_before;
	const double m1_after = world.core.money_total()
		- ogs::bank::totals(world.ws).cash;
	CHECK(m1_after == Approx(m1_before - 500.0));
	CHECK(ogs::bank::totals(world.ws).deposits_funding == Approx(500.0));
	CHECK(ogs::bank::deposit_claims_total(world.ws) == Approx(500.0));
}

TEST_CASE("R2a: colchao usa a media de 30 d do food_spend") {
	ogs::test::PocketWorld world;
	enable_banks(world, true, true);
	world.ws.banks_rework.protected_food_months = 6.0;

	ogs::City& city = world.ws.cities.front();
	city.unified_cash = true;
	city.regional_budget_limit_month = 0.0;
	city.private_cash = 0.0;
	city.food_spend_hist_n = 0;
	city.food_spend_hist_pos = 0;
	city.food_spend_hist_day = -1;
	for (int day = 1; day <= 30; ++day) {
		world.ws.day = day;
		city.pop.food_spend = 1.0;
		ogs::bank::sweep_deposits(world.ws);
	}
	const double full_month = 6.0 * 365.0 / 12.0;
	CHECK(city.protected_cash == Approx(full_month));

	// Dia de escassez (gasto instantaneo 0,1): o colchao NAO encolhe junto.
	world.ws.day = 31;
	city.pop.food_spend = 0.1;
	ogs::bank::sweep_deposits(world.ws);
	const double average = (29.0 * 1.0 + 0.1) / 30.0;
	CHECK(city.protected_cash == Approx(full_month * average));
	CHECK(city.protected_cash > 0.9 * full_month);
}

TEST_CASE("R2b1: saque atomico movo claim/D_f/caixa juntos e conserva") {
	ogs::test::PocketWorld world;
	enable_banks(world, true, true);
	world.ws.banks_rework.protected_food_months = 0.0;
	ogs::bank::ensure(world.ws, "AAA",
		ogs::bank::kRolePublic).name_status = "no_data";
	ogs::Bank& bank = present(world, ogs::bank::kRolePrivate);

	ogs::City& city = world.ws.cities.front();
	city.unified_cash = true;
	city.regional_budget_limit_month = 0.0;
	city.pop.food_spend = 0.0;
	city.firm_sub = 300.0;
	city.service_sub = 200.0;
	city.firm_cash = 300.0;
	city.service_cash = 200.0;
	city.private_cash = 500.0;
	ogs::bank::sweep_deposits(world.ws);
	REQUIRE(city.claim_total() == Approx(500.0));
	REQUIRE(bank.cash == Approx(500.0));

	const double money_before = world.core.money_total();
	const double m0_before = world.core.m0_total();
	const double broad_before = world.core.broad_money();
	const double x = ogs::deposit::withdraw_city(world.ws, city, true, 200.0);

	CHECK(x == Approx(200.0));
	CHECK(city.firm_sub == Approx(200.0));
	CHECK(city.service_sub == Approx(0.0));
	CHECK(city.private_cash == Approx(200.0));
	CHECK(city.claim_total() == Approx(300.0));
	CHECK(bank.cash == Approx(300.0));
	CHECK(bank.deposits_funding == Approx(300.0));
	CHECK(world.core.money_total() == Approx(money_before));
	CHECK(world.core.m0_total() == Approx(m0_before));
	CHECK(world.core.broad_money() == Approx(broad_before));
	const ogs::balance::Ledger book = ogs::balance::ledger(world.ws,
		world.core.money_total());
	CHECK(book.deposit_claims == Approx(book.bank_deposits_funding));
}

TEST_CASE("R2b1: pagamento usa caixa primeiro e deposito depois") {
	ogs::test::PocketWorld world;
	enable_banks(world, true, true);
	world.ws.banks_rework.protected_food_months = 0.0;
	ogs::bank::ensure(world.ws, "AAA",
		ogs::bank::kRolePublic).name_status = "no_data";
	ogs::Bank& bank = present(world, ogs::bank::kRolePrivate);

	ogs::City& city = world.ws.cities.front();
	city.unified_cash = true;
	city.regional_budget_limit_month = 0.0;
	city.pop.food_spend = 0.0;
	city.firm_sub = 300.0;
	city.service_sub = 200.0;
	city.firm_cash = 300.0;
	city.service_cash = 200.0;
	city.private_cash = 500.0;
	ogs::bank::sweep_deposits(world.ws);
	REQUIRE(city.private_cash == Approx(0.0));
	REQUIRE(city.claim_total() == Approx(500.0));

	// Float de 100 na frente do deposito: 100 de caixa + 150 de claim.
	city.firm_receive(100.0);
	const double paid = city.firm_pay(250.0);
	CHECK(paid == Approx(250.0));
	CHECK(city.firm_sub == Approx(0.0));
	CHECK(city.private_cash == Approx(0.0));
	CHECK(city.claim_total() == Approx(350.0));
	CHECK(bank.cash == Approx(350.0));
	CHECK(bank.deposits_funding == Approx(350.0));
}

TEST_CASE("R2b1: pagamento inteiro coberto pelo deposito") {
	ogs::test::PocketWorld world;
	enable_banks(world, true, true);
	world.ws.banks_rework.protected_food_months = 0.0;
	ogs::bank::ensure(world.ws, "AAA",
		ogs::bank::kRolePublic).name_status = "no_data";
	ogs::Bank& bank = present(world, ogs::bank::kRolePrivate);

	ogs::City& city = world.ws.cities.front();
	city.unified_cash = true;
	city.regional_budget_limit_month = 0.0;
	city.pop.food_spend = 0.0;
	city.firm_sub = 200.0;
	city.service_sub = 0.0;
	city.firm_cash = 200.0;
	city.service_cash = 0.0;
	city.private_cash = 200.0;
	ogs::bank::sweep_deposits(world.ws);
	REQUIRE(city.private_cash == Approx(0.0));

	const double money_before = world.core.money_total();
	const double paid = city.firm_pay(150.0);
	CHECK(paid == Approx(150.0));
	CHECK(city.private_cash == Approx(0.0));
	CHECK(city.claim_total() == Approx(50.0));
	CHECK(bank.cash == Approx(50.0));
	CHECK(bank.deposits_funding == Approx(50.0));
}

TEST_CASE("R2b1: grupo paga juro com o deposito sacavel (pay_group_capped)") {
	ogs::test::PocketWorld world;
	enable_banks(world, true, true);
	world.ws.day = 0;
	world.ws.bank_credit_enabled = true;
	world.ws.bank_default_days = 30;
	world.ws.crisis.enabled = false;
	ogs::bank::ensure(world.ws, "AAA",
		ogs::bank::kRolePublic).name_status = "no_data";
	ogs::Bank& bank = present(world, ogs::bank::kRolePrivate);
	REQUIRE(!world.ws.groups.empty());
	ogs::Group& group = world.ws.groups.front();

	// O caminho REAL de producao e' `bank::phase` -> `pay_group_capped`
	// (deposit::pay_group foi removido: so' o teste o chamava).
	const double rate = ogs::bank::loan_rate(world.ws, "AAA");
	const double interest = 400.0 * rate / ogs::bank::kDaysPerYear;
	REQUIRE(interest > 1e-9);
	group.cash = 0.0;
	group.debt = 400.0;
	group.credit_arrears = 0.0;
	group.credit_rate_add = 0.0;
	group.credit_default_days = 0;
	group.debt_claims = {claim_to(bank.id, 400.0)};
	group.bank_deposit = 400.0;
	group.deposit_bank_id = bank.id;
	bank.cash = interest;
	bank.deposits_funding = 400.0;
	bank.loans = 400.0;
	bank.deposits = 0.0;
	ogs::deposit::link(world.ws);

	const double money_before = world.core.money_total();
	double money_delta = 0.0;
	const int defaults = ogs::bank::phase(world.ws, &money_delta);

	CHECK(defaults == 0);
	// Juro pago pelo claim: sai do funding e volta ao caixa do banco; o
	// principal e o agregado ficam (juro nao e' capitalizado). O grupo do
	// fixture ainda tem um excedente minimo ao capital de giro, entao a
	// amortizacao do dia (fase 2 do servicing) roda; o teste mede a
	// identidade exata em vez de fingir que ela nao existe.
	const double repaid = group.credit_repaid;
	CHECK(repaid >= 0.0);
	CHECK(bank.interest_income == Approx(interest));
	CHECK(group.debt_claims[0].principal == Approx(400.0 - repaid));
	CHECK(group.bank_deposit
		== Approx(400.0 - interest - repaid));
	CHECK(bank.deposits_funding
		== Approx(400.0 - interest - repaid));
	CHECK(bank.cash == Approx(interest));
	CHECK(group.debt == Approx(400.0 - repaid));
	CHECK(bank.loans == Approx(400.0 - repaid));
	// A parcela paga pelo claim nao destroi M0 (so' troca D_f por caixa).
	CHECK(money_delta == Approx(0.0));
	CHECK(world.core.money_total() == Approx(money_before));
}

// ---------------------------------------------------------------------------
// R2b1: servicing por credor, bloqueio de execucao multi-credor, C6 por claim
// e competicao deterministica por taxa/capacidade.
// ---------------------------------------------------------------------------

TEST_CASE("R2b1: grupo com dois credores paga cada juro ao banco do claim") {
	ogs::test::PocketWorld world;
	enable_banks(world, true, true);
	world.ws.day = 0;
	world.ws.bank_credit_enabled = true;
	ogs::Bank& pub = present(world, ogs::bank::kRolePublic);
	ogs::Bank& pri = present(world, ogs::bank::kRolePrivate);
	REQUIRE(!world.ws.groups.empty());
	ogs::Group& group = world.ws.groups.front();

	const double rate = ogs::bank::loan_rate(world.ws, "AAA");
	const double interest_pub = 600.0 * rate / ogs::bank::kDaysPerYear;
	const double interest_pri = 400.0 * rate / ogs::bank::kDaysPerYear;
	group.cash = interest_pub + interest_pri;
	group.debt = 1000.0;
	group.credit_arrears = 0.0;
	group.credit_rate_add = 0.0;
	group.bank_deposit = 0.0;
	group.debt_claims = {claim_to(pub.id, 600.0),
		claim_to(pri.id, 400.0)};
	sort_claims(group.debt_claims);
	pub.loans = 600.0;
	pub.deposits = interest_pub;
	pri.loans = 400.0;
	pri.deposits = interest_pri;

	double money_delta = 0.0;
	const int defaults = ogs::bank::phase(world.ws, &money_delta);

	CHECK(defaults == 0);
	CHECK(pub.interest_income == Approx(interest_pub));
	CHECK(pri.interest_income == Approx(interest_pri));
	// Juro nao e' capitalizado: principal e agregado ficam iguais.
	CHECK(group.debt == Approx(1000.0));
	CHECK(group.credit_arrears == Approx(0.0));
	CHECK(group.debt_claims[0].principal == Approx(400.0));
	CHECK(group.debt_claims[1].principal == Approx(600.0));
	CHECK(money_delta == Approx(-(interest_pub + interest_pri)));
}

TEST_CASE("R2b1: execucao multi-credor e' bloqueada e o dia conta uma vez") {
	ogs::test::PocketWorld world;
	enable_banks(world, true, true);
	world.ws.day = 0;
	world.ws.bank_credit_enabled = true;
	world.ws.bank_default_days = 1;
	world.ws.crisis.enabled = false;
	ogs::Bank& pub = present(world, ogs::bank::kRolePublic);
	ogs::Bank& pri = present(world, ogs::bank::kRolePrivate);
	REQUIRE(!world.ws.groups.empty());
	ogs::Group& group = world.ws.groups.front();

	group.cash = 0.0;
	group.debt = 2.0e9;
	group.credit_arrears = 0.0;
	group.credit_rate_add = 0.0;
	group.credit_default_days = 0;
	group.debt_claims = {claim_to(pub.id, 1.0e9),
		claim_to(pri.id, 1.0e9)};
	sort_claims(group.debt_claims);
	pub.loans = 1.0e9;
	pub.deposits = 0.0;
	pri.loans = 1.0e9;
	pri.deposits = 0.0;

	const int defaults = ogs::bank::phase(world.ws);

	// Dois credores em aberto: nenhum executa (o caixa seria tomado 2x).
	CHECK(defaults == 0);
	CHECK(group.debt == Approx(2.0e9));
	CHECK(group.debt_claims[0].principal == Approx(1.0e9));
	CHECK(group.debt_claims[1].principal == Approx(1.0e9));
	// O dia de atraso conta UMA vez, nao uma por credor.
	CHECK(group.credit_default_days == 1);
	REQUIRE(world.building("PORTUS_SAWMILL") != nullptr);
	CHECK(!world.building("PORTUS_SAWMILL")->bankrupt);
}

TEST_CASE("R2b1: cidade com dois credores paga cada juro ao banco do claim") {
	ogs::test::PocketWorld world;
	enable_banks(world, true, true);
	world.ws.day = 0;
	world.ws.bank_credit_enabled = true;
	ogs::Bank& pub = present(world, ogs::bank::kRolePublic);
	ogs::Bank& pri = present(world, ogs::bank::kRolePrivate);
	ogs::City& city = world.ws.cities.front();

	const double rate = ogs::bank::loan_rate(world.ws, "AAA");
	const double interest_pub = 600.0 * rate / ogs::bank::kDaysPerYear;
	const double interest_pri = 400.0 * rate / ogs::bank::kDaysPerYear;
	const double interest = interest_pub + interest_pri;
	city.unified_cash = true;
	city.firm_sub = interest;
	city.firm_cash = interest;
	city.service_sub = 0.0;
	city.service_cash = 0.0;
	city.private_cash = interest;
	city.firm_debt = 1000.0;
	city.firm_credit_arrears = 0.0;
	city.firm_credit_rate_add = 0.0;
	city.firm_credit_default_days = 0;
	city.firm_debt_claims = {claim_to(pub.id, 600.0),
		claim_to(pri.id, 400.0)};
	sort_claims(city.firm_debt_claims);
	pub.loans = 600.0;
	pub.deposits = interest_pub;
	pri.loans = 400.0;
	pri.deposits = interest_pri;

	const int defaults = ogs::bank::phase(world.ws);

	CHECK(defaults == 0);
	CHECK(pub.interest_income == Approx(interest_pub));
	CHECK(pri.interest_income == Approx(interest_pri));
	CHECK(city.firm_debt == Approx(1000.0));
	CHECK(city.firm_credit_arrears == Approx(0.0));
	CHECK(city.firm_debt_claims[0].principal == Approx(400.0));
	CHECK(city.firm_debt_claims[1].principal == Approx(600.0));
}

TEST_CASE("R2b1: apply_loss baixa pro rata so o claim do credor") {
	ogs::test::PocketWorld world;
	enable_banks(world, true, true);
	ogs::Bank& pub = present(world, ogs::bank::kRolePublic);
	ogs::Bank& pri = present(world, ogs::bank::kRolePrivate);
	REQUIRE(!world.ws.groups.empty());
	ogs::Group& group = world.ws.groups.front();
	ogs::City& city = world.ws.cities.front();

	group.debt = 1000.0;
	group.credit_arrears = 0.0;
	group.debt_claims = {claim_to(pub.id, 600.0),
		claim_to(pri.id, 400.0)};
	sort_claims(group.debt_claims);
	city.firm_debt = 400.0;
	city.firm_credit_arrears = 0.0;
	city.firm_debt_claims = {claim_to(pub.id, 400.0)};
	pub.loans = 1000.0;
	pub.arrears = 0.0;
	pri.loans = 400.0;

	const double written = ogs::bank_crisis::apply_loss(world.ws, pub.id,
		300.0);

	// Book do credor = 600 (grupo) + 400 (cidade); 300 pro rata.
	CHECK(written == Approx(300.0));
	CHECK(group.debt_claims[0].principal == Approx(400.0));
	CHECK(group.debt_claims[1].principal == Approx(420.0));
	CHECK(city.firm_debt_claims[0].principal == Approx(280.0));
	CHECK(group.debt == Approx(820.0));
	CHECK(city.firm_debt == Approx(280.0));
	CHECK(pub.loans == Approx(700.0));
	CHECK(pub.writeoffs == Approx(300.0));
	// O outro credor nao e' tocado.
	CHECK(pri.loans == Approx(400.0));
}

namespace {

// Cria os dois papeis e tenta um emprestimo de `amount`; devolve o credor
// gravado no claim (vazio quando recusado). `private_first` varia a ordem de
// criacao para provar que a escolha nao depende dela.
std::string lend_choice(bool private_first, double pub_cash,
		double pri_cash, double pri_deposits, double amount,
		bool* ok_out = nullptr) {
	ogs::test::PocketWorld world;
	enable_banks(world, true, true);
	world.ws.day = 0;
	world.ws.bank_credit_enabled = true;
	if (private_first) {
		present(world, ogs::bank::kRolePrivate);
		present(world, ogs::bank::kRolePublic);
	} else {
		present(world, ogs::bank::kRolePublic);
		present(world, ogs::bank::kRolePrivate);
	}
	ogs::Bank& pub = world.ws.banks[ogs::bank::bank_id("AAA",
		ogs::bank::kRolePublic)];
	ogs::Bank& pri = world.ws.banks[ogs::bank::bank_id("AAA",
		ogs::bank::kRolePrivate)];
	REQUIRE(!world.ws.groups.empty());
	ogs::Group& group = world.ws.groups.front();
	group.cash = 0.0;
	group.debt = 0.0;
	group.credit_arrears = 0.0;
	group.debt_claims.clear();
	ogs::Building* sawmill = world.building("PORTUS_SAWMILL");
	REQUIRE(sawmill != nullptr);
	sawmill->capital_per_capacity = 1000.0;
	sawmill->profit = 1000.0;
	// Capacidade = equity/0,10 - loans; liquidez = cash/0,10 - D_t.
	pub.loans = 1000.0;
	pub.deposits = 1000.0;
	pub.cash = pub_cash;
	pri.loans = 1000.0;
	pri.deposits = pri_deposits;
	pri.cash = pri_cash;
	double out = 0.0;
	std::string error;
	const bool ok = ogs::bank::lend(world.ws, group.id, amount, &out,
		&error);
	if (ok_out != nullptr) {
		*ok_out = ok;
	}
	if (!ok || group.debt_claims.empty()) {
		return std::string();
	}
	return group.debt_claims.front().bank_id;
}

} // namespace

TEST_CASE("R2b1: credito escolhe a menor taxa com capacidade (deterministico)") {
	// Privado mais solido => taxa menor; ambos com capacidade positiva.
	const std::string private_choice = lend_choice(false, 250.0, 500.0,
		1000.0, 100.0);
	CHECK(private_choice == ogs::bank::bank_id("AAA",
		ogs::bank::kRolePrivate));
	// A ordem de criacao dos bancos nao muda a escolha.
	CHECK(lend_choice(true, 250.0, 500.0, 1000.0, 100.0)
		== private_choice);

	// Empate de taxa/capacidade: desempate deterministico por id
	// ("AAA/private" < "AAA/public").
	CHECK(lend_choice(false, 500.0, 500.0, 1000.0, 10.0)
		== ogs::bank::bank_id("AAA", ogs::bank::kRolePrivate));
}

TEST_CASE("R2b1: banco sem capacidade nao e' escolhido") {
	// Privado mais solido (taxa menor) mas sem caixa: liquidez 0; o publico
	// (capacidade 500) e' o unico elegivel.
	bool ok = false;
	const std::string choice = lend_choice(false, 250.0, 0.0, 500.0, 100.0,
		&ok);
	CHECK(ok);
	CHECK(choice == ogs::bank::bank_id("AAA", ogs::bank::kRolePublic));
}

TEST_CASE("R2b1: alocacao mensal move claim e lastro juntos") {
	ogs::test::PocketWorld world;
	enable_banks(world, true, true);
	ogs::Bank& a = present(world, ogs::bank::kRolePublic);
	ogs::Bank& b = present(world, ogs::bank::kRolePrivate);
	// Score = solidez pura: sem relacionamento e sem pendulo.
	world.ws.banks_rework.alloc_w_rel = 0.0;
	world.ws.banks_rework.alloc_w_sol = 1.0;
	world.ws.banks_rework.alloc_w_pend = 0.0;
	world.ws.banks_rework.alloc_min_capital_ratio = 0.08;
	a.loans = 1000.0;
	a.deposits = 800.0;
	a.cash = 100.0;      // equity 300 -> solidez 0,30
	b.loans = 1000.0;
	b.deposits = 500.0;
	b.cash = 500.0;      // equity 1000 -> solidez 1,00

	ogs::City& city = world.ws.cities.front();
	city.bank_accounts.clear();
	ogs::City::BankAccount account;
	account.bank_id = a.id;
	account.firm = 100.0;
	city.bank_accounts.push_back(account);
	a.deposits_funding = 100.0;
	a.cash = 200.0;
	const double claims_before = ogs::bank::deposit_claims_total(world.ws);
	const double money_before = world.core.money_total();

	// Fora do fecho mensal: no-op.
	world.ws.day = 29;
	ogs::bank::allocate_deposits_monthly(world.ws);
	CHECK(city.claim_total() == Approx(100.0));
	CHECK(ogs::bank::deposit_claims_total(world.ws) == Approx(claims_before));

	world.ws.day = 30;
	ogs::bank::allocate_deposits_monthly(world.ws);
	// R2b1 (fix B1): alocacao NORMALIZADA pelos scores (nao "tudo no
	// melhor"): 0,30/(0,30+1,00) fica em A e 1,00/(0,30+1,00) vai para B.
	const double total_score = 0.30 + 1.00;
	const double keep_a = 100.0 * 0.30 / total_score;
	const double move_b = 100.0 * 1.00 / total_score;
	CHECK(city.claim_total() == Approx(100.0));
	CHECK(city.bank_accounts.size() == 2);
	const ogs::City::BankAccount* at_a = nullptr;
	const ogs::City::BankAccount* at_b = nullptr;
	for (const ogs::City::BankAccount& row : city.bank_accounts) {
		if (row.bank_id == a.id) {
			at_a = &row;
		}
		if (row.bank_id == b.id) {
			at_b = &row;
		}
	}
	REQUIRE(at_a != nullptr);
	REQUIRE(at_b != nullptr);
	CHECK(at_a->firm == Approx(keep_a));
	CHECK(at_b->firm == Approx(move_b));
	CHECK(a.deposits_funding == Approx(keep_a));
	CHECK(b.deposits_funding == Approx(move_b));
	CHECK(a.cash == Approx(200.0 - move_b));
	CHECK(b.cash == Approx(500.0 + move_b));
	CHECK(world.core.money_total() == Approx(money_before));
	CHECK(ogs::bank::deposit_claims_total(world.ws)
		== Approx(ogs::bank::totals(world.ws).deposits_funding));
}

// ---------------------------------------------------------------------------
// R2b1 (QA R2b1): A1 (claims no save + migracao por tomador), B4 (invariante
// por tomador/credor), B1 (alocacao pro rata e pendulo deterministico), A3
// (sinal de liquidez), A4 (renegociacao com claim zerado) e B2 (lend_city).
// ---------------------------------------------------------------------------

namespace {

ogs::Group* group_by_id(ogs::WorldState& ws, const std::string& id) {
	for (ogs::Group& group : ws.groups) {
		if (group.id == id) {
			return &group;
		}
	}
	return nullptr;
}

ogs::City* city_by_id(ogs::WorldState& ws, const std::string& id) {
	for (ogs::City& city : ws.cities) {
		if (city.id == id) {
			return &city;
		}
	}
	return nullptr;
}

const ogs::BankLoan* city_claim(const ogs::City& city,
		const std::string& bank_id) {
	for (const ogs::BankLoan& claim : city.firm_debt_claims) {
		if (claim.bank_id == bank_id) {
			return &claim;
		}
	}
	return nullptr;
}

// Soma principal/atraso dos claims de um tomador.
void claim_sums(const std::vector<ogs::BankLoan>& claims, double* principal,
		double* arrears) {
	*principal = 0.0;
	*arrears = 0.0;
	for (const ogs::BankLoan& claim : claims) {
		*principal += std::max(0.0, claim.principal);
		*arrears += std::max(0.0, claim.arrears);
	}
}

// B4: todo tomador com divida tem claims que somam o agregado, e todo banco
// tem `loans`/`arrears` iguais aos claims dos seus tomadores.
void check_claim_invariant(const ogs::WorldState& ws) {
	for (const ogs::Group& group : ws.groups) {
		if (group.debt + group.credit_arrears <= 1e-9) {
			continue;
		}
		double principal = 0.0;
		double arrears = 0.0;
		claim_sums(group.debt_claims, &principal, &arrears);
		CHECK(principal == Approx(group.debt));
		CHECK(arrears == Approx(group.credit_arrears));
	}
	for (const ogs::City& city : ws.cities) {
		if (city.firm_debt + city.firm_credit_arrears <= 1e-9) {
			continue;
		}
		double principal = 0.0;
		double arrears = 0.0;
		claim_sums(city.firm_debt_claims, &principal, &arrears);
		CHECK(principal == Approx(city.firm_debt));
		CHECK(arrears == Approx(city.firm_credit_arrears));
	}
	for (const auto& row : ws.banks) {
		const ogs::Bank& bank = row.second;
		double principal = 0.0;
		double arrears = 0.0;
		for (const ogs::Group& group : ws.groups) {
			for (const ogs::BankLoan& claim : group.debt_claims) {
				if (claim.bank_id == bank.id) {
					principal += std::max(0.0, claim.principal);
					arrears += std::max(0.0, claim.arrears);
				}
			}
		}
		for (const ogs::City& city : ws.cities) {
			for (const ogs::BankLoan& claim : city.firm_debt_claims) {
				if (claim.bank_id == bank.id) {
					principal += std::max(0.0, claim.principal);
					arrears += std::max(0.0, claim.arrears);
				}
			}
		}
		CHECK(principal == Approx(std::max(0.0, bank.loans)));
		CHECK(arrears == Approx(std::max(0.0, bank.arrears)));
	}
}

} // namespace

TEST_CASE("R2b1 (A1): claims de divida sobrevivem ao round-trip do save") {
	ogs::test::PocketWorld source;
	enable_banks(source, true, true);
	source.ws.day = 42;
	ogs::Bank& pub = present(source, ogs::bank::kRolePublic);
	ogs::Bank& pri = present(source, ogs::bank::kRolePrivate);
	REQUIRE(!source.ws.groups.empty());
	ogs::Group& group = source.ws.groups.front();
	group.debt = 1000.0;
	group.credit_arrears = 30.0;
	group.debt_claims = {claim_to(pub.id, 600.0, 20.0),
		claim_to(pri.id, 400.0, 10.0)};
	sort_claims(group.debt_claims);
	// "AAA/private" < "AAA/public": [0] = privado, [1] = publico.
	group.debt_claims[0].rate_add = 0.02;
	group.debt_claims[0].default_days = 5;
	group.debt_claims[1].rate_add = 0.01;
	group.debt_claims[1].default_days = 3;
	pub.loans = 600.0;
	pub.arrears = 20.0;
	pri.loans = 900.0;
	pri.arrears = 35.0;
	ogs::City& city = source.ws.cities.front();
	city.firm_debt = 500.0;
	city.firm_credit_arrears = 25.0;
	city.firm_debt_claims = {claim_to(pri.id, 500.0, 25.0)};
	const std::string group_id = group.id;
	const std::string city_id = city.id;
	const std::string pub_id = pub.id;
	const std::string pri_id = pri.id;

	const ogs::save::Value saved = ogs::save::to_value(source.ws,
		source.core);
	ogs::test::PocketWorld target;
	ogs::save::from_value(target.ws, target.core, saved);

	ogs::Group* loaded_group = group_by_id(target.ws, group_id);
	REQUIRE(loaded_group != nullptr);
	REQUIRE(loaded_group->debt_claims.size() == 2);
	CHECK(loaded_group->ws_link == &target.ws);
	CHECK(loaded_group->debt_claims[0].bank_id == pri_id);
	CHECK(loaded_group->debt_claims[0].principal == Approx(400.0));
	CHECK(loaded_group->debt_claims[0].arrears == Approx(10.0));
	CHECK(loaded_group->debt_claims[0].rate_add == Approx(0.02));
	CHECK(loaded_group->debt_claims[0].default_days == 5);
	CHECK(loaded_group->debt_claims[1].bank_id == pub_id);
	CHECK(loaded_group->debt_claims[1].principal == Approx(600.0));
	CHECK(loaded_group->debt_claims[1].arrears == Approx(20.0));
	CHECK(loaded_group->debt_claims[1].rate_add == Approx(0.01));
	CHECK(loaded_group->debt_claims[1].default_days == 3);
	CHECK(loaded_group->debt == Approx(1000.0));
	CHECK(loaded_group->credit_arrears == Approx(30.0));

	ogs::City* loaded_city = city_by_id(target.ws, city_id);
	REQUIRE(loaded_city != nullptr);
	REQUIRE(loaded_city->firm_debt_claims.size() == 1);
	CHECK(loaded_city->firm_debt_claims[0].bank_id == pri_id);
	CHECK(loaded_city->firm_debt_claims[0].principal == Approx(500.0));
	CHECK(loaded_city->firm_debt_claims[0].arrears == Approx(25.0));
	CHECK(loaded_city->firm_debt == Approx(500.0));
	check_claim_invariant(target.ws);

	// C6 usa os claims carregados: so' o credor baixado muda, pro rata no
	// book dele (grupo 400+10 + cidade 500+25 = 935; 93,5 = 10%).
	const double written = ogs::bank_crisis::apply_loss(target.ws, pri_id,
		93.5);
	CHECK(written == Approx(93.5));
	CHECK(loaded_group->debt_claims[0].principal == Approx(360.0));
	CHECK(loaded_group->debt_claims[0].arrears == Approx(9.0));
	CHECK(loaded_group->debt_claims[1].principal == Approx(600.0));
	CHECK(loaded_group->debt_claims[1].arrears == Approx(20.0));
	CHECK(loaded_city->firm_debt_claims[0].principal == Approx(450.0));
	CHECK(loaded_city->firm_debt_claims[0].arrears == Approx(22.5));
	CHECK(loaded_group->debt == Approx(960.0));
	CHECK(loaded_group->credit_arrears == Approx(29.0));
	check_claim_invariant(target.ws);
}

TEST_CASE("R2b1 (A1): load sem claims atribui a divida legada ao credor") {
	ogs::test::PocketWorld source;
	enable_banks(source, true, true);
	ogs::Bank& pub = present(source, ogs::bank::kRolePublic);
	present(source, ogs::bank::kRolePrivate);
	// Grupo legado: divida agregada sem nenhum claim (save v44 anterior ao
	// fix). `state_owned` fixa o credor em `lender()` = banco publico.
	ogs::Group legacy;
	legacy.id = "AAA_LEGACY";
	legacy.name = legacy.id;
	legacy.origin_nation = "AAA";
	legacy.state_owned = true;
	legacy.debt = 1000.0;
	legacy.credit_arrears = 50.0;
	legacy.cash = 0.0;
	source.ws.groups.push_back(legacy);
	source.core.rebuild_indexes();
	ogs::City& city = source.ws.cities.front();
	city.firm_debt = 400.0;
	city.firm_credit_arrears = 20.0;
	city.firm_debt_claims.clear();
	pub.loans = 1400.0;
	pub.arrears = 70.0;
	const std::string city_id = city.id;
	const std::string pub_id = pub.id;

	const ogs::save::Value saved = ogs::save::to_value(source.ws,
		source.core);
	ogs::test::PocketWorld target;
	ogs::save::from_value(target.ws, target.core, saved);

	ogs::Group* loaded = group_by_id(target.ws, "AAA_LEGACY");
	REQUIRE(loaded != nullptr);
	REQUIRE(loaded->debt_claims.size() == 1);
	CHECK(loaded->debt_claims[0].bank_id == pub_id);
	CHECK(loaded->debt_claims[0].principal == Approx(1000.0));
	CHECK(loaded->debt_claims[0].arrears == Approx(50.0));
	ogs::City* loaded_city = city_by_id(target.ws, city_id);
	REQUIRE(loaded_city != nullptr);
	REQUIRE(loaded_city->firm_debt_claims.size() == 1);
	CHECK(loaded_city->firm_debt_claims[0].bank_id == pub_id);
	CHECK(loaded_city->firm_debt_claims[0].principal == Approx(400.0));
	CHECK(loaded_city->firm_debt_claims[0].arrears == Approx(20.0));
	check_claim_invariant(target.ws);

	// Idempotente: rodar a migracao de novo nao duplica claim nem dinheiro.
	const double money_before = target.core.money_total();
	ogs::bank::rebuild_claims_after_load(target.ws);
	CHECK(loaded->debt_claims.size() == 1);
	CHECK(loaded->debt_claims[0].principal == Approx(1000.0));
	CHECK(loaded_city->firm_debt_claims.size() == 1);
	CHECK(target.core.money_total() == Approx(money_before));

	// Servicing roda no caminho por claim (sem divida zumbi).
	target.ws.crisis.enabled = false;
	ogs::Bank& target_pub = target.ws.banks[pub_id];
	target_pub.deposits = 0.0;
	target_pub.cash = 0.0;
	ogs::bank::phase(target.ws);
	CHECK(loaded->credit_arrears > 50.0);
	CHECK(loaded->debt_claims[0].arrears
		== Approx(loaded->credit_arrears));
	check_claim_invariant(target.ws);
}

TEST_CASE("R2b1 (A1): mundo misto serve divida legada e por claim no mesmo dia") {
	ogs::test::PocketWorld world;
	enable_banks(world, true, true);
	world.ws.crisis.enabled = false;
	ogs::Bank& pub = present(world, ogs::bank::kRolePublic);
	REQUIRE(!world.ws.groups.empty());
	const std::string claim_group_id = world.ws.groups.front().id;
	ogs::Group legacy;
	legacy.id = "AAA_LEGACY";
	legacy.name = legacy.id;
	legacy.origin_nation = "AAA";
	legacy.state_owned = true;
	legacy.debt = 600.0;
	legacy.cash = 0.0;
	world.ws.groups.push_back(legacy);
	world.core.rebuild_indexes();

	ogs::Group& claim_group = *group_by_id(world.ws, claim_group_id);
	claim_group.cash = 0.0;
	claim_group.debt = 400.0;
	claim_group.credit_arrears = 0.0;
	claim_group.credit_rate_add = 0.0;
	claim_group.debt_claims = {claim_to(pub.id, 400.0)};
	pub.loans = 1000.0;
	pub.deposits = 0.0;
	pub.cash = 0.0;
	pub.arrears = 0.0;

	CHECK(ogs::bank::phase(world.ws) == 0);
	ogs::Group& after_legacy = *group_by_id(world.ws, "AAA_LEGACY");
	// A divida legada continua sendo servida pelo caminho antigo...
	CHECK(after_legacy.credit_arrears > 0.0);
	CHECK(after_legacy.debt_claims.empty());
	// ...e a divida com claim paga/capitaliza pelo credor do claim.
	CHECK(claim_group.debt_claims[0].arrears > 0.0);
	CHECK(claim_group.credit_arrears
		== Approx(claim_group.debt_claims[0].arrears));
	// Por tomador: o claim soma o agregado do grupo com claim.
	double principal = 0.0;
	double arrears = 0.0;
	claim_sums(claim_group.debt_claims, &principal, &arrears);
	CHECK(principal == Approx(claim_group.debt));
	CHECK(arrears == Approx(claim_group.credit_arrears));
	CHECK(after_legacy.debt == Approx(600.0));

	// C6 enxerga os dois: o book do credor e' claim + agregado legado.
	const double book = 400.0 + claim_group.debt_claims[0].arrears
		+ after_legacy.debt + after_legacy.credit_arrears;
	const double marked = ogs::bank_crisis::apply_loss(world.ws, pub.id,
		100.0);
	CHECK(marked == Approx(100.0));
	const double keep = 1.0 - 100.0 / book;
	CHECK(claim_group.debt_claims[0].principal == Approx(400.0 * keep));
	CHECK(after_legacy.debt == Approx(600.0 * keep));
	claim_sums(claim_group.debt_claims, &principal, &arrears);
	CHECK(principal == Approx(claim_group.debt));
	CHECK(arrears == Approx(claim_group.credit_arrears));
	// O ativo do credor cobre as duas rotas (claim + legado).
	CHECK(claim_group.debt_claims[0].principal + after_legacy.debt
		== Approx(pub.loans));
}

TEST_CASE("R2b1 (B4): S claims == agregado por tomador e book por credor") {
	ogs::test::PocketWorld world;
	enable_banks(world, true, true);
	world.ws.crisis.enabled = false;
	ogs::Bank& pub = present(world, ogs::bank::kRolePublic);
	ogs::Bank& pri = present(world, ogs::bank::kRolePrivate);
	REQUIRE(!world.ws.groups.empty());
	ogs::Group& first = world.ws.groups.front();
	first.cash = 0.0;
	first.debt = 500.0;
	first.credit_arrears = 10.0;
	first.debt_claims = {claim_to(pub.id, 300.0, 7.0),
		claim_to(pri.id, 200.0, 3.0)};
	sort_claims(first.debt_claims);
	ogs::Group second;
	second.id = "AAA_SECOND";
	second.name = second.id;
	second.origin_nation = "AAA";
	second.state_owned = true;
	second.debt = 400.0;
	second.cash = 0.0;
	second.debt_claims = {claim_to(pri.id, 400.0)};
	world.ws.groups.push_back(second);
	world.core.rebuild_indexes();
	ogs::City& city = world.ws.cities.front();
	city.firm_cash = 0.0;
	city.private_cash = 0.0;
	city.firm_debt = 250.0;
	city.firm_credit_arrears = 5.0;
	city.firm_debt_claims = {claim_to(pub.id, 250.0, 5.0)};
	pub.loans = 550.0;
	pub.arrears = 12.0;
	pub.deposits = 0.0;
	pub.cash = 0.0;
	pri.loans = 600.0;
	pri.arrears = 3.0;
	pri.deposits = 0.0;
	pri.cash = 0.0;

	check_claim_invariant(world.ws);
	ogs::bank::phase(world.ws);
	check_claim_invariant(world.ws);
	ogs::bank_crisis::apply_loss(world.ws, pub.id, 55.0);
	check_claim_invariant(world.ws);
}

TEST_CASE("R2b1 (B1): alocacao leva so' a cobertura pro rata do claim") {
	ogs::test::PocketWorld world;
	enable_banks(world, true, true);
	ogs::Bank& a = present(world, ogs::bank::kRolePublic);
	ogs::Bank& b = present(world, ogs::bank::kRolePrivate);
	// Score = solidez pura; piso de solidez zerado.
	world.ws.banks_rework.alloc_w_rel = 0.0;
	world.ws.banks_rework.alloc_w_sol = 1.0;
	world.ws.banks_rework.alloc_w_pend = 0.0;
	world.ws.banks_rework.alloc_min_capital_ratio = 0.0;
	a.loans = 1000.0;
	a.deposits = 100.0;
	a.cash = 50.0;       // equity 950 -> solidez 0,95
	b.loans = 1000.0;
	b.deposits = 950.0;
	b.cash = 0.0;        // equity 50 -> solidez 0,05

	ogs::City& city = world.ws.cities.front();
	city.bank_accounts.clear();
	ogs::City::BankAccount account;
	account.bank_id = a.id;
	account.firm = 100.0;
	city.bank_accounts.push_back(account);
	a.deposits_funding = 100.0;
	const double money_before = world.core.money_total();
	const double score_a = ogs::bank::equity(a) / a.loans;
	const double score_b = ogs::bank::equity(b) / b.loans;

	world.ws.day = 30;
	ogs::bank::allocate_deposits_monthly(world.ws);

	// Alvo normalizado: A fica com score_a/(score_a+score_b) e B com o resto.
	// O excedente de A so' leva a cobertura pro rata do caixa
	// (cash/D_f = 50/100 = 50%); o caminho antigo levaria todo o excedente.
	const double target_b = 100.0 * score_b / (score_a + score_b);
	const double excess = target_b;
	const double moved = std::min(excess, 50.0 * (excess / 100.0));
	CHECK(city.claim_total() == Approx(100.0));
	CHECK(a.deposits_funding == Approx(100.0 - moved));
	CHECK(b.deposits_funding == Approx(moved));
	CHECK(a.cash == Approx(50.0 - moved));
	CHECK(b.cash == Approx(moved));
	// 2,78 < 5,56: a cobertura pro rata corta o excedente pela metade.
	CHECK(moved < excess);
	CHECK(world.core.money_total() == Approx(money_before));
	CHECK(ogs::bank::deposit_claims_total(world.ws)
		== Approx(ogs::bank::totals(world.ws).deposits_funding));
}

TEST_CASE("R2b1 (B1): pendulo mensal e' deterministico e muda com o mes") {
	const auto setup = [](ogs::test::PocketWorld& world) {
		enable_banks(world, true, true);
		ogs::Bank& a = present(world, ogs::bank::kRolePublic);
		ogs::Bank& b = present(world, ogs::bank::kRolePrivate);
		// Score = pendulo puro; solidez igual (1,0) e relacionamento zerado.
		world.ws.banks_rework.alloc_w_rel = 0.0;
		world.ws.banks_rework.alloc_w_sol = 0.0;
		world.ws.banks_rework.alloc_w_pend = 1.0;
		world.ws.banks_rework.alloc_min_capital_ratio = 0.8;
		a.loans = 1000.0;
		a.deposits = 0.0;
		a.cash = 100.0;
		b.loans = 1000.0;
		b.deposits = 0.0;
		b.cash = 0.0;
		ogs::City& city = world.ws.cities.front();
		city.bank_accounts.clear();
		ogs::City::BankAccount account;
		account.bank_id = a.id;
		account.firm = 100.0;
		city.bank_accounts.push_back(account);
		a.deposits_funding = 100.0;
		world.ws.day = 30;
	};

	ogs::test::PocketWorld w1;
	ogs::test::PocketWorld w2;
	setup(w1);
	setup(w2);
	ogs::bank::allocate_deposits_monthly(w1.ws);
	ogs::bank::allocate_deposits_monthly(w2.ws);

	ogs::City& c1 = w1.ws.cities.front();
	ogs::City& c2 = w2.ws.cities.front();
	REQUIRE(c1.bank_accounts.size() == 2);
	REQUIRE(c2.bank_accounts.size() == 2);
	const auto share_of = [](const ogs::City& city, const std::string& id) {
		for (const ogs::City::BankAccount& row : city.bank_accounts) {
			if (row.bank_id == id) {
				return row.firm;
			}
		}
		return -1.0;
	};
	const std::string a_id = ogs::bank::bank_id("AAA",
		ogs::bank::kRolePublic);
	const double share_1 = share_of(c1, a_id);
	const double share_2 = share_of(c2, a_id);
	// Duas construcoes independentes dao o mesmo retrato (hash estavel).
	CHECK(share_1 == Approx(share_2));
	// O pendulo fica em 0,9..1,1: a fatia de A em 40%..62% com dois bancos.
	CHECK(share_1 > 40.0);
	CHECK(share_1 < 62.0);
	CHECK(c1.claim_total() == Approx(100.0));

	// Repetir no mesmo mes e' no-op (o alvo ja foi atingido).
	ogs::bank::allocate_deposits_monthly(w1.ws);
	CHECK(share_of(c1, a_id) == Approx(share_1));

	// Mes seguinte: o hash muda e a fatia muda (procurado em 6 meses para
	// nao depender de uma coincidencia do fixture).
	bool changed = false;
	double previous = share_1;
	for (int day = 60; day <= 210 && !changed; day += 30) {
		w1.ws.day = day;
		ogs::bank::allocate_deposits_monthly(w1.ws);
		const double share = share_of(c1, a_id);
		CHECK(c1.claim_total() == Approx(100.0));
		CHECK(share > 40.0);
		CHECK(share < 62.0);
		CHECK(ogs::bank::deposit_claims_total(w1.ws)
			== Approx(ogs::bank::totals(w1.ws).deposits_funding));
		changed = std::fabs(share - previous) > 1e-6;
		previous = share;
	}
	REQUIRE(changed);
	CHECK(w1.core.money_total() == Approx(w2.core.money_total()));
}

TEST_CASE("R2b1 (A3): saque abaixo do piso acende o sinal uma vez por dia") {
	SUBCASE("abaixo do piso conta um evento por dia e persiste no save") {
		ogs::test::PocketWorld world;
		enable_banks(world, true, true);
		ogs::bank::ensure(world.ws, "AAA",
			ogs::bank::kRolePublic).name_status = "no_data";
		ogs::Bank& bank = present(world, ogs::bank::kRolePrivate);
		REQUIRE(!world.ws.groups.empty());
		ogs::Group& group = world.ws.groups.front();
		group.cash = 0.0;
		group.bank_deposit = 50.0;
		group.deposit_bank_id = bank.id;
		bank.deposits = 550.0;
		bank.deposits_funding = 50.0;
		bank.cash = 60.0;   // piso = 10% * (550+50) = 60
		ogs::deposit::link(world.ws);
		world.ws.day = 0;
		CHECK(ogs::bank::liquidity_floor(world.ws, bank) == Approx(60.0));

		// O pagamento NAO e' bloqueado; o saque deixa o caixa abaixo do piso.
		CHECK(ogs::deposit::withdraw_group(world.ws, group, 10.0)
			== Approx(10.0));
		CHECK(bank.cash < ogs::bank::liquidity_floor(world.ws, bank));
		CHECK(bank.liquidity_floor_breaches == 1);
		CHECK(bank.last_liquidity_floor_breach_day == 0);
		// Segundo saque no mesmo dia: conta uma vez.
		CHECK(ogs::deposit::withdraw_group(world.ws, group, 10.0)
			== Approx(10.0));
		CHECK(bank.liquidity_floor_breaches == 1);
		// Dia seguinte: novo evento.
		world.ws.day = 1;
		CHECK(ogs::deposit::withdraw_group(world.ws, group, 10.0)
			== Approx(10.0));
		CHECK(bank.liquidity_floor_breaches == 2);
		CHECK(bank.last_liquidity_floor_breach_day == 1);

		// O sinal vai ao save v44 no ON.
		const std::string bank_id = bank.id;
		const ogs::save::Value saved = ogs::save::to_value(world.ws,
			world.core);
		ogs::test::PocketWorld target;
		ogs::save::from_value(target.ws, target.core, saved);
		const ogs::Bank& loaded = target.ws.banks[bank_id];
		CHECK(loaded.liquidity_floor_breaches == 2);
		CHECK(loaded.last_liquidity_floor_breach_day == 1);
	}

	SUBCASE("caixa acima do piso nao acende nada") {
		ogs::test::PocketWorld world;
		enable_banks(world, true, true);
		ogs::bank::ensure(world.ws, "AAA",
			ogs::bank::kRolePublic).name_status = "no_data";
		ogs::Bank& bank = present(world, ogs::bank::kRolePrivate);
		REQUIRE(!world.ws.groups.empty());
		ogs::Group& group = world.ws.groups.front();
		group.cash = 0.0;
		group.bank_deposit = 10.0;
		group.deposit_bank_id = bank.id;
		bank.deposits = 500.0;
		bank.deposits_funding = 10.0;
		bank.cash = 100.0;  // piso = 10% * 510 = 51
		ogs::deposit::link(world.ws);
		world.ws.day = 3;
		CHECK(ogs::deposit::withdraw_group(world.ws, group, 5.0)
			== Approx(5.0));
		CHECK(bank.cash >= ogs::bank::liquidity_floor(world.ws, bank));
		CHECK(bank.liquidity_floor_breaches == 0);
		CHECK(bank.last_liquidity_floor_breach_day == -1);
	}
}

TEST_CASE("R2b1 (A4): claim zerado nao bloqueia a renegociacao") {
	ogs::test::PocketWorld world;
	enable_banks(world, true, true);
	ogs::Bank& pub = present(world, ogs::bank::kRolePublic);
	present(world, ogs::bank::kRolePrivate);
	ogs::Group group;
	group.id = "AAA_RENEG";
	group.name = group.id;
	group.origin_nation = "AAA";
	group.state_owned = true;
	group.debt = 100000.0;
	group.cash = 0.0;
	group.credit_rate_add = world.ws.crisis.credit_rate_add;
	group.credit_arrears_days =
		std::max(1, world.ws.crisis.credit_renegotiation_days);
	// Dois claims, mas so' UM em aberto: o zerado (quitado) nao pode travar
	// a renegociacao (o gate antigo olhava `size()`).
	const std::string zero_id = "AAA/private";
	group.debt_claims = {claim_to(zero_id, 0.0),
		claim_to(pub.id, 100000.0)};
	sort_claims(group.debt_claims);
	world.ws.groups.push_back(group);
	world.core.rebuild_indexes();
	pub.loans = 100000.0;
	pub.deposits = 0.0;
	pub.cash = 0.0;
	pub.arrears = 0.0;

	const double rate = ogs::bank::loan_rate(world.ws, "AAA")
		+ world.ws.crisis.credit_rate_add;
	const double interest = 100000.0 * rate / ogs::bank::kDaysPerYear;
	const double fee = world.ws.crisis.credit_penalty * 100000.0;

	CHECK(ogs::bank::phase(world.ws) == 0);
	ogs::Group& after = *group_by_id(world.ws, "AAA_RENEG");
	CHECK(after.credit_renegotiations == 1);
	CHECK(after.credit_arrears == Approx(0.0));
	CHECK(after.debt == Approx(100000.0 + interest + fee));
	REQUIRE(after.debt_claims.size() == 2);
	CHECK(after.debt_claims[0].bank_id == zero_id);
	CHECK(after.debt_claims[0].principal == Approx(0.0));
	CHECK(after.debt_claims[1].bank_id == pub.id);
	CHECK(after.debt_claims[1].principal
		== Approx(100000.0 + interest + fee));
	check_claim_invariant(world.ws);
}

TEST_CASE("R2b1 (B2): lend_city so' mantem o credor com presenca e capacidade") {
	SUBCASE("credor existente sem capacidade perde para quem tem") {
		ogs::test::PocketWorld world;
		enable_banks(world, true, true);
		world.ws.bank_credit_enabled = true;
		ogs::Bank& pub = present(world, ogs::bank::kRolePublic);
		ogs::Bank& pri = present(world, ogs::bank::kRolePrivate);
		ogs::City& city = world.ws.cities.front();
		city.firm_debt = 500.0;
		city.firm_credit_arrears = 0.0;
		city.firm_debt_claims = {claim_to(pri.id, 500.0)};
		// Privado sem folga: equity 0 -> capacidade 0.
		pri.loans = 1000.0;
		pri.deposits = 1000.0;
		pri.cash = 0.0;
		// Publico com folga de capital e liquidez.
		pub.loans = 1000.0;
		pub.deposits = 1000.0;
		pub.cash = 200.0;

		double out = 0.0;
		std::string error;
		const bool ok = ogs::bank::lend_city(world.ws, city.id, 100.0,
			&out, &error);
		REQUIRE_MESSAGE(ok, error);
		CHECK(out == Approx(100.0));
		CHECK(city.firm_debt == Approx(600.0));
		const ogs::BankLoan* kept = city_claim(city, pri.id);
		const ogs::BankLoan* added = city_claim(city, pub.id);
		REQUIRE(kept != nullptr);
		REQUIRE(added != nullptr);
		CHECK(kept->principal == Approx(500.0));
		CHECK(added->principal == Approx(100.0));
	}

	SUBCASE("credor existente com capacidade continua o par tomador<->banco") {
		ogs::test::PocketWorld world;
		enable_banks(world, true, true);
		world.ws.bank_credit_enabled = true;
		ogs::Bank& pub = present(world, ogs::bank::kRolePublic);
		ogs::Bank& pri = present(world, ogs::bank::kRolePrivate);
		ogs::City& city = world.ws.cities.front();
		city.firm_debt = 500.0;
		city.firm_credit_arrears = 0.0;
		city.firm_debt_claims = {claim_to(pri.id, 500.0)};
		// Privado presente e com capacidade: mantem o par.
		pri.loans = 1000.0;
		pri.deposits = 500.0;
		pri.cash = 400.0;
		// Publico mais solido (taxa menor), mas nao deve roubar o par.
		pub.loans = 1000.0;
		pub.deposits = 1000.0;
		pub.cash = 1000.0;

		double out = 0.0;
		std::string error;
		const bool ok = ogs::bank::lend_city(world.ws, city.id, 100.0,
			&out, &error);
		REQUIRE_MESSAGE(ok, error);
		CHECK(out == Approx(100.0));
		REQUIRE(city.firm_debt_claims.size() == 1);
		CHECK(city.firm_debt_claims[0].bank_id == pri.id);
		CHECK(city.firm_debt_claims[0].principal == Approx(600.0));
		CHECK(city_claim(city, pub.id) == nullptr);
		CHECK(pub.loans == Approx(1000.0));
	}

	SUBCASE("credor absorvido nao e' mantido: compete de novo") {
		ogs::test::PocketWorld world;
		enable_banks(world, true, true);
		world.ws.bank_credit_enabled = true;
		ogs::Bank& pub = present(world, ogs::bank::kRolePublic);
		ogs::Bank& pri = present(world, ogs::bank::kRolePrivate);
		ogs::City& city = world.ws.cities.front();
		city.firm_debt = 500.0;
		city.firm_credit_arrears = 0.0;
		city.firm_debt_claims = {claim_to(pri.id, 500.0)};
		pri.absorbed_into = pub.id;
		pri.loans = 1000.0;
		pri.deposits = 500.0;
		pri.cash = 400.0;
		pub.loans = 1000.0;
		pub.deposits = 1000.0;
		pub.cash = 200.0;

		double out = 0.0;
		std::string error;
		const bool ok = ogs::bank::lend_city(world.ws, city.id, 100.0,
			&out, &error);
		REQUIRE_MESSAGE(ok, error);
		CHECK(out == Approx(100.0));
		const ogs::BankLoan* kept = city_claim(city, pri.id);
		const ogs::BankLoan* added = city_claim(city, pub.id);
		REQUIRE(kept != nullptr);
		REQUIRE(added != nullptr);
		CHECK(kept->principal == Approx(500.0));
		CHECK(added->principal == Approx(100.0));
	}
}
