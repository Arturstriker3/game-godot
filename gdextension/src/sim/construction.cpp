// Módulo de domínio: construção como projeto físico (§15, 2026-11-16).
// Toda obra (fábrica, mina, fazenda, expansão) vira um cronograma de materiais
// + mão de obra do setor de construção, com prazo e dono (state/private/grupo
// F9). O pagamento sai do dono conforme avança; sem material a obra PARA (não
// falha). Ao concluir, o prédio entra em operação (ou a expansão soma
// capacidade). C++ puro (STL via sim_core.h), sem godot_cpp.
#include "sim_core.h"

#include "construction.h"
#include "housing.h"
#include "profiling.h"
#include "sim_constants.h"
#include "urban_land.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace {
// Segurança numérica: nada de NaN/Inf no estado nem nos outputs.
inline double safe_nonneg(double v) {
	return std::isfinite(v) ? std::max(0.0, v) : 0.0;
}
} // namespace

namespace ogs {

int SimCore::project_index(const std::string& building_id) const {
	for (std::size_t index = 0; index < ws_.construction_projects.size(); ++index) {
		if (ws_.construction_projects[index].building_id == building_id) {
			return static_cast<int>(index);
		}
	}
	return -1;
}

void SimCore::mark_project_strategic(const std::string& building_id) {
	for (ConstructionProject& project : ws_.construction_projects) {
		if (project.building_id == building_id) {
			project.strategic = true;
			return;
		}
	}
}

// Parcela de TERRA de uma obra: o que o dono paga à prefeitura pelo uso do
// solo ocupado (land_price da zona × área). É uma FATIA do `total_cost`, não
// um custo extra. Sem preço de terra na cidade/zona devolve 0 (ausência
// explícita — não se inventa preço).
double SimCore::project_land_cost(
		const ConstructionProject& project) const {
	// URBAN_INFRA: a obra É a abertura do solo (a prefeitura paga a
	// infraestrutura, não o lote) — sem parcela de terra.
	if (project.kind == CONSTRUCTION_URBAN_INFRA
			|| project.urban_action != urban_land::URBAN_NONE) {
		return 0.0;
	}
	const City* city = find_city(project.city);
	if (city == nullptr) {
		return 0.0;
	}
	const std::size_t city_index =
		static_cast<std::size_t>(city - ws_.cities.data());
	if (city_index >= ws_.land.size() || city_index >= ws_.city_gov.size()) {
		// Solo não carregado (cenário sintético/save antigo) ou prefeitura
		// ausente: sem preço e sem destino para a terra — ausência explícita.
		return 0.0;
	}
	const CityLand& land = ws_.land[city_index];
	const double budget = std::max(0.0, project.total_cost);
	double area_m2 = 0.0;
	double price_per_m2 = 0.0;
	if (project.kind == CONSTRUCTION_HOUSING) {
		// Moradia: as unidades entregues ocupam a área do padrão residencial
		// (m² por unidade da densidade/padrão do projeto).
		const int density = std::clamp(
			project.housing_density, 0, DENSITY_COUNT - 1);
		const int tier = std::clamp(project.housing_tier, 0, TIER_COUNT - 1);
		area_m2 = std::max(0.0, project.housing_units)
			* kUrbanUnitM2[density][tier]
			/ std::max(1.0, kUrbanFar[density]);
		price_per_m2 = std::max(0.0, land.land_price[ZONE_RES]);
	} else {
		// Prédio econômico URBANO (receita industrial ou fábrica legada com
		// insumo declarado) e a expansão de um deles: a planta ocupa a zona
		// industrial, m² por vaga. Fazenda/mina (extração primária, solo rural,
		// sem zona) não paga terra.
		const bool urban_factory = project.recipe >= 0
			|| project.input_product >= 0;
		if (!urban_factory) {
			return 0.0;
		}
		area_m2 = std::max(0.0, project.jobs) * kUrbanFactoryM2PerWorker;
		price_per_m2 = std::max(0.0, land.land_price[ZONE_IND]);
	}
	if (area_m2 <= 0.0 || price_per_m2 <= 0.0) {
		return 0.0;
	}
	double cost = area_m2 * price_per_m2;
	if (project.kind == CONSTRUCTION_HOUSING) {
		// Moradia (Q-015, integração): a parcela de terreno é a MESMA que entra
		// no custo da unidade (`build_cost_unit`): lote = m² ÷ FAR, limitado a
		// kHousingLandShareMax do valor. As duas contas precisam bater, ou a
		// terra creditada à prefeitura volta a comer o orçamento inteiro.
		cost = std::min(cost, kHousingLandShareMax * budget);
	} else {
		// Prédio econômico: a terra fica limitada a uma fração do orçamento,
		// para o material + mão de obra ficarem com o resto.
		cost = std::min(cost, kConstructionLandShareMax * budget);
	}
	// A terra é uma fatia: nunca mais que o orçamento do dono.
	cost = std::min(cost, budget);
	return std::isfinite(cost) ? std::max(0.0, cost) : 0.0;
}

// Calcula materiais (receita de obra), prazo e parcela de mão de obra, e
// empilha o projeto. A ordem dos materiais segue a tabela de
// construction.h; bens ausentes do catálogo são pulados (não bloqueiam).
void SimCore::add_construction_project(ConstructionProject& project) {
	const bool factory = project.recipe >= 0;
	const construction::MaterialSpec* specs = nullptr;
	const int spec_count = factory ? construction::factory_materials(specs)
		: construction::base_materials(specs);

	project.material_count = 0;
	// Terra: parcela do orçamento que vai para a prefeitura pelo uso do solo.
	// O dono já debitou o `total_cost` inteiro no ponto de criação; aqui a
	// terra é creditada no caixa municipal e sai do fundo reservado. Dinheiro
	// conservado (prefeitura + reserved).
	project.land_cost = project_land_cost(project);
	project.reserved = std::max(0.0, project.total_cost - project.land_cost);
	{
		City* land_city = find_city(project.city);
		if (land_city != nullptr && project.land_cost > 0.0) {
			const std::size_t land_index =
				static_cast<std::size_t>(land_city - ws_.cities.data());
			if (land_index < ws_.city_gov.size()) {
				ws_.city_gov[land_index].cash += project.land_cost;
			}
		}
	}
	const double build_cost = std::max(0.0, project.total_cost - project.land_cost);
	// Parcela MATERIAL do orçamento: a moradia (§19 W3) consome mais material
	// por unidade que um prédio econômico (kHousingMaterialShare).
	const double material_share = project.kind == CONSTRUCTION_HOUSING
		? kHousingMaterialShare : kConstructionMaterialShare;
	for (int slot = 0; slot < spec_count; ++slot) {
		const int product = product_index(specs[slot].good);
		if (product < 0) {
			continue;  // bem ausente do catálogo: a obra não exige
		}
		const double base = base_price(product);
		if (base <= 1e-9) {
			continue;
		}
		const int m = project.material_count;
		project.materials[m] = product;
		// Quantidade contratada no preço-base (contrato de obra declarado):
		// peso × material_share × build_cost ÷ base_price.
		project.material_qty[m] = specs[slot].weight
			* material_share * build_cost
			/ base;
		project.material_delivered[m] = 0.0;
		++project.material_count;
	}
	project.labor_cost_total =
		(1.0 - material_share) * build_cost;
	if (project.deepen_stage >= 0) {
		// D-086: o aprofundamento tem prazo próprio (90 dias + 30 por estágio
		// aberto), independente da escala de tamanho da obra nova.
		project.duration_days = kMineDeepenBaseDays
			+ kMineDeepenDaysPerStage * project.deepen_stage;
	} else if (project.kind == CONSTRUCTION_HOUSING) {
		// Moradia (§19 W3): prazo declarado, sem escala por tamanho (é a
		// mesma obra de unidades do W3, agora na fila única).
		project.duration_days = std::max(1, kHousingBuildDays);
	} else {
		// Duração por tipo + escala pelo tamanho (§15). Comida (fazenda da cesta) é
		// rápida para não travar a oferta alimentar; mina (primária com jazida) é
		// média; fábrica (receita) é pesada. O tamanho (capacidade em t/dia) escala
		// o prazo com fator logarítmico limitado — estimativa declarada.
		const bool is_food = project.recipe < 0 && project.input_product < 0
			&& food_calories(project.output_product) > 0.0;
		City* project_city = find_city(project.city);
		const bool is_mine = !factory && !is_food && project_city != nullptr
			&& project.output_product >= 0
			&& project_city->deposits[project.output_product].tracked;
		const double base_days = factory
			? static_cast<double>(kConstructionFactoryDays)
			: is_food ? static_cast<double>(kConstructionFoodDays)
			: is_mine ? static_cast<double>(kConstructionMineDays)
			: static_cast<double>(kConstructionPrimaryDays);
		const double size_ratio = std::max(1.0, project.capacity)
			/ kConstructionSizeReference;
		const double size_scale = std::clamp(
			1.0 + kConstructionSizeLogScale * std::log2(size_ratio),
			kConstructionSizeScaleMin, kConstructionSizeScaleMax);
		project.duration_days = std::max(1,
			static_cast<int>(std::llround(base_days * size_scale)));
	}
	project.days_elapsed = 0;
	project.paid = 0.0;
	project.labor_paid = 0.0;
	project.paused = false;
	project.pause_reason.clear();
	project.created_day = ws_.day;
	ws_.construction_projects.push_back(project);
}

namespace {
// §15/D-086 — saldo NÃO gasto do fundo reservado de uma obra que sai da fila.
// O dono debitou o orçamento inteiro na abertura e a obra pagou material/mão de
// obra conforme avançou; o que sobrar (preços de material abaixo do contrato,
// obra que perdeu o alvo) volta ao dono. Sem isso o capital reservado evaporava
// com a remoção do projeto (dinheiro não conservado).
void refund_project_reserved(WorldState& ws, const ConstructionProject& p) {
	const double amount = std::max(0.0, p.reserved);
	if (amount <= 0.0) {
		return;
	}
	City* owner_city = nullptr;
	for (City& city : ws.cities) {
		if (city.id == p.city) {
			owner_city = &city;
			break;
		}
	}
	if (p.owner == "state") {
		if (owner_city != nullptr) {
			owner_city->government_budget += amount;
		} else {
			ws.construction_cash += amount;
		}
		return;
	}
	if (p.owner == "private") {
		if (owner_city != nullptr) {
			owner_city->firm_cash += amount;
		} else {
			ws.construction_cash += amount;
		}
		return;
	}
	// Grupo F9: `national:<g>` / `foreign:<g>@<nação>`.
	std::string group_id = p.owner;
	const std::string national = "national:";
	const std::string foreign = "foreign:";
	if (group_id.rfind(national, 0) == 0) {
		group_id = group_id.substr(national.size());
	} else if (group_id.rfind(foreign, 0) == 0) {
		group_id = group_id.substr(foreign.size());
		const std::size_t at = group_id.find('@');
		if (at != std::string::npos) {
			group_id = group_id.substr(0, at);
		}
	} else {
		group_id.clear();
	}
	for (Group& group : ws.groups) {
		if (!group_id.empty() && group.id == group_id) {
			group.cash += amount;
			return;
		}
	}
	// Dono não localizado: o saldo fica no setor de construção (dinheiro do
	// mundo), nunca desaparece.
	ws.construction_cash += amount;
}

// Materializa o prédio de uma obra concluída (ou soma capacidade numa expansão).
// O dinheiro já foi pago ao longo da obra; aqui só entra a operação.
void complete_project(WorldState& ws, const ConstructionProject& p) {
	// O saldo não gasto do fundo volta ao dono (conservação entre o débito na
	// abertura e a remoção do projeto).
	refund_project_reserved(ws, p);
	// HOUSING (§19 W3 / fatia A2): a obra entrega UNIDADES de moradia ao
	// estoque da cidade e à posse do dono — não materializa prédio.
	if (p.kind == CONSTRUCTION_HOUSING) {
		housing::apply_completed_project(ws, p);
		return;
	}
	// §19 W1: obra de infraestrutura urbana altera o solo (área/densidade/infra)
	// em vez de materializar prédio.
	if (p.kind == CONSTRUCTION_URBAN_INFRA
			|| p.urban_action != urban_land::URBAN_NONE) {
		urban_land::apply_completed_project(ws, p);
		return;
	}
	if (p.expand_existing) {
		for (Building& building : ws.buildings) {
			if (building.id == p.building_id && building.city == p.city) {
				const double old_capacity = std::max(1e-9, building.capacity);
				building.capacity += std::max(0.0, p.capacity);
				// D-086: aprofundamento — abre o estágio novo na MESMA mina, usando
				// a reserva de REFERÊNCIA da jazida (não o saldo restante), e volta
				// a produzir 100% enquanto ela durar. A folha/opex sobem na mesma
				// proporção da capacidade (a mina reaberta volta a operar cheia).
				if (p.deepen_stage >= 0) {
					for (City& city : ws.cities) {
						if (city.id != p.city) {
							continue;
						}
						SimCore::set_mine_stage(city, building, p.deepen_stage);
						const double scale = building.capacity / old_capacity;
						building.jobs *= scale;
						building.opex *= scale;
						building.loss_days = 0;
						break;
					}
				}
				return;
			}
		}
		// O alvo sumiu (ex.: faliu durante a obra): a expansão se perde. O
		// dinheiro já saiu do dono e foi pago a material/serviço — conservado.
		return;
	}
	Building building;
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
		? std::max(1.0, p.total_cost / p.capacity) : kCapitalPerCapacity;
	building.jobs = std::max(0.0, p.jobs);
	building.wage = std::max(0.0, p.wage);
	building.opex = std::max(0.0, p.opex);
	for (int level = 0; level < kEducationLevels; ++level) {
		building.education_mix[level] = p.education_mix[level];
	}
	building.local_share = std::clamp(p.local_share, 0.0, 1.0);
	building.partner_group = p.partner_group;
	building.strategic = p.strategic;
	// Fábrica nova começa em rampa (25% da capacidade): evita o pico de
	// produção/oferta no primeiro dia (mesma regra do build instantâneo). Minas
	// e fazendas entram em 100% (extração primária não tem curva de setup).
	if (p.recipe >= 0 || p.input_product >= 0) {
		building.output_scale = 0.25;
		building.last_utilization = 0.25;
	}
	// Carência de falência (empresa nova real, §15): a planta construída não é
	// avaliada por prejuízo durante os primeiros kRampDays de operação — o
	// capital da obra é investimento, não prejuízo operacional.
	building.ramp_days = kRampDays;
	// D-086: mina concluída abre o estágio 0 (fatia da reserva da jazida).
	for (City& city : ws.cities) {
		if (city.id == building.city) {
			if (SimCore::is_mine_building(city, building)) {
				SimCore::set_mine_stage(city, building, 0);
			}
			break;
		}
	}
	ws.buildings.push_back(building);
}
} // namespace

// Avança as obras do dia, na ordem de criação (determinístico). Por cidade há
// um orçamento diário de obra (trabalhadores do setor de construção ×
// produtividade); cada projeto consome materiais do mercado local, paga o dono
// conforme avança e conclui quando o prazo fecha.
void SimCore::tick_construction() {
	if (ws_.construction_projects.empty()) {
		return;
	}

	// Capacidade diária por cidade (XGS/dia): sector_workers[1] é
	// indústria/mineração/construção (F9), o espelho mais próximo do setor de
	// construção. Piso declarado evita travar cidade só-agrícola ou no 1º dia
	// (sector_workers ainda 0).
	std::vector<double> capacity(ws_.cities.size(), 0.0);
	for (std::size_t index = 0; index < ws_.cities.size(); ++index) {
		const double workers = std::max(kConstructionWorkerFloor,
			ws_.cities[index].sector_workers[1]);
		capacity[index] = workers * kConstructionValuePerWorkerDay;
	}

	std::vector<std::size_t> completed;
	for (std::size_t index = 0; index < ws_.construction_projects.size(); ++index) {
		ConstructionProject& project = ws_.construction_projects[index];
		project.paused = false;
		project.pause_reason.clear();

		City* city = find_city(project.city);
		if (city == nullptr) {
			// Cidade sumiu (não deveria): descarta a obra devolvendo o fundo ao
			// setor de construção (dinheiro conservado).
			ws_.construction_cash += std::max(0.0, project.reserved);
			completed.push_back(index);
			continue;
		}
		const std::size_t city_index =
			static_cast<std::size_t>(city - ws_.cities.data());

		// Necessidades diárias de material (taxa constante: completa no prazo).
		double day_material_cost = 0.0;
		double need[kMaxConstructionMaterials] = {};
		bool short_material = false;
		for (int m = 0; m < project.material_count; ++m) {
			const int product = project.materials[m];
			if (product < 0 || product >= ws_.products) {
				continue;
			}
			need[m] = project.material_qty[m]
				/ std::max(1, project.duration_days);
			Market& market = city->markets[product];
			if (market.stock + 1e-9 < need[m]) {
				short_material = true;
				project.pause_reason = std::string("sem material: ")
					+ product_name(product);
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

		// O capital da obra já está RESERVADO no início (investimento, §15): a
		// obra paga material/serviço do fundo reservado e NÃO pausa por
		// liquidez temporária do dono (que já comprometeu o custo no limite de
		// comprometimento). Sem material ela ainda para; sem reserva (save
		// antigo/estado raro) cai no comportamento anterior (pausa).
		if (project.reserved + 1e-9 < total_out) {
			project.paused = true;
			project.pause_reason = "reserva de obra esgotada";
			continue;
		}

		// Avança: debita o fundo reservado, compra materiais do mercado e paga
		// o serviço de construção. Dinheiro conservado: fundo -total_out, firma
		// +material, serviço +mão de obra.
		project.reserved -= total_out;
		for (int m = 0; m < project.material_count; ++m) {
			const int product = project.materials[m];
			if (product < 0 || product >= ws_.products) {
				continue;
			}
			Market& market = city->markets[product];
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
			complete_project(ws_, project);
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
