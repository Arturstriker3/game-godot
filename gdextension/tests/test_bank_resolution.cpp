// R2c - RESOLUCAO BANCARIA, LEILAO, CHARTER E CASCATA.
// So C++ puro: nenhuma API do Godot. Os valores abaixo sao do fixture de
// bolso e existem para provar as identidades de conservacao, nao para
// calibrar o jogo.
//
// Casos (docs/economy/rework/R2_plano.md 5.4/9):
//   * leilao de carteira: comprador paga caixa, book e claims migram juntos;
//   * bail-in pro rata D_f/D_l conserva e o claim acompanha o passivo;
//   * charter publico com teto: Tesouro paga, austeridade corta limites;
//   * acima do teto: deserto explicito (sem banco presente) e reentrada;
//   * cascata do publico: Tesouro recapitaliza com austeridade;
//   * sem Tesouro: bail-in/fundo e valvula <= 1 % M0/dia (nao `rescue_debt`).
#include "doctest/doctest.h"
#include "pocket_world.h"

#include "sim/bank.h"
#include "sim/bank_crisis.h"
#include "sim/central_bank.h"
#include "sim/state_levers.h"
#include "sim/treasury.h"

#include <algorithm>
#include <string>
#include <vector>

using doctest::Approx;

namespace {

constexpr double kEps = 1e-9;

void enable_banks(ogs::test::PocketWorld& world, bool resolution) {
	world.ws.regional_cash = true;
	ogs::BanksRework cfg;
	cfg.enabled = true;
	cfg.capital = true;
	cfg.deposits = true;
	cfg.resolution = resolution;
	world.core.set_banks_rework(cfg);
}

void enable_treasury(ogs::test::PocketWorld& world) {
	world.core.set_national_treasury(true, 1.0, 0.5, 0.35, 0.5);
	world.core.set_firms_own_cash(true);
}

ogs::Bank& present(ogs::test::PocketWorld& world, int role) {
	ogs::Bank& bank = ogs::bank::ensure(world.ws, "AAA", role);
	bank.name = "AAA bank";
	bank.name_status = "present";
	return bank;
}

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

ogs::BankLoan claim_to(const std::string& bank_id, double principal) {
	ogs::BankLoan claim;
	claim.bank_id = bank_id;
	claim.principal = principal;
	return claim;
}

// Um tomador com claim no banco que quebra e caixa para o bail-in de D_l.
ogs::Group& borrower(ogs::test::PocketWorld& world,
		const std::string& bank_id, double debt, double cash) {
	ogs::Group& group = world.ws.groups.front();
	group.state_owned = false;
	group.debt = debt;
	group.credit_arrears = 0.0;
	group.cash = cash;
	group.debt_claims.clear();
	group.debt_claims.push_back(claim_to(bank_id, debt));
	return group;
}

void set_charter_cap(ogs::test::PocketWorld& world, double gdp,
		double monthly_revenue) {
	ogs::BankSeedRow row;
	row.model_gdp_annual = gdp;
	world.ws.bank_seed.nations["AAA"] = row;
	for (std::size_t index = 0; index < world.ws.cities.size(); ++index) {
		if (index < world.ws.city_gov.size()) {
			world.ws.city_gov[index].target_revenue_month =
				monthly_revenue;
		}
	}
}

} // namespace

TEST_CASE("R2c: leilao paga caixa e leva carteira + claim de divida juntos") {
	ogs::test::PocketWorld world;
	enable_banks(world, true);
	ogs::Bank& failing = present(world, ogs::bank::kRolePrivate);
	ogs::Bank& buyer = present(world, ogs::bank::kRoleForeign);
	failing.cash = 0.0;
	failing.loans = 1000.0;
	failing.arrears = 0.0;
	failing.deposits = 1000.0;
	failing.bc_borrowing = 100.0;
	failing.capital = 0.0;
	buyer.cash = 700.0;
	buyer.loans = 0.0;
	buyer.arrears = 0.0;
	ogs::Group& group = borrower(world, failing.id, 1000.0, 400.0);
	const double money_before = world.core.money_total();

	const double gap = ogs::bank_crisis::resolve(world.ws, failing.id);
	CHECK(gap == Approx(100.0));
	// 1000 de book com desagio de 30 % = 700 pagos em caixa.
	CHECK(buyer.cash == Approx(0.0));
	CHECK(buyer.loans == Approx(1000.0));
	CHECK(failing.loans == Approx(0.0));
	CHECK(failing.cash == Approx(700.0));
	// O claim de divida acompanha a carteira (nada orfao).
	REQUIRE(group.debt_claims.size() == 1);
	CHECK(group.debt_claims.front().bank_id == buyer.id);
	CHECK(group.debt_claims.front().principal == Approx(1000.0));
	CHECK(claims_to(world.ws, failing.id) == Approx(0.0));
	// Bail-in de D_l: 0,4 x 1000 = 400, limitado ao caixa do tomador.
	CHECK(group.cash == Approx(0.0));
	CHECK(failing.deposits == Approx(600.0));
	CHECK(failing.bailin_cut == Approx(400.0));
	// O banco sobrevive recapitalizado: caixa 700 cobre 600 + 100 de BC.
	CHECK(failing.resolved);
	CHECK(failing.resolution == "auction+bailin");
	CHECK(world.core.money_total()
		== Approx(money_before - 400.0).epsilon(1e-9));
}

TEST_CASE("R2c: bail-in pro rata D_f/D_l e deserto preservam o passivo") {
	ogs::test::PocketWorld world(false);
	enable_banks(world, true);
	ogs::Bank& failing = present(world, ogs::bank::kRolePrivate);
	failing.cash = 0.0;
	failing.loans = 0.0;
	failing.arrears = 0.0;
	failing.deposits = 1000.0;          // D_l
	failing.deposits_funding = 1000.0;  // D_f
	failing.capital = 0.0;
	ogs::City& city = world.ws.cities.front();
	city.bank_accounts.clear();
	ogs::City::BankAccount account;
	account.bank_id = failing.id;
	account.firm = 1000.0;
	city.bank_accounts.push_back(account);
	ogs::Group& group = borrower(world, failing.id, 1000.0, 400.0);
	// Sem teto de charter: o residual vira deserto.
	set_charter_cap(world, 0.0, 0.0);
	const double money_before = world.core.money_total();

	ogs::bank_crisis::resolve(world.ws, failing.id);
	// Alvo nao segurado = 0,4 x 2000 = 800, metade em cada tipo.
	CHECK(failing.bailin_cut == Approx(800.0));
	CHECK(group.cash == Approx(0.0));
	CHECK(failing.deposits == Approx(600.0));
	CHECK(failing.deposits_funding == Approx(0.0));
	CHECK(claims_to(world.ws, failing.id) == Approx(0.0));
	// Deserto: o papel nao esta mais presente, mas o passivo nao pago fica
	// declarado na casca (zerar D_l sem cobrar caixa quebraria o invariante).
	CHECK(failing.name_status == "none");
	CHECK(failing.charter == "none");
	CHECK(failing.resolution.find("desert") != std::string::npos);
	CHECK(world.core.money_total()
		== Approx(money_before - 400.0).epsilon(1e-9));
}

TEST_CASE("R2c: charter publico com teto paga o Tesouro e corta orcamentos") {
	ogs::test::PocketWorld world;
	enable_banks(world, true);
	enable_treasury(world);
	ogs::Bank& failing = present(world, ogs::bank::kRolePrivate);
	failing.cash = 0.0;
	failing.loans = 0.0;
	failing.arrears = 0.0;
	failing.deposits = 1000.0;
	failing.deposits_funding = 0.0;
	failing.capital = 0.0;
	ogs::Group& group = borrower(world, failing.id, 1000.0, 400.0);
	// Teto folgado: 0,5 % de um PIB grande.
	set_charter_cap(world, 1000000000.0, 0.0);
	world.ws.nation_treasury["AAA"] = 1000.0;
	double budget_before = 0.0;
	for (std::size_t index = 0; index < world.ws.cities.size(); ++index) {
		world.ws.cities[index].regional_budget_limit_month = 1000.0;
		budget_before += 1000.0;
	}
	const double money_before = world.core.money_total();

	ogs::bank_crisis::resolve(world.ws, failing.id);
	// Bail-in 400 -> sobram 600; o Tesouro paga 600 dentro do teto.
	CHECK(group.cash == Approx(0.0));
	CHECK(world.ws.nation_treasury["AAA"] == Approx(400.0).epsilon(1e-9));
	CHECK(failing.absorbed_into
		== ogs::bank::bank_id("AAA", ogs::bank::kRolePublic));
	CHECK(failing.name_status == "none");
	const ogs::Bank* pub = ogs::bank::find(world.ws, "AAA",
		ogs::bank::kRolePublic);
	REQUIRE(pub != nullptr);
	CHECK(pub->name_status == "present");
	CHECK(pub->charter == "chartered_in_crisis");
	CHECK(pub->deposits == Approx(600.0));
	CHECK(pub->cash == Approx(600.0));
	CHECK(pub->capital == Approx(600.0));
	// Austeridade: os limites regionais caem pro rata no valor pago.
	double budget_after = 0.0;
	for (const ogs::City& city : world.ws.cities) {
		budget_after += city.regional_budget_limit_month;
	}
	CHECK(budget_after == Approx(budget_before - 600.0).epsilon(1e-9));
	CHECK(world.core.money_total()
		== Approx(money_before - 400.0).epsilon(1e-9));
}

TEST_CASE("R2c: custo acima do teto = deserto; charter reabre o banco") {
	ogs::test::PocketWorld world(false);
	enable_banks(world, true);
	enable_treasury(world);
	ogs::Bank& failing = present(world, ogs::bank::kRolePrivate);
	failing.cash = 0.0;
	failing.loans = 0.0;
	failing.arrears = 0.0;
	failing.deposits = 1000.0;
	failing.deposits_funding = 0.0;
	failing.capital = 0.0;
	borrower(world, failing.id, 1000.0, 400.0);
	// Teto minusculo: 0,5 % de 1000 = 5, muito abaixo dos 600 necessarios.
	set_charter_cap(world, 1000.0, 0.0);
	world.ws.nation_treasury["AAA"] = 10000.0;

	ogs::bank_crisis::resolve(world.ws, failing.id);
	CHECK(failing.name_status == "none");
	CHECK(failing.resolution.find("desert") != std::string::npos);
	CHECK(world.ws.nation_treasury["AAA"] == Approx(10000.0));
	// Nenhum banco presente na nacao = deserto explicito (derivado).
	bool any_present = false;
	for (const auto& row : world.ws.banks) {
		if (row.second.nation == "AAA"
				&& row.second.name_status == "present"
				&& row.second.absorbed_into.empty()) {
			any_present = true;
		}
	}
	CHECK_FALSE(any_present);

	// Reentrada pela alavanca de charter (A3/state_levers): cria o publico
	// pagando do Tesouro e o papel volta a estar presente.
	const ogs::TreasuryBankResult opened =
		ogs::action_charter_public_bank_from_treasury(world.ws, "AAA", 100.0);
	CHECK(opened.created);
	CHECK(opened.error.empty());
	CHECK(world.ws.nation_treasury["AAA"] == Approx(9900.0).epsilon(1e-9));
	const ogs::Bank* pub = ogs::bank::find(world.ws, "AAA",
		ogs::bank::kRolePublic);
	REQUIRE(pub != nullptr);
	CHECK(pub->name_status == "present");
	CHECK(pub->charter == "chartered_by_lever");
	CHECK(pub->cash == Approx(100.0).epsilon(1e-9));
	// Sem saldo a alavanca falha limpa: nada e movido.
	world.ws.nation_treasury["AAA"] = 0.0;
	const ogs::TreasuryBankResult refused =
		ogs::action_charter_public_bank_from_treasury(world.ws, "AAA", 50.0);
	CHECK_FALSE(refused.created);
	CHECK(refused.error == "saldo insuficiente do Tesouro");
	CHECK(world.ws.nation_treasury["AAA"] == Approx(0.0));
}

TEST_CASE("R2c: cascata do publico recapitaliza pelo Tesouro com austeridade") {
	ogs::test::PocketWorld world;
	enable_banks(world, true);
	enable_treasury(world);
	ogs::Bank& failing = present(world, ogs::bank::kRolePublic);
	failing.cash = 0.0;
	failing.loans = 0.0;
	failing.arrears = 0.0;
	failing.deposits = 1000.0;
	failing.capital = 0.0;
	world.ws.nation_treasury["AAA"] = 1000.0;
	double budget_before = 0.0;
	for (std::size_t index = 0; index < world.ws.cities.size(); ++index) {
		world.ws.cities[index].regional_budget_limit_month = 1000.0;
		budget_before += 1000.0;
	}
	const double money_before = world.core.money_total();

	const double gap = ogs::bank_crisis::resolve(world.ws, failing.id);
	CHECK(gap == Approx(1000.0));
	CHECK(world.ws.nation_treasury["AAA"] == Approx(0.0).epsilon(1e-9));
	CHECK(failing.cash == Approx(1000.0));
	CHECK(failing.capital == Approx(1000.0));
	CHECK(failing.deposits == Approx(1000.0));
	CHECK(failing.treasury_rescue == Approx(1000.0));
	CHECK(failing.resolution == "treasury");
	// Nao houve bail-in: o Tesouro cobriu o rombo antes do deposito.
	CHECK(failing.bailin_cut == Approx(0.0));
	// Austeridade: corta pro rata o valor pago (1000 dos 2000 de limite).
	double budget_after = 0.0;
	for (const ogs::City& city : world.ws.cities) {
		budget_after += city.regional_budget_limit_month;
	}
	CHECK(budget_after == Approx(budget_before - gap).epsilon(1e-9));
	CHECK(world.core.money_total() == Approx(money_before));
}

TEST_CASE("R2c: sem Tesouro a valvula emite <= 1 % do M0/dia") {
	ogs::test::PocketWorld world;
	enable_banks(world, true);
	enable_treasury(world);
	ogs::Bank& failing = present(world, ogs::bank::kRolePublic);
	failing.cash = 0.0;
	failing.loans = 0.0;
	failing.arrears = 0.0;
	failing.deposits = 1000.0;
	failing.capital = 0.0;
	borrower(world, failing.id, 1000.0, 400.0);
	// Tesouro vazio: a cascata cai no bail-in/fundo/valvula.
	world.ws.nation_treasury["AAA"] = 0.0;
	// Fundo cobre 590; sobram 10 depois do bail-in de 400.
	world.ws.deposit_insurance["AAA"].fund = 590.0;
	// Moeda controlada com M0 real de 5.000 -> teto de 50 no dia.
	ogs::Currency currency;
	currency.code = "AAA";
	currency.nations = {"AAA"};
	currency.bank_independence = ogs::central_bank::kIndependenceControlled;
	world.ws.currencies.push_back(currency);
	world.ws.currency_by_code["AAA"] = 0;
	// O indice nacao->moeda e' derivado no rebuild_indexes; o teste insere a
	// moeda DEPOIS do PocketWorld, entao alimenta o mapa como o load faz.
	world.ws.nation_currency["AAA"] = 0;
	world.ws.monetary["AAA"] = ogs::MonetaryState();
	ogs::City& city = world.ws.cities.front();
	city.firm_sub = 5000.0;
	city.firm_cash = 5000.0;
	city.private_cash = 5000.0;
	const double m0 = world.core.m0_total();
	REQUIRE(m0 > 0.0);

	ogs::bank_crisis::resolve(world.ws, failing.id, "equity_negative", m0);
	// Bail-in 400 (D_l) + fundo 590 + valvula 10.
	CHECK(failing.bailin_cut == Approx(400.0));
	CHECK(failing.insured_payout == Approx(590.0));
	CHECK(world.ws.deposit_insurance["AAA"].fund == Approx(0.0));
	const ogs::MonetaryState& state = world.ws.monetary.at("AAA");
	CHECK(state.emission == Approx(10.0));
	CHECK(state.emission <= 0.01 * m0 + 1e-9);
	CHECK(world.ws.gov_bonds["AAA"] == Approx(10.0));
	CHECK(failing.cash == Approx(600.0));
	CHECK(failing.deposits == Approx(600.0));
	CHECK(failing.resolution == "bailin+insurance+valve");
}
