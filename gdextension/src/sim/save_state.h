#pragma once
// Serialização save/load dentro da biblioteca de simulação, sem godot-cpp:
// porte mecânico de edge/save_load.cpp para ogs::save, sobre o shim de valor
// save_value.h (Array/Dictionary/Variant são aliases de Value; String é a
// string UTF-8 do shim).
//
// O formato (chaves, ordem de inserção e tipos) é o do save real: o save_sha256
// do jogo depende dele, então nada aqui pode "melhorar" sem quebrar saves.

#include "save_value.h"
#include "world_state.h"

namespace ogs {
class SimCore;
}

namespace ogs::save {

// Serializa todo o estado do cenário (save). Sem lock: quem chama segura o mutex.
Value to_value(const WorldState& ws, const SimCore& core);

// Restaura o estado do cenário a partir de to_value() (load + migrações).
// Sem lock: quem chama segura o mutex e invalida o cache de status.
void from_value(WorldState& ws, SimCore& core, const Value& data);

} // namespace ogs::save
