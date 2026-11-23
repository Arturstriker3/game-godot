// D-125 — EFICIÊNCIA DE PRODUÇÃO (estilo HOI4). Ver
// docs/economy/D125_efficiency.md e
// data/definitions/production/efficiency.json.
//
// Regra: nada produz no máximo de imediato. Construção nova (ou que troca de
// produto) começa baixa e cresce com os dias de operação. A eficiência
// multiplica a PRODUÇÃO — folha/opex continuam cheios (a planta emprega a
// equipe enquanto aprende). O `phase_efficiency` roda no início do dia, antes
// da agricultura e da produção, e é determinístico (ordem de `ws_.buildings`).
#include "efficiency.h"

#include "agriculture.h"
#include "sim_core.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace ogs {
namespace eff {
namespace {

// Teto/base da curva (protege divisões por zero de um JSON mal editado).
double safe_days(double value, double fallback) {
	return value > 1e-9 ? value : fallback;
}

// Data civil do dia (mesmo calendário da agricultura: dia 0 = 1/1/2027).
GameDate today_of(const WorldState& ws) {
	return game_date_from_day(ws.day);
}

// Próximo dia de plantio (delta em dias) do par (cidade, produto). -1 quando
// o par não tem plano sazonal declarado (ausência honesta).
int next_plant_delta(const WorldState& ws, std::size_t city_index, int product) {
	if (product < 0 || product >= ws.products) {
		return -1;
	}
	const GameDate today = today_of(ws);
	const int year_days = std::max(1, game_year_days(today.year));
	int best = -1;
	for (const CropPlan& plan : ws.crop_plans) {
		if (plan.city_index != static_cast<int>(city_index)
				|| plan.product != product) {
			continue;
		}
		for (int index = 0; index < plan.season_count; ++index) {
			const int start = plan.seasons[index].plant_start_doy;
			if (start <= 0) {
				continue;
			}
			int delta = start - today.doy;
			if (delta < 0) {
				delta += year_days;
			}
			if (best < 0 || delta < best) {
				best = delta;
			}
		}
	}
	return best;
}

} // namespace

EfficiencyClass classify(const WorldState& ws, const City& city,
		const Building& building) {
	if (is_refinery(ws, building)) {
		return EfficiencyClass::kRefinery;
	}
	if (building.recipe >= 0 || building.input_product >= 0) {
		return EfficiencyClass::kFactory;
	}
	// Extração primária: só é mina onde existe jazida declarada (a mesma
	// regra do estágio D-086); o resto é lavoura/pecuária.
	if (building.output_product >= 0 && building.output_product < kMaxProducts
			&& city.deposits[building.output_product].tracked) {
		return EfficiencyClass::kMine;
	}
	if (is_ranch(ws, building)) {
		return EfficiencyClass::kRanch;
	}
	return EfficiencyClass::kFarm;
}

const EfficiencyCurve& curve(const WorldState& ws, EfficiencyClass cls) {
	switch (cls) {
	case EfficiencyClass::kRefinery:
		return ws.efficiency.refinery;
	case EfficiencyClass::kMine:
		return ws.efficiency.mine;
	case EfficiencyClass::kFarm:
		return ws.efficiency.farm;
	case EfficiencyClass::kRanch:
		return ws.efficiency.ranch;
	case EfficiencyClass::kFactory:
	default:
		return ws.efficiency.factory;
	}
}

double cap(const WorldState& ws, const Building& building, EfficiencyClass cls) {
	// Indústria/refino maturam até 100% sem equipamento. Lavoura/pecuária/mina
	// dependem do trator/máquina para chegar ao teto: sem ele, `cap_without`.
	if (cls == EfficiencyClass::kFactory || cls == EfficiencyClass::kRefinery) {
		return 1.0;
	}
	const bool powered = building.equipment > 0.0
		&& building.equipment_active;
	return powered ? 1.0
		: std::clamp(ws.efficiency.equipment_cap_without, 0.0, 1.0);
}

double step(const WorldState& ws, const Building& building,
		EfficiencyClass cls, bool equipment_active) {
	const EfficiencyCurve& c = curve(ws, cls);
	const double start = std::clamp(c.start, 0.0, 1.0);
	const double mid = std::clamp(c.mid, start, 1.0);
	const double mid_days = safe_days(c.mid_days, 1.0);
	const double full_days = safe_days(c.full_days, mid_days + 1.0);
	const double phase_one = (mid - start) / mid_days;
	const double phase_two = (1.0 - mid) / std::max(1e-9, full_days - mid_days);
	double rate = building.production_efficiency < mid ? phase_one : phase_two;
	if (building.equipment > 0.0 && equipment_active) {
		rate *= std::max(1.0, ws.efficiency.equipment_rate_boost);
	}
	return std::max(0.0, rate);
}

bool is_refinery(const WorldState& ws, const Building& building) {
	if (building.recipe < 0
			|| building.recipe >= static_cast<int>(ws.recipes.size())) {
		return false;
	}
	// A spec liga o refino à receita de gasolina (F0/D-123 `gasoline_a`).
	return ws.recipes[static_cast<std::size_t>(building.recipe)].id.rfind(
		"gasoline_", 0) == 0;
}

bool is_ranch(const WorldState& ws, const Building& building) {
	if (building.output_product < 0
			|| building.output_product >= ws.products) {
		return false;
	}
	const std::string& name = ws.product_names[
		static_cast<std::size_t>(building.output_product)];
	// A pecuária é declarada como produto de ciclo lento; hoje só a carne.
	return name == "meat";
}

void mature(Building& building, bool with_equipment) {
	building.production_efficiency = 1.0;
	building.efficiency_days = 9999;
	building.transition_days = 0;
	building.idle_today = false;
	building.equipment = with_equipment ? 1.0 : 0.0;
	building.equipment_active = with_equipment;
}

void init_new(WorldState& ws, const City& city, Building& building) {
	const EfficiencyClass cls = classify(ws, city, building);
	building.production_efficiency =
		std::clamp(curve(ws, cls).start, 0.0, 1.0);
	building.efficiency_days = 0;
	building.transition_days = 0;
	building.idle_today = false;
	building.equipment = 0.0;
	building.equipment_active = false;
	building.refinery_mode = 0;
}

int transition_days(const WorldState& ws, const City& city,
		const Building& building, int product) {
	const EfficiencyClass cls = classify(ws, city, building);
	switch (cls) {
	case EfficiencyClass::kRefinery:
		return std::max(0, ws.efficiency.transition_refinery_days);
	case EfficiencyClass::kFactory: {
		const std::string& sector = product >= 0 && product < ws.products
			? ws.product_sectors[static_cast<std::size_t>(product)] : std::string();
		if (sector == "military") {
			return std::max(0, ws.efficiency.transition_military_days);
		}
		if (sector == "electronics") {
			return std::max(0, ws.efficiency.transition_semiconductor_days);
		}
		return std::max(0, ws.efficiency.transition_factory_days);
	}
	case EfficiencyClass::kFarm:
	case EfficiencyClass::kRanch: {
		const auto found = ws.city_lookup.find(building.city);
		if (found == ws.city_lookup.end()) {
			return -1;
		}
		return next_plant_delta(ws, static_cast<std::size_t>(found->second),
			product);
	}
	case EfficiencyClass::kMine:
	default:
		return -1;  // mina não troca de produto: a jazida manda
	}
}

bool same_family(const WorldState& ws, int left, int right) {
	if (left < 0 || right < 0 || left >= ws.products || right >= ws.products) {
		return false;
	}
	if (static_cast<std::size_t>(left) >= ws.product_sectors.size()
			|| static_cast<std::size_t>(right) >= ws.product_sectors.size()) {
		return false;
	}
	const std::string& a = ws.product_sectors[static_cast<std::size_t>(left)];
	const std::string& b = ws.product_sectors[static_cast<std::size_t>(right)];
	return !a.empty() && a == b;
}

double mode_share(const WorldState& ws, int mode, int product) {
	if (mode < 0 || mode > 2) {
		return 0.0;
	}
	if (product == ws.efficiency.refinery_gasoline) {
		return ws.efficiency.mode_shares[mode][0];
	}
	if (product == ws.efficiency.refinery_diesel) {
		return ws.efficiency.mode_shares[mode][1];
	}
	if (product == ws.efficiency.refinery_jet) {
		return ws.efficiency.mode_shares[mode][2];
	}
	return 0.0;
}

double main_yield(const WorldState& ws, const Building& building,
		const Recipe& recipe) {
	if (!is_refinery(ws, building) || building.refinery_mode <= 0) {
		return recipe.yield;
	}
	const double share = mode_share(ws, building.refinery_mode, recipe.output);
	return share > 0.0 ? share : recipe.yield;
}

double byproduct_yield(const WorldState& ws, const Building& building,
		const Recipe& recipe, int slot) {
	if (slot < 0 || slot >= recipe.byproduct_count) {
		return 0.0;
	}
	if (!is_refinery(ws, building) || building.refinery_mode <= 0) {
		return recipe.byproduct_qty[slot];
	}
	const double share = mode_share(ws, building.refinery_mode,
		recipe.byproducts[slot]);
	return share > 0.0 ? share : recipe.byproduct_qty[slot];
}

double harvest_factor(const WorldState& ws, std::size_t city_index,
		int product) {
	if (city_index >= ws.buildings_by_city.size()) {
		return 1.0;
	}
	double weighted = 0.0;
	double total = 0.0;
	for (const int bi : ws.buildings_by_city[city_index]) {
		const Building& building = ws.buildings[static_cast<std::size_t>(bi)];
		if (building.output_product != product || building.idle_today) {
			continue;
		}
		const City& city = ws.cities[city_index];
		const EfficiencyClass cls = classify(ws, city, building);
		if (cls != EfficiencyClass::kFarm && cls != EfficiencyClass::kRanch) {
			continue;
		}
		const double weight = std::max(0.0, building.capacity);
		weighted += weight * std::clamp(building.production_efficiency, 0.0, 1.0);
		total += weight;
	}
	return total > 0.0 ? weighted / total : 1.0;
}

} // namespace eff

// ---------------------------------------------------------------------------
// Fase diária da eficiência: transição (zero produção), combustível do
// equipamento e maturação. Ordem de `ws_.buildings` = determinística.
// ---------------------------------------------------------------------------
void SimCore::phase_efficiency() {
	const EfficiencyParams& params = ws_.efficiency;
	for (std::size_t city_index = 0; city_index < ws_.cities.size();
			++city_index) {
		City& city = ws_.cities[city_index];
		for (const int bi : ws_.buildings_by_city[city_index]) {
			Building& building = ws_.buildings[static_cast<std::size_t>(bi)];
			building.idle_today = false;
			building.equipment_active = false;
			if (building.transition_days > 0) {
				// Troca de produto/modo: a planta fica parada (sem produção e
				// sem maturação) durante os dias de transição.
				--building.transition_days;
				building.idle_today = true;
				continue;
			}
			const EfficiencyClass cls = eff::classify(ws_, city, building);
			const bool powered_class = cls == EfficiencyClass::kMine
				|| cls == EfficiencyClass::kFarm
				|| cls == EfficiencyClass::kRanch;
			if (powered_class && building.equipment > 0.0) {
				// Trator/máquina ligado: precisa de diesel no mercado da
				// cidade hoje. Sem o bem, o equipamento PARA no dia (não
				// acelera e não consome); a eficiência conquistada não cai.
				const int fuel = params.equipment_fuel_product;
				const double demand = std::max(0.0, building.capacity)
					* std::clamp(building.equipment, 0.0, 1.0)
					* std::max(0.0, params.equipment_fuel_per_capacity_day);
				bool active = true;
				if (fuel >= 0 && fuel < ws_.products && demand > 1e-12) {
					Market& market = city.markets[fuel];
					if (market.stock + 1e-12 < demand) {
						active = false;
					} else {
						const double unit = market.price;
						market.stock = std::max(0.0, market.stock - demand);
						market.demand += demand;
						market.sold += demand;
						city.revenue_by_product[fuel] += demand * unit;
						building.input_value += demand * unit;
					}
				}
				building.equipment_active = active;
			}
			++building.efficiency_days;
			const double rate = eff::step(ws_, building, cls,
				building.equipment_active);
			const double ceiling = eff::cap(ws_, building, cls);
			building.production_efficiency = std::min(ceiling,
				building.production_efficiency + rate);
		}
	}
}

} // namespace ogs
