#pragma once
#include <cstddef>
// §19 (W3) — MERCADO DE MORADIA. C++ puro (só STL), nada de godot_cpp.
//
// A lógica roda UMA vez por tick em `sim/housing.cpp` (SimCore::phase_housing),
// logo DEPOIS da renda/consumo da POP (fase 5) e ANTES dos serviços/comércio:
// o aluguel é cobrado da RENDA do dia, depois da comida e ANTES do consumo de
// life/luxury — a fase de renda reserva `daily_rent_obligation` no orçamento
// discricionário e `phase_housing` executa a transferência. Nada é criado nem
// destruído: todo fluxo é transferência entre contas que já existem (POP,
// firmas, prefeitura, setor de serviços).
//
// Modelo (agregado por cidade, sem agente por prédio):
//  - demanda por padrão = domicílios do estrato (classe de consumo da POP);
//    a base `households` vem do estoque da fonte (que É população ÷ tamanho do
//    domicílio — housing_city.json), então o mundo nasce em equilíbrio;
//  - oferta = unidades do padrão (depreciação anual remove a mesma fração);
//    excesso de demanda num padrão TRANSBORDA para o padrão abaixo (filtragem);
//  - aluguel mensal se move no máximo ±2%/mês pelo desequilíbrio, ancorado na
//    renda do estrato (teto de ônus de aluguel);
//  - preço do imóvel = aluguel × 12 ÷ cap rate (§19.6, 0,05/ano);
//  - construção (§15, tipo "moradia"): incorporadora constrói MID/HIGH quando
//    o preço cobre o custo com margem E há falta; a POP autoconsntrói LOW
//    POPULAR/MEDIO com a poupança acima da reserva de comida. A obra entra na
//    FILA ÚNICA da cidade (`ws.construction_projects`, kind
//    CONSTRUCTION_HOUSING) e avança no tick do §15: materiais do mercado
//    local, mão de obra do setor de construção e entrega das unidades ao fim
//    do prazo. O capital é reservado pelo próprio dono na abertura (a parcela
//    de TERRA é creditada à prefeitura);
//  - favela SÓ no último caso: falta popular prolongada, nenhuma vaga em
//    padrão algum e aluguel popular acima do ônus suportável.
//
// `housing_stress` (0..1) é o sinal publicado por cidade para o W4 (migração)
// e para a UI: falta de unidade no padrão popular + ônus de aluguel acima do
// teto. Não há campo "stress" em outro lugar do núcleo.

namespace ogs {

struct WorldState;
struct ConstructionProject;
class SimCore;

// Dono da obra de moradia — vai em `ConstructionProject::housing_owner`
// (a obra mora na fila única §15, kind CONSTRUCTION_HOUSING). O máximo de
// obras de moradia SIMULTÂNEAS por cidade na fila é kHousingMaxProjects.
enum HousingProjectState : int {
	HOUSING_PROJECT_IDLE = 0,
	HOUSING_PROJECT_PRIVATE = 1,     // incorporadora (capital privado)
	HOUSING_PROJECT_SELF = 2,        // autoconsntrução da POP (poupança)
	HOUSING_PROJECT_GOVERNMENT = 3,  // moradia social (caixa da prefeitura, W2)
};

constexpr int kHousingMaxProjects = 4;

namespace housing {

// Custo de UMA unidade (igual ao de antes): anos de aluguel + land_price × m².
// `city_index` sem dado urbano devolve 0 (ausência explícita).
double build_cost_unit(const WorldState& ws, std::size_t city_index,
	int density, int tier);

// Moradia na fila única: valida caixa do dono, cobra o `total_cost` do dono,
// monta o ConstructionProject kind HOUSING e chama
// core.add_construction_project (que credita a terra à prefeitura e
// reserva o resto). `owner_state` = HOUSING_PROJECT_PRIVATE (caixa das firmas)
// | SELF (poupança da POP) | GOVERNMENT (caixa da prefeitura). false + nada
// pago quando não cabe.
bool start_project(WorldState& ws, SimCore& core, std::size_t city_index,
	int owner_state, int density, int tier, double units);

// Aluguel que os inquilinos devem pagar HOJE (XGS/dia): fatura mensal ÷ 30,
// limitada pelo teto de ônus (kHousingRentBurdenCap) da renda dos
// inquilinos. É a MESMA conta que phase_housing usa para pagar; a fase de
// renda (consumption.cpp) a desconta do orçamento discricionário do dia, para
// o aluguel sair da RENDA depois da comida e ANTES de life/luxury (Q-015/B2).
// Nunca maior que a fatura nem que o teto de ônus; 0 sem dado urbano.
double daily_rent_obligation(const WorldState& ws, std::size_t city_index);

// Nº de obras de moradia em andamento nesta cidade (nunca maior que
// kHousingMaxProjects).
int project_count(const WorldState& ws, std::size_t city_index);

// Moradia social em andamento nesta cidade? (a prefeitura financia uma por vez)
bool has_social_project(const WorldState& ws, std::size_t city_index);

// Obra de moradia da PREFEITURA (moradia social popular). As unidades são
// reduzidas ao que o caixa paga (mínimo 1); dono GOVERNMENT e posse indo para
// OWNER_GOVERNMENT na entrega.
bool start_social_housing(WorldState& ws, SimCore& core, std::size_t city_index,
	double units);

// Conclusão (chamada por construction.cpp): soma as unidades (densidade×padrão)
// e a fração de POSSE do dono do projeto; tenure novo como aluguel só no caso
// GOVERNMENT (comportamento atual do W3).
void apply_completed_project(WorldState& ws,
	const ConstructionProject& project);

} // namespace housing

} // namespace ogs
