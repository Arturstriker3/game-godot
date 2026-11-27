// R1a - caixa unico regional (flag `rework.regional_cash`).
//
// Garantias, com o fixture `pocket_world` e o MESMO caminho do save real:
//   a) a fusao dos dois caixas conserva o total (money_total nao muda) e os
//      sub-saldos virtuais nascem dos caixas antigos;
//   b) a invariante DURA `firm_sub + service_sub == private_cash` vale em
//      credito, debito e transferencia interna;
//   c) o debito de cada setor e autorizado SO ate o seu sub-saldo (o pedido
//      pode ser maior; o retorno e o efetivamente pago; nada fica negativo);
//   d) transferencia interna (firmas <-> servicos) move valor entre os
//      sub-saldos e e no-op monetario no total;
//   e) o save/load com a flag ON preserva bit-a-bit o documento (com os dois
//      sub-saldos) e com a flag OFF o documento nao ganha chave nova;
//   f) o experimento t160-t163 (sub-teto externo por fatia) segue no codigo,
//      mas nunca libera acima do sub-saldo e o default e `off`.
#include "doctest/doctest.h"
#include "pocket_world.h"

#include "sim/ai/ai_view.h"
#include "sim/save_state.h"
#include "sim/scenario_fixture.h"
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

void check_sub_invariant(const ogs::City& city) {
	CHECK(city.firm_sub >= -1e-9);
	CHECK(city.service_sub >= -1e-9);
	CHECK(city.firm_sub + city.service_sub
		== Approx(city.private_cash).epsilon(1e-9));
	// Os espelhos publicados acompanham os sub-saldos no ON.
	CHECK(city.firm_cash == Approx(city.firm_sub));
	CHECK(city.service_cash == Approx(city.service_sub));
}

} // namespace

TEST_CASE("R1a: fusao do caixa conserva o total e semeia os sub-saldos") {
	ogs::test::PocketWorld world;
	seed_cash(world, 50000.0, 30000.0);
	const double before = world.core.money_total();

	world.core.set_regional_cash(true);

	CHECK(world.ws.regional_cash);
	CHECK(world.core.money_total() == Approx(before));
	for (const ogs::City& city : world.ws.cities) {
		CHECK(city.unified_cash);
		CHECK(city.private_cash == Approx(80000.0));
		// Semeadura: firm_sub/service_sub a partir dos caixas antigos.
		CHECK(city.firm_sub == Approx(50000.0));
		CHECK(city.service_sub == Approx(30000.0));
		check_sub_invariant(city);
	}

	// Desligar devolve o estado anterior: os dois caixas intactos e o pool 0.
	world.core.set_regional_cash(false);
	CHECK_FALSE(world.ws.regional_cash);
	CHECK(world.core.money_total() == Approx(before));
	for (const ogs::City& city : world.ws.cities) {
		CHECK_FALSE(city.unified_cash);
		CHECK(city.private_cash == Approx(0.0));
		CHECK(city.firm_sub == Approx(0.0));
		CHECK(city.service_sub == Approx(0.0));
	}
}

TEST_CASE("R1a: credito por origem mantem a invariante dos sub-saldos") {
	ogs::test::PocketWorld world;
	seed_cash(world, 50000.0, 30000.0);
	world.core.set_regional_cash(true);
	ogs::City& city = world.ws.cities[0];
	const double total_before = world.core.money_total();

	// Receita de bens -> firm_sub; receita de servicos -> service_sub.
	city.firm_receive(1000.0);
	CHECK(city.firm_sub == Approx(51000.0));
	CHECK(city.service_sub == Approx(30000.0));
	CHECK(city.private_cash == Approx(81000.0));
	check_sub_invariant(city);

	city.service_receive(2000.0);
	CHECK(city.firm_sub == Approx(51000.0));
	CHECK(city.service_sub == Approx(32000.0));
	CHECK(city.private_cash == Approx(83000.0));
	check_sub_invariant(city);
	CHECK(world.core.money_total() == Approx(total_before + 3000.0));
}

TEST_CASE("R1a: debito autorizado so ate o sub-saldo (sem negativo)") {
	ogs::test::PocketWorld world;
	seed_cash(world, 50000.0, 30000.0);
	world.core.set_regional_cash(true);
	ogs::City& city = world.ws.cities[0];

	// Pedido maior que o saldo da firma: paga o que existe, nao fica negativo
	// e nao consome o sub-saldo dos servicos.
	const double paid_firm = city.firm_pay(60000.0);
	CHECK(paid_firm == Approx(50000.0));
	CHECK(city.firm_sub == Approx(0.0));
	CHECK(city.service_sub == Approx(30000.0));
	CHECK(city.private_cash == Approx(30000.0));
	check_sub_invariant(city);

	// Mesma regra para os servicos: autoriza ate service_sub.
	const double paid_service = city.service_pay(99999.0);
	CHECK(paid_service == Approx(30000.0));
	CHECK(city.firm_sub == Approx(0.0));
	CHECK(city.service_sub == Approx(0.0));
	CHECK(city.private_cash == Approx(0.0));
	check_sub_invariant(city);

	// Zero disponivel: o pedido nao move dinheiro nem cria saldo negativo.
	CHECK(city.firm_pay(10.0) == Approx(0.0));
	CHECK(city.service_pay(10.0) == Approx(0.0));
	CHECK(city.private_cash == Approx(0.0));
	check_sub_invariant(city);
}

TEST_CASE("R1a: transferencia interna move o sub-saldo sem criar dinheiro") {
	ogs::test::PocketWorld world;
	seed_cash(world, 50000.0, 30000.0);
	world.core.set_regional_cash(true);
	ogs::City& city = world.ws.cities[0];
	const double total_before = world.core.money_total();
	const double pool_before = city.private_cash;

	// O mesmo par pay/receive usado por opex, insumos de servico, frete e
	// construcao: firmas pagam servicos.
	const double amount = 12000.0;
	const double firm_paid = city.firm_pay(amount);
	CHECK(firm_paid == Approx(amount));
	city.service_receive(firm_paid);

	CHECK(city.private_cash == Approx(pool_before));
	CHECK(world.core.money_total() == Approx(total_before));
	CHECK(city.firm_sub == Approx(38000.0));
	CHECK(city.service_sub == Approx(42000.0));
	check_sub_invariant(city);

	// E o inverso: servicos compram insumos das firmas.
	const double service_paid = city.service_pay(5000.0);
	CHECK(service_paid == Approx(5000.0));
	city.firm_receive(service_paid);
	CHECK(city.private_cash == Approx(pool_before));
	CHECK(city.firm_sub == Approx(43000.0));
	CHECK(city.service_sub == Approx(37000.0));
	check_sub_invariant(city);

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
			CHECK(city.firm_sub == Approx(0.0));
			CHECK(city.service_sub == Approx(0.0));
		}
		const std::string after = ogs::save::dump_json(
			ogs::save::to_value(target.ws, target.core));
		CHECK(after == before);
	}

	// ON: o documento carrega a flag, o pool e os dois sub-saldos; o roundtrip
	// e bit-exato.
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
			CHECK(city.firm_sub == Approx(50000.0));
			CHECK(city.service_sub == Approx(30000.0));
			check_sub_invariant(city);
		}
		const std::string after = ogs::save::dump_json(
			ogs::save::to_value(target.ws, target.core));
		CHECK(after == before);
	}
}

TEST_CASE("R1a: save legado ON sem sub-saldos e semeado dos espelhos") {
	// Um save ON anterior a correcao tem `private_cash` e os espelhos, mas nao
	// `firm_sub`/`service_sub`. O load semeia dos espelhos e preserva o total.
	ogs::test::PocketWorld world;
	seed_cash(world, 50000.0, 30000.0);
	world.core.set_regional_cash(true);
	ogs::City& city = world.ws.cities[0];
	// Estado que o load legado deixa: pool + espelhos, sub-saldos ausentes.
	city.firm_sub = 0.0;
	city.service_sub = 0.0;
	city.firm_cash = 50000.0;
	city.service_cash = 30000.0;
	city.private_cash = 80000.0;
	city.seed_subbalances_from_mirrors();
	CHECK(city.firm_sub == Approx(50000.0));
	CHECK(city.service_sub == Approx(30000.0));
	CHECK(city.private_cash == Approx(80000.0));
	check_sub_invariant(city);

	// Espelho negativo no save legado: a divida cruzada nao atravessa o
	// sub-saldo; o total permanece e o setor negativo zera.
	city.firm_cash = -2000.0;
	city.service_cash = 82000.0;
	city.private_cash = 80000.0;
	city.seed_subbalances_from_mirrors();
	CHECK(city.firm_sub == Approx(0.0));
	CHECK(city.service_sub == Approx(80000.0));
	CHECK(city.private_cash == Approx(80000.0));
	check_sub_invariant(city);
}

TEST_CASE("R1a: sub-teto externo por setor usa a fatia da receita") {
	ogs::test::PocketWorld world;
	seed_cash(world, 50000.0, 30000.0);
	// Experimento t160-t163 explicito: o default agora e `off`.
	world.core.set_regional_cash(true, 1);
	ogs::City& city = world.ws.cities[0];
	CHECK(city.unified_cash);
	// A fatia nasce da razao dos sub-saldos (50k de 80k) e o pool e 80k.
	CHECK(city.measured_firm_revenue_share() == Approx(0.625));
	CHECK(city.firm_external_room() == Approx(50000.0));
	CHECK(city.service_external_room() == Approx(30000.0));

	// Transferencia INTERNA move os sub-saldos; a fatia nao muda, mas o teto
	// efetivo nunca passa do sub-saldo.
	city.firm_pay(10000.0);
	city.service_receive(10000.0);
	CHECK(city.private_cash == Approx(80000.0));
	CHECK(city.firm_external_room() == Approx(40000.0));
	CHECK(city.service_external_room() == Approx(30000.0));

	// Pagamento EXTERNO das firmas consome o teto do setor e o sub-saldo.
	city.firm_external_pay(20000.0);
	CHECK(city.private_cash == Approx(60000.0));
	CHECK(city.firm_external_paid_today == Approx(20000.0));
	CHECK(city.firm_external_room() == Approx(20000.0));
	CHECK(city.service_external_room() == Approx(30000.0));

	// O servico paga a fatia dele DEPOIS da firma, sempre limitado ao saldo.
	city.service_external_pay(30000.0);
	CHECK(city.private_cash == Approx(30000.0));
	CHECK(city.service_external_room() == Approx(0.0));
	CHECK(city.firm_external_room() == Approx(20000.0));
	check_sub_invariant(city);

	// Desligado: o teto volta a ser o proprio caixa (caminho antigo).
	world.core.set_regional_cash(false);
	seed_cash(world, 50000.0, 30000.0);
	ogs::City& off_city = world.ws.cities[0];
	CHECK(off_city.firm_external_room() == Approx(50000.0));
	CHECK(off_city.service_external_room() == Approx(30000.0));
	off_city.firm_external_pay(12000.0);
	CHECK(off_city.firm_cash == Approx(38000.0));
	// Fora do pool, o contador diario nao e usado.
	CHECK(off_city.firm_external_paid_today == Approx(0.0));
}

TEST_CASE("R1a: sub-teto externo off (default) e isencao de essencial") {
	// Modo "off" (default): sem teto por fatia. O teto e o sub-saldo do setor
	// — os dois nunca leem o dinheiro um do outro.
	{
		ogs::test::PocketWorld world;
		seed_cash(world, 50000.0, 30000.0);
		world.core.set_regional_cash(true);
		ogs::City& city = world.ws.cities[0];
		CHECK(city.external_subceiling == 0);
		CHECK(city.firm_external_room() == Approx(50000.0));
		CHECK(city.service_external_room() == Approx(30000.0));
		const double paid = city.firm_external_pay(30000.0);
		CHECK(paid == Approx(30000.0));
		CHECK(city.private_cash == Approx(50000.0));
		CHECK(city.firm_external_room() == Approx(20000.0));
		CHECK(city.service_external_room() == Approx(30000.0));
		check_sub_invariant(city);
	}
	// Modo "nonessential": pagamento comum usa a fatia; o pagamento da cesta
	// alimentar (`essential=true`) usa o sub-saldo inteiro (nunca o pool).
	{
		ogs::test::PocketWorld world;
		seed_cash(world, 50000.0, 30000.0);
		world.core.set_regional_cash(true, 2);
		ogs::City& city = world.ws.cities[0];
		CHECK(city.external_subceiling == 2);
		CHECK(city.firm_external_room(false) == Approx(50000.0));
		CHECK(city.service_external_room(false) == Approx(30000.0));
		CHECK(city.firm_external_room(true) == Approx(50000.0));
		CHECK(city.service_external_room(true) == Approx(30000.0));
		// O essencial consome o sub-saldo da firma; o servico segue intacto.
		city.firm_external_pay(40000.0);
		CHECK(city.private_cash == Approx(40000.0));
		CHECK(city.firm_external_room(true) == Approx(10000.0));
		CHECK(city.service_external_room(true) == Approx(30000.0));
		check_sub_invariant(city);
	}
}

TEST_CASE("R1a: pendulo mensal da fatia usa a receita do dia") {
	ogs::test::PocketWorld world;
	seed_cash(world, 50000.0, 30000.0);
	world.core.set_regional_cash(true, 1);
	ogs::City& city = world.ws.cities[0];
	REQUIRE(world.ws.products > 0);
	const int product = 0;
	city.revenue_by_product[product] = 1000.0;
	city.service_revenue = 0.0;

	world.core.phase_reset();

	// Uma amostra 100% firmas anda 1/30 do caminho (0,625 -> 0,6375).
	CHECK(city.firm_revenue_share == Approx(0.6375));
	// O reset zera os pagamentos externos do dia.
	CHECK(city.firm_external_paid_today == Approx(0.0));
	CHECK(city.service_external_paid_today == Approx(0.0));
}

TEST_CASE("R1a: base investivel da IA usa o sub-saldo, sem duplo desconto") {
	ogs::test::PocketWorld world;
	seed_cash(world, 50000.0, 30000.0);
	world.core.set_regional_cash(true);
	ogs::AiView view;
	ogs::snapshot_ai_view(world.ws, world.core, view, nullptr);
	REQUIRE(view.city_rows.size() == world.ws.cities.size());
	const ogs::AiCityRow& row = view.city_rows[0];
	// A visao publica o sub-saldo das firmas (nao o pool); a fatia fica 1,0.
	CHECK(row.firm_cash == Approx(50000.0));
	CHECK(row.firm_revenue_share == Approx(1.0));
	CHECK(row.firm_cash * row.firm_revenue_share == Approx(50000.0));

	// Desligado, a visao e a base antiga (fatia 1, caixa das firmas).
	world.core.set_regional_cash(false);
	seed_cash(world, 50000.0, 30000.0);
	ogs::AiView off_view;
	ogs::snapshot_ai_view(world.ws, world.core, off_view, nullptr);
	const ogs::AiCityRow& off_row = off_view.city_rows[0];
	CHECK(off_row.firm_cash == Approx(50000.0));
	CHECK(off_row.firm_revenue_share == Approx(1.0));
}

// R1a (QA t167) - achados 1/M1 e 2/M2 e os menores.
TEST_CASE("R1a: save legado ON com pool 0 e espelhos opostos nao cria dinheiro") {
	ogs::test::PocketWorld world;
	seed_cash(world, 50000.0, 30000.0);
	world.core.set_regional_cash(true);
	ogs::City& city = world.ws.cities[0];
	// Estado do dreno t163 (save ON legado sem firm_sub/service_sub): o
	// pool armazenado e 0 e os espelhos estao cruzados. A semeadura NAO
	// pode trocar o pool pelo total dos espelhos (o clamp do negativo
	// viraria dinheiro novo).
	city.firm_sub = 0.0;
	city.service_sub = 0.0;
	city.private_cash = 0.0;
	city.firm_cash = -5000.0;
	city.service_cash = 5000.0;
	city.seed_subbalances_from_mirrors();
	CHECK(city.private_cash == Approx(0.0));
	CHECK(city.firm_sub == Approx(0.0));
	CHECK(city.service_sub == Approx(0.0));
	check_sub_invariant(city);

	// Pool positivo com os mesmos espelhos cruzados: preserva o pool e
	// manda tudo para o lado positivo (o negativo zera, sem compensacao
	// que aumente o total).
	city.firm_cash = -2000.0;
	city.service_cash = 42000.0;
	city.private_cash = 40000.0;
	city.seed_subbalances_from_mirrors();
	CHECK(city.private_cash == Approx(40000.0));
	CHECK(city.firm_sub == Approx(0.0));
	CHECK(city.service_sub == Approx(40000.0));
	check_sub_invariant(city);
}

TEST_CASE("R1a: firm_pay/service_pay negativo estornam como no OFF") {
	ogs::test::PocketWorld world;
	seed_cash(world, 50000.0, 30000.0);
	world.core.set_regional_cash(true);
	ogs::City& city = world.ws.cities[0];
	const double total_before = world.core.money_total();

	// Estorno: credita o setor e o pool, exatamente como o OFF credita o
	// caixa (antes o clamp do ON virava no-op).
	CHECK(city.firm_pay(-1000.0) == Approx(-1000.0));
	CHECK(city.firm_sub == Approx(51000.0));
	CHECK(city.service_sub == Approx(30000.0));
	CHECK(city.private_cash == Approx(81000.0));
	CHECK(world.core.money_total() == Approx(total_before + 1000.0));
	check_sub_invariant(city);

	CHECK(city.service_pay(-500.0) == Approx(-500.0));
	CHECK(city.service_sub == Approx(30500.0));
	CHECK(city.private_cash == Approx(81500.0));
	check_sub_invariant(city);

	// Desligado, a mesma chamada credita o caixa (semantica espelhada).
	world.core.set_regional_cash(false);
	seed_cash(world, 50000.0, 30000.0);
	ogs::City& off_city = world.ws.cities[0];
	CHECK(off_city.firm_pay(-1000.0) == Approx(-1000.0));
	CHECK(off_city.firm_cash == Approx(51000.0));
	CHECK(off_city.service_pay(-500.0) == Approx(-500.0));
	CHECK(off_city.service_cash == Approx(30500.0));
}

TEST_CASE("R1a: compra em outra cidade do mesmo pais nao consome o teto externo") {
	ogs::test::PocketWorld world;
	seed_cash(world, 50000.0, 30000.0);
	// Experimento t160-t163 ligado: a fatia existe e e restritiva.
	world.core.set_regional_cash(true, 1);
	ogs::City& city = world.ws.cities[0];
	city.firm_revenue_share = 0.05;
	CHECK(city.firm_external_room() == Approx(4000.0));

	// Vendedor do proprio pais: autoriza pelo sub-saldo (nao pela fatia) e
	// o pagamento NAO soma no teto externo do dia.
	CHECK(city.trade_firm_room(false, 10000.0) == Approx(50000.0));
	const double domestic = city.trade_firm_pay(false, 10000.0,
		ogs::City::kR1TradeDomestic);
	CHECK(domestic == Approx(10000.0));
	CHECK(city.firm_external_paid_today == Approx(0.0));
	check_sub_invariant(city);

	// Vendedor de fora do pais: consome o teto externo.
	const double abroad = city.trade_firm_pay(true, 10000.0,
		ogs::City::kR1ImportsAbroad);
	CHECK(abroad == Approx(10000.0));
	CHECK(city.firm_external_paid_today == Approx(10000.0));
	check_sub_invariant(city);
}

TEST_CASE("R1a: cenario minimo respeita a flag ON") {
	ogs::WorldState ws;
	ws.regional_cash = true;
	ws.external_subceiling = ogs::City::kSubceilingOff;
	ogs::SimCore core(ws);
	ogs::initialize_scenario_fixture(ws, core);
	REQUIRE(ws.cities.size() == 2);
	for (const ogs::City& city : ws.cities) {
		CHECK(city.unified_cash);
		CHECK(city.private_cash > 0.0);
		CHECK(city.firm_sub >= 0.0);
		CHECK(city.service_sub >= 0.0);
		CHECK(city.firm_sub + city.service_sub
			== Approx(city.private_cash).epsilon(1e-9));
		CHECK(city.firm_cash == Approx(city.firm_sub));
		CHECK(city.service_cash == Approx(city.service_sub));
	}
	// AGRIA/PORTUS do fixture: 50k+30k e 80k+60k.
	CHECK(ws.cities[0].private_cash == Approx(80000.0));
	CHECK(ws.cities[0].firm_sub == Approx(50000.0));
	CHECK(ws.cities[0].service_sub == Approx(30000.0));
	CHECK(ws.cities[1].private_cash == Approx(140000.0));
	CHECK(ws.cities[1].firm_sub == Approx(80000.0));
	CHECK(ws.cities[1].service_sub == Approx(60000.0));
}

TEST_CASE("R1a: service_pay no limite do sub-saldo e inclusivo e exato") {
	ogs::test::PocketWorld world;
	seed_cash(world, 50000.0, 30000.0);
	world.core.set_regional_cash(true);
	ogs::City& city = world.ws.cities[0];
	// Pedido exatamente igual ao sub-saldo: paga tudo, zera o setor e a
	// soma fecha o pool sem residuo (limite inclusivo).
	CHECK(city.service_pay(30000.0) == Approx(30000.0));
	CHECK(city.service_sub == Approx(0.0));
	CHECK(city.private_cash == Approx(50000.0));
	check_sub_invariant(city);
}
