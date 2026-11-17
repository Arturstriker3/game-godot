// Módulo de domínio: construção como projeto físico (§15, 2026-11-16).
// Toda obra (fábrica, mina, fazenda, expansão) vira um cronograma de materiais
// + mão de obra do setor de construção, com prazo e dono (state/private/grupo
// F9). O pagamento sai do dono conforme avança; sem material a obra PARA (não
// falha). Ao concluir, o prédio entra em operação (ou a expansão soma
// capacidade). C++ puro (STL via sim_core.h), sem godot_cpp.
#include "sim_core.h"

#include "construction.h"
#include "profiling.h"
#include "sim_constants.h"

#include <algorithm>
#include <cmath>

namespace {
// Segurança numérica: nada de NaN/Inf no estado nem nos outputs.
inline double safe_nonneg(double v) {
	return std::isfinite(v) ? std::max(0.0, v) : 0.0;
}
} // namespace

namespace ogs {

int SimCore::demo_project_index(const std::string& building_id) const {
	for (std::size_t index = 0; index < ws_.construction_projects.size(); ++index) {
		if (ws_.construction_projects[index].building_id == building_id) {
			return static_cast<int>(index);
		}
	}
	return -1;
}

void SimCore::demo_mark_project_strategic(const std::string& building_id) {
	for (DemoConstructionProject& project : ws_.construction_projects) {
		if (project.building_id == building_id) {
			project.strategic = true;
			return;
		}
	}
}

// Calcula materiais (receita de obra), prazo e parcela de mão de obra, e
// empilha o projeto. A ordem dos materiais segue a tabela de
// construction.h; bens ausentes do catálogo são pulados (não bloqueiam).
void SimCore::demo_add_construction_project(DemoConstructionProject& project) {
	const bool factory = project.recipe >= 0;
	const construction::MaterialSpec* specs = nullptr;
	const int spec_count = factory ? construction::factory_materials(specs)
		: construction::base_materials(specs);

	project.material_count = 0;
	for (int slot = 0; slot < spec_count; ++slot) {
		const int product = demo_product_index(specs[slot].good);
		if (product < 0) {
			continue;  // bem ausente do catálogo: a obra não exige
		}
		const double base = demo_base_price(product);
		if (base <= 1e-9) {
			continue;
		}
		const int m = project.material_count;
		project.materials[m] = product;
		// Quantidade contratada no preço-base (contrato de obra declarado):
		// peso × material_share × total_cost ÷ base_price.
		project.material_qty[m] = specs[slot].weight
			* kDemoConstructionMaterialShare * std::max(0.0, project.total_cost)
			/ base;
		project.material_delivered[m] = 0.0;
		++project.material_count;
	}
	project.labor_cost_total =
		(1.0 - kDemoConstructionMaterialShare) * std::max(0.0, project.total_cost);
	project.duration_days = factory
		? kDemoConstructionFactoryDays : kDemoConstructionPrimaryDays;
	project.days_elapsed = 0;
	project.paid = 0.0;
	project.labor_paid = 0.0;
	project.paused = false;
	project.pause_reason.clear();
	project.created_day = ws_.day;
	ws_.construction_projects.push_back(project);
}

namespace {
// Materializa o prédio de uma obra concluída (ou soma capacidade numa expansão).
// O dinheiro já foi pago ao longo da obra; aqui só entra a operação.
void demo_complete_project(WorldState& ws, const DemoConstructionProject& p) {
	if (p.expand_existing) {
		for (DemoBuilding& building : ws.buildings) {
			if (building.id == p.building_id && building.city == p.city) {
				building.capacity += std::max(0.0, p.capacity);
				return;
			}
		}
		// O alvo sumiu (ex.: faliu durante a obra): a expansão se perde. O
		// dinheiro já saiu do dono e foi pago a material/serviço — conservado.
		return;
	}
	DemoBuilding building;
	building.id = p.building_id;
	building.city = p.city;
	building.kind = 0;
	building.resource = p.resource;
	building.owner = p.owner;
	building.input_product = p.input_product;
	building.output_product = p.output_product;
	building.recipe = p.recipe;
	building.capacity = std::max(0.0, p.capacity);
	building.input_per_output = p.input_per_output;
	building.capital_per_capacity = p.capacity > 0.0
		? std::max(1.0, p.total_cost / p.capacity) : kDemoCapitalPerCapacity;
	building.jobs = std::max(0.0, p.jobs);
	building.wage = std::max(0.0, p.wage);
	building.opex = std::max(0.0, p.opex);
	for (int level = 0; level < kDemoEducationLevels; ++level) {
		building.education_mix[level] = p.education_mix[level];
	}
	building.local_share = std::clamp(p.local_share, 0.0, 1.0);
	building.partner_group = p.partner_group;
	building.strategic = p.strategic;
	// Fábrica nova começa em rampa (25% da capacidade): evita o pico de
	// produção/oferta no primeiro dia (mesma regra do build instantâneo).
	if (p.recipe >= 0 || p.input_product >= 0) {
		building.output_scale = 0.25;
		building.last_utilization = 0.25;
	}
	ws.buildings.push_back(building);
}

// Resolve o caixa do dono de uma obra (ponteiro; nullptr = dono desconhecido).
double* demo_project_owner_cash(WorldState& ws, const DemoConstructionProject& p,
		DemoCity& city) {
	if (p.group_id.empty()) {
		return p.owner == "state" ? &city.government_budget : &city.firm_cash;
	}
	for (DemoGroup& group : ws.groups) {
		if (group.id == p.group_id) {
			return &group.cash;
		}
	}
	return nullptr;
}
} // namespace

// Avança as obras do dia, na ordem de criação (determinístico). Por cidade há
// um orçamento diário de obra (trabalhadores do setor de construção ×
// produtividade); cada projeto consome materiais do mercado local, paga o dono
// conforme avança e conclui quando o prazo fecha.
void SimCore::demo_tick_construction() {
	if (ws_.construction_projects.empty()) {
		return;
	}

	// Capacidade diária por cidade (XGS/dia): sector_workers[1] é
	// indústria/mineração/construção (F9), o espelho mais próximo do setor de
	// construção. Piso declarado evita travar cidade só-agrícola ou no 1º dia
	// (sector_workers ainda 0).
	std::vector<double> capacity(ws_.cities.size(), 0.0);
	for (std::size_t index = 0; index < ws_.cities.size(); ++index) {
		const double workers = std::max(kDemoConstructionWorkerFloor,
			ws_.cities[index].sector_workers[1]);
		capacity[index] = workers * kDemoConstructionValuePerWorkerDay;
	}

	std::vector<std::size_t> completed;
	for (std::size_t index = 0; index < ws_.construction_projects.size(); ++index) {
		DemoConstructionProject& project = ws_.construction_projects[index];
		project.paused = false;
		project.pause_reason.clear();

		DemoCity* city = find_demo_city(project.city);
		if (city == nullptr) {
			// Cidade sumiu (não deveria): descarta a obra sem pagar.
			completed.push_back(index);
			continue;
		}
		const std::size_t city_index =
			static_cast<std::size_t>(city - ws_.cities.data());

		// Necessidades diárias de material (taxa constante: completa no prazo).
		double day_material_cost = 0.0;
		double need[kDemoMaxConstructionMaterials] = {};
		bool short_material = false;
		for (int m = 0; m < project.material_count; ++m) {
			const int product = project.materials[m];
			if (product < 0 || product >= ws_.products) {
				continue;
			}
			need[m] = project.material_qty[m]
				/ std::max(1, project.duration_days);
			DemoMarket& market = city->markets[product];
			if (market.stock + 1e-9 < need[m]) {
				short_material = true;
				project.pause_reason = std::string("sem material: ")
					+ demo_product_name(product);
				break;
			}
			day_material_cost += need[m]
				* std::max(1e-9, market.price);
		}
		if (short_material) {
			project.paused = true;
			continue;
		}
		// Parcela de mão de obra (serviço de construção) do dia.
		const double day_labor_cost = project.labor_cost_total
			/ std::max(1, project.duration_days);
		const double total_out = day_material_cost + day_labor_cost;

		// Capacidade do setor de construção da cidade.
		if (city_index < capacity.size()
				&& capacity[city_index] + 1e-9 < total_out) {
			project.paused = true;
			project.pause_reason = "sem capacidade de construção";
			continue;
		}

		// Dinheiro do dono: paga só o que tem; sem dinheiro a obra para.
		double* owner_cash = demo_project_owner_cash(ws_, project, *city);
		if (owner_cash == nullptr || *owner_cash + 1e-9 < total_out) {
			project.paused = true;
			project.pause_reason = "dono sem dinheiro";
			continue;
		}

		// Avança: debita o dono, compra materiais do mercado e paga o serviço
		// de construção. Dinheiro conservado: dono -total_out, firma
		// +material, serviço +mão de obra.
		*owner_cash -= total_out;
		for (int m = 0; m < project.material_count; ++m) {
			const int product = project.materials[m];
			if (product < 0 || product >= ws_.products) {
				continue;
			}
			DemoMarket& market = city->markets[product];
			const double bought = need[m];
			const double paid = bought * std::max(1e-9, market.price);
			market.stock -= bought;
			market.demand += bought;
			market.sold += bought;
			market.desired += bought;
			city->firm_cash += paid;
			city->revenue_by_product[product] += paid;
			project.material_delivered[m] += bought;
		}
		city->service_cash += day_labor_cost;
		city->construction_revenue_pending += total_out;
		project.paid += total_out;
		project.labor_paid += day_labor_cost;
		project.days_elapsed += 1;
		if (city_index < capacity.size()) {
			capacity[city_index] -= total_out;
		}

		if (project.days_elapsed >= project.duration_days) {
			demo_complete_project(ws_, project);
			completed.push_back(index);
		}
	}

	// Remove as obras concluídas (ordem inversa para não invalidar índices).
	for (auto it = completed.rbegin(); it != completed.rend(); ++it) {
		ws_.construction_projects.erase(
			ws_.construction_projects.begin()
			+ static_cast<std::ptrdiff_t>(*it));
	}
}

} // namespace ogs
