#pragma once
// Constantes de balanceamento e geometria do núcleo econômico (S1 do
// docs/economy/core_refactor_plan.md). Sem dependência do engine Godot: só STL.
// Extraídas do prólogo de economy_sim.cpp (antes viviam num namespace anônimo).

namespace ogs {

constexpr double kPi = 3.14159265358979323846;
constexpr double kEarthRadiusKm = 6371.0;
constexpr double kMapWidth = 5000.0;
constexpr double kMapHeight = 2500.0;
constexpr double kHoursPerDay = 24.0;

// Sobrecarga de embarque por lote (a frota sai em comboio, não em série).
// Usada por make_leg (tempo do trecho), por isso vive no primeiro bloco.
constexpr double kDemoLoadHoursPerBatch = 0.5;

// Versão do schema do save do modo demo. Aumente ao mudar a estrutura do
// dicionário; demo_from_dict recusa versões futuras (maiores) e migra as
// anteriores.
// v3: farinha/pão removidos da cadeia (enum DemoProduct deslocou -2 a partir do
// ferro) e 21 produtos. demo_from_dict remapeia saves v1/v2 (com farinha/pão).
// v5: frota nacional (F4) — `fleets` e a reserva de veículos de cada carga em
// `in_transit`. Save v4 sem a chave deriva a frota de população/PIB.
// v6: moedas/câmbio e tarifas (F5) — `currencies`, `tariffs` e a cotação de
// importação por cidade (`import_price`). Save v5 sem as chaves mantém as
// moedas já carregadas e o preço do mercado.
// v7: preço de mercado na ÂNCORA (F5, opção c). Saves v6 e anteriores tinham o
// preço na moeda local; demo_from_dict converte pela taxa da nação.
// v8: `body_armor` removido da cadeia (decisão do dono). Saves v7 com 114
// produtos são remapeados por id (`products`) e o índice 112 passa a ser
// `infantry_gear`.
// v9: trabalho, educação e renda (F7) — bloco `pop.labor` (idade ativa,
// educação por nível, força de trabalho/emprego/desemprego, vagas, salários,
// estudantes e nível de renda), `minimum_wages` por nação e `wage_rigidity`.
// Save v8 e anteriores não têm o bloco: o perfil é derivado do índice de renda
// já gravado (migração documentada em demo_from_dict).
// v10: serviços (F8) - `services` por cidade (mercados dos 20 setores, receita
// de exportação/governo, custo de insumos e cobertura de saúde/educação) e
// `service_businesses` (catálogo global de negócios). Save v9 e anteriores não
// têm as chaves: o mundo é semeado de negócios no load (migração).
// v11: investimento e propriedade (F9) - `groups` (id, nome, origem, setor,
// caixa, acumulados), `group_flows` (repatriação/dividendos/impostos por
// nação), `fdi_policies`, `sector_incentives` e `confidence`. Save v10 e
// anteriores não têm as chaves: os grupos são semeados a partir dos ativos
// privados no load (regra determinística de demo_seed_groups).
constexpr int kDemoSaveVersion = 11;

// F8: serviços. Preço reage a demanda financiada/oferta, dentro da banda da
// âncora; prejuízo persistente fecha o negócio (contado à parte das falências
// industriais). A entrada só acontece com demanda não atendida e margem.
//
// F8 v2 (calibração, Q-004): a banda foi APERTADA de [0,55; 2,20] para
// [0,70; 1,40] — o teto de 2,20× deixava a margem disparar (a oferta
// insuficiente empurrava o preço ao teto, ~69% de margem bruta). Com a âncora
// ligada ao salário local e a banda mais realista, a margem líquida cai para a
// faixa de 5–20%. A reserva de capital caiu de 4× para 1×: o multiplicador de
// 4× era prudência de POUPANÇA de família; o caixa do setor já mantém o capital
// de giro (8 dias) separado, então 1× basta como gate de liquidez.
constexpr int kDemoServiceLossDays = 60;
constexpr double kDemoServicePriceSmooth = 0.12;
constexpr double kDemoServicePriceBandLow = 0.70;
constexpr double kDemoServicePriceBandHigh = 1.40;
constexpr double kDemoServiceEntryMargin = 0.08;
constexpr double kDemoServiceCapitalReserve = 1.0;
constexpr double kDemoServiceHealthNeedPerCapita = 0.00002;  // unidades/dia
constexpr double kDemoServiceEducationNeedPerCapita = 0.000015;
constexpr double kDemoServiceHealthSatisfactionBonus = 0.03;
constexpr double kDemoServiceEducationRateBonus = 0.60;

// Até esta distância, o vendedor do mesmo país é considerado mercado regional.
constexpr double kRegionalRadiusKm = 500.0;

} // namespace ogs
