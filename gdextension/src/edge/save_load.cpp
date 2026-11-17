#include "save_load.h"

#include "value_bridge.h"

#include "../sim/save_state.h"

namespace ogs {
namespace edge {

// Adaptador fino: a serialização (chaves, ordem de inserção, tipos, migrações)
// vive em sim/save_state.cpp, sobre o shim sim/save_value.h. Aqui só se troca
// de representação: Dictionary do Godot <-> ogs::save::Value.
godot::Dictionary to_dict(const WorldState& ws, const SimCore& core) {
	return static_cast<godot::Dictionary>(to_godot(save::to_value(ws, core)));
}

void from_dict(WorldState& ws, SimCore& core, const godot::Dictionary& data) {
	save::from_value(ws, core, from_godot(data));
}

} // namespace edge
} // namespace ogs
