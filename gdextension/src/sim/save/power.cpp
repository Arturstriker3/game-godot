// H1g — save/load por domínio: energia (caixa, parque, rede e interconexões).
//
// Blocos movidos VERBATIM de save_state.cpp (mesmas linhas, mesma ordem de
// escrita de chaves). Qualquer reordenação muda os bytes do save.
#include "save_internal.h"

#include "../power.h"
#include "../sim_constants.h"
#include "../sim_core.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace ogs::save {
using namespace detail;

namespace power {

void write(Dictionary& data, const WorldState& ws) {
	// PARTE E (v35/D-116) - ENERGIA: caixa do setor, regra, estado por cidade,
	// usinas e a rede por nação. ADITIVO: v34 e anteriores chegam sem o bloco
	// (ver a carga) e o PowerService semeia o parque do dado público. Um mundo
	// SEM energia semeada não grava o bloco: salvar→carregar→salvar tem de dar
	// o mesmo hash (sem estado "isolado" inventado no load).
	if (ws.power_ready) {
		Dictionary block = make_object();
		block["cash"] = ws.power_cash;
		block["ready"] = ws.power_ready;
		Dictionary policy = make_object();
		std::map<std::string, double> param_map;
		power_params_to_map(ws.power_params, param_map);
		for (const auto& row : param_map) {
			policy[sim_str(row.first.c_str())] = row.second;
		}
		block["policy"] = policy;
		Array power_cities = make_array();
		const std::size_t power_count = std::min(ws.power_cities.size(),
			ws.cities.size());
		for (std::size_t index = 0; index < power_count; ++index) {
			const PowerCityState& state = ws.power_cities[index];
			Dictionary entry = make_object();
			entry["city"] = sim_str(ws.cities[index].id.c_str());
			entry["connection"] = state.connection;
			entry["line_mw"] = state.line_mw;
			entry["has_data"] = state.has_data;
			entry["access_pct"] = state.access_pct;
			entry["loss_pct"] = state.loss_pct;
			Array cap = make_array();
			Array solar = make_array();
			for (int fuel = 0; fuel < kPowerFuelCount; ++fuel) {
				cap.append(state.cap_mw[fuel]);
			}
			for (int month = 0; month < kPowerMonthCount; ++month) {
				solar.append(state.solar_month[month]);
			}
			entry["cap_mw"] = cap;
			entry["solar_month"] = solar;
			// D-122 (v38): vento do sítio para a obra nova render local.
			entry["wind_ms"] = state.wind_ms;
			// Ancoragem nacional (D-116): a base Ember, a renda/perda do ano-base
			// e a calibração do CF por fonte precisam sobreviver ao load; sem
			// eles a demanda voltaria à curva global no dia seguinte.
			entry["demand_base_mwh"] = state.demand_base_mwh;
			entry["demand_income0"] = state.demand_income0;
			entry["demand_loss0"] = state.demand_loss0;
			Array cf_scale = make_array();
			for (int fuel = 0; fuel < kPowerFuelCount; ++fuel) {
				cf_scale.append(state.cf_scale[fuel]);
			}
			entry["cf_scale"] = cf_scale;
			entry["demand_mwh"] = state.demand_mwh;
			entry["served_mwh"] = state.served_mwh;
			entry["price"] = state.price;
			// E7 (v37): atacado médio, tarifa de varejo e repasse de escassez.
			entry["average_cost"] = state.average_cost;
			entry["tariff"] = state.tariff;
			entry["scarcity_pass"] = state.scarcity_pass;
			entry["ration_days"] = state.ration_days;
			entry["lost_gdp"] = state.lost_gdp;
			// Efeitos do dia publicados pela cidade (fator industrial/serviço,
			// insatisfação e conta da indústria): sem eles o round-trip do
			// status divergiria até o próximo tick.
			const City& saved_city = ws.cities[index];
			entry["stress"] = saved_city.power_stress;
			entry["industry_factor"] = saved_city.power_industry_factor;
			entry["service_factor"] = saved_city.power_service_factor;
			entry["served_ratio"] = saved_city.power_served_ratio;
			entry["power_price"] = saved_city.power_price;
			entry["power_cost_industry"] =
				saved_city.power_cost_industry;
			entry["power_cost_per_value"] =
				saved_city.power_cost_per_value;
			power_cities.append(entry);
		}
		block["cities"] = power_cities;
		Array plants = make_array();
		for (const PowerPlant& plant : ws.power_plants) {
			Dictionary entry = make_object();
			entry["id"] = sim_str(plant.id.c_str());
			entry["city_index"] = plant.city_index;
			entry["fuel"] = plant.fuel;
			entry["mw"] = plant.mw;
			entry["owner_group"] = sim_str(plant.owner_group.c_str());
			entry["owner_nation"] = sim_str(plant.owner_nation.c_str());
			entry["capex"] = plant.capex;
			entry["opex_per_mwh"] = plant.opex_per_mwh;
			entry["jobs"] = plant.jobs;
			entry["wage"] = plant.wage;
			entry["built_day"] = plant.built_day;
			entry["initial"] = plant.initial;
			// D-122 (v38): CF de sítio da usina nova (0 = parque do ano-base).
			entry["cf_base"] = plant.cf_base;
			// Geração do dia (MWh): o status publica esse valor até o próximo
			// despacho; o save o preserva como os agregados da rede já fazem.
			entry["generated_mwh"] = plant.generated_mwh;
			// D-126 (v41): crise da usina e eficiência de despacho.
			entry["loss_days"] = plant.loss_days;
			entry["mothballed"] = plant.mothballed;
			entry["mothball_day"] = plant.mothball_day;
			entry["mothball_unpaid_days"] = plant.mothball_unpaid_days;
			entry["crisis_stage"] = plant.crisis_stage;
			entry["last_layoff_day"] = plant.last_layoff_day;
			entry["reopen_signal_days"] = plant.reopen_signal_days;
			entry["production_efficiency"] = plant.production_efficiency;
			plants.append(entry);
		}
		block["plants"] = plants;
		// std::map ordenado: a ordem do array é estável (round-trip byte a byte).
		Dictionary grids = make_object();
		for (const auto& row : ws.power_grids) {
			const PowerGridState& grid = row.second;
			Dictionary entry = make_object();
			entry["nation"] = sim_str(grid.nation.c_str());
			entry["exists"] = grid.exists;
			entry["price"] = grid.price;
			// E7 (v37): atacado médio, tarifa, repasse e margem do dia.
			entry["average_cost"] = grid.average_cost;
			entry["tariff"] = grid.tariff;
			entry["scarcity_pass"] = grid.scarcity_pass;
			entry["retail_margin"] = grid.retail_margin;
			entry["demand_mwh"] = grid.demand_mwh;
			entry["served_mwh"] = grid.served_mwh;
			entry["ration_days"] = grid.ration_days;
			entry["lost_gdp"] = grid.lost_gdp;
			entry["last_build_day"] = grid.last_build_day;
			Array by_fuel = make_array();
			for (int fuel = 0; fuel < kPowerFuelCount; ++fuel) {
				by_fuel.append(grid.generation_by_fuel[fuel]);
			}
			entry["generation_by_fuel"] = by_fuel;
			// E5 (save v36): ledger do comércio do dia.
			entry["import_mwh"] = grid.import_mwh;
			entry["export_mwh"] = grid.export_mwh;
			entry["import_value"] = grid.import_value;
			entry["export_value"] = grid.export_value;
			entry["interconnect_count"] = grid.interconnect_count;
			// D-122 (v38): curtailment do dia (MWh) para o status/round-trip.
			entry["curtailment_mwh"] = grid.curtailment_mwh;
			grids[sim_str(row.first.c_str())] = entry;
		}
		block["grids"] = grids;
		// E5 (save v36): interconexões (ano-base + obras). O vetor já está em
		// ordem determinística; o load remonta o estado e o mesmo round-trip.
		block["interconnects_ready"] = ws.power_interconnects_ready;
		Array interconnects = make_array();
		for (const InterconnectState& line : ws.interconnects) {
			Dictionary entry = make_object();
			entry["a"] = sim_str(line.a.c_str());
			entry["b"] = sim_str(line.b.c_str());
			entry["capacity_mw"] = line.capacity_mw;
			entry["built_day"] = line.built_day;
			entry["initial"] = line.initial;
			// E7 (v37): contrato de suprimento do ano-base.
			entry["contract_importer"] = sim_str(
				line.contract_importer.c_str());
			entry["contract_exporter"] = sim_str(
				line.contract_exporter.c_str());
			entry["contract_mw"] = line.contract_mw;
			entry["contract_flow_mwh"] = line.contract_flow_mwh;
			entry["contract_sent_mwh"] = line.contract_sent_mwh;
			entry["contract_price"] = line.contract_price;
			entry["contract_value"] = line.contract_value;
			entry["importer_today"] = sim_str(line.importer_today.c_str());
			entry["exporter_today"] = sim_str(line.exporter_today.c_str());
			entry["flow_mwh"] = line.flow_mwh;
			entry["sent_mwh"] = line.sent_mwh;
			entry["price"] = line.price;
			entry["value"] = line.value;
			interconnects.append(entry);
		}
		block["interconnects"] = interconnects;
		data["power"] = block;
	}
}

void read(WorldState& ws, const Dictionary& data, int version) {
	// PARTE E (v35/D-116) - ENERGIA. v34 e anteriores chegam SEM o bloco:
	// `power_ready = false`, sem parque e sem rede (ausência declarada); o
	// PowerService aplica o dado público depois do load e semeia o parque.
	// O `power_cities` é paralelo a `cities`: o save guarda o id e a carga
	// remapeia para o índice atual (cidade que sumiu fica sem estado).
	// O `power_cities` é paralelo a `cities` SÓ quando o save tem o bloco
	// (v35+); sem o bloco o vetor fica VAZIO, como num mundo novo — senão
	// salvar→carregar→salvar mudaria o hash do save (estado que nunca foi
	// semeado apareceria como rede "isolada"). v34 e anteriores seguem o
	// caminho: o PowerService semeia o parque depois do load.
	if (data.has("power")) {
		ws.power_cities.assign(ws.cities.size(), PowerCityState());
	} else {
		ws.power_cities.clear();
	}
	ws.power_plants.clear();
	ws.power_grids.clear();
	ws.power_cash = 0.0;
	ws.power_ready = false;
	ws.power_params = PowerParams{};
	// E5: o estado de interconexões é do save; v35 chega sem a chave e o
	// PowerService semeia o ano-base depois do load.
	ws.interconnects.clear();
	ws.power_interconnects_ready = false;
	if (data.has("power")) {
		const Dictionary block = data.get("power", Dictionary());
		ws.power_cash = double(block.get("cash", 0.0));
		ws.power_ready = bool(block.get("ready", false));
		const Value& policy_variant = block.get("policy", Value());
		if (policy_variant.get_type() == Variant::DICTIONARY) {
			const Dictionary policy = policy_variant;
			std::map<std::string, double> param_map;
			for (const auto& row : policy.entries()) {
				param_map[row.first] = double(row.second);
			}
			ws.power_params.apply(param_map);
		}
		std::map<std::string, std::size_t> city_index;
		for (std::size_t index = 0; index < ws.cities.size(); ++index) {
			city_index[ws.cities[index].id] = index;
		}
		const Array saved_cities = block.get("cities", Array());
		for (int index = 0; index < saved_cities.size(); ++index) {
			if (saved_cities[index].get_type() != Variant::DICTIONARY) {
				continue;
			}
			const Dictionary entry = saved_cities[index];
			const std::string city_id = std::string(
				String(entry.get("city", "")).utf8().get_data());
			const auto found = city_index.find(city_id);
			if (found == city_index.end()) {
				continue;
			}
			PowerCityState& state = ws.power_cities[found->second];
			state.connection = std::clamp(int(entry.get("connection", 0)),
				0, static_cast<int>(POWER_CONNECTED));
			state.line_mw = std::max(0.0, double(entry.get("line_mw", 0.0)));
			state.has_data = bool(entry.get("has_data", false));
			state.access_pct = std::clamp(
				double(entry.get("access_pct", 100.0)), 0.0, 100.0);
			state.loss_pct = std::clamp(
				double(entry.get("loss_pct", 10.0)), 0.0, 100.0);
			const Array cap = entry.get("cap_mw", Array());
			for (int fuel = 0; fuel < kPowerFuelCount && fuel < cap.size();
					++fuel) {
				state.cap_mw[fuel] = std::max(0.0, double(cap[fuel]));
			}
			const Array solar = entry.get("solar_month", Array());
			for (int month = 0; month < kPowerMonthCount
					&& month < solar.size(); ++month) {
				state.solar_month[month] = std::clamp(
					double(solar[month]), 0.0, 1.0);
			}
			// D-122 (v38): sem a chave (save antigo) fica 0 = sem vento local.
			state.wind_ms = std::max(0.0,
				double(entry.get("wind_ms", 0.0)));
			// Save v35 antigo (sem âncora): base 0 mantém a curva global e
			// cf_scale fica 1,0 (default do struct). Saves novos preservam.
			state.demand_base_mwh = std::max(0.0,
				double(entry.get("demand_base_mwh", 0.0)));
			state.demand_income0 = std::max(0.0,
				double(entry.get("demand_income0", 0.0)));
			state.demand_loss0 = std::clamp(
				double(entry.get("demand_loss0", 0.0)), 0.0, 100.0);
			const Array cf_scale = entry.get("cf_scale", Array());
			for (int fuel = 0; fuel < kPowerFuelCount
					&& fuel < cf_scale.size(); ++fuel) {
				state.cf_scale[fuel] = std::clamp(
					double(cf_scale[fuel]), 0.0, 5.0);
			}
			state.demand_mwh = std::max(0.0,
				double(entry.get("demand_mwh", 0.0)));
			state.served_mwh = std::max(0.0,
				double(entry.get("served_mwh", 0.0)));
			state.price = std::max(0.0, double(entry.get("price", 0.0)));
			// E7 (v37): save v36 chega sem as chaves; 0 = a tarifa do próximo
			// tick usa o atacado médio derivado, nada inventado para trás.
			state.average_cost = std::max(0.0,
				double(entry.get("average_cost", 0.0)));
			state.tariff = std::max(0.0,
				double(entry.get("tariff", 0.0)));
			state.scarcity_pass = std::max(0.0,
				double(entry.get("scarcity_pass", 0.0)));
			state.ration_days = std::max(0,
				int(entry.get("ration_days", 0)));
			state.lost_gdp = std::max(0.0,
				double(entry.get("lost_gdp", 0.0)));
			City& loaded_city = ws.cities[found->second];
			loaded_city.power_stress = std::clamp(
				double(entry.get("stress", 0.0)), 0.0, 1.0);
			loaded_city.power_industry_factor = std::clamp(
				double(entry.get("industry_factor", 1.0)), 0.0, 1.0);
			loaded_city.power_service_factor = std::clamp(
				double(entry.get("service_factor", 1.0)), 0.0, 1.0);
			loaded_city.power_served_ratio = std::clamp(
				double(entry.get("served_ratio", 1.0)), 0.0, 1.0);
			loaded_city.power_price = std::max(0.0,
				double(entry.get("power_price", 0.0)));
			loaded_city.power_cost_industry = std::max(0.0,
				double(entry.get("power_cost_industry", 0.0)));
			loaded_city.power_cost_per_value = std::max(0.0,
				double(entry.get("power_cost_per_value", 0.0)));
		}
		const Array plants = block.get("plants", Array());
		for (int index = 0; index < plants.size(); ++index) {
			if (plants[index].get_type() != Variant::DICTIONARY) {
				continue;
			}
			const Dictionary entry = plants[index];
			PowerPlant plant;
			plant.id = std::string(
				String(entry.get("id", "")).utf8().get_data());
			if (plant.id.empty()) {
				continue;
			}
			plant.city_index = int(entry.get("city_index", -1));
			plant.fuel = std::clamp(int(entry.get("fuel", 0)), 0,
				kPowerFuelCount - 1);
			plant.mw = std::max(0.0, double(entry.get("mw", 0.0)));
			plant.owner_group = std::string(
				String(entry.get("owner_group", "")).utf8().get_data());
			plant.owner_nation = std::string(
				String(entry.get("owner_nation", "")).utf8().get_data());
			plant.capex = std::max(0.0, double(entry.get("capex", 0.0)));
			plant.opex_per_mwh = std::max(0.0,
				double(entry.get("opex_per_mwh", 0.0)));
			plant.jobs = std::max(0.0, double(entry.get("jobs", 0.0)));
			plant.wage = std::max(0.0, double(entry.get("wage", 0.0)));
			plant.built_day = int(entry.get("built_day", 0));
			plant.initial = bool(entry.get("initial", false));
			// D-122 (v38): sem a chave (save antigo) fica 0 = calibrado pela
			// Ember (a usina antiga continua reproduzindo o ano-base).
			plant.cf_base = std::clamp(
				double(entry.get("cf_base", 0.0)), 0.0, 1.0);
			plant.generated_mwh = std::max(0.0,
				double(entry.get("generated_mwh", 0.0)));
			if (version >= 41) {
				plant.loss_days = std::max(0,
					int(entry.get("loss_days", 0)));
				plant.mothballed = bool(entry.get("mothballed", false));
				plant.mothball_day = int(entry.get("mothball_day", -1));
				plant.mothball_unpaid_days = std::max(0,
					int(entry.get("mothball_unpaid_days", 0)));
				plant.crisis_stage = std::clamp(
					int(entry.get("crisis_stage", 0)), 0, 6);
				plant.last_layoff_day = std::max(-1000000,
					int(entry.get("last_layoff_day", -1000000)));
				plant.reopen_signal_days = std::max(0,
					int(entry.get("reopen_signal_days", 0)));
				plant.production_efficiency = std::clamp(
					double(entry.get("production_efficiency", 1.0)),
					0.0, 1.0);
			}
			ws.power_plants.push_back(std::move(plant));
		}
		const Value& grids_variant = block.get("grids", Value());
		if (grids_variant.get_type() == Variant::DICTIONARY) {
			const Dictionary grids = grids_variant;
			for (const auto& row : grids.entries()) {
				if (row.first.empty()
						|| row.second.get_type() != Variant::DICTIONARY) {
					continue;
				}
				const Dictionary entry = row.second;
				PowerGridState grid;
				grid.nation = row.first;
				grid.exists = bool(entry.get("exists", false));
				grid.price = std::max(0.0,
					double(entry.get("price", 0.0)));
				grid.average_cost = std::max(0.0,
					double(entry.get("average_cost", 0.0)));
				grid.tariff = std::max(0.0,
					double(entry.get("tariff", 0.0)));
				grid.scarcity_pass = std::max(0.0,
					double(entry.get("scarcity_pass", 0.0)));
				grid.retail_margin = double(
					entry.get("retail_margin", 0.0));
				grid.demand_mwh = std::max(0.0,
					double(entry.get("demand_mwh", 0.0)));
				grid.served_mwh = std::max(0.0,
					double(entry.get("served_mwh", 0.0)));
				grid.ration_days = std::max(0,
					int(entry.get("ration_days", 0)));
				grid.lost_gdp = std::max(0.0,
					double(entry.get("lost_gdp", 0.0)));
				grid.last_build_day = int(entry.get("last_build_day", -1000000));
				// E5 (v36): ledger do comércio. Save v35 chega sem as chaves
				// (0 = sem comércio no dia — nada inventado para trás).
				grid.import_mwh = std::max(0.0,
					double(entry.get("import_mwh", 0.0)));
				grid.export_mwh = std::max(0.0,
					double(entry.get("export_mwh", 0.0)));
				grid.import_value = std::max(0.0,
					double(entry.get("import_value", 0.0)));
				grid.export_value = std::max(0.0,
					double(entry.get("export_value", 0.0)));
				grid.interconnect_count = std::max(0,
					int(entry.get("interconnect_count", 0)));
				// D-122 (v38): curtailment do dia; save antigo fica 0.
				grid.curtailment_mwh = std::max(0.0,
					double(entry.get("curtailment_mwh", 0.0)));
				const Array by_fuel = entry.get("generation_by_fuel", Array());
				for (int fuel = 0; fuel < kPowerFuelCount
						&& fuel < by_fuel.size(); ++fuel) {
					grid.generation_by_fuel[fuel] = std::max(0.0,
						double(by_fuel[fuel]));
				}
				ws.power_grids[row.first] = grid;
			}
		}
		// E5 (v36): interconexões. Save v35 chega sem a chave: `ready` fica
		// falso e o PowerService semeia as linhas do ano-base depois do load.
		ws.power_interconnects_ready = bool(
			block.get("interconnects_ready", false));
		const Array links = block.get("interconnects", Array());
		for (int index = 0; index < links.size(); ++index) {
			if (links[index].get_type() != Variant::DICTIONARY) {
				continue;
			}
			const Dictionary entry = links[index];
			InterconnectState line;
			line.a = std::string(
				String(entry.get("a", "")).utf8().get_data());
			line.b = std::string(
				String(entry.get("b", "")).utf8().get_data());
			if (line.a.empty() || line.b.empty() || line.a == line.b) {
				continue;
			}
			if (line.b < line.a) {
				std::swap(line.a, line.b);
			}
			line.capacity_mw = std::max(0.0,
				double(entry.get("capacity_mw", 0.0)));
			line.built_day = int(entry.get("built_day", 0));
			line.initial = bool(entry.get("initial", false));
			line.contract_importer = std::string(
				String(entry.get("contract_importer", "")).utf8().get_data());
			line.contract_exporter = std::string(
				String(entry.get("contract_exporter", "")).utf8().get_data());
			line.contract_mw = std::max(0.0,
				double(entry.get("contract_mw", 0.0)));
			line.contract_flow_mwh = std::max(0.0,
				double(entry.get("contract_flow_mwh", 0.0)));
			line.contract_sent_mwh = std::max(0.0,
				double(entry.get("contract_sent_mwh", 0.0)));
			line.contract_price = std::max(0.0,
				double(entry.get("contract_price", 0.0)));
			line.contract_value = std::max(0.0,
				double(entry.get("contract_value", 0.0)));
			line.importer_today = std::string(
				String(entry.get("importer_today", "")).utf8().get_data());
			line.exporter_today = std::string(
				String(entry.get("exporter_today", "")).utf8().get_data());
			line.flow_mwh = std::max(0.0,
				double(entry.get("flow_mwh", 0.0)));
			line.sent_mwh = std::max(0.0,
				double(entry.get("sent_mwh", 0.0)));
			line.price = std::max(0.0, double(entry.get("price", 0.0)));
			line.value = std::max(0.0, double(entry.get("value", 0.0)));
			ws.interconnects.push_back(line);
		}
	}
}

} // namespace power

} // namespace ogs::save
