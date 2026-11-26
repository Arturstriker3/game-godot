#pragma once
// Agregado por nação da fachada (H1c — modularização, refatoração pura).
//
// `EconomySim::capture_nation_labor`, o retrato semanal do trabalho
// (`find_labor_snapshot`) e `EconomySim::build_nations` vivem em
// `status_nation.cpp`: a captura do agregado `nations[].labor` no fecho do dia
// `day % 7 == 0`, a busca do retrato mais recente e o bloco `nations` do
// status (totais por nação, frota, mercado de trabalho e Gini de riqueza) a
// partir do `WorldState`. Continuam sendo métodos de `EconomySim` — declarados
// em `economy_sim.h` — porque leem `ws_`, `sim_core_`, `status_keys_`,
// `labor_snapshot_` e os slots de `StatusCache` da fachada; este header existe
// como âncora do módulo (documentação + inclusão estável).
//
// Nenhuma regra, ordem de escrita, ordem de chaves ou nome de campo mudou na
// extração: os corpos foram movidos verbatim e `economy_sim.cpp` só passou a
// linkar os mesmos símbolos de outro TU.
