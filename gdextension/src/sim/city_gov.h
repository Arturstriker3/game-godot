#pragma once
// §19 (W2) — prefeitura: caixa, receita mensal, despesa e decisão de obra
// urbana (docs/economy/s19_urban_spec.md, pacote W2).
//
// Fluxo do mês (a cada 30 dias de simulação):
//   receita  = IPTU (donos não-governo) + taxas locais (faturamento de
//              serviços) + repasse do tesouro nacional;
//   despesa  = manutenção urbana + obras (infraestrutura e moradia social).
// Dinheiro conservado: nenhuma rubrica cria ou destrói XGS. O IPTU sai da
// poupança das famílias e do caixa das firmas; a taxa local sai do caixa dos
// serviços; o repasse sai do orçamento público das cidades da nação (o
// "tesouro nacional" do modelo); a manutenção vai para o setor de serviços e a
// obra paga o setor de construção. C++ puro (STL), nada de godot_cpp em sim/.
//
// PONTO DE INTEGRAÇÃO COM O W1 (sim/urban_land.cpp): a prefeitura NÃO executa a
// obra física. Ela chama `start_expansion` / `start_densify` declaradas aqui;
// quando o W1 chegar, o corpo dessas funções passa a empilhar um
// DemoConstructionProject §15 do tipo "infra_urbana" (materiais + prazo do
// setor de construção) e a conclusão soma área/capacidade em `CityLand` dentro
// do urban_land.cpp. Até lá a versão local mantém o MESMO formato (orçamento
// reservado + prazo + reserva de solo) e conclui somando área/capacidade, para
// o caixa da prefeitura não ficar preso para sempre e a conservação de dinheiro
// já valer em produção. Moradia social seguirá o mesmo caminho no W3.

#include "world_state.h"

namespace ogs {

class SimCore;

namespace city_gov {

// ---------------------------------------------------------------------------
// Calibração e parâmetros do W2. São parâmetros DECLARADOS do modelo (a fonte
// não os publica); cada um diz de onde vem e o que o substitui.
// ---------------------------------------------------------------------------
// Fechamento mensal: o calendário da simulação é de 12 meses de 30 dias.
constexpr int kMonthDays = 30;
constexpr int kDaysPerYear = 360;
// Carga tributária de referência da economia (% do PIB): converte o PESO do
// imposto predial no total de tributos (property_tax_pct_total_tax, W0) em
// alíquota efetiva. A alíquota crua do W0 (~5-10% ao ano sobre o estoque) é
// ~10x a alíquota efetiva real; ancorar no PIB devolve a receita ao nível
// observado.
constexpr double kTotalTaxPctOfGdp = 0.30;
// Manutenção urbana como fração da receita municipal alvo (% do PIB da fonte):
// vias, limpeza, iluminação e parques. Cresce com a população porque o alvo é o
// PIB municipal (PIB por habitante x população).
constexpr double kMaintenanceShareOfTarget = 0.45;
// Ocupação de zona que dispara a decisão de adensar/expandir (spec §19 W2).
constexpr double kBusyOccupancy = 0.90;
// Fração do caixa da prefeitura que UMA frente (obra de solo ou lote de moradia
// social) pode consumir no fechamento do mês. Não vem de fonte externa: é a
// régua declarada do "sem explosão" — o município não zera o caixa numa obra,
// e o que sobra fica de reserva para o mês seguinte (inclusive para juntar o
// suficiente para uma obra maior). Sem essa fração a expansão comprava todo o
// caixa disponível e o caixa fechava o mês em zero em toda cidade cheia.
constexpr double kWorksCashShare = 0.5;
// m² de piso por negócio (serviço ou fábrica urbana) na capacidade de zona.
// SUBSTITUÍDO na integração: o W1 mede a zona por TRABALHADOR
// (kDemoUrbanServiceM2PerWorker / kDemoUrbanFactoryM2PerWorker). A constante
// fica só como registro do estado anterior do pacote W2.
constexpr double kBusinessFloorM2 = 200.0;
// Expansão padrão: km² abertos por obra (limitado pela reserva e pelo caixa).
constexpr double kExpansionKm2 = 1.0;
// Adensar custa esta fração do km² de infraestrutura (spec: custo menor).
// SUBSTITUÍDO na integração pelo custo por NÍVEL do W1
// (kDemoUrbanDensifyCostFactorLowMid/MidHigh) — registro do pacote W2.
constexpr double kDensifyCostShare = 0.35;
// Pessoas por domicílio, para estimar o déficit habitacional. O W3 mede a
// demanda por estrato; até lá é o tamanho médio declarado do domicílio.
constexpr double kPeoplePerDwelling = 3.0;
// Prazos de obra (dias de simulação).
constexpr int kProjectDaysExpand = 45;
constexpr int kProjectDaysDensify = 25;
constexpr int kProjectDaysSocialHousing = 30;

// ---------------------------------------------------------------------------
// Ciclo diário.
// ---------------------------------------------------------------------------
// Avança as obras urbanas do dia e, quando `day` fecha um mês, roda o fluxo
// financeiro da prefeitura e a decisão de obra. Serial e determinístico (a
// ordem é o índice de cidade; o repasse é agregado por nação em ordem de
// primeira aparição, nunca pela ordem de um unordered_map).
// `core` é necessário só para abrir a obra §15 de solo (integração W1/W2);
// nulo = fluxo financeiro roda, decisão de obra não (sem estado para empilhar).
void tick(WorldState& ws, int day, SimCore* core = nullptr);

// Dinheiro do mundo guardado pela prefeitura: caixa + fundos de obra ainda não
// pagos. Entra em `demo_money_total` para a conservação valer em qualquer ponto
// do tick.
double money_total(const WorldState& ws);

// Retrato/reversão do tick (invariantes rígidos): solo e prefeitura. A moradia
// fica de fora porque o W2 não a muta (W3).
void snapshot(const WorldState& ws, DemoSnapshot& out);
void restore(WorldState& ws, const DemoSnapshot& in);

// ---------------------------------------------------------------------------
// PONTO DE INTEGRAÇÃO COM O W1/W3 — obras urbanas.
// Criam a obra, debitam o caixa da prefeitura para o fundo reservado e devolvem
// false se não há slot, área, unidade ou caixa. `km2`/`units` são reduzidos ao
// que o orçamento disponível paga (não se contrata obra sem dinheiro).
// ---------------------------------------------------------------------------
bool start_expansion(CityGov& gov, const CityLand& land, int zone, double km2,
	double cost, int day);
bool start_densify(CityGov& gov, int zone, double cost, int day);
bool start_social_housing(CityGov& gov, double units, double cost, int day);

// Obra em andamento? (a prefeitura toca uma frente por vez e uma de moradia).
bool has_project(const CityGov& gov);

} // namespace city_gov
} // namespace ogs
