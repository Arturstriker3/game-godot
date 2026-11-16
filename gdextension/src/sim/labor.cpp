// Módulo de domínio: trabalho — mercado de trabalho, educação, estratos,
// classes, salários e âncoras setoriais (S12 do docs/economy/core_refactor_plan.md).
// SimCore:: métodos movidos de sim_core.cpp — realocação byte-idêntica.
#include "sim_core.h"

#include "profiling.h"
#include "sim_constants.h"
#include "sim_labor.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace ogs {
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
	// S16: o trabalho é independente por cidade (escreve só na própria cidade e
	// nas suas construções; lê só dados globais imutáveis no dia) — paralelizável
	// com resultado bit-idêntico ao serial. A redução não é necessária (não há
	// acumulador global nesta fase).
	auto process_city = [this, &opex_factor](std::size_t begin, std::size_t end) {
		for (std::size_t city_index = begin; city_index < end; ++city_index) {
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
	};
	pool().parallel_for(ws_.cities.size(), process_city);

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
} // namespace ogs
