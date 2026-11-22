#pragma once
// Tipos POD + enums + constantes do núcleo econômico (S1 do
// docs/economy/core_refactor_plan.md). Sem dependência do engine Godot: só STL.
// S5a: os tipos deixam de ser aninhados em EconomySim e passam a viver no
// namespace `ogs` (STL puro), para o WorldState/SimCore os usarem sem godot_cpp.
// economy_sim.* passa a ser fachada (conversão Godot <-> sim). Nada de godot_cpp.

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "urban_types.h"

namespace ogs {

// ==== BLOCO DE TIPOS (extraído de economy_sim.h) ====
	enum class TransportMode { Truck, Ship, Plane };

	// Camadas de mercado, resolvidas nesta ordem: a cidade compra primeiro de
	// vizinhas do próprio país, depois do resto do país e por último no exterior.
	enum class MarketScope { Regional, National, International };

	struct ModeSpec {
		TransportMode mode;
		const char* name;
		double speed_kmh;
		double batch_capacity_tons;
		double cost_per_ton_km;
		double turnaround_hours;
		// F4: o frete deixa de ser só distância × tarifa. `fuel_per_ton_km` é o
		// combustível físico consumido (t de fuel por t·km) e
		// `fleet_cost_per_ton_km` é o uso da frota (depreciação/crew/seguro)
		// somado ao preço do frete. Coeficientes de combustível na ordem física
		// (caminhão com retorno vazio, navio a 20 kn, avião cargueiro).
		double fuel_per_ton_km;
		double fleet_cost_per_ton_km;
		// Jornada real: o caminhão roda só ~10 h/dia (motorista/descanso), navio
		// e avião operam 24 h. A velocidade do trecho é `speed_kmh` durante a
		// jornada; o tempo de calendário usa a média `speed * duty / 24`.
		double duty_hours_per_day;
		// Desvio entre a rota real e a distância em linha reta (grande círculo):
		// estrada contorna relevo/fronteira, navio contorna continente/canal.
		// O avião voa praticamente em linha reta.
		double route_factor;
	};

	// Coordenadas no grid do mapa (5000x2500), na projeção equiretangular.
	struct MapPoint {
		double x;
		double y;
	};

	struct Leg {
		TransportMode mode;
		std::string from;
		std::string to;
		MapPoint from_point;
		MapPoint to_point;
		double distance_km;
		double load_tons;
		int trips;
		double transit_hours;
		double cost;
	};

	struct Port {
		std::string id;
		std::string name;
		std::string nation;  // dono (estratégico: sem porto, sem mar)
		MapPoint center;
		std::string landmass;
		// Cidade-sede (`port_of`). NÃO é dono exclusivo: qualquer cidade da
		// nação na mesma massa de terra usa o porto (ver port_design.md).
		std::string host_city;
		// Capacidade e fila (t/dia). `base_capacity_t_per_day` é o nível 1
		// (população da cidade-sede); `capacity_t_per_day` já inclui a expansão.
		double base_capacity_t_per_day = 0.0;
		double capacity_t_per_day = 0.0;
		double used_today_t = 0.0;  // carga comprometida no dia corrente
		double queue_t = 0.0;       // pico de carga acima da capacidade no dia
		double delay_hours = 0.0;   // atraso acumulado do dia (fila)
		double demurrage = 0.0;     // custo de fila do dia (pago ao frete)
		// Obra de expansão em andamento (API expand_port).
		double pending_capacity_t_per_day = 0.0;
		int pending_days = 0;
	};

	struct TradeOrder {
		std::string from_city;
		std::string to_city;
		std::string from_nation;
		std::string to_nation;
		MarketScope scope = MarketScope::Regional;
		int product = -1;
		double amount = 0.0;
		double transit_hours = 0.0;
		double freight_cost = 0.0;
		// F5: decomposição do preço entregue, por tonelada. `origin_price` está
		// na moeda do vendedor; os demais valores em âncora. `delivered_price`
		// está na moeda do comprador.
		double origin_price = 0.0;
		double origin_price_anchor = 0.0;
		double tariff = 0.0;
		double delivered_price = 0.0;
		double fx_rate = 1.0;          // âncora por unidade da moeda compradora
		double seller_fx_rate = 1.0;   // âncora por unidade da moeda vendedora
		std::string buyer_currency;
		std::string seller_currency;
		std::vector<Leg> legs;
	};

	// ------------------------------------------------------------------
	// Cenário de validação: duas cidades, população agregada (POPs) e trigo
	// consumido diretamente. Vive no mesmo núcleo para validar as regras antes
	// de generalizar o modo europeu; a ordem dos produtos é a de
	// data/definitions/economy/goods.json (não há enum de produtos).
	// ------------------------------------------------------------------
	// Mesma ordem de data/definitions/economy/goods.json. A contagem é variável
	// (set_goods): os 21 primários continuam nas posições 0..20 e os 92
	// bens industriais entram em 21..112. kMaxProducts é só o teto de
	// segurança do vetor.
	static constexpr int kMaxProducts = 160;
	static constexpr int kMaxRecipeInputs = 6;
	// §15: materiais de uma obra (construction_material, steel_structure,
	// electrical_material; fábricas acrescentam industrial_machinery).
	static constexpr int kMaxConstructionMaterials = 4;
	// F3: níveis de necessidade (basic/life/luxury, needs.json).
	static constexpr int kMaxNeedsLevels = 3;
	// F7 calibração: níveis de consumo alcançáveis pela renda de um estrato
	// (0 = nenhum, 1 = basic, 2 = life, 3 = luxury). É a fatia da POP por nível
	// publicada no status — por isso tem uma posição a mais que a cesta.
	static constexpr int kIncomeLevels = kMaxNeedsLevels + 1;
	// F7: níveis de educação (básico, intermediário, superior). Não é dado de
	// catálogo: são três faixas de escolaridade da população em idade ativa.
	static constexpr int kEducationLevels = 3;
	// B2: cinco estratos de renda (quintis) por cidade. SEPARADOS da educação:
	// a escolaridade define o acesso a vaga; o quintil define a renda de
	// capital/dispersão salarial, o consumo, a poupança e o patrimônio. Os
	// nomes antigos `stratum_*[kEducationLevels]` continuam existindo como
	// espelho educacional (contrato de status/UI); os campos novos abaixo são
	// indexados por quintil.
	static constexpr int kIncomeStrata = 5;
	// B2: posse de moradia por estrato (própria, alugada, informal). Mesma
	// ordem de housing.h (TENURE_*), para a ligação com o §19.
	static constexpr int kStrataTenureCount = 3;
	// F8: setores de serviço (services.json). O teto é fixo para o estado por
	// cidade caber em arrays planos; os dados declaram 20 e o loader recusa
	// arquivo com contagem diferente (a publicação/UI indexa por posição).
	static constexpr int kMaxServiceSectors = 20;
	// Campos publicados por setor na cidade (ver service_publish_fields em
	// economy_sim.cpp): demanda, oferta, preço, receita, emprego, negócios,
	// demanda não atendida, exportação (unidades), exportação (receita),
	// cobertura privada, custo de insumos e custo de serviços.
	static constexpr int kServiceFields = 12;
	// G — safras por (cidade, produto). O teto cobre as 2 safras declaradas nos
	// dados (1ª e 2ª soja da ARG, kharif/rabi da IND); 4 é folga de segurança.
	static constexpr int kMaxCropSeasons = 4;
	// G — bens da CADEIA alimentar com prazo de prateleira publicado
	// (product_shelf_life.json: 12 produtos). O índice é a POSIÇÃO na lista
	// `ws_.agri_chain` (resolvida no catálogo), não o índice do produto.
	static constexpr int kMaxAgriChain = 12;
	// Limite de negócios por cidade/setor e de aberturas por cidade/dia.
	static constexpr int kServiceMaxPerSector = 4;
	static constexpr int kServiceOpensPerDay = 5;
	// Cesta alimentar (B3/B4) agora data-driven (set_needs), derivada de
	// needs.json (basic/food/substitutes + equivalence.calories). O padrão
	// preserva a cesta validada de 5 culturas até o autoload carregar o dado.
	// A necessidade é medida em toneladas-equivalentes de grão: o fator calórico
	// converte tonelada real em tonelada-equivalente (batata ~1/4 das calorias).
	static constexpr int kMaxFoodProducts = 16;
	static constexpr int kFoodProductIds[5] = {0, 2, 3, 16, 17};
	static constexpr double kCaloricFactor[5] = {1.0, 1.0, 1.0, 0.25, 1.3};

	// Os 8 campos PUBLICADOS no status ficam primeiro e em ordem fixa: o cache
	// de publicação compara o bloco inteiro com um memcmp de 64 bytes (um
	// único teste em vez de 8 comparações de double por produto/cidade).
	// `sold_prev` é interno (muda todo dia) e fica no fim, fora da comparação.
	struct Market {
		double stock = 0.0;
		double price = 1.0;
		double demand = 0.0;   // compras realizadas (consumo, insumos e exportação)
		double supply = 0.0;   // produção ofertada no dia
		double desired = 0.0;  // demanda desejada antes de estoque/orçamento
		double sold = 0.0;     // vendas realizadas no dia
		double shortage = 0.0; // demanda desejada não atendida
		double surplus = 0.0;  // estoque acima do necessário
		double sold_prev = 0.0; // vendas do dia anterior (base da produção) - interno
		// F7 calibração: parte da demanda desejada que NÃO tem renda/renda
		// disponível por trás. É sinal de escassez para a IA (entra em
		// `desired`/`shortage`), mas não pressiona o preço: preço é formado por
		// demanda financiada, não por desejo sem poder de compra.
		double latent = 0.0;
		// D-091 — demanda EFETIVA (fluxo diário, interno, fora do bloco de 8
		// campos publicados): o que os compradores REALMENTE tentam comprar dado
		// caixa/insumos/uso, acumulado na fonte. `desired`/`latent` continuam
		// sendo o sinal da IA (incluindo o bootstrap de plena capacidade da usina
		// ociosa); `effective` é o que pressiona o preço. Documentado em
		// docs/economy/price_formation.md.
		double effective = 0.0;
	};
	static_assert(offsetof(Market, stock) == 0, "Market: stock primeiro");
	static_assert(offsetof(Market, surplus) == 7 * sizeof(double),
		"Market: os 8 campos publicados precisam ser contíguos");

	// ------------------------------------------------------------------
	// G — PLANO DE SAFRA por (cidade, produto). É DADO (calendário FAO/USDA,
	// rendimento FAOSTAT, área colhida) que chega pelo AgricultureService e
	// viaja no bloco `definitions` do snapshot — não é estado do jogo. O
	// ESTADO da safra (o que já foi entregue/plantado no ano) mora no mesmo
	// struct e é serializado no save v24.
	// ------------------------------------------------------------------
	// Janela de uma safra em DIA DO ANO (1..365, calendário civil do jogo, que
	// começa em 2027-01-01). `start > end` = janela que atravessa o ano.
	struct CropSeasonPlan {
		int plant_start_doy = 0;
		int plant_end_doy = 0;
		int harvest_start_doy = 0;
		int harvest_end_doy = 0;
		// Fração do orçamento anual desta safra. Sem dado de participação por
		// safra nos arquivos, o AgricultureService declara 1/n (regra
		// declarada) — nunca um número inventado por país.
		double share = 1.0;
	};
	// Estado persistente de uma safra (save v24).
	struct CropSeasonState {
		// Ano civil (chave da instância) do último PLANTIO cobrado. 0 = nunca.
		int planted_year = 0;
		// Fator de rendimento da safra (1 = fertilizante comprado integral;
		// < 1 = faltou insumo no mercado). Aplicado à entrega da colheita.
		double yield_scale = 1.0;
		// Rendimento CONGELADO no início da instância de colheita: o plantio
		// seguinte (janelas agregadas se sobrepõem) não pode reescrever o
		// orçamento de uma safra que já está sendo entregue.
		double harvest_yield_scale = 1.0;
		// Fertilizante efetivamente comprado no plantio (t do bem
		// `fertilizer`), para auditoria.
		double fertilizer_t = 0.0;
		// Chave (ano civil de início da janela) da instância de colheita em
		// curso e o que já foi entregue nela (t BRUTAS, antes da perda).
		int harvest_year = 0;
		double delivered_t = 0.0;
		// Perda de entrada (pós-colheita) do mês, para o status por nação.
		double loss_month_t = 0.0;
		int loss_month_key = 0;
	};
	struct CropPlan {
		std::string city;
		int product = -1;
		int city_index = -1;  // derivado (não serializado)
		double area_ha = 0.0;
		double yield_t_ha = 0.0;
		// Fertilizante nitrogenado aplicado (dado IFA FUBC, kg N/ha).
		double n_kg_ha = 0.0;
		int season_count = 0;
		CropSeasonPlan seasons[kMaxCropSeasons] = {};
		CropSeasonState state[kMaxCropSeasons] = {};
		// Derivado (NÃO serializado): toneladas líquidas entregues hoje por
		// este plano. Serve ao status e ao teste de sazonalidade.
		double delivered_today_t = 0.0;
	};

	// F3: uma categoria de necessidade (needs.json → levels.<nível>.categories).
	// `substitutes` mantém a ordem declarada (do melhor ao último caso) e
	// `equivalence` converte tonelada real em tonelada-equivalente da categoria.
	struct NeedsCategory {
		std::string id;
		double per_capita_day = 0.0;  // per_capita_t_month / 30
		std::vector<int> substitutes;
		std::vector<double> equivalence;
	};

	// F3: um nível (basic/life/luxury) com peso na satisfação agregada.
	struct NeedsLevel {
		std::string id;
		int priority = 1;
		double weight = 0.0;
		std::vector<NeedsCategory> categories;
	};

	struct Recipe {
		std::string id;
		int output = -1;
		double yield = 1.0;
		int input_count = 0;
		int inputs[kMaxRecipeInputs] = {};
		double input_qty[kMaxRecipeInputs] = {};
		int byproduct_count = 0;
		int byproducts[kMaxRecipeInputs] = {};
		double byproduct_qty[kMaxRecipeInputs] = {};
		// Engenharia do processo (data/definitions/economy/recipes.json).
		double capital_per_capacity = 2000.0;
		double jobs_per_ton = 7.5;
		double wage = 2.0;
		double opex_per_ton = 2.5;
		// F7: mistura de escolaridade das vagas da receita (básico,
		// intermediário, superior), soma 1. Lida de `education_mix` no
		// recipes.json quando declarada; senão derivada do `sector` (tabela
		// documentada em set_recipes).
		double education_mix[kEducationLevels] = {0.60, 0.30, 0.10};
	};

	struct Building {
		std::string id;
		std::string city;
		// A2: índice de cidade derivado (não serializado). Evita o hash de
		// `city` a cada reconstrução do índice por cidade (todo tick).
		int city_index = -1;
		int kind = 0; // 0=fazenda (único tipo hoje; campo mantido por exibição)
		std::string resource;  // recurso produzido (id de goods.json)
		std::string owner = "private"; // private ou state
		int input_product = -1; // -1 = extração primária (sem insumo)
		int output_product = 0;
		// Receita industrial (-1 = produtor primário com input_product/ratio).
		// A receita completa (até 6 insumos + subprodutos) vive em recipes_.
		int recipe = -1;
		double capacity = 0.0;
		double input_per_output = 0.0;
		double capital_per_capacity = 2000.0;
		double jobs = 0.0;
		double wage = 0.0;
		double opex = 0.0;
		double workers = 0.0;
		double utilization = 0.0;
		double last_utilization = 1.0; // operação do dia anterior (custos)
		double output_scale = 1.0; // reduzido por prejuízos persistentes
		// Rampa de operação de planta nova (construída via projeto físico):
		// dias restantes de carência. Enquanto > 0, output_scale cresce
		// linearmente de kRampStart até 1,0 e a falência por prejuízo NÃO
		// conta (empresa nova real). Planta madura/semeada = 0.
		int ramp_days = 0;
		int loss_days = 0;
		bool bankrupt = false; // fechada por insolvência (prejuízo persistente)
		// D-086 (decisão do dono): SEMPRE false — o recurso nunca acaba, então
		// nenhuma mina encerra por depleção. Campo mantido por contrato de
		// save/status (v18 ignora o valor salvo e força false ao carregar).
		bool depleted = false;
		// Planta estratégica semeada no bootstrap industrial (decisão §12.3):
		// não fecha por insolvência enquanto não há demanda final (F3); o
		// governo cobre o prejuízo. Marcada explicitamente para o save/status.
		bool strategic = false;
		// F9: participação do parceiro local em joint venture. `owner` guarda
		// o grupo estrangeiro; `partner_group` o grupo nacional que divide o
		// lucro na fração `local_share`. Fora de JV, local_share = 0.
		double local_share = 0.0;
		std::string partner_group;
		// B5 (save v21): royalty/imposto de concessão - fração da receita da
		// mina/fazenda CONCEDIDA paga ao Estado (governo nacional). 0 fora de
		// concessão. `owner` continua o grupo concessionário (F9, sem duplicar
		// donos); o royalty é o efeito de renda da concessão. Modelo declarado.
		double royalty = 0.0;
		// Royalty efetivamente pago no dia (derivado, nao persistido).
		double royalty_paid = 0.0;
		double revenue = 0.0;
		double input_value = 0.0;
		double costs = 0.0;
		double profit = 0.0;
		// Valores efetivamente pagos no dia (podem ser menores que o contrato
		// quando o caixa da empresa não cobre a folha e o opex).
		double paid_wages = 0.0;
		double paid_opex = 0.0;
		// C1 (save v26) — PASSIVO da firma: salário atrasado do PERÍODO
		// corrente. É a parcela da folha contratada que o caixa da cidade não
		// cobriu (`pay_scale`); a folha reduzida pela D-099 por ILIQUIDEZ da
		// safra NÃO é dívida (não é devida — o contrato é a folha efetiva).
		// O par é o ativo das famílias em `Pop::arrears_receivable` (plantas +
		// serviços) e `Region::arrears_receivable` (rural); some dos dois lados
		// quando a planta fecha. A rolagem/liquidação com juros é a C3.
		double wage_arrears = 0.0;
		// F7: mistura de escolaridade das vagas da construção (soma 1). Fazenda
		// e extração quase só básico; indústria segue a receita/setor.
		double education_mix[kEducationLevels] = {0.90, 0.09, 0.01};
		// D-086 — ESTÁGIO DE MINA (extração primária com jazida rastreada).
		// `stage` é a profundidade de operação (0 = primeiro poço/galeria);
		// `stage_reserve_kt` é o que o estágio corrente ainda pode extrair
		// antes de cair ao piso e `stage_mined_kt` o que ele já extraiu. O
		// estágio NÃO é uma reserva paralela: cada tonelada também debita
		// `Deposit::remaining_kt` (contabilidade — a jazida é a referência de
		// profundidade, não um limite físico; o recurso nunca acaba).
		// `stage_reserve_kt <= 0` = estágio esgotado (produção no piso).
		// Fazenda/indústria mantêm 0/0/0 e produção relativa 1,0.
		int stage = 0;
		double stage_reserve_kt = 0.0;
		double stage_mined_kt = 0.0;
		// D-086b: a mina foi semeada OPERANDO pela produção real e ainda espera
		// a declaração da jazida para abrir a reserva do estágio
		// (`EconomySim::apply_set_deposit`). Não vai ao save: depois do dia 0 a
		// reserva está no próprio `stage_reserve_kt`.
		bool stage_pending = false;
	};

	// ------------------------------------------------------------------
	// G2 — ARMAZENAGEM (save v25). 6 tipos operacionais + cofre/paiol só
	// estrutura. O armazém é AGREGADO por (cidade, tipo, dono) — o núcleo não
	// modela prédio a prédio; a obra continua sendo um projeto físico §15.
	//   * silo / tanque / gás / pátio ficam na REGIÃO (pulmão da cidade-polo);
	//   * câmara fria / armazém geral ficam na CIDADE;
	//   * cofre e paiol são só estrutura nesta parte (sem fluxo de estoque).
	// O estoque de MERCADO continua em `City::markets`; `StorageSite::stock` é
	// o estoque GUARDADO do dono (reserva do governo ou estoque privado).
	// ------------------------------------------------------------------
	enum StorageType : int {
		STORAGE_SILO = 0,
		STORAGE_COLD = 1,
		STORAGE_FUEL = 2,
		STORAGE_GAS = 3,
		STORAGE_BULK = 4,
		STORAGE_GENERAL = 5,
		STORAGE_VAULT = 6,
		STORAGE_MAGAZINE = 7,
		kStorageTypeCount = 8,
	};
	enum StorageScope : int {
		STORAGE_GOVERNMENT = 0,
		STORAGE_PRIVATE = 1,
		kStorageScopeCount = 2,
	};

	struct StorageSite {
		std::string city;
		// Índice derivado (não serializado) da cidade em `WorldState::cities`.
		int city_index = -1;
		int type = STORAGE_SILO;
		int scope = STORAGE_GOVERNMENT;
		// Capacidade instalada (t) e estoque guardado por produto (t).
		double capacity_t = 0.0;
		std::vector<double> stock;
		// Parcela de PULMÃO do estoque: toneladas que entraram pela colheita e
		// ainda não escoaram para o mercado da cidade-polo. Subconjunto de
		// `stock`; a liberação gradual consome só ele (compra/liberação da IA
		// mexe no estoque retido, não no pulmão).
		std::vector<double> inflow_stock;
		// Fluxos do dia (derivados, zerados no `phase_reset`).
		double bought_today_t = 0.0;   // compra do mercado para o armazém
		double released_today_t = 0.0; // devolução do armazém ao mercado
		double received_today_t = 0.0; // colheita desviada para o silo da região
		double loss_today_t = 0.0;     // perecibilidade + perda de armazenagem
		double maintenance_paid_today = 0.0;
		double access_paid_today = 0.0; // custo de acesso região→cidade
	};

	// ------------------------------------------------------------------
	// §15 (construction as physical project, 2026-11-16). Toda construção ou
	// expansão vira um PROJETO que avança por dia: materiais entregues pelo
	// mercado, mão de obra/capacidade do setor de construção, prazo e dono
	// (state/private/grupo F9). O pagamento sai do dono conforme avança; sem
	// material ele PARA (não falha). Ao concluir, o prédio entra em operação
	// (ou a expansão soma capacidade). Ver sim/construction.{h,cpp}.
	// ------------------------------------------------------------------
	// Kind da obra na fila única da cidade (§15 unificado): prédio econômico,
	// moradia (§19 W3) ou infraestrutura urbana (§19 W1). Capacidade de
	// construção, materiais (mercado), terra (prefeitura) e dono são os MESMOS
	// para todos; a conclusão despacha pelo kind.
	enum ConstructionKind : int {
		CONSTRUCTION_BUILDING = 0,
		CONSTRUCTION_HOUSING = 1,
		CONSTRUCTION_URBAN_INFRA = 2,
		// B5: obra de infraestrutura da REGIÃO/estado (estrada/energia/água).
		// Não cria prédio: ao concluir sobe o nível no Region da cidade.
		CONSTRUCTION_REGION_INFRA = 3,
		// G2: obra de ARMAZÉM agregado (silo/tanque/câmara fria/...). Não cria
		// prédio: ao concluir soma capacidade ao `StorageSite` do dono.
		CONSTRUCTION_STORAGE = 4,
		// PARTE E (D-116): usina nova e linha de conexão à rede. A usina vira
		// `PowerPlant` ao concluir; a linha muda o estado da cidade
		// (isolada/parcial → conectada) com a capacidade `power_mw`.
		CONSTRUCTION_POWER_PLANT = 5,
		CONSTRUCTION_POWER_LINE = 6,
	};

	struct ConstructionProject {
		std::string id;          // id do projeto (== building_id em obra nova)
		std::string city;
		std::string owner;       // "state" | "private" | "national:<g>" | "foreign:<g>@<n>"
		std::string group_id;    // grupo financiador (vazio p/ state/private)
		std::string resource;    // bem produzido (id de goods.json)
		int output_product = -1;
		int recipe = -1;
		int input_product = -1;
		double capacity = 0.0;   // capacidade nova (obra) ou adicionada (expansão)
		double jobs = 0.0;
		double wage = 0.0;
		double opex = 0.0;
		double input_per_output = 0.0;
		double education_mix[kEducationLevels] = {0.90, 0.09, 0.01};
		double local_share = 0.0;
		std::string partner_group;
		bool strategic = false;      // planta estratégica (bootstrap industrial)
		bool expand_existing = false; // true = soma capacidade a building_id
		std::string building_id;     // prédio final (obra) ou alvo (expansão)
		// Economia do projeto: `total_cost` é o orçamento estimado; o dono paga
		// conforme a obra avança (não tudo no início).
		double total_cost = 0.0;
		double labor_cost_total = 0.0; // parcela de mão de obra (1 - material_share)
		double paid = 0.0;             // total pago pelo dono até agora
		double labor_paid = 0.0;       // mão de obra paga até agora
		// §15 calibração: o capital da obra é INVESTIMENTO, não caixa de giro.
		// O dono reserva o custo total no início (`reserved`, debitado do caixa)
		// e a obra paga material/serviço desse fundo ao avançar — NÃO compete
		// com a folha/opex e não pausa por liquidez temporária (uma mina em
		// cidade importadora pausava por "dono sem dinheiro" e nunca concluía,
		// tirando a renda que paga a importação de comida — SOL/DJI/SAH). O
		// `reserved` entra em `money_total` para a conservação valer.
		double reserved = 0.0;         // fundo de obra ainda não gasto
		// Materiais da receita de obra (construction_material, steel_structure,
		// electrical_material; fábricas também industrial_machinery).
		int material_count = 0;
		int materials[kMaxConstructionMaterials] = {};
		double material_qty[kMaxConstructionMaterials] = {};      // t totais
		double material_delivered[kMaxConstructionMaterials] = {}; // t entregues
		// Cronograma. Sem material/sem dinheiro, `days_elapsed` não avança e o
		// projeto para (pausa declarada em pause_reason).
		int duration_days = 30;
		int days_elapsed = 0;
		bool paused = false;
		std::string pause_reason;
		int created_day = 0;
		// §19 (W1): obra de INFRAESTRUTURA URBANA (não cria prédio). 0 = obra
		// econômica normal; 1 = expandir zona (soma area_km2 e infra_capacity);
		// 2 = adensar zona (sobe max_density em um passo). `urban_zone` é o
		// Zone alvo e `urban_km2` a área adicionada (só expansão).
		int urban_action = 0;
		int urban_zone = 0;
		double urban_km2 = 0.0;
		int kind = CONSTRUCTION_BUILDING;
		// Moradia (kind HOUSING): densidade, padrão, unidades entregues e dono
		// (HOUSING_PROJECT_PRIVATE/SELF/GOVERNMENT, ver housing.h).
		int housing_density = 0;
		int housing_tier = 0;
		int housing_owner = 0;
		double housing_units = 0.0;
		// Parcela do `total_cost` paga à PREFEITURA pelo uso do solo
		// (land_price da zona × área ocupada). Já saiu do caixa do dono na
		// abertura; `reserved` é o resto (material + mão de obra).
		double land_cost = 0.0;
		// D-086: obra de APROFUNDAMENTO de mina. >= 0 = o estágio que a obra
		// abre ao concluir (building_id é a mina alvo; a capacidade é somada e
		// o estágio novo reivindica uma fatia da reserva restante da jazida).
		// -1 = obra normal (prédio novo, expansão, moradia, infra).
		int deepen_stage = -1;
		// B5: tipo de infra da região (0 estrada, 1 energia, 2 água) quando
		// kind == CONSTRUCTION_REGION_INFRA. -1 = não se aplica.
		int infra_type = -1;
		// G2: armazém agregado quando kind == CONSTRUCTION_STORAGE. O dono vem
		// de `owner` ("state" = governo, "private" = firma/cooperativa).
		int storage_type = -1;
		double storage_capacity_t = 0.0;
		// PARTE E (D-116): obra de energia — fonte (PowerFuel), potência em MW
		// da usina nova ou capacidade da linha. -1 = não se aplica.
		int power_fuel = -1;
		double power_mw = 0.0;
	};

	struct Pop {
		double population = 0.0;
		double labor_force = 0.0;
		double employed = 0.0;
		double unemployed = 0.0;
		double dependents = 0.0;
		double wage_income = 0.0;
		double capital_income = 0.0;
		double transfers = 0.0;
		double taxes = 0.0;
		double service_spend = 0.0;
		double disposable = 0.0;
		// Renda externa do dia (serviços urbanos/pesca vendidos ao resto do
		// mundo). Não é salário doméstico: entra no orçamento das famílias e é
		// paga pelo setor externo (foreign_cash), conservando o dinheiro total.
		double external_income = 0.0;
		// B2: aluguel RECEBIDO pelas famílias donas no dia (credita no próximo
		// dia, como o lucro distribuído). Separado do capital para o status por
		// estrato; os dois vão para os quintis donos.
		double rent_income = 0.0;
		double food_spend = 0.0;
		double consumed = 0.0;
		double need = 0.0;
		double satisfaction = 0.0;
		double savings = 0.0;
		// C1 (save v26) — ATIVO financeiro das famílias: salário atrasado a
		// receber (o espelho do passivo das firmas da cidade + região). NÃO é
		// poupança e NÃO entra em `money_total` (é uma obrigação, não dinheiro).
		// O rateio por quintil é DERIVADO na leitura (participação da folha do
		// dia anterior), não é um segundo número persistido.
		double arrears_receivable = 0.0;
		// Consumo por categoria/slot da cesta alimentar (data-driven).
		std::vector<double> desired_by_category;
		std::vector<double> consumed_by_category;
		// F3: necessidades em três níveis. `level_*` têm kMaxNeedsLevels
		// posições (t-eq/dia). `needs_satisfaction` é a média ponderada dos
		// níveis com dado; `needs_level` é o maior nível atendido (>= metade da
		// cesta). `pop.satisfaction` continua sendo a satisfação ALIMENTAR
		// (contrato do status/UI legado e do teste de fome).
		std::vector<double> level_want;
		std::vector<double> level_consumed;
		std::vector<double> level_satisfaction;
		std::vector<double> durable_stock;  // por produto (toneladas)
		double needs_spend = 0.0;           // life + luxury no dia
		double durable_spend = 0.0;         // compras de duráveis no dia
		double needs_satisfaction = 0.0;
		int needs_level = 0;
		// ------------------------------------------------------------------
		// F7 — população ativa, educação e mercado de trabalho.
		//
		// A POP é dividida em: idade ativa (15–64), força de trabalho (idade
		// ativa × participação, menos quem estuda), empregados/desempregados por
		// nível de educação, e fora da força (crianças, aposentados, estudantes).
		// `education` guarda a escolaridade da população em idade ativa; quem
		// estuda continua contado no nível de ORIGEM até concluir e migrar.
		// `population` continua sendo a unidade de simulação (cidade), e
		// `dependents = population - labor_force` preserva o contrato anterior.
		// ------------------------------------------------------------------
		double working_age = 0.0;      // 15–64 anos
		double retirees = 0.0;         // 65+ (fora da força por idade)
		double students = 0.0;         // em formação (fora da força)
		double recruit_reserve = 0.0;  // 18–49 (só número; exército futuro)
		// ------------------------------------------------------------------
		// B1 — demografia por coortes. `age_band` é a POP em pessoas nas cinco
		// faixas (0-14, 15-24, 25-44, 45-64, 65+), semeada da estrutura etária
		// nacional e movida um mês por vez pela fase de demografia.
		// `demography_ready` falso = cidade sem dado nacional: idade ativa,
		// aposentados e reserva recrutável continuam vindos do perfil
		// paramétrico (comportamento legado). `children` é a coorte 0-14 e entra
		// na POP fora da força (`students`). Os fatores de necessidade são o
		// peso das coortes relativo à composição do dia da semeadura (1,0 no
		// legado), aplicados sobre a necessidade per capita de saúde/educação.
		// ------------------------------------------------------------------
		double age_band[5] = {};
		bool demography_ready = false;
		double children = 0.0;
		double health_need_factor = 1.0;
		double education_need_factor = 1.0;
		double health_weight_ref = 0.0;
		double education_weight_ref = 0.0;
		double education[kEducationLevels] = {};
		double participation[kEducationLevels] = {};
		double labor_force_by_level[kEducationLevels] = {};
		double employed_by_level[kEducationLevels] = {};
		double unemployed_by_level[kEducationLevels] = {};
		double vacancies_by_level[kEducationLevels] = {};
		// Vagas que sobraram SEM candidato após a alocação em cascata (nem
		// sobrequalificados). É a escassez real do nível; o desemprego do mesmo
		// nível é a sobra. juntos, definem o aperto que move o salário.
		double unfilled_by_level[kEducationLevels] = {};
		double wage_by_level[kEducationLevels] = {};
		double wage_income_by_level[kEducationLevels] = {};
		// `true` depois do primeiro ajuste ancorado no valor adicionado: a
		// âncora inicial de criação do mundo é um palpite, não um salário
		// histórico que precise de inércia.
		bool wage_anchored = false;
		// Em formação para o nível 1 (intermediário) e 2 (superior). A
		// conclusão é gradual (coorte exponencial com duração em dias).
		double studying[kEducationLevels - 1] = {};
		// Nível de consumo alcançável pela RENDA corrente (0 = nenhum,
		// 1 = basic, 2 = life, 3 = luxury). O gasto sequencial da F3 continua
		// sendo o que compra; este nível é o teto que a renda permite.
		int income_level = 0;
		// Gasto do dia por nível de necessidade (para estimar o custo unitário
		// da cesta que a renda consegue pagar amanhã).
		std::vector<double> level_spend;
		// Vagas EFETIVAMENTE preenchidas por nível (inclui o sobrequalificado
		// que ocupa a vaga de baixo). É o peso da folha: o salário-alvo de cada
		// nível é normalizado por ele para que a folha total seja exatamente a
		// parcela do trabalho no valor adicionado.
		double filled_by_slot[kEducationLevels] = {};
		// ------------------------------------------------------------------
		// F7 calibração — RENDA POR ESTRATO. A POP é dividida em estratos de
		// renda alinhados ao nível de educação/ocupação (básico, intermediário,
		// superior). Cada estrato tem a sua própria renda e o seu próprio nível
		// de consumo (basic/life/luxury), então um país tem fatias em níveis
		// diferentes em vez de um único nível para a cidade inteira. O lucro
		// (renda de capital) vai só para os DONOS — o estrato superior —, não
		// é dividido igualmente entre toda a POP.
		// ------------------------------------------------------------------
		double stratum_population[kEducationLevels] = {};
		double stratum_wage_income[kEducationLevels] = {};
		double stratum_external_income[kEducationLevels] = {};
		double stratum_capital_income[kEducationLevels] = {};
		double stratum_disposable[kEducationLevels] = {};
		int stratum_income_level[kEducationLevels] = {};
		// ------------------------------------------------------------------
		// F7 calibração — CLASSES DE CONSUMO ANCORADAS NO GINI.
		//
		// Quando a nação tem Gini declarado (`income_gini > 0`), a repartição
		// da POP em basic/life/luxury NÃO vem da escolaridade: vem de uma
		// distribuição lognormal com média no PIB PPP per capita real
		// (índice × kIncomeAnchorWorldMean cestas básicas) e sigma derivado do
		// Gini. Classe 0 = abaixo da cesta básica; 1 = acima da básica e abaixo
		// do luxo; 2 = acima de kIncomeLuxuryMultiple cestas básicas. Os estratos
		// de escolaridade continuam existindo para o mercado de trabalho; a
		// classe é quem define o teto de consumo (e o que o status publica).
		// Sem Gini (`income_gini == 0`, cidades sintéticas/save antigo), as
		// classes espelham os estratos e o comportamento anterior é preservado.
		// ------------------------------------------------------------------
		double class_population[kEducationLevels] = {};
		int class_income_level[kEducationLevels] = {};
		// Fração da POP por nível de consumo alcançado (índice 0 = nenhum,
		// 1 = basic, 2 = life, 3 = luxury). É o que o status/report publica.
		double income_level_share[kIncomeLevels] = {};
		// ------------------------------------------------------------------
		// B2 — ESTRATOS DE RENDA (QUINTIS) COM PATRIMÔNIO.
		//
		// A POP é repartida em 5 quintis de renda (20% de pessoas cada) numa
		// dimensão INDEPENDENTE da escolaridade. Cada estrato tem composição
		// educacional própria (acesso a vaga), dispersão salarial própria
		// (`wage_factor`), participação no capital (`capital_share`), poupança,
		// dívida (0 até a Parte C), patrimônio e posse de moradia.
		//
		// A repartição INICIAL vem de dado real por nação (quintis de renda do
		// World Bank SI.DST via `strata_baseline.json`); sem o dado, o núcleo
		// deriva a proxy lognormal do Gini declarado. O Gini do dia 365 NÃO é
		// imposto: ele emerge das rendas pagas (salário por escolaridade ×
		// fator de dispersão, capital pelos donos, aluguel recebido).
		// `strata_ready` distingue a cidade semeada do save legado/sintético.
		// ------------------------------------------------------------------
		bool strata_ready = false;
		double strata_income_share[kIncomeStrata] = {};
		double stratum_people[kIncomeStrata] = {};
		double stratum_wage[kIncomeStrata] = {};
		double stratum_capital[kIncomeStrata] = {};
		double stratum_rent[kIncomeStrata] = {};
		double stratum_transfer[kIncomeStrata] = {};
		double stratum_net_income[kIncomeStrata] = {};
		double stratum_savings[kIncomeStrata] = {};
		double stratum_debt[kIncomeStrata] = {};
		// Participações em grupos (avaliação, não dinheiro): semeada como
		// resíduo da riqueza-alvo documentada; sem fluxo até a Parte C5.
		double stratum_equity[kIncomeStrata] = {};
		double stratum_wealth[kIncomeStrata] = {};
		double stratum_saving_rate[kIncomeStrata] = {};
		double stratum_service_spend[kIncomeStrata] = {};
		double stratum_rent_paid[kIncomeStrata] = {};
		double stratum_rent_bill[kIncomeStrata] = {};
		double stratum_food_spend[kIncomeStrata] = {};
		double stratum_need[kIncomeStrata] = {};
		double stratum_consumed[kIncomeStrata] = {};
		// Dispersão salarial DENTRO do nível de escolaridade (experiência,
		// jornada, informalidade): normalizada para média 1 no dia da
		// semeadura a partir dos quintis de renda observados.
		double stratum_wage_factor[kIncomeStrata] = {};
		// Participação na renda de capital/aluguel (riqueza mais concentrada
		// que a renda; proxy documentada, ver `set_city_strata`).
		double stratum_capital_share[kIncomeStrata] = {};
		double stratum_education_mix[kIncomeStrata][kEducationLevels] = {};
		double stratum_tenure[kIncomeStrata][kStrataTenureCount] = {};
		// Teto de consumo alcançado pelo estrato hoje (0 = nenhum, 1 = basic,
		// 2 = life, 3 = luxury).
		int stratum_level[kIncomeStrata] = {};
		// B2 (correção): a repartição vigente veio de DADO observado
		// (`set_city_strata`) e não da proxy lognormal do Gini. Só a proxy é
		// re-semeada quando o Gini é declarado depois do `initialize_world`.
		// Não entra no save: é derivado (o load mantém a repartição gravada).
		bool strata_from_data = false;
		// Fator de ciclo de vida da taxa de poupança por faixa etária da
		// cidade (B1): crianças 0, jovens/idiosos < meia-idade. Publicado
		// para a validação "meia-idade poupa mais".
		double lifecycle_saving[5] = {};

		// ------------------------------------------------------------------
		// B7 — INFORMALIDADE, MORTALIDADE E INSATISFAÇÃO (save v22).
		// ------------------------------------------------------------------
		// Trabalho informal: fração informal do emprego (agregado) e por
		// quintil; renda informal e folha do dia (informativas). A fração é
		// semeada do dado nacional e formaliza mensalmente (cadência A6).
		double informal_share = 0.0;
		double informal_labor_share[kIncomeStrata] = {};
		double informal_income[kIncomeStrata] = {};
		double informal_wage_bill = 0.0;
		// Serviços informais (agregado, SEM firma individual).
		double informal_service_workers = 0.0;
		double informal_service_output = 0.0;
		double informal_service_demand = 0.0;
		double informal_service_revenue = 0.0;
		double informal_service_price = 0.0;
		double informal_service_share = 0.0;
		// Mortalidade: base da tabela (esperada) e efetiva do último
		// fechamento; excesso acumulado desde o dia 0 (fome + saúde e só a
		// parcela de comida); meses consecutivos de fome (migração B4);
		// custo da cesta no último fechamento e a variação desde então.
		double deaths_expected = 0.0;
		double deaths_total = 0.0;
		double excess_deaths_total = 0.0;
		double excess_deaths_famine_total = 0.0;
		int famine_months = 0;
		double food_price_ref = 0.0;
		double food_inflation_month = 0.0;
		double health_access_ref = 0.0;
		// Insatisfação (medição 0..1) e componentes publicados.
		double dissatisfaction = 0.0;
		double dissatisfaction_by_stratum[kIncomeStrata] = {};
		double dissatisfaction_unemployment = 0.0;
		double dissatisfaction_rent = 0.0;
		double dissatisfaction_food = 0.0;
		double dissatisfaction_inflation = 0.0;
		double dissatisfaction_informality = 0.0;
		// Derivados (NÃO persistidos): satisfação do dia anterior (insumo da
		// mortalidade mensal, que roda antes da renda recomputar a de hoje) e
		// o fator de crise da renda informal (pay_scale do dia). Semeado o
		// informal a partir do dado nacional (evita re-semear em re-seed).
		double satisfaction_prev = 1.0;
		double informal_crisis_factor = 1.0;
		bool informality_seeded = false;

	};

	// ------------------------------------------------------------------
	// F8 — SERVIÇOS. 20 setores data-driven (services.json). O núcleo guarda
	// definições carregadas uma vez e, por cidade/setor, um mercado de
	// unidades/dia (preço na âncora XGS) + os negócios que ofertam.
	//
	// O dinheiro NÃO nasce em lugar novo: as famílias pagam com
	// `pop.service_spend`, as empresas com `opex_revenue`, o governo com o
	// orçamento e o resto do mundo com `foreign_cash`; a receita dos negócios é
	// uma ALOCAÇÃO do `city.service_cash` que já recebeu esses fluxos. Entrada
	// de negócio financia o capital com o CAIXA DO PRÓPRIO SETOR (service_cash,
	// lucros retidos) — a poupança que paga comida não é tocada — e a saída
	// baixa a equity residual; `money_total` continua conservado.
	// ------------------------------------------------------------------
	struct ServiceSector {
		std::string id;
		std::string name_pt;
		std::string name_en;
		std::string name_es;
		std::string icon;
		double price = 1.0;
		double pop_share[kMaxNeedsLevels] = {};
		double business_share = 0.0;
		double freight_share = 0.0;
		double government_share = 0.0;
		struct Input {
			int product = -1;
			double qty = 0.0;
		};
		std::vector<Input> inputs;
		std::vector<int> service_inputs;
		std::vector<double> service_input_qty;
		double education_mix[kEducationLevels] = {0.70, 0.25, 0.05};
		double jobs_per_unit = 3.0;
		double capital_per_unit = 96.0;
		double min_capacity = 5.0;
		bool exportable = false;
		double export_share = 0.0;
		bool health = false;
		bool education = false;
		std::string source = "estimated:structure";
	};

	// Estado diário de um setor numa cidade. `revenue` é dinheiro do pool de
	// serviços (famílias + B2B + governo + exportação), não unidades.
	struct ServiceMarket {
		double demand = 0.0;       // unidades desejadas e financiadas
		double supply = 0.0;       // unidades ofertadas pelos negócios
		double unmet = 0.0;        // demanda financiada sem oferta
		double price = 1.0;        // âncora XGS por unidade
		double base_price = 1.0;
		double revenue = 0.0;
		double input_cost = 0.0;   // bens comprados no mercado da cidade
		double service_cost = 0.0; // serviços intermediários comprados
		double jobs = 0.0;         // fatia de city.service_jobs
		double businesses = 0.0;
		double exports = 0.0;      // unidades exportadas
		double export_revenue = 0.0;
		double coverage = 0.0;     // saúde/educação privadas: 0..1
		// F8: demanda das famílias por nível de consumo (unidades/dia), o que
		// permite testar/auditar a separação basic/life/luxury.
		double pop_demand[kMaxNeedsLevels] = {};
	};

	struct ServiceBusiness {
		int id = 0;
		int sector = -1;
		std::string city;
		int owner_stratum = kEducationLevels - 1;
		double capacity = 0.0;
		double jobs = 0.0;
		double workers = 0.0;
		double capital = 0.0;
		double capital_left = 0.0;
		double utilization = 0.0;
		double revenue = 0.0;
		double wages = 0.0;
		double input_cost = 0.0;
		double service_cost = 0.0;
		double profit = 0.0;
		int loss_days = 0;
		bool bankrupt = false;
		int opened_day = 0;
		// Semente estável do nome: o rótulo é gerado no GDScript por cultura do
		// dono (`BusinessNameGenerator`); o núcleo guarda só o número.
		unsigned long long name_seed = 0;
	};

	// Reserva mineral do território da cidade para um produto. D-086 (decisão do
	// dono): é só REFERÊNCIA de profundidade — escala a curva de custo do
	// aprofundamento e a fatia que cada estágio reivindica; a extração NUNCA
	// zera nem bloqueia obra. `remaining_kt` continua sendo debitado
	// (contabilidade, cai a 0 e fica lá) e `extracted_kt` acumula o que já saiu
	// (ultrapassar a referência encarece o aprofundamento). Sem `tracked`, o
	// produto não tem reserva declarada e a extração é ilimitada (comportamento
	// legado, p.ex. ferro sem dados de depósito).
	struct Deposit {
		bool tracked = false;
		double initial_kt = 0.0;   // reserva de REFERÊNCIA (escala de custo)
		double remaining_kt = 0.0; // saldo contábil (pode chegar a 0 e seguir)
		double quality = 1.0;      // 0..1: eficiência da extração
		double depth_m = 0.0;      // profundidade média: encarece o opex
		double extracted_kt = 0.0; // extraído acumulado (além da referência encarece)
	};

	// F4: frota nacional de frete. Os veículos são um estoque físico (1 t do
	// bem `truck`/`cargo_ship`/`cargo_plane` = 1 veículo — a produção da F2
	// entra no estoque) e o combustível é consumido por t·km de cada trecho.
	// `health` (0,5..1,0) reduz a capacidade quando faltam `spare_parts` na
	// manutenção mensal.
	struct Fleet {
		double trucks = 0.0;
		double ships = 0.0;
		double planes = 0.0;
		// Veículos ocupados por cargas em trânsito (liberados na chegada).
		double trucks_busy = 0.0;
		double ships_busy = 0.0;
		double planes_busy = 0.0;
		// Alvo derivado de população/PIB (init_fleets), usado pela
		// reposição mensal do governo.
		double target_trucks = 0.0;
		double target_ships = 0.0;
		double target_planes = 0.0;
		double health[3] = {1.0, 1.0, 1.0};
		double fuel = 0.0;         // t de combustível no estoque da nação
		// Reserva-alvo de combustível (t), derivada de população/PIB na criação
		// da frota; a compra mensal repõe o consumo até este nível.
		double fuel_target = 0.0;
		double spare_parts = 0.0;  // t de peças no estoque da nação
		double fuel_used_today = 0.0;
		double fuel_used_total = 0.0;
		double fuel_bought_total = 0.0;
		double parts_used_total = 0.0;
		double vehicles_bought_total = 0.0;
		// Carga (t) que não embarcou no dia por falta de veículo/combustível.
		double queue_today = 0.0;
		double missed_no_fleet = 0.0;
		double missed_no_fuel = 0.0;
		// true = frota inicial derivada de população/PIB (source: derived).
		bool derived = true;
	};

	// F5: moeda de uma nação. `rate` = valor em âncora (XGS) de 1 unidade
	// local. `balance_ema` é a balança comercial (exportações - importações,
	// em âncora) suavizada em 30 dias; `gross_ema` é o volume (export+import)
	// suavizado, usado para normalizar a pressão cambial. `peg` congela a taxa,
	// `float` (padrão quando o dado não declara regime) aplica o drift dentro
	// da banda [kFxBandLow, kFxBandHigh] × taxa inicial.
	struct Currency {
		std::string code;
		std::vector<std::string> nations;
		double rate = 1.0;
		double base_rate = 1.0;
		bool floating = true;
		bool fallback = false;
		std::string source = "default";
		double balance_ema = 0.0;
		double gross_ema = 0.0;
		// C2a (D-103) — POLÍTICA da moeda (salva no v27, sem mecânica nesta
		// fase; C2b liga emissão/juros/intervenção). Os enums estão em
		// central_bank.h: 0 floating / 1 peg / 2 band;
		// 0 no_data / 1 independent / 2 controlled. `exchange_regime` nasce do
		// `floating` atual (peg é o default do dado, que não traz fonte de
		// regime) e `bank_independence` nasce `no_data` — ausência declarada.
		int exchange_regime = 1;
		int bank_independence = 0;
	};

	// F5: tarifa declarada (buyer, seller, product, ad valorem). O lookup usa
	// uma chave textual buyer|seller|product; a ordem do vetor é determinística
	// para o status.
	struct Tariff {
		std::string buyer;
		std::string seller;
		std::string product;
		double rate = 0.0;
	};

	// F9: grupo/empresa com identidade. O nome vem do cliente (cultura de
	// origem); o núcleo guarda só o que precisa para conservar dinheiro e
	// publicar o status. `profit_by_host` acumula o lucro ainda não distribuído
	// por nação anfitriã — é o que permite separar repatriação (IDE) de
	// dividendo doméstico sem varrer as construções a cada pagamento.
	struct Group {
		std::string id;
		std::string name;
		std::string origin_nation;
		std::string sector;
		double cash = 0.0;
		// Lucro acumulado ainda não distribuído. Separado do caixa para que uma
		// capitalização (fund_group) não vire dividendo no dia seguinte.
		double distributable = 0.0;
		double revenue_total = 0.0;
		double profit_total = 0.0;
		double dividends_total = 0.0;
		double taxes_total = 0.0;
		double capex_total = 0.0;
		bool state_owned = false;
		std::map<std::string, double> profit_by_host;
		// C3 (D-107) - CRÉDITO DE EMPRESA: o grupo passa a ter balanço com
		// dívida. `debt` é o principal em aberto com o banco agregado da nação
		// de origem; `credit_arrears` é o juro vencido e NÃO pago (não é
		// capitalizado em silêncio: fica como conta a pagar do grupo e a
		// receber do banco, como o salário atrasado da C1).
		double debt = 0.0;
		double credit_arrears = 0.0;
		double credit_total = 0.0;   // principal emprestado acumulado
		double credit_repaid = 0.0;  // principal pago acumulado
		int credit_default_days = 0;
		int credit_defaults = 0;
	};

	// C4 (D-110) - BANCO COM DONO: a C3 tinha "1 banco agregado por nação";
	// aqui cada nação ganha até TRÊS bancos, identificados por papel: público
	// (dono Estado/Tesouro), privado nacional (dono as famílias da nação) e
	// estrangeiro (dono grupo/nação de outro país; só existe quando o dado
	// declara). Contrato em docs/economy/C4_banks.md.
	//
	// Dinheiro bancário: o empréstimo cria DEPÓSITO (`deposits`, passivo do
	// banco e caixa do tomador), não M0 — `money_total` sobe, `M0` não
	// (M1 = M0 + depósitos). O capital pago pelo dono é claim dele
	// (`capital`), o lucro retido é `equity − capital` e o dividendo mensal
	// converte parte do retido em depósito do dono. Calote e resolução são
	// C6: o patrimônio pode ficar negativo e nada socorre o banco.
	struct Bank {
		std::string id;             // "<NAÇÃO>/<papel>"
		std::string nation;
		std::string currency;
		// 0 = público (Estado), 1 = privado nacional (famílias), 2 = estrangeiro.
		int role = 1;
		std::string name;           // banks.json (fictício/genérico); vazio = no_data
		std::string name_status = "no_data";  // present | no_data
		std::string owner_kind = "national";  // state | national | foreign
		std::string owner_nation;   // país de origem do banco estrangeiro
		double share = 0.0;         // participação inicial declarada (soma 1)
		double loans = 0.0;         // principal a receber (ativo)
		double arrears = 0.0;       // juros vencidos a receber (ativo)
		double deposits = 0.0;      // depósitos à vista (passivo; dinheiro bancário)
		double bc_borrowing = 0.0;  // dívida com o BC (reserva criada; 0 na C4)
		double cash = 0.0;          // caixa do banco (dinheiro do mundo)
		double capital = 0.0;       // capital pago pelo dono (claim do dono)
		double interest_income = 0.0;
		double interest_expense = 0.0;
		double writeoffs = 0.0;     // perda acumulada por calote
		double dividends = 0.0;     // dividendo distribuído acumulado (âncora)
		double remittance = 0.0;    // remessa do estrangeiro ao país de origem (âncora)
		double remittance_origin = 0.0;  // a mesma remessa na moeda de origem (C2c)
		double harvest_interest = 0.0;   // juro do envelope da safra recebido
		int defaults = 0;
		int loans_count = 0;
		int last_default_day = -1;
		int last_dividend_day = -1;
		// ------------------------------------------------------------------
		// C6 (D-114) - CRISE, CORRIDA E RESOLUCAO. Contrato em
		// docs/economy/C6_bank_crisis.md e parametros em
		// data/definitions/economy/bank_crisis.json (default de jogo).
		//
		// `confidence` e a confianca do depositante (0..1), suavizada por dia:
		// nasce 1,0 e converge para a leitura MEDIDA de solvencia, liquidez,
		// perdas recentes, contagio da mesma nacao e confianca da bolsa (C5).
		// Abaixo de `run_threshold` o banco entra em corrida: uma fracao
		// declarada dos depositos sai por dia e vira papel-moeda (reserva
		// liberada), deposito do banco publico (fuga para qualidade) ou
		// remessa ao exterior (banco estrangeiro). Tudo conserva dinheiro.
		//
		// O banco abaixo do capital minimo corta credito novo; o insolvente
		// (patrimonio < 0) passa pela hierarquia de resolucao: acionistas
		// zerados -> deposito nao segurado cortado -> fundo de seguro ->
		// Tesouro/divida publica -> incorporacao pelo banco publico. Os
		// custos ficam registrados em `BankCrisisEvent` e somam o rombo.
		// ------------------------------------------------------------------
		double confidence = 1.0;
		bool loss_seeded = false;    // 1a perda entra inteira na EMA
		double loss_ema = 0.0;       // perda recente (EMA, valor absoluto)
		// Choque reconhecido HOJE (medicao). Vai ao save v34: sem ele, um load
		// no meio do dia perdia a perda do dia e a continuação divergia.
		double loss_today = 0.0;
		// C6: a confianca mede DETERIORACAO contra a referencia do proprio
		// banco (o estado que o depositante conhecia), nao um nivel absoluto:
		// o banco da C4 nasce com patrimonio fino e sem reserva, e usar o
		// nivel absoluto faria todo o mundo base correr sem nenhum choque.
		bool baseline_seeded = false;
		double open_capital_ratio = 0.0;
		double open_liquidity_ratio = 0.0;
		int run_days = 0;
		int last_run_day = -1;
		double run_withdrawn = 0.0;  // saques acumulados (ancora)
		double run_cash = 0.0;       // parte paga em papel-moeda
		double run_public = 0.0;     // parte migrada ao banco publico
		double lolr_borrowed = 0.0;  // redesconto acumulado (BC)
		double lolr_interest = 0.0;  // juro de penalidade pago
		int lolr_days = 0;
		double insured_payout = 0.0; // seguro de deposito recebido
		double bailin_cut = 0.0;     // deposito NAO segurado cortado
		double treasury_rescue = 0.0;// Tesouro (caixa + divida emitida)
		double capital_flight = 0.0; // remessa da matriz estrangeira (ancora)
		int resolution_day = -1;
		bool resolved = false;
		std::string resolution = "none";  // bailin|insurance|treasury|takeover|liquidation
		// Banco absorvido pelo publico: a linha fica como casca (saldos 0,
		// share 0) para nao recriar o papel e para o tomador continuar mapeado.
		std::string absorbed_into;
		// Indicadores do ULTIMO fechamento (publicados; recomputados por dia).
		double last_capital_ratio = 0.0;
		double last_liquidity_ratio = 0.0;
		double last_npl_ratio = 0.0;
	};

	// C6 (D-114) - FUNDO DE SEGURO DE DEPOSITO por nacao. Entra no dinheiro
	// do mundo (o premio sai do caixa do banco e o pagamento volta como caixa
	// do banco resolvido): nenhuma etapa cria ou some com dinheiro.
	struct DepositInsurance {
		double fund = 0.0;
		double premiums = 0.0;
		double payouts = 0.0;
		int last_payout_day = -1;
	};

	// C6 (D-114) - evento de crise (anel de 64; o contador total fica no
	// `WorldState`). Todo custo e registrado por PARTE: o que o acionista
	// perdeu, o que o depositante nao segurado perdeu, o que o fundo pagou, o
	// que o Tesouro pagou e o que a incorporacao custou ao banco publico.
	struct BankCrisisEvent {
		int day = -1;
		std::string bank;      // "<NACAO>/<papel>"
		std::string nation;
		// run | lolr | resolution | insurance | treasury | takeover |
		// liquidation | credit_cut
		std::string kind;
		std::string cause;
		double gap = 0.0;               // rombo reconhecido (patrimonio < 0)
		double cost_shareholders = 0.0;
		double cost_depositors = 0.0;
		double cost_fund = 0.0;
		double cost_treasury = 0.0;
		double cost_takeover = 0.0;
		double capital_flight = 0.0;
		double lolr = 0.0;
		double emission = 0.0;
	};

	// C6 (D-114) - REGRAS da crise (0 = default declarado em
	// `sim/bank_crisis.h`, espelho de bank_crisis.json). `enabled` e ESTADO
	// (vai ao save); o resto e regra carregada pelo dado/comando.
	struct BankCrisisPolicy {
		bool enabled = true;
		double capital_min = 0.0;         // 0 = kCapitalMinDefault
		double capital_cut_floor = 0.0;   // 0 = kCapitalCutFloorDefault
		// RESERVADO: nenhuma regra consome o minimo de liquidez hoje (o sinal
		// de confianca usa a referencia do proprio banco, nao um piso). Fica
		// no save/status como parametro declarado, sem prometer efeito.
		double liquidity_min = 0.0;       // reservado (0 = kLiquidityMinDefault)
		double loss_scale = 0.0;          // 0 = kLossScaleDefault
		double loss_ema_days = 0.0;       // 0 = kLossEmaDaysDefault
		double confidence_start = 0.0;    // 0 = kConfidenceStartDefault
		double smoothing = 0.0;           // 0 = kConfidenceSmoothingDefault
		double w_solvency = 0.0;
		double w_liquidity = 0.0;
		double w_losses = 0.0;
		double w_contagion = 0.0;
		double w_market = 0.0;
		double run_threshold = 0.0;       // 0 = kRunThresholdDefault
		double run_share = 0.0;           // 0 = kRunShareDefault
		double flight_to_quality = 0.0;   // 0 = kFlightToQualityDefault
		double lolr_spread = 0.0;         // 0 = kLolrSpreadDefault
		double lolr_haircut = 0.0;        // 0 = kLolrHaircutDefault
		double lolr_max_assets = 0.0;     // 0 = kLolrMaxAssetsDefault
		double insured_share = 0.0;       // 0 = kInsuredShareDefault
		double premium_daily = 0.0;       // 0 = kPremiumDailyDefault
		double fund_target_ratio = 0.0;   // 0 = kFundTargetRatioDefault
		double capital_flight_share = 0.0;// 0 = kCapitalFlightShareDefault
		// Materialidade: patrimonio so e' "insolvente" abaixo desta tolerancia
		// (0 = default declarado). Sem ela, o arredondamento do juro deixava
		// equity = -1e-9 e o banco era "resolvido" por zero.
		double insolvency_tolerance = 0.0;  // 0 = kInsolvencyToleranceDefault
		// Tolerancia RELATIVA aos ativos (1e-6 = 1 parte por milhao): bancos
		// grandes tem ruido de juro maior que bancos pequenos.
		double insolvency_tolerance_ratio = 0.0;
		// Alavancas de resolucao (laboratorio/teste; default de jogo = true).
		bool treasury_debt_rescue = true; // Tesouro emite divida / BC carrega
		bool takeover_public = true;      // residual vira banco publico
	};

	// C5 (D-112) - BOLSA DE VALORES POR PAÍS: uma praça por nação com os grupos
	// listados. A ação é ativo do detentor e claim do emissor; o preço é de
	// ativo (valorização NÃO cria dinheiro) e só o dividendo move caixa.
	// Contrato em docs/economy/C5_stock_exchange.md.
	struct StockHoldings {
		double households_high = 0.0;         // ações do estrato alto da nação
		std::map<std::string, double> banks;  // banco -> ações
		std::map<std::string, double> groups; // grupo -> ações
		double foreign = 0.0;                 // exterior (banco estrangeiro)
		double total() const;
	};
	struct StockTicker {
		std::string group_id;
		std::string nation;    // nação da bolsa (origem do grupo)
		std::string exchange;  // id da bolsa == nação
		double shares = 0.0;
		double price = 0.0;
		// Patrimônio medido do grupo no último fechamento semanal (base do q de
		// Tobin, gravado para o score da IA não varrer as construções).
		double book_equity = 0.0;
		// Lucro diário esperado (EMA do lucro medido) e o acumulado do grupo no
		// último fechamento de dividendo (base do lucro do período).
		double profit_ema = 0.0;
		double profit_at_last_dividend = 0.0;
		double dividend_last = 0.0;
		double dividend_limited = 0.0;
		int dividend_day = -1;
		int listed_day = 0;
		int delisted_day = -1;
		bool listed = false;
		std::string reason = "listed"; // listed | no_origin | state_owned | ...
		StockHoldings holdings;
	};
	struct StockExchange {
		std::string nation;
		std::string name;                  // fictício/genérico; nunca marca real
		std::string name_status = "game_default";
		std::string currency;
		double confidence = 1.0;           // expectativa adaptativa
		double index_base = 0.0;           // market cap no dia de formação
		int last_price_day = -1;
		// Distribuição acumulada (soma zero: sai do emissor, entra no detentor).
		double dividends_paid = 0.0;
		double remittance = 0.0;           // fatia do exterior (âncora)
		double remittance_origin = 0.0;    // a mesma remessa na moeda de origem
		// Medição do dia (não serializada).
		double production_ema = 0.0;
		bool production_seeded = false;
	};

	// F9: abertura ao capital estrangeiro por (nação, setor).
	struct FdiPolicy {
		std::string nation;
		std::string sector;
		int mode = 0;  // 0 open, 1 restricted, 2 joint_venture, 3 closed
		double min_local = 0.0;
	};

	// F9: incentivo setorial (isenção/subsídio).
	struct SectorIncentive {
		std::string nation;
		std::string sector;
		double tax_holiday = 0.0;
		double subsidy = 0.0;
	};

	// F9: fluxo acumulado de lucro por nação (balança/câmbio futuros).
	struct GroupFlow {
		double repatriated_out = 0.0;  // lucro de estrangeiro que saiu do anfitrião
		double repatriated_in = 0.0;   // lucro recebido pela origem
		double dividends = 0.0;        // dividendos pagos aos donos nacionais
		double taxes = 0.0;            // imposto corporativo recebido
		double subsidy = 0.0;          // incentivo pago ao grupo
	};

	// Necessidade de transporte de um pedido (por trecho): veículos e fuel.
	struct TransportDemand {
		double trucks = 0.0;
		double ships = 0.0;
		double planes = 0.0;
		double fuel = 0.0;
		double distance_truck_km = 0.0;
		double distance_ship_km = 0.0;
	};

	// F9 — emprego setorial. Âncoras (renda, agro, indústria, informalidade)
	// ordenadas por renda; interpoladas no log da renda. Ver
	// data/definitions/economy/sector_employment.json (fonte ILO/ILOSTAT).
	struct SectorAnchor {
		double income = 1.0;
		double agriculture = 0.0;
		double industry = 0.0;
		double informal = 0.0;
	};

// ---------------------------------------------------------------------------
// B3 — REGIÃO DA CIDADE (estados inteiros, admin-1 do Natural Earth).
//
// Um bloco por cidade: toda a população fora da aglomeração (o interior) vive
// aqui. Bloco SIMPLIFICADO por decisão do dono: sem mercado de solo/moradia e
// sem prefeitura própria — a simulação urbana completa existe só na cidade. O
// interior reaproveita B1 (5 coortes etárias) e B2 (5 quintis de renda) numa
// dimensão separada da POP urbana. O caixa (`cash`) é dinheiro do mundo e entra
// em `money_total`. Ver docs/plans/b3_state_regions.md e a decisão D-085.
// ---------------------------------------------------------------------------
// Um estado admin-1 anexado à região da cidade (id estável do Natural Earth).
struct RegionState {
	std::string id;
	std::string name;
	double population = 0.0;
	double area_km2 = 0.0;
};

struct Region {
	// `ready` = tem dado de região (state_regions.json ou migração v17).
	// Falso em cenário sintético (AGRIA/PORTUS) e em cidade sem entrada.
	bool ready = false;
	// Nome do estado principal (maior população); vazio sem dado.
	std::string name;
	// População total da região (urbano + interior), a aglomeração (urbana) e o
	// interior. Invariante: population_region = urban_population + region_pop.
	double population_region = 0.0;
	double urban_population = 0.0;
	double region_pop = 0.0;
	// Urbanização nacional (World Bank SP.URB.TOTL.IN.ZS); -1 = sem dado
	// (mix neutro declarado). Alimenta `employment_mix` (fórmula em
	// sim_constants.h, kRegion*).
	double urban_share = -1.0;
	// Mix de emprego do interior: [0] agro/extração (primário), [1] indústria
	// leve, [2] serviços locais — soma 1. Derivado de `urban_share` (modelo).
	double employment_mix[3] = {};
	std::vector<RegionState> states;
	// B1: coortes etárias do interior (mesma ordem de Pop::age_band).
	double age_band[5] = {};
	bool demography_ready = false;
	// B2: quintis do interior (20% de pessoas cada; renda por quintil).
	bool strata_ready = false;
	double strata_income_share[kIncomeStrata] = {};
	double stratum_people[kIncomeStrata] = {};
	double stratum_savings[kIncomeStrata] = {};
	double stratum_net_income[kIncomeStrata] = {};
	double stratum_food_spend[kIncomeStrata] = {};
	double stratum_need[kIncomeStrata] = {};
	double stratum_consumed[kIncomeStrata] = {};
	// Caixa do interior (dinheiro do mundo) e fluxos do dia (derivados; não
	// precisam ser persistidos — são recalculados a cada dia).
	double cash = 0.0;
	// C1 (save v26) — ATIVO financeiro das famílias do interior: salário
	// rural atrasado a receber (o espelho do passivo `City::rural_wage_arrears`,
	// pago pelo caixa da firma da cidade-polo). Não é dinheiro.
	double arrears_receivable = 0.0;
	double wage_income = 0.0;   // salário do setor primário (semana)
	double income = 0.0;        // renda disponível do dia
	double employment = 0.0;    // empregados no setor primário
	double food_spend = 0.0;    // gasto com comida no dia
	double food_need = 0.0;     // necessidade (t-eq de grão)
	double food_consumed = 0.0; // consumido (t-eq de grão)
	double satisfaction = 0.0;  // satisfação alimentar (0..1)
	double taxes = 0.0;         // imposto pago à prefeitura
	// ------------------------------------------------------------------
	// B4 — migração (save v20): acumuladores persistentes + fluxos do último
	// fechamento mensal (derivados). A migração move PESSOAS (coortes e quintis)
	// e a POUPANÇA que as acompanha entre a região e o polo urbano (êxodo) e
	// entre cidades da mesma nação — conservando pessoas e dinheiro. Os
	// acumuladores contam desde o dia 0 do save; o fluxo do mês é sobrescrito no
	// dia da cadência A6 da cidade (como wage_income/income).
	// ------------------------------------------------------------------
	double migration_rural_in_total = 0.0;  // interior → urbano (êxodo) acumulado
	double migration_city_in_total = 0.0;   // outras cidades → esta, acumulado
	double migration_city_out_total = 0.0;  // esta → outras cidades, acumulado
	double migration_month_rural_in = 0.0;  // êxodo do último fechamento
	double migration_month_city_in = 0.0;   // entrada de outras cidades (janela própria)
	double migration_month_city_out = 0.0;  // saída cidade→cidade do último fecho
	double migration_month_net = 0.0;       // rural_in + city_in − city_out (último fecho)
	double migration_month_rate = 0.0;      // net ÷ população regional (fração/mês)
	// ------------------------------------------------------------------
	// B5 (save v21) - INFRAESTRUTURA do estado: níveis 0..kInfraMaxLevel de
	// estrada, energia (luz) e água. Persistidos. Efeitos: custo de acesso
	// (estrada), produtividade do trabalho primário (os três com pesos) e
	// êxodo (B4). Os multiplicadores são DERIVADOS (recalculados a cada
	// uso, não persistidos). Nível inicial sem dado = 0 (ausência explícita).
	// ------------------------------------------------------------------
	int infra_road = 0;
	int infra_power = 0;
	int infra_water = 0;
	// Manutenção do dia (derivada, recalculada todo dia; não persistida).
	double infra_maintenance = 0.0;
	// ------------------------------------------------------------------
	// B7 — INFORMALIDADE, MORTALIDADE E INSATISFAÇÃO (save v22). Mesmos
	// campos do bloco urbano (Pop), com os fluxos do interior.
	// ------------------------------------------------------------------
	double informal_share = 0.0;
	double informal_labor_share[kIncomeStrata] = {};
	double informal_income[kIncomeStrata] = {};
	double informal_wage_bill = 0.0;
	double informal_service_workers = 0.0;
	double informal_service_output = 0.0;
	double informal_service_demand = 0.0;
	double informal_service_revenue = 0.0;
	double informal_service_price = 0.0;
	double informal_service_share = 0.0;
	double deaths_expected = 0.0;
	double deaths_total = 0.0;
	double excess_deaths_total = 0.0;
	double excess_deaths_famine_total = 0.0;
	int famine_months = 0;
	double food_price_ref = 0.0;
	double food_inflation_month = 0.0;
	double health_access_ref = 0.0;
	double dissatisfaction = 0.0;
	double dissatisfaction_by_stratum[kIncomeStrata] = {};
	double dissatisfaction_unemployment = 0.0;
	double dissatisfaction_rent = 0.0;
	double dissatisfaction_food = 0.0;
	double dissatisfaction_inflation = 0.0;
	double dissatisfaction_informality = 0.0;
	// Derivados (NÃO persistidos): satisfação do dia anterior e a marca de
	// informalidade semeada (evita re-semear em re-seed). A região usa a
	// satisfação FRESCA (region_demography_month roda depois do consumo).
	double satisfaction_prev = 1.0;
	double informal_crisis_factor = 1.0;
	bool informality_seeded = false;
	// ------------------------------------------------------------------
	// B6 — PATRIMÔNIO IMOBILIÁRIO DA REGIÃO (save v23), cadência MENSAL (A6):
	// terra produtiva, moradia do interior e ANEL PERIURBANO da cidade-polo.
	// Os valores de ativo NÃO são dinheiro (`money_total` não os inclui): o
	// único fluxo é o aluguel do anel, debitado e creditado DENTRO da região.
	// `property_ready` = os hectares vieram do dado (`land_property.json` + a
	// regra de rateio aplicada no GDScript); falso = região inerte para a B6
	// (save v22 carregado antes de o dado ser reaplicado).
	// `ring_population` é um SUBCONJUNTO de `region_pop` (moradores da
	// periferia da cidade-polo, ainda contados no interior): pessoas nunca são
	// criadas nem somadas duas vezes em `city.population`.
	// ------------------------------------------------------------------
	bool property_ready = false;
	double land_ha = 0.0;                 // hectares agrícolas (dado × rateio)
	double land_gini = 0.0;               // Gini fundiário efetivo (0..1)
	bool land_gini_observed = false;      // veio do dado (não do fallback)
	double land_value_per_ha = 0.0;       // XGS/ha (mercado)
	double land_book_value_per_ha = 0.0;  // base contábil (1ª avaliação)
	double land_income_month = 0.0;       // renda primária do mês (fecho)
	double commodity_index = 1.0;         // preço ÷ preço-base D0 dos primários
	double housing_price = 0.0;           // XGS/unidade (moradia do interior)
	double housing_book_price = 0.0;      // base contábil da moradia
	double housing_rent = 0.0;            // XGS/unidade/mês
	double ring_population = 0.0;         // pessoas no anel (⊆ region_pop)
	double ring_price = 0.0;              // XGS/unidade no anel
	double ring_book_price = 0.0;         // base contábil do anel
	double ring_rent = 0.0;               // XGS/unidade/mês no anel
	double ring_rent_paid_month = 0.0;    // aluguel do anel pago no fecho
	double migration_ring_in_total = 0.0; // excedente assentado no anel (acum.)
	double migration_month_ring_in = 0.0; // idem, último fecho
	double property_tax_rate = 0.0;       // alíquota anual (0 = sem imposto)
	double property_tax_potential = 0.0;  // receita potencial do mês
	// Distribuição por quintil (valor de mercado; NÃO é dinheiro).
	double stratum_land_value[kIncomeStrata] = {};
	double stratum_home_value[kIncomeStrata] = {};
	double stratum_ring_value[kIncomeStrata] = {};
	double stratum_wealth[kIncomeStrata] = {};
};


	// ------------------------------------------------------------------
	// PARTE E / D-116 — ENERGIA ELÉTRICA (save v35). Ver sim/power.{h,cpp}
	// e docs/economy/E2E3_power_grid.md.
	//
	// A rede NÃO nasce nacional para todos: cada nação só tem OPERADOR
	// NACIONAL quando o dado público declara acesso à eletricidade suficiente
	// (regra declarada em power_params.json), e o operador só inclui as
	// cidades CONECTADAS. Cidade tem três estados: conectada (compartilha a
	// rede nacional), parcial (troca até `line_mw`) e isolada (só as usinas
	// da própria cidade/arredor, com preço e racionamento próprios).
	//
	// Dinheiro: `WorldState::power_cash` é o caixa agregado do setor elétrico;
	// consumidor paga ao gerador, combustível sai do mercado, O&M/salários e
	// lucro vão ao dono. Tudo soma zero (conservação intocada).
	// ------------------------------------------------------------------
	enum PowerFuel : int {
		POWER_HYDRO = 0,
		POWER_SOLAR = 1,
		POWER_WIND = 2,
		POWER_COAL = 3,
		POWER_GAS = 4,
		POWER_OIL = 5,
		POWER_NUCLEAR = 6,
		POWER_BIOMASS = 7,
		POWER_OTHER = 8,
		kPowerFuelCount = 9,
	};
	// `solar_month` usa 12 posições (janeiro..dezembro).
	constexpr int kPowerMonthCount = 12;

	enum PowerConnection : int {
		POWER_ISOLATED = 0,
		POWER_PARTIAL = 1,
		POWER_CONNECTED = 2,
	};

	struct PowerCityState {
		// Estado da conexão (persistido): isolada/parcial/conectada e a
		// capacidade da linha em MW (parcial = teto de troca; conectada =
		// capacidade da subestação, 0 = sem limite declarado).
		int connection = POWER_ISOLATED;
		double line_mw = 0.0;
		// `has_data` = a cidade tem parque/potencial declarado no dado. Cidade
		// fora do dado não é simulada como falta (ausência explícita), e sim
		// isenta de racionamento — nunca se inventa um blecaute.
		bool has_data = false;
		// Acesso à eletricidade (%) do dado público aplicado à cidade e a perda
		// T&D inicial da nação (%). A perda EFETIVA do dia = base × infra B5,
		// com piso técnico > 0 (kPowerLossFloorPct).
		double access_pct = 100.0;
		double loss_pct = 10.0;
		// Teto (potencial) por fonte em MW — dado do mapa; 0 = sem potencial
		// declarado (nenhuma usina nova daquela fonte).
		double cap_mw[kPowerFuelCount] = {};
		// Fator de capacidade SOLAR por mês (PVOUT mensal do Global Solar
		// Atlas ÷ 24 h). Vazio/0 = usa o fator anual declarado.
		double solar_month[kPowerMonthCount] = {};
		// Indicadores do dia/rede (persistidos para o status e a duração do
		// blecaute): demanda/servido em MWh, preço da rede (XGS/MWh), dias
		// seguidos de racionamento e PIB perdido ESTIMADO (VOLL declarado).
		double demand_mwh = 0.0;
		double served_mwh = 0.0;
		double price = 0.0;
		int ration_days = 0;
		double lost_gdp = 0.0;
	};

	struct PowerPlant {
		std::string id;
		int city_index = -1;
		int fuel = POWER_OTHER;
		double mw = 0.0;
		// Dono: vazio = Estado (nação do controlador da cidade); senão id de
		// grupo da F9. `owner_nation` é a nação que recebe o lucro estatal.
		std::string owner_group;
		std::string owner_nation;
		// Economia da usina (XGS): capital de obra, O&M por MWh, empregos e
		// salário por emprego/dia.
		double capex = 0.0;
		double opex_per_mwh = 0.0;
		double jobs = 0.0;
		double wage = 0.0;
		int built_day = 0;
		bool initial = false;
		// Geração do dia (derivada do despacho; NÃO persistida).
		double generated_mwh = 0.0;
	};

	struct PowerGridState {
		std::string nation;
		bool exists = false;
		double price = 0.0;
		double demand_mwh = 0.0;
		double served_mwh = 0.0;
		int ration_days = 0;
		double lost_gdp = 0.0;
		int connected = 0;
		int partial = 0;
		int isolated = 0;
		// Cooldown da decisão de obra da IA (dia do último projeto aberto).
		int last_build_day = -1000000;
		double generation_by_fuel[kPowerFuelCount] = {};
	};

	struct City {
		std::string id;
		std::string name;
		std::string nation;
		std::string home_nation;  // região natural (território que controla)
		std::string landmass;     // massa de terra contínua (rota terrestre)
		// A2: índices derivados de `nation`/`landmass` na tabela de nomes do
		// WorldState (índice = posição lexicográfica; ver NameTable). NÃO são
		// serializados: rebuild_indexes(true) os preenche no carregamento. O
		// caminho quente compara ints em vez de strings.
		int nation_index = -1;
		int landmass_index = -1;
		MapPoint center{0.0, 0.0};
		// `population` é a população REAL exibida (2026, nação inteira rateada
		// entre as cidades). `simulation_population` é a aglomeração da fonte,
		// a unidade de simulação: TODO o cálculo econômico usa esta, para o
		// balanço (comida, caixa, empregos, preços) continuar calibrado. Campo
		// de save; cenário antigo/sintético sem ele usa `population`.
		double population = 0.0;
		double simulation_population = 0.0;
		double firm_cash = 0.0;
		double service_cash = 0.0;
		// C1 (save v26) — PASSIVOS das firmas da cidade, por origem do atraso:
		// folha do setor de serviços e folha rural (paga pelo caixa da firma da
		// cidade-polo). O atraso por planta fica em `Building::wage_arrears`.
		double service_wage_arrears = 0.0;
		double rural_wage_arrears = 0.0;
		double government_budget = 0.0;
		// Multiplicador da renda externa (PIB PPP per capita da nação,
		// normalizado pela média mundial ponderada = 1,0). Ver
		// set_external_income_index e §20 do balance_review.
		double external_income_index = 1.0;
		// F7 calibração: produtividade nacional (TFP). 0 = DERIVAR do índice de
		// renda real (`clamp(external_income_index, 0,25, 4,0)`, o PIB PPP per
		// capita já carregado, `source: derived:gdp_ppp`); > 0 = fator
		// explícito do laboratório. Multiplica o valor adicionado por
		// trabalhador na âncora salarial (update_wages); não move preços
		// nem cria dinheiro (o pagamento segue limitado ao caixa da firma).
		double productivity_index = 0.0;
		// F7 calibração: Gini de renda da nação (0..1). 0 = NÃO DECLARADO
		// (cidade sintética/save antigo) -> a repartição de consumo usa os
		// estratos de escolaridade (comportamento anterior). Quando declarado
		// (GeographyService, `estimated` quando vem da mediana regional da
		// distribuição), a POP é dividida em classes por uma lognormal de média
		// ancorada no PIB PPP per capita e sigma derivado do Gini.
		double income_gini = 0.0;
		double service_jobs = 0.0;
		// F8: vagas de serviço efetivamente preenchidas (ver labor_day).
		double service_workers = 0.0;
		// F9: vagas de serviço PAGAS (preenchidas pelo setor simulado). O
		// emprego estrutural de serviços vai em `sector_workers[2]` e
		// `service_workers`; o salário do setor continua saindo daqui.
		double service_jobs_filled = 0.0;
		// F9 — emprego setorial (docs/economy/sector_employment_report.md).
		// `sector_workers`: 0 agropecuária, 1 indústria/mineração, 2 serviços
		// (residual), 3 informal (SOBREPOSIÇÃO: parte dos três primeiros; não
		// soma com eles). `sector_share` é a fração do emprego (0..1).
		// Derivado todo dia em update_sector_employment; não é campo de
		// save (recalculado no primeiro tick após carregar).
		double sector_workers[4] = {};
		double sector_share[4] = {};
		// Produção agrícola declarada no dia (t/dia de alimentos primários) e
		// autoconsumo em espécie das famílias agrícolas (t-eq/dia) — ver o
		// piso de subsistência em tick_core.
		double agri_output_tons = 0.0;
		double in_kind_food = 0.0;
		double service_wage = 0.0;
		double service_wage_bill = 0.0;
		double service_revenue = 0.0;
		double service_profit = 0.0;
		// Receitas de serviços empresariais (opex), frete e construção.
		double opex_revenue = 0.0;
		double freight_revenue = 0.0;
		double construction_revenue = 0.0;
		// Investimento/construção pedido entre ticks entra no PIB do próximo
		// dia, em vez de ser zerado pelo reset de fluxos antes de ser contado.
		double construction_revenue_pending = 0.0;
		double capital_income_next = 0.0;
		// B2: aluguel recebido pelas famílias donas no dia (fase de moradia),
		// pago junto do capital no dia seguinte. Fica separado para o relatório
		// por estrato; entra em `money_total` como o capital.
		double rent_income_next = 0.0;
		double gdp = 0.0;
		// F3: compra institucional do Estado no dia (paper/computer).
		double government_goods_spend = 0.0;
		// F5: última cotação de importação (preço entregue na moeda local) por
		// produto; forma o preço local em cidade sem produtor (§2.7).
		double import_price[kMaxProducts] = {};
		// F5: tarifa arrecadada no dia (receita do governo do comprador).
		double tariff_revenue = 0.0;
		double revenue_by_product[kMaxProducts] = {};
		// Fluxos comerciais do dia, por produto.
		double imported[kMaxProducts] = {};
		double exported[kMaxProducts] = {};
		double import_value[kMaxProducts] = {};
		double export_value[kMaxProducts] = {};
		// Aptidão territorial por produto (0..1). A produção primária é
		// multiplicada por ela; o GeographyService sobrescreve com dados reais.
		// Inicializador: trigo/ferro/arroz = 1.0, os demais = 0.0 (default).
		double suitability[kMaxProducts] = {1.0, 1.0, 1.0};
		// Teto de demanda externa (t/dia) por produto, declarado pelo
		// GeographyService (participação da nação no mercado mundial). 0 = sem
		// exportação deste bem nesta cidade.
		double external_demand[kMaxProducts] = {};
		// Teto físico de capacidade agrícola (t/dia) por produto, derivado da
		// geografia (área cultivável × produtividade FAO). NÃO entra no save:
		// chega por `set_land_capacity` (GeographyService) e viaja no bloco
		// `definitions` do snapshot.
		double land_capacity[kMaxProducts] = {};
		// ------------------------------------------------------------------
		// G — AGRICULTURA SAZONAL (save v24). A DEFINIÇÃO da safra (área,
		// rendimento, janelas) mora em `WorldState::crop_plans`, porque é
		// esparsa (poucas entradas por cidade) e estática; aqui ficam o hook de
		// qualidade do armazém e o livro de perdas do mês.
		// ------------------------------------------------------------------
		// Qualidade do armazém 0..1 (`set_storage_quality`). < 0 = DERIVAR da
		// infra B5 do estado (ver `SimCore::city_storage_quality`).
		double storage_quality = -1.0;
		// Perdas do mês por bem da CADEIA (posição em `ws_.agri_chain`):
		// perecibilidade (prazo de prateleira) e perda de armazenagem mensal.
		// Diagnóstico (não entra no save; recalculado a cada mês).
		double agri_loss_perishable[kMaxAgriChain] = {};
		double agri_loss_storage[kMaxAgriChain] = {};
		// G — consumo EM ESPÉCIE das famílias agrícolas por slot da cesta
		// (t/dia). Sai do estoque do mercado SEM entrar em `market.demand`
		// (consumption.cpp); é o que fecha a conta da entressafra. Diagnóstico.
		double agri_in_kind[kMaxFoodProducts] = {};
		// G — toneladas que CHEGARAM fisicamente ao estoque neste dia (o
		// `imported` do mercado conta a compra e é zerado logo depois da
		// chegada). Só para a conta da entressafra; zerado no phase_arrival.
		double agri_arrived[kMaxProducts] = {};
		// G — produção diária EQUIVALENTE da safra (t/dia): Σ área × rendimento
		// ÷ 365 dos planos da cidade. Derivada (`rebuild_crop_indexes`); é o
		// teto do autoconsumo das famílias agrícolas quando há plano sazonal —
		// a fazenda não produz pela capacidade nominal, e o autoconsumo não pode
		// comer um estoque que a lavoura não repõe.
		double agri_seasonal_daily = 0.0;
		// Reservas minerais por produto (extração com esgotamento).
		Deposit deposits[kMaxProducts];
		Pop pop;
		// B3: região (estados inteiros) — bloco simplificado do interior.
		Region region;
		Market markets[kMaxProducts];
		// F8: mercados de serviço por setor (ordem de services.json).
		ServiceMarket service_markets[kMaxServiceSectors];
		double service_export_revenue = 0.0;
		double service_government_revenue = 0.0;
		double service_input_cost = 0.0;
		double service_business_profit = 0.0;
		int service_business_count = 0;
		double private_health_cover = 0.0;
		double private_education_cover = 0.0;
		// Tonelagem movimentada no dia anterior (base do B2B de logística).
		double service_tonnage_prev = 0.0;
		// PARTE E / D-116 — derivados do dia (NÃO persistidos; recalculados a
		// cada `phase_power`). `power_industry_factor` limita a produção e o
		// emprego industriais; `power_service_factor`, os serviços;
		// `power_stress` (0..1) entra na insatisfação B7 com peso declarado;
		// `power_cost_industry` é a conta de energia da indústria do dia
		// (XGS), usada no piso de custo da formação de preço.
		double power_price = 0.0;
		double power_served_ratio = 1.0;
		double power_industry_factor = 1.0;
		double power_service_factor = 1.0;
		double power_cost_industry = 0.0;
		double power_cost_per_value = 0.0;
		double power_stress = 0.0;
	};

	// Fases medidas por set_profiling (ordem = ordem do tick).
	enum Phase {
		kPhaseArrival = 0, kPhaseReset, kPhaseDemography, kPhaseLabor,
		kPhaseProduction, kPhaseRaw, kPhaseIncome,
		kPhaseServices, kPhaseTrade,
		kPhaseAccounting, kPhasePrices, kPhaseClamp,
		kPhaseFiscal, kPhaseFx, kPhaseInvariants, kPhaseStatus,
		// Sub-fases do comércio (perfil fino do tick). Medem só dentro da fase
		// `trade`: o custo total de comida/bens e o laço quente (compradores /
		// produto×comprador). O "prep" (índices/grade) sai por subtração.
		kPhaseTradeFood, kPhaseTradeFoodBuyers,
		kPhaseTradeGoods, kPhaseTradeGoodsLoop,
		// Zonas de perfil finas (A2): recortam trechos que hoje caem agregados
		// numa fase só (prólogo, trânsito de chegada e fiscal). Só medição —
		// nenhum efeito no jogo.
		kPhasePrologueSnapshot, kPhasePrologueIndexes, kPhasePrologueOther,
		kPhaseArrivalTransit,
		kPhaseFiscalCityGov, kPhaseFiscalEqualize, kPhaseFiscalGroups,
		// Zonas finas do comércio (A7): preparo, cálculo (paralelo) e liquidação
		// (serial) do comércio de alimento, mais candidatos/ofertas/liquidação
		// dentro do laço de bens. Só medição — nenhum efeito no jogo.
		kPhaseTradeFoodPrep, kPhaseTradeFoodCalc, kPhaseTradeFoodSettle,
		kPhaseTradeGoodsCand, kPhaseTradeGoodsOffers, kPhaseTradeGoodsSettle,
		// Zonas de perfil da A6 (cadência): custo dos sistemas candidatos a sair
		// do diário (moradia, solo, prefeitura, decisões de serviço) e do
		// retrato/fase da IA. Só medição — nenhum efeito no jogo.
		kPhaseHousing, kPhaseLandMonth, kPhaseFleetDay, kPhaseServicesDecisions,
		kPhaseAi, kPhaseAiView,
		// B3: economia da região (estados inteiros) — consumo diário, salário
		// semanal e demografia mensal do interior.
		kPhaseRegion,
		// B4: migração (êxodo rural + cidade → cidade na mesma nação).
		kPhaseMigration,
		// B7: insatisfação (medição pura, sem dinheiro/pessoas) no fim do dia.
		kPhaseSocial,
		// B6: patrimônio da região (terra/moradia/anel), cadência mensal.
		kPhaseProperty,
		// G: agricultura sazonal (colheita na janela, plantio/fertilizante,
		// perdas de armazenagem e perecibilidade). Roda depois do reset e antes
		// da produção — a safra entra no estoque do dia.
		kPhaseAgriculture,
		// G2: armazenagem (colheita desviada para o silo da região, liberação
		// gradual ao mercado da cidade-polo pagando acesso, estoque guardado do
		// governo/privado, perdas e manutenção). Roda logo depois da agricultura
		// e antes da produção/comércio.
		kPhaseStorage,
		// C5 (D-112): bolsa de valores (confiança, preço semanal e dividendo
		// mensal). Roda depois do crédito e antes dos invariantes — o retrato do
		// tick e a reversão precisam ver o preço/confiança finais.
		kPhaseStock,
		// C6 (D-114): crise bancaria (confianca, corrida, redesconto,
		// aperto e resolucao). Roda depois da bolsa e antes dos invariantes.
		kPhaseBankCrisis,
		// PARTE E (D-116): despacho por merito, preco da rede e racionamento.
		// Roda ANTES do trabalho/producao (o fator de servico limita o emprego
		// e a producao do dia). A liquidacao financeira roda depois da
		// contabilidade, quando as firmas ja receberam a receita do dia.
		kPhasePower,
		kPhasePowerSettle,
		kPhaseCount
	};

	struct Shipment {
		std::string to_city;
		int product = 0;
		double amount = 0.0;
		int arrival_day = 0;
		// F4: frota reservada pelo pedido (liberada na chegada). O combustível
		// já foi debitado no embarque; fica registrado para auditoria/save.
		std::string fleet_nation;
		double fleet_trucks = 0.0;
		double fleet_ships = 0.0;
		double fleet_planes = 0.0;
		double fuel = 0.0;
	};

	// C2b1 (D-104) — estado da POLÍTICA MONETÁRIA de uma moeda: taxa básica
	// (anual; < 0 = nenhuma decisão ainda, resolve no neutro do jogo), M0
	// criado pelo BC por operação contabilizada, carteira de títulos e a
	// leitura (inflação/hiato) que a regra usou. `emission_today` alimenta o
	// invariante de conservação do dia.
	// C2b2 (D-106) - ESTADO CAMBIAL por moeda (save v29). A paridade e a
	// largura da banda salvas mandam; 0/-1 significam "o dado do F5 / o default
	// de jogo" (nunca um numero inventado). `reserves` e a moeda de reserva em
	// unidades de ancora; `funded` falso = fixo/banda ADMINISTRATIVO sem reserva
	// declarada (o F5 ja segurava a taxa sem custo) - a defesa medida so existe
	// com reserva.
	struct FxState {
		double parity = 0.0;
		double band_width = -1.0;
		double reserves = 0.0;
		double conversion_price = 0.0;
		bool funded = false;
		int devaluations = 0;
		int last_event_day = -1;
		// Medicao do dia (nao serializada).
		double intervention_today = 0.0;
		// C2c (D-108) - CESTA resolvida (moeda -> peso, soma 1). Vazia = cesta
		// declarada sem composicao (alvo nao calculado) ou ancora em moeda unica.
		// `basket_source` diz de onde os pesos vieram (nunca fica implicito).
		std::map<std::string, double> basket;
		std::string basket_source;
	};

	// C2c (D-108) - REGIME REAL por moeda (IMF AREAER 2026) e a regra de quem
	// pode muda-lo. Vem de data/definitions/economy/fx_regimes_2026.json pelo
	// carregador GDScript (`set_fx_regimes`); o status publica a fonte.
	struct FxRegimeInfo {
		int regime = 1;                  // central_bank::ExchangeRegime
		std::string anchor;              // moeda-ancora ("" = sem ancora)
		std::string anchor_type = "none";// none | currency | composite
		std::map<std::string, double> basket;  // pesos PUBLICADOS (raro)
		std::string anchor_source;       // onde a ancora esta declarada
		std::string issuer;              // nacao emissora ("" = sem emissor unico)
		bool locked = false;             // moeda de uniao (regra de bloco futura)
		std::string source;              // fonte do regime (AREAER / declaracao)
	};

	// C2b2 (D-106) - evento cambial do status (anel de 64; o contador total de
	// desvalorizacoes e a fonte de "quantas").
	struct FxEvent {
		int day = -1;
		std::string currency;
		std::string type;  // devaluation | reserve_purchase | gold_conversion
		double old_parity = 0.0;
		double new_parity = 0.0;
		double reserves_used = 0.0;
	};

	struct MonetaryState {
		double rate = -1.0;
		double emission = 0.0;
		double securities = 0.0;
		double emission_today = 0.0;
		double inflation_used = 0.0;
		bool has_inflation = false;
		double gap = 0.0;  // hiato de desemprego em pontos percentuais
		int last_day = -1;
		// C2b1 — medição do dia (não vai ao save): taxa que a regra queria e o
		// déficit que o BC viu/ deixou de financiar pelo teto de senhoriagem.
		double target_rate = 0.0;
		double deficit_measured = 0.0;
		double unfunded = 0.0;
	};

	// Retrato do estado demo para reverter um tick que viole um invariante.
	struct Snapshot {
		std::vector<City> cities;
		std::vector<Building> buildings;
		std::vector<ConstructionProject> construction_projects;
		std::vector<ServiceBusiness> service_businesses;
		int next_service_business_id = 1;
		int service_bankruptcies = 0;
		std::vector<Shipment> transit;
		std::unordered_map<std::string, Fleet> fleets;
		// F9: estado dos grupos (caixa + lucro por anfitrião), confiança das
		// nações e fluxos acumulados — desfazer um tick corrompido também
		// desfaz a distribuição de lucro e a repatriação.
		std::vector<Group> groups;
		std::unordered_map<std::string, double> confidence;
		std::unordered_map<std::string, GroupFlow> group_flows;
		std::vector<Currency> currencies;
		// §19 (W2): solo e prefeitura. A prefeitura move área/capacidade do solo
		// (obra concluída) e o próprio caixa; desfazer o tick desfaz isso.
		std::vector<CityLand> land;
		std::vector<CityGov> city_gov;
		// G2: armazéns agregados (reserva do governo + estoque privado). A
		// reversão de um tick corrompido também desfaz compra/liberação/colheita.
		std::vector<StorageSite> storage_sites;
		// C2b1 (D-104): política monetária e emissão. Reverter um tick tem de
		// desfazer o M0 criado e a dívida pública que nasceu dele, e devolver a
		// janela do déficit (`gov_cash_open` é medição de runtime, mas tem de
		// voltar com o mundo reverted).
		std::map<std::string, MonetaryState> monetary;
		std::map<std::string, double> gov_bonds;
		// C6 (D-114): títulos do resgate sem BC em poder do exterior. Reverter
		// um tick tem de desfazer o título junto com o caixa que o pagou.
		std::map<std::string, double> external_bonds;
		std::map<std::string, double> gov_cash_open;
		// C2b2 (D-106): reservas/paridade/eventos. Reverter um tick tem de
		// desfazer a intervenção e a desvalorização do dia junto com o caixa.
		std::map<std::string, FxState> fx;
		std::vector<FxEvent> fx_events;
		// C3 (D-107): desfazer um tick tem de desfazer o empréstimo, o juro, o
		// pagamento e o calote junto com o caixa que eles moveram.
		std::map<std::string, Bank> banks;
		// C6 (D-114): fundo de seguro e eventos do dia. Reverter um tick tem de
		// desfazer premio/pagamento do fundo e os eventos registrados.
		std::map<std::string, DepositInsurance> deposit_insurance;
		std::vector<BankCrisisEvent> bank_crisis_events;
		int bank_crisis_events_total = 0;
		// C5 (D-112): bolsa por nação e papéis listados. A valorização não move
		// caixa; o dividendo move (soma zero). Reverter um tick corrompido tem
		// de desfazer preço, confiança e dividendo.
		std::map<std::string, StockExchange> stock_exchanges;
		std::map<std::string, StockTicker> stock_tickers;
		// PARTE E (D-116): parque, rede, racionamento e caixa do setor elétrico.
		// A liquidação move caixa de consumidores, geradores e governo; reverter
		// um tick tem de desfazer tudo isso junto com o estado da rede.
		std::vector<PowerCityState> power_cities;
		std::vector<PowerPlant> power_plants;
		std::map<std::string, PowerGridState> power_grids;
		double power_cash = 0.0;
		double emission_today = 0.0;
		double transport_cash = 0.0;
		double foreign_cash = 0.0;
		double construction_cash = 0.0;
		double produced[kMaxProducts] = {};
		double consumed_total[kMaxProducts] = {};
		double produced_today[kMaxProducts] = {};
		double consumed_today[kMaxProducts] = {};
		int day = 0;
	};

} // namespace ogs
