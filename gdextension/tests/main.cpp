// Runner dos testes unitarios C++ do nucleo (FASE 2).
//
// doctest header-only (MIT, gdextension/third_party/doctest). O executavel
// linka apenas opengs_sim: nenhum godot-cpp, nenhum engine, nenhuma cena.
//
//   sim_unit_tests                 # roda tudo
//   sim_unit_tests --list-test-cases
//   sim_unit_tests -tc="bank*"     # filtro por caso (sintaxe do doctest)
//
// Saida 0 = verde; != 0 = ao menos um caso falhou (o runner headless
// tools/run_tests.ps1 -Tier quick usa esse codigo para falhar rapido).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"
