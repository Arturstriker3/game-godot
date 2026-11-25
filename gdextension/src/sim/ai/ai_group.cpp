// Implementação de ai_group.h (A5): porte fiel do investidor por grupo do
// GDScript (`core/economy/ai/group_investor.gd`) sobre o retrato do dia.
//
// Regra de ouro deste arquivo: cada função é a TRANSCRIÇÃO de uma função do
// GDScript — mesma ordem de leitura, mesmas condições, mesmas constantes e
// mesmas operações em ponto flutuante. Não introduza critério novo nem
// "simplifique" a ordem dos testes: o aceite é a equivalência com o GDScript.
//
// Diferenças de CONTEXTO (as únicas):
//   * o fatiamento por frame (`queue_day` / `run_slice` / `drain`) não existe:
//     `run` faz o dia inteiro de uma vez, na ordem final do `drain`;
//   * os dados do dia vêm de `AiView` (ai_view.h) em vez do dicionário
//     `status`/`light` (a base por recurso é montada do mesmo retrato);
//   * os sinais leves (`light`) viram `ai_market`/`ai_pop` e a base de obras
//     `c_output`/`c_capacity` da view;
//   * a tarifa vem de `SimCore::tariff_rate_text` (o `get_tariff` textual do
//     GDScript já resolve o nome do bem para o índice do núcleo);
//   * estado entre dias: só `AiState::group_cash`; `_policy_cache`,
//     `_tariff_cache`, `_incumbents` e `_base` são locais ao dia (o GDScript os
//     zera em `_prepare_caches`/`_load_signals`, que aqui é o começo de `run`);
//   * nenhum log de decisão (`_decisions`): não tem efeito no mundo nem entra
//     no save.

#include "ai_group.h"

#include "ai_phase.h"
#include "ai_private.h"
#include "ai_templates.h"
#include "ai_view.h"

#include "../ai_brain.h"
#include "../bank.h"
#include "../bank_crisis.h"
#include "../efficiency.h"
#include "../stock.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ogs {
namespace {

// Constantes de `group_investor.gd` (mesmos valores, mesmo significado).
constexpr double kMinCapital = 6000.0;
constexpr double kMaxCapacity = 12.0;
constexpr double kMinReturn = 0.0018;
constexpr double kFoodSecureSatisfaction = 0.98;
// D-131 fase C: mesma barreira de sítio do investidor privado
// (`kMinSuitability` em ai_private.cpp). Sem ela o grupo tentava toda cidade,
// inclusive aptidão 0 (produção zero, sem receita).
constexpr double kMinSuitability = 0.30;
constexpr double kCashShare = 0.5;

// Cesta alimentar na ORDEM de decisão (== `Templates.FOOD_RESOURCES`).
constexpr const char* kFoodResources[] = {
	"wheat", "rice", "maize", "potato", "soy",
};

bool is_food_resource(const std::string& resource) {
	for (const char* name : kFoodResources) {
		if (resource == name) {
			return true;
		}
	}
	return false;
}

// Política de IDE da nação anfitriã para um setor (== `_policy`).
struct FdiPolicyRow {
	int mode = 0;  // 0 open, 1 restricted, 2 joint_venture, 3 closed
	double min_local = 0.0;
};

// Parâmetros do template pré-lidos uma vez por recurso/dia
// (== `_resource_context`). `tmpl` continua válido: aponta para
// `ctx.templates.all`, que vive o dia inteiro.
struct ResourceContext {
	const AiTemplate* tmpl = nullptr;
	int product = -1;
	bool is_food = false;
	// Custo puramente interno (jobs × salário + opex): piso do custo variável,
	// usado para descartar candidato sem montar receita.
	double base_unit = 0.0;
	int input_product = -1;
	double input_ratio = 0.0;
	bool has_recipes = false;
	double capital_per_capacity = 2000.0;
};

// Candidato já aceito da base de um recurso (== entrada de `_base[r]`).
struct ResourceCandidate {
	int city = -1;
	std::string resource;
	int product = -1;
	double margin = 0.0;
	double capital_per_capacity = 0.0;
	std::string recipe;
	bool is_food = false;
	double price = 0.0;
	double shortage = 0.0;
	double desired = 0.0;
	// D-131 fase C: oferta efetiva/demanda futura do escopo no dia (R2), para
	// o preço esperado do forecast — o mesmo par que o privado usa.
	double supply_future = 0.0;
	double demand_future = 0.0;
	// D-125: eficiência média esperada da rampa (a mesma conta do investidor
	// privado, `ai_expected_new_efficiency`); 1,0 na cesta alimentar.
	double ramp = 1.0;
};

// Melhor oportunidade de um grupo (== retorno de `_score_candidate`).
struct Opportunity {
	bool valid = false;
	int city = -1;
	std::string resource;
	int product = -1;
	std::string recipe;
	double capacity = 0.0;
	double cost = 0.0;
	double margin = 0.0;
	double score = 0.0;
	bool foreign = false;
	std::string partner;
	double local_share = 0.0;
};

// Trabalho de UM dia de decisões de grupo. Vive na pilha de `run`: nada aqui
// sobrevive ao dia (o caixa local é `ctx.state.group_cash`).
struct GroupDay {
	explicit GroupDay(AiContext& context) : ctx(context) {}

	AiContext& ctx;

	// Sinais do dia.
	bool food_secure = true;
	// Capacidade futura em obras por produto, somada em TODAS as cidades
	// (== `_pending_global`).
	std::vector<double> pending_global;
	std::unordered_map<std::string, FdiPolicyRow> policy_cache;
	std::unordered_map<std::string, double> tariff_cache;
	// Índice "nação|recurso" → ids dos grupos donos (== `_incumbents`).
	std::unordered_map<std::string, std::unordered_set<std::string>> incumbents;
	bool incumbents_ready = false;

	// Base por recurso, válida para o dia (== `_base`, já completa).
	std::unordered_map<std::string, std::vector<ResourceCandidate>> base;

	// Fila de grupos da vez, na ordem do ranking (índices de `view.groups`).
	std::vector<int> queue;

	// D-125: média da rampa por (produto, classe) — a conta percorre a curva
	// dia a dia e o candidato é montado uma vez por (cidade, recurso).
	mutable std::unordered_map<long long, double> ramp_cache;

	// == `drain()`: prepara o dia e decide cada grupo da fila.
	void run();
	// == `_begin_group`: caixa local abaixo de MIN_CAPITAL não decide.
	bool has_capital(int group_index) const;
	// == `_advance_group` + `_finish_group`: melhor oportunidade do grupo.
	void decide_group(int group_index, Opportunity& best);
	// == `_resources_for`: cesta sempre; setor só com comida segura.
	void resources_for(const std::string& sector, std::vector<std::string>& out)
		const;
	// == `_resource_context`: template do recurso (`false` = sem template).
	bool resource_context(const std::string& resource, ResourceContext& out) const;
	// == `_build_base_slice` concluído: base do recurso, em cache do dia.
	const std::vector<ResourceCandidate>& base_for(const std::string& resource);
	// == `_append_candidate`: filtros exatos da base de um recurso.
	void append_candidate(std::vector<ResourceCandidate>& out, int city_index,
		const std::string& resource, const ResourceContext& context) const;
	// == `_score_candidate`: risco/retorno do grupo para um candidato.
	bool score_candidate(int group_index, const ResourceCandidate& entry,
		Opportunity& out);
	// D-125: média da rampa de uma planta nova do template (mesma função do
	// investidor privado); memorizada por (produto, classe) no dia.
	double ramp_efficiency(const AiTemplate& tmpl, int city_index,
		EfficiencyClass cls) const;
	// == `_execute`: emite o `group_invest` e debita o caixa local.
	void execute(int group_index, const Opportunity& opportunity);
	// == `_load_signals`: sinais do dia e caches zerados.
	void load_signals();
	// == `_compute_food_secure`: satisfação alimentar média do mundo.
	bool compute_food_secure(const AiView& view) const;
	// == `_policy`: primeira linha de `ws.fdi_policies` com nação e setor.
	FdiPolicyRow fdi_policy(const std::string& nation,
		const std::string& sector);
	// == `_confidence`: `ws.confidence` (default 1,0).
	double nation_confidence(const std::string& nation) const;
	// == `_tariff`: `get_tariff(nação, origem, recurso)` do GDScript.
	double tariff_rate(const std::string& nation, const std::string& origin,
		int product);
	// == `_is_incumbent`: o grupo dono aparece em nação|recurso?
	bool is_incumbent(const std::string& group_id, const std::string& nation,
		const std::string& resource);
	// == `_partner_for`: sócio nacional não estatal com caixa para a cota.
	std::string partner_for(const AiGroupRow& group, const std::string& nation,
		double required) const;
};

void GroupDay::run() {
	const AiView& view = ctx.view;
	// `_prepare_slice` passo 0: fila da vez, na ordem do ranking (a view já é
	// o `get_group_ranking` do dia). A6: período no estado do mundo (antes 30).
	queue.clear();
	const int interval = std::max(1, ctx.ws.cadence.monthly_period);
	const int phase = ctx.day % interval;
	for (int index = 0; index < static_cast<int>(view.groups.size()); ++index) {
		if (index % interval == phase) {
			queue.push_back(index);
		}
	}
	// `_prepare_slice` passo 1: comida segura + sinais do dia, ANTES dos
	// grupos (a ordem de `_load_signals` em relação à fila não muda valores).
	food_secure = compute_food_secure(view);
	load_signals();

	for (const int index : queue) {
		if (!has_capital(index)) {
			// Sem capital não há decisão: evita montar a base do setor à toa.
			continue;
		}
		Opportunity best;
		decide_group(index, best);
		if (best.valid) {
			ctx.state.brain_group_executed += 1;
			execute(index, best);
		}
	}
}

bool GroupDay::has_capital(int group_index) const {
	if (group_index < 0
			|| group_index >= static_cast<int>(ctx.state.group_cash.size())) {
		return false;
	}
	// C3 (D-107): o crédito disponível também conta como capital de decisão
	// (zero quando a mecânica está desligada).
	double available =
		ctx.state.group_cash[static_cast<std::size_t>(group_index)];
	if (group_index < static_cast<int>(ctx.state.group_credit.size())) {
		available +=
			ctx.state.group_credit[static_cast<std::size_t>(group_index)];
	}
	return available >= kMinCapital;
}

void GroupDay::decide_group(int group_index, Opportunity& best) {
	const AiView& view = ctx.view;
	const AiGroupRow& group = view.groups[static_cast<std::size_t>(group_index)];
	std::vector<std::string> resources;
	resources_for(group.sector, resources);
	// `_best_score` começa em MIN_RETURN: só uma oportunidade ESTRITAMENTE
	// melhor vence (empate fica com o primeiro recurso/cidade da ordem).
	// D-131 fase C: com o cérebro ligado o R1/R5 já é o portão duro de
	// solvência/payback; o score volta a ser só RANKING (o corte antigo de
	// 0,18%/dia bloqueava projetos que o forecast aprova). Desligado mantém
	// exatamente a regra antiga.
	double best_score = ctx.ws.ai_brain.enabled ? 0.0 : kMinReturn;
	best = Opportunity();
	for (const std::string& resource : resources) {
		const std::vector<ResourceCandidate>& candidates = base_for(resource);
		for (const ResourceCandidate& entry : candidates) {
			Opportunity candidate;
			if (!score_candidate(group_index, entry, candidate)) {
				continue;
			}
			if (candidate.score > best_score) {
				best = candidate;
				best_score = candidate.score;
			}
		}
	}
}

// Recursos na ordem de decisão: a cesta alimentar sempre; o setor do grupo só
// quando o mundo está com a comida segura. Os recursos do setor saem do
// goods.json na ordem dos produtos (== `_sector_of`, montado do arquivo), com
// duplicatas de comida puladas como no laço original.
void GroupDay::resources_for(const std::string& sector,
		std::vector<std::string>& out) const {
	out.clear();
	for (const char* name : kFoodResources) {
		out.emplace_back(name);
	}
	// D-131 fase C: com o cérebro ligado o setor do grupo também é avaliado
	// mesmo com a comida abaixo de 0,98. O motivo: o R1/R5 já reprova um
	// projeto insolvente ou que nasceria para hibernar, e a cesta alimentar
	// continua sendo avaliada PRIMEIRO na mesma decisão (o score decide). O
	// portão de comida de 0,98 era a proteção do mundo sem forecast; o caminho
	// `enabled=false` mantém exatamente a regra antiga.
	if (!food_secure && !ctx.ws.ai_brain.enabled) {
		return;
	}
	const WorldState& ws = ctx.ws;
	const int named = std::min(
		std::min(static_cast<int>(ws.product_names.size()),
			static_cast<int>(ws.product_sectors.size())),
		ws.products);
	for (int product = 0; product < named; ++product) {
		const std::string& declared =
			ws.product_sectors[static_cast<std::size_t>(product)];
		if (declared.empty() || declared != sector) {
			continue;
		}
		const std::string& resource =
			ws.product_names[static_cast<std::size_t>(product)];
		if (resource.empty()) {
			continue;
		}
		if (is_food_resource(resource)) {
			continue;
		}
		out.push_back(resource);
	}
}

bool GroupDay::resource_context(const std::string& resource,
		ResourceContext& out) const {
	const AiTemplate* tmpl = ctx.templates.by_resource(resource);
	if (tmpl == nullptr || tmpl->product < 0) {
		return false;
	}
	out = ResourceContext();
	out.tmpl = tmpl;
	out.product = tmpl->product;
	out.is_food = ctx.templates.is_food(resource);
	out.base_unit = tmpl->jobs_per_ton * tmpl->wage + tmpl->opex_per_ton;
	out.input_product = tmpl->input;
	out.input_ratio = tmpl->input_ratio;
	// `has_recipes` = o template tem alternativas (== `template.has("recipes")`).
	out.has_recipes = tmpl->is_recipe;
	out.capital_per_capacity = tmpl->capital_per_capacity;
	return true;
}

const std::vector<ResourceCandidate>& GroupDay::base_for(
		const std::string& resource) {
	const auto found = base.find(resource);
	if (found != base.end()) {
		return found->second;
	}
	std::vector<ResourceCandidate> out;
	ResourceContext context;
	// Recurso sem template fecha a base vazia na hora.
	if (resource_context(resource, context)) {
		for (int city_index = 0; city_index < ctx.view.cities; ++city_index) {
			append_candidate(out, city_index, resource, context);
		}
	}
	return base.emplace(resource, std::move(out)).first->second;
}

void GroupDay::append_candidate(std::vector<ResourceCandidate>& out,
		int city_index, const std::string& resource,
		const ResourceContext& context) const {
	const AiView& view = ctx.view;
	const int product = context.product;
	if (product < 0 || product >= view.products) {
		return;
	}
	// R2/Q-014: lavouras industriais (algodão/madeira) não têm mercado próprio
	// formado — a escassez que vale é a DEMANDA DERIVADA da cadeia a jusante já
	// instalada, líquida da capacidade do próprio insumo.
	double derived_gap = 0.0;
	if (ctx.templates.is_land_competing(resource)) {
		derived_gap =
			ai_derived_input_demand(view, ctx.templates, city_index, product)
			- ai_existing_capacity(view, city_index, product);
		if (derived_gap <= 0.0) {
			return;
		}
	}
	const double price = ai_market(view, city_index, product, kAiPrice);
	if (price <= 0.0) {
		return;
	}
	double shortage = ai_market(view, city_index, product, kAiShortage);
	const double sold = ai_market(view, city_index, product, kAiSold);
	const double desired = ai_market(view, city_index, product, kAiDesired);
	if (derived_gap > shortage) {
		shortage = derived_gap;
	}
	// §15/D-060: a escassez efetiva desconta a capacidade futura em obras — a
	// lacuna já atendida por uma obra em voo (local ou global) não dispara
	// outro candidato. R3/Q-014: na CESTA ALIMENTAR o pending GLOBAL não
	// desconta; a obra LOCAL continua descontando.
	const bool is_food = context.is_food;
	const bool brain = ctx.ws.ai_brain.enabled;
	const double raw_shortage = std::max(0.0, shortage);
	double scope_supply = 0.0;
	double scope_demand = 0.0;
	if (brain) {
		// D-131 fase B (R2): oferta efetiva do escopo (comida = região; demais
		// = mundo) já inclui obras em voo e `pending_committed` da fase. O
		// gatilho local continua sendo o teto da lacuna.
		const bool region_scope = is_food
			? ctx.ws.ai_brain.scope_food == 0
			: ctx.ws.ai_brain.scope_other == 0;
		const int region = region_scope ? ai_risk::region_slot(
			ctx.state.brain_supply,
			view.city_rows[static_cast<std::size_t>(city_index)].landmass) : -1;
		scope_supply = ai_risk::supply_effective(
			ctx.state.brain_supply, ctx.state, product, region, region_scope);
		scope_demand = ai_risk::demand_future(ctx.state.brain_supply,
			product, region, region_scope,
			ctx.ws.ai_brain.demand_growth_days);
		shortage = std::min(raw_shortage, std::max(0.0, scope_demand - scope_supply));
	} else {
		shortage = std::max(0.0,
			shortage - ai_pending_capacity(view, city_index, product));
		if (product < static_cast<int>(pending_global.size()) && !is_food) {
			shortage = std::max(0.0,
				shortage - pending_global[static_cast<std::size_t>(product)]);
		}
	}
	if (is_food) {
		// Comida: só onde há escassez local (nunca em excesso).
		if (shortage <= 0.0) {
			return;
		}
	} else if (shortage <= 0.0 && sold <= 0.0 && desired <= 0.0) {
		return;
	}
	const double base_unit = context.base_unit;
	// O corte por custo base é necessário, nunca suficiente: o insumo só
	// encarece — filtrar por ele não muda quem ganha.
	if (price <= base_unit) {
		return;
	}
	const std::vector<double>& suitability =
		view.city_rows[static_cast<std::size_t>(city_index)].suitability;
	// D-131 fase C (correção de escala): o privado só considera sítio com
	// aptidão mínima (`kMinSuitability`) e depósito para minério. O grupo
	// avaliava TODAS as cidades, inclusive aptidão 0 (receita zero e gate (d)
	// garantido) — 24% dos forecasts eram lixo. Mesma barreira dos dois lados.
	if (!context.has_recipes) {
		const double aptitude = product < static_cast<int>(suitability.size())
			? suitability[static_cast<std::size_t>(product)] : 0.0;
		if (aptitude < kMinSuitability) {
			return;
		}
		if (ctx.templates.is_mineral(resource)
				&& !ai_has_deposit(view, city_index, product)) {
			return;
		}
	}
	// §15/D-060: lavouras industriais só são viáveis em terra boa.
	if (ctx.templates.is_land_competing(resource)) {
		if (product >= static_cast<int>(suitability.size())
				|| suitability[static_cast<std::size_t>(product)] < 0.50) {
			return;
		}
	}
	// Alternativa de menor custo corrente na cidade (== `_light_best_recipe`)
	// — devolve índice em `view.recipes`, -1 quando nenhuma serve.
	const int recipe_index =
		ai_best_recipe(view, ctx.templates, city_index, *context.tmpl);
	if (context.has_recipes && recipe_index < 0) {
		return;
	}
	double capital_per_capacity = context.capital_per_capacity;
	double unit_cost = base_unit;
	std::string recipe_id;
	if (recipe_index >= 0) {
		const Recipe& recipe =
			view.recipes[static_cast<std::size_t>(recipe_index)];
		capital_per_capacity = recipe.capital_per_capacity;
		recipe_id = recipe.id;
		for (int slot = 0; slot < recipe.input_count; ++slot) {
			unit_cost += recipe.input_qty[slot] / recipe.yield
				* ai_market(view, city_index, recipe.inputs[slot], kAiPrice);
		}
		// F0 (D-123): a receita é um LOTE com co-produção — o valor de mercado
		// de TODAS as saídas extras abate o custo, igual ao `pick_recipe` e à
		// margem da firma. Era o defeito do diagnóstico t138: sem o crédito, a
		// refinaria custava 45/0,45 = 100/t contra 78,61 da gasolina e a IA
		// nunca a escolhia, então gasolina/diesel/querosene ficavam em 0.
		for (int slot = 0; slot < recipe.byproduct_count; ++slot) {
			const int byproduct = recipe.byproducts[slot];
			if (byproduct < 0 || byproduct >= view.products) {
				continue;
			}
			double price = ai_market(view, city_index, byproduct, kAiPrice);
			if (price <= 0.0) {
				// Fallback único do co-produto sem mercado (t145): âncora
				// do catálogo, o mesmo valor do `pick_recipe` e da margem.
				price = ai_base_price(view, byproduct);
			}
			unit_cost -= recipe.byproduct_qty[slot] * price / recipe.yield;
		}
		unit_cost = std::max(0.0, unit_cost);
	} else if (context.input_product >= 0) {
		unit_cost += context.input_ratio
			* ai_market(view, city_index, context.input_product, kAiPrice);
	}
	// §15/D-060: aptidão < 1,0 reduz a produção → custo por tonelada produzida
	// sobe para custo ÷ aptidão (mesmo gate do privado/governo).
	if (!context.has_recipes && !ctx.templates.is_mineral(resource)) {
		if (product >= 0 && product < static_cast<int>(suitability.size())) {
			const double apt = suitability[static_cast<std::size_t>(product)];
			if (apt > 0.0 && apt < 1.0) {
				unit_cost = unit_cost / apt;
			}
		}
	}
	if (capital_per_capacity <= 0.0) {
		return;
	}
	const double margin = price - unit_cost;
	if (margin <= 0.0) {
		return;
	}
	// D-125: o retorno desconta a rampa de maturação, como no investidor
	// privado (`ai_expected_new_efficiency`). A cesta alimentar mantém o peso
	// cheio: a comida é prioridade declarada do grupo e o candidato só nasce
	// com escassez local.
	double ramp = 1.0;
	if (!is_food && context.tmpl != nullptr) {
		const City& city = ctx.ws.cities[static_cast<std::size_t>(city_index)];
		const EfficiencyClass cls = eff::classify_new(ctx.ws, city, product,
			recipe_index, context.input_product);
		ramp = ramp_efficiency(*context.tmpl, city_index, cls);
	}
	ResourceCandidate entry;
	entry.city = city_index;
	entry.resource = resource;
	entry.product = product;
	entry.margin = margin;
	entry.capital_per_capacity = capital_per_capacity;
	entry.recipe = recipe_id;
	entry.is_food = is_food;
	entry.price = price;
	entry.shortage = shortage;
	entry.desired = desired;
	entry.ramp = ramp;
	entry.supply_future = scope_supply;
	entry.demand_future = scope_demand;
	out.push_back(std::move(entry));
}

double GroupDay::ramp_efficiency(const AiTemplate& tmpl, int city_index,
		EfficiencyClass cls) const {
	const long long key = static_cast<long long>(tmpl.product)
		* (static_cast<long long>(EfficiencyClass::kRanch) + 1)
		+ static_cast<int>(cls);
	const auto found = ramp_cache.find(key);
	if (found != ramp_cache.end()) {
		return found->second;
	}
	const double value = ai_expected_new_efficiency(ctx, city_index, tmpl);
	ramp_cache[key] = value;
	return value;
}

bool GroupDay::score_candidate(int group_index, const ResourceCandidate& entry,
		Opportunity& out) {
	const AiView& view = ctx.view;
	if (group_index < 0
			|| group_index >= static_cast<int>(ctx.state.group_cash.size())) {
		return false;
	}
	const AiGroupRow& group = view.groups[static_cast<std::size_t>(group_index)];
	const double cash =
		ctx.state.group_cash[static_cast<std::size_t>(group_index)];
	// C3 (D-107): o crédito disponível do dia entra como capacidade de capex
	// (zero quando a mecânica está desligada — o mundo default não muda).
	const double credit =
		group_index < static_cast<int>(ctx.state.group_credit.size())
		? ctx.state.group_credit[static_cast<std::size_t>(group_index)] : 0.0;
	const double available = cash + credit;
	const double capital_per_capacity = entry.capital_per_capacity;
	if (capital_per_capacity <= 0.0) {
		return false;
	}
	const AiCityRow& city = view.city_rows[static_cast<std::size_t>(entry.city)];
	const std::string& city_nation = city.nation;
	const std::string& origin = group.origin_nation;
	const std::string& group_sector = group.sector;
	const std::string& resource = entry.resource;
	const bool foreign = city_nation != origin;
	const bool brain = ctx.ws.ai_brain.enabled;
	double margin = entry.margin;
	const double shortage = entry.shortage;
	const double desired = entry.desired;
	if (brain && ai_risk::loss_blocked(ctx.ws, ctx.ws.ai_brain, std::string(),
			group.id, entry.product)) {
		// R5: planta do MESMO grupo no bem e já na escada D-126 há 30 dias.
		ctx.state.brain_loss_blocked += 1;
		return false;
	}
	if (foreign) {
		// IDE só quando o mercado local justifica e a abertura permite.
		if (shortage <= 0.0 && desired < 1.0) {
			return false;
		}
		const FdiPolicyRow policy = fdi_policy(city_nation, group_sector);
		if (policy.mode == 3) {
			return false;
		}
		if (policy.mode == 1
				&& !is_incumbent(group.id, city_nation, resource)) {
			return false;
		}
	}
	double capacity = 0.0;
	double cost = 0.0;
	if (brain) {
		// D-131 fase C (R5) — paridade com o privado: a capacidade NÃO é mais
		// `caixa × 0,5 / capital`; o teto é a lacuna do escopo e o forecast
		// mensal fecha o caixa com o crédito de investimento do grupo
		// (`bank::credit_limit`, já com o aperto do banco). Sem lacuna não há
		// obra (a concorrência da R2 já entrou em `shortage`).
		if (shortage < 0.5) {
			return false;
		}
		const AiTemplate* tmpl = ctx.templates.by_resource(entry.resource);
		if (tmpl == nullptr) {
			return false;
		}
		const int recipe_index =
			entry.recipe.empty() ? -1 : ctx.core.recipe_index(entry.recipe);
		int input_product = tmpl->input;
		double input_ratio = tmpl->input_ratio;
		double jobs_per_capacity = tmpl->jobs_per_ton;
		double wage = tmpl->wage;
		double opex_per_ton = tmpl->opex_per_ton;
		if (recipe_index >= 0
				&& recipe_index < static_cast<int>(view.recipes.size())) {
			const Recipe& recipe =
				view.recipes[static_cast<std::size_t>(recipe_index)];
			if (recipe.input_count > 0) {
				input_product = recipe.inputs[0];
				input_ratio = recipe.input_qty[0] / recipe.yield;
				jobs_per_capacity = recipe.jobs_per_ton;
				wage = recipe.wage;
				opex_per_ton = recipe.opex_per_ton;
			}
		}
		// O preço esperado (R3) é o mesmo do privado, com a oferta/demanda do
		// escopo da R2; a margem só muda na parcela do preço.
		const double price_expected = ai_brain_expected_price(ctx, entry.city,
			entry.product, entry.price, entry.supply_future,
			entry.demand_future);
		margin = entry.margin + (price_expected - entry.price);
		if (margin <= 0.0) {
			return false;
		}
		ai_brain::ForecastInput in;
		in.cash = cash;
		in.credit_approved = credit;
		const Group* world_group = nullptr;
		for (const Group& row : ctx.ws.groups) {
			if (row.id == group.id) {
				world_group = &row;
				break;
			}
		}
		if (world_group != nullptr) {
			if (const Bank* bank = bank::lender(ctx.ws, *world_group)) {
				in.credit_approved *= bank_crisis::credit_factor(ctx.ws, *bank);
			}
		}
		in.candidate_capacity = std::clamp(shortage, 0.5, kMaxCapacity);
		in.unit_capex = capital_per_capacity;
		in.efficiency_class = eff::classify_new(ctx.ws,
			ctx.ws.cities[static_cast<std::size_t>(entry.city)],
			entry.product, recipe_index, input_product);
		in.from_efficiency = eff::curve(ctx.ws, in.efficiency_class).start;
		in.equipment_active = false;
		in.construction_days = ai_brain_construction_days(ctx, *tmpl,
			entry.is_food, ctx.templates.is_mineral(entry.resource),
			in.candidate_capacity);
		in.aptitude = 1.0;
		if (recipe_index < 0 && entry.product >= 0
				&& entry.product < static_cast<int>(city.suitability.size())) {
			in.aptitude = std::clamp(
				city.suitability[static_cast<std::size_t>(entry.product)],
				0.0, 1.0);
		}
		in.stage_factor = 1.0;
		in.area_scale = 1.0;
		in.jobs_per_capacity = jobs_per_capacity;
		in.wage = wage;
		in.opex_per_ton = opex_per_ton;
		in.input_ratio = input_ratio;
		in.input_price = input_product >= 0
			? ai_market(view, entry.city, input_product, kAiPrice) : 0.0;
		in.price_expected = price_expected;
		in.supply_future = entry.supply_future;
		in.demand_future = entry.demand_future;
		in.loan_rate_annual = bank::loan_rate(ctx.ws, city.nation);
		in.margin_per_ton = margin;
		in.expected_efficiency =
			ai_expected_new_efficiency(ctx, entry.city, *tmpl);
		in.payback_limit_days = tmpl->is_recipe ? 730.0
			: (ctx.templates.is_bulk_low_value(entry.resource) ? 730.0
				: 150.0);
		in.min_capacity = 0.5;
		ctx.state.brain_group_attempts += 1;
		const ai_brain::ForecastResult forecast =
			ai_brain::forecast_monthly(ctx.ws.ai_brain, ctx.ws, in);
		if (!forecast.pass || forecast.capacity < 0.5) {
			ctx.state.brain_group_rejected += 1;
			// D-131 fase C (diagnóstico): gate que reprovou + amostra.
			const int gate = forecast.failed_gate >= 0
					&& forecast.failed_gate <= 6
				? forecast.failed_gate : 0;
			ctx.state.brain_group_gate_rejected[static_cast<std::size_t>(gate)]
				+= 1;
			if (forecast.revenue_total <= 1e-9) {
				ctx.state.brain_group_zero_revenue += 1;
			}
			if (ctx.state.brain_group_samples.size() < 10
					&& forecast.failed_gate != 0) {
				bool seen_combo = false;
				for (const AiForecastSample& row :
						ctx.state.brain_group_samples) {
					if (row.product == entry.product
							&& row.gate == forecast.failed_gate) {
						seen_combo = true;
						break;
					}
				}
				if (!seen_combo) {
					AiForecastSample sample;
					sample.day = ctx.day;
					sample.city = entry.city;
					sample.product = entry.product;
					sample.gate = forecast.failed_gate;
					sample.cash = in.cash;
					sample.credit = in.credit_approved;
					sample.capex = forecast.capex;
					sample.revenue = forecast.revenue_total;
					sample.cost = forecast.cost_total;
					sample.payback = forecast.payback_days;
					sample.trough = forecast.trough_profit;
					sample.min_balance = forecast.min_balance;
					sample.final_balance = forecast.final_balance;
					sample.cap_max = forecast.cap_max;
					sample.capacity = forecast.capacity;
					sample.price = in.price_expected;
					sample.margin = in.margin_per_ton;
					ctx.state.brain_group_samples.push_back(sample);
				}
			}
			return false;
		}
		capacity = forecast.capacity;
		cost = capacity * capital_per_capacity;
	} else {
		capacity = std::clamp(available * kCashShare / capital_per_capacity,
			0.5, kMaxCapacity);
		cost = capacity * capital_per_capacity;
		if (cost > available) {
			capacity = available / capital_per_capacity;
			cost = capacity * capital_per_capacity;
		}
	}
	if (capacity < 0.5) {
		return false;
	}
	// Rampa de operação: a planta nova não entrega a capacidade cheia no dia 1
	// (D-125) — o retorno usa a média da curva, como o investidor privado.
	const double ramp = entry.ramp;
	const double profit_day = margin * capacity * ramp;
	if (profit_day <= 0.0) {
		return false;
	}
	double risk = 1.0;
	std::string partner;
	double local_share = 0.0;
	if (foreign) {
		const FdiPolicyRow policy = fdi_policy(city_nation, group_sector);
		const double tariff = tariff_rate(city_nation, origin, entry.product);
		// IDE evita a tarifa: quanto maior a tarifa, mais o investimento local
		// compensa (o risco de mercado cresce, mas o prêmio também).
		risk *= 1.0 + std::max(0.0, 0.4 - tariff) * 2.0;
		risk *= 1.0 + (1.0 - nation_confidence(city_nation)) * 2.0;
		if (policy.mode == 2) {
			local_share = policy.min_local;
			if (local_share <= 0.0) {
				local_share = 0.3;
			}
			partner = partner_for(group, city_nation, cost * local_share);
			if (partner.empty()) {
				return false;
			}
			risk *= 1.0 + local_share;
		}
	}
	const double score = profit_day / std::max(cost, 1.0) / risk
		* ogs::stock::investment_factor(ctx.ws, group.id);
	// D-131 fase B (R4): tetos de exposição do grupo e anti-manada global.
	if (ctx.ws.ai_brain.enabled) {
		const int sector = ai_risk::sector_slot(ctx.state.brain_exposure,
			entry.product);
		const AiExposure* exposure = ai_risk::group_exposure(
			ctx.state.brain_exposure, group.id);
		AiExposure empty;
		if (exposure == nullptr) {
			empty.by_product.assign(static_cast<std::size_t>(
				ctx.state.brain_exposure.products), 0.0);
			empty.by_sector.assign(static_cast<std::size_t>(
				ctx.state.brain_exposure.sectors), 0.0);
			exposure = &empty;
		}
		if (!ai_risk::exposure_allows(ctx.ws.ai_brain, *exposure,
				entry.product, sector, cost, cash + credit)) {
			ctx.state.brain_risk_rejected += 1;
			ctx.state.brain_exposure_rejected += 1;
			return false;
		}
		const double existing_capacity = entry.product >= 0
			&& entry.product < static_cast<int>(
				ctx.state.brain_supply.world_capacity.size())
			? ctx.state.brain_supply.world_capacity[
				static_cast<std::size_t>(entry.product)]
			: 0.0;
		if (!ai_risk::herd_allows(ctx.state, ctx.ws.ai_brain, entry.product,
				entry.is_food, ctx.day, capacity, existing_capacity)) {
			ctx.state.brain_risk_rejected += 1;
			ctx.state.brain_herd_rejected += 1;
			return false;
		}
	}
	out = Opportunity();
	out.valid = true;
	out.city = entry.city;
	out.resource = resource;
	out.product = entry.product;
	out.recipe = entry.recipe;
	out.capacity = capacity;
	out.cost = cost;
	out.margin = margin;
	out.score = score;
	out.foreign = foreign;
	out.partner = std::move(partner);
	out.local_share = local_share;
	return true;
}

void GroupDay::execute(int group_index, const Opportunity& opportunity) {
	const AiView& view = ctx.view;
	const AiGroupRow& group = view.groups[static_cast<std::size_t>(group_index)];
	const std::string& city_id =
		view.city_rows[static_cast<std::size_t>(opportunity.city)].id;
	// `"GRP-" + id sem '-' + "-" + dia` (== `String(group["id"]).replace`).
	std::string group_tag;
	group_tag.reserve(group.id.size());
	for (const char letter : group.id) {
		if (letter != '-') {
			group_tag.push_back(letter);
		}
	}
	const std::string building_id =
		"GRP-" + group_tag + "-" + std::to_string(ctx.day);
	// C3 (D-107): se o capex passar do caixa local, o grupo toma crédito do
	// banco agregado ANTES da obra (o pedido é um comando A3 registrado no
	// log). O limite do dia já entrou em `group_credit` na visão do dia.
	int cash_slot = -1;
	for (int index = 0; index < static_cast<int>(view.groups.size());
			++index) {
		if (view.groups[static_cast<std::size_t>(index)].id == group.id) {
			if (index < static_cast<int>(ctx.state.group_cash.size())) {
				cash_slot = index;
			}
			break;
		}
	}
	if (cash_slot >= 0) {
		const double gap = opportunity.cost
			- ctx.state.group_cash[static_cast<std::size_t>(cash_slot)];
		if (gap > 1e-9) {
			const AiActionResult borrow = ai_emit_group_borrow(ctx, group.id,
				gap);
			if (borrow.cost > 0.0) {
				ctx.state.group_cash[static_cast<std::size_t>(cash_slot)] +=
					borrow.cost;
			}
		}
	}
	const AiActionResult result = ai_emit_group_invest(ctx, group.id, city_id,
		building_id, opportunity.resource, opportunity.product,
		opportunity.capacity, opportunity.recipe, opportunity.partner);
	if (!result.created) {
		return;
	}
	ctx.state.brain_group_created += 1;
	// Mantém o caixa local em dia para as próximas decisões do mesmo dia.
	for (int index = 0; index < static_cast<int>(view.groups.size()); ++index) {
		if (view.groups[static_cast<std::size_t>(index)].id == group.id) {
			if (index < static_cast<int>(ctx.state.group_cash.size())) {
				ctx.state.group_cash[static_cast<std::size_t>(index)] -=
					result.cost;
			}
			break;
		}
	}
}

void GroupDay::load_signals() {
	policy_cache.clear();
	tariff_cache.clear();
	// `_measure_pending_global`: capacidade futura em obras por produto, somada
	// em TODAS as cidades (ordem da tabela plana `c_*`).
	const AiView& view = ctx.view;
	pending_global.assign(static_cast<std::size_t>(std::max(view.products, 0)),
		0.0);
	for (std::size_t index = 0; index < view.c_output.size(); ++index) {
		const int product = view.c_output[index];
		if (product >= 0 && product < view.products) {
			pending_global[static_cast<std::size_t>(product)] +=
				view.c_capacity[index];
		}
	}
	// Incumbentes: índice montado sob demanda no primeiro candidato IDE.
	incumbents.clear();
	incumbents_ready = false;
}

bool GroupDay::compute_food_secure(const AiView& view) const {
	// Satisfação alimentar média do mundo (== `light.p_satisfaction`): sem
	// cidade (status vazio) o mundo é considerado seguro.
	double total = 0.0;
	int count = 0;
	for (int city_index = 0; city_index < view.cities; ++city_index) {
		total += ai_pop(view, city_index, kAiSatisfaction);
		++count;
	}
	if (count == 0) {
		return true;
	}
	return total / static_cast<double>(count) >= kFoodSecureSatisfaction;
}

FdiPolicyRow GroupDay::fdi_policy(const std::string& nation,
		const std::string& sector) {
	const std::string key = nation + "|" + sector;
	const auto cached = policy_cache.find(key);
	if (cached != policy_cache.end()) {
		return cached->second;
	}
	// Primeira linha de `ws.fdi_policies` com nação e setor vence (ordem do
	// vetor, a mesma do status `get_investment_signals`).
	FdiPolicyRow found;
	for (const FdiPolicy& entry : ctx.ws.fdi_policies) {
		if (entry.nation == nation && entry.sector == sector) {
			found.mode = entry.mode;
			found.min_local = entry.min_local;
			break;
		}
	}
	policy_cache.emplace(key, found);
	return found;
}

double GroupDay::nation_confidence(const std::string& nation) const {
	const auto found = ctx.ws.confidence.find(nation);
	return found == ctx.ws.confidence.end() ? 1.0 : found->second;
}

double GroupDay::tariff_rate(const std::string& nation,
		const std::string& origin, int product) {
	const std::string key =
		nation + "|" + origin + "|" + std::to_string(product);
	const auto cached = tariff_cache.find(key);
	if (cached != tariff_cache.end()) {
		return cached->second;
	}
	const double value = ctx.core.tariff_rate_text(nation, origin, product);
	tariff_cache.emplace(key, value);
	return value;
}

bool GroupDay::is_incumbent(const std::string& group_id,
		const std::string& nation, const std::string& resource) {
	const AiView& view = ctx.view;
	if (!incumbents_ready) {
		incumbents_ready = true;
		for (std::size_t index = 0; index < view.i_city.size(); ++index) {
			const int city_index = view.i_city[index];
			std::string city_nation;
			if (city_index >= 0 && city_index < view.cities) {
				city_nation = view.city_rows[
					static_cast<std::size_t>(city_index)].nation;
			}
			const std::string key =
				city_nation + "|" + view.i_resource[index];
			incumbents[key].insert(view.i_owner_group[index]);
		}
	}
	const auto found = incumbents.find(nation + "|" + resource);
	if (found == incumbents.end()) {
		return false;
	}
	return found->second.find(group_id) != found->second.end();
}

std::string GroupDay::partner_for(const AiGroupRow& group,
		const std::string& nation, double required) const {
	const AiView& view = ctx.view;
	for (int index = 0; index < static_cast<int>(view.groups.size()); ++index) {
		const AiGroupRow& candidate =
			view.groups[static_cast<std::size_t>(index)];
		if (candidate.id == group.id) {
			continue;
		}
		if (candidate.origin_nation != nation) {
			continue;
		}
		if (candidate.state_owned) {
			continue;
		}
		const double cash = index < static_cast<int>(ctx.state.group_cash.size())
			? ctx.state.group_cash[static_cast<std::size_t>(index)] : 0.0;
		if (cash >= required) {
			return candidate.id;
		}
	}
	return std::string();
}

} // namespace

void AiGroupInvestor::run(AiContext& ctx) {
	GroupDay day(ctx);
	day.run();
}

} // namespace ogs
