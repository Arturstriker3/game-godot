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
#include "thread_pool.h"
#include "world_state.h"

#include <memory>

namespace ogs {

// Token seguro para id determinístico (letras/números; o resto vira '-').
inline std::string demo_sane_token(const std::string& text) {
	std::string out;
	out.reserve(text.size());
	for (const char c : text) {
		const unsigned char u = static_cast<unsigned char>(c);
		const bool ok = (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z')
			|| (u >= '0' && u <= '9');
		out.push_back(ok ? c : '-');
	}
	while (!out.empty() && out.back() == '-') {
		out.pop_back();
	}
	return out.empty() ? std::string("X") : out;
}

// S16 — buffer de redução por cidade para as fases de renda/consumo. As
// contribuições de cada cidade aos acumuladores globais (`foreign_cash`,
// `consumed_total`, `consumed_today`) são gravadas em slots disjuntos
// (índice da cidade × produto) e reduzidas em ordem crescente de cidade numa
// passada serial — reproduz a ordem de soma do loop serial original.
struct CityAccumScratch {
	std::vector<double> foreign;          // por cidade (saídas p/ o exterior)
	std::vector<double> consumed_total;   // cidade × produto
	std::vector<double> consumed_today;   // cidade × produto
	std::size_t n_products = 0;
};

class SimCore {
public:
	explicit SimCore(WorldState& ws);

	// ------------------------------------------------------------------
	// S16 — paralelismo determinístico por cidade.
	// ------------------------------------------------------------------
	// Define o número de threads das fases por cidade: 1 = serial (default);
	// <= 0 = automático (min(núcleos - 2, 8)); > 1 = esse número. As fases
	// globais (trade, fiscal, câmbio, invariantes) seguem seriais.
	void demo_set_sim_threads(int threads);
	int demo_sim_threads() const { return sim_threads_; }

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
	// 2b. F4: manutenção/compra mensal da frota e zera a fila do dia. Roda
	// antes do comércio para combustível/veículos comprados já valerem hoje.
	void phase_fleet();
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
	// 6. Comércio de alimento (cesta de calorias) e dos demais bens.
	void phase_trade();
	// 7. Receita por construção, lucro, distribuição de capital e PIB.
	void phase_accounting();
	// 8. Preços reagem a estoque, oferta, demanda e custo de produção.
	void phase_prices();
	// 9. Limite diário único por mercado (depois de todas as regras de preço).
	void phase_clamp();
	// 9b. Redistribuição fiscal nacional + dividendos/repatriação dos grupos.
	void phase_fiscal();
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
	// S16: grava as compras no scratch de redução por cidade (não no acumulador
	// global) para a fase de renda rodar paralela e determinística.
	double demo_consume_category(DemoCity& city, const DemoNeedsCategory& category,
		double want_ge, double& budget, double& spend,
		CityAccumScratch& scratch, std::size_t city_index);
	// F3: compra institucional do governo (paper/computer) no dia.
	void demo_consume_government(DemoCity& city,
		CityAccumScratch& scratch, std::size_t city_index);
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

	// ------------------------------------------------------------------
	// S6b — helpers de load/setup movidos da fachada.
	// ------------------------------------------------------------------
	// Refaz a tabela `buyer|seller|product` -> taxa (demo_set_tariff, save).
	void demo_rebuild_tariff_lookup();
	// F7: preenche população, escolaridade, participação e salários iniciais.
	void demo_apply_labor_profile(DemoCity& city);
	// F9: semeia os grupos a partir dos ativos privados (idempotente).
	void demo_seed_groups();
	// Id de receita -> índice no `recipe_lookup` (-1 se não existir).
	int demo_recipe_index(const std::string& recipe_id) const;

	// ------------------------------------------------------------------
	// §15 — construção como projeto físico (sim/construction.cpp).
	// ------------------------------------------------------------------
	// Calcula materiais/prazo e empilha a obra. Chamado pela fachada (build,
	// grupo e expansão). `expand_existing` soma capacidade a um prédio existente
	// em vez de criar um novo.
	void demo_add_construction_project(DemoConstructionProject& project);
	// Avança as obras do dia: materiais comprados do mercado, capacidade do
	// setor de construção, pagamento incremental do dono e conclusão.
	void demo_tick_construction();
	// Índice de uma obra em andamento pelo id do prédio final (-1 se não há).
	int demo_project_index(const std::string& building_id) const;
	// Marca a obra `strategic` (bootstrap industrial) pelo id do prédio final.
	void demo_mark_project_strategic(const std::string& building_id);

	// ------------------------------------------------------------------
	// Geometria do mapa e frete (S5b).
	// ------------------------------------------------------------------
	// Capacidade/custo/tempo de cada modal (calibragem do porto).
	static const ModeSpec& spec(TransportMode mode);
	static const char* scope_name(MarketScope scope);
	/** Cria um trecho e calcula viagens, tempo e custo. */
	Leg make_leg(TransportMode mode, const std::string& from, const std::string& to,
		const MapPoint& from_point, const MapPoint& to_point, double load_tons) const;
	double distance_km(const MapPoint& a, const MapPoint& b) const;

	// ------------------------------------------------------------------
	// Porto (S5b): capacidade, fila e cobrança.
	// ------------------------------------------------------------------
	const Port* demo_nearest_port(const std::string& nation,
		const std::string& landmass, const MapPoint& from) const;
	Port* find_demo_port(const std::string& id);
	int demo_city_index_near(const std::string& nation,
		const std::string& landmass, const MapPoint& from) const;
	void demo_init_port_capacities();
	void demo_reset_port_usage();
	double demo_port_charge(Port& port, double tons, double& demurrage_out);

	// ------------------------------------------------------------------
	// Frota nacional (F4 / S5b).
	// ------------------------------------------------------------------
	void demo_init_fleets();
	double demo_nation_sim_population(const std::string& nation) const;
	double demo_nation_income_index(const std::string& nation) const;
	bool demo_nation_food_secure(const std::string& nation) const;
	DemoTransportDemand demo_route_demand(const DemoCity& seller,
		const DemoCity& buyer, int seller_port, int buyer_port, double tons) const;
	double demo_fleet_capacity_scale(const std::string& nation,
		const DemoTransportDemand& demand, bool& fuel_limited) const;
	void demo_fleet_reserve(const std::string& nation,
		const DemoTransportDemand& demand);
	double demo_fleet_fuel_price() const;
	double demo_leg_unit_freight(TransportMode mode,
		double distance_km_value) const;
	double demo_leg_unit_freight_food(TransportMode mode,
		double distance_km_value) const;
	// Compra de frota: a fachada faz o parse de String e as validações de erro;
	// o núcleo compra no mercado interno da nação e credita a frota.
	double demo_fleet_buy_core(const std::string& nation, int product_index,
		double amount, double budget_share, double& spent_out,
		double& unfilled_out);

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
	// S16: número de threads configurado. 0 = automático (default,
	// min(núcleos-2, 8)); > 0 = esse número (1 = serial). O pool persistente é
	// criado sob demanda com o número efetivo de threads. Nunca usado fora do
	// tick (o writer já segura o mutex da fachada).
	int sim_threads_ = 0;
	std::unique_ptr<ThreadPool> pool_;
	// Número efetivo de threads (resolve o modo automático).
	int effective_threads() const;
	// Devolve o pool atual, recriando-o se o número de threads mudou.
	ThreadPool& pool();
	// S16: simula o dia de serviços de uma cidade (passos 1-8, sem abrir/fechar
	// negócios); grava a saída de divisas da exportação em foreign_slots[sector]
	// e a poupança acima da reserva de comida (calculada NO TOPO do dia) em
	// savings_surplus_out para a 2ª passada.
	void demo_services_day(DemoCity& city, double* foreign_slots,
		double& savings_surplus_out);
	// S16: abertura/fechamento de negócios em 2ª passada serial (ordem fixa
	// cidade -> setor), usando o savings_surplus do TOPO do dia (o serial lia a
	// reserva de comida antes de o passo 6 reduzir estoques — bit-idêntico).
	void demo_services_open_close(DemoCity& city, double savings_surplus);
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
	// 6. Comércio de alimento (cesta de calorias) entre cidades.
	void demo_trade_food();
	// 6b. Comércio dos demais bens (insumos industriais), pela mesma malha.
	void demo_trade_goods();
};

} // namespace ogs
