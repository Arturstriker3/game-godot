#pragma once
// Adaptador save/load da fachada Godot (A1a: núcleo C++ puro).
//
// A serialização NÃO mora mais aqui: o formato do save é o de
// `sim/save_state.cpp` (to_value/from_value, porte do save v14 sobre o shim
// `sim/save_value.h`) e o bloco estático das definições é o de
// `sim/save_definitions.cpp`. Este arquivo é só a PONTE Dictionary Godot <->
// `ogs::save::Value` (edge/value_bridge.*), com as mesmas assinaturas que a
// fachada EconomySim sempre usou. Vive em edge/ (não em sim/) porque toca
// tipos do Godot, que violam o gate §2.2 de sim/.

#include <godot_cpp/variant/dictionary.hpp>

#include "../sim/sim_core.h"
#include "../sim/world_state.h"

namespace ogs {
namespace edge {

// Serializa todo o estado do cenário (save). Sem lock: a fachada segura o mutex.
godot::Dictionary to_dict(const WorldState& ws, const SimCore& core);

// Restaura o estado do cenário a partir de to_dict() (load + migrações).
// Sem lock: a fachada segura o mutex e invalida o cache de status.
void from_dict(WorldState& ws, SimCore& core, const godot::Dictionary& data);

} // namespace edge
} // namespace ogs
