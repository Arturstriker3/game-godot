#pragma once
// Núcleo da simulação econômica (S5b do docs/economy/core_refactor_plan.md).
// C++ puro (só STL, via world_state.h/sim_types.h) — nada de godot_cpp.
//
// A classe `ogs::SimCore` recebe o `WorldState&` na construção e executa a
// ordem do dia: um prólogo (retrato, dinheiro, dia, obras, índices, preços de
// abertura) seguido de uma fase por etapa econômica. A fachada `EconomySim`
// (economy_sim.*) guarda UM `WorldState ws_` e UM `SimCore sim_core_{ws_}`,
// converte Godot <-> sim e loga os invariantes (o núcleo não emite push_error).
//
// S5b é fatiado: a cada commit um grupo de fases migra de `EconomySim` para
// cá. As fases que ainda não migraram continuam inline no `tick_core`.

#include "sim_labor.h"
#include "thread_pool.h"
#include "world_state.h"

#include <memory>

namespace ogs {

// Token seguro para id determinístico (letras/números; o resto vira '-').
inline std::string sane_token(const std::string& text) {
	std::string out;
	out.reserve(text.size());
	for (const char c : text) {
		const unsigned char u = static_cast<unsigned char>(c);
		const bool ok = (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z')
			|| (u >= '0' && u <= '9');
		out.push_back(ok ? c : '-');
	}
	while (!out.empty() && out.back() == '-') {
		out.pop_back();
	}
	return out.empty() ? std::string("X") : out;
}

// S16 — buffer de redução por cidade para as fases de renda/consumo. As
// contribuições de cada cidade aos acumuladores globais (`foreign_cash`,
// `consumed_total`, `consumed_today`) são gravadas em slots disjuntos
// (índice da cidade × produto) e reduzidas em ordem crescente de cidade numa
// passada serial — reproduz a ordem de soma do loop serial original.
struct CityAccumScratch {
	std::vector<double> foreign;          // por cidade (saídas p/ o exterior)
	std::vector<double> consumed_total;   // cidade × produto
	std::vector<double> consumed_today;   // cidade × produto
	std::size_t n_products = 0;
};

class SimCore {
public:
	explicit SimCore(WorldState& ws);

	// ------------------------------------------------------------------
	// S16 — paralelismo determinístico por cidade.
	// ------------------------------------------------------------------
	// Define o número de threads das fases por cidade: 1 = serial (default);
	// <= 0 = automático (min(núcleos - 2, 8)); > 1 = esse número. As fases
	// globais (trade, fiscal, câmbio, invariantes) seguem seriais.
	void set_sim_threads(int threads);
	int sim_threads() const { return sim_threads_; }

	// ------------------------------------------------------------------
	// Fases da ordem do dia (crescem a cada fatia de S5b).
	// ------------------------------------------------------------------
	// Prólogo: retrato para reversão, dinheiro antes, avanço do dia, obras de
	// porto, remoção de falidos/esgotados, índices e preços de abertura.
	void prologue();
	// 1. Chegada das cargas pedidas no dia anterior.
	void phase_arrival();
	// 2. Zera os fluxos do dia.
	void phase_reset();
	// 2b. F4: manutenção/compra mensal da frota e zera a fila do dia. Roda
	// antes do comércio para combustível/veículos comprados já valerem hoje.
	void phase_fleet();
	// 3. Mercado de trabalho e salários.
	void phase_labor();
	// 4. Produção: cada construção declara insumo e produto.
	void phase_production();
	// 4c. Recursos brutos (minerais, soja, algodão e madeira): exportação.
	void phase_raw();
	// 5. Renda das famílias, impostos, transferências e consumo alimentar.
	void phase_income();
	// 5b. §19 W3: mercado de moradia (aluguel, construção e aperto). Roda
	// depois da comida (a POP já comprou a cesta) e antes de serviços/comércio:
	// o aluguel só usa a poupança acima da reserva de comida.
	void phase_housing();
	// F8. Serviços: demanda das classes, B2B, governo, exportação, produção dos
	// negócios, preço, entrada e saída — uma cidade por vez.
	void phase_services();
	// 6. Comércio de alimento (cesta de calorias) e dos demais bens.
	void phase_trade();
	// 7. Receita por construção, lucro, distribuição de capital e PIB.
	void phase_accounting();
	// 8. Preços reagem a estoque, oferta, demanda e custo de produção.
	void phase_prices();
	// 9. Limite diário único por mercado (depois de todas as regras de preço).
	void phase_clamp();
	// 9b. Redistribuição fiscal nacional + dividendos/repatriação dos grupos.
	void phase_fiscal();
	// 9c. Câmbio dinâmico: a balança comercial do dia move as moedas float.
	void phase_fx();
	// 10. Invariantes: se o tick corrompeu o estado, reverte o retrato.
	void phase_invariants();

	// ------------------------------------------------------------------
	// Helpers compartilhados com a fachada (públicos).
	// ------------------------------------------------------------------
	City* find_city(const std::string& id);
	const City* find_city(const std::string& id) const;
	// Teto físico de capacidade agrícola (t/dia) de uma cidade para um produto.
	// Dado de GEOGRAFIA: não é serializado no save — chega do GeographyService
	// e viaja no bloco `definitions` do snapshot. Produto fora de
	// [0, ws_.products) ou cidade ausente é ignorado; valor é limitado a >= 0.
	void set_land_capacity(const std::string& city_id, int product, double value);
	// A2: `full=false` é o caminho de todo tick (só o índice por cidade;
	// cidades/nações/tarifas só mudam no carregamento).
	void rebuild_indexes(bool full = true);
	double money_total() const;
	void init_pop_needs(Pop& pop) const;
	static double extraction_efficiency(const Deposit& deposit);
	double base_price(int product) const;
	int food_product(int slot) const;
	double food_calories(int product) const;
	// Multiplicador do opex pela profundidade da jazida (1.0 sem reserva).
	static double depth_opex_factor(const Deposit& deposit);
	// F7 calibração: TFP efetiva da cidade (fator explícito ou derivado do
	// PIB PPP per capita real).
	double effective_productivity(const City& city) const;
	// F7: escolaridade das vagas por setor industrial (fallback da receita).
	static void education_mix_for_sector(const std::string& sector,
		double* mix);
	// F9: âncoras ILO interpoladas no log do índice de renda.
	void sector_anchor_values(double income, double* out) const;
	// F9: decompõe o emprego do dia em agro/indústria/serviços + informalidade.
	void update_sector_employment(City& city);
	// F7 calibração: recalcula os estratos de renda a partir da escolaridade.
	void refresh_strata(Pop& pop) const;
	// F7: fecha o dia do mercado de trabalho de uma cidade.
	double labor_day(City& city);
	// F7 — custo diário da cesta completa de um nível de necessidade aos preços
	// que a cidade pratica, por substituto não-durável mais barato.
	double level_basket_cost(const City& city, int level_index) const;
	// F7 comida — custo efetivo da necessidade alimentar do dia: estoque local
	// na proporção da compra mais o resíduo ao CIF conhecido.
	void food_basket_cost(const City& city, double need,
		double& local_cost, double& import_cost, double& per_ge) const;
	// F3/F7: substituto preferido da categoria (sinal de desejo no mercado).
	bool category_top_market(City& city, const NeedsCategory& category,
		Market*& market, double& factor, double& durable_scale);
	// F3: registra o desejo da categoria sem comprar (sinal de escassez).
	void register_category_demand(City& city,
		const NeedsCategory& category, double want_ge);
	// F3: consome uma categoria de necessidade e debita o orçamento do dia.
	// S16: grava as compras no scratch de redução por cidade (não no acumulador
	// global) para a fase de renda rodar paralela e determinística.
	double consume_category(City& city, const NeedsCategory& category,
		double want_ge, double& budget, double& spend,
		CityAccumScratch& scratch, std::size_t city_index);
	// F3: compra institucional do governo (paper/computer) no dia.
	void consume_government(City& city,
		CityAccumScratch& scratch, std::size_t city_index);
	// F7 calibração: classes de consumo a partir do Gini e da cesta básica.
	void refresh_income_classes(City& city) const;
	// F5: câmbio/tarifas consultados pela fase de serviços.
	const Currency* currency_for(const std::string& nation) const;
	double competitiveness(const std::string& nation) const;
	double fx_rate(const std::string& nation) const;
	const char* currency_code(const std::string& nation) const;
	// Refaz os índices code->moeda e nação->moeda depois de carregar a lista.
	void rebuild_currency_indexes();
	// Atualiza as taxas pela balança comercial do dia (moedas float).
	void tick_fx();
	double tariff_rate(const std::string& buyer_nation,
		const std::string& seller_nation, int product) const;
	// A2: mesma tarifa resolvida por índices de nação (caminho quente do
	// comércio) e o caminho textual original (fallback quando alguma linha não
	// é resolvível por índice).
	double tariff_rate_at_nation(int buyer_nation, int seller_nation,
		int product) const;
	double tariff_rate_text(const std::string& buyer_nation,
		const std::string& seller_nation, int product) const;
	// A2: tarifa no laço quente — índices quando a tabela está completa, senão
	// o caminho textual (mesmo resultado sempre).
	double tariff_rate_cities(const City& buyer_city, const City& seller_city,
		int product) const;
	int product_index(const std::string& product) const;
	const char* product_name(int product) const;
	// F8: serviços.
	int service_sector_index(const std::string& sector) const;
	void rebuild_service_indexes();
	double city_food_reserve(const City& city) const;
	bool service_open(City& city, int sector, double capacity,
		double capital);
	void service_close(ServiceBusiness& business);
	void seed_services();

	// ------------------------------------------------------------------
	// S6b — helpers de load/setup movidos da fachada.
	// ------------------------------------------------------------------
	// Refaz a tabela `buyer|seller|product` -> taxa (set_tariff, save).
	void rebuild_tariff_lookup();
	// F7: preenche população, escolaridade, participação e salários iniciais.
	void apply_labor_profile(City& city);
	// F9: semeia os grupos a partir dos ativos privados (idempotente).
	void seed_groups();
	// Id de receita -> índice no `recipe_lookup` (-1 se não existir).
	int recipe_index(const std::string& recipe_id) const;

	// ------------------------------------------------------------------
	// §15 — construção como projeto físico (sim/construction.cpp).
	// ------------------------------------------------------------------
	// Calcula materiais/prazo e empilha a obra. Chamado pela fachada (build,
	// grupo e expansão). `expand_existing` soma capacidade a um prédio existente
	// em vez de criar um novo.
	void add_construction_project(ConstructionProject& project);
	// Parcela de TERRA de um projeto (XGS): land_price da zona × área que a
	// obra ocupa. HOUSING: unidades × m²/unidade do padrão (ZONE_RES);
	// BUILDING urbano (receita/insumo declarado) e expansão: vagas ×
	// kUrbanFactoryM2PerWorker (ZONE_IND), limitada a
	// kConstructionLandShareMax do orçamento; fazenda/mina (solo rural,
	// sem zona) e URBAN_INFRA (a obra É a abertura do solo) = 0. Sem preço de
	// terra na cidade/zona devolve 0 (ausência explícita).
	double project_land_cost(const ConstructionProject& project) const;
	// Avança as obras do dia: materiais comprados do mercado, capacidade do
	// setor de construção, pagamento incremental do dono e conclusão.
	void tick_construction();
	// Índice de uma obra em andamento pelo id do prédio final (-1 se não há).
	int project_index(const std::string& building_id) const;
	// Marca a obra `strategic` (bootstrap industrial) pelo id do prédio final.
	void mark_project_strategic(const std::string& building_id);

	// ------------------------------------------------------------------
	// Geometria do mapa e frete (S5b).
	// ------------------------------------------------------------------
	// Capacidade/custo/tempo de cada modal (calibragem do porto).
	static const ModeSpec& spec(TransportMode mode);
	static const char* scope_name(MarketScope scope);
	/** Cria um trecho e calcula viagens, tempo e custo. */
	Leg make_leg(TransportMode mode, const std::string& from, const std::string& to,
		const MapPoint& from_point, const MapPoint& to_point, double load_tons) const;
	double distance_km(const MapPoint& a, const MapPoint& b) const;

	// ------------------------------------------------------------------
	// Porto (S5b): capacidade, fila e cobrança.
	// ------------------------------------------------------------------
	const Port* nearest_port(const std::string& nation,
		const std::string& landmass, const MapPoint& from) const;
	Port* find_port(const std::string& id);
	int city_index_near(const std::string& nation,
		const std::string& landmass, const MapPoint& from) const;
	void init_port_capacities();
	void reset_port_usage();
	double port_charge(Port& port, double tons, double& demurrage_out);

	// ------------------------------------------------------------------
	// Frota nacional (F4 / S5b).
	// ------------------------------------------------------------------
	void init_fleets();
	double nation_sim_population(const std::string& nation) const;
	double nation_income_index(const std::string& nation) const;
	bool nation_food_secure(const std::string& nation) const;
	TransportDemand route_demand(const City& seller,
		const City& buyer, int seller_port, int buyer_port, double tons) const;
	double fleet_capacity_scale(const std::string& nation,
		const TransportDemand& demand, bool& fuel_limited) const;
	void fleet_reserve(const std::string& nation,
		const TransportDemand& demand);
	double fleet_fuel_price() const;
	double leg_unit_freight(TransportMode mode,
		double distance_km_value) const;
	double leg_unit_freight_food(TransportMode mode,
		double distance_km_value) const;
	// Compra de frota: a fachada faz o parse de String e as validações de erro;
	// o núcleo compra no mercado interno da nação e credita a frota.
	double fleet_buy_core(const std::string& nation, int product_index,
		double amount, double budget_share, double& spent_out,
		double& unfilled_out);

	// F7: atualiza os salários por nível depois do lucro do dia (mercado
	// apertado/folgado, margem das firmas, âncora de renda do país, piso de
	// salário mínimo e rigidez). Chamado na fase de contabilidade.
	void update_wages(City& city, double building_margin);
	// F9: propriedade e grupos.
	int group_index(const std::string& id) const;
	Group* find_group(const std::string& id);
	const Group* find_group(const std::string& id) const;
	// Decompõe o dono de uma construção: kind 0 state, 1 national, 2 foreign,
	// 3 private (legado). Preenche grupo e nação de origem.
	int owner_kind(const std::string& owner, std::string* group_id,
		std::string* origin_nation) const;
	int fdi_mode(const std::string& nation, const std::string& sector) const;
	double sector_tax_holiday(const std::string& nation,
		const std::string& sector) const;
	double sector_subsidy(const std::string& nation,
		const std::string& sector) const;
	double nation_confidence(const std::string& nation) const;
	// Credita `amount` no capital das famílias da nação, ponderado pelo estrato
	// dono (topo), e devolve o total efetivamente creditado.
	double credit_nation_households(const std::string& nation, double amount);
	// Imposto corporativo + retenção: move o lucro do grupo da cidade para o
	// caixa dele (ou para o orçamento, se estatal). Chamado na contabilidade.
	void route_group_profit(int group_index, const std::string& host_nation,
		double profit, double revenue, City& city, double& firm_surplus);

	// Dinheiro antes do tick (para a fase de invariantes, ainda na fachada).
	double money_before() const { return money_before_; }
	// Preços de abertura (para a fase de clamp, ainda na fachada).
	const std::vector<std::vector<double>>& open_prices() const { return open_prices_; }

	// Invariantes rígidos do tick: devolve a lista de violações (vazia se o
	// estado está sadio). A fachada é quem loga (`push_error`).
	std::vector<std::string> check_invariants(double money_before) const;
	// Aplica um retrato salvo, revertendo o tick corrompido.
	void restore_state(const Snapshot& in);

private:
	WorldState& ws_;
	double money_before_ = 0.0;
	// S16: número de threads configurado. 0 = automático (default,
	// min(núcleos-2, 8)); > 0 = esse número (1 = serial). O pool persistente é
	// criado sob demanda com o número efetivo de threads. Nunca usado fora do
	// tick (o writer já segura o mutex da fachada).
	int sim_threads_ = 0;
	std::unique_ptr<ThreadPool> pool_;
	// Número efetivo de threads (resolve o modo automático).
	int effective_threads() const;
	// Devolve o pool atual, recriando-o se o número de threads mudou.
	ThreadPool& pool();
	// S16: simula o dia de serviços de uma cidade (passos 1-8, sem abrir/fechar
	// negócios); grava a saída de divisas da exportação em foreign_slots[sector]
	// e a poupança acima da reserva de comida (calculada NO TOPO do dia) em
	// savings_surplus_out para a 2ª passada.
	void services_day(City& city, double* foreign_slots,
		double& savings_surplus_out);
	// S16: abertura/fechamento de negócios em 2ª passada serial (ordem fixa
	// cidade -> setor), usando o savings_surplus do TOPO do dia (o serial lia a
	// reserva de comida antes de o passo 6 reduzir estoques — bit-idêntico).
	void services_open_close(City& city, double savings_surplus);
	// Preço de abertura por cidade/produto, capturado no prólogo e usado pela
	// fase de clamp (limite diário único). Reaproveitado entre dias.
	std::vector<std::vector<double>> open_prices_;

	// Retrato do estado para reverter o tick se um invariante rígido falhar.
	void snapshot_state(Snapshot& out) const;
	// Aplica obras de expansão de porto cujo prazo venceu neste dia.
	void tick_port_expansion();
	// Libera os veículos de um pedido que chegou.
	void fleet_release(const std::string& nation,
		const TransportDemand& demand);
	// Escolhe a receita de menor custo unitário corrente para um produto.
	int pick_recipe(const City& city, int output_product) const;
	// 6. Comércio de alimento (cesta de calorias) entre cidades.
	void trade_food();
	// 6b. Comércio dos demais bens (insumos industriais), pela mesma malha.
	void trade_goods();
};

} // namespace ogs
