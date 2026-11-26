// H1f — sim/power/grid.cpp: rede, despacho por mérito, interconexões e a fase
// do dia. Blocos movidos VERBATIM de `sim/power.cpp`; nenhuma regra, ordem ou
// número mudou. O rateio da demanda e a tarifa de varejo continuam dentro de
// `phase_power` porque são o mesmo corpo determinístico que alimenta o
// despacho; a liquidação (pagamentos) vive em retail.cpp.
#include "power_internal.h"

#include "../sim_core.h"
#include "../world_state.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace ogs {

namespace {

// Prédio industrial (receita ou insumo declarado) — é o que sente o
// racionamento na produção e no emprego.
bool is_industrial(const Building& building) {
	return building.recipe >= 0 || building.input_product >= 0;
}

} // namespace

namespace {

constexpr double kPowerPi = 3.14159265358979323846;

// FNV-1a de 32 bits: fase determinística por NAÇÃO (não usa std::hash, que
// não é estável entre compiladores/execuções — o determinismo é contrato).
std::uint32_t power_phase_hash(const std::string& text) {
	std::uint32_t hash = 2166136261u;
	for (const unsigned char byte : text) {
		hash ^= static_cast<std::uint32_t>(byte);
		hash *= 16777619u;
	}
	return hash;
}

} // namespace

namespace power_detail {

int month_of_day(int day) {
	const int doy = ((day % 365) + 365) % 365;
	return std::min(11, (doy * 12) / 365);
}

// Produto de combustível de cada fonte térmica (-1 = sem compra de mercado).
int fuel_product_of(const WorldState& ws, int fuel) {
	const char* good = nullptr;
	// F0: num snapshot v38 e anterior a lista de bens vem do próprio arquivo e o
	// combustível ainda é o `fuel` genérico; o papel antigo é o fallback.
	const char* legacy = nullptr;
	switch (fuel) {
	case POWER_COAL: good = "coal"; break;
	case POWER_GAS: good = "gas"; break;
	// F0: usina a óleo queima diesel refinado (não petróleo cru).
	case POWER_OIL: good = "diesel"; legacy = "fuel"; break;
	case POWER_NUCLEAR: good = "uranium"; break;
	case POWER_BIOMASS: good = "wood"; break;
	default: good = nullptr; break;
	}
	if (good == nullptr) {
		return -1;
	}
	const auto found = ws.product_lookup.find(good);
	if (found != ws.product_lookup.end()) {
		return found->second;
	}
	if (legacy != nullptr) {
		const auto old = ws.product_lookup.find(legacy);
		if (old != ws.product_lookup.end()) {
			return old->second;
		}
	}
	return -1;
}

} // namespace power_detail

using namespace power_detail;

double power_day_factor(const PowerParams& params, int fuel, int day,
		const std::string& nation, bool southern) {
	if (!power_fuel_is_renewable(fuel)) {
		return 1.0;
	}
	const double amp = fuel == POWER_HYDRO
		? params.hydro_season_amp : params.weather_amp[fuel];
	if (!(amp > 0.0)) {
		return 1.0;
	}
	const std::uint32_t hash = power_phase_hash(nation);
	const double doy = static_cast<double>(((day % 365) + 365) % 365);
	double factor = 1.0;
	if (fuel == POWER_HYDRO) {
		// Cheia no fim do inverno/início da primavera do hemisfério (dia 75 no
		// norte, 255 no sul), deslocada em até ±30 dias pelo clima da nação.
		const double base = southern ? 255.0 : 75.0;
		const double offset = static_cast<double>(hash % 61u) - 30.0;
		factor += amp * std::cos(
			2.0 * kPowerPi * (doy - (base + offset)) / 365.0);
	} else {
		// Sazonalidade + "clima" de alta frequência. As duas harmônicas têm
		// período que divide 365 dias, então a média em QUALQUER janela de
		// 365 dias é exatamente 1 e o ano-base calibrado não se move.
		const double phase_days = static_cast<double>(hash % 365u);
		factor += amp * std::cos(
			2.0 * kPowerPi * (doy - phase_days) / 365.0);
		factor += 0.5 * amp * std::sin(
			4.0 * kPowerPi * doy / 365.0 + phase_days * 2.0 * kPowerPi / 365.0);
	}
	return std::clamp(factor, 0.0, 2.0);
}


double power_capacity_credit(const PowerParams& params, int fuel) {
	if (fuel < 0 || fuel >= kPowerFuelCount) {
		return 0.0;
	}
	return std::clamp(params.capacity_credit[fuel], 0.0, 1.0);
}

void after_load(WorldState& ws) {
	// Sem estado de energia semeado (`power_cities` vazio) nada é criado aqui:
	// a ausência é declarada e o PowerService decide o que aplicar. O vetor
	// só é realinhado quando já existe estado (save v35/semeado).
	if (!ws.power_cities.empty()
			&& ws.power_cities.size() != ws.cities.size()) {
		ws.power_cities.resize(ws.cities.size());
	}
	for (PowerPlant& plant : ws.power_plants) {
		if (plant.city_index < 0
				|| plant.city_index >= static_cast<int>(ws.cities.size())) {
			plant.city_index = -1;
		}
	}
	for (auto& row : ws.power_grids) {
		PowerGridState& grid = row.second;
		grid.connected = 0;
		grid.partial = 0;
		grid.isolated = 0;
		grid.exists = false;
	}
	for (std::size_t index = 0; index < ws.power_cities.size()
			&& index < ws.cities.size(); ++index) {
		PowerGridState& grid = ws.power_grids[ws.cities[index].nation];
		grid.nation = ws.cities[index].nation;
		const PowerCityState& state = ws.power_cities[index];
		if (state.connection == POWER_CONNECTED) {
			grid.connected += 1;
			grid.exists = true;
		} else if (state.connection == POWER_PARTIAL) {
			grid.partial += 1;
			grid.exists = true;
		} else {
			grid.isolated += 1;
		}
	}
	ws.power_ready = ws.power_ready && !ws.power_cities.empty();
}

// E5 (D-117) — semeia as interconexões do ano-base. A capacidade do dado é
// escalada pela fração simulada da nação IMPORTADORA (o fluxo do dado é a
// importação dela, como a demanda Ember), do mesmo jeito que o parque.
void seed_interconnects(WorldState& ws,
		const std::vector<InterconnectDef>& defs, bool only_missing) {
	if (only_missing && ws.power_interconnects_ready) {
		return;
	}
	if (defs.empty()) {
		ws.interconnects.clear();
		ws.power_interconnects_ready = true;
		return;
	}
	if (ws.power_grids.empty()) {
		// Sem redes declaradas ainda: não marca pronto (o chamador reaplica
		// depois de `set_power_definitions`).
		return;
	}
	std::map<std::string, double> sim_pop;
	std::map<std::string, double> shown_pop;
	for (const City& city : ws.cities) {
		if (city.population > 0.0 && city.simulation_population > 0.0) {
			sim_pop[city.nation] += city.simulation_population;
			shown_pop[city.nation] += city.population;
		}
	}
	const auto nation_scale = [&sim_pop, &shown_pop](
			const std::string& nation) {
		const auto sim = sim_pop.find(nation);
		const auto shown = shown_pop.find(nation);
		if (sim == sim_pop.end() || shown == shown_pop.end()
				|| shown->second <= 0.0) {
			return 1.0;
		}
		return std::clamp(sim->second / shown->second, 0.0, 1.0);
	};
	ws.interconnects.clear();
	for (const InterconnectDef& def : defs) {
		if (def.a.empty() || def.b.empty() || def.a == def.b
				|| !(def.capacity_mw > 0.0)) {
			continue;
		}
		// Só redes NACIONAIS comerciam (D-116): operador + conectada nos dois
		// lados. Rede parcial/isolada fica de fora da E5.
		const auto grid_a = ws.power_grids.find(def.a);
		const auto grid_b = ws.power_grids.find(def.b);
		if (grid_a == ws.power_grids.end() || grid_b == ws.power_grids.end()
				|| !grid_a->second.exists || !grid_b->second.exists
				|| grid_a->second.connected <= 0
				|| grid_b->second.connected <= 0) {
			continue;
		}
		const std::string& deficit_nation = def.importer.empty()
			? def.a : def.importer;
		InterconnectState line;
		line.a = def.a;
		line.b = def.b;
		line.capacity_mw = def.capacity_mw * nation_scale(deficit_nation);
		line.built_day = ws.day;
		line.initial = true;
		// E7 (D-120): contrato de suprimento do ano-base. A direção é a do
		// dado (`importer`); a quantidade média (MW) é escalada pela fração
		// simulada do importador — a mesma escala da capacidade e do parque —
		// e nunca passa da capacidade da linha. Sem `importer` (par só de
		// capacidade), o contrato fica 0: a linha segue só no mercado de sobra.
		if (!def.importer.empty()
				&& (def.importer == def.a || def.importer == def.b)) {
			line.contract_importer = def.importer;
			line.contract_exporter = def.importer == def.a ? def.b : def.a;
			line.contract_mw = std::min(line.capacity_mw,
				std::max(0.0, def.contract_mw)
				* nation_scale(line.contract_importer));
		}
		ws.interconnects.push_back(line);
	}
	std::stable_sort(ws.interconnects.begin(), ws.interconnects.end(),
		[](const InterconnectState& left, const InterconnectState& right) {
			if (left.a != right.a) {
				return left.a < right.a;
			}
			return left.b < right.b;
		});
	ws.power_interconnects_ready = true;
}

// ---------------------------------------------------------------------------
// SimCore — API de declaração/obra.
// ---------------------------------------------------------------------------
void SimCore::set_power_city(const std::string& city_id, int connection,
		double line_mw, double access_pct) {
	City* city = find_city(city_id);
	if (city == nullptr) {
		return;
	}
	const std::size_t index = static_cast<std::size_t>(city - ws_.cities.data());
	if (ws_.power_cities.size() != ws_.cities.size()) {
		ws_.power_cities.resize(ws_.cities.size());
	}
	PowerCityState& state = ws_.power_cities[index];
	state.has_data = true;
	state.connection = std::clamp(connection, 0, 2);
	state.line_mw = std::max(0.0, line_mw);
	state.access_pct = std::clamp(access_pct, 0.0, 100.0);
	after_load(ws_);
}

bool SimCore::build_power_line(const std::string& city_id, double mw,
		const std::string& group_id, std::string* err) {
	const auto fail = [&err](const char* message) {
		if (err != nullptr) {
			*err = message;
		}
		return false;
	};
	City* city = find_city(city_id);
	if (city == nullptr) {
		return fail("cidade desconhecida");
	}
	if (!(mw > 0.0)) {
		return fail("capacidade inválida");
	}
	if (ws_.power_cities.size() != ws_.cities.size()) {
		ws_.power_cities.resize(ws_.cities.size());
	}
	const std::size_t index = static_cast<std::size_t>(city - ws_.cities.data());
	PowerCityState& state = ws_.power_cities[index];
	for (const ConstructionProject& project : ws_.construction_projects) {
		if (project.kind == CONSTRUCTION_POWER_LINE
				&& project.city == city->id) {
			return fail("obra em andamento");
		}
	}
	const PowerParams& params = ws_.power_params;
	const double cost = mw * params.capex_per_mw_line;
	double* cash = nullptr;
	std::string owner = "state";
	if (group_id.empty()) {
		cash = &city->government_budget;
	} else {
		Group* group = find_group(group_id);
		if (group == nullptr) {
			return fail("grupo desconhecido");
		}
		cash = &group->cash;
		owner = group->origin_nation == city->nation
			? "national:" + group->id
			: "foreign:" + group->id + "@" + group->origin_nation;
	}
	if (*cash < cost) {
		return fail("orçamento insuficiente");
	}
	ConstructionProject project;
	project.id = "PWL-" + city->id + "-" + std::to_string(ws_.day);
	project.building_id = project.id;
	project.city = city->id;
	project.owner = owner;
	project.group_id = group_id;
	project.kind = CONSTRUCTION_POWER_LINE;
	project.power_mw = mw;
	project.output_product = -1;
	project.total_cost = cost;
	project.duration_days = params.build_days_line;
	*cash -= cost;
	add_construction_project(project);
	(void)state;
	return true;
}

void SimCore::set_power_interconnects(
		const std::vector<InterconnectDef>& defs, bool only_missing) {
	seed_interconnects(ws_, defs, only_missing);
}

// E5 (D-117) — obra de interconexão entre duas REDES NACIONAIS. Mesmo padrão
// da linha de cidade (`build_power_line`): paga `capex_per_mw_line` × MW do
// caixa do dono, entra na fila §15 e, ao concluir, soma capacidade ao par.
bool SimCore::build_interconnect(const std::string& nation_a,
		const std::string& nation_b, double mw, const std::string& group_id,
		std::string* err) {
	const auto fail = [&err](const char* message) {
		if (err != nullptr) {
			*err = message;
		}
		return false;
	};
	if (nation_a.empty() || nation_b.empty() || nation_a == nation_b) {
		return fail("nações inválidas");
	}
	if (!(mw > 0.0)) {
		return fail("capacidade inválida");
	}
	const auto grid_a = ws_.power_grids.find(nation_a);
	const auto grid_b = ws_.power_grids.find(nation_b);
	if (grid_a == ws_.power_grids.end() || !grid_a->second.exists
			|| grid_a->second.connected <= 0) {
		return fail("rede nacional ausente: nação A");
	}
	if (grid_b == ws_.power_grids.end() || !grid_b->second.exists
			|| grid_b->second.connected <= 0) {
		return fail("rede nacional ausente: nação B");
	}
	std::string a = nation_a;
	std::string b = nation_b;
	if (b < a) {
		std::swap(a, b);
	}
	for (const ConstructionProject& project : ws_.construction_projects) {
		if (project.kind == CONSTRUCTION_INTERCONNECT
				&& project.power_nation_a == a
				&& project.power_nation_b == b) {
			return fail("obra em andamento");
		}
	}
	// Cidade que assina a obra: a maior CONECTADA da nação A (determinístico:
	// ordem do vetor, empate fica com a primeira).
	City* owner_city = nullptr;
	const std::size_t city_count = std::min(ws_.power_cities.size(),
		ws_.cities.size());
	for (std::size_t index = 0; index < city_count; ++index) {
		City& city = ws_.cities[index];
		if (city.nation != a
				|| ws_.power_cities[index].connection != POWER_CONNECTED) {
			continue;
		}
		if (owner_city == nullptr
				|| city.simulation_population > owner_city->simulation_population) {
			owner_city = &city;
		}
	}
	if (owner_city == nullptr) {
		return fail("cidade conectada ausente na nação A");
	}
	const PowerParams& params = ws_.power_params;
	const double cost = mw * params.capex_per_mw_line;
	double* cash = nullptr;
	std::string owner = "state";
	if (group_id.empty()) {
		cash = &owner_city->government_budget;
	} else {
		Group* group = find_group(group_id);
		if (group == nullptr) {
			return fail("grupo desconhecido");
		}
		cash = &group->cash;
		owner = group->origin_nation == a
			? "national:" + group->id
			: "foreign:" + group->id + "@" + group->origin_nation;
	}
	if (*cash < cost) {
		return fail("orçamento insuficiente");
	}
	ConstructionProject project;
	project.id = "PWI-" + a + "-" + b + "-" + std::to_string(ws_.day)
		+ "-" + std::to_string(ws_.construction_projects.size());
	project.building_id = project.id;
	project.city = owner_city->id;
	project.owner = owner;
	project.group_id = group_id;
	project.kind = CONSTRUCTION_INTERCONNECT;
	project.power_nation_a = a;
	project.power_nation_b = b;
	project.power_mw = mw;
	project.output_product = -1;
	project.total_cost = cost;
	project.duration_days = params.build_days_line;
	*cash -= cost;
	add_construction_project(project);
	return true;
}

// ---------------------------------------------------------------------------
// Fase do dia: demanda, despacho por mérito, preço, racionamento, efeitos.
// ---------------------------------------------------------------------------
void SimCore::phase_power() {
	if (!ws_.power_ready) {
		return;
	}
	WorldState& ws = ws_;
	const PowerParams& params = ws.power_params;
	const std::size_t city_count = ws.cities.size();
	if (ws.power_cities.size() != city_count) {
		ws.power_cities.resize(city_count);
	}
	if (ws.power_cities.empty()) {
		return;
	}
	const int month = month_of_day(ws.day);
	// Sazonalidade (E2/E3): a geracao ANUAL de cada fonte e ancorada na Ember,
	// mas a solar usa o PVOUT mensal. Sem deslocar as fontes despachaveis, o
	// inverno (PVOUT baixo) vira blecaute mesmo com o ano fechando. O fator
	// abaixo mantem a media anual = 1 e cobre o residuo mensal da solar; nao
	// muda VOLL, preco de escassez, racionamento, insatisfacao nem perdas.
	// Nuclear, hidro e vento ficam de fora (baseload/sem serie mensal).
	struct SeasonShape {
		double solar_year = 0.0;
		double dispatch_year = 0.0;
		std::array<double, kPowerMonthCount> solar_month{};
		std::array<double, kPowerMonthCount> factor{};
		bool has_factor = false;
	};
	std::map<std::string, SeasonShape> season;
	for (const PowerPlant& plant : ws.power_plants) {
		if (plant.city_index < 0
				|| plant.city_index >= static_cast<int>(city_count)) {
			continue;
		}
		const std::size_t index = static_cast<std::size_t>(plant.city_index);
		const City& city = ws.cities[index];
		const PowerCityState& state = ws.power_cities[index];
		double cf = std::clamp(params.cf[plant.fuel], 0.0, 1.0);
		if (plant.fuel == POWER_SOLAR) {
			double sum = 0.0;
			int months = 0;
			for (int m = 0; m < kPowerMonthCount; ++m) {
				if (state.solar_month[m] > 0.0) {
					sum += state.solar_month[m];
					months += 1;
				}
			}
			if (months > 0) {
				cf = sum / static_cast<double>(months);
			}
		}
		const double scale = std::clamp(state.cf_scale[plant.fuel], 0.0, 5.0);
		const double annual = std::max(0.0, plant.mw)
			* std::clamp(cf * scale, 0.0, 1.0) * kPowerHoursPerYear;
		SeasonShape& shape = season[city.nation];
		if (plant.fuel == POWER_SOLAR) {
			shape.solar_year += annual;
			for (int m = 0; m < kPowerMonthCount; ++m) {
				shape.solar_month[m] += std::max(0.0, plant.mw)
					* std::clamp(state.solar_month[m], 0.0, 1.0);
			}
		} else if (plant.fuel != POWER_NUCLEAR
				&& power_fuel_is_thermal(plant.fuel)) {
			shape.dispatch_year += annual;
		}
	}
	for (auto& entry : season) {
		SeasonShape& shape = entry.second;
		double solar_sum = 0.0;
		for (int m = 0; m < kPowerMonthCount; ++m) {
			solar_sum += shape.solar_month[m];
		}
		if (shape.dispatch_year <= 0.0 || shape.solar_year <= 0.0
				|| solar_sum <= 0.0) {
			for (int m = 0; m < kPowerMonthCount; ++m) {
				shape.factor[m] = 1.0;
			}
			continue;
		}
		const double ratio = shape.solar_year / shape.dispatch_year;
		for (int m = 0; m < kPowerMonthCount; ++m) {
			const double share = shape.solar_month[m] / solar_sum;
			shape.factor[m] = std::clamp(
				1.0 + ratio * (1.0 - 12.0 * share), 0.0, 5.0);
		}
		shape.has_factor = true;
	}
	struct PlantDay {
		double avail_mw = 0.0;
		double cost = 0.0;
	};
	std::vector<PlantDay> days(ws.power_plants.size());
	for (PowerPlant& plant : ws.power_plants) {
		plant.generated_mwh = 0.0;
		plant.day_avail_mw = 0.0;
		plant.day_cost = 0.0;
	}
	std::vector<std::vector<int>> plants_by_city(city_count);
	for (std::size_t p = 0; p < ws.power_plants.size(); ++p) {
		PowerPlant& plant = ws.power_plants[p];
		if (plant.city_index < 0
				|| plant.city_index >= static_cast<int>(city_count)) {
			continue;
		}
		const std::size_t index = static_cast<std::size_t>(plant.city_index);
		const City& city = ws.cities[index];
		const PowerCityState& state = ws.power_cities[index];
		PlantDay& day = days[p];
		double cf = std::clamp(params.cf[plant.fuel], 0.0, 1.0);
		if (plant.fuel == POWER_SOLAR && state.solar_month[month] > 0.0) {
			cf = std::clamp(state.solar_month[month], 0.0, 1.0);
		}
		// Calibração nacional (D-116): a geração Ember do ano-base define o
		// fator efetivo da fonte naquela nação. O teto 1,0 mantém a física
		// (CF ≤ 1); 0 = a fonte não gerou no ano-base (dado).
		double cf_scale = std::clamp(state.cf_scale[plant.fuel], 0.0, 5.0);
		if (plant.cf_base > 0.0) {
			// D-122: usina NOVA. Rende pelo potencial do SÍTIO (PVOUT/vento/CF
			// hídrico), com teto e penalidade de balanceamento — NÃO usa o
			// cf_scale da Ember, que calibra só o parque do ano-base.
			cf = std::clamp(plant.cf_base, 0.0, 1.0);
			cf_scale = 1.0;
			if (plant.fuel == POWER_SOLAR && state.solar_month[month] > 0.0) {
				// Forma mensal local do PVOUT no mês, já com o teto da fonte.
				cf = std::clamp(
					std::clamp(state.solar_month[month], 0.0, 1.0),
					0.0, std::clamp(params.cf_cap[POWER_SOLAR], 0.0, 1.0))
					* params.balance_penalty[POWER_SOLAR];
			}
		}
		// D-122: variação diária determinística (solar/eólica/hídrica) da usina
		// NOVA. A média em 365 dias é exata 1, então o ano-base calibrado não
		// se move. O parque do ano-base fica na âncora Ember (não há série
		// diária pública de vento/vazão para ele; ver §renováveis do doc).
		if (plant.cf_base > 0.0) {
			const bool southern = city.center.y > kMapHeight * 0.5;
			cf *= power_day_factor(params, plant.fuel, ws.day, city.nation,
				southern);
		}
		day.avail_mw = std::max(0.0, plant.mw)
			* std::clamp(cf * cf_scale, 0.0, 1.0)
			* (plant.mothballed ? 0.0
				: std::clamp(plant.production_efficiency, 0.0, 1.0));
		if (plant.fuel != POWER_SOLAR && plant.fuel != POWER_NUCLEAR
				&& power_fuel_is_thermal(plant.fuel)) {
			const auto found = season.find(city.nation);
			if (found != season.end() && found->second.has_factor) {
				day.avail_mw = std::min(
					day.avail_mw * found->second.factor[month],
					std::max(0.0, plant.mw));
			}
		}
		// D-126: a crise (fim do dia) lê a disponibilidade que o mérito usou
		// hoje para decidir se a usina é necessária ao pico da rede.
		plant.day_avail_mw = day.avail_mw;
		double fuel_cost = 0.0;
		if (power_fuel_is_thermal(plant.fuel)
				&& params.fuel_t_per_mwh[plant.fuel] > 0.0) {
			const int product = fuel_product_of(ws, plant.fuel);
			double price = 0.0;
			bool local = false;
			if (product >= 0 && product < ws.products) {
				price = city.markets[product].price;
				if (price <= 0.0) {
					price = base_price(product);
				}
				local = city.markets[product].stock > 1e-9;
			}
			fuel_cost = params.fuel_t_per_mwh[plant.fuel] * price
				* (local ? 1.0 : params.fuel_import_premium);
		}
		// D-118: o custo marginal é um PREÇO, e o preço da energia vive na
		// unidade salário-dia. O termo de combustível (declarado como custo no
		// mercado de BENS, em XGS) entra convertido pela MESMA âncora, então a
		// ordem de mérito e os custos relativos ficam idênticos aos da regra
		// declarada; a compra do combustível continua paga ao preço de mercado
		// do bem (ver `settle`), e a diferença fica no resultado do setor.
		const double anchor = params.salary_day_xgs > 0.0
			? params.salary_day_xgs : kLaborWageInitial;
		day.cost = anchor * fuel_cost + plant.opex_per_mwh;
		plants_by_city[index].push_back(static_cast<int>(p));
	}

	// Demanda por cidade: âncora Ember do ano-base quando a nação tem o dado
	// (só a curva de renda relativa move a base) ou a curva global declarada;
	// acesso (dado) e perdas T&D entram no rateio e na perda efetiva.
	std::vector<double> demand_mwh(city_count, 0.0);
	for (std::size_t index = 0; index < city_count; ++index) {
		City& city = ws.cities[index];
		PowerCityState& state = ws.power_cities[index];
		state.demand_mwh = 0.0;
		state.served_mwh = 0.0;
		state.price = 0.0;
		if (!state.has_data) {
			// Ausência declarada: não se simula falta sem dado de energia.
			city.power_served_ratio = 1.0;
			city.power_industry_factor = 1.0;
			city.power_service_factor = 1.0;
			city.power_price = 0.0;
			city.power_stress = 0.0;
			city.power_cost_industry = 0.0;
			city.power_cost_per_value = 0.0;
			continue;
		}
		// A economia precifica a população SIMULADA (aglomerações); a exibida é
		// o retrato do mapa. Demanda, contas e custos usam a mesma escala do
		// parque semeado (fração simulada, ver `seed`).
		const double people = city.simulation_population > 0.0
			? city.simulation_population : city.population;
		const double access = std::clamp(state.access_pct / 100.0,
			params.access_floor, 1.0);
		const double income = std::clamp(std::max(0.0, city.external_income_index),
			0.05, params.income_index_cap);
		const double per_capita_kwh = params.kwh_per_capita_day
			* std::pow(income, params.income_elasticity) * access;
		// Perda efetiva: a base T&D do dado reduzida pela infra B5 de energia
		// (piso técnico > 0 — obra nunca zera a perda).
		const double infra = std::clamp(
			static_cast<double>(city.region.infra_power) / kInfraMaxLevel,
			0.0, 1.0);
		const double loss_pct = std::clamp(
			state.loss_pct * (1.0 - params.loss_infra_reduction * infra),
			params.loss_floor_pct, params.loss_max_pct);
		const double loss = std::clamp(loss_pct / 100.0, 0.0, 0.8);
		double mwh = 0.0;
		if (state.demand_base_mwh > 0.0 && state.demand_income0 > 0.0) {
			// Âncora nacional (D-116): a base Ember do ano-base só se move pela
			// razão de renda (renda_t / renda_0)^e. A perda T&D entra RELATIVA
			// à do ano-base (a Ember mede no lado da geração, já com perdas),
			// então obra de infraestrutura continua reduzindo a necessidade.
			const double growth = income / state.demand_income0;
			const double loss0 = std::clamp(
				state.demand_loss0 / 100.0, 0.0, 0.8);
			mwh = state.demand_base_mwh
				* std::pow(std::max(0.0, growth), params.income_elasticity)
				* (1.0 - loss0) / (1.0 - loss);
		} else {
			// Sem Ember para a nação: curva global declarada (a mesma do
			// power_params.json, calibrada no §6.1 do doc da E2/E3).
			mwh = people * per_capita_kwh / 1000.0 / (1.0 - loss);
		}
		state.demand_mwh = std::max(0.0, mwh);
		demand_mwh[index] = state.demand_mwh;
	}

	// Estado de despacho por rede: a rede nacional (operador) compartilha as
	// usinas das cidades conectadas/parciais; a isolada roda só com as suas.
	struct Dispatch {
		double served = 0.0;
		double price = 0.0;
	};
	const auto dispatch_pool = [&days, &ws](const std::vector<int>& pool,
			double demand, Dispatch& out) {
		out.served = 0.0;
		out.price = 0.0;
		for (const int plant_index : pool) {
			if (out.served >= demand) {
				break;
			}
			PlantDay& day = days[static_cast<std::size_t>(plant_index)];
			const double take = std::min(day.avail_mw * 24.0,
				demand - out.served);
			if (take <= 0.0) {
				continue;
			}
			ws.power_plants[static_cast<std::size_t>(plant_index)].generated_mwh
				+= take;
			out.served += take;
			out.price = day.cost;
		}
	};
	const auto sort_pool = [&days, &ws](std::vector<int>& pool) {
		std::stable_sort(pool.begin(), pool.end(),
			[&days, &ws](int a, int b) {
				const PlantDay& left = days[static_cast<std::size_t>(a)];
				const PlantDay& right = days[static_cast<std::size_t>(b)];
				if (left.cost != right.cost) {
					return left.cost < right.cost;
				}
				const PowerPlant& pa = ws.power_plants[static_cast<std::size_t>(a)];
				const PowerPlant& pb = ws.power_plants[static_cast<std::size_t>(b)];
				if (pa.fuel != pb.fuel) {
					return pa.fuel < pb.fuel;
				}
				if (pa.city_index != pb.city_index) {
					return pa.city_index < pb.city_index;
				}
				return pa.id < pb.id;
			});
	};

	// E5 (D-117): estado da rede NACIONAL do dia, usado no comércio com as
	// vizinhas. Só redes nacionais entram (D-116: cidade isolada/rede local
	// não comercia). `pool` é o mérito já ordenado; o excedente é o que sobrou
	// de capacidade sem despacho.
	struct GridTrade {
		bool national = false;
		std::vector<int> connected;
		std::vector<int> partial;
		std::vector<int> pool;
		double connected_demand = 0.0;
		double connected_served = 0.0;
		double domestic_price = 0.0;
		double marginal_price = 0.0;
		// E7 (D-120): meta do contrato de suprimento do dia e o que o
		// exportador realmente gerou para ele (antes da própria demanda).
		double contract_target = 0.0;
		double contract_done = 0.0;
	};
	std::map<std::string, GridTrade> trade_states;

	// E7 (D-120): contratos de suprimento do ano-base por exportador. Só
	// valem entre duas REDES NACIONAIS (mesma regra da E5) e até a
	// capacidade da linha; a quantidade já vem escalada do seed.
	std::map<std::string, double> contract_targets;
	for (const InterconnectState& line : ws.interconnects) {
		if (!(line.contract_mw > 0.0)
				|| line.contract_importer.empty()
				|| line.contract_exporter.empty()) {
			continue;
		}
		const auto imp = ws.power_grids.find(line.contract_importer);
		const auto exp = ws.power_grids.find(line.contract_exporter);
		if (imp == ws.power_grids.end() || exp == ws.power_grids.end()
				|| !imp->second.exists || !exp->second.exists
				|| imp->second.connected <= 0 || exp->second.connected <= 0) {
			continue;
		}
		contract_targets[line.contract_exporter] += line.contract_mw * 24.0;
	}
	// O contrato é uma ENTREGA média, não uma obrigação de gerar energia que
	// o importador não consome: o alvo do dia é limitado ao déficit esperado
	// do importador (demanda − disponibilidade local do próprio parque).
	// Sem esse teto, um contrato dimensionado no fluxo médio anual geraria
	// energia ociosa nos dias de demanda baixa.
	for (const InterconnectState& line : ws.interconnects) {
		if (!(line.contract_mw > 0.0)
				|| line.contract_importer.empty()
				|| line.contract_exporter.empty()) {
			continue;
		}
		const auto imp = ws.power_grids.find(line.contract_importer);
		if (imp == ws.power_grids.end() || !imp->second.exists
				|| imp->second.connected <= 0) {
			continue;
		}
		double imp_demand = 0.0;
		double imp_local = 0.0;
		for (std::size_t index = 0; index < city_count; ++index) {
			if (ws.cities[index].nation != line.contract_importer
					|| !ws.power_cities[index].has_data) {
				continue;
			}
			imp_demand += demand_mwh[index];
			for (const int p : plants_by_city[index]) {
				imp_local += days[static_cast<std::size_t>(p)].avail_mw
					* 24.0;
			}
		}
		const double deficit = std::max(0.0, imp_demand - imp_local);
		double& target = contract_targets[line.contract_exporter];
		const double line_target = line.contract_mw * 24.0;
		target = std::max(0.0, target - line_target
			+ std::min(line_target, deficit));
	}

	// Zera os agregados da rede do dia.
	for (auto& row : ws.power_grids) {
		PowerGridState& grid = row.second;
		grid.demand_mwh = 0.0;
		grid.served_mwh = 0.0;
		grid.price = 0.0;
		grid.retail_margin = 0.0;
		grid.curtailment_mwh = 0.0;
		for (int fuel = 0; fuel < kPowerFuelCount; ++fuel) {
			grid.generation_by_fuel[fuel] = 0.0;
		}
	}

	// Reserva a ordem determinística das nações (mapa ordenado).
	for (auto& row : ws.power_grids) {
		PowerGridState& grid = row.second;
		const std::string& nation = row.first;
		std::vector<int> connected;
		std::vector<int> partial;
		std::vector<int> isolated;
		for (std::size_t index = 0; index < city_count; ++index) {
			if (ws.cities[index].nation != nation
					|| !ws.power_cities[index].has_data) {
				continue;
			}
			const int connection = ws.power_cities[index].connection;
			if (connection == POWER_CONNECTED && grid.connected > 0) {
				connected.push_back(static_cast<int>(index));
			} else if (connection == POWER_PARTIAL
					&& (grid.connected > 0 || grid.partial > 0)) {
				partial.push_back(static_cast<int>(index));
			} else {
				isolated.push_back(static_cast<int>(index));
			}
		}
		// Rede nacional de fato: existe operador e ao menos uma conectada.
		const bool national = grid.connected > 0;
		if (!national) {
			isolated.insert(isolated.end(), connected.begin(), connected.end());
			isolated.insert(isolated.end(), partial.begin(), partial.end());
			connected.clear();
			partial.clear();
		}
		// Isoladas: mérito local; preço local.
		for (const int index : isolated) {
			std::vector<int> pool = plants_by_city[static_cast<std::size_t>(index)];
			sort_pool(pool);
			Dispatch out;
			dispatch_pool(pool, demand_mwh[static_cast<std::size_t>(index)], out);
			PowerCityState& state = ws.power_cities[static_cast<std::size_t>(index)];
			state.served_mwh = out.served;
			state.price = std::clamp(out.price * params.price_scale,
				out.served > 0.0 ? params.price_floor : 0.0,
				params.price_ceiling);
			if (out.served < demand_mwh[static_cast<std::size_t>(index)]
					- 1e-9) {
				state.price = params.price_ceiling;
			}
		}
		if (national) {
			// Pool nacional: usinas das conectadas e das parciais (o limite da
			// linha é aplicado na entrega da cidade parcial, simplificação
			// declarada — não se modela fluxo físico entre subestações).
			std::vector<int> pool;
			for (const int index : connected) {
				const std::vector<int>& local =
					plants_by_city[static_cast<std::size_t>(index)];
				pool.insert(pool.end(), local.begin(), local.end());
			}
			for (const int index : partial) {
				const std::vector<int>& local =
					plants_by_city[static_cast<std::size_t>(index)];
				pool.insert(pool.end(), local.begin(), local.end());
			}
			sort_pool(pool);
			double grid_demand = 0.0;
			for (const int index : connected) {
				grid_demand += demand_mwh[static_cast<std::size_t>(index)];
			}
			for (const int index : partial) {
				grid_demand += demand_mwh[static_cast<std::size_t>(index)];
			}
			Dispatch out;
			// E7 (D-120): o contrato de suprimento é despachado ANTES da
			// demanda marginal do exportador; a carga do mérito é demanda +
			// contratos. Se a disponibilidade não cobre as duas, o contrato cai
			// proporcionalmente (regra declarada) e a própria demanda divide a
			// escassez na mesma proporção.
			const auto target = contract_targets.find(nation);
			const double contract_target = target == contract_targets.end()
				? 0.0 : target->second;
			dispatch_pool(pool, grid_demand + contract_target, out);
			const double contract_done = contract_target > 0.0
				? std::min(contract_target,
					out.served * contract_target
						/ std::max(1e-9, grid_demand + contract_target))
				: 0.0;
			const double own_served = std::max(0.0, out.served - contract_done);
			const double price = own_served + 1e-9 < grid_demand
				? params.price_ceiling
				: std::clamp(out.price * params.price_scale,
					out.served > 0.0 ? params.price_floor : 0.0,
					params.price_ceiling);
			grid.price = price;
			// Geração por cidade (para o limite da linha parcial). A parcela
			// exportada no contrato não abastece a cidade: rateio declarado
			// pela fração servida doméstica.
			const double own_ratio = out.served > 1e-9
				? own_served / out.served : 0.0;
			std::vector<double> generation(city_count, 0.0);
			for (std::size_t p = 0; p < ws.power_plants.size(); ++p) {
				const PowerPlant& plant = ws.power_plants[p];
				if (plant.city_index >= 0
						&& plant.city_index < static_cast<int>(city_count)) {
					generation[static_cast<std::size_t>(plant.city_index)]
						+= plant.generated_mwh * own_ratio;
				}
			}
			// Parciais: servido = geração local + importação até a linha.
			double partial_served_total = 0.0;
			for (const int index : partial) {
				PowerCityState& state =
					ws.power_cities[static_cast<std::size_t>(index)];
				const double demand = demand_mwh[static_cast<std::size_t>(index)];
				const double local_gen = generation[static_cast<std::size_t>(index)];
				const double line_mwh = state.line_mw * 24.0;
				const double served = std::min(demand,
					local_gen + line_mwh);
				state.served_mwh = std::max(0.0, served);
				state.price = price;
				partial_served_total += state.served_mwh;
			}
			// Conectadas: rateio do restante proporcional à demanda.
			double connected_demand = 0.0;
			for (const int index : connected) {
				connected_demand += demand_mwh[static_cast<std::size_t>(index)];
			}
			const double remaining = std::max(0.0,
				own_served - partial_served_total);
			for (const int index : connected) {
				PowerCityState& state =
					ws.power_cities[static_cast<std::size_t>(index)];
				const double demand = demand_mwh[static_cast<std::size_t>(index)];
				const double share = connected_demand > 0.0
					? demand / connected_demand : 0.0;
				state.served_mwh = std::min(demand, remaining * share);
				state.price = price;
			}
			// Agregados da rede.
			for (const int index : connected) {
				const PowerCityState& state =
					ws.power_cities[static_cast<std::size_t>(index)];
				grid.demand_mwh += state.demand_mwh;
				grid.served_mwh += state.served_mwh;
			}
			for (const int index : partial) {
				const PowerCityState& state =
					ws.power_cities[static_cast<std::size_t>(index)];
				grid.demand_mwh += state.demand_mwh;
				grid.served_mwh += state.served_mwh;
			}
			for (std::size_t p = 0; p < ws.power_plants.size(); ++p) {
				const PowerPlant& plant = ws.power_plants[p];
				if (plant.city_index < 0
						|| plant.city_index >= static_cast<int>(city_count)) {
					continue;
				}
				if (ws.cities[static_cast<std::size_t>(plant.city_index)].nation
						!= nation) {
					continue;
				}
				grid.generation_by_fuel[plant.fuel] += plant.generated_mwh;
			}
			// E5: estado do dia para o comércio entre redes nacionais.
			GridTrade& trade = trade_states[nation];
			trade.national = true;
			trade.connected = connected;
			trade.partial = partial;
			trade.pool = pool;
			trade.connected_demand = connected_demand;
			trade.connected_served = 0.0;
			for (const int index : connected) {
				trade.connected_served +=
					ws.power_cities[static_cast<std::size_t>(index)].served_mwh;
			}
			trade.domestic_price = price;
			trade.marginal_price = out.served > 0.0
				? std::clamp(out.price * params.price_scale,
					params.price_floor, params.price_ceiling)
				: params.price_floor;
			trade.contract_target = contract_target;
			trade.contract_done = contract_done;
		} else {
			for (const int index : isolated) {
				const PowerCityState& state =
					ws.power_cities[static_cast<std::size_t>(index)];
				grid.demand_mwh += state.demand_mwh;
				grid.served_mwh += state.served_mwh;
			}
			for (std::size_t p = 0; p < ws.power_plants.size(); ++p) {
				const PowerPlant& plant = ws.power_plants[p];
				if (plant.city_index < 0
						|| plant.city_index >= static_cast<int>(city_count)) {
					continue;
				}
				if (ws.cities[static_cast<std::size_t>(plant.city_index)].nation
						!= nation) {
					continue;
				}
				grid.generation_by_fuel[plant.fuel] += plant.generated_mwh;
			}
		}
		// (Os indicadores da rede saem depois do comércio, em um laço próprio,
		// porque a fração servida muda quando entra energia importada.)
	}

	// ------------------------------------------------------------------
	// E5 (D-117) — comércio diário entre redes NACIONAIS vizinhas.
	//
	// Regra: depois do balanço de cada rede, a nação com excedente vende para
	// a vizinha em déficit até a capacidade da linha. O preço de fronteira é o
	// custo marginal do exportador (última usina despachada para a exportação)
	// dividido por (1 - perda): a perda é o piso técnico de transmissão que já
	// existe em `loss_floor_pct` (não se inventa coeficiente novo). O importador
	// paga esse preço pela TARIFA de varejo (E7): o valor importado entra no
	// ledger da rede (`import_value`), forma o custo médio do dia e só vira
	// dinheiro na cobrança da tarifa (`phase_power_settle`, caixa global) —
	// nada é debitado direto do `power_cash`; a soma zero se mantém. Cidade
	// isolada / rede local NÃO comercia (D-116).
	//
	// TODO(relações): guerra/sanção/hostilidade NÃO corta a linha. O mundo não
	// modela diplomacia (`tpl.dossie.relations.note`: "a diplomacia não é
	// simulada"); quando houver a API de relações, o gancho entra aqui.
	const double line_loss = std::clamp(
		params.loss_floor_pct / 100.0, 0.0, 0.5);

	// ------------------------------------------------------------------
	// E7 (D-120) — CONTRATO DE SUPRIMENTO do ano-base (fluxo BRUTO
	// bilateral). O exportador já despachou o contrato junto da própria
	// demanda (bloco nacional acima); aqui a energia é entregue ao importador
	// e o preço do contrato é o CUSTO MÉDIO de atacado do exportador (não o
	// spot marginal do dia), dividido por (1 - perda da linha). O contrato cai
	// proporcionalmente quando o exportador está curto (regra declarada no
	// despacho); o importador recebe só até o próprio déficit.
	//
	// O preço de escassez do atacado, o VOLL, o racionamento e as perdas NÃO
	// mudam: o contrato é uma camada de entrega, não um novo preço de falta.
	for (InterconnectState& line : ws.interconnects) {
		line.contract_flow_mwh = 0.0;
		line.contract_sent_mwh = 0.0;
		line.contract_price = 0.0;
		line.contract_value = 0.0;
		if (!(line.contract_mw > 0.0)
				|| line.contract_importer.empty()
				|| line.contract_exporter.empty()) {
			continue;
		}
		auto imp_it = trade_states.find(line.contract_importer);
		auto exp_it = trade_states.find(line.contract_exporter);
		if (imp_it == trade_states.end() || exp_it == trade_states.end()
				|| !imp_it->second.national || !exp_it->second.national
				|| exp_it->second.contract_target <= 1e-9) {
			continue;
		}
		GridTrade& importer = imp_it->second;
		GridTrade& exporter = exp_it->second;
		const double target_share = line.contract_mw * 24.0
			/ exporter.contract_target;
		double sent = exporter.contract_done * target_share;
		if (sent <= 1e-9) {
			continue;
		}
		double delivered = sent * (1.0 - line_loss);
		double unmet_total = 0.0;
		for (const int index : importer.connected) {
			const PowerCityState& state = ws.power_cities[index];
			unmet_total += std::max(0.0,
				state.demand_mwh - state.served_mwh);
		}
		if (delivered > unmet_total) {
			delivered = unmet_total;
			sent = line_loss < 1.0 ? delivered / (1.0 - line_loss) : 0.0;
		}
		if (sent <= 1e-9 || delivered <= 1e-9) {
			continue;
		}
		const double served_before = importer.connected_served;
		double remaining = delivered;
		for (std::size_t slot = 0; slot < importer.connected.size();
				++slot) {
			const int index = importer.connected[slot];
			PowerCityState& state = ws.power_cities[index];
			const double unmet = std::max(0.0,
				state.demand_mwh - state.served_mwh);
			double give = unmet_total > 0.0
				? delivered * unmet / unmet_total : 0.0;
			if (slot + 1 == importer.connected.size()) {
				give = remaining;
			}
			const double cap = std::max(0.0, std::min(unmet, remaining));
			give = std::clamp(give, 0.0, cap);
			state.served_mwh += give;
			remaining -= give;
		}
		importer.connected_served = 0.0;
		for (const int index : importer.connected) {
			importer.connected_served +=
				ws.power_cities[index].served_mwh;
		}
		const double received = importer.connected_served - served_before;
		if (received <= 1e-9) {
			continue;
		}
		const double received_ratio = delivered > 1e-9
			? received / delivered : 0.0;
		const double sent_actual = sent * received_ratio;
		// Custo médio do atacado do exportador no dia (pondera geração e
		// custo de cada usina; inclui o próprio contrato).
		double gen = 0.0;
		double cost_value = 0.0;
		for (std::size_t p = 0; p < ws.power_plants.size(); ++p) {
			const PowerPlant& plant = ws.power_plants[p];
			if (plant.city_index < 0
					|| plant.city_index >= static_cast<int>(city_count)
					|| plant.generated_mwh <= 0.0) {
				continue;
			}
			if (ws.cities[static_cast<std::size_t>(plant.city_index)].nation
					!= line.contract_exporter) {
				continue;
			}
			gen += plant.generated_mwh;
			cost_value += plant.generated_mwh
				* days[p].cost;
		}
		const double wholesale = gen > 1e-9
			? cost_value / gen : params.price_floor;
		const double border_price = std::clamp(
			wholesale * params.price_scale / (1.0 - line_loss),
			params.price_floor, params.price_ceiling);
		const double value = received * border_price;
		line.contract_flow_mwh = received;
		line.contract_sent_mwh = sent_actual;
		line.contract_price = border_price;
		line.contract_value = value;
		PowerGridState& imp_grid = ws.power_grids[line.contract_importer];
		PowerGridState& exp_grid = ws.power_grids[line.contract_exporter];
		imp_grid.import_mwh += received;
		imp_grid.import_value += value;
		exp_grid.export_mwh += sent_actual;
		exp_grid.export_value += value;
	}

	// ------------------------------------------------------------------
	// E5 (D-117) — comércio diário entre redes NACIONAIS vizinhas.
	//
	// Regra: depois do balanço de cada rede, a nação com excedente vende para
	// a vizinha em déficit até a capacidade da linha. O preço de fronteira é o
	// custo marginal do exportador (última usina despachada para a exportação)
	// dividido por (1 - perda): a perda é o piso técnico de transmissão que já
	// existe em `loss_floor_pct` (não se inventa coeficiente novo). O importador
	// paga esse preço pela TARIFA de varejo (E7): o valor importado entra no
	// ledger da rede (`import_value`), forma o custo médio do dia e só vira
	// dinheiro na cobrança da tarifa (`phase_power_settle`, caixa global) —
	// nada é debitado direto do `power_cash`; a soma zero se mantém. Cidade
	// isolada / rede local NÃO comercia (D-116).
	//
	// TODO(relações): guerra/sanção/hostilidade NÃO corta a linha. O mundo não
	// modela diplomacia (`tpl.dossie.relations.note`: "a diplomacia não é
	// simulada"); quando houver a API de relações, o gancho entra aqui.
	for (InterconnectState& line : ws.interconnects) {
		line.importer_today.clear();
		line.exporter_today.clear();
		line.flow_mwh = 0.0;
		line.sent_mwh = 0.0;
		line.price = 0.0;
		line.value = 0.0;
		auto left = trade_states.find(line.a);
		auto right = trade_states.find(line.b);
		if (left == trade_states.end() || right == trade_states.end()
				|| !left->second.national || !right->second.national) {
			continue;
		}
		const auto deficit_of = [](const GridTrade& grid) {
			return std::max(0.0,
				grid.connected_demand - grid.connected_served);
		};
		GridTrade* importer = nullptr;
		GridTrade* exporter = nullptr;
		const std::string* importer_tag = nullptr;
		const std::string* exporter_tag = nullptr;
		if (deficit_of(left->second) > 1e-6
				&& deficit_of(right->second) <= 1e-6) {
			importer = &left->second;
			importer_tag = &line.a;
			exporter = &right->second;
			exporter_tag = &line.b;
		} else if (deficit_of(right->second) > 1e-6) {
			importer = &right->second;
			importer_tag = &line.b;
			exporter = &left->second;
			exporter_tag = &line.a;
		}
		if (importer == nullptr || exporter == nullptr) {
			continue;
		}
		// Excedente do exportador: capacidade ainda não despachada no mérito.
		double spare_mwh = 0.0;
		for (const int plant_index : exporter->pool) {
			const std::size_t p = static_cast<std::size_t>(plant_index);
			spare_mwh += std::max(0.0, days[p].avail_mw * 24.0
				- ws.power_plants[p].generated_mwh);
		}
		double sent = std::min(
			std::max(0.0, line.capacity_mw) * 24.0, spare_mwh);
		if (sent <= 1e-9) {
			continue;
		}
		double delivered = sent * (1.0 - line_loss);
		const double deficit = deficit_of(*importer);
		if (delivered > deficit) {
			delivered = deficit;
			sent = line_loss < 1.0 ? delivered / (1.0 - line_loss) : 0.0;
		}
		if (sent <= 1e-9 || delivered <= 1e-9) {
			continue;
		}
		// Despacha o extra no mérito do exportador (gera de verdade: o
		// combustível e o O&M entram na liquidação do dia).
		double need = sent;
		double sent_done = 0.0;
		double marginal_cost = 0.0;
		for (const int plant_index : exporter->pool) {
			if (need <= 1e-9) {
				break;
			}
			const std::size_t p = static_cast<std::size_t>(plant_index);
			PowerPlant& plant = ws.power_plants[p];
			const double free_mwh = std::max(0.0,
				days[p].avail_mw * 24.0 - plant.generated_mwh);
			const double take = std::min(free_mwh, need);
			if (take <= 0.0) {
				continue;
			}
			plant.generated_mwh += take;
			need -= take;
			sent_done += take;
			marginal_cost = days[p].cost;
		}
		if (sent_done <= 1e-9) {
			continue;
		}
		delivered = sent_done * (1.0 - line_loss);
		// Preço de fronteira: o exportador recebe o custo marginal pela energia
		// que injeta; a perda da linha é paga pelo importador (÷ (1 - perda)).
		const double border_price = std::clamp(
			marginal_cost * params.price_scale / (1.0 - line_loss),
			params.price_floor, params.price_ceiling);
		const double served_before = importer->connected_served;
		// O importado entra nas CONECTADAS, pro rata do que falta. A cidade
		// parcial não recebe além da própria linha (D-116).
		double unmet_total = 0.0;
		for (const int index : importer->connected) {
			const PowerCityState& state = ws.power_cities[index];
			unmet_total += std::max(0.0,
				state.demand_mwh - state.served_mwh);
		}
		double remaining = delivered;
		for (std::size_t slot = 0; slot < importer->connected.size();
				++slot) {
			const int index = importer->connected[slot];
			PowerCityState& state = ws.power_cities[index];
			const double unmet = std::max(0.0,
				state.demand_mwh - state.served_mwh);
			double give = unmet_total > 0.0
				? delivered * unmet / unmet_total : 0.0;
			if (slot + 1 == importer->connected.size()) {
				give = remaining;
			}
			const double cap = std::max(0.0, std::min(unmet, remaining));
			give = std::clamp(give, 0.0, cap);
			state.served_mwh += give;
			remaining -= give;
		}
		importer->connected_served = 0.0;
		for (const int index : importer->connected) {
			importer->connected_served +=
				ws.power_cities[index].served_mwh;
		}
		const double received = importer->connected_served - served_before;
		if (received <= 1e-9) {
			continue;
		}
		const double value = received * border_price;
		// Preço do importador: se o importado fechou o déficit, a escassez
		// acaba e o preço doméstico volta ao marginal; o importado entra pelo
		// preço de fronteira. Se ainda falta, o preço de escassez continua.
		const double still_short = deficit_of(*importer);
		const double base_price = still_short > 1e-6
			? importer->domestic_price : importer->marginal_price;
		const double total_served = served_before + received;
		double weighted = base_price;
		if (total_served > 1e-9) {
			weighted = (served_before * base_price + value) / total_served;
		}
		weighted = std::clamp(weighted, params.price_floor,
			params.price_ceiling);
		for (const int index : importer->connected) {
			ws.power_cities[index].price = weighted;
		}
		for (const int index : importer->partial) {
			ws.power_cities[index].price = importer->domestic_price;
		}
		PowerGridState& imp_grid = ws.power_grids[*importer_tag];
		PowerGridState& exp_grid = ws.power_grids[*exporter_tag];
		imp_grid.price = weighted;
		imp_grid.import_mwh += received;
		imp_grid.import_value += value;
		exp_grid.export_mwh += sent_done;
		exp_grid.export_value += value;
		line.importer_today = *importer_tag;
		line.exporter_today = *exporter_tag;
		line.flow_mwh = received;
		line.sent_mwh = sent_done;
		line.price = border_price;
		line.value = value;
	}

	// ------------------------------------------------------------------
	// E7 (D-120) — TARIFA DE VAREJO por rede, depois do comércio.
	//
	// Atacado médio = custo ponderado da energia entregue à rede no dia
	// (geração doméstica a custo de mérito + importado ao preço de fronteira),
	// suavizado por média móvel declarada (`retail_scarcity_days`). A tarifa
	// paga pelo consumidor = atacado médio ÷ (1 - parcela de rede/tributos) +
	// repasse de escassez suavizado. O repasse de escassez só incide sobre a
	// fração NÃO servida (déficit pequeno não multiplica a conta inteira) e é
	// suavizado; o spot de escassez continua intacto para VOLL/racionamento.
	const double retail_divisor = std::max(0.1,
		1.0 - std::clamp(params.retail_network_share, 0.0, 0.9));
	const double window = std::max(0.0, params.retail_scarcity_days);
	const auto ema = [window](double previous, double sample) {
		if (window <= 1e-9 || previous <= 0.0) {
			return sample;
		}
		return previous + (sample - previous) / window;
	};
	// O repasse de escassez parte de zero: a média móvel não pode cobrar o
	// pico inteiro no primeiro dia (era o defeito do §8.6). Com janela 0 o
	// repasse é imediato (regra declarada).
	const auto ema_pass = [window](double previous, double sample) {
		if (window <= 1e-9) {
			return sample;
		}
		return previous + (sample - previous) / window;
	};
	std::vector<bool> in_national_grid(city_count, false);
	for (const auto& row : trade_states) {
		const GridTrade& trade = row.second;
		if (!trade.national) {
			continue;
		}
		for (const int index : trade.connected) {
			in_national_grid[static_cast<std::size_t>(index)] = true;
		}
		for (const int index : trade.partial) {
			in_national_grid[static_cast<std::size_t>(index)] = true;
		}
	}
	for (auto& row : ws.power_grids) {
		PowerGridState& grid = row.second;
		const std::string& nation = row.first;
		// O importado do dia já está nas cidades; o agregado da rede ainda é o
		// de antes do comércio (o laço de indicadores abaixo o refaz). A tarifa
		// usa a fração servida FINAL, então recalcula aqui.
		grid.demand_mwh = 0.0;
		grid.served_mwh = 0.0;
		for (std::size_t index = 0; index < city_count; ++index) {
			if (ws.cities[index].nation != nation
					|| !ws.power_cities[index].has_data) {
				continue;
			}
			grid.demand_mwh += ws.power_cities[index].demand_mwh;
			grid.served_mwh += ws.power_cities[index].served_mwh;
		}
		double gen = 0.0;
		double cost_value = 0.0;
		for (std::size_t p = 0; p < ws.power_plants.size(); ++p) {
			const PowerPlant& plant = ws.power_plants[p];
			if (plant.city_index < 0
					|| plant.city_index >= static_cast<int>(city_count)
					|| plant.generated_mwh <= 0.0
					|| ws.cities[static_cast<std::size_t>(plant.city_index)]
						.nation != nation) {
				continue;
			}
			gen += plant.generated_mwh;
			cost_value += plant.generated_mwh * days[p].cost;
		}
		const double import_mwh = std::max(0.0, grid.import_mwh);
		const double import_value = std::max(0.0, grid.import_value);
		const double supply = gen + import_mwh;
		const double daily = supply > 1e-9
			? (cost_value + import_value) / supply
			: std::max(params.price_floor, grid.price);
		grid.average_cost = ema(grid.average_cost, daily);
		const double shortfall = grid.demand_mwh > 1e-9
			? std::clamp((grid.demand_mwh - grid.served_mwh)
				/ grid.demand_mwh, 0.0, 1.0) : 0.0;
		const double premium = std::max(0.0,
			grid.price - grid.average_cost) * shortfall;
		grid.scarcity_pass = ema_pass(grid.scarcity_pass, premium);
		grid.tariff = std::max(0.0,
			grid.average_cost / retail_divisor + grid.scarcity_pass);
	}
	for (std::size_t index = 0; index < city_count; ++index) {
		PowerCityState& state = ws.power_cities[index];
		if (!state.has_data) {
			continue;
		}
		if (in_national_grid[index]) {
			const PowerGridState& grid =
				ws.power_grids[ws.cities[index].nation];
			state.average_cost = grid.average_cost;
			state.tariff = grid.tariff;
			state.scarcity_pass = grid.scarcity_pass;
			continue;
		}
		// Cidade isolada (sem rede nacional): atacado médio do próprio pool.
		double gen = 0.0;
		double cost_value = 0.0;
		for (const int p : plants_by_city[index]) {
			const double generated =
				ws.power_plants[static_cast<std::size_t>(p)].generated_mwh;
			if (generated <= 0.0) {
				continue;
			}
			gen += generated;
			cost_value += generated * days[static_cast<std::size_t>(p)].cost;
		}
		const double daily = gen > 1e-9 ? cost_value / gen
			: std::max(params.price_floor, state.price);
		state.average_cost = ema(state.average_cost, daily);
		const double shortfall = state.demand_mwh > 1e-9
			? std::clamp((state.demand_mwh - state.served_mwh)
				/ state.demand_mwh, 0.0, 1.0) : 0.0;
		const double premium = std::max(0.0,
			state.price - state.average_cost) * shortfall;
		state.scarcity_pass = ema_pass(state.scarcity_pass, premium);
		state.tariff = std::max(0.0,
			state.average_cost / retail_divisor + state.scarcity_pass);
	}

	// Agregados e indicadores da rede DEPOIS do comércio: a fração servida
	// inclui o importado; o racionamento e o VOLL são os mesmos do E3.
	for (auto& row : ws.power_grids) {
		PowerGridState& grid = row.second;
		grid.demand_mwh = 0.0;
		grid.served_mwh = 0.0;
		grid.interconnect_count = 0;
		for (std::size_t index = 0; index < city_count; ++index) {
			if (ws.cities[index].nation != row.first
					|| !ws.power_cities[index].has_data) {
				continue;
			}
			grid.demand_mwh += ws.power_cities[index].demand_mwh;
			grid.served_mwh += ws.power_cities[index].served_mwh;
		}
		for (const InterconnectState& line : ws.interconnects) {
			if (line.a == row.first || line.b == row.first) {
				grid.interconnect_count += 1;
			}
		}
		const double ratio = grid.demand_mwh > 0.0
			? grid.served_mwh / grid.demand_mwh : 1.0;
		if (ratio < params.ration_threshold) {
			grid.ration_days += 1;
			grid.lost_gdp += (grid.demand_mwh - grid.served_mwh)
				* std::max(grid.price, params.price_floor)
				* params.voll_multiple;
		} else {
			grid.ration_days = 0;
		}
	}

	// Consequências por cidade (produção, emprego, serviços, insatisfação).
	for (std::size_t index = 0; index < city_count; ++index) {
		City& city = ws.cities[index];
		PowerCityState& state = ws.power_cities[index];
		if (!state.has_data) {
			continue;
		}
		const double demand = state.demand_mwh;
		const double ratio = demand > 0.0
			? std::clamp(state.served_mwh / demand, 0.0, 1.0) : 1.0;
		state.served_mwh = std::min(state.served_mwh, demand);
		city.power_served_ratio = ratio;
		// E7 (D-120): a economia paga a TARIFA de varejo; o spot (`state.price`)
		// continua publicado como preço de atacado/escassez.
		city.power_price = state.tariff > 0.0 ? state.tariff : state.price;
		// Piso declarado: geração própria/fóssil na cidade segura uma fração
		// da indústria (gerador a diesel) — não é atalho, é a planta local.
		double local_fossil_mw = 0.0;
		for (const int p : plants_by_city[index]) {
			const PowerPlant& plant = ws.power_plants[static_cast<std::size_t>(p)];
			if (power_fuel_is_thermal(plant.fuel)) {
				local_fossil_mw += days[static_cast<std::size_t>(p)].avail_mw;
			}
		}
		const double demand_mw = demand / 24.0;
		double self_floor = 0.0;
		if (demand_mw > 1e-9) {
			self_floor = std::min(params.self_generation_floor,
				local_fossil_mw / demand_mw);
		}
		const double industry = std::clamp(std::max(ratio, self_floor),
			params.industry_floor, 1.0);
		city.power_industry_factor = industry;
		city.power_service_factor = std::clamp(
			params.service_factor_min + (1.0 - params.service_factor_min) * ratio,
			params.service_factor_min, 1.0);
		// Conta da indústria do dia e custo por unidade de valor (piso de
		// custo da formação de preço — Parte E, E3/7).
		const double industry_mwh = state.served_mwh * params.share_industry;
		city.power_cost_industry = industry_mwh
			* (state.tariff > 0.0 ? state.tariff : state.price);
		double value = 0.0;
		for (const int bi : ws.buildings_by_city[index]) {
			const Building& building = ws.buildings[static_cast<std::size_t>(bi)];
			if (is_industrial(building) && building.output_product >= 0
					&& building.output_product < ws.products) {
				value += std::max(0.0, building.capacity)
					* base_price(building.output_product);
			}
		}
		city.power_cost_per_value = value > 1e-9
			? std::min(city.power_cost_industry / value,
				std::max(0.0, params.industry_cost_share_max)) : 0.0;
		// Insatisfação: cresce com a duração do blecaute (dias seguidos).
		if (ratio < params.ration_threshold) {
			state.ration_days += 1;
			state.lost_gdp += (demand - state.served_mwh)
				* std::max(state.price, params.price_floor)
				* params.voll_multiple;
		} else {
			state.ration_days = 0;
		}
		const double duration = static_cast<double>(state.ration_days);
		const double duration_factor = duration
			/ (duration + std::max(1.0, params.stress_duration_days));
		city.power_stress = std::clamp((1.0 - ratio)
			* params.stress_per_unserved * duration_factor, 0.0, 1.0);
	}

	// D-122: CURTILAMENTO. Sem armazenamento, a energia renovável disponível e
	// não despachada é desperdiçada (não abastece ninguém, não entra na
	// geração). Fica no ledger da rede para o status e o relatório.
	for (std::size_t p = 0; p < ws.power_plants.size(); ++p) {
		const PowerPlant& plant = ws.power_plants[p];
		if (!power_fuel_is_renewable(plant.fuel)
				|| plant.city_index < 0
				|| plant.city_index >= static_cast<int>(city_count)) {
			continue;
		}
		const double available = std::max(0.0, days[p].avail_mw) * 24.0;
		const double unused = available - std::max(0.0, plant.generated_mwh);
		if (unused <= 1e-9) {
			continue;
		}
		const std::string& nation = ws.cities[
			static_cast<std::size_t>(plant.city_index)].nation;
		ws.power_grids[nation].curtailment_mwh += unused;
	}

	// IA: onde o racionamento persiste, o Estado abre obra de usina (fila
	// §15). Determinístico: nação em ordem de mapa, fonte mais barata com
	// potencial, cidade conectada de maior população com folga.
	for (auto& row : ws.power_grids) {
		PowerGridState& grid = row.second;
		if (!grid.exists || grid.ration_days < params.build_ration_days) {
			continue;
		}
		if (ws.day - grid.last_build_day < params.build_cooldown_days) {
			continue;
		}
		const std::string& nation = row.first;
		const double demand_mw = grid.demand_mwh / 24.0;
		const double wanted = std::max(5.0, demand_mw * params.build_share_demand);
		int best_fuel = -1;
		std::string best_city;
		double best_mw = 0.0;
		double best_cost = 0.0;
		for (int fuel = 0; fuel < kPowerFuelCount; ++fuel) {
			for (std::size_t index = 0; index < city_count; ++index) {
				const City& city = ws.cities[index];
				if (city.nation != nation) {
					continue;
				}
				const PowerCityState& state = ws.power_cities[index];
				if (state.connection == POWER_ISOLATED || !state.has_data) {
					continue;
				}
				double used = 0.0;
				for (const PowerPlant& plant : ws.power_plants) {
					if (plant.city_index == static_cast<int>(index)
							&& plant.fuel == fuel) {
						used += plant.mw;
					}
				}
				const double headroom = state.cap_mw[fuel] - used;
				if (headroom < wanted * 0.5) {
					continue;
				}
				const double mw = std::min(wanted, headroom);
				// D-122: a IA compara o custo do PRIMEIRO ANO por MW FIRME:
				// capex da obra + combustível declarado ao preço do mercado
				// local (com prêmio de importação quando falta estoque),
				// dividido pelo crédito de capacidade no pico. Renovável é caro
				// de construir e entrega pouco no pico; a térmica é barata de
				// construir mas paga combustível — a troca do dono, explícita.
				double fuel_year = 0.0;
				if (params.fuel_t_per_mwh[fuel] > 0.0) {
					const int product = fuel_product_of(ws, fuel);
					if (product >= 0 && product < ws.products) {
						double price = city.markets[product].price;
						if (price <= 0.0) {
							price = base_price(product);
						}
						const bool local =
							city.markets[product].stock > 1e-9;
						fuel_year = params.fuel_t_per_mwh[fuel] * price
							* (local ? 1.0 : params.fuel_import_premium)
							* 8760.0
							* std::clamp(params.cf[fuel], 0.0, 1.0);
					}
				}
				const double credit = std::max(0.05,
					power_capacity_credit(params, fuel));
				const double cost = mw * (power_new_capex_per_mw(params, fuel)
					+ fuel_year) / credit;
				if (best_fuel < 0 || cost < best_cost) {
					best_fuel = fuel;
					best_city = city.id;
					best_mw = mw;
					best_cost = cost;
				}
			}
		}
		if (best_fuel < 0) {
			continue;
		}
		City* target = find_city(best_city);
		if (target == nullptr || target->government_budget < best_cost) {
			continue;
		}
		if (build_power_plant(best_city, power_fuel_name(best_fuel), best_mw,
				std::string(), nullptr)) {
			grid.last_build_day = ws.day;
		}
	}
}

} // namespace ogs
