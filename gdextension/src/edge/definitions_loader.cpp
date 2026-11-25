#include "definitions_loader.h"

#include "economy_sim.h"
#include "sim/central_bank.h"
#include "sim/exchange.h"

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace godot {

namespace {

// Copia exata do helper de economy_sim.cpp (mesma semantica, mesmo efeito):
// seguranca numerica, nada de NaN/Inf no estado nem nos outputs.
inline double safe_nonneg(double v) {
	return std::isfinite(v) ? std::max(0.0, v) : 0.0;
}

} // namespace

using namespace ogs;

void EconomySim::set_currencies(const Variant& currencies) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	Array list;
	// Sem `regime` declarado no dado a taxa fica FIXA (`peg`). O dado Ã© do F1 e
	// nÃ£o traz a fonte do regime cambial (currencies.json: `regime: null`), entÃ£o
	// inventar flutuaÃ§Ã£o violaria a regra "nÃ£o invente dados" â€” e o drift diÃ¡rio
	// move preÃ§os/competitividade sem fonte. `regime: "float"` no JSON ativa o
	// drift por balanÃ§a.
	std::string default_regime = "peg";
	if (currencies.get_type() == Variant::DICTIONARY) {
		const Dictionary data = currencies;
		const Variant entries = data.get("currencies", Array());
		if (entries.get_type() == Variant::ARRAY) {
			list = entries;
		}
		// `regime: null` no dado (currencies.json nÃ£o tem fonte de regime) NÃƒO
		// Ã© flutuaÃ§Ã£o: sÃ³ o valor explÃ­cito "float" ativa o drift e o fator de
		// competitividade. Qualquer outro valor mantÃ©m a taxa FIXA.
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
	// Ãºltimo recurso (nunca inventa um PIB).
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
		// C2a (D-103): o regime da polÃ­tica nasce do regime jÃ¡ aceito pelo
		// F5 â€” nenhuma mecÃ¢nica nova. "band" (banda administrada) usa o mesmo
		// drift dentro da banda do fx.cpp; a intervenÃ§Ã£o Ã© C2b.
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
	// nÃ£o mais do "peg administrativo" Ãºnico. Moeda sem entrada no arquivo
	// mantÃ©m o que o dado declarou (nada Ã© inventado).
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
	// partida do peg (nenhuma paridade nominal Ã© publicada pelo FMI).
	ogs::exchange::initialize_anchors(ws_);

	// O preÃ§o interno estÃ¡ na Ã¢ncora: carregar moedas NÃƒO reescala preÃ§os. A
	// taxa sÃ³ entra na exibiÃ§Ã£o local e na competitividade float.
	if (ws_.active && !ws_.cities.empty()) {
		++status_static_version_;
		status_invalidate();
	}
}
void EconomySim::set_goods(const Array& goods) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	// Tabela de produtos: 21 primÃ¡rios nas posiÃ§Ãµes 0..20 (ordem do goods.json
	// legado) e os bens novos em `order` (21..112). O nÃºcleo nÃ£o lÃª arquivos:
	// todo o conteÃºdo entra por aqui, no padrÃ£o do set_goods antigo.
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
	// D-125: índices dos bens do equipamento e dos três graus do refino. A
	// resolução é aqui (o catálogo é a fonte); produto ausente = -1 e a regra
	// fica inerte (ausência explícita).
	ws_.efficiency.equipment_fuel_product = product_index(String("diesel"));
	ws_.efficiency.refinery_gasoline = product_index(String("gasoline"));
	ws_.efficiency.refinery_diesel = product_index(String("diesel"));
	ws_.efficiency.refinery_jet = product_index(String("jet_fuel"));

	// Mundo jÃ¡ inicializado: os produtos novos entram com o preÃ§o-base e sem
	// estoque. Os Ã­ndices existentes (0..previous_products-1) nÃ£o mudam.
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
	// F4: os Ã­ndices dos bens da frota dependem dos ids carregados. Se o mundo
	// jÃ¡ existe e ainda nÃ£o tem frota (set_goods chamado depois da
	// inicializaÃ§Ã£o, como no autoload), deriva agora.
	ws_.fleet_product_diesel = product_index(String("diesel"));
	ws_.fleet_product_jet = product_index(String("jet_fuel"));
	ws_.fleet_product_truck = product_index(String("truck"));
	ws_.fleet_product_ship = product_index(String("cargo_ship"));
	ws_.fleet_product_plane = product_index(String("cargo_plane"));
	ws_.fleet_product_parts = product_index(String("spare_parts"));
	if (ws_.active && ws_.fleets.empty()) {
		init_fleets();
	}
}
void EconomySim::set_recipes(const Array& recipes) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	ws_.recipes.clear();
	ws_.recipe_lookup.clear();
	ws_.recipes.reserve(static_cast<std::size_t>(recipes.size()));
	for (int index = 0; index < recipes.size(); ++index) {
		const Dictionary entry = recipes[index];
		// Co-produção (F0): `outputs[]` declara todas as saídas do lote, com a
		// quantidade de cada uma. A primeira é o produto principal (capacity,
		// demanda e preço de referência); as demais viram byproducts com a
		// quantidade do próprio dado — o motor já produzia múltiplas saídas,
		// isso só preserva a fração declarada em vez do coeficiente unitário.
		const Array outputs = entry.get("outputs", Array());
		const bool has_outputs = outputs.size() > 0;
		Dictionary first_output;
		if (has_outputs) {
			first_output = outputs[0];
		}
		const int output = has_outputs
			? product_index(String(first_output.get("good", "")))
			: product_index(String(entry.get("output", "")));
		if (output < 0) {
			continue;  // receita de bem que nÃ£o existe no goods.json carregado
		}
		Recipe recipe;
		recipe.id = std::string(String(entry.get("id", "")).utf8().get_data());
		recipe.output = output;
		const double declared_yield = has_outputs
			? static_cast<double>(first_output.get("qty", 1.0))
			: static_cast<double>(entry.get("yield", 1.0));
		recipe.yield = std::max(1e-9, safe_nonneg(declared_yield));
		recipe.capital_per_capacity = std::max(1.0, safe_nonneg(
			static_cast<double>(entry.get("capital_per_capacity", 2000.0))));
		recipe.jobs_per_ton = safe_nonneg(
			static_cast<double>(entry.get("jobs_per_ton", 7.5)));
		recipe.wage = safe_nonneg(static_cast<double>(entry.get("wage", 2.0)));
		recipe.opex_per_ton = safe_nonneg(
			static_cast<double>(entry.get("opex_per_ton", 2.5)));
		// G1: prazo de obra e classe da planta (opcionais). 0 = tabela atual;
		// `plant_class` é só metadado de status/i18n, não altera a simulação.
		const double construction_days =
			static_cast<double>(entry.get("construction_days", 0));
		recipe.construction_days = std::isfinite(construction_days)
			&& construction_days > 0.0
			? static_cast<int>(std::min(construction_days, 3650.0)) : 0;
		recipe.plant_class = std::string(
			String(entry.get("plant_class", "")).utf8().get_data());
		// F7: escolaridade das vagas. `education_mix` explÃ­cito vence a tabela
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
		// Com `outputs[]`, as saídas extras vêm da própria lista (qty do dado);
		// sem ele, o caminho legado lê `byproducts` (coeficiente unitário).
		Array byproducts = has_outputs
			? outputs : Array(entry.get("byproducts", Array()));
		const int first_byproduct = has_outputs ? 1 : 0;
		for (int slot = first_byproduct;
				slot < byproducts.size() && slot < kMaxRecipeInputs; ++slot) {
			const Dictionary item = byproducts[slot];
			const int product = product_index(String(item.get("good", "")));
			const double qty = safe_nonneg(static_cast<double>(item.get("qty", 1.0)));
			if (product < 0 || product == recipe.output || qty <= 0.0) {
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
			ws_.industry_seeded = true;  // o mundo jÃ¡ tem indÃºstria
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
		return;  // recipes.json ainda nÃ£o carregou
	}
	// Maiores cidades (capitais/megacidades) por populaÃ§Ã£o de simulaÃ§Ã£o.
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
	// R1/Q-014: as plantas de referÃªncia abaixo sÃ£o o estoque industrial
	// PRÃ‰-EXISTENTE do cenÃ¡rio (nÃ£o obra nova): nascem concluÃ­das. A obra fÃ­sica
	// (Â§15) segue valendo para tudo o que a IA/o jogador investe depois.
	const bool previous_bootstrap = bootstrap_construction_;
	bootstrap_construction_ = true;
	for (int slot = 0; slot < seed_cities; ++slot) {
		City& city = ws_.cities[static_cast<std::size_t>(order[slot])];
		// Capacidade de referÃªncia = 5% da necessidade alimentar diÃ¡ria da
		// cidade, em toneladas de farinha/pÃ£o. Ã‰ uma ESTIMATIVA de transiÃ§Ã£o
		// (Â§12.3), nÃ£o um dado observado; calibrada no laboratÃ³rio em F3.
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
	// F3: corrente final representativa (bem de POP) para a demanda final nÃ£o
	// ficar sÃ³ no papel. A IA ainda nÃ£o atravessa a cadeia inteira quando o
	// insumo nÃ£o existe (gate de `_input_available` + uma construÃ§Ã£o por
	// visita), entÃ£o as 3 maiores cidades com aptidÃ£o para a matÃ©ria-prima
	// recebem uma cadeia de referÃªncia cottonâ†’fabricâ†’clothes e woodâ†’celluloseâ†’
	// paper. Ã‰ ESTIMATIVA de transiÃ§Ã£o (Â§12.3), marcada `strategic`, nÃ£o dado
	// observado; as capacidades derivam da necessidade per capita do nÃ­vel.
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
		// Sem aptidÃ£o para a matÃ©ria-prima nÃ£o hÃ¡ cadeia local (nada inventado).
		if (city.suitability[raw_product] < 0.30) {
			return 0;
		}
		const double final_capacity = std::clamp(
			city.simulation_population * per_capita_day, 2.0, max_capacity);
		for (const auto& existing : ws_.buildings) {
			if (existing.city == city.id
					&& existing.id.find("SEED-" + std::string(final_id)) != std::string::npos) {
				return 0;  // jÃ¡ semeada
			}
		}
		const Recipe& mid = ws_.recipes[static_cast<std::size_t>(mid_recipe)];
		const Recipe& final = ws_.recipes[static_cast<std::size_t>(final_recipe)];
		// R2/Q-014: a lavoura bruta (algodÃ£o/madeira) nasce JUNTO da fÃ¡brica que
		// a consome â€” sem ela a matÃ©ria-prima nÃ£o existe no mundo (a IA excluÃ­a
		// essas lavouras, entÃ£o fabric/cellulose/paper ficavam sem oferta desde o
		// dia 0). Capacidade = o insumo que a transformaÃ§Ã£o consome; custo pelo
		// capital padrÃ£o por capacidade das primÃ¡rias (ESTIMATIVA Â§12.3).
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
		// SÃ³ capital com folga alimentar: a fÃ¡brica de referÃªncia nÃ£o pode
		// competir com a comida de uma naÃ§Ã£o importadora (EGY/SAH). A folga Ã©
		// medida pela capacidade primÃ¡ria de alimento jÃ¡ instalada Ã— aptidÃ£o.
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
			// basic/clothing: 0,0004 t/mÃªs por habitante.
			built += seed_final_chain(city, "cotton", "fabric", "clothes",
				0.0004 / 30.0, 20.0);
			// institutional/government paper: 0,005 Ã— Ã¢ncora alimentar/dia.
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
		UtilityFunctions::print(String(("economia: bootstrap industrial (estimativa) â€” "
			+ std::to_string(created) + " plantas de referÃªncia semeadas").c_str()));
	}
}
void EconomySim::set_needs(const Dictionary& needs) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	// Cesta alimentar a partir do nÃ­vel bÃ¡sico de needs.json. SÃ³ a categoria
	// `food` tem Ã¢ncora fÃ­sica no nÃºcleo hoje (kNeedPerCapita); as demais
	// categorias ficam para F3.
	const Dictionary levels = needs.get("levels", Dictionary());
	const Dictionary basic = levels.get("basic", Dictionary());
	const Dictionary categories = basic.get("categories", Dictionary());
	const Dictionary food = categories.get("food", Dictionary());
	const Array substitutes = food.get("substitutes", Array());
	if (substitutes.is_empty()) {
		return;  // mantÃ©m a cesta validada (5 culturas)
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
		// MantÃ©m a ordem data-driven, mas garante os 5 grÃ£os validados no
		// inÃ­cio quando estÃ£o entre os substitutos (a ordem de consumo importa
		// sob orÃ§amento apertado: os mais baratos/legados primeiro).
		products.push_back(product);
		factors.push_back(factor);
	}
	if (products.size() < 2) {
		return;
	}
	// Ordem de consumo: os 5 grÃ£os/raÃ­zes validados primeiro (a ordem importa
	// sob orÃ§amento apertado e no fallback "sem estoque"); os processados
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

	// F3: nÃ­veis completos (basic/life/luxury). A cesta alimentar acima Ã© o
	// contrato validado e continua com o laÃ§o prÃ³prio no tick; aqui entram os
	// nÃ­veis e as categorias nÃ£o alimentares, com a ordem dos substitutos
	// preservada (do melhor ao Ãºltimo caso).
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
			// Cada categoria declara a tabela de equivalÃªncia que usa
			// (ex.: basic/food â†’ equivalence.calories).
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
	// Consumo institucional (decisÃ£o Â§12.10): o Estado compra paper/computer.
	// O dado de needs.json lista os BENS (`institutional.government`); a
	// quantidade nÃ£o existe na spec, entÃ£o usa-se uma fraÃ§Ã£o ESTIMADA da Ã¢ncora
	// alimentar (kNeedPerCapita), declarada na tabela abaixo e registrada em
	// GAME_STATE/perf como estimativa â€” nunca como mediÃ§Ã£o. Se um needs.json
	// futuro trouxer `institutional_per_capita_t_month.government.<bem>`, ele
	// vence a estimativa.
	const Dictionary institutional = needs.get("institutional", Dictionary());
	const Array government = institutional.get("government", Array());
	const Dictionary quantities_root =
		needs.get("institutional_per_capita_t_month", Dictionary());
	const Dictionary quantities = quantities_root.get("government", Dictionary());
	static const struct {
		const char* id;
		double anchor_share;  // fraÃ§Ã£o de kNeedPerCapita por dia
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

} // namespace godot
