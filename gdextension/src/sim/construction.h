#pragma once
// §15 — construção como projeto físico (2026-11-16). Módulo de domínio: a
// receita de materiais de uma obra e o cronograma. C++ puro (STL via
// sim_types.h), nada de godot_cpp.
//
// A implementação dos métodos `SimCore::add_construction_project` /
// `tick_construction` / `project_index` /
// `city_committed_construction` vive em `construction.cpp` (módulo do
// `SimCore`, como freight/logistics/trade/...). Esta header expõe a TABELA DE
// MATERIAIS (receita de obra) para o save/load e para os testes.

#include "sim_types.h"

namespace ogs {
namespace construction {

// Um material da receita de obra: bem (id de goods.json) e peso na divisão do
// custo de materiais (a lista soma 1,0). A quantidade contratada é
// `peso × material_share × total_cost / base_price` — contrato de obra no
// preço-base (kConstructionMaterialShare em sim_constants.h).
struct MaterialSpec {
	const char* good;
	double weight;
};

// Materiais de uma obra padrão (toda obra). Ordem fixa — o save/load grava a
// quantidade por posição.
inline int base_materials(const MaterialSpec*& out) {
	static const MaterialSpec list[3] = {
		{"construction_material", 0.50},
		{"steel_structure", 0.30},
		{"electrical_material", 0.20},
	};
	out = list;
	return 3;
}

// Fábricas (receita) acrescentam maquinário industrial: lista renormalizada.
inline int factory_materials(const MaterialSpec*& out) {
	static const MaterialSpec list[4] = {
		{"construction_material", 0.35},
		{"steel_structure", 0.25},
		{"electrical_material", 0.15},
		{"industrial_machinery", 0.25},
	};
	out = list;
	return 4;
}

} // namespace construction
} // namespace ogs
