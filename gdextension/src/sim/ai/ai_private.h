#pragma once
// A5 — investidor privado da IA econômica em C++.
//
// PORTE FIEL de `core/economy/ai/private_investor.gd` (807 linhas): as mesmas
// funções, na mesma ordem de leitura, com as mesmas condições, constantes e
// operações em ponto flutuante. Nada aqui decide por critério novo — o
// aceite é a equivalência com o GDScript.
//
// Mapa função → função (GDScript → C++):
//   prepare_totals          → prepare (mede 1× por dia: comida, obras, grupos)
//   run / run_city          → run_city (plano de UMA cidade)
//   _run_city               → plan_city
//   _try_food               → try_food
//   _try_template           → try_template
//   _refresh_groups         → refresh_groups
//   _pick_group             → pick_group
//   _resource_available     → resource_available
//   _input_available        → input_available
//   _chain_inputs_producible→ chain_inputs_producible
//   _find_producer          → find_producer
//   _price_risk             → price_risk
//   system_food_short       → system_food_short
//   system_total            → system_total
//
// Diferenças de CONTEXTO (as únicas): os dados do dia vêm de `AiView`
// (ai_view.h) em vez do dicionário `status`/`light`; os totais do sistema já
// vêm somados em `view.tot_*` (== `market_totals`); a comida/região e a
// capacidade em obras globais são medidas UMA vez por dia em `prepare` (o
// `run_city` do GDScript as remediria se recebesse outro status); o teto de
// terra vem de `City::land_capacity` (== `GeographyService`); e não há log de
// decisão (não tem efeito no mundo nem entra no save).
//
// Contrato:
//   * uma ação por cidade/visita: expande o produtor existente (`invest`) ou
//     abre (`build` privado / `group_invest` com grupo nacional com caixa no
//     setor), na ordem de `AiTemplates::all` filtrada por `non_food`;
//   * todas as consultas de mercado/POP/construções/jazidas/obras vêm da
//     `AiView` congelada (ai_view.h), nunca do WorldState vivo;
//   * estado próprio (`_history` de preços, contador de id, caixa local dos
//     grupos) vive em `AiState` (ai_phase.h), nunca em membros deste struct.

#include <string>
#include <unordered_map>
#include <vector>

namespace ogs {

struct AiContext;
struct AiTemplate;

// Campo dos totais por produto (== `system_total(product, field)`).
enum AiTotalField : int {
	kAiTotalSurplus = 0,
	kAiTotalSold,
	kAiTotalDemand,
};

struct AiPrivateInvestor {
	// Mede UMA vez por dia o que não muda entre cidades: balanço alimentar
	// (mundo e massa de terra), capacidade futura em obras por produto e o
	// mapa de grupos por (nação|setor) com o caixa local do dia
	// (== `prepare_totals` no `plan()` do GDScript). O `AiPhase::run` chama
	// isto ANTES do laço de cidades.
	void prepare(const AiContext& ctx);

	// Plano de UMA cidade (== `run_city` do GDScript, sem rebind do `light`).
	void run_city(AiContext& ctx, int city_index);

private:
	// == `_measure_food_balance`: oferta × necessidade de comida em t-eq/dia,
	// no mundo (`state.food_need`/`food_supply`) e por massa de terra
	// (`state.region_need`/`region_supply`, indexados pelo id da massa).
	void measure_food_balance(const AiContext& ctx);
	// == `_measure_pending`: soma da capacidade em obras de TODAS as cidades
	// por produto (`_sum_pending`).
	void measure_pending(const AiContext& ctx);
	// == `_refresh_groups`: buckets (nação|setor) com o índice de `view.groups`
	// dos grupos não estatais, na ordem do ranking (== `get_groups`).
	void refresh_groups(const AiContext& ctx);
	// Índice da massa de terra (NameTable do núcleo; massa fora dele recebe um
	// id depois das declaradas). -1 = massa não resolvida.
	int region_slot(const AiContext& ctx, const std::string& landmass) const;

	// == `system_food_short`: oferta abaixo da necessidade × FOOD_SUPPLY_TARGET
	// no mundo ou na massa de terra informada ("" ou desconhecida = mundo).
	bool system_food_short(const AiContext& ctx,
		const std::string& region) const;
	// == `system_total`: total do sistema para um produto e campo.
	double system_total(const AiContext& ctx, int product,
		AiTotalField field) const;

	// == `_run_city`: comida → não alimentares → UMA ação → retorna.
	void plan_city(AiContext& ctx, int city_index);
	// == `_try_food`: melhor aptidão × preço (bônus 2× para cultura já
	// estabelecida) e os gates comuns.
	bool try_food(AiContext& ctx, int city_index, double food_deficit,
		double available, double min_suitability);
	// == `_try_template`: gates de oferta/margem/payback e a construção ou
	// expansão em si. `local_products` = conjunto O(1) de produtos locais
	// (nullptr = não fornecido, como o `{}` default do GDScript).
	bool try_template(AiContext& ctx, int city_index,
		const std::string& resource, const AiTemplate& tmpl, double shortage,
		double available, const std::vector<char>* local_products);
	// == `_resource_available`: aptidão/jazida/insumo ocioso.
	bool resource_available(const AiContext& ctx, int city_index,
		const AiTemplate& tmpl) const;
	// == `_input_available`: ao menos uma alternativa viável (estoque ou
	// produtor local de cada insumo).
	bool input_available(const AiContext& ctx, int city_index,
		const AiTemplate& tmpl, const std::vector<char>* local_products) const;
	// == `_chain_inputs_producible`: a fábrica nasce antes do insumo quando o
	// insumo tem oferta no sistema ou é produzível nesta cidade.
	bool chain_inputs_producible(const AiContext& ctx, int city_index,
		const AiTemplate& tmpl) const;
	// == `_find_producer`: id do primeiro produtor não estatal do produto ("").
	std::string find_producer(const AiContext& ctx, int city_index,
		int product) const;
	// == `_pick_group`: grupo nacional com caixa para o custo no setor do bem;
	// debita a cópia local do caixa (`state.private_group_cash`) e devolve o
	// índice em `view.groups` (-1 = nenhum).
	int pick_group(AiContext& ctx, const std::string& nation, int product,
		double cost);
	// == `_price_risk`: desvio/média de até 30 preços por cidade × produto.
	double price_risk(AiContext& ctx, int city_index, int product,
		double price);

	// Soma da capacidade em obras por produto, em todas as cidades
	// (== `_sum_pending`), medida em `prepare`.
	std::vector<double> sum_pending_;
	// (nação|setor) → índices em `view.groups`, na ordem do ranking
	// (== `_groups_by_key`).
	std::unordered_map<std::string, std::vector<int>> groups_by_key_;
	// Massas de terra sem id no NameTable do núcleo (status montado à mão em
	// teste/ferramenta): recebem um id depois das declaradas, para a soma por
	// região continuar existindo sem inventar índice.
	std::unordered_map<std::string, int> extra_regions_;
};

// D-125: eficiência média da rampa de uma planta NOVA do template no horizonte
// de decisão do investidor (a mesma conta do gate de payback do privado:
// `eff::mean_efficiency` sobre a curva da classe). Usada pelo investidor de
// grupo em `ai_group.cpp`; `existing` vazio e sem equipamento próprio.
double ai_expected_new_efficiency(const AiContext& ctx, int city_index,
	const AiTemplate& tmpl);

// D-131 (fase C) — helpers do cérebro compartilhados com o investidor de grupo
// (paridade): preço esperado com a série/cache do dia e prazo de obra da planta
// nova. Só devem ser chamados com `ai_brain.enabled`.
double ai_brain_expected_price(AiContext& ctx, int city_index, int product,
	double price, double supply_future, double demand_future);
int ai_brain_construction_days(const AiContext& ctx, const AiTemplate& tmpl,
	bool is_food, bool is_mineral, double capacity);

// D-131 (fase C) — crédito de INVESTIMENTO do privado (cidade): limite da
// D-126 (`city_credit_limit`, 0,5 × ativos − dívida) multiplicado pelo aperto
// de capital do banco público (`bank_crisis::credit_factor`). NÃO é o giro de
// crise (`credit_working_days`): 0 sem a mecânica de crédito ligada.
double ai_investment_credit(const AiContext& ctx, int city_index);

} // namespace ogs
