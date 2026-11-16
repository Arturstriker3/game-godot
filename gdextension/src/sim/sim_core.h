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
	// 5. Renda das famílias, impostos, transferências e consumo alimentar.
	void phase_income();
	// F8. Serviços: demanda das classes, B2B, governo, exportação, produção dos
	// negócios, preço, entrada e saída — uma cidade por vez.
	void phase_services();
	// 7. Receita por construção, lucro, distribuição de capital e PIB.
	void phase_accounting();
	// 8. Preços reagem a estoque, oferta, demanda e custo de produção.
	void phase_prices();
	// 9. Limite diário único por mercado (depois de todas as regras de preço).
	void phase_clamp();
	// 9c. Câmbio dinâmico: a balança comercial do dia move as moedas float.
	void phase_fx();
	// 10. Invariantes: se o tick corrompeu o estado, reverte o retrato.
	void phase_invariants();

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
	// F7 — custo diário da cesta completa de um nível de necessidade aos preços
	// que a cidade pratica, por substituto não-durável mais barato.
	double demo_level_basket_cost(const DemoCity& city, int level_index) const;
	// F7 comida — custo efetivo da necessidade alimentar do dia: estoque local
	// na proporção da compra mais o resíduo ao CIF conhecido.
	void demo_food_basket_cost(const DemoCity& city, double need,
		double& local_cost, double& import_cost, double& per_ge) const;
	// F3/F7: substituto preferido da categoria (sinal de desejo no mercado).
	bool demo_category_top_market(DemoCity& city, const DemoNeedsCategory& category,
		DemoMarket*& market, double& factor, double& durable_scale);
	// F3: registra o desejo da categoria sem comprar (sinal de escassez).
	void demo_register_category_demand(DemoCity& city,
		const DemoNeedsCategory& category, double want_ge);
	// F3: consome uma categoria de necessidade e debita o orçamento do dia.
	double demo_consume_category(DemoCity& city, const DemoNeedsCategory& category,
		double want_ge, double& budget, double& spend);
	// F3: compra institucional do governo (paper/computer) no dia.
	void demo_consume_government(DemoCity& city);
	// F7 calibração: classes de consumo a partir do Gini e da cesta básica.
	void demo_refresh_income_classes(DemoCity& city) const;
	// F5: câmbio/tarifas consultados pela fase de serviços.
	const DemoCurrency* demo_currency_for(const std::string& nation) const;
	double demo_competitiveness(const std::string& nation) const;
	double demo_fx_rate(const std::string& nation) const;
	const char* demo_currency_code(const std::string& nation) const;
	// Refaz os índices code->moeda e nação->moeda depois de carregar a lista.
	void demo_rebuild_currency_indexes();
	// Atualiza as taxas pela balança comercial do dia (moedas float).
	void demo_tick_fx();
	double demo_tariff_rate(const std::string& buyer_nation,
		const std::string& seller_nation, int product) const;
	int demo_product_index(const std::string& product) const;
	const char* demo_product_name(int product) const;
	// F8: serviços.
	int demo_service_sector_index(const std::string& sector) const;
	void demo_rebuild_service_indexes();
	double demo_city_food_reserve(const DemoCity& city) const;
	bool demo_service_open(DemoCity& city, int sector, double capacity,
		double capital);
	void demo_service_close(DemoServiceBusiness& business);
	void demo_seed_services();
	void demo_services_day(DemoCity& city);

	// F7: atualiza os salários por nível depois do lucro do dia (mercado
	// apertado/folgado, margem das firmas, âncora de renda do país, piso de
	// salário mínimo e rigidez). Chamado na fase de contabilidade.
	void demo_update_wages(DemoCity& city, double building_margin);
	// F9: propriedade e grupos.
	int demo_group_index(const std::string& id) const;
	DemoGroup* find_demo_group(const std::string& id);
	const DemoGroup* find_demo_group(const std::string& id) const;
	// Decompõe o dono de uma construção: kind 0 state, 1 national, 2 foreign,
	// 3 private (legado). Preenche grupo e nação de origem.
	int demo_owner_kind(const std::string& owner, std::string* group_id,
		std::string* origin_nation) const;
	int demo_fdi_mode(const std::string& nation, const std::string& sector) const;
	double demo_sector_tax_holiday(const std::string& nation,
		const std::string& sector) const;
	double demo_sector_subsidy(const std::string& nation,
		const std::string& sector) const;
	double demo_nation_confidence(const std::string& nation) const;
	// Credita `amount` no capital das famílias da nação, ponderado pelo estrato
	// dono (topo), e devolve o total efetivamente creditado.
	double demo_credit_nation_households(const std::string& nation, double amount);
	// Imposto corporativo + retenção: move o lucro do grupo da cidade para o
	// caixa dele (ou para o orçamento, se estatal). Chamado na contabilidade.
	void demo_route_group_profit(int group_index, const std::string& host_nation,
		double profit, double revenue, DemoCity& city, double& firm_surplus);

	// Dinheiro antes do tick (para a fase de invariantes, ainda na fachada).
	double money_before() const { return money_before_; }
	// Preços de abertura (para a fase de clamp, ainda na fachada).
	const std::vector<std::vector<double>>& open_prices() const { return open_prices_; }

	// Invariantes rígidos do tick: devolve a lista de violações (vazia se o
	// estado está sadio). A fachada é quem loga (`push_error`).
	std::vector<std::string> demo_check_invariants(double money_before) const;
	// Aplica um retrato salvo, revertendo o tick corrompido.
	void demo_restore(const DemoSnapshot& in);

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
