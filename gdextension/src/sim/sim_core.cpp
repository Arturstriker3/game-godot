#include "sim_core.h"

#include "profiling.h"
#include "sim_constants.h"
#include "sim_labor.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace ogs {

SimCore::SimCore(WorldState& ws) : ws_(ws) {}

// ------------------------------------------------------------------
// Helpers compartilhados.
// ------------------------------------------------------------------

DemoCity* SimCore::find_demo_city(const std::string& id) {
	const auto found = ws_.city_lookup.find(id);
	if (found == ws_.city_lookup.end()) {
		return nullptr;
	}
	return &ws_.cities[static_cast<std::size_t>(found->second)];
}

const DemoCity* SimCore::find_demo_city(const std::string& id) const {
	const auto found = ws_.city_lookup.find(id);
	if (found == ws_.city_lookup.end()) {
		return nullptr;
	}
	return &ws_.cities[static_cast<std::size_t>(found->second)];
}

void SimCore::demo_rebuild_indexes() {
	ws_.city_lookup.clear();
	ws_.city_lookup.reserve(ws_.cities.size() * 2);
	for (std::size_t index = 0; index < ws_.cities.size(); ++index) {
		ws_.city_lookup.emplace(ws_.cities[index].id, static_cast<int>(index));
	}
	// Uma lista de construções por cidade: as fases do tick e o resumo passam a
	// visitar só as construções locais, em vez de varrer o mundo inteiro.
	ws_.buildings_by_city.assign(ws_.cities.size(), {});
	for (std::size_t index = 0; index < ws_.buildings.size(); ++index) {
		const auto found = ws_.city_lookup.find(ws_.buildings[index].city);
		if (found == ws_.city_lookup.end()) {
			continue;
		}
		ws_.buildings_by_city[static_cast<std::size_t>(found->second)]
			.push_back(static_cast<int>(index));
	}
	// F4: cidades por nação, na ordem estável dos índices de cidade. A frota é
	// nacional (o governo compra combustível/peças/veículos no mercado interno),
	// então a compra visita só as cidades da nação, nunca o mundo.
	ws_.cities_by_nation.clear();
	ws_.cities_by_nation.reserve(ws_.cities.size() / 4 + 8);
	ws_.fleet_order.clear();
	ws_.fleet_order.reserve(ws_.cities.size() / 4 + 8);
	for (std::size_t index = 0; index < ws_.cities.size(); ++index) {
		const std::string& nation = ws_.cities[index].nation;
		auto found = ws_.cities_by_nation.find(nation);
		if (found == ws_.cities_by_nation.end()) {
			found = ws_.cities_by_nation.emplace(nation, std::vector<int>{}).first;
			ws_.fleet_order.push_back(nation);
		}
		found->second.push_back(static_cast<int>(index));
	}
}

double SimCore::demo_money_total() const {
	double total = ws_.transport_cash + ws_.foreign_cash + ws_.construction_cash;
	for (const DemoCity& city : ws_.cities) {
		// capital_income_next é um crédito pendente às famílias: entra no total
		// para a conservação valer em qualquer ponto do tick.
		total += city.pop.savings + city.firm_cash + city.service_cash
		       + city.government_budget + city.capital_income_next;
	}
	// F9: o caixa dos grupos é dinheiro do mundo (capital retido/dividendos a
	// pagar). Sem ele, mover lucro da firma para o grupo pareceria destruição.
	for (const DemoGroup& group : ws_.groups) {
		total += group.cash;
	}
	return total;
}

void SimCore::demo_snapshot(DemoSnapshot& out) const {
	out.cities = ws_.cities;
	out.buildings = ws_.buildings;
	out.service_businesses = ws_.service_businesses;
	out.next_service_business_id = ws_.next_service_business_id;
	out.service_bankruptcies = ws_.service_bankruptcies;
	out.transit = ws_.in_transit;
	out.fleets = ws_.fleets;
	out.groups = ws_.groups;
	out.confidence = ws_.confidence;
	out.group_flows = ws_.group_flows;
	out.currencies = ws_.currencies;
	out.transport_cash = ws_.transport_cash;
	out.foreign_cash = ws_.foreign_cash;
	out.construction_cash = ws_.construction_cash;
	for (int index = 0; index < ws_.products; ++index) {
		out.produced[index] = ws_.produced[index];
		out.consumed_total[index] = ws_.consumed_total[index];
		out.produced_today[index] = ws_.produced_today[index];
		out.consumed_today[index] = ws_.consumed_today[index];
	}
	out.day = ws_.day;
}



// ------------------------------------------------------------------
// Porto: capacidade, fila e cobrança.
// ------------------------------------------------------------------







// ------------------------------------------------------------------
// Frota nacional (F4).
// ------------------------------------------------------------------












void SimCore::demo_init_pop_needs(DemoPop& pop) const {
	if (pop.level_want.size() != kDemoMaxNeedsLevels) {
		pop.level_want.assign(kDemoMaxNeedsLevels, 0.0);
	}
	if (pop.level_consumed.size() != kDemoMaxNeedsLevels) {
		pop.level_consumed.assign(kDemoMaxNeedsLevels, 0.0);
	}
	if (pop.level_satisfaction.size() != kDemoMaxNeedsLevels) {
		pop.level_satisfaction.assign(kDemoMaxNeedsLevels, 0.0);
	}
	// F7: gasto por nível (custo unitário da cesta para o teto de renda).
	if (pop.level_spend.size() != kDemoMaxNeedsLevels) {
		pop.level_spend.assign(kDemoMaxNeedsLevels, 0.0);
	}
	// `resize` preserva o prefixo: um bem novo não zera o estoque durável.
	if (pop.durable_stock.size() != static_cast<std::size_t>(std::max(0, ws_.products))) {
		pop.durable_stock.resize(static_cast<std::size_t>(std::max(0, ws_.products)), 0.0);
	}
}

// ------------------------------------------------------------------
// Ordem do dia (fases).
// ------------------------------------------------------------------

void SimCore::prologue() {
	// Retrato para reverter o tick se um invariante rígido for violado.
	// Buffer PERSISTENTE: os vetores são reutilizados entre dias (com 114 bens
	// uma cidade custa ~27 KB; realocar 834 cópias por tick custava ~5 ms).
	DemoSnapshot& before = ws_.snapshot_buffer;
	demo_snapshot(before);
	money_before_ = demo_money_total();

	++ws_.day;
	ws_.last_orders.clear();
	// Obras de porto com prazo vencido entram em operação neste dia.
	demo_tick_port_expansion();

	// Encerra as construções falidas no início do tick: assim o PIB do dia já
	// não as inclui e o status fica consistente (a falência foi marcada no tick
	// anterior, no fim da fase 7).
	for (auto it = ws_.buildings.begin(); it != ws_.buildings.end();) {
		if (it->bankrupt) {
			++ws_.bankruptcies;
			it = ws_.buildings.erase(it);
			continue;
		}
		// Mina com jazida esgotada encerra a operação no dia seguinte: o fim da
		// reserva é o fim econômico planejado, não uma insolvência. Contá-la
		// como falência inflava o número (99 minas de ouro/diamante de jazidas
		// de poucos dias em 730 d) e escondia as falências de verdade (§19).
		if (it->depleted) {
			++ws_.depleted_mines;
			it = ws_.buildings.erase(it);
			continue;
		}
		++it;
	}
	// O tick apaga construções falidas: os índices por cidade são refeitos
	// antes de qualquer fase usá-los.
	demo_rebuild_indexes();

	// Preço de abertura do dia por cidade/produto. O limite de variação é
	// aplicado UMA vez, no fim do tick: importação e reação de mercado não
	// podem somar dois passos de 5% e produzir um salto de 10% no mesmo dia.
	open_prices_.assign(ws_.cities.size(), {});
	for (std::size_t city_index = 0; city_index < ws_.cities.size(); ++city_index) {
		open_prices_[city_index].resize(ws_.products);
		for (int product = 0; product < ws_.products; ++product) {
			open_prices_[city_index][product] =
				ws_.cities[city_index].markets[product].price;
		}
	}
}


void SimCore::phase_reset() {
	// 2. Zera os fluxos do dia.
	for (int index = 0; index < ws_.products; ++index) {
		ws_.produced_today[index] = 0.0;
		ws_.consumed_today[index] = 0.0;
	}
	for (DemoCity& city : ws_.cities) {
		city.pop.wage_income = 0.0;
		city.pop.capital_income = 0.0;
		city.pop.transfers = 0.0;
		city.pop.taxes = 0.0;
		city.pop.service_spend = 0.0;
		city.pop.disposable = 0.0;
		city.pop.external_income = 0.0;
		city.pop.food_spend = 0.0;
		city.pop.consumed = 0.0;
		city.pop.need = 0.0;
		city.pop.satisfaction = 0.0;
		city.pop.needs_spend = 0.0;
		city.pop.durable_spend = 0.0;
		city.pop.needs_satisfaction = 0.0;
		city.pop.needs_level = 0;
		// F3: níveis e duráveis (redimensiona só quando o mundo mudou).
		demo_init_pop_needs(city.pop);
		for (int need_level = 0; need_level < kDemoMaxNeedsLevels; ++need_level) {
			city.pop.level_want[need_level] = 0.0;
			city.pop.level_consumed[need_level] = 0.0;
			city.pop.level_satisfaction[need_level] = 0.0;
		}
		city.government_goods_spend = 0.0;
		city.tariff_revenue = 0.0;
		city.service_wage_bill = 0.0;
		city.service_revenue = 0.0;
		city.service_profit = 0.0;
		city.opex_revenue = 0.0;
		city.freight_revenue = 0.0;
		city.construction_revenue = city.construction_revenue_pending;
		city.construction_revenue_pending = 0.0;
		city.gdp = 0.0;
		// F8: fluxos do dia dos serviços (preço/base persistidos).
		city.service_export_revenue = 0.0;
		city.service_government_revenue = 0.0;
		city.service_input_cost = 0.0;
		city.service_business_profit = 0.0;
		for (int sector = 0; sector < kDemoMaxServiceSectors; ++sector) {
			DemoServiceMarket& service = city.service_markets[sector];
			service.demand = 0.0;
			service.supply = 0.0;
			service.unmet = 0.0;
			service.revenue = 0.0;
			service.input_cost = 0.0;
			service.service_cost = 0.0;
			service.jobs = 0.0;
			service.businesses = 0.0;
			service.exports = 0.0;
			service.export_revenue = 0.0;
			service.coverage = 0.0;
			for (int level = 0; level < kDemoMaxNeedsLevels; ++level) {
				service.pop_demand[level] = 0.0;
			}
		}
		for (int product = 0; product < ws_.products; ++product) {
			city.markets[product].demand = 0.0;
			city.markets[product].supply = 0.0;
			city.markets[product].desired = 0.0;
			city.markets[product].latent = 0.0;
			city.markets[product].sold_prev = city.markets[product].sold;
			city.markets[product].sold = 0.0;
			city.markets[product].shortage = 0.0;
			city.markets[product].surplus = 0.0;
			city.revenue_by_product[product] = 0.0;
			city.imported[product] = 0.0;
			city.exported[product] = 0.0;
			city.import_value[product] = 0.0;
			city.export_value[product] = 0.0;
		}
		if (city.pop.desired_by_category.size() != ws_.food_products.size()) {
			city.pop.desired_by_category.assign(ws_.food_products.size(), 0.0);
			city.pop.consumed_by_category.assign(ws_.food_products.size(), 0.0);
		}
		for (int product = 0; product < static_cast<int>(ws_.food_products.size()); ++product) {
			city.pop.desired_by_category[product] = 0.0;
			city.pop.consumed_by_category[product] = 0.0;
		}
	}
	for (DemoBuilding& building : ws_.buildings) {
		building.workers = 0.0;
		building.utilization = 0.0;
		building.revenue = 0.0;
		building.input_value = 0.0;
		building.costs = 0.0;
		building.profit = 0.0;
		building.paid_wages = 0.0;
		building.paid_opex = 0.0;
	}

	OGS_SIM_PHASE_LAP(kDemoPhaseReset);
}


void SimCore::phase_labor() {
	// Minas mais fundas custam mais para operar: opex × fator de profundidade
	// (1.0 para tudo que não é extração com jazida declarada).
	auto opex_factor = [this](const DemoCity& owner_city, const DemoBuilding& building) {
		if (building.input_product >= 0 || building.output_product < 0
				|| building.output_product >= ws_.products) {
			return 1.0;
		}
		return demo_depth_opex_factor(owner_city.deposits[building.output_product]);
	};
	for (std::size_t city_index = 0; city_index < ws_.cities.size(); ++city_index) {
		DemoCity& city = ws_.cities[city_index];
		const std::vector<int>& local_buildings =
			ws_.buildings_by_city[city_index];
		// F7: o dia do mercado de trabalho resolve qualificação, força de
		// trabalho por nível, vagas, emprego e folha. O opex continua sendo o
		// custo de operação das plantas (pago em seguida, também limitado ao
		// caixa da firma).
		double opex_total = 0.0;
		for (const int bi : local_buildings) {
			DemoBuilding& building = ws_.buildings[static_cast<std::size_t>(bi)];
			const bool secondary = building.recipe >= 0 || building.input_product >= 0;
			// Planta secundária ociosa não paga o opex cheio: o custo acompanha
			// a operação do dia anterior (piso de 5% de stand-by).
			const double operating = secondary
				? std::clamp(building.last_utilization, 0.05, 1.0) : 1.0;
			opex_total += building.opex * opex_factor(city, building) * operating;
		}
		demo_labor_day(city);
		// Opex é comprado de serviços domésticos (manutenção, energia), não é
		// uma importação: mantém o dinheiro dentro da economia. A liquidez do
		// opex é a que sobrou do caixa depois da folha.
		const double opex_available = std::max(0.0, city.firm_cash);
		const double opex_scale = opex_total > opex_available
			? (opex_total > 0.0 ? opex_available / opex_total : 1.0) : 1.0;
		for (const int bi : local_buildings) {
			DemoBuilding& building = ws_.buildings[static_cast<std::size_t>(bi)];
			const bool secondary = building.recipe >= 0 || building.input_product >= 0;
			const double operating = secondary
				? std::clamp(building.last_utilization, 0.05, 1.0) : 1.0;
			building.paid_opex = building.opex * opex_factor(city, building)
				* operating * opex_scale;
		}
		const double paid_opex = opex_total * opex_scale;
		city.firm_cash -= paid_opex;
		city.service_cash += paid_opex;
		city.opex_revenue = paid_opex;
	}

	OGS_SIM_PHASE_LAP(kDemoPhaseLabor);
}

// ------------------------------------------------------------------
// F7/F9 — trabalho, educação, renda e emprego setorial.
// ------------------------------------------------------------------

void SimCore::demo_refresh_strata(DemoPop& pop) const {
	// Estrato = escolaridade/ocupação (básico, intermediário, superior). A
	// população do estrato inclui os dependentes: a escolaridade é medida na
	// idade ativa, mas a renda do estrato sustenta toda a sua população.
	double education_total = 0.0;
	for (int level = 0; level < kDemoEducationLevels; ++level) {
		education_total += std::max(0.0, pop.education[level]);
	}
	for (int level = 0; level < kDemoEducationLevels; ++level) {
		pop.stratum_population[level] = education_total > 1e-9
			? pop.population * std::max(0.0, pop.education[level]) / education_total
			: pop.population / double(kDemoEducationLevels);
	}
}

void SimCore::demo_education_mix_for_sector(const std::string& sector,
		double* mix) {	// Escolaridade das vagas por setor (recipes.json `sector`). Semicondutores/
	// eletrônicos e máquinas pedem mais superior; alimentos, têxteis, madeira e
	// embalagem são intensivos em básico. Tabela de engenharia do modelo.
	struct Row {
		const char* sector;
		double mix[3];
	};
	static const Row kRows[] = {
		{"electronics", {0.22, 0.40, 0.38}},
		{"machinery", {0.30, 0.45, 0.25}},
		{"chemical", {0.40, 0.42, 0.18}},
		{"military", {0.35, 0.45, 0.20}},
		{"luxury", {0.45, 0.38, 0.17}},
		{"metal", {0.50, 0.38, 0.12}},
		{"transport", {0.55, 0.35, 0.10}},
		{"construction", {0.65, 0.30, 0.05}},
		{"food", {0.70, 0.25, 0.05}},
		{"packaging", {0.70, 0.25, 0.05}},
		{"textile", {0.72, 0.24, 0.04}},
		{"wood", {0.72, 0.24, 0.04}},
		{"agro", {0.88, 0.10, 0.02}},
	};
	for (const Row& row : kRows) {
		if (sector == row.sector) {
			for (int level = 0; level < kDemoEducationLevels; ++level) {
				mix[level] = row.mix[level];
			}
			return;
		}
	}
	const double fallback[3] = {0.60, 0.30, 0.10};
	for (int level = 0; level < kDemoEducationLevels; ++level) {
		mix[level] = fallback[level];
	}
}

void SimCore::demo_sector_anchor_values(double income, double* out) const {
	// Interpolação linear no log da renda entre as âncoras observadas (ILO).
	// Fora da faixa, vale o extremo (nação mais pobre/rica do conjunto).
	double incomes[kDemoSectorAnchorCount];
	double agri[kDemoSectorAnchorCount];
	double industry[kDemoSectorAnchorCount];
	double informal[kDemoSectorAnchorCount];
	int count = 0;
	if (!ws_.sector_anchors.empty()) {
		for (const DemoSectorAnchor& anchor : ws_.sector_anchors) {
			incomes[count] = anchor.income;
			agri[count] = anchor.agriculture;
			industry[count] = anchor.industry;
			informal[count] = anchor.informal;
			++count;
		}
	} else {
		for (int i = 0; i < kDemoSectorAnchorCount; ++i) {
			incomes[i] = kDemoSectorAnchorDefaults[i].income;
			agri[i] = kDemoSectorAnchorDefaults[i].agriculture;
			industry[i] = kDemoSectorAnchorDefaults[i].industry;
			informal[i] = kDemoSectorAnchorDefaults[i].informal;
		}
		count = kDemoSectorAnchorCount;
	}
	const double x = std::log(std::clamp(income, 0.02, 10.0));
	if (x <= std::log(incomes[0])) {
		out[0] = agri[0];
		out[1] = industry[0];
		out[2] = informal[0];
		return;
	}
	if (x >= std::log(incomes[count - 1])) {
		out[0] = agri[count - 1];
		out[1] = industry[count - 1];
		out[2] = informal[count - 1];
		return;
	}
	for (int i = 1; i < count; ++i) {
		const double x0 = std::log(incomes[i - 1]);
		const double x1 = std::log(incomes[i]);
		if (x > x1) {
			continue;
		}
		const double t = x1 > x0 ? std::clamp((x - x0) / (x1 - x0), 0.0, 1.0) : 0.0;
		out[0] = agri[i - 1] + (agri[i] - agri[i - 1]) * t;
		out[1] = industry[i - 1] + (industry[i] - industry[i - 1]) * t;
		out[2] = informal[i - 1] + (informal[i] - informal[i - 1]) * t;
		return;
	}
	out[0] = agri[count - 1];
	out[1] = industry[count - 1];
	out[2] = informal[count - 1];
}

void SimCore::demo_update_sector_employment(DemoCity& city) {
	DemoPop& pop = city.pop;
	const double tfp = std::clamp(demo_effective_productivity(city), 0.25, 4.0);
	double anchors[3] = {0.0, 0.0, 0.0};
	demo_sector_anchor_values(tfp, anchors);
	// 1. Produção agrícola local (t/dia): construções PRIMÁRIAS cujo produto
	// está na cesta alimentar. É a capacidade declarada — a mesma que o status
	// publica em `production` —, não o vendido do dia (preço/caixa não muda a
	// necessidade de mão de obra da lavoura).
	city.agri_output_tons = 0.0;
	double industry_va = 0.0;
	const std::size_t city_index =
		static_cast<std::size_t>(&city - ws_.cities.data());
	if (city_index < ws_.buildings_by_city.size()) {
		for (const int bi : ws_.buildings_by_city[city_index]) {
			const DemoBuilding& building =
				ws_.buildings[static_cast<std::size_t>(bi)];
			if (building.output_product < 0) {
				continue;
			}
			if (building.input_product >= 0 || building.recipe >= 0) {
				// Valor adicionado do dia da planta secundária (receita menos
				// insumos, como no PIB da cidade).
				industry_va += std::max(0.0,
					building.revenue - building.input_value);
				continue;
			}
			for (const int food : ws_.food_products) {
				if (food == building.output_product) {
					city.agri_output_tons +=
						std::max(0.0, building.capacity * building.output_scale);
					break;
				}
			}
		}
	}
	const double employed = std::max(0.0, pop.employed);
	if (employed <= 0.0) {
		for (int i = 0; i < 4; ++i) {
			city.sector_workers[i] = 0.0;
			city.sector_share[i] = 0.0;
		}
		return;
	}
	const double labor_force = std::max(pop.labor_force, 1e-9);
	// 2. Demanda de trabalho agrícola: t/dia × base / TFP^expoente. A âncora
	// ILO dá a ESTRUTURA (share of total employment); a produção local modula
	// em [band_min, band_max] — a capacidade de fazenda do cenário (1 por
	// cidade) não é uma função de produção calibrada, então ela não pode
	// substituir a estrutura observada (ver o relatório F9).
	const double production_share = city.agri_output_tons
		* ws_.agri_labor_per_ton / std::pow(tfp, ws_.agri_labor_exponent)
		/ labor_force;
	const double production_factor = anchors[0] > 1e-9
		? std::clamp(production_share / anchors[0],
			ws_.agri_production_band_min, ws_.agri_production_band_max)
		: 1.0;
	double agri = std::clamp(anchors[0] * production_factor, 0.0, 0.65)
		* employed;
	// 3. Indústria/mineração: âncora de renda (ILO) e, como piso adicional, o
	// valor adicionado simulado por trabalhador (1/TFP). No cenário atual a
	// indústria é um esqueleto (poucas plantas), então a âncora domina.
	const double va_jobs = ws_.industry_labor_per_va > 0.0
		? industry_va * ws_.industry_labor_per_va
			/ std::pow(tfp, ws_.industry_labor_exponent)
		: 0.0;
	double industry = std::clamp(
		std::max(anchors[1] * employed, va_jobs), 0.0, 0.60 * employed);
	// 4. Serviços é o residual. A soma nunca passa do emprego; a informalidade
	// é uma sobreposição (parte dos três setores), não um quarto setor.
	if (agri + industry > employed) {
		const double scale = employed / std::max(agri + industry, 1e-9);
		agri *= scale;
		industry *= scale;
	}
	const double services = std::max(0.0, employed - agri - industry);
	city.sector_workers[0] = agri;
	city.sector_workers[1] = industry;
	city.sector_workers[2] = services;
	city.sector_workers[3] = std::clamp(anchors[2], 0.0, 1.0) * employed;
	for (int i = 0; i < 4; ++i) {
		city.sector_share[i] = employed > 1e-9
			? city.sector_workers[i] / employed : 0.0;
	}
	// F8 → F9: `service_workers` publica o EMPREGO do setor de serviços (a
	// fatia estrutural), não mais as vagas pagas pelo setor. As vagas pagas
	// continuam em `service_jobs_filled` — salário e folha seguem por elas.
	city.service_workers = services;
}

double SimCore::demo_labor_day(DemoCity& city) {
	DemoPop& pop = city.pop;
	// 1. QUALIFICAÇÃO — conclusão. A coorte em formação conclui a uma taxa
	// constante (coorte exponencial com duração média de 3/4 anos): quem
	// conclui muda de nível de escolaridade e passa a disputar as vagas do
	// nível acima. A população não muda; só a distribuição de educação.
	for (int dst = 1; dst < kDemoEducationLevels; ++dst) {
		double& cohort = pop.studying[dst - 1];
		const double duration = kLaborStudyDurationDays[dst - 1];
		if (cohort <= 0.0 || duration <= 0.0) {
			continue;
		}
		const double completed = cohort / duration
			* (1.0 + kDemoServiceEducationRateBonus * city.private_education_cover);
		const int src = dst - 1;
		const double moved = std::min(completed, pop.education[src]);
		cohort = std::max(0.0, cohort - moved);
		pop.education[src] = std::max(0.0, pop.education[src] - moved);
		pop.education[dst] += moved;
	}
	// 2. QUALIFICAÇÃO — matrícula. O prêmio salarial do nível acima (que
	// EMERGE do aperto de cada mercado) atrai jovens do nível de origem. Sem
	// vagas no destino (desemprego alto), a escola não cresce: não se forma
	// gente para ficar desempregada. Não há custo de escola nesta fase.
	for (int dst = 1; dst < kDemoEducationLevels; ++dst) {
		const int src = dst - 1;
		const double premium = pop.wage_by_level[dst]
			/ std::max(pop.wage_by_level[src], 1e-9);
		if (premium <= kLaborStudyPremiumThreshold) {
			continue;
		}
		const double target_supply = pop.labor_force_by_level[dst];
		const double target_unemployment = target_supply > 0.0
			? pop.unemployed_by_level[dst] / target_supply : 0.0;
		const double scarcity = std::clamp(
			1.0 - target_unemployment / 0.08, 0.0, 1.0);
		const double annual = std::clamp(
			(premium - kLaborStudyPremiumThreshold) * kLaborStudyElasticity,
			0.0, kLaborStudyMaxAnnual) * scarcity;
		double& cohort = pop.studying[dst - 1];
		double flow = pop.education[src] * kLaborStudyYouthShare * annual / 365.0;
		flow = std::min(flow,
			std::max(0.0, pop.unemployed_by_level[src])
				* kLaborStudyJoblessShare);
		flow = std::min(flow,
			std::max(0.0, pop.education[src] - cohort)
				* kLaborStudyMaxDailyShare);
		if (flow > 0.0) {
			cohort += flow;
		}
	}
	// 3. FORÇA DE TRABALHO por nível (estudantes ficam fora; educação de
	// origem permanece até a conclusão).
	pop.labor_force = 0.0;
	pop.students = 0.0;
	for (int level = 0; level < kDemoEducationLevels; ++level) {
		const double studying = level < kDemoEducationLevels - 1
			? pop.studying[level] : 0.0;
		pop.students += studying;
		pop.labor_force_by_level[level] = std::max(0.0,
			pop.education[level] * pop.participation[level] - studying);
		pop.labor_force += pop.labor_force_by_level[level];
	}
	// 4. VAGAS por nível: serviços urbanos + cada construção (a mistura da
	// receita/setor). É a DEMANDA que o mercado de trabalho precisa preencher.
	double vacancy[kDemoEducationLevels] = {};
	for (int level = 0; level < kDemoEducationLevels; ++level) {
		vacancy[level] += city.service_jobs * kLaborServiceMix[level];
	}
	const std::size_t city_index =
		static_cast<std::size_t>(&city - ws_.cities.data());
	if (city_index < ws_.buildings_by_city.size()) {
		for (const int bi : ws_.buildings_by_city[city_index]) {
			const DemoBuilding& building =
				ws_.buildings[static_cast<std::size_t>(bi)];
			if (building.jobs <= 0.0) {
				continue;
			}
			for (int level = 0; level < kDemoEducationLevels; ++level) {
				vacancy[level] += building.jobs * building.education_mix[level];
			}
		}
	}
	// 5. ALOCAÇÃO: cada trabalhador procura a vaga do SEU nível e, se não
	// houver, aceita uma vaga ABAIXO (sobrequalificado); nunca o contrário.
	// `remaining` guarda as vagas não preenchidas por nível (escassez).
	double remaining[kDemoEducationLevels];
	double assigned[kDemoEducationLevels][kDemoEducationLevels] = {};
	for (int level = 0; level < kDemoEducationLevels; ++level) {
		remaining[level] = vacancy[level];
		pop.vacancies_by_level[level] = vacancy[level];
		pop.unfilled_by_level[level] = 0.0;
	}
	for (int edu = kDemoEducationLevels - 1; edu >= 0; --edu) {
		double supply = pop.labor_force_by_level[edu];
		for (int slot = edu; slot >= 0 && supply > 1e-12; --slot) {
			const double take = std::min(supply, remaining[slot]);
			if (take <= 0.0) {
				continue;
			}
			assigned[edu][slot] += take;
			remaining[slot] -= take;
			supply -= take;
		}
		pop.employed_by_level[edu] = 0.0;
		for (int slot = 0; slot < kDemoEducationLevels; ++slot) {
			pop.employed_by_level[edu] += assigned[edu][slot];
		}
		pop.unemployed_by_level[edu] =
			std::max(0.0, pop.labor_force_by_level[edu]
				- pop.employed_by_level[edu]);
	}
	for (int level = 0; level < kDemoEducationLevels; ++level) {
		pop.unfilled_by_level[level] = std::max(0.0, remaining[level]);
	}
	double fill[kDemoEducationLevels];
	for (int level = 0; level < kDemoEducationLevels; ++level) {
		fill[level] = vacancy[level] > 1e-12
			? std::clamp((vacancy[level] - remaining[level]) / vacancy[level],
				0.0, 1.0)
			: 1.0;
		// Vagas efetivamente preenchidas: peso da folha no salário ancorado no
		// valor adicionado (inclui o sobrequalificado que ocupa a vaga de baixo).
		pop.filled_by_slot[level] = std::max(0.0, vacancy[level] - remaining[level]);
	}
	// 6. FOLHA: cada construção emprega `jobs × Σ(mix × preenchimento)` e paga
	// o salário do NÍVEL DA VAGA. Vaga qualificada sem trabalhador qualificado
	// reduz o emprego (e a produção, via `workers/jobs`) proporcionalmente.
	double buildings_payroll = 0.0;
	if (city_index < ws_.buildings_by_city.size()) {
		for (const int bi : ws_.buildings_by_city[city_index]) {
			DemoBuilding& building = ws_.buildings[static_cast<std::size_t>(bi)];
			double workers_fraction = 0.0;
			double contract_wage = 0.0;
			double plant_payroll = 0.0;
			for (int level = 0; level < kDemoEducationLevels; ++level) {
				workers_fraction += building.education_mix[level] * fill[level];
				contract_wage += building.education_mix[level]
					* pop.wage_by_level[level];
				plant_payroll += building.education_mix[level] * fill[level]
					* pop.wage_by_level[level];
			}
			building.workers = building.jobs * workers_fraction;
			building.wage = contract_wage;
			const bool secondary =
				building.recipe >= 0 || building.input_product >= 0;
			const double operating = secondary
				? std::clamp(building.last_utilization, 0.05, 1.0) : 1.0;
			buildings_payroll += building.jobs * plant_payroll * operating;
		}
	}
	double service_fraction = 0.0;
	double service_contract_wage = 0.0;
	double service_payroll_per_job = 0.0;
	for (int level = 0; level < kDemoEducationLevels; ++level) {
		service_fraction += kLaborServiceMix[level] * fill[level];
		service_contract_wage += kLaborServiceMix[level]
			* pop.wage_by_level[level];
		service_payroll_per_job += kLaborServiceMix[level] * fill[level]
			* pop.wage_by_level[level];
	}
	// F8: vagas de serviço efetivamente preenchidas (peso da mistura de
	// escolaridade × preenchimento). É o emprego real do setor, não a meta.
	city.service_jobs_filled = city.service_jobs * service_fraction;
	city.service_workers = city.service_jobs_filled;
	pop.employed = 0.0;
	pop.unemployed = 0.0;
	for (int level = 0; level < kDemoEducationLevels; ++level) {
		pop.employed += pop.employed_by_level[level];
		pop.unemployed += pop.unemployed_by_level[level];
		pop.wage_income_by_level[level] = 0.0;
		for (int slot = 0; slot < kDemoEducationLevels; ++slot) {
			pop.wage_income_by_level[level] += assigned[level][slot]
				* pop.wage_by_level[slot];
		}
	}
	pop.dependents = std::max(0.0, pop.population - pop.labor_force);
	// F9: decompõe o emprego do dia em agro/indústria/serviços + informalidade.
	demo_update_sector_employment(city);
	// F7 calibração: estratos de renda = escolaridade/ocupação (ver
	// demo_refresh_strata). A população do estrato inclui os dependentes.
	demo_refresh_strata(pop);
	// 7. Caixa da firma e do setor de serviços: a folha total é paga só até o
	// caixa disponível (restrição de liquidez do modelo, preservada). O setor de
	// serviços também só paga até o próprio caixa.
	const double buildings_full_payroll = buildings_payroll;
	const double firm_available = std::max(0.0, city.firm_cash);
	const double committed = buildings_full_payroll;
	const double pay_scale = committed > firm_available
		? firm_available / committed : 1.0;
	if (city_index < ws_.buildings_by_city.size()) {
		for (const int bi : ws_.buildings_by_city[city_index]) {
			DemoBuilding& building = ws_.buildings[static_cast<std::size_t>(bi)];
			const bool secondary =
				building.recipe >= 0 || building.input_product >= 0;
			const double operating = secondary
				? std::clamp(building.last_utilization, 0.05, 1.0) : 1.0;
			double plant_payroll = 0.0;
			for (int level = 0; level < kDemoEducationLevels; ++level) {
				plant_payroll += building.education_mix[level] * fill[level]
					* pop.wage_by_level[level];
			}
			building.paid_wages =
				building.jobs * plant_payroll * operating * pay_scale;
		}
	}
	const double full_service_bill = city.service_jobs * service_payroll_per_job;
	const double paid_service_bill = std::min(full_service_bill,
		std::max(0.0, city.service_cash));
	// Ganchos do laboratório mantidos: `service_wage_adjust` é a velocidade com
	// que o salário pago do setor converge para a média do mercado de trabalho
	// e `service_wage_max` limita o valor (parâmetros de balanceamento, não
	// política). Com os padrões (0,10 e 1,0) o salário acompanha o mercado.
	const double market_service_wage = service_fraction > 1e-12
		? service_contract_wage / std::max(service_fraction, 1e-12) : 0.0;
	if (market_service_wage > 0.0) {
		city.service_wage += (market_service_wage - city.service_wage)
			* std::clamp(ws_.service_wage_adjust, 0.0, 1.0);
	}
	city.service_wage = std::min(
		std::max(0.0, city.service_wage), ws_.service_wage_max);
	city.service_wage_bill = paid_service_bill;
	const double paid_total = buildings_full_payroll * pay_scale
		+ paid_service_bill;
	// A renda por nível é a folha CHEIA por nível, escalada pela mesma fração
	// efetivamente paga (liquidez + teto do caixa de serviços).
	const double full_level_total = pop.wage_income_by_level[0]
		+ pop.wage_income_by_level[1] + pop.wage_income_by_level[2];
	const double paid_ratio = full_level_total > 1e-12
		? std::clamp(paid_total / full_level_total, 0.0, 1.0) : 0.0;
	for (int level = 0; level < kDemoEducationLevels; ++level) {
		pop.wage_income_by_level[level] *= paid_ratio;
	}
	pop.wage_income = pop.wage_income_by_level[0]
		+ pop.wage_income_by_level[1] + pop.wage_income_by_level[2];
	city.firm_cash -= buildings_payroll * pay_scale;
	city.service_cash -= paid_service_bill;
	return pop.wage_income;
}

// ------------------------------------------------------------------
// F7/F9 — renda das famílias, impostos, transferências e consumo (cesta
// alimentar, níveis de necessidade e classes de renda).
// ------------------------------------------------------------------

void SimCore::phase_income() {
	// 5. Renda das famílias, impostos, transferências e consumo alimentar.
	for (DemoCity& city : ws_.cities) {
		DemoPop& pop = city.pop;
		pop.capital_income = city.capital_income_next;
		city.capital_income_next = 0.0;
		pop.taxes = (pop.wage_income + pop.capital_income) * kDemoTaxRate;
		// Orçamento público equilibrado: tudo o que é arrecadado volta como
		// transferência. Sem isso, o governo acumula receita e drena demanda.
		pop.transfers = pop.taxes * kDemoTransferShare;
		pop.disposable = std::max(0.0,
			pop.wage_income + pop.capital_income + pop.transfers - pop.taxes);
		// Renda externa (serviços urbanos/pesca/turismo): o resto do mundo paga
		// por serviços da cidade. Entra no orçamento das famílias e sai do setor
		// externo — é a renda que financia a importação de alimento onde não há
		// terra produtiva. Não é tributada nem infla a folha doméstica (§20).
		// O índice por cidade vem do PIB PPP per capita da nação (GeographyService,
		// nations.csv), normalizado para média mundial = 1,0; cidade sem índice
		// declarado (cenário sintético, save antigo) mantém a média (1,0).
		//
		// F7 calibração: é um PISO DE SUBSISTÊNCIA, então o índice é limitado à
		// média mundial (kDemoExternalIncomeIndexCap). Escalado até o topo, esse
		// bloco sozinho pagava 3× a cesta completa das famílias dos EUA/DEU e
		// punha o mundo inteiro no nível luxury. A renda acima da subsistência
		// vem do valor adicionado (salário + lucro), medido na cidade.
		pop.external_income = city.simulation_population
			* kDemoExternalIncomePerCapita
			* std::min(city.external_income_index, kDemoExternalIncomeIndexCap);
		pop.disposable += pop.external_income;
		ws_.foreign_cash -= pop.external_income;

		// Necessidade alimentar do dia em toneladas-equivalentes de grão (B3) e
		// custo EFETIVO da cesta: os preços dos alimentos que a cidade REALMENTE
		// tem em estoque (na proporção de disponibilidade usada na compra) mais
		// o resíduo importado ao CIF conhecido. Antes, o subsídio usava o
		// alimento mais barato do MUNDO — pão/processado sem estoque local — e
		// subestimava em 3-4× o custo do grão da prateleira; a cidade comprava
		// só uma fração da necessidade mesmo gastando todo o orçamento
		// (NGA-LAG, SYR-DAM, SOL-HAR em 365 d).
		pop.need = city.simulation_population * kDemoNeedPerCapita;
		double food_local_cost = 0.0;
		double food_import_cost = 0.0;
		double food_unit_cost = 0.0;
		demo_food_basket_cost(city, pop.need, food_local_cost, food_import_cost,
			food_unit_cost);

		// A renda externa é renda de subsistência: o gasto discricionário em
		// serviços incide só sobre a renda DOMÉSTICA. Sem isso a cidade sem
		// terra gastava 45% da renda externa em serviços e sobrava menos que a
		// cesta (TKM ficava em 64% em 730 d, §20). A comida tem prioridade de
		// ORDENAÇÃO (é comprada antes de life/luxury e o governo cobre o que
		// faltar); o setor de serviços continua sendo a fonte de renda dos
		// empregos residuais, por isso não é zerado pela cesta.
		// F8 — FREIO DE FOME: quando a cidade não consegue cobrir a cesta nem
		// com a poupança mais a renda doméstica do dia, o gasto em serviços só
		// consome o que passar da cesta; sem essa situação, vale a fração de 45%
		// da renda doméstica (comportamento F7), que financia o setor cuja folha
		// e lucro voltam para a POP. A condição é de AFORDABILIDADE, não a
		// satisfação de ontem: cidade nova (sem comida comprada ainda) não pode
		// cair num laço de "sem serviço → sem renda → sem comida".
		const double basket_cost = pop.need * food_unit_cost
			* kDemoServiceFoodGuardDays;
		const double domestic_income =
			std::max(0.0, pop.disposable - pop.external_income);
		double service_base = domestic_income;
		if (basket_cost > 0.0 && domestic_income + std::max(0.0, pop.savings)
				< basket_cost) {
			service_base = std::max(0.0, domestic_income - basket_cost);
		}
		pop.service_spend = service_base * kDemoServiceSpendShare;
		city.service_cash += pop.service_spend;
		city.government_budget += pop.taxes - pop.transfers;

		// F7 calibração — RENDA POR ESTRATO. A folha por escolaridade vem do
		// mercado de trabalho (já paga e escalada pela liquidez); o lucro vai só
		// para os DONOS (estrato superior); a renda externa de subsistência é
		// uniforme per capita. O orçamento de compra continua sendo o da cidade
		// (o dinheiro é conservado); o que muda por estrato é QUEM pode comprar
		// cada nível de cesta.
		double stratum_pop_sum = 0.0;
		for (int level = 0; level < kDemoEducationLevels; ++level) {
			stratum_pop_sum += std::max(0.0, pop.stratum_population[level]);
		}
		for (int level = 0; level < kDemoEducationLevels; ++level) {
			const double share = stratum_pop_sum > 1e-9
				? std::max(0.0, pop.stratum_population[level]) / stratum_pop_sum
				: 1.0 / double(kDemoEducationLevels);
			pop.stratum_wage_income[level] = pop.wage_income_by_level[level];
			pop.stratum_external_income[level] = pop.external_income * share;
			pop.stratum_capital_income[level] = 0.0;
			pop.stratum_disposable[level] =
				pop.stratum_wage_income[level]
				+ pop.stratum_external_income[level];
		}
		// Lucro (renda de capital) só para os donos: o estrato de escolaridade
		// superior. Dividi-lo igualmente entre toda a POP é o que fazia
		// 832/834 cidades caírem no nível máximo de consumo.
		pop.stratum_capital_income[kDemoEducationLevels - 1] =
			std::max(0.0, pop.capital_income);
		pop.stratum_disposable[kDemoEducationLevels - 1] +=
			std::max(0.0, pop.capital_income);

		// F7 calibração — classes de consumo do dia (Gini declarado) ou espelho
		// dos estratos de escolaridade (legado). O teto de consumo de hoje usa
		// a classe; no caminho legado, o nível do estrato veio de ontem.
		demo_refresh_income_classes(city);

		double budget = pop.savings + pop.disposable - pop.service_spend;
		// Subsídio de subsistência: o governo garante a cesta alimentar mínima
		// quando o orçamento de comida não cobre (ex.: cidade sem produção nem
		// renda própria). Primeiro a parcela que está na prateleira (estoque
		// local); depois o resíduo que terá de ser importado, para que a fase 6
		// (comércio) encontre caixa. Sai do orçamento público — conserva o
		// dinheiro.
		double food_topup_local = 0.0;
		if (food_local_cost > 0.0 && budget < food_local_cost) {
			food_topup_local = std::min(food_local_cost - budget,
				std::max(0.0, city.government_budget));
			if (food_topup_local > 0.0) {
				budget += food_topup_local;
				city.government_budget -= food_topup_local;
				pop.transfers += food_topup_local;
			}
		}
		double food_topup_import = 0.0;
		if (food_import_cost > 0.0) {
			const double own_left = std::max(0.0, budget - food_local_cost);
			const double shortfall = std::max(0.0, food_import_cost - own_left);
			food_topup_import = std::min(shortfall,
				std::max(0.0, city.government_budget));
			if (food_topup_import > 0.0) {
				// Fica com as famílias: a importação é paga na fase 6 com
				// `pop.savings` quando firma e governo não cobrem.
				budget += food_topup_import;
				city.government_budget -= food_topup_import;
				pop.transfers += food_topup_import;
			}
		}

		// Consumo (B3): primeiro o estoque local, proporcional à disponibilidade
		// (toneladas-equivalentes de grão); o que faltar é comprado na fase 6
		// (comércio), sempre pelo alimento mais barato por caloria. A demanda
		// desejada é registrada mesmo sem estoque (sinal de escassez para a IA e
		// para os preços); a compra realizada é limitada por estoque e orçamento.
		double consumed = 0.0;      // toneladas-equivalentes de grão
		double food_spend = 0.0;
		double total_avail = 0.0;
		std::vector<double> avail_ge(ws_.food_products.size(), 0.0);
		for (int slot = 0; slot < static_cast<int>(ws_.food_products.size()); ++slot) {
			const int product = demo_food_product(slot);
			avail_ge[slot] = city.markets[product].stock * demo_food_calories(product);
			total_avail += avail_ge[slot];
		}
		double remaining_need = pop.need;
		for (int slot = 0; slot < static_cast<int>(ws_.food_products.size()) && remaining_need > 1e-12; ++slot) {
			const int product = demo_food_product(slot);
			const double factor = demo_food_calories(product);
			if (factor <= 0.0) {
				continue;
			}
			DemoMarket& market = city.markets[product];
			const double price = market.price;
			if (price <= 0.0) {
				continue;
			}
			// Desejo proporcional à disponibilidade; sem estoque nenhum, todo o
			// desejo recai sobre o primeiro alimento da cesta (trigo — o mais
			// barato por caloria na base), para manter o sinal de escassez.
			double want_ge = total_avail > 0.0
				? pop.need * (avail_ge[slot] / total_avail)
				: (slot == 0 ? pop.need : 0.0);
			want_ge = std::min(want_ge, remaining_need);
			double desired_tons = want_ge / factor;
			if (desired_tons * price > budget) {
				desired_tons = std::max(0.0, budget) / price;
			}
			if (desired_tons <= 1e-12) {
				continue;
			}
			market.desired += desired_tons;
			pop.desired_by_category[slot] += desired_tons;
			const double quantity = std::min(desired_tons, market.stock);
			if (quantity <= 0.0) {
				continue;
			}
			const double paid = quantity * price;
			market.stock -= quantity;
			market.demand += quantity;
			market.sold += quantity;
			budget -= paid;
			food_spend += paid;
			consumed += quantity * factor;
			pop.consumed_by_category[slot] += quantity;
			city.revenue_by_product[product] += paid;
			city.firm_cash += paid;
			ws_.consumed_total[product] += quantity;
			ws_.consumed_today[product] += quantity;
			remaining_need -= quantity * factor;
		}
		// F9 — PISO DE SUBSISTÊNCIA AGRÍCOLA (autoconsumo em espécie). Só age
		// quando a cidade NÃO conseguiu cobrir a cesta com o que comprou: as
		// famílias agrícolas comem da própria produção (agricultura familiar),
		// limitado pela necessidade dessas famílias e por uma fração da produção
		// local. O alimento sai do ESTOQUE (conservação): não é comprado, então
		// não há pagamento nem criação de comida. Sem déficit, não faz nada.
		double in_kind = 0.0;
		if (remaining_need > 1e-9 && city.agri_output_tons > 0.0
				&& city.sector_workers[0] > 0.0) {
			const double farm_household_size = std::clamp(
				city.simulation_population
					/ std::max(city.pop.labor_force, 1e-9), 1.0, 6.0);
			const double household_need = city.sector_workers[0]
				* farm_household_size * kDemoNeedPerCapita;
			const double own_production = city.agri_output_tons
				* ws_.agri_own_consumption_share;
			in_kind = std::min(
				std::min(remaining_need, household_need), own_production);
			double left = in_kind;
			for (int slot = 0; slot < static_cast<int>(ws_.food_products.size())
					&& left > 1e-12; ++slot) {
				const int product = demo_food_product(slot);
				const double factor = demo_food_calories(product);
				if (factor <= 0.0) {
					continue;
				}
				DemoMarket& market = city.markets[product];
				const double available = std::max(0.0, market.stock * factor);
				const double take = std::min(left, available);
				if (take <= 1e-12) {
					continue;
				}
				const double tons = take / factor;
				market.stock = std::max(0.0, market.stock - tons);
				pop.consumed_by_category[slot] += tons;
				ws_.consumed_total[product] += tons;
				ws_.consumed_today[product] += tons;
				consumed += take;
				left -= take;
			}
			in_kind -= left;
		}
		city.in_kind_food = in_kind;
		pop.consumed = consumed;
		pop.food_spend = food_spend;
		pop.satisfaction = pop.need > 0.0
			? std::clamp(consumed / pop.need, 0.0, 1.0) : 1.0;

		// Subsídio de subsistência LOCAL não gasto volta ao governo: sem isso, a
		// cidade sem estoque de alimento convertia o subsídio em poupança e ele
		// acabava financiando life/luxury (dinheiro conservado, mas fora do
		// destino). A parcela de IMPORTAÇÃO não volta: ela precisa continuar com
		// as famílias até a fase 6 pagar a carga.
		if (food_topup_local > 0.0) {
			const double unused = std::min(food_topup_local, std::max(0.0, budget));
			if (unused > 0.0) {
				budget -= unused;
				city.government_budget += unused;
				pop.transfers -= unused;
			}
		}

		// F3: necessidades das POPs em três níveis (basic/life/luxury). O
		// orçamento é sequencial: o alimento básico acima já foi pago; aqui
		// entram as demais categorias do basic, depois life e por último luxury
		// — o nível alcançado é consequência da renda disponível. Faltar
		// basic/food continua derrubando `pop.satisfaction` (fome); faltar
		// life/luxury derruba a satisfação do nível e a agregada.
		if (ws_.needs_loaded) {
			// Duráveis: descarte com perda de durable_replacement_per_year, na
			// taxa diária que perde exatamente r em 365 dias.
			for (int product = 0; product < ws_.products; ++product) {
				if (product < static_cast<int>(pop.durable_stock.size())
						&& product < static_cast<int>(ws_.product_durable.size())
						&& ws_.product_durable[static_cast<std::size_t>(product)] != 0) {
					pop.durable_stock[static_cast<std::size_t>(product)] =
						std::max(0.0,
							pop.durable_stock[static_cast<std::size_t>(product)]
							* (1.0 - ws_.durable_decay));
				}
			}
			// Reserva de alimento: a POP só compra life/luxury com a poupança
			// acima de kDemoFoodReserveDays de cesta. Isso preserva a capacidade
			// de importar comida em nações sem terra (o alimento continua sendo
			// a primeira prioridade do orçamento).
			double needs_budget = budget;
			if (food_unit_cost > 0.0) {
				const double reserve = pop.need * kDemoFoodReserveDays * food_unit_cost;
				// Protege no máximo a poupança existente: se ela já é menor que
				// o colchão, a renda corrente do dia continua disponível.
				const double protected_savings =
					std::min(std::max(0.0, pop.savings), reserve);
				needs_budget = std::max(0.0, budget - protected_savings);
			}
			double needs_spent = 0.0;
			double weighted = 0.0;
			double weight_sum = 0.0;
			for (double& level_spend : pop.level_spend) {
				level_spend = 0.0;
			}
			// População por CLASSE de consumo (basic/life/luxury; soma =
			// população da cidade). Com Gini declarado a classe vem da
			// distribuição de renda; sem Gini, espelha o estrato de
			// escolaridade (legado). É ela que define o desejo e o teto de
			// compra de cada nível.
			double class_total = 0.0;
			for (int cls = 0; cls < kDemoEducationLevels; ++cls) {
				class_total += std::max(0.0, pop.class_population[cls]);
			}
			for (int level_index = 0;
					level_index < static_cast<int>(ws_.needs_levels.size())
					&& level_index < kDemoMaxNeedsLevels; ++level_index) {
				const DemoNeedsLevel& level =
					ws_.needs_levels[static_cast<std::size_t>(level_index)];
				// F7: a renda define o nível de consumo POR ESTRATO. O básico é
				// sempre prioridade de todos; life/luxury só são comprados pelo
				// estrato cuja renda corrente cobre a cesta acumulada. Sem
				// renda, o desejo fica registrado e a satisfação cai — não se
				// inventa consumo com poupança.
				double level_want = 0.0;
				double level_got = 0.0;
				const double spend_before = needs_spent;
				if (level_index == 0) {
					pop.level_spend[0] += food_spend;
				}
				for (int cls = 0; cls < kDemoEducationLevels; ++cls) {
					const double class_pop =
						std::max(0.0, pop.class_population[cls]);
					if (class_pop <= 1e-9) {
						continue;
					}
					const double class_share = class_total > 1e-9
						? class_pop / class_total
						: 1.0 / double(kDemoEducationLevels);
					const bool affordable = level_index == 0
						|| level_index + 1 <= pop.class_income_level[cls];
					for (const DemoNeedsCategory& category : level.categories) {
						// A cesta alimentar básica tem laço próprio (validado) e
						// já foi consumida acima; reaproveita `pop.need/consumed`
						// para não cobrar duas vezes a mesma refeição.
						if (level_index == ws_.needs_food_level
								&& category.id == std::string("food")) {
							level_want += pop.need * class_share;
							level_got += pop.consumed * class_share;
							continue;
						}
						const double want_ge = category.per_capita_day
							* class_pop;
						level_want += want_ge;
						if (!affordable) {
							// Renda insuficiente: não compra (o consumo efetivo
							// respeita a renda), mas a NECESSIDADE entra no
							// mercado como desejo — o sinal de escassez da IA.
							demo_register_category_demand(city, category, want_ge);
							continue;
						}
						level_got += demo_consume_category(
							city, category, want_ge, needs_budget, needs_spent);
					}
				}
				pop.level_spend[level_index] += needs_spent - spend_before;
				pop.level_want[level_index] = level_want;
				pop.level_consumed[level_index] = level_got;
				const double satisfaction = level_want > 0.0
					? std::clamp(level_got / level_want, 0.0, 1.0) : 0.0;
				pop.level_satisfaction[level_index] = satisfaction;
				if (level_want > 0.0 && level.weight > 0.0) {
					weighted += satisfaction * level.weight;
					weight_sum += level.weight;
				}
			}
			pop.needs_satisfaction = weight_sum > 0.0
				? std::clamp(weighted / weight_sum, 0.0, 1.0) : 0.0;
			// F8: gancho de saúde privada. A cobertura (0..1) soma um pequeno
			// bônus de satisfação; o serviço público fica para a F9.
			pop.needs_satisfaction = std::clamp(pop.needs_satisfaction
				+ kDemoServiceHealthSatisfactionBonus * city.private_health_cover,
				0.0, 1.0);
			// Nível alcançado = maior nível com pelo menos metade da cesta,
			// exigindo os níveis anteriores (basic → life → luxury).
			pop.needs_level = 0;
			for (int level_index = 0;
					level_index < static_cast<int>(ws_.needs_levels.size())
					&& level_index < kDemoMaxNeedsLevels; ++level_index) {
				if (pop.level_satisfaction[level_index] >= 0.5) {
					pop.needs_level = level_index + 1;
				} else {
					break;
				}
			}
			pop.needs_spend = needs_spent;
			budget -= needs_spent;
			// F7: teto de consumo pela RENDA REAL de CADA ESTRATO — quanto da
			// cesta completa de cada nível a renda corrente do estrato cobre,
			// aos PREÇOS DE MERCADO de hoje. O nível vale a partir do dia
			// seguinte; é ele que libera life/luxury no orçamento do estrato.
			// Antes o custo vinha do gasto observado, que é zero quando o nível
			// já estava bloqueado: a cidade nunca saía do nível 1 (auto-bloqueio).
			// Preço de mercado existe mesmo sem compra e quebra o laço.
			// F7 calibração — GATE LEGADO por estrato. Continua sendo calculado
			// sempre (o campo `income_level_by_stratum` é lido pela UI/painel);
			// com Gini declarado ele NÃO define o consumo — a classe ancorada no
			// PIB PPP define (`demo_refresh_income_classes`).
			{
				double cumulative_cost[kDemoMaxNeedsLevels] = {};
				double running_cost = 0.0;
				// `demo_level_basket_cost` devolve o custo da CIDADE INTEIRA; o gate
				// de renda compara com a renda PER CAPITA do estrato, então o custo
				// é normalizado pela população.
				const double basket_population =
					std::max(1e-9, city.simulation_population);
				for (int level_index = 0;
						level_index < static_cast<int>(ws_.needs_levels.size())
						&& level_index < kDemoMaxNeedsLevels; ++level_index) {
					running_cost += demo_level_basket_cost(city, level_index);
					cumulative_cost[level_index] = running_cost / basket_population;
				}
				for (int stratum = 0; stratum < kDemoEducationLevels; ++stratum) {
					const double stratum_pop =
						std::max(0.0, pop.stratum_population[stratum]);
					const double per_capita = stratum_pop > 1e-9
						? std::max(0.0, pop.stratum_disposable[stratum]) / stratum_pop
						: 0.0;
					int income_level = 0;
					for (int level_index = 0;
							level_index < static_cast<int>(ws_.needs_levels.size())
							&& level_index < kDemoMaxNeedsLevels; ++level_index) {
						if (per_capita + 1e-12
								>= cumulative_cost[level_index]
									* ws_.income_basket_margin) {
							income_level = level_index + 1;
						} else {
							break;
						}
					}
					pop.stratum_income_level[stratum] = income_level;
				}
				// Sem Gini (legado), o gate de amanhã é o cap de classe; com
				// Gini, os caps vêm da distribuição e não são sobrescritos.
				if (city.income_gini <= 0.0) {
					demo_refresh_income_classes(city);
				}
			}
			// Agregado da cidade: o nível mediano (a maior parte da POP) — é o
			// que o mapa/UI e os testes legados leem — mais a fatia da POP em
			// cada nível, que é o número que separa os países no relatório.
			double population_seen = 0.0;
			int median_level = 0;
			bool median_found = false;
			const double city_population = city.simulation_population;
			for (int level_index = 0; level_index < kDemoIncomeLevels;
					++level_index) {
				pop.income_level_share[level_index] = 0.0;
			}
			for (int cls = 0; cls < kDemoEducationLevels; ++cls) {
				const int reached = pop.class_income_level[cls];
				if (reached >= 0 && reached < kDemoIncomeLevels) {
					pop.income_level_share[reached] +=
						std::max(0.0, pop.class_population[cls]);
				}
			}
			for (int level_index = 0; level_index < kDemoIncomeLevels;
					++level_index) {
				const double share = city_population > 1e-9
					? pop.income_level_share[level_index] / city_population : 0.0;
				pop.income_level_share[level_index] = std::clamp(share, 0.0, 1.0);
				if (!median_found) {
					population_seen += pop.income_level_share[level_index]
						* city_population;
					if (population_seen >= city_population * 0.5) {
						median_level = level_index;
						median_found = true;
					}
				}
			}
			pop.income_level = median_level;
		} else {
			// Sem needs.json (AGRIA/PORTUS e saves antigos): um único nível,
			// exatamente o comportamento validado.
			pop.needs_satisfaction = pop.satisfaction;
			pop.level_want[0] = pop.need;
			pop.level_consumed[0] = pop.consumed;
			pop.level_satisfaction[0] = pop.satisfaction;
			pop.needs_level = pop.satisfaction >= 0.5 ? 1 : 0;
			for (int level_index = 0; level_index < kDemoIncomeLevels;
					++level_index) {
				pop.income_level_share[level_index] = 0.0;
			}
			pop.income_level_share[pop.income_level] = 1.0;
		}
		// Consumo institucional do Estado (paper/computer) — decisão §12.10.
		demo_consume_government(city);
		pop.savings = budget;
	}

	OGS_SIM_PHASE_LAP(kDemoPhaseIncome);
}

void SimCore::phase_services() {
	for (DemoCity& city : ws_.cities) {
		demo_services_day(city);
	}

	OGS_SIM_PHASE_LAP(kDemoPhaseServices);
}

void SimCore::phase_accounting() {
	// 7. Receita por construção, lucro, distribuição de capital e PIB.
	for (std::size_t city_index = 0; city_index < ws_.cities.size(); ++city_index) {
		DemoCity& city = ws_.cities[city_index];
		const std::vector<int>& local_buildings =
			ws_.buildings_by_city[city_index];
		std::array<double, kDemoMaxProducts> capacity_by_output{};
		for (const int bi : local_buildings) {
			const DemoBuilding& building =
				ws_.buildings[static_cast<std::size_t>(bi)];
			capacity_by_output[building.output_product] +=
				building.capacity * building.output_scale;
		}
		for (const int bi : local_buildings) {
			DemoBuilding& building =
				ws_.buildings[static_cast<std::size_t>(bi)];
			const double effective = building.capacity * building.output_scale;
			const double share = capacity_by_output[building.output_product] > 0.0
				? effective / capacity_by_output[building.output_product] : 0.0;
			building.revenue = city.revenue_by_product[building.output_product] * share;
			building.costs = building.paid_wages + building.paid_opex
				+ building.input_value;
			building.profit = building.revenue - building.costs;
		}
		// Planta estratégica (bootstrap industrial §12.3): enquanto a demanda
		// final não existe (F3), o governo cobre o prejuízo da planta de
		// referência em vez de deixá-la fechar. Transferência explícita
		// governo -> firma: conserva o dinheiro e não maquia o resultado do
		// setor (a perda continua visível em `profit` antes do subsídio).
		for (const int bi : local_buildings) {
			DemoBuilding& building =
				ws_.buildings[static_cast<std::size_t>(bi)];
			if (!building.strategic || building.profit >= 0.0) {
				continue;
			}
			const double subsidy = std::min(-building.profit,
				std::max(0.0, city.government_budget));
			if (subsidy > 0.0) {
				city.government_budget -= subsidy;
				city.firm_cash += subsidy;
				building.profit += subsidy;
			}
		}

		// A receita de serviços inclui consumo das famílias, opex empresarial,
		// frete e construção: são serviços domésticos, não importações. F8 soma
		// a exportação de serviços (paga pelo setor externo) e a compra do
		// governo (transferência do orçamento); os insumos comprados pelos
		// negócios reduzem o lucro do setor (o dinheiro foi para as firmas).
		city.service_revenue = city.pop.service_spend + city.opex_revenue
			+ city.freight_revenue + city.construction_revenue
			+ city.service_export_revenue + city.service_government_revenue;
		city.service_profit = city.service_revenue - city.service_wage_bill
			- city.service_input_cost;

		double private_profit = 0.0;
		double state_profit = 0.0;
		double value_added = 0.0;
		// F9: lucro e faturamento por grupo presente na cidade. O JV divide a
		// linha do ativo entre o grupo estrangeiro e o parceiro nacional.
		std::vector<std::array<double, 3>> group_rows;  // índice, lucro, receita
		auto add_group_row = [&group_rows](int group_index, double profit,
				double revenue) {
			for (std::array<double, 3>& row : group_rows) {
				if (static_cast<int>(row[0]) == group_index) {
					row[1] += profit;
					row[2] += revenue;
					return;
				}
			}
			group_rows.push_back({static_cast<double>(group_index), profit, revenue});
		};
		for (const int bi : local_buildings) {
			const DemoBuilding& building =
				ws_.buildings[static_cast<std::size_t>(bi)];
			if (building.owner == "state") {
				state_profit += building.profit;
			} else {
				std::string group_id;
				std::string origin;
				const int owner_kind = demo_owner_kind(building.owner, &group_id,
					&origin);
				const int group_index = owner_kind == 1 || owner_kind == 2
					? demo_group_index(group_id) : -1;
				if (group_index >= 0) {
					const double local_share = std::clamp(building.local_share, 0.0, 1.0);
					const int partner_index = building.partner_group.empty()
						? -1 : demo_group_index(building.partner_group);
					if (local_share > 0.0 && partner_index >= 0) {
						add_group_row(group_index, building.profit * (1.0 - local_share),
							building.revenue * (1.0 - local_share));
						add_group_row(partner_index, building.profit * local_share,
							building.revenue * local_share);
					} else {
						add_group_row(group_index, building.profit, building.revenue);
					}
				} else {
					// Dono de grupo desconhecido (save antigo/editado): cai no
					// privado agregado, sem criar dinheiro.
					private_profit += building.profit;
				}
			}
			value_added += building.revenue - building.input_value;
		}

		// Dividendos só depois de reconstituir o capital de giro. Lucros de
		// estatais vão para o orçamento público; privados, para as famílias.
		const double firm_target =
			city.simulation_population * kDemoFirmCashPerCapita;
		double firm_surplus = std::max(0.0, city.firm_cash - firm_target);
		// F9: o lucro dos ativos de grupo sai da firma para o grupo (imposto
		// corporativo fica no anfitrião; o resto vira caixa do grupo e, no
		// fechamento do dia, dividendo ao estrato dono). A retenção é o que
		// financia o investimento seguinte; nada é criado ou destruído.
		for (const std::array<double, 3>& row : group_rows) {
			demo_route_group_profit(static_cast<int>(row[0]), city.nation,
				row[1], row[2], city, firm_surplus);
		}
		firm_surplus = std::max(0.0, city.firm_cash - firm_target);
		const double positive_total = std::max(0.0, private_profit)
			+ std::max(0.0, state_profit);
		const double distributable = std::min(
			positive_total * kDemoCapitalDistribution, firm_surplus);
		const double private_share = positive_total > 0.0
			? std::max(0.0, private_profit) / positive_total : 0.0;
		const double modeled_distribution = distributable * private_share;
		const double state_distribution = distributable - modeled_distribution;
		const double service_target =
			(city.service_wage_bill + city.service_input_cost)
			* kDemoServiceCashBufferDays;
		const double service_surplus = std::max(0.0,
			city.service_cash - service_target);
		// F8: o lucro do setor é renda da POP dona (plano §13). O caixa guarda
		// só o capital de giro (dias de folha + insumos); o resto do lucro é
		// distribuído no dia seguinte. O alvo antigo por habitante (0,05/pessoa)
		// quase nunca era alcançado e retinha o lucro para sempre: a cidade
		// pagava serviços sem receber o lucro de volta e esgotava a poupança
		// que compra comida (SDN-KHA, SOL-HAR em 365 d, F8).
		const double service_distribution = std::min(
			std::max(0.0, city.service_profit * kDemoCapitalDistribution),
			service_surplus);
		city.firm_cash -= modeled_distribution + state_distribution;
		city.service_cash -= service_distribution;
		city.government_budget += state_distribution;
		city.capital_income_next = modeled_distribution + service_distribution;
		city.gdp = value_added + city.service_revenue;

		// Comportamento das empresas: prejuízo persistente reduz a produção;
		// demanda acima da capacidade com lucro permite expansão.
		for (const int bi : local_buildings) {
			DemoBuilding& building =
				ws_.buildings[static_cast<std::size_t>(bi)];
			// Planta secundária ociosa não é insolvente: sem operação no dia,
			// não acumula dias de prejuízo (o custo de stand-by já é pequeno).
			const bool operating = building.recipe < 0 && building.input_product < 0
				? true : building.utilization > 1e-9;
			if (building.profit < 0.0 && operating) {
				++building.loss_days;
			} else {
				building.loss_days = 0;
			}
			// Falência: prejuízo persistente além do corte fecha a firma. A
			// regra é explícita — a construção some (capacidade e empregos), em
			// vez de ficar produzindo no piso no vermelho para sempre. Planta
			// estratégica é mantida pelo governo (ver subsídio acima).
			if (building.loss_days >= kDemoLossDaysBeforeClose && !building.strategic) {
				building.bankrupt = true;
			}
			if (building.loss_days >= kDemoLossDaysBeforeCut) {
				building.output_scale = std::max(kDemoMinOutputScale,
					building.output_scale * kDemoLossCut);
			} else if (building.profit > 0.0) {
				// Recuperação gradual após um período de prejuízo.
				building.output_scale = std::min(1.0, building.output_scale * 1.02);
			}
		}

		// F7: salários por nível reagem ao fechamento do dia — aperto do
		// mercado (vagas × trabalhadores), margem das firmas e âncora de renda
		// do país, com rigidez. O prêmio por educação emerge da escassez.
		double building_revenue = 0.0;
		double building_profit = 0.0;
		for (const int bi : local_buildings) {
			const DemoBuilding& building =
				ws_.buildings[static_cast<std::size_t>(bi)];
			building_revenue += building.revenue;
			building_profit += building.profit;
		}
		const double firm_margin = building_revenue > 1e-9
			? std::clamp(building_profit / building_revenue, -0.25, 0.25) : 0.0;
		demo_update_wages(city, firm_margin);

		// O salário de serviços se ajusta ao mercado de trabalho residual e o
		// emprego segue a receita do setor ao novo salário. Se a receita não
		// sustenta nem o salário de subsistência, sobra desemprego de verdade —
		// não um piso artificial de 5% de vagas.
		double cheapest_food = 0.0;
		for (int slot = 0; slot < static_cast<int>(ws_.food_products.size()); ++slot) {
			const int product = demo_food_product(slot);
			const double factor = demo_food_calories(product);
			if (factor <= 0.0) {
				continue;
			}
			const double per_calorie = city.markets[product].price / factor;
			if (per_calorie > 0.0
					&& (cheapest_food <= 0.0 || per_calorie < cheapest_food)) {
				cheapest_food = per_calorie;
			}
		}
		const double subsistence_wage = kDemoNeedPerCapita * cheapest_food;
		double modeled_jobs_total = 0.0;
		for (const int bi : local_buildings) {
			modeled_jobs_total += ws_.buildings[static_cast<std::size_t>(bi)].jobs;
		}
		const double residual_labor = std::max(0.0,
			city.pop.labor_force - modeled_jobs_total);
		const double service_supply = residual_labor
			* (1.0 - ws_.natural_unemployment);
		// F7: quem define o salário do setor de serviços é o MERCADO DE TRABALHO
		// (demo_labor_day grava aqui a média ponderada por nível/mistura). A
		// antiga regra de "receita ÷ força residual" brigava com ela e travava o
		// mundo em ~15% de desemprego. A receita continua limitando as VAGAS: se
		// ela não paga nem o salário de subsistência, sobra desemprego de
		// verdade — não um piso artificial de vagas.
		// F7 calibração: o setor de serviços é o empregador RESIDUAL do modelo
		// (serviços urbanos absorvem quem não está nas construções). Quem limita
		// a FOLHA é o caixa do setor (liquidez, em demo_labor_day), não o número
		// de vagas: cortar vagas pela receita transformava um salário-alvo alto
		// em desemprego em massa (1ª medição da calibração: EUA 41,8%, DEU
		// 48,8%), porque a receita de serviços é uma fração da renda das
		// famílias e não acompanha a parcela do trabalho no valor adicionado.
		(void)subsistence_wage;
		city.service_jobs += (service_supply - city.service_jobs)
			* kDemoServiceAdjust;
	}

	OGS_SIM_PHASE_LAP(kDemoPhaseAccounting);
}

void SimCore::phase_fiscal() {
	// Equalização fiscal per capita dentro de cada nação: o orçamento público é
	// repartido entre as cidades proporcionalmente à população. Uma cidade rica
	// financia a segurança alimentar de uma cidade pobre da mesma nação (ex.:
	// Paris sustenta Cayenne). Conserva dinheiro — só move government_budget
	// entre cidades da MESMA nação.
	std::unordered_map<std::string, std::vector<DemoCity*>> by_nation;
	by_nation.reserve(ws_.cities.size() / 4 + 8);
	for (DemoCity& city : ws_.cities) {
		by_nation[city.nation].push_back(&city);
	}
	// Cada nação é independente: a ordem de varredura não muda o resultado.
	for (auto& entry : by_nation) {
		std::vector<DemoCity*>& cities = entry.second;
		double total_budget = 0.0;
		double total_population = 0.0;
		for (DemoCity* city : cities) {
			total_budget += city->government_budget;
			total_population += city->simulation_population;
		}
		if (cities.size() < 2 || total_population <= 0.0) {
			continue;
		}
		for (DemoCity* city : cities) {
			city->government_budget =
				total_budget * (city->simulation_population / total_population);
		}
	}
	if (ws_.groups.empty()) {
		// Sem grupos não há dividendo, mas a fase continua cronometrada.
		OGS_SIM_PHASE_LAP(kDemoPhaseFiscal);
		return;
	}
	// Recuperação lenta da confiança depois do prazo da nacionalização.
	for (auto it = ws_.confidence_penalty_day.begin();
			it != ws_.confidence_penalty_day.end();) {
		if (ws_.day >= it->second) {
			auto confidence = ws_.confidence.find(it->first);
			if (confidence != ws_.confidence.end()) {
				confidence->second = std::min(1.0, confidence->second + 0.005);
			}
			if (confidence == ws_.confidence.end()
					|| confidence->second >= 1.0) {
				it = ws_.confidence_penalty_day.erase(it);
				continue;
			}
		}
		++it;
	}
	for (std::size_t index = 0; index < ws_.groups.size(); ++index) {
		DemoGroup& group = ws_.groups[index];
		if (group.state_owned || group.distributable <= 1e-9) {
			continue;
		}
		// Dividendo ao estrato dono: fração do lucro acumulado. O que sobra
		// fica como capital para o próximo investimento.
		double payable = group.distributable * kDemoGroupDividendShare;
		payable = std::min(payable, group.cash);
		if (payable <= 1e-9) {
			continue;
		}
		double host_total = 0.0;
		for (const auto& row : group.profit_by_host) {
			host_total += std::max(0.0, row.second);
		}
		if (host_total <= 1e-9) {
			const double credited = demo_credit_nation_households(
				group.origin_nation, payable);
			if (credited > 0.0) {
				group.cash -= credited;
				group.distributable = std::max(0.0, group.distributable - credited);
				group.dividends_total += credited;
				ws_.group_flows[group.origin_nation].dividends += credited;
			}
			continue;
		}
		double credited_total = 0.0;
		std::vector<std::string> exhausted;
		for (auto& row : group.profit_by_host) {
			const double share = payable * std::max(0.0, row.second) / host_total;
			if (share <= 1e-9) {
				continue;
			}
			const bool domestic = row.first == group.origin_nation;
			const double credited = demo_credit_nation_households(
				group.origin_nation, share);
			if (credited <= 0.0) {
				continue;
			}
			credited_total += credited;
			if (domestic) {
				ws_.group_flows[row.first].dividends += credited;
			} else {
				// Repatriação: sai do anfitrião, entra na origem.
				ws_.group_flows[row.first].repatriated_out += credited;
				ws_.group_flows[group.origin_nation].repatriated_in += credited;
			}
			row.second -= share;
			if (row.second <= 1e-9) {
				exhausted.push_back(row.first);
			}
		}
		for (const std::string& host : exhausted) {
			group.profit_by_host.erase(host);
		}
		if (credited_total > 0.0) {
			group.cash -= credited_total;
			group.distributable = std::max(0.0,
				group.distributable - credited_total);
			group.dividends_total += credited_total;
		}
	}

	OGS_SIM_PHASE_LAP(kDemoPhaseFiscal);
}

void SimCore::phase_fx() {
	// 9c. Câmbio dinâmico: a balança comercial do dia move as moedas float
	// dentro da banda (déficit desvaloriza, superávit valoriza). Não cria nem
	// destrói dinheiro: só muda a taxa usada na próxima decisão de compra.
	demo_tick_fx();

	OGS_SIM_PHASE_LAP(kDemoPhaseFx);
}

void SimCore::phase_invariants() {
	// 10. Invariantes: se o tick corrompeu o estado (dinheiro não conservado,
	// caixa/estoque negativo ou preço fora da banda), registra e reverte.
	ws_.invariant_violations = demo_check_invariants(money_before());
	if (!ws_.invariant_violations.empty()) {
		demo_restore(ws_.snapshot_buffer);
	}
	OGS_SIM_PHASE_LAP(kDemoPhaseInvariants);
}

void SimCore::demo_restore(const DemoSnapshot& in) {
	ws_.cities = in.cities;
	ws_.buildings = in.buildings;
	ws_.service_businesses = in.service_businesses;
	ws_.next_service_business_id = in.next_service_business_id;
	ws_.service_bankruptcies = in.service_bankruptcies;
	ws_.in_transit = in.transit;
	ws_.fleets = in.fleets;
	ws_.groups = in.groups;
	ws_.confidence = in.confidence;
	ws_.group_flows = in.group_flows;
	// F5: o drift cambial roda antes do invariante; desfazer um tick
	// corrompido também desfaz a taxa do dia.
	ws_.currencies = in.currencies;
	demo_rebuild_currency_indexes();
	ws_.transport_cash = in.transport_cash;
	ws_.foreign_cash = in.foreign_cash;
	ws_.construction_cash = in.construction_cash;
	for (int index = 0; index < ws_.products; ++index) {
		ws_.produced[index] = in.produced[index];
		ws_.consumed_total[index] = in.consumed_total[index];
		ws_.produced_today[index] = in.produced_today[index];
		ws_.consumed_today[index] = in.consumed_today[index];
	}
	ws_.day = in.day;
	demo_rebuild_indexes();
	demo_rebuild_service_indexes();
	// F9: o índice de grupos acompanha o vetor restaurado.
	ws_.group_lookup.clear();
	for (std::size_t index = 0; index < ws_.groups.size(); ++index) {
		ws_.group_lookup[ws_.groups[index].id] = static_cast<int>(index);
	}
}

std::vector<std::string> SimCore::demo_check_invariants(double money_before) const {
	std::vector<std::string> violations;

	// 1. Conservação de dinheiro: toda transação move dinheiro entre caixas
	// (firma, serviço, governo, poupança, transporte, exterior, construção).
	// O resto do mundo (foreign_cash) pode ficar negativo por design.
	const double money_after = demo_money_total();
	const double drift = std::abs(money_after - money_before);
	const double tolerance = std::max(1e-3, std::abs(money_before) * 1e-12);
	if (!std::isfinite(money_after) || drift > tolerance) {
		violations.push_back("dinheiro não conservado: antes="
			+ std::to_string(money_before) + " depois="
			+ std::to_string(money_after) + " drift=" + std::to_string(drift));
	}

	for (const DemoCity& city : ws_.cities) {
		auto check_nonneg = [&violations, &city](const char* label, double value) {
			if (!std::isfinite(value) || value < -1e-6) {
				violations.push_back(std::string("caixa negativo (") + label + ") em "
					+ city.id + ": " + std::to_string(value));
			}
		};
		check_nonneg("firm", city.firm_cash);
		check_nonneg("service", city.service_cash);
		check_nonneg("government", city.government_budget);
		check_nonneg("savings", city.pop.savings);

		// Preço na âncora: o câmbio não move mais o preço interno (só a
		// competitividade das ofertas float e a exibição local), então a banda
		// pode ser comparada sem folga.
		for (int product = 0; product < ws_.products; ++product) {
			const DemoMarket& market = city.markets[product];
			const double base = demo_base_price(product);
			const double floor_price = base * 0.5;
			const double ceiling = base * 3.0;
			if (!std::isfinite(market.price)
					|| market.price < floor_price
					|| market.price > ceiling) {
				violations.push_back("preço fora da banda em " + city.id + " "
					+ demo_product_name(product) + ": " + std::to_string(market.price)
					+ " (base âncora " + std::to_string(base) + ", moeda "
					+ demo_currency_code(city.nation) + " taxa "
					+ std::to_string(demo_fx_rate(city.nation)) + ")");
			}
			if (!std::isfinite(market.stock) || market.stock < -1e-6) {
				violations.push_back("estoque negativo em " + city.id + " "
					+ demo_product_name(product) + ": " + std::to_string(market.stock));
			}
		}
	}

	auto check_global = [&violations](const char* label, double value) {
		if (!std::isfinite(value) || value < -1e-6) {
			violations.push_back(std::string("caixa global negativo (") + label
				+ "): " + std::to_string(value));
		}
	};
	check_global("transport", ws_.transport_cash);
	check_global("construction", ws_.construction_cash);
	// F9: caixa de grupo é dinheiro do mundo; não pode ser negativo nem NaN.
	for (const DemoGroup& group : ws_.groups) {
		if (!std::isfinite(group.cash) || group.cash < -1e-6) {
			violations.push_back("caixa de grupo inválido (" + group.id + "): "
				+ std::to_string(group.cash));
		}
	}

	return violations;
}

double SimCore::demo_level_basket_cost(const DemoCity& city,
		int level_index) const {
	if (level_index < 0
			|| level_index >= static_cast<int>(ws_.needs_levels.size())) {
		return 0.0;
	}
	const DemoNeedsLevel& level =
		ws_.needs_levels[static_cast<std::size_t>(level_index)];
	const double population = std::max(0.0, city.simulation_population);
	double cost = 0.0;
	for (const DemoNeedsCategory& category : level.categories) {
		double want = category.per_capita_day * population;
		if (want <= 0.0) {
			continue;
		}
		// Duráveis comprados com poupança: a quantidade declarada é a posse e o
		// fluxo diário de renda precisa cobrir apenas a REPOSIÇÃO anual
		// (`durable_replacement_per_year`), não uma compra por mês. Sem isso o
		// telefone (0,000225 t/mês) custava ~1/3 de toda a renda diária e
		// travava life/luxury em qualquer nação.
		const double replacement = std::clamp(ws_.durable_decay * 365.0, 0.0, 0.99);
		double cheapest = 0.0;  // preço por tonelada-equivalente
		bool has_durable = false;
		double cheapest_durable = 0.0;
		for (std::size_t slot = 0; slot < category.substitutes.size(); ++slot) {
			const int product = category.substitutes[slot];
			if (product < 0 || product >= ws_.products) {
				continue;
			}
			const double factor = category.equivalence[slot];
			if (factor <= 0.0) {
				continue;
			}
			const double price = city.markets[product].price;
			if (price <= 0.0) {
				continue;
			}
			const double per_ge = price / factor;
			const bool durable =
				product < static_cast<int>(ws_.product_durable.size())
				&& ws_.product_durable[static_cast<std::size_t>(product)] != 0;
			if (durable) {
				if (!has_durable || per_ge < cheapest_durable) {
					has_durable = true;
					cheapest_durable = per_ge;
				}
				continue;
			}
			if (cheapest <= 0.0 || per_ge < cheapest) {
				cheapest = per_ge;
			}
		}
		if (cheapest > 0.0) {
			cost += want * cheapest;
		} else if (has_durable) {
			// Só há substituto durável: cobra a reposição, não a posse inteira.
			cost += want * (replacement / 12.0) * cheapest_durable;
		}
	}
	return cost;
}

void SimCore::demo_food_basket_cost(const DemoCity& city, double need,
		double& local_cost, double& import_cost, double& per_ge) const {
	local_cost = 0.0;
	import_cost = 0.0;
	per_ge = 0.0;
	const int slots = std::min(
		static_cast<int>(ws_.food_products.size()), kDemoMaxFoodProducts);
	if (slots <= 0) {
		return;
	}
	// Uma passada: guarda a disponibilidade e o preço por tonelada-equivalente
	// de cada slot (evita repetir a busca de fator calórico, que é O(slots)).
	double stock_ge[kDemoMaxFoodProducts];
	double unit_price[kDemoMaxFoodProducts];
	double total_avail = 0.0;
	double cheapest = 0.0;
	double cheapest_import = 0.0;
	for (int slot = 0; slot < slots; ++slot) {
		const int product = demo_food_product(slot);
		const double factor = demo_food_calories(product);
		stock_ge[slot] = 0.0;
		unit_price[slot] = 0.0;
		if (factor <= 0.0) {
			continue;
		}
		stock_ge[slot] = std::max(0.0, city.markets[product].stock) * factor;
		total_avail += stock_ge[slot];
		const double price = city.markets[product].price;
		if (price > 0.0) {
			unit_price[slot] = price / factor;
			if (cheapest <= 0.0 || unit_price[slot] < cheapest) {
				cheapest = unit_price[slot];
			}
		}
		// CIF conhecido: origem efetiva + tarifa (sem o frete, que é serviço
		// doméstico do comprador). É o preço que o importador realmente paga.
		const double cif = city.import_price[product];
		if (cif > 0.0) {
			const double unit = cif / factor;
			if (cheapest_import <= 0.0 || unit < cheapest_import) {
				cheapest_import = unit;
			}
		}
	}
	if (cheapest_import <= 0.0) {
		cheapest_import = cheapest;
	}
	if (cheapest <= 0.0) {
		cheapest = cheapest_import;
	}
	if (cheapest <= 0.0) {
		return;
	}
	if (total_avail <= 0.0 || need <= 0.0) {
		import_cost = std::max(0.0, need) * cheapest_import;
		per_ge = cheapest_import;
		return;
	}
	// Mesma proporção de disponibilidade usada na compra (B3). O custo é o da
	// cesta que a cidade consegue montar hoje; o que faltar para a necessidade
	// é importado ao CIF conhecido.
	double covered = 0.0;
	for (int slot = 0; slot < slots; ++slot) {
		if (stock_ge[slot] <= 0.0 || unit_price[slot] <= 0.0) {
			continue;
		}
		const double want = std::min(need, need * (stock_ge[slot] / total_avail));
		local_cost += want * unit_price[slot];
		covered += want;
	}
	const double residual = std::max(0.0, need - covered);
	import_cost = residual * cheapest_import;
	per_ge = (local_cost + import_cost) / need;
}

bool SimCore::demo_category_top_market(DemoCity& city,
		const DemoNeedsCategory& category, DemoMarket*& market, double& factor,
		double& durable_scale) {
	// Substituto PREFERIDO da categoria: o primeiro com preço de mercado e
	// equivalência válidas. É nele que o desejo da categoria é registrado como
	// sinal de mercado (a cesta tenta os substitutos nesta ordem).
	for (std::size_t slot = 0; slot < category.substitutes.size(); ++slot) {
		const int product = category.substitutes[slot];
		if (product < 0 || product >= ws_.products) {
			continue;
		}
		const double slot_factor = category.equivalence[slot];
		if (slot_factor <= 0.0 || city.markets[product].price <= 0.0) {
			continue;
		}
		const bool durable =
			product < static_cast<int>(ws_.product_durable.size())
			&& ws_.product_durable[static_cast<std::size_t>(product)] != 0;
		market = &city.markets[product];
		factor = slot_factor;
		durable_scale = durable
			? std::clamp(ws_.durable_decay * 365.0 / 12.0, 0.0, 1.0) : 1.0;
		return true;
	}
	return false;
}

void SimCore::demo_register_category_demand(DemoCity& city,
		const DemoNeedsCategory& category, double want_ge) {
	if (want_ge <= 0.0) {
		return;
	}
	DemoMarket* market = nullptr;
	double factor = 0.0;
	double durable_scale = 1.0;
	if (!demo_category_top_market(city, category, market, factor, durable_scale)) {
		return;
	}
	if (durable_scale <= 0.0) {
		return;
	}
	// Desejo declarado do tamanho da NECESSIDADE, independentemente de renda
	// ou estoque: é o sinal que a IA lê (`market.shortage`) para abrir a
	// cadeia do bem final. A compra efetiva fica na demo_consume_category e
	// respeita orçamento e estoque; nada é comprado aqui. Como não há renda
	// por trás, o valor entra também em `latent`: a IA vê a escassez, mas o
	// preço não sobe por desejo sem poder de compra.
	const double desired_tons = want_ge / factor * durable_scale;
	market->desired += desired_tons;
	market->latent += desired_tons;
}

double SimCore::demo_consume_category(DemoCity& city,
		const DemoNeedsCategory& category, double want_ge, double& budget,
		double& spend) {
	if (want_ge <= 0.0 || category.substitutes.empty()) {
		return 0.0;
	}
	// Demanda DESEJADA da categoria: registrada pelo tamanho do desejo no
	// substituto preferido, não pelo estoque. É o sinal de escassez que a IA
	// lê; a compra abaixo é que respeita caixa e estoque. A parcela que o
	// orçamento do dia não cobre entra em `latent` e não pressiona o preço.
	{
		DemoMarket* signal_market = nullptr;
		double signal_factor = 0.0;
		double signal_scale = 1.0;
		if (demo_category_top_market(city, category, signal_market,
				signal_factor, signal_scale) && signal_scale > 0.0) {
			const double full_tons = want_ge / signal_factor * signal_scale;
			const double backed_tons = std::min(full_tons,
				signal_market->price > 0.0
					? std::max(0.0, budget) / signal_market->price : 0.0);
			signal_market->desired += full_tons;
			signal_market->latent += std::max(0.0, full_tons - backed_tons);
		}
	}
	double remaining = want_ge;
	double consumed_ge = 0.0;
	for (std::size_t slot = 0;
			slot < category.substitutes.size() && remaining > 1e-12; ++slot) {
		const int product = category.substitutes[slot];
		if (product < 0 || product >= ws_.products) {
			continue;
		}
		const double factor = category.equivalence[slot];
		if (factor <= 0.0) {
			continue;
		}
		DemoMarket& market = city.markets[product];
		const double price = market.price;
		if (price <= 0.0) {
			continue;
		}
		// Durável: a posse declarada custa um fluxo de REPOSIÇÃO anual, não uma
		// compra por mês (um telefone por pessoa/mês drenava toda a renda).
		const bool durable =
			product < static_cast<int>(ws_.product_durable.size())
			&& ws_.product_durable[static_cast<std::size_t>(product)] != 0;
		const double durable_scale = durable
			? std::clamp(ws_.durable_decay * 365.0 / 12.0, 0.0, 1.0) : 1.0;
		double desired_tons = durable_scale > 0.0
			? remaining / factor * durable_scale : 0.0;
		// Compra efetiva: o desejo limitado pelo estoque e pelo orçamento do dia.
		double quantity = std::min(desired_tons, std::max(0.0, market.stock));
		if (quantity * price > budget) {
			quantity = std::max(0.0, budget) / price;
		}
		if (quantity <= 1e-12) {
			continue;
		}
		const double paid = quantity * price;
		market.stock -= quantity;
		market.demand += quantity;
		market.sold += quantity;
		budget -= paid;
		spend += paid;
		city.revenue_by_product[product] += paid;
		city.firm_cash += paid;
		ws_.consumed_total[product] += quantity;
		ws_.consumed_today[product] += quantity;
		const double got_ge = durable_scale > 0.0
			? quantity * factor / durable_scale : 0.0;
		consumed_ge += got_ge;
		remaining = std::max(0.0, remaining - got_ge);
		if (durable) {
			if (product < static_cast<int>(city.pop.durable_stock.size())) {
				city.pop.durable_stock[static_cast<std::size_t>(product)] += quantity;
			}
			city.pop.durable_spend += paid;
		}
	}
	return consumed_ge;
}

void SimCore::demo_consume_government(DemoCity& city) {
	if (!ws_.needs_loaded || ws_.government_products.empty()) {
		return;
	}
	// Teto prudencial: a compra institucional não pode consumir mais que uma
	// fração do valor da cesta alimentar diária da cidade. Sem o teto, o
	// computer (preço alto) drenava o orçamento que importa comida em nações
	// sem terra (EGY 1,000 → 0,708 em 365 d).
	double cheapest_food = 0.0;
	for (int slot = 0; slot < static_cast<int>(ws_.food_products.size()); ++slot) {
		const int product = demo_food_product(slot);
		const double factor = demo_food_calories(product);
		if (factor <= 0.0) {
			continue;
		}
		const double per_calorie = city.markets[product].price / factor;
		if (per_calorie > 0.0
				&& (cheapest_food <= 0.0 || per_calorie < cheapest_food)) {
			cheapest_food = per_calorie;
		}
	}
	double budget = std::max(0.0, city.government_budget);
	if (cheapest_food > 0.0) {
		const double food_value = city.simulation_population
			* kDemoNeedPerCapita * cheapest_food;
		budget = std::min(budget,
			food_value * kDemoGovernmentGoodsFoodShare);
	} else {
		budget = std::min(budget, budget * 0.01);
	}
	// Divisão igual entre os bens institucionais: o computer (caro) não pode
	// consumir a cota inteira e deixar o paper sem compra.
	const double per_product_budget = budget / std::max<std::size_t>(
		1, ws_.government_products.size());
	for (std::size_t index = 0; index < ws_.government_products.size(); ++index) {
		const int product = ws_.government_products[index];
		if (product < 0 || product >= ws_.products) {
			continue;
		}
		const double want = ws_.government_per_capita_day[index]
			* city.simulation_population;
		if (want <= 1e-12) {
			continue;
		}
		DemoMarket& market = city.markets[product];
		const double price = market.price;
		if (price <= 0.0) {
			continue;
		}
		double desired_tons = want;
		if (desired_tons * price > per_product_budget) {
			desired_tons = std::max(0.0, per_product_budget) / price;
		}
		if (desired_tons <= 1e-12) {
			continue;
		}
		market.desired += desired_tons;
		const double quantity = std::min(desired_tons,
			std::max(0.0, market.stock));
		if (quantity <= 0.0) {
			continue;
		}
		const double paid = quantity * price;
		market.stock -= quantity;
		market.demand += quantity;
		market.sold += quantity;
		city.government_budget -= paid;
		city.government_goods_spend += paid;
		city.firm_cash += paid;
		city.revenue_by_product[product] += paid;
		ws_.consumed_total[product] += quantity;
		ws_.consumed_today[product] += quantity;
	}
}

void SimCore::demo_refresh_income_classes(DemoCity& city) const {
	DemoPop& pop = city.pop;
	const double population = std::max(0.0, city.simulation_population);
	const double gini = std::clamp(city.income_gini, 0.0, 1.0);
	double basic_cost = 0.0;
	if (population > 1e-9) {
		basic_cost = demo_level_basket_cost(city, 0) / population;
	}
	// Sem Gini declarado (cidade sintética/save antigo) ou sem preço de cesta
	// (mundo sem needs.json): espelha os estratos de escolaridade. É o
	// comportamento anterior, preservado fora do cenário real.
	if (gini <= 0.0 || basic_cost <= 1e-12) {
		for (int level = 0; level < kDemoEducationLevels; ++level) {
			pop.class_population[level] = std::max(0.0, pop.stratum_population[level]);
			pop.class_income_level[level] = pop.stratum_income_level[level];
		}
		return;
	}
	// Log-normal: sigma do Gini (2Φ(σ/√2)-1 = G) e média ancorada no PIB PPP
	// per capita real, em cestas básicas (média mundial = kIncomeAnchorWorldMean).
	double sigma = std::sqrt(2.0) * normal_quantile(0.5 * (1.0 + gini));
	sigma = std::clamp(sigma, 0.15, 1.50);
	const double index = std::clamp(city.external_income_index, 0.01, 8.0);
	const double mean_units = kIncomeAnchorWorldMean * index;
	const double mu = std::log(mean_units) - 0.5 * sigma * sigma;
	// Limiares de classe em cestas básicas: pobreza = 1×; luxo = 7×.
	const double basic_share = std::clamp(
		normal_cdf((0.0 - mu) / sigma), 0.0, 1.0);
	const double luxury_share = std::clamp(
		1.0 - normal_cdf((std::log(kIncomeLuxuryMultiple) - mu) / sigma),
		0.0, 1.0);
	const double life_share = std::max(
		0.0, 1.0 - basic_share - luxury_share);
	pop.class_population[0] = population * basic_share;
	pop.class_population[1] = population * life_share;
	pop.class_population[2] = population * luxury_share;
	// basic compra só o nível 0; life, 0-1; luxury, todos (caps em 1-based).
	pop.class_income_level[0] = 1;
	pop.class_income_level[1] = 2;
	pop.class_income_level[2] = kDemoMaxNeedsLevels;
}

// ------------------------------------------------------------------
// F5 — câmbio/tarifas consultados pela fase de serviços.
// ------------------------------------------------------------------

const ogs::DemoCurrency* SimCore::demo_currency_for(
		const std::string& nation) const {
	const auto found = ws_.nation_currency.find(nation);
	if (found == ws_.nation_currency.end()
			|| found->second < 0
			|| found->second >= static_cast<int>(ws_.currencies.size())) {
		return nullptr;
	}
	return &ws_.currencies[static_cast<std::size_t>(found->second)];
}

double SimCore::demo_fx_rate(const std::string& nation) const {
	const DemoCurrency* currency = demo_currency_for(nation);
	if (currency == nullptr || !std::isfinite(currency->rate)
			|| currency->rate <= 0.0) {
		return 1.0;
	}
	return currency->rate;
}

const char* SimCore::demo_currency_code(const std::string& nation) const {
	const DemoCurrency* currency = demo_currency_for(nation);
	return currency != nullptr ? currency->code.c_str() : "XGS";
}

void SimCore::demo_rebuild_currency_indexes() {
	ws_.currency_by_code.clear();
	ws_.nation_currency.clear();
	ws_.currency_by_code.reserve(ws_.currencies.size() * 2);
	for (std::size_t index = 0; index < ws_.currencies.size(); ++index) {
		DemoCurrency& currency = ws_.currencies[index];
		if (currency.code.empty()) {
			continue;
		}
		ws_.currency_by_code[currency.code] = static_cast<int>(index);
		for (const std::string& nation : currency.nations) {
			if (!nation.empty()) {
				ws_.nation_currency[nation] = static_cast<int>(index);
			}
		}
	}
}

void SimCore::demo_tick_fx() {
	if (ws_.currencies.empty() || ws_.cities.empty()) {
		return;
	}
	std::vector<double> exports(ws_.currencies.size(), 0.0);
	std::vector<double> imports(ws_.currencies.size(), 0.0);
	for (const DemoCity& city : ws_.cities) {
		const auto found = ws_.nation_currency.find(city.nation);
		if (found == ws_.nation_currency.end()) {
			continue;
		}
		const std::size_t index = static_cast<std::size_t>(found->second);
		for (int product = 0; product < ws_.products; ++product) {
			exports[index] += city.export_value[product];
			imports[index] += city.import_value[product];
		}
	}
	const double alpha = 1.0 / kDemoFxBalanceEmaDays;
	for (std::size_t index = 0; index < ws_.currencies.size(); ++index) {
		DemoCurrency& currency = ws_.currencies[index];
		const double flow = exports[index] - imports[index];
		const double gross = exports[index] + imports[index];
		currency.balance_ema += (flow - currency.balance_ema) * alpha;
		currency.gross_ema += (gross - currency.gross_ema) * alpha;
		if (!currency.floating) {
			currency.rate = currency.base_rate;
			continue;
		}
		// Pressão normalizada pelo volume: um déficit que representa 10% do
		// comércio (kDemoFxBalanceScale) satura em tanh(1) ~ 0,76. O passo
		// diário é pequeno (0,05%), então o câmbio reage à persistência da
		// balança, não a um dia isolado.
		const double scale = std::max(1.0,
			currency.gross_ema * kDemoFxBalanceScale);
		const double pressure = std::tanh(currency.balance_ema / scale);
		const double next = currency.rate * (1.0 + kDemoFxDailyStep * pressure);
		currency.rate = std::clamp(next,
			currency.base_rate * kDemoFxBandLow,
			currency.base_rate * kDemoFxBandHigh);
	}

	// O preço interno está na âncora: o passo cambial NÃO reescala preços. Só
	// a exibição (`price_local = preço ÷ taxa`) e a competitividade das
	// ofertas float (taxa ÷ taxa-base) leem a taxa corrente.
}

double SimCore::demo_competitiveness(const std::string& nation) const {
	// Fator de competitividade aplicado à âncora (F5, opção c). Com a taxa
	// FIXA (peg) o fator é 1: o valor interno não muda. Com regime float, a
	// taxa pode desviar ±10% da referência; o fator `taxa ÷ taxa-base`
	// reproduz o efeito de preço rígido na moeda local (desvalorização barateia
	// a oferta externa do país sem reescrever o preço interno), documentado em
	// GAME_STATE §15/D-027.
	const DemoCurrency* currency = demo_currency_for(nation);
	if (currency == nullptr || !std::isfinite(currency->rate)
			|| !std::isfinite(currency->base_rate)
			|| currency->rate <= 0.0 || currency->base_rate <= 0.0) {
		return 1.0;
	}
	if (!currency->floating) {
		return 1.0;
	}
	return std::clamp(currency->rate / currency->base_rate,
		kDemoFxBandLow, kDemoFxBandHigh);
}

double SimCore::demo_tariff_rate(const std::string& buyer_nation,
		const std::string& seller_nation, int product) const {
	if (ws_.tariff_lookup.empty() || product < 0 || product >= ws_.products) {
		return 0.0;
	}
	const std::string good = demo_product_name(product);
	const std::string keys[4] = {
		buyer_nation + '|' + seller_nation + '|' + good,
		buyer_nation + "|*|" + good,
		buyer_nation + '|' + seller_nation + "|*",
		buyer_nation + "|*|*",
	};
	for (const std::string& key : keys) {
		const auto found = ws_.tariff_lookup.find(key);
		if (found != ws_.tariff_lookup.end()) {
			return std::clamp(found->second, 0.0, 1.0);
		}
	}
	return 0.0;
}

int SimCore::demo_product_index(const std::string& product) const {
	const auto found = ws_.product_lookup.find(product);
	if (found == ws_.product_lookup.end()) {
		return -1;
	}
	return found->second;
}

const char* SimCore::demo_product_name(int product) const {
	if (product >= 0 && product < ws_.products
			&& product < static_cast<int>(ws_.product_names.size())) {
		return ws_.product_names[static_cast<std::size_t>(product)].c_str();
	}
	return "";
}


// ------------------------------------------------------------------
// F8 — serviços.
// ------------------------------------------------------------------

int SimCore::demo_service_sector_index(const std::string& sector) const {
	for (std::size_t index = 0; index < ws_.service_sectors.size(); ++index) {
		if (ws_.service_sectors[index].id == sector) {
			return static_cast<int>(index);
		}
	}
	return -1;
}

void SimCore::demo_rebuild_service_indexes() {
	ws_.service_by_city.assign(ws_.cities.size(), {});
	ws_.service_by_sector.assign(ws_.service_sectors.size(), {});
	for (int index = 0; index < static_cast<int>(ws_.service_businesses.size()); ++index) {
		const DemoServiceBusiness& business =
			ws_.service_businesses[static_cast<std::size_t>(index)];
		if (business.bankrupt) {
			continue;
		}
		const auto found = ws_.city_lookup.find(business.city);
		if (found != ws_.city_lookup.end()) {
			ws_.service_by_city[static_cast<std::size_t>(found->second)].push_back(index);
		}
		if (business.sector >= 0
				&& business.sector < static_cast<int>(ws_.service_by_sector.size())) {
			ws_.service_by_sector[static_cast<std::size_t>(business.sector)].push_back(index);
		}
	}
}

double SimCore::demo_city_food_reserve(const DemoCity& city) const {
	// F8 v2: reserva alimentar robusta (XGS) que a poupança deve preservar antes
	// de financiar abertura de negócio. Usa a MESMA regra prudencial da F3
	// (kDemoFoodReserveDays de cesta) — a poupança que importa comida em nação
	// sem terra fica intocada; só o excedente financia serviço.
	const double need = std::max(0.0,
		city.simulation_population * kDemoNeedPerCapita);
	double local = 0.0;
	double import = 0.0;
	double per_ge = 0.0;
	demo_food_basket_cost(city, need, local, import, per_ge);
	return need * kDemoFoodReserveDays * per_ge;
}

bool SimCore::demo_service_open(DemoCity& city, int sector, double capacity,
		double capital) {
	if (sector < 0 || sector >= static_cast<int>(ws_.service_sectors.size())) {
		return false;
	}
	const DemoServiceSector& definition =
		ws_.service_sectors[static_cast<std::size_t>(sector)];
	capacity = std::max(0.0, capacity);
	capital = std::max(0.0, capital);
	if (capacity <= 0.0) {
		return false;
	}
	// F8 v2: o capital de abertura NÃO pode sair da poupança que paga comida
	// (F3 soma savings + renda para importar alimento — DJI regredia 0,472→0,355
	// quando o capital drenava essa poupança, Q-004). Ordem de financiamento:
	//   1º caixa do PRÓPRIO SETOR (lucros retidos em service_cash) — já está no
	//      setor, vira equity do negócio sem movimento de dinheiro;
	//   2º poupança das famílias ACIMA da reserva alimentar robusta — move
	//      savings→service_cash (e é devolvida no fechamento).
	// Se nem a soma cobre o capital sem tocar a reserva de comida, não abre.
	const double food_reserve = demo_city_food_reserve(city);
	const double savings_surplus = std::max(0.0, city.pop.savings - food_reserve);
	const double from_sector = std::min(capital, std::max(0.0, city.service_cash));
	const double from_savings = std::min(capital - from_sector, savings_surplus);
	if (from_sector + from_savings < capital) {
		return false;
	}
	if (from_savings > 0.0) {
		city.pop.savings -= from_savings;
		city.service_cash += from_savings;
	}
	DemoServiceBusiness business;
	business.id = ws_.next_service_business_id++;
	business.sector = sector;
	business.city = city.id;
	business.owner_stratum = kDemoEducationLevels - 1;
	business.capacity = capacity;
	business.jobs = capacity * definition.jobs_per_unit;
	business.workers = business.jobs;
	// `capital` guarda só a parcela da POP (devolvida no fechamento); a equity
	// total (capital_left) soma a parcela do setor.
	business.capital = from_savings;
	business.capital_left = from_sector + from_savings;
	business.opened_day = ws_.day;
	unsigned long long seed = 1469598103934665603ULL;
	auto mix = [&seed](const std::string& text) {
		for (const char c : text) {
			seed ^= static_cast<unsigned char>(c);
			seed *= 1099511628211ULL;
		}
		seed ^= 0xffULL;
		seed *= 1099511628211ULL;
	};
	mix(city.id);
	mix(definition.id);
	mix(std::to_string(business.id));
	business.name_seed = seed;
	const int index = static_cast<int>(ws_.service_businesses.size());
	ws_.service_businesses.push_back(std::move(business));
	const auto found = ws_.city_lookup.find(city.id);
	if (found != ws_.city_lookup.end()) {
		if (ws_.service_by_city.size() != ws_.cities.size()) {
			ws_.service_by_city.assign(ws_.cities.size(), {});
		}
		ws_.service_by_city[static_cast<std::size_t>(found->second)].push_back(index);
	}
	if (ws_.service_by_sector.size() != ws_.service_sectors.size()) {
		ws_.service_by_sector.assign(ws_.service_sectors.size(), {});
	}
	ws_.service_by_sector[static_cast<std::size_t>(sector)].push_back(index);
	city.service_business_count += 1;
	return true;
}

void SimCore::demo_service_close(DemoServiceBusiness& business) {
	if (business.bankrupt) {
		return;
	}
	business.bankrupt = true;
	DemoCity* city = find_demo_city(business.city);
	if (city != nullptr) {
		// F8 v2: devolve à POP no máximo a parcela que ELA aportou
		// (`business.capital` = from_savings); a parcela financiada pelo setor
		// fica no setor. `capital_left` pode exceder o aporte quando o negócio
		// lucra — o lucro já foi distribuído no agregado do setor, então só o
		// aporte da POP volta (o teto pelo service_cash evita drenar o giro).
		const double returned = std::min(
			std::max(0.0, business.capital_left),
			std::max(0.0, business.capital));
		const double paid_back = std::min(returned, std::max(0.0, city->service_cash));
		city->service_cash -= paid_back;
		city->pop.savings += paid_back;
		city->service_business_count = std::max(0, city->service_business_count - 1);
	}
	business.capital_left = 0.0;
	ws_.service_bankruptcies += 1;
}

void SimCore::demo_seed_services() {
	ws_.service_businesses.clear();
	ws_.service_businesses.shrink_to_fit();
	ws_.service_bankruptcies = 0;
	ws_.next_service_business_id = 1;
	ws_.service_by_city.assign(ws_.cities.size(), {});
	ws_.service_by_sector.assign(ws_.service_sectors.size(), {});
	if (!ws_.services_loaded || ws_.service_sectors.empty()) {
		return;
	}
	for (DemoCity& city : ws_.cities) {
		city.service_business_count = 0;
		for (int sector = 0; sector < kDemoMaxServiceSectors; ++sector) {
			DemoServiceMarket& market = city.service_markets[sector];
			market.base_price =
				ws_.service_sectors[static_cast<std::size_t>(sector)].price;
			if (market.price <= 0.0 || !std::isfinite(market.price)) {
				market.price = market.base_price;
			}
		}
		const double population = std::max(1.0, city.simulation_population);
		int count = static_cast<int>(std::clamp(
			4.0 + std::log10(std::max(1000.0, population)) * 2.0, 4.0, 16.0));
		// Escolhe os setores com maior peso de consumo das famílias; setores
		// exportáveis entram quando o país tem renda acima da média.
		std::vector<int> order(ws_.service_sectors.size());
		std::vector<double> weight(ws_.service_sectors.size(), 0.0);
		for (std::size_t sector = 0; sector < ws_.service_sectors.size(); ++sector) {
			order[sector] = static_cast<int>(sector);
			const DemoServiceSector& definition = ws_.service_sectors[sector];
			weight[sector] = definition.pop_share[0] + definition.pop_share[1] * 1.4
				+ definition.pop_share[2] * 1.8 + definition.business_share
				+ definition.government_share * 0.5;
		}
		std::stable_sort(order.begin(), order.end(), [&weight](int a, int b) {
			return weight[static_cast<std::size_t>(a)]
				> weight[static_cast<std::size_t>(b)];
		});
		for (int slot = 0; slot < count && slot < static_cast<int>(order.size()); ++slot) {
			const int sector = order[static_cast<std::size_t>(slot)];
			const DemoServiceSector& definition =
				ws_.service_sectors[static_cast<std::size_t>(sector)];
			// F8 fix: sem o teto arbitrário de 40 postos. A semente parte do
			// mínimo viável do setor; a ENTRADA (passo 9) cresce a oferta até a
			// demanda real. A folha sobre efetivos torna o negócio ocioso
			// não-letivo, então superdimensionar não é mais fatal.
			const double capacity = definition.min_capacity;
			const double capital = capacity * definition.capital_per_unit;
			// F8 v2: o capital de abertura vem do caixa do próprio setor e, se
			// faltar, da poupança das famílias ACIMA da reserva de comida — o
			// gate interno de demo_service_open decide. Cidade pequena/pobre
			// semeia menos negócios e abre os demais quando houver caixa.
			demo_service_open(city, sector, capacity, capital);
		}
		if (city.external_income_index > 1.2) {
			for (int sector = 0; sector < static_cast<int>(ws_.service_sectors.size()); ++sector) {
				const DemoServiceSector& definition =
					ws_.service_sectors[static_cast<std::size_t>(sector)];
				if (!definition.exportable) {
					continue;
				}
				bool present = false;
				for (const int index : ws_.service_by_city[static_cast<std::size_t>(
						&city - ws_.cities.data())]) {
					if (ws_.service_businesses[static_cast<std::size_t>(index)].sector
							== sector) {
						present = true;
						break;
					}
				}
				if (present) {
					continue;
				}
				const double capacity = definition.min_capacity;
				const double capital = capacity * definition.capital_per_unit;
				demo_service_open(city, sector, capacity, capital);
			}
		}
	}
}

void SimCore::demo_services_day(DemoCity& city) {
	if (!ws_.services_loaded || ws_.service_sectors.empty()) {
		return;
	}
	const std::size_t city_index =
		static_cast<std::size_t>(&city - ws_.cities.data());
	if (city_index >= ws_.service_by_city.size()) {
		ws_.service_by_city.assign(ws_.cities.size(), {});
	}
	if (ws_.service_by_sector.size() != ws_.service_sectors.size()) {
		ws_.service_by_sector.assign(ws_.service_sectors.size(), {});
	}
	const DemoPop& pop = city.pop;
	const double wage = std::max(0.0, city.service_wage);
	// F8 v2 (calibração, Q-004): o custo de trabalho do negócio é o salário de
	// mercado INTEGRAL — sem o teto `preço÷jobs_per_unit×0,68`, que era a muleta
	// que mascarava o preço sub-calibrado. A margem realista vem da ÂNCORA DE
	// PREÇO escalada pelo salário local (passo 8): price/jobs_per_unit acompanha
	// o salário, então o negócio não fica estruturalmente no prejuízo em país
	// rico — o teto contábil deixa de ser necessário.
	// Reserva alimentar robusta (calculada UMA vez por cidade/dia): a poupança
	// acima dela é o que pode financiar a entrada de negócio, sem tocar a
	// poupança que importa comida (DJI, Q-004).
	const double food_reserve = demo_city_food_reserve(city);
	const double savings_surplus = std::max(0.0, city.pop.savings - food_reserve);

	// 1. Pesos da demanda das famílias por classe de consumo (basic/life/
	// luxury). Com Gini, `class_population` é a repartição real; sem Gini,
	// espelha os estratos (mesma regra da F7). O dinheiro é o
	// `pop.service_spend` já debitado das famílias; aqui ele é DISTRIBUÍDO.
	double class_pop[kDemoEducationLevels] = {
		std::max(0.0, pop.class_population[0]),
		std::max(0.0, pop.class_population[1]),
		std::max(0.0, pop.class_population[2])};
	if (class_pop[0] + class_pop[1] + class_pop[2] <= 1e-9) {
		for (int level = 0; level < kDemoEducationLevels; ++level) {
			class_pop[level] = std::max(0.0, pop.stratum_population[level]);
		}
	}
	double weight[kDemoMaxServiceSectors] = {};
	double weight_total = 0.0;
	for (int sector = 0; sector < kDemoMaxServiceSectors; ++sector) {
		const DemoServiceSector& definition =
			ws_.service_sectors[static_cast<std::size_t>(sector)];
		double value = 0.0;
		for (int level = 0; level < kDemoMaxNeedsLevels; ++level) {
			value += class_pop[level] * definition.pop_share[level];
		}
		weight[sector] = value;
		weight_total += value;
	}
	const double household_pool = std::max(0.0, pop.service_spend);

	// 2. B2B: o opex pago pelas empresas é receita de serviços. Finanças,
	// profissionais, TI, telecom, manutenção e limpeza entram por participação
	// declarada; logística entra pela TONELAGEM movimentada ontem (sold_prev da
	// cesta + insumos), como pede o plano.
	double tonnage = 0.0;
	for (int product = 0; product < ws_.products; ++product) {
		tonnage += std::max(0.0, city.markets[product].sold_prev);
	}
	city.service_tonnage_prev = tonnage;
	const double b2b_pool = std::max(0.0, city.opex_revenue);
	double b2b_weight[kDemoMaxServiceSectors] = {};
	double b2b_total = 0.0;
	for (int sector = 0; sector < kDemoMaxServiceSectors; ++sector) {
		const DemoServiceSector& definition =
			ws_.service_sectors[static_cast<std::size_t>(sector)];
		double value = definition.business_share;
		if (definition.freight_share > 0.0) {
			value += definition.freight_share
				* std::clamp(tonnage / 2000.0, 0.05, 4.0);
		}
		b2b_weight[sector] = value;
		b2b_total += value;
	}

	// 3. Governo: compra uma fração pequena do orçamento em serviços
	// (limpeza/segurança/resíduos, construção, profissionais, saúde/educação).
	// É transferência explícita governo → service_cash (dinheiro conservado).
	double government_pool = 0.0;
	double gov_weight[kDemoMaxServiceSectors] = {};
	double gov_total = 0.0;
	for (int sector = 0; sector < kDemoMaxServiceSectors; ++sector) {
		gov_weight[sector] =
			ws_.service_sectors[static_cast<std::size_t>(sector)].government_share;
		gov_total += gov_weight[sector];
	}
	if (gov_total > 0.0) {
		// A fração é da ARRECADAÇÃO DO DIA (fluxo), não do caixa acumulado:
		// cobrar a fração sobre o estoque todo dia drenava o tesouro que
		// financia a importação de comida (AFG-KAN/KAB em 365 d, F8). O caixa
		// do dia continua sendo o teto.
		const double daily_flow = std::max(0.0, pop.taxes);
		government_pool = std::min(daily_flow * ws_.service_government_share,
			std::max(0.0, city.government_budget));
		city.government_budget -= government_pool;
		city.service_cash += government_pool;
		city.service_government_revenue += government_pool;
	}

	// 4. Capacidade e exportação. A exportação é atendida primeiro; o resto da
	// oferta fica para o mercado doméstico. O preço de exportação usa a
	// competitividade cambial da F5 (a tarifa declarada para "services" reduz a
	// competitividade, sem criar/destruir dinheiro).
	const double competitiveness = std::clamp(
		demo_competitiveness(city.nation), 0.5, 1.5);
	const int service_tariff_product = demo_product_index(std::string("services"));
	const double service_tariff = service_tariff_product >= 0
		? demo_tariff_rate(city.nation, "*", service_tariff_product) : 0.0;
	const double export_factor = competitiveness * (1.0 - std::clamp(service_tariff, 0.0, 1.0));
	double sector_supply[kDemoMaxServiceSectors] = {};
	double sector_exports[kDemoMaxServiceSectors] = {};
	double sector_export_revenue[kDemoMaxServiceSectors] = {};
	// Contagem de negócios ativos por setor, na MESMA passada que soma a oferta:
	// o laço de entrada (passo 9) usa este vetor em vez de varrer os negócios
	// da cidade para cada setor (era O(setores × negócios)). Nenhum negócio
	// abre/fecha entre aqui e o passo 9 (abertura/fechamento acontecem no próprio
	// passo 9), então a contagem é idêntica à varredura antiga — resultado igual.
	int active_by_sector[kDemoMaxServiceSectors] = {};
	for (const int index : ws_.service_by_city[city_index]) {
		DemoServiceBusiness& business =
			ws_.service_businesses[static_cast<std::size_t>(index)];
		if (business.bankrupt || business.sector < 0
				|| business.sector >= kDemoMaxServiceSectors) {
			continue;
		}
		const double capacity = std::max(0.0,
			business.capacity * std::clamp(business.utilization > 0.0
				? business.utilization : 1.0, 0.05, 1.0));
		sector_supply[business.sector] += capacity;
		++active_by_sector[business.sector];
	}
	const double external_factor = std::clamp(city.external_income_index, 0.2, 3.0);
	for (int sector = 0; sector < kDemoMaxServiceSectors; ++sector) {
		const DemoServiceSector& definition =
			ws_.service_sectors[static_cast<std::size_t>(sector)];
		if (!definition.exportable || definition.export_share <= 0.0) {
			continue;
		}
		const double units = sector_supply[sector] * definition.export_share
			* external_factor;
		if (units <= 0.0) {
			continue;
		}
		const double revenue = units
			* city.service_markets[sector].price * export_factor;
		sector_exports[sector] = units;
		sector_export_revenue[sector] = revenue;
		city.service_export_revenue += revenue;
		city.service_cash += revenue;
		ws_.foreign_cash -= revenue;
	}

	// 5. Mercados: demanda em unidades = dinheiro / preço. A receita só é
	// realizada na proporção atendida (demanda sem oferta não vira dinheiro).
	double total_units_served = 0.0;
	double total_service_cost = 0.0;
	double total_input_cost = 0.0;
	for (int sector = 0; sector < kDemoMaxServiceSectors; ++sector) {
		DemoServiceMarket& market = city.service_markets[sector];
		market.demand = 0.0;
		market.supply = sector_supply[sector];
		market.unmet = 0.0;
		market.revenue = 0.0;
		market.input_cost = 0.0;
		market.service_cost = 0.0;
		market.jobs = 0.0;
		market.businesses = 0.0;
		market.exports = sector_exports[sector];
		market.export_revenue = sector_export_revenue[sector];
		market.coverage = 0.0;
		double money = 0.0;
		if (weight_total > 0.0) {
			money += household_pool * weight[sector] / weight_total;
		}
		if (b2b_total > 0.0) {
			money += b2b_pool * b2b_weight[sector] / b2b_total;
		}
		if (gov_total > 0.0) {
			money += government_pool * gov_weight[sector] / gov_total;
		}
		if (market.price <= 0.0) {
			market.price = std::max(0.01,
				ws_.service_sectors[static_cast<std::size_t>(sector)].price);
		}
		const double domestic_demand = money / market.price;
		const double total_demand = domestic_demand + market.exports;
		market.demand = total_demand;
		for (int level = 0; level < kDemoMaxNeedsLevels; ++level) {
			market.pop_demand[level] = weight_total > 0.0 && market.price > 1e-12
				? household_pool * class_pop[level]
					* ws_.service_sectors[static_cast<std::size_t>(sector)].pop_share[level]
					/ weight_total / market.price
				: 0.0;
		}
		const double served = std::min(market.supply, total_demand);
		const double domestic_served = std::max(0.0,
			std::min(domestic_demand, served - market.exports));
		const double export_served = std::min(market.exports, served);
		// Dinheiro realizado: fração doméstica atendida do pool + exportação
		// efetivamente entregue. O pool não gasto fica em service_cash.
		const double realized = domestic_demand > 1e-12
			? money * domestic_served / domestic_demand : 0.0;
		market.revenue = realized + export_served * market.price * export_factor;
		if (market.demand > 0.0) {
			market.unmet = std::max(0.0, market.demand - served);
		}
		total_units_served += served;
		// Cobertura privada de saúde/educação: oferta por habitante contra a
		// necessidade de referência (gancho documentado, não serviço público).
		if (ws_.service_sectors[static_cast<std::size_t>(sector)].health) {
			const double need = std::max(1e-9,
				city.simulation_population * kDemoServiceHealthNeedPerCapita);
			market.coverage = std::clamp(market.supply / need, 0.0, 1.0);
			city.private_health_cover = market.coverage;
		}
		if (ws_.service_sectors[static_cast<std::size_t>(sector)].education) {
			const double need = std::max(1e-9,
				city.simulation_population * kDemoServiceEducationNeedPerCapita);
			market.coverage = std::clamp(market.supply / need, 0.0, 1.0);
			city.private_education_cover = market.coverage;
		}
	}

	// 6. Negócios: produzem a sua fatia da oferta, compram insumos (bens e
	// serviços) e pagam a folha (fatia de city.service_wage_bill). O lucro é
	// informativo para entrada/saída; a distribuição aos donos continua no
	// agregado do setor de serviços (service_profit).
	std::vector<double> sector_revenue_gain(kDemoMaxServiceSectors, 0.0);
	for (const int index : ws_.service_by_city[city_index]) {
		DemoServiceBusiness& business =
			ws_.service_businesses[static_cast<std::size_t>(index)];
		if (business.bankrupt || business.sector < 0
				|| business.sector >= kDemoMaxServiceSectors) {
			continue;
		}
		DemoServiceMarket& market = city.service_markets[business.sector];
		const DemoServiceSector& definition =
			ws_.service_sectors[static_cast<std::size_t>(business.sector)];
		market.businesses += 1.0;
		const double capacity = std::max(0.0, business.capacity);
		double units = 0.0;
		if (market.supply > 1e-12) {
			units = std::min(capacity, market.supply > 0.0
				? std::max(0.0, market.demand) * capacity / market.supply : 0.0);
			units = std::min(units, capacity);
		}
		business.utilization = capacity > 1e-12 ? units / capacity : 0.0;
		business.workers = business.jobs * std::clamp(business.utilization, 0.05, 1.0);
		const double share = market.supply > 1e-12 ? capacity / market.supply : 0.0;
		business.revenue = market.revenue * share;
		// Insumos de bens: compra no mercado da cidade ao preço corrente, até o
		// estoque disponível. O dinheiro sai de service_cash e entra em
		// firm_cash (produtor), conservando o total.
		double input_cost = 0.0;
		for (const DemoServiceSector::Input& input : definition.inputs) {
			if (input.product < 0 || input.product >= ws_.products) {
				continue;
			}
			DemoMarket& goods = city.markets[input.product];
			const double needed = input.qty * units;
			const double bought = std::min(needed, std::max(0.0, goods.stock));
			if (bought <= 0.0) {
				continue;
			}
			const double paid = bought * goods.price;
			goods.stock -= bought;
			goods.demand += bought;
			goods.sold += bought;
			goods.desired += needed;
			city.revenue_by_product[input.product] += paid;
			input_cost += paid;
		}
		business.input_cost = input_cost;
		// Insumos de serviço: transferência entre pools setoriais (não sai do
		// service_cash; o lucro do fornecedor sobe e o do comprador cai).
		double service_cost = 0.0;
		for (std::size_t slot = 0; slot < definition.service_inputs.size(); ++slot) {
			const int supplier = definition.service_inputs[slot];
			if (supplier < 0 || supplier >= kDemoMaxServiceSectors) {
				continue;
			}
			const double qty = definition.service_input_qty[slot] * units;
			const double price = city.service_markets[supplier].price;
			const double paid = qty * price;
			service_cost += paid;
			sector_revenue_gain[static_cast<std::size_t>(supplier)] += paid;
		}
		business.service_cost = service_cost;
		// F8 fix: folha sobre EFETIVOS (workers), não sobre POSTOS (jobs). O
		// negócio ocioso não paga a folha cheia — o custo acompanha a
		// utilização. A folha agregada do setor (demo_labor_day) continua sendo
		// a do empregador residual da F7; aqui só se atribui o custo marginal.
		business.wages = business.workers * wage;
		business.profit = business.revenue - business.wages
			- business.input_cost - business.service_cost;
		business.capital_left = std::max(0.0,
			business.capital_left + business.profit);
		if (business.profit < 0.0 && business.utilization > 1e-9) {
			++business.loss_days;
		} else if (business.profit > 0.0) {
			business.loss_days = 0;
		}
		total_input_cost += business.input_cost;
		total_service_cost += business.service_cost;
	}
	// Receita dos fornecedores de serviços: aumenta o pool publicado do setor.
	for (int sector = 0; sector < kDemoMaxServiceSectors; ++sector) {
		if (sector_revenue_gain[static_cast<std::size_t>(sector)] > 0.0) {
			city.service_markets[sector].revenue +=
				sector_revenue_gain[static_cast<std::size_t>(sector)];
		}
	}
	// E chega aos negócios fornecedores (mesma proporção de capacidade): o
	// lucro do comprador já pagou o serviço, então o efeito agregado é nulo.
	for (const int index : ws_.service_by_city[city_index]) {
		DemoServiceBusiness& business =
			ws_.service_businesses[static_cast<std::size_t>(index)];
		if (business.bankrupt || business.sector < 0
				|| business.sector >= kDemoMaxServiceSectors) {
			continue;
		}
		const double gain = sector_revenue_gain[static_cast<std::size_t>(business.sector)];
		if (gain <= 0.0) {
			continue;
		}
		const double supply = city.service_markets[business.sector].supply;
		const double share = supply > 1e-12 ? business.capacity / supply : 0.0;
		business.revenue += gain * share;
		business.profit += gain * share;
		business.capital_left += gain * share;
	}
	// Paga os insumos de bens: service_cash → firm_cash.
	const double paid_inputs = std::min(total_input_cost,
		std::max(0.0, city.service_cash));
	if (paid_inputs > 0.0) {
		city.service_cash -= paid_inputs;
		city.firm_cash += paid_inputs;
	}
	city.service_input_cost = total_input_cost;

	// 7. Emprego por setor: divide o emprego AGREGADO de serviços da F7 (o
	// empregador residual) pela participação dos negócios de cada setor. Sem
	// negócios, a divisão segue os pesos de demanda — a soma continua igual a
	// city.service_jobs (nenhum emprego é criado ou perdido aqui).
	double jobs_weight[kDemoMaxServiceSectors] = {};
	double jobs_total = 0.0;
	for (const int index : ws_.service_by_city[city_index]) {
		const DemoServiceBusiness& business =
			ws_.service_businesses[static_cast<std::size_t>(index)];
		if (business.bankrupt || business.sector < 0
				|| business.sector >= kDemoMaxServiceSectors) {
			continue;
		}
		jobs_weight[business.sector] += std::max(0.0, business.jobs);
		jobs_total += std::max(0.0, business.jobs);
	}
	if (jobs_total <= 1e-12) {
		for (int sector = 0; sector < kDemoMaxServiceSectors; ++sector) {
			jobs_weight[sector] = weight[sector];
			jobs_total += weight[sector];
		}
	}
	for (int sector = 0; sector < kDemoMaxServiceSectors; ++sector) {
		city.service_markets[sector].jobs = jobs_total > 1e-12
			? city.service_jobs * jobs_weight[sector] / jobs_total : 0.0;
	}

	// 8. Preço: reage à razão demanda financiada/oferta, com suavização e
	// banda em torno da âncora. Oferta zero com demanda > 0 sobe ao teto.
	for (int sector = 0; sector < kDemoMaxServiceSectors; ++sector) {
		DemoServiceMarket& market = city.service_markets[sector];
		const DemoServiceSector& definition =
			ws_.service_sectors[static_cast<std::size_t>(sector)];
		// F8 v2 — ÂNCORA LIGADA AO SALÁRIO DO PAÍS: o preço-âncora do setor
		// escala com o salário de serviço local (kDemoServiceWage = 0,15, a
		// âncora salarial da F7). Sem isso price/jobs_per_unit ficava fixo em
		// ~0,22 e, em país rico, abaixo do salário — o negócio formal ficava no
		// prejuízo estrutural (o teto 0,68 mascarava). Com a âncora escalando, a
		// receita por posto passa a ≈ 1,47× o salário local, então a margem fica
		// estável em país rico e pobre; os limites evitam preço zero/explosivo.
		const double anchor_scale = std::clamp(
			wage / kDemoServiceWage, kDemoServiceAnchorScaleMin,
			kDemoServiceAnchorScaleMax);
		market.base_price = definition.price * anchor_scale
			* kDemoServiceMarginCalibration;
		const double ratio = market.supply > 1e-9
			? std::max(0.0, market.demand) / market.supply
			: (market.demand > 1e-9 ? 4.0 : 1.0);
		const double target = market.base_price * std::clamp(
			std::pow(ratio, 0.35), kDemoServicePriceBandLow,
			kDemoServicePriceBandHigh);
		market.price += (target - market.price) * kDemoServicePriceSmooth;
		market.price = std::clamp(market.price,
			market.base_price * kDemoServicePriceBandLow,
			market.base_price * kDemoServicePriceBandHigh);
	}

	// 9. Entrada e saída. Entrada só com demanda não atendida, margem esperada
	// acima do limiar e capital financiável pelo caixa do setor + poupança acima
	// da reserva de comida. Até kDemoServiceOpensPerDay aberturas por cidade/dia
	// (teto, não alvo: sem demanda não atendida, não abre nada).
	int opens = 0;
	for (int sector = 0; sector < kDemoMaxServiceSectors && opens < kDemoServiceOpensPerDay; ++sector) {
		DemoServiceMarket& market = city.service_markets[sector];
		const DemoServiceSector& definition =
			ws_.service_sectors[static_cast<std::size_t>(sector)];
		if (market.unmet < definition.min_capacity * 0.5) {
			continue;
		}
		if (active_by_sector[sector] >= kDemoServiceMaxPerSector) {
			continue;
		}
		// F8 v2: capacidade pela DEMANDA não atendida, com folga de 25% para a
		// oferta se manter à frente do crescimento da renda. Financiada pelo
		// caixa do setor + poupança das famílias ACIMA da reserva de comida
		// (nunca pela poupança que paga comida — DJI, Q-004).
		const double finance_base = std::max(0.0, city.service_cash) + savings_surplus;
		const double affordable = finance_base > 0.0
			? finance_base
				/ (definition.capital_per_unit * kDemoServiceCapitalReserve) : 0.0;
		if (affordable < definition.min_capacity) {
			continue;
		}
		const double capacity = std::max(definition.min_capacity,
			std::min(market.unmet * 1.25, affordable));
		const double revenue = capacity * market.price;
		double expected_cost = capacity * definition.jobs_per_unit * wage;
		for (const DemoServiceSector::Input& input : definition.inputs) {
			if (input.product >= 0 && input.product < ws_.products) {
				expected_cost += capacity * input.qty * city.markets[input.product].price;
			}
		}
		for (std::size_t slot = 0; slot < definition.service_inputs.size(); ++slot) {
			const int supplier = definition.service_inputs[slot];
			if (supplier >= 0 && supplier < kDemoMaxServiceSectors) {
				expected_cost += capacity * definition.service_input_qty[slot]
					* city.service_markets[supplier].price;
			}
		}
		const double margin = revenue > 1e-9
			? (revenue - expected_cost) / revenue : -1.0;
		if (margin < kDemoServiceEntryMargin) {
			continue;
		}
		const double capital = capacity * definition.capital_per_unit;
		if (demo_service_open(city, sector, capacity, capital)) {
			++opens;
		}
	}
	for (const int index : ws_.service_by_city[city_index]) {
		DemoServiceBusiness& business =
			ws_.service_businesses[static_cast<std::size_t>(index)];
		if (business.bankrupt) {
			continue;
		}
		if (business.loss_days >= kDemoServiceLossDays) {
			demo_service_close(business);
		}
	}
	city.service_business_count = 0;
	for (const int index : ws_.service_by_city[city_index]) {
		if (!ws_.service_businesses[static_cast<std::size_t>(index)].bankrupt) {
			++city.service_business_count;
		}
	}
	(void)total_units_served;
	(void)total_service_cost;
}

// ------------------------------------------------------------------
// F7 — salários ao fechamento do dia.
// ------------------------------------------------------------------

void SimCore::demo_update_wages(DemoCity& city, double building_margin) {
	DemoPop& pop = city.pop;
	const auto minimum = ws_.minimum_wages.find(city.nation);
	double floor = minimum != ws_.minimum_wages.end()
		? std::max(0.0, minimum->second) : 0.0;
	const double margin = std::clamp(building_margin, -0.25, 0.25);
	// 1. Parcela do trabalho no valor adicionado do dia. `city.gdp` é o valor
	// adicionado das construções (receita − insumos) + a receita de serviços,
	// já calculado nesta fase. A faixa 50–65% acompanha a renda real do país
	// (países ricos têm setor de serviços maior e participação do trabalho
	// maior); não é um multiplicador de preço.
	const double t = std::clamp(
		std::clamp(city.external_income_index, 0.0, 8.0) / kLaborIncomeSaturation,
		0.0, 1.0);
	const double labor_share = kDemoLaborShareLow
		+ (kDemoLaborShareHigh - kDemoLaborShareLow) * t;
	double filled_total = 0.0;
	for (int level = 0; level < kDemoEducationLevels; ++level) {
		filled_total += std::max(0.0, pop.filled_by_slot[level]);
	}
	// F7 calibração — PISO DE SUBSISTÊNCIA. Com a TFP como FATOR COMPLETO
	// (nível do país, não prêmio), um país de PIB PPP per capita 0,37 teria
	// alvo salarial abaixo da cesta alimentar. O piso garante que um emprego
	// pague a comida de um trabalhador e seus dependentes (população ÷ vagas
	// preenchidas) pelo alimento mais barato por caloria. Não cria dinheiro: o
	// pagamento continua limitado ao caixa da firma (pay_scale).
	if (filled_total > 1e-9 && city.simulation_population > 0.0) {
		double cheapest_food = 0.0;
		for (int slot = 0;
				slot < static_cast<int>(ws_.food_products.size()); ++slot) {
			const int product = demo_food_product(slot);
			const double factor = demo_food_calories(product);
			if (factor <= 0.0) {
				continue;
			}
			const double per_calorie = city.markets[product].price / factor;
			if (per_calorie > 0.0
					&& (cheapest_food <= 0.0 || per_calorie < cheapest_food)) {
				cheapest_food = per_calorie;
			}
		}
		if (cheapest_food > 0.0) {
			floor = std::max(floor,
				cheapest_food * kDemoNeedPerCapita
					* city.simulation_population / filled_total);
		}
	}
	const double productivity = demo_effective_productivity(city);
	const double value_added = std::max(0.0, city.gdp) * productivity;
	// TFP nacional (`derived:gdp_ppp`): o valor adicionado POR TRABALHADOR
	// usado na âncora é o do país relativo à média mundial. O pagamento
	// continua limitado ao caixa da firma (pay_scale), então a TFP muda a
	// divisão salário/lucro — não a quantidade de dinheiro em circulação.
	if (filled_total <= 1e-9 || value_added <= 1e-9) {
		// Sem emprego ou sem valor adicionado no dia: não há âncora nova. Os
		// salários ficam onde estavam (o piso continua valendo).
		for (int level = 0; level < kDemoEducationLevels; ++level) {
			pop.wage_by_level[level] = std::max(
				floor, pop.wage_by_level[level]);
		}
		return;
	}
	const double wage_bill_target = labor_share * value_added;
	const double average_target = wage_bill_target / filled_total;
	// 2. Prêmio RELATIVO por nível: razão fixa pequena × aperto do mercado do
	// nível (escassez sobe, sobra derruba) × margem das firmas. A normalização
	// por Σ(vagas × razão) mantém a folha total = parcela × VA: o aperto muda a
	// distribuição entre os níveis, não o tamanho da folha.
	double ratio[kDemoEducationLevels];
	double ratio_weighted = 0.0;
	for (int level = 0; level < kDemoEducationLevels; ++level) {
		const double supply = std::max(pop.labor_force_by_level[level], 1e-9);
		const double demand = std::max(0.0, pop.vacancies_by_level[level]);
		const double unfilled = std::max(0.0, pop.unfilled_by_level[level]);
		const double jobless = std::max(0.0, pop.unemployed_by_level[level]);
		const double scale = std::max(std::max(demand, supply), 1e-9);
		const double tightness = std::clamp(
			(unfilled - jobless) / scale, -1.0, 1.0);
		double margin_factor = 1.0;
		if (margin > 0.0 && tightness > 0.0) {
			margin_factor = 1.0 + kLaborWageMarginGain * margin;
		} else if (margin < 0.0) {
			margin_factor = std::clamp(
				1.0 + kLaborWageMarginGain * margin, 0.85, 1.0);
		}
		ratio[level] = kLaborWageRatio[level]
			* (1.0 + kLaborWageTightnessGain * tightness) * margin_factor;
		ratio[level] = std::max(ratio[level], 0.10);
		ratio_weighted += pop.filled_by_slot[level] * ratio[level];
	}
	if (ratio_weighted <= 1e-12) {
		return;
	}
	// 3. Alvo por nível e passo diário limitado pela rigidez. O primeiro ajuste
	// de uma cidade sem histórico (âncora inicial = palpite de criação do
	// mundo) entra direto: não é um salário observado que precise de inércia.
	const bool anchored = pop.wage_anchored;
	for (int level = 0; level < kDemoEducationLevels; ++level) {
		// Σ(vagas × salário) = parcela × VA: o salário do nível é o salário
		// médio e a razão normalizada redistribui a folha entre os níveis.
		const double target = std::max(floor,
			average_target * filled_total * ratio[level] / ratio_weighted);
		const double current = pop.wage_by_level[level];
		double next = target;
		if (anchored) {
			const double step = std::max(current, target) * ws_.wage_rigidity;
			next = current + std::clamp(target - current, -step, step);
		}
		pop.wage_by_level[level] = std::clamp(next, floor, 100.0);
	}
	pop.wage_anchored = true;
}

// ------------------------------------------------------------------
// F9 — investimento e propriedade
// ------------------------------------------------------------------

int SimCore::demo_owner_kind(const std::string& owner, std::string* group_id,
		std::string* origin_nation) const {
	if (group_id != nullptr) {
		group_id->clear();
	}
	if (origin_nation != nullptr) {
		origin_nation->clear();
	}
	if (owner == "state") {
		return 0;
	}
	const std::string national_prefix = "national:";
	if (owner.rfind(national_prefix, 0) == 0) {
		if (group_id != nullptr) {
			*group_id = owner.substr(national_prefix.size());
		}
		return 1;
	}
	const std::string foreign_prefix = "foreign:";
	if (owner.rfind(foreign_prefix, 0) == 0) {
		const std::string rest = owner.substr(foreign_prefix.size());
		const std::size_t at = rest.find('@');
		if (group_id != nullptr) {
			*group_id = at == std::string::npos ? rest : rest.substr(0, at);
		}
		if (origin_nation != nullptr) {
			*origin_nation = at == std::string::npos
				? std::string() : rest.substr(at + 1);
		}
		return 2;
	}
	return 3;  // "private" e qualquer dono desconhecido
}

int SimCore::demo_group_index(const std::string& id) const {
	const auto found = ws_.group_lookup.find(id);
	return found == ws_.group_lookup.end() ? -1 : found->second;
}

DemoGroup* SimCore::find_demo_group(const std::string& id) {
	const int index = demo_group_index(id);
	return index < 0 ? nullptr : &ws_.groups[static_cast<std::size_t>(index)];
}

const DemoGroup* SimCore::find_demo_group(
		const std::string& id) const {
	const int index = demo_group_index(id);
	return index < 0 ? nullptr : &ws_.groups[static_cast<std::size_t>(index)];
}

int SimCore::demo_fdi_mode(const std::string& nation,
		const std::string& sector) const {
	const auto found = ws_.fdi_lookup.find(nation + "|" + sector);
	return found == ws_.fdi_lookup.end()
		? 0 : ws_.fdi_policies[static_cast<std::size_t>(found->second)].mode;
}

double SimCore::demo_sector_tax_holiday(const std::string& nation,
		const std::string& sector) const {
	const auto found = ws_.incentive_lookup.find(nation + "|" + sector);
	if (found == ws_.incentive_lookup.end()) {
		return 0.0;
	}
	return std::clamp(
		ws_.incentives[static_cast<std::size_t>(found->second)].tax_holiday,
		0.0, 1.0);
}

double SimCore::demo_sector_subsidy(const std::string& nation,
		const std::string& sector) const {
	const auto found = ws_.incentive_lookup.find(nation + "|" + sector);
	if (found == ws_.incentive_lookup.end()) {
		return 0.0;
	}
	return std::max(0.0,
		ws_.incentives[static_cast<std::size_t>(found->second)].subsidy);
}

double SimCore::demo_nation_confidence(const std::string& nation) const {
	const auto found = ws_.confidence.find(nation);
	return found == ws_.confidence.end() ? 1.0
		: std::clamp(found->second, 0.0, 1.0);
}

double SimCore::demo_credit_nation_households(const std::string& nation,
		double amount) {
	if (amount <= 0.0) {
		return 0.0;
	}
	// F9-perf: só as cidades da nação (índice ws_.cities_by_nation), não o
	// mundo. Antes eram duas varreduras de 834 cidades por grupo com dividendo
	// a pagar — a fase `fiscal` do tick pagava isso todo dia.
	const auto found = ws_.cities_by_nation.find(nation);
	if (found == ws_.cities_by_nation.end() || found->second.empty()) {
		return 0.0;
	}
	const std::vector<int>& indices = found->second;
	// O dono do capital é aproximado pelo estrato de maior escolaridade (a
	// proxy disponível no modelo para os proprietários); sem ele, população.
	double weight_total = 0.0;
	for (const int index : indices) {
		const DemoCity& city = ws_.cities[static_cast<std::size_t>(index)];
		double weight = city.pop.class_population[kDemoEducationLevels - 1];
		if (!(weight > 0.0)) {
			weight = city.simulation_population;
		}
		weight_total += weight;
	}
	if (!(weight_total > 0.0)) {
		return 0.0;
	}
	for (const int index : indices) {
		DemoCity& city = ws_.cities[static_cast<std::size_t>(index)];
		double weight = city.pop.class_population[kDemoEducationLevels - 1];
		if (!(weight > 0.0)) {
			weight = city.simulation_population;
		}
		city.capital_income_next += amount * weight / weight_total;
	}
	return amount;
}

void SimCore::demo_route_group_profit(int group_index,
		const std::string& host_nation, double profit, double revenue,
		DemoCity& city, double& firm_surplus) {
	if (group_index < 0 || group_index >= static_cast<int>(ws_.groups.size())) {
		return;
	}
	DemoGroup& group = ws_.groups[static_cast<std::size_t>(group_index)];
	group.revenue_total += revenue;
	group.profit_total += profit;
	if (profit > 0.0) {
		if (group.state_owned) {
			// Lucro de estatal -> orçamento do anfitrião (dinheiro conservado).
			const double moved = std::min(profit, firm_surplus);
			city.firm_cash -= moved;
			city.government_budget += moved;
			firm_surplus -= moved;
			return;
		}
		// Imposto corporativo: fica no anfitrião. A isenção do incentivo
		// setorial reduz a alíquota.
		const double holiday = demo_sector_tax_holiday(host_nation, group.sector);
		const double tax_rate = std::clamp(
			kDemoCorporateTaxRate * (1.0 - holiday), 0.0, 1.0);
		const double tax = std::min(profit * tax_rate, firm_surplus);
		city.firm_cash -= tax;
		city.government_budget += tax;
		firm_surplus -= tax;
		group.taxes_total += tax;
		ws_.group_flows[host_nation].taxes += tax;
		const double net = profit - tax;
		const double moved = std::min(net, firm_surplus);
		city.firm_cash -= moved;
		group.cash += moved;
		group.distributable += moved;
		firm_surplus -= moved;
		group.profit_by_host[host_nation] += moved;
		// Incentivo: subsídio ad valorem sobre a receita do ativo, pago pelo
		// governo do anfitrião (política explícita; 0 por padrão).
		const double subsidy_rate = demo_sector_subsidy(host_nation, group.sector);
		if (subsidy_rate > 0.0 && revenue > 0.0) {
			const double subsidy = std::min(
				std::max(0.0, city.government_budget), revenue * subsidy_rate);
			if (subsidy > 0.0) {
				city.government_budget -= subsidy;
				group.cash += subsidy;
				group.distributable += subsidy;
				ws_.group_flows[host_nation].subsidy += subsidy;
			}
		}
	} else if (profit < 0.0) {
		// O grupo recapitaliza o ativo até o limite do caixa; o resto da perda
		// continua na firma (pode levar a falência, como antes).
		const double inject = std::min(-profit, group.cash);
		if (inject > 0.0) {
			group.cash -= inject;
			city.firm_cash += inject;
		}
	}
}

// ------------------------------------------------------------------
// S6b — helpers de load/setup movidos da fachada (economy_sim.cpp).
// ------------------------------------------------------------------

void SimCore::demo_rebuild_tariff_lookup() {
	ws_.tariff_lookup.clear();
	ws_.tariff_lookup.reserve(ws_.tariffs.size() * 2);
	for (const DemoTariff& tariff : ws_.tariffs) {
		ws_.tariff_lookup[tariff.buyer + '|' + tariff.seller + '|'
			+ tariff.product] = std::clamp(tariff.rate, 0.0, 1.0);
	}
}

void SimCore::demo_apply_labor_profile(DemoCity& city) {
	DemoPop& pop = city.pop;
	const double population = std::max(0.0, city.simulation_population);
	pop.population = population;
	const double t = std::clamp(
		std::clamp(city.external_income_index, 0.0, 8.0) / kLaborIncomeSaturation,
		0.0, 1.0);
	pop.working_age = population * (kLaborWorkingAgeLow
		+ (kLaborWorkingAgeHigh - kLaborWorkingAgeLow) * t);
	pop.retirees = population * (kLaborRetireeLow
		+ (kLaborRetireeHigh - kLaborRetireeLow) * t);
	pop.recruit_reserve = pop.working_age * kLaborRecruitShare;
	const double basic = kLaborBasicEduLow
		+ (kLaborBasicEduHigh - kLaborBasicEduLow) * t;
	double higher = kLaborHigherEduLow
		+ (kLaborHigherEduHigh - kLaborHigherEduLow) * t;
	higher = std::min(higher, std::max(0.0, 1.0 - basic - 0.02));
	const double intermediate = std::max(0.0, 1.0 - basic - higher);
	pop.education[0] = pop.working_age * basic;
	pop.education[1] = pop.working_age * intermediate;
	pop.education[2] = pop.working_age * higher;
	const double country_participation = kLaborParticipationLow
		+ (kLaborParticipationHigh - kLaborParticipationLow) * t;
	const double scale = country_participation / kLaborParticipationByLevel[0];
	for (int level = 0; level < kDemoEducationLevels; ++level) {
		pop.participation[level] = std::clamp(
			kLaborParticipationByLevel[level] * scale, 0.35, 0.92);
	}
	const double student_share = kLaborStudentShareLow
		+ (kLaborStudentShareHigh - kLaborStudentShareLow) * t;
	const double students = pop.working_age * student_share;
	pop.studying[0] = students * 0.60;
	pop.studying[1] = students * 0.40;
	pop.students = pop.studying[0] + pop.studying[1];
	const double factor = labor_income_factor(city.external_income_index);
	const auto minimum = ws_.minimum_wages.find(city.nation);
	const double floor = minimum != ws_.minimum_wages.end()
		? std::max(0.0, minimum->second) : 0.0;
	pop.labor_force = 0.0;
	for (int level = 0; level < kDemoEducationLevels; ++level) {
		const double studying = level < kDemoEducationLevels - 1
			? pop.studying[level] : 0.0;
		pop.labor_force_by_level[level] = std::max(0.0,
			pop.education[level] * pop.participation[level] - studying);
		pop.employed_by_level[level] = 0.0;
		pop.unemployed_by_level[level] = pop.labor_force_by_level[level];
		pop.vacancies_by_level[level] = 0.0;
		pop.unfilled_by_level[level] = 0.0;
		pop.wage_income_by_level[level] = 0.0;
		pop.wage_by_level[level] = std::max(floor,
			kLaborWageInitial * kLaborWageRatio[level] * factor);
		pop.labor_force += pop.labor_force_by_level[level];
	}
	pop.wage_anchored = false;
	pop.employed = 0.0;
	pop.unemployed = pop.labor_force;
	pop.dependents = std::max(0.0, population - pop.labor_force);
	pop.wage_income = 0.0;
	// A renda básica é sempre permitida; life/luxury dependem da renda real
	// (income_level calculado no fim da fase de renda).
	pop.income_level = 1;
	if (pop.level_spend.size() != ws_.needs_levels.size()) {
		pop.level_spend.assign(ws_.needs_levels.size(), 0.0);
	}
	demo_refresh_strata(pop);
	demo_refresh_income_classes(city);
}

void SimCore::demo_seed_groups() {
	if (ws_.groups_seeded || !ws_.groups.empty()) {
		return;
	}
	// Agrupa por (nação, setor do produto) — a regra documentada no
	// GAME_STATE/docs. `std::map` mantém a ordem determinística.
	std::map<std::string, std::vector<int>> buckets;
	for (std::size_t index = 0; index < ws_.buildings.size(); ++index) {
		const DemoBuilding& building = ws_.buildings[index];
		if (building.owner != "private") {
			continue;
		}
		const DemoCity* city = find_demo_city(building.city);
		if (city == nullptr) {
			continue;
		}
		std::string sector;
		if (building.output_product >= 0
				&& building.output_product < static_cast<int>(ws_.product_sectors.size())) {
			sector = ws_.product_sectors[static_cast<std::size_t>(building.output_product)];
		}
		if (sector.empty()) {
			sector = (building.recipe >= 0 || building.input_product >= 0)
				? "industry" : "primary";
		}
		buckets[city->nation + "|" + sector].push_back(static_cast<int>(index));
	}
	int chunk_serial = 0;
	for (auto& entry : buckets) {
		std::vector<int>& indices = entry.second;
		const std::size_t separator = entry.first.find('|');
		const std::string nation = entry.first.substr(0, separator);
		const std::string sector = separator == std::string::npos
			? std::string("primary") : entry.first.substr(separator + 1);
		std::sort(indices.begin(), indices.end(), [this](int a, int b) {
			return ws_.buildings[static_cast<std::size_t>(a)].id
				< ws_.buildings[static_cast<std::size_t>(b)].id;
		});
		for (std::size_t start = 0; start < indices.size();
				start += static_cast<std::size_t>(kDemoGroupChunkAssets)) {
			DemoGroup group;
			group.id = "G-" + demo_sane_token(nation) + "-"
				+ demo_sane_token(sector) + "-" + std::to_string(chunk_serial);
			group.origin_nation = nation;
			group.sector = sector;
			const int group_index = static_cast<int>(ws_.groups.size());
			ws_.group_lookup[group.id] = group_index;
			ws_.groups.push_back(group);
			const std::size_t end = std::min(indices.size(),
				start + static_cast<std::size_t>(kDemoGroupChunkAssets));
			for (std::size_t i = start; i < end; ++i) {
				DemoBuilding& building =
					ws_.buildings[static_cast<std::size_t>(indices[i])];
				building.owner = "national:" + group.id;
			}
			++chunk_serial;
		}
	}
	ws_.groups_seeded = true;
}

int SimCore::demo_recipe_index(const std::string& recipe_id) const {
	const auto found = ws_.recipe_lookup.find(recipe_id);
	if (found == ws_.recipe_lookup.end()) {
		return -1;
	}
	return found->second;
}

} // namespace ogs
