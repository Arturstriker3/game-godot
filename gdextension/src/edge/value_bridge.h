#pragma once
// Ponte entre o `Variant` do Godot e o `ogs::save::Value` da biblioteca de
// simulação (fase A1a: núcleo C++ puro, sem godot-cpp).
//
// O núcleo (`src/sim/`) não conhece Godot: o save é serializado em
// `ogs::save::Value` (save_value.h). Este arquivo é a ÚNICA camada que converte
// os dois mundos — o adaptador `edge/save_load.cpp` e a fachada `EconomySim`
// (snapshot) continuam falando Dictionary/Array/Variant com o GDScript, e o
// núcleo continua em STL.
//
// Contrato de conversão (exato, sem inventar tipo):
//   * NIL        <-> NIL;
//   * BOOL       <-> BOOL;
//   * INT        <-> INT   (int64);
//   * FLOAT      <-> FLOAT (double);
//   * STRING     <-> STRING, STRING_NAME -> STRING (bytes UTF-8, nunca Latin-1:
//                    nomes com acento não podem corromper no round-trip);
//   * ARRAY      <-> ARRAY (por índice, ordem preservada);
//   * DICTIONARY <-> DICTIONARY (iterado por `keys()`, ordem de inserção
//                    preservada — o hash do save depende dela).
// Qualquer outro tipo (Vector2, Packed*Array, Object...) vira NIL nos dois
// sentidos: o save não os usa e converter esconderia o erro.

#include <godot_cpp/variant/variant.hpp>

#include "../sim/save_value.h"

namespace ogs {
namespace edge {

// Value do núcleo -> Variant do Godot (Array/Dictionary nativos).
godot::Variant to_godot(const save::Value& value);

// Variant do Godot -> Value do núcleo. Tipo fora do contrato devolve NIL.
save::Value from_godot(const godot::Variant& value);

} // namespace edge
} // namespace ogs
