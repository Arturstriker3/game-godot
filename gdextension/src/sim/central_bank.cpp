// C2a — banco central por moeda: medição do dia (leitura). Ver central_bank.h
// e docs/economy/C2_central_bank.md (D-103). Nada aqui move dinheiro.

#include "central_bank.h"

#include "city_gov.h"
#include "exchange.h"
#include "sim_constants.h"

#include <algorithm>
#include <cmath>

namespace ogs {
namespace central_bank {

namespace {

const char* const kRegimeNames[] = {"floating", "peg", "band"};
const char* const kIndependenceNames[] = {"no_data", "independent", "controlled"};
const char* const kStandardNames[] = {"fiat", "bretton_woods", "gold"};

const char* name_at(const char* const* names, int count, int value,
		const char* fallback) {
	if (value < 0 || value >= count) {
		return fallback;
	}
	return names[value];
}

int value_of(const std::string& name, const char* const* names, int count,
		int fallback) {
	for (int index = 0; index < count; ++index) {
		if (name == names[index]) {
			return index;
		}
	}
	return fallback;
}

double nonneg(double value) {
	return std::isfinite(value) ? std::max(0.0, value) : 0.0;
}

// Índice da moeda da nação; -1 = nação fora da tabela (ausência explícita).
int currency_index_of(const WorldState& ws, const std::string& nation) {
	const auto found = ws.nation_currency.find(nation);
	if (found == ws.nation_currency.end() || found->second < 0
			|| found->second >= static_cast<int>(ws.currencies.size())) {
		return -1;
	}
	return found->second;
}

// Pesos da cesta de alimentos: fatores calóricos do `needs.json`; sem cesta
// carregada, todos os alimentos pesam igual (mesma regra do IPC do G2).
void food_weights(const WorldState& ws, std::vector<double>& weight,
		double& weight_sum) {
	weight.assign(static_cast<std::size_t>(std::max(0, ws.products)), 0.0);
	weight_sum = 0.0;
	for (std::size_t slot = 0; slot < ws.food_products.size(); ++slot) {
		const int product = ws.food_products[slot];
		if (product < 0 || product >= ws.products) {
			continue;
		}
		const double factor = slot < ws.food_factors.size()
			? std::max(0.0, ws.food_factors[slot]) : 1.0;
		weight[static_cast<std::size_t>(product)] = factor;
		weight_sum += factor;
	}
	if (weight_sum <= 1e-12) {
		for (std::size_t slot = 0; slot < ws.food_products.size(); ++slot) {
			const int product = ws.food_products[slot];
			if (product >= 0 && product < ws.products) {
				weight[static_cast<std::size_t>(product)] = 1.0;
				weight_sum += 1.0;
			}
		}
	}
}

} // namespace

const char* regime_name(int value) {
	return name_at(kRegimeNames, 3, value, "peg");
}
const char* independence_name(int value) {
	return name_at(kIndependenceNames, 3, value, "no_data");
}
const char* standard_name(int value) {
	return name_at(kStandardNames, 3, value, "fiat");
}
int regime_from_name(const std::string& name, int fallback) {
	return value_of(name, kRegimeNames, 3, fallback);
}
int independence_from_name(const std::string& name, int fallback) {
	return value_of(name, kIndependenceNames, 3, fallback);
}
int standard_from_name(const std::string& name, int fallback) {
	return value_of(name, kStandardNames, 3, fallback);
}

MoneyAllocation allocate_money(const WorldState& ws, double money_total) {
	MoneyAllocation out;
	out.total = money_total;
	out.by_currency.assign(ws.currencies.size(), 0.0);

	const auto add = [&out](int currency, double value) {
		if (currency >= 0
				&& currency < static_cast<int>(out.by_currency.size())) {
			out.by_currency[static_cast<std::size_t>(currency)] += value;
			out.allocated += value;
		} else {
			out.no_currency += value;
		}
	};

	// Mesmos termos e a mesma ordem do `SimCore::money_total()`.
	const std::size_t gov_count = std::min(ws.cities.size(), ws.city_gov.size());
	for (std::size_t index = 0; index < ws.cities.size(); ++index) {
		const City& city = ws.cities[index];
		double cash = city.pop.savings + city.firm_cash + city.service_cash
			+ city.government_budget + city.capital_income_next
			+ city.rent_income_next + city.region.cash;
		if (index < gov_count) {
			const CityGov& gov = ws.city_gov[index];
			cash += nonneg(gov.cash);
			for (int slot = 0; slot < gov.project_count; ++slot) {
				cash += nonneg(gov.projects[slot].reserved);
			}
		}
		add(currency_index_of(ws, city.nation), cash);
	}
	for (const Group& group : ws.groups) {
		if (group.origin_nation.empty()) {
			out.group_cash += group.cash;
			continue;
		}
		add(currency_index_of(ws, group.origin_nation), group.cash);
	}
	for (const ConstructionProject& project : ws.construction_projects) {
		const auto found = ws.city_lookup.find(project.city);
		if (found == ws.city_lookup.end() || found->second < 0
				|| found->second >= static_cast<int>(ws.cities.size())) {
			out.project_cash += project.reserved;
			continue;
		}
		add(currency_index_of(ws,
			ws.cities[static_cast<std::size_t>(found->second)].nation),
			project.reserved);
	}
	// C3 (D-107): o caixa do banco agregado é M0 em poder do banco — a
	// "reserva bancária" que a C2a já previa na conta do M0. Entra na moeda
	// do banco (a mesma do BC que o financia).
	for (const auto& row : ws.banks) {
		add(currency_index_of(ws, row.second.nation), row.second.cash);
	}
	// Caixas de setor sem nação atribuível e a posição do exterior.
	out.sector_cash = ws.transport_cash + ws.construction_cash;
	out.external = ws.foreign_cash;
	return out;
}

std::map<std::string, CpiRow> cpi_by_nation(const WorldState& ws,
		const std::vector<double>& base_prices) {
	std::map<std::string, CpiRow> out;
	std::vector<double> weight;
	double weight_sum = 0.0;
	food_weights(ws, weight, weight_sum);
	if (weight_sum <= 1e-12) {
		return out;  // sem cesta: CPI ausente (não se inventa índice)
	}

	struct Accum {
		double index_weight = 0.0;
		double weighted = 0.0;
	};
	std::map<std::string, Accum> acc;
	for (const City& city : ws.cities) {
		// Peso da cidade: a cesta dos QUINTIS; sem estratos, a necessidade da
		// cidade; sem ambas, população de simulação pela necessidade per capita.
		double city_weight = 0.0;
		for (int stratum = 0; stratum < kIncomeStrata; ++stratum) {
			city_weight += std::max(0.0, city.pop.stratum_need[stratum]);
		}
		if (city_weight <= 1e-12) {
			city_weight = std::max(0.0, city.pop.need);
		}
		if (city_weight <= 1e-12) {
			city_weight = std::max(0.0, city.simulation_population)
				* kNeedPerCapita;
		}
		double city_index = 0.0;
		double city_index_weight = 0.0;
		for (int product = 0; product < ws.products; ++product) {
			const double factor = weight[static_cast<std::size_t>(product)];
			if (factor <= 0.0) {
				continue;
			}
			const double price = std::max(0.0, city.markets[product].price);
			if (price <= 0.0) {
				continue;
			}
			const double base = std::max(1e-9,
				product < static_cast<int>(base_prices.size())
					? base_prices[static_cast<std::size_t>(product)] : 0.0);
			city_index += factor * (price / base);
			city_index_weight += factor;
		}
		Accum& row = acc[city.nation];
		if (city_index_weight > 0.0) {
			row.index_weight += city_weight;
			row.weighted += city_weight * (city_index / city_index_weight);
		}
	}
	for (const auto& entry : acc) {
		CpiRow row;
		row.weight = entry.second.index_weight;
		if (row.weight > 1e-12) {
			row.index = entry.second.weighted / row.weight;
			row.ok = std::isfinite(row.index) && row.index > 0.0;
		}
		if (row.ok) {
			out[entry.first] = row;
		}
	}
	return out;
}

bool cpi_sample_due(const WorldState& ws) {
	if (ws.cpi_last_observe_day < 0) {
		return true;
	}
	return ws.day - ws.cpi_last_observe_day >= kCpiSampleDays;
}

void observe_cpi(WorldState& ws, const std::vector<double>& base_prices) {
	if (!cpi_sample_due(ws)) {
		return;
	}
	const std::map<std::string, CpiRow> rows = cpi_by_nation(ws, base_prices);
	for (const auto& entry : rows) {
		if (!entry.second.ok) {
			continue;  // dia sem preço de cesta: não grava amostra
		}
		CpiHistory& history = ws.cpi_history[entry.first];
		if (history.count >= kCpiSampleSlots) {
			for (int index = 1; index < kCpiSampleSlots; ++index) {
				history.samples[index - 1] = history.samples[index];
				history.sample_day[index - 1] = history.sample_day[index];
			}
			history.count = kCpiSampleSlots - 1;
		}
		const int slot = std::clamp(history.count, 0, kCpiSampleSlots - 1);
		history.samples[slot] = entry.second.index;
		history.sample_day[slot] = ws.day;
		history.count = slot + 1;
	}
	ws.cpi_last_observe_day = ws.day;
}

Inflation inflation_of(const CpiHistory& history) {
	Inflation out;
	const int count = std::clamp(history.count, 0, kCpiSampleSlots);
	out.observations = count;
	if (count <= 0) {
		return out;
	}
	const int last = count - 1;
	out.index = history.samples[last];
	out.has_index = std::isfinite(out.index) && out.index > 0.0;
	if (!out.has_index) {
		return out;
	}
	out.span_days = std::max(0,
		history.sample_day[last] - history.sample_day[0]);
	if (count >= kCpiSampleSlots) {
		const double first = history.samples[0];
		if (std::isfinite(first) && first > 0.0) {
			out.has_12m = true;
			out.rate_12m = out.index / first - 1.0;
		}
	}
	if (count >= 2 && out.span_days > 0) {
		const double first = history.samples[0];
		if (std::isfinite(first) && first > 0.0) {
			out.annualized = std::pow(out.index / first,
				static_cast<double>(kModelYearDays)
					/ static_cast<double>(out.span_days)) - 1.0;
			out.has_annualized = std::isfinite(out.annualized);
		}
	}
	return out;
}

double gold_price(const WorldState& ws, const std::vector<double>& base_prices,
		std::string* basis) {
	const auto set_basis = [basis](const char* text) {
		if (basis != nullptr) {
			*basis = text;
		}
	};
	const auto found = ws.product_lookup.find("gold");
	if (found == ws.product_lookup.end()) {
		set_basis("sem o produto ouro no catálogo");
		return 0.0;
	}
	const int product = found->second;
	if (product < 0 || product >= ws.products) {
		set_basis("sem o produto ouro no catálogo");
		return 0.0;
	}
	double price_sum = 0.0;
	int price_count = 0;
	for (const City& city : ws.cities) {
		const double price = city.markets[product].price;
		if (std::isfinite(price) && price > 0.0) {
			price_sum += price;
			++price_count;
		}
	}
	if (price_count > 0) {
		set_basis("mean_market_price");
		return price_sum / static_cast<double>(price_count);
	}
	const double base = product < static_cast<int>(base_prices.size())
		? base_prices[static_cast<std::size_t>(product)] : 0.0;
	set_basis("catalog_base_price");
	return nonneg(base);
}

std::vector<CurrencyBank> banks(const WorldState& ws,
		const MoneyAllocation& money, const std::vector<double>& base_prices) {
	std::vector<CurrencyBank> out;
	out.reserve(ws.currencies.size());
	const double gold_price_t = gold_price(ws, base_prices, nullptr);
	for (std::size_t index = 0; index < ws.currencies.size(); ++index) {
		const Currency& currency = ws.currencies[index];
		CurrencyBank bank;
		bank.code = currency.code;
		bank.regime = currency.exchange_regime;
		bank.independence = currency.bank_independence;
		bank.nations = currency.nations;
		bank.currency_in_circulation = index < money.by_currency.size()
			? money.by_currency[index] : 0.0;
		// C3 (D-107): com o banco agregado, o caixa dele é a reserva bancária
		// (M0 em poder do banco). Sem banco, M1 = M2 = M0 e a ausência de
		// depósito continua declarada — a C4 é quem traz o depósito.
		bank.bank_reserves = 0.0;
		for (const auto& row : ws.banks) {
			if (currency_of_nation_is(ws, row.second.nation, currency.code)) {
				bank.bank_reserves += nonneg(row.second.cash);
			}
		}
		bank.m0 = bank.currency_in_circulation + bank.bank_reserves;
		bank.m1 = bank.m0;
		bank.m2 = bank.m1;
		const auto gold = ws.reserve_gold_tonnes.find(currency.code);
		bank.gold_tonnes = gold != ws.reserve_gold_tonnes.end()
			? nonneg(gold->second) : 0.0;
		bank.gold_value = bank.gold_tonnes * gold_price_t;
		// C2b2 (D-106): regime efetivo, paridade, reservas e cobertura. Sem
		// reserva declarada (`funded` falso) o fixo continua administrativo,
		// como o F5 deixa o mundo; o padrão fiduciário não muda nada.
		const auto fx = ws.fx.find(currency.code);
		bank.regime_saved = currency.exchange_regime;
		bank.regime_effective = exchange::effective_regime(ws, currency);
		bank.parity = exchange::parity_of(ws, currency);
		bank.band_width = currency.exchange_regime
			== central_bank::kRegimeBand
			? exchange::band_of(ws, currency) : 0.0;
		bank.conversion_price = exchange::conversion_price(ws, currency,
			base_prices);
		if (fx != ws.fx.end()) {
			bank.reserve_currency_holdings = nonneg(fx->second.reserves);
			bank.funded = fx->second.funded;
			bank.intervention_today = fx->second.intervention_today;
			bank.devaluations = fx->second.devaluations;
			bank.last_event_day = fx->second.last_event_day;
		}
		const exchange::Coverage cover = exchange::coverage(ws, currency,
			bank.m0, base_prices);
		bank.coverage_ratio = cover.ratio;
		// C2b1 (D-104): títulos na carteira do BC (ativo financeiro criado
		// pela emissão contra dívida do governo). Sem emissão = 0.
		const auto monetary = ws.monetary.find(currency.code);
		bank.securities = monetary != ws.monetary.end()
			? monetary->second.securities : 0.0;
		out.push_back(std::move(bank));
	}
	return out;
}

} // namespace central_bank
} // namespace ogs
