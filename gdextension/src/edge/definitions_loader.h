#pragma once
// Carregador de definições da fachada (H1a — modularização, refatoração pura).
//
// `EconomySim::set_currencies`, `set_goods`, `set_recipes`, `seed_industry` e
// `set_needs` vivem em `definitions_loader.cpp`: são o caminho por onde os
// dados do jogo (currencies.json, goods.json, recipes.json, needs.json e o
// bootstrap de indústria) entram no `WorldState`. Continuam sendo métodos de
// `EconomySim` — declarados em `economy_sim.h` — porque escrevem direto no
// estado protegido pelo mutex da fachada; este header existe como âncora do
// módulo (documentação + inclusão estável).
//
// Nenhuma regra, ordem de iteração ou nome de campo mudou na extração: o corpo
// das funções foi movido verbatim e `economy_sim.cpp` só passou a linkar o
// mesmo símbolo de outro TU.
