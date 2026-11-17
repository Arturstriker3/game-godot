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
// v12: construção como projeto físico (§15) - `construction_projects` (obras em
// andamento). Save v11 e anteriores não têm a chave: não há projeto em voo, o
// mundo carrega com a lista vazia (migração trivial, sem estado a reconstruir).
// v13: solo urbano, moradia e prefeitura (§19, W0) - bloco `urban` por cidade
// (área por zona, densidade permitida, reserva, unidades por densidade/padrão,
// posse/aluguel/preço e contas da prefeitura). Save v12 e anteriores não têm a
// chave: os vetores ficam como estão (vazios num load frio ou com os padrões
// carregados pelo demo_set_urban), sem lógica diária a reconstruir.
// v14: a obra de MORADIA sai da lista própria (`housing[].projects`) e entra na
// fila única `construction_projects` com `kind = CONSTRUCTION_HOUSING` (+
// housing_density/tier/owner/units e land_cost). Ao carregar v13, cada obra da
// lista antiga é migrada para a fila com `duration_days = days_left` e custo 0
// (o capital já foi pago/pausado na versão antiga).
constexpr int kDemoSaveVersion = 14;

// §15 — construção como projeto físico. Obras são cronogramas de materiais +
// mão de obra do setor de construção; o pagamento sai do dono conforme avança.
// ---------------------------------------------------------------------------
// Fração do custo da obra que é MATERIAL (o resto é mão de obra/serviço). Os
// materiais são comprados do mercado local pelo preço corrente; a quantidade
// contratada é dimensionada pelo PREÇO-BASE (contrato de obra declarado).
constexpr double kDemoConstructionMaterialShare = 0.15;
// §15 unificado: a terra é uma FATIA do orçamento do dono (não um custo
// extra). Em prédio econômico ela é limitada a esta fração, para o
// material+mão de obra ficarem com o resto do orçamento.
constexpr double kDemoConstructionLandShareMax = 0.10;
// Prazo de obra por tipo (dias), estimativa de ordem de grandeza ("meses pelo
// tamanho", §15) — não é dado observado de cronograma de obra. A comida
// (fazenda da cesta) é RÁPIDA para não travar a oferta alimentar de nações que
// dependem de importação (a IA de comida planeja com antecedência); mina é
// média (obra de terraplenagem/galeria); fábrica (receita) é pesada.
constexpr int kDemoConstructionFoodDays = 15;
constexpr int kDemoConstructionMineDays = 45;
constexpr int kDemoConstructionFactoryDays = 60;
constexpr int kDemoConstructionPrimaryDays = 30;
// Escala do prazo pelo TAMANHO da obra (capacidade em t/dia): obras maiores
// levam mais tempo. fator = clamp(1 + log_scale × log2(cap/ref), min, max).
// Estimativa declarada — não é dado observado de cronograma.
constexpr double kDemoConstructionSizeReference = 10.0;
constexpr double kDemoConstructionSizeLogScale = 0.5;
constexpr double kDemoConstructionSizeScaleMin = 0.5;
constexpr double kDemoConstructionSizeScaleMax = 2.5;
// Rampa de operação de planta NOVA (construída via projeto físico): começa em
// kDemoRampStart da capacidade e cresce LINEARMENTE até 100% ao longo de
// kDemoRampDays dias de CARÊNCIA, durante os quais a falência por prejuízo NÃO
// conta (empresa nova real: o capital da obra é investimento, não prejuízo
// operacional; a planta precisa de tempo para formar carteira antes de ser
// avaliada). O opex da planta em rampa acompanha o output_scale (custo fixo
// reduzido), não o custo de 100%.
constexpr double kDemoRampStart = 0.25;
constexpr int kDemoRampDays = 90;
// Valor de obra (XGS) que cada trabalhador do setor de construção avança por
// dia. Ordem de grandeza ancorada na produtividade de serviços (capital/jobs
// da F8); limita o surto de obras mas não bloqueia a segurança alimentar.
constexpr double kDemoConstructionValuePerWorkerDay = 20.0;
// Piso de trabalhadores de construção por cidade (setor indústria/construção)
// para a obra nunca travar em cidade sem indústria declarada (sector_workers
// vira 0 no primeiro dia, em cidades só-agrícolas e logo após um load). 100
// trabalhadores × 20 XGS/dia = 2.000 XGS/dia de obra: uma obra típica avança;
// obras grandes (fábrica pesada) ou muitas obras simultâneas dividem o teto.
constexpr double kDemoConstructionWorkerFloor = 100.0;
// Estoque inicial de materiais de construção por cidade (derivado da população
// de simulação). É a "obra em estoque" pré-existente da economia; declara-se
// como `derived:initial_stock` — não é dado observado de estoque de materiais.
// Generoso o bastante para a construção do cenário (365 d) não travar: no mundo
// real ainda não há produtor de materiais de obra, então o estoque é o único
// suprimento até a indústria pesada entrar (direção futura do §15).
constexpr double kDemoConstructionMaterialStockPerCapita = 0.002;
constexpr double kDemoSteelStructureStockPerCapita = 0.001;
constexpr double kDemoElectricalMaterialStockPerCapita = 0.0002;
// Maquinário industrial (fábricas): caro (base_price alto), então a quantidade
// por habitante é pequena — cobre a obra inicial de fábrica sem sobra.
constexpr double kDemoIndustrialMachineryStockPerCapita = 0.0001;

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

// Extração mineral. 1 kt = 1000 t.
constexpr double kDemoTonsPerKiloton = 1000.0;
// Qualidade -> eficiência da extração: a produção efetiva é multiplicada por
// piso + (1 - piso) × qualidade (minério pobre rende menos por dia de operação).
constexpr double kDemoQualityEfficiencyFloor = 0.5;
// Profundidade -> custo: opex × (1 + profundidade / escala), com teto.
constexpr double kDemoDepthOpexScaleM = 2000.0;
constexpr double kDemoDepthOpexMaxFactor = 3.0;
// Primeiro produto "bruto" (sem consumo local) na ordem de goods.json: 1=ferro.
// A fase de matérias-primas pula os alimentos da cesta (B3) via demo_food_calories,
// então este valor só delimita onde começa a venda externa (minerais, algodão,
// madeira).
constexpr int kDemoFirstRawProduct = 1;

constexpr double kDemoNeedPerCapita = 0.0003;
// F3: reserva de alimento (dias) que a POP mantém em poupança antes de gastar
// em life/luxury. Sem ela, a compra de conforto drenava a poupança usada para
// importar comida em nações sem terra (EGY/SAH em 365 d). É uma regra
// prudencial declarada, não um número observado: o colchão cobre uma década de
// cesta, então o conforto é pago essencialmente pela renda corrente, não pela
// poupança que financia a importação de alimento (EGY tinha poupança acima de
// um ano e ainda perdia comida em 365 d).
constexpr double kDemoFoodReserveDays = 3650.0;
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
constexpr double kDemoGovernmentGoodsFoodShare = 0.05;
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
// (índice por cidade, ver demo_set_external_income_index), normalizado para que
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
constexpr double kDemoExternalIncomePerCapita = 0.012;
constexpr double kDemoExternalIncomeIndexCap = 1.0;
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
constexpr double kDemoLaborShare = 0.55;
constexpr double kDemoLaborShareLow = 0.50;
constexpr double kDemoLaborShareHigh = 0.65;
constexpr double kDemoServiceShare = 0.30;
constexpr double kDemoServiceWage = 0.15;
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
constexpr double kDemoServiceAnchorScaleMin = 0.10;
constexpr double kDemoServiceAnchorScaleMax = 4.0;
// F8 v2 — MARGEM-ALVO: reduz a receita por posto de ~1,47× para ~1,25× o
// salário local (margem bruta ~20% antes de insumos). Combinado com a banda
// apertada [0,70; 1,40], a margem líquida mediana cai para ~5–20% conforme o
// setor (varejo baixo, finanças/TI mais alto), sem o prejuízo estrutural do
// preço sub-calibrado que o teto 0,68 mascarava (Q-004).
constexpr double kDemoServiceMarginCalibration = 0.85;
constexpr double kDemoServiceSpendShare = 0.45;
constexpr double kDemoTaxRate = 0.15;
constexpr double kDemoTransferShare = 1.0;
constexpr double kDemoCapitalDistribution = 1.0;
// F9 — investimento e propriedade.
// Imposto corporativo sobre o lucro do ativo do grupo, pago ao orçamento do
// anfitrião. Retenção: fração do lucro líquido que fica no caixa do grupo
// (investimento seguinte); o resto é dividendo ao estrato dono no fim do dia.
constexpr double kDemoCorporateTaxRate = 0.10;
constexpr double kDemoGroupDividendShare = 0.80;
// Lote de ativos por grupo na semeadura (nação × setor). Mantém "algumas
// centenas" de grupos no mundo real em vez de um grupo por cidade.
constexpr int kDemoGroupChunkAssets = 12;
// Nacionalização: confiança cai a 35% e só se recupera depois de 180 dias.
constexpr int kDemoNationalizationDays = 180;
constexpr double kDemoNationalizationConfidence = 0.35;
constexpr double kDemoNationalizationCompensation = 0.5;
constexpr double kDemoFreightPerTonKm = 0.10;
// Frete marítimo (entre massas de terra), espelhando a taxa de navio do
// mercado legado: 5x mais barato que o caminhão por tonelada-km.
constexpr double kDemoFreightShipPerTonKm = 0.02;

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
constexpr double kDemoFleetTrucksPerCapita = 1.0 / 12500.0;
constexpr double kDemoFleetShipsPerCapita = 1.0 / 250000.0;
constexpr double kDemoFleetPlanesPerCapita = 1.0 / 2500000.0;
constexpr double kDemoFleetMinTrucks = 16.0;
constexpr double kDemoFleetMinShips = 4.0;
constexpr double kDemoFleetFuelDays = 400.0;
// Reserva estratégica mínima por habitante de simulação (t/capita). Ordem de
// grandeza de um estoque nacional de combustível (não é medição): cobre a
// expansão industrial que a IA cria em 60–365 d, quando o volume de carga
// cresce ~7× sobre a estimativa da frota. O piso de 400 dias da frota continua
// valendo para nações pequenas.
constexpr double kDemoFleetFuelPerCapita = 0.01;
// Consumo diário estimado por veículo (t/dia), usado para dimensionar o
// estoque-alvo de combustível. Derivado do uso medido (~0,09 t/caminhão/dia;
// navio e avião têm tanques/passagens maiores).
constexpr double kDemoFleetFuelPerTruckDay = 0.12;
constexpr double kDemoFleetFuelPerShipDay = 0.80;
constexpr double kDemoFleetFuelPerPlaneDay = 1.50;
// Manutenção mensal: peças por veículo/mês (t). Um caminhão ~20 kg de
// componentes por mês; navio/avião uma ordem de grandeza acima.
constexpr double kDemoFleetPartsPerTruckMonth = 0.02;
constexpr double kDemoFleetPartsPerShipMonth = 0.40;
constexpr double kDemoFleetPartsPerPlaneMonth = 0.15;
// Reserva inicial de peças, em meses de manutenção. Transição da F4: o mundo
// começa sem fábricas de `spare_parts`; 18 meses cobrem o horizonte de 365 d
// com folga (como os 400 dias de fuel) e evitam que a saúde da frota caia ao
// piso de 0,5 no meio do ano — o que recriava o gargalo de comida da F3.
// A reposição mensal continua mirando 3 meses, não a reserva cheia.
constexpr double kDemoFleetPartsReserveMonths = 18.0;
constexpr double kDemoFleetPartsTargetMonths = 3.0;
// Atrito mensal (acidentes/obsolescência) e saúde da frota.
constexpr double kDemoFleetAttritionMonth = 0.002;
constexpr double kDemoFleetHealthFloor = 0.50;
constexpr double kDemoFleetHealthLossMonth = 0.10;
constexpr double kDemoFleetHealthRecoveryMonth = 0.05;
// Fração do orçamento da cidade-sede que o governo pode gastar por compra.
constexpr double kDemoFleetBudgetShare = 0.10;
// Cobertura de comida (dias de necessidade) exigida para o governo investir
// em frota. A comida tem prioridade no orçamento público: sem esta folga, o
// procurement mensal competia com a importação de alimento (AFG/LBY caíam em
// 365 d de IA). Abaixo do limiar, só há compra de emergência de fuel/peças.
constexpr double kDemoFleetFoodSecureDays = 3.0;

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
constexpr double kDemoFxDailyStep = 0.0005;
constexpr double kDemoFxBandLow = 0.90;
constexpr double kDemoFxBandHigh = 1.10;
constexpr double kDemoFxBalanceEmaDays = 30.0;
constexpr double kDemoFxBalanceScale = 0.10;

// ---------------------------------------------------------------------------
// Portos (ver docs/economy/port_design.md).
//
// Capacidade de nível 1 pela população de simulação da cidade-sede: piso de
// 1.000 t/dia (~0,4 Mt/ano, porto pequeno) e teto de 50.000 t/dia (~18 Mt/ano,
// calado de metrópole). O nível multiplica a base (1..N).
// ---------------------------------------------------------------------------
constexpr double kDemoPortBaseTons = 1000.0;
constexpr double kDemoPortTonsPerCapita = 0.002;  // 2 kg/habitante/dia
constexpr double kDemoPortMaxTons = 50000.0;
// Capital público por t/dia de capacidade nova e prazo da obra.
constexpr double kDemoPortCapitalPerTonCapacity = 500.0;
constexpr int kDemoPortBuildDays = 30;
// Tempo de transbordo (carga/descarga) por operação no cais.
constexpr double kDemoPortHandlingHours = 8.0;
// Fila: custo de demurrage por tonelada e por dia de atraso fracionário.
constexpr double kDemoPortDemurragePerTonDay = 0.5;
constexpr double kDemoPriceSmoothing = 0.20;

// Produção segue as vendas do dia anterior, com pequena folga de estoque.
constexpr double kDemoProductionBuffer = 1.10;
// Estoque-alvo em dias de venda. A produção repõe metade da diferença por dia:
// sem estoque-alvo, produzir sempre 10% acima das vendas faria o estoque
// crescer para sempre (minas e fazendas com demanda externa fixa).
// Prejuízo persistente reduz a produção; lucro com demanda alta pode expandir.
constexpr int kDemoLossDaysBeforeCut = 10;
constexpr double kDemoLossCut = 0.95;
constexpr double kDemoMinOutputScale = 0.20;
// Dias seguidos de prejuízo até a firma fechar a construção (falência). Depois
// de kDemoLossDaysBeforeCut a produção já foi cortada ao piso; se o prejuízo
// persistir até aqui, a firma encerra a atividade em vez de ficar no vermelho
// para sempre.
constexpr int kDemoLossDaysBeforeClose = 60;
// Capital de giro: as empresas retêm caixa suficiente para importar e pagar
// salários antes de distribuir lucros.
constexpr double kDemoFirmCashPerCapita = 0.10;
constexpr double kDemoServiceCashPerCapita = 0.05;
// F8: caixa do setor de serviços = capital de giro em dias de folha + insumos.
// O excedente do lucro é renda da POP dona (ver demo_tick / distribuição).
constexpr double kDemoServiceCashBufferDays = 8.0;
// F8: a fração de serviços respeita a cesta do dia — em cidade que importa
// comida o serviço não pode consumir o que compra o alimento. 0 desliga o
// guarda (comportamento F7: 45% da renda doméstica, cesta depois).
constexpr double kDemoServiceFoodGuardDays = 1.0;
// Serviços ajustam o emprego para cobrir a folha com a própria receita.
constexpr double kDemoServiceAdjust = 0.10;
// Limites usados pelo benchmark sintético (o cenário demo usa o equilíbrio
// do mercado de trabalho residual).
constexpr double kDemoServiceMinShare = 0.05;
constexpr double kDemoServiceMaxShare = 0.30;
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
// (ver sim/sim_constants.h — kDemoFirstRawProduct e as constantes de extração
// mineral mudaram para lá na S5b).


// Aptidão de referência do território de AGRIA (trigo, dado real de
// agriculture.json). Mantém a produção validada de 20 t/dia e serve de valor
// inicial até o GeographyService carregar o dado em tempo de execução.
constexpr double kDemoReferenceWheatSuitability = 0.702;
constexpr double kDemoFarmCapacityTons = 20.0;
// Capital necessário para adicionar 1 t/dia de capacidade produtiva.
constexpr double kDemoCapitalPerCapacity = 2000.0;

// Inicialização do mundo real (initialize_demo_world). Valores derivados do
// cenário de validação AGRIA/PORTUS (fazenda de 20 t/dia, 150 empregos,
// salário 2.0 e opex 50), escalados para a população/construções reais.
constexpr double kDemoSavingsPerCapita = 0.50;
constexpr double kDemoGovernmentPerCapita = 1.0;
constexpr double kDemoJobsPerTon = 7.5;   // 150 empregos / 20 t
constexpr double kDemoFarmWage = 2.0;     // igual à fazenda demo
constexpr double kDemoOpexPerTon = 2.5;   // 50 opex / 20 t
// Fração da necessidade diária mantida como estoque antes de exportar. O
// vendedor já consumiu a própria necessidade na fase 5; a produção da fase 4
// repõe no dia seguinte. Buffer alto aqui espreme as cidades pequenas (a oferta
// vendável fica menor que a demanda e a última compradora — ex.: Cayenne — não
// recebe nada).
constexpr double kDemoTradeBufferDays = 0.0;
// Teto de cargas de comércio por dia (proteção contra loop). Com 40 cidades,
// 64 bastava; com 252 e 66 nações, 64 embarques/dia deixavam as cidades de
// índice alto (SAU, CHN...) permanentemente fora da fila — todas as
// compradoras com estoque zero empatam em cobertura relativa 3,0 e as 64
// primeiras do índice consumiam o teto todos os dias (§19). 512 cobre o pico
// de reabastecimento mundial e cai sozinho quando os estoques enchem.
constexpr std::size_t kDemoMaxShipmentsPerDay = 512;
// Capacidade de transporte diária de uma cidade, por habitante (t/dia). 1 kg
// por habitante/dia (~3x a necessidade alimentar de 0,3 kg) — um teto físico
// para o comércio não mover carga "infinita" num único dia, sem apertar o fluxo
// normal de comida.
constexpr double kDemoTradeCapacityPerCapita = 0.001;

constexpr std::size_t kDemoTradeExhaustiveLimit = 24;
// Célula do índice espacial do comércio, em unidades do mapa (~8 km cada):
// 32 unidades ≈ 250 km. O comprador varre a grade em anéis até juntar K
// vendedores, então a célula só afeta a constante do custo de busca.
constexpr double kDemoTradeCellMapUnits = 32.0;

// ---------------------------------------------------------------------------
// §19 — solo urbano, zoneamento e imóveis (W0).
//
// Estrutura do estoque: m² de piso por unidade para cada densidade × padrão
// (casas são maiores que apartamentos; o padrão alto ocupa mais área). São
// parâmetros de modelo — a fonte publica aluguel/preço por m², não por unidade.
constexpr double kDemoUrbanUnitM2[3][3] = {
	{60.0, 100.0, 180.0},  // DENSITY_LOW: casa
	{55.0, 85.0, 140.0},   // DENSITY_MID: prédio
	{50.0, 75.0, 110.0},   // DENSITY_HIGH: torre
};
// FAR (área construída ÷ área do lote) por densidade. Capacidade da zona =
// área × FAR ÷ m² por unidade (W1).
constexpr double kDemoUrbanFar[3] = {1.0, 2.5, 6.0};
// Taxa de capitalização fixa (aluguel anual ÷ valor do imóvel) — §19.6.
constexpr double kDemoUrbanCapRate = 0.05;
// Depreciação anual do estoque (fração do valor/condição) — §19.3.
constexpr double kDemoUrbanDepreciationPerYear = 0.01;
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
constexpr double kDemoUrbanInfraCostMonths = 2.0;
constexpr double kDemoUrbanInfraFloorPerKm2 = 1000000.0;
// Custo crescente do SOLO (spec §19, "Custo crescente"): o custo por km² sobe
// com o raio da cidade (redes mais longas até o centro) e com a fração da
// reserva já consumida (o que sobra é o terreno pior). Fatores lineares
// declarados: +50% quando o raio dobra; +80% quando a reserva toda foi usada.
constexpr double kDemoUrbanExpandDistanceK = 0.5;
constexpr double kDemoUrbanExpandReliefK = 0.8;
// Taxa local sobre o faturamento de serviços da cidade (fração) usada enquanto
// o W2 não calibra a receita municipal contra o dado real.
constexpr double kDemoUrbanMunicipalFeeRate = 0.02;

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
constexpr double kDemoUrbanServiceM2PerWorker = 25.0;
constexpr double kDemoUrbanFactoryM2PerWorker = 150.0;
// Preço da terra = esta fração do valor do imóvel médio (XGS/m²). A fonte não
// publica preço de terra por cidade; a parcela do terreno no imóvel é o proxy
// declarado (W0 deixou o campo em 0 de propósito).
constexpr double kDemoUrbanLandShareOfHousePrice = 0.30;
// Curva de ocupação -> pressão de preço: convexa perto de 1 e com teto
// declarado (evita preço explosivo quando a ocupação passa de 1).
constexpr double kDemoUrbanLandMaxPressure = 8.0;
// Atualização mensal do preço da terra: fração do caminho até o alvo por mês
// (suave, não um salto).
constexpr double kDemoUrbanLandPriceSmooth = 0.25;
// Renda média local no alvo do preço: ganho linear em (índice de renda − 1),
// limitado a ±0,5/1,0 do índice.
constexpr double kDemoUrbanLandIncomeGain = 0.5;
// Demanda de zona sinalizada decai à metade a cada mês.
constexpr double kDemoUrbanZoneDemandDecay = 0.5;
// População que a infraestrutura passa a atender por km² expandido.
constexpr double kDemoUrbanInfraPopPerKm2 = 5000.0;
// Adensar fica mais caro a cada nível (spec §19, "Custo crescente"):
//  - LOW→MID exige reforço de água/esgoto/energia: 0,35 do km² de expansão;
//  - MID→HIGH exige estrutura de aço, elevadores e redes pesadas: 0,85.
// Os custos são uma fração do custo de infraestrutura por km² (mesma obra,
// sem terra nova) e o nível alto ainda troca a receita de material para
// steel_structure/electrical_material.
constexpr double kDemoUrbanDensifyCostFactorLowMid = 0.35;
constexpr double kDemoUrbanDensifyCostFactorMidHigh = 0.85;
// Mistura de material do adensamento MID→HIGH: pesos relativos aplicados sobre
// a receita de material do §15, que continua somando a mesma fração do custo.
constexpr double kDemoUrbanDensifyHighSteelWeight = 1.8;
constexpr double kDemoUrbanDensifyHighElectricalWeight = 1.6;
// Régua da obra de infraestrutura: km² -> unidades de capacidade do §15 (só
// escala o prazo; a obra urbana não cria prédio).
constexpr double kDemoUrbanKm2ToConstructionUnits = 20.0;
// Fechamento do "mês" do calendário do jogo para preço da terra e decadência
// da demanda de zona.
constexpr int kDemoUrbanMonthDays = 30;

// §19 W3 — MERCADO DE MORADIA (sim/housing.cpp). Parâmetros de MODELO, não
// dados observados; o ônus de aluguel e a cap rate vêm da fonte (OECD AHD
// HC1.2 e §19.6).
// ---------------------------------------------------------------------------
// Ônus de aluguel: a fonte publica a mediana dos inquilinos (0,28). O nível
// inicial do aluguel é REANCORADO na renda medida do núcleo nessa fração
// (housing_city.json publica USD/m² — escala ~2,7 ordens de grandeza acima da
// renda simulada em XGS; o valor absoluto da fonte não é transportável).
constexpr double kDemoHousingInitialBurden = 0.28;
// Teto de ônus de aluguel do inquilino: o aluguel de um domicílio não passa
// desta fração da renda mensal do domicílio (trava de longo prazo).
constexpr double kDemoHousingRentBurdenCap = 0.40;
// Reserva de comida (dias de cesta) que o aluguel NUNCA toca: comida tem
// prioridade sobre moradia (spec §19/W3).
constexpr double kDemoHousingFoodReserveDays = 30.0;
// Ajuste mensal do aluguel: passo máximo (±2%/mês) e elasticidade ao
// desequilíbrio demanda × oferta.
constexpr double kDemoHousingRentStepMax = 0.02;
constexpr double kDemoHousingRentElasticity = 0.50;
// Custo de construção de uma unidade em ANOS DE ALUGUEL do padrão (parâmetro
// adimensional, independente da escala monetária do mundo) — a terra entra
// separada pelo land_price da zona residencial (W1). Uma unidade vale 20 anos
// de aluguel (§19.6); construir custa ~14.
constexpr double kDemoHousingBuildCostYears = 14.0;
constexpr double kDemoHousingDeveloperMargin = 0.20;
// Parcela MATERIAL da obra de moradia (o resto é mão de obra/serviço).
constexpr double kDemoHousingMaterialShare = 0.40;
// Prazo de obra de moradia (dias) e tamanho do lote de unidades por mês.
constexpr int kDemoHousingBuildDays = 90;
constexpr double kDemoHousingBatchShare = 0.005;     // 0,5% da demanda do padrão
constexpr double kDemoHousingSelfBuildShare = 0.002; // autoconsntrução popular
// Manutenção paga pelos donos (fração do aluguel) ao setor de serviços.
constexpr double kDemoHousingMaintenanceShare = 0.10;
// Favela (ÚLTIMO caso): falta popular mínima, vacância total máxima, dias
// consecutivos de aperto, fatia de domicílios por mês e penalidade de
// satisfação por fatia informal (limitada a 5 p.p. pela própria fração).
constexpr double kDemoHousingSlumShortage = 0.05;
constexpr double kDemoHousingSlumVacancyMax = 0.01;
constexpr int kDemoHousingSlumDays = 30;
constexpr double kDemoHousingSlumShare = 0.01;
constexpr double kDemoHousingSlumSatisfaction = 0.05;


} // namespace ogs
