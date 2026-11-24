#pragma once
// Fixture "mundo de bolso" dos testes unitarios C++ (FASE 2).
//
// Monta um WorldState MINIMO — 1 nacao (AAA), 2 cidades, banco publico,
// poucos bens/receitas/predios — a partir de tests/fixtures/pocket_world.json,
// sem carregar o mundo inteiro nem o Godot. O bloco `products`/`recipes` passa
// pelo MESMO parser do snapshot real (`save::definitions_from`); cidades,
// predios e grupos vem das listas do fixture.
//
// Contrato:
//   * o fixture NAO e dado de jogo: os numeros de producao seguem D-125/D-127
//     e o resto existe so para o world_state ser coerente;
//   * `core` guarda uma referencia para `ws` — PocketWorld nao e copiavel;
//   * os indices da D-125 que a fachada Godot resolve em `set_goods`
//     (gasolina/diesel/querosene/combustivel do equipamento) sao resolvidos
//     aqui, porque o parser de definitions nao os conhece.
//
// Documentacao: docs/testing/cpp_unit_tests.md.

#include "sim/world_state.h"
#include "sim/sim_core.h"

#include <cstdint>
#include <map>
#include <string>

namespace ogs::test {

// Caminho absoluto da fixture versionada (OPENGS_SOURCE_DIR fixado no CMake).
std::string fixture_path();

struct PocketWorld {
	WorldState ws;
	SimCore core{ws};

	PocketWorld();
	PocketWorld(const PocketWorld&) = delete;
	PocketWorld& operator=(const PocketWorld&) = delete;

	// Indice do bem pelo id (aborta o teste com -1 se o fixture nao o tiver).
	int product(const std::string& id) const;
	// Indice da receita pelo id em ws.recipes (-1 se nao existir).
	int recipe(const std::string& id) const;
	const City& city(const std::string& id) const;
	Building* building(const std::string& id);
	const Building* building(const std::string& id) const;

	// Perda declarada (co_loss) da receita no fixture; 0 se nao declarada.
	// O nucleo nao simula co_loss (o builder de dados valida a conservacao);
	// este campo deixa o teste de massa da D-127 ler o numero declarado.
	double declared_loss(const std::string& recipe_id) const {
		const auto found = co_loss.find(recipe_id);
		return found == co_loss.end() ? 0.0 : found->second;
	}

	std::map<std::string, double> co_loss;
};

// Digest FNV-1a de 64 bits do save serializado do mundo (determinismo).
std::uint64_t digest(const WorldState& ws, const SimCore& core);

} // namespace ogs::test
