#pragma once
// G2 — IA de ARMAZÉM (privado + governo) em C++ puro.
//
// Regras (docs/economy/storage_g2.md, D-100):
//   * PRIVADO constrói por PAYBACK: spread esperado (histórico de preço visto;
//     sem histórico, o spread declarado por tipo) × volume anual da região
//     contra obra + manutenção + perda no prazo. Perfil CONSERVADOR ×
//     ESPECULADOR sai do Gini de renda (dado da cidade): o especulador vende
//     mais perto da colheita e guarda mais tempo.
//   * PRIVADO guarda/vende pela próxima colheita (calendário da região, que a
//     view carrega) e pelo histórico de preço visto; NÃO tem bola de cristal.
//   * Anti-fome: com satisfação alimentar abaixo do piso, o privado não compra
//     comida e libera o estoque (socorro), para a regra não virar ganância.
//   * GOVERNO compra na colheita/preço baixo e libera na entressafra/preço alto
//     ou crise (satisfação baixa), dentro do orçamento e da capacidade.
//   * Cadência: semanal, junto das demais decisões da IA (A6, dia % período);
//     a decisão mensal de preço/estoque emerge do histórico de 365 dias.
//
// Toda leitura vem da `AiView` congelada do dia (ai_view.h); as ações saem
// como comando A3 e são aplicadas na hora (`ai_emit_storage_*`).
#include <string>
#include <unordered_map>
#include <vector>

namespace ogs {

struct AiContext;

struct AiStorageInvestor {
	// Decisão de UMA cidade da vez: observa preços, decide PRIVADO e depois
	// GOVERNO. No máximo uma ação por dono/cidade na visita.
	void run_city(AiContext& ctx, int city_index);

private:
	// Preço de referência do produto na cidade: média do histórico observado
	// (até 365 dias); 0 = ainda sem histórico.
	double reference_price(const AiContext& ctx, int city_index,
		int product) const;
	// Próxima colheita da cidade (dias) e se hoje há janela ativa.
	double days_to_harvest(const AiContext& ctx, int city_index) const;
	bool harvest_active(const AiContext& ctx, int city_index) const;
	void observe_prices(AiContext& ctx, int city_index);
	bool private_decisions(AiContext& ctx, int city_index);
	bool government_decisions(AiContext& ctx, int city_index);
};

} // namespace ogs
