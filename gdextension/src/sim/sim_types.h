#pragma once
// Fragmento de tipos POD + enums + constantes do núcleo econômico (S1 do
// docs/economy/core_refactor_plan.md). Sem dependência do engine Godot: só STL.
// É incluído DENTRO da classe EconomySim (economy_sim.h), no bloco private — os
// tipos continuam aninhados (EconomySim::DemoCity etc.), nada de namespace novo.

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

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
		// Obra de expansão em andamento (API demo_expand_port).
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
	// de generalizar o modo europeu.
	// ------------------------------------------------------------------
	enum class DemoProduct {
		Wheat = 0, Iron = 1, Rice = 2, Maize = 3,
		Bauxite = 4, Lead = 5, Gold = 6, Phosphate = 7, Tungsten = 8,
		RareEarths = 9, Sulfur = 10, Diamond = 11, Coal = 12, Copper = 13,
		Gas = 14, Oil = 15, Potato = 16, Soy = 17, Cotton = 18, Wood = 19,
		Uranium = 20
	};
	// Mesma ordem de data/definitions/economy/goods.json. A contagem é variável
	// (demo_set_goods): os 21 primários continuam nas posições 0..20 e os 92
	// bens industriais entram em 21..112. kDemoMaxProducts é só o teto de
	// segurança do vetor.
	static constexpr int kDemoMaxProducts = 160;
	static constexpr int kDemoMaxRecipeInputs = 6;
	// F3: níveis de necessidade (basic/life/luxury, needs.json).
	static constexpr int kDemoMaxNeedsLevels = 3;
	// F7 calibração: níveis de consumo alcançáveis pela renda de um estrato
	// (0 = nenhum, 1 = basic, 2 = life, 3 = luxury). É a fatia da POP por nível
	// publicada no status — por isso tem uma posição a mais que a cesta.
	static constexpr int kDemoIncomeLevels = kDemoMaxNeedsLevels + 1;
	// F7: níveis de educação (básico, intermediário, superior). Não é dado de
	// catálogo: são três faixas de escolaridade da população em idade ativa.
	static constexpr int kDemoEducationLevels = 3;
	// F8: setores de serviço (services.json). O teto é fixo para o estado por
	// cidade caber em arrays planos; os dados declaram 20 e o loader recusa
	// arquivo com contagem diferente (a publicação/UI indexa por posição).
	static constexpr int kDemoMaxServiceSectors = 20;
	// Campos publicados por setor na cidade (ver demo_service_publish_fields em
	// economy_sim.cpp): demanda, oferta, preço, receita, emprego, negócios,
	// demanda não atendida, exportação (unidades), exportação (receita),
	// cobertura privada, custo de insumos e custo de serviços.
	static constexpr int kDemoServiceFields = 12;
	// Limite de negócios por cidade/setor e de aberturas por cidade/dia.
	static constexpr int kDemoServiceMaxPerSector = 4;
	static constexpr int kDemoServiceOpensPerDay = 1;
	// Cesta alimentar (B3/B4) agora data-driven (demo_set_needs), derivada de
	// needs.json (basic/food/substitutes + equivalence.calories). O padrão
	// preserva a cesta validada de 5 culturas até o autoload carregar o dado.
	// A necessidade é medida em toneladas-equivalentes de grão: o fator calórico
	// converte tonelada real em tonelada-equivalente (batata ~1/4 das calorias).
	static constexpr int kDemoMaxFoodProducts = 16;
	static constexpr int kDemoFoodProductIds[5] = {0, 2, 3, 16, 17};
	static constexpr double kDemoCaloricFactor[5] = {1.0, 1.0, 1.0, 0.25, 1.3};

	// Os 8 campos PUBLICADOS no status ficam primeiro e em ordem fixa: o cache
	// de publicação compara o bloco inteiro com um memcmp de 64 bytes (um
	// único teste em vez de 8 comparações de double por produto/cidade).
	// `sold_prev` é interno (muda todo dia) e fica no fim, fora da comparação.
	struct DemoMarket {
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
	};
	static_assert(offsetof(DemoMarket, stock) == 0, "DemoMarket: stock primeiro");
	static_assert(offsetof(DemoMarket, surplus) == 7 * sizeof(double),
		"DemoMarket: os 8 campos publicados precisam ser contíguos");

	// F3: uma categoria de necessidade (needs.json → levels.<nível>.categories).
	// `substitutes` mantém a ordem declarada (do melhor ao último caso) e
	// `equivalence` converte tonelada real em tonelada-equivalente da categoria.
	struct DemoNeedsCategory {
		std::string id;
		double per_capita_day = 0.0;  // per_capita_t_month / 30
		std::vector<int> substitutes;
		std::vector<double> equivalence;
	};

	// F3: um nível (basic/life/luxury) com peso na satisfação agregada.
	struct DemoNeedsLevel {
		std::string id;
		int priority = 1;
		double weight = 0.0;
		std::vector<DemoNeedsCategory> categories;
	};

	struct DemoRecipe {
		std::string id;
		int output = -1;
		double yield = 1.0;
		int input_count = 0;
		int inputs[kDemoMaxRecipeInputs] = {};
		double input_qty[kDemoMaxRecipeInputs] = {};
		int byproduct_count = 0;
		int byproducts[kDemoMaxRecipeInputs] = {};
		double byproduct_qty[kDemoMaxRecipeInputs] = {};
		// Engenharia do processo (data/definitions/economy/recipes.json).
		double capital_per_capacity = 2000.0;
		double jobs_per_ton = 7.5;
		double wage = 2.0;
		double opex_per_ton = 2.5;
		// F7: mistura de escolaridade das vagas da receita (básico,
		// intermediário, superior), soma 1. Lida de `education_mix` no
		// recipes.json quando declarada; senão derivada do `sector` (tabela
		// documentada em demo_set_recipes).
		double education_mix[kDemoEducationLevels] = {0.60, 0.30, 0.10};
	};

	struct DemoBuilding {
		std::string id;
		std::string city;
		int kind = 0; // 0=fazenda (único tipo hoje; campo mantido por exibição)
		std::string resource;  // recurso produzido (id de goods.json)
		std::string owner = "private"; // private ou state
		int input_product = -1; // -1 = extração primária (sem insumo)
		int output_product = 0;
		// Receita industrial (-1 = produtor primário com input_product/ratio).
		// A receita completa (até 6 insumos + subprodutos) vive em demo_recipes_.
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
		int loss_days = 0;
		bool bankrupt = false; // fechada por insolvência (prejuízo persistente)
		bool depleted = false; // jazida esgotada: encerra sem contar falência
		// Planta estratégica semeada no bootstrap industrial (decisão §12.3):
		// não fecha por insolvência enquanto não há demanda final (F3); o
		// governo cobre o prejuízo. Marcada explicitamente para o save/status.
		bool strategic = false;
		// F9: participação do parceiro local em joint venture. `owner` guarda
		// o grupo estrangeiro; `partner_group` o grupo nacional que divide o
		// lucro na fração `local_share`. Fora de JV, local_share = 0.
		double local_share = 0.0;
		std::string partner_group;
		double revenue = 0.0;
		double input_value = 0.0;
		double costs = 0.0;
		double profit = 0.0;
		// Valores efetivamente pagos no dia (podem ser menores que o contrato
		// quando o caixa da empresa não cobre a folha e o opex).
		double paid_wages = 0.0;
		double paid_opex = 0.0;
		// F7: mistura de escolaridade das vagas da construção (soma 1). Fazenda
		// e extração quase só básico; indústria segue a receita/setor.
		double education_mix[kDemoEducationLevels] = {0.90, 0.09, 0.01};
	};

	struct DemoPop {
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
		double food_spend = 0.0;
		double consumed = 0.0;
		double need = 0.0;
		double satisfaction = 0.0;
		double savings = 0.0;
		// Consumo por categoria/slot da cesta alimentar (data-driven).
		std::vector<double> desired_by_category;
		std::vector<double> consumed_by_category;
		// F3: necessidades em três níveis. `level_*` têm kDemoMaxNeedsLevels
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
		double education[kDemoEducationLevels] = {};
		double participation[kDemoEducationLevels] = {};
		double labor_force_by_level[kDemoEducationLevels] = {};
		double employed_by_level[kDemoEducationLevels] = {};
		double unemployed_by_level[kDemoEducationLevels] = {};
		double vacancies_by_level[kDemoEducationLevels] = {};
		// Vagas que sobraram SEM candidato após a alocação em cascata (nem
		// sobrequalificados). É a escassez real do nível; o desemprego do mesmo
		// nível é a sobra. juntos, definem o aperto que move o salário.
		double unfilled_by_level[kDemoEducationLevels] = {};
		double wage_by_level[kDemoEducationLevels] = {};
		double wage_income_by_level[kDemoEducationLevels] = {};
		// `true` depois do primeiro ajuste ancorado no valor adicionado: a
		// âncora inicial de criação do mundo é um palpite, não um salário
		// histórico que precise de inércia.
		bool wage_anchored = false;
		// Em formação para o nível 1 (intermediário) e 2 (superior). A
		// conclusão é gradual (coorte exponencial com duração em dias).
		double studying[kDemoEducationLevels - 1] = {};
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
		double filled_by_slot[kDemoEducationLevels] = {};
		// ------------------------------------------------------------------
		// F7 calibração — RENDA POR ESTRATO. A POP é dividida em estratos de
		// renda alinhados ao nível de educação/ocupação (básico, intermediário,
		// superior). Cada estrato tem a sua própria renda e o seu próprio nível
		// de consumo (basic/life/luxury), então um país tem fatias em níveis
		// diferentes em vez de um único nível para a cidade inteira. O lucro
		// (renda de capital) vai só para os DONOS — o estrato superior —, não
		// é dividido igualmente entre toda a POP.
		// ------------------------------------------------------------------
		double stratum_population[kDemoEducationLevels] = {};
		double stratum_wage_income[kDemoEducationLevels] = {};
		double stratum_external_income[kDemoEducationLevels] = {};
		double stratum_capital_income[kDemoEducationLevels] = {};
		double stratum_disposable[kDemoEducationLevels] = {};
		int stratum_income_level[kDemoEducationLevels] = {};
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
		double class_population[kDemoEducationLevels] = {};
		int class_income_level[kDemoEducationLevels] = {};
		// Fração da POP por nível de consumo alcançado (índice 0 = nenhum,
		// 1 = basic, 2 = life, 3 = luxury). É o que o status/report publica.
		double income_level_share[kDemoIncomeLevels] = {};
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
	// de negócio transfere capital da poupança da POP dona para `service_cash`
	// e a saída devolve o residual — `demo_money_total` continua conservado.
	// ------------------------------------------------------------------
	struct DemoServiceSector {
		std::string id;
		std::string name_pt;
		std::string name_en;
		std::string name_es;
		std::string icon;
		double price = 1.0;
		double pop_share[kDemoMaxNeedsLevels] = {};
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
		double education_mix[kDemoEducationLevels] = {0.70, 0.25, 0.05};
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
	struct DemoServiceMarket {
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
		double pop_demand[kDemoMaxNeedsLevels] = {};
	};

	struct DemoServiceBusiness {
		int id = 0;
		int sector = -1;
		std::string city;
		int owner_stratum = kDemoEducationLevels - 1;
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

	// Reserva mineral do território da cidade para um produto. Compartilhada
	// pelas minas da cidade: cada tonelada extraída reduz remaining_kt. Sem
	// tracked, o produto não tem reserva declarada e a extração é ilimitada
	// (comportamento legado, p.ex. ferro sem dados de depósito).
	struct DemoDeposit {
		bool tracked = false;
		double initial_kt = 0.0;
		double remaining_kt = 0.0;
		double quality = 1.0;    // 0..1: eficiência da extração
		double depth_m = 0.0;    // profundidade média: encarece o opex
		double extracted_kt = 0.0;
	};

	// F4: frota nacional de frete. Os veículos são um estoque físico (1 t do
	// bem `truck`/`cargo_ship`/`cargo_plane` = 1 veículo — a produção da F2
	// entra no estoque) e o combustível é consumido por t·km de cada trecho.
	// `health` (0,5..1,0) reduz a capacidade quando faltam `spare_parts` na
	// manutenção mensal.
	struct DemoFleet {
		double trucks = 0.0;
		double ships = 0.0;
		double planes = 0.0;
		// Veículos ocupados por cargas em trânsito (liberados na chegada).
		double trucks_busy = 0.0;
		double ships_busy = 0.0;
		double planes_busy = 0.0;
		// Alvo derivado de população/PIB (demo_init_fleets), usado pela
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
	// da banda [kDemoFxBandLow, kDemoFxBandHigh] × taxa inicial.
	struct DemoCurrency {
		std::string code;
		std::vector<std::string> nations;
		double rate = 1.0;
		double base_rate = 1.0;
		bool floating = true;
		bool fallback = false;
		std::string source = "default";
		double balance_ema = 0.0;
		double gross_ema = 0.0;
	};

	// F5: tarifa declarada (buyer, seller, product, ad valorem). O lookup usa
	// uma chave textual buyer|seller|product; a ordem do vetor é determinística
	// para o status.
	struct DemoTariff {
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
	struct DemoGroup {
		std::string id;
		std::string name;
		std::string origin_nation;
		std::string sector;
		double cash = 0.0;
		// Lucro acumulado ainda não distribuído. Separado do caixa para que uma
		// capitalização (demo_fund_group) não vire dividendo no dia seguinte.
		double distributable = 0.0;
		double revenue_total = 0.0;
		double profit_total = 0.0;
		double dividends_total = 0.0;
		double taxes_total = 0.0;
		double capex_total = 0.0;
		bool state_owned = false;
		std::map<std::string, double> profit_by_host;
	};

	// F9: abertura ao capital estrangeiro por (nação, setor).
	struct DemoFdiPolicy {
		std::string nation;
		std::string sector;
		int mode = 0;  // 0 open, 1 restricted, 2 joint_venture, 3 closed
		double min_local = 0.0;
	};

	// F9: incentivo setorial (isenção/subsídio).
	struct DemoSectorIncentive {
		std::string nation;
		std::string sector;
		double tax_holiday = 0.0;
		double subsidy = 0.0;
	};

	// F9: fluxo acumulado de lucro por nação (balança/câmbio futuros).
	struct DemoGroupFlow {
		double repatriated_out = 0.0;  // lucro de estrangeiro que saiu do anfitrião
		double repatriated_in = 0.0;   // lucro recebido pela origem
		double dividends = 0.0;        // dividendos pagos aos donos nacionais
		double taxes = 0.0;            // imposto corporativo recebido
		double subsidy = 0.0;          // incentivo pago ao grupo
	};

	// Necessidade de transporte de um pedido (por trecho): veículos e fuel.
	struct DemoTransportDemand {
		double trucks = 0.0;
		double ships = 0.0;
		double planes = 0.0;
		double fuel = 0.0;
		double distance_truck_km = 0.0;
		double distance_ship_km = 0.0;
	};


	struct DemoCity {
		std::string id;
		std::string name;
		std::string nation;
		std::string home_nation;  // região natural (território que controla)
		std::string landmass;     // massa de terra contínua (rota terrestre)
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
		double government_budget = 0.0;
		// Multiplicador da renda externa (PIB PPP per capita da nação,
		// normalizado pela média mundial ponderada = 1,0). Ver
		// demo_set_external_income_index e §20 do balance_review.
		double external_income_index = 1.0;
		// F7 calibração: produtividade nacional (TFP). 0 = DERIVAR do índice de
		// renda real (`clamp(external_income_index, 0,25, 4,0)`, o PIB PPP per
		// capita já carregado, `source: derived:gdp_ppp`); > 0 = fator
		// explícito do laboratório. Multiplica o valor adicionado por
		// trabalhador na âncora salarial (demo_update_wages); não move preços
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
		// F8: vagas de serviço efetivamente preenchidas (ver demo_labor_day).
		double service_workers = 0.0;
		// F9: vagas de serviço PAGAS (preenchidas pelo setor simulado). O
		// emprego estrutural de serviços vai em `sector_workers[2]` e
		// `service_workers`; o salário do setor continua saindo daqui.
		double service_jobs_filled = 0.0;
		// F9 — emprego setorial (docs/economy/sector_employment_report.md).
		// `sector_workers`: 0 agropecuária, 1 indústria/mineração, 2 serviços
		// (residual), 3 informal (SOBREPOSIÇÃO: parte dos três primeiros; não
		// soma com eles). `sector_share` é a fração do emprego (0..1).
		// Derivado todo dia em demo_update_sector_employment; não é campo de
		// save (recalculado no primeiro tick após carregar).
		double sector_workers[4] = {};
		double sector_share[4] = {};
		// Produção agrícola declarada no dia (t/dia de alimentos primários) e
		// autoconsumo em espécie das famílias agrícolas (t-eq/dia) — ver o
		// piso de subsistência em demo_tick_core.
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
		double gdp = 0.0;
		// F3: compra institucional do Estado no dia (paper/computer).
		double government_goods_spend = 0.0;
		// F5: última cotação de importação (preço entregue na moeda local) por
		// produto; forma o preço local em cidade sem produtor (§2.7).
		double import_price[kDemoMaxProducts] = {};
		// F5: tarifa arrecadada no dia (receita do governo do comprador).
		double tariff_revenue = 0.0;
		double revenue_by_product[kDemoMaxProducts] = {};
		// Fluxos comerciais do dia, por produto.
		double imported[kDemoMaxProducts] = {};
		double exported[kDemoMaxProducts] = {};
		double import_value[kDemoMaxProducts] = {};
		double export_value[kDemoMaxProducts] = {};
		// Aptidão territorial por produto (0..1). A produção primária é
		// multiplicada por ela; o GeographyService sobrescreve com dados reais.
		// Inicializador: trigo/ferro/arroz = 1.0, os demais = 0.0 (default).
		double suitability[kDemoMaxProducts] = {1.0, 1.0, 1.0};
		// Teto de demanda externa (t/dia) por produto, declarado pelo
		// GeographyService (participação da nação no mercado mundial). 0 = sem
		// exportação deste bem nesta cidade.
		double external_demand[kDemoMaxProducts] = {};
		// Reservas minerais por produto (extração com esgotamento).
		DemoDeposit deposits[kDemoMaxProducts];
		DemoPop pop;
		DemoMarket markets[kDemoMaxProducts];
		// F8: mercados de serviço por setor (ordem de services.json).
		DemoServiceMarket service_markets[kDemoMaxServiceSectors];
		double service_export_revenue = 0.0;
		double service_government_revenue = 0.0;
		double service_input_cost = 0.0;
		double service_business_profit = 0.0;
		int service_business_count = 0;
		double private_health_cover = 0.0;
		double private_education_cover = 0.0;
		// Tonelagem movimentada no dia anterior (base do B2B de logística).
		double service_tonnage_prev = 0.0;
	};

	// Fases medidas por demo_set_profiling (ordem = ordem do tick).
	enum DemoPhase {
		kDemoPhaseArrival = 0, kDemoPhaseReset, kDemoPhaseLabor,
		kDemoPhaseProduction, kDemoPhaseRaw, kDemoPhaseIncome,
		kDemoPhaseServices, kDemoPhaseTrade,
		kDemoPhaseAccounting, kDemoPhasePrices, kDemoPhaseClamp,
		kDemoPhaseFiscal, kDemoPhaseFx, kDemoPhaseInvariants, kDemoPhaseStatus,
		kDemoPhaseCount
	};

	struct DemoShipment {
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

	// Retrato do estado demo para reverter um tick que viole um invariante.
	struct DemoSnapshot {
		std::vector<DemoCity> cities;
		std::vector<DemoBuilding> buildings;
		std::vector<DemoServiceBusiness> service_businesses;
		int next_service_business_id = 1;
		int service_bankruptcies = 0;
		std::vector<DemoShipment> transit;
		std::unordered_map<std::string, DemoFleet> fleets;
		// F9: estado dos grupos (caixa + lucro por anfitrião), confiança das
		// nações e fluxos acumulados — desfazer um tick corrompido também
		// desfaz a distribuição de lucro e a repatriação.
		std::vector<DemoGroup> groups;
		std::unordered_map<std::string, double> confidence;
		std::unordered_map<std::string, DemoGroupFlow> group_flows;
		std::vector<DemoCurrency> currencies;
		double transport_cash = 0.0;
		double foreign_cash = 0.0;
		double construction_cash = 0.0;
		double produced[kDemoMaxProducts] = {};
		double consumed_total[kDemoMaxProducts] = {};
		double produced_today[kDemoMaxProducts] = {};
		double consumed_today[kDemoMaxProducts] = {};
		int day = 0;
	};
