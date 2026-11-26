// H1f — sim/power/retail.cpp: liquidação do dia (tarifa de varejo paga,
// remuneração do atacado, combustível, O&M/salários e lucro do dono).
// Blocos movidos VERBATIM de `sim/power.cpp`; nenhuma regra, ordem ou número
// mudou.
#include "power_internal.h"

#include "../sim_core.h"
#include "../world_state.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace ogs {

namespace {

// Guarda de faixa do índice de cidade da usina antes de indexar `cities`.
// `assert` em debug (estado impossível, denuncia o produtor do dado) e skip
// determinístico em release: um índice inválido não pode ler memória fora do
// vetor nem mudar a ordem do fecho.
bool plant_city_in_range(const WorldState& ws, const PowerPlant& plant) {
	const bool ok = plant.city_index >= 0
		&& plant.city_index < static_cast<int>(ws.cities.size());
	assert(ok);
	return ok;
}

} // namespace

using namespace power_detail;

// ---------------------------------------------------------------------------
// Liquidação financeira do dia (depois da contabilidade): consumidor paga ao
// gerador; combustível sai do mercado; O&M/salários e lucro do dono. Soma
// zero sempre — o caixa agregado do setor (`power_cash`) entra em money_total.
// ---------------------------------------------------------------------------
void SimCore::phase_power_settle() {
	if (!ws_.power_ready || ws_.power_cities.size() != ws_.cities.size()) {
		return;
	}
	WorldState& ws = ws_;
	const PowerParams& params = ws.power_params;
	double pool = ws.power_cash;
	// 1. Consumidor paga a TARIFA de varejo: famílias, firmas, serviços e
	// governo (E7/D-120). O gerador recebe o custo médio de atacado; a
	// diferença (rede + tributos + repasse de escassez) fica com a
	// distribuidora/Estado pelo caixa que já existe. Só o servido é cobrado; o
	// que o caixa não cobre NÃO vira dívida (perda do setor). Soma zero: o
	// dinheiro que sai do consumidor entra no caixa do setor ou no orçamento
	// do Estado, nunca some nem é criado.
	for (std::size_t index = 0; index < ws.cities.size(); ++index) {
		City& city = ws.cities[index];
		const PowerCityState& state = ws.power_cities[index];
		if (!state.has_data || state.served_mwh <= 0.0) {
			continue;
		}
		const double tariff = state.tariff > 0.0 ? state.tariff : state.price;
		if (tariff <= 0.0) {
			continue;
		}
		const double served = state.served_mwh;
		const double bill_h = served * params.share_households * tariff;
		const double bill_i = served * params.share_industry * tariff;
		const double bill_s = served * params.share_services * tariff;
		const double bill_g = served * params.share_other * tariff;
		const double before = city.pop.savings;
		charge_households(city.pop, bill_h);
		const double paid_h = std::max(0.0, before - city.pop.savings);
		pool += paid_h;
		// R1a: pagamento ao setor de energia e EXTERNO ao caixa privado; com o
		// caixa unico ele sai do pool uma vez e respeita o sub-teto do setor
		// (fatia da receita medida). Desligado, e a conta de antes.
		const double paid_i = std::min(bill_i,
			city.firm_external_room());
		city.firm_external_pay(paid_i);
		pool += paid_i;
		const double paid_s = std::min(bill_s,
			city.service_external_room());
		city.service_external_pay(paid_s);
		pool += paid_s;
		const double paid_g = std::min(bill_g,
			std::max(0.0, city.government_budget));
		city.government_budget -= paid_g;
		pool += paid_g;
		// Remuneração do atacado (custo médio do período). A tarifa é
		// normalmente maior; a margem vai ao Estado/distribuidora. Se a tarifa
		// ficar abaixo do atacado, o Estado cobre até o próprio orçamento; o
		// resto é perda do setor (não se cria dinheiro).
		const double wholesale = state.average_cost > 0.0
			? state.average_cost : state.price;
		const double paid_total = paid_h + paid_i + paid_s + paid_g;
		const double margin = paid_total - served * wholesale;
		double to_state = 0.0;
		if (margin > 0.0) {
			to_state = margin;
		} else if (margin < 0.0) {
			to_state = -std::min(-margin,
				std::max(0.0, city.government_budget));
		}
		if (to_state > 0.0) {
			pool -= to_state;
			city.government_budget += to_state;
		} else if (to_state < 0.0) {
			const double subsidy = -to_state;
			city.government_budget -= subsidy;
			pool += subsidy;
		}
		const auto grid_found = ws.power_grids.find(city.nation);
		if (grid_found != ws.power_grids.end()) {
			grid_found->second.retail_margin += to_state;
		}
	}
	// 2. Custo das usinas do dia: combustível (mercado) + O&M + salários.
	const std::size_t plant_count = ws.power_plants.size();
	std::vector<double> fuel_local(plant_count, 0.0);
	std::vector<double> fuel_import(plant_count, 0.0);
	std::vector<int> fuel_product(plant_count, -1);
	std::vector<double> opex(plant_count, 0.0);
	std::vector<double> wages(plant_count, 0.0);
	double total_cost = 0.0;
	const int month = month_of_day(ws.day);
	for (std::size_t p = 0; p < plant_count; ++p) {
		PowerPlant& plant = ws.power_plants[p];
		plant.day_cost = 0.0;
		const double gen = plant.generated_mwh;
		if (gen <= 0.0 || !plant_city_in_range(ws, plant)) {
			continue;
		}
		City& city = ws.cities[static_cast<std::size_t>(plant.city_index)];
		if (power_fuel_is_thermal(plant.fuel)
				&& params.fuel_t_per_mwh[plant.fuel] > 0.0) {
			const int product = fuel_product_of(ws, plant.fuel);
			const double tons = gen * params.fuel_t_per_mwh[plant.fuel];
			double price = 0.0;
			double local = 0.0;
			if (product >= 0 && product < ws.products) {
				price = city.markets[product].price;
				if (price <= 0.0) {
					price = base_price(product);
				}
				Market& market = city.markets[product];
				local = std::min(tons, std::max(0.0, market.stock));
				// O combustível comprado sai do estoque do mercado da usina e
				// entra na demanda efetiva do dia (o preço reage à térmica).
				market.stock -= local;
				ws.consumed_today[product] += local;
				market.effective += tons;
			}
			fuel_product[p] = product;
			fuel_local[p] = local * price;
			fuel_import[p] = (tons - local) * price
				* params.fuel_import_premium;
		}
		// Salários seguem a operação do dia (geração ÷ disponibilidade).
		double cf = std::clamp(params.cf[plant.fuel], 0.0, 1.0);
		if (plant.fuel == POWER_SOLAR) {
			const PowerCityState& state =
				ws.power_cities[static_cast<std::size_t>(plant.city_index)];
			if (state.solar_month[month] > 0.0) {
				cf = std::clamp(state.solar_month[month], 0.0, 1.0);
			}
		}
		const double avail_mw = std::max(0.0, plant.mw) * cf;
		const double operating = avail_mw > 1e-9
			? std::clamp((gen / 24.0) / avail_mw, 0.0, 1.0) : 0.0;
		opex[p] = gen * plant.opex_per_mwh;
		wages[p] = plant.jobs * plant.wage * operating;
		total_cost += fuel_local[p] + fuel_import[p] + opex[p] + wages[p];
		// D-126: custo de caixa do dia da usina (o que a crise compara com a
		// receita do despacho; não inclui o custo fixo da capacidade ociosa).
		plant.day_cost = fuel_local[p] + fuel_import[p] + opex[p]
			+ wages[p];
	}
	// 3. Paga o custo até o caixa (parcial = perda declarada do setor):
	// combustível aos produtores (ou ao exterior), O&M/salários na cidade.
	const double pay_ratio = total_cost > 1e-9
		? std::clamp(pool / total_cost, 0.0, 1.0) : 0.0;
	double paid_total = 0.0;
	for (std::size_t p = 0; p < plant_count; ++p) {
		const PowerPlant& plant = ws.power_plants[p];
		if (plant.generated_mwh <= 0.0) {
			continue;
		}
		// Guarda de faixa: `assert` em debug e skip determinístico em release.
		if (!plant_city_in_range(ws, plant)) {
			continue;
		}
		City& city = ws.cities[static_cast<std::size_t>(plant.city_index)];
		const double fuel_paid = (fuel_local[p] + fuel_import[p]) * pay_ratio;
		if (fuel_paid > 0.0 && fuel_product[p] >= 0
				&& fuel_product[p] < ws.products) {
			// O combustível LOCAL saiu do mercado da própria cidade: quem
			// vendeu foram as firmas dessa cidade (é a única origem que o
			// mercado registra). A parcela IMPORTADA sai do país.
			const double fuel_list = fuel_local[p] + fuel_import[p];
			const double local_share = fuel_list > 1e-12
				? fuel_local[p] / fuel_list : 0.0;
			// R1a: receita do gerador e do O&M entra pelo credito do setor de
			// firmas (com o caixa unico, credita o pool; desligado, o caixa).
			city.firm_receive(fuel_paid * local_share);
			ws.foreign_cash += fuel_paid * (1.0 - local_share);
			paid_total += fuel_paid;
		}
		const double opex_paid = opex[p] * pay_ratio;
		city.firm_receive(opex_paid);
		paid_total += opex_paid;
		const double wages_paid = wages[p] * pay_ratio;
		credit_households(city.pop, wages_paid);
		paid_total += wages_paid;
	}
	pool = std::max(0.0, pool - paid_total);
	// 4. Lucro (o que sobrou) ao dono; soma zero (sai de `power_cash`).
	double generated_total = 0.0;
	for (const PowerPlant& plant : ws.power_plants) {
		generated_total += std::max(0.0, plant.generated_mwh);
	}
	const double profit_total = pool;
	std::size_t last_owner = plant_count;
	for (std::size_t p = 0; p < plant_count; ++p) {
		if (ws.power_plants[p].generated_mwh > 0.0
				&& ws.power_plants[p].city_index >= 0) {
			last_owner = p;
		}
	}
	double distributed = 0.0;
	for (std::size_t p = 0; p < plant_count; ++p) {
		const PowerPlant& plant = ws.power_plants[p];
		if (plant.generated_mwh <= 0.0 || generated_total <= 1e-9
				|| profit_total <= 0.0) {
			continue;
		}
		// Guarda de faixa: `assert` em debug e skip determinístico em release.
		if (!plant_city_in_range(ws, plant)) {
			continue;
		}
		City& city = ws.cities[static_cast<std::size_t>(plant.city_index)];
		const double profit = p == last_owner
			? std::max(0.0, profit_total - distributed)
			: profit_total * plant.generated_mwh / generated_total;
		distributed += profit;
		if (plant.owner_group.empty()) {
			city.government_budget += profit;
		} else {
			Group* group = find_group(plant.owner_group);
			if (group != nullptr) {
				group->cash += profit;
			} else {
				city.government_budget += profit;
			}
		}
	}
	ws.power_cash = 0.0;
}


} // namespace ogs
