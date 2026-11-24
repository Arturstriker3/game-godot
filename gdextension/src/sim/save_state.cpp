// Serialização save/load dentro da biblioteca de simulação (sem godot-cpp):
// porte mecânico de edge/save_load.cpp para ogs::save, com os aliases
// Array/Dictionary/Variant do shim de valor (save_value.h).
//
// As chaves, a ORDEM de inserção em `data[...]`, os valores e todas as
// expressões numéricas são idênticas às do original: o save_sha256 do jogo
// depende disso.
#include "save_state.h"

#include "bank.h"
#include "bank_crisis.h"
#include "central_bank.h"
#include "crisis.h"
#include "exchange.h"
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

namespace {

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
static Dictionary save_informal_block(double share, const double* labor_share,
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

static Dictionary save_mortality_block(double deaths_expected, double deaths_total,
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

static Dictionary save_dissatisfaction_block(double index, const double* by_stratum,
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

static void load_informal_block(const Dictionary& d, double& share,
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

static void load_mortality_block(const Dictionary& d, double& deaths_expected,
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

static void load_dissatisfaction_block(const Dictionary& d, double& index,
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
inline String sim_str(const char* utf8) {
	return String(utf8);
}

inline double safe_nonneg(double v) {
	return std::isfinite(v) ? std::max(0.0, v) : 0.0;
}

// Fração 0..1 (taxas da prefeitura e condição do estoque, save v13).
inline double safe_unit(double v) {
	return std::isfinite(v) ? std::clamp(v, 0.0, 1.0) : 0.0;
}

// População de SIMULAÇÃO da cidade. Desde a calibração de 2026, `population`
// no cenário é a população real exibida (nação inteira rateada) e a aglomeração
// da fonte viaja em `sim_population`. Sem o campo (save antigo, cenário
// sintético), a população histórica É a unidade de simulação — comportamento
// anterior preservado.
inline double read_sim_population(const Dictionary& data, double population) {
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
// consulta.
constexpr double kLegacyFuelGasolineShare = 45.0 / 83.0;
constexpr double kLegacyFuelDieselShare = 29.0 / 83.0;
constexpr double kLegacyFuelJetShare = 9.0 / 83.0;

// F0: ids de receita do combustivel generico (saves v38 e anteriores) mapeados
// para a receita que ocupa o lugar. O id novo vem de recipes.json; a receita
// antiga continua valida para um snapshot antigo (que traz a propria lista).
static const char* legacy_recipe_alias(const std::string& id) {
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

} // namespace

Value to_value(const WorldState& ws, const SimCore& core) {
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

	Array cities = make_array();
	for (const City& city : ws.cities) {
		Dictionary entry = make_object();
		entry["id"] = sim_str(city.id.c_str());
		entry["name"] = sim_str(city.name.c_str());
		entry["nation"] = sim_str(city.nation.c_str());
		entry["home_nation"] = sim_str(city.home_nation.c_str());
		entry["landmass"] = sim_str(city.landmass.c_str());
		Array center = make_array();
		center.append(city.center.x);
		center.append(city.center.y);
		entry["center"] = center;
		entry["population"] = city.population;
		entry["sim_population"] = city.simulation_population;
		entry["firm_cash"] = city.firm_cash;
		entry["service_cash"] = city.service_cash;
		// D-126 (v41): crédito de capital de giro da cidade e o prêmio de crise.
		entry["firm_debt"] = city.firm_debt;
		entry["firm_credit_arrears"] = city.firm_credit_arrears;
		entry["firm_credit_total"] = city.firm_credit_total;
		entry["firm_credit_rate_add"] = city.firm_credit_rate_add;
		entry["firm_credit_default_days"] = city.firm_credit_default_days;
		entry["firm_credit_defaults"] = city.firm_credit_defaults;
		entry["firm_credit_renegotiations"] =
			city.firm_credit_renegotiations;
		// C1 (save v26): salário atrasado das firmas da cidade (passivo).
		entry["service_wage_arrears"] = city.service_wage_arrears;
		entry["rural_wage_arrears"] = city.rural_wage_arrears;
		entry["government_budget"] = city.government_budget;
		entry["service_jobs"] = city.service_jobs;
		entry["service_wage"] = city.service_wage;
		entry["service_wage_bill"] = city.service_wage_bill;
		entry["service_revenue"] = city.service_revenue;
		entry["service_profit"] = city.service_profit;
		entry["opex_revenue"] = city.opex_revenue;
		entry["freight_revenue"] = city.freight_revenue;
		entry["construction_revenue"] = city.construction_revenue;
		entry["construction_revenue_pending"] = city.construction_revenue_pending;
		entry["capital_income_next"] = city.capital_income_next;
		// B2: aluguel pendente às famílias donas (pago no dia seguinte, como o
		// capital). Sem persistir, o load perde a fatura e a conservação muda.
		entry["rent_income_next"] = city.rent_income_next;
		entry["gdp"] = city.gdp;
		entry["external_income_index"] = city.external_income_index;
		// TFP: 0 = derivada do índice de renda (não duplica o dado no save).
		entry["productivity_index"] = city.productivity_index;
		// Gini: 0 = não declarado (legado por escolaridade).
		entry["income_gini"] = city.income_gini;
		// F8: serviços (save v10). Mercados dos 20 setores em arrays de 12
		// campos (ordem fixa) + fluxos/caixa do dia e cobertura privada.
		entry["service_export_revenue"] = city.service_export_revenue;
		entry["service_government_revenue"] = city.service_government_revenue;
		entry["service_input_cost"] = city.service_input_cost;
		entry["service_business_profit"] = city.service_business_profit;
		entry["service_business_count"] = city.service_business_count;
		entry["private_health_cover"] = city.private_health_cover;
		entry["private_education_cover"] = city.private_education_cover;
		entry["service_tonnage_prev"] = city.service_tonnage_prev;
		Array service_markets = make_array();
		for (int sector = 0; sector < kMaxServiceSectors; ++sector) {
			const ServiceMarket& market = city.service_markets[sector];
			Array fields = make_array();
			fields.append(market.demand);
			fields.append(market.supply);
			fields.append(market.unmet);
			fields.append(market.price);
			fields.append(market.base_price);
			fields.append(market.revenue);
			fields.append(market.input_cost);
			fields.append(market.service_cost);
			fields.append(market.jobs);
			fields.append(market.businesses);
			fields.append(market.exports);
			fields.append(market.export_revenue);
			fields.append(market.coverage);
			service_markets.append(fields);
		}
		entry["services"] = service_markets;

		Array suitability = make_array();
		Array revenue = make_array();
		Array imported = make_array();
		Array exported = make_array();
		Array import_value = make_array();
		Array export_value = make_array();
		Array external_demand = make_array();
		Array import_price = make_array();
		for (int product = 0; product < ws.products; ++product) {
			suitability.append(city.suitability[product]);
			revenue.append(city.revenue_by_product[product]);
			imported.append(city.imported[product]);
			exported.append(city.exported[product]);
			import_value.append(city.import_value[product]);
			export_value.append(city.export_value[product]);
			external_demand.append(city.external_demand[product]);
			import_price.append(city.import_price[product]);
		}
		entry["suitability"] = suitability;
		entry["revenue_by_product"] = revenue;
		entry["imported"] = imported;
		entry["exported"] = exported;
		entry["import_value"] = import_value;
		entry["export_value"] = export_value;
		entry["external_demand"] = external_demand;
		entry["import_price"] = import_price;

		// Reservas minerais (só jazidas declaradas): o saldo restante persiste.
		Dictionary deposits = make_object();
		for (int product = 0; product < ws.products; ++product) {
			const Deposit& deposit = city.deposits[product];
			if (!deposit.tracked) {
				continue;
			}
			Dictionary deposit_entry = make_object();
			deposit_entry["initial_kt"] = deposit.initial_kt;
			deposit_entry["remaining_kt"] = deposit.remaining_kt;
			deposit_entry["extracted_kt"] = deposit.extracted_kt;
			deposit_entry["quality"] = deposit.quality;
			deposit_entry["depth_m"] = deposit.depth_m;
			deposits[String(core.product_name(product))] = deposit_entry;
		}
		entry["deposits"] = deposits;

		Dictionary pop = make_object();
		pop["population"] = city.pop.population;
		pop["labor_force"] = city.pop.labor_force;
		pop["employed"] = city.pop.employed;
		pop["unemployed"] = city.pop.unemployed;
		pop["dependents"] = city.pop.dependents;
		pop["wage_income"] = city.pop.wage_income;
		pop["capital_income"] = city.pop.capital_income;
		pop["transfers"] = city.pop.transfers;
		pop["taxes"] = city.pop.taxes;
		pop["service_spend"] = city.pop.service_spend;
		pop["disposable"] = city.pop.disposable;
		pop["external_income"] = city.pop.external_income;
		pop["food_spend"] = city.pop.food_spend;
		pop["consumed"] = city.pop.consumed;
		pop["need"] = city.pop.need;
		pop["satisfaction"] = city.pop.satisfaction;
		// B2: quando os quintis existem, a poupança gravada é a SOMA deles
		// (mesma ordem do load) — o round-trip sem tick fica bit-exato; o
		// escalar é só um cache da soma.
		double savings_saved = city.pop.savings;
		if (city.pop.strata_ready) {
			savings_saved = 0.0;
			for (int s = 0; s < kIncomeStrata; ++s) {
				savings_saved += city.pop.stratum_savings[s];
			}
		}
		pop["savings"] = savings_saved;
		// C1 (save v26): salário atrasado a receber (ativo das famílias;
		// não é poupança e não entra em `money_total`).
		pop["arrears"] = city.pop.arrears_receivable;
		Array desired_by_category = make_array();
		Array consumed_by_category = make_array();
		for (int product = 0; product < static_cast<int>(ws.food_products.size()); ++product) {
			desired_by_category.append(city.pop.desired_by_category[product]);
			consumed_by_category.append(city.pop.consumed_by_category[product]);
		}
		pop["desired_by_category"] = desired_by_category;
		pop["consumed_by_category"] = consumed_by_category;
		// F3: estado das necessidades (níveis, duráveis e gasto próprio). Campos
		// aditivos: save v4 anterior continua carregando (default zero).
		Dictionary needs_state = make_object();
		needs_state["level"] = city.pop.needs_level;
		needs_state["satisfaction"] = city.pop.needs_satisfaction;
		needs_state["spend"] = city.pop.needs_spend;
		needs_state["durable_spend"] = city.pop.durable_spend;
		Array level_satisfaction = make_array();
		Array level_want = make_array();
		Array level_consumed = make_array();
		for (int level = 0; level < kMaxNeedsLevels; ++level) {
			level_satisfaction.append(level < static_cast<int>(
				city.pop.level_satisfaction.size()) ? city.pop.level_satisfaction[level] : 0.0);
			level_want.append(level < static_cast<int>(
				city.pop.level_want.size()) ? city.pop.level_want[level] : 0.0);
			level_consumed.append(level < static_cast<int>(
				city.pop.level_consumed.size()) ? city.pop.level_consumed[level] : 0.0);
		}
		needs_state["level_satisfaction"] = level_satisfaction;
		needs_state["level_want"] = level_want;
		needs_state["level_consumed"] = level_consumed;
		Dictionary durable_stock = make_object();
		for (int product = 0; product < ws.products
				&& product < static_cast<int>(city.pop.durable_stock.size()); ++product) {
			if (city.pop.durable_stock[static_cast<std::size_t>(product)] > 0.0) {
				durable_stock[String(core.product_name(product))] =
					city.pop.durable_stock[static_cast<std::size_t>(product)];
			}
		}
		needs_state["durable_stock"] = durable_stock;
		pop["needs"] = needs_state;
		// B1 (save v16): coortes etárias (5 faixas) e os pesos de referência de
		// saúde/educação do dia da semeadura. Array vazio = cidade sem estrutura
		// nacional (perfil paramétrico legado, sem coorte a persistir).
		Array age_bands = make_array();
		if (city.pop.demography_ready) {
			for (int band = 0; band < kAgeBands; ++band) {
				age_bands.append(std::max(0.0, city.pop.age_band[band]));
			}
		}
		pop["age_bands"] = age_bands;
		pop["health_weight_ref"] = std::max(0.0, city.pop.health_weight_ref);
		pop["education_weight_ref"] =
			std::max(0.0, city.pop.education_weight_ref);
		// F7: mercado de trabalho e educação (save v9). Bloco aditivo: os campos
		// de compatibilidade acima continuam iguais.
		Dictionary labor = make_object();
		labor["working_age"] = city.pop.working_age;
		labor["retirees"] = city.pop.retirees;
		labor["students"] = city.pop.students;
		labor["recruit_reserve"] = city.pop.recruit_reserve;
		labor["education"] = Array{city.pop.education[0],
			city.pop.education[1], city.pop.education[2]};
		labor["participation"] = Array{city.pop.participation[0],
			city.pop.participation[1], city.pop.participation[2]};
		labor["labor_force_by_level"] = Array{
			city.pop.labor_force_by_level[0],
			city.pop.labor_force_by_level[1],
			city.pop.labor_force_by_level[2]};
		labor["employed_by_level"] = Array{city.pop.employed_by_level[0],
			city.pop.employed_by_level[1], city.pop.employed_by_level[2]};
		labor["unemployed_by_level"] = Array{
			city.pop.unemployed_by_level[0],
			city.pop.unemployed_by_level[1],
			city.pop.unemployed_by_level[2]};
		labor["vacancies_by_level"] = Array{
			city.pop.vacancies_by_level[0],
			city.pop.vacancies_by_level[1],
			city.pop.vacancies_by_level[2]};
		labor["unfilled_by_level"] = Array{
			city.pop.unfilled_by_level[0],
			city.pop.unfilled_by_level[1],
			city.pop.unfilled_by_level[2]};
		labor["wage_by_level"] = Array{city.pop.wage_by_level[0],
			city.pop.wage_by_level[1], city.pop.wage_by_level[2]};
		labor["wage_income_by_level"] = Array{
			city.pop.wage_income_by_level[0],
			city.pop.wage_income_by_level[1],
			city.pop.wage_income_by_level[2]};
		labor["studying"] = Array{city.pop.studying[0], city.pop.studying[1]};
		labor["income_level"] = city.pop.income_level;
		labor["wage_anchored"] = city.pop.wage_anchored;
		labor["income_level_share"] = Array{city.pop.income_level_share[0],
			city.pop.income_level_share[1], city.pop.income_level_share[2],
			city.pop.income_level_share[3]};
		labor["income_level_by_stratum"] = Array{
			city.pop.stratum_income_level[0],
			city.pop.stratum_income_level[1],
			city.pop.stratum_income_level[2]};
		pop["labor"] = labor;
		// B2 (save v17): cinco quintis de renda. Aditivo — o save anterior
		// migra semeando a proxy do Gini declarado (strata.cpp).
		Dictionary strata = make_object();
		Array strata_income_share = make_array();
		Array strata_people = make_array();
		Array strata_wage_factor = make_array();
		Array strata_capital_share = make_array();
		Array strata_savings = make_array();
		Array strata_debt = make_array();
		Array strata_equity = make_array();
		Array strata_wealth = make_array();
		Array strata_saving_rate = make_array();
		Array strata_net_income = make_array();
		Array strata_rent_paid = make_array();
		Array strata_level = make_array();
		Array strata_education = make_array();
		Array strata_tenure = make_array();
		Array strata_lifecycle = make_array();
		for (int s = 0; s < kIncomeStrata; ++s) {
			strata_income_share.append(city.pop.strata_income_share[s]);
			strata_people.append(city.pop.stratum_people[s]);
			strata_wage_factor.append(city.pop.stratum_wage_factor[s]);
			strata_capital_share.append(city.pop.stratum_capital_share[s]);
			strata_savings.append(city.pop.stratum_savings[s]);
			strata_debt.append(city.pop.stratum_debt[s]);
			strata_equity.append(city.pop.stratum_equity[s]);
			strata_wealth.append(city.pop.stratum_wealth[s]);
			strata_saving_rate.append(city.pop.stratum_saving_rate[s]);
			strata_net_income.append(city.pop.stratum_net_income[s]);
			strata_rent_paid.append(city.pop.stratum_rent_paid[s]);
			strata_level.append(city.pop.stratum_level[s]);
			Array education_row = make_array();
			Array tenure_row = make_array();
			for (int e = 0; e < kEducationLevels; ++e) {
				education_row.append(city.pop.stratum_education_mix[s][e]);
			}
			for (int kind = 0; kind < kStrataTenureCount; ++kind) {
				tenure_row.append(city.pop.stratum_tenure[s][kind]);
			}
			strata_education.append(education_row);
			strata_tenure.append(tenure_row);
		}
		for (int band = 0; band < 5; ++band) {
			strata_lifecycle.append(city.pop.lifecycle_saving[band]);
		}
		strata["ready"] = city.pop.strata_ready;
		strata["income_share"] = strata_income_share;
		strata["people"] = strata_people;
		strata["wage_factor"] = strata_wage_factor;
		strata["capital_share"] = strata_capital_share;
		strata["savings"] = strata_savings;
		strata["debt"] = strata_debt;
		strata["equity"] = strata_equity;
		strata["wealth"] = strata_wealth;
		strata["saving_rate"] = strata_saving_rate;
		strata["net_income"] = strata_net_income;
		strata["rent_paid"] = strata_rent_paid;
		strata["level"] = strata_level;
		strata["education"] = strata_education;
		strata["tenure"] = strata_tenure;
		strata["lifecycle"] = strata_lifecycle;
		pop["strata"] = strata;
		// B7 (save v22): informalidade, mortalidade e insatisfação (urbano).
		pop["informal"] = save_informal_block(city.pop.informal_share,
			city.pop.informal_labor_share, city.pop.informal_income,
			city.pop.informal_wage_bill, city.pop.informal_service_workers,
			city.pop.informal_service_output, city.pop.informal_service_demand,
			city.pop.informal_service_revenue, city.pop.informal_service_price,
			city.pop.informal_service_share);
		pop["mortality"] = save_mortality_block(city.pop.deaths_expected,
			city.pop.deaths_total, city.pop.excess_deaths_total,
			city.pop.excess_deaths_famine_total, city.pop.famine_months,
			city.pop.food_price_ref, city.pop.food_inflation_month,
			city.pop.health_access_ref, city.pop.satisfaction_prev);
		pop["dissatisfaction"] = save_dissatisfaction_block(
			city.pop.dissatisfaction, city.pop.dissatisfaction_by_stratum,
			city.pop.dissatisfaction_unemployment, city.pop.dissatisfaction_rent,
			city.pop.dissatisfaction_food, city.pop.dissatisfaction_inflation,
			city.pop.dissatisfaction_informality);
		entry["pop"] = pop;
		// B3 (save v19): região da cidade (estados inteiros). Estado persistente
		// (população, caixa, coortes e quintis do interior); os fluxos diários
		// (emprego, renda, comida, satisfação) são derivados e recalculados.
		{
			const Region& region = city.region;
			Dictionary region_entry = make_object();
			region_entry["ready"] = region.ready;
			region_entry["name"] = sim_str(region.name.c_str());
			region_entry["population_region"] = region.population_region;
			region_entry["urban_population"] = region.urban_population;
			region_entry["region_pop"] = region.region_pop;
			region_entry["urban_share"] = region.urban_share;
			region_entry["cash"] = region.cash;
			// B7: a renda do dia (semanal) é usada pelo serviço informal diário
			// (compute_region_informal_services); sem persistir, o load zerava a
			// renda e a fatia informal divergia do caminho direto. São fluxos
			// derivados, mas o serviço informal depende deles entre os fechos
			// semanais — por isso viajam no save.
			region_entry["wage_income"] = region.wage_income;
			region_entry["income"] = region.income;
			region_entry["taxes"] = region.taxes;
			Array mix = make_array();
			mix.append(region.employment_mix[0]);
			mix.append(region.employment_mix[1]);
			mix.append(region.employment_mix[2]);
			region_entry["employment_mix"] = mix;
			Array region_states = make_array();
			for (const RegionState& state : region.states) {
				Dictionary state_entry = make_object();
				state_entry["id"] = sim_str(state.id.c_str());
				state_entry["name"] = sim_str(state.name.c_str());
				state_entry["population"] = state.population;
				state_entry["area_km2"] = state.area_km2;
				region_states.append(state_entry);
			}
			region_entry["states"] = region_states;			Array region_age_bands = make_array();
			if (region.demography_ready) {
				for (int band = 0; band < kAgeBands; ++band) {
					region_age_bands.append(std::max(0.0, region.age_band[band]));
				}
			}
			region_entry["age_bands"] = region_age_bands;
			Dictionary region_strata = make_object();
			region_strata["ready"] = region.strata_ready;
			Array region_income_share = make_array();
			Array region_people = make_array();
			Array region_savings = make_array();
			for (int s = 0; s < kIncomeStrata; ++s) {
				region_income_share.append(region.strata_income_share[s]);
				region_people.append(region.stratum_people[s]);
				region_savings.append(region.stratum_savings[s]);
			}
			region_strata["income_share"] = region_income_share;
			region_strata["people"] = region_people;
			region_strata["savings"] = region_savings;
			region_entry["strata"] = region_strata;
			// C1 (save v26): salário rural atrasado a receber (ativo das
			// famílias do interior).
			region_entry["arrears"] = region.arrears_receivable;
			// B4 (save v20): migração — acumuladores (o fluxo do mês é derivado).
			Dictionary region_migration = make_object();
			region_migration["rural_in_total"] = region.migration_rural_in_total;
			region_migration["city_in_total"] = region.migration_city_in_total;
			region_migration["city_out_total"] = region.migration_city_out_total;
			region_entry["migration"] = region_migration;
			// B5 (save v21): infraestrutura do estado — níveis (a manutenção do
			// dia é derivada e não vai ao save).
			Dictionary region_infra = make_object();
			region_infra["road"] = region.infra_road;
			region_infra["power"] = region.infra_power;
			region_infra["water"] = region.infra_water;
			region_entry["infra"] = region_infra;
			// B6 (save v23): patrimônio da região — terra, moradia, anel. Os
			// valores de ativo NÃO são dinheiro; a alíquota (0 por padrão) e a
			// receita potencial são campo publicado, não arrecadação.
			{
				Dictionary region_property = make_object();
				region_property["ready"] = region.property_ready;
				region_property["land_ha"] = region.land_ha;
				region_property["land_gini"] = region.land_gini;
				region_property["land_gini_observed"] =
					region.land_gini_observed;
				region_property["land_value_per_ha"] = region.land_value_per_ha;
				region_property["land_book_value_per_ha"] =
					region.land_book_value_per_ha;
				region_property["land_income_month"] = region.land_income_month;
				region_property["commodity_index"] = region.commodity_index;
				region_property["housing_price"] = region.housing_price;
				region_property["housing_book_price"] = region.housing_book_price;
				region_property["housing_rent"] = region.housing_rent;
				region_property["ring_population"] = region.ring_population;
				region_property["ring_price"] = region.ring_price;
				region_property["ring_book_price"] = region.ring_book_price;
				region_property["ring_rent"] = region.ring_rent;
				region_property["ring_rent_paid_month"] =
					region.ring_rent_paid_month;
				region_property["migration_ring_in_total"] =
					region.migration_ring_in_total;
				region_property["migration_month_ring_in"] =
					region.migration_month_ring_in;
				region_property["tax_rate"] = region.property_tax_rate;
				region_property["tax_potential"] = region.property_tax_potential;
				const auto put_values = [&region_property](
						const char* key, const double* values) {
					Array row = make_array();
					for (int s = 0; s < kIncomeStrata; ++s) {
						row.append(values[s]);
					}
					region_property[key] = row;
				};
				put_values("land_value", region.stratum_land_value);
				put_values("home_value", region.stratum_home_value);
				put_values("ring_value", region.stratum_ring_value);
				put_values("wealth", region.stratum_wealth);
				region_entry["property"] = region_property;
			}
			// B7 (save v22): informalidade, mortalidade e insatisfação (interior).
			region_entry["informal"] = save_informal_block(region.informal_share,
				region.informal_labor_share, region.informal_income,
				region.informal_wage_bill, region.informal_service_workers,
				region.informal_service_output, region.informal_service_demand,
				region.informal_service_revenue, region.informal_service_price,
				region.informal_service_share);
			region_entry["mortality"] = save_mortality_block(region.deaths_expected,
				region.deaths_total, region.excess_deaths_total,
				region.excess_deaths_famine_total, region.famine_months,
				region.food_price_ref, region.food_inflation_month,
				region.health_access_ref, region.satisfaction_prev);
			region_entry["dissatisfaction"] = save_dissatisfaction_block(
				region.dissatisfaction, region.dissatisfaction_by_stratum,
				region.dissatisfaction_unemployment, region.dissatisfaction_rent,
				region.dissatisfaction_food, region.dissatisfaction_inflation,
				region.dissatisfaction_informality);
			entry["region"] = region_entry;
		}
		entry["government_goods_spend"] = city.government_goods_spend;
		// §15 (save v12): emprego setorial (agro/indústria/serviços + informal).
		// É estado DERIVADO, mas o prólogo usa `sector_workers[1]` como base da
		// capacidade de construção do dia — sem persistir, o load zera o vetor e
		// a capacidade cai para o piso no primeiro tick (divergência de round-trip).
		entry["sector_workers"] = Array{city.sector_workers[0],
			city.sector_workers[1], city.sector_workers[2], city.sector_workers[3]};
		entry["sector_share"] = Array{city.sector_share[0],
			city.sector_share[1], city.sector_share[2], city.sector_share[3]};

		Array markets = make_array();
		for (int product = 0; product < ws.products; ++product) {
			Dictionary market = make_object();
			market["stock"] = city.markets[product].stock;
			market["price"] = city.markets[product].price;
			market["demand"] = city.markets[product].demand;
			market["supply"] = city.markets[product].supply;
			market["desired"] = city.markets[product].desired;
			market["sold"] = city.markets[product].sold;
			market["sold_prev"] = city.markets[product].sold_prev;
			market["shortage"] = city.markets[product].shortage;
			market["surplus"] = city.markets[product].surplus;
			markets.append(market);
		}
		entry["markets"] = markets;
		cities.append(entry);
	}
	data["cities"] = cities;

	Array buildings = make_array();
	for (const Building& building : ws.buildings) {
		Dictionary entry = make_object();
		entry["id"] = sim_str(building.id.c_str());
		entry["city"] = sim_str(building.city.c_str());
		entry["kind"] = building.kind;
		entry["resource"] = sim_str(building.resource.c_str());
		entry["owner"] = sim_str(building.owner.c_str());
		entry["local_share"] = building.local_share;
		entry["partner_group"] = sim_str(building.partner_group.c_str());
		entry["input_product"] = building.input_product;
		entry["output_product"] = building.output_product;
		entry["recipe"] = building.recipe >= 0
			&& building.recipe < static_cast<int>(ws.recipes.size())
			? sim_str(ws.recipes[static_cast<std::size_t>(building.recipe)].id.c_str())
			: String("");
		entry["capacity"] = building.capacity;
		entry["input_per_output"] = building.input_per_output;
		entry["capital_per_capacity"] = building.capital_per_capacity;
		entry["jobs"] = building.jobs;
		entry["wage"] = building.wage;
		entry["opex"] = building.opex;
		// F7: mistura de escolaridade das vagas (save v9).
		entry["education_mix"] = Array{building.education_mix[0],
			building.education_mix[1], building.education_mix[2]};
		entry["output_scale"] = building.output_scale;
		entry["last_utilization"] = building.last_utilization;
		entry["ramp_days"] = building.ramp_days;
		entry["strategic"] = building.strategic;
		entry["loss_days"] = building.loss_days;
		// Estado de encerramento da construção: falida por insolvência ou mina
		// com jazida esgotada. A remoção acontece no prólogo do tick seguinte;
		// sem persistir a marca, o load "ressuscita" a construção por um dia.
		entry["bankrupt"] = building.bankrupt;
		entry["depleted"] = building.depleted;
		// D-086 (save v18): estágio de mina e a fatia de reserva que ele trabalha.
		entry["stage"] = building.stage;
		entry["stage_reserve_kt"] = building.stage_reserve_kt;
		entry["stage_mined_kt"] = building.stage_mined_kt;
		// B5 (save v21): royalty/imposto de concessão (fração da receita paga
		// ao Estado). 0 fora de concessão.
		entry["royalty"] = building.royalty;
		// C1 (save v26): salário atrasado da planta (passivo da firma).
		entry["wage_arrears"] = building.wage_arrears;
		// D-125 (save v40): eficiência de produção, transição, equipamento e
		// modo da refinaria. Mundo semeado nasce maduro (1.0/9999/equipamento
		// ligado); construção nova carrega a curva baixa da classe.
		entry["production_efficiency"] = building.production_efficiency;
		entry["efficiency_days"] = building.efficiency_days;
		entry["transition_days"] = building.transition_days;
		entry["equipment"] = building.equipment;
		entry["refinery_mode"] = building.refinery_mode;
		// D-126 (save v41): estado da sobrevivência em crise.
		entry["crisis_stage"] = building.crisis_stage;
		entry["last_layoff_day"] = building.last_layoff_day;
		entry["last_credit_day"] = building.last_credit_day;
		entry["last_liquidation_day"] = building.last_liquidation_day;
		entry["mothballed"] = building.mothballed;
		entry["mothball_day"] = building.mothball_day;
		entry["mothball_unpaid_days"] = building.mothball_unpaid_days;
		entry["reopen_signal_days"] = building.reopen_signal_days;
		entry["farm_area_reduced"] = building.farm_area_reduced;
		buildings.append(entry);
	}
	data["buildings"] = buildings;

	// §15 (save v12): obras de construção em andamento. Save v11 e anteriores
	// não têm a chave: não há projeto em voo (migração trivial).
	Array construction_projects = make_array();
	for (const ConstructionProject& project : ws.construction_projects) {
		Dictionary entry = make_object();
		entry["id"] = sim_str(project.id.c_str());
		entry["building_id"] = sim_str(project.building_id.c_str());
		entry["city"] = sim_str(project.city.c_str());
		entry["owner"] = sim_str(project.owner.c_str());
		entry["group_id"] = sim_str(project.group_id.c_str());
		entry["resource"] = sim_str(project.resource.c_str());
		entry["output_product"] = project.output_product;
		entry["recipe"] = project.recipe >= 0
			&& project.recipe < static_cast<int>(ws.recipes.size())
			? sim_str(ws.recipes[static_cast<std::size_t>(project.recipe)].id.c_str())
			: String("");
		entry["input_product"] = project.input_product;
		entry["capacity"] = project.capacity;
		entry["jobs"] = project.jobs;
		entry["wage"] = project.wage;
		entry["opex"] = project.opex;
		entry["input_per_output"] = project.input_per_output;
		entry["education_mix"] = Array{project.education_mix[0],
			project.education_mix[1], project.education_mix[2]};
		entry["local_share"] = project.local_share;
		entry["partner_group"] = sim_str(project.partner_group.c_str());
		entry["strategic"] = project.strategic;
		entry["expand_existing"] = project.expand_existing;
		entry["total_cost"] = project.total_cost;
		entry["labor_cost_total"] = project.labor_cost_total;
		entry["paid"] = project.paid;
		entry["labor_paid"] = project.labor_paid;
		entry["reserved"] = project.reserved;
		entry["duration_days"] = project.duration_days;
		entry["days_elapsed"] = project.days_elapsed;
		entry["paused"] = project.paused;
		entry["pause_reason"] = sim_str(project.pause_reason.c_str());
		entry["created_day"] = project.created_day;
		// §19 (W1): obra de infraestrutura urbana (expansão/adensamento de
		// zona). Save anterior ao W1 não tem a chave: 0 = obra normal.
		entry["urban_action"] = project.urban_action;
		entry["urban_zone"] = project.urban_zone;
		entry["urban_km2"] = project.urban_km2;
		// Save v14: kind da obra na fila única (§15 unificado) + os campos da
		// moradia (§19 W3) e a parcela de TERRA paga à prefeitura.
		entry["kind"] = project.kind;
		entry["housing_density"] = project.housing_density;
		entry["housing_tier"] = project.housing_tier;
		entry["housing_owner"] = project.housing_owner;
		entry["housing_units"] = project.housing_units;
		entry["land_cost"] = project.land_cost;
		// D-086 (save v18): obra de APROFUNDAMENTO de mina (-1 = obra normal).
		entry["deepen_stage"] = project.deepen_stage;
		// B5 (save v21): tipo de infra da região (0 estrada, 1 energia, 2 água)
		// quando kind == CONSTRUCTION_REGION_INFRA; -1 = não se aplica.
		entry["infra_type"] = project.infra_type;
		// G2 (save v25): obra de armazém (tipo e capacidade a entregar).
		entry["storage_type"] = project.storage_type;
		entry["storage_capacity_t"] = project.storage_capacity_t;
		// PARTE E (save v35): obra de energia — fonte (usina) e potência (MW).
		entry["power_fuel"] = project.power_fuel;
		entry["power_mw"] = project.power_mw;
		// E5 (save v36): interconexão nova — par canônico de nações.
		entry["power_nation_a"] = sim_str(project.power_nation_a.c_str());
		entry["power_nation_b"] = sim_str(project.power_nation_b.c_str());
		Array materials = make_array();
		Array material_qty = make_array();
		Array material_delivered = make_array();
		for (int m = 0; m < project.material_count; ++m) {
			materials.append(project.materials[m]);
			material_qty.append(project.material_qty[m]);
			material_delivered.append(project.material_delivered[m]);
		}
		entry["materials"] = materials;
		entry["material_qty"] = material_qty;
		entry["material_delivered"] = material_delivered;
		construction_projects.append(entry);
	}
	data["construction_projects"] = construction_projects;

	// §19 (save v13): solo urbano, moradia e prefeitura por cidade. Vetores
	// paralelos a `cities`; arrays na ordem fixa dos enums (zonas, densidades,
	// padrões, posse). Sem set_urban (cenário sintético) a lista é vazia.
	Array urban = make_array();
	const std::size_t urban_count = std::min(
		{ws.land.size(), ws.housing.size(), ws.city_gov.size(),
			ws.cities.size()});
	for (std::size_t index = 0; index < urban_count; ++index) {
		const CityLand& land = ws.land[index];
		const CityHousing& house = ws.housing[index];
		const CityGov& gov = ws.city_gov[index];
		Dictionary entry = make_object();
		entry["city"] = sim_str(ws.cities[index].id.c_str());
		Array area = make_array();
		Array max_density = make_array();
		Array land_price = make_array();
		for (int zone = 0; zone < ZONE_COUNT; ++zone) {
			area.append(land.area_km2[zone]);
			max_density.append(static_cast<int>(land.max_density[zone]));
			land_price.append(land.land_price[zone]);
		}
		entry["area_km2"] = area;
		entry["max_density"] = max_density;
		entry["land_price"] = land_price;
		entry["reserve_km2"] = land.reserve_km2;
		entry["infra_capacity"] = land.infra_capacity;
		entry["informal_km2"] = land.informal_km2;
		// §19 (W1): demanda de zona barrada por falta de espaço (sinal para a
		// prefeitura). Save anterior ao W1 não tem a chave: zero.
		Array zone_demand = make_array();
		for (int zone = 0; zone < ZONE_COUNT; ++zone) {
			zone_demand.append(land.zone_demand[zone]);
		}
		entry["zone_demand"] = zone_demand;
		// Q-015-C: fração do envelope construída na escala da cidade. Save
		// anterior não tem a chave e volta ao comportamento antigo (1,0).
		Array zone_fill = make_array();
		for (int zone = 0; zone < ZONE_COUNT; ++zone) {
			zone_fill.append(land.zone_fill[zone]);
		}
		entry["zone_fill"] = zone_fill;
		// §19 (integração): base do custo crescente (raio e reserva no
		// carregamento). Save anterior não tem a chave: cai no campo do loader
		// (ausência explícita, fator 1 sem inventar aumento).
		entry["initial_area_km2"] = land.initial_area_km2;
		entry["initial_reserve_km2"] = land.initial_reserve_km2;
		Array units = make_array();
		for (int density = 0; density < DENSITY_COUNT; ++density) {
			Array row = make_array();
			for (int tier = 0; tier < TIER_COUNT; ++tier) {
				row.append(house.units[density][tier]);
			}
			units.append(row);
		}
		entry["units"] = units;
		entry["condition"] = house.condition;
		Array owner_share = make_array();
		for (int owner = 0; owner < OWNER_COUNT; ++owner) {
			owner_share.append(house.owner_share[owner]);
		}
		entry["owner_share"] = owner_share;
		Array tenure = make_array();
		for (int tier = 0; tier < TIER_COUNT; ++tier) {
			Array row = make_array();
			for (int kind = 0; kind < TENURE_COUNT; ++kind) {
				row.append(house.tenure[tier][kind]);
			}
			tenure.append(row);
		}
		entry["tenure"] = tenure;
		Array rent = make_array();
		Array price = make_array();
		Array vacancy = make_array();
		for (int tier = 0; tier < TIER_COUNT; ++tier) {
			rent.append(house.rent[tier]);
			price.append(house.price[tier]);
			vacancy.append(house.vacancy[tier]);
		}
		entry["rent"] = rent;
		entry["price"] = price;
		entry["vacancy"] = vacancy;
		entry["informal_units"] = house.informal_units;
		Dictionary gov_entry = make_object();
		gov_entry["cash"] = gov.cash;
		gov_entry["iptu_rate"] = gov.iptu_rate;
		gov_entry["fee_rate"] = gov.fee_rate;
		gov_entry["transfer_share"] = gov.transfer_share;
		gov_entry["month_revenue_iptu"] = gov.month_revenue_iptu;
		gov_entry["month_revenue_fees"] = gov.month_revenue_fees;
		gov_entry["month_revenue_transfer"] = gov.month_revenue_transfer;
		gov_entry["month_expense_maintenance"] = gov.month_expense_maintenance;
		gov_entry["month_expense_infrastructure"] =
			gov.month_expense_infrastructure;
		gov_entry["month_expense_social_housing"] =
			gov.month_expense_social_housing;
		entry["gov"] = gov_entry;
		urban.append(entry);
	}
	data["urban"] = urban;

	// §19 (W2): prefeitura em operação — calibração do repasse e obra urbana
	// (decisão, orçamento reservado e prazo). Bloco PRÓPRIO para não disputar o
	// dicionário `gov` do bloco `urban` com os pacotes W1/W3.
	Array city_gov_save = make_array();
	const std::size_t city_gov_count =
		std::min(ws.city_gov.size(), ws.cities.size());
	for (std::size_t index = 0; index < city_gov_count; ++index) {
		const CityGov& gov = ws.city_gov[index];
		Dictionary entry = make_object();
		entry["city"] = sim_str(ws.cities[index].id.c_str());
		entry["revenue_pct_gdp"] = gov.revenue_pct_gdp;
		entry["target_revenue_month"] = gov.target_revenue_month;
		entry["month_transfer_paid"] = gov.month_transfer_paid;
		// A6: janela do fechamento mensal desta cidade (escalonamento).
		entry["month_marker"] = gov.month_marker;
		Array projects = make_array();
		for (int slot = 0; slot < gov.project_count; ++slot) {
			const CityGovProject& project = gov.projects[slot];
			Dictionary row = make_object();
			row["kind"] = static_cast<int>(project.kind);
			row["zone"] = project.zone;
			row["area_km2"] = project.area_km2;
			row["units"] = project.units;
			row["cost"] = project.cost;
			row["reserved"] = project.reserved;
			row["days_left"] = project.days_left;
			row["created_day"] = project.created_day;
			projects.append(row);
		}
		entry["projects"] = projects;
		city_gov_save.append(entry);
	}
	data["city_gov"] = city_gov_save;
	// §19 W3 — estado do MERCADO DE MORADIA por cidade (bloco próprio, para
	// não conflitar com o bloco `urban` do W0/W1/W2). Guarda a base de
	// domicílios, o sinal de aperto e os fluxos do mês. Desde o save v14 as
	// obras em andamento NÃO moram aqui: elas são `construction_projects` com
	// kind CONSTRUCTION_HOUSING (fila única §15).
	Array housing_state = make_array();
	const std::size_t housing_count = std::min(
		ws.housing.size(), ws.cities.size());
	for (std::size_t index = 0; index < housing_count; ++index) {
		const CityHousing& house = ws.housing[index];
		Dictionary entry = make_object();
		entry["city"] = sim_str(ws.cities[index].id.c_str());
		entry["households"] = house.households;
		Array households_by_tier = make_array();
		for (int tier = 0; tier < TIER_COUNT; ++tier) {
			households_by_tier.append(house.households_by_tier[tier]);
		}
		entry["households_by_tier"] = households_by_tier;
		entry["pop_anchor"] = house.pop_anchor;
		entry["rent_anchored"] = house.rent_anchored;
		Array rent_cap_ratio = make_array();
		for (int tier = 0; tier < TIER_COUNT; ++tier) {
			rent_cap_ratio.append(house.rent_cap_ratio[tier]);
		}
		entry["rent_cap_ratio"] = rent_cap_ratio;
		entry["housing_stress"] = house.housing_stress;
		entry["stress_days"] = house.stress_days;
		entry["rent_paid_month"] = house.rent_paid_month;
		// §19 (integração): o aluguel pago NO DIA também é estado (o teste de
		// round-trip do W1 compara o retrato urbano inteiro); sem a chave o
		// load zerava o dia e o save divergia.
		entry["rent_paid_today"] = house.rent_paid_today;
		entry["rent_households_month"] = house.rent_households_month;
		entry["rent_firms_month"] = house.rent_firms_month;
		entry["rent_government_month"] = house.rent_government_month;
		entry["rent_burden"] = house.rent_burden;
		entry["units_built_month"] = house.units_built_month;
		entry["month_marker"] = house.month_marker;
		// save v14: as obras de moradia vivem em `construction_projects`
		// (kind CONSTRUCTION_HOUSING); não há mais lista própria aqui.
		housing_state.append(entry);
	}
	data["housing"] = housing_state;

	// F8: negócios de serviço (catálogo global). O nome NÃO é gravado: é
	// derivado da semente + cultura do dono no GDScript (rótulo), então o save
	// guarda só `name_seed` e o vínculo cidade/setor.
	Array service_businesses = make_array();
	for (const ServiceBusiness& business : ws.service_businesses) {
		Dictionary entry = make_object();
		entry["id"] = business.id;
		entry["sector"] = business.sector;
		entry["city"] = sim_str(business.city.c_str());
		entry["owner_stratum"] = business.owner_stratum;
		entry["capacity"] = business.capacity;
		entry["jobs"] = business.jobs;
		entry["workers"] = business.workers;
		entry["capital"] = business.capital;
		entry["capital_left"] = business.capital_left;
		entry["utilization"] = business.utilization;
		entry["revenue"] = business.revenue;
		entry["wages"] = business.wages;
		entry["input_cost"] = business.input_cost;
		entry["service_cost"] = business.service_cost;
		entry["profit"] = business.profit;
		entry["loss_days"] = business.loss_days;
		entry["bankrupt"] = business.bankrupt;
		entry["opened_day"] = business.opened_day;
		entry["name_seed"] = static_cast<double>(business.name_seed % 9007199254740991ULL);
		service_businesses.append(entry);
	}
	data["service_businesses"] = service_businesses;
	data["service_bankruptcies"] = ws.service_bankruptcies;

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

	// F5: moedas (taxa, base, regime, balança) e tarifas declaradas.
	Array currencies = make_array();
	for (const Currency& currency : ws.currencies) {
		Dictionary entry = make_object();
		entry["code"] = sim_str(currency.code.c_str());
		entry["rate_per_anchor"] = currency.rate;
		entry["base_rate"] = currency.base_rate;
		entry["floating"] = currency.floating;
		entry["fallback"] = currency.fallback;
		entry["source"] = sim_str(currency.source.c_str());
		entry["balance_ema"] = currency.balance_ema;
		entry["gross_ema"] = currency.gross_ema;
		// C2a (v27/D-103): política da moeda. Salva, sem mecânica nesta fase.
		entry["exchange_regime"] = sim_str(
			central_bank::regime_name(currency.exchange_regime));
		entry["bank_independence"] = sim_str(
			central_bank::independence_name(currency.bank_independence));
		Array nations = make_array();
		for (const std::string& nation : currency.nations) {
			nations.append(sim_str(nation.c_str()));
		}
		entry["nations"] = nations;
		currencies.append(entry);
	}
	data["currencies"] = currencies;
	Array tariffs = make_array();
	for (const Tariff& tariff : ws.tariffs) {
		Dictionary entry = make_object();
		entry["buyer"] = sim_str(tariff.buyer.c_str());
		entry["seller"] = sim_str(tariff.seller.c_str());
		entry["product"] = sim_str(tariff.product.c_str());
		entry["rate"] = tariff.rate;
		tariffs.append(entry);
	}
	data["tariffs"] = tariffs;
	// C2a (v27/D-103) — BANCO CENTRAL POR MOEDA: política do mundo, reserva de
	// ouro do BC por moeda (dado real da G2) e a série mensal do CPI. Tudo é
	// ADITIVO: save v26 carrega com fiduciário/USD (defaults declarados pelo
	// plano), ouro 0 sem declaração e série vazia — nada inventado para trás.
	{
		Dictionary policy = make_object();
		policy["standard"] = sim_str(
			central_bank::standard_name(ws.monetary_standard));
		policy["reserve_currency"] = sim_str(ws.reserve_currency.c_str());
		data["monetary_policy"] = policy;
	}
	{
		Dictionary bank = make_object();
		bank["gold_declared"] = ws.reserve_gold_declared;
		Dictionary gold = make_object();
		for (const auto& row : ws.reserve_gold_tonnes) {
			gold[sim_str(row.first.c_str())] = row.second;
		}
		bank["gold_tonnes_by_currency"] = gold;
		bank["gold_unallocated_tonnes"] = ws.reserve_gold_unallocated;
		bank["cpi_last_day"] = ws.cpi_last_observe_day;
		Dictionary cpi = make_object();
		for (const auto& row : ws.cpi_history) {
			if (row.second.count <= 0) {
				continue;
			}
			Dictionary entry = make_object();
			Array values = make_array();
			Array days = make_array();
			const int count = std::clamp(row.second.count, 0,
				central_bank::kCpiSampleSlots);
			for (int index = 0; index < count; ++index) {
				values.append(row.second.samples[index]);
				days.append(row.second.sample_day[index]);
			}
			entry["index"] = values;
			entry["days"] = days;
			cpi[sim_str(row.first.c_str())] = entry;
		}
		bank["cpi"] = cpi;
		data["central_bank"] = bank;
	}
	// C2b1 (v28/D-104) — POLÍTICA MONETÁRIA ATIVA: taxa básica por moeda, M0
	// emitido por operação contabilizada, carteira de títulos do BC e dívida
	// pública por nação. ADITIVO: v27 carrega com taxa sem decisão (resolve no
	// neutro do jogo), emissão 0 e dívida 0 — nada inventado para trás.
	{
		Dictionary block = make_object();
		Dictionary policies = make_object();
		for (const auto& row : ws.monetary) {
			Dictionary entry = make_object();
			entry["rate"] = row.second.rate;
			entry["emission"] = row.second.emission;
			entry["securities"] = row.second.securities;
			entry["last_day"] = row.second.last_day;
			policies[sim_str(row.first.c_str())] = entry;
		}
		block["policies"] = policies;
		Dictionary bonds = make_object();
		for (const auto& row : ws.gov_bonds) {
			bonds[sim_str(row.first.c_str())] = row.second;
		}
		block["gov_bonds"] = bonds;
		// C6 (D-114): títulos do resgate sem BC em poder do exterior (claim do
		// credor; pago pela posição externa). Aditivo no v34 — ausente = 0.
		Dictionary external_bonds = make_object();
		for (const auto& row : ws.external_bonds) {
			external_bonds[sim_str(row.first.c_str())] = row.second;
		}
		block["external_bonds"] = external_bonds;
		// Janela do déficit (medição que alimenta a decisão do dia seguinte):
		// sem ela, um load recomeçaria a janela no caixa do load e a
		// monetização divergiria do caminho direto.
		Dictionary cash_open = make_object();
		for (const auto& row : ws.gov_cash_open) {
			cash_open[sim_str(row.first.c_str())] = row.second;
		}
		block["gov_cash_open"] = cash_open;
		data["monetary"] = block;
	}
	// C2b2 (v29/D-106) — CÂMBIO: paridade, largura da banda, reservas em
	// moeda de reserva, preço fixo de conversão do ouro, cobertura e o anel de
	// eventos. ADITIVO: v28 carrega com paridade 0 (o `base_rate` do F5),
	// banda no default, reservas 0 (fixo administrativo) e nenhum evento.
	{
		Dictionary block = make_object();
		Dictionary currencies = make_object();
		for (const auto& row : ws.fx) {
			Dictionary entry = make_object();
			entry["parity"] = row.second.parity;
			entry["band_width"] = row.second.band_width;
			entry["reserves"] = row.second.reserves;
			entry["conversion_price"] = row.second.conversion_price;
			entry["funded"] = row.second.funded;
			entry["devaluations"] = row.second.devaluations;
			entry["last_event_day"] = row.second.last_event_day;
			// C2c (v31/D-108): composição da cesta resolvida no mundo.
			Dictionary basket = make_object();
			for (const auto& unit : row.second.basket) {
				basket[sim_str(unit.first.c_str())] = unit.second;
			}
			entry["basket"] = basket;
			entry["basket_source"] =
				sim_str(row.second.basket_source.c_str());
			currencies[sim_str(row.first.c_str())] = entry;
		}
		block["currencies"] = currencies;
		Array events = make_array();
		for (const FxEvent& row : ws.fx_events) {
			Dictionary entry = make_object();
			entry["day"] = row.day;
			entry["currency"] = sim_str(row.currency.c_str());
			entry["type"] = sim_str(row.type.c_str());
			entry["old_parity"] = row.old_parity;
			entry["new_parity"] = row.new_parity;
			entry["reserves_used"] = row.reserves_used;
			events.append(entry);
		}
		block["events"] = events;
		Dictionary params = make_object();
		params["band_width"] = ws.fx_band_width;
		params["defence_share"] = ws.fx_defence_share;
		params["gold_reserve_ratio"] = ws.fx_gold_reserve_ratio;
		block["parameters"] = params;
		data["fx"] = block;
	}
	// C2c (v31/D-108) — REGIME REAL por moeda (IMF AREAER 30/04/2026): regime,
	// âncora (moeda ou cesta), fonte, emissor e a trava de união monetária.
	// ADITIVO: v30 carrega os regimes do arquivo pelo `set_fx_regimes`.
	{
		Dictionary block = make_object();
		block["source"] = sim_str(ws.fx_regime_source.c_str());
		Array no_policy = make_array();
		for (const std::string& nation : ws.fx_no_policy_nations) {
			no_policy.append(sim_str(nation.c_str()));
		}
		block["no_policy"] = no_policy;
		Dictionary countries = make_object();
		for (const auto& row : ws.fx_regime) {
			Dictionary entry = make_object();
			entry["regime"] =
				sim_str(central_bank::regime_name(row.second.regime));
			entry["anchor"] = sim_str(row.second.anchor.c_str());
			entry["anchor_type"] = sim_str(row.second.anchor_type.c_str());
			entry["anchor_source"] =
				sim_str(row.second.anchor_source.c_str());
			entry["issuer"] = sim_str(row.second.issuer.c_str());
			entry["locked"] = row.second.locked;
			entry["source"] = sim_str(row.second.source.c_str());
			Dictionary basket = make_object();
			for (const auto& unit : row.second.basket) {
				basket[sim_str(unit.first.c_str())] = unit.second;
			}
			entry["basket"] = basket;
			countries[sim_str(row.first.c_str())] = entry;
		}
		block["currencies"] = countries;
		data["fx_regimes"] = block;
	}
	// C3 (v30/D-107) — CRÉDITO DE EMPRESA: banco agregado por nação e o
	// estado do crédito. ADITIVO: v29 carrega desligado, sem banco e com
	// dívida 0 (nada inventado para trás).
	{
		Dictionary block = make_object();
		block["enabled"] = ws.bank_credit_enabled;
		Dictionary rows = make_object();
		for (const auto& row : ws.banks) {
			const Bank& bank = row.second;
			Dictionary entry = make_object();
			entry["id"] = sim_str(bank.id.c_str());
			entry["nation"] = sim_str(bank.nation.c_str());
			entry["role"] = sim_str(bank::role_key(bank.role));
			entry["name"] = sim_str(bank.name.c_str());
			entry["name_status"] = sim_str(bank.name_status.c_str());
			entry["owner_kind"] = sim_str(bank.owner_kind.c_str());
			entry["owner_nation"] = sim_str(bank.owner_nation.c_str());
			entry["share"] = bank.share;
			entry["currency"] = sim_str(bank.currency.c_str());
			entry["loans"] = bank.loans;
			entry["arrears"] = bank.arrears;
			entry["deposits"] = bank.deposits;
			entry["bc_borrowing"] = bank.bc_borrowing;
			entry["cash"] = bank.cash;
			entry["capital"] = bank.capital;
			entry["interest_income"] = bank.interest_income;
			entry["interest_expense"] = bank.interest_expense;
			entry["writeoffs"] = bank.writeoffs;
			entry["dividends"] = bank.dividends;
			entry["remittance"] = bank.remittance;
			entry["remittance_origin"] = bank.remittance_origin;
			entry["harvest_interest"] = bank.harvest_interest;
			entry["defaults"] = bank.defaults;
			entry["loans_count"] = bank.loans_count;
			entry["last_default_day"] = bank.last_default_day;
			entry["last_dividend_day"] = bank.last_dividend_day;
			// C6 (D-114): estado de crise por banco (confianca, corrida,
			// redesconto, seguro, bail-in, resolucao).
			entry["confidence"] = bank.confidence;
			entry["loss_seeded"] = bank.loss_seeded;
			entry["loss_ema"] = bank.loss_ema;
			// Perda reconhecida HOJE: sem ela, um load no meio do dia perdia o
			// choque que ainda não entrou na EMA e a continuação divergia.
			entry["loss_today"] = bank.loss_today;
			entry["run_days"] = bank.run_days;
			entry["last_run_day"] = bank.last_run_day;
			entry["run_withdrawn"] = bank.run_withdrawn;
			entry["run_cash"] = bank.run_cash;
			entry["run_public"] = bank.run_public;
			entry["lolr_borrowed"] = bank.lolr_borrowed;
			entry["lolr_interest"] = bank.lolr_interest;
			entry["lolr_days"] = bank.lolr_days;
			entry["insured_payout"] = bank.insured_payout;
			entry["bailin_cut"] = bank.bailin_cut;
			entry["treasury_rescue"] = bank.treasury_rescue;
			entry["capital_flight"] = bank.capital_flight;
			entry["resolution_day"] = bank.resolution_day;
			entry["resolved"] = bank.resolved;
			entry["resolution"] = sim_str(bank.resolution.c_str());
			entry["absorbed_into"] = sim_str(bank.absorbed_into.c_str());
			entry["baseline_seeded"] = bank.baseline_seeded;
			entry["open_capital_ratio"] = bank.open_capital_ratio;
			entry["open_liquidity_ratio"] = bank.open_liquidity_ratio;
			entry["last_capital_ratio"] = bank.last_capital_ratio;
			entry["last_liquidity_ratio"] = bank.last_liquidity_ratio;
			entry["last_npl_ratio"] = bank.last_npl_ratio;
			rows[sim_str(row.first.c_str())] = entry;
		}
		block["banks"] = rows;
		data["banks"] = block;
	}
	// C6 (v34/D-114) - CRISE BANCARIA: regra, fundo de seguro por nacao e o
	// anel de eventos. ADITIVO: v33 e anteriores chegam sem o bloco e a
	// migracao (`bank_crisis::after_load`) inicia os bancos calmos.
	{
		Dictionary block = make_object();
		block["enabled"] = ws.bank_crisis.enabled;
		Dictionary policy = make_object();
		policy["capital_min"] = ws.bank_crisis.capital_min;
		policy["capital_cut_floor"] = ws.bank_crisis.capital_cut_floor;
		policy["liquidity_min"] = ws.bank_crisis.liquidity_min;
		policy["loss_scale"] = ws.bank_crisis.loss_scale;
		policy["loss_ema_days"] = ws.bank_crisis.loss_ema_days;
		policy["confidence_start"] = ws.bank_crisis.confidence_start;
		policy["smoothing"] = ws.bank_crisis.smoothing;
		policy["w_solvency"] = ws.bank_crisis.w_solvency;
		policy["w_liquidity"] = ws.bank_crisis.w_liquidity;
		policy["w_losses"] = ws.bank_crisis.w_losses;
		policy["w_contagion"] = ws.bank_crisis.w_contagion;
		policy["w_market"] = ws.bank_crisis.w_market;
		policy["run_threshold"] = ws.bank_crisis.run_threshold;
		policy["run_share"] = ws.bank_crisis.run_share;
		policy["flight_to_quality"] = ws.bank_crisis.flight_to_quality;
		policy["lolr_spread"] = ws.bank_crisis.lolr_spread;
		policy["lolr_haircut"] = ws.bank_crisis.lolr_haircut;
		policy["lolr_max_assets"] = ws.bank_crisis.lolr_max_assets;
		policy["insured_share"] = ws.bank_crisis.insured_share;
		policy["premium_daily"] = ws.bank_crisis.premium_daily;
		policy["fund_target_ratio"] = ws.bank_crisis.fund_target_ratio;
		policy["capital_flight_share"] =
			ws.bank_crisis.capital_flight_share;
		policy["insolvency_tolerance"] =
			ws.bank_crisis.insolvency_tolerance;
		policy["insolvency_tolerance_ratio"] =
			ws.bank_crisis.insolvency_tolerance_ratio;
		policy["treasury_debt_rescue"] =
			ws.bank_crisis.treasury_debt_rescue;
		policy["takeover_public"] = ws.bank_crisis.takeover_public;
		block["policy"] = policy;
		Dictionary funds = make_object();
		for (const auto& fund_row : ws.deposit_insurance) {
			Dictionary entry = make_object();
			entry["fund"] = fund_row.second.fund;
			entry["premiums"] = fund_row.second.premiums;
			entry["payouts"] = fund_row.second.payouts;
			entry["last_payout_day"] = fund_row.second.last_payout_day;
			funds[sim_str(fund_row.first.c_str())] = entry;
		}
		block["funds"] = funds;
		Array events = make_array();
		for (const BankCrisisEvent& event : ws.bank_crisis_events) {
			Dictionary entry = make_object();
			entry["day"] = event.day;
			entry["bank"] = sim_str(event.bank.c_str());
			entry["nation"] = sim_str(event.nation.c_str());
			entry["kind"] = sim_str(event.kind.c_str());
			entry["cause"] = sim_str(event.cause.c_str());
			entry["gap"] = event.gap;
			entry["cost_shareholders"] = event.cost_shareholders;
			entry["cost_depositors"] = event.cost_depositors;
			entry["cost_fund"] = event.cost_fund;
			entry["cost_treasury"] = event.cost_treasury;
			entry["cost_takeover"] = event.cost_takeover;
			entry["capital_flight"] = event.capital_flight;
			entry["lolr"] = event.lolr;
			entry["emission"] = event.emission;
			events.append(entry);
		}
		block["events"] = events;
		block["events_total"] = ws.bank_crisis_events_total;
		data["bank_crisis"] = block;
	}
	// C5 (v33/D-112) - BOLSA: uma praça por nação e um papel por grupo
	// listado. ADITIVO: v32 e anteriores chegam sem o bloco e recebem a bolsa
	// criada na migração (preço = patrimônio medido; nada inventado).
	{
		Dictionary block = make_object();
		block["enabled"] = ws.stock_enabled;
		Dictionary policy = make_object();
		policy["investment_channel"] = ws.stock_investment_channel;
		policy["risk_premium"] = ws.stock_risk_premium;
		policy["payout_ratio"] = ws.stock_payout_ratio;
		policy["step"] = ws.stock_step;
		policy["expectation_k"] = ws.stock_expectation_k;
		block["policy"] = policy;
		Dictionary exchanges = make_object();
		for (const auto& row : ws.stock_exchanges) {
			const StockExchange& exchange = row.second;
			Dictionary entry = make_object();
			entry["nation"] = sim_str(exchange.nation.c_str());
			entry["name"] = sim_str(exchange.name.c_str());
			entry["name_status"] = sim_str(exchange.name_status.c_str());
			entry["currency"] = sim_str(exchange.currency.c_str());
			entry["confidence"] = exchange.confidence;
			entry["index_base"] = exchange.index_base;
			entry["last_price_day"] = exchange.last_price_day;
			entry["production_ema"] = exchange.production_ema;
			entry["production_seeded"] = exchange.production_seeded;
			entry["dividends_paid"] = exchange.dividends_paid;
			entry["remittance"] = exchange.remittance;
			entry["remittance_origin"] = exchange.remittance_origin;
			exchanges[sim_str(row.first.c_str())] = entry;
		}
		block["exchanges"] = exchanges;
		Dictionary tickers = make_object();
		for (const auto& row : ws.stock_tickers) {
			const StockTicker& ticker = row.second;
			Dictionary entry = make_object();
			entry["group_id"] = sim_str(ticker.group_id.c_str());
			entry["nation"] = sim_str(ticker.nation.c_str());
			entry["exchange"] = sim_str(ticker.exchange.c_str());
			entry["shares"] = ticker.shares;
			entry["price"] = ticker.price;
			entry["book_equity"] = ticker.book_equity;
			entry["profit_ema"] = ticker.profit_ema;
			entry["profit_at_last_dividend"] =
				ticker.profit_at_last_dividend;
			entry["dividend_last"] = ticker.dividend_last;
			entry["dividend_limited"] = ticker.dividend_limited;
			entry["dividend_day"] = ticker.dividend_day;
			entry["listed_day"] = ticker.listed_day;
			entry["delisted_day"] = ticker.delisted_day;
			entry["listed"] = ticker.listed;
			entry["reason"] = sim_str(ticker.reason.c_str());
			Dictionary holdings = make_object();
			holdings["households_high"] = ticker.holdings.households_high;
			Dictionary banks = make_object();
			for (const auto& bank_row : ticker.holdings.banks) {
				banks[sim_str(bank_row.first.c_str())] = bank_row.second;
			}
			holdings["banks"] = banks;
			Dictionary groups = make_object();
			for (const auto& group_row : ticker.holdings.groups) {
				groups[sim_str(group_row.first.c_str())] = group_row.second;
			}
			holdings["groups"] = groups;
			holdings["foreign"] = ticker.holdings.foreign;
			entry["holdings"] = holdings;
			tickers[sim_str(row.first.c_str())] = entry;
		}
		block["tickers"] = tickers;
		data["stock"] = block;
	}
	// PARTE E (v35/D-116) - ENERGIA: caixa do setor, regra, estado por cidade,
	// usinas e a rede por nação. ADITIVO: v34 e anteriores chegam sem o bloco
	// (ver a carga) e o PowerService semeia o parque do dado público. Um mundo
	// SEM energia semeada não grava o bloco: salvar→carregar→salvar tem de dar
	// o mesmo hash (sem estado "isolado" inventado no load).
	if (ws.power_ready) {
		Dictionary block = make_object();
		block["cash"] = ws.power_cash;
		block["ready"] = ws.power_ready;
		Dictionary policy = make_object();
		std::map<std::string, double> param_map;
		power_params_to_map(ws.power_params, param_map);
		for (const auto& row : param_map) {
			policy[sim_str(row.first.c_str())] = row.second;
		}
		block["policy"] = policy;
		Array power_cities = make_array();
		const std::size_t power_count = std::min(ws.power_cities.size(),
			ws.cities.size());
		for (std::size_t index = 0; index < power_count; ++index) {
			const PowerCityState& state = ws.power_cities[index];
			Dictionary entry = make_object();
			entry["city"] = sim_str(ws.cities[index].id.c_str());
			entry["connection"] = state.connection;
			entry["line_mw"] = state.line_mw;
			entry["has_data"] = state.has_data;
			entry["access_pct"] = state.access_pct;
			entry["loss_pct"] = state.loss_pct;
			Array cap = make_array();
			Array solar = make_array();
			for (int fuel = 0; fuel < kPowerFuelCount; ++fuel) {
				cap.append(state.cap_mw[fuel]);
			}
			for (int month = 0; month < kPowerMonthCount; ++month) {
				solar.append(state.solar_month[month]);
			}
			entry["cap_mw"] = cap;
			entry["solar_month"] = solar;
			// D-122 (v38): vento do sítio para a obra nova render local.
			entry["wind_ms"] = state.wind_ms;
			// Ancoragem nacional (D-116): a base Ember, a renda/perda do ano-base
			// e a calibração do CF por fonte precisam sobreviver ao load; sem
			// eles a demanda voltaria à curva global no dia seguinte.
			entry["demand_base_mwh"] = state.demand_base_mwh;
			entry["demand_income0"] = state.demand_income0;
			entry["demand_loss0"] = state.demand_loss0;
			Array cf_scale = make_array();
			for (int fuel = 0; fuel < kPowerFuelCount; ++fuel) {
				cf_scale.append(state.cf_scale[fuel]);
			}
			entry["cf_scale"] = cf_scale;
			entry["demand_mwh"] = state.demand_mwh;
			entry["served_mwh"] = state.served_mwh;
			entry["price"] = state.price;
			// E7 (v37): atacado médio, tarifa de varejo e repasse de escassez.
			entry["average_cost"] = state.average_cost;
			entry["tariff"] = state.tariff;
			entry["scarcity_pass"] = state.scarcity_pass;
			entry["ration_days"] = state.ration_days;
			entry["lost_gdp"] = state.lost_gdp;
			// Efeitos do dia publicados pela cidade (fator industrial/serviço,
			// insatisfação e conta da indústria): sem eles o round-trip do
			// status divergiria até o próximo tick.
			const City& saved_city = ws.cities[index];
			entry["stress"] = saved_city.power_stress;
			entry["industry_factor"] = saved_city.power_industry_factor;
			entry["service_factor"] = saved_city.power_service_factor;
			entry["served_ratio"] = saved_city.power_served_ratio;
			entry["power_price"] = saved_city.power_price;
			entry["power_cost_industry"] =
				saved_city.power_cost_industry;
			entry["power_cost_per_value"] =
				saved_city.power_cost_per_value;
			power_cities.append(entry);
		}
		block["cities"] = power_cities;
		Array plants = make_array();
		for (const PowerPlant& plant : ws.power_plants) {
			Dictionary entry = make_object();
			entry["id"] = sim_str(plant.id.c_str());
			entry["city_index"] = plant.city_index;
			entry["fuel"] = plant.fuel;
			entry["mw"] = plant.mw;
			entry["owner_group"] = sim_str(plant.owner_group.c_str());
			entry["owner_nation"] = sim_str(plant.owner_nation.c_str());
			entry["capex"] = plant.capex;
			entry["opex_per_mwh"] = plant.opex_per_mwh;
			entry["jobs"] = plant.jobs;
			entry["wage"] = plant.wage;
			entry["built_day"] = plant.built_day;
			entry["initial"] = plant.initial;
			// D-122 (v38): CF de sítio da usina nova (0 = parque do ano-base).
			entry["cf_base"] = plant.cf_base;
			// Geração do dia (MWh): o status publica esse valor até o próximo
			// despacho; o save o preserva como os agregados da rede já fazem.
			entry["generated_mwh"] = plant.generated_mwh;
			// D-126 (v41): crise da usina e eficiência de despacho.
			entry["loss_days"] = plant.loss_days;
			entry["mothballed"] = plant.mothballed;
			entry["mothball_day"] = plant.mothball_day;
			entry["mothball_unpaid_days"] = plant.mothball_unpaid_days;
			entry["crisis_stage"] = plant.crisis_stage;
			entry["last_layoff_day"] = plant.last_layoff_day;
			entry["reopen_signal_days"] = plant.reopen_signal_days;
			entry["production_efficiency"] = plant.production_efficiency;
			plants.append(entry);
		}
		block["plants"] = plants;
		// std::map ordenado: a ordem do array é estável (round-trip byte a byte).
		Dictionary grids = make_object();
		for (const auto& row : ws.power_grids) {
			const PowerGridState& grid = row.second;
			Dictionary entry = make_object();
			entry["nation"] = sim_str(grid.nation.c_str());
			entry["exists"] = grid.exists;
			entry["price"] = grid.price;
			// E7 (v37): atacado médio, tarifa, repasse e margem do dia.
			entry["average_cost"] = grid.average_cost;
			entry["tariff"] = grid.tariff;
			entry["scarcity_pass"] = grid.scarcity_pass;
			entry["retail_margin"] = grid.retail_margin;
			entry["demand_mwh"] = grid.demand_mwh;
			entry["served_mwh"] = grid.served_mwh;
			entry["ration_days"] = grid.ration_days;
			entry["lost_gdp"] = grid.lost_gdp;
			entry["last_build_day"] = grid.last_build_day;
			Array by_fuel = make_array();
			for (int fuel = 0; fuel < kPowerFuelCount; ++fuel) {
				by_fuel.append(grid.generation_by_fuel[fuel]);
			}
			entry["generation_by_fuel"] = by_fuel;
			// E5 (save v36): ledger do comércio do dia.
			entry["import_mwh"] = grid.import_mwh;
			entry["export_mwh"] = grid.export_mwh;
			entry["import_value"] = grid.import_value;
			entry["export_value"] = grid.export_value;
			entry["interconnect_count"] = grid.interconnect_count;
			// D-122 (v38): curtailment do dia (MWh) para o status/round-trip.
			entry["curtailment_mwh"] = grid.curtailment_mwh;
			grids[sim_str(row.first.c_str())] = entry;
		}
		block["grids"] = grids;
		// E5 (save v36): interconexões (ano-base + obras). O vetor já está em
		// ordem determinística; o load remonta o estado e o mesmo round-trip.
		block["interconnects_ready"] = ws.power_interconnects_ready;
		Array interconnects = make_array();
		for (const InterconnectState& line : ws.interconnects) {
			Dictionary entry = make_object();
			entry["a"] = sim_str(line.a.c_str());
			entry["b"] = sim_str(line.b.c_str());
			entry["capacity_mw"] = line.capacity_mw;
			entry["built_day"] = line.built_day;
			entry["initial"] = line.initial;
			// E7 (v37): contrato de suprimento do ano-base.
			entry["contract_importer"] = sim_str(
				line.contract_importer.c_str());
			entry["contract_exporter"] = sim_str(
				line.contract_exporter.c_str());
			entry["contract_mw"] = line.contract_mw;
			entry["contract_flow_mwh"] = line.contract_flow_mwh;
			entry["contract_sent_mwh"] = line.contract_sent_mwh;
			entry["contract_price"] = line.contract_price;
			entry["contract_value"] = line.contract_value;
			entry["importer_today"] = sim_str(line.importer_today.c_str());
			entry["exporter_today"] = sim_str(line.exporter_today.c_str());
			entry["flow_mwh"] = line.flow_mwh;
			entry["sent_mwh"] = line.sent_mwh;
			entry["price"] = line.price;
			entry["value"] = line.value;
			interconnects.append(entry);
		}
		block["interconnects"] = interconnects;
		data["power"] = block;
	}
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
	return data;
}

// Restaura o estado do cenário a partir de to_dict() (load + migrações).
void from_value(WorldState& ws, SimCore& core, const Dictionary& data) {
	// Recusa save de versão futura (maior que a que este binário conhece).
	const int version = int(data.get("version", 0));
	if (version > kSaveVersion) {
		ws.active = false;
		return;
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

	// F5: moedas e tarifas (save v6). Carregadas ANTES das cidades porque o
	// preço-base local (moeda da nação) depende da taxa. Save v5 sem as chaves
	// mantém o que já está carregado pelo autoload (mundo em XGS se não houver).
	if (data.has("currencies")) {
		const Array saved_currencies = data.get("currencies", Array());
		ws.currencies.clear();
		ws.currencies.reserve(static_cast<std::size_t>(saved_currencies.size()));
		for (int index = 0; index < saved_currencies.size(); ++index) {
			const Dictionary entry = saved_currencies[index];
			const String code = String(entry.get("code", ""));
			if (code.is_empty()) {
				continue;
			}
			Currency currency;
			currency.code = std::string(code.utf8().get_data());
			currency.rate = double(entry.get("rate_per_anchor", 1.0));
			if (!std::isfinite(currency.rate) || currency.rate <= 0.0) {
				currency.rate = 1.0;
			}
			currency.base_rate = double(entry.get("base_rate", currency.rate));
			if (!std::isfinite(currency.base_rate) || currency.base_rate <= 0.0) {
				currency.base_rate = currency.rate;
			}
			currency.floating = bool(entry.get("floating", true));
			currency.fallback = bool(entry.get("fallback", false));
			currency.source = std::string(
				String(entry.get("source", "default")).utf8().get_data());
			currency.balance_ema = double(entry.get("balance_ema", 0.0));
			currency.gross_ema = double(entry.get("gross_ema", 0.0));
			// C2a (v27): política da moeda. Save v26 e anteriores NÃO têm as
			// chaves — o regime nasce do `floating` que já existia (peg é o
			// default do dado) e a independência nasce `no_data` (ausência
			// declarada; nada é inventado para trás).
			currency.exchange_regime = central_bank::regime_from_name(
				std::string(String(entry.get("exchange_regime", ""))
					.utf8().get_data()),
				currency.floating ? central_bank::kRegimeFloating
					: central_bank::kRegimePeg);
			currency.bank_independence = central_bank::independence_from_name(
				std::string(String(entry.get("bank_independence", ""))
					.utf8().get_data()),
				central_bank::kIndependenceNoData);
			const Variant nations_variant = entry.get("nations", Array());
			if (nations_variant.get_type() == Variant::ARRAY) {
				const Array nations = nations_variant;
				for (int n = 0; n < nations.size(); ++n) {
					const std::string nation(
						nations[n].operator String().utf8().get_data());
					if (!nation.empty()) {
						currency.nations.push_back(nation);
					}
				}
			}
			ws.currencies.push_back(std::move(currency));
		}
		core.rebuild_currency_indexes();
	}
	// C2a (v27/D-103) — banco central por moeda: política do mundo, reserva de
	// ouro e série do CPI. Save v26 e anteriores chegam SEM os blocos: o
	// estado volta aos defaults DECLARADOS (fiduciário, moeda de reserva
	// americana — padrões do plano) com ouro 0/sem declaração e série vazia.
	ws.monetary_standard = central_bank::kStandardFiat;
	ws.reserve_currency = "USD";
	ws.reserve_gold_tonnes.clear();
	ws.reserve_gold_declared = false;
	ws.reserve_gold_unallocated = 0.0;
	ws.cpi_history.clear();
	ws.cpi_last_observe_day = -1;
	if (data.has("monetary_policy")) {
		const Dictionary policy = data.get("monetary_policy", Dictionary());
		ws.monetary_standard = central_bank::standard_from_name(
			std::string(String(policy.get("standard", "fiat"))
				.utf8().get_data()),
			central_bank::kStandardFiat);
		const String reserve = String(policy.get("reserve_currency", "USD"));
		if (!reserve.is_empty()) {
			ws.reserve_currency = std::string(reserve.utf8().get_data());
		}
	}
	if (data.has("central_bank")) {
		const Dictionary bank = data.get("central_bank", Dictionary());
		ws.reserve_gold_declared = bool(bank.get("gold_declared", false));
		const Variant gold_variant = bank.get("gold_tonnes_by_currency",
			Dictionary());
		if (gold_variant.get_type() == Variant::DICTIONARY) {
			const Dictionary& gold = gold_variant;
			for (const auto& row : gold.entries()) {
				if (row.first.empty()) {
					continue;
				}
				const double tonnes = double(row.second);
				if (!std::isfinite(tonnes) || tonnes <= 0.0) {
					continue;
				}
				ws.reserve_gold_tonnes[row.first] = tonnes;
			}
		}
		ws.reserve_gold_unallocated = std::max(0.0,
			double(bank.get("gold_unallocated_tonnes", 0.0)));
		ws.cpi_last_observe_day = int(bank.get("cpi_last_day", -1));
		const Variant cpi_variant = bank.get("cpi", Dictionary());
		if (cpi_variant.get_type() == Variant::DICTIONARY) {
			const Dictionary& cpi = cpi_variant;
			for (const auto& row_entry : cpi.entries()) {
				if (row_entry.first.empty()
						|| row_entry.second.get_type()
							!= Variant::DICTIONARY) {
					continue;
				}
				const Dictionary& row = row_entry.second;
				const Variant values_variant = row.get("index", Array());
				const Variant days_variant = row.get("days", Array());
				if (values_variant.get_type() != Variant::ARRAY) {
					continue;
				}
				const Array& values = values_variant;
				const Array days = days_variant.get_type() == Variant::ARRAY
					? Array(days_variant) : Value::make_array();
				CpiHistory history;
				for (int index = 0;
						index < values.size()
						&& index < central_bank::kCpiSampleSlots; ++index) {
					const double value = double(values[index]);
					if (!std::isfinite(value) || value <= 0.0) {
						continue;
					}
					const int slot = history.count;
					history.samples[slot] = value;
					history.sample_day[slot] = index < days.size()
						? int(days[index]) : 0;
					history.count = slot + 1;
				}
				if (history.count > 0) {
					ws.cpi_history[row_entry.first] = history;
				}
			}
		}
	}
	// C2b1 (v28/D-104) — política monetária ativa. Save v27 e anteriores
	// chegam SEM o bloco: taxa sem decisão (`rate < 0` → neutro do jogo),
	// emissão/carteira/dívida 0 e a emissão do dia 0 (só o dia conta).
	ws.monetary.clear();
	ws.gov_bonds.clear();
	ws.external_bonds.clear();
	ws.gov_cash_open.clear();
	ws.emission_today = 0.0;
	if (data.has("monetary")) {
		const Dictionary block = data.get("monetary", Dictionary());
		const Variant policies_variant = block.get("policies", Dictionary());
		if (policies_variant.get_type() == Variant::DICTIONARY) {
			const Dictionary& policies = policies_variant;
			for (const auto& row_entry : policies.entries()) {
				if (row_entry.first.empty()
						|| row_entry.second.get_type()
							!= Variant::DICTIONARY) {
					continue;
				}
				const Dictionary& row = row_entry.second;
				MonetaryState state;
				const double rate = double(row.get("rate", -1.0));
				state.rate = std::isfinite(rate) ? rate : -1.0;
				const double emission = double(row.get("emission", 0.0));
				state.emission = std::isfinite(emission)
					? std::max(0.0, emission) : 0.0;
				const double securities = double(row.get("securities", 0.0));
				state.securities = std::isfinite(securities)
					? std::max(0.0, securities) : 0.0;
				state.last_day = int(row.get("last_day", -1));
				ws.monetary[row_entry.first] = state;
			}
		}
		const Variant bonds_variant = block.get("gov_bonds", Dictionary());
		if (bonds_variant.get_type() == Variant::DICTIONARY) {
			const Dictionary& bonds = bonds_variant;
			for (const auto& row : bonds.entries()) {
				if (row.first.empty()) {
					continue;
				}
				const double value = double(row.second);
				if (std::isfinite(value) && value > 0.0) {
					ws.gov_bonds[row.first] = value;
				}
			}
		}
		const Variant open_variant = block.get("gov_cash_open", Dictionary());
		if (open_variant.get_type() == Variant::DICTIONARY) {
			const Dictionary& cash = open_variant;
			for (const auto& row : cash.entries()) {
				if (row.first.empty()) {
					continue;
				}
				const double value = double(row.second);
				if (std::isfinite(value)) {
					ws.gov_cash_open[row.first] = value;
				}
			}
		}
		// C6 (D-114): títulos do resgate sem BC em poder do exterior. v33 e
		// anteriores chegam sem a chave: mapa vazio (ausência declarada).
		const Variant external_variant =
			block.get("external_bonds", Dictionary());
		if (external_variant.get_type() == Variant::DICTIONARY) {
			const Dictionary& external = external_variant;
			for (const auto& row : external.entries()) {
				if (row.first.empty()) {
					continue;
				}
				const double value = double(row.second);
				if (std::isfinite(value) && value > 0.0) {
					ws.external_bonds[row.first] = value;
				}
			}
		}
	}
	// C2b2 (v29/D-106) — CÂMBIO. Save v28 e anteriores chegam SEM o bloco:
	// paridade 0 (o `base_rate` do F5), banda no default de jogo, reservas 0
	// (fixo administrativo, como o F5), sem eventos e sem desvalorização.
	ws.fx.clear();
	ws.fx_events.clear();
	ws.fx_band_width = 0.0;
	ws.fx_defence_share = 0.0;
	ws.fx_gold_reserve_ratio = 0.0;
	if (data.has("fx")) {
		const Dictionary block = data.get("fx", Dictionary());
		const Variant currencies_variant = block.get("currencies", Dictionary());
		if (currencies_variant.get_type() == Variant::DICTIONARY) {
			const Dictionary& currencies = currencies_variant;
			for (const auto& row_entry : currencies.entries()) {
				if (row_entry.first.empty()
						|| row_entry.second.get_type()
							!= Variant::DICTIONARY) {
					continue;
				}
				const Dictionary& row = row_entry.second;
				FxState state;
				state.parity = std::max(0.0,
					double(row.get("parity", 0.0)));
				state.band_width = std::clamp(
					double(row.get("band_width", -1.0)), -1.0, 1.0);
				state.reserves = std::max(0.0,
					double(row.get("reserves", 0.0)));
				state.conversion_price = std::max(0.0,
					double(row.get("conversion_price", 0.0)));
				state.funded = bool(row.get("funded", false));
				state.devaluations = std::max(0,
					int(row.get("devaluations", 0)));
				state.last_event_day = int(row.get("last_event_day", -1));
				// C2c (v31/D-108): cesta resolvida (v30 chega vazia e o
				// `resolve_baskets` a preenche pelo comércio medido).
				const Variant basket_variant = row.get("basket", Variant());
				if (basket_variant.get_type() == Variant::DICTIONARY) {
					const Dictionary basket = basket_variant;
					for (const auto& unit : basket.entries()) {
						const std::string code(
							String(unit.first).utf8().get_data());
						const double weight = double(unit.second);
						if (!code.empty() && std::isfinite(weight)
								&& weight > 0.0) {
							state.basket[code] = weight;
						}
					}
				}
				state.basket_source = std::string(
					String(row.get("basket_source", "")).utf8().get_data());
				ws.fx[row_entry.first] = state;
			}
		}
		const Variant events_variant = block.get("events", Array());
		if (events_variant.get_type() == Variant::ARRAY) {
			const Array& events = events_variant;
			for (int index = 0; index < events.size(); ++index) {
				if (events[index].get_type() != Variant::DICTIONARY) {
					continue;
				}
				const Dictionary entry = events[index];
				FxEvent row;
				row.day = int(entry.get("day", -1));
				row.currency = std::string(
					String(entry.get("currency", "")).utf8().get_data());
				row.type = std::string(
					String(entry.get("type", "")).utf8().get_data());
				row.old_parity = double(entry.get("old_parity", 0.0));
				row.new_parity = double(entry.get("new_parity", 0.0));
				row.reserves_used = double(entry.get("reserves_used", 0.0));
				if (row.currency.empty()) {
					continue;
				}
				ws.fx_events.push_back(std::move(row));
			}
			if (static_cast<int>(ws.fx_events.size()) > 64) {
				ws.fx_events.erase(ws.fx_events.begin(),
					ws.fx_events.end() - 64);
			}
		}
		const Variant params_variant = block.get("parameters", Dictionary());
		if (params_variant.get_type() == Variant::DICTIONARY) {
			const Dictionary& params = params_variant;
			ws.fx_band_width = std::max(0.0,
				double(params.get("band_width", 0.0)));
			ws.fx_defence_share = std::max(0.0,
				double(params.get("defence_share", 0.0)));
			ws.fx_gold_reserve_ratio = std::max(0.0,
				double(params.get("gold_reserve_ratio", 0.0)));
		}
	}
	// C3 (v30/D-107) — CRÉDITO. Save v29 e anteriores chegam SEM o bloco:
	// desligado, sem banco e com dívida 0 (a migração não inventa credor).
	ws.banks.clear();
	ws.bank_credit_enabled = false;
	if (data.has("banks")) {
		const Dictionary block = data.get("banks", Dictionary());
		ws.bank_credit_enabled = bool(block.get("enabled", false));
		// v32 (C4/D-110): um registro por banco (`banks`), com papel e dono.
		const Value& rows_variant = block.get("banks", Value());
		const Value& nations_variant = block.get("nations", Value());
		if (rows_variant.get_type() == Variant::DICTIONARY
				&& !Dictionary(rows_variant).is_empty()) {
			const Dictionary& rows = rows_variant;
			for (const auto& row_entry : rows.entries()) {
				if (row_entry.first.empty()
						|| row_entry.second.get_type()
							!= Variant::DICTIONARY) {
					continue;
				}
				const Dictionary& row = row_entry.second;
				Bank bank;
				bank.id = row_entry.first;
				bank.nation = std::string(String(row.get("nation", ""))
					.utf8().get_data());
				if (bank.nation.empty()) {
					const std::size_t slash = bank.id.find('/');
					bank.nation = slash == std::string::npos ? bank.id
						: bank.id.substr(0, slash);
				}
				int role = bank::role_from_key(std::string(
					String(row.get("role", "")).utf8().get_data()));
				if (role < 0) {
					const std::size_t slash = bank.id.find('/');
					role = slash == std::string::npos ? -1
						: bank::role_from_key(bank.id.substr(slash + 1));
				}
				bank.role = role < 0 ? 1 : role;
				bank.name = std::string(String(row.get("name", ""))
					.utf8().get_data());
				bank.name_status = std::string(
					String(row.get("name_status", "no_data"))
						.utf8().get_data());
				bank.owner_kind = std::string(
					String(row.get("owner_kind", "national"))
						.utf8().get_data());
				bank.owner_nation = std::string(
					String(row.get("owner_nation", "")).utf8().get_data());
				bank.share = std::max(0.0, double(row.get("share", 0.0)));
				bank.currency = std::string(
					String(row.get("currency", "")).utf8().get_data());
				bank.loans = std::max(0.0, double(row.get("loans", 0.0)));
				bank.arrears = std::max(0.0, double(row.get("arrears", 0.0)));
				bank.deposits = std::max(0.0,
					double(row.get("deposits", 0.0)));
				bank.bc_borrowing = std::max(0.0,
					double(row.get("bc_borrowing", 0.0)));
				bank.cash = std::max(0.0, double(row.get("cash", 0.0)));
				bank.capital = std::max(0.0, double(row.get("capital", 0.0)));
				bank.interest_income = std::max(0.0,
					double(row.get("interest_income", 0.0)));
				bank.interest_expense = std::max(0.0,
					double(row.get("interest_expense", 0.0)));
				bank.writeoffs = std::max(0.0,
					double(row.get("writeoffs", 0.0)));
				bank.dividends = std::max(0.0,
					double(row.get("dividends", 0.0)));
				bank.remittance = std::max(0.0,
					double(row.get("remittance", 0.0)));
				bank.remittance_origin = std::max(0.0,
					double(row.get("remittance_origin", 0.0)));
				bank.harvest_interest = std::max(0.0,
					double(row.get("harvest_interest", 0.0)));
				bank.defaults = std::max(0, int(row.get("defaults", 0)));
				bank.loans_count = std::max(0,
					int(row.get("loans_count", 0)));
				bank.last_default_day = int(
					row.get("last_default_day", -1));
				bank.last_dividend_day = int(
					row.get("last_dividend_day", -1));
				// C6 (D-114): estado de crise. v33 e anteriores nao tem as
				// chaves: o banco carrega CALMO (confianca 1,0, sem corrida,
				// sem resolucao) - nada inventado para tras.
				bank.confidence = std::clamp(
					double(row.get("confidence", 1.0)), 0.0, 1.0);
				bank.loss_seeded = bool(row.get("loss_seeded", false));
				bank.loss_ema = std::max(0.0,
					double(row.get("loss_ema", 0.0)));
				bank.loss_today = std::max(0.0,
					double(row.get("loss_today", 0.0)));
				bank.run_days = std::max(0, int(row.get("run_days", 0)));
				bank.last_run_day = int(row.get("last_run_day", -1));
				bank.run_withdrawn = std::max(0.0,
					double(row.get("run_withdrawn", 0.0)));
				bank.run_cash = std::max(0.0,
					double(row.get("run_cash", 0.0)));
				bank.run_public = std::max(0.0,
					double(row.get("run_public", 0.0)));
				bank.lolr_borrowed = std::max(0.0,
					double(row.get("lolr_borrowed", 0.0)));
				bank.lolr_interest = std::max(0.0,
					double(row.get("lolr_interest", 0.0)));
				bank.lolr_days = std::max(0,
					int(row.get("lolr_days", 0)));
				bank.insured_payout = std::max(0.0,
					double(row.get("insured_payout", 0.0)));
				bank.bailin_cut = std::max(0.0,
					double(row.get("bailin_cut", 0.0)));
				bank.treasury_rescue = std::max(0.0,
					double(row.get("treasury_rescue", 0.0)));
				bank.capital_flight = std::max(0.0,
					double(row.get("capital_flight", 0.0)));
				bank.resolution_day = int(
					row.get("resolution_day", -1));
				bank.resolved = bool(row.get("resolved", false));
				bank.resolution = std::string(String(
					row.get("resolution", "none")).utf8().get_data());
				bank.absorbed_into = std::string(String(
					row.get("absorbed_into", "")).utf8().get_data());
				bank.baseline_seeded = bool(
					row.get("baseline_seeded", false));
				bank.open_capital_ratio = double(
					row.get("open_capital_ratio", 0.0));
				bank.open_liquidity_ratio = double(
					row.get("open_liquidity_ratio", 0.0));
				bank.last_capital_ratio = double(
					row.get("last_capital_ratio", 0.0));
				bank.last_liquidity_ratio = double(
					row.get("last_liquidity_ratio", 0.0));
				bank.last_npl_ratio = double(
					row.get("last_npl_ratio", 0.0));
				ws.banks[bank.id] = bank;
			}
		} else if (nations_variant.get_type() == Variant::DICTIONARY) {
			// MIGRAÇÃO v31 -> v32: o banco AGREGADO por nação da C3 é dividido
			// pelos três papéis com as participações declaradas (0,45 / 0,40 /
			// 0,15) e o resíduo de arredondamento fica no estrangeiro, de modo
			// que a soma por nação é bit-exata. `deposits = 0` (a C3 não tinha
			// dinheiro bancário). Nomes entram por `set_bank_definitions`; aqui
			// só a conservação.
			const Dictionary& nations = nations_variant;
			for (const auto& row_entry : nations.entries()) {
				if (row_entry.first.empty()
						|| row_entry.second.get_type()
							!= Variant::DICTIONARY) {
					continue;
				}
				const Dictionary& row = row_entry.second;
				const double loans =
					std::max(0.0, double(row.get("loans", 0.0)));
				const double arrears =
					std::max(0.0, double(row.get("arrears", 0.0)));
				const double borrowing = std::max(0.0,
					double(row.get("bc_borrowing", 0.0)));
				const double cash =
					std::max(0.0, double(row.get("cash", 0.0)));
				const double capital =
					std::max(0.0, double(row.get("capital", 0.0)));
				const double income = std::max(0.0,
					double(row.get("interest_income", 0.0)));
				const double expense = std::max(0.0,
					double(row.get("interest_expense", 0.0)));
				const double writeoffs = std::max(0.0,
					double(row.get("writeoffs", 0.0)));
				const int defaults =
					std::max(0, int(row.get("defaults", 0)));
				const int count = std::max(0,
					int(row.get("loans_count", 0)));
				const int last_default = int(
					row.get("last_default_day", -1));
				const std::string currency = std::string(
					String(row.get("currency", "")).utf8().get_data());
				static const double kShares[3] = { 0.45, 0.40, 0.15 };
				double loan_part = 0.0;
				double arrears_part = 0.0;
				double borrowing_part = 0.0;
				double cash_part = 0.0;
				double capital_part = 0.0;
				double income_part = 0.0;
				double expense_part = 0.0;
				double writeoff_part = 0.0;
				int count_part = 0;
				for (int part = 0; part < 3; ++part) {
					const bool last = part == 2;
					const double l = last ? loans - loan_part
						: loans * kShares[part];
					const double a = last ? arrears - arrears_part
						: arrears * kShares[part];
					const double b = last ? borrowing - borrowing_part
						: borrowing * kShares[part];
					const double c = last ? cash - cash_part
						: cash * kShares[part];
					const double k = last ? capital - capital_part
						: capital * kShares[part];
					const double i = last ? income - income_part
						: income * kShares[part];
					const double e = last ? expense - expense_part
						: expense * kShares[part];
					const double w = last ? writeoffs - writeoff_part
						: writeoffs * kShares[part];
					const int n = last ? count - count_part
						: static_cast<int>(count * kShares[part]);
					loan_part += l;
					arrears_part += a;
					borrowing_part += b;
					cash_part += c;
					capital_part += k;
					income_part += i;
					expense_part += e;
					writeoff_part += w;
					count_part += n;
					Bank bank;
					bank.nation = row_entry.first;
					bank.role = part;
					bank.id = bank::bank_id(bank.nation, part);
					bank.currency = currency;
					bank.owner_kind = part == 0 ? "state"
						: (part == 1 ? "national" : "foreign");
					bank.name_status = "no_data";
					bank.share = kShares[part];
					bank.loans = l;
					bank.arrears = a;
					bank.bc_borrowing = b;
					bank.cash = c;
					bank.capital = k;
					bank.interest_income = i;
					bank.interest_expense = e;
					bank.writeoffs = w;
					bank.loans_count = n;
					bank.defaults = part == 0 ? defaults : 0;
					bank.last_default_day = part == 0 ? last_default : -1;
					ws.banks[bank.id] = bank;
				}
			}
		}
	}
	if (data.has("tariffs")) {
		const Array saved_tariffs = data.get("tariffs", Array());
		ws.tariffs.clear();
		ws.tariffs.reserve(static_cast<std::size_t>(saved_tariffs.size()));
		for (int index = 0; index < saved_tariffs.size(); ++index) {
			const Dictionary entry = saved_tariffs[index];
			Tariff tariff;
			tariff.buyer = std::string(
				String(entry.get("buyer", "")).utf8().get_data());
			if (tariff.buyer.empty()) {
				continue;
			}
			tariff.seller = std::string(
				String(entry.get("seller", "*")).utf8().get_data());
			tariff.product = std::string(
				String(entry.get("product", "*")).utf8().get_data());
			if (tariff.seller.empty()) {
				tariff.seller = "*";
			}
			if (tariff.product.empty()) {
				tariff.product = "*";
			}
			tariff.rate = std::clamp(
				double(entry.get("rate", 0.0)), 0.0, 1.0);
			ws.tariffs.push_back(std::move(tariff));
		}
		core.rebuild_tariff_lookup();
	}
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
	// C5 (v33/D-112) - BOLSA. v32 e anteriores chegam SEM o bloco: a bolsa é
	// criada no fim da carga (`stock::after_load`), com preço = patrimônio
	// medido e a carteira do default declarado. v33 carrega o estado salvo.
	ws.stock_exchanges.clear();
	ws.stock_tickers.clear();
	ws.stock_enabled = true;
	ws.stock_investment_channel = true;
	ws.stock_risk_premium = 0.0;
	ws.stock_payout_ratio = 0.0;
	ws.stock_step = 0.0;
	ws.stock_expectation_k = 0.0;
	if (data.has("stock")) {
		const Dictionary block = data.get("stock", Dictionary());
		ws.stock_enabled = bool(block.get("enabled", true));
		const Value& policy_variant = block.get("policy", Value());
		if (policy_variant.get_type() == Variant::DICTIONARY) {
			const Dictionary params = policy_variant;
			ws.stock_investment_channel = bool(
				params.get("investment_channel", true));
			ws.stock_risk_premium = std::max(0.0,
				double(params.get("risk_premium", 0.0)));
			ws.stock_payout_ratio = std::max(0.0,
				double(params.get("payout_ratio", 0.0)));
			ws.stock_step = std::max(0.0,
				double(params.get("step", 0.0)));
			ws.stock_expectation_k = std::max(0.0,
				double(params.get("expectation_k", 0.0)));
		}
		const Value& exchanges_variant = block.get("exchanges", Value());
		if (exchanges_variant.get_type() == Variant::DICTIONARY) {
			const Dictionary& exchanges = exchanges_variant;
			for (const auto& row_entry : exchanges.entries()) {
				if (row_entry.first.empty() || row_entry.second.get_type()
						!= Variant::DICTIONARY) {
					continue;
				}
				const Dictionary& row = row_entry.second;
				StockExchange exchange;
				exchange.nation = std::string(
					String(row.get("nation", "")).utf8().get_data());
				if (exchange.nation.empty()) {
					exchange.nation = row_entry.first;
				}
				exchange.name = std::string(
					String(row.get("name", "")).utf8().get_data());
				exchange.name_status = std::string(
					String(row.get("name_status", "game_default"))
						.utf8().get_data());
				exchange.currency = std::string(
					String(row.get("currency", "")).utf8().get_data());
				exchange.confidence = std::clamp(
					double(row.get("confidence", 1.0)), 0.65, 1.35);
				exchange.index_base = std::max(0.0,
					double(row.get("index_base", 0.0)));
				exchange.last_price_day = int(
					row.get("last_price_day", -1));
				// Medição que afeta a trajetória (EMA da produção da nação):
				// vai ao save para a continuação ser idêntica ao caminho direto.
				exchange.production_ema = std::max(0.0,
					double(row.get("production_ema", 0.0)));
				exchange.production_seeded = bool(
					row.get("production_seeded", false));
				exchange.dividends_paid = std::max(0.0,
					double(row.get("dividends_paid", 0.0)));
				exchange.remittance = std::max(0.0,
					double(row.get("remittance", 0.0)));
				exchange.remittance_origin = std::max(0.0,
					double(row.get("remittance_origin", 0.0)));
				ws.stock_exchanges[exchange.nation] = exchange;
			}
		}
		const Value& tickers_variant = block.get("tickers", Value());
		if (tickers_variant.get_type() == Variant::DICTIONARY) {
			const Dictionary& tickers = tickers_variant;
			for (const auto& row_entry : tickers.entries()) {
				if (row_entry.first.empty() || row_entry.second.get_type()
						!= Variant::DICTIONARY) {
					continue;
				}
				const Dictionary& row = row_entry.second;
				StockTicker ticker;
				ticker.group_id = std::string(
					String(row.get("group_id", "")).utf8().get_data());
				if (ticker.group_id.empty()) {
					ticker.group_id = row_entry.first;
				}
				ticker.nation = std::string(
					String(row.get("nation", "")).utf8().get_data());
				ticker.exchange = std::string(
					String(row.get("exchange", "")).utf8().get_data());
				if (ticker.exchange.empty()) {
					ticker.exchange = ticker.nation;
				}
				ticker.shares = std::max(0.0,
					double(row.get("shares", 0.0)));
				ticker.price = std::max(0.0, double(row.get("price", 0.0)));
				ticker.book_equity = std::max(0.0,
					double(row.get("book_equity", 0.0)));
				ticker.profit_ema = double(row.get("profit_ema", 0.0));
				ticker.profit_at_last_dividend = double(
					row.get("profit_at_last_dividend", 0.0));
				ticker.dividend_last = std::max(0.0,
					double(row.get("dividend_last", 0.0)));
				ticker.dividend_limited = std::max(0.0,
					double(row.get("dividend_limited", 0.0)));
				ticker.dividend_day = int(row.get("dividend_day", -1));
				ticker.listed_day = int(row.get("listed_day", 0));
				ticker.delisted_day = int(row.get("delisted_day", -1));
				ticker.listed = bool(row.get("listed", false));
				ticker.reason = std::string(
					String(row.get("reason", "listed")).utf8().get_data());
				const Value& holdings_variant = row.get("holdings", Value());
				if (holdings_variant.get_type() == Variant::DICTIONARY) {
					const Dictionary& holdings = holdings_variant;
					ticker.holdings.households_high = std::max(0.0,
						double(holdings.get("households_high", 0.0)));
					const Value& banks_variant = holdings.get("banks", Value());
					if (banks_variant.get_type() == Variant::DICTIONARY) {
						const Dictionary& bank_map = banks_variant;
						for (const auto& bank_row : bank_map.entries()) {
							ticker.holdings.banks[bank_row.first] =
								std::max(0.0, double(bank_row.second));
						}
					}
					const Value& groups_variant = holdings.get("groups",
						Value());
					if (groups_variant.get_type() == Variant::DICTIONARY) {
						const Dictionary& group_map = groups_variant;
						for (const auto& group_row : group_map.entries()) {
							ticker.holdings.groups[group_row.first] =
								std::max(0.0, double(group_row.second));
						}
					}
					ticker.holdings.foreign = std::max(0.0,
						double(holdings.get("foreign", 0.0)));
				}
				ws.stock_tickers[ticker.group_id] = ticker;
			}
		}
	}
	// C6 (v34/D-114) - CRISE BANCARIA. v33 e anteriores chegam SEM o bloco:
	// `bank_crisis::after_load` inicia os bancos calmos (confianca 1,0), fundo
	// 0 e nenhum evento. O policy salvo e' regra (0 = default declarado).
	ws.bank_crisis = BankCrisisPolicy{};
	ws.deposit_insurance.clear();
	ws.bank_crisis_events.clear();
	ws.bank_crisis_events_total = 0;
	if (data.has("bank_crisis")) {
		const Dictionary block = data.get("bank_crisis", Dictionary());
		ws.bank_crisis.enabled = bool(block.get("enabled", true));
		const Value& policy_variant = block.get("policy", Value());
		if (policy_variant.get_type() == Variant::DICTIONARY) {
			const Dictionary params = policy_variant;
			BankCrisisPolicy& pol = ws.bank_crisis;
			pol.capital_min = std::max(0.0,
				double(params.get("capital_min", 0.0)));
			pol.capital_cut_floor = std::clamp(
				double(params.get("capital_cut_floor", 0.0)), 0.0, 1.0);
			pol.liquidity_min = std::max(0.0,
				double(params.get("liquidity_min", 0.0)));
			pol.loss_scale = std::max(0.0,
				double(params.get("loss_scale", 0.0)));
			pol.loss_ema_days = std::max(0.0,
				double(params.get("loss_ema_days", 0.0)));
			pol.confidence_start = std::max(0.0,
				double(params.get("confidence_start", 0.0)));
			pol.smoothing = std::clamp(
				double(params.get("smoothing", 0.0)), 0.0, 1.0);
			pol.w_solvency = std::max(0.0,
				double(params.get("w_solvency", 0.0)));
			pol.w_liquidity = std::max(0.0,
				double(params.get("w_liquidity", 0.0)));
			pol.w_losses = std::max(0.0,
				double(params.get("w_losses", 0.0)));
			pol.w_contagion = std::max(0.0,
				double(params.get("w_contagion", 0.0)));
			pol.w_market = std::max(0.0,
				double(params.get("w_market", 0.0)));
			pol.run_threshold = std::max(0.0,
				double(params.get("run_threshold", 0.0)));
			pol.run_share = std::clamp(
				double(params.get("run_share", 0.0)), 0.0, 1.0);
			pol.flight_to_quality = std::clamp(
				double(params.get("flight_to_quality", 0.0)), 0.0, 1.0);
			pol.lolr_spread = std::max(0.0,
				double(params.get("lolr_spread", 0.0)));
			pol.lolr_haircut = std::clamp(
				double(params.get("lolr_haircut", 0.0)), 0.0, 1.0);
			pol.lolr_max_assets = std::clamp(
				double(params.get("lolr_max_assets", 0.0)), 0.0, 1.0);
			pol.insured_share = std::clamp(
				double(params.get("insured_share", 0.0)), 0.0, 1.0);
			pol.premium_daily = std::max(0.0,
				double(params.get("premium_daily", 0.0)));
			pol.fund_target_ratio = std::max(0.0,
				double(params.get("fund_target_ratio", 0.0)));
			pol.capital_flight_share = std::clamp(
				double(params.get("capital_flight_share", 0.0)),
				0.0, 1.0);
			pol.insolvency_tolerance = std::max(0.0,
				double(params.get("insolvency_tolerance", 0.0)));
			pol.insolvency_tolerance_ratio = std::max(0.0,
				double(params.get("insolvency_tolerance_ratio", 0.0)));
			pol.treasury_debt_rescue = bool(
				params.get("treasury_debt_rescue", true));
			pol.takeover_public = bool(
				params.get("takeover_public", true));
		}
		const Value& funds_variant = block.get("funds", Value());
		if (funds_variant.get_type() == Variant::DICTIONARY) {
			const Dictionary& funds = funds_variant;
			for (const auto& row_entry : funds.entries()) {
				if (row_entry.first.empty()
						|| row_entry.second.get_type()
							!= Variant::DICTIONARY) {
					continue;
				}
				const Dictionary& row = row_entry.second;
				DepositInsurance fund;
				fund.fund = std::max(0.0,
					double(row.get("fund", 0.0)));
				fund.premiums = std::max(0.0,
					double(row.get("premiums", 0.0)));
				fund.payouts = std::max(0.0,
					double(row.get("payouts", 0.0)));
				fund.last_payout_day = int(
					row.get("last_payout_day", -1));
				ws.deposit_insurance[std::string(row_entry.first)] = fund;
			}
		}
		const Array events = block.get("events", Array());
		for (int index = 0; index < events.size(); ++index) {
			const Dictionary row = events[index];
			BankCrisisEvent event;
			event.day = int(row.get("day", -1));
			event.bank = std::string(
				String(row.get("bank", "")).utf8().get_data());
			event.nation = std::string(
				String(row.get("nation", "")).utf8().get_data());
			event.kind = std::string(
				String(row.get("kind", "")).utf8().get_data());
			event.cause = std::string(
				String(row.get("cause", "")).utf8().get_data());
			event.gap = double(row.get("gap", 0.0));
			event.cost_shareholders = double(
				row.get("cost_shareholders", 0.0));
			event.cost_depositors = double(
				row.get("cost_depositors", 0.0));
			event.cost_fund = double(row.get("cost_fund", 0.0));
			event.cost_treasury = double(
				row.get("cost_treasury", 0.0));
			event.cost_takeover = double(
				row.get("cost_takeover", 0.0));
			event.capital_flight = double(
				row.get("capital_flight", 0.0));
			event.lolr = double(row.get("lolr", 0.0));
			event.emission = double(row.get("emission", 0.0));
			ws.bank_crisis_events.push_back(event);
		}
		// O anel publicado tem tamanho fixo: um save adulterado (ou com mais
		// de kEventRing eventos) não pode inchar o status. Fica o mais recente,
		// como em `register_event`; `events_total` guarda o total.
		if (static_cast<int>(ws.bank_crisis_events.size())
				> bank_crisis::kEventRing) {
			const auto first = ws.bank_crisis_events.end()
				- bank_crisis::kEventRing;
			ws.bank_crisis_events.erase(
				ws.bank_crisis_events.begin(), first);
		}
		ws.bank_crisis_events_total = int(
			block.get("events_total", 0));
	}
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

	ws.cities.clear();
	Array cities = data.get("cities", Array());
	for (int index = 0; index < cities.size(); ++index) {
		Dictionary entry = cities[index];
		City city;
		city.id = std::string(String(entry.get("id", "")).utf8().get_data());
		city.name = std::string(String(entry.get("name", "")).utf8().get_data());
		city.nation = std::string(String(entry.get("nation", "")).utf8().get_data());
		// F5 v7: preço de mercado passou a ficar na ÂNCORA. Saves v6 e
		// anteriores guardavam preço local; converte uma vez pela taxa da nação
		// (as moedas foram carregadas antes das cidades).
		const double save_fx = version >= 7
			? 1.0 : std::max(1e-9, core.fx_rate(city.nation));
		city.home_nation = std::string(
			String(entry.get("home_nation", String(city.nation.c_str()))).utf8().get_data());
		city.landmass =
			std::string(String(entry.get("landmass", "")).utf8().get_data());
		Array center = entry.get("center", Array());
		if (center.size() >= 2) {
			city.center = {double(center[0]), double(center[1])};
		}
		city.population = double(entry.get("population", 0.0));
		// Save antigo (ou cenário sintético) sem o campo: a população real
		// exibida é a própria unidade de simulação (comportamento anterior).
		city.simulation_population = read_sim_population(entry, city.population);
		city.firm_cash = double(entry.get("firm_cash", 0.0));
		city.service_cash = double(entry.get("service_cash", 0.0));
		// D-126 (v41): crédito de giro da cidade. v40 e anteriores chegam sem
		// a chave (dívida zero, sem crise em curso).
		city.firm_debt = version >= 41
			? safe_nonneg(double(entry.get("firm_debt", 0.0))) : 0.0;
		city.firm_credit_arrears = version >= 41
			? safe_nonneg(double(entry.get("firm_credit_arrears", 0.0))) : 0.0;
		city.firm_credit_total = version >= 41
			? safe_nonneg(double(entry.get("firm_credit_total", 0.0))) : 0.0;
		city.firm_credit_rate_add = version >= 41
			? std::max(0.0, double(entry.get("firm_credit_rate_add", 0.0)))
			: 0.0;
		city.firm_credit_default_days = version >= 41
			? std::max(0, int(entry.get("firm_credit_default_days", 0))) : 0;
		city.firm_credit_defaults = version >= 41
			? std::max(0, int(entry.get("firm_credit_defaults", 0))) : 0;
		city.firm_credit_renegotiations = version >= 41
			? std::max(0, int(entry.get("firm_credit_renegotiations", 0)))
			: 0;
		// C1 (save v26): salário atrasado das firmas da cidade. v25 e
		// anteriores não têm a chave — nasce 0.
		city.service_wage_arrears = version >= 26
			? std::max(0.0, double(entry.get("service_wage_arrears", 0.0))) : 0.0;
		city.rural_wage_arrears = version >= 26
			? std::max(0.0, double(entry.get("rural_wage_arrears", 0.0))) : 0.0;
		city.government_budget = double(entry.get("government_budget", 0.0));
		// Save antigo não tem o índice: mantém a média mundial (1,0).
		city.external_income_index = std::clamp(
			double(entry.get("external_income_index", 1.0)), 0.0, 8.0);
		// F7 calibração: TFP derivada do PIB PPP per capita. Save v9 e
		// anteriores não têm o campo: 0 = DERIVAR do índice de renda já gravado
		// (o mesmo `derived:gdp_ppp`), sem inventar produtividade.
		city.productivity_index = std::clamp(
			double(entry.get("productivity_index", 0.0)), 0.0, 8.0);
		// F7 calibração: save anterior à distribuição por Gini não tem o campo;
		// 0 mantém o gate legado por estrato até o GeographyService declarar.
		city.income_gini = std::clamp(
			double(entry.get("income_gini", 0.0)), 0.0, 1.0);
		city.service_jobs = double(entry.get("service_jobs", 0.0));
		city.service_wage = double(entry.get("service_wage", 0.0));
		city.service_wage_bill = double(entry.get("service_wage_bill", 0.0));
		city.service_revenue = double(entry.get("service_revenue", 0.0));
		city.service_profit = double(entry.get("service_profit", 0.0));
		city.opex_revenue = double(entry.get("opex_revenue", 0.0));
		city.freight_revenue = double(entry.get("freight_revenue", 0.0));
		city.construction_revenue = double(entry.get("construction_revenue", 0.0));
		city.construction_revenue_pending =
			double(entry.get("construction_revenue_pending", 0.0));
		city.capital_income_next = double(entry.get("capital_income_next", 0.0));
		city.rent_income_next = std::max(0.0,
			double(entry.get("rent_income_next", 0.0)));
		city.gdp = double(entry.get("gdp", 0.0));
		// F8: serviços (save v10). Sem o bloco (v9 e anteriores), os mercados
		// ficam no preço-âncora do catálogo e os negócios são semeados no fim
		// do load (`seed_services`).
		city.service_export_revenue = double(entry.get("service_export_revenue", 0.0));
		city.service_government_revenue =
			double(entry.get("service_government_revenue", 0.0));
		city.service_input_cost = double(entry.get("service_input_cost", 0.0));
		city.service_business_profit =
			double(entry.get("service_business_profit", 0.0));
		city.service_business_count =
			int(entry.get("service_business_count", 0));
		city.private_health_cover = std::clamp(
			double(entry.get("private_health_cover", 0.0)), 0.0, 1.0);
		city.private_education_cover = std::clamp(
			double(entry.get("private_education_cover", 0.0)), 0.0, 1.0);
		city.service_tonnage_prev = double(entry.get("service_tonnage_prev", 0.0));
		const Array saved_services = entry.get("services", Array());
		for (int sector = 0; sector < kMaxServiceSectors; ++sector) {
			ServiceMarket& market = city.service_markets[sector];
			if (sector < static_cast<int>(ws.service_sectors.size())) {
				market.base_price =
					ws.service_sectors[static_cast<std::size_t>(sector)].price;
			}
			market.price = market.base_price;
			if (sector >= saved_services.size()
					|| saved_services[sector].get_type() != Variant::ARRAY) {
				continue;
			}
			const Array fields = saved_services[sector];
			if (fields.size() < 13) {
				continue;
			}
			market.demand = double(fields[0]);
			market.supply = double(fields[1]);
			market.unmet = double(fields[2]);
			market.price = double(fields[3]) > 0.0
				? double(fields[3]) : market.base_price;
			market.base_price = double(fields[4]) > 0.0
				? double(fields[4]) : market.base_price;
			market.revenue = double(fields[5]);
			market.input_cost = double(fields[6]);
			market.service_cost = double(fields[7]);
			market.jobs = double(fields[8]);
			market.businesses = double(fields[9]);
			market.exports = double(fields[10]);
			market.export_revenue = double(fields[11]);
			market.coverage = std::clamp(double(fields[12]), 0.0, 1.0);
		}

		Array suitability = entry.get("suitability", Array());
		Array revenue = entry.get("revenue_by_product", Array());
		Array imported = entry.get("imported", Array());
		Array exported = entry.get("exported", Array());
		Array import_value = entry.get("import_value", Array());
		Array export_value = entry.get("export_value", Array());
		Array external_demand = entry.get("external_demand", Array());
		Array import_price = entry.get("import_price", Array());
		suitability = migrate_array(suitability);
		revenue = migrate_array(revenue);
		imported = migrate_array(imported);
		exported = migrate_array(exported);
		import_value = migrate_array(import_value);
		export_value = migrate_array(export_value);
		external_demand = migrate_array(external_demand);
		import_price = migrate_array(import_price);
		for (int product = 0; product < ws.products; ++product) {
			city.suitability[product] = product < suitability.size()
				? double(suitability[product]) : (product >= 1 ? 0.0 : 1.0);
			city.revenue_by_product[product] = product < revenue.size()
				? double(revenue[product]) : 0.0;
			city.imported[product] = product < imported.size()
				? double(imported[product]) : 0.0;
			city.exported[product] = product < exported.size()
				? double(exported[product]) : 0.0;
			city.import_value[product] = product < import_value.size()
				? double(import_value[product]) : 0.0;
			city.export_value[product] = product < export_value.size()
				? double(export_value[product]) : 0.0;
			city.external_demand[product] = product < external_demand.size()
				? double(external_demand[product]) : 0.0;
			// F5: cotação de importação (na âncora desde o v7). Save anterior à
			// F5 não tem o array: sem cotação até a primeira compra externa.
			city.import_price[product] = product < import_price.size()
				? safe_nonneg(double(import_price[product])) * save_fx : 0.0;
		}

		Dictionary saved_deposits = entry.get("deposits", Dictionary());
		for (int product = 0; product < ws.products; ++product) {
			const String key = String(core.product_name(product));
			if (!saved_deposits.has(key)) {
				continue;
			}
			Dictionary deposit_entry = saved_deposits[key];
			Deposit& deposit = city.deposits[product];
			deposit.tracked = true;
			deposit.initial_kt = double(deposit_entry.get("initial_kt", 0.0));
			deposit.remaining_kt = double(deposit_entry.get("remaining_kt", 0.0));
			deposit.extracted_kt = double(deposit_entry.get("extracted_kt", 0.0));
			deposit.quality = double(deposit_entry.get("quality", 1.0));
			deposit.depth_m = double(deposit_entry.get("depth_m", 0.0));
		}

		Dictionary pop = entry.get("pop", Dictionary());
		city.pop.population = double(pop.get("population", city.simulation_population));
		city.pop.labor_force = double(pop.get("labor_force", 0.0));
		city.pop.employed = double(pop.get("employed", 0.0));
		city.pop.unemployed = double(pop.get("unemployed", 0.0));
		city.pop.dependents = double(pop.get("dependents", 0.0));
		city.pop.wage_income = double(pop.get("wage_income", 0.0));
		city.pop.capital_income = double(pop.get("capital_income", 0.0));
		city.pop.transfers = double(pop.get("transfers", 0.0));
		city.pop.taxes = double(pop.get("taxes", 0.0));
		city.pop.service_spend = double(pop.get("service_spend", 0.0));
		city.pop.disposable = double(pop.get("disposable", 0.0));
		city.pop.external_income = double(pop.get("external_income", 0.0));
		city.pop.food_spend = double(pop.get("food_spend", 0.0));
		city.pop.consumed = double(pop.get("consumed", 0.0));
		city.pop.need = double(pop.get("need", 0.0));
		city.pop.satisfaction = double(pop.get("satisfaction", 0.0));
		city.pop.savings = double(pop.get("savings", 0.0));
		// C1 (save v26): salário atrasado a receber. v25 e anteriores = 0.
		city.pop.arrears_receivable = version >= 26
			? std::max(0.0, double(pop.get("arrears", 0.0))) : 0.0;
		// F3: estado das necessidades (aditivo; save anterior = zero).
		core.init_pop_needs(city.pop);
		Dictionary needs_state = pop.get("needs", Dictionary());
		city.pop.needs_level = int(needs_state.get("level", 0));
		city.pop.needs_satisfaction = double(needs_state.get("satisfaction",
			city.pop.satisfaction));
		city.pop.needs_spend = double(needs_state.get("spend", 0.0));
		city.pop.durable_spend = double(needs_state.get("durable_spend", 0.0));
		Array saved_level_sat = needs_state.get("level_satisfaction", Array());
		Array saved_level_want = needs_state.get("level_want", Array());
		Array saved_level_cons = needs_state.get("level_consumed", Array());
		for (int level = 0; level < kMaxNeedsLevels; ++level) {
			city.pop.level_satisfaction[level] = level < saved_level_sat.size()
				? double(saved_level_sat[level]) : 0.0;
			city.pop.level_want[level] = level < saved_level_want.size()
				? double(saved_level_want[level]) : 0.0;
			city.pop.level_consumed[level] = level < saved_level_cons.size()
				? double(saved_level_cons[level]) : 0.0;
		}
		Dictionary saved_durable = needs_state.get("durable_stock", Dictionary());
		for (int product = 0; product < ws.products
				&& product < static_cast<int>(city.pop.durable_stock.size()); ++product) {
			const String key = String(core.product_name(product));
			if (saved_durable.has(key)) {
				city.pop.durable_stock[static_cast<std::size_t>(product)] =
					std::max(0.0, double(saved_durable[key]));
			}
		}
		// F7: mercado de trabalho e educação (save v9). Save v8 e anteriores não
		// têm o bloco: o perfil é derivado do índice de renda já gravado. A
		// força de trabalho, o emprego e os salários são recalculados pelo
		// mesmo modelo do primeiro dia (migração documentada).
		Dictionary saved_labor = pop.get("labor", Dictionary());
		const bool has_labor = version >= 9 && !saved_labor.is_empty();
		if (!has_labor) {
			core.apply_labor_profile(city);
		} else {
			Pop& labor_pop = city.pop;
			labor_pop.working_age = std::max(0.0,
				double(saved_labor.get("working_age", labor_pop.working_age)));
			labor_pop.retirees = std::max(0.0,
				double(saved_labor.get("retirees", 0.0)));
			labor_pop.students = std::max(0.0,
				double(saved_labor.get("students", 0.0)));
			labor_pop.recruit_reserve = std::max(0.0,
				double(saved_labor.get("recruit_reserve", 0.0)));
			const auto read_levels = [&saved_labor](const char* key,
					double* out, int count) {
				const Array values = saved_labor.get(key, Array());
				for (int level = 0; level < count; ++level) {
					out[level] = level < values.size()
						? std::max(0.0, double(values[level])) : out[level];
				}
			};
			read_levels("education", labor_pop.education,
				kEducationLevels);
			read_levels("participation", labor_pop.participation,
				kEducationLevels);
			read_levels("labor_force_by_level", labor_pop.labor_force_by_level,
				kEducationLevels);
			read_levels("employed_by_level", labor_pop.employed_by_level,
				kEducationLevels);
			read_levels("unemployed_by_level", labor_pop.unemployed_by_level,
				kEducationLevels);
			read_levels("vacancies_by_level", labor_pop.vacancies_by_level,
				kEducationLevels);
			// B6: o save JÁ grava `unfilled_by_level` mas o load não o lia — o
			// round-trip sem tick deixava de ser bit-exato (campo derivado que
			// volta do save; o próximo `labor_day` o recalcula).
			read_levels("unfilled_by_level", labor_pop.unfilled_by_level,
				kEducationLevels);
			read_levels("wage_by_level", labor_pop.wage_by_level,
				kEducationLevels);
			read_levels("wage_income_by_level",
				labor_pop.wage_income_by_level, kEducationLevels);
			read_levels("studying", labor_pop.studying,
				kEducationLevels - 1);
			// B1: `students` é a POP fora da força em formação — crianças
			// (coorte 0-14) + quem estuda (ver apply_labor_profile/labor_day).
			labor_pop.students = labor_pop.children + labor_pop.studying[0]
				+ labor_pop.studying[1];
			labor_pop.income_level = std::clamp(
				int(saved_labor.get("income_level", 1)), 0, 3);
			// F7 calibração: blocos aditivos. Save anterior ao calibração (v9
			// sem eles) cai no nível único e mantém o salário carregado como
			// histórico (wage_anchored = true, com inércia).
			const bool has_strata = saved_labor.has("income_level_by_stratum");
			if (has_strata) {
				const Array strata = saved_labor.get("income_level_by_stratum", Array());
				for (int stratum = 0; stratum < kEducationLevels; ++stratum) {
					labor_pop.stratum_income_level[stratum] = stratum < strata.size()
						? std::clamp(int(strata[stratum]), 0, 3)
						: labor_pop.income_level;
				}
			} else {
				for (int stratum = 0; stratum < kEducationLevels; ++stratum) {
					labor_pop.stratum_income_level[stratum] = labor_pop.income_level;
				}
			}
			const Array share = saved_labor.get("income_level_share", Array());
			for (int level_index = 0; level_index < kIncomeLevels; ++level_index) {
				labor_pop.income_level_share[level_index] = level_index < share.size()
					? std::clamp(double(share[level_index]), 0.0, 1.0) : 0.0;
			}
			if (share.is_empty()) {
				labor_pop.income_level_share[labor_pop.income_level] = 1.0;
			}
			labor_pop.wage_anchored = bool(saved_labor.get("wage_anchored", true));
			labor_pop.labor_force = 0.0;
			for (int level = 0; level < kEducationLevels; ++level) {
				labor_pop.labor_force +=
					labor_pop.labor_force_by_level[level];
			}
			labor_pop.employed = 0.0;
			labor_pop.unemployed = 0.0;
			for (int level = 0; level < kEducationLevels; ++level) {
				labor_pop.employed += labor_pop.employed_by_level[level];
				labor_pop.unemployed += labor_pop.unemployed_by_level[level];
			}
			labor_pop.wage_income = 0.0;
			for (int level = 0; level < kEducationLevels; ++level) {
				labor_pop.wage_income += labor_pop.wage_income_by_level[level];
			}
			labor_pop.dependents = std::max(0.0,
				city.pop.population - labor_pop.labor_force);
		}
		// B1 (save v16): coortes etárias, DEPOIS do bloco de trabalho. O save v16
		// restaura as bandas e reaplica o que é derivado delas (idade ativa,
		// crianças, fatores de saúde/educação). O v15 e anteriores não têm as
		// bandas: são semeadas da estrutura nacional e a escolaridade guardada é
		// reescalada pela razão da idade ativa, para o perfil paramétrico antigo
		// não ficar inconsistente com as coortes até o primeiro mês. Sem dado
		// para a nação, a cidade fica no perfil paramétrico legado.
		{
			const Array saved_age_bands = pop.get("age_bands", Array());
			double age_total = 0.0;
			const bool has_age_bands = version >= 16
				&& saved_age_bands.size() >= kAgeBands;
			for (int band = 0; band < kAgeBands && has_age_bands; ++band) {
				age_total += std::max(0.0, double(saved_age_bands[band]));
			}
			if (has_age_bands && age_total > 0.0) {
				for (int band = 0; band < kAgeBands; ++band) {
					city.pop.age_band[band] =
						std::max(0.0, double(saved_age_bands[band]));
				}
				city.pop.health_weight_ref = std::max(0.0,
					double(pop.get("health_weight_ref", 0.0)));
				city.pop.education_weight_ref = std::max(0.0,
					double(pop.get("education_weight_ref", 0.0)));
				city.pop.demography_ready = true;
				core.apply_demography(city);
				// O bloco de trabalho acima montou `students` antes de as
				// coortes existirem (`children` ainda 0). Recalcula aqui para o
				// status logo após o load e um save sem tick fecharem exato.
				city.pop.students = city.pop.children + city.pop.studying[0]
					+ city.pop.studying[1];
				city.pop.dependents = std::max(0.0,
					city.pop.population - city.pop.labor_force);
			} else {
				const double old_working = std::max(0.0, city.pop.working_age);
				core.seed_demography(city);
				if (city.pop.demography_ready) {
					const double new_working = city.pop.age_band[1]
						+ city.pop.age_band[2] + city.pop.age_band[3];
					const double ratio = old_working > 1e-6
						? std::clamp(new_working / old_working, 0.5, 2.0) : 1.0;
					for (int level = 0; level < kEducationLevels; ++level) {
						city.pop.education[level] *= ratio;
					}
					for (int track = 0; track < kEducationLevels - 1; ++track) {
						city.pop.studying[track] *= ratio;
					}
					core.apply_demography(city);
					city.pop.labor_force = 0.0;
					city.pop.employed = 0.0;
					city.pop.unemployed = 0.0;
					for (int level = 0; level < kEducationLevels; ++level) {
						const double studying = level < kEducationLevels - 1
							? city.pop.studying[level] : 0.0;
						city.pop.labor_force_by_level[level] = std::max(0.0,
							city.pop.education[level]
								* city.pop.participation[level] - studying);
						city.pop.employed_by_level[level] = std::min(
							city.pop.employed_by_level[level],
							city.pop.labor_force_by_level[level]);
						city.pop.unemployed_by_level[level] = std::max(0.0,
							city.pop.labor_force_by_level[level]
								- city.pop.employed_by_level[level]);
						city.pop.labor_force +=
							city.pop.labor_force_by_level[level];
						city.pop.employed += city.pop.employed_by_level[level];
						city.pop.unemployed += city.pop.unemployed_by_level[level];
					}
					city.pop.students = city.pop.children
						+ city.pop.studying[0] + city.pop.studying[1];
					city.pop.dependents = std::max(0.0,
						city.pop.population - city.pop.labor_force);
				}
			}
		}
		// B2 (save v17): cinco quintis de renda. Save v16 e anteriores não têm
		// o bloco: a semeadura acontece no FIM do load (depois do bloco
		// `urban`, para a posse de moradia já estar carregada) com a proxy do
		// Gini declarado. A poupança escalar existente é repartida pela renda.
		const Dictionary saved_strata = pop.get("strata", Dictionary());
		if (version >= 17 && !saved_strata.is_empty()) {
			const auto read_strata = [&saved_strata](const char* key,
					double* out) {
				const Array values = saved_strata.get(key, Array());
				for (int s = 0; s < kIncomeStrata; ++s) {
					if (s < values.size()) {
						out[s] = std::max(0.0, double(values[s]));
					}
				}
			};
			read_strata("income_share", city.pop.strata_income_share);
			read_strata("people", city.pop.stratum_people);
			read_strata("wage_factor", city.pop.stratum_wage_factor);
			read_strata("capital_share", city.pop.stratum_capital_share);
			read_strata("savings", city.pop.stratum_savings);
			read_strata("debt", city.pop.stratum_debt);
			read_strata("equity", city.pop.stratum_equity);
			read_strata("wealth", city.pop.stratum_wealth);
			read_strata("saving_rate", city.pop.stratum_saving_rate);
			read_strata("net_income", city.pop.stratum_net_income);
			read_strata("rent_paid", city.pop.stratum_rent_paid);
			const Array levels = saved_strata.get("level", Array());
			for (int s = 0; s < kIncomeStrata; ++s) {
				city.pop.stratum_level[s] = s < levels.size()
					? std::clamp(int(levels[s]), 0, kIncomeLevels - 1) : 0;
			}
			const Array education = saved_strata.get("education", Array());
			for (int s = 0; s < kIncomeStrata && s < education.size(); ++s) {
				if (education[s].get_type() != Variant::ARRAY) {
					continue;
				}
				const Array row = education[s];
				for (int e = 0; e < kEducationLevels && e < row.size(); ++e) {
					city.pop.stratum_education_mix[s][e] =
						std::max(0.0, double(row[e]));
				}
			}
			const Array tenure = saved_strata.get("tenure", Array());
			for (int s = 0; s < kIncomeStrata && s < tenure.size(); ++s) {
				if (tenure[s].get_type() != Variant::ARRAY) {
					continue;
				}
				const Array row = tenure[s];
				for (int kind = 0; kind < kStrataTenureCount
						&& kind < row.size(); ++kind) {
					city.pop.stratum_tenure[s][kind] =
						std::max(0.0, double(row[kind]));
				}
			}
			const Array lifecycle = saved_strata.get("lifecycle", Array());
			for (int band = 0; band < 5 && band < lifecycle.size(); ++band) {
				city.pop.lifecycle_saving[band] =
					std::max(0.0, double(lifecycle[band]));
			}
			city.pop.strata_ready = bool(saved_strata.get("ready", true));
			// O escalar da cidade É a soma dos quintis (round-trip exato).
			if (city.pop.strata_ready) {
				double total = 0.0;
				for (int s = 0; s < kIncomeStrata; ++s) {
					total += city.pop.stratum_savings[s];
				}
				city.pop.savings = total;
			}
		}
		// B7 (save v22): informalidade, mortalidade e insatisfação (urbano).
		// v21 e anteriores migram depois (segunda passada, com o dado da nação).
		if (version >= 22) {
			load_informal_block(pop.get("informal", Dictionary()),
				city.pop.informal_share, city.pop.informal_labor_share,
				city.pop.informal_income, city.pop.informal_wage_bill,
				city.pop.informal_service_workers, city.pop.informal_service_output,
				city.pop.informal_service_demand, city.pop.informal_service_revenue,
				city.pop.informal_service_price, city.pop.informal_service_share);
			load_mortality_block(pop.get("mortality", Dictionary()),
				city.pop.deaths_expected, city.pop.deaths_total,
				city.pop.excess_deaths_total, city.pop.excess_deaths_famine_total,
				city.pop.famine_months, city.pop.food_price_ref,
				city.pop.food_inflation_month, city.pop.health_access_ref,
				city.pop.satisfaction_prev);
			load_dissatisfaction_block(pop.get("dissatisfaction", Dictionary()),
				city.pop.dissatisfaction, city.pop.dissatisfaction_by_stratum,
				city.pop.dissatisfaction_unemployment, city.pop.dissatisfaction_rent,
				city.pop.dissatisfaction_food, city.pop.dissatisfaction_inflation,
				city.pop.dissatisfaction_informality);
			city.pop.informality_seeded = true;
		}
		// B3 (save v19): região da cidade (estados inteiros). Save v18 e
		// anteriores migram no mundo real: `region_pop = max(0, population -
		// simulation_population)` (dado já no save), coortes/estratos da nação e
		// mix neutro (sem dado de urbanização). Nada é inventado.
		{
			Region region;
			const Dictionary saved_region = entry.get("region", Dictionary());
			if (version >= 19 && !saved_region.is_empty()) {
				region.ready = bool(saved_region.get("ready", true));
				region.name = std::string(
					String(saved_region.get("name", "")).utf8().get_data());
				region.population_region = safe_nonneg(
					double(saved_region.get("population_region", 0.0)));
				region.urban_population = safe_nonneg(
					double(saved_region.get("urban_population", 0.0)));
				region.region_pop = safe_nonneg(
					double(saved_region.get("region_pop", 0.0)));
				const double urban_share = double(
					saved_region.get("urban_share", -1.0));
				region.urban_share = std::isfinite(urban_share)
					&& urban_share >= 0.0 ? std::clamp(urban_share, 0.0, 1.0)
					: -1.0;
				region.cash = std::max(0.0,
					double(saved_region.get("cash", 0.0)));
				// B7: renda do dia (semanal), usada pelo serviço informal diário.
				region.wage_income = safe_nonneg(
					double(saved_region.get("wage_income", 0.0)));
				region.income = safe_nonneg(
					double(saved_region.get("income", 0.0)));
				region.taxes = safe_nonneg(
					double(saved_region.get("taxes", 0.0)));
				const Array mix = saved_region.get("employment_mix", Array());
				for (int m = 0; m < 3 && m < mix.size(); ++m) {
					region.employment_mix[m] = safe_nonneg(double(mix[m]));
				}
				const Array region_states = saved_region.get("states", Array());
				for (int index = 0; index < region_states.size(); ++index) {
					if (region_states[index].get_type() != Variant::DICTIONARY) {
						continue;
					}
					const Dictionary state_entry = region_states[index];
					RegionState state;
					state.id = std::string(
						String(state_entry.get("id", "")).utf8().get_data());
					state.name = std::string(
						String(state_entry.get("name", "")).utf8().get_data());
					state.population = safe_nonneg(
						double(state_entry.get("population", 0.0)));
					state.area_km2 = safe_nonneg(
						double(state_entry.get("area_km2", 0.0)));
					region.states.push_back(std::move(state));
				}
				const Array region_age_bands =
					saved_region.get("age_bands", Array());
				if (region_age_bands.size() >= kAgeBands) {
					double total = 0.0;
					for (int band = 0; band < kAgeBands; ++band) {
						region.age_band[band] =
							std::max(0.0, double(region_age_bands[band]));
						total += region.age_band[band];
					}
					if (total > 0.0) {
						region.demography_ready = true;
					}
				}
				const Dictionary region_strata =
					saved_region.get("strata", Dictionary());
				region.strata_ready = bool(region_strata.get("ready", true));
				const auto read_region_strata = [&region_strata](
						const char* key, double* out) {
					const Array values = region_strata.get(key, Array());
					for (int s = 0; s < kIncomeStrata; ++s) {
						out[s] = s < values.size()
							? std::max(0.0, double(values[s])) : 0.0;
					}
				};
				read_region_strata("income_share", region.strata_income_share);
				read_region_strata("people", region.stratum_people);
				read_region_strata("savings", region.stratum_savings);
				// C1 (save v26): salário rural atrasado a receber. v25 e
				// anteriores = 0.
				region.arrears_receivable = version >= 26
					? std::max(0.0, double(saved_region.get("arrears", 0.0)))
					: 0.0;
				// B4 (save v20): acumuladores de migração. v19 e anteriores não
				// têm o bloco — nascem zerados (a migração conta do dia 0 do
				// save novo; nada é inventado para trás).
				const Dictionary region_migration =
					saved_region.get("migration", Dictionary());
				region.migration_rural_in_total = safe_nonneg(
					double(region_migration.get("rural_in_total", 0.0)));
				region.migration_city_in_total = safe_nonneg(
					double(region_migration.get("city_in_total", 0.0)));
				region.migration_city_out_total = safe_nonneg(
					double(region_migration.get("city_out_total", 0.0)));
				// B5 (save v21): níveis de infra. v20 e anteriores não têm o
				// bloco — nascem zerados (nível 0 = ausência explícita; nada é
				// inventado para trás). O valor é limitado a 0..kInfraMaxLevel.
				const Dictionary region_infra =
					saved_region.get("infra", Dictionary());
				region.infra_road = std::clamp(int(region_infra.get("road", 0)),
					0, kInfraMaxLevel);
				region.infra_power = std::clamp(int(region_infra.get("power", 0)),
					0, kInfraMaxLevel);
				region.infra_water = std::clamp(int(region_infra.get("water", 0)),
					0, kInfraMaxLevel);
				// B6 (save v23): patrimônio da região. v22 e anteriores NÃO têm
				// o bloco: a região fica inerte (`property_ready` falso) até o
				// dado ser reaplicado pelo GDScript (`set_region_property`), e a
				// alíquota nasce 0 — nada é inventado para trás.
				if (version >= 23) {
					const Dictionary region_property =
						saved_region.get("property", Dictionary());
					region.land_ha = safe_nonneg(
						double(region_property.get("land_ha", 0.0)));
					// Sem hectares não há patrimônio: a região fica inerte mesmo
					// que o bloco exista (ausência explícita).
					region.property_ready =
						bool(region_property.get("ready", true))
						&& region.land_ha > 0.0;
					region.land_gini = std::clamp(
						double(region_property.get("land_gini", 0.0)), 0.0, 1.0);
					region.land_gini_observed =
						bool(region_property.get("land_gini_observed", false));
					region.land_value_per_ha = safe_nonneg(
						double(region_property.get("land_value_per_ha", 0.0)));
					region.land_book_value_per_ha = safe_nonneg(
						double(region_property.get("land_book_value_per_ha", 0.0)));
					region.land_income_month = safe_nonneg(
						double(region_property.get("land_income_month", 0.0)));
					region.commodity_index = safe_nonneg(
						double(region_property.get("commodity_index", 1.0)));
					region.housing_price = safe_nonneg(
						double(region_property.get("housing_price", 0.0)));
					region.housing_book_price = safe_nonneg(
						double(region_property.get("housing_book_price", 0.0)));
					region.housing_rent = safe_nonneg(
						double(region_property.get("housing_rent", 0.0)));
					region.ring_population = safe_nonneg(
						double(region_property.get("ring_population", 0.0)));
					region.ring_price = safe_nonneg(
						double(region_property.get("ring_price", 0.0)));
					region.ring_book_price = safe_nonneg(
						double(region_property.get("ring_book_price", 0.0)));
					region.ring_rent = safe_nonneg(
						double(region_property.get("ring_rent", 0.0)));
					region.ring_rent_paid_month = safe_nonneg(
						double(region_property.get("ring_rent_paid_month", 0.0)));
					region.migration_ring_in_total = safe_nonneg(
						double(region_property.get("migration_ring_in_total", 0.0)));
					region.migration_month_ring_in = safe_nonneg(
						double(region_property.get("migration_month_ring_in", 0.0)));
					region.property_tax_rate = std::clamp(
						double(region_property.get("tax_rate", 0.0)), 0.0, 1.0);
					region.property_tax_potential = safe_nonneg(
						double(region_property.get("tax_potential", 0.0)));
					const auto read_property_values = [&region_property](
							const char* key, double* out) {
						const Array values =
							region_property.get(key, Array());
						for (int s = 0; s < kIncomeStrata; ++s) {
							out[s] = s < values.size()
								? safe_nonneg(double(values[s])) : 0.0;
						}
					};
					read_property_values("land_value", region.stratum_land_value);
					read_property_values("home_value", region.stratum_home_value);
					read_property_values("ring_value", region.stratum_ring_value);
					read_property_values("wealth", region.stratum_wealth);
					// O anel é um subconjunto do interior (invariante de pessoas).
					region.ring_population = std::min(region.ring_population,
						std::max(0.0, region.region_pop));
				}
				// B7 (save v22): informalidade, mortalidade e insatisfação do
				// interior. v21 e anteriores migram na segunda passada.
				if (version >= 22) {
					load_informal_block(saved_region.get("informal", Dictionary()),
						region.informal_share, region.informal_labor_share,
						region.informal_income, region.informal_wage_bill,
						region.informal_service_workers, region.informal_service_output,
						region.informal_service_demand, region.informal_service_revenue,
						region.informal_service_price, region.informal_service_share);
					load_mortality_block(saved_region.get("mortality", Dictionary()),
						region.deaths_expected, region.deaths_total,
						region.excess_deaths_total, region.excess_deaths_famine_total,
						region.famine_months, region.food_price_ref,
						region.food_inflation_month, region.health_access_ref,
						region.satisfaction_prev);
					load_dissatisfaction_block(
						saved_region.get("dissatisfaction", Dictionary()),
						region.dissatisfaction, region.dissatisfaction_by_stratum,
						region.dissatisfaction_unemployment, region.dissatisfaction_rent,
						region.dissatisfaction_food, region.dissatisfaction_inflation,
						region.dissatisfaction_informality);
					region.informality_seeded = true;
				}
			} else if (ws.world_mode) {
				// v18 → v19: interior = população regional exibida - população de
				// simulação (ambos já no save). Cenário sintético (world_mode
				// false) continua sem região (ausência explícita).
				region.ready = true;
				region.population_region = safe_nonneg(city.population);
				region.urban_population = safe_nonneg(city.simulation_population);
				region.region_pop = std::max(0.0,
					city.population - city.simulation_population);
				region.urban_share = -1.0;
			}
			city.region = std::move(region);
			if (city.region.ready) {
				// Reconciliia sem re-semear os quintis já carregados do save
				// (seed_region resetaria a poupança/people lidas acima).
				core.recompute_region_mix(city);
				core.apply_region_demography(city);
				if (!city.region.strata_ready) {
					core.seed_region_strata(city, nullptr);
				}
			}
		}
		city.government_goods_spend = double(entry.get("government_goods_spend", 0.0));
		// §15 (save v12): emprego setorial — base da capacidade de construção do
		// prólogo (ver to_dict). Save v11 não tem a chave: zeros, recalculado no
		// primeiro tick (comportamento anterior, só que agora o piso da obra cobre).
		const Array saved_sector_workers = entry.get("sector_workers", Array());
		const Array saved_sector_share = entry.get("sector_share", Array());
		for (int slot = 0; slot < 4; ++slot) {
			city.sector_workers[slot] = slot < saved_sector_workers.size()
				? std::max(0.0, double(saved_sector_workers[slot])) : 0.0;
			city.sector_share[slot] = slot < saved_sector_share.size()
				? std::clamp(double(saved_sector_share[slot]), 0.0, 1.0) : 0.0;
		}
		Array desired_by_category = pop.get("desired_by_category", Array());
		Array consumed_by_category = pop.get("consumed_by_category", Array());
		city.pop.desired_by_category.assign(ws.food_products.size(), 0.0);
		city.pop.consumed_by_category.assign(ws.food_products.size(), 0.0);
		for (int product = 0; product < static_cast<int>(ws.food_products.size()); ++product) {
			city.pop.desired_by_category[product] =
				product < desired_by_category.size()
					? double(desired_by_category[product]) : 0.0;
			city.pop.consumed_by_category[product] =
				product < consumed_by_category.size()
					? double(consumed_by_category[product]) : 0.0;
		}

		Array markets = entry.get("markets", Array());
		// F0: estoque do combustivel generico no save antigo (indice antigo de
		// `fuel` na lista salva). Rateado nas tres grades logo abaixo.
		double legacy_fuel_stock = 0.0;
		if (legacy_fuel_index >= 0 && legacy_fuel_index < markets.size()) {
			const Dictionary legacy_market = markets[legacy_fuel_index];
			legacy_fuel_stock = safe_nonneg(
				double(legacy_market.get("stock", 0.0)));
		}
		if (has_saved_products || needs_legacy_migration) {
			// Remapeia os mercados antigos por id (save v7 com 114 produtos,
			// descartando `body_armor`) ou pelo deslocamento -2 (save v1/v2 com
			// farinha/pão). Mercado sem correspondência recebe o preço-base.
			Array remapped;
			remapped.resize(ws.products);
			for (int new_index = 0; new_index < ws.products; ++new_index) {
				Dictionary m;
				m["stock"] = 0.0;
				m["price"] = core.base_price(new_index);
				m["demand"] = 0.0;
				m["supply"] = 0.0;
				m["desired"] = 0.0;
				m["sold"] = 0.0;
				m["sold_prev"] = 0.0;
				m["shortage"] = 0.0;
				m["surplus"] = 0.0;
				remapped[new_index] = m;
			}
			for (int old_index = 0; old_index < markets.size(); ++old_index) {
				const int new_index = migrate_index(old_index);
				if (new_index >= 0 && new_index < ws.products) {
					remapped[new_index] = markets[old_index];
				}
			}
			markets = remapped;
		}
		// Saves antigos (4 produtos) não trazem mercados dos novos bens: preço-base.
		for (int product = markets.size(); product < ws.products; ++product) {
			if (product >= 0) {
				city.markets[product].price = core.base_price(product);
			}
		}
		for (int product = 0; product < ws.products && product < markets.size(); ++product) {
			Dictionary market = markets[product];
			city.markets[product].stock = double(market.get("stock", 0.0));
			city.markets[product].price = double(market.get("price", 1.0)) * save_fx;
			city.markets[product].demand = double(market.get("demand", 0.0));
			city.markets[product].supply = double(market.get("supply", 0.0));
			city.markets[product].desired = double(market.get("desired", 0.0));
			city.markets[product].sold = double(market.get("sold", 0.0));
			city.markets[product].sold_prev = double(market.get("sold_prev", 0.0));
			city.markets[product].shortage = double(market.get("shortage", 0.0));
			city.markets[product].surplus = double(market.get("surplus", 0.0));
		}
		// F0: o estoque generico de `fuel` do save antigo entra nas tres
		// grades pelas fracoes da refinaria. O preco local de cada grade comeca
		// no preco-base declarado em goods.json (nao ha preco generico a
		// herdar sem inventar conversao entre bens diferentes).
		if (legacy_fuel_stock > 0.0) {
			city.markets[legacy_gasoline].stock +=
				legacy_fuel_stock * kLegacyFuelGasolineShare;
			city.markets[legacy_diesel].stock +=
				legacy_fuel_stock * kLegacyFuelDieselShare;
			city.markets[legacy_jet].stock +=
				legacy_fuel_stock * kLegacyFuelJetShare;
		}
		ws.cities.push_back(city);
	}

	ws.buildings.clear();
	Array buildings = data.get("buildings", Array());
	for (int index = 0; index < buildings.size(); ++index) {
		Dictionary entry = buildings[index];
		Building building;
		building.id = std::string(String(entry.get("id", "")).utf8().get_data());
		building.city = std::string(String(entry.get("city", "")).utf8().get_data());
		building.kind = int(entry.get("kind", 0));
		building.resource =
			std::string(String(entry.get("resource", "")).utf8().get_data());
		building.owner =
			std::string(String(entry.get("owner", "private")).utf8().get_data());
		building.local_share = std::clamp(
			double(entry.get("local_share", 0.0)), 0.0, 1.0);
		building.partner_group = std::string(
			String(entry.get("partner_group", "")).utf8().get_data());
		building.input_product = int(entry.get("input_product", -1));
		building.output_product = int(entry.get("output_product", 0));
		if (has_saved_products || needs_legacy_migration) {
			const int legacy_output = legacy_substitute(building.output_product);
			building.input_product = migrate_index(building.input_product);
			building.output_product = migrate_index(building.output_product);
			if (building.output_product < 0 && legacy_output >= 0) {
				// F0: esta construcao produzia o `fuel` generico; passa a
				// produzir gasoline (a refinaria entrega diesel e querosene
				// por co-producao).
				building.output_product = legacy_output;
			}
			if (building.output_product < 0) {
				// Produto removido da cadeia (farinha/pão ou body_armor):
				// construção descartada.
				continue;
			}
		}
		building.capacity = double(entry.get("capacity", 0.0));
		building.input_per_output = double(entry.get("input_per_output", 0.0));
		// Receita industrial: o id é resolvido na tabela carregada por
		// set_recipes (v4). Save v3 não tem a chave -> produtor legado.
		const std::string building_recipe_id =
			std::string(String(entry.get("recipe", "")).utf8().get_data());
		building.recipe = building_recipe_id.empty()
			? -1 : core.recipe_index(building_recipe_id);
		if (building.recipe < 0) {
			// F0: receita do combustivel generico (fuel_a..fuel_d) de um save
			// antigo -> a receita que ocupa o lugar hoje.
			const char* alias = legacy_recipe_alias(building_recipe_id);
			if (alias != nullptr) {
				building.recipe = core.recipe_index(std::string(alias));
			}
		}
		building.capital_per_capacity = std::max(1.0, safe_nonneg(
			double(entry.get("capital_per_capacity", kCapitalPerCapacity))));
		building.jobs = double(entry.get("jobs", 0.0));
		building.wage = double(entry.get("wage", 0.0));
		building.opex = double(entry.get("opex", 0.0));
		// F7: mistura de escolaridade. Save v8 sem a chave: usa a da receita
		// resolvida; primária segue o padrão do struct (quase só básico).
		if (entry.has("education_mix")) {
			const Array mix = entry.get("education_mix", Array());
			double total = 0.0;
			for (int level = 0;
					level < kEducationLevels && level < mix.size(); ++level) {
				building.education_mix[level] = std::max(0.0, double(mix[level]));
				total += building.education_mix[level];
			}
			if (total > 1e-9) {
				for (int level = 0; level < kEducationLevels; ++level) {
					building.education_mix[level] /= total;
				}
			}
		} else if (building.recipe >= 0
				&& building.recipe < static_cast<int>(ws.recipes.size())) {
			const Recipe& recipe =
				ws.recipes[static_cast<std::size_t>(building.recipe)];
			for (int level = 0; level < kEducationLevels; ++level) {
				building.education_mix[level] = recipe.education_mix[level];
			}
		}
		building.output_scale = double(entry.get("output_scale", 1.0));
		building.last_utilization = double(entry.get("last_utilization", 1.0));
		building.ramp_days = std::max(0, int(entry.get("ramp_days", 0)));
		building.strategic = bool(entry.get("strategic", false));
		building.loss_days = int(entry.get("loss_days", 0));
		building.bankrupt = bool(entry.get("bankrupt", false));
		// D-125 (save v40): eficiência de produção. Save v39 e anteriores não
		// têm as chaves: a planta é estoque PRÉ-EXISTENTE do ano-base, então
		// carrega MADURA (eficiência 1, trator/máquina ligados) — nada
		// inventado para trás e o comportamento anterior preservado.
		building.production_efficiency = std::clamp(
			version >= 40 ? double(entry.get("production_efficiency", 1.0)) : 1.0,
			0.0, 1.0);
		building.efficiency_days = version >= 40
			? std::max(0, int(entry.get("efficiency_days", 9999))) : 9999;
		building.transition_days = version >= 40
			? std::max(0, int(entry.get("transition_days", 0))) : 0;
		building.equipment = std::clamp(version >= 40
			? double(entry.get("equipment", 0.0)) : 1.0, 0.0, 1.0);
		building.refinery_mode = version >= 40
			? std::clamp(int(entry.get("refinery_mode", 0)), 0, 2) : 0;
		// D-126 (save v41): sobrevivência em crise. Save v40 e anteriores
		// carrega neutro (sem hibernação, sem demissão/liquidação em curso).
		if (version >= 41) {
			building.crisis_stage = std::clamp(
				int(entry.get("crisis_stage", 0)), 0, 6);
			building.last_layoff_day = std::max(-1000000,
				int(entry.get("last_layoff_day", -1000000)));
			building.last_credit_day = std::max(-1000000,
				int(entry.get("last_credit_day", -1000000)));
			building.last_liquidation_day = std::max(-1000000,
				int(entry.get("last_liquidation_day", -1000000)));
			building.mothballed = bool(entry.get("mothballed", false));
			building.mothball_day = int(entry.get("mothball_day", -1));
			building.mothball_unpaid_days = std::max(0,
				int(entry.get("mothball_unpaid_days", 0)));
			building.reopen_signal_days = std::max(0,
				int(entry.get("reopen_signal_days", 0)));
			building.farm_area_reduced = bool(
				entry.get("farm_area_reduced", false));
		}
		building.idle_today = false;
		building.equipment_active = building.equipment > 0.0;
		// D-086 (decisão do dono): o recurso nunca acaba — ignora o `depleted`
		// salvo (que antes marcava a jazida esgotada) e força `false`. O formato
		// e a versão (v18) NÃO mudam.
		building.depleted = false;
		// D-086 (save v18): estágio de mina. Save v17 e anteriores (sem `stage`)
		// entra no ÚLTIMO estágio com o estágio esgotado: é a mina histórica que
		// produz quase nada e só volta pagando o aprofundamento.
		if (entry.has("stage")) {
			building.stage = std::max(0, int(entry.get("stage", 0)));
			building.stage_reserve_kt = std::max(0.0,
				double(entry.get("stage_reserve_kt", 0.0)));
			building.stage_mined_kt = std::clamp(
				double(entry.get("stage_mined_kt", 0.0)), 0.0,
				building.stage_reserve_kt);
		} else {
			building.stage = kMineInitialStage;
			building.stage_reserve_kt = 0.0;
			building.stage_mined_kt = 0.0;
		}
		// B5 (save v21): royalty/imposto de concessão. Save v20 e anteriores
		// não têm a chave: 0 (sem concessão — nada inventado para trás).
		building.royalty = std::clamp(
			double(entry.get("royalty", 0.0)), 0.0, 1.0);
		// C1 (save v26): salário atrasado da planta. v25 e anteriores não têm
		// a chave — nasce 0 (nada inventado para trás).
		building.wage_arrears = version >= 26
			? std::max(0.0, double(entry.get("wage_arrears", 0.0))) : 0.0;
		ws.buildings.push_back(building);
	}

	// §15 (save v12): obras em andamento. Save v11 e anteriores não têm a chave:
	// lista vazia (nenhum projeto em voo no mundo antigo — migração trivial).
	ws.construction_projects.clear();
	const Array saved_projects = data.get("construction_projects", Array());
	for (int index = 0; index < saved_projects.size(); ++index) {
		if (saved_projects[index].get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary entry = saved_projects[index];
		ConstructionProject project;
		project.id = std::string(String(entry.get("id", "")).utf8().get_data());
		if (project.id.empty()) {
			continue;
		}
		// §19 (W1): obra de infraestrutura urbana. Ausente no save anterior ao
		// W1 = 0 (obra econômica normal). A obra urbana não tem produto, então
		// a migração de índice de produto abaixo não se aplica a ela.
		project.urban_action = std::clamp(
			int(entry.get("urban_action", 0)), 0, 2);
		project.urban_zone = std::clamp(
			int(entry.get("urban_zone", 0)), 0, ZONE_COUNT - 1);
		project.urban_km2 = safe_nonneg(double(entry.get("urban_km2", 0.0)));
		// Save v14: kind explícito na fila única (0 prédio, 1 moradia, 2 infra).
		// Save v13 e anteriores não têm a chave: a derivação por `urban_action`
		// mantém a obra urbana como infra (2) e o resto como prédio (0).
		project.kind = std::clamp(
			int(entry.get("kind", project.urban_action != 0 ? 2 : 0)), 0,
			static_cast<int>(CONSTRUCTION_POWER_LINE));
		// Moradia (§19 W3): densidade, padrão, dono e unidades da obra, mais a
		// parcela de terra já paga à prefeitura na abertura.
		project.housing_density = std::clamp(
			int(entry.get("housing_density", 0)), 0, DENSITY_COUNT - 1);
		project.housing_tier = std::clamp(
			int(entry.get("housing_tier", 0)), 0, TIER_COUNT - 1);
		project.housing_owner = std::clamp(
			int(entry.get("housing_owner", 0)), 0,
			static_cast<int>(HOUSING_PROJECT_GOVERNMENT));
		project.housing_units = safe_nonneg(
			double(entry.get("housing_units", 0.0)));
		project.land_cost = safe_nonneg(double(entry.get("land_cost", 0.0)));
		// Obra urbana e moradia não têm produto: a migração de índice de produto
		// abaixo não se aplica a elas.
		const bool urban_project = project.urban_action != 0
			|| project.kind == CONSTRUCTION_HOUSING;
		project.building_id = std::string(
			String(entry.get("building_id", "")).utf8().get_data());
		project.city = std::string(String(entry.get("city", "")).utf8().get_data());
		project.owner = std::string(
			String(entry.get("owner", "private")).utf8().get_data());
		project.group_id = std::string(
			String(entry.get("group_id", "")).utf8().get_data());
		project.resource = std::string(
			String(entry.get("resource", "")).utf8().get_data());
		project.output_product = int(entry.get("output_product", -1));
		if (!urban_project && (has_saved_products || needs_legacy_migration)) {
			const int legacy_output = legacy_substitute(project.output_product);
			project.output_product = migrate_index(project.output_product);
			if (project.output_product < 0 && legacy_output >= 0) {
				// F0: obra do `fuel` generico -> gasoline (ver save v39).
				project.output_product = legacy_output;
			}
			if (project.output_product < 0) {
				continue;  // produto removido: obra descartada
			}
		}
		const std::string project_recipe_id =
			std::string(String(entry.get("recipe", "")).utf8().get_data());
		project.recipe = project_recipe_id.empty()
			? -1 : core.recipe_index(project_recipe_id);
		if (project.recipe < 0) {
			const char* alias = legacy_recipe_alias(project_recipe_id);
			if (alias != nullptr) {
				project.recipe = core.recipe_index(std::string(alias));
			}
		}
		project.input_product = int(entry.get("input_product", -1));
		if (!urban_project && (has_saved_products || needs_legacy_migration)) {
			project.input_product = migrate_index(project.input_product);
		}
		project.capacity = std::max(0.0, double(entry.get("capacity", 0.0)));
		project.jobs = std::max(0.0, double(entry.get("jobs", 0.0)));
		project.wage = std::max(0.0, double(entry.get("wage", 0.0)));
		project.opex = std::max(0.0, double(entry.get("opex", 0.0)));
		project.input_per_output = double(entry.get("input_per_output", 0.0));
		const Array mix = entry.get("education_mix", Array());
		if (mix.size() >= kEducationLevels) {
			double total = 0.0;
			for (int level = 0; level < kEducationLevels; ++level) {
				project.education_mix[level] = std::max(0.0, double(mix[level]));
				total += project.education_mix[level];
			}
			if (total > 1e-9) {
				for (int level = 0; level < kEducationLevels; ++level) {
					project.education_mix[level] /= total;
				}
			}
		}
		project.local_share = std::clamp(
			double(entry.get("local_share", 0.0)), 0.0, 1.0);
		project.partner_group = std::string(
			String(entry.get("partner_group", "")).utf8().get_data());
		project.strategic = bool(entry.get("strategic", false));
		project.expand_existing = bool(entry.get("expand_existing", false));
		project.total_cost = std::max(0.0, double(entry.get("total_cost", 0.0)));
		project.labor_cost_total = std::max(0.0,
			double(entry.get("labor_cost_total", 0.0)));
		project.paid = std::max(0.0, double(entry.get("paid", 0.0)));
		project.labor_paid = std::max(0.0, double(entry.get("labor_paid", 0.0)));
		project.reserved = std::max(0.0, double(entry.get("reserved", 0.0)));
		project.duration_days = std::max(1, int(entry.get("duration_days", 30)));
		project.days_elapsed = std::max(0, int(entry.get("days_elapsed", 0)));
		project.paused = bool(entry.get("paused", false));
		project.pause_reason = std::string(
			String(entry.get("pause_reason", "")).utf8().get_data());
		project.created_day = int(entry.get("created_day", 0));
		// D-086: obra de aprofundamento de mina. Save v17 e anteriores sem a chave: -1.
		project.deepen_stage = std::max(-1, int(entry.get("deepen_stage", -1)));
		// B5: tipo de infra da região. Save v20 e anteriores sem a chave: -1.
		project.infra_type = std::max(-1,
			std::min(int(entry.get("infra_type", -1)), kInfraTypeCount - 1));
	// G2 (v25): obra de armazém. Save v24 e anteriores não têm as chaves:
	// -1 = não é armazém (nada inventado para trás).
	project.storage_type = std::max(-1,
		std::min(int(entry.get("storage_type", -1)), kStorageTypeCount - 1));
	project.storage_capacity_t = safe_nonneg(
		double(entry.get("storage_capacity_t", 0.0)));
	// PARTE E (v35): obra de energia. Save v34 e anteriores não têm as chaves:
	// -1/0 = não é obra de energia (nada inventado para trás).
	project.power_fuel = std::max(-1,
		std::min(int(entry.get("power_fuel", -1)), kPowerFuelCount - 1));
	project.power_mw = safe_nonneg(double(entry.get("power_mw", 0.0)));
	project.power_nation_a = std::string(
		String(entry.get("power_nation_a", "")).utf8().get_data());
	project.power_nation_b = std::string(
		String(entry.get("power_nation_b", "")).utf8().get_data());
		const Array materials = entry.get("materials", Array());
		const Array material_qty = entry.get("material_qty", Array());
		const Array material_delivered = entry.get("material_delivered", Array());
		project.material_count = std::min(
			static_cast<int>(materials.size()), kMaxConstructionMaterials);
		for (int m = 0; m < project.material_count; ++m) {
			int product = int(materials[m]);
			if (has_saved_products || needs_legacy_migration) {
				product = migrate_index(product);
			}
			project.materials[m] = product;
			project.material_qty[m] = m < material_qty.size()
				? std::max(0.0, double(material_qty[m])) : 0.0;
			project.material_delivered[m] = m < material_delivered.size()
				? std::max(0.0, double(material_delivered[m])) : 0.0;
		}
		ws.construction_projects.push_back(std::move(project));
	}

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
	// PARTE E (v35/D-116) - ENERGIA. v34 e anteriores chegam SEM o bloco:
	// `power_ready = false`, sem parque e sem rede (ausência declarada); o
	// PowerService aplica o dado público depois do load e semeia o parque.
	// O `power_cities` é paralelo a `cities`: o save guarda o id e a carga
	// remapeia para o índice atual (cidade que sumiu fica sem estado).
	// O `power_cities` é paralelo a `cities` SÓ quando o save tem o bloco
	// (v35+); sem o bloco o vetor fica VAZIO, como num mundo novo — senão
	// salvar→carregar→salvar mudaria o hash do save (estado que nunca foi
	// semeado apareceria como rede "isolada"). v34 e anteriores seguem o
	// caminho: o PowerService semeia o parque depois do load.
	if (data.has("power")) {
		ws.power_cities.assign(ws.cities.size(), PowerCityState());
	} else {
		ws.power_cities.clear();
	}
	ws.power_plants.clear();
	ws.power_grids.clear();
	ws.power_cash = 0.0;
	ws.power_ready = false;
	ws.power_params = PowerParams{};
	// E5: o estado de interconexões é do save; v35 chega sem a chave e o
	// PowerService semeia o ano-base depois do load.
	ws.interconnects.clear();
	ws.power_interconnects_ready = false;
	if (data.has("power")) {
		const Dictionary block = data.get("power", Dictionary());
		ws.power_cash = double(block.get("cash", 0.0));
		ws.power_ready = bool(block.get("ready", false));
		const Value& policy_variant = block.get("policy", Value());
		if (policy_variant.get_type() == Variant::DICTIONARY) {
			const Dictionary policy = policy_variant;
			std::map<std::string, double> param_map;
			for (const auto& row : policy.entries()) {
				param_map[row.first] = double(row.second);
			}
			ws.power_params.apply(param_map);
		}
		std::map<std::string, std::size_t> city_index;
		for (std::size_t index = 0; index < ws.cities.size(); ++index) {
			city_index[ws.cities[index].id] = index;
		}
		const Array saved_cities = block.get("cities", Array());
		for (int index = 0; index < saved_cities.size(); ++index) {
			if (saved_cities[index].get_type() != Variant::DICTIONARY) {
				continue;
			}
			const Dictionary entry = saved_cities[index];
			const std::string city_id = std::string(
				String(entry.get("city", "")).utf8().get_data());
			const auto found = city_index.find(city_id);
			if (found == city_index.end()) {
				continue;
			}
			PowerCityState& state = ws.power_cities[found->second];
			state.connection = std::clamp(int(entry.get("connection", 0)),
				0, static_cast<int>(POWER_CONNECTED));
			state.line_mw = std::max(0.0, double(entry.get("line_mw", 0.0)));
			state.has_data = bool(entry.get("has_data", false));
			state.access_pct = std::clamp(
				double(entry.get("access_pct", 100.0)), 0.0, 100.0);
			state.loss_pct = std::clamp(
				double(entry.get("loss_pct", 10.0)), 0.0, 100.0);
			const Array cap = entry.get("cap_mw", Array());
			for (int fuel = 0; fuel < kPowerFuelCount && fuel < cap.size();
					++fuel) {
				state.cap_mw[fuel] = std::max(0.0, double(cap[fuel]));
			}
			const Array solar = entry.get("solar_month", Array());
			for (int month = 0; month < kPowerMonthCount
					&& month < solar.size(); ++month) {
				state.solar_month[month] = std::clamp(
					double(solar[month]), 0.0, 1.0);
			}
			// D-122 (v38): sem a chave (save antigo) fica 0 = sem vento local.
			state.wind_ms = std::max(0.0,
				double(entry.get("wind_ms", 0.0)));
			// Save v35 antigo (sem âncora): base 0 mantém a curva global e
			// cf_scale fica 1,0 (default do struct). Saves novos preservam.
			state.demand_base_mwh = std::max(0.0,
				double(entry.get("demand_base_mwh", 0.0)));
			state.demand_income0 = std::max(0.0,
				double(entry.get("demand_income0", 0.0)));
			state.demand_loss0 = std::clamp(
				double(entry.get("demand_loss0", 0.0)), 0.0, 100.0);
			const Array cf_scale = entry.get("cf_scale", Array());
			for (int fuel = 0; fuel < kPowerFuelCount
					&& fuel < cf_scale.size(); ++fuel) {
				state.cf_scale[fuel] = std::clamp(
					double(cf_scale[fuel]), 0.0, 5.0);
			}
			state.demand_mwh = std::max(0.0,
				double(entry.get("demand_mwh", 0.0)));
			state.served_mwh = std::max(0.0,
				double(entry.get("served_mwh", 0.0)));
			state.price = std::max(0.0, double(entry.get("price", 0.0)));
			// E7 (v37): save v36 chega sem as chaves; 0 = a tarifa do próximo
			// tick usa o atacado médio derivado, nada inventado para trás.
			state.average_cost = std::max(0.0,
				double(entry.get("average_cost", 0.0)));
			state.tariff = std::max(0.0,
				double(entry.get("tariff", 0.0)));
			state.scarcity_pass = std::max(0.0,
				double(entry.get("scarcity_pass", 0.0)));
			state.ration_days = std::max(0,
				int(entry.get("ration_days", 0)));
			state.lost_gdp = std::max(0.0,
				double(entry.get("lost_gdp", 0.0)));
			City& loaded_city = ws.cities[found->second];
			loaded_city.power_stress = std::clamp(
				double(entry.get("stress", 0.0)), 0.0, 1.0);
			loaded_city.power_industry_factor = std::clamp(
				double(entry.get("industry_factor", 1.0)), 0.0, 1.0);
			loaded_city.power_service_factor = std::clamp(
				double(entry.get("service_factor", 1.0)), 0.0, 1.0);
			loaded_city.power_served_ratio = std::clamp(
				double(entry.get("served_ratio", 1.0)), 0.0, 1.0);
			loaded_city.power_price = std::max(0.0,
				double(entry.get("power_price", 0.0)));
			loaded_city.power_cost_industry = std::max(0.0,
				double(entry.get("power_cost_industry", 0.0)));
			loaded_city.power_cost_per_value = std::max(0.0,
				double(entry.get("power_cost_per_value", 0.0)));
		}
		const Array plants = block.get("plants", Array());
		for (int index = 0; index < plants.size(); ++index) {
			if (plants[index].get_type() != Variant::DICTIONARY) {
				continue;
			}
			const Dictionary entry = plants[index];
			PowerPlant plant;
			plant.id = std::string(
				String(entry.get("id", "")).utf8().get_data());
			if (plant.id.empty()) {
				continue;
			}
			plant.city_index = int(entry.get("city_index", -1));
			plant.fuel = std::clamp(int(entry.get("fuel", 0)), 0,
				kPowerFuelCount - 1);
			plant.mw = std::max(0.0, double(entry.get("mw", 0.0)));
			plant.owner_group = std::string(
				String(entry.get("owner_group", "")).utf8().get_data());
			plant.owner_nation = std::string(
				String(entry.get("owner_nation", "")).utf8().get_data());
			plant.capex = std::max(0.0, double(entry.get("capex", 0.0)));
			plant.opex_per_mwh = std::max(0.0,
				double(entry.get("opex_per_mwh", 0.0)));
			plant.jobs = std::max(0.0, double(entry.get("jobs", 0.0)));
			plant.wage = std::max(0.0, double(entry.get("wage", 0.0)));
			plant.built_day = int(entry.get("built_day", 0));
			plant.initial = bool(entry.get("initial", false));
			// D-122 (v38): sem a chave (save antigo) fica 0 = calibrado pela
			// Ember (a usina antiga continua reproduzindo o ano-base).
			plant.cf_base = std::clamp(
				double(entry.get("cf_base", 0.0)), 0.0, 1.0);
			plant.generated_mwh = std::max(0.0,
				double(entry.get("generated_mwh", 0.0)));
			if (version >= 41) {
				plant.loss_days = std::max(0,
					int(entry.get("loss_days", 0)));
				plant.mothballed = bool(entry.get("mothballed", false));
				plant.mothball_day = int(entry.get("mothball_day", -1));
				plant.mothball_unpaid_days = std::max(0,
					int(entry.get("mothball_unpaid_days", 0)));
				plant.crisis_stage = std::clamp(
					int(entry.get("crisis_stage", 0)), 0, 6);
				plant.last_layoff_day = std::max(-1000000,
					int(entry.get("last_layoff_day", -1000000)));
				plant.reopen_signal_days = std::max(0,
					int(entry.get("reopen_signal_days", 0)));
				plant.production_efficiency = std::clamp(
					double(entry.get("production_efficiency", 1.0)),
					0.0, 1.0);
			}
			ws.power_plants.push_back(std::move(plant));
		}
		const Value& grids_variant = block.get("grids", Value());
		if (grids_variant.get_type() == Variant::DICTIONARY) {
			const Dictionary grids = grids_variant;
			for (const auto& row : grids.entries()) {
				if (row.first.empty()
						|| row.second.get_type() != Variant::DICTIONARY) {
					continue;
				}
				const Dictionary entry = row.second;
				PowerGridState grid;
				grid.nation = row.first;
				grid.exists = bool(entry.get("exists", false));
				grid.price = std::max(0.0,
					double(entry.get("price", 0.0)));
				grid.average_cost = std::max(0.0,
					double(entry.get("average_cost", 0.0)));
				grid.tariff = std::max(0.0,
					double(entry.get("tariff", 0.0)));
				grid.scarcity_pass = std::max(0.0,
					double(entry.get("scarcity_pass", 0.0)));
				grid.retail_margin = double(
					entry.get("retail_margin", 0.0));
				grid.demand_mwh = std::max(0.0,
					double(entry.get("demand_mwh", 0.0)));
				grid.served_mwh = std::max(0.0,
					double(entry.get("served_mwh", 0.0)));
				grid.ration_days = std::max(0,
					int(entry.get("ration_days", 0)));
				grid.lost_gdp = std::max(0.0,
					double(entry.get("lost_gdp", 0.0)));
				grid.last_build_day = int(entry.get("last_build_day", -1000000));
				// E5 (v36): ledger do comércio. Save v35 chega sem as chaves
				// (0 = sem comércio no dia — nada inventado para trás).
				grid.import_mwh = std::max(0.0,
					double(entry.get("import_mwh", 0.0)));
				grid.export_mwh = std::max(0.0,
					double(entry.get("export_mwh", 0.0)));
				grid.import_value = std::max(0.0,
					double(entry.get("import_value", 0.0)));
				grid.export_value = std::max(0.0,
					double(entry.get("export_value", 0.0)));
				grid.interconnect_count = std::max(0,
					int(entry.get("interconnect_count", 0)));
				// D-122 (v38): curtailment do dia; save antigo fica 0.
				grid.curtailment_mwh = std::max(0.0,
					double(entry.get("curtailment_mwh", 0.0)));
				const Array by_fuel = entry.get("generation_by_fuel", Array());
				for (int fuel = 0; fuel < kPowerFuelCount
						&& fuel < by_fuel.size(); ++fuel) {
					grid.generation_by_fuel[fuel] = std::max(0.0,
						double(by_fuel[fuel]));
				}
				ws.power_grids[row.first] = grid;
			}
		}
		// E5 (v36): interconexões. Save v35 chega sem a chave: `ready` fica
		// falso e o PowerService semeia as linhas do ano-base depois do load.
		ws.power_interconnects_ready = bool(
			block.get("interconnects_ready", false));
		const Array links = block.get("interconnects", Array());
		for (int index = 0; index < links.size(); ++index) {
			if (links[index].get_type() != Variant::DICTIONARY) {
				continue;
			}
			const Dictionary entry = links[index];
			InterconnectState line;
			line.a = std::string(
				String(entry.get("a", "")).utf8().get_data());
			line.b = std::string(
				String(entry.get("b", "")).utf8().get_data());
			if (line.a.empty() || line.b.empty() || line.a == line.b) {
				continue;
			}
			if (line.b < line.a) {
				std::swap(line.a, line.b);
			}
			line.capacity_mw = std::max(0.0,
				double(entry.get("capacity_mw", 0.0)));
			line.built_day = int(entry.get("built_day", 0));
			line.initial = bool(entry.get("initial", false));
			line.contract_importer = std::string(
				String(entry.get("contract_importer", "")).utf8().get_data());
			line.contract_exporter = std::string(
				String(entry.get("contract_exporter", "")).utf8().get_data());
			line.contract_mw = std::max(0.0,
				double(entry.get("contract_mw", 0.0)));
			line.contract_flow_mwh = std::max(0.0,
				double(entry.get("contract_flow_mwh", 0.0)));
			line.contract_sent_mwh = std::max(0.0,
				double(entry.get("contract_sent_mwh", 0.0)));
			line.contract_price = std::max(0.0,
				double(entry.get("contract_price", 0.0)));
			line.contract_value = std::max(0.0,
				double(entry.get("contract_value", 0.0)));
			line.importer_today = std::string(
				String(entry.get("importer_today", "")).utf8().get_data());
			line.exporter_today = std::string(
				String(entry.get("exporter_today", "")).utf8().get_data());
			line.flow_mwh = std::max(0.0,
				double(entry.get("flow_mwh", 0.0)));
			line.sent_mwh = std::max(0.0,
				double(entry.get("sent_mwh", 0.0)));
			line.price = std::max(0.0, double(entry.get("price", 0.0)));
			line.value = std::max(0.0, double(entry.get("value", 0.0)));
			ws.interconnects.push_back(line);
		}
	}
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
	ws.service_businesses.clear();
	ws.next_service_business_id = 1;
	ws.service_bankruptcies = int(data.get("service_bankruptcies", 0));
	const Array saved_businesses = data.get("service_businesses", Array());
	for (int index = 0; index < saved_businesses.size(); ++index) {
		if (saved_businesses[index].get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary entry = saved_businesses[index];
		ServiceBusiness business;
		business.id = int(entry.get("id", 0));
		business.sector = int(entry.get("sector", -1));
		business.city = std::string(
			String(entry.get("city", "")).utf8().get_data());
		business.owner_stratum = std::clamp(
			int(entry.get("owner_stratum", kEducationLevels - 1)),
			0, kEducationLevels - 1);
		business.capacity = safe_nonneg(double(entry.get("capacity", 0.0)));
		business.jobs = safe_nonneg(double(entry.get("jobs", 0.0)));
		business.workers = safe_nonneg(double(entry.get("workers", 0.0)));
		business.capital = safe_nonneg(double(entry.get("capital", 0.0)));
		business.capital_left = safe_nonneg(double(entry.get("capital_left", 0.0)));
		business.utilization = std::clamp(
			double(entry.get("utilization", 0.0)), 0.0, 1.0);
		business.revenue = double(entry.get("revenue", 0.0));
		business.wages = safe_nonneg(double(entry.get("wages", 0.0)));
		business.input_cost = safe_nonneg(double(entry.get("input_cost", 0.0)));
		business.service_cost = safe_nonneg(double(entry.get("service_cost", 0.0)));
		business.profit = double(entry.get("profit", 0.0));
		business.loss_days = std::max(0, int(entry.get("loss_days", 0)));
		business.bankrupt = bool(entry.get("bankrupt", false));
		business.opened_day = int(entry.get("opened_day", 0));
		const double seed = double(entry.get("name_seed", 0.0));
		business.name_seed = seed > 0.0
			? static_cast<unsigned long long>(seed) : 0ULL;
		if (business.name_seed == 0ULL) {
			unsigned long long hash = 1469598103934665603ULL;
			for (const char c : business.city + "|" + std::to_string(business.id)) {
				hash ^= static_cast<unsigned char>(c);
				hash *= 1099511628211ULL;
			}
			business.name_seed = hash;
		}
		ws.next_service_business_id = std::max(
			ws.next_service_business_id, business.id + 1);
		ws.service_businesses.push_back(std::move(business));
	}
	if (ws.service_businesses.empty() && ws.services_loaded) {
		core.seed_services();
	} else {
		core.rebuild_service_indexes();
	}
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
	if (data.has("urban")) {
		const Array saved_urban = data.get("urban", Array());
		ws.land.clear();
		ws.housing.clear();
		ws.city_gov.clear();
		if (!saved_urban.is_empty()) {
			ws.land.assign(ws.cities.size(), CityLand{});
			ws.housing.assign(ws.cities.size(), CityHousing{});
			ws.city_gov.assign(ws.cities.size(), CityGov{});
		}
		for (int index = 0; index < saved_urban.size(); ++index) {
			if (saved_urban[index].get_type() != Variant::DICTIONARY) {
				continue;
			}
			const Dictionary entry = saved_urban[index];
			const std::string city_id = std::string(
				String(entry.get("city", "")).utf8().get_data());
			const auto found = ws.city_lookup.find(city_id);
			if (found == ws.city_lookup.end() || found->second < 0
					|| static_cast<std::size_t>(found->second)
						>= ws.cities.size()) {
				continue;
			}
			const std::size_t city_index =
				static_cast<std::size_t>(found->second);
			CityLand& land = ws.land[city_index];
			CityHousing& house = ws.housing[city_index];
			CityGov& gov = ws.city_gov[city_index];
			const Array area = entry.get("area_km2", Array());
			const Array max_density = entry.get("max_density", Array());
			const Array land_price = entry.get("land_price", Array());
			for (int zone = 0; zone < ZONE_COUNT; ++zone) {
				land.area_km2[zone] = zone < area.size()
					? safe_nonneg(double(area[zone])) : 0.0;
				land.max_density[zone] = zone < max_density.size()
					? static_cast<Density>(std::clamp(
						int(max_density[zone]), 0, DENSITY_COUNT - 1))
					: DENSITY_LOW;
				land.land_price[zone] = zone < land_price.size()
					? safe_nonneg(double(land_price[zone])) : 0.0;
			}
			land.reserve_km2 = safe_nonneg(
				double(entry.get("reserve_km2", 0.0)));
			land.infra_capacity = safe_nonneg(
				double(entry.get("infra_capacity", 0.0)));
			land.informal_km2 = safe_nonneg(
				double(entry.get("informal_km2", 0.0)));
			const Array zone_demand = entry.get("zone_demand", Array());
			for (int zone = 0; zone < ZONE_COUNT; ++zone) {
				land.zone_demand[zone] = zone < zone_demand.size()
					? safe_nonneg(double(zone_demand[zone])) : 0.0;
			}
			// Q-015-C: fração do envelope construída. Save anterior ao campo
			// (ou cenário sintético) = 1,0: comportamento anterior, sem
			// recalibrar capacidade de mundo já em andamento.
			const Array zone_fill = entry.get("zone_fill", Array());
			for (int zone = 0; zone < ZONE_COUNT; ++zone) {
				const double fill = zone < zone_fill.size()
					? safe_nonneg(double(zone_fill[zone])) : 1.0;
				land.zone_fill[zone] = fill > 0.0 ? fill : 1.0;
			}
			// §19 (integração): base do custo crescente. Save anterior ao campo
			// deriva do estado atual (área/reserva de hoje = base do próximo
			// carregamento): o custo volta a subir a partir de agora em vez de
			// inventar um raio original que ninguém mediu.
			land.initial_area_km2 = safe_nonneg(
				double(entry.get("initial_area_km2", 0.0)));
			land.initial_reserve_km2 = safe_nonneg(
				double(entry.get("initial_reserve_km2", 0.0)));
			if (land.initial_area_km2 <= 0.0) {
				double area_total = 0.0;
				for (int zone = 0; zone < ZONE_COUNT; ++zone) {
					area_total += safe_nonneg(land.area_km2[zone]);
				}
				land.initial_area_km2 = area_total;
			}
			if (land.initial_reserve_km2 <= 0.0) {
				land.initial_reserve_km2 = land.reserve_km2;
			}
			const Array units = entry.get("units", Array());
			for (int density = 0; density < DENSITY_COUNT; ++density) {
				if (density >= units.size()
						|| units[density].get_type() != Variant::ARRAY) {
					continue;
				}
				const Array row = units[density];
				for (int tier = 0; tier < TIER_COUNT; ++tier) {
					house.units[density][tier] = tier < row.size()
						? safe_nonneg(double(row[tier])) : 0.0;
				}
			}
			house.condition = safe_unit(
				double(entry.get("condition", 0.0)));
			const Array owner_share = entry.get("owner_share", Array());
			for (int owner = 0; owner < OWNER_COUNT; ++owner) {
				house.owner_share[owner] = owner < owner_share.size()
					? safe_nonneg(double(owner_share[owner])) : 0.0;
			}
			const Array tenure = entry.get("tenure", Array());
			for (int tier = 0; tier < TIER_COUNT; ++tier) {
				if (tier >= tenure.size()
						|| tenure[tier].get_type() != Variant::ARRAY) {
					continue;
				}
				const Array row = tenure[tier];
				for (int kind = 0; kind < TENURE_COUNT; ++kind) {
					house.tenure[tier][kind] = kind < row.size()
						? safe_nonneg(double(row[kind])) : 0.0;
				}
			}
			const Array rent = entry.get("rent", Array());
			const Array price = entry.get("price", Array());
			const Array vacancy = entry.get("vacancy", Array());
			for (int tier = 0; tier < TIER_COUNT; ++tier) {
				house.rent[tier] = tier < rent.size()
					? safe_nonneg(double(rent[tier])) : 0.0;
				house.price[tier] = tier < price.size()
					? safe_nonneg(double(price[tier])) : 0.0;
				house.vacancy[tier] = tier < vacancy.size()
					? safe_nonneg(double(vacancy[tier])) : 0.0;
			}
			house.informal_units = safe_nonneg(
				double(entry.get("informal_units", 0.0)));
			const Dictionary gov_entry = entry.get("gov", Dictionary());
			gov.cash = double(gov_entry.get("cash", 0.0));
			gov.iptu_rate = safe_unit(
				double(gov_entry.get("iptu_rate", 0.0)));
			gov.fee_rate = safe_unit(
				double(gov_entry.get("fee_rate", 0.0)));
			gov.transfer_share = safe_unit(
				double(gov_entry.get("transfer_share", 0.0)));
			gov.month_revenue_iptu = safe_nonneg(
				double(gov_entry.get("month_revenue_iptu", 0.0)));
			gov.month_revenue_fees = safe_nonneg(
				double(gov_entry.get("month_revenue_fees", 0.0)));
			gov.month_revenue_transfer = safe_nonneg(
				double(gov_entry.get("month_revenue_transfer", 0.0)));
			gov.month_expense_maintenance = safe_nonneg(
				double(gov_entry.get("month_expense_maintenance", 0.0)));
			gov.month_expense_infrastructure = safe_nonneg(
				double(gov_entry.get("month_expense_infrastructure", 0.0)));
			gov.month_expense_social_housing = safe_nonneg(
				double(gov_entry.get("month_expense_social_housing", 0.0)));
		}
	}

	// §19 (W2): prefeitura em operação (bloco próprio). Save v13 anterior ao W2
	// não tem a chave: fica sem repasse calibrado e sem obra, que é o estado
	// honesto (a próxima carga dos JSON preenche revenue_pct_gdp).
	if (data.has("city_gov")) {
		const Array saved_city_gov = data.get("city_gov", Array());
		for (int index = 0; index < saved_city_gov.size(); ++index) {
			if (saved_city_gov[index].get_type() != Variant::DICTIONARY) {
				continue;
			}
			const Dictionary entry = saved_city_gov[index];
			const std::string city_id = std::string(
				String(entry.get("city", "")).utf8().get_data());
			const auto found = ws.city_lookup.find(city_id);
			if (found == ws.city_lookup.end() || found->second < 0
					|| static_cast<std::size_t>(found->second)
						>= ws.city_gov.size()) {
				continue;
			}
			CityGov& gov = ws.city_gov[static_cast<std::size_t>(found->second)];
			gov.revenue_pct_gdp = safe_unit(
				double(entry.get("revenue_pct_gdp", 0.0)));
			gov.target_revenue_month = safe_nonneg(
				double(entry.get("target_revenue_month", 0.0)));
			gov.month_transfer_paid = safe_nonneg(
				double(entry.get("month_transfer_paid", 0.0)));
			// A6: janela do fechamento mensal (save v14 não tem: 0 = primeira
			// janela, que a fase escala pelos dias decorridos desde o dia 0).
			gov.month_marker = std::max(
				0, int(entry.get("month_marker", 0)));
			const Array projects = entry.get("projects", Array());
			gov.project_count = 0;
			for (int slot = 0; slot < projects.size()
					&& gov.project_count < kCityGovMaxProjects; ++slot) {
				if (projects[slot].get_type() != Variant::DICTIONARY) {
					continue;
				}
				const Dictionary row = projects[slot];
				CityGovProject& project = gov.projects[gov.project_count];
				project = CityGovProject{};
				project.kind = static_cast<UrbanProjectKind>(std::clamp(
					int(row.get("kind", 0)), 0, URBAN_PROJECT_COUNT - 1));
				project.zone = std::clamp(int(row.get("zone", 0)), 0,
					ZONE_COUNT - 1);
				project.area_km2 = safe_nonneg(double(row.get("area_km2", 0.0)));
				project.units = safe_nonneg(double(row.get("units", 0.0)));
				project.cost = safe_nonneg(double(row.get("cost", 0.0)));
				project.reserved = safe_nonneg(double(row.get("reserved", 0.0)));
				project.days_left = std::max(0,
					int(row.get("days_left", 0)));
				project.created_day = int(row.get("created_day", 0));
				if (project.days_left <= 0) {
					continue;
				}
				++gov.project_count;
			}
		}
	}

	// §19 W3 — estado do mercado de moradia (bloco `housing`, aditivo). Save
	// anterior ao W3 não tem a chave: os campos ficam no padrão (0 = mercado
	// ainda não rodou) e a fase deriva a base de domicílios do estoque.
	if (data.has("housing")) {
		const Array saved_housing = data.get("housing", Array());
		for (int index = 0; index < saved_housing.size(); ++index) {
			if (saved_housing[index].get_type() != Variant::DICTIONARY) {
				continue;
			}
			const Dictionary entry = saved_housing[index];
			const std::string city_id = std::string(
				String(entry.get("city", "")).utf8().get_data());
			const auto found = ws.city_lookup.find(city_id);
			if (found == ws.city_lookup.end() || found->second < 0
					|| static_cast<std::size_t>(found->second)
						>= ws.housing.size()) {
				continue;
			}
			CityHousing& house = ws.housing[
				static_cast<std::size_t>(found->second)];
			house.households = safe_nonneg(
				double(entry.get("households", 0.0)));
			const Array hh_tier = entry.get("households_by_tier", Array());
			for (int tier = 0; tier < TIER_COUNT; ++tier) {
				house.households_by_tier[tier] = tier < hh_tier.size()
					? safe_nonneg(double(hh_tier[tier])) : 0.0;
			}
			house.pop_anchor = safe_nonneg(
				double(entry.get("pop_anchor", 0.0)));
			house.rent_anchored = bool(entry.get("rent_anchored", false));
			const Array cap_ratio = entry.get("rent_cap_ratio", Array());
			for (int tier = 0; tier < TIER_COUNT; ++tier) {
				house.rent_cap_ratio[tier] = tier < cap_ratio.size()
					? std::clamp(double(cap_ratio[tier]), 1.0, 2.5) : 1.0;
			}
			house.housing_stress = safe_unit(
				double(entry.get("housing_stress", 0.0)));
			house.stress_days = std::max(
				0, int(entry.get("stress_days", 0)));
			house.rent_paid_month = safe_nonneg(
				double(entry.get("rent_paid_month", 0.0)));
			house.rent_paid_today = safe_nonneg(
				double(entry.get("rent_paid_today", 0.0)));
			house.rent_households_month = safe_nonneg(
				double(entry.get("rent_households_month", 0.0)));
			house.rent_firms_month = safe_nonneg(
				double(entry.get("rent_firms_month", 0.0)));
			house.rent_government_month = safe_nonneg(
				double(entry.get("rent_government_month", 0.0)));
			house.rent_burden = safe_unit(
				double(entry.get("rent_burden", 0.0)));
			house.units_built_month = safe_nonneg(
				double(entry.get("units_built_month", 0.0)));
			house.month_marker = std::max(
				0, int(entry.get("month_marker", 0)));
			// save v14: a obra de moradia vive em `construction_projects`
			// (kind CONSTRUCTION_HOUSING). Save v13 e anteriores guardavam a
			// lista em `housing[].projects`: cada obra antiga é MIGRADA para a
			// fila única com o prazo RESTANTE e custo 0 — o capital já foi pago
			// na versão antiga, então a obra só cumpre o prazo, sem consumir
			// caixa nem capacidade (total_out = 0). Ordem determinística: índice
			// da entrada `housing` e depois o slot.
			if (version < 14) {
				const Array projects = entry.get("projects", Array());
				const int max_slots = std::min(
					static_cast<int>(projects.size()), kHousingMaxProjects);
				for (int slot = 0; slot < max_slots; ++slot) {
					if (projects[slot].get_type() != Variant::DICTIONARY) {
						continue;
					}
					const Dictionary p = projects[slot];
					const int state = std::clamp(
						int(p.get("state", 0)), 0,
						static_cast<int>(HOUSING_PROJECT_GOVERNMENT));
					if (state == HOUSING_PROJECT_IDLE) {
						continue;  // slot vazio no save antigo
					}
					ConstructionProject project;
					project.id = "HSG-V13-" + city_id + "-"
						+ std::to_string(slot);
					project.building_id = project.id;
					project.city = city_id;
					project.owner = state == HOUSING_PROJECT_GOVERNMENT
						? "state" : "private";
					project.output_product = -1;
					project.recipe = -1;
					project.input_product = -1;
					project.kind = CONSTRUCTION_HOUSING;
					project.housing_owner = state;
					project.housing_density = std::clamp(
						int(p.get("density", 0)), 0, DENSITY_COUNT - 1);
					project.housing_tier = std::clamp(
						int(p.get("tier", 0)), 0, TIER_COUNT - 1);
					project.housing_units = safe_nonneg(
						double(p.get("units", 0.0)));
					project.capacity = project.housing_units;
					project.total_cost = 0.0;
					project.reserved = 0.0;
					project.labor_cost_total = 0.0;
					project.material_count = 0;
					project.duration_days = std::max(
						1, int(p.get("days_left", 0)));
					project.days_elapsed = 0;
					project.created_day = ws.day;
					project.paused = false;
					ws.construction_projects.push_back(std::move(project));
				}
			}
		}
	}
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