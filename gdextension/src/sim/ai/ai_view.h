#pragma once
// A5 — RETRATO CONGELADO DO DIA PARA A IA ECONÔMICA.
//
// A IA em C++ roda como fase do tick, DEPOIS de o dia fechar. Para que as
// regras não leiam o mundo vivo no meio das decisões, o dia é fotografado uma
// vez em `AiView` e todos os helpers leem SÓ essa cópia: nenhuma função daqui
// toca `ws.cities`/`ws.buildings` depois de `snapshot_ai_view`.
//
// O conteúdo é o MESMO do bloco `light` do status (S7-c) mais `market_totals` e
// o ranking de grupos:
//   * mercados cidade × produto: preço, estoque, oferta, excedente, vendido,
//     escassez e desejado — os 7 campos que a IA lê (`fill_light_signals`);
//   * POP por cidade: necessidade alimentar, consumo, satisfação, força de
//     trabalho e desemprego;
//   * construções por cidade (ordem cidade → construção de
//     `buildings_by_city`): id, produto de saída, dono, capacidade, utilização,
//     custos e aptidão do sítio (= `city.suitability[output_product]`);
//   * jazidas rastreadas por cidade/produto: reserva restante e
//     `SimCore::depth_opex_factor`;
//   * obras em andamento por cidade: produto de saída, capacidade e se é
//     expansão (`expand_existing`);
//   * incumbentes (cidade, recurso, grupo dono) com o MESMO filtro do bloco
//     `light`: `owner_kind` com grupo e `resource` não vazios;
//   * totais por produto: surplus, sold e desired somando SÓ os positivos, na
//     ordem das cidades (bit a bit igual a `market_totals`);
//   * ranking de grupos na ordem de `EconomySim::get_group_ranking` (receita
//     total desc, caixa desc, id asc), com o caixa do dia para a IA mutar.
//
// Além do `light`, a view guarda o mínimo de identidade por cidade (id, nação,
// massa de terra, caixa da firma, orçamento do governo), a aptidão territorial,
// a demanda externa por produto (as duas últimas já existem no bloco pesado
// do status) e o teto físico de terra cultivável (`City::land_capacity`, que a
// geografia declara), e uma cópia POSICIONAL de `ws.recipes`: os índices
// guardados em `AiTemplate::recipes` valem nessa cópia, então
// `best_recipe`/`variable_cost` resolvem a alternativa sem tocar o WorldState.
//
// Tudo isto é cópia de arrays do WorldState no instante do snapshot. Nenhum
// helper abaixo pode ser reescrito para ler o mundo vivo, nem mesmo "só o
// catálogo": o contrato é que o dia inteiro da IA enxerga um estado só.

#include "../world_state.h"
#include "ai_templates.h"

#include <string>
#include <vector>

namespace ogs {

class SimCore;

// Linha de mercado de uma cidade para um produto (os 7 doubles do `light`).
struct AiMarketRow {
	double price = 1.0;
	double stock = 0.0;
	double supply = 0.0;
	double surplus = 0.0;
	double sold = 0.0;
	double shortage = 0.0;
	double desired = 0.0;
};

// Escalares da POP que a IA lê (bloco `p_*` do `light`).
struct AiPopRow {
	double need = 0.0;
	double consumed = 0.0;
	double satisfaction = 0.0;
	double labor_force = 0.0;
	double unemployed = 0.0;
};

// Identidade e dotações da cidade que a IA usa para decidir (não são do bloco
// `light`, mas vêm do mesmo retrato do dia).
struct AiCityRow {
	std::string id;
	std::string nation;
	std::string landmass;
	double firm_cash = 0.0;
	double government_budget = 0.0;
	// `City::suitability` por produto (aptidão territorial 0..1).
	std::vector<double> suitability;
	// `City::external_demand` por produto (teto de exportação, t/dia).
	std::vector<double> external_demand;
};

// Linha do ranking de grupos (ordem de `get_group_ranking`).
struct AiGroupRow {
	std::string id;
	std::string origin_nation;
	std::string sector;
	bool state_owned = false;
	double cash = 0.0;
	double distributable = 0.0;
	double revenue_total = 0.0;
};

struct AiView {
	// Dimensões do retrato (== ws.cities.size() e ws.products no snapshot).
	int cities = 0;
	int products = 0;

	// Cidade → linha (tamanho = cities).
	std::vector<AiCityRow> city_rows;
	// Mercados cidade × produto: índice = city * products + product.
	std::vector<AiMarketRow> markets;
	// Teto físico agrícola cidade × produto (`City::land_capacity`): área
	// cultivável × produtividade FAO, o mesmo valor que
	// `GeographyService.city_land_capacity_t_day` declara ao núcleo. 0 = a
	// geografia não declarou teto para o produto (ausência explícita).
	std::vector<double> c_land_capacity;
	// POP por cidade (tamanho = cities).
	std::vector<AiPopRow> pop;

	// Construções por cidade em SoA: `b_offset` tem cities + 1 posições e a
	// faixa da cidade é [b_offset[c], b_offset[c + 1]) — ordem cidade →
	// construção de `buildings_by_city`, a mesma do bloco `light`.
	std::vector<int> b_offset;
	std::vector<std::string> b_id;
	std::vector<std::string> b_owner;
	std::vector<int> b_output;
	std::vector<double> b_capacity;
	std::vector<double> b_utilization;
	std::vector<double> b_costs;
	std::vector<double> b_site_suitability;
	// D-086 — estágio de mina por construção (ordem de `b_offset`): profundidade
	// de operação e produção relativa do estágio (0,05..1,0). 0/1,0 nas demais.
	std::vector<int> b_stage;
	std::vector<double> b_stage_factor;
	// D-086: custo do PRÓXIMO aprofundamento por construção (ordem de
	// `b_offset`), calculado pelo próprio núcleo (`SimCore::mine_deepen_cost`)
	// para a IA não duplicar a fórmula; 0.0 nas construções que não são mina.
	std::vector<double> b_deepen_cost;
	// Jazidas RASTREADAS por cidade (ordem de produto), offsets cities + 1.
	std::vector<int> d_offset;
	std::vector<int> d_product;
	std::vector<double> d_remaining_kt;
	std::vector<double> d_opex_factor;
	// D-086: qualidade da jazida (0..1) — o custo do aprofundamento usa a mesma
	// eficiência da extração (minério pobre = mais rocha por tonelada).
	std::vector<double> d_quality;

	// Obras em andamento por cidade (ordem de criação do vetor do núcleo),
	// offsets cities + 1. `c_expand` = 1 quando a obra soma capacidade a um
	// prédio existente (o produto já existe na cidade).
	std::vector<int> c_offset;
	std::vector<int> c_output;
	std::vector<double> c_capacity;
	std::vector<int> c_expand;

	// Incumbentes (F9): cidade, recurso e grupo dono — mesmo filtro e ordem do
	// bloco `light` (cidade → construção, dono-grupo, recurso não vazio).
	std::vector<int> i_city;
	std::vector<std::string> i_resource;
	std::vector<std::string> i_owner_group;

	// Totais por produto (tamanho = products): soma SÓ dos valores positivos,
	// na ordem das cidades — idêntico a `market_totals`.
	std::vector<double> tot_surplus;
	std::vector<double> tot_sold;
	std::vector<double> tot_desired;  // `market_totals.demand` no status

	// Ranking de grupos (mesma ordem de `EconomySim::get_group_ranking`).
	std::vector<AiGroupRow> groups;

	// Cópia POSICIONAL de `ws.recipes`: `AiTemplate::recipes` guarda índices
	// nesta cópia (e, por construção, em `ws.recipes`).
	std::vector<Recipe> recipes;
};

// Fotografa o dia em `view` (limpa e preenche). `core` só empresta
// `SimCore::owner_kind` (o parser de dono que o núcleo já tem) para montar a
// lista de incumbentes: nenhuma leitura viva acontece depois do snapshot.
void snapshot_ai_view(const WorldState& ws, const SimCore& core, AiView& view);

// ==== Helpers de leitura (só a view; nunca o WorldState vivo) ====

// Campo do mercado (as chaves `m_*` do bloco `light`).
enum AiMarketField : int {
	kAiPrice = 0,
	kAiStock,
	kAiSupply,
	kAiSurplus,
	kAiSold,
	kAiShortage,
	kAiDesired,
};

// Escalar da POP (as chaves `p_*` do bloco `light`).
enum AiPopField : int {
	kAiNeed = 0,
	kAiConsumed,
	kAiSatisfaction,
	kAiLaborForce,
	kAiUnemployed,
};

// == `IndustryTemplates.market_field` / `.pop_field` (ramo leve).
double ai_market(const AiView& view, int city, int product, AiMarketField field);
double ai_pop(const AiView& view, int city, AiPopField field);

// Faixa [begin, end) da cidade nas tabelas planas (== `_building_range`,
// `d_offset` e `c_offset` do `light`). Cidade sem contexto devolve 0/0.
int ai_b_begin(const AiView& view, int city);
int ai_b_end(const AiView& view, int city);
int ai_d_begin(const AiView& view, int city);
int ai_d_end(const AiView& view, int city);
int ai_c_begin(const AiView& view, int city);
int ai_c_end(const AiView& view, int city);

// == `IndustryTemplates.has_deposit` (D-086): existe jazida RASTREADA do
// produto na cidade (a reserva é referência de profundidade — NÃO se exige
// `remaining_kt > 0`; o recurso nunca acaba).
bool ai_has_deposit(const AiView& view, int city, int product);
// Fator de profundidade da jazida (1.0 sem jazida) — o `opex_factor` que o
// núcleo aplica ao opex de extração (== `variable_cost_mined`).
double ai_deposit_opex_factor(const AiView& view, int city, int product);
// D-086: qualidade da jazida (0..1; 0 sem jazida declarada). O custo do
// aprofundamento usa a eficiência da extração (0,5 + 0,5 × qualidade).
double ai_deposit_quality(const AiView& view, int city, int product);

// == `IndustryTemplates.pending_capacity`: capacidade em obra (nova + expansão).
double ai_pending_capacity(const AiView& view, int city, int product);
// == `IndustryTemplates.has_pending_build`: obra NOVA (não expansão) em voo.
bool ai_has_pending_build(const AiView& view, int city, int product);
// D-086: posição (índice nas tabelas planas da cidade) da mina do produto, ou
// -1. `state_only` restringe a mina do Estado; falso aceita qualquer dono que
// não seja o Estado (firma/grupo). O produto precisa ter jazida declarada.
// Entre as construções do produto com o dono pedido, devolve a de MENOR custo
// de próximo estágio (empate = primeira) — a ordem das construções no mundo é
// adversa de propósito (teste mine_stage).
int ai_mine_index(const AiView& view, int city, int product, bool state_only);
// D-086: capacidade ADICIONADA pelo aprofundamento da mina em `index` e o custo
// da obra. O custo vem do retrato do dia (`AiView::b_deepen_cost`), calculado
// pelo próprio núcleo (`SimCore::mine_deepen_cost`) — a IA NÃO duplica a
// fórmula. `city` é mantido por assinatura (a view já é por cidade).
double ai_mine_deepen_capacity(const AiView& view, int index);
double ai_mine_deepen_cost(const AiView& view, int city, int index);// == `IndustryTemplates.existing_capacity`: capacidade nominal instalada.
double ai_existing_capacity(const AiView& view, int city, int product);
// == `IndustryTemplates.producer_id`: id da PRIMEIRA construção da cidade com
// aquele output e owner != "state" ("" quando não há).
std::string ai_producer_id(const AiView& view, int city, int product);
// == `IndustryTemplates.has_producer`/`local_products`: alguma construção da
// cidade produz o bem (qualquer dono).
bool ai_local_has(const AiView& view, int city, int product);
// == `IndustryTemplates.observed_cost`: custo / produção efetiva (capacidade ×
// utilização) das construções do produto; -1 quando a produção é 0.
double ai_observed_cost(const AiView& view, int city, int product);

// == `GeographyService.city_land_capacity_t_day` (que o núcleo guarda em
// `City::land_capacity`): teto físico de capacidade agrícola (t/dia) da cidade
// para o produto. 0 quando a geografia não declarou teto (cidade/produto fora
// do retrato devolve 0 — ausência explícita, nunca estimativa).
double ai_land_capacity(const AiView& view, int city, int product);

// Índice O(1) das construções locais por produto (== `building_index`):
// contagem, soma da utilização efetiva (util/site, com piso 0.01 no sítio) e
// quantas entraram na soma.
struct AiBuildingIndex {
	std::vector<int> counts;
	std::vector<double> util_sum;
	std::vector<int> util_n;
};
AiBuildingIndex ai_build_index(const AiView& view, int city);
// == `IndustryTemplates.buildings_of_type`.
int ai_buildings_of_type(const AiView& view, int city, int product,
		const AiBuildingIndex& index);
// == `IndustryTemplates.avg_utilization` (1.0 quando não há construção).
double ai_avg_utilization(const AiView& view, int city, int product,
		const AiBuildingIndex& index);

// == `IndustryTemplates.derived_input_demand`: demanda derivada (t/dia) do
// insumo, somando a capacidade instalada de cada saída × o maior qty/yield
// entre as alternativas daquela saída.
double ai_derived_input_demand(const AiView& view, const AiTemplates& templates,
		int city, int product);
// == `IndustryTemplates.best_recipe`: índice em `view.recipes` (== `ws.recipes`)
// da alternativa de menor custo `Σ qty × preço / yield` (preço <= 0 vira 1.0);
// -1 quando nenhuma alternativa serve.
int ai_best_recipe(const AiView& view, const AiTemplates& templates, int city,
		const AiTemplate& tmpl);
// == `IndustryTemplates.variable_cost`: custo variável por tonelada com o preço
// do insumo de mercado (a melhor alternativa, quando é receita).
double ai_variable_cost(const AiView& view, const AiTemplates& templates,
		int city, const AiTemplate& tmpl);
// == `IndustryTemplates.variable_cost_mined`: custo variável por tonelada de
// mina, com o opex multiplicado pela profundidade da jazida.
double ai_variable_cost_mined(const AiView& view, const AiTemplates& templates,
		int city, const AiTemplate& tmpl);

} // namespace ogs
