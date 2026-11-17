// Módulo de domínio: preços — regra de preço, clamp e escassez
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
void SimCore::phase_prices() {
	// 8. Preços reagem a estoque, oferta, demanda e custo de produção.
	// S16: a regra de preço é independente por cidade (escreve só nos mercados
	// da própria cidade; lê só dados globais imutáveis no dia) — sem acumulador
	// global, paralelizável sem redução e bit-idêntica ao serial.
	auto process_city = [this](std::size_t begin, std::size_t end) {
		for (std::size_t city_index = begin; city_index < end; ++city_index) {
			City& city = ws_.cities[city_index];
			const std::vector<int>& local_buildings =
				ws_.buildings_by_city[city_index];
			std::vector<char> has_producer(static_cast<std::size_t>(ws_.products), 0);
			for (const int bi : local_buildings) {
				const Building& building =
					ws_.buildings[static_cast<std::size_t>(bi)];
				if (building.output_product >= 0
						&& building.output_product < ws_.products) {
					has_producer[building.output_product] = true;
				}
			}
			for (int product = 0; product < ws_.products; ++product) {
				const double base = base_price(product);
				Market& market = city.markets[product];
				if (!has_producer[product]) {
					// §2.7: cidade sem produtor tem o preço disciplinado pelo CIF da
					// importação (na âncora), com a mesma suavização e o mesmo
					// limite diário dos produtores. Sem importação registrada, o preço
					// permanece (não se inventa cotação).
					//
					// A paridade age como TETO de concorrência: sem produtor local, o
					// consumidor pode importar, então o preço não fica ACIMA do CIF —
					// mas a importação não puxa o preço para cima. Motivo (calibração
					// F5, `economy_world_test` 365 d): puxar até o CIF levava EGY/MRT/
					// SDN ao teto de 3× o preço-base e derrubava a comida (EGY 1,000 →
					// 0,594 sem moedas); o frete de comida já é um serviço do comprador
					// e o piso calibrado da cesta (F4) não absorve 3× base.
					if (city.import_price[product] <= 0.0) {
						continue;
					}
					const double target = std::clamp(city.import_price[product],
						base * 0.5, base * 3.0);
					const double capped = std::min(market.price, target);
					const double max_step = base * 0.05;
					double next_import = market.price
						+ (capped - market.price) * kPriceSmoothing;
					next_import = std::clamp(next_import,
						market.price - max_step, market.price + max_step);
					market.price = std::clamp(next_import,
						base * 0.5, base * 3.0);
					continue;
				}
				// Pressão usa a demanda desejada FINANCIADA: compras não atendidas
				// por falta de estoque continuam pressionando o preço, mas o desejo
				// sem renda (`latent`, sinal da IA) não infla o preço de um bem que
				// ninguém no mercado pode pagar.
				const double financed_demand = std::max(0.0,
					market.desired - market.latent);
				const double scarcity = financed_demand
					/ std::max(market.stock + market.supply, 0.001);

				double kind_capacity = 0.0;
				double kind_labor_opex = 0.0;
				for (const int bi : local_buildings) {
					const Building& building =
						ws_.buildings[static_cast<std::size_t>(bi)];
					if (building.output_product == product) {
						kind_capacity += building.capacity;
						kind_labor_opex += building.workers * building.wage
							+ building.opex;
					}
				}
				const double unit_labor_opex = kind_capacity > 0.0
					? kind_labor_opex / kind_capacity : 0.0;

				// Piso de custo: insumos da receita + mão de obra/opex por unidade
				// de capacidade. Preços, salário e opex estão todos na âncora (F5),
				// então não há conversão de moeda aqui.
				double cost_floor = base;
				for (const int bi : local_buildings) {
					const Building& building =
						ws_.buildings[static_cast<std::size_t>(bi)];
					if (building.output_product != product) {
						continue;
					}
					if (building.recipe >= 0
							&& building.recipe < static_cast<int>(ws_.recipes.size())) {
						const Recipe& recipe =
							ws_.recipes[static_cast<std::size_t>(building.recipe)];
						double input_cost = 0.0;
						for (int slot = 0; slot < recipe.input_count; ++slot) {
							input_cost += recipe.input_qty[slot] / recipe.yield
								* city.markets[recipe.inputs[slot]].price;
						}
						cost_floor = input_cost + unit_labor_opex;
						break;
					}
					if (building.input_product >= 0) {
						cost_floor = city.markets[building.input_product].price
							* building.input_per_output + unit_labor_opex;
						break;
					}
				}
				const double target = std::max(
					base * (1.0 + std::clamp(scarcity - 1.0, -0.5, 2.0)),
					cost_floor * 1.05);
				// Limite de variação diária evita oscilações explosivas.
				const double max_step = base * 0.05;
				double next = market.price
					+ (target - market.price) * kPriceSmoothing;
				next = std::clamp(next, market.price - max_step,
					market.price + max_step);
				market.price = std::clamp(next, base * 0.5, base * 3.0);
			}

			// Registro de escassez e excesso para diagnóstico e visualização.
			for (int product = 0; product < ws_.products; ++product) {
				Market& market = city.markets[product];
				market.shortage = std::max(0.0, market.desired - market.demand);
				market.surplus = std::max(0.0, market.stock
					- std::max(market.desired, market.sold) * 3.0);
			}
		}
	};
	pool().parallel_for(ws_.cities.size(), process_city);

	OGS_SIM_PHASE_LAP(kPhasePrices);
}
void SimCore::phase_clamp() {
	// 9. Limite diário único por mercado, aplicado depois de todas as regras
	// de preço (importação, custo e escassez). O piso e o teto continuam
	// valendo mesmo que o preço de abertura esteja fora da faixa.
	// S16: o clamp é independente por cidade×produto (escreve só no próprio
	// mercado; lê só o preço de abertura imutável do dia) — sem acumulador
	// global, paralelizável sem redução e bit-idêntico ao serial.
	auto process_city = [this](std::size_t begin, std::size_t end) {
		for (std::size_t city_index = begin; city_index < end; ++city_index) {
			for (int product = 0; product < ws_.products; ++product) {
				Market& market = ws_.cities[city_index].markets[product];
				const double base = base_price(product);
				const double step = base * 0.05;
				const double floor = base * 0.5;
				const double ceiling = base * 3.0;
				// O preço de abertura pode ter saído da banda por um choque
				// (price_shock). Clampa primeiro para dentro da banda, senão
				// `open - step` fica acima do teto e `lower > upper` desfaz o clamp
				// da fase 8, prendendo o preço fora da faixa (bug de preço pós-choque).
				const double open = std::clamp(
					open_prices()[city_index][product], floor, ceiling);
				const double lower = std::max(floor, open - step);
				const double upper = std::min(ceiling, open + step);
				market.price = std::clamp(market.price, lower, upper);
			}
		}
	};
	pool().parallel_for(ws_.cities.size(), process_city);

	OGS_SIM_PHASE_LAP(kPhaseClamp);
}
} // namespace ogs
