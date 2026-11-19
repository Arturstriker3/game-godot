// G2 — armazenagem agregada, reserva nacional e pulmão região→cidade (D-100).
// Ver docs/economy/storage_g2.md.
//
// Ordem dentro de `phase_storage` (roda logo depois de `phase_agriculture`):
//   1. a colheita do dia que tem silo regional é DESVIADA do mercado da cidade
//      para o estoque privado do silo (`inflow_stock`, o pulmão);
//   2. perdas de perecibilidade (diária) e de armazenagem (mensal, infra B5);
//   3. manutenção diária da estrutura, paga pelo dono ao setor de serviços;
//   4. liberação gradual do pulmão ao mercado da cidade-polo, pagando o custo
//      de acesso da estrada (B5) ao transporte — dinheiro conservado.
//
// O que NÃO acontece aqui: nenhuma decisão de compra/liberação do governo ou
// do privado (isso é a IA, que emite comandos) e nenhuma formação de preço (o
// preço segue D-091/D-092; a armazenagem muda o VOLUME ofertado e a
// sazonalidade aparece por aí).
#include "storage.h"

#include "agriculture.h"
#include "profiling.h"
#include "sim_constants.h"
#include "sim_core.h"
#include "world_state.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace ogs {
namespace {

struct TypeName {
	int type;
	const char* name;
	const char* alias;
};

constexpr TypeName kTypeNames[] = {
	{STORAGE_SILO, "silo", "silo_graneleiro"},
	{STORAGE_COLD, "cold", "camara_fria"},
	{STORAGE_FUEL, "fuel_tank", "tanque_combustiveis"},
	{STORAGE_GAS, "gas", "gas"},
	{STORAGE_BULK, "bulk_yard", "patio_granéis"},
	{STORAGE_GENERAL, "general", "armazem_geral"},
	{STORAGE_VAULT, "vault", "cofre"},
	{STORAGE_MAGAZINE, "magazine", "paiol"},
};

constexpr const char* kSiloProducts[] = {
	"wheat", "rice", "maize", "soy", "feed", "bran", "flour",
};
constexpr const char* kColdProducts[] = {"meat", "potato", "bread"};
constexpr const char* kFuelProducts[] = {
	"oil", "fuel", "ethanol", "chemicals", "alcohol",
};
constexpr const char* kGasProducts[] = {"gas"};
constexpr const char* kBulkProducts[] = {
	"iron", "bauxite", "lead", "phosphate", "tungsten", "sulfur", "coal",
	"copper", "wood", "cotton", "charcoal", "refined_sulfur", "rubber",
};
constexpr const char* kVaultProducts[] = {
	"gold", "diamond", "uranium", "rare_earths", "gold_bar",
	"processed_rare_earths", "jewelry",
};
constexpr const char* kMagazineProducts[] = {
	"ammunition", "artillery_ammo", "explosives", "gunpowder",
	"combat_ration", "small_arms", "artillery", "casing", "projectile",
	"military_drone", "infantry_gear",
};

template <std::size_t N>
bool name_in(const std::string& name, const char* const (&list)[N]) {
	for (const char* item : list) {
		if (name == item) {
			return true;
		}
	}
	return false;
}

int default_type_for_product(const std::string& name) {
	if (name_in(name, kSiloProducts)) {
		return STORAGE_SILO;
	}
	if (name_in(name, kColdProducts)) {
		return STORAGE_COLD;
	}
	if (name_in(name, kFuelProducts)) {
		return STORAGE_FUEL;
	}
	if (name_in(name, kGasProducts)) {
		return STORAGE_GAS;
	}
	if (name_in(name, kBulkProducts)) {
		return STORAGE_BULK;
	}
	if (name_in(name, kVaultProducts)) {
		return STORAGE_VAULT;
	}
	if (name_in(name, kMagazineProducts)) {
		return STORAGE_MAGAZINE;
	}
	return STORAGE_GENERAL;
}

// Perda mensal de armazenagem agravada pela qualidade do armazém (mesma regra
// de agriculture.cpp; mantida local para o módulo não depender de símbolo
// interno do outro .cpp).
double aggravated_storage_loss(double fraction, double quality) {
	const double factor = 1.0 + kAgriStorageLossAggravation
		* (1.0 - std::clamp(quality, 0.0, 1.0));
	return std::clamp(fraction * factor, 0.0, kAgriLossCap);
}

// Remove `loss` do produto preservando a fração de pulmão (`inflow_stock`).
void remove_stock(StorageSite& site, int product, double loss) {
	if (product < 0) {
		return;
	}
	const std::size_t p = static_cast<std::size_t>(product);
	if (p >= site.stock.size() || loss <= 0.0) {
		return;
	}
	const double before = std::max(0.0, site.stock[p]);
	const double after = std::max(0.0, before - loss);
	const double ratio = before > 1e-12 ? after / before : 0.0;
	site.stock[p] = after;
	if (p < site.inflow_stock.size()) {
		site.inflow_stock[p] = std::max(0.0, site.inflow_stock[p] * ratio);
	}
}

} // namespace

// ---------------------------------------------------------------------------
// Nomes e propriedades dos tipos
// ---------------------------------------------------------------------------

const char* storage_type_name(int type) {
	for (const TypeName& row : kTypeNames) {
		if (row.type == type) {
			return row.name;
		}
	}
	return nullptr;
}

bool storage_type_from_name(const std::string& name, int& out) {
	for (const TypeName& row : kTypeNames) {
		if (name == row.name || name == row.alias) {
			out = row.type;
			return true;
		}
	}
	if (name == "cold_storage") {
		out = STORAGE_COLD;
		return true;
	}
	if (name == "general_warehouse") {
		out = STORAGE_GENERAL;
		return true;
	}
	return false;
}

const char* storage_scope_name(int scope) {
	return scope == STORAGE_PRIVATE ? "private" : "government";
}

bool storage_scope_from_name(const std::string& name, int& out) {
	if (name == "private" || name == "privado" || name == "privada") {
		out = STORAGE_PRIVATE;
		return true;
	}
	if (name == "government" || name == "state" || name == "governo"
			|| name == "public" || name == "publico") {
		out = STORAGE_GOVERNMENT;
		return true;
	}
	return false;
}

bool storage_type_is_region(int type) {
	return type == STORAGE_SILO || type == STORAGE_FUEL
		|| type == STORAGE_GAS || type == STORAGE_BULK;
}

bool storage_type_is_structure_only(int type) {
	return type == STORAGE_VAULT || type == STORAGE_MAGAZINE;
}

bool storage_type_accepts(int type, int product_type) {
	return type >= 0 && type < kStorageTypeCount
		&& !storage_type_is_structure_only(type)
		&& type == product_type;
}

// ---------------------------------------------------------------------------
// Índice e site
// ---------------------------------------------------------------------------

void rebuild_storage_indexes(WorldState& ws) {
	const std::size_t products =
		static_cast<std::size_t>(std::max(0, ws.products));
	if (ws.storage_product_type.size()
			!= products || ws.storage_products != ws.products) {
		ws.storage_product_type.assign(products, -1);
		for (std::size_t index = 0; index < products; ++index) {
			if (index < ws.product_names.size()) {
				ws.storage_product_type[index] =
					default_type_for_product(ws.product_names[index]);
			}
		}
		ws.storage_products = ws.products;
	}
	for (StorageSite& site : ws.storage_sites) {
		site.city_index = -1;
		const auto found = ws.city_lookup.find(site.city);
		if (found != ws.city_lookup.end()) {
			site.city_index = found->second;
		}
		if (site.stock.size() != products) {
			site.stock.resize(products, 0.0);
			site.inflow_stock.resize(products, 0.0);
		}
	}
	ws.storage_index_ready = true;
}

void set_storage_product_type(WorldState& ws, int product, int type) {
	if (product < 0 || product >= ws.products || type < 0
			|| type >= kStorageTypeCount) {
		return;
	}
	if (static_cast<std::size_t>(product) >= ws.storage_product_type.size()) {
		rebuild_storage_indexes(ws);
	}
	if (static_cast<std::size_t>(product) < ws.storage_product_type.size()) {
		ws.storage_product_type[static_cast<std::size_t>(product)] = type;
	}
}

int find_storage_site(const WorldState& ws, int city_index, int type,
		int scope) {
	if (city_index < 0 || type < 0 || type >= kStorageTypeCount
			|| scope < 0 || scope >= kStorageScopeCount) {
		return -1;
	}
	for (std::size_t index = 0; index < ws.storage_sites.size(); ++index) {
		const StorageSite& site = ws.storage_sites[index];
		if (site.city_index == city_index && site.type == type
				&& site.scope == scope) {
			return static_cast<int>(index);
		}
	}
	return -1;
}

int ensure_storage_site(WorldState& ws, int city_index, int type, int scope) {
	const int found = find_storage_site(ws, city_index, type, scope);
	if (found >= 0) {
		return found;
	}
	if (city_index < 0
			|| static_cast<std::size_t>(city_index) >= ws.cities.size()
			|| type < 0 || type >= kStorageTypeCount
			|| scope < 0 || scope >= kStorageScopeCount) {
		return -1;
	}
	StorageSite site;
	site.city = ws.cities[static_cast<std::size_t>(city_index)].id;
	site.city_index = city_index;
	site.type = type;
	site.scope = scope;
	site.capacity_t = 0.0;
	site.stock.assign(static_cast<std::size_t>(std::max(0, ws.products)), 0.0);
	site.inflow_stock.assign(
		static_cast<std::size_t>(std::max(0, ws.products)), 0.0);
	ws.storage_sites.push_back(std::move(site));
	return static_cast<int>(ws.storage_sites.size()) - 1;
}

void add_storage_capacity(WorldState& ws, int city_index, int type, int scope,
		double tons) {
	if (!(tons > 0.0)) {
		return;
	}
	const int index = ensure_storage_site(ws, city_index, type, scope);
	if (index < 0) {
		return;
	}
	ws.storage_sites[static_cast<std::size_t>(index)].capacity_t += tons;
}

double storage_free_capacity(const WorldState& ws, const StorageSite& site) {
	if (storage_type_is_structure_only(site.type)) {
		return 0.0;
	}
	double used = 0.0;
	for (std::size_t product = 0; product < site.stock.size(); ++product) {
		const int product_type =
			product < ws.storage_product_type.size()
			? ws.storage_product_type[product] : -1;
		if (storage_type_accepts(site.type, product_type)) {
			used += std::max(0.0, site.stock[product]);
		}
	}
	return std::max(0.0, site.capacity_t - used);
}

double storage_total_stock(const StorageSite& site) {
	double total = 0.0;
	for (const double value : site.stock) {
		total += std::max(0.0, value);
	}
	return total;
}

double storage_stock(const StorageSite& site, int product) {
	if (product < 0
			|| static_cast<std::size_t>(product) >= site.stock.size()) {
		return 0.0;
	}
	return std::max(0.0, site.stock[static_cast<std::size_t>(product)]);
}

void apply_completed_storage_project(WorldState& ws,
		const ConstructionProject& project) {
	if (project.storage_type < 0
			|| project.storage_type >= kStorageTypeCount) {
		return;
	}
	for (std::size_t index = 0; index < ws.cities.size(); ++index) {
		if (ws.cities[index].id != project.city) {
			continue;
		}
		const int scope = project.owner == "private" ? STORAGE_PRIVATE
			: STORAGE_GOVERNMENT;
		add_storage_capacity(ws, static_cast<int>(index),
			project.storage_type, scope, project.storage_capacity_t);
		return;
	}
}

// ---------------------------------------------------------------------------
// API do SimCore
// ---------------------------------------------------------------------------

void SimCore::rebuild_storage_indexes() {
	ogs::rebuild_storage_indexes(ws_);
}

void SimCore::set_storage_capacity(const std::string& city_id, int type,
		int scope, double tons) {
	City* city = find_city(city_id);
	if (city == nullptr || type < 0 || type >= kStorageTypeCount
			|| scope < 0 || scope >= kStorageScopeCount) {
		return;
	}
	const int city_index = static_cast<int>(city - ws_.cities.data());
	const int index = ensure_storage_site(ws_, city_index, type, scope);
	if (index < 0) {
		return;
	}
	// ASSIGNA (não soma): a chamada é idempotente no load do jogo, como os
	// setters de geografia. Capacidade 0 remove o armazém vazio.
	StorageSite& site = ws_.storage_sites[static_cast<std::size_t>(index)];
	// Cofre/paiol guardam a capacidade da ESTRUTURA (sem fluxo de estoque).
	site.capacity_t = std::max(0.0, tons);
}

void SimCore::set_storage_product_type(int product, int type) {
	ogs::set_storage_product_type(ws_, product, type);
}

// ---------------------------------------------------------------------------
// Fase
// ---------------------------------------------------------------------------

void SimCore::phase_storage() {
	if (ws_.storage_sites.empty()) {
		OGS_SIM_PHASE_LAP(kPhaseStorage);
		return;
	}
	if (!ws_.storage_index_ready) {
		ogs::rebuild_storage_indexes(ws_);
	}
	const GameDate today = game_date_from_day(ws_.day);
	const int month_key = today.year * 100 + today.month;
	const bool month_changed = ws_.storage_month_key != month_key
		&& ws_.day > 1;
	ws_.storage_month_key = month_key;

	for (StorageSite& site : ws_.storage_sites) {
		site.bought_today_t = 0.0;
		site.released_today_t = 0.0;
		site.received_today_t = 0.0;
		site.loss_today_t = 0.0;
		site.maintenance_paid_today = 0.0;
		site.access_paid_today = 0.0;
	}

	const std::size_t products = static_cast<std::size_t>(
		std::max(0, ws_.products));

	// 1. PULMÃO: a colheita do dia com silo regional desvia do mercado para o
	//    estoque privado do silo, até a capacidade livre. Sem silo, nada muda
	//    (a colheita continua indo direto ao mercado, como na G).
	std::vector<double> harvest(ws_.cities.size() * products, 0.0);
	for (const CropPlan& plan : ws_.crop_plans) {
		if (plan.city_index < 0 || plan.product < 0
				|| plan.product >= ws_.products
				|| plan.delivered_today_t <= 0.0) {
			continue;
		}
		harvest[static_cast<std::size_t>(plan.city_index) * products
			+ static_cast<std::size_t>(plan.product)] +=
			plan.delivered_today_t;
	}
	for (std::size_t city_index = 0; city_index < ws_.cities.size();
			++city_index) {
		City& city = ws_.cities[city_index];
		for (std::size_t product = 0; product < products; ++product) {
			const double grown = harvest[city_index * products + product];
			if (grown <= 0.0) {
				continue;
			}
			const int product_type =
				product < ws_.storage_product_type.size()
				? ws_.storage_product_type[product] : -1;
			if (product_type < 0 || !storage_type_is_region(product_type)) {
				continue;
			}
			const int site_index = find_storage_site(ws_,
				static_cast<int>(city_index), product_type, STORAGE_PRIVATE);
			if (site_index < 0) {
				continue;
			}
			StorageSite& site =
				ws_.storage_sites[static_cast<std::size_t>(site_index)];
			const double moved = std::min(grown,
				std::min(storage_free_capacity(ws_, site),
					std::max(0.0, city.markets[product].stock)));
			if (moved <= 0.0) {
				continue;
			}
			city.markets[product].stock -= moved;
			if (site.stock.size() != products) {
				site.stock.resize(products, 0.0);
				site.inflow_stock.resize(products, 0.0);
			}
			site.stock[product] += moved;
			site.inflow_stock[product] += moved;
			site.received_today_t += moved;
		}
	}

	// 2. Perdas e manutenção de cada armazém.
	for (StorageSite& site : ws_.storage_sites) {
		if (site.city_index < 0
				|| static_cast<std::size_t>(site.city_index)
					>= ws_.cities.size()) {
			continue;
		}
		City& city = ws_.cities[static_cast<std::size_t>(site.city_index)];
		const double quality = city_storage_quality(city);
		const bool cold = site.type == STORAGE_COLD;
		const bool cold_powered = city.region.infra_power > 0;
		if (site.stock.size() != products) {
			site.stock.resize(products, 0.0);
			site.inflow_stock.resize(products, 0.0);
		}
		for (std::size_t product = 0; product < products; ++product) {
			const double held = site.stock[product];
			if (held <= 0.0) {
				continue;
			}
			const int product_type =
				product < ws_.storage_product_type.size()
				? ws_.storage_product_type[product] : -1;
			if (!storage_type_accepts(site.type, product_type)) {
				// Estoque órfão (tipo do produto mudou): devolve ao mercado da
				// cidade em vez de desaparecer.
				city.markets[product].stock += held;
				site.stock[product] = 0.0;
				if (product < site.inflow_stock.size()) {
					site.inflow_stock[product] = 0.0;
				}
				continue;
			}
			double loss = 0.0;
			if (product < ws_.crop_shelf_daily.size()) {
				double daily = ws_.crop_shelf_daily[product];
				if (cold && !cold_powered) {
					daily *= kStorageColdNoPowerLossFactor;
				}
				if (daily > 0.0) {
					loss += held * std::clamp(daily, 0.0, kAgriLossCap);
				}
			}
			if (month_changed && product < ws_.crop_loss_pct.size()) {
				const double total_pct = ws_.crop_loss_pct[product];
				const double storage_pct =
					product < ws_.crop_storage_share_pct.size()
					? ws_.crop_storage_share_pct[product] : -1.0;
				if (total_pct > 0.0) {
					const double annual = (storage_pct >= 0.0 ? storage_pct
						: total_pct * (1.0 - kAgriEntryLossShare)) / 100.0;
					const double monthly = aggravated_storage_loss(
						annual / 12.0, quality);
					loss += held * std::clamp(monthly, 0.0,
						kAgriLossCap);
				}
			}
			if (loss > 0.0) {
				remove_stock(site, static_cast<int>(product), loss);
				site.loss_today_t += loss;
			}
		}
		// Manutenção: dono paga ao setor de serviços (dinheiro conservado).
		if (site.capacity_t > 0.0 && site.type >= 0
				&& site.type < kStorageTypeCount) {
			const double due = site.capacity_t
				* kStorageMaintenancePerTonDay[site.type];
			double paid = 0.0;
			if (site.scope == STORAGE_PRIVATE) {
				paid = std::min(due, std::max(0.0, city.firm_cash));
				city.firm_cash -= paid;
			} else {
				paid = std::min(due, std::max(0.0, city.government_budget));
				city.government_budget -= paid;
			}
			city.service_cash += paid;
			site.maintenance_paid_today = paid;
		}
	}

	// 3. PULMÃO de saída: o que entrou no silo regional escoa ao mercado da
	//    cidade-polo ao longo de kStorageLungReleaseDays, pagando o custo de
	//    acesso da estrada (B5) ao setor de transporte.
	if (kStorageLungReleaseDays > 0.0) {
		for (StorageSite& site : ws_.storage_sites) {
			if (site.city_index < 0
					|| static_cast<std::size_t>(site.city_index)
						>= ws_.cities.size()
					|| !storage_type_is_region(site.type)
					|| site.inflow_stock.size() != products) {
				continue;
			}
			City& city = ws_.cities[static_cast<std::size_t>(site.city_index)];
			for (std::size_t product = 0; product < products; ++product) {
				const double inflow = site.inflow_stock[product];
				if (inflow <= 0.0 || site.stock[product] <= 0.0) {
					continue;
				}
				const double amount = std::min(site.stock[product],
					inflow / kStorageLungReleaseDays);
				if (amount <= 0.0) {
					continue;
				}
				Market& market = city.markets[product];
				site.stock[product] -= amount;
				site.inflow_stock[product] =
					std::max(0.0, inflow - amount);
				market.stock += amount;
				market.supply += amount;
				site.released_today_t += amount;
				// Custo de acesso região→polo: penalidade da estrada B5 sobre o
				// valor liberado, paga pelo dono ao transporte (conservado).
				const double price = market.price > 0.0 ? market.price
					: base_price(static_cast<int>(product));
				const double road = std::clamp(
					static_cast<double>(city.region.infra_road)
					/ static_cast<double>(kInfraMaxLevel), 0.0, 1.0);
				const double penalty = std::max(0.0,
					ws_.infra_access_penalty) * (1.0 - road);
				const double due = amount * std::max(0.0, price) * penalty;
				double paid = 0.0;
				if (site.scope == STORAGE_PRIVATE) {
					paid = std::min(due, std::max(0.0, city.firm_cash));
					city.firm_cash -= paid;
				} else {
					paid = std::min(due,
						std::max(0.0, city.government_budget));
					city.government_budget -= paid;
				}
				ws_.transport_cash += paid;
				site.access_paid_today += paid;
			}
		}
	}

	OGS_SIM_PHASE_LAP(kPhaseStorage);
}

} // namespace ogs
