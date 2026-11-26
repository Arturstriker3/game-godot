#pragma once
// Ponte de comandos da fachada (H1e — modularização, refatoração pura).
//
// A fila A3 e o seu caminho de aplicação vivem em `commands_bridge.cpp`:
// `EconomySim::dispatch_command`, `EconomySim::apply_pending_locked`,
// `EconomySim::apply_commands`, `EconomySim::pending_command_count`,
// `EconomySim::get_command_log`, `EconomySim::clear_command_log`,
// `EconomySim::command_log_truncated`, `EconomySim::replay_commands`,
// `EconomySim::apply_one_locked` e os conversores log <-> `ogs::Command`
// (`command_to_dict`/`command_from_dict`, namespace anônimo do TU).
//
// `apply_one_locked` é o despachante: ele mantém o `switch` de tipos de comando
// e chama os `apply_*` (que continuam sendo corpos da fachada neste passo).
// Tudo continua sendo membro de `EconomySim` — declarado em `economy_sim.h` —
// porque os corpos leem `ws_`, `commands_`, `mutex_`, `tick_in_progress_` e
// `tick_thread_`; este header existe como âncora do módulo (documentação +
// inclusão estável).
//
// Nenhuma regra, ordem de aplicação, ordem de chaves ou nome de campo mudou na
// extração: os corpos foram movidos verbatim e `economy_sim.cpp` só passou a
// linkar os mesmos símbolos de outro TU.
