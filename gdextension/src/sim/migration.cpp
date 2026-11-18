// migration.cpp — B4: migração mensal (êxodo rural → polo urbano e cidade →
// cidade na mesma nação). Conserva pessoas e dinheiro. Cadência A6 (mesma
// agenda da B3): cada cidade roda no seu dia (`cadence_due(day, city_index,
// monthly_period)`), num laço SERIAL determinístico.
//
// Modelo (docs/economy/B4_migration.md):
//   - Êxodo rural: migra quando a renda urbana esperada (salário × chance de
//     emprego − custo de moradia) supera a renda rural por pessoa pela margem
//     `kMigrationGapThreshold`. O fluxo é `kMigrationMonthlyMax` por mês,
//     saturado pelo diferencial e limitado pela VAGA de moradia no destino
//     (unidades vazias do mercado de moradia, W3).
//   - Cidade → cidade (mesma nação): a fonte empurra para as cidades de maior
//     utilidade (renda líquida esperada + serviços), mesma taxa e mesmo limite
//     de vaga. Internacional fica FORA desta fase (registrado no doc).
//   - Estrutura: migrantes mais jovens (kMigrationAgeWeight) e, no êxodo, dos
//     quintis mais baixos (kMigrationStrataWeight). A poupança per capita do
//     estrato de origem acompanha o migrante — dinheiro conservado.
//
// C++ puro (STL via sim_core.h), sem godot_cpp.

#include "sim_core.h"

#include "housing.h"
#include "sim_constants.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace ogs {

namespace {

constexpr double kMigEps = 1e-12;

// Piscina de pessoas: coortes (age_band) + quintis (stratum_people) + poupança
// por quintil (stratum_savings) + o escalar de dinheiro (region.cash ou
// pop.savings — em ambos, o invariante é escalar = Σ poupança dos quintis).
// Region e Pop têm a mesma forma; os ponteiros evitam duplicar a mecânica.
struct Pool {
	double* age_band;         // [kAgeBands]
	double* stratum_people;   // [kIncomeStrata]
	double* stratum_savings;  // [kIncomeStrata]
	double* cash;             // region.cash | pop.savings
};

double pool_population(const Pool& p) {
	double total = 0.0;
	for (int b = 0; b < kAgeBands; ++b) {
		total += std::max(0.0, p.age_band[b]);
	}
	if (total > kMigEps) {
		return total;
	}
	for (int s = 0; s < kIncomeStrata; ++s) {
		total += std::max(0.0, p.stratum_people[s]);
	}
	return total;
}

// Retira `migrants` pessoas da piscina repartidas por peso etário e por peso
// de estrato, carregando a poupança per capita do estrato de origem. Preenche
// age_out/strata_out/savings_out e devolve o número efetivo (== migrants).
double pool_take(Pool& p, double migrants, const double* age_weight,
		const double* strata_weight, double* age_out, double* strata_out,
		double* savings_out) {
	for (int b = 0; b < kAgeBands; ++b) {
		age_out[b] = 0.0;
	}
	for (int s = 0; s < kIncomeStrata; ++s) {
		strata_out[s] = 0.0;
		savings_out[s] = 0.0;
	}
	const double total = pool_population(p);
	if (total <= kMigEps || migrants <= kMigEps) {
		return 0.0;
	}
	migrants = std::min(migrants, total);
	// Composição etária (peso relativo por faixa); a última faixa absorve o
	// resíduo para a soma sair EXATA (conservação bit a bit).
	double age_weight_total = 0.0;
	for (int b = 0; b < kAgeBands; ++b) {
		age_weight_total += std::max(0.0, p.age_band[b]) * age_weight[b];
	}
	double age_acc = 0.0;
	for (int b = 0; b + 1 < kAgeBands; ++b) {
		age_out[b] = age_weight_total > kMigEps
			? migrants * std::max(0.0, p.age_band[b]) * age_weight[b]
				/ age_weight_total
			: migrants / double(kAgeBands);
		age_acc += age_out[b];
	}
	age_out[kAgeBands - 1] = migrants - age_acc;
	// Composição de estrato (peso relativo por quintil).
	double strata_weight_total = 0.0;
	for (int s = 0; s < kIncomeStrata; ++s) {
		strata_weight_total += std::max(0.0, p.stratum_people[s])
			* strata_weight[s];
	}
	double strata_acc = 0.0;
	for (int s = 0; s + 1 < kIncomeStrata; ++s) {
		strata_out[s] = strata_weight_total > kMigEps
			? migrants * std::max(0.0, p.stratum_people[s]) * strata_weight[s]
				/ strata_weight_total
			: migrants / double(kIncomeStrata);
		strata_acc += strata_out[s];
	}
	strata_out[kIncomeStrata - 1] = migrants - strata_acc;
	// Poupança que acompanha: per capita do estrato de origem × pessoas do
	// estrato (clamp defensivo; nunca excede a poupança do estrato).
	double total_savings = 0.0;
	for (int s = 0; s < kIncomeStrata; ++s) {
		const double per_person = p.stratum_people[s] > kMigEps
			? std::max(0.0, p.stratum_savings[s]) / p.stratum_people[s]
			: 0.0;
		savings_out[s] = std::min(strata_out[s] * per_person,
			std::max(0.0, p.stratum_savings[s]));
		total_savings += savings_out[s];
	}
	// Debita.
	for (int b = 0; b < kAgeBands; ++b) {
		p.age_band[b] = std::max(0.0, p.age_band[b] - age_out[b]);
	}
	for (int s = 0; s < kIncomeStrata; ++s) {
		p.stratum_people[s] = std::max(0.0, p.stratum_people[s] - strata_out[s]);
		p.stratum_savings[s] = std::max(0.0,
			p.stratum_savings[s] - savings_out[s]);
	}
	*p.cash = std::max(0.0, *p.cash - total_savings);
	return migrants;
}

// Credita migrantes na piscina destino (mesma composição/poupança que saíram).
void pool_add(Pool& p, const double* age_in, const double* strata_in,
		const double* savings_in) {
	for (int b = 0; b < kAgeBands; ++b) {
		p.age_band[b] += std::max(0.0, age_in[b]);
	}
	double total_savings = 0.0;
	for (int s = 0; s < kIncomeStrata; ++s) {
		p.stratum_people[s] += std::max(0.0, strata_in[s]);
		p.stratum_savings[s] += std::max(0.0, savings_in[s]);
		total_savings += std::max(0.0, savings_in[s]);
	}
	*p.cash += total_savings;
}

// Vaga de moradia no destino (pessoas): unidades vazias × pessoas por
// domicílio. 0 = sem vaga (destino cheio) — a migração não entra além do que o
// mercado de moradia comporta.
double urban_vacancy_people(const WorldState& ws, std::size_t ci) {
	if (ci >= ws.housing.size()) {
		return 0.0;
	}
	const CityHousing& house = ws.housing[ci];
	double units = 0.0;
	for (int t = 0; t < TIER_COUNT; ++t) {
		units += std::max(0.0, house.vacancy[t]);
	}
	if (units <= kMigEps) {
		return 0.0;
	}
	const double households = house.households > 0.0 ? house.households : 0.0;
	if (households <= kMigEps) {
		return 0.0;
	}
	const double pop = house.pop_anchor > 0.0
		? house.pop_anchor
		: std::max(1.0, ws.cities[ci].simulation_population);
	return units * (pop / households);
}

// Utilidade urbana de uma cidade (por trabalhador/dia): salário × chance de
// emprego − aluguel + serviços. Lê os sinais do fechamento anterior (estado
// persistido — moradia/salários do dia já rodado), como a mobilidade B2.
double city_utility(const WorldState& ws, std::size_t ci) {
	const City& city = ws.cities[ci];
	const double labor_force = std::max(1.0, city.pop.labor_force);
	// Salário urbano × chance de emprego = (folha ÷ empregados) × (empregados ÷
	// força de trabalho) = folha ÷ força. A folha só paga os empregados, então
	// o quociente JÁ embute a chance de emprego (evita o duplo desconto).
	const double expected = std::max(0.0, city.pop.wage_income) / labor_force;
	const double rent = std::max(0.0, housing::daily_rent_obligation(ws, ci))
		/ labor_force;
	const double services = std::max(0.0, city.service_revenue)
		/ std::max(1.0, city.simulation_population);
	return expected - rent + kMigrationServiceWeight * services;
}

// Êxodo rural: interior → polo urbano da MESMA cidade. Devolve o nº de pessoas
// movidas (0 = sem fluxo: sem dado, sem diferencial ou sem vaga no destino).
double migrate_region_to_urban(WorldState& ws, SimCore& core, std::size_t ci) {
	City& city = ws.cities[ci];
	Region& region = city.region;
	Pop& pop = city.pop;
	if (!region.ready || !region.demography_ready || !region.strata_ready
			|| !pop.demography_ready || !pop.strata_ready) {
		return 0.0;
	}
	const double region_pop = std::max(0.0, region.region_pop);
	if (region_pop <= kMigEps) {
		return 0.0;
	}
	const double rural_income = std::max(0.0, region.income) / region_pop;
	const double urban_net = city_utility(ws, ci);
	const double gap = rural_income > kMigEps
		? (urban_net - rural_income) / rural_income : 0.0;
	if (gap <= kMigrationGapThreshold) {
		return 0.0;
	}
	const double drive = std::clamp(
		(gap - kMigrationGapThreshold) / kMigrationGapScale, 0.0, 1.0);
	double migrants = region_pop * kMigrationMonthlyMax * drive;
	migrants = std::min(migrants, urban_vacancy_people(ws, ci));
	if (migrants <= kMigEps) {
		return 0.0;
	}

	Pool src{region.age_band, region.stratum_people, region.stratum_savings,
		&region.cash};
	Pool dst{pop.age_band, pop.stratum_people, pop.stratum_savings,
		&pop.savings};
	double age_out[kAgeBands];
	double strata_out[kIncomeStrata];
	double savings_out[kIncomeStrata];
	const double moved = pool_take(src, migrants, kMigrationAgeWeight,
		kMigrationStrataWeight, age_out, strata_out, savings_out);
	pool_add(dst, age_out, strata_out, savings_out);

	region.migration_rural_in_total += moved;
	region.migration_month_rural_in = moved;
	// O interior perdeu gente e o urbano ganhou: reaplica os totais derivados.
	core.apply_demography(city);
	core.apply_region_demography(city);
	return moved;
}

// Cidade → cidade (mesma nação): a cidade fonte `ci` empurra para cidades de
// maior utilidade. Fonte-cêntrico (determinístico): cada cidade, no seu dia da
// cadência, distribui a própria saída. Devolve o total que saiu.
double migrate_city_out(WorldState& ws, SimCore& core, std::size_t ci,
		const std::vector<double>& utility) {
	City& src = ws.cities[ci];
	Pop& spop = src.pop;
	if (!spop.demography_ready || !spop.strata_ready) {
		return 0.0;
	}
	const double src_pop = std::max(0.0, spop.population);
	if (src_pop <= kMigEps) {
		return 0.0;
	}
	const double u_i = utility[ci];
	// Soma das utilidades acima da minha (e o máximo), na minha nação.
	double nation_max = u_i;
	double weight_sum = 0.0;
	for (std::size_t j = 0; j < ws.cities.size(); ++j) {
		if (j == ci || ws.cities[j].nation != src.nation) {
			continue;
		}
		const double u_j = utility[j];
		if (u_j <= u_i) {
			continue;
		}
		nation_max = std::max(nation_max, u_j);
		weight_sum += (u_j - u_i);
	}
	if (weight_sum <= kMigEps) {
		return 0.0;
	}
	const double gap = std::max(0.0, nation_max - u_i)
		/ std::max(std::abs(u_i), kMigEps);
	if (gap <= kMigrationGapThreshold) {
		return 0.0;
	}
	const double drive = std::clamp(
		(gap - kMigrationGapThreshold) / kMigrationGapScale, 0.0, 1.0);
	const double out_total = src_pop * kMigrationMonthlyMax * drive;
	if (out_total <= kMigEps) {
		return 0.0;
	}

	// Perfil de estrato da cidade → cidade: proporcional à população (o
	// diferencial atrai todas as faixas; só o êxodo rural enviesa o estrato).
	static constexpr double kUniformStrata[kIncomeStrata] = {1.0, 1.0, 1.0, 1.0, 1.0};
	Pool sp{spop.age_band, spop.stratum_people, spop.stratum_savings,
		&spop.savings};
	double moved_total = 0.0;
	for (std::size_t j = 0; j < ws.cities.size(); ++j) {
		if (j == ci || ws.cities[j].nation != src.nation) {
			continue;
		}
		const double u_j = utility[j];
		if (u_j <= u_i) {
			continue;
		}
		City& dst = ws.cities[j];
		Pop& dpop = dst.pop;
		if (!dpop.demography_ready || !dpop.strata_ready) {
			continue;
		}
		double m = out_total * (u_j - u_i) / weight_sum;
		m = std::min(m, urban_vacancy_people(ws, j));
		if (m <= kMigEps) {
			continue;
		}
		double age_out[kAgeBands];
		double strata_out[kIncomeStrata];
		double savings_out[kIncomeStrata];
		Pool dp{dpop.age_band, dpop.stratum_people, dpop.stratum_savings,
			&dpop.savings};
		const double moved = pool_take(sp, m, kMigrationAgeWeight,
			kUniformStrata, age_out, strata_out, savings_out);
		pool_add(dp, age_out, strata_out, savings_out);
		src.region.migration_city_out_total += moved;
		dst.region.migration_city_in_total += moved;
		// Fluxo do mês do DESTINO: a entrada conta na janela do próprio destino
		// (cada cidade mede no seu dia A6; o reset acontece lá, numa passada
		// anterior à migração do dia, então nada é zerado por outra cidade).
		dst.region.migration_month_city_in += moved;
		moved_total += moved;
		core.apply_demography(dst);
		core.reconcile_city_population(dst);
	}
	if (moved_total > kMigEps) {
		src.region.migration_month_city_out = moved_total;
		core.apply_demography(src);
		core.reconcile_city_population(src);
	}
	return moved_total;
}

} // namespace

void SimCore::phase_migration() {
	// Laço SERIAL (determinístico). A utilidade de cada cidade é um retrato
	// congelado no topo do dia (como o plano de comércio A7): a migração de uma
	// cidade não realimenta a decisão de outra no MESMO dia.
	const int month_period = ws_.cadence.monthly_period;
	const std::size_t n = ws_.cities.size();
	std::vector<double> utility(n, 0.0);
	for (std::size_t i = 0; i < n; ++i) {
		utility[i] = city_utility(ws_, i);
	}
	// Passada 1 — RESET dos fluxos do mês para as cidades que fecham HOJE.
	// Roda ANTES de qualquer migração: a entrada cidade→cidade de outra cidade
	// no mesmo dia não pode ser zerada por um reset posterior.
	for (std::size_t i = 0; i < n; ++i) {
		if (!cadence_due(ws_.day, i, month_period)) {
			continue;
		}
		Region& region = ws_.cities[i].region;
		if (!region.ready) {
			continue;  // sem dado de região = inerte
		}
		region.migration_month_rural_in = 0.0;
		region.migration_month_city_in = 0.0;
		region.migration_month_city_out = 0.0;
		region.migration_month_net = 0.0;
		region.migration_month_rate = 0.0;
	}
	// Passada 2 — MIGRAÇÃO: êxodo rural (interior → polo urbano) e cidade →
	// cidade (mesma nação), na ordem fixa das cidades.
	for (std::size_t i = 0; i < n; ++i) {
		if (!cadence_due(ws_.day, i, month_period)) {
			continue;
		}
		City& city = ws_.cities[i];
		if (!city.region.ready) {
			continue;
		}
		migrate_region_to_urban(ws_, *this, i);
		migrate_city_out(ws_, *this, i, utility);
	}
	// Passada 3 — TAXA LÍQUIDA do mês (fração da população exibida), já com a
	// entrada cidade→cidade completa (todas as fontes rodaram na passada 2).
	for (std::size_t i = 0; i < n; ++i) {
		if (!cadence_due(ws_.day, i, month_period)) {
			continue;
		}
		City& city = ws_.cities[i];
		Region& region = city.region;
		if (!region.ready) {
			continue;
		}
		const double total_pop = std::max(1.0,
			city.simulation_population + std::max(0.0, region.region_pop));
		region.migration_month_net = region.migration_month_rural_in
			+ region.migration_month_city_in - region.migration_month_city_out;
		region.migration_month_rate = region.migration_month_net / total_pop;
	}
}

} // namespace ogs
