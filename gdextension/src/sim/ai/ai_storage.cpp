// G2 — IA de armazém (privado + governo). Ver ai_storage.h e
// docs/economy/storage_g2.md. C++ puro; nenhuma leitura do WorldState vivo:
// tudo sai da `AiView` congelada do dia.
#include "ai_storage.h"

#include "ai_phase.h"
#include "ai_view.h"
#include "../sim_constants.h"
#include "../storage.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace ogs {
namespace {

std::uint64_t price_key(int city_index, int product) {
	return (static_cast<std::uint64_t>(city_index) << 32)
		| static_cast<std::uint64_t>(product);
}

double safe_price(double value) {
	return std::isfinite(value) && value > 0.0 ? value : 0.0;
}

} // namespace

void AiStorageInvestor::observe_prices(AiContext& ctx,
		int city_index) {
	for (int product = 0; product < ctx.view.products; ++product) {
		const double price = safe_price(ai_market(ctx.view, city_index,
			product, kAiPrice));
		if (price <= 0.0) {
			continue;
		}
		std::vector<double>& history =
			ctx.state.storage_prices[price_key(city_index, product)];
		history.push_back(price);
		if (history.size() > 365) {
			history.erase(history.begin());
		}
	}
}

double AiStorageInvestor::reference_price(const AiContext& ctx,
		int city_index, int product) const {
	const auto found = ctx.state.storage_prices.find(
		price_key(city_index, product));
	if (found == ctx.state.storage_prices.end() || found->second.empty()) {
		return 0.0;
	}
	double sum = 0.0;
	for (const double value : found->second) {
		sum += value;
	}
	return sum / static_cast<double>(found->second.size());
}

double AiStorageInvestor::days_to_harvest(const AiContext& ctx,
		int city_index) const {
	if (city_index < 0 || city_index >= ctx.view.cities) {
		return -1.0;
	}
	return ctx.view.next_harvest_days[static_cast<std::size_t>(city_index)];
}

bool AiStorageInvestor::harvest_active(const AiContext& ctx,
		int city_index) const {
	if (city_index < 0 || city_index >= ctx.view.cities) {
		return false;
	}
	return ctx.view.harvest_active[static_cast<std::size_t>(city_index)] != 0;
}

bool AiStorageInvestor::private_decisions(AiContext& ctx, int city_index) {
	const std::string& city = ctx.view.city_rows[
		static_cast<std::size_t>(city_index)].id;
	const double satisfaction = ai_pop(ctx.view, city_index, kAiSatisfaction);
	const double gini = ctx.view.city_rows[
		static_cast<std::size_t>(city_index)].income_gini;
	const bool speculator = gini > kStorageSpeculatorGini;
	const double days_to = days_to_harvest(ctx, city_index);
	const bool harvesting = harvest_active(ctx, city_index);

	// 1. Anti-fome: satisfação baixa -> libera a comida guardada (socorro,
	// sem pagamento). O privado não especula contra a mesa da cidade.
	if (satisfaction < kStoragePrivateReleaseSatisfaction) {
		for (int product = 0; product < ctx.view.products; ++product) {
			const int type = ctx.view.product_storage_type[
				static_cast<std::size_t>(product)];
			if (type < 0 || type >= kStorageTypeCount
					|| storage_type_is_structure_only(type)
					|| ai_food_calories(ctx.view, product) <= 0.0) {
				continue;
			}
			const double held = ai_storage_stock(ctx.view, city_index, type,
				STORAGE_PRIVATE);
			if (held <= 1e-6) {
				continue;
			}
			if (ai_emit_storage_trade(ctx, city, type, STORAGE_PRIVATE,
					product, held, false, true).created) {
				return true;
			}
		}
	}

	// 2. Construção por payback (spread × volume da região).
	for (int type = 0; type < kStorageTypeCount; ++type) {
		const double annual = ai_storage_annual(ctx.view, city_index, type);
		if (annual <= 0.0 || storage_type_is_structure_only(type)) {
			continue;
		}
		const double capacity = ai_storage_capacity(ctx.view, city_index,
			type, STORAGE_PRIVATE);
		const double target = annual * kStorageInitialMonths / 12.0;
		if (target <= 0.0 || capacity >= target * 0.5) {
			continue;
		}
		const std::string key = city + "|" + std::to_string(type)
			+ "|private";
		const int last = ctx.state.storage_last_build_day.count(key) > 0
			? ctx.state.storage_last_build_day[key] : -100000;
		if (ctx.day - last < 60) {
			continue;
		}
		// Preço de referência do tipo: o primeiro produto do tipo com histórico
		// (ou o preço corrente, quando o histórico ainda não existe).
		double ref = 0.0;
		int observed_product = -1;
		for (int product = 0; product < ctx.view.products; ++product) {
			if (ctx.view.product_storage_type[
					static_cast<std::size_t>(product)] != type) {
				continue;
			}
			if (observed_product < 0) {
				observed_product = product;
			}
			const double price = reference_price(ctx, city_index, product);
			if (price > 0.0) {
				ref = price;
				break;
			}
			const double current = safe_price(ai_market(ctx.view, city_index,
				product, kAiPrice));
			if (current > 0.0) {
				ref = current;
				break;
			}
		}
		if (ref <= 0.0) {
			continue;
		}
		// Spread observado (máx-mín)/média do histórico; sem histórico, o
		// spread declarado do tipo.
		double spread = kStorageExpectedSpread[type];
		if (observed_product >= 0) {
			const auto found = ctx.state.storage_prices.find(
				price_key(city_index, observed_product));
			if (found != ctx.state.storage_prices.end()
					&& found->second.size() >= 21) {
				double lo = found->second[0];
				double hi = found->second[0];
				double sum = 0.0;
				for (const double value : found->second) {
					lo = std::min(lo, value);
					hi = std::max(hi, value);
					sum += value;
				}
				const double mean = sum / static_cast<double>(
					found->second.size());
				if (mean > 0.0) {
					spread = std::max(spread, (hi - lo) / mean);
				}
			}
		}
		const double build_tons = std::min(target, capacity > 0.0
			? target - capacity : target);
		if (build_tons <= 0.0) {
			continue;
		}
		const double gross = build_tons * ref * spread;
		const double build_cost = build_tons * kStorageCostPerTon[type];
		const double annual_maintenance = build_tons
			* kStorageMaintenancePerTonDay[type] * 365.0;
		const double annual_loss = build_tons * ref * 0.05;
		const double years = std::max(1.0, kStoragePrivatePaybackYears);
		if (gross * years < build_cost
				+ (annual_maintenance + annual_loss) * years) {
			continue;
		}
		const double affordable = std::max(0.0,
			ctx.view.city_rows[static_cast<std::size_t>(city_index)].firm_cash)
			* kStoragePrivateBuildCashShare;
		const double tons = std::min(build_tons,
			affordable / kStorageCostPerTon[type]);
		if (tons <= 0.0) {
			continue;
		}
		const AiActionResult result = ai_emit_storage_build(ctx, city, type,
			STORAGE_PRIVATE, tons);
		if (result.created) {
			ctx.state.storage_last_build_day[key] = ctx.day;
			return true;
		}
	}

	// 3. Guardar/vender pela próxima colheita e pelo histórico de preço.
	const double sell_days = speculator ? kStorageSpeculatorSellDays
		: kStorageConservativeSellDays;
	for (int product = 0; product < ctx.view.products; ++product) {
		const int type = ctx.view.product_storage_type[
			static_cast<std::size_t>(product)];
		if (type < 0 || type >= kStorageTypeCount
				|| storage_type_is_structure_only(type)) {
			continue;
		}
		const double price = safe_price(ai_market(ctx.view, city_index,
			product, kAiPrice));
		const double ref = reference_price(ctx, city_index, product);
		if (price <= 0.0 || ref <= 0.0) {
			continue;
		}
		const bool food = ai_food_calories(ctx.view, product) > 0.0;
		const double held = ai_storage_stock(ctx.view, city_index, type,
			STORAGE_PRIVATE);
		const double capacity = ai_storage_capacity(ctx.view, city_index, type,
			STORAGE_PRIVATE);
		const double stock = ai_market(ctx.view, city_index, product,
			kAiStock);
		if (harvesting && price <= ref * 0.98
				&& (!food || satisfaction >= kStoragePrivateBuySatisfactionFloor)
				&& held < capacity) {
			const double budget = ctx.view.city_rows[
				static_cast<std::size_t>(city_index)].firm_cash
				* kStoragePrivateCashShare;
			const double tons = std::min(std::min(stock * 0.05,
				capacity - held), budget / price);
			if (tons > 1e-6 && ai_emit_storage_trade(ctx, city, type,
					STORAGE_PRIVATE, product, tons, true, false).created) {
				return true;
			}
		}
		if (!harvesting && days_to >= 0.0 && days_to <= sell_days
				&& price >= ref * 1.03 && held > 1e-6) {
			const double tons = held * 0.25;
			if (tons > 1e-6 && ai_emit_storage_trade(ctx, city, type,
					STORAGE_PRIVATE, product, tons, false, false).created) {
				return true;
			}
		}
	}
	return false;
}

bool AiStorageInvestor::government_decisions(AiContext& ctx,
		int city_index) {
	const std::string& city = ctx.view.city_rows[
		static_cast<std::size_t>(city_index)].id;
	const double satisfaction = ai_pop(ctx.view, city_index, kAiSatisfaction);
	const bool harvesting = harvest_active(ctx, city_index);
	const double budget = ctx.view.city_rows[
		static_cast<std::size_t>(city_index)].government_budget;

	// 1. Crise alimentar: libera reserva sem pagamento (o mercado recebe a
	// tonelada; nenhuma moeda é criada).
	if (satisfaction < kStorageReliefSatisfaction) {
		for (int product = 0; product < ctx.view.products; ++product) {
			const int type = ctx.view.product_storage_type[
				static_cast<std::size_t>(product)];
			if (type < 0 || type >= kStorageTypeCount
					|| storage_type_is_structure_only(type)
					|| ai_food_calories(ctx.view, product) <= 0.0) {
				continue;
			}
			const double held = ai_storage_stock(ctx.view, city_index, type,
				STORAGE_GOVERNMENT);
			if (held <= 1e-6) {
				continue;
			}
			if (ai_emit_storage_trade(ctx, city, type, STORAGE_GOVERNMENT,
					product, held * 0.5, false, true).created) {
				return true;
			}
		}
	}

	for (int product = 0; product < ctx.view.products; ++product) {
		const int type = ctx.view.product_storage_type[
			static_cast<std::size_t>(product)];
		if (type < 0 || type >= kStorageTypeCount
				|| storage_type_is_structure_only(type)) {
			continue;
		}
		const double price = safe_price(ai_market(ctx.view, city_index,
			product, kAiPrice));
		const double ref = reference_price(ctx, city_index, product);
		if (price <= 0.0 || ref <= 0.0) {
			continue;
		}
		const double capacity = ai_storage_capacity(ctx.view, city_index, type,
			STORAGE_GOVERNMENT);
		const double held = ai_storage_stock(ctx.view, city_index, type,
			STORAGE_GOVERNMENT);
		const bool food = ai_food_calories(ctx.view, product) > 0.0;
		const bool food_secure =
			satisfaction >= kStorageGovernmentBuySatisfactionFloor;
		// Compra só do EXCEDENTE declarado pelo mercado (`surplus` = estoque
		// acima de 3 dias de demanda) e, em comida, só onde a cidade já come
		// com folga: ilha/micro-mercado não perde comida para a reserva.
		// `surplus > 0` é o próprio teste de "mercado minúsculo" — mercado sem
		// sobra não vende nada ao governo.
		const double surplus = ai_market(ctx.view, city_index, product,
			kAiSurplus);
		if (harvesting && price <= ref * kStorageGovernmentLowPrice
				&& budget > 0.0 && held < capacity && surplus > 0.0
				&& (!food || food_secure)) {
			const double tons = std::min(std::min(surplus
				* kStorageGovernmentBuyShare, capacity - held),
				budget / price);
			if (tons > 1e-6 && ai_emit_storage_trade(ctx, city, type,
					STORAGE_GOVERNMENT, product, tons, true, false).created) {
				return true;
			}
		}
		// Libera onde falta: comida guardada volta de graça ao mercado da
		// cidade com satisfação abaixo do piso de compra (a reserva existe
		// para a mesa, não para o silo).
		if (food && !food_secure && held > 1e-6) {
			const double tons = held * kStorageGovernmentReleaseShare;
			if (tons > 1e-6 && ai_emit_storage_trade(ctx, city, type,
					STORAGE_GOVERNMENT, product, tons, false, true).created) {
				return true;
			}
		}
		// Libera na entressafra/preço alto: devolve ao mercado local.
		if (!harvesting && price >= ref * kStorageGovernmentHighPrice
				&& held > 1e-6) {
			const double tons = held * kStorageGovernmentReleaseShare;
			if (tons > 1e-6 && ai_emit_storage_trade(ctx, city, type,
				STORAGE_GOVERNMENT, product, tons, false, false).created) {
				return true;
			}
		}
	}
	return false;
}

void AiStorageInvestor::run_city(AiContext& ctx, int city_index) {
	if (city_index < 0 || city_index >= ctx.view.cities) {
		return;
	}
	observe_prices(ctx, city_index);
	private_decisions(ctx, city_index);
	government_decisions(ctx, city_index);
}

} // namespace ogs
