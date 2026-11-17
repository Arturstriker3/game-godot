#include "commands.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace ogs {
namespace {

struct TypeName {
	CommandType type;
	const char* name;
};

// Nomes curtos e estaveis: viram a chave "type" do log JSON e o replay os le
// de volta. Nunca renomeie um nome que ja saiu num log versionado.
constexpr TypeName kTypeNames[] = {
	{CommandType::Invest, "invest"},
	{CommandType::Build, "build"},
	{CommandType::SpendGovernment, "spend_government"},
	{CommandType::PriceShock, "price_shock"},
	{CommandType::SetSuitability, "set_suitability"},
	{CommandType::SetDeposit, "set_deposit"},
	{CommandType::SetExternalDemand, "set_external_demand"},
	{CommandType::SetExternalIncomeIndex, "set_external_income_index"},
	{CommandType::SetProductivityIndex, "set_productivity_index"},
	{CommandType::SetIncomeGini, "set_income_gini"},
	{CommandType::SetMinimumWage, "set_minimum_wage"},
	{CommandType::SetWageRigidity, "set_wage_rigidity"},
	{CommandType::SetTariff, "set_tariff"},
	{CommandType::SetCurrencyRate, "set_currency_rate"},
	{CommandType::ExpandPort, "expand_port"},
	{CommandType::SetPortCapacity, "set_port_capacity"},
	{CommandType::SetMarketStock, "set_market_stock"},
	{CommandType::SetMarketPrice, "set_market_price"},
	{CommandType::FleetBuy, "fleet_buy"},
	{CommandType::FundGroup, "fund_group"},
	{CommandType::GroupInvest, "group_invest"},
	{CommandType::SetFdiPolicy, "set_fdi_policy"},
	{CommandType::SetSectorIncentive, "set_sector_incentive"},
	{CommandType::CreateStateCompany, "create_state_company"},
	{CommandType::Privatize, "privatize"},
	{CommandType::Nationalize, "nationalize"},
	{CommandType::UrbanExpand, "urban_expand"},
	{CommandType::UrbanDensify, "urban_densify"},
	{CommandType::UrbanSocialHousing, "urban_social_housing"},
	{CommandType::SetServiceCapacity, "set_service_capacity"},
	{CommandType::SetServiceBusinessState, "set_service_business_state"},
};

} // namespace

const char* command_type_name(CommandType type) {
	for (const TypeName& entry : kTypeNames) {
		if (entry.type == type) {
			return entry.name;
		}
	}
	return nullptr;
}

bool command_type_from_name(const std::string& name, CommandType& out) {
	for (const TypeName& entry : kTypeNames) {
		if (name == entry.name) {
			out = entry.type;
			return true;
		}
	}
	return false;
}

uint64_t CommandQueue::push(Command cmd) {
	std::lock_guard<std::mutex> lock(mutex_);
	cmd.seq = ++next_seq_;
	pending_.push_back(std::move(cmd));
	return next_seq_;
}

std::vector<Command> CommandQueue::take_pending() {
	std::lock_guard<std::mutex> lock(mutex_);
	std::vector<Command> taken;
	taken.swap(pending_);
	// Ordem de emissao. O `seq` ja e monotonico, entao um sort estavel mantem
	// a ordem de push mesmo se um dia o push deixar de ser serializado.
	std::stable_sort(taken.begin(), taken.end(),
		[](const Command& a, const Command& b) { return a.seq < b.seq; });
	return taken;
}

uint64_t CommandQueue::last_seq() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return next_seq_;
}

bool CommandQueue::take_by_seq(uint64_t seq, Command& out) {
	std::lock_guard<std::mutex> lock(mutex_);
	for (std::vector<Command>::iterator it = pending_.begin();
			it != pending_.end(); ++it) {
		if (it->seq != seq) {
			continue;
		}
		out = std::move(*it);
		pending_.erase(it);
		return true;
	}
	return false;
}

std::size_t CommandQueue::pending_size() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return pending_.size();
}

void CommandQueue::clear_pending() {
	std::lock_guard<std::mutex> lock(mutex_);
	pending_.clear();
}

void CommandQueue::log(const Command& cmd) {
	std::lock_guard<std::mutex> lock(mutex_);
	if (log_.size() >= kMaxCommandLog) {
		truncated_ = true;
		return;
	}
	log_.push_back(cmd);
}

std::vector<Command> CommandQueue::log_entries() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return log_;
}

std::size_t CommandQueue::log_size() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return log_.size();
}

bool CommandQueue::truncated() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return truncated_;
}

void CommandQueue::clear_log() {
	std::lock_guard<std::mutex> lock(mutex_);
	log_.clear();
}

void CommandQueue::reset() {
	std::lock_guard<std::mutex> lock(mutex_);
	pending_.clear();
	log_.clear();
	truncated_ = false;
	next_seq_ = 0;
}

} // namespace ogs
