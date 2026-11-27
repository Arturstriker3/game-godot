// R1c - EMPRESAS OPERAM 100% DO CAIXA PROPRIO (flag
// `rework.firms_own_cash`).
//
// Garantias, com o fixture `pocket_world`:
//   a) configuracao invalida (firms ON sem `regional_cash`) e tratada como
//      desligada e deixa o motivo em `firms_config_error`;
//   b) com a flag ligada o subsídio setorial ad valorem nao paga nada ao
//      grupo (o aporte publico automatico morre) e o dinheiro nao se move;
//   c) com a flag ligada a cobertura do prejuizo da planta estrategica
//      tambem morre (nenhum credito no caixa privado vindo do orcamento);
//   d) construcao privada nunca debita orcamento publico;
//   e) save/load: a flag ON preserva a chave e reaplica; OFF nao ganha chave.
#include "doctest/doctest.h"
#include "pocket_world.h"

#include "sim/actions.h"
#include "sim/save_state.h"
#include "sim/sim_core.h"
#include "sim/state_levers.h"
#include "sim/treasury.h"

#include <cstddef>
#include <string>

using doctest::Approx;

namespace {

void seed_cash(ogs::test::PocketWorld& world, double firm, double service,
		double government) {
	for (ogs::City& city : world.ws.cities) {
		city.firm_cash = firm;
		city.service_cash = service;
		city.government_budget = government;
	}
}

ogs::City& city_by_id(ogs::test::PocketWorld& world, const char* id) {
	for (ogs::City& city : world.ws.cities) {
		if (city.id == id) {
			return city;
		}
	}
	return world.ws.cities[0];
}

int group_index(ogs::test::PocketWorld& world, const std::string& id) {
	for (std::size_t index = 0; index < world.ws.groups.size(); ++index) {
		if (world.ws.groups[index].id == id) {
			return static_cast<int>(index);
		}
	}
	return -1;
}

// Liga o pool regional e, opcionalmente, a flag das empresas.
void enable_flags(ogs::test::PocketWorld& world, bool firms_own_cash) {
	world.core.set_regional_cash(true);
	world.core.set_firms_own_cash(firms_own_cash);
}

// R1c (QA t177): liga as empresas-do-caixa-proprio E o Tesouro Nacional, com
// saldo inicial para as alavancas A3 e um orcamento municipal visivel (limite
// do mes, nao caixa) que os testes provam NAO ser debitado.
void enable_treasury_levers(ogs::test::PocketWorld& world, double treasury) {
	world.core.set_regional_cash(true);
	world.core.set_national_treasury(true, 1.0, 0.5, 0.35, 0.5);
	world.core.set_firms_own_cash(true);
	world.ws.nation_treasury["AAA"] = treasury;
	for (ogs::City& city : world.ws.cities) {
		city.government_budget = 7000.0;
	}
}

// Incentivo setorial do fixture: 20% da receita da madeira.
void add_wood_subsidy(ogs::test::PocketWorld& world, double rate) {
	ogs::SectorIncentive incentive;
	incentive.nation = "AAA";
	incentive.sector = "wood";
	incentive.subsidy = rate;
	world.ws.incentives.push_back(incentive);
	world.ws.incentive_lookup["AAA|wood"] =
		static_cast<int>(world.ws.incentives.size() - 1);
}

} // namespace

TEST_CASE("R1c: configuracao invalida (firms sem caixa regional) desliga") {
	ogs::test::PocketWorld world;
	world.core.set_regional_cash(false);
	world.core.set_firms_own_cash(true);
	CHECK_FALSE(world.ws.firms_own_cash);
	CHECK_FALSE(world.ws.firms_config_error.empty());

	// Com o caixa regional ligado a mesma chamada vale.
	ogs::test::PocketWorld valid;
	valid.core.set_regional_cash(true);
	valid.core.set_firms_own_cash(true);
	CHECK(valid.ws.firms_own_cash);
	CHECK(valid.ws.firms_config_error.empty());

	// Desligar o caixa regional desliga as empresas na mesma chamada.
	valid.core.set_regional_cash(false);
	CHECK_FALSE(valid.ws.firms_own_cash);
}

TEST_CASE("R1c: subsidio setorial publico morre sob a flag") {
	// Dois mundos identicos: OFF (aporte pago) e ON (aporte morto). A diferenca
	// do caixa do grupo e do orcamento publico e exatamente o subsidio.
	ogs::test::PocketWorld off;
	ogs::test::PocketWorld on;
	for (ogs::test::PocketWorld* world : {&off, &on}) {
		seed_cash(*world, 1.0e8, 50000.0, 10000.0);
		add_wood_subsidy(*world, 0.20);
		world->ws.group_lookup["AAA_WOOD"] =
			group_index(*world, "AAA_WOOD");
		// Receita do dia do moinho do grupo (capacidade unica do produto).
		ogs::City& portus = city_by_id(*world, "PORTUS");
		portus.revenue_by_product[world->product("planks")] = 1000.0;
	}
	off.core.set_regional_cash(true);
	on.core.set_regional_cash(true);
	on.core.set_firms_own_cash(true);
	CHECK_FALSE(off.ws.firms_own_cash);
	CHECK(on.ws.firms_own_cash);

	const int group = group_index(off, "AAA_WOOD");
	const double off_group_before = off.ws.groups[group].cash;
	const double off_gov_before =
		city_by_id(off, "PORTUS").government_budget;
	off.core.phase_accounting();
	const double off_group_delta = off.ws.groups[group].cash - off_group_before;
	const double off_gov_delta =
		city_by_id(off, "PORTUS").government_budget - off_gov_before;

	const double on_group_before = on.ws.groups[group].cash;
	const double on_gov_before = city_by_id(on, "PORTUS").government_budget;
	on.core.phase_accounting();
	const double on_group_delta = on.ws.groups[group].cash - on_group_before;
	const double on_gov_delta =
		city_by_id(on, "PORTUS").government_budget - on_gov_before;

	// O subsidio (20% de 1000 = 200) so aparece no braco OFF.
	CHECK(off_group_delta - on_group_delta == Approx(200.0));
	CHECK(off_gov_delta - on_gov_delta == Approx(-200.0));
	// Sem aporte, o grupo recebeu so o lucro liquido (nao zero por acaso).
	CHECK(on_group_delta > 0.0);
}

TEST_CASE("R1c: socorro automatico da planta estrategica morre sob a flag") {
	// Planta estrategica com prejuizo de 100: OFF cobre do orcamento publico;
	// ON deixa a perda com a firma (nenhum credito no caixa privado).
	ogs::test::PocketWorld off;
	ogs::test::PocketWorld on;
	for (ogs::test::PocketWorld* world : {&off, &on}) {
		seed_cash(*world, 1000.0, 1000.0, 10000.0);
		ogs::Building* farm = world->building("AGRA_FARM");
		REQUIRE(farm != nullptr);
		farm->owner = "state";
		farm->strategic = true;
		farm->paid_wages = 100.0;
	}
	off.core.set_regional_cash(true);
	on.core.set_regional_cash(true);
	on.core.set_firms_own_cash(true);

	const double off_firm_before = city_by_id(off, "AGRA").firm_liquidity();
	const double on_firm_before = city_by_id(on, "AGRA").firm_liquidity();
	off.core.phase_accounting();
	on.core.phase_accounting();
	const double off_gain = city_by_id(off, "AGRA").firm_liquidity()
		- off_firm_before;
	const double on_gain = city_by_id(on, "AGRA").firm_liquidity()
		- on_firm_before;

	CHECK(off_gain == Approx(100.0));
	CHECK(on_gain == Approx(0.0));
}

TEST_CASE("R1c: construcao privada nao debita orcamento publico") {
	ogs::test::PocketWorld world;
	seed_cash(world, 50000.0, 10000.0, 7777.0);
	enable_flags(world, true);
	ogs::City& portus = city_by_id(world, "PORTUS");
	const double government_before = portus.government_budget;
	const double firm_before = portus.firm_liquidity();
	const double money_before = world.core.money_total();
	const int planks = world.product("planks");
	const int recipe = world.recipe("planks");
	REQUIRE(recipe >= 0);

	const ogs::BuildResult result = ogs::action_build(world.ws, world.core,
		"PORTUS", "R1C-PRIVATE-PLANT", "timber", -1, planks, 1.0, 0.0,
		0.0, 0.0, 0.0, "private", 1000.0, "planks", true, false);
	REQUIRE(result.created);
	CHECK(result.cost == Approx(1000.0));
	// O caixa publico nao foi tocado: a planta saiu do caixa privado.
	CHECK(portus.government_budget == Approx(government_before));
	CHECK(portus.firm_liquidity() == Approx(firm_before - 1000.0));
	CHECK(world.core.money_total() == Approx(money_before));
}

TEST_CASE("R1c: save/load preserva a flag ON e OFF nao ganha chave") {
	ogs::test::PocketWorld on;
	seed_cash(on, 50000.0, 30000.0, 1000.0);
	enable_flags(on, true);
	const ogs::save::Value saved = ogs::save::to_value(on.ws, on.core);
	CHECK(saved.has("firms_own_cash"));
	const std::string before = ogs::save::dump_json(saved);
	ogs::test::PocketWorld target;
	ogs::save::from_value(target.ws, target.core, saved);
	CHECK(target.ws.firms_own_cash);
	CHECK(ogs::save::dump_json(
		ogs::save::to_value(target.ws, target.core)) == before);

	ogs::test::PocketWorld off;
	seed_cash(off, 50000.0, 30000.0, 1000.0);
	enable_flags(off, false);
	CHECK_FALSE(ogs::save::to_value(off.ws, off.core).has("firms_own_cash"));
}

TEST_CASE("R1c: fund_group sob ON debita o Tesouro, nao o orcamento municipal") {
	// Alavanca A3 explicita (QA t177): com a flag ON o orcamento municipal e
	// LIMITE do mes, entao o aporte tem de sair do Tesouro da nacao. Saldo
	// insuficiente falha limpo (sem debito parcial).
	ogs::test::PocketWorld world;
	seed_cash(world, 1000.0, 1000.0, 7000.0);
	enable_treasury_levers(world, 1000.0);
	REQUIRE(world.ws.firms_own_cash);
	REQUIRE(ogs::treasury::enabled(world.ws));
	const int group = group_index(world, "AAA_WOOD");
	REQUIRE(group >= 0);
	world.ws.group_lookup["AAA_WOOD"] = group;
	const double group_before = world.ws.groups[group].cash;
	const double budget_before =
		city_by_id(world, "PORTUS").government_budget;
	const double money_before = world.core.money_total();

	const ogs::TreasuryFundResult ok =
		ogs::action_fund_group_from_treasury(world.ws, "AAA_WOOD", 200.0);
	CHECK(ok.funded);
	CHECK(ok.moved == Approx(200.0));
	CHECK(ok.error.empty());
	CHECK(world.ws.groups[group].cash == Approx(group_before + 200.0));
	CHECK(ogs::treasury::balance(world.ws, "AAA") == Approx(800.0));
	CHECK(city_by_id(world, "PORTUS").government_budget
		== Approx(budget_before));
	// Transferencia Tesouro -> grupo: nenhum dinheiro criado/destruido.
	CHECK(world.core.money_total() == Approx(money_before));

	// Saldo insuficiente: o comando falha limpo e nada se move.
	world.ws.nation_treasury["AAA"] = 100.0;
	const double cash_now = world.ws.groups[group].cash;
	const double money_now = world.core.money_total();
	const ogs::TreasuryFundResult fail =
		ogs::action_fund_group_from_treasury(world.ws, "AAA_WOOD", 200.0);
	CHECK_FALSE(fail.funded);
	CHECK(fail.moved == Approx(0.0));
	CHECK_FALSE(fail.error.empty());
	CHECK(world.ws.groups[group].cash == Approx(cash_now));
	CHECK(ogs::treasury::balance(world.ws, "AAA") == Approx(100.0));
	CHECK(world.core.money_total() == Approx(money_now));
}

TEST_CASE("R1c: create_state_company sob ON debita o Tesouro e falha sem saldo") {
	ogs::test::PocketWorld world;
	seed_cash(world, 1000.0, 1000.0, 7000.0);
	enable_treasury_levers(world, 1000.0);
	REQUIRE(ogs::treasury::enabled(world.ws));
	const std::size_t groups_before = world.ws.groups.size();
	const double budget_before = city_by_id(world, "AGRA").government_budget;
	const double money_before = world.core.money_total();

	const ogs::TreasuryCompanyResult ok =
		ogs::action_create_state_company_from_treasury(world.ws, "AAA",
			"Estatal Teste", 300.0);
	CHECK(ok.created);
	CHECK(ok.capital == Approx(300.0));
	CHECK_FALSE(ok.id.empty());
	CHECK(ogs::treasury::balance(world.ws, "AAA") == Approx(700.0));
	CHECK(world.ws.groups.size() == groups_before + 1);
	const int created = group_index(world, ok.id);
	REQUIRE(created >= 0);
	CHECK(world.ws.groups[static_cast<std::size_t>(created)].cash
		== Approx(300.0));
	CHECK(world.ws.groups[static_cast<std::size_t>(created)].state_owned);
	CHECK(city_by_id(world, "AGRA").government_budget
		== Approx(budget_before));
	CHECK(world.core.money_total() == Approx(money_before));

	// Sem saldo: NENHUMA estatal e criada e o Tesouro fica intacto.
	world.ws.nation_treasury["AAA"] = 100.0;
	const std::size_t groups_now = world.ws.groups.size();
	const double money_now = world.core.money_total();
	const ogs::TreasuryCompanyResult fail =
		ogs::action_create_state_company_from_treasury(world.ws, "AAA",
			"Estatal Falha", 300.0);
	CHECK_FALSE(fail.created);
	CHECK_FALSE(fail.error.empty());
	CHECK(fail.capital == Approx(0.0));
	CHECK(world.ws.groups.size() == groups_now);
	CHECK(ogs::treasury::balance(world.ws, "AAA") == Approx(100.0));
	CHECK(world.core.money_total() == Approx(money_now));
}
