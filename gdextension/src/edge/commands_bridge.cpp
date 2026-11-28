#include "commands_bridge.h"

#include "economy_sim.h"
#include "sim/bank.h"
#include "sim/central_bank.h"
#include "sim/crisis.h"

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace godot {

namespace {

// Hash da thread corrente (mesmo corpo do de `economy_sim.cpp`): separa a
// thread do dia da de fora em `dispatch_command` (A3). Duplicado inline no
// namespace anonimo, como o `safe_nonneg` do H1a: mesma semantica, mesmo
// resultado, sem abrir membro privado.
std::size_t current_thread_hash() {
	return std::hash<std::thread::id>{}(std::this_thread::get_id());
}

} // namespace

// ---------------------------------------------------------------------------
// A3 - fila de comandos (sim/commands.h; racional em
// docs/economy/refactor/A3_commands.md).
//
// Cada mÃ©todo que muda o mundo (jogador ou IA) agora sÃ³ monta um Command,
// enfileira e pede a aplicaÃ§Ã£o. A ordem de aplicaÃ§Ã£o Ã© a de emissÃ£o; a fila Ã©
// aplicada (a) no inÃ­cio do dia, antes do prÃ³logo (tick_core), e (b) na hora,
// quando o mÃ©todo precisa devolver o efeito (contrato de retorno de invest,
// build, group_invest, ...) - esse Ã© o Ãºnico caminho sÃ­ncrono.
// ---------------------------------------------------------------------------

namespace {

// Log/dict <-> Command. O log Ã© a evidÃªncia do replay e sai em JSON pelo
Dictionary command_to_dict(const ogs::Command& cmd) {
	Dictionary entry;
	const char* name = ogs::command_type_name(cmd.type);
	entry["type"] = String(name != nullptr ? name : "");
	entry["day"] = cmd.day;
	entry["apply_day"] = cmd.apply_day;
	entry["seq"] = static_cast<int64_t>(cmd.seq);
	Array strings;
	for (int slot = 0; slot < ogs::kCommandStrings; ++slot) {
		if (cmd.s[slot].empty()) {
			break;
		}
		strings.push_back(String(cmd.s[slot].c_str()));
	}
	if (!strings.is_empty()) {
		entry["s"] = strings;
	}
	bool has_double = false;
	for (int slot = 0; slot < ogs::kCommandDoubles; ++slot) {
		if (cmd.v[slot] != 0.0) {
			has_double = true;
		}
	}
	if (has_double) {
		Array doubles;
		doubles.resize(ogs::kCommandDoubles);
		for (int slot = 0; slot < ogs::kCommandDoubles; ++slot) {
			doubles[slot] = cmd.v[slot];
		}
		entry["v"] = doubles;
	}
	bool has_int = false;
	for (int slot = 0; slot < ogs::kCommandInts; ++slot) {
		if (cmd.i[slot] != 0) {
			has_int = true;
		}
	}
	if (has_int) {
		Array ints;
		ints.resize(ogs::kCommandInts);
		for (int slot = 0; slot < ogs::kCommandInts; ++slot) {
			ints[slot] = cmd.i[slot];
		}
		entry["i"] = ints;
	}
	if (cmd.b) {
		entry["b"] = true;
	}
	return entry;
}

bool command_from_dict(const Dictionary& entry, ogs::Command& out) {
	ogs::CommandType type = ogs::CommandType::None;
	const String type_name = entry.get("type", String());
	if (!ogs::command_type_from_name(
			std::string(type_name.utf8().get_data()), type)) {
		return false;
	}
	out = ogs::Command();
	out.type = type;
	out.day = static_cast<int>(entry.get("day", 0));
	out.apply_day = static_cast<int>(entry.get("apply_day", 0));
	out.seq = static_cast<uint64_t>(static_cast<int64_t>(entry.get("seq", 0)));
	if (entry.has("s")) {
		const Array strings = entry.get("s", Array());
		for (int slot = 0; slot < strings.size() && slot < ogs::kCommandStrings;
				++slot) {
			out.s[slot] = std::string(String(strings[slot]).utf8().get_data());
		}
	}
	if (entry.has("v")) {
		const Array doubles = entry.get("v", Array());
		for (int slot = 0; slot < doubles.size() && slot < ogs::kCommandDoubles;
				++slot) {
			out.v[slot] = static_cast<double>(doubles[slot]);
		}
	}
	if (entry.has("i")) {
		const Array ints = entry.get("i", Array());
		for (int slot = 0; slot < ints.size() && slot < ogs::kCommandInts; ++slot) {
			out.i[slot] = static_cast<int>(ints[slot]);
		}
	}
	out.b = static_cast<bool>(entry.get("b", false));
	return true;
}

} // namespace

Dictionary EconomySim::dispatch_command(const ogs::Command& cmd) {
	// Dia em andamento em OUTRA thread (tick_scheduler): a aÃ§Ã£o sÃ³ entra na
	// fila. Nada do `ws_` Ã© tocado aqui, entÃ£o nÃ£o hÃ¡ disputa com o escritor
	// Ãºnico - Ã© o caminho que dispensa o mutex recursivo. O efeito chega no
	// inÃ­cio do prÃ³ximo dia (`apply_pending_locked` em tick_core).
	//
	// Na PRÃ“PRIA thread do dia (seed_industry chama build, por exemplo) a
	// mutaÃ§Ã£o aplica na hora: o tick Ã© o escritor, jÃ¡ estÃ¡ sob o mutex e a
	// ordem interna do dia Ã© parte do resultado.
	const bool in_tick_thread = tick_thread_.load(std::memory_order_acquire)
		== current_thread_hash();
	if (tick_in_progress_.load(std::memory_order_acquire) && !in_tick_thread) {
		ogs::Command queued = cmd;
		queued.day = current_day_.load(std::memory_order_relaxed);
		const uint64_t seq = commands_.push(queued);
		Dictionary deferred;
		deferred["queued"] = true;
		deferred["seq"] = static_cast<int64_t>(seq);
		deferred["apply_day"] = queued.day + 1;
		return deferred;
	}
	// Fora do dia: aplica agora, na ordem de emissÃ£o. Comandos anteriores na
	// fila (se houver) saem antes - Ã© o que garante a ordem determinÃ­stica.
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	ogs::Command accepted = cmd;
	accepted.day = ws_.day;
	const uint64_t seq = commands_.push(accepted);
	return apply_pending_locked(seq);
}

Dictionary EconomySim::apply_pending_locked(uint64_t wanted_seq) {
	Dictionary receipt;
	const std::vector<ogs::Command> pending = commands_.take_pending();
	if (pending.empty()) {
		return receipt;
	}
	for (const ogs::Command& pending_cmd : pending) {
		// CÃ³pia: o log grava o dia em que o comando FOI aplicado. Os emitidos
		// entre dias aplicam no mesmo dia; os emitidos com o dia em andamento
		// (outra thread) aplicam no inÃ­cio do dia seguinte - o replay usa
		// `apply_day`, nÃ£o `day`, para acertar os dois casos.
		ogs::Command cmd = pending_cmd;
		cmd.apply_day = ws_.day;
		const Dictionary result = apply_one_locked(cmd);
		commands_.log(cmd);
		if (wanted_seq != 0 && cmd.seq == wanted_seq) {
			receipt = result;
		}
	}
	// Ponto ÃšNICO de invalidaÃ§Ã£o do lote (os aplicadores nÃ£o invalidam mais).
	if (!tick_in_progress_.load(std::memory_order_relaxed)) {
		status_invalidate();
	}
	return receipt;
}

void EconomySim::apply_commands() {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	apply_pending_locked(0);
}

int EconomySim::pending_command_count() const {
	return static_cast<int>(commands_.pending_size());
}

Array EconomySim::get_command_log() const {
	Array out;
	const std::vector<ogs::Command> entries = commands_.log_entries();
	out.resize(static_cast<int>(entries.size()));
	for (int index = 0; index < out.size(); ++index) {
		out[index] = command_to_dict(entries[static_cast<std::size_t>(index)]);
	}
	return out;
}

void EconomySim::clear_command_log() {
	commands_.clear_log();
}

bool EconomySim::command_log_truncated() const {
	return commands_.truncated();
}

bool EconomySim::replay_commands(const Array& entries, int target_day) {
	const std::lock_guard<std::recursive_mutex> lock(mutex_);
	for (int index = 0; index < entries.size(); ++index) {
		ogs::Command cmd;
		if (!command_from_dict(entries[index], cmd)) {
			return false;
		}
		// O relÃ³gio do replay Ã© o dia em que o comando FOI aplicado (nÃ£o o dia
		// em que foi emitido): os dois coincidem para as aÃ§Ãµes entre dias e
		// diferem em 1 para as que esperaram a fila (dia em andamento).
		const int when = cmd.apply_day > 0 ? cmd.apply_day : cmd.day;
		while (ws_.day < when) {
			tick_core();
		}
		commands_.push(cmd);
		apply_pending_locked(0);
	}
	while (ws_.day < target_day) {
		tick_core();
	}
	return true;
}

// ---------------------------------------------------------------------------
// A3 - aplicaÃ§Ã£o de um comando jÃ¡ tirado da fila. Roda sempre sob o mutex do
// escritor; nÃ£o enfileira nada (os `apply_*` sÃ£o os corpos antigos).
// ---------------------------------------------------------------------------

Dictionary EconomySim::apply_one_locked(const ogs::Command& cmd) {
	Dictionary result;
	switch (cmd.type) {
		case ogs::CommandType::Invest:
			return apply_invest(String(cmd.s[0].c_str()), String(cmd.s[1].c_str()),
				cmd.v[0]);
		case ogs::CommandType::Build:
			return apply_build(String(cmd.s[0].c_str()), String(cmd.s[1].c_str()),
				String(cmd.s[2].c_str()), cmd.i[0], cmd.i[1], cmd.v[0], cmd.v[1],
				cmd.v[2], cmd.v[3], cmd.v[4], String(cmd.s[3].c_str()), cmd.v[5],
				String(cmd.s[4].c_str()), cmd.b);
		case ogs::CommandType::SpendGovernment:
			result["ok"] = apply_spend_government(String(cmd.s[0].c_str()), cmd.v[0]);
			return result;
		case ogs::CommandType::PriceShock:
			apply_price_shock(String(cmd.s[0].c_str()), cmd.v[0]);
			return result;
		case ogs::CommandType::SetSuitability:
			apply_set_suitability(String(cmd.s[0].c_str()),
				String(cmd.s[1].c_str()), cmd.v[0]);
			return result;
		case ogs::CommandType::SetDeposit:
			result["ok"] = apply_set_deposit(String(cmd.s[0].c_str()),
				String(cmd.s[1].c_str()), cmd.v[0], cmd.v[1], cmd.v[2]);
			return result;
		case ogs::CommandType::SetExternalDemand:
			apply_set_external_demand(String(cmd.s[0].c_str()),
				String(cmd.s[1].c_str()), cmd.v[0]);
			return result;
		case ogs::CommandType::SetExternalIncomeIndex:
			apply_set_external_income_index(String(cmd.s[0].c_str()), cmd.v[0]);
			return result;
		case ogs::CommandType::SetProductivityIndex:
			apply_set_productivity_index(String(cmd.s[0].c_str()), cmd.v[0]);
			return result;
		case ogs::CommandType::SetIncomeGini:
			apply_set_income_gini(String(cmd.s[0].c_str()), cmd.v[0]);
			return result;
		case ogs::CommandType::SetMinimumWage:
			apply_set_minimum_wage(String(cmd.s[0].c_str()), cmd.v[0]);
			return result;
		case ogs::CommandType::SetWageRigidity:
			apply_set_wage_rigidity(cmd.v[0]);
			return result;
		case ogs::CommandType::SetTariff:
			apply_set_tariff(String(cmd.s[0].c_str()), String(cmd.s[1].c_str()),
				String(cmd.s[2].c_str()), cmd.v[0]);
			return result;
		case ogs::CommandType::SetCurrencyRate:
			apply_set_currency_rate(String(cmd.s[0].c_str()), cmd.v[0], cmd.b,
				cmd.v[1]);
			return result;
		case ogs::CommandType::ExpandPort:
			return apply_expand_port(String(cmd.s[0].c_str()), cmd.v[0]);
		case ogs::CommandType::SetPortCapacity:
			apply_set_port_capacity(String(cmd.s[0].c_str()), cmd.v[0]);
			return result;
		case ogs::CommandType::SetMarketStock:
			apply_set_market_stock(String(cmd.s[0].c_str()),
				String(cmd.s[1].c_str()), cmd.v[0]);
			return result;
		case ogs::CommandType::SetMarketPrice:
			apply_set_market_price(String(cmd.s[0].c_str()),
				String(cmd.s[1].c_str()), cmd.v[0]);
			return result;
		// D-125: eficiência de produção (troca de produto/modo/equipamento).
		case ogs::CommandType::ChangeProduct:
			return apply_change_product(String(cmd.s[0].c_str()),
				String(cmd.s[1].c_str()), String(cmd.s[2].c_str()),
				String(cmd.s[3].c_str()));
		case ogs::CommandType::SetRefineryMode:
			return apply_set_refinery_mode(String(cmd.s[0].c_str()),
				String(cmd.s[1].c_str()), String(cmd.s[2].c_str()));
		case ogs::CommandType::SetBuildingEquipment:
			return apply_set_building_equipment(String(cmd.s[0].c_str()),
				String(cmd.s[1].c_str()), cmd.v[0]);
		case ogs::CommandType::FleetBuy:
			return apply_fleet_buy(String(cmd.s[0].c_str()),
				String(cmd.s[1].c_str()), cmd.v[0], cmd.v[1]);
		case ogs::CommandType::FundGroup:
			return apply_fund_group(String(cmd.s[0].c_str()), cmd.v[0]);
		case ogs::CommandType::GroupInvest:
			return apply_group_invest(String(cmd.s[0].c_str()),
				String(cmd.s[1].c_str()), String(cmd.s[2].c_str()),
				String(cmd.s[3].c_str()), cmd.i[0], cmd.v[0],
				String(cmd.s[4].c_str()), String(cmd.s[5].c_str()));
		case ogs::CommandType::SetFdiPolicy:
			return apply_set_fdi_policy(String(cmd.s[0].c_str()),
				String(cmd.s[1].c_str()), String(cmd.s[2].c_str()), cmd.v[0]);
		case ogs::CommandType::SetSectorIncentive:
			return apply_set_sector_incentive(String(cmd.s[0].c_str()),
				String(cmd.s[1].c_str()), cmd.v[0], cmd.v[1]);
		case ogs::CommandType::CreateStateCompany:
			return apply_create_state_company(String(cmd.s[0].c_str()),
				String(cmd.s[1].c_str()), cmd.v[0]);
		case ogs::CommandType::CharterPublicBank:
			return apply_charter_public_bank(String(cmd.s[0].c_str()),
				cmd.v[0]);
		case ogs::CommandType::Privatize:
			return apply_privatize(String(cmd.s[0].c_str()),
				String(cmd.s[1].c_str()), String(cmd.s[2].c_str()));
		case ogs::CommandType::Nationalize:
			return apply_nationalize(String(cmd.s[0].c_str()),
				String(cmd.s[1].c_str()));
		case ogs::CommandType::Concede:
			return apply_concede(String(cmd.s[0].c_str()),
				String(cmd.s[1].c_str()), String(cmd.s[2].c_str()), cmd.v[0]);
		case ogs::CommandType::UrbanExpand:
			return apply_urban_expand(String(cmd.s[0].c_str()), cmd.i[0], cmd.v[0]);
		case ogs::CommandType::UrbanDensify:
			return apply_urban_densify(String(cmd.s[0].c_str()), cmd.i[0]);
		case ogs::CommandType::UrbanSocialHousing:
			return apply_urban_social_housing(String(cmd.s[0].c_str()), cmd.v[0]);
		case ogs::CommandType::SetServiceCapacity:
			apply_set_service_capacity(String(cmd.s[0].c_str()),
				String(cmd.s[1].c_str()), cmd.v[0]);
			return result;
		case ogs::CommandType::SetServiceBusinessState:
			apply_set_service_business_state(cmd.i[0], cmd.i[1], cmd.v[0]);
			return result;
		case ogs::CommandType::DeepenMine:
			return apply_deepen_mine(String(cmd.s[0].c_str()),
				String(cmd.s[1].c_str()));
		case ogs::CommandType::InfraBuild:
			return apply_infra_build(String(cmd.s[0].c_str()), cmd.i[0]);
		case ogs::CommandType::StorageBuild:
			return apply_storage_build(String(cmd.s[0].c_str()), cmd.i[0],
				cmd.i[1], cmd.v[0]);
		case ogs::CommandType::StorageTrade:
			return apply_storage_trade(String(cmd.s[0].c_str()), cmd.i[0],
				cmd.i[1], static_cast<int>(cmd.v[1]), cmd.v[0], cmd.b,
				cmd.s[1] == "relief");
		case ogs::CommandType::SetExchangeRegime: {
			// C2b2 (D-106): regime salvo + paridade/banda declaradas. Sem
			// paridade (v[0] = 0) vale o `base_rate` do dado do F5.
			Dictionary out;
			const auto found = ws_.currency_by_code.find(cmd.s[0]);
			if (found == ws_.currency_by_code.end()) {
				out["ok"] = false;
				out["error"] = "moeda desconhecida";
				return out;
			}
			Currency& currency = ws_.currencies[
				static_cast<std::size_t>(found->second)];
			// D-108(c): sÃ³ o EMISSOR muda regime/padrÃ£o da prÃ³pria moeda;
			// moeda de uniÃ£o fica travada e naÃ§Ã£o dolarizada nÃ£o tem polÃ­tica.
			const auto info = ws_.fx_regime.find(currency.code);
			if (info != ws_.fx_regime.end()) {
				if (info->second.locked) {
					out["ok"] = false;
					out["error"] = "moeda de uniao â€” regras de bloco futuras";
					return out;
				}
				if (!cmd.s[2].empty()) {
					const bool no_policy = std::find(
						ws_.fx_no_policy_nations.begin(),
						ws_.fx_no_policy_nations.end(), cmd.s[2])
						!= ws_.fx_no_policy_nations.end();
					if (no_policy) {
						out["ok"] = false;
						out["error"] = "nacao sem moeda propria: sem politica "
							"cambial (dolarizada)";
						return out;
					}
					if (!info->second.issuer.empty()
							&& cmd.s[2] != info->second.issuer) {
						out["ok"] = false;
						out["error"] = "somente o emissor da moeda pode mudar "
							"o regime";
						return out;
					}
				}
			}
			const int regime = ogs::central_bank::regime_from_name(cmd.s[1],
				currency.exchange_regime);
			currency.exchange_regime = regime;
			// O toggle do F5 acompanha: o regime Ã© a fonte, agora que ele age.
			currency.floating = regime
				!= ogs::central_bank::kRegimePeg;
			// O espelho de estado acompanha o comando: o load restaura
			// `currency.exchange_regime` a partir de `fx_regime`
			// (save_state.cpp, bloco 2), entÃ£o sem isto um regime comandado
			// voltaria ao do arquivo ao recarregar.
			if (info != ws_.fx_regime.end()) {
				info->second.regime = regime;
			}
			FxState& state = ws_.fx[currency.code];
			if (cmd.v[0] > 0.0) {
				state.parity = cmd.v[0];
			}
			if (cmd.v[1] >= 0.0) {
				state.band_width = std::clamp(cmd.v[1], 0.0, 1.0);
			}
			out["ok"] = true;
			out["currency"] = String(currency.code.c_str());
			out["regime"] = ogs::central_bank::regime_name(regime);
			return out;
		}
		case ogs::CommandType::SetMonetaryStandard: {
			Dictionary out;
			const int standard = ogs::central_bank::standard_from_name(
				cmd.s[0], -1);
			if (standard < 0) {
				out["ok"] = false;
				out["error"] = "padrao desconhecido";
				return out;
			}
			ws_.monetary_standard = standard;
			// O preÃ§o fixo de conversÃ£o do ouro Ã© o de catÃ¡logo do jogo,
			// congelado no comando (nÃ£o Ã© o par histÃ³rico de 1944).
			if (standard != ogs::central_bank::kStandardFiat) {
				const int gold = sim_core_.product_index(std::string("gold"));
				const double price = gold >= 0 ? sim_core_.base_price(gold)
					: 0.0;
				for (Currency& currency : ws_.currencies) {
					FxState& state = ws_.fx[currency.code];
					if (state.conversion_price <= 0.0) {
						state.conversion_price = price;
					}
				}
			}
			out["ok"] = true;
			out["standard"] =
				ogs::central_bank::standard_name(standard);
			return out;
		}
		case ogs::CommandType::SetBankCredit: {
			Dictionary out;
			ws_.bank_credit_enabled = cmd.i[0] != 0;
			out["ok"] = true;
			out["enabled"] = ws_.bank_credit_enabled;
			return out;
		}
		case ogs::CommandType::GroupBorrow: {
			double granted = 0.0;
			std::string error;
			const bool ok = ogs::bank::lend(ws_,
				std::string(cmd.s[0].c_str()), cmd.v[0], &granted,
				&error);
			// D-131: empréstimo de grupo fora da fase da crise cria M1; registra
			// no acumulado auditável (o tick já fechou).
			if (granted > 0.0) {
				ws_.bank_money_delta_total += granted;
			}
			Dictionary out;
			out["ok"] = ok;
			out["granted"] = granted;
			out["group"] = String(cmd.s[0].c_str());
			if (!ok) {
				out["error"] = String(error.c_str());
			}
			return out;
		}
		case ogs::CommandType::CityBorrow: {
			double granted = 0.0;
			std::string error;
			const bool ok = ogs::crisis::borrow_city(ws_,
				std::string(cmd.s[0].c_str()), cmd.v[0], &granted,
				&error);
			Dictionary out;
			out["ok"] = ok;
			out["granted"] = granted;
			out["city"] = String(cmd.s[0].c_str());
			if (!ok) {
				out["error"] = String(error.c_str());
			}
			return out;
		}
		case ogs::CommandType::None:
			break;
	}
	return result;
}

} // namespace godot
