#pragma once
// Ficha e colunas de cidade da fachada (H1b — modularização, refatoração pura).
//
// `EconomySim::fill_city_status` vive em `status_city.cpp`: é o preenchimento
// do `StatusCitySlot` (dicionário da ficha da cidade + colunas) a partir do
// `WorldState`. Continua sendo método de `EconomySim` — declarado em
// `economy_sim.h` — porque lê direto no estado da fachada; este header existe
// como âncora do módulo (documentação + inclusão estável).
//
// Nenhuma regra, ordem de escrita, espelho incremental ou nome de campo mudou
// na extração: o corpo foi movido verbatim e `economy_sim.cpp` só passou a
// linkar o mesmo símbolo de outro TU.
