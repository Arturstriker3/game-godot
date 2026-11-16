// Módulo de domínio: produção — capacidade, extração, receitas e matérias-primas
// (S10 do docs/economy/core_refactor_plan.md).
// SimCore:: métodos movidos de sim_core.cpp — realocação byte-idêntica.
#include "sim_core.h"

#include "profiling.h"
#include "sim_constants.h"
#include "sim_labor.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace ogs {
// Eficiência da extração (0.5..1). Sem reserva declarada não há qualidade
// conhecida: eficiência 1 (comportamento legado).
double SimCore::demo_extraction_efficiency(const DemoDeposit& deposit) {
	if (!deposit.tracked) {
		return 1.0;
	}
	return kDemoQualityEfficiencyFloor
		+ (1.0 - kDemoQualityEfficiencyFloor) * std::clamp(deposit.quality, 0.0, 1.0);
}
double SimCore::demo_base_price(int product) const {
	if (product >= 0 && product < ws_.products) {
		return ws_.base_prices[product];
	}
	return 40.0;
}
int SimCore::demo_food_product(int slot) const {
	return (slot >= 0 && slot < static_cast<int>(ws_.food_products.size()))
		? ws_.food_products[static_cast<std::size_t>(slot)] : -1;
}
double SimCore::demo_food_calories(int product) const {
	for (std::size_t slot = 0; slot < ws_.food_products.size(); ++slot) {
		if (ws_.food_products[slot] == product) {
			return ws_.food_factors[slot];
		}
	}
	return 0.0;
}
int SimCore::demo_pick_recipe(const DemoCity& city, int output_product) const {
	int best = -1;
	double best_cost = 0.0;
	for (std::size_t index = 0; index < ws_.recipes.size(); ++index) {
		const DemoRecipe& recipe = ws_.recipes[index];
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
			const DemoMarket& market = city.markets[input];
			// Insumo só conta como disponível quando há estoque físico: uma
			// receita sem insumo produziria zero e venceria a comparação de custo.
			if (market.stock <= 1e-9) {
				available = false;
				break;
			}
			const double unit_price = market.price > 0.0
				? market.price : demo_base_price(input);
			cost += qty * unit_price / recipe.yield;
		}
		if (!available) {
			continue;
		}
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
	std::vector<std::array<double, kDemoMaxProducts>> capacity_by_city_product(
		ws_.cities.size());
	for (auto& slots : capacity_by_city_product) {
		slots.fill(0.0);
	}
	for (const DemoBuilding& building : ws_.buildings) {
		if (building.output_product < 0 || building.output_product >= ws_.products) {
			continue;
		}
		const auto found = ws_.city_lookup.find(building.city);
		if (found == ws_.city_lookup.end()) {
			continue;
		}
		capacity_by_city_product[static_cast<std::size_t>(found->second)]
			[building.output_product] += building.capacity * building.output_scale;
	}

	for (DemoBuilding& building : ws_.buildings) {
		DemoCity* city = find_demo_city(building.city);
		if (city == nullptr || building.output_product < 0
				|| building.output_product >= ws_.products) {
			continue;
		}

		const std::size_t city_slot =
			static_cast<std::size_t>(city - ws_.cities.data());
		const double city_capacity =
			capacity_by_city_product[city_slot][building.output_product];
		const double capacity_share = city_capacity > 0.0
			? building.capacity * building.output_scale / city_capacity : 0.0;
		const double sold_prev = city->markets[building.output_product].sold_prev;
		const double labor = building.jobs > 0.0
			? building.workers / building.jobs : 1.0;
		const bool primary = building.recipe < 0 && building.input_product < 0;
		// Extração: aptidão territorial × eficiência pela qualidade do minério
		// (1.0 quando não há jazida declarada).
		DemoDeposit& deposit = city->deposits[building.output_product];
		const double suitability = primary
			? std::clamp(city->suitability[building.output_product], 0.0, 1.0)
				* demo_extraction_efficiency(deposit)
			: 1.0;

		// Capacidade e vendas na mesma unidade (toneladas efetivas).
		const double effective_capacity = city_capacity * suitability;
		// Estoque disponível antes da produção de hoje (stock já inclui o que
		// foi produzido nesta passagem; supply é exatamente essa produção).
		const double stock_before = std::max(0.0,
			city->markets[building.output_product].stock
			- city->markets[building.output_product].supply);
		// Repõe metade da distância até o estoque-alvo, para cima ou para baixo.
		// Isso evita tanto a produção infinita (excedente acumulado) quanto a
		// parada brusca quando o estoque está acima do necessário.
		double target = sold_prev;
		if (sold_prev <= 0.0) {
			// Produtor novo (ou sem vendas) enche o mercado com a capacidade
			// EFETIVA (output_scale), o que dá ao produto a primeira oferta
			// observável — é o que permite o comércio/consumo começar.
			target = effective_capacity;
		} else {
			const double desired_stock = sold_prev * ws_.inventory_cover_days;
			target = sold_prev
				+ (desired_stock - stock_before) * ws_.inventory_recovery;
		}
		target = std::clamp(target, 0.0, effective_capacity);
		const double desired_output = target * capacity_share * labor;
		double input_used = 0.0;
		double output = desired_output;
		if (building.recipe >= 0) {
			// Receita industrial (1..6 insumos, yield e subprodutos). A
			// alternativa de menor custo corrente entre as disponíveis na
			// cidade é escolhida a cada dia (determinístico; empate = primeira).
			const int chosen = demo_pick_recipe(*city, building.output_product);
			if (chosen >= 0) {
				building.recipe = chosen;
			}
			const DemoRecipe& recipe =
				ws_.recipes[static_cast<std::size_t>(building.recipe)];
			const double batch = recipe.yield > 0.0
				? desired_output / recipe.yield : 0.0;
			double scale = 1.0;
			for (int slot = 0; slot < recipe.input_count; ++slot) {
				const int input = recipe.inputs[slot];
				const double needed = recipe.input_qty[slot] * batch;
				DemoMarket& input_market = city->markets[input];
				input_market.desired += needed;
				if (needed > 1e-12) {
					scale = std::min(scale, input_market.stock / needed);
				}
			}
			scale = std::clamp(scale, 0.0, 1.0);
			for (int slot = 0; slot < recipe.input_count; ++slot) {
				const int input = recipe.inputs[slot];
				const double used = recipe.input_qty[slot] * batch * scale;
				DemoMarket& input_market = city->markets[input];
				input_market.stock = std::max(0.0, input_market.stock - used);
				input_market.demand += used;
				input_market.sold += used;
				city->revenue_by_product[input] += used * input_market.price;
				building.input_value += used * input_market.price;
				input_used += used;
			}
			output = batch * scale * recipe.yield;
			for (int slot = 0; slot < recipe.byproduct_count; ++slot) {
				const int product = recipe.byproducts[slot];
				const double amount = recipe.byproduct_qty[slot] * batch * scale;
				if (amount <= 0.0) {
					continue;
				}
				city->markets[product].stock += amount;
				city->markets[product].supply += amount;
				ws_.produced[product] += amount;
				ws_.produced_today[product] += amount;
			}
		} else if (building.input_product >= 0) {
			const double needed_input = desired_output * building.input_per_output;
			DemoMarket& input_market = city->markets[building.input_product];
			input_market.desired += needed_input;
			input_used = std::min(needed_input, input_market.stock);
			output = building.input_per_output > 0.0
				? input_used / building.input_per_output : 0.0;

			input_market.stock -= input_used;
			input_market.demand += input_used;
			input_market.sold += input_used;
			// A receita do setor fornecedor inclui a transferência interna entre
			// construções (quando houver cadeia de refino), senão o valor
			// adicionado do fornecedor ficaria negativo.
			city->revenue_by_product[building.input_product] += input_used
				* input_market.price;
			building.input_value = input_used * input_market.price;
		} else if (deposit.tracked) {
			// Esgotamento: a mina só extrai o que resta da reserva da jazida
			// (compartilhada pelas minas da cidade) e a debita na mesma medida.
			const double available_tons = deposit.remaining_kt * kDemoTonsPerKiloton;
			output = std::clamp(output, 0.0, available_tons);
			const double extracted_kt = output / kDemoTonsPerKiloton;
			deposit.remaining_kt = std::max(0.0, deposit.remaining_kt - extracted_kt);
			deposit.extracted_kt += extracted_kt;
			// Reserva zerada: a mina encerra no próximo dia SEM virar falência.
			if (deposit.remaining_kt <= 1e-9) {
				building.depleted = true;
			}
		}

		city->markets[building.output_product].stock += output;
		city->markets[building.output_product].supply += output;
		building.utilization = building.capacity > 0.0
			? output / building.capacity : 0.0;
		building.last_utilization = building.utilization;
		ws_.produced[building.output_product] += output;
		ws_.produced_today[building.output_product] += output;
	}

	OGS_SIM_PHASE_LAP(kDemoPhaseProduction);
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
	for (const DemoBuilding& building : ws_.buildings) {
		if (building.recipe >= 0
				&& building.recipe < static_cast<int>(ws_.recipes.size())) {
			const DemoRecipe& recipe = ws_.recipes[static_cast<std::size_t>(building.recipe)];
			for (int slot = 0; slot < recipe.input_count; ++slot) {
				if (recipe.inputs[slot] >= 0 && recipe.inputs[slot] < ws_.products) {
					product_is_input[static_cast<std::size_t>(recipe.inputs[slot])] = 1;
				}
			}
		} else if (building.input_product >= 0 && building.input_product < ws_.products) {
			product_is_input[static_cast<std::size_t>(building.input_product)] = 1;
		}
	}
	for (DemoCity& city : ws_.cities) {
		for (int product = kDemoFirstRawProduct; product < ws_.products; ++product) {
			if (demo_food_calories(product) > 0.0) {
				continue;
			}
			if (product_is_input[static_cast<std::size_t>(product)] != 0) {
				continue;  // insumo de cadeia ativa: fica no mercado interno
			}
			const double ceiling = city.external_demand[product];
			if (ceiling <= 0.0) {
				continue;  // sem demanda externa declarada (sem depósito/aptidão)
			}
			DemoMarket& market = city.markets[product];
			market.desired += ceiling;
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
			ws_.foreign_cash -= revenue;
			ws_.consumed_total[product] += sold;
			ws_.consumed_today[product] += sold;
		}
	}

	OGS_SIM_PHASE_LAP(kDemoPhaseRaw);
}

// Multiplicador do opex pela profundidade da jazida (1.0 sem reserva).
double SimCore::demo_depth_opex_factor(const DemoDeposit& deposit) {
	if (!deposit.tracked) {
		return 1.0;
	}
	return std::min(kDemoDepthOpexMaxFactor,
		1.0 + std::max(0.0, deposit.depth_m) / kDemoDepthOpexScaleM);
}
double SimCore::demo_effective_productivity(const DemoCity& city) const {
	// F7 calibração — TFP nacional. O cenário não traz valor adicionado por
	// trabalhador; o índice de renda real (PIB PPP per capita / média mundial,
	// `derived:gdp_ppp`) é a única medida de nível de produtividade já carregada
	// e entra como FATOR COMPLETO, normalizado para a média mundial = 1,0:
	// EUA ~3,5, DEU ~2,9, BRA ~0,9, IND ~0,46, NGA ~0,37. Aplicar só um prêmio
	// `max(1, índice)` deixava a Nigéria produzindo como a média mundial e
	// invertia a distribuição (menos gente no basic do que a renda real
	// permite). O limite [0,25, 4,0] contém os extremos reais (QAT ~5,0) sem
	// inverter a ordem entre as nações jogáveis; o piso de subsistência da
	// âncora salarial (ver demo_update_wages) impede que o fator < 1 derrube o
	// trabalhador abaixo da cesta alimentar.
	if (city.productivity_index > 0.0) {
		return std::clamp(city.productivity_index, 0.25, 4.0);
	}
	return std::clamp(city.external_income_index, 0.25, 4.0);
}
} // namespace ogs
