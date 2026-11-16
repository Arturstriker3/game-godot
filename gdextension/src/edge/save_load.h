#pragma once
// Serialização save/load (S6 do docs/economy/core_refactor_plan.md). Vive em
// edge/ (não sim/) porque mexe com Dictionary/Array/String do Godot, que
// violam o gate §2.2 de sim/. A fachada EconomySim delega demo_to_dict para cá.

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
