// C2b1 — emissão de moeda e juros do banco central (D-104).
// Ver docs/economy/C2b1_monetary_policy.md.
#include "monetary.h"

#include "central_bank.h"
#include "bank.h"
#include "city_gov.h"
#include "exchange.h"
#include "sim_core.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace ogs {
namespace monetary {

namespace {

// Caixa do governo de uma nação (orçamento nacional por cidade + prefeituras).
// Mesma enumeração de `balance::government()` antes da dívida: aqui interessa o
// CAIXA (o que o governo deixou de ter no dia = déficit medido).
double government_cash(const WorldState& ws, const std::string& nation) {
	double cash = 0.0;
	const std::size_t gov_count = std::min(ws.cities.size(),
		ws.city_gov.size());
	for (std::size_t index = 0; index < ws.cities.size(); ++index) {
		const City& city = ws.cities[index];
		if (city.nation != nation) {
			continue;
		}
		cash += city.government_budget;
		if (index < gov_count) {
			const CityGov& gov = ws.city_gov[index];
			cash += std::max(0.0, gov.cash);
			for (int slot = 0; slot < gov.project_count; ++slot) {
				cash += std::max(0.0, gov.projects[slot].reserved);
			}
		}
	}
	return cash;
}

bool has_nation(const std::vector<std::string>& nations,
		const std::string& nation) {
	return std::find(nations.begin(), nations.end(), nation) != nations.end();
}

} // namespace

int effective_independence(const WorldState& ws, const std::string& code) {
	const auto found = ws.currency_by_code.find(code);
	if (found == ws.currency_by_code.end()) {
		return kIndependenceDefault;
	}
	const Currency& currency =
		ws.currencies[static_cast<std::size_t>(found->second)];
	if (currency.bank_independence
			== central_bank::kIndependenceNoData) {
		// Ausência declarada da C2a: o default do jogo entra aqui, declarado
		// no status (`independence_saved` × `independence_effective`).
		return kIndependenceDefault;
	}
	return currency.bank_independence;
}

double effective_rate(const WorldState& ws, const std::string& code) {
	const auto found = ws.monetary.find(code);
	if (found == ws.monetary.end() || found->second.rate < 0.0) {
		return kNeutralRate;
	}
	return std::clamp(found->second.rate, kRateFloor, kRateCap);
}

double rate_for_nation(const WorldState& ws, const std::string& nation) {
	const auto found = ws.nation_currency.find(nation);
	if (found == ws.nation_currency.end()) {
		// Nação sem moeda no estado (mundo sem `set_currencies`): não há banco
		// central nem taxa — ausência declarada. O custo de capital não entra.
		return 0.0;
	}
	const Currency& currency =
		ws.currencies[static_cast<std::size_t>(found->second)];
	return effective_rate(ws, currency.code);
}

Reading reading(const WorldState& ws, const std::string& code,
	const std::map<std::string, central_bank::CpiRow>& cpi) {
	Reading out;
	const auto found = ws.currency_by_code.find(code);
	if (found == ws.currency_by_code.end()) {
		return out;
	}
	const Currency& currency =
		ws.currencies[static_cast<std::size_t>(found->second)];
	double inflation_sum = 0.0;
	double inflation_weight = 0.0;
	double gap_sum = 0.0;
	double gap_weight = 0.0;
	bool annualized_any = false;
	bool annualized_all = true;
	for (const std::string& nation : currency.nations) {
		// Inflação: média ponderada pelo peso do CPI da nação (a MESMA cesta
		// do IPC do G2). Série sem 12 meses usa a anualizada; sem série, a
		// nação não entra (ausência declarada, nada é completado).
		const auto history = ws.cpi_history.find(nation);
		if (history != ws.cpi_history.end()) {
			const central_bank::Inflation inflation =
				central_bank::inflation_of(history->second);
			const auto cpi_row = cpi.find(nation);
			const double weight = cpi_row != cpi.end()
				? std::max(0.0, cpi_row->second.weight) : 0.0;
			if (inflation.has_12m) {
				inflation_sum += inflation.rate_12m * weight;
				inflation_weight += weight;
				annualized_all = false;
			} else if (inflation.has_annualized) {
				inflation_sum += inflation.annualized * weight;
				inflation_weight += weight;
				annualized_any = true;
			}
		}
	}
	for (const City& city : ws.cities) {
		if (!has_nation(currency.nations, city.nation)) {
			continue;
		}
		const double labor = city.pop.labor_force;
		if (labor <= 1e-9) {
			continue;
		}
		const double population = std::max(0.0, city.simulation_population);
		const double unemployment = std::clamp(
			city.pop.unemployed / labor, 0.0, 1.0);
		// Hiato em pontos percentuais de força de trabalho: desemprego abaixo
		// do natural = economia aquecida (hiato positivo).
		// D-113: antes do baseline do mercado de trabalho (dia 1..29) o modelo
		// ainda está alocando a força de trabalho e a leitura não existe —
		// ausência declarada, o hiato não entra na regra.
		if (ws.day < kGapBaselineDay) {
			continue;
		}
		const double gap_pp = (ws.natural_unemployment - unemployment) * 100.0;
		gap_sum += gap_pp * population;
		gap_weight += population;
	}
	if (inflation_weight > 0.0) {
		out.has_inflation = true;
		out.inflation = inflation_sum / inflation_weight;
		out.annualized = annualized_any && annualized_all;
	}
	if (gap_weight > 0.0) {
		out.gap = gap_sum / gap_weight;
	}
	out.weight = gap_weight;
	return out;
}

double taylor_target(const Reading& in) {
	double target = kNeutralRate;
	if (in.has_inflation) {
		target += kTaylorInflationWeight * (in.inflation - kTargetInflation);
	}
	target += kTaylorGapWeight * (in.gap / 100.0);
	return std::clamp(target, kRateFloor, kRateCap);
}

double next_rate(double current, double target) {
	// D-113: a suavização declarada (0,5) é por DECISÃO e a leitura do CPI tem
	// cadência mensal (central_bank::kCpiSampleDays). Convertida ao passo diário,
	// 1 − (1 − 0,5)^(1/30) ≈ 0,0228: a taxa percorre 2,28 % do hiato por dia em
	// vez de metade (meia-vida de 1 dia, teleporte ao alvo em ~4 dias).
	const double smoothing = 1.0 - std::pow(1.0 - kRateSmoothing,
		1.0 / static_cast<double>(central_bank::kCpiSampleDays));
	const double smoothed = current + (target - current) * smoothing;
	const double step = std::clamp(smoothed - current, -kMaxRateStepPerDay,
		kMaxRateStepPerDay);
	return std::clamp(current + step, kRateFloor, kRateCap);
}

void tick(WorldState& ws, double money_total,
	const std::vector<double>& base_prices) {
	// C2b2 (D-106): `emission_today` NÃO é zerado aqui. O prologue do dia já o
	// zera e a fase de câmbio registra a emissão LÍQUIDA da intervenção antes
	// desta fase (venda de reserva contrai, compra emite). Zerar apagaria a
	// intervenção do dia e quebraria a conservação "dinheiro = anterior +
	// emissão registrada".
	const std::map<std::string, central_bank::CpiRow> cpi =
		central_bank::cpi_by_nation(ws, base_prices);
	const central_bank::MoneyAllocation money =
		central_bank::allocate_money(ws, money_total);

	for (std::size_t index = 0; index < ws.currencies.size(); ++index) {
		Currency& currency = ws.currencies[index];
		MonetaryState& state = ws.monetary[currency.code];
		state.emission_today = 0.0;
		const int independence = effective_independence(ws, currency.code);
		const double current = effective_rate(ws, currency.code);
		// 1. Taxa básica.
		if (independence == central_bank::kIndependenceControlled) {
			// Objetivo do governo declarado: juro abaixo do neutro; a emissão
			// (abaixo) cobre o déficit medido. Sem inflação o juro não reage.
			const double target = std::max(kRateFloor,
				kNeutralRate - kControlledSpread);
			state.rate = next_rate(current, target);
			state.target_rate = target;
			state.has_inflation = false;
			state.inflation_used = 0.0;
			state.gap = 0.0;
		} else {
			const Reading in = reading(ws, currency.code, cpi);
			const double target = taylor_target(in);
			state.rate = next_rate(current, target);
			state.target_rate = target;
			state.has_inflation = in.has_inflation;
			state.inflation_used = in.has_inflation ? in.inflation : 0.0;
			state.gap = in.gap;
		}
		state.last_day = ws.day;

		// 2. Emissão: só o BC CONTROLADO monetiza o déficit medido do dia,
		// até 1 % do M0 da moeda. Independente não emite (não há título
		// público anterior nem banco para redesconto: ausência declarada).
		if (independence != central_bank::kIndependenceControlled) {
			continue;
		}
		const double m0 = index < money.by_currency.size()
			? std::max(0.0, money.by_currency[index]) : 0.0;
		// C2b2 (D-106): sob o PADRÃO OURO a emissão respeita a cobertura de
		// ouro do cofre (M0 ≤ cobertura × valor do ouro). Nos demais padrões o
		// teto é infinito e nada muda (default fiduciário).
		double emission_cap = kControlledEmissionShareOfM0 * m0;
		const double headroom = exchange::emission_headroom(ws, currency, m0,
			base_prices);
		if (std::isfinite(headroom)) {
			emission_cap = std::min(emission_cap,
				std::max(0.0, headroom));
		}
		// Déficit MEDIDO do dia, por nação desta moeda: a queda do caixa do
		// governo desde o último fecho (janela contínua).
		std::vector<std::pair<std::string, double>> deficits;
		double deficit_total = 0.0;
		for (const std::string& nation : currency.nations) {
			const auto open = ws.gov_cash_open.find(nation);
			if (open == ws.gov_cash_open.end()) {
				continue;
			}
			const double now = government_cash(ws, nation);
			const double deficit = std::max(0.0, open->second - now);
			if (deficit <= 1e-9) {
				continue;
			}
			deficits.emplace_back(nation, deficit);
			deficit_total += deficit;
		}
		state.deficit_measured = deficit_total;
		double remaining = emission_cap;
		double funded = 0.0;
		for (const auto& row : deficits) {
			if (remaining <= 1e-9) {
				break;
			}
			const std::string& nation = row.first;
			const double amount = std::min(row.second, remaining);
			if (amount <= 1e-9) {
				continue;
			}
			// Crédito ao orçamento público (dinheiro criado entra no mundo) e
			// dívida pública do mesmo valor (título na carteira do BC).
			double total_population = 0.0;
			int last_city = -1;
			for (int city_index = 0;
					city_index < static_cast<int>(ws.cities.size());
					++city_index) {
				const City& city = ws.cities[static_cast<std::size_t>(city_index)];
				if (city.nation != nation) {
					continue;
				}
				last_city = city_index;
				total_population += std::max(0.0, city.simulation_population);
			}
			double credited = 0.0;
			int seen = 0;
			for (int city_index = 0;
					city_index < static_cast<int>(ws.cities.size());
					++city_index) {
				City& city = ws.cities[static_cast<std::size_t>(city_index)];
				if (city.nation != nation) {
					continue;
				}
				++seen;
				double share = 0.0;
				if (city_index == last_city) {
					share = amount - credited;
				} else {
					const double population = std::max(0.0,
						city.simulation_population);
					share = total_population > 0.0
						? amount * population / total_population
						: amount / static_cast<double>(std::max(1, seen));
				}
				share = std::max(0.0, share);
				city.government_budget += share;
				credited += share;
			}
			if (credited <= 0.0) {
				continue;
			}
			state.emission += credited;
			state.securities += credited;
			state.emission_today += credited;
			ws.gov_bonds[nation] += credited;
			ws.emission_today += credited;
			remaining -= credited;
			funded += credited;
		}
		state.unfunded = std::max(0.0, deficit_total - funded);
	}
	// C2b1: o caixa do governo no fecho (depois do crédito da emissão) é a base
	// do déficit do dia seguinte. Uma passada pelas cidades (não uma por nação).
	std::map<std::string, double> gov_cash_now;
	const std::size_t gov_count = std::min(ws.cities.size(),
		ws.city_gov.size());
	for (std::size_t index = 0; index < ws.cities.size(); ++index) {
		const City& city = ws.cities[index];
		double cash = city.government_budget;
		if (index < gov_count) {
			const CityGov& gov = ws.city_gov[index];
			cash += std::max(0.0, gov.cash);
			for (int slot = 0; slot < gov.project_count; ++slot) {
				cash += std::max(0.0, gov.projects[slot].reserved);
			}
		}
		gov_cash_now[city.nation] += cash;
	}
	ws.gov_cash_open = std::move(gov_cash_now);
}

} // namespace monetary

// 9d. C2b1 (D-104): a política monetária do dia. A taxa é decidida com a
// leitura do fecho (CPI da C2a + hiato de desemprego) e o BC controlado
// monetiza o déficit que o governo teve no dia, até o limite de senhoriagem.
void SimCore::phase_monetary() {
	std::vector<double> base_list(static_cast<std::size_t>(ws_.products), 0.0);
	for (int product = 0; product < ws_.products; ++product) {
		base_list[static_cast<std::size_t>(product)] = base_price(product);
	}
	monetary::tick(ws_, m0_total(), base_list);
}

void SimCore::phase_credit() {
	// C3 (D-107): sem crédito e sem dívida o dia não muda (o default do jogo
	// é desligado) - a fase sai sem tocar no estado.
	// C4 (D-110): a variação dos depósitos é o dinheiro BANCÁRIO criado no dia
	// (empréstimo e dividendo criam; principal, juro e calote destroem). O
	// invariante da conservação soma este valor ao `emission_today` do BC.
	double deposits_before = 0.0;
	for (const auto& row : ws_.banks) {
		deposits_before += row.second.deposits;
	}
	bank::phase(ws_);
	double deposits_after = 0.0;
	for (const auto& row : ws_.banks) {
		deposits_after += row.second.deposits;
	}
	ws_.bank_money_today += deposits_after - deposits_before;
}

} // namespace ogs
