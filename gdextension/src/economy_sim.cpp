#include "economy_sim.h"
#include "edge/save_load.h"
#include "sim/profiling.h"
#include "sim/sim_constants.h"
#include "sim/scenario_fixture.h"

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
#include <map>
#include <set>
#include <utility>

namespace godot {

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

// §15: custo ainda não pago das obras de uma cidade para um dono ("state",
// "private" ou o id de um grupo). Base do limite de comprometimento: o dono não
// inicia obra nova cujo custo, somado ao já comprometido, estoura o caixa atual.
inline double demo_committed_construction(const WorldState& ws,
		const std::string& city_id, const std::string& owner) {
	double committed = 0.0;
	for (const DemoConstructionProject& project : ws.construction_projects) {
		if (project.city != city_id) {
			continue;
		}
		const bool match = owner == "state" ? project.owner == "state"
			: owner == "private" ? project.owner == "private"
			: !project.group_id.empty() && project.group_id == owner;
		if (match) {
			committed += std::max(0.0, project.total_cost - project.paid);
		}
	}
	return committed;
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
	// sobrescreve ambos com goods.json/needs.json via demo_set_goods/needs.
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
		ws_.food_products.push_back(kDemoFoodProductIds[index]);
		ws_.food_factors.push_back(kDemoCaloricFactor[index]);
	}
	// F3: sem goods.json carregado nenhum dos 21 primários é durável.
	ws_.product_durable.assign(kDemoMaxProducts, 0);
}

EconomySim::~EconomySim() {
	// Se o sim for liberado com um dia AINDA rodando na thread do pool (o jogo
	// encerrando, por exemplo), espera o tick sair antes de destruir os
	// contêineres do status: destruí-los debaixo de quem os escreve era um
	// access violation (0xC0000005) intermitente no fim do processo. O mutex é
	// recursivo, então a mesma thread do tick não bloqueia aqui.
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
}

void EconomySim::_bind_methods() {
	ClassDB::bind_method(D_METHOD("initialize_demo"), &EconomySim::initialize_demo);
	ClassDB::bind_method(D_METHOD("initialize_demo_world", "cities", "ports", "buildings"),
	                     &EconomySim::initialize_demo_world);
	ClassDB::bind_method(D_METHOD("tick_demo"), &EconomySim::tick_demo);
	ClassDB::bind_method(D_METHOD("demo_tick_and_publish"), &EconomySim::demo_tick_and_publish);
	ClassDB::bind_method(D_METHOD("demo_set_profiling", "enabled"),
	                     &EconomySim::demo_set_profiling);
	ClassDB::bind_method(D_METHOD("demo_set_sim_threads", "threads"),
	                     &EconomySim::demo_set_sim_threads);
	ClassDB::bind_method(D_METHOD("demo_get_profile"), &EconomySim::demo_get_profile);
	ClassDB::bind_method(D_METHOD("demo_warm_status"), &EconomySim::demo_warm_status);
	ClassDB::bind_method(D_METHOD("get_demo_status"), &EconomySim::get_demo_status);
	ClassDB::bind_method(D_METHOD("get_city_status", "city_id"),
	                     &EconomySim::get_city_status);
	ClassDB::bind_method(D_METHOD("demo_price_shock", "product", "multiplier"),
	                     &EconomySim::demo_price_shock);
	ClassDB::bind_method(D_METHOD("demo_set_suitability", "city_id", "product", "value"),
	                     &EconomySim::demo_set_suitability);
	ClassDB::bind_method(D_METHOD("demo_set_deposit", "city_id", "product", "reserve_kt",
	                     "quality", "depth_m"),
	                     &EconomySim::demo_set_deposit);
	ClassDB::bind_method(D_METHOD("demo_set_external_demand", "city_id", "product",
	                     "ceiling_t_per_day"),
	                     &EconomySim::demo_set_external_demand);
	ClassDB::bind_method(D_METHOD("demo_set_external_income_index", "city_id", "index"),
	                     &EconomySim::demo_set_external_income_index);
	ClassDB::bind_method(D_METHOD("demo_set_productivity_index", "city_id", "index"),
	                     &EconomySim::demo_set_productivity_index);
	ClassDB::bind_method(D_METHOD("demo_set_income_gini", "city_id", "gini"),
	                     &EconomySim::demo_set_income_gini);
	ClassDB::bind_method(D_METHOD("demo_set_labor_profile", "city_id", "profile"),
	                     &EconomySim::demo_set_labor_profile);
	ClassDB::bind_method(D_METHOD("demo_set_minimum_wage", "nation", "value"),
	                     &EconomySim::demo_set_minimum_wage);
	ClassDB::bind_method(D_METHOD("demo_set_wage_rigidity", "value"),
	                     &EconomySim::demo_set_wage_rigidity);
	ClassDB::bind_method(D_METHOD("demo_set_goods", "goods"),
	                     &EconomySim::demo_set_goods);
	ClassDB::bind_method(D_METHOD("demo_set_recipes", "recipes"),
	                     &EconomySim::demo_set_recipes);
	ClassDB::bind_method(D_METHOD("demo_set_needs", "needs"),
	                     &EconomySim::demo_set_needs);
	ClassDB::bind_method(D_METHOD("demo_set_services", "services"),
	                     &EconomySim::demo_set_services);
	ClassDB::bind_method(D_METHOD("demo_set_sector_employment", "data"),
	                     &EconomySim::demo_set_sector_employment);
	ClassDB::bind_method(D_METHOD("demo_set_instant_construction", "instant"),
	                     &EconomySim::demo_set_instant_construction);
	ClassDB::bind_method(D_METHOD("demo_get_service_sectors"),
	                     &EconomySim::demo_get_service_sectors);
	ClassDB::bind_method(D_METHOD("demo_get_services_detail", "city_id"),
	                     &EconomySim::demo_get_services_detail);
	ClassDB::bind_method(D_METHOD("demo_set_service_capacity", "city_id", "sector",
	                     "units_per_day"),
	                     &EconomySim::demo_set_service_capacity);
	ClassDB::bind_method(D_METHOD("demo_set_service_business_state", "business_id",
	                     "loss_days", "profit"),
	                     &EconomySim::demo_set_service_business_state);
	ClassDB::bind_method(D_METHOD("demo_get_service_business_count"),
	                     &EconomySim::demo_get_service_business_count);
	ClassDB::bind_method(D_METHOD("demo_get_service_bankruptcies"),
	                     &EconomySim::demo_get_service_bankruptcies);
	ClassDB::bind_method(D_METHOD("demo_seed_industry"),
	                     &EconomySim::demo_seed_industry);
	ClassDB::bind_method(D_METHOD("demo_invest", "city_id", "building_id", "amount"),
	                     &EconomySim::demo_invest);
	ClassDB::bind_method(D_METHOD("demo_build", "city_id", "building_id", "resource",
	                     "input_product", "output_product", "capacity", "jobs", "wage",
	                     "opex", "input_per_output", "owner", "cost", "recipe_id"),
	                     &EconomySim::demo_build, DEFVAL(""));
	ClassDB::bind_method(D_METHOD("demo_spend_government", "city_id", "amount"),
	                     &EconomySim::demo_spend_government);
	ClassDB::bind_method(D_METHOD("demo_expand_port", "port_id", "t_per_day"),
	                     &EconomySim::demo_expand_port);
	ClassDB::bind_method(D_METHOD("demo_set_port_capacity", "port_id", "t_per_day"),
	                     &EconomySim::demo_set_port_capacity);
	ClassDB::bind_method(D_METHOD("demo_to_dict"), &EconomySim::demo_to_dict);
	ClassDB::bind_method(D_METHOD("demo_from_dict", "data"), &EconomySim::demo_from_dict);
	ClassDB::bind_method(D_METHOD("demo_set_balance", "values"),
	                     &EconomySim::demo_set_balance);
	ClassDB::bind_method(D_METHOD("demo_set_fleet", "nation", "trucks", "ships",
	                     "planes", "fuel", "spare_parts"),
	                     &EconomySim::demo_set_fleet);
	ClassDB::bind_method(D_METHOD("demo_get_fleet", "nation"),
	                     &EconomySim::demo_get_fleet);
	ClassDB::bind_method(D_METHOD("demo_fleet_buy", "nation", "product", "amount",
	                     "budget_share"),
	                     &EconomySim::demo_fleet_buy, DEFVAL(0.1));
	ClassDB::bind_method(D_METHOD("demo_set_market_stock", "city_id", "product", "stock"),
	                     &EconomySim::demo_set_market_stock);
	ClassDB::bind_method(D_METHOD("demo_set_market_price", "city_id", "product", "price"),
	                     &EconomySim::demo_set_market_price);
	ClassDB::bind_method(D_METHOD("demo_set_currencies", "currencies"),
	                     &EconomySim::demo_set_currencies);
	ClassDB::bind_method(D_METHOD("demo_get_currency", "nation"),
	                     &EconomySim::demo_get_currency);
	ClassDB::bind_method(D_METHOD("demo_get_currencies"),
	                     &EconomySim::demo_get_currencies);
	ClassDB::bind_method(D_METHOD("demo_set_currency_rate", "code", "rate", "floating",
	                     "base_rate"),
	                     &EconomySim::demo_set_currency_rate, DEFVAL(true), DEFVAL(-1.0));
	ClassDB::bind_method(D_METHOD("demo_set_tariff", "buyer_nation", "seller_nation",
	                     "product", "rate"),
	                     &EconomySim::demo_set_tariff);
	ClassDB::bind_method(D_METHOD("demo_get_tariff", "buyer_nation", "seller_nation",
	                     "product"),
	                     &EconomySim::demo_get_tariff);
	ClassDB::bind_method(D_METHOD("demo_get_tariffs"),
	                     &EconomySim::demo_get_tariffs);
	ClassDB::bind_method(D_METHOD("demo_seed_groups"), &EconomySim::demo_seed_groups);
	ClassDB::bind_method(D_METHOD("demo_set_group_names", "names"),
	                     &EconomySim::demo_set_group_names);
	ClassDB::bind_method(D_METHOD("demo_get_groups"), &EconomySim::demo_get_groups);
	ClassDB::bind_method(D_METHOD("demo_get_group_ranking"),
	                     &EconomySim::demo_get_group_ranking);
	ClassDB::bind_method(D_METHOD("demo_get_group", "group_id"),
	                     &EconomySim::demo_get_group);
	ClassDB::bind_method(D_METHOD("demo_fund_group", "group_id", "amount"),
	                     &EconomySim::demo_fund_group);
	ClassDB::bind_method(D_METHOD("demo_group_invest", "group_id", "city_id",
	                     "building_id", "resource", "output_product", "capacity",
	                     "recipe_id", "partner_group"),
	                     &EconomySim::demo_group_invest, DEFVAL(""));
	ClassDB::bind_method(D_METHOD("demo_set_fdi_policy", "nation", "sector", "mode",
	                     "min_local"),
	                     &EconomySim::demo_set_fdi_policy, DEFVAL(0.0));
	ClassDB::bind_method(D_METHOD("demo_set_sector_incentive", "nation", "sector",
	                     "tax_holiday", "subsidy"),
	                     &EconomySim::demo_set_sector_incentive);
	ClassDB::bind_method(D_METHOD("demo_create_state_company", "nation", "name",
	                     "capital"),
	                     &EconomySim::demo_create_state_company);
	ClassDB::bind_method(D_METHOD("demo_privatize", "city_id", "building_id",
	                     "buyer_group"),
	                     &EconomySim::demo_privatize);
	ClassDB::bind_method(D_METHOD("demo_nationalize", "city_id", "building_id"),
	                     &EconomySim::demo_nationalize);
	ClassDB::bind_method(D_METHOD("demo_get_investment_status"),
	                     &EconomySim::demo_get_investment_status);
	ClassDB::bind_method(D_METHOD("demo_get_investment_signals"),
	                     &EconomySim::demo_get_investment_signals);
	ClassDB::bind_method(D_METHOD("demo_groups_seeded"),
	                     &EconomySim::demo_groups_seeded);
}

void EconomySim::demo_fill_leg_status(Dictionary& dict, const Leg& leg) const {
	const ModeSpec& mode_spec = spec(leg.mode);
	const DemoStatusKeys& k = demo_status_keys_;
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

const ogs::DemoCurrency* EconomySim::demo_currency_for(
		const std::string& nation) const {
	return sim_core_.demo_currency_for(nation);
}

ogs::DemoCurrency* EconomySim::demo_currency_for(
		const std::string& nation) {
	const auto found = ws_.nation_currency.find(nation);
	if (found == ws_.nation_currency.end()
			|| found->second < 0
			|| found->second >= static_cast<int>(ws_.currencies.size())) {
		return nullptr;
	}
	return &ws_.currencies[static_cast<std::size_t>(found->second)];
}

double EconomySim::demo_competitiveness(const std::string& nation) const {
	return sim_core_.demo_competitiveness(nation);
}

void EconomySim::demo_rebuild_tariff_lookup() {
	sim_core_.demo_rebuild_tariff_lookup();
}

double EconomySim::demo_tariff_rate(const std::string& buyer_nation,
		const std::string& seller_nation, int product) const {
	return sim_core_.demo_tariff_rate(buyer_nation, seller_nation, product);
}

Array EconomySim::demo_get_tariffs() const {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	Array out;
	out.resize(static_cast<int>(ws_.tariffs.size()));
	for (std::size_t index = 0; index < ws_.tariffs.size(); ++index) {
		const DemoTariff& tariff = ws_.tariffs[index];
		Dictionary entry;
		entry["buyer"] = String(tariff.buyer.c_str());
		entry["seller"] = String(tariff.seller.c_str());
		entry["product"] = String(tariff.product.c_str());
		entry["rate"] = tariff.rate;
		out[static_cast<int>(index)] = entry;
	}
	return out;
}

double EconomySim::demo_get_tariff(const String& buyer_nation,
		const String& seller_nation, const String& product) const {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	const int index = product == "*" ? -1 : demo_product_index(product);
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
	return demo_tariff_rate(std::string(buyer_nation.utf8().get_data()),
		std::string(seller_nation.utf8().get_data()), index);
}

void EconomySim::demo_set_tariff(const String& buyer_nation,
		const String& seller_nation, const String& product, double rate) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	DemoTariff tariff;
	tariff.buyer = std::string(buyer_nation.utf8().get_data());
	tariff.seller = seller_nation.is_empty() ? "*"
		: std::string(seller_nation.utf8().get_data());
	tariff.product = product.is_empty() ? "*"
		: std::string(product.utf8().get_data());
	tariff.rate = std::clamp(std::isfinite(rate) ? rate : 0.0, 0.0, 1.0);
	const std::string key = tariff.buyer + '|' + tariff.seller + '|'
		+ tariff.product;
	for (DemoTariff& existing : ws_.tariffs) {
		if (existing.buyer == tariff.buyer && existing.seller == tariff.seller
				&& existing.product == tariff.product) {
			existing.rate = tariff.rate;
			ws_.tariff_lookup[key] = tariff.rate;
			demo_status_invalidate();
			return;
		}
	}
	ws_.tariffs.push_back(tariff);
	ws_.tariff_lookup[key] = tariff.rate;
	demo_status_invalidate();
}

Dictionary EconomySim::demo_get_currency(const String& nation) const {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	Dictionary out;
	const std::string tag(nation.utf8().get_data());
	const DemoCurrency* currency = demo_currency_for(tag);
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

Array EconomySim::demo_get_currencies() const {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	Array out;
	out.resize(static_cast<int>(ws_.currencies.size()));
	for (std::size_t index = 0; index < ws_.currencies.size(); ++index) {
		const DemoCurrency& currency = ws_.currencies[index];
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

void EconomySim::demo_set_currency_rate(const String& code, double rate,
		bool floating, double base_rate) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	const auto found = ws_.currency_by_code.find(std::string(code.utf8().get_data()));
	if (found == ws_.currency_by_code.end() || !std::isfinite(rate) || rate <= 0.0) {
		return;
	}
	DemoCurrency& currency = ws_.currencies[static_cast<std::size_t>(found->second)];
	currency.rate = rate;
	// `base_rate` fixa a referência da banda e da competitividade float. Sem
	// ele, a taxa informada vira a própria referência (comportamento anterior).
	currency.base_rate = (std::isfinite(base_rate) && base_rate > 0.0)
		? base_rate : rate;
	currency.floating = floating;
	demo_status_invalidate();
}

void EconomySim::demo_set_currencies(const Variant& currencies) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
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

	std::vector<DemoCurrency> next;
	next.reserve(static_cast<std::size_t>(list.size()));
	for (int index = 0; index < list.size(); ++index) {
		const Dictionary entry = list[index];
		const String code = String(entry.get("code", ""));
		if (code.is_empty()) {
			continue;
		}
		DemoCurrency currency;
		currency.code = std::string(code.utf8().get_data());
		currency.source = std::string(String(entry.get("source", "default")).utf8().get_data());
		const String regime = String(entry.get("regime", String(default_regime.c_str())));
		currency.floating = regime == "float";
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
	ws_.currencies = std::move(next);
	demo_rebuild_currency_indexes();

	// O preço interno está na âncora: carregar moedas NÃO reescala preços. A
	// taxa só entra na exibição local e na competitividade float.
	if (ws_.active && !ws_.cities.empty()) {
		++demo_status_static_version_;
		demo_status_invalidate();
	}
}


void EconomySim::demo_set_goods(const Array& goods) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	// Tabela de produtos: 21 primários nas posições 0..20 (ordem do goods.json
	// legado) e os bens novos em `order` (21..112). O núcleo não lê arquivos:
	// todo o conteúdo entra por aqui, no padrão do demo_set_goods antigo.
	const int previous_products = ws_.products;
	std::vector<std::string> names = ws_.product_names;
	std::vector<double> prices(kDemoMaxProducts, 0.0);
	std::vector<char> durable(kDemoMaxProducts, 0);
	std::vector<std::string> sectors = ws_.product_sectors;
	sectors.resize(kDemoMaxProducts);
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
		order = std::clamp(order, 0, kDemoMaxProducts - 1);
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
	product_count = std::min(product_count, kDemoMaxProducts);
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
		for (DemoCity& city : ws_.cities) {
			for (int product = previous_products; product < product_count; ++product) {
				city.markets[product].price = demo_base_price(product);
			}
		}
		++demo_status_static_version_;
		++demo_status_deposit_version_;
		demo_status_invalidate();
	}
	// F4: os índices dos bens da frota dependem dos ids carregados. Se o mundo
	// já existe e ainda não tem frota (demo_set_goods chamado depois da
	// inicialização, como no autoload), deriva agora.
	ws_.fleet_product_fuel = demo_product_index(String("fuel"));
	ws_.fleet_product_truck = demo_product_index(String("truck"));
	ws_.fleet_product_ship = demo_product_index(String("cargo_ship"));
	ws_.fleet_product_plane = demo_product_index(String("cargo_plane"));
	ws_.fleet_product_parts = demo_product_index(String("spare_parts"));
	if (ws_.active && ws_.fleets.empty()) {
		demo_init_fleets();
	}
}

void EconomySim::initialize_demo() {
	// Novo mundo: o cache de status pertence ao mundo anterior.
	demo_status_slots_[0] = DemoStatusCache{};
	demo_status_slots_[1] = DemoStatusCache{};
	demo_status_live_ = -1;
	demo_status_built_day_ = -1;
	++demo_status_static_version_;
	++demo_status_deposit_version_;
	// S14: o cenário AGRIA/PORTUS vive em sim/scenario_fixture.cpp (C++ puro);
	// aqui só o cache de status (estado Godot) é resetado.
	initialize_demo_fixture(ws_, sim_core_);
}

void EconomySim::initialize_demo_world(const Array& cities, const Array& ports,
		const Array& buildings) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	// Novo mundo: o cache de status pertence ao mundo anterior.
	demo_status_slots_[0] = DemoStatusCache{};
	demo_status_slots_[1] = DemoStatusCache{};
	demo_status_live_ = -1;
	demo_status_built_day_ = -1;
	++demo_status_static_version_;
	++demo_status_deposit_version_;
	ws_.active = true;
	ws_.world_mode = true;
	ws_.day = 0;
	ws_.cities.clear();
	ws_.buildings.clear();
	ws_.construction_projects.clear();
	ws_.in_transit.clear();
	ws_.invariant_violations.clear();
	ws_.ports.clear();
	ws_.last_orders.clear();
	ws_.bankruptcies = 0;
	ws_.depleted_mines = 0;
	ws_.transport_cash = 0.0;
	ws_.foreign_cash = 0.0;
	ws_.construction_cash = 0.0;
	ws_.industry_seeded = false;
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
	for (int index = 0; index < kDemoMaxProducts; ++index) {
		ws_.produced[index] = 0.0;
		ws_.consumed_total[index] = 0.0;
		ws_.produced_today[index] = 0.0;
		ws_.consumed_today[index] = 0.0;
	}

	// Cidades reais do cenário: identidade geográfica + caixas iniciais.
	for (int index = 0; index < cities.size(); ++index) {
		const Dictionary d = cities[index];
		DemoCity city;
		city.id = std::string(String(d["id"]).utf8().get_data());
		city.name = std::string(String(d["name"]).utf8().get_data());
		city.nation = std::string(String(d["nation"]).utf8().get_data());
		city.home_nation =
			std::string(String(d.get("home_nation", d["nation"])).utf8().get_data());
		city.landmass = std::string(String(d.get("landmass", "")).utf8().get_data());
		city.population = safe_nonneg(static_cast<double>(d["population"]));
		city.simulation_population = read_sim_population(d, city.population);
		city.center = {static_cast<double>(d["x"]), static_cast<double>(d["y"])};

		// Caixas iniciais por habitante (constantes do modelo demo). A escala é
		// a população de SIMULAÇÃO: a população real exibida não muda o balanço.
		city.firm_cash = city.simulation_population * kDemoFirmCashPerCapita;
		city.service_cash = city.simulation_population * kDemoServiceCashPerCapita;
		city.government_budget =
			city.simulation_population * kDemoGovernmentPerCapita;
		city.service_jobs = city.simulation_population * kDemoServiceShare;
		city.service_wage = kDemoServiceWage;
		city.pop.population = city.simulation_population;
		city.pop.savings = city.simulation_population * kDemoSavingsPerCapita;
		city.pop.desired_by_category.assign(ws_.food_products.size(), 0.0);
		city.pop.consumed_by_category.assign(ws_.food_products.size(), 0.0);
		demo_init_pop_needs(city.pop);
		// F7: perfil inicial de trabalho/educação a partir do índice de renda
		// (1,0 por padrão; o GeographyService reaplica quando tem o PIB real).
		demo_apply_labor_profile(city);
		// Aptidão padrão (inicializador do struct: suitability = {1.0, 1.0, 1.0,
		// 0, ...} — trigo/ferro/arroz em 1.0). O GeographyService sobrescreve com
		// o dado real em tempo de execução.
		city.suitability[0] = 1.0;
		for (int product = 0; product < ws_.products; ++product) {
			city.markets[product] = {0.0, demo_base_price(product),
				0.0, 0.0};
		}
		// §15: estoque inicial de materiais de construção por cidade (a "obra em
		// estoque" pré-existente, derivada da população de simulação). Sem ele a
		// construção real (sem produtor de materiais no mundo inicial) travaria
		// no primeiro dia. `derived:initial_stock` — não é dado observado.
		const int cmat = demo_product_index(String("construction_material"));
		const int csteel = demo_product_index(String("steel_structure"));
		const int celec = demo_product_index(String("electrical_material"));
		const int cmach = demo_product_index(String("industrial_machinery"));
		if (cmat >= 0) {
			city.markets[cmat].stock =
				city.simulation_population * kDemoConstructionMaterialStockPerCapita;
		}
		if (csteel >= 0) {
			city.markets[csteel].stock =
				city.simulation_population * kDemoSteelStructureStockPerCapita;
		}
		if (celec >= 0) {
			city.markets[celec].stock =
				city.simulation_population * kDemoElectricalMaterialStockPerCapita;
		}
		if (cmach >= 0) {
			city.markets[cmach].stock =
				city.simulation_population * kDemoIndustrialMachineryStockPerCapita;
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
		DemoBuilding building;
		building.id = std::string(
			String(d.get("id", d.get("building_id", ""))).utf8().get_data());
		building.city = std::string(String(d["city_id"]).utf8().get_data());
		building.resource = std::string(
			String(d.get("resource", "wheat")).utf8().get_data());
		const int output_product = demo_product_index(
			String(building.resource.c_str()));
		building.output_product = output_product >= 0 ? output_product : 0;
		building.kind = 0;  // tipo (0=fazenda); o produto fica em output_product
		building.owner = "private";
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
		building.capacity = capacity;
		building.input_per_output = 0.0;
		building.jobs = capacity * aptitude * kDemoJobsPerTon;
		building.wage = kDemoFarmWage;
		building.opex = capacity * aptitude * kDemoOpexPerTon;
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

	demo_rebuild_indexes();
	// Capacidade de nível 1 depende da população da cidade-sede: só depois do
	// índice de cidades (e de os portos existirem).
	demo_init_port_capacities();
	// F4: frota inicial derivada de população/PIB (source: derived). Os bens da
	// frota só resolvem depois de demo_set_goods; aqui as contagens/estoques já
	// existem e os índices são atualizados quando os bens chegam.
	demo_init_fleets();
	// F8: serviços — catálogo já carregado pelo autoload/teste; os negócios
	// iniciais saem do caixa do próprio setor (capital fica em service_cash).
	demo_seed_services();
	// F9: grupos/empresas a partir dos ativos privados existentes.
	demo_seed_groups();
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

Dictionary EconomySim::demo_get_fleet(const String& nation) const {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	Dictionary out;
	const std::string tag(nation.utf8().get_data());
	const auto found = ws_.fleets.find(tag);
	if (found == ws_.fleets.end()) {
		out["error"] = "nacao sem frota";
		return out;
	}
	const DemoFleet& fleet = found->second;
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

void EconomySim::demo_set_fleet(const String& nation, double trucks, double ships,
		double planes, double fuel, double spare_parts) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	const std::string tag(nation.utf8().get_data());
	DemoFleet fleet = DemoFleet{};
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
	demo_status_invalidate();
}

Dictionary EconomySim::demo_fleet_buy(const String& nation, const String& product,
		double amount, double budget_share) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	Dictionary result;
	result["bought"] = 0.0;
	result["spent"] = 0.0;
	result["unfilled"] = 0.0;
	result["error"] = "";
	const std::string tag(nation.utf8().get_data());
	const int product_index = demo_product_index(product);
	if (product_index < 0) {
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
	const double bought = sim_core_.demo_fleet_buy_core(tag, product_index, amount,
		budget_share, spent, unfilled);
	result["bought"] = bought;
	result["spent"] = spent;
	result["unfilled"] = unfilled;
	demo_status_invalidate();
	return result;
}

void EconomySim::demo_set_market_stock(const String& city_id, const String& product,
		double stock) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	DemoCity* city = find_demo_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr) {
		return;
	}
	const int product_index = demo_product_index(product);
	if (product_index < 0) {
		return;
	}
	city->markets[product_index].stock = std::max(0.0, stock);
	++demo_status_static_version_;
	demo_status_invalidate();
}

void EconomySim::demo_set_market_price(const String& city_id,
		const String& product, double price) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	DemoCity* city = find_demo_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr || !std::isfinite(price)) {
		return;
	}
	const int product_index = demo_product_index(product);
	if (product_index < 0) {
		return;
	}
	const double base = demo_base_price(product_index);
	city->markets[product_index].price = std::clamp(
		std::max(1e-6, price), base * 0.1, base * 10.0);
	demo_status_invalidate();
}

Dictionary EconomySim::demo_expand_port(const String& port_id, double t_per_day) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	Dictionary result;
	result["expanded"] = false;
	result["cost"] = 0.0;
	result["days"] = 0;
	result["capacity"] = 0.0;
	result["error"] = "";

	Port* port = find_demo_port(std::string(port_id.utf8().get_data()));
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
		payer_index = demo_city_index_near(port->nation, port->landmass, port->center);
	}
	if (payer_index < 0) {
		result["error"] = "cidade-sede ausente";
		return result;
	}
	DemoCity& payer = ws_.cities[static_cast<std::size_t>(payer_index)];
	const double cost = add * kDemoPortCapitalPerTonCapacity;
	if (payer.government_budget + 1e-9 < cost) {
		result["error"] = "orcamento insuficiente";
		return result;
	}
	payer.government_budget -= cost;
	ws_.construction_cash += cost;
	port->pending_capacity_t_per_day += add;
	port->pending_days = std::max(port->pending_days, kDemoPortBuildDays);
	demo_status_invalidate();
	result["expanded"] = true;
	result["cost"] = cost;
	result["days"] = port->pending_days;
	return result;
}

void EconomySim::demo_set_port_capacity(const String& port_id, double t_per_day) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	Port* port = find_demo_port(std::string(port_id.utf8().get_data()));
	if (port == nullptr) {
		return;
	}
	port->base_capacity_t_per_day = safe_nonneg(t_per_day);
	port->capacity_t_per_day = port->base_capacity_t_per_day;
	demo_status_invalidate();
}

void EconomySim::demo_set_instant_construction(bool instant) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	instant_construction_ = instant;
}

Dictionary EconomySim::tick_demo() {
	demo_tick_and_publish();
	// Cache hit: demo_tick_and_publish() já montou e publicou o dia em curso.
	return get_demo_status();
}

void EconomySim::demo_tick_and_publish() {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
#if defined(OGS_PROFILING)
	// Sink das fases do tick: os 15 slots de `demo_profile_us_`, com o toggle
	// `demo_profile_` como interruptor (desligado, nenhum relógio é lido).
	ogs::profile::Sink phase_sink{demo_profile_us_, kDemoPhaseCount, &demo_profile_};
	const ogs::profile::SinkScope phase_scope(&phase_sink);
#endif
	demo_tick_core();
	{
		// Fase `status` (RAII): fecha ao sair do bloco, depois do tick.
		OGS_SIM_ZONE(kDemoPhaseStatus);
		get_demo_status();
	}
}

void EconomySim::demo_status_invalidate() {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	demo_status_built_day_ = -1;
}

void EconomySim::demo_set_profiling(bool enabled) {
	demo_profile_ = enabled;
	if (!enabled) {
		for (double& value : demo_profile_us_) {
			value = 0.0;
		}
	}
}

void EconomySim::demo_set_sim_threads(int threads) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	sim_core_.demo_set_sim_threads(threads);
}

Dictionary EconomySim::demo_get_profile() const {
	static const char* kNames[kDemoPhaseCount] = {
		"arrival", "reset", "labor", "production", "raw", "income", "services",
		"trade", "accounting", "prices", "clamp", "fiscal", "fx", "invariants",
		"status",
		"trade_food", "trade_food_buyers",
		"trade_goods", "trade_goods_loop",
	};
	Dictionary profile;
	if (!demo_profile_) {
		return profile;
	}
	for (int phase = 0; phase < kDemoPhaseCount; ++phase) {
		profile[kNames[phase]] = demo_profile_us_[phase];
	}
	static const char* kStatusNames[4] = {
		"status_cities", "status_orders", "status_nations", "status_misc",
	};
	for (int part = 0; part < 4; ++part) {
		profile[kStatusNames[part]] = demo_status_sub_us_[part];
	}
	return profile;
}

// Pré-aquece os dois slots do status. O primeiro dia publicado em um slot
// frio paga a criação de TODOS os dicionários por cidade (medido no cenário
// real: 60 ms contra 17 ms num dia quente). Aqui isso vira custo de carga, e
// nenhum tick paga a alocação. Só roda com o mundo já preenchido (aptidão,
// demanda externa e índice de renda aplicados), senão o bloco estático do
// status sairia com os valores default.
void EconomySim::demo_warm_status() {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	if (!ws_.active) {
		return;
	}
	get_demo_status();  // preenche o slot corrente
	if (demo_status_live_ < 0) {
		return;
	}
	// `built_day_ = -1` faz a próxima chamada construir no OUTRO slot.
	demo_status_built_day_ = -1;
	get_demo_status();
}

void EconomySim::demo_tick_core() {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);

	if (!ws_.active) {
		initialize_demo();
	}
	// §12.3: se o mundo real ainda não tem indústria, semeia poucas plantas de
	// referência (moinho + padaria) antes de contar dinheiro/invariantes.
	demo_seed_industry();

	// S5b: retrato, dinheiro antes, avanço do dia, obras de porto, remoção de
	// falidos/esgotados, índices e preços de abertura migraram para ogs::SimCore.
	sim_core_.prologue();

	// 1. Chegada das cargas pedidas no dia anterior.
	sim_core_.phase_arrival();

	// 2. Zera os fluxos do dia.
	sim_core_.phase_reset();

	// 2b. F4: manutenção/compra mensal da frota e zera a fila do dia. Roda
	// antes do comércio para combustível/veículos comprados já valerem hoje
	// (migrou p/ SimCore).
	sim_core_.phase_fleet();

	// 3. Mercado de trabalho e salários.
	sim_core_.phase_labor();

	// 4. Produção: cada construção declara insumo e produto (migrou p/ SimCore).
	sim_core_.phase_production();

	// 4c. Recursos brutos (minerais, soja, algodão e madeira) (migrou p/ SimCore).
	sim_core_.phase_raw();

	// 5. Renda das famílias, impostos, transferências e consumo alimentar
	// (migrou p/ SimCore).
	sim_core_.phase_income();

	// F8: serviços (migrou p/ SimCore).
	sim_core_.phase_services();

	// 6. Comércio de alimento e dos demais bens (migrou p/ SimCore).
	sim_core_.phase_trade();

	// 7. Receita por construção, lucro, distribuição de capital e PIB
	// (migrou p/ SimCore).
	sim_core_.phase_accounting();

	// 8. Preços reagem a estoque, oferta, demanda e custo de produção
	// (migrou p/ SimCore).
	sim_core_.phase_prices();

	// 9. Limite diário único por mercado, aplicado depois de todas as regras
	// de preço (migrou p/ SimCore).
	sim_core_.phase_clamp();

	// 9b-9b2. Redistribuição fiscal nacional + dividendos/repatriação dos grupos
	// (migrou p/ SimCore).
	sim_core_.phase_fiscal();

	// 9c. Câmbio dinâmico: a balança comercial do dia move as moedas float
	// (migrou p/ SimCore).
	sim_core_.phase_fx();

	// 10. Invariantes: se o tick corrompeu o estado (dinheiro não conservado,
	// caixa/estoque negativo ou preço fora da banda), registra e reverte
	// (migrou p/ SimCore; o log dos invariantes fica na fachada).
	sim_core_.phase_invariants();
	if (!ws_.invariant_violations.empty()) {
		for (const std::string& violation : ws_.invariant_violations) {
			UtilityFunctions::push_error(
				String(("economia.invariante: " + violation).c_str()));
		}
	}
}

EconomySim::DemoStatusKeys::DemoStatusKeys() :
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
		d_depleted("d_depleted")
{}

void EconomySim::demo_fill_city_status(std::size_t city_index,
		DemoStatusCitySlot& slot, double in_transit, bool heavy) const {
	const DemoCity& city = ws_.cities[city_index];
	const DemoStatusKeys& k = demo_status_keys_;
	Dictionary& dict = slot.dict;
	// Taxa de câmbio da nação, resolvida UMA vez por cidade (antes eram 2
	// lookups de moeda por cidade: aqui e no bloco de mercados).
	const double fx_rate = demo_fx_rate(city.nation);

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
		std::string(demo_currency_code(city.nation)), k.currency);

	// Campos que só mudam quando um dado do território muda (aptidão, demanda
	// externa, posição): reescritos apenas quando a versão estática sobe.
	if (!slot.static_filled || slot.static_version != demo_status_static_version_) {
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
		slot.static_version = demo_status_static_version_;
	}

	// `population` = populacao real exibida; `sim_population` = unidade de
	// simulacao. As divisoes per-capita usam a segunda, para nao mudar com o
	// rateio nacional (o PIB nasce da populacao de simulacao).
	//
	// Cada escrita é uma chamada à ponte GDExtension com hash da chave
	// (~200 ns): 25 campos × 834 cidades custavam ~4 ms/dia. População,
	// população de simulação e os indicadores derivados ficam parados em boa
	// parte das cidades — comparar double antes evita reescrevê-los.
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
	sync_scalar(k.population, city.population);
	sync_scalar(k.simulation_population, city.simulation_population);
	sync_scalar(k.gdp, city.gdp);
	sync_scalar(k.gdp_per_capita, city.simulation_population > 0.0
		? city.gdp / city.simulation_population : 0.0);
	sync_scalar(k.external_income_index, city.external_income_index);
	sync_scalar(k.firm_cash, city.firm_cash);
	sync_scalar(k.service_cash, city.service_cash);
	sync_scalar(k.government_budget, city.government_budget);
	sync_scalar(k.government_goods_spend, city.government_goods_spend);
	sync_scalar(k.service_revenue, city.service_revenue);
	sync_scalar(k.service_jobs, city.service_jobs);
	sync_scalar(k.service_workers, city.service_workers);
	sync_scalar(k.service_jobs_filled, city.service_jobs_filled);
	sync_scalar(k.in_kind_food, city.in_kind_food);
	sync_scalar(k.service_profit, city.service_profit);
	sync_scalar(k.opex_revenue, city.opex_revenue);
	sync_scalar(k.freight_revenue, city.freight_revenue);
	sync_scalar(k.construction_revenue, city.construction_revenue);
	sync_scalar(k.service_wage, city.service_jobs > 0.0
		? city.service_wage_bill / city.service_jobs : 0.0);
	sync_scalar(k.disposable_per_capita, city.simulation_population > 0.0
		? city.pop.disposable / city.simulation_population : 0.0);

	// Padrão de vida: quantas cestas alimentares diárias a renda disponível
	// por pessoa compra. Diferente do PIB per capita, usa renda das famílias.
	double cheapest_price = 0.0;
	for (int food = 0; food < static_cast<int>(ws_.food_products.size()); ++food) {
		const int product = demo_food_product(food);
		const double factor = demo_food_calories(product);
		if (factor <= 0.0) {
			continue;
		}
		const double per_calorie = city.markets[product].price / factor;
		if (per_calorie > 0.0
				&& (cheapest_price <= 0.0 || per_calorie < cheapest_price)) {
			cheapest_price = per_calorie;
		}
	}
	const double basket_cost = kDemoNeedPerCapita * cheapest_price;
	const double living_standard = basket_cost > 0.0
		? (city.pop.disposable / city.simulation_population) / basket_cost : 0.0;
	sync_scalar(k.living_standard, living_standard);

	// Indicadores derivados, no espírito do formato legado, para a camada
	// visual: produção primária (t/dia), empregos, renda relativa, grau de
	// industrialização, estoque de trigo e necessidade diária.
	double production = 0.0;
	double secondary_capacity = 0.0;
	double total_capacity = 0.0;
	const std::vector<int>& local_buildings = ws_.buildings_by_city[city_index];
	for (const int bi : local_buildings) {
		const DemoBuilding& building = ws_.buildings[static_cast<std::size_t>(bi)];
		total_capacity += building.capacity * building.output_scale;
		if (building.input_product < 0) {
			production += building.capacity * building.output_scale;
		} else {
			secondary_capacity += building.capacity * building.output_scale;
		}
	}
	sync_scalar(k.production, production);
	sync_scalar(k.jobs, city.pop.employed);
	sync_scalar(k.income, std::clamp(living_standard, 0.0, 1.0));
	sync_scalar(k.industrialization, total_capacity > 0.0
		? std::clamp(secondary_capacity / total_capacity, 0.0, 1.0) : 0.0);
	sync_scalar(k.stockpile, city.markets[0].stock);
	sync_scalar(k.need, city.pop.need);
	sync_scalar(k.deficit, std::max(0.0, city.pop.need - city.pop.consumed));
	sync_scalar(k.consumed, city.pop.consumed);
	sync_scalar(k.in_transit, in_transit);
	sync_scalar(k.fx_rate, fx_rate);
	sync_scalar(k.tariff_revenue, city.tariff_revenue);
	sync_scalar(k.productivity_index, demo_effective_productivity(city));
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
	// do cenário real (o mundo sintético do bench não tem depósito nenhum).
	if (slot.deposit_entry.size() < ws_.products) {
		slot.deposit_entry.resize(static_cast<std::size_t>(ws_.products));
		slot.deposit_present.assign(static_cast<std::size_t>(ws_.products), 0);
		slot.deposit_depleted.assign(static_cast<std::size_t>(ws_.products), 0);
		slot.deposit_written.assign(static_cast<std::size_t>(ws_.products),
			DemoDeposit{});
		slot.deposit_key.clear();
		slot.deposit_key.reserve(static_cast<std::size_t>(ws_.products));
		for (int product = 0; product < ws_.products; ++product) {
			slot.deposit_key.emplace_back(demo_product_name(product));
		}
	}
	// Jazida descoberta depois do primeiro dia (a IA/geografia chama
	// `demo_set_deposit`): a lista de jazidas rastreadas é recalculada uma vez e
	// só as novas entram no dicionário (as antigas mantêm o bloco estático).
	if (slot.deposit_tracked_version != demo_status_deposit_version_) {
		slot.deposit_tracked.clear();
		for (int product = 0; product < ws_.products; ++product) {
			if (city.deposits[product].tracked) {
				slot.deposit_tracked.push_back(product);
			}
		}
		slot.deposit_tracked_version = demo_status_deposit_version_;
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
			const DemoDeposit& deposit = city.deposits[product];
			Dictionary& entry = slot.deposit_entry[static_cast<std::size_t>(product)];
			DemoDeposit& previous = slot.deposit_written[static_cast<std::size_t>(product)];
			const bool first = slot.deposit_present[static_cast<std::size_t>(product)] == 0;
			if (first) {
				entry[k.initial_kt] = deposit.initial_kt;
				entry[k.quality] = deposit.quality;
				entry[k.depth_m] = deposit.depth_m;
				entry[k.efficiency] = demo_extraction_efficiency(deposit);
				entry[k.opex_factor] = demo_depth_opex_factor(deposit);
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
			const bool depleted = deposit.remaining_kt <= 0.0;
			if (first || slot.deposit_depleted[static_cast<std::size_t>(product)] != (depleted ? 1 : 0)) {
				entry[k.depleted] = depleted;
				slot.deposit_depleted[static_cast<std::size_t>(product)] = depleted ? 1 : 0;
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
			slot.education.resize(kDemoEducationLevels);
			slot.education_share.resize(kDemoEducationLevels);
			slot.labor_force_by_level.resize(kDemoEducationLevels);
			slot.employed_by_level.resize(kDemoEducationLevels);
			slot.unemployed_by_level.resize(kDemoEducationLevels);
			slot.unemployment_by_level.resize(kDemoEducationLevels);
			slot.vacancies_by_level.resize(kDemoEducationLevels);
			slot.wage_by_level.resize(kDemoEducationLevels);
			slot.wage_income_by_level.resize(kDemoEducationLevels);
			slot.studying.resize(kDemoEducationLevels - 1);
			slot.income_level_share.resize(kDemoIncomeLevels);
			slot.income_level_by_stratum.resize(kDemoEducationLevels);
			slot.stratum_disposable.resize(kDemoEducationLevels);
			slot.stratum_population.resize(kDemoEducationLevels);
			slot.stratum_wage_income.resize(kDemoEducationLevels);
		}
		// Grupo "raro": educação, força por nível, estudo, fatias de renda e o
		// nível de consumo de cada estrato. Compara com o último publicado e só
		// toca nos Arrays quando algo muda (mudam em escala de semanas).
		{
			double rare[16];
			int ri = 0;
			for (int level = 0; level < kDemoEducationLevels; ++level) {
				rare[ri++] = city.pop.education[level];
			}
			for (int level = 0; level < kDemoEducationLevels; ++level) {
				rare[ri++] = city.pop.labor_force_by_level[level];
			}
			for (int track = 0; track < kDemoEducationLevels - 1; ++track) {
				rare[ri++] = city.pop.studying[track];
			}
			for (int level = 0; level < kDemoIncomeLevels; ++level) {
				rare[ri++] = city.pop.income_level_share[level];
			}
			for (int stratum = 0; stratum < kDemoEducationLevels; ++stratum) {
				rare[ri++] = double(city.pop.stratum_income_level[stratum]);
			}
			if (!slot.labor_rare_filled
					|| std::memcmp(rare, slot.labor_rare_written,
						sizeof(double) * static_cast<std::size_t>(ri)) != 0) {
				for (int level = 0; level < kDemoEducationLevels; ++level) {
					slot.education[level] = city.pop.education[level];
					slot.labor_force_by_level[level] =
						city.pop.labor_force_by_level[level];
				}
				for (int track = 0; track < kDemoEducationLevels - 1; ++track) {
					slot.studying[track] = city.pop.studying[track];
				}
				for (int level = 0; level < kDemoIncomeLevels; ++level) {
					slot.income_level_share[level] =
						city.pop.income_level_share[level];
				}
				for (int stratum = 0; stratum < kDemoEducationLevels; ++stratum) {
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
			for (int level = 0; level < kDemoEducationLevels; ++level) {
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
				for (int level = 0; level < kDemoEducationLevels; ++level) {
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
			for (int stratum = 0; stratum < kDemoEducationLevels; ++stratum) {
				strata[si++] = city.pop.stratum_disposable[stratum];
				strata[si++] = city.pop.stratum_population[stratum];
				strata[si++] = city.pop.stratum_wage_income[stratum];
			}
			if (!slot.stratum_written_filled
					|| std::memcmp(strata, slot.stratum_written,
						sizeof(double) * static_cast<std::size_t>(si)) != 0) {
				for (int stratum = 0; stratum < kDemoEducationLevels; ++stratum) {
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
	const double local_rate = std::max(1e-9, fx_rate);
	bool markets_dirty = false;
	for (int product = 0; product < ws_.products; ++product) {
		const DemoMarket& source = city.markets[product];
		DemoMarket& previous = slot.market_written[static_cast<std::size_t>(product)];
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
		// são contíguos (ver DemoMarket): um memcmp de 64 bytes por produto.
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
		services.resize(kDemoMaxServiceSectors * kDemoServiceFields);
		double* out = services.ptrw();
		for (int sector = 0; sector < kDemoMaxServiceSectors; ++sector) {
			const DemoServiceMarket& market = city.service_markets[sector];
			double* base = out + sector * kDemoServiceFields;
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
	if (slot.buildings.size() != local_buildings.size()) {
		slot.buildings.resize(local_buildings.size());
		slot.buildings_bound = false;
	}
	if (slot.building_entry.size() < local_buildings.size()) {
		slot.building_entry.resize(local_buildings.size());
		slot.building_cache.resize(local_buildings.size());
	}
	for (std::size_t index = 0; index < local_buildings.size(); ++index) {
		const DemoBuilding& building =
			ws_.buildings[static_cast<std::size_t>(local_buildings[index])];
		Dictionary& entry = slot.building_entry[index];
		DemoStatusCitySlot::BuildingCache& cached = slot.building_cache[index];
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
			const int owner_kind = demo_owner_kind(building.owner, &group_id,
				&origin_nation);
			entry[k.owner_kind] = owner_kind == 0 ? String("state")
				: owner_kind == 1 ? String("national")
				: owner_kind == 2 ? String("foreign") : String("private");
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
				const DemoRecipe& recipe =
					ws_.recipes[static_cast<std::size_t>(building.recipe)];
				entry[k.recipe] = String(recipe.id.c_str());
				Array inputs;
				Array byproducts;
				inputs.resize(recipe.input_count);
				byproducts.resize(recipe.byproduct_count);
				for (int slot = 0; slot < recipe.input_count; ++slot) {
					Dictionary input;
					input[k.good] = String(demo_product_name(recipe.inputs[slot]));
					input[k.qty] = recipe.input_qty[slot];
					inputs[slot] = input;
				}
				for (int slot = 0; slot < recipe.byproduct_count; ++slot) {
					Dictionary byproduct;
					byproduct[k.good] = String(demo_product_name(recipe.byproducts[slot]));
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
	}
	if (!slot.buildings_published) {
		dict[k.buildings] = slot.buildings;
		slot.buildings_published = true;
	}
	slot.buildings_bound = true;
	}  // fim do bloco pesado (buildings)
}

void EconomySim::demo_fill_light_signals(DemoStatusCache& cache) const {
	const DemoStatusKeys& k = demo_status_keys_;
	const std::size_t cities = ws_.cities.size();
	const std::size_t products = static_cast<std::size_t>(ws_.products);
	const std::size_t market_len = cities * products;

	// Mercados: 7 campos que a IA lê (preço âncora, estoque, oferta, excedente,
	// vendido, escassez, desejado). São os MESMOS doubles do bloco pesado por
	// cidade (`source.price` etc. em demo_fill_city_status) — bit a bit.
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
		const DemoCity& city = ws_.cities[city_index];
		const std::size_t base = city_index * products;
		for (int product = 0; product < ws_.products; ++product) {
			const DemoMarket& market = city.markets[product];
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
			const DemoBuilding& building =
				ws_.buildings[static_cast<std::size_t>(bi)];
			std::string group_id;
			std::string origin_nation;
			demo_owner_kind(building.owner, &group_id, &origin_nation);
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
	// do bloco pesado (demo_fill_city_status), na ordem cidade → construção
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
		const DemoCity& bcity = ws_.cities[city_index];
		for (const int bi : ws_.buildings_by_city[city_index]) {
			const DemoBuilding& building =
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
		const DemoCity& dcity = ws_.cities[city_index];
		for (int product = 0; product < ws_.products; ++product) {
			const DemoDeposit& deposit = dcity.deposits[product];
			if (!deposit.tracked) {
				continue;
			}
			cache.light_d_product.push_back(product);
			cache.light_d_resource.push_back(String(demo_product_name(product)));
			cache.light_d_initial_kt.push_back(deposit.initial_kt);
			cache.light_d_remaining_kt.push_back(deposit.remaining_kt);
			cache.light_d_quality.push_back(deposit.quality);
			cache.light_d_depth_m.push_back(deposit.depth_m);
			cache.light_d_efficiency.push_back(demo_extraction_efficiency(deposit));
			cache.light_d_opex_factor.push_back(demo_depth_opex_factor(deposit));
			cache.light_d_extracted_kt.push_back(deposit.extracted_kt);
			cache.light_d_depleted.push_back(deposit.remaining_kt <= 0.0 ? 1 : 0);
		}
		cache.light_d_offset.push_back(
			static_cast<int32_t>(cache.light_d_product.size()));
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
}

Dictionary EconomySim::get_demo_status() {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	// Snapshot do dia já publicado: devolve o mesmo dicionário (a UI lê o dia
	// inteiro; mutações entre ticks só valem no próximo status).
	if (demo_status_live_ >= 0 && demo_status_built_day_ == ws_.day) {
		return demo_status_slots_[demo_status_live_].root;
	}
	const int slot_index = demo_status_live_ < 0 ? 0 : 1 - demo_status_live_;
	DemoStatusCache& cache = demo_status_slots_[slot_index];
	const DemoStatusKeys& k = demo_status_keys_;
	Dictionary& root = cache.root;

	// Subdivisao da fase status no benchmark (sem custo no jogo): as 4 marcas
	// `OGS_SIM_PHASE_LAP(part)` abaixo gravam em `demo_status_sub_us_`.
#if defined(OGS_PROFILING)
	ogs::profile::Sink status_sink{demo_status_sub_us_, 4, &demo_profile_};
	const ogs::profile::SinkScope status_scope(&status_sink);
#endif

	root[k.mode] = "demo";
	root[k.day] = ws_.day;
	root[k.calendar_day] = ws_.day;
	root[k.money_total] = demo_money_total();
	root[k.transport_cash] = ws_.transport_cash;
	root[k.foreign_cash] = ws_.foreign_cash;
	root[k.construction_cash] = ws_.construction_cash;
	root[k.bankruptcies] = ws_.bankruptcies;
	root[k.depleted_mines] = ws_.depleted_mines;
	// F8: catálogo de serviços (uma vez) + contadores de negócios do dia.
	if (ws_.services_loaded && !cache.service_sectors_filled) {
		cache.service_sectors = demo_get_service_sectors();
		root[k.service_sectors] = cache.service_sectors;
		cache.service_sectors_filled = true;
	}
	root[k.service_businesses] = demo_get_service_business_count();
	root[k.service_bankruptcies] = ws_.service_bankruptcies;

	// Cabeçalho: os contêineres são pendurados no dicionário UMA vez; a partir
	// daí só são redimensionados/reescritos no lugar.
	if (!cache.header_filled) {
		cache.products.resize(ws_.products);
		for (int index = 0; index < ws_.products; ++index) {
			cache.products[index] = String(demo_product_name(index));
		}
		cache.tot_produced.resize(ws_.products);
		cache.tot_consumed.resize(ws_.products);
		cache.tot_produced_today.resize(ws_.products);
		cache.tot_consumed_today.resize(ws_.products);
		root[k.capital_cost_per_capacity] = kDemoCapitalPerCapacity;
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
	cache.in_transit_by_city.assign(ws_.cities.size(), 0.0);
	for (const DemoShipment& shipment : ws_.in_transit) {
		const auto found = ws_.city_lookup.find(shipment.to_city);
		if (found != ws_.city_lookup.end()) {
			cache.in_transit_by_city[static_cast<std::size_t>(found->second)]
				+= shipment.amount;
		}
	}
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
	for (const DemoCity& city : ws_.cities) {
		for (int product = 0; product < ws_.products; ++product) {
			const DemoMarket& market = city.markets[product];
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
	demo_fill_light_signals(cache);

	cache.cities.resize(ws_.cities.size());
	cache.city_slots.resize(ws_.cities.size());
	for (std::size_t city_index = 0; city_index < ws_.cities.size(); ++city_index) {
		DemoStatusCitySlot& slot = cache.city_slots[city_index];
		demo_fill_city_status(city_index, slot,
			cache.in_transit_by_city[city_index], false);
		cache.cities[city_index] = slot.dict;
	}
	OGS_SIM_PHASE_LAP(0);

	// Portos (modo mundo) para o minimapa e o mundo 3D.
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

	// Pedidos de comércio do último tick (comboios 3D e HUD).
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
			? String(demo_product_name(order.product)) : String("wheat");
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
			demo_fill_leg_status(leg_dict, order.legs[leg_index]);
			legs[leg_index] = leg_dict;
		}
		dict[k.legs] = legs;
		cache.orders[index] = dict;
	}
	OGS_SIM_PHASE_LAP(1);

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
		double education[kDemoEducationLevels] = {};
		double labor_force_by_level[kDemoEducationLevels] = {};
		double employed_by_level[kDemoEducationLevels] = {};
		double unemployed_by_level[kDemoEducationLevels] = {};
		double vacancies_by_level[kDemoEducationLevels] = {};
		double wage_weighted[kDemoEducationLevels] = {};
		double wage_weight[kDemoEducationLevels] = {};
		// F7 calibração: fatia da POP da nação por nível de consumo
		// (0 = nenhum, 1 = basic, 2 = life, 3 = luxury).
		double income_share[kDemoIncomeLevels] = {};
		double income_population = 0.0;
		// F7 calibração: TFP nacional ponderada pela população de simulação.
		double productivity_weighted = 0.0;
	};
	// S17: cadência do agregado `labor` (semanal). Os campos diários
	// (população, estoque, necessidade, déficit, produção, trânsito) seguem
	// diários; o bloco `labor` (educação/força/emprego/fatias de renda) é
	// recomputado uma vez por semana — é dado de exibição, não decisão da IA.
	// A simulação (save) não muda: só a publicação do agregado é escalonada.
	const bool labor_due = !cache.labor_cadence_built || (ws_.day % 7 == 0);
	std::vector<std::string> nation_order;
	std::vector<NationTotals> nation_totals;
	std::unordered_map<std::string, int> nation_slot;
	nation_slot.reserve(ws_.cities.size() * 2);
	for (std::size_t city_index = 0; city_index < ws_.cities.size(); ++city_index) {
		const DemoCity& city = ws_.cities[city_index];
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
		if (labor_due) {
		totals.working_age += city.pop.working_age;
		totals.labor_force += city.pop.labor_force;
		totals.employed += city.pop.employed;
		totals.unemployed += city.pop.unemployed;
		totals.students += city.pop.students;
		totals.recruit_reserve += city.pop.recruit_reserve;
		totals.income_population += std::max(0.0, city.simulation_population);
		totals.productivity_weighted += demo_effective_productivity(city)
			* std::max(0.0, city.simulation_population);
		for (int level_index = 0; level_index < kDemoIncomeLevels; ++level_index) {
			totals.income_share[level_index] +=
				city.pop.income_level_share[level_index]
				* std::max(0.0, city.simulation_population);
		}
		for (int level = 0; level < kDemoEducationLevels; ++level) {
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
			const DemoBuilding& building =
				ws_.buildings[static_cast<std::size_t>(bi)];
			if (building.input_product < 0) {
				totals.production += building.capacity * building.output_scale;
			}
		}
	}
	for (std::size_t index = 0; index < cache.in_transit_by_city.size(); ++index) {
		const double amount = cache.in_transit_by_city[index];
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
		DemoStatusNationSlot& slot = cache.nation_slots[index];
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
		// F4: frota/combustível da nação (a UI e a IA leem daqui; a fila do dia
		// é o sinal de compra de veículos/combustível). O sub-dicionário e o
		// Array de saúde são reaproveitados entre ticks (byte-idêntico: mesmos
		// valores, só sem realocar os contêineres).
		const auto fleet_found = ws_.fleets.find(tag);
		if (fleet_found != ws_.fleets.end()) {
			const DemoFleet& fleet = fleet_found->second;
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
		// S17: cadência semanal — fora do dia `labor_due`, o bloco `labor` do
		// dia anterior é mantido (agregado de exibição, não decisão da IA).
		if (labor_due) {
		Dictionary& labor = slot.labor;
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
			slot.unemployment_by_level.resize(kDemoEducationLevels);
			slot.wage_by_level.resize(kDemoEducationLevels);
			slot.income_share.resize(kDemoIncomeLevels);
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
		slot.education[0] = totals.education[0];
		slot.education[1] = totals.education[1];
		slot.education[2] = totals.education[2];
		slot.labor_force_by_level[0] = totals.labor_force_by_level[0];
		slot.labor_force_by_level[1] = totals.labor_force_by_level[1];
		slot.labor_force_by_level[2] = totals.labor_force_by_level[2];
		slot.employed_by_level[0] = totals.employed_by_level[0];
		slot.employed_by_level[1] = totals.employed_by_level[1];
		slot.employed_by_level[2] = totals.employed_by_level[2];
		slot.unemployed_by_level[0] = totals.unemployed_by_level[0];
		slot.unemployed_by_level[1] = totals.unemployed_by_level[1];
		slot.unemployed_by_level[2] = totals.unemployed_by_level[2];
		slot.vacancies_by_level[0] = totals.vacancies_by_level[0];
		slot.vacancies_by_level[1] = totals.vacancies_by_level[1];
		slot.vacancies_by_level[2] = totals.vacancies_by_level[2];
		for (int level = 0; level < kDemoEducationLevels; ++level) {
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
		// F7 calibração: fatia da POP por nível de consumo (a soma é 1).
		for (int level_index = 0; level_index < kDemoIncomeLevels; ++level_index) {
			slot.income_share[level_index] = totals.income_population > 1e-9
				? totals.income_share[level_index] / totals.income_population : 0.0;
		}
		// F7 calibração: fator de produtividade nacional (`derived:gdp_ppp`,
		// 1,0 = média mundial), ponderado pela população de simulação.
		labor[k.productivity_index] = totals.income_population > 1e-9
			? totals.productivity_weighted / totals.income_population : 1.0;
		nation[k.labor] = labor;
		}
		cache.nations[index] = nation;
	}
	if (labor_due) {
		cache.labor_cadence_built = true;
	}

	for (int index = 0; index < ws_.products; ++index) {
		cache.tot_produced[index] = ws_.produced[index];
		cache.tot_consumed[index] = ws_.consumed_total[index];
		cache.tot_produced_today[index] = ws_.produced_today[index];
		cache.tot_consumed_today[index] = ws_.consumed_today[index];
	}
	// F5: moedas e tarifas para a UI da F6. Reescritas em lugar (o Array é
	// referência): só cresce quando o conjunto muda.
	cache.currencies.resize(static_cast<int>(ws_.currencies.size()));
	for (std::size_t index = 0; index < ws_.currencies.size(); ++index) {
		const DemoCurrency& currency = ws_.currencies[index];
		Dictionary entry;
		entry[k.code] = String(currency.code.c_str());
		entry[k.rate] = currency.rate;
		entry[k.regime] = currency.floating ? "float" : "peg";
		entry[k.fallback] = currency.fallback;
		entry[k.trade_balance] = currency.balance_ema;
		entry[k.source] = String(currency.source.c_str());
		cache.currencies[static_cast<int>(index)] = entry;
	}
	cache.tariffs.resize(static_cast<int>(ws_.tariffs.size()));
	for (std::size_t index = 0; index < ws_.tariffs.size(); ++index) {
		const DemoTariff& tariff = ws_.tariffs[index];
		Dictionary entry;
		entry[k.buyer] = String(tariff.buyer.c_str());
		entry[k.seller] = String(tariff.seller.c_str());
		entry[k.product] = String(tariff.product.c_str());
		entry[k.rate] = tariff.rate;
		cache.tariffs[static_cast<int>(index)] = entry;
	}
	OGS_SIM_PHASE_LAP(2);
	demo_status_live_ = slot_index;
	demo_status_built_day_ = ws_.day;
	return root;
}


Dictionary EconomySim::get_city_status(const String& city_id) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
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
	for (const DemoShipment& shipment : ws_.in_transit) {
		if (shipment.to_city == id) {
			in_transit += shipment.amount;
		}
	}

	// Slot frio (nada preenchido): `demo_fill_city_status` escreve todos os
	// campos — mesmo conteúdo que o bloco por cidade do status diário, mais o
	// detalhe pesado (markets/pop/services/buildings/deposits) sob demanda.
	DemoStatusCitySlot slot;
	demo_fill_city_status(city_index, slot, in_transit, true);
	return slot.dict;
}


void EconomySim::demo_price_shock(const String& product, double multiplier) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	int index = demo_product_index(product);
	if (index < 0) {
		return;
	}
	for (DemoCity& city : ws_.cities) {
		const double base = demo_base_price(index);
		city.markets[index].price = std::clamp(
			city.markets[index].price * multiplier, base * 0.1, base * 10.0);
	}
	demo_status_invalidate();
}

int EconomySim::demo_product_index(const String& product) const {
	return sim_core_.demo_product_index(std::string(product.utf8().get_data()));
}

const char* EconomySim::demo_product_name(int product) const {
	return sim_core_.demo_product_name(product);
}

int EconomySim::demo_recipe_index(const String& recipe_id) const {
	return sim_core_.demo_recipe_index(std::string(recipe_id.utf8().get_data()));
}

void EconomySim::demo_set_recipes(const Array& recipes) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	ws_.recipes.clear();
	ws_.recipe_lookup.clear();
	ws_.recipes.reserve(static_cast<std::size_t>(recipes.size()));
	for (int index = 0; index < recipes.size(); ++index) {
		const Dictionary entry = recipes[index];
		const String output_id = String(entry.get("output", ""));
		const int output = demo_product_index(output_id);
		if (output < 0) {
			continue;  // receita de bem que não existe no goods.json carregado
		}
		DemoRecipe recipe;
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
		demo_education_mix_for_sector(sector, recipe.education_mix);
		if (entry.has("education_mix")) {
			double mix[3] = {-1.0, -1.0, -1.0};
			const Variant mix_value = entry["education_mix"];
			if (mix_value.get_type() == Variant::ARRAY) {
				const Array values = mix_value;
				for (int level = 0;
						level < kDemoEducationLevels && level < values.size();
						++level) {
					mix[level] = std::max(0.0, double(values[level]));
				}
			} else if (mix_value.get_type() == Variant::DICTIONARY) {
				const Dictionary values = mix_value;
				static const char* kNames[3] = {
					"basic", "intermediate", "higher"};
				for (int level = 0; level < kDemoEducationLevels; ++level) {
					if (values.has(kNames[level])) {
						mix[level] = std::max(0.0,
							double(values[kNames[level]]));
					}
				}
			}
			const double total = std::max(0.0, mix[0]) + std::max(0.0, mix[1])
				+ std::max(0.0, mix[2]);
			if (total > 1e-9) {
				for (int level = 0; level < kDemoEducationLevels; ++level) {
					recipe.education_mix[level] = std::max(0.0, mix[level]) / total;
				}
			}
		}
		Array inputs = entry.get("inputs", Array());
		for (int slot = 0; slot < inputs.size() && slot < kDemoMaxRecipeInputs; ++slot) {
			const Dictionary input = inputs[slot];
			const int product = demo_product_index(String(input.get("good", "")));
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
		for (int slot = 0; slot < byproducts.size() && slot < kDemoMaxRecipeInputs; ++slot) {
			const Dictionary item = byproducts[slot];
			const int product = demo_product_index(String(item.get("good", "")));
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

void EconomySim::demo_seed_industry() {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	if (!ws_.world_mode || ws_.industry_seeded || ws_.recipes.empty()
			|| ws_.cities.empty()) {
		return;
	}
	for (const DemoBuilding& building : ws_.buildings) {
		if (building.recipe >= 0 || building.input_product >= 0) {
			ws_.industry_seeded = true;  // o mundo já tem indústria
			return;
		}
	}
	const int flour_product = demo_product_index(String("flour"));
	const int bread_product = demo_product_index(String("bread"));
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
	for (int slot = 0; slot < seed_cities; ++slot) {
		DemoCity& city = ws_.cities[static_cast<std::size_t>(order[slot])];
		// Capacidade de referência = 5% da necessidade alimentar diária da
		// cidade, em toneladas de farinha/pão. É uma ESTIMATIVA de transição
		// (§12.3), não um dado observado; calibrada no laboratório em F3.
		const double capacity = std::clamp(
			city.simulation_population * kDemoNeedPerCapita * 0.05, 2.0, 20.0);
		const DemoRecipe& flour = ws_.recipes[static_cast<std::size_t>(flour_recipe)];
		const double cost = capacity * flour.capital_per_capacity;
		if (city.firm_cash + city.government_budget < cost * 2.0) {
			continue;
		}
		const String city_id(city.id.c_str());
		const String mill_id(("SEED-FLOUR-" + city.id).c_str());
		Dictionary mill = demo_build(city_id, mill_id, "flour", -1, flour_product,
			capacity, 0.0, 0.0, 0.0, 1.0, "state", cost, String(flour.id.c_str()));
		if (!bool(mill.get("created", false))) {
			continue;
		}
		if (DemoBuilding* built = find_demo_building(city.id,
				std::string(mill_id.utf8().get_data()))) {
			built->strategic = true;
		}
		sim_core_.demo_mark_project_strategic(
			std::string(mill_id.utf8().get_data()));
		++created;
		if (bread_recipe >= 0) {
			const DemoRecipe& bread =
				ws_.recipes[static_cast<std::size_t>(bread_recipe)];
			const double bread_capacity = capacity * 0.8;
			const double bread_cost = bread_capacity * bread.capital_per_capacity;
			const String bakery_id(("SEED-BREAD-" + city.id).c_str());
			Dictionary bakery = demo_build(city_id, bakery_id, "bread", -1,
				bread_product, bread_capacity, 0.0, 0.0, 0.0, 1.0, "state",
				bread_cost, String(bread.id.c_str()));
			if (bool(bakery.get("created", false))) {
				if (DemoBuilding* built = find_demo_building(city.id,
						std::string(bakery_id.utf8().get_data()))) {
					built->strategic = true;
				}
				sim_core_.demo_mark_project_strategic(
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
		const int product = demo_product_index(String(output_id));
		for (std::size_t index = 0; index < ws_.recipes.size(); ++index) {
			if (ws_.recipes[index].output == product) {
				return static_cast<int>(index);
			}
		}
		return -1;
	};
	auto seed_final_chain = [this, &recipe_for](DemoCity& city, const char* raw_id,
			const char* mid_id, const char* final_id, double per_capita_day,
			double max_capacity) -> int {
		const int raw_product = demo_product_index(String(raw_id));
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
		const DemoRecipe& mid = ws_.recipes[static_cast<std::size_t>(mid_recipe)];
		const DemoRecipe& final = ws_.recipes[static_cast<std::size_t>(final_recipe)];
		// Só as fábricas são semeadas: a matéria-prima entra por comércio (o
		// `demo_trade_goods` já trata o insumo da receita) e a IA constrói as
		// fazendas de algodão/madeira quando o shortage aparece. Semear a
		// fazenda aqui tirava mão de obra primária das fazendas de alimento.
		const double total_cost =
			final_capacity * mid.capital_per_capacity
			+ final_capacity * final.capital_per_capacity;
		if (city.government_budget < total_cost) {
			return 0;
		}
		const String city_id(city.id.c_str());
		int built = 0;
		const String mid_building_id(("SEED-" + std::string(mid_id) + "-" + city.id).c_str());
		Dictionary mid_build = demo_build(city_id, mid_building_id, String(mid_id),
			-1, ws_.recipes[static_cast<std::size_t>(mid_recipe)].output,
			final_capacity, 0.0, 0.0, 0.0, 1.0, "state",
			final_capacity * mid.capital_per_capacity,
			String(mid.id.c_str()));
		if (bool(mid_build.get("created", false))) {
			if (DemoBuilding* node = find_demo_building(city.id,
					std::string(mid_building_id.utf8().get_data()))) {
				node->strategic = true;
			}
			sim_core_.demo_mark_project_strategic(
				std::string(mid_building_id.utf8().get_data()));
			++built;
		}
		const String final_building_id(("SEED-" + std::string(final_id) + "-" + city.id).c_str());
		Dictionary final_build = demo_build(city_id, final_building_id, String(final_id),
			-1, ws_.recipes[static_cast<std::size_t>(final_recipe)].output,
			final_capacity, 0.0, 0.0, 0.0, 1.0, "state",
			final_capacity * final.capital_per_capacity,
			String(final.id.c_str()));
		if (bool(final_build.get("created", false))) {
			if (DemoBuilding* node = find_demo_building(city.id,
					std::string(final_building_id.utf8().get_data()))) {
				node->strategic = true;
			}
			sim_core_.demo_mark_project_strategic(
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
			DemoCity& city = ws_.cities[static_cast<std::size_t>(city_index)];
			double food_capacity = 0.0;
			for (const int bi : ws_.buildings_by_city[static_cast<std::size_t>(city_index)]) {
				const DemoBuilding& building = ws_.buildings[static_cast<std::size_t>(bi)];
				if (demo_food_calories(building.output_product) > 0.0) {
					food_capacity += building.capacity
						* city.suitability[building.output_product];
				}
			}
			const double food_need = city.simulation_population * kDemoNeedPerCapita;
			if (food_capacity < food_need * 1.10) {
				continue;
			}
			int built = 0;
			// basic/clothing: 0,0004 t/mês por habitante.
			built += seed_final_chain(city, "cotton", "fabric", "clothes",
				0.0004 / 30.0, 20.0);
			// institutional/government paper: 0,005 × âncora alimentar/dia.
			built += seed_final_chain(city, "wood", "cellulose", "paper",
				kDemoNeedPerCapita * 0.005, 10.0);
			if (built > 0) {
				++chains_seeded;
				created += built;
			}
		}
	}
	ws_.industry_seeded = true;
	if (created > 0) {
		UtilityFunctions::print(String(("economia: bootstrap industrial (estimativa) — "
			+ std::to_string(created) + " plantas de referência semeadas").c_str()));
	}
}

void EconomySim::demo_set_needs(const Dictionary& needs) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	// Cesta alimentar a partir do nível básico de needs.json. Só a categoria
	// `food` tem âncora física no núcleo hoje (kDemoNeedPerCapita); as demais
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
			&& static_cast<int>(products.size()) < kDemoMaxFoodProducts; ++index) {
		const String id = String(substitutes[index]);
		const int product = demo_product_index(id);
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
		const int index = demo_product_index(String(legacy));
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
		if (static_cast<int>(ws_.needs_levels.size()) >= kDemoMaxNeedsLevels) {
			break;
		}
		DemoNeedsLevel level;
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
			DemoNeedsCategory category;
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
				const int product = demo_product_index(id);
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
		++demo_status_static_version_;
		demo_status_invalidate();
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
	// alimentar (kDemoNeedPerCapita), declarada na tabela abaixo e registrada em
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
		double anchor_share;  // fração de kDemoNeedPerCapita por dia
	} kEstimatedGovernmentNeeds[] = {
		{"paper", 0.005},
		{"computer", 0.0001},
	};
	for (int index = 0; index < government.size(); ++index) {
		const String id = String(government[index]);
		const int product = demo_product_index(id);
		if (product < 0) {
			continue;
		}
		double per_day = 0.0;
		if (quantities.has(id)) {
			per_day = std::max(0.0, static_cast<double>(quantities[id])) / 30.0;
		} else {
			for (const auto& estimate : kEstimatedGovernmentNeeds) {
				if (id == String(estimate.id)) {
					per_day = kDemoNeedPerCapita * estimate.anchor_share;
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
	for (DemoCity& city : ws_.cities) {
		demo_init_pop_needs(city.pop);
	}
	++demo_status_static_version_;
	demo_status_invalidate();
}

ogs::DemoBuilding* EconomySim::find_demo_building(const std::string& city_id,
		const std::string& id) {
	for (DemoBuilding& building : ws_.buildings) {
		if (building.city == city_id && building.id == id) {
			return &building;
		}
	}
	return nullptr;
}

void EconomySim::demo_set_balance(const Dictionary& values) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
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
	// consumo. Padrão = kDemoIncomeBasketMargin; o laboratório pode isolá-la
	// (1.0 = sem folga) para testar a regra do nível sem depender da calibração.
	ws_.income_basket_margin = std::clamp(
		number("income_basket_margin", ws_.income_basket_margin), 1.0, 8.0);
}

void EconomySim::demo_set_suitability(const String& city_id, const String& product,
		double value) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	const int index = demo_product_index(product);
	if (index < 0) {
		return;
	}
	DemoCity* city = find_demo_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr) {
		return;
	}
	city->suitability[index] = std::clamp(value, 0.0, 1.0);
	// O status reuse arrays de aptidão: invalida só quando o dado muda.
	++demo_status_static_version_;
	demo_status_invalidate();
}

bool EconomySim::demo_set_deposit(const String& city_id, const String& product,
		double reserve_kt, double quality, double depth_m) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	const int index = demo_product_index(product);
	if (index < 0) {
		return false;
	}
	DemoCity* city = find_demo_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr) {
		return false;
	}
	DemoDeposit& deposit = city->deposits[index];
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
	++demo_status_deposit_version_;
	demo_status_invalidate();
	return true;
}

void EconomySim::demo_set_external_demand(const String& city_id, const String& product,
		double ceiling_t_per_day) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	const int index = demo_product_index(product);
	if (index < 0) {
		return;
	}
	DemoCity* city = find_demo_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr) {
		return;
	}
	city->external_demand[index] = safe_nonneg(ceiling_t_per_day);
	++demo_status_static_version_;
	demo_status_invalidate();
}

void EconomySim::demo_set_external_income_index(const String& city_id, double index) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	DemoCity* city = find_demo_city(std::string(city_id.utf8().get_data()));
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
	demo_apply_labor_profile(*city);
	++demo_status_static_version_;
	demo_status_invalidate();
}

void EconomySim::demo_set_productivity_index(const String& city_id, double index) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	DemoCity* city = find_demo_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr) {
		return;
	}
	// 0 (ou negativo) = DERIVAR do índice de renda real: o dado do PIB PPP per
	// capita chega por demo_set_external_income_index e a TFP acompanha. Um
	// valor positivo é fator explícito do laboratório. Os limites só contêm
	// dado corrompido: o cenário real fica em ~0,38 (NGA) a ~3,57 (USA).
	city->productivity_index = index > 0.0
		? std::clamp(index, 0.05, 8.0) : 0.0;
	++demo_status_static_version_;
	demo_status_invalidate();
}

void EconomySim::demo_set_income_gini(const String& city_id, double gini) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	DemoCity* city = find_demo_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr) {
		return;
	}
	// [0, 1]: 0 desliga (legado por escolaridade); dados corrompidos não viram
	// sigma negativo. A faixa real estimada por região fica em 0,33–0,47.
	city->income_gini = std::clamp(gini, 0.0, 1.0);
	// Recalcula as classes na hora: o status lido antes do primeiro tick já
	// publica a repartição ancorada (se o mundo ainda não tem preços, cai no
	// espelho dos estratos, sem inventar Gini).
	demo_refresh_income_classes(*city);
	++demo_status_static_version_;
	demo_status_invalidate();
}

// ---------------------------------------------------------------------------
// F7 — trabalho, educação e renda.
// ---------------------------------------------------------------------------
void EconomySim::demo_apply_labor_profile(DemoCity& city) {
	sim_core_.demo_apply_labor_profile(city);
}

void EconomySim::demo_set_labor_profile(const String& city_id,
		const Dictionary& profile) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	DemoCity* city = find_demo_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr) {
		return;
	}
	demo_apply_labor_profile(*city);
	DemoPop& pop = city->pop;
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
			if (shares.size() >= kDemoEducationLevels) {
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
					level < kDemoEducationLevels && level < rates.size(); ++level) {
				pop.participation[level] = std::clamp(
					double(rates[level]), 0.0, 1.0);
			}
		} else {
			const double rate = std::clamp(double(value), 0.0, 1.0);
			for (int level = 0; level < kDemoEducationLevels; ++level) {
				pop.participation[level] = rate;
			}
		}
	}
	if (profile.has("wage_by_level")) {
		const Variant value = profile["wage_by_level"];
		if (value.get_type() == Variant::ARRAY) {
			const Array wages = value;
			for (int level = 0;
					level < kDemoEducationLevels && level < wages.size(); ++level) {
				pop.wage_by_level[level] = std::max(0.0, double(wages[level]));
			}
		}
	}
	pop.labor_force = 0.0;
	for (int level = 0; level < kDemoEducationLevels; ++level) {
		const double studying = level < kDemoEducationLevels - 1
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
	demo_refresh_strata(pop);
	demo_status_invalidate();
}

void EconomySim::demo_set_minimum_wage(const String& nation, double value) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
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
		for (DemoCity& city : ws_.cities) {
			if (city.nation != tag) {
				continue;
			}
			for (int level = 0; level < kDemoEducationLevels; ++level) {
				city.pop.wage_by_level[level] = std::max(
					city.pop.wage_by_level[level], floor);
			}
		}
	}
	demo_status_invalidate();
}

void EconomySim::demo_set_wage_rigidity(double value) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	ws_.wage_rigidity = std::clamp(value, 0.0, 0.25);
	demo_status_invalidate();
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

void EconomySim::demo_set_sector_employment(const Variant& data) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	if (data.get_type() != Variant::DICTIONARY) {
		return;
	}
	const Dictionary root = data;
	const Array anchors = root.get("anchors", Array());
	std::vector<DemoSectorAnchor> parsed;
	parsed.reserve(anchors.size());
	for (int index = 0; index < anchors.size(); ++index) {
		if (anchors[index].get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary entry = anchors[index];
		DemoSectorAnchor anchor;
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
		[](const DemoSectorAnchor& a, const DemoSectorAnchor& b) {
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
	demo_status_invalidate();
}

// ---------------------------------------------------------------------------
// F8 — SERVIÇOS
// ---------------------------------------------------------------------------

void EconomySim::demo_set_services(const Variant& services) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
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
	if (sectors.size() != kDemoMaxServiceSectors) {
		return;
	}
	std::vector<DemoServiceSector> parsed;
	std::vector<std::vector<std::string>> service_ids;
	parsed.reserve(kDemoMaxServiceSectors);
	service_ids.reserve(kDemoMaxServiceSectors);
	for (int index = 0; index < sectors.size(); ++index) {
		if (sectors[index].get_type() != Variant::DICTIONARY) {
			return;
		}
		const Dictionary d = sectors[index];
		DemoServiceSector sector;
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
		for (int level = 0; level < kDemoMaxNeedsLevels; ++level) {
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
			const int product = demo_product_index(
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
		for (int level = 0; level < kDemoEducationLevels; ++level) {
			sector.education_mix[level] = level < mix.size()
				? std::max(0.0, static_cast<double>(mix[level])) : 0.0;
			mix_sum += sector.education_mix[level];
		}
		if (mix_sum <= 1e-9) {
			sector.education_mix[0] = 1.0;
		} else {
			for (int level = 0; level < kDemoEducationLevels; ++level) {
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
	for (DemoCity& city : ws_.cities) {
		for (int sector = 0; sector < kDemoMaxServiceSectors; ++sector) {
			DemoServiceMarket& market = city.service_markets[sector];
			market.base_price = ws_.service_sectors[static_cast<std::size_t>(sector)].price;
			if (market.price <= 0.0 || !std::isfinite(market.price)) {
				market.price = market.base_price;
			}
		}
	}
	if (!ws_.cities.empty() && ws_.service_businesses.empty()) {
		demo_seed_services();
	}
}

Array EconomySim::demo_get_service_sectors() const {
	Array out;
	for (const DemoServiceSector& sector : ws_.service_sectors) {
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
		for (int level = 0; level < kDemoEducationLevels; ++level) {
			mix.append(sector.education_mix[level]);
		}
		entry["education_mix"] = mix;
		entry["source"] = String(sector.source.c_str());
		out.append(entry);
	}
	return out;
}

Dictionary EconomySim::demo_get_services_detail(const String& city_id) const {
	Dictionary out;
	const std::string id = std::string(city_id.utf8().get_data());
	const auto found = ws_.city_lookup.find(id);
	if (found == ws_.city_lookup.end()) {
		out["error"] = "cidade inexistente";
		return out;
	}
	const DemoCity& city = ws_.cities[static_cast<std::size_t>(found->second)];
	out["city"] = city_id;
	out["nation"] = String(city.nation.c_str());
	out["name"] = String(city.name.c_str());
	Array sectors;
	for (int sector = 0; sector < static_cast<int>(ws_.service_sectors.size()); ++sector) {
		const DemoServiceSector& definition =
			ws_.service_sectors[static_cast<std::size_t>(sector)];
		const DemoServiceMarket& market = city.service_markets[sector];
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
		for (int level = 0; level < kDemoMaxNeedsLevels; ++level) {
			pop_demand.append(market.pop_demand[level]);
		}
		entry["pop_demand"] = pop_demand;
		sectors.append(entry);
	}
	out["sectors"] = sectors;
	Array businesses;
	for (const int index : ws_.service_by_city[static_cast<std::size_t>(found->second)]) {
		const DemoServiceBusiness& business =
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
	for (int level = 0; level < kDemoEducationLevels; ++level) {
		class_population.append(city.pop.class_population[level]);
	}
	for (int level = 0; level < kDemoIncomeLevels; ++level) {
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

void EconomySim::demo_set_service_capacity(const String& city_id,
		const String& sector, double units_per_day) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	DemoCity* city = find_demo_city(std::string(city_id.utf8().get_data()));
	const int sector_index = demo_service_sector_index(
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
			const DemoServiceSector& definition =
				ws_.service_sectors[static_cast<std::size_t>(sector_index)];
			demo_service_open(*city, sector_index, units_per_day,
				units_per_day * definition.capital_per_unit);
		}
		return;
	}
	const double each = units_per_day / static_cast<double>(local.size());
	for (const int index : local) {
		DemoServiceBusiness& business =
			ws_.service_businesses[static_cast<std::size_t>(index)];
		business.capacity = each;
		business.jobs = each
			* ws_.service_sectors[static_cast<std::size_t>(sector_index)].jobs_per_unit;
		business.workers = business.jobs;
		business.loss_days = 0;
	}
}

void EconomySim::demo_set_service_business_state(int business_id, int loss_days,
		double profit) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	for (DemoServiceBusiness& business : ws_.service_businesses) {
		if (business.id != business_id) {
			continue;
		}
		business.loss_days = std::max(0, loss_days);
		business.profit = profit;
		return;
	}
}

int EconomySim::demo_get_service_business_count() const {
	int count = 0;
	for (const DemoServiceBusiness& business : ws_.service_businesses) {
		if (!business.bankrupt) {
			++count;
		}
	}
	return count;
}

int EconomySim::demo_get_service_bankruptcies() const {
	return ws_.service_bankruptcies;
}

Dictionary EconomySim::demo_invest(const String& city_id, const String& building_id,
		double amount) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	Dictionary result;
	result["invested"] = 0.0;
	result["capacity_added"] = 0.0;
	result["error"] = "";

	DemoCity* city = find_demo_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr) {
		result["error"] = "cidade desconhecida";
		return result;
	}
	DemoBuilding* building = find_demo_building(city->id,
		std::string(building_id.utf8().get_data()));
	if (building == nullptr) {
		result["error"] = "construção desconhecida";
		return result;
	}
	if (amount <= 0.0) {
		result["error"] = "valor inválido";
		return result;
	}
	if (city->firm_cash < amount) {
		result["error"] = "capital insuficiente";
		return result;
	}
	if (building->input_product < 0
			&& city->suitability[building->output_product] <= 0.0) {
		result["error"] = "sem aptidão territorial";
		return result;
	}
	if (building->input_product < 0) {
		const DemoDeposit& deposit = city->deposits[building->output_product];
		if (deposit.tracked && deposit.remaining_kt <= 0.0) {
			result["error"] = "reserva esgotada";
			return result;
		}
	}

	const double capacity_added = amount / std::max(1.0, building->capital_per_capacity);

	if (instant_construction_) {
		city->firm_cash -= amount;
		// A expansão é construída por serviços domésticos: o dinheiro vira
		// receita de construção (contada no PIB do próximo tick) e não
		// desaparece em um caixa inerte.
		city->service_cash += amount;
		city->construction_revenue_pending += amount;
		building->capacity += capacity_added;
		result["invested"] = amount;
		result["capacity_added"] = capacity_added;
		demo_status_invalidate();
		return result;
	}

	// §15: expansão vira projeto físico (mesma regra de obra nova). O dono
	// compromete o custo (limite do surto) e paga conforme a obra avança; a
	// capacidade entra ao concluir. Anti-duplicação: uma expansão em andamento
	// por prédio (a IA expandiria o mesmo prédio todo dia durante a obra).
	for (const DemoConstructionProject& in_flight : ws_.construction_projects) {
		if (in_flight.expand_existing
				&& in_flight.building_id == building->id
				&& in_flight.city == city->id) {
			result["error"] = "obra em andamento";
			return result;
		}
	}
	const double committed =
		demo_committed_construction(ws_, city->id, "private");
	if (committed + amount > city->firm_cash) {
		result["error"] = "capital insuficiente";
		return result;
	}

	DemoConstructionProject project;
	project.id = building->id + "-EXP";
	project.building_id = building->id;
	project.city = city->id;
	project.owner = "private";
	project.resource = building->resource;
	project.input_product = building->input_product;
	project.output_product = building->output_product;
	project.recipe = building->recipe;
	project.capacity = capacity_added;
	project.input_per_output = building->input_per_output;
	for (int level = 0; level < kDemoEducationLevels; ++level) {
		project.education_mix[level] = building->education_mix[level];
	}
	project.jobs = building->jobs * (capacity_added / std::max(1e-9, building->capacity));
	project.wage = building->wage;
	project.opex = building->opex * (capacity_added / std::max(1e-9, building->capacity));
	project.expand_existing = true;
	project.total_cost = amount;
	sim_core_.demo_add_construction_project(project);
	result["invested"] = amount;
	result["capacity_added"] = capacity_added;
	result["project_id"] = String(project.id.c_str());
	demo_status_invalidate();
	return result;
}

Dictionary EconomySim::demo_build(const String& city_id, const String& building_id,
		const String& resource, int input_product, int output_product,
		double capacity, double jobs, double wage, double opex,
		double input_per_output, const String& owner, double cost,
		const String& recipe_id) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	Dictionary result;
	result["created"] = false;
	result["cost"] = 0.0;
	result["error"] = "";

	DemoCity* city = find_demo_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr) {
		result["error"] = "cidade desconhecida";
		return result;
	}
	const std::string id(building_id.utf8().get_data());
	const std::string owner_name(owner.utf8().get_data());
	if (id.empty()) {
		result["error"] = "identificador vazio";
		return result;
	}
	if (find_demo_building(city->id, id) != nullptr) {
		result["error"] = "construção já existe";
		return result;
	}
	if (output_product < 0 || output_product >= ws_.products) {
		result["error"] = "produto inválido";
		return result;
	}
	if (input_product < -1 || input_product >= ws_.products) {
		result["error"] = "insumo inválido";
		return result;
	}
	int recipe = -1;
	if (!recipe_id.is_empty()) {
		recipe = demo_recipe_index(recipe_id);
		if (recipe < 0) {
			result["error"] = "receita desconhecida";
			return result;
		}
		const DemoRecipe& chosen = ws_.recipes[static_cast<std::size_t>(recipe)];
		if (chosen.output != output_product) {
			result["error"] = "receita não produz o bem";
			return result;
		}
		// O primeiro insumo da receita alimenta os campos legados de exibição
		// (input_product/input_per_output); a fase 4 usa a receita completa.
		input_product = chosen.inputs[0];
		input_per_output = chosen.input_qty[0] / chosen.yield;
		if (jobs <= 0.0) {
			jobs = capacity * chosen.jobs_per_ton;
		}
		if (wage <= 0.0) {
			wage = chosen.wage;
		}
		if (opex <= 0.0) {
			opex = capacity * chosen.opex_per_ton;
		}
	}
	double education_mix[kDemoEducationLevels] = {0.90, 0.09, 0.01};
	if (recipe >= 0) {
		const DemoRecipe& chosen = ws_.recipes[static_cast<std::size_t>(recipe)];
		for (int level = 0; level < kDemoEducationLevels; ++level) {
			education_mix[level] = chosen.education_mix[level];
		}
	} else if (input_product >= 0) {
		// Fábrica legada (sem receita): mistura industrial padrão.
		education_mix[0] = 0.60;
		education_mix[1] = 0.30;
		education_mix[2] = 0.10;
	}
	if (capacity <= 0.0 || jobs < 0.0 || wage < 0.0 || opex < 0.0 || cost < 0.0) {
		result["error"] = "parâmetros inválidos";
		return result;
	}
	if (recipe < 0 && input_product < 0
			&& city->suitability[output_product] <= 0.0) {
		result["error"] = "sem aptidão territorial";
		return result;
	}
	if (recipe < 0 && input_product < 0) {
		const DemoDeposit& deposit = city->deposits[output_product];
		if (deposit.tracked && deposit.remaining_kt <= 0.0) {
			result["error"] = "reserva esgotada";
			return result;
		}
	}
	const bool state_owned = owner_name == "state";

	if (instant_construction_) {
		// Hook de teste (construção instantânea): comportamento anterior —
		// prédio pronto na hora e custo integral debitado do dono.
		if (state_owned) {
			if (city->government_budget < cost) {
				result["error"] = "orçamento insuficiente";
				return result;
			}
			city->government_budget -= cost;
		} else {
			if (city->firm_cash < cost) {
				result["error"] = "capital insuficiente";
				return result;
			}
			city->firm_cash -= cost;
		}

		DemoBuilding building;
		building.id = id;
		building.city = city->id;
		building.kind = 0;  // tipo (0=fazenda); o produto fica em output_product
		building.resource = std::string(resource.utf8().get_data());
		building.owner = state_owned ? "state" : "private";
		building.input_product = input_product;
		building.output_product = output_product;
		building.recipe = recipe;
		building.capacity = capacity;
		building.input_per_output = input_per_output;
		for (int level = 0; level < kDemoEducationLevels; ++level) {
			building.education_mix[level] = education_mix[level];
		}
		building.capital_per_capacity = capacity > 0.0
			? std::max(1.0, cost / capacity) : kDemoCapitalPerCapacity;
		building.jobs = jobs;
		building.wage = wage;
		building.opex = opex;
		// Fábrica nova começa em rampa (25% da capacidade): evita o pico de
		// produção/oferta no primeiro dia e deixa a curva de preço reagir.
		if (recipe >= 0 || input_product >= 0) {
			building.output_scale = 0.25;
			building.last_utilization = 0.25;
		}
		ws_.buildings.push_back(building);
		// Índice por cidade: insere SÓ a construção nova. O tick reconstrói o
		// índice no início, após remover falidas.
		const std::size_t city_index =
			static_cast<std::size_t>(city - ws_.cities.data());
		if (city_index < ws_.buildings_by_city.size()) {
			ws_.buildings_by_city[city_index].push_back(
				static_cast<int>(ws_.buildings.size() - 1));
		} else {
			demo_rebuild_indexes();
		}

		// O investimento é pago ao setor de construção doméstico e entra no PIB
		// do próximo tick como receita de construção.
		city->service_cash += cost;
		city->construction_revenue_pending += cost;
		result["created"] = true;
		result["cost"] = cost;
		demo_status_invalidate();
		return result;
	}

	// §15: obra vira PROJETO físico. O dono COMPROMETE o custo (limite do surto
	// de obras: o total comprometido não pode estourar o caixa atual), mas paga
	// conforme a obra avança; sem material/sem dinheiro a obra PARA (não falha).
	// Anti-duplicação: não abre obra nova do MESMO produto enquanto outra está
	// em andamento na cidade (a IA não enxerga obra em voo no status — sem o
	// guarda ela empilhava uma mina/fazenda por dia e, ao concluírem, a
	// sobreoferta falia a coorte inteira).
	for (const DemoConstructionProject& in_flight : ws_.construction_projects) {
		if (!in_flight.expand_existing && in_flight.city == city->id
				&& in_flight.output_product == output_product) {
			result["error"] = "obra em andamento";
			return result;
		}
	}
	const std::string commit_owner = state_owned ? "state" : "private";
	const double committed = demo_committed_construction(ws_, city->id, commit_owner);
	const double commit_budget = state_owned ? city->government_budget
		: city->firm_cash;
	if (committed + cost > commit_budget) {
		result["error"] = state_owned ? "orçamento insuficiente"
			: "capital insuficiente";
		return result;
	}

	DemoConstructionProject project;
	project.id = id;
	project.building_id = id;
	project.city = city->id;
	project.owner = state_owned ? "state" : "private";
	project.resource = std::string(resource.utf8().get_data());
	project.input_product = input_product;
	project.output_product = output_product;
	project.recipe = recipe;
	project.capacity = capacity;
	project.input_per_output = input_per_output;
	for (int level = 0; level < kDemoEducationLevels; ++level) {
		project.education_mix[level] = education_mix[level];
	}
	project.jobs = jobs;
	project.wage = wage;
	project.opex = opex;
	project.total_cost = cost;
	sim_core_.demo_add_construction_project(project);
	result["created"] = true;
	result["cost"] = cost;
	result["project_id"] = String(id.c_str());
	demo_status_invalidate();
	return result;
}

// =====================================================================
// F9 — investimento e propriedade
// =====================================================================

void EconomySim::demo_seed_groups() {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	sim_core_.demo_seed_groups();
	++demo_status_static_version_;
	demo_status_invalidate();
}

int EconomySim::demo_set_group_names(const Dictionary& names) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	int applied = 0;
	for (const Variant& key : names.keys()) {
		const std::string id(String(key).utf8().get_data());
		DemoGroup* group = find_demo_group(id);
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
		demo_status_invalidate();
	}
	return applied;
}

Array EconomySim::demo_get_groups() const {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	struct GroupStats {
		int assets = 0;
		double capacity = 0.0;
		double employed = 0.0;
		std::set<std::string> countries;
	};
	std::vector<GroupStats> stats(ws_.groups.size());
	for (const DemoBuilding& building : ws_.buildings) {
		std::string group_id;
		std::string origin;
		const int kind = demo_owner_kind(building.owner, &group_id, &origin);
		if (kind != 1 && kind != 2) {
			continue;
		}
		const int index = demo_group_index(group_id);
		if (index < 0) {
			continue;
		}
		GroupStats& row = stats[static_cast<std::size_t>(index)];
		row.assets += 1;
		row.capacity += building.capacity * building.output_scale;
		row.employed += building.workers;
		if (const DemoCity* city = find_demo_city(building.city)) {
			row.countries.insert(city->nation);
		}
	}
	std::vector<int> order(ws_.groups.size());
	for (std::size_t index = 0; index < order.size(); ++index) {
		order[index] = static_cast<int>(index);
	}
	std::sort(order.begin(), order.end(), [this](int a, int b) {
		const DemoGroup& ga = ws_.groups[static_cast<std::size_t>(a)];
		const DemoGroup& gb = ws_.groups[static_cast<std::size_t>(b)];
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
		const DemoGroup& group = ws_.groups[static_cast<std::size_t>(index)];
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

Array EconomySim::demo_get_group_ranking() const {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	// Ordem idêntica à de demo_get_groups (faturamento, caixa, id), sem a
	// varredura de construções: a IA precisa do caixa do dia e da vez de cada
	// grupo, não dos ativos/países. Evita 1 std::set + 1 entrada de ranking por
	// grupo a cada dia de decisão.
	std::vector<int> order(ws_.groups.size());
	for (std::size_t index = 0; index < order.size(); ++index) {
		order[index] = static_cast<int>(index);
	}
	std::sort(order.begin(), order.end(), [this](int a, int b) {
		const DemoGroup& ga = ws_.groups[static_cast<std::size_t>(a)];
		const DemoGroup& gb = ws_.groups[static_cast<std::size_t>(b)];
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
		const DemoGroup& group = ws_.groups[static_cast<std::size_t>(index)];
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

Dictionary EconomySim::demo_get_group(const String& group_id) const {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	const std::string id(group_id.utf8().get_data());
	const int index = demo_group_index(id);
	Dictionary entry;
	if (index < 0) {
		entry["error"] = "grupo desconhecido";
		return entry;
	}
	const DemoGroup& group = ws_.groups[static_cast<std::size_t>(index)];
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

Dictionary EconomySim::demo_fund_group(const String& group_id, double amount) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	Dictionary result;
	result["funded"] = false;
	result["moved"] = 0.0;
	result["error"] = "";
	DemoGroup* group = find_demo_group(std::string(group_id.utf8().get_data()));
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
	for (DemoCity& city : ws_.cities) {
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
	demo_status_invalidate();
	return result;
}

Dictionary EconomySim::demo_group_invest(const String& group_id,
		const String& city_id, const String& building_id, const String& resource,
		int output_product, double capacity, const String& recipe_id,
		const String& partner_group) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	Dictionary result;
	result["created"] = false;
	result["cost"] = 0.0;
	result["error"] = "";
	const std::string gid(group_id.utf8().get_data());
	DemoGroup* group = find_demo_group(gid);
	if (group == nullptr) {
		result["error"] = "grupo desconhecido";
		return result;
	}
	DemoCity* city = find_demo_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr) {
		result["error"] = "cidade desconhecida";
		return result;
	}
	const std::string id(building_id.utf8().get_data());
	if (id.empty()) {
		result["error"] = "identificador vazio";
		return result;
	}
	if (find_demo_building(city->id, id) != nullptr) {
		result["error"] = "construção já existe";
		return result;
	}
	if (output_product < 0 || output_product >= ws_.products) {
		result["error"] = "produto inválido";
		return result;
	}
	if (capacity <= 0.0) {
		result["error"] = "capacidade inválida";
		return result;
	}
	const std::string sector = output_product < static_cast<int>(ws_.product_sectors.size())
		? ws_.product_sectors[static_cast<std::size_t>(output_product)]
		: std::string();
	int recipe = -1;
	double capital_per_capacity = kDemoCapitalPerCapacity;
	double jobs = 0.0;
	double wage = 0.0;
	double opex = 0.0;
	double education_mix[kDemoEducationLevels] = {0.90, 0.09, 0.01};
	if (!recipe_id.is_empty()) {
		recipe = demo_recipe_index(recipe_id);
		if (recipe < 0) {
			result["error"] = "receita desconhecida";
			return result;
		}
		const DemoRecipe& chosen = ws_.recipes[static_cast<std::size_t>(recipe)];
		if (chosen.output != output_product) {
			result["error"] = "receita não produz o bem";
			return result;
		}
		capital_per_capacity = std::max(1.0, chosen.capital_per_capacity);
		jobs = capacity * chosen.jobs_per_ton;
		wage = chosen.wage;
		opex = capacity * chosen.opex_per_ton;
		for (int level = 0; level < kDemoEducationLevels; ++level) {
			education_mix[level] = chosen.education_mix[level];
		}
	} else if (city->suitability[output_product] <= 0.0) {
		result["error"] = "sem aptidão territorial";
		return result;
	} else {
		// Produção primária por grupo: mesmos coeficientes da fazenda inicial
		// (estimativa declarada; o núcleo não tem tabela por produto). Sem
		// folha/opex o lucro ficaria inflado e o emprego zerado.
		jobs = capacity * kDemoJobsPerTon;
		wage = kDemoFarmWage;
		opex = capacity * kDemoOpexPerTon;
	}
	const bool foreign = group->origin_nation != city->nation;
	double local_share = 0.0;
	std::string partner_id;
	DemoGroup* partner = nullptr;
	if (foreign) {
		const int mode = demo_fdi_mode(city->nation, sector);
		if (mode == 3) {
			result["error"] = "setor fechado ao capital estrangeiro";
			return result;
		}
		if (demo_nation_confidence(city->nation) < 0.5) {
			result["error"] = "confiança baixa após nacionalização";
			return result;
		}
		if (mode == 2) {
			partner_id = std::string(partner_group.utf8().get_data());
			if (partner_id.empty()) {
				result["error"] = "joint venture exige parceiro local";
				return result;
			}
			partner = find_demo_group(partner_id);
			if (partner == nullptr || partner->origin_nation != city->nation
					|| partner->state_owned) {
				result["error"] = "parceiro local inválido";
				return result;
			}
			const auto policy = ws_.fdi_lookup.find(city->nation + "|" + sector);
			local_share = policy == ws_.fdi_lookup.end()
				? 0.30 : std::clamp(
					ws_.fdi_policies[static_cast<std::size_t>(policy->second)].min_local,
					0.0, 1.0);
			if (local_share <= 0.0) {
				local_share = 0.30;
			}
		} else if (mode == 1) {
			// Restrito: só quem já opera no setor do anfitrião.
			bool incumbent = false;
			for (const DemoBuilding& building : ws_.buildings) {
				if (building.owner.rfind("foreign:", 0) != 0
						|| building.output_product != output_product) {
					continue;
				}
				std::string owner_group;
				std::string origin;
				demo_owner_kind(building.owner, &owner_group, &origin);
				const DemoCity* host = find_demo_city(building.city);
				if (host != nullptr && host->nation == city->nation
						&& owner_group == gid) {
					incumbent = true;
					break;
				}
			}
			if (!incumbent) {
				result["error"] = "setor restrito a incumbentes";
				return result;
			}
		}
		if (recipe < 0) {
			const DemoDeposit& deposit = city->deposits[output_product];
			if (deposit.tracked && deposit.remaining_kt <= 0.0) {
				result["error"] = "reserva esgotada";
				return result;
			}
		}
	}
	const double cost = capacity * capital_per_capacity;
	const double partner_cost = cost * local_share;
	const double owner_cost = cost - partner_cost;

	if (instant_construction_) {
		if (group->cash + 1e-9 < owner_cost) {
			result["error"] = "capital insuficiente";
			result["cash"] = group->cash;
			result["cost"] = cost;
			return result;
		}
		if (partner != nullptr && partner->cash + 1e-9 < partner_cost) {
			result["error"] = "capital insuficiente do parceiro local";
			result["cost"] = cost;
			return result;
		}
		group->cash -= owner_cost;
		group->capex_total += owner_cost;
		if (partner != nullptr && partner_cost > 0.0) {
			partner->cash -= partner_cost;
			partner->capex_total += partner_cost;
		}

		DemoBuilding building;
		building.id = id;
		building.city = city->id;
		building.kind = 0;
		building.resource = std::string(resource.utf8().get_data());
		building.owner = foreign
			? "foreign:" + gid + "@" + group->origin_nation
			: "national:" + gid;
		building.input_product = recipe >= 0
			? ws_.recipes[static_cast<std::size_t>(recipe)].inputs[0] : -1;
		building.output_product = output_product;
		building.recipe = recipe;
		building.capacity = capacity;
		building.input_per_output = recipe >= 0
			? ws_.recipes[static_cast<std::size_t>(recipe)].input_qty[0]
				/ std::max(1e-9,
					ws_.recipes[static_cast<std::size_t>(recipe)].yield)
			: 0.0;
		building.capital_per_capacity = capital_per_capacity;
		building.jobs = jobs;
		building.wage = wage;
		building.opex = opex;
		for (int level = 0; level < kDemoEducationLevels; ++level) {
			building.education_mix[level] = education_mix[level];
		}
		building.local_share = local_share;
		building.partner_group = partner != nullptr ? partner->id : std::string();
		if (recipe >= 0) {
			building.output_scale = 0.25;
			building.last_utilization = 0.25;
		}
		ws_.buildings.push_back(building);
		const std::size_t city_index =
			static_cast<std::size_t>(city - ws_.cities.data());
		if (city_index < ws_.buildings_by_city.size()) {
			ws_.buildings_by_city[city_index].push_back(
				static_cast<int>(ws_.buildings.size() - 1));
		} else {
			demo_rebuild_indexes();
		}
		city->service_cash += cost;
		city->construction_revenue_pending += cost;
		result["created"] = true;
		result["cost"] = cost;
		result["owner"] = String(building.owner.c_str());
		result["local_share"] = local_share;
		demo_status_invalidate();
		return result;
	}

	// §15: obra vira projeto físico. O grupo financia a sua fatia conforme a
	// obra avança; o parceiro de JV paga a fatia local na hora (contribuição
	// adiantada ao serviço de construção). Sem material/sem dinheiro, a obra
	// PARA.
	// Anti-duplicação: não abre obra nova do MESMO produto enquanto outra está
	// em andamento na cidade (mesma regra do build do governo).
	for (const DemoConstructionProject& in_flight : ws_.construction_projects) {
		if (!in_flight.expand_existing && in_flight.city == city->id
				&& in_flight.output_product == output_product) {
			result["error"] = "obra em andamento";
			return result;
		}
	}
	const double committed = demo_committed_construction(ws_, city->id, gid);
	if (committed + owner_cost > group->cash) {
		result["error"] = "capital insuficiente";
		result["cash"] = group->cash;
		result["cost"] = cost;
		return result;
	}
	if (partner != nullptr) {
		const double pcommitted =
			demo_committed_construction(ws_, city->id, partner->id);
		if (pcommitted + partner_cost > partner->cash) {
			result["error"] = "capital insuficiente do parceiro local";
			result["cost"] = cost;
			return result;
		}
		partner->cash -= partner_cost;
		partner->capex_total += partner_cost;
		city->service_cash += partner_cost;
		city->construction_revenue_pending += partner_cost;
	}
	group->capex_total += owner_cost;

	DemoConstructionProject project;
	project.id = id;
	project.building_id = id;
	project.city = city->id;
	project.owner = foreign
		? "foreign:" + gid + "@" + group->origin_nation
		: "national:" + gid;
	project.group_id = gid;
	project.resource = std::string(resource.utf8().get_data());
	project.input_product = recipe >= 0
		? ws_.recipes[static_cast<std::size_t>(recipe)].inputs[0] : -1;
	project.output_product = output_product;
	project.recipe = recipe;
	project.capacity = capacity;
	project.input_per_output = recipe >= 0
		? ws_.recipes[static_cast<std::size_t>(recipe)].input_qty[0]
			/ std::max(1e-9,
				ws_.recipes[static_cast<std::size_t>(recipe)].yield)
		: 0.0;
	project.jobs = jobs;
	project.wage = wage;
	project.opex = opex;
	for (int level = 0; level < kDemoEducationLevels; ++level) {
		project.education_mix[level] = education_mix[level];
	}
	project.local_share = local_share;
	project.partner_group = partner != nullptr ? partner->id : std::string();
	project.total_cost = owner_cost;
	sim_core_.demo_add_construction_project(project);
	result["created"] = true;
	result["cost"] = cost;
	result["owner"] = String(project.owner.c_str());
	result["local_share"] = local_share;
	result["project_id"] = String(id.c_str());
	demo_status_invalidate();
	return result;
}

Dictionary EconomySim::demo_set_fdi_policy(const String& nation,
		const String& sector, const String& mode, double min_local) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
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
	DemoFdiPolicy policy;
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

Dictionary EconomySim::demo_set_sector_incentive(const String& nation,
		const String& sector, double tax_holiday, double subsidy) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	Dictionary result;
	result["ok"] = true;
	const std::string tag(nation.utf8().get_data());
	const std::string sector_id(sector.utf8().get_data());
	const std::string key = tag + "|" + sector_id;
	DemoSectorIncentive incentive;
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

Dictionary EconomySim::demo_create_state_company(const String& nation,
		const String& name, double capital) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	Dictionary result;
	result["created"] = false;
	result["error"] = "";
	const std::string tag(nation.utf8().get_data());
	if (tag.empty()) {
		result["error"] = "nação vazia";
		return result;
	}
	DemoGroup group;
	group.state_owned = true;
	group.origin_nation = tag;
	group.sector = "state";
	const String company_name = name.strip_edges();
	group.name = company_name.is_empty()
		? std::string() : std::string(company_name.utf8().get_data());
	int serial = 0;
	do {
		group.id = "S-" + demo_sane_token(tag) + "-" + std::to_string(serial);
		++serial;
	} while (ws_.group_lookup.find(group.id) != ws_.group_lookup.end());
	ws_.group_lookup[group.id] = static_cast<int>(ws_.groups.size());
	ws_.groups.push_back(group);
	DemoGroup* created = &ws_.groups.back();
	double remaining = std::max(0.0, capital);
	for (DemoCity& city : ws_.cities) {
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
	demo_status_invalidate();
	return result;
}

Dictionary EconomySim::demo_privatize(const String& city_id,
		const String& building_id, const String& buyer_group) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	Dictionary result;
	result["ok"] = false;
	result["error"] = "";
	DemoCity* city = find_demo_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr) {
		result["error"] = "cidade desconhecida";
		return result;
	}
	DemoBuilding* building = find_demo_building(city->id,
		std::string(building_id.utf8().get_data()));
	if (building == nullptr) {
		result["error"] = "ativo desconhecido";
		return result;
	}
	if (building->owner != "state") {
		result["error"] = "ativo não é estatal";
		return result;
	}
	DemoGroup* buyer = find_demo_group(std::string(buyer_group.utf8().get_data()));
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
		if (demo_fdi_mode(city->nation, sector) == 3) {
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
	result["ok"] = true;
	result["price"] = price;
	result["owner"] = String(building->owner.c_str());
	demo_status_invalidate();
	return result;
}

Dictionary EconomySim::demo_nationalize(const String& city_id,
		const String& building_id) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	Dictionary result;
	result["ok"] = false;
	result["error"] = "";
	DemoCity* city = find_demo_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr) {
		result["error"] = "cidade desconhecida";
		return result;
	}
	DemoBuilding* building = find_demo_building(city->id,
		std::string(building_id.utf8().get_data()));
	if (building == nullptr) {
		result["error"] = "ativo desconhecido";
		return result;
	}
	std::string group_id;
	std::string origin;
	const int kind = demo_owner_kind(building->owner, &group_id, &origin);
	if (kind == 0) {
		result["error"] = "ativo já é estatal";
		return result;
	}
	const double book = building->capacity * building->capital_per_capacity;
	const std::string previous_owner = building->owner;
	const double compensation = std::min(book * kDemoNationalizationCompensation,
		std::max(0.0, city->government_budget));
	if (compensation > 0.0) {
		city->government_budget -= compensation;
		if (DemoGroup* owner = find_demo_group(group_id)) {
			owner->cash += compensation;
		} else {
			city->firm_cash += compensation;
		}
	}
	building->owner = "state";
	building->local_share = 0.0;
	building->partner_group.clear();
	// Derruba a confiança do anfitrião por um período: o IDE novo fica
	// bloqueado enquanto a confiança estiver abaixo de 0,5.
	ws_.confidence[city->nation] = kDemoNationalizationConfidence;
	ws_.confidence_penalty_day[city->nation] = ws_.day + kDemoNationalizationDays;
	result["ok"] = true;
	result["compensation"] = compensation;
	result["confidence"] = demo_nation_confidence(city->nation);
	result["previous_owner"] = String(previous_owner.c_str());
	demo_status_invalidate();
	return result;
}

Dictionary EconomySim::demo_get_investment_status() const {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	Dictionary out;
	out["groups"] = demo_get_groups();
	struct NationOwnership {
		double production[3] = {};
		double employment[3] = {};
		double capacity[3] = {};
		int assets[3] = {};
	};
	std::map<std::string, NationOwnership> nations;
	for (const DemoBuilding& building : ws_.buildings) {
		const DemoCity* city = find_demo_city(building.city);
		if (city == nullptr) {
			continue;
		}
		std::string group_id;
		std::string origin;
		const int kind = demo_owner_kind(building.owner, &group_id, &origin);
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
		record["confidence"] = demo_nation_confidence(entry.first);
		ownership[String(entry.first.c_str())] = record;
	}
	out["nations"] = ownership;

	Array flows;
	std::set<std::string> flow_nations;
	for (const auto& entry : ws_.group_flows) {
		flow_nations.insert(entry.first);
	}
	for (const std::string& nation : flow_nations) {
		const DemoGroupFlow flow = ws_.group_flows.at(nation);
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
	for (const DemoFdiPolicy& policy : ws_.fdi_policies) {
		Dictionary record;
		record["nation"] = String(policy.nation.c_str());
		record["sector"] = String(policy.sector.c_str());
		record["mode"] = policy.mode;
		record["min_local"] = policy.min_local;
		policies.append(record);
	}
	out["fdi_policies"] = policies;
	Array incentives;
	for (const DemoSectorIncentive& incentive : ws_.incentives) {
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

Dictionary EconomySim::demo_get_investment_signals() const {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
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
	for (const DemoFdiPolicy& policy : ws_.fdi_policies) {
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
	for (const DemoSectorIncentive& incentive : ws_.incentives) {
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

bool EconomySim::demo_spend_government(const String& city_id, double amount) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	DemoCity* city = find_demo_city(std::string(city_id.utf8().get_data()));
	if (city == nullptr || amount <= 0.0 || city->government_budget < amount) {
		return false;
	}
	city->government_budget -= amount;
	// Serviço doméstico (prospecção, estudos, licenciamento).
	city->service_cash += amount;
	city->service_revenue += amount;
	city->service_profit += amount;
	city->gdp += amount;
	demo_status_invalidate();
	return true;
}

Dictionary EconomySim::demo_to_dict() const {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	return ogs::edge::to_dict(ws_, sim_core_);
}

void EconomySim::demo_from_dict(const Dictionary& data) {
	const std::lock_guard<std::recursive_mutex> demo_lock(demo_mutex_);
	// O save substitui o mundo: o cache de status (dicionários reaproveitados)
	// não pode sobreviver a ele.
	demo_status_slots_[0] = DemoStatusCache{};
	demo_status_slots_[1] = DemoStatusCache{};
	demo_status_live_ = -1;
	demo_status_built_day_ = -1;
	++demo_status_static_version_;
	++demo_status_deposit_version_;
	ogs::edge::from_dict(ws_, sim_core_, data);
}

} // namespace godot
