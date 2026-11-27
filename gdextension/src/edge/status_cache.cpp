#include "status_cache.h"

#include "economy_sim.h"
#include "sim/balance.h"
#include "sim/profiling.h"
#include "sim/sim_constants.h"
#include "sim/stock.h"

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_float64_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace godot {

using namespace ogs;

void EconomySim::fill_light_signals(StatusCache& cache) const {
	const StatusKeys& k = status_keys_;
	const std::size_t cities = ws_.cities.size();
	const std::size_t products = static_cast<std::size_t>(ws_.products);
	const std::size_t market_len = cities * products;

	// Mercados: 7 campos que a IA lÃª (preÃ§o Ã¢ncora, estoque, oferta, excedente,
	// vendido, escassez, desejado). SÃ£o os MESMOS doubles do bloco pesado por
	// cidade (`source.price` etc. em fill_city_status) â€” bit a bit.
	cache.light_m_price.resize(static_cast<int>(market_len));
	cache.light_m_stock.resize(static_cast<int>(market_len));
	cache.light_m_supply.resize(static_cast<int>(market_len));
	cache.light_m_surplus.resize(static_cast<int>(market_len));
	cache.light_m_sold.resize(static_cast<int>(market_len));
	cache.light_m_shortage.resize(static_cast<int>(market_len));
	cache.light_m_desired.resize(static_cast<int>(market_len));
	double* lm_price = cache.light_m_price.ptrw();
	double* lm_stock = cache.light_m_stock.ptrw();
	double* lm_supply = cache.light_m_supply.ptrw();
	double* lm_surplus = cache.light_m_surplus.ptrw();
	double* lm_sold = cache.light_m_sold.ptrw();
	double* lm_shortage = cache.light_m_shortage.ptrw();
	double* lm_desired = cache.light_m_desired.ptrw();

	// POP: 5 escalares que a IA lÃª por cidade (necessidade alimentar, consumo,
	// satisfaÃ§Ã£o alimentar, forÃ§a de trabalho e desemprego).
	cache.light_p_need.resize(static_cast<int>(cities));
	cache.light_p_consumed.resize(static_cast<int>(cities));
	cache.light_p_satisfaction.resize(static_cast<int>(cities));
	cache.light_p_labor_force.resize(static_cast<int>(cities));
	cache.light_p_unemployed.resize(static_cast<int>(cities));
	double* lp_need = cache.light_p_need.ptrw();
	double* lp_consumed = cache.light_p_consumed.ptrw();
	double* lp_satisfaction = cache.light_p_satisfaction.ptrw();
	double* lp_labor_force = cache.light_p_labor_force.ptrw();
	double* lp_unemployed = cache.light_p_unemployed.ptrw();

	for (std::size_t city_index = 0; city_index < cities; ++city_index) {
		const City& city = ws_.cities[city_index];
		const std::size_t base = city_index * products;
		for (int product = 0; product < ws_.products; ++product) {
			const Market& market = city.markets[product];
			const std::size_t at = base + static_cast<std::size_t>(product);
			lm_price[at] = market.price;
			lm_stock[at] = market.stock;
			lm_supply[at] = market.supply;
			lm_surplus[at] = market.surplus;
			lm_sold[at] = market.sold;
			lm_shortage[at] = market.shortage;
			lm_desired[at] = market.desired;
		}
		lp_need[city_index] = city.pop.need;
		lp_consumed[city_index] = city.pop.consumed;
		lp_satisfaction[city_index] = city.pop.satisfaction;
		lp_labor_force[city_index] = city.pop.labor_force;
		lp_unemployed[city_index] = city.pop.unemployed;
	}

	// Incumbentes (F9): pares (cidade, recurso) com dono-grupo â€” o que o
	// group_investor montava varrendo city["buildings"] de TODAS as cidades.
	// Mesmo filtro do GDScript: owner_group e resource nÃ£o-vazios. Ordem:
	// cidade â†’ construÃ§Ã£o (buildings_by_city), idÃªntica ao Array do bloco pesado.
	cache.light_i_city.clear();
	cache.light_i_resource.clear();
	cache.light_i_owner_group.clear();
	for (std::size_t city_index = 0; city_index < cities; ++city_index) {
		for (const int bi : ws_.buildings_by_city[city_index]) {
			const Building& building =
				ws_.buildings[static_cast<std::size_t>(bi)];
			std::string group_id;
			std::string origin_nation;
			owner_kind(building.owner, &group_id, &origin_nation);
			if (group_id.empty() || building.resource.empty()) {
				continue;
			}
			cache.light_i_city.push_back(static_cast<int32_t>(city_index));
			cache.light_i_resource.push_back(String(building.resource.c_str()));
			cache.light_i_owner_group.push_back(String(group_id.c_str()));
		}
	}

	// ConstruÃ§Ãµes (CSR): o que a IA por cidade lÃª â€” produto de saÃ­da, dono,
	// capacidade, utilizaÃ§Ã£o, custos e aptidÃ£o do sÃ­tio. SÃ£o os MESMOS valores
	// do bloco pesado (fill_city_status), na ordem cidade â†’ construÃ§Ã£o
	// (buildings_by_city). `site_suitability` = suitability do produto de saÃ­da.
	cache.light_b_offset.clear();
	cache.light_b_id.clear();
	cache.light_b_output_product.clear();
	cache.light_b_owner.clear();
	cache.light_b_capacity.clear();
	cache.light_b_utilization.clear();
	cache.light_b_costs.clear();
	cache.light_b_site_suitability.clear();
	cache.light_b_offset.push_back(0);
	for (std::size_t city_index = 0; city_index < cities; ++city_index) {
		const City& bcity = ws_.cities[city_index];
		for (const int bi : ws_.buildings_by_city[city_index]) {
			const Building& building =
				ws_.buildings[static_cast<std::size_t>(bi)];
			cache.light_b_id.push_back(String(building.id.c_str()));
			cache.light_b_output_product.push_back(building.output_product);
			cache.light_b_owner.push_back(String(building.owner.c_str()));
			cache.light_b_capacity.push_back(building.capacity);
			cache.light_b_utilization.push_back(building.utilization);
			cache.light_b_costs.push_back(building.costs);
			cache.light_b_site_suitability.push_back(
				bcity.suitability[building.output_product]);
		}
		cache.light_b_offset.push_back(
			static_cast<int32_t>(cache.light_b_output_product.size()));
	}

	// Jazidas (CSR): o que a IA por cidade lÃª â€” recurso (nome do produto),
	// reserva restante e fator de profundidade, mais o bloco estÃ¡tico completo
	// para a tabela ser um substituto do dicionÃ¡rio pesado. Mesmas jazidas
	// rastreadas do bloco pesado (deposit.tracked), ordem de produto.
	cache.light_d_offset.clear();
	cache.light_d_product.clear();
	cache.light_d_resource.clear();
	cache.light_d_initial_kt.clear();
	cache.light_d_remaining_kt.clear();
	cache.light_d_quality.clear();
	cache.light_d_depth_m.clear();
	cache.light_d_efficiency.clear();
	cache.light_d_opex_factor.clear();
	cache.light_d_extracted_kt.clear();
	cache.light_d_depleted.clear();
	cache.light_d_offset.push_back(0);
	for (std::size_t city_index = 0; city_index < cities; ++city_index) {
		const City& dcity = ws_.cities[city_index];
		for (int product = 0; product < ws_.products; ++product) {
			const Deposit& deposit = dcity.deposits[product];
			if (!deposit.tracked) {
				continue;
			}
			cache.light_d_product.push_back(product);
			cache.light_d_resource.push_back(String(product_name(product)));
			cache.light_d_initial_kt.push_back(deposit.initial_kt);
			cache.light_d_remaining_kt.push_back(deposit.remaining_kt);
			cache.light_d_quality.push_back(deposit.quality);
			cache.light_d_depth_m.push_back(deposit.depth_m);
			cache.light_d_efficiency.push_back(extraction_efficiency(deposit));
			cache.light_d_opex_factor.push_back(depth_opex_factor(deposit));
			cache.light_d_extracted_kt.push_back(deposit.extracted_kt);
			// D-086 (decisÃ£o do dono): o recurso nunca acaba â€” `d_depleted` Ã©
			// SEMPRE 0 (campo mantido por contrato do bloco `light`).
			cache.light_d_depleted.push_back(0);
		}
		cache.light_d_offset.push_back(
			static_cast<int32_t>(cache.light_d_product.size()));
	}

	// Obras em andamento (CSR, Â§15): o que a IA lÃª para nÃ£o reconstruir a
	// mesma lacuna enquanto uma obra estÃ¡ em voo (direÃ§Ã£o de calibraÃ§Ã£o do
	// D-060). Ordem cidade â†’ obra (ordem de criaÃ§Ã£o do vetor
	// construction_projects), determinÃ­stica. `c_expand` = 1 marca expansÃ£o de
	// prÃ©dio existente (o produto JÃ existe na cidade); 0 Ã© obra nova.
	cache.light_c_offset.clear();
	cache.light_c_output_product.clear();
	cache.light_c_capacity.clear();
	cache.light_c_expand.clear();
	cache.light_c_offset.push_back(0);
	for (std::size_t city_index = 0; city_index < cities; ++city_index) {
		const std::string& ocity = ws_.cities[city_index].id;
		for (const ConstructionProject& project : ws_.construction_projects) {
			if (project.city != ocity) {
				continue;
			}
			cache.light_c_output_product.push_back(project.output_product);
			cache.light_c_capacity.push_back(project.capacity);
			cache.light_c_expand.push_back(project.expand_existing ? 1 : 0);
		}
		cache.light_c_offset.push_back(
			static_cast<int32_t>(cache.light_c_output_product.size()));
	}

	// Dictionary Ã© por referÃªncia: pendura os arrays (o `ptrw` desanexa o
	// buffer por copy-on-write, entÃ£o reassocia a cada dia) e sÃ³ cria as
	// chaves escalares uma vez.
	Dictionary& light = cache.light;
	if (!cache.light_filled) {
		light[k.product_count] = static_cast<int>(products);
		light[k.city_count] = static_cast<int>(cities);
		cache.light_filled = true;
	}
	light[k.m_price] = cache.light_m_price;
	light[k.m_stock] = cache.light_m_stock;
	light[k.m_supply] = cache.light_m_supply;
	light[k.m_surplus] = cache.light_m_surplus;
	light[k.m_sold] = cache.light_m_sold;
	light[k.m_shortage] = cache.light_m_shortage;
	light[k.m_desired] = cache.light_m_desired;
	light[k.p_need] = cache.light_p_need;
	light[k.p_consumed] = cache.light_p_consumed;
	light[k.p_satisfaction] = cache.light_p_satisfaction;
	light[k.p_labor_force] = cache.light_p_labor_force;
	light[k.p_unemployed] = cache.light_p_unemployed;
	light[k.i_city] = cache.light_i_city;
	light[k.i_resource] = cache.light_i_resource;
	light[k.i_owner_group] = cache.light_i_owner_group;
	light[k.b_offset] = cache.light_b_offset;
	light[k.b_id] = cache.light_b_id;
	light[k.b_output_product] = cache.light_b_output_product;
	light[k.b_owner] = cache.light_b_owner;
	light[k.b_capacity] = cache.light_b_capacity;
	light[k.b_utilization] = cache.light_b_utilization;
	light[k.b_costs] = cache.light_b_costs;
	light[k.b_site_suitability] = cache.light_b_site_suitability;
	light[k.d_offset] = cache.light_d_offset;
	light[k.d_product] = cache.light_d_product;
	light[k.d_resource] = cache.light_d_resource;
	light[k.d_initial_kt] = cache.light_d_initial_kt;
	light[k.d_remaining_kt] = cache.light_d_remaining_kt;
	light[k.d_quality] = cache.light_d_quality;
	light[k.d_depth_m] = cache.light_d_depth_m;
	light[k.d_efficiency] = cache.light_d_efficiency;
	light[k.d_opex_factor] = cache.light_d_opex_factor;
	light[k.d_extracted_kt] = cache.light_d_extracted_kt;
	light[k.d_depleted] = cache.light_d_depleted;
	light[k.c_offset] = cache.light_c_offset;
	light[k.c_output_product] = cache.light_c_output_product;
	light[k.c_capacity] = cache.light_c_capacity;
	light[k.c_expand] = cache.light_c_expand;
}

// ---------------------------------------------------------------------------
// A4: pedaÃ§os do status extraÃ­dos para as consultas sob demanda reusarem o
// MESMO cÃ³digo (mesmos valores, mesma ordem de chaves). Movidos de
// `get_status()` sem mudanÃ§a de lÃ³gica.
// ---------------------------------------------------------------------------

void EconomySim::build_ports(StatusCache& cache) {
	const StatusKeys& k = status_keys_;
	cache.ports.resize(ws_.ports.size());
	cache.port_slots.resize(ws_.ports.size());
	for (std::size_t index = 0; index < ws_.ports.size(); ++index) {
		const Port& port = ws_.ports[index];
		Dictionary& dict = cache.port_slots[index];
		dict[k.id] = String(port.id.c_str());
		dict[k.name] = String(port.name.c_str());
		dict[k.nation] = String(port.nation.c_str());
		dict[k.x] = port.center.x;
		dict[k.y] = port.center.y;
		dict[k.landmass] = String(port.landmass.c_str());
		// Capacidade/fila (port_design.md Â§2): o porto serve a naÃ§Ã£o toda;
		// `host_city` Ã© sÃ³ a sede (Ã¢ncora e base da capacidade).
		dict[k.host_city] = String(port.host_city.c_str());
		const double base = std::max(1.0, port.base_capacity_t_per_day);
		dict[k.level] = std::max(1, static_cast<int>(std::ceil(
			port.capacity_t_per_day / base)));
		dict[k.capacity_t_per_day] = port.capacity_t_per_day;
		dict[k.throughput] = port.used_today_t;
		dict[k.queue] = port.queue_t;
		dict[k.delay_hours] = port.delay_hours;
		dict[k.utilization] = port.capacity_t_per_day > 0.0
			? port.used_today_t / port.capacity_t_per_day : 0.0;
		cache.ports[index] = dict;
	}
}

void EconomySim::build_orders(StatusCache& cache) {
	const StatusKeys& k = status_keys_;
	cache.orders.resize(ws_.last_orders.size());
	cache.order_slots.resize(ws_.last_orders.size());
	cache.order_leg_slots.resize(ws_.last_orders.size());
	if (cache.order_leg_entry.size() < cache.order_slots.size()) {
		cache.order_leg_entry.resize(cache.order_slots.size());
	}
	for (std::size_t index = 0; index < ws_.last_orders.size(); ++index) {
		const TradeOrder& order = ws_.last_orders[index];
		Dictionary& dict = cache.order_slots[index];
		dict[k.from_city] = String(order.from_city.c_str());
		dict[k.to_city] = String(order.to_city.c_str());
		dict[k.from_nation] = String(order.from_nation.c_str());
		dict[k.to_nation] = String(order.to_nation.c_str());
		dict[k.scope] = String(scope_name(order.scope));
		dict[k.origin_price] = order.origin_price;
		dict[k.origin_price_anchor] = order.origin_price_anchor;
		dict[k.tariff] = order.tariff;
		dict[k.delivered_price] = order.delivered_price;
		dict[k.fx_rate] = order.fx_rate;
		dict[k.seller_fx_rate] = order.seller_fx_rate;
		dict[k.buyer_currency] = String(order.buyer_currency.c_str());
		dict[k.seller_currency] = String(order.seller_currency.c_str());
		dict[k.good] = order.product >= 0 && order.product < ws_.products
			? String(product_name(order.product)) : String("wheat");
		dict[k.amount] = order.amount;
		dict[k.transit_hours] = order.transit_hours;
		dict[k.freight_cost] = order.freight_cost;
		Array& legs = cache.order_leg_slots[index];
		legs.resize(order.legs.size());
		std::vector<Dictionary>& leg_entries = cache.order_leg_entry[index];
		if (leg_entries.size() < order.legs.size()) {
			leg_entries.resize(order.legs.size());
		}
		for (std::size_t leg_index = 0; leg_index < order.legs.size(); ++leg_index) {
			Dictionary& leg_dict = leg_entries[leg_index];
			fill_leg_status(leg_dict, order.legs[leg_index]);
			legs[leg_index] = leg_dict;
		}
		dict[k.legs] = legs;
		cache.orders[index] = dict;
	}
}

void EconomySim::build_currencies(StatusCache& cache) {
	const StatusKeys& k = status_keys_;
	cache.currencies.resize(static_cast<int>(ws_.currencies.size()));
	for (std::size_t index = 0; index < ws_.currencies.size(); ++index) {
		const Currency& currency = ws_.currencies[index];
		Dictionary entry;
		entry[k.code] = String(currency.code.c_str());
		entry[k.rate] = currency.rate;
		entry[k.regime] = currency.floating ? "float" : "peg";
		entry[k.fallback] = currency.fallback;
		entry[k.trade_balance] = currency.balance_ema;
		entry[k.source] = String(currency.source.c_str());
		cache.currencies[static_cast<int>(index)] = entry;
	}
}

void EconomySim::build_tariffs(StatusCache& cache) {
	const StatusKeys& k = status_keys_;
	cache.tariffs.resize(static_cast<int>(ws_.tariffs.size()));
	for (std::size_t index = 0; index < ws_.tariffs.size(); ++index) {
		const Tariff& tariff = ws_.tariffs[index];
		Dictionary entry;
		entry[k.buyer] = String(tariff.buyer.c_str());
		entry[k.seller] = String(tariff.seller.c_str());
		entry[k.product] = String(tariff.product.c_str());
		entry[k.rate] = tariff.rate;
		cache.tariffs[static_cast<int>(index)] = entry;
	}
}

Dictionary EconomySim::get_status() {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	// Snapshot do dia jÃ¡ publicado: devolve o mesmo dicionÃ¡rio (a UI lÃª o dia
	// inteiro; mutaÃ§Ãµes entre ticks sÃ³ valem no prÃ³ximo status).
	if (status_live_ >= 0 && status_built_day_ == ws_.day) {
		return status_slots_[status_live_].root;
	}
	const int slot_index = status_live_ < 0 ? 0 : 1 - status_live_;
	StatusCache& cache = status_slots_[slot_index];
	const StatusKeys& k = status_keys_;
	Dictionary& root = cache.root;

	// Subdivisao da fase status na medicao por fase (sem custo no jogo): as 4 marcas
	// `OGS_SIM_PHASE_LAP(part)` abaixo gravam em `status_sub_us_`.
#if defined(OGS_PROFILING)
	ogs::profile::Sink status_sink{status_sub_us_, 4, &profile_};
	const ogs::profile::SinkScope status_scope(&status_sink);
#endif

	root[k.mode] = "demo";
	root[k.day] = ws_.day;
	root[k.calendar_day] = ws_.day;
	// R1b/R1c: configuracao invalida das flags do rework exposta no status (a
	// chave so existe quando ha erro - OFF continua sem chave nova).
	if (!ws_.treasury_config_error.empty()) {
		root["treasury_config_error"] = String(
			ws_.treasury_config_error.c_str());
	}
	if (!ws_.firms_config_error.empty()) {
		root["firms_own_cash_config_error"] = String(
			ws_.firms_config_error.c_str());
	}
	// G2: diagnóstico da semente (critério 9: residual e falta publicados).
	if (ws_.bootstrap.loaded) {
		Dictionary bootstrap;
		bootstrap["enabled"] = ws_.bootstrap.enabled;
		bootstrap["plants"] = ws_.bootstrap_plants;
		bootstrap["shortfall"] = ws_.bootstrap_shortfall;
		bootstrap["unseeded_share"] = ws_.bootstrap_unseeded_share;
		root["bootstrap"] = bootstrap;
	}
	root[k.money_total] = money_total();
	// D-126: dinheiro bancário criado/destruído (M1) acumulado nos dias válidos.
	// `money_total` pode crescer por crédito legítimo; os guardas de conservação
	// somam este contador ao total inicial em vez de exigir constância.
	root[k.bank_money_delta_total] = ws_.bank_money_delta_total;
	root[k.transport_cash] = ws_.transport_cash;
	root[k.foreign_cash] = ws_.foreign_cash;
	root[k.construction_cash] = ws_.construction_cash;
	// C1 (D-102): identidade financeira do dia. `financial_sum` Ã© o novo
	// invariante (tem de ser 0); `cash_residual` Ã© a checagem cruzada entre a
	// enumeraÃ§Ã£o do balanÃ§o e o `money_total` acima. O detalhe por agente sai
	// sob demanda em `get_balance_status()` (A4).
	{
		const ogs::balance::Ledger book = ogs::balance::ledger(ws_,
			money_total());
		Dictionary balance;
		balance["money_total"] = book.money_total;
		balance["cash_total"] = book.cash_total();
		balance["cash_residual"] = book.cash_residual();
		balance["receivables"] = book.receivables;
		balance["payables"] = book.payables;
		balance["arrears_plants"] = book.arrears_plants;
		balance["arrears_service"] = book.arrears_service;
		balance["arrears_rural"] = book.arrears_rural;
		balance["monetary_base"] = book.monetary_base;
		balance["monetary_claims"] = book.monetary_claims;
		balance["government_debt"] = book.government_debt;
		// C3 (D-107): o crÃ©dito de empresa Ã© zero no default do jogo; os dois
		// nÃºmeros ficam no resumo diÃ¡rio para o painel/diagnÃ³stico nÃ£o precisar
		// montar o status inteiro.
		balance["group_debt"] = book.group_debt;
		balance["bank_loan_assets"] = book.bank_loans;
		// C5 (D-112): aÃ§Ã£o dos dois lados (carteiras = emissores) pelo mesmo
		// valor de mercado. ValorizaÃ§Ã£o nÃ£o cria dinheiro; o dividendo sim.
		balance["stock_assets"] = book.stock_assets;
		balance["stock_claims"] = book.stock_claims;
		balance["financial_sum"] = book.financial_sum();
		balance["ok"] = std::abs(book.financial_sum()) <= 1e-3
			&& std::abs(book.cash_residual()) <= 1e-3;
		root["balance"] = balance;
	}
	// C5 (D-112): resumo barato da bolsa (Ã­ndice global e confianÃ§a mediana).
	{
		const ogs::stock::Policy pol = ogs::stock::policy(ws_);
		Dictionary stock;
		stock["enabled"] = pol.enabled;
		double cap = 0.0;
		double base_cap = 0.0;
		double dividends_today = 0.0;
		int listed = 0;
		std::vector<double> confidences;
		confidences.reserve(ws_.stock_exchanges.size());
		for (const auto& row : ws_.stock_exchanges) {
			base_cap += std::max(0.0, row.second.index_base);
			confidences.push_back(row.second.confidence);
		}
		for (const auto& row : ws_.stock_tickers) {
			if (!row.second.listed) {
				continue;
			}
			listed += 1;
			cap += ogs::stock::market_cap(row.second);
			if (row.second.dividend_day == ws_.day) {
				dividends_today += row.second.dividend_last;
			}
		}
		double confidence_p50 = 1.0;
		if (!confidences.empty()) {
			std::sort(confidences.begin(), confidences.end());
			confidence_p50 = confidences[confidences.size() / 2];
		}
		stock["index_global"] = base_cap > 0.0
			? cap / base_cap * 100.0 : 100.0;
		stock["market_cap"] = cap;
		stock["listed"] = listed;
		stock["confidence_p50"] = confidence_p50;
		stock["dividends_today"] = dividends_today;
		root["stock"] = stock;
	}
	root[k.bankruptcies] = ws_.bankruptcies;
	root[k.depleted_mines] = ws_.depleted_mines;
	// B4: totais de migraÃ§Ã£o do mundo (acumulados) + urbanizaÃ§Ã£o CORRENTE.
	// O urbano vivo Ã© `simulation_population` (Î£ coortes, acompanha demografia e
	// migraÃ§Ã£o); a regiÃ£o viva Ã© `region.region_pop` (Î£ coortes do interior).
	// `region.urban_population`/`region.population_region` sÃ£o a SEED do dia 0 e
	// nÃ£o podem ser usados aqui.
	{
		double rural_in = 0.0;
		double city_in = 0.0;
		double city_out = 0.0;
		double urban = 0.0;
		double region_pop = 0.0;
		for (const City& city : ws_.cities) {
			const Region& r = city.region;
			rural_in += r.migration_rural_in_total;
			city_in += r.migration_city_in_total;
			city_out += r.migration_city_out_total;
			urban += city.simulation_population;
			region_pop += r.region_pop;
		}
		Dictionary migration;
		migration["rural_in_total"] = rural_in;
		migration["city_in_total"] = city_in;
		migration["city_out_total"] = city_out;
		migration["urban_population"] = urban;
		migration["region_population"] = region_pop;
		migration["urbanization"] = (urban + region_pop) > 0.0
			? urban / (urban + region_pop) : 0.0;
		root[k.migration] = migration;
	}
	// F8: catÃ¡logo de serviÃ§os (uma vez) + contadores de negÃ³cios do dia.
	if (ws_.services_loaded && !cache.service_sectors_filled) {
		cache.service_sectors = get_service_sectors();
		root[k.service_sectors] = cache.service_sectors;
		cache.service_sectors_filled = true;
	}
	root[k.service_businesses] = get_service_business_count();
	root[k.service_bankruptcies] = ws_.service_bankruptcies;

	// CabeÃ§alho: os contÃªineres sÃ£o pendurados no dicionÃ¡rio UMA vez; a partir
	// daÃ­ sÃ³ sÃ£o redimensionados/reescritos no lugar.
	if (!cache.header_filled) {
		cache.products.resize(ws_.products);
		for (int index = 0; index < ws_.products; ++index) {
			cache.products[index] = String(product_name(index));
		}
		cache.tot_produced.resize(ws_.products);
		cache.tot_consumed.resize(ws_.products);
		cache.tot_produced_today.resize(ws_.products);
		cache.tot_consumed_today.resize(ws_.products);
		root[k.capital_cost_per_capacity] = kCapitalPerCapacity;
		root[k.products] = cache.products;
		root[k.cities] = cache.cities;
		root[k.ports] = cache.ports;
		root[k.trade_orders] = cache.orders;
		root[k.nations] = cache.nations;
		root[k.currencies] = cache.currencies;
		root[k.tariffs] = cache.tariffs;
		root[k.invariant_violations] = cache.violations;
		root[k.market_totals] = cache.market_totals;
		root[k.produced_total] = cache.tot_produced;
		root[k.consumed_total] = cache.tot_consumed;
		root[k.produced_today] = cache.tot_produced_today;
		root[k.consumed_today] = cache.tot_consumed_today;
		root[k.light] = cache.light;
		cache.header_filled = true;
	}

	cache.violations.resize(ws_.invariant_violations.size());
	for (std::size_t index = 0; index < ws_.invariant_violations.size(); ++index) {
		cache.violations[index] = String(ws_.invariant_violations[index].c_str());
	}

	// Carga em trÃ¢nsito por cidade numa passada Ãºnica. Antes, cada cidade
	// varria TODAS as cargas do mundo (O(cidades Ã— cargas) por dia).
	// A4: mesma passada Ãºnica do status, agora em `compute_in_transit`.
	compute_in_transit(cache.in_transit_by_city);
	OGS_SIM_PHASE_LAP(3);

	// Totais do sistema por produto â€” a mesma soma que a IA fazia em GDScript
	// sobre `cities[]` (858 cidades Ã— 21 mercados, ~13 ms no frame da virada).
	// Aqui Ã© uma passada de doubles na memÃ³ria do nÃºcleo, na thread do tick.
	cache.tot_surplus.resize(ws_.products);
	cache.tot_sold.resize(ws_.products);
	cache.tot_demand.resize(ws_.products);
	for (int product = 0; product < ws_.products; ++product) {
		cache.tot_surplus[product] = 0.0;
		cache.tot_sold[product] = 0.0;
		cache.tot_demand[product] = 0.0;
	}
	// Soma sÃ³ os valores positivos, na ordem das cidades: idÃªntico (bit a bit)
	// ao laÃ§o que o `PrivateInvestor.prepare_totals` fazia.
	for (const City& city : ws_.cities) {
		for (int product = 0; product < ws_.products; ++product) {
			const Market& market = city.markets[product];
			if (market.surplus > 0.0) {
				cache.tot_surplus[product] += market.surplus;
			}
			if (market.sold > 0.0) {
				cache.tot_sold[product] += market.sold;
			}
			// F3: a IA usa este agregado como "existe demanda corrente no
			// sistema". Com bens finais ainda sem produtor, `demand` (compras
			// realizadas) Ã© zero e sÃ³ `desired` carrega a necessidade das POPs â€”
			// publicar `desired` permite a fÃ¡brica nascer onde hÃ¡ demanda real.
			if (market.desired > 0.0) {
				cache.tot_demand[product] += market.desired;
			}
		}
	}
	// Packed arrays sÃ£o COW: depois de mutar o buffer da cache, o dicionÃ¡rio
	// precisa receber a referÃªncia de novo (senÃ£o o consumidor lÃª o dia anterior).
	cache.market_totals[k.surplus] = cache.tot_surplus;
	cache.market_totals[k.sold] = cache.tot_sold;
	cache.market_totals[k.demand] = cache.tot_demand;

	// Sinais leves por cidade (S7-c): arrays planos para a varredura diÃ¡ria da
	// IA (mercados, POP e incumbentes) â€” o detalhe pesado por cidade fica sob
	// demanda em get_city_status(id) na prÃ³xima fatia.
	fill_light_signals(cache);

	cache.cities.resize(ws_.cities.size());
	cache.city_slots.resize(ws_.cities.size());
	for (std::size_t city_index = 0; city_index < ws_.cities.size(); ++city_index) {
		StatusCitySlot& slot = cache.city_slots[city_index];
		fill_city_status(city_index, slot,
			cache.in_transit_by_city[city_index], false);
		cache.cities[city_index] = slot.dict;
	}
	OGS_SIM_PHASE_LAP(0);

	// Portos (modo mundo) para o minimapa e o mundo 3D. A4: o mesmo cÃ³digo
	// atende `get_ports()`.
	build_ports(cache);

	// Pedidos de comÃ©rcio do Ãºltimo tick (comboios 3D e HUD). A4: o mesmo
	// cÃ³digo atende `get_trade_orders()`.
	build_orders(cache);
	OGS_SIM_PHASE_LAP(1);

	// NaÃ§Ãµes agregadas (telas de polÃ­tica/economia). A4: o mesmo cÃ³digo
	// atende `get_nation_rows()`/`get_nation_status(tag)`.
	if (labor_snapshot_day_ < 0) {
		capture_nation_labor();
	}
	build_nations(cache, cache.in_transit_by_city);

	for (int index = 0; index < ws_.products; ++index) {
		cache.tot_produced[index] = ws_.produced[index];
		cache.tot_consumed[index] = ws_.consumed_total[index];
		cache.tot_produced_today[index] = ws_.produced_today[index];
		cache.tot_consumed_today[index] = ws_.consumed_today[index];
	}
	// F5: moedas e tarifas para a UI da F6. A4: o mesmo cÃ³digo atende
	// `get_currencies()`/`get_tariffs()`.
	build_currencies(cache);
	build_tariffs(cache);
	OGS_SIM_PHASE_LAP(2);
	status_live_ = slot_index;
	status_built_day_ = ws_.day;
	return root;
}
} // namespace godot
