#pragma once
// A5 — TEMPLATES DE ENGENHARIA DA IA ECONÔMICA (fonte única em C++).
//
// Os parâmetros de construção da IA (empregos/t, salário, opex, insumo,
// fator calórico e capital por capacidade) têm uma cópia só: a tabela
// `kPrimaries` de ai_templates.cpp. O GAME_STATE §15 mantém a lista
// documentada (mesmos números) e o espelho GDScript saiu no R0.2
// (`docs/economy/rework/R0.md`). Os três investidores e o governo leem daqui.
//
// Escopo: só DADO derivado de `WorldState` (goods.json + recipes.json). Nada
// de mercado, caixa, status ou RNG; nenhuma mutação do mundo. STL puro.
//
// Contrato de construção (`build_ai_templates`):
//   * `all` é a LISTA PERCORRIDA pelos laços da IA (== `_all_cache` do
//     GDScript): primários na ordem da TABELA (declaração, não do goods.json)
//     seguidos das receitas cuja saída NÃO é um primário, na ordem da primeira
//     aparição da saída em `ws.recipes` (== ordem do recipes.json). Primário
//     vence a receita do mesmo id: a receita `charcoal` (que produz `coal`)
//     fica fora de `all`, porque `coal` já está lá como primário. O produto de
//     um primário é resolvido por `ws.product_lookup` — nunca um índice fixo;
//     id ausente em goods.json faz a entrada ser PULADA.
//   * `recipe_templates` é a lista de receitas POR SAÍDA (== `_recipe_templates`
//     do GDScript): uma entrada por saída com receita, na ordem da primeira
//     aparição em `ws.recipes`. Inclui saídas que já são primárias (ex.: `coal`
//     via `charcoal`). É essa lista que `derived_input_demand` percorre no
//     GDScript — por isso ela não sofre o filtro de `all`.
//   * Um template de receita usa o PRIMEIRO recipe da saída (o de vitrine:
//     jobs/opex/capital); `recipes` guarda TODOS os índices daquela saída na
//     ordem do arquivo (é o que `best_recipe` percorre).
//   * Os conjuntos (`food`, `minerals`, `land_competing`, `bulk_low_value`,
//     `non_food`) são vetores de ÍNDICES em `all`, na ordem declarada — a
//     iteração da IA é sempre por vetor, nunca por hash.
//   * `recipes_by_output` tem uma posição por produto (`ws.products`), cada
//     uma com os índices de `ws.recipes` daquela saída na ordem do arquivo.
//
// O mapa `index_` é interno: só serve para lookup por nome (`by_resource`/
// `is_*`). Ele é reconstruído a cada `build_ai_templates` e nunca serializado.

#include <string>
#include <unordered_map>
#include <vector>

#include "../world_state.h"

namespace ogs {

// Parâmetros de engenharia de UMA construção (primário ou receita industrial).
struct AiTemplate {
	std::string resource;  // id do bem (goods.json): "wheat", "steel", ...
	int product = -1;      // índice do bem no núcleo
	int input = -1;        // -1 = extração primária; senão 1º insumo da receita
	double jobs_per_ton = 0.0;
	double wage = 0.0;
	double opex_per_ton = 0.0;
	// Primário: sempre 0. Receita: 0 (o insumo vem em `input` e as quantidades
	// por receita em `recipes`).
	double input_ratio = 0.0;
	// Tonelada-equivalente de grão por tonelada real (cesta alimentar). Os
	// primários não alimentares e as receitas ficam em 0; quem não declara é
	// tratado como 1.0 na cesta (ver kCaloricFactor/set_needs).
	double caloric_factor = 0.0;
	double capital_per_capacity = 2000.0;
	bool is_recipe = false;
	// Índices em `WorldState::recipes` (ordem do arquivo). Vazio nos primários.
	std::vector<int> recipes;
};

struct AiTemplates {
	// Lista percorrida pela IA: primários (ordem da tabela) + receitas cuja
	// saída não é primária (1ª aparição da saída). Primário vence a receita do
	// mesmo id.
	std::vector<AiTemplate> all;
	// Receitas por saída (1ª aparição da saída): inclui as saídas que já são
	// primárias (ex.: `coal` via receita `charcoal`). Fica FORA de `all` quando
	// o id já é primário; é a lista de `derived_input_demand`.
	std::vector<AiTemplate> recipe_templates;
	// Índices em `all` dos conjuntos da IA, na ordem declarada.
	std::vector<int> food;
	std::vector<int> minerals;
	std::vector<int> non_food;       // `all` sem as entradas de `food`
	std::vector<int> land_competing;
	std::vector<int> bulk_low_value;
	// Por produto: índices de `WorldState::recipes` daquela saída (ordem do
	// arquivo). Tamanho = ws.products.
	std::vector<std::vector<int>> recipes_by_output;

	// Lookup por nome de recurso (interno; preenchido em build_ai_templates).
	// Primeira aparição vence: o primário, não a receita do mesmo produto.
	std::unordered_map<std::string, int> index_;

	const AiTemplate* by_resource(const std::string& resource) const;
	bool is_food(const std::string& resource) const;
	bool is_mineral(const std::string& resource) const;
	bool is_land_competing(const std::string& resource) const;
	bool is_bulk_low_value(const std::string& resource) const;
	// Primeiro template de `all` com esse produto (-1 se não houver).
	int resource_index_for_product(int product) const;
};

// Reconstrói `out` (limpa e preenche) a partir de goods.json + recipes.json.
void build_ai_templates(const WorldState& ws, AiTemplates& out);

// ---------------------------------------------------------------------------
// R0.2 — VISÃO ESTÁTICA DOS PRIMÁRIOS (fonte única para o world-gen GDScript).
//
// A tabela `kPrimaries` continua sendo a única cópia dos números de engenharia
// dos 21 primários. Estas funções a expõem sem exigir um `WorldState` (o
// `GeographyService` semeia as minas ANTES de `initialize_world`) e sem
// duplicar a lista no GDScript. Os índices de produto NÃO aparecem aqui: quem
// cria o mundo resolve o id pelo `product_lookup`.
// ---------------------------------------------------------------------------
struct PrimaryTemplateInfo {
	std::string resource;
	double jobs_per_ton = 0.0;
	double wage = 0.0;
	double opex_per_ton = 0.0;
	double caloric_factor = 0.0;
};
std::vector<PrimaryTemplateInfo> primary_template_table();
std::vector<std::string> food_resource_ids();
std::vector<std::string> mineral_resource_ids();
std::vector<std::string> land_competing_resource_ids();
std::vector<std::string> bulk_low_value_resource_ids();

} // namespace ogs
