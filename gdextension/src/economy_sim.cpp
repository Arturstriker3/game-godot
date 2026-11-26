#include "economy_sim.h"
#include "edge/save_load.h"
#include "edge/value_bridge.h"
#include "sim/actions.h"
#include "sim/agriculture.h"
#include "sim/balance.h"
#include "sim/bank.h"
#include "sim/bootstrap.h"
#include "sim/bank_crisis.h"
#include "sim/central_bank.h"
#include "sim/crisis.h"
#include "sim/ai_brain.h"
#include "sim/efficiency.h"
#include "sim/exchange.h"
#include "sim/monetary.h"
#include "sim/power.h"
#include "sim/storage.h"
#include "sim/profiling.h"
#include "sim/save_definitions.h"
#include "sim/save_state.h"
#include "sim/sim_constants.h"
#include "sim/scenario_fixture.h"
#include "sim/sim_day.h"
#include "sim/stock.h"
#include "sim/urban_land.h"
#include "sim/housing.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_float64_array.hpp>
#include <godot_cpp/variant/string.hpp>

#if defined(OGS_PROFILING)
#include <chrono>
#endif
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <thread>
#include <utility>

namespace godot {

namespace {

// Hash da thread corrente: separa "a thread do dia" de "outra thread" em
// `dispatch_command` (A3). Barato e estÃ¡vel durante toda a sessÃ£o.
std::size_t current_thread_hash() {
	return std::hash<std::thread::id>{}(std::this_thread::get_id());
}

} // namespace

using namespace ogs;

namespace {

// SeguranÃ§a numÃ©rica: nada de NaN/Inf no estado nem nos outputs.
inline double safe_value(double v) {
	return std::isfinite(v) ? v : 0.0;
}
inline double safe_nonneg(double v) {
	return std::isfinite(v) ? std::max(0.0, v) : 0.0;
}
inline double safe_unit(double v) {
	return std::isfinite(v) ? std::clamp(v, 0.0, 1.0) : 0.0;
}

// Â§19: leitura defensiva de um sub-dicionÃ¡rio do JSON carregado pelo GDScript
// (chave ausente ou de outro tipo vira dicionÃ¡rio vazio â€” ausÃªncia explÃ­cita).
inline Dictionary dict_value(const Dictionary& parent, const String& key) {
	const Variant value = parent.get(key, Variant());
	return value.get_type() == Variant::DICTIONARY ? Dictionary(value) : Dictionary();
}

// B3: lÃª o bloco `region` de um dict de cidade (state_regions.json jÃ¡
// resolvido pelo GDScript) e preenche o struct Region. Devolve false quando o
// bloco estÃ¡ ausente/invÃ¡lido â€” regiÃ£o inerte (ausÃªncia explÃ­cita).
inline bool parse_region(const Dictionary& d, Region& region) {
	if (!d.has("population_region") && !d.has("urban_population")) {
		return false;
	}
	region.ready = true;
	region.population_region = safe_nonneg(
		static_cast<double>(d.get("population_region", 0.0)));
	const double urban = safe_nonneg(
		static_cast<double>(d.get("urban_population", 0.0)));
	// Urbano capado no total (20 cidades tÃªm urban > population_region: as 19
	// abaixo do urbano da D-005 + SGP-SIN, cuja regiÃ£o Ã© 0).
	region.urban_population = std::min(urban, region.population_region);
	// Invariante: interior = total - urbano (piso 0). O campo do JSON
	// (`region_population`) Ã© sÃ³ referÃªncia; recalculamos para o invariante
	// valer mesmo com dado inconsistente.
	region.region_pop = std::max(0.0,
		region.population_region - region.urban_population);
	const Variant us = d.get("urban_share", Variant());
	if (us.get_type() == Variant::FLOAT || us.get_type() == Variant::INT) {
		const double value = static_cast<double>(us);
		// -1 (ou negativo) = sem dado (mix neutro); sÃ³ valores >= 0 sÃ£o fraÃ§Ã£o.
		region.urban_share = value >= 0.0 ? std::clamp(value, 0.0, 1.0) : -1.0;
	} else {
		region.urban_share = -1.0;
	}
	region.name = std::string(String(d.get("name", "")).utf8().get_data());
	const Array states = d.get("states", Array());
	for (int index = 0; index < states.size(); ++index) {
		if (states[index].get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary entry = states[index];
		RegionState state;
		state.id = std::string(String(entry.get("id", "")).utf8().get_data());
		state.name = std::string(
			String(entry.get("name", "")).utf8().get_data());
		state.population = safe_nonneg(
			static_cast<double>(entry.get("population", 0.0)));
		state.area_km2 = safe_nonneg(
			static_cast<double>(entry.get("area_km2", 0.0)));
		region.states.push_back(std::move(state));
	}
	return true;
}

// Â§19: height_class do urban_land.json â†’ gabarito do jogo. Valor ausente ou
// desconhecido cai em baixa densidade (o save antigo nÃ£o tem o campo).
inline Density density_from_height_class(const String& height_class) {
	if (height_class == "high") {
		return DENSITY_HIGH;
	}
	if (height_class == "mid") {
		return DENSITY_MID;
	}
	return DENSITY_LOW;
}

// PopulaÃ§Ã£o de SIMULAÃ‡ÃƒO da cidade. Desde a calibraÃ§Ã£o de 2026, `population`
// no cenÃ¡rio Ã© a populaÃ§Ã£o real exibida (naÃ§Ã£o inteira rateada) e a aglomeraÃ§Ã£o
// da fonte viaja em `sim_population`. Sem o campo (save antigo, cenÃ¡rio
// sintÃ©tico), a populaÃ§Ã£o histÃ³rica Ã‰ a unidade de simulaÃ§Ã£o â€” comportamento
// anterior preservado.
inline double read_sim_population(const Dictionary& data, double population) {
	if (!data.has("sim_population")) {
		return population;
	}
	const double value = safe_nonneg(static_cast<double>(data["sim_population"]));
	return value > 0.0 ? value : population;
}

} // namespace

EconomySim::EconomySim() {
	// Produtos padrÃ£o (21 primÃ¡rios) e cesta alimentar validada. O autoload
	// sobrescreve ambos com goods.json/needs.json via set_goods/needs.
	ws_.products = 21;
	ws_.product_names.clear();
	ws_.product_names.reserve(21);
	static const char* const kDefaults[21] = {
		"wheat", "iron", "rice", "maize", "bauxite", "lead", "gold",
		"phosphate", "tungsten", "rare_earths", "sulfur", "diamond",
		"coal", "copper", "gas", "oil", "potato", "soy", "cotton", "wood", "uranium"};
	for (int index = 0; index < 21; ++index) {
		ws_.product_names.emplace_back(kDefaults[index]);
	}
	ws_.product_lookup.clear();
	for (int index = 0; index < ws_.products; ++index) {
		ws_.product_lookup[ws_.product_names[static_cast<std::size_t>(index)]] = index;
	}
	ws_.food_products.clear();
	ws_.food_factors.clear();
	for (int index = 0; index < 5; ++index) {
		ws_.food_products.push_back(kFoodProductIds[index]);
		ws_.food_factors.push_back(kCaloricFactor[index]);
	}
	// F3: sem goods.json carregado nenhum dos 21 primÃ¡rios Ã© durÃ¡vel.
	ws_.product_durable.assign(kMaxProducts, 0);
}

EconomySim::~EconomySim() {
	// Se o sim for liberado com um dia AINDA rodando na thread do pool (o jogo
	// encerrando, por exemplo), espera o tick sair antes de destruir os
	// contÃªineres do status: destruÃ­-los debaixo de quem os escreve era um
	// access violation (0xC0000005) intermitente no fim do processo. O mutex Ã©
	// recursivo, entÃ£o a mesma thread do tick nÃ£o bloqueia aqui.
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
}

void EconomySim::_bind_methods() {
	ClassDB::bind_method(D_METHOD("initialize_scenario"), &EconomySim::initialize_scenario);
	ClassDB::bind_method(D_METHOD("initialize_world", "cities", "ports", "buildings"),
	                     &EconomySim::initialize_world);
	ClassDB::bind_method(D_METHOD("tick_day"), &EconomySim::tick_day);
	ClassDB::bind_method(D_METHOD("tick_and_publish"), &EconomySim::tick_and_publish);
	ClassDB::bind_method(D_METHOD("apply_commands"), &EconomySim::apply_commands);
	ClassDB::bind_method(D_METHOD("pending_command_count"),
	                     &EconomySim::pending_command_count);
	ClassDB::bind_method(D_METHOD("get_command_log"), &EconomySim::get_command_log);
	ClassDB::bind_method(D_METHOD("clear_command_log"), &EconomySim::clear_command_log);
	ClassDB::bind_method(D_METHOD("command_log_truncated"),
	                     &EconomySim::command_log_truncated);
	ClassDB::bind_method(D_METHOD("replay_commands", "entries", "target_day"),
	                     &EconomySim::replay_commands);
	ClassDB::bind_method(D_METHOD("set_profiling", "enabled"),
	                     &EconomySim::set_profiling);
	ClassDB::bind_method(D_METHOD("set_sim_threads", "threads"),
	                     &EconomySim::set_sim_threads);
	ClassDB::bind_method(D_METHOD("get_profile"), &EconomySim::get_profile);
	ClassDB::bind_method(D_METHOD("warm_status"), &EconomySim::warm_status);
	ClassDB::bind_method(D_METHOD("get_status"), &EconomySim::get_status);
	ClassDB::bind_method(D_METHOD("get_city_status", "city_id"),
	                     &EconomySim::get_city_status);
	// A4: consultas sob demanda (status leve + colunas + fichas + ranking).
	ClassDB::bind_method(D_METHOD("get_day_summary"),
	                     &EconomySim::get_day_summary);
	ClassDB::bind_method(D_METHOD("get_city_columns"),
	                     &EconomySim::get_city_columns);
	ClassDB::bind_method(D_METHOD("get_nation_columns"),
	                     &EconomySim::get_nation_columns);
	ClassDB::bind_method(D_METHOD("get_city_rows"),
	                     &EconomySim::get_city_rows);
	ClassDB::bind_method(D_METHOD("get_city_info", "city_id"),
	                     &EconomySim::get_city_info);
	ClassDB::bind_method(D_METHOD("get_nation_rows"),
	                     &EconomySim::get_nation_rows);
	ClassDB::bind_method(D_METHOD("get_nation_status", "nation_tag"),
	                     &EconomySim::get_nation_status);
	ClassDB::bind_method(D_METHOD("get_trade_orders"),
	                     &EconomySim::get_trade_orders);
	ClassDB::bind_method(D_METHOD("get_ports"), &EconomySim::get_ports);
	ClassDB::bind_method(D_METHOD("get_currency_rows"),
	                     &EconomySim::get_currency_rows);
	ClassDB::bind_method(D_METHOD("get_products"), &EconomySim::get_products);
	ClassDB::bind_method(D_METHOD("get_light_columns"),
	                     &EconomySim::get_light_columns);
	ClassDB::bind_method(D_METHOD("get_city_ranking", "field", "count"),
	                     &EconomySim::get_city_ranking);
	ClassDB::bind_method(D_METHOD("get_nation_ranking", "field", "count"),
	                     &EconomySim::get_nation_ranking);
	ClassDB::bind_method(D_METHOD("get_data_version"),
	                     &EconomySim::get_data_version);
	ClassDB::bind_method(D_METHOD("get_save_version"),
	                     &EconomySim::get_save_version);
	ClassDB::bind_method(D_METHOD("price_shock", "product", "multiplier"),
	                     &EconomySim::price_shock);
	ClassDB::bind_method(D_METHOD("set_suitability", "city_id", "product", "value"),
	                     &EconomySim::set_suitability);
	ClassDB::bind_method(D_METHOD("set_land_capacity", "city_id", "product", "value"),
	                     &EconomySim::set_land_capacity);
	// G (D-096) â€” agricultura sazonal.
	ClassDB::bind_method(D_METHOD("set_crop_plan", "city_id", "product",
	                     "area_ha", "yield_t_ha", "seasons"),
	                     &EconomySim::set_crop_plan);
	ClassDB::bind_method(D_METHOD("set_crop_initial_stock", "city_id", "product",
	                     "tons"), &EconomySim::set_crop_initial_stock);
	ClassDB::bind_method(D_METHOD("set_crop_loss", "product", "loss_pct",
	                     "storage_share_pct"), &EconomySim::set_crop_loss);
	ClassDB::bind_method(D_METHOD("set_shelf_life", "product", "days"),
	                     &EconomySim::set_shelf_life);
	ClassDB::bind_method(D_METHOD("set_fertilizer_n", "city_id", "product",
	                     "n_kg_ha"), &EconomySim::set_fertilizer_n);
	ClassDB::bind_method(D_METHOD("set_storage_quality", "city_id", "quality"),
	                     &EconomySim::set_storage_quality);
	ClassDB::bind_method(D_METHOD("get_agriculture_status"),
	                     &EconomySim::get_agriculture_status);
	ClassDB::bind_method(D_METHOD("set_storage_capacity", "city_id", "type",
		"owner", "tons"), &EconomySim::set_storage_capacity);
	ClassDB::bind_method(D_METHOD("set_storage_product_type", "product",
		"type"), &EconomySim::set_storage_product_type);
	ClassDB::bind_method(D_METHOD("storage_build", "city_id", "type", "owner",
		"tons"), &EconomySim::storage_build);
	ClassDB::bind_method(D_METHOD("storage_trade", "city_id", "type", "owner",
		"product", "tons", "buy", "relief"), &EconomySim::storage_trade);
	ClassDB::bind_method(D_METHOD("get_storage_status"),
		&EconomySim::get_storage_status);
	ClassDB::bind_method(D_METHOD("get_balance_status"),
		&EconomySim::get_balance_status);
	ClassDB::bind_method(D_METHOD("get_central_bank_status"),
		&EconomySim::get_central_bank_status);
	ClassDB::bind_method(D_METHOD("set_central_bank_gold", "tonnes_by_nation"),
		&EconomySim::set_central_bank_gold);
	ClassDB::bind_method(D_METHOD("set_central_bank_policy", "currency",
		"independence"), &EconomySim::set_central_bank_policy);
	// C2b2 (D-106) â€” regimes de cÃ¢mbio, padrÃ£o monetÃ¡rio e ouro fÃ­sico.
	ClassDB::bind_method(D_METHOD("set_exchange_regime", "currency", "regime",
		"parity", "band_width", "nation"),
		&EconomySim::set_exchange_regime, DEFVAL(String()));
	// C2c (D-108) â€” regimes reais (IMF AREAER 2026) e sucessÃ£o de moeda.
	ClassDB::bind_method(D_METHOD("set_fx_regimes", "data"),
		&EconomySim::set_fx_regimes);
	ClassDB::bind_method(D_METHOD("migrate_currency", "from", "to",
		"units_per_target"), &EconomySim::migrate_currency);
	ClassDB::bind_method(D_METHOD("set_monetary_standard", "standard"),
		&EconomySim::set_monetary_standard);
	ClassDB::bind_method(D_METHOD("set_central_bank_reserves",
		"anchor_by_currency"), &EconomySim::set_central_bank_reserves);
	ClassDB::bind_method(D_METHOD("seed_central_bank_reserves", "days"),
		&EconomySim::seed_central_bank_reserves);
	ClassDB::bind_method(D_METHOD("set_exchange_policy", "band_width",
		"defence_share", "gold_reserve_ratio"),
		&EconomySim::set_exchange_policy);
	ClassDB::bind_method(D_METHOD("central_bank_gold_trade", "currency",
		"tonnes", "buy"), &EconomySim::central_bank_gold_trade);
	ClassDB::bind_method(D_METHOD("central_bank_gold_convert", "currency",
		"tonnes"), &EconomySim::central_bank_gold_convert);
	// C3 (D-107): crÃ©dito de empresa (banco agregado como credor).
	ClassDB::bind_method(D_METHOD("set_bank_credit", "enabled"),
		&EconomySim::set_bank_credit);
	ClassDB::bind_method(D_METHOD("set_bank_policy", "spread", "leverage_cap",
		"interest_cover", "default_days", "repay_share"),
		&EconomySim::set_bank_policy);
	ClassDB::bind_method(D_METHOD("set_bank_capital", "nation", "amount",
		"role"), &EconomySim::set_bank_capital,
		DEFVAL(String("public")));
	ClassDB::bind_method(D_METHOD("set_bank_definitions", "definitions"),
		&EconomySim::set_bank_definitions);
	ClassDB::bind_method(D_METHOD("group_borrow", "group_id", "amount"),
		&EconomySim::group_borrow);
	ClassDB::bind_method(D_METHOD("get_bank_status"), &EconomySim::get_bank_status);
	// C6 (D-114): crise bancÃ¡ria (regras de laboratÃ³rio e choque de perda).
	ClassDB::bind_method(D_METHOD("set_bank_crisis", "config"),
		&EconomySim::set_bank_crisis);
	ClassDB::bind_method(D_METHOD("bank_shock", "bank_id", "loss",
		"loss_share"), &EconomySim::bank_shock,
		DEFVAL(0.0));
	ClassDB::bind_method(D_METHOD("set_stock_exchange", "config"),
		&EconomySim::set_stock_exchange);
	ClassDB::bind_method(D_METHOD("get_stock_status"), &EconomySim::get_stock_status);
	ClassDB::bind_method(D_METHOD("set_storage_stock", "city_id", "type",
		"owner", "product", "tons"), &EconomySim::set_storage_stock);
	ClassDB::bind_method(D_METHOD("get_storage_product_types"),
		&EconomySim::get_storage_product_types);
	// PARTE E (D-116) â€” energia elÃ©trica (save v35).
	ClassDB::bind_method(D_METHOD("set_power_definitions", "nations", "cities",
		"params", "only_missing"), &EconomySim::set_power_definitions,
		DEFVAL(true));
	ClassDB::bind_method(D_METHOD("set_power_params", "values"),
		&EconomySim::set_power_params);
	ClassDB::bind_method(D_METHOD("set_power_city", "city_id", "connection",
		"line_mw", "access_pct"), &EconomySim::set_power_city);
	ClassDB::bind_method(D_METHOD("build_power_plant", "city_id", "fuel", "mw",
		"group_id"), &EconomySim::build_power_plant, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("build_power_line", "city_id", "mw",
		"group_id"), &EconomySim::build_power_line, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("set_power_interconnects", "links",
		"only_missing"), &EconomySim::set_power_interconnects,
		DEFVAL(true));
	ClassDB::bind_method(D_METHOD("build_interconnect", "nation_a",
		"nation_b", "mw", "group_id"), &EconomySim::build_interconnect,
		DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("get_power_status"),
		&EconomySim::get_power_status);
	ClassDB::bind_method(D_METHOD("set_deposit", "city_id", "product", "reserve_kt",
	                     "quality", "depth_m"),
	                     &EconomySim::set_deposit);
	ClassDB::bind_method(D_METHOD("set_external_demand", "city_id", "product",
	                     "ceiling_t_per_day"),
	                     &EconomySim::set_external_demand);
	ClassDB::bind_method(D_METHOD("set_external_income_index", "city_id", "index"),
	                     &EconomySim::set_external_income_index);
	ClassDB::bind_method(D_METHOD("set_productivity_index", "city_id", "index"),
	                     &EconomySim::set_productivity_index);
	ClassDB::bind_method(D_METHOD("set_income_gini", "city_id", "gini"),
	                     &EconomySim::set_income_gini);
	ClassDB::bind_method(D_METHOD("set_city_strata", "city_id", "shares"),
	                     &EconomySim::set_city_strata);
	ClassDB::bind_method(D_METHOD("set_city_region", "city_id", "data"),
	                     &EconomySim::set_city_region);
	ClassDB::bind_method(D_METHOD("get_region_status", "city_id"),
	                     &EconomySim::get_region_status);
	ClassDB::bind_method(D_METHOD("set_labor_profile", "city_id", "profile"),
	                     &EconomySim::set_labor_profile);
	ClassDB::bind_method(D_METHOD("set_minimum_wage", "nation", "value"),
	                     &EconomySim::set_minimum_wage);
	ClassDB::bind_method(D_METHOD("set_wage_rigidity", "value"),
	                     &EconomySim::set_wage_rigidity);
	ClassDB::bind_method(D_METHOD("set_goods", "goods"),
	                     &EconomySim::set_goods);
	ClassDB::bind_method(D_METHOD("set_recipes", "recipes"),
	                     &EconomySim::set_recipes);
	ClassDB::bind_method(D_METHOD("set_needs", "needs"),
	                     &EconomySim::set_needs);
	ClassDB::bind_method(D_METHOD("set_bootstrap_industry", "values"),
	                     &EconomySim::set_bootstrap_industry);
	ClassDB::bind_method(D_METHOD("set_services", "services"),
	                     &EconomySim::set_services);
	ClassDB::bind_method(D_METHOD("set_sector_employment", "data"),
	                     &EconomySim::set_sector_employment);
	ClassDB::bind_method(D_METHOD("set_age_structure", "data"),
	                     &EconomySim::set_age_structure);
	ClassDB::bind_method(D_METHOD("set_informality", "data"),
	                     &EconomySim::set_informality);
	ClassDB::bind_method(D_METHOD("set_health_access", "data"),
	                     &EconomySim::set_health_access);
	ClassDB::bind_method(D_METHOD("set_region_infra", "data"),
	                     &EconomySim::set_region_infra);
	ClassDB::bind_method(D_METHOD("set_region_property", "data"),
	                     &EconomySim::set_region_property);
	ClassDB::bind_method(D_METHOD("set_property_tax_rate", "nation", "rate"),
	                     &EconomySim::set_property_tax_rate);
	ClassDB::bind_method(D_METHOD("set_urban", "urban_land", "housing_city",
	                     "housing", "municipal_finance"),
	                     &EconomySim::set_urban);
	ClassDB::bind_method(D_METHOD("get_urban_city", "city_id"),
	                     &EconomySim::get_urban_city);
	ClassDB::bind_method(D_METHOD("urban_expand", "city_id", "zone", "km2"),
	                     &EconomySim::urban_expand);
	ClassDB::bind_method(D_METHOD("urban_densify", "city_id", "zone"),
	                     &EconomySim::urban_densify);
	ClassDB::bind_method(D_METHOD("urban_social_housing", "city_id", "units"),
	                     &EconomySim::urban_social_housing);
	ClassDB::bind_method(D_METHOD("set_instant_construction", "instant"),
	                     &EconomySim::set_instant_construction);
	// A5: fase da IA no fim do dia (desligada por padrÃ£o).
	ClassDB::bind_method(D_METHOD("set_ai_enabled", "enabled"),
	                     &EconomySim::set_ai_enabled);
	ClassDB::bind_method(D_METHOD("get_service_sectors"),
	                     &EconomySim::get_service_sectors);
	ClassDB::bind_method(D_METHOD("get_services_detail", "city_id"),
	                     &EconomySim::get_services_detail);
	ClassDB::bind_method(D_METHOD("set_service_capacity", "city_id", "sector",
	                     "units_per_day"),
	                     &EconomySim::set_service_capacity);
	ClassDB::bind_method(D_METHOD("set_service_business_state", "business_id",
	                     "loss_days", "profit"),
	                     &EconomySim::set_service_business_state);
	ClassDB::bind_method(D_METHOD("get_service_business_count"),
	                     &EconomySim::get_service_business_count);
	ClassDB::bind_method(D_METHOD("get_service_bankruptcies"),
	                     &EconomySim::get_service_bankruptcies);
	ClassDB::bind_method(D_METHOD("get_bankruptcy_diag"),
	                     &EconomySim::get_bankruptcy_diag);
	ClassDB::bind_method(D_METHOD("seed_industry"),
	                     &EconomySim::seed_industry);
	ClassDB::bind_method(D_METHOD("invest", "city_id", "building_id", "amount"),
	                     &EconomySim::invest);
	ClassDB::bind_method(D_METHOD("build", "city_id", "building_id", "resource",
	                     "input_product", "output_product", "capacity", "jobs", "wage",
	                     "opex", "input_per_output", "owner", "cost", "recipe_id"),
	                     &EconomySim::build, DEFVAL(""));
	ClassDB::bind_method(D_METHOD("spend_government", "city_id", "amount"),
	                     &EconomySim::spend_government);
	ClassDB::bind_method(D_METHOD("expand_port", "port_id", "t_per_day"),
	                     &EconomySim::expand_port);
	ClassDB::bind_method(D_METHOD("set_port_capacity", "port_id", "t_per_day"),
	                     &EconomySim::set_port_capacity);
	ClassDB::bind_method(D_METHOD("to_dict"), &EconomySim::to_dict);
	ClassDB::bind_method(D_METHOD("from_dict", "data"), &EconomySim::from_dict);
	ClassDB::bind_method(D_METHOD("to_snapshot"),
	                     &EconomySim::to_snapshot);
	ClassDB::bind_method(D_METHOD("from_snapshot", "snapshot"),
	                     &EconomySim::from_snapshot);
	// A1: as mesmas fotos, mas pelo serializador da biblioteca (round-trip
	// IEEE-exato) â€” o snapshot nÃ£o atravessa o JSON do Godot.
	ClassDB::bind_method(D_METHOD("to_snapshot_file", "path"),
	                     &EconomySim::to_snapshot_file);
	ClassDB::bind_method(D_METHOD("from_snapshot_file", "path"),
	                     &EconomySim::from_snapshot_file);
	ClassDB::bind_method(D_METHOD("set_balance", "values"),
	                     &EconomySim::set_balance);
	ClassDB::bind_method(D_METHOD("set_fleet", "nation", "trucks", "ships",
	                     "planes", "diesel", "jet_fuel", "spare_parts"),
	                     &EconomySim::set_fleet);
	ClassDB::bind_method(D_METHOD("get_fleet", "nation"),
	                     &EconomySim::get_fleet);
	ClassDB::bind_method(D_METHOD("fleet_buy", "nation", "product", "amount",
	                     "budget_share"),
	                     &EconomySim::fleet_buy, DEFVAL(0.1));
	ClassDB::bind_method(D_METHOD("set_market_stock", "city_id", "product", "stock"),
	                     &EconomySim::set_market_stock);
	ClassDB::bind_method(D_METHOD("set_market_price", "city_id", "product", "price"),
	                     &EconomySim::set_market_price);
	// D-125: eficiência de produção (API sem UI; comandos A3).
	ClassDB::bind_method(D_METHOD("change_product", "city_id", "building_id",
	                     "product", "recipe_id"),
	                     &EconomySim::change_product, DEFVAL(""));
	ClassDB::bind_method(D_METHOD("set_refinery_mode", "city_id",
	                     "building_id", "mode"),
	                     &EconomySim::set_refinery_mode);
	ClassDB::bind_method(D_METHOD("set_building_equipment", "city_id",
	                     "building_id", "level"),
	                     &EconomySim::set_building_equipment);
	ClassDB::bind_method(D_METHOD("set_efficiency_params", "values"),
	                     &EconomySim::set_efficiency_params);
	ClassDB::bind_method(D_METHOD("get_efficiency_params"),
	                     &EconomySim::get_efficiency_params);
	// D-126: sobrevivencia em crise (parametros, status e acoes).
	ClassDB::bind_method(D_METHOD("set_crisis_params", "values"),
	                     &EconomySim::set_crisis_params);
	ClassDB::bind_method(D_METHOD("get_crisis_params"),
	                     &EconomySim::get_crisis_params);
	ClassDB::bind_method(D_METHOD("set_ai_brain_params", "values"),
	                     &EconomySim::set_ai_brain_params);
	ClassDB::bind_method(D_METHOD("get_ai_brain_params"),
	                     &EconomySim::get_ai_brain_params);
	ClassDB::bind_method(D_METHOD("get_ai_brain_status"),
	                     &EconomySim::get_ai_brain_status);
	ClassDB::bind_method(D_METHOD("get_crisis_status"),
	                     &EconomySim::get_crisis_status);
	ClassDB::bind_method(D_METHOD("crisis_layoff", "city_id", "building_id",
	                     "share"),
	                     &EconomySim::crisis_layoff, DEFVAL(0.0));
	ClassDB::bind_method(D_METHOD("crisis_borrow", "city_id", "amount"),
	                     &EconomySim::crisis_borrow, DEFVAL(0.0));
	ClassDB::bind_method(D_METHOD("crisis_liquidate", "city_id",
	                     "building_id", "share"),
	                     &EconomySim::crisis_liquidate, DEFVAL(0.0));
	ClassDB::bind_method(D_METHOD("crisis_mothball", "city_id",
	                     "building_id"),
	                     &EconomySim::crisis_mothball);
	ClassDB::bind_method(D_METHOD("crisis_reopen", "city_id", "building_id"),
	                     &EconomySim::crisis_reopen);
	ClassDB::bind_method(D_METHOD("crisis_auction", "city_id",
	                     "building_id"),
	                     &EconomySim::crisis_auction);
	ClassDB::bind_method(D_METHOD("crisis_set_stress", "city_id",
	                     "building_id", "loss_days"),
	                     &EconomySim::crisis_set_stress);
	ClassDB::bind_method(D_METHOD("crisis_step"),
	                     &EconomySim::crisis_step);
	ClassDB::bind_method(D_METHOD("mature_world_efficiency", "buildings"),
	                     &EconomySim::mature_world_efficiency,
	                     DEFVAL(Array()));
	ClassDB::bind_method(D_METHOD("set_currencies", "currencies"),
	                     &EconomySim::set_currencies);
	ClassDB::bind_method(D_METHOD("get_currency", "nation"),
	                     &EconomySim::get_currency);
	ClassDB::bind_method(D_METHOD("get_currencies"),
	                     &EconomySim::get_currencies);
	ClassDB::bind_method(D_METHOD("set_currency_rate", "code", "rate", "floating",
	                     "base_rate"),
	                     &EconomySim::set_currency_rate, DEFVAL(true), DEFVAL(-1.0));
	ClassDB::bind_method(D_METHOD("set_tariff", "buyer_nation", "seller_nation",
	                     "product", "rate"),
	                     &EconomySim::set_tariff);
	ClassDB::bind_method(D_METHOD("get_tariff", "buyer_nation", "seller_nation",
	                     "product"),
	                     &EconomySim::get_tariff);
	ClassDB::bind_method(D_METHOD("get_tariffs"),
	                     &EconomySim::get_tariffs);
	ClassDB::bind_method(D_METHOD("seed_groups"), &EconomySim::seed_groups);
	ClassDB::bind_method(D_METHOD("set_group_names", "names"),
	                     &EconomySim::set_group_names);
	ClassDB::bind_method(D_METHOD("get_groups"), &EconomySim::get_groups);
	ClassDB::bind_method(D_METHOD("get_group_ranking"),
	                     &EconomySim::get_group_ranking);
	ClassDB::bind_method(D_METHOD("get_group", "group_id"),
	                     &EconomySim::get_group);
	ClassDB::bind_method(D_METHOD("fund_group", "group_id", "amount"),
	                     &EconomySim::fund_group);
	ClassDB::bind_method(D_METHOD("group_invest", "group_id", "city_id",
	                     "building_id", "resource", "output_product", "capacity",
	                     "recipe_id", "partner_group"),
	                     &EconomySim::group_invest, DEFVAL(""));
	// D-086: aprofundar/expandir mina (comando A3 `deepen_mine`).
	ClassDB::bind_method(D_METHOD("deepen_mine", "city_id", "building_id"),
	                     &EconomySim::deepen_mine);
	ClassDB::bind_method(D_METHOD("infra_build", "city_id", "infra_type"),
	                     &EconomySim::infra_build);
	ClassDB::bind_method(D_METHOD("set_fdi_policy", "nation", "sector", "mode",
	                     "min_local"),
	                     &EconomySim::set_fdi_policy, DEFVAL(0.0));
	ClassDB::bind_method(D_METHOD("set_sector_incentive", "nation", "sector",
	                     "tax_holiday", "subsidy"),
	                     &EconomySim::set_sector_incentive);
	ClassDB::bind_method(D_METHOD("create_state_company", "nation", "name",
	                     "capital"),
	                     &EconomySim::create_state_company);
	ClassDB::bind_method(D_METHOD("privatize", "city_id", "building_id",
	                     "buyer_group"),
	                     &EconomySim::privatize);
	ClassDB::bind_method(D_METHOD("nationalize", "city_id", "building_id"),
	                     &EconomySim::nationalize);
	ClassDB::bind_method(D_METHOD("concede", "city_id", "building_id",
	                     "group_id", "royalty_rate"),
	                     &EconomySim::concede, DEFVAL(0.0));
	ClassDB::bind_method(D_METHOD("get_investment_status"),
	                     &EconomySim::get_investment_status);
	ClassDB::bind_method(D_METHOD("get_investment_signals"),
	                     &EconomySim::get_investment_signals);
	ClassDB::bind_method(D_METHOD("groups_seeded"),
	                     &EconomySim::groups_seeded);
}

void EconomySim::fill_leg_status(Dictionary& dict, const Leg& leg) const {
	const ModeSpec& mode_spec = spec(leg.mode);
	const StatusKeys& k = status_keys_;
	dict[k.mode] = String(mode_spec.name);
	dict[k.from] = String(leg.from.c_str());
	dict[k.to] = String(leg.to.c_str());
	dict[k.from_px] = Vector2(leg.from_point.x, leg.from_point.y);
	dict[k.to_px] = Vector2(leg.to_point.x, leg.to_point.y);
	dict[k.distance_km] = leg.distance_km;
	dict[k.load_tons] = leg.load_tons;
	dict[k.trips] = leg.trips;
	dict[k.transit_hours] = leg.transit_hours;
	dict[k.cost] = leg.cost;
	dict[k.speed_kmh] = mode_spec.speed_kmh;
	dict[k.batch_capacity_tons] = mode_spec.batch_capacity_tons;
}

// CenÃ¡rio de validaÃ§Ã£o: duas cidades, POPs agregados, trigo consumido direto.
// ---------------------------------------------------------------------------

const ogs::Currency* EconomySim::currency_for(
		const std::string& nation) const {
	return sim_core_.currency_for(nation);
}

ogs::Currency* EconomySim::currency_for(
		const std::string& nation) {
	const auto found = ws_.nation_currency.find(nation);
	if (found == ws_.nation_currency.end()
			|| found->second < 0
			|| found->second >= static_cast<int>(ws_.currencies.size())) {
		return nullptr;
	}
	return &ws_.currencies[static_cast<std::size_t>(found->second)];
}

double EconomySim::competitiveness(const std::string& nation) const {
	return sim_core_.competitiveness(nation);
}

void EconomySim::rebuild_tariff_lookup() {
	sim_core_.rebuild_tariff_lookup();
}

double EconomySim::tariff_rate(const std::string& buyer_nation,
		const std::string& seller_nation, int product) const {
	return sim_core_.tariff_rate(buyer_nation, seller_nation, product);
}

Array EconomySim::get_tariffs() const {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Array out;
	out.resize(static_cast<int>(ws_.tariffs.size()));
	for (std::size_t index = 0; index < ws_.tariffs.size(); ++index) {
		const Tariff& tariff = ws_.tariffs[index];
		Dictionary entry;
		entry["buyer"] = String(tariff.buyer.c_str());
		entry["seller"] = String(tariff.seller.c_str());
		entry["product"] = String(tariff.product.c_str());
		entry["rate"] = tariff.rate;
		out[static_cast<int>(index)] = entry;
	}
	return out;
}

double EconomySim::get_tariff(const String& buyer_nation,
		const String& seller_nation, const String& product) const {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const int index = product == "*" ? -1 : product_index(product);
	if (product != "*" && index < 0) {
		return 0.0;
	}
	if (index < 0) {
		const std::string buyer(buyer_nation.utf8().get_data());
		const std::string seller(seller_nation.utf8().get_data());
		const std::string keys[2] = {
			buyer + '|' + seller + "|*",
			buyer + "|*|*",
		};
		for (const std::string& key : keys) {
			const auto found = ws_.tariff_lookup.find(key);
			if (found != ws_.tariff_lookup.end()) {
				return std::clamp(found->second, 0.0, 1.0);
			}
		}
		return 0.0;
	}
	return tariff_rate(std::string(buyer_nation.utf8().get_data()),
		std::string(seller_nation.utf8().get_data()), index);
}

void EconomySim::apply_set_tariff(const String& buyer_nation,
		const String& seller_nation, const String& product, double rate) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Tariff tariff;
	tariff.buyer = std::string(buyer_nation.utf8().get_data());
	tariff.seller = seller_nation.is_empty() ? "*"
		: std::string(seller_nation.utf8().get_data());
	tariff.product = product.is_empty() ? "*"
		: std::string(product.utf8().get_data());
	tariff.rate = std::clamp(std::isfinite(rate) ? rate : 0.0, 0.0, 1.0);
	const std::string key = tariff.buyer + '|' + tariff.seller + '|'
		+ tariff.product;
	for (Tariff& existing : ws_.tariffs) {
		if (existing.buyer == tariff.buyer && existing.seller == tariff.seller
				&& existing.product == tariff.product) {
			existing.rate = tariff.rate;
			ws_.tariff_lookup[key] = tariff.rate;
			// A2: o Ã­ndice inteiro acompanha a mudanÃ§a (a tabela de naÃ§Ãµes jÃ¡
			// existe neste ponto; o rebuild Ã© da ordem de centenas de linhas).
			// A3: a invalidaÃ§Ã£o fica em `apply_pending_locked`, ponto Ãºnico do lote.
			sim_core_.rebuild_tariff_lookup();
			return;
		}
	}
	ws_.tariffs.push_back(tariff);
	ws_.tariff_lookup[key] = tariff.rate;
	sim_core_.rebuild_tariff_lookup();
}

Dictionary EconomySim::get_currency(const String& nation) const {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Dictionary out;
	const std::string tag(nation.utf8().get_data());
	const Currency* currency = currency_for(tag);
	if (currency == nullptr) {
		out["nation"] = nation;
		out["code"] = "XGS";
		out["rate"] = 1.0;
		out["base_rate"] = 1.0;
		out["source"] = "default:anchor";
		out["regime"] = "float";
		out["floating"] = true;
		out["fallback"] = false;
		out["trade_balance"] = 0.0;
		return out;
	}
	out["nation"] = nation;
	out["code"] = String(currency->code.c_str());
	out["rate"] = currency->rate;
	out["base_rate"] = currency->base_rate;
	out["source"] = String(currency->source.c_str());
	out["regime"] = currency->floating ? "float" : "peg";
	out["floating"] = currency->floating;
	out["fallback"] = currency->fallback;
	out["trade_balance"] = currency->balance_ema;
	out["gross_trade"] = currency->gross_ema;
	return out;
}

Array EconomySim::get_currencies() const {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Array out;
	out.resize(static_cast<int>(ws_.currencies.size()));
	for (std::size_t index = 0; index < ws_.currencies.size(); ++index) {
		const Currency& currency = ws_.currencies[index];
		Dictionary entry;
		entry["code"] = String(currency.code.c_str());
		entry["rate"] = currency.rate;
		entry["base_rate"] = currency.base_rate;
		entry["regime"] = currency.floating ? "float" : "peg";
		entry["fallback"] = currency.fallback;
		entry["source"] = String(currency.source.c_str());
		entry["trade_balance"] = currency.balance_ema;
		entry["gross_trade"] = currency.gross_ema;
		Array nations;
		nations.resize(static_cast<int>(currency.nations.size()));
		for (std::size_t n = 0; n < currency.nations.size(); ++n) {
			nations[static_cast<int>(n)] = String(currency.nations[n].c_str());
		}
		entry["nations"] = nations;
		out[static_cast<int>(index)] = entry;
	}
	return out;
}

void EconomySim::apply_set_currency_rate(const String& code, double rate,
		bool floating, double base_rate) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const auto found = ws_.currency_by_code.find(std::string(code.utf8().get_data()));
	if (found == ws_.currency_by_code.end() || !std::isfinite(rate) || rate <= 0.0) {
		return;
	}
	Currency& currency = ws_.currencies[static_cast<std::size_t>(found->second)];
	currency.rate = rate;
	// `base_rate` fixa a referÃªncia da banda e da competitividade float. Sem
	// ele, a taxa informada vira a prÃ³pria referÃªncia (comportamento anterior).
	currency.base_rate = (std::isfinite(base_rate) && base_rate > 0.0)
		? base_rate : rate;
	currency.floating = floating;
	// C2b2 (D-106): o regime salvo Ã© a fonte do ramo do dia (float/peg/band).
	// O toggle do F5 (`set_currency_rate(..., floating)`) tem de mover os dois
	// juntos; senÃ£o a moeda fica `floating` para o cÃ¢mbio sombreado e `peg`
	// para o ramo do dia, e a taxa congela (moeda de fixture parada em 1,0).
	// `band` nÃ£o Ã© rebaixada: ela jÃ¡ flutua dentro da banda.
	if (floating && currency.exchange_regime == central_bank::kRegimePeg) {
		currency.exchange_regime = central_bank::kRegimeFloating;
	} else if (!floating
			&& currency.exchange_regime == central_bank::kRegimeFloating) {
		currency.exchange_regime = central_bank::kRegimePeg;
	}
}

int EconomySim::set_fx_regimes(const Dictionary& data) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	ws_.fx_regime.clear();
	ws_.fx_no_policy_nations.clear();
	ws_.fx_regime_source.clear();
	// Fonte do arquivo inteiro (o status publica o que estÃ¡ valendo).
	const String as_of = String(data.get("as_of", ""));
	const String title = String(data.get("title", "IMF AREAER"));
	std::string source = std::string(title.utf8().get_data());
	if (!as_of.is_empty()) {
		source += " (" + std::string(as_of.utf8().get_data()) + ")";
	}
	ws_.fx_regime_source = source;
	const Variant rows_variant = data.get("currencies", Variant());
	if (rows_variant.get_type() != Variant::DICTIONARY) {
		return 0;
	}
	const Dictionary rows = rows_variant;
	int count = 0;
	const Array codes = rows.keys();
	for (int index = 0; index < codes.size(); ++index) {
		const std::string code(String(codes[index]).utf8().get_data());
		const Variant entry_variant = rows.get(codes[index], Variant());
		if (code.empty() || entry_variant.get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary entry = entry_variant;
		ogs::FxRegimeInfo info;
		// Regime: o vocabulÃ¡rio do arquivo Ã© o do jogo (floating/band/peg).
		// "unknown" (sem fonte no FMI) Ã© preenchido pela declaraÃ§Ã£o do emissor
		// no DADO; se chegar vazio, o carregador declara band de jogo (Â§D-108d)
		// em vez de deixar a moeda sem regime.
		const String regime = String(entry.get("game_regime", ""));
		info.regime = ogs::central_bank::regime_from_name(
			std::string(regime.utf8().get_data()), ogs::central_bank::kRegimeBand);
		info.anchor = std::string(String(entry.get("anchor", ""))
			.utf8().get_data());
		info.anchor_type = std::string(String(entry.get("anchor_type", "none"))
			.utf8().get_data());
		if (info.anchor.empty() && info.anchor_type == "currency") {
			info.anchor_type = "none";
		}
		info.anchor_source = std::string(String(entry.get("anchor_source", ""))
			.utf8().get_data());
		info.source = std::string(String(entry.get("source", ""))
			.utf8().get_data());
		if (info.source.empty()) {
			info.source = source + " â€” default de jogo";
		}
		// ComposiÃ§Ã£o PUBLICADA (quando o dado traz): {moeda: peso}.
		const Variant basket_variant = entry.get("basket", Variant());
		if (basket_variant.get_type() == Variant::DICTIONARY) {
			const Dictionary basket = basket_variant;
			const Array units = basket.keys();
			for (int unit_index = 0; unit_index < units.size(); ++unit_index) {
				const std::string unit(
					String(units[unit_index]).utf8().get_data());
				const double value = double(basket.get(units[unit_index], 0.0));
				if (!unit.empty() && std::isfinite(value) && value > 0.0) {
					info.basket[unit] = value;
				}
			}
		}
		// Emissor: a naÃ§Ã£o da moeda quando ela Ã© de um paÃ­s sÃ³. Moeda
		// compartilhada por vÃ¡rias naÃ§Ãµes (uniÃ£o monetÃ¡ria/dolarizaÃ§Ã£o) fica
		// TRAVADA: ninguÃ©m muda o regime (regra de bloco Ã© fase futura).
		const Variant nations_variant = entry.get("nations", Array());
		if (nations_variant.get_type() == Variant::ARRAY) {
			const Array nations = nations_variant;
			const bool shared = bool(entry.get("shared_currency", false));
			info.locked = shared || nations.size() > 1;
			if (nations.size() == 1) {
				info.issuer = std::string(String(nations[0]).utf8().get_data());
			}
		}
		// Dolarizados / sem moeda prÃ³pria: sem polÃ­tica cambial.
		const Variant exceptions_variant = entry.get("exceptions", Variant());
		if (exceptions_variant.get_type() == Variant::DICTIONARY) {
			const Dictionary exceptions = exceptions_variant;
			const Array tags = exceptions.keys();
			for (int tag_index = 0; tag_index < tags.size(); ++tag_index) {
				const std::string tag(String(tags[tag_index]).utf8().get_data());
				if (!tag.empty()) {
					ws_.fx_no_policy_nations.push_back(tag);
				}
			}
		}
		ws_.fx_regime[code] = std::move(info);
		++count;
	}
	// D-108(d): moeda sem classificaÃ§Ã£o AREAER nÃ£o fica sem regime. O bloco
	// `game_fallback` do MESMO arquivo declara o default de jogo (e a fonte,
	// quando hÃ¡ uma declaraÃ§Ã£o do emissor). NÃ£o Ã© dado observado.
	const Variant fallback_variant = data.get("game_fallback", Variant());
	if (fallback_variant.get_type() == Variant::DICTIONARY) {
		const Dictionary fallback = fallback_variant;
		const Variant rows_fallback = fallback.get("currencies", Variant());
		if (rows_fallback.get_type() == Variant::DICTIONARY) {
			const Dictionary rows = rows_fallback;
			const Array fallback_codes = rows.keys();
			for (int index = 0; index < fallback_codes.size(); ++index) {
				const std::string code(
					String(fallback_codes[index]).utf8().get_data());
				const Variant entry_variant =
					rows.get(fallback_codes[index], Variant());
				if (code.empty()
						|| entry_variant.get_type() != Variant::DICTIONARY) {
					continue;
				}
				const Dictionary entry = entry_variant;
				const auto found = ws_.fx_regime.find(code);
				if (found == ws_.fx_regime.end()) {
					continue;
				}
				FxRegimeInfo& info = found->second;
				const String regime = String(entry.get("regime", "band"));
				info.regime = ogs::central_bank::regime_from_name(
					std::string(regime.utf8().get_data()),
					ogs::central_bank::kRegimeBand);
				const String note = String(entry.get("source", ""));
				if (!note.is_empty()) {
					info.source = std::string(note.utf8().get_data());
				}
				info.anchor.clear();
				info.anchor_type = "none";
			}
		}
	}
	// O mesmo carregador roda sobre um mundo jÃ¡ criado (teste/API): aplica o
	// regime Ã s moedas que estÃ£o no estado. Em mundo vazio, `set_currencies`
	// aplica depois (o mundo nasce com o regime real).
	for (Currency& currency : ws_.currencies) {
		const auto found = ws_.fx_regime.find(currency.code);
		if (found == ws_.fx_regime.end()) {
			continue;
		}
		currency.exchange_regime = found->second.regime;
		// `band` deriva dentro da faixa (C2b2 Â§8); `peg` nÃ£o deriva.
		currency.floating = found->second.regime
			!= ogs::central_bank::kRegimePeg;
	}
	if (!ws_.currencies.empty()) {
		ogs::exchange::resolve_baskets(ws_);
		ogs::exchange::initialize_anchors(ws_);
	}
	status_invalidate();
	return count;
}


void EconomySim::initialize_scenario() {
	// Novo mundo: o cache de status pertence ao mundo anterior.
	status_slots_[0] = StatusCache{};
	status_slots_[1] = StatusCache{};
	status_live_ = -1;
	status_built_day_ = -1;
	++status_static_version_;
	++status_deposit_version_;
	// A3: a fila de comandos e o log tambÃ©m sÃ£o do mundo anterior.
	commands_.reset();
	// A5: o estado da IA pertence ao mundo â€” contadores, janelas e templates
	// de um cenÃ¡rio nÃ£o valem no outro.
	ai_phase_.reset();
	current_day_.store(0, std::memory_order_relaxed);
	// S14: o cenÃ¡rio AGRIA/PORTUS vive em sim/scenario_fixture.cpp (C++ puro);
	// aqui sÃ³ o cache de status (estado Godot) Ã© resetado.
	initialize_scenario_fixture(ws_, sim_core_);
}

void EconomySim::initialize_world(const Array& cities, const Array& ports,
		const Array& buildings) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	// Novo mundo: o cache de status pertence ao mundo anterior.
	status_slots_[0] = StatusCache{};
	status_slots_[1] = StatusCache{};
	status_live_ = -1;
	status_built_day_ = -1;
	++status_static_version_;
	++status_deposit_version_;
	// A3: a fila de comandos e o log tambÃ©m sÃ£o do mundo anterior.
	commands_.reset();
	// A5: mundo novo, estado da IA novo (sem os ids/janelas do anterior).
	ai_phase_.reset();
	current_day_.store(0, std::memory_order_relaxed);
	ws_.active = true;
	ws_.world_mode = true;
	ws_.day = 0;
	ws_.cities.clear();
	ws_.buildings.clear();
	ws_.construction_projects.clear();
	// C5 (D-112): a bolsa Ã© dos grupos deste mundo; o estado anterior nÃ£o vale
	// e a bolsa nasce no primeiro tick com o patrimÃ´nio medido do grupo.
	ogs::stock::reset(ws_);
	// C6 (D-114): mundo novo zera fundo de seguro, eventos e estado de crise
	// (os bancos nascem calmos e sem resoluÃ§Ã£o).
	ogs::bank_crisis::reset(ws_);
	// Â§19: mundo novo zera os vetores urbanos; o set_urban os preenche
	// depois (o casamento Ã© pelo id da cidade criada aqui).
	ws_.land.clear();
	ws_.housing.clear();
	ws_.city_gov.clear();
	ws_.in_transit.clear();
	ws_.invariant_violations.clear();
	ws_.ports.clear();
	ws_.last_orders.clear();
	ws_.bankruptcies = 0;
	// D-099: composiÃ§Ã£o das falÃªncias Ã© diagnÃ³stico de runtime â€” mundo novo
	// comeÃ§a do zero.
	for (int product = 0; product < kMaxProducts; ++product) {
		ws_.bankrupt_by_output[product] = 0;
	}
	ws_.bankrupt_planned_primary = 0;
	ws_.depleted_mines = 0;
	ws_.transport_cash = 0.0;
	ws_.foreign_cash = 0.0;
	ws_.construction_cash = 0.0;
	ws_.industry_seeded = false;
	// G: mundo novo zera o plano de safras e o livro de perdas (o
	// AgricultureService declara os planos depois, como a geografia).
	ws_.crop_plans.clear();
	ws_.crop_plan_lookup.clear();
	ws_.crop_planned.clear();
	ws_.crop_index_ready = false;
	ws_.crop_loss_pct.clear();
	ws_.crop_storage_share_pct.clear();
	ws_.crop_shelf_days.clear();
	ws_.crop_shelf_daily.clear();
	ws_.agri_chain.clear();
	ws_.fertilizer_product = -1;
	ws_.agri_month_key = 0;
	ws_.agri_delivered_today = 0.0;
	ws_.agri_harvests_today = 0;
	// F9: mundo novo, grupos/polÃ­tica novos (a semeadura roda no fim).
	ws_.groups.clear();
	ws_.group_lookup.clear();
	ws_.groups_seeded = false;
	ws_.fdi_policies.clear();
	ws_.fdi_lookup.clear();
	ws_.incentives.clear();
	ws_.incentive_lookup.clear();
	ws_.confidence.clear();
	ws_.confidence_penalty_day.clear();
	ws_.group_flows.clear();
	for (int index = 0; index < kMaxProducts; ++index) {
		ws_.produced[index] = 0.0;
		ws_.consumed_total[index] = 0.0;
		ws_.produced_today[index] = 0.0;
		ws_.consumed_today[index] = 0.0;
	}

	// Cidades reais do cenÃ¡rio: identidade geogrÃ¡fica + caixas iniciais.
	for (int index = 0; index < cities.size(); ++index) {
		const Dictionary d = cities[index];
		City city;
		city.id = std::string(String(d["id"]).utf8().get_data());
		city.name = std::string(String(d["name"]).utf8().get_data());
		city.nation = std::string(String(d["nation"]).utf8().get_data());
		city.home_nation =
			std::string(String(d.get("home_nation", d["nation"])).utf8().get_data());
		city.landmass = std::string(String(d.get("landmass", "")).utf8().get_data());
		// B3: regiÃ£o da cidade (estados inteiros). Com dado (`region` no dict),
		// `population` vira o total da regiÃ£o e `simulation_population` vira a
		// aglomeraÃ§Ã£o urbana (capada). Sem o bloco (cenÃ¡rio sintÃ©tico), mantÃ©m o
		// comportamento anterior (population = rateio; sim_population = urbano).
		Region region;
		const bool has_region = parse_region(d.get("region", Dictionary()), region);
		if (has_region) {
			city.population = region.population_region;
			city.simulation_population = region.urban_population;
		} else {
			city.population = safe_nonneg(static_cast<double>(d["population"]));
			city.simulation_population = read_sim_population(d, city.population);
		}
		city.center = {static_cast<double>(d["x"]), static_cast<double>(d["y"])};

		// Caixas iniciais por habitante (constantes do modelo demo). A escala Ã©
		// a populaÃ§Ã£o de SIMULAÃ‡ÃƒO: a populaÃ§Ã£o real exibida nÃ£o muda o balanÃ§o.
		city.firm_cash = city.simulation_population * kFirmCashPerCapita;
		city.service_cash = city.simulation_population * kServiceCashPerCapita;
		city.government_budget =
			city.simulation_population * kGovernmentPerCapita;
		city.service_jobs = city.simulation_population * kServiceShare;
		city.service_wage = kServiceWage;
		city.pop.population = city.simulation_population;
		city.pop.savings = city.simulation_population * kSavingsPerCapita;
		city.pop.desired_by_category.assign(ws_.food_products.size(), 0.0);
		city.pop.consumed_by_category.assign(ws_.food_products.size(), 0.0);
		init_pop_needs(city.pop);
		// F7: perfil inicial de trabalho/educaÃ§Ã£o a partir do Ã­ndice de renda
		// (1,0 por padrÃ£o; o GeographyService reaplica quando tem o PIB real).
		apply_labor_profile(city);
		// B3: o bloco da regiÃ£o (estados inteiros) foi parseado acima; semeia a
		// demografia e os quintis do interior e reconcilia a populaÃ§Ã£o exibida.
		city.region = std::move(region);
		if (city.region.ready) {
			sim_core_.seed_region(city);
		}
		// AptidÃ£o padrÃ£o (inicializador do struct: suitability = {1.0, 1.0, 1.0,
		// 0, ...} â€” trigo/ferro/arroz em 1.0). O GeographyService sobrescreve com
		// o dado real em tempo de execuÃ§Ã£o.
		city.suitability[0] = 1.0;
		for (int product = 0; product < ws_.products; ++product) {
			city.markets[product] = {0.0, base_price(product),
				0.0, 0.0};
		}
		// Â§15: estoque inicial de materiais de construÃ§Ã£o por cidade (a "obra em
		// estoque" prÃ©-existente, derivada da populaÃ§Ã£o de simulaÃ§Ã£o). Sem ele a
		// construÃ§Ã£o real (sem produtor de materiais no mundo inicial) travaria
		// no primeiro dia. `derived:initial_stock` â€” nÃ£o Ã© dado observado.
		const int cmat = product_index(String("construction_material"));
		const int csteel = product_index(String("steel_structure"));
		const int celec = product_index(String("electrical_material"));
		const int cmach = product_index(String("industrial_machinery"));
		if (cmat >= 0) {
			city.markets[cmat].stock =
				city.simulation_population * kConstructionMaterialStockPerCapita;
		}
		if (csteel >= 0) {
			city.markets[csteel].stock =
				city.simulation_population * kSteelStructureStockPerCapita;
		}
		if (celec >= 0) {
			city.markets[celec].stock =
				city.simulation_population * kElectricalMaterialStockPerCapita;
		}
		if (cmach >= 0) {
			city.markets[cmach].stock =
				city.simulation_population * kIndustrialMachineryStockPerCapita;
		}
		ws_.cities.push_back(std::move(city));
	}

	// ConstruÃ§Ãµes reais (fazendas primÃ¡rias): o produto vem do campo "resource"
	// (resolvido pelo GeographyService para o alimento bÃ¡sico de maior aptidÃ£o Ã—
	// caloria da naÃ§Ã£o da cidade); ausente, mantÃ©m trigo (comportamento legado).
	// A produÃ§Ã£o efetiva segue multiplicando a capacidade pela aptidÃ£o do produto
	// escolhido (fase 4b usa city.suitability[output_product]).
	for (int index = 0; index < buildings.size(); ++index) {
		const Dictionary d = buildings[index];
		Building building;
		building.id = std::string(
			String(d.get("id", d.get("building_id", ""))).utf8().get_data());
		building.city = std::string(String(d["city_id"]).utf8().get_data());
		building.resource = std::string(
			String(d.get("resource", "wheat")).utf8().get_data());
		const int output_product = product_index(
			String(building.resource.c_str()));
		building.output_product = output_product >= 0 ? output_product : 0;
		building.kind = 0;  // tipo (0=fazenda); o produto fica em output_product
		// D-086: mina/poÃ§o inicial jÃ¡ EXISTENTE. `stage` = -1 no contrato do
		// GeographyService significa "Ãºltimo estÃ¡gio" (o nÃºmero mora aqui, nÃ£o
		// no GDScript): a construÃ§Ã£o histÃ³rica produz no piso (5%).
		// D-086b: `stage >= 0` Ã© a mina semeada PELA PRODUÃ‡ÃƒO REAL de 2024/2025
		// (lista de extraÃ§Ã£o, `extraction_2025.json`) â€” ela nasce operando e a
		// reserva do estÃ¡gio Ã© aberta quando a jazida Ã© declarada
		// (`apply_set_deposit`), porque a semente chega antes do `set_deposit`.
		// Dono padrÃ£o: Estado.
		{
			const int stage_in = int(d.get("stage", 0));
			building.stage = stage_in < 0 ? kMineInitialStage : stage_in;
			building.stage_pending = d.has("stage") && stage_in >= 0;
			building.stage_reserve_kt = 0.0;
			building.stage_mined_kt = 0.0;
		}
		const std::string owner_in = std::string(
			String(d.get("owner", "private")).utf8().get_data());
		building.owner = owner_in == "state" ? "state" : "private";
		building.input_product = -1;
		const double capacity =
			safe_nonneg(static_cast<double>(d["output_tons_per_day"]));
		// AptidÃ£o da cultura escolhida na bacia da cidade (opcional; 1.0 no
		// caminho legado). A capacidade nominal Ã© o teto de terra; a produÃ§Ã£o
		// efetiva da fase 4b Ã© capacidade Ã— aptidÃ£o. Folha e opex acompanham a
		// FRAÃ‡ÃƒO APTA da terra â€” antes seguiam a capacidade nominal inteira e
		// uma fazenda de aptidÃ£o 4% pagava 100% dos custos para produzir 4%,
		// falindo em 60 dias (Â§19). Sem essa correÃ§Ã£o o mundo perdia 83 das 252
		// fazendas iniciais no dia 61.
		const double aptitude = std::clamp(
			safe_nonneg(static_cast<double>(d.get("aptitude", 1.0))), 0.0, 1.0);
		// D-086b: a semente de produÃ§Ã£o real informa a rÃ©gua de engenharia da
		// MINA (a mesma tabela que a IA usa para abrir/expandir mina). Sem os
		// campos, o caminho legado (fazenda: 7,5 empregos/t, 2,5 opex/t) fica
		// intacto â€” fazendas e minas histÃ³ricas nÃ£o mudam. A mina semeada com a
		// rÃ©gua da fazenda pagava ~2-4x a folha de uma mina igual da IA e
		// falia com a venda da cidade limitada pela demanda local.
		const double jobs_per_ton = d.has("jobs_per_ton")
			? safe_nonneg(static_cast<double>(d["jobs_per_ton"])) : kJobsPerTon;
		const double opex_per_ton = d.has("opex_per_ton")
			? safe_nonneg(static_cast<double>(d["opex_per_ton"])) : kOpexPerTon;
		const double wage_in = d.has("wage")
			? safe_nonneg(static_cast<double>(d["wage"])) : kFarmWage;
		building.capacity = capacity;
		building.input_per_output = 0.0;
		building.jobs = capacity * aptitude * jobs_per_ton;
		building.wage = wage_in;
		building.opex = capacity * aptitude * opex_per_ton;
		// D-125: o mundo inicial é o ano-base — a fazenda/mina já existe
		// madura (eficiência 1, trator/máquina ligados). Construção nova via
		// obra/projeto nasce na curva baixa.
		ogs::eff::mature(building, true);
		ws_.buildings.push_back(std::move(building));
	}

	// Portos do cenÃ¡rio (identidade geogrÃ¡fica, para o minimapa e os comboios).
	for (int index = 0; index < ports.size(); ++index) {
		const Dictionary d = ports[index];
		Port port;
		port.id = std::string(String(d["id"]).utf8().get_data());
		port.name = std::string(String(d["name"]).utf8().get_data());
		port.nation = std::string(String(d["nation"]).utf8().get_data());
		port.center = {static_cast<double>(d["x"]), static_cast<double>(d["y"])};
		port.landmass = std::string(String(d.get("landmass", "")).utf8().get_data());
		// Cidade-sede: sÃ³ a Ã¢ncora do porto (nÃ£o Ã© dono exclusivo do trÃ¡fego).
		port.host_city = std::string(String(d.get("port_of", d.get("host_city", "")))
			.utf8().get_data());
		ws_.ports.push_back(std::move(port));
	}

	rebuild_indexes();
	// Capacidade de nÃ­vel 1 depende da populaÃ§Ã£o da cidade-sede: sÃ³ depois do
	// Ã­ndice de cidades (e de os portos existirem).
	init_port_capacities();
	// F4: frota inicial derivada de populaÃ§Ã£o/PIB (source: derived). Os bens da
	// frota sÃ³ resolvem depois de set_goods; aqui as contagens/estoques jÃ¡
	// existem e os Ã­ndices sÃ£o atualizados quando os bens chegam.
	init_fleets();
	// F8: serviÃ§os â€” catÃ¡logo jÃ¡ carregado pelo autoload/teste; os negÃ³cios
	// iniciais saem do caixa do prÃ³prio setor (capital fica em service_cash).
	seed_services();
	// F9: grupos/empresas a partir dos ativos privados existentes.
	seed_groups();
}

// ---------------------------------------------------------------------------
// F4 â€” frota nacional e combustÃ­vel.
//
// Contrato: cada trecho de um pedido exige (a) veÃ­culo livre da naÃ§Ã£o
// VENDEDORA na capacidade do ModeSpec (caminhÃ£o 25 t, navio 2.000 t, aviÃ£o 20 t)
// e (b) combustÃ­vel no estoque nacional, consumido por tÂ·km. Sem os dois, a
// carga nÃ£o embarca; a diferenÃ§a entra em `queue_today` e o governo repÃµe frota
// e combustÃ­vel na manutenÃ§Ã£o mensal. NÃ£o hÃ¡ nÃºmero fora do dado: a frota
// inicial Ã© derivada de populaÃ§Ã£o/PIB (`source: derived`), o combustÃ­vel Ã© um
// bem fÃ­sico (fuel, order 47) e os veÃ­culos sÃ£o os bens finais da F2
// (truck/cargo_ship/cargo_plane).
// ---------------------------------------------------------------------------

Dictionary EconomySim::get_fleet(const String& nation) const {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Dictionary out;
	const std::string tag(nation.utf8().get_data());
	const auto found = ws_.fleets.find(tag);
	if (found == ws_.fleets.end()) {
		out["error"] = "nacao sem frota";
		return out;
	}
	const Fleet& fleet = found->second;
	out["nation"] = nation;
	out["trucks"] = fleet.trucks;
	out["ships"] = fleet.ships;
	out["planes"] = fleet.planes;
	out["trucks_busy"] = fleet.trucks_busy;
	out["ships_busy"] = fleet.ships_busy;
	out["planes_busy"] = fleet.planes_busy;
	out["health"] = Array{fleet.health[0], fleet.health[1], fleet.health[2]};
	// Capacidade efetiva de um lote por modal (t) â€” veÃ­culos Ã— saÃºde Ã— carga.
	out["capacity_tons"] = fleet.trucks * fleet.health[0] * 25.0
		+ fleet.ships * fleet.health[1] * 2000.0
		+ fleet.planes * fleet.health[2] * 20.0;
	// F0: estoque e alvo por grau. Os totais `fuel_*` continuam no contrato
	// (soma dos dois graus) para auditoria e testes de conservacao.
	out["diesel"] = fleet.diesel;
	out["diesel_target"] = fleet.diesel_target;
	out["jet_fuel"] = fleet.jet_fuel;
	out["jet_target"] = fleet.jet_target;
	out["spare_parts"] = fleet.spare_parts;
	out["fuel_used_today"] = fleet.fuel_used_today;
	out["fuel_used_total"] = fleet.fuel_used_total;
	out["diesel_used_today"] = fleet.diesel_used_today;
	out["diesel_used_total"] = fleet.diesel_used_total;
	out["jet_used_today"] = fleet.jet_used_today;
	out["jet_used_total"] = fleet.jet_used_total;
	out["fuel_bought_total"] = fleet.fuel_bought_total;
	out["diesel_bought_total"] = fleet.diesel_bought_total;
	out["jet_bought_total"] = fleet.jet_bought_total;
	out["parts_used_total"] = fleet.parts_used_total;
	out["vehicles_bought_total"] = fleet.vehicles_bought_total;
	out["queue_today"] = fleet.queue_today;
	out["missed_no_fleet"] = fleet.missed_no_fleet;
	out["missed_no_fuel"] = fleet.missed_no_fuel;
	out["derived"] = fleet.derived;
	out["source"] = fleet.derived ? "derived" : "declared";
	out["target_trucks"] = fleet.target_trucks;
	out["target_ships"] = fleet.target_ships;
	out["target_planes"] = fleet.target_planes;
	return out;
}

void EconomySim::set_fleet(const String& nation, double trucks, double ships,
		double planes, double diesel, double jet_fuel, double spare_parts) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const std::string tag(nation.utf8().get_data());
	Fleet fleet = Fleet{};
	fleet.trucks = std::max(0.0, trucks);
	fleet.ships = std::max(0.0, ships);
	fleet.planes = std::max(0.0, planes);
	fleet.target_trucks = fleet.trucks;
	fleet.target_ships = fleet.ships;
	fleet.target_planes = fleet.planes;
	fleet.health[0] = 1.0;
	fleet.health[1] = 1.0;
	fleet.health[2] = 1.0;
	fleet.diesel = std::max(0.0, diesel);
	fleet.diesel_target = fleet.diesel;
	fleet.jet_fuel = std::max(0.0, jet_fuel);
	fleet.jet_target = fleet.jet_fuel;
	fleet.spare_parts = std::max(0.0, spare_parts);
	fleet.derived = false;
	ws_.fleets[tag] = fleet;
	bool known = false;
	for (const std::string& entry : ws_.fleet_order) {
		if (entry == tag) {
			known = true;
			break;
		}
	}
	if (!known) {
		ws_.fleet_order.push_back(tag);
	}
	status_invalidate();
}

Dictionary EconomySim::apply_fleet_buy(const String& nation, const String& product,
		double amount, double budget_share) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Dictionary result;
	result["bought"] = 0.0;
	result["spent"] = 0.0;
	result["unfilled"] = 0.0;
	result["error"] = "";
	const std::string tag(nation.utf8().get_data());
	const int product_slot = product_index(product);
	if (product_slot < 0) {
		result["error"] = "produto desconhecido";
		return result;
	}
	if (amount <= 1e-9) {
		result["error"] = "quantidade invalida";
		return result;
	}
	const auto fleet_found = ws_.fleets.find(tag);
	if (fleet_found == ws_.fleets.end()) {
		result["error"] = "nacao sem frota";
		return result;
	}
	const auto cities_found = ws_.cities_by_nation.find(tag);
	if (cities_found == ws_.cities_by_nation.end() || cities_found->second.empty()) {
		result["error"] = "nacao sem cidade";
		return result;
	}
	double spent = 0.0;
	double unfilled = 0.0;
	const double bought = sim_core_.fleet_buy_core(tag, product_slot, amount,
		budget_share, spent, unfilled);
	result["bought"] = bought;
	result["spent"] = spent;
	result["unfilled"] = unfilled;
	return result;
}

void EconomySim::apply_set_market_stock(const String& city_id, const String& product,
		double stock) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	City* city = find_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr) {
		return;
	}
	const int product_slot = product_index(product);
	if (product_slot < 0) {
		return;
	}
	city->markets[product_slot].stock = std::max(0.0, stock);
	++status_static_version_;
}

void EconomySim::apply_set_market_price(const String& city_id,
		const String& product, double price) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	City* city = find_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr || !std::isfinite(price)) {
		return;
	}
	const int product_slot = product_index(product);
	if (product_slot < 0) {
		return;
	}
	const double base = base_price(product_slot);
	city->markets[product_slot].price = std::clamp(
		std::max(1e-6, price), base * 0.1, base * 10.0);
}

// ---------------------------------------------------------------------------
// D-125 — eficiência de produção: aplicação dos comandos (quem aplica é o
// escritor único; o mutex recursivo aqui é o mesmo padrão dos demais apply_*).
// ---------------------------------------------------------------------------
Dictionary EconomySim::apply_change_product(const String& city_id,
		const String& building_id, const String& product,
		const String& recipe_id) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const ogs::ChangeProductResult outcome = ogs::action_change_product(ws_,
		sim_core_, std::string(city_id.utf8().get_data()),
		std::string(building_id.utf8().get_data()),
		std::string(product.utf8().get_data()),
		std::string(recipe_id.utf8().get_data()));
	Dictionary result;
	result["changed"] = outcome.changed;
	result["efficiency"] = outcome.efficiency;
	result["transition_days"] = outcome.transition_days;
	result["error"] = String(outcome.error.c_str());
	return result;
}

Dictionary EconomySim::apply_set_refinery_mode(const String& city_id,
		const String& building_id, const String& mode) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const ogs::RefineryModeResult outcome = ogs::action_set_refinery_mode(ws_,
		sim_core_, std::string(city_id.utf8().get_data()),
		std::string(building_id.utf8().get_data()),
		std::string(mode.utf8().get_data()));
	Dictionary result;
	result["changed"] = outcome.changed;
	result["mode"] = outcome.mode;
	result["transition_days"] = outcome.transition_days;
	result["error"] = String(outcome.error.c_str());
	return result;
}

Dictionary EconomySim::apply_set_building_equipment(const String& city_id,
		const String& building_id, double level) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const ogs::EquipmentResult outcome = ogs::action_set_building_equipment(ws_,
		sim_core_, std::string(city_id.utf8().get_data()),
		std::string(building_id.utf8().get_data()), level);
	Dictionary result;
	result["set"] = outcome.set;
	result["level"] = outcome.level;
	result["error"] = String(outcome.error.c_str());
	return result;
}

Dictionary EconomySim::apply_expand_port(const String& port_id, double t_per_day) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Dictionary result;
	result["expanded"] = false;
	result["cost"] = 0.0;
	result["days"] = 0;
	result["capacity"] = 0.0;
	result["error"] = "";

	Port* port = find_port(std::string(port_id.utf8().get_data()));
	if (port == nullptr) {
		result["error"] = "porto desconhecido";
		return result;
	}
	result["capacity"] = port->capacity_t_per_day;
	const double add = safe_nonneg(t_per_day);
	if (add <= 1e-9) {
		result["error"] = "capacidade invalida";
		return result;
	}

	// Obra pÃºblica: paga o orÃ§amento da cidade-sede e credita o setor de
	// construÃ§Ã£o (dinheiro conservado).
	int payer_index = -1;
	if (!port->host_city.empty()) {
		const auto found = ws_.city_lookup.find(port->host_city);
		if (found != ws_.city_lookup.end()) {
			payer_index = found->second;
		}
	}
	if (payer_index < 0) {
		payer_index = city_index_near(port->nation, port->landmass, port->center);
	}
	if (payer_index < 0) {
		result["error"] = "cidade-sede ausente";
		return result;
	}
	City& payer = ws_.cities[static_cast<std::size_t>(payer_index)];
	const double cost = add * kPortCapitalPerTonCapacity;
	if (payer.government_budget + 1e-9 < cost) {
		result["error"] = "orcamento insuficiente";
		return result;
	}
	payer.government_budget -= cost;
	ws_.construction_cash += cost;
	port->pending_capacity_t_per_day += add;
	port->pending_days = std::max(port->pending_days, kPortBuildDays);
	result["expanded"] = true;
	result["cost"] = cost;
	result["days"] = port->pending_days;
	return result;
}

void EconomySim::apply_set_port_capacity(const String& port_id, double t_per_day) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Port* port = find_port(std::string(port_id.utf8().get_data()));
	if (port == nullptr) {
		return;
	}
	port->base_capacity_t_per_day = safe_nonneg(t_per_day);
	port->capacity_t_per_day = port->base_capacity_t_per_day;
}

void EconomySim::set_instant_construction(bool instant) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	instant_construction_ = instant;
}

void EconomySim::set_ai_enabled(bool enabled) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	// SÃ³ a flag: o estado entre dias da IA pertence ao MUNDO (resetado em
	// initialize_*/from_snapshot*, preservado no load por from_dict).
	ai_enabled_ = enabled;
}

Dictionary EconomySim::tick_day() {
	tick_and_publish();
	// A4: tools/testes/goldens recebem o status completo SOB PEDIDO. Este
	// caminho mantÃ©m a cadÃªncia por slot da S17 exatamente como antes (o
	// primeiro build de um slot calcula o dia corrente) e desliga de vez a
	// semeadura pelo retrato semanal. O jogo usa `tick_and_publish` +
	// `get_day_summary` e nunca chega aqui; o agendador de teste, que sÃ³ chama
	// `tick_and_publish`, mantÃ©m a semeadura.
	status_seed_labor_from_snapshot_ = false;
	return get_status();
}

void EconomySim::tick_and_publish() {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	// A3: enquanto o dia roda, quem muta o mundo sÃ³ enfileira (dispatch_command
	// vÃª a flag e nÃ£o toca no `ws_`). A flag Ã© limpa na saÃ­da, inclusive quando
	// o dia sai por exceÃ§Ã£o.
	struct TickGuard {
		std::atomic<bool>& flag;
		std::atomic<std::size_t>& thread;
		TickGuard(std::atomic<bool>& value, std::atomic<std::size_t>& owner) :
				flag(value), thread(owner) {
			thread.store(current_thread_hash(), std::memory_order_release);
			flag.store(true, std::memory_order_release);
		}
		~TickGuard() {
			flag.store(false, std::memory_order_release);
			thread.store(0, std::memory_order_release);
		}
	} tick_guard(tick_in_progress_, tick_thread_);
#if defined(OGS_PROFILING)
	// Sink das fases do tick: os 15 slots de `profile_us_`, com o toggle
	// `profile_` como interruptor (desligado, nenhum relÃ³gio Ã© lido).
	ogs::profile::Sink phase_sink{profile_us_, kPhaseCount, &profile_};
	const ogs::profile::SinkScope phase_scope(&phase_sink);
#endif
#if defined(OGS_PROFILING)
	const auto tick_perf_start = std::chrono::steady_clock::now();
	auto tick_perf_mark = tick_perf_start;
	const auto perf_mark = [&tick_perf_mark]() {
		const auto now = std::chrono::steady_clock::now();
		const double us = std::chrono::duration<double, std::micro>(
			now - tick_perf_mark).count();
		tick_perf_mark = now;
		return us;
	};
#endif
	tick_core();
#if defined(OGS_PROFILING)
	OGS_SIM_COUNTER("tick_us_core", perf_mark());
#endif
	if (ai_enabled_) {
		// A5/D-076: fase da IA no FIM do dia, jÃ¡ com o dia fechado (`ws_.day` Ã©
		// o dia que acabou de rodar) e ANTES da publicaÃ§Ã£o do status. A IA lÃª
		// um retrato prÃ³prio (`AiView`), nÃ£o o status, entÃ£o a ordem nÃ£o muda
		// nenhuma decisÃ£o; e cada comando emitido Ã© aplicado na hora pela
		// prÃ³pria fase (ai_emit_*), entÃ£o o status abaixo jÃ¡ sai com os
		// efeitos do dia. Rodar a IA depois do status obrigava a invalidÃ¡-lo e
		// a construÃ­-lo DUAS vezes por dia (~20 ms), e a segunda construÃ§Ã£o
		// lia o cache semanal do slot anterior â€” publicando `nations[].labor`
		// com atÃ© 13 dias de atraso.
		OGS_SIM_ZONE(kPhaseAi);
		ai_phase_.run(ws_, sim_core_, commands_, instant_construction_,
			bootstrap_construction_);
	}
#if defined(OGS_PROFILING)
	OGS_SIM_COUNTER("tick_us_ai", perf_mark());
#endif
	{
		// Fase `status` (RAII). A4: o dia NÃƒO monta mais o Dictionary do mundo
		// inteiro. No fecho semanal captura-se o retrato de trabalho das naÃ§Ãµes
		// (S17), em doubles puros; o resumo leve do dia Ã© montado sob demanda
		// por `get_day_summary()` depois do join (main thread).
		OGS_SIM_ZONE(kPhaseStatus);
		if ((ws_.day % 7) == 0 || labor_snapshot_day_ < 0) {
			capture_nation_labor();
		}
	}
	current_day_.store(ws_.day, std::memory_order_relaxed);
	++data_version_;
#if defined(OGS_PROFILING)
	OGS_SIM_COUNTER("tick_us_status", perf_mark());
	const double tick_total_us =
		std::chrono::duration<double, std::micro>(
			std::chrono::steady_clock::now() - tick_perf_start).count();
	OGS_SIM_COUNTER("tick_us_total", tick_total_us);
#endif
}

void EconomySim::status_invalidate() {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	status_built_day_ = -1;
	++data_version_;
	// A4: as consultas cacheadas por dia tambÃ©m ficam velhas.
	columns_day_ = -1;
	query_rows_day_ = -1;
	query_light_day_ = -1;
	query_orders_day_ = -1;
}

int EconomySim::get_data_version() const {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	return data_version_;
}

int EconomySim::get_save_version() const {
	return ogs::kSaveVersion;
}

void EconomySim::set_profiling(bool enabled) {
	profile_ = enabled;
	if (!enabled) {
		for (double& value : profile_us_) {
			value = 0.0;
		}
	}
}

void EconomySim::set_sim_threads(int threads) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	sim_core_.set_sim_threads(threads);
}

Dictionary EconomySim::get_profile() const {
	static const char* kNames[kPhaseCount] = {
		"arrival", "reset", "demography", "labor", "production", "raw",
		"income", "services",
		"trade", "accounting", "prices", "clamp", "fiscal", "fx", "invariants",
		"status",
		"trade_food", "trade_food_buyers",
		"trade_goods", "trade_goods_loop",
		"prologue_snapshot", "prologue_indexes", "prologue_other",
		"arrival_transit",
		"fiscal_city_gov", "fiscal_equalize", "fiscal_groups",
		"trade_food_prep", "trade_food_calc", "trade_food_settle",
		"trade_goods_cand", "trade_goods_offers", "trade_goods_settle",
		"housing", "land_month", "fleet_day", "services_decisions", "ai", "ai_view",
		"region", "migration", "social", "property", "agriculture", "storage",
		"stock", "bank_crisis", "crisis", "power", "power_settle",
		// G2 perf rodada 3: subdivisão da fase `ai` (só medição).
		"ai_floor", "ai_prepare", "ai_supply_cache", "ai_exposure_cache",
		"ai_groups", "ai_private", "ai_gov", "ai_storage", "ai_refinery",
		"ai_loss_scan", "ai_forecast",
	};
	Dictionary profile;
	if (!profile_) {
		return profile;
	}
	for (int phase = 0; phase < kPhaseCount; ++phase) {
		profile[kNames[phase]] = profile_us_[phase];
	}
	static const char* kStatusNames[4] = {
		"status_cities", "status_orders", "status_nations", "status_misc",
	};
	for (int part = 0; part < 4; ++part) {
		profile[kStatusNames[part]] = status_sub_us_[part];
	}
#if defined(OGS_PROFILING)
	// G2 perf rodada 3: contadores não-temporais (células do piso/dia,
	// candidatos, forecasts). O último valor do dia; só medição.
	for (const ogs::profile::CounterSample& sample :
			ogs::profile::counter_samples()) {
		if (sample.name != nullptr) {
			profile[String("counter_") + String(sample.name)] = sample.value;
		}
	}
#endif
	return profile;
}

// PrÃ©-aquece os dois slots do status. O primeiro dia publicado em um slot
// frio paga a criaÃ§Ã£o de TODOS os dicionÃ¡rios por cidade (medido no cenÃ¡rio
// real: 60 ms contra 17 ms num dia quente). Aqui isso vira custo de carga, e
// nenhum tick paga a alocaÃ§Ã£o. SÃ³ roda com o mundo jÃ¡ preenchido (aptidÃ£o,
// demanda externa e Ã­ndice de renda aplicados), senÃ£o o bloco estÃ¡tico do
// status sairia com os valores default.
void EconomySim::warm_status() {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	if (!ws_.active) {
		return;
	}
	get_status();  // preenche o slot corrente
	if (status_live_ < 0) {
		return;
	}
	// `built_day_ = -1` faz a prÃ³xima chamada construir no OUTRO slot.
	status_built_day_ = -1;
	get_status();
}

void EconomySim::tick_core() {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);

	if (!ws_.active) {
		initialize_scenario();
	}
	// A3: o dia comeÃ§a aplicando a fila de comandos (ordem de emissÃ£o), antes
	// do prÃ³logo e de qualquer fase. Comandos emitidos com o dia em andamento
	// (outra thread) chegam aqui; os emitidos entre dias jÃ¡ foram aplicados
	// pelo dispatch e a fila estÃ¡ vazia.
	apply_pending_locked(0);
	current_day_.store(ws_.day, std::memory_order_relaxed);
	// Â§12.3: se o mundo real ainda nÃ£o tem indÃºstria, semeia poucas plantas de
	// referÃªncia (moinho + padaria) antes de contar dinheiro/invariantes.
	seed_industry();

	// A ordem do dia (prÃ³logo + fases) Ã© da biblioteca (sim/sim_day.cpp): a
	// mesma sequÃªncia roda sem o Godot. Aqui ficam sÃ³ a inicializaÃ§Ã£o acima e o
	// log dos invariantes abaixo.
	ogs::tick_day(ws_, sim_core_);
	if (!ws_.invariant_violations.empty()) {
		for (const std::string& violation : ws_.invariant_violations) {
			UtilityFunctions::push_error(
				String(("economia.invariante: " + violation).c_str()));
		}
	}
}

EconomySim::StatusKeys::StatusKeys() :
		age_bands("age_bands"),
		amount("amount"),
		bank_money_delta_total("bank_money_delta_total"),
		bankruptcies("bankruptcies"),
		buildings("buildings"),
		calendar_day("calendar_day"),
		capacity("capacity"),
		capital_cost_per_capacity("capital_cost_per_capacity"),
		capital_income("capital_income"),
		center("center"),
		cities("cities"),
		city_count("city_count"),
		construction_cash("construction_cash"),
		construction_revenue("construction_revenue"),
		consumed("consumed"),
		consumed_by_category("consumed_by_category"),
		consumed_today("consumed_today"),
		consumed_total("consumed_total"),
		costs("costs"),
		day("day"),
		deficit("deficit"),
		demand("demand"),
		dependents("dependents"),
		depleted("depleted"),
		depleted_mines("depleted_mines"),
		deposits("deposits"),
		depth_m("depth_m"),
		desired("desired"),
		desired_by_category("desired_by_category"),
		disposable("disposable"),
		disposable_per_capita("disposable_per_capita"),
		education("education"),
		education_share("education_share"),
		efficiency("efficiency"),
		employed("employed"),
		employed_by_level("employed_by_level"),
		export_value("export_value"),
		exported("exported"),
		external_demand("external_demand"),
		external_income("external_income"),
		external_income_index("external_income_index"),
		extracted_kt("extracted_kt"),
		firm_cash("firm_cash"),
		food_spend("food_spend"),
		foreign_cash("foreign_cash"),
		freight_cost("freight_cost"),
		freight_revenue("freight_revenue"),
		from_city("from_city"),
		from_nation("from_nation"),
		gdp("gdp"),
		gdp_per_capita("gdp_per_capita"),
		good("good"),
		government_budget("government_budget"),
		home_nation("home_nation"),
		id("id"),
		import_value("import_value"),
		imported("imported"),
		in_transit("in_transit"),
		income("income"),
		income_level("income_level"),
		income_level_share("income_level_share"),
		income_level_by_stratum("income_level_by_stratum"),
		income_strata("income_strata"),
		stratum_disposable("stratum_disposable"),
		stratum_population("stratum_population"),
		stratum_wage_income("stratum_wage_income"),
		industrialization("industrialization"),
		initial_kt("initial_kt"),
		input_product("input_product"),
		input_value("input_value"),
		invariant_violations("invariant_violations"),
		jobs("jobs"),
		kind("kind"),
		labor("labor"),
		labor_force("labor_force"),
		labor_force_by_level("labor_force_by_level"),
		landmass("landmass"),
		legs("legs"),
		living_standard("living_standard"),
		loss_days("loss_days"),
		market_totals("market_totals"),
		markets("markets"),
		migration("migration"),
		minimum_wage("minimum_wage"),
		mode("mode"),
		money_total("money_total"),
		name("name"),
		nation("nation"),
		nations("nations"),
		need("need"),
		opex("opex"),
		opex_factor("opex_factor"),
		opex_revenue("opex_revenue"),
		output_product("output_product"),
		output_scale("output_scale"),
		owner("owner"),
		pop("pop"),
		population("population"),
		ports("ports"),
		price("price"),
		price_local("price_local"),
		produced_today("produced_today"),
		produced_total("produced_total"),
		productivity_index("productivity_index"),
		product("product"),
		production("production"),
		products("products"),
		profit("profit"),
		quality("quality"),
		recruit_reserve("recruit_reserve"),
		remaining_kt("remaining_kt"),
		resource("resource"),
		retirees("retirees"),
		revenue("revenue"),
		satisfaction("satisfaction"),
		savings("savings"),
		scope("scope"),
		service_cash("service_cash"),
		service_jobs("service_jobs"),
		services("services"),
		service_sectors("service_sectors"),
		service_businesses("service_businesses"),
		service_bankruptcies("service_bankruptcies"),
		service_exports("service_exports"),
		service_profit("service_profit"),
		service_revenue("service_revenue"),
		service_spend("service_spend"),
		service_wage("service_wage"),
		service_workers("service_workers"),
		service_jobs_filled("service_jobs_filled"),
		sector_workers("sector_workers"),
		sector_share("sector_share"),
		in_kind_food("in_kind_food"),
		shortage("shortage"),
		simulation_population("simulation_population"),
		site_suitability("site_suitability"),
		sold("sold"),
		stock("stock"),
		stockpile("stockpile"),
		students("students"),
		studying("studying"),
		suitability("suitability"),
		supply("supply"),
		surplus("surplus"),
		tag("tag"),
		taxes("taxes"),
		to_city("to_city"),
		to_nation("to_nation"),
		trade_orders("trade_orders"),
		trade_orders_count("trade_orders_count"),
		transfers("transfers"),
		transit_hours("transit_hours"),
		transport_cash("transport_cash"),
		unemployed("unemployed"),
		unemployed_by_level("unemployed_by_level"),
		unemployment_by_level("unemployment_by_level"),
		utilization("utilization"),
		vacancies_by_level("vacancies_by_level"),
		wage("wage"),
		wage_by_level("wage_by_level"),
		wage_income("wage_income"),
		wage_income_by_level("wage_income_by_level"),
		wage_rigidity("wage_rigidity"),
		workers("workers"),
		working_age("working_age"),
		x("x"),
		y("y"),
		host_city("host_city"),
		capacity_t_per_day("capacity_t_per_day"),
		delay_hours("delay_hours"),
		level("level"),
		queue("queue"),
		throughput("throughput"),
		recipe("recipe"),
		inputs("inputs"),
		byproducts("byproducts"),
		qty("qty"),
		capital_per_capacity("capital_per_capacity"),
		plant_class("plant_class"),
		last_utilization("last_utilization"),
		strategic("strategic"),
		yield("yield"),
		from("from"),
		to("to"),
		from_px("from_px"),
		to_px("to_px"),
		distance_km("distance_km"),
		load_tons("load_tons"),
		trips("trips"),
		speed_kmh("speed_kmh"),
		batch_capacity_tons("batch_capacity_tons"),
		dispatched("dispatched"),
		cost("cost"),
		needs_level("needs_level"),
		needs_satisfaction("needs_satisfaction"),
		basic_satisfaction("basic_satisfaction"),
		life_satisfaction("life_satisfaction"),
		luxury_satisfaction("luxury_satisfaction"),
		needs_spend("needs_spend"),
		durable_spend("durable_spend"),
		durable_stock("durable_stock"),
		government_goods_spend("government_goods_spend"),
		trucks("trucks"),
		ships("ships"),
		planes("planes"),
		fleet("fleet"),
		fuel("fuel"),
		diesel("diesel"),
		diesel_target("diesel_target"),
		jet_fuel("jet_fuel"),
		jet_target("jet_target"),
		diesel_used("diesel_used"),
		jet_used("jet_used"),
		spare_parts("spare_parts"),
		health("health"),
		fuel_used("fuel_used"),
		missed_no_fleet("missed_no_fleet"),
		missed_no_fuel("missed_no_fuel"),
		source("source"),
		target_trucks("target_trucks"),
		target_ships("target_ships"),
		target_planes("target_planes"),
		currency("currency"),
		currencies("currencies"),
		code("code"),
		rate("rate"),
		regime("regime"),
		trade_balance("trade_balance"),
		fallback("fallback"),
		tariff("tariff"),
		tariffs("tariffs"),
		seller("seller"),
		buyer("buyer"),
		origin_price("origin_price"),
		origin_price_anchor("origin_price_anchor"),
		delivered_price("delivered_price"),
		fx_rate("fx_rate"),
		seller_currency("seller_currency"),
		buyer_currency("buyer_currency"),
		seller_fx_rate("seller_fx_rate"),
		tariff_revenue("tariff_revenue"),
		owner_kind("owner_kind"),
		owner_group("owner_group"),
		owner_nation("owner_nation"),
		local_share("local_share"),
		groups("groups"),
		// D-086: estÃ¡gio de mina.
		stage("stage"),
		stage_factor("stage_factor"),
		stage_reserve_kt("stage_reserve_kt"),
		stage_mined_kt("stage_mined_kt"),
		next_deepen_cost("next_deepen_cost"),
		production_efficiency("production_efficiency"),
		efficiency_days("efficiency_days"),
		transition_days("transition_days"),
		equipment("equipment"),
		refinery_mode("refinery_mode"),
		light("light"),
		product_count("product_count"),
		m_price("m_price"),
		m_stock("m_stock"),
		m_supply("m_supply"),
		m_surplus("m_surplus"),
		m_sold("m_sold"),
		m_shortage("m_shortage"),
		m_desired("m_desired"),
		p_need("p_need"),
		p_consumed("p_consumed"),
		p_satisfaction("p_satisfaction"),
		p_labor_force("p_labor_force"),
		p_unemployed("p_unemployed"),
		i_city("i_city"),
		i_resource("i_resource"),
		i_owner_group("i_owner_group"),
		b_offset("b_offset"),
		b_id("b_id"),
		b_output_product("b_output_product"),
		b_owner("b_owner"),
		b_capacity("b_capacity"),
		b_utilization("b_utilization"),
		b_costs("b_costs"),
		b_site_suitability("b_site_suitability"),
		d_offset("d_offset"),
		d_resource("d_resource"),
		d_product("d_product"),
		d_initial_kt("d_initial_kt"),
		d_remaining_kt("d_remaining_kt"),
		d_quality("d_quality"),
		d_depth_m("d_depth_m"),
		d_efficiency("d_efficiency"),
		d_opex_factor("d_opex_factor"),
		d_extracted_kt("d_extracted_kt"),
		d_depleted("d_depleted"),
		c_offset("c_offset"),
		c_output_product("c_output_product"),
		c_capacity("c_capacity"),
		c_expand("c_expand"),
		wealth_gini("wealth_gini")
{}

EconomySim::CityScalars EconomySim::city_scalars(std::size_t city_index,
		double in_transit) const {
	const City& city = ws_.cities[city_index];
	// A4: a MESMA conta que o bloco leve do status fazia inline, extraÃ­da para
	// a ficha (`get_city_info`) e as colunas (`get_city_columns`) nÃ£o
	// divergirem. `population` = populacao real exibida; `simulation_population`
	// = unidade de simulacao.
	CityScalars s;
	s.population = city.population;
	s.simulation_population = city.simulation_population;
	s.gdp = city.gdp;
	s.gdp_per_capita = city.simulation_population > 0.0
		? city.gdp / city.simulation_population : 0.0;
	s.external_income_index = city.external_income_index;
	s.firm_cash = city.firm_cash;
	s.service_cash = city.service_cash;
	s.government_budget = city.government_budget;
	s.government_goods_spend = city.government_goods_spend;
	s.service_revenue = city.service_revenue;
	s.service_jobs = city.service_jobs;
	s.service_workers = city.service_workers;
	s.service_jobs_filled = city.service_jobs_filled;
	s.in_kind_food = city.in_kind_food;
	s.service_profit = city.service_profit;
	s.opex_revenue = city.opex_revenue;
	s.freight_revenue = city.freight_revenue;
	s.construction_revenue = city.construction_revenue;
	s.service_wage = city.service_jobs > 0.0
		? city.service_wage_bill / city.service_jobs : 0.0;
	s.disposable_per_capita = city.simulation_population > 0.0
		? city.pop.disposable / city.simulation_population : 0.0;

	// PadrÃ£o de vida: quantas cestas alimentares diÃ¡rias a renda disponÃ­vel
	// por pessoa compra. Diferente do PIB per capita, usa renda das famÃ­lias.
	double cheapest_price = 0.0;
	for (int food = 0; food < static_cast<int>(ws_.food_products.size()); ++food) {
		const int product = food_product(food);
		const double factor = food_calories(product);
		if (factor <= 0.0) {
			continue;
		}
		const double per_calorie = city.markets[product].price / factor;
		if (per_calorie > 0.0
				&& (cheapest_price <= 0.0 || per_calorie < cheapest_price)) {
			cheapest_price = per_calorie;
		}
	}
	const double basket_cost = kNeedPerCapita * cheapest_price;
	s.living_standard = basket_cost > 0.0 && city.simulation_population > 0.0
		? (city.pop.disposable / city.simulation_population) / basket_cost : 0.0;

	// Indicadores derivados, no espÃ­rito do formato legado, para a camada
	// visual: produÃ§Ã£o primÃ¡ria (t/dia), empregos, renda relativa, grau de
	// industrializaÃ§Ã£o, estoque de trigo e necessidade diÃ¡ria.
	double production = 0.0;
	double secondary_capacity = 0.0;
	double total_capacity = 0.0;
	for (const int bi : ws_.buildings_by_city[city_index]) {
		const Building& building = ws_.buildings[static_cast<std::size_t>(bi)];
		total_capacity += building.capacity * building.output_scale;
		if (building.input_product < 0) {
			production += building.capacity * building.output_scale;
		} else {
			secondary_capacity += building.capacity * building.output_scale;
		}
	}
	s.production = production;
	s.jobs = city.pop.employed;
	s.income = std::clamp(s.living_standard, 0.0, 1.0);
	s.industrialization = total_capacity > 0.0
		? std::clamp(secondary_capacity / total_capacity, 0.0, 1.0) : 0.0;
	s.stockpile = city.markets[0].stock;
	s.need = city.pop.need;
	s.deficit = std::max(0.0, city.pop.need - city.pop.consumed);
	s.consumed = city.pop.consumed;
	s.in_transit = in_transit;
	s.fx_rate = fx_rate(city.nation);
	s.tariff_revenue = city.tariff_revenue;
	s.productivity_index = effective_productivity(city);
	return s;
}

void EconomySim::compute_in_transit(std::vector<double>& out) const {
	// A4: a mesma passada Ãºnica do status, reusada pelas consultas.
	out.assign(ws_.cities.size(), 0.0);
	for (const Shipment& shipment : ws_.in_transit) {
		const auto found = ws_.city_lookup.find(shipment.to_city);
		if (found != ws_.city_lookup.end()) {
			out[static_cast<std::size_t>(found->second)] += shipment.amount;
		}
	}
}

Dictionary EconomySim::get_city_status(const String& city_id) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Dictionary result;
	const std::string id = std::string(city_id.utf8().get_data());
	const auto found = ws_.city_lookup.find(id);
	if (found == ws_.city_lookup.end()) {
		return result;
	}
	const std::size_t city_index = static_cast<std::size_t>(found->second);

	// Carga em trÃ¢nsito PARA esta cidade (o status diÃ¡rio calcula a mesma soma
	// numa passada Ãºnica; aqui Ã© sÃ³ uma cidade, entÃ£o varre as cargas do mundo).
	double in_transit = 0.0;
	for (const Shipment& shipment : ws_.in_transit) {
		if (shipment.to_city == id) {
			in_transit += shipment.amount;
		}
	}

	// Slot frio (nada preenchido): `fill_city_status` escreve todos os
	// campos â€” mesmo conteÃºdo que o bloco por cidade do status diÃ¡rio, mais o
	// detalhe pesado (markets/pop/services/buildings/deposits) sob demanda.
	StatusCitySlot slot;
	fill_city_status(city_index, slot, in_transit, true);
	return slot.dict;
}


Dictionary EconomySim::get_day_summary() {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const StatusKeys& k = status_keys_;
	// A4: resumo leve, construÃ­do do zero a cada chamada (â‰ˆ20 chaves) para o
	// dicionÃ¡rio entregue Ã  UI ser imutÃ¡vel â€” o tick roda na worker thread e o
	// consumidor nÃ£o pode ver um dicionÃ¡rio compartilhado sendo reescrito.
	Dictionary root;
	root[k.mode] = "demo";
	root[k.day] = ws_.day;
	root[k.calendar_day] = ws_.day;
	// G2: diagnóstico da semente (critério 9: residual e falta publicados).
	if (ws_.bootstrap.loaded) {
		Dictionary bootstrap;
		bootstrap["enabled"] = ws_.bootstrap.enabled;
		bootstrap["plants"] = ws_.bootstrap_plants;
		bootstrap["shortfall"] = ws_.bootstrap_shortfall;
		bootstrap["unseeded_share"] = ws_.bootstrap_unseeded_share;
		root["bootstrap"] = bootstrap;
	}
	root[k.money_total] = money_total();
	// D-126: dinheiro bancário criado/destruído (M1) acumulado nos dias válidos.
	// `money_total` pode crescer por crédito legítimo; os guardas de conservação
	// somam este contador ao total inicial em vez de exigir constância.
	root[k.bank_money_delta_total] = ws_.bank_money_delta_total;
	root[k.transport_cash] = ws_.transport_cash;
	root[k.foreign_cash] = ws_.foreign_cash;
	root[k.construction_cash] = ws_.construction_cash;
	root[k.bankruptcies] = ws_.bankruptcies;
	root[k.depleted_mines] = ws_.depleted_mines;
	root[k.service_businesses] = get_service_business_count();
	root[k.service_bankruptcies] = ws_.service_bankruptcies;
	Array violations;
	violations.resize(static_cast<int>(ws_.invariant_violations.size()));
	for (std::size_t index = 0; index < ws_.invariant_violations.size(); ++index) {
		violations[static_cast<int>(index)] =
			String(ws_.invariant_violations[index].c_str());
	}
	root[k.invariant_violations] = violations;
	Array products;
	products.resize(ws_.products);
	for (int index = 0; index < ws_.products; ++index) {
		products[index] = String(product_name(index));
	}
	root[k.products] = products;
	root[k.product_count] = ws_.products;
	root[k.city_count] = static_cast<int64_t>(ws_.cities.size());

	// Totais do mundo: uma passada, na ordem das cidades (mesma soma que a UI
	// fazia sobre `cities[]`, bit a bit).
	std::vector<double> transit;
	compute_in_transit(transit);
	double population = 0.0;
	double production = 0.0;
	double stockpile = 0.0;
	double need = 0.0;
	double consumed = 0.0;
	double deficit = 0.0;
	double in_transit = 0.0;
	double informal_share_sum = 0.0;
	double dissatisfaction_sum = 0.0;
	double excess_deaths = 0.0;
	for (std::size_t city_index = 0; city_index < ws_.cities.size(); ++city_index) {
		const City& city = ws_.cities[city_index];
		double city_production = 0.0;
		for (const int bi : ws_.buildings_by_city[city_index]) {
			const Building& building =
				ws_.buildings[static_cast<std::size_t>(bi)];
			if (building.input_product < 0) {
				city_production += building.capacity * building.output_scale;
			}
		}
		population += city.population;
		production += city_production;
		stockpile += city.markets[0].stock;
		need += city.pop.need;
		consumed += city.pop.consumed;
		deficit += std::max(0.0, city.pop.need - city.pop.consumed);
		in_transit += transit[city_index];
		// B7: agregados leves (mÃ©dia ponderada por populaÃ§Ã£o).
		informal_share_sum += city.pop.informal_share * city.population;
		dissatisfaction_sum += city.pop.dissatisfaction * city.population;
		excess_deaths += city.pop.excess_deaths_total
			+ city.region.excess_deaths_total;
	}
	root[k.population] = population;
	root[k.production] = production;
	root[k.stockpile] = stockpile;
	root[k.need] = need;
	root[k.consumed] = consumed;
	root[k.deficit] = deficit;
	root[k.in_transit] = in_transit;
	root["informal_share"] = population > 0.0
		? informal_share_sum / population : 0.0;
	root["dissatisfaction"] = population > 0.0
		? dissatisfaction_sum / population : 0.0;
	root["excess_deaths_total"] = excess_deaths;
	root[k.trade_orders_count] = static_cast<int64_t>(ws_.last_orders.size());
	return root;
}

Dictionary EconomySim::get_city_columns() {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	if (columns_day_ == ws_.day
			&& columns_static_version_ == status_static_version_) {
		return columns_cache_;
	}
	const std::size_t count = ws_.cities.size();
	std::vector<double> transit;
	compute_in_transit(transit);
	Dictionary& out = columns_cache_;
	out.clear();
	out["count"] = static_cast<int64_t>(count);
	PackedStringArray ids;
	PackedStringArray names;
	PackedStringArray nations;
	PackedStringArray currencies;
	PackedFloat64Array xs;
	PackedFloat64Array ys;
	PackedFloat64Array deposits_remaining;
	ids.resize(static_cast<int>(count));
	names.resize(static_cast<int>(count));
	nations.resize(static_cast<int>(count));
	currencies.resize(static_cast<int>(count));
	xs.resize(static_cast<int>(count));
	ys.resize(static_cast<int>(count));
	deposits_remaining.resize(static_cast<int>(count));
	// Uma PackedFloat64Array por escalar, na MESMA ordem/nomes do bloco leve.
	static const char* kScalarNames[] = {
		"population", "simulation_population", "gdp", "gdp_per_capita",
		"external_income_index", "firm_cash", "service_cash",
		"government_budget", "government_goods_spend", "service_revenue",
		"service_jobs", "service_workers", "service_jobs_filled",
		"in_kind_food", "service_profit", "opex_revenue", "freight_revenue",
		"construction_revenue", "service_wage", "disposable_per_capita",
		"living_standard", "production", "jobs", "income",
		"industrialization", "stockpile", "need", "deficit", "consumed",
		"in_transit", "fx_rate", "tariff_revenue", "productivity_index",
	};
	constexpr int kScalars = static_cast<int>(sizeof(kScalarNames)
		/ sizeof(kScalarNames[0]));
	PackedFloat64Array scalars[kScalars];
	for (int s = 0; s < kScalars; ++s) {
		scalars[s].resize(static_cast<int>(count));
	}
	String* id_ptr = ids.ptrw();
	String* name_ptr = names.ptrw();
	String* nation_ptr = nations.ptrw();
	String* currency_ptr = currencies.ptrw();
	double* x_ptr = xs.ptrw();
	double* y_ptr = ys.ptrw();
	double* deposit_ptr = deposits_remaining.ptrw();
	double* scalar_ptrs[kScalars];
	for (int s = 0; s < kScalars; ++s) {
		scalar_ptrs[s] = scalars[s].ptrw();
	}
	for (std::size_t index = 0; index < count; ++index) {
		const City& city = ws_.cities[index];
		id_ptr[index] = String(city.id.c_str());
		name_ptr[index] = String(city.name.c_str());
		nation_ptr[index] = String(city.nation.c_str());
		currency_ptr[index] = String(currency_code(city.nation));
		x_ptr[index] = city.center.x;
		y_ptr[index] = city.center.y;
		double remaining = 0.0;
		for (int product = 0; product < ws_.products; ++product) {
			const Deposit& deposit = city.deposits[product];
			if (deposit.tracked) {
				remaining += deposit.remaining_kt;
			}
		}
		deposit_ptr[index] = remaining;
		const CityScalars values_out = city_scalars(index, transit[index]);
		const double values[kScalars] = {
			values_out.population, values_out.simulation_population,
			values_out.gdp, values_out.gdp_per_capita,
			values_out.external_income_index, values_out.firm_cash,
			values_out.service_cash, values_out.government_budget,
			values_out.government_goods_spend, values_out.service_revenue,
			values_out.service_jobs, values_out.service_workers,
			values_out.service_jobs_filled, values_out.in_kind_food,
			values_out.service_profit, values_out.opex_revenue,
			values_out.freight_revenue, values_out.construction_revenue,
			values_out.service_wage, values_out.disposable_per_capita,
			values_out.living_standard, values_out.production,
			values_out.jobs, values_out.income,
			values_out.industrialization, values_out.stockpile,
			values_out.need, values_out.deficit, values_out.consumed,
			values_out.in_transit, values_out.fx_rate,
			values_out.tariff_revenue, values_out.productivity_index,
		};
		for (int s = 0; s < kScalars; ++s) {
			scalar_ptrs[s][index] = values[s];
		}
	}
	out["id"] = ids;
	out["name"] = names;
	out["nation"] = nations;
	out["currency"] = currencies;
	out["x"] = xs;
	out["y"] = ys;
	out["deposit_remaining_kt"] = deposits_remaining;
	for (int s = 0; s < kScalars; ++s) {
		out[StringName(kScalarNames[s])] = scalars[s];
	}
	columns_day_ = ws_.day;
	columns_static_version_ = status_static_version_;
	return out;
}

Array EconomySim::get_city_rows() {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	if (query_rows_day_ == ws_.day
			&& query_rows_static_version_ == status_static_version_
			&& query_city_rows_.size() == static_cast<int>(ws_.cities.size())) {
		return query_city_rows_;
	}
	std::vector<double> transit;
	compute_in_transit(transit);
	query_city_slots_.resize(ws_.cities.size());
	query_city_rows_.resize(static_cast<int>(ws_.cities.size()));
	for (std::size_t index = 0; index < ws_.cities.size(); ++index) {
		StatusCitySlot& slot = query_city_slots_[index];
		fill_city_status(index, slot, transit[index], false);
		query_city_rows_[static_cast<int>(index)] = slot.dict;
	}
	query_rows_day_ = ws_.day;
	query_rows_static_version_ = status_static_version_;
	return query_city_rows_;
}

Dictionary EconomySim::get_city_info(const String& city_id) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Dictionary result;
	const std::string id = std::string(city_id.utf8().get_data());
	const auto found = ws_.city_lookup.find(id);
	if (found == ws_.city_lookup.end()) {
		return result;
	}
	const std::size_t city_index = static_cast<std::size_t>(found->second);
	double in_transit = 0.0;
	for (const Shipment& shipment : ws_.in_transit) {
		if (shipment.to_city == id) {
			in_transit += shipment.amount;
		}
	}
	StatusCitySlot slot;
	fill_city_status(city_index, slot, in_transit, false);
	return slot.dict;
}

Array EconomySim::get_nation_rows() {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	// Se o status do dia jÃ¡ foi montado (tools/testes/uma tela pediu), devolve
	// o MESMO bloco `nations` â€” as fichas consultadas batem bit a bit com ele.
	if (status_live_ >= 0 && status_built_day_ == ws_.day) {
		return status_slots_[status_live_].nations;
	}
	std::vector<double> transit;
	compute_in_transit(transit);
	StatusCache cache;
	build_nations(cache, transit);
	return cache.nations;
}

Dictionary EconomySim::get_nation_status(const String& nation_tag) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Dictionary result;
	const Array nations = get_nation_rows();
	for (int index = 0; index < nations.size(); ++index) {
		const Dictionary nation = nations[index];
		if (String(nation.get("tag", "")) == nation_tag) {
			return nation;
		}
	}
	return result;
}

Array EconomySim::get_trade_orders() {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	if (query_orders_day_ != ws_.day) {
		StatusCache cache;
		build_orders(cache);
		query_orders_cache_ = cache.orders;
		query_orders_day_ = ws_.day;
	}
	return query_orders_cache_;
}

Array EconomySim::get_ports() {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	StatusCache cache;
	build_ports(cache);
	return cache.ports;
}

Array EconomySim::get_currency_rows() {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	StatusCache cache;
	build_currencies(cache);
	return cache.currencies;
}

Array EconomySim::get_products() {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Array products;
	products.resize(ws_.products);
	for (int index = 0; index < ws_.products; ++index) {
		products[index] = String(product_name(index));
	}
	return products;
}

Dictionary EconomySim::get_light_columns() {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	if (query_light_day_ != ws_.day) {
		StatusCache cache;
		fill_light_signals(cache);
		query_light_cache_ = cache.light;
		query_light_day_ = ws_.day;
	}
	return query_light_cache_;
}

Array EconomySim::get_city_ranking(const String& field, int count) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Array ranking;
	const Dictionary columns = get_city_columns();
	if (!columns.has(field)) {
		return ranking;
	}
	const Variant values = columns[field];
	if (values.get_type() != Variant::PACKED_FLOAT64_ARRAY) {
		return ranking;
	}
	const PackedFloat64Array numbers = values;
	const PackedStringArray ids = columns["id"];
	const PackedStringArray names = columns["name"];
	const PackedStringArray nations = columns["nation"];
	const int total = static_cast<int>(numbers.size());
	std::vector<int> order(static_cast<std::size_t>(total));
	for (int index = 0; index < total; ++index) {
		order[static_cast<std::size_t>(index)] = index;
	}
	std::stable_sort(order.begin(), order.end(),
		[&numbers, &ids](int left, int right) {
			if (numbers[left] != numbers[right]) {
				return numbers[left] > numbers[right];
			}
			return ids[left] < ids[right];
		});
	const int limit = count > 0 ? std::min(count, total) : total;
	ranking.resize(limit);
	for (int rank = 0; rank < limit; ++rank) {
		const int index = order[static_cast<std::size_t>(rank)];
		Dictionary row;
		row["id"] = ids[index];
		row["name"] = names[index];
		row["nation"] = nations[index];
		row["value"] = numbers[index];
		ranking[rank] = row;
	}
	return ranking;
}

Array EconomySim::get_nation_ranking(const String& field, int count) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Array ranking;
	const Array nations = get_nation_rows();
	std::vector<Dictionary> rows;
	rows.reserve(static_cast<std::size_t>(nations.size()));
	for (int index = 0; index < nations.size(); ++index) {
		const Dictionary nation = nations[index];
		if (nation.has(field)) {
			rows.push_back(nation);
		}
	}
	std::stable_sort(rows.begin(), rows.end(),
		[&field](const Dictionary& left, const Dictionary& right) {
			const double a = double(left.get(field, 0.0));
			const double b = double(right.get(field, 0.0));
			if (a != b) {
				return a > b;
			}
			return String(left.get("tag", "")) < String(right.get("tag", ""));
		});
	const int total = static_cast<int>(rows.size());
	const int limit = count > 0 ? std::min(count, total) : total;
	ranking.resize(limit);
	for (int rank = 0; rank < limit; ++rank) {
		const Dictionary& nation = rows[static_cast<std::size_t>(rank)];
		Dictionary row;
		row["tag"] = nation.get("tag", "");
		row["name"] = nation.get("name", "");
		row["value"] = nation.get(field, 0.0);
		ranking[rank] = row;
	}
	return ranking;
}

Dictionary EconomySim::get_nation_columns() {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const Array nations = get_nation_rows();
	const int count = nations.size();
	Dictionary out;
	out["count"] = static_cast<int64_t>(count);
	PackedStringArray tags;
	PackedStringArray names;
	tags.resize(count);
	names.resize(count);
	PackedInt32Array city_count;
	city_count.resize(count);
	static const char* kFields[] = {
		"population", "production", "stockpile", "need", "deficit", "in_transit",
		"wealth_gini",
	};
	constexpr int kFieldCount = static_cast<int>(sizeof(kFields)
		/ sizeof(kFields[0]));
	PackedFloat64Array values[kFieldCount];
	for (int f = 0; f < kFieldCount; ++f) {
		values[f].resize(count);
	}
	for (int index = 0; index < count; ++index) {
		const Dictionary nation = nations[index];
		tags[index] = String(nation.get("tag", ""));
		names[index] = String(nation.get("name", ""));
		city_count[index] = int(nation.get("city_count", 0));
		for (int f = 0; f < kFieldCount; ++f) {
			values[f].ptrw()[index] = double(nation.get(kFields[f], 0.0));
		}
	}
	out["tag"] = tags;
	out["name"] = names;
	out["city_count"] = city_count;
	for (int f = 0; f < kFieldCount; ++f) {
		out[StringName(kFields[f])] = values[f];
	}
	return out;
}

void EconomySim::apply_price_shock(const String& product, double multiplier) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	int index = product_index(product);
	if (index < 0) {
		return;
	}
	for (City& city : ws_.cities) {
		const double base = base_price(index);
		city.markets[index].price = std::clamp(
			city.markets[index].price * multiplier, base * 0.1, base * 10.0);
	}
}

int EconomySim::product_index(const String& product) const {
	return sim_core_.product_index(std::string(product.utf8().get_data()));
}

const char* EconomySim::product_name(int product) const {
	return sim_core_.product_name(product);
}

int EconomySim::recipe_index(const String& recipe_id) const {
	return sim_core_.recipe_index(std::string(recipe_id.utf8().get_data()));
}

ogs::Building* EconomySim::find_building(const std::string& city_id,
		const std::string& id) {
	return ogs::find_building(ws_, city_id, id);
}

void EconomySim::set_balance(const Dictionary& values) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	auto number = [&values](const char* key, double fallback) {
		return values.has(key) ? double(values[key]) : fallback;
	};
	ws_.natural_unemployment = std::clamp(
		number("natural_unemployment", ws_.natural_unemployment), 0.0, 0.5);
	ws_.service_wage_adjust = std::clamp(
		number("service_wage_adjust", ws_.service_wage_adjust), 0.0, 1.0);
	ws_.service_wage_max = std::clamp(
		number("service_wage_max", ws_.service_wage_max), 0.01, 10.0);
	ws_.inventory_cover_days = std::clamp(
		number("inventory_cover_days", ws_.inventory_cover_days), 1.0, 30.0);
	ws_.inventory_recovery = std::clamp(
		number("inventory_recovery", ws_.inventory_recovery), 0.05, 1.0);
	// Busca do comÃ©rcio de alimento (K vizinhos + M ofertas externas).
	ws_.trade_neighbors = std::clamp(
		static_cast<int>(number("trade_neighbors", ws_.trade_neighbors)), 1, 256);
	ws_.trade_external = std::clamp(
		static_cast<int>(number("trade_external", ws_.trade_external)), 0, 256);
	// F7 calibraÃ§Ã£o: folga de custo de vida do gate de renda â†’ nÃ­vel de
	// consumo. PadrÃ£o = kIncomeBasketMargin; o laboratÃ³rio pode isolÃ¡-la
	// (1.0 = sem folga) para testar a regra do nÃ­vel sem depender da calibraÃ§Ã£o.
	ws_.income_basket_margin = std::clamp(
		number("income_basket_margin", ws_.income_basket_margin), 1.0, 8.0);
	// B5 (D-089): penalidade mÃ¡xima de acesso da infra (estrada 0). Gancho de
	// laboratÃ³rio do A/B da infra â€” 0 isola o frete do efeito dos nÃ­veis e o
	// padrÃ£o continua sendo kInfraAccessPenaltyMax.
	ws_.infra_access_penalty = std::clamp(
		number("infra_access_penalty", ws_.infra_access_penalty), 0.0, 1.0);
	// D-105 + D-111: juro da taxa bÃ¡sica no envelope de crÃ©dito da safra.
	// PadrÃ£o do jogo = LIGADO (a C4 criou o credor); o A/B desliga por aqui.
	if (values.has("harvest_credit_rate")) {
		ws_.harvest_credit_rate = number("harvest_credit_rate", 0.0) > 0.5;
	}
	// C3 (D-107): regras do crÃ©dito de empresa (default de jogo em
	// data/definitions/economy/bank_credit.json). 0 = default do sim/bank.h.
	// O mundo novo nasce com o crÃ©dito ligado (D-111); o teste/laboratÃ³rio
	// desliga ou liga explicitamente por aqui.
	if (values.has("bank_credit_enabled")) {
		ws_.bank_credit_enabled =
			number("bank_credit_enabled", 0.0) > 0.5;
	}
	ws_.bank_spread = std::clamp(
		number("bank_spread", ws_.bank_spread), 0.0, 1.0);
	ws_.bank_leverage_cap = std::clamp(
		number("bank_leverage_cap", ws_.bank_leverage_cap), 0.0, 10.0);
	ws_.bank_interest_cover = std::clamp(
		number("bank_interest_cover", ws_.bank_interest_cover), 0.0, 50.0);
	ws_.bank_default_days = std::clamp(
		static_cast<int>(number("bank_default_days",
			static_cast<double>(ws_.bank_default_days))), 0, 3650);
	ws_.bank_repay_share = std::clamp(
		number("bank_repay_share", ws_.bank_repay_share), 0.0, 1.0);
}

void EconomySim::apply_set_suitability(const String& city_id, const String& product,
		double value) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const int index = product_index(product);
	if (index < 0) {
		return;
	}
	City* city = find_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr) {
		return;
	}
	city->suitability[index] = std::clamp(value, 0.0, 1.0);
	// O status reuse arrays de aptidÃ£o: invalida sÃ³ quando o dado muda.
	++status_static_version_;
}

bool EconomySim::apply_set_deposit(const String& city_id, const String& product,
		double reserve_kt, double quality, double depth_m) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const int index = product_index(product);
	if (index < 0) {
		return false;
	}
	City* city = find_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr) {
		return false;
	}
	Deposit& deposit = city->deposits[index];
	deposit.quality = std::clamp(safe_nonneg(quality), 0.0, 1.0);
	deposit.depth_m = safe_nonneg(depth_m);
	if (deposit.tracked) {
		// Idempotente: nÃ£o reabastece uma jazida jÃ¡ em exploraÃ§Ã£o (load/save).
		return false;
	}
	deposit.tracked = true;
	deposit.initial_kt = safe_nonneg(reserve_kt);
	deposit.remaining_kt = deposit.initial_kt;
	deposit.extracted_kt = 0.0;
	// D-086b: a mina semeada pela produÃ§Ã£o real nasce OPERANDO. O contrato do
	// GeographyService chega no `initialize_world`, antes de a jazida ser
	// declarada aqui; a reserva do estÃ¡gio (fatia de referÃªncia do nÃºcleo) Ã©
	// aberta agora, na primeira declaraÃ§Ã£o do depÃ³sito. Mina histÃ³rica
	// (`stage_pending == false`) fica no piso atÃ© pagar o aprofundamento.
	for (Building& building : ws_.buildings) {
		// `stage_pending` primeiro: a maioria das construÃ§Ãµes nÃ£o Ã© semente e o
		// teste barato evita o compare de string por construÃ§Ã£o/depÃ³sito.
		if (!building.stage_pending || building.city != city->id
				|| building.output_product != index
				|| building.stage_reserve_kt > 1e-12) {
			continue;
		}
		if (SimCore::is_mine_building(*city, building)) {
			sim_core_.set_mine_stage(*city, building, building.stage);
		}
	}
	++status_deposit_version_;
	return true;
}

void EconomySim::apply_set_external_demand(const String& city_id, const String& product,
		double ceiling_t_per_day) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const int index = product_index(product);
	if (index < 0) {
		return;
	}
	City* city = find_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr) {
		return;
	}
	city->external_demand[index] = safe_nonneg(ceiling_t_per_day);
	++status_static_version_;
}

void EconomySim::apply_set_external_income_index(const String& city_id, double index) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	City* city = find_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr) {
		return;
	}
	// [0, 8]: 0 desliga a renda (calibraÃ§Ã£o/teste), 8 cobre com folga a maior
	// razÃ£o PIB PPP per capita do cenÃ¡rio (Singapura â‰ˆ 6,3Ã— a mÃ©dia mundial) e
	// corta dados corrompidos sem virar um teto econÃ´mico silencioso.
	city->external_income_index = std::clamp(index, 0.0, 8.0);
	// F7: a estrutura etÃ¡ria, a escolaridade e a participaÃ§Ã£o derivam do mesmo
	// Ã­ndice de renda real (Ãºnico dado por paÃ­s do cenÃ¡rio). Reaplica o perfil
	// quando o Ã­ndice chega do GeographyService, logo apÃ³s criar o mundo.
	apply_labor_profile(*city);
	++status_static_version_;
}

void EconomySim::apply_set_productivity_index(const String& city_id, double index) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	City* city = find_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr) {
		return;
	}
	// 0 (ou negativo) = DERIVAR do Ã­ndice de renda real: o dado do PIB PPP per
	// capita chega por set_external_income_index e a TFP acompanha. Um
	// valor positivo Ã© fator explÃ­cito do laboratÃ³rio. Os limites sÃ³ contÃªm
	// dado corrompido: o cenÃ¡rio real fica em ~0,38 (NGA) a ~3,57 (USA).
	city->productivity_index = index > 0.0
		? std::clamp(index, 0.05, 8.0) : 0.0;
	++status_static_version_;
}

void EconomySim::apply_set_city_strata(const String& city_id,
		const PackedFloat64Array& shares) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	City* city = find_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr) {
		return;
	}
	double values[kIncomeStrata];
	double total = 0.0;
	for (int s = 0; s < kIncomeStrata; ++s) {
		values[s] = s < shares.size() ? std::max(0.0, shares[s]) : 0.0;
		total += values[s];
	}
	if (total > 1e-9) {
		for (int s = 0; s < kIncomeStrata; ++s) {
			values[s] /= total;
		}
		sim_core_.seed_strata(*city, values);
	} else {
		// Sem dado: re-semeia com a proxy do Gini declarado.
		sim_core_.seed_strata(*city, nullptr);
	}
	// B3: o interior espelha os quintis observados do paÃ­s (20% de pessoas cada;
	// a renda acompanha o alvo nacional). Sem dado urbano, 20% iguais.
	if (city->region.ready) {
		sim_core_.seed_region_strata(*city, total > 1e-9 ? values : nullptr);
	}
	refresh_income_classes(*city);
	++status_static_version_;
}

void EconomySim::set_city_strata(const String& city_id,
		const PackedFloat64Array& shares) {
	// API de SETUP (initialize_world/set_urban/apply_to_sim): antes do dia 1,
	// como set_external_income_index e set_income_gini na inicializaÃ§Ã£o. NÃ£o Ã©
	// aÃ§Ã£o de jogador â€” nÃ£o passa pela fila A3 porque nÃ£o existe, no jogo, um
	// comando para reescrever a distribuiÃ§Ã£o inicial.
	apply_set_city_strata(city_id, shares);
}

void EconomySim::apply_set_city_region(const String& city_id,
		const Dictionary& data) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	City* city = find_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr) {
		return;
	}
	Region region;
	if (!parse_region(data, region)) {
		return;
	}
	city->region = std::move(region);
	city->population = city->region.population_region;
	city->simulation_population = city->region.urban_population;
	// A aglomeraÃ§Ã£o pode ter mudado: re-semeia o perfil urbano (demografia/
	// estratos) e o bloco do interior. API de setup â€” vale antes do dia 1.
	apply_labor_profile(*city);
	sim_core_.seed_region(*city);
	++status_static_version_;
	status_invalidate();
}

void EconomySim::set_city_region(const String& city_id, const Dictionary& data) {
	// API de SETUP, como set_city_strata: aplica direto (antes do dia 1).
	apply_set_city_region(city_id, data);
}

Dictionary EconomySim::get_region_status(const String& city_id) const {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Dictionary out;
	const City* city = find_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr || !city->region.ready) {
		return out;
	}
	const Region& region = city->region;
	out["city"] = city_id;
	out["name"] = String(region.name.c_str());
	out["population"] = region.population_region;
	out["urban_population"] = region.urban_population;
	out["region_population"] = region.region_pop;
	out["urban_share"] = region.urban_share;
	out["employment"] = region.employment;
	out["employment_mix"] = Array{region.employment_mix[0],
		region.employment_mix[1], region.employment_mix[2]};
	// Renda em base DIÃRIA (o salÃ¡rio do interior Ã© pago por semana na cadÃªncia
	// A6; `region.wage_income`/`income`/`taxes` jÃ¡ guardam o equivalente diÃ¡rio).
	// Os totais semanais (brutos) ficam explÃ­citos com o sufixo `_weekly`.
	const double week = double(std::max(1, ws_.cadence.weekly_period));
	out["wage_income"] = region.wage_income;
	out["income"] = region.income;
	out["taxes"] = region.taxes;
	out["wage_income_weekly"] = region.wage_income * week;
	out["income_weekly"] = region.income * week;
	out["food_spend"] = region.food_spend;
	out["food_need"] = region.food_need;
	out["food_consumed"] = region.food_consumed;
	out["satisfaction"] = region.satisfaction;
	out["cash"] = region.cash;
	Array states;
	for (const RegionState& state : region.states) {
		Dictionary entry;
		entry["id"] = String(state.id.c_str());
		entry["name"] = String(state.name.c_str());
		entry["population"] = state.population;
		entry["area_km2"] = state.area_km2;
		states.append(entry);
	}
	out["states"] = states;
	Array strata_people;
	Array strata_income_share;
	Array strata_savings;
	for (int s = 0; s < kIncomeStrata; ++s) {
		strata_people.append(region.stratum_people[s]);
		strata_income_share.append(region.strata_income_share[s]);
		strata_savings.append(region.stratum_savings[s]);
	}
	out["strata_people"] = strata_people;
	out["strata_income_share"] = strata_income_share;
	out["strata_savings"] = strata_savings;
	// B4: migraÃ§Ã£o â€” acumuladores + fluxo do Ãºltimo fechamento mensal + a
	// urbanizaÃ§Ã£o CORRENTE (urbano vivo Ã· populaÃ§Ã£o exibida) contra a referÃªncia
	// nacional (World Bank; -1 = sem dado). O urbano vivo Ã© `simulation_population`
	// (Î£ coortes), nÃ£o `region.urban_population` (a SEED do dia 0).
	{
		Dictionary migration;
		migration["rural_in_total"] = region.migration_rural_in_total;
		migration["city_in_total"] = region.migration_city_in_total;
		migration["city_out_total"] = region.migration_city_out_total;
		migration["month_rural_in"] = region.migration_month_rural_in;
		migration["month_city_in"] = region.migration_month_city_in;
		migration["month_city_out"] = region.migration_month_city_out;
		migration["month_net"] = region.migration_month_net;
		migration["month_rate"] = region.migration_month_rate;
		migration["urban_share"] = city->population > 0.0
			? city->simulation_population / city->population : 0.0;
		migration["urban_share_ref"] = region.urban_share;
		out["migration"] = migration;
	}
	// B5: infraestrutura do estado â€” nÃ­veis, manutenÃ§Ã£o do dia e os
	// multiplicadores derivados (custo de acesso, produtividade, Ãªxodo).
	{
		Dictionary infra;
		infra["road"] = region.infra_road;
		infra["power"] = region.infra_power;
		infra["water"] = region.infra_water;
		infra["maintenance"] = region.infra_maintenance;
		infra["access_multiplier"] =
			ogs::SimCore::infra_access_multiplier(region,
				ws_.infra_access_penalty);
		infra["productivity_multiplier"] =
			ogs::SimCore::infra_productivity_multiplier(region);
		infra["exodus_multiplier"] =
			ogs::SimCore::infra_exodus_multiplier(region);
		out["infra"] = infra;
	}
	// B7: informalidade, mortalidade e insatisfaÃ§Ã£o do interior.
	{
		Dictionary informal;
		informal["share"] = region.informal_share;
		Array by_quintile;
		by_quintile.resize(kIncomeStrata);
		for (int s = 0; s < kIncomeStrata; ++s) {
			by_quintile[s] = region.informal_labor_share[s];
		}
		informal["by_quintile"] = by_quintile;
		informal["wage_bill"] = region.informal_wage_bill;
		Dictionary service;
		service["workers"] = region.informal_service_workers;
		service["output"] = region.informal_service_output;
		service["demand"] = region.informal_service_demand;
		service["revenue"] = region.informal_service_revenue;
		service["price"] = region.informal_service_price;
		service["share"] = region.informal_service_share;
		informal["service"] = service;
		out["informal"] = informal;

		Dictionary mortality;
		mortality["deaths"] = region.deaths_total;
		mortality["expected_deaths"] = region.deaths_expected;
		mortality["excess_total"] = region.excess_deaths_total;
		mortality["excess_famine_total"] = region.excess_deaths_famine_total;
		mortality["famine_months"] = region.famine_months;
		mortality["health_access"] = sim_core_.nation_health_access(*city);
		mortality["health_access_ref"] = region.health_access_ref;
		out["mortality"] = mortality;

		Dictionary dissatisfaction;
		dissatisfaction["index"] = region.dissatisfaction;
		Array dq;
		dq.resize(kIncomeStrata);
		for (int s = 0; s < kIncomeStrata; ++s) {
			dq[s] = region.dissatisfaction_by_stratum[s];
		}
		dissatisfaction["by_quintile"] = dq;
		Dictionary components;
		components["unemployment"] = region.dissatisfaction_unemployment;
		components["rent"] = region.dissatisfaction_rent;
		components["food"] = region.dissatisfaction_food;
		components["inflation"] = region.dissatisfaction_inflation;
		components["informality"] = region.dissatisfaction_informality;
		components["land"] = 0.0;
		dissatisfaction["components"] = components;
		out["dissatisfaction"] = dissatisfaction;
	}
	// B5: concessÃµes â€” dono (F9) e royalty por mina/fazenda (extraÃ§Ã£o primÃ¡ria)
	// da regiÃ£o (construÃ§Ãµes da cidade-polo). Dono = "state" | "national:<g>" |
	// "foreign:<g>@<n>" | "private".
	{
		Array concessions;
		const std::size_t cidx = static_cast<std::size_t>(
			city - ws_.cities.data());
		if (cidx < ws_.buildings_by_city.size()) {
			for (const int bi : ws_.buildings_by_city[cidx]) {
				const Building& b =
					ws_.buildings[static_cast<std::size_t>(bi)];
				if (b.recipe >= 0 || b.input_product >= 0) {
					continue;  // sÃ³ mina/fazenda (extraÃ§Ã£o primÃ¡ria)
				}
				Dictionary c;
				c["id"] = String(b.id.c_str());
				c["resource"] = String(b.resource.c_str());
				c["output_product"] = b.output_product;
				c["owner"] = String(b.owner.c_str());
				c["royalty"] = b.royalty;
				c["royalty_paid"] = b.royalty_paid;
				concessions.append(c);
			}
		}
		out["concessions"] = concessions;
	}
	// B6: patrimÃ´nio da regiÃ£o â€” terra, moradia do interior e anel periurbano.
	// Os VALORES sÃ£o de mercado (nÃ£o sÃ£o dinheiro); `book` Ã© a base contÃ¡bil
	// (1Âª avaliaÃ§Ã£o) e o fluxo mensal Ã© o aluguel efetivamente pago no fecho.
	// Os ativos vÃªm em `assets` com a semÃ¢ntica de BALANÃ‡O da C1 (tipo, dono,
	// quantidade, valor de mercado, valor contÃ¡bil e fluxo mensal).
	{
		const std::size_t cidx = static_cast<std::size_t>(
			city - ws_.cities.data());
		const double size = [&]() {
			if (cidx >= ws_.housing.size()) {
				return 0.0;
			}
			const CityHousing& house = ws_.housing[cidx];
			return house.households > 0.0 && house.pop_anchor > 0.0
				? house.pop_anchor / house.households : 0.0;
		}();
		const double land_value = region.land_ha * region.land_value_per_ha;
		// O anel tem estoque prÃ³prio: a casa do interior Ã© de quem NÃƒO vive nele.
		const double housing_units = size > 0.0
			? std::max(0.0, region.region_pop - region.ring_population) / size
			: 0.0;
		const double ring_units = size > 0.0
			? std::max(0.0, region.ring_population) / size : 0.0;
		Dictionary property;
		property["ready"] = region.property_ready;
		property["land_ha"] = region.land_ha;
		property["land_gini"] = region.land_gini;
		property["land_gini_observed"] = region.land_gini_observed;
		property["land_value_per_ha"] = region.land_value_per_ha;
		property["land_value"] = land_value;
		property["land_income_month"] = region.land_income_month;
		property["commodity_index"] = region.commodity_index;
		property["infra_value_multiplier"] =
			ogs::SimCore::property_infra_multiplier(region);
		property["housing_units"] = housing_units;
		property["housing_price"] = region.housing_price;
		property["housing_rent"] = region.housing_rent;
		property["housing_value"] = housing_units * region.housing_price;
		property["ring_population"] = region.ring_population;
		property["ring_units"] = ring_units;
		property["ring_price"] = region.ring_price;
		property["ring_rent"] = region.ring_rent;
		property["ring_rent_paid_month"] = region.ring_rent_paid_month;
		property["ring_value"] = ring_units * region.ring_price;
		property["ring_in_total"] = region.migration_ring_in_total;
		property["ring_month_in"] = region.migration_month_ring_in;
		property["total_value"] = sim_core_.region_property_value(region);
		property["tax_rate"] = region.property_tax_rate;
		property["tax_potential"] = region.property_tax_potential;
		Array land_row;
		Array home_row;
		Array ring_row;
		Array wealth_row;
		land_row.resize(kIncomeStrata);
		home_row.resize(kIncomeStrata);
		ring_row.resize(kIncomeStrata);
		wealth_row.resize(kIncomeStrata);
		for (int s = 0; s < kIncomeStrata; ++s) {
			land_row[s] = region.stratum_land_value[s];
			home_row[s] = region.stratum_home_value[s];
			ring_row[s] = region.stratum_ring_value[s];
			wealth_row[s] = region.stratum_wealth[s];
		}
		property["land_value_by_quintile"] = land_row;
		property["home_value_by_quintile"] = home_row;
		property["ring_value_by_quintile"] = ring_row;
		property["wealth_by_quintile"] = wealth_row;
		property["wealth_gini"] = ogs::SimCore::quintile_gini(
			region.stratum_wealth, kIncomeStrata);
		// BALANÃ‡O (pronto para a C1): um ativo por tipo Ã— dono (quintil).
		// `market_value` = valor de mercado; `book_value` = valor contÃ¡bil (1Âª
		// avaliaÃ§Ã£o, congelada â€” nÃ£o hÃ¡ compra/venda nesta fase);
		// `income_month` = fluxo de renda mensal (aluguel do anel repartido
		// entre os donos; terra e moradia prÃ³prias nÃ£o geram aluguel).
		Array assets;
		double ring_value_total = 0.0;
		for (int s = 0; s < kIncomeStrata; ++s) {
			ring_value_total += region.stratum_ring_value[s];
		}
		for (int s = 0; s < kIncomeStrata; ++s) {
			const String owner = "quintile:" + String::num_int64(s);
			const double land_quantity = region.land_value_per_ha > 0.0
				? region.stratum_land_value[s] / region.land_value_per_ha : 0.0;
			Dictionary land_asset;
			land_asset["type"] = "land";
			land_asset["owner"] = owner;
			land_asset["owner_kind"] = "quintile";
			land_asset["unit"] = "ha";
			land_asset["quantity"] = land_quantity;
			land_asset["market_value"] = region.stratum_land_value[s];
			land_asset["book_value"] =
				land_quantity * region.land_book_value_per_ha;
			land_asset["income_month"] = region.land_ha > 0.0
				? region.land_income_month * land_quantity / region.land_ha : 0.0;
			assets.append(land_asset);
			const double home_quantity = region.housing_price > 0.0
				? region.stratum_home_value[s] / region.housing_price : 0.0;
			Dictionary home_asset;
			home_asset["type"] = "housing";
			home_asset["owner"] = owner;
			home_asset["owner_kind"] = "quintile";
			home_asset["unit"] = "unit";
			home_asset["quantity"] = home_quantity;
			home_asset["market_value"] = region.stratum_home_value[s];
			home_asset["book_value"] =
				home_quantity * region.housing_book_price;
			home_asset["income_month"] = 0.0;
			assets.append(home_asset);
			const double ring_quantity = region.ring_price > 0.0
				? region.stratum_ring_value[s] / region.ring_price : 0.0;
			Dictionary ring_asset;
			ring_asset["type"] = "ring";
			ring_asset["owner"] = owner;
			ring_asset["owner_kind"] = "quintile";
			ring_asset["unit"] = "unit";
			ring_asset["quantity"] = ring_quantity;
			ring_asset["market_value"] = region.stratum_ring_value[s];
			ring_asset["book_value"] = ring_quantity * region.ring_book_price;
			ring_asset["income_month"] = ring_value_total > 0.0
				? region.ring_rent_paid_month * region.stratum_ring_value[s]
					/ ring_value_total : 0.0;
			assets.append(ring_asset);
		}
		property["assets"] = assets;
		out["property"] = property;
	}
	return out;
}

void EconomySim::apply_set_income_gini(const String& city_id, double gini) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	City* city = find_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr) {
		return;
	}
	// [0, 1]: 0 desliga (legado por escolaridade); dados corrompidos nÃ£o viram
	// sigma negativo. A faixa real estimada por regiÃ£o fica em 0,33â€“0,47.
	city->income_gini = std::clamp(gini, 0.0, 1.0);
	// B2 (correÃ§Ã£o): o Gini chega DEPOIS do `initialize_world` (o GDScript
	// declara a renda no `apply_to_sim`), mas os quintis jÃ¡ foram semeados pela
	// proxy com Gini zero â€” repartiÃ§Ã£o igual, que zerava life/luxury na classe
	// publicada. Re-semeia a proxy com o Gini declarado; a repartiÃ§Ã£o de DADO
	// observado (set_city_strata) nÃ£o Ã© tocada.
	if (city->pop.strata_ready && !city->pop.strata_from_data) {
		sim_core_.seed_strata(*city, nullptr);
	}
	// Recalcula as classes na hora: o status lido antes do primeiro tick jÃ¡
	// publica a repartiÃ§Ã£o ancorada (se o mundo ainda nÃ£o tem preÃ§os, cai no
	// espelho dos estratos, sem inventar Gini).
	refresh_income_classes(*city);
	++status_static_version_;
}

// ---------------------------------------------------------------------------
// F7 â€” trabalho, educaÃ§Ã£o e renda.
// ---------------------------------------------------------------------------
void EconomySim::apply_labor_profile(City& city) {
	sim_core_.apply_labor_profile(city);
}

void EconomySim::set_labor_profile(const String& city_id,
		const Dictionary& profile) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	City* city = find_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr) {
		return;
	}
	apply_labor_profile(*city);
	Pop& pop = city->pop;
	const double population = std::max(0.0, city->simulation_population);
	if (profile.has("working_age_share")) {
		const double share = std::clamp(
			double(profile["working_age_share"]), 0.0, 1.0);
		pop.working_age = population * share;
		pop.recruit_reserve = pop.working_age * kLaborRecruitShare;
	}
	if (profile.has("retiree_share")) {
		pop.retirees = population * std::clamp(
			double(profile["retiree_share"]), 0.0, 1.0);
	}
	if (profile.has("education")) {
		const Variant value = profile["education"];
		if (value.get_type() == Variant::ARRAY) {
			const Array shares = value;
			if (shares.size() >= kEducationLevels) {
				double basic = std::max(0.0, double(shares[0]));
				double intermediate = std::max(0.0, double(shares[1]));
				double higher = std::max(0.0, double(shares[2]));
				const double sum = basic + intermediate + higher;
				if (sum > 0.0) {
					basic /= sum;
					intermediate /= sum;
					higher /= sum;
					pop.education[0] = pop.working_age * basic;
					pop.education[1] = pop.working_age * intermediate;
					pop.education[2] = pop.working_age * higher;
					// Perfil explÃ­cito substitui a estrutura etÃ¡ria: as coortes
					// em formaÃ§Ã£o do perfil derivado da renda nÃ£o podem vazar
					// para a nova distribuiÃ§Ã£o (senÃ£o "todo mundo bÃ¡sico" ainda
					// formava superiores todos os dias).
					pop.studying[0] = 0.0;
					pop.studying[1] = 0.0;
				}
			}
		}
	}
	if (profile.has("participation")) {
		const Variant value = profile["participation"];
		if (value.get_type() == Variant::ARRAY) {
			const Array rates = value;
			for (int level = 0;
					level < kEducationLevels && level < rates.size(); ++level) {
				pop.participation[level] = std::clamp(
					double(rates[level]), 0.0, 1.0);
			}
		} else {
			const double rate = std::clamp(double(value), 0.0, 1.0);
			for (int level = 0; level < kEducationLevels; ++level) {
				pop.participation[level] = rate;
			}
		}
	}
	if (profile.has("wage_by_level")) {
		const Variant value = profile["wage_by_level"];
		if (value.get_type() == Variant::ARRAY) {
			const Array wages = value;
			for (int level = 0;
					level < kEducationLevels && level < wages.size(); ++level) {
				pop.wage_by_level[level] = std::max(0.0, double(wages[level]));
			}
		}
	}
	pop.labor_force = 0.0;
	for (int level = 0; level < kEducationLevels; ++level) {
		const double studying = level < kEducationLevels - 1
			? pop.studying[level] : 0.0;
		pop.labor_force_by_level[level] = std::max(0.0,
			pop.education[level] * pop.participation[level] - studying);
		pop.unemployed_by_level[level] = pop.labor_force_by_level[level];
		pop.employed_by_level[level] = 0.0;
		pop.unfilled_by_level[level] = 0.0;
		pop.labor_force += pop.labor_force_by_level[level];
	}
	pop.unemployed = pop.labor_force;
	pop.employed = 0.0;
	pop.students = pop.studying[0] + pop.studying[1];
	pop.dependents = std::max(0.0, population - pop.labor_force);
	refresh_strata(pop);
	status_invalidate();
}

void EconomySim::apply_set_minimum_wage(const String& nation, double value) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const std::string tag(nation.utf8().get_data());
	if (tag.empty()) {
		return;
	}
	if (value <= 0.0) {
		ws_.minimum_wages.erase(tag);
	} else {
		ws_.minimum_wages[tag] = std::min(value, 100.0);
	}
	// O piso vale a partir do prÃ³ximo ajuste salarial; cidades sem histÃ³rico de
	// salÃ¡rio recebem o Ã¢ncora com o piso imediatamente.
	const double floor = value > 0.0 ? std::min(value, 100.0) : 0.0;
	if (floor > 0.0) {
		for (City& city : ws_.cities) {
			if (city.nation != tag) {
				continue;
			}
			for (int level = 0; level < kEducationLevels; ++level) {
				city.pop.wage_by_level[level] = std::max(
					city.pop.wage_by_level[level], floor);
			}
		}
	}
}

void EconomySim::apply_set_wage_rigidity(double value) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	ws_.wage_rigidity = std::clamp(value, 0.0, 0.25);
}

// ---------------------------------------------------------------------------
// F9 â€” EMPREGO SETORIAL (docs/economy/sector_employment_report.md)
//
// O mercado de trabalho da F7 continua fechando emprego/desemprego como antes
// (o setor de serviÃ§os Ã© o empregador residual). A F9 decompÃµe esse emprego em
// setores com estrutura ancorada em dados observados:
//   0 agropecuÃ¡ria â€” da produÃ§Ã£o local de alimentos (t/dia) e da TFP;
//   1 indÃºstria/mineraÃ§Ã£o/construÃ§Ã£o â€” da Ã¢ncora ILO por renda e do valor
//     adicionado simulado (quando ele passa da Ã¢ncora);
//   2 serviÃ§os â€” residual (formal + informal de serviÃ§os);
//   3 informal â€” SOBREPOSIÃ‡ÃƒO (fraÃ§Ã£o do emprego total), nÃ£o soma com 0..2.
// NÃ£o cria nem destrÃ³i dinheiro: a renda domiciliar continua agregada por
// cidade. O Ãºnico fluxo novo Ã© o autoconsumo agrÃ­cola em espÃ©cie (ver o piso de
// subsistÃªncia no consumo), que vem da produÃ§Ã£o local e Ã© conservado no estoque.
// ---------------------------------------------------------------------------

void EconomySim::set_sector_employment(const Variant& data) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	if (data.get_type() != Variant::DICTIONARY) {
		return;
	}
	const Dictionary root = data;
	const Array anchors = root.get("anchors", Array());
	std::vector<SectorAnchor> parsed;
	parsed.reserve(anchors.size());
	for (int index = 0; index < anchors.size(); ++index) {
		if (anchors[index].get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary entry = anchors[index];
		SectorAnchor anchor;
		anchor.income = std::clamp(
			static_cast<double>(entry.get("income_index", 1.0)), 0.05, 8.0);
		anchor.agriculture = std::clamp(
			static_cast<double>(entry.get("agriculture", 0.0)), 0.0, 0.85);
		anchor.industry = std::clamp(
			static_cast<double>(entry.get("industry", 0.0)), 0.0, 0.70);
		anchor.informal = std::clamp(
			static_cast<double>(entry.get("informal", 0.0)), 0.0, 1.0);
		parsed.push_back(anchor);
	}
	if (parsed.size() < 2) {
		return;
	}
	std::sort(parsed.begin(), parsed.end(),
		[](const SectorAnchor& a, const SectorAnchor& b) {
			return a.income < b.income;
		});
	const Dictionary agri = root.get("agriculture", Dictionary());
	if (!agri.is_empty()) {
		ws_.agri_labor_per_ton = std::clamp(
			static_cast<double>(agri.get("labor_per_ton_base",
				ws_.agri_labor_per_ton)), 0.1, 5000.0);
		ws_.agri_labor_exponent = std::clamp(
			static_cast<double>(agri.get("productivity_exponent",
				ws_.agri_labor_exponent)), 0.0, 4.0);
		ws_.agri_production_band_min = std::clamp(
			static_cast<double>(agri.get("production_band_min",
				ws_.agri_production_band_min)), 0.05, 1.0);
		ws_.agri_production_band_max = std::clamp(
			static_cast<double>(agri.get("production_band_max",
				ws_.agri_production_band_max)), 1.0, 4.0);
		ws_.agri_own_consumption_share = std::clamp(
			static_cast<double>(agri.get("own_consumption_share",
				ws_.agri_own_consumption_share)), 0.0, 1.0);
	}
	const Dictionary industry = root.get("industry", Dictionary());
	if (!industry.is_empty()) {
		ws_.industry_labor_per_va = std::clamp(
			static_cast<double>(industry.get("labor_per_value_added",
				ws_.industry_labor_per_va)), 0.0, 1000.0);
		ws_.industry_labor_exponent = std::clamp(
			static_cast<double>(industry.get("productivity_exponent",
				ws_.industry_labor_exponent)), 0.0, 4.0);
	}
	ws_.sector_anchors = std::move(parsed);
	ws_.sector_loaded = true;
	status_invalidate();
}

// ---------------------------------------------------------------------------
// B1 â€” ESTRUTURA ETÃRIA (data/definitions/population/age_structure_2026.json)
//
// DEFINIÃ‡ÃƒO estÃ¡tica, nÃ£o estado do mundo: sÃ³ o mapa por tag de naÃ§Ã£o entra no
// WorldState. O arquivo traz cinco faixas como fraÃ§Ã£o da populaÃ§Ã£o, a fraÃ§Ã£o
// 18-49, a TFR e as taxas brutas de natalidade/mortalidade por mil. NaÃ§Ã£o cujas
// faixas nÃ£o fecham em 1 (tolerÃ¢ncia de 2%) Ã© descartada inteira: dado
// inconsistente nÃ£o vira nÃºmero de simulaÃ§Ã£o. As cidades NÃƒO sÃ£o tocadas aqui:
// o mapa Ã© a DEFINIÃ‡ÃƒO e a semeadura das coortes acontece em
// `SimCore::seed_demography` (chamada por apply_labor_profile e pelo load do
// save), que precisa das cidades jÃ¡ criadas.
// ---------------------------------------------------------------------------

void EconomySim::set_age_structure(const Variant& data) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	if (data.get_type() != Variant::DICTIONARY) {
		return;
	}
	const Dictionary root = data;
	const Dictionary nations = root.get("nations", Dictionary());
	const Array tags = nations.keys();
	std::map<std::string, AgeStructure> parsed;
	for (int index = 0; index < tags.size(); ++index) {
		const String tag = String(tags[index]);
		if (tag.is_empty()) {
			continue;
		}
		const Variant entry = nations.get(tag, Variant());
		if (entry.get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary nation = entry;
		const Dictionary bands = nation.get("bands", Dictionary());
		if (bands.is_empty()) {
			continue;
		}
		AgeStructure structure;
		structure.band[0] = std::clamp(
			static_cast<double>(bands.get("age_0_14", 0.0)), 0.0, 1.0);
		structure.band[1] = std::clamp(
			static_cast<double>(bands.get("age_15_24", 0.0)), 0.0, 1.0);
		structure.band[2] = std::clamp(
			static_cast<double>(bands.get("age_25_44", 0.0)), 0.0, 1.0);
		structure.band[3] = std::clamp(
			static_cast<double>(bands.get("age_45_64", 0.0)), 0.0, 1.0);
		structure.band[4] = std::clamp(
			static_cast<double>(bands.get("age_65_plus", 0.0)), 0.0, 1.0);
		const double total = structure.band[0] + structure.band[1]
			+ structure.band[2] + structure.band[3] + structure.band[4];
		if (total < 0.98 || total > 1.02) {
			continue;
		}
		structure.frac_18_49 = std::clamp(
			static_cast<double>(nation.get("frac_18_49", 0.0)), 0.0, 1.0);
		structure.births_per_1000 = std::clamp(
			static_cast<double>(nation.get("cbr_per_1000", 0.0)), 0.0, 80.0);
		structure.deaths_per_1000 = std::clamp(
			static_cast<double>(nation.get("cdr_per_1000", 0.0)), 0.0, 80.0);
		structure.tfr = std::clamp(
			static_cast<double>(nation.get("tfr_2026", 0.0)), 0.0, 10.0);
		structure.valid = true;
		parsed[std::string(tag.utf8().get_data())] = structure;
	}
	if (parsed.empty()) {
		return;
	}
	ws_.age_structures = std::move(parsed);
	++status_static_version_;
}

// ---------------------------------------------------------------------------
// B7 â€” INFORMALIDADE E ACESSO Ã€ SAÃšDE (data/definitions/economy/informality.json
// e data/definitions/population/health_access.json).
//
// DEFINIÃ‡ÃƒO estÃ¡tica no WorldState (como `age_structures`), NÃƒO entra no save:
// a fraÃ§Ã£o informal inicial Ã© semeada no dia 0 (`SimCore::seed_informality`) e
// a referÃªncia de saÃºde Ã© lida na semeadura e na mortalidade mensal. O loader
// tolera arquivo ausente (o GDScript entrega {}) e cai na Ã¢ncora de renda /
// kHealthAccessDefault quando a naÃ§Ã£o nÃ£o tem entrada.
// ---------------------------------------------------------------------------

void EconomySim::set_informality(const Variant& data) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	if (data.get_type() != Variant::DICTIONARY) {
		return;
	}
	const Dictionary root = data;
	// O arquivo usa `rates` (tag -> fraÃ§Ã£o 0..1); `nations` Ã© aceito por
	// compatibilidade (mesmo layout de age_structure).
	Dictionary nations = root.get("rates", Dictionary());
	if (nations.is_empty()) {
		nations = root.get("nations", Dictionary());
	}
	const Array tags = nations.keys();
	std::map<std::string, double> parsed;
	for (int index = 0; index < tags.size(); ++index) {
		const String tag = String(tags[index]);
		if (tag.is_empty()) {
			continue;
		}
		const Variant entry = nations.get(tag, Variant());
		double value = -1.0;
		if (entry.get_type() == Variant::DICTIONARY) {
			const Dictionary d = entry;
			value = static_cast<double>(d.get("value", -1.0));
			if (value < 0.0) {
				value = static_cast<double>(d.get("informal", -1.0));
			}
			if (value < 0.0) {
				value = static_cast<double>(d.get("rate", -1.0));
			}
		} else if (entry.get_type() == Variant::FLOAT
				|| entry.get_type() == Variant::INT) {
			value = static_cast<double>(entry);
		}
		if (!std::isfinite(value) || value < 0.0 || value > 1.0) {
			continue;
		}
		parsed[std::string(tag.utf8().get_data())] = value;
	}
	if (parsed.empty()) {
		return;
	}
	ws_.informal_rates = std::move(parsed);
	++status_static_version_;
}

void EconomySim::set_health_access(const Variant& data) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	if (data.get_type() != Variant::DICTIONARY) {
		return;
	}
	const Dictionary root = data;
	Dictionary nations = root.get("rates", Dictionary());
	if (nations.is_empty()) {
		nations = root.get("nations", Dictionary());
	}
	const Array tags = nations.keys();
	std::map<std::string, double> parsed;
	for (int index = 0; index < tags.size(); ++index) {
		const String tag = String(tags[index]);
		if (tag.is_empty()) {
			continue;
		}
		const Variant entry = nations.get(tag, Variant());
		double value = -1.0;
		if (entry.get_type() == Variant::DICTIONARY) {
			const Dictionary d = entry;
			value = static_cast<double>(d.get("value", -1.0));
			if (value < 0.0) {
				value = static_cast<double>(d.get("coverage", -1.0));
			}
			if (value < 0.0) {
				value = static_cast<double>(d.get("index", -1.0));
			}
		} else if (entry.get_type() == Variant::FLOAT
				|| entry.get_type() == Variant::INT) {
			value = static_cast<double>(entry);
		}
		if (!std::isfinite(value) || value < 0.0 || value > 1.0) {
			continue;
		}
		parsed[std::string(tag.utf8().get_data())] = value;
	}
	if (parsed.empty()) {
		return;
	}
	ws_.health_access = std::move(parsed);
	++status_static_version_;
}

// ---------------------------------------------------------------------------
// B5 (D-089) â€” NÃVEL INICIAL DE INFRAESTRUTURA DO ESTADO
// (data/definitions/scenario/state_infrastructure_2026.json, World Bank).
//
// O GDScript aplica a regra %->nÃ­vel (core/economy/initial_infra.gd) e entrega
// `{state_id: {road, power, water}}`. Aqui sÃ³ se resolve o estado -> regiÃ£o:
// regiÃ£o com vÃ¡rios estados usa o MENOR nÃ­vel de cada tipo (pior caso, a mesma
// lÃ³gica rural do dado) e estado sem entrada vale 0 (ausÃªncia explÃ­cita). Os
// nÃ­veis ficam no Region e sÃ£o persistidos pelo save v21+ (o formato nÃ£o muda).
// ---------------------------------------------------------------------------

void EconomySim::set_region_infra(const Variant& data) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	if (data.get_type() != Variant::DICTIONARY) {
		return;
	}
	const Dictionary by_state = data;
	if (by_state.is_empty()) {
		return;
	}
	int applied = 0;
	for (City& city : ws_.cities) {
		Region& region = city.region;
		if (!region.ready || region.states.empty()) {
			continue;
		}
		int road = kInfraMaxLevel;
		int power = kInfraMaxLevel;
		int water = kInfraMaxLevel;
		for (const RegionState& state : region.states) {
			int levels[kInfraTypeCount] = {0, 0, 0};
			const Variant raw = by_state.get(String(state.id.c_str()), Variant());
			if (raw.get_type() == Variant::DICTIONARY) {
				const Dictionary entry = raw;
				levels[0] = static_cast<int>(entry.get("road", 0));
				levels[1] = static_cast<int>(entry.get("power", 0));
				levels[2] = static_cast<int>(entry.get("water", 0));
			}
			for (int type = 0; type < kInfraTypeCount; ++type) {
				const int level = std::clamp(levels[type], 0, kInfraMaxLevel);
				int* target = nullptr;
				switch (type) {
					case 0: target = &road; break;
					case 1: target = &power; break;
					default: target = &water; break;
				}
				*target = std::min(*target, level);
			}
		}
		region.infra_road = road;
		region.infra_power = power;
		region.infra_water = water;
		++applied;
	}
	if (applied > 0) {
		++status_static_version_;
	}
}

void EconomySim::set_region_property(const Variant& data) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	if (data.get_type() != Variant::DICTIONARY) {
		return;
	}
	const Dictionary by_city = data;
	if (by_city.is_empty()) {
		return;
	}
	int applied = 0;
	for (City& city : ws_.cities) {
		Region& region = city.region;
		if (!region.ready || region.property_ready) {
			continue;  // save v23 manda; sem regiÃ£o nÃ£o hÃ¡ B6
		}
		const Variant raw = by_city.get(String(city.id.c_str()), Variant());
		if (raw.get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary entry = raw;
		const double land_ha = safe_nonneg(
			static_cast<double>(entry.get("land_ha", 0.0)));
		const double land_gini = std::clamp(
			static_cast<double>(entry.get("land_gini", 0.0)), 0.0, 1.0);
		const bool observed = bool(entry.get("gini_observed", false));
		if (!(land_ha > 0.0)) {
			continue;
		}
		sim_core_.seed_region_property(city, land_ha, land_gini, observed);
		if (region.property_ready) {
			++applied;
		}
	}
	if (applied > 0) {
		++status_static_version_;
	}
}

void EconomySim::set_property_tax_rate(const String& nation,
		double rate) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const std::string tag(nation.utf8().get_data());
	const double value = std::clamp(std::isfinite(rate) ? rate : 0.0, 0.0, 1.0);
	int applied = 0;
	for (City& city : ws_.cities) {
		if (!city.region.ready || city.nation != tag) {
			continue;
		}
		city.region.property_tax_rate = value;
		++applied;
	}
	if (applied > 0) {
		++status_static_version_;
	}
}

// ---------------------------------------------------------------------------
// F8 â€” SERVIÃ‡OS
// ---------------------------------------------------------------------------

void EconomySim::set_services(const Variant& services) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Array sectors;
	if (services.get_type() == Variant::DICTIONARY) {
		const Dictionary data = services;
		sectors = data.get("sectors", Array());
		ws_.service_government_share = std::clamp(
			safe_nonneg(static_cast<double>(data.get("government_spend_share",
				ws_.service_government_share))), 0.0, 0.05);
	} else if (services.get_type() == Variant::ARRAY) {
		sectors = services;
	}
	if (sectors.size() != kMaxServiceSectors) {
		return;
	}
	std::vector<ServiceSector> parsed;
	std::vector<std::vector<std::string>> service_ids;
	parsed.reserve(kMaxServiceSectors);
	service_ids.reserve(kMaxServiceSectors);
	for (int index = 0; index < sectors.size(); ++index) {
		if (sectors[index].get_type() != Variant::DICTIONARY) {
			return;
		}
		const Dictionary d = sectors[index];
		ServiceSector sector;
		sector.id = std::string(String(d.get("id", "")).utf8().get_data());
		if (sector.id.empty()) {
			return;
		}
		sector.name_pt = std::string(String(d.get("name_pt", sector.id.c_str())).utf8().get_data());
		sector.name_en = std::string(String(d.get("name_en", sector.id.c_str())).utf8().get_data());
		sector.name_es = std::string(String(d.get("name_es", sector.id.c_str())).utf8().get_data());
		sector.icon = std::string(String(d.get("icon", sector.id.c_str())).utf8().get_data());
		sector.price = std::clamp(
			safe_nonneg(static_cast<double>(d.get("price", 1.0))), 0.01, 100000.0);
		const Dictionary pop_share = d.get("pop_share", Dictionary());
		for (int level = 0; level < kMaxNeedsLevels; ++level) {
			static const char* const kLevelKeys[3] = {"basic", "life", "luxury"};
			sector.pop_share[level] = std::max(0.0,
				static_cast<double>(pop_share.get(kLevelKeys[level], 0.0)));
		}
		sector.business_share = std::max(0.0,
			static_cast<double>(d.get("business_share", 0.0)));
		sector.freight_share = std::max(0.0,
			static_cast<double>(d.get("freight_share", 0.0)));
		sector.government_share = std::max(0.0,
			static_cast<double>(d.get("government_share", 0.0)));
		const Array inputs = d.get("input_goods", Array());
		for (int slot = 0; slot < inputs.size(); ++slot) {
			const Dictionary input = inputs[slot];
			const int product = product_index(
				String(input.get("good", "")));
			const double qty = safe_nonneg(
				static_cast<double>(input.get("per_unit", 0.0)));
			if (product < 0 || qty <= 0.0) {
				continue;
			}
			sector.inputs.push_back({product, qty});
		}
		std::vector<std::string> names;
		const Array service_inputs = d.get("input_services", Array());
		for (int slot = 0; slot < service_inputs.size() && slot < 2; ++slot) {
			const Dictionary input = service_inputs[slot];
			const std::string id = std::string(
				String(input.get("sector", "")).utf8().get_data());
			const double qty = safe_nonneg(
				static_cast<double>(input.get("per_unit", 0.0)));
			if (id.empty() || qty <= 0.0) {
				continue;
			}
			names.push_back(id);
			sector.service_input_qty.push_back(qty);
		}
		const Array mix = d.get("education_mix", Array());
		double mix_sum = 0.0;
		for (int level = 0; level < kEducationLevels; ++level) {
			sector.education_mix[level] = level < mix.size()
				? std::max(0.0, static_cast<double>(mix[level])) : 0.0;
			mix_sum += sector.education_mix[level];
		}
		if (mix_sum <= 1e-9) {
			sector.education_mix[0] = 1.0;
		} else {
			for (int level = 0; level < kEducationLevels; ++level) {
				sector.education_mix[level] /= mix_sum;
			}
		}
		sector.jobs_per_unit = std::clamp(
			safe_nonneg(static_cast<double>(d.get("jobs_per_unit", 3.0))), 0.001, 1000.0);
		sector.capital_per_unit = std::clamp(
			safe_nonneg(static_cast<double>(d.get("capital_per_unit", 96.0))), 0.01, 1.0e7);
		sector.min_capacity = std::clamp(
			safe_nonneg(static_cast<double>(d.get("min_capacity", 5.0))), 0.01, 1.0e7);
		sector.exportable = bool(d.get("exportable", false));
		sector.export_share = std::clamp(
			safe_nonneg(static_cast<double>(d.get("export_share", 0.0))), 0.0, 0.5);
		sector.health = bool(d.get("health", false));
		sector.education = bool(d.get("education", false));
		sector.source = std::string(
			String(d.get("source", "estimated:structure")).utf8().get_data());
		parsed.push_back(std::move(sector));
		service_ids.push_back(std::move(names));
	}
	// Resolve os serviÃ§os intermediÃ¡rios depois de todos os ids existirem.
	for (std::size_t index = 0; index < parsed.size(); ++index) {
		for (const std::string& id : service_ids[index]) {
			int found = -1;
			for (std::size_t other = 0; other < parsed.size(); ++other) {
				if (parsed[other].id == id) {
					found = static_cast<int>(other);
					break;
				}
			}
			if (found >= 0 && found != static_cast<int>(index)) {
				parsed[index].service_inputs.push_back(found);
			} else if (found == static_cast<int>(index)) {
				parsed[index].service_input_qty.pop_back();
			}
		}
		if (parsed[index].service_inputs.size() != parsed[index].service_input_qty.size()) {
			parsed[index].service_inputs.clear();
			parsed[index].service_input_qty.clear();
		}
	}
	ws_.service_sectors = std::move(parsed);
	ws_.services_loaded = true;
	for (City& city : ws_.cities) {
		for (int sector = 0; sector < kMaxServiceSectors; ++sector) {
			ServiceMarket& market = city.service_markets[sector];
			market.base_price = ws_.service_sectors[static_cast<std::size_t>(sector)].price;
			if (market.price <= 0.0 || !std::isfinite(market.price)) {
				market.price = market.base_price;
			}
		}
	}
	if (!ws_.cities.empty() && ws_.service_businesses.empty()) {
		seed_services();
	}
}

Array EconomySim::get_service_sectors() const {
	Array out;
	for (const ServiceSector& sector : ws_.service_sectors) {
		Dictionary entry;
		entry["id"] = String(sector.id.c_str());
		entry["name_pt"] = String(sector.name_pt.c_str());
		entry["name_en"] = String(sector.name_en.c_str());
		entry["name_es"] = String(sector.name_es.c_str());
		entry["icon"] = String(sector.icon.c_str());
		entry["price"] = sector.price;
		entry["exportable"] = sector.exportable;
		entry["health"] = sector.health;
		entry["education"] = sector.education;
		entry["jobs_per_unit"] = sector.jobs_per_unit;
		Array mix;
		for (int level = 0; level < kEducationLevels; ++level) {
			mix.append(sector.education_mix[level]);
		}
		entry["education_mix"] = mix;
		entry["source"] = String(sector.source.c_str());
		out.append(entry);
	}
	return out;
}

Dictionary EconomySim::get_services_detail(const String& city_id) const {
	Dictionary out;
	const std::string id = std::string(city_id.utf8().get_data());
	const auto found = ws_.city_lookup.find(id);
	if (found == ws_.city_lookup.end()) {
		out["error"] = "cidade inexistente";
		return out;
	}
	const City& city = ws_.cities[static_cast<std::size_t>(found->second)];
	out["city"] = city_id;
	out["nation"] = String(city.nation.c_str());
	out["name"] = String(city.name.c_str());
	Array sectors;
	for (int sector = 0; sector < static_cast<int>(ws_.service_sectors.size()); ++sector) {
		const ServiceSector& definition =
			ws_.service_sectors[static_cast<std::size_t>(sector)];
		const ServiceMarket& market = city.service_markets[sector];
		Dictionary entry;
		entry["id"] = String(definition.id.c_str());
		entry["name_pt"] = String(definition.name_pt.c_str());
		entry["name_en"] = String(definition.name_en.c_str());
		entry["name_es"] = String(definition.name_es.c_str());
		entry["icon"] = String(definition.icon.c_str());
		entry["demand"] = market.demand;
		entry["supply"] = market.supply;
		entry["unmet"] = market.unmet;
		entry["price"] = market.price;
		entry["revenue"] = market.revenue;
		entry["jobs"] = market.jobs;
		entry["businesses"] = market.businesses;
		entry["exports"] = market.exports;
		entry["export_revenue"] = market.export_revenue;
		entry["coverage"] = market.coverage;
		entry["input_cost"] = market.input_cost;
		entry["service_cost"] = market.service_cost;
		Array pop_demand;
		for (int level = 0; level < kMaxNeedsLevels; ++level) {
			pop_demand.append(market.pop_demand[level]);
		}
		entry["pop_demand"] = pop_demand;
		sectors.append(entry);
	}
	out["sectors"] = sectors;
	Array businesses;
	for (const int index : ws_.service_by_city[static_cast<std::size_t>(found->second)]) {
		const ServiceBusiness& business =
			ws_.service_businesses[static_cast<std::size_t>(index)];
		if (business.bankrupt) {
			continue;
		}
		Dictionary entry;
		entry["id"] = business.id;
		entry["sector"] = String(
			ws_.service_sectors[static_cast<std::size_t>(business.sector)].id.c_str());
		entry["owner_stratum"] = business.owner_stratum;
		entry["capacity"] = business.capacity;
		entry["jobs"] = business.jobs;
		entry["workers"] = business.workers;
		entry["capital"] = business.capital;
		entry["capital_left"] = business.capital_left;
		entry["revenue"] = business.revenue;
		entry["wages"] = business.wages;
		entry["input_cost"] = business.input_cost;
		entry["service_cost"] = business.service_cost;
		entry["profit"] = business.profit;
		entry["loss_days"] = business.loss_days;
		entry["opened_day"] = business.opened_day;
		entry["name_seed"] = static_cast<double>(
			business.name_seed % 9007199254740991ULL);
		businesses.append(entry);
	}
	out["businesses"] = businesses;
	out["private_health_cover"] = city.private_health_cover;
	out["private_education_cover"] = city.private_education_cover;
	out["service_jobs"] = city.service_jobs;
	out["service_businesses"] = city.service_business_count;
	Array class_population;
	Array income_level_share;
	for (int level = 0; level < kEducationLevels; ++level) {
		class_population.append(city.pop.class_population[level]);
	}
	for (int level = 0; level < kIncomeLevels; ++level) {
		income_level_share.append(city.pop.income_level_share[level]);
	}
	out["class_population"] = class_population;
	out["income_level_share"] = income_level_share;
	out["service_tonnage_prev"] = city.service_tonnage_prev;
	out["service_export_revenue"] = city.service_export_revenue;
	out["service_government_revenue"] = city.service_government_revenue;
	out["service_input_cost"] = city.service_input_cost;
	return out;
}

void EconomySim::apply_set_service_capacity(const String& city_id,
		const String& sector, double units_per_day) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	City* city = find_city(std::string(city_id.utf8().get_data()));
	const int sector_index = service_sector_index(
		std::string(sector.utf8().get_data()));
	if (city == nullptr || sector_index < 0) {
		return;
	}
	units_per_day = std::max(0.0, units_per_day);
	std::vector<int> local;
	for (int index = 0; index < static_cast<int>(ws_.service_businesses.size()); ++index) {
		if (ws_.service_businesses[static_cast<std::size_t>(index)].city == city->id
				&& ws_.service_businesses[static_cast<std::size_t>(index)].sector == sector_index
				&& !ws_.service_businesses[static_cast<std::size_t>(index)].bankrupt) {
			local.push_back(index);
		}
	}
	if (local.empty()) {
		if (units_per_day > 0.0) {
			const ServiceSector& definition =
				ws_.service_sectors[static_cast<std::size_t>(sector_index)];
			service_open(*city, sector_index, units_per_day,
				units_per_day * definition.capital_per_unit);
		}
		return;
	}
	const double each = units_per_day / static_cast<double>(local.size());
	for (const int index : local) {
		ServiceBusiness& business =
			ws_.service_businesses[static_cast<std::size_t>(index)];
		business.capacity = each;
		business.jobs = each
			* ws_.service_sectors[static_cast<std::size_t>(sector_index)].jobs_per_unit;
		business.workers = business.jobs;
		business.loss_days = 0;
	}
}

void EconomySim::apply_set_service_business_state(int business_id, int loss_days,
		double profit) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	for (ServiceBusiness& business : ws_.service_businesses) {
		if (business.id != business_id) {
			continue;
		}
		business.loss_days = std::max(0, loss_days);
		business.profit = profit;
		return;
	}
}

int EconomySim::get_service_business_count() const {
	int count = 0;
	for (const ServiceBusiness& business : ws_.service_businesses) {
		if (!business.bankrupt) {
			++count;
		}
	}
	return count;
}

int EconomySim::get_service_bankruptcies() const {
	return ws_.service_bankruptcies;
}

Dictionary EconomySim::get_bankruptcy_diag() const {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Dictionary by_product;
	for (int product = 0; product < ws_.products; ++product) {
		if (ws_.bankrupt_by_output[product] > 0) {
			by_product[String(sim_core_.product_name(product))] =
				ws_.bankrupt_by_output[product];
		}
	}
	Dictionary out;
	out["total"] = ws_.bankruptcies;
	out["with_crop_plan"] = ws_.bankrupt_planned_primary;
	out["by_product"] = by_product;
	return out;
}

// A5: a lÃ³gica dos aplicadores vive em sim/actions.cpp (C++ puro); aqui sÃ³ a
// borda â€” converte o resultado em `Dictionary` escrevendo as MESMAS chaves, nas
// mesmas condiÃ§Ãµes do original.
Dictionary EconomySim::apply_invest(const String& city_id, const String& building_id,
		double amount) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const ogs::InvestResult outcome = ogs::action_invest(ws_, sim_core_,
		std::string(city_id.utf8().get_data()),
		std::string(building_id.utf8().get_data()), amount,
		instant_construction_);
	Dictionary result;
	result["invested"] = outcome.invested;
	result["capacity_added"] = outcome.capacity_added;
	result["error"] = String(outcome.error.c_str());
	// "project_id" sÃ³ existe quando a expansÃ£o virou projeto fÃ­sico.
	if (!outcome.project_id.empty()) {
		result["project_id"] = String(outcome.project_id.c_str());
	}
	return result;
}

Dictionary EconomySim::apply_deepen_mine(const String& city_id,
		const String& building_id) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const ogs::DeepenResult outcome = ogs::action_deepen_mine(ws_, sim_core_,
		std::string(city_id.utf8().get_data()),
		std::string(building_id.utf8().get_data()), instant_construction_);
	Dictionary result;
	result["started"] = outcome.started;
	result["cost"] = outcome.cost;
	result["capacity_added"] = outcome.capacity_added;
	result["stage"] = outcome.stage;
	result["duration_days"] = outcome.duration_days;
	result["error"] = String(outcome.error.c_str());
	if (!outcome.project_id.empty()) {
		result["project_id"] = String(outcome.project_id.c_str());
	}
	return result;
}

Dictionary EconomySim::apply_infra_build(const String& city_id, int infra_type) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const ogs::InfraResult outcome = ogs::action_infra_build(ws_, sim_core_,
		std::string(city_id.utf8().get_data()), infra_type,
		instant_construction_);
	Dictionary result;
	result["started"] = outcome.started;
	result["cost"] = outcome.cost;
	result["infra_type"] = outcome.infra_type;
	result["next_level"] = outcome.next_level;
	result["duration_days"] = outcome.duration_days;
	result["error"] = String(outcome.error.c_str());
	if (!outcome.project_id.empty()) {
		result["project_id"] = String(outcome.project_id.c_str());
	}
	return result;
}

Dictionary EconomySim::apply_storage_build(const String& city_id, int type,
		int scope, double tons) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const ogs::StorageBuildResult outcome = ogs::action_storage_build(ws_,
		sim_core_, std::string(city_id.utf8().get_data()), type, scope, tons,
		instant_construction_);
	Dictionary result;
	result["started"] = outcome.started;
	result["cost"] = outcome.cost;
	result["capacity_added"] = outcome.capacity_added;
	result["type"] = String(ogs::storage_type_name(outcome.type)
		? ogs::storage_type_name(outcome.type) : "");
	result["owner"] = String(ogs::storage_scope_name(outcome.scope));
	result["duration_days"] = outcome.duration_days;
	result["error"] = String(outcome.error.c_str());
	if (!outcome.project_id.empty()) {
		result["project_id"] = String(outcome.project_id.c_str());
	}
	return result;
}

Dictionary EconomySim::apply_storage_trade(const String& city_id, int type,
		int scope, int product, double tons, bool buy, bool relief) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const ogs::StorageTradeResult outcome = ogs::action_storage_trade(ws_,
		sim_core_, std::string(city_id.utf8().get_data()), type, scope,
		product, tons, buy, relief);
	Dictionary result;
	result["ok"] = outcome.ok;
	result["tons"] = outcome.tons;
	result["value"] = outcome.value;
	result["free_tons"] = outcome.free_tons;
	result["error"] = String(outcome.error.c_str());
	return result;
}

// =====================================================================
// PARTE E (D-116) â€” ENERGIA ELÃ‰TRICA
// =====================================================================
namespace {
// Converte {fonte: valor} do Dictionary do Godot para o vetor por fonte.
void read_fuel_values(const Dictionary& source, double* out) {
	for (const Variant& key : source.keys()) {
		const int fuel = ogs::power_fuel_from_name(
			std::string(String(key).utf8().get_data()));
		if (fuel < 0) {
			continue;
		}
		out[fuel] = std::max(0.0, double(source[key]));
	}
}
} // namespace

int EconomySim::set_power_definitions(const Dictionary& nations,
		const Dictionary& cities, const Dictionary& params,
		bool only_missing) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	ogs::PowerParams rule = ws_.power_params;
	std::map<std::string, double> values;
	for (const Variant& key : params.keys()) {
		values[std::string(String(key).utf8().get_data())] = double(params[key]);
	}
	rule.apply(values);
	std::map<std::string, ogs::PowerNationDef> nation_defs;
	for (const Variant& key : nations.keys()) {
		const String tag = key;
		if (tag.is_empty()) {
			continue;
		}
		const Variant raw = nations[key];
		if (raw.get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary entry = raw;
		ogs::PowerNationDef def;
		def.access_pct = double(entry.get("access_pct", 100.0));
		def.access_urban_pct = double(entry.get("access_urban_pct",
			def.access_pct));
		def.access_rural_pct = double(entry.get("access_rural_pct",
			def.access_pct));
		def.loss_pct = double(entry.get("loss_pct", 10.0));
		// Ancoragem nacional (D-116): demanda-base Ember do ano-base e geração
		// real por fonte (MWh/ano). Ausentes = curva global e fator global.
		def.demand_base_mwh_day = std::max(0.0,
			double(entry.get("demand_base_mwh_day", 0.0)));
		def.has_demand = def.demand_base_mwh_day > 0.0;
		const Variant generation = entry.get("generation_mwh_year", Variant());
		if (generation.get_type() == Variant::DICTIONARY) {
			const Dictionary generation_row = generation;
			for (const Variant& fuel_key : generation_row.keys()) {
				const int fuel = ogs::power_fuel_from_name(
					std::string(String(fuel_key).utf8().get_data()));
				if (fuel < 0) {
					continue;
				}
				const Variant raw_value = generation_row[fuel_key];
				if (raw_value.get_type() != Variant::FLOAT
						&& raw_value.get_type() != Variant::INT) {
					continue;
				}
				def.generation_mwh_year[fuel] = std::max(0.0,
					double(raw_value));
			}
		}
		def.has_data = true;
		nation_defs[std::string(tag.utf8().get_data())] = def;
	}
	std::map<std::string, ogs::PowerCityDef> city_defs;
	for (const Variant& key : cities.keys()) {
		const String city_id = key;
		if (city_id.is_empty()) {
			continue;
		}
		const Variant raw = cities[key];
		if (raw.get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary entry = raw;
		ogs::PowerCityDef def;
		def.access_pct = double(entry.get("access_pct", 100.0));
		def.loss_pct = double(entry.get("loss_pct", 10.0));
		const Variant park = entry.get("park", Variant());
		if (park.get_type() == Variant::DICTIONARY) {
			read_fuel_values(park, def.park);
		}
		const Variant cap = entry.get("cap", Variant());
		if (cap.get_type() == Variant::DICTIONARY) {
			read_fuel_values(cap, def.cap);
		}
		const Array solar = entry.get("solar_month", Array());
		for (int month = 0; month < ogs::kPowerMonthCount
				&& month < solar.size(); ++month) {
			def.solar_month[month] = std::clamp(double(solar[month]),
				0.0, 1.0);
		}
		// D-122: vento médio anual do sítio (GWA) para o potencial da obra nova.
		def.wind_ms = std::max(0.0, double(entry.get("wind_ms", 0.0)));
		city_defs[std::string(city_id.utf8().get_data())] = def;
	}
	sim_core_.set_power_definitions(rule, nation_defs, city_defs,
		only_missing);
	status_invalidate();
	return static_cast<int>(city_defs.size());
}

int EconomySim::set_power_params(const Dictionary& values) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	std::map<std::string, double> parsed;
	for (const Variant& key : values.keys()) {
		parsed[std::string(String(key).utf8().get_data())] = double(values[key]);
	}
	sim_core_.set_power_params(parsed);
	return static_cast<int>(parsed.size());
}

bool EconomySim::set_power_city(const String& city_id, int connection,
		double line_mw, double access_pct) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	sim_core_.set_power_city(std::string(city_id.utf8().get_data()),
		connection, line_mw, access_pct);
	return true;
}

Dictionary EconomySim::build_power_plant(const String& city_id,
		const String& fuel, double mw, const String& group_id) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	std::string error;
	const bool started = sim_core_.build_power_plant(
		std::string(city_id.utf8().get_data()),
		std::string(fuel.utf8().get_data()), mw,
		std::string(group_id.utf8().get_data()), &error,
		instant_construction_);
	Dictionary result;
	result["started"] = started;
	result["error"] = String(error.c_str());
	if (started) {
		status_invalidate();
	}
	return result;
}

Dictionary EconomySim::build_power_line(const String& city_id, double mw,
		const String& group_id) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	std::string error;
	const bool started = sim_core_.build_power_line(
		std::string(city_id.utf8().get_data()), mw,
		std::string(group_id.utf8().get_data()), &error);
	Dictionary result;
	result["started"] = started;
	result["error"] = String(error.c_str());
	if (started) {
		status_invalidate();
	}
	return result;
}

int EconomySim::set_power_interconnects(const Array& links,
		bool only_missing) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	std::vector<ogs::InterconnectDef> defs;
	const bool ready_before = ws_.power_interconnects_ready;
	for (const Variant& raw : links) {
		if (raw.get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary entry = raw;
		ogs::InterconnectDef def;
		def.a = std::string(
			String(entry.get("a", "")).utf8().get_data());
		def.b = std::string(
			String(entry.get("b", "")).utf8().get_data());
		def.importer = std::string(
			String(entry.get("importer", "")).utf8().get_data());
		def.capacity_mw = std::max(0.0,
			double(entry.get("capacity_mw", 0.0)));
		// E7 (D-120): fluxo BRUTO do ano-base como contrato de suprimento.
		def.contract_mw = std::max(0.0,
			double(entry.get("contract_mw", 0.0)));
		if (def.a.empty() || def.b.empty() || def.a == def.b) {
			continue;
		}
		if (def.b < def.a) {
			std::swap(def.a, def.b);
		}
		defs.push_back(def);
	}
	sim_core_.set_power_interconnects(defs, only_missing);
	status_invalidate();
	// O retorno é o número de linhas SEMEADAS nesta chamada: um save v36 já
	// pronto não re-semeia (`only_missing`), então devolve 0.
	if (only_missing && ready_before) {
		return 0;
	}
	return static_cast<int>(ws_.interconnects.size());
}

Dictionary EconomySim::build_interconnect(const String& nation_a,
		const String& nation_b, double mw, const String& group_id) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	std::string error;
	const bool started = sim_core_.build_interconnect(
		std::string(nation_a.utf8().get_data()),
		std::string(nation_b.utf8().get_data()), mw,
		std::string(group_id.utf8().get_data()), &error);
	Dictionary result;
	result["started"] = started;
	result["error"] = String(error.c_str());
	if (started) {
		status_invalidate();
	}
	return result;
}

Dictionary EconomySim::get_power_status() const {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Dictionary out;
	out["day"] = ws_.day;
	out["ready"] = ws_.power_ready;
	out["cash"] = ws_.power_cash;
	const ogs::PowerParams& params = ws_.power_params;
	// D-118: âncora salarial derivada do sim (XGS por salário-dia) e os
	// limites EFETIVOS em XGS/MWh — diagnóstico do relatório de escala
	// monetária; a regra continua em salário-dia no JSON.
	out["salary_day_xgs"] = params.salary_day_xgs;
	out["price_floor"] = params.price_floor;
	out["price_ceiling"] = params.price_ceiling;
	// Parque instalado (MW) e geraÃ§Ã£o do dia (MWh) por fonte, no mundo.
	double park_total[ogs::kPowerFuelCount] = {};
	double generation_total[ogs::kPowerFuelCount] = {};
	for (const ogs::PowerPlant& plant : ws_.power_plants) {
		if (plant.fuel < 0 || plant.fuel >= ogs::kPowerFuelCount) {
			continue;
		}
		park_total[plant.fuel] += std::max(0.0, plant.mw);
		generation_total[plant.fuel] += std::max(0.0,
			plant.generated_mwh);
	}
	Dictionary park;
	Dictionary generation;
	for (int fuel = 0; fuel < ogs::kPowerFuelCount; ++fuel) {
		park[String(ogs::power_fuel_name(fuel))] = park_total[fuel];
		generation[String(ogs::power_fuel_name(fuel))] =
			generation_total[fuel];
	}
	out["installed_mw_by_fuel"] = park;
	out["generation_mwh_by_fuel"] = generation;
	out["plants"] = static_cast<int>(ws_.power_plants.size());
	// D-122: a troca que o jogador e a IA precisam ver — capex/multiplicador,
	// O&M, CF declarado/teto, penalidade de balanceamento, crédito de
	// capacidade no pico e prazo da obra, por fonte. Tudo em XGS (D-118).
	Dictionary build_costs;
	for (int fuel = 0; fuel < ogs::kPowerFuelCount; ++fuel) {
		Dictionary entry;
		entry["capex_per_mw"] = ogs::power_new_capex_per_mw(params, fuel);
		entry["capex_mult"] = params.capex_mult[fuel];
		entry["opex_per_mwh"] = params.opex_per_mwh[fuel];
		entry["cf_declared"] = params.cf[fuel];
		entry["cf_cap"] = params.cf_cap[fuel];
		entry["balance_penalty"] = params.balance_penalty[fuel];
		entry["capacity_credit"] = ogs::power_capacity_credit(params, fuel);
		entry["build_days"] = ogs::power_build_days(params, fuel);
		build_costs[String(ogs::power_fuel_name(fuel))] = entry;
	}
	out["build_costs"] = build_costs;
	// Usinas NOVAS (obra de jogador/IA): potência, CF de sítio e crédito de
	// capacidade. O parque do ano-base não aparece aqui (calibrado pela Ember).
	Array new_plants;
	for (const ogs::PowerPlant& plant : ws_.power_plants) {
		if (plant.initial || plant.fuel < 0
				|| plant.fuel >= ogs::kPowerFuelCount) {
			continue;
		}
		Dictionary entry;
		entry["id"] = String(plant.id.c_str());
		entry["city"] = plant.city_index >= 0
			&& plant.city_index < static_cast<int>(ws_.cities.size())
			? String(ws_.cities[static_cast<std::size_t>(plant.city_index)]
				.id.c_str()) : String();
		entry["fuel"] = String(ogs::power_fuel_name(plant.fuel));
		entry["mw"] = plant.mw;
		entry["cf_base"] = plant.cf_base;
		entry["capacity_credit"] =
			ogs::power_capacity_credit(params, plant.fuel);
		entry["built_day"] = plant.built_day;
		new_plants.append(entry);
	}
	out["new_plants"] = new_plants;
	double world_curtailment = 0.0;
	for (const auto& row : ws_.power_grids) {
		world_curtailment += std::max(0.0, row.second.curtailment_mwh);
	}
	out["curtailment_mwh"] = world_curtailment;
	// Agregado do mundo: sÃ³ as cidades COM dado (ausÃªncia nÃ£o Ã© falta).
	double world_demand = 0.0;
	double world_served = 0.0;
	double world_lost_gdp = 0.0;
	double world_bill = 0.0;
	double world_tariff_bill = 0.0;
	int rationing_cities = 0;
	int max_ration_days = 0;
	double world_loss_weighted = 0.0;
	const std::size_t city_count = std::min(ws_.power_cities.size(),
		ws_.cities.size());
	Dictionary city_rows;
	Dictionary nation_rows;
	// Estrutura por naÃ§Ã£o (mapa vazio = naÃ§Ã£o sem rede declarada).
	std::map<std::string, Dictionary> nation_accumulator;
	for (std::size_t index = 0; index < city_count; ++index) {
		const ogs::PowerCityState& state = ws_.power_cities[index];
		const ogs::City& city = ws_.cities[index];
		Dictionary row;
		row["city"] = String(city.id.c_str());
		row["nation"] = String(city.nation.c_str());
		row["has_data"] = state.has_data;
		const char* connection = state.connection == ogs::POWER_CONNECTED
			? "connected"
			: (state.connection == ogs::POWER_PARTIAL ? "partial"
				: "isolated");
		row["connection"] = String(connection);
		row["line_mw"] = state.line_mw;
		row["access_pct"] = state.access_pct;
		// Perda efetiva do dia: base do dado reduzida pela infra B5, com piso
		// tÃ©cnico (obra nunca zera a perda).
		const double infra = std::clamp(
			static_cast<double>(city.region.infra_power) / ogs::kInfraMaxLevel,
			0.0, 1.0);
		const double loss_pct = std::clamp(
			state.loss_pct * (1.0 - params.loss_infra_reduction * infra),
			params.loss_floor_pct, params.loss_max_pct);
		row["loss_pct"] = state.loss_pct;
		row["loss_pct_effective"] = loss_pct;
		row["demand_mwh"] = state.demand_mwh;
		row["served_mwh"] = state.served_mwh;
		const double ratio = state.demand_mwh > 0.0
			? std::clamp(state.served_mwh / state.demand_mwh, 0.0, 1.0) : 1.0;
		row["served_ratio"] = ratio;
		row["price"] = state.price;
		row["tariff"] = state.tariff;
		row["average_cost"] = state.average_cost;
		row["scarcity_pass"] = state.scarcity_pass;
		row["ration_days"] = state.ration_days;
		row["lost_gdp"] = state.lost_gdp;
		row["stress"] = city.power_stress;
		row["industry_factor"] = city.power_industry_factor;
		row["service_factor"] = city.power_service_factor;
		row["power_cost_industry"] = city.power_cost_industry;
		row["power_cost_per_value"] = city.power_cost_per_value;
		city_rows[String(city.id.c_str())] = row;
		if (!state.has_data) {
			continue;
		}
		world_demand += state.demand_mwh;
		world_served += state.served_mwh;
		world_lost_gdp += state.lost_gdp;
		world_bill += state.served_mwh * state.price;
		world_tariff_bill += state.served_mwh
			* (state.tariff > 0.0 ? state.tariff : state.price);
		world_loss_weighted += loss_pct * state.demand_mwh;
		if (state.ration_days > 0) {
			rationing_cities += 1;
		}
		max_ration_days = std::max(max_ration_days, state.ration_days);
	}
	out["cities"] = city_rows;
	out["demand_mwh"] = world_demand;
	out["served_mwh"] = world_served;
	out["served_ratio"] = world_demand > 0.0
		? world_served / world_demand : 1.0;
	out["average_price"] = world_served > 0.0
		? world_bill / world_served : 0.0;
	out["average_tariff"] = world_served > 0.0
		? world_tariff_bill / world_served : 0.0;
	out["rationing_cities"] = rationing_cities;
	out["max_ration_days"] = max_ration_days;
	out["lost_gdp"] = world_lost_gdp;
	out["loss_pct"] = world_demand > 0.0
		? world_loss_weighted / world_demand : 0.0;
	// E5 (D-117): comércio do dia entre redes NACIONAIS. `flow_mwh` é a energia
	// entregue ao importador (o que entra na carga); `sent_mwh` é a gerada pelo
	// exportador (inclui a perda da linha) e `value` é o dinheiro a preço de
	// fronteira que entrou no caixa do setor.
	double world_trade_flow = 0.0;
	double world_trade_sent = 0.0;
	double world_trade_value = 0.0;
	Array interconnect_rows;
	for (const ogs::InterconnectState& line : ws_.interconnects) {
		Dictionary entry;
		entry["a"] = String(line.a.c_str());
		entry["b"] = String(line.b.c_str());
		entry["capacity_mw"] = line.capacity_mw;
		entry["built_day"] = line.built_day;
		entry["initial"] = line.initial;
		// E7 (D-120): contrato de suprimento do ano-base.
		entry["contract_importer"] = String(line.contract_importer.c_str());
		entry["contract_exporter"] = String(line.contract_exporter.c_str());
		entry["contract_mw"] = line.contract_mw;
		entry["contract_flow_mwh"] = line.contract_flow_mwh;
		entry["contract_sent_mwh"] = line.contract_sent_mwh;
		entry["contract_price"] = line.contract_price;
		entry["contract_value"] = line.contract_value;
		entry["importer"] = String(line.importer_today.c_str());
		entry["exporter"] = String(line.exporter_today.c_str());
		entry["flow_mwh"] = line.flow_mwh;
		entry["sent_mwh"] = line.sent_mwh;
		entry["price"] = line.price;
		entry["value"] = line.value;
		world_trade_flow += line.flow_mwh;
		world_trade_sent += line.sent_mwh;
		world_trade_value += line.value;
		interconnect_rows.append(entry);
	}
	out["interconnects"] = interconnect_rows;
	out["interconnect_links"] = static_cast<int>(ws_.interconnects.size());
	out["interconnect_flow_mwh"] = world_trade_flow;
	out["interconnect_sent_mwh"] = world_trade_sent;
	out["interconnect_value"] = world_trade_value;
	// Rede por naÃ§Ã£o: usa o estado publicado no dia (o agregado da fase).
	Dictionary nations_out;
	for (const auto& row : ws_.power_grids) {
		const ogs::PowerGridState& grid = row.second;
		Dictionary entry;
		entry["nation"] = String(grid.nation.c_str());
		entry["exists"] = grid.exists;
		entry["price"] = grid.price;
		entry["tariff"] = grid.tariff;
		entry["average_cost"] = grid.average_cost;
		entry["scarcity_pass"] = grid.scarcity_pass;
		entry["retail_margin"] = grid.retail_margin;
		entry["demand_mwh"] = grid.demand_mwh;
		entry["served_mwh"] = grid.served_mwh;
		entry["served_ratio"] = grid.demand_mwh > 0.0
			? grid.served_mwh / grid.demand_mwh : 1.0;
		entry["ration_days"] = grid.ration_days;
		entry["lost_gdp"] = grid.lost_gdp;
		entry["connected"] = grid.connected;
		entry["partial"] = grid.partial;
		entry["isolated"] = grid.isolated;
		entry["last_build_day"] = grid.last_build_day;
		// E5 (D-117): ledger do comércio da rede.
		entry["import_mwh"] = grid.import_mwh;
		entry["export_mwh"] = grid.export_mwh;
		entry["import_value"] = grid.import_value;
		entry["export_value"] = grid.export_value;
		entry["interconnect_count"] = grid.interconnect_count;
		// D-122: renovável disponível e não despachado no dia (MWh).
		entry["curtailment_mwh"] = grid.curtailment_mwh;
		Dictionary by_fuel;
		for (int fuel = 0; fuel < ogs::kPowerFuelCount; ++fuel) {
			by_fuel[String(ogs::power_fuel_name(fuel))] =
				grid.generation_by_fuel[fuel];
		}
		entry["generation_mwh_by_fuel"] = by_fuel;
		nations_out[String(row.first.c_str())] = entry;
	}
	out["nations"] = nations_out;
	return out;
}

Dictionary EconomySim::apply_build(const String& city_id,
		const String& building_id, const String& resource, int input_product,
		int output_product,
		double capacity, double jobs, double wage, double opex,
		double input_per_output, const String& owner, double cost,
		const String& recipe_id) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const ogs::BuildResult outcome = ogs::action_build(ws_, sim_core_,
		std::string(city_id.utf8().get_data()),
		std::string(building_id.utf8().get_data()),
		std::string(resource.utf8().get_data()), input_product, output_product,
		capacity, jobs, wage, opex, input_per_output,
		std::string(owner.utf8().get_data()), cost,
		std::string(recipe_id.utf8().get_data()), instant_construction_,
		bootstrap_construction_);
	Dictionary result;
	result["created"] = outcome.created;
	result["cost"] = outcome.cost;
	result["error"] = String(outcome.error.c_str());
	// Â§19: sinaliza Ã  IA que a obra ficou pendente por falta de zona (sÃ³ true).
	if (outcome.zone_pending) {
		result["zone_pending"] = true;
	}
	// "project_id" sÃ³ existe quando a obra virou projeto fÃ­sico.
	if (!outcome.project_id.empty()) {
		result["project_id"] = String(outcome.project_id.c_str());
	}
	return result;
}

// =====================================================================
// F9 â€” investimento e propriedade
// =====================================================================

void EconomySim::seed_groups() {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	sim_core_.seed_groups();
	++status_static_version_;
	status_invalidate();
}

int EconomySim::set_group_names(const Dictionary& names) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	int applied = 0;
	for (const Variant& key : names.keys()) {
		const std::string id(String(key).utf8().get_data());
		Group* group = find_group(id);
		if (group == nullptr) {
			continue;
		}
		const String value = names[key];
		if (value.is_empty()) {
			continue;
		}
		group->name = std::string(value.utf8().get_data());
		++applied;
	}
	if (applied > 0) {
		status_invalidate();
	}
	return applied;
}

Array EconomySim::get_groups() const {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	struct GroupStats {
		int assets = 0;
		double capacity = 0.0;
		double employed = 0.0;
		std::set<std::string> countries;
	};
	std::vector<GroupStats> stats(ws_.groups.size());
	for (const Building& building : ws_.buildings) {
		std::string group_id;
		std::string origin;
		const int kind = owner_kind(building.owner, &group_id, &origin);
		if (kind != 1 && kind != 2) {
			continue;
		}
		const int index = group_index(group_id);
		if (index < 0) {
			continue;
		}
		GroupStats& row = stats[static_cast<std::size_t>(index)];
		row.assets += 1;
		row.capacity += building.capacity * building.output_scale;
		row.employed += building.workers;
		if (const City* city = find_city(building.city)) {
			row.countries.insert(city->nation);
		}
	}
	std::vector<int> order(ws_.groups.size());
	for (std::size_t index = 0; index < order.size(); ++index) {
		order[index] = static_cast<int>(index);
	}
	std::sort(order.begin(), order.end(), [this](int a, int b) {
		const Group& ga = ws_.groups[static_cast<std::size_t>(a)];
		const Group& gb = ws_.groups[static_cast<std::size_t>(b)];
		if (ga.revenue_total != gb.revenue_total) {
			return ga.revenue_total > gb.revenue_total;
		}
		if (ga.cash != gb.cash) {
			return ga.cash > gb.cash;
		}
		return ga.id < gb.id;
	});
	Array result;
	for (const int index : order) {
		const Group& group = ws_.groups[static_cast<std::size_t>(index)];
		const GroupStats& row = stats[static_cast<std::size_t>(index)];
		Dictionary entry;
		entry["id"] = String(group.id.c_str());
		entry["name"] = group.name.empty()
			? String(group.id.c_str()) : String(group.name.c_str());
		entry["origin_nation"] = String(group.origin_nation.c_str());
		entry["sector"] = String(group.sector.c_str());
		entry["state_owned"] = group.state_owned;
		entry["cash"] = group.cash;
		entry["distributable"] = group.distributable;
		entry["revenue_total"] = group.revenue_total;
		entry["profit_total"] = group.profit_total;
		entry["dividends_total"] = group.dividends_total;
		entry["taxes_total"] = group.taxes_total;
		entry["capex_total"] = group.capex_total;
		entry["assets"] = row.assets;
		entry["capacity"] = row.capacity;
		entry["employed"] = row.employed;
		entry["countries"] = static_cast<int>(row.countries.size());
		result.append(entry);
	}
	return result;
}

Array EconomySim::get_group_ranking() const {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	// Ordem idÃªntica Ã  de get_groups (faturamento, caixa, id), sem a
	// varredura de construÃ§Ãµes: a IA precisa do caixa do dia e da vez de cada
	// grupo, nÃ£o dos ativos/paÃ­ses. Evita 1 std::set + 1 entrada de ranking por
	// grupo a cada dia de decisÃ£o.
	std::vector<int> order(ws_.groups.size());
	for (std::size_t index = 0; index < order.size(); ++index) {
		order[index] = static_cast<int>(index);
	}
	std::sort(order.begin(), order.end(), [this](int a, int b) {
		const Group& ga = ws_.groups[static_cast<std::size_t>(a)];
		const Group& gb = ws_.groups[static_cast<std::size_t>(b)];
		if (ga.revenue_total != gb.revenue_total) {
			return ga.revenue_total > gb.revenue_total;
		}
		if (ga.cash != gb.cash) {
			return ga.cash > gb.cash;
		}
		return ga.id < gb.id;
	});
	Array result;
	result.resize(static_cast<int>(order.size()));
	int slot = 0;
	for (const int index : order) {
		const Group& group = ws_.groups[static_cast<std::size_t>(index)];
		Dictionary entry;
		entry["id"] = String(group.id.c_str());
		entry["origin_nation"] = String(group.origin_nation.c_str());
		entry["sector"] = String(group.sector.c_str());
		entry["state_owned"] = group.state_owned;
		entry["cash"] = group.cash;
		entry["distributable"] = group.distributable;
		entry["revenue_total"] = group.revenue_total;
		result[slot++] = entry;
	}
	return result;
}

Dictionary EconomySim::get_group(const String& group_id) const {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const std::string id(group_id.utf8().get_data());
	const int index = group_index(id);
	Dictionary entry;
	if (index < 0) {
		entry["error"] = "grupo desconhecido";
		return entry;
	}
	const Group& group = ws_.groups[static_cast<std::size_t>(index)];
	entry["id"] = String(group.id.c_str());
	entry["name"] = group.name.empty()
		? String(group.id.c_str()) : String(group.name.c_str());
	entry["origin_nation"] = String(group.origin_nation.c_str());
	entry["sector"] = String(group.sector.c_str());
	entry["state_owned"] = group.state_owned;
	entry["cash"] = group.cash;
	entry["distributable"] = group.distributable;
	entry["revenue_total"] = group.revenue_total;
	entry["profit_total"] = group.profit_total;
	entry["dividends_total"] = group.dividends_total;
	entry["taxes_total"] = group.taxes_total;
	entry["capex_total"] = group.capex_total;
	Dictionary by_host;
	for (const auto& row : group.profit_by_host) {
		by_host[String(row.first.c_str())] = row.second;
	}
	entry["profit_by_host"] = by_host;
	return entry;
}

Dictionary EconomySim::apply_fund_group(const String& group_id, double amount) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Dictionary result;
	result["funded"] = false;
	result["moved"] = 0.0;
	result["error"] = "";
	Group* group = find_group(std::string(group_id.utf8().get_data()));
	if (group == nullptr) {
		result["error"] = "grupo desconhecido";
		return result;
	}
	if (amount <= 0.0) {
		result["error"] = "valor invÃ¡lido";
		return result;
	}
	// Retira do orÃ§amento das cidades da naÃ§Ã£o de origem, na ordem dos Ã­ndices
	// (determinÃ­stica). NÃ£o cria dinheiro.
	double remaining = amount;
	for (City& city : ws_.cities) {
		if (remaining <= 0.0) {
			break;
		}
		if (city.nation != group->origin_nation) {
			continue;
		}
		const double take = std::min(remaining, std::max(0.0, city.government_budget));
		if (take <= 0.0) {
			continue;
		}
		city.government_budget -= take;
		group->cash += take;
		remaining -= take;
	}
	const double moved = amount - remaining;
	result["moved"] = moved;
	result["funded"] = moved > 0.0;
	if (moved <= 0.0) {
		result["error"] = "sem orÃ§amento na naÃ§Ã£o de origem";
	}
	return result;
}

Dictionary EconomySim::apply_group_invest(const String& group_id,
		const String& city_id, const String& building_id, const String& resource,
		int output_product, double capacity, const String& recipe_id,
		const String& partner_group) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const ogs::GroupInvestResult outcome = ogs::action_group_invest(ws_, sim_core_,
		std::string(group_id.utf8().get_data()),
		std::string(city_id.utf8().get_data()),
		std::string(building_id.utf8().get_data()),
		std::string(resource.utf8().get_data()), output_product, capacity,
		std::string(recipe_id.utf8().get_data()),
		std::string(partner_group.utf8().get_data()), instant_construction_);
	Dictionary result;
	result["created"] = outcome.created;
	result["cost"] = outcome.cost;
	result["error"] = String(outcome.error.c_str());
	// O original sÃ³ gravava "cash" nas recusas por capital insuficiente do
	// prÃ³prio grupo (caminho instantÃ¢neo e caminho de projeto).
	if (outcome.error == "capital insuficiente") {
		result["cash"] = outcome.cash;
	}
	// "owner" e "local_share" sÃ³ nas duas conclusÃµes (instantÃ¢nea e projeto).
	if (outcome.created) {
		result["owner"] = String(outcome.owner.c_str());
		result["local_share"] = outcome.local_share;
	}
	// "project_id" sÃ³ existe quando a obra virou projeto fÃ­sico.
	if (!outcome.project_id.empty()) {
		result["project_id"] = String(outcome.project_id.c_str());
	}
	return result;
}

Dictionary EconomySim::apply_set_fdi_policy(const String& nation,
		const String& sector, const String& mode, double min_local) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Dictionary result;
	result["ok"] = false;
	result["error"] = "";
	const std::string tag(nation.utf8().get_data());
	const std::string sector_id(sector.utf8().get_data());
	const String mode_name = mode.to_lower();
	int mode_value = -1;
	if (mode_name == "open" || mode_name == "aberto") {
		mode_value = 0;
	} else if (mode_name == "restricted" || mode_name == "restrito") {
		mode_value = 1;
	} else if (mode_name == "joint_venture" || mode_name == "joint") {
		mode_value = 2;
	} else if (mode_name == "closed" || mode_name == "fechado") {
		mode_value = 3;
	}
	if (mode_value < 0) {
		result["error"] = "modo invÃ¡lido";
		return result;
	}
	const std::string key = tag + "|" + sector_id;
	const auto found = ws_.fdi_lookup.find(key);
	FdiPolicy policy;
	policy.nation = tag;
	policy.sector = sector_id;
	policy.mode = mode_value;
	policy.min_local = std::clamp(min_local, 0.0, 1.0);
	if (found == ws_.fdi_lookup.end()) {
		ws_.fdi_lookup[key] = static_cast<int>(ws_.fdi_policies.size());
		ws_.fdi_policies.push_back(policy);
	} else {
		ws_.fdi_policies[static_cast<std::size_t>(found->second)] = policy;
	}
	result["ok"] = true;
	result["nation"] = String(tag.c_str());
	result["sector"] = String(sector_id.c_str());
	result["mode"] = mode_name;
	result["min_local"] = policy.min_local;
	return result;
}

Dictionary EconomySim::apply_set_sector_incentive(const String& nation,
		const String& sector, double tax_holiday, double subsidy) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Dictionary result;
	result["ok"] = true;
	const std::string tag(nation.utf8().get_data());
	const std::string sector_id(sector.utf8().get_data());
	const std::string key = tag + "|" + sector_id;
	SectorIncentive incentive;
	incentive.nation = tag;
	incentive.sector = sector_id;
	incentive.tax_holiday = std::clamp(tax_holiday, 0.0, 1.0);
	incentive.subsidy = std::max(0.0, subsidy);
	const auto found = ws_.incentive_lookup.find(key);
	if (found == ws_.incentive_lookup.end()) {
		ws_.incentive_lookup[key] = static_cast<int>(ws_.incentives.size());
		ws_.incentives.push_back(incentive);
	} else {
		ws_.incentives[static_cast<std::size_t>(found->second)] = incentive;
	}
	result["nation"] = String(tag.c_str());
	result["sector"] = String(sector_id.c_str());
	result["tax_holiday"] = incentive.tax_holiday;
	result["subsidy"] = incentive.subsidy;
	return result;
}

Dictionary EconomySim::apply_create_state_company(const String& nation,
		const String& name, double capital) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Dictionary result;
	result["created"] = false;
	result["error"] = "";
	const std::string tag(nation.utf8().get_data());
	if (tag.empty()) {
		result["error"] = "naÃ§Ã£o vazia";
		return result;
	}
	Group group;
	group.state_owned = true;
	group.origin_nation = tag;
	group.sector = "state";
	const String company_name = name.strip_edges();
	group.name = company_name.is_empty()
		? std::string() : std::string(company_name.utf8().get_data());
	int serial = 0;
	do {
		group.id = "S-" + sane_token(tag) + "-" + std::to_string(serial);
		++serial;
	} while (ws_.group_lookup.find(group.id) != ws_.group_lookup.end());
	ws_.group_lookup[group.id] = static_cast<int>(ws_.groups.size());
	ws_.groups.push_back(group);
	Group* created = &ws_.groups.back();
	double remaining = std::max(0.0, capital);
	for (City& city : ws_.cities) {
		if (remaining <= 0.0) {
			break;
		}
		if (city.nation != tag) {
			continue;
		}
		const double take = std::min(remaining, std::max(0.0, city.government_budget));
		city.government_budget -= take;
		created->cash += take;
		remaining -= take;
	}
	result["created"] = true;
	result["id"] = String(created->id.c_str());
	result["capital"] = created->cash;
	if (created->cash <= 0.0) {
		result["warning"] = "nenhum orÃ§amento disponÃ­vel na naÃ§Ã£o";
	}
	return result;
}

Dictionary EconomySim::apply_privatize(const String& city_id,
		const String& building_id, const String& buyer_group) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Dictionary result;
	result["ok"] = false;
	result["error"] = "";
	City* city = find_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr) {
		result["error"] = "cidade desconhecida";
		return result;
	}
	Building* building = find_building(city->id,
		std::string(building_id.utf8().get_data()));
	if (building == nullptr) {
		result["error"] = "ativo desconhecido";
		return result;
	}
	if (building->owner != "state") {
		result["error"] = "ativo nÃ£o Ã© estatal";
		return result;
	}
	Group* buyer = find_group(std::string(buyer_group.utf8().get_data()));
	if (buyer == nullptr) {
		result["error"] = "comprador desconhecido";
		return result;
	}
	const bool foreign = buyer->origin_nation != city->nation;
	if (foreign) {
		const std::string sector = building->output_product >= 0
			&& building->output_product < static_cast<int>(ws_.product_sectors.size())
			? ws_.product_sectors[static_cast<std::size_t>(building->output_product)]
			: std::string();
		if (fdi_mode(city->nation, sector) == 3) {
			result["error"] = "setor fechado ao capital estrangeiro";
			return result;
		}
	}
	const double price = building->capacity * building->capital_per_capacity;
	if (buyer->cash + 1e-9 < price) {
		result["error"] = "caixa insuficiente do comprador";
		return result;
	}
	buyer->cash -= price;
	buyer->capex_total += price;
	city->government_budget += price;
	building->owner = foreign
		? "foreign:" + buyer->id + "@" + buyer->origin_nation
		: "national:" + buyer->id;
	building->local_share = 0.0;
	building->partner_group.clear();
	// D1: o ativo privatizado Ã© propriedade plena do comprador, nÃ£o concessÃ£o â€”
	// limpar o royalty evita renda residual (conceder -> estatizar -> privatizar
	// deixava 5% e o ativo aparecia como concessÃ£o no status).
	building->royalty = 0.0;
	building->royalty_paid = 0.0;
	result["ok"] = true;
	result["price"] = price;
	result["owner"] = String(building->owner.c_str());
	return result;
}

// B5: conceder mina/fazenda do Estado a um grupo (nacional/estrangeiro) com um
// royalty sobre a produÃ§Ã£o. Igual Ã  privatizaÃ§Ã£o na transferÃªncia de dinheiro
// (o comprador paga o valor contÃ¡bil ao governo), mas o Estado mantÃ©m uma renda
// de royalty (`building.royalty`). O dono F9 Ã© o grupo concessionÃ¡rio â€” sem
// duplicar sistema de donos.
Dictionary EconomySim::apply_concede(const String& city_id,
		const String& building_id, const String& group_id,
		double royalty_rate) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const ogs::ConcedeResult outcome = ogs::action_concede(ws_, sim_core_,
		std::string(city_id.utf8().get_data()),
		std::string(building_id.utf8().get_data()),
		std::string(group_id.utf8().get_data()), royalty_rate);
	Dictionary result;
	result["ok"] = outcome.ok;
	result["price"] = outcome.price;
	result["royalty"] = outcome.royalty;
	result["error"] = String(outcome.error.c_str());
	if (!outcome.owner.empty()) {
		result["owner"] = String(outcome.owner.c_str());
	}
	return result;
}
Dictionary EconomySim::apply_nationalize(const String& city_id,
		const String& building_id) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Dictionary result;
	result["ok"] = false;
	result["error"] = "";
	City* city = find_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr) {
		result["error"] = "cidade desconhecida";
		return result;
	}
	Building* building = find_building(city->id,
		std::string(building_id.utf8().get_data()));
	if (building == nullptr) {
		result["error"] = "ativo desconhecido";
		return result;
	}
	std::string group_id;
	std::string origin;
	const int kind = owner_kind(building->owner, &group_id, &origin);
	if (kind == 0) {
		result["error"] = "ativo jÃ¡ Ã© estatal";
		return result;
	}
	const double book = building->capacity * building->capital_per_capacity;
	const std::string previous_owner = building->owner;
	const double compensation = std::min(book * kNationalizationCompensation,
		std::max(0.0, city->government_budget));
	if (compensation > 0.0) {
		city->government_budget -= compensation;
		if (Group* owner = find_group(group_id)) {
			owner->cash += compensation;
		} else {
			city->firm_cash += compensation;
		}
	}
	building->owner = "state";
	building->local_share = 0.0;
	building->partner_group.clear();
	// D1: estatizar extingue a concessÃ£o â€” o Estado nÃ£o paga royalty a si mesmo
	// nem o ativo pode seguir publicado como concessÃ£o.
	building->royalty = 0.0;
	building->royalty_paid = 0.0;
	// Derruba a confianÃ§a do anfitriÃ£o por um perÃ­odo: o IDE novo fica
	// bloqueado enquanto a confianÃ§a estiver abaixo de 0,5.
	ws_.confidence[city->nation] = kNationalizationConfidence;
	ws_.confidence_penalty_day[city->nation] = ws_.day + kNationalizationDays;
	result["ok"] = true;
	result["compensation"] = compensation;
	result["confidence"] = nation_confidence(city->nation);
	result["previous_owner"] = String(previous_owner.c_str());
	return result;
}

Dictionary EconomySim::get_investment_status() const {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Dictionary out;
	out["groups"] = get_groups();
	struct NationOwnership {
		double production[3] = {};
		double employment[3] = {};
		double capacity[3] = {};
		int assets[3] = {};
	};
	std::map<std::string, NationOwnership> nations;
	for (const Building& building : ws_.buildings) {
		const City* city = find_city(building.city);
		if (city == nullptr) {
			continue;
		}
		std::string group_id;
		std::string origin;
		const int kind = owner_kind(building.owner, &group_id, &origin);
		const int slot = kind == 0 ? 0 : (kind == 2 ? 2 : 1);
		NationOwnership& row = nations[city->nation];
		row.production[slot] += building.revenue;
		row.employment[slot] += building.workers;
		row.capacity[slot] += building.capacity * building.output_scale;
		row.assets[slot] += 1;
	}
	Dictionary ownership;
	for (const auto& entry : nations) {
		const NationOwnership& row = entry.second;
		double production_total = 0.0;
		double employment_total = 0.0;
		double capacity_total = 0.0;
		int assets_total = 0;
		for (int slot = 0; slot < 3; ++slot) {
			production_total += row.production[slot];
			employment_total += row.employment[slot];
			capacity_total += row.capacity[slot];
			assets_total += row.assets[slot];
		}
		Dictionary record;
		Array production;
		Array employment;
		Array capacity;
		Array assets;
		for (int slot = 0; slot < 3; ++slot) {
			production.append(production_total > 0.0
				? row.production[slot] / production_total : 0.0);
			employment.append(employment_total > 0.0
				? row.employment[slot] / employment_total : 0.0);
			capacity.append(capacity_total > 0.0
				? row.capacity[slot] / capacity_total : 0.0);
			assets.append(row.assets[slot]);
		}
		record["production_share"] = production;   // [state, national, foreign]
		record["employment_share"] = employment;
		record["capacity_share"] = capacity;
		record["assets"] = assets;
		record["production_total"] = production_total;
		record["employment_total"] = employment_total;
		record["assets_total"] = assets_total;
		record["confidence"] = nation_confidence(entry.first);
		ownership[String(entry.first.c_str())] = record;
	}
	out["nations"] = ownership;

	Array flows;
	std::set<std::string> flow_nations;
	for (const auto& entry : ws_.group_flows) {
		flow_nations.insert(entry.first);
	}
	for (const std::string& nation : flow_nations) {
		const GroupFlow flow = ws_.group_flows.at(nation);
		Dictionary record;
		record["nation"] = String(nation.c_str());
		record["repatriated_out"] = flow.repatriated_out;
		record["repatriated_in"] = flow.repatriated_in;
		record["dividends"] = flow.dividends;
		record["taxes"] = flow.taxes;
		record["subsidy"] = flow.subsidy;
		flows.append(record);
	}
	out["flows"] = flows;

	Array policies;
	for (const FdiPolicy& policy : ws_.fdi_policies) {
		Dictionary record;
		record["nation"] = String(policy.nation.c_str());
		record["sector"] = String(policy.sector.c_str());
		record["mode"] = policy.mode;
		record["min_local"] = policy.min_local;
		policies.append(record);
	}
	out["fdi_policies"] = policies;
	Array incentives;
	for (const SectorIncentive& incentive : ws_.incentives) {
		Dictionary record;
		record["nation"] = String(incentive.nation.c_str());
		record["sector"] = String(incentive.sector.c_str());
		record["tax_holiday"] = incentive.tax_holiday;
		record["subsidy"] = incentive.subsidy;
		incentives.append(record);
	}
	out["sector_incentives"] = incentives;
	out["group_count"] = static_cast<int>(ws_.groups.size());
	return out;
}

Dictionary EconomySim::get_investment_signals() const {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Dictionary out;
	// ConfianÃ§a: sÃ³ as naÃ§Ãµes com penalidade registrada; as demais valem 1,0
	// (o consumidor usa o default). Sem varredura de construÃ§Ãµes.
	Dictionary nations;
	for (const auto& row : ws_.confidence) {
		Dictionary entry;
		entry["confidence"] = row.second;
		nations[String(row.first.c_str())] = entry;
	}
	out["nations"] = nations;
	Array policies;
	policies.resize(static_cast<int>(ws_.fdi_policies.size()));
	int slot = 0;
	for (const FdiPolicy& policy : ws_.fdi_policies) {
		Dictionary record;
		record["nation"] = String(policy.nation.c_str());
		record["sector"] = String(policy.sector.c_str());
		record["mode"] = policy.mode;
		record["min_local"] = policy.min_local;
		policies[slot++] = record;
	}
	out["fdi_policies"] = policies;
	Array incentives;
	incentives.resize(static_cast<int>(ws_.incentives.size()));
	slot = 0;
	for (const SectorIncentive& incentive : ws_.incentives) {
		Dictionary record;
		record["nation"] = String(incentive.nation.c_str());
		record["sector"] = String(incentive.sector.c_str());
		record["tax_holiday"] = incentive.tax_holiday;
		record["subsidy"] = incentive.subsidy;
		incentives[slot++] = record;
	}
	out["sector_incentives"] = incentives;
	return out;
}

bool EconomySim::apply_spend_government(const String& city_id, double amount) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	City* city = find_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr || amount <= 0.0 || city->government_budget < amount) {
		return false;
	}
	city->government_budget -= amount;
	// ServiÃ§o domÃ©stico (prospecÃ§Ã£o, estudos, licenciamento).
	city->service_cash += amount;
	city->service_revenue += amount;
	city->service_profit += amount;
	city->gdp += amount;
	return true;
}

Dictionary EconomySim::to_dict() const {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	return ogs::edge::to_dict(ws_, sim_core_, &ai_phase_.potential_cache());
}

void EconomySim::set_urban(const Dictionary& urban_land,
		const Dictionary& housing_city, const Dictionary& housing,
		const Dictionary& municipal_finance) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	if (ws_.cities.empty()) {
		return;  // mundo ainda nÃ£o criado: sem id de cidade nÃ£o hÃ¡ o que casar
	}
	const Dictionary land_cities = dict_value(urban_land, "cities");
	const Dictionary housing_cities = dict_value(housing_city, "cities");
	const Dictionary nation_housing = dict_value(housing, "nations");
	const Dictionary nation_finance = dict_value(
		municipal_finance, "nations");
	const std::size_t count = ws_.cities.size();
	// Vetores temporÃ¡rios: sÃ³ substituem o estado urbano se ALGUMA cidade casar
	// com os dados. CenÃ¡rio sintÃ©tico (AGRIA/PORTUS) e load sem os JSON mantÃªm
	// os vetores vazios â€” ausÃªncia explÃ­cita, sem linhas de zeros no save.
	std::vector<CityLand> parsed_land(count);
	std::vector<CityHousing> parsed_housing(count);
	std::vector<CityGov> parsed_gov(count);
	bool matched = false;
	static const char* const kTierKeys[TIER_COUNT] = {
		"popular", "medium", "high"};
	static const char* const kTenureKeys[TENURE_COUNT] = {
		"own", "rent", "informal"};
	for (std::size_t index = 0; index < count; ++index) {
		const City& city = ws_.cities[index];
		const String city_id(city.id.c_str());
		const Dictionary u = dict_value(land_cities, city_id);
		const Dictionary h = dict_value(housing_cities, city_id);
		if (u.is_empty() && h.is_empty()) {
			continue;  // cidade sem dado urbano: defaults vazios/neutros
		}
		matched = true;
		// --- Solo (urban_land.json) ---
		CityLand& land = parsed_land[index];
		const double built_up = safe_nonneg(
			static_cast<double>(u.get("built_up_km2", 0.0)));
		const Dictionary zoning = dict_value(u, "zoning_split");
		land.area_km2[ZONE_RES] = built_up * safe_nonneg(
			static_cast<double>(zoning.get("residential", 0.0)));
		land.area_km2[ZONE_COM] = built_up * safe_nonneg(
			static_cast<double>(zoning.get("commercial", 0.0)));
		land.area_km2[ZONE_IND] = built_up * safe_nonneg(
			static_cast<double>(zoning.get("industrial", 0.0)));
		const Density max_density = density_from_height_class(
			String(u.get("height_class", "")));
		for (int zone = 0; zone < ZONE_COUNT; ++zone) {
			land.max_density[zone] = max_density;
			land.zone_fill[zone] = 1.0;
		}
		land.reserve_km2 = safe_nonneg(
			static_cast<double>(u.get("expandable_reserve_km2", 0.0)));
		// Infraestrutura atual atende a populaÃ§Ã£o de simulaÃ§Ã£o da cidade; sem
		// dado de cobertura, Ã© o ponto de partida honesto (expansÃ£o no W2).
		land.infra_capacity = safe_nonneg(city.simulation_population);
		// Â§19 (integraÃ§Ã£o): base do custo crescente â€” raio e reserva no
		// carregamento. Ãrea inicial = Ã¡rea construÃ­da da fonte (sem a
		// informal, que nasce dentro/fora da malha conforme a fonte).
		land.initial_area_km2 = built_up;
		land.initial_reserve_km2 = land.reserve_km2;

		// --- Moradia (housing_city.json; housing.json como fallback) ---
		CityHousing& house = parsed_housing[index];
		const Dictionary by_tier = dict_value(h, "housing_stock_by_tier");
		double stock[TIER_COUNT] = {};
		double stock_total = 0.0;
		for (int tier = 0; tier < TIER_COUNT; ++tier) {
			stock[tier] = safe_nonneg(static_cast<double>(
				by_tier.get(kTierKeys[tier], 0.0)));
			stock_total += stock[tier];
		}
		if (stock_total <= 0.0) {
			const Dictionary nation_split = dict_value(
				nation_housing, String(city.nation.c_str()));
			const Dictionary split = dict_value(
				nation_split, "housing_stock_split");
			const double total = safe_nonneg(
				static_cast<double>(h.get("housing_stock_total", 0.0)));
			double split_sum = 0.0;
			for (int tier = 0; tier < TIER_COUNT; ++tier) {
				stock[tier] = total * safe_nonneg(
					static_cast<double>(split.get(kTierKeys[tier], 0.0)));
				split_sum += stock[tier];
			}
			if (split_sum > 0.0) {
				stock_total = split_sum;
			}
		}
		// A fonte divide o estoque por PADRÃƒO, nÃ£o por densidade. A densidade
		// inicial segue a capacidade fÃ­sica permitida (FAR) atÃ© o gabarito da
		// cidade: um municÃ­pio baixo fica 100% em casas; um alto reparte entre
		// casa/prÃ©dio/torre na proporÃ§Ã£o da capacidade. Derivado, nÃ£o medido.
		double weights[DENSITY_COUNT] = {};
		double weight_sum = 0.0;
		for (int density = 0; density < DENSITY_COUNT; ++density) {
			if (density <= static_cast<int>(max_density)) {
				weights[density] = kUrbanFar[density];
				weight_sum += weights[density];
			}
		}
		if (weight_sum <= 0.0) {
			weights[DENSITY_LOW] = 1.0;
			weight_sum = 1.0;
		}
		for (int density = 0; density < DENSITY_COUNT; ++density) {
			for (int tier = 0; tier < TIER_COUNT; ++tier) {
				house.units[density][tier] =
					stock[tier] * weights[density] / weight_sum;
			}
		}
		// Posse por estrato: tenure_by_tier da cidade; sem a linha, o padrÃ£o
		// nacional de housing.json (casa prÃ³pria + favela; aluguel = resto).
		const Dictionary tenure = dict_value(h, "tenure_by_tier");
		const Dictionary nation = dict_value(
			nation_housing, String(city.nation.c_str()));
		const double nation_own = safe_unit(
			static_cast<double>(nation.get("homeownership_rate", 0.0)));
		const double nation_slum = safe_unit(
			static_cast<double>(nation.get("slum_pct_urban", 0.0)) / 100.0);
		for (int tier = 0; tier < TIER_COUNT; ++tier) {
			const Dictionary row = dict_value(tenure, kTierKeys[tier]);
			double row_sum = 0.0;
			for (int kind = 0; kind < TENURE_COUNT; ++kind) {
				house.tenure[tier][kind] = safe_nonneg(
					static_cast<double>(row.get(kTenureKeys[kind], 0.0)));
				row_sum += house.tenure[tier][kind];
			}
			if (row_sum <= 1e-9) {
				const double informal = tier == TIER_POPULAR ? nation_slum : 0.0;
				const double own = nation_own;
				const double rent = std::max(0.0, 1.0 - own - informal);
				house.tenure[tier][TENURE_OWN] = own;
				house.tenure[tier][TENURE_RENT] = rent;
				house.tenure[tier][TENURE_INFORMAL] = informal;
				row_sum = own + rent + informal;
			}
			if (row_sum > 1e-9) {
				for (int kind = 0; kind < TENURE_COUNT; ++kind) {
					house.tenure[tier][kind] /= row_sum;
				}
			}
		}
		// Donos: casa prÃ³pria da fonte = famÃ­lias; o restante do estoque Ã©
		// atribuÃ­do a empresas atÃ© W2/W3 criarem moradia social (governo).
		bool has_ownership = false;
		double ownership = 0.0;
		if (h.has("homeownership_rate")) {
			ownership = safe_unit(
				static_cast<double>(h["homeownership_rate"]));
			has_ownership = true;
		} else if (nation.has("homeownership_rate")) {
			ownership = nation_own;
			has_ownership = true;
		}
		if (has_ownership) {
			house.owner_share[OWNER_HOUSEHOLD] = ownership;
			house.owner_share[OWNER_FIRM] = 1.0 - ownership;
			house.owner_share[OWNER_GOVERNMENT] = 0.0;
		}
		// Aluguel/preÃ§o: a fonte publica USD/mÂ². O cÃ¢mbio do jogo Ã© a Ã¢ncora
		// XGS, referenciada no dÃ³lar (currencies.json: USD = 1,0) e a fonte
		// deriva o USD da renda PPP â€” o valor jÃ¡ estÃ¡ em Ã¢ncora, 1 USD = 1 XGS.
		// A taxa da naÃ§Ã£o entra na EXIBIÃ‡ÃƒO local (price_local = XGS Ã· taxa);
		// multiplicar por ela aqui converteria o valor em moeda local e mudaria
		// a magnitude em 5-10x (aluguel deixaria de bater com a renda).
		const double rent_m2 = safe_nonneg(
			static_cast<double>(h.get("rent_per_m2_usd", 0.0)));
		const double price_m2 = safe_nonneg(
			static_cast<double>(h.get("price_per_m2_usd", 0.0)));
		const int unit_density = static_cast<int>(max_density);
		for (int tier = 0; tier < TIER_COUNT; ++tier) {
			const double unit_m2 = kUrbanUnitM2[unit_density][tier];
			house.rent[tier] = rent_m2 * unit_m2;
			house.price[tier] = price_m2 * unit_m2;
		}
		for (int tier = 0; tier < TIER_COUNT; ++tier) {
			house.informal_units +=
				stock[tier] * house.tenure[tier][TENURE_INFORMAL];
		}
		// Ãrea informal derivada das unidades informais: sem FAR para favela,
		// uma unidade ocupa o mÂ² de piso do padrÃ£o popular baixo.
		land.informal_km2 = house.informal_units
			* kUrbanUnitM2[DENSITY_LOW][TIER_POPULAR] / 1.0e6;
		// Â§19 W1 / Q-015-C: capacidade da MORADIA no dia 0. O envelope do
		// lote (Ã¡rea Ã— FAR Ã· mÂ²) Ã© 5-15Ã— o estoque real: na escala da cidade
		// a maior parte do solo Ã© rua, pÃ¡tio, uso misto e construÃ§Ã£o antiga.
		// `zone_fill` guarda a fraÃ§Ã£o do envelope que corresponde ao estoque
		// FORMAL (a unidade informal mora em informal_km2, fora da zona) para
		// a ocupaÃ§Ã£o nascer no alvo da faixa de populaÃ§Ã£o urbana
		// (kUrbanBandOccupancy). Sem isso a cidade nasce com 5-15Ã— de
		// folga: o preÃ§o da terra nÃ£o reage e a prefeitura nunca precisa de
		// solo novo. COM/IND ficam em 1,0 (emprego modelado, outra escala).
		land.zone_fill[ZONE_RES] = 1.0;
		const double physical_res = urban_land::zone_capacity(land, ZONE_RES);
		const double formal_units = std::max(0.0, stock_total - house.informal_units);
		const double urban_population = safe_nonneg(
			static_cast<double>(u.get("urban_population", 0.0)));
		// Alvo por FAIXA de populaÃ§Ã£o urbana (a mesma que o relatÃ³rio de
		// auditoria agrupa): metrÃ³pole quase cheia, cidade mÃ©dia com folga
		// moderada, pequena com folga.
		const double target_occupancy = urban_population >= 5.0e6
			? kUrbanBandOccupancy[3]
			: urban_population >= 1.0e6 ? kUrbanBandOccupancy[2]
			: urban_population >= 3.0e5 ? kUrbanBandOccupancy[1]
			: kUrbanBandOccupancy[0];
		if (physical_res > 0.0 && formal_units > 0.0 && target_occupancy > 0.0) {
			land.zone_fill[ZONE_RES] = formal_units / (physical_res * target_occupancy);
		}

		// --- Prefeitura (municipal_finance.json) ---
		CityGov& gov = parsed_gov[index];
		const Dictionary fin = dict_value(
			nation_finance, String(city.nation.c_str()));
		if (!fin.is_empty()) {
			// A fonte nÃ£o publica a alÃ­quota municipal: usa-se o peso do IPTU
			// no total de tributos como proxy da taxa anual sobre o valor do
			// estoque (calibraÃ§Ã£o real fica para o W2).
			gov.iptu_rate = std::clamp(
				safe_nonneg(static_cast<double>(
					fin.get("property_tax_pct_total_tax", 0.0))) / 100.0,
				0.0, 0.20);
			gov.transfer_share = safe_unit(
				static_cast<double>(fin.get(
					"central_transfers_pct_subnational", 0.0)) / 100.0);
			// Â§19 W2: alvo da receita municipal do mÃªs = receita subnacional
			// observada/proxy da fonte (% do PIB) Ã— PIB municipal do mÃªs. Sem o
			// campo (no_data), fica 0: a prefeitura nÃ£o inventa nÃ­vel de receita.
			gov.revenue_pct_gdp = safe_unit(
				static_cast<double>(fin.get(
					"subnational_revenue_pct_gdp", 0.0)) / 100.0);
		}
		gov.fee_rate = kUrbanMunicipalFeeRate;
		gov.cash = 0.0;  // sem caixa municipal na fonte; o W2 acumula receita
	}
	if (matched) {
		ws_.land = std::move(parsed_land);
		ws_.housing = std::move(parsed_housing);
		ws_.city_gov = std::move(parsed_gov);
	} else {
		ws_.land.clear();
		ws_.housing.clear();
		ws_.city_gov.clear();
	}
}

Dictionary EconomySim::get_urban_city(const String& city_id) const {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Dictionary out;
	const std::string id(city_id.utf8().get_data());
	const auto found = ws_.city_lookup.find(id);
	if (found == ws_.city_lookup.end() || found->second < 0) {
		return out;
	}
	const std::size_t index = static_cast<std::size_t>(found->second);
	if (index >= ws_.cities.size() || index >= ws_.land.size()
			|| index >= ws_.housing.size()
			|| index >= ws_.city_gov.size()) {
		return out;
	}
	const City& city = ws_.cities[index];
	const CityLand& land = ws_.land[index];
	const CityHousing& house = ws_.housing[index];
	const CityGov& gov = ws_.city_gov[index];
	out["city"] = city_id;
	out["nation"] = String(city.nation.c_str());
	out["population"] = city.simulation_population;
	// Â§19 (integraÃ§Ã£o): PIB diÃ¡rio e populaÃ§Ã£o, usados pela calibraÃ§Ã£o do custo
	// de infraestrutura contra a receita municipal (teste/UI).
	out["gdp_day"] = city.gdp;
	Array area;
	Array max_density;
	Array land_price;
	for (int zone = 0; zone < ZONE_COUNT; ++zone) {
		area.append(land.area_km2[zone]);
		max_density.append(static_cast<int>(land.max_density[zone]));
		land_price.append(land.land_price[zone]);
	}
	out["area_km2"] = area;
	out["max_density"] = max_density;
	out["land_price"] = land_price;
	out["reserve_km2"] = land.reserve_km2;
	out["infra_capacity"] = land.infra_capacity;
	out["informal_km2"] = land.informal_km2;
	// Â§19 (integraÃ§Ã£o): base do custo crescente e tamanho atual do solo, para
	// a UI/teste conferirem o fator de distÃ¢ncia/relevo.
	out["initial_area_km2"] = land.initial_area_km2;
	out["initial_reserve_km2"] = land.initial_reserve_km2;
	out["expansion_cost_per_km2"] = urban_land::expansion_cost_per_km2(ws_, index);
	Array units;
	for (int density = 0; density < DENSITY_COUNT; ++density) {
		Array row;
		for (int tier = 0; tier < TIER_COUNT; ++tier) {
			row.append(house.units[density][tier]);
		}
		units.append(row);
	}
	out["units"] = units;
	out["condition"] = house.condition;
	Array owner_share;
	for (int owner = 0; owner < OWNER_COUNT; ++owner) {
		owner_share.append(house.owner_share[owner]);
	}
	out["owner_share"] = owner_share;
	Array tenure;
	for (int tier = 0; tier < TIER_COUNT; ++tier) {
		Array row;
		for (int kind = 0; kind < TENURE_COUNT; ++kind) {
			row.append(house.tenure[tier][kind]);
		}
		tenure.append(row);
	}
	out["tenure"] = tenure;
	Array rent;
	Array price;
	Array vacancy;
	for (int tier = 0; tier < TIER_COUNT; ++tier) {
		rent.append(house.rent[tier]);
		price.append(house.price[tier]);
		vacancy.append(house.vacancy[tier]);
	}
	out["rent"] = rent;
	out["price"] = price;
	out["vacancy"] = vacancy;
	out["informal_units"] = house.informal_units;
	// B2: quintis (populaÃ§Ã£o, renda disponÃ­vel, aluguel pago/faturado, posse
	// alugada e nÃ­vel de consumo). O probe urbano de 365 d calcula Ã´nus e
	// pago/faturado POR ESTRATO INQUILINO a partir daqui.
	{
		Array strata_population;
		Array strata_income;
		Array strata_rent_paid;
		Array strata_rent_bill;
		Array strata_tenure_rent;
		Array strata_tenure_informal;
		Array strata_level;
		for (int s = 0; s < kIncomeStrata; ++s) {
			strata_population.append(city.pop.stratum_people[s]);
			strata_income.append(city.pop.stratum_net_income[s]);
			strata_rent_paid.append(city.pop.stratum_rent_paid[s]);
			strata_rent_bill.append(city.pop.stratum_rent_bill[s]);
			strata_tenure_rent.append(city.pop.stratum_tenure[s][TENURE_RENT]);
			strata_tenure_informal.append(
				city.pop.stratum_tenure[s][TENURE_INFORMAL]);
			strata_level.append(city.pop.stratum_level[s]);
		}
		out["strata_population"] = strata_population;
		out["strata_income"] = strata_income;
		out["strata_rent_paid"] = strata_rent_paid;
		out["strata_rent_bill"] = strata_rent_bill;
		out["strata_tenure_rent"] = strata_tenure_rent;
		out["strata_tenure_informal"] = strata_tenure_informal;
		out["strata_level"] = strata_level;
	}
	// B3: regiÃ£o da cidade (estados inteiros) â€” campos leves para o probe e a
	// ficha. Cidade sem dado publica 0/vazio (ausÃªncia explÃ­cita).
	{
		const Region& region = city.region;
		out["region_population"] = region.region_pop;
		out["region_name"] = String(region.name.c_str());
		Array region_states;
		for (const RegionState& state : region.states) {
			region_states.append(String(state.name.c_str()));
		}
		out["region_states"] = region_states;
		out["region_employment"] = region.employment;
		out["region_food"] = region.food_consumed;
		// B4: acumuladores de migraÃ§Ã£o e a urbanizaÃ§Ã£o corrente do bloco.
		out["region_migration_rural_in"] = region.migration_rural_in_total;
		out["region_migration_city_in"] = region.migration_city_in_total;
		out["region_migration_city_out"] = region.migration_city_out_total;
		out["region_urban_share"] = region.population_region > 0.0
			? region.urban_population / region.population_region : 0.0;
	}
	// Â§19 W3: sinal de aperto por cidade (W4/UI) e fluxo do dia/mÃªs.
	out["housing_stress"] = house.housing_stress;
	out["rent_paid"] = house.rent_paid_today;
	out["rent_paid_month"] = house.rent_paid_month;
	// Repasse do aluguel aos donos no mÃªs (famÃ­lia â†’ renda de capital; empresa
	// â†’ caixa das firmas; prefeitura â†’ caixa municipal) e a manutenÃ§Ã£o.
	out["rent_to_households"] = house.rent_households_month;
	out["rent_to_firms"] = house.rent_firms_month;
	out["rent_to_government"] = house.rent_government_month;
	out["rent_burden"] = house.rent_burden;
	out["units_built_month"] = house.units_built_month;
	Dictionary gov_out;
	gov_out["cash"] = gov.cash;
	gov_out["iptu_rate"] = gov.iptu_rate;
	gov_out["fee_rate"] = gov.fee_rate;
	gov_out["transfer_share"] = gov.transfer_share;
	gov_out["month_revenue_iptu"] = gov.month_revenue_iptu;
	gov_out["month_revenue_fees"] = gov.month_revenue_fees;
	gov_out["month_revenue_transfer"] = gov.month_revenue_transfer;
	gov_out["month_expense_maintenance"] = gov.month_expense_maintenance;
	gov_out["month_expense_infrastructure"] = gov.month_expense_infrastructure;
	gov_out["month_expense_social_housing"] = gov.month_expense_social_housing;
	// Â§19 W2: calibraÃ§Ã£o do repasse, repasse pago por esta cidade e a obra em
	// andamento (o teste e a UI leem daqui).
	gov_out["revenue_pct_gdp"] = gov.revenue_pct_gdp;
	gov_out["target_revenue_month"] = gov.target_revenue_month;
	gov_out["month_transfer_paid"] = gov.month_transfer_paid;
	Array projects;
	for (int slot = 0; slot < gov.project_count; ++slot) {
		const CityGovProject& project = gov.projects[slot];
		Dictionary row;
		row["kind"] = static_cast<int>(project.kind);
		row["zone"] = project.zone;
		row["area_km2"] = project.area_km2;
		row["units"] = project.units;
		row["cost"] = project.cost;
		row["reserved"] = project.reserved;
		row["days_left"] = project.days_left;
		row["created_day"] = project.created_day;
		projects.append(row);
	}
	gov_out["projects"] = projects;
	out["gov"] = gov_out;
	// Â§19 (W1): capacidade, uso e ocupaÃ§Ã£o de cada zona [res, com, ind] e o
	// sinal de demanda barrada (insumo da IA da prefeitura no W2).
	Array zone_capacity;
	Array zone_usage;
	Array zone_occupancy;
	Array zone_demand;
	for (int zone = 0; zone < ZONE_COUNT; ++zone) {
		// Capacidade EFETIVA (com piso no uso): Ã© a que a ocupaÃ§Ã£o usa, entÃ£o
		// `zone_usage/zone_capacity` nunca passa de 1 por inconsistÃªncia de
		// fonte (Q-015-C).
		zone_capacity.append(urban_land::zone_capacity_for(ws_, index, zone));
		zone_usage.append(urban_land::zone_usage(ws_, index, zone));
		zone_occupancy.append(urban_land::zone_occupancy(ws_, index, zone));
		zone_demand.append(land.zone_demand[zone]);
	}
	out["zone_capacity"] = zone_capacity;
	out["zone_usage"] = zone_usage;
	out["zone_occupancy"] = zone_occupancy;
	out["zone_demand"] = zone_demand;
	return out;
}

namespace {
// Resultado de uma obra urbana para o GDScript (teste/UI/prefeitura).
Dictionary urban_project_result(bool ok, const std::string& error,
		const std::string& project_id, double cost) {
	Dictionary out;
	out["ok"] = ok;
	out["error"] = String::utf8(error.c_str());
	out["project_id"] = String::utf8(project_id.c_str());
	out["cost"] = cost;
	return out;
}
} // namespace

Dictionary EconomySim::apply_urban_expand(const String& city_id, int zone,
		double km2) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const std::string id(city_id.utf8().get_data());
	const auto found = ws_.city_lookup.find(id);
	if (found == ws_.city_lookup.end() || found->second < 0) {
		return urban_project_result(false, "cidade desconhecida", "", 0.0);
	}
	std::string error;
	std::string project_id;
	const bool ok = urban_land::start_project(ws_, sim_core_,
		static_cast<std::size_t>(found->second), zone, urban_land::URBAN_EXPAND,
		km2, "state", &error, &project_id);
	double cost = 0.0;
	if (ok && !ws_.construction_projects.empty()) {
		cost = ws_.construction_projects.back().total_cost;
	}
	if (ok) {
	}
	return urban_project_result(ok, error, project_id, cost);
}

Dictionary EconomySim::apply_urban_densify(const String& city_id, int zone) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const std::string id(city_id.utf8().get_data());
	const auto found = ws_.city_lookup.find(id);
	if (found == ws_.city_lookup.end() || found->second < 0) {
		return urban_project_result(false, "cidade desconhecida", "", 0.0);
	}
	std::string error;
	std::string project_id;
	const bool ok = urban_land::start_project(ws_, sim_core_,
		static_cast<std::size_t>(found->second), zone, urban_land::URBAN_DENSIFY,
		0.0, "state", &error, &project_id);
	double cost = 0.0;
	if (ok && !ws_.construction_projects.empty()) {
		cost = ws_.construction_projects.back().total_cost;
	}
	if (ok) {
	}
	return urban_project_result(ok, error, project_id, cost);
}

Dictionary EconomySim::apply_urban_social_housing(const String& city_id,
		double units) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Dictionary out;
	out["ok"] = false;
	out["units"] = 0.0;
	out["cost"] = 0.0;
	const std::string id(city_id.utf8().get_data());
	const auto found = ws_.city_lookup.find(id);
	if (found == ws_.city_lookup.end() || found->second < 0) {
		return out;
	}
	const std::size_t index = static_cast<std::size_t>(found->second);
	if (index >= ws_.city_gov.size() || index >= ws_.housing.size()) {
		return out;
	}
	const double cash_before = ws_.city_gov[index].cash;
	double units_before = 0.0;
	for (const ConstructionProject& project : ws_.construction_projects) {
		if (project.city == id && project.kind == CONSTRUCTION_HOUSING
				&& project.housing_owner == HOUSING_PROJECT_GOVERNMENT) {
			units_before += project.housing_units;
		}
	}
	if (!housing::start_social_housing(ws_, sim_core_, index, units)) {
		return out;
	}
	double built_units = 0.0;
	for (const ConstructionProject& project : ws_.construction_projects) {
		if (project.city == id && project.kind == CONSTRUCTION_HOUSING
				&& project.housing_owner == HOUSING_PROJECT_GOVERNMENT) {
			built_units += project.housing_units;
		}
	}
	out["ok"] = true;
	out["units"] = std::max(0.0, built_units - units_before);
	// Custo LÃQUIDO: a parcela de TERRA voltou para o caixa da prefeitura no
	// momento da abertura (o resto foi reservado na obra).
	out["cost"] = std::max(0.0, cash_before - ws_.city_gov[index].cash);
	return out;
}

void EconomySim::from_dict(const Dictionary& data) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	// O save substitui o mundo: o cache de status (dicionÃ¡rios reaproveitados)
	// nÃ£o pode sobreviver a ele.
	status_slots_[0] = StatusCache{};
	status_slots_[1] = StatusCache{};
	status_live_ = -1;
	status_built_day_ = -1;
	++status_static_version_;
	++status_deposit_version_;
	// A3: save carregado nÃ£o herda a fila nem o log do mundo anterior.
	commands_.reset();
	current_day_.store(0, std::memory_order_relaxed);
	ogs::edge::from_dict(ws_, sim_core_, data, &ai_phase_.potential_cache());
	// C4 (D-110): o save v31 migrado nÃ£o tem nomes e o v32 pode vir de outro
	// dado; as definiÃ§Ãµes de `banks.json` sÃ£o reaplicadas para nomear os
	// bancos e â€” se o dado nÃ£o declara estrangeiro â€” fundir o papel no
	// privado conservando valores.
	if (!bank_defs_.empty()) {
		ogs::bank::apply_definitions(ws_, bank_defs_);
	}
}

Dictionary EconomySim::to_snapshot() const {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	// Mesmo lock de to_dict: o snapshot tem de ver mundo + definiÃ§Ãµes do
	// MESMO dia (nenhum tick no meio).
	Dictionary snapshot;
	snapshot["definitions"] =
		ogs::edge::to_godot(ogs::save::definitions_to(ws_));
	snapshot["save"] = ogs::edge::to_dict(ws_, sim_core_, &ai_phase_.potential_cache());
	return snapshot;
}

void EconomySim::from_snapshot(const Dictionary& snapshot) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const Variant definitions = snapshot.get("definitions", Variant());
	const Variant save_block = snapshot.get("save", Variant());
	if (definitions.get_type() != Variant::DICTIONARY) {
		UtilityFunctions::push_error(
			"economia.snapshot: bloco 'definitions' ausente ou invÃ¡lido.");
		return;
	}
	if (save_block.get_type() != Variant::DICTIONARY) {
		UtilityFunctions::push_error(
			"economia.snapshot: bloco 'save' ausente ou invÃ¡lido.");
		return;
	}
	// O snapshot substitui o mundo inteiro: o cache de status (dicionÃ¡rios
	// reaproveitados) nÃ£o pode sobreviver a ele (mesmo caminho de
	// from_dict).
	status_slots_[0] = StatusCache{};
	status_slots_[1] = StatusCache{};
	status_live_ = -1;
	status_built_day_ = -1;
	++status_static_version_;
	++status_deposit_version_;
	// A3: snapshot carregado nÃ£o herda a fila nem o log do mundo anterior.
	commands_.reset();
	// A5: o snapshot substitui o mundo inteiro â€” o estado da IA tambÃ©m nÃ£o
	// sobrevive (diferente de from_dict, que Ã© load de save: ver o comentÃ¡rio
	// de AiPhase::reset).
	ai_phase_.reset();
	current_day_.store(0, std::memory_order_relaxed);
	// DefiniÃ§Ãµes primeiro: o save assume produtos/receitas/necessidades jÃ¡
	// carregados (Ã­ndices incluÃ­dos). A conversÃ£o do Variant Ã© feita UMA vez e
	// reaproveitada para o teto agrÃ­cola, que sÃ³ entra depois do save.
	const ogs::save::Value definition_data = ogs::edge::from_godot(definitions);
	ogs::save::definitions_from(ws_, definition_data);
	ogs::edge::from_dict(ws_, sim_core_,
		static_cast<Dictionary>(save_block), &ai_phase_.potential_cache());
	// `land_capacity` Ã© dado de geografia e sÃ³ pode entrar depois das cidades:
	// `definitions_from` rodou com o mundo vazio e ignorou os itens.
	ogs::save::apply_land_capacity(ws_, sim_core_, definition_data);
}

bool EconomySim::to_snapshot_file(const String& path) const {
	// Mesmo lock recursivo de to_snapshot: o snapshot tem de ver mundo +
	// definiÃ§Ãµes do MESMO dia (nenhum tick no meio).
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	ogs::save::Value snapshot = ogs::save::Value::make_object();
	snapshot["definitions"] = ogs::save::definitions_to(ws_);
	snapshot["save"] = ogs::save::to_value(ws_, sim_core_,
		&ai_phase_.potential_cache());
	// A serializaÃ§Ã£o Ã© a da BIBLIOTECA (nlohmann via dump_json), nÃ£o a do
	// Godot: Ã© o Ãºnico caminho com round-trip IEEE-exato para |x| < 1.
	const std::string text = ogs::save::dump_json(snapshot, 2);
	const std::string file_path(path.utf8().get_data());
	// BinÃ¡rio e truncado: UTF-8 cru, sem BOM e sem traduÃ§Ã£o de fim de linha.
	std::ofstream file(file_path, std::ios::binary | std::ios::trunc);
	if (!file) {
		UtilityFunctions::push_error(
			"economia.snapshot: nÃ£o foi possÃ­vel abrir para escrita: "
			+ String(file_path.c_str()));
		return false;
	}
	file.write(text.data(), static_cast<std::streamsize>(text.size()));
	file.flush();
	if (!file) {
		UtilityFunctions::push_error(
			"economia.snapshot: falha ao gravar o arquivo: "
			+ String(file_path.c_str()));
		return false;
	}
	return true;
}

bool EconomySim::from_snapshot_file(const String& path) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const std::string file_path(path.utf8().get_data());
	ogs::save::Value snapshot;
	std::string error;
	if (!ogs::save::parse_json_file(file_path, snapshot, error)) {
		UtilityFunctions::push_error(
			"economia.snapshot: snapshot ilegÃ­vel (" + String(file_path.c_str())
			+ "): " + String(error.c_str()));
		return false;
	}
	if (snapshot.get_type() != ogs::save::Value::DICTIONARY) {
		UtilityFunctions::push_error(
			"economia.snapshot: o snapshot nÃ£o Ã© um objeto JSON: "
			+ String(file_path.c_str()));
		return false;
	}
	const ogs::save::Value& definitions = snapshot["definitions"];
	const ogs::save::Value& save_block = snapshot["save"];
	if (definitions.get_type() != ogs::save::Value::DICTIONARY) {
		UtilityFunctions::push_error(
			"economia.snapshot: bloco 'definitions' ausente ou invÃ¡lido.");
		return false;
	}
	if (save_block.get_type() != ogs::save::Value::DICTIONARY) {
		UtilityFunctions::push_error(
			"economia.snapshot: bloco 'save' ausente ou invÃ¡lido.");
		return false;
	}
	// O snapshot substitui o mundo inteiro: o cache de status nÃ£o sobrevive
	// (mesmo caminho de from_snapshot).
	status_slots_[0] = StatusCache{};
	status_slots_[1] = StatusCache{};
	status_live_ = -1;
	status_built_day_ = -1;
	++status_static_version_;
	++status_deposit_version_;
	// A3: snapshot carregado nÃ£o herda a fila nem o log do mundo anterior.
	commands_.reset();
	// A5: mesmo motivo de from_snapshot â€” snapshot Ã© mundo inteiro novo.
	ai_phase_.reset();
	current_day_.store(0, std::memory_order_relaxed);
	// DefiniÃ§Ãµes primeiro: o save assume produtos/receitas/necessidades jÃ¡
	// carregados (Ã­ndices incluÃ­dos). `save::from_value` Ã© exatamente o que
	// `edge::from_dict` chama: nada de Variant no meio do caminho.
	ogs::save::definitions_from(ws_, definitions);
	ogs::save::from_value(ws_, sim_core_, save_block,
		&ai_phase_.potential_cache());
	// SÃ³ agora as cidades existem: o teto agrÃ­cola do bloco estÃ¡tico entra no
	// nÃºcleo depois do estado dinÃ¢mico (mesmo motivo de from_snapshot).
	ogs::save::apply_land_capacity(ws_, sim_core_, definitions);
	return true;
}

namespace {

std::string to_std(const String& value) {
	return std::string(value.utf8().get_data());
}

} // namespace

// ---------------------------------------------------------------------------
// A3 - superfÃ­cie pÃºblica: MESMA assinatura de antes, agora enfileirando.
// Slots por tipo (o log JSON usa estes mesmos nomes): ver A3_commands.md.
// ---------------------------------------------------------------------------

Dictionary EconomySim::invest(const String& city_id, const String& building_id,
		double amount) {
	ogs::Command cmd;
	cmd.type = ogs::CommandType::Invest;
	cmd.s[0] = to_std(city_id);
	cmd.s[1] = to_std(building_id);
	cmd.v[0] = amount;
	return dispatch_command(cmd);
}

Dictionary EconomySim::build(const String& city_id, const String& building_id,
		const String& resource, int input_product, int output_product,
		double capacity, double jobs, double wage, double opex,
		double input_per_output, const String& owner, double cost,
		const String& recipe_id) {
	ogs::Command cmd;
	cmd.type = ogs::CommandType::Build;
	cmd.s[0] = to_std(city_id);
	cmd.s[1] = to_std(building_id);
	cmd.s[2] = to_std(resource);
	cmd.s[3] = to_std(owner);
	cmd.s[4] = to_std(recipe_id);
	cmd.i[0] = input_product;
	cmd.i[1] = output_product;
	cmd.v[0] = capacity;
	cmd.v[1] = jobs;
	cmd.v[2] = wage;
	cmd.v[3] = opex;
	cmd.v[4] = input_per_output;
	cmd.v[5] = cost;
	return dispatch_command(cmd);
}

bool EconomySim::spend_government(const String& city_id, double amount) {
	ogs::Command cmd;
	cmd.type = ogs::CommandType::SpendGovernment;
	cmd.s[0] = to_std(city_id);
	cmd.v[0] = amount;
	const Dictionary result = dispatch_command(cmd);
	if (result.has("queued")) {
		// Comando aceito; o gasto acontece no inÃ­cio do prÃ³ximo dia. SÃ³
		// acontece para quem chama com o dia em andamento (outra thread).
		return true;
	}
	return static_cast<bool>(result.get("ok", false));
}

void EconomySim::price_shock(const String& product, double multiplier) {
	ogs::Command cmd;
	cmd.type = ogs::CommandType::PriceShock;
	cmd.s[0] = to_std(product);
	cmd.v[0] = multiplier;
	dispatch_command(cmd);
}

void EconomySim::set_suitability(const String& city_id, const String& product,
		double value) {
	ogs::Command cmd;
	cmd.type = ogs::CommandType::SetSuitability;
	cmd.s[0] = to_std(city_id);
	cmd.s[1] = to_std(product);
	cmd.v[0] = value;
	dispatch_command(cmd);
}

void EconomySim::set_land_capacity(const String& city_id, const String& product,
		double value) {
	// Dado de geografia que sÃ³ a IA consome: entra direto no nÃºcleo (sem fila
	// de comandos) e NÃƒO invalida o status publicado â€” nada na UI muda.
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const int index = product_index(product);
	if (index < 0) {
		return;
	}
	sim_core_.set_land_capacity(std::string(city_id.utf8().get_data()), index,
		value);
}

// ---------------------------------------------------------------------------
// G â€” AGRICULTURA SAZONAL (D-096)
// ---------------------------------------------------------------------------

void EconomySim::set_crop_plan(const String& city_id, const String& product,
		double area_ha, double yield_t_ha, const Array& seasons) {
	// Dado de simulaÃ§Ã£o declarado antes do dia 1 (como a geografia/estratos):
	// entra direto no nÃºcleo, sem fila de comandos.
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const int index = product_index(product);
	if (index < 0) {
		return;
	}
	std::vector<ogs::CropSeasonPlan> parsed;
	parsed.reserve(static_cast<std::size_t>(seasons.size()));
	for (int slot = 0; slot < seasons.size(); ++slot) {
		const Variant entry = seasons[slot];
		if (entry.get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary data = entry;
		ogs::CropSeasonPlan season;
		season.plant_start_doy = std::clamp(
			int(data.get("plant_start", 0)), 0, 366);
		season.plant_end_doy = std::clamp(
			int(data.get("plant_end", 0)), 0, 366);
		season.harvest_start_doy = std::clamp(
			int(data.get("harvest_start", 0)), 0, 366);
		season.harvest_end_doy = std::clamp(
			int(data.get("harvest_end", 0)), 0, 366);
		season.share = std::clamp(
			double(data.get("share", 0.0)), 0.0, 1.0);
		parsed.push_back(season);
	}
	sim_core_.set_crop_plan(std::string(city_id.utf8().get_data()), index,
		area_ha, yield_t_ha, parsed);
	// O Ã­ndice (cidade Ã— produto) muda com o plano: refaz agora para a
	// supressÃ£o da produÃ§Ã£o diÃ¡ria valer no primeiro dia.
	sim_core_.rebuild_crop_indexes();
}

void EconomySim::set_crop_initial_stock(const String& city_id,
		const String& product, double tons) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const int index = product_index(product);
	if (index < 0) {
		return;
	}
	sim_core_.set_crop_initial_stock(std::string(city_id.utf8().get_data()),
		index, tons);
}

void EconomySim::set_crop_loss(const String& product, double loss_pct,
		double storage_share_pct) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const int index = product_index(product);
	if (index < 0) {
		return;
	}
	sim_core_.set_crop_loss(index, loss_pct, storage_share_pct);
}

void EconomySim::set_shelf_life(const String& product, int days) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const int index = product_index(product);
	if (index < 0) {
		return;
	}
	sim_core_.set_shelf_life(index, days);
}

void EconomySim::set_fertilizer_n(const String& city_id, const String& product,
		double n_kg_ha) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const int index = product_index(product);
	if (index < 0) {
		return;
	}
	sim_core_.set_fertilizer_n(std::string(city_id.utf8().get_data()), index,
		n_kg_ha);
}

void EconomySim::set_storage_quality(const String& city_id, double quality) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	sim_core_.set_storage_quality(std::string(city_id.utf8().get_data()),
		quality);
}

// ---------------------------------------------------------------------------
// G2 â€” ARMAZENAGEM (D-100)
// ---------------------------------------------------------------------------

void EconomySim::set_storage_capacity(const String& city_id,
		const String& type, const String& owner, double tons) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	int type_index = -1;
	int scope = -1;
	if (!ogs::storage_type_from_name(std::string(type.utf8().get_data()),
			type_index)
			|| !ogs::storage_scope_from_name(std::string(owner.utf8().get_data()),
				scope)) {
		return;
	}
	sim_core_.set_storage_capacity(std::string(city_id.utf8().get_data()),
		type_index, scope, tons);
}

void EconomySim::set_storage_product_type(const String& product,
		const String& type) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const int resolved_product = product_index(product);
	int type_index = -1;
	if (resolved_product < 0
			|| !ogs::storage_type_from_name(std::string(type.utf8().get_data()),
				type_index)) {
		return;
	}
	sim_core_.set_storage_product_type(resolved_product, type_index);
}

void EconomySim::set_storage_stock(const String& city_id, const String& type,
		const String& owner, const String& product, double tons) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	int type_index = -1;
	int scope = -1;
	const int resolved_product = product_index(product);
	if (resolved_product < 0
			|| !ogs::storage_type_from_name(std::string(type.utf8().get_data()),
				type_index)
			|| !ogs::storage_scope_from_name(std::string(owner.utf8().get_data()),
				scope)) {
		return;
	}
	sim_core_.set_storage_stock(std::string(city_id.utf8().get_data()),
		type_index, scope, resolved_product, tons);
}

Dictionary EconomySim::storage_build(const String& city_id,
		const String& type, const String& owner, double tons) {
	int type_index = -1;
	int scope = -1;
	if (!ogs::storage_type_from_name(std::string(type.utf8().get_data()),
			type_index)
			|| !ogs::storage_scope_from_name(std::string(owner.utf8().get_data()),
				scope)) {
		Dictionary result;
		result["started"] = false;
		result["error"] = "tipo/dono invÃ¡lido";
		return result;
	}
	ogs::Command cmd;
	cmd.type = ogs::CommandType::StorageBuild;
	cmd.s[0] = to_std(city_id);
	cmd.i[0] = type_index;
	cmd.i[1] = scope;
	cmd.v[0] = tons;
	return dispatch_command(cmd);
}

Dictionary EconomySim::storage_trade(const String& city_id,
		const String& type, const String& owner, const String& product,
		double tons, bool buy, bool relief) {
	int type_index = -1;
	int scope = -1;
	const int resolved_product = product_index(product);
	if (resolved_product < 0
			|| !ogs::storage_type_from_name(std::string(type.utf8().get_data()),
				type_index)
			|| !ogs::storage_scope_from_name(std::string(owner.utf8().get_data()),
				scope)) {
		Dictionary result;
		result["ok"] = false;
		result["error"] = "tipo/dono/produto invÃ¡lido";
		return result;
	}
	ogs::Command cmd;
	cmd.type = ogs::CommandType::StorageTrade;
	cmd.s[0] = to_std(city_id);
	cmd.i[0] = type_index;
	cmd.i[1] = scope;
	cmd.v[0] = tons;
	cmd.v[1] = static_cast<double>(resolved_product);
	cmd.b = buy;
	// `relief` nÃ£o cabe no `b` (que jÃ¡ Ã© o lado); usa o slot s[1].
	cmd.s[1] = relief ? "relief" : "paid";
	return dispatch_command(cmd);
}

Dictionary EconomySim::get_agriculture_status() const {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Dictionary out;
	if (ws_.crop_plans.empty()) {
		return out;
	}
	const ogs::GameDate today = ogs::game_date_from_day(ws_.day);
	Dictionary date;
	date["year"] = today.year;
	date["month"] = today.month;
	date["day"] = today.day;
	date["doy"] = today.doy;
	out["day"] = ws_.day;
	out["date"] = date;
	out["delivered_today_t"] = ws_.agri_delivered_today;
	out["harvests_today"] = ws_.agri_harvests_today;

	// Slot de cada produto na lista da cadeia (Ã­ndice do livro de perdas).
	std::vector<int> slot_of(static_cast<std::size_t>(ws_.products), -1);
	for (std::size_t slot = 0; slot < ws_.agri_chain.size(); ++slot) {
		const int product = ws_.agri_chain[slot];
		if (product >= 0 && product < ws_.products) {
			slot_of[static_cast<std::size_t>(product)] =
				static_cast<int>(slot);
		}
	}
	// Slot de cada produto na cesta alimentar (consumo em espÃ©cie das famÃ­lias
	// agrÃ­colas, que NÃƒO passa por `market.demand`).
	std::vector<int> food_slot_of(static_cast<std::size_t>(ws_.products), -1);
	for (std::size_t slot = 0; slot < ws_.food_products.size(); ++slot) {
		const int product = ws_.food_products[slot];
		if (product >= 0 && product < ws_.products) {
			food_slot_of[static_cast<std::size_t>(product)] =
				static_cast<int>(slot);
		}
	}

	// Acumuladores por naÃ§Ã£o (std::map: ordem determinÃ­stica no dump).
	struct CropAcc {
		double stock_t = 0.0;
		double demand_t_day = 0.0;
		double imported_today_t = 0.0;
		double exported_today_t = 0.0;
		double arrived_today_t = 0.0;
		double in_kind_today_t = 0.0;
		double production_year_t = 0.0;
		double loss_month_t = 0.0;
		double entry_loss_month_t = 0.0;
		double storage_loss_month_t = 0.0;
		double delivered_today_t = 0.0;
		double next_harvest_days = -1.0;
		int next_harvest_doy = -1;
		bool harvesting = false;
	};
	struct NationAcc {
		double quality_sum = 0.0;
		int quality_count = 0;
		std::map<int, CropAcc> crops;  // produto -> acumulador
	};
	std::map<std::string, NationAcc> nations;

	for (std::size_t city_index = 0; city_index < ws_.cities.size();
			++city_index) {
		const City& city = ws_.cities[city_index];
		const double quality = ogs::SimCore::city_storage_quality(city);
		for (const ogs::CropPlan& plan : ws_.crop_plans) {
			if (plan.city_index != static_cast<int>(city_index)) {
				continue;
			}
			NationAcc& nation = nations[city.nation];
			nation.quality_sum += quality;
			++nation.quality_count;
			CropAcc& crop = nation.crops[plan.product];
			const double stock = city.markets[plan.product].stock;
			crop.stock_t += stock;
			crop.demand_t_day += city.markets[plan.product].demand;
			crop.imported_today_t += city.imported[plan.product];
			crop.exported_today_t += city.exported[plan.product];
			crop.arrived_today_t += city.agri_arrived[plan.product];
			// Consumo em ESPÃ‰CIE das famÃ­lias agrÃ­colas: sai do estoque do
			// mercado da cidade sem entrar em `market.demand` (consumption.cpp).
			// Sem ele a conta da entressafra nÃ£o fecha.
			const int food_slot = plan.product >= 0
				&& plan.product < ws_.products
				? food_slot_of[static_cast<std::size_t>(plan.product)] : -1;
			if (food_slot >= 0 && food_slot < kMaxFoodProducts) {
				crop.in_kind_today_t += city.agri_in_kind[food_slot];
			}
			crop.delivered_today_t += plan.delivered_today_t;
			if (plan.delivered_today_t > 0.0) {
				crop.harvesting = true;
			}
			int chain_slot = plan.product >= 0 && plan.product < ws_.products
				? slot_of[static_cast<std::size_t>(plan.product)] : -1;
			if (chain_slot >= 0) {
				const double storage_loss = city.agri_loss_perishable[
						static_cast<std::size_t>(chain_slot)]
					+ city.agri_loss_storage[
						static_cast<std::size_t>(chain_slot)];
				crop.loss_month_t += storage_loss;
				crop.storage_loss_month_t += storage_loss;
			}
			for (int season = 0; season < plan.season_count; ++season) {
				const ogs::CropSeasonPlan& window = plan.seasons[season];
				const ogs::CropSeasonState& state = plan.state[season];
				crop.loss_month_t += state.loss_month_t;
				crop.entry_loss_month_t += state.loss_month_t;
				// ProduÃ§Ã£o da safra em curso: o acumulado da instÃ¢ncia que
				// contÃ©m hoje (janela que atravessa o ano continua contando).
				const int instance = ogs::window_start_year(today.year,
					today.doy, window.harvest_start_doy);
				const int position = ogs::window_position(today.doy,
					window.harvest_start_doy, window.harvest_end_doy);
				if (state.harvest_year == today.year
						|| (position >= 0 && state.harvest_year == instance)) {
					crop.production_year_t += state.delivered_t;
				}
				// PrÃ³xima colheita: 0 quando jÃ¡ estÃ¡ colhendo; senÃ£o os dias
				// atÃ© o inÃ­cio da prÃ³xima instÃ¢ncia da janela.
				double days = 0.0;
				if (position < 0) {
					const int year_days = ogs::game_year_days(today.year);
					days = window.harvest_start_doy >= today.doy
						? static_cast<double>(window.harvest_start_doy
							- today.doy)
						: static_cast<double>(year_days - today.doy
							+ window.harvest_start_doy);
				}
				if (crop.next_harvest_days < 0.0
						|| days < crop.next_harvest_days) {
					crop.next_harvest_days = days;
					crop.next_harvest_doy = window.harvest_start_doy;
				}
			}
		}
	}

	Array nations_out;
	for (const auto& pair : nations) {
		Dictionary entry;
		entry["nation"] = String(pair.first.c_str());
		entry["storage_quality"] = pair.second.quality_count > 0
			? pair.second.quality_sum / pair.second.quality_count : 0.0;
		Array crops;
		for (const auto& crop_pair : pair.second.crops) {
			const CropAcc& crop = crop_pair.second;
			Dictionary item;
			item["product"] = String(sim_core_.product_name(crop_pair.first));
			item["stock_t"] = crop.stock_t;
			item["demand_t_day"] = crop.demand_t_day;
			item["imported_today_t"] = crop.imported_today_t;
			item["exported_today_t"] = crop.exported_today_t;
			item["arrived_today_t"] = crop.arrived_today_t;
			item["in_kind_today_t"] = crop.in_kind_today_t;
			// Dias de consumo: estoque Ã· consumo do dia. Sem consumo
			// observado nÃ£o hÃ¡ nÃºmero (-1 = sem observaÃ§Ã£o).
			item["days_cover"] = crop.demand_t_day > 0.0
				? crop.stock_t / crop.demand_t_day : -1.0;
			item["next_harvest_days"] = crop.next_harvest_days;
			item["next_harvest_doy"] = crop.next_harvest_doy;
			item["production_year_t"] = crop.production_year_t;
			item["loss_month_t"] = crop.loss_month_t;
			item["entry_loss_month_t"] = crop.entry_loss_month_t;
			item["storage_loss_month_t"] = crop.storage_loss_month_t;
			item["delivered_today_t"] = crop.delivered_today_t;
			item["harvesting"] = crop.harvesting;
			crops.append(item);
		}
		entry["crops"] = crops;
		nations_out.append(entry);
	}
	out["nations"] = nations_out;
	// Totais acumulados desde o dia 0, por bem da cadeia (diagnÃ³stico do teste
	// e da UI; nÃ£o entram no save).
	Dictionary totals;
	Dictionary perishable;
	Dictionary storage;
	Dictionary entry;
	// Estoque do dia por bem da cadeia (TODAS as cidades, inclusive as sem
	// plano â€” Ã© o estoque que a perecibilidade e a armazenagem alcanÃ§am).
	Dictionary stock_by_product;
	for (const int product : ws_.agri_chain) {
		if (product < 0 || product >= ws_.products) {
			continue;
		}
		double stock = 0.0;
		for (const City& city : ws_.cities) {
			stock += std::max(0.0, city.markets[product].stock);
		}
		stock_by_product[String(sim_core_.product_name(product))] = stock;
	}
	for (const int product : ws_.agri_chain) {
		if (product < 0 || product >= ws_.products) {
			continue;
		}
		const String name(sim_core_.product_name(product));
		perishable[name] = ws_.agri_perishable_total[product];
		storage[name] = ws_.agri_storage_total[product];
		entry[name] = ws_.agri_entry_total[product];
	}
	totals["perishable_t"] = perishable;
	totals["storage_t"] = storage;
	totals["entry_t"] = entry;
	totals["stock_t"] = stock_by_product;
	out["totals"] = totals;
	return out;
}

bool EconomySim::set_deposit(const String& city_id, const String& product,
		double reserve_kt, double quality, double depth_m) {
	ogs::Command cmd;
	cmd.type = ogs::CommandType::SetDeposit;
	cmd.s[0] = to_std(city_id);
	cmd.s[1] = to_std(product);
	cmd.v[0] = reserve_kt;
	cmd.v[1] = quality;
	cmd.v[2] = depth_m;
	const Dictionary result = dispatch_command(cmd);
	if (result.has("queued")) {
		return true;
	}
	return static_cast<bool>(result.get("ok", false));
}

Dictionary EconomySim::get_balance_status() const {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Dictionary out;
	out["day"] = ws_.day;

	const auto sheet_to_dict = [](const ogs::balance::Sheet& sheet) {
		Dictionary row;
		row["cash"] = sheet.cash;
		row["receivables"] = sheet.receivables;
		row["payables"] = sheet.payables;
		row["debt"] = sheet.debt;
		row["financial_assets"] = sheet.financial_assets();
		row["financial_liabilities"] = sheet.financial_liabilities();
		row["inventory"] = sheet.inventory;
		row["fixed_capital"] = sheet.fixed_capital;
		row["property"] = sheet.property;
		row["net_worth"] = sheet.net_worth();
		return row;
	};

	const double total = money_total();
	const ogs::balance::Ledger book = ogs::balance::ledger(ws_, total);
	{
		Dictionary invariant;
		invariant["money_total"] = book.money_total;
		invariant["cash_total"] = book.cash_total();
		invariant["cash_residual"] = book.cash_residual();
		invariant["receivables"] = book.receivables;
		invariant["payables"] = book.payables;
		invariant["arrears_plants"] = book.arrears_plants;
		invariant["arrears_service"] = book.arrears_service;
		invariant["arrears_rural"] = book.arrears_rural;
		invariant["monetary_base"] = book.monetary_base;
		invariant["financial_sum"] = book.financial_sum();
		invariant["ok"] = std::abs(book.financial_sum()) <= 1e-3
			&& std::abs(book.cash_residual()) <= 1e-3;
		out["invariant"] = invariant;
	}

	Dictionary world;
	world["families"] = sheet_to_dict(ogs::balance::families(ws_));
	world["firms"] = sheet_to_dict(ogs::balance::firms(ws_));
	world["government"] = sheet_to_dict(ogs::balance::government(ws_));
	world["external"] = sheet_to_dict(ogs::balance::external(ws_));
	// C3 (D-107): o setor bancÃ¡rio (banco agregado por naÃ§Ã£o) e o balanÃ§o de
	// cada grupo. Sem crÃ©dito o setor Ã© zero e os grupos publicam dÃ­vida 0.
	{
		Dictionary row = sheet_to_dict(ogs::balance::banking(ws_));
		const ogs::bank::Totals totals = ogs::bank::totals(ws_);
		row["loans_to_groups"] = totals.loans;
		row["interest_arrears"] = totals.arrears;
		row["bc_borrowing"] = totals.bc_borrowing;
		row["capital"] = totals.capital;
		row["equity"] = totals.equity;
		row["writeoffs"] = totals.writeoffs;
		row["defaults"] = totals.defaults;
		world["banks"] = row;
	}
	{
		const ogs::balance::FirmSolvency solvency =
			ogs::balance::firm_solvency(ws_);
		Dictionary row;
		row["count"] = solvency.count;
		row["liquid"] = solvency.liquid;
		row["illiquid"] = solvency.illiquid;
		row["insolvent"] = solvency.insolvent;
		world["firms_solvency"] = row;
	}
	// C3 (D-107): balanÃ§o de cada grupo (ordem = `ws.groups`, estÃ¡vel).
	{
		const std::vector<ogs::balance::GroupSheet> sheets =
			ogs::balance::group_sheets(ws_);
		Array groups;
		for (const ogs::balance::GroupSheet& sheet : sheets) {
			Dictionary row = sheet_to_dict(sheet);
			row["id"] = String(sheet.id.c_str());
			row["name"] = String(sheet.name.c_str());
			row["origin_nation"] = String(sheet.origin_nation.c_str());
			row["sector"] = String(sheet.sector.c_str());
			row["arrears"] = sheet.payables;
			row["daily_cost"] = sheet.daily_cost;
			row["daily_profit"] = sheet.daily_profit;
			row["building_count"] = sheet.building_count;
			// Alavancagem = dÃ­vida Ã· (patrimÃ´nio lÃ­quido + dÃ­vida); sem
			// patrimÃ´nio e sem dÃ­vida Ã© 0 (ausÃªncia declarada, nÃ£o 100%).
			const double base = sheet.net_worth() + sheet.debt;
			row["leverage"] = std::abs(base) > 1e-9
				? sheet.debt / base : 0.0;
			row["inventory_basis"] = sheet.building_count > 0
				? "share of city market stock by capacity" : "no_buildings";
			groups.append(row);
		}
		world["groups"] = groups;
	}
	out["world"] = world;

	// Por naÃ§Ã£o: as mesmas contas, restritas Ã s cidades do paÃ­s. Caixas que
	// nÃ£o tÃªm naÃ§Ã£o (fundos de obra em voo, grupos sem origem declarada) ficam
	// FORA da soma por paÃ­s e aparecem em `unallocated` (nada Ã© rateado por
	// palpite).
	std::vector<std::string> nations;
	for (const ogs::City& city : ws_.cities) {
		if (std::find(nations.begin(), nations.end(), city.nation)
				== nations.end()) {
			nations.push_back(city.nation);
		}
	}
	std::sort(nations.begin(), nations.end());
	Dictionary by_nation;
	for (const std::string& nation : nations) {
		Dictionary row;
		row["families"] = sheet_to_dict(ogs::balance::families(ws_, nation));
		row["firms"] = sheet_to_dict(ogs::balance::firms(ws_, nation));
		row["government"] = sheet_to_dict(
			ogs::balance::government(ws_, nation));
		// O `foreign_cash` do nÃºcleo Ã© GLOBAL: a posiÃ§Ã£o do exterior nÃ£o Ã©
		// decomposta por paÃ­s (ausÃªncia declarada, nÃ£o rateio inventado).
		Dictionary external;
		external["decomposed"] = false;
		external["reason"] = "foreign_cash global no nÃºcleo";
		row["external"] = external;
		by_nation[String(nation.c_str())] = row;
	}
	out["by_nation"] = by_nation;
	out["by_nation_order"] = [&nations]() {
		Array order;
		for (const std::string& nation : nations) {
			order.append(String(nation.c_str()));
		}
		return order;
	}();
	out["unallocated"] = [&]() {
		Dictionary row;
		double group_cash = 0.0;
		for (const ogs::Group& group : ws_.groups) {
			if (group.origin_nation.empty()) {
				group_cash += group.cash;
			}
		}
		double construction_funds = 0.0;
		for (const ogs::ConstructionProject& project
				: ws_.construction_projects) {
			construction_funds += project.reserved;
		}
		row["group_cash_without_origin"] = group_cash;
		row["construction_funds"] = construction_funds;
		row["note"] = "caixas sem naÃ§Ã£o atribuÃ­vel (leitura)";
		return row;
	}();
	out["notes"] = [&]() {
		Dictionary notes;
		notes["monetary_base"] =
			"passivo do emissor: base monetÃ¡ria M0 medida por moeda (C2a/D-103); "
			"a emissÃ£o Ã© a C2b";
		notes["arrears"] =
			"salÃ¡rio atrasado = passivo da firma + ativo das famÃ­lias";
		notes["equity"] =
			"participaÃ§Ã£o em grupos (stratum_equity) nÃ£o entra: avaliaÃ§Ã£o jÃ¡ "
			"contida nos ativos dos grupos";
		notes["banks"] =
			"banco agregado por nacao (C3/D-107): credor minimo, sem deposito "
			"(M1 = M2 = M0) e sem capital semeado (ausencia declarada; C4)";
		return notes;
	}();
	return out;
}

Dictionary EconomySim::get_central_bank_status() const {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Dictionary out;
	out["day"] = ws_.day;

	std::vector<double> base_list(
		static_cast<std::size_t>(std::max(0, ws_.products)), 0.0);
	for (int product = 0; product < ws_.products; ++product) {
		base_list[static_cast<std::size_t>(product)] =
			sim_core_.base_price(product);
	}

	{
		Dictionary policy;
		policy["standard"] =
			ogs::central_bank::standard_name(ws_.monetary_standard);
		policy["reserve_currency"] = String(ws_.reserve_currency.c_str());
		policy["active"] = true;
		// D-105 + D-111: o juro da taxa bÃ¡sica no envelope de crÃ©dito da safra
		// (D-099) estÃ¡ ligado? Verdadeiro desde a D-111 (a C4 criou o credor).
		// Declarado em `data/definitions/economy/monetary_policy.json` e
		// conferido pelo teste.
		policy["harvest_credit_rate"] = ws_.harvest_credit_rate;
		// C2b2 (D-106): o padrÃ£o AGE (Bretton Woods e padrÃ£o ouro forÃ§am o peg
		// e a conversÃ£o do ouro). A troca Ã© sÃ³ por comando na fila A3.
		const ogs::exchange::Parameters params =
			ogs::exchange::parameters(ws_);
		policy["standard_forces_peg"] =
			ogs::exchange::standard_forces_peg(ws_);
		policy["band_width"] = params.band_width;
		policy["defence_share"] = params.defence_share;
		policy["gold_reserve_ratio"] = params.gold_reserve_ratio;
		policy["gold_conversion_basis"] =
			"catalogo goods.json, congelado no comando de padrao";
		// C2c (D-108): de onde veio o regime inicial de cada moeda.
		policy["regime_source"] = String(ws_.fx_regime_source.c_str());
		policy["regime_currencies"] =
			static_cast<int>(ws_.fx_regime.size());
		policy["note"] =
			"C2b1 (D-104): taxa bÃ¡sica por moeda (Taylor no BC independente; "
			"objetivo do governo no controlado) e emissÃ£o sÃ³ por compra de "
			"tÃ­tulos do governo pelo BC controlado. Bancos/redesconto sÃ£o C4.";
		out["monetary_policy"] = policy;
	}

	const double total = money_total();
	const ogs::central_bank::MoneyAllocation money =
		ogs::central_bank::allocate_money(ws_, total);
	{
		Dictionary invariant;
		invariant["money_total"] = money.total;
		invariant["allocated"] = money.allocated;
		invariant["unallocated"] = money.unallocated();
		invariant["residual"] = money.residual();
		invariant["ok"] = std::abs(money.residual())
			<= std::max(1e-3, std::abs(total) * 1e-12);
		out["invariant"] = invariant;
	}

	std::string gold_basis;
	const double gold_price =
		ogs::central_bank::gold_price(ws_, base_list, &gold_basis);
	{
		Dictionary gold;
		gold["price"] = gold_price;
		gold["price_basis"] = String(gold_basis.c_str());
		gold["unallocated_tonnes"] = ws_.reserve_gold_unallocated;
		gold["declared"] = ws_.reserve_gold_declared;
		out["gold"] = gold;
	}

	const std::vector<ogs::central_bank::CurrencyBank> banks =
		ogs::central_bank::banks(ws_, money, base_list);
	Dictionary currencies;
	double m0_sum = 0.0;
	double m1_sum = 0.0;
	double m2_sum = 0.0;
	double gold_tonnes_sum = 0.0;
	double gold_value_sum = 0.0;
	for (const ogs::central_bank::CurrencyBank& bank : banks) {
		Dictionary row;
		row["code"] = String(bank.code.c_str());
		row["regime"] = ogs::central_bank::regime_name(bank.regime);
		row["bank_independence"] =
			ogs::central_bank::independence_name(bank.independence);
		// C2b1 (D-104): polÃ­tica ATIVA da moeda. `bank_independence` acima Ã© o
		// valor SALVO (pode ser `no_data`); o efetivo resolve no default
		// declarado do jogo. A taxa, a leitura e a emissÃ£o saem do estado.
		{
			const auto state = ws_.monetary.find(bank.code);
			Dictionary active;
			const int effective = ogs::monetary::effective_independence(ws_,
				bank.code);
			active["independence_saved"] =
				ogs::central_bank::independence_name(bank.independence);
			active["independence_effective"] =
				ogs::central_bank::independence_name(effective);
			active["rate"] = ogs::monetary::effective_rate(ws_, bank.code);
			active["target_rate"] = state != ws_.monetary.end()
				? state->second.target_rate : ogs::monetary::kNeutralRate;
			active["target_inflation"] = ogs::monetary::kTargetInflation;
			active["neutral_rate"] = ogs::monetary::kNeutralRate;
			active["max_step_per_day"] = ogs::monetary::kMaxRateStepPerDay;
			active["smoothing"] = ogs::monetary::kRateSmoothing;
			// D-113: a suavizaÃ§Ã£o Ã© por decisÃ£o mensal; a taxa anda pelo
			// equivalente diÃ¡rio. Os dois valores vÃ£o publicados para a
			// mediÃ§Ã£o nÃ£o precisar recalcular.
			active["smoothing_per_day"] = 1.0 - std::pow(
				1.0 - ogs::monetary::kRateSmoothing,
				1.0 / static_cast<double>(
					ogs::central_bank::kCpiSampleDays));
			active["gap_baseline_day"] = ogs::monetary::kGapBaselineDay;
			active["annualize_min_span_days"] =
				ogs::central_bank::kAnnualizeMinSpanDays;
			active["inflation_weight"] =
				ogs::monetary::kTaylorInflationWeight;
			active["gap_weight"] = ogs::monetary::kTaylorGapWeight;
			active["rate_floor"] = ogs::monetary::kRateFloor;
			active["rate_cap"] = ogs::monetary::kRateCap;
			active["deficit_today"] = state != ws_.monetary.end()
				? state->second.deficit_measured : 0.0;
			active["unfunded_today"] = state != ws_.monetary.end()
				? state->second.unfunded : 0.0;
			active["gap"] = state != ws_.monetary.end()
				? state->second.gap : 0.0;
			// D-113: separa "hiato ausente" de "hiato zero".
			active["has_gap"] = ogs::monetary::has_gap_reading(ws_, bank.code);
			active["has_inflation"] = state != ws_.monetary.end()
				&& state->second.has_inflation;
			active["inflation_used"] = state != ws_.monetary.end()
				? state->second.inflation_used : 0.0;
			active["emission_today"] = state != ws_.monetary.end()
				? state->second.emission_today : 0.0;
			active["emission_total"] = state != ws_.monetary.end()
				? state->second.emission : 0.0;
			active["securities"] = state != ws_.monetary.end()
				? state->second.securities : 0.0;
			active["last_day"] = state != ws_.monetary.end()
				? state->second.last_day : -1;
			row["policy"] = active;
		}
		Array nations;
		for (const std::string& nation : bank.nations) {
			nations.append(String(nation.c_str()));
		}
		row["nations"] = nations;
		row["nations_count"] = static_cast<int>(bank.nations.size());
		// Passivo (base monetÃ¡ria) e agregados medidos.
		row["currency_in_circulation"] = bank.currency_in_circulation;
		row["bank_reserves"] = bank.bank_reserves;
		row["m0"] = bank.m0;
		row["m1"] = bank.m1;
		row["m2"] = bank.m2;
		{
			Dictionary assets;
			assets["gold_tonnes"] = bank.gold_tonnes;
			assets["gold_value"] = bank.gold_value;
			assets["reserve_currency_holdings"] =
				bank.reserve_currency_holdings;
			assets["securities"] = bank.securities;
			assets["financial_assets"] = bank.financial_assets();
			assets["total"] = bank.total_assets();
			row["assets"] = assets;
		}
		{
			Dictionary liabilities;
			liabilities["m0"] = bank.liabilities();
			row["liabilities"] = liabilities;
		}
		row["net_financial_worth"] = bank.net_financial_worth();
		// C2b2 (D-106): regime efetivo, paridade, banda, reservas, cobertura e
		// eventos da moeda. `funded` falso = fixo/banda administrativo (o F5 jÃ¡
		// segurava a taxa sem reserva).
		{
			Dictionary fx;
			fx["regime_saved"] =
				ogs::central_bank::regime_name(bank.regime_saved);
			fx["regime_effective"] =
				ogs::central_bank::regime_name(bank.regime_effective);
			fx["parity"] = bank.parity;
			fx["band_width"] = bank.band_width;
			const double rate = sim_core_.fx_rate(
				bank.nations.empty() ? std::string() : bank.nations.front());
			fx["rate"] = rate;
			fx["deviation"] = bank.parity > 0.0
				? rate / bank.parity - 1.0 : 0.0;
			fx["reserves"] = bank.reserve_currency_holdings;
			fx["reserves_local"] =
				bank.reserve_currency_holdings * bank.parity;
			fx["funded"] = bank.funded;
			fx["intervention_today"] = bank.intervention_today;
			fx["devaluations"] = bank.devaluations;
			fx["last_event_day"] = bank.last_event_day;
			fx["coverage"] = bank.coverage_ratio;
			fx["conversion_price"] = bank.conversion_price;
			// C2c (D-108): de onde veio o regime, contra o que ele prende a
			// taxa, quem Ã© o emissor e se a moeda estÃ¡ travada (uniÃ£o).
			const auto info = ws_.fx_regime.find(bank.code);
			const auto state = ws_.fx.find(bank.code);
			fx["regime_source"] = info != ws_.fx_regime.end()
				? String(info->second.source.c_str())
				: String(ws_.fx_regime_source.empty()
					? "sem regime real carregado" : ws_.fx_regime_source.c_str());
			fx["anchor"] = info != ws_.fx_regime.end()
				? String(info->second.anchor.c_str()) : String();
			fx["anchor_type"] = info != ws_.fx_regime.end()
				? String(info->second.anchor_type.c_str()) : String("none");
			fx["anchor_source"] = info != ws_.fx_regime.end()
				? String(info->second.anchor_source.c_str()) : String();
			fx["issuer"] = info != ws_.fx_regime.end()
				? String(info->second.issuer.c_str()) : String();
			fx["locked"] = info != ws_.fx_regime.end()
				&& info->second.locked;
			{
				Dictionary basket;
				std::map<std::string, double> weights;
				if (info != ws_.fx_regime.end()) {
					weights = info->second.basket;
				}
				if (weights.empty() && state != ws_.fx.end()) {
					weights = state->second.basket;
				}
				for (const auto& unit : weights) {
					basket[String(unit.first.c_str())] = unit.second;
				}
				fx["anchor_basket"] = basket;
				fx["basket_source"] = state != ws_.fx.end()
					? String(state->second.basket_source.c_str()) : String();
			}
			{
				const auto found_currency = ws_.currency_by_code.find(bank.code);
				fx["target_rate"] = found_currency == ws_.currency_by_code.end()
					? 0.0 : ogs::exchange::target_rate(ws_,
						ws_.currencies[static_cast<std::size_t>(
							found_currency->second)]);
				fx["anchor_value"] =
					found_currency == ws_.currency_by_code.end() ? 0.0
					: ogs::exchange::anchor_value(ws_,
						ws_.currencies[static_cast<std::size_t>(
							found_currency->second)]);
			}
			row["fx"] = fx;
		}
		currencies[String(bank.code.c_str())] = row;
		m0_sum += bank.m0;
		m1_sum += bank.m1;
		m2_sum += bank.m2;
		gold_tonnes_sum += bank.gold_tonnes;
		gold_value_sum += bank.gold_value;
	}
	out["currencies"] = currencies;
	{
		Dictionary totals;
		totals["currencies"] = static_cast<int>(banks.size());
		totals["m0"] = m0_sum;
		totals["m1"] = m1_sum;
		totals["m2"] = m2_sum;
		totals["gold_tonnes"] = gold_tonnes_sum;
		totals["gold_value"] = gold_value_sum;
		totals["bank_reserves"] = 0.0;
		totals["securities"] = 0.0;
		totals["emission_today"] = ws_.emission_today;
		double emission_total = 0.0;
		double securities_total = 0.0;
		for (const auto& row : ws_.monetary) {
			emission_total += row.second.emission;
			securities_total += row.second.securities;
		}
		totals["securities"] = securities_total;
		totals["emission_total"] = emission_total;
		double bonds_total = 0.0;
		for (const auto& row : ws_.gov_bonds) {
			bonds_total += row.second;
		}
		totals["government_debt"] = bonds_total;
		// C2b2 (D-106): reservas, fatia do cofre e contador de desvalorizaÃ§Ãµes.
		double reserves_total = 0.0;
		double vault_total = 0.0;
		int devaluations_total = 0;
		int funded_total = 0;
		for (const ogs::central_bank::CurrencyBank& bank : banks) {
			reserves_total += bank.reserve_currency_holdings;
			devaluations_total += bank.devaluations;
			if (bank.funded) {
				funded_total += 1;
			}
		}
		const int gold_index = sim_core_.product_index(std::string("gold"));
		if (gold_index >= 0) {
			for (const ogs::StorageSite& site : ws_.storage_sites) {
				if (site.type != ogs::STORAGE_VAULT
						|| site.scope != ogs::STORAGE_GOVERNMENT) {
					continue;
				}
				if (static_cast<std::size_t>(gold_index) < site.stock.size()) {
					vault_total += std::max(0.0,
						site.stock[static_cast<std::size_t>(gold_index)]);
				}
			}
		}
		totals["reserves"] = reserves_total;
		totals["funded_currencies"] = funded_total;
		totals["vault_gold_tonnes"] = vault_total;
		totals["devaluations"] = devaluations_total;
		out["totals"] = totals;
	}
	{
		// C2b2: anel de eventos do cÃ¢mbio (mais recente primeiro).
		Array events;
		for (auto it = ws_.fx_events.rbegin(); it != ws_.fx_events.rend();
				++it) {
			Dictionary entry;
			entry["day"] = it->day;
			entry["currency"] = String(it->currency.c_str());
			entry["type"] = String(it->type.c_str());
			entry["old_parity"] = it->old_parity;
			entry["new_parity"] = it->new_parity;
			entry["reserves_used"] = it->reserves_used;
			events.append(entry);
		}
		out["fx_events"] = events;
	}
	{
		// C2b1: dÃ­vida pÃºblica por naÃ§Ã£o (sÃ³ quem tem tÃ­tulo do BC).
		Dictionary bonds;
		for (const auto& row : ws_.gov_bonds) {
			bonds[String(row.first.c_str())] = row.second;
		}
		out["government_bonds"] = bonds;
	}

	// CPI por paÃ­s: o Ã­ndice Ã© o MESMO do IPC do G2 (uma dona sÃ³). A inflaÃ§Ã£o
	// de 12 meses vem da sÃ©rie mensal; sem 12 meses, `has_12m` Ã© falso â€”
	// ausÃªncia declarada, nunca um nÃºmero inventado.
	const std::map<std::string, ogs::central_bank::CpiRow> cpi =
		ogs::central_bank::cpi_by_nation(ws_, base_list);
	Dictionary cpi_rows;
	int cpi_nations = 0;
	int with_12m = 0;
	double cpi_sum = 0.0;
	for (const auto& entry : cpi) {
		Dictionary row;
		row["index"] = entry.second.index;
		row["weight"] = entry.second.weight;
		const auto found = ws_.cpi_history.find(entry.first);
		const ogs::central_bank::Inflation inflation =
			found != ws_.cpi_history.end()
				? ogs::central_bank::inflation_of(found->second)
				: ogs::central_bank::Inflation{};
		row["observations"] = inflation.observations;
		row["span_days"] = inflation.span_days;
		row["has_12m"] = inflation.has_12m;
		if (inflation.has_12m) {
			row["inflation_12m"] = inflation.rate_12m;
			++with_12m;
		}
		row["has_annualized"] = inflation.has_annualized;
		if (inflation.has_annualized) {
			row["inflation_annualized"] = inflation.annualized;
		}
		row["last_sample_day"] = found != ws_.cpi_history.end()
			&& found->second.count > 0
			? found->second.sample_day[found->second.count - 1] : -1;
		cpi_rows[String(entry.first.c_str())] = row;
		++cpi_nations;
		cpi_sum += entry.second.index;
	}
	out["cpi"] = cpi_rows;
	{
		Dictionary cpi_totals;
		cpi_totals["nations"] = cpi_nations;
		cpi_totals["with_12m"] = with_12m;
		cpi_totals["mean_index"] = cpi_nations > 0
			? cpi_sum / static_cast<double>(cpi_nations) : 0.0;
		cpi_totals["sample_days"] =
			static_cast<int>(ogs::central_bank::kCpiSampleDays);
		cpi_totals["last_sample_day"] = ws_.cpi_last_observe_day;
		cpi_totals["basis"] =
			"cesta de alimentos do G2 (fator calÃ³rico x preÃ§o/base x necessidade "
			"dos quintis); 12 meses = 360 dias do modelo";
		out["cpi_totals"] = cpi_totals;
	}

	{
		Dictionary row;
		row["sector_cash"] = money.sector_cash;
		row["group_cash_without_origin"] = money.group_cash;
		row["project_cash_without_city"] = money.project_cash;
		row["external_foreign_cash"] = money.external;
		row["nation_without_currency"] = money.no_currency;
		row["total"] = money.unallocated();
		row["note"] =
			"caixas sem naÃ§Ã£o atribuÃ­vel (o exterior do nÃºcleo Ã© global); nada "
			"Ã© rateado por palpite";
		out["unallocated"] = row;
	}

	{
		Dictionary notes;
		notes["banks"] =
			"sem banco (C4): reservas bancÃ¡rias 0, entÃ£o M1 = M2 = M0";
		notes["securities"] =
			"tÃ­tulos 0: nÃ£o hÃ¡ dÃ­vida pÃºblica no estado da C1 (C3/C4)";
		notes["reserve_currency_holdings"] =
			"0: o exterior do nÃºcleo Ã© um Ãºnico foreign_cash global, nÃ£o "
			"decomposto por moeda (C2b)";
		notes["gold"] =
			"ouro = dado real da G2/D-101 por moeda do banco central; Ã© ativo "
			"REAL e nÃ£o entra na identidade financeira";
		notes["names"] =
			"nome institucional do BC vem do dado D-090 no GDScript; o nÃºcleo "
			"nÃ£o inventa nome";
		out["notes"] = notes;
	}
	return out;
}

double EconomySim::set_central_bank_gold(const Dictionary& tonnes_by_nation) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	ws_.reserve_gold_tonnes.clear();
	ws_.reserve_gold_unallocated = 0.0;
	double total = 0.0;
	const auto add = [&](const std::string& currency, double tonnes) {
		if (!std::isfinite(tonnes) || tonnes <= 0.0) {
			return;
		}
		if (!currency.empty()) {
			const auto found = ws_.currency_by_code.find(currency);
			if (found != ws_.currency_by_code.end()) {
				ws_.reserve_gold_tonnes[currency] += tonnes;
				total += tonnes;
				return;
			}
		}
		ws_.reserve_gold_unallocated += tonnes;
		total += tonnes;
	};
	const Array keys = tonnes_by_nation.keys();
	for (int index = 0; index < keys.size(); ++index) {
		const String nation = String(keys[index]);
		if (nation.is_empty()) {
			continue;
		}
		const double tonnes = double(tonnes_by_nation.get(nation, 0.0));
		if (!std::isfinite(tonnes) || tonnes <= 0.0) {
			continue;
		}
		// NaÃ§Ã£o â†’ moeda (o banco central Ã© POR MOEDA; a zona do euro e o CFA
		// somam as reservas dos membros, como no dado).
		std::string currency;
		const std::string tag(nation.utf8().get_data());
		const auto found = ws_.nation_currency.find(tag);
		if (found != ws_.nation_currency.end() && found->second >= 0
				&& found->second < static_cast<int>(ws_.currencies.size())) {
			currency = ws_.currencies[static_cast<std::size_t>(
				found->second)].code;
		}
		add(currency, tonnes);
	}
	ws_.reserve_gold_declared = total > 0.0;
	++data_version_;
	return total;
}

bool EconomySim::set_central_bank_policy(const String& currency,
		const String& independence) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const std::string code(currency.utf8().get_data());
	const auto found = ws_.currency_by_code.find(code);
	if (found == ws_.currency_by_code.end()) {
		return false;
	}
	const std::string name(independence.utf8().get_data());
	const int value = ogs::central_bank::independence_from_name(name,
		ogs::central_bank::kIndependenceNoData);
	// Nome invÃ¡lido nÃ£o vira `no_data` por acidente: exige um dos trÃªs nomes.
	if (name != "no_data" && name != "independent" && name != "controlled") {
		return false;
	}
	ws_.currencies[static_cast<std::size_t>(found->second)]
		.bank_independence = value;
	status_invalidate();
	return true;
}

// ---------------------------------------------------------------------------
// C2b2 (D-106) â€” regimes de cÃ¢mbio, padrÃ£o monetÃ¡rio e ouro fÃ­sico.
// ---------------------------------------------------------------------------
bool EconomySim::set_exchange_regime(const String& currency,
		const String& regime, double parity, double band_width,
		const String& nation) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const std::string code(currency.utf8().get_data());
	if (ws_.currency_by_code.find(code) == ws_.currency_by_code.end()) {
		return false;
	}
	const std::string name(regime.utf8().get_data());
	if (name != "floating" && name != "peg" && name != "band") {
		return false;
	}
	// D-108(c): moeda de uniÃ£o/dolarizada nÃ£o aceita comando de regime. A
	// recusa vale jÃ¡ no pedido (o motivo tambÃ©m volta na aplicaÃ§Ã£o do A3).
	const std::string acting(nation.utf8().get_data());
	const auto info = ws_.fx_regime.find(code);
	if (info != ws_.fx_regime.end()) {
		if (info->second.locked) {
			return false;
		}
		if (!acting.empty()) {
			const bool no_policy = std::find(ws_.fx_no_policy_nations.begin(),
				ws_.fx_no_policy_nations.end(), acting)
				!= ws_.fx_no_policy_nations.end();
			if (no_policy) {
				return false;
			}
			if (!info->second.issuer.empty()
					&& acting != info->second.issuer) {
				return false;
			}
		}
	}
	ogs::Command cmd;
	cmd.type = ogs::CommandType::SetExchangeRegime;
	cmd.s[0] = code;
	cmd.s[1] = name;
	cmd.s[2] = std::string(nation.utf8().get_data());
	cmd.v[0] = std::isfinite(parity) ? std::max(0.0, parity) : 0.0;
	cmd.v[1] = std::isfinite(band_width) ? band_width : -1.0;
	dispatch_command(cmd);
	return true;
}

double EconomySim::migrate_currency(const String& from, const String& to,
		double units_per_target) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const double converted = ogs::exchange::migrate_currency(ws_,
		std::string(from.utf8().get_data()), std::string(to.utf8().get_data()),
		units_per_target);
	if (converted > 0.0) {
		++status_static_version_;
		status_invalidate();
	}
	return converted;
}

bool EconomySim::set_monetary_standard(const String& standard) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const std::string name(standard.utf8().get_data());
	if (name != "fiat" && name != "bretton_woods" && name != "gold") {
		return false;
	}
	ogs::Command cmd;
	cmd.type = ogs::CommandType::SetMonetaryStandard;
	cmd.s[0] = name;
	dispatch_command(cmd);
	return true;
}

double EconomySim::set_central_bank_reserves(
		const Dictionary& anchor_by_currency) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	double total = 0.0;
	const Array codes = anchor_by_currency.keys();
	for (int index = 0; index < codes.size(); ++index) {
		const String key = codes[index];
		if (key.is_empty()) {
			continue;
		}
		const double value = double(anchor_by_currency.get(key, 0.0));
		if (!std::isfinite(value)) {
			continue;
		}
		const std::string code(key.utf8().get_data());
		if (ws_.currency_by_code.find(code) == ws_.currency_by_code.end()) {
			continue;
		}
		ogs::exchange::set_reserves(ws_, code, value);
		total += std::max(0.0, value);
	}
	status_invalidate();
	return total;
}

double EconomySim::seed_central_bank_reserves(int days) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const double total = ogs::exchange::seed_reserves_from_imports(ws_,
		days);
	status_invalidate();
	return total;
}

void EconomySim::set_exchange_policy(double band_width, double defence_share,
		double gold_reserve_ratio) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	ogs::exchange::set_parameters(ws_, band_width, defence_share,
		gold_reserve_ratio);
	status_invalidate();
}

// ---------------------------------------------------------------------------
// C3 (D-107) â€” crÃ©dito de empresa (banco agregado como credor).
// ---------------------------------------------------------------------------

Dictionary EconomySim::set_bank_credit(bool enabled) {
	ogs::Command cmd;
	cmd.type = ogs::CommandType::SetBankCredit;
	cmd.i[0] = enabled ? 1 : 0;
	return dispatch_command(cmd);
}

void EconomySim::set_bank_policy(double spread, double leverage_cap,
		double interest_cover, int default_days, double repay_share) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	ws_.bank_spread = std::max(0.0, spread);
	ws_.bank_leverage_cap = std::max(0.0, leverage_cap);
	ws_.bank_interest_cover = std::max(0.0, interest_cover);
	ws_.bank_default_days = std::max(0, default_days);
	ws_.bank_repay_share = std::clamp(repay_share, 0.0, 1.0);
	status_invalidate();
}

Dictionary EconomySim::set_bank_capital(const String& nation, double amount,
		const String& role) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const std::string role_key = to_std(role);
	const int role_index = ogs::bank::role_from_key(role_key);
	double paid = 0.0;
	std::string error;
	if (role_index < 0) {
		error = "papel de banco desconhecido";
	} else {
		ogs::bank::inject_capital(ws_, std::string(nation.utf8().get_data()),
			role_index, amount, &paid, &error);
	}
	status_invalidate();
	Dictionary out;
	out["ok"] = paid > 0.0;
	out["paid"] = paid;
	out["nation"] = nation;
	out["role"] = role;
	if (paid <= 0.0) {
		out["error"] = String(error.c_str());
	}
	return out;
}

void EconomySim::set_bank_definitions(const Dictionary& definitions) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	// Formato de banks.json: { nations: { ISO3: { public:{name,status},
	// private:{name,status}, foreign:{status,branch_candidate,origin_iso3} }}}.
	const Variant nations_variant = definitions.get("nations", Variant());
	if (nations_variant.get_type() != Variant::DICTIONARY) {
		return;
	}
	const Dictionary nations = nations_variant;
	const Array codes = nations.keys();
	// Dado com `null` (ausÃªncia declarada) vira "" â€” nunca "<null>".
	const auto text_of = [](const Dictionary& row, const char* key) {
		const Variant value = row.get(key, Variant());
		if (value.get_type() != Variant::STRING) {
			return std::string();
		}
		return std::string(String(value).utf8().get_data());
	};
	std::vector<ogs::bank::NameData> defs;
	defs.reserve(static_cast<std::size_t>(codes.size()));
	for (int index = 0; index < codes.size(); ++index) {
		const std::string code(String(codes[index]).utf8().get_data());
		const Variant entry_variant = nations.get(codes[index], Variant());
		if (code.empty() || entry_variant.get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary entry = entry_variant;
		ogs::bank::NameData data;
		data.nation = code;
		const Variant pub_variant = entry.get("public", Variant());
		if (pub_variant.get_type() == Variant::DICTIONARY) {
			const Dictionary pub = pub_variant;
			data.public_name = text_of(pub, "name");
			data.public_present = text_of(pub, "status") == "present";
		}
		const Variant pri_variant = entry.get("private", Variant());
		if (pri_variant.get_type() == Variant::DICTIONARY) {
			const Dictionary pri = pri_variant;
			data.private_name = text_of(pri, "name");
			data.private_present = text_of(pri, "status") == "present";
		}
		const Variant fgn_variant = entry.get("foreign", Variant());
		if (fgn_variant.get_type() == Variant::DICTIONARY) {
			const Dictionary fgn = fgn_variant;
			data.foreign_name = text_of(fgn, "branch_candidate");
			data.foreign_origin = text_of(fgn, "origin_iso3");
			data.foreign_present = text_of(fgn, "status") == "present";
		}
		defs.push_back(data);
	}
	bank_defs_ = defs;
	ogs::bank::apply_definitions(ws_, bank_defs_);
	status_invalidate();
}

Dictionary EconomySim::set_bank_crisis(const Dictionary& raw_config) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	// C6 (D-114): alavancas de configuraÃ§Ã£o (laboratÃ³rio/teste). `enabled` Ã©
	// estado e vai ao save; o resto Ã© regra (0 = default de sim/bank_crisis.h).
	// Aceita o JSON como estÃ¡ (seÃ§Ãµes capital/liquidity/losses/confidence/
	// lolr/insurance/resolution) e tambÃ©m as chaves planas.
	Dictionary config_flat = raw_config;
	const auto sub = [&raw_config](const char* section) {
		const Variant value = raw_config.get(section, Variant());
		return value.get_type() == Variant::DICTIONARY
			? Dictionary(value) : Dictionary();
	};
	const Dictionary capital = sub("capital");
	const Dictionary liquidity = sub("liquidity");
	const Dictionary losses = sub("losses");
	const Dictionary confidence = sub("confidence");
	const Dictionary weights = [&confidence]() {
		const Variant value = confidence.get("weights", Variant());
		return value.get_type() == Variant::DICTIONARY
			? Dictionary(value) : Dictionary();
	}();
	const Dictionary lolr = sub("lolr");
	const Dictionary insurance = sub("insurance");
	const Dictionary resolution = sub("resolution");
	const auto copy = [&config_flat](const Dictionary& src,
			const char* from, const char* to) {
		if (config_flat.has(to) || !src.has(from)) {
			return;
		}
		config_flat[to] = src.get(from, Variant());
	};
	if (!config_flat.has("enabled") && raw_config.has("enabled")) {
		config_flat["enabled"] = raw_config.get("enabled", Variant());
	}
	copy(capital, "min_ratio", "capital_min");
	copy(capital, "credit_cut_floor", "capital_cut_floor");
	copy(liquidity, "min_ratio", "liquidity_min");
	copy(losses, "confidence_scale", "loss_scale");
	copy(losses, "ema_days", "loss_ema_days");
	copy(confidence, "start", "confidence_start");
	copy(confidence, "smoothing", "smoothing");
	copy(confidence, "run_threshold", "run_threshold");
	copy(confidence, "run_share", "run_share");
	copy(confidence, "flight_to_quality", "flight_to_quality");
	copy(weights, "solvency", "w_solvency");
	copy(weights, "liquidity", "w_liquidity");
	copy(weights, "losses", "w_losses");
	copy(weights, "contagion", "w_contagion");
	copy(weights, "market", "w_market");
	copy(lolr, "spread", "lolr_spread");
	copy(lolr, "collateral_haircut", "lolr_haircut");
	copy(lolr, "max_share_of_assets", "lolr_max_assets");
	copy(insurance, "insured_share", "insured_share");
	copy(insurance, "premium_daily", "premium_daily");
	copy(insurance, "fund_target_ratio", "fund_target_ratio");
	copy(resolution, "treasury_debt_rescue", "treasury_debt_rescue");
	copy(resolution, "takeover_public", "takeover_public");
	copy(resolution, "capital_flight_share", "capital_flight_share");
	copy(resolution, "insolvency_tolerance", "insolvency_tolerance");
	copy(resolution, "insolvency_tolerance_ratio",
		"insolvency_tolerance_ratio");
	const Dictionary& config = config_flat;
	const auto number = [&config](const char* key) {
		return double(config.get(key, 0.0));
	};
	if (config.has("enabled")) {
		ws_.bank_crisis.enabled = bool(config.get("enabled", true));
	}
	if (config.has("capital_min")) {
		ws_.bank_crisis.capital_min = std::max(0.0,
			number("capital_min"));
	}
	if (config.has("capital_cut_floor")) {
		ws_.bank_crisis.capital_cut_floor = std::clamp(
			number("capital_cut_floor"), 0.0, 1.0);
	}
	// RESERVADO: `liquidity_min` Ã© publicado/salvo, mas nenhuma regra o
	// consome (o sinal de liquidez da confianÃ§a usa a referÃªncia do banco).
	if (config.has("liquidity_min")) {
		ws_.bank_crisis.liquidity_min = std::max(0.0,
			number("liquidity_min"));
	}
	if (config.has("loss_scale")) {
		ws_.bank_crisis.loss_scale = std::max(0.0, number("loss_scale"));
	}
	if (config.has("loss_ema_days")) {
		ws_.bank_crisis.loss_ema_days = std::max(0.0,
			number("loss_ema_days"));
	}
	if (config.has("smoothing")) {
		ws_.bank_crisis.smoothing = std::clamp(number("smoothing"),
			0.0, 1.0);
	}
	if (config.has("run_threshold")) {
		ws_.bank_crisis.run_threshold = std::max(0.0,
			number("run_threshold"));
	}
	if (config.has("run_share")) {
		ws_.bank_crisis.run_share = std::clamp(number("run_share"),
			0.0, 1.0);
	}
	if (config.has("flight_to_quality")) {
		ws_.bank_crisis.flight_to_quality = std::clamp(
			number("flight_to_quality"), 0.0, 1.0);
	}
	if (config.has("lolr_spread")) {
		ws_.bank_crisis.lolr_spread = std::max(0.0,
			number("lolr_spread"));
	}
	if (config.has("lolr_haircut")) {
		ws_.bank_crisis.lolr_haircut = std::clamp(
			number("lolr_haircut"), 0.0, 1.0);
	}
	if (config.has("lolr_max_assets")) {
		ws_.bank_crisis.lolr_max_assets = std::clamp(
			number("lolr_max_assets"), 0.0, 1.0);
	}
	if (config.has("insured_share")) {
		ws_.bank_crisis.insured_share = std::clamp(
			number("insured_share"), 0.0, 1.0);
	}
	if (config.has("premium_daily")) {
		ws_.bank_crisis.premium_daily = std::max(0.0,
			number("premium_daily"));
	}
	if (config.has("fund_target_ratio")) {
		ws_.bank_crisis.fund_target_ratio = std::max(0.0,
			number("fund_target_ratio"));
	}
	if (config.has("capital_flight_share")) {
		ws_.bank_crisis.capital_flight_share = std::clamp(
			number("capital_flight_share"), 0.0, 1.0);
	}
	if (config.has("w_solvency")) {
		ws_.bank_crisis.w_solvency = std::max(0.0, number("w_solvency"));
	}
	if (config.has("w_liquidity")) {
		ws_.bank_crisis.w_liquidity = std::max(0.0,
			number("w_liquidity"));
	}
	if (config.has("w_losses")) {
		ws_.bank_crisis.w_losses = std::max(0.0, number("w_losses"));
	}
	if (config.has("w_contagion")) {
		ws_.bank_crisis.w_contagion = std::max(0.0,
			number("w_contagion"));
	}
	if (config.has("w_market")) {
		ws_.bank_crisis.w_market = std::max(0.0, number("w_market"));
	}
	if (config.has("insolvency_tolerance")) {
		ws_.bank_crisis.insolvency_tolerance = std::max(0.0,
			number("insolvency_tolerance"));
	}
	if (config.has("insolvency_tolerance_ratio")) {
		ws_.bank_crisis.insolvency_tolerance_ratio = std::max(0.0,
			number("insolvency_tolerance_ratio"));
	}
	if (config.has("treasury_debt_rescue")) {
		ws_.bank_crisis.treasury_debt_rescue = bool(
			config.get("treasury_debt_rescue", true));
	}
	if (config.has("takeover_public")) {
		ws_.bank_crisis.takeover_public = bool(
			config.get("takeover_public", true));
	}
	status_invalidate();
	const ogs::bank_crisis::Policy pol = ogs::bank_crisis::policy(ws_);
	Dictionary out;
	out["enabled"] = pol.enabled;
	out["capital_min"] = pol.capital_min;
	out["liquidity_min"] = pol.liquidity_min;
	out["loss_scale"] = pol.loss_scale;
	out["loss_ema_days"] = pol.loss_ema_days;
	out["smoothing"] = pol.smoothing;
	out["w_solvency"] = pol.w_solvency;
	out["w_liquidity"] = pol.w_liquidity;
	out["w_losses"] = pol.w_losses;
	out["w_contagion"] = pol.w_contagion;
	out["w_market"] = pol.w_market;
	out["run_threshold"] = pol.run_threshold;
	out["run_share"] = pol.run_share;
	out["flight_to_quality"] = pol.flight_to_quality;
	out["lolr_spread"] = pol.lolr_spread;
	out["lolr_haircut"] = pol.lolr_haircut;
	out["lolr_max_assets"] = pol.lolr_max_assets;
	out["insured_share"] = pol.insured_share;
	out["premium_daily"] = pol.premium_daily;
	out["fund_target_ratio"] = pol.fund_target_ratio;
	out["capital_flight_share"] = pol.capital_flight_share;
	out["insolvency_tolerance"] = pol.insolvency_tolerance;
	out["insolvency_tolerance_ratio"] = pol.insolvency_tolerance_ratio;
	out["treasury_debt_rescue"] = pol.treasury_debt_rescue;
	out["takeover_public"] = pol.takeover_public;
	return out;
}

Dictionary EconomySim::bank_shock(const String& bank_id, double loss,
		double loss_share) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const std::string id(bank_id.utf8().get_data());
	double written = 0.0;
	if (loss > 0.0) {
		written = ogs::bank_crisis::apply_loss(ws_, id, loss);
	} else if (loss_share > 0.0) {
		written = ogs::bank_crisis::apply_loss_share(ws_, id, loss_share);
	}
	status_invalidate();
	Dictionary out;
	out["ok"] = written > 0.0;
	out["bank"] = bank_id;
	out["written"] = written;
	out["loss_share"] = loss_share;
	return out;
}

Dictionary EconomySim::group_borrow(const String& group_id, double amount) {
	ogs::Command cmd;
	cmd.type = ogs::CommandType::GroupBorrow;
	cmd.s[0] = to_std(group_id);
	cmd.v[0] = amount;
	return dispatch_command(cmd);
}

Dictionary EconomySim::set_stock_exchange(const Dictionary& config) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	// C5 (D-112): alavancas de configuraÃ§Ã£o do laboratÃ³rio (nÃ£o sÃ£o feature de
	// jogador nesta fase). `enabled` Ã© estado e vai ao save; os parÃ¢metros sÃ£o
	// regra e 0 significa "usar o default declarado em sim/stock.h".
	if (config.has("enabled")) {
		ws_.stock_enabled = bool(config.get("enabled", true));
	}
	if (config.has("investment_channel")) {
		ws_.stock_investment_channel = bool(
			config.get("investment_channel", true));
	}
	if (config.has("risk_premium")) {
		ws_.stock_risk_premium = std::max(0.0,
			double(config.get("risk_premium", 0.0)));
	}
	if (config.has("payout_ratio")) {
		ws_.stock_payout_ratio = std::max(0.0,
			double(config.get("payout_ratio", 0.0)));
	}
	if (config.has("step")) {
		ws_.stock_step = std::max(0.0, double(config.get("step", 0.0)));
	}
	if (config.has("expectation_k")) {
		ws_.stock_expectation_k = std::max(0.0,
			double(config.get("expectation_k", 0.0)));
	}
	status_invalidate();
	Dictionary out;
	out["enabled"] = ws_.stock_enabled;
	out["investment_channel"] = ws_.stock_investment_channel;
	return out;
}

Dictionary EconomySim::get_stock_status() const {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Dictionary out;
	out["day"] = ws_.day;
	const ogs::stock::Policy pol = ogs::stock::policy(ws_);
	{
		Dictionary policy;
		policy["enabled"] = pol.enabled;
		policy["investment_channel"] = pol.investment_channel;
		policy["risk_premium"] = pol.risk_premium;
		policy["payout_ratio"] = pol.payout_ratio;
		policy["step"] = pol.step;
		policy["expectation_k"] = pol.expectation_k;
		policy["price_period_days"] = ogs::stock::kPricePeriodDays;
		policy["dividend_period_days"] = pol.dividend_period_days;
		policy["shares"] = pol.shares;
		policy["price_floor_rate"] = ogs::stock::kPriceFloorRate;
		policy["index_warmup_days"] = ogs::stock::kIndexWarmupDays;
		policy["profit_ema_k"] = ogs::stock::kProfitEmaK;
		policy["pe_floor"] = ogs::stock::kPeFloor;
		policy["pe_ceiling"] = ogs::stock::kPeCeiling;
		policy["rate_pass_through"] = ogs::stock::kRatePassThrough;
		// D-113 (calibraÃ§Ã£o prÃ©-C6): porte mÃ­nimo de listagem declarado.
		policy["min_listed_price"] = ogs::stock::kMinListedPrice;
		policy["listing_reason"] = "below_min_price";
		policy["rate_expected"] =
			"neutral + pass_through * (spot - neutral) (D-112 fix)";
		policy["profit_reference"] =
			"measured mean 1/n during warmup, then EMA (D-112 fix)";
		policy["name_source"] =
			"game_default (fictÃ­cio/genÃ©rico, sem marca real)";
		policy["source"] = "default de jogo (sim/stock.h)";
		// AusÃªncias declaradas desta fase.
		policy["trading"] = "absent (C5)";
		policy["ipo"] = "later";
		policy["regulation"] = "later";
		policy["crash"] = "C6";
		policy["holders"] = "households, banks, groups, foreign";
		policy["wealth_channel"] = "measured (C5)";
		policy["consumption_channel"] = "deferred (C5)";
		out["policy"] = policy;
	}
	const ogs::stock::Totals totals = ogs::stock::totals(ws_);
	{
		Dictionary row;
		row["market_cap"] = totals.market_cap;
		row["book_equity"] = totals.book_equity;
		row["holdings"] = totals.holdings;
		row["index_global"] = totals.index_global;
		row["dividends_paid"] = totals.dividends_paid;
		row["remittance"] = totals.remittance;
		row["remittance_origin"] = totals.remittance_origin;
		row["listed"] = totals.listed;
		row["unlisted"] = totals.unlisted;
		row["exchanges"] = totals.exchanges;
		// Identidade da bolsa: carteiras âˆ’ market cap (tem de ser 0).
		row["equity_identity"] = totals.market_cap - totals.holdings;
		out["totals"] = row;
	}
	// PatrimÃ´nio medido do grupo no Ãºltimo fechamento (base do q de Tobin).
	Array exchanges;
	for (const auto& row : ws_.stock_exchanges) {
		const ogs::StockExchange& exchange = row.second;
		Dictionary entry;
		entry["nation"] = String(exchange.nation.c_str());
		if (exchange.name.empty()) {
			entry["name"] = Variant();
		} else {
			entry["name"] = String(exchange.name.c_str());
		}
		entry["name_status"] = String(exchange.name_status.c_str());
		entry["currency"] = String(exchange.currency.c_str());
		entry["policy_rate"] = ogs::monetary::rate_for_nation(ws_,
			exchange.nation);
		entry["confidence"] = exchange.confidence;
		const ogs::stock::Signals values = ogs::stock::signals(ws_, exchange);
		Dictionary signals;
		if (values.has_inflation) {
			signals["inflation"] = values.inflation;
		} else {
			signals["inflation"] = Variant();
		}
		if (values.has_rate) {
			signals["rate"] = values.rate;
		} else {
			signals["rate"] = Variant();
		}
		if (values.has_production) {
			signals["production"] = values.production_growth;
		} else {
			signals["production"] = Variant();
		}
		if (values.has_bankruptcies) {
			signals["bankruptcies"] = values.bankruptcy_share;
		} else {
			signals["bankruptcies"] = Variant();
		}
		if (values.has_devaluations) {
			signals["devaluations"] = values.devaluations;
		} else {
			signals["devaluations"] = Variant();
		}
		if (values.has_unemployment) {
			signals["unemployment"] = values.unemployment;
		} else {
			signals["unemployment"] = Variant();
		}
		Array missing;
		for (const std::string& name : values.missing) {
			missing.append(String(name.c_str()));
		}
		signals["missing"] = missing;
		signals["raw"] = values.raw;
		signals["adaptive"] = "expectation_k per day (C5)";
		entry["signals"] = signals;
		entry["index"] = ogs::stock::index_of(ws_, exchange);
		entry["index_base"] = exchange.index_base;
		entry["index_forming"] = !(exchange.index_base > 0.0);
		entry["last_price_day"] = exchange.last_price_day;
		entry["dividends_paid"] = exchange.dividends_paid;
		entry["remittance"] = exchange.remittance;
		entry["remittance_origin"] = exchange.remittance_origin;
		// Top papÃ©is da praÃ§a por market cap (5) â€” determinÃ­stico no empate.
		std::vector<std::pair<double, std::string>> ranked;
		double cap = 0.0;
		int listed = 0;
		for (const auto& ticker_row : ws_.stock_tickers) {
			const ogs::StockTicker& ticker = ticker_row.second;
			if (ticker.exchange != exchange.nation || !ticker.listed) {
				continue;
			}
			const double value = ogs::stock::market_cap(ticker);
			cap += value;
			listed += 1;
			ranked.emplace_back(value, ticker.group_id);
		}
		std::sort(ranked.begin(), ranked.end(),
			[](const std::pair<double, std::string>& left,
					const std::pair<double, std::string>& right) {
				if (left.first != right.first) {
					return left.first > right.first;
				}
				return left.second < right.second;
			});
		Array top;
		const std::size_t keep = std::min<std::size_t>(5, ranked.size());
		for (std::size_t index = 0; index < keep; ++index) {
			const ogs::StockTicker* ticker =
				ogs::stock::find_ticker(ws_, ranked[index].second);
			Dictionary paper;
			paper["group_id"] = String(ranked[index].second.c_str());
			paper["market_cap"] = ranked[index].first;
			paper["price"] = ticker == nullptr ? 0.0 : ticker->price;
			top.append(paper);
		}
		entry["top"] = top;
		entry["market_cap"] = cap;
		entry["listed"] = listed;
		exchanges.append(entry);
	}
	out["exchanges"] = exchanges;
	Array tickers;
	for (const auto& row : ws_.stock_tickers) {
		const ogs::StockTicker& ticker = row.second;
		Dictionary entry;
		entry["group_id"] = String(ticker.group_id.c_str());
		entry["nation"] = String(ticker.nation.c_str());
		entry["exchange"] = String(ticker.exchange.c_str());
		entry["listed"] = ticker.listed;
		entry["reason"] = String(ticker.reason.c_str());
		entry["shares"] = ticker.shares;
		entry["price"] = ticker.price;
		entry["fundamental"] = ogs::stock::fundamental_price(ws_, ticker, pol);
		entry["market_cap"] = ogs::stock::market_cap(ticker);
		const double equity = ticker.book_equity;
		entry["book_equity"] = equity;
		entry["q"] = equity > 0.0
			? ogs::stock::market_cap(ticker) / equity : 0.0;
		entry["pe"] = ticker.profit_ema > 1e-9
			? ogs::stock::market_cap(ticker) / (ticker.profit_ema * 365.0)
			: 0.0;
		entry["profit_ema"] = ticker.profit_ema;
		entry["dividend_last"] = ticker.dividend_last;
		entry["dividend_limited"] = ticker.dividend_limited;
		entry["dividend_day"] = ticker.dividend_day;
		entry["listed_day"] = ticker.listed_day;
		entry["delisted_day"] = ticker.delisted_day;
		Dictionary holdings;
		holdings["households_high"] = ticker.holdings.households_high;
		Dictionary banks;
		for (const auto& holder : ticker.holdings.banks) {
			banks[String(holder.first.c_str())] = holder.second;
		}
		holdings["banks"] = banks;
		Dictionary groups;
		for (const auto& holder : ticker.holdings.groups) {
			groups[String(holder.first.c_str())] = holder.second;
		}
		holdings["groups"] = groups;
		holdings["foreign"] = ticker.holdings.foreign;
		entry["holdings"] = holdings;
		tickers.append(entry);
	}
	out["tickers"] = tickers;
	{
		Dictionary effects;
		effects["household_wealth"] = "measured: share holdings by nation";
		effects["investment"] = "q of Tobin (clamped 0,8..1,2)";
		effects["dividends"] = "monthly, paid from issuer cash (sum zero)";
		effects["foreign_remittance"] = "via exchange parity (C2c)";
		effects["money_creation"] = "none (asset price)";
		out["effects"] = effects;
	}
	return out;
}

Dictionary EconomySim::get_bank_status() const {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Dictionary out;
	out["day"] = ws_.day;
	const ogs::bank::Policy pol = ogs::bank::policy(ws_);
	{
		Dictionary policy;
		policy["enabled"] = pol.enabled;
		policy["spread"] = pol.spread;
		policy["leverage_cap"] = pol.leverage_cap;
		policy["interest_cover"] = pol.interest_cover;
		policy["default_days"] = pol.default_days;
		policy["repay_share"] = pol.repay_share;
		policy["source"] =
			"default de jogo (data/definitions/economy/bank_credit.json)";
		// AusÃªncias declaradas desta fase (C4/C6), nÃ£o nÃºmeros inventados.
		// C4 (D-110): depÃ³sitos Ã  vista sÃ£o dinheiro bancÃ¡rio (firmas), e o
		// capital de cada banco Ã© pago pelo SEU dono (Estado, famÃ­lias ou
		// exterior). ResoluÃ§Ã£o de banco insolvente Ã© C6.
		policy["deposits"] = "firm deposits (bank money, C4)";
		policy["deposits_owner"] = "C4";
		policy["resolution"] = "C6";
		policy["dividend_payout"] = ogs::bank::kDividendPayoutDefault;
		policy["dividend_period_days"] = ogs::bank::kDividendPeriodDays;
		Dictionary capital_source;
		capital_source["public"] = "treasury (state owner)";
		capital_source["private"] = "households (national owner)";
		capital_source["foreign"] = "external (foreign owner)";
		policy["capital_source"] = capital_source;
		// C6 (D-114): crise, corrida e resolucao. Todos os numeros sao default
		// de jogo declarado em data/definitions/economy/bank_crisis.json.
		const ogs::bank_crisis::Policy crisis = ogs::bank_crisis::policy(ws_);
		policy["crisis_enabled"] = crisis.enabled;
		policy["capital_min_ratio"] = crisis.capital_min;
		policy["liquidity_min_ratio"] = crisis.liquidity_min;
		policy["loss_scale"] = crisis.loss_scale;
		policy["loss_ema_days"] = crisis.loss_ema_days;
		policy["confidence_smoothing"] = crisis.smoothing;
		policy["run_threshold"] = crisis.run_threshold;
		policy["run_share"] = crisis.run_share;
		policy["flight_to_quality"] = crisis.flight_to_quality;
		policy["lolr_spread"] = crisis.lolr_spread;
		policy["lolr_haircut"] = crisis.lolr_haircut;
		policy["lolr_max_assets"] = crisis.lolr_max_assets;
		policy["insured_share"] = crisis.insured_share;
		policy["premium_daily"] = crisis.premium_daily;
		policy["fund_target_ratio"] = crisis.fund_target_ratio;
		policy["capital_flight_share"] = crisis.capital_flight_share;
		policy["insolvency_tolerance"] = crisis.insolvency_tolerance;
		policy["insolvency_tolerance_ratio"] =
			crisis.insolvency_tolerance_ratio;
		policy["treasury_debt_rescue"] = crisis.treasury_debt_rescue;
		policy["takeover_public"] = crisis.takeover_public;
		policy["crisis_source"] =
			"default de jogo (data/definitions/economy/bank_crisis.json)";
		policy["rescue_order"] =
			"shareholders -> uninsured deposits -> insurance fund -> "
			"treasury/debt -> public bank takeover";
		out["policy"] = policy;
	}
	const ogs::bank::Totals totals = ogs::bank::totals(ws_);
	const double group_debt = ogs::bank::total_group_debt(ws_);
	const double group_arrears = ogs::bank::total_group_arrears(ws_);
	{
		Dictionary row;
		row["loans"] = totals.loans;
		row["arrears"] = totals.arrears;
		row["bc_borrowing"] = totals.bc_borrowing;
		row["cash"] = totals.cash;
		row["capital"] = totals.capital;
		row["equity"] = totals.equity;
		row["writeoffs"] = totals.writeoffs;
		row["interest_income"] = totals.interest_income;
		row["interest_expense"] = totals.interest_expense;
		row["defaults"] = totals.defaults;
		row["loans_count"] = totals.loans_count;
		row["group_debt"] = group_debt;
		row["group_arrears"] = group_arrears;
		// C4 (D-110): dinheiro bancÃ¡rio e distribuiÃ§Ã£o ao dono.
		row["deposits"] = totals.deposits;
		row["retained"] = totals.retained;
		row["dividends"] = totals.dividends;
		row["remittance"] = totals.remittance;
		row["remittance_origin"] = totals.remittance_origin;
		row["harvest_interest"] = totals.harvest_interest;
		row["banks"] = totals.banks;
		// C4 (D-110): M1 global = dinheiro do mundo âˆ’ cofre dos bancos (o
		// cofre Ã© reserva, que Ã© M0 e nÃ£o M1); M0 = dinheiro âˆ’ depÃ³sitos.
		double vault = 0.0;
		for (const auto& bank_row : ws_.banks) {
			vault += std::max(0.0, bank_row.second.cash);
		}
		row["m1"] = std::max(0.0, money_total() - vault);
		row["m0"] = sim_core_.m0_total();
		// C6 (D-114): totais da crise (fundos, corridas, redesconto, custos).
		const ogs::bank_crisis::Totals crisis =
			ogs::bank_crisis::totals(ws_);
		row["confidence_sum"] = crisis.confidence_sum;
		row["confidence_min"] = crisis.confidence_min;
		row["confidence_max"] = crisis.confidence_max;
		row["fund"] = crisis.fund_total;
		row["premiums"] = crisis.premiums;
		row["payouts"] = crisis.payouts;
		row["runs"] = crisis.runs;
		row["run_days"] = crisis.run_days;
		row["resolutions"] = crisis.resolutions;
		row["run_withdrawn"] = crisis.run_withdrawn;
		row["run_cash"] = crisis.run_cash;
		row["run_public"] = crisis.run_public;
		row["capital_flight"] = crisis.capital_flight;
		row["lolr_borrowed"] = crisis.lolr_borrowed;
		row["lolr_interest"] = crisis.lolr_interest;
		row["insured_payout"] = crisis.insured_payout;
		row["bailin_cut"] = crisis.bailin_cut;
		row["treasury_rescue"] = crisis.treasury_rescue;
		row["crisis_events"] = crisis.events_total;
		out["totals"] = row;
	}
	// ParticipaÃ§Ã£o efetiva nos emprÃ©stimos da naÃ§Ã£o (0 sem crÃ©dito) e lista
	// por BANCO (C4/D-110). O agregado por naÃ§Ã£o vem logo abaixo.
	std::map<std::string, double> nation_assets;
	for (const auto& row : ws_.banks) {
		nation_assets[row.second.nation] += row.second.loans
			+ row.second.arrears;
	}
	Array banks;
	// Uma linha por banco (o mapa Ã© ordenado por naÃ§Ã£o: ordem determinÃ­stica).
	Array nations;
	for (const auto& row : ws_.banks) {
		const ogs::Bank& bank = row.second;
		Dictionary entry;
		entry["id"] = String(bank.id.c_str());
		entry["nation"] = String(bank.nation.c_str());
		entry["role"] = String(ogs::bank::role_key(bank.role));
		// Nome sÃ³ quando o dado o declara: chave ausente Ã© a ausÃªncia honesta
		// (`name_status = no_data`), nunca um nome inventado.
		if (!bank.name.empty()) {
			entry["name"] = String::utf8(bank.name.c_str());
		}
		entry["name_status"] = String(bank.name_status.c_str());
		entry["owner_kind"] = String(bank.owner_kind.c_str());
		entry["owner_nation"] = String(bank.owner_nation.c_str());
		entry["share"] = bank.share;
		const double nation_total = nation_assets[bank.nation];
		entry["share_loans"] = nation_total > 1e-9
			? (bank.loans + bank.arrears) / nation_total : 0.0;
		entry["currency"] = String(bank.currency.c_str());
		entry["policy_rate"] = ogs::monetary::rate_for_nation(ws_,
			bank.nation);
		entry["loan_rate"] = ogs::bank::loan_rate(ws_, bank.nation);
		entry["loans"] = bank.loans;
		entry["arrears"] = bank.arrears;
		entry["bc_borrowing"] = bank.bc_borrowing;
		entry["cash"] = bank.cash;
		entry["capital"] = bank.capital;
		entry["equity"] = ogs::bank::equity(bank);
		// RazÃ£o de capital: patrimÃ´nio Ã· emprÃ©stimo em aberto (negativa =
		// insolvente; a resoluÃ§Ã£o Ã© C6).
		entry["equity_ratio"] = (bank.loans + bank.arrears) > 1e-9
			? ogs::bank::equity(bank) / (bank.loans + bank.arrears) : 0.0;
		entry["writeoffs"] = bank.writeoffs;
		entry["defaults"] = bank.defaults;
		entry["loans_count"] = bank.loans_count;
		entry["last_default_day"] = bank.last_default_day;
		entry["deposits"] = bank.deposits;
		entry["retained"] = ogs::bank::retained(bank);
		entry["dividends"] = bank.dividends;
		entry["remittance"] = bank.remittance;
		entry["remittance_origin"] = bank.remittance_origin;
		entry["harvest_interest"] = bank.harvest_interest;
		entry["last_dividend_day"] = bank.last_dividend_day;
		// C6 (D-114): confianÃ§a, indicadores medidos, corrida, redesconto,
		// seguro e resoluÃ§Ã£o. `credit_factor` Ã© o aperto aplicado ao crÃ©dito
		// novo (1 = sem aperto; 0 = crÃ©dito cortado).
		const ogs::bank_crisis::Indicators crisis_ind =
			ogs::bank_crisis::indicators(bank);
		entry["confidence"] = bank.confidence;
		entry["capital_ratio"] = crisis_ind.capital_ratio;
		entry["liquidity_ratio"] = crisis_ind.liquidity_ratio;
		entry["npl_ratio"] = crisis_ind.npl_ratio;
		entry["credit_factor"] =
			ogs::bank_crisis::credit_factor(ws_, bank);
		entry["run_days"] = bank.run_days;
		entry["last_run_day"] = bank.last_run_day;
		entry["run_withdrawn"] = bank.run_withdrawn;
		entry["run_cash"] = bank.run_cash;
		entry["run_public"] = bank.run_public;
		entry["capital_flight"] = bank.capital_flight;
		entry["lolr_borrowed"] = bank.lolr_borrowed;
		entry["lolr_interest"] = bank.lolr_interest;
		entry["insured_payout"] = bank.insured_payout;
		entry["bailin_cut"] = bank.bailin_cut;
		entry["treasury_rescue"] = bank.treasury_rescue;
		entry["resolved"] = bank.resolved;
		entry["resolution"] = String(bank.resolution.c_str());
		entry["resolution_day"] = bank.resolution_day;
		entry["absorbed_into"] = String(bank.absorbed_into.c_str());
		banks.append(entry);
	}
	// Agregado por naÃ§Ã£o (soma dos papÃ©is) â€” contrato da C3 preservado.
	std::vector<std::string> nation_order;
	std::map<std::string, Dictionary> by_nation;
	for (const auto& row : ws_.banks) {
		const ogs::Bank& bank = row.second;
		auto found = by_nation.find(bank.nation);
		if (found == by_nation.end()) {
			Dictionary entry;
			entry["nation"] = String(bank.nation.c_str());
			entry["currency"] = String(bank.currency.c_str());
			entry["policy_rate"] = ogs::monetary::rate_for_nation(ws_,
				bank.nation);
			entry["loan_rate"] = ogs::bank::loan_rate(ws_, bank.nation);
			entry["loans"] = 0.0;
			entry["arrears"] = 0.0;
			entry["deposits"] = 0.0;
			entry["bc_borrowing"] = 0.0;
			entry["cash"] = 0.0;
			entry["capital"] = 0.0;
			entry["equity"] = 0.0;
			entry["writeoffs"] = 0.0;
			entry["defaults"] = 0;
			entry["loans_count"] = 0;
			entry["last_default_day"] = -1;
			entry["banks"] = 0;
			found = by_nation.emplace(bank.nation, entry).first;
			nation_order.push_back(bank.nation);
		}
		Dictionary entry = found->second;
		entry["loans"] = double(entry["loans"]) + bank.loans;
		entry["arrears"] = double(entry["arrears"]) + bank.arrears;
		entry["deposits"] = double(entry["deposits"]) + bank.deposits;
		entry["bc_borrowing"] = double(entry["bc_borrowing"])
			+ bank.bc_borrowing;
		entry["cash"] = double(entry["cash"]) + bank.cash;
		entry["capital"] = double(entry["capital"]) + bank.capital;
		entry["equity"] = double(entry["equity"])
			+ ogs::bank::equity(bank);
		entry["writeoffs"] = double(entry["writeoffs"]) + bank.writeoffs;
		entry["defaults"] = int(entry["defaults"]) + bank.defaults;
		entry["loans_count"] = int(entry["loans_count"]) + bank.loans_count;
		entry["last_default_day"] = std::max(int(entry["last_default_day"]),
			bank.last_default_day);
		entry["banks"] = int(entry["banks"]) + 1;
		found->second = entry;
	}
	for (const std::string& code : nation_order) {
		Dictionary entry = by_nation[code];
		const double assets = double(entry["loans"])
			+ double(entry["arrears"]);
		entry["equity_ratio"] = assets > 1e-9
			? double(entry["equity"]) / assets : 0.0;
		nations.append(entry);
	}
	out["nations"] = nations;
	out["banks"] = banks;
	// SÃ³ os grupos com posiÃ§Ã£o de crÃ©dito (a lista inteira Ã© o
	// `get_balance_status().world.groups`).
	Array groups;
	for (const ogs::Group& group : ws_.groups) {
		if (group.debt <= 0.0 && group.credit_arrears <= 0.0
				&& group.credit_defaults == 0) {
			continue;
		}
		Dictionary row;
		row["id"] = String(group.id.c_str());
		row["name"] = String(group.name.c_str());
		row["origin_nation"] = String(group.origin_nation.c_str());
		row["sector"] = String(group.sector.c_str());
		row["cash"] = group.cash;
		row["debt"] = group.debt;
		row["arrears"] = group.credit_arrears;
		row["credit_total"] = group.credit_total;
		row["credit_repaid"] = group.credit_repaid;
		row["default_days"] = group.credit_default_days;
		row["defaults"] = group.credit_defaults;
		groups.append(row);
	}
	out["groups"] = groups;
	{
		const ogs::balance::Ledger book = ogs::balance::ledger(ws_,
			money_total());
		Dictionary invariant;
		const double loans_gap = book.bank_loans - book.group_debt;
		const double arrears_gap = book.bank_arrears - book.group_arrears;
		invariant["loans_vs_debt_residual"] = loans_gap;
		invariant["arrears_vs_debt_residual"] = arrears_gap;
		invariant["financial_sum"] = book.financial_sum();
		invariant["cash_residual"] = book.cash_residual();
		const double tolerance = std::max(1e-3,
			std::abs(book.money_total) * 1e-12);
		invariant["ok"] = std::abs(loans_gap) <= tolerance
			&& std::abs(arrears_gap) <= tolerance
			&& std::abs(book.financial_sum()) <= tolerance
			&& std::abs(book.cash_residual()) <= tolerance;
		out["invariant"] = invariant;
	}
	// C6 (D-114): fundo de seguro por naÃ§Ã£o e o anel de eventos (dia, banco,
	// causa e custo por parte). O total de eventos fica em `totals`.
	{
		Array funds;
		for (const auto& row : ws_.deposit_insurance) {
			Dictionary entry;
			entry["nation"] = String(row.first.c_str());
			entry["fund"] = row.second.fund;
			entry["premiums"] = row.second.premiums;
			entry["payouts"] = row.second.payouts;
			entry["last_payout_day"] = row.second.last_payout_day;
			funds.append(entry);
		}
		out["insurance"] = funds;
		Array events;
		for (const ogs::BankCrisisEvent& event : ws_.bank_crisis_events) {
			Dictionary entry;
			entry["day"] = event.day;
			entry["bank"] = String(event.bank.c_str());
			entry["nation"] = String(event.nation.c_str());
			entry["kind"] = String(event.kind.c_str());
			entry["cause"] = String(event.cause.c_str());
			entry["gap"] = event.gap;
			entry["cost_shareholders"] = event.cost_shareholders;
			entry["cost_depositors"] = event.cost_depositors;
			entry["cost_fund"] = event.cost_fund;
			entry["cost_treasury"] = event.cost_treasury;
			entry["cost_takeover"] = event.cost_takeover;
			entry["capital_flight"] = event.capital_flight;
			entry["lolr"] = event.lolr;
			entry["emission"] = event.emission;
			events.append(entry);
		}
		out["events"] = events;
		Dictionary sources;
		sources["parameters"] = "data/definitions/economy/bank_crisis.json";
		sources["liquidity"] =
			"cash / deposits (C4 banks hold no reserves: default min is 0)";
		sources["insured_ceiling"] =
			"declared share of deposits (no depositor ledger in the model)";
		sources["resolution"] =
			"shareholders -> uninsured -> fund -> treasury/debt -> takeover";
		out["crisis_sources"] = sources;
	}
	return out;
}

Dictionary EconomySim::central_bank_gold_trade(const String& currency,
		double tonnes, bool buy) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const ogs::exchange::GoldTrade trade = ogs::exchange::gold_trade(ws_,
		std::string(currency.utf8().get_data()), tonnes, buy);
	Dictionary out;
	out["ok"] = trade.ok;
	out["tons"] = trade.tons;
	out["value"] = trade.value;
	out["city"] = String(trade.city.c_str());
	out["error"] = String(trade.error.c_str());
	status_invalidate();
	return out;
}

Dictionary EconomySim::central_bank_gold_convert(const String& currency,
		double tonnes) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	std::vector<double> base_list(
		static_cast<std::size_t>(std::max(0, ws_.products)), 0.0);
	for (int product = 0; product < ws_.products; ++product) {
		base_list[static_cast<std::size_t>(product)] =
			sim_core_.base_price(product);
	}
	const ogs::exchange::GoldTrade trade = ogs::exchange::gold_convert(ws_,
		std::string(currency.utf8().get_data()), tonnes, base_list);
	Dictionary out;
	out["ok"] = trade.ok;
	out["tons"] = trade.tons;
	out["value"] = trade.value;
	out["city"] = String(trade.city.c_str());
	out["error"] = String(trade.error.c_str());
	status_invalidate();
	return out;
}

Dictionary EconomySim::get_storage_status() const {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Dictionary out;
	out["day"] = ws_.day;
	const std::size_t products =
		static_cast<std::size_t>(std::max(0, ws_.products));
	const std::vector<std::string>& names = ws_.product_names;

	// Totais por tipo (estrutura e estoque) para a ficha da UI/teste.
	double type_capacity[ogs::kStorageTypeCount] = {};
	double type_gov[ogs::kStorageTypeCount] = {};
	double type_priv[ogs::kStorageTypeCount] = {};
	Dictionary city_rows;
	for (const ogs::StorageSite& site : ws_.storage_sites) {
		if (site.type < 0 || site.type >= ogs::kStorageTypeCount) {
			continue;
		}
		double gov_stock = 0.0;
		double priv_stock = 0.0;
		double inflow = 0.0;
		Dictionary by_product;
		for (std::size_t product = 0; product < products; ++product) {
			const double quantity = product < site.stock.size()
				? std::max(0.0, site.stock[product]) : 0.0;
			if (quantity <= 1e-9) {
				continue;
			}
			if (site.scope == ogs::STORAGE_PRIVATE) {
				priv_stock += quantity;
			} else {
				gov_stock += quantity;
			}
			if (product < site.inflow_stock.size()) {
				inflow += std::max(0.0, site.inflow_stock[product]);
			}
			if (product < names.size()) {
				by_product[String(names[product].c_str())] = quantity;
			}
		}
		type_capacity[site.type] += std::max(0.0, site.capacity_t);
		type_gov[site.type] += gov_stock;
		type_priv[site.type] += priv_stock;
		Dictionary row;
		row["city"] = String(site.city.c_str());
		row["type"] = String(ogs::storage_type_name(site.type)
			? ogs::storage_type_name(site.type) : "");
		row["region"] = ogs::storage_type_is_region(site.type);
		row["structure_only"] =
			ogs::storage_type_is_structure_only(site.type);
		row["owner"] = String(ogs::storage_scope_name(site.scope));
		row["capacity_t"] = site.capacity_t;
		row["stock_t"] = gov_stock + priv_stock;
		row["inflow_t"] = inflow;
		row["by_product"] = by_product;
		row["loss_today_t"] = site.loss_today_t;
		row["maintenance_paid_today"] = site.maintenance_paid_today;
		row["access_paid_today"] = site.access_paid_today;
		row["received_today_t"] = site.received_today_t;
		row["bought_today_t"] = site.bought_today_t;
		row["released_today_t"] = site.released_today_t;
		const std::string key = site.city + "|" + std::to_string(site.type)
			+ "|" + std::to_string(site.scope);
		city_rows[String(key.c_str())] = row;
	}
	out["sites"] = city_rows;

	Array types;
	Dictionary totals;
	double total_capacity = 0.0;
	double total_gov = 0.0;
	double total_priv = 0.0;
	for (int type = 0; type < ogs::kStorageTypeCount; ++type) {
		Dictionary row;
		row["type"] = String(ogs::storage_type_name(type)
			? ogs::storage_type_name(type) : "");
		row["region"] = ogs::storage_type_is_region(type);
		row["structure_only"] = ogs::storage_type_is_structure_only(type);
		row["capacity_t"] = type_capacity[type];
		row["government_stock_t"] = type_gov[type];
		row["private_stock_t"] = type_priv[type];
		types.append(row);
		total_capacity += type_capacity[type];
		total_gov += type_gov[type];
		total_priv += type_priv[type];
	}
	out["types"] = types;
	totals["capacity_t"] = total_capacity;
	totals["government_stock_t"] = total_gov;
	totals["private_stock_t"] = total_priv;
	totals["sites"] = static_cast<int>(ws_.storage_sites.size());
	out["totals"] = totals;

	// ------------------------------------------------------------------
	// Camada NACIONAL (leitura): reserva/estoque por produto, capacidade,
	// dias de consumo cobertos, IPC e preÃ§o mÃ©dio ponderados pela cesta dos
	// quintis. NÃƒO existe mercado nacional â€” sÃ³ a leitura agregada.
	// ------------------------------------------------------------------
	struct NationAcc {
		double capacity = 0.0;
		double gov_stock = 0.0;
		double priv_stock = 0.0;
		double population = 0.0;
		double weight = 0.0;
		double food_need_t_day = 0.0;
		double gov_by_product[kMaxProducts] = {};
		double priv_by_product[kMaxProducts] = {};
		double demand_t_day[kMaxProducts] = {};
		double price_weight[kMaxProducts] = {};
		double price_weighted[kMaxProducts] = {};
	};
	std::map<std::string, NationAcc> nations;

	// Pesos da cesta: fatores calÃ³ricos do dado (needs.json). Sem cesta
	// carregada, todos os alimentos pesam igual (declarado).
	std::vector<double> food_weight(products, 0.0);
	double food_weight_sum = 0.0;
	for (std::size_t slot = 0; slot < ws_.food_products.size(); ++slot) {
		const int product = ws_.food_products[slot];
		if (product < 0 || product >= ws_.products) {
			continue;
		}
		const double factor = slot < ws_.food_factors.size()
			? std::max(0.0, ws_.food_factors[slot]) : 1.0;
		food_weight[static_cast<std::size_t>(product)] = factor;
		food_weight_sum += factor;
	}
	if (food_weight_sum <= 1e-12) {
		for (std::size_t slot = 0; slot < ws_.food_products.size(); ++slot) {
			const int product = ws_.food_products[slot];
			if (product >= 0 && product < ws_.products) {
				food_weight[static_cast<std::size_t>(product)] = 1.0;
				food_weight_sum += 1.0;
			}
		}
	}

	for (const ogs::StorageSite& site : ws_.storage_sites) {
		if (site.city_index < 0
				|| static_cast<std::size_t>(site.city_index) >= ws_.cities.size()) {
			continue;
		}
		const ogs::City& city =
			ws_.cities[static_cast<std::size_t>(site.city_index)];
		NationAcc& nation = nations[city.nation];
		nation.capacity += std::max(0.0, site.capacity_t);
		for (std::size_t product = 0; product < products; ++product) {
			const double quantity = product < site.stock.size()
				? std::max(0.0, site.stock[product]) : 0.0;
			if (site.scope == ogs::STORAGE_PRIVATE) {
				nation.priv_stock += quantity;
				nation.priv_by_product[product] += quantity;
			} else {
				nation.gov_stock += quantity;
				nation.gov_by_product[product] += quantity;
			}
		}
	}

	for (const ogs::City& city : ws_.cities) {
		NationAcc& nation = nations[city.nation];
		// Peso da cidade no IPC: a cesta dos QUINTIS (necessidade de cada
		// estrato); sem estratos, a necessidade da POP; sem ambas, a
		// populaÃ§Ã£o de simulaÃ§Ã£o pela necessidade per capita.
		double city_weight = 0.0;
		for (int stratum = 0; stratum < ogs::kIncomeStrata; ++stratum) {
			city_weight += std::max(0.0,
				city.pop.stratum_need[stratum]);
		}
		if (city_weight <= 1e-12) {
			city_weight = std::max(0.0, city.pop.need);
		}
		if (city_weight <= 1e-12) {
			city_weight = std::max(0.0, city.simulation_population)
				* ogs::kNeedPerCapita;
		}
		nation.population += city.population;
		nation.weight += city_weight;
		nation.food_need_t_day += city_weight;
		// O IPC em si tem UMA dona (`central_bank::cpi_by_nation`, C2a/D-103);
		// aqui sÃ³ acumula a demanda e o preÃ§o mÃ©dio por produto.
		if (food_weight_sum > 0.0) {
			for (std::size_t product = 0; product < products; ++product) {
				const double weight = food_weight[product];
				if (weight <= 0.0) {
					continue;
				}
				const double price = std::max(0.0, city.markets[product].price);
				if (price > 0.0) {
					nation.demand_t_day[product] +=
						std::max(0.0, city.markets[product].demand);
					nation.price_weight[product] += city_weight;
					nation.price_weighted[product] += city_weight * price;
				}
			}
		}
	}

	// IPC por naÃ§Ã£o: a MESMA funÃ§Ã£o que o banco central publica (C2a/D-103).
	std::vector<double> base_list(products, 0.0);
	for (std::size_t product = 0; product < products; ++product) {
		base_list[product] = sim_core_.base_price(static_cast<int>(product));
	}
	const std::map<std::string, ogs::central_bank::CpiRow> cpi_rows =
		ogs::central_bank::cpi_by_nation(ws_, base_list);

	Dictionary nation_rows;
	for (const auto& entry : nations) {
		const std::string& tag = entry.first;
		const NationAcc& nation = entry.second;
		Dictionary row;
		row["nation"] = String(tag.c_str());
		row["population"] = nation.population;
		row["capacity_t"] = nation.capacity;
		row["government_stock_t"] = nation.gov_stock;
		row["private_stock_t"] = nation.priv_stock;
		row["food_need_t_day"] = nation.food_need_t_day;
		Dictionary gov_by_product;
		Dictionary priv_by_product;
		Dictionary days_cover;
		for (std::size_t product = 0; product < products; ++product) {
			if (product < names.size()) {
				const String key(names[product].c_str());
				if (nation.gov_by_product[product] > 1e-9) {
					gov_by_product[key] = nation.gov_by_product[product];
				}
				if (nation.priv_by_product[product] > 1e-9) {
					priv_by_product[key] = nation.priv_by_product[product];
				}
				const double demand = nation.demand_t_day[product];
				if (demand > 1e-9) {
					days_cover[key] = nation.gov_by_product[product] / demand;
				}
			}
		}
		row["government_by_product"] = gov_by_product;
		row["private_by_product"] = priv_by_product;
		row["government_days_cover"] = days_cover;
		{
			const auto found = cpi_rows.find(tag);
			row["ipc"] = found != cpi_rows.end() && found->second.ok
				? found->second.index : 0.0;
		}
		Dictionary avg_price;
		for (std::size_t product = 0; product < products; ++product) {
			if (product < names.size()
					&& nation.price_weight[product] > 1e-12) {
				avg_price[String(names[product].c_str())] =
					nation.price_weighted[product]
					/ nation.price_weight[product];
			}
		}
		row["avg_price"] = avg_price;
		nation_rows[String(tag.c_str())] = row;
	}
	out["nations"] = nation_rows;
	return out;
}

Dictionary EconomySim::get_storage_product_types() const {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Dictionary out;
	for (std::size_t product = 0; product < ws_.storage_product_type.size();
			++product) {
		if (product >= ws_.product_names.size()) {
			continue;
		}
		const int type = ws_.storage_product_type[product];
		out[String(ws_.product_names[product].c_str())] =
			String(ogs::storage_type_name(type)
				? ogs::storage_type_name(type) : "");
	}
	return out;
}

void EconomySim::set_external_demand(const String& city_id,
		const String& product, double ceiling_t_per_day) {
	ogs::Command cmd;
	cmd.type = ogs::CommandType::SetExternalDemand;
	cmd.s[0] = to_std(city_id);
	cmd.s[1] = to_std(product);
	cmd.v[0] = ceiling_t_per_day;
	dispatch_command(cmd);
}

void EconomySim::set_external_income_index(const String& city_id, double index) {
	ogs::Command cmd;
	cmd.type = ogs::CommandType::SetExternalIncomeIndex;
	cmd.s[0] = to_std(city_id);
	cmd.v[0] = index;
	dispatch_command(cmd);
}

void EconomySim::set_productivity_index(const String& city_id, double index) {
	ogs::Command cmd;
	cmd.type = ogs::CommandType::SetProductivityIndex;
	cmd.s[0] = to_std(city_id);
	cmd.v[0] = index;
	dispatch_command(cmd);
}

void EconomySim::set_income_gini(const String& city_id, double gini) {
	ogs::Command cmd;
	cmd.type = ogs::CommandType::SetIncomeGini;
	cmd.s[0] = to_std(city_id);
	cmd.v[0] = gini;
	dispatch_command(cmd);
}

void EconomySim::set_minimum_wage(const String& nation, double value) {
	ogs::Command cmd;
	cmd.type = ogs::CommandType::SetMinimumWage;
	cmd.s[0] = to_std(nation);
	cmd.v[0] = value;
	dispatch_command(cmd);
}

void EconomySim::set_wage_rigidity(double value) {
	ogs::Command cmd;
	cmd.type = ogs::CommandType::SetWageRigidity;
	cmd.v[0] = value;
	dispatch_command(cmd);
}

void EconomySim::set_tariff(const String& buyer_nation,
		const String& seller_nation, const String& product, double rate) {
	ogs::Command cmd;
	cmd.type = ogs::CommandType::SetTariff;
	cmd.s[0] = to_std(buyer_nation);
	cmd.s[1] = to_std(seller_nation);
	cmd.s[2] = to_std(product);
	cmd.v[0] = rate;
	dispatch_command(cmd);
}

void EconomySim::set_currency_rate(const String& code, double rate,
		bool floating, double base_rate) {
	ogs::Command cmd;
	cmd.type = ogs::CommandType::SetCurrencyRate;
	cmd.s[0] = to_std(code);
	cmd.v[0] = rate;
	cmd.v[1] = base_rate;
	cmd.b = floating;
	dispatch_command(cmd);
}

Dictionary EconomySim::expand_port(const String& port_id, double t_per_day) {
	ogs::Command cmd;
	cmd.type = ogs::CommandType::ExpandPort;
	cmd.s[0] = to_std(port_id);
	cmd.v[0] = t_per_day;
	return dispatch_command(cmd);
}

void EconomySim::set_port_capacity(const String& port_id, double t_per_day) {
	ogs::Command cmd;
	cmd.type = ogs::CommandType::SetPortCapacity;
	cmd.s[0] = to_std(port_id);
	cmd.v[0] = t_per_day;
	dispatch_command(cmd);
}

void EconomySim::set_market_stock(const String& city_id,
		const String& product, double stock) {
	ogs::Command cmd;
	cmd.type = ogs::CommandType::SetMarketStock;
	cmd.s[0] = to_std(city_id);
	cmd.s[1] = to_std(product);
	cmd.v[0] = stock;
	dispatch_command(cmd);
}

void EconomySim::set_market_price(const String& city_id,
		const String& product, double price) {
	ogs::Command cmd;
	cmd.type = ogs::CommandType::SetMarketPrice;
	cmd.s[0] = to_std(city_id);
	cmd.s[1] = to_std(product);
	cmd.v[0] = price;
	dispatch_command(cmd);
}

// ---------------------------------------------------------------------------
// D-125 — API da eficiência de produção. Os comandos entram na fila A3 como
// qualquer mutação; os parâmetros e o "mundo maduro" são setup (aplicam na
// hora, como set_balance/set_goods).
// ---------------------------------------------------------------------------
Dictionary EconomySim::change_product(const String& city_id,
		const String& building_id, const String& product,
		const String& recipe_id) {
	ogs::Command cmd;
	cmd.type = ogs::CommandType::ChangeProduct;
	cmd.s[0] = to_std(city_id);
	cmd.s[1] = to_std(building_id);
	cmd.s[2] = to_std(product);
	cmd.s[3] = to_std(recipe_id);
	return dispatch_command(cmd);
}

Dictionary EconomySim::set_refinery_mode(const String& city_id,
		const String& building_id, const String& mode) {
	ogs::Command cmd;
	cmd.type = ogs::CommandType::SetRefineryMode;
	cmd.s[0] = to_std(city_id);
	cmd.s[1] = to_std(building_id);
	cmd.s[2] = to_std(mode);
	return dispatch_command(cmd);
}

Dictionary EconomySim::set_building_equipment(const String& city_id,
		const String& building_id, double level) {
	ogs::Command cmd;
	cmd.type = ogs::CommandType::SetBuildingEquipment;
	cmd.s[0] = to_std(city_id);
	cmd.s[1] = to_std(building_id);
	cmd.v[0] = level;
	return dispatch_command(cmd);
}

void EconomySim::set_efficiency_params(const Dictionary& values) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	EfficiencyParams& p = ws_.efficiency;
	auto number = [](const Dictionary& block, const char* key,
			double fallback) -> double {
		return block.has(key) ? double(block.get(key, fallback)) : fallback;
	};
	auto read_curve = [&number](const Dictionary& block, const String& key,
			EfficiencyCurve& curve) {
		if (!block.has(key)) {
			return;
		}
		const Dictionary row = block.get(key, Dictionary());
		if (row.is_empty()) {
			return;
		}
		curve.start = std::clamp(number(row, "start", curve.start), 0.0, 1.0);
		curve.mid = std::clamp(number(row, "mid", curve.mid),
			curve.start, 1.0);
		curve.mid_days = std::max(1.0, number(row, "mid_days",
			curve.mid_days));
		curve.full_days = std::max(curve.mid_days, number(row, "full_days",
			curve.full_days));
	};
	read_curve(values, "factory", p.factory);
	read_curve(values, "refinery", p.refinery);
	read_curve(values, "mine", p.mine);
	read_curve(values, "farm", p.farm);
	read_curve(values, "ranch", p.ranch);
	if (values.has("equipment")) {
		const Dictionary block = values.get("equipment", Dictionary());
		p.equipment_cap_without = std::clamp(number(block,
			"cap_without", p.equipment_cap_without), 0.0, 1.0);
		p.equipment_rate_boost = std::max(1.0, number(block,
			"rate_boost", p.equipment_rate_boost));
		p.equipment_fuel_per_capacity_day = std::max(0.0, number(block,
			"fuel_per_capacity_day", p.equipment_fuel_per_capacity_day));
		if (block.has("fuel_product")) {
			const int fuel = product_index(String(block.get("fuel_product",
				String())));
			if (fuel >= 0) {
				p.equipment_fuel_product = fuel;
			}
		}
	}
	if (values.has("transitions")) {
		const Dictionary block = values.get("transitions", Dictionary());
		p.transition_factory_days = std::max(0, int(number(block,
			"factory_days", p.transition_factory_days)));
		p.transition_refinery_days = std::max(0, int(number(block,
			"refinery_days", p.transition_refinery_days)));
		p.transition_military_days = std::max(0, int(number(block,
			"military_days", p.transition_military_days)));
		p.transition_semiconductor_days = std::max(0, int(number(block,
			"semiconductor_days", p.transition_semiconductor_days)));
		p.transition_mode_days = std::max(0, int(number(block,
			"mode_days", p.transition_mode_days)));
	}
	if (values.has("retention")) {
		const Dictionary block = values.get("retention", Dictionary());
		p.retention_same_family = std::clamp(number(block, "same_family",
			p.retention_same_family), 0.0, 1.0);
		p.retention_other = std::clamp(number(block, "other",
			p.retention_other), 0.0, 1.0);
	}
	// Modos da refinaria: o JSON nomeia os produtos (`gasoline`, `diesel`,
	// `jet_fuel`); a resolução para índice usa o catálogo carregado.
	if (values.has("refinery_modes")) {
		const Dictionary block = values.get("refinery_modes", Dictionary());
		const String order[3] = {String("standard"), String("diesel"),
			String("jet")};
		const String goods[3] = {String("gasoline"), String("diesel"),
			String("jet_fuel")};
		for (int mode = 0; mode < 3; ++mode) {
			if (!block.has(order[mode])) {
				continue;
			}
			const Dictionary row = block.get(order[mode], Dictionary());
			for (int slot = 0; slot < 3; ++slot) {
				if (row.has(goods[slot])) {
					p.mode_shares[mode][slot] = std::clamp(double(
						row.get(goods[slot], p.mode_shares[mode][slot])),
						0.0, 1.0);
				}
			}
		}
	}
	if (values.has("ai")) {
		const Dictionary block = values.get("ai", Dictionary());
		p.ai_mode_cooldown_days = std::max(0, int(number(block,
			"mode_cooldown_days", p.ai_mode_cooldown_days)));
		p.ai_mode_payback_days = std::max(1, int(number(block,
			"mode_payback_days", p.ai_mode_payback_days)));
		p.ai_mode_min_gain = std::clamp(number(block, "mode_min_gain",
			p.ai_mode_min_gain), 0.0, 1.0);
	}
	status_invalidate();
}

// D-125: leitura dos parâmetros aplicados (JSON ou espelho compilado). Mesmas
// chaves numéricas do bloco `definitions.efficiency` do save; a IA em GDScript
// usa para estimar a rampa sem duplicar a tabela de balanceamento.
Dictionary EconomySim::get_efficiency_params() const {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const EfficiencyParams& p = ws_.efficiency;
	Dictionary out;
	auto curve_dict = [](const EfficiencyCurve& curve) {
		Dictionary row;
		row["start"] = curve.start;
		row["mid"] = curve.mid;
		row["mid_days"] = curve.mid_days;
		row["full_days"] = curve.full_days;
		return row;
	};
	out["factory"] = curve_dict(p.factory);
	out["refinery"] = curve_dict(p.refinery);
	out["mine"] = curve_dict(p.mine);
	out["farm"] = curve_dict(p.farm);
	out["ranch"] = curve_dict(p.ranch);
	out["equipment_cap_without"] = p.equipment_cap_without;
	out["equipment_rate_boost"] = p.equipment_rate_boost;
	out["equipment_fuel_per_capacity_day"] =
		p.equipment_fuel_per_capacity_day;
	out["transition_factory_days"] = p.transition_factory_days;
	out["transition_refinery_days"] = p.transition_refinery_days;
	out["transition_military_days"] = p.transition_military_days;
	out["transition_semiconductor_days"] = p.transition_semiconductor_days;
	out["transition_mode_days"] = p.transition_mode_days;
	out["retention_same_family"] = p.retention_same_family;
	out["retention_other"] = p.retention_other;
	// D-125: modos da refinaria na ordem de `refinery_modes.order`
	// (standard, diesel, jet) × (gasoline, diesel, jet_fuel); a MESMA matriz
	// do bloco `definitions.efficiency.mode_shares` do save.
	Array modes;
	for (int mode = 0; mode < 3; ++mode) {
		Array row;
		row.append(p.mode_shares[mode][0]);
		row.append(p.mode_shares[mode][1]);
		row.append(p.mode_shares[mode][2]);
		modes.append(row);
	}
	out["mode_shares"] = modes;
	out["ai_mode_cooldown_days"] = p.ai_mode_cooldown_days;
	out["ai_mode_payback_days"] = p.ai_mode_payback_days;
	out["ai_mode_min_gain"] = p.ai_mode_min_gain;
	return out;
}

void EconomySim::set_crisis_params(const Dictionary& values) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	std::map<std::string, double> params;
	for (const Variant& key : values.keys()) {
		const Variant value = values.get(key, Variant());
		if (value.get_type() == Variant::INT
				|| value.get_type() == Variant::FLOAT
				|| value.get_type() == Variant::BOOL) {
			params[std::string(String(key).utf8().get_data())] =
				double(value);
		}
	}
	crisis::apply_params(ws_, params);
	status_invalidate();
}

Dictionary EconomySim::get_crisis_params() const {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Dictionary out;
	for (const auto& row : crisis::params_map(ws_)) {
		out[String(row.first.c_str())] = row.second;
	}
	return out;
}

void EconomySim::set_ai_brain_params(const Dictionary& values) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	std::map<std::string, double> params;
	// O JSON é seccionado (`forecast`, `price`, ...); achata uma seção por vez
	// em chaves pontuadas ("forecast.month_days"). Chaves já planas passam.
	std::function<void(const Dictionary&, const std::string&)> flatten =
		[&flatten, &params](const Dictionary& source,
			const std::string& prefix) {
			for (const Variant& key : source.keys()) {
				const Variant value = source.get(key, Variant());
				const std::string name = prefix
					+ std::string(String(key).utf8().get_data());
				if (value.get_type() == Variant::DICTIONARY) {
					flatten(Dictionary(value), name + ".");
					continue;
				}
				if (value.get_type() == Variant::INT
						|| value.get_type() == Variant::FLOAT
						|| value.get_type() == Variant::BOOL) {
					params[name] = double(value);
					continue;
				}
				// D-131 fase B: os escopos chegam como nome do JSON
				// (`region`/`world`) e viram o enum do struct (0/1).
				if (value.get_type() == Variant::STRING) {
					const std::string text =
						std::string(String(value).utf8().get_data());
					if (name == "competition.scope_food") {
						params[name] = text == "world" ? 1.0 : 0.0;
					} else if (name == "competition.scope_other") {
						params[name] = text == "region" ? 0.0 : 1.0;
					}
				}
			}
		};
	flatten(values, std::string());
	ai_brain::apply_params(ws_, params);
	status_invalidate();
}

Dictionary EconomySim::get_ai_brain_params() const {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Dictionary out;
	for (const auto& row : ai_brain::params_map(ws_)) {
		out[String(row.first.c_str())] = row.second;
	}
	return out;
}

Dictionary EconomySim::get_ai_brain_status() const {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Dictionary out;
	out["params"] = get_ai_brain_params();
	const AiState& state = ai_phase_.state();
	out["attempts"] = state.brain_attempts;
	out["rejected"] = state.brain_rejected;
	out["reject_share"] = state.brain_attempts > 0
		? double(state.brain_rejected) / double(state.brain_attempts) : 0.0;
	// Fase B: candidatos únicos por (cidade, produto) por mês e o maior
	// número de plantas do mesmo bem em qualquer janela de 30 d observada.
	out["unique_candidates_month"] = state.brain_unique_month_candidates;
	out["unique_rejected_month"] = state.brain_unique_rejected_month;
	out["risk_rejected"] = state.brain_risk_rejected;
	// Fase C: distribuição por gate e contadores do grupo (diagnóstico).
	out["exposure_rejected"] = state.brain_exposure_rejected;
	out["herd_rejected"] = state.brain_herd_rejected;
	out["loss_blocked"] = state.brain_loss_blocked;
	out["competition_skipped"] = state.brain_competition_skipped;
	out["group_attempts"] = state.brain_group_attempts;
	out["group_rejected"] = state.brain_group_rejected;
	// D-131 fase C (diagnóstico): reprovações por gate e amostras (privado e
	// grupo). Só leitura: nada disto entra no save nem no mundo.
	const auto gate_counts = [](const std::array<int, 7>& gates) {
		Dictionary out;
		out["a"] = gates[1];
		out["b"] = gates[2];
		out["c"] = gates[3];
		out["d"] = gates[4];
		out["e"] = gates[5];
		out["cap_max"] = gates[6];
		out["unknown"] = gates[0];
		return out;
	};
	const auto sample_list = [](const std::vector<AiForecastSample>& rows) {
		Array out;
		for (const AiForecastSample& row : rows) {
			Dictionary sample;
			sample["day"] = row.day;
			sample["city"] = row.city;
			sample["product"] = row.product;
			sample["gate"] = row.gate;
			sample["cash"] = row.cash;
			sample["credit"] = row.credit;
			sample["capex"] = row.capex;
			sample["revenue"] = row.revenue;
			sample["cost"] = row.cost;
			sample["payback"] = row.payback;
			sample["trough"] = row.trough;
			sample["min_balance"] = row.min_balance;
			sample["final_balance"] = row.final_balance;
			sample["cap_max"] = row.cap_max;
			sample["capacity"] = row.capacity;
			sample["price"] = row.price;
			sample["margin"] = row.margin;
			out.append(sample);
		}
		return out;
	};
	out["gate_rejected"] = gate_counts(state.brain_gate_rejected);
	out["group_gate_rejected"] = gate_counts(state.brain_group_gate_rejected);
	out["samples"] = sample_list(state.brain_samples);
	out["group_samples"] = sample_list(state.brain_group_samples);
	out["zero_revenue"] = state.brain_zero_revenue;
	out["group_zero_revenue"] = state.brain_group_zero_revenue;
	out["group_executed"] = state.brain_group_executed;
	out["group_created"] = state.brain_group_created;
	std::map<int, int> herd_plants;
	int herd_max = 0;
	for (const AiHerdEvent& event : state.herd_events) {
		const int count = ++herd_plants[event.product];
		herd_max = std::max(herd_max, count);
	}
	out["herd_max_plants_30d"] = herd_max;
	return out;
}

Dictionary EconomySim::get_crisis_status() const {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Dictionary out;
	out["params"] = get_crisis_params();
	out["layoffs"] = ws_.crisis_layoffs;
	out["jobs_cut"] = ws_.crisis_jobs_cut;
	out["severance_paid"] = ws_.crisis_severance_paid;
	out["loans"] = ws_.crisis_loans;
	out["renegotiations"] = ws_.crisis_renegotiations;
	out["penalty_paid"] = ws_.crisis_penalty_paid;
	out["liquidations"] = ws_.crisis_liquidations;
	out["liquidated_t"] = ws_.crisis_liquidated_t;
	out["liquidated_value"] = ws_.crisis_liquidated_value;
	out["mothballs"] = ws_.crisis_mothballs;
	out["reopens"] = ws_.crisis_reopens;
	out["reopen_paid"] = ws_.crisis_reopen_paid;
	out["auctions"] = ws_.crisis_auctions;
	out["auction_value"] = ws_.crisis_auction_value;
	out["auctions_unsold"] = ws_.crisis_auctions_unsold;
	out["farm_area_reductions"] = ws_.crisis_farm_area_reductions;
	out["power_auctions"] = ws_.crisis_power_auctions;
	out["power_auction_value"] = ws_.crisis_power_auction_value;
	Array buildings;
	for (const Building& building : ws_.buildings) {
		Dictionary row;
		row["city"] = String(building.city.c_str());
		row["id"] = String(building.id.c_str());
		row["owner"] = String(building.owner.c_str());
		row["output_product"] = building.output_product >= 0
			? String(ws_.product_names[static_cast<std::size_t>(
				building.output_product)].c_str()) : String();
		row["capacity"] = building.capacity;
		row["jobs"] = building.jobs;
		row["workers"] = building.workers;
		row["loss_days"] = building.loss_days;
		row["crisis_stage"] = building.crisis_stage;
		row["mothballed"] = building.mothballed;
		row["mothball_unpaid_days"] = building.mothball_unpaid_days;
		row["production_efficiency"] = building.production_efficiency;
		row["bankrupt"] = building.bankrupt;
		row["farm_area_reduced"] = building.farm_area_reduced;
		buildings.append(row);
	}
	out["buildings"] = buildings;
	Array plants;
	for (const PowerPlant& plant : ws_.power_plants) {
		Dictionary row;
		row["id"] = String(plant.id.c_str());
		row["city"] = plant.city_index >= 0
			&& plant.city_index < static_cast<int>(ws_.cities.size())
			? String(ws_.cities[static_cast<std::size_t>(
				plant.city_index)].id.c_str()) : String();
		row["owner_group"] = String(plant.owner_group.c_str());
		row["mw"] = plant.mw;
		row["jobs"] = plant.jobs;
		row["loss_days"] = plant.loss_days;
		row["crisis_stage"] = plant.crisis_stage;
		row["mothballed"] = plant.mothballed;
		row["mothball_unpaid_days"] = plant.mothball_unpaid_days;
		row["production_efficiency"] = plant.production_efficiency;
		row["bankrupt"] = plant.bankrupt;
		plants.append(row);
	}
	out["power_plants"] = plants;
	Array cities;
	for (const City& city : ws_.cities) {
		Dictionary row;
		row["city"] = String(city.id.c_str());
		row["firm_cash"] = city.firm_cash;
		row["service_cash"] = city.service_cash;
		row["firm_debt"] = city.firm_debt;
		row["firm_credit_arrears"] = city.firm_credit_arrears;
		row["firm_credit_rate_add"] = city.firm_credit_rate_add;
		row["firm_credit_renegotiations"] = city.firm_credit_renegotiations;
		row["firm_credit_defaults"] = city.firm_credit_defaults;
		cities.append(row);
	}
	out["cities"] = cities;
	return out;
}

Dictionary EconomySim::crisis_layoff(const String& city_id,
		const String& building_id, double share) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Dictionary out;
	std::string error;
	const bool ok = crisis::layoff(ws_, sim_core_,
		std::string(city_id.utf8().get_data()),
		std::string(building_id.utf8().get_data()), share, &error);
	out["ok"] = ok;
	if (!ok) {
		out["error"] = String(error.c_str());
	}
	const Building* building = ogs::find_building(ws_,
		std::string(city_id.utf8().get_data()),
		std::string(building_id.utf8().get_data()));
	if (building != nullptr) {
		out["jobs"] = building->jobs;
		out["severance_paid"] = ws_.crisis_severance_paid;
	}
	status_invalidate();
	return out;
}

Dictionary EconomySim::crisis_borrow(const String& city_id, double amount) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Dictionary out;
	std::string error;
	const std::string city = std::string(city_id.utf8().get_data());
	double before = 0.0;
	for (const City& row : ws_.cities) {
		if (row.id == city) {
			before = row.firm_debt;
			break;
		}
	}
	const bool ok = crisis::borrow_working_capital(ws_, sim_core_, city, amount,
		&error);
	out["ok"] = ok;
	if (!ok) {
		out["error"] = String(error.c_str());
	}
	for (const City& row : ws_.cities) {
		if (row.id == city) {
			out["granted"] = row.firm_debt - before;
			out["debt"] = row.firm_debt;
			break;
		}
	}
	status_invalidate();
	return out;
}

Dictionary EconomySim::crisis_liquidate(const String& city_id,
		const String& building_id, double share) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Dictionary out;
	std::string error;
	const int before_t = ws_.crisis_liquidations;
	const double before_tons = ws_.crisis_liquidated_t;
	const double before_value = ws_.crisis_liquidated_value;
	const bool ok = crisis::liquidate_stock(ws_, sim_core_,
		std::string(city_id.utf8().get_data()),
		std::string(building_id.utf8().get_data()), share, &error);
	out["ok"] = ok;
	if (!ok) {
		out["error"] = String(error.c_str());
	}
	out["events"] = ws_.crisis_liquidations - before_t;
	out["tons"] = ws_.crisis_liquidated_t - before_tons;
	out["value"] = ws_.crisis_liquidated_value - before_value;
	status_invalidate();
	return out;
}

Dictionary EconomySim::crisis_mothball(const String& city_id,
		const String& building_id) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Dictionary out;
	std::string error;
	const bool ok = crisis::mothball(ws_, sim_core_,
		std::string(city_id.utf8().get_data()),
		std::string(building_id.utf8().get_data()), &error);
	out["ok"] = ok;
	if (!ok) {
		out["error"] = String(error.c_str());
	}
	status_invalidate();
	return out;
}

Dictionary EconomySim::crisis_reopen(const String& city_id,
		const String& building_id) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Dictionary out;
	std::string error;
	const bool ok = crisis::reopen(ws_, sim_core_,
		std::string(city_id.utf8().get_data()),
		std::string(building_id.utf8().get_data()), &error);
	out["ok"] = ok;
	if (!ok) {
		out["error"] = String(error.c_str());
	}
	status_invalidate();
	return out;
}

Dictionary EconomySim::crisis_auction(const String& city_id,
		const String& building_id) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Dictionary out;
	std::string error;
	const bool ok = crisis::auction(ws_, sim_core_,
		std::string(city_id.utf8().get_data()),
		std::string(building_id.utf8().get_data()), &error);
	out["ok"] = ok;
	if (!ok) {
		out["error"] = String(error.c_str());
	}
	status_invalidate();
	return out;
}

Dictionary EconomySim::crisis_set_stress(const String& city_id,
		const String& building_id, int loss_days) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Dictionary out;
	Building* building = ogs::find_building(ws_,
		std::string(city_id.utf8().get_data()),
		std::string(building_id.utf8().get_data()));
	if (building == nullptr) {
		out["ok"] = false;
		out["error"] = "construcao desconhecida";
		return out;
	}
	building->loss_days = std::clamp(loss_days, 0, 1000000);
	out["ok"] = true;
	out["loss_days"] = building->loss_days;
	status_invalidate();
	return out;
}

Dictionary EconomySim::crisis_step() {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	sim_core_.phase_crisis();
	status_invalidate();
	return get_crisis_status();
}

void EconomySim::mature_world_efficiency(const Array& buildings) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	std::set<std::string> filter;
	for (int index = 0; index < buildings.size(); ++index) {
		filter.insert(std::string(String(buildings[index]).utf8().get_data()));
	}
	for (Building& building : ws_.buildings) {
		if (!filter.empty() && filter.find(building.id) == filter.end()) {
			continue;
		}
		const auto found = ws_.city_lookup.find(building.city);
		if (found == ws_.city_lookup.end()) {
			ogs::eff::mature(building, true);
			continue;
		}
		const City& city = ws_.cities[static_cast<std::size_t>(found->second)];
		const ogs::EfficiencyClass cls = ogs::eff::classify(ws_, city,
			building);
		const bool powered = cls != ogs::EfficiencyClass::kFactory
			&& cls != ogs::EfficiencyClass::kRefinery;
		ogs::eff::mature(building, powered);
	}
	status_invalidate();
}

Dictionary EconomySim::fleet_buy(const String& nation, const String& product,
		double amount, double budget_share) {
	ogs::Command cmd;
	cmd.type = ogs::CommandType::FleetBuy;
	cmd.s[0] = to_std(nation);
	cmd.s[1] = to_std(product);
	cmd.v[0] = amount;
	cmd.v[1] = budget_share;
	return dispatch_command(cmd);
}

Dictionary EconomySim::fund_group(const String& group_id, double amount) {
	ogs::Command cmd;
	cmd.type = ogs::CommandType::FundGroup;
	cmd.s[0] = to_std(group_id);
	cmd.v[0] = amount;
	return dispatch_command(cmd);
}

Dictionary EconomySim::group_invest(const String& group_id,
		const String& city_id, const String& building_id, const String& resource,
		int output_product, double capacity, const String& recipe_id,
		const String& partner_group) {
	ogs::Command cmd;
	cmd.type = ogs::CommandType::GroupInvest;
	cmd.s[0] = to_std(group_id);
	cmd.s[1] = to_std(city_id);
	cmd.s[2] = to_std(building_id);
	cmd.s[3] = to_std(resource);
	cmd.s[4] = to_std(recipe_id);
	cmd.s[5] = to_std(partner_group);
	cmd.i[0] = output_product;
	cmd.v[0] = capacity;
	return dispatch_command(cmd);
}

Dictionary EconomySim::set_fdi_policy(const String& nation,
		const String& sector, const String& mode, double min_local) {
	ogs::Command cmd;
	cmd.type = ogs::CommandType::SetFdiPolicy;
	cmd.s[0] = to_std(nation);
	cmd.s[1] = to_std(sector);
	cmd.s[2] = to_std(mode);
	cmd.v[0] = min_local;
	return dispatch_command(cmd);
}

Dictionary EconomySim::set_sector_incentive(const String& nation,
		const String& sector, double tax_holiday, double subsidy) {
	ogs::Command cmd;
	cmd.type = ogs::CommandType::SetSectorIncentive;
	cmd.s[0] = to_std(nation);
	cmd.s[1] = to_std(sector);
	cmd.v[0] = tax_holiday;
	cmd.v[1] = subsidy;
	return dispatch_command(cmd);
}

Dictionary EconomySim::create_state_company(const String& nation,
		const String& name, double capital) {
	ogs::Command cmd;
	cmd.type = ogs::CommandType::CreateStateCompany;
	cmd.s[0] = to_std(nation);
	cmd.s[1] = to_std(name);
	cmd.v[0] = capital;
	return dispatch_command(cmd);
}

Dictionary EconomySim::privatize(const String& city_id,
		const String& building_id, const String& buyer_group) {
	ogs::Command cmd;
	cmd.type = ogs::CommandType::Privatize;
	cmd.s[0] = to_std(city_id);
	cmd.s[1] = to_std(building_id);
	cmd.s[2] = to_std(buyer_group);
	return dispatch_command(cmd);
}

Dictionary EconomySim::nationalize(const String& city_id,
		const String& building_id) {
	ogs::Command cmd;
	cmd.type = ogs::CommandType::Nationalize;
	cmd.s[0] = to_std(city_id);
	cmd.s[1] = to_std(building_id);
	return dispatch_command(cmd);
}

Dictionary EconomySim::concede(const String& city_id,
		const String& building_id, const String& group_id, double royalty_rate) {
	ogs::Command cmd;
	cmd.type = ogs::CommandType::Concede;
	cmd.s[0] = to_std(city_id);
	cmd.s[1] = to_std(building_id);
	cmd.s[2] = to_std(group_id);
	cmd.v[0] = royalty_rate;
	return dispatch_command(cmd);
}

Dictionary EconomySim::urban_expand(const String& city_id, int zone, double km2) {
	ogs::Command cmd;
	cmd.type = ogs::CommandType::UrbanExpand;
	cmd.s[0] = to_std(city_id);
	cmd.i[0] = zone;
	cmd.v[0] = km2;
	return dispatch_command(cmd);
}

Dictionary EconomySim::urban_densify(const String& city_id, int zone) {
	ogs::Command cmd;
	cmd.type = ogs::CommandType::UrbanDensify;
	cmd.s[0] = to_std(city_id);
	cmd.i[0] = zone;
	return dispatch_command(cmd);
}

Dictionary EconomySim::urban_social_housing(const String& city_id, double units) {
	ogs::Command cmd;
	cmd.type = ogs::CommandType::UrbanSocialHousing;
	cmd.s[0] = to_std(city_id);
	cmd.v[0] = units;
	return dispatch_command(cmd);
}

Dictionary EconomySim::deepen_mine(const String& city_id,
		const String& building_id) {
	ogs::Command cmd;
	cmd.type = ogs::CommandType::DeepenMine;
	cmd.s[0] = to_std(city_id);
	cmd.s[1] = to_std(building_id);
	return dispatch_command(cmd);
}

Dictionary EconomySim::infra_build(const String& city_id, int infra_type) {
	ogs::Command cmd;
	cmd.type = ogs::CommandType::InfraBuild;
	cmd.s[0] = to_std(city_id);
	cmd.i[0] = infra_type;
	return dispatch_command(cmd);
}

void EconomySim::set_service_capacity(const String& city_id,
		const String& sector, double units_per_day) {
	ogs::Command cmd;
	cmd.type = ogs::CommandType::SetServiceCapacity;
	cmd.s[0] = to_std(city_id);
	cmd.s[1] = to_std(sector);
	cmd.v[0] = units_per_day;
	dispatch_command(cmd);
}

void EconomySim::set_service_business_state(int business_id, int loss_days,
		double profit) {
	ogs::Command cmd;
	cmd.type = ogs::CommandType::SetServiceBusinessState;
	cmd.i[0] = business_id;
	cmd.i[1] = loss_days;
	cmd.v[0] = profit;
	dispatch_command(cmd);
}

} // namespace godot
