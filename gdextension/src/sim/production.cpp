// Módulo de domínio: produção — capacidade, extração, receitas e matérias-primas
// (S10 do docs/economy/core_refactor_plan.md).
// SimCore:: métodos movidos de sim_core.cpp — realocação byte-idêntica.
#include "sim_core.h"

#include "efficiency.h"
#include "profiling.h"
#include "sim_constants.h"
#include "sim_labor.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace ogs {
// Eficiência da extração (0.5..1). Sem reserva declarada não há qualidade
// conhecida: eficiência 1 (comportamento legado).
double SimCore::extraction_efficiency(const Deposit& deposit) {
	if (!deposit.tracked) {
		return 1.0;
	}
	return kQualityEfficiencyFloor
		+ (1.0 - kQualityEfficiencyFloor) * std::clamp(deposit.quality, 0.0, 1.0);
}
double SimCore::base_price(int product) const {
	if (product >= 0 && product < ws_.products) {
		return ws_.base_prices[product];
	}
	return 40.0;
}
int SimCore::food_product(int slot) const {
	return (slot >= 0 && slot < static_cast<int>(ws_.food_products.size()))
		? ws_.food_products[static_cast<std::size_t>(slot)] : -1;
}
double SimCore::food_calories(int product) const {
	for (std::size_t slot = 0; slot < ws_.food_products.size(); ++slot) {
		if (ws_.food_products[slot] == product) {
			return ws_.food_factors[slot];
		}
	}
	return 0.0;
}
int SimCore::pick_recipe(const City& city, int output_product) const {
	int best = -1;
	double best_cost = 0.0;
	for (std::size_t index = 0; index < ws_.recipes.size(); ++index) {
		const Recipe& recipe = ws_.recipes[index];
		if (recipe.output != output_product || recipe.input_count <= 0
				|| recipe.yield <= 0.0) {
			continue;
		}
		bool available = true;
		double cost = 0.0;
		for (int slot = 0; slot < recipe.input_count; ++slot) {
			const int input = recipe.inputs[slot];
			const double qty = recipe.input_qty[slot];
			if (input < 0 || input >= ws_.products || qty <= 0.0) {
				available = false;
				break;
			}
			const Market& market = city.markets[input];
			// Insumo só conta como disponível quando há estoque físico: uma
			// receita sem insumo produziria zero e venceria a comparação de custo.
			if (market.stock <= 1e-9) {
				available = false;
				break;
			}
			const double unit_price = market.price > 0.0
				? market.price : base_price(input);
			cost += qty * unit_price / recipe.yield;
		}
		if (!available) {
			continue;
		}
		// F0 (co-produção): o custo por tonelada do produto principal é
		// abatido pelo valor das saídas extras, na mesma base por lote.
		for (int slot = 0; slot < recipe.byproduct_count; ++slot) {
			const int product = recipe.byproducts[slot];
			if (product < 0 || product >= ws_.products) {
				continue;
			}
			const Market& market = city.markets[product];
			const double price = market.price > 0.0
				? market.price : base_price(product);
			cost -= recipe.byproduct_qty[slot] * price / recipe.yield;
		}
		cost = std::max(0.0, cost);
		if (best < 0 || cost < best_cost) {
			best = static_cast<int>(index);
			best_cost = cost;
		}
	}
	return best;
}
void SimCore::phase_production() {
	// 4. Produção: cada construção declara insumo e produto. A produção segue
	// as vendas do dia anterior (com folga de estoque), limitada pela
	// capacidade, pela aptidão territorial e pelos insumos disponíveis.
	// Capacidade por cidade/produto, calculada uma vez por dia. Antes, cada
	// construção varria TODAS as construções do mundo para achar a capacidade
	// local do seu produto — O(construções²), inviável em escala mundial.
	std::vector<std::array<double, kMaxProducts>> capacity_by_city_product(
		ws_.cities.size());
	for (auto& slots : capacity_by_city_product) {
		slots.fill(0.0);
	}
	for (const Building& building : ws_.buildings) {
		if (building.output_product < 0 || building.output_product >= ws_.products) {
			continue;
		}
		const auto found = ws_.city_lookup.find(building.city);
		if (found == ws_.city_lookup.end()) {
			continue;
		}
		// G — a lavoura com plano sazonal NÃO produz diariamente: a safra entra
		// em `phase_agriculture` dentro da janela de colheita. Somar a
		// capacidade da fazenda aqui contaria a mesma tonelada duas vezes.
		if (crop_planned_at(static_cast<std::size_t>(found->second),
				building.output_product)) {
			continue;
		}
		capacity_by_city_product[static_cast<std::size_t>(found->second)]
			[building.output_product] += building.capacity * building.output_scale;
	}

	// S16: a produção é independente por cidade (cada construção escreve só na
	// própria cidade e em si mesma; lê só dados globais imutáveis no dia). Os
	// acumuladores `produced`/`produced_today` são somas de double por produto:
	// para o resultado ser bit-idêntico ao serial (que soma na ordem do vetor
	// global `ws_.buildings`), cada construção grava a própria contribuição num
	// scratch indexado pelo seu índice global, e uma passada serial reduz em
	// ordem crescente de índice de construção — a MESMA ordem do loop serial.
	const std::size_t n_buildings = ws_.buildings.size();
	const std::size_t n_products = static_cast<std::size_t>(ws_.products);
	std::vector<double> produced_scratch(n_buildings * n_products, 0.0);
	std::vector<double> produced_today_scratch(n_buildings * n_products, 0.0);

	auto process_city = [this, &capacity_by_city_product, &produced_scratch,
			&produced_today_scratch, n_products](
			std::size_t begin, std::size_t end) {
		for (std::size_t city_index = begin; city_index < end; ++city_index) {
			City& city = ws_.cities[city_index];
			const std::vector<int>& local_buildings =
				ws_.buildings_by_city[city_index];
			for (const int bi : local_buildings) {
				Building& building =
					ws_.buildings[static_cast<std::size_t>(bi)];
				if (building.output_product < 0
						|| building.output_product >= ws_.products) {
					continue;
				}
				// D-125: planta em troca de produto/modo não produz no dia
				// (transição); a eficiência corrente multiplica a produção.
				if (building.idle_today || building.mothballed) {
					continue;
				}
				const double efficiency = std::clamp(
					building.production_efficiency, 0.0, 1.0);
				// G — lavoura sazonal: a produção diária é suprimida (a safra
				// vem de phase_agriculture na janela de colheita).
				if (crop_planned_at(city_index, building.output_product)) {
					continue;
				}
				const double city_capacity =
					capacity_by_city_product[city_index][building.output_product];
				const double capacity_share = city_capacity > 0.0
					? building.capacity * building.output_scale / city_capacity : 0.0;
				const double sold_prev = city.markets[building.output_product].sold_prev;
				const double labor = building.jobs > 0.0
					? building.workers / building.jobs : 1.0;
				const bool primary = building.recipe < 0 && building.input_product < 0;
				// PARTE E (D-116): energia servida limita a indústria do dia.
				// Sem energia a planta para (piso declarado); a produção cai
				// quase proporcionalmente ao racionamento.
				const bool industrial = !primary;
				const double power_factor = industrial
					? std::clamp(city.power_industry_factor, 0.0, 1.0) : 1.0;
				// D-092: custo unitário de trabalho/opex por unidade de
				// capacidade (mesma proxy de energia/capital da fase de preço) e
				// preço do produto, para a firma cortar produção quando a margem
				// fica negativa.
				const double unit_labor_opex = building.capacity > 0.0
					? (building.workers * building.wage + building.opex)
						/ building.capacity : 0.0;
				const double output_price =
					city.markets[building.output_product].price;
				// Extração: aptidão territorial × eficiência pela qualidade do minério
				// (1.0 quando não há jazida declarada) × produção relativa do
				// ESTÁGIO da mina (D-086: decai com a reserva do estágio, piso 5%).
				Deposit& deposit = city.deposits[building.output_product];
				const double suitability = primary
					? std::clamp(city.suitability[building.output_product], 0.0, 1.0)
						* extraction_efficiency(deposit)
						* mine_stage_factor(city, building)
					: 1.0;

				// Capacidade e vendas na mesma unidade (toneladas efetivas).
				const double effective_capacity = city_capacity * suitability;
				// Estoque disponível antes da produção de hoje (stock já inclui o que
				// foi produzido nesta passagem; supply é exatamente essa produção).
				const double stock_before = std::max(0.0,
					city.markets[building.output_product].stock
					- city.markets[building.output_product].supply);
				// Repõe metade da distância até o estoque-alvo, para cima ou para baixo.
				// Isso evita tanto a produção infinita (excedente acumulado) quanto a
				// parada brusca quando o estoque está acima do necessário.
				double target = sold_prev;
				if (sold_prev <= 0.0) {
					// Produtor novo (ou sem vendas) garante a PRIMEIRA oferta
					// observável do produto — e SÓ ela. Industrial
					// (receita/insumo) repõe a capacidade enquanto o estoque
					// estiver abaixo do ALVO de bootstrap: menos que metade do
					// mínimo que um dia de plena capacidade produz (o piso do
					// corte por margem, kProductionMarginFloor, multiplicado
					// pela energia servida). Como o dia de bootstrap produz ao
					// menos esse mínimo, a oferta acontece UMA vez; com estoque
					// acima do alvo e venda zero a produção é zero, como manda
					// o estoque-alvo das demais indústrias. Antes repunha a
					// capacidade cheia TODO dia (mesmo com estoque formado),
					// então a usina sem venda enchia o estoque pagando
					// folha/opex/insumo até fechar (diagnóstico t141). A
					// extração primária mantém o bootstrap legado: o minério
					// parado no chão não tem custo de insumo comprado e é o
					// que dá liquidez ao mercado.
					const double bootstrap_floor = 0.5 * kProductionMarginFloor
						* effective_capacity * capacity_share * labor
						* power_factor;
					target = industrial
						? (stock_before < bootstrap_floor ? effective_capacity : 0.0)
						: effective_capacity;
				} else {
					const double desired_stock = sold_prev * ws_.inventory_cover_days;
					target = sold_prev
						+ (desired_stock - stock_before) * ws_.inventory_recovery;
				}
				target = std::clamp(target, 0.0, effective_capacity);
				// D-125 (correção): a eficiência limita a CAPACIDADE da planta,
				// não o alvo de vendas. O alvo já vem em toneladas de mercado
				// (`sold_prev` + reposição do estoque-alvo); multiplicá-lo pela
				// eficiência encolhia a produção a cada dia (venda < alvo ×
				// eficiência), levando fazenda/mineira nova a zero antes de
				// repor a primeira venda — e à falência com folha cheia. O teto
				// por construção aplica a eficiência UMA vez (junto da aptidão/
				// estágio da extração), como a curva manda.
				const double building_capacity =
					building.capacity * building.output_scale;
				double desired_output = target * capacity_share * labor
					* power_factor;
				desired_output = std::min(desired_output,
					building_capacity * suitability * labor * power_factor
						* efficiency);
				double input_used = 0.0;
				double output = desired_output;
				if (building.recipe >= 0) {
					// Receita industrial (1..6 insumos, yield e subprodutos). A
					// alternativa de menor custo corrente entre as disponíveis na
					// cidade é escolhida a cada dia (determinístico; empate = primeira).
					const int chosen = pick_recipe(city, building.output_product);
					if (chosen >= 0) {
						building.recipe = chosen;
					}
					const Recipe& recipe =
						ws_.recipes[static_cast<std::size_t>(building.recipe)];
					// D-125: rendimentos EFETIVOS da receita (a refinaria no
					// modo diesel/querosene troca as frações do lote). Fora
					// do refino são os próprios valores do dado.
					const double out_yield = eff::main_yield(ws_, building, recipe);
					// D-092 — DEMANDA DA FIRMA POR MARGEM. Custo unitário
					// corrente (insumos + trabalho/opex); com o preço do
					// produto abaixo dele a produção cai proporcionalmente
					// (piso de 25%), e com ela o pedido de insumo. É o canal
					// que impede a demanda industrial de puxar ferro/carvão/
					// petróleo/gás sem fim quando o produto já não paga.
					double unit_input_cost = 0.0;
					if (out_yield > 0.0) {
						for (int slot = 0; slot < recipe.input_count; ++slot) {
							const int input = recipe.inputs[slot];
							if (input < 0 || input >= ws_.products) {
								continue;
							}
							unit_input_cost += recipe.input_qty[slot] / out_yield
								* city.markets[input].price;
						}
					}
					const double unit_cost = unit_input_cost + unit_labor_opex
						+ (industrial ? city.power_cost_per_value
							* base_price(building.output_product) : 0.0);
					// F0 (co-produção): a margem da firma soma o valor das
					// saídas extras. Sem isso a refinaria pareceria deficitária
					// (o custo do petróleo é dividido só pela gasolina) e
					// cortaria produção mesmo com diesel/querosene vendáveis.
					double unit_byproduct_revenue = 0.0;
					if (out_yield > 0.0) {
						for (int slot = 0; slot < recipe.byproduct_count; ++slot) {
							const int product = recipe.byproducts[slot];
							if (product < 0 || product >= ws_.products) {
								continue;
							}
							// Fallback único do co-produto sem mercado (t145): o
							// mesmo preço-base do `pick_recipe` e da IA.
							const Market& product_market = city.markets[product];
							const double product_price = product_market.price > 0.0
								? product_market.price : base_price(product);
							unit_byproduct_revenue += eff::byproduct_yield(ws_,
								building, recipe, slot) / out_yield * product_price;
						}
					}
					const double margin = output_price + unit_byproduct_revenue
						- unit_cost;
					double margin_scale = 1.0;
					if (unit_cost > 0.0 && margin < 0.0) {
						// Só corta com prejuízo PROFUNDO (> 25% do custo); em
						// -100% chega ao piso. Prejuízo raso não derruba
						// insumo essencial (combustível/material) de imediato.
						const double severity = std::clamp(
							(-margin / unit_cost - kProductionMarginCutStart)
								/ (1.0 - kProductionMarginCutStart),
							0.0, 1.0);
						margin_scale = 1.0
							- (1.0 - kProductionMarginFloor) * severity;
					}
					const double batch = out_yield > 0.0
						? desired_output * margin_scale / out_yield : 0.0;
					double scale = 1.0;
					for (int slot = 0; slot < recipe.input_count; ++slot) {
						const int input = recipe.inputs[slot];
						const double needed = recipe.input_qty[slot] * batch;
						Market& input_market = city.markets[input];
						input_market.desired += needed;
						// D-091: a usina só pressiona o preço do insumo quando tem
						// venda real no produto (sold_prev > 0). A usina ociosa/faminta
						// (bootstrap de plena capacidade, sold_prev <= 0) mantém o
						// `desired` cheio para a IA, mas não contribui para
						// `effective` — senão pede plena capacidade TODO dia e satura
						// o preço (docs/economy/price_formation.md).
						if (sold_prev > 0.0) {
							input_market.effective += needed;
						}
						if (needed > 1e-12) {
							scale = std::min(scale, input_market.stock / needed);
						}
					}
					scale = std::clamp(scale, 0.0, 1.0);
					for (int slot = 0; slot < recipe.input_count; ++slot) {
						const int input = recipe.inputs[slot];
						const double used = recipe.input_qty[slot] * batch * scale;
						Market& input_market = city.markets[input];
						input_market.stock = std::max(0.0, input_market.stock - used);
						input_market.demand += used;
						input_market.sold += used;
						city.revenue_by_product[input] += used * input_market.price;
						building.input_value += used * input_market.price;
						input_used += used;
					}
					output = batch * scale * out_yield;
					for (int slot = 0; slot < recipe.byproduct_count; ++slot) {
						const int product = recipe.byproducts[slot];
						const double amount = eff::byproduct_yield(ws_, building,
							recipe, slot) * batch * scale;
						if (amount <= 0.0) {
							continue;
						}
						city.markets[product].stock += amount;
						city.markets[product].supply += amount;
						produced_scratch[static_cast<std::size_t>(bi) * n_products
							+ static_cast<std::size_t>(product)] += amount;
						produced_today_scratch[static_cast<std::size_t>(bi) * n_products
							+ static_cast<std::size_t>(product)] += amount;
					}
				} else if (building.input_product >= 0) {
					const double input_price =
						city.markets[building.input_product].price;
					const double unit_cost = input_price
						* building.input_per_output + unit_labor_opex;
					const double margin = output_price - unit_cost;
					double margin_scale = 1.0;
					if (unit_cost > 0.0 && margin < 0.0) {
						// Só corta com prejuízo PROFUNDO (> 25% do custo); em
						// -100% chega ao piso. Prejuízo raso não derruba
						// insumo essencial (combustível/material) de imediato.
						const double severity = std::clamp(
							(-margin / unit_cost - kProductionMarginCutStart)
								/ (1.0 - kProductionMarginCutStart),
							0.0, 1.0);
						margin_scale = 1.0
							- (1.0 - kProductionMarginFloor) * severity;
					}
					const double needed_input = desired_output * margin_scale
						* building.input_per_output;
					Market& input_market = city.markets[building.input_product];
					input_market.desired += needed_input;
					// D-091: mesma regra da receita — pressão de preço só com venda
					// real (sold_prev > 0); a usina ociosa não infla o insumo.
					if (sold_prev > 0.0) {
						input_market.effective += needed_input;
					}
					input_used = std::min(needed_input, input_market.stock);
					output = building.input_per_output > 0.0
						? input_used / building.input_per_output : 0.0;

					input_market.stock -= input_used;
					input_market.demand += input_used;
					input_market.sold += input_used;
					// A receita do setor fornecedor inclui a transferência interna entre
					// construções (quando houver cadeia de refino), senão o valor
					// adicionado do fornecedor ficaria negativo.
					city.revenue_by_product[building.input_product] += input_used
						* input_market.price;
					building.input_value = input_used * input_market.price;
				} else if (deposit.tracked) {
					// D-086 (decisão do dono): o recurso NUNCA acaba. A extração NÃO
					// é limitada pelo saldo restante da jazida — a reserva é só
					// REFERÊNCIA de profundidade (escala da curva de custo do
					// aprofundamento). Continuamos debitando `remaining_kt`
					// (contabilidade: cai a 0 e fica lá) e somando `extracted_kt`
					// (o que passou da referência encarece o aprofundamento).
					const double extracted_kt = output / kTonsPerKiloton;
					deposit.remaining_kt = std::max(0.0, deposit.remaining_kt - extracted_kt);
					deposit.extracted_kt += extracted_kt;
					// D-086: a extração também consome o ESTÁGIO corrente. Quando
					// ele fecha, a produção fica no piso até alguém pagar o
					// aprofundamento (o estágio, não a jazida, controla o ritmo).
					if (building.stage_reserve_kt > 1e-12) {
						building.stage_mined_kt = std::min(building.stage_reserve_kt,
							building.stage_mined_kt + extracted_kt);
					}
				}

				city.markets[building.output_product].stock += output;
				city.markets[building.output_product].supply += output;
				// D-125: `utilization` publicado é produção ÷ capacidade
				// (inclui a eficiência); `last_utilization` (base da folha/opex
				// e do AI view) é a OPERAÇÃO sem eficiência — a planta emprega a
				// equipe mesmo aprendendo.
				building.utilization = building.capacity > 0.0
					? output / building.capacity : 0.0;
				building.last_utilization = building.capacity > 0.0
						&& efficiency > 1e-9
					? std::clamp(output / (building.capacity * efficiency), 0.0, 1.0)
					: building.utilization;
				produced_scratch[static_cast<std::size_t>(bi) * n_products
					+ static_cast<std::size_t>(building.output_product)] += output;
				produced_today_scratch[static_cast<std::size_t>(bi) * n_products
					+ static_cast<std::size_t>(building.output_product)] += output;
			}
		}
	};
	pool().parallel_for(ws_.cities.size(), process_city);

	// Redução serial em ordem crescente de índice de construção (a ordem do
	// loop serial original): soma as contribuições de hoje aos totais.
	for (std::size_t bi = 0; bi < n_buildings; ++bi) {
		for (int product = 0; product < ws_.products; ++product) {
			ws_.produced[product] += produced_scratch[bi * n_products
				+ static_cast<std::size_t>(product)];
			ws_.produced_today[product] += produced_today_scratch[bi * n_products
				+ static_cast<std::size_t>(product)];
		}
	}

	OGS_SIM_PHASE_LAP(kPhaseProduction);
}
void SimCore::phase_raw() {
	// 4c. Recursos brutos (minerais, soja, algodão e madeira): o resto do mundo
	// compra ao preço do mercado até um TETO de demanda externa por cidade
	// (external_demand), declarado pelo GeographyService como a participação da
	// nação no mercado mundial (reserva esgotada em ~25 anos — o mesmo horizonte
	// de max_rate_kt_day do deposits.json). Com o teto, a produção acima dele
	// vira estoque e a fase 8 derruba o preço. §5.4: só é exportado o bem com
	// `external_demand > 0` que NÃO é insumo de nenhuma construção ativa no
	// mundo (regra explícita, no lugar do antigo filtro "não é comida").
	std::vector<char> product_is_input(static_cast<std::size_t>(ws_.products), 0);
	for (const Building& building : ws_.buildings) {
		if (building.recipe >= 0
				&& building.recipe < static_cast<int>(ws_.recipes.size())) {
			const Recipe& recipe = ws_.recipes[static_cast<std::size_t>(building.recipe)];
			for (int slot = 0; slot < recipe.input_count; ++slot) {
				if (recipe.inputs[slot] >= 0 && recipe.inputs[slot] < ws_.products) {
					product_is_input[static_cast<std::size_t>(recipe.inputs[slot])] = 1;
				}
			}
		} else if (building.input_product >= 0 && building.input_product < ws_.products) {
			product_is_input[static_cast<std::size_t>(building.input_product)] = 1;
		}
	}

	// S16: a exportação bruta é independente por cidade. Os acumuladores
	// (`foreign_cash` = subtração; `consumed_total`/`consumed_today` = somas) são
	// reduzidos em ordem crescente de cidade E produto — a MESMA ordem do loop
	// serial original (que já percorria as cidades em ordem).
	const std::size_t n_cities = ws_.cities.size();
	const std::size_t n_products = static_cast<std::size_t>(ws_.products);
	std::vector<double> foreign_scratch(n_cities * n_products, 0.0);
	std::vector<double> consumed_total_scratch(n_cities * n_products, 0.0);
	std::vector<double> consumed_today_scratch(n_cities * n_products, 0.0);

	auto process_city = [this, &product_is_input, &foreign_scratch,
			&consumed_total_scratch, &consumed_today_scratch, n_products](
			std::size_t begin, std::size_t end) {
		for (std::size_t city_index = begin; city_index < end; ++city_index) {
			City& city = ws_.cities[city_index];
			for (int product = kFirstRawProduct; product < ws_.products; ++product) {
				if (food_calories(product) > 0.0) {
					continue;
				}
				if (product_is_input[static_cast<std::size_t>(product)] != 0) {
					continue;  // insumo de cadeia ativa: fica no mercado interno
				}
				const double ceiling = city.external_demand[product];
				if (ceiling <= 0.0) {
					continue;  // sem demanda externa declarada (sem depósito/aptidão)
				}
				Market& market = city.markets[product];
				market.desired += ceiling;
				// D-091: a demanda externa é financiada (o resto do mundo paga o
				// preço de mercado), então entra integral na pressão de preço.
				market.effective += ceiling;
				const double sold = std::min(market.stock, ceiling);
				if (sold <= 0.0) {
					continue;
				}
				const double revenue = sold * market.price;
				market.stock -= sold;
				market.demand += sold;
				market.sold += sold;
				city.firm_cash += revenue;
				city.revenue_by_product[product] += revenue;
				city.exported[product] += sold;
				city.export_value[product] += revenue;
				foreign_scratch[city_index * n_products
					+ static_cast<std::size_t>(product)] += revenue;
				consumed_total_scratch[city_index * n_products
					+ static_cast<std::size_t>(product)] += sold;
				consumed_today_scratch[city_index * n_products
					+ static_cast<std::size_t>(product)] += sold;
			}
		}
	};
	pool().parallel_for(ws_.cities.size(), process_city);

	// Redução serial em ordem crescente de cidade e de produto (a ordem do
	// loop serial original).
	for (std::size_t city_index = 0; city_index < n_cities; ++city_index) {
		for (int product = 0; product < ws_.products; ++product) {
			ws_.foreign_cash -= foreign_scratch[city_index * n_products
				+ static_cast<std::size_t>(product)];
			ws_.consumed_total[product] += consumed_total_scratch[city_index * n_products
				+ static_cast<std::size_t>(product)];
			ws_.consumed_today[product] += consumed_today_scratch[city_index * n_products
				+ static_cast<std::size_t>(product)];
		}
	}

	OGS_SIM_PHASE_LAP(kPhaseRaw);
}

// Multiplicador do opex pela profundidade da jazida (1.0 sem reserva).
double SimCore::depth_opex_factor(const Deposit& deposit) {
	if (!deposit.tracked) {
		return 1.0;
	}
	return std::min(kDepthOpexMaxFactor,
		1.0 + std::max(0.0, deposit.depth_m) / kDepthOpexScaleM);
}

// ---------------------------------------------------------------------------
// D-086 — ESTÁGIO DE MINA. A mina tem profundidades de operação; cada estágio
// trabalha uma fatia da reserva restante da jazida e a produção cai com ela.
// As três primeiras funções são estáticas (leem só a cidade/construção) para
// a fase de produção paralela usá-las sem estado.
// ---------------------------------------------------------------------------
bool SimCore::is_mine_building(const City& city, const Building& building) {
	if (building.recipe >= 0 || building.input_product >= 0) {
		return false;  // indústria: não é extração primária
	}
	if (building.output_product < 0 || building.output_product >= kMaxProducts) {
		return false;
	}
	// Só existe estágio onde existe jazida declarada (o mapa de recursos é a
	// fonte: produto sem depósito rastreado mantém a extração ilimitada legada).
	return city.deposits[building.output_product].tracked;
}
double SimCore::mine_stage_factor(const City& city, const Building& building) {
	if (!is_mine_building(city, building)) {
		return 1.0;
	}
	if (building.stage_reserve_kt <= 1e-12) {
		return kMineStageFloor;  // estágio esgotado: "quase nada"
	}
	const double remaining = std::max(0.0, building.stage_reserve_kt
		- building.stage_mined_kt);
	return std::clamp(remaining / building.stage_reserve_kt,
		kMineStageFloor, 1.0);
}
double SimCore::mine_deepen_capacity(const Building& building) {
	return std::max(0.0, building.capacity) * (kMineDeepenCapacityFactor - 1.0);
}
double SimCore::mine_deepen_cost(const City& city,
		const Building& building) const {
	if (!is_mine_building(city, building)) {
		return 0.0;
	}
	const Deposit& deposit = city.deposits[building.output_product];
	const int next_stage = building.stage + 1;
	// Referência de profundidade da jazida (decisão do dono): a reserva NUNCA
	// zera a produção nem bloqueia o aprofundamento — ela só escala o custo da
	// obra. `max(initial, remaining)` é defensivo contra reserva zerada.
	const double reference = std::max(deposit.initial_kt, deposit.remaining_kt);
	// Qualidade: minério pobre exige mover mais rocha por tonelada útil — o
	// MESMO fator da produção (0,5..1) entra invertido no custo (1..2).
	const double quality = std::max(1e-9, extraction_efficiency(deposit));
	// Quanto a extração já passou da referência, até o teto declarado: a mina
	// que já trabalhou muito além da referência fica mais cara, nunca infinita.
	const double extracted_ratio = std::clamp(
		deposit.extracted_kt / std::max(1e-9, reference),
		0.0, kMineDeepenExtractionCostCap);
	return mine_deepen_capacity(building) * kCapitalPerCapacity
		* depth_opex_factor(deposit) / quality
		* std::pow(kMineDeepenStageCostFactor, static_cast<double>(next_stage))
		* (1.0 + kMineDeepenExtractionCostFactor * extracted_ratio);
}
int SimCore::mine_deepen_days(const Building& building) {
	return kMineDeepenBaseDays
		+ kMineDeepenDaysPerStage * std::max(0, building.stage + 1);
}
void SimCore::set_mine_stage(City& city, Building& building, int stage) {
	building.stage = std::max(0, stage);
	const Deposit& deposit = city.deposits[building.output_product];
	// D-086 (decisão do dono): o estágio reivindica uma fatia da reserva de
	// REFERÊNCIA da jazida (não do saldo restante) — o recurso nunca acaba e a
	// extração não encerra a mina.
	const double reference = std::max(deposit.initial_kt, deposit.remaining_kt);
	building.stage_reserve_kt = std::max(1e-9,
		reference * kMineStageReserveShare);
	building.stage_mined_kt = 0.0;
}

double SimCore::effective_productivity(const City& city) const {
	// F7 calibração — TFP nacional. O cenário não traz valor adicionado por
	// trabalhador; o índice de renda real (PIB PPP per capita / média mundial,
	// `derived:gdp_ppp`) é a única medida de nível de produtividade já carregada
	// e entra como FATOR COMPLETO, normalizado para a média mundial = 1,0:
	// EUA ~3,5, DEU ~2,9, BRA ~0,9, IND ~0,46, NGA ~0,37. Aplicar só um prêmio
	// `max(1, índice)` deixava a Nigéria produzindo como a média mundial e
	// invertia a distribuição (menos gente no basic do que a renda real
	// permite). O limite [0,25, 4,0] contém os extremos reais (QAT ~5,0) sem
	// inverter a ordem entre as nações jogáveis; o piso de subsistência da
	// âncora salarial (ver update_wages) impede que o fator < 1 derrube o
	// trabalhador abaixo da cesta alimentar.
	if (city.productivity_index > 0.0) {
		return std::clamp(city.productivity_index, 0.25, 4.0);
	}
	return std::clamp(city.external_income_index, 0.25, 4.0);
}
} // namespace ogs
