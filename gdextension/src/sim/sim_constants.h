#pragma once
// Constantes de balanceamento e geometria do núcleo econômico (S1 do
// docs/economy/core_refactor_plan.md). Sem dependência do engine Godot: só STL.
// Extraídas do prólogo de economy_sim.cpp (antes viviam num namespace anônimo).

#include <string>

namespace ogs {

constexpr double kPi = 3.14159265358979323846;
constexpr double kEarthRadiusKm = 6371.0;
constexpr double kMapWidth = 5000.0;
constexpr double kMapHeight = 2500.0;
constexpr double kHoursPerDay = 24.0;

// Sobrecarga de embarque por lote (a frota sai em comboio, não em série).
// Usada por make_leg (tempo do trecho), por isso vive no primeiro bloco.
constexpr double kLoadHoursPerBatch = 0.5;

// Versão do schema do save do modo demo. Aumente ao mudar a estrutura do
// dicionário; from_dict recusa versões futuras (maiores) e migra as
// anteriores.
// v3: farinha/pão removidos da cadeia (os índices de produto deslocaram -2 a
// partir do ferro) e 21 produtos. from_dict remapeia saves v1/v2.
// v5: frota nacional (F4) — `fleets` e a reserva de veículos de cada carga em
// `in_transit`. Save v4 sem a chave deriva a frota de população/PIB.
// v6: moedas/câmbio e tarifas (F5) — `currencies`, `tariffs` e a cotação de
// importação por cidade (`import_price`). Save v5 sem as chaves mantém as
// moedas já carregadas e o preço do mercado.
// v7: preço de mercado na ÂNCORA (F5, opção c). Saves v6 e anteriores tinham o
// preço na moeda local; from_dict converte pela taxa da nação.
// v8: `body_armor` removido da cadeia (decisão do dono). Saves v7 com 114
// produtos são remapeados por id (`products`) e o índice 112 passa a ser
// `infantry_gear`.
// v9: trabalho, educação e renda (F7) — bloco `pop.labor` (idade ativa,
// educação por nível, força de trabalho/emprego/desemprego, vagas, salários,
// estudantes e nível de renda), `minimum_wages` por nação e `wage_rigidity`.
// Save v8 e anteriores não têm o bloco: o perfil é derivado do índice de renda
// já gravado (migração documentada em from_dict).
// v10: serviços (F8) - `services` por cidade (mercados dos 20 setores, receita
// de exportação/governo, custo de insumos e cobertura de saúde/educação) e
// `service_businesses` (catálogo global de negócios). Save v9 e anteriores não
// têm as chaves: o mundo é semeado de negócios no load (migração).
// v11: investimento e propriedade (F9) - `groups` (id, nome, origem, setor,
// caixa, acumulados), `group_flows` (repatriação/dividendos/impostos por
// nação), `fdi_policies`, `sector_incentives` e `confidence`. Save v10 e
// anteriores não têm as chaves: os grupos são semeados a partir dos ativos
// privados no load (regra determinística de seed_groups).
// v12: construção como projeto físico (§15) - `construction_projects` (obras em
// andamento). Save v11 e anteriores não têm a chave: não há projeto em voo, o
// mundo carrega com a lista vazia (migração trivial, sem estado a reconstruir).
// v13: solo urbano, moradia e prefeitura (§19, W0) - bloco `urban` por cidade
// (área por zona, densidade permitida, reserva, unidades por densidade/padrão,
// posse/aluguel/preço e contas da prefeitura). Save v12 e anteriores não têm a
// chave: os vetores ficam como estão (vazios num load frio ou com os padrões
// carregados pelo set_urban), sem lógica diária a reconstruir.
// v14: a obra de MORADIA sai da lista própria (`housing[].projects`) e entra na
// fila única `construction_projects` com `kind = CONSTRUCTION_HOUSING` (+
// housing_density/tier/owner/units e land_cost). Ao carregar v13, cada obra da
// lista antiga é migrada para a fila com `duration_days = days_left` e custo 0
// (o capital já foi pago/pausado na versão antiga).
// v15 (A6): cadência por sistema escalonada. Bloco novo `cadence`
// (`weekly_period` 7, `monthly_period` 30). Save v14 e anteriores carregam os
// padrões — o escalonamento é derivado do dia e do índice da cidade, então não
// há cursor a migrar.
// v16 (B1): demografia por coortes — bloco `pop.age_bands` (5 faixas etárias,
// vazio quando a cidade não tem estrutura nacional), `pop.health_weight_ref` e
// `pop.education_weight_ref` (pesos do dia da semeadura). Save v15 e anteriores
// não têm as chaves: as coortes são semeadas da estrutura nacional no load
// (`seed_demography`); sem dado para a nação, o perfil paramétrico legado
// continua valendo.
// v17 (B2): estratos de renda (quintis) — bloco `pop.strata` (pessoas,
// quintis-alvo, dispersão, capital, poupança, dívida, patrimônio, taxa,
// aluguel pago/faturado, nível, educação, posse e ciclo de vida) e
// `rent_income_next` por cidade. Save v16 e anteriores não têm o bloco: a
// semeadura acontece no FIM do load (depois do `urban`, com a posse de moradia
// carregada) e reparte a poupança escalar pela renda (`seed_strata`).
// v18 (minas, D-086): cada mina/poço de extração mineral tem um ESTÁGIO de
// operação (`stage`, `stage_reserve_kt`, `stage_mined_kt`) e a obra de
// aprofundamento (`construction_projects[].deepen_stage`). Save v17 e
// anteriores não têm as chaves: toda construção de extração primária entra no
// ÚLTIMO estágio com o estágio esgotado — é exatamente o mundo que a decisão do
// dono descreve (minas antigas produzindo quase nada). A migração é ADITIVA e
// roda sobre o mesmo load: v16 → v17 (strata, acima) → v18 (estágio de mina).
// v19 (B3, D-085): região da cidade (estados inteiros) — bloco `city.region`
// (nome, população total/urbana/interior, urbanização nacional, mix de emprego,
// estados, coortes etárias e quintis do interior, caixa). Save v18 e anteriores
// migram: `region_pop = max(0, population - simulation_population)` (já no
// save), coortes/estratos da nação e mix neutro (sem dado de urbanização).
// v20 (B4, D-088): migração — acumuladores `region.migration` (êxodo rural →
// urbano e cidade → cidade na mesma nação). Save v19 e anteriores não têm o
// bloco: os acumuladores nascem zerados (a migração começa a contar do dia 0
// do save novo; nada é inventado para trás). A cadeia v16 → v20 é o mesmo
// caminho aditivo: cada etapa olha só as chaves que faltam (strata em v17,
// estágio de mina em v18, região em v19, migração em v20).
// v21 (B5, D-089): infraestrutura do estado (`region.infra`) e royalty de
// concessão (`building.royalty`). Save v20 e anteriores migram com níveis 0 e
// royalty 0 (nada inventado para trás).
// v22 (B7, D-093): informalidade, serviços informais, mortalidade e
// insatisfação — blocos `pop.informal`/`pop.mortality`/`pop.dissatisfaction`
// (urbano) e `region.informal`/`region.mortality`/`region.dissatisfaction`
// (interior). Save v21 e anteriores migram semeando a informalidade do dado da
// nação (ou âncora de renda), a referência de saúde e os contadores zerados.
// v23 (B6, D-095): patrimônio da região (`region.property`) — hectares, Gini
// fundiário, valores de terra/moradia/anel, população do anel, alíquotas e a
// distribuição por quintil. Save v22 e anteriores carregam sem o bloco: a
// região fica inerte até o dado ser reaplicado (GDScript, `set_region_property`
// só age quando `property_ready` é falso) e a alíquota nasce 0.
// v24 (G, D-096): agricultura sazonal — bloco raiz `agriculture` com o estado
// por (cidade, produto, safra): ano do plantio, fator de rendimento por
// fertilizante, ano da colheita em curso e o que já foi entregue. Save v23 e
// anteriores migram com o estado zerado: a safra do ano corrente recomeça no
// dia do load (a produção já entregue antes do save não é reinventada). O
// bloco `definitions` ganha `crop_plans`/`crop_losses`/`shelf_life`, que são
// dado (redeclarado pelo AgricultureService no jogo). A cadeia é ADITIVA:
// v22 → v23 (B6) → v24 (G), cada degrau olhando só as chaves que faltam, então
// um save v22 atravessa os dois degraus no mesmo load.
// v25 (G2, D-100): armazenagem — bloco raiz `storage` com os armazéns
// agregados por (cidade, tipo, dono): capacidade e estoque guardado do governo
// e do privado. Save v24 e anteriores carregam SEM armazém (nenhuma capacidade
// ou tonelada inventada para trás); o StorageService reaplica a capacidade
// inicial do dado/regra declarada depois do load. Obras de armazém em curso
// viajam em `construction_projects` com `kind = CONSTRUCTION_STORAGE`.
// v26 (C1, D-102): contabilidade por balanço — o SALÁRIO ATRASADO da D-099
// (parcela da folha que o caixa não cobriu) passa a ser registrado como
// passivo da firma (`Building::wage_arrears`, `City::service_wage_arrears`,
// `City::rural_wage_arrears`) e ativo das famílias
// (`Pop::arrears_receivable`, `Region::arrears_receivable`). Save v25 e
// anteriores carregam com atrasado ZERO (nada é inventado para trás: o
// fenômeno anterior não deixa registro retroativo). O resto do balanço é
// LEITURA derivada do estado existente e não ocupa o save.
// v27 (C2a, D-103): banco central por moeda (estado + medição). Campos novos,
// todos ADITIVOS: em cada moeda, `exchange_regime` (nasce do `floating` que já
// existia; peg é o default do dado) e `bank_independence` (nasce `no_data`);
// blocos de raiz `monetary_policy` (padrão monetário = fiduciário e moeda de
// reserva = USD, os defaults declarados pelo plano) e `central_bank` (reserva
// de ouro do BC por moeda, dado real da G2/D-101, e a série mensal do CPI por
// nação). Nenhuma mecânica nova: nada emite, cobra juros ou intervém — é a
// C2b. Save v26 carrega sem ouro e com a série vazia (nada inventado).
// v31 (C2c, D-108): regime REAL por moeda (IMF AREAER 30/04/2026), âncora
// (moeda ou cesta), fonte, emissor e trava de união monetária; a cesta
// resolvida vai no `fx.currencies[moeda]`. ADITIVO: v30 carrega com os regimes
// do arquivo (`set_fx_regimes`) e as moedas extintas fundidas no substituto.
// v35 (PARTE E, D-116): ENERGIA ELÉTRICA — bloco raiz `power` com o caixa
// agregado do setor (`power_cash`), a regra (`policy`), o estado por cidade
// (conexão conectada/parcial/isolada, capacidade da linha, acesso e perda,
// teto e parque solar mensal, e os indicadores do dia) e as usinas por cidade.
// A rede por nação (`grids`) guarda o cooldown da obra da IA. ADITIVO: v34 e
// anteriores chegam SEM o bloco — `power_ready = false`, sem parque e sem
// rede (ausência declarada); o PowerService aplica as definições do dado
// público depois do load e semeia o parque real. As obras de energia em voo
// viajam em `construction_projects` com `kind = CONSTRUCTION_POWER_PLANT`/
// `CONSTRUCTION_POWER_LINE` e os campos `power_fuel`/`power_mw`.
// v36 (PARTE E, D-117): interconexões entre redes NACIONAIS (E5). ADITIVO: o
// bloco `power` ganha `interconnects` (par a/b, capacidade, fluxo/preço do dia
// e a marca de linha do ano-base × obra) e `interconnects_ready`; cada rede
// ganha o ledger `import_mwh`/`export_mwh`/`import_value`/`export_value`. As
// obras em voo viajam em `construction_projects` com
// `kind = CONSTRUCTION_INTERCONNECT` e `power_nation_a`/`power_nation_b`. Save
// v35 carrega sem a chave: `ready = false` e o PowerService semeia as linhas do
// ano-base depois do load (nada inventado para trás).
//
// D-125 (save v40, aditivo): por construção `production_efficiency`,
// `efficiency_days`, `transition_days`, `equipment` e `refinery_mode`; o bloco
// `definitions.efficiency` guarda os parâmetros de balanceamento da curva.
// Save v39 carrega com a planta MADURA (eficiência 1, equipamento ligado): o
// estoque existente é o ano-base e o comportamento anterior é preservado.
//
// D-131 (save v42, aditivo): o bloco `definitions.ai_brain` guarda os
// parâmetros do cérebro econômico da IA (fase A: previsão de caixa + preço
// esperado). Save v41 carrega com os defaults do struct (`enabled=false`),
// que reproduzem as decisões atuais.
constexpr int kSaveVersion = 44;

// ---------------------------------------------------------------------------
// B1 — DEMOGRAFIA POR COORTES (5 faixas). O DADO é a estrutura etária nacional
// (age_structure_2026.json: frações por faixa, CBR/CDR por mil e TFR). O que
// segue é MODELO declarado, calibrável no laboratório — não é dado observado:
// ---------------------------------------------------------------------------
// Número de coortes: 0-14, 15-24, 25-44, 45-64 e 65+ (mesma ordem do arquivo e
// de `AgeStructure`/`Pop::age_band`).
constexpr int kAgeBands = 5;
// Fluxo ANUAL de saída de cada faixa para a seguinte, em fração da faixa de
// origem: 1/15 (0-14 → 15-24), 1/10 (15-24), 1/20 (25-44) e 1/20 (45-64); o
// fluxo mensal é este dividido por 12. Leitura mínima de uma população
// estacionária com faixas de 15/10/20/20 anos: não há idade declarada dentro da
// faixa, então o escoamento uniforme é a aproximação do modelo — não reproduz a
// pirâmide real de nenhuma nação.
constexpr double kAgeAgingAnnual[4] = {1.0/15.0, 1.0/10.0, 1.0/20.0, 1.0/20.0};
// Perfil RELATIVO de mortalidade por faixa (peso, não taxa): o CDR medido da
// nação dá o TOTAL de mortes e estes pesos dizem COMO ele se reparte entre as
// coortes. Forma declarada: mortalidade infantil/materna nas duas primeiras
// faixas (0,25 e 0,10), adulto jovem baixo (0,30), meia-idade subindo (0,85) e
// 65+ concentrando a maior parte (3,40) — ordem de grandeza de uma tábua de
// vida, não uma tábua calibrada.
constexpr double kAgeDeathWeight[5] = {0.25, 0.10, 0.30, 0.85, 3.40};
// Perfil RELATIVO da necessidade de SAÚDE por faixa: reparte a necessidade per
// capita do setor entre as coortes. Crianças e jovens consomem pouco (0,05 e
// 0,10), adultos em idade ativa um pouco mais (0,25), meia-idade sobe (0,80) e
// 65+ concentra o gasto (2,50). O multiplicador é normalizado pela composição
// do dia da semeadura (`health_weight_ref`), então a média do mundo não muda.
constexpr double kAgeHealthWeight[5] = {0.05, 0.10, 0.25, 0.80, 2.50};
// Perfil RELATIVO da demanda de EDUCAÇÃO por faixa: a escola é da infância e da
// juventude. 0-14 pesa 1,00 (escolarização obrigatória), 15-24 ainda frequenta
// (0,60), 25-44 residual (0,10), 45+ quase nada (0,02 e 0,00). Escala a
// necessidade per capita do setor; não é matrícula observada.
constexpr double kAgeEducationWeight[5] = {1.00, 0.60, 0.10, 0.02, 0.00};

// ---------------------------------------------------------------------------
// B3 — REGIÃO DA CIDADE (estados inteiros). O DADO é a urbanização nacional
// (urban_share_reference.json, World Bank SP.URB.TOTL.IN.ZS, 10 nações) e a
// população por estado (state_regions.json). O que segue é MODELO DECLARADO:
// ---------------------------------------------------------------------------
// Urbanização usada quando a nação não tem dado (`urban_share` = -1): mix
// neutro explícito (metade rural/metade urbana) — ausência de dado declarada,
// não número por país inventado.
constexpr double kRegionUrbanShareNeutral = 0.50;
// Mix de emprego do interior por urbanização (fração do emprego): primário
// (agro/extração/mineração/madeira) cai com a urbanização, indústria leve e
// serviços locais sobem. Interpolação LINEAR entre os dois extremos:
//   mix[0] = lerp(kRegionAgriAtRural, kRegionAgriAtUrban, urban_share)
//   mix[1] = lerp(kRegionIndustryAtRural, kRegionIndustryAtUrban, urban_share)
//   mix[2] = 1 - mix[0] - mix[1]
// Ordem de grandeza: o campo é majoritariamente primário mesmo em países
// urbanizados (o interior é o que resta fora da aglomeração), mas a fração de
// serviços/indústria leve cresce com a renda/urbanização. Não é dado por país.
constexpr double kRegionAgriAtRural = 0.70;
constexpr double kRegionAgriAtUrban = 0.35;
constexpr double kRegionIndustryAtRural = 0.15;
constexpr double kRegionIndustryAtUrban = 0.25;
// Desconto rural: o salário médio do interior é uma fração do salário urbano
// do mesmo nível de escolaridade (renda rural < urbana, padrão da Parte D).
constexpr double kRegionRuralWageDiscount = 0.55;
// Fração da população do interior empregada no setor PRIMÁRIO (agro/extração/
// mineração/madeira) — o `employment_mix[0]` multiplica esta base. MODELO
// declarado (ordem de grandeza de uma força de trabalho rural); não é dado
// por país.
constexpr double kRegionLaborShare = 0.35;
// Poupança inicial do interior por habitante (bootstrap). Igual à urbana
// (kSavingsPerCapita): o interior começa com caixa para comprar comida antes
// do primeiro salário semanal (cadência A6). Entra em `money_total` desde o
// dia 0 — é a mesma convenção da poupança urbana semeada em initialize_world.
constexpr double kRegionSavingsPerCapita = 0.50;
// Necessidade alimentar per capita do interior (t-eq de grão/dia). Igual à
// urbana por construção (kNeedPerCapita): a diferença está no acesso/preço, não
// na necessidade física.
// (Usa kNeedPerCapita diretamente; esta nota documenta a decisão.)
// Imposto do interior: mesma alíquota kTaxRate, pago à prefeitura da
// cidade-polo (contraparte no caixa municipal).

// ---------------------------------------------------------------------------
// B4 — MIGRAÇÃO (êxodo rural → polo urbano e cidade → cidade na mesma nação).
// O DADO é o mesmo da B3 (urbanização nacional de `urban_share_reference.json`
// e população por estado de `state_regions.json`). O que segue é MODELO
// DECLARADO, calibrável no laboratório — nenhum número por país inventado.
// ---------------------------------------------------------------------------
// Taxa MÁXIMA mensal de migração (fração da população de origem por mês). Teto
// do fluxo; ordem de grandeza de urbanização rápida (~6%/ano se sempre no teto,
// o triplo da média mundial real — é o limite, não o valor típico).
constexpr double kMigrationMonthlyMax = 0.005;
// Gap mínimo (relativo) para disparar o êxodo: a renda urbana esperada
// (salário × chance de emprego − moradia) precisa superar a renda rural por
// pessoa nesta margem. Evita churn por diferença de 1 ulp e migração quando o
// diferencial é nulo (equilíbrio).
constexpr double kMigrationGapThreshold = 0.10;
// Escala de saturação do gap: o fluxo atinge o teto quando o diferencial
// relativo chega a este valor ACIMA do limiar (gap relativo de 60% satura).
constexpr double kMigrationGapScale = 0.50;
// Perfil ETÁRIO do migrante (pesos relativos por faixa 0-14..65+): os jovens
// (15-24 e 25-44) pesam mais e os 65+ quase não migram. Reparte o fluxo ENTRE
// as faixas disponíveis da origem (o campo envelhece).
constexpr double kMigrationAgeWeight[5] = {0.30, 0.80, 1.00, 0.45, 0.10};
// Perfil de ESTRATO do êxodo (pesos relativos por quintil, do mais pobre ao
// mais rico): saem primeiro os quintis mais baixos (o pobre é empurrado do
// campo; o rico tem mais a perder).
constexpr double kMigrationStrataWeight[5] = {1.00, 0.80, 0.55, 0.35, 0.15};
// Peso dos SERVIÇOS na utilidade de cidade → cidade: a utilidade é a renda
// líquida esperada + este peso × renda de serviços per capita (diária). Pequeno
// de propósito — salário/emprego/moradia dominam; o termo só desempata cidades
// com renda parecida e captura o "serviços" do plano §B4.
constexpr double kMigrationServiceWeight = 0.10;

// ---------------------------------------------------------------------------
// B5 — INFRAESTRUTURA DA REGIÃO/ESTADO (estrada, energia/luz, água). GAMEPLAY
// do estado aprovado pelo dono (2026-11-17, §B5 do plano). Três níveis por
// região, cada um 0..kInfraMaxLevel. O DADO (acesso a eletricidade/água por
// país, quando houver) é usado só como nível INICIAL na geografia; o que segue
// é MODELO DECLARADO, calibrável no laboratório — nenhum número por país
// inventado. Fontes/parâmetros em docs/economy/B5_state_levers.md (D-089).
// ---------------------------------------------------------------------------
// Nível máximo de cada infra (estrada, energia, água). Pelo menos 3.
constexpr int kInfraMaxLevel = 3;
// Número de tipos de infra (ordem estável p/ save/comando/status: 0 estrada,
// 1 energia, 2 água).
constexpr int kInfraTypeCount = 3;
// Custo da obra para subir UM nível de um tipo (XGS), por habitante da região.
// Obra física §15 (projeto) com prazo kInfraBuildDays, paga pelo GOVERNO
// NACIONAL (não pela prefeitura) — modelo declarado.
constexpr double kInfraCostPerCapitaPerLevel = 0.01;
// Prazo da obra de infra (dias) — projeto físico §15, independente do tamanho.
constexpr int kInfraBuildDays = 90;
// Manutenção recorrente (XGS/dia) por nível ativo, por habitante da região.
// Entra na despesa NACIONAL (debita o caixa do governo) e é paga ao setor de
// construção/serviços — dinheiro conservado. Modelo declarado.
constexpr double kInfraMaintenancePerCapitaPerLevelPerDay = 0.0000005;
// Peso de cada tipo na produtividade do TRABALHO primário da região (soma 1).
// Estrada escoa a produção, energia mecaniza, água sustenta gente e lavoura.
constexpr double kInfraProductivityWeight[kInfraTypeCount] = {0.40, 0.35, 0.25};
// Ganho MÁXIMO de produtividade (fração sobre a base) com todos os tipos no
// máximo: prod_eff = 1 + kInfraProductivityGain * Σ(weight × level/max).
constexpr double kInfraProductivityGain = 0.30;
// Custo de acesso região ↔ polo: penalidade MÁXIMA (fração sobre o preço) com
// estrada 0, caindo linearmente a 0 na estrada máxima. É frete/overhead pago ao
// setor de transporte (conserva dinheiro), embutido na transação da região.
constexpr double kInfraAccessPenaltyMax = 0.30;
// Êxodo (B4): fator de expulsão MÁXIMO (multiplicador sobre o fluxo) com infra
// 0, caindo a 1,0 no máximo. Pesos dos tipos na cobertura combinada (soma 1).
constexpr double kInfraExodusBoostMax = 0.50;
constexpr double kInfraExodusWeight[kInfraTypeCount] = {0.50, 0.30, 0.20};

// ---------------------------------------------------------------------------
// B5 — CONCESSÕES de mineração e terra (F9). Royalty/imposto padrão de uma
// concessão: fração da receita da mina/fazenda concedida paga ao Estado.
// MODELO DECLARADO (o concessionário fica com o resto do lucro; o Estado
// mantém a renda do royalty mesmo sem operar a mina).
// ---------------------------------------------------------------------------
constexpr double kConcessionRoyaltyRate = 0.05;
// IA do governo: fator de estágio ABAIXO do qual uma mina estatal é considerada
// ociosa/esgotada e vira candidata a concessão (produz ~o piso de 5%). É a
// mesma régua do piso da D-086; declarado para a heurística ficar determinística.
constexpr double kConcessionFloorStageFactor = 0.10;

// §15 — construção como projeto físico. Obras são cronogramas de materiais +
// mão de obra do setor de construção; o pagamento sai do dono conforme avança.
// ---------------------------------------------------------------------------
// Fração do custo da obra que é MATERIAL (o resto é mão de obra/serviço). Os
// materiais são comprados do mercado local pelo preço corrente; a quantidade
// contratada é dimensionada pelo PREÇO-BASE (contrato de obra declarado).
constexpr double kConstructionMaterialShare = 0.15;
// §15 unificado: a terra é uma FATIA do orçamento do dono (não um custo
// extra). Em prédio econômico ela é limitada a esta fração, para o
// material+mão de obra ficarem com o resto do orçamento.
constexpr double kConstructionLandShareMax = 0.10;
// Prazo de obra por tipo (dias), estimativa de ordem de grandeza ("meses pelo
// tamanho", §15) — não é dado observado de cronograma de obra. A comida
// (fazenda da cesta) é RÁPIDA para não travar a oferta alimentar de nações que
// dependem de importação (a IA de comida planeja com antecedência); mina é
// média (obra de terraplenagem/galeria); fábrica (receita) é pesada.
constexpr int kConstructionFoodDays = 15;
constexpr int kConstructionMineDays = 45;
constexpr int kConstructionFactoryDays = 60;
constexpr int kConstructionPrimaryDays = 30;
// Escala do prazo pelo TAMANHO da obra (capacidade em t/dia): obras maiores
// levam mais tempo. fator = clamp(1 + log_scale × log2(cap/ref), min, max).
// Estimativa declarada — não é dado observado de cronograma.
constexpr double kConstructionSizeReference = 10.0;
constexpr double kConstructionSizeLogScale = 0.5;
constexpr double kConstructionSizeScaleMin = 0.5;
constexpr double kConstructionSizeScaleMax = 2.5;
// Rampa de operação de planta NOVA (construída via projeto físico): começa em
// kRampStart da capacidade e cresce LINEARMENTE até 100% ao longo de
// kRampDays dias de CARÊNCIA, durante os quais a falência por prejuízo NÃO
// conta (empresa nova real: o capital da obra é investimento, não prejuízo
// operacional; a planta precisa de tempo para formar carteira antes de ser
// avaliada). O opex da planta em rampa acompanha o output_scale (custo fixo
// reduzido), não o custo de 100%.
constexpr double kRampStart = 0.25;
constexpr int kRampDays = 90;
// Valor de obra (XGS) que cada trabalhador do setor de construção avança por
// dia. Ordem de grandeza ancorada na produtividade de serviços (capital/jobs
// da F8); limita o surto de obras mas não bloqueia a segurança alimentar.
constexpr double kConstructionValuePerWorkerDay = 20.0;
// Piso de trabalhadores de construção por cidade (setor indústria/construção)
// para a obra nunca travar em cidade sem indústria declarada (sector_workers
// vira 0 no primeiro dia, em cidades só-agrícolas e logo após um load). 100
// trabalhadores × 20 XGS/dia = 2.000 XGS/dia de obra: uma obra típica avança;
// obras grandes (fábrica pesada) ou muitas obras simultâneas dividem o teto.
constexpr double kConstructionWorkerFloor = 100.0;
// Estoque inicial de materiais de construção por cidade (derivado da população
// de simulação). É a "obra em estoque" pré-existente da economia; declara-se
// como `derived:initial_stock` — não é dado observado de estoque de materiais.
// Generoso o bastante para a construção do cenário (365 d) não travar: no mundo
// real ainda não há produtor de materiais de obra, então o estoque é o único
// suprimento até a indústria pesada entrar (direção futura do §15).
constexpr double kConstructionMaterialStockPerCapita = 0.002;
constexpr double kSteelStructureStockPerCapita = 0.001;
constexpr double kElectricalMaterialStockPerCapita = 0.0002;
// Maquinário industrial (fábricas): caro (base_price alto), então a quantidade
// por habitante é pequena — cobre a obra inicial de fábrica sem sobra.
constexpr double kIndustrialMachineryStockPerCapita = 0.0001;

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
constexpr int kServiceLossDays = 60;
constexpr double kServicePriceSmooth = 0.12;
constexpr double kServicePriceBandLow = 0.70;
constexpr double kServicePriceBandHigh = 1.40;
constexpr double kServiceEntryMargin = 0.08;
constexpr double kServiceCapitalReserve = 1.0;
constexpr double kServiceHealthNeedPerCapita = 0.00002;  // unidades/dia
constexpr double kServiceEducationNeedPerCapita = 0.000015;
constexpr double kServiceHealthSatisfactionBonus = 0.03;
constexpr double kServiceEducationRateBonus = 0.60;

// Até esta distância, o vendedor do mesmo país é considerado mercado regional.
constexpr double kRegionalRadiusKm = 500.0;

// Extração mineral. 1 kt = 1000 t.
constexpr double kTonsPerKiloton = 1000.0;
// Qualidade -> eficiência da extração: a produção efetiva é multiplicada por
// piso + (1 - piso) × qualidade (minério pobre rende menos por dia de operação).
constexpr double kQualityEfficiencyFloor = 0.5;
// Profundidade -> custo: opex × (1 + profundidade / escala), com teto.
constexpr double kDepthOpexScaleM = 2000.0;
constexpr double kDepthOpexMaxFactor = 3.0;
// Primeiro produto "bruto" (sem consumo local) na ordem de goods.json: 1=ferro.
// A fase de matérias-primas pula os alimentos da cesta (B3) via food_calories,
// então este valor só delimita onde começa a venda externa (minerais, algodão,
// madeira).
constexpr int kFirstRawProduct = 1;

// ---------------------------------------------------------------------------
// ESTÁGIO DE MINA (D-086, 2026-11-17) — mecânica, sem calibração de produção.
//
// MINA/POÇO = extração PRIMÁRIA de um produto com jazida rastreada (minerais,
// petróleo, gás, carvão, urânio). Fazenda não tem estágio: produto sem jazida
// (`deposit.tracked == false`) mantém o comportamento anterior.
//
// DECISÃO DO DONO (2026-11-17): o recurso NUNCA acaba. A reserva da jazida é só
// REFERÊNCIA de profundidade (escala da curva de custo): aprofundar/expandir
// nunca é bloqueado por reserva e a produção NUNCA zera por esgotamento — ela
// segue no piso do estágio mesmo com `remaining_kt == 0`. Cada estágio novo fica
// mais caro (curva crescente e CONVEXA), então em algum ponto é mais barato
// abrir/aprofundar em outro lugar: a necessidade migra pelo mapa.
//
// Curva de declínio (declarada, calibrável depois): a produção efetiva do
// estágio é `clamp(stage_remaining_kt / stage_reserve_kt, piso, 1)`. O estágio
// nasce cheio (produção 100%) e decai LINEARMENTE com a reserva que ele ainda
// pode extrair; ao esgotar, a mina fica no PISO — "quase nada", não zero: o
// poço continua existindo (e empregando uma equipe mínima) até alguém pagar o
// aprofundamento.
constexpr double kMineStageFloor = 0.05;
// Estágio das minas que JÁ EXISTEM no dia 0 (estado inicial da decisão do
// dono): o "último estágio" de um poço histórico, com o estágio esgotado.
// Aprofundar abre o estágio seguinte (kMineInitialStage + 1, + 2, ...).
constexpr int kMineInitialStage = 3;
// Fração da reserva de REFERÊNCIA (`initial_kt`) da jazida que o estágio novo
// reivindica ao abrir — NÃO é fração do saldo restante. 0,5 = cada
// aprofundamento trabalha metade da reserva de referência; a jazida não
// "encolhe" de estágio em estágio (o recurso nunca acaba).
constexpr double kMineStageReserveShare = 0.5;
// Cada aprofundamento AUMENTA a capacidade nominal da mina neste fator (a obra
// reabre o poço com mais poços de acesso/bombas). A produção só volta a 100% se
// o novo estágio estiver cheio.
constexpr double kMineDeepenCapacityFactor = 1.5;
// Custo do aprofundamento: capacidade nova × capital/t/dia × profundidade
// (MESMO `depth_opex_factor` do opex, 1..3) ÷ eficiência de qualidade (0,5..1 —
// minério pobre exige mover mais rocha) × `kMineDeepenStageCostFactor`^
// (estágio novo) × (1 + `kMineDeepenExtractionCostFactor` × min(extraído ÷
// referência, `kMineDeepenExtractionCostCap`)). O fator geométrico por estágio
// torna a curva CRESCENTE e CONVEXA; o fator de extração encarece a obra à
// medida que a jazida de referência já foi ultrapassada (teto declarado evita
// custo explosivo).
constexpr double kMineDeepenStageCostFactor = 1.25;
// Cada fator de extração além da referência adiciona esta fração ao custo, e o
// teto da razão extraído/referência evita custo infinito (a referência é
// `max(initial_kt, remaining_kt)`, defensivo contra reserva zerada).
constexpr double kMineDeepenExtractionCostFactor = 0.5;
constexpr double kMineDeepenExtractionCostCap = 4.0;
// Prazo da obra de aprofundamento: 90 dias + 30 por estágio já aberto
// (estimativa declarada de obra de mina, mesmo espírito de
// kConstructionMineDays; não é cronograma observado).
constexpr int kMineDeepenBaseDays = 90;
constexpr int kMineDeepenDaysPerStage = 30;
constexpr double kNeedPerCapita = 0.0003;
// F3: reserva de alimento (dias) que a POP mantém em poupança antes de gastar
// em life/luxury. Sem ela, a compra de conforto drenava a poupança usada para
// importar comida em nações sem terra (EGY/SAH em 365 d). É uma regra
// prudencial declarada, não um número observado: o colchão cobre uma década de
// cesta, então o conforto é pago essencialmente pela renda corrente, não pela
// poupança que financia a importação de alimento (EGY tinha poupança acima de
// um ano e ainda perdia comida em 365 d).
constexpr double kFoodReserveDays = 3650.0;
// F7: margem de poupança do gate de renda → consumo. A cidade só declara o
// nível de consumo life/luxury quando a renda corrente cobre a cesta acumulada
// dos níveis com esta folga. Regra prudencial declarada (não é dado observado):
// a cesta de `needs.json` cobre os bens comerciáveis do modelo; moradia,
// energia, transporte urbano e serviços não têm preço próprio no núcleo (o
// setor de serviços é um salário médio, não uma cesta). A folga representa esse
// custo de vida não modelado — sem ela, qualquer renda acima da cesta de bens
// liberava luxury em todo o mundo (F7: 832/834 cidades no nível 3). O valor
// mora em ws_.income_basket_margin (calibrado no economy_world_test; ver
// balance_review §23).
// F3: teto do gasto institucional (paper/computer) como fração do valor da
// cesta alimentar diária da cidade. A compra do Estado não pode competir com a
// importação de comida (EGY caía a 0,708 em 365 d quando o computer drenava o
// orçamento). Estimativa declarada, calibrável no laboratório.
constexpr double kGovernmentGoodsFoodShare = 0.05;
// Renda externa das famílias: serviços urbanos, pesca e turismo vendidos ao
// resto do mundo. Dimensão derivada da própria cesta de subsistência — a
// necessidade alimentar per capita (0,0003 t/dia) paga ao preço-base do trigo
// (40/t) = 0,012 por pessoa/dia. É o que uma cidade SEM terra produtiva
// (deserto, gelo, ilha de coral) precisa ganhar para importar o alimento que
// não planta; sem isso o caixa zera, a IA não investe e a nação passa fome
// mesmo com dinheiro parado no resto do mundo (§20). Sai do setor externo
// (foreign_cash), então `money_total` continua conservado.
//
// A renda NÃO é uniforme: é proporcional ao PIB PPP per capita REAL da nação
// (índice por cidade, ver set_external_income_index), normalizado para que
// a média mundial ponderada por população continue nesta base.
//
// F7 calibração: acima da média mundial (índice > 1) a transferência NÃO cresce
// mais. Ela é um PISO DE SUBSISTÊNCIA — o alimento que uma cidade sem terra
// produtiva precisa importar —, não uma transferência de riqueza: escalada até
// o topo, ela sozinha pagava 3× a cesta completa das famílias dos EUA/DEU e
// levava o mundo inteiro ao nível luxury, independentemente de salário/lucro.
// A renda acima da subsistência tem de vir do valor adicionado (salário +
// lucro), que é medido. Países pobres continuam abaixo de 1,0 e mantêm o piso
// proporcional ao PIB real.
constexpr double kExternalIncomePerCapita = 0.012;
constexpr double kExternalIncomeIndexCap = 1.0;
// F7 calibração — ANCORAGEM DA DISTRIBUIÇÃO DE RENDA (head, F7 final).
//
// A renda média por pessoa de cada nação é ancorada no PIB PPP per capita real
// (índice normalizado, média mundial = 1,0): a média da distribuição é
// `kIncomeAnchorWorldMean` cestas básicas × índice. A cesta básica (`basic`)
// custa 1×, a `life` acumulada deve custar 2,5–3× e a `luxury` acumulada 6–10×
// (limiares de classe no núcleo, multiplicadores abaixo). O sigma da lognormal
// vem do Gini da nação (GeographyService; `estimated` quando regional).
//
// Calibração verificada em 60 d com IA (economy_income_distribution_test):
// EUA/DEU basic ≤ 15% e maioria life; BRA basic ~41%; IND ~68%; NGA ~78%.
constexpr double kIncomeAnchorWorldMean = 2.0;   // cestas básicas/pessoa/dia
constexpr double kIncomeLuxuryMultiple = 7.0;    // luxo = 7× cesta básica
constexpr double kLaborShareLow = 0.50;
constexpr double kLaborShareHigh = 0.65;
constexpr double kServiceShare = 0.30;
constexpr double kServiceWage = 0.15;
// F8 v2 — ÂNCORA DE PREÇO DE SERVIÇO LIGADA AO SALÁRIO DO PAÍS. O preço-âncora
// por setor (`definition.price`) escala com o salário de serviço local relativo
// ao salário-âncora da F7 (0,15 XGS/posto/dia): price/jobs_per_unit ≈ 0,22 na
// âncora, então a receita por posto passa a ser ≈ 1,47× o salário LOCAL — a
// margem bruta (~32% antes de insumos) fica estável em país rico e pobre, sem o
// teto `preço÷jobs_per_unit×0,68` que mascarava o preço sub-calibrado (Q-004).
// Os limites evitam preço zero/explosivo quando o salário foge da faixa típica.
// O piso de 0,10 cobre salários de subsistência (~0,015 XGS/posto/dia) sem
// travar a escala em país muito pobre — era o piso de 0,50 que deixava a
// margem de nações pobres disparar (a receita por posto ficava 3× acima do
// salário real).
constexpr double kServiceAnchorScaleMin = 0.10;
constexpr double kServiceAnchorScaleMax = 4.0;
// F8 v2 — MARGEM-ALVO: reduz a receita por posto de ~1,47× para ~1,25× o
// salário local (margem bruta ~20% antes de insumos). Combinado com a banda
// apertada [0,70; 1,40], a margem líquida mediana cai para ~5–20% conforme o
// setor (varejo baixo, finanças/TI mais alto), sem o prejuízo estrutural do
// preço sub-calibrado que o teto 0,68 mascarava (Q-004).
constexpr double kServiceMarginCalibration = 0.85;
constexpr double kServiceSpendShare = 0.45;
constexpr double kTaxRate = 0.15;
constexpr double kTransferShare = 1.0;
constexpr double kCapitalDistribution = 1.0;
// ---------------------------------------------------------------------------
// B2 — ESTRATOS DE RENDA (QUINTIS) COM PATRIMÔNIO.
//
// Os valores abaixo são MODELO DECLARADO, não medição por país. A repartição
// inicial vem de dado observado (quintis de renda World Bank SI.DST via
// `strata_baseline.json`); estes parâmetros dizem como a estrutura evolui.
// Fontes do PADRÃO (não dos números): World Bank Global Findex/OECD para a
// poupança crescente com a renda; CEX/SCF (EUA) e OECD para o perfil de
// poupança por idade (jovem e idoso poupam menos, meia-idade mais); UBS Global
// Wealth Report 2024 para a concentração da riqueza acima da renda. A
// calibração de cada número está em `docs/economy/B2_strata.md`.
// ---------------------------------------------------------------------------
// Taxa de poupança-alvo por quintil (fração da renda disponível acima da cesta
// básica e do aluguel). Cresce com a renda: o pobre consome quase tudo.
// (5 = Pop::kIncomeStrata; static_assert em strata.cpp.)
constexpr double kStrataSavingRate[5] = {
	0.02, 0.04, 0.07, 0.12, 0.20};
// Ciclo de vida (B1): fator multiplicativo da taxa acima por faixa etária.
// 0-14 não poupa (não tem renda própria); 15-24 entra no mercado; 25-64 é o
// pico; 65+ passa a despoupar (renda fixa/transferência).
constexpr double kStrataLifecycleSaving[5] = {
	0.0, 0.40, 1.00, 1.15, 0.50};
// A riqueza é mais concentrada que a renda: participação no capital/aluguel
// proporcional a `income_share^expoente`. Expoente 1,6 na proxy calibrada do
// UBS 2024 (topo ~65-80% da riqueza) — declarado, sem dado por país no repo.
constexpr double kStrataWealthConcentration = 1.6;
// Fração MODELO da renda familiar que vem de capital + aluguel no dia da
// semeadura. Serve só para não contar duas vezes a mesma fatia: a renda
// salarial é semeada com (1 - esta fração) do quintil observado e o capital
// preenche o resto. Calibrada no sweep de 365 d (B2_strata.md §6).
constexpr double kStrataCapitalIncomeShare = 0.12;
// Fração MODELO da renda externa de subsistência repartida IGUAL por pessoa;
// o resto acompanha a renda de mercado (os serviços/pesca/turismo que geram a
// receita são de quem trabalha neles). 100% igual achatava o Gini dos países
// desiguais em ~12 pontos (BRA/ZAF no dia 5); 100% pro-rata tirava o piso de
// comida dos pobres. O meio-termo mantém o piso e a desigualdade observada.
constexpr double kStrataExternalEqualShare = 0.40;
// Mobilidade mensal entre quintis adjacentes: fração máxima da população do
// estrato que muda de faixa por mês. Pequena de propósito (a mobilidade real é
// de poucos pontos percentuais por década; aqui é um sinal, não o fenômeno).
constexpr double kStrataMobilityRate = 0.003;
// Gini da riqueza ~ Gini da renda + este degrau (UBS 2024: riqueza bem mais
// concentrada; usado só na proxy de riqueza, não no consumo).
constexpr double kStrataWealthGiniGap = 0.15;

// F9 — investimento e propriedade.
// Imposto corporativo sobre o lucro do ativo do grupo, pago ao orçamento do
// anfitrião. Retenção: fração do lucro líquido que fica no caixa do grupo
// (investimento seguinte); o resto é dividendo ao estrato dono no fim do dia.
constexpr double kCorporateTaxRate = 0.10;
constexpr double kGroupDividendShare = 0.80;
// Lote de ativos por grupo na semeadura (nação × setor). Mantém "algumas
// centenas" de grupos no mundo real em vez de um grupo por cidade.
constexpr int kGroupChunkAssets = 12;
// Nacionalização: confiança cai a 35% e só se recupera depois de 180 dias.
constexpr int kNationalizationDays = 180;
constexpr double kNationalizationConfidence = 0.35;
constexpr double kNationalizationCompensation = 0.5;

// ---------------------------------------------------------------------------
// F4 — frota e combustível.
//
// A frota inicial é DERIVADA de população/PIB (não é um número mágico):
// caminhão por 12,5 mil habitantes de simulação, navio por 250 mil, avião por
// 2,5 milhões, multiplicados por um fator de renda (PIB PPP) de 0,6 a 2,0.
// Cobertura calibrada no cenário real para ~6× o uso diário medido (~500
// pedidos/dia), de modo que a frota NÃO seja um gargalo no mundo baseline: ela
// existe, ocupa veículos e consome combustível, e passa a limitar quando há
// choque/expansão de comércio (a fila aciona a reposição do governo).
// Calibração F4 (economy_world_test --ai=1): a frota derivada original (1/2
// desta) fazia EGY cair de 1,000 (F3, 60 d) para 0,846 — os pequenos
// exportadores marítimos ficavam presos na coleta do cais e a comida estagnava
// em quem já estava abastecido. Com 2× e os pisos abaixo, 60 d e 365 d com IA
// ficam dentro do baseline F3 (365 d: 0 nações pioradas, 26 falências = base).
// Estoque estratégico inicial de combustível: 400 dias de uso estimado pela
// frota derivada (reserva declarada, `source: derived`). A reserva é o
// endowment de transição da F4: a reposição mensal do governo compra fuel no
// mercado interno (a compra entra em `desired` mesmo sem estoque) e a produção
// das fábricas de fuel da F2 substitui a reserva com o tempo. O teto global de
// 512 embarques/dia limita a variação do consumo, então 400 dias cobrem 1 ano
// de operação no cenário real sem transformar combustível em gargalo imediato —
// o portão "sem fuel não embarca" é real e testado em cenário controlado.
//
// Pisos de 16 caminhões / 4 navios: o piso antigo (8/2) deixava nações
// pequenas sem frota para a coleta no cais; com 2× na razão per capita o
// mundo fica dentro do baseline F3 em 365 d (0 nações pioradas, 26 falências =
// base) e o resíduo de 60 d é 0,021 em EGY — oscilação de chegada de comida,
// não regressão de balanço (ver GAME_STATE §10).
constexpr double kFleetTrucksPerCapita = 1.0 / 12500.0;
constexpr double kFleetShipsPerCapita = 1.0 / 250000.0;
constexpr double kFleetPlanesPerCapita = 1.0 / 2500000.0;
constexpr double kFleetMinTrucks = 16.0;
constexpr double kFleetMinShips = 4.0;
constexpr double kFleetFuelDays = 400.0;
// Reserva estratégica mínima por habitante de simulação (t/capita). Ordem de
// grandeza de um estoque nacional de combustível (não é medição): cobre a
// expansão industrial que a IA cria em 60–365 d, quando o volume de carga
// cresce ~7× sobre a estimativa da frota. O piso de 400 dias da frota continua
// valendo para nações pequenas.
constexpr double kFleetFuelPerCapita = 0.01;
// Consumo diário estimado por veículo (t/dia), usado para dimensionar o
// estoque-alvo de combustível. Derivado do uso medido (~0,09 t/caminhão/dia;
// navio e avião têm tanques/passagens maiores).
constexpr double kFleetFuelPerTruckDay = 0.12;
constexpr double kFleetFuelPerShipDay = 0.80;
constexpr double kFleetFuelPerPlaneDay = 1.50;
// Manutenção mensal: peças por veículo/mês (t). Um caminhão ~20 kg de
// componentes por mês; navio/avião uma ordem de grandeza acima.
constexpr double kFleetPartsPerTruckMonth = 0.02;
constexpr double kFleetPartsPerShipMonth = 0.40;
constexpr double kFleetPartsPerPlaneMonth = 0.15;
// Reserva inicial de peças, em meses de manutenção. Transição da F4: o mundo
// começa sem fábricas de `spare_parts`; 18 meses cobrem o horizonte de 365 d
// com folga (como os 400 dias de fuel) e evitam que a saúde da frota caia ao
// piso de 0,5 no meio do ano — o que recriava o gargalo de comida da F3.
// A reposição mensal continua mirando 3 meses, não a reserva cheia.
constexpr double kFleetPartsReserveMonths = 18.0;
constexpr double kFleetPartsTargetMonths = 3.0;
// Atrito mensal (acidentes/obsolescência) e saúde da frota.
constexpr double kFleetAttritionMonth = 0.002;
constexpr double kFleetHealthFloor = 0.50;
constexpr double kFleetHealthLossMonth = 0.10;
constexpr double kFleetHealthRecoveryMonth = 0.05;
// Fração do orçamento da cidade-sede que o governo pode gastar por compra.
constexpr double kFleetBudgetShare = 0.10;
// Cobertura de comida (dias de necessidade) exigida para o governo investir
// em frota. A comida tem prioridade no orçamento público: sem esta folga, o
// procurement mensal competia com a importação de alimento (AFG/LBY caíam em
// 365 d de IA). Abaixo do limiar, só há compra de emergência de fuel/peças.
constexpr double kFleetFoodSecureDays = 3.0;

// ---------------------------------------------------------------------------
// F5 — câmbio, tarifa e preço entregue.
//
// `rate` = valor em XGS de 1 unidade local. O preço de mercado fica na ÂNCORA
// XGS (fonte de verdade do núcleo e da IA); a moeda local é só EXIBIÇÃO
// (`price_local = price_anchor ÷ taxa`, calculado ao publicar o status) e nunca
// volta para a simulação. Isso remove o round-trip `(base ÷ taxa) × taxa` que
// desviava 1 ulp nas decisões discretas (§15, F5).
//
// Drift diário máximo de 0,05% e banda de ±10% em torno da taxa inicial. A
// regra é conservadora de propósito: a calibração da F4 é sensível à chegada
// de comida e uma desvalorização forte realimentaria a fome. A balança é
// suavizada em 30 dias (EMA) e normalizada pelo volume do comércio; déficit
// persistente desvaloriza, superávit valoriza (§4.4/§5.6).
constexpr double kFxDailyStep = 0.0005;
constexpr double kFxBandLow = 0.90;
constexpr double kFxBandHigh = 1.10;
constexpr double kFxBalanceEmaDays = 30.0;
constexpr double kFxBalanceScale = 0.10;

// ---------------------------------------------------------------------------
// Portos (ver docs/economy/port_design.md).
//
// Capacidade de nível 1 pela população de simulação da cidade-sede: piso de
// 1.000 t/dia (~0,4 Mt/ano, porto pequeno) e teto de 50.000 t/dia (~18 Mt/ano,
// calado de metrópole). O nível multiplica a base (1..N).
// ---------------------------------------------------------------------------
constexpr double kPortBaseTons = 1000.0;
constexpr double kPortTonsPerCapita = 0.002;  // 2 kg/habitante/dia
constexpr double kPortMaxTons = 50000.0;
// Capital público por t/dia de capacidade nova e prazo da obra.
constexpr double kPortCapitalPerTonCapacity = 500.0;
constexpr int kPortBuildDays = 30;
// Tempo de transbordo (carga/descarga) por operação no cais.
constexpr double kPortHandlingHours = 8.0;
// Fila: custo de demurrage por tonelada e por dia de atraso fracionário.
constexpr double kPortDemurragePerTonDay = 0.5;
constexpr double kPriceSmoothing = 0.20;

// ---------------------------------------------------------------------------
// D-092 — EQUILÍBRIO DE PREÇO (ver docs/economy/price_formation.md).
//
// A D-091 tirou a banda [0,5×, 3×] e deixou o alvo em `base × escassez`. Com
// escassez persistente o preço andava +5% do base TODO dia (ferro/carvão/
// petróleo/gás/bauxita chegaram a 19,25× em 365 d). O equilíbrio passa a ser
// custo + markup: o preço converge para o CUSTO UNITÁRIO da firma vezes
// (1 + markup), e o markup sobe com a falta relativa (demanda efetiva ÷ oferta
// disponível) e com a ocupação, caindo quando sobra estoque. O base do preço
// real inicial é o PISO (âncora leve), nunca um teto.
// ---------------------------------------------------------------------------
constexpr double kPriceMarkupBase = 0.08;             // margem normal (custo × 1,08)
constexpr double kPriceMarkupGain = 0.55;             // sensibilidade ao desequilíbrio (log)
constexpr double kPriceMarkupOccupancyGain = 0.25;    // prêmio por ocupação alta
constexpr double kPriceMarkupMax = 2.2;               // teto do MARKUP (não do preço)
// Passo diário: 5% do base (amortecimento antigo) OU 6% do preço de abertura,
// o que for maior. O segundo termo é o que permite CONVERGIR de um preço muito
// alto (choque) em semanas, em vez de descer só 5% do base por dia.
constexpr double kPriceMaxDailyMove = 0.06;

// D-092 — elasticidade-preço da demanda por classe de necessidade
// (básico → vida → luxo): básico pouco elástico, luxo muito elástico. A
// quantidade comprada cai com `(preço/base)^-elasticidade`, com piso para não
// zerar a cesta de quem tem renda.
constexpr double kPriceElasticityBasic = 0.10;
constexpr double kPriceElasticityLife = 0.55;
constexpr double kPriceElasticityLuxury = 1.20;
constexpr double kPriceElasticityQuantityFloor = 0.25;

// D-092 — firmas: com margem NEGATIVA (preço do produto abaixo do custo de
// insumos + trabalho/opex) a produção industrial cai — mas só quando o prejuízo
// é PROFUNDO (> 25% do custo), com piso de 25%. O piso/corte alto é deliberado:
// combustível/material de construção são insumos da frota e das obras; cortar
// a produção com qualquer margem negativa quebrava a logística de comida
// (medido no 365 d). O corte profundo por prejuízo persistente continua no
// `output_scale` (kLossDaysBeforeCut), como antes.
constexpr double kProductionMarginCutStart = 0.25;
constexpr double kProductionMarginFloor = 0.25;

// Estoque-alvo em dias de venda. A produção repõe metade da diferença por dia:
// sem estoque-alvo, produzir sempre 10% acima das vendas faria o estoque
// crescer para sempre (minas e fazendas com demanda externa fixa).
// Prejuízo persistente reduz a produção; lucro com demanda alta pode expandir.
constexpr int kLossDaysBeforeCut = 10;
constexpr double kLossCut = 0.95;
constexpr double kMinOutputScale = 0.20;
// Dias seguidos de prejuízo até a firma fechar a construção (falência). Depois
// de kLossDaysBeforeCut a produção já foi cortada ao piso; se o prejuízo
// persistir até aqui, a firma encerra a atividade em vez de ficar no vermelho
// para sempre.
constexpr int kLossDaysBeforeClose = 60;
// Capital de giro: as empresas retêm caixa suficiente para importar e pagar
// salários antes de distribuir lucros.
constexpr double kFirmCashPerCapita = 0.10;
constexpr double kServiceCashPerCapita = 0.05;
// F8: caixa do setor de serviços = capital de giro em dias de folha + insumos.
// O excedente do lucro é renda da POP dona (ver tick / distribuição).
constexpr double kServiceCashBufferDays = 8.0;
// F8: a fração de serviços respeita a cesta do dia — em cidade que importa
// comida o serviço não pode consumir o que compra o alimento. 0 desliga o
// guarda (comportamento F7: 45% da renda doméstica, cesta depois).
constexpr double kServiceFoodGuardDays = 1.0;
// Serviços ajustam o emprego para cobrir a folha com a própria receita.
constexpr double kServiceAdjust = 0.10;
// O salário de serviços busca o equilíbrio do mercado de trabalho residual,
// com piso no custo de subsistência (uma cesta alimentar por pessoa). Sem
// isso o salário fixo de 0,15/dia (3x o PIB per capita) só financiava ~10%
// da população e o desemprego aparecia em 80-90% por construção.
// Desemprego friccional: parte da força de trabalho residual está sempre
// procurando vaga. Sem isso o mercado de trabalho perfeitamente flexível
// produziria 0,0% de desemprego, o que também não é plausível.
// Demanda externa de ferro (fora do consumo alimentar): o ferro flui pelo
// mesmo mecanismo de teto data-driven dos demais bens brutos (fase 4d,
// external_demand declarado pelo GeographyService), não por um caso especial.
// Primeiro produto "bruto" (sem consumo local) na ordem de goods.json: 1=ferro
// (ver sim/sim_constants.h — kFirstRawProduct e as constantes de extração
// mineral mudaram para lá na S5b).


// Aptidão de referência do território de AGRIA (trigo, dado real de
// agriculture.json). Mantém a produção validada de 20 t/dia e serve de valor
// inicial até o GeographyService carregar o dado em tempo de execução.
constexpr double kReferenceWheatSuitability = 0.702;
constexpr double kFarmCapacityTons = 20.0;
// Capital necessário para adicionar 1 t/dia de capacidade produtiva.
constexpr double kCapitalPerCapacity = 2000.0;

// Inicialização do mundo real (initialize_world). Valores derivados do
// cenário de validação AGRIA/PORTUS (fazenda de 20 t/dia, 150 empregos,
// salário 2.0 e opex 50), escalados para a população/construções reais.
constexpr double kSavingsPerCapita = 0.50;
constexpr double kGovernmentPerCapita = 1.0;
constexpr double kJobsPerTon = 7.5;   // 150 empregos / 20 t
constexpr double kFarmWage = 2.0;     // igual à fazenda demo
constexpr double kOpexPerTon = 2.5;   // 50 opex / 20 t
// Fração da necessidade diária mantida como estoque antes de exportar. O
// vendedor já consumiu a própria necessidade na fase 5; a produção da fase 4
// repõe no dia seguinte. Buffer alto aqui espreme as cidades pequenas (a oferta
// vendável fica menor que a demanda e a última compradora — ex.: Cayenne — não
// recebe nada).
constexpr double kTradeBufferDays = 0.0;
// Teto de cargas de comércio por dia (proteção contra loop). Com 40 cidades,
// 64 bastava; com 252 e 66 nações, 64 embarques/dia deixavam as cidades de
// índice alto (SAU, CHN...) permanentemente fora da fila — todas as
// compradoras com estoque zero empatam em cobertura relativa 3,0 e as 64
// primeiras do índice consumiam o teto todos os dias (§19). 512 cobre o pico
// de reabastecimento mundial e cai sozinho quando os estoques enchem.
constexpr std::size_t kMaxShipmentsPerDay = 512;
// Capacidade de transporte diária de uma cidade, por habitante (t/dia). 1 kg
// por habitante/dia (~3x a necessidade alimentar de 0,3 kg) — um teto físico
// para o comércio não mover carga "infinita" num único dia, sem apertar o fluxo
// normal de comida.
constexpr double kTradeCapacityPerCapita = 0.001;

constexpr std::size_t kTradeExhaustiveLimit = 24;
// Célula do índice espacial do comércio, em unidades do mapa (~8 km cada):
// 32 unidades ≈ 250 km. O comprador varre a grade em anéis até juntar K
// vendedores, então a célula só afeta a constante do custo de busca.
constexpr double kTradeCellMapUnits = 32.0;

// ---------------------------------------------------------------------------
// §19 — solo urbano, zoneamento e imóveis (W0).
//
// Estrutura do estoque: m² de piso por unidade para cada densidade × padrão
// (casas são maiores que apartamentos; o padrão alto ocupa mais área). São
// parâmetros de modelo — a fonte publica aluguel/preço por m², não por unidade.
constexpr double kUrbanUnitM2[3][3] = {
	{60.0, 100.0, 180.0},  // DENSITY_LOW: casa
	{55.0, 85.0, 140.0},   // DENSITY_MID: prédio
	{50.0, 75.0, 110.0},   // DENSITY_HIGH: torre
};
// FAR (área construída ÷ área do lote) por densidade. É o ENVELOPE do lote —
// o máximo que o gabarito permite —, não o FAR bruto realizado na escala da
// cidade (esse é 5-15× menor: a maior parte do solo construído é rua, pátio,
// uso misto e construção antiga; ver kUrbanZoneFill abaixo). Capacidade
// física da zona = área × FAR ÷ m² por unidade (W1).
constexpr double kUrbanFar[3] = {1.0, 2.5, 6.0};
// Ocupação RES ALVO no dia 0, por FAIXA de população urbana (frente Q-015-C):
// <300 mil, 300 mil–1 M, 1–5 M, >5 M. É CALIBRAÇÃO declarada, não dado
// observado — a fonte não publica "quão cheia" cada cidade está. O alvo
// reproduz a hierarquia real do mercado de solo: metrópole nasce quase cheia
// (0,85), cidade média com folga moderada (0,70-0,72), cidade pequena com
// folga (0,55), para o preço da terra subir com o crescimento e a prefeitura
// só precisar de solo novo quando a cidade enche. O dado de base do estoque
// vem de UN DESA (tamanho do domicílio), Atlas of Urban Expansion 2016 (área
// construída e densidade) e World Bank SDG 11.1.1 (informalidade) — ver
// docs/team/reviews/Q-015_fix_zonas.md para a distribuição medida antes/depois.
constexpr double kUrbanBandOccupancy[4] = {0.55, 0.70, 0.72, 0.85};
// Teto de ocupação inicial/efetiva da moradia (fração do envelope). Um estoque
// ACIMA do envelope calibrado (área do Atlas 2014 × população 2026, como
// NPL-POK) não é cidade com 137% de ocupação: a capacidade efetiva tem piso
// em estoque ÷ este teto. Cidade nesse teto está fisicamente cheia — moradia
// nova ali exige expandir ou adensar (o envelope cresce), não empilhar mais
// unidade no mesmo lote.
constexpr double kUrbanOccupancyCap = 0.95;
// Taxa de capitalização fixa (aluguel anual ÷ valor do imóvel) — §19.6.
constexpr double kUrbanCapRate = 0.05;
// Depreciação anual do estoque (fração do valor/condição) — §19.3.
constexpr double kUrbanDepreciationPerYear = 0.01;
// Custo de infraestrutura por km² expandido (XGS). ESCALA: meses de receita
// municipal alvo (municipal_finance.json × PIB do mês). A fonte não publica
// custo de urbanização por cidade e a escala monetária do modelo não é a do
// PIB real (o PIB simulado de São Paulo é ~4 ordens de grandeza menor que o
// observado), então ancorar em US$/km² da literatura não transporta. O que se
// reproduz é a RESTRIÇÃO OBSERVADA: com a manutenção consumindo 45% do alvo,
// sobram 55% do ano para capital, e 2 meses de receita por km² fazem uma
// metrópole abrir ~3 km²/ano — "alguns km²/ano" da spec — antes de o custo
// crescente (raio e relevo) comer o orçamento.
//   conta: 12 meses × 0,55 ÷ 2 meses/km² = 3,3 km²/ano no raio inicial.
// Sem dado municipal (cenário sintético), usa o piso declarado abaixo.
constexpr double kUrbanInfraCostMonths = 2.0;
constexpr double kUrbanInfraFloorPerKm2 = 1000000.0;
// Custo crescente do SOLO (spec §19, "Custo crescente"): o custo por km² sobe
// com o raio da cidade (redes mais longas até o centro) e com a fração da
// reserva já consumida (o que sobra é o terreno pior). Fatores lineares
// declarados: +50% quando o raio dobra; +80% quando a reserva toda foi usada.
constexpr double kUrbanExpandDistanceK = 0.5;
constexpr double kUrbanExpandReliefK = 0.8;
// Taxa local sobre o faturamento de serviços da cidade (fração) usada enquanto
// o W2 não calibra a receita municipal contra o dado real.
constexpr double kUrbanMunicipalFeeRate = 0.02;

// ---------------------------------------------------------------------------
// §19 W1 — solo urbano e zoneamento.
//
// Capacidade das zonas COM e IND medida em TRABALHADORES, não em "negócios":
// m² de piso por trabalhador. Referências de ordem de grandeza (declaradas,
// não medições por cidade):
//  - serviços/escritório: ~25 m² de área útil por trabalhador (faixa OECD de
//    espaço de escritório 15–35 m²/pessoa; lojas e restaurantes ficam abaixo,
//    escritórios acima — 25 é o meio);
//  - indústria urbana: ~150 m² por trabalhador (faixa 100–200 m²/pessoa em
//    parques industriais e galpões com pátio e logística).
// Assim a zona limita pelo EMPREGO que cabe no gabarito, e não pelo número de
// estabelecimentos agregados — metrópole cheia encosta na capacidade de
// verdade quando o emprego de serviço/indústria enche o FAR permitido.
constexpr double kUrbanServiceM2PerWorker = 25.0;
constexpr double kUrbanFactoryM2PerWorker = 150.0;
// Preço da terra = esta fração do valor do imóvel médio (XGS/m²). A fonte não
// publica preço de terra por cidade; a parcela do terreno no imóvel é o proxy
// declarado (W0 deixou o campo em 0 de propósito).
constexpr double kUrbanLandShareOfHousePrice = 0.30;
constexpr double kUrbanLandShareOfHousePriceCap = 0.45;
// Curva de ocupação -> pressão de preço: convexa perto de 1 e com teto
// declarado (evita preço explosivo quando a ocupação passa de 1).
constexpr double kUrbanLandMaxPressure = 8.0;
// Atualização mensal do preço da terra: fração do caminho até o alvo por mês
// (suave, não um salto).
constexpr double kUrbanLandPriceSmooth = 0.25;
// Renda média local no alvo do preço: ganho linear em (índice de renda − 1),
// limitado a ±0,5/1,0 do índice.
constexpr double kUrbanLandIncomeGain = 0.5;
// Demanda de zona sinalizada decai à metade a cada mês.
constexpr double kUrbanZoneDemandDecay = 0.5;
// População que a infraestrutura passa a atender por km² expandido.
constexpr double kUrbanInfraPopPerKm2 = 5000.0;
// Adensar fica mais caro a cada nível (spec §19, "Custo crescente"):
//  - LOW→MID exige reforço de água/esgoto/energia: 0,35 do km² de expansão;
//  - MID→HIGH exige estrutura de aço, elevadores e redes pesadas: 0,85.
// Os custos são uma fração do custo de infraestrutura por km² (mesma obra,
// sem terra nova) e o nível alto ainda troca a receita de material para
// steel_structure/electrical_material.
constexpr double kUrbanDensifyCostFactorLowMid = 0.35;
constexpr double kUrbanDensifyCostFactorMidHigh = 0.85;
// Mistura de material do adensamento MID→HIGH: pesos relativos aplicados sobre
// a receita de material do §15, que continua somando a mesma fração do custo.
constexpr double kUrbanDensifyHighSteelWeight = 1.8;
constexpr double kUrbanDensifyHighElectricalWeight = 1.6;
// Régua da obra de infraestrutura: km² -> unidades de capacidade do §15 (só
// escala o prazo; a obra urbana não cria prédio).
constexpr double kUrbanKm2ToConstructionUnits = 20.0;
// Fechamento do "mês" do calendário do jogo para preço da terra e decadência
// da demanda de zona. A6: o dia do fechamento virou `cadence.monthly_period`
// (estado do mundo, escalonado por cidade); esta constante fica como registro
// do calendário de 30 dias.
constexpr int kUrbanMonthDays = 30;

// §19 W3 — MERCADO DE MORADIA (sim/housing.cpp). Parâmetros de MODELO, não
// dados observados; o ônus de aluguel e a cap rate vêm da fonte (OECD AHD
// HC1.2 e §19.6).
// ---------------------------------------------------------------------------
// Ônus de aluguel: a fonte publica a mediana dos inquilinos (0,28). O nível
// inicial do aluguel é REANCORADO na renda medida do núcleo nessa fração
// (housing_city.json publica USD/m² — escala ~2,7 ordens de grandeza acima da
// renda simulada em XGS; o valor absoluto da fonte não é transportável).
constexpr double kHousingInitialBurden = 0.28;
// Teto de ônus de aluguel do inquilino: o aluguel de um domicílio não passa
// desta fração da renda mensal do domicílio (trava de longo prazo).
constexpr double kHousingRentBurdenCap = 0.40;
// Piso da faixa de ônus de aluguel (Q-015: 0,15–0,40). O nível é reancorado
// na RENDA CORRENTE quando o ônus de MERCADO do mês fechado cai abaixo do piso:
// a âncora de renda é a conversão do nível da fonte (renda de hoje, não a do
// dia 1), e a renda nominal cresce com a industrialização. Sem o piso, o
// aluguel ancorado no dia 1 fica congelado e o ônus medido cai para 0,08–0,14
// (AUS-WEI/IND-MUM em 365 d). Não é movimento de mercado: o passo de ±2%/mês
// por desequilíbrio continua sendo o único sinal de oferta/demanda.
constexpr double kHousingBurdenFloor = 0.15;
// Reserva de comida (dias de cesta) que o aluguel NUNCA toca: comida tem
// prioridade sobre moradia (spec §19/W3).
constexpr double kHousingFoodReserveDays = 30.0;
// Ajuste mensal do aluguel: passo máximo (±2%/mês) e elasticidade ao
// desequilíbrio demanda × oferta.
constexpr double kHousingRentStepMax = 0.02;
constexpr double kHousingRentElasticity = 0.50;
// Custo de construção de uma unidade em ANOS DE ALUGUEL do padrão (parâmetro
// adimensional, independente da escala monetária do mundo) — a terra entra
// separada pelo land_price da zona residencial (W1). Uma unidade vale 20 anos
// de aluguel (§19.6, price = aluguel × 12 ÷ 0,05); construir custa 6 (30% do
// valor), o que deixa margem para a incorporadora em TODO o mercado, inclusive
// no mais caro da amostra (Q-015/B1).
//
// Calibração documentada (não é dado observado; é o teto que o modelo admite):
// com terra = 30% do valor do imóvel médio (kUrbanLandShareOfHousePrice) e
// margem declarada de 20%, a condição price ≥ 1,2 × (obra + terra) exige
// B ≤ 0,5333 ÷ cap_rate = 10,67 anos no caso neutro; no mercado mais caro
// (JPN-TOK, terra ainda × ~1,47 pelo índice de renda local) o teto medido é
// B ≤ 6,66 anos. B = 14 (70% do valor) + terra (30–44%) somava 100–114% do
// preço — a margem era matematicamente INALCANÇÁVEL e nenhuma unidade saía.
// B = 6 fica 10% abaixo do teto de Tóquio e equivale a custo de obra ≈ 30% do
// valor de mercado, dentro da faixa de custo de reposição observada em
// metrópoles caras (estrutura ~25–40% do preço; o resto é terreno).
constexpr double kHousingBuildCostYears = 6.0;
constexpr double kHousingDeveloperMargin = 0.20;
// Teto da participação do TERRENO no valor da unidade (Q-015, integração). A
// terra entra por `m² construído ÷ FAR × preço do m² de solo` (o lote que
// sustenta a unidade, não a área construída); em zona de FAR baixo e preço de
// solo alto (curva de pressão saturada perto da ocupação 1) o terreno chegaria
// a mais de 100% do valor — solo valendo mais que a construção que ele
// sustenta. Faixa observada da participação do terreno no preço: 20-50% em
// metrópoles (Tokyo/Mumbai no topo, Lagos/São Paulo abaixo). 0,45 é a
// calibração declarada do teto e mantém a margem do incorporador (obra 30% +
// terreno ≤45% = custo ≤75% do valor contra a exigência de ≤83,3%).
constexpr double kHousingLandShareMax = 0.45;
// Parcela MATERIAL da obra de moradia = a MESMA receita do §15 (Q-015/§5): a
// obra de moradia não é exceção da fila única; material é o que o §15 declara
// (15% do orçamento, o resto é mão de obra/serviço). A antiga fração de 40%
// fazia a mesma obra em XGS exigir 2,7× mais material sem decisão declarada.
constexpr double kHousingMaterialShare = kConstructionMaterialShare;
// Prazo de obra de moradia (dias) e tamanho do lote de unidades por mês.
constexpr int kHousingBuildDays = 90;
constexpr double kHousingBatchShare = 0.005;     // 0,5% da demanda do padrão
constexpr double kHousingSelfBuildShare = 0.002; // autoconsntrução popular
// Manutenção paga pelos donos (fração do aluguel) ao setor de serviços.
constexpr double kHousingMaintenanceShare = 0.10;
// Favela (ÚLTIMO caso): falta popular mínima, vacância total máxima, dias
// consecutivos de aperto, fatia de domicílios por mês e penalidade de
// satisfação por fatia informal (limitada a 5 p.p. pela própria fração).
// A falta é medida na demanda PRÓPRIA do padrão popular (sem o transbordo dos
// padrões altos): a depreciação do estoque médio não pode fabricar falta
// popular (Q-015/B5).
constexpr double kHousingSlumShortage = 0.05;
// Falta popular SEVERA: 1 em 5 domicílios pobres sem unidade formal. Nesse
// ponto a falta deixou de ser de preço e é de estoque, então a área informal
// abre mesmo com o aluguel abaixo do teto de ônus (calibração declarada; o
// caso de preço — aluguel no teto de ônus — já abre pela própria trava).
constexpr double kHousingSlumSevereShortage = 0.20;
constexpr double kHousingSlumVacancyMax = 0.01;
constexpr int kHousingSlumDays = 30;
constexpr double kHousingSlumShare = 0.01;
constexpr double kHousingSlumSatisfaction = 0.05;

// ---------------------------------------------------------------------------
// B7 — INFORMALIDADE, SERVIÇOS INFORMAIS, MORTALIDADE E INSATISFAÇÃO.
//
// O DADO é a taxa de emprego informal por nação (ILOSTAT/ILO,
// data/definitions/economy/informality.json) e a cobertura de saúde de
// referência (WHO UHC SCI, data/definitions/population/health_access.json).
// Tudo abaixo é MODELO DECLARADO, calibrável no laboratório — nenhum número
// por país inventado. Fontes/parâmetros em docs/economy/B7_informality.md
// (D-093).
// ---------------------------------------------------------------------------
// Cobertura de saúde usada como referência neutra quando a nação não tem dado.
constexpr double kHealthAccessDefault = 0.5;
// Gradiente de informalidade por quintil (MODELO): a informalidade se concentra
// na base da distribuição. O quintil só REPARTE a base nacional; a média
// ponderada por população reproduz exatamente o dado.
constexpr double kInformalQuintileGradient[5] = {1.55, 1.30, 1.00, 0.75, 0.45};
// Informalidade rural maior que a urbana (fator sobre a base nacional).
constexpr double kRegionInformalRuralFactor = 1.15;
// Formalização mensal (cadência A6, mesmo dia do fechamento de estratos):
// velocidade de convergência ao alvo (fração/mês) e os pesos dos drivers
// (renda, escolaridade, infra B5, enforcement de política — futuro).
constexpr double kInformalFormalizationRate = 0.02;
constexpr double kFormalizationIncome = 0.35;
constexpr double kFormalizationEducation = 0.25;
constexpr double kFormalizationInfra = 0.20;
constexpr double kFormalizationEnforcement = 0.0;
// Renda do trabalho informal: fração da formal (mesmo quintil). A folha total
// NÃO muda — o peso do quintil é renormalizado (conservação de dinheiro).
constexpr double kInformalWagePenalty = 0.75;
// Crise: quando a folha é paga com desconto de liquidez (pay_scale < 1), a
// renda informal sofre o dobro (o que o informal deixa de receber fica no
// caixa de quem pagou).
constexpr double kInformalCrisisSensitivity = 2.0;
// Serviços informais (agregado, sem firma individual): emprego como fração da
// força informal, produtividade por trabalhador, desconto de preço e os
// parâmetros de demanda (base, elasticidade de preço, desemprego, teto).
constexpr double kInformalServiceEmploymentShare = 0.60;
constexpr double kInformalServiceUnitsPerWorker = 1.0;
constexpr double kInformalProductivityFactor = 0.55;
constexpr double kInformalPriceDiscount = 0.60;
// Base da demanda informal em PREÇO NORMAL (formal/informal = 1/0,60 ≈ 1,667,
// desemprego ~5%, low_share 0,4): 0,20 × 1,667 × 1,05 × 0,7 ≈ 0,245 da cesta
// doméstica de serviços. Com 0,55 a fatia passava de 2/3 no dia 1 e sufocava o
// serviço formal (fechamento em massa) — concorrência real, não takeover.
constexpr double kInformalDemandBase = 0.20;
constexpr double kInformalDemandPriceElasticity = 1.0;
constexpr double kInformalDemandUnemployment = 1.0;
constexpr double kInformalDemandMax = 0.75;
// Mortalidade ligada a comida e saúde. A tabela observada (CBR/CDR) é a BASE
// (multiplicador 1 em situação normal); fome e falta de saúde adicionam o
// excesso, com teto declarado. Crianças (0-14) e idosos (65+) pesam mais.
constexpr double kFoodMortalityThreshold = 0.90;
constexpr double kFamineMortalityWeight = 1.0;
constexpr double kHealthMortalityWeight = 0.30;
constexpr double kMortalityMaxExcess = 3.0;
constexpr double kAgeMortalityVulnerability[5] = {1.6, 0.8, 0.7, 1.0, 1.9};
// Fome prolongada empurra a migração B4: após estes meses consecutivos de fome,
// o êxodo rural→urbano e a saída cidade→cidade recebem esta pressão extra
// (proporção declarada), limitada pela vaga de moradia existente.
constexpr int kFamineMigrationMonths = 3;
constexpr double kFamineMigrationBoost = 0.50;
// Insatisfação (só medição + status, 0..1): pesos dos componentes (soma 1).
// Terra/patrimônio é gancho da B6 (peso 0 por enquanto).
constexpr double kDissatisfactionUnemployment = 0.30;
constexpr double kDissatisfactionRent = 0.20;
constexpr double kDissatisfactionFood = 0.25;
constexpr double kDissatisfactionInflation = 0.15;
constexpr double kDissatisfactionInformality = 0.10;
constexpr double kDissatisfactionLand = 0.00;
// PARTE E (D-116): falta de energia — peso do componente no bloco urbano e no
// interior. O valor do componente é `City::power_stress` (0..1), que já cresce
// com a DURAÇÃO do blecaute (ver power.cpp). O termo é ADITIVO sobre o índice
// anterior (o divisor segue o antigo: urbano 1,00, interior 0,50), então com
// energia plenamente servida a insatisfação é a mesma de antes da energia.
constexpr double kDissatisfactionPower = 0.20;
// Jovens (15-24) pesam mais na insatisfação com desemprego.
constexpr double kDissatisfactionYouthWeight = 0.50;

// ---------------------------------------------------------------------------
// G — AGRICULTURA SAZONAL (D-096).
//
// O DADO é: área colhida e rendimento por país/cultura (FAOSTAT), janelas de
// plantio/colheita (FAO/JRC/USDA), perda pós-colheita (APHLIS/FAO 2011),
// fertilizante N por cultura (IFA FUBC), estoque de fim de safra (USDA PSD) e
// dias de prateleira (FoodKeeper/FAO INPhO). O que segue é MODELO DECLARADO —
// calibrável no laboratório, sem número por país inventado:
// ---------------------------------------------------------------------------
// Ano civil do dia 0 do jogo (1º de janeiro). É a MESMA âncora do autoload
// (`Economy.START_UNIX` = 2027-01-01T00:00:00Z): o núcleo não tem calendário
// próprio, ele deriva ano/dia-do-ano desta constante + `WorldState::day`.
constexpr int kAgriStartYear = 2027;
// Fração da perda pós-colheita que incide no ATO de entrada no silo (o dado
// `loss_pct` cobre manuseio + armazenagem). 0,60 = a maior parte da perda
// acontece antes/durante a entrada; o resto vira perda mensal de estoque. É
// MODELO declarado (o dado não separa as duas etapas por país; quando
// `storage_share_pct` existe ele é usado direto na perda mensal).
constexpr double kAgriEntryLossShare = 0.60;
// Agravamento da perda quando o armazém é ruim: fator = 1 + k*(1 - q), com
// q = qualidade 0..1. Com q = 0 a perda é 2,5× a publicada; com q = 1, 1×.
constexpr double kAgriStorageLossAggravation = 1.5;
// Teto da perda somada, em fração (proteção contra dado extremo + q = 0).
constexpr double kAgriLossCap = 0.95;
// Perecibilidade: MEIA-VIDA do estoque parado = `storage_days` do produto
// (regra declarada: sem lote/idade por tonelada no núcleo, o decaimento é
// exponencial e o prazo de prateleira vira a meia-vida). Produto com
// `data_status = no_data` (soja, farelo, ração) NÃO decai.
constexpr double kAgriShelfHalfLife = 0.5;
// Conteúdo de N do bem `fertilizer` (t de N por t de fertilizante). O bem é
// genérico (ureia 46%, nitrato de amônio 34%, blends NPK 15-20%); 0,30 é a
// média declarada de um mix, calibrável na Parte E (gás/fosfato).
constexpr double kAgriFertilizerNContent = 0.30;
// Piso de rendimento quando NENHUM fertilizante foi comprado: a safra de
// sequeiro rende esta fração do rendimento com insumo. A deficiência de
// nutrientes responde por boa parte do hiato de produtividade (FAO), mas zerar
// a safra por falta de estoque no mercado transformaria a ausência da indústria
// de fertilizante (Parte E, ainda não existe no mundo) em fome mundial. MODELO
// declarado, calibrável com a Parte E.
constexpr double kAgriNoFertilizerYieldFloor = 0.35;
// Qualidade do armazém DERIVADA da infra B5 quando `set_storage_quality` não
// foi chamado: piso (nenhuma infra) + ganho pela cobertura ponderada.
// Energia pesa mais (aeração/refrigeração), depois estrada (silo chega ao
// porto/mercado), por último água (limpeza/umidade). MODELO declarado.
constexpr double kAgriStorageQualityFloor = 0.15;
constexpr double kAgriStorageQualityGain = 0.85;
constexpr double kAgriStorageQualityWeight[3] = {0.35, 0.45, 0.20};
// Dias de estoque de referência quando o dado de estoque inicial não existe por
// nação/cultura (`crop_stocks._meta.rule_days_default` = 60). O
// AgricultureService aplica a regra; a constante fica aqui como registro.
constexpr int kAgriStockRuleDays = 60;

// ---------------------------------------------------------------------------
// B6 — PATRIMÔNIO IMOBILIÁRIO DA REGIÃO (terra, moradia do interior e anel
// periurbano da cidade-polo). Cadência MENSAL (A6).
//
// O DADO é a área agrícola em hectares por nação e o Gini fundiário
// (data/definitions/economy/land_property.json, FAOSTAT 2023 + FAO LINEQ; a
// regra de rateio nacional→estado está declarada no próprio arquivo e é
// aplicada por core/economy/land_property.gd). Os PREÇOS não têm dado: são
// MODELO DECLARADO — capitalização da renda fundiária medida no núcleo, com a
// infra B5 e o preço das commodities. Nenhum valor de ativo entra em
// `money_total`: só o aluguel do anel é fluxo (débito e crédito na região).
// Documento: docs/economy/B6_region_property.md (D-095).
// ---------------------------------------------------------------------------
// Cap rate ANUAL do ativo: o MESMO 5%/ano do mercado de moradia (§19.6,
// `price = aluguel × 12 ÷ 0,05`). Vale para a terra (renda capitalizada) e
// para a moradia rural/anel — um único fator de capitalização no projeto.
constexpr double kPropertyCapRate = 0.05;
// Valorização da TERRA: alvo = renda fundiária ANUAL por hectare ÷ cap rate.
// A renda fundiária é a folha primária do interior (`region.wage_income`, base
// diária) × 30 — "a renda das fazendas/minas da região". Amortecimento mensal
// declarado: fração da distância ao alvo e passo máximo por mês.
constexpr double kPropertyValueDamping = 0.25;
constexpr double kPropertyValueStepMax = 0.10;
// A infra B5 valoriza a terra: alvo × (1 + ganho × cobertura), com a cobertura
// combinada dos MESMOS pesos da produtividade primária (kInfraProductivityWeight).
constexpr double kPropertyInfraValueGain = 0.50;
// Índice de commodities: média (peso = capacidade de extração) do preço
// corrente ÷ preço-base D0 dos produtos PRIMÁRIOS da cidade-polo (fazendas e
// minas). Faixa declarada — boom eleva, quebra derruba.
constexpr double kPropertyCommodityIndexMin = 0.25;
constexpr double kPropertyCommodityIndexMax = 4.00;
// Moradia do INTERIOR (autoconstruída): fração do preço da unidade POPULAR do
// polo. O interior não tem mercado de solo (§B3) — casa própria construída,
// sem aluguel de mercado; o valor é patrimônio do dono.
constexpr double kPropertyRuralHomeShare = 0.40;
// ANEL PERIURBANO: preço = preço popular do polo × desconto de distância
// (kPropertyRingDistanceDiscount por 100 km do raio efetivo da região, derivado
// da área real dos estados) × (1 − penalidade da estrada B5).
constexpr double kPropertyRingDistanceDiscount = 0.35;
constexpr double kPropertyRingRoadPenalty = 0.30;
// Absorção do anel: fração da população urbana que a periferia informal
// consegue assentar POR MÊS quando o polo está cheio (o "construtor" do anel;
// sem ela o excedente continuaria barrado pela vaga da B4).
constexpr double kPropertyRingMonthlyAbsorb = 0.005;
// Pressão de aluguel no anel: o aluguel sobe com a fatia da população que vive
// nela (periferia cheia = aluguel maior).
constexpr double kPropertyRingRentPressure = 0.75;
// Imposto sobre a propriedade (anual, sobre o valor de mercado). 0,0 por padrão:
// a alíquota é CAMPO persistido e publicado (receita potencial), mas a
// arrecadação é do macro político — nada é arrecadado enquanto for 0.
constexpr double kPropertyTaxDefaultRate = 0.0;

// ---------------------------------------------------------------------------
// G2 — ARMAZENAGEM, RESERVA NACIONAL E CAMADA NACIONAL (save v25).
// O DADO (capacidade real de silos/reserva estratégica por país e o mapeamento
// produto→tipo) chega em paralelo (`data/g2-storage`). Até ele entrar, a REGRA
// DECLARADA é "meses de produção": a capacidade inicial de cada tipo nasce dos
// meses de produção/consumo já conhecidos pelo núcleo. Nenhum número por país
// é inventado; os valores abaixo são MODELO calibrável no laboratório.
// ---------------------------------------------------------------------------
// Capacidade inicial (regra declarada) como MESES de produção da região/cidade.
// D-101: o `StorageService` aplica `capacidade = cobertura_real (meses) ×
// produção anual do jogo / 12` — o dado real entra como COBERTURA, não como
// tonelagem absoluta; esta constante é a cobertura usada quando o dado não tem
// meses publicados para o tipo.
constexpr double kStorageInitialMonths = 2.0;
// Fatia da capacidade inicial declarada como RESERVA DO GOVERNO. O resto é
// estoque privado (produtor/cooperativa/comerciante). Modelo declarado: a
// reserva nacional é uma parte da estrutura, não a estrutura inteira.
constexpr double kStorageGovernmentShare = 0.25;
// Custo de obra por tonelada de capacidade (XGS/t) por tipo. Ordem de grandeza
// de um silo metálico/câmara fria/tanque; MODELO declarado, calibrável.
constexpr double kStorageCostPerTon[8] = {
	35.0,   // silo graneleiro
	120.0,  // câmara fria / frigorífico
	80.0,   // tanque de petróleo/combustíveis
	90.0,   // armazenagem de gás
	25.0,   // pátio de granéis
	60.0,   // armazém geral
	150.0,  // cofre (só estrutura nesta parte)
	140.0,  // paiol militar (só estrutura nesta parte)
};
// Manutenção diária por tonelada instalada (XGS/t/dia), paga pelo dono ao setor
// de serviços (dinheiro conservado). ~1,1 XGS/t/ano no silo.
constexpr double kStorageMaintenancePerTonDay[8] = {
	0.003, 0.010, 0.007, 0.008, 0.002, 0.005, 0.012, 0.012,
};
// Prazo base da obra de armazém (dias): independe do tamanho (o cronograma §15
// escala pelo custo/capacidade como as demais obras primárias).
constexpr int kStorageBuildDays = 90;
// Pulmão região→cidade: fração do estoque liberada por dia (1/k dias). O silo
// da região esvazia ao longo de ~2 meses, não no dia da colheita.
constexpr double kStorageLungReleaseDays = 60.0;
// Câmara fria sem energia (infra B5 = 0): a perda por perecibilidade é
// multiplicada por este fator (campo para a Parte E; sem energia não há frio).
constexpr double kStorageColdNoPowerLossFactor = 3.0;
// IA privada: spread esperado safra→entressafra por tipo quando ainda não há
// histórico de preço observado (declarado; o histórico vence quando existir).
constexpr double kStorageExpectedSpread[8] = {
	0.18, 0.22, 0.15, 0.15, 0.12, 0.15, 0.05, 0.05,
};
// Payback mínimo exigido da obra (spread × volume ÷ (obra + manutenção + perda)
// em anos). Abaixo disso a IA privada não constrói.
constexpr double kStoragePrivatePaybackYears = 6.0;
// Perfil especulador: Gini de renda acima deste valor guarda mais e vende mais
// perto da colheita; abaixo, perfil conservador. Dado (Gini) define o perfil.
constexpr double kStorageSpeculatorGini = 0.45;
// IA privada: fração máxima do caixa da firma comprometida numa compra/obra.
constexpr double kStoragePrivateCashShare = 0.10;
constexpr double kStoragePrivateBuildCashShare = 0.25;
// IA privada: não compra comida quando a satisfação alimentar local está abaixo
// deste piso, e libera o estoque de comida (anti-fome em tempo de paz).
constexpr double kStoragePrivateBuySatisfactionFloor = 0.90;
constexpr double kStoragePrivateReleaseSatisfaction = 0.85;
// IA do governo: compra na colheita até esta fração do EXCEDENTE do mercado
// (`Market::surplus`, o estoque acima de 3 dias de demanda) e libera na
// entressafra, na falta e na crise até esta fração do estoque guardado.
constexpr double kStorageGovernmentBuyShare = 0.10;
constexpr double kStorageGovernmentReleaseShare = 0.20;
// D-101 — PISO DE SEGURANÇA ALIMENTAR da armazenagem (consumido/necessidade,
// o mesmo número que a IA já lê como satisfação). Vale para toda decisão que
// RETIRA comida do mercado: a IA do governo só forma reserva acima dele, o
// pulmão do silo não desvia colheita abaixo dele e a reserva guardada é
// liberada abaixo dele. Comida no silo não pode deixar a cidade com mais fome.
constexpr double kStorageFoodSecureSatisfaction = 0.95;
// IA do governo: preço "baixo" / "alto" vs. referência (histórico/base).
constexpr double kStorageGovernmentLowPrice = 0.95;
constexpr double kStorageGovernmentHighPrice = 1.05;
// Dias antes da próxima colheita em que o privado começa a vender (conservador
// × especulador). O calendário da região é o dado; isto é o comportamento.
constexpr double kStorageConservativeSellDays = 60.0;
constexpr double kStorageSpeculatorSellDays = 20.0;
// Fome: satisfação abaixo deste valor faz o governo liberar reserva mesmo sem
// pagamento (socorro; bens não são dinheiro, nenhuma moeda é criada).
constexpr double kStorageReliefSatisfaction = 0.80;

} // namespace ogs
