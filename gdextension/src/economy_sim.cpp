#include "economy_sim.h"
#include "edge/save_load.h"
#include "edge/value_bridge.h"
#include "sim/actions.h"
#include "sim/agriculture.h"
#include "sim/balance.h"
#include "sim/bank.h"
#include "sim/central_bank.h"
#include "sim/exchange.h"
#include "sim/monetary.h"
#include "sim/storage.h"
#include "sim/profiling.h"
#include "sim/save_definitions.h"
#include "sim/save_state.h"
#include "sim/sim_constants.h"
#include "sim/scenario_fixture.h"
#include "sim/sim_day.h"
#include "sim/urban_land.h"
#include "sim/housing.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_float64_array.hpp>
#include <godot_cpp/variant/string.hpp>
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
// `dispatch_command` (A3). Barato e estável durante toda a sessão.
std::size_t current_thread_hash() {
	return std::hash<std::thread::id>{}(std::this_thread::get_id());
}

} // namespace

using namespace ogs;

namespace {

// Segurança numérica: nada de NaN/Inf no estado nem nos outputs.
inline double safe_value(double v) {
	return std::isfinite(v) ? v : 0.0;
}
inline double safe_nonneg(double v) {
	return std::isfinite(v) ? std::max(0.0, v) : 0.0;
}
inline double safe_unit(double v) {
	return std::isfinite(v) ? std::clamp(v, 0.0, 1.0) : 0.0;
}

// §19: leitura defensiva de um sub-dicionário do JSON carregado pelo GDScript
// (chave ausente ou de outro tipo vira dicionário vazio — ausência explícita).
inline Dictionary dict_value(const Dictionary& parent, const String& key) {
	const Variant value = parent.get(key, Variant());
	return value.get_type() == Variant::DICTIONARY ? Dictionary(value) : Dictionary();
}

// B3: lê o bloco `region` de um dict de cidade (state_regions.json já
// resolvido pelo GDScript) e preenche o struct Region. Devolve false quando o
// bloco está ausente/inválido — região inerte (ausência explícita).
inline bool parse_region(const Dictionary& d, Region& region) {
	if (!d.has("population_region") && !d.has("urban_population")) {
		return false;
	}
	region.ready = true;
	region.population_region = safe_nonneg(
		static_cast<double>(d.get("population_region", 0.0)));
	const double urban = safe_nonneg(
		static_cast<double>(d.get("urban_population", 0.0)));
	// Urbano capado no total (20 cidades têm urban > population_region: as 19
	// abaixo do urbano da D-005 + SGP-SIN, cuja região é 0).
	region.urban_population = std::min(urban, region.population_region);
	// Invariante: interior = total - urbano (piso 0). O campo do JSON
	// (`region_population`) é só referência; recalculamos para o invariante
	// valer mesmo com dado inconsistente.
	region.region_pop = std::max(0.0,
		region.population_region - region.urban_population);
	const Variant us = d.get("urban_share", Variant());
	if (us.get_type() == Variant::FLOAT || us.get_type() == Variant::INT) {
		const double value = static_cast<double>(us);
		// -1 (ou negativo) = sem dado (mix neutro); só valores >= 0 são fração.
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

// §19: height_class do urban_land.json → gabarito do jogo. Valor ausente ou
// desconhecido cai em baixa densidade (o save antigo não tem o campo).
inline Density density_from_height_class(const String& height_class) {
	if (height_class == "high") {
		return DENSITY_HIGH;
	}
	if (height_class == "mid") {
		return DENSITY_MID;
	}
	return DENSITY_LOW;
}

// População de SIMULAÇÃO da cidade. Desde a calibração de 2026, `population`
// no cenário é a população real exibida (nação inteira rateada) e a aglomeração
// da fonte viaja em `sim_population`. Sem o campo (save antigo, cenário
// sintético), a população histórica É a unidade de simulação — comportamento
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
	// Produtos padrão (21 primários) e cesta alimentar validada. O autoload
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
	// F3: sem goods.json carregado nenhum dos 21 primários é durável.
	ws_.product_durable.assign(kMaxProducts, 0);
}

EconomySim::~EconomySim() {
	// Se o sim for liberado com um dia AINDA rodando na thread do pool (o jogo
	// encerrando, por exemplo), espera o tick sair antes de destruir os
	// contêineres do status: destruí-los debaixo de quem os escreve era um
	// access violation (0xC0000005) intermitente no fim do processo. O mutex é
	// recursivo, então a mesma thread do tick não bloqueia aqui.
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
	ClassDB::bind_method(D_METHOD("price_shock", "product", "multiplier"),
	                     &EconomySim::price_shock);
	ClassDB::bind_method(D_METHOD("set_suitability", "city_id", "product", "value"),
	                     &EconomySim::set_suitability);
	ClassDB::bind_method(D_METHOD("set_land_capacity", "city_id", "product", "value"),
	                     &EconomySim::set_land_capacity);
	// G (D-096) — agricultura sazonal.
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
	// C2b2 (D-106) — regimes de câmbio, padrão monetário e ouro físico.
	ClassDB::bind_method(D_METHOD("set_exchange_regime", "currency", "regime",
		"parity", "band_width", "nation"),
		&EconomySim::set_exchange_regime, DEFVAL(String()));
	// C2c (D-108) — regimes reais (IMF AREAER 2026) e sucessão de moeda.
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
	// C3 (D-107): crédito de empresa (banco agregado como credor).
	ClassDB::bind_method(D_METHOD("set_bank_credit", "enabled"),
		&EconomySim::set_bank_credit);
	ClassDB::bind_method(D_METHOD("set_bank_policy", "spread", "leverage_cap",
		"interest_cover", "default_days", "repay_share"),
		&EconomySim::set_bank_policy);
	ClassDB::bind_method(D_METHOD("set_bank_capital", "nation", "amount"),
		&EconomySim::set_bank_capital);
	ClassDB::bind_method(D_METHOD("group_borrow", "group_id", "amount"),
		&EconomySim::group_borrow);
	ClassDB::bind_method(D_METHOD("get_bank_status"), &EconomySim::get_bank_status);
	ClassDB::bind_method(D_METHOD("set_storage_stock", "city_id", "type",
		"owner", "product", "tons"), &EconomySim::set_storage_stock);
	ClassDB::bind_method(D_METHOD("get_storage_product_types"),
		&EconomySim::get_storage_product_types);
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
	// A5: fase da IA no fim do dia (desligada por padrão).
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
	// IEEE-exato) — o snapshot não atravessa o JSON do Godot.
	ClassDB::bind_method(D_METHOD("to_snapshot_file", "path"),
	                     &EconomySim::to_snapshot_file);
	ClassDB::bind_method(D_METHOD("from_snapshot_file", "path"),
	                     &EconomySim::from_snapshot_file);
	ClassDB::bind_method(D_METHOD("set_balance", "values"),
	                     &EconomySim::set_balance);
	ClassDB::bind_method(D_METHOD("set_fleet", "nation", "trucks", "ships",
	                     "planes", "fuel", "spare_parts"),
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

// Cenário de validação: duas cidades, POPs agregados, trigo consumido direto.
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
			// A2: o índice inteiro acompanha a mudança (a tabela de nações já
			// existe neste ponto; o rebuild é da ordem de centenas de linhas).
			// A3: a invalidação fica em `apply_pending_locked`, ponto único do lote.
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
	// `base_rate` fixa a referência da banda e da competitividade float. Sem
	// ele, a taxa informada vira a própria referência (comportamento anterior).
	currency.base_rate = (std::isfinite(base_rate) && base_rate > 0.0)
		? base_rate : rate;
	currency.floating = floating;
	// C2b2 (D-106): o regime salvo é a fonte do ramo do dia (float/peg/band).
	// O toggle do F5 (`set_currency_rate(..., floating)`) tem de mover os dois
	// juntos; senão a moeda fica `floating` para o câmbio sombreado e `peg`
	// para o ramo do dia, e a taxa congela (moeda de fixture parada em 1,0).
	// `band` não é rebaixada: ela já flutua dentro da banda.
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
	// Fonte do arquivo inteiro (o status publica o que está valendo).
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
		// Regime: o vocabulário do arquivo é o do jogo (floating/band/peg).
		// "unknown" (sem fonte no FMI) é preenchido pela declaração do emissor
		// no DADO; se chegar vazio, o carregador declara band de jogo (§D-108d)
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
			info.source = source + " — default de jogo";
		}
		// Composição PUBLICADA (quando o dado traz): {moeda: peso}.
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
		// Emissor: a nação da moeda quando ela é de um país só. Moeda
		// compartilhada por várias nações (união monetária/dolarização) fica
		// TRAVADA: ninguém muda o regime (regra de bloco é fase futura).
		const Variant nations_variant = entry.get("nations", Array());
		if (nations_variant.get_type() == Variant::ARRAY) {
			const Array nations = nations_variant;
			const bool shared = bool(entry.get("shared_currency", false));
			info.locked = shared || nations.size() > 1;
			if (nations.size() == 1) {
				info.issuer = std::string(String(nations[0]).utf8().get_data());
			}
		}
		// Dolarizados / sem moeda própria: sem política cambial.
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
	// D-108(d): moeda sem classificação AREAER não fica sem regime. O bloco
	// `game_fallback` do MESMO arquivo declara o default de jogo (e a fonte,
	// quando há uma declaração do emissor). Não é dado observado.
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
	// O mesmo carregador roda sobre um mundo já criado (teste/API): aplica o
	// regime às moedas que estão no estado. Em mundo vazio, `set_currencies`
	// aplica depois (o mundo nasce com o regime real).
	for (Currency& currency : ws_.currencies) {
		const auto found = ws_.fx_regime.find(currency.code);
		if (found == ws_.fx_regime.end()) {
			continue;
		}
		currency.exchange_regime = found->second.regime;
		// `band` deriva dentro da faixa (C2b2 §8); `peg` não deriva.
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

void EconomySim::set_currencies(const Variant& currencies) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Array list;
	// Sem `regime` declarado no dado a taxa fica FIXA (`peg`). O dado é do F1 e
	// não traz a fonte do regime cambial (currencies.json: `regime: null`), então
	// inventar flutuação violaria a regra "não invente dados" — e o drift diário
	// move preços/competitividade sem fonte. `regime: "float"` no JSON ativa o
	// drift por balança.
	std::string default_regime = "peg";
	if (currencies.get_type() == Variant::DICTIONARY) {
		const Dictionary data = currencies;
		const Variant entries = data.get("currencies", Array());
		if (entries.get_type() == Variant::ARRAY) {
			list = entries;
		}
		// `regime: null` no dado (currencies.json não tem fonte de regime) NÃO
		// é flutuação: só o valor explícito "float" ativa o drift e o fator de
		// competitividade. Qualquer outro valor mantém a taxa FIXA.
		const Variant regime_var = data.get("regime", Variant());
		if (regime_var.get_type() == Variant::STRING) {
			const String regime = regime_var;
			if (!regime.is_empty()) {
				default_regime = std::string(regime.utf8().get_data());
			}
		}
	} else if (currencies.get_type() == Variant::ARRAY) {
		list = currencies;
	}
	if (list.is_empty()) {
		return;
	}

	// Fallback documentado para moedas sem PIB PPP (currencies.json:
	// `rate_per_anchor: null`, `source: unknown:no_gdp`): usa a mediana das
	// taxas conhecidas e marca source `fallback:median`. A taxa 1,0 fica como
	// último recurso (nunca inventa um PIB).
	std::vector<double> known_rates;
	known_rates.reserve(static_cast<std::size_t>(list.size()));
	for (int index = 0; index < list.size(); ++index) {
		const Dictionary entry = list[index];
		if (!entry.has("rate_per_anchor")) {
			continue;
		}
		const Variant raw = entry.get("rate_per_anchor", Variant());
		if (raw.get_type() == Variant::NIL) {
			continue;
		}
		const double rate = static_cast<double>(raw);
		if (std::isfinite(rate) && rate > 0.0) {
			known_rates.push_back(rate);
		}
	}
	double fallback_rate = 1.0;
	if (!known_rates.empty()) {
		std::sort(known_rates.begin(), known_rates.end());
		fallback_rate = known_rates[known_rates.size() / 2];
	}

	std::vector<Currency> next;
	next.reserve(static_cast<std::size_t>(list.size()));
	for (int index = 0; index < list.size(); ++index) {
		const Dictionary entry = list[index];
		const String code = String(entry.get("code", ""));
		if (code.is_empty()) {
			continue;
		}
		Currency currency;
		currency.code = std::string(code.utf8().get_data());
		currency.source = std::string(String(entry.get("source", "default")).utf8().get_data());
		const String regime = String(entry.get("regime", String(default_regime.c_str())));
		currency.floating = regime == "float";
		// C2a (D-103): o regime da política nasce do regime já aceito pelo
		// F5 — nenhuma mecânica nova. "band" (banda administrada) usa o mesmo
		// drift dentro da banda do fx.cpp; a intervenção é C2b.
		currency.exchange_regime = central_bank::kRegimePeg;
		if (regime == "float") {
			currency.exchange_regime = central_bank::kRegimeFloating;
		} else if (regime == "band") {
			currency.exchange_regime = central_bank::kRegimeBand;
			currency.floating = true;
		}
		currency.bank_independence = central_bank::kIndependenceNoData;
		bool valid = false;
		if (entry.has("rate_per_anchor")) {
			const Variant raw = entry.get("rate_per_anchor", Variant());
			if (raw.get_type() != Variant::NIL) {
				const double rate = static_cast<double>(raw);
				if (std::isfinite(rate) && rate > 0.0) {
					currency.rate = rate;
					valid = true;
				}
			}
		}
		if (!valid) {
			currency.rate = fallback_rate;
			currency.fallback = true;
			currency.source = "fallback:median";
		}
		currency.base_rate = currency.rate;
		const Variant nations_variant = entry.get("nations", Array());
		if (nations_variant.get_type() == Variant::ARRAY) {
			const Array nations = nations_variant;
			for (int n = 0; n < nations.size(); ++n) {
				const std::string nation(nations[n].operator String().utf8().get_data());
				if (!nation.empty()) {
					currency.nations.push_back(nation);
				}
			}
		}
		next.push_back(std::move(currency));
	}
	if (next.empty()) {
		return;
	}
	// C2c (D-108): o regime inicial vem do IMF AREAER (fx_regimes_2026.json),
	// não mais do "peg administrativo" único. Moeda sem entrada no arquivo
	// mantém o que o dado declarou (nada é inventado).
	for (Currency& currency : next) {
		const auto found = ws_.fx_regime.find(currency.code);
		if (found == ws_.fx_regime.end()) {
			continue;
		}
		currency.exchange_regime = found->second.regime;
		currency.floating = found->second.regime
			!= ogs::central_bank::kRegimePeg;
	}
	ws_.currencies = std::move(next);
	rebuild_currency_indexes();
	// Paridade CRUZADA inicial: a taxa com que o mundo nasce vira o ponto de
	// partida do peg (nenhuma paridade nominal é publicada pelo FMI).
	ogs::exchange::initialize_anchors(ws_);

	// O preço interno está na âncora: carregar moedas NÃO reescala preços. A
	// taxa só entra na exibição local e na competitividade float.
	if (ws_.active && !ws_.cities.empty()) {
		++status_static_version_;
		status_invalidate();
	}
}


void EconomySim::set_goods(const Array& goods) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	// Tabela de produtos: 21 primários nas posições 0..20 (ordem do goods.json
	// legado) e os bens novos em `order` (21..112). O núcleo não lê arquivos:
	// todo o conteúdo entra por aqui, no padrão do set_goods antigo.
	const int previous_products = ws_.products;
	std::vector<std::string> names = ws_.product_names;
	std::vector<double> prices(kMaxProducts, 0.0);
	std::vector<char> durable(kMaxProducts, 0);
	std::vector<std::string> sectors = ws_.product_sectors;
	sectors.resize(kMaxProducts);
	for (int index = 0; index < previous_products; ++index) {
		if (index < static_cast<int>(names.size())) {
			prices[static_cast<std::size_t>(index)] = ws_.base_prices[index];
		}
	}
	for (int index = 0; index < static_cast<int>(ws_.product_durable.size()); ++index) {
		durable[static_cast<std::size_t>(index)] = ws_.product_durable[static_cast<std::size_t>(index)];
	}
	for (int index = 0; index < goods.size(); ++index) {
		const Dictionary entry = goods[index];
		const String id = String(entry.get("id", ""));
		if (id.is_empty()) {
			continue;
		}
		int order = entry.has("order") ? int(entry.get("order", index)) : index;
		order = std::clamp(order, 0, kMaxProducts - 1);
		if (static_cast<int>(names.size()) <= order) {
			names.resize(static_cast<std::size_t>(order) + 1);
		}
		names[static_cast<std::size_t>(order)] = std::string(id.utf8().get_data());
		const double price = safe_nonneg(static_cast<double>(entry.get("base_price", 0.0)));
		if (price > 0.0) {
			prices[static_cast<std::size_t>(order)] = price;
		}
		durable[static_cast<std::size_t>(order)] =
			bool(entry.get("durable", false)) ? 1 : 0;
		const String sector = String(entry.get("sector", ""));
		sectors[static_cast<std::size_t>(order)] =
			sector.is_empty() ? std::string() : std::string(sector.utf8().get_data());
	}
	int product_count = 21;
	for (int index = 0; index < static_cast<int>(names.size()); ++index) {
		if (!names[static_cast<std::size_t>(index)].empty()) {
			product_count = std::max(product_count, index + 1);
		}
	}
	product_count = std::min(product_count, kMaxProducts);
	names.resize(static_cast<std::size_t>(product_count));
	ws_.product_names = std::move(names);
	ws_.product_lookup.clear();
	ws_.product_lookup.reserve(static_cast<std::size_t>(product_count) * 2);
	for (int index = 0; index < product_count; ++index) {
		const std::string& name = ws_.product_names[static_cast<std::size_t>(index)];
		if (!name.empty()) {
			ws_.product_lookup[name] = index;
		}
		const double price = prices[static_cast<std::size_t>(index)];
		ws_.base_prices[index] = price > 0.0 ? price : 40.0;
	}
	ws_.products = product_count;
	ws_.product_durable = std::move(durable);
	ws_.product_sectors = std::move(sectors);
	ws_.product_sectors.resize(static_cast<std::size_t>(product_count));

	// Mundo já inicializado: os produtos novos entram com o preço-base e sem
	// estoque. Os índices existentes (0..previous_products-1) não mudam.
	if (ws_.active && product_count > previous_products) {
		for (City& city : ws_.cities) {
			for (int product = previous_products; product < product_count; ++product) {
				city.markets[product].price = base_price(product);
			}
		}
		++status_static_version_;
		++status_deposit_version_;
		status_invalidate();
	}
	// F4: os índices dos bens da frota dependem dos ids carregados. Se o mundo
	// já existe e ainda não tem frota (set_goods chamado depois da
	// inicialização, como no autoload), deriva agora.
	ws_.fleet_product_fuel = product_index(String("fuel"));
	ws_.fleet_product_truck = product_index(String("truck"));
	ws_.fleet_product_ship = product_index(String("cargo_ship"));
	ws_.fleet_product_plane = product_index(String("cargo_plane"));
	ws_.fleet_product_parts = product_index(String("spare_parts"));
	if (ws_.active && ws_.fleets.empty()) {
		init_fleets();
	}
}

void EconomySim::initialize_scenario() {
	// Novo mundo: o cache de status pertence ao mundo anterior.
	status_slots_[0] = StatusCache{};
	status_slots_[1] = StatusCache{};
	status_live_ = -1;
	status_built_day_ = -1;
	++status_static_version_;
	++status_deposit_version_;
	// A3: a fila de comandos e o log também são do mundo anterior.
	commands_.reset();
	// A5: o estado da IA pertence ao mundo — contadores, janelas e templates
	// de um cenário não valem no outro.
	ai_phase_.reset();
	current_day_.store(0, std::memory_order_relaxed);
	// S14: o cenário AGRIA/PORTUS vive em sim/scenario_fixture.cpp (C++ puro);
	// aqui só o cache de status (estado Godot) é resetado.
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
	// A3: a fila de comandos e o log também são do mundo anterior.
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
	// §19: mundo novo zera os vetores urbanos; o set_urban os preenche
	// depois (o casamento é pelo id da cidade criada aqui).
	ws_.land.clear();
	ws_.housing.clear();
	ws_.city_gov.clear();
	ws_.in_transit.clear();
	ws_.invariant_violations.clear();
	ws_.ports.clear();
	ws_.last_orders.clear();
	ws_.bankruptcies = 0;
	// D-099: composição das falências é diagnóstico de runtime — mundo novo
	// começa do zero.
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
	// F9: mundo novo, grupos/política novos (a semeadura roda no fim).
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

	// Cidades reais do cenário: identidade geográfica + caixas iniciais.
	for (int index = 0; index < cities.size(); ++index) {
		const Dictionary d = cities[index];
		City city;
		city.id = std::string(String(d["id"]).utf8().get_data());
		city.name = std::string(String(d["name"]).utf8().get_data());
		city.nation = std::string(String(d["nation"]).utf8().get_data());
		city.home_nation =
			std::string(String(d.get("home_nation", d["nation"])).utf8().get_data());
		city.landmass = std::string(String(d.get("landmass", "")).utf8().get_data());
		// B3: região da cidade (estados inteiros). Com dado (`region` no dict),
		// `population` vira o total da região e `simulation_population` vira a
		// aglomeração urbana (capada). Sem o bloco (cenário sintético), mantém o
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

		// Caixas iniciais por habitante (constantes do modelo demo). A escala é
		// a população de SIMULAÇÃO: a população real exibida não muda o balanço.
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
		// F7: perfil inicial de trabalho/educação a partir do índice de renda
		// (1,0 por padrão; o GeographyService reaplica quando tem o PIB real).
		apply_labor_profile(city);
		// B3: o bloco da região (estados inteiros) foi parseado acima; semeia a
		// demografia e os quintis do interior e reconcilia a população exibida.
		city.region = std::move(region);
		if (city.region.ready) {
			sim_core_.seed_region(city);
		}
		// Aptidão padrão (inicializador do struct: suitability = {1.0, 1.0, 1.0,
		// 0, ...} — trigo/ferro/arroz em 1.0). O GeographyService sobrescreve com
		// o dado real em tempo de execução.
		city.suitability[0] = 1.0;
		for (int product = 0; product < ws_.products; ++product) {
			city.markets[product] = {0.0, base_price(product),
				0.0, 0.0};
		}
		// §15: estoque inicial de materiais de construção por cidade (a "obra em
		// estoque" pré-existente, derivada da população de simulação). Sem ele a
		// construção real (sem produtor de materiais no mundo inicial) travaria
		// no primeiro dia. `derived:initial_stock` — não é dado observado.
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

	// Construções reais (fazendas primárias): o produto vem do campo "resource"
	// (resolvido pelo GeographyService para o alimento básico de maior aptidão ×
	// caloria da nação da cidade); ausente, mantém trigo (comportamento legado).
	// A produção efetiva segue multiplicando a capacidade pela aptidão do produto
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
		// D-086: mina/poço inicial já EXISTENTE. `stage` = -1 no contrato do
		// GeographyService significa "último estágio" (o número mora aqui, não
		// no GDScript): a construção histórica produz no piso (5%).
		// D-086b: `stage >= 0` é a mina semeada PELA PRODUÇÃO REAL de 2024/2025
		// (lista de extração, `extraction_2025.json`) — ela nasce operando e a
		// reserva do estágio é aberta quando a jazida é declarada
		// (`apply_set_deposit`), porque a semente chega antes do `set_deposit`.
		// Dono padrão: Estado.
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
		// Aptidão da cultura escolhida na bacia da cidade (opcional; 1.0 no
		// caminho legado). A capacidade nominal é o teto de terra; a produção
		// efetiva da fase 4b é capacidade × aptidão. Folha e opex acompanham a
		// FRAÇÃO APTA da terra — antes seguiam a capacidade nominal inteira e
		// uma fazenda de aptidão 4% pagava 100% dos custos para produzir 4%,
		// falindo em 60 dias (§19). Sem essa correção o mundo perdia 83 das 252
		// fazendas iniciais no dia 61.
		const double aptitude = std::clamp(
			safe_nonneg(static_cast<double>(d.get("aptitude", 1.0))), 0.0, 1.0);
		// D-086b: a semente de produção real informa a régua de engenharia da
		// MINA (a mesma tabela que a IA usa para abrir/expandir mina). Sem os
		// campos, o caminho legado (fazenda: 7,5 empregos/t, 2,5 opex/t) fica
		// intacto — fazendas e minas históricas não mudam. A mina semeada com a
		// régua da fazenda pagava ~2-4x a folha de uma mina igual da IA e
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
		ws_.buildings.push_back(std::move(building));
	}

	// Portos do cenário (identidade geográfica, para o minimapa e os comboios).
	for (int index = 0; index < ports.size(); ++index) {
		const Dictionary d = ports[index];
		Port port;
		port.id = std::string(String(d["id"]).utf8().get_data());
		port.name = std::string(String(d["name"]).utf8().get_data());
		port.nation = std::string(String(d["nation"]).utf8().get_data());
		port.center = {static_cast<double>(d["x"]), static_cast<double>(d["y"])};
		port.landmass = std::string(String(d.get("landmass", "")).utf8().get_data());
		// Cidade-sede: só a âncora do porto (não é dono exclusivo do tráfego).
		port.host_city = std::string(String(d.get("port_of", d.get("host_city", "")))
			.utf8().get_data());
		ws_.ports.push_back(std::move(port));
	}

	rebuild_indexes();
	// Capacidade de nível 1 depende da população da cidade-sede: só depois do
	// índice de cidades (e de os portos existirem).
	init_port_capacities();
	// F4: frota inicial derivada de população/PIB (source: derived). Os bens da
	// frota só resolvem depois de set_goods; aqui as contagens/estoques já
	// existem e os índices são atualizados quando os bens chegam.
	init_fleets();
	// F8: serviços — catálogo já carregado pelo autoload/teste; os negócios
	// iniciais saem do caixa do próprio setor (capital fica em service_cash).
	seed_services();
	// F9: grupos/empresas a partir dos ativos privados existentes.
	seed_groups();
}

// ---------------------------------------------------------------------------
// F4 — frota nacional e combustível.
//
// Contrato: cada trecho de um pedido exige (a) veículo livre da nação
// VENDEDORA na capacidade do ModeSpec (caminhão 25 t, navio 2.000 t, avião 20 t)
// e (b) combustível no estoque nacional, consumido por t·km. Sem os dois, a
// carga não embarca; a diferença entra em `queue_today` e o governo repõe frota
// e combustível na manutenção mensal. Não há número fora do dado: a frota
// inicial é derivada de população/PIB (`source: derived`), o combustível é um
// bem físico (fuel, order 47) e os veículos são os bens finais da F2
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
	// Capacidade efetiva de um lote por modal (t) — veículos × saúde × carga.
	out["capacity_tons"] = fleet.trucks * fleet.health[0] * 25.0
		+ fleet.ships * fleet.health[1] * 2000.0
		+ fleet.planes * fleet.health[2] * 20.0;
	out["fuel"] = fleet.fuel;
	out["fuel_target"] = fleet.fuel_target;
	out["spare_parts"] = fleet.spare_parts;
	out["fuel_used_today"] = fleet.fuel_used_today;
	out["fuel_used_total"] = fleet.fuel_used_total;
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
		double planes, double fuel, double spare_parts) {
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
	fleet.fuel = std::max(0.0, fuel);
	fleet.fuel_target = fleet.fuel;
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

	// Obra pública: paga o orçamento da cidade-sede e credita o setor de
	// construção (dinheiro conservado).
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
	// Só a flag: o estado entre dias da IA pertence ao MUNDO (resetado em
	// initialize_*/from_snapshot*, preservado no load por from_dict).
	ai_enabled_ = enabled;
}

Dictionary EconomySim::tick_day() {
	tick_and_publish();
	// A4: tools/testes/goldens recebem o status completo SOB PEDIDO. Este
	// caminho mantém a cadência por slot da S17 exatamente como antes (o
	// primeiro build de um slot calcula o dia corrente) e desliga de vez a
	// semeadura pelo retrato semanal. O jogo usa `tick_and_publish` +
	// `get_day_summary` e nunca chega aqui; o agendador de teste, que só chama
	// `tick_and_publish`, mantém a semeadura.
	status_seed_labor_from_snapshot_ = false;
	return get_status();
}

void EconomySim::tick_and_publish() {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	// A3: enquanto o dia roda, quem muta o mundo só enfileira (dispatch_command
	// vê a flag e não toca no `ws_`). A flag é limpa na saída, inclusive quando
	// o dia sai por exceção.
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
	// `profile_` como interruptor (desligado, nenhum relógio é lido).
	ogs::profile::Sink phase_sink{profile_us_, kPhaseCount, &profile_};
	const ogs::profile::SinkScope phase_scope(&phase_sink);
#endif
	tick_core();
	if (ai_enabled_) {
		// A5/D-076: fase da IA no FIM do dia, já com o dia fechado (`ws_.day` é
		// o dia que acabou de rodar) e ANTES da publicação do status. A IA lê
		// um retrato próprio (`AiView`), não o status, então a ordem não muda
		// nenhuma decisão; e cada comando emitido é aplicado na hora pela
		// própria fase (ai_emit_*), então o status abaixo já sai com os
		// efeitos do dia. Rodar a IA depois do status obrigava a invalidá-lo e
		// a construí-lo DUAS vezes por dia (~20 ms), e a segunda construção
		// lia o cache semanal do slot anterior — publicando `nations[].labor`
		// com até 13 dias de atraso.
		OGS_SIM_ZONE(kPhaseAi);
		ai_phase_.run(ws_, sim_core_, commands_, instant_construction_,
			bootstrap_construction_);
	}
	{
		// Fase `status` (RAII). A4: o dia NÃO monta mais o Dictionary do mundo
		// inteiro. No fecho semanal captura-se o retrato de trabalho das nações
		// (S17), em doubles puros; o resumo leve do dia é montado sob demanda
		// por `get_day_summary()` depois do join (main thread).
		OGS_SIM_ZONE(kPhaseStatus);
		if ((ws_.day % 7) == 0 || labor_snapshot_day_ < 0) {
			capture_nation_labor();
		}
	}
	current_day_.store(ws_.day, std::memory_order_relaxed);
	++data_version_;
}

void EconomySim::status_invalidate() {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	status_built_day_ = -1;
	++data_version_;
	// A4: as consultas cacheadas por dia também ficam velhas.
	columns_day_ = -1;
	query_rows_day_ = -1;
	query_light_day_ = -1;
	query_orders_day_ = -1;
}

int EconomySim::get_data_version() const {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	return data_version_;
}

// ---------------------------------------------------------------------------
// A3 - fila de comandos (sim/commands.h; racional em
// docs/economy/refactor/A3_commands.md).
//
// Cada método que muda o mundo (jogador ou IA) agora só monta um Command,
// enfileira e pede a aplicação. A ordem de aplicação é a de emissão; a fila é
// aplicada (a) no início do dia, antes do prólogo (tick_core), e (b) na hora,
// quando o método precisa devolver o efeito (contrato de retorno de invest,
// build, group_invest, ...) - esse é o único caminho síncrono.
// ---------------------------------------------------------------------------

namespace {

// Log/dict <-> Command. O log é a evidência do replay e sai em JSON pelo
Dictionary command_to_dict(const ogs::Command& cmd) {
	Dictionary entry;
	const char* name = ogs::command_type_name(cmd.type);
	entry["type"] = String(name != nullptr ? name : "");
	entry["day"] = cmd.day;
	entry["apply_day"] = cmd.apply_day;
	entry["seq"] = static_cast<int64_t>(cmd.seq);
	Array strings;
	for (int slot = 0; slot < ogs::kCommandStrings; ++slot) {
		if (cmd.s[slot].empty()) {
			break;
		}
		strings.push_back(String(cmd.s[slot].c_str()));
	}
	if (!strings.is_empty()) {
		entry["s"] = strings;
	}
	bool has_double = false;
	for (int slot = 0; slot < ogs::kCommandDoubles; ++slot) {
		if (cmd.v[slot] != 0.0) {
			has_double = true;
		}
	}
	if (has_double) {
		Array doubles;
		doubles.resize(ogs::kCommandDoubles);
		for (int slot = 0; slot < ogs::kCommandDoubles; ++slot) {
			doubles[slot] = cmd.v[slot];
		}
		entry["v"] = doubles;
	}
	bool has_int = false;
	for (int slot = 0; slot < ogs::kCommandInts; ++slot) {
		if (cmd.i[slot] != 0) {
			has_int = true;
		}
	}
	if (has_int) {
		Array ints;
		ints.resize(ogs::kCommandInts);
		for (int slot = 0; slot < ogs::kCommandInts; ++slot) {
			ints[slot] = cmd.i[slot];
		}
		entry["i"] = ints;
	}
	if (cmd.b) {
		entry["b"] = true;
	}
	return entry;
}

bool command_from_dict(const Dictionary& entry, ogs::Command& out) {
	ogs::CommandType type = ogs::CommandType::None;
	const String type_name = entry.get("type", String());
	if (!ogs::command_type_from_name(
			std::string(type_name.utf8().get_data()), type)) {
		return false;
	}
	out = ogs::Command();
	out.type = type;
	out.day = static_cast<int>(entry.get("day", 0));
	out.apply_day = static_cast<int>(entry.get("apply_day", 0));
	out.seq = static_cast<uint64_t>(static_cast<int64_t>(entry.get("seq", 0)));
	if (entry.has("s")) {
		const Array strings = entry.get("s", Array());
		for (int slot = 0; slot < strings.size() && slot < ogs::kCommandStrings;
				++slot) {
			out.s[slot] = std::string(String(strings[slot]).utf8().get_data());
		}
	}
	if (entry.has("v")) {
		const Array doubles = entry.get("v", Array());
		for (int slot = 0; slot < doubles.size() && slot < ogs::kCommandDoubles;
				++slot) {
			out.v[slot] = static_cast<double>(doubles[slot]);
		}
	}
	if (entry.has("i")) {
		const Array ints = entry.get("i", Array());
		for (int slot = 0; slot < ints.size() && slot < ogs::kCommandInts; ++slot) {
			out.i[slot] = static_cast<int>(ints[slot]);
		}
	}
	out.b = static_cast<bool>(entry.get("b", false));
	return true;
}

} // namespace

Dictionary EconomySim::dispatch_command(const ogs::Command& cmd) {
	// Dia em andamento em OUTRA thread (tick_scheduler): a ação só entra na
	// fila. Nada do `ws_` é tocado aqui, então não há disputa com o escritor
	// único - é o caminho que dispensa o mutex recursivo. O efeito chega no
	// início do próximo dia (`apply_pending_locked` em tick_core).
	//
	// Na PRÓPRIA thread do dia (seed_industry chama build, por exemplo) a
	// mutação aplica na hora: o tick é o escritor, já está sob o mutex e a
	// ordem interna do dia é parte do resultado.
	const bool in_tick_thread = tick_thread_.load(std::memory_order_acquire)
		== current_thread_hash();
	if (tick_in_progress_.load(std::memory_order_acquire) && !in_tick_thread) {
		ogs::Command queued = cmd;
		queued.day = current_day_.load(std::memory_order_relaxed);
		const uint64_t seq = commands_.push(queued);
		Dictionary deferred;
		deferred["queued"] = true;
		deferred["seq"] = static_cast<int64_t>(seq);
		deferred["apply_day"] = queued.day + 1;
		return deferred;
	}
	// Fora do dia: aplica agora, na ordem de emissão. Comandos anteriores na
	// fila (se houver) saem antes - é o que garante a ordem determinística.
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	ogs::Command accepted = cmd;
	accepted.day = ws_.day;
	const uint64_t seq = commands_.push(accepted);
	return apply_pending_locked(seq);
}

Dictionary EconomySim::apply_pending_locked(uint64_t wanted_seq) {
	Dictionary receipt;
	const std::vector<ogs::Command> pending = commands_.take_pending();
	if (pending.empty()) {
		return receipt;
	}
	for (const ogs::Command& pending_cmd : pending) {
		// Cópia: o log grava o dia em que o comando FOI aplicado. Os emitidos
		// entre dias aplicam no mesmo dia; os emitidos com o dia em andamento
		// (outra thread) aplicam no início do dia seguinte - o replay usa
		// `apply_day`, não `day`, para acertar os dois casos.
		ogs::Command cmd = pending_cmd;
		cmd.apply_day = ws_.day;
		const Dictionary result = apply_one_locked(cmd);
		commands_.log(cmd);
		if (wanted_seq != 0 && cmd.seq == wanted_seq) {
			receipt = result;
		}
	}
	// Ponto ÚNICO de invalidação do lote (os aplicadores não invalidam mais).
	if (!tick_in_progress_.load(std::memory_order_relaxed)) {
		status_invalidate();
	}
	return receipt;
}

void EconomySim::apply_commands() {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	apply_pending_locked(0);
}

int EconomySim::pending_command_count() const {
	return static_cast<int>(commands_.pending_size());
}

Array EconomySim::get_command_log() const {
	Array out;
	const std::vector<ogs::Command> entries = commands_.log_entries();
	out.resize(static_cast<int>(entries.size()));
	for (int index = 0; index < out.size(); ++index) {
		out[index] = command_to_dict(entries[static_cast<std::size_t>(index)]);
	}
	return out;
}

void EconomySim::clear_command_log() {
	commands_.clear_log();
}

bool EconomySim::command_log_truncated() const {
	return commands_.truncated();
}

bool EconomySim::replay_commands(const Array& entries, int target_day) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	for (int index = 0; index < entries.size(); ++index) {
		ogs::Command cmd;
		if (!command_from_dict(entries[index], cmd)) {
			return false;
		}
		// O relógio do replay é o dia em que o comando FOI aplicado (não o dia
		// em que foi emitido): os dois coincidem para as ações entre dias e
		// diferem em 1 para as que esperaram a fila (dia em andamento).
		const int when = cmd.apply_day > 0 ? cmd.apply_day : cmd.day;
		while (ws_.day < when) {
			tick_core();
		}
		commands_.push(cmd);
		apply_pending_locked(0);
	}
	while (ws_.day < target_day) {
		tick_core();
	}
	return true;
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
	return profile;
}

// Pré-aquece os dois slots do status. O primeiro dia publicado em um slot
// frio paga a criação de TODOS os dicionários por cidade (medido no cenário
// real: 60 ms contra 17 ms num dia quente). Aqui isso vira custo de carga, e
// nenhum tick paga a alocação. Só roda com o mundo já preenchido (aptidão,
// demanda externa e índice de renda aplicados), senão o bloco estático do
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
	// `built_day_ = -1` faz a próxima chamada construir no OUTRO slot.
	status_built_day_ = -1;
	get_status();
}

void EconomySim::tick_core() {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);

	if (!ws_.active) {
		initialize_scenario();
	}
	// A3: o dia começa aplicando a fila de comandos (ordem de emissão), antes
	// do prólogo e de qualquer fase. Comandos emitidos com o dia em andamento
	// (outra thread) chegam aqui; os emitidos entre dias já foram aplicados
	// pelo dispatch e a fila está vazia.
	apply_pending_locked(0);
	current_day_.store(ws_.day, std::memory_order_relaxed);
	// §12.3: se o mundo real ainda não tem indústria, semeia poucas plantas de
	// referência (moinho + padaria) antes de contar dinheiro/invariantes.
	seed_industry();

	// A ordem do dia (prólogo + fases) é da biblioteca (sim/sim_day.cpp): a
	// mesma sequência roda sem o Godot. Aqui ficam só a inicialização acima e o
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
		// D-086: estágio de mina.
		stage("stage"),
		stage_factor("stage_factor"),
		stage_reserve_kt("stage_reserve_kt"),
		stage_mined_kt("stage_mined_kt"),
		next_deepen_cost("next_deepen_cost"),
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
	// A4: a MESMA conta que o bloco leve do status fazia inline, extraída para
	// a ficha (`get_city_info`) e as colunas (`get_city_columns`) não
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

	// Padrão de vida: quantas cestas alimentares diárias a renda disponível
	// por pessoa compra. Diferente do PIB per capita, usa renda das famílias.
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

	// Indicadores derivados, no espírito do formato legado, para a camada
	// visual: produção primária (t/dia), empregos, renda relativa, grau de
	// industrialização, estoque de trigo e necessidade diária.
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
	// A4: a mesma passada única do status, reusada pelas consultas.
	out.assign(ws_.cities.size(), 0.0);
	for (const Shipment& shipment : ws_.in_transit) {
		const auto found = ws_.city_lookup.find(shipment.to_city);
		if (found != ws_.city_lookup.end()) {
			out[static_cast<std::size_t>(found->second)] += shipment.amount;
		}
	}
}

void EconomySim::fill_city_status(std::size_t city_index,
		StatusCitySlot& slot, double in_transit, bool heavy) const {
	const City& city = ws_.cities[city_index];
	const StatusKeys& k = status_keys_;
	Dictionary& dict = slot.dict;
	// Taxa de câmbio da nação, resolvida UMA vez por cidade (antes eram 2
	// lookups de moeda por cidade: aqui e no bloco de mercados).
	const double city_fx = fx_rate(city.nation);

	// Strings estáveis: comparam a origem (memcmp) antes de realocar a String.
	// `nation`/`home_nation` entram aqui porque o território pode mudar com a
	// política; id, nome e massa de terra são imutáveis após a inicialização.
	auto sync_string = [&dict](std::string& src, String& stored,
			const std::string& value, const String& key) {
		if (src != value) {
			src = value;
			stored = String(value.c_str());
			dict[key] = stored;
		}
	};
	sync_string(slot.id_src, slot.id, city.id, k.id);
	sync_string(slot.name_src, slot.name, city.name, k.name);
	sync_string(slot.nation_src, slot.nation, city.nation, k.nation);
	sync_string(slot.home_src, slot.home_nation, city.home_nation, k.home_nation);
	sync_string(slot.landmass_src, slot.landmass, city.landmass, k.landmass);
	sync_string(slot.currency_src, slot.currency,
		std::string(currency_code(city.nation)), k.currency);

	// Campos que só mudam quando um dado do território muda (aptidão, demanda
	// externa, posição): reescritos apenas quando a versão estática sobe.
	if (!slot.static_filled || slot.static_version != status_static_version_) {
		dict[k.x] = city.center.x;
		dict[k.y] = city.center.y;
		slot.center.resize(2);
		slot.center[0] = city.center.x;
		slot.center[1] = city.center.y;
		dict[k.center] = slot.center;

		slot.suitability.resize(ws_.products);
		slot.external_demand.resize(ws_.products);
		for (int product = 0; product < ws_.products; ++product) {
			slot.suitability[product] = city.suitability[product];
			slot.external_demand[product] = city.external_demand[product];
		}
		dict[k.suitability] = slot.suitability;
		dict[k.external_demand] = slot.external_demand;
		slot.static_filled = true;
		slot.static_version = status_static_version_;
	}

	// A4: os escalares saem de `city_scalars` (POD) para a ficha e as colunas
	// usarem a MESMA conta. A ordem das escritas e dos espelhos não muda.
	const CityScalars scalars = city_scalars(city_index, in_transit);
	double* scalar_mirror = slot.scalar_written;
	bool& scalar_filled = slot.scalar_written_filled;
	int scalar_index = 0;
	auto sync_scalar = [&dict, &k, scalar_mirror, &scalar_filled, &scalar_index](
			StringName key, double value) {
		if (!scalar_filled || scalar_mirror[scalar_index] != value) {
			dict[key] = value;
			scalar_mirror[scalar_index] = value;
		}
		++scalar_index;
	};
	sync_scalar(k.population, scalars.population);
	sync_scalar(k.simulation_population, scalars.simulation_population);
	sync_scalar(k.gdp, scalars.gdp);
	sync_scalar(k.gdp_per_capita, scalars.gdp_per_capita);
	sync_scalar(k.external_income_index, scalars.external_income_index);
	sync_scalar(k.firm_cash, scalars.firm_cash);
	sync_scalar(k.service_cash, scalars.service_cash);
	sync_scalar(k.government_budget, scalars.government_budget);
	sync_scalar(k.government_goods_spend, scalars.government_goods_spend);
	sync_scalar(k.service_revenue, scalars.service_revenue);
	sync_scalar(k.service_jobs, scalars.service_jobs);
	sync_scalar(k.service_workers, scalars.service_workers);
	sync_scalar(k.service_jobs_filled, scalars.service_jobs_filled);
	sync_scalar(k.in_kind_food, scalars.in_kind_food);
	sync_scalar(k.service_profit, scalars.service_profit);
	sync_scalar(k.opex_revenue, scalars.opex_revenue);
	sync_scalar(k.freight_revenue, scalars.freight_revenue);
	sync_scalar(k.construction_revenue, scalars.construction_revenue);
	sync_scalar(k.service_wage, scalars.service_wage);
	sync_scalar(k.disposable_per_capita, scalars.disposable_per_capita);
	sync_scalar(k.living_standard, scalars.living_standard);
	sync_scalar(k.production, scalars.production);
	sync_scalar(k.jobs, scalars.jobs);
	sync_scalar(k.income, scalars.income);
	sync_scalar(k.industrialization, scalars.industrialization);
	sync_scalar(k.stockpile, scalars.stockpile);
	sync_scalar(k.need, scalars.need);
	sync_scalar(k.deficit, scalars.deficit);
	sync_scalar(k.consumed, scalars.consumed);
	sync_scalar(k.in_transit, scalars.in_transit);
	sync_scalar(k.fx_rate, scalars.fx_rate);
	sync_scalar(k.tariff_revenue, scalars.tariff_revenue);
	sync_scalar(k.productivity_index, scalars.productivity_index);
	slot.scalar_written_filled = true;


	// F9: emprego setorial (4 posições). Espelho de 8 doubles: só reescreve o
	// PackedFloat64Array quando algum valor muda (o custo por escrita é a
	// ponte GDExtension, não o cálculo).
	{
		bool changed = !slot.sector_written_filled
			|| slot.sector_workers.size() != 4;
		if (!changed) {
			for (int i = 0; i < 4; ++i) {
				if (slot.sector_written[i] != city.sector_workers[i]
						|| slot.sector_written[4 + i] != city.sector_share[i]) {
					changed = true;
					break;
				}
			}
		}
		if (changed) {
			slot.sector_workers.resize(4);
			slot.sector_share.resize(4);
			for (int i = 0; i < 4; ++i) {
				slot.sector_workers[i] = city.sector_workers[i];
				slot.sector_share[i] = city.sector_share[i];
				slot.sector_written[i] = city.sector_workers[i];
				slot.sector_written[4 + i] = city.sector_share[i];
			}
			dict[k.sector_workers] = slot.sector_workers;
			dict[k.sector_share] = slot.sector_share;
			slot.sector_written_filled = true;
		}
	}

	// S7-c: o detalhe pesado (deposits/pop/markets/services) só é publicado
	// quando `heavy` — isto é, em get_city_status(id). O status diário publica
	// apenas os escalares leves e o bloco `light`.
	if (heavy) {
	// Reservas minerais declaradas (só produtos com jazida conhecida; ausência
	// da chave = sem dado, não "zero"). O bloco ESTÁTICO (reserva inicial,
	// qualidade, profundidade, eficiência, opex) é escrito uma vez; por dia só
	// mudam reserva restante, extraído e esgotado. Antes o dicionário inteiro
	// era reconstruído a cada dia — custo que crescia com as cidades mineiras
	// do cenário real (o cenário de validação não tem depósito nenhum).
	if (slot.deposit_entry.size() < ws_.products) {
		slot.deposit_entry.resize(static_cast<std::size_t>(ws_.products));
		slot.deposit_present.assign(static_cast<std::size_t>(ws_.products), 0);
		slot.deposit_depleted.assign(static_cast<std::size_t>(ws_.products), 0);
		slot.deposit_written.assign(static_cast<std::size_t>(ws_.products),
			Deposit{});
		slot.deposit_key.clear();
		slot.deposit_key.reserve(static_cast<std::size_t>(ws_.products));
		for (int product = 0; product < ws_.products; ++product) {
			slot.deposit_key.emplace_back(product_name(product));
		}
	}
	// Jazida descoberta depois do primeiro dia (a IA/geografia chama
	// `set_deposit`): a lista de jazidas rastreadas é recalculada uma vez e
	// só as novas entram no dicionário (as antigas mantêm o bloco estático).
	if (slot.deposit_tracked_version != status_deposit_version_) {
		slot.deposit_tracked.clear();
		for (int product = 0; product < ws_.products; ++product) {
			if (city.deposits[product].tracked) {
				slot.deposit_tracked.push_back(product);
			}
		}
		slot.deposit_tracked_version = status_deposit_version_;
	}
	// `deposits` só é remontado quando a cidade tem jazida rastreada. Cidade
	// sem jazida publica o dicionário vazio UMA vez (a chave sempre existe:
	// ausência seria "sem dado", não "sem reserva"); sem isso o bloco custava
	// ~4,5 ms/dia só de referenciar 834 dicionários.
	if (slot.deposit_tracked.empty()) {
		if (!slot.deposits_published) {
			dict[k.deposits] = slot.deposits;
			slot.deposits_published = true;
		}
	} else {
		for (const int product : slot.deposit_tracked) {
			const Deposit& deposit = city.deposits[product];
			Dictionary& entry = slot.deposit_entry[static_cast<std::size_t>(product)];
			Deposit& previous = slot.deposit_written[static_cast<std::size_t>(product)];
			const bool first = slot.deposit_present[static_cast<std::size_t>(product)] == 0;
			if (first) {
				entry[k.initial_kt] = deposit.initial_kt;
				entry[k.quality] = deposit.quality;
				entry[k.depth_m] = deposit.depth_m;
				entry[k.efficiency] = extraction_efficiency(deposit);
				entry[k.opex_factor] = depth_opex_factor(deposit);
				slot.deposits[slot.deposit_key[static_cast<std::size_t>(product)]] = entry;
				slot.deposit_present[static_cast<std::size_t>(product)] = 1;
			}
			if (first || previous.remaining_kt != deposit.remaining_kt) {
				entry[k.remaining_kt] = deposit.remaining_kt;
				previous.remaining_kt = deposit.remaining_kt;
			}
			if (first || previous.extracted_kt != deposit.extracted_kt) {
				entry[k.extracted_kt] = deposit.extracted_kt;
				previous.extracted_kt = deposit.extracted_kt;
			}
			// D-086 (decisão do dono): o recurso nunca acaba — publica `depleted`
			// SEMPRE false (campo mantido por contrato de status/save).
			if (first || slot.deposit_depleted[static_cast<std::size_t>(product)] != 0) {
				entry[k.depleted] = false;
				slot.deposit_depleted[static_cast<std::size_t>(product)] = 0;
			}
		}
		// Dictionary é por referência no Godot 4: uma única atribuição basta —
		// as entradas novas e os valores atualizados aparecem no publicado.
		// (Reatribuir todo dia custava ~1,4 µs × 662 cidades ≈ 0,9 ms.)
		if (!slot.deposits_published) {
			dict[k.deposits] = slot.deposits;
			slot.deposits_published = true;
		}
	}

	Dictionary& pop = slot.pop;
	// Valores diários: só reescreve o que mudou. Cada `pop[chave] = valor` é
	// uma chamada à ponte GDExtension com hash da chave (~190 ns); com 20
	// campos × 834 cidades isso custava ~3 ms/dia. `population`,
	// `labor_force` e `dependents` quase nunca mudam.
	{
		double* mirror = slot.pop_written;
		bool& filled = slot.pop_written_filled;
		int index = 0;
		auto sync_pop = [&pop, &k, mirror, &filled, &index](StringName key, double value) {
			if (!filled || mirror[index] != value) {
				pop[key] = value;
				mirror[index] = value;
			}
			++index;
		};
		sync_pop(k.population, city.pop.population);
		sync_pop(k.labor_force, city.pop.labor_force);
		sync_pop(k.employed, city.pop.employed);
		sync_pop(k.unemployed, city.pop.unemployed);
		sync_pop(k.dependents, city.pop.dependents);
		sync_pop(k.wage_income, city.pop.wage_income);
		sync_pop(k.capital_income, city.pop.capital_income);
		sync_pop(k.transfers, city.pop.transfers);
		sync_pop(k.taxes, city.pop.taxes);
		sync_pop(k.service_spend, city.pop.service_spend);
		sync_pop(k.disposable, city.pop.disposable);
		sync_pop(k.external_income, city.pop.external_income);
		sync_pop(k.food_spend, city.pop.food_spend);
		sync_pop(k.consumed, city.pop.consumed);
		sync_pop(k.need, city.pop.need);
		sync_pop(k.satisfaction, city.pop.satisfaction);
		sync_pop(k.savings, city.pop.savings);
		sync_pop(k.needs_level, double(city.pop.needs_level));
		sync_pop(k.needs_satisfaction, city.pop.needs_satisfaction);
		sync_pop(k.basic_satisfaction, city.pop.level_satisfaction.size() > 0
			? city.pop.level_satisfaction[0] : 0.0);
		sync_pop(k.life_satisfaction, city.pop.level_satisfaction.size() > 1
			? city.pop.level_satisfaction[1] : 0.0);
		sync_pop(k.luxury_satisfaction, city.pop.level_satisfaction.size() > 2
			? city.pop.level_satisfaction[2] : 0.0);
		sync_pop(k.needs_spend, city.pop.needs_spend);
		sync_pop(k.durable_spend, city.pop.durable_spend);
		double durable_total = 0.0;
		for (const double tons : city.pop.durable_stock) {
			durable_total += tons;
		}
		sync_pop(k.durable_stock, durable_total);
		// F7: população ativa, fora da força e mercado de trabalho.
		sync_pop(k.working_age, city.pop.working_age);
		sync_pop(k.retirees, city.pop.retirees);
		sync_pop(k.students, city.pop.students);
		sync_pop(k.recruit_reserve, city.pop.recruit_reserve);
		sync_pop(k.income_level, double(city.pop.income_level));
		const auto minimum_wage = ws_.minimum_wages.find(city.nation);
		sync_pop(k.minimum_wage, minimum_wage != ws_.minimum_wages.end()
			? std::max(0.0, minimum_wage->second) : 0.0);
		sync_pop(k.wage_rigidity, ws_.wage_rigidity);
		if (!filled) {
			slot.education.resize(kEducationLevels);
			slot.education_share.resize(kEducationLevels);
			slot.labor_force_by_level.resize(kEducationLevels);
			slot.employed_by_level.resize(kEducationLevels);
			slot.unemployed_by_level.resize(kEducationLevels);
			slot.unemployment_by_level.resize(kEducationLevels);
			slot.vacancies_by_level.resize(kEducationLevels);
			slot.wage_by_level.resize(kEducationLevels);
			slot.wage_income_by_level.resize(kEducationLevels);
			slot.studying.resize(kEducationLevels - 1);
			slot.income_level_share.resize(kIncomeLevels);
			slot.income_level_by_stratum.resize(kEducationLevels);
			slot.stratum_disposable.resize(kEducationLevels);
			slot.stratum_population.resize(kEducationLevels);
			slot.stratum_wage_income.resize(kEducationLevels);
		}
		// Grupo "raro": educação, força por nível, estudo, fatias de renda e o
		// nível de consumo de cada estrato. Compara com o último publicado e só
		// toca nos Arrays quando algo muda (mudam em escala de semanas).
		{
			double rare[16];
			int ri = 0;
			for (int level = 0; level < kEducationLevels; ++level) {
				rare[ri++] = city.pop.education[level];
			}
			for (int level = 0; level < kEducationLevels; ++level) {
				rare[ri++] = city.pop.labor_force_by_level[level];
			}
			for (int track = 0; track < kEducationLevels - 1; ++track) {
				rare[ri++] = city.pop.studying[track];
			}
			for (int level = 0; level < kIncomeLevels; ++level) {
				rare[ri++] = city.pop.income_level_share[level];
			}
			for (int stratum = 0; stratum < kEducationLevels; ++stratum) {
				rare[ri++] = double(city.pop.stratum_income_level[stratum]);
			}
			if (!slot.labor_rare_filled
					|| std::memcmp(rare, slot.labor_rare_written,
						sizeof(double) * static_cast<std::size_t>(ri)) != 0) {
				for (int level = 0; level < kEducationLevels; ++level) {
					slot.education[level] = city.pop.education[level];
					slot.labor_force_by_level[level] =
						city.pop.labor_force_by_level[level];
				}
				for (int track = 0; track < kEducationLevels - 1; ++track) {
					slot.studying[track] = city.pop.studying[track];
				}
				for (int level = 0; level < kIncomeLevels; ++level) {
					slot.income_level_share[level] =
						city.pop.income_level_share[level];
				}
				for (int stratum = 0; stratum < kEducationLevels; ++stratum) {
					slot.income_level_by_stratum[stratum] =
						city.pop.stratum_income_level[stratum];
				}
				std::memcpy(slot.labor_rare_written, rare,
					sizeof(double) * static_cast<std::size_t>(ri));
				slot.labor_rare_filled = true;
			}
		}
		// Grupo "diário": salários, emprego, vagas e folha por escolaridade.
		{
			double daily[24];
			int di = 0;
			for (int level = 0; level < kEducationLevels; ++level) {
				daily[di++] = city.pop.working_age > 1e-9
					? city.pop.education[level] / city.pop.working_age : 0.0;
				daily[di++] = city.pop.employed_by_level[level];
				daily[di++] = city.pop.unemployed_by_level[level];
				daily[di++] = city.pop.labor_force_by_level[level] > 1e-9
					? city.pop.unemployed_by_level[level]
						/ city.pop.labor_force_by_level[level] : 0.0;
				daily[di++] = city.pop.vacancies_by_level[level];
				daily[di++] = city.pop.wage_by_level[level];
				daily[di++] = city.pop.wage_income_by_level[level];
			}
			if (!slot.labor_daily_filled
					|| std::memcmp(daily, slot.labor_daily_written,
						sizeof(double) * static_cast<std::size_t>(di)) != 0) {
				for (int level = 0; level < kEducationLevels; ++level) {
					slot.education_share[level] = city.pop.working_age > 1e-9
						? city.pop.education[level] / city.pop.working_age : 0.0;
					slot.employed_by_level[level] =
						city.pop.employed_by_level[level];
					slot.unemployed_by_level[level] =
						city.pop.unemployed_by_level[level];
					slot.unemployment_by_level[level] =
						city.pop.labor_force_by_level[level] > 1e-9
						? city.pop.unemployed_by_level[level]
							/ city.pop.labor_force_by_level[level] : 0.0;
					slot.vacancies_by_level[level] =
						city.pop.vacancies_by_level[level];
					slot.wage_by_level[level] = city.pop.wage_by_level[level];
					slot.wage_income_by_level[level] =
						city.pop.wage_income_by_level[level];
				}
				std::memcpy(slot.labor_daily_written, daily,
					sizeof(double) * static_cast<std::size_t>(di));
				slot.labor_daily_filled = true;
			}
		}
		// Renda por estrato: o memcmp evita reescrever os três vetores quando
		// os valores não mudaram (população e folha do estrato variam devagar).
		{
			double strata[12];
			int si = 0;
			for (int stratum = 0; stratum < kEducationLevels; ++stratum) {
				strata[si++] = city.pop.stratum_disposable[stratum];
				strata[si++] = city.pop.stratum_population[stratum];
				strata[si++] = city.pop.stratum_wage_income[stratum];
			}
			if (!slot.stratum_written_filled
					|| std::memcmp(strata, slot.stratum_written,
						sizeof(double) * static_cast<std::size_t>(si)) != 0) {
				for (int stratum = 0; stratum < kEducationLevels; ++stratum) {
					slot.stratum_disposable[stratum] =
						city.pop.stratum_disposable[stratum];
					slot.stratum_population[stratum] =
						city.pop.stratum_population[stratum];
					slot.stratum_wage_income[stratum] =
						city.pop.stratum_wage_income[stratum];
				}
				std::memcpy(slot.stratum_written, strata,
					sizeof(double) * static_cast<std::size_t>(si));
				slot.stratum_written_filled = true;
			}
		}
		if (!filled) {
			pop[k.income_level_share] = slot.income_level_share;
			pop[k.income_level_by_stratum] = slot.income_level_by_stratum;
			pop[k.stratum_disposable] = slot.stratum_disposable;
			pop[k.stratum_population] = slot.stratum_population;
			pop[k.stratum_wage_income] = slot.stratum_wage_income;
			pop[k.education] = slot.education;
			pop[k.education_share] = slot.education_share;
			pop[k.labor_force_by_level] = slot.labor_force_by_level;
			pop[k.employed_by_level] = slot.employed_by_level;
			pop[k.unemployed_by_level] = slot.unemployed_by_level;
			pop[k.unemployment_by_level] = slot.unemployment_by_level;
			pop[k.vacancies_by_level] = slot.vacancies_by_level;
			pop[k.wage_by_level] = slot.wage_by_level;
			pop[k.wage_income_by_level] = slot.wage_income_by_level;
			pop[k.studying] = slot.studying;
		}
		if (!filled) {
			slot.desired_by_category.resize(ws_.food_products.size());
			slot.consumed_by_category.resize(ws_.food_products.size());
		}
		for (int food = 0; food < static_cast<int>(ws_.food_products.size()); ++food) {
			slot.desired_by_category[food] = city.pop.desired_by_category[food];
			slot.consumed_by_category[food] = city.pop.consumed_by_category[food];
		}
		if (!filled) {
			pop[k.desired_by_category] = slot.desired_by_category;
			pop[k.consumed_by_category] = slot.consumed_by_category;
		} else {
			bool changed = false;
			for (int food = 0; food < static_cast<int>(ws_.food_products.size()); ++food) {
				if (slot.pop_food_written[food] != city.pop.desired_by_category[food]
						|| slot.pop_food_written[ws_.food_products.size() + food]
							!= city.pop.consumed_by_category[food]) {
					changed = true;
					break;
				}
			}
			if (changed) {
				pop[k.desired_by_category] = slot.desired_by_category;
				pop[k.consumed_by_category] = slot.consumed_by_category;
			}
		}
		for (int food = 0; food < static_cast<int>(ws_.food_products.size()); ++food) {
			slot.pop_food_written[food] = city.pop.desired_by_category[food];
			slot.pop_food_written[ws_.food_products.size() + food] =
				city.pop.consumed_by_category[food];
		}
		filled = true;
	}
	if (!slot.pop_published) {
		dict[k.pop] = pop;
		slot.pop_published = true;
	}

	// B7: informalidade, mortalidade e insatisfação (blocos pesados, sob
	// demanda em get_city_status). Construídos a cada chamada — o caminho é
	// raro (uma cidade por consulta), então não há cache/mirror.
	{
		Dictionary informal;
		informal["share"] = city.pop.informal_share;
		Array by_quintile;
		by_quintile.resize(kIncomeStrata);
		for (int s = 0; s < kIncomeStrata; ++s) {
			by_quintile[s] = city.pop.informal_labor_share[s];
		}
		informal["by_quintile"] = by_quintile;
		informal["wage_bill"] = city.pop.informal_wage_bill;
		Dictionary service;
		service["workers"] = city.pop.informal_service_workers;
		service["output"] = city.pop.informal_service_output;
		service["demand"] = city.pop.informal_service_demand;
		service["revenue"] = city.pop.informal_service_revenue;
		service["price"] = city.pop.informal_service_price;
		service["share"] = city.pop.informal_service_share;
		informal["service"] = service;
		dict["informal"] = informal;

		Dictionary mortality;
		mortality["deaths"] = city.pop.deaths_total;
		mortality["expected_deaths"] = city.pop.deaths_expected;
		mortality["excess_total"] = city.pop.excess_deaths_total;
		mortality["excess_famine_total"] = city.pop.excess_deaths_famine_total;
		mortality["famine_months"] = city.pop.famine_months;
		mortality["health_access"] = sim_core_.nation_health_access(city);
		mortality["health_access_ref"] = city.pop.health_access_ref;
		dict["mortality"] = mortality;

		Dictionary dissatisfaction;
		dissatisfaction["index"] = city.pop.dissatisfaction;
		Array dq;
		dq.resize(kIncomeStrata);
		for (int s = 0; s < kIncomeStrata; ++s) {
			dq[s] = city.pop.dissatisfaction_by_stratum[s];
		}
		dissatisfaction["by_quintile"] = dq;
		Dictionary components;
		components["unemployment"] = city.pop.dissatisfaction_unemployment;
		components["rent"] = city.pop.dissatisfaction_rent;
		components["food"] = city.pop.dissatisfaction_food;
		components["inflation"] = city.pop.dissatisfaction_inflation;
		components["informality"] = city.pop.dissatisfaction_informality;
		components["land"] = 0.0;
		dissatisfaction["components"] = components;
		dict["dissatisfaction"] = dissatisfaction;
	}

	// B1: coortes etárias (5 faixas). Publicadas como Array de doubles no bloco
	// `pop`, reescritas só quando a composição muda (uma vez por mês).
	{
		double bands[kAgeBands];
		for (int band = 0; band < kAgeBands; ++band) {
			bands[band] = city.pop.demography_ready
				? std::max(0.0, city.pop.age_band[band]) : 0.0;
		}
		if (!slot.age_bands_filled
				|| std::memcmp(bands, slot.age_bands_written,
					sizeof(double) * static_cast<std::size_t>(kAgeBands)) != 0) {
			slot.age_bands.assign(bands, bands + kAgeBands);
			Array published;
			published.resize(kAgeBands);
			for (int band = 0; band < kAgeBands; ++band) {
				published[band] = slot.age_bands[static_cast<std::size_t>(band)];
			}
			pop[k.age_bands] = published;
			std::memcpy(slot.age_bands_written, bands,
				sizeof(double) * static_cast<std::size_t>(kAgeBands));
			slot.age_bands_filled = true;
		}
	}

	// B2: cinco estratos de renda no bloco `pop`. Publica população, renda
	// disponível, poupança, dívida, patrimônio, taxa de poupança, posse (3),
	// nível de consumo, aluguel pago e a fatia da renda gasta em comida — os
	// números que a validação de padrão real lê. Republicado quando muda.
	{
		double flat[kIncomeStrata * 14 + 7];
		int fi = 0;
		const auto put = [&flat, &fi](double value) {
			flat[fi++] = value;
		};
		for (int s = 0; s < kIncomeStrata; ++s) put(city.pop.strata_income_share[s]);
		for (int s = 0; s < kIncomeStrata; ++s) put(city.pop.stratum_people[s]);
		for (int s = 0; s < kIncomeStrata; ++s) put(city.pop.stratum_net_income[s]);
		for (int s = 0; s < kIncomeStrata; ++s) put(city.pop.stratum_savings[s]);
		for (int s = 0; s < kIncomeStrata; ++s) put(city.pop.stratum_debt[s]);
		for (int s = 0; s < kIncomeStrata; ++s) put(city.pop.stratum_wealth[s]);
		for (int s = 0; s < kIncomeStrata; ++s) put(city.pop.stratum_saving_rate[s]);
		for (int s = 0; s < kIncomeStrata; ++s) put(double(city.pop.stratum_level[s]));
		for (int s = 0; s < kIncomeStrata; ++s) put(city.pop.stratum_rent_paid[s]);
		for (int s = 0; s < kIncomeStrata; ++s) put(city.pop.stratum_rent_bill[s]);
		for (int s = 0; s < kIncomeStrata; ++s) {
			put(city.pop.stratum_net_income[s] > 1e-9
				? city.pop.stratum_food_spend[s] / city.pop.stratum_net_income[s]
				: 0.0);
		}
		for (int s = 0; s < kIncomeStrata; ++s) {
			put(city.pop.stratum_tenure[s][0]);
		}
		for (int s = 0; s < kIncomeStrata; ++s) {
			put(city.pop.stratum_tenure[s][1]);
		}
		for (int s = 0; s < kIncomeStrata; ++s) {
			put(city.pop.stratum_tenure[s][2]);
		}
		put(sim_core_.strata_gini(city.pop));
		// B6: Gini de RIQUEZA dos quintis (poupança + imóvel + terra +
		// participações; no urbano a terra é 0 — ela é da região).
		put(ogs::SimCore::quintile_gini(city.pop.stratum_wealth, kIncomeStrata));
		for (int band = 0; band < 5; ++band) {
			put(city.pop.lifecycle_saving[band]);
		}
		if (!slot.income_strata_filled
				|| std::memcmp(flat, slot.income_strata_written.data(),
					sizeof(double) * static_cast<std::size_t>(fi)) != 0) {
			slot.income_strata_written.assign(flat, flat + fi);
			Dictionary out;
			const char* const names[14] = {"income_share", "population", "income", "savings",
				"debt", "wealth", "saving_rate", "level", "rent_paid", "rent_bill",
				"food_income_share", "tenure_own", "tenure_rent",
				"tenure_informal"};
			for (int field = 0; field < 14; ++field) {
				Array row;
				row.resize(kIncomeStrata);
				for (int s = 0; s < kIncomeStrata; ++s) {
					row[s] = flat[field * kIncomeStrata + s];
				}
				out[String(names[field])] = row;
			}
			out["gini"] = flat[kIncomeStrata * 14];
			out["wealth_gini"] = flat[kIncomeStrata * 14 + 1];
			Array lifecycle;
			lifecycle.resize(5);
			for (int band = 0; band < 5; ++band) {
				lifecycle[band] = flat[kIncomeStrata * 14 + 2 + band];
			}
			out["lifecycle_saving"] = lifecycle;
			slot.income_strata = out;
			pop[k.income_strata] = slot.income_strata;
			slot.income_strata_filled = true;
		}
	}

	// `resize` com o mesmo tamanho já provoca copy-on-write no Array
	// compartilhado com o dicionário publicado (medido: ~4,5 ms/dia, 114
	// Variants copiados por cidade). Só redimensiona quando muda de fato.
	if (slot.markets.size() != ws_.products) {
		slot.markets.resize(ws_.products);
	}
	if (slot.market_entry.size() < ws_.products) {
		slot.market_entry.resize(static_cast<std::size_t>(ws_.products));
		slot.market_written.resize(static_cast<std::size_t>(ws_.products));
		slot.market_written_filled.assign(static_cast<std::size_t>(ws_.products), 0);
		slot.market_local_written.assign(static_cast<std::size_t>(ws_.products), 0.0);
		slot.io_written.assign(static_cast<std::size_t>(4 * ws_.products), 0.0);
		slot.io_filled.assign(4, 0);
	}
	// Preço local é EXIBIÇÃO: âncora ÷ taxa da moeda da nação (F5, opção c).
	// Não volta para a simulação; o valor na âncora é a fonte de verdade.
	const double local_rate = std::max(1e-9, city_fx);
	bool markets_dirty = false;
	for (int product = 0; product < ws_.products; ++product) {
		const Market& source = city.markets[product];
		Market& previous = slot.market_written[static_cast<std::size_t>(product)];
		const double local = source.price / local_rate;
		if (slot.market_written_filled[static_cast<std::size_t>(product)] == 0) {
			// Primeira gravação desta cidade: cria as chaves do mercado.
			Dictionary& market = slot.market_entry[static_cast<std::size_t>(product)];
			market[k.product] = product;
			market[k.stock] = source.stock;
			market[k.price] = source.price;
			market[k.price_local] = local;
			market[k.demand] = source.demand;
			market[k.supply] = source.supply;
			market[k.desired] = source.desired;
			market[k.sold] = source.sold;
			market[k.shortage] = source.shortage;
			market[k.surplus] = source.surplus;
			previous = source;
			slot.market_local_written[static_cast<std::size_t>(product)] = local;
			slot.market_written_filled[static_cast<std::size_t>(product)] = 1;
			slot.markets[product] = market;
			markets_dirty = true;
			continue;
		}
		// Caminho comum (mercado publicado igual ao dia anterior): NENHUM
		// acesso ao Dictionary. A comparação cobre os 8 campos publicados, que
		// são contíguos (ver Market): um memcmp de 64 bytes por produto.
		// `sold_prev` é interno e muda TODO dia (`sold_prev = sold` no reset);
		// por isso fica no fim do struct, fora do bloco comparado.
		const bool local_changed = slot.market_local_written[
			static_cast<std::size_t>(product)] != local;
		if (!local_changed && std::memcmp(&previous, &source, sizeof(double) * 8) == 0) {
			continue;
		}
		markets_dirty = true;
		Dictionary& market = slot.market_entry[static_cast<std::size_t>(product)];
		if (local_changed) {
			market[k.price_local] = local;
			slot.market_local_written[static_cast<std::size_t>(product)] = local;
		}
		if (previous.stock != source.stock) {
			market[k.stock] = source.stock;
			previous.stock = source.stock;
		}
		if (previous.price != source.price) {
			market[k.price] = source.price;
			previous.price = source.price;
		}
		if (previous.demand != source.demand) {
			market[k.demand] = source.demand;
			previous.demand = source.demand;
		}
		if (previous.supply != source.supply) {
			market[k.supply] = source.supply;
			previous.supply = source.supply;
		}
		if (previous.desired != source.desired) {
			market[k.desired] = source.desired;
			previous.desired = source.desired;
		}
		if (previous.sold != source.sold) {
			market[k.sold] = source.sold;
			previous.sold = source.sold;
		}
		if (previous.shortage != source.shortage) {
			market[k.shortage] = source.shortage;
			previous.shortage = source.shortage;
		}
		if (previous.surplus != source.surplus) {
			market[k.surplus] = source.surplus;
			previous.surplus = source.surplus;
		}
	}
	// Sem nenhuma mudança o Array publicado continua o mesmo objeto: reatribuir
	// só custaria uma chamada à ponte GDExtension por cidade (~0,3 ms/dia).
	if (markets_dirty) {
		dict[k.markets] = slot.markets;
	}

	// F8: bloco plano de serviços (20 × 12). Só reescreve quando algum valor
	// muda; o Array publicado é reutilizado pela UI (ordem de service_sectors).
	if (ws_.services_loaded) {
		PackedFloat64Array& services = slot.services;
		services.resize(kMaxServiceSectors * kServiceFields);
		double* out = services.ptrw();
		for (int sector = 0; sector < kMaxServiceSectors; ++sector) {
			const ServiceMarket& market = city.service_markets[sector];
			double* base = out + sector * kServiceFields;
			base[0] = market.demand;
			base[1] = market.supply;
			base[2] = market.price;
			base[3] = market.revenue;
			base[4] = market.jobs;
			base[5] = market.businesses;
			base[6] = market.unmet;
			base[7] = market.exports;
			base[8] = market.export_revenue;
			base[9] = market.coverage;
			base[10] = market.input_cost;
			base[11] = market.service_cost;
		}
		bool dirty = !slot.services_filled;
		if (!dirty) {
			const double* previous = slot.services_written.ptr();
			for (int index = 0; index < services.size(); ++index) {
				if (previous[index] != out[index]) {
					dirty = true;
					break;
				}
			}
		}
		if (dirty) {
			slot.services_written = services;
			dict[k.services] = services;
			slot.services_filled = true;
		}
	}
	}  // fim do bloco pesado (deposits/pop/markets/services)

	// `Array` é tipo por referência no Godot 4 (validado em teste): mutar
	// `slot.imported` já aparece no dicionário publicado. Redimensionar para o
	// mesmo tamanho era uma chamada à ponte por array/cidade/dia sem efeito.
	if (slot.io_written.size() < static_cast<std::size_t>(4 * ws_.products)) {
		slot.io_written.assign(static_cast<std::size_t>(4 * ws_.products), 0.0);
		slot.io_filled.assign(4, 0);
	}
	if (slot.imported.size() != ws_.products) {
		slot.imported.resize(ws_.products);
		slot.exported.resize(ws_.products);
		slot.import_value.resize(ws_.products);
		slot.export_value.resize(ws_.products);
	}
	{
		// Quatro arrays de fluxo, um por produto (ws_.products): idem
		// mercados, só produtos com comércio mudam (o resto fica em 0 dia após
		// dia) e a linha inteira é comparada com um memcmp antes do hash.
		const double* source_values[4] = {
			city.imported, city.exported, city.import_value, city.export_value};
		Array* targets[4] = {
			&slot.imported, &slot.exported, &slot.import_value, &slot.export_value};
		const StringName* keys[4] = {&k.imported, &k.exported, &k.import_value, &k.export_value};
		for (int array_index = 0; array_index < 4; ++array_index) {
			Array& target = *targets[array_index];
			double* previous = &slot.io_written[static_cast<std::size_t>(
				array_index * ws_.products)];
			const bool first = slot.io_filled[static_cast<std::size_t>(array_index)] == 0;
			// Linha inteira igual ao status anterior: nenhum hash a pagar.
			if (!first && std::memcmp(previous, source_values[array_index],
					sizeof(double) * static_cast<std::size_t>(ws_.products)) == 0) {
				continue;
			}
			for (int product = 0; product < ws_.products; ++product) {
				const double value = source_values[array_index][product];
				if (first || previous[product] != value) {
					target[product] = value;
					previous[product] = value;
				}
			}
			if (first) {
				dict[*keys[array_index]] = target;
				slot.io_filled[static_cast<std::size_t>(array_index)] = 1;
			}
		}
	}

	// S7-c: construções também são detalhe pesado sob demanda.
	if (heavy) {
	const std::vector<int>& local_buildings = ws_.buildings_by_city[city_index];
	if (slot.buildings.size() != local_buildings.size()) {
		slot.buildings.resize(local_buildings.size());
		slot.buildings_bound = false;
	}
	if (slot.building_entry.size() < local_buildings.size()) {
		slot.building_entry.resize(local_buildings.size());
		slot.building_cache.resize(local_buildings.size());
	}
	for (std::size_t index = 0; index < local_buildings.size(); ++index) {
		const Building& building =
			ws_.buildings[static_cast<std::size_t>(local_buildings[index])];
		Dictionary& entry = slot.building_entry[index];
		StatusCitySlot::BuildingCache& cached = slot.building_cache[index];
		const bool identity_changed = !cached.filled || cached.id_src != building.id;
		// F9: privatização/nacionalização muda o dono sem mudar a identidade.
		const bool owner_changed = cached.owner_src != building.owner;
		// Bloco estático: só quando a construção nasce (ou muda de identidade).
		if (identity_changed || owner_changed) {
			cached.id_src = building.id;
			cached.id = String(building.id.c_str());
			entry[k.id] = cached.id;
			entry[k.kind] = building.kind;
			cached.resource_src = building.resource;
			cached.resource = String(building.resource.c_str());
			entry[k.resource] = cached.resource;
			cached.owner_src = building.owner;
			cached.owner = String(building.owner.c_str());
			entry[k.owner] = cached.owner;
			// F9: decomposição do dono para a UI/status (sem parse no cliente).
			std::string group_id;
			std::string origin_nation;
			const int owner_code = owner_kind(building.owner, &group_id,
				&origin_nation);
			entry[k.owner_kind] = owner_code == 0 ? String("state")
				: owner_code == 1 ? String("national")
				: owner_code == 2 ? String("foreign") : String("private");
			entry[k.owner_group] = String(group_id.c_str());
			entry[k.owner_nation] = origin_nation.empty()
				? String() : String(origin_nation.c_str());
			entry[k.local_share] = building.local_share;
			entry[k.input_product] = building.input_product;
			entry[k.output_product] = building.output_product;
			entry[k.capital_per_capacity] = building.capital_per_capacity;
			entry[k.strategic] = building.strategic;
			cached.recipe = -2;
			cached.filled = true;
		}
		// Reinsere o `entry` no Array publicado só quando ele nasce/muda de
		// índice; mutações do Dictionary já são visíveis pela referência.
		if (!slot.buildings_bound || identity_changed) {
			slot.buildings[index] = entry;
		}
		// Receita (insumos/subprodutos): as Arrays/Dictionaries só são
		// reconstruídos quando a receita da construção muda — nunca por dia.
		if (cached.recipe != building.recipe) {
			cached.recipe = building.recipe;
			if (building.recipe >= 0
					&& building.recipe < static_cast<int>(ws_.recipes.size())) {
				const Recipe& recipe =
					ws_.recipes[static_cast<std::size_t>(building.recipe)];
				entry[k.recipe] = String(recipe.id.c_str());
				Array inputs;
				Array byproducts;
				inputs.resize(recipe.input_count);
				byproducts.resize(recipe.byproduct_count);
				for (int slot = 0; slot < recipe.input_count; ++slot) {
					Dictionary input;
					input[k.good] = String(product_name(recipe.inputs[slot]));
					input[k.qty] = recipe.input_qty[slot];
					inputs[slot] = input;
				}
				for (int slot = 0; slot < recipe.byproduct_count; ++slot) {
					Dictionary byproduct;
					byproduct[k.good] = String(product_name(recipe.byproducts[slot]));
					byproduct[k.qty] = recipe.byproduct_qty[slot];
					byproducts[slot] = byproduct;
				}
				entry[k.inputs] = inputs;
				entry[k.byproducts] = byproducts;
				entry[k.yield] = recipe.yield;
			} else {
				entry[k.recipe] = String();
				entry[k.inputs] = Array();
				entry[k.byproducts] = Array();
			}
		}
		// Valores diários: só reescreve o que mudou (comparar double é ~20×
		// mais barato que um Dictionary::operator[] com hash de StringName).
		auto sync_double = [&entry, &cached](StringName key, double& mirror,
				double value) {
			if (mirror != value) {
				entry[key] = value;
				mirror = value;
			}
		};
		sync_double(k.last_utilization, cached.last_utilization, building.last_utilization);
		sync_double(k.capacity, cached.capacity, building.capacity);
		sync_double(k.jobs, cached.jobs, building.jobs);
		sync_double(k.wage, cached.wage, building.wage);
		sync_double(k.opex, cached.opex, building.opex);
		sync_double(k.workers, cached.workers, building.workers);
		sync_double(k.utilization, cached.utilization, building.utilization);
		sync_double(k.output_scale, cached.output_scale, building.output_scale);
		if (cached.loss_days != static_cast<double>(building.loss_days)) {
			entry[k.loss_days] = building.loss_days;
			cached.loss_days = static_cast<double>(building.loss_days);
		}
		sync_double(k.revenue, cached.revenue, building.revenue);
		sync_double(k.input_value, cached.input_value, building.input_value);
		sync_double(k.costs, cached.costs, building.costs);
		sync_double(k.profit, cached.profit, building.profit);
		sync_double(k.site_suitability, cached.site,
			city.suitability[building.output_product]);
		// D-086 — estágio de mina: profundidade de operação, produção relativa do
		// estágio (0,05..1,0) e custo do PRÓXIMO aprofundamento. Fazenda/indústria
		// publicam estágio 0 / fator 1,0 (não têm estágio) — ausência explícita.
		{
			const bool mine = SimCore::is_mine_building(city, building);
			const int stage = mine ? building.stage : 0;
			if (cached.stage != stage) {
				entry[k.stage] = stage;
				cached.stage = stage;
			}
			sync_double(k.stage_factor, cached.stage_factor,
				SimCore::mine_stage_factor(city, building));
			sync_double(k.stage_reserve_kt, cached.stage_reserve_kt,
				mine ? building.stage_reserve_kt : 0.0);
			sync_double(k.stage_mined_kt, cached.stage_mined_kt,
				mine ? building.stage_mined_kt : 0.0);
			// D-086 (decisão do dono): `can_deepen` só exige a mina não falida — a
			// reserva NUNCA bloqueia o aprofundamento (a jazida é referência de
			// profundidade, não um limite físico).
			const bool can_deepen = mine && !building.bankrupt;
			sync_double(k.next_deepen_cost, cached.next_deepen_cost,
				can_deepen ? sim_core_.mine_deepen_cost(city, building) : 0.0);
		}
	}
	if (!slot.buildings_published) {
		dict[k.buildings] = slot.buildings;
		slot.buildings_published = true;
	}
	slot.buildings_bound = true;
	}  // fim do bloco pesado (buildings)
}

void EconomySim::fill_light_signals(StatusCache& cache) const {
	const StatusKeys& k = status_keys_;
	const std::size_t cities = ws_.cities.size();
	const std::size_t products = static_cast<std::size_t>(ws_.products);
	const std::size_t market_len = cities * products;

	// Mercados: 7 campos que a IA lê (preço âncora, estoque, oferta, excedente,
	// vendido, escassez, desejado). São os MESMOS doubles do bloco pesado por
	// cidade (`source.price` etc. em fill_city_status) — bit a bit.
	cache.light_m_price.resize(static_cast<int>(market_len));
	cache.light_m_stock.resize(static_cast<int>(market_len));
	cache.light_m_supply.resize(static_cast<int>(market_len));
	cache.light_m_surplus.resize(static_cast<int>(market_len));
	cache.light_m_sold.resize(static_cast<int>(market_len));
	cache.light_m_shortage.resize(static_cast<int>(market_len));
	cache.light_m_desired.resize(static_cast<int>(market_len));
	double* lm_price = cache.light_m_price.ptrw();
	double* lm_stock = cache.light_m_stock.ptrw();
	double* lm_supply = cache.light_m_supply.ptrw();
	double* lm_surplus = cache.light_m_surplus.ptrw();
	double* lm_sold = cache.light_m_sold.ptrw();
	double* lm_shortage = cache.light_m_shortage.ptrw();
	double* lm_desired = cache.light_m_desired.ptrw();

	// POP: 5 escalares que a IA lê por cidade (necessidade alimentar, consumo,
	// satisfação alimentar, força de trabalho e desemprego).
	cache.light_p_need.resize(static_cast<int>(cities));
	cache.light_p_consumed.resize(static_cast<int>(cities));
	cache.light_p_satisfaction.resize(static_cast<int>(cities));
	cache.light_p_labor_force.resize(static_cast<int>(cities));
	cache.light_p_unemployed.resize(static_cast<int>(cities));
	double* lp_need = cache.light_p_need.ptrw();
	double* lp_consumed = cache.light_p_consumed.ptrw();
	double* lp_satisfaction = cache.light_p_satisfaction.ptrw();
	double* lp_labor_force = cache.light_p_labor_force.ptrw();
	double* lp_unemployed = cache.light_p_unemployed.ptrw();

	for (std::size_t city_index = 0; city_index < cities; ++city_index) {
		const City& city = ws_.cities[city_index];
		const std::size_t base = city_index * products;
		for (int product = 0; product < ws_.products; ++product) {
			const Market& market = city.markets[product];
			const std::size_t at = base + static_cast<std::size_t>(product);
			lm_price[at] = market.price;
			lm_stock[at] = market.stock;
			lm_supply[at] = market.supply;
			lm_surplus[at] = market.surplus;
			lm_sold[at] = market.sold;
			lm_shortage[at] = market.shortage;
			lm_desired[at] = market.desired;
		}
		lp_need[city_index] = city.pop.need;
		lp_consumed[city_index] = city.pop.consumed;
		lp_satisfaction[city_index] = city.pop.satisfaction;
		lp_labor_force[city_index] = city.pop.labor_force;
		lp_unemployed[city_index] = city.pop.unemployed;
	}

	// Incumbentes (F9): pares (cidade, recurso) com dono-grupo — o que o
	// group_investor montava varrendo city["buildings"] de TODAS as cidades.
	// Mesmo filtro do GDScript: owner_group e resource não-vazios. Ordem:
	// cidade → construção (buildings_by_city), idêntica ao Array do bloco pesado.
	cache.light_i_city.clear();
	cache.light_i_resource.clear();
	cache.light_i_owner_group.clear();
	for (std::size_t city_index = 0; city_index < cities; ++city_index) {
		for (const int bi : ws_.buildings_by_city[city_index]) {
			const Building& building =
				ws_.buildings[static_cast<std::size_t>(bi)];
			std::string group_id;
			std::string origin_nation;
			owner_kind(building.owner, &group_id, &origin_nation);
			if (group_id.empty() || building.resource.empty()) {
				continue;
			}
			cache.light_i_city.push_back(static_cast<int32_t>(city_index));
			cache.light_i_resource.push_back(String(building.resource.c_str()));
			cache.light_i_owner_group.push_back(String(group_id.c_str()));
		}
	}

	// Construções (CSR): o que a IA por cidade lê — produto de saída, dono,
	// capacidade, utilização, custos e aptidão do sítio. São os MESMOS valores
	// do bloco pesado (fill_city_status), na ordem cidade → construção
	// (buildings_by_city). `site_suitability` = suitability do produto de saída.
	cache.light_b_offset.clear();
	cache.light_b_id.clear();
	cache.light_b_output_product.clear();
	cache.light_b_owner.clear();
	cache.light_b_capacity.clear();
	cache.light_b_utilization.clear();
	cache.light_b_costs.clear();
	cache.light_b_site_suitability.clear();
	cache.light_b_offset.push_back(0);
	for (std::size_t city_index = 0; city_index < cities; ++city_index) {
		const City& bcity = ws_.cities[city_index];
		for (const int bi : ws_.buildings_by_city[city_index]) {
			const Building& building =
				ws_.buildings[static_cast<std::size_t>(bi)];
			cache.light_b_id.push_back(String(building.id.c_str()));
			cache.light_b_output_product.push_back(building.output_product);
			cache.light_b_owner.push_back(String(building.owner.c_str()));
			cache.light_b_capacity.push_back(building.capacity);
			cache.light_b_utilization.push_back(building.utilization);
			cache.light_b_costs.push_back(building.costs);
			cache.light_b_site_suitability.push_back(
				bcity.suitability[building.output_product]);
		}
		cache.light_b_offset.push_back(
			static_cast<int32_t>(cache.light_b_output_product.size()));
	}

	// Jazidas (CSR): o que a IA por cidade lê — recurso (nome do produto),
	// reserva restante e fator de profundidade, mais o bloco estático completo
	// para a tabela ser um substituto do dicionário pesado. Mesmas jazidas
	// rastreadas do bloco pesado (deposit.tracked), ordem de produto.
	cache.light_d_offset.clear();
	cache.light_d_product.clear();
	cache.light_d_resource.clear();
	cache.light_d_initial_kt.clear();
	cache.light_d_remaining_kt.clear();
	cache.light_d_quality.clear();
	cache.light_d_depth_m.clear();
	cache.light_d_efficiency.clear();
	cache.light_d_opex_factor.clear();
	cache.light_d_extracted_kt.clear();
	cache.light_d_depleted.clear();
	cache.light_d_offset.push_back(0);
	for (std::size_t city_index = 0; city_index < cities; ++city_index) {
		const City& dcity = ws_.cities[city_index];
		for (int product = 0; product < ws_.products; ++product) {
			const Deposit& deposit = dcity.deposits[product];
			if (!deposit.tracked) {
				continue;
			}
			cache.light_d_product.push_back(product);
			cache.light_d_resource.push_back(String(product_name(product)));
			cache.light_d_initial_kt.push_back(deposit.initial_kt);
			cache.light_d_remaining_kt.push_back(deposit.remaining_kt);
			cache.light_d_quality.push_back(deposit.quality);
			cache.light_d_depth_m.push_back(deposit.depth_m);
			cache.light_d_efficiency.push_back(extraction_efficiency(deposit));
			cache.light_d_opex_factor.push_back(depth_opex_factor(deposit));
			cache.light_d_extracted_kt.push_back(deposit.extracted_kt);
			// D-086 (decisão do dono): o recurso nunca acaba — `d_depleted` é
			// SEMPRE 0 (campo mantido por contrato do bloco `light`).
			cache.light_d_depleted.push_back(0);
		}
		cache.light_d_offset.push_back(
			static_cast<int32_t>(cache.light_d_product.size()));
	}

	// Obras em andamento (CSR, §15): o que a IA lê para não reconstruir a
	// mesma lacuna enquanto uma obra está em voo (direção de calibração do
	// D-060). Ordem cidade → obra (ordem de criação do vetor
	// construction_projects), determinística. `c_expand` = 1 marca expansão de
	// prédio existente (o produto JÁ existe na cidade); 0 é obra nova.
	cache.light_c_offset.clear();
	cache.light_c_output_product.clear();
	cache.light_c_capacity.clear();
	cache.light_c_expand.clear();
	cache.light_c_offset.push_back(0);
	for (std::size_t city_index = 0; city_index < cities; ++city_index) {
		const std::string& ocity = ws_.cities[city_index].id;
		for (const ConstructionProject& project : ws_.construction_projects) {
			if (project.city != ocity) {
				continue;
			}
			cache.light_c_output_product.push_back(project.output_product);
			cache.light_c_capacity.push_back(project.capacity);
			cache.light_c_expand.push_back(project.expand_existing ? 1 : 0);
		}
		cache.light_c_offset.push_back(
			static_cast<int32_t>(cache.light_c_output_product.size()));
	}

	// Dictionary é por referência: pendura os arrays (o `ptrw` desanexa o
	// buffer por copy-on-write, então reassocia a cada dia) e só cria as
	// chaves escalares uma vez.
	Dictionary& light = cache.light;
	if (!cache.light_filled) {
		light[k.product_count] = static_cast<int>(products);
		light[k.city_count] = static_cast<int>(cities);
		cache.light_filled = true;
	}
	light[k.m_price] = cache.light_m_price;
	light[k.m_stock] = cache.light_m_stock;
	light[k.m_supply] = cache.light_m_supply;
	light[k.m_surplus] = cache.light_m_surplus;
	light[k.m_sold] = cache.light_m_sold;
	light[k.m_shortage] = cache.light_m_shortage;
	light[k.m_desired] = cache.light_m_desired;
	light[k.p_need] = cache.light_p_need;
	light[k.p_consumed] = cache.light_p_consumed;
	light[k.p_satisfaction] = cache.light_p_satisfaction;
	light[k.p_labor_force] = cache.light_p_labor_force;
	light[k.p_unemployed] = cache.light_p_unemployed;
	light[k.i_city] = cache.light_i_city;
	light[k.i_resource] = cache.light_i_resource;
	light[k.i_owner_group] = cache.light_i_owner_group;
	light[k.b_offset] = cache.light_b_offset;
	light[k.b_id] = cache.light_b_id;
	light[k.b_output_product] = cache.light_b_output_product;
	light[k.b_owner] = cache.light_b_owner;
	light[k.b_capacity] = cache.light_b_capacity;
	light[k.b_utilization] = cache.light_b_utilization;
	light[k.b_costs] = cache.light_b_costs;
	light[k.b_site_suitability] = cache.light_b_site_suitability;
	light[k.d_offset] = cache.light_d_offset;
	light[k.d_product] = cache.light_d_product;
	light[k.d_resource] = cache.light_d_resource;
	light[k.d_initial_kt] = cache.light_d_initial_kt;
	light[k.d_remaining_kt] = cache.light_d_remaining_kt;
	light[k.d_quality] = cache.light_d_quality;
	light[k.d_depth_m] = cache.light_d_depth_m;
	light[k.d_efficiency] = cache.light_d_efficiency;
	light[k.d_opex_factor] = cache.light_d_opex_factor;
	light[k.d_extracted_kt] = cache.light_d_extracted_kt;
	light[k.d_depleted] = cache.light_d_depleted;
	light[k.c_offset] = cache.light_c_offset;
	light[k.c_output_product] = cache.light_c_output_product;
	light[k.c_capacity] = cache.light_c_capacity;
	light[k.c_expand] = cache.light_c_expand;
}

// ---------------------------------------------------------------------------
// A4: pedaços do status extraídos para as consultas sob demanda reusarem o
// MESMO código (mesmos valores, mesma ordem de chaves). Movidos de
// `get_status()` sem mudança de lógica.
// ---------------------------------------------------------------------------

void EconomySim::build_ports(StatusCache& cache) {
	const StatusKeys& k = status_keys_;
	cache.ports.resize(ws_.ports.size());
	cache.port_slots.resize(ws_.ports.size());
	for (std::size_t index = 0; index < ws_.ports.size(); ++index) {
		const Port& port = ws_.ports[index];
		Dictionary& dict = cache.port_slots[index];
		dict[k.id] = String(port.id.c_str());
		dict[k.name] = String(port.name.c_str());
		dict[k.nation] = String(port.nation.c_str());
		dict[k.x] = port.center.x;
		dict[k.y] = port.center.y;
		dict[k.landmass] = String(port.landmass.c_str());
		// Capacidade/fila (port_design.md §2): o porto serve a nação toda;
		// `host_city` é só a sede (âncora e base da capacidade).
		dict[k.host_city] = String(port.host_city.c_str());
		const double base = std::max(1.0, port.base_capacity_t_per_day);
		dict[k.level] = std::max(1, static_cast<int>(std::ceil(
			port.capacity_t_per_day / base)));
		dict[k.capacity_t_per_day] = port.capacity_t_per_day;
		dict[k.throughput] = port.used_today_t;
		dict[k.queue] = port.queue_t;
		dict[k.delay_hours] = port.delay_hours;
		dict[k.utilization] = port.capacity_t_per_day > 0.0
			? port.used_today_t / port.capacity_t_per_day : 0.0;
		cache.ports[index] = dict;
	}
}

void EconomySim::build_orders(StatusCache& cache) {
	const StatusKeys& k = status_keys_;
	cache.orders.resize(ws_.last_orders.size());
	cache.order_slots.resize(ws_.last_orders.size());
	cache.order_leg_slots.resize(ws_.last_orders.size());
	if (cache.order_leg_entry.size() < cache.order_slots.size()) {
		cache.order_leg_entry.resize(cache.order_slots.size());
	}
	for (std::size_t index = 0; index < ws_.last_orders.size(); ++index) {
		const TradeOrder& order = ws_.last_orders[index];
		Dictionary& dict = cache.order_slots[index];
		dict[k.from_city] = String(order.from_city.c_str());
		dict[k.to_city] = String(order.to_city.c_str());
		dict[k.from_nation] = String(order.from_nation.c_str());
		dict[k.to_nation] = String(order.to_nation.c_str());
		dict[k.scope] = String(scope_name(order.scope));
		dict[k.origin_price] = order.origin_price;
		dict[k.origin_price_anchor] = order.origin_price_anchor;
		dict[k.tariff] = order.tariff;
		dict[k.delivered_price] = order.delivered_price;
		dict[k.fx_rate] = order.fx_rate;
		dict[k.seller_fx_rate] = order.seller_fx_rate;
		dict[k.buyer_currency] = String(order.buyer_currency.c_str());
		dict[k.seller_currency] = String(order.seller_currency.c_str());
		dict[k.good] = order.product >= 0 && order.product < ws_.products
			? String(product_name(order.product)) : String("wheat");
		dict[k.amount] = order.amount;
		dict[k.transit_hours] = order.transit_hours;
		dict[k.freight_cost] = order.freight_cost;
		Array& legs = cache.order_leg_slots[index];
		legs.resize(order.legs.size());
		std::vector<Dictionary>& leg_entries = cache.order_leg_entry[index];
		if (leg_entries.size() < order.legs.size()) {
			leg_entries.resize(order.legs.size());
		}
		for (std::size_t leg_index = 0; leg_index < order.legs.size(); ++leg_index) {
			Dictionary& leg_dict = leg_entries[leg_index];
			fill_leg_status(leg_dict, order.legs[leg_index]);
			legs[leg_index] = leg_dict;
		}
		dict[k.legs] = legs;
		cache.orders[index] = dict;
	}
}

void EconomySim::build_currencies(StatusCache& cache) {
	const StatusKeys& k = status_keys_;
	cache.currencies.resize(static_cast<int>(ws_.currencies.size()));
	for (std::size_t index = 0; index < ws_.currencies.size(); ++index) {
		const Currency& currency = ws_.currencies[index];
		Dictionary entry;
		entry[k.code] = String(currency.code.c_str());
		entry[k.rate] = currency.rate;
		entry[k.regime] = currency.floating ? "float" : "peg";
		entry[k.fallback] = currency.fallback;
		entry[k.trade_balance] = currency.balance_ema;
		entry[k.source] = String(currency.source.c_str());
		cache.currencies[static_cast<int>(index)] = entry;
	}
}

void EconomySim::build_tariffs(StatusCache& cache) {
	const StatusKeys& k = status_keys_;
	cache.tariffs.resize(static_cast<int>(ws_.tariffs.size()));
	for (std::size_t index = 0; index < ws_.tariffs.size(); ++index) {
		const Tariff& tariff = ws_.tariffs[index];
		Dictionary entry;
		entry[k.buyer] = String(tariff.buyer.c_str());
		entry[k.seller] = String(tariff.seller.c_str());
		entry[k.product] = String(tariff.product.c_str());
		entry[k.rate] = tariff.rate;
		cache.tariffs[static_cast<int>(index)] = entry;
	}
}

void EconomySim::capture_nation_labor() {
	// S17/A4: o agregado `nations[].labor` é semanal. Capturado no fecho do dia
	// `day % 7 == 0` como doubles puros, para um `get_status()` sob demanda em
	// qualquer dia publicar exatamente o que a publicação diária antiga
	// publicaria (o retrato semanal mais recente). A simulação não muda.
	labor_snapshot_.clear();
	std::unordered_map<std::string, int> index;
	index.reserve(ws_.cities.size() * 2);
	for (const City& city : ws_.cities) {
		auto found = index.find(city.nation);
		if (found == index.end()) {
			found = index.emplace(city.nation,
				static_cast<int>(labor_snapshot_.size())).first;
			labor_snapshot_.push_back(NationLaborSnapshot{});
			labor_snapshot_.back().tag = city.nation;
		}
		NationLaborSnapshot& snap =
			labor_snapshot_[static_cast<std::size_t>(found->second)];
		snap.working_age += city.pop.working_age;
		snap.labor_force += city.pop.labor_force;
		snap.employed += city.pop.employed;
		snap.unemployed += city.pop.unemployed;
		snap.students += city.pop.students;
		snap.recruit_reserve += city.pop.recruit_reserve;
		snap.income_population += std::max(0.0, city.simulation_population);
		snap.productivity_weighted += effective_productivity(city)
			* std::max(0.0, city.simulation_population);
		for (int level_index = 0; level_index < kIncomeLevels; ++level_index) {
			snap.income_share[level_index] +=
				city.pop.income_level_share[level_index]
				* std::max(0.0, city.simulation_population);
		}
		for (int level = 0; level < kEducationLevels; ++level) {
			snap.education[level] += city.pop.education[level];
			snap.labor_force_by_level[level] +=
				city.pop.labor_force_by_level[level];
			snap.employed_by_level[level] += city.pop.employed_by_level[level];
			snap.unemployed_by_level[level] +=
				city.pop.unemployed_by_level[level];
			snap.vacancies_by_level[level] +=
				city.pop.vacancies_by_level[level];
			snap.wage_weighted[level] += city.pop.wage_by_level[level]
				* city.pop.labor_force_by_level[level];
			snap.wage_weight[level] += city.pop.labor_force_by_level[level];
		}
	}
	for (NationLaborSnapshot& snap : labor_snapshot_) {
		const auto minimum = ws_.minimum_wages.find(snap.tag);
		snap.minimum_wage = minimum != ws_.minimum_wages.end()
			? std::max(0.0, minimum->second) : 0.0;
		snap.wage_rigidity = ws_.wage_rigidity;
	}
	labor_snapshot_day_ = ws_.day;
}

const EconomySim::NationLaborSnapshot* EconomySim::find_labor_snapshot(
		const std::string& tag) const {
	for (const NationLaborSnapshot& snap : labor_snapshot_) {
		if (snap.tag == tag) {
			return &snap;
		}
	}
	return nullptr;
}

void EconomySim::build_nations(StatusCache& cache,
		const std::vector<double>& in_transit_by_city) {
	const StatusKeys& k = status_keys_;
	// Nações agregadas (telas de política/economia), na ordem de aparição.
	// Uma única passada sobre as cidades (antes: nação × cidade × construção).
	struct NationTotals {
		double population = 0.0;
		double production = 0.0;
		double stockpile = 0.0;
		double need = 0.0;
		double deficit = 0.0;
		double in_transit = 0.0;
		int city_count = 0;
		// F7: mercado de trabalho agregado (soma das cidades da nação).
		double working_age = 0.0;
		double labor_force = 0.0;
		double employed = 0.0;
		double unemployed = 0.0;
		double students = 0.0;
		double recruit_reserve = 0.0;
		double education[kEducationLevels] = {};
		double labor_force_by_level[kEducationLevels] = {};
		double employed_by_level[kEducationLevels] = {};
		double unemployed_by_level[kEducationLevels] = {};
		double vacancies_by_level[kEducationLevels] = {};
		double wage_weighted[kEducationLevels] = {};
		double wage_weight[kEducationLevels] = {};
		// F7 calibração: fatia da POP da nação por nível de consumo
		// (0 = nenhum, 1 = basic, 2 = life, 3 = luxury).
		double income_share[kIncomeLevels] = {};
		double income_population = 0.0;
		// F7 calibração: TFP nacional ponderada pela população de simulação.
		double productivity_weighted = 0.0;
		// B6: patrimônio por quintil somado nas cidades + regiões da nação
		// (poupança + imóvel + terra + participações) — base do Gini de riqueza.
		double wealth[kIncomeStrata] = {};
	};
	// S17: cadência do agregado `labor` (semanal), POR SLOT como sempre foi.
	// A4: um slot parado (o jogo/agendador não constrói status todo dia) é
	// semeado pelo retrato semanal em vez de publicar valores de semanas atrás
	// — é o valor que a publicação diária teria no último múltiplo de 7.
	const bool seed_labor = status_seed_labor_from_snapshot_
		&& labor_snapshot_day_ >= 0 && (ws_.day % 7) != 0
		&& (!cache.labor_cadence_built
			|| labor_snapshot_day_ > cache.labor_values_day);
	const bool labor_due = !cache.labor_cadence_built
		|| (ws_.day % 7 == 0) || seed_labor;
	std::vector<std::string> nation_order;
	std::vector<NationTotals> nation_totals;
	std::unordered_map<std::string, int> nation_slot;
	nation_slot.reserve(ws_.cities.size() * 2);
	for (std::size_t city_index = 0; city_index < ws_.cities.size(); ++city_index) {
		const City& city = ws_.cities[city_index];
		auto found = nation_slot.find(city.nation);
		if (found == nation_slot.end()) {
			found = nation_slot.emplace(city.nation,
				static_cast<int>(nation_order.size())).first;
			nation_order.push_back(city.nation);
			nation_totals.push_back(NationTotals{});
		}
		NationTotals& totals = nation_totals[static_cast<std::size_t>(found->second)];
		++totals.city_count;
		totals.population += city.population;
		totals.stockpile += city.markets[0].stock;
		totals.need += city.pop.need;
		totals.deficit += std::max(0.0, city.pop.need - city.pop.consumed);
		// B6: riqueza nacional = urbana + região (patrimônio por quintil).
		for (int s = 0; s < kIncomeStrata; ++s) {
			totals.wealth[s] += std::max(0.0, city.pop.stratum_wealth[s])
				+ std::max(0.0, city.region.stratum_wealth[s]);
		}
		if (labor_due) {
			totals.working_age += city.pop.working_age;
			totals.labor_force += city.pop.labor_force;
			totals.employed += city.pop.employed;
			totals.unemployed += city.pop.unemployed;
			totals.students += city.pop.students;
			totals.recruit_reserve += city.pop.recruit_reserve;
			totals.income_population += std::max(0.0, city.simulation_population);
			totals.productivity_weighted += effective_productivity(city)
				* std::max(0.0, city.simulation_population);
			for (int level_index = 0; level_index < kIncomeLevels; ++level_index) {
				totals.income_share[level_index] +=
					city.pop.income_level_share[level_index]
					* std::max(0.0, city.simulation_population);
			}
			for (int level = 0; level < kEducationLevels; ++level) {
				totals.education[level] += city.pop.education[level];
				totals.labor_force_by_level[level] +=
					city.pop.labor_force_by_level[level];
				totals.employed_by_level[level] += city.pop.employed_by_level[level];
				totals.unemployed_by_level[level] +=
					city.pop.unemployed_by_level[level];
				totals.vacancies_by_level[level] +=
					city.pop.vacancies_by_level[level];
				totals.wage_weighted[level] += city.pop.wage_by_level[level]
					* city.pop.labor_force_by_level[level];
				totals.wage_weight[level] += city.pop.labor_force_by_level[level];
			}
		}
		for (const int bi : ws_.buildings_by_city[city_index]) {
			const Building& building =
				ws_.buildings[static_cast<std::size_t>(bi)];
			if (building.input_product < 0) {
				totals.production += building.capacity * building.output_scale;
			}
		}
	}
	for (std::size_t index = 0; index < in_transit_by_city.size(); ++index) {
		const double amount = in_transit_by_city[index];
		if (amount == 0.0) {
			continue;
		}
		const auto slot = nation_slot.find(ws_.cities[index].nation);
		if (slot != nation_slot.end()) {
			nation_totals[static_cast<std::size_t>(slot->second)].in_transit += amount;
		}
	}
	static const std::pair<const char*, const char*> kNames[] = {
		{"GBR", "England"}, {"FRA", "France"}, {"DEU", "Germany"},
		{"NED", "Netherlands"}, {"BEL", "Belgium"},
	};
	cache.nations.resize(nation_order.size());
	cache.nation_slots.resize(nation_order.size());
	for (std::size_t index = 0; index < nation_order.size(); ++index) {
		const std::string& tag = nation_order[index];
		const NationTotals& totals = nation_totals[index];
		StatusNationSlot& slot = cache.nation_slots[index];
		Dictionary& nation = slot.dict;
		nation[k.tag] = String(tag.c_str());
		const char* display = tag.c_str();
		for (const auto& [code, name] : kNames) {
			if (tag == code) {
				display = name;
			}
		}
		nation[k.name] = String(display);
		nation[k.city_count] = totals.city_count;
		nation[k.population] = totals.population;
		nation[k.production] = totals.production;
		nation[k.stockpile] = totals.stockpile;
		nation[k.need] = totals.need;
		nation[k.deficit] = totals.deficit;
		nation[k.in_transit] = totals.in_transit;
		// B6: Gini de riqueza da nação (poupança + imóvel + terra + ações).
		nation[k.wealth_gini] = ogs::SimCore::quintile_gini(
			totals.wealth, kIncomeStrata);
		// F4: frota/combustível da nação (a UI e a IA leem daqui; a fila do dia
		// é o sinal de compra de veículos/combustível). O sub-dicionário e o
		// Array de saúde são reaproveitados entre ticks (byte-idêntico: mesmos
		// valores, só sem realocar os contêineres).
		const auto fleet_found = ws_.fleets.find(tag);
		if (fleet_found != ws_.fleets.end()) {
			const Fleet& fleet = fleet_found->second;
			Dictionary& fleet_dict = slot.fleet;
			fleet_dict[k.trucks] = fleet.trucks;
			fleet_dict[k.ships] = fleet.ships;
			fleet_dict[k.planes] = fleet.planes;
			fleet_dict[k.fuel] = fleet.fuel;
			fleet_dict[k.spare_parts] = fleet.spare_parts;
			if (!slot.fleet_built) {
				slot.fleet_health.resize(3);
				fleet_dict[k.health] = slot.fleet_health;
				slot.fleet_built = true;
			}
			slot.fleet_health[0] = fleet.health[0];
			slot.fleet_health[1] = fleet.health[1];
			slot.fleet_health[2] = fleet.health[2];
			fleet_dict[k.fuel_used] = fleet.fuel_used_today;
			fleet_dict[k.queue] = fleet.queue_today;
			fleet_dict[k.missed_no_fleet] = fleet.missed_no_fleet;
			fleet_dict[k.missed_no_fuel] = fleet.missed_no_fuel;
			fleet_dict[k.source] = fleet.derived ? "derived" : "declared";
			nation[k.fleet] = fleet_dict;
		}
		// F7: mercado de trabalho da nação (telas de política/trabalho). O
		// sub-dicionário e os Arrays de níveis são reaproveitados entre ticks.
		// S17: cadência semanal POR SLOT (como sempre foi). A4: o primeiro
		// build de um slot frio — jogo/agendador não constroem status todo dia —
		// é semeado pelo retrato semanal, o mesmo valor que a publicação diária
		// teria no último múltiplo de 7.
		if (labor_due) {
			Dictionary& labor = slot.labor;
			const NationLaborSnapshot* snap = find_labor_snapshot(tag);
			if (seed_labor && snap != nullptr) {
				labor[k.working_age] = snap->working_age;
				labor[k.labor_force] = snap->labor_force;
				labor[k.employed] = snap->employed;
				labor[k.unemployed] = snap->unemployed;
				labor[k.students] = snap->students;
				labor[k.recruit_reserve] = snap->recruit_reserve;
				if (!slot.labor_built) {
					slot.education.resize(3);
					slot.labor_force_by_level.resize(3);
					slot.employed_by_level.resize(3);
					slot.unemployed_by_level.resize(3);
					slot.vacancies_by_level.resize(3);
					slot.unemployment_by_level.resize(kEducationLevels);
					slot.wage_by_level.resize(kEducationLevels);
					slot.income_share.resize(kIncomeLevels);
					labor[k.education] = slot.education;
					labor[k.labor_force_by_level] = slot.labor_force_by_level;
					labor[k.employed_by_level] = slot.employed_by_level;
					labor[k.unemployed_by_level] = slot.unemployed_by_level;
					labor[k.vacancies_by_level] = slot.vacancies_by_level;
					labor[k.unemployment_by_level] = slot.unemployment_by_level;
					labor[k.wage_by_level] = slot.wage_by_level;
					labor[k.income_level_share] = slot.income_share;
					slot.labor_built = true;
				}
				for (int level = 0; level < kEducationLevels; ++level) {
					slot.education[level] = snap->education[level];
					slot.labor_force_by_level[level] =
						snap->labor_force_by_level[level];
					slot.employed_by_level[level] = snap->employed_by_level[level];
					slot.unemployed_by_level[level] =
						snap->unemployed_by_level[level];
					slot.vacancies_by_level[level] = snap->vacancies_by_level[level];
					slot.unemployment_by_level[level] =
						snap->labor_force_by_level[level] > 1e-9
						? snap->unemployed_by_level[level]
							/ snap->labor_force_by_level[level] : 0.0;
					slot.wage_by_level[level] = snap->wage_weight[level] > 1e-9
						? snap->wage_weighted[level] / snap->wage_weight[level]
						: snap->minimum_wage;
				}
				labor[k.minimum_wage] = snap->minimum_wage;
				labor[k.wage_rigidity] = snap->wage_rigidity;
				for (int level_index = 0; level_index < kIncomeLevels; ++level_index) {
					slot.income_share[level_index] =
						snap->income_population > 1e-9
						? snap->income_share[level_index]
							/ snap->income_population : 0.0;
				}
				labor[k.productivity_index] = snap->income_population > 1e-9
					? snap->productivity_weighted / snap->income_population : 1.0;
			} else {
				labor[k.working_age] = totals.working_age;
				labor[k.labor_force] = totals.labor_force;
				labor[k.employed] = totals.employed;
				labor[k.unemployed] = totals.unemployed;
				labor[k.students] = totals.students;
				labor[k.recruit_reserve] = totals.recruit_reserve;
				if (!slot.labor_built) {
					slot.education.resize(3);
					slot.labor_force_by_level.resize(3);
					slot.employed_by_level.resize(3);
					slot.unemployed_by_level.resize(3);
					slot.vacancies_by_level.resize(3);
					slot.unemployment_by_level.resize(kEducationLevels);
					slot.wage_by_level.resize(kEducationLevels);
					slot.income_share.resize(kIncomeLevels);
					labor[k.education] = slot.education;
					labor[k.labor_force_by_level] = slot.labor_force_by_level;
					labor[k.employed_by_level] = slot.employed_by_level;
					labor[k.unemployed_by_level] = slot.unemployed_by_level;
					labor[k.vacancies_by_level] = slot.vacancies_by_level;
					labor[k.unemployment_by_level] = slot.unemployment_by_level;
					labor[k.wage_by_level] = slot.wage_by_level;
					labor[k.income_level_share] = slot.income_share;
					slot.labor_built = true;
				}
				for (int level = 0; level < kEducationLevels; ++level) {
					slot.education[level] = totals.education[level];
					slot.labor_force_by_level[level] =
						totals.labor_force_by_level[level];
					slot.employed_by_level[level] =
						totals.employed_by_level[level];
					slot.unemployed_by_level[level] =
						totals.unemployed_by_level[level];
					slot.vacancies_by_level[level] =
						totals.vacancies_by_level[level];
					slot.unemployment_by_level[level] =
						totals.labor_force_by_level[level] > 1e-9
						? totals.unemployed_by_level[level]
							/ totals.labor_force_by_level[level] : 0.0;
					slot.wage_by_level[level] = totals.wage_weight[level] > 1e-9
						? totals.wage_weighted[level] / totals.wage_weight[level]
						: (ws_.minimum_wages.find(tag) != ws_.minimum_wages.end()
							? std::max(0.0, ws_.minimum_wages.at(tag)) : 0.0);
				}
				const auto minimum = ws_.minimum_wages.find(tag);
				labor[k.minimum_wage] = minimum != ws_.minimum_wages.end()
					? std::max(0.0, minimum->second) : 0.0;
				labor[k.wage_rigidity] = ws_.wage_rigidity;
				for (int level_index = 0; level_index < kIncomeLevels; ++level_index) {
					slot.income_share[level_index] =
						totals.income_population > 1e-9
						? totals.income_share[level_index]
							/ totals.income_population : 0.0;
				}
				labor[k.productivity_index] = totals.income_population > 1e-9
					? totals.productivity_weighted / totals.income_population : 1.0;
			}
			nation[k.labor] = labor;
		}
		cache.nations[index] = nation;
	}
	if (labor_due) {
		cache.labor_values_day = seed_labor ? labor_snapshot_day_ : ws_.day;
	}
	cache.labor_cadence_built = true;
}

Dictionary EconomySim::get_status() {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	// Snapshot do dia já publicado: devolve o mesmo dicionário (a UI lê o dia
	// inteiro; mutações entre ticks só valem no próximo status).
	if (status_live_ >= 0 && status_built_day_ == ws_.day) {
		return status_slots_[status_live_].root;
	}
	const int slot_index = status_live_ < 0 ? 0 : 1 - status_live_;
	StatusCache& cache = status_slots_[slot_index];
	const StatusKeys& k = status_keys_;
	Dictionary& root = cache.root;

	// Subdivisao da fase status na medicao por fase (sem custo no jogo): as 4 marcas
	// `OGS_SIM_PHASE_LAP(part)` abaixo gravam em `status_sub_us_`.
#if defined(OGS_PROFILING)
	ogs::profile::Sink status_sink{status_sub_us_, 4, &profile_};
	const ogs::profile::SinkScope status_scope(&status_sink);
#endif

	root[k.mode] = "demo";
	root[k.day] = ws_.day;
	root[k.calendar_day] = ws_.day;
	root[k.money_total] = money_total();
	root[k.transport_cash] = ws_.transport_cash;
	root[k.foreign_cash] = ws_.foreign_cash;
	root[k.construction_cash] = ws_.construction_cash;
	// C1 (D-102): identidade financeira do dia. `financial_sum` é o novo
	// invariante (tem de ser 0); `cash_residual` é a checagem cruzada entre a
	// enumeração do balanço e o `money_total` acima. O detalhe por agente sai
	// sob demanda em `get_balance_status()` (A4).
	{
		const ogs::balance::Ledger book = ogs::balance::ledger(ws_,
			money_total());
		Dictionary balance;
		balance["money_total"] = book.money_total;
		balance["cash_total"] = book.cash_total();
		balance["cash_residual"] = book.cash_residual();
		balance["receivables"] = book.receivables;
		balance["payables"] = book.payables;
		balance["arrears_plants"] = book.arrears_plants;
		balance["arrears_service"] = book.arrears_service;
		balance["arrears_rural"] = book.arrears_rural;
		balance["monetary_base"] = book.monetary_base;
		balance["monetary_claims"] = book.monetary_claims;
		balance["government_debt"] = book.government_debt;
		// C3 (D-107): o crédito de empresa é zero no default do jogo; os dois
		// números ficam no resumo diário para o painel/diagnóstico não precisar
		// montar o status inteiro.
		balance["group_debt"] = book.group_debt;
		balance["bank_loan_assets"] = book.bank_loans;
		balance["financial_sum"] = book.financial_sum();
		balance["ok"] = std::abs(book.financial_sum()) <= 1e-3
			&& std::abs(book.cash_residual()) <= 1e-3;
		root["balance"] = balance;
	}
	root[k.bankruptcies] = ws_.bankruptcies;
	root[k.depleted_mines] = ws_.depleted_mines;
	// B4: totais de migração do mundo (acumulados) + urbanização CORRENTE.
	// O urbano vivo é `simulation_population` (Σ coortes, acompanha demografia e
	// migração); a região viva é `region.region_pop` (Σ coortes do interior).
	// `region.urban_population`/`region.population_region` são a SEED do dia 0 e
	// não podem ser usados aqui.
	{
		double rural_in = 0.0;
		double city_in = 0.0;
		double city_out = 0.0;
		double urban = 0.0;
		double region_pop = 0.0;
		for (const City& city : ws_.cities) {
			const Region& r = city.region;
			rural_in += r.migration_rural_in_total;
			city_in += r.migration_city_in_total;
			city_out += r.migration_city_out_total;
			urban += city.simulation_population;
			region_pop += r.region_pop;
		}
		Dictionary migration;
		migration["rural_in_total"] = rural_in;
		migration["city_in_total"] = city_in;
		migration["city_out_total"] = city_out;
		migration["urban_population"] = urban;
		migration["region_population"] = region_pop;
		migration["urbanization"] = (urban + region_pop) > 0.0
			? urban / (urban + region_pop) : 0.0;
		root[k.migration] = migration;
	}
	// F8: catálogo de serviços (uma vez) + contadores de negócios do dia.
	if (ws_.services_loaded && !cache.service_sectors_filled) {
		cache.service_sectors = get_service_sectors();
		root[k.service_sectors] = cache.service_sectors;
		cache.service_sectors_filled = true;
	}
	root[k.service_businesses] = get_service_business_count();
	root[k.service_bankruptcies] = ws_.service_bankruptcies;

	// Cabeçalho: os contêineres são pendurados no dicionário UMA vez; a partir
	// daí só são redimensionados/reescritos no lugar.
	if (!cache.header_filled) {
		cache.products.resize(ws_.products);
		for (int index = 0; index < ws_.products; ++index) {
			cache.products[index] = String(product_name(index));
		}
		cache.tot_produced.resize(ws_.products);
		cache.tot_consumed.resize(ws_.products);
		cache.tot_produced_today.resize(ws_.products);
		cache.tot_consumed_today.resize(ws_.products);
		root[k.capital_cost_per_capacity] = kCapitalPerCapacity;
		root[k.products] = cache.products;
		root[k.cities] = cache.cities;
		root[k.ports] = cache.ports;
		root[k.trade_orders] = cache.orders;
		root[k.nations] = cache.nations;
		root[k.currencies] = cache.currencies;
		root[k.tariffs] = cache.tariffs;
		root[k.invariant_violations] = cache.violations;
		root[k.market_totals] = cache.market_totals;
		root[k.produced_total] = cache.tot_produced;
		root[k.consumed_total] = cache.tot_consumed;
		root[k.produced_today] = cache.tot_produced_today;
		root[k.consumed_today] = cache.tot_consumed_today;
		root[k.light] = cache.light;
		cache.header_filled = true;
	}

	cache.violations.resize(ws_.invariant_violations.size());
	for (std::size_t index = 0; index < ws_.invariant_violations.size(); ++index) {
		cache.violations[index] = String(ws_.invariant_violations[index].c_str());
	}

	// Carga em trânsito por cidade numa passada única. Antes, cada cidade
	// varria TODAS as cargas do mundo (O(cidades × cargas) por dia).
	// A4: mesma passada única do status, agora em `compute_in_transit`.
	compute_in_transit(cache.in_transit_by_city);
	OGS_SIM_PHASE_LAP(3);

	// Totais do sistema por produto — a mesma soma que a IA fazia em GDScript
	// sobre `cities[]` (858 cidades × 21 mercados, ~13 ms no frame da virada).
	// Aqui é uma passada de doubles na memória do núcleo, na thread do tick.
	cache.tot_surplus.resize(ws_.products);
	cache.tot_sold.resize(ws_.products);
	cache.tot_demand.resize(ws_.products);
	for (int product = 0; product < ws_.products; ++product) {
		cache.tot_surplus[product] = 0.0;
		cache.tot_sold[product] = 0.0;
		cache.tot_demand[product] = 0.0;
	}
	// Soma só os valores positivos, na ordem das cidades: idêntico (bit a bit)
	// ao laço que o `PrivateInvestor.prepare_totals` fazia.
	for (const City& city : ws_.cities) {
		for (int product = 0; product < ws_.products; ++product) {
			const Market& market = city.markets[product];
			if (market.surplus > 0.0) {
				cache.tot_surplus[product] += market.surplus;
			}
			if (market.sold > 0.0) {
				cache.tot_sold[product] += market.sold;
			}
			// F3: a IA usa este agregado como "existe demanda corrente no
			// sistema". Com bens finais ainda sem produtor, `demand` (compras
			// realizadas) é zero e só `desired` carrega a necessidade das POPs —
			// publicar `desired` permite a fábrica nascer onde há demanda real.
			if (market.desired > 0.0) {
				cache.tot_demand[product] += market.desired;
			}
		}
	}
	// Packed arrays são COW: depois de mutar o buffer da cache, o dicionário
	// precisa receber a referência de novo (senão o consumidor lê o dia anterior).
	cache.market_totals[k.surplus] = cache.tot_surplus;
	cache.market_totals[k.sold] = cache.tot_sold;
	cache.market_totals[k.demand] = cache.tot_demand;

	// Sinais leves por cidade (S7-c): arrays planos para a varredura diária da
	// IA (mercados, POP e incumbentes) — o detalhe pesado por cidade fica sob
	// demanda em get_city_status(id) na próxima fatia.
	fill_light_signals(cache);

	cache.cities.resize(ws_.cities.size());
	cache.city_slots.resize(ws_.cities.size());
	for (std::size_t city_index = 0; city_index < ws_.cities.size(); ++city_index) {
		StatusCitySlot& slot = cache.city_slots[city_index];
		fill_city_status(city_index, slot,
			cache.in_transit_by_city[city_index], false);
		cache.cities[city_index] = slot.dict;
	}
	OGS_SIM_PHASE_LAP(0);

	// Portos (modo mundo) para o minimapa e o mundo 3D. A4: o mesmo código
	// atende `get_ports()`.
	build_ports(cache);

	// Pedidos de comércio do último tick (comboios 3D e HUD). A4: o mesmo
	// código atende `get_trade_orders()`.
	build_orders(cache);
	OGS_SIM_PHASE_LAP(1);

	// Nações agregadas (telas de política/economia). A4: o mesmo código
	// atende `get_nation_rows()`/`get_nation_status(tag)`.
	if (labor_snapshot_day_ < 0) {
		capture_nation_labor();
	}
	build_nations(cache, cache.in_transit_by_city);

	for (int index = 0; index < ws_.products; ++index) {
		cache.tot_produced[index] = ws_.produced[index];
		cache.tot_consumed[index] = ws_.consumed_total[index];
		cache.tot_produced_today[index] = ws_.produced_today[index];
		cache.tot_consumed_today[index] = ws_.consumed_today[index];
	}
	// F5: moedas e tarifas para a UI da F6. A4: o mesmo código atende
	// `get_currencies()`/`get_tariffs()`.
	build_currencies(cache);
	build_tariffs(cache);
	OGS_SIM_PHASE_LAP(2);
	status_live_ = slot_index;
	status_built_day_ = ws_.day;
	return root;
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

	// Carga em trânsito PARA esta cidade (o status diário calcula a mesma soma
	// numa passada única; aqui é só uma cidade, então varre as cargas do mundo).
	double in_transit = 0.0;
	for (const Shipment& shipment : ws_.in_transit) {
		if (shipment.to_city == id) {
			in_transit += shipment.amount;
		}
	}

	// Slot frio (nada preenchido): `fill_city_status` escreve todos os
	// campos — mesmo conteúdo que o bloco por cidade do status diário, mais o
	// detalhe pesado (markets/pop/services/buildings/deposits) sob demanda.
	StatusCitySlot slot;
	fill_city_status(city_index, slot, in_transit, true);
	return slot.dict;
}


Dictionary EconomySim::get_day_summary() {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const StatusKeys& k = status_keys_;
	// A4: resumo leve, construído do zero a cada chamada (≈20 chaves) para o
	// dicionário entregue à UI ser imutável — o tick roda na worker thread e o
	// consumidor não pode ver um dicionário compartilhado sendo reescrito.
	Dictionary root;
	root[k.mode] = "demo";
	root[k.day] = ws_.day;
	root[k.calendar_day] = ws_.day;
	root[k.money_total] = money_total();
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
		// B7: agregados leves (média ponderada por população).
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
	// Se o status do dia já foi montado (tools/testes/uma tela pediu), devolve
	// o MESMO bloco `nations` — as fichas consultadas batem bit a bit com ele.
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

void EconomySim::set_recipes(const Array& recipes) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	ws_.recipes.clear();
	ws_.recipe_lookup.clear();
	ws_.recipes.reserve(static_cast<std::size_t>(recipes.size()));
	for (int index = 0; index < recipes.size(); ++index) {
		const Dictionary entry = recipes[index];
		const String output_id = String(entry.get("output", ""));
		const int output = product_index(output_id);
		if (output < 0) {
			continue;  // receita de bem que não existe no goods.json carregado
		}
		Recipe recipe;
		recipe.id = std::string(String(entry.get("id", "")).utf8().get_data());
		recipe.output = output;
		recipe.yield = std::max(1e-9, safe_nonneg(
			static_cast<double>(entry.get("yield", 1.0))));
		recipe.capital_per_capacity = std::max(1.0, safe_nonneg(
			static_cast<double>(entry.get("capital_per_capacity", 2000.0))));
		recipe.jobs_per_ton = safe_nonneg(
			static_cast<double>(entry.get("jobs_per_ton", 7.5)));
		recipe.wage = safe_nonneg(static_cast<double>(entry.get("wage", 2.0)));
		recipe.opex_per_ton = safe_nonneg(
			static_cast<double>(entry.get("opex_per_ton", 2.5)));
		// F7: escolaridade das vagas. `education_mix` explícito vence a tabela
		// por setor (dado do Game Systems Designer); sem ele, o setor decide.
		const std::string sector(
			String(entry.get("sector", "")).utf8().get_data());
		education_mix_for_sector(sector, recipe.education_mix);
		if (entry.has("education_mix")) {
			double mix[3] = {-1.0, -1.0, -1.0};
			const Variant mix_value = entry["education_mix"];
			if (mix_value.get_type() == Variant::ARRAY) {
				const Array values = mix_value;
				for (int level = 0;
						level < kEducationLevels && level < values.size();
						++level) {
					mix[level] = std::max(0.0, double(values[level]));
				}
			} else if (mix_value.get_type() == Variant::DICTIONARY) {
				const Dictionary values = mix_value;
				static const char* kNames[3] = {
					"basic", "intermediate", "higher"};
				for (int level = 0; level < kEducationLevels; ++level) {
					if (values.has(kNames[level])) {
						mix[level] = std::max(0.0,
							double(values[kNames[level]]));
					}
				}
			}
			const double total = std::max(0.0, mix[0]) + std::max(0.0, mix[1])
				+ std::max(0.0, mix[2]);
			if (total > 1e-9) {
				for (int level = 0; level < kEducationLevels; ++level) {
					recipe.education_mix[level] = std::max(0.0, mix[level]) / total;
				}
			}
		}
		Array inputs = entry.get("inputs", Array());
		for (int slot = 0; slot < inputs.size() && slot < kMaxRecipeInputs; ++slot) {
			const Dictionary input = inputs[slot];
			const int product = product_index(String(input.get("good", "")));
			const double qty = safe_nonneg(static_cast<double>(input.get("qty", 1.0)));
			if (product < 0 || qty <= 0.0) {
				continue;
			}
			recipe.inputs[recipe.input_count] = product;
			recipe.input_qty[recipe.input_count] = qty;
			++recipe.input_count;
		}
		if (recipe.input_count <= 0) {
			continue;
		}
		Array byproducts = entry.get("byproducts", Array());
		for (int slot = 0; slot < byproducts.size() && slot < kMaxRecipeInputs; ++slot) {
			const Dictionary item = byproducts[slot];
			const int product = product_index(String(item.get("good", "")));
			const double qty = safe_nonneg(static_cast<double>(item.get("qty", 1.0)));
			if (product < 0 || qty <= 0.0) {
				continue;
			}
			recipe.byproducts[recipe.byproduct_count] = product;
			recipe.byproduct_qty[recipe.byproduct_count] = qty;
			++recipe.byproduct_count;
		}
		ws_.recipe_lookup[recipe.id] = static_cast<int>(ws_.recipes.size());
		ws_.recipes.push_back(recipe);
	}
}

void EconomySim::seed_industry() {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	if (!ws_.world_mode || ws_.industry_seeded || ws_.recipes.empty()
			|| ws_.cities.empty()) {
		return;
	}
	for (const Building& building : ws_.buildings) {
		if (building.recipe >= 0 || building.input_product >= 0) {
			ws_.industry_seeded = true;  // o mundo já tem indústria
			return;
		}
	}
	const int flour_product = product_index(String("flour"));
	const int bread_product = product_index(String("bread"));
	int flour_recipe = -1;
	int bread_recipe = -1;
	for (std::size_t index = 0; index < ws_.recipes.size(); ++index) {
		if (ws_.recipes[index].output == flour_product && flour_recipe < 0) {
			flour_recipe = static_cast<int>(index);
		}
		if (ws_.recipes[index].output == bread_product && bread_recipe < 0) {
			bread_recipe = static_cast<int>(index);
		}
	}
	if (flour_recipe < 0) {
		return;  // recipes.json ainda não carregou
	}
	// Maiores cidades (capitais/megacidades) por população de simulação.
	std::vector<int> order(ws_.cities.size());
	for (std::size_t index = 0; index < order.size(); ++index) {
		order[index] = static_cast<int>(index);
	}
	std::stable_sort(order.begin(), order.end(), [this](int left, int right) {
		return ws_.cities[static_cast<std::size_t>(left)].simulation_population
			> ws_.cities[static_cast<std::size_t>(right)].simulation_population;
	});
	const int seed_cities = std::min<int>(6, static_cast<int>(order.size()));
	int created = 0;
	// R1/Q-014: as plantas de referência abaixo são o estoque industrial
	// PRÉ-EXISTENTE do cenário (não obra nova): nascem concluídas. A obra física
	// (§15) segue valendo para tudo o que a IA/o jogador investe depois.
	const bool previous_bootstrap = bootstrap_construction_;
	bootstrap_construction_ = true;
	for (int slot = 0; slot < seed_cities; ++slot) {
		City& city = ws_.cities[static_cast<std::size_t>(order[slot])];
		// Capacidade de referência = 5% da necessidade alimentar diária da
		// cidade, em toneladas de farinha/pão. É uma ESTIMATIVA de transição
		// (§12.3), não um dado observado; calibrada no laboratório em F3.
		const double capacity = std::clamp(
			city.simulation_population * kNeedPerCapita * 0.05, 2.0, 20.0);
		const Recipe& flour = ws_.recipes[static_cast<std::size_t>(flour_recipe)];
		const double cost = capacity * flour.capital_per_capacity;
		if (city.firm_cash + city.government_budget < cost * 2.0) {
			continue;
		}
		const String city_id(city.id.c_str());
		const String mill_id(("SEED-FLOUR-" + city.id).c_str());
		Dictionary mill = build(city_id, mill_id, "flour", -1, flour_product,
			capacity, 0.0, 0.0, 0.0, 1.0, "state", cost, String(flour.id.c_str()));
		if (!bool(mill.get("created", false))) {
			continue;
		}
		if (Building* built = find_building(city.id,
				std::string(mill_id.utf8().get_data()))) {
			built->strategic = true;
		}
		sim_core_.mark_project_strategic(
			std::string(mill_id.utf8().get_data()));
		++created;
		if (bread_recipe >= 0) {
			const Recipe& bread =
				ws_.recipes[static_cast<std::size_t>(bread_recipe)];
			const double bread_capacity = capacity * 0.8;
			const double bread_cost = bread_capacity * bread.capital_per_capacity;
			const String bakery_id(("SEED-BREAD-" + city.id).c_str());
			Dictionary bakery = build(city_id, bakery_id, "bread", -1,
				bread_product, bread_capacity, 0.0, 0.0, 0.0, 1.0, "state",
				bread_cost, String(bread.id.c_str()));
			if (bool(bakery.get("created", false))) {
				if (Building* built = find_building(city.id,
						std::string(bakery_id.utf8().get_data()))) {
					built->strategic = true;
				}
				sim_core_.mark_project_strategic(
					std::string(bakery_id.utf8().get_data()));
				++created;
			}
		}
	}
	// F3: corrente final representativa (bem de POP) para a demanda final não
	// ficar só no papel. A IA ainda não atravessa a cadeia inteira quando o
	// insumo não existe (gate de `_input_available` + uma construção por
	// visita), então as 3 maiores cidades com aptidão para a matéria-prima
	// recebem uma cadeia de referência cotton→fabric→clothes e wood→cellulose→
	// paper. É ESTIMATIVA de transição (§12.3), marcada `strategic`, não dado
	// observado; as capacidades derivam da necessidade per capita do nível.
	auto recipe_for = [this](const char* output_id) {
		const int product = product_index(String(output_id));
		for (std::size_t index = 0; index < ws_.recipes.size(); ++index) {
			if (ws_.recipes[index].output == product) {
				return static_cast<int>(index);
			}
		}
		return -1;
	};
	auto seed_final_chain = [this, &recipe_for](City& city, const char* raw_id,
			const char* mid_id, const char* final_id, double per_capita_day,
			double max_capacity) -> int {
		const int raw_product = product_index(String(raw_id));
		const int mid_recipe = recipe_for(mid_id);
		const int final_recipe = recipe_for(final_id);
		if (raw_product < 0 || mid_recipe < 0 || final_recipe < 0) {
			return 0;
		}
		// Sem aptidão para a matéria-prima não há cadeia local (nada inventado).
		if (city.suitability[raw_product] < 0.30) {
			return 0;
		}
		const double final_capacity = std::clamp(
			city.simulation_population * per_capita_day, 2.0, max_capacity);
		for (const auto& existing : ws_.buildings) {
			if (existing.city == city.id
					&& existing.id.find("SEED-" + std::string(final_id)) != std::string::npos) {
				return 0;  // já semeada
			}
		}
		const Recipe& mid = ws_.recipes[static_cast<std::size_t>(mid_recipe)];
		const Recipe& final = ws_.recipes[static_cast<std::size_t>(final_recipe)];
		// R2/Q-014: a lavoura bruta (algodão/madeira) nasce JUNTO da fábrica que
		// a consome — sem ela a matéria-prima não existe no mundo (a IA excluía
		// essas lavouras, então fabric/cellulose/paper ficavam sem oferta desde o
		// dia 0). Capacidade = o insumo que a transformação consome; custo pelo
		// capital padrão por capacidade das primárias (ESTIMATIVA §12.3).
		double raw_capacity = 0.0;
		for (int input = 0; input < mid.input_count; ++input) {
			if (mid.inputs[input] == raw_product) {
				raw_capacity += final_capacity * mid.input_qty[input] / mid.yield;
			}
		}
		if (raw_capacity <= 0.0) {
			return 0;
		}
		raw_capacity = std::max(2.0, raw_capacity);
		const double raw_cost = raw_capacity * kCapitalPerCapacity;
		const double total_cost =
			raw_cost
			+ final_capacity * mid.capital_per_capacity
			+ final_capacity * final.capital_per_capacity;
		if (city.government_budget < total_cost) {
			return 0;
		}
		const String city_id(city.id.c_str());
		int built = 0;
		const String raw_building_id(("SEED-" + std::string(raw_id) + "-" + city.id).c_str());
		Dictionary raw_build = build(city_id, raw_building_id, String(raw_id),
			-1, raw_product, raw_capacity, 0.0, 0.0, 0.0, 1.0, "state",
			raw_cost, String(""));
		if (bool(raw_build.get("created", false))) {
			if (Building* node = find_building(city.id,
					std::string(raw_building_id.utf8().get_data()))) {
				node->strategic = true;
			}
			++built;
		}
		const String mid_building_id(("SEED-" + std::string(mid_id) + "-" + city.id).c_str());
		Dictionary mid_build = build(city_id, mid_building_id, String(mid_id),
			-1, ws_.recipes[static_cast<std::size_t>(mid_recipe)].output,
			final_capacity, 0.0, 0.0, 0.0, 1.0, "state",
			final_capacity * mid.capital_per_capacity,
			String(mid.id.c_str()));
		if (bool(mid_build.get("created", false))) {
			if (Building* node = find_building(city.id,
					std::string(mid_building_id.utf8().get_data()))) {
				node->strategic = true;
			}
			sim_core_.mark_project_strategic(
				std::string(mid_building_id.utf8().get_data()));
			++built;
		}
		const String final_building_id(("SEED-" + std::string(final_id) + "-" + city.id).c_str());
		Dictionary final_build = build(city_id, final_building_id, String(final_id),
			-1, ws_.recipes[static_cast<std::size_t>(final_recipe)].output,
			final_capacity, 0.0, 0.0, 0.0, 1.0, "state",
			final_capacity * final.capital_per_capacity,
			String(final.id.c_str()));
		if (bool(final_build.get("created", false))) {
			if (Building* node = find_building(city.id,
					std::string(final_building_id.utf8().get_data()))) {
				node->strategic = true;
			}
			sim_core_.mark_project_strategic(
				std::string(final_building_id.utf8().get_data()));
			++built;
		}
		return built;
	};
	if (ws_.needs_loaded) {
		// Só capital com folga alimentar: a fábrica de referência não pode
		// competir com a comida de uma nação importadora (EGY/SAH). A folga é
		// medida pela capacidade primária de alimento já instalada × aptidão.
		int chains_seeded = 0;
		const int scan = std::min<int>(24, static_cast<int>(order.size()));
		for (int slot = 0; slot < scan && chains_seeded < 3; ++slot) {
			const int city_index = order[static_cast<std::size_t>(slot)];
			City& city = ws_.cities[static_cast<std::size_t>(city_index)];
			double food_capacity = 0.0;
			for (const int bi : ws_.buildings_by_city[static_cast<std::size_t>(city_index)]) {
				const Building& building = ws_.buildings[static_cast<std::size_t>(bi)];
				if (food_calories(building.output_product) > 0.0) {
					food_capacity += building.capacity
						* city.suitability[building.output_product];
				}
			}
			const double food_need = city.simulation_population * kNeedPerCapita;
			if (food_capacity < food_need * 1.10) {
				continue;
			}
			int built = 0;
			// basic/clothing: 0,0004 t/mês por habitante.
			built += seed_final_chain(city, "cotton", "fabric", "clothes",
				0.0004 / 30.0, 20.0);
			// institutional/government paper: 0,005 × âncora alimentar/dia.
			built += seed_final_chain(city, "wood", "cellulose", "paper",
				kNeedPerCapita * 0.005, 10.0);
			if (built > 0) {
				++chains_seeded;
				created += built;
			}
		}
	}
	bootstrap_construction_ = previous_bootstrap;
	ws_.industry_seeded = true;
	if (created > 0) {
		UtilityFunctions::print(String(("economia: bootstrap industrial (estimativa) — "
			+ std::to_string(created) + " plantas de referência semeadas").c_str()));
	}
}

void EconomySim::set_needs(const Dictionary& needs) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	// Cesta alimentar a partir do nível básico de needs.json. Só a categoria
	// `food` tem âncora física no núcleo hoje (kNeedPerCapita); as demais
	// categorias ficam para F3.
	const Dictionary levels = needs.get("levels", Dictionary());
	const Dictionary basic = levels.get("basic", Dictionary());
	const Dictionary categories = basic.get("categories", Dictionary());
	const Dictionary food = categories.get("food", Dictionary());
	const Array substitutes = food.get("substitutes", Array());
	if (substitutes.is_empty()) {
		return;  // mantém a cesta validada (5 culturas)
	}
	const Dictionary equivalence = needs.get("equivalence", Dictionary());
	const Dictionary calories = equivalence.get("calories", Dictionary());
	std::vector<int> products;
	std::vector<double> factors;
	for (int index = 0; index < substitutes.size()
			&& static_cast<int>(products.size()) < kMaxFoodProducts; ++index) {
		const String id = String(substitutes[index]);
		const int product = product_index(id);
		if (product < 0) {
			continue;
		}
		double factor = 1.0;
		if (calories.has(id)) {
			factor = static_cast<double>(calories[id]);
		}
		factor = std::clamp(factor, 0.05, 4.0);
		// Mantém a ordem data-driven, mas garante os 5 grãos validados no
		// início quando estão entre os substitutos (a ordem de consumo importa
		// sob orçamento apertado: os mais baratos/legados primeiro).
		products.push_back(product);
		factors.push_back(factor);
	}
	if (products.size() < 2) {
		return;
	}
	// Ordem de consumo: os 5 grãos/raízes validados primeiro (a ordem importa
	// sob orçamento apertado e no fallback "sem estoque"); os processados
	// (bread/meat/canned_food/bagged_rice) entram depois.
	static const char* const kLegacyFood[] = {
		"wheat", "rice", "maize", "potato", "soy"};
	std::vector<int> ordered_products;
	std::vector<double> ordered_factors;
	for (const char* legacy : kLegacyFood) {
		const int index = product_index(String(legacy));
		for (std::size_t slot = 0; slot < products.size(); ++slot) {
			if (products[slot] == index) {
				ordered_products.push_back(products[slot]);
				ordered_factors.push_back(factors[slot]);
				break;
			}
		}
	}
	for (std::size_t slot = 0; slot < products.size(); ++slot) {
		bool duplicate = false;
		for (const int existing : ordered_products) {
			if (existing == products[slot]) {
				duplicate = true;
				break;
			}
		}
		if (!duplicate) {
			ordered_products.push_back(products[slot]);
			ordered_factors.push_back(factors[slot]);
		}
	}
	ws_.food_products = std::move(ordered_products);
	ws_.food_factors = std::move(ordered_factors);

	// F3: níveis completos (basic/life/luxury). A cesta alimentar acima é o
	// contrato validado e continua com o laço próprio no tick; aqui entram os
	// níveis e as categorias não alimentares, com a ordem dos substitutos
	// preservada (do melhor ao último caso).
	ws_.needs_levels.clear();
	ws_.needs_food_level = -1;
	ws_.needs_loaded = false;
	ws_.government_products.clear();
	ws_.government_per_capita_day.clear();
	const Dictionary levels_dict = needs.get("levels", Dictionary());
	const Array level_names = levels_dict.keys();
	struct ParsedLevel {
		int priority = 1;
		String name;
		Dictionary data;
	};
	std::vector<ParsedLevel> parsed_levels;
	parsed_levels.reserve(static_cast<std::size_t>(level_names.size()));
	for (int index = 0; index < level_names.size(); ++index) {
		const String name = String(level_names[index]);
		const Dictionary data = levels_dict.get(name, Dictionary());
		if (data.is_empty()) {
			continue;
		}
		ParsedLevel entry;
		entry.priority = int(data.get("priority", index + 1));
		entry.name = name;
		entry.data = data;
		parsed_levels.push_back(std::move(entry));
	}
	std::stable_sort(parsed_levels.begin(), parsed_levels.end(),
		[](const ParsedLevel& left, const ParsedLevel& right) {
			return left.priority < right.priority;
		});
	for (const ParsedLevel& parsed : parsed_levels) {
		if (static_cast<int>(ws_.needs_levels.size()) >= kMaxNeedsLevels) {
			break;
		}
		NeedsLevel level;
		level.id = std::string(parsed.name.utf8().get_data());
		level.priority = parsed.priority;
		level.weight = std::clamp(
			static_cast<double>(parsed.data.get("satisfaction_weight", 0.0)), 0.0, 1.0);
		const int level_index = static_cast<int>(ws_.needs_levels.size());
		const Dictionary categories = parsed.data.get("categories", Dictionary());
		const Array category_names = categories.keys();
		for (int ci = 0; ci < category_names.size(); ++ci) {
			const String category_name = String(category_names[ci]);
			const Dictionary category_data = categories.get(category_name, Dictionary());
			const Array substitutes = category_data.get("substitutes", Array());
			if (substitutes.is_empty()) {
				continue;
			}
			NeedsCategory category;
			category.id = std::string(category_name.utf8().get_data());
			category.per_capita_day = std::max(0.0,
				static_cast<double>(category_data.get("per_capita_t_month", 0.0)))
				/ 30.0;
			// Cada categoria declara a tabela de equivalência que usa
			// (ex.: basic/food → equivalence.calories).
			const String eq_key = String(category_data.get("equivalence_key", ""));
			Dictionary eq_table;
			if (!eq_key.is_empty()) {
				eq_table = equivalence.get(eq_key, Dictionary());
			}
			for (int si = 0; si < substitutes.size(); ++si) {
				const String id = String(substitutes[si]);
				const int product = product_index(id);
				if (product < 0) {
					continue;
				}
				double factor = 1.0;
				if (eq_table.has(id)) {
					factor = static_cast<double>(eq_table[id]);
				}
				factor = std::clamp(factor, 0.05, 8.0);
				category.substitutes.push_back(product);
				category.equivalence.push_back(factor);
			}
			if (category.substitutes.empty()) {
				continue;
			}
			if (category_name == String("food") && ws_.needs_food_level < 0) {
				ws_.needs_food_level = level_index;
			}
			level.categories.push_back(std::move(category));
		}
		if (level.categories.empty()) {
			continue;
		}
		ws_.needs_levels.push_back(std::move(level));
	}
	if (ws_.needs_levels.empty()) {
		++status_static_version_;
		status_invalidate();
		return;
	}
	ws_.needs_loaded = true;
	const double replacement_per_year = std::clamp(
		static_cast<double>(needs.get("durable_replacement_per_year", 0.1)), 0.0, 0.99);
	ws_.durable_decay = replacement_per_year > 0.0
		? 1.0 - std::pow(1.0 - replacement_per_year, 1.0 / 365.0) : 0.0;
	// Consumo institucional (decisão §12.10): o Estado compra paper/computer.
	// O dado de needs.json lista os BENS (`institutional.government`); a
	// quantidade não existe na spec, então usa-se uma fração ESTIMADA da âncora
	// alimentar (kNeedPerCapita), declarada na tabela abaixo e registrada em
	// GAME_STATE/perf como estimativa — nunca como medição. Se um needs.json
	// futuro trouxer `institutional_per_capita_t_month.government.<bem>`, ele
	// vence a estimativa.
	const Dictionary institutional = needs.get("institutional", Dictionary());
	const Array government = institutional.get("government", Array());
	const Dictionary quantities_root =
		needs.get("institutional_per_capita_t_month", Dictionary());
	const Dictionary quantities = quantities_root.get("government", Dictionary());
	static const struct {
		const char* id;
		double anchor_share;  // fração de kNeedPerCapita por dia
	} kEstimatedGovernmentNeeds[] = {
		{"paper", 0.005},
		{"computer", 0.0001},
	};
	for (int index = 0; index < government.size(); ++index) {
		const String id = String(government[index]);
		const int product = product_index(id);
		if (product < 0) {
			continue;
		}
		double per_day = 0.0;
		if (quantities.has(id)) {
			per_day = std::max(0.0, static_cast<double>(quantities[id])) / 30.0;
		} else {
			for (const auto& estimate : kEstimatedGovernmentNeeds) {
				if (id == String(estimate.id)) {
					per_day = kNeedPerCapita * estimate.anchor_share;
					break;
				}
			}
		}
		if (per_day <= 0.0) {
			continue;
		}
		ws_.government_products.push_back(product);
		ws_.government_per_capita_day.push_back(per_day);
	}
	// Necessidades carregadas depois do mundo: o estado por cidade acompanha.
	for (City& city : ws_.cities) {
		init_pop_needs(city.pop);
	}
	++status_static_version_;
	status_invalidate();
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
	// Busca do comércio de alimento (K vizinhos + M ofertas externas).
	ws_.trade_neighbors = std::clamp(
		static_cast<int>(number("trade_neighbors", ws_.trade_neighbors)), 1, 256);
	ws_.trade_external = std::clamp(
		static_cast<int>(number("trade_external", ws_.trade_external)), 0, 256);
	// F7 calibração: folga de custo de vida do gate de renda → nível de
	// consumo. Padrão = kIncomeBasketMargin; o laboratório pode isolá-la
	// (1.0 = sem folga) para testar a regra do nível sem depender da calibração.
	ws_.income_basket_margin = std::clamp(
		number("income_basket_margin", ws_.income_basket_margin), 1.0, 8.0);
	// B5 (D-089): penalidade máxima de acesso da infra (estrada 0). Gancho de
	// laboratório do A/B da infra — 0 isola o frete do efeito dos níveis e o
	// padrão continua sendo kInfraAccessPenaltyMax.
	ws_.infra_access_penalty = std::clamp(
		number("infra_access_penalty", ws_.infra_access_penalty), 0.0, 1.0);
	// D-105: juro da taxa básica no envelope de crédito da safra. Padrão do
	// jogo = DESLIGADO (sem credor até a C4); o A/B de 365 d liga por aqui.
	if (values.has("harvest_credit_rate")) {
		ws_.harvest_credit_rate = number("harvest_credit_rate", 0.0) > 0.5;
	}
	// C3 (D-107): regras do crédito de empresa (default de jogo em
	// data/definitions/economy/bank_credit.json). 0 = default do sim/bank.h.
	// O teste/laboratório liga o crédito por aqui; o jogo nasce desligado.
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
	// O status reuse arrays de aptidão: invalida só quando o dado muda.
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
		// Idempotente: não reabastece uma jazida já em exploração (load/save).
		return false;
	}
	deposit.tracked = true;
	deposit.initial_kt = safe_nonneg(reserve_kt);
	deposit.remaining_kt = deposit.initial_kt;
	deposit.extracted_kt = 0.0;
	// D-086b: a mina semeada pela produção real nasce OPERANDO. O contrato do
	// GeographyService chega no `initialize_world`, antes de a jazida ser
	// declarada aqui; a reserva do estágio (fatia de referência do núcleo) é
	// aberta agora, na primeira declaração do depósito. Mina histórica
	// (`stage_pending == false`) fica no piso até pagar o aprofundamento.
	for (Building& building : ws_.buildings) {
		// `stage_pending` primeiro: a maioria das construções não é semente e o
		// teste barato evita o compare de string por construção/depósito.
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
	// [0, 8]: 0 desliga a renda (calibração/teste), 8 cobre com folga a maior
	// razão PIB PPP per capita do cenário (Singapura ≈ 6,3× a média mundial) e
	// corta dados corrompidos sem virar um teto econômico silencioso.
	city->external_income_index = std::clamp(index, 0.0, 8.0);
	// F7: a estrutura etária, a escolaridade e a participação derivam do mesmo
	// índice de renda real (único dado por país do cenário). Reaplica o perfil
	// quando o índice chega do GeographyService, logo após criar o mundo.
	apply_labor_profile(*city);
	++status_static_version_;
}

void EconomySim::apply_set_productivity_index(const String& city_id, double index) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	City* city = find_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr) {
		return;
	}
	// 0 (ou negativo) = DERIVAR do índice de renda real: o dado do PIB PPP per
	// capita chega por set_external_income_index e a TFP acompanha. Um
	// valor positivo é fator explícito do laboratório. Os limites só contêm
	// dado corrompido: o cenário real fica em ~0,38 (NGA) a ~3,57 (USA).
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
	// B3: o interior espelha os quintis observados do país (20% de pessoas cada;
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
	// como set_external_income_index e set_income_gini na inicialização. Não é
	// ação de jogador — não passa pela fila A3 porque não existe, no jogo, um
	// comando para reescrever a distribuição inicial.
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
	// A aglomeração pode ter mudado: re-semeia o perfil urbano (demografia/
	// estratos) e o bloco do interior. API de setup — vale antes do dia 1.
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
	// Renda em base DIÁRIA (o salário do interior é pago por semana na cadência
	// A6; `region.wage_income`/`income`/`taxes` já guardam o equivalente diário).
	// Os totais semanais (brutos) ficam explícitos com o sufixo `_weekly`.
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
	// B4: migração — acumuladores + fluxo do último fechamento mensal + a
	// urbanização CORRENTE (urbano vivo ÷ população exibida) contra a referência
	// nacional (World Bank; -1 = sem dado). O urbano vivo é `simulation_population`
	// (Σ coortes), não `region.urban_population` (a SEED do dia 0).
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
	// B5: infraestrutura do estado — níveis, manutenção do dia e os
	// multiplicadores derivados (custo de acesso, produtividade, êxodo).
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
	// B7: informalidade, mortalidade e insatisfação do interior.
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
	// B5: concessões — dono (F9) e royalty por mina/fazenda (extração primária)
	// da região (construções da cidade-polo). Dono = "state" | "national:<g>" |
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
					continue;  // só mina/fazenda (extração primária)
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
	// B6: patrimônio da região — terra, moradia do interior e anel periurbano.
	// Os VALORES são de mercado (não são dinheiro); `book` é a base contábil
	// (1ª avaliação) e o fluxo mensal é o aluguel efetivamente pago no fecho.
	// Os ativos vêm em `assets` com a semântica de BALANÇO da C1 (tipo, dono,
	// quantidade, valor de mercado, valor contábil e fluxo mensal).
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
		// O anel tem estoque próprio: a casa do interior é de quem NÃO vive nele.
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
		// BALANÇO (pronto para a C1): um ativo por tipo × dono (quintil).
		// `market_value` = valor de mercado; `book_value` = valor contábil (1ª
		// avaliação, congelada — não há compra/venda nesta fase);
		// `income_month` = fluxo de renda mensal (aluguel do anel repartido
		// entre os donos; terra e moradia próprias não geram aluguel).
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
	// [0, 1]: 0 desliga (legado por escolaridade); dados corrompidos não viram
	// sigma negativo. A faixa real estimada por região fica em 0,33–0,47.
	city->income_gini = std::clamp(gini, 0.0, 1.0);
	// B2 (correção): o Gini chega DEPOIS do `initialize_world` (o GDScript
	// declara a renda no `apply_to_sim`), mas os quintis já foram semeados pela
	// proxy com Gini zero — repartição igual, que zerava life/luxury na classe
	// publicada. Re-semeia a proxy com o Gini declarado; a repartição de DADO
	// observado (set_city_strata) não é tocada.
	if (city->pop.strata_ready && !city->pop.strata_from_data) {
		sim_core_.seed_strata(*city, nullptr);
	}
	// Recalcula as classes na hora: o status lido antes do primeiro tick já
	// publica a repartição ancorada (se o mundo ainda não tem preços, cai no
	// espelho dos estratos, sem inventar Gini).
	refresh_income_classes(*city);
	++status_static_version_;
}

// ---------------------------------------------------------------------------
// F7 — trabalho, educação e renda.
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
					// Perfil explícito substitui a estrutura etária: as coortes
					// em formação do perfil derivado da renda não podem vazar
					// para a nova distribuição (senão "todo mundo básico" ainda
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
	// O piso vale a partir do próximo ajuste salarial; cidades sem histórico de
	// salário recebem o âncora com o piso imediatamente.
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
// F9 — EMPREGO SETORIAL (docs/economy/sector_employment_report.md)
//
// O mercado de trabalho da F7 continua fechando emprego/desemprego como antes
// (o setor de serviços é o empregador residual). A F9 decompõe esse emprego em
// setores com estrutura ancorada em dados observados:
//   0 agropecuária — da produção local de alimentos (t/dia) e da TFP;
//   1 indústria/mineração/construção — da âncora ILO por renda e do valor
//     adicionado simulado (quando ele passa da âncora);
//   2 serviços — residual (formal + informal de serviços);
//   3 informal — SOBREPOSIÇÃO (fração do emprego total), não soma com 0..2.
// Não cria nem destrói dinheiro: a renda domiciliar continua agregada por
// cidade. O único fluxo novo é o autoconsumo agrícola em espécie (ver o piso de
// subsistência no consumo), que vem da produção local e é conservado no estoque.
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
// B1 — ESTRUTURA ETÁRIA (data/definitions/population/age_structure_2026.json)
//
// DEFINIÇÃO estática, não estado do mundo: só o mapa por tag de nação entra no
// WorldState. O arquivo traz cinco faixas como fração da população, a fração
// 18-49, a TFR e as taxas brutas de natalidade/mortalidade por mil. Nação cujas
// faixas não fecham em 1 (tolerância de 2%) é descartada inteira: dado
// inconsistente não vira número de simulação. As cidades NÃO são tocadas aqui:
// o mapa é a DEFINIÇÃO e a semeadura das coortes acontece em
// `SimCore::seed_demography` (chamada por apply_labor_profile e pelo load do
// save), que precisa das cidades já criadas.
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
// B7 — INFORMALIDADE E ACESSO À SAÚDE (data/definitions/economy/informality.json
// e data/definitions/population/health_access.json).
//
// DEFINIÇÃO estática no WorldState (como `age_structures`), NÃO entra no save:
// a fração informal inicial é semeada no dia 0 (`SimCore::seed_informality`) e
// a referência de saúde é lida na semeadura e na mortalidade mensal. O loader
// tolera arquivo ausente (o GDScript entrega {}) e cai na âncora de renda /
// kHealthAccessDefault quando a nação não tem entrada.
// ---------------------------------------------------------------------------

void EconomySim::set_informality(const Variant& data) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	if (data.get_type() != Variant::DICTIONARY) {
		return;
	}
	const Dictionary root = data;
	// O arquivo usa `rates` (tag -> fração 0..1); `nations` é aceito por
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
// B5 (D-089) — NÍVEL INICIAL DE INFRAESTRUTURA DO ESTADO
// (data/definitions/scenario/state_infrastructure_2026.json, World Bank).
//
// O GDScript aplica a regra %->nível (core/economy/initial_infra.gd) e entrega
// `{state_id: {road, power, water}}`. Aqui só se resolve o estado -> região:
// região com vários estados usa o MENOR nível de cada tipo (pior caso, a mesma
// lógica rural do dado) e estado sem entrada vale 0 (ausência explícita). Os
// níveis ficam no Region e são persistidos pelo save v21+ (o formato não muda).
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
			continue;  // save v23 manda; sem região não há B6
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
// F8 — SERVIÇOS
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
	// Resolve os serviços intermediários depois de todos os ids existirem.
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

// A5: a lógica dos aplicadores vive em sim/actions.cpp (C++ puro); aqui só a
// borda — converte o resultado em `Dictionary` escrevendo as MESMAS chaves, nas
// mesmas condições do original.
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
	// "project_id" só existe quando a expansão virou projeto físico.
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

Dictionary EconomySim::apply_build(const String& city_id, const String& building_id,
		const String& resource, int input_product, int output_product,
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
	// §19: sinaliza à IA que a obra ficou pendente por falta de zona (só true).
	if (outcome.zone_pending) {
		result["zone_pending"] = true;
	}
	// "project_id" só existe quando a obra virou projeto físico.
	if (!outcome.project_id.empty()) {
		result["project_id"] = String(outcome.project_id.c_str());
	}
	return result;
}

// =====================================================================
// F9 — investimento e propriedade
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
	// Ordem idêntica à de get_groups (faturamento, caixa, id), sem a
	// varredura de construções: a IA precisa do caixa do dia e da vez de cada
	// grupo, não dos ativos/países. Evita 1 std::set + 1 entrada de ranking por
	// grupo a cada dia de decisão.
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
		result["error"] = "valor inválido";
		return result;
	}
	// Retira do orçamento das cidades da nação de origem, na ordem dos índices
	// (determinística). Não cria dinheiro.
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
		result["error"] = "sem orçamento na nação de origem";
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
	// O original só gravava "cash" nas recusas por capital insuficiente do
	// próprio grupo (caminho instantâneo e caminho de projeto).
	if (outcome.error == "capital insuficiente") {
		result["cash"] = outcome.cash;
	}
	// "owner" e "local_share" só nas duas conclusões (instantânea e projeto).
	if (outcome.created) {
		result["owner"] = String(outcome.owner.c_str());
		result["local_share"] = outcome.local_share;
	}
	// "project_id" só existe quando a obra virou projeto físico.
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
		result["error"] = "modo inválido";
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
		result["error"] = "nação vazia";
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
		result["warning"] = "nenhum orçamento disponível na nação";
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
		result["error"] = "ativo não é estatal";
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
	// D1: o ativo privatizado é propriedade plena do comprador, não concessão —
	// limpar o royalty evita renda residual (conceder -> estatizar -> privatizar
	// deixava 5% e o ativo aparecia como concessão no status).
	building->royalty = 0.0;
	building->royalty_paid = 0.0;
	result["ok"] = true;
	result["price"] = price;
	result["owner"] = String(building->owner.c_str());
	return result;
}

// B5: conceder mina/fazenda do Estado a um grupo (nacional/estrangeiro) com um
// royalty sobre a produção. Igual à privatização na transferência de dinheiro
// (o comprador paga o valor contábil ao governo), mas o Estado mantém uma renda
// de royalty (`building.royalty`). O dono F9 é o grupo concessionário — sem
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
		result["error"] = "ativo já é estatal";
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
	// D1: estatizar extingue a concessão — o Estado não paga royalty a si mesmo
	// nem o ativo pode seguir publicado como concessão.
	building->royalty = 0.0;
	building->royalty_paid = 0.0;
	// Derruba a confiança do anfitrião por um período: o IDE novo fica
	// bloqueado enquanto a confiança estiver abaixo de 0,5.
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
	// Confiança: só as nações com penalidade registrada; as demais valem 1,0
	// (o consumidor usa o default). Sem varredura de construções.
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
	// Serviço doméstico (prospecção, estudos, licenciamento).
	city->service_cash += amount;
	city->service_revenue += amount;
	city->service_profit += amount;
	city->gdp += amount;
	return true;
}

Dictionary EconomySim::to_dict() const {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	return ogs::edge::to_dict(ws_, sim_core_);
}

void EconomySim::set_urban(const Dictionary& urban_land,
		const Dictionary& housing_city, const Dictionary& housing,
		const Dictionary& municipal_finance) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	if (ws_.cities.empty()) {
		return;  // mundo ainda não criado: sem id de cidade não há o que casar
	}
	const Dictionary land_cities = dict_value(urban_land, "cities");
	const Dictionary housing_cities = dict_value(housing_city, "cities");
	const Dictionary nation_housing = dict_value(housing, "nations");
	const Dictionary nation_finance = dict_value(
		municipal_finance, "nations");
	const std::size_t count = ws_.cities.size();
	// Vetores temporários: só substituem o estado urbano se ALGUMA cidade casar
	// com os dados. Cenário sintético (AGRIA/PORTUS) e load sem os JSON mantêm
	// os vetores vazios — ausência explícita, sem linhas de zeros no save.
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
		// Infraestrutura atual atende a população de simulação da cidade; sem
		// dado de cobertura, é o ponto de partida honesto (expansão no W2).
		land.infra_capacity = safe_nonneg(city.simulation_population);
		// §19 (integração): base do custo crescente — raio e reserva no
		// carregamento. Área inicial = área construída da fonte (sem a
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
		// A fonte divide o estoque por PADRÃO, não por densidade. A densidade
		// inicial segue a capacidade física permitida (FAR) até o gabarito da
		// cidade: um município baixo fica 100% em casas; um alto reparte entre
		// casa/prédio/torre na proporção da capacidade. Derivado, não medido.
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
		// Posse por estrato: tenure_by_tier da cidade; sem a linha, o padrão
		// nacional de housing.json (casa própria + favela; aluguel = resto).
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
		// Donos: casa própria da fonte = famílias; o restante do estoque é
		// atribuído a empresas até W2/W3 criarem moradia social (governo).
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
		// Aluguel/preço: a fonte publica USD/m². O câmbio do jogo é a âncora
		// XGS, referenciada no dólar (currencies.json: USD = 1,0) e a fonte
		// deriva o USD da renda PPP — o valor já está em âncora, 1 USD = 1 XGS.
		// A taxa da nação entra na EXIBIÇÃO local (price_local = XGS ÷ taxa);
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
		// Área informal derivada das unidades informais: sem FAR para favela,
		// uma unidade ocupa o m² de piso do padrão popular baixo.
		land.informal_km2 = house.informal_units
			* kUrbanUnitM2[DENSITY_LOW][TIER_POPULAR] / 1.0e6;
		// §19 W1 / Q-015-C: capacidade da MORADIA no dia 0. O envelope do
		// lote (área × FAR ÷ m²) é 5-15× o estoque real: na escala da cidade
		// a maior parte do solo é rua, pátio, uso misto e construção antiga.
		// `zone_fill` guarda a fração do envelope que corresponde ao estoque
		// FORMAL (a unidade informal mora em informal_km2, fora da zona) para
		// a ocupação nascer no alvo da faixa de população urbana
		// (kUrbanBandOccupancy). Sem isso a cidade nasce com 5-15× de
		// folga: o preço da terra não reage e a prefeitura nunca precisa de
		// solo novo. COM/IND ficam em 1,0 (emprego modelado, outra escala).
		land.zone_fill[ZONE_RES] = 1.0;
		const double physical_res = urban_land::zone_capacity(land, ZONE_RES);
		const double formal_units = std::max(0.0, stock_total - house.informal_units);
		const double urban_population = safe_nonneg(
			static_cast<double>(u.get("urban_population", 0.0)));
		// Alvo por FAIXA de população urbana (a mesma que o relatório de
		// auditoria agrupa): metrópole quase cheia, cidade média com folga
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
			// A fonte não publica a alíquota municipal: usa-se o peso do IPTU
			// no total de tributos como proxy da taxa anual sobre o valor do
			// estoque (calibração real fica para o W2).
			gov.iptu_rate = std::clamp(
				safe_nonneg(static_cast<double>(
					fin.get("property_tax_pct_total_tax", 0.0))) / 100.0,
				0.0, 0.20);
			gov.transfer_share = safe_unit(
				static_cast<double>(fin.get(
					"central_transfers_pct_subnational", 0.0)) / 100.0);
			// §19 W2: alvo da receita municipal do mês = receita subnacional
			// observada/proxy da fonte (% do PIB) × PIB municipal do mês. Sem o
			// campo (no_data), fica 0: a prefeitura não inventa nível de receita.
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
	// §19 (integração): PIB diário e população, usados pela calibração do custo
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
	// §19 (integração): base do custo crescente e tamanho atual do solo, para
	// a UI/teste conferirem o fator de distância/relevo.
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
	// B2: quintis (população, renda disponível, aluguel pago/faturado, posse
	// alugada e nível de consumo). O probe urbano de 365 d calcula ônus e
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
	// B3: região da cidade (estados inteiros) — campos leves para o probe e a
	// ficha. Cidade sem dado publica 0/vazio (ausência explícita).
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
		// B4: acumuladores de migração e a urbanização corrente do bloco.
		out["region_migration_rural_in"] = region.migration_rural_in_total;
		out["region_migration_city_in"] = region.migration_city_in_total;
		out["region_migration_city_out"] = region.migration_city_out_total;
		out["region_urban_share"] = region.population_region > 0.0
			? region.urban_population / region.population_region : 0.0;
	}
	// §19 W3: sinal de aperto por cidade (W4/UI) e fluxo do dia/mês.
	out["housing_stress"] = house.housing_stress;
	out["rent_paid"] = house.rent_paid_today;
	out["rent_paid_month"] = house.rent_paid_month;
	// Repasse do aluguel aos donos no mês (família → renda de capital; empresa
	// → caixa das firmas; prefeitura → caixa municipal) e a manutenção.
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
	// §19 W2: calibração do repasse, repasse pago por esta cidade e a obra em
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
	// §19 (W1): capacidade, uso e ocupação de cada zona [res, com, ind] e o
	// sinal de demanda barrada (insumo da IA da prefeitura no W2).
	Array zone_capacity;
	Array zone_usage;
	Array zone_occupancy;
	Array zone_demand;
	for (int zone = 0; zone < ZONE_COUNT; ++zone) {
		// Capacidade EFETIVA (com piso no uso): é a que a ocupação usa, então
		// `zone_usage/zone_capacity` nunca passa de 1 por inconsistência de
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
	// Custo LÍQUIDO: a parcela de TERRA voltou para o caixa da prefeitura no
	// momento da abertura (o resto foi reservado na obra).
	out["cost"] = std::max(0.0, cash_before - ws_.city_gov[index].cash);
	return out;
}

void EconomySim::from_dict(const Dictionary& data) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	// O save substitui o mundo: o cache de status (dicionários reaproveitados)
	// não pode sobreviver a ele.
	status_slots_[0] = StatusCache{};
	status_slots_[1] = StatusCache{};
	status_live_ = -1;
	status_built_day_ = -1;
	++status_static_version_;
	++status_deposit_version_;
	// A3: save carregado não herda a fila nem o log do mundo anterior.
	commands_.reset();
	current_day_.store(0, std::memory_order_relaxed);
	ogs::edge::from_dict(ws_, sim_core_, data);
}

Dictionary EconomySim::to_snapshot() const {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	// Mesmo lock de to_dict: o snapshot tem de ver mundo + definições do
	// MESMO dia (nenhum tick no meio).
	Dictionary snapshot;
	snapshot["definitions"] =
		ogs::edge::to_godot(ogs::save::definitions_to(ws_));
	snapshot["save"] = ogs::edge::to_dict(ws_, sim_core_);
	return snapshot;
}

void EconomySim::from_snapshot(const Dictionary& snapshot) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const Variant definitions = snapshot.get("definitions", Variant());
	const Variant save_block = snapshot.get("save", Variant());
	if (definitions.get_type() != Variant::DICTIONARY) {
		UtilityFunctions::push_error(
			"economia.snapshot: bloco 'definitions' ausente ou inválido.");
		return;
	}
	if (save_block.get_type() != Variant::DICTIONARY) {
		UtilityFunctions::push_error(
			"economia.snapshot: bloco 'save' ausente ou inválido.");
		return;
	}
	// O snapshot substitui o mundo inteiro: o cache de status (dicionários
	// reaproveitados) não pode sobreviver a ele (mesmo caminho de
	// from_dict).
	status_slots_[0] = StatusCache{};
	status_slots_[1] = StatusCache{};
	status_live_ = -1;
	status_built_day_ = -1;
	++status_static_version_;
	++status_deposit_version_;
	// A3: snapshot carregado não herda a fila nem o log do mundo anterior.
	commands_.reset();
	// A5: o snapshot substitui o mundo inteiro — o estado da IA também não
	// sobrevive (diferente de from_dict, que é load de save: ver o comentário
	// de AiPhase::reset).
	ai_phase_.reset();
	current_day_.store(0, std::memory_order_relaxed);
	// Definições primeiro: o save assume produtos/receitas/necessidades já
	// carregados (índices incluídos). A conversão do Variant é feita UMA vez e
	// reaproveitada para o teto agrícola, que só entra depois do save.
	const ogs::save::Value definition_data = ogs::edge::from_godot(definitions);
	ogs::save::definitions_from(ws_, definition_data);
	ogs::edge::from_dict(ws_, sim_core_,
		static_cast<Dictionary>(save_block));
	// `land_capacity` é dado de geografia e só pode entrar depois das cidades:
	// `definitions_from` rodou com o mundo vazio e ignorou os itens.
	ogs::save::apply_land_capacity(ws_, sim_core_, definition_data);
}

bool EconomySim::to_snapshot_file(const String& path) const {
	// Mesmo lock recursivo de to_snapshot: o snapshot tem de ver mundo +
	// definições do MESMO dia (nenhum tick no meio).
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	ogs::save::Value snapshot = ogs::save::Value::make_object();
	snapshot["definitions"] = ogs::save::definitions_to(ws_);
	snapshot["save"] = ogs::save::to_value(ws_, sim_core_);
	// A serialização é a da BIBLIOTECA (nlohmann via dump_json), não a do
	// Godot: é o único caminho com round-trip IEEE-exato para |x| < 1.
	const std::string text = ogs::save::dump_json(snapshot, 2);
	const std::string file_path(path.utf8().get_data());
	// Binário e truncado: UTF-8 cru, sem BOM e sem tradução de fim de linha.
	std::ofstream file(file_path, std::ios::binary | std::ios::trunc);
	if (!file) {
		UtilityFunctions::push_error(
			"economia.snapshot: não foi possível abrir para escrita: "
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
			"economia.snapshot: snapshot ilegível (" + String(file_path.c_str())
			+ "): " + String(error.c_str()));
		return false;
	}
	if (snapshot.get_type() != ogs::save::Value::DICTIONARY) {
		UtilityFunctions::push_error(
			"economia.snapshot: o snapshot não é um objeto JSON: "
			+ String(file_path.c_str()));
		return false;
	}
	const ogs::save::Value& definitions = snapshot["definitions"];
	const ogs::save::Value& save_block = snapshot["save"];
	if (definitions.get_type() != ogs::save::Value::DICTIONARY) {
		UtilityFunctions::push_error(
			"economia.snapshot: bloco 'definitions' ausente ou inválido.");
		return false;
	}
	if (save_block.get_type() != ogs::save::Value::DICTIONARY) {
		UtilityFunctions::push_error(
			"economia.snapshot: bloco 'save' ausente ou inválido.");
		return false;
	}
	// O snapshot substitui o mundo inteiro: o cache de status não sobrevive
	// (mesmo caminho de from_snapshot).
	status_slots_[0] = StatusCache{};
	status_slots_[1] = StatusCache{};
	status_live_ = -1;
	status_built_day_ = -1;
	++status_static_version_;
	++status_deposit_version_;
	// A3: snapshot carregado não herda a fila nem o log do mundo anterior.
	commands_.reset();
	// A5: mesmo motivo de from_snapshot — snapshot é mundo inteiro novo.
	ai_phase_.reset();
	current_day_.store(0, std::memory_order_relaxed);
	// Definições primeiro: o save assume produtos/receitas/necessidades já
	// carregados (índices incluídos). `save::from_value` é exatamente o que
	// `edge::from_dict` chama: nada de Variant no meio do caminho.
	ogs::save::definitions_from(ws_, definitions);
	ogs::save::from_value(ws_, sim_core_, save_block);
	// Só agora as cidades existem: o teto agrícola do bloco estático entra no
	// núcleo depois do estado dinâmico (mesmo motivo de from_snapshot).
	ogs::save::apply_land_capacity(ws_, sim_core_, definitions);
	return true;
}

// ---------------------------------------------------------------------------
// A3 - aplicação de um comando já tirado da fila. Roda sempre sob o mutex do
// escritor; não enfileira nada (os `apply_*` são os corpos antigos).
// ---------------------------------------------------------------------------

namespace {

std::string to_std(const String& value) {
	return std::string(value.utf8().get_data());
}

} // namespace

Dictionary EconomySim::apply_one_locked(const ogs::Command& cmd) {
	Dictionary result;
	switch (cmd.type) {
		case ogs::CommandType::Invest:
			return apply_invest(String(cmd.s[0].c_str()), String(cmd.s[1].c_str()),
				cmd.v[0]);
		case ogs::CommandType::Build:
			return apply_build(String(cmd.s[0].c_str()), String(cmd.s[1].c_str()),
				String(cmd.s[2].c_str()), cmd.i[0], cmd.i[1], cmd.v[0], cmd.v[1],
				cmd.v[2], cmd.v[3], cmd.v[4], String(cmd.s[3].c_str()), cmd.v[5],
				String(cmd.s[4].c_str()));
		case ogs::CommandType::SpendGovernment:
			result["ok"] = apply_spend_government(String(cmd.s[0].c_str()), cmd.v[0]);
			return result;
		case ogs::CommandType::PriceShock:
			apply_price_shock(String(cmd.s[0].c_str()), cmd.v[0]);
			return result;
		case ogs::CommandType::SetSuitability:
			apply_set_suitability(String(cmd.s[0].c_str()),
				String(cmd.s[1].c_str()), cmd.v[0]);
			return result;
		case ogs::CommandType::SetDeposit:
			result["ok"] = apply_set_deposit(String(cmd.s[0].c_str()),
				String(cmd.s[1].c_str()), cmd.v[0], cmd.v[1], cmd.v[2]);
			return result;
		case ogs::CommandType::SetExternalDemand:
			apply_set_external_demand(String(cmd.s[0].c_str()),
				String(cmd.s[1].c_str()), cmd.v[0]);
			return result;
		case ogs::CommandType::SetExternalIncomeIndex:
			apply_set_external_income_index(String(cmd.s[0].c_str()), cmd.v[0]);
			return result;
		case ogs::CommandType::SetProductivityIndex:
			apply_set_productivity_index(String(cmd.s[0].c_str()), cmd.v[0]);
			return result;
		case ogs::CommandType::SetIncomeGini:
			apply_set_income_gini(String(cmd.s[0].c_str()), cmd.v[0]);
			return result;
		case ogs::CommandType::SetMinimumWage:
			apply_set_minimum_wage(String(cmd.s[0].c_str()), cmd.v[0]);
			return result;
		case ogs::CommandType::SetWageRigidity:
			apply_set_wage_rigidity(cmd.v[0]);
			return result;
		case ogs::CommandType::SetTariff:
			apply_set_tariff(String(cmd.s[0].c_str()), String(cmd.s[1].c_str()),
				String(cmd.s[2].c_str()), cmd.v[0]);
			return result;
		case ogs::CommandType::SetCurrencyRate:
			apply_set_currency_rate(String(cmd.s[0].c_str()), cmd.v[0], cmd.b,
				cmd.v[1]);
			return result;
		case ogs::CommandType::ExpandPort:
			return apply_expand_port(String(cmd.s[0].c_str()), cmd.v[0]);
		case ogs::CommandType::SetPortCapacity:
			apply_set_port_capacity(String(cmd.s[0].c_str()), cmd.v[0]);
			return result;
		case ogs::CommandType::SetMarketStock:
			apply_set_market_stock(String(cmd.s[0].c_str()),
				String(cmd.s[1].c_str()), cmd.v[0]);
			return result;
		case ogs::CommandType::SetMarketPrice:
			apply_set_market_price(String(cmd.s[0].c_str()),
				String(cmd.s[1].c_str()), cmd.v[0]);
			return result;
		case ogs::CommandType::FleetBuy:
			return apply_fleet_buy(String(cmd.s[0].c_str()),
				String(cmd.s[1].c_str()), cmd.v[0], cmd.v[1]);
		case ogs::CommandType::FundGroup:
			return apply_fund_group(String(cmd.s[0].c_str()), cmd.v[0]);
		case ogs::CommandType::GroupInvest:
			return apply_group_invest(String(cmd.s[0].c_str()),
				String(cmd.s[1].c_str()), String(cmd.s[2].c_str()),
				String(cmd.s[3].c_str()), cmd.i[0], cmd.v[0],
				String(cmd.s[4].c_str()), String(cmd.s[5].c_str()));
		case ogs::CommandType::SetFdiPolicy:
			return apply_set_fdi_policy(String(cmd.s[0].c_str()),
				String(cmd.s[1].c_str()), String(cmd.s[2].c_str()), cmd.v[0]);
		case ogs::CommandType::SetSectorIncentive:
			return apply_set_sector_incentive(String(cmd.s[0].c_str()),
				String(cmd.s[1].c_str()), cmd.v[0], cmd.v[1]);
		case ogs::CommandType::CreateStateCompany:
			return apply_create_state_company(String(cmd.s[0].c_str()),
				String(cmd.s[1].c_str()), cmd.v[0]);
		case ogs::CommandType::Privatize:
			return apply_privatize(String(cmd.s[0].c_str()),
				String(cmd.s[1].c_str()), String(cmd.s[2].c_str()));
		case ogs::CommandType::Nationalize:
			return apply_nationalize(String(cmd.s[0].c_str()),
				String(cmd.s[1].c_str()));
		case ogs::CommandType::Concede:
			return apply_concede(String(cmd.s[0].c_str()),
				String(cmd.s[1].c_str()), String(cmd.s[2].c_str()), cmd.v[0]);
		case ogs::CommandType::UrbanExpand:
			return apply_urban_expand(String(cmd.s[0].c_str()), cmd.i[0], cmd.v[0]);
		case ogs::CommandType::UrbanDensify:
			return apply_urban_densify(String(cmd.s[0].c_str()), cmd.i[0]);
		case ogs::CommandType::UrbanSocialHousing:
			return apply_urban_social_housing(String(cmd.s[0].c_str()), cmd.v[0]);
		case ogs::CommandType::SetServiceCapacity:
			apply_set_service_capacity(String(cmd.s[0].c_str()),
				String(cmd.s[1].c_str()), cmd.v[0]);
			return result;
		case ogs::CommandType::SetServiceBusinessState:
			apply_set_service_business_state(cmd.i[0], cmd.i[1], cmd.v[0]);
			return result;
		case ogs::CommandType::DeepenMine:
			return apply_deepen_mine(String(cmd.s[0].c_str()),
				String(cmd.s[1].c_str()));
		case ogs::CommandType::InfraBuild:
			return apply_infra_build(String(cmd.s[0].c_str()), cmd.i[0]);
		case ogs::CommandType::StorageBuild:
			return apply_storage_build(String(cmd.s[0].c_str()), cmd.i[0],
				cmd.i[1], cmd.v[0]);
		case ogs::CommandType::StorageTrade:
			return apply_storage_trade(String(cmd.s[0].c_str()), cmd.i[0],
				cmd.i[1], static_cast<int>(cmd.v[1]), cmd.v[0], cmd.b,
				cmd.s[1] == "relief");
		case ogs::CommandType::SetExchangeRegime: {
			// C2b2 (D-106): regime salvo + paridade/banda declaradas. Sem
			// paridade (v[0] = 0) vale o `base_rate` do dado do F5.
			Dictionary out;
			const auto found = ws_.currency_by_code.find(cmd.s[0]);
			if (found == ws_.currency_by_code.end()) {
				out["ok"] = false;
				out["error"] = "moeda desconhecida";
				return out;
			}
			Currency& currency = ws_.currencies[
				static_cast<std::size_t>(found->second)];
			// D-108(c): só o EMISSOR muda regime/padrão da própria moeda;
			// moeda de união fica travada e nação dolarizada não tem política.
			const auto info = ws_.fx_regime.find(currency.code);
			if (info != ws_.fx_regime.end()) {
				if (info->second.locked) {
					out["ok"] = false;
					out["error"] = "moeda de uniao — regras de bloco futuras";
					return out;
				}
				if (!cmd.s[2].empty()) {
					const bool no_policy = std::find(
						ws_.fx_no_policy_nations.begin(),
						ws_.fx_no_policy_nations.end(), cmd.s[2])
						!= ws_.fx_no_policy_nations.end();
					if (no_policy) {
						out["ok"] = false;
						out["error"] = "nacao sem moeda propria: sem politica "
							"cambial (dolarizada)";
						return out;
					}
					if (!info->second.issuer.empty()
							&& cmd.s[2] != info->second.issuer) {
						out["ok"] = false;
						out["error"] = "somente o emissor da moeda pode mudar "
							"o regime";
						return out;
					}
				}
			}
			const int regime = ogs::central_bank::regime_from_name(cmd.s[1],
				currency.exchange_regime);
			currency.exchange_regime = regime;
			// O toggle do F5 acompanha: o regime é a fonte, agora que ele age.
			currency.floating = regime
				== ogs::central_bank::kRegimeFloating;
			FxState& state = ws_.fx[currency.code];
			if (cmd.v[0] > 0.0) {
				state.parity = cmd.v[0];
			}
			if (cmd.v[1] >= 0.0) {
				state.band_width = std::clamp(cmd.v[1], 0.0, 1.0);
			}
			out["ok"] = true;
			out["currency"] = String(currency.code.c_str());
			out["regime"] = ogs::central_bank::regime_name(regime);
			return out;
		}
		case ogs::CommandType::SetMonetaryStandard: {
			Dictionary out;
			const int standard = ogs::central_bank::standard_from_name(
				cmd.s[0], -1);
			if (standard < 0) {
				out["ok"] = false;
				out["error"] = "padrao desconhecido";
				return out;
			}
			ws_.monetary_standard = standard;
			// O preço fixo de conversão do ouro é o de catálogo do jogo,
			// congelado no comando (não é o par histórico de 1944).
			if (standard != ogs::central_bank::kStandardFiat) {
				const int gold = sim_core_.product_index(std::string("gold"));
				const double price = gold >= 0 ? sim_core_.base_price(gold)
					: 0.0;
				for (Currency& currency : ws_.currencies) {
					FxState& state = ws_.fx[currency.code];
					if (state.conversion_price <= 0.0) {
						state.conversion_price = price;
					}
				}
			}
			out["ok"] = true;
			out["standard"] =
				ogs::central_bank::standard_name(standard);
			return out;
		}
		case ogs::CommandType::SetBankCredit: {
			Dictionary out;
			ws_.bank_credit_enabled = cmd.i[0] != 0;
			out["ok"] = true;
			out["enabled"] = ws_.bank_credit_enabled;
			return out;
		}
		case ogs::CommandType::GroupBorrow: {
			double granted = 0.0;
			std::string error;
			const bool ok = ogs::bank::lend(ws_,
				std::string(cmd.s[0].c_str()), cmd.v[0], &granted,
				&error);
			Dictionary out;
			out["ok"] = ok;
			out["granted"] = granted;
			out["group"] = String(cmd.s[0].c_str());
			if (!ok) {
				out["error"] = String(error.c_str());
			}
			return out;
		}
		case ogs::CommandType::None:
			break;
	}
	return result;
}

// ---------------------------------------------------------------------------
// A3 - superfície pública: MESMA assinatura de antes, agora enfileirando.
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
		// Comando aceito; o gasto acontece no início do próximo dia. Só
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
	// Dado de geografia que só a IA consome: entra direto no núcleo (sem fila
	// de comandos) e NÃO invalida o status publicado — nada na UI muda.
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	const int index = product_index(product);
	if (index < 0) {
		return;
	}
	sim_core_.set_land_capacity(std::string(city_id.utf8().get_data()), index,
		value);
}

// ---------------------------------------------------------------------------
// G — AGRICULTURA SAZONAL (D-096)
// ---------------------------------------------------------------------------

void EconomySim::set_crop_plan(const String& city_id, const String& product,
		double area_ha, double yield_t_ha, const Array& seasons) {
	// Dado de simulação declarado antes do dia 1 (como a geografia/estratos):
	// entra direto no núcleo, sem fila de comandos.
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
	// O índice (cidade × produto) muda com o plano: refaz agora para a
	// supressão da produção diária valer no primeiro dia.
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
// G2 — ARMAZENAGEM (D-100)
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
		result["error"] = "tipo/dono inválido";
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
		result["error"] = "tipo/dono/produto inválido";
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
	// `relief` não cabe no `b` (que já é o lado); usa o slot s[1].
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

	// Slot de cada produto na lista da cadeia (índice do livro de perdas).
	std::vector<int> slot_of(static_cast<std::size_t>(ws_.products), -1);
	for (std::size_t slot = 0; slot < ws_.agri_chain.size(); ++slot) {
		const int product = ws_.agri_chain[slot];
		if (product >= 0 && product < ws_.products) {
			slot_of[static_cast<std::size_t>(product)] =
				static_cast<int>(slot);
		}
	}
	// Slot de cada produto na cesta alimentar (consumo em espécie das famílias
	// agrícolas, que NÃO passa por `market.demand`).
	std::vector<int> food_slot_of(static_cast<std::size_t>(ws_.products), -1);
	for (std::size_t slot = 0; slot < ws_.food_products.size(); ++slot) {
		const int product = ws_.food_products[slot];
		if (product >= 0 && product < ws_.products) {
			food_slot_of[static_cast<std::size_t>(product)] =
				static_cast<int>(slot);
		}
	}

	// Acumuladores por nação (std::map: ordem determinística no dump).
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
			// Consumo em ESPÉCIE das famílias agrícolas: sai do estoque do
			// mercado da cidade sem entrar em `market.demand` (consumption.cpp).
			// Sem ele a conta da entressafra não fecha.
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
				// Produção da safra em curso: o acumulado da instância que
				// contém hoje (janela que atravessa o ano continua contando).
				const int instance = ogs::window_start_year(today.year,
					today.doy, window.harvest_start_doy);
				const int position = ogs::window_position(today.doy,
					window.harvest_start_doy, window.harvest_end_doy);
				if (state.harvest_year == today.year
						|| (position >= 0 && state.harvest_year == instance)) {
					crop.production_year_t += state.delivered_t;
				}
				// Próxima colheita: 0 quando já está colhendo; senão os dias
				// até o início da próxima instância da janela.
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
			// Dias de consumo: estoque ÷ consumo do dia. Sem consumo
			// observado não há número (-1 = sem observação).
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
	// Totais acumulados desde o dia 0, por bem da cadeia (diagnóstico do teste
	// e da UI; não entram no save).
	Dictionary totals;
	Dictionary perishable;
	Dictionary storage;
	Dictionary entry;
	// Estoque do dia por bem da cadeia (TODAS as cidades, inclusive as sem
	// plano — é o estoque que a perecibilidade e a armazenagem alcançam).
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
	// C3 (D-107): o setor bancário (banco agregado por nação) e o balanço de
	// cada grupo. Sem crédito o setor é zero e os grupos publicam dívida 0.
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
	// C3 (D-107): balanço de cada grupo (ordem = `ws.groups`, estável).
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
			// Alavancagem = dívida ÷ (patrimônio líquido + dívida); sem
			// patrimônio e sem dívida é 0 (ausência declarada, não 100%).
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

	// Por nação: as mesmas contas, restritas às cidades do país. Caixas que
	// não têm nação (fundos de obra em voo, grupos sem origem declarada) ficam
	// FORA da soma por país e aparecem em `unallocated` (nada é rateado por
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
		// O `foreign_cash` do núcleo é GLOBAL: a posição do exterior não é
		// decomposta por país (ausência declarada, não rateio inventado).
		Dictionary external;
		external["decomposed"] = false;
		external["reason"] = "foreign_cash global no núcleo";
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
		row["note"] = "caixas sem nação atribuível (leitura)";
		return row;
	}();
	out["notes"] = [&]() {
		Dictionary notes;
		notes["monetary_base"] =
			"passivo do emissor: base monetária M0 medida por moeda (C2a/D-103); "
			"a emissão é a C2b";
		notes["arrears"] =
			"salário atrasado = passivo da firma + ativo das famílias";
		notes["equity"] =
			"participação em grupos (stratum_equity) não entra: avaliação já "
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
		// D-105: o juro da taxa básica no envelope de crédito da safra (D-099)
		// está ligado? Falso = desligado até existir credor (C4). Declarado em
		// `data/definitions/economy/monetary_policy.json` e conferido pelo teste.
		policy["harvest_credit_rate"] = ws_.harvest_credit_rate;
		// C2b2 (D-106): o padrão AGE (Bretton Woods e padrão ouro forçam o peg
		// e a conversão do ouro). A troca é só por comando na fila A3.
		const ogs::exchange::Parameters params =
			ogs::exchange::parameters(ws_);
		policy["standard_forces_peg"] =
			ogs::exchange::standard_forces_peg(ws_);
		policy["band_width"] = params.band_width;
		policy["defence_share"] = params.defence_share;
		policy["gold_reserve_ratio"] = params.gold_reserve_ratio;
		policy["gold_conversion_basis"] =
			"catalogo goods.json, congelado no comando de padrao";
		policy["note"] =
			"C2b1 (D-104): taxa básica por moeda (Taylor no BC independente; "
			"objetivo do governo no controlado) e emissão só por compra de "
			"títulos do governo pelo BC controlado. Bancos/redesconto são C4.";
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
		// C2b1 (D-104): política ATIVA da moeda. `bank_independence` acima é o
		// valor SALVO (pode ser `no_data`); o efetivo resolve no default
		// declarado do jogo. A taxa, a leitura e a emissão saem do estado.
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
		// Passivo (base monetária) e agregados medidos.
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
		// eventos da moeda. `funded` falso = fixo/banda administrativo (o F5 já
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
			// taxa, quem é o emissor e se a moeda está travada (união).
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
		// C2b2 (D-106): reservas, fatia do cofre e contador de desvalorizações.
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
		// C2b2: anel de eventos do câmbio (mais recente primeiro).
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
		// C2b1: dívida pública por nação (só quem tem título do BC).
		Dictionary bonds;
		for (const auto& row : ws_.gov_bonds) {
			bonds[String(row.first.c_str())] = row.second;
		}
		out["government_bonds"] = bonds;
	}

	// CPI por país: o índice é o MESMO do IPC do G2 (uma dona só). A inflação
	// de 12 meses vem da série mensal; sem 12 meses, `has_12m` é falso —
	// ausência declarada, nunca um número inventado.
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
			"cesta de alimentos do G2 (fator calórico x preço/base x necessidade "
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
			"caixas sem nação atribuível (o exterior do núcleo é global); nada "
			"é rateado por palpite";
		out["unallocated"] = row;
	}

	{
		Dictionary notes;
		notes["banks"] =
			"sem banco (C4): reservas bancárias 0, então M1 = M2 = M0";
		notes["securities"] =
			"títulos 0: não há dívida pública no estado da C1 (C3/C4)";
		notes["reserve_currency_holdings"] =
			"0: o exterior do núcleo é um único foreign_cash global, não "
			"decomposto por moeda (C2b)";
		notes["gold"] =
			"ouro = dado real da G2/D-101 por moeda do banco central; é ativo "
			"REAL e não entra na identidade financeira";
		notes["names"] =
			"nome institucional do BC vem do dado D-090 no GDScript; o núcleo "
			"não inventa nome";
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
		// Nação → moeda (o banco central é POR MOEDA; a zona do euro e o CFA
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
	// Nome inválido não vira `no_data` por acidente: exige um dos três nomes.
	if (name != "no_data" && name != "independent" && name != "controlled") {
		return false;
	}
	ws_.currencies[static_cast<std::size_t>(found->second)]
		.bank_independence = value;
	status_invalidate();
	return true;
}

// ---------------------------------------------------------------------------
// C2b2 (D-106) — regimes de câmbio, padrão monetário e ouro físico.
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
	// D-108(c): moeda de união/dolarizada não aceita comando de regime. A
	// recusa vale já no pedido (o motivo também volta na aplicação do A3).
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
// C3 (D-107) — crédito de empresa (banco agregado como credor).
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

Dictionary EconomySim::set_bank_capital(const String& nation, double amount) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	double paid = 0.0;
	std::string error;
	ogs::bank::inject_capital(ws_, std::string(nation.utf8().get_data()),
		amount, &paid, &error);
	status_invalidate();
	Dictionary out;
	out["ok"] = paid > 0.0;
	out["paid"] = paid;
	out["nation"] = nation;
	if (paid <= 0.0) {
		out["error"] = String(error.c_str());
	}
	return out;
}

Dictionary EconomySim::group_borrow(const String& group_id, double amount) {
	ogs::Command cmd;
	cmd.type = ogs::CommandType::GroupBorrow;
	cmd.s[0] = to_std(group_id);
	cmd.v[0] = amount;
	return dispatch_command(cmd);
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
		// Ausências declaradas desta fase (C4/C6), não números inventados.
		policy["deposits"] = "absent";
		policy["deposits_owner"] = "C4";
		policy["resolution"] = "C6";
		policy["capital_source"] = "treasury (state owner until C4)";
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
		out["totals"] = row;
	}
	// Uma linha por banco (o mapa é ordenado por nação: ordem determinística).
	Array nations;
	for (const auto& row : ws_.banks) {
		const ogs::Bank& bank = row.second;
		Dictionary entry;
		entry["nation"] = String(bank.nation.c_str());
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
		// Razão de capital: patrimônio ÷ empréstimo em aberto (negativa =
		// insolvente; a resolução é C6).
		entry["equity_ratio"] = (bank.loans + bank.arrears) > 1e-9
			? ogs::bank::equity(bank) / (bank.loans + bank.arrears) : 0.0;
		entry["writeoffs"] = bank.writeoffs;
		entry["defaults"] = bank.defaults;
		entry["loans_count"] = bank.loans_count;
		entry["last_default_day"] = bank.last_default_day;
		nations.append(entry);
	}
	out["nations"] = nations;
	// Só os grupos com posição de crédito (a lista inteira é o
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
	// dias de consumo cobertos, IPC e preço médio ponderados pela cesta dos
	// quintis. NÃO existe mercado nacional — só a leitura agregada.
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

	// Pesos da cesta: fatores calóricos do dado (needs.json). Sem cesta
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
		// população de simulação pela necessidade per capita.
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
		// aqui só acumula a demanda e o preço médio por produto.
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

	// IPC por nação: a MESMA função que o banco central publica (C2a/D-103).
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
