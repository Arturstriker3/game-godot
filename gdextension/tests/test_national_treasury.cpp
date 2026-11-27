// R1b - Tesouro Nacional + ORCAMENTO REGIONAL (flag
// `rework.national_treasury`).
//
// Garantias, com o fixture `pocket_world` e o MESMO caminho do save real:
//   a) configuracao invalida (Tesouro ON sem `regional_cash`) e tratada como
//      desligada e deixa o motivo em `treasury_config_error`;
//   b) o dinheiro NAO e transferido para a regiao: `start_month` grava apenas
//      limite/contadores e nao move o saldo do Tesouro;
//   c) o gasto publico debita o Tesouro na reconciliacao, ate o limite; o
//      contador nunca fica negativo e a soma gasta <= arrecadado + saldo;
//   d) a dotacao e proporcional a populacao com piso minimo por regiao;
//   e) o saldo do Tesouro e atribuido a moeda da nacao (regressao t170);
//   f) o orcamento regional nao toca os sub-saldos privados do R1a;
//   g) save/load ON preserva flag, saldos e contadores; OFF nao ganha chave.
#include "doctest/doctest.h"
#include "pocket_world.h"

#include "sim/central_bank.h"
#include "sim/save_state.h"
#include "sim/sim_core.h"
#include "sim/treasury.h"

#include <algorithm>
#include <cstddef>
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

// Gasta do restante de servicos como os chamadores reais fazem: o gasto e
// limitado pelo contador (o excesso simplesmente nao acontece = adiado).
double spend_services(ogs::City& city, double demand) {
	const double paid = std::min(demand, std::max(0.0, city.government_budget));
	city.government_budget -= paid;
	return paid;
}

// Camada urbana completa: o save so grava o bloco `urban` (e o
// `city_gov.cash` de obras) quando land/housing/city_gov estao dimensionados.
void resize_urban(ogs::test::PocketWorld& world) {
	const std::size_t count = world.ws.cities.size();
	world.ws.land.resize(count);
	world.ws.housing.resize(count);
	world.ws.city_gov.resize(count);
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

	// Desligar nao destroi dinheiro: o Tesouro e os contadores virtuais viram
	// dinheiro real da primeira cidade da nacao (mundo OFF).
	ogs::treasury::collect(valid.ws, "AAA", 123.0);
	const double total_before = valid.core.money_total();
	valid.core.set_national_treasury(false);
	CHECK_FALSE(valid.ws.national_treasury);
	CHECK(ogs::treasury::total_balance(valid.ws) == Approx(0.0));
	CHECK(valid.core.money_total() == Approx(total_before));
}

TEST_CASE("R1b: arrecadar conserva e o dinheiro fica no Tesouro") {
	ogs::test::PocketWorld world;
	seed_cash(world, 50000.0, 30000.0);
	enable_treasury(world, 0.5);
	const double money_before = world.core.money_total();

	// O debito do pagador ja aconteceu; aqui so a contraparte no Tesouro.
	ogs::treasury::collect(world.ws, "AAA", 900.0);
	CHECK(world.core.money_total() == Approx(money_before + 900.0));
	CHECK(ogs::treasury::balance(world.ws, "AAA") == Approx(900.0));
	CHECK(world.ws.cities[0].government_budget == Approx(0.0));
	CHECK(world.ws.cities[1].government_budget == Approx(0.0));

	// Abrir o mes NAO transfere: grava dotacao/contadores e o saldo fica.
	const double limit = ogs::treasury::start_month(world.ws);
	CHECK(limit == Approx(900.0));
	CHECK(ogs::treasury::balance(world.ws, "AAA") == Approx(900.0));
	CHECK(world.core.money_total() == Approx(money_before + 900.0));
	CHECK(world.ws.cities[0].regional_budget_limit_month
		+ world.ws.cities[1].regional_budget_limit_month == Approx(900.0));
}

TEST_CASE("R1b: o saldo do Tesouro e atribuido a moeda da nacao") {
	// Regressao (t170): o saldo do Tesouro entrou no `money_total`, mas a
	// alocacao por moeda (invariante da C2a) nao o conhecia e o tick era
	// revertido todo dia (smoke ON travado em 10/10). O caixa arrecadado tem
	// de ser integralmente atribuido: residual zero, nenhuma violacao.
	ogs::test::PocketWorld world;
	seed_cash(world, 50000.0, 30000.0);
	enable_treasury(world, 0.5);
	ogs::treasury::collect(world.ws, "AAA", 900.0);
	const double money = world.core.money_total();
	const ogs::central_bank::MoneyAllocation before =
		ogs::central_bank::allocate_money(world.ws, money);
	CHECK(before.residual() == Approx(0.0));
	CHECK(before.allocated_total() == Approx(money));

	// Abrir o mes (sem transferir) mantem a atribuicao fechando.
	ogs::treasury::start_month(world.ws);
	const double after_limit = world.core.money_total();
	const ogs::central_bank::MoneyAllocation allocated =
		ogs::central_bank::allocate_money(world.ws, after_limit);
	CHECK(allocated.residual() == Approx(0.0));
	CHECK(allocated.allocated_total() == Approx(after_limit));

	// Gasto publico: o Tesouro cai, o contador cai junto e a atribuicao segue
	// fechando (sem o orcamento regional no total).
	ogs::City& city = world.ws.cities[0];
	city.regional_budget_open_services = city.government_budget;
	spend_services(city, 100.0);
	ogs::treasury::reconcile(world.ws);
	const double after_spend = world.core.money_total();
	const ogs::central_bank::MoneyAllocation spent =
		ogs::central_bank::allocate_money(world.ws, after_spend);
	CHECK(spent.residual() == Approx(0.0));
	CHECK(spent.allocated_total() == Approx(after_spend));
}

TEST_CASE("R1b: dotacao e proporcional a populacao com piso por regiao") {
	// Sem piso: rateio puro pela populacao (2/3 e 1/3 no fixture 1M/500k).
	{
		ogs::test::PocketWorld world;
		enable_treasury(world, 0.0);
		ogs::treasury::collect(world.ws, "AAA", 900.0);
		ogs::treasury::start_month(world.ws);
		CHECK(world.ws.cities[0].regional_budget_limit_month
			== Approx(600.0));
		CHECK(world.ws.cities[1].regional_budget_limit_month
			== Approx(300.0));
	}
	// Piso: uma regiao minuscula (1.000 de 1.001.000) recebe ao menos metade do
	// rateio igualitario (floor/n = 0,25 => ~18% da dotacao, nao 0,1%).
	{
		ogs::test::PocketWorld world;
		enable_treasury(world, 0.5);
		world.ws.cities[0].simulation_population = 1000000.0;
		world.ws.cities[1].simulation_population = 1000.0;
		ogs::treasury::collect(world.ws, "AAA", 900.0);
		ogs::treasury::start_month(world.ws);
		const double agra = world.ws.cities[0].regional_budget_limit_month;
		const double portus = world.ws.cities[1].regional_budget_limit_month;
		CHECK(agra + portus == Approx(900.0));
		CHECK(portus > 179.0);       // muito acima dos 0,9 do rateio puro
		CHECK(portus < 181.0);       // e abaixo do rateio igualitario (450)
		CHECK(portus == Approx(900.0 * 0.25 / 1.249000999).epsilon(1e-6));
	}
	// works_share: a fatia de obras vai para `CityGov::cash`; o resto para o
	// limite de servicos (a soma e a dotacao).
	{
		ogs::test::PocketWorld world;
		world.ws.city_gov.resize(world.ws.cities.size());
		enable_treasury(world, 0.0, 0.25);
		ogs::treasury::collect(world.ws, "AAA", 800.0);
		ogs::treasury::start_month(world.ws);
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

TEST_CASE("R1b: limite exato debita o Tesouro; estouro fica adiado") {
	ogs::test::PocketWorld world;
	world.ws.city_gov.resize(world.ws.cities.size());
	enable_treasury(world, 0.0);
	ogs::treasury::collect(world.ws, "AAA", 1000.0);
	ogs::treasury::start_month(world.ws);
	ogs::City& city = world.ws.cities[0];
	const double limit = city.regional_budget_limit_month;
	CHECK(limit > 0.0);
	city.regional_budget_open_services = city.government_budget;
	const double treasury_before = ogs::treasury::balance(world.ws, "AAA");

	// Demanda acima do limite: o chamador so consegue debitar o contador.
	const double paid = spend_services(city, limit * 3.0);
	CHECK(paid == Approx(limit));
	CHECK(city.government_budget == Approx(0.0));
	CHECK(city.government_budget >= -1e-9);
	ogs::treasury::reconcile(world.ws);
	CHECK(city.regional_budget_spent_month == Approx(limit));
	CHECK(ogs::treasury::balance(world.ws, "AAA")
		== Approx(treasury_before - limit));
	CHECK(world.ws.treasury_spent_total == Approx(limit));

	// A soma gasta nunca passa do arrecadado + saldo inicial.
	CHECK(world.ws.treasury_spent_total
		<= world.ws.treasury_collected_total + 1000.0 + 1e-6);
}

TEST_CASE("R1b: gastos <= arrecadacao + saldo ao longo dos meses") {
	ogs::test::PocketWorld world;
	world.ws.city_gov.resize(world.ws.cities.size());
	enable_treasury(world, 0.0);
	const double seed = 500.0;
	ogs::treasury::collect(world.ws, "AAA", seed);
	for (int month = 0; month < 6; ++month) {
		ogs::treasury::start_month(world.ws);
		for (ogs::City& city : world.ws.cities) {
			city.regional_budget_open_services = city.government_budget;
			spend_services(city, city.government_budget * 0.75);
		}
		ogs::treasury::reconcile(world.ws);
		CHECK(ogs::treasury::total_balance(world.ws) >= -1e-9);
		// Imposto do dia seguinte repoe o Tesouro.
		ogs::treasury::collect(world.ws, "AAA", 120.0);
	}
	CHECK(world.ws.treasury_spent_total
		== Approx(seed + 6.0 * 120.0
			- ogs::treasury::total_balance(world.ws)));
	CHECK(world.ws.treasury_spent_total
		<= world.ws.treasury_collected_total + 1e-6);
}

TEST_CASE("R1b: orcamento regional nao toca os sub-saldos privados (R1a)") {
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
	ogs::treasury::start_month(world.ws);

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
	// ON: flag, saldos por nacao, contadores e acumulados sobrevivem.
	{
		ogs::test::PocketWorld world;
		seed_cash(world, 50000.0, 30000.0);
		enable_treasury(world, 0.5);
		ogs::treasury::collect(world.ws, "AAA", 700.0);
		ogs::treasury::start_month(world.ws);
		ogs::City& city = world.ws.cities[0];
		city.regional_budget_open_services = city.government_budget;
		spend_services(city, 40.0);
		ogs::treasury::reconcile(world.ws);
		CHECK(ogs::treasury::balance(world.ws, "AAA") == Approx(660.0));

		const ogs::save::Value saved =
			ogs::save::to_value(world.ws, world.core);
		CHECK(saved.has("national_treasury"));
		CHECK(saved.has("treasury"));
		const std::string before = ogs::save::dump_json(saved);
		ogs::test::PocketWorld target;
		ogs::save::from_value(target.ws, target.core, saved);
		CHECK(target.ws.national_treasury);
		CHECK(ogs::treasury::balance(target.ws, "AAA") == Approx(660.0));
		CHECK(target.ws.treasury_collected_total == Approx(700.0));
		CHECK(target.ws.treasury_spent_total == Approx(40.0));
		CHECK(target.ws.cities[0].regional_budget_spent_month == Approx(40.0));
		CHECK(target.ws.cities[0].regional_budget_limit_month
			== Approx(world.ws.cities[0].regional_budget_limit_month));
		const std::string after = ogs::save::dump_json(
			ogs::save::to_value(target.ws, target.core));
		CHECK(after == before);
	}
	// Flag do save ON aplicada sem o caixa regional: config invalida =>
	// desligado e o total publico vira dinheiro real (nao se destroi nada).
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
		CHECK(target.core.money_total() == Approx(money_before + 25.0));
	}
}

TEST_CASE("R1b: save/load ON preserva a dotacao de obras (QA t173)") {
	// QA t173: `apply_national_treasury_after_load` rodava ANTES de
	// `read_urban`; o resto de `CityGov::cash` chegava 0 no apply, entao o 1o
	// reconcile (prologue) tratava o caixa de obras como RECEITA do Tesouro
	// (criava dinheiro) e zerava a dotacao de obras do mes. Usa
	// `works_share > 0` de proposito: o teste antigo so cobria 0.
	ogs::test::PocketWorld world;
	seed_cash(world, 50000.0, 30000.0);
	resize_urban(world);
	enable_treasury(world, 0.0, 0.25);
	ogs::treasury::collect(world.ws, "AAA", 800.0);
	ogs::treasury::start_month(world.ws);
	const double works_start = world.ws.city_gov[0].cash
		+ world.ws.city_gov[1].cash;
	CHECK(works_start == Approx(200.0));
	CHECK(works_start + world.ws.cities[0].government_budget
		+ world.ws.cities[1].government_budget == Approx(800.0));

	ogs::City& city = world.ws.cities[0];
	city.regional_budget_open_services = city.government_budget;
	spend_services(city, 40.0);
	world.ws.city_gov[0].cash -= 30.0;
	CHECK(ogs::treasury::reconcile(world.ws) == Approx(70.0));
	const double works_left = world.ws.city_gov[0].cash;
	CHECK(works_left > 0.0);
	CHECK(ogs::treasury::balance(world.ws, "AAA") == Approx(730.0));
	const double money_before = world.core.money_total();

	const ogs::save::Value saved =
		ogs::save::to_value(world.ws, world.core);
	ogs::test::PocketWorld target;
	ogs::save::from_value(target.ws, target.core, saved);
	CHECK(target.ws.national_treasury);
	CHECK(target.core.money_total() == Approx(money_before));
	CHECK(target.ws.city_gov.size() == target.ws.cities.size());
	CHECK(target.ws.city_gov[0].cash == Approx(works_left));
	CHECK(target.ws.cities[0].regional_budget_open_works
		== Approx(works_left));
	CHECK(target.ws.cities[0].regional_budget_open_services
		== Approx(target.ws.cities[0].government_budget));
	// O 1o reconcile depois do load nao inventa receita nem zera as obras.
	CHECK(ogs::treasury::reconcile(target.ws) == Approx(0.0));
	CHECK(ogs::treasury::balance(target.ws, "AAA") == Approx(730.0));
	CHECK(target.ws.treasury_collected_total == Approx(800.0));
	CHECK(target.ws.treasury_spent_total == Approx(70.0));
	CHECK(target.ws.city_gov[0].cash == Approx(works_left));
	CHECK(target.core.money_total() == Approx(money_before));
}

TEST_CASE("R1b: save v42 com a flag ON absorve o caixa municipal (QA t173)") {
	// Save anterior ao R1b (sem `national_treasury`/`treasury`) carregado com
	// a flag ligada pela configuracao: no mundo antigo `government_budget` e
	// `CityGov::cash` eram dinheiro real; sob ON sao limite. Sem absorver, o
	// caixa municipal saia de `money_total` (o dinheiro sumia). Absorver
	// conserva o total e o mes abre com dotacao.
	ogs::test::PocketWorld source;
	seed_cash(source, 50000.0, 30000.0);
	resize_urban(source);
	source.ws.cities[0].government_budget = 120.0;
	source.ws.cities[1].government_budget = 80.0;
	source.ws.city_gov[0].cash = 300.0;
	source.ws.city_gov[1].cash = 200.0;
	const double money_before = source.core.money_total();
	ogs::save::Value saved = ogs::save::to_value(source.ws, source.core);
	REQUIRE_FALSE(saved.has("national_treasury"));
	REQUIRE_FALSE(saved.has("treasury"));
	// v42: o estado real de um save pre-R1b.
	saved["version"] = ogs::save::Value(42);

	ogs::test::PocketWorld target;
	target.core.set_regional_cash(true);
	target.core.set_national_treasury(true, 1.0, 0.5, 0.35, 0.5);
	REQUIRE(target.ws.national_treasury);
	ogs::save::from_value(target.ws, target.core, saved);
	CHECK(target.ws.national_treasury);
	CHECK(target.core.money_total() == Approx(money_before));
	// O caixa municipal (700) virou saldo do Tesouro e arrecadacao.
	CHECK(ogs::treasury::balance(target.ws, "AAA") == Approx(700.0));
	CHECK(target.ws.treasury_collected_total == Approx(700.0));
	CHECK(target.ws.treasury_spent_total == Approx(0.0));
	CHECK(target.ws.regional_budget_started);
	double limit = 0.0;
	double services = 0.0;
	double works = 0.0;
	for (std::size_t index = 0; index < target.ws.cities.size(); ++index) {
		limit += target.ws.cities[index].regional_budget_limit_month;
		services += target.ws.cities[index].government_budget;
		works += index < target.ws.city_gov.size()
			? target.ws.city_gov[index].cash : 0.0;
	}
	CHECK(limit == Approx(700.0));
	CHECK(services + works == Approx(700.0));
	CHECK(works == Approx(245.0));
	// Nada a reconciliar: o 1o tick nao adia nem cria receita.
	CHECK(ogs::treasury::reconcile(target.ws) == Approx(0.0));
	CHECK(ogs::treasury::balance(target.ws, "AAA") == Approx(700.0));
	CHECK(target.core.money_total() == Approx(money_before));
}

TEST_CASE("R1b: despesa adiada nunca excede o saldo do Tesouro (QA t173)") {
	// Ramo defensivo do `reconcile`: o gasto ja saiu dos contadores (e foi
	// creditado ao destinatario) mas o Tesouro nao tem saldo. O adiado volta
	// ao contador limitado ao saldo que existia antes do debito; sem o clamp
	// um gasto sem lastro voltava inteiro e a despesa seguinte criava dinheiro
	// de novo. Com os limites = saldo da abertura o ramo nao ocorre.
	ogs::test::PocketWorld world;
	world.ws.city_gov.resize(world.ws.cities.size());
	enable_treasury(world, 0.0);
	ogs::treasury::collect(world.ws, "AAA", 1000.0);
	ogs::treasury::start_month(world.ws);
	ogs::City& city = world.ws.cities[0];
	city.regional_budget_open_services = city.government_budget;
	const double services_open = city.government_budget;
	// Tesouro curto: 40 para honrar um gasto de 400.
	world.ws.nation_treasury["AAA"] = 40.0;
	const double money_before = world.core.money_total();
	spend_services(city, 400.0);
	CHECK(ogs::treasury::reconcile(world.ws) == Approx(40.0));
	CHECK(ogs::treasury::balance(world.ws, "AAA") == Approx(0.0));
	CHECK(city.regional_budget_spent_month == Approx(40.0));
	// O adiado (360 sem o clamp) fica limitado ao saldo que havia: 40.
	CHECK(city.regional_budget_deferred_month == Approx(40.0));
	CHECK(city.regional_budget_deferred_month <= 40.0 + 1e-9);
	CHECK(city.government_budget == Approx(services_open - 400.0 + 40.0));
	CHECK(city.regional_budget_open_services
		== Approx(city.government_budget));
	// A reconciliacao so debita o que pagou (contadores nao sao caixa).
	CHECK(world.core.money_total() == Approx(money_before - 40.0));

	// Com lastro o pagamento e integral e nada fica adiado (paid == spent).
	ogs::test::PocketWorld backed;
	backed.ws.city_gov.resize(backed.ws.cities.size());
	enable_treasury(backed, 0.0);
	ogs::treasury::collect(backed.ws, "AAA", 1000.0);
	ogs::treasury::start_month(backed.ws);
	ogs::City& paid_city = backed.ws.cities[0];
	paid_city.regional_budget_open_services = paid_city.government_budget;
	spend_services(paid_city, 100.0);
	CHECK(ogs::treasury::reconcile(backed.ws) == Approx(100.0));
	CHECK(paid_city.regional_budget_deferred_month == Approx(0.0));
	CHECK(paid_city.regional_budget_spent_month == Approx(100.0));
	CHECK(ogs::treasury::balance(backed.ws, "AAA") == Approx(900.0));
}

TEST_CASE("R1b: desligar o caixa regional desliga o Tesouro (QA t173)") {
	// Configuracao coerente: o Tesouro so existe com o caixa unico regional.
	// Desligar o caixa regional desliga o Tesouro na mesma chamada e devolve
	// o saldo ao mundo OFF sem criar nem destruir dinheiro (com o mes aberto
	// os contadores sao limite, nao caixa).
	ogs::test::PocketWorld world;
	enable_treasury(world, 0.5);
	ogs::treasury::collect(world.ws, "AAA", 500.0);
	ogs::treasury::start_month(world.ws);
	CHECK(ogs::treasury::enabled(world.ws));
	const double money_before = world.core.money_total();
	world.core.set_regional_cash(false);
	CHECK_FALSE(world.ws.regional_cash);
	CHECK_FALSE(world.ws.national_treasury);
	CHECK_FALSE(ogs::treasury::enabled(world.ws));
	CHECK_FALSE(world.ws.treasury_config_error.empty());
	CHECK(ogs::treasury::total_balance(world.ws) == Approx(0.0));
	CHECK_FALSE(world.ws.regional_budget_started);
	CHECK(world.core.money_total() == Approx(money_before));
	CHECK(world.ws.cities[0].government_budget == Approx(500.0));
	CHECK(world.ws.cities[0].regional_budget_limit_month == Approx(0.0));
	const bool no_works_cash = world.ws.city_gov.empty()
		|| world.ws.city_gov[0].cash == Approx(0.0);
	CHECK(no_works_cash);
}
