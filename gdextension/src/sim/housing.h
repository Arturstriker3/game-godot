#pragma once
// §19 (W3) — MERCADO DE MORADIA. C++ puro (só STL), nada de godot_cpp.
//
// A lógica roda UMA vez por tick em `sim/housing.cpp` (SimCore::phase_housing),
// logo DEPOIS da renda/consumo da POP (fase 5) e ANTES dos serviços/comércio:
// a comida do dia já foi comprada e o aluguel só usa a poupança acima da
// reserva de comida — comida tem prioridade. Nada é criado nem destruído: todo
// fluxo é transferência entre contas que já existem (POP, firmas, prefeitura,
// setor de serviços).
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
//    POPULAR/MEDIO com a poupança acima da reserva de comida. A obra consome
//    materiais do mercado local, paga o setor de construção e entra em
//    operação depois do prazo — a reserva de capital é do próprio dono;
//  - favela SÓ no último caso: falta popular prolongada, nenhuma vaga em
//    padrão algum e aluguel popular acima do ônus suportável.
//
// `housing_stress` (0..1) é o sinal publicado por cidade para o W4 (migração)
// e para a UI: falta de unidade no padrão popular + ônus de aluguel acima do
// teto. Não há campo "stress" em outro lugar do núcleo.

namespace ogs {

// Obra de moradia em andamento (projeto §15 tipo "moradia"). O custo é pago
// (reservado) na criação; as unidades entram no estoque ao fim do prazo. Lista
// de tamanho fixo por cidade: o mercado abre no máximo
// `kDemoHousingMaxProjects` obras por vez, o que mantém o save pequeno.
enum HousingProjectState : int {
	HOUSING_PROJECT_IDLE = 0,
	HOUSING_PROJECT_PRIVATE = 1,  // incorporadora (capital privado)
	HOUSING_PROJECT_SELF = 2,     // autoconsntrução da POP (poupança)
};

struct HousingProject {
	int state = HOUSING_PROJECT_IDLE;
	int density = 1;  // DENSITY_MID
	int tier = 0;     // TIER_POPULAR
	double units = 0.0;
	int days_left = 0;
};

constexpr int kDemoHousingMaxProjects = 4;

} // namespace ogs
