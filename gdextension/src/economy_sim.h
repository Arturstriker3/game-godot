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
	 * F7 calibração: produtividade nacional (TFP) de uma cidade, derivada do
	 * PIB PPP per capita REAL já carregado pelo `GeographyService`
	 * (`external_income_index`, `source: derived:gdp_ppp`).
	 *
	 * Por padrão (índice armazenado = 0) a TFP é DERIVADA do índice de renda:
	 * `clamp(external_income_index, 0,25, 4,0)` — EUA ~3,6, DEU ~3,0, BRA ~0,9,
	 * IND ~0,47, NGA ~0,38 da média mundial. O setter permite que o laboratório
	 * declare um fator explícito; `index <= 0` volta à derivação.
	 *
	 * A TFP multiplica o VALOR ADICIONADO POR TRABALHADOR usado na âncora
	 * salarial (parcela do trabalho × valor adicionado × TFP ÷ vagas
	 * preenchidas), de modo que a folha reflita a produtividade do país. Não
	 * cria dinheiro: o pagamento continua limitado ao caixa da firma
	 * (`pay_scale`), então a diferença aparece na distribuição entre salário e
	 * lucro — países pobres com salário baixo e lucro concentrado nos donos,
	 * ricos com folha maior.
	 * @param city_id Identificador da cidade.
	 * @param index Fator explícito (1 = média mundial; <= 0 = derivar).
	 *              Limitado a [0,05, 8].
	 */
	void demo_set_productivity_index(const String& city_id, double index);

	/**
	 * F7: sobrescreve o perfil de trabalho/educação de uma cidade (hook de
	 * teste/administração e migração). Chaves aceitas (todas opcionais):
	 * `working_age_share` (0..1), `retiree_share` (0..1), `participation`
	 * (array de 3 frações, ou número único) e `education` (array de 3 frações
	 * que somam 1, básico/intermediário/superior). Sem chave, o valor derivado
	 * do índice de renda é mantido. Recalcula a força de trabalho por nível e
	 * reinicializa os salários no âncora do país; não move a população.
	 */
	void demo_set_labor_profile(const String& city_id, const Dictionary& profile);

	/**
	 * F7: salário mínimo por nação (piso diário, em XGS). `value <= 0` desliga
	 * o piso (padrão do jogo). Gancho de política: a UI/governo pode definir.
	 */
	void demo_set_minimum_wage(const String& nation, double value);

	/**
	 * F7: rigidez salarial (fração máxima de ajuste por dia, 0..0,25).
	 * Padrão 0,01 (1%/dia). Gancho de política/sindicatos.
	 */
	void demo_set_wage_rigidity(double value);

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
	 * F8: carrega os setores de serviço (services.json → `sectors`). Pode ser
	 * chamado antes ou depois do mundo: as definições são só catálogo; os
	 * mercados por cidade existem sempre (preço-base default até o dado chegar).
	 * Um arquivo com contagem diferente de kDemoMaxServiceSectors é recusado
	 * (a publicação é indexada por posição).
	 */
	void demo_set_services(const Variant& services);

	/**
	 * F9: carrega a estrutura de emprego setorial (sector_employment.json).
	 * Determinística: a mesma entrada gera a mesma fração por cidade. Chame
	 * antes (ou depois) de criar o mundo — os valores são recalculados todo dia
	 * em demo_update_sector_employment. Sem chamada, vale o espelho compilado
	 * das cinco âncoras (testes/benchmark).
	 */
	void demo_set_sector_employment(const Variant& data);

	/** F8: catálogo carregado (id, nomes, ícone, educaçao, exportável). */
	Array demo_get_service_sectors() const;

	/**
	 * F8: detalhe de uma cidade para a tela de Serviços — mercados dos 20
	 * setores + negócios (dono, capacidade, emprego, lucro, semente do nome).
	 * Fora do status diário de propósito: a lista completa de negócios não é
	 * republicada a cada tick.
	 */
	Dictionary demo_get_services_detail(const String& city_id) const;

	/**
	 * F8: hook de teste/administração — fixa a capacidade AGREGADA de um setor
	 * numa cidade, redistribuindo entre os negócios existentes (ou criando um
	 * se não houver). Serve para medir o efeito da educação/saúde privadas sem
	 * depender da entrada endógena.
	 */
	void demo_set_service_capacity(const String& city_id, const String& sector,
		double units_per_day);

	/** F8: hook de teste — força lucro/dias de prejuízo de um negócio. */
	void demo_set_service_business_state(int business_id, int loss_days,
		double profit);

	/** F8: número de negócios de serviço ativos (não falidos). */
	int demo_get_service_business_count() const;

	/** F8: falências de negócios de serviço acumuladas. */
	int demo_get_service_bankruptcies() const;

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
	 * `recipe_id` (opcional, v4): receita industrial de recipes.json; quando
	 * presente, input_product/input_per_output são derivados do primeiro
	 * insumo da receita (compatibilidade de exibição) e a fase de produção usa
	 * a receita completa (1..6 insumos + subprodutos).
	 * @returns Resumo com "created", "cost" e "error".
	 */
	Dictionary demo_build(const String& city_id, const String& building_id,
		const String& resource, int input_product, int output_product,
		double capacity, double jobs, double wage, double opex,
		double input_per_output, const String& owner, double cost,
		const String& recipe_id = "");

	/**
	 * Gasta orçamento público em serviços domésticos (ex.: prospecção).
	 * @returns true quando havia orçamento suficiente.
	 */
	bool demo_spend_government(const String& city_id, double amount);

	/**
	 * Expande a capacidade de um porto (obra pública): o custo sai do orçamento
	 * da cidade-sede e é pago ao setor de construção; a capacidade entra em
	 * operação depois de `kDemoPortBuildDays` dias.
	 * @param port_id Identificador do porto (ports.json).
	 * @param t_per_day Capacidade a adicionar, em t/dia.
	 * @returns {expanded, cost, days, capacity, error}.
	 */
	Dictionary demo_expand_port(const String& port_id, double t_per_day);

	/**
	 * Hook de teste/administração: define a capacidade corrente de um porto,
	 * mantendo a base (nível 1). Não cobra nem agenda obra.
	 */
	void demo_set_port_capacity(const String& port_id, double t_per_day);

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
	 * F4: define a frota nacional (hook de teste/administração). Marca a frota
	 * como `derived: false` (valor declarado, não derivado). Libera os veículos
	 * ocupados; o combustível e as peças passam a ser exatamente os informados.
	 */
	void demo_set_fleet(const String& nation, double trucks, double ships,
		double planes, double fuel, double spare_parts);

	/** F4: fotografia da frota de uma nação (contagens, saúde, fila do dia). */
	Dictionary demo_get_fleet(const String& nation) const;

	/**
	 * F4: compra `amount` toneladas do bem no mercado interno da nação e
	 * transfere para a frota (1 t de truck/cargo_ship/cargo_plane = 1 veículo;
	 * fuel/spare_parts vão para o estoque). Paga com o orçamento da cidade-sede
	 * (maior população de simulação), até `budget_share` do caixa dela.
	 * @returns {bought, spent, unfilled, error}.
	 */
	Dictionary demo_fleet_buy(const String& nation, const String& product,
		double amount, double budget_share = 0.1);

	/**
	 * Hook de teste/administração: define o estoque de um produto no mercado de
	 * uma cidade (não é usado pelo jogo; serve para exercitar a compra de frota
	 * em cenários controlados).
	 */
	void demo_set_market_stock(const String& city_id, const String& product,
		double stock);

	/**
	 * Hook de teste/administracao: define o preco (na ANCORA XGS) de um produto
	 * no mercado de uma cidade. A moeda local e so exibicao (`price_local`).
	 */
	void demo_set_market_price(const String& city_id, const String& product,
		double price);

	/**
	 * F5: carrega as moedas (currencies.json). Cada item traz `code`,
	 * `nations[]`, `rate_per_anchor` (valor em XGS de 1 unidade local) e
	 * `source`; `rate_per_anchor` nulo/<=0 e uma moeda `unknown:no_gdp` e usa
	 * o fallback documentado (mediana das taxas conhecidas, source
	 * `fallback:median`). `regime` "peg" congela a taxa; ausente/"float" deixa
	 * o cambio dinamico por balanca comercial. Pode ser chamado antes ou depois
	 * do mundo: os precos ficam na ancora e NAO sao reescalados.
	 */
	void demo_set_currencies(const Variant& currencies);

	/** F5: taxa de cambio da moeda de uma nacao (ancora por unidade local). */
	Dictionary demo_get_currency(const String& nation) const;

	/** F5: todas as moedas carregadas (status/UI e testes). */
	Array demo_get_currencies() const;

	/**
	 * F5: hook de teste/administracao para a taxa de uma moeda (por code).
	 * Tambem fixa a base da banda (`base_rate`; sem ele, a propria taxa).
	 * `floating` controla o drift diario e o fator de competitividade.
	 */
	void demo_set_currency_rate(const String& code, double rate,
		bool floating = true, double base_rate = -1.0);

	/**
	 * F5: tarifa ad valorem (0..1) cobrada do comprador `buyer_nation` sobre o
	 * valor CIF (origem convertida + frete) do bem `product` importado de
	 * `seller_nation`. `seller_nation = "*"` vale para qualquer origem.
	 * `product = "*"` vale para todos os bens. A tarifa e receita do governo
	 * do comprador (dinheiro conservado).
	 */
	void demo_set_tariff(const String& buyer_nation, const String& seller_nation,
		const String& product, double rate);

	/** F5: tarifa efetiva (buyer, seller, product) apos os coringas. */
	double demo_get_tariff(const String& buyer_nation,
		const String& seller_nation, const String& product) const;

	/** F5: tarifas declaradas (status/UI). */
	Array demo_get_tariffs() const;

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
		// Unidade de simulação (ver DemoCity). População exibida em `population`.
		double simulation_population = 0.0;
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
		// Quantidade de escalares publicados em demo_fill_city_status; o
		// espelho abaixo é indexado por ordem de chamada de sync_scalar.
		// F9: eram 31 chamadas com espelho de 30 (escrita fora do limite desde a
		// F7); agora 33 (service_jobs_filled e in_kind_food entraram).
		static constexpr int kDemoStatusScalarCount = 33;
		Dictionary dict;
		// Strings estáveis: evitam realocar String a cada dia.
		std::string id_src, name_src, nation_src, home_src, landmass_src, currency_src;
		String id, name, nation, home_nation, landmass, currency;
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
		// Lista de produtos com jazida rastreada nesta cidade. Recalculada só
		// quando uma jazida nova é descoberta: o laço diário percorre apenas as
		// jazidas da cidade, não os 113 bens (medido: ~4,5 ms/dia só de varredura).
		std::vector<int> deposit_tracked;
		int deposit_tracked_version = -1;
		bool deposits_published = false;
		Dictionary pop;
		// Espelhos do que já foi escrito em `pop`: só reescreve o que mudou
		// (cada escrita é uma chamada à ponte GDExtension com hash da chave).
		// 26 campos da F3 + 7 da F7 (idade ativa, aposentados, estudantes,
		// reserva recrutável, nível de renda, salário mínimo, rigidez).
		double pop_written[33] = {};
		bool pop_written_filled = false;
		bool pop_published = false;
		// F7: vetores de educação/trabalho reutilizados (3 posições cada). São
		// mutados em lugar e compartilhados com o Dictionary publicado; só são
		// reinseridos quando o bloco é criado.
		Array education;
		Array education_share;
		Array labor_force_by_level;
		Array employed_by_level;
		Array unemployed_by_level;
		Array unemployment_by_level;
		Array vacancies_by_level;
		Array wage_by_level;
		Array wage_income_by_level;
		Array studying;
		// F7 calibração: renda por estrato (nível de consumo de cada estrato,
		// fatia da POP por nível e renda disponível do estrato).
		Array income_level_share;
		Array income_level_by_stratum;
		Array stratum_disposable;
		Array stratum_population;
		Array stratum_wage_income;
		// F7 perf: espelhos dos vetores acima. Eles são mutados em lugar (e
		// compartilhados com o `pop` publicado), então cada elemento escrito é
		// uma chamada Variant. Educação/estratos mudam em escala de semanas:
		// só reescreve o grupo quando o conteúdo muda de fato.
		double labor_rare_written[16] = {};   // education, força, estudo, fatias, níveis
		bool labor_rare_filled = false;
		double labor_daily_written[24] = {};  // folha/emprego/vagas por nível
		bool labor_daily_filled = false;
		double stratum_written[12] = {};      // renda/população/folha por estrato
		bool stratum_written_filled = false;
		// Espelho dos 25 escalares publicados por cidade (população, PIB,
		// caixas, indicadores derivados): escrita só quando o double muda.
		double scalar_written[kDemoStatusScalarCount] = {};
		bool scalar_written_filled = false;
		// desejado/consumido por categoria alimentar (2 × kDemoMaxFoodProducts).
		double pop_food_written[2 * kDemoMaxFoodProducts] = {};
		Array desired_by_category;
		Array consumed_by_category;
		Array markets;
		std::vector<Dictionary> market_entry;  // 1 por produto
		// F8: bloco plano de serviços (20 setores × kDemoServiceFields). Array
		// de doubles em vez de 20 dicionários por cidade: a publicação custa uma
		// escrita só e a UI indexa pela ordem de `service_sectors`.
		PackedFloat64Array services;
		PackedFloat64Array services_written;
		bool services_filled = false;
		// F9: emprego setorial (4 posições: agro, indústria, serviços,
		// informal) e a fração do emprego. Espelho de 8 doubles evita
		// reescrever o PackedFloat64Array quando nada mudou.
		PackedFloat64Array sector_workers;
		PackedFloat64Array sector_share;
		double sector_written[8] = {};
		bool sector_written_filled = false;
		bool sector_published = false;
		Array buildings;
		std::vector<Dictionary> building_entry;  // cresce com a cidade
		// `buildings` é Array (referência): o dicionário publicado enxerga os
		// elementos mutados. Só é preciso reinserir cada `entry` quando o
		// conjunto muda de tamanho (ou a identidade do prédio no índice muda).
		bool buildings_bound = false;
		bool buildings_published = false;
		// Bloco ESTÁTICO de cada construção (id, kind, resource, owner, receita,
		// insumos, subprodutos): escrito uma vez e reescrito só quando a
		// construção muda. Antes cada prédio alocava Strings + Arrays todos os
		// dias (medido: ~12 ms/dia com ~4 mil prédios no dia 30).
		struct BuildingCache {
			std::string id_src, resource_src, owner_src;
			String id, resource, owner;
			bool filled = false;
			int recipe = -2;
			// Últimos valores diários publicados (comparação de double evita
			// reescrever o Dictionary para prédios parados).
			double capacity = -1.0, jobs = -1.0, wage = -1.0, opex = -1.0;
			double output_scale = -1.0, site = -1.0, last_utilization = -1.0;
			double workers = -1.0, utilization = -1.0, revenue = -1.0;
			double input_value = -1.0, costs = -1.0, profit = -1.0;
			double loss_days = -1.0;
		};
		std::vector<BuildingCache> building_cache;
		Array imported, exported, import_value, export_value;
		// Cópias dos últimos valores gravados em `markets` e nos quatro fluxos
		// de comércio. A cidade típica movimenta 2–5 dos 21 produtos: escrever
		// os outros 16 custava ~100 mil hashes de String por dia (650 cidades).
		// Comparar double é ~20× mais barato que escrever em Dictionary.
		std::vector<DemoMarket> market_written;
		// F5: preço local publicado (`price_anchor ÷ taxa`), por produto. A
		// comparação evita reescrever o Dictionary quando nada mudou; com
		// câmbio float a taxa muda e TODOS os locais são republicados no dia.
		std::vector<double> market_local_written;
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
		StringName amount, bankruptcies, buildings, calendar_day, capacity, capital_cost_per_capacity, capital_income, center, cities, city_count, construction_cash, construction_revenue, consumed, consumed_by_category, consumed_today, consumed_total, costs, day, deficit, demand, dependents, depleted, depleted_mines, deposits, depth_m, desired, desired_by_category, disposable, disposable_per_capita, education, education_share, efficiency, employed, employed_by_level, export_value, exported, external_demand, external_income, external_income_index, extracted_kt, firm_cash, food_spend, foreign_cash, freight_cost, freight_revenue, from_city, from_nation, gdp, gdp_per_capita, good, government_budget, home_nation, id, import_value, imported, in_transit, income, income_level, income_level_by_stratum, income_level_share, stratum_disposable, stratum_population, stratum_wage_income, industrialization, initial_kt, input_product, input_value, invariant_violations, jobs, kind, labor, labor_force, labor_force_by_level, landmass, legs, living_standard, loss_days, market_totals, markets, minimum_wage, mode, money_total, name, nation, nations, need, opex, opex_factor, opex_revenue, output_product, output_scale, owner, pop, population, ports, price, price_local, produced_today, produced_total, productivity_index, product, production, products, profit, quality, recruit_reserve, remaining_kt, resource, retirees, revenue, satisfaction, savings, scope, service_cash, service_jobs, services, service_sectors, service_businesses, service_bankruptcies, service_exports, service_profit, service_revenue, service_spend, service_wage, service_workers, service_jobs_filled, sector_workers, sector_share, in_kind_food, shortage, simulation_population, site_suitability, sold, stock, stockpile, students, studying, suitability, supply, surplus, tag, taxes, to_city, to_nation, trade_orders, transfers, transit_hours, transport_cash, unemployed, unemployed_by_level, unemployment_by_level, utilization, vacancies_by_level, wage, wage_by_level, wage_income, wage_income_by_level, wage_rigidity, workers, working_age, x, y, host_city, capacity_t_per_day, delay_hours, level, queue, throughput, recipe, inputs, byproducts, qty, capital_per_capacity, last_utilization, strategic, yield, from, to, from_px, to_px, distance_km, load_tons, trips, speed_kmh, batch_capacity_tons, dispatched, cost, needs_level, needs_satisfaction, basic_satisfaction, life_satisfaction, luxury_satisfaction, needs_spend, durable_spend, durable_stock, government_goods_spend, trucks, ships, planes, fleet, fuel, spare_parts, health, fuel_used, missed_no_fleet, missed_no_fuel, source, target_trucks, target_ships, target_planes, currency, currencies, code, rate, regime, trade_balance, fallback, tariff, tariffs, seller, buyer, origin_price, origin_price_anchor, delivered_price, fx_rate, seller_currency, buyer_currency, seller_fx_rate, tariff_revenue;
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
		Array currencies;
		Array tariffs;
		Array service_sectors;
		bool service_sectors_filled = false;
		Array tot_produced, tot_consumed, tot_produced_today, tot_consumed_today;
		// Totais do sistema por produto (soma dos mercados das cidades). A IA
		// lia 858 cidades × 21 mercados em GDScript no frame da virada (~13 ms);
		// o núcleo publica aqui a mesma soma, calculada na thread do tick.
		Dictionary market_totals;
		PackedFloat64Array tot_surplus, tot_sold, tot_demand;
		std::vector<DemoStatusCitySlot> city_slots;
		std::vector<Dictionary> port_slots;
		std::vector<Dictionary> order_slots;
		std::vector<Array> order_leg_slots;
		// Dicionários de perna reaproveitados (1 por perna de pedido): antes
		// cada pedido alocava um Dictionary novo por perna a cada dia.
		std::vector<std::vector<Dictionary>> order_leg_entry;
		std::vector<Dictionary> nation_slots;
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
		// Benchmark sintetico: a aglomeracao e a propria populacao.
		double simulation_population = 0.0;
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
	/** Preenche um Dictionary de perna para o status, reutilizando as chaves. */
	void demo_fill_leg_status(Dictionary& dict, const Leg& leg) const;
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
	void demo_trade_goods();
	// F4: manutenção mensal (peças/atrito) e compra de combustível, peças e
	// veículos pelo governo. Roda depois do reset e antes do comércio.
	void demo_tick_fleet();
	// Deriva a frota inicial de população/PIB (source: derived): veículos,
	// estoque de combustível e de peças. Chamado ao inicializar e ao carregar
	// um save sem o bloco `fleets`.
	void demo_init_fleets();
	// População de simulação e índice de renda (PIB PPP) ponderados por nação.
	double demo_nation_sim_population(const std::string& nation) const;
	double demo_nation_income_index(const std::string& nation) const;
	/**
	 * F4: a nação tem folga alimentar? Soma o estoque de comida (em t-eq de
	 * grão) das cidades e compara com a necessidade diária ×
	 * `kDemoFleetFoodSecureDays`. Sem essa folga, o governo não investe em
	 * frota: o orçamento público fica com a comida.
	 */
	bool demo_nation_food_secure(const std::string& nation) const;
	/**
	 * Frota/combustível necessários para um pedido. Na mesma massa de terra o
	 * trecho é caminhão; entre massas é caminhão → navio → caminhão (os dois
	 * cais). Veículos = ceil(tons / capacidade do ModeSpec) por trecho.
	 */
	DemoTransportDemand demo_route_demand(const DemoCity& seller,
		const DemoCity& buyer, int seller_port, int buyer_port, double tons) const;
	/**
	 * Fração (0..1) do pedido que a frota livre + combustível da nação
	 * comportam hoje. `fuel_limited` indica qual insumo foi o gargalo (para a
	 * fila e o motivo no log/status).
	 */
	double demo_fleet_capacity_scale(const std::string& nation,
		const DemoTransportDemand& demand, bool& fuel_limited) const;
	/** Reserva veículos/fuel para um pedido que está embarcando. */
	void demo_fleet_reserve(const std::string& nation,
		const DemoTransportDemand& demand);
	/** Libera os veículos de um pedido que chegou. */
	void demo_fleet_release(const std::string& nation,
		const DemoTransportDemand& demand);
	/** Custo unitário do frete (tarifa + combustível + uso da frota), por t·km. */
	double demo_leg_unit_freight(TransportMode mode, double distance_km) const;
	/**
	 * F4: custo unitário do frete só com a tarifa, por t·km. Alimentos usam
	 * esta base: a segurança alimentar tem prioridade e o custo de
	 * combustível/frota da logística de comida não é repassado ao comprador
	 * (o consumo físico de fuel continua valendo em `demo_route_demand`).
	 */
	double demo_leg_unit_freight_food(TransportMode mode, double distance_km) const;
	double demo_fleet_fuel_price() const;
	void demo_national_fiscal_transfer();
	const Port* demo_nearest_port(const std::string& nation, const std::string& landmass,
	                              const MapPoint& from) const;
	Port* find_demo_port(const std::string& id);
	/**
	 * Índice da cidade da nação/massa mais próxima do ponto (-1 se nenhuma).
	 * Usado como cidade-sede/cobradora quando `port_of` não aponta para uma
	 * cidade existente (cenário sintético, save antigo).
	 */
	int demo_city_index_near(const std::string& nation, const std::string& landmass,
	                         const MapPoint& from) const;
	/**
	 * Capacidade inicial de cada porto (nível 1) pela população de simulação da
	 * cidade-sede (`port_of`); sem sede conhecida, usa a cidade mais próxima da
	 * mesma nação/massa e, por fim, o piso de referência. Chamado no fim de
	 * initialize_demo_world e de demo_from_dict (save sem portos).
	 */
	void demo_init_port_capacities();
	/** Zera o uso do dia e as métricas de fila de todos os portos. */
	void demo_reset_port_usage();
	/** Aplica obras de expansão de porto cujo prazo venceu neste dia. */
	void demo_tick_port_expansion();
	/**
	 * Compromete `tons` de carga num porto e devolve o atraso em dias (fila).
	 * Acumula `used_today_t`, `queue_t`, `delay_hours` e `demurrage`;
	 * `demurrage_out` recebe o custo de fila a cobrar do comprador.
	 */
	double demo_port_charge(Port& port, double tons, double& demurrage_out);
	int demo_product_index(const String& product) const;
	// Slot da cesta alimentar (0..demo_food_products_.size()-1) -> id de produto.
	int demo_food_product(int slot) const;
	// Id de produto -> fator calórico (0.0 se não for alimento da cesta).
	double demo_food_calories(int product) const;
	const char* demo_product_name(int product) const;
	int demo_recipe_index(const String& recipe_id) const;
	/**
	 * Escolhe a receita de menor custo unitário corrente entre as alternativas
	 * do produto cujos insumos existem na cidade (empate: primeira declarada).
	 * Devolve -1 quando o produto não tem receita ou nenhum insumo disponível.
	 */
	int demo_pick_recipe(const DemoCity& city, int output_product) const;
	static double demo_extraction_efficiency(const DemoDeposit& deposit);
	static double demo_depth_opex_factor(const DemoDeposit& deposit);
	double demo_base_price(int product) const;
	// F5: câmbio, preço entregue e tarifa.
	const DemoCurrency* demo_currency_for(const std::string& nation) const;
	DemoCurrency* demo_currency_for(const std::string& nation);
	double demo_fx_rate(const std::string& nation) const;
	/**
	 * F5: fator de competitividade da âncora (`1` com câmbio fixo; `taxa ÷
	 * taxa-base` no regime float, dentro da banda ±10%). Aplicado ao valor de
	 * oferta/importação de uma nação float — é o efeito de preço rígido na
	 * moeda local, sem alterar o preço interno (âncora).
	 */
	double demo_competitiveness(const std::string& nation) const;
	const char* demo_currency_code(const std::string& nation) const;
	// Tarifa ad valorem efetiva (coringas "*").
	double demo_tariff_rate(const std::string& buyer_nation,
		const std::string& seller_nation, int product) const;
	// Refaz os índices code->moeda e nação->moeda depois de carregar a lista
	// (demo_set_currencies, demo_from_dict).
	void demo_rebuild_currency_indexes();
	// Refaz a tabela `buyer|seller|product` -> taxa (demo_set_tariff,
	// demo_from_dict).
	void demo_rebuild_tariff_lookup();
	// Atualiza as taxas pela balança comercial do dia (moedas float).
	void demo_tick_fx();
	/**
	 * Define os bens e preços-base a partir de dados externos (goods.json).
	 * Cada item deve ter "id" (wheat/iron/...), "base_price" e, opcionalmente,
	 * "order" (0..kDemoMaxProducts-1). A ordem 0..20 dos primários é preservada
	 * e os bens novos entram nas posições seguintes. Pode ser chamado antes ou
	 * depois da inicialização: o estado existente é preservado por índice.
	 */
	void demo_set_goods(const Array& goods);
	/**
	 * Define as receitas industriais (recipes.json): produto, yield, insumos com
	 * quantidade, subprodutos e engenharia (jobs/wage/opex/capital por t/dia).
	 */
	void demo_set_recipes(const Array& recipes);
	/**
	 * Define a cesta alimentar a partir de needs.json: usa
	 * `levels.basic.categories.food.substitutes` e `equivalence.calories`.
	 * Mantém a cesta de 5 culturas quando o dicionário não traz o dado.
	 */
	void demo_set_needs(const Dictionary& needs);
	// F3: zera/redimensiona o estado de necessidades de uma POP (duráveis por
	// produto, vetores dos 3 níveis) sem apagar estoque já acumulado.
	void demo_init_pop_needs(DemoPop& pop) const;
	/**
	 * F7: deriva e (re)inicializa o perfil de trabalho/educação de uma cidade a
	 * partir do índice de renda externa (PIB PPP per capita já normalizado).
	 * É a única fonte de dado por país disponível no cenário; a tabela de
	 * faixas é uma ESTIMATIVA declarada (ordem de grandeza ILO/World Bank,
	 * Barro-Lee/UNDP) e está documentada em economy_sim.cpp. Não mexe em
	 * população nem em caixa.
	 */
	void demo_apply_labor_profile(DemoCity& city);
	/**
	 * F7 calibração: recalcula a população de cada estrato de renda
	 * (escolaridade/ocupação) a partir da distribuição de educação. Inclui os
	 * dependentes: a escolaridade é medida na idade ativa, mas a renda do
	 * estrato sustenta toda a sua população.
	 */
	void demo_refresh_strata(DemoPop& pop) const;
	/**
	 * F7: escolaridade das vagas por setor industrial (fallback quando a receita
	 * não declara `education_mix`). Tabela documentada; escreve 3 frações.
	 */
	static void demo_education_mix_for_sector(const std::string& sector,
		double* mix);
	/**
	 * F7: fecha o dia do mercado de trabalho de uma cidade — qualificação
	 * (estudantes entram/saem da força), emprego por nível, folha de pagamento e
	 * salários dinâmicos. Devolve a receita de salários efetivamente paga.
	 */
	double demo_labor_day(DemoCity& city);

	/**
	 * F9 — emprego setorial: decompõe o emprego do dia em agropecuária,
	 * indústria/mineração e serviços (residual), com a informalidade como
	 * sobreposição. A agricultura vem da produção local (t/dia) e da TFP; a
	 * indústria da âncora de renda (ILO) e do valor adicionado simulado.
	 * Roda no fim de demo_labor_day (emprego já conhecido).
	 */
	void demo_update_sector_employment(DemoCity& city);
	/** F9: âncoras ILO interpoladas no log do índice de renda. */
	void demo_sector_anchor_values(double income, double* out) const;

	// F8: índices cidade→negócios e setor→negócios (como os de construção).
	void demo_rebuild_service_indexes();
	// F8: semeia os negócios iniciais de cada cidade (capital da poupança da
	// POP dona). Roda no boot do mundo e ao carregar save anterior ao v10.
	void demo_seed_services();
	// F8: fecha o dia dos serviços de uma cidade — demanda das classes, B2B,
	// governo, exportação, produção dos negócios, preço, entrada e saída.
	void demo_services_day(DemoCity& city);
	// F8: abre um negócio do setor com a capacidade pedida. `capital` sai da
	// poupança da POP e entra em service_cash (conservação).
	bool demo_service_open(DemoCity& city, int sector, double capacity,
		double capital);
	// F8: fecha um negócio insolvente, devolvendo o capital residual à POP.
	void demo_service_close(DemoServiceBusiness& business);
	int demo_service_sector_index(const std::string& sector) const;

	/**
	 * F7 — custo diário da cesta completa de um nível de necessidade aos preços
	 * que a cidade pratica, por substituto não-durável mais barato (preço /
	 * equivalência). Duráveis ficam fora: são comprados com poupança e sua
	 * intensidade é a reposição anual, não um fluxo diário de renda.
	 */
	double demo_level_basket_cost(const DemoCity& city, int level_index) const;
	/**
	 * F7 comida — custo EFETIVO da necessidade alimentar do dia: os preços dos
	 * alimentos que a cidade REALMENTE tem em estoque (na mesma proporção de
	 * disponibilidade usada na compra) mais o resíduo importado ao CIF
	 * conhecido (`import_price`) ou, sem ele, ao preço local. O preço do
	 * alimento mais barato do mundo (pão processado sem estoque local)
	 * subestimava o subsídio de subsistência das POPs.
	 *
	 * `local_cost` = custo da parcela coberta pelo estoque local;
	 * `import_cost` = custo do que falta e terá de ser importado;
	 * `per_ge` = (local_cost + import_cost) / need, ou o preço mais barato por
	 * caloria quando não há necessidade. Sem alocação: uma passada por produto.
	 */
	void demo_food_basket_cost(const DemoCity& city, double need,
		double& local_cost, double& import_cost, double& per_ge) const;
	/**
	 * F7: atualiza os salários por nível depois do lucro do dia (mercado
	 * apertado/folgado, margem das firmas, âncora de renda do país, piso de
	 * salário mínimo e rigidez). Chamado na fase de contabilidade.
	 */
	void demo_update_wages(DemoCity& city, double building_margin);
	/**
	 * F7 calibração: TFP efetiva da cidade — o fator explícito do laboratório
	 * quando declarado (> 0) ou o FATOR COMPLETO derivado do PIB PPP per capita
	 * real (`clamp(external_income_index, 0,25, 4,0)`, `derived:gdp_ppp`,
	 * normalizado para a média mundial = 1,0). É o nível de produtividade do
	 * país, não um prêmio: países abaixo da média recebem fator < 1. O piso de
	 * subsistência da âncora salarial (demo_update_wages) impede que o fator
	 * < 1 derrube o trabalhador abaixo da cesta alimentar.
	 * EUA ~3,5, DEU ~2,9, BRA ~0,9, IND ~0,46, NGA ~0,37.
	 */
	double demo_effective_productivity(const DemoCity& city) const;
	/**
	 * F7 calibração — declara o Gini de renda da nação (0..1). 0 desliga a
	 * divisão por classes e mantém os estratos de escolaridade.
	 */
	void demo_set_income_gini(const String& city_id, double gini);
	/**
	 * F7 calibração — recalcula as classes de consumo (população e teto) a
	 * partir do Gini declarado e do custo da cesta básica do dia. Sem Gini,
	 * espelha os estratos de escolaridade (`stratum_*`).
	 */
	void demo_refresh_income_classes(DemoCity& city) const;
	/**
	 * Consome uma categoria de necessidade da POP. `want_ge` é a cesta do dia em
	 * toneladas-equivalentes; devolve o consumido em t-eq e debita o orçamento.
	 * Percorre os substitutos na ordem declarada (do melhor ao último caso),
	 * registrando a demanda mesmo quando não há estoque (sinal para a IA).
	 * A cesta alimentar básica mantém o laço próprio validado (proporcional à
	 * disponibilidade) e não passa por aqui.
	 */
	double demo_consume_category(DemoCity& city, const DemoNeedsCategory& category,
		double want_ge, double& budget, double& spend);
	// F3/F7: substituto preferido de uma categoria (primeiro com preço), usado
	// para registrar o desejo de mercado; e o registro do desejo em si.
	bool demo_category_top_market(DemoCity& city, const DemoNeedsCategory& category,
		DemoMarket*& market, double& factor, double& durable_scale);
	void demo_register_category_demand(DemoCity& city,
		const DemoNeedsCategory& category, double want_ge);
	// F3: compra institucional do governo (paper/computer) no dia.
	void demo_consume_government(DemoCity& city);
	/**
	 * Bootstrap industrial (§12.3): no cenário real, semeia uma planta
	 * estratégica de referência (moinho + padaria) em poucas capitais quando o
	 * mundo ainda não tem nenhuma construção secundária. Nota: é uma
	 * ESTIMATIVA de transição, não dado observado; run once, marcada no status.
	 */
	void demo_seed_industry();
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
	// F7: rigidez salarial (fração máxima de ajuste por dia) e piso de salário
	// mínimo por nação (0 = desligado). Ganchos de política.
	double demo_wage_rigidity_ = 0.01;
	// F7 calibração: folga de custo de vida não modelado no gate de renda →
	// nível de consumo. A cesta de needs.json cobre só os bens comerciáveis;
	// moradia/energia/serviços não têm preço no núcleo, então o nível exige uma
	// folga. Sem ela qualquer renda acima da cesta liberava luxury no mundo
	// inteiro (832/834 cidades). Calibrada no economy_world_test.
	double demo_income_basket_margin_ = 2.30;
	std::unordered_map<std::string, double> demo_minimum_wages_;
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
	// F8: catálogo de serviços e negócios (um vetor global; índices por cidade
	// e por setor são reconstruídos quando o conjunto muda).
	bool demo_services_loaded_ = false;
	double demo_service_government_share_ = 0.002;
	// F9 — emprego setorial. Âncoras (renda, agro, indústria, informalidade)
	// ordenadas por renda; interpoladas no log da renda. Ver
	// data/definitions/economy/sector_employment.json (fonte ILO/ILOSTAT).
	struct DemoSectorAnchor {
		double income = 1.0;
		double agriculture = 0.0;
		double industry = 0.0;
		double informal = 0.0;
	};
	bool demo_sector_loaded_ = false;
	std::vector<DemoSectorAnchor> demo_sector_anchors_;
	// Demanda de trabalho agrícola: t/dia × base / TFP^expoente.
	double demo_agri_labor_per_ton_ = 55.0;
	double demo_agri_labor_exponent_ = 1.4486;
	double demo_agri_production_band_min_ = 0.9;
	double demo_agri_production_band_max_ = 1.1;
	// Autoconsumo máximo das famílias agrícolas, como fração da produção
	// local disponível — o resto da produção vai ao mercado.
	double demo_agri_own_consumption_share_ = 0.5;
	// Indústria: trabalhadores por unidade de valor adicionado (XGS/dia).
	double demo_industry_labor_per_va_ = 3.0;
	double demo_industry_labor_exponent_ = 1.4486;
	std::vector<DemoServiceSector> demo_service_sectors_;
	std::vector<DemoServiceBusiness> demo_service_businesses_;
	std::vector<std::vector<int>> demo_service_by_city_;
	std::vector<std::vector<int>> demo_service_by_sector_;
	int demo_next_service_business_id_ = 1;
	int demo_service_bankruptcies_ = 0;
	// Índices derivados do mundo demo (ver demo_rebuild_indexes).
	std::unordered_map<std::string, int> demo_city_lookup_;
	std::vector<std::vector<int>> demo_buildings_by_city_;
	// F4: cidades por nação (ordem de índice) para a frota/compra do governo.
	std::unordered_map<std::string, std::vector<int>> demo_cities_by_nation_;
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
	double demo_produced_[kDemoMaxProducts] = {};
	double demo_consumed_total_[kDemoMaxProducts] = {};
	double demo_produced_today_[kDemoMaxProducts] = {};
	double demo_consumed_today_[kDemoMaxProducts] = {};
	// Produtos ativos (demo_set_goods). 21 por padrão (cenário de validação).
	int demo_products_ = 21;
	std::vector<std::string> demo_product_names_;
	std::unordered_map<std::string, int> demo_product_lookup_;
	// Receitas industriais e índice por id (recipes.json).
	std::vector<DemoRecipe> demo_recipes_;
	std::unordered_map<std::string, int> demo_recipe_lookup_;
	// Cesta alimentar data-driven (needs.json); default = 5 culturas validadas.
	std::vector<int> demo_food_products_;
	std::vector<double> demo_food_factors_;
	// F3: tabela de necessidades por nível (needs.json), substitutos,
	// equivalência e consumo institucional do governo. `demo_needs_loaded_`
	// falso = comportamento de 1 nível (cenário AGRIA/PORTUS e saves antigos).
	std::vector<DemoNeedsLevel> demo_needs_levels_;
	bool demo_needs_loaded_ = false;
	int demo_needs_food_level_ = -1;
	// Decaimento diário que perde exatamente durable_replacement_per_year em
	// 365 dias: 1 - (1 - r)^(1/365).
	double demo_durable_decay_ = 0.0;
	std::vector<char> demo_product_durable_;  // por produto (goods.json durable)
	std::vector<int> demo_government_products_;
	std::vector<double> demo_government_per_capita_day_;
	// Bootstrap industrial já executado neste mundo.
	bool demo_industry_seeded_ = false;
	// F4: frota por nação (ordem determinística em demo_fleet_order_) e índices
	// dos bens da frota (fuel/truck/cargo_ship/cargo_plane/spare_parts).
	std::unordered_map<std::string, DemoFleet> demo_fleets_;
	std::vector<std::string> demo_fleet_order_;
	int demo_fleet_product_fuel_ = -1;
	int demo_fleet_product_truck_ = -1;
	int demo_fleet_product_ship_ = -1;
	int demo_fleet_product_plane_ = -1;
	int demo_fleet_product_parts_ = -1;
	// F5: moedas por code/nacao, ordem determinística e tarifas declaradas.
	std::vector<DemoCurrency> demo_currencies_;
	std::unordered_map<std::string, int> demo_currency_by_code_;
	std::unordered_map<std::string, int> demo_nation_currency_;
	std::vector<DemoTariff> demo_tariffs_;
	std::unordered_map<std::string, double> demo_tariff_lookup_;
	// Preços-base dos bens (data-driven: demo_set_goods/initialize_demo_world).
	double demo_base_prices_[kDemoMaxProducts] = {
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
	// Sobe quando uma jazida nova é revelada (demo_set_deposit/save). Separado
	// da versão estática para uma descoberta não forçar a reescrita de
	// aptidão/demanda externa de todas as cidades (~190 mil escritas/dia).
	int demo_status_deposit_version_ = 0;
	// Retrato do tick reaproveitado entre dias (ver demo_tick_core).
	DemoSnapshot demo_snapshot_buffer_;
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
