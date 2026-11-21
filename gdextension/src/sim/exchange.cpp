// C2b2 — regimes de câmbio, padrão monetário e ouro físico (D-106).
// Ver docs/economy/C2b2_fx_gold.md.
#include "exchange.h"

#include "central_bank.h"
#include "sim_constants.h"
#include "storage.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ogs {
namespace exchange {

namespace {

constexpr double kEpsilon = 1e-9;

bool nation_in(const Currency& currency, const std::string& nation) {
	return std::find(currency.nations.begin(), currency.nations.end(), nation)
		!= currency.nations.end();
}

double currency_rate_guard(const Currency& currency) {
	if (!std::isfinite(currency.rate) || currency.rate <= 0.0) {
		return currency.base_rate > 0.0 ? currency.base_rate : 1.0;
	}
	return currency.rate;
}

// Passo do flutuante do F5 (idêntico ao `tick_fx` antigo): pressão da balança
// normalizada pelo volume, passo de 0,05 %/dia, preso à banda ±10 % do base.
double floating_step(const Currency& currency) {
	if (!currency.floating) {
		return currency.base_rate;
	}
	const double scale = std::max(1.0, currency.gross_ema * kFxBalanceScale);
	const double pressure = std::tanh(currency.balance_ema / scale);
	const double next = currency.rate * (1.0 + kFxDailyStep * pressure);
	return std::clamp(next, currency.base_rate * kFxBandLow,
		currency.base_rate * kFxBandHigh);
}

int gold_product(const WorldState& ws) {
	const auto found = ws.product_lookup.find("gold");
	return found == ws.product_lookup.end() ? -1 : found->second;
}

// Cofre do governo da moeda: primeiro país da lista com armazém `vault` do
// governo e capacidade > 0 (o StorageService declara a capacidade das
// toneladas do dado). Devolve -1 quando não há cofre: ausência declarada.
int find_vault(const WorldState& ws, const Currency& currency,
		int* out_city) {
	if (out_city != nullptr) {
		*out_city = -1;
	}
	for (const std::string& nation : currency.nations) {
		for (std::size_t index = 0; index < ws.cities.size(); ++index) {
			if (ws.cities[index].nation != nation) {
				continue;
			}
			const int site = find_storage_site(ws, static_cast<int>(index),
				STORAGE_VAULT, STORAGE_GOVERNMENT);
			if (site < 0) {
				continue;
			}
			if (ws.storage_sites[static_cast<std::size_t>(site)].capacity_t
					<= 0.0) {
				continue;
			}
			if (out_city != nullptr) {
				*out_city = static_cast<int>(index);
			}
			return site;
		}
	}
	return -1;
}

void ensure_stock_slots(StorageSite& site, int products) {
	const std::size_t count = static_cast<std::size_t>(std::max(0, products));
	if (site.stock.size() != count) {
		site.stock.resize(count, 0.0);
	}
	if (site.inflow_stock.size() != count) {
		site.inflow_stock.resize(count, 0.0);
	}
}

// C2b2 (D-106): a moeda local que o BC recolhe numa venda de reserva vem dos
// IMPORTADORES da moeda (o outro lado da operação: quem compra divisa paga em
// moeda local), proporcional ao caixa das firmas da cidade. Nunca passa do
// caixa — o que não couber limita a intervenção (e pode levar à
// desvalorização). Devolve o valor efetivamente recolhido.
double destroy_local_money(WorldState& ws, const Currency& currency,
		double amount) {
	if (!(amount > 0.0)) {
		return 0.0;
	}
	double total = 0.0;
	for (const City& city : ws.cities) {
		if (nation_in(currency, city.nation)) {
			total += std::max(0.0, city.firm_cash);
		}
	}
	const double take = std::min(amount, total);
	if (take <= kEpsilon) {
		return 0.0;
	}
	double assigned = 0.0;
	for (std::size_t index = 0; index < ws.cities.size(); ++index) {
		City& city = ws.cities[index];
		if (!nation_in(currency, city.nation)) {
			continue;
		}
		const double cash = std::max(0.0, city.firm_cash);
		if (cash <= 0.0) {
			continue;
		}
		double part = take * cash / std::max(total, kEpsilon);
		part = std::min(part, take - assigned);
		part = std::min(part, cash);
		if (part <= 0.0) {
			continue;
		}
		city.firm_cash -= part;
		assigned += part;
		if (assigned >= take - kEpsilon) {
			break;
		}
	}
	if (assigned < take - kEpsilon) {
		// Sobra por arredondamento: sai da última cidade com caixa.
		for (std::size_t index = ws.cities.size(); index > 0; --index) {
			City& city = ws.cities[index - 1];
			if (!nation_in(currency, city.nation)) {
				continue;
			}
			const double cash = std::max(0.0, city.firm_cash);
			const double part = std::min(take - assigned, cash);
			if (part > 0.0) {
				city.firm_cash -= part;
				assigned += part;
				break;
			}
		}
	}
	return assigned;
}

// Compra de reserva emite moeda local, creditada aos EXPORTADORES (quem vende
// divisa recebe moeda local), proporcional ao valor exportado do dia. Sem
// exportador, a primeira cidade da moeda absorve (o mercado é essa cidade).
void create_local_money(WorldState& ws, const Currency& currency,
		double amount) {
	if (!(amount > 0.0)) {
		return;
	}
	double total = 0.0;
	for (const City& city : ws.cities) {
		if (!nation_in(currency, city.nation)) {
			continue;
		}
		double value = 0.0;
		for (int product = 0; product < ws.products; ++product) {
			value += std::max(0.0, city.export_value[product]);
		}
		total += value;
	}
	double assigned = 0.0;
	bool any = false;
	for (std::size_t index = 0; index < ws.cities.size(); ++index) {
		City& city = ws.cities[index];
		if (!nation_in(currency, city.nation)) {
			continue;
		}
		double value = 0.0;
		for (int product = 0; product < ws.products; ++product) {
			value += std::max(0.0, city.export_value[product]);
		}
		if (total <= 0.0 && any) {
			break;
		}
		const double part = total > 0.0
			? amount * value / total : amount - assigned;
		city.firm_cash += std::max(0.0, part);
		assigned += std::max(0.0, part);
		any = true;
		if (assigned >= amount - kEpsilon) {
			break;
		}
	}
}

} // namespace

Parameters parameters(const WorldState& ws) {
	Parameters out;
	out.band_width = ws.fx_band_width > 0.0
		? ws.fx_band_width : kBandWidthDefault;
	out.defence_share = ws.fx_defence_share > 0.0
		? ws.fx_defence_share : kDefenceShareDefault;
	out.gold_reserve_ratio = ws.fx_gold_reserve_ratio > 0.0
		? ws.fx_gold_reserve_ratio : kGoldReserveRatioDefault;
	return out;
}

void set_parameters(WorldState& ws, double band_width, double defence_share,
		double gold_reserve_ratio) {
	if (band_width > 0.0) {
		ws.fx_band_width = band_width;
	}
	if (defence_share > 0.0) {
		ws.fx_defence_share = defence_share;
	}
	if (gold_reserve_ratio > 0.0) {
		ws.fx_gold_reserve_ratio = gold_reserve_ratio;
	}
}

bool standard_forces_peg(const WorldState& ws) {
	return ws.monetary_standard == central_bank::kStandardBrettonWoods
		|| ws.monetary_standard == central_bank::kStandardGold;
}

int effective_regime(const WorldState& ws, const Currency& currency) {
	if (standard_forces_peg(ws)) {
		return central_bank::kRegimePeg;
	}
	return currency.exchange_regime;
}

double parity_of(const WorldState& ws, const Currency& currency) {
	const auto found = ws.fx.find(currency.code);
	if (found != ws.fx.end() && found->second.parity > 0.0) {
		return found->second.parity;
	}
	if (currency.base_rate > 0.0) {
		return currency.base_rate;
	}
	return 1.0;
}

namespace {

// Taxa de uma moeda pelo código (âncora). 0 = moeda ausente/taxa inválida.
double rate_of_code(const WorldState& ws, const std::string& code) {
	const auto found = ws.currency_by_code.find(code);
	if (found == ws.currency_by_code.end()) {
		return 0.0;
	}
	const std::size_t index = static_cast<std::size_t>(found->second);
	if (index >= ws.currencies.size()) {
		return 0.0;
	}
	return currency_rate_guard(ws.currencies[index]);
}

const FxRegimeInfo* regime_info(const WorldState& ws, const std::string& code) {
	const auto found = ws.fx_regime.find(code);
	return found == ws.fx_regime.end() ? nullptr : &found->second;
}

} // namespace

bool has_anchor(const WorldState& ws, const Currency& currency) {
	const FxRegimeInfo* info = regime_info(ws, currency.code);
	if (info == nullptr) {
		return false;
	}
	if (info->anchor_type == "currency") {
		return !info->anchor.empty();
	}
	if (info->anchor_type == "composite") {
		return true;
	}
	return false;
}

double anchor_value(const WorldState& ws, const Currency& currency) {
	const FxRegimeInfo* info = regime_info(ws, currency.code);
	if (info == nullptr) {
		return 0.0;
	}
	if (info->anchor_type == "currency") {
		return rate_of_code(ws, info->anchor);
	}
	if (info->anchor_type != "composite") {
		return 0.0;
	}
	// Cesta: composição PUBLICADA (info->basket) ou resolvida no estado
	// (state.basket). Sem pesos, a âncora não tem alvo — ausência declarada.
	std::map<std::string, double> weights = info->basket;
	if (weights.empty()) {
		const auto state = ws.fx.find(currency.code);
		if (state != ws.fx.end()) {
			weights = state->second.basket;
		}
	}
	if (weights.empty()) {
		return 0.0;
	}
	double log_sum = 0.0;
	double weight_sum = 0.0;
	for (const auto& row : weights) {
		if (!(row.second > 0.0)) {
			continue;
		}
		const double rate = rate_of_code(ws, row.first);
		if (!(rate > 0.0)) {
			return 0.0;
		}
		log_sum += row.second * std::log(rate);
		weight_sum += row.second;
	}
	if (!(weight_sum > 0.0)) {
		return 0.0;
	}
	return std::exp(log_sum / weight_sum);
}

void resolve_baskets(WorldState& ws) {
	if (ws.currencies.empty() || ws.cities.empty()) {
		return;
	}
	std::vector<std::string> pending;
	for (const auto& row : ws.fx_regime) {
		if (row.second.anchor_type != "composite"
				|| !row.second.basket.empty()) {
			continue;
		}
		pending.push_back(row.first);
	}
	if (pending.empty()) {
		return;
	}
	// Comércio medido por moeda (o mesmo vetor que o `tick` usa).
	std::vector<double> trade(ws.currencies.size(), 0.0);
	for (const City& city : ws.cities) {
		const auto found = ws.nation_currency.find(city.nation);
		if (found == ws.nation_currency.end()) {
			continue;
		}
		const std::size_t index = static_cast<std::size_t>(found->second);
		if (index >= trade.size()) {
			continue;
		}
		for (int product = 0; product < ws.products; ++product) {
			trade[index] += std::max(0.0, city.export_value[product])
				+ std::max(0.0, city.import_value[product]);
		}
	}
	double total = 0.0;
	for (const double value : trade) {
		total += value;
	}
	if (!(total > kEpsilon)) {
		// Sem comércio medido ainda: a cesta fica sem pesos (nada inventado).
		return;
	}
	// Candidatos = as moedas com maior comércio medido (top 3), excluindo a
	// própria. É DEFAULT DE JOGO: o quadro do FMI não publica a composição.
	std::vector<std::size_t> order(ws.currencies.size());
	for (std::size_t index = 0; index < order.size(); ++index) {
		order[index] = index;
	}
	std::stable_sort(order.begin(), order.end(),
		[&](std::size_t left, std::size_t right) {
			if (trade[left] != trade[right]) {
				return trade[left] > trade[right];
			}
			return ws.currencies[left].code < ws.currencies[right].code;
		});
	for (const std::string& code : pending) {
		const auto found = ws.currency_by_code.find(code);
		if (found == ws.currency_by_code.end()) {
			continue;
		}
		const std::size_t self = static_cast<std::size_t>(found->second);
		FxState& state = ws.fx[code];
		if (!state.basket.empty()) {
			continue;
		}
		double sum = 0.0;
		std::map<std::string, double> basket;
		for (const std::size_t index : order) {
			if (index == self || basket.size() >= 3) {
				continue;
			}
			if (!(trade[index] > kEpsilon)) {
				break;
			}
			basket[ws.currencies[index].code] = trade[index];
			sum += trade[index];
		}
		if (basket.empty() || !(sum > kEpsilon)) {
			continue;
		}
		for (auto& row : basket) {
			row.second /= sum;
		}
		state.basket = std::move(basket);
		state.basket_source = "default de jogo: participacao no comercio "
			"medido (a composicao da cesta nao e publicada pelo FMI)";
	}
}

void initialize_anchors(WorldState& ws) {
	for (const Currency& currency : ws.currencies) {
		if (!has_anchor(ws, currency)) {
			continue;
		}
		FxState& state = ws.fx[currency.code];
		if (state.parity > 0.0) {
			continue;
		}
		const double anchor = anchor_value(ws, currency);
		const double base = currency.base_rate > 0.0
			? currency.base_rate : currency_rate_guard(currency);
		if (!(anchor > 0.0) || !(base > 0.0)) {
			continue;
		}
		// Paridade CRUZADA derivada da taxa com que o mundo nasceu: o peg não
		// move a taxa no dia 1 (§7.2 de fx_regimes_data.md).
		state.parity = anchor / base;
	}
}

double target_rate(const WorldState& ws, const Currency& currency) {
	if (!has_anchor(ws, currency)) {
		return parity_of(ws, currency);
	}
	const double anchor = anchor_value(ws, currency);
	const double parity = parity_of(ws, currency);
	if (!(anchor > 0.0) || !(parity > 0.0)) {
		// Cesta sem composição: alvo indisponível; a taxa fica onde está (é o
		// peg administrativo do F5, sem custo e sem número inventado).
		return currency_rate_guard(currency);
	}
	return anchor / parity;
}

double migrate_currency(WorldState& ws, const std::string& from,
		const std::string& to, double units_per_target) {
	if (from == to || !(units_per_target > 0.0)) {
		return 0.0;
	}
	const auto from_found = ws.currency_by_code.find(from);
	const auto to_found = ws.currency_by_code.find(to);
	if (from_found == ws.currency_by_code.end()
			|| to_found == ws.currency_by_code.end()) {
		return 0.0;
	}
	const int from_index = from_found->second;
	const int to_index = to_found->second;
	if (from_index < 0 || to_index < 0 || from_index == to_index
			|| from_index >= static_cast<int>(ws.currencies.size())
			|| to_index >= static_cast<int>(ws.currencies.size())) {
		return 0.0;
	}
	Currency& source = ws.currencies[static_cast<std::size_t>(from_index)];
	Currency& dest = ws.currencies[static_cast<std::size_t>(to_index)];
	for (const std::string& nation : source.nations) {
		if (!nation_in(dest, nation)) {
			dest.nations.push_back(nation);
		}
	}
	// Paridade LOCAL (unidades por 1 âncora): 1 unidade do destino vale
	// `units_per_target` unidades extintas, então divide.
	double converted = 0.0;
	const auto fx_source = ws.fx.find(from);
	if (fx_source != ws.fx.end()) {
		const FxState state = fx_source->second;
		FxState& target = ws.fx[to];
		converted = state.parity;
		if (state.parity > 0.0 && target.parity <= 0.0) {
			target.parity = state.parity / units_per_target;
		}
		target.reserves += state.reserves;
		target.funded = target.funded || state.funded;
		if (target.band_width < 0.0) {
			target.band_width = state.band_width;
		}
		if (target.conversion_price <= 0.0) {
			target.conversion_price = state.conversion_price;
		}
		target.devaluations += state.devaluations;
		if (state.last_event_day > target.last_event_day) {
			target.last_event_day = state.last_event_day;
		}
		ws.fx.erase(fx_source);
	}
	// Ouro do BC, emissão e bancos: valores em âncora/nação — mudam de CHAVE,
	// não de valor (o balanço financeiro soma zero nos dois lados).
	const auto gold = ws.reserve_gold_tonnes.find(from);
	if (gold != ws.reserve_gold_tonnes.end()) {
		ws.reserve_gold_tonnes[to] += gold->second;
		ws.reserve_gold_tonnes.erase(gold);
	}
	const auto monetary = ws.monetary.find(from);
	if (monetary != ws.monetary.end()) {
		MonetaryState& target = ws.monetary[to];
		target.emission += monetary->second.emission;
		target.securities += monetary->second.securities;
		if (target.rate < 0.0) {
			target.rate = monetary->second.rate;
		}
		ws.monetary.erase(monetary);
	}
	for (auto& row : ws.banks) {
		if (row.second.currency == from) {
			row.second.currency = to;
		}
	}
	// A moeda extinta não tem mais política própria (nem regime no arquivo).
	ws.fx_regime.erase(from);
	ws.currencies.erase(ws.currencies.begin() + from_index);
	ws.currency_by_code.clear();
	ws.nation_currency.clear();
	for (std::size_t index = 0; index < ws.currencies.size(); ++index) {
		const Currency& row = ws.currencies[index];
		ws.currency_by_code[row.code] = static_cast<int>(index);
		for (const std::string& nation : row.nations) {
			ws.nation_currency[nation] = static_cast<int>(index);
		}
	}
	return converted;
}

double band_of(const WorldState& ws, const Currency& currency) {
	const auto found = ws.fx.find(currency.code);
	if (found != ws.fx.end() && found->second.band_width >= 0.0) {
		return std::clamp(found->second.band_width, 0.0, 1.0);
	}
	return parameters(ws).band_width;
}

double conversion_price(const WorldState& ws, const Currency& currency,
		const std::vector<double>& base_prices) {
	const auto found = ws.fx.find(currency.code);
	if (found != ws.fx.end() && found->second.conversion_price > 0.0) {
		return found->second.conversion_price;
	}
	const int gold = gold_product(ws);
	if (gold >= 0 && static_cast<std::size_t>(gold) < base_prices.size()
			&& base_prices[static_cast<std::size_t>(gold)] > 0.0) {
		return base_prices[static_cast<std::size_t>(gold)];
	}
	return 0.0;
}

void push_event(WorldState& ws, Event event) {
	FxEvent row;
	row.day = event.day;
	row.currency = event.currency;
	row.type = event.type;
	row.old_parity = event.old_parity;
	row.new_parity = event.new_parity;
	row.reserves_used = event.reserves_used;
	ws.fx_events.push_back(std::move(row));
	if (static_cast<int>(ws.fx_events.size()) > kMaxEvents) {
		ws.fx_events.erase(ws.fx_events.begin());
	}
}

DayStats tick(WorldState& ws, const std::vector<double>& base_prices) {
	(void)base_prices;
	DayStats stats;
	if (ws.currencies.empty() || ws.cities.empty()) {
		return stats;
	}
	// C2c (D-108): a cesta sem composição publicada ganha pesos do comércio
	// medido do próprio mundo e a paridade cruzada nasce da taxa inicial.
	resolve_baskets(ws);
	initialize_anchors(ws);
	const Parameters params = parameters(ws);
	std::vector<double> exports(ws.currencies.size(), 0.0);
	std::vector<double> imports(ws.currencies.size(), 0.0);
	for (const City& city : ws.cities) {
		const auto found = ws.nation_currency.find(city.nation);
		if (found == ws.nation_currency.end()) {
			continue;
		}
		const std::size_t index = static_cast<std::size_t>(found->second);
		for (int product = 0; product < ws.products; ++product) {
			exports[index] += city.export_value[product];
			imports[index] += city.import_value[product];
		}
	}
	const double alpha = 1.0 / kFxBalanceEmaDays;
	for (std::size_t index = 0; index < ws.currencies.size(); ++index) {
		Currency& currency = ws.currencies[index];
		FxState& state = ws.fx[currency.code];
		state.intervention_today = 0.0;
		const double flow = exports[index] - imports[index];
		const double gross = exports[index] + imports[index];
		currency.balance_ema += (flow - currency.balance_ema) * alpha;
		currency.gross_ema += (gross - currency.gross_ema) * alpha;
		const int regime = effective_regime(ws, currency);
		if (regime == central_bank::kRegimeFloating) {
			// Caminho do F5, inalterado.
			currency.rate = floating_step(currency);
			continue;
		}
		const double parity = parity_of(ws, currency);
		// C2c (D-108): com âncora, o alvo é a taxa CRUZADA (âncora ÷ paridade)
		// — um peg a USD acompanha o dólar em vez de arrastar a taxa pelo XGS.
		// Sem âncora o caminho é o da C2b2, número por número.
		const double anchor = has_anchor(ws, currency)
			? anchor_value(ws, currency) : 0.0;
		const bool cross = anchor > 0.0 && parity > 0.0;
		const double target = cross ? anchor / parity : parity;
		const double width = regime == central_bank::kRegimeBand
			? band_of(ws, currency) : 0.0;
		const double low = target * (1.0 - width);
		const double high = target * (1.0 + width);
		const double shadow = floating_step(currency);
		// Sem reserva declarada o fixo/banda é ADMINISTRATIVO (o F5 já segura a
		// taxa em `base_rate` sem custo): a defesa medida só existe quando há
		// reserva. A ausência é declarada no status (`funded = false`).
		if (!state.funded) {
			currency.rate = regime == central_bank::kRegimeBand
				? std::clamp(shadow, low, high)
				: target;
			continue;
		}
		// Volume em unidades da ÂNCORA: o comércio é medido em XGS.
		const double volume_anchor = currency.gross_ema / std::max(
			cross ? anchor : parity, kEpsilon);
		if (shadow > high) {
			// Depreciação: o BC vende moeda de reserva e recolhe (destrói) a
			// moeda local paga pelos importadores da moeda.
			const double gap = shadow / std::max(high, kEpsilon) - 1.0;
			const double need = std::max(0.0, volume_anchor * gap
				* params.defence_share);
			double used = std::min(need, std::max(0.0, state.reserves));
			if (used > kEpsilon) {
				const double local = destroy_local_money(ws, currency,
					used * target);
				used = local / std::max(target, kEpsilon);
				state.reserves = std::max(0.0, state.reserves - used);
				state.intervention_today -= used;
				stats.reserves_sold += used;
				stats.money_destroyed += local;
				stats.defended += 1;
			}
			if (need > used + kEpsilon) {
				// Não há reserva para o excesso: desvalorização forçada para o
				// câmbio sombreado. A paridade nova é a taxa de mercado.
				const double old_parity = parity;
				// A paridade nova mantém a relação com a âncora: taxa = alvo.
				state.parity = cross ? anchor / std::max(shadow, kEpsilon)
					: shadow;
				state.reserves = 0.0;
				currency.rate = shadow;
				state.devaluations += 1;
				state.last_event_day = ws.day;
				stats.devaluations += 1;
				Event event;
				event.day = ws.day;
				event.currency = currency.code;
				event.type = "devaluation";
				event.old_parity = old_parity;
				event.new_parity = shadow;
				event.reserves_used = used;
				push_event(ws, event);
			} else {
				currency.rate = high;
			}
		} else if (shadow < low) {
			// Apreciação: o BC compra reserva e emite moeda local, creditada
			// aos exportadores da moeda (quem vende divisa recebe local).
			const double gap = 1.0 - shadow / std::max(low, kEpsilon);
			const double amount = std::max(0.0, volume_anchor * gap
				* params.defence_share);
			if (amount > kEpsilon) {
				const double local = amount * target;
				state.reserves += amount;
				create_local_money(ws, currency, local);
				state.intervention_today += amount;
				stats.reserves_bought += amount;
				stats.money_created += local;
				stats.defended += 1;
			}
			currency.rate = low;
		} else {
			currency.rate = shadow;
		}
	}
	// Nenhum preço é reescalado: a taxa só vale para exibição e
	// competitividade (o contrato do F5 continua).
	// C2b2 (D-106): a intervenção cambial é OPERAÇÃO DO BC — a moeda local
	// criada (compra de reserva) ou recolhida (venda) entra na emissão LÍQUIDA
	// registrada do dia, a mesma que o invariante da C1 confere.
	ws.emission_today += stats.money_created - stats.money_destroyed;
	return stats;
}

namespace {

double gold_ledger(const WorldState& ws, const std::string& code) {
	const auto found = ws.reserve_gold_tonnes.find(code);
	return found == ws.reserve_gold_tonnes.end() ? 0.0 : found->second;
}

} // namespace

double vault_gold(const WorldState& ws, const Currency& currency) {
	const int gold = gold_product(ws);
	if (gold < 0) {
		return 0.0;
	}
	double total = 0.0;
	for (const StorageSite& site : ws.storage_sites) {
		if (site.type != STORAGE_VAULT || site.scope != STORAGE_GOVERNMENT) {
			continue;
		}
		if (site.city_index < 0
				|| static_cast<std::size_t>(site.city_index) >= ws.cities.size()) {
			continue;
		}
		if (!nation_in(currency, ws.cities[
				static_cast<std::size_t>(site.city_index)].nation)) {
			continue;
		}
		if (static_cast<std::size_t>(gold) < site.stock.size()) {
			total += std::max(0.0, site.stock[static_cast<std::size_t>(gold)]);
		}
	}
	return total;
}

Coverage coverage(const WorldState& ws, const Currency& currency, double m0,
		const std::vector<double>& base_prices) {
	Coverage out;
	out.gold_tonnes = gold_ledger(ws, currency.code);
	out.has_gold = out.gold_tonnes > 0.0;
	out.gold_value_anchor = out.gold_tonnes
		* conversion_price(ws, currency, base_prices);
	out.m0 = std::max(0.0, m0);
	const double parity = parity_of(ws, currency);
	out.m0_anchor = out.m0 / std::max(parity, kEpsilon);
	if (out.m0_anchor > kEpsilon) {
		out.ratio = out.gold_value_anchor / out.m0_anchor;
	}
	return out;
}

double emission_headroom(const WorldState& ws, const Currency& currency,
		double m0, const std::vector<double>& base_prices) {
	if (ws.monetary_standard != central_bank::kStandardGold) {
		return std::numeric_limits<double>::infinity();
	}
	const Parameters params = parameters(ws);
	const Coverage cover = coverage(ws, currency, m0, base_prices);
	const double max_m0_anchor = cover.gold_value_anchor
		/ std::max(params.gold_reserve_ratio, kEpsilon);
	const double headroom = std::max(0.0, max_m0_anchor - cover.m0_anchor);
	return headroom * std::max(parity_of(ws, currency), kEpsilon);
}

void set_reserves(WorldState& ws, const std::string& code,
		double anchor_units) {
	FxState& state = ws.fx[code];
	state.reserves = std::max(0.0, anchor_units);
	state.funded = state.funded || state.reserves > 0.0;
}

double seed_reserves_from_imports(WorldState& ws, int days) {
	if (ws.currencies.empty() || days <= 0) {
		return 0.0;
	}
	std::vector<double> imports(ws.currencies.size(), 0.0);
	for (const City& city : ws.cities) {
		const auto found = ws.nation_currency.find(city.nation);
		if (found == ws.nation_currency.end()) {
			continue;
		}
		const std::size_t index = static_cast<std::size_t>(found->second);
		for (int product = 0; product < ws.products; ++product) {
			imports[index] += city.import_value[product];
		}
	}
	double total = 0.0;
	for (std::size_t index = 0; index < ws.currencies.size(); ++index) {
		const Currency& currency = ws.currencies[index];
		const double parity = parity_of(ws, currency);
		const double anchor = imports[index] / std::max(parity, kEpsilon)
			* static_cast<double>(days);
		if (anchor <= 0.0) {
			continue;
		}
		FxState& state = ws.fx[currency.code];
		state.reserves += anchor;
		state.funded = true;
		total += anchor;
	}
	return total;
}

GoldTrade gold_trade(WorldState& ws, const std::string& currency_code,
		double tonnes, bool buy) {
	GoldTrade out;
	const auto found = ws.currency_by_code.find(currency_code);
	if (found == ws.currency_by_code.end()) {
		out.error = "moeda desconhecida";
		return out;
	}
	const Currency& currency = ws.currencies[static_cast<std::size_t>(
		found->second)];
	const int gold = gold_product(ws);
	if (gold < 0) {
		out.error = "bem gold ausente do catalogo";
		return out;
	}
	if (!(tonnes > 0.0)) {
		out.error = "quantidade invalida";
		return out;
	}
	int city_index = -1;
	const int site_index = find_vault(ws, currency, &city_index);
	if (site_index < 0 || city_index < 0) {
		out.error = "sem cofre declarado";
		return out;
	}
	StorageSite& site = ws.storage_sites[static_cast<std::size_t>(site_index)];
	ensure_stock_slots(site, ws.products);
	City& city = ws.cities[static_cast<std::size_t>(city_index)];
	Market& market = city.markets[gold];
	const double price = market.price > 0.0
		? market.price : ws.base_prices[gold];
	if (!(price > 0.0)) {
		out.error = "preco invalido";
		return out;
	}
	const std::size_t slot = static_cast<std::size_t>(gold);
	if (buy) {
		const double free_capacity = storage_free_capacity(ws, site);
		double qty = std::min(tonnes, std::min(std::max(0.0, market.stock),
			free_capacity));
		qty = std::min(qty, std::max(0.0, city.government_budget) / price);
		if (qty <= kEpsilon) {
			out.error = "sem estoque/capacidade/caixa";
			return out;
		}
		const double value = qty * price;
		city.government_budget -= value;
		// O vendedor é a firma local (mesmo contrato do storage_trade).
		city.firm_cash += value;
		city.revenue_by_product[gold] += value;
		market.stock -= qty;
		market.demand += qty;
		market.sold += qty;
		market.effective += qty;
		site.stock[slot] += qty;
		site.bought_today_t += qty;
		ws.reserve_gold_tonnes[currency_code] = gold_ledger(ws, currency_code)
			+ qty;
		out.ok = true;
		out.tons = qty;
		out.value = value;
		out.city = city.id;
		return out;
	}
	double qty = std::min(tonnes, std::max(0.0, site.stock[slot]));
	qty = std::min(qty, std::max(0.0, city.firm_cash) / price);
	if (qty <= kEpsilon) {
		out.error = "sem ouro para vender/caixa do comprador";
		return out;
	}
	const double value = qty * price;
	city.firm_cash -= value;
	city.government_budget += value;
	market.stock += qty;
	site.stock[slot] -= qty;
	site.released_today_t += qty;
	ws.reserve_gold_tonnes[currency_code] = std::max(0.0,
		gold_ledger(ws, currency_code) - qty);
	out.ok = true;
	out.tons = qty;
	out.value = value;
	out.city = city.id;
	return out;
}

GoldTrade gold_convert(WorldState& ws, const std::string& currency_code,
		double tonnes, const std::vector<double>& base_prices) {
	GoldTrade out;
	if (ws.monetary_standard != central_bank::kStandardBrettonWoods
			&& ws.monetary_standard != central_bank::kStandardGold) {
		out.error = "padrao nao converte ouro";
		return out;
	}
	const auto found = ws.currency_by_code.find(currency_code);
	if (found == ws.currency_by_code.end()) {
		out.error = "moeda desconhecida";
		return out;
	}
	Currency& currency = ws.currencies[static_cast<std::size_t>(found->second)];
	const int gold = gold_product(ws);
	if (gold < 0) {
		out.error = "bem gold ausente do catalogo";
		return out;
	}
	if (!(tonnes > 0.0)) {
		out.error = "quantidade invalida";
		return out;
	}
	int city_index = -1;
	const int site_index = find_vault(ws, currency, &city_index);
	if (site_index < 0 || city_index < 0) {
		out.error = "sem cofre declarado";
		return out;
	}
	StorageSite& site = ws.storage_sites[static_cast<std::size_t>(site_index)];
	ensure_stock_slots(site, ws.products);
	City& city = ws.cities[static_cast<std::size_t>(city_index)];
	Market& market = city.markets[gold];
	const double price = conversion_price(ws, currency, base_prices);
	const double parity = parity_of(ws, currency);
	FxState& state = ws.fx[currency_code];
	if (!(price > 0.0) || !(parity > 0.0)) {
		out.error = "preco/paridade invalidos";
		return out;
	}
	double qty = std::min(tonnes, std::max(0.0, market.stock));
	qty = std::min(qty, storage_free_capacity(ws, site));
	qty = std::min(qty, state.reserves / price);
	if (qty <= kEpsilon) {
		out.error = "sem ouro no mercado/capacidade/reserva";
		return out;
	}
	const double value_anchor = qty * price;
	const double local = value_anchor * parity;
	const std::size_t slot = static_cast<std::size_t>(gold);
	market.stock -= qty;
	market.demand += qty;
	market.sold += qty;
	market.effective += qty;
	site.stock[slot] += qty;
	site.bought_today_t += qty;
	state.reserves = std::max(0.0, state.reserves - value_anchor);
	// O exterior entrega o ouro e recebe moeda local emitida contra a reserva
	// gasta: o passivo monetário e a posição do exterior andam juntos (C1).
	ws.foreign_cash += local;
	ws.reserve_gold_tonnes[currency_code] = gold_ledger(ws, currency_code)
		+ qty;
	Event event;
	event.day = ws.day;
	event.currency = currency_code;
	event.type = "gold_conversion";
	event.old_parity = parity;
	event.new_parity = parity;
	event.reserves_used = value_anchor;
	push_event(ws, event);
	out.ok = true;
	out.tons = qty;
	out.value = value_anchor;
	out.city = city.id;
	return out;
}

} // namespace exchange
} // namespace ogs
