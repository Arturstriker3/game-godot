// G — agricultura sazonal (D-096): plantio/fertilizante, colheita na janela,
// perda de armazenagem e perecibilidade. Ver docs/economy/agriculture_seasonal.md.
//
// O QUE É DADO (AgricultureService -> WorldState, bloco `definitions`):
//   * área colhida (ha) e rendimento (t/ha) por país/cultura (FAOSTAT QCL);
//   * janelas de plantio/colheita por país/cultura (FAO/JRC/USDA NASS/AH-664);
//   * perda pós-colheita e fatia de armazenagem (APHLIS / FAO 2011);
//   * fertilizante N por cultura (IFA FUBC);
//   * estoque de fim de safra e consumo (USDA PSD) -> estoque inicial;
//   * dias de prateleira dos 12 bens da cadeia (FoodKeeper / FAO INPhO).
//
// O QUE É MODELO DECLARADO (constantes em sim_constants.h, calibráveis):
//   * quanto da perda total incide na entrada (kAgriEntryLossShare);
//   * agravamento da perda por armazém ruim (kAgriStorageLossAggravation);
//   * meia-vida do estoque = prazo de prateleira (kAgriShelfHalfLife);
//   * conversão N -> toneladas do bem `fertilizer` (kAgriFertilizerNContent);
//   * piso de rendimento sem fertilizante (kAgriNoFertilizerYieldFloor);
//   * qualidade do armazém derivada da infra B5 quando não declarada.
//
// REGRA DURA: nenhum preço é formado aqui. A sazonalidade entra pelo VOLUME
// (oferta concentrada na colheita, estoque drenando na entressafra); o preço
// livre é o da D-091/D-092 (prices.cpp).

#include "agriculture.h"

#include "profiling.h"
#include "sim_constants.h"
#include "sim_core.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace ogs {

namespace {

constexpr int kMonthDays[12] = {
	31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

bool is_leap_year(int year) {
	return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int month_length(int year, int month) {
	if (month == 2 && is_leap_year(year)) {
		return 29;
	}
	return kMonthDays[std::clamp(month, 1, 12) - 1];
}

// Primeiro dia do ano (doy) de um mês, no ano civil dado (bissexto conta).
int first_doy_of_month(int year, int month) {
	int doy = 1;
	for (int m = 1; m < month; ++m) {
		doy += month_length(year, m);
	}
	return doy;
}

} // namespace

int game_year_days(int year) {
	return is_leap_year(year) ? 366 : 365;
}

GameDate game_date_from_day(int day) {
	GameDate date;
	int remaining = std::max(0, day);
	int year = kAgriStartYear;
	for (;;) {
		const int length = game_year_days(year);
		if (remaining < length) {
			break;
		}
		remaining -= length;
		++year;
	}
	date.year = year;
	date.doy = remaining + 1;
	int month = 1;
	int left = remaining + 1;
	while (month < 12 && left > month_length(year, month)) {
		left -= month_length(year, month);
		++month;
	}
	date.month = month;
	date.day = left;
	return date;
}

bool doy_in_window(int doy, int start, int end) {
	if (start <= 0 || end <= 0) {
		return false;
	}
	if (start <= end) {
		return doy >= start && doy <= end;
	}
	return doy >= start || doy <= end;
}

int window_length(int start, int end) {
	if (start <= 0 || end <= 0) {
		return 0;
	}
	if (start <= end) {
		return end - start + 1;
	}
	// Atravessa o ano: conta no calendário de 365 dias (o bissexto só desloca
	// a posição em um dia; a fração entregue continua monotônica).
	return (365 - start + 1) + end;
}

int window_position(int doy, int start, int end) {
	if (!doy_in_window(doy, start, end)) {
		return -1;
	}
	if (start <= end) {
		return doy - start;
	}
	if (doy >= start) {
		return doy - start;
	}
	return (365 - start + 1) + (doy - 1);
}

int window_start_year(int year, int doy, int start) {
	if (start <= 0) {
		return year;
	}
	return doy >= start ? year : year - 1;
}

// ---------------------------------------------------------------------------
// API de declaração
// ---------------------------------------------------------------------------

void SimCore::set_crop_plan(const std::string& city_id, int product,
		double area_ha, double yield_t_ha,
		const std::vector<CropSeasonPlan>& seasons) {
	if (city_id.empty() || product < 0 || product >= ws_.products) {
		return;
	}
	if (!(area_ha > 0.0) || !(yield_t_ha > 0.0) || seasons.empty()) {
		return;
	}
	const int count = std::min(static_cast<int>(seasons.size()),
		kMaxCropSeasons);
	const std::string key = city_id + "|" + std::to_string(product);
	CropPlan plan;
	plan.city = city_id;
	plan.product = product;
	plan.area_ha = area_ha;
	plan.yield_t_ha = yield_t_ha;
	plan.season_count = 0;
	for (int index = 0; index < count; ++index) {
		const CropSeasonPlan& in = seasons[static_cast<std::size_t>(index)];
		// Janela inválida (mês fora de 1..12 convertido para 0) é descartada:
		// a safra sem os dois extremos não entrega nada.
		if (in.harvest_start_doy <= 0 || in.harvest_end_doy <= 0) {
			continue;
		}
		plan.seasons[plan.season_count] = in;
		++plan.season_count;
	}
	if (plan.season_count == 0) {
		return;
	}
	const auto found = ws_.crop_plan_lookup.find(key);
	if (found != ws_.crop_plan_lookup.end()) {
		// Substitui a DEFINIÇÃO preservando o ESTADO já registrado (o service
		// redeclara o plano no load do jogo; a safra em curso não pode ser
		// reentregue).
		CropPlan& target = ws_.crop_plans[static_cast<std::size_t>(found->second)];
		const int keep = std::min(target.season_count, plan.season_count);
		for (int index = 0; index < keep; ++index) {
			plan.state[index] = target.state[index];
		}
		target = plan;
	} else {
		ws_.crop_plan_lookup.emplace(key,
			static_cast<int>(ws_.crop_plans.size()));
		ws_.crop_plans.push_back(plan);
	}
	ws_.crop_index_ready = false;
}

void SimCore::set_crop_initial_stock(const std::string& city_id, int product,
		double tons) {
	if (product < 0 || product >= ws_.products || tons < 0.0) {
		return;
	}
	City* city = find_city(city_id);
	if (city == nullptr) {
		return;
	}
	city->markets[product].stock = tons;
}

void SimCore::set_crop_loss(int product, double loss_pct,
		double storage_share_pct) {
	if (product < 0 || product >= ws_.products) {
		return;
	}
	const std::size_t size = static_cast<std::size_t>(ws_.products);
	if (ws_.crop_loss_pct.size() < size) {
		ws_.crop_loss_pct.resize(size, 0.0);
		ws_.crop_storage_share_pct.resize(size, -1.0);
	}
	ws_.crop_loss_pct[static_cast<std::size_t>(product)] =
		std::clamp(loss_pct, 0.0, 100.0);
	ws_.crop_storage_share_pct[static_cast<std::size_t>(product)] =
		storage_share_pct < 0.0 ? -1.0 : std::clamp(storage_share_pct, 0.0, 100.0);
}

void SimCore::set_shelf_life(int product, int days) {
	if (product < 0 || product >= ws_.products) {
		return;
	}
	const std::size_t size = static_cast<std::size_t>(ws_.products);
	if (ws_.crop_shelf_days.size() < size) {
		ws_.crop_shelf_days.resize(size, 0);
		ws_.crop_shelf_daily.resize(size, 0.0);
	}
	ws_.crop_shelf_days[static_cast<std::size_t>(product)] =
		std::max(0, days);
	// Fração diária de perda com meia-vida = prazo de prateleira (regra
	// declarada em sim_constants.h). Pré-calculada: o tick não paga pow.
	ws_.crop_shelf_daily[static_cast<std::size_t>(product)] = days > 0
		? 1.0 - std::pow(kAgriShelfHalfLife, 1.0 / static_cast<double>(days))
		: 0.0;
	if (std::find(ws_.agri_chain.begin(), ws_.agri_chain.end(), product)
			== ws_.agri_chain.end()) {
		ws_.agri_chain.push_back(product);
	}
}

void SimCore::set_fertilizer_n(const std::string& city_id, int product,
		double n_kg_ha) {
	if (product < 0 || product >= ws_.products || n_kg_ha < 0.0) {
		return;
	}
	const std::string key = city_id + "|" + std::to_string(product);
	const auto found = ws_.crop_plan_lookup.find(key);
	if (found == ws_.crop_plan_lookup.end()) {
		// Sem plano declarado não há safra para fertilizar: ignorado (o
		// service declara o plano antes do N).
		return;
	}
	ws_.crop_plans[static_cast<std::size_t>(found->second)].n_kg_ha =
		std::min(n_kg_ha, 2000.0);
}

void SimCore::set_storage_quality(const std::string& city_id, double quality) {
	City* city = find_city(city_id);
	if (city == nullptr) {
		return;
	}
	city->storage_quality = std::clamp(quality, 0.0, 1.0);
}

void SimCore::rebuild_crop_indexes() {
	ws_.crop_plan_lookup.clear();
	for (std::size_t index = 0; index < ws_.crop_plans.size(); ++index) {
		CropPlan& plan = ws_.crop_plans[index];
		plan.city_index = -1;
		const auto found = ws_.city_lookup.find(plan.city);
		if (found != ws_.city_lookup.end()) {
			plan.city_index = found->second;
		}
		ws_.crop_plan_lookup[plan.city + "|" + std::to_string(plan.product)] =
			static_cast<int>(index);
	}
	ws_.fertilizer_product = product_index("fertilizer");
	const std::size_t cities = ws_.cities.size();
	const std::size_t products = static_cast<std::size_t>(
		std::max(0, ws_.products));
	ws_.crop_planned.assign(cities * products, 0);
	for (City& city : ws_.cities) {
		city.agri_seasonal_daily = 0.0;
	}
	for (const CropPlan& plan : ws_.crop_plans) {
		if (plan.city_index < 0 || plan.product < 0
				|| plan.product >= ws_.products) {
			continue;
		}
		ws_.crop_planned[static_cast<std::size_t>(plan.city_index) * products
			+ static_cast<std::size_t>(plan.product)] = 1;
		ws_.cities[static_cast<std::size_t>(plan.city_index)]
			.agri_seasonal_daily += plan.area_ha * plan.yield_t_ha / 365.0;
	}
	ws_.crop_index_ready = true;
}

bool SimCore::crop_planned_at(std::size_t city_index, int product) const {
	if (!ws_.crop_index_ready || product < 0 || product >= ws_.products) {
		return false;
	}
	const std::size_t products = static_cast<std::size_t>(ws_.products);
	const std::size_t at = city_index * products
		+ static_cast<std::size_t>(product);
	if (at >= ws_.crop_planned.size()) {
		return false;
	}
	return ws_.crop_planned[at] != 0;
}

double SimCore::city_storage_quality(const City& city) {
	if (city.storage_quality >= 0.0) {
		return std::clamp(city.storage_quality, 0.0, 1.0);
	}
	// Derivado da infra B5 do estado: cobertura ponderada (estrada/energia/
	// água) normalizada pelo nível máximo, escalada entre o piso e 1,0.
	const double levels[3] = {
		static_cast<double>(city.region.infra_road),
		static_cast<double>(city.region.infra_power),
		static_cast<double>(city.region.infra_water)};
	double coverage = 0.0;
	for (int type = 0; type < 3; ++type) {
		coverage += kAgriStorageQualityWeight[type]
			* std::clamp(levels[type] / kInfraMaxLevel, 0.0, 1.0);
	}
	return std::clamp(kAgriStorageQualityFloor
		+ kAgriStorageQualityGain * coverage, 0.0, 1.0);
}

// ---------------------------------------------------------------------------
// Fase
// ---------------------------------------------------------------------------

namespace {

// Fração de perda agravada pela qualidade do armazém (0..kAgriLossCap).
double aggravated_loss(double fraction, double quality) {
	const double factor = 1.0 + kAgriStorageLossAggravation
		* (1.0 - std::clamp(quality, 0.0, 1.0));
	return std::clamp(fraction * factor, 0.0, kAgriLossCap);
}

} // namespace

void SimCore::phase_agriculture() {
	ws_.agri_delivered_today = 0.0;
	ws_.agri_harvests_today = 0;
	if (ws_.crop_plans.empty() && ws_.agri_chain.empty()) {
		return;
	}
	if (!ws_.crop_index_ready) {
		rebuild_crop_indexes();
	}
	for (CropPlan& plan : ws_.crop_plans) {
		plan.delivered_today_t = 0.0;
	}
	const GameDate today = game_date_from_day(ws_.day);
	const int month_key = today.year * 100 + today.month;
	const bool month_changed = ws_.agri_month_key != month_key;
	const bool apply_monthly = month_changed && ws_.day > 1;
	ws_.agri_month_key = month_key;

	const std::size_t cities = ws_.cities.size();
	const std::size_t chain = ws_.agri_chain.size();
	const std::size_t product_count = static_cast<std::size_t>(
		std::max(0, ws_.products));

	// 1. Perecibilidade (diária) e perda de armazenagem (mensal) por cidade.
	//    A perda de armazenagem só roda quando o mês vira: a primeira aplicação
	//    acontece no dia 2 (não há mês de estoque no dia 1).
	for (std::size_t city_index = 0; city_index < cities; ++city_index) {
		City& city = ws_.cities[city_index];
		const double quality = city_storage_quality(city);
		if (month_changed) {
			for (std::size_t slot = 0; slot < chain; ++slot) {
				city.agri_loss_perishable[slot] = 0.0;
				city.agri_loss_storage[slot] = 0.0;
			}
		}
		for (std::size_t slot = 0; slot < chain; ++slot) {
			const int product = ws_.agri_chain[slot];
			if (product < 0 || product >= ws_.products) {
				continue;
			}
			Market& market = city.markets[product];
			if (market.stock <= 0.0) {
				continue;
			}
			// Reduzir o estoque não pode deixar o mercado negativo.
			if (static_cast<std::size_t>(product) < ws_.crop_shelf_daily.size()) {
				const double daily = ws_.crop_shelf_daily[
					static_cast<std::size_t>(product)];
				if (daily > 0.0 && market.stock > 0.0) {
					const double loss = market.stock
						* std::clamp(daily, 0.0, kAgriLossCap);
					market.stock = std::max(0.0, market.stock - loss);
					city.agri_loss_perishable[slot] += loss;
					ws_.agri_perishable_total[product] += loss;
				}
			}
			if (!apply_monthly) {
				continue;
			}
			if (static_cast<std::size_t>(product)
					>= ws_.crop_loss_pct.size()) {
				continue;
			}
			const double total_pct =
				ws_.crop_loss_pct[static_cast<std::size_t>(product)];
			const double storage_pct = ws_.crop_storage_share_pct[
				static_cast<std::size_t>(product)];
			if (total_pct <= 0.0) {
				continue;
			}
			const double annual = (storage_pct >= 0.0 ? storage_pct
				: total_pct * (1.0 - kAgriEntryLossShare)) / 100.0;
			const double monthly = aggravated_loss(annual / 12.0, quality);
			const double loss = market.stock * monthly;
			market.stock = std::max(0.0, market.stock - loss);
			city.agri_loss_storage[slot] += loss;
			ws_.agri_storage_total[product] += loss;
		}
	}

	if (ws_.crop_plans.empty()) {
		OGS_SIM_PHASE_LAP(kPhaseAgriculture);
		return;
	}

	// 2. Plantio: fertilizante cobrado UMA vez por safra/ano, no início da
	//    janela. Sem estoque do bem no mercado da cidade o rendimento da safra
	//    cai (piso de solo natural, declarado) — o plantio não zera a safra.
	const int fert = ws_.fertilizer_product;
	for (CropPlan& plan : ws_.crop_plans) {
		if (plan.city_index < 0) {
			continue;
		}
		City& city = ws_.cities[static_cast<std::size_t>(plan.city_index)];
		const double annual_budget = plan.area_ha * plan.yield_t_ha;
		if (annual_budget <= 0.0) {
			continue;
		}
		for (int index = 0; index < plan.season_count; ++index) {
			const CropSeasonPlan& season = plan.seasons[index];
			if (!doy_in_window(today.doy, season.plant_start_doy,
					season.plant_end_doy)) {
				continue;
			}
			CropSeasonState& state = plan.state[index];
			const int key = window_start_year(today.year, today.doy,
				season.plant_start_doy);
			if (state.planted_year == key) {
				continue;
			}
			state.planted_year = key;
			state.yield_scale = 1.0;
			state.fertilizer_t = 0.0;
			const double need_t = plan.n_kg_ha * plan.area_ha
				* std::max(0.0, season.share)
				/ (kAgriFertilizerNContent * 1000.0);
			if (need_t > 0.0 && fert >= 0 && fert < ws_.products) {
				Market& market = city.markets[fert];
				const double bought = std::min(need_t,
					std::max(0.0, market.stock));
				if (bought > 0.0) {
					const double price = market.price > 0.0
						? market.price : base_price(fert);
					market.stock -= bought;
					market.demand += bought;
					market.sold += bought;
					city.revenue_by_product[fert] += bought * price;
					// Custo do insumo atribuído às fazendas da cultura na
					// cidade, na proporção da capacidade (mesmo critério da
					// contabilidade para a receita do produto).
					double total_capacity = 0.0;
					for (const int bi : ws_.buildings_by_city[
							static_cast<std::size_t>(plan.city_index)]) {
						const Building& building =
							ws_.buildings[static_cast<std::size_t>(bi)];
						if (building.output_product == plan.product
								&& building.recipe < 0
								&& building.input_product < 0) {
							total_capacity += building.capacity
								* building.output_scale;
						}
					}
					if (total_capacity > 0.0) {
						for (const int bi : ws_.buildings_by_city[
								static_cast<std::size_t>(plan.city_index)]) {
							Building& building =
								ws_.buildings[static_cast<std::size_t>(bi)];
							if (building.output_product != plan.product
									|| building.recipe >= 0
									|| building.input_product >= 0) {
								continue;
							}
							building.input_value += bought * price
								* (building.capacity * building.output_scale
									/ total_capacity);
						}
					}
				}
				const double ratio = need_t > 0.0
					? std::clamp(bought / need_t, 0.0, 1.0) : 1.0;
				// Piso de solo natural: sem N comprado a safra ainda rende a
				// fração declarada (agricultura de sequeiro); o insumo leva ao
				// rendimento cheio.
				state.yield_scale = kAgriNoFertilizerYieldFloor
					+ (1.0 - kAgriNoFertilizerYieldFloor) * ratio;
				state.fertilizer_t = bought;
			}
		}
	}

	// 3. Colheita: a safra chega CONCENTRADA dentro da janela, em parcelas
	//    iguais por dia, e ZERO fora dela. O acumulado por safra/ano torna a
	//    entrega idempotente (recarregar o mesmo dia não entrega de novo).
	for (CropPlan& plan : ws_.crop_plans) {
		if (plan.city_index < 0) {
			continue;
		}
		City& city = ws_.cities[static_cast<std::size_t>(plan.city_index)];
		const double quality = city_storage_quality(city);
		const double total_pct = static_cast<std::size_t>(plan.product)
			< ws_.crop_loss_pct.size()
			? ws_.crop_loss_pct[static_cast<std::size_t>(plan.product)] : 0.0;
		const double entry_loss = aggravated_loss(
			total_pct / 100.0 * kAgriEntryLossShare, quality);
		const double annual_budget = plan.area_ha * plan.yield_t_ha;
		for (int index = 0; index < plan.season_count; ++index) {
			const CropSeasonPlan& season = plan.seasons[index];
			const int position = window_position(today.doy,
				season.harvest_start_doy, season.harvest_end_doy);
			if (position < 0) {
				continue;
			}
			CropSeasonState& state = plan.state[index];
			const int key = window_start_year(today.year, today.doy,
				season.harvest_start_doy);
			const int length = window_length(season.harvest_start_doy,
				season.harvest_end_doy);
			if (length <= 0) {
				continue;
			}
			const double fraction = static_cast<double>(position + 1)
				/ static_cast<double>(length);
			if (state.harvest_year != key) {
				state.harvest_year = key;
				state.delivered_t = 0.0;
				// Congela o rendimento da safra: o plantio do CICLO SEGUINTE não
				// reescreve o orçamento desta colheita.
				state.harvest_yield_scale = state.yield_scale;
				// Primeira vez que vemos esta instância de colheita JÁ EM CURSO
				// (início do mundo em janeiro, ou load de save sem o estado da
				// safra): o que já passou da janela foi colhido antes do nosso
				// relógio e está no estoque. Marca como entregue para não somar
				// a mesma tonelada duas vezes (nada de lote retroativo).
				if (position > 0) {
					state.delivered_t = annual_budget
						* std::max(0.0, season.share)
						* state.harvest_yield_scale * fraction;
					continue;
				}
			}
			const double season_budget = annual_budget
				* std::max(0.0, season.share) * state.harvest_yield_scale;
			const double target = season_budget * fraction;
			const double gross = target - state.delivered_t;
			if (gross <= 1e-12) {
				continue;
			}
			state.delivered_t = target;
			const double net = gross * (1.0 - entry_loss);
			const double lost = gross - net;
			Market& market = city.markets[plan.product];
			market.stock += net;
			market.supply += net;
			if (static_cast<std::size_t>(plan.product) < product_count) {
				ws_.produced[plan.product] += net;
				ws_.produced_today[plan.product] += net;
			}
			ws_.agri_delivered_today += net;
			++ws_.agri_harvests_today;
			plan.delivered_today_t += net;
			// Livro de perdas do mês: a perda de ENTRADA (pós-colheita) fica no
			// estado da safra (`loss_month_t`), porque ela é do PLANO; o livro
			// da cidade guarda só armazenagem + perecibilidade (sem contar a
			// mesma tonelada duas vezes no status).
			const int loss_key = today.year * 100 + today.month;
			if (state.loss_month_key != loss_key) {
				state.loss_month_key = loss_key;
				state.loss_month_t = 0.0;
			}
			state.loss_month_t += lost;
			ws_.agri_entry_total[plan.product] += lost;
		}
	}

	OGS_SIM_PHASE_LAP(kPhaseAgriculture);
}

} // namespace ogs
