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

double SimCore::price_demand_scale(double price, double base,
		double elasticity) {
	// D-092: demanda sensível a preço. Até o preço-base a necessidade é
	// integral; acima dele a quantidade cai com a elasticidade da classe
	// (básico pouco, luxo muito), com piso para não zerar a cesta.
	if (!(price > 0.0) || !(base > 0.0) || !(elasticity > 0.0)) {
		return 1.0;
	}
	const double ratio = price / base;
	if (ratio <= 1.0) {
		return 1.0;
	}
	const double scale = std::pow(ratio, -elasticity);
	return std::clamp(scale, kPriceElasticityQuantityFloor, 1.0);
}

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
				// Passo diário: o amortecimento antigo (5% do base) ou 6% do
				// preço de abertura, o que for maior (D-092). Usar o MESMO
				// passo no `phase_clamp` garante que o dia inteiro não some
				// dois passos.
				const double open = price_guard(
					open_prices()[city_index][product], market.price);
				const double max_step = std::max(
					base * 0.05, open * kPriceMaxDailyMove);
				if (!has_producer[product]) {
					// §2.7: cidade sem produtor tem o preço disciplinado pelo CIF da
					// importação (na âncora), com a mesma suavização e o mesmo
					// limite diário dos produtores. Sem importação registrada, o preço
					// permanece (não se inventa cotação).
					//
					// HISTÓRICO (F5, antes da D-109): o CIF era apenas TETO — a
					// importação puxava o preço só para baixo. Motivo da calibração
					// (F5, `economy_world_test` 365 d): puxar até o CIF levava EGY/
					// MRT/SDN ao teto de 3× o preço-base e derrubava a comida (EGY
					// 1,000 → 0,594 sem moedas); o frete de comida já é um serviço do
					// comprador e o piso calibrado da cesta (F4) não absorve 3× base.
					// A D-109 troca o teto pelo ALVO nos dois sentidos e mantém a
					// suavização e o passo diário da D-092 como amortecimento; D-091:
					// o CIF entra CRU (sem a banda fixa [0,5×, 3×] do base) — só a
					// guarda numérica se aplica.
					// C2c-P1 (D-109): o CIF é o ALVO de concorrência nos DOIS
					// sentidos. O câmbio que encarece a origem SOBE o preço e o
					// que barateia DESCE, com a mesma suavização e o mesmo limite
					// diário dos produtores (D-092) — o teto antigo fazia o
					// repasse só para baixo e compunha deflação de importação em
					// cadeia (C2c §4.2). Sem teto artificial: o preço converge
					// para o custo CIF, como manda a formação por custo (D-091).
					//
					// A cotação só vale se o comércio do DIA a renovou
					// (`imported` > 0). Sem cotação de hoje não se inventa preço:
					// o último CIF conhecido não é cotação vigente.
					const double cif = city.import_price[product];
					if (cif <= 0.0 || !(city.imported[product] > 0.0)) {
						continue;
					}
					const double target = price_guard(cif, market.price);
					double next_import = market.price
						+ (target - market.price) * kPriceSmoothing;
					next_import = price_guard(next_import, market.price);
					next_import = std::clamp(next_import,
						open - max_step, open + max_step);
					market.price = price_guard(next_import, market.price);
					continue;
				}
				// Demanda EFETIVA (D-091): o que os compradores realmente tentam
				// comprar ao preço corrente, acumulado na fonte em
				// `Market::effective`. O desejo sem renda (`latent`) e o bootstrap
				// de plena capacidade da usina ociosa continuam no `desired`
				// (sinal da IA), mas NÃO pressionam o preço.
				const double effective = std::max(0.0, market.effective);
				const double available = std::max(
					market.stock + market.supply, 0.001);
				// Desequilíbrio relativo: > 1 falta, < 1 sobra. É a variável
				// que entra no MARKUP — não num alvo `base × escassez` que
				// cresce sem fim.
				const double imbalance = effective / available;

				double kind_capacity = 0.0;
				double kind_labor_opex = 0.0;
				bool kind_industrial = false;
				for (const int bi : local_buildings) {
					const Building& building =
						ws_.buildings[static_cast<std::size_t>(bi)];
					if (building.output_product == product) {
						kind_capacity += building.capacity;
						kind_labor_opex += building.workers * building.wage
							+ building.opex;
						if (building.recipe >= 0
								|| building.input_product >= 0) {
							kind_industrial = true;
						}
					}
				}
				const double unit_labor_opex = kind_capacity > 0.0
					? kind_labor_opex / kind_capacity : 0.0;

				// CUSTO UNITÁRIO (D-092): insumos da receita por unidade
				// produzida + trabalho/opex por unidade de capacidade. O opex é
				// a proxy declarada de energia/capital (não há preço de energia
				// no núcleo antes da Parte E). Preço de insumo já na âncora.
				double unit_input_cost = 0.0;
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
						if (recipe.yield > 0.0) {
							for (int slot = 0; slot < recipe.input_count; ++slot) {
								const int input = recipe.inputs[slot];
								if (input < 0 || input >= ws_.products) {
									continue;
								}
								unit_input_cost += recipe.input_qty[slot] / recipe.yield
									* city.markets[input].price;
							}
						}
						break;
					}
					if (building.input_product >= 0
							&& building.input_product < ws_.products) {
						unit_input_cost = city.markets[building.input_product].price
							* building.input_per_output;
						break;
					}
				}
				const double unit_cost = unit_labor_opex + unit_input_cost
					+ (kind_industrial
						? city.power_cost_per_value * base : 0.0);
				// PISO DE CUSTO com âncora leve no preço real inicial: o preço
				// nunca fica abaixo do custo unitário nem abaixo do base do
				// catálogo (o preço do dia 0). É um PISO, não um teto.
				const double cost_floor = std::max(unit_cost, base);

				// MARKUP da firma: sobe com a falta relativa (log — satura, não
				// explode) e com a ocupação da capacidade; cai quando sobra
				// estoque/oferta. Cap em `kPriceMarkupMax`, que é teto da MARGEM
				// (modelo), não banda de preço.
				const double excess = std::max(0.0, imbalance - 1.0);
				const double occupancy = kind_capacity > 0.0
					? std::clamp(market.supply / kind_capacity, 0.0, 1.0) : 0.0;
				double markup = kPriceMarkupBase
					+ kPriceMarkupGain * std::log1p(excess)
					+ kPriceMarkupOccupancyGain * occupancy
						* std::min(1.0, excess);
				markup = std::clamp(markup, 0.0, kPriceMarkupMax);
				double target = cost_floor * (1.0 + markup);
				// C2c-P1 (D-109): a concorrência importada é o TETO do alvo na
				// cidade com produtor também — regra de economia aberta. Se o CIF
				// do dia é mais barato que custo+markup, o preço converge para o
				// CIF; o piso passa a ser o CUSTO VARIÁVEL (`unit_cost`), não o
				// base do catálogo: a âncora leve do dia 0 não pode impedir que
				// importação mais barata apareça no preço interno. Importação mais
				// cara NÃO empurra o preço para cima: nesse caso quem forma o preço
				// é o produtor local (o repasse de alta chega pelo custo do insumo
				// importado). Sem cotação do dia, a regra de custo+markup vale.
				const double cif = city.import_price[product];
				if (cif > 0.0 && city.imported[product] > 0.0 && cif < target) {
					target = std::max(cif, unit_cost);
				}

				// A variação diária permanece como amortecimento (5% do base
				// ou 6% do preço de abertura), e a guarda numérica cobre
				// NaN/inf/overflow. O alvo é convergente: com a demanda
				// elástica e a oferta respondendo, `imbalance` volta perto de 1
				// e o markup volta ao normal.
				double next = market.price
					+ (target - market.price) * kPriceSmoothing;
				next = price_guard(next, market.price);
				next = std::clamp(next, open - max_step, open + max_step);
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
	// — o preço varia livremente. D-092: o limite é 5% do base ou 6% do preço
	// de abertura (o mesmo passo do `phase_prices`), mais a guarda numérica
	// (finito e > 0). S16: o clamp é independente por cidade×produto (escreve
	// só no próprio mercado; lê só o preço de abertura imutável do dia) — sem
	// acumulador global, paralelizável sem redução e bit-idêntico ao serial.
	auto process_city = [this](std::size_t begin, std::size_t end) {
		for (std::size_t city_index = begin; city_index < end; ++city_index) {
			for (int product = 0; product < ws_.products; ++product) {
				Market& market = ws_.cities[city_index].markets[product];
				const double base = base_price(product);
				// Sem banda, um choque extremo pode deixar o preço de abertura
				// NaN/inf/≤0: a guarda usa o preço corrente como âncora, então
				// `lower <= upper` sempre vale (step > 0).
				const double open = price_guard(
					open_prices()[city_index][product], market.price);
				const double step = std::max(
					base * 0.05, open * kPriceMaxDailyMove);
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
