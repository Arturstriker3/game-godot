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
#include "ai_templates.h"
#include "ai_view.h"

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

// Estado da IA que NÃO é serializado no save e não cabe no WorldState: as
// janelas de preço do risco, o contador de id das plantas novas, o cooldown de
// obra do governo e as visões locais de caixa do dia (grupos). Vive no objeto
// `AiPhase` do chamador; a `AiPhase` não guarda nada fora daqui.
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
	// Medidas do dia que os investidores compartilham (comida no mundo/região).
	double food_need = 0.0;
	double food_supply = 0.0;
	std::vector<double> region_need;
	std::vector<double> region_supply;
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
// D-083: aprofundar/expandir mina existente (comando `deepen_mine`).
// s0 cidade, s1 prédio.
AiActionResult ai_emit_deepen(AiContext& ctx, const std::string& city,
		const std::string& building);
AiActionResult ai_emit_build(AiContext& ctx, const std::string& city,
		const std::string& building, const std::string& resource, int input,
		int output, double capacity, double jobs, double wage, double opex,
		double input_per_ratio, const std::string& owner, double cost,
		const std::string& recipe_id);
AiActionResult ai_emit_group_invest(AiContext& ctx, const std::string& group,
		const std::string& city, const std::string& building,
		const std::string& resource, int output, double capacity,
		const std::string& recipe_id, const std::string& partner);

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
};

} // namespace ogs
