// Implementação de ai_private.h (A5): porte fiel do investidor privado do
// GDScript (`core/economy/ai/private_investor.gd`) sobre o retrato do dia.
//
// Regra de ouro deste arquivo: cada função é a TRANSCRIÇÃO de uma função do
// GDScript — mesma ordem de leitura, mesmas condições, mesmas constantes e
// mesmas operações em ponto flutuante (`std::max`/`std::min`/`std::clamp`,
// `std::pow`, `std::sqrt`). Não introduza critério novo nem "simplifique" a
// ordem dos testes: o aceite é a equivalência bit a bit com o GDScript.
//
// Os únicos desvios são de CONTEXTO, listados em ai_private.h: a view congelada
// no lugar do dicionário `status`, os totais já somados em `view.tot_*`, as
// medidas do dia feitas uma vez em `prepare` e nenhum log de decisão.

#include "ai_private.h"

#include "ai_phase.h"
#include "ai_view.h"
#include "../sim_constants.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ogs {
namespace {

// Constantes de `private_investor.gd` (mesmos valores, mesmo significado).
constexpr double kCapitalShare = 0.35;
constexpr double kMinPaybackDays = 150.0;
// Horizonte de payback para bens bulk de baixo valor (enxofre): extratores de
// margem fina por tonelada mas reserva de décadas.
constexpr double kMinPaybackDaysBulk = 730.0;
// Fábricas (F2): a demanda final ainda é parcial (F3), então a indústria usa um
// horizonte de payback maior que o de uma mina/fazenda madura.
constexpr double kMinPaybackDaysIndustry = 730.0;
constexpr double kExportCapacityFactor = 1.2;
constexpr double kMinMarginPerTon = 0.5;
constexpr double kMinShortageTons = 0.5;
constexpr double kMinCapacity = 2.0;
constexpr double kMaxCapacity = 40.0;
constexpr double kMinSuitability = 0.30;
constexpr int kRiskWindow = 30;
constexpr double kBaseRisk = 0.15;
// F3 — prioridade alimentar: cadeias finais só com folga alimentar local.
constexpr double kFoodSlackForChains = 1.10;
constexpr double kFoodSupplyTarget = 1.0;
constexpr double kMinSuitabilityExport = 0.30;

// Maiúsculas ASCII (== `String.to_upper()` para os ids de bem, que são ASCII)
// usadas no id dos prédios criados pela IA.
std::string ascii_upper(const std::string& text) {
	std::string out = text;
	for (char& letter : out) {
		if (letter >= 'a' && letter <= 'z') {
			letter = static_cast<char>(letter - 'a' + 'A');
		}
	}
	return out;
}

// Conjunto O(1) dos produtos produzidos por construções locais
// (== `Templates.local_products`, chave = output_product).
std::vector<char> local_products_of(const AiView& view, int city) {
	std::vector<char> products(
		static_cast<std::size_t>(std::max(view.products, 0)), 0);
	const int begin = ai_b_begin(view, city);
	const int end = ai_b_end(view, city);
	for (int slot = begin; slot < end; ++slot) {
		const int product = view.b_output[static_cast<std::size_t>(slot)];
		if (product >= 0 && product < view.products) {
			products[static_cast<std::size_t>(product)] = 1;
		}
	}
	return products;
}

bool has_local(const std::vector<char>* products, int product) {
	if (products == nullptr || product < 0
			|| product >= static_cast<int>(products->size())) {
		return false;
	}
	return (*products)[static_cast<std::size_t>(product)] != 0;
}

// Oferta local de comida hoje, em t-eq de grão
// (== `Templates.local_food_supply`): soma do supply dos alimentos da cesta ×
// fator calórico, na ordem declarada da cesta.
double local_food_supply(const AiContext& ctx, int city_index) {
	const AiView& view = ctx.view;
	double total = 0.0;
	for (const int index : ctx.templates.food) {
		const AiTemplate& tmpl =
			ctx.templates.all[static_cast<std::size_t>(index)];
		if (tmpl.product < 0 || tmpl.product >= view.products) {
			continue;
		}
		total += ai_market(view, city_index, tmpl.product, kAiSupply)
			* tmpl.caloric_factor;
	}
	return total;
}

double caloric_factor_of(const AiContext& ctx, const std::string& resource) {
	const AiTemplate* tmpl = ctx.templates.by_resource(resource);
	return tmpl != nullptr ? tmpl->caloric_factor : 1.0;
}

} // namespace

void AiPrivateInvestor::prepare(const AiContext& ctx) {
	// Ordem de `prepare_totals`: comida, obras em andamento e grupos. Os totais
	// por produto NÃO são somados aqui: `view.tot_*` já é o `market_totals`.
	measure_food_balance(ctx);
	measure_pending(ctx);
	refresh_groups(ctx);
}

int AiPrivateInvestor::region_slot(const AiContext& ctx,
		const std::string& landmass) const {
	const int declared = ctx.ws.landmass_table.find(landmass);
	if (declared >= 0) {
		return declared;
	}
	const auto extra = extra_regions_.find(landmass);
	return extra == extra_regions_.end() ? -1 : extra->second;
}

void AiPrivateInvestor::measure_food_balance(const AiContext& ctx) {
	const AiView& view = ctx.view;
	AiState& state = ctx.state;
	// Massa de terra → índice: NameTable do núcleo; massa fora dele (status
	// montado à mão em teste) ganha um id depois das declaradas.
	extra_regions_.clear();
	int region_count = std::max(ctx.ws.landmass_table.size(), 0);
	for (const AiCityRow& row : view.city_rows) {
		if (ctx.ws.landmass_table.find(row.landmass) >= 0) {
			continue;
		}
		if (extra_regions_.find(row.landmass) == extra_regions_.end()) {
			extra_regions_.emplace(row.landmass, region_count);
			++region_count;
		}
	}
	state.region_need.assign(static_cast<std::size_t>(region_count), 0.0);
	state.region_supply.assign(static_cast<std::size_t>(region_count), 0.0);

	double need = 0.0;
	double supply = 0.0;
	// Mesmas somas do `light` (arrays planos), na ordem das cidades.
	for (int city_index = 0; city_index < view.cities; ++city_index) {
		const double city_need = std::max(0.0, ai_pop(view, city_index, kAiNeed));
		const double city_supply = local_food_supply(ctx, city_index);
		need += city_need;
		supply += city_supply;
		const int slot = region_slot(
			ctx, view.city_rows[static_cast<std::size_t>(city_index)].landmass);
		if (slot >= 0 && slot < region_count) {
			state.region_need[static_cast<std::size_t>(slot)] += city_need;
			state.region_supply[static_cast<std::size_t>(slot)] += city_supply;
		}
	}
	state.food_need = need;
	state.food_supply = supply;
}

void AiPrivateInvestor::measure_pending(const AiContext& ctx) {
	const AiView& view = ctx.view;
	// Capacidade futura em obras por produto (soma em TODAS as cidades, na
	// ordem cidade → obra da tabela plana `c_*`).
	sum_pending_.assign(static_cast<std::size_t>(std::max(view.products, 0)), 0.0);
	for (std::size_t index = 0; index < view.c_output.size(); ++index) {
		const int product = view.c_output[index];
		if (product >= 0 && product < view.products) {
			sum_pending_[static_cast<std::size_t>(product)] +=
				view.c_capacity[index];
		}
	}
}

void AiPrivateInvestor::refresh_groups(const AiContext& ctx) {
	// F9: buckets (nação|setor) dos grupos não estatais, na ordem do ranking
	// (`get_groups` == `get_group_ranking`). O caixa fica em
	// `state.private_group_cash`, debitado a cada build do mesmo dia.
	groups_by_key_.clear();
	const std::vector<AiGroupRow>& groups = ctx.view.groups;
	for (int index = 0; index < static_cast<int>(groups.size()); ++index) {
		const AiGroupRow& group = groups[static_cast<std::size_t>(index)];
		if (group.state_owned) {
			continue;
		}
		const std::string key = group.origin_nation + "|" + group.sector;
		groups_by_key_[key].push_back(index);
	}
}

bool AiPrivateInvestor::system_food_short(const AiContext& ctx,
		const std::string& region) const {
	double need = ctx.state.food_need;
	double supply = ctx.state.food_supply;
	if (!region.empty()) {
		const int slot = region_slot(ctx, region);
		if (slot >= 0
				&& slot < static_cast<int>(ctx.state.region_need.size())) {
			need = ctx.state.region_need[static_cast<std::size_t>(slot)];
			supply = slot < static_cast<int>(ctx.state.region_supply.size())
				? ctx.state.region_supply[static_cast<std::size_t>(slot)]
				: 0.0;
		}
	}
	return need > 0.0 && supply < need * kFoodSupplyTarget;
}

double AiPrivateInvestor::system_total(const AiContext& ctx, int product,
		AiTotalField field) const {
	const std::vector<double>* values = &ctx.view.tot_desired;
	if (field == kAiTotalSurplus) {
		values = &ctx.view.tot_surplus;
	} else if (field == kAiTotalSold) {
		values = &ctx.view.tot_sold;
	}
	// Produto fora do catálogo: ausência declarada (0,0), não estimativa.
	if (product < 0 || product >= static_cast<int>(values->size())) {
		return 0.0;
	}
	return (*values)[static_cast<std::size_t>(product)];
}

void AiPrivateInvestor::run_city(AiContext& ctx, int city_index) {
	if (city_index < 0 || city_index >= ctx.view.cities) {
		return;
	}
	// O GDScript rebinda o `light` na cidade aqui; a view já é por cidade.
	plan_city(ctx, city_index);
}

void AiPrivateInvestor::plan_city(AiContext& ctx, int city_index) {
	const AiView& view = ctx.view;
	const AiCityRow& city = view.city_rows[static_cast<std::size_t>(city_index)];
	const double firm_cash = city.firm_cash;
	const double capital_cost = kCapitalPerCapacity;
	const double available = firm_cash * kCapitalShare;
	if (available < capital_cost * kMinCapacity) {
		return;
	}
	const double food_need = std::max(0.0, ai_pop(view, city_index, kAiNeed));
	const double food_supply = local_food_supply(ctx, city_index);
	const double food_deficit = std::max(0.0, food_need - food_supply);
	// Folga alimentar local: habilita cadeias finais e lavouras industriais
	// (algodão/madeira) sem competir com a comida.
	const bool food_secure = food_need <= 0.0
		|| food_supply >= food_need * kFoodSlackForChains;
	const std::string& region = city.landmass;
	// Produtos produzidos localmente em O(1) (custo medido no laço quente).
	const std::vector<char> local_products = local_products_of(view, city_index);

	// 1. Alimento (B3): déficit alimentar (t-eq) → melhor cultura
	// (aptidão × preço). Sem déficit, mas com o mundo/região curto e comida
	// segura, a cidade excedente também expande a cultura de melhor
	// aptidão×preço. Sem déficit e sem escassez, só registra o excesso de
	// oferta (diagnóstico no GDScript — sem efeito no mundo, sem log aqui).
	if (food_deficit >= kMinShortageTons) {
		if (try_food(ctx, city_index, food_deficit, available, kMinSuitability)) {
			return;
		}
	} else if (system_food_short(ctx, region) && food_secure) {
		if (try_food(ctx, city_index, std::max(food_need, kMinShortageTons),
				available, kMinSuitabilityExport)) {
			return;
		}
	}

	// 2. Matérias-primas e bens industriais, na ordem de `all` sem a cesta
	// (ordem ESTÁVEL — não reordenar).
	for (const int index : ctx.templates.non_food) {
		const AiTemplate& tmpl =
			ctx.templates.all[static_cast<std::size_t>(index)];
		const std::string& resource = tmpl.resource;
		const int product = tmpl.product;
		if (product < 0) {
			continue;
		}
		double shortage = ai_market(view, city_index, product, kAiShortage);
		// Gate barato ANTES do trabalho pesado (custo medido).
		if (shortage < kMinShortageTons) {
			// R2/Q-014: lavouras industriais (algodão/madeira) não têm mercado
			// próprio formado — a escassez que importa é a DEMANDA DERIVADA da
			// cadeia a jusante já instalada, líquida da capacidade do insumo.
			if (!ctx.templates.is_land_competing(resource)) {
				continue;
			}
			shortage = ai_derived_input_demand(view, ctx.templates, city_index,
					product)
				- ai_existing_capacity(view, city_index, product);
			if (shortage < kMinShortageTons) {
				continue;
			}
		}
		if (tmpl.is_recipe) {
			// Cadeia final só com folga alimentar (F3).
			if (!food_secure) {
				continue;
			}
			if (system_total(ctx, product, kAiTotalDemand) <= 0.1) {
				continue;
			}
			// Atravessar a cadeia: a fábrica nasce antes do insumo quando ele é
			// produzível na cidade ou já tem oferta no sistema.
			if (!input_available(ctx, city_index, tmpl, &local_products)
					&& !chain_inputs_producible(ctx, city_index, tmpl)) {
				continue;
			}
		}
		if (try_template(ctx, city_index, resource, tmpl, shortage, available,
				&local_products)) {
			return;
		}
	}
}

bool AiPrivateInvestor::try_food(AiContext& ctx, int city_index,
		double food_deficit, double available, double min_suitability) {
	const AiView& view = ctx.view;
	const AiCityRow& city = view.city_rows[static_cast<std::size_t>(city_index)];
	const std::vector<double>& suitability = city.suitability;
	std::string best;
	double best_score = -1.0;
	for (const int index : ctx.templates.food) {
		const AiTemplate& tmpl =
			ctx.templates.all[static_cast<std::size_t>(index)];
		const int product = tmpl.product;
		if (product < 0 || product >= static_cast<int>(suitability.size())
				|| suitability[static_cast<std::size_t>(product)]
					< min_suitability) {
			continue;
		}
		const double price = ai_market(view, city_index, product, kAiPrice);
		// Prefere expandir uma cultura JÁ estabelecida na cidade (fix do churn
		// de milho redundante, B4): o bônus 2× evita diversificar por ganho
		// marginal de aptidão×preço.
		const bool established = ai_local_has(view, city_index, product);
		const double score = suitability[static_cast<std::size_t>(product)]
			* price * (established ? 2.0 : 1.0);
		if (best.empty() || score > best_score) {
			best_score = score;
			best = tmpl.resource;
		}
	}
	if (best.empty()) {
		return false;
	}
	const double shortage = food_deficit / caloric_factor_of(ctx, best);
	const AiTemplate* chosen = ctx.templates.by_resource(best);
	if (chosen == nullptr) {
		return false;
	}
	// `local_products` não é passado (== `{}` default do GDScript).
	return try_template(ctx, city_index, best, *chosen, shortage, available,
		nullptr);
}

bool AiPrivateInvestor::try_template(AiContext& ctx, int city_index,
		const std::string& resource, const AiTemplate& tmpl, double shortage,
		double available, const std::vector<char>* local_products) {
	const AiView& view = ctx.view;
	const AiCityRow& city = view.city_rows[static_cast<std::size_t>(city_index)];
	const bool is_recipe = tmpl.is_recipe;
	const int product = tmpl.product;
	const double sold = ai_market(view, city_index, product, kAiSold);
	const double surplus = ai_market(view, city_index, product, kAiSurplus);
	// §15/D-060: a capacidade futura em obras conta na decisão — a lacuna já
	// atendida por uma obra em voo não dispara outra construção.
	double effective_shortage = std::max(0.0,
		shortage - ai_pending_capacity(view, city_index, product));
	if (product < static_cast<int>(sum_pending_.size())) {
		// R3/Q-014: na CESTA ALIMENTAR o pending GLOBAL não desconta (a obra em
		// outra cidade não garante entrega nesta). A obra LOCAL continua
		// descontando (anti-empilhamento na cidade).
		if (!ctx.templates.is_food(resource)) {
			effective_shortage = std::max(0.0,
				effective_shortage
				- sum_pending_[static_cast<std::size_t>(product)]);
		}
	}

	// F3 — oferta mundial curta: para a cesta alimentar, o excedente LOCAL não
	// bloqueia a expansão em região excedente.
	const bool food_priority = ctx.templates.is_food(resource)
		&& system_food_short(ctx, city.landmass);
	if (surplus > std::max(1.0, sold * 0.20) && !food_priority) {
		return false;
	}
	const double system_surplus = system_total(ctx, product, kAiTotalSurplus);
	const double system_sold = system_total(ctx, product, kAiTotalSold);
	// Correção de raiz da biestabilidade (§11.3): o teto de "excesso de oferta
	// no sistema" NÃO se aplica a alimentos da cesta.
	if (system_surplus > std::max(1.0, system_sold * 0.25)
			&& !ctx.templates.is_food(resource)) {
		return false;
	}
	if (effective_shortage < kMinShortageTons) {
		return false;
	}
	if (!resource_available(ctx, city_index, tmpl)) {
		return false;
	}
	if (!input_available(ctx, city_index, tmpl, local_products)) {
		return false;
	}

	const double price = ai_market(view, city_index, product, kAiPrice);
	const double observed = ai_observed_cost(view, city_index, product);
	const bool is_mineral = ctx.templates.is_mineral(resource) && !is_recipe;
	// Margem "honesta" para minas: inclui o fator de profundidade da jazida
	// (opex_factor) quando ainda não há produtor observado.
	double unit_cost = 0.0;
	if (observed >= 0.0) {
		unit_cost = observed;
	} else if (is_mineral) {
		unit_cost = ai_variable_cost_mined(view, ctx.templates, city_index, tmpl);
	} else {
		unit_cost = ai_variable_cost(view, ctx.templates, city_index, tmpl);
		// §15/D-060: aptidão < 1.0 reduz a produção efetiva → custo por tonelada
		// produzida = custo ÷ aptidão (mesmo gate do governo).
		if (!is_recipe) {
			if (product >= 0
					&& product < static_cast<int>(city.suitability.size())) {
				const double apt =
					city.suitability[static_cast<std::size_t>(product)];
				if (apt > 0.0 && apt < 1.0) {
					unit_cost = unit_cost / apt;
				}
			}
		}
	}
	// Fábrica sem demanda madura: o preço de referência é o piso de custo.
	double reference_price = price;
	if (is_recipe) {
		reference_price = std::max(price, unit_cost * 1.05);
	}
	const double margin = reference_price - unit_cost;
	if (margin <= kMinMarginPerTon) {
		return false;
	}

	const double unit_capital = tmpl.capital_per_capacity;
	double capacity = std::clamp(effective_shortage, kMinCapacity, kMaxCapacity);
	if (is_mineral) {
		// Mesmo dimensionamento da mina estatal (B4): teto na demanda externa
		// × 1,2, para não superexpandir além do que o mercado absorve.
		const double external_demand = product >= 0
			&& product < static_cast<int>(city.external_demand.size())
			? city.external_demand[static_cast<std::size_t>(product)] : 0.0;
		capacity = std::clamp(
			std::min(effective_shortage,
				external_demand * kExportCapacityFactor),
			kMinCapacity, kMaxCapacity);
	}
	// Terra limitada: a fazenda (alimento da cesta) não pode ultrapassar a
	// capacidade física da área cultivável da cidade, menos o já instalado. Só
	// quando há dado de área (land_cap > 0): cidade sem bacia não tem teto.
	if (ctx.templates.is_food(resource)) {
		const double land_cap = ai_land_capacity(view, city_index, product);
		if (land_cap > 0.0) {
			const double used = ai_existing_capacity(view, city_index, product);
			capacity = std::min(capacity, std::max(0.0, land_cap - used));
		}
	}
	if (capacity * unit_capital >= available) {
		capacity = available / unit_capital;
	}
	if (capacity < kMinCapacity) {
		return false;
	}
	const double cost = capacity * unit_capital;
	const double daily_profit = margin * capacity;
	const double payback = cost / std::max(daily_profit, 0.001);
	const double risk = price_risk(ctx, city_index, product, reference_price);
	const double adjusted_payback = payback / std::max(0.5, 1.0 - risk);
	double payback_limit = kMinPaybackDays;
	if (is_recipe) {
		payback_limit = kMinPaybackDaysIndustry;
	} else if (ctx.templates.is_bulk_low_value(resource)) {
		payback_limit = kMinPaybackDaysBulk;
	}
	if (adjusted_payback > payback_limit) {
		return false;
	}

	const std::string city_id = city.id;
	const std::string existing = find_producer(ctx, city_index, product);
	AiActionResult result;
	if (!existing.empty()) {
		// D-086: mina existente é APROFUNDADA (obra que abre o estágio seguinte e
		// repõe a produção), não expandida por investimento genérico — é o mesmo
		// dinheiro e o mesmo §15, mas com o estágio no comando. O retorno é o do
		// APROFUNDAMENTO (t/dia adicionadas × margem ÷ custo da obra), não o da
		// planta hipotética do template: sem isso a decisão não compara
		// "aprofundar × abrir nova" no critério do dono.
		if (is_mineral) {
			const int mine_at = ai_mine_index(view, city_index, product, false);
			if (mine_at < 0) {
				return false;
			}
			const std::size_t at = static_cast<std::size_t>(mine_at);
			const double extra = ai_mine_deepen_capacity(view, mine_at)
				+ std::max(0.0, view.b_capacity[at])
					* (1.0 - view.b_stage_factor[at]);
			const double deepen_cost = ai_mine_deepen_cost(view, city_index,
				mine_at);
			const double deepen_profit = margin * extra;
			if (extra <= 0.0 || deepen_cost <= 0.0 || deepen_profit <= 0.0
					|| deepen_cost / deepen_profit > payback_limit) {
				return false;
			}
			result = ai_emit_deepen(ctx, city_id, existing);
		} else {
			// Expansão: id do produtor existente.
			result = ai_emit_invest(ctx, city_id, existing, cost);
		}
	} else {
		int input_product = tmpl.input;
		double input_ratio = tmpl.input_ratio;
		double jobs_per_ton = tmpl.jobs_per_ton;
		double wage = tmpl.wage;
		double opex_per_ton = tmpl.opex_per_ton;
		std::string recipe_id;
		if (is_recipe) {
			const int recipe_index =
				ai_best_recipe(view, ctx.templates, city_index, tmpl);
			if (recipe_index < 0) {
				return false;
			}
			const Recipe& recipe =
				view.recipes[static_cast<std::size_t>(recipe_index)];
			// Receita sem insumo não existe no catálogo do núcleo (o porte
			// recusa a alternativa em vez de inventar índice).
			if (recipe.input_count <= 0) {
				return false;
			}
			recipe_id = recipe.id;
			input_product = recipe.inputs[0];
			input_ratio = recipe.input_qty[0] / recipe.yield;
			jobs_per_ton = recipe.jobs_per_ton;
			wage = recipe.wage;
			opex_per_ton = recipe.opex_per_ton;
		}
		const int group = pick_group(ctx, city.nation, product, cost);
		if (group >= 0) {
			// O contador incrementa ANTES de emitir (como no GDScript): o id
			// carrega o valor corrente e o próximo build usa o seguinte.
			const int id = ctx.state.private_id++;
			const std::string target =
				"AI-GRP-" + std::to_string(id) + "-" + ascii_upper(resource);
			const std::string group_id =
				view.groups[static_cast<std::size_t>(group)].id;
			result = ai_emit_group_invest(ctx, group_id, city_id, target,
				resource, product, capacity, recipe_id, std::string());
		} else {
			const int id = ctx.state.private_id++;
			const std::string target =
				"AI-" + ascii_upper(resource) + "-" + std::to_string(id);
			result = ai_emit_build(ctx, city_id, target, resource, input_product,
				product, capacity, capacity * jobs_per_ton, wage,
				capacity * opex_per_ton, input_ratio, "private", cost,
				recipe_id);
		}
	}
	if (!result.error.empty()) {
		return false;
	}
	return true;
}

bool AiPrivateInvestor::resource_available(const AiContext& ctx, int city_index,
		const AiTemplate& tmpl) const {
	if (tmpl.is_recipe) {
		return true;  // transformação: depende de insumo, não de aptidão/jazida
	}
	if (tmpl.input >= 0) {
		return true;
	}
	const AiCityRow& city =
		ctx.view.city_rows[static_cast<std::size_t>(city_index)];
	const int product = tmpl.product;
	if (product < 0 || product >= static_cast<int>(city.suitability.size())
			|| city.suitability[static_cast<std::size_t>(product)]
				< kMinSuitability) {
		return false;
	}
	// §15/D-060: lavouras industriais (algodão/madeira) competem por terra com
	// a cesta e só são viáveis em terra boa (aptidão marginal produz fração da
	// capacidade e a fazenda fale quando o preço normaliza).
	if (ctx.templates.is_land_competing(tmpl.resource)
			&& city.suitability[static_cast<std::size_t>(product)] < 0.50) {
		return false;
	}
	if (ctx.templates.is_mineral(tmpl.resource)) {
		return ai_has_deposit(ctx.view, city_index, product);
	}
	return true;
}

bool AiPrivateInvestor::input_available(const AiContext& ctx, int city_index,
		const AiTemplate& tmpl, const std::vector<char>* local_products) const {
	const AiView& view = ctx.view;
	// Conjunto pré-calculado por cidade no laço quente; chamadas fora dele
	// (ex.: `try_food`) nunca passam receitas, então o fallback só recompõe o
	// conjunto quando há receita sem cache.
	std::vector<char> fallback;
	const std::vector<char>* local = local_products;
	if ((local == nullptr || local->empty()) && tmpl.is_recipe) {
		fallback = local_products_of(view, city_index);
		local = &fallback;
	}
	if (tmpl.is_recipe) {
		// A cidade só precisa ter ao menos UMA alternativa viável (estoque ou
		// produtor local de todos os insumos).
		for (const int recipe_index : tmpl.recipes) {
			if (recipe_index < 0
					|| recipe_index >= static_cast<int>(view.recipes.size())) {
				continue;
			}
			const Recipe& recipe =
				view.recipes[static_cast<std::size_t>(recipe_index)];
			bool viable = true;
			for (int slot = 0; slot < recipe.input_count; ++slot) {
				const int product = recipe.inputs[slot];
				const double has_stock =
					ai_market(view, city_index, product, kAiStock)
					+ ai_market(view, city_index, product, kAiSupply);
				if (has_stock > 0.1) {
					continue;
				}
				if (!has_local(local, product)) {
					viable = false;
					break;
				}
			}
			if (viable) {
				return true;
			}
		}
		return false;
	}
	const int input_product = tmpl.input;
	if (input_product < 0) {
		return true;
	}
	if (ai_market(view, city_index, input_product, kAiStock)
			+ ai_market(view, city_index, input_product, kAiSupply) > 0.1) {
		return true;
	}
	return has_local(local, input_product);
}

bool AiPrivateInvestor::chain_inputs_producible(const AiContext& ctx,
		int city_index, const AiTemplate& tmpl) const {
	const AiView& view = ctx.view;
	for (const int recipe_index : tmpl.recipes) {
		if (recipe_index < 0
				|| recipe_index >= static_cast<int>(view.recipes.size())) {
			continue;
		}
		const Recipe& recipe =
			view.recipes[static_cast<std::size_t>(recipe_index)];
		bool viable = true;
		for (int slot = 0; slot < recipe.input_count; ++slot) {
			const int product = recipe.inputs[slot];
			if (system_total(ctx, product, kAiTotalSold) > 0.1) {
				continue;
			}
			const int resource_index =
				ctx.templates.resource_index_for_product(product);
			if (resource_index < 0) {
				viable = false;
				break;
			}
			const AiTemplate& input_template =
				ctx.templates.all[static_cast<std::size_t>(resource_index)];
			if (!resource_available(ctx, city_index, input_template)) {
				viable = false;
				break;
			}
		}
		if (viable) {
			return true;
		}
	}
	return false;
}

std::string AiPrivateInvestor::find_producer(const AiContext& ctx,
		int city_index, int product) const {
	return ai_producer_id(ctx.view, city_index, product);
}

int AiPrivateInvestor::pick_group(AiContext& ctx, const std::string& nation,
		int product, double cost) {
	if (nation.empty()) {
		return -1;
	}
	// Setor do bem: `goods.json` (== `_good_sector`); vazio/ausente = primary.
	std::string sector = "primary";
	if (product >= 0
			&& product < static_cast<int>(ctx.ws.product_sectors.size())) {
		const std::string& declared =
			ctx.ws.product_sectors[static_cast<std::size_t>(product)];
		if (!declared.empty()) {
			sector = declared;
		}
	}
	const std::string key = nation + "|" + sector;
	const auto found = groups_by_key_.find(key);
	if (found == groups_by_key_.end()) {
		return -1;
	}
	const std::vector<int>& slots = found->second;
	for (const int index : slots) {
		if (index < 0
				|| index >= static_cast<int>(ctx.state.private_group_cash.size())) {
			continue;
		}
		double& cash =
			ctx.state.private_group_cash[static_cast<std::size_t>(index)];
		if (cash + 1e-9 >= cost) {
			// Caixa local do dia: a próxima decisão vê o valor já debitado.
			cash -= cost;
			return index;
		}
	}
	return -1;
}

double AiPrivateInvestor::price_risk(AiContext& ctx, int city_index, int product,
		double price) {
	// Chave (cidade × produto) sem colisão: cidade × 1e6 + produto.
	const std::uint64_t key =
		static_cast<std::uint64_t>(city_index) * 1000000ULL
		+ static_cast<std::uint64_t>(product);
	std::vector<double>& series = ctx.state.risk_history[key];
	series.push_back(price);
	while (series.size() > static_cast<std::size_t>(kRiskWindow)) {
		series.erase(series.begin());
	}
	if (series.size() < 5) {
		return kBaseRisk;
	}
	double mean = 0.0;
	for (const double value : series) {
		mean += value;
	}
	mean /= static_cast<double>(series.size());
	if (mean <= 0.0) {
		return kBaseRisk;
	}
	double variance = 0.0;
	for (const double value : series) {
		variance += std::pow(value - mean, 2.0);
	}
	variance /= static_cast<double>(series.size());
	return std::clamp(std::sqrt(variance) / mean, 0.0, 0.5);
}

} // namespace ogs
