#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_float64_array.hpp>

#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace godot {

/**
 * Núcleo econômico do jogo, exposto ao Godot via GDExtension.
 *
 * O mundo é formado por cidades: cada nação possui uma ou mais cidades, e cada
 * cidade tem população, produção e demanda próprias. A necessidade de trigo é
 * resolvida entre cidades por um mercado em três camadas — regional (cidades
 * vizinhas do mesmo país), nacional (resto do país) e internacional — e a carga
 * só chega ao destino após o transporte físico.
 *
 * Tipos de transporte, com velocidade e capacidade distintas:
 *   caminhão - velocidade mediana, carga pequena
 *   navio    - velocidade baixa e constante, carga enorme
 *   avião    - velocidade alta, carga média (emergência de estoque)
 */
class EconomySim : public RefCounted {
	GDCLASS(EconomySim, RefCounted)

public:
	EconomySim();
	~EconomySim() override;

	/** Cria cidades e portos a partir de dados de cenário (JSON) e zera o relógio. */
	void initialize(const Array& cities, const Array& ports);

	/** Avança um dia de simulação e devolve o resumo do dia. */
	Dictionary tick();

	/** Estado atual (estoques, população, trânsito) sem avançar o tempo. */
	Dictionary get_status() const;

	/** Inicializa o cenário de validação de duas cidades com população agregada. */
	void initialize_demo();

	/**
	 * Inicializa o modelo demo a partir dos dados reais do cenário (cidades,
	 * portos e construções), em vez do cenário de validação AGRIA/PORTUS. Este é
	 * o passo do contrato único: o ciclo completo roda sobre o mundo real.
	 */
	void initialize_demo_world(const Array& cities, const Array& ports, const Array& buildings);

	/** Avança um dia do cenário de validação. */
	Dictionary tick_demo();

	/**
	 * Executa o dia do cenário de validação sem montar o resumo (nenhum Variant
	 * é criado). É a metade "cara" do tick: o jogo roda em WorkerThreadPool e
	 * chama get_demo_status() na main thread quando o trabalho termina.
	 */
	void demo_tick_core();

	/**
	 * Roda o dia E monta/publica o status, sem devolver nada.
	 *
	 * É o que a thread do WorkerThreadPool chama (ver tick_scheduler.gd): o
	 * trabalho pesado e a montagem do status ficam fora da main thread e
	 * NENHUM Variant cruza a fronteira da thread (nem o retorno). Depois do
	 * join, a main thread chama get_demo_status(), que devolve o slot já
	 * publicado em O(1).
	 */
	void demo_tick_and_publish();

	/** Liga/desliga a medição por fase do tick (custo desprezível). */
	void demo_set_profiling(bool enabled);

	/**
	 * Marca o status publicado como desatualizado. Qualquer método que muda o
	 * estado fora do tick (investir, construir, choque de preço, aptidão) tem de
	 * chamar isto: senão get_demo_status() devolve o snapshot do início do dia e
	 * a UI mostra caixa/capacidade antigos até o próximo tick.
	 */
	void demo_status_invalidate();

	/** Microssegundos por fase do último tick (vazio quando desligado). */
	Dictionary demo_get_profile() const;
	// Pré-aquece os dois slots do status: sem isso o primeiro dia que publica
	// num slot frio paga a criação de todos os dicionários das cidades.
	void demo_warm_status();

	/**
	 * Estado do cenário de validação (cidades, POPs, mercados, PIB).
	 *
	 * O dicionário é REAPROVEITADO entre ticks (ver DemoStatusCache): cada
	 * chamada reescreve os valores nos mesmos contêineres, sem alocar 650 × 21
	 * dicionários por dia. Contrato: o resultado é uma fotografia viva, válida
	 * até a próxima chamada; quem precisa reter o valor entre dias deve copiar
	 * (`duplicate(true)` no GDScript).
	 */
	Dictionary get_demo_status();

	/** Choque de preço em um produto (id de goods.json, ex.: "wheat"). */
	void demo_price_shock(const String& product, double multiplier);

	/**
	 * Define a aptidão territorial de um produto para uma cidade.
	 * @param city_id Identificador da cidade.
	 * @param product id de goods.json (ex.: "wheat", "iron").
	 * @param value Aptidão entre 0 e 1 (0 impede a produção primária).
	 */
	void demo_set_suitability(const String& city_id, const String& product, double value);

	/**
	 * Declara a reserva mineral do território de uma cidade para um produto
	 * (extração com esgotamento). As minas da cidade que produzem o produto
	 * consomem esta reserva compartilhada; reserva zerada para a produção.
	 * Idempotente: se o produto já tem reserva, mantém o saldo restante (um
	 * save carregado não é "reabastecido") e só atualiza qualidade/profundidade.
	 * @param product id de goods.json (ex.: "iron", "gold").
	 * @param reserve_kt Reserva inicial em quilotoneladas (1 kt = 1000 t).
	 * @param quality Qualidade 0..1 (eficiência = 0,5 + 0,5 × qualidade).
	 * @param depth_m Profundidade média em metros (encarece o opex).
	 * @returns true se a reserva foi criada agora.
	 */
	bool demo_set_deposit(const String& city_id, const String& product,
		double reserve_kt, double quality, double depth_m);

	/**
	 * Define o índice de renda externa de uma cidade (multiplicador sobre a
	 * renda-base de serviços/exportações não modelados).
	 *
	 * A nação recebe renda do resto do mundo proporcional ao seu PIB PPP per
	 * capita REAL: o `GeographyService` calcula, a partir de `nations.csv`
	 * (gdp_ppp_bn/population), um índice normalizado para que a média mundial
	 * ponderada por população fique em 1,0 — ou seja, a renda média continua
	 * `kDemoExternalIncomePerCapita`. Nações ricas sem terra (SAU/QAT/ISL)
	 * importam mais; nações pobres recebem o piso (10% da média) e continuam
	 * frágeis. O valor é pago pelo setor externo (`foreign_cash`), então
	 * `money_total` continua conservado.
	 * @param city_id Identificador da cidade.
	 * @param index Multiplicador (0 = sem renda externa; 1 = média mundial).
	 *              Limitado a [0, 8] para conter dados corrompidos.
	 */
	void demo_set_external_income_index(const String& city_id, double index);

	/**
	 * Define o teto de demanda externa (t/dia) de um produto para uma cidade.
	 * É a demanda LIMITADA do resto do mundo para bens brutos (minerais, soja,
	 * algodão e madeira): a fase 4d compra no máximo este teto por dia, e a
	 * produção acima dele vira estoque, o que derruba o preço (fase 8). O teto
	 * vem da participação da nação no mercado mundial — reserva esgotada em ~25
	 * anos (GeographyService.apply_to_sim). Sem teto declarado (0), o bem não é
	 * exportado (comportamento legado dos bens sem depósito/aptidão).
	 * @param product id de goods.json (ex.: "coal", "soy").
	 * @param ceiling_t_per_day Teto diário em toneladas (>= 0).
	 */
	void demo_set_external_demand(const String& city_id, const String& product,
		double ceiling_t_per_day);

	/**
	 * Investe capital da empresa na expansão de uma construção existente.
	 * O valor sai do caixa da empresa e é pago ao setor de construção.
	 * @param city_id Cidade da construção.
	 * @param building_id Identificador da construção.
	 * @param amount Capital a investir.
	 * @returns Resumo com "invested", "capacity_added" e "error".
	 */
	Dictionary demo_invest(const String& city_id, const String& building_id, double amount);

	/**
	 * Cria uma construção nova. O custo sai do caixa da empresa (private) ou do
	 * orçamento do governo (state) e é pago ao setor de construção.
	 * @returns Resumo com "created", "cost" e "error".
	 */
	Dictionary demo_build(const String& city_id, const String& building_id,
		const String& resource, int input_product, int output_product,
		double capacity, double jobs, double wage, double opex,
		double input_per_output, const String& owner, double cost);

	/**
	 * Gasta orçamento público em serviços domésticos (ex.: prospecção).
	 * @returns true quando havia orçamento suficiente.
	 */
	bool demo_spend_government(const String& city_id, double amount);

	/** Serializa todo o estado do cenário (save). */
	Dictionary demo_to_dict() const;

	/** Restaura o estado do cenário a partir de demo_to_dict() (load). */
	void demo_from_dict(const Dictionary& data);

	/**
	 * Ajusta parâmetros de balanceamento usados pelo laboratório.
	 * Chaves aceitas: natural_unemployment, service_wage_adjust,
	 * service_wage_max, inventory_cover_days, inventory_recovery.
	 */
	void demo_set_balance(const Dictionary& values);

	/**
	 * Cria um mundo sintético determinístico para benchmark.
	 * @param city_count Número de cidades.
	 * @param buildings_per_city Construções por cidade (mínimo 3).
	 * @param seed Semente determinística da geração.
	 */
	void initialize_benchmark(int city_count, int buildings_per_city, int seed);

	/**
	 * Executa o benchmark e devolve médias, p95, fases e memória.
	 * @param days Número de dias medidos.
	 * @param warmup_days Dias descartados antes da medição.
	 */
	Dictionary run_benchmark(int days, int warmup_days);

	/** Resumo do mundo de benchmark (contagens, memória, checksum). */
	Dictionary get_benchmark_status() const;

	int get_benchmark_city_count() const;

protected:
	static void _bind_methods();

private:
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

	struct Shipment {
		std::string to_city;
		double amount;
		double arrival_hour;
	};

	struct City {
		std::string id;
		std::string nation;       // dono atual (economia + cor do território)
		std::string home_nation;  // região natural (território que controla)
		std::string name;
		double population;
		double production;
		double stockpile;
		MapPoint center;
		// Massa de terra contínua (rota por terra só dentro dela). O porto de
		// travessia é uma entidade separada (ver `Port`/`ports_`).
		std::string landmass;
		// Valores por dia, recalculados a cada tick.
		double need = 0.0;
		double consumed = 0.0;
		double deficit = 0.0;
		double dispatched = 0.0;
		// Indicadores derivados, lidos pelo crescimento urbano visual.
		double jobs = 0.0;
		double income = 0.0;
		double industrialization = 0.0;
	};

	struct Port {
		std::string id;
		std::string name;
		std::string nation;  // dono (estratégico: sem porto, sem mar)
		MapPoint center;
		std::string landmass;
	};

	struct TradeOrder {
		std::string from_city;
		std::string to_city;
		std::string from_nation;
		std::string to_nation;
		MarketScope scope = MarketScope::Regional;
		double amount = 0.0;
		double transit_hours = 0.0;
		double freight_cost = 0.0;
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
	// Mesma ordem de data/definitions/economy/goods.json.
	static constexpr int kDemoProducts = 21;
	// Farinha/pão foram removidos da cadeia (subprodutos ficam para depois):
	// a comida é consumida diretamente das culturas. A cesta alimentar (B3) é
	// substituível entre trigo, arroz, milho, batata e soja.
	// A necessidade é medida em toneladas-equivalentes de grão: o fator calórico
	// converte tonelada real em tonelada-equivalente (batata ~1/4 das calorias).
	static constexpr int kDemoFoodProducts = 5;
	// Ids de produto (índice em kDemoProductNames) e fator calórico por slot da
	// cesta. Fonte (FAO/INFOODS, composição média): cereais ~3600 kcal/kg;
	// batata fresca ~770 kcal/kg — ~1/4 das calorias por tonelada (fator 0,25);
	// soja seca (grãos maduros) ~4460 kcal/kg (USDA FoodData Central) vs trigo
	// ~3400 kcal/kg (FAO/INFOODS) — 446/340 ≈ 1,31, arredondado para 1,3.
	static constexpr int kDemoFoodProductIds[kDemoFoodProducts] = {0, 2, 3, 16, 17};
	static constexpr double kDemoCaloricFactor[kDemoFoodProducts] = {1.0, 1.0, 1.0, 0.25, 1.3};

	struct DemoMarket {
		double stock = 0.0;
		double price = 1.0;
		double demand = 0.0;   // compras realizadas (consumo, insumos e exportação)
		double supply = 0.0;   // produção ofertada no dia
		double desired = 0.0;  // demanda desejada antes de estoque/orçamento
		double sold = 0.0;     // vendas realizadas no dia
		double sold_prev = 0.0; // vendas do dia anterior (base da produção)
		double shortage = 0.0; // demanda desejada não atendida
		double surplus = 0.0;  // estoque acima do necessário
	};

	struct DemoBuilding {
		std::string id;
		std::string city;
		int kind = 0; // 0=fazenda (único tipo hoje; campo mantido por exibição)
		std::string resource;  // recurso produzido (id de goods.json)
		std::string owner = "private"; // private ou state
		int input_product = -1; // -1 = extração primária (sem insumo)
		int output_product = 0;
		double capacity = 0.0;
		double input_per_output = 0.0;
		double jobs = 0.0;
		double wage = 0.0;
		double opex = 0.0;
		double workers = 0.0;
		double utilization = 0.0;
		double output_scale = 1.0; // reduzido por prejuízos persistentes
		int loss_days = 0;
		bool bankrupt = false; // fechada por insolvência (prejuízo persistente)
		bool depleted = false; // jazida esgotada: encerra sem contar falência
		double revenue = 0.0;
		double input_value = 0.0;
		double costs = 0.0;
		double profit = 0.0;
		// Valores efetivamente pagos no dia (podem ser menores que o contrato
		// quando o caixa da empresa não cobre a folha e o opex).
		double paid_wages = 0.0;
		double paid_opex = 0.0;
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
		// Consumo por categoria (hoje só trigo): desejado e realizado.
		double desired_by_category[kDemoFoodProducts] = {0.0};
		double consumed_by_category[kDemoFoodProducts] = {0.0};
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

	struct DemoCity {
		std::string id;
		std::string name;
		std::string nation;
		std::string home_nation;  // região natural (território que controla)
		std::string landmass;     // massa de terra contínua (rota terrestre)
		MapPoint center{0.0, 0.0};
		double population = 0.0;
		double firm_cash = 0.0;
		double service_cash = 0.0;
		double government_budget = 0.0;
		// Multiplicador da renda externa (PIB PPP per capita da nação,
		// normalizado pela média mundial ponderada = 1,0). Ver
		// demo_set_external_income_index e §20 do balance_review.
		double external_income_index = 1.0;
		double service_jobs = 0.0;
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
		double revenue_by_product[kDemoProducts] = {};
		// Fluxos comerciais do dia, por produto.
		double imported[kDemoProducts] = {};
		double exported[kDemoProducts] = {};
		double import_value[kDemoProducts] = {};
		double export_value[kDemoProducts] = {};
		// Aptidão territorial por produto (0..1). A produção primária é
		// multiplicada por ela; o GeographyService sobrescreve com dados reais.
		// Inicializador: trigo/ferro/arroz = 1.0, os demais = 0.0 (default).
		double suitability[kDemoProducts] = {1.0, 1.0, 1.0};
		// Teto de demanda externa (t/dia) por produto, declarado pelo
		// GeographyService (participação da nação no mercado mundial). 0 = sem
		// exportação deste bem nesta cidade.
		double external_demand[kDemoProducts] = {};
		// Reservas minerais por produto (extração com esgotamento).
		DemoDeposit deposits[kDemoProducts];
		DemoPop pop;
		DemoMarket markets[kDemoProducts];
	};

	// Fases medidas por demo_set_profiling (ordem = ordem do tick).
	enum DemoPhase {
		kDemoPhaseArrival = 0, kDemoPhaseReset, kDemoPhaseLabor,
		kDemoPhaseProduction, kDemoPhaseRaw, kDemoPhaseIncome, kDemoPhaseTrade,
		kDemoPhaseAccounting, kDemoPhasePrices, kDemoPhaseClamp,
		kDemoPhaseFiscal, kDemoPhaseInvariants, kDemoPhaseStatus,
		kDemoPhaseCount
	};

	struct DemoShipment {
		std::string to_city;
		int product = 0;
		double amount = 0.0;
		int arrival_day = 0;
	};

	// Retrato do estado demo para reverter um tick que viole um invariante.
	struct DemoSnapshot {
		std::vector<DemoCity> cities;
		std::vector<DemoBuilding> buildings;
		std::vector<DemoShipment> transit;
		double transport_cash = 0.0;
		double foreign_cash = 0.0;
		double construction_cash = 0.0;
		double produced[kDemoProducts] = {};
		double consumed_total[kDemoProducts] = {};
		double produced_today[kDemoProducts] = {};
		double consumed_today[kDemoProducts] = {};
		int day = 0;
	};

	// ------------------------------------------------------------------
	// Cache do status: os mesmos Dictionary/Array são reescritos a cada dia.
	//
	// Motivo: montar o status do zero custa ~62 ms em 650 cidades (650 × 21
	// dicionários de mercado, 650 POPs, 650 camadas de construção). Reescrever
	// valores em contêineres existentes não aloca e não re-hasheia a estrutura.
	// Campos imutáveis (id, nome, centro, aptidão) só são reescritos quando a
	// origem muda, comparando contra a cópia em std::string (memcmp barato).
	// ------------------------------------------------------------------
	struct DemoStatusCitySlot {
		Dictionary dict;
		// Strings estáveis: evitam realocar String a cada dia.
		std::string id_src, name_src, nation_src, home_src, landmass_src;
		String id, name, nation, home_nation, landmass;
		Array center;
		Array suitability;
		Array external_demand;
		Dictionary deposits;
		std::vector<Dictionary> deposit_entry;  // 1 por produto (lazy)
		// Bloco estático de cada jazida (reserva inicial, qualidade,
		// profundidade, eficiência, opex) escrito UMA vez; por dia só mudam
		// reserva restante, extraído e esgotado. Antes o dicionário inteiro era
		// reconstruído a cada dia (String + Dictionary aninhado por cidade).
		std::vector<char> deposit_present;      // 1 por produto
		std::vector<char> deposit_depleted;     // último `depleted` publicado
		std::vector<DemoDeposit> deposit_written;  // último valor publicado
		std::vector<String> deposit_key;        // nome do produto (chave)
		// Versão estática com que o bloco de jazida foi escrito: uma jazida
		// descoberta depois (`demo_set_deposit`) força reescrevê-lo.
		int deposit_static_version = -1;
		Dictionary pop;
		Array desired_by_category;
		Array consumed_by_category;
		Array markets;
		std::vector<Dictionary> market_entry;  // 1 por produto
		Array buildings;
		std::vector<Dictionary> building_entry;  // cresce com a cidade
		Array imported, exported, import_value, export_value;
		// Cópias dos últimos valores gravados em `markets` e nos quatro fluxos
		// de comércio. A cidade típica movimenta 2–5 dos 21 produtos: escrever
		// os outros 16 custava ~100 mil hashes de String por dia (650 cidades).
		// Comparar double é ~20× mais barato que escrever em Dictionary.
		std::vector<DemoMarket> market_written;
		std::vector<char> market_written_filled;  // 1 por produto
		std::vector<double> io_written;           // 4 × kDemoProducts
		std::vector<char> io_filled;              // 4 flags (um por array)
		bool static_filled = false;
		int static_version = -1;
	};

	// Chaves do status pré-construídas. `dict["price"]` converte UTF-8 ->
	// UTF-32 e aloca uma String a cada escrita; com as chaves prontas a escrita
	// fica só com o hash e a atribuição. São `StringName` (hash calculado uma
	// vez, comparação por ponteiro): cada cidade reescreve ~45 chaves por dia e
	// o `String` recalculava o hash de todas elas (medido no cenário real de
	// 858 cidades). O GDScript continua lendo por `String` — o hash é o mesmo e
	// o Dictionary compara `String`/`StringName` pela sequência de caracteres.
	struct DemoStatusKeys {
		StringName amount, bankruptcies, buildings, calendar_day, capacity, capital_cost_per_capacity, capital_income, center, cities, city_count, construction_cash, construction_revenue, consumed, consumed_by_category, consumed_today, consumed_total, costs, day, deficit, demand, dependents, depleted, depleted_mines, deposits, depth_m, desired, desired_by_category, disposable, disposable_per_capita, efficiency, employed, export_value, exported, external_demand, external_income, external_income_index, extracted_kt, firm_cash, food_spend, foreign_cash, freight_cost, freight_revenue, from_city, from_nation, gdp, gdp_per_capita, good, government_budget, home_nation, id, import_value, imported, in_transit, income, industrialization, initial_kt, input_product, input_value, invariant_violations, jobs, kind, labor_force, landmass, legs, living_standard, loss_days, market_totals, markets, mode, money_total, name, nation, nations, need, opex, opex_factor, opex_revenue, output_product, output_scale, owner, pop, population, ports, price, produced_today, produced_total, product, production, products, profit, quality, remaining_kt, resource, revenue, satisfaction, savings, scope, service_cash, service_profit, service_revenue, service_spend, service_wage, shortage, site_suitability, sold, stock, stockpile, suitability, supply, surplus, tag, taxes, to_city, to_nation, trade_orders, transfers, transit_hours, transport_cash, unemployed, utilization, wage, wage_income, workers, x, y;
		DemoStatusKeys();
	};

	struct DemoStatusCache {
		Dictionary root;
		Array cities;
		Array ports;
		Array orders;
		Array products;
		Array violations;
		Array nations;
		Array tot_produced, tot_consumed, tot_produced_today, tot_consumed_today;
		// Totais do sistema por produto (soma dos mercados das cidades). A IA
		// lia 858 cidades × 21 mercados em GDScript no frame da virada (~13 ms);
		// o núcleo publica aqui a mesma soma, calculada na thread do tick.
		Dictionary market_totals;
		PackedFloat64Array tot_surplus, tot_sold, tot_demand;
		std::vector<DemoStatusCitySlot> city_slots;
		std::vector<Dictionary> port_slots;
		std::vector<Dictionary> order_slots;
		std::vector<Array> order_leg_slots;		std::vector<Dictionary> nation_slots;
		// Caminho total em trânsito por cidade (evita varredura O(cidades ×
		// cargas) dentro do laço de cidades).
		std::vector<double> in_transit_by_city;
		bool header_filled = false;
	};

	// ------------------------------------------------------------------
	// Benchmark: economia sintética com estruturas planas. Sem strings,
	// sem nós e sem alocações por tick. Índices substituem identificadores.
	// Modela só trigo (produção/consumo diretos) — farinha/pão removidos.
	// ------------------------------------------------------------------
	static constexpr int kBenchProducts = 1;

	struct BenchCity {
		double population = 0.0;
		double firm_cash = 0.0;
		double service_cash = 0.0;
		double government_budget = 0.0;
		double capital_income_next = 0.0;
		double gdp = 0.0;
		double service_jobs = 0.0;
		double service_wage = 0.0;
		double service_wage_bill = 0.0;
		double service_revenue = 0.0;
		double service_profit = 0.0;
		double opex_revenue = 0.0;
		double need = 0.0;
		double disposable = 0.0;
		double consumed = 0.0;
		double food_spend = 0.0;
		double satisfaction = 0.0;
		double savings = 0.0;
		double labor_force = 0.0;
		double employed = 0.0;
		double unemployed = 0.0;
		double wage_income = 0.0;
		double capital_income = 0.0;
		double taxes = 0.0;
		double transfers = 0.0;
		double service_spend = 0.0;
	};

	struct BenchMarket {
		double stock = 0.0;
		double price = 1.0;
		double demand = 0.0;
		double supply = 0.0;
		double desired = 0.0;
		double sold = 0.0;
		double sold_prev = 0.0;
	};

	struct BenchBuilding {
		int city = 0;
		int output = 0;
		int input = -1;
		int output_market = 0;
		int input_market = -1;
		double capacity = 0.0;
		double input_per_output = 0.0;
		double output_share = 0.0;
		double jobs = 0.0;
		double wage = 0.0;
		double opex = 0.0;
		double workers = 0.0;
		double utilization = 0.0;
		double output_scale = 1.0;
		int loss_days = 0;
		double revenue = 0.0;
		double input_value = 0.0;
		double costs = 0.0;
		double profit = 0.0;
	};

	struct BenchTradeLink {
		int from = 0;
		int to = 0;
		int product = 0;
		int from_market = 0;
		int to_market = 0;
		double capacity = 0.0;
	};

	static const ModeSpec& spec(TransportMode mode);
	static const char* scope_name(MarketScope scope);

	City* find_city(const std::string& id);
	const City* find_city(const std::string& id) const;

	/**
	 * Monta o plano de transporte entre duas cidades.
	 * @param seller Cidade vendedora.
	 * @param buyer Cidade compradora.
	 * @param amount Carga total em toneladas.
	 * @param air_share Fração que segue por avião.
	 */
	std::vector<Leg> build_route(const City& seller, const City& buyer, double amount,
	                             double air_share) const;

	/** Cria um trecho e calcula viagens, tempo e custo. */
	Leg make_leg(TransportMode mode, const std::string& from, const std::string& to,
	             const MapPoint& from_point, const MapPoint& to_point, double load_tons) const;

	/**
	 * Abastece uma cidade compradora a partir dos vendedores disponíveis.
	 * @param buyer Cidade com déficit regional.
	 * @param remaining Déficit ainda sem cobertura; é reduzido conforme o pedido é criado.
	 * @param sellers Vendedores com excedente.
	 * @param scope Camada de mercado que pode ser usada neste passo.
	 * @returns true se um pedido foi criado.
	 */
	bool dispatch_trade(City& buyer, double& remaining, const std::vector<City*>& sellers,
	                    MarketScope scope);

	/** Indica se o vendedor pertence à camada de mercado pedida. */
	bool in_scope(const City& buyer, const City& seller, MarketScope scope) const;

	/** Rota marítima entre a Europa continental e a Inglaterra. */
	std::vector<Leg> build_overseas_route(const City& seller, const City& buyer, double amount) const;

	double distance_km(const MapPoint& a, const MapPoint& b) const;
	double in_transit_total(const std::string& city_id) const;
	const Port* nearest_port(const std::string& nation, const std::string& landmass,
	                         const MapPoint& from) const;

	Dictionary city_to_dict(const City& city) const;
	Dictionary port_to_dict(const Port& port) const;
	Dictionary nation_to_dict(const std::string& tag) const;
	Dictionary leg_to_dict(const Leg& leg) const;
	Dictionary order_to_dict(const TradeOrder& order) const;

	DemoCity* find_demo_city(const std::string& id);
	const DemoCity* find_demo_city(const std::string& id) const;
	/**
	 * Refaz os índices derivados do mundo (id da cidade -> posição e
	 * construções por cidade). Chamado ao inicializar, ao carregar um save e no
	 * início de cada tick: sem eles, as fases varrem TODAS as construções para
	 * cada cidade (O(cidades × construções), que não escala para 650 cidades).
	 */
	void demo_rebuild_indexes();
	/**
	 * Preenche o slot de status de uma cidade (reaproveitando dicionários).
	 * @param city_index Posição em demo_cities_.
	 * @param slot Cache da cidade (mesma posição em demo_status_cache_).
	 * @param in_transit Carga em trânsito até a cidade (já somada em passada única).
	 */
	void demo_fill_city_status(std::size_t city_index, DemoStatusCitySlot& slot,
		double in_transit) const;
	double demo_money_total() const;
	std::vector<std::string> demo_check_invariants(double money_before) const;
	void demo_snapshot(DemoSnapshot& out) const;
	void demo_restore(const DemoSnapshot& in);
	void demo_trade_food();
	void demo_national_fiscal_transfer();
	const Port* demo_nearest_port(const std::string& nation, const std::string& landmass,
	                              const MapPoint& from) const;
	static int demo_product_index(const String& product);
	// Slot da cesta alimentar (0..kDemoFoodProducts-1) -> id de produto.
	static int demo_food_product(int slot);
	// Id de produto -> fator calórico (0.0 se não for alimento da cesta).
	static double demo_food_calories(int product);
	static const char* demo_product_name(int product);
	static double demo_extraction_efficiency(const DemoDeposit& deposit);
	static double demo_depth_opex_factor(const DemoDeposit& deposit);
	double demo_base_price(int product) const;
	/**
	 * Define os preços-base dos bens a partir de dados externos (goods.json).
	 * Cada item deve ter "id" (wheat/iron/...) e "base_price". Bens
	 * desconhecidos mantêm o preço padrão.
	 */
	void demo_set_goods(const Array& goods);
	DemoBuilding* find_demo_building(const std::string& city_id, const std::string& id);

	void tick_benchmark_internal();
	void benchmark_reset();
	void benchmark_labor();
	void benchmark_production();
	void benchmark_consumption();
	void benchmark_trade();
	void benchmark_accounting();
	void benchmark_prices();
	void benchmark_investment();
	double benchmark_checksum() const;
	std::size_t benchmark_memory_bytes() const;
	int bench_market(int city, int product) const {
		return city * kBenchProducts + product;
	}

	std::vector<City> cities_;
	std::vector<Port> ports_;
	std::vector<Shipment> in_transit_;
	std::vector<TradeOrder> last_orders_;
	int day_ = 0;
	double clock_hours_ = 0.0;

	// Estado do cenário de validação de duas cidades. Compartilha o mesmo
	// calendário (day_) do modo legado: apenas um dos modos roda por vez.
	bool demo_active_ = false;
	// true quando o demo foi inicializado a partir do mundo real
	// (initialize_demo_world), em vez do cenário AGRIA/PORTUS. Ativa o comércio
	// genérico entre cidades no lugar dos blocos específicos de validação.
	bool demo_world_mode_ = false;
	// Parâmetros de balanceamento do cenário demo (configuráveis pelo
	// laboratório; os padrões preservam o comportamento do jogo).
	double demo_natural_unemployment_ = 0.04;
	double demo_service_wage_adjust_ = 0.10;
	double demo_service_wage_max_ = 1.0;
	double demo_inventory_cover_days_ = 3.0;
	double demo_inventory_recovery_ = 0.5;
	// Índice espacial do comércio de alimento: por comprador, só os K
	// vendedores mais próximos entram na disputa, além de M ofertas
	// representativas do mercado externo (as mais baratas por caloria no globo).
	// Mundos pequenos (<= kDemoTradeExhaustiveLimit) mantêm a varredura completa
	// para preservar exatamente o comportamento validado no cenário real.
	int demo_trade_neighbors_ = 12;
	int demo_trade_external_ = 8;
	// Medição por fase do tick (benchmark oficial; desligada no jogo).
	bool demo_profile_ = false;
	double demo_profile_us_[kDemoPhaseCount] = {};
	// Subdivisão da fase `status` (benchmark): cidades, pedidos, nações, resto.
	double demo_status_sub_us_[4] = {};
	std::vector<DemoCity> demo_cities_;
	std::vector<DemoBuilding> demo_buildings_;
	// Índices derivados do mundo demo (ver demo_rebuild_indexes).
	std::unordered_map<std::string, int> demo_city_lookup_;
	std::vector<std::vector<int>> demo_buildings_by_city_;
	std::vector<DemoShipment> demo_in_transit_;
	std::vector<std::string> demo_invariant_violations_;
	// Portos do cenário real (modo mundo) e pedidos de comércio do último tick,
	// para a camada visual (minimapa e comboios 3D).
	std::vector<Port> demo_ports_;
	std::vector<TradeOrder> demo_last_orders_;
	int demo_bankruptcies_ = 0;
	int demo_depleted_mines_ = 0;
	double demo_transport_cash_ = 0.0;
	double demo_foreign_cash_ = 0.0;
	// Setor de construção: recebe o capital investido em expansão.
	double demo_construction_cash_ = 0.0;
	double demo_produced_[kDemoProducts] = {};
	double demo_consumed_total_[kDemoProducts] = {};
	double demo_produced_today_[kDemoProducts] = {};
	double demo_consumed_today_[kDemoProducts] = {};
	// Preços-base dos bens (data-driven: demo_set_goods/initialize_demo_world).
	double demo_base_prices_[kDemoProducts] = {
		40.0, 60.0, 55.0, 42.0, 15.0, 25.0, 3000.0, 10.0, 200.0, 400.0,
		15.0, 5000.0, 6.0, 60.0, 30.0, 45.0, 35.0, 45.0, 70.0, 25.0, 500.0};
	// Status reaproveitado entre dias (ver DemoStatusCache). Dois slots
	// alternados: o worker do tick escreve no slot inativo enquanto a UI/IA
	// continuam lendo o último status COMPLETO (double buffer).
	DemoStatusCache demo_status_slots_[2];
	int demo_status_live_ = -1;
	int demo_status_built_day_ = -1;
	// Chaves prontas do status (evitam converter const char* -> String a cada
	// escrita; ver DemoStatusKeys).
	DemoStatusKeys demo_status_keys_;
	// Sobe quando um campo estático das cidades muda (aptidão, demanda externa,
	// depósitos revelados): força a reescrita desses contêineres no status.
	int demo_status_static_version_ = 0;
	// Serializa o acesso ao estado demo entre a thread do tick e a main thread
	// (UI, IA, ferramentas). O `sim` tem UM escritor por convenção
	// (tick_scheduler); o mutex é a rede de segurança para quem chamar
	// tick_demo/get_demo_status/investir enquanto um dia roda na thread — sem
	// ele a disputa vira corrupção de estado e crash (signal 11).
	// Recursivo: tick_demo() chama demo_tick_core() e get_demo_status().
	mutable std::recursive_mutex demo_mutex_;

	// Estado do benchmark sintético (estruturas planas, sem alocação por tick).
	bool bench_active_ = false;
	int bench_cities_count_ = 0;
	int bench_buildings_per_city_ = 0;
	int bench_seed_ = 0;
	int bench_day_ = 0;
	double bench_transport_cash_ = 0.0;
	std::vector<BenchCity> bench_cities_;
	std::vector<BenchMarket> bench_markets_;
	std::vector<BenchBuilding> bench_buildings_;
	std::vector<BenchTradeLink> bench_links_;
	std::vector<double> bench_capacity_total_;
	std::vector<double> bench_unit_labor_opex_;
	std::vector<double> bench_revenue_;
	std::vector<double> bench_produced_total_;
	std::vector<double> bench_consumed_total_;
	std::vector<double> bench_tick_us_;
	double bench_phase_reset_us_ = 0.0;
	double bench_phase_market_us_ = 0.0;
	double bench_phase_pop_us_ = 0.0;
	double bench_phase_production_us_ = 0.0;
	double bench_phase_trade_us_ = 0.0;
	double bench_phase_accounting_us_ = 0.0;
	double bench_phase_prices_us_ = 0.0;
	double bench_phase_investment_us_ = 0.0;

	// Portos usados nas rotas internacionais (Calais e Dover).
	MapPoint france_port_{2525.7, 542.4};
	MapPoint england_port_{2518.2, 539.8};
};

} // namespace godot
