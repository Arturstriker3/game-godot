#pragma once
// R2b1 (rework.banks.deposits) - o deposito de funding e' LIQUIDO.
//
// Contrato do docs/economy/rework/R2_plano.md 4.3/4.6: todo caminho que
// avalia capacidade de pagamento ou cobra le CAIXA + CLAIM SACAVEL NO DIA,
// na ordem caixa -> deposito. O saque e' atomico: claim, deposits_funding,
// bank.cash e o caixa do pagador mudam na MESMA chamada; a conservacao
// (caixa sai do banco e entra no pagador) vale sempre.
//
// Sem `banks.enabled + banks.deposits` as leituras devolvem 0 e os saques
// nao tocam em nada - o caminho OFF/legado fica bit a bit o de antes.

#include <string>

namespace ogs {

struct WorldState;
struct City;
struct Group;

namespace deposit {

// Liga as cidades/grupos ao WorldState que os contem (nao serializado).
// Idempotente. Sem o link, as leituras devolvem 0 (caminho caixa puro).
void link(WorldState& ws);

// Claim sacavel hoje: min(claim, bank.cash * claim / S claims do banco).
double liquid_city(const WorldState& ws, const City& city, bool firm);
double liquid_group(const WorldState& ws, const Group& group);

// Saque atomico no banco de relacionamento. Move `x` de bank.cash/D_f
// para o caixa do pagador e reduz o claim. Devolve o sacado (<= want).
double withdraw_city(WorldState& ws, City& city, bool firm, double want);
double withdraw_group(WorldState& ws, Group& group, double want);

// Pagamento do grupo: caixa primeiro, deposito depois. `from_cash` e
// `from_claim` (opcionais) registram a ORIGEM do dinheiro - o servicing usa
// isso para debitar D_l so' na parcela que saiu do caixa de emprestimo.
double pay_group(WorldState& ws, Group& group, double value,
	double* from_cash = nullptr, double* from_claim = nullptr);

} // namespace deposit
} // namespace ogs
