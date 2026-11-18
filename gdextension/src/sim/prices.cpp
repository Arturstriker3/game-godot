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
namespace {
// Proteção NUMÉRICA (D-091): o preço precisa ser finito e > 0. Sem teto
// econômico, a única defesa contra NaN/inf/overflow do cálculo de escassez é
// manter o preço anterior — nunca se publica preço inválido nem se deixa o
// mercado divergir. Não é uma banda econômica: vale para QUALQUER nível de
// preço, só descarta resultados não representáveis.
double price_guard(double candidate, double previous) {
	if (std::isfinite(candidate) && candidate > 0.0) {
		return candidate;
	}
	return std::isfinite(previous) && previous > 0.0 ? previous : 1.0;
}
} // namespace

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
					//
					// D-091: o CIF entra CRU (sem a banda fixa [0,5×, 3×] do base) — o
					// teto é o próprio preço de importação, que é dado real. Só a
					// guarda numérica se aplica.
					if (city.import_price[product] <= 0.0) {
						continue;
					}
					const double target = price_guard(
						city.import_price[product], market.price);
					const double capped = std::min(market.price, target);
					const double max_step = base * 0.05;
					double next_import = market.price
						+ (capped - market.price) * kPriceSmoothing;
					next_import = price_guard(next_import, market.price);
					next_import = std::clamp(next_import,
						market.price - max_step, market.price + max_step);
					market.price = price_guard(next_import, market.price);
					continue;
				}
				// Pressão usa a demanda EFETIVA (D-091): o que os compradores
				// realmente tentam comprar dado caixa/insumos/uso, acumulado na
				// fonte em `Market::effective`. O desejo sem renda (`latent`) e o
				// bootstrap de plena capacidade da usina ociosa continuam no
				// `desired` (sinal da IA), mas NÃO pressionam o preço.
				const double effective = std::max(0.0, market.effective);
				const double scarcity = effective
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
				// D-091: SEM teto fixo. O alvo é `base × scarcity`, limitado só pelo
				// PISO DE CUSTO (econômico, não banda fixa): o preço nunca fica
				// abaixo do custo de produção + 5% de margem declarada. A variação
				// diária (kPriceSmoothing + limite de ±5% do base) permanece como
				// amortecimento, e a guarda numérica cobre NaN/inf/overflow.
				const double target = std::max(
					base * scarcity,
					cost_floor * 1.05);
				// Limite de variação diária evita oscilações explosivas.
				const double max_step = base * 0.05;
				double next = market.price
					+ (target - market.price) * kPriceSmoothing;
				next = price_guard(next, market.price);
				next = std::clamp(next, market.price - max_step,
					market.price + max_step);
				market.price = price_guard(next, market.price);
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
	// de preço (importação, custo e escassez). D-091: SEM banda econômica fixa
	// — o preço varia livremente; o único limite é a variação diária de ±5% do
	// base ao redor do preço de abertura, mais a guarda numérica (finito e > 0).
	// S16: o clamp é independente por cidade×produto (escreve só no próprio
	// mercado; lê só o preço de abertura imutável do dia) — sem acumulador
	// global, paralelizável sem redução e bit-idêntico ao serial.
	auto process_city = [this](std::size_t begin, std::size_t end) {
		for (std::size_t city_index = begin; city_index < end; ++city_index) {
			for (int product = 0; product < ws_.products; ++product) {
				Market& market = ws_.cities[city_index].markets[product];
				const double base = base_price(product);
				const double step = base * 0.05;
				// Sem banda, um choque extremo pode deixar o preço de abertura
				// NaN/inf/≤0: a guarda usa o preço corrente como âncora, então
				// `lower <= upper` sempre vale (step > 0).
				const double open = price_guard(
					open_prices()[city_index][product], market.price);
				const double lower = open - step;
				const double upper = open + step;
				market.price = std::clamp(market.price, lower, upper);
				market.price = price_guard(market.price, open);
			}
		}
	};
	pool().parallel_for(ws_.cities.size(), process_city);

	OGS_SIM_PHASE_LAP(kPhaseClamp);
}
} // namespace ogs
