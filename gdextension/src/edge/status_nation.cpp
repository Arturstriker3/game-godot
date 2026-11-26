#include "status_nation.h"

#include "economy_sim.h"

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_float64_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace godot {

using namespace ogs;

void EconomySim::capture_nation_labor() {
	// S17/A4: o agregado `nations[].labor` Ã© semanal. Capturado no fecho do dia
	// `day % 7 == 0` como doubles puros, para um `get_status()` sob demanda em
	// qualquer dia publicar exatamente o que a publicaÃ§Ã£o diÃ¡ria antiga
	// publicaria (o retrato semanal mais recente). A simulaÃ§Ã£o nÃ£o muda.
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
	// NaÃ§Ãµes agregadas (telas de polÃ­tica/economia), na ordem de apariÃ§Ã£o.
	// Uma Ãºnica passada sobre as cidades (antes: naÃ§Ã£o Ã— cidade Ã— construÃ§Ã£o).
	struct NationTotals {
		double population = 0.0;
		double production = 0.0;
		double stockpile = 0.0;
		double need = 0.0;
		double deficit = 0.0;
		double in_transit = 0.0;
		int city_count = 0;
		// F7: mercado de trabalho agregado (soma das cidades da naÃ§Ã£o).
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
		// F7 calibraÃ§Ã£o: fatia da POP da naÃ§Ã£o por nÃ­vel de consumo
		// (0 = nenhum, 1 = basic, 2 = life, 3 = luxury).
		double income_share[kIncomeLevels] = {};
		double income_population = 0.0;
		// F7 calibraÃ§Ã£o: TFP nacional ponderada pela populaÃ§Ã£o de simulaÃ§Ã£o.
		double productivity_weighted = 0.0;
		// B6: patrimÃ´nio por quintil somado nas cidades + regiÃµes da naÃ§Ã£o
		// (poupanÃ§a + imÃ³vel + terra + participaÃ§Ãµes) â€” base do Gini de riqueza.
		double wealth[kIncomeStrata] = {};
	};
	// S17: cadÃªncia do agregado `labor` (semanal), POR SLOT como sempre foi.
	// A4: um slot parado (o jogo/agendador nÃ£o constrÃ³i status todo dia) Ã©
	// semeado pelo retrato semanal em vez de publicar valores de semanas atrÃ¡s
	// â€” Ã© o valor que a publicaÃ§Ã£o diÃ¡ria teria no Ãºltimo mÃºltiplo de 7.
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
		// B6: riqueza nacional = urbana + regiÃ£o (patrimÃ´nio por quintil).
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
		// B6: Gini de riqueza da naÃ§Ã£o (poupanÃ§a + imÃ³vel + terra + aÃ§Ãµes).
		nation[k.wealth_gini] = ogs::SimCore::quintile_gini(
			totals.wealth, kIncomeStrata);
		// F4: frota/combustÃ­vel da naÃ§Ã£o (a UI e a IA leem daqui; a fila do dia
		// Ã© o sinal de compra de veÃ­culos/combustÃ­vel). O sub-dicionÃ¡rio e o
		// Array de saÃºde sÃ£o reaproveitados entre ticks (byte-idÃªntico: mesmos
		// valores, sÃ³ sem realocar os contÃªineres).
		const auto fleet_found = ws_.fleets.find(tag);
		if (fleet_found != ws_.fleets.end()) {
			const Fleet& fleet = fleet_found->second;
			Dictionary& fleet_dict = slot.fleet;
			fleet_dict[k.trucks] = fleet.trucks;
			fleet_dict[k.ships] = fleet.ships;
			fleet_dict[k.planes] = fleet.planes;
			fleet_dict[k.fuel] = fleet.diesel + fleet.jet_fuel;
			fleet_dict[k.diesel] = fleet.diesel;
			fleet_dict[k.diesel_target] = fleet.diesel_target;
			fleet_dict[k.jet_fuel] = fleet.jet_fuel;
			fleet_dict[k.jet_target] = fleet.jet_target;
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
			fleet_dict[k.diesel_used] = fleet.diesel_used_today;
			fleet_dict[k.jet_used] = fleet.jet_used_today;
			fleet_dict[k.queue] = fleet.queue_today;
			fleet_dict[k.missed_no_fleet] = fleet.missed_no_fleet;
			fleet_dict[k.missed_no_fuel] = fleet.missed_no_fuel;
			fleet_dict[k.source] = fleet.derived ? "derived" : "declared";
			nation[k.fleet] = fleet_dict;
		}
		// F7: mercado de trabalho da naÃ§Ã£o (telas de polÃ­tica/trabalho). O
		// sub-dicionÃ¡rio e os Arrays de nÃ­veis sÃ£o reaproveitados entre ticks.
		// S17: cadÃªncia semanal POR SLOT (como sempre foi). A4: o primeiro
		// build de um slot frio â€” jogo/agendador nÃ£o constroem status todo dia â€”
		// Ã© semeado pelo retrato semanal, o mesmo valor que a publicaÃ§Ã£o diÃ¡ria
		// teria no Ãºltimo mÃºltiplo de 7.
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

} // namespace godot
