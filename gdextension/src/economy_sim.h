#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <string>
#include <vector>

namespace godot {

/**
 * Núcleo econômico do Projeto B, exposto ao Godot via GDExtension.
 *
 * Hierarquia: uma nação agrega vários estados. Cada estado tem população,
 * produção e demanda próprias, então a necessidade de trigo é regional e o
 * comércio acontece entre estados (inclusive dentro do mesmo país).
 *
 * O mercado é resolvido em três camadas, sempre nesta ordem: regional (estados
 * vizinhos do mesmo país), nacional (resto do país) e internacional. Mesmo
 * dentro do país a mercadoria é transportada do estado vendedor até o estado
 * que a consome.
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

	/** Cria os estados das três nações e zera o relógio de simulação. */
	void initialize();

	/** Avança um dia de simulação e devolve o resumo do dia. */
	Dictionary tick();

	/** Estado atual (estoques, população, trânsito) sem avançar o tempo. */
	Dictionary get_status() const;

protected:
	static void _bind_methods();

private:
	enum class TransportMode { Truck, Ship, Plane };

	// Camadas de mercado, resolvidas nesta ordem: o estado compra primeiro de
	// vizinhos do próprio país, depois do resto do país e por último no exterior.
	enum class MarketScope { Regional, National, International };

	struct ModeSpec {
		TransportMode mode;
		const char* name;
		double speed_kmh;
		double batch_capacity_tons;
		double cost_per_ton_km;
	};

	// Coordenadas no grid do mapa (5000x2500).
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
		std::string to_state;
		double amount;
		double arrival_hour;
	};

	struct State {
		std::string id;
		std::string nation;
		std::string name;
		double population;
		double production;
		double stockpile;
		MapPoint center;
		// Valores por dia, recalculados a cada tick.
		double need = 0.0;
		double consumed = 0.0;
		double deficit = 0.0;
		double dispatched = 0.0;
	};

	struct TradeOrder {
		std::string from_state;
		std::string to_state;
		std::string from_nation;
		std::string to_nation;
		MarketScope scope = MarketScope::Regional;
		double amount = 0.0;
		double transit_hours = 0.0;
		double freight_cost = 0.0;
		std::vector<Leg> legs;
	};

	static const ModeSpec& spec(TransportMode mode);
	static const char* scope_name(MarketScope scope);

	/** Popula os estados das três nações. */
	void define_states();

	State* find_state(const std::string& id);
	const State* find_state(const std::string& id) const;

	/**
	 * Monta o plano de transporte entre dois estados.
	 * @param seller Estado vendedor.
	 * @param buyer Estado comprador.
	 * @param amount Carga total em toneladas.
	 * @param air_share Fração que segue por avião.
	 */
	std::vector<Leg> build_route(const State& seller, const State& buyer, double amount,
	                             double air_share) const;

	/** Cria um trecho e calcula viagens, tempo e custo. */
	Leg make_leg(TransportMode mode, const std::string& from, const std::string& to,
	             const MapPoint& from_point, const MapPoint& to_point, double load_tons) const;

	/**
	 * Abastece um estado comprador a partir dos vendedores disponíveis.
	 * @param buyer Estado com déficit regional.
	 * @param remaining Déficit ainda sem cobertura; é reduzido conforme o pedido é criado.
	 * @param sellers Vendedores com excedente.
	 * @param scope Camada de mercado que pode ser usada neste passo.
	 * @returns true se um pedido foi criado.
	 */
	bool dispatch_trade(State& buyer, double& remaining, const std::vector<State*>& sellers,
	                    MarketScope scope);

	/** Indica se o vendedor pertence à camada de mercado pedida. */
	bool in_scope(const State& buyer, const State& seller, MarketScope scope) const;

	/** Rota marítima entre a Europa continental e a Inglaterra. */
	std::vector<Leg> build_overseas_route(const State& seller, const State& buyer, double amount) const;

	double distance_km(const MapPoint& a, const MapPoint& b) const;
	double in_transit_total(const std::string& state_id) const;

	Dictionary state_to_dict(const State& state) const;
	Dictionary nation_to_dict(const std::string& tag) const;
	Dictionary leg_to_dict(const Leg& leg) const;
	Dictionary order_to_dict(const TradeOrder& order) const;

	std::vector<State> states_;
	std::vector<Shipment> in_transit_;
	std::vector<TradeOrder> last_orders_;
	int day_ = 0;
	double clock_hours_ = 0.0;

	// Portos usados nas rotas internacionais (Calais e Dover).
	MapPoint france_port_{2525.7, 542.4};
	MapPoint england_port_{2518.2, 539.8};
};

} // namespace godot
