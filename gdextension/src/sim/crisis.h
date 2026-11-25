#pragma once
// D-126 — SOBREVIVÊNCIA EM CRISE.
//
// Regras puras sobre WorldState/SimCore, sem godot-cpp. O contrato e os
// defaults declarados estão em docs/economy/D126_crisis.md e
// data/definitions/economy/crisis.json.
//
// A ordem da reação é FIXA (não é parâmetro):
//   1. cortar produção (regra existente de `loss_days`);
//   2. demitir com rescisão;
//   3. crédito de capital de giro (1 renegociação com multa);
//   4. liquidar estoque com desconto;
//   5. hibernar (mothball) e reabrir com custo;
//   6. falência -> leilão do prédio (sem comprador, remove).
//
// Toda ação é determinística: a ordem de visita é a ordem do save (buildings,
// groups e power_plants), sem `unordered_map`. Nenhum fluxo cria/destrói
// dinheiro fora das regras bancárias já existentes.
#include <cstddef>
#include <map>
#include <string>

#include "world_state.h"

namespace ogs {

class SimCore;

namespace crisis {

// Aplica um mapa chave->valor (do JSON ou do save) sobre os parâmetros, sem
// tocar em chaves ausentes. Valores inválidos são ignorados (nunca lança).
void apply_params(WorldState& ws, const std::map<std::string, double>& values);

// Mapa completo dos parâmetros (ordem estável) para o save/status.
std::map<std::string, double> params_map(const WorldState& ws);

// Fase do dia: reação determinística das construções, usinas e serviços.
// `bank_money_delta` recebe o dinheiro bancário criado/destruído pelos
// empréstimos/juros do dia (o invariante soma ao `emission_today`).
void phase(WorldState& ws, SimCore& core, double* bank_money_delta);

// ---------------------------------------------------------------------------
// Ações explícitas (API do jogador/teste). Todas devolvem false com `error`
// quando recusadas; nenhuma inventa números.
// ---------------------------------------------------------------------------
// D-131/R5: limite do crédito de INVESTIMENTO da cidade (0,5 × ativos −
// dívida). É o mesmo teto que a D-126 já usa na escada; o giro de crise
// (`credit_working_days`) é outra coisa e não passa por aqui.
double city_credit_limit(const WorldState& ws, std::size_t city_index);
bool borrow_city(WorldState& ws, const std::string& city_id, double amount,
	double* out_granted, std::string* error);
bool layoff(WorldState& ws, SimCore& core, const std::string& city_id,
	const std::string& building_id, double share, std::string* error);
bool borrow_working_capital(WorldState& ws, SimCore& core,
	const std::string& city_id, double amount, std::string* error);
bool liquidate_stock(WorldState& ws, SimCore& core, const std::string& city_id,
	const std::string& building_id, double share, std::string* error);
bool mothball(WorldState& ws, SimCore& core, const std::string& city_id,
	const std::string& building_id, std::string* error);
bool reopen(WorldState& ws, SimCore& core, const std::string& city_id,
	const std::string& building_id, std::string* error);
// Leilão imediato (teste/validação). Se não houver comprador, o prédio é
// marcado `bankrupt` e a remoção segue a regra existente do início do tick.
bool auction(WorldState& ws, SimCore& core, const std::string& city_id,
	const std::string& building_id, std::string* error);

} // namespace crisis
} // namespace ogs
