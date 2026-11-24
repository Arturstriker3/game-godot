#include "sim_day.h"

#include "bank_crisis.h"
#include "profiling.h"
#include "stock.h"
#include "urban_land.h"

namespace ogs {

void tick_day(WorldState& ws, SimCore& core) {
	// S5b: retrato, dinheiro antes, avanço do dia, obras de porto, remoção de
	// falidos/esgotados, índices e preços de abertura.
	core.prologue();

	// 2a0. B2: mobilidade mensal entre quintis + patrimônio por estrato (A6).
	// Roda ANTES do `phase_reset`: a renda que orienta o fluxo é a do dia
	// anterior (o reset zera `stratum_net_income`; rodar depois deixava a
	// mobilidade morta — achado do verificador).
	{
		OGS_SIM_ZONE(kPhaseDemography);
		core.phase_strata();
	}

	// §19 (W1): preço da terra por ocupação/renda e decadência do sinal de
	// demanda de zona (fechamento mensal; não move dinheiro).
	{
		OGS_SIM_ZONE(kPhaseLandMonth);
		urban_land::phase_month(ws);
	}

	// 1. Chegada das cargas pedidas no dia anterior.
	core.phase_arrival();

	// 2. Zera os fluxos do dia.
	core.phase_reset();

	// D-125: eficiência de produção do dia (transição sem produção,
	// combustível do trator/máquina e maturação). Antes da agricultura: a
	// safra e a produção do dia já saem com a eficiência atualizada.
	core.phase_efficiency();

	// 2b-G. Agricultura sazonal: plantio/fertilizante, colheita DENTRO da
	// janela (zero fora), perda de armazenagem e perecibilidade. Antes da
	// produção: a safra do dia já está no estoque quando a cidade compra.
	core.phase_agriculture();

	// 2b-G2. Armazenagem: a colheita com silo regional é desviada para o
	// pulmão e liberada aos poucos ao mercado da cidade-polo; perdas,
	// manutenção e custo de acesso. Antes da produção/comércio do dia.
	core.phase_storage();

	// 2b-E. PARTE E (D-116): energia — demanda, despacho por mérito, preço da
	// rede e racionamento. Antes do trabalho/produção: o fator de energia
	// limita o emprego industrial e a produção do dia.
	{
		OGS_SIM_ZONE(kPhasePower);
		core.phase_power();
	}

	// 2a. B1: demografia (fluxo mensal por cidade, cadência A6). Roda antes do
	// trabalho: a fase de labor já lê as coortes atualizadas do mês.
	{
		OGS_SIM_ZONE(kPhaseDemography);
		core.phase_demography();
	}

	// 2b. F4: manutenção/compra mensal da frota e zera a fila do dia. Roda
	// antes do comércio para combustível/veículos comprados já valerem hoje.
	{
		OGS_SIM_ZONE(kPhaseFleetDay);
		core.phase_fleet();
	}

	// 3. Mercado de trabalho e salários.
	core.phase_labor();

	// 4. Produção: cada construção declara insumo e produto.
	core.phase_production();

	// 4c. Recursos brutos (minerais, soja, algodão e madeira).
	core.phase_raw();

	// 5. Renda das famílias, impostos, transferências e consumo alimentar.
	core.phase_income();

	// 5b0. B3: economia da região (estados inteiros) — consumo diário,
	// emprego/salário semanais e estratos mensais (cadência A6). Roda depois
	// da comida urbana (a região compra o que sobra da prateleira da cidade) e
	// antes da moradia/serviços/comércio.
	{
		OGS_SIM_ZONE(kPhaseRegion);
		core.phase_region();
	}

	// 5b. §19 W3: mercado de moradia (aluguel, construção, aperto).
	core.phase_housing();

	// 5c. B4: migração (êxodo rural + cidade → cidade na mesma nação). Roda
	// DEPOIS da moradia: lê a vaga (vacância) e o sinal de aperto do fechamento
	// do dia, antes de serviços/comércio.
	{
		OGS_SIM_ZONE(kPhaseMigration);
		core.phase_migration();
	}

	// 5c1. B6: patrimônio da região (cadência MENSAL) — valoriza a terra pela
	// renda primária × infra B5 × commodities, fecha moradia/anel, cobra o
	// aluguel do anel (conservado) e publica a distribuição por quintil. Roda
	// DEPOIS da migração (o anel do mês já recebeu o excedente) e antes dos
	// serviços/comércio.
	core.phase_property();

	// F8: serviços.
	core.phase_services();

	// 5c2. B7: insatisfação (medição 0..1 por quintil/cidade/região). Roda
	// depois dos serviços e da moradia (aluguel pago e comida já fechados); não
	// move dinheiro nem pessoas.
	{
		OGS_SIM_ZONE(kPhaseSocial);
		core.phase_social();
	}

	// 6. Comércio de alimento e dos demais bens.
	core.phase_trade();

	// 7. Receita por construção, lucro, distribuição de capital e PIB.
	core.phase_accounting();

	// 7b. PARTE E (D-116): liquidação da energia — consumidor paga ao
	// gerador, combustível sai do mercado, O&M/salários e lucro do dono.
	// Depois da contabilidade: as firmas já receberam a receita do dia.
	{
		OGS_SIM_ZONE(kPhasePowerSettle);
		core.phase_power_settle();
	}

	// 8. Preços reagem a estoque, oferta, demanda e custo de produção.
	core.phase_prices();

	// 9. Limite diário único por mercado, aplicado depois de todas as regras
	// de preço.
	core.phase_clamp();

	// 9b-9b2. Redistribuição fiscal nacional + dividendos/repatriação dos
	// grupos.
	core.phase_fiscal();

	// 9c. Câmbio dinâmico: a balança comercial do dia move as moedas float.
	core.phase_fx();

	// 9d. C2b1: juro básico por moeda e emissão (só o BC controlado monetiza
	// o déficit medido do dia). Roda depois de todo o gasto/receita do dia, de
	// modo que o déficit e o caixa do governo do fecho sejam os medidos.
	core.phase_monetary();

	// 9e. C3 (D-107): crédito de empresa. Juro do dia, juros vencidos,
	// amortização pelo excedente ao capital de giro e — se o grupo ficar
	// insolvente além do prazo — o calote, com a perda no banco. Sem crédito
	// (default) a fase não toca no estado.
	core.phase_credit();

	// 9f. C5 (D-112): bolsa de valores. Lucro esperado e confiança (diários),
	// preço convergente no fechamento SEMANAL e dividendo no fechamento
	// MENSAL, pagos do caixa do emissor aos detentores (soma zero). A
	// valorização não move caixa. Roda antes dos invariantes para que a
	// reversão de um tick corrompido desfaça preço/confiança/dividendo.
	{
		OGS_SIM_ZONE(kPhaseStock);
		stock::phase(ws);
	}

	// 9g. C6 (D-114): crise bancária. Prêmio do seguro, indicadores e
	// confiança do depositante (com a confiança da bolsa acima), corrida com
	// redesconto do BC/papel-moeda/fuga para o banco público, aperto de
	// crédito e resolução do insolvente (bail-in → seguro → Tesouro →
	// incorporação). Roda depois da bolsa e antes dos invariantes: a
	// variação de depósitos da crise entra no dinheiro bancário do dia.
	{
		OGS_SIM_ZONE(kPhaseBankCrisis);
		bank_crisis::phase(ws);
	}

	// 9h. D-126: sobrevivência em crise. Reação determinística (corte já no
	// accounting, demissão, crédito de giro, liquidação, hibernação e leilão)
	// depois do crédito bancário e antes dos invariantes. O dinheiro bancário
	// criado pelos empréstimos do dia entra em `bank_money_today`.
	{
		OGS_SIM_ZONE(kPhaseCrisis);
		core.phase_crisis();
	}

	// 10. Invariantes: se o tick corrompeu o estado (dinheiro não conservado,
	// caixa/estoque negativo ou preço inválido — NaN/inf/≤0; sem banda), registra
	// e reverte. O log fica na fachada.
	core.phase_invariants();
}

} // namespace ogs
