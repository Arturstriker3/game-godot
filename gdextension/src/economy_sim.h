#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <cstddef>
#include <string>
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

	/** Estado do cenário de validação (cidades, POPs, mercados, PIB). */
	Dictionary get_demo_status() const;

	/** Choque de preço em um produto ("wheat", "flour" ou "bread"). */
	void demo_price_shock(const String& product, double multiplier);

	/**
	 * Define a aptidão territorial de um produto para uma cidade.
	 * @param city_id Identificador da cidade.
	 * @param product "wheat", "flour" ou "bread".
	 * @param value Aptidão entre 0 e 1 (0 impede a produção primária).
	 */
	void demo_set_suitability(const String& city_id, const String& product, double value);

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
	// Cenário de validação: duas cidades, população agregada (POPs), trigo,
	// farinha e pão. Vive no mesmo núcleo para validar as regras antes de
	// generalizar o modo europeu.
	// ------------------------------------------------------------------
	enum class DemoProduct { Wheat = 0, Flour = 1, Bread = 2, Iron = 3 };
	static constexpr int kDemoProducts = 4;
	static constexpr int kDemoFoodProducts = 3;

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
		int kind = 0; // 0=fazenda, 1=moinho, 2=padaria (legado de exibição)
		std::string resource;  // recurso produzido: wheat, flour, bread ou iron
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
		double food_spend = 0.0;
		double consumed = 0.0;
		double need = 0.0;
		double satisfaction = 0.0;
		double savings = 0.0;
		// Consumo por categoria (trigo, farinha, pão): desejado e realizado.
		double desired_by_category[kDemoFoodProducts] = {0.0, 0.0, 0.0};
		double consumed_by_category[kDemoFoodProducts] = {0.0, 0.0, 0.0};
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
		double revenue_by_product[kDemoProducts] = {0.0, 0.0, 0.0, 0.0};
		// Fluxos comerciais do dia, por produto.
		double imported[kDemoProducts] = {0.0, 0.0, 0.0, 0.0};
		double exported[kDemoProducts] = {0.0, 0.0, 0.0, 0.0};
		double import_value[kDemoProducts] = {0.0, 0.0, 0.0, 0.0};
		double export_value[kDemoProducts] = {0.0, 0.0, 0.0, 0.0};
		// Aptidão territorial por produto (0..1). A produção primária é
		// multiplicada por ela; o GeographyService sobrescreve com dados reais.
		double suitability[kDemoProducts] = {1.0, 1.0, 1.0, 0.0};
		DemoPop pop;
		DemoMarket markets[kDemoProducts];
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
		double produced[kDemoProducts] = {0.0, 0.0, 0.0, 0.0};
		double consumed_total[kDemoProducts] = {0.0, 0.0, 0.0, 0.0};
		double produced_today[kDemoProducts] = {0.0, 0.0, 0.0, 0.0};
		double consumed_today[kDemoProducts] = {0.0, 0.0, 0.0, 0.0};
		int day = 0;
	};

	// ------------------------------------------------------------------
	// Benchmark: economia sintética com estruturas planas. Sem strings,
	// sem nós e sem alocações por tick. Índices substituem identificadores.
	// ------------------------------------------------------------------
	static constexpr int kBenchProducts = 3;

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
	Dictionary demo_city_to_dict(const DemoCity& city) const;
	double demo_money_total() const;
	std::vector<std::string> demo_check_invariants(double money_before) const;
	void demo_snapshot(DemoSnapshot& out) const;
	void demo_restore(const DemoSnapshot& in);
	void demo_trade_wheat();
	static int demo_product_index(const String& product);
	static const char* demo_product_name(int product);
	static double demo_base_price(int product);
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
	std::vector<DemoCity> demo_cities_;
	std::vector<DemoBuilding> demo_buildings_;
	std::vector<DemoShipment> demo_in_transit_;
	std::vector<std::string> demo_invariant_violations_;
	double demo_transport_cash_ = 0.0;
	double demo_foreign_cash_ = 0.0;
	// Setor de construção: recebe o capital investido em expansão.
	double demo_construction_cash_ = 0.0;
	double demo_produced_[kDemoProducts] = {0.0, 0.0, 0.0, 0.0};
	double demo_consumed_total_[kDemoProducts] = {0.0, 0.0, 0.0, 0.0};
	double demo_produced_today_[kDemoProducts] = {0.0, 0.0, 0.0, 0.0};
	double demo_consumed_today_[kDemoProducts] = {0.0, 0.0, 0.0, 0.0};

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
