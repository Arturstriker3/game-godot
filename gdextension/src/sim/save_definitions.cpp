// Implementação de save_definitions.h: bloco ESTÁTICO do WorldState (definições
// carregadas de data/definitions + parâmetros de balanceamento) <-> Value JSON.
//
// Layout do objeto (ordem fixa — é ela que dá o hash/dump estável):
//   {
//     "products":   { "count", "names", "base_prices", "sectors", "durable" },
//     "recipes":    [ { receita } ],
//     "needs":      { "loaded", "food_level", "durable_decay", "levels",
//                     "food_products", "food_factors",
//                     "government_products", "government_per_capita_day" },
//     "services":   { "loaded", "government_share", "sectors" },
//     "employment": { "loaded", "anchors", <campos agri_*/industry_*> },
//     "balance":    { <parâmetros do demo_set_balance> }
//   }
//
// Regra de leitura: nenhuma chave é obrigatória. Chave ausente (ou de tipo
// errado) deixa o campo do WorldState como está — é o default do struct.
// Nenhum laço percorre unordered_map: os índices derivados são reconstruídos.

#include "save_definitions.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace ogs {
namespace save {
namespace {

// ---------------------------------------------------------------------------
// Leitura tolerante (nunca lança, nunca exige a chave)
// ---------------------------------------------------------------------------

int read_scalar_int(const Value& value, int fallback) {
	const Value::Type type = value.get_type();
	if (type != Value::INT && type != Value::FLOAT) {
		return fallback;
	}
	return static_cast<int>(value);
}

double read_scalar_double(const Value& value, double fallback) {
	const Value::Type type = value.get_type();
	if (type != Value::INT && type != Value::FLOAT) {
		return fallback;
	}
	return static_cast<double>(value);
}

bool read_scalar_bool(const Value& value, bool fallback) {
	return value.get_type() == Value::BOOL ? static_cast<bool>(value) : fallback;
}

std::string read_scalar_string(const Value& value,
		const std::string& fallback = std::string()) {
	return value.get_type() == Value::STRING
		? static_cast<std::string>(value) : fallback;
}

int read_int(const Value& object, const char* key, int fallback) {
	return read_scalar_int(object[key], fallback);
}

double read_double(const Value& object, const char* key, double fallback) {
	return read_scalar_double(object[key], fallback);
}

bool read_bool(const Value& object, const char* key, bool fallback) {
	return read_scalar_bool(object[key], fallback);
}

std::string read_string(const Value& object, const char* key,
		const std::string& fallback = std::string()) {
	return read_scalar_string(object[key], fallback);
}

// Sub-objeto/sub-array: nullptr quando a chave não existe ou tem outro tipo.
const Value* read_object(const Value& object, const char* key) {
	const Value& slot = object[key];
	return slot.get_type() == Value::DICTIONARY ? &slot : nullptr;
}

const Value* read_array(const Value& object, const char* key) {
	const Value& slot = object[key];
	return slot.get_type() == Value::ARRAY ? &slot : nullptr;
}

std::vector<int> read_int_vector(const Value& array) {
	std::vector<int> parsed;
	parsed.reserve(static_cast<std::size_t>(array.size()));
	for (int index = 0; index < array.size(); ++index) {
		parsed.push_back(read_scalar_int(array[index], 0));
	}
	return parsed;
}

std::vector<double> read_double_vector(const Value& array) {
	std::vector<double> parsed;
	parsed.reserve(static_cast<std::size_t>(array.size()));
	for (int index = 0; index < array.size(); ++index) {
		parsed.push_back(read_scalar_double(array[index], 0.0));
	}
	return parsed;
}

std::vector<std::string> read_string_vector(const Value& array) {
	std::vector<std::string> parsed;
	parsed.reserve(static_cast<std::size_t>(array.size()));
	for (int index = 0; index < array.size(); ++index) {
		parsed.push_back(read_scalar_string(array[index]));
	}
	return parsed;
}

// ---------------------------------------------------------------------------
// Escrita
// ---------------------------------------------------------------------------

// `Value` nasce NIL: um array sem itens sairia como `null` no dump se não
// fosse tipado explicitamente aqui. Todo array emitido passa por esta função.
Value empty_array() {
	Value value;
	value.resize(0);
	return value;
}

Value array_of(const std::vector<int>& values) {
	Value out = empty_array();
	for (const int value : values) {
		out.append(Value(value));
	}
	return out;
}

Value array_of(const std::vector<double>& values) {
	Value out = empty_array();
	for (const double value : values) {
		out.append(Value(value));
	}
	return out;
}

Value array_of(const std::vector<std::string>& values) {
	Value out = empty_array();
	for (const std::string& value : values) {
		out.append(Value(value));
	}
	return out;
}

Value array_of(const double* values, int count) {
	Value out = empty_array();
	for (int index = 0; index < count; ++index) {
		out.append(Value(values[index]));
	}
	return out;
}

// ---------------------------------------------------------------------------
// 1. Produtos (goods.json -> demo_set_goods)
// ---------------------------------------------------------------------------

Value products_to(const WorldState& ws) {
	// `base_prices` é fixo em kDemoMaxProducts: só os `products` ativos têm
	// significado (o resto é cauda do teto de segurança).
	const int count = std::clamp(ws.products, 0, kDemoMaxProducts);
	Value out;
	out["count"] = Value(count);
	out["names"] = array_of(ws.product_names);
	Value prices = empty_array();
	for (int index = 0; index < count; ++index) {
		prices.append(Value(ws.base_prices[index]));
	}
	out["base_prices"] = prices;
	out["sectors"] = array_of(ws.product_sectors);
	Value durable = empty_array();
	for (const char flag : ws.product_durable) {
		durable.append(Value(flag != 0));
	}
	out["durable"] = durable;
	return out;
}

void products_from(WorldState& ws, const Value& block) {
	const int count = read_int(block, "count", -1);
	if (count >= 0) {
		ws.products = std::min(count, kDemoMaxProducts);
	}
	if (const Value* names = read_array(block, "names")) {
		ws.product_names = read_string_vector(*names);
	}
	if (const Value* prices = read_array(block, "base_prices")) {
		const int available = std::min(prices->size(),
			std::clamp(ws.products, 0, kDemoMaxProducts));
		for (int index = 0; index < available; ++index) {
			ws.base_prices[index] = read_scalar_double((*prices)[index],
				ws.base_prices[index]);
		}
	}
	if (const Value* sectors = read_array(block, "sectors")) {
		ws.product_sectors = read_string_vector(*sectors);
		// O vetor paralelo aos produtos não passa do teto do modelo.
		if (static_cast<int>(ws.product_sectors.size()) > kDemoMaxProducts) {
			ws.product_sectors.resize(static_cast<std::size_t>(kDemoMaxProducts));
		}
	}
	if (const Value* durable = read_array(block, "durable")) {
		const int available = std::min(durable->size(), kDemoMaxProducts);
		ws.product_durable.assign(static_cast<std::size_t>(available), 0);
		for (int index = 0; index < available; ++index) {
			ws.product_durable[static_cast<std::size_t>(index)] =
				read_scalar_bool((*durable)[index], false) ? 1 : 0;
		}
	}
	// Índice derivado: mesmo critério de demo_set_goods (nome vazio não entra,
	// só os `products` ativos).
	ws.product_lookup.clear();
	const int active = std::clamp(ws.products, 0,
		static_cast<int>(ws.product_names.size()));
	for (int index = 0; index < active; ++index) {
		const std::string& name = ws.product_names[static_cast<std::size_t>(index)];
		if (!name.empty()) {
			ws.product_lookup[name] = index;
		}
	}
}

// ---------------------------------------------------------------------------
// 2. Receitas (recipes.json -> demo_set_recipes)
// ---------------------------------------------------------------------------

Value recipes_to(const WorldState& ws) {
	Value out = empty_array();
	for (const DemoRecipe& recipe : ws.recipes) {
		Value entry;
		entry["id"] = Value(recipe.id);
		entry["output"] = Value(recipe.output);
		entry["yield"] = Value(recipe.yield);
		const int input_count = std::clamp(recipe.input_count, 0,
			kDemoMaxRecipeInputs);
		entry["input_count"] = Value(input_count);
		Value inputs = empty_array();
		for (int slot = 0; slot < input_count; ++slot) {
			Value item;
			item["product"] = Value(recipe.inputs[slot]);
			item["qty"] = Value(recipe.input_qty[slot]);
			inputs.append(item);
		}
		entry["inputs"] = inputs;
		const int byproduct_count = std::clamp(recipe.byproduct_count, 0,
			kDemoMaxRecipeInputs);
		entry["byproduct_count"] = Value(byproduct_count);
		Value byproducts = empty_array();
		for (int slot = 0; slot < byproduct_count; ++slot) {
			Value item;
			item["product"] = Value(recipe.byproducts[slot]);
			item["qty"] = Value(recipe.byproduct_qty[slot]);
			byproducts.append(item);
		}
		entry["byproducts"] = byproducts;
		entry["capital_per_capacity"] = Value(recipe.capital_per_capacity);
		entry["jobs_per_ton"] = Value(recipe.jobs_per_ton);
		entry["wage"] = Value(recipe.wage);
		entry["opex_per_ton"] = Value(recipe.opex_per_ton);
		entry["education_mix"] = array_of(recipe.education_mix,
			kDemoEducationLevels);
		out.append(entry);
	}
	return out;
}

void recipes_from(WorldState& ws, const Value& array) {
	ws.recipes.clear();
	ws.recipe_lookup.clear();
	for (int index = 0; index < array.size(); ++index) {
		const Value& entry = array[index];
		if (entry.get_type() != Value::DICTIONARY) {
			continue;
		}
		// Defaults do struct preservados no que o JSON não trouxer.
		DemoRecipe recipe;
		recipe.id = read_string(entry, "id");
		recipe.output = read_int(entry, "output", recipe.output);
		recipe.yield = read_double(entry, "yield", recipe.yield);
		recipe.capital_per_capacity = read_double(entry, "capital_per_capacity",
			recipe.capital_per_capacity);
		recipe.jobs_per_ton = read_double(entry, "jobs_per_ton",
			recipe.jobs_per_ton);
		recipe.wage = read_double(entry, "wage", recipe.wage);
		recipe.opex_per_ton = read_double(entry, "opex_per_ton",
			recipe.opex_per_ton);
		if (const Value* mix = read_array(entry, "education_mix")) {
			for (int level = 0; level < kDemoEducationLevels; ++level) {
				recipe.education_mix[level] = read_scalar_double((*mix)[level],
					recipe.education_mix[level]);
			}
		}
		// `input_count` manda, limitado ao que o array realmente traz (o dump
		// grava os dois coerentes; um arquivo editado à mão não estoura o
		// array fixo do struct).
		const int declared_inputs = read_int(entry, "input_count", -1);
		if (const Value* inputs = read_array(entry, "inputs")) {
			const int available = inputs->size();
			int input_count = declared_inputs >= 0
				? std::min(declared_inputs, available) : available;
			input_count = std::clamp(input_count, 0, kDemoMaxRecipeInputs);
			recipe.input_count = input_count;
			for (int slot = 0; slot < input_count; ++slot) {
				const Value& item = (*inputs)[slot];
				recipe.inputs[slot] = read_int(item, "product", -1);
				recipe.input_qty[slot] = read_double(item, "qty", 0.0);
			}
		}
		const int declared_byproducts = read_int(entry, "byproduct_count", -1);
		if (const Value* byproducts = read_array(entry, "byproducts")) {
			const int available = byproducts->size();
			int byproduct_count = declared_byproducts >= 0
				? std::min(declared_byproducts, available) : available;
			byproduct_count = std::clamp(byproduct_count, 0,
				kDemoMaxRecipeInputs);
			recipe.byproduct_count = byproduct_count;
			for (int slot = 0; slot < byproduct_count; ++slot) {
				const Value& item = (*byproducts)[slot];
				recipe.byproducts[slot] = read_int(item, "product", -1);
				recipe.byproduct_qty[slot] = read_double(item, "qty", 0.0);
			}
		}
		ws.recipes.push_back(std::move(recipe));
		// Índice derivado: mesmo esquema de demo_set_recipes (id -> posição).
		const DemoRecipe& stored = ws.recipes.back();
		ws.recipe_lookup[stored.id] = static_cast<int>(ws.recipes.size()) - 1;
	}
}

// ---------------------------------------------------------------------------
// 3. Necessidades (needs.json -> demo_set_needs)
// ---------------------------------------------------------------------------

Value needs_to(const WorldState& ws) {
	Value out;
	out["loaded"] = Value(ws.needs_loaded);
	out["food_level"] = Value(ws.needs_food_level);
	out["durable_decay"] = Value(ws.durable_decay);
	Value levels = empty_array();
	for (const DemoNeedsLevel& level : ws.needs_levels) {
		Value entry;
		entry["id"] = Value(level.id);
		entry["priority"] = Value(level.priority);
		entry["weight"] = Value(level.weight);
		Value categories = empty_array();
		for (const DemoNeedsCategory& category : level.categories) {
			Value item;
			item["id"] = Value(category.id);
			item["per_capita_day"] = Value(category.per_capita_day);
			item["substitutes"] = array_of(category.substitutes);
			item["equivalence"] = array_of(category.equivalence);
			categories.append(item);
		}
		entry["categories"] = categories;
		levels.append(entry);
	}
	out["levels"] = levels;
	out["food_products"] = array_of(ws.food_products);
	out["food_factors"] = array_of(ws.food_factors);
	out["government_products"] = array_of(ws.government_products);
	out["government_per_capita_day"] = array_of(ws.government_per_capita_day);
	return out;
}

void needs_from(WorldState& ws, const Value& block) {
	ws.needs_loaded = read_bool(block, "loaded", ws.needs_loaded);
	ws.needs_food_level = read_int(block, "food_level", ws.needs_food_level);
	ws.durable_decay = read_double(block, "durable_decay", ws.durable_decay);
	if (const Value* array = read_array(block, "food_products")) {
		ws.food_products = read_int_vector(*array);
	}
	if (const Value* array = read_array(block, "food_factors")) {
		ws.food_factors = read_double_vector(*array);
	}
	if (const Value* array = read_array(block, "government_products")) {
		ws.government_products = read_int_vector(*array);
	}
	if (const Value* array = read_array(block, "government_per_capita_day")) {
		ws.government_per_capita_day = read_double_vector(*array);
	}
	if (const Value* levels = read_array(block, "levels")) {
		std::vector<DemoNeedsLevel> parsed;
		const int available = std::min(levels->size(), kDemoMaxNeedsLevels);
		parsed.reserve(static_cast<std::size_t>(available));
		for (int index = 0; index < available; ++index) {
			const Value& entry = (*levels)[index];
			if (entry.get_type() != Value::DICTIONARY) {
				continue;
			}
			DemoNeedsLevel level;
			level.id = read_string(entry, "id");
			level.priority = read_int(entry, "priority", level.priority);
			level.weight = read_double(entry, "weight", level.weight);
			if (const Value* categories = read_array(entry, "categories")) {
				for (int slot = 0; slot < categories->size(); ++slot) {
					const Value& source = (*categories)[slot];
					if (source.get_type() != Value::DICTIONARY) {
						continue;
					}
					DemoNeedsCategory category;
					category.id = read_string(source, "id");
					category.per_capita_day = read_double(source,
						"per_capita_day", category.per_capita_day);
					if (const Value* substitutes = read_array(source,
							"substitutes")) {
						category.substitutes = read_int_vector(*substitutes);
					}
					if (const Value* equivalence = read_array(source,
							"equivalence")) {
						category.equivalence = read_double_vector(*equivalence);
					}
					level.categories.push_back(std::move(category));
				}
			}
			parsed.push_back(std::move(level));
		}
		ws.needs_levels = std::move(parsed);
	}
}

// ---------------------------------------------------------------------------
// 4. Serviços (services.json -> demo_set_services)
// ---------------------------------------------------------------------------

Value services_to(const WorldState& ws) {
	Value out;
	out["loaded"] = Value(ws.services_loaded);
	out["government_share"] = Value(ws.service_government_share);
	Value sectors = empty_array();
	for (const DemoServiceSector& sector : ws.service_sectors) {
		Value entry;
		entry["id"] = Value(sector.id);
		entry["name_pt"] = Value(sector.name_pt);
		entry["name_en"] = Value(sector.name_en);
		entry["name_es"] = Value(sector.name_es);
		entry["icon"] = Value(sector.icon);
		entry["price"] = Value(sector.price);
		entry["pop_share"] = array_of(sector.pop_share, kDemoMaxNeedsLevels);
		entry["business_share"] = Value(sector.business_share);
		entry["freight_share"] = Value(sector.freight_share);
		entry["government_share"] = Value(sector.government_share);
		Value inputs = empty_array();
		for (const DemoServiceSector::Input& input : sector.inputs) {
			Value item;
			item["product"] = Value(input.product);
			item["qty"] = Value(input.qty);
			inputs.append(item);
		}
		entry["inputs"] = inputs;
		// `service_inputs` são ÍNDICES de setor (ordem do vetor); como o array
		// sai na mesma ordem, os índices continuam válidos na volta.
		entry["service_inputs"] = array_of(sector.service_inputs);
		entry["service_input_qty"] = array_of(sector.service_input_qty);
		entry["education_mix"] = array_of(sector.education_mix,
			kDemoEducationLevels);
		entry["jobs_per_unit"] = Value(sector.jobs_per_unit);
		entry["capital_per_unit"] = Value(sector.capital_per_unit);
		entry["min_capacity"] = Value(sector.min_capacity);
		entry["exportable"] = Value(sector.exportable);
		entry["export_share"] = Value(sector.export_share);
		entry["health"] = Value(sector.health);
		entry["education"] = Value(sector.education);
		entry["source"] = Value(sector.source);
		sectors.append(entry);
	}
	out["sectors"] = sectors;
	return out;
}

void services_from(WorldState& ws, const Value& block) {
	ws.services_loaded = read_bool(block, "loaded", ws.services_loaded);
	ws.service_government_share = read_double(block, "government_share",
		ws.service_government_share);
	if (const Value* sectors = read_array(block, "sectors")) {
		std::vector<DemoServiceSector> parsed;
		for (int index = 0; index < sectors->size(); ++index) {
			const Value& entry = (*sectors)[index];
			if (entry.get_type() != Value::DICTIONARY) {
				continue;
			}
			DemoServiceSector sector;
			sector.id = read_string(entry, "id");
			sector.name_pt = read_string(entry, "name_pt");
			sector.name_en = read_string(entry, "name_en");
			sector.name_es = read_string(entry, "name_es");
			sector.icon = read_string(entry, "icon");
			sector.price = read_double(entry, "price", sector.price);
			if (const Value* shares = read_array(entry, "pop_share")) {
				for (int level = 0; level < kDemoMaxNeedsLevels; ++level) {
					sector.pop_share[level] = read_scalar_double(
						(*shares)[level], sector.pop_share[level]);
				}
			}
			sector.business_share = read_double(entry, "business_share",
				sector.business_share);
			sector.freight_share = read_double(entry, "freight_share",
				sector.freight_share);
			sector.government_share = read_double(entry, "government_share",
				sector.government_share);
			if (const Value* inputs = read_array(entry, "inputs")) {
				for (int slot = 0; slot < inputs->size(); ++slot) {
					const Value& item = (*inputs)[slot];
					if (item.get_type() != Value::DICTIONARY) {
						continue;
					}
					DemoServiceSector::Input input;
					input.product = read_int(item, "product", input.product);
					input.qty = read_double(item, "qty", input.qty);
					sector.inputs.push_back(input);
				}
			}
			if (const Value* service_inputs = read_array(entry,
					"service_inputs")) {
				sector.service_inputs = read_int_vector(*service_inputs);
			}
			if (const Value* service_qty = read_array(entry,
					"service_input_qty")) {
				sector.service_input_qty = read_double_vector(*service_qty);
			}
			if (const Value* mix = read_array(entry, "education_mix")) {
				for (int level = 0; level < kDemoEducationLevels; ++level) {
					sector.education_mix[level] = read_scalar_double(
						(*mix)[level], sector.education_mix[level]);
				}
			}
			sector.jobs_per_unit = read_double(entry, "jobs_per_unit",
				sector.jobs_per_unit);
			sector.capital_per_unit = read_double(entry, "capital_per_unit",
				sector.capital_per_unit);
			sector.min_capacity = read_double(entry, "min_capacity",
				sector.min_capacity);
			sector.exportable = read_bool(entry, "exportable", sector.exportable);
			sector.export_share = read_double(entry, "export_share",
				sector.export_share);
			sector.health = read_bool(entry, "health", sector.health);
			sector.education = read_bool(entry, "education", sector.education);
			sector.source = read_string(entry, "source", sector.source);
			parsed.push_back(std::move(sector));
		}
		ws.service_sectors = std::move(parsed);
	}
}

// ---------------------------------------------------------------------------
// 5. Emprego por setor (sector_employment.json -> demo_set_sector_employment)
//    + parâmetros agri/industry do WorldState
// ---------------------------------------------------------------------------

Value employment_to(const WorldState& ws) {
	Value out;
	out["loaded"] = Value(ws.sector_loaded);
	Value anchors = empty_array();
	for (const DemoSectorAnchor& anchor : ws.sector_anchors) {
		Value entry;
		entry["income"] = Value(anchor.income);
		entry["agriculture"] = Value(anchor.agriculture);
		entry["industry"] = Value(anchor.industry);
		entry["informal"] = Value(anchor.informal);
		anchors.append(entry);
	}
	out["anchors"] = anchors;
	out["agri_labor_per_ton"] = Value(ws.agri_labor_per_ton);
	out["agri_labor_exponent"] = Value(ws.agri_labor_exponent);
	out["agri_production_band_min"] = Value(ws.agri_production_band_min);
	out["agri_production_band_max"] = Value(ws.agri_production_band_max);
	out["agri_own_consumption_share"] = Value(ws.agri_own_consumption_share);
	out["industry_labor_per_va"] = Value(ws.industry_labor_per_va);
	out["industry_labor_exponent"] = Value(ws.industry_labor_exponent);
	return out;
}

void employment_from(WorldState& ws, const Value& block) {
	ws.sector_loaded = read_bool(block, "loaded", ws.sector_loaded);
	ws.agri_labor_per_ton = read_double(block, "agri_labor_per_ton",
		ws.agri_labor_per_ton);
	ws.agri_labor_exponent = read_double(block, "agri_labor_exponent",
		ws.agri_labor_exponent);
	ws.agri_production_band_min = read_double(block, "agri_production_band_min",
		ws.agri_production_band_min);
	ws.agri_production_band_max = read_double(block, "agri_production_band_max",
		ws.agri_production_band_max);
	ws.agri_own_consumption_share = read_double(block,
		"agri_own_consumption_share", ws.agri_own_consumption_share);
	ws.industry_labor_per_va = read_double(block, "industry_labor_per_va",
		ws.industry_labor_per_va);
	ws.industry_labor_exponent = read_double(block, "industry_labor_exponent",
		ws.industry_labor_exponent);
	if (const Value* anchors = read_array(block, "anchors")) {
		std::vector<DemoSectorAnchor> parsed;
		parsed.reserve(static_cast<std::size_t>(anchors->size()));
		for (int index = 0; index < anchors->size(); ++index) {
			const Value& entry = (*anchors)[index];
			if (entry.get_type() != Value::DICTIONARY) {
				continue;
			}
			DemoSectorAnchor anchor;
			anchor.income = read_double(entry, "income", anchor.income);
			anchor.agriculture = read_double(entry, "agriculture",
				anchor.agriculture);
			anchor.industry = read_double(entry, "industry", anchor.industry);
			anchor.informal = read_double(entry, "informal", anchor.informal);
			parsed.push_back(anchor);
		}
		ws.sector_anchors = std::move(parsed);
	}
}

// ---------------------------------------------------------------------------
// 6. Parâmetros de balanceamento do WorldState (demo_set_balance)
// ---------------------------------------------------------------------------

Value balance_to(const WorldState& ws) {
	Value out;
	out["natural_unemployment"] = Value(ws.natural_unemployment);
	out["service_wage_adjust"] = Value(ws.service_wage_adjust);
	out["service_wage_max"] = Value(ws.service_wage_max);
	out["wage_rigidity"] = Value(ws.wage_rigidity);
	out["income_basket_margin"] = Value(ws.income_basket_margin);
	out["inventory_cover_days"] = Value(ws.inventory_cover_days);
	out["inventory_recovery"] = Value(ws.inventory_recovery);
	out["trade_neighbors"] = Value(ws.trade_neighbors);
	out["trade_external"] = Value(ws.trade_external);
	return out;
}

void balance_from(WorldState& ws, const Value& block) {
	ws.natural_unemployment = read_double(block, "natural_unemployment",
		ws.natural_unemployment);
	ws.service_wage_adjust = read_double(block, "service_wage_adjust",
		ws.service_wage_adjust);
	ws.service_wage_max = read_double(block, "service_wage_max",
		ws.service_wage_max);
	ws.wage_rigidity = read_double(block, "wage_rigidity", ws.wage_rigidity);
	ws.income_basket_margin = read_double(block, "income_basket_margin",
		ws.income_basket_margin);
	ws.inventory_cover_days = read_double(block, "inventory_cover_days",
		ws.inventory_cover_days);
	ws.inventory_recovery = read_double(block, "inventory_recovery",
		ws.inventory_recovery);
	ws.trade_neighbors = read_int(block, "trade_neighbors", ws.trade_neighbors);
	ws.trade_external = read_int(block, "trade_external", ws.trade_external);
}

} // namespace

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

Value definitions_to(const WorldState& ws) {
	Value out;
	// A ordem das chaves é parte do contrato (dump estável byte a byte).
	out["products"] = products_to(ws);
	out["recipes"] = recipes_to(ws);
	out["needs"] = needs_to(ws);
	out["services"] = services_to(ws);
	out["employment"] = employment_to(ws);
	out["balance"] = balance_to(ws);
	return out;
}

void definitions_from(WorldState& ws, const Value& data) {
	// Raiz que não é objeto: nada a restaurar (nem throw, nem campo zerado).
	if (data.get_type() != Value::DICTIONARY) {
		return;
	}
	if (const Value* block = read_object(data, "products")) {
		products_from(ws, *block);
	}
	if (const Value* array = read_array(data, "recipes")) {
		recipes_from(ws, *array);
	}
	if (const Value* block = read_object(data, "needs")) {
		needs_from(ws, *block);
	}
	if (const Value* block = read_object(data, "services")) {
		services_from(ws, *block);
	}
	if (const Value* block = read_object(data, "employment")) {
		employment_from(ws, *block);
	}
	if (const Value* block = read_object(data, "balance")) {
		balance_from(ws, *block);
	}
}

} // namespace save
} // namespace ogs
