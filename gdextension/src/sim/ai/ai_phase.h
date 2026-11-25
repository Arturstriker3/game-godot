#pragma once
// A5 — FASE DA IA ECONÔMICA EM C++ (infraestrutura).
//
// Esta parte entrega o encanamento da IA em C++ puro, sem as regras dos
// investidores:
//
//   1. `AiView` (ai_view.h) — retrato congelado do dia: mercados, POP,
//      construções, jazidas, obras, incumbentes, totais por produto e ranking
//      de grupos;
//   2. `AiContext`/`AiState` — o que uma regra precisa para decidir (mundo,
//      núcleo, fila de comandos, templates, view e o estado NÃO serializado que
//      vive entre dias);
//   3. `ai_emit_*` — emite o `Command` no formato do A3, retira SÓ ele da fila
//      (`take_by_seq`) e aplica a ação do núcleo (`action_invest`,
//      `action_build`, `action_group_invest`), registrando no log com o dia de
//      aplicação;
//   4. `AiPhase::run` — o agendador: fotografa o dia, garante os templates,
//      chama `AiGroupInvestor::run` (grupos, na ordem do ranking) e depois
//      `private` e `government` nas cidades da vez (`day % 7 == índice % 7`,
//      como o `economic_ai.gd`).
//
// As regras vivem nos módulos dos investidores: `AiGroupInvestor` (ai_group.h,
// F9), `AiPrivateInvestor` (ai_private.h) e `AiGovernment` (ai_government.h).
// A fachada (`EconomySim`) só chama `AiPhase::run` com `set_ai_enabled(true)`
// (DESLIGADO por padrão: os testes e o caminho GDScript existentes não mudam);
// o CLI headless roda COM IA por padrão (`--ai=1`) e `--ai=0` reproduz o mundo
// sem IA.

#include "../commands.h"
#include "../sim_core.h"
#include "../world_state.h"
#include "ai_government.h"
#include "ai_group.h"
#include "ai_private.h"
#include "ai_risk.h"
#include "ai_storage.h"
#include "ai_templates.h"
#include "ai_view.h"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ogs {

// Resultado de uma ação emitida pela IA, já convertido do resultado do núcleo
// (InvestResult/BuildResult/GroupInvestResult) para o que a regra consulta.
struct AiActionResult {
	bool created = false;
	double cost = 0.0;
	double cash = 0.0;
	double local_share = 0.0;
	std::string error;
	std::string owner;
};

// D-131 fase B (R4) — planta nova registrada para a janela móvel de 30 dias.
struct AiHerdEvent {
	int day = 0;
	int product = -1;
	double capacity = 0.0;
};

// Estado da IA que NÃO é serializado no save e não cabe no WorldState: as
// janelas de preço do risco, o contador de id das plantas novas, o cooldown de
// obra do governo e as visões locais de caixa do dia (grupos). Vive no objeto
// `AiPhase` do chamador; a `AiPhase` não guarda nada fora daqui.
// D-131 fase C (diagnóstico): uma rejeição amostrada do forecast, com os
// valores que explicam o gate. Não é serializado (como os contadores).
struct AiForecastSample {
	int day = 0;
	int city = -1;
	int product = -1;
	int gate = 0;
	double cash = 0.0;
	double credit = 0.0;
	double capex = 0.0;
	double revenue = 0.0;
	double cost = 0.0;
	double payback = 0.0;
	double trough = 0.0;
	double min_balance = 0.0;
	double final_balance = 0.0;
	double cap_max = 0.0;
	double capacity = 0.0;
	double price = 0.0;
	double margin = 0.0;
};

struct AiState {
	// Contadores de id dos prédios criados pela IA (nome estável no log).
	int private_id = 1;
	int government_id = 1;
	// Cidade → dia da última obra (cooldown do governo).
	std::unordered_map<std::string, int> last_build_day;
	// Chave (cidade × produto) → janela de 30 preços do privado (risco).
	std::unordered_map<std::uint64_t, std::vector<double>> risk_history;
	// Caixa dos grupos na visão do privado (copiado da view no início do dia e
	// debitado a cada `group_invest` do mesmo dia).
	std::vector<double> private_group_cash;
	// Caixa dos grupos na visão do investidor de grupo.
	std::vector<double> group_cash;
	// C3 (D-107): crédito disponível por grupo no início do dia (0 quando a
	// mecânica está desligada). O investidor usa `cash + crédito` como
	// capacidade de capex e pede o empréstimo do que faltar.
	std::vector<double> group_credit;
	// Medidas do dia que os investidores compartilham (comida no mundo/região).
	double food_need = 0.0;
	double food_supply = 0.0;
	// D-125: dia da última troca de modo da refinaria (id da construção → dia).
	// Não é serializado: o cooldown da IA é uma janela de decisão do processo,
	// como as demais janelas de preço (`risk_history`).
	std::unordered_map<std::string, int> last_refinery_mode_day;
	std::vector<double> region_need;
	std::vector<double> region_supply;
	// G2 — janelas de preço observadas pela IA de armazém (chave
	// cidade × produto), até 365 dias, e o dia da última obra por
	// cidade|tipo|dono (anti-loop).
	std::unordered_map<std::uint64_t, std::vector<double>> storage_prices;
	std::unordered_map<std::string, int> storage_last_build_day;
	// D-131 — janelas de preço do cérebro econômico (chave cidade × produto),
	// o dia da última amostra e o cache de preço esperado do dia. Não são
	// serializados (mesma regra de `risk_history`).
	std::unordered_map<std::uint64_t, std::vector<double>> brain_price_history;
	std::unordered_map<std::uint64_t, int> brain_price_day;
	std::unordered_map<std::uint64_t, double> brain_price_expected;
	int brain_cache_day = -1;
	// D-131 — diagnóstico da fase A: forecasts avaliados e rejeições do gate.
	int brain_attempts = 0;
	// Rejeições do forecast de caixa (as únicas que entram em
	// `reject_share`; R4 tem contador próprio para a razão não passar de 1).
	int brain_rejected = 0;
	int brain_risk_rejected = 0;
	std::unordered_map<std::uint64_t, int> brain_rejected_month;
	int brain_unique_rejected_month = 0;
	// D-131 fase C: distribuição por gate (diagnóstico). `brain_exposure_rejected`
	// e `brain_herd_rejected` separam o antigo `risk_rejected`; `brain_loss_blocked`
	// é o bloqueio D-126 e `brain_competition_skipped` a lacuna de escopo nula.
	// Os contadores de grupo são separados dos do privado.
	int brain_exposure_rejected = 0;
	int brain_herd_rejected = 0;
	int brain_loss_blocked = 0;
	int brain_competition_skipped = 0;
	int brain_group_attempts = 0;
	int brain_group_rejected = 0;
	// D-131 fase C (diagnóstico): distribuição das reprovações do forecast por
	// gate (índice = `ForecastResult::failed_gate`, 0..6) para privado e grupo,
	// e as primeiras amostras de rejeição de cada lado (valores do forecast).
	std::array<int, 7> brain_gate_rejected{};
	std::array<int, 7> brain_group_gate_rejected{};
	std::vector<AiForecastSample> brain_samples;
	std::vector<AiForecastSample> brain_group_samples;
	// Rejeições do forecast em que a receita do horizonte ficou ZERO (aptidão
	// zero: cidade sem sítio/depósito) — separa "sem mercado" de "custo alto".
	int brain_zero_revenue = 0;
	int brain_group_zero_revenue = 0;
	// D-131 fase C: funil do grupo depois do forecast (oportunidade escolhida
	// e obra realmente criada pelo núcleo).
	int brain_group_executed = 0;
	int brain_group_created = 0;
	// D-131 fase B (R2/R4). Capacidade das obras emitidas NA MESMA fase
	// (privado + grupo + governo, ordem determinística), zerada no começo do dia.
	// `pending_committed` é o total MUNDO por produto (soma de todas as regiões);
	// `pending_committed_region` guarda o recorte por (massa de terra, produto) —
	// a cesta alimentar desconta só a obra da própria região (R3/Q-014). Os
	// caches do dia (oferta efetiva e exposição) são reconstruídos junto; o
	// histórico de demanda tem até 90 dias por produto. Nada disto é serializado.
	std::unordered_map<int, double> pending_committed;
	std::unordered_map<std::uint64_t, double> pending_committed_region;
	std::vector<AiHerdEvent> herd_events;
	AiSupplyCache brain_supply;
	AiExposureCache brain_exposure;
	std::vector<std::vector<double>> brain_demand_history;
	// Candidatos únicos por (cidade, produto) por mês: o mapa guarda o último
	// mês contado e o total soma a primeira aparição de cada chave no mês.
	std::unordered_map<std::uint64_t, int> brain_candidate_month;
	int brain_unique_month_candidates = 0;
	int brain_phase_day = -1;
};

// O que uma regra recebe: tudo por referência, nada copiado.
struct AiContext {
	WorldState& ws;
	SimCore& core;
	CommandQueue& queue;
	const AiTemplates& templates;
	AiView& view;
	AiState& state;
	int day = 0;
	// Construção instantânea (laboratório/testes) e construção de bootstrap
	// (planta estratégica): os mesmos sinalizadores que a fachada passa para
	// `build`/`invest`.
	bool instant = false;
	bool bootstrap = false;
};

// ==== Emissão de comandos (formato do A3) ====
//
// Cada função: monta o `Command`, empilha (`push`), retira SÓ ele da fila
// (`take_by_seq` — comandos da UI que chegaram no meio ficam pendentes),
// aplica a ação do núcleo e registra no log com `apply_day = day`.
//
// Invest:     s0 cidade, s1 prédio, v0 valor.
// Build:      s0 cidade, s1 prédio, s2 recurso, s3 dono, s4 receita;
//             i0 insumo, i1 saída, v0 capacidade, v1 empregos, v2 salário,
//             v3 opex, v4 insumo/produto, v5 custo.
// GroupInvest: s0 grupo, s1 cidade, s2 prédio, s3 recurso, s4 receita,
//             s5 parceiro, i0 saída, v0 capacidade.
AiActionResult ai_emit_invest(AiContext& ctx, const std::string& city,
		const std::string& building, double amount);
// D-086: aprofundar/expandir mina existente (comando `deepen_mine`).
// s0 cidade, s1 prédio.
AiActionResult ai_emit_deepen(AiContext& ctx, const std::string& city,
		const std::string& building);
// B5: obra de infraestrutura da região (comando `infra_build`).
// s0 cidade, i0 tipo (0 estrada, 1 energia, 2 água).
AiActionResult ai_emit_infra(AiContext& ctx, const std::string& city,
		int infra_type);
// B5: concessão de mina/fazenda estatal a grupo (comando `concede`).
// s0 cidade, s1 prédio, s2 grupo, v0 royalty (<= 0 usa o padrão).
AiActionResult ai_emit_concede(AiContext& ctx, const std::string& city,
		const std::string& building, const std::string& group, double royalty);
// G2 — obra/troca de armazém (comandos `storage_build`/`storage_trade`).
// StorageBuild: s0 cidade, i0 tipo, i1 dono, v0 toneladas de capacidade.
// StorageTrade: s0 cidade, s1 "relief"/"paid", i0 tipo, i1 dono, v0 toneladas,
//               v1 produto, b = compra (true)/liberação (false).
AiActionResult ai_emit_storage_build(AiContext& ctx, const std::string& city,
		int type, int scope, double tons);
AiActionResult ai_emit_storage_trade(AiContext& ctx, const std::string& city,
		int type, int scope, int product, double tons, bool buy, bool relief);
AiActionResult ai_emit_build(AiContext& ctx, const std::string& city,
		const std::string& building, const std::string& resource, int input,
		int output, double capacity, double jobs, double wage, double opex,
		double input_per_ratio, const std::string& owner, double cost,
		const std::string& recipe_id);
// C3 (D-107): pedido de crédito do grupo (comando `group_borrow`).
// s0 grupo, v0 valor pedido. `cost` = concedido.
AiActionResult ai_emit_group_borrow(AiContext& ctx, const std::string& group,
		double amount);
// D-131 fase C (R5): crédito de INVESTIMENTO da cidade (`city_borrow`).
// s0 cidade, v0 valor pedido. `cost` = concedido. Só a IA do cérebro emite.
AiActionResult ai_emit_city_borrow(AiContext& ctx, const std::string& city,
		double amount);
AiActionResult ai_emit_group_invest(AiContext& ctx, const std::string& group,
		const std::string& city, const std::string& building,
		const std::string& resource, int output, double capacity,
		const std::string& recipe_id, const std::string& partner);
// D-125: troca de modo da refinaria (comando `set_refinery_mode`).
// s0 cidade, s1 prédio, s2 modo (`standard`/`diesel`/`jet`).
AiActionResult ai_emit_refinery_mode(AiContext& ctx, const std::string& city,
		const std::string& building, const std::string& mode);

// Fase da IA do dia. O chamador guarda UM objeto (o estado entre dias vive em
// `state_`); `run` é quem decide a vez das cidades.
class AiPhase {
public:
	// `day` = `ws.day` no fim do tick (a fachada passa depois de fechar o dia).
	void run(WorldState& ws, SimCore& core, CommandQueue& queue, bool instant,
			bool bootstrap);

	// Mundo NOVO (initialize_world/initialize_scenario/from_snapshot*): zera o
	// estado entre dias (`AiState` — contadores de id voltam a 1, mapas e
	// vetores vazios), o retrato do dia (`view_`) e os templates, que passam a
	// ser reconstruídos no próximo `run`. O objeto continua utilizável; um
	// `from_dict` (load de save) NÃO chama isto — o estado da IA sobrevive ao
	// load, como o objeto EconomicAI no GDScript.
	void reset();

	// D-131 — leitura do estado entre dias (contadores de diagnóstico da fase A).
	const AiState& state() const { return state_; }

private:
	// Retrato do dia (reaproveitado entre dias: os vetores mantêm capacidade).
	AiView view_;
	// Templates de engenharia: construídos uma vez; refeitos só quando o
	// catálogo de produtos muda de tamanho.
	AiTemplates templates_;
	int templates_products_ = -1;
	AiState state_;
	AiPrivateInvestor private_investor_;
	AiGroupInvestor group_investor_;
	AiGovernment government_;
	AiStorageInvestor storage_investor_;
};

} // namespace ogs
