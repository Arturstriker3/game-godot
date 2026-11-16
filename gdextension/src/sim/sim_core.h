#pragma once
// Núcleo da simulação econômica (S5b do docs/economy/core_refactor_plan.md).
// C++ puro (só STL, via world_state.h/sim_types.h) — nada de godot_cpp.
//
// A classe `ogs::SimCore` recebe o `WorldState&` na construção e executa a
// ordem do dia: um prólogo (retrato, dinheiro, dia, obras, índices, preços de
// abertura) seguido de uma fase por etapa econômica. A fachada `EconomySim`
// (economy_sim.*) guarda UM `WorldState ws_` e UM `SimCore sim_core_{ws_}`,
// converte Godot <-> sim e loga os invariantes (o núcleo não emite push_error).
//
// S5b é fatiado: a cada commit um grupo de fases migra de `EconomySim` para
// cá. As fases que ainda não migraram continuam inline no `demo_tick_core`.

#include "sim_labor.h"
#include "world_state.h"

namespace ogs {

class SimCore {
public:
	explicit SimCore(WorldState& ws);

	// ------------------------------------------------------------------
	// Fases da ordem do dia (crescem a cada fatia de S5b).
	// ------------------------------------------------------------------
	// Prólogo: retrato para reversão, dinheiro antes, avanço do dia, obras de
	// porto, remoção de falidos/esgotados, índices e preços de abertura.
	void prologue();
	// 1. Chegada das cargas pedidas no dia anterior.
	void phase_arrival();
	// 2. Zera os fluxos do dia.
	void phase_reset();
	// 3. Mercado de trabalho e salários.
	void phase_labor();
	// 4. Produção: cada construção declara insumo e produto.
	void phase_production();
	// 4c. Recursos brutos (minerais, soja, algodão e madeira): exportação.
	void phase_raw();

	// ------------------------------------------------------------------
	// Helpers compartilhados com a fachada (públicos).
	// ------------------------------------------------------------------
	DemoCity* find_demo_city(const std::string& id);
	const DemoCity* find_demo_city(const std::string& id) const;
	void demo_rebuild_indexes();
	double demo_money_total() const;
	void demo_init_pop_needs(DemoPop& pop) const;
	static double demo_extraction_efficiency(const DemoDeposit& deposit);
	double demo_base_price(int product) const;
	int demo_food_product(int slot) const;
	double demo_food_calories(int product) const;
	// Multiplicador do opex pela profundidade da jazida (1.0 sem reserva).
	static double demo_depth_opex_factor(const DemoDeposit& deposit);
	// F7 calibração: TFP efetiva da cidade (fator explícito ou derivado do
	// PIB PPP per capita real).
	double demo_effective_productivity(const DemoCity& city) const;
	// F7: escolaridade das vagas por setor industrial (fallback da receita).
	static void demo_education_mix_for_sector(const std::string& sector,
		double* mix);
	// F9: âncoras ILO interpoladas no log do índice de renda.
	void demo_sector_anchor_values(double income, double* out) const;
	// F9: decompõe o emprego do dia em agro/indústria/serviços + informalidade.
	void demo_update_sector_employment(DemoCity& city);
	// F7 calibração: recalcula os estratos de renda a partir da escolaridade.
	void demo_refresh_strata(DemoPop& pop) const;
	// F7: fecha o dia do mercado de trabalho de uma cidade.
	double demo_labor_day(DemoCity& city);

	// Dinheiro antes do tick (para a fase de invariantes, ainda na fachada).
	double money_before() const { return money_before_; }
	// Preços de abertura (para a fase de clamp, ainda na fachada).
	const std::vector<std::vector<double>>& open_prices() const { return open_prices_; }

private:
	WorldState& ws_;
	double money_before_ = 0.0;
	// Preço de abertura por cidade/produto, capturado no prólogo e usado pela
	// fase de clamp (limite diário único). Reaproveitado entre dias.
	std::vector<std::vector<double>> open_prices_;

	// Retrato do estado para reverter o tick se um invariante rígido falhar.
	void demo_snapshot(DemoSnapshot& out) const;
	// Aplica obras de expansão de porto cujo prazo venceu neste dia.
	void demo_tick_port_expansion();
	// Libera os veículos de um pedido que chegou.
	void demo_fleet_release(const std::string& nation,
		const DemoTransportDemand& demand);
	// Escolhe a receita de menor custo unitário corrente para um produto.
	int demo_pick_recipe(const DemoCity& city, int output_product) const;
};

} // namespace ogs
