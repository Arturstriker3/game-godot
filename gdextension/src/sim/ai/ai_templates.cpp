// Implementação de ai_templates.h (A5): a fonte única dos parâmetros de
// engenharia da IA econômica.
//
// A tabela de primários é a MESMA de core/economy/ai/industry_templates.gd
// (`TEMPLATES`) e do GAME_STATE §15 — mesmos números, uma cópia só. Os índices
// de produto NÃO são fixos aqui: saem de `ws.product_lookup`, então a ordem do
// goods.json pode mudar sem corromper a IA.
//
// Nenhuma ordem depende de hash: as entradas de `all` vêm da tabela (ordem de
// declaração) e depois das receitas na ordem do arquivo; os conjuntos são
// vetores de índices. O `index_` é só atalho de lookup por nome.
//
// `all` espelha o `_all_cache` do GDScript: primário vence a receita do mesmo
// id, então a receita `charcoal` (produz `coal`) não entra duas vezes e
// `non_food` também não. `recipe_templates` espelha `_recipe_templates`: uma
// entrada por saída com receita, inclusive saídas primárias.

#include "ai_templates.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace ogs {
namespace {

// Primário: `output` é o próprio id do bem (resolvido em product_lookup).
// Sempre sem insumo (input = -1, input_ratio = 0).
struct PrimarySpec {
	const char* resource;
	double jobs_per_ton;
	double wage;
	double opex_per_ton;
	double caloric_factor;
};

// ORDEM EXATA de `AiTemplates::all` para os primários (== declaração do
// TEMPLATES do GDScript, que é a ordem de decisão da IA).
constexpr PrimarySpec kPrimaries[] = {
	{"wheat", 7.5, 2.0, 2.5, 1.0},
	{"iron", 3.35, 1.0, 6.65, 0.0},
	{"rice", 7.5, 2.0, 2.5, 1.0},
	{"maize", 7.5, 2.0, 2.5, 1.0},
	{"potato", 7.5, 2.0, 2.5, 0.25},
	// Extração primária de minerais/energéticos (B4). Base: mina de ferro do
	// cenário (3,35 empregos/t, salário 1,0, opex 6,65/t). Bulk a céu aberto
	// (carvão, bauxita, fosfato, enxofre) é mais barato e menos mão de obra/t;
	// poços (petróleo/gás) são automatizados; metais profundos e de alto valor
	// têm mais opex. A profundidade da jazida ainda multiplica o opex no
	// núcleo (depth_opex_factor, até 3×).
	{"coal", 1.5, 1.0, 2.5, 0.0},
	{"copper", 3.0, 1.0, 8.0, 0.0},
	{"gas", 0.6, 1.5, 4.0, 0.0},
	{"oil", 0.8, 1.5, 6.0, 0.0},
	{"bauxite", 2.0, 1.0, 3.0, 0.0},
	{"lead", 3.0, 1.0, 8.0, 0.0},
	{"gold", 8.0, 1.5, 40.0, 0.0},
	{"phosphate", 2.0, 1.0, 3.0, 0.0},
	{"tungsten", 5.0, 1.2, 30.0, 0.0},
	{"rare_earths", 5.0, 1.2, 35.0, 0.0},
	{"sulfur", 2.0, 1.0, 2.0, 0.0},
	{"diamond", 10.0, 1.5, 50.0, 0.0},
	{"uranium", 4.0, 1.5, 45.0, 0.0},
	// Renováveis não alimentares: soja/algodão são lavouras (mesma base do
	// trigo); madeira é serraria/floresta (mais mecânica, menos gente). A soja
	// também é alimento da cesta (fator calórico 1,3).
	{"soy", 7.5, 2.0, 2.5, 1.3},
	{"cotton", 7.5, 2.0, 2.5, 0.0},
	{"wood", 5.0, 1.5, 3.0, 0.0},
};

// Cesta alimentar substituível (B3): trigo, arroz, milho, batata e soja.
constexpr const char* kFood[] = {"wheat", "rice", "maize", "potato", "soy"};

// Minerais/energéticos (extração primária com jazida).
constexpr const char* kMinerals[] = {
	"iron", "coal", "copper", "gas", "oil", "bauxite", "lead", "gold",
	"phosphate", "tungsten", "rare_earths", "sulfur", "diamond", "uranium",
};

// Bens cujo fator de produção divide a mesma terra/trabalho da cesta.
constexpr const char* kLandCompeting[] = {"cotton", "wood"};

// Bulk de alto volume e margem fina por tonelada (horizonte de payback maior).
constexpr const char* kBulkLowValue[] = {"sulfur"};

template <std::size_t N>
std::size_t count_of(const char* const (&)[N]) {
	return N;
}

bool contains(const std::vector<int>& set, int index) {
	return std::find(set.begin(), set.end(), index) != set.end();
}

// Índices de `all` para os nomes declarados, na ordem do array de nomes.
// Nome ausente em `all` é pulado (ausência explícita, nunca um palpite).
void collect_set(const std::unordered_map<std::string, int>& index,
		const char* const* names, std::size_t count, std::vector<int>& out) {
	out.clear();
	for (std::size_t slot = 0; slot < count; ++slot) {
		const auto found = index.find(names[slot]);
		if (found != index.end()) {
			out.push_back(found->second);
		}
	}
}

} // namespace

const AiTemplate* AiTemplates::by_resource(const std::string& resource) const {
	const auto found = index_.find(resource);
	if (found == index_.end()) {
		return nullptr;
	}
	const int index = found->second;
	if (index < 0 || index >= static_cast<int>(all.size())) {
		return nullptr;
	}
	return &all[static_cast<std::size_t>(index)];
}

bool AiTemplates::is_food(const std::string& resource) const {
	const auto found = index_.find(resource);
	return found != index_.end() && contains(food, found->second);
}

bool AiTemplates::is_mineral(const std::string& resource) const {
	const auto found = index_.find(resource);
	return found != index_.end() && contains(minerals, found->second);
}

bool AiTemplates::is_land_competing(const std::string& resource) const {
	const auto found = index_.find(resource);
	return found != index_.end() && contains(land_competing, found->second);
}

bool AiTemplates::is_bulk_low_value(const std::string& resource) const {
	const auto found = index_.find(resource);
	return found != index_.end() && contains(bulk_low_value, found->second);
}

int AiTemplates::resource_index_for_product(int product) const {
	for (int index = 0; index < static_cast<int>(all.size()); ++index) {
		if (all[static_cast<std::size_t>(index)].product == product) {
			return index;
		}
	}
	return -1;
}

void build_ai_templates(const WorldState& ws, AiTemplates& out) {
	out.all.clear();
	out.recipe_templates.clear();
	out.food.clear();
	out.minerals.clear();
	out.non_food.clear();
	out.land_competing.clear();
	out.bulk_low_value.clear();
	out.index_.clear();

	const int products = std::clamp(ws.products, 0, kMaxProducts);
	const int named = std::min(products,
		static_cast<int>(ws.product_names.size()));

	// 1. Primários, na ordem da tabela. Produto que não existe no goods.json
	// carregado é pulado (o núcleo continua funcionando com catálogo parcial).
	for (const PrimarySpec& spec : kPrimaries) {
		const auto found = ws.product_lookup.find(std::string(spec.resource));
		if (found == ws.product_lookup.end()) {
			continue;
		}
		AiTemplate entry;
		entry.resource = spec.resource;
		entry.product = found->second;
		entry.input = -1;
		entry.input_ratio = 0.0;
		entry.caloric_factor = spec.caloric_factor;
		entry.jobs_per_ton = spec.jobs_per_ton;
		entry.wage = spec.wage;
		entry.opex_per_ton = spec.opex_per_ton;
		entry.is_recipe = false;
		out.all.push_back(std::move(entry));
	}

	// 2. Receitas: percorre `ws.recipes` na ordem do arquivo e agrupa por
	// saída. A primeira aparição define a posição em `recipe_templates` e a
	// engenharia do template de vitrine (jobs/opex/capital).
	out.recipes_by_output.assign(static_cast<std::size_t>(products),
		std::vector<int>());
	std::vector<int> recipe_template_of_product(
		static_cast<std::size_t>(products), -1);
	for (int recipe_index = 0; recipe_index < static_cast<int>(ws.recipes.size());
			++recipe_index) {
		const Recipe& recipe = ws.recipes[static_cast<std::size_t>(recipe_index)];
		const int output = recipe.output;
		if (output < 0 || output >= products) {
			continue;
		}
		out.recipes_by_output[static_cast<std::size_t>(output)].push_back(
			recipe_index);
		if (recipe_template_of_product[static_cast<std::size_t>(output)] >= 0) {
			continue;
		}
		// Sem nome/id de produto não há como nomear o recurso: fica fora de
		// `recipe_templates`/`all` (mas continua listado em
		// `recipes_by_output`).
		if (output >= named) {
			continue;
		}
		AiTemplate entry;
		entry.resource = ws.product_names[static_cast<std::size_t>(output)];
		entry.product = output;
		entry.input = recipe.input_count > 0 ? recipe.inputs[0] : -1;
		entry.jobs_per_ton = recipe.jobs_per_ton;
		entry.wage = recipe.wage;
		entry.opex_per_ton = recipe.opex_per_ton;
		entry.capital_per_capacity = recipe.capital_per_capacity;
		entry.is_recipe = true;
		entry.input_ratio = 0.0;
		entry.caloric_factor = 0.0;
		recipe_template_of_product[static_cast<std::size_t>(output)] =
			static_cast<int>(out.recipe_templates.size());
		out.recipe_templates.push_back(std::move(entry));
	}
	// `recipes` de cada template de receita = TODAS as alternativas da saída
	// (os índices já estão na ordem do arquivo em `recipes_by_output`).
	for (AiTemplate& entry : out.recipe_templates) {
		const int product = entry.product;
		if (product < 0 || product >= products) {
			continue;
		}
		entry.recipes = out.recipes_by_output[static_cast<std::size_t>(product)];
	}

	// 3. `all` = primários + SOMENTE as receitas cuja saída ainda não é um
	// primário (primário vence a receita do mesmo id, como no `_all_cache`).
	// `index_` acompanha `all` (primeira aparição vence).
	out.index_.reserve(out.all.size() + out.recipe_templates.size());
	for (int index = 0; index < static_cast<int>(out.all.size()); ++index) {
		out.index_.emplace(out.all[static_cast<std::size_t>(index)].resource,
			index);
	}
	for (const AiTemplate& entry : out.recipe_templates) {
		if (out.index_.find(entry.resource) != out.index_.end()) {
			continue;
		}
		out.index_.emplace(entry.resource, static_cast<int>(out.all.size()));
		out.all.push_back(entry);
	}

	// 4. Conjuntos da IA, na ordem declarada (índices em `all`).
	collect_set(out.index_, kFood, count_of(kFood), out.food);
	collect_set(out.index_, kMinerals, count_of(kMinerals), out.minerals);
	collect_set(out.index_, kLandCompeting, count_of(kLandCompeting),
		out.land_competing);
	collect_set(out.index_, kBulkLowValue, count_of(kBulkLowValue),
		out.bulk_low_value);

	// 5. Não alimentares na ordem de `all` (primários, depois receitas).
	for (int index = 0; index < static_cast<int>(out.all.size()); ++index) {
		if (!contains(out.food, index)) {
			out.non_food.push_back(index);
		}
	}
}

namespace {

template <std::size_t N>
std::vector<std::string> names_to_vector(const char* const (&names)[N]) {
	std::vector<std::string> out;
	out.reserve(N);
	for (std::size_t slot = 0; slot < N; ++slot) {
		out.emplace_back(names[slot]);
	}
	return out;
}

} // namespace

// R0.2 — a tabela de primários e os conjuntos, sem depender de `WorldState`.
std::vector<PrimaryTemplateInfo> primary_template_table() {
	std::vector<PrimaryTemplateInfo> out;
	out.reserve(sizeof(kPrimaries) / sizeof(kPrimaries[0]));
	for (const PrimarySpec& spec : kPrimaries) {
		out.push_back({spec.resource, spec.jobs_per_ton, spec.wage,
			spec.opex_per_ton, spec.caloric_factor});
	}
	return out;
}

std::vector<std::string> food_resource_ids() {
	return names_to_vector(kFood);
}

std::vector<std::string> mineral_resource_ids() {
	return names_to_vector(kMinerals);
}

std::vector<std::string> land_competing_resource_ids() {
	return names_to_vector(kLandCompeting);
}

std::vector<std::string> bulk_low_value_resource_ids() {
	return names_to_vector(kBulkLowValue);
}

} // namespace ogs
