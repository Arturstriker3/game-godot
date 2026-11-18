#pragma once
// Serialização do BLOCO ESTÁTICO do WorldState (as definições do jogo) para o
// Value JSON puro-C++ de save_value.h, para que um executável rode o mundo sem
// o Godot.
//
// Aqui viaja o que é carregado UMA vez a partir de data/definitions e fica
// guardado no WorldState:
//   * goods.json            -> set_goods            (produtos, preços-base,
//                                                         setor, duráveis);
//   * recipes.json          -> set_recipes          (receitas + índices);
//   * needs.json            -> set_needs            (níveis, categorias,
//                                                         cesta alimentar,
//                                                         governo, decaimento);
//   * services.json         -> set_services         (setores de serviço);
//   * sector_employment.json-> set_sector_employment (âncoras e demanda
//                                                         de trabalho);
//   * set_balance      -> parâmetros de balanceamento do demo.
//   * land_capacity    -> teto físico agrícola por cidade/produto (dado de
//                         GEOGRAFIA: chega pelo GeographyService e não entra
//                         no save; viaja aqui só para o snapshot ser
//                         autossuficiente).
//   * age_structure_2026.json -> set_age_structure   (faixas etárias, fração
//                         18-49, TFR e taxas brutas por nação — definição
//                         estática do B1; o save de estado ainda não a lê).
//
// Estado DINÂMICO (cidades, prédios, obras, serviços ofertados, grupos, moedas,
// portos, mercado, caixas) NÃO mora aqui: é do save.
//
// Contrato:
//   * `definitions_from` nunca lança e não exige chave existente: campo ausente
//     mantém o default do WorldState (ausência explícita, nunca um número
//     inventado); objeto que não é DICTIONARY é ignorado por inteiro.
//   * A saída é determinística: só `std::vector` e o `std::map` ordenado de
//     `age_structures` são percorridos, e nenhum `unordered_map` é
//     serializado. Os índices derivados
//     (`product_lookup`, `recipe_lookup`) são reconstruídos na leitura, como
//     fazem set_goods/set_recipes.

#include "save_value.h"
#include "world_state.h"

namespace ogs {

// Forward: `apply_land_capacity` aplica no núcleo, mas este header não precisa
// da definição (o .cpp inclui sim_core.h).
class SimCore;

namespace save {

// Bloco estático -> objeto JSON (chaves e ordem documentadas em .cpp).
Value definitions_to(const WorldState& ws);

// Objeto JSON -> bloco estático, preservando o que já está no `ws` para as
// chaves ausentes.
void definitions_from(WorldState& ws, const Value& data);

// `definitions["land_capacity"]` -> `core.set_land_capacity`, para ser chamado
// DEPOIS do estado dinâmico ter sido carregado. O bloco `definitions` é lido
// antes das cidades existirem, então `definitions_from` não consegue aplicar o
// teto agrícola: aqui ele entra no núcleo com o mundo já povoado. Mesmo filtro
// de `definitions_from`: item inválido é ignorado, nunca lança e chave ausente
// não tem efeito.
void apply_land_capacity(WorldState& ws, SimCore& core,
		const Value& definitions);

} // namespace save
} // namespace ogs
