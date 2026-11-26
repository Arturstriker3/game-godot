#pragma once
// Cache de sinais e do status do dia da fachada (H1d — modularização,
// refatoração pura).
//
// `EconomySim::fill_light_signals`, `EconomySim::build_ports`,
// `EconomySim::build_orders`, `EconomySim::build_currencies`,
// `EconomySim::build_tariffs` e `EconomySim::get_status` vivem em
// `status_cache.cpp`: são os sinais leves por cidade (mercados, POP,
// incumbentes, construções, jazidas e obras), os blocos de portos, ordens,
// câmbio e tarifas, e a montagem do dicionário de status do dia a partir do
// `WorldState`. Continuam sendo métodos de `EconomySim` — declarados em
// `economy_sim.h` — porque leem `ws_`, `sim_core_`, `status_keys_` e os slots
// de status da fachada; este header existe como âncora do módulo
// (documentação + inclusão estável).
//
// Nenhuma regra, ordem de escrita, ordem de chaves ou nome de campo mudou na
// extração: os corpos foram movidos verbatim e `economy_sim.cpp` só passou a
// linkar os mesmos símbolos de outro TU.
