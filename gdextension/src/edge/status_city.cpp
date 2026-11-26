#include "status_city.h"

#include "economy_sim.h"

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_float64_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

namespace godot {

using namespace ogs;
void EconomySim::fill_city_status(std::size_t city_index,
		StatusCitySlot& slot, double in_transit, bool heavy) const {
	const City& city = ws_.cities[city_index];
	const StatusKeys& k = status_keys_;
	Dictionary& dict = slot.dict;
	// Taxa de cÃ¢mbio da naÃ§Ã£o, resolvida UMA vez por cidade (antes eram 2
	// lookups de moeda por cidade: aqui e no bloco de mercados).
	const double city_fx = fx_rate(city.nation);

	// Strings estÃ¡veis: comparam a origem (memcmp) antes de realocar a String.
	// `nation`/`home_nation` entram aqui porque o territÃ³rio pode mudar com a
	// polÃ­tica; id, nome e massa de terra sÃ£o imutÃ¡veis apÃ³s a inicializaÃ§Ã£o.
	auto sync_string = [&dict](std::string& src, String& stored,
			const std::string& value, const String& key) {
		if (src != value) {
			src = value;
			stored = String(value.c_str());
			dict[key] = stored;
		}
	};
	sync_string(slot.id_src, slot.id, city.id, k.id);
	sync_string(slot.name_src, slot.name, city.name, k.name);
	sync_string(slot.nation_src, slot.nation, city.nation, k.nation);
	sync_string(slot.home_src, slot.home_nation, city.home_nation, k.home_nation);
	sync_string(slot.landmass_src, slot.landmass, city.landmass, k.landmass);
	sync_string(slot.currency_src, slot.currency,
		std::string(currency_code(city.nation)), k.currency);

	// Campos que sÃ³ mudam quando um dado do territÃ³rio muda (aptidÃ£o, demanda
	// externa, posiÃ§Ã£o): reescritos apenas quando a versÃ£o estÃ¡tica sobe.
	if (!slot.static_filled || slot.static_version != status_static_version_) {
		dict[k.x] = city.center.x;
		dict[k.y] = city.center.y;
		slot.center.resize(2);
		slot.center[0] = city.center.x;
		slot.center[1] = city.center.y;
		dict[k.center] = slot.center;

		slot.suitability.resize(ws_.products);
		slot.external_demand.resize(ws_.products);
		for (int product = 0; product < ws_.products; ++product) {
			slot.suitability[product] = city.suitability[product];
			slot.external_demand[product] = city.external_demand[product];
		}
		dict[k.suitability] = slot.suitability;
		dict[k.external_demand] = slot.external_demand;
		slot.static_filled = true;
		slot.static_version = status_static_version_;
	}

	// A4: os escalares saem de `city_scalars` (POD) para a ficha e as colunas
	// usarem a MESMA conta. A ordem das escritas e dos espelhos nÃ£o muda.
	const CityScalars scalars = city_scalars(city_index, in_transit);
	double* scalar_mirror = slot.scalar_written;
	bool& scalar_filled = slot.scalar_written_filled;
	int scalar_index = 0;
	auto sync_scalar = [&dict, &k, scalar_mirror, &scalar_filled, &scalar_index](
			StringName key, double value) {
		if (!scalar_filled || scalar_mirror[scalar_index] != value) {
			dict[key] = value;
			scalar_mirror[scalar_index] = value;
		}
		++scalar_index;
	};
	sync_scalar(k.population, scalars.population);
	sync_scalar(k.simulation_population, scalars.simulation_population);
	sync_scalar(k.gdp, scalars.gdp);
	sync_scalar(k.gdp_per_capita, scalars.gdp_per_capita);
	sync_scalar(k.external_income_index, scalars.external_income_index);
	sync_scalar(k.firm_cash, scalars.firm_cash);
	sync_scalar(k.service_cash, scalars.service_cash);
	sync_scalar(k.government_budget, scalars.government_budget);
	sync_scalar(k.government_goods_spend, scalars.government_goods_spend);
	sync_scalar(k.service_revenue, scalars.service_revenue);
	sync_scalar(k.service_jobs, scalars.service_jobs);
	sync_scalar(k.service_workers, scalars.service_workers);
	sync_scalar(k.service_jobs_filled, scalars.service_jobs_filled);
	sync_scalar(k.in_kind_food, scalars.in_kind_food);
	sync_scalar(k.service_profit, scalars.service_profit);
	sync_scalar(k.opex_revenue, scalars.opex_revenue);
	sync_scalar(k.freight_revenue, scalars.freight_revenue);
	sync_scalar(k.construction_revenue, scalars.construction_revenue);
	sync_scalar(k.service_wage, scalars.service_wage);
	sync_scalar(k.disposable_per_capita, scalars.disposable_per_capita);
	sync_scalar(k.living_standard, scalars.living_standard);
	sync_scalar(k.production, scalars.production);
	sync_scalar(k.jobs, scalars.jobs);
	sync_scalar(k.income, scalars.income);
	sync_scalar(k.industrialization, scalars.industrialization);
	sync_scalar(k.stockpile, scalars.stockpile);
	sync_scalar(k.need, scalars.need);
	sync_scalar(k.deficit, scalars.deficit);
	sync_scalar(k.consumed, scalars.consumed);
	sync_scalar(k.in_transit, scalars.in_transit);
	sync_scalar(k.fx_rate, scalars.fx_rate);
	sync_scalar(k.tariff_revenue, scalars.tariff_revenue);
	sync_scalar(k.productivity_index, scalars.productivity_index);
	slot.scalar_written_filled = true;
	// R1a: com o caixa unico regional, o pool privado real. Chave aditiva - so
	// existe com a flag ligada (status desligado byte-identico ao anterior).
	// `firm_cash`/`service_cash` continuam publicados como estatistica por setor.
	if (city.unified_cash) {
		dict[StringName("private_cash")] = city.private_cash;
		// R1a (continuacao): fatia medida das firmas na receita privada (teto
		// externo e base do investidor). Diagnostico; chave aditiva do ON.
		dict[StringName("firm_revenue_share")] =
			city.measured_firm_revenue_share();
	}


	// F9: emprego setorial (4 posiÃ§Ãµes). Espelho de 8 doubles: sÃ³ reescreve o
	// PackedFloat64Array quando algum valor muda (o custo por escrita Ã© a
	// ponte GDExtension, nÃ£o o cÃ¡lculo).
	{
		bool changed = !slot.sector_written_filled
			|| slot.sector_workers.size() != 4;
		if (!changed) {
			for (int i = 0; i < 4; ++i) {
				if (slot.sector_written[i] != city.sector_workers[i]
						|| slot.sector_written[4 + i] != city.sector_share[i]) {
					changed = true;
					break;
				}
			}
		}
		if (changed) {
			slot.sector_workers.resize(4);
			slot.sector_share.resize(4);
			for (int i = 0; i < 4; ++i) {
				slot.sector_workers[i] = city.sector_workers[i];
				slot.sector_share[i] = city.sector_share[i];
				slot.sector_written[i] = city.sector_workers[i];
				slot.sector_written[4 + i] = city.sector_share[i];
			}
			dict[k.sector_workers] = slot.sector_workers;
			dict[k.sector_share] = slot.sector_share;
			slot.sector_written_filled = true;
		}
	}

	// S7-c: o detalhe pesado (deposits/pop/markets/services) sÃ³ Ã© publicado
	// quando `heavy` â€” isto Ã©, em get_city_status(id). O status diÃ¡rio publica
	// apenas os escalares leves e o bloco `light`.
	if (heavy) {
	// Reservas minerais declaradas (sÃ³ produtos com jazida conhecida; ausÃªncia
	// da chave = sem dado, nÃ£o "zero"). O bloco ESTÃTICO (reserva inicial,
	// qualidade, profundidade, eficiÃªncia, opex) Ã© escrito uma vez; por dia sÃ³
	// mudam reserva restante, extraÃ­do e esgotado. Antes o dicionÃ¡rio inteiro
	// era reconstruÃ­do a cada dia â€” custo que crescia com as cidades mineiras
	// do cenÃ¡rio real (o cenÃ¡rio de validaÃ§Ã£o nÃ£o tem depÃ³sito nenhum).
	if (slot.deposit_entry.size() < ws_.products) {
		slot.deposit_entry.resize(static_cast<std::size_t>(ws_.products));
		slot.deposit_present.assign(static_cast<std::size_t>(ws_.products), 0);
		slot.deposit_depleted.assign(static_cast<std::size_t>(ws_.products), 0);
		slot.deposit_written.assign(static_cast<std::size_t>(ws_.products),
			Deposit{});
		slot.deposit_key.clear();
		slot.deposit_key.reserve(static_cast<std::size_t>(ws_.products));
		for (int product = 0; product < ws_.products; ++product) {
			slot.deposit_key.emplace_back(product_name(product));
		}
	}
	// Jazida descoberta depois do primeiro dia (a IA/geografia chama
	// `set_deposit`): a lista de jazidas rastreadas Ã© recalculada uma vez e
	// sÃ³ as novas entram no dicionÃ¡rio (as antigas mantÃªm o bloco estÃ¡tico).
	if (slot.deposit_tracked_version != status_deposit_version_) {
		slot.deposit_tracked.clear();
		for (int product = 0; product < ws_.products; ++product) {
			if (city.deposits[product].tracked) {
				slot.deposit_tracked.push_back(product);
			}
		}
		slot.deposit_tracked_version = status_deposit_version_;
	}
	// `deposits` sÃ³ Ã© remontado quando a cidade tem jazida rastreada. Cidade
	// sem jazida publica o dicionÃ¡rio vazio UMA vez (a chave sempre existe:
	// ausÃªncia seria "sem dado", nÃ£o "sem reserva"); sem isso o bloco custava
	// ~4,5 ms/dia sÃ³ de referenciar 834 dicionÃ¡rios.
	if (slot.deposit_tracked.empty()) {
		if (!slot.deposits_published) {
			dict[k.deposits] = slot.deposits;
			slot.deposits_published = true;
		}
	} else {
		for (const int product : slot.deposit_tracked) {
			const Deposit& deposit = city.deposits[product];
			Dictionary& entry = slot.deposit_entry[static_cast<std::size_t>(product)];
			Deposit& previous = slot.deposit_written[static_cast<std::size_t>(product)];
			const bool first = slot.deposit_present[static_cast<std::size_t>(product)] == 0;
			if (first) {
				entry[k.initial_kt] = deposit.initial_kt;
				entry[k.quality] = deposit.quality;
				entry[k.depth_m] = deposit.depth_m;
				entry[k.efficiency] = extraction_efficiency(deposit);
				entry[k.opex_factor] = depth_opex_factor(deposit);
				slot.deposits[slot.deposit_key[static_cast<std::size_t>(product)]] = entry;
				slot.deposit_present[static_cast<std::size_t>(product)] = 1;
			}
			if (first || previous.remaining_kt != deposit.remaining_kt) {
				entry[k.remaining_kt] = deposit.remaining_kt;
				previous.remaining_kt = deposit.remaining_kt;
			}
			if (first || previous.extracted_kt != deposit.extracted_kt) {
				entry[k.extracted_kt] = deposit.extracted_kt;
				previous.extracted_kt = deposit.extracted_kt;
			}
			// D-086 (decisÃ£o do dono): o recurso nunca acaba â€” publica `depleted`
			// SEMPRE false (campo mantido por contrato de status/save).
			if (first || slot.deposit_depleted[static_cast<std::size_t>(product)] != 0) {
				entry[k.depleted] = false;
				slot.deposit_depleted[static_cast<std::size_t>(product)] = 0;
			}
		}
		// Dictionary Ã© por referÃªncia no Godot 4: uma Ãºnica atribuiÃ§Ã£o basta â€”
		// as entradas novas e os valores atualizados aparecem no publicado.
		// (Reatribuir todo dia custava ~1,4 Âµs Ã— 662 cidades â‰ˆ 0,9 ms.)
		if (!slot.deposits_published) {
			dict[k.deposits] = slot.deposits;
			slot.deposits_published = true;
		}
	}

	Dictionary& pop = slot.pop;
	// Valores diÃ¡rios: sÃ³ reescreve o que mudou. Cada `pop[chave] = valor` Ã©
	// uma chamada Ã  ponte GDExtension com hash da chave (~190 ns); com 20
	// campos Ã— 834 cidades isso custava ~3 ms/dia. `population`,
	// `labor_force` e `dependents` quase nunca mudam.
	{
		double* mirror = slot.pop_written;
		bool& filled = slot.pop_written_filled;
		int index = 0;
		auto sync_pop = [&pop, &k, mirror, &filled, &index](StringName key, double value) {
			if (!filled || mirror[index] != value) {
				pop[key] = value;
				mirror[index] = value;
			}
			++index;
		};
		sync_pop(k.population, city.pop.population);
		sync_pop(k.labor_force, city.pop.labor_force);
		sync_pop(k.employed, city.pop.employed);
		sync_pop(k.unemployed, city.pop.unemployed);
		sync_pop(k.dependents, city.pop.dependents);
		sync_pop(k.wage_income, city.pop.wage_income);
		sync_pop(k.capital_income, city.pop.capital_income);
		sync_pop(k.transfers, city.pop.transfers);
		sync_pop(k.taxes, city.pop.taxes);
		sync_pop(k.service_spend, city.pop.service_spend);
		sync_pop(k.disposable, city.pop.disposable);
		sync_pop(k.external_income, city.pop.external_income);
		sync_pop(k.food_spend, city.pop.food_spend);
		sync_pop(k.consumed, city.pop.consumed);
		sync_pop(k.need, city.pop.need);
		sync_pop(k.satisfaction, city.pop.satisfaction);
		sync_pop(k.savings, city.pop.savings);
		sync_pop(k.needs_level, double(city.pop.needs_level));
		sync_pop(k.needs_satisfaction, city.pop.needs_satisfaction);
		sync_pop(k.basic_satisfaction, city.pop.level_satisfaction.size() > 0
			? city.pop.level_satisfaction[0] : 0.0);
		sync_pop(k.life_satisfaction, city.pop.level_satisfaction.size() > 1
			? city.pop.level_satisfaction[1] : 0.0);
		sync_pop(k.luxury_satisfaction, city.pop.level_satisfaction.size() > 2
			? city.pop.level_satisfaction[2] : 0.0);
		sync_pop(k.needs_spend, city.pop.needs_spend);
		sync_pop(k.durable_spend, city.pop.durable_spend);
		double durable_total = 0.0;
		for (const double tons : city.pop.durable_stock) {
			durable_total += tons;
		}
		sync_pop(k.durable_stock, durable_total);
		// F7: populaÃ§Ã£o ativa, fora da forÃ§a e mercado de trabalho.
		sync_pop(k.working_age, city.pop.working_age);
		sync_pop(k.retirees, city.pop.retirees);
		sync_pop(k.students, city.pop.students);
		sync_pop(k.recruit_reserve, city.pop.recruit_reserve);
		sync_pop(k.income_level, double(city.pop.income_level));
		const auto minimum_wage = ws_.minimum_wages.find(city.nation);
		sync_pop(k.minimum_wage, minimum_wage != ws_.minimum_wages.end()
			? std::max(0.0, minimum_wage->second) : 0.0);
		sync_pop(k.wage_rigidity, ws_.wage_rigidity);
		if (!filled) {
			slot.education.resize(kEducationLevels);
			slot.education_share.resize(kEducationLevels);
			slot.labor_force_by_level.resize(kEducationLevels);
			slot.employed_by_level.resize(kEducationLevels);
			slot.unemployed_by_level.resize(kEducationLevels);
			slot.unemployment_by_level.resize(kEducationLevels);
			slot.vacancies_by_level.resize(kEducationLevels);
			slot.wage_by_level.resize(kEducationLevels);
			slot.wage_income_by_level.resize(kEducationLevels);
			slot.studying.resize(kEducationLevels - 1);
			slot.income_level_share.resize(kIncomeLevels);
			slot.income_level_by_stratum.resize(kEducationLevels);
			slot.stratum_disposable.resize(kEducationLevels);
			slot.stratum_population.resize(kEducationLevels);
			slot.stratum_wage_income.resize(kEducationLevels);
		}
		// Grupo "raro": educaÃ§Ã£o, forÃ§a por nÃ­vel, estudo, fatias de renda e o
		// nÃ­vel de consumo de cada estrato. Compara com o Ãºltimo publicado e sÃ³
		// toca nos Arrays quando algo muda (mudam em escala de semanas).
		{
			double rare[16];
			int ri = 0;
			for (int level = 0; level < kEducationLevels; ++level) {
				rare[ri++] = city.pop.education[level];
			}
			for (int level = 0; level < kEducationLevels; ++level) {
				rare[ri++] = city.pop.labor_force_by_level[level];
			}
			for (int track = 0; track < kEducationLevels - 1; ++track) {
				rare[ri++] = city.pop.studying[track];
			}
			for (int level = 0; level < kIncomeLevels; ++level) {
				rare[ri++] = city.pop.income_level_share[level];
			}
			for (int stratum = 0; stratum < kEducationLevels; ++stratum) {
				rare[ri++] = double(city.pop.stratum_income_level[stratum]);
			}
			if (!slot.labor_rare_filled
					|| std::memcmp(rare, slot.labor_rare_written,
						sizeof(double) * static_cast<std::size_t>(ri)) != 0) {
				for (int level = 0; level < kEducationLevels; ++level) {
					slot.education[level] = city.pop.education[level];
					slot.labor_force_by_level[level] =
						city.pop.labor_force_by_level[level];
				}
				for (int track = 0; track < kEducationLevels - 1; ++track) {
					slot.studying[track] = city.pop.studying[track];
				}
				for (int level = 0; level < kIncomeLevels; ++level) {
					slot.income_level_share[level] =
						city.pop.income_level_share[level];
				}
				for (int stratum = 0; stratum < kEducationLevels; ++stratum) {
					slot.income_level_by_stratum[stratum] =
						city.pop.stratum_income_level[stratum];
				}
				std::memcpy(slot.labor_rare_written, rare,
					sizeof(double) * static_cast<std::size_t>(ri));
				slot.labor_rare_filled = true;
			}
		}
		// Grupo "diÃ¡rio": salÃ¡rios, emprego, vagas e folha por escolaridade.
		{
			double daily[24];
			int di = 0;
			for (int level = 0; level < kEducationLevels; ++level) {
				daily[di++] = city.pop.working_age > 1e-9
					? city.pop.education[level] / city.pop.working_age : 0.0;
				daily[di++] = city.pop.employed_by_level[level];
				daily[di++] = city.pop.unemployed_by_level[level];
				daily[di++] = city.pop.labor_force_by_level[level] > 1e-9
					? city.pop.unemployed_by_level[level]
						/ city.pop.labor_force_by_level[level] : 0.0;
				daily[di++] = city.pop.vacancies_by_level[level];
				daily[di++] = city.pop.wage_by_level[level];
				daily[di++] = city.pop.wage_income_by_level[level];
			}
			if (!slot.labor_daily_filled
					|| std::memcmp(daily, slot.labor_daily_written,
						sizeof(double) * static_cast<std::size_t>(di)) != 0) {
				for (int level = 0; level < kEducationLevels; ++level) {
					slot.education_share[level] = city.pop.working_age > 1e-9
						? city.pop.education[level] / city.pop.working_age : 0.0;
					slot.employed_by_level[level] =
						city.pop.employed_by_level[level];
					slot.unemployed_by_level[level] =
						city.pop.unemployed_by_level[level];
					slot.unemployment_by_level[level] =
						city.pop.labor_force_by_level[level] > 1e-9
						? city.pop.unemployed_by_level[level]
							/ city.pop.labor_force_by_level[level] : 0.0;
					slot.vacancies_by_level[level] =
						city.pop.vacancies_by_level[level];
					slot.wage_by_level[level] = city.pop.wage_by_level[level];
					slot.wage_income_by_level[level] =
						city.pop.wage_income_by_level[level];
				}
				std::memcpy(slot.labor_daily_written, daily,
					sizeof(double) * static_cast<std::size_t>(di));
				slot.labor_daily_filled = true;
			}
		}
		// Renda por estrato: o memcmp evita reescrever os trÃªs vetores quando
		// os valores nÃ£o mudaram (populaÃ§Ã£o e folha do estrato variam devagar).
		{
			double strata[12];
			int si = 0;
			for (int stratum = 0; stratum < kEducationLevels; ++stratum) {
				strata[si++] = city.pop.stratum_disposable[stratum];
				strata[si++] = city.pop.stratum_population[stratum];
				strata[si++] = city.pop.stratum_wage_income[stratum];
			}
			if (!slot.stratum_written_filled
					|| std::memcmp(strata, slot.stratum_written,
						sizeof(double) * static_cast<std::size_t>(si)) != 0) {
				for (int stratum = 0; stratum < kEducationLevels; ++stratum) {
					slot.stratum_disposable[stratum] =
						city.pop.stratum_disposable[stratum];
					slot.stratum_population[stratum] =
						city.pop.stratum_population[stratum];
					slot.stratum_wage_income[stratum] =
						city.pop.stratum_wage_income[stratum];
				}
				std::memcpy(slot.stratum_written, strata,
					sizeof(double) * static_cast<std::size_t>(si));
				slot.stratum_written_filled = true;
			}
		}
		if (!filled) {
			pop[k.income_level_share] = slot.income_level_share;
			pop[k.income_level_by_stratum] = slot.income_level_by_stratum;
			pop[k.stratum_disposable] = slot.stratum_disposable;
			pop[k.stratum_population] = slot.stratum_population;
			pop[k.stratum_wage_income] = slot.stratum_wage_income;
			pop[k.education] = slot.education;
			pop[k.education_share] = slot.education_share;
			pop[k.labor_force_by_level] = slot.labor_force_by_level;
			pop[k.employed_by_level] = slot.employed_by_level;
			pop[k.unemployed_by_level] = slot.unemployed_by_level;
			pop[k.unemployment_by_level] = slot.unemployment_by_level;
			pop[k.vacancies_by_level] = slot.vacancies_by_level;
			pop[k.wage_by_level] = slot.wage_by_level;
			pop[k.wage_income_by_level] = slot.wage_income_by_level;
			pop[k.studying] = slot.studying;
		}
		if (!filled) {
			slot.desired_by_category.resize(ws_.food_products.size());
			slot.consumed_by_category.resize(ws_.food_products.size());
		}
		for (int food = 0; food < static_cast<int>(ws_.food_products.size()); ++food) {
			slot.desired_by_category[food] = city.pop.desired_by_category[food];
			slot.consumed_by_category[food] = city.pop.consumed_by_category[food];
		}
		if (!filled) {
			pop[k.desired_by_category] = slot.desired_by_category;
			pop[k.consumed_by_category] = slot.consumed_by_category;
		} else {
			bool changed = false;
			for (int food = 0; food < static_cast<int>(ws_.food_products.size()); ++food) {
				if (slot.pop_food_written[food] != city.pop.desired_by_category[food]
						|| slot.pop_food_written[ws_.food_products.size() + food]
							!= city.pop.consumed_by_category[food]) {
					changed = true;
					break;
				}
			}
			if (changed) {
				pop[k.desired_by_category] = slot.desired_by_category;
				pop[k.consumed_by_category] = slot.consumed_by_category;
			}
		}
		for (int food = 0; food < static_cast<int>(ws_.food_products.size()); ++food) {
			slot.pop_food_written[food] = city.pop.desired_by_category[food];
			slot.pop_food_written[ws_.food_products.size() + food] =
				city.pop.consumed_by_category[food];
		}
		filled = true;
	}
	if (!slot.pop_published) {
		dict[k.pop] = pop;
		slot.pop_published = true;
	}

	// B7: informalidade, mortalidade e insatisfaÃ§Ã£o (blocos pesados, sob
	// demanda em get_city_status). ConstruÃ­dos a cada chamada â€” o caminho Ã©
	// raro (uma cidade por consulta), entÃ£o nÃ£o hÃ¡ cache/mirror.
	{
		Dictionary informal;
		informal["share"] = city.pop.informal_share;
		Array by_quintile;
		by_quintile.resize(kIncomeStrata);
		for (int s = 0; s < kIncomeStrata; ++s) {
			by_quintile[s] = city.pop.informal_labor_share[s];
		}
		informal["by_quintile"] = by_quintile;
		informal["wage_bill"] = city.pop.informal_wage_bill;
		Dictionary service;
		service["workers"] = city.pop.informal_service_workers;
		service["output"] = city.pop.informal_service_output;
		service["demand"] = city.pop.informal_service_demand;
		service["revenue"] = city.pop.informal_service_revenue;
		service["price"] = city.pop.informal_service_price;
		service["share"] = city.pop.informal_service_share;
		informal["service"] = service;
		dict["informal"] = informal;

		Dictionary mortality;
		mortality["deaths"] = city.pop.deaths_total;
		mortality["expected_deaths"] = city.pop.deaths_expected;
		mortality["excess_total"] = city.pop.excess_deaths_total;
		mortality["excess_famine_total"] = city.pop.excess_deaths_famine_total;
		mortality["famine_months"] = city.pop.famine_months;
		mortality["health_access"] = sim_core_.nation_health_access(city);
		mortality["health_access_ref"] = city.pop.health_access_ref;
		dict["mortality"] = mortality;

		Dictionary dissatisfaction;
		dissatisfaction["index"] = city.pop.dissatisfaction;
		Array dq;
		dq.resize(kIncomeStrata);
		for (int s = 0; s < kIncomeStrata; ++s) {
			dq[s] = city.pop.dissatisfaction_by_stratum[s];
		}
		dissatisfaction["by_quintile"] = dq;
		Dictionary components;
		components["unemployment"] = city.pop.dissatisfaction_unemployment;
		components["rent"] = city.pop.dissatisfaction_rent;
		components["food"] = city.pop.dissatisfaction_food;
		components["inflation"] = city.pop.dissatisfaction_inflation;
		components["informality"] = city.pop.dissatisfaction_informality;
		components["land"] = 0.0;
		dissatisfaction["components"] = components;
		dict["dissatisfaction"] = dissatisfaction;
	}

	// B1: coortes etÃ¡rias (5 faixas). Publicadas como Array de doubles no bloco
	// `pop`, reescritas sÃ³ quando a composiÃ§Ã£o muda (uma vez por mÃªs).
	{
		double bands[kAgeBands];
		for (int band = 0; band < kAgeBands; ++band) {
			bands[band] = city.pop.demography_ready
				? std::max(0.0, city.pop.age_band[band]) : 0.0;
		}
		if (!slot.age_bands_filled
				|| std::memcmp(bands, slot.age_bands_written,
					sizeof(double) * static_cast<std::size_t>(kAgeBands)) != 0) {
			slot.age_bands.assign(bands, bands + kAgeBands);
			Array published;
			published.resize(kAgeBands);
			for (int band = 0; band < kAgeBands; ++band) {
				published[band] = slot.age_bands[static_cast<std::size_t>(band)];
			}
			pop[k.age_bands] = published;
			std::memcpy(slot.age_bands_written, bands,
				sizeof(double) * static_cast<std::size_t>(kAgeBands));
			slot.age_bands_filled = true;
		}
	}

	// B2: cinco estratos de renda no bloco `pop`. Publica populaÃ§Ã£o, renda
	// disponÃ­vel, poupanÃ§a, dÃ­vida, patrimÃ´nio, taxa de poupanÃ§a, posse (3),
	// nÃ­vel de consumo, aluguel pago e a fatia da renda gasta em comida â€” os
	// nÃºmeros que a validaÃ§Ã£o de padrÃ£o real lÃª. Republicado quando muda.
	{
		double flat[kIncomeStrata * 14 + 7];
		int fi = 0;
		const auto put = [&flat, &fi](double value) {
			flat[fi++] = value;
		};
		for (int s = 0; s < kIncomeStrata; ++s) put(city.pop.strata_income_share[s]);
		for (int s = 0; s < kIncomeStrata; ++s) put(city.pop.stratum_people[s]);
		for (int s = 0; s < kIncomeStrata; ++s) put(city.pop.stratum_net_income[s]);
		for (int s = 0; s < kIncomeStrata; ++s) put(city.pop.stratum_savings[s]);
		for (int s = 0; s < kIncomeStrata; ++s) put(city.pop.stratum_debt[s]);
		for (int s = 0; s < kIncomeStrata; ++s) put(city.pop.stratum_wealth[s]);
		for (int s = 0; s < kIncomeStrata; ++s) put(city.pop.stratum_saving_rate[s]);
		for (int s = 0; s < kIncomeStrata; ++s) put(double(city.pop.stratum_level[s]));
		for (int s = 0; s < kIncomeStrata; ++s) put(city.pop.stratum_rent_paid[s]);
		for (int s = 0; s < kIncomeStrata; ++s) put(city.pop.stratum_rent_bill[s]);
		for (int s = 0; s < kIncomeStrata; ++s) {
			put(city.pop.stratum_net_income[s] > 1e-9
				? city.pop.stratum_food_spend[s] / city.pop.stratum_net_income[s]
				: 0.0);
		}
		for (int s = 0; s < kIncomeStrata; ++s) {
			put(city.pop.stratum_tenure[s][0]);
		}
		for (int s = 0; s < kIncomeStrata; ++s) {
			put(city.pop.stratum_tenure[s][1]);
		}
		for (int s = 0; s < kIncomeStrata; ++s) {
			put(city.pop.stratum_tenure[s][2]);
		}
		put(sim_core_.strata_gini(city.pop));
		// B6: Gini de RIQUEZA dos quintis (poupanÃ§a + imÃ³vel + terra +
		// participaÃ§Ãµes; no urbano a terra Ã© 0 â€” ela Ã© da regiÃ£o).
		put(ogs::SimCore::quintile_gini(city.pop.stratum_wealth, kIncomeStrata));
		for (int band = 0; band < 5; ++band) {
			put(city.pop.lifecycle_saving[band]);
		}
		if (!slot.income_strata_filled
				|| std::memcmp(flat, slot.income_strata_written.data(),
					sizeof(double) * static_cast<std::size_t>(fi)) != 0) {
			slot.income_strata_written.assign(flat, flat + fi);
			Dictionary out;
			const char* const names[14] = {"income_share", "population", "income", "savings",
				"debt", "wealth", "saving_rate", "level", "rent_paid", "rent_bill",
				"food_income_share", "tenure_own", "tenure_rent",
				"tenure_informal"};
			for (int field = 0; field < 14; ++field) {
				Array row;
				row.resize(kIncomeStrata);
				for (int s = 0; s < kIncomeStrata; ++s) {
					row[s] = flat[field * kIncomeStrata + s];
				}
				out[String(names[field])] = row;
			}
			out["gini"] = flat[kIncomeStrata * 14];
			out["wealth_gini"] = flat[kIncomeStrata * 14 + 1];
			Array lifecycle;
			lifecycle.resize(5);
			for (int band = 0; band < 5; ++band) {
				lifecycle[band] = flat[kIncomeStrata * 14 + 2 + band];
			}
			out["lifecycle_saving"] = lifecycle;
			slot.income_strata = out;
			pop[k.income_strata] = slot.income_strata;
			slot.income_strata_filled = true;
		}
	}

	// `resize` com o mesmo tamanho jÃ¡ provoca copy-on-write no Array
	// compartilhado com o dicionÃ¡rio publicado (medido: ~4,5 ms/dia, 114
	// Variants copiados por cidade). SÃ³ redimensiona quando muda de fato.
	if (slot.markets.size() != ws_.products) {
		slot.markets.resize(ws_.products);
	}
	if (slot.market_entry.size() < ws_.products) {
		slot.market_entry.resize(static_cast<std::size_t>(ws_.products));
		slot.market_written.resize(static_cast<std::size_t>(ws_.products));
		slot.market_written_filled.assign(static_cast<std::size_t>(ws_.products), 0);
		slot.market_local_written.assign(static_cast<std::size_t>(ws_.products), 0.0);
		slot.io_written.assign(static_cast<std::size_t>(4 * ws_.products), 0.0);
		slot.io_filled.assign(4, 0);
	}
	// PreÃ§o local Ã© EXIBIÃ‡ÃƒO: Ã¢ncora Ã· taxa da moeda da naÃ§Ã£o (F5, opÃ§Ã£o c).
	// NÃ£o volta para a simulaÃ§Ã£o; o valor na Ã¢ncora Ã© a fonte de verdade.
	const double local_rate = std::max(1e-9, city_fx);
	bool markets_dirty = false;
	for (int product = 0; product < ws_.products; ++product) {
		const Market& source = city.markets[product];
		Market& previous = slot.market_written[static_cast<std::size_t>(product)];
		const double local = source.price / local_rate;
		if (slot.market_written_filled[static_cast<std::size_t>(product)] == 0) {
			// Primeira gravaÃ§Ã£o desta cidade: cria as chaves do mercado.
			Dictionary& market = slot.market_entry[static_cast<std::size_t>(product)];
			market[k.product] = product;
			market[k.stock] = source.stock;
			market[k.price] = source.price;
			market[k.price_local] = local;
			market[k.demand] = source.demand;
			market[k.supply] = source.supply;
			market[k.desired] = source.desired;
			market[k.sold] = source.sold;
			market[k.shortage] = source.shortage;
			market[k.surplus] = source.surplus;
			previous = source;
			slot.market_local_written[static_cast<std::size_t>(product)] = local;
			slot.market_written_filled[static_cast<std::size_t>(product)] = 1;
			slot.markets[product] = market;
			markets_dirty = true;
			continue;
		}
		// Caminho comum (mercado publicado igual ao dia anterior): NENHUM
		// acesso ao Dictionary. A comparaÃ§Ã£o cobre os 8 campos publicados, que
		// sÃ£o contÃ­guos (ver Market): um memcmp de 64 bytes por produto.
		// `sold_prev` Ã© interno e muda TODO dia (`sold_prev = sold` no reset);
		// por isso fica no fim do struct, fora do bloco comparado.
		const bool local_changed = slot.market_local_written[
			static_cast<std::size_t>(product)] != local;
		if (!local_changed && std::memcmp(&previous, &source, sizeof(double) * 8) == 0) {
			continue;
		}
		markets_dirty = true;
		Dictionary& market = slot.market_entry[static_cast<std::size_t>(product)];
		if (local_changed) {
			market[k.price_local] = local;
			slot.market_local_written[static_cast<std::size_t>(product)] = local;
		}
		if (previous.stock != source.stock) {
			market[k.stock] = source.stock;
			previous.stock = source.stock;
		}
		if (previous.price != source.price) {
			market[k.price] = source.price;
			previous.price = source.price;
		}
		if (previous.demand != source.demand) {
			market[k.demand] = source.demand;
			previous.demand = source.demand;
		}
		if (previous.supply != source.supply) {
			market[k.supply] = source.supply;
			previous.supply = source.supply;
		}
		if (previous.desired != source.desired) {
			market[k.desired] = source.desired;
			previous.desired = source.desired;
		}
		if (previous.sold != source.sold) {
			market[k.sold] = source.sold;
			previous.sold = source.sold;
		}
		if (previous.shortage != source.shortage) {
			market[k.shortage] = source.shortage;
			previous.shortage = source.shortage;
		}
		if (previous.surplus != source.surplus) {
			market[k.surplus] = source.surplus;
			previous.surplus = source.surplus;
		}
	}
	// Sem nenhuma mudanÃ§a o Array publicado continua o mesmo objeto: reatribuir
	// sÃ³ custaria uma chamada Ã  ponte GDExtension por cidade (~0,3 ms/dia).
	if (markets_dirty) {
		dict[k.markets] = slot.markets;
	}

	// F8: bloco plano de serviÃ§os (20 Ã— 12). SÃ³ reescreve quando algum valor
	// muda; o Array publicado Ã© reutilizado pela UI (ordem de service_sectors).
	if (ws_.services_loaded) {
		PackedFloat64Array& services = slot.services;
		services.resize(kMaxServiceSectors * kServiceFields);
		double* out = services.ptrw();
		for (int sector = 0; sector < kMaxServiceSectors; ++sector) {
			const ServiceMarket& market = city.service_markets[sector];
			double* base = out + sector * kServiceFields;
			base[0] = market.demand;
			base[1] = market.supply;
			base[2] = market.price;
			base[3] = market.revenue;
			base[4] = market.jobs;
			base[5] = market.businesses;
			base[6] = market.unmet;
			base[7] = market.exports;
			base[8] = market.export_revenue;
			base[9] = market.coverage;
			base[10] = market.input_cost;
			base[11] = market.service_cost;
		}
		bool dirty = !slot.services_filled;
		if (!dirty) {
			const double* previous = slot.services_written.ptr();
			for (int index = 0; index < services.size(); ++index) {
				if (previous[index] != out[index]) {
					dirty = true;
					break;
				}
			}
		}
		if (dirty) {
			slot.services_written = services;
			dict[k.services] = services;
			slot.services_filled = true;
		}
	}
	}  // fim do bloco pesado (deposits/pop/markets/services)

	// `Array` Ã© tipo por referÃªncia no Godot 4 (validado em teste): mutar
	// `slot.imported` jÃ¡ aparece no dicionÃ¡rio publicado. Redimensionar para o
	// mesmo tamanho era uma chamada Ã  ponte por array/cidade/dia sem efeito.
	if (slot.io_written.size() < static_cast<std::size_t>(4 * ws_.products)) {
		slot.io_written.assign(static_cast<std::size_t>(4 * ws_.products), 0.0);
		slot.io_filled.assign(4, 0);
	}
	if (slot.imported.size() != ws_.products) {
		slot.imported.resize(ws_.products);
		slot.exported.resize(ws_.products);
		slot.import_value.resize(ws_.products);
		slot.export_value.resize(ws_.products);
	}
	{
		// Quatro arrays de fluxo, um por produto (ws_.products): idem
		// mercados, sÃ³ produtos com comÃ©rcio mudam (o resto fica em 0 dia apÃ³s
		// dia) e a linha inteira Ã© comparada com um memcmp antes do hash.
		const double* source_values[4] = {
			city.imported, city.exported, city.import_value, city.export_value};
		Array* targets[4] = {
			&slot.imported, &slot.exported, &slot.import_value, &slot.export_value};
		const StringName* keys[4] = {&k.imported, &k.exported, &k.import_value, &k.export_value};
		for (int array_index = 0; array_index < 4; ++array_index) {
			Array& target = *targets[array_index];
			double* previous = &slot.io_written[static_cast<std::size_t>(
				array_index * ws_.products)];
			const bool first = slot.io_filled[static_cast<std::size_t>(array_index)] == 0;
			// Linha inteira igual ao status anterior: nenhum hash a pagar.
			if (!first && std::memcmp(previous, source_values[array_index],
					sizeof(double) * static_cast<std::size_t>(ws_.products)) == 0) {
				continue;
			}
			for (int product = 0; product < ws_.products; ++product) {
				const double value = source_values[array_index][product];
				if (first || previous[product] != value) {
					target[product] = value;
					previous[product] = value;
				}
			}
			if (first) {
				dict[*keys[array_index]] = target;
				slot.io_filled[static_cast<std::size_t>(array_index)] = 1;
			}
		}
	}

	// S7-c: construÃ§Ãµes tambÃ©m sÃ£o detalhe pesado sob demanda.
	if (heavy) {
	const std::vector<int>& local_buildings = ws_.buildings_by_city[city_index];
	if (slot.buildings.size() != local_buildings.size()) {
		slot.buildings.resize(local_buildings.size());
		slot.buildings_bound = false;
	}
	if (slot.building_entry.size() < local_buildings.size()) {
		slot.building_entry.resize(local_buildings.size());
		slot.building_cache.resize(local_buildings.size());
	}
	for (std::size_t index = 0; index < local_buildings.size(); ++index) {
		const Building& building =
			ws_.buildings[static_cast<std::size_t>(local_buildings[index])];
		Dictionary& entry = slot.building_entry[index];
		StatusCitySlot::BuildingCache& cached = slot.building_cache[index];
		const bool identity_changed = !cached.filled || cached.id_src != building.id;
		// F9: privatizaÃ§Ã£o/nacionalizaÃ§Ã£o muda o dono sem mudar a identidade.
		const bool owner_changed = cached.owner_src != building.owner;
		// Bloco estÃ¡tico: sÃ³ quando a construÃ§Ã£o nasce (ou muda de identidade).
		if (identity_changed || owner_changed) {
			cached.id_src = building.id;
			cached.id = String(building.id.c_str());
			entry[k.id] = cached.id;
			entry[k.kind] = building.kind;
			cached.resource_src = building.resource;
			cached.resource = String(building.resource.c_str());
			entry[k.resource] = cached.resource;
			cached.owner_src = building.owner;
			cached.owner = String(building.owner.c_str());
			entry[k.owner] = cached.owner;
			// F9: decomposiÃ§Ã£o do dono para a UI/status (sem parse no cliente).
			std::string group_id;
			std::string origin_nation;
			const int owner_code = owner_kind(building.owner, &group_id,
				&origin_nation);
			entry[k.owner_kind] = owner_code == 0 ? String("state")
				: owner_code == 1 ? String("national")
				: owner_code == 2 ? String("foreign") : String("private");
			entry[k.owner_group] = String(group_id.c_str());
			entry[k.owner_nation] = origin_nation.empty()
				? String() : String(origin_nation.c_str());
			entry[k.local_share] = building.local_share;
			entry[k.input_product] = building.input_product;
			entry[k.output_product] = building.output_product;
			entry[k.capital_per_capacity] = building.capital_per_capacity;
			entry[k.strategic] = building.strategic;
			cached.recipe = -2;
			cached.filled = true;
		}
		// Reinsere o `entry` no Array publicado sÃ³ quando ele nasce/muda de
		// Ã­ndice; mutaÃ§Ãµes do Dictionary jÃ¡ sÃ£o visÃ­veis pela referÃªncia.
		if (!slot.buildings_bound || identity_changed) {
			slot.buildings[index] = entry;
		}
		// Receita (insumos/subprodutos): as Arrays/Dictionaries sÃ³ sÃ£o
		// reconstruÃ­dos quando a receita da construÃ§Ã£o muda â€” nunca por dia.
		if (cached.recipe != building.recipe) {
			cached.recipe = building.recipe;
			if (building.recipe >= 0
					&& building.recipe < static_cast<int>(ws_.recipes.size())) {
				const Recipe& recipe =
					ws_.recipes[static_cast<std::size_t>(building.recipe)];
				entry[k.recipe] = String(recipe.id.c_str());
				Array inputs;
				Array byproducts;
				inputs.resize(recipe.input_count);
				byproducts.resize(recipe.byproduct_count);
				for (int slot = 0; slot < recipe.input_count; ++slot) {
					Dictionary input;
					input[k.good] = String(product_name(recipe.inputs[slot]));
					input[k.qty] = recipe.input_qty[slot];
					inputs[slot] = input;
				}
				for (int slot = 0; slot < recipe.byproduct_count; ++slot) {
					Dictionary byproduct;
					byproduct[k.good] = String(product_name(recipe.byproducts[slot]));
					byproduct[k.qty] = recipe.byproduct_qty[slot];
					byproducts[slot] = byproduct;
				}
				entry[k.inputs] = inputs;
				entry[k.byproducts] = byproducts;
				entry[k.yield] = recipe.yield;
				// G1: classe da planta (metadado; "" = sem classe).
				entry[k.plant_class] = String(recipe.plant_class.c_str());
			} else {
				entry[k.recipe] = String();
				entry[k.inputs] = Array();
				entry[k.byproducts] = Array();
				entry[k.plant_class] = String();
			}
		}
		// Valores diÃ¡rios: sÃ³ reescreve o que mudou (comparar double Ã© ~20Ã—
		// mais barato que um Dictionary::operator[] com hash de StringName).
		auto sync_double = [&entry, &cached](StringName key, double& mirror,
				double value) {
			if (mirror != value) {
				entry[key] = value;
				mirror = value;
			}
		};
		sync_double(k.last_utilization, cached.last_utilization, building.last_utilization);
		// D-125: eficiência de produção, dias, transição, equipamento e modo.
		sync_double(k.production_efficiency, cached.production_efficiency,
			building.production_efficiency);
		sync_double(k.equipment, cached.equipment, building.equipment);
		if (cached.efficiency_days != building.efficiency_days) {
			entry[k.efficiency_days] = building.efficiency_days;
			cached.efficiency_days = building.efficiency_days;
		}
		if (cached.transition_days != building.transition_days) {
			entry[k.transition_days] = building.transition_days;
			cached.transition_days = building.transition_days;
		}
		if (cached.refinery_mode != building.refinery_mode) {
			entry[k.refinery_mode] = building.refinery_mode;
			cached.refinery_mode = building.refinery_mode;
		}
		sync_double(k.capacity, cached.capacity, building.capacity);
		sync_double(k.jobs, cached.jobs, building.jobs);
		sync_double(k.wage, cached.wage, building.wage);
		sync_double(k.opex, cached.opex, building.opex);
		sync_double(k.workers, cached.workers, building.workers);
		sync_double(k.utilization, cached.utilization, building.utilization);
		sync_double(k.output_scale, cached.output_scale, building.output_scale);
		if (cached.loss_days != static_cast<double>(building.loss_days)) {
			entry[k.loss_days] = building.loss_days;
			cached.loss_days = static_cast<double>(building.loss_days);
		}
		sync_double(k.revenue, cached.revenue, building.revenue);
		sync_double(k.input_value, cached.input_value, building.input_value);
		sync_double(k.costs, cached.costs, building.costs);
		sync_double(k.profit, cached.profit, building.profit);
		sync_double(k.site_suitability, cached.site,
			city.suitability[building.output_product]);
		// D-086 â€” estÃ¡gio de mina: profundidade de operaÃ§Ã£o, produÃ§Ã£o relativa do
		// estÃ¡gio (0,05..1,0) e custo do PRÃ“XIMO aprofundamento. Fazenda/indÃºstria
		// publicam estÃ¡gio 0 / fator 1,0 (nÃ£o tÃªm estÃ¡gio) â€” ausÃªncia explÃ­cita.
		{
			const bool mine = SimCore::is_mine_building(city, building);
			const int stage = mine ? building.stage : 0;
			if (cached.stage != stage) {
				entry[k.stage] = stage;
				cached.stage = stage;
			}
			sync_double(k.stage_factor, cached.stage_factor,
				SimCore::mine_stage_factor(city, building));
			sync_double(k.stage_reserve_kt, cached.stage_reserve_kt,
				mine ? building.stage_reserve_kt : 0.0);
			sync_double(k.stage_mined_kt, cached.stage_mined_kt,
				mine ? building.stage_mined_kt : 0.0);
			// D-086 (decisÃ£o do dono): `can_deepen` sÃ³ exige a mina nÃ£o falida â€” a
			// reserva NUNCA bloqueia o aprofundamento (a jazida Ã© referÃªncia de
			// profundidade, nÃ£o um limite fÃ­sico).
			const bool can_deepen = mine && !building.bankrupt;
			sync_double(k.next_deepen_cost, cached.next_deepen_cost,
				can_deepen ? sim_core_.mine_deepen_cost(city, building) : 0.0);
		}
	}
	if (!slot.buildings_published) {
		dict[k.buildings] = slot.buildings;
		slot.buildings_published = true;
	}
	slot.buildings_bound = true;
	}  // fim do bloco pesado (buildings)
}

} // namespace godot
