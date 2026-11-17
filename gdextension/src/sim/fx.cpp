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
#include <cstdint>

namespace ogs {
void SimCore::phase_fx() {
	// 9c. Câmbio dinâmico: a balança comercial do dia move as moedas float
	// dentro da banda (déficit desvaloriza, superávit valoriza). Não cria nem
	// destrói dinheiro: só muda a taxa usada na próxima decisão de compra.
	tick_fx();

	OGS_SIM_PHASE_LAP(kPhaseFx);
}

// ------------------------------------------------------------------
// F5 — câmbio/tarifas consultados pela fase de serviços.
// ------------------------------------------------------------------

const ogs::Currency* SimCore::currency_for(
		const std::string& nation) const {
	const auto found = ws_.nation_currency.find(nation);
	if (found == ws_.nation_currency.end()
			|| found->second < 0
			|| found->second >= static_cast<int>(ws_.currencies.size())) {
		return nullptr;
	}
	return &ws_.currencies[static_cast<std::size_t>(found->second)];
}
double SimCore::fx_rate(const std::string& nation) const {
	const Currency* currency = currency_for(nation);
	if (currency == nullptr || !std::isfinite(currency->rate)
			|| currency->rate <= 0.0) {
		return 1.0;
	}
	return currency->rate;
}
const char* SimCore::currency_code(const std::string& nation) const {
	const Currency* currency = currency_for(nation);
	return currency != nullptr ? currency->code.c_str() : "XGS";
}
void SimCore::rebuild_currency_indexes() {
	ws_.currency_by_code.clear();
	ws_.nation_currency.clear();
	ws_.currency_by_code.reserve(ws_.currencies.size() * 2);
	for (std::size_t index = 0; index < ws_.currencies.size(); ++index) {
		Currency& currency = ws_.currencies[index];
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
void SimCore::tick_fx() {
	if (ws_.currencies.empty() || ws_.cities.empty()) {
		return;
	}
	std::vector<double> exports(ws_.currencies.size(), 0.0);
	std::vector<double> imports(ws_.currencies.size(), 0.0);
	for (const City& city : ws_.cities) {
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
	const double alpha = 1.0 / kFxBalanceEmaDays;
	for (std::size_t index = 0; index < ws_.currencies.size(); ++index) {
		Currency& currency = ws_.currencies[index];
		const double flow = exports[index] - imports[index];
		const double gross = exports[index] + imports[index];
		currency.balance_ema += (flow - currency.balance_ema) * alpha;
		currency.gross_ema += (gross - currency.gross_ema) * alpha;
		if (!currency.floating) {
			currency.rate = currency.base_rate;
			continue;
		}
		// Pressão normalizada pelo volume: um déficit que representa 10% do
		// comércio (kFxBalanceScale) satura em tanh(1) ~ 0,76. O passo
		// diário é pequeno (0,05%), então o câmbio reage à persistência da
		// balança, não a um dia isolado.
		const double scale = std::max(1.0,
			currency.gross_ema * kFxBalanceScale);
		const double pressure = std::tanh(currency.balance_ema / scale);
		const double next = currency.rate * (1.0 + kFxDailyStep * pressure);
		currency.rate = std::clamp(next,
			currency.base_rate * kFxBandLow,
			currency.base_rate * kFxBandHigh);
	}

	// O preço interno está na âncora: o passo cambial NÃO reescala preços. Só
	// a exibição (`price_local = preço ÷ taxa`) e a competitividade das
	// ofertas float (taxa ÷ taxa-base) leem a taxa corrente.
}
double SimCore::competitiveness(const std::string& nation) const {
	// Fator de competitividade aplicado à âncora (F5, opção c). Com a taxa
	// FIXA (peg) o fator é 1: o valor interno não muda. Com regime float, a
	// taxa pode desviar ±10% da referência; o fator `taxa ÷ taxa-base`
	// reproduz o efeito de preço rígido na moeda local (desvalorização barateia
	// a oferta externa do país sem reescrever o preço interno), documentado em
	// GAME_STATE §15/D-027.
	const Currency* currency = currency_for(nation);
	if (currency == nullptr || !std::isfinite(currency->rate)
			|| !std::isfinite(currency->base_rate)
			|| currency->rate <= 0.0 || currency->base_rate <= 0.0) {
		return 1.0;
	}
	if (!currency->floating) {
		return 1.0;
	}
	return std::clamp(currency->rate / currency->base_rate,
		kFxBandLow, kFxBandHigh);
}
namespace {
// A2: chave inteira da tarifa — (comprador+1)<<42 | (vendedor+1)<<21 | (produto+1).
// -1 codifica o curinga '*'. Os índices são < 2^21 (nações) e < 2^21 (produtos).
inline std::uint64_t tariff_key(int buyer, int seller, int product) {
	return (static_cast<std::uint64_t>(buyer + 1) << 42)
		| (static_cast<std::uint64_t>(seller + 1) << 21)
		| static_cast<std::uint64_t>(product + 1);
}
}  // namespace

double SimCore::tariff_rate(const std::string& buyer_nation,
		const std::string& seller_nation, int product) const {
	if (ws_.tariff_lookup.empty() || product < 0 || product >= ws_.products) {
		return 0.0;
	}
	const int buyer = ws_.nation_table.find(buyer_nation);
	const int seller = ws_.nation_table.find(seller_nation);
	// Tabela por índice incompleta (linha com nação/produto não resolvível) ou
	// nação fora da tabela: o resultado do caminho textual é o contrato.
	if (buyer < 0 || seller < 0
			|| ws_.tariff_by_index.size() != ws_.tariff_lookup.size()) {
		return tariff_rate_text(buyer_nation, seller_nation, product);
	}
	return tariff_rate_at_nation(buyer, seller, product);
}

double SimCore::tariff_rate_at_nation(int buyer_nation, int seller_nation,
		int product) const {
	if (ws_.tariff_lookup.empty() || product < 0 || product >= ws_.products
			|| ws_.tariff_by_index.size() != ws_.tariff_lookup.size()) {
		return 0.0;
	}
	// Mesma ordem de resolução do caminho textual: (b,s,p), (b,*,p), (b,s,*),
	// (b,*,*). O primeiro que existir vence.
	const std::uint64_t keys[4] = {
		tariff_key(buyer_nation, seller_nation, product),
		tariff_key(buyer_nation, -1, product),
		tariff_key(buyer_nation, seller_nation, -1),
		tariff_key(buyer_nation, -1, -1),
	};
	for (int index = 0; index < 4; ++index) {
		const auto found = ws_.tariff_by_index.find(keys[index]);
		if (found != ws_.tariff_by_index.end()) {
			return std::clamp(found->second, 0.0, 1.0);
		}
	}
	return 0.0;
}

// A2: tarifa no laço quente do comércio — índices inteiros quando a tabela por
// índice está completa; senão o caminho textual (o resultado é o mesmo).
double SimCore::tariff_rate_cities(const City& buyer_city,
		const City& seller_city, int product) const {
	if (!ws_.tariff_lookup.empty()
			&& ws_.tariff_by_index.size() == ws_.tariff_lookup.size()
			&& buyer_city.nation_index >= 0 && seller_city.nation_index >= 0) {
		return tariff_rate_at_nation(buyer_city.nation_index,
			seller_city.nation_index, product);
	}
	return tariff_rate(buyer_city.nation, seller_city.nation, product);
}

double SimCore::tariff_rate_text(const std::string& buyer_nation,
		const std::string& seller_nation, int product) const {
	if (ws_.tariff_lookup.empty() || product < 0 || product >= ws_.products) {
		return 0.0;
	}
	const std::string good = product_name(product);
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
int SimCore::product_index(const std::string& product) const {
	const auto found = ws_.product_lookup.find(product);
	if (found == ws_.product_lookup.end()) {
		return -1;
	}
	return found->second;
}
const char* SimCore::product_name(int product) const {
	if (product >= 0 && product < ws_.products
			&& product < static_cast<int>(ws_.product_names.size())) {
		return ws_.product_names[static_cast<std::size_t>(product)].c_str();
	}
	return "";
}
void SimCore::rebuild_tariff_lookup() {
	ws_.tariff_lookup.clear();
	ws_.tariff_lookup.reserve(ws_.tariffs.size() * 2);
	// A2: índice inteiro na mesma passada. Uma linha só entra quando TODAS as
	// suas chaves são resolvíveis (nação no NameTable, produto no product_lookup);
	// se alguma não for, o mapa fica menor que tariff_lookup e tariff_rate usa o
	// caminho textual — ausência explícita, nunca um resultado diferente.
	ws_.tariff_by_index.clear();
	ws_.tariff_by_index.reserve(ws_.tariffs.size() * 2);
	for (const Tariff& tariff : ws_.tariffs) {
		ws_.tariff_lookup[tariff.buyer + '|' + tariff.seller + '|'
			+ tariff.product] = std::clamp(tariff.rate, 0.0, 1.0);
	}
	if (ws_.nation_table.size() == 0
			|| ws_.product_lookup.size() < static_cast<std::size_t>(ws_.products)) {
		return;
	}
	for (const Tariff& tariff : ws_.tariffs) {
		int buyer = -1;
		if (tariff.buyer != "*") {
			buyer = ws_.nation_table.find(tariff.buyer);
			if (buyer < 0) {
				continue;  // nação fora da tabela: caminho textual
			}
		}
		int seller = -1;
		if (tariff.seller != "*") {
			seller = ws_.nation_table.find(tariff.seller);
			if (seller < 0) {
				continue;
			}
		}
		int product = -1;
		if (tariff.product != "*") {
			const auto found = ws_.product_lookup.find(tariff.product);
			if (found == ws_.product_lookup.end()) {
				continue;  // produto fora do catálogo: caminho textual
			}
			product = found->second;
		}
		ws_.tariff_by_index[tariff_key(buyer, seller, product)] =
			std::clamp(tariff.rate, 0.0, 1.0);
	}
}
} // namespace ogs
