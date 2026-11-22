// Módulo de domínio: consumo — needs, cestas, alimento, categorias, governo
// e duráveis (S11 do docs/economy/core_refactor_plan.md).
// SimCore:: métodos movidos de sim_core.cpp — realocação byte-idêntica.
#include "sim_core.h"

#include "profiling.h"
#include "housing.h"
#include "sim_constants.h"
#include "sim_labor.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace ogs {

namespace {
// D-092: elasticidade-preço da demanda por classe de necessidade. A ordem dos
// níveis em needs.json é básico → vida → luxo; qualquer nível além do segundo
// é tratado como luxo (declarado, não inferido de id).
double needs_level_elasticity(int level_index) {
	if (level_index <= 0) {
		return kPriceElasticityBasic;
	}
	if (level_index == 1) {
		return kPriceElasticityLife;
	}
	return kPriceElasticityLuxury;
}
} // namespace

void SimCore::init_pop_needs(Pop& pop) const {
	if (pop.level_want.size() != kMaxNeedsLevels) {
		pop.level_want.assign(kMaxNeedsLevels, 0.0);
	}
	if (pop.level_consumed.size() != kMaxNeedsLevels) {
		pop.level_consumed.assign(kMaxNeedsLevels, 0.0);
	}
	if (pop.level_satisfaction.size() != kMaxNeedsLevels) {
		pop.level_satisfaction.assign(kMaxNeedsLevels, 0.0);
	}
	// F7: gasto por nível (custo unitário da cesta para o teto de renda).
	if (pop.level_spend.size() != kMaxNeedsLevels) {
		pop.level_spend.assign(kMaxNeedsLevels, 0.0);
	}
	// `resize` preserva o prefixo: um bem novo não zera o estoque durável.
	if (pop.durable_stock.size() != static_cast<std::size_t>(std::max(0, ws_.products))) {
		pop.durable_stock.resize(static_cast<std::size_t>(std::max(0, ws_.products)), 0.0);
	}
}

// ------------------------------------------------------------------
// F7/F9 — renda das famílias, impostos, transferências e consumo (cesta
// alimentar, níveis de necessidade e classes de renda).
// ------------------------------------------------------------------

void SimCore::phase_income() {
	// 5. Renda das famílias, impostos, transferências e consumo alimentar.
	// S16: a renda/consumo é independente por cidade (escreve só na própria
	// cidade e nos seus mercados; lê só dados globais imutáveis no dia). Os
	// acumuladores (`foreign_cash` = subtração; `consumed_total`/`consumed_today`
	// = somas) vão para um scratch por cidade e são reduzidos em ordem crescente
	// de cidade numa passada serial — a MESMA ordem do loop serial.
	CityAccumScratch scratch;
	scratch.foreign.assign(ws_.cities.size(), 0.0);
	const std::size_t n_products = static_cast<std::size_t>(ws_.products);
	scratch.consumed_total.assign(ws_.cities.size() * n_products, 0.0);
	scratch.consumed_today.assign(ws_.cities.size() * n_products, 0.0);
	scratch.n_products = n_products;

	auto process_city = [this, &scratch, n_products](
			std::size_t begin, std::size_t end) {
		for (std::size_t city_index = begin; city_index < end; ++city_index) {
			City& city = ws_.cities[city_index];
		Pop& pop = city.pop;
		pop.capital_income = city.capital_income_next;
		city.capital_income_next = 0.0;
		pop.rent_income = city.rent_income_next;
		city.rent_income_next = 0.0;
		// B7: o imposto incide só sobre a parcela FORMAL do salário. A soma por
		// quintil (e o `pop.taxes`) é calculada em refresh_strata_taxes, que
		// também preenche a renda de mercado dos quintis (com a penalidade
		// informal). Roda aqui, antes de transferências/disponível/gasto.
		refresh_strata_taxes(city);
		// Orçamento público equilibrado: tudo o que é arrecadado volta como
		// transferência. Sem isso, o governo acumula receita e drena demanda.
		pop.transfers = pop.taxes * kTransferShare;
		pop.disposable = std::max(0.0,
			pop.wage_income + pop.capital_income + pop.rent_income
			+ pop.transfers - pop.taxes);
		// Renda externa (serviços urbanos/pesca/turismo): o resto do mundo paga
		// por serviços da cidade. Entra no orçamento das famílias e sai do setor
		// externo — é a renda que financia a importação de alimento onde não há
		// terra produtiva. Não é tributada nem infla a folha doméstica (§20).
		// O índice por cidade vem do PIB PPP per capita da nação (GeographyService,
		// nations.csv), normalizado para média mundial = 1,0; cidade sem índice
		// declarado (cenário sintético, save antigo) mantém a média (1,0).
		//
		// F7 calibração: é um PISO DE SUBSISTÊNCIA, então o índice é limitado à
		// média mundial (kExternalIncomeIndexCap). Escalado até o topo, esse
		// bloco sozinho pagava 3× a cesta completa das famílias dos EUA/DEU e
		// punha o mundo inteiro no nível luxury. A renda acima da subsistência
		// vem do valor adicionado (salário + lucro), medido na cidade.
		pop.external_income = city.simulation_population
			* kExternalIncomePerCapita
			* std::min(city.external_income_index, kExternalIncomeIndexCap);
		pop.disposable += pop.external_income;
		scratch.foreign[city_index] += pop.external_income;

		// Necessidade alimentar do dia em toneladas-equivalentes de grão (B3) e
		// custo EFETIVO da cesta: os preços dos alimentos que a cidade REALMENTE
		// tem em estoque (na proporção de disponibilidade usada na compra) mais
		// o resíduo importado ao CIF conhecido. Antes, o subsídio usava o
		// alimento mais barato do MUNDO — pão/processado sem estoque local — e
		// subestimava em 3-4× o custo do grão da prateleira; a cidade comprava
		// só uma fração da necessidade mesmo gastando todo o orçamento
		// (NGA-LAG, SYR-DAM, SOL-HAR em 365 d).
		pop.need = city.simulation_population * kNeedPerCapita;
		double food_local_cost = 0.0;
		double food_import_cost = 0.0;
		double food_unit_cost = 0.0;
		food_basket_cost(city, pop.need, food_local_cost, food_import_cost,
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
			* kServiceFoodGuardDays;
		const double domestic_income =
			std::max(0.0, pop.disposable - pop.external_income);
		double service_base = domestic_income;
		if (basket_cost > 0.0 && domestic_income + std::max(0.0, pop.savings)
				< basket_cost) {
			service_base = std::max(0.0, domestic_income - basket_cost);
		}
		pop.service_spend = service_base * kServiceSpendShare;
		city.service_cash += pop.service_spend;
		city.government_budget += pop.taxes - pop.transfers;

		// F7 calibração — RENDA POR ESTRATO. A folha por escolaridade vem do
		// mercado de trabalho (já paga e escalada pela liquidez); o lucro vai só
		// para os DONOS (estrato superior); a renda externa de subsistência é
		// uniforme per capita. O orçamento de compra continua sendo o da cidade
		// (o dinheiro é conservado); o que muda por estrato é QUEM pode comprar
		// cada nível de cesta.
		double stratum_pop_sum = 0.0;
		for (int level = 0; level < kEducationLevels; ++level) {
			stratum_pop_sum += std::max(0.0, pop.stratum_population[level]);
		}
		for (int level = 0; level < kEducationLevels; ++level) {
			const double share = stratum_pop_sum > 1e-9
				? std::max(0.0, pop.stratum_population[level]) / stratum_pop_sum
				: 1.0 / double(kEducationLevels);
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
		pop.stratum_capital_income[kEducationLevels - 1] =
			std::max(0.0, pop.capital_income);
		pop.stratum_disposable[kEducationLevels - 1] +=
			std::max(0.0, pop.capital_income);

		// F7 calibração — classes de consumo do dia (Gini declarado ancorado no
		// índice de renda; ver `refresh_income_classes`). Continuam publicadas
		// (contrato de status/UI e testes legados); a B2 NÃO consome mais por
		// elas: o consumo, a poupança e o patrimônio são por QUINTIL.
		refresh_income_classes(city);
		// B2: renda dos cinco quintis a partir dos MESMOS totais do dia
		// (conservação por construção — ver strata.cpp).
		refresh_strata_income(city);
		double cash[kIncomeStrata] = {};
		double food_spend_s[kIncomeStrata] = {};
		double consumed_s[kIncomeStrata] = {};
		double need_s[kIncomeStrata] = {};
		double people_s[kIncomeStrata] = {};
		double people_total = 0.0;
		for (int s = 0; s < kIncomeStrata; ++s) {
			people_s[s] = std::max(0.0, pop.stratum_people[s]);
			people_total += people_s[s];
			// A necessidade alimentar é per capita: cada quintil come a sua.
			need_s[s] = people_s[s] * kNeedPerCapita;
		}
		const bool strata_live = pop.strata_ready && people_total > 1e-9;
		if (strata_live) {
			for (int s = 0; s < kIncomeStrata; ++s) {
				cash[s] = std::max(0.0, pop.stratum_savings[s])
					+ std::max(0.0, pop.stratum_net_income[s])
					- std::max(0.0, pop.stratum_service_spend[s]);
			}
		} else {
			// Cidade sem estrato semeado (não acontece no mundo real; cenário
			// sintético antigo): um único orçamento no quintil do meio, para a
			// fase nunca perder dinheiro.
			cash[kIncomeStrata / 2] = std::max(0.0,
				pop.savings + pop.disposable - pop.service_spend);
		}
		const double cash_start = [&cash]() {
			double total = 0.0;
			for (int s = 0; s < kIncomeStrata; ++s) {
				total += cash[s];
			}
			return total;
		}();
		auto cash_total = [&cash]() {
			double total = 0.0;
			for (int s = 0; s < kIncomeStrata; ++s) {
				total += cash[s];
			}
			return total;
		};
		// Subsídio de subsistência: o governo garante a cesta alimentar mínima
		// de quem não a alcança. O valor sai do orçamento público e entra nos
		// estratos com maior FALTA para a cesta (proporcional), de forma
		// determinística. Primeiro a parcela que está na prateleira; depois o
		// resíduo importado ao CIF conhecido.
		double basket_short[kIncomeStrata] = {};
		double basket_short_total = 0.0;
		for (int s = 0; s < kIncomeStrata; ++s) {
			const double target = need_s[s] * food_unit_cost
				* kServiceFoodGuardDays;
			basket_short[s] = std::max(0.0, target - cash[s]);
			basket_short_total += basket_short[s];
		}
		auto add_cash = [&](double amount) {
			if (amount <= 0.0) {
				return;
			}
			for (int s = 0; s < kIncomeStrata; ++s) {
				const double share = basket_short_total > 1e-9
					? basket_short[s] / basket_short_total
					: (people_total > 1e-9
						? people_s[s] / people_total
						: 1.0 / double(kIncomeStrata));
				cash[s] += amount * share;
			}
		};
		double food_topup_local = 0.0;
		if (food_local_cost > 0.0 && cash_total() < food_local_cost) {
			food_topup_local = std::min(food_local_cost - cash_total(),
				std::max(0.0, city.government_budget));
			if (food_topup_local > 0.0) {
				add_cash(food_topup_local);
				city.government_budget -= food_topup_local;
				pop.transfers += food_topup_local;
			}
		}
		double food_topup_import = 0.0;
		if (food_import_cost > 0.0) {
			const double own_left = std::max(0.0, cash_total() - food_local_cost);
			const double shortfall = std::max(0.0, food_import_cost - own_left);
			food_topup_import = std::min(shortfall,
				std::max(0.0, city.government_budget));
			if (food_topup_import > 0.0) {
				// Fica com as famílias: a importação é paga na fase 6 com
				// `pop.savings` quando firma e governo não cobrem.
				add_cash(food_topup_import);
				city.government_budget -= food_topup_import;
				pop.transfers += food_topup_import;
			}
		}

		// Consumo alimentar (B2). A cesta é comprada do orçamento AGREGADO da
		// cidade — as famílias compartilham a mesa (era assim no modelo anterior,
		// e é o que evita o pobre passar fome enquanto o rico tem caixa parado).
		// Cada quintil consome a MESMA necessidade per capita; o CUSTO é
		// atribuído por pessoa, então a fração da renda gasta em comida cai com
		// o estrato sem racionar o acesso à prateleira. O dinheiro sai do caixa
		// dos quintis na proporção do caixa de cada um.
		double consumed = 0.0;      // toneladas-equivalentes de grão
		double food_spend = 0.0;
		double total_avail = 0.0;
		std::vector<double> avail_ge(ws_.food_products.size(), 0.0);
		for (int slot = 0; slot < static_cast<int>(ws_.food_products.size()); ++slot) {
			const int product = food_product(slot);
			avail_ge[slot] = city.markets[product].stock * food_calories(product);
			total_avail += avail_ge[slot];
		}
		double remaining_need_total = 0.0;
		for (int s = 0; s < kIncomeStrata; ++s) {
			remaining_need_total += need_s[s];
		}
		double food_budget = cash_total();
		for (int slot = 0;
				slot < static_cast<int>(ws_.food_products.size())
				&& remaining_need_total > 1e-12; ++slot) {
			const int product = food_product(slot);
			const double factor = food_calories(product);
			if (factor <= 0.0) {
				continue;
			}
			Market& market = city.markets[product];
			const double price = market.price;
			if (price <= 0.0) {
				continue;
			}
			double want_ge = total_avail > 0.0
				? remaining_need_total * (avail_ge[slot] / total_avail)
				: (slot == 0 ? remaining_need_total : 0.0);
			want_ge = std::min(want_ge, remaining_need_total);
			// D-092: o desejo publicado (`desired`) mantém a regra da D-091
			// (limitado pelo orçamento, sinal da IA); a quantidade COMPRADA ao
			// preço corrente leva a elasticidade de básico.
			const double need_tons = want_ge / factor;
			double desired_tons = need_tons;
			if (desired_tons * price > food_budget) {
				desired_tons = std::max(0.0, food_budget) / price;
			}
			if (desired_tons <= 1e-12) {
				continue;
			}
			const double demand_scale = price_demand_scale(
				price, base_price(product), kPriceElasticityBasic);
			const double buy_tons = desired_tons * demand_scale;
			market.desired += desired_tons;
			// D-091: a comida é financiada (desired_tons já vem limitado pelo
			// orçamento), então entra integral na pressão de preço — menos a
			// elasticidade de básico (D-092).
			market.effective += std::max(0.0, buy_tons);
			pop.desired_by_category[slot] += desired_tons;
			const double quantity = std::min(buy_tons, market.stock);
			if (quantity <= 0.0) {
				continue;
			}
			const double paid = quantity * price;
			market.stock -= quantity;
			market.demand += quantity;
			market.sold += quantity;
			food_budget -= paid;
			food_spend += paid;
			consumed += quantity * factor;
			pop.consumed_by_category[slot] += quantity;
			city.revenue_by_product[product] += paid;
			city.firm_cash += paid;
			scratch.consumed_total[city_index * n_products
				+ static_cast<std::size_t>(product)] += quantity;
			scratch.consumed_today[city_index * n_products
				+ static_cast<std::size_t>(product)] += quantity;
			remaining_need_total -= quantity * factor;
		}
		// Atribuição por pessoa: necessidade e custo da cesta.
		pop.need = 0.0;
		for (int s = 0; s < kIncomeStrata; ++s) {
			pop.need += need_s[s];
		}
		for (int s = 0; s < kIncomeStrata; ++s) {
			const double share = pop.need > 1e-9
				? need_s[s] / pop.need : 0.0;
			consumed_s[s] = consumed * share;
			food_spend_s[s] = food_spend * share;
		}
		{
			const double cash_before = cash_total();
			for (int s = 0; s < kIncomeStrata; ++s) {
				const double share = cash_before > 1e-9
					? std::max(0.0, cash[s]) / cash_before : 0.0;
				cash[s] = std::max(0.0, cash[s] - food_spend * share);
			}
		}
		// F9 — PISO DE SUBSISTÊNCIA AGRÍCOLA (autoconsumo em espécie). Só age
		// quando a cidade NÃO conseguiu cobrir a cesta: as famílias agrícolas
		// comem da própria produção, limitado pela necessidade das famílias e
		// por uma fração da produção local. O alimento sai do ESTOQUE
		// (conservação): não é comprado, então não há pagamento nem criação de
		// comida.
		double in_kind = 0.0;
		double remaining_total = 0.0;
		for (int s = 0; s < kIncomeStrata; ++s) {
			remaining_total += std::max(0.0, need_s[s] - consumed_s[s]);
		}
		if (remaining_total > 1e-9 && city.agri_output_tons > 0.0
				&& city.sector_workers[0] > 0.0) {
			const double farm_household_size = std::clamp(
				city.simulation_population
					/ std::max(city.pop.labor_force, 1e-9), 1.0, 6.0);
			const double household_need = city.sector_workers[0]
				* farm_household_size * kNeedPerCapita;
			// G: com plano sazonal a produção do dia é a da SAFRA (média anual
			// em t/dia), não a capacidade nominal da fazenda — o autoconsumo não
			// pode drenar um estoque que a lavoura só repõe na colheita.
			const double farm_output = city.agri_seasonal_daily > 0.0
				? city.agri_seasonal_daily : city.agri_output_tons;
			const double own_production = farm_output
				* ws_.agri_own_consumption_share;
			in_kind = std::min(
				std::min(remaining_total, household_need), own_production);
			double left = in_kind;
			for (int slot = 0;
					slot < static_cast<int>(ws_.food_products.size())
					&& left > 1e-12; ++slot) {
				const int product = food_product(slot);
				const double factor = food_calories(product);
				if (factor <= 0.0) {
					continue;
				}
				Market& market = city.markets[product];
				const double available = std::max(0.0, market.stock * factor);
				const double take = std::min(left, available);
				if (take <= 1e-12) {
					continue;
				}
				const double tons = take / factor;
				market.stock = std::max(0.0, market.stock - tons);
				pop.consumed_by_category[slot] += tons;
				// G: consumo em espécie medido à parte (o `consumed_by_category`
				// mistura compra de mercado + em espécie; a conta da entressafra
				// precisa separar as duas).
				if (slot < kMaxFoodProducts) {
					city.agri_in_kind[slot] += tons;
				}
				scratch.consumed_total[city_index * n_products
					+ static_cast<std::size_t>(product)] += tons;
				scratch.consumed_today[city_index * n_products
					+ static_cast<std::size_t>(product)] += tons;
				left -= take;
			}
			in_kind -= left;
			consumed += in_kind;
			for (int s = 0; s < kIncomeStrata; ++s) {
				const double share = pop.need > 1e-9
					? need_s[s] / pop.need : 0.0;
				consumed_s[s] += in_kind * share;
			}
		}
		city.in_kind_food = in_kind;
		for (int s = 0; s < kIncomeStrata; ++s) {
			pop.stratum_need[s] = need_s[s];
			pop.stratum_food_spend[s] = food_spend_s[s];
			pop.stratum_consumed[s] = consumed_s[s];
		}
		pop.consumed = consumed;
		pop.food_spend = food_spend;
		pop.satisfaction = pop.need > 0.0
			? std::clamp(consumed / pop.need, 0.0, 1.0) : 1.0;
		// B7: a mortalidade mensal roda ANTES da renda (usa a satisfação de
		// ontem); guarda o retrato do dia para o próximo fechamento.
		pop.satisfaction_prev = pop.satisfaction;
		// B7: inflação de comida do fechamento mensal (cadência A6): o primeiro
		// fechamento só grava a referência; os seguintes publicam a variação.
		if (cadence_due(ws_.day, city_index, ws_.cadence.monthly_period)) {
			if (pop.food_price_ref <= 0.0) {
				pop.food_price_ref = food_unit_cost;
				pop.food_inflation_month = 0.0;
			} else if (pop.food_price_ref > 0.0) {
				pop.food_inflation_month = food_unit_cost
					/ pop.food_price_ref - 1.0;
				pop.food_price_ref = food_unit_cost;
			}
		}

		// Subsídio de subsistência LOCAL não gasto volta ao governo: sem isso, a
		// cidade sem estoque de alimento convertia o subsídio em poupança e ele
		// acabava financiando life/luxury (dinheiro conservado, mas fora do
		// destino). A parcela de IMPORTAÇÃO não volta: ela precisa continuar com
		// as famílias até a fase 6 pagar a carga.
		if (food_topup_local > 0.0) {
			// Devolve no máximo o que SOBROU em caixa: a sobra é o top-up (o
			// último dinheiro a entrar). Devolver `top-up − sobra` devolvia
			// justamente o que foi gasto em comida — dinheiro criado
			// (ERI-KER, dia 74: 14,23 XGS).
			double refunded = std::min(food_topup_local,
				std::max(0.0, cash_total()));
			double left = refunded;
			for (int s = 0; s < kIncomeStrata && left > 1e-12; ++s) {
				int top = 0;
				for (int t = 1; t < kIncomeStrata; ++t) {
					if (cash[t] > cash[top]) {
						top = t;
					}
				}
				const double take = std::min(left, std::max(0.0, cash[top]));
				cash[top] = std::max(0.0, cash[top] - take);
				left -= take;
			}
			refunded -= left;
			if (refunded > 0.0) {
				city.government_budget += refunded;
				pop.transfers -= refunded;
			}
		}

		// F3: necessidades em três níveis (basic/life/luxury) POR ESTRATO. O
		// orçamento é sequencial: o alimento já foi pago; aqui entram as demais
		// categorias do basic, depois life e por último luxury. O teto de cada
		// quintil vem da sua renda per capita aos preços de hoje.
		if (ws_.needs_loaded) {
			// Duráveis: descarte com perda de durable_replacement_per_year.
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
			// Teto de consumo por quintil: custo acumulado da cesta aos preços
			// de hoje contra a renda per capita do estrato. O nível vale para
			// o gasto; a falta de renda NÃO inventa consumo com poupança.
			double cumulative_cost[kMaxNeedsLevels] = {};
			{
				double running_cost = 0.0;
				const double basket_population =
					std::max(1e-9, city.simulation_population);
				for (int level_index = 0;
						level_index < static_cast<int>(ws_.needs_levels.size())
						&& level_index < kMaxNeedsLevels; ++level_index) {
					running_cost += level_basket_cost(city, level_index);
					cumulative_cost[level_index] = running_cost / basket_population;
				}
			}
			int level_cap[kIncomeStrata] = {};
			for (int s = 0; s < kIncomeStrata; ++s) {
				const double per_capita = people_s[s] > 1e-9
					? std::max(0.0, pop.stratum_net_income[s]) / people_s[s]
					: 0.0;
				for (int level_index = 0;
						level_index < static_cast<int>(ws_.needs_levels.size())
						&& level_index < kMaxNeedsLevels; ++level_index) {
					if (per_capita + 1e-12 >= cumulative_cost[level_index]
							* ws_.income_basket_margin) {
						level_cap[s] = level_index + 1;
					} else {
						break;
					}
				}
				pop.stratum_level[s] = level_cap[s];
			}
			// Reserva de comida, aluguel e poupança-alvo: o que o estrato NÃO
			// pode gastar em life/luxury hoje. O aluguel é reservado por
			// inquilino (posse do §19 por estrato) e a poupança-alvo é a taxa
			// do quintil × ciclo de vida (B1) sobre a renda acima de comida e
			// aluguel. Reservar não gasta: fica na poupança do estrato.
			const double rent_obligation =
				housing::daily_rent_obligation(ws_, city_index);
			double tenant_weight[kIncomeStrata] = {};
			double tenant_total = 0.0;
			for (int s = 0; s < kIncomeStrata; ++s) {
				tenant_weight[s] = people_s[s] * std::clamp(
					pop.stratum_tenure[s][TENURE_RENT], 0.0, 1.0);
				tenant_total += tenant_weight[s];
			}
			double needs_budget[kIncomeStrata] = {};
			for (int s = 0; s < kIncomeStrata; ++s) {
				const double rent_reserve = tenant_total > 1e-9
					? rent_obligation * tenant_weight[s] / tenant_total : 0.0;
				pop.stratum_rent_bill[s] = rent_reserve;
				const double food_reserve = need_s[s] * kFoodReserveDays
					* food_unit_cost;
				const double protected_savings = std::min(
					std::max(0.0, pop.stratum_savings[s]), food_reserve);
				const double saving_target = pop.stratum_saving_rate[s]
					* std::max(0.0, pop.stratum_net_income[s]
						- food_spend_s[s] - rent_reserve);
				needs_budget[s] = std::max(0.0, cash[s]
					- protected_savings - rent_reserve - saving_target);
			}
			double needs_spent = 0.0;
			double weighted = 0.0;
			double weight_sum = 0.0;
			for (double& level_spend : pop.level_spend) {
				level_spend = 0.0;
			}
			double spend_s[kIncomeStrata] = {};
			double spend_total_prev = 0.0;
			for (int level_index = 0;
					level_index < static_cast<int>(ws_.needs_levels.size())
					&& level_index < kMaxNeedsLevels; ++level_index) {
				const NeedsLevel& level =
					ws_.needs_levels[static_cast<std::size_t>(level_index)];
				double level_want = 0.0;
				double level_got = 0.0;
				if (level_index == 0) {
					pop.level_spend[0] += food_spend;
				}
				for (int s = 0; s < kIncomeStrata; ++s) {
					if (people_s[s] <= 1e-9) {
						continue;
					}
					const bool affordable = level_index == 0
						|| level_index + 1 <= level_cap[s];
					for (const NeedsCategory& category : level.categories) {
						// A cesta alimentar básica tem laço próprio (validado) e
						// já foi consumida acima; reaproveita `need/consumed` do
						// estrato para não cobrar duas vezes a mesma refeição.
						if (level_index == ws_.needs_food_level
								&& category.id == std::string("food")) {
							level_want += need_s[s];
							level_got += consumed_s[s];
							continue;
						}
						const double want_ge = category.per_capita_day * people_s[s];
						level_want += want_ge;
						if (!affordable) {
							// Renda insuficiente: não compra (o consumo efetivo
							// respeita a renda), mas a NECESSIDADE entra no
							// mercado como desejo — o sinal de escassez da IA.
							register_category_demand(city, category, want_ge);
							continue;
						}
						level_got += consume_category(
							city, category, want_ge, needs_budget[s], spend_s[s],
							scratch, city_index,
							needs_level_elasticity(level_index));
					}
				}
				double spend_level = 0.0;
				for (int s = 0; s < kIncomeStrata; ++s) {
					spend_level += spend_s[s];
				}
				needs_spent = spend_level;
				pop.level_spend[level_index] += spend_level - spend_total_prev;
				spend_total_prev = spend_level;
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
			// F8: gancho de saúde privada.
			pop.needs_satisfaction = std::clamp(pop.needs_satisfaction
				+ kServiceHealthSatisfactionBonus * city.private_health_cover,
				0.0, 1.0);
			// Nível alcançado = maior nível com pelo menos metade da cesta.
			pop.needs_level = 0;
			for (int level_index = 0;
					level_index < static_cast<int>(ws_.needs_levels.size())
					&& level_index < kMaxNeedsLevels; ++level_index) {
				if (pop.level_satisfaction[level_index] >= 0.5) {
					pop.needs_level = level_index + 1;
				} else {
					break;
				}
			}
			pop.needs_spend = needs_spent;
			// A sobra do orçamento de necessidades (o que não foi gasto) fica
			// com o estrato como poupança; o gasto efetivo já saiu de
			// `needs_budget` por referência.
			for (int s = 0; s < kIncomeStrata; ++s) {
				const double spent = spend_s[s];
				cash[s] = std::max(0.0, cash[s] - spent);
			}
			// Fatia da POP em cada nível de consumo alcançado (por quintil).
			for (int level_index = 0; level_index < kIncomeLevels;
					++level_index) {
				pop.income_level_share[level_index] = 0.0;
			}
			const double city_population = city.simulation_population;
			for (int s = 0; s < kIncomeStrata; ++s) {
				const int reached = std::clamp(level_cap[s], 0, kIncomeLevels - 1);
				pop.income_level_share[reached] += people_s[s];
			}
			double population_seen = 0.0;
			int median_level = 0;
			bool median_found = false;
			for (int level_index = 0; level_index < kIncomeLevels;
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
			for (int level_index = 0; level_index < kIncomeLevels;
					++level_index) {
				pop.income_level_share[level_index] = 0.0;
			}
			pop.income_level_share[pop.income_level] = 1.0;
		}
		// Consumo institucional do Estado (paper/computer) — decisão §12.10.
		consume_government(city, scratch, city_index);
		// Fecha a poupança: a soma dos quintis É o escalar da cidade.
		pop.savings = 0.0;
		for (int s = 0; s < kIncomeStrata; ++s) {
			if (pop.strata_ready) {
				pop.stratum_savings[s] = std::max(0.0, cash[s]);
				pop.savings += pop.stratum_savings[s];
			} else {
				pop.savings += std::max(0.0, cash[s]);
			}
		}
		}
	};
	pool().parallel_for(ws_.cities.size(), process_city);

	// Redução serial em ordem crescente de cidade (a ordem do loop serial).
	for (std::size_t city_index = 0; city_index < ws_.cities.size(); ++city_index) {
		ws_.foreign_cash -= scratch.foreign[city_index];
		for (int product = 0; product < ws_.products; ++product) {
			ws_.consumed_total[product] += scratch.consumed_total[
				city_index * n_products + static_cast<std::size_t>(product)];
			ws_.consumed_today[product] += scratch.consumed_today[
				city_index * n_products + static_cast<std::size_t>(product)];
		}
	}

	OGS_SIM_PHASE_LAP(kPhaseIncome);
}
double SimCore::level_basket_cost(const City& city,
		int level_index) const {
	if (level_index < 0
			|| level_index >= static_cast<int>(ws_.needs_levels.size())) {
		return 0.0;
	}
	const NeedsLevel& level =
		ws_.needs_levels[static_cast<std::size_t>(level_index)];
	const double population = std::max(0.0, city.simulation_population);
	double cost = 0.0;
	for (const NeedsCategory& category : level.categories) {
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
void SimCore::food_basket_cost(const City& city, double need,
		double& local_cost, double& import_cost, double& per_ge) const {
	local_cost = 0.0;
	import_cost = 0.0;
	per_ge = 0.0;
	const int slots = std::min(
		static_cast<int>(ws_.food_products.size()), kMaxFoodProducts);
	if (slots <= 0) {
		return;
	}
	// Uma passada: guarda a disponibilidade e o preço por tonelada-equivalente
	// de cada slot (evita repetir a busca de fator calórico, que é O(slots)).
	double stock_ge[kMaxFoodProducts];
	double unit_price[kMaxFoodProducts];
	double total_avail = 0.0;
	double cheapest = 0.0;
	double cheapest_import = 0.0;
	for (int slot = 0; slot < slots; ++slot) {
		const int product = food_product(slot);
		const double factor = food_calories(product);
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
bool SimCore::category_top_market(City& city,
		const NeedsCategory& category, Market*& market, double& factor,
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
void SimCore::register_category_demand(City& city,
		const NeedsCategory& category, double want_ge) {
	if (want_ge <= 0.0) {
		return;
	}
	Market* market = nullptr;
	double factor = 0.0;
	double durable_scale = 1.0;
	if (!category_top_market(city, category, market, factor, durable_scale)) {
		return;
	}
	if (durable_scale <= 0.0) {
		return;
	}
	// Desejo declarado do tamanho da NECESSIDADE, independentemente de renda
	// ou estoque: é o sinal que a IA lê (`market.shortage`) para abrir a
	// cadeia do bem final. A compra efetiva fica na consume_category e
	// respeita orçamento e estoque; nada é comprado aqui. Como não há renda
	// por trás, o valor entra também em `latent`: a IA vê a escassez, mas o
	// preço não sobe por desejo sem poder de compra.
	const double desired_tons = want_ge / factor * durable_scale;
	market->desired += desired_tons;
	market->latent += desired_tons;
}
double SimCore::consume_category(City& city,
		const NeedsCategory& category, double want_ge, double& budget,
		double& spend, CityAccumScratch& scratch, std::size_t city_index,
		double elasticity) {
	if (want_ge <= 0.0 || category.substitutes.empty()) {
		return 0.0;
	}
	// Demanda DESEJADA da categoria: registrada pelo tamanho do desejo no
	// substituto preferido, não pelo estoque. É o sinal de escassez que a IA
	// lê; a compra abaixo é que respeita caixa e estoque. A parcela que o
	// orçamento do dia não cobre entra em `latent` e não pressiona o preço.
	// D-092: a quantidade COMPRADA ao preço corrente cai com a elasticidade da
	// classe (básico pouco, luxo muito); o desejo físico continua integral em
	// `desired` para o investimento ver a falta.
	double demand_scale = 1.0;
	{
		Market* signal_market = nullptr;
		double signal_factor = 0.0;
		double signal_scale = 1.0;
		if (category_top_market(city, category, signal_market,
				signal_factor, signal_scale) && signal_scale > 0.0) {
			double signal_base = 0.0;
			for (std::size_t slot = 0; slot < category.substitutes.size(); ++slot) {
				const int product = category.substitutes[slot];
				if (product < 0 || product >= ws_.products) {
					continue;
				}
				if (&city.markets[product] == signal_market) {
					signal_base = base_price(product);
					break;
				}
			}
			demand_scale = price_demand_scale(
				signal_market->price, signal_base, elasticity);
			const double full_tons = want_ge / signal_factor * signal_scale;
			const double elastic_tons = full_tons * demand_scale;
			const double backed_tons = std::min(elastic_tons,
				signal_market->price > 0.0
					? std::max(0.0, budget) / signal_market->price : 0.0);
			signal_market->desired += full_tons;
			signal_market->latent += std::max(0.0, full_tons - backed_tons);
			// D-091: só a parcela financiada (backed_tons) pressiona o preço;
			// o desejo acima do orçamento do dia fica em `latent` (sinal da IA).
			signal_market->effective += backed_tons;
		}
	}
	double remaining = want_ge * demand_scale;
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
		Market& market = city.markets[product];
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
		scratch.consumed_total[city_index * scratch.n_products
			+ static_cast<std::size_t>(product)] += quantity;
		scratch.consumed_today[city_index * scratch.n_products
			+ static_cast<std::size_t>(product)] += quantity;
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
void SimCore::consume_government(City& city,
		CityAccumScratch& scratch, std::size_t city_index) {
	if (!ws_.needs_loaded || ws_.government_products.empty()) {
		return;
	}
	// Teto prudencial: a compra institucional não pode consumir mais que uma
	// fração do valor da cesta alimentar diária da cidade. Sem o teto, o
	// computer (preço alto) drenava o orçamento que importa comida em nações
	// sem terra (EGY 1,000 → 0,708 em 365 d).
	double cheapest_food = 0.0;
	for (int slot = 0; slot < static_cast<int>(ws_.food_products.size()); ++slot) {
		const int product = food_product(slot);
		const double factor = food_calories(product);
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
			* kNeedPerCapita * cheapest_food;
		budget = std::min(budget,
			food_value * kGovernmentGoodsFoodShare);
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
		Market& market = city.markets[product];
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
		// D-091: compra institucional financiada pelo orçamento público.
		market.effective += desired_tons;
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
		scratch.consumed_total[city_index * scratch.n_products
			+ static_cast<std::size_t>(product)] += quantity;
		scratch.consumed_today[city_index * scratch.n_products
			+ static_cast<std::size_t>(product)] += quantity;
	}
}
} // namespace ogs
