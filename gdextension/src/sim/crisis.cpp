// D-126 — SOBREVIVÊNCIA EM CRISE (núcleo puro, sem godot-cpp).
//
// Contrato: docs/economy/D126_crisis.md. A ordem da reação é fixa e a visita é
// determinística (ordem do save). Nenhum número é inventado: os parâmetros vêm
// de data/definitions/economy/crisis.json (defaults declarados) ou do save.
#include "crisis.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <utility>
#include <vector>

#include "balance.h"
#include "bank.h"
#include "efficiency.h"
#include "sim_core.h"

namespace ogs {
namespace crisis {

namespace {

constexpr const char* kPrefixNational = "national:";
constexpr const char* kPrefixForeign = "foreign:";

bool starts_with(const std::string& text, const char* prefix) {
	return text.rfind(prefix, 0) == 0;
}

City* find_city(WorldState& ws, const std::string& id) {
	for (City& city : ws.cities) {
		if (city.id == id) {
			return &city;
		}
	}
	return nullptr;
}

Building* find_building(WorldState& ws, const std::string& city_id,
		const std::string& building_id) {
	for (Building& building : ws.buildings) {
		if (building.id == building_id && building.city == city_id) {
			return &building;
		}
	}
	return nullptr;
}

Group* find_group(WorldState& ws, const std::string& id) {
	for (Group& group : ws.groups) {
		if (group.id == id) {
			return &group;
		}
	}
	return nullptr;
}

std::string owner_group_id(const std::string& owner) {
	if (starts_with(owner, kPrefixNational)) {
		return owner.substr(std::char_traits<char>::length(kPrefixNational));
	}
	if (starts_with(owner, kPrefixForeign)) {
		const std::size_t begin = std::char_traits<char>::length(kPrefixForeign);
		const std::size_t at = owner.find('@', begin);
		return at == std::string::npos ? owner.substr(begin)
			: owner.substr(begin, at - begin);
	}
	return {};
}

Group* owner_group(WorldState& ws, const Building& building) {
	const std::string id = owner_group_id(building.owner);
	return id.empty() ? nullptr : find_group(ws, id);
}

double building_daily_cost(const Building& building) {
	if (building.mothballed) {
		return 0.0;
	}
	double operating = 1.0;
	if (building.recipe >= 0 || building.input_product >= 0) {
		operating = std::clamp(building.last_utilization, 0.05, 1.0);
	}
	return std::max(0.0, building.opex) * operating
		+ std::max(0.0, building.jobs) * std::max(0.0, building.wage)
			* operating;
}

double owner_cash(const WorldState& ws, const City& city,
		const Building& building) {
	if (building.owner == "state") {
		return std::max(0.0, city.government_budget);
	}
	if (building.owner == "private") {
		return std::max(0.0, city.firm_cash);
	}
	const std::string id = owner_group_id(building.owner);
	if (id.empty()) {
		return 0.0;
	}
	for (const Group& group : ws.groups) {
		if (group.id == id) {
			return std::max(0.0, group.cash);
		}
	}
	return 0.0;
}

// Debita o dono até o caixa disponível. Devolve o pago (0 quando não há dono
// reconhecido ou caixa). O crédito correspondente fica com o chamador.
double debit_owner(WorldState& ws, City& city, Building& building,
		double amount) {
	if (!(amount > 0.0)) {
		return 0.0;
	}
	if (building.owner == "state") {
		const double paid = std::min(amount,
			std::max(0.0, city.government_budget));
		city.government_budget -= paid;
		return paid;
	}
	if (building.owner == "private") {
		const double paid = std::min(amount, std::max(0.0, city.firm_cash));
		city.firm_cash -= paid;
		return paid;
	}
	Group* group = owner_group(ws, building);
	if (group == nullptr) {
		return 0.0;
	}
	const double paid = std::min(amount, std::max(0.0, group->cash));
	group->cash -= paid;
	return paid;
}

void credit_owner(WorldState& ws, City& city, Building& building,
		double amount) {
	if (!(amount > 0.0)) {
		return;
	}
	if (building.owner == "state") {
		city.government_budget += amount;
		return;
	}
	if (building.owner == "private") {
		city.firm_cash += amount;
		return;
	}
	Group* group = owner_group(ws, building);
	if (group != nullptr) {
		group->cash += amount;
		return;
	}
	// Dono desconhecido: mantém o dinheiro no caixa das firmas da cidade em vez
	// de destruí-lo (conservação antes de semântica de propriedade).
	city.firm_cash += amount;
}

// Capacidade efetiva do par (cidade, produto), sem depender de `last_*`.
double city_product_capacity(const WorldState& ws, std::size_t city_index,
		int product) {
	if (city_index >= ws.buildings_by_city.size()) {
		return 0.0;
	}
	double total = 0.0;
	for (const int bi : ws.buildings_by_city[city_index]) {
		const Building& building = ws.buildings[static_cast<std::size_t>(bi)];
		if (building.output_product == product) {
			total += std::max(0.0, building.capacity)
				* std::max(0.0, building.output_scale);
		}
	}
	return total;
}

double stock_share(const WorldState& ws, std::size_t city_index,
		const Building& building) {
	const double total = city_product_capacity(ws, city_index,
		building.output_product);
	if (total <= 1e-12) {
		return 0.0;
	}
	return std::clamp(std::max(0.0, building.capacity)
		* std::max(0.0, building.output_scale) / total, 0.0, 1.0);
}

double city_credit_limit(const WorldState& ws, std::size_t city_index) {
	if (city_index >= ws.cities.size()) {
		return 0.0;
	}
	const City& city = ws.cities[city_index];
	double assets = std::max(0.0, city.firm_cash)
		+ std::max(0.0, city.service_cash);
	if (city_index < ws.buildings_by_city.size()) {
		for (const int bi : ws.buildings_by_city[city_index]) {
			const Building& building =
				ws.buildings[static_cast<std::size_t>(bi)];
			if (building.owner == "private" || building.owner == "state") {
				assets += std::max(0.0, building.capital_per_capacity)
					* std::max(0.0, building.capacity);
			}
		}
	}
	return std::max(0.0, ws.crisis.credit_asset_share * assets
		- city.firm_debt - city.firm_credit_arrears);
}

bool borrow_city(WorldState& ws, const std::string& city_id, double amount,
		double* out_granted, std::string* error) {
	City* city = find_city(ws, city_id);
	if (city == nullptr) {
		if (error != nullptr) {
			*error = "cidade desconhecida";
		}
		return false;
	}
	double granted = 0.0;
	std::string bank_error;
	if (!bank::lend_city(ws, city_id, amount, &granted, &bank_error)) {
		if (error != nullptr) {
			*error = bank_error.empty() ? "credito recusado" : bank_error;
		}
		return false;
	}
	city->firm_credit_rate_add = std::max(city->firm_credit_rate_add,
		std::max(0.0, ws.crisis.credit_rate_add));
	ws.crisis_loans += granted;
	if (out_granted != nullptr) {
		*out_granted = granted;
	}
	return granted > 0.0;
}

bool borrow_group(WorldState& ws, Group& group, double amount,
		double* out_granted, std::string* error) {
	double granted = 0.0;
	std::string bank_error;
	if (!bank::lend(ws, group.id, amount, &granted, &bank_error)) {
		if (error != nullptr) {
			*error = bank_error.empty() ? "credito recusado" : bank_error;
		}
		return false;
	}
	group.credit_rate_add = std::max(group.credit_rate_add,
		std::max(0.0, ws.crisis.credit_rate_add));
	ws.crisis_loans += granted;
	if (out_granted != nullptr) {
		*out_granted = granted;
	}
	return granted > 0.0;
}

double start_efficiency(const WorldState& ws) {
	return std::clamp(
		eff::curve(ws, EfficiencyClass::kFactory).start, 0.0, 1.0);
}

double building_start_efficiency(const WorldState& ws, const City& city,
		const Building& building) {
	return std::clamp(
		eff::curve(ws, eff::classify(ws, city, building)).start, 0.0, 1.0);
}

// Paga a rescisão ao mercado de trabalho da cidade.
void pay_severance(SimCore& core, City& city, double amount) {
	if (amount > 0.0) {
		core.credit_households(city.pop, amount);
	}
}

// Comprador do leilão: grupos na ordem do save e, depois, o Estado. Devolve o
// índice do grupo (-1 = nenhum) e marca `state_buyer`.
int choose_buyer(WorldState& ws, const City& city, double price,
		const std::string& seller_group, bool* state_buyer) {
	*state_buyer = false;
	if (!(price > 0.0)) {
		return -1;
	}
	for (std::size_t index = 0; index < ws.groups.size(); ++index) {
		const Group& group = ws.groups[index];
		if (group.id == seller_group) {
			continue;
		}
		if (group.cash + 1e-9 >= price) {
			return static_cast<int>(index);
		}
	}
	const double cap = std::max(0.0, city.government_budget)
		* std::clamp(ws.crisis.auction_state_share, 0.0, 1.0);
	if (cap + 1e-9 >= price) {
		*state_buyer = true;
	}
	return -1;
}

void pay_auction_proceeds(WorldState& ws, SimCore& core, City& city,
		Building& building, double price, double* bank_money_delta) {
	double remainder = std::max(0.0, price);
	// 1. Rescisão em atraso vai às famílias (obrigação morre com a venda).
	const double wage = std::min(remainder,
		std::max(0.0, building.wage_arrears));
	if (wage > 0.0) {
		building.wage_arrears -= wage;
		city.pop.arrears_receivable = std::max(0.0,
			city.pop.arrears_receivable - wage);
		core.credit_households(city.pop, wage);
		remainder -= wage;
	}
	Group* seller = owner_group(ws, building);
	if (seller != nullptr) {
		Bank* bank = bank::lender(ws, *seller);
		const double claim = std::max(0.0,
			seller->debt + seller->credit_arrears);
		if (bank != nullptr && claim > 0.0 && remainder > 0.0) {
			const double paid = std::min({ remainder, claim,
				std::max(0.0, bank->deposits) });
			if (paid > 0.0) {
				const double debt_paid = std::min(paid, seller->debt);
				seller->debt -= debt_paid;
				const double arrears_paid = std::min(
					std::max(0.0, paid - debt_paid),
					seller->credit_arrears);
				seller->credit_arrears -= arrears_paid;
				bank->loans = std::max(0.0, bank->loans - debt_paid);
				bank->arrears = std::max(0.0,
					bank->arrears - arrears_paid);
				bank->deposits -= paid;
				if (bank_money_delta != nullptr) {
					*bank_money_delta -= paid;
				}
				remainder -= paid;
			}
		}
		seller->cash += remainder;
		return;
	}
	if (building.owner == "state") {
		city.government_budget += remainder;
		return;
	}
	city.firm_cash += remainder;
}

// Venda imediata do prédio. Sem comprador marca `bankrupt` (remoção no
// início do próximo tick, regra existente).
// D-126 (lab): `OGS_CRISIS_TRACE=1` imprime o leilão SEM comprador com
// cidade/nação e bem — só mede o mundo real; não muda estado nem regra.
void trace_unsold_auction(const WorldState& ws, const char* kind,
		const City& city, const std::string& id, const char* detail,
		double price) {
	if (std::getenv("OGS_CRISIS_TRACE") == nullptr) {
		return;
	}
	std::fprintf(stdout,
		"[crisis-auction-unsold] day=%d kind=%s city=%s nation=%s id=%s "
		"detail=%s price=%.2f gov=%.2f\n",
		ws.day, kind, city.id.c_str(), city.nation.c_str(), id.c_str(),
		detail, price, city.government_budget);
}

bool auction_building(WorldState& ws, SimCore& core, Building& building,
		double* bank_money_delta, std::string* error) {
	City* city = find_city(ws, building.city);
	if (city == nullptr) {
		if (error != nullptr) {
			*error = "cidade desconhecida";
		}
		return false;
	}
	const double book = std::max(1.0,
		std::max(0.0, building.capital_per_capacity)
			* std::max(0.0, building.capacity));
	const double price = book * (1.0 - std::clamp(
		ws.crisis.auction_discount, 0.0, 1.0));
	const std::string seller = owner_group_id(building.owner);
	bool state_buyer = false;
	const int buyer_index = choose_buyer(ws, *city, price, seller,
		&state_buyer);
	if (state_buyer && building.owner == "state") {
		state_buyer = false;
	}
	if (buyer_index < 0 && !state_buyer) {
		building.bankrupt = true;
		ws.crisis_auctions_unsold += 1;
		const int output = building.output_product;
		const char* detail = output >= 0
			&& output < static_cast<int>(ws.product_names.size())
			? ws.product_names[static_cast<std::size_t>(output)].c_str()
			: (building.input_product >= 0
				&& building.input_product
					< static_cast<int>(ws.product_names.size())
				? ws.product_names[static_cast<std::size_t>(
					building.input_product)].c_str() : "-");
		trace_unsold_auction(ws, "building", *city, building.id, detail,
			price);
		if (error != nullptr) {
			*error = "sem comprador no leilao";
		}
		return false;
	}
	std::string new_owner;
	if (buyer_index >= 0) {
		Group& buyer = ws.groups[static_cast<std::size_t>(buyer_index)];
		buyer.cash -= price;
		new_owner = buyer.origin_nation == city->nation
			? std::string(kPrefixNational) + buyer.id
			: std::string(kPrefixForeign) + buyer.id + "@" + city->nation;
	} else {
		city->government_budget -= price;
		new_owner = "state";
	}
	pay_auction_proceeds(ws, core, *city, building, price, bank_money_delta);
	building.owner = new_owner;
	building.local_share = 0.0;
	building.partner_group.clear();
	building.royalty = 0.0;
	building.bankrupt = false;
	building.loss_days = 0;
	building.crisis_stage = 0;
	building.mothballed = false;
	building.mothball_day = -1;
	building.mothball_unpaid_days = 0;
	building.reopen_signal_days = 0;
	building.output_scale = 1.0;
	building.production_efficiency = building_start_efficiency(ws, *city,
		building);
	building.efficiency_days = 0;
	building.transition_days = 0;
	building.equipment = 0.0;
	building.last_layoff_day = -1000000;
	building.last_credit_day = -1000000;
	building.last_liquidation_day = -1000000;
	ws.crisis_auctions += 1;
	ws.crisis_auction_value += price;
	return true;
}

bool auction_power_plant(WorldState& ws, PowerPlant& plant,
		double* bank_money_delta, std::string* error) {
	if (plant.city_index < 0
			|| plant.city_index >= static_cast<int>(ws.cities.size())) {
		plant.bankrupt = true;
		plant.mothballed = false;
		ws.crisis_auctions_unsold += 1;
		if (error != nullptr) {
			*error = "usina sem cidade";
		}
		return false;
	}
	City& city = ws.cities[static_cast<std::size_t>(plant.city_index)];
	const double book = std::max(1.0, std::max(0.0, plant.capex));
	const double price = book * (1.0 - std::clamp(
		ws.crisis.auction_discount, 0.0, 1.0));
	bool state_buyer = false;
	const int buyer_index = choose_buyer(ws, city, price,
		plant.owner_group, &state_buyer);
	if (state_buyer && plant.owner_group.empty()) {
		state_buyer = false;
	}
	if (buyer_index < 0 && !state_buyer) {
		plant.bankrupt = true;
		plant.mothballed = false;
		ws.crisis_auctions_unsold += 1;
		trace_unsold_auction(ws, "plant", city, plant.id,
			power_fuel_name(plant.fuel), price);
		if (error != nullptr) {
			*error = "sem comprador no leilao da usina";
		}
		return false;
	}
	if (buyer_index >= 0) {
		Group& buyer = ws.groups[static_cast<std::size_t>(buyer_index)];
		buyer.cash -= price;
		const std::string old = plant.owner_group;
		plant.owner_group = buyer.id;
		plant.owner_nation = buyer.origin_nation;
		Group* seller = old.empty() ? nullptr : find_group(ws, old);
		if (seller != nullptr) {
			Bank* bank = bank::lender(ws, *seller);
			const double claim = std::max(0.0,
				seller->debt + seller->credit_arrears);
			const double paid = bank == nullptr ? 0.0 : std::min({
				price, claim, std::max(0.0, bank->deposits) });
			if (paid > 0.0) {
				seller->debt = std::max(0.0, seller->debt - paid);
				bank->loans = std::max(0.0, bank->loans - paid);
				bank->deposits -= paid;
				if (bank_money_delta != nullptr) {
					*bank_money_delta -= paid;
				}
			}
			seller->cash += std::max(0.0, price - paid);
		} else {
			city.government_budget += price;
		}
	} else {
		city.government_budget -= price;
		const std::string old = plant.owner_group;
		plant.owner_group.clear();
		Group* seller = old.empty() ? nullptr : find_group(ws, old);
		if (seller != nullptr) {
			seller->cash += price;
		} else {
			// Estado comprando do Estado: a troca de caixa é conservada.
			city.government_budget += price;
		}
	}
	plant.bankrupt = false;
	plant.loss_days = 0;
	plant.mothballed = false;
	plant.mothball_day = -1;
	plant.mothball_unpaid_days = 0;
	plant.crisis_stage = 0;
	plant.reopen_signal_days = 0;
	plant.production_efficiency = start_efficiency(ws);
	ws.crisis_auctions += 1;
	ws.crisis_auction_value += price;
	ws.crisis_power_auctions += 1;
	ws.crisis_power_auction_value += price;
	return true;
}

double plant_owner_cash(WorldState& ws, const City& city,
		const PowerPlant& plant) {
	if (plant.owner_group.empty()) {
		return std::max(0.0, city.government_budget);
	}
	Group* group = find_group(ws, plant.owner_group);
	return group == nullptr ? 0.0 : std::max(0.0, group->cash);
}

double debit_plant_owner(WorldState& ws, City& city, PowerPlant& plant,
		double amount) {
	if (!(amount > 0.0)) {
		return 0.0;
	}
	if (plant.owner_group.empty()) {
		const double paid = std::min(amount,
			std::max(0.0, city.government_budget));
		city.government_budget -= paid;
		return paid;
	}
	Group* group = find_group(ws, plant.owner_group);
	if (group == nullptr) {
		return 0.0;
	}
	const double paid = std::min(amount, std::max(0.0, group->cash));
	group->cash -= paid;
	return paid;
}

double plant_unit_cost(const PowerPlant& plant) {
	return std::max(0.0, plant.opex_per_mwh)
		+ (std::max(0.0, plant.mw) > 1e-9
			? std::max(0.0, plant.jobs) * std::max(0.0, plant.wage)
				/ std::max(0.0, plant.mw)
			: 0.0);
}

// D-126 (correção): energia que a usina consegue entregar no pico, na escala
// do dia. Ativa = a disponibilidade que o mérito usou hoje (`day_avail_mw`,
// já com CF/calibração/eficiência). Hibernada = o potencial que ela teria ao
// reabrir (CF declarado × calibração × eficiência retida) — é o que entra na
// decisão de reabertura quando a rede fica curta.
double plant_potential_mwh(const WorldState& ws, const PowerPlant& plant) {
	if (plant.city_index < 0
			|| plant.city_index >= static_cast<int>(ws.cities.size())) {
		return 0.0;
	}
	if (!plant.mothballed && plant.day_avail_mw > 0.0) {
		return std::max(0.0, plant.day_avail_mw) * 24.0;
	}
	if (plant.city_index >= static_cast<int>(ws.power_cities.size())) {
		return 0.0;
	}
	const double mw = std::max(0.0, plant.mw);
	if (mw <= 0.0 || plant.fuel < 0 || plant.fuel >= kPowerFuelCount) {
		return 0.0;
	}
	const PowerCityState& state = ws.power_cities[
		static_cast<std::size_t>(plant.city_index)];
	double cf = std::clamp(ws.power_params.cf[plant.fuel], 0.0, 1.0);
	const double scale = std::clamp(state.cf_scale[plant.fuel], 0.0, 5.0);
	const double efficiency = std::clamp(plant.production_efficiency, 0.0,
		1.0);
	return mw * 24.0 * std::clamp(cf * scale, 0.0, 1.0) * efficiency;
}

// D-126 (correção): capacidade FIRME que a crise tem de manter por rede/nacão.
// A demanda do dia (`PowerGridState::demand_mwh`) é o único sinal de carga que
// o despacho E2/E3 modela, então o pico é a própria demanda diária; a margem
// de reserva é declarada em `crisis.power_reserve_margin`. Usinas ATIVAS mais
// baratas por custo marginal formam o prefixo necessário; se elas não cobrem
// o pico × (1 + margem), as hibernadas mais baratas são necessárias e podem
// reabrir. Só o que está fora do prefixo é excedente (candidato a demitir,
// hibernar e ir a leilão).
std::vector<char> plant_necessity(const WorldState& ws) {
	std::vector<char> needed(ws.power_plants.size(), 0);
	std::map<std::string, std::vector<std::size_t>> by_nation;
	for (std::size_t index = 0; index < ws.power_plants.size(); ++index) {
		const PowerPlant& plant = ws.power_plants[index];
		if (plant.bankrupt || plant.city_index < 0
				|| plant.city_index >= static_cast<int>(ws.cities.size())) {
			continue;
		}
		by_nation[ws.cities[static_cast<std::size_t>(plant.city_index)].nation]
			.push_back(index);
	}
	const double margin = std::clamp(ws.crisis.power_reserve_margin, 0.0, 1.0);
	const auto by_cost = [&ws](const std::vector<std::size_t>& pool) {
		std::vector<std::size_t> sorted = pool;
		std::stable_sort(sorted.begin(), sorted.end(),
			[&ws](std::size_t a, std::size_t b) {
				const PowerPlant& pa = ws.power_plants[a];
				const PowerPlant& pb = ws.power_plants[b];
				// Ordem de mérito ESTRUTURAL: o custo marginal declarado
				// (O&M por MWh; o termo de combustível do dia vive no despacho
				// E2/E3 e não muda o prefixo de capacidade). Não usar a folha:
				// demitir reduziria o custo unitário e mudaria a ordem, o que
				// inverteria a histerese.
				const double ca = std::max(0.0, pa.opex_per_mwh);
				const double cb = std::max(0.0, pb.opex_per_mwh);
				if (ca != cb) {
					return ca < cb;
				}
				if (pa.fuel != pb.fuel) {
					return pa.fuel < pb.fuel;
				}
				if (pa.city_index != pb.city_index) {
					return pa.city_index < pb.city_index;
				}
				return pa.id < pb.id;
			});
		return sorted;
	};
	for (const auto& row : by_nation) {
		const std::vector<std::size_t>& pool = row.second;
		const auto grid = ws.power_grids.find(row.first);
		const double demand = grid == ws.power_grids.end()
			? 0.0 : std::max(0.0, grid->second.demand_mwh);
		if (!(demand > 0.0)) {
			// Sem carga medida (ausência declarada): não se hiberna nada por
			// falta de necessidade — o comportamento conservador é manter.
			for (const std::size_t index : pool) {
				needed[index] = 1;
			}
			continue;
		}
		const double required = demand * (1.0 + margin);
		std::vector<std::size_t> active;
		std::vector<std::size_t> mothballed;
		for (const std::size_t index : pool) {
			if (ws.power_plants[index].mothballed) {
				mothballed.push_back(index);
			} else {
				active.push_back(index);
			}
		}
		double covered = 0.0;
		for (const std::size_t index : by_cost(active)) {
			if (covered + 1e-9 >= required) {
				break;
			}
			needed[index] = 1;
			covered += plant_potential_mwh(ws, ws.power_plants[index]);
		}
		if (covered + 1e-9 < required) {
			// A rede está curta com o parque ativo: as hibernadas mais baratas
			// são necessárias e reabrem (sujeitas à histerese do sinal).
			for (const std::size_t index : by_cost(mothballed)) {
				if (covered + 1e-9 >= required) {
					break;
				}
				needed[index] = 1;
				covered += plant_potential_mwh(ws,
					ws.power_plants[index]);
			}
		}
	}
	return needed;
}

void react_power(WorldState& ws, SimCore& core, PowerPlant& plant,
		bool needed, double* bank_money_delta) {
	if (plant.bankrupt) {
		return;
	}
	if (plant.city_index < 0
			|| plant.city_index >= static_cast<int>(ws.cities.size())) {
		return;
	}
	City& city = ws.cities[static_cast<std::size_t>(plant.city_index)];
	const double price = [&]() {
		const auto found = ws.power_grids.find(city.nation);
		if (found != ws.power_grids.end() && found->second.price > 0.0) {
			return found->second.price;
		}
		return std::max(0.0, city.power_price);
	}();
	const double gen = std::max(0.0, plant.generated_mwh);
	const double revenue = gen * price;
	const double payroll = std::max(0.0, plant.jobs)
		* std::max(0.0, plant.wage);
	if (plant.mothballed) {
		// D-126: manutencao de hibernacao = `mothball_opex_share` (10%) do
		// OPEX diario da usina (O&M por MWh x capacidade x 24 h), NAO da folha
		// parada. A folha nao e' custo de hibernacao: sem producao nao ha salario
		// proporcional, e a spec manda pagar o opex minimo.
		const double daily_opex = std::max(0.0, plant.opex_per_mwh)
			* std::max(0.0, plant.mw) * 24.0;
		const double maintenance = daily_opex
			* std::clamp(ws.crisis.mothball_opex_share, 0.0, 1.0);
		const double paid = debit_plant_owner(ws, city, plant, maintenance);
		if (paid > 0.0) {
			city.service_cash += paid;
		}
		if (paid + 1e-9 < maintenance) {
			plant.mothball_unpaid_days += 1;
		} else {
			plant.mothball_unpaid_days = 0;
		}
		const double unit = plant_unit_cost(plant);
		// D-126 (correção): reabre pelo sinal de preço que já existia OU
		// porque a rede voltou a precisar da usina (pico × reserva). O mesmo
		// contador de dias dá a histerese nos dois casos.
		if (needed || (price > unit * 1.05 && unit > 0.0)) {
			plant.reopen_signal_days += 1;
		} else {
			plant.reopen_signal_days = 0;
		}
		const double reopen_cost = std::max(0.0, plant.mw)
			* std::max(0.0, ws.crisis.reopen_cost_per_capacity);
		if (plant.reopen_signal_days
				>= std::max(1, ws.crisis.reopen_signal_days)
				&& plant_owner_cash(ws, city, plant) + 1e-9 >= reopen_cost) {
			const double paid_reopen = debit_plant_owner(ws, city, plant,
				reopen_cost);
			if (paid_reopen > 0.0) {
				city.service_cash += paid_reopen;
				plant.mothballed = false;
				plant.mothball_day = -1;
				plant.reopen_signal_days = 0;
				// Histerese: zerar os dias de prejuízo evita que a usina
				// hiberne de novo no dia seguinte só por herdar o contador.
				plant.loss_days = 0;
				plant.crisis_stage = 0;
				ws.crisis_reopens += 1;
				ws.crisis_reopen_paid += paid_reopen;
			}
		}
		if (plant.mothball_unpaid_days
				>= std::max(1, ws.crisis.mothball_grace_days)) {
			auction_power_plant(ws, plant, bank_money_delta, nullptr);
		}
		return;
	}
	// D-126 (correção): a usina paga só o custo de caixa do dia (`day_cost`,
	// o mesmo de `phase_power_settle`: combustível + O&M + salários
	// proporcionais à geração). A folha parada de uma usina ociosa não é
	// prejuízo de operação — é custo de RESERVA. Para a usina NECESSÁRIA ao
	// pico da rede (pico × (1 + margem)) esse custo não vira gatilho de
	// demissão/hibernação/leilão: sem pagamento de capacidade, apenas se
	// registra o prejuízo. Só a usina EXCEDENTE é candidata, comparando a
	// receita com o custo cheio (reserva desperdiçada).
	const double avail_mw = std::max(0.0, plant.day_avail_mw);
	const double operating = avail_mw > 1e-9
		? std::clamp((gen / 24.0) / avail_mw, 0.0, 1.0)
		: (gen > 0.0 ? 1.0 : 0.0);
	double variable_cost = gen > 0.0
		? std::max(0.0, plant.day_cost) : 0.0;
	if (variable_cost <= 0.0 && gen > 0.0) {
		// Fallback quando a liquidação do dia não rodou (sem energia pronta):
		// O&M + salários proporcionais, sem inventar combustível.
		variable_cost = gen * std::max(0.0, plant.opex_per_mwh)
			+ payroll * operating;
	}
	const double idle_fixed = payroll * (1.0 - operating);
	const double cost = needed ? variable_cost
		: variable_cost + idle_fixed;
	if (revenue + 1e-9 < cost) {
		plant.loss_days += 1;
	} else {
		plant.loss_days = 0;
	}
	if (needed) {
		return;
	}
	if (plant.loss_days >= std::max(1, ws.crisis.layoff_days)
			&& ws.day - plant.last_layoff_day
				>= std::max(1, ws.crisis.layoff_cooldown_days)
			&& plant.jobs > 1.0) {
		const double share = std::clamp(ws.crisis.layoff_share, 0.0, 1.0);
		const double keep = std::max(0.0,
			ws.crisis.layoff_min_jobs_share);
		double cut = std::max(0.0, plant.jobs * share);
		cut = std::min(cut, std::max(0.0, plant.jobs * (1.0 - keep)));
		const double per_job = std::max(0.0, plant.wage)
			* std::max(0.0, ws.crisis.severance_days);
		if (per_job > 0.0) {
			const double affordable = std::floor(
				plant_owner_cash(ws, city, plant) / per_job);
			cut = std::min(cut, std::max(0.0, affordable));
		}
		if (cut >= 1.0 - 1e-9) {
			const double paid = debit_plant_owner(ws, city, plant,
				cut * per_job);
			if (paid > 0.0) {
				pay_severance(core, city, paid);
				plant.jobs = std::max(0.0, plant.jobs - cut);
				plant.last_layoff_day = ws.day;
				ws.crisis_layoffs += 1;
				ws.crisis_jobs_cut += cut;
				ws.crisis_severance_paid += paid;
			}
		}
	}
	if (plant.loss_days >= std::max(1, ws.crisis.mothball_days)) {
		plant.mothballed = true;
		plant.mothball_day = ws.day;
		plant.mothball_unpaid_days = 0;
		plant.reopen_signal_days = 0;
		plant.crisis_stage = 5;
		plant.production_efficiency = std::clamp(
			plant.production_efficiency
				* std::clamp(ws.crisis.mothball_efficiency_retention,
					0.0, 1.0), 0.0, 1.0);
		ws.crisis_mothballs += 1;
		return;
	}
	if (plant.loss_days >= std::max(1, ws.crisis.bankrupt_days)) {
		auction_power_plant(ws, plant, bank_money_delta, nullptr);
	}
}

} // namespace

void apply_params(WorldState& ws, const std::map<std::string, double>& values) {
	CrisisParams& p = ws.crisis;
	const auto get = [&values](const char* key, double fallback) {
		const auto found = values.find(key);
		return found == values.end() ? fallback : found->second;
	};
	p.enabled = get("enabled", p.enabled ? 1.0 : 0.0) > 0.5;
	p.cut_days = std::max(0, int(get("cut_days", p.cut_days)));
	p.layoff_days = std::max(0, int(get("layoff_days", p.layoff_days)));
	p.layoff_share = std::clamp(get("layoff_share", p.layoff_share), 0.0, 1.0);
	p.layoff_min_jobs_share = std::clamp(
		get("layoff_min_jobs_share", p.layoff_min_jobs_share), 0.0, 1.0);
	p.severance_days = std::max(0.0,
		get("severance_days", p.severance_days));
	p.layoff_cooldown_days = std::max(1,
		int(get("layoff_cooldown_days", p.layoff_cooldown_days)));
	p.credit_days = std::max(0, int(get("credit_days", p.credit_days)));
	p.credit_working_days = std::max(0.0,
		get("credit_working_days", p.credit_working_days));
	p.credit_asset_share = std::clamp(
		get("credit_asset_share", p.credit_asset_share), 0.0, 1.0);
	p.credit_rate_add = std::max(0.0,
		get("credit_rate_add", p.credit_rate_add));
	p.credit_renegotiation_days = std::max(1,
		int(get("credit_renegotiation_days", p.credit_renegotiation_days)));
	p.credit_penalty = std::clamp(
		get("credit_penalty", p.credit_penalty), 0.0, 1.0);
	p.credit_max_renegotiations = std::max(0,
		int(get("credit_max_renegotiations", p.credit_max_renegotiations)));
	p.liquidate_days = std::max(0,
		int(get("liquidate_days", p.liquidate_days)));
	p.liquidation_discount = std::clamp(
		get("liquidation_discount", p.liquidation_discount), 0.0, 1.0);
	p.liquidation_share = std::clamp(
		get("liquidation_share", p.liquidation_share), 0.0, 1.0);
	p.mothball_days = std::max(0,
		int(get("mothball_days", p.mothball_days)));
	p.mothball_opex_share = std::clamp(
		get("mothball_opex_share", p.mothball_opex_share), 0.0, 1.0);
	p.mothball_efficiency_retention = std::clamp(
		get("mothball_efficiency_retention",
			p.mothball_efficiency_retention), 0.0, 1.0);
	p.reopen_cost_per_capacity = std::max(0.0,
		get("reopen_cost_per_capacity", p.reopen_cost_per_capacity));
	p.reopen_signal_days = std::max(1,
		int(get("reopen_signal_days", p.reopen_signal_days)));
	p.mothball_grace_days = std::max(1,
		int(get("mothball_grace_days", p.mothball_grace_days)));
	p.bankrupt_days = std::max(1,
		int(get("bankrupt_days", p.bankrupt_days)));
	p.auction_discount = std::clamp(
		get("auction_discount", p.auction_discount), 0.0, 1.0);
	p.auction_state_share = std::clamp(
		get("auction_state_share", p.auction_state_share), 0.0, 1.0);
	p.farm_area_reduction = std::clamp(
		get("farm_area_reduction", p.farm_area_reduction), 0.0, 1.0);
	p.farm_area_reduction_days = std::max(0,
		int(get("farm_area_reduction_days", p.farm_area_reduction_days)));
	p.power_reserve_margin = std::clamp(
		get("power_reserve_margin", p.power_reserve_margin), 0.0, 1.0);
}

std::map<std::string, double> params_map(const WorldState& ws) {
	const CrisisParams& p = ws.crisis;
	return {
		{"enabled", p.enabled ? 1.0 : 0.0},
		{"cut_days", double(p.cut_days)},
		{"layoff_days", double(p.layoff_days)},
		{"layoff_share", p.layoff_share},
		{"layoff_min_jobs_share", p.layoff_min_jobs_share},
		{"severance_days", p.severance_days},
		{"layoff_cooldown_days", double(p.layoff_cooldown_days)},
		{"credit_days", double(p.credit_days)},
		{"credit_working_days", p.credit_working_days},
		{"credit_asset_share", p.credit_asset_share},
		{"credit_rate_add", p.credit_rate_add},
		{"credit_renegotiation_days", double(p.credit_renegotiation_days)},
		{"credit_penalty", p.credit_penalty},
		{"credit_max_renegotiations", double(p.credit_max_renegotiations)},
		{"liquidate_days", double(p.liquidate_days)},
		{"liquidation_discount", p.liquidation_discount},
		{"liquidation_share", p.liquidation_share},
		{"mothball_days", double(p.mothball_days)},
		{"mothball_opex_share", p.mothball_opex_share},
		{"mothball_efficiency_retention", p.mothball_efficiency_retention},
		{"reopen_cost_per_capacity", p.reopen_cost_per_capacity},
		{"reopen_signal_days", double(p.reopen_signal_days)},
		{"mothball_grace_days", double(p.mothball_grace_days)},
		{"bankrupt_days", double(p.bankrupt_days)},
		{"auction_discount", p.auction_discount},
		{"auction_state_share", p.auction_state_share},
		{"farm_area_reduction", p.farm_area_reduction},
		{"farm_area_reduction_days", double(p.farm_area_reduction_days)},
		{"power_reserve_margin", p.power_reserve_margin},
	};
}

bool layoff(WorldState& ws, SimCore& core, const std::string& city_id,
		const std::string& building_id, double share, std::string* error) {
	Building* building = find_building(ws, city_id, building_id);
	City* city = find_city(ws, city_id);
	if (building == nullptr || city == nullptr) {
		if (error != nullptr) {
			*error = "construcao/cidade desconhecida";
		}
		return false;
	}
	if (building->mothballed || building->bankrupt) {
		if (error != nullptr) {
			*error = "construcao hibernada/falida";
		}
		return false;
	}
	const double requested = share > 0.0 ? share : ws.crisis.layoff_share;
	const double keep = std::max(0.0, ws.crisis.layoff_min_jobs_share);
	double cut = std::max(0.0, building->jobs * std::clamp(requested, 0.0, 1.0));
	cut = std::min(cut, std::max(0.0, building->jobs * (1.0 - keep)));
	const double per_job = std::max(0.0, building->wage)
		* std::max(0.0, ws.crisis.severance_days);
	if (cut < 1.0 - 1e-9 || per_job <= 0.0) {
		if (error != nullptr) {
			*error = "sem vagas ou salario para demitir";
		}
		return false;
	}
	const double affordable = std::floor(
		owner_cash(ws, *city, *building) / per_job);
	cut = std::min(cut, std::max(0.0, affordable));
	if (cut < 1.0 - 1e-9) {
		if (error != nullptr) {
			*error = "caixa insuficiente para a rescisao";
		}
		return false;
	}
	const double paid = debit_owner(ws, *city, *building, cut * per_job);
	if (paid <= 0.0) {
		if (error != nullptr) {
			*error = "caixa insuficiente para a rescisao";
		}
		return false;
	}
	pay_severance(core, *city, paid);
	building->jobs = std::max(0.0, building->jobs - cut);
	building->workers = std::min(building->workers, building->jobs);
	building->last_layoff_day = ws.day;
	building->crisis_stage = std::max(building->crisis_stage, 2);
	ws.crisis_layoffs += 1;
	ws.crisis_jobs_cut += cut;
	ws.crisis_severance_paid += paid;
	return true;
}

bool borrow_working_capital(WorldState& ws, SimCore& core,
		const std::string& city_id, double amount, std::string* error) {
	(void)core;
	City* city = find_city(ws, city_id);
	if (city == nullptr) {
		if (error != nullptr) {
			*error = "cidade desconhecida";
		}
		return false;
	}
	const std::size_t city_index = static_cast<std::size_t>(
		city - ws.cities.data());
	double need = amount;
	if (!(need > 0.0)) {
		double daily = 0.0;
		if (city_index < ws.buildings_by_city.size()) {
			for (const int bi : ws.buildings_by_city[city_index]) {
				daily += building_daily_cost(
					ws.buildings[static_cast<std::size_t>(bi)]);
			}
		}
		need = std::max(0.0, ws.crisis.credit_working_days * daily
			- std::max(0.0, city->firm_cash));
	}
	const double limit = city_credit_limit(ws, city_index);
	const double ask = std::min(need, limit);
	if (!(ask > 1e-9)) {
		if (error != nullptr) {
			*error = "sem limite de credito de giro";
		}
		return false;
	}
	return borrow_city(ws, city_id, ask, nullptr, error);
}

bool liquidate_stock(WorldState& ws, SimCore& core, const std::string& city_id,
		const std::string& building_id, double share, std::string* error) {
	(void)core;
	Building* building = find_building(ws, city_id, building_id);
	City* city = find_city(ws, city_id);
	if (building == nullptr || city == nullptr) {
		if (error != nullptr) {
			*error = "construcao/cidade desconhecida";
		}
		return false;
	}
	if (building->output_product < 0
			|| building->output_product >= ws.products) {
		if (error != nullptr) {
			*error = "construcao sem produto";
		}
		return false;
	}
	const std::size_t city_index = static_cast<std::size_t>(
		city - ws.cities.data());
	Market& market = city->markets[building->output_product];
	const double price = std::max(0.0, market.price);
	if (price <= 1e-12 || market.stock <= 1e-12) {
		if (error != nullptr) {
			*error = "sem estoque/preco para liquidar";
		}
		return false;
	}
	const double fraction = std::clamp(
		share > 0.0 ? share : ws.crisis.liquidation_share, 0.0, 1.0);
	double tons = std::max(0.0, market.stock)
		* stock_share(ws, city_index, *building) * fraction;
	const double unit = price * (1.0 - std::clamp(
		ws.crisis.liquidation_discount, 0.0, 1.0));
	if (unit <= 1e-12 || tons <= 1e-12) {
		if (error != nullptr) {
			*error = "estoque atribuido insuficiente";
		}
		return false;
	}
	const double value = tons * unit;
	if (value <= 1e-9) {
		if (error != nullptr) {
			*error = "exterior sem caixa para a liquidacao";
		}
		return false;
	}
	// O caixa do exterior pode ficar negativo por design (é o espelho do
	// comércio): a compra de liquidação entra inteira, sem truncar o lote.
	ws.foreign_cash -= value;
	market.stock = std::max(0.0, market.stock - tons);
	credit_owner(ws, *city, *building, value);
	building->last_liquidation_day = ws.day;
	building->crisis_stage = std::max(building->crisis_stage, 4);
	ws.crisis_liquidations += 1;
	ws.crisis_liquidated_t += tons;
	ws.crisis_liquidated_value += value;
	return true;
}

bool mothball(WorldState& ws, SimCore& core, const std::string& city_id,
		const std::string& building_id, std::string* error) {
	(void)core;
	Building* building = find_building(ws, city_id, building_id);
	if (building == nullptr) {
		if (error != nullptr) {
			*error = "construcao desconhecida";
		}
		return false;
	}
	if (building->mothballed) {
		if (error != nullptr) {
			*error = "construcao ja hibernada";
		}
		return false;
	}
	building->mothballed = true;
	building->mothball_day = ws.day;
	building->mothball_unpaid_days = 0;
	building->reopen_signal_days = 0;
	building->crisis_stage = 5;
	building->production_efficiency = std::clamp(
		building->production_efficiency
			* std::clamp(ws.crisis.mothball_efficiency_retention, 0.0, 1.0),
		0.0, 1.0);
	ws.crisis_mothballs += 1;
	return true;
}

bool reopen(WorldState& ws, SimCore& core, const std::string& city_id,
		const std::string& building_id, std::string* error) {
	(void)core;
	Building* building = find_building(ws, city_id, building_id);
	City* city = find_city(ws, city_id);
	if (building == nullptr || city == nullptr) {
		if (error != nullptr) {
			*error = "construcao/cidade desconhecida";
		}
		return false;
	}
	if (!building->mothballed) {
		if (error != nullptr) {
			*error = "construcao nao esta hibernada";
		}
		return false;
	}
	const double cost = std::max(0.0, building->capacity)
		* std::max(0.0, ws.crisis.reopen_cost_per_capacity);
	const double paid = debit_owner(ws, *city, *building, cost);
	if (paid + 1e-9 < cost) {
		if (paid > 0.0) {
			credit_owner(ws, *city, *building, paid);
		}
		if (error != nullptr) {
			*error = "caixa insuficiente para reabrir";
		}
		return false;
	}
	city->service_cash += paid;
	building->mothballed = false;
	building->mothball_day = -1;
	building->mothball_unpaid_days = 0;
	building->reopen_signal_days = 0;
	building->reopen_paid_today = paid;
	ws.crisis_reopens += 1;
	ws.crisis_reopen_paid += paid;
	return true;
}

bool auction(WorldState& ws, SimCore& core, const std::string& city_id,
		const std::string& building_id, std::string* error) {
	Building* building = find_building(ws, city_id, building_id);
	if (building == nullptr) {
		if (error != nullptr) {
			*error = "construcao desconhecida";
		}
		return false;
	}
	return auction_building(ws, core, *building, nullptr, error);
}

void phase(WorldState& ws, SimCore& core, double* bank_money_delta) {	if (!ws.crisis.enabled) {
		return;
	}
	if (bank_money_delta != nullptr) {
		*bank_money_delta = 0.0;
	}
	// 1. Construções (inclui refinarias e fazendas), na ordem do save.
	for (Building& building : ws.buildings) {
		if (building.bankrupt || building.strategic) {
			continue;
		}
		City* city = find_city(ws, building.city);
		if (city == nullptr) {
			continue;
		}
		const std::size_t city_index = static_cast<std::size_t>(
			city - ws.cities.data());
		// Fazenda: reduz a área para a próxima safra (uma vez por episódio).
		if (!building.mothballed
				&& building.recipe < 0 && building.input_product < 0
				&& building.output_product >= 0
				&& !building.farm_area_reduced
				&& building.loss_days
					>= std::max(1, ws.crisis.farm_area_reduction_days)) {
			const auto found = ws.crop_plan_lookup.find(
				city->id + "|" + std::to_string(building.output_product));
			if (found != ws.crop_plan_lookup.end()) {
				const int plan_index = found->second;
				if (plan_index >= 0
						&& plan_index < static_cast<int>(ws.crop_plans.size())) {
					CropPlan& plan = ws.crop_plans[
						static_cast<std::size_t>(plan_index)];
					plan.area_scale = std::clamp(plan.area_scale
						* (1.0 - std::clamp(
							ws.crisis.farm_area_reduction, 0.0, 1.0)),
						0.10, 1.0);
					building.farm_area_reduced = true;
					building.crisis_stage = std::max(
						building.crisis_stage, 1);
					ws.crisis_farm_area_reductions += 1;
				}
			}
		}
		if (building.mothballed) {
			const double maintenance = std::max(0.0, building.opex)
				* std::clamp(ws.crisis.mothball_opex_share, 0.0, 1.0);
			const double paid = debit_owner(ws, *city, building, maintenance);
			if (paid > 0.0) {
				city->service_cash += paid;
				building.paid_opex = paid;
			}
			if (paid + 1e-9 < maintenance) {
				building.mothball_unpaid_days += 1;
			} else {
				building.mothball_unpaid_days = 0;
			}
			const double price = building.output_product >= 0
				&& building.output_product < ws.products
				? std::max(0.0,
					city->markets[building.output_product].price) : 0.0;
			const double unit = std::max(0.0, building.capacity) > 1e-9
				? (std::max(0.0, building.opex)
					+ std::max(0.0, building.jobs)
						* std::max(0.0, building.wage))
					/ std::max(0.0, building.capacity)
				: 0.0;
			if (price > unit * 1.05 && unit > 0.0) {
				building.reopen_signal_days += 1;
			} else {
				building.reopen_signal_days = 0;
			}
			if (building.reopen_signal_days
					>= std::max(1, ws.crisis.reopen_signal_days)
					&& owner_cash(ws, *city, building) + 1e-9
						>= std::max(0.0, building.capacity)
							* std::max(0.0,
								ws.crisis.reopen_cost_per_capacity)) {
				reopen(ws, core, building.city, building.id, nullptr);
			}
			if (building.mothball_unpaid_days
					>= std::max(1, ws.crisis.mothball_grace_days)) {
				auction_building(ws, core, building, bank_money_delta,
					nullptr);
			}
			continue;
		}
		// 2. Demissão (determinística; sem caixa, a parte não paga não demite).
		if (building.loss_days >= std::max(1, ws.crisis.layoff_days)
				&& ws.day - building.last_layoff_day
					>= std::max(1, ws.crisis.layoff_cooldown_days)
				&& building.jobs > 1.0) {
			layoff(ws, core, building.city, building.id,
				ws.crisis.layoff_share, nullptr);
		}
		// 3. Crédito de capital de giro.
		if (building.loss_days >= std::max(1, ws.crisis.credit_days)
				&& ws.day - building.last_credit_day
					>= std::max(1, ws.crisis.credit_days)) {
			const double daily = building_daily_cost(building);
			const double need = std::max(0.0,
				ws.crisis.credit_working_days * daily
				- owner_cash(ws, *city, building));
			if (need > 1e-9) {
				Group* group = owner_group(ws, building);
				double granted = 0.0;
				if (group != nullptr) {
					const double limit = std::max(0.0,
						bank::credit_limit(ws, *group));
					if (limit > 1e-9 && borrow_group(ws, *group,
							std::min(need, limit), &granted, nullptr)) {
						if (bank_money_delta != nullptr) {
							*bank_money_delta += granted;
						}
						building.last_credit_day = ws.day;
						building.crisis_stage = std::max(
							building.crisis_stage, 3);
					}
				} else if (city_credit_limit(ws, city_index) > 1e-9
						&& borrow_city(ws, city->id,
							std::min(need,
								city_credit_limit(ws, city_index)),
							&granted, nullptr)) {
					if (bank_money_delta != nullptr) {
						*bank_money_delta += granted;
					}
					building.last_credit_day = ws.day;
					building.crisis_stage = std::max(
						building.crisis_stage, 3);
				}
			}
		}
		// 4. Liquidação de estoque com desconto.
		if (building.loss_days >= std::max(1, ws.crisis.liquidate_days)
				&& ws.day - building.last_liquidation_day
					>= std::max(1, ws.crisis.credit_days)
				&& liquidate_stock(ws, core, building.city, building.id,
					ws.crisis.liquidation_share, nullptr)) {
			building.crisis_stage = std::max(building.crisis_stage, 4);
		}
		// 5. Hibernação.
		if (building.loss_days >= std::max(1, ws.crisis.mothball_days)) {
			mothball(ws, core, building.city, building.id, nullptr);
			continue;
		}
		// 6. Falência -> leilão (o prédio não some; sem comprador, remove).
		if (building.loss_days >= std::max(1, ws.crisis.bankrupt_days)) {
			auction_building(ws, core, building, bank_money_delta, nullptr);
		}
	}
	// 2. Serviços: crédito de giro agregado quando o caixa do setor está sem
	// fôlego e algum negócio acumulou prejuízo. Mesmo credor/limite da cidade.
	for (std::size_t index = 0; index < ws.service_businesses.size();
			++index) {
		const ServiceBusiness& business = ws.service_businesses[index];
		if (business.bankrupt || business.loss_days
				< std::max(1, ws.crisis.credit_days)) {
			continue;
		}
		City* city = find_city(ws, business.city);
		if (city == nullptr) {
			continue;
		}
		if (city->service_cash >= std::max(0.0, city->service_wage_bill)
				* std::max(1.0, ws.crisis.credit_working_days)) {
			continue;
		}
		if (city->firm_debt + city->firm_credit_arrears > 1e-9) {
			continue;
		}
		const double need = std::max(0.0,
			ws.crisis.credit_working_days
				* (std::max(0.0, city->service_wage_bill)
					+ std::max(0.0, city->service_input_cost))
				- std::max(0.0, city->service_cash));
		const std::size_t city_index = static_cast<std::size_t>(
			city - ws.cities.data());
		const double limit = city_credit_limit(ws, city_index);
		if (need > 1e-9 && limit > 1e-9) {
			double granted = 0.0;
			if (borrow_city(ws, city->id, std::min(need, limit), &granted,
					nullptr) && bank_money_delta != nullptr) {
				*bank_money_delta += granted;
			}
		}
	}
	// 3. Usinas (mesmas regras das construções: corte, demissão, hibernação e
	// leilão; sem estoque de produto para liquidar). D-126 (correção): a
	// necessidade é calculada UMA vez por rede, antes do laço, e só a usina
	// excedente ao pico × (1 + margem de reserva) é candidata.
	const std::vector<char> power_needed = plant_necessity(ws);
	for (std::size_t index = 0; index < ws.power_plants.size(); ++index) {
		react_power(ws, core, ws.power_plants[index],
			index < power_needed.size() && power_needed[index] != 0,
			bank_money_delta);
	}
	// Remove usinas sem comprador no leilão (regra antiga de falência).
	for (auto it = ws.power_plants.begin(); it != ws.power_plants.end();) {
		if (it->bankrupt) {
			it = ws.power_plants.erase(it);
			continue;
		}
		++it;
	}
}

} // namespace crisis

void SimCore::phase_crisis() {
	double bank_money_delta = 0.0;
	crisis::phase(ws_, *this, &bank_money_delta);
	ws_.bank_money_today += bank_money_delta;
}

} // namespace ogs
