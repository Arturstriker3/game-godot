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

namespace urban_land {

// Tipo de obra de infraestrutura urbana (vai no projeto §15).
enum UrbanAction : int {
	URBAN_NONE = 0,    // obra econômica normal (cria prédio)
	URBAN_EXPAND = 1,  // soma área à zona, consome reserve_km2 e cria infra
	URBAN_DENSIFY = 2, // sobe max_density da zona em um passo
};

// Capacidade física de uma zona em "slots": RES em unidades de moradia, COM em
// negócios de serviço e IND em plantas industriais. Capacidade = área × FAR da
// densidade permitida ÷ m² por unidade/negócio/planta.
double zone_capacity(const CityLand& land, int zone);

// Uso corrente da zona. RES: unidades de moradia carregadas; COM: negócios de
// serviço ativos; IND: plantas industriais/mineiras urbanas em operação mais as
// obras em andamento (para o limite não ser furado por várias decisões no
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

// Fase mensal (dia múltiplo de kDemoUrbanMonthDays): preço da terra por zona e
// decadência do sinal de demanda. Não move dinheiro; só lê o mundo.
void phase_month(WorldState& ws);

// Abre obra §15 de expansão (`km2` > 0, exige reserve_km2) ou adensamento
// (exige infraestrutura para a população e densidade abaixo do máximo). O dono
// ("state" ou "private") reserva o custo no início, como qualquer obra §15.
// Devolve false e preenche `error` quando a obra não pode começar.
bool start_project(WorldState& ws, SimCore& core, std::size_t city_index, int zone,
	int action, double km2, const std::string& owner, std::string* error,
	std::string* project_id);

// Conclusão de uma obra §15 de infraestrutura urbana (chamada por
// construction.cpp no lugar da materialização do prédio).
void apply_completed_project(WorldState& ws, const DemoConstructionProject& project);

} // namespace urban_land
} // namespace ogs
