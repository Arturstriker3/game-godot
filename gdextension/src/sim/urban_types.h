#pragma once
// §19 (W0) — solo urbano, moradia e prefeitura por cidade.
// C++ puro (só STL): sem dependência do engine em sim/. As estruturas são
// AGREGADAS por cidade (sem agente por prédio) e vivem em WorldState em vetores
// paralelos a `cities` (mesmo índice). O W0 só define e carrega os dados;
// nenhuma fase diária os muta ainda (W1/W2/W3).

#include "housing.h"

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
	// §19 (W1 / Q-015-C) — fração do ENVELOPE construtivo (área × FAR ÷ m²)
	// efetivamente construída nesta cidade. O FAR de sim_constants.h é o
	// limite do lote; na escala da cidade só parte dele está construída
	// (ruas, pátios, construção antiga). Preenchido no loader para a MORADIA
	// de modo que a ocupação formal do dia 0 nasça no alvo da faixa de
	// população urbana (kUrbanBandOccupancy); COM/IND ficam em 1,0.
	// 1,0 = sem calibração (save antigo, cenário sintético): comportamento
	// anterior preservado.
	double zone_fill[ZONE_COUNT] = {1.0, 1.0, 1.0};
	// §19 (W1) — demanda de zona não atendida: quantas decisões (abrir negócio
	// de serviço, instalar fábrica) foram barradas por falta de espaço desde o
	// último fechamento de mês. É o sinal que a prefeitura (W2) lê para decidir
	// expandir/adensar; decai à metade por mês (demanda velha não manda).
	double zone_demand[ZONE_COUNT] = {};

	// ------------------------------------------------------------------
	// §19 (integração) — custo crescente do solo (spec, seção "Custo
	// crescente"). Guardam o tamanho da cidade no CARREGAMENTO: o custo de
	// cada km² novo sobe com o raio atual ÷ raio inicial (redes mais longas
	// até o centro) e com a fração da reserva já consumida (o que sobra é o
	// terreno pior — encosta, várzea). Preenchidos no loader e no save; 0 =
	// cidade sem dado (os fatores ficam em 1, sem inventar aumento).
	// ------------------------------------------------------------------
	double initial_area_km2 = 0.0;
	double initial_reserve_km2 = 0.0;
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

	// ------------------------------------------------------------------
	// §19 W3 — mercado de moradia (ver sim/housing.h para o modelo).
	// Bloco aditivo: save v13 grava estas chaves na entrada `housing`; save
	// anterior ao W3 carrega com os padrões (0 = mercado ainda não rodou, a
	// fase deriva a base do estoque no primeiro tick).
	// ------------------------------------------------------------------
	// Domicílios da cidade (base da demanda por padrão). A fonte define o
	// estoque como população ÷ tamanho do domicílio, então a fase o inicializa
	// com o estoque carregado — equilíbrio inicial explícito, sem inventar
	// tamanho de domicílio que a fonte já embutiu.
	double households = 0.0;
	// Domicílios por padrão (repartição do estoque da fonte, que É a
	// distribuição de renda) e população no carregamento — a demanda de cada
	// padrão escala com a população da cidade.
	double households_by_tier[TIER_COUNT] = {};
	double pop_anchor = 0.0;
	// O nível de aluguel da fonte (USD/m²) é reancorado UMA vez na renda
	// medida do núcleo; depois disso o mercado manda. `rent_cap_ratio` guarda o
	// gradiente de aluguel por padrão da fonte (médio/alto ÷ popular), usado
	// como teto de ônus de cada padrão.
	bool rent_anchored = false;
	double rent_cap_ratio[TIER_COUNT] = {1.0, 1.0, 1.0};
	// Sinal 0..1 de aperto habitacional (falta popular + ônus de aluguel acima
	// do teto). Insumo do W4 (migração) e da UI.
	double housing_stress = 0.0;
	// Dias consecutivos em aperto popular (a favela é o último caso).
	int stress_days = 0;
	// Aluguel efetivamente pago no dia (XGS) e acumulados do mês por dono.
	double rent_paid_today = 0.0;
	double rent_paid_month = 0.0;
	double rent_households_month = 0.0;
	double rent_firms_month = 0.0;
	double rent_government_month = 0.0;
	// Ônus do aluguel no mês (aluguel pago ÷ renda disponível do período).
	double rent_burden = 0.0;
	// Unidades entregues no mês corrente (auditoria/teste).
	double units_built_month = 0.0;
	// Dia do último fechamento mensal processado (ajuste de aluguel/obras).
	int month_marker = 0;
};

// §19 (W2) — obra urbana pedida pela prefeitura. O W2 DECIDE, financia
// (reserva o caixa do projeto) e acompanha o prazo; a execução física por
// materiais do §15 e a conclusão de solo são do W1 (sim/urban_land.cpp) e a
// construção de moradia é do W3 (sim/housing.cpp). Ver o ponto de integração em
// `city_gov.h` (`start_expansion`/`start_densify`/`start_social_housing`).
enum UrbanProjectKind : int {
	URBAN_PROJECT_EXPAND = 0,          // abrir `area_km2` de reserva numa zona
	URBAN_PROJECT_DENSIFY = 1,         // subir o gabarito de uma zona
	URBAN_PROJECT_SOCIAL_HOUSING = 2,  // unidades populares do governo
	URBAN_PROJECT_COUNT = 3,
};

// Slots de obra simultânea por cidade (uma expansão e uma moradia social).
constexpr int kCityGovMaxProjects = 2;

struct CityGovProject {
	UrbanProjectKind kind = URBAN_PROJECT_EXPAND;
	int zone = ZONE_RES;
	// Expansão: km² abertos. Moradia social: unidades populares entregues.
	double area_km2 = 0.0;
	double units = 0.0;
	// Orçamento da obra (XGS): `reserved` é o que ainda não foi pago e entra no
	// dinheiro do mundo (como o fundo das obras do §15).
	double cost = 0.0;
	double reserved = 0.0;
	int days_left = 0;
	int created_day = 0;
};

// Prefeitura de uma cidade. Receita = IPTU + taxas locais + repasse nacional;
// despesa = manutenção, infraestrutura/expansão e moradia social. O W0 define
// os parâmetros; o fluxo mensal e as obras são do W2 (sim/city_gov.cpp).
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
	// §19 (W2) — calibração e obras. `revenue_pct_gdp` é a receita subnacional
	// da fonte (municipal_finance.json, `subnational_revenue_pct_gdp`, fração do
	// PIB) que define o alvo da receita municipal do mês; 0 = sem dado (não se
	// inventa nível de receita: repasse e manutenção ficam em zero).
	double revenue_pct_gdp = 0.0;
	double target_revenue_month = 0.0;
	// Parte do repasse que ESTA cidade pagou do tesouro nacional (para o teste
	// conferir que repasse recebido = repasse pago dentro da nação).
	double month_transfer_paid = 0.0;
	CityGovProject projects[kCityGovMaxProjects] = {};
	int project_count = 0;
};

} // namespace ogs
