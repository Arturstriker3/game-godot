// Implementacao da fixture "mundo de bolso" (ver pocket_world.h).
#include "pocket_world.h"

#include "sim/bank.h"
#include "sim/efficiency.h"
#include "sim/save_definitions.h"
#include "sim/save_state.h"
#include "sim/save_value.h"

#include <stdexcept>

namespace ogs::test {
namespace {

const save::Value kEmpty;

const save::Value& field(const save::Value& value, const char* key) {
	return value.get(key, kEmpty);
}

std::string text(const save::Value& value, const char* key,
		const std::string& fallback = std::string()) {
	const save::Value& item = field(value, key);
	return item.get_type() == save::Value::NIL
		? fallback : std::string(static_cast<std::string>(item));
}

double number(const save::Value& value, const char* key, double fallback = 0.0) {
	const save::Value& item = field(value, key);
	return item.get_type() == save::Value::NIL ? fallback : double(item);
}

bool flag(const save::Value& value, const char* key, bool fallback = false) {
	const save::Value& item = field(value, key);
	return item.get_type() == save::Value::NIL ? fallback : bool(item);
}

} // namespace

std::string fixture_path() {
	return std::string(OPENGS_SOURCE_DIR) + "/tests/fixtures/pocket_world.json";
}

int PocketWorld::product(const std::string& id) const {
	const auto found = ws.product_lookup.find(id);
	return found == ws.product_lookup.end() ? -1 : found->second;
}

int PocketWorld::recipe(const std::string& id) const {
	const auto found = ws.recipe_lookup.find(id);
	return found == ws.recipe_lookup.end() ? -1 : found->second;
}

const City& PocketWorld::city(const std::string& id) const {
	const auto found = ws.city_lookup.find(id);
	if (found == ws.city_lookup.end()) {
		throw std::runtime_error("pocket_world: cidade ausente: " + id);
	}
	return ws.cities[static_cast<std::size_t>(found->second)];
}

Building* PocketWorld::building(const std::string& id) {
	for (Building& item : ws.buildings) {
		if (item.id == id) {
			return &item;
		}
	}
	return nullptr;
}

const Building* PocketWorld::building(const std::string& id) const {
	for (const Building& item : ws.buildings) {
		if (item.id == id) {
			return &item;
		}
	}
	return nullptr;
}

PocketWorld::PocketWorld() {
	save::Value root;
	std::string error;
	if (!save::parse_json_file(fixture_path(), root, error)) {
		throw std::runtime_error("pocket_world: " + error + " ("
			+ fixture_path() + ")");
	}

	// Mundo ATIVO e em modo mundo. Nada aqui e "demo": o fixture e o mundo.
	ws.active = true;
	ws.world_mode = true;
	ws.day = 0;
	ws.bank_credit_enabled = true;

	// 1. Produtos pelo parser do snapshot real (goods.json -> definitions).
	save::Value products_block = save::Value::make_object();
	products_block["products"] = field(root, "products");
	save::definitions_from(ws, products_block);

	// 2. Receitas: o fixture declara os bens por NOME (legivel); o parser do
	// save usa INDICE, como o snapshot real. Converte antes de entregar.
	save::Value recipes = field(root, "recipes");
	for (int index = 0; index < recipes.size(); ++index) {
		save::Value& entry = recipes[index];
		if (entry.get_type() != save::Value::DICTIONARY) {
			continue;
		}
		const std::string id = text(entry, "id");
		if (entry.has("co_loss")) {
			co_loss[id] = number(entry, "co_loss");
		}
		// O parser do save usa INDICE em `output`/`inputs`/`byproducts`; o
		// fixture legivel declara NOMES. Converte os tres.
		if (entry.has("output")) {
			const int output_index = product(text(entry, "output"));
			if (output_index < 0) {
				throw std::runtime_error("pocket_world: bem de saida '"
					+ id + "' ausente no catalogo do fixture");
			}
			entry["output"] = save::Value(output_index);
		}
		for (const char* list : { "inputs", "byproducts" }) {
			save::Value& items = entry[list];
			for (int slot = 0; slot < items.size(); ++slot) {
				save::Value& item = items[slot];
				if (item.get_type() != save::Value::DICTIONARY) {
					continue;
				}
				const int product_index = product(text(item, "product"));
				if (product_index < 0) {
					throw std::runtime_error("pocket_world: bem da receita '"
						+ id + "' ausente no catalogo do fixture");
				}
				item["product"] = save::Value(product_index);
			}
		}
	}
	save::Value recipes_block = save::Value::make_object();
	recipes_block["recipes"] = recipes;
	save::definitions_from(ws, recipes_block);

	// 3. Indices da D-125 que a fachada Godot resolve em `set_goods` e o
	// parser de definitions nao conhece.
	ws.efficiency.refinery_gasoline = product("gasoline");
	ws.efficiency.refinery_diesel = product("diesel");
	ws.efficiency.refinery_jet = product("jet_fuel");
	ws.efficiency.equipment_fuel_product = product("diesel");

	// 4. Cidades (1 nacao, 2 cidades). Populacao COM populacao de simulacao:
	// a economia usa `simulation_population`; a exibida fica igual no fixture.
	const save::Value cities = field(root, "cities");
	for (int index = 0; index < cities.size(); ++index) {
		const save::Value& row = cities[index];
		City city;
		city.id = text(row, "id");
		city.name = text(row, "name");
		city.nation = text(row, "nation");
		city.home_nation = city.nation;
		city.landmass = text(row, "landmass");
		city.population = number(row, "population");
		city.simulation_population = number(row, "simulation_population",
			city.population);
		// Perfil legado (demography_ready = false): so a populacao e semeada.
		// Os derivados (forca de trabalho, ocupados, idade ativa) NAO sao
		// preenchidos a mao: o load os reconstroi e um valor injetado aqui
		// tornaria o roundtrip do save falso.
		Pop& pop = city.pop;
		pop.population = city.simulation_population;
		// Sem dado etario, o load canoniza `dependents = population - LABOR`
		// (forca de trabalho 0). Reproduzir a MESMA canonizacao no fixture
		// mantem o roundtrip do save exato.
		pop.dependents = pop.population;
		// Mercado com preco-base (estoque 0): e o que o tick espera encontrar.
		for (int product_index = 0; product_index < ws.products; ++product_index) {
			city.markets[product_index].price =
				ws.base_prices[product_index];
		}
		ws.cities.push_back(std::move(city));
	}

	// 5. Predios: receita/bem por nome -> indice; nascem MADUROS (o mundo
	// inicial da D-125 nasce maduro) e sem equipamento (sem diesel no fixture).
	const save::Value buildings = field(root, "buildings");
	for (int index = 0; index < buildings.size(); ++index) {
		const save::Value& row = buildings[index];
		Building building;
		building.id = text(row, "id");
		building.city = text(row, "city");
		building.owner = text(row, "owner", "private");
		building.output_product = product(text(row, "output"));
		const std::string recipe_id = text(row, "recipe");
		building.recipe = recipe_id.empty() ? -1 : recipe(recipe_id);
		building.input_product = -1;
		building.capacity = number(row, "capacity");
		if (building.recipe >= 0) {
			const Recipe& recipe = ws.recipes[
				static_cast<std::size_t>(building.recipe)];
			building.capital_per_capacity = recipe.capital_per_capacity;
			building.jobs = building.capacity * recipe.jobs_per_ton;
			building.wage = recipe.wage;
			building.opex = building.capacity * recipe.opex_per_ton;
			building.output_scale = 1.0;
		}
		eff::mature(building, false);
		ws.buildings.push_back(std::move(building));
	}

	// 6. Grupos (donos das plantas): caixa inicial conserva dinheiro.
	const save::Value groups = field(root, "groups");
	for (int index = 0; index < groups.size(); ++index) {
		const save::Value& row = groups[index];
		Group group;
		group.id = text(row, "id");
		group.name = text(row, "name", group.id);
		group.origin_nation = text(row, "origin_nation");
		group.sector = text(row, "sector");
		group.cash = number(row, "cash");
		group.state_owned = flag(row, "state_owned");
		ws.groups.push_back(std::move(group));
	}

	// 7. Banco publico da nacao (o credor do grupo estatal).
	Bank& bank = bank::ensure(ws, "AAA", bank::kRolePublic);
	bank.name = "AAA Public Bank";
	bank.name_status = "present";
	bank.share = 1.0;

	// 8. Indices derivados (cidade, tabela de nacoes, predios por cidade).
	core.rebuild_indexes();

	// 9. Frota derivada (F4): o load de um mundo SEM bloco `fleets` chama
	// `init_fleets`. Semeia aqui tambem para o fixture ser ponto fixo do save.
	core.init_fleets();

	// 10. Strata semeada: o save/load de uma cidade SEM o bloco `strata` chama
	// `seed_strata` ao carregar (save antigo/cenario sintetico). Semeia aqui
	// tambem para o fixture ser um ponto fixo do roundtrip do save.
	for (City& city : ws.cities) {
		if (!city.pop.strata_ready) {
			core.seed_strata(city, nullptr);
		}
	}
}

std::uint64_t digest(const WorldState& ws, const SimCore& core) {
	const std::string text = save::dump_json(save::to_value(ws, core));
	std::uint64_t hash = 1469598103934665603ULL;  // FNV offset basis
	for (const unsigned char byte : text) {
		hash ^= static_cast<std::uint64_t>(byte);
		hash *= 1099511628211ULL;                 // FNV prime
	}
	return hash;
}

} // namespace ogs::test
