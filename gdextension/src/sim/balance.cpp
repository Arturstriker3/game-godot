// C1 — contabilidade por balanço (leitura). Ver balance.h e
// docs/economy/C1_balance_sheet.md. Nada aqui move dinheiro: só soma o estado.

#include "balance.h"

#include "city_gov.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>
#include <utility>

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

// C3 (D-107): dono de grupo de uma construção — a MESMA convenção textual de
// `SimCore::owner_kind` ("national:<g>" / "foreign:<g>@<n>"), sem o SimCore
// (o balanço é leitura pura). Vazio = não é de grupo.
std::string owner_group_id(const Building& building) {
	const std::string national = "national:";
	if (building.owner.rfind(national, 0) == 0) {
		return building.owner.substr(national.size());
	}
	const std::string foreign = "foreign:";
	if (building.owner.rfind(foreign, 0) == 0) {
		const std::string rest = building.owner.substr(foreign.size());
		const std::size_t at = rest.find('@');
		return at == std::string::npos ? rest : rest.substr(0, at);
	}
	return std::string();
}

} // namespace

Ledger ledger(const WorldState& ws, double money_total) {
	Ledger out;
	out.money_total = money_total;
	// C2b1 (D-104): o passivo LÍQUIDO do emissor é M0 menos os títulos que o
	// BC carrega. Comprar títulos do governo cria M0 (caixa público +) e dívida
	// pública (−), então a soma financeira do mundo continua 0; sem operação
	// (C1/C2a) `claims` é 0 e o passivo volta a ser o `money_total`.
	// C3 (D-107): o empréstimo do BC ao banco é ativo do emissor igual ao
	// título — entra na mesma conta (`bank_borrowing`).
	for (const auto& row : ws.monetary) {
		out.monetary_claims += row.second.securities;
	}
	for (const auto& row : ws.gov_bonds) {
		out.government_debt += row.second;
	}
	// C3: setor bancário. O capital pago pelo dono é passivo do banco e claim
	// do dono. C4 (D-110): o capital é separado por dono (Estado, famílias,
	// exterior) e o depósito à vista é passivo do banco (dinheiro bancário).
	for (const auto& row : ws.banks) {
		const Bank& bank = row.second;
		out.bank_cash += bank.cash;
		out.bank_loans += bank.loans;
		out.bank_arrears += bank.arrears;
		out.bank_borrowing += bank.bc_borrowing;
		out.bank_capital += bank.capital;
		out.bank_deposits += bank.deposits;
		if (bank.owner_kind == "state") {
			out.government_claims += bank.capital;
		} else if (bank.owner_kind == "foreign") {
			out.external_claims += bank.capital;
		} else {
			out.household_claims += bank.capital;
		}
	}
	out.banking = out.bank_cash + out.bank_loans + out.bank_arrears
		- out.bank_borrowing - out.bank_capital - out.bank_deposits;
	out.monetary_claims += out.bank_borrowing;
	out.monetary_base = money_total - out.monetary_claims
		- out.bank_deposits;

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
		out.arrears_service += city.service_wage_arrears;
		out.arrears_rural += city.rural_wage_arrears;

		// Governo: orçamento público da cidade (o "tesouro nacional" do modelo).
		out.government += city.government_budget;
	}
	// Atrasado por planta (o passivo individual do prédio).
	for (const Building& building : ws.buildings) {
		out.payables += building.wage_arrears;
		out.arrears_plants += building.wage_arrears;
	}
	// Caixas de setor e de grupo (dinheiro do mundo, como em money_total).
	for (const Group& group : ws.groups) {
		out.firms_cash += group.cash;
		// C3 (D-107): a dívida do grupo é passivo das firmas; o juro vencido
		// também (a receber do banco, do outro lado).
		out.group_debt += group.debt;
		out.group_arrears += group.credit_arrears;
	}
	for (const ConstructionProject& project : ws.construction_projects) {
		out.firms_cash += project.reserved;
	}
	out.firms_cash += ws.transport_cash + ws.construction_cash;
	// O caixa do setor financeiro ainda não existe (C2); o caixa do setor de
	// construção é privado.
	out.firms = out.firms_cash - out.payables - out.group_debt
		- out.group_arrears;

	// Governo: orçamento nacional por cidade + prefeituras (caixa e obras).
	out.government += city_gov::money_total(ws);
	// C4: o claim de capital do Estado já foi somado no laço dos bancos
	// (`government_claims`); NÃO entra em `government` (que é caixa) nem em
	// `cash_total`.

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
		// C3 (D-107): dívida de crédito do grupo (passivo) e juros vencidos.
		sheet.debt += group.debt + group.credit_arrears;
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
		sheet.cash += city.government_budget;
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
	// C2b1 (D-104): dívida pública emitida (títulos comprados pelo BC). É
	// passivo financeiro do governo — a contrapartida da emissão de M0.
	if (nation.empty()) {
		for (const auto& row : ws.gov_bonds) {
			sheet.debt += row.second;
		}
	} else {
		const auto found = ws.gov_bonds.find(nation);
		if (found != ws.gov_bonds.end()) {
			sheet.debt += found->second;
		}
	}
	// C3 (D-107): o capital pago ao banco é claim do Tesouro. C4 (D-110): só o
	// capital do banco PÚBLICO é claim do Estado; o do privado é das famílias e
	// o do estrangeiro é do exterior (ver `ledger`).
	if (nation.empty()) {
		for (const auto& row : ws.banks) {
			if (row.second.owner_kind == "state") {
				sheet.equity_claims += std::max(0.0, row.second.capital);
			}
		}
	} else {
		for (const auto& row : ws.banks) {
			if (row.second.nation == nation
					&& row.second.owner_kind == "state") {
				sheet.equity_claims += std::max(0.0, row.second.capital);
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

// C3 (D-107) — balanço do grupo. Ver o contrato em balance.h e o desenho em
// docs/economy/C3_group_balance.md §5. Nada aqui move dinheiro.
std::vector<GroupSheet> group_sheets(const WorldState& ws) {
	std::vector<GroupSheet> out(ws.groups.size());
	std::unordered_map<std::string, int> lookup;
	lookup.reserve(ws.groups.size());
	for (std::size_t index = 0; index < ws.groups.size(); ++index) {
		const Group& group = ws.groups[index];
		GroupSheet& sheet = out[index];
		sheet.id = group.id;
		sheet.name = group.name;
		sheet.origin_nation = group.origin_nation;
		sheet.sector = group.sector;
		sheet.cash = group.cash;
		sheet.debt = group.debt;
		sheet.payables = group.credit_arrears;
		lookup[group.id] = static_cast<int>(index);
	}
	// Capacidade das construções de grupo por cidade×produto (a MESMA regra de
	// rateio da receita: capacidade efetiva ÷ capacidade do produto).
	struct Cell {
		double total = 0.0;
		std::vector<std::pair<int, double>> parts;
	};
	std::map<long long, Cell> cells;
	for (const Building& building : ws.buildings) {
		const std::string gid = owner_group_id(building);
		if (gid.empty()) {
			continue;
		}
		const auto found = lookup.find(gid);
		if (found == lookup.end()) {
			continue;
		}
		const int index = found->second;
		const double partner_share = std::clamp(building.local_share, 0.0, 1.0);
		const double own_share = 1.0 - partner_share;
		const double book = building_book_value(building);
		const double cost = std::max(0.0, building.paid_wages)
			+ std::max(0.0, building.paid_opex);
		out[static_cast<std::size_t>(index)].fixed_capital += book * own_share;
		out[static_cast<std::size_t>(index)].daily_cost += cost * own_share;
		out[static_cast<std::size_t>(index)].daily_profit +=
			building.profit * own_share;
		out[static_cast<std::size_t>(index)].building_count += 1;
		if (partner_share > 0.0 && !building.partner_group.empty()) {
			const auto partner = lookup.find(building.partner_group);
			if (partner != lookup.end()) {
				GroupSheet& row = out[static_cast<std::size_t>(partner->second)];
				row.fixed_capital += book * partner_share;
				row.daily_cost += cost * partner_share;
				row.daily_profit += building.profit * partner_share;
			}
		}
		if (building.city_index < 0 || building.output_product < 0
				|| building.output_product >= kMaxProducts) {
			continue;
		}
		const double capacity = std::max(0.0, building.capacity)
			* std::max(0.0, building.output_scale);
		if (capacity <= 0.0) {
			continue;
		}
		Cell& cell = cells[static_cast<long long>(building.city_index)
			* kMaxProducts + building.output_product];
		cell.total += capacity;
		cell.parts.emplace_back(index, capacity);
	}
	for (const auto& row : cells) {
		const int city_index = static_cast<int>(row.first / kMaxProducts);
		const int product = static_cast<int>(row.first % kMaxProducts);
		if (city_index < 0
				|| city_index >= static_cast<int>(ws.cities.size())
				|| row.second.total <= 1e-12) {
			continue;
		}
		const City& city = ws.cities[static_cast<std::size_t>(city_index)];
		const double value = std::max(0.0, city.markets[product].stock)
			* price_of(ws, city, product);
		if (value <= 0.0) {
			continue;
		}
		for (const auto& part : row.second.parts) {
			GroupSheet& sheet = out[static_cast<std::size_t>(part.first)];
			sheet.inventory += value * part.second / row.second.total;
		}
	}
	return out;
}

GroupSheet group_sheet(const WorldState& ws, const Group& group) {
	const std::vector<GroupSheet> rows = group_sheets(ws);
	for (const GroupSheet& row : rows) {
		if (row.id == group.id) {
			return row;
		}
	}
	GroupSheet empty;
	empty.id = group.id;
	empty.name = group.name;
	empty.origin_nation = group.origin_nation;
	empty.sector = group.sector;
	empty.cash = group.cash;
	empty.debt = group.debt;
	empty.payables = group.credit_arrears;
	return empty;
}

Sheet banking(const WorldState& ws) {
	Sheet sheet;
	for (const auto& row : ws.banks) {
		const Bank& bank = row.second;
		sheet.cash += bank.cash;
		sheet.loans += bank.loans + bank.arrears;
		sheet.debt += bank.bc_borrowing + bank.capital + bank.deposits;
		sheet.deposits += bank.deposits;
	}
	return sheet;
}

Sheet banking_nation(const WorldState& ws, const std::string& nation) {
	Sheet sheet;
	for (const auto& row : ws.banks) {
		if (row.second.nation != nation) {
			continue;
		}
		const Bank& bank = row.second;
		sheet.cash += bank.cash;
		sheet.loans += bank.loans + bank.arrears;
		sheet.debt += bank.bc_borrowing + bank.capital + bank.deposits;
		sheet.deposits += bank.deposits;
	}
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
