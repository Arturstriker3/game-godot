// Serialização save/load dentro da biblioteca de simulação (sem godot-cpp):
// porte mecânico de edge/save_load.cpp para ogs::save, com os aliases
// Array/Dictionary/Variant do shim de valor (save_value.h).
//
// As chaves, a ORDEM de inserção em `data[...]`, os valores e todas as
// expressões numéricas são idênticas às do original: o save_sha256 do jogo
// depende disso.
#include "save_state.h"
#include "save/save_internal.h"

#include "bank.h"
#include "bank_crisis.h"
#include "central_bank.h"
#include "crisis.h"
#include "exchange.h"
#include "potential_demand.h"
#include "sim_constants.h"
#include "sim_core.h"
#include "stock.h"
#include "storage.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace ogs::save {

namespace detail {

// Contêiner VAZIO precisa nascer com o tipo explícito: um Valor sem tipo sai
// como `null` no JSON, e o save espera `{}` / `[]` (senão o save_sha256 muda).
// O shim do núcleo expõe os dois construtores (`Value::make_object/array`).
Dictionary make_object() { return Value::make_object(); }

Array make_array() { return Value::make_array(); }

// C2c (D-108e) — MOEDAS EXTINTAS no cenário. A taxa irrevocável converte a
// paridade LOCAL (unidades por 1 âncora); o dinheiro do núcleo é em XGS e não
// se move. HRK não existe no cenário (a Croácia já nasce no EUR): a linha fica
// como regra declarada e nunca dispara.
struct CurrencySuccession {
	const char* from;
	const char* to;
	double units_per_target;
	const char* source;
};

constexpr CurrencySuccession kCurrencySuccessions[] = {
	{"BGN", "EUR", 1.95583,
		"Decisao (UE) 2025/1407: Bulgaria no euro em 2026-01-01, 1 EUR = 1,95583 BGN"},
	{"HRK", "EUR", 7.53450,
		"Croatia no euro em 2023-01-01, 1 EUR = 7,53450 HRK (moeda fora do cenario)"},
};

// ---------------------------------------------------------------------------
// B7 — serialização/leitura dos blocos informal/mortality/dissatisfaction. O
// mesmo formato serve para Pop (urbano) e Region (interior): os campos têm os
// mesmos nomes nos dois.
// ---------------------------------------------------------------------------
Dictionary save_informal_block(double share, const double* labor_share,
		const double* income, double wage_bill, double workers, double output,
		double demand, double revenue, double price, double service_share) {
	Dictionary d = make_object();
	d["share"] = share;
	Array labor = make_array();
	Array inc = make_array();
	for (int s = 0; s < kIncomeStrata; ++s) {
		labor.append(labor_share[s]);
		inc.append(income[s]);
	}
	d["labor_share"] = labor;
	d["income"] = inc;
	d["wage_bill"] = wage_bill;
	Dictionary svc = make_object();
	svc["workers"] = workers;
	svc["output"] = output;
	svc["demand"] = demand;
	svc["revenue"] = revenue;
	svc["price"] = price;
	svc["share"] = service_share;
	d["service"] = svc;
	return d;
}

Dictionary save_mortality_block(double deaths_expected, double deaths_total,
		double excess_total, double excess_famine_total, int famine_months,
		double food_price_ref, double food_inflation_month, double health_access_ref,
		double satisfaction_prev) {
	Dictionary d = make_object();
	d["deaths_expected"] = deaths_expected;
	d["deaths_total"] = deaths_total;
	d["excess_total"] = excess_total;
	d["excess_famine_total"] = excess_famine_total;
	d["famine_months"] = famine_months;
	d["food_price_ref"] = food_price_ref;
	d["food_inflation_month"] = food_inflation_month;
	d["health_access_ref"] = health_access_ref;
	d["satisfaction_prev"] = satisfaction_prev;
	return d;
}

Dictionary save_dissatisfaction_block(double index, const double* by_stratum,
		double unemployment, double rent, double food, double inflation,
		double informality) {
	Dictionary d = make_object();
	d["index"] = index;
	Array bs = make_array();
	for (int s = 0; s < kIncomeStrata; ++s) {
		bs.append(by_stratum[s]);
	}
	d["by_stratum"] = bs;
	d["unemployment"] = unemployment;
	d["rent"] = rent;
	d["food"] = food;
	d["inflation"] = inflation;
	d["informality"] = informality;
	return d;
}

void load_informal_block(const Dictionary& d, double& share,
		double* labor_share, double* income, double& wage_bill, double& workers,
		double& output, double& demand, double& revenue, double& price,
		double& service_share) {
	share = std::clamp(double(d.get("share", 0.0)), 0.0, 1.0);
	const Array labor = d.get("labor_share", Array());
	const Array inc = d.get("income", Array());
	for (int s = 0; s < kIncomeStrata; ++s) {
		labor_share[s] = s < labor.size()
			? std::clamp(double(labor[s]), 0.0, 1.0) : 0.0;
		income[s] = s < inc.size() ? std::max(0.0, double(inc[s])) : 0.0;
	}
	wage_bill = std::max(0.0, double(d.get("wage_bill", 0.0)));
	const Dictionary svc = d.get("service", Dictionary());
	workers = std::max(0.0, double(svc.get("workers", 0.0)));
	output = std::max(0.0, double(svc.get("output", 0.0)));
	demand = std::max(0.0, double(svc.get("demand", 0.0)));
	revenue = std::max(0.0, double(svc.get("revenue", 0.0)));
	price = std::max(0.0, double(svc.get("price", 0.0)));
	service_share = std::clamp(double(svc.get("share", 0.0)), 0.0, 1.0);
}

void load_mortality_block(const Dictionary& d, double& deaths_expected,
		double& deaths_total, double& excess_total, double& excess_famine_total,
		int& famine_months, double& food_price_ref, double& food_inflation_month,
		double& health_access_ref, double& satisfaction_prev) {
	deaths_expected = std::max(0.0, double(d.get("deaths_expected", 0.0)));
	deaths_total = std::max(0.0, double(d.get("deaths_total", 0.0)));
	excess_total = std::max(0.0, double(d.get("excess_total", 0.0)));
	excess_famine_total = std::max(0.0,
		double(d.get("excess_famine_total", 0.0)));
	famine_months = std::max(0, int(d.get("famine_months", 0)));
	food_price_ref = std::max(0.0, double(d.get("food_price_ref", 0.0)));
	food_inflation_month = double(d.get("food_inflation_month", 0.0));
	health_access_ref = std::clamp(
		double(d.get("health_access_ref", kHealthAccessDefault)), 0.0, 1.0);
	satisfaction_prev = std::clamp(
		double(d.get("satisfaction_prev", 1.0)), 0.0, 1.0);
}

void load_dissatisfaction_block(const Dictionary& d, double& index,
		double* by_stratum, double& unemployment, double& rent, double& food,
		double& inflation, double& informality) {
	index = std::clamp(double(d.get("index", 0.0)), 0.0, 1.0);
	const Array bs = d.get("by_stratum", Array());
	for (int s = 0; s < kIncomeStrata; ++s) {
		by_stratum[s] = s < bs.size() ? std::clamp(double(bs[s]), 0.0, 1.0) : 0.0;
	}
	unemployment = std::clamp(double(d.get("unemployment", 0.0)), 0.0, 1.0);
	rent = std::clamp(double(d.get("rent", 0.0)), 0.0, 1.0);
	food = std::clamp(double(d.get("food", 0.0)), 0.0, 1.0);
	inflation = std::clamp(double(d.get("inflation", 0.0)), 0.0, 1.0);
	informality = std::clamp(double(d.get("informality", 0.0)), 0.0, 1.0);
}

// Constrói um String Godot a partir de UTF-8. O construtor `String(const char*)`
// desta godot-cpp interpreta os bytes como Latin-1, então `String(x.c_str())`
// corrompia os nomes não-ASCII a cada round-trip save/load (Córdoba -> CÃ³rdoba
// -> CÃÂ³rdoba...). `String::utf8` faz a leitura correta dos bytes UTF-8.
String sim_str(const char* utf8) {
	return String(utf8);
}

double safe_nonneg(double v) {
	return std::isfinite(v) ? std::max(0.0, v) : 0.0;
}

// Fração 0..1 (taxas da prefeitura e condição do estoque, save v13).
double safe_unit(double v) {
	return std::isfinite(v) ? std::clamp(v, 0.0, 1.0) : 0.0;
}

// População de SIMULAÇÃO da cidade. Desde a calibração de 2026, `population`
// no cenário é a população real exibida (nação inteira rateada) e a aglomeração
// da fonte viaja em `sim_population`. Sem o campo (save antigo, cenário
// sintético), a população histórica É a unidade de simulação — comportamento
// anterior preservado.
double read_sim_population(const Dictionary& data, double population) {
	if (!data.has("sim_population")) {
		return population;
	}
	const double value = safe_nonneg(static_cast<double>(data["sim_population"]));
	return value > 0.0 ? value : population;
}

// ---------------------------------------------------------------------------
// Migração de índice de produto (save v1/v2 -> v3).
//
// Saves antigos tinham farinha=1 e pão=2 (entre trigo e ferro). Ao removê-los,
// o enum deslocou -2 a partir do ferro (iron 3->1, rice 4->2, ...). A migração
// reverte esse deslocamento: trigo (0) fica, farinha/pão (1/2) são descartados
// e os demais (>=3) deslocam -2.
// ---------------------------------------------------------------------------

// Índice antigo -> novo. Retorna -1 para farinha/pão (produto removido).
static int migrate_product_index(int old_index) {
	if (old_index < 0) {
		return -1;  // sem produto/insumo
	}
	if (old_index == 0) {
		return 0;  // trigo inalterado
	}
	if (old_index == 1 || old_index == 2) {
		return -1;  // farinha/pão removidos
	}
	return old_index - 2;
}

// Remapeia um array por-produto do layout antigo (com farinha/pão) para o
// atual (products posições). new_index 0 <- old_index 0; new_index >= 1 <-
// old_index (new_index + 2). Posições sem correspondência ficam 0.0.
static Array migrate_product_array(const Array& old, int products) {
	Array out;
	out.resize(products);
	for (int new_index = 0; new_index < products; ++new_index) {
		const int old_index = (new_index == 0) ? 0 : (new_index + 2);
		if (old_index >= 0 && old_index < old.size()) {
			out[new_index] = old[old_index];
		} else {
			out[new_index] = 0.0;
		}
	}
	return out;
}

// Remapeia um array por-produto usando o vetor old_index -> new_index (-1
// descarta o produto). Posições sem correspondência ficam 0.0.
static Array remap_product_array(const Array& old,
		const std::vector<int>& remap, int products) {
	Array out;
	out.resize(products);
	for (int index = 0; index < products; ++index) {
		out[index] = 0.0;
	}
	const int limit = static_cast<int>(
		std::min<std::size_t>(static_cast<std::size_t>(old.size()), remap.size()));
	for (int old_index = 0; old_index < limit; ++old_index) {
		const int new_index = remap[static_cast<std::size_t>(old_index)];
		if (new_index >= 0 && new_index < products) {
			out[new_index] = old[old_index];
		}
	}
	return out;
}

// F0 (save v39) — o bem generico `fuel` virou gasoline/diesel/jet_fuel. Um
// save anterior guarda o estoque no indice antigo de `fuel`; a migracao rateia
// esse estoque pelas fracoes declaradas da refinaria (EIA 2024: 45/29/9 de
// 83 t uteis). Sao constantes de MIGRACAO, nao de simulacao: nada no dia as
// consulta. As fracoes vivem em save_internal.h (mesmos valores) para o TU de
// cidade usar a mesma definicao.

// F0: ids de receita do combustivel generico (saves v38 e anteriores) mapeados
// para a receita que ocupa o lugar. O id novo vem de recipes.json; a receita
// antiga continua valida para um snapshot antigo (que traz a propria lista).
const char* legacy_recipe_alias(const std::string& id) {
	if (id == "fuel_a") {
		return "gasoline_a";
	}
	if (id == "fuel_b") {
		return "gasoline_b";
	}
	if (id == "fuel_c") {
		return "diesel_a";
	}
	if (id == "fuel_d") {
		return "diesel_b";
	}
	return nullptr;
}

// F0 (t145): num snapshot v38 e anterior a lista de bens vem do PROPRIO arquivo
// e o combustivel de referencia e o bem generico `fuel`. Sem o id moderno, o
// papel volta ao antigo — senao `fleet_product_diesel`/`jet` e o insumo da
// usina a oleo ficariam -1 e a frota/planta parariam em silencio.
static int product_or_legacy(const SimCore& core, const char* modern,
		const char* legacy) {
	const int index = core.product_index(std::string(modern));
	if (index >= 0) {
		return index;
	}
	return legacy != nullptr
		? core.product_index(std::string(legacy)) : -1;
}

} // namespace detail

using namespace detail;

Value to_value(const WorldState& ws, const SimCore& core,
		const PotentialDemandCache* potential_cache) {
	Dictionary data = make_object();
	data["version"] = kSaveVersion;
	data["active"] = ws.active;
	data["world_mode"] = ws.world_mode;
	data["day"] = ws.day;
	// A6: cadência por sistema. Muda QUANDO cada cidade fecha o mês/semana, então
	// é estado verdadeiro do mundo (não configuração de exibição).
	{
		Dictionary cadence = make_object();
		cadence["weekly_period"] = ws.cadence.weekly_period;
		cadence["monthly_period"] = ws.cadence.monthly_period;
		data["cadence"] = cadence;
	}
	data["transport_cash"] = ws.transport_cash;
	data["foreign_cash"] = ws.foreign_cash;
	data["construction_cash"] = ws.construction_cash;
	// D-126: contador diagnóstico do dinheiro bancário criado/destruído (M1).
	// Assinado; entra no save para que um load no meio da corrida não zere a
	// contabilidade dos guardas de conservação.
	data["bank_money_delta_total"] = ws.bank_money_delta_total;
	// Contadores de encerramento de construções (falidas/esgotadas) e a marca de
	// bootstrap industrial. São ESTADO VERDADEIRO (não cache): sem eles, o load
	// re-semeava a indústria (24 plantas extras) e perdia o histórico de
	// falências/jazidas esgotadas, divergindo do caminho direto já no 1º tick.
	data["bankruptcies"] = ws.bankruptcies;
	data["depleted_mines"] = ws.depleted_mines;
	data["industry_seeded"] = ws.industry_seeded;
	// D-126 (v41): parâmetros de crise aplicados + contadores da reação. O
	// bloco preserva o balanceamento que gerou o mundo; v40 chega sem ele.
	{
		Dictionary block = make_object();
		Dictionary params = make_object();
		for (const auto& row : crisis::params_map(ws)) {
			params[sim_str(row.first.c_str())] = row.second;
		}
		block["params"] = params;
		block["layoffs"] = ws.crisis_layoffs;
		block["jobs_cut"] = ws.crisis_jobs_cut;
		block["severance_paid"] = ws.crisis_severance_paid;
		block["loans"] = ws.crisis_loans;
		block["renegotiations"] = ws.crisis_renegotiations;
		block["penalty_paid"] = ws.crisis_penalty_paid;
		block["liquidations"] = ws.crisis_liquidations;
		block["liquidated_t"] = ws.crisis_liquidated_t;
		block["liquidated_value"] = ws.crisis_liquidated_value;
		block["mothballs"] = ws.crisis_mothballs;
		block["reopens"] = ws.crisis_reopens;
		block["reopen_paid"] = ws.crisis_reopen_paid;
		block["auctions"] = ws.crisis_auctions;
		block["auction_value"] = ws.crisis_auction_value;
		block["auctions_unsold"] = ws.crisis_auctions_unsold;
		block["farm_area_reductions"] = ws.crisis_farm_area_reductions;
		block["power_auctions"] = ws.crisis_power_auctions;
		block["power_auction_value"] = ws.crisis_power_auction_value;
		data["crisis"] = block;
	}
	// G (v24) — estado da safra por (cidade, produto, safra): ano do plantio,
	// fator de rendimento por fertilizante e o que já foi entregue na colheita
	// do ano. Só entram as entradas com estado (o plano em si é DEFINIÇÃO e vai
	// no bloco `definitions`).
	{
		Dictionary agriculture = make_object();
		agriculture["month_key"] = ws.agri_month_key;
		Array plans = make_array();
		for (const CropPlan& plan : ws.crop_plans) {
			Array seasons = make_array();
			bool touched = false;
			for (int index = 0; index < plan.season_count; ++index) {
				const CropSeasonState& state = plan.state[index];
				Dictionary item = make_object();
				item["planted_year"] = state.planted_year;
				item["yield_scale"] = state.yield_scale;
				item["harvest_yield_scale"] = state.harvest_yield_scale;
				item["fertilizer_t"] = state.fertilizer_t;
				item["harvest_year"] = state.harvest_year;
				item["delivered_t"] = state.delivered_t;
				item["loss_month_t"] = state.loss_month_t;
				item["loss_month_key"] = state.loss_month_key;
				// D-126 (v41): área da estação (plantada e congelada).
				item["planted_area_scale"] = state.planted_area_scale;
				item["harvest_area_scale"] = state.harvest_area_scale;
				if (state.planted_year != 0 || state.harvest_year != 0
						|| state.delivered_t != 0.0
						|| state.yield_scale != 1.0
						|| state.fertilizer_t != 0.0
						|| state.planted_area_scale != 1.0) {
					touched = true;
				}
				seasons.append(item);
			}
			if (!touched && plan.area_scale == 1.0) {
				continue;
			}
			Dictionary entry = make_object();
			entry["city"] = Value(plan.city);
			entry["product"] = Value(ws.product_names[
				static_cast<std::size_t>(plan.product)]);
			entry["area_scale"] = plan.area_scale;
			entry["seasons"] = seasons;
			plans.append(entry);
		}
		agriculture["plans"] = plans;
		data["agriculture"] = agriculture;
	}

	// G2 (v25) — ARMAZENAGEM: armazéns agregados por (cidade, tipo, dono) com
	// capacidade e estoque guardado. O mapeamento produto→tipo é DEFINIÇÃO e
	// não vai ao save (o StorageService reaplica do JSON/tabela no load).
	{
		Dictionary storage = make_object();
		Array sites = make_array();
		for (const StorageSite& site : ws.storage_sites) {
			Dictionary entry = make_object();
			entry["city"] = Value(site.city);
			entry["type"] = Value(storage_type_name(site.type)
				? storage_type_name(site.type) : "");
			entry["owner"] = Value(storage_scope_name(site.scope));
			entry["capacity_t"] = site.capacity_t;
			Array stock = make_array();
			for (std::size_t product = 0; product < site.stock.size();
					++product) {
				const double quantity = site.stock[product];
				const double inflow = product < site.inflow_stock.size()
					? site.inflow_stock[product] : 0.0;
				if (quantity <= 1e-9 && inflow <= 1e-9) {
					continue;
				}
				if (product >= static_cast<std::size_t>(ws.products)
						|| product >= ws.product_names.size()) {
					continue;
				}
				Dictionary item = make_object();
				item["product"] = Value(ws.product_names[product]);
				item["tons"] = std::max(0.0, quantity);
				item["inflow_t"] = std::max(0.0,
					std::min(inflow, std::max(0.0, quantity)));
				stock.append(item);
			}
			entry["stock"] = stock;
			sites.append(entry);
		}
		storage["sites"] = sites;
		storage["month_key"] = ws.storage_month_key;
		data["storage"] = storage;
	}

	// Lista canônica de produtos (id na posição do índice). O contrato é por
	// `order`; os 21 primeiros são os mesmos do save v3.
	Array product_names = make_array();
	for (int index = 0; index < ws.products; ++index) {
		product_names.append(String(core.product_name(index)));
	}
	data["products"] = product_names;

	Array produced = make_array();
	Array consumed = make_array();
	for (int index = 0; index < ws.products; ++index) {
		produced.append(ws.produced[index]);
		consumed.append(ws.consumed_total[index]);
	}
	data["produced_total"] = produced;
	data["consumed_total"] = consumed;

	city::write(data, ws, core);

	Array transit = make_array();
	for (const Shipment& shipment : ws.in_transit) {
		Dictionary entry = make_object();
		entry["to_city"] = sim_str(shipment.to_city.c_str());
		entry["product"] = shipment.product;
		entry["amount"] = shipment.amount;
		entry["arrival_day"] = shipment.arrival_day;
		// F4: reserva de frota do pedido (liberada na chegada).
		entry["fleet_nation"] = sim_str(shipment.fleet_nation.c_str());
		entry["fleet_trucks"] = shipment.fleet_trucks;
		entry["fleet_ships"] = shipment.fleet_ships;
		entry["fleet_planes"] = shipment.fleet_planes;
		entry["diesel"] = shipment.diesel;
		entry["jet_fuel"] = shipment.jet_fuel;
		transit.append(entry);
	}
	data["in_transit"] = transit;

	// F4: frota nacional. Uma entrada por nação, com a saúde por modal.
	Array fleets = make_array();
	for (const std::string& nation : ws.fleet_order) {
		const auto found = ws.fleets.find(nation);
		if (found == ws.fleets.end()) {
			continue;
		}
		const Fleet& fleet = found->second;
		Dictionary entry = make_object();
		entry["nation"] = sim_str(nation.c_str());
		entry["trucks"] = fleet.trucks;
		entry["ships"] = fleet.ships;
		entry["planes"] = fleet.planes;
		entry["target_trucks"] = fleet.target_trucks;
		entry["target_ships"] = fleet.target_ships;
		entry["target_planes"] = fleet.target_planes;
		entry["diesel"] = fleet.diesel;
		entry["diesel_target"] = fleet.diesel_target;
		entry["jet_fuel"] = fleet.jet_fuel;
		entry["jet_target"] = fleet.jet_target;
		entry["spare_parts"] = fleet.spare_parts;
		entry["health"] = Array{fleet.health[0], fleet.health[1], fleet.health[2]};
		entry["fuel_used_total"] = fleet.fuel_used_total;
		entry["fuel_bought_total"] = fleet.fuel_bought_total;
		entry["diesel_used_total"] = fleet.diesel_used_total;
		entry["jet_used_total"] = fleet.jet_used_total;
		entry["diesel_bought_total"] = fleet.diesel_bought_total;
		entry["jet_bought_total"] = fleet.jet_bought_total;
		entry["parts_used_total"] = fleet.parts_used_total;
		entry["vehicles_bought_total"] = fleet.vehicles_bought_total;
		entry["derived"] = fleet.derived;
		fleets.append(entry);
	}
	data["fleets"] = fleets;

	finance::write(data, ws);
	power::write(data, ws);
	// F9 (save v11): grupos, fluxos, política de IDE, incentivos e confiança.
	Array groups = make_array();
	for (const Group& group : ws.groups) {
		Dictionary entry = make_object();
		entry["id"] = sim_str(group.id.c_str());
		entry["name"] = sim_str(group.name.c_str());
		entry["origin_nation"] = sim_str(group.origin_nation.c_str());
		entry["sector"] = sim_str(group.sector.c_str());
		entry["cash"] = group.cash;
		entry["distributable"] = group.distributable;
		entry["revenue_total"] = group.revenue_total;
		entry["profit_total"] = group.profit_total;
		entry["dividends_total"] = group.dividends_total;
		entry["taxes_total"] = group.taxes_total;
		entry["capex_total"] = group.capex_total;
		entry["state_owned"] = group.state_owned;
		// C3 (D-107): o balanço do grupo ganha dívida de crédito e juro vencido.
		entry["debt"] = group.debt;
		entry["credit_arrears"] = group.credit_arrears;
		entry["credit_total"] = group.credit_total;
		entry["credit_repaid"] = group.credit_repaid;
		entry["credit_default_days"] = group.credit_default_days;
		entry["credit_defaults"] = group.credit_defaults;
		// D-126 (v41): prêmio de juro do capital de giro de crise.
		entry["credit_rate_add"] = group.credit_rate_add;
		entry["credit_renegotiations"] = group.credit_renegotiations;
		entry["credit_arrears_days"] = group.credit_arrears_days;
		Array by_host = make_array();
		for (const auto& row : group.profit_by_host) {
			Dictionary host = make_object();
			host["nation"] = sim_str(row.first.c_str());
			host["profit"] = row.second;
			by_host.append(host);
		}
		entry["profit_by_host"] = by_host;
		groups.append(entry);
	}
	data["groups"] = groups;
	// Ordem determinística por nação (o mapa é unordered; iterar direto mudaria
	// a ordem do array a cada load e quebraria o round-trip byte a byte).
	Array group_flows = make_array();
	std::vector<std::string> group_flow_keys;
	group_flow_keys.reserve(ws.group_flows.size());
	for (const auto& row : ws.group_flows) {
		group_flow_keys.push_back(row.first);
	}
	std::sort(group_flow_keys.begin(), group_flow_keys.end());
	for (const std::string& nation : group_flow_keys) {
		const GroupFlow& flow = ws.group_flows.at(nation);
		Dictionary entry = make_object();
		entry["nation"] = sim_str(nation.c_str());
		entry["repatriated_out"] = flow.repatriated_out;
		entry["repatriated_in"] = flow.repatriated_in;
		entry["dividends"] = flow.dividends;
		entry["taxes"] = flow.taxes;
		entry["subsidy"] = flow.subsidy;
		group_flows.append(entry);
	}
	data["group_flows"] = group_flows;
	Array fdi_policies = make_array();
	for (const FdiPolicy& policy : ws.fdi_policies) {
		Dictionary entry = make_object();
		entry["nation"] = sim_str(policy.nation.c_str());
		entry["sector"] = sim_str(policy.sector.c_str());
		entry["mode"] = policy.mode;
		entry["min_local"] = policy.min_local;
		fdi_policies.append(entry);
	}
	data["fdi_policies"] = fdi_policies;
	Array incentives = make_array();
	for (const SectorIncentive& incentive : ws.incentives) {
		Dictionary entry = make_object();
		entry["nation"] = sim_str(incentive.nation.c_str());
		entry["sector"] = sim_str(incentive.sector.c_str());
		entry["tax_holiday"] = incentive.tax_holiday;
		entry["subsidy"] = incentive.subsidy;
		incentives.append(entry);
	}
	data["sector_incentives"] = incentives;
	// Ordem determinística por nação (unordered_map -> array estável).
	Array confidence = make_array();
	std::vector<std::string> confidence_keys;
	confidence_keys.reserve(ws.confidence.size());
	for (const auto& row : ws.confidence) {
		confidence_keys.push_back(row.first);
	}
	std::sort(confidence_keys.begin(), confidence_keys.end());
	for (const std::string& nation : confidence_keys) {
		Dictionary entry = make_object();
		entry["nation"] = sim_str(nation.c_str());
		entry["value"] = ws.confidence.at(nation);
		const auto penalty = ws.confidence_penalty_day.find(nation);
		entry["penalty_day"] = penalty == ws.confidence_penalty_day.end()
			? 0 : penalty->second;
		confidence.append(entry);
	}
	data["confidence"] = confidence;
	// F7: política salarial (save v9). `minimum_wages` por nação (ausente/0 =
	// piso desligado) e a rigidez diária configurável. Ordem por nação.
	Array minimum_wages = make_array();
	std::vector<std::string> wage_keys;
	wage_keys.reserve(ws.minimum_wages.size());
	for (const auto& row : ws.minimum_wages) {
		if (row.second > 0.0) {
			wage_keys.push_back(row.first);
		}
	}
	std::sort(wage_keys.begin(), wage_keys.end());
	for (const std::string& nation_tag : wage_keys) {
		Dictionary entry = make_object();
		entry["nation"] = sim_str(nation_tag.c_str());
		entry["value"] = ws.minimum_wages.at(nation_tag);
		minimum_wages.append(entry);
	}
	data["minimum_wages"] = minimum_wages;
	data["wage_rigidity"] = ws.wage_rigidity;

	// Portos: identidade + capacidade/fila/obra. Campos aditivos (v3 continua
	// válido; save anterior não tem a chave e o loader mantém os portos do
	// mundo, recalculando a capacidade).
	Array ports = make_array();
	for (const Port& port : ws.ports) {
		Dictionary entry = make_object();
		entry["id"] = sim_str(port.id.c_str());
		entry["name"] = sim_str(port.name.c_str());
		entry["nation"] = sim_str(port.nation.c_str());
		entry["landmass"] = sim_str(port.landmass.c_str());
		entry["host_city"] = sim_str(port.host_city.c_str());
		entry["x"] = port.center.x;
		entry["y"] = port.center.y;
		entry["base_capacity_t_per_day"] = port.base_capacity_t_per_day;
		entry["capacity_t_per_day"] = port.capacity_t_per_day;
		entry["pending_capacity_t_per_day"] = port.pending_capacity_t_per_day;
		entry["pending_days"] = port.pending_days;
		ports.append(entry);
	}
	data["ports"] = ports;
	// G2 (save aditivo, sem bump): cache mensal do piso de demanda potencial.
	// Esparso (só células não nulas) para não inflar o arquivo; ausente quando
	// o piso não rodou (brain/piso desligado) e em saves antigos.
	if (potential_cache != nullptr && potential_cache->needs_loaded
			&& potential_cache->day >= 0 && potential_cache->cities > 0
			&& potential_cache->products > 0
			&& potential_cache->values.size()
				== static_cast<std::size_t>(potential_cache->cities)
					* static_cast<std::size_t>(
						potential_cache->products)) {
		Dictionary floor = make_object();
		floor["day"] = potential_cache->day;
		floor["cities"] = potential_cache->cities;
		floor["products"] = potential_cache->products;
		floor["recipes"] = static_cast<long long>(potential_cache->recipes);
		floor["needs_levels"] =
			static_cast<long long>(potential_cache->needs_levels);
		floor["government_products"] =
			static_cast<long long>(potential_cache->government_products);
		floor["durable_decay"] = potential_cache->durable_decay;
		floor["discount"] = potential_cache->discount;
		floor["cap"] = potential_cache->cap;
		Array entries = make_array();
		const int products = potential_cache->products;
		for (std::size_t cell = 0; cell < potential_cache->values.size();
				++cell) {
			const double value = potential_cache->values[cell];
			if (value == 0.0) {
				continue;  // esparso: zero é ausência de piso
			}
			Array cell_value = make_array();
			cell_value.append(static_cast<long long>(
				static_cast<int>(cell) / products));
			cell_value.append(static_cast<long long>(
				static_cast<int>(cell) % products));
			cell_value.append(value);
			entries.append(cell_value);
		}
		floor["values"] = entries;
		data["potential_cache"] = floor;
	}
	return data;
}

// Restaura o estado do cenário a partir de to_dict() (load + migrações).
void from_value(WorldState& ws, SimCore& core, const Dictionary& data,
		PotentialDemandCache* potential_cache) {
	// Recusa save de versão futura (maior que a que este binário conhece).
	const int version = int(data.get("version", 0));
	if (version > kSaveVersion) {
		ws.active = false;
		return;
	}
	// G2: restaura o cache mensal do piso (chave aditiva; save antigo ou
	// brain desligado não a tem e o cache fica vazio = recalculado no dia).
	if (potential_cache != nullptr) {
		*potential_cache = PotentialDemandCache{};
		const Dictionary floor = data.get("potential_cache", Dictionary());
		if (floor.size() > 0) {
			const int day = int(floor.get("day", -1));
			const int cities = int(floor.get("cities", 0));
			const int products = int(floor.get("products", 0));
			const Array entries = floor.get("values", Array());
			const std::size_t cells =
				static_cast<std::size_t>(std::max(cities, 0))
				* static_cast<std::size_t>(std::max(products, 0));
			if (day >= 0 && cities > 0 && products > 0) {
				potential_cache->values.assign(cells, 0.0);
				for (int index = 0; index < entries.size(); ++index) {
					const Array cell_value = entries[index];
					if (cell_value.size() != 3) {
						continue;
					}
					const int city = int(cell_value[0]);
					const int product = int(cell_value[1]);
					if (city < 0 || city >= cities || product < 0
							|| product >= products) {
						continue;
					}
					potential_cache->values[
						static_cast<std::size_t>(city * products + product)]
						= double(cell_value[2]);
				}
				potential_cache->cities = cities;
				potential_cache->products = products;
				potential_cache->day = day;
				potential_cache->recipes = static_cast<std::size_t>(
					int(floor.get("recipes", 0)));
				potential_cache->needs_levels = static_cast<std::size_t>(
					int(floor.get("needs_levels", 0)));
				potential_cache->government_products =
					static_cast<std::size_t>(
						int(floor.get("government_products", 0)));
				potential_cache->durable_decay =
					double(floor.get("durable_decay", 0.0));
				potential_cache->discount = double(floor.get("discount", -1.0));
				potential_cache->cap = double(floor.get("cap", -1.0));
				potential_cache->needs_loaded = true;
			}
		}
	}
	// Saves v1/v2 (antes da remoção de farinha/pão) trazem índices de produto
	// no layout antigo (farinha=1, pão=2, ferro=3...): remapeia para o layout
	// atual (ferro=1, arroz=2...), descartando farinha/pão.
	//
	// Saves v7 trazem a lista `products` com 114 ids (inclui `body_armor`).
	// Quando ela existe, a migração é POR ID (o nome do produto vence o
	// índice), então remoções futuras não deslocam estoques, mercados,
	// construções ou cargas em trânsito.
	const Array saved_products = data.get("products", Array());
	const bool has_saved_products = saved_products.size() > 0;
	const bool needs_legacy_migration = !has_saved_products && version < 3;
	// F0 (save v39): o bem generico `fuel` foi separado em gasoline/diesel/
	// jet_fuel. No caminho normal o mundo ja veio com a lista NOVA de bens
	// (set_goods antes do load), entao o indice antigo de `fuel` nao resolve:
	// o estoque e rateado nas fracoes da refinaria e a frota pelo consumo de
	// cada modal. Num snapshot antigo a lista de bens vem do proprio arquivo e
	// `fuel` continua existindo — ai nada e migrado (o mundo antigo segue
	// inteiro, com o seu proprio motor).
	const int legacy_gasoline = core.product_index("gasoline");
	const int legacy_diesel = core.product_index("diesel");
	const int legacy_jet = core.product_index("jet_fuel");
	const bool legacy_fuel_migration = version < 39
		&& core.product_index("fuel") < 0
		&& legacy_gasoline >= 0 && legacy_diesel >= 0 && legacy_jet >= 0;
	int legacy_fuel_index = -1;
	if (legacy_fuel_migration && has_saved_products) {
		for (int index = 0; index < saved_products.size(); ++index) {
			if (String(saved_products[index]) == "fuel") {
				legacy_fuel_index = index;
				break;
			}
		}
	}
	// Produto do save antigo que era `fuel`: vira gasoline (produto principal
	// da refinaria). Usado por construcoes, obras e cargas; o estoque de
	// mercado/armazem usa a divisao pelas fracoes, nao este substituto.
	const auto legacy_substitute = [&](int old_index) -> int {
		if (!legacy_fuel_migration || !has_saved_products
				|| old_index < 0 || old_index >= saved_products.size()) {
			return -1;
		}
		return String(saved_products[old_index]) == "fuel"
			? legacy_gasoline : -1;
	};
	std::vector<int> product_remap;
	if (has_saved_products) {
		product_remap.resize(
			static_cast<std::size_t>(saved_products.size()), -1);
		for (int index = 0; index < saved_products.size(); ++index) {
			product_remap[static_cast<std::size_t>(index)] =
				core.product_index(std::string(String(saved_products[index]).utf8().get_data()));
		}
	}
	const auto migrate_array = [&](const Array& old) -> Array {
		if (has_saved_products) {
			return remap_product_array(old, product_remap, ws.products);
		}
		if (needs_legacy_migration) {
			return migrate_product_array(old, ws.products);
		}
		return old;
	};
	const auto migrate_index = [&](int old_index) -> int {
		if (has_saved_products) {
			if (old_index < 0
					|| old_index >= static_cast<int>(product_remap.size())) {
				return -1;
			}
			return product_remap[static_cast<std::size_t>(old_index)];
		}
		if (needs_legacy_migration) {
			return migrate_product_index(old_index);
		}
		return old_index;
	};
	ws.active = bool(data.get("active", true));
	ws.world_mode = bool(data.get("world_mode", false));
	// A6: cadência por sistema. Save v14 e anteriores não têm o bloco: ficam com
	// os padrões do `WorldState` (semana 7, mês 30) — sem migração de cursor.
	ws.cadence = CadenceState{};
	if (data.has("cadence")) {
		const Dictionary cadence = data.get("cadence", Dictionary());
		ws.cadence.weekly_period =
			std::max(0, int(cadence.get("weekly_period", 7)));
		ws.cadence.monthly_period =
			std::max(0, int(cadence.get("monthly_period", 30)));
	}
	ws.day = int(data.get("day", 0));
	ws.transport_cash = double(data.get("transport_cash", 0.0));
	ws.foreign_cash = double(data.get("foreign_cash", 0.0));
	ws.construction_cash = double(data.get("construction_cash", 0.0));
	// D-126: contador do M1 criado pelo crédito. Ausente em save v40 e em
	// fixtures antigas: 0 (nada inventado para trás).
	ws.bank_money_delta_total =
		double(data.get("bank_money_delta_total", 0.0));
	ws.bankruptcies = int(data.get("bankruptcies", 0));
	// D-099: a composição das falências é diagnóstico de RUNTIME, não vai ao
	// save/fluxo de dicionário — carregar um mundo zera os contadores.
	for (int product = 0; product < kMaxProducts; ++product) {
		ws.bankrupt_by_output[product] = 0;
	}
	ws.bankrupt_planned_primary = 0;
	ws.depleted_mines = int(data.get("depleted_mines", 0));
	ws.industry_seeded = bool(data.get("industry_seeded", false));
	// D-126 (v41): parâmetros e contadores da sobrevivência em crise. Save v40
	// e anteriores não têm o bloco: o WorldState mantém os defaults declarados
	// e nenhuma reação em curso é inventada.
	if (version >= 41 && data.has("crisis")) {
		const Dictionary block = data.get("crisis", Dictionary());
		const Value& params_variant = block.get("params", Value());
		if (params_variant.get_type() == Variant::DICTIONARY) {
			const Dictionary params = params_variant;
			std::map<std::string, double> values;
			for (const auto& row : params.entries()) {
				values[row.first] = double(row.second);
			}
			crisis::apply_params(ws, values);
		}
		ws.crisis_layoffs = std::max(0, int(block.get("layoffs", 0)));
		ws.crisis_jobs_cut = safe_nonneg(double(block.get("jobs_cut", 0.0)));
		ws.crisis_severance_paid = safe_nonneg(
			double(block.get("severance_paid", 0.0)));
		ws.crisis_loans = safe_nonneg(double(block.get("loans", 0.0)));
		ws.crisis_renegotiations = std::max(0,
			int(block.get("renegotiations", 0)));
		ws.crisis_penalty_paid = safe_nonneg(
			double(block.get("penalty_paid", 0.0)));
		ws.crisis_liquidations = std::max(0,
			int(block.get("liquidations", 0)));
		ws.crisis_liquidated_t = safe_nonneg(
			double(block.get("liquidated_t", 0.0)));
		ws.crisis_liquidated_value = safe_nonneg(
			double(block.get("liquidated_value", 0.0)));
		ws.crisis_mothballs = std::max(0,
			int(block.get("mothballs", 0)));
		ws.crisis_reopens = std::max(0, int(block.get("reopens", 0)));
		ws.crisis_reopen_paid = safe_nonneg(
			double(block.get("reopen_paid", 0.0)));
		ws.crisis_auctions = std::max(0, int(block.get("auctions", 0)));
		ws.crisis_auction_value = safe_nonneg(
			double(block.get("auction_value", 0.0)));
		ws.crisis_auctions_unsold = std::max(0,
			int(block.get("auctions_unsold", 0)));
		ws.crisis_farm_area_reductions = std::max(0,
			int(block.get("farm_area_reductions", 0)));
		ws.crisis_power_auctions = std::max(0,
			int(block.get("power_auctions", 0)));
		ws.crisis_power_auction_value = safe_nonneg(
			double(block.get("power_auction_value", 0.0)));
	}

	finance::read_money(ws, core, data);
	finance::read_tariffs(ws, core, data);
	// F9 (save v11): grupos e política. Save v10 e anteriores não têm as
	// chaves: os grupos são semeados a partir dos ativos privados no fim do
	// load (regra determinística de seed_groups).
	ws.groups.clear();
	ws.group_lookup.clear();
	const Array saved_groups = data.get("groups", Array());
	for (int index = 0; index < saved_groups.size(); ++index) {
		const Dictionary entry = saved_groups[index];
		Group group;
		group.id = std::string(String(entry.get("id", "")).utf8().get_data());
		if (group.id.empty()) {
			continue;
		}
		group.name = std::string(String(entry.get("name", "")).utf8().get_data());
		group.origin_nation = std::string(
			String(entry.get("origin_nation", "")).utf8().get_data());
		group.sector = std::string(String(entry.get("sector", "")).utf8().get_data());
		group.cash = std::max(0.0, double(entry.get("cash", 0.0)));
		group.distributable = std::max(0.0,
			double(entry.get("distributable", 0.0)));
		group.revenue_total = std::max(0.0,
			double(entry.get("revenue_total", 0.0)));
		group.profit_total = double(entry.get("profit_total", 0.0));
		group.dividends_total = std::max(0.0,
			double(entry.get("dividends_total", 0.0)));
		group.taxes_total = std::max(0.0, double(entry.get("taxes_total", 0.0)));
		group.capex_total = std::max(0.0, double(entry.get("capex_total", 0.0)));
		group.state_owned = bool(entry.get("state_owned", false));
		// C3 (D-107): migração v29 → v30 carrega com dívida 0 (nada inventado
		// para trás); o crédito ligado é estado salvo.
		group.debt = std::max(0.0, double(entry.get("debt", 0.0)));
		group.credit_arrears = std::max(0.0,
			double(entry.get("credit_arrears", 0.0)));
		group.credit_total = std::max(0.0,
			double(entry.get("credit_total", 0.0)));
		group.credit_repaid = std::max(0.0,
			double(entry.get("credit_repaid", 0.0)));
		group.credit_default_days = std::max(0,
			int(entry.get("credit_default_days", 0)));
		group.credit_defaults = std::max(0,
			int(entry.get("credit_defaults", 0)));
		// D-126 (v41): v40 e anteriores chegam sem prêmio/renegociação.
		group.credit_rate_add = version >= 41
			? std::max(0.0, double(entry.get("credit_rate_add", 0.0))) : 0.0;
		group.credit_renegotiations = version >= 41
			? std::max(0, int(entry.get("credit_renegotiations", 0))) : 0;
		group.credit_arrears_days = version >= 41
			? std::max(0, int(entry.get("credit_arrears_days", 0))) : 0;
		const Array by_host = entry.get("profit_by_host", Array());
		for (int host_index = 0; host_index < by_host.size(); ++host_index) {
			const Dictionary host = by_host[host_index];
			const std::string nation = std::string(
				String(host.get("nation", "")).utf8().get_data());
			if (!nation.empty()) {
				group.profit_by_host[nation] = double(host.get("profit", 0.0));
			}
		}
		ws.group_lookup[group.id] = static_cast<int>(ws.groups.size());
		ws.groups.push_back(std::move(group));
	}
	finance::read_stock(ws, data);
	finance::read_bank_crisis(ws, data);
	ws.fdi_policies.clear();
	ws.fdi_lookup.clear();
	const Array saved_policies = data.get("fdi_policies", Array());
	for (int index = 0; index < saved_policies.size(); ++index) {
		const Dictionary entry = saved_policies[index];
		FdiPolicy policy;
		policy.nation = std::string(
			String(entry.get("nation", "")).utf8().get_data());
		policy.sector = std::string(
			String(entry.get("sector", "")).utf8().get_data());
		policy.mode = std::clamp(int(entry.get("mode", 0)), 0, 3);
		policy.min_local = std::clamp(double(entry.get("min_local", 0.0)), 0.0, 1.0);
		if (policy.nation.empty()) {
			continue;
		}
		ws.fdi_lookup[policy.nation + "|" + policy.sector] =
			static_cast<int>(ws.fdi_policies.size());
		ws.fdi_policies.push_back(std::move(policy));
	}
	ws.incentives.clear();
	ws.incentive_lookup.clear();
	const Array saved_incentives = data.get("sector_incentives", Array());
	for (int index = 0; index < saved_incentives.size(); ++index) {
		const Dictionary entry = saved_incentives[index];
		SectorIncentive incentive;
		incentive.nation = std::string(
			String(entry.get("nation", "")).utf8().get_data());
		incentive.sector = std::string(
			String(entry.get("sector", "")).utf8().get_data());
		incentive.tax_holiday = std::clamp(
			double(entry.get("tax_holiday", 0.0)), 0.0, 1.0);
		incentive.subsidy = std::max(0.0, double(entry.get("subsidy", 0.0)));
		if (incentive.nation.empty()) {
			continue;
		}
		ws.incentive_lookup[incentive.nation + "|" + incentive.sector] =
			static_cast<int>(ws.incentives.size());
		ws.incentives.push_back(std::move(incentive));
	}
	ws.confidence.clear();
	ws.confidence_penalty_day.clear();
	const Array saved_confidence = data.get("confidence", Array());
	for (int index = 0; index < saved_confidence.size(); ++index) {
		const Dictionary entry = saved_confidence[index];
		const std::string nation = std::string(
			String(entry.get("nation", "")).utf8().get_data());
		if (nation.empty()) {
			continue;
		}
		ws.confidence[nation] = std::clamp(
			double(entry.get("value", 1.0)), 0.0, 1.0);
		const int penalty = int(entry.get("penalty_day", 0));
		if (penalty > 0) {
			ws.confidence_penalty_day[nation] = penalty;
		}
	}
	ws.group_flows.clear();
	const Array saved_flows = data.get("group_flows", Array());
	for (int index = 0; index < saved_flows.size(); ++index) {
		const Dictionary entry = saved_flows[index];
		const std::string nation = std::string(
			String(entry.get("nation", "")).utf8().get_data());
		if (nation.empty()) {
			continue;
		}
		GroupFlow flow;
		flow.repatriated_out = std::max(0.0,
			double(entry.get("repatriated_out", 0.0)));
		flow.repatriated_in = std::max(0.0,
			double(entry.get("repatriated_in", 0.0)));
		flow.dividends = std::max(0.0, double(entry.get("dividends", 0.0)));
		flow.taxes = std::max(0.0, double(entry.get("taxes", 0.0)));
		flow.subsidy = std::max(0.0, double(entry.get("subsidy", 0.0)));
		ws.group_flows[nation] = flow;
	}
	// F7: política salarial. Save v8 e anteriores não têm as chaves: piso
	// desligado e rigidez padrão (1%/dia).
	ws.minimum_wages.clear();
	Array saved_minimum_wages = data.get("minimum_wages", Array());
	for (int index = 0; index < saved_minimum_wages.size(); ++index) {
		const Dictionary entry = saved_minimum_wages[index];
		const String tag = String(entry.get("nation", ""));
		const double value = std::clamp(double(entry.get("value", 0.0)),
			0.0, 100.0);
		if (!tag.is_empty() && value > 0.0) {
			ws.minimum_wages[std::string(tag.utf8().get_data())] = value;
		}
	}
	ws.wage_rigidity = std::clamp(
		double(data.get("wage_rigidity", ws.wage_rigidity)), 0.0, 0.25);

	Array produced = migrate_array(data.get("produced_total", Array()));
	Array consumed = migrate_array(data.get("consumed_total", Array()));
	for (int index = 0; index < ws.products; ++index) {
		ws.produced[index] = index < produced.size() ? double(produced[index]) : 0.0;
		ws.consumed_total[index] =
			index < consumed.size() ? double(consumed[index]) : 0.0;
		ws.produced_today[index] = 0.0;
		ws.consumed_today[index] = 0.0;
	}

	city::read(ws, core, data, version, migrate_array,
		migrate_index, legacy_substitute, has_saved_products,
		needs_legacy_migration, legacy_fuel_index,
		legacy_gasoline, legacy_diesel, legacy_jet);

	ws.in_transit.clear();
	Array transit = data.get("in_transit", Array());
	for (int index = 0; index < transit.size(); ++index) {
		Dictionary entry = transit[index];
		Shipment shipment;
		shipment.to_city =
			std::string(String(entry.get("to_city", "")).utf8().get_data());
		shipment.product = int(entry.get("product", 0));
		if (has_saved_products || needs_legacy_migration) {
			const int legacy_product = legacy_substitute(shipment.product);
			const int migrated = migrate_index(shipment.product);
			if (migrated < 0) {
				if (legacy_product >= 0) {
					// F0: carga de `fuel` em transito -> gasoline (a tonelagem e
					// preservada; a reserva de frota do pedido nao muda).
					shipment.product = legacy_product;
				} else {
					// Produto removido da cadeia em trânsito (farinha/pão ou
					// body_armor): carga descartada.
					continue;
				}
			} else {
				shipment.product = migrated;
			}
		}
		shipment.amount = double(entry.get("amount", 0.0));
		shipment.arrival_day = int(entry.get("arrival_day", 0));
		// F4: reserva da frota (save v5). Save anterior: frota livre ao carregar.
		shipment.fleet_nation =
			std::string(String(entry.get("fleet_nation", "")).utf8().get_data());
		shipment.fleet_trucks = double(entry.get("fleet_trucks", 0.0));
		shipment.fleet_ships = double(entry.get("fleet_ships", 0.0));
		shipment.fleet_planes = double(entry.get("fleet_planes", 0.0));
		// F0: combustivel do trecho por grau. Save v38 e anterior guardava um
		// total (`fuel`): rateia pelo consumo diario que o modal declararia
		// (avião -> querosene; caminhao/navio -> diesel) — campo de auditoria,
		// nao afeta a reserva (ja debitada no embarque).
		shipment.diesel = safe_nonneg(double(entry.get("diesel", 0.0)));
		shipment.jet_fuel = safe_nonneg(double(entry.get("jet_fuel", 0.0)));
		if (version < 39 && entry.has("fuel")) {
			const double legacy_fuel = safe_nonneg(double(entry.get("fuel", 0.0)));
			const double jet_day = shipment.fleet_planes * kFleetFuelPerPlaneDay;
			const double diesel_day = shipment.fleet_trucks * kFleetFuelPerTruckDay
				+ shipment.fleet_ships * kFleetFuelPerShipDay;
			const double total_day = jet_day + diesel_day;
			const double jet_share = total_day > 1e-12 ? jet_day / total_day : 0.0;
			shipment.diesel += legacy_fuel * (1.0 - jet_share);
			shipment.jet_fuel += legacy_fuel * jet_share;
		}
		ws.in_transit.push_back(shipment);
	}

	// Portos (campo aditivo, ausente em saves anteriores ao porto): o save
	// preserva capacidade, expansão em obra e fila do dia. Sem "ports", mantém
	// os portos do mundo já carregado e recalcula a capacidade pela população
	// que veio no save (save antigo é compatível: comportamento anterior).
	const bool has_saved_ports = data.has("ports");
	if (has_saved_ports) {
		ws.ports.clear();
		Array ports = data.get("ports", Array());
		for (int index = 0; index < ports.size(); ++index) {
			Dictionary entry = ports[index];
			Port port;
			port.id = std::string(String(entry.get("id", "")).utf8().get_data());
			port.name = std::string(String(entry.get("name", "")).utf8().get_data());
			port.nation = std::string(String(entry.get("nation", "")).utf8().get_data());
			port.landmass =
				std::string(String(entry.get("landmass", "")).utf8().get_data());
			port.host_city =
				std::string(String(entry.get("host_city", "")).utf8().get_data());
			port.center = {double(entry.get("x", 0.0)), double(entry.get("y", 0.0))};
			port.base_capacity_t_per_day =
				safe_nonneg(double(entry.get("base_capacity_t_per_day", 0.0)));
			port.capacity_t_per_day =
				safe_nonneg(double(entry.get("capacity_t_per_day",
					port.base_capacity_t_per_day)));
			port.pending_capacity_t_per_day =
				safe_nonneg(double(entry.get("pending_capacity_t_per_day", 0.0)));
			port.pending_days = std::max(0, int(entry.get("pending_days", 0)));
			ws.ports.push_back(std::move(port));
		}
	}

	core.rebuild_indexes();
	if (!has_saved_ports && !ws.ports.empty()) {
		core.init_port_capacities();
	}
	power::read(ws, data, version);
	// Recompte os membros da rede e valide o índice de cidade das usinas: a
	// carga pode trazer cidade que não existe mais (usina fica órfã, fora do
	// despacho — declarado no status).
	ogs::after_load(ws);
	// B7 (save v22): migração v21 → v22. Save anterior não tem os blocos: a
	// informalidade é semeada do dado da nação (ou âncora de renda), a
	// referência de saúde do dado (ou kHealthAccessDefault) e os contadores
	// (excesso, fome) nascem zerados. `satisfaction_prev` parte da satisfação
	// carregada (a do último dia).
	if (version < 22 && ws.world_mode) {
		for (City& city : ws.cities) {
			core.seed_informality(city);
			city.pop.satisfaction_prev = city.pop.satisfaction;
			city.region.satisfaction_prev = city.region.satisfaction;
		}
	}
	// F4: frota (save v5). Save anterior não tem o bloco: deriva de
	// população/PIB, documentado como `source: derived`.
	ws.fleets.clear();
	ws.fleet_order.clear();
	const Array saved_fleets = data.get("fleets", Array());
	for (int index = 0; index < saved_fleets.size(); ++index) {
		Dictionary entry = saved_fleets[index];
		const std::string nation =
			std::string(String(entry.get("nation", "")).utf8().get_data());
		if (nation.empty()) {
			continue;
		}
		Fleet fleet;
		fleet.trucks = safe_nonneg(double(entry.get("trucks", 0.0)));
		fleet.ships = safe_nonneg(double(entry.get("ships", 0.0)));
		fleet.planes = safe_nonneg(double(entry.get("planes", 0.0)));
		fleet.target_trucks = safe_nonneg(double(entry.get("target_trucks", 0.0)));
		fleet.target_ships = safe_nonneg(double(entry.get("target_ships", 0.0)));
		fleet.target_planes = safe_nonneg(double(entry.get("target_planes", 0.0)));
		// F0 (v39): estoque por grau. Save v38 e anterior guarda um `fuel`
		// unico: rateia pelo consumo diario de cada modal (caminhao/navio ->
		// diesel, aviao -> querosene), a mesma regra de init_fleets.
		fleet.diesel = safe_nonneg(double(entry.get("diesel", 0.0)));
		fleet.diesel_target = safe_nonneg(
			double(entry.get("diesel_target", 0.0)));
		fleet.jet_fuel = safe_nonneg(double(entry.get("jet_fuel", 0.0)));
		fleet.jet_target = safe_nonneg(double(entry.get("jet_target", 0.0)));
		const double legacy_diesel_day = fleet.trucks * kFleetFuelPerTruckDay
			+ fleet.ships * kFleetFuelPerShipDay;
		const double legacy_jet_day = fleet.planes * kFleetFuelPerPlaneDay;
		const double legacy_total_day = legacy_diesel_day + legacy_jet_day;
		const double legacy_jet_share = legacy_total_day > 1e-12
			? legacy_jet_day / legacy_total_day : 0.0;
		if (version < 39 && entry.has("fuel")) {
			const double legacy_fuel = safe_nonneg(
				double(entry.get("fuel", 0.0)));
			const double legacy_target = safe_nonneg(
				double(entry.get("fuel_target", 0.0)));
			fleet.diesel += legacy_fuel * (1.0 - legacy_jet_share);
			fleet.jet_fuel += legacy_fuel * legacy_jet_share;
			fleet.diesel_target += legacy_target * (1.0 - legacy_jet_share);
			fleet.jet_target += legacy_target * legacy_jet_share;
		}
		fleet.spare_parts = safe_nonneg(double(entry.get("spare_parts", 0.0)));
		Array health = entry.get("health", Array());
		for (int mode = 0; mode < 3; ++mode) {
			fleet.health[mode] = mode < health.size()
				? std::clamp(double(health[mode]), kFleetHealthFloor, 1.0) : 1.0;
		}
		fleet.fuel_used_total = double(entry.get("fuel_used_total", 0.0));
		fleet.fuel_bought_total = double(entry.get("fuel_bought_total", 0.0));
		fleet.diesel_used_total =
			double(entry.get("diesel_used_total", 0.0));
		fleet.jet_used_total =
			double(entry.get("jet_used_total", 0.0));
		fleet.diesel_bought_total =
			double(entry.get("diesel_bought_total", 0.0));
		fleet.jet_bought_total =
			double(entry.get("jet_bought_total", 0.0));
		if (version < 39 && !entry.has("diesel_used_total")) {
			// F0: contadores historicos do save antigo divididos pela mesma
			// proporcao de consumo (auditoria; nao entra no dia).
			fleet.diesel_used_total =
				fleet.fuel_used_total * (1.0 - legacy_jet_share);
			fleet.jet_used_total =
				fleet.fuel_used_total * legacy_jet_share;
			fleet.diesel_bought_total =
				fleet.fuel_bought_total * (1.0 - legacy_jet_share);
			fleet.jet_bought_total =
				fleet.fuel_bought_total * legacy_jet_share;
		}
		fleet.parts_used_total = double(entry.get("parts_used_total", 0.0));
		fleet.vehicles_bought_total =
			double(entry.get("vehicles_bought_total", 0.0));
		fleet.derived = bool(entry.get("derived", false));
		ws.fleets.emplace(nation, fleet);
		ws.fleet_order.push_back(nation);
	}
	// Reserva das cargas em trânsito: reconstrói os veículos ocupados.
	for (const Shipment& shipment : ws.in_transit) {
		if (shipment.fleet_nation.empty()) {
			continue;
		}
		TransportDemand busy;
		busy.trucks = shipment.fleet_trucks;
		busy.ships = shipment.fleet_ships;
		busy.planes = shipment.fleet_planes;
		const auto found = ws.fleets.find(shipment.fleet_nation);
		if (found == ws.fleets.end()) {
			continue;
		}
		found->second.trucks_busy += busy.trucks;
		found->second.ships_busy += busy.ships;
		found->second.planes_busy += busy.planes;
	}
	if (ws.fleets.empty()) {
		core.init_fleets();
	} else {
		// Índices dos bens da frota (products já carregados antes do load).
		// F0 (t145): snapshot v38 e anterior usa o `fuel` genérico no lugar de
		// diesel/querosene — o papel volta ao id antigo em vez de -1.
		ws.fleet_product_diesel = product_or_legacy(core, "diesel", "fuel");
		ws.fleet_product_jet = product_or_legacy(core, "jet_fuel", "fuel");
		ws.fleet_product_truck = core.product_index(std::string(String("truck").utf8().get_data()));
		ws.fleet_product_ship = core.product_index(std::string(String("cargo_ship").utf8().get_data()));
		ws.fleet_product_plane = core.product_index(std::string(String("cargo_plane").utf8().get_data()));
		ws.fleet_product_parts = core.product_index(std::string(String("spare_parts").utf8().get_data()));
	}

	// F8: negócios de serviço (save v10). Save v9 e anteriores não têm o bloco:
	// semeia os negócios como num mundo novo (migração declarada).
	city::read_services(ws, core, data);
	// F9: save v10 e anteriores não têm grupos. Semeia a partir dos ativos
	// privados (id determinístico); os nomes entram pelo cliente no próximo
	// boot, como no mundo recém-criado.
	if (ws.groups.empty()) {
		ws.groups_seeded = false;
		core.seed_groups();
	} else {
		ws.groups_seeded = true;
	}

	// §19 (save v13): solo urbano, moradia e prefeitura por cidade. O save v12
	// e anteriores não têm a chave: os vetores ficam como estão (vazios num load
	// frio ou com os padrões carregados pelo set_urban), sem lógica diária
	// a reconstruir. O casamento é por id, não por posição.
	city::read_urban(ws, data);

	// §19 (W2): prefeitura em operação (bloco próprio). Save v13 anterior ao W2
	// não tem a chave: fica sem repasse calibrado e sem obra, que é o estado
	// honesto (a próxima carga dos JSON preenche revenue_pct_gdp).
	city::read_city_gov(ws, data);

	// §19 W3 — estado do mercado de moradia (bloco `housing`, aditivo). Save
	// anterior ao W3 não tem a chave: os campos ficam no padrão (0 = mercado
	// ainda não rodou) e a fase deriva a base de domicílios do estoque.
	city::read_housing(ws, data, version);
	// B2: cidade que não trouxe o bloco `strata` (save v16 ou cenário
	// sintético) semeia agora, DEPOIS do bloco `urban` (posse de moradia já
	// carregada). A poupança escalar existente é repartida pela renda.
	for (City& city : ws.cities) {
		if (!city.pop.strata_ready) {
			core.seed_strata(city, nullptr);
		}
	}
	// G (v24) — estado da safra por (cidade, produto, safra). Save v23 e
	// anteriores não têm o bloco: o estado nasce zerado (a safra do ano
	// corrente recomeça do zero no dia do load; nada é inventado para trás).
	if (version >= 24) {
		const Dictionary agriculture = data.get("agriculture", Dictionary());
		ws.agri_month_key = int(agriculture.get("month_key", 0));
		const Array plans = agriculture.get("plans", Array());
		for (int index = 0; index < plans.size(); ++index) {
			const Dictionary entry = plans[index];
			const std::string city = std::string(
				String(entry.get("city", String())).utf8().get_data());
			const std::string product = std::string(
				String(entry.get("product", String())).utf8().get_data());
			if (city.empty() || product.empty()) {
				continue;
			}
			const auto product_found = ws.product_lookup.find(product);
			if (product_found == ws.product_lookup.end()) {
				continue;
			}
			const auto plan_found = ws.crop_plan_lookup.find(
				city + "|" + std::to_string(product_found->second));
			if (plan_found == ws.crop_plan_lookup.end()) {
				continue;
			}
			CropPlan& plan = ws.crop_plans[
				static_cast<std::size_t>(plan_found->second)];
			// D-126 (v41): área efetiva do plano. v40 e anteriores chegam com
			// área cheia (1.0) — nada inventado para trás.
			plan.area_scale = version >= 41
				? std::clamp(double(entry.get("area_scale", 1.0)), 0.10, 1.0)
				: 1.0;
			const Array seasons = entry.get("seasons", Array());
			const int count = std::min(plan.season_count, seasons.size());
			for (int slot = 0; slot < count; ++slot) {
				const Dictionary item = seasons[slot];
				CropSeasonState& state = plan.state[slot];
				state.planted_year = int(item.get("planted_year", 0));
				state.yield_scale = std::clamp(
					double(item.get("yield_scale", 1.0)), 0.0, 1.0);
				state.harvest_yield_scale = std::clamp(
					double(item.get("harvest_yield_scale",
						double(item.get("yield_scale", 1.0)))), 0.0, 1.0);
				state.fertilizer_t = safe_nonneg(
					double(item.get("fertilizer_t", 0.0)));
				state.harvest_year = int(item.get("harvest_year", 0));
				state.delivered_t = safe_nonneg(
					double(item.get("delivered_t", 0.0)));
				state.loss_month_t = safe_nonneg(
					double(item.get("loss_month_t", 0.0)));
				state.loss_month_key = int(item.get("loss_month_key", 0));
				state.planted_area_scale = version >= 41
					? std::clamp(double(item.get("planted_area_scale", 1.0)),
						0.10, 1.0) : 1.0;
				state.harvest_area_scale = version >= 41
					? std::clamp(double(item.get("harvest_area_scale", 1.0)),
						0.10, 1.0) : 1.0;
			}
		}
	}
	// G2 (v25) — ARMAZENAGEM. Save v24 e anteriores não têm o bloco: nenhuma
	// capacidade ou tonelada é inventada para trás; o StorageService reaplica a
	// capacidade inicial (dado ou regra declarada) depois do load.
	if (version >= 25 && data.has("storage")) {
		const Dictionary storage = data.get("storage", Dictionary());
		ws.storage_month_key = int(storage.get("month_key", 0));
		const Array sites = storage.get("sites", Array());
		ws.storage_sites.clear();
		ws.storage_sites.reserve(static_cast<std::size_t>(sites.size()));
		for (int index = 0; index < sites.size(); ++index) {
			const Dictionary entry = sites[index];
			const std::string city = std::string(
				String(entry.get("city", String())).utf8().get_data());
			const std::string type_name = std::string(
				String(entry.get("type", String())).utf8().get_data());
			const std::string owner = std::string(
				String(entry.get("owner", String())).utf8().get_data());
			int type = -1;
			int scope = -1;
			if (city.empty()
					|| !storage_type_from_name(type_name, type)
					|| !storage_scope_from_name(owner, scope)) {
				continue;
			}
			const auto city_found = ws.city_lookup.find(city);
			if (city_found == ws.city_lookup.end()) {
				continue;
			}
			StorageSite site;
			site.city = city;
			site.city_index = city_found->second;
			site.type = type;
			site.scope = scope;
			site.capacity_t = safe_nonneg(
				double(entry.get("capacity_t", 0.0)));
			site.stock.assign(static_cast<std::size_t>(ws.products), 0.0);
			site.inflow_stock.assign(
				static_cast<std::size_t>(ws.products), 0.0);
			const Array stock = entry.get("stock", Array());
			for (int slot = 0; slot < stock.size(); ++slot) {
				const Dictionary item = stock[slot];
				const std::string product = std::string(
					String(item.get("product", String())).utf8().get_data());
				// F0: estoque do `fuel` generico no armazem de um save antigo:
				// rateado nas tres grades (mesmas fracoes da refinaria).
				if (legacy_fuel_migration && product == "fuel") {
					const double tons = safe_nonneg(
						double(item.get("tons", 0.0)));
					const double inflow = std::min(tons,
						safe_nonneg(double(item.get("inflow_t", 0.0))));
					site.stock[legacy_gasoline] +=
						tons * kLegacyFuelGasolineShare;
					site.stock[legacy_diesel] +=
						tons * kLegacyFuelDieselShare;
					site.stock[legacy_jet] += tons * kLegacyFuelJetShare;
					site.inflow_stock[legacy_gasoline] +=
						inflow * kLegacyFuelGasolineShare;
					site.inflow_stock[legacy_diesel] +=
						inflow * kLegacyFuelDieselShare;
					site.inflow_stock[legacy_jet] +=
						inflow * kLegacyFuelJetShare;
					continue;
				}
				const auto product_found = ws.product_lookup.find(product);
				if (product_found == ws.product_lookup.end()) {
					continue;
				}
				const std::size_t product_index =
					static_cast<std::size_t>(product_found->second);
				if (product_index >= site.stock.size()) {
					continue;
				}
				const double tons = safe_nonneg(
					double(item.get("tons", 0.0)));
				const double inflow = std::min(tons,
					safe_nonneg(double(item.get("inflow_t", 0.0))));
				site.stock[product_index] += tons;
				site.inflow_stock[product_index] += inflow;
			}
			ws.storage_sites.push_back(std::move(site));
		}
	}
	// C2c (v31/D-108) — REGIMES REAIS e SUCESSÃO de moeda.
	// 1. O bloco salvo (v31) manda; save v30 e anteriores fica com o que o
	//    `set_fx_regimes` carregou do arquivo ANTES do load (o mundo antigo
	//    recebe os regimes reais — decisão D-108a).
	if (data.has("fx_regimes")) {
		// v31: o bloco do save substitui o que veio do arquivo.
		ws.fx_regime.clear();
		ws.fx_no_policy_nations.clear();
		ws.fx_regime_source.clear();
		const Dictionary block = data.get("fx_regimes", Dictionary());
		ws.fx_regime_source = std::string(
			String(block.get("source", "")).utf8().get_data());
		const Variant no_policy_variant = block.get("no_policy", Array());
		if (no_policy_variant.get_type() == Variant::ARRAY) {
			const Array no_policy = no_policy_variant;
			for (int index = 0; index < no_policy.size(); ++index) {
				const std::string tag(String(no_policy[index]).utf8().get_data());
				if (!tag.empty()) {
					ws.fx_no_policy_nations.push_back(tag);
				}
			}
		}
		const Variant rows_variant = block.get("currencies", Dictionary());
		if (rows_variant.get_type() == Variant::DICTIONARY) {
			const Dictionary rows = rows_variant;
			for (const auto& row_entry : rows.entries()) {
				if (row_entry.first.empty()
						|| row_entry.second.get_type()
							!= Variant::DICTIONARY) {
					continue;
				}
				const std::string code(
					String(row_entry.first).utf8().get_data());
				const Dictionary row = row_entry.second;
				FxRegimeInfo info;
				info.regime = central_bank::regime_from_name(
					std::string(String(row.get("regime", "")).utf8().get_data()),
					central_bank::kRegimeBand);
				info.anchor = std::string(String(row.get("anchor", ""))
					.utf8().get_data());
				info.anchor_type = std::string(String(row.get("anchor_type",
					"none")).utf8().get_data());
				info.anchor_source = std::string(String(row.get(
					"anchor_source", "")).utf8().get_data());
				info.issuer = std::string(String(row.get("issuer", ""))
					.utf8().get_data());
				info.locked = bool(row.get("locked", false));
				info.source = std::string(String(row.get("source", ""))
					.utf8().get_data());
				const Variant basket_variant = row.get("basket", Variant());
				if (basket_variant.get_type() == Variant::DICTIONARY) {
					const Dictionary basket = basket_variant;
					for (const auto& unit : basket.entries()) {
						const std::string unit_code(
							String(unit.first).utf8().get_data());
						const double weight = double(unit.second);
						if (!unit_code.empty() && std::isfinite(weight)
								&& weight > 0.0) {
							info.basket[unit_code] = weight;
						}
					}
				}
				ws.fx_regime[code] = std::move(info);
			}
		}
	}
	// 2. O regime SALVO da moeda acompanha o regime real (o dado vence o
	//    "peg administrativo" que o save v30 carregava por default).
	for (Currency& currency : ws.currencies) {
		const auto found = ws.fx_regime.find(currency.code);
		if (found == ws.fx_regime.end()) {
			continue;
		}
		currency.exchange_regime = found->second.regime;
		currency.floating = found->second.regime
			!= central_bank::kRegimePeg;
	}
	// 3. Moeda extinta (D-108e): funde no substituto conservando valor.
	for (const CurrencySuccession& row : kCurrencySuccessions) {
		exchange::migrate_currency(ws, row.from, row.to,
			row.units_per_target);
	}
	// 4. Paridade cruzada onde o save antigo não tinha paridade.
	exchange::initialize_anchors(ws);
	// Índices derivados dos planos (cidade × produto) e a resolução do bem
	// `fertilizer`: só agora as cidades existem.
	core.rebuild_crop_indexes();
	// G2: mapeamento produto→tipo e índice de cidade dos armazéns carregados.
	rebuild_storage_indexes(ws);
	// C5 (D-112): v32 e anteriores recebem a bolsa criada aqui (preço =
	// patrimônio medido, carteira do default declarado); v33+ só completa
	// linhas novas. Depende de grupos, construções e bancos já carregados.
	stock::after_load(ws, version);
	// C6 (D-114): v33 e anteriores recebem os bancos CALMOS (confianca 1,0),
	// fundo 0 e nenhum evento; v34+ carrega o estado salvo.
	bank_crisis::after_load(ws, version);
}

} // namespace ogs::save