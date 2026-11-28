#pragma once
// H1g — contrato INTERNO do save/load dividido por domínio.
//
// `save_state.cpp` continua dono de `to_value`/`from_value` e dos helpers
// compartilhados (namespace `detail`); cada TU em `sim/save/` recebe um bloco
// contíguo do arquivo original e escreve/lê as chaves NA MESMA ORDEM. O dump é
// um objeto com ordem de inserção (save_value.h), então reordenar aqui muda os
// bytes do save e o `save_sha256` do jogo. Não reordene nada.
//
// Os corpos foram movidos verbatim; os parâmetros têm os mesmos nomes das
// variáveis que o bloco já usava (ex.: `migrate_array`, `version`), para que as
// linhas movidas sigam byte-idênticas.

#include "../save_state.h"

#include <functional>

namespace ogs::save {

namespace detail {

Dictionary make_object();
Array make_array();
String sim_str(const char* utf8);
double safe_nonneg(double v);
double safe_unit(double v);
double read_sim_population(const Dictionary& data, double population);
// R2b1 (fix A1): claims de divida por credor (ordenados por `bank_id`).
void write_bank_claims(Array& out, const std::vector<BankLoan>& claims);
void load_bank_claims(std::vector<BankLoan>& out, const Array& rows);

Dictionary save_informal_block(double share, const double* labor_share,
		const double* income, double wage_bill, double workers, double output,
		double demand, double revenue, double price, double service_share);
Dictionary save_mortality_block(double deaths_expected, double deaths_total,
		double excess_total, double excess_famine_total, int famine_months,
		double food_price_ref, double food_inflation_month, double health_access_ref,
		double satisfaction_prev);
Dictionary save_dissatisfaction_block(double index, const double* by_stratum,
		double unemployment, double rent, double food, double inflation,
		double informality);
void load_informal_block(const Dictionary& d, double& share,
		double* labor_share, double* income, double& wage_bill, double& workers,
		double& output, double& demand, double& revenue, double& price,
		double& service_share);
void load_mortality_block(const Dictionary& d, double& deaths_expected,
		double& deaths_total, double& excess_total, double& excess_famine_total,
		int& famine_months, double& food_price_ref, double& food_inflation_month,
		double& health_access_ref, double& satisfaction_prev);
void load_dissatisfaction_block(const Dictionary& d, double& index,
		double* by_stratum, double& unemployment, double& rent, double& food,
		double& inflation, double& informality);

// F0 (save v39) — frações de migração do combustível genérico (mesmos valores
// e tipo do antigo `constexpr` no TU de `save_state.cpp`; agora no header para
// o TU de cidade usar a MESMA definição).
inline constexpr double kLegacyFuelGasolineShare = 45.0 / 83.0;
inline constexpr double kLegacyFuelDieselShare = 29.0 / 83.0;
inline constexpr double kLegacyFuelJetShare = 9.0 / 83.0;
// Aliases de receita do combustível genérico (v38 e anteriores).
const char* legacy_recipe_alias(const std::string& id);

} // namespace detail

namespace city {

// `cities`, `buildings`, `construction_projects`, `urban`, `city_gov`,
// `housing`, `service_businesses` e `service_bankruptcies`.
void write(Dictionary& data, const WorldState& ws, const SimCore& core);

void read(WorldState& ws, SimCore& core, const Dictionary& data, int version,
		const std::function<Array(const Array&)>& migrate_array,
		const std::function<int(int)>& migrate_index,
		const std::function<int(int)>& legacy_substitute,
		bool has_saved_products, bool needs_legacy_migration,
		int legacy_fuel_index, int legacy_gasoline, int legacy_diesel,
		int legacy_jet);
void read_services(WorldState& ws, SimCore& core, const Dictionary& data);
void read_urban(WorldState& ws, const Dictionary& data);
void read_city_gov(WorldState& ws, const Dictionary& data);
void read_housing(WorldState& ws, const Dictionary& data, int version);

} // namespace city

namespace pop {

void write(Dictionary& entry, const City& city, const WorldState& ws,
		const SimCore& core);
void read(Dictionary& entry, City& city, const WorldState& ws, SimCore& core,
		int version);
void read_categories(const Dictionary& entry, City& city,
		const WorldState& ws);

} // namespace pop

namespace region {

void write(Dictionary& entry, const City& city);
void read(Dictionary& entry, City& city, const WorldState& ws, SimCore& core,
		int version);

} // namespace region

namespace power {

void write(Dictionary& data, const WorldState& ws);
void read(WorldState& ws, const Dictionary& data, int version);

} // namespace power

namespace finance {

// `currencies`, `tariffs`, `monetary_policy`, `central_bank`, `monetary`, `fx`,
// `fx_regimes`, `banks`, `bank_crisis` e `stock` (ordem de escrita do save).
void write(Dictionary& data, const WorldState& ws);
void read_money(WorldState& ws, SimCore& core, const Dictionary& data);
void read_tariffs(WorldState& ws, SimCore& core, const Dictionary& data);
void read_stock(WorldState& ws, const Dictionary& data);
void read_bank_crisis(WorldState& ws, const Dictionary& data);

} // namespace finance

} // namespace ogs::save
