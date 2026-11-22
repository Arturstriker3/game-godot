#pragma once
// PARTE E / D-116 — energia elétrica: usinas, geração, redes por região/
// nacional, despacho por mérito, preço e consequências da falta.
//
// Modelo DETERMINÍSTICO (sem sorteio, ordem fixa por índice):
//   * eletricidade é FLUXO (não estocável); o preço é por REDE em XGS/MWh;
//   * a rede nacional só existe onde o dado público declara OPERADOR
//     NACIONAL, e inclui só as cidades CONECTADAS; cidade parcial troca com
//     a rede até `line_mw`; cidade isolada roda só com as próprias usinas;
//   * demanda = famílias (per capita × acesso) + indústria/serviços/outros
//     (participação declarada) + perdas T&D;
//   * despacho diário por custo marginal; falta = racionamento que limita a
//     produção industrial (quase proporcional), derruba o emprego, os
//     serviços e a satisfação (crescente com a duração), contrai o PIB e a
//     receita dos grupos pelos canais que já existem.
//
// Parâmetros: data/definitions/energy/power_params.json (default de jogo
// declarado, espelho aqui). Números novos NÃO são inventados como dado: são
// modelo declarado, com a fonte pública dos DADOS (acesso/perdas, parque,
// potencial) registrada em docs/economy/E2E3_power_grid.md.

#include "sim_constants.h"
#include "sim_types.h"

#include <map>
#include <string>
#include <vector>

namespace ogs {

struct WorldState;

// ---------------------------------------------------------------------------
// Parâmetros de REGRA (não vão ao save; 0 = default declarado daqui).
// ---------------------------------------------------------------------------
struct PowerParams {
	// Demanda: kWh por pessoa/dia no índice de renda 1,0, elasticidade-renda
	// e teto do índice (evita outlier de PPP per capita). Participação por
	// bloco (soma 1,0) — modelo declarado, não dado por país.
	double kwh_per_capita_day = 9.0;
	double income_elasticity = 1.0;
	double income_index_cap = 5.0;
	double share_households = 0.30;
	double share_industry = 0.42;
	double share_services = 0.21;
	double share_other = 0.07;
	double access_floor = 0.02;
	// Fator de capacidade por fonte (anual). Solar pode ser sobreposto pelo
	// PVOUT mensal do Global Solar Atlas (por cidade). Vento sem sazonalidade
	// (o GWA não publica série mensal) — dívida declarada.
	double cf[kPowerFuelCount] = {0.50, 0.22, 0.35, 0.60, 0.55,
		0.40, 0.85, 0.55, 0.45};
	// Perdas T&D: piso técnico > 0, teto do dado, redução pela infra B5.
	double loss_floor_pct = 4.0;
	double loss_max_pct = 40.0;
	double loss_infra_reduction = 0.25;
	// Rede: acesso (%) a partir do qual a nação declara operador nacional; e
	// os cortes do indicador urbano×rural para conectada/parcial.
	double operator_access_pct = 60.0;
	double connected_score = 80.0;
	double partial_score = 50.0;
	// Capacidade inicial da linha da cidade PARCIAL como fração do parque
	// local (MW) — modelo declarado; a obra (§15) troca o estado.
	double partial_line_share = 0.25;
	double partial_line_min_mw = 10.0;
	// Preço (XGS/MWh): escala do jogo, piso e teto de escassez declarados.
	double price_scale = 1.0;
	double price_floor = 0.05;
	double price_ceiling = 30.0;
	// Combustível por MWh de térmica (t/MWh) e prêmio do combustível
	// importado quando o mercado local não cobre a compra (declarado).
	double fuel_t_per_mwh[kPowerFuelCount] = {0.0, 0.0, 0.0, 0.00035,
		0.00018, 0.00023, 0.00000002, 0.0006, 0.0004};
	double fuel_import_premium = 1.5;
	// Consequências da falta: piso da indústria (geração própria/gerador a
	// diesel), piso quando há usina fóssil na própria cidade, fator de
	// serviços e sensibilidade/prazo da insatisfação.
	double industry_floor = 0.05;
	double self_generation_floor = 0.15;
	double service_factor_min = 0.5;
	// Repasse máximo do custo de energia ao preço industrial, como fração do
	// preço base do bem (modelo declarado): a conta da energia da cidade
	// dividida pelo valor da indústria local pode explodir onde a cidade tem
	// poucas plantas; o excedente fica como perda do setor, não vira preço.
	double industry_cost_share_max = 0.05;
	double stress_per_unserved = 4.0;
	double stress_duration_days = 10.0;
	// Racionamento: fração servida abaixo da qual o dia conta como blecaute.
	double ration_threshold = 0.995;
	// VOLL declarado (múltiplo do preço da rede) para o "PIB perdido".
	double voll_multiple = 100.0;
	// Obra da IA: dias de racionamento que disparam a decisão e o cooldown.
	int build_ration_days = 20;
	int build_cooldown_days = 60;
	// Capacidade nova máxima por decisão da IA, como fração da demanda da
	// rede, e a fração do parque atual usada como teto térmico (potencial).
	double build_share_demand = 0.08;
	double thermal_cap_multiple = 2.0;
	// Custo de obra declarado (XGS/MW) e prazo (dias) por fonte.
	double capex_per_mw[kPowerFuelCount] = {900.0, 700.0, 750.0, 1200.0,
		1000.0, 900.0, 2600.0, 800.0, 900.0};
	double opex_per_mwh[kPowerFuelCount] = {0.15, 0.10, 0.12, 0.25, 0.22,
		0.30, 0.35, 0.30, 0.30};
	int build_days_plant = 180;
	int build_days_line = 120;
	// Linha nova: capacidade em MW por custo (XGS/MW) e fração da demanda.
	double capex_per_mw_line = 150.0;
	double line_build_share_demand = 0.25;

	// Preenche os campos presentes no mapa (nomes = campos do JSON).
	void apply(const std::map<std::string, double>& values);
};

// Definições (dado público) por nação e por cidade.
struct PowerNationDef {
	double access_pct = 100.0;
	double access_urban_pct = 100.0;
	double access_rural_pct = 100.0;
	double loss_pct = 10.0;
	bool has_data = false;
	// Ancoragem nacional (D-116): demanda Ember do ano-base em MWh/dia, na
	// escala do mundo real (o núcleo multiplica pela fração simulada) e a
	// geração Ember do MESMO ano por fonte em MWh/ano. -1 = fonte não
	// publicada (sem calibração; fator global declarado); 0 = publicada como
	// zero (a fonte não gera naquele ano). Sem `has_demand`, a demanda usa a
	// curva global declarada (não há dado Ember para a nação).
	double demand_base_mwh_day = 0.0;
	double generation_mwh_year[kPowerFuelCount] = {};
	bool has_demand = false;

	PowerNationDef() {
		for (int fuel = 0; fuel < kPowerFuelCount; ++fuel) {
			generation_mwh_year[fuel] = -1.0;
		}
	}
};

struct PowerCityDef {
	double access_pct = 100.0;
	double loss_pct = 10.0;
	// Parque instalado inicial e teto (potencial) por fonte, MW.
	double park[kPowerFuelCount] = {};
	double cap[kPowerFuelCount] = {};
	// PVOUT mensal (kWh/kWp/dia) ÷ 24 h = fator de capacidade solar do mês.
	double solar_month[kPowerMonthCount] = {};
};

// E5 (D-117) — interconexão entre redes NACIONAIS, derivada do dado
// (data/definitions/energy/interconnects.json): par canônico `a < b`, a nação
// importadora do ano-base e a capacidade da linha em MW na escala do DADO
// (o seed multiplica pela fração simulada da nação importadora, como o parque).
struct InterconnectDef {
	std::string a;
	std::string b;
	std::string importer;
	double capacity_mw = 0.0;
};

// Nomes canônicos das fontes (ordem de `PowerFuel`): usado no status/save.
const char* power_fuel_name(int fuel);
// Índice da fonte pelo nome; -1 = desconhecida.
int power_fuel_from_name(const std::string& name);
// Fontes térmicas (compram combustível): carvão, gás, óleo, nuclear, biomassa.
bool power_fuel_is_thermal(int fuel);

// Aplica as DEFINIÇÕES e semeia o parque inicial. `only_missing` (default
// true) não sobrescreve o estado de um save.
void seed(WorldState& ws, const PowerParams& params,
	const std::map<std::string, PowerNationDef>& nations,
	const std::map<std::string, PowerCityDef>& cities, bool only_missing);

// E5 (D-117): semeia as interconexões do ano-base. Só entram pares cujas duas
// nações têm REDE NACIONAL (operador + ao menos uma cidade conectada) neste
// mundo; a capacidade é escalada pela fração simulada da nação importadora.
// `only_missing` preserva as linhas que vieram de um save (v36+).
void seed_interconnects(WorldState& ws,
	const std::vector<InterconnectDef>& defs, bool only_missing);

// Recalcula os índices derivados depois do load. Cidade sem estado de energia
// continua `power_ready = false` (ausência declarada).
void after_load(WorldState& ws);

// Mapa nome→valor dos parâmetros de REGRA (os mesmos nomes do
// data/definitions/energy/power_params.json). O save v35 guarda o mapa para o
// round-trip não perder a regra declarada; `PowerParams::apply` relê.
void power_params_to_map(const PowerParams& params,
	std::map<std::string, double>& out);

} // namespace ogs
