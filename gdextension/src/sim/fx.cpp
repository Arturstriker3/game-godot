// Módulo de domínio: câmbio, moedas, tarifas e índices de produto
// (S14 do docs/economy/core_refactor_plan.md).
// SimCore:: métodos movidos de sim_core.cpp — realocação byte-idêntica.
#include "sim_core.h"

#include "profiling.h"
#include "sim_constants.h"
#include "sim_labor.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace ogs {
void SimCore::phase_fx() {
	// 9c. Câmbio dinâmico: a balança comercial do dia move as moedas float
	// dentro da banda (déficit desvaloriza, superávit valoriza). Não cria nem
	// destrói dinheiro: só muda a taxa usada na próxima decisão de compra.
	demo_tick_fx();

	OGS_SIM_PHASE_LAP(kDemoPhaseFx);
}

// ------------------------------------------------------------------
// F5 — câmbio/tarifas consultados pela fase de serviços.
// ------------------------------------------------------------------

const ogs::DemoCurrency* SimCore::demo_currency_for(
		const std::string& nation) const {
	const auto found = ws_.nation_currency.find(nation);
	if (found == ws_.nation_currency.end()
			|| found->second < 0
			|| found->second >= static_cast<int>(ws_.currencies.size())) {
		return nullptr;
	}
	return &ws_.currencies[static_cast<std::size_t>(found->second)];
}
double SimCore::demo_fx_rate(const std::string& nation) const {
	const DemoCurrency* currency = demo_currency_for(nation);
	if (currency == nullptr || !std::isfinite(currency->rate)
			|| currency->rate <= 0.0) {
		return 1.0;
	}
	return currency->rate;
}
const char* SimCore::demo_currency_code(const std::string& nation) const {
	const DemoCurrency* currency = demo_currency_for(nation);
	return currency != nullptr ? currency->code.c_str() : "XGS";
}
void SimCore::demo_rebuild_currency_indexes() {
	ws_.currency_by_code.clear();
	ws_.nation_currency.clear();
	ws_.currency_by_code.reserve(ws_.currencies.size() * 2);
	for (std::size_t index = 0; index < ws_.currencies.size(); ++index) {
		DemoCurrency& currency = ws_.currencies[index];
		if (currency.code.empty()) {
			continue;
		}
		ws_.currency_by_code[currency.code] = static_cast<int>(index);
		for (const std::string& nation : currency.nations) {
			if (!nation.empty()) {
				ws_.nation_currency[nation] = static_cast<int>(index);
			}
		}
	}
}
void SimCore::demo_tick_fx() {
	if (ws_.currencies.empty() || ws_.cities.empty()) {
		return;
	}
	std::vector<double> exports(ws_.currencies.size(), 0.0);
	std::vector<double> imports(ws_.currencies.size(), 0.0);
	for (const DemoCity& city : ws_.cities) {
		const auto found = ws_.nation_currency.find(city.nation);
		if (found == ws_.nation_currency.end()) {
			continue;
		}
		const std::size_t index = static_cast<std::size_t>(found->second);
		for (int product = 0; product < ws_.products; ++product) {
			exports[index] += city.export_value[product];
			imports[index] += city.import_value[product];
		}
	}
	const double alpha = 1.0 / kDemoFxBalanceEmaDays;
	for (std::size_t index = 0; index < ws_.currencies.size(); ++index) {
		DemoCurrency& currency = ws_.currencies[index];
		const double flow = exports[index] - imports[index];
		const double gross = exports[index] + imports[index];
		currency.balance_ema += (flow - currency.balance_ema) * alpha;
		currency.gross_ema += (gross - currency.gross_ema) * alpha;
		if (!currency.floating) {
			currency.rate = currency.base_rate;
			continue;
		}
		// Pressão normalizada pelo volume: um déficit que representa 10% do
		// comércio (kDemoFxBalanceScale) satura em tanh(1) ~ 0,76. O passo
		// diário é pequeno (0,05%), então o câmbio reage à persistência da
		// balança, não a um dia isolado.
		const double scale = std::max(1.0,
			currency.gross_ema * kDemoFxBalanceScale);
		const double pressure = std::tanh(currency.balance_ema / scale);
		const double next = currency.rate * (1.0 + kDemoFxDailyStep * pressure);
		currency.rate = std::clamp(next,
			currency.base_rate * kDemoFxBandLow,
			currency.base_rate * kDemoFxBandHigh);
	}

	// O preço interno está na âncora: o passo cambial NÃO reescala preços. Só
	// a exibição (`price_local = preço ÷ taxa`) e a competitividade das
	// ofertas float (taxa ÷ taxa-base) leem a taxa corrente.
}
double SimCore::demo_competitiveness(const std::string& nation) const {
	// Fator de competitividade aplicado à âncora (F5, opção c). Com a taxa
	// FIXA (peg) o fator é 1: o valor interno não muda. Com regime float, a
	// taxa pode desviar ±10% da referência; o fator `taxa ÷ taxa-base`
	// reproduz o efeito de preço rígido na moeda local (desvalorização barateia
	// a oferta externa do país sem reescrever o preço interno), documentado em
	// GAME_STATE §15/D-027.
	const DemoCurrency* currency = demo_currency_for(nation);
	if (currency == nullptr || !std::isfinite(currency->rate)
			|| !std::isfinite(currency->base_rate)
			|| currency->rate <= 0.0 || currency->base_rate <= 0.0) {
		return 1.0;
	}
	if (!currency->floating) {
		return 1.0;
	}
	return std::clamp(currency->rate / currency->base_rate,
		kDemoFxBandLow, kDemoFxBandHigh);
}
double SimCore::demo_tariff_rate(const std::string& buyer_nation,
		const std::string& seller_nation, int product) const {
	if (ws_.tariff_lookup.empty() || product < 0 || product >= ws_.products) {
		return 0.0;
	}
	const std::string good = demo_product_name(product);
	const std::string keys[4] = {
		buyer_nation + '|' + seller_nation + '|' + good,
		buyer_nation + "|*|" + good,
		buyer_nation + '|' + seller_nation + "|*",
		buyer_nation + "|*|*",
	};
	for (const std::string& key : keys) {
		const auto found = ws_.tariff_lookup.find(key);
		if (found != ws_.tariff_lookup.end()) {
			return std::clamp(found->second, 0.0, 1.0);
		}
	}
	return 0.0;
}
int SimCore::demo_product_index(const std::string& product) const {
	const auto found = ws_.product_lookup.find(product);
	if (found == ws_.product_lookup.end()) {
		return -1;
	}
	return found->second;
}
const char* SimCore::demo_product_name(int product) const {
	if (product >= 0 && product < ws_.products
			&& product < static_cast<int>(ws_.product_names.size())) {
		return ws_.product_names[static_cast<std::size_t>(product)].c_str();
	}
	return "";
}
void SimCore::demo_rebuild_tariff_lookup() {
	ws_.tariff_lookup.clear();
	ws_.tariff_lookup.reserve(ws_.tariffs.size() * 2);
	for (const DemoTariff& tariff : ws_.tariffs) {
		ws_.tariff_lookup[tariff.buyer + '|' + tariff.seller + '|'
			+ tariff.product] = std::clamp(tariff.rate, 0.0, 1.0);
	}
}
} // namespace ogs
