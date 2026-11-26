// H1g — save/load por domínio: população por cidade.
//
// Blocos movidos VERBATIM de save_state.cpp (mesmas linhas, mesma ordem de
// escrita de chaves). Qualquer reordenação muda os bytes do save.
#include "save_internal.h"

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

namespace pop {

void write(Dictionary& entry, const City& city, const WorldState& ws,
		const SimCore& core) {
	Dictionary pop = make_object();
	pop["population"] = city.pop.population;
	pop["labor_force"] = city.pop.labor_force;
	pop["employed"] = city.pop.employed;
	pop["unemployed"] = city.pop.unemployed;
	pop["dependents"] = city.pop.dependents;
	pop["wage_income"] = city.pop.wage_income;
	pop["capital_income"] = city.pop.capital_income;
	pop["transfers"] = city.pop.transfers;
	pop["taxes"] = city.pop.taxes;
	pop["service_spend"] = city.pop.service_spend;
	pop["disposable"] = city.pop.disposable;
	pop["external_income"] = city.pop.external_income;
	pop["food_spend"] = city.pop.food_spend;
	pop["consumed"] = city.pop.consumed;
	pop["need"] = city.pop.need;
	pop["satisfaction"] = city.pop.satisfaction;
	// B2: quando os quintis existem, a poupança gravada é a SOMA deles
	// (mesma ordem do load) — o round-trip sem tick fica bit-exato; o
	// escalar é só um cache da soma.
	double savings_saved = city.pop.savings;
	if (city.pop.strata_ready) {
		savings_saved = 0.0;
		for (int s = 0; s < kIncomeStrata; ++s) {
			savings_saved += city.pop.stratum_savings[s];
		}
	}
	pop["savings"] = savings_saved;
	// C1 (save v26): salário atrasado a receber (ativo das famílias;
	// não é poupança e não entra em `money_total`).
	pop["arrears"] = city.pop.arrears_receivable;
	Array desired_by_category = make_array();
	Array consumed_by_category = make_array();
	for (int product = 0; product < static_cast<int>(ws.food_products.size()); ++product) {
		desired_by_category.append(city.pop.desired_by_category[product]);
		consumed_by_category.append(city.pop.consumed_by_category[product]);
	}
	pop["desired_by_category"] = desired_by_category;
	pop["consumed_by_category"] = consumed_by_category;
	// F3: estado das necessidades (níveis, duráveis e gasto próprio). Campos
	// aditivos: save v4 anterior continua carregando (default zero).
	Dictionary needs_state = make_object();
	needs_state["level"] = city.pop.needs_level;
	needs_state["satisfaction"] = city.pop.needs_satisfaction;
	needs_state["spend"] = city.pop.needs_spend;
	needs_state["durable_spend"] = city.pop.durable_spend;
	Array level_satisfaction = make_array();
	Array level_want = make_array();
	Array level_consumed = make_array();
	for (int level = 0; level < kMaxNeedsLevels; ++level) {
		level_satisfaction.append(level < static_cast<int>(
			city.pop.level_satisfaction.size()) ? city.pop.level_satisfaction[level] : 0.0);
		level_want.append(level < static_cast<int>(
			city.pop.level_want.size()) ? city.pop.level_want[level] : 0.0);
		level_consumed.append(level < static_cast<int>(
			city.pop.level_consumed.size()) ? city.pop.level_consumed[level] : 0.0);
	}
	needs_state["level_satisfaction"] = level_satisfaction;
	needs_state["level_want"] = level_want;
	needs_state["level_consumed"] = level_consumed;
	Dictionary durable_stock = make_object();
	for (int product = 0; product < ws.products
			&& product < static_cast<int>(city.pop.durable_stock.size()); ++product) {
		if (city.pop.durable_stock[static_cast<std::size_t>(product)] > 0.0) {
			durable_stock[String(core.product_name(product))] =
				city.pop.durable_stock[static_cast<std::size_t>(product)];
		}
	}
	needs_state["durable_stock"] = durable_stock;
	pop["needs"] = needs_state;
	// B1 (save v16): coortes etárias (5 faixas) e os pesos de referência de
	// saúde/educação do dia da semeadura. Array vazio = cidade sem estrutura
	// nacional (perfil paramétrico legado, sem coorte a persistir).
	Array age_bands = make_array();
	if (city.pop.demography_ready) {
		for (int band = 0; band < kAgeBands; ++band) {
			age_bands.append(std::max(0.0, city.pop.age_band[band]));
		}
	}
	pop["age_bands"] = age_bands;
	pop["health_weight_ref"] = std::max(0.0, city.pop.health_weight_ref);
	pop["education_weight_ref"] =
		std::max(0.0, city.pop.education_weight_ref);
	// F7: mercado de trabalho e educação (save v9). Bloco aditivo: os campos
	// de compatibilidade acima continuam iguais.
	Dictionary labor = make_object();
	labor["working_age"] = city.pop.working_age;
	labor["retirees"] = city.pop.retirees;
	labor["students"] = city.pop.students;
	labor["recruit_reserve"] = city.pop.recruit_reserve;
	labor["education"] = Array{city.pop.education[0],
		city.pop.education[1], city.pop.education[2]};
	labor["participation"] = Array{city.pop.participation[0],
		city.pop.participation[1], city.pop.participation[2]};
	labor["labor_force_by_level"] = Array{
		city.pop.labor_force_by_level[0],
		city.pop.labor_force_by_level[1],
		city.pop.labor_force_by_level[2]};
	labor["employed_by_level"] = Array{city.pop.employed_by_level[0],
		city.pop.employed_by_level[1], city.pop.employed_by_level[2]};
	labor["unemployed_by_level"] = Array{
		city.pop.unemployed_by_level[0],
		city.pop.unemployed_by_level[1],
		city.pop.unemployed_by_level[2]};
	labor["vacancies_by_level"] = Array{
		city.pop.vacancies_by_level[0],
		city.pop.vacancies_by_level[1],
		city.pop.vacancies_by_level[2]};
	labor["unfilled_by_level"] = Array{
		city.pop.unfilled_by_level[0],
		city.pop.unfilled_by_level[1],
		city.pop.unfilled_by_level[2]};
	labor["wage_by_level"] = Array{city.pop.wage_by_level[0],
		city.pop.wage_by_level[1], city.pop.wage_by_level[2]};
	labor["wage_income_by_level"] = Array{
		city.pop.wage_income_by_level[0],
		city.pop.wage_income_by_level[1],
		city.pop.wage_income_by_level[2]};
	labor["studying"] = Array{city.pop.studying[0], city.pop.studying[1]};
	labor["income_level"] = city.pop.income_level;
	labor["wage_anchored"] = city.pop.wage_anchored;
	labor["income_level_share"] = Array{city.pop.income_level_share[0],
		city.pop.income_level_share[1], city.pop.income_level_share[2],
		city.pop.income_level_share[3]};
	labor["income_level_by_stratum"] = Array{
		city.pop.stratum_income_level[0],
		city.pop.stratum_income_level[1],
		city.pop.stratum_income_level[2]};
	pop["labor"] = labor;
	// B2 (save v17): cinco quintis de renda. Aditivo — o save anterior
	// migra semeando a proxy do Gini declarado (strata.cpp).
	Dictionary strata = make_object();
	Array strata_income_share = make_array();
	Array strata_people = make_array();
	Array strata_wage_factor = make_array();
	Array strata_capital_share = make_array();
	Array strata_savings = make_array();
	Array strata_debt = make_array();
	Array strata_equity = make_array();
	Array strata_wealth = make_array();
	Array strata_saving_rate = make_array();
	Array strata_net_income = make_array();
	Array strata_rent_paid = make_array();
	Array strata_level = make_array();
	Array strata_education = make_array();
	Array strata_tenure = make_array();
	Array strata_lifecycle = make_array();
	for (int s = 0; s < kIncomeStrata; ++s) {
		strata_income_share.append(city.pop.strata_income_share[s]);
		strata_people.append(city.pop.stratum_people[s]);
		strata_wage_factor.append(city.pop.stratum_wage_factor[s]);
		strata_capital_share.append(city.pop.stratum_capital_share[s]);
		strata_savings.append(city.pop.stratum_savings[s]);
		strata_debt.append(city.pop.stratum_debt[s]);
		strata_equity.append(city.pop.stratum_equity[s]);
		strata_wealth.append(city.pop.stratum_wealth[s]);
		strata_saving_rate.append(city.pop.stratum_saving_rate[s]);
		strata_net_income.append(city.pop.stratum_net_income[s]);
		strata_rent_paid.append(city.pop.stratum_rent_paid[s]);
		strata_level.append(city.pop.stratum_level[s]);
		Array education_row = make_array();
		Array tenure_row = make_array();
		for (int e = 0; e < kEducationLevels; ++e) {
			education_row.append(city.pop.stratum_education_mix[s][e]);
		}
		for (int kind = 0; kind < kStrataTenureCount; ++kind) {
			tenure_row.append(city.pop.stratum_tenure[s][kind]);
		}
		strata_education.append(education_row);
		strata_tenure.append(tenure_row);
	}
	for (int band = 0; band < 5; ++band) {
		strata_lifecycle.append(city.pop.lifecycle_saving[band]);
	}
	strata["ready"] = city.pop.strata_ready;
	strata["income_share"] = strata_income_share;
	strata["people"] = strata_people;
	strata["wage_factor"] = strata_wage_factor;
	strata["capital_share"] = strata_capital_share;
	strata["savings"] = strata_savings;
	strata["debt"] = strata_debt;
	strata["equity"] = strata_equity;
	strata["wealth"] = strata_wealth;
	strata["saving_rate"] = strata_saving_rate;
	strata["net_income"] = strata_net_income;
	strata["rent_paid"] = strata_rent_paid;
	strata["level"] = strata_level;
	strata["education"] = strata_education;
	strata["tenure"] = strata_tenure;
	strata["lifecycle"] = strata_lifecycle;
	pop["strata"] = strata;
	// B7 (save v22): informalidade, mortalidade e insatisfação (urbano).
	pop["informal"] = save_informal_block(city.pop.informal_share,
		city.pop.informal_labor_share, city.pop.informal_income,
		city.pop.informal_wage_bill, city.pop.informal_service_workers,
		city.pop.informal_service_output, city.pop.informal_service_demand,
		city.pop.informal_service_revenue, city.pop.informal_service_price,
		city.pop.informal_service_share);
	pop["mortality"] = save_mortality_block(city.pop.deaths_expected,
		city.pop.deaths_total, city.pop.excess_deaths_total,
		city.pop.excess_deaths_famine_total, city.pop.famine_months,
		city.pop.food_price_ref, city.pop.food_inflation_month,
		city.pop.health_access_ref, city.pop.satisfaction_prev);
	pop["dissatisfaction"] = save_dissatisfaction_block(
		city.pop.dissatisfaction, city.pop.dissatisfaction_by_stratum,
		city.pop.dissatisfaction_unemployment, city.pop.dissatisfaction_rent,
		city.pop.dissatisfaction_food, city.pop.dissatisfaction_inflation,
		city.pop.dissatisfaction_informality);
	entry["pop"] = pop;
}

void read(Dictionary& entry, City& city, const WorldState& ws,
		SimCore& core, int version) {
	Dictionary pop = entry.get("pop", Dictionary());
	city.pop.population = double(pop.get("population", city.simulation_population));
	city.pop.labor_force = double(pop.get("labor_force", 0.0));
	city.pop.employed = double(pop.get("employed", 0.0));
	city.pop.unemployed = double(pop.get("unemployed", 0.0));
	city.pop.dependents = double(pop.get("dependents", 0.0));
	city.pop.wage_income = double(pop.get("wage_income", 0.0));
	city.pop.capital_income = double(pop.get("capital_income", 0.0));
	city.pop.transfers = double(pop.get("transfers", 0.0));
	city.pop.taxes = double(pop.get("taxes", 0.0));
	city.pop.service_spend = double(pop.get("service_spend", 0.0));
	city.pop.disposable = double(pop.get("disposable", 0.0));
	city.pop.external_income = double(pop.get("external_income", 0.0));
	city.pop.food_spend = double(pop.get("food_spend", 0.0));
	city.pop.consumed = double(pop.get("consumed", 0.0));
	city.pop.need = double(pop.get("need", 0.0));
	city.pop.satisfaction = double(pop.get("satisfaction", 0.0));
	city.pop.savings = double(pop.get("savings", 0.0));
	// C1 (save v26): salário atrasado a receber. v25 e anteriores = 0.
	city.pop.arrears_receivable = version >= 26
		? std::max(0.0, double(pop.get("arrears", 0.0))) : 0.0;
	// F3: estado das necessidades (aditivo; save anterior = zero).
	core.init_pop_needs(city.pop);
	Dictionary needs_state = pop.get("needs", Dictionary());
	city.pop.needs_level = int(needs_state.get("level", 0));
	city.pop.needs_satisfaction = double(needs_state.get("satisfaction",
		city.pop.satisfaction));
	city.pop.needs_spend = double(needs_state.get("spend", 0.0));
	city.pop.durable_spend = double(needs_state.get("durable_spend", 0.0));
	Array saved_level_sat = needs_state.get("level_satisfaction", Array());
	Array saved_level_want = needs_state.get("level_want", Array());
	Array saved_level_cons = needs_state.get("level_consumed", Array());
	for (int level = 0; level < kMaxNeedsLevels; ++level) {
		city.pop.level_satisfaction[level] = level < saved_level_sat.size()
			? double(saved_level_sat[level]) : 0.0;
		city.pop.level_want[level] = level < saved_level_want.size()
			? double(saved_level_want[level]) : 0.0;
		city.pop.level_consumed[level] = level < saved_level_cons.size()
			? double(saved_level_cons[level]) : 0.0;
	}
	Dictionary saved_durable = needs_state.get("durable_stock", Dictionary());
	for (int product = 0; product < ws.products
			&& product < static_cast<int>(city.pop.durable_stock.size()); ++product) {
		const String key = String(core.product_name(product));
		if (saved_durable.has(key)) {
			city.pop.durable_stock[static_cast<std::size_t>(product)] =
				std::max(0.0, double(saved_durable[key]));
		}
	}
	// F7: mercado de trabalho e educação (save v9). Save v8 e anteriores não
	// têm o bloco: o perfil é derivado do índice de renda já gravado. A
	// força de trabalho, o emprego e os salários são recalculados pelo
	// mesmo modelo do primeiro dia (migração documentada).
	Dictionary saved_labor = pop.get("labor", Dictionary());
	const bool has_labor = version >= 9 && !saved_labor.is_empty();
	if (!has_labor) {
		core.apply_labor_profile(city);
	} else {
		Pop& labor_pop = city.pop;
		labor_pop.working_age = std::max(0.0,
			double(saved_labor.get("working_age", labor_pop.working_age)));
		labor_pop.retirees = std::max(0.0,
			double(saved_labor.get("retirees", 0.0)));
		labor_pop.students = std::max(0.0,
			double(saved_labor.get("students", 0.0)));
		labor_pop.recruit_reserve = std::max(0.0,
			double(saved_labor.get("recruit_reserve", 0.0)));
		const auto read_levels = [&saved_labor](const char* key,
				double* out, int count) {
			const Array values = saved_labor.get(key, Array());
			for (int level = 0; level < count; ++level) {
				out[level] = level < values.size()
					? std::max(0.0, double(values[level])) : out[level];
			}
		};
		read_levels("education", labor_pop.education,
			kEducationLevels);
		read_levels("participation", labor_pop.participation,
			kEducationLevels);
		read_levels("labor_force_by_level", labor_pop.labor_force_by_level,
			kEducationLevels);
		read_levels("employed_by_level", labor_pop.employed_by_level,
			kEducationLevels);
		read_levels("unemployed_by_level", labor_pop.unemployed_by_level,
			kEducationLevels);
		read_levels("vacancies_by_level", labor_pop.vacancies_by_level,
			kEducationLevels);
		// B6: o save JÁ grava `unfilled_by_level` mas o load não o lia — o
		// round-trip sem tick deixava de ser bit-exato (campo derivado que
		// volta do save; o próximo `labor_day` o recalcula).
		read_levels("unfilled_by_level", labor_pop.unfilled_by_level,
			kEducationLevels);
		read_levels("wage_by_level", labor_pop.wage_by_level,
			kEducationLevels);
		read_levels("wage_income_by_level",
			labor_pop.wage_income_by_level, kEducationLevels);
		read_levels("studying", labor_pop.studying,
			kEducationLevels - 1);
		// B1: `students` é a POP fora da força em formação — crianças
		// (coorte 0-14) + quem estuda (ver apply_labor_profile/labor_day).
		labor_pop.students = labor_pop.children + labor_pop.studying[0]
			+ labor_pop.studying[1];
		labor_pop.income_level = std::clamp(
			int(saved_labor.get("income_level", 1)), 0, 3);
		// F7 calibração: blocos aditivos. Save anterior ao calibração (v9
		// sem eles) cai no nível único e mantém o salário carregado como
		// histórico (wage_anchored = true, com inércia).
		const bool has_strata = saved_labor.has("income_level_by_stratum");
		if (has_strata) {
			const Array strata = saved_labor.get("income_level_by_stratum", Array());
			for (int stratum = 0; stratum < kEducationLevels; ++stratum) {
				labor_pop.stratum_income_level[stratum] = stratum < strata.size()
					? std::clamp(int(strata[stratum]), 0, 3)
					: labor_pop.income_level;
			}
		} else {
			for (int stratum = 0; stratum < kEducationLevels; ++stratum) {
				labor_pop.stratum_income_level[stratum] = labor_pop.income_level;
			}
		}
		const Array share = saved_labor.get("income_level_share", Array());
		for (int level_index = 0; level_index < kIncomeLevels; ++level_index) {
			labor_pop.income_level_share[level_index] = level_index < share.size()
				? std::clamp(double(share[level_index]), 0.0, 1.0) : 0.0;
		}
		if (share.is_empty()) {
			labor_pop.income_level_share[labor_pop.income_level] = 1.0;
		}
		labor_pop.wage_anchored = bool(saved_labor.get("wage_anchored", true));
		labor_pop.labor_force = 0.0;
		for (int level = 0; level < kEducationLevels; ++level) {
			labor_pop.labor_force +=
				labor_pop.labor_force_by_level[level];
		}
		labor_pop.employed = 0.0;
		labor_pop.unemployed = 0.0;
		for (int level = 0; level < kEducationLevels; ++level) {
			labor_pop.employed += labor_pop.employed_by_level[level];
			labor_pop.unemployed += labor_pop.unemployed_by_level[level];
		}
		labor_pop.wage_income = 0.0;
		for (int level = 0; level < kEducationLevels; ++level) {
			labor_pop.wage_income += labor_pop.wage_income_by_level[level];
		}
		labor_pop.dependents = std::max(0.0,
			city.pop.population - labor_pop.labor_force);
	}
	// B1 (save v16): coortes etárias, DEPOIS do bloco de trabalho. O save v16
	// restaura as bandas e reaplica o que é derivado delas (idade ativa,
	// crianças, fatores de saúde/educação). O v15 e anteriores não têm as
	// bandas: são semeadas da estrutura nacional e a escolaridade guardada é
	// reescalada pela razão da idade ativa, para o perfil paramétrico antigo
	// não ficar inconsistente com as coortes até o primeiro mês. Sem dado
	// para a nação, a cidade fica no perfil paramétrico legado.
	{
		const Array saved_age_bands = pop.get("age_bands", Array());
		double age_total = 0.0;
		const bool has_age_bands = version >= 16
			&& saved_age_bands.size() >= kAgeBands;
		for (int band = 0; band < kAgeBands && has_age_bands; ++band) {
			age_total += std::max(0.0, double(saved_age_bands[band]));
		}
		if (has_age_bands && age_total > 0.0) {
			for (int band = 0; band < kAgeBands; ++band) {
				city.pop.age_band[band] =
					std::max(0.0, double(saved_age_bands[band]));
			}
			city.pop.health_weight_ref = std::max(0.0,
				double(pop.get("health_weight_ref", 0.0)));
			city.pop.education_weight_ref = std::max(0.0,
				double(pop.get("education_weight_ref", 0.0)));
			city.pop.demography_ready = true;
			core.apply_demography(city);
			// O bloco de trabalho acima montou `students` antes de as
			// coortes existirem (`children` ainda 0). Recalcula aqui para o
			// status logo após o load e um save sem tick fecharem exato.
			city.pop.students = city.pop.children + city.pop.studying[0]
				+ city.pop.studying[1];
			city.pop.dependents = std::max(0.0,
				city.pop.population - city.pop.labor_force);
		} else {
			const double old_working = std::max(0.0, city.pop.working_age);
			core.seed_demography(city);
			if (city.pop.demography_ready) {
				const double new_working = city.pop.age_band[1]
					+ city.pop.age_band[2] + city.pop.age_band[3];
				const double ratio = old_working > 1e-6
					? std::clamp(new_working / old_working, 0.5, 2.0) : 1.0;
				for (int level = 0; level < kEducationLevels; ++level) {
					city.pop.education[level] *= ratio;
				}
				for (int track = 0; track < kEducationLevels - 1; ++track) {
					city.pop.studying[track] *= ratio;
				}
				core.apply_demography(city);
				city.pop.labor_force = 0.0;
				city.pop.employed = 0.0;
				city.pop.unemployed = 0.0;
				for (int level = 0; level < kEducationLevels; ++level) {
					const double studying = level < kEducationLevels - 1
						? city.pop.studying[level] : 0.0;
					city.pop.labor_force_by_level[level] = std::max(0.0,
						city.pop.education[level]
							* city.pop.participation[level] - studying);
					city.pop.employed_by_level[level] = std::min(
						city.pop.employed_by_level[level],
						city.pop.labor_force_by_level[level]);
					city.pop.unemployed_by_level[level] = std::max(0.0,
						city.pop.labor_force_by_level[level]
							- city.pop.employed_by_level[level]);
					city.pop.labor_force +=
						city.pop.labor_force_by_level[level];
					city.pop.employed += city.pop.employed_by_level[level];
					city.pop.unemployed += city.pop.unemployed_by_level[level];
				}
				city.pop.students = city.pop.children
					+ city.pop.studying[0] + city.pop.studying[1];
				city.pop.dependents = std::max(0.0,
					city.pop.population - city.pop.labor_force);
			}
		}
	}
	// B2 (save v17): cinco quintis de renda. Save v16 e anteriores não têm
	// o bloco: a semeadura acontece no FIM do load (depois do bloco
	// `urban`, para a posse de moradia já estar carregada) com a proxy do
	// Gini declarado. A poupança escalar existente é repartida pela renda.
	const Dictionary saved_strata = pop.get("strata", Dictionary());
	if (version >= 17 && !saved_strata.is_empty()) {
		const auto read_strata = [&saved_strata](const char* key,
				double* out) {
			const Array values = saved_strata.get(key, Array());
			for (int s = 0; s < kIncomeStrata; ++s) {
				if (s < values.size()) {
					out[s] = std::max(0.0, double(values[s]));
				}
			}
		};
		read_strata("income_share", city.pop.strata_income_share);
		read_strata("people", city.pop.stratum_people);
		read_strata("wage_factor", city.pop.stratum_wage_factor);
		read_strata("capital_share", city.pop.stratum_capital_share);
		read_strata("savings", city.pop.stratum_savings);
		read_strata("debt", city.pop.stratum_debt);
		read_strata("equity", city.pop.stratum_equity);
		read_strata("wealth", city.pop.stratum_wealth);
		read_strata("saving_rate", city.pop.stratum_saving_rate);
		read_strata("net_income", city.pop.stratum_net_income);
		read_strata("rent_paid", city.pop.stratum_rent_paid);
		const Array levels = saved_strata.get("level", Array());
		for (int s = 0; s < kIncomeStrata; ++s) {
			city.pop.stratum_level[s] = s < levels.size()
				? std::clamp(int(levels[s]), 0, kIncomeLevels - 1) : 0;
		}
		const Array education = saved_strata.get("education", Array());
		for (int s = 0; s < kIncomeStrata && s < education.size(); ++s) {
			if (education[s].get_type() != Variant::ARRAY) {
				continue;
			}
			const Array row = education[s];
			for (int e = 0; e < kEducationLevels && e < row.size(); ++e) {
				city.pop.stratum_education_mix[s][e] =
					std::max(0.0, double(row[e]));
			}
		}
		const Array tenure = saved_strata.get("tenure", Array());
		for (int s = 0; s < kIncomeStrata && s < tenure.size(); ++s) {
			if (tenure[s].get_type() != Variant::ARRAY) {
				continue;
			}
			const Array row = tenure[s];
			for (int kind = 0; kind < kStrataTenureCount
					&& kind < row.size(); ++kind) {
				city.pop.stratum_tenure[s][kind] =
					std::max(0.0, double(row[kind]));
			}
		}
		const Array lifecycle = saved_strata.get("lifecycle", Array());
		for (int band = 0; band < 5 && band < lifecycle.size(); ++band) {
			city.pop.lifecycle_saving[band] =
				std::max(0.0, double(lifecycle[band]));
		}
		city.pop.strata_ready = bool(saved_strata.get("ready", true));
		// O escalar da cidade É a soma dos quintis (round-trip exato).
		if (city.pop.strata_ready) {
			double total = 0.0;
			for (int s = 0; s < kIncomeStrata; ++s) {
				total += city.pop.stratum_savings[s];
			}
			city.pop.savings = total;
		}
	}
	// B7 (save v22): informalidade, mortalidade e insatisfação (urbano).
	// v21 e anteriores migram depois (segunda passada, com o dado da nação).
	if (version >= 22) {
		load_informal_block(pop.get("informal", Dictionary()),
			city.pop.informal_share, city.pop.informal_labor_share,
			city.pop.informal_income, city.pop.informal_wage_bill,
			city.pop.informal_service_workers, city.pop.informal_service_output,
			city.pop.informal_service_demand, city.pop.informal_service_revenue,
			city.pop.informal_service_price, city.pop.informal_service_share);
		load_mortality_block(pop.get("mortality", Dictionary()),
			city.pop.deaths_expected, city.pop.deaths_total,
			city.pop.excess_deaths_total, city.pop.excess_deaths_famine_total,
			city.pop.famine_months, city.pop.food_price_ref,
			city.pop.food_inflation_month, city.pop.health_access_ref,
			city.pop.satisfaction_prev);
		load_dissatisfaction_block(pop.get("dissatisfaction", Dictionary()),
			city.pop.dissatisfaction, city.pop.dissatisfaction_by_stratum,
			city.pop.dissatisfaction_unemployment, city.pop.dissatisfaction_rent,
			city.pop.dissatisfaction_food, city.pop.dissatisfaction_inflation,
			city.pop.dissatisfaction_informality);
		city.pop.informality_seeded = true;
	}
}

void read_categories(const Dictionary& entry, City& city,
		const WorldState& ws) {
	const Dictionary pop = entry.get("pop", Dictionary());
	Array desired_by_category = pop.get("desired_by_category", Array());
	Array consumed_by_category = pop.get("consumed_by_category", Array());
	city.pop.desired_by_category.assign(ws.food_products.size(), 0.0);
	city.pop.consumed_by_category.assign(ws.food_products.size(), 0.0);
	for (int product = 0; product < static_cast<int>(ws.food_products.size()); ++product) {
		city.pop.desired_by_category[product] =
			product < desired_by_category.size()
				? double(desired_by_category[product]) : 0.0;
		city.pop.consumed_by_category[product] =
			product < consumed_by_category.size()
				? double(consumed_by_category[product]) : 0.0;
	}
}

} // namespace pop

} // namespace ogs::save
