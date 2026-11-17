#pragma once
// §19 (W1) — solo urbano e zoneamento (docs/economy/s19_urban_spec.md).
//
// Módulo de domínio do `SimCore`/`WorldState`, C++ puro (STL via sim_types.h e
// world_state.h): nada de godot_cpp em sim/. Responsabilidades do W1:
//   - capacidade e ocupação de cada zona (RES: unidades de moradia; COM:
//     negócios de serviço; IND: plantas industriais urbanas);
//   - preço da terra por zona, mensal e suave, movido por ocupação e renda;
//   - a zona como LIMITE de decisão: abrir negócio de serviço sem espaço COM
//     (ou fábrica sem espaço IND) não falha — fica pendente e sinaliza
//     `zone_demand` para a prefeitura (W2);
//   - expansão/adensamento de zona como obra §15 (tipo `infra_urbana`).
//
// A obra urbana usa o mesmo cronograma do §15 (materiais
// construction_material/steel_structure/electrical_material + mão de obra do
// setor de construção). Ao concluir, `apply_completed_project` altera o solo
// (não materializa prédio). Sem lógica de dinheiro aqui além do pagamento do
// dono pelo projeto (reservado no início, gasto conforme a obra avança).

#include "sim_types.h"
#include "world_state.h"

#include <cstddef>
#include <string>

namespace ogs {

class SimCore;
struct CityGov;

namespace urban_land {

// Tipo de obra de infraestrutura urbana (vai no projeto §15).
enum UrbanAction : int {
	URBAN_NONE = 0,    // obra econômica normal (cria prédio)
	URBAN_EXPAND = 1,  // soma área à zona, consome reserve_km2 e cria infra
	URBAN_DENSIFY = 2, // sobe max_density da zona em um passo
};

// Capacidade física de uma zona: RES em unidades de moradia, COM e IND em
// TRABALHADORES (m² de piso por trabalhador). Capacidade = área × FAR da
// densidade permitida ÷ m² por unidade/trabalhador × `zone_fill` (fração do
// envelope construída à escala da cidade; 1,0 em save antigo/cenário).
double zone_capacity(const CityLand& land, int zone);

// Capacidade EFETIVA: a física, com piso no uso corrente (nenhuma cidade tem
// ocupação acima de 100% — estoque maior que o envelope é inconsistência de
// fonte, não cidade com 137% de ocupação). É a que a ocupação e o gate usam.
double zone_capacity_for(const WorldState& ws, std::size_t city_index, int zone);

// Uso corrente da zona, na mesma unidade da capacidade. RES: unidades de
// moradia carregadas; COM: empregos dos negócios de serviço ativos; IND:
// empregos das plantas industriais urbanas em operação mais as obras de
// fábrica em andamento (para o limite não ser furado por várias decisões no
// mesmo dia).
double zone_usage(const WorldState& ws, std::size_t city_index, int zone);

// Ocupação = uso ÷ capacidade (0 quando a zona está vazia; um valor grande mas
// finito quando há uso sem capacidade — o preço satura no teto declarado).
double zone_occupancy(const WorldState& ws, std::size_t city_index, int zone);

// Há espaço para `slots` unidades/negócios/plantas na zona?
bool zone_has_room(const WorldState& ws, std::size_t city_index, int zone,
	double slots);

// Registra uma decisão barrada pela falta de espaço (sinal para a prefeitura).
void signal_zone_demand(CityLand& land, int zone);

// Fase mensal (dia múltiplo de kUrbanMonthDays): preço da terra por zona e
// decadência do sinal de demanda. Não move dinheiro; só lê o mundo.
// A6: hoje a fase e ESCALONADA por cidade ((dia - indice) %
// cadence.monthly_period == 0); a linha acima descreve o calendario antigo
// (dia global), mantida como registro.
void phase_month(WorldState& ws);

// ---------------------------------------------------------------------------
// §19 (integração) — custo crescente do solo (spec, "Custo crescente").
// ---------------------------------------------------------------------------
// Custo de 1 km² de expansão HOJE nesta cidade (XGS/km²): base × fator de
// distância (raio atual ÷ raio do carregamento) × fator de relevo (fração da
// reserva já consumida — o que sobra é o terreno pior). Sem dado de solo
// devolve 0 (ausência explícita).
double expansion_cost_per_km2(const WorldState& ws, std::size_t city_index);

// Custo de subir o gabarito da zona um nível (XGS): área da zona × custo do
// km² × fator do nível (LOW→MID 0,35; MID→HIGH 0,85). Devolve 0 quando a zona
// já está no máximo ou não há solo carregado.
double densify_cost(const WorldState& ws, std::size_t city_index, int zone);

// Abre obra §15 de expansão (`km2` > 0, exige reserve_km2) ou adensamento
// (exige infraestrutura para a população e densidade abaixo do máximo).
//
// Dono: `owner` "state" ou "private". O capital é RESERVADO no início, como
// qualquer obra §15 — de `city.government_budget`/`city.firm_cash`, ou do
// CAIXA DA PREFEITURA quando `payer` não é nulo (integração W2: o dinheiro é
// da prefeitura, não do orçamento público genérico). Devolve false e preenche
// `error` quando a obra não pode começar.
bool start_project(WorldState& ws, SimCore& core, std::size_t city_index, int zone,
	int action, double km2, const std::string& owner, std::string* error,
	std::string* project_id, CityGov* payer = nullptr);

// Conclusão de uma obra §15 de infraestrutura urbana (chamada por
// construction.cpp no lugar da materialização do prédio).
void apply_completed_project(WorldState& ws, const ConstructionProject& project);

} // namespace urban_land
} // namespace ogs
