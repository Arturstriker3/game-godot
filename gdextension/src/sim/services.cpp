// Módulo de domínio: serviços — setores, negócios, demanda e produção
// (S13 do docs/economy/core_refactor_plan.md).
// SimCore:: métodos movidos de sim_core.cpp — realocação byte-idêntica.
#include "sim_core.h"

#include "profiling.h"
#include "sim_constants.h"
#include "sim_labor.h"
#include "urban_land.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace ogs {
void SimCore::phase_services() {
	// Prepara os índices por cidade/setor UMA vez (o serial fazia isso dentro
	// de cada cidade; a re-assign é idempotente e não pode rodar em paralelo).
	if (ws_.service_by_city.size() != ws_.cities.size()) {
		ws_.service_by_city.assign(ws_.cities.size(), {});
	}
	if (ws_.service_by_sector.size() != ws_.service_sectors.size()) {
		ws_.service_by_sector.assign(ws_.service_sectors.size(), {});
	}
	// S16: a simulação de serviços é independente por cidade (escreve só na
	// própria cidade e nas suas construções; lê só dados globais imutáveis no
	// dia). A única saída global é a exportação de divisas (ws_.foreign_cash,
	// subtração) — vai para um scratch por cidade×setor e é reduzida em ordem
	// crescente de cidade E setor (a MESMA ordem do loop serial).
	std::vector<double> foreign_scratch(
		ws_.cities.size() * kMaxServiceSectors, 0.0);
	// O savings_surplus do TOPO do dia (antes de o passo 6 reduzir estoques de
	// comida) é o que o serial usava no `finance_base` da abertura; guardamos
	// por cidade para a 2ª passada reproduzir exatamente o mesmo valor.
	std::vector<double> savings_surplus_scratch(ws_.cities.size(), 0.0);
	auto process_city = [this, &foreign_scratch, &savings_surplus_scratch](
			std::size_t begin, std::size_t end) {
		for (std::size_t city_index = begin; city_index < end; ++city_index) {
			services_day(ws_.cities[city_index],
				&foreign_scratch[city_index * kMaxServiceSectors],
				savings_surplus_scratch[city_index]);
		}
	};
	pool().parallel_for(ws_.cities.size(), process_city);

	// Redução serial: ordem crescente de cidade E de setor.
	for (std::size_t city_index = 0; city_index < ws_.cities.size(); ++city_index) {
		for (int sector = 0; sector < kMaxServiceSectors; ++sector) {
			ws_.foreign_cash -= foreign_scratch[
				city_index * kMaxServiceSectors
				+ static_cast<std::size_t>(sector)];
		}
	}

	// 2ª passada serial: abertura/fechamento em ordem fixa (cidade -> setor),
	// com o savings_surplus do TOPO do dia (mesmo valor do serial). A6: decisão
	// SEMANAL por cidade (`(dia - índice) % 7 == 0`); demanda e faturamento
	// continuam diários na 1ª passada.
	{
		OGS_SIM_ZONE(kPhaseServicesDecisions);
		const int period = ws_.cadence.weekly_period;
		for (std::size_t city_index = 0; city_index < ws_.cities.size();
				++city_index) {
			if (!cadence_due(ws_.day, city_index, period)) {
				continue;
			}
			services_open_close(ws_.cities[city_index],
				savings_surplus_scratch[city_index]);
		}
	}

	OGS_SIM_PHASE_LAP(kPhaseServices);
}

// ------------------------------------------------------------------
// F8 — serviços.
// ------------------------------------------------------------------

int SimCore::service_sector_index(const std::string& sector) const {
	for (std::size_t index = 0; index < ws_.service_sectors.size(); ++index) {
		if (ws_.service_sectors[index].id == sector) {
			return static_cast<int>(index);
		}
	}
	return -1;
}
void SimCore::rebuild_service_indexes() {
	ws_.service_by_city.assign(ws_.cities.size(), {});
	ws_.service_by_sector.assign(ws_.service_sectors.size(), {});
	for (int index = 0; index < static_cast<int>(ws_.service_businesses.size()); ++index) {
		const ServiceBusiness& business =
			ws_.service_businesses[static_cast<std::size_t>(index)];
		if (business.bankrupt) {
			continue;
		}
		const auto found = ws_.city_lookup.find(business.city);
		if (found != ws_.city_lookup.end()) {
			ws_.service_by_city[static_cast<std::size_t>(found->second)].push_back(index);
		}
		if (business.sector >= 0
				&& business.sector < static_cast<int>(ws_.service_by_sector.size())) {
			ws_.service_by_sector[static_cast<std::size_t>(business.sector)].push_back(index);
		}
	}
}
double SimCore::city_food_reserve(const City& city) const {
	// F8 v2: reserva alimentar robusta (XGS) que a poupança deve preservar antes
	// de financiar abertura de negócio. Usa a MESMA regra prudencial da F3
	// (kFoodReserveDays de cesta) — a poupança que importa comida em nação
	// sem terra fica intocada; só o excedente financia serviço.
	const double need = std::max(0.0,
		city.simulation_population * kNeedPerCapita);
	double local = 0.0;
	double import = 0.0;
	double per_ge = 0.0;
	food_basket_cost(city, need, local, import, per_ge);
	return need * kFoodReserveDays * per_ge;
}
bool SimCore::service_open(City& city, int sector, double capacity,
		double capital) {
	if (sector < 0 || sector >= static_cast<int>(ws_.service_sectors.size())) {
		return false;
	}
	const ServiceSector& definition =
		ws_.service_sectors[static_cast<std::size_t>(sector)];
	capacity = std::max(0.0, capacity);
	capital = std::max(0.0, capital);
	if (capacity <= 0.0) {
		return false;
	}
	// F8 v2: o capital de abertura NÃO pode sair da poupança que paga comida
	// (F3 soma savings + renda para importar alimento — DJI regredia 0,472→0,355
	// quando o capital drenava essa poupança, Q-004). Ordem de financiamento:
	//   1º caixa do PRÓPRIO SETOR (lucros retidos em service_cash) — já está no
	//      setor, vira equity do negócio sem movimento de dinheiro;
	//   2º poupança das famílias ACIMA da reserva alimentar robusta — move
	//      savings→service_cash (e é devolvida no fechamento).
	// Se nem a soma cobre o capital sem tocar a reserva de comida, não abre.
	const double food_reserve = city_food_reserve(city);
	const double savings_surplus = std::max(0.0, city.pop.savings - food_reserve);
	const double from_sector = std::min(capital, std::max(0.0, city.service_cash));
	const double from_savings = std::min(capital - from_sector, savings_surplus);
	if (from_sector + from_savings < capital) {
		return false;
	}
	// §19 (W1): abrir negócio de serviço exige espaço na zona COMERCIAL. Sem
	// espaço a decisão NÃO falha: fica pendente (a entrada tenta de novo no dia
	// seguinte) e sinaliza demanda de zona para a prefeitura (W2 expandir/
	// adensar). O limite só vale quando a cidade tem solo urbano carregado.
	if (!ws_.land.empty() && ws_.land.size() == ws_.cities.size()) {
		const std::size_t urban_index =
			static_cast<std::size_t>(&city - ws_.cities.data());
		// A zona comercial é medida em TRABALHADORES (m² por trabalhador): o
		// negócio novo ocupa o emprego que ele declara criar.
		const double jobs = std::max(1.0, capacity * definition.jobs_per_unit);
		if (!urban_land::zone_has_room(ws_, urban_index, ZONE_COM, jobs)) {
			urban_land::signal_zone_demand(ws_.land[urban_index], ZONE_COM);
			return false;
		}
	}
	if (from_savings > 0.0) {
		// B2: capital do negócio sai da poupança rateada pelos quintis.
		charge_households(city.pop, from_savings);
		city.service_cash += from_savings;
	}
	ServiceBusiness business;
	business.id = ws_.next_service_business_id++;
	business.sector = sector;
	business.city = city.id;
	business.owner_stratum = kEducationLevels - 1;
	business.capacity = capacity;
	business.jobs = capacity * definition.jobs_per_unit;
	business.workers = business.jobs;
	// `capital` guarda só a parcela da POP (devolvida no fechamento); a equity
	// total (capital_left) soma a parcela do setor.
	business.capital = from_savings;
	business.capital_left = from_sector + from_savings;
	business.opened_day = ws_.day;
	unsigned long long seed = 1469598103934665603ULL;
	auto mix = [&seed](const std::string& text) {
		for (const char c : text) {
			seed ^= static_cast<unsigned char>(c);
			seed *= 1099511628211ULL;
		}
		seed ^= 0xffULL;
		seed *= 1099511628211ULL;
	};
	mix(city.id);
	mix(definition.id);
	mix(std::to_string(business.id));
	business.name_seed = seed;
	const int index = static_cast<int>(ws_.service_businesses.size());
	ws_.service_businesses.push_back(std::move(business));
	const auto found = ws_.city_lookup.find(city.id);
	if (found != ws_.city_lookup.end()) {
		if (ws_.service_by_city.size() != ws_.cities.size()) {
			ws_.service_by_city.assign(ws_.cities.size(), {});
		}
		ws_.service_by_city[static_cast<std::size_t>(found->second)].push_back(index);
	}
	if (ws_.service_by_sector.size() != ws_.service_sectors.size()) {
		ws_.service_by_sector.assign(ws_.service_sectors.size(), {});
	}
	ws_.service_by_sector[static_cast<std::size_t>(sector)].push_back(index);
	city.service_business_count += 1;
	return true;
}
void SimCore::service_close(ServiceBusiness& business) {
	if (business.bankrupt) {
		return;
	}
	business.bankrupt = true;
	City* city = find_city(business.city);
	if (city != nullptr) {
		// F8 v2: devolve à POP no máximo a parcela que ELA aportou
		// (`business.capital` = from_savings); a parcela financiada pelo setor
		// fica no setor. `capital_left` pode exceder o aporte quando o negócio
		// lucra — o lucro já foi distribuído no agregado do setor, então só o
		// aporte da POP volta (o teto pelo service_cash evita drenar o giro).
		const double returned = std::min(
			std::max(0.0, business.capital_left),
			std::max(0.0, business.capital));
		const double paid_back = std::min(returned, std::max(0.0, city->service_cash));
		city->service_cash -= paid_back;
		// B2: o aporte devolvido volta para a poupança dos quintis.
		credit_households(city->pop, paid_back);
		city->service_business_count = std::max(0, city->service_business_count - 1);
	}
	business.capital_left = 0.0;
	ws_.service_bankruptcies += 1;
}
void SimCore::seed_services() {
	ws_.service_businesses.clear();
	ws_.service_businesses.shrink_to_fit();
	ws_.service_bankruptcies = 0;
	ws_.next_service_business_id = 1;
	ws_.service_by_city.assign(ws_.cities.size(), {});
	ws_.service_by_sector.assign(ws_.service_sectors.size(), {});
	if (!ws_.services_loaded || ws_.service_sectors.empty()) {
		return;
	}
	for (City& city : ws_.cities) {
		city.service_business_count = 0;
		for (int sector = 0; sector < kMaxServiceSectors; ++sector) {
			ServiceMarket& market = city.service_markets[sector];
			market.base_price =
				ws_.service_sectors[static_cast<std::size_t>(sector)].price;
			if (market.price <= 0.0 || !std::isfinite(market.price)) {
				market.price = market.base_price;
			}
		}
		const double population = std::max(1.0, city.simulation_population);
		int count = static_cast<int>(std::clamp(
			4.0 + std::log10(std::max(1000.0, population)) * 2.0, 4.0, 16.0));
		// Escolhe os setores com maior peso de consumo das famílias; setores
		// exportáveis entram quando o país tem renda acima da média.
		std::vector<int> order(ws_.service_sectors.size());
		std::vector<double> weight(ws_.service_sectors.size(), 0.0);
		for (std::size_t sector = 0; sector < ws_.service_sectors.size(); ++sector) {
			order[sector] = static_cast<int>(sector);
			const ServiceSector& definition = ws_.service_sectors[sector];
			weight[sector] = definition.pop_share[0] + definition.pop_share[1] * 1.4
				+ definition.pop_share[2] * 1.8 + definition.business_share
				+ definition.government_share * 0.5;
		}
		std::stable_sort(order.begin(), order.end(), [&weight](int a, int b) {
			return weight[static_cast<std::size_t>(a)]
				> weight[static_cast<std::size_t>(b)];
		});
		for (int slot = 0; slot < count && slot < static_cast<int>(order.size()); ++slot) {
			const int sector = order[static_cast<std::size_t>(slot)];
			const ServiceSector& definition =
				ws_.service_sectors[static_cast<std::size_t>(sector)];
			// F8 fix: sem o teto arbitrário de 40 postos. A semente parte do
			// mínimo viável do setor; a ENTRADA (passo 9) cresce a oferta até a
			// demanda real. A folha sobre efetivos torna o negócio ocioso
			// não-letivo, então superdimensionar não é mais fatal.
			const double capacity = definition.min_capacity;
			const double capital = capacity * definition.capital_per_unit;
			// F8 v2: o capital de abertura vem do caixa do próprio setor e, se
			// faltar, da poupança das famílias ACIMA da reserva de comida — o
			// gate interno de service_open decide. Cidade pequena/pobre
			// semeia menos negócios e abre os demais quando houver caixa.
			service_open(city, sector, capacity, capital);
		}
		if (city.external_income_index > 1.2) {
			for (int sector = 0; sector < static_cast<int>(ws_.service_sectors.size()); ++sector) {
				const ServiceSector& definition =
					ws_.service_sectors[static_cast<std::size_t>(sector)];
				if (!definition.exportable) {
					continue;
				}
				bool present = false;
				for (const int index : ws_.service_by_city[static_cast<std::size_t>(
						&city - ws_.cities.data())]) {
					if (ws_.service_businesses[static_cast<std::size_t>(index)].sector
							== sector) {
						present = true;
						break;
					}
				}
				if (present) {
					continue;
				}
				const double capacity = definition.min_capacity;
				const double capital = capacity * definition.capital_per_unit;
				service_open(city, sector, capacity, capital);
			}
		}
	}
}
void SimCore::services_day(City& city, double* foreign_slots,
		double& savings_surplus_out) {
	if (!ws_.services_loaded || ws_.service_sectors.empty()) {
		return;
	}
	const std::size_t city_index =
		static_cast<std::size_t>(&city - ws_.cities.data());
	const Pop& pop = city.pop;
	const double wage = std::max(0.0, city.service_wage);
	// F8 v2 (calibração, Q-004): o custo de trabalho do negócio é o salário de
	// mercado INTEGRAL — sem o teto `preço÷jobs_per_unit×0,68`, que era a muleta
	// que mascarava o preço sub-calibrado. A margem realista vem da ÂNCORA DE
	// PREÇO escalada pelo salário local (passo 8): price/jobs_per_unit acompanha
	// o salário, então o negócio não fica estruturalmente no prejuízo em país
	// rico — o teto contábil deixa de ser necessário.
	// Reserva alimentar robusta (calculada UMA vez por cidade/dia): a poupança
	// acima dela é o que pode financiar a entrada de negócio, sem tocar a
	// poupança que importa comida (DJI, Q-004).
	const double food_reserve = city_food_reserve(city);
	const double savings_surplus = std::max(0.0, city.pop.savings - food_reserve);
	savings_surplus_out = savings_surplus;

	// 1. Pesos da demanda das famílias por classe de consumo (basic/life/
	// luxury). Com Gini, `class_population` é a repartição real; sem Gini,
	// espelha os estratos (mesma regra da F7). O dinheiro é o
	// `pop.service_spend` já debitado das famílias; aqui ele é DISTRIBUÍDO.
	double class_pop[kEducationLevels] = {
		std::max(0.0, pop.class_population[0]),
		std::max(0.0, pop.class_population[1]),
		std::max(0.0, pop.class_population[2])};
	if (class_pop[0] + class_pop[1] + class_pop[2] <= 1e-9) {
		for (int level = 0; level < kEducationLevels; ++level) {
			class_pop[level] = std::max(0.0, pop.stratum_population[level]);
		}
	}
	double weight[kMaxServiceSectors] = {};
	double weight_total = 0.0;
	for (int sector = 0; sector < kMaxServiceSectors; ++sector) {
		const ServiceSector& definition =
			ws_.service_sectors[static_cast<std::size_t>(sector)];
		double value = 0.0;
		for (int level = 0; level < kMaxNeedsLevels; ++level) {
			value += class_pop[level] * definition.pop_share[level];
		}
		weight[sector] = value;
		weight_total += value;
	}
	const double household_pool = std::max(0.0, pop.service_spend);

	// B7: serviços informais agregados (sem firma) — desvia uma fatia do gasto
	// doméstico em serviços ANTES da divisão entre os setores formais. O dinheiro
	// realizado vai para os trabalhadores informais (crédito nos quintis), NÃO
	// entra em service_cash; o pool formal é o restante (conservação total).
	const double informal_revenue = compute_informal_services(city);
	const double formal_household_pool = std::max(0.0,
		household_pool - informal_revenue);

	// 2. B2B: o opex pago pelas empresas é receita de serviços. Finanças,
	// profissionais, TI, telecom, manutenção e limpeza entram por participação
	// declarada; logística entra pela TONELAGEM movimentada ontem (sold_prev da
	// cesta + insumos), como pede o plano.
	double tonnage = 0.0;
	for (int product = 0; product < ws_.products; ++product) {
		tonnage += std::max(0.0, city.markets[product].sold_prev);
	}
	city.service_tonnage_prev = tonnage;
	const double b2b_pool = std::max(0.0, city.opex_revenue);
	double b2b_weight[kMaxServiceSectors] = {};
	double b2b_total = 0.0;
	for (int sector = 0; sector < kMaxServiceSectors; ++sector) {
		const ServiceSector& definition =
			ws_.service_sectors[static_cast<std::size_t>(sector)];
		double value = definition.business_share;
		if (definition.freight_share > 0.0) {
			value += definition.freight_share
				* std::clamp(tonnage / 2000.0, 0.05, 4.0);
		}
		b2b_weight[sector] = value;
		b2b_total += value;
	}

	// 3. Governo: compra uma fração pequena do orçamento em serviços
	// (limpeza/segurança/resíduos, construção, profissionais, saúde/educação).
	// É transferência explícita governo → service_cash (dinheiro conservado).
	double government_pool = 0.0;
	double gov_weight[kMaxServiceSectors] = {};
	double gov_total = 0.0;
	for (int sector = 0; sector < kMaxServiceSectors; ++sector) {
		gov_weight[sector] =
			ws_.service_sectors[static_cast<std::size_t>(sector)].government_share;
		gov_total += gov_weight[sector];
	}
	if (gov_total > 0.0) {
		// A fração é da ARRECADAÇÃO DO DIA (fluxo), não do caixa acumulado:
		// cobrar a fração sobre o estoque todo dia drenava o tesouro que
		// financia a importação de comida (AFG-KAN/KAB em 365 d, F8). O caixa
		// do dia continua sendo o teto.
		const double daily_flow = std::max(0.0, pop.taxes);
		government_pool = std::min(daily_flow * ws_.service_government_share,
			std::max(0.0, city.government_budget));
		city.government_budget -= government_pool;
		city.service_cash += government_pool;
		city.service_government_revenue += government_pool;
	}

	// 4. Capacidade e exportação. A exportação é atendida primeiro; o resto da
	// oferta fica para o mercado doméstico. O preço de exportação usa a
	// competitividade cambial da F5 (a tarifa declarada para "services" reduz a
	// competitividade, sem criar/destruir dinheiro).
	const double fx_competitiveness = std::clamp(
		competitiveness(city.nation), 0.5, 1.5);
	const int service_tariff_product = product_index(std::string("services"));
	const double service_tariff = service_tariff_product >= 0
		? tariff_rate(city.nation, "*", service_tariff_product) : 0.0;
	const double export_factor = fx_competitiveness * (1.0 - std::clamp(service_tariff, 0.0, 1.0));
	double sector_supply[kMaxServiceSectors] = {};
	double sector_exports[kMaxServiceSectors] = {};
	double sector_export_revenue[kMaxServiceSectors] = {};
	// Contagem de negócios ativos por setor, na MESMA passada que soma a oferta:
	// o laço de entrada (passo 9) usa este vetor em vez de varrer os negócios
	// da cidade para cada setor (era O(setores × negócios)). Nenhum negócio
	// abre/fecha entre aqui e o passo 9 (abertura/fechamento acontecem no próprio
	// passo 9), então a contagem é idêntica à varredura antiga — resultado igual.
	int active_by_sector[kMaxServiceSectors] = {};
	for (const int index : ws_.service_by_city[city_index]) {
		ServiceBusiness& business =
			ws_.service_businesses[static_cast<std::size_t>(index)];
		if (business.bankrupt || business.sector < 0
				|| business.sector >= kMaxServiceSectors) {
			continue;
		}
		const double capacity = std::max(0.0,
			business.capacity * std::clamp(business.utilization > 0.0
				? business.utilization : 1.0, 0.05, 1.0));
		sector_supply[business.sector] += capacity;
		++active_by_sector[business.sector];
	}
	const double external_factor = std::clamp(city.external_income_index, 0.2, 3.0);
	for (int sector = 0; sector < kMaxServiceSectors; ++sector) {
		const ServiceSector& definition =
			ws_.service_sectors[static_cast<std::size_t>(sector)];
		if (!definition.exportable || definition.export_share <= 0.0) {
			continue;
		}
		const double units = sector_supply[sector] * definition.export_share
			* external_factor;
		if (units <= 0.0) {
			continue;
		}
		const double revenue = units
			* city.service_markets[sector].price * export_factor;
		sector_exports[sector] = units;
		sector_export_revenue[sector] = revenue;
		city.service_export_revenue += revenue;
		city.service_cash += revenue;
		foreign_slots[sector] += revenue;
	}

	// 5. Mercados: demanda em unidades = dinheiro / preço. A receita só é
	// realizada na proporção atendida (demanda sem oferta não vira dinheiro).
	double total_units_served = 0.0;
	double total_service_cost = 0.0;
	double total_input_cost = 0.0;
	for (int sector = 0; sector < kMaxServiceSectors; ++sector) {
		ServiceMarket& market = city.service_markets[sector];
		market.demand = 0.0;
		market.supply = sector_supply[sector];
		market.unmet = 0.0;
		market.revenue = 0.0;
		market.input_cost = 0.0;
		market.service_cost = 0.0;
		market.jobs = 0.0;
		market.businesses = 0.0;
		market.exports = sector_exports[sector];
		market.export_revenue = sector_export_revenue[sector];
		market.coverage = 0.0;
		double money = 0.0;
		if (weight_total > 0.0) {
			money += formal_household_pool * weight[sector] / weight_total;
		}
		if (b2b_total > 0.0) {
			money += b2b_pool * b2b_weight[sector] / b2b_total;
		}
		if (gov_total > 0.0) {
			money += government_pool * gov_weight[sector] / gov_total;
		}
		if (market.price <= 0.0) {
			market.price = std::max(0.01,
				ws_.service_sectors[static_cast<std::size_t>(sector)].price);
		}
		const double domestic_demand = money / market.price;
		const double total_demand = domestic_demand + market.exports;
		market.demand = total_demand;
		for (int level = 0; level < kMaxNeedsLevels; ++level) {
			market.pop_demand[level] = weight_total > 0.0 && market.price > 1e-12
				? formal_household_pool * class_pop[level]
					* ws_.service_sectors[static_cast<std::size_t>(sector)].pop_share[level]
					/ weight_total / market.price
				: 0.0;
		}
		const double served = std::min(market.supply, total_demand);
		const double domestic_served = std::max(0.0,
			std::min(domestic_demand, served - market.exports));
		const double export_served = std::min(market.exports, served);
		// Dinheiro realizado: fração doméstica atendida do pool + exportação
		// efetivamente entregue. O pool não gasto fica em service_cash.
		const double realized = domestic_demand > 1e-12
			? money * domestic_served / domestic_demand : 0.0;
		market.revenue = realized + export_served * market.price * export_factor;
		if (market.demand > 0.0) {
			market.unmet = std::max(0.0, market.demand - served);
		}
		total_units_served += served;
		// Cobertura privada de saúde/educação: oferta por habitante contra a
		// necessidade de referência (gancho documentado, não serviço público).
		// B1: a necessidade escala com as coortes (65+ pesa mais na saúde,
		// crianças na educação); sem demografia o fator é 1,0 (legado).
		if (ws_.service_sectors[static_cast<std::size_t>(sector)].health) {
			const double need = std::max(1e-9,
				city.simulation_population * kServiceHealthNeedPerCapita
					* city.pop.health_need_factor);
			market.coverage = std::clamp(market.supply / need, 0.0, 1.0);
			city.private_health_cover = market.coverage;
		}
		if (ws_.service_sectors[static_cast<std::size_t>(sector)].education) {
			const double need = std::max(1e-9,
				city.simulation_population * kServiceEducationNeedPerCapita
					* city.pop.education_need_factor);
			market.coverage = std::clamp(market.supply / need, 0.0, 1.0);
			city.private_education_cover = market.coverage;
		}
	}

	// 6. Negócios: produzem a sua fatia da oferta, compram insumos (bens e
	// serviços) e pagam a folha (fatia de city.service_wage_bill). O lucro é
	// informativo para entrada/saída; a distribuição aos donos continua no
	// agregado do setor de serviços (service_profit).
	std::vector<double> sector_revenue_gain(kMaxServiceSectors, 0.0);
	for (const int index : ws_.service_by_city[city_index]) {
		ServiceBusiness& business =
			ws_.service_businesses[static_cast<std::size_t>(index)];
		if (business.bankrupt || business.sector < 0
				|| business.sector >= kMaxServiceSectors) {
			continue;
		}
		ServiceMarket& market = city.service_markets[business.sector];
		const ServiceSector& definition =
			ws_.service_sectors[static_cast<std::size_t>(business.sector)];
		market.businesses += 1.0;
		const double capacity = std::max(0.0, business.capacity);
		double units = 0.0;
		if (market.supply > 1e-12) {
			units = std::min(capacity, market.supply > 0.0
				? std::max(0.0, market.demand) * capacity / market.supply : 0.0);
			units = std::min(units, capacity);
		}
		business.utilization = capacity > 1e-12 ? units / capacity : 0.0;
		business.workers = business.jobs * std::clamp(business.utilization, 0.05, 1.0);
		const double share = market.supply > 1e-12 ? capacity / market.supply : 0.0;
		business.revenue = market.revenue * share;
		// Insumos de bens: compra no mercado da cidade ao preço corrente, até o
		// estoque disponível. O dinheiro sai de service_cash e entra em
		// firm_cash (produtor), conservando o total.
		double input_cost = 0.0;
		for (const ServiceSector::Input& input : definition.inputs) {
			if (input.product < 0 || input.product >= ws_.products) {
				continue;
			}
			Market& goods = city.markets[input.product];
			const double needed = input.qty * units;
			const double bought = std::min(needed, std::max(0.0, goods.stock));
			if (bought <= 0.0) {
				continue;
			}
			const double paid = bought * goods.price;
			goods.stock -= bought;
			goods.demand += bought;
			goods.sold += bought;
			goods.desired += needed;
			city.revenue_by_product[input.product] += paid;
			input_cost += paid;
		}
		business.input_cost = input_cost;
		// Insumos de serviço: transferência entre pools setoriais (não sai do
		// service_cash; o lucro do fornecedor sobe e o do comprador cai).
		double service_cost = 0.0;
		for (std::size_t slot = 0; slot < definition.service_inputs.size(); ++slot) {
			const int supplier = definition.service_inputs[slot];
			if (supplier < 0 || supplier >= kMaxServiceSectors) {
				continue;
			}
			const double qty = definition.service_input_qty[slot] * units;
			const double price = city.service_markets[supplier].price;
			const double paid = qty * price;
			service_cost += paid;
			sector_revenue_gain[static_cast<std::size_t>(supplier)] += paid;
		}
		business.service_cost = service_cost;
		// F8 fix: folha sobre EFETIVOS (workers), não sobre POSTOS (jobs). O
		// negócio ocioso não paga a folha cheia — o custo acompanha a
		// utilização. A folha agregada do setor (labor_day) continua sendo
		// a do empregador residual da F7; aqui só se atribui o custo marginal.
		business.wages = business.workers * wage;
		business.profit = business.revenue - business.wages
			- business.input_cost - business.service_cost;
		business.capital_left = std::max(0.0,
			business.capital_left + business.profit);
		if (business.profit < 0.0 && business.utilization > 1e-9) {
			++business.loss_days;
		} else if (business.profit > 0.0) {
			business.loss_days = 0;
		}
		total_input_cost += business.input_cost;
		total_service_cost += business.service_cost;
	}
	// Receita dos fornecedores de serviços: aumenta o pool publicado do setor.
	for (int sector = 0; sector < kMaxServiceSectors; ++sector) {
		if (sector_revenue_gain[static_cast<std::size_t>(sector)] > 0.0) {
			city.service_markets[sector].revenue +=
				sector_revenue_gain[static_cast<std::size_t>(sector)];
		}
	}
	// E chega aos negócios fornecedores (mesma proporção de capacidade): o
	// lucro do comprador já pagou o serviço, então o efeito agregado é nulo.
	for (const int index : ws_.service_by_city[city_index]) {
		ServiceBusiness& business =
			ws_.service_businesses[static_cast<std::size_t>(index)];
		if (business.bankrupt || business.sector < 0
				|| business.sector >= kMaxServiceSectors) {
			continue;
		}
		const double gain = sector_revenue_gain[static_cast<std::size_t>(business.sector)];
		if (gain <= 0.0) {
			continue;
		}
		const double supply = city.service_markets[business.sector].supply;
		const double share = supply > 1e-12 ? business.capacity / supply : 0.0;
		business.revenue += gain * share;
		business.profit += gain * share;
		business.capital_left += gain * share;
	}
	// Paga os insumos de bens: service_cash → firm_cash.
	const double paid_inputs = std::min(total_input_cost,
		std::max(0.0, city.service_cash));
	if (paid_inputs > 0.0) {
		city.service_cash -= paid_inputs;
		city.firm_cash += paid_inputs;
	}
	city.service_input_cost = total_input_cost;

	// 7. Emprego por setor: divide o emprego AGREGADO de serviços da F7 (o
	// empregador residual) pela participação dos negócios de cada setor. Sem
	// negócios, a divisão segue os pesos de demanda — a soma continua igual a
	// city.service_jobs (nenhum emprego é criado ou perdido aqui).
	double jobs_weight[kMaxServiceSectors] = {};
	double jobs_total = 0.0;
	for (const int index : ws_.service_by_city[city_index]) {
		const ServiceBusiness& business =
			ws_.service_businesses[static_cast<std::size_t>(index)];
		if (business.bankrupt || business.sector < 0
				|| business.sector >= kMaxServiceSectors) {
			continue;
		}
		jobs_weight[business.sector] += std::max(0.0, business.jobs);
		jobs_total += std::max(0.0, business.jobs);
	}
	if (jobs_total <= 1e-12) {
		for (int sector = 0; sector < kMaxServiceSectors; ++sector) {
			jobs_weight[sector] = weight[sector];
			jobs_total += weight[sector];
		}
	}
	for (int sector = 0; sector < kMaxServiceSectors; ++sector) {
		city.service_markets[sector].jobs = jobs_total > 1e-12
			? city.service_jobs * jobs_weight[sector] / jobs_total : 0.0;
	}

	// 8. Preço: reage à razão demanda financiada/oferta, com suavização e
	// banda em torno da âncora. Oferta zero com demanda > 0 sobe ao teto.
	for (int sector = 0; sector < kMaxServiceSectors; ++sector) {
		ServiceMarket& market = city.service_markets[sector];
		const ServiceSector& definition =
			ws_.service_sectors[static_cast<std::size_t>(sector)];
		// F8 v2 — ÂNCORA LIGADA AO SALÁRIO DO PAÍS: o preço-âncora do setor
		// escala com o salário de serviço local (kServiceWage = 0,15, a
		// âncora salarial da F7). Sem isso price/jobs_per_unit ficava fixo em
		// ~0,22 e, em país rico, abaixo do salário — o negócio formal ficava no
		// prejuízo estrutural (o teto 0,68 mascarava). Com a âncora escalando, a
		// receita por posto passa a ≈ 1,47× o salário local, então a margem fica
		// estável em país rico e pobre; os limites evitam preço zero/explosivo.
		const double anchor_scale = std::clamp(
			wage / kServiceWage, kServiceAnchorScaleMin,
			kServiceAnchorScaleMax);
		market.base_price = definition.price * anchor_scale
			* kServiceMarginCalibration;
		const double ratio = market.supply > 1e-9
			? std::max(0.0, market.demand) / market.supply
			: (market.demand > 1e-9 ? 4.0 : 1.0);
		const double target = market.base_price * std::clamp(
			std::pow(ratio, 0.35), kServicePriceBandLow,
			kServicePriceBandHigh);
		market.price += (target - market.price) * kServicePriceSmooth;
		market.price = std::clamp(market.price,
			market.base_price * kServicePriceBandLow,
			market.base_price * kServicePriceBandHigh);
	}

	(void)total_units_served;
	(void)total_service_cost;
}

// S16: 2ª passada serial de serviços — abertura e fechamento de negócios em
// ordem fixa (cidade -> setor), depois que todas as cidades simularam o dia.
// A ordem importa: a abertura consome o caixa do setor (service_cash) e a
// poupança das famílias, e o fechamento reduz a contagem de negócios ativos.
void SimCore::services_open_close(City& city, double savings_surplus) {
	if (!ws_.services_loaded || ws_.service_sectors.empty()) {
		return;
	}
	const std::size_t city_index =
		static_cast<std::size_t>(&city - ws_.cities.data());
	const double wage = std::max(0.0, city.service_wage);
	// Recomputa a contagem de negócios ativos por setor (mesma passada do
	// passo 4): nada abriu/fechou entre a fase paralela e esta 2ª passada.
	int active_by_sector[kMaxServiceSectors] = {};
	for (const int index : ws_.service_by_city[city_index]) {
		const ServiceBusiness& business =
			ws_.service_businesses[static_cast<std::size_t>(index)];
		if (business.bankrupt || business.sector < 0
				|| business.sector >= kMaxServiceSectors) {
			continue;
		}
		++active_by_sector[business.sector];
	}

	// 9. Entrada e saída. Entrada só com demanda não atendida, margem esperada
	// acima do limiar e capital financiável pelo caixa do setor + poupança acima
	// da reserva de comida. Até kServiceOpensPerDay aberturas por cidade/dia
	// (teto, não alvo: sem demanda não atendida, não abre nada).
	int opens = 0;
	for (int sector = 0; sector < kMaxServiceSectors && opens < kServiceOpensPerDay; ++sector) {
		ServiceMarket& market = city.service_markets[sector];
		const ServiceSector& definition =
			ws_.service_sectors[static_cast<std::size_t>(sector)];
		if (market.unmet < definition.min_capacity * 0.5) {
			continue;
		}
		if (active_by_sector[sector] >= kServiceMaxPerSector) {
			continue;
		}
		// F8 v2: capacidade pela DEMANDA não atendida, com folga de 25% para a
		// oferta se manter à frente do crescimento da renda. Financiada pelo
		// caixa do setor + poupança das famílias ACIMA da reserva de comida
		// (nunca pela poupança que paga comida — DJI, Q-004).
		const double finance_base = std::max(0.0, city.service_cash) + savings_surplus;
		const double affordable = finance_base > 0.0
			? finance_base
				/ (definition.capital_per_unit * kServiceCapitalReserve) : 0.0;
		if (affordable < definition.min_capacity) {
			continue;
		}
		const double capacity = std::max(definition.min_capacity,
			std::min(market.unmet * 1.25, affordable));
		const double revenue = capacity * market.price;
		double expected_cost = capacity * definition.jobs_per_unit * wage;
		for (const ServiceSector::Input& input : definition.inputs) {
			if (input.product >= 0 && input.product < ws_.products) {
				expected_cost += capacity * input.qty * city.markets[input.product].price;
			}
		}
		for (std::size_t slot = 0; slot < definition.service_inputs.size(); ++slot) {
			const int supplier = definition.service_inputs[slot];
			if (supplier >= 0 && supplier < kMaxServiceSectors) {
				expected_cost += capacity * definition.service_input_qty[slot]
					* city.service_markets[supplier].price;
			}
		}
		const double margin = revenue > 1e-9
			? (revenue - expected_cost) / revenue : -1.0;
		if (margin < kServiceEntryMargin) {
			continue;
		}
		const double capital = capacity * definition.capital_per_unit;
		if (service_open(city, sector, capacity, capital)) {
			++opens;
		}
	}
	for (const int index : ws_.service_by_city[city_index]) {
		ServiceBusiness& business =
			ws_.service_businesses[static_cast<std::size_t>(index)];
		if (business.bankrupt) {
			continue;
		}
		if (business.loss_days >= kServiceLossDays) {
			service_close(business);
		}
	}
	city.service_business_count = 0;
	for (const int index : ws_.service_by_city[city_index]) {
		if (!ws_.service_businesses[static_cast<std::size_t>(index)].bankrupt) {
			++city.service_business_count;
		}
	}
}
} // namespace ogs
