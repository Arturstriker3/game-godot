#include "sim_day.h"

#include "profiling.h"
#include "urban_land.h"

namespace ogs {

void tick_day(WorldState& ws, SimCore& core) {
	// S5b: retrato, dinheiro antes, avanço do dia, obras de porto, remoção de
	// falidos/esgotados, índices e preços de abertura.
	core.prologue();

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

	// 5b. §19 W3: mercado de moradia (aluguel, construção, aperto).
	core.phase_housing();

	// F8: serviços.
	core.phase_services();

	// 6. Comércio de alimento e dos demais bens.
	core.phase_trade();

	// 7. Receita por construção, lucro, distribuição de capital e PIB.
	core.phase_accounting();

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

	// 10. Invariantes: se o tick corrompeu o estado (dinheiro não conservado,
	// caixa/estoque negativo ou preço fora da banda), registra e reverte.
	// O log fica na fachada.
	core.phase_invariants();
}

} // namespace ogs
