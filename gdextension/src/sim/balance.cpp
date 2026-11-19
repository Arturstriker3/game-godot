// C1 — contabilidade por balanço (leitura). Ver balance.h e
// docs/economy/C1_balance_sheet.md. Nada aqui move dinheiro: só soma o estado.

#include "balance.h"

#include "city_gov.h"

#include <algorithm>
#include <cmath>

namespace ogs {
namespace balance {

namespace {

double price_of(const WorldState& ws, const City& city, int product) {
	if (product < 0 || product >= ws.products || product >= kMaxProducts) {
		return 0.0;
	}
	const double price = city.markets[product].price;
	return std::isfinite(price) && price > 0.0 ? price : 0.0;
}

// Estoque FÍSICO do mercado da cidade a preço de mercado (mercador/atacadista).
double city_inventory_value(const WorldState& ws, const City& city) {
	double value = 0.0;
	const int count = std::min(ws.products, static_cast<int>(kMaxProducts));
	for (int product = 0; product < count; ++product) {
		const double stock = std::max(0.0, city.markets[product].stock);
		value += stock * price_of(ws, city, product);
	}
	return value;
}

const std::string& nation_of_city(const WorldState& ws, int city_index) {
	static const std::string kEmpty;
	if (city_index < 0 || city_index >= static_cast<int>(ws.cities.size())) {
		return kEmpty;
	}
	return ws.cities[static_cast<std::size_t>(city_index)].nation;
}

double building_book_value(const Building& building) {
	return std::max(0.0, building.capital_per_capacity)
		* std::max(0.0, building.capacity);
}

} // namespace

Ledger ledger(const WorldState& ws, double money_total) {
	Ledger out;
	out.money_total = money_total;
	// Enquanto não há banco (C2), a moeda em circulação é o passivo da
	// autoridade monetária — placeholder declarado, não dinheiro novo.
	out.monetary_base = money_total;

	for (const City& city : ws.cities) {
		// Famílias: poupança dos quintis (o escalar da cidade é a soma), as
		// rendas pendentes (capital/aluguel creditados amanhã) e o caixa da
		// região (B3). O salário atrasado é um ATIVO financeiro.
		const double families_cash = city.pop.savings
			+ city.capital_income_next + city.rent_income_next
			+ city.region.cash;
		const double families_claims = city.pop.arrears_receivable
			+ city.region.arrears_receivable;
		out.families_cash += families_cash;
		out.receivables += families_claims;
		out.families += families_cash + families_claims;

		// Firmas: caixa das plantas e do setor de serviços; o salário atrasado
		// é PASSIVO delas (mesma obrigação do ativo acima).
		out.firms_cash += city.firm_cash + city.service_cash;
		out.payables += city.service_wage_arrears + city.rural_wage_arrears;
	}
	// Atrasado por planta (o passivo individual do prédio).
	for (const Building& building : ws.buildings) {
		out.payables += building.wage_arrears;
	}
	// Caixas de setor e de grupo (dinheiro do mundo, como em money_total).
	for (const Group& group : ws.groups) {
		out.firms_cash += group.cash;
	}
	for (const ConstructionProject& project : ws.construction_projects) {
		out.firms_cash += project.reserved;
	}
	out.firms_cash += ws.transport_cash + ws.construction_cash;
	// O caixa do setor financeiro ainda não existe (C2); o caixa do setor de
	// construção é privado.
	out.firms = out.firms_cash - out.payables;

	// Governo: orçamento nacional por cidade + prefeituras (caixa e obras).
	out.government = city_gov::money_total(ws);

	// Setor externo: `foreign_cash` pode ser negativo por desenho (o exterior
	// financia importação). É a posição do exterior, não dinheiro doméstico.
	out.external = ws.foreign_cash;
	return out;
}

Sheet families(const WorldState& ws, const std::string& nation) {
	Sheet sheet;
	for (const City& city : ws.cities) {
		if (!nation.empty() && city.nation != nation) {
			continue;
		}
		sheet.cash += city.pop.savings + city.capital_income_next
			+ city.rent_income_next + city.region.cash;
		sheet.receivables += city.pop.arrears_receivable
			+ city.region.arrears_receivable;
		// B6: terra + moradia do interior + anel periurbano, por quintil.
		const Region& region = city.region;
		if (region.property_ready) {
			for (int s = 0; s < kIncomeStrata; ++s) {
				sheet.property += std::max(0.0, region.stratum_land_value[s])
					+ std::max(0.0, region.stratum_home_value[s])
					+ std::max(0.0, region.stratum_ring_value[s]);
			}
		}
		// Duráveis das famílias (F3) a preço de mercado.
		const int count = std::min(ws.products,
			static_cast<int>(city.pop.durable_stock.size()));
		for (int product = 0; product < count; ++product) {
			const double tons = std::max(0.0, city.pop.durable_stock[
				static_cast<std::size_t>(product)]);
			if (tons > 0.0) {
				sheet.inventory += tons * price_of(ws, city, product);
			}
		}
	}
	return sheet;
}

Sheet firms(const WorldState& ws, const std::string& nation) {
	Sheet sheet;
	for (const City& city : ws.cities) {
		if (!nation.empty() && city.nation != nation) {
			continue;
		}
		sheet.cash += city.firm_cash + city.service_cash;
		sheet.payables += city.service_wage_arrears + city.rural_wage_arrears;
		// Estoque do mercado da cidade é do comerciante/firma local.
		sheet.inventory += city_inventory_value(ws, city);
	}
	for (const Building& building : ws.buildings) {
		if (!nation.empty()
				&& nation_of_city(ws, building.city_index) != nation) {
			continue;
		}
		sheet.payables += building.wage_arrears;
		sheet.fixed_capital += building_book_value(building);
	}
	for (const StorageSite& site : ws.storage_sites) {
		if (site.scope != STORAGE_PRIVATE) {
			continue;  // reserva pública é do governo
		}
		if (!nation.empty()
				&& nation_of_city(ws, site.city_index) != nation) {
			continue;
		}
		const int count = std::min({ws.products,
			static_cast<int>(site.stock.size()), static_cast<int>(kMaxProducts)});
		const City* city = site.city_index >= 0
			&& site.city_index < static_cast<int>(ws.cities.size())
			? &ws.cities[static_cast<std::size_t>(site.city_index)] : nullptr;
		if (city == nullptr) {
			continue;
		}
		for (int product = 0; product < count; ++product) {
			const double tons = std::max(0.0,
				site.stock[static_cast<std::size_t>(product)]);
			if (tons > 0.0) {
				sheet.inventory += tons * price_of(ws, *city, product);
			}
		}
	}
	for (const Group& group : ws.groups) {
		if (!nation.empty() && group.origin_nation != nation) {
			continue;
		}
		sheet.cash += group.cash;
	}
	return sheet;
}

Sheet government(const WorldState& ws, const std::string& nation) {
	Sheet sheet;
	const std::size_t gov_count = std::min(ws.cities.size(), ws.city_gov.size());
	for (std::size_t index = 0; index < ws.cities.size(); ++index) {
		const City& city = ws.cities[index];
		if (!nation.empty() && city.nation != nation) {
			continue;
		}
		sheet.cash += std::max(0.0, city.government_budget);
		if (index < gov_count) {
			const CityGov& gov = ws.city_gov[index];
			sheet.cash += std::max(0.0, gov.cash);
			for (int slot = 0; slot < gov.project_count; ++slot) {
				sheet.cash += std::max(0.0, gov.projects[slot].reserved);
			}
		}
		// Estoque de armazém do GOVERNO (reserva nacional da G2) é ativo real.
		for (const StorageSite& site : ws.storage_sites) {
			if (site.scope != STORAGE_GOVERNMENT || site.city_index
					!= static_cast<int>(index)) {
				continue;
			}
			const int count = std::min({ws.products,
				static_cast<int>(site.stock.size()),
				static_cast<int>(kMaxProducts)});
			for (int product = 0; product < count; ++product) {
				const double tons = std::max(0.0,
					site.stock[static_cast<std::size_t>(product)]);
				if (tons > 0.0) {
					sheet.inventory += tons * price_of(ws, city, product);
				}
			}
		}
	}
	return sheet;
}

Sheet external(const WorldState& ws) {
	Sheet sheet;
	sheet.cash = ws.foreign_cash;
	return sheet;
}

FirmSolvency firm_solvency(const WorldState& ws) {
	FirmSolvency out;
	for (const Building& building : ws.buildings) {
		++out.count;
		const double assets = building_book_value(building);
		const double net = assets - std::max(0.0, building.wage_arrears);
		if (net < 0.0) {
			++out.insolvent;
			continue;
		}
		// Ilíquida: o patrimônio cobre, mas não dá 30 dias do custo efetivamente
		// pago (folha + opex do último dia).
		const double monthly_cost = 30.0
			* (std::max(0.0, building.paid_wages)
				+ std::max(0.0, building.paid_opex));
		if (monthly_cost > 0.0 && net < monthly_cost) {
			++out.illiquid;
			continue;
		}
		++out.liquid;
	}
	return out;
}

double marketable_assets(const WorldState& ws, std::size_t city_index,
		const Building& building, double price, double standing_value) {
	// Mesma matemática da D-099: (estoque do mercado + safra em pé) × preço ×
	// fatia da firma na capacidade do par cidade×produto.
	if (price <= 0.0) {
		return 0.0;
	}
	const double share = firm_production_share(ws, city_index, building);
	if (share <= 0.0) {
		return 0.0;
	}
	const City& city = ws.cities[city_index];
	const double stock = std::max(0.0,
		city.markets[building.output_product].stock);
	return (stock + std::max(0.0, standing_value)) * price * share;
}

double firm_production_share(const WorldState& ws, std::size_t city_index,
		const Building& building) {
	if (city_index >= ws.cities.size()
			|| city_index >= ws.buildings_by_city.size()
			|| building.output_product < 0
			|| building.output_product >= ws.products
			|| building.output_product >= kMaxProducts) {
		return 0.0;
	}
	double total_capacity = 0.0;
	for (const int index : ws.buildings_by_city[city_index]) {
		const Building& other = ws.buildings[static_cast<std::size_t>(index)];
		if (other.recipe < 0 && other.input_product < 0
				&& other.output_product == building.output_product) {
			total_capacity += other.capacity * other.output_scale;
		}
	}
	if (total_capacity <= 1e-12) {
		return 0.0;
	}
	const double own = building.capacity * building.output_scale;
	return std::clamp(own / total_capacity, 0.0, 1.0);
}

} // namespace balance
} // namespace ogs
