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
#include "sim/save_state.h"
#include "sim/sim_core.h"

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
