#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>

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

	/** Cria as cidades das nações e zera o relógio de simulação. */
	void initialize();

	/** Avança um dia de simulação e devolve o resumo do dia. */
	Dictionary tick();

	/** Estado atual (estoques, população, trânsito) sem avançar o tempo. */
	Dictionary get_status() const;

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
		// Indicadores derivados, lidos pelo crescimento urbano visual.
		double jobs = 0.0;
		double income = 0.0;
		double industrialization = 0.0;
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

	static const ModeSpec& spec(TransportMode mode);
	static const char* scope_name(MarketScope scope);

	/** Popula as cidades das nações. */
	void define_cities();

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

	Dictionary city_to_dict(const City& city) const;
	Dictionary nation_to_dict(const std::string& tag) const;
	Dictionary leg_to_dict(const Leg& leg) const;
	Dictionary order_to_dict(const TradeOrder& order) const;

	std::vector<City> cities_;
	std::vector<Shipment> in_transit_;
	std::vector<TradeOrder> last_orders_;
	int day_ = 0;
	double clock_hours_ = 0.0;

	// Portos usados nas rotas internacionais (Calais e Dover).
	MapPoint france_port_{2525.7, 542.4};
	MapPoint england_port_{2518.2, 539.8};
};

} // namespace godot
