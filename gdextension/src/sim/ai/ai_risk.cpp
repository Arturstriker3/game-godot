// D-131 fase B — R2 (concorrência) e R4 (exposição/anti-manada).
// Ver ai_risk.h e docs/economy/D131_ai_brain.md. Regras puras: só o retrato do
// dia (`AiView`), os parâmetros declarados e o estado entre dias (`AiState`).
#include "ai_risk.h"

#include "ai_phase.h"
#include "../efficiency.h"
#include "../profiling.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace ogs {
namespace {

constexpr int kHerdWindowDays = 30;
constexpr double kEps = 1e-9;

// Chave estável do pending por (região, produto): a região vai deslocada em 32
// bits para não colidir entre produtos.
std::uint64_t pending_key(int region, int product) {
	return (static_cast<std::uint64_t>(region + 1) << 32)
		| static_cast<std::uint32_t>(product);
}

// Dono-grupo no formato do núcleo: `national:<g>` ou `foreign:<g>@<n>`.
bool owner_group_id(const std::string& owner, std::string* group_id) {
	if (owner.rfind("national:", 0) == 0) {
		*group_id = owner.substr(9);
		return !group_id->empty();
	}
	if (owner.rfind("foreign:", 0) == 0) {
		const std::string rest = owner.substr(8);
		const std::size_t at = rest.find('@');
		*group_id = at == std::string::npos ? rest : rest.substr(0, at);
		return !group_id->empty();
	}
	return false;
}

double mean_of(const std::vector<double>& values, std::size_t begin,
		std::size_t end) {
	if (begin >= end || end > values.size()) {
		return 0.0;
	}
	double total = 0.0;
	for (std::size_t index = begin; index < end; ++index) {
		total += values[index];
	}
	return total / static_cast<double>(end - begin);
}

// Inclinação relativa por dia da média móvel: (MA_recente − MA_anterior) /
// MA_anterior / janela. Menos de 5 observações = ausência declarada (0).
// Com histórico curto a janela usa metade do disponível (mínimo 2), a mesma
// convenção de `price_trend`.
double demand_growth(const std::vector<double>& history, int window_days) {
	const std::size_t count = history.size();
	if (count < 5) {
		return 0.0;
	}
	const std::size_t wanted =
		static_cast<std::size_t>(std::max(1, window_days));
	const std::size_t window = count >= 2 * wanted
		? wanted : std::max<std::size_t>(2, count / 2);
	if (window < 2 || count < 2 * window) {
		return 0.0;
	}
	const double current = mean_of(history, count - window, count);
	const double previous = mean_of(history, count - 2 * window,
		count - window);
	if (previous <= kEps) {
		return 0.0;
	}
	return (current - previous) / previous / static_cast<double>(window);
}

// Capital declarado por unidade de capacidade do produto (fallback do núcleo).
double capital_per_capacity(const AiTemplates& templates, int product) {
	const int index = templates.resource_index_for_product(product);
	if (index < 0 || index >= static_cast<int>(templates.all.size())) {
		return 2000.0;
	}
	return std::max(0.0,
		templates.all[static_cast<std::size_t>(index)].capital_per_capacity);
}

void exposure_resize(AiExposure& exposure, int products, int sectors) {
	if (static_cast<int>(exposure.by_product.size()) != products) {
		exposure.by_product.assign(static_cast<std::size_t>(products), 0.0);
	}
	if (static_cast<int>(exposure.by_sector.size()) != sectors) {
		exposure.by_sector.assign(static_cast<std::size_t>(sectors), 0.0);
	}
}

void exposure_add_value(AiExposure& exposure, int product, int sector,
		double capital) {
	if (capital <= 0.0) {
		return;
	}
	if (product >= 0
			&& product < static_cast<int>(exposure.by_product.size())) {
		double& slot = exposure.by_product[static_cast<std::size_t>(product)];
		if (slot <= 0.0) {
			exposure.goods += 1;
		}
		slot += capital;
	}
	if (sector >= 0
			&& sector < static_cast<int>(exposure.by_sector.size())) {
		exposure.by_sector[static_cast<std::size_t>(sector)] += capital;
	}
	exposure.total += capital;
}

} // namespace

namespace ai_risk {

void build_supply_cache(const WorldState& ws, const AiView& view,
		const AiTemplates& templates, const AiBrainParams& params,
		const std::vector<std::vector<double>>& demand_history,
		AiSupplyCache& out) {
	out = AiSupplyCache();
	out.products = std::max(0, view.products);
	out.world_supply.assign(static_cast<std::size_t>(out.products), 0.0);
	out.world_demand.assign(static_cast<std::size_t>(out.products), 0.0);
	out.world_growth.assign(static_cast<std::size_t>(out.products), 0.0);
	out.world_capacity.assign(static_cast<std::size_t>(out.products), 0.0);
	// Massa de terra → índice: tabela do núcleo primeiro; massas fora dela
	// ganham um id depois das declaradas (mesma regra do investidor privado).
	int regions = std::max(0, static_cast<int>(ws.landmass_table.size()));
	for (int index = 0; index < regions; ++index) {
		out.region_index.emplace(ws.landmass_table.at(index), index);
	}
	for (std::size_t index = 0; index < view.city_rows.size(); ++index) {
		const std::string& landmass = view.city_rows[index].landmass;
		if (ws.landmass_table.find(landmass) >= 0) {
			continue;
		}
		if (out.region_index.find(landmass) == out.region_index.end()) {
			out.region_index.emplace(landmass, regions);
			++regions;
		}
	}
	out.regions = regions;
	out.region_supply.assign(
		static_cast<std::size_t>(out.products * out.regions), 0.0);
	out.region_demand.assign(
		static_cast<std::size_t>(out.products * out.regions), 0.0);

	const auto region_of = [&ws, &out](const std::string& landmass) {
		const int declared = ws.landmass_table.find(landmass);
		if (declared >= 0) {
			return declared;
		}
		const auto extra = out.region_index.find(landmass);
		return extra == out.region_index.end() ? -1 : extra->second;
	};

	// Oferta existente + rampa por cidade (ordem do retrato).
	for (int city = 0; city < view.cities; ++city) {
		const int region = region_of(
			view.city_rows[static_cast<std::size_t>(city)].landmass);
		const int begin = ai_b_begin(view, city);
		const int end = ai_b_end(view, city);
		for (int slot = begin; slot < end; ++slot) {
			const int product = view.b_output[static_cast<std::size_t>(slot)];
			if (product < 0 || product >= out.products) {
				continue;
			}
			const double capacity =
				std::max(0.0, view.b_capacity[static_cast<std::size_t>(slot)]);
			const double utilization = std::clamp(
				view.b_utilization[static_cast<std::size_t>(slot)], 0.0, 1.0);
			const double site = std::clamp(
				view.b_site_suitability[static_cast<std::size_t>(slot)],
				0.0, 1.0);
			const double stage = std::max(0.0,
				view.b_stage_factor[static_cast<std::size_t>(slot)]);
			const double eff_now = std::clamp(
				view.b_production_efficiency[static_cast<std::size_t>(slot)],
				0.0, 1.0);
			const double eff_cap = std::clamp(
				view.b_eff_cap[static_cast<std::size_t>(slot)], 0.0, 1.0);
			// `utilization` publicada é produção/capacidade: já inclui aptidão,
			// eficiência e estágio (production.cpp). A rampa restante é a
			// metade da distância até o teto da classe — estimativa declarada
			// da média do resto da curva, sem simular dia a dia.
			const double existing = capacity * utilization;
			const double ramping = std::max(0.0, capacity * site * stage
				* (eff_cap - eff_now) * 0.5);
			out.world_supply[static_cast<std::size_t>(product)] +=
				existing + ramping;
			out.world_capacity[static_cast<std::size_t>(product)] += capacity;
			if (region >= 0 && region < out.regions) {
				out.region_supply[static_cast<std::size_t>(
					region * out.products + product)] += existing + ramping;
			}
		}
	}

	// Obras em andamento: capacidade × eficiência de entrega (início da curva
	// da classe do produto/receita). Expansão também entra (capacidade nova);
	// a rampa que ela herda do prédio existente fica no termo acima.
	for (std::size_t index = 0; index < view.c_output.size(); ++index) {
		const int product = view.c_output[index];
		if (product < 0 || product >= out.products) {
			continue;
		}
		// Cidade do projeto: a tabela plana segue `c_offset` (cidade → obra).
		int city = -1;
		for (int candidate = 0; candidate < view.cities; ++candidate) {
			if (ai_c_begin(view, candidate) <= static_cast<int>(index)
					&& static_cast<int>(index) < ai_c_end(view, candidate)) {
				city = candidate;
				break;
			}
		}
		if (city < 0) {
			continue;
		}
		const City& city_state =
			ws.cities[static_cast<std::size_t>(city)];
		const int recipe = index < view.c_recipe.size()
			? view.c_recipe[index] : -1;
		const EfficiencyClass cls = eff::classify_new(ws, city_state, product,
			recipe, -1);
		const double start = eff::curve(ws, cls).start;
		const double pending = std::max(0.0, view.c_capacity[index]) * start;
		out.world_supply[static_cast<std::size_t>(product)] += pending;
		const int region = region_of(
			view.city_rows[static_cast<std::size_t>(city)].landmass);
		if (region >= 0 && region < out.regions) {
			out.region_supply[static_cast<std::size_t>(
				region * out.products + product)] += pending;
		}
	}

	// Demanda do dia (mundo = `tot_desired`; região = soma dos desejados
	// positivos das cidades da massa) e crescimento da média móvel.
	out.world_demand = view.tot_desired;
	out.world_demand.resize(static_cast<std::size_t>(out.products), 0.0);
	for (int product = 0; product < out.products; ++product) {
		const std::vector<double>* history = product
			< static_cast<int>(demand_history.size())
			? &demand_history[static_cast<std::size_t>(product)] : nullptr;
		out.world_growth[static_cast<std::size_t>(product)] = history != nullptr
			? demand_growth(*history, params.demand_growth_days) : 0.0;
	}
	for (int city = 0; city < view.cities; ++city) {
		const int region = region_of(
			view.city_rows[static_cast<std::size_t>(city)].landmass);
		if (region < 0 || region >= out.regions) {
			continue;
		}
		for (int product = 0; product < out.products; ++product) {
			const double desired = ai_market(view, city, product, kAiDesired);
			if (desired > 0.0) {
				out.region_demand[static_cast<std::size_t>(
					region * out.products + product)] += desired;
			}
		}
	}
}

void build_exposure_cache(const WorldState& ws, const AiView& view,
		const AiTemplates& templates, AiExposureCache& out) {
	out = AiExposureCache();
	out.products = std::max(0, view.products);
	// Setores na ordem de primeira aparição do goods.json (determinística).
	for (int product = 0; product < out.products; ++product) {
		if (product >= static_cast<int>(ws.product_sectors.size())) {
			out.product_sector.push_back(-1);
			continue;
		}
		const std::string& sector =
			ws.product_sectors[static_cast<std::size_t>(product)];
		int slot = -1;
		for (std::size_t index = 0; index < out.sector_names.size(); ++index) {
			if (out.sector_names[index] == sector) {
				slot = static_cast<int>(index);
				break;
			}
		}
		if (slot < 0) {
			slot = static_cast<int>(out.sector_names.size());
			out.sector_names.push_back(sector);
		}
		out.product_sector.push_back(slot);
	}
	out.sectors = static_cast<int>(out.sector_names.size());
	out.city.assign(static_cast<std::size_t>(view.cities), AiExposure());
	for (int city = 0; city < view.cities; ++city) {
		exposure_resize(out.city[static_cast<std::size_t>(city)], out.products,
			out.sectors);
		out.city_index[view.city_rows[static_cast<std::size_t>(city)].id] =
			city;
	}
	for (int city = 0; city < view.cities; ++city) {
		const AiCityRow& row = view.city_rows[static_cast<std::size_t>(city)];
		const int begin = ai_b_begin(view, city);
		const int end = ai_b_end(view, city);
		for (int slot = begin; slot < end; ++slot) {
			const int product = view.b_output[static_cast<std::size_t>(slot)];
			if (product < 0 || product >= out.products) {
				continue;
			}
			const std::string& owner =
				view.b_owner[static_cast<std::size_t>(slot)];
			const double capital = std::max(0.0,
				view.b_capacity[static_cast<std::size_t>(slot)])
				* capital_per_capacity(templates, product);
			const int sector = out.product_sector[
				static_cast<std::size_t>(product)];
			std::string group_id;
			if (owner == "private") {
				exposure_add_value(out.city[static_cast<std::size_t>(city)],
					product, sector, capital);
			} else if (owner_group_id(owner, &group_id)) {
				AiExposure& exposure = out.groups[group_id];
				exposure_resize(exposure, out.products, out.sectors);
				exposure_add_value(exposure, product, sector, capital);
			}
		}
		(void)row;
	}
}

int region_slot(const AiSupplyCache& cache, const std::string& landmass) {
	const auto found = cache.region_index.find(landmass);
	return found == cache.region_index.end() ? -1 : found->second;
}

int sector_slot(const AiExposureCache& cache, int product) {
	if (product < 0
			|| product >= static_cast<int>(cache.product_sector.size())) {
		return -1;
	}
	return cache.product_sector[static_cast<std::size_t>(product)];
}

double supply_effective(const AiSupplyCache& cache, const AiState& state,
		int product, int region, bool region_scope) {
	if (product < 0 || product >= cache.products) {
		return 0.0;
	}
	double base = 0.0;
	if (region_scope && region >= 0 && region < cache.regions) {
		base = cache.region_supply[static_cast<std::size_t>(
			region * cache.products + product)];
		// R3/Q-014: na cesta alimentar só a obra da MESMA região desconta; a
		// obra em outra massa de terra não garante entrega aqui.
		const auto committed = state.pending_committed_region.find(
			pending_key(region, product));
		if (committed != state.pending_committed_region.end()) {
			base += committed->second;
		}
	} else {
		base = cache.world_supply[static_cast<std::size_t>(product)];
		// Escopo mundo: toda obra emitida na fase conta, inclusive as de região
		// não resolvida (elas só não entram no recorte regional acima).
		const auto committed = state.pending_committed.find(product);
		if (committed != state.pending_committed.end()) {
			base += committed->second;
		}
	}
	return base;
}

void pending_add(AiState& state, int region, int product, double capacity) {
	if (product < 0 || !(capacity > 0.0)) {
		return;
	}
	state.pending_committed[product] += capacity;
	if (region >= 0) {
		state.pending_committed_region[pending_key(region, product)] += capacity;
	}
}

double demand_future(const AiSupplyCache& cache, int product, int region,
		bool region_scope, int growth_days) {
	if (product < 0 || product >= cache.products) {
		return 0.0;
	}
	double demand = 0.0;
	if (region_scope && region >= 0 && region < cache.regions) {
		demand = cache.region_demand[static_cast<std::size_t>(
			region * cache.products + product)];
	} else {
		demand = cache.world_demand[static_cast<std::size_t>(product)];
	}
	const double growth =
		cache.world_growth[static_cast<std::size_t>(product)];
	return demand * (1.0 + growth * static_cast<double>(
		std::max(1, growth_days)));
}

const AiExposure* city_exposure(const AiExposureCache& cache, int city) {
	if (city < 0 || city >= static_cast<int>(cache.city.size())) {
		return nullptr;
	}
	return &cache.city[static_cast<std::size_t>(city)];
}

const AiExposure* group_exposure(const AiExposureCache& cache,
		const std::string& group_id) {
	const auto found = cache.groups.find(group_id);
	return found == cache.groups.end() ? nullptr : &found->second;
}

bool exposure_allows(const AiBrainParams& params, const AiExposure& exposure,
		int product, int sector, double cost, double cash) {
	if (cost <= 0.0) {
		return true;
	}
	const int products = static_cast<int>(exposure.by_product.size());
	const int sectors = static_cast<int>(exposure.by_sector.size());
	if (product < 0 || product >= products || sector < 0 || sector >= sectors) {
		// Sem catálogo de setor não há como medir o teto: não bloqueia por
		// ausência de dado (o teto por bem continua valendo).
		if (product < 0 || product >= products) {
			return true;
		}
	}
	const double base = std::max(0.0, cash) + exposure.total;
	const double good_after =
		exposure.by_product[static_cast<std::size_t>(product)] + cost;
	if (good_after > params.max_good_share * base + kEps) {
		return false;
	}
	if (sector >= 0 && sector < sectors) {
		const double sector_after =
			exposure.by_sector[static_cast<std::size_t>(sector)] + cost;
		if (sector_after > params.max_sector_share * base + kEps) {
			return false;
		}
	}
	if (exposure.by_product[static_cast<std::size_t>(product)] <= 0.0
			&& exposure.goods >= params.max_goods_per_agent) {
		return false;
	}
	return true;
}

bool herd_allows(const AiState& state, const AiBrainParams& params,
		int product, bool food, int day, double capacity,
		double existing_capacity) {
	const double multiplier = food
		? std::max(0.0, params.food_multiplier) : 1.0;
	const int limit = static_cast<int>(std::lround(
		static_cast<double>(std::max(0, params.max_new_plants_per_good_30d))
		* multiplier));
	int count = 0;
	double planned = 0.0;
	for (const AiHerdEvent& event : state.herd_events) {
		if (event.product != product) {
			continue;
		}
		const int age = day - event.day;
		if (age < 0 || age >= kHerdWindowDays) {
			continue;
		}
		++count;
		planned += std::max(0.0, event.capacity);
	}
	if (count >= limit) {
		return false;
	}
	const double capacity_limit = std::max(0.0, existing_capacity)
		* std::max(0.0, params.max_new_capacity_share_30d) * multiplier;
	// A primeira planta da janela é sempre permitida (um bem sem capacidade
	// existente nunca destravaria); o teto de capacidade vale das seguintes.
	if (count > 0 && capacity_limit > 0.0
			&& planned + std::max(0.0, capacity) > capacity_limit + kEps) {
		return false;
	}
	return true;
}

void herd_record(AiState& state, int product, double capacity, int day) {
	state.herd_events.erase(std::remove_if(state.herd_events.begin(),
		state.herd_events.end(), [day](const AiHerdEvent& event) {
			const int age = day - event.day;
			return age < 0 || age >= kHerdWindowDays;
		}), state.herd_events.end());
	AiHerdEvent event;
	event.day = day;
	event.product = product;
	event.capacity = std::max(0.0, capacity);
	state.herd_events.push_back(event);
}

bool loss_blocked(const WorldState& ws, const AiBrainParams& params,
		const std::string& city_id, const std::string& owner_id, int product) {
	if (!params.enabled || !ws.crisis.enabled || product < 0) {
		return false;
	}
	OGS_SIM_ZONE_ADD(kPhaseAiLossScan);
	const int limit = std::max(1, params.block_loss_days);
	for (const Building& building : ws.buildings) {
		if (building.output_product != product || building.bankrupt) {
			continue;
		}
		if (building.loss_days < limit) {
			continue;
		}
		if (owner_id.empty()) {
			// Privado: o caixa é o da cidade e o dono é `private`.
			if (building.owner == "private" && building.city == city_id) {
				return true;
			}
			continue;
		}
		std::string group_id;
		if (owner_group_id(building.owner, &group_id) && group_id == owner_id) {
			return true;
		}
	}
	return false;
}

void exposure_add(AiExposureCache& cache, const std::string& city_id,
		const std::string& owner, int product, double cost) {
	if (cost <= 0.0) {
		return;
	}
	const int sector = sector_slot(cache, product);
	std::string group_id;
	if (owner == "private") {
		const auto found = cache.city_index.find(city_id);
		if (found == cache.city_index.end()) {
			return;
		}
		AiExposure& exposure = cache.city[static_cast<std::size_t>(found->second)];
		exposure_resize(exposure, cache.products, cache.sectors);
		exposure_add_value(exposure, product, sector, cost);
		return;
	}
	if (owner_group_id(owner, &group_id)) {
		AiExposure& exposure = cache.groups[group_id];
		exposure_resize(exposure, cache.products, cache.sectors);
		exposure_add_value(exposure, product, sector, cost);
	}
}

} // namespace ai_risk
} // namespace ogs
