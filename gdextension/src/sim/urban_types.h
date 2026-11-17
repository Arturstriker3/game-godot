#pragma once
// §19 (W0) — solo urbano, moradia e prefeitura por cidade.
// C++ puro (só STL): sem dependência do engine em sim/. As estruturas são
// AGREGADAS por cidade (sem agente por prédio) e vivem em WorldState em vetores
// paralelos a `cities` (mesmo índice). O W0 só define e carrega os dados;
// nenhuma fase diária os muta ainda (W1/W2/W3).

namespace ogs {

// Uso do solo. A ordem é a do save e dos arrays por zona — não reordene.
enum Zone : int {
	ZONE_RES = 0,   // residencial
	ZONE_COM = 1,   // comercial/serviços
	ZONE_IND = 2,   // industrial
	ZONE_COUNT = 3,
};

// Gabarito permitido por zona: casa (baixa), prédio (média), torre (alta).
enum Density : int {
	DENSITY_LOW = 0,
	DENSITY_MID = 1,
	DENSITY_HIGH = 2,
	DENSITY_COUNT = 3,
};

// Padrão de renda do estrato (liga ao POPULAR/MEDIO/ALTO da distribuição).
enum Tier : int {
	TIER_POPULAR = 0,
	TIER_MEDIO = 1,
	TIER_ALTO = 2,
	TIER_COUNT = 3,
};

// Dono do imóvel: família (POP), empresa/grupo ou governo (moradia social).
enum Owner : int {
	OWNER_HOUSEHOLD = 0,
	OWNER_FIRM = 1,
	OWNER_GOVERNMENT = 2,
	OWNER_COUNT = 3,
};

// Posse por estrato: própria, alugada ou informal (favela).
enum Tenure : int {
	TENURE_OWN = 0,
	TENURE_RENT = 1,
	TENURE_INFORMAL = 2,
	TENURE_COUNT = 3,
};

// Solo urbano de uma cidade (dado real do urban_land.json).
struct CityLand {
	// Área construída (km²) por zona. Soma = built_up_km2 da fonte.
	double area_km2[ZONE_COUNT] = {};
	// Gabarito máximo permitido em cada zona (height_class da fonte). No W0 a
	// mesma densidade vale para as três zonas; W1 pode diferenciar por zona.
	Density max_density[ZONE_COUNT] = {DENSITY_LOW, DENSITY_LOW, DENSITY_LOW};
	// Reserva expansível (km²), limitada pela geografia na fonte.
	double reserve_km2 = 0.0;
	// Preço do terreno por zona (XGS/m²). A fonte não publica preço de terra
	// por cidade: começa em 0 (ausência explícita) e o W1 o move com ocupação
	// e renda local.
	double land_price[ZONE_COUNT] = {};
	// População que a infraestrutura atual atende. No W0 = população de
	// simulação da cidade; expansão/adensamento do W2 alteram.
	double infra_capacity = 0.0;
	// Área informal (km²), derivada das unidades informais da fonte (W3 pode
	// urbanizar/regularizar e reduzi-la).
	double informal_km2 = 0.0;
};

// Estoque de moradia de uma cidade, agregado por densidade × padrão.
struct CityHousing {
	// Unidades por densidade e padrão (soma por padrão = housing_stock_by_tier).
	double units[DENSITY_COUNT][TIER_COUNT] = {};
	// Condição média do estoque (0..1). 1,0 = recém-construído; a depreciação
	// anual do W3 reduz.
	double condition = 1.0;
	// Fração do estoque por tipo de dono (soma 1). Household = casa própria da
	// fonte; o restante é atribuído a empresas até W2/W3 criarem moradia social.
	double owner_share[OWNER_COUNT] = {};
	// Posse por estrato: {própria, alugada, informal}; soma 1 por estrato.
	double tenure[TIER_COUNT][TENURE_COUNT] = {};
	// Aluguel mensal por unidade (XGS/unidade/mês) e valor de venda (XGS), por
	// padrão. Fonte em USD/m² × m² da unidade; 0 = sem dado (não se inventa).
	double rent[TIER_COUNT] = {};
	double price[TIER_COUNT] = {};
	// Unidades vazias por padrão. No W0 = 0 (estoque = domicílios da fonte);
	// o mercado do W3 passa a medir vacância.
	double vacancy[TIER_COUNT] = {};
	// Unidades informais (subconjunto do estoque, pela tenure informal).
	double informal_units = 0.0;
};

// Prefeitura de uma cidade. Receita = IPTU + taxas locais + repasse nacional;
// despesa = manutenção, infraestrutura/expansão e moradia social. No W0 só os
// parâmetros e o acumulador do mês existem (o fluxo é do W2).
struct CityGov {
	double cash = 0.0;
	// Taxa anual do IPTU sobre o valor do estoque (fração) e taxa local sobre
	// o faturamento de serviços da cidade (fração). Repasse como fração da
	// receita municipal alvo — sai do tesouro nacional.
	double iptu_rate = 0.0;
	double fee_rate = 0.0;
	double transfer_share = 0.0;
	// Acumulado do mês corrente por rubrica (zerado no fechamento do mês).
	double month_revenue_iptu = 0.0;
	double month_revenue_fees = 0.0;
	double month_revenue_transfer = 0.0;
	double month_expense_maintenance = 0.0;
	double month_expense_infrastructure = 0.0;
	double month_expense_social_housing = 0.0;
};

} // namespace ogs
