// Módulo de domínio: demografia por coortes (B1).
//
// A POP de cada cidade é cinco coortes (0-14, 15-24, 25-44, 45-64, 65+). O DADO
// vem da estrutura etária nacional (age_structure_2026.json): as frações do dia
// 1, a taxa bruta de natalidade (CBR) e a de mortalidade (CDR) por mil. A
// composição por faixa da mortalidade e os fluxos de envelhecimento são MODELO
// declarado (sim_constants.h, kAgeAgingAnnual/kAgeDeathWeight), não dado
// observado.
//
// A fase roda no calendário mensal (A6): cada cidade fecha o mês em um dia
// próprio, então o custo diário é ~1/30 das cidades. Laço SERIAL: a ordem das
// cidades é fixa e o resultado é determinístico.
//
// C++ puro (STL via sim_core.h), sem godot_cpp.
#include "sim_core.h"

#include "profiling.h"
#include "sim_constants.h"

#include <algorithm>
#include <cmath>

namespace ogs {

void SimCore::seed_demography(City& city) {
	Pop& pop = city.pop;
	if (pop.demography_ready) {
		return;
	}
	// Sem estrutura nacional NÃO se inventa coorte: a cidade fica no perfil
	// paramétrico legado (idade ativa/aposentados derivados do índice de renda).
	const auto found = ws_.age_structures.find(city.nation);
	if (found == ws_.age_structures.end() || !found->second.valid) {
		return;
	}
	const AgeStructure& structure = found->second;
	const double population = std::max(0.0, city.simulation_population);
	double total = 0.0;
	for (int band = 0; band < kAgeBands; ++band) {
		pop.age_band[band] = population * structure.band[band];
		total += pop.age_band[band];
	}
	// As frações do arquivo são arredondadas: o resíduo volta para a faixa
	// 25-44 (a maior), para a soma das coortes ser exatamente a população.
	pop.age_band[2] = std::max(0.0, pop.age_band[2] + (population - total));
	// Referências dos pesos: a composição do dia da semeadura. O fator de
	// necessidade é o peso de hoje sobre esta referência, então a média do
	// mundo não muda quando o dado chega — só a repartição entre cidades.
	double health = 0.0;
	double education = 0.0;
	for (int band = 0; band < kAgeBands; ++band) {
		const double value = std::max(0.0, pop.age_band[band]);
		health += kAgeHealthWeight[band] * value;
		education += kAgeEducationWeight[band] * value;
	}
	pop.health_weight_ref = std::max(health, 1e-9);
	pop.education_weight_ref = std::max(education, 1e-9);
	pop.demography_ready = true;
}

void SimCore::apply_demography(City& city) {
	seed_demography(city);
	Pop& pop = city.pop;
	if (!pop.demography_ready) {
		return;
	}
	double total = 0.0;
	for (int band = 0; band < kAgeBands; ++band) {
		pop.age_band[band] = std::max(0.0, pop.age_band[band]);
		total += pop.age_band[band];
	}
	// A população da cidade É a soma das coortes (população variável).
	city.simulation_population = total;
	pop.population = total;
	pop.children = pop.age_band[0];
	// Idade ativa = 15-24 + 25-44 + 45-64 (aposentadoria em 65).
	pop.working_age = pop.age_band[1] + pop.age_band[2] + pop.age_band[3];
	pop.retirees = pop.age_band[4];
	// Reserva recrutável (18-49): fração observada da nação sobre a população
	// de hoje. Sem o dado (não deveria acontecer com ready), 0.
	const auto found = ws_.age_structures.find(city.nation);
	const bool has_structure =
		found != ws_.age_structures.end() && found->second.valid;
	pop.recruit_reserve = has_structure
		? std::max(0.0, found->second.frac_18_49) * total : 0.0;
	double health = 0.0;
	double education = 0.0;
	for (int band = 0; band < kAgeBands; ++band) {
		const double value = std::max(0.0, pop.age_band[band]);
		health += kAgeHealthWeight[band] * value;
		education += kAgeEducationWeight[band] * value;
	}
	pop.health_need_factor = std::clamp(
		health / std::max(pop.health_weight_ref, 1e-9), 0.25, 4.0);
	pop.education_need_factor = std::clamp(
		education / std::max(pop.education_weight_ref, 1e-9), 0.25, 4.0);
	// B2: os quintis acompanham a população (o mix de quintis é preservado).
	rescale_strata_population(pop);
}

void SimCore::phase_demography() {
	// Cadência mensal (A6) escalonada por índice de cidade. O laço é SERIAL: o
	// fluxo de um mês depende só das coortes da própria cidade, mas manter a
	// ordem fixa deixa o resultado determinístico (e barato: ~1/30 por dia).
	const int month_period = ws_.cadence.monthly_period;
	for (std::size_t city_index = 0; city_index < ws_.cities.size();
			++city_index) {
		if (!cadence_due(ws_.day, city_index, month_period)) {
			continue;
		}
		City& city = ws_.cities[city_index];
		// Semeia (primeira vez) e atualiza o perfil que depende da idade.
		apply_demography(city);
		Pop& pop = city.pop;
		if (!pop.demography_ready) {
			continue;
		}
		const auto found = ws_.age_structures.find(city.nation);
		if (found == ws_.age_structures.end() || !found->second.valid) {
			continue;
		}
		const AgeStructure& structure = found->second;
		double band[kAgeBands];
		double total = 0.0;
		for (int index = 0; index < kAgeBands; ++index) {
			band[index] = std::max(0.0, pop.age_band[index]);
			total += band[index];
		}
		if (total <= 0.0) {
			continue;
		}
		// Fluxo do MÊS: as taxas brutas medidas (por mil/ano) sobre a
		// população, divididas por 12.
		const double births = total * structure.births_per_1000 / 1000.0 / 12.0;
		const double deaths = total * structure.deaths_per_1000 / 1000.0 / 12.0;
		// Mortes por faixa: o CDR é o total; os pesos relativos só dizem como
		// ele se reparte. Peso total 0 (população zero em todas as faixas) não
		// chega aqui (`total > 0`), mas a repartição igual fica como fallback.
		double shape[kAgeBands];
		double shape_total = 0.0;
		for (int index = 0; index < kAgeBands; ++index) {
			shape[index] = band[index] * kAgeDeathWeight[index];
			shape_total += shape[index];
		}
		double death_band[kAgeBands] = {};
		for (int index = 0; index < kAgeBands; ++index) {
			death_band[index] = shape_total > 0.0
				? deaths * shape[index] / shape_total
				: deaths / double(kAgeBands);
		}
		// Envelhecimento: fração anual da faixa dividida por 12 (a última faixa
		// não tem para onde sair).
		double aging[kAgeBands] = {};
		for (int index = 0; index + 1 < kAgeBands; ++index) {
			aging[index] = band[index] * kAgeAgingAnnual[index] / 12.0;
		}
		double next[kAgeBands];
		next[0] = band[0] + births - death_band[0] - aging[0];
		for (int index = 1; index + 1 < kAgeBands; ++index) {
			next[index] = band[index] + aging[index - 1] - death_band[index]
				- aging[index];
		}
		next[kAgeBands - 1] = band[kAgeBands - 1] + aging[kAgeBands - 2]
			- death_band[kAgeBands - 1];
		double new_total = 0.0;
		for (int index = 0; index < kAgeBands; ++index) {
			next[index] = std::max(0.0, next[index]);
			new_total += next[index];
		}
		if (new_total <= 0.0) {
			continue;
		}
		// Educação e formação acompanham a idade ativa: a mesma população em
		// idade de trabalhar/estudar sustenta as mesmas pessoas nos níveis.
		// Fator limitado a [0,5; 2,0] para um mês não reescrever a estrutura
		// educacional (o conteúdo é ajustado mês a mês).
		const double old_working = std::max(0.0, pop.working_age);
		const double new_working = next[1] + next[2] + next[3];
		const double ratio = std::clamp(
			new_working / std::max(old_working, 1e-9), 0.5, 2.0);
		for (int level = 0; level < kEducationLevels; ++level) {
			pop.education[level] *= ratio;
		}
		for (int track = 0; track < kEducationLevels - 1; ++track) {
			pop.studying[track] *= ratio;
		}
		// População REAL exibida = urbano + interior (B3). O interior tem a
		// própria demografia (region.cpp, cadência mensal); aqui o urbano
		// mudou, então a população exibida é reconciliada pelo invariante.
		for (int index = 0; index < kAgeBands; ++index) {
			pop.age_band[index] = next[index];
		}
		apply_demography(city);
		reconcile_city_population(city);
	}
}

} // namespace ogs
