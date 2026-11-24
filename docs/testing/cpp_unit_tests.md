# Testes unitários C++ do núcleo (FASE 2)

Testes **sem Godot**: um executável nativo (`sim_unit_tests`) que linka
`opengs_sim` (a biblioteca estática de `gdextension/src/sim/`) e o doctest
header-only. Nada de godot-cpp, cena, `.import` ou motor — por isso roda em
**milissegundos**.

- Framework: [doctest 2.4.12](https://github.com/doctest/doctest) (MIT),
  vendorizado em `gdextension/third_party/doctest/` com `LICENSE.txt`.
- Alvo CMake: `sim_unit_tests`, ligado por `-DOPENGS_BUILD_UNIT_TESTS=ON`
  (padrão OFF). A extensão Godot continua sendo compilada por padrão
  (`OPENGS_BUILD_EXTENSION=ON`); para rodar só os testes use
  `-DOPENGS_BUILD_EXTENSION=OFF` e não precise de `GODOTCPP_DIR`.
- Fonte dos testes: `gdextension/tests/` (todos os `.cpp` do alvo estão listados
  no `gdextension/CMakeLists.txt`).

## Compilar e rodar (Windows, toolchain do projeto)

```powershell
$cmake = "G:\tools\cpp-tools\Scripts\cmake.exe"
$ninja = "G:\tools\cpp-tools\Scripts\ninja.exe"
$cxx   = "G:\tools\llvm-mingw-20261104-ucrt-x86_64\bin\clang++.exe"

& $cmake -S gdextension -B gdextension/build-unit-tests -G Ninja `
    -DCMAKE_CXX_COMPILER="$cxx" -DCMAKE_MAKE_PROGRAM="$ninja" `
    -DCMAKE_BUILD_TYPE=Release `
    -DOPENGS_BUILD_UNIT_TESTS=ON -DOPENGS_BUILD_EXTENSION=OFF

& $cmake --build gdextension/build-unit-tests --target sim_unit_tests -j 4

# roda tudo (saída 0 = verde; != 0 = caso falhou)
& gdextension/build-unit-tests/sim_unit_tests.exe
# filtro por nome de caso (sintaxe do doctest)
& gdextension/build-unit-tests/sim_unit_tests.exe -tc="bank:*"
```

O diretório `gdextension/build-unit-tests/` é ignorado pelo Git e pelo mapa de
impacto (mesma regra de `gdextension/build*`). A fixture é lida do worktree em
runtime pelo caminho fixado na compilação (`OPENGS_SOURCE_DIR`).

## Integração com o runner

`tools/run_tests.ps1 -Tier quick` (e `pr`) roda as checagens de **CLI** do mapa
antes de qualquer Godot. A entrada `sim_unit_tests` em
`tools/test_impact_map.json`:

- `requires`: `gdextension/build-unit-tests/sim_unit_tests.exe` — se o
  executável não estiver compilado, a checagem vira **SKIP** (não derruba o
  tier);
- `fail_fast: true` — se o executável existir e falhar, o runner **aborta antes
  de abrir o Godot** e sai com código 1;
- roda **sempre** no quick/pr, independente do diff. Como qualquer mudança em
  `gdextension/src/**` pode alterar o núcleo, esse alvo é a primeira barreira
  (`gdextension/src/** → sim_unit_tests sempre`).

## Fixture "mundo de bolso"

`gdextension/tests/fixtures/pocket_world.json` + `pocket_world.{h,cpp}` montam
um `WorldState` mínimo:

- 1 nação (`AAA`), 2 cidades (`AGRA`, `PORTUS`), banco público;
- 12 bens (trigo, madeira, tábua, serragem, petróleo, gasolina, diesel,
  querosene, etanol, munição, chip, carne), 5 receitas e 3 prédios;
- 2 grupos donos das plantas; frota e quintis semeados pelo próprio núcleo
  (`init_fleets`/`seed_strata`) para o mundo ser **ponto fixo do save**.

O bloco `products`/`recipes` passa pelo **mesmo parser do snapshot real**
(`save::definitions_from`). O fixture declara os bens por nome (legível); o
helper converte nome → índice antes de entregar, porque o parser do save usa
índices. `co_loss` (perda declarada da D-127) não existe no `Recipe` C++: o
helper guarda o número em `PocketWorld::declared_loss()` para o teste de massa.

**Não é dado de simulação**: os números de produção seguem D-125/D-127 e os
preços existem só para o mercado não ser zero. Não use o fixture para calibrar
o jogo.

Uso típico:

```cpp
#include "pocket_world.h"

TEST_CASE("minha regra") {
    ogs::test::PocketWorld world;          // monta o mundo do JSON
    const int planks = world.product("planks");
    ogs::Building* sawmill = world.building("PORTUS_SAWMILL");
    ...
}
```

## Escrever um teste novo

1. Crie/edite um `gdextension/tests/test_<área>.cpp` (todos os arquivos estão
   listados explicitamente no `CMakeLists.txt`; ao criar um novo, acrescente-o
   ao `add_executable(sim_unit_tests ...)`).
2. Inclua `"doctest/doctest.h"` e `"pocket_world.h"`; o `main.cpp` já define
   `DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN`.
3. Use `TEST_CASE("D-xxx: regra em pt-BR")`, `CHECK`/`REQUIRE` e
   `doctest::Approx` para double. Nome do caso em pt-BR (aparece no log).
4. Teste **uma regra por caso**; prefira o helper puro do núcleo
   (`eff::step`, `bank::phase`, `save::to_value`…) em vez de rodar o mundo
   inteiro. Não invente números: use os parâmetros do `WorldState`/JSON.
5. Para determinismo use `ogs::test::digest(world.ws, world.core)` (FNV-1a do
   save serializado) — mesma semente/mesmo fixture ⇒ mesmo digest.
6. Rode `sim_unit_tests.exe` e depois `run_tests.ps1 -Tier quick` se quiser
   exercitar a integração do runner.

## Cobertura atual (36 casos, 195 asserções)

| área | casos | o que cobre |
|---|---:|---|
| Banco (D-128) | 6 | posição vazia; calote/seizure conserva dinheiro e `bank_money_delta == 0`; amortização destrói depósito e fecha `money_after = money_before + delta`; patrimônio/lucro retido; credor estatal; somas |
| Crise (D-126) | 5 | rescisão reduz caixa e trabalhadores com dinheiro conservado; crédito de giro só com banco existente (sem banco ⇒ sem crédito); renegociação cobra a multa uma vez e capitaliza o resto; hibernação de usina pela necessidade de pico × 1,15 (necessária não hiberna, excedente sim); leilão transfere o prédio com conservação |
| Eficiência (D-125) | 13 | rampa fábrica 30→90→100 % (90/180 d); mina 25→100 % em 240 d; fazenda 20→100 % em 365 d (85 % sem trator); pecuária 540 d; teto/equipamento (1,5x); média esperada; dias de transição por setor; retenção por família; refinaria só petróleo; modos 45/29/9, 38/36/9, 40/28/15; espelho do `efficiency.json` |
| Massa (D-127) | 5 | serraria 0,55/0,12/0,33; moagem 0,75/0,25; refinaria 0,45/0,29/0,09/0,17; dado real `recipes.json` |
| Save/load | 4 | roundtrip bit-exato do documento; dinheiro/dia/entidades; catálogo/receitas; load idempotente |
| Determinismo | 3 | mesma semente ⇒ mesmo digest (sem e com tick); dias andados mudam o digest |
| **Total** | **36** | |

Os casos da **D-126** (bateria Godot `tools/d126_crisis_test.gd`) estão portados
em `tests/test_crisis.cpp` (mundo de bolso, sem godot-cpp). As usinas do caso de
pico × reserva são estado de teste montado no próprio caso (o fixture não tem
parque); não são dado de simulação.

## Medição (24/11/2026, worktree `opengs-wt-cpptests`)

| item | valor |
|---|---:|
| build completo pós-merge D-126 (46 TUs do núcleo + 8 TUs de teste, `-j4`, Release) | **43,5 s** |
| build incremental (1 arquivo de teste alterado) | **~2,6 s** |
| execução dos 36 casos | **0,04 s** (5 execuções: 0,039–0,046 s) |
| passos: `tools/run_tests.ps1 -Tier quick` | primeiro, antes do Godot |

## Limites (declarados)

- O núcleo C++ é a biblioteca `opengs_sim`; a fachada Godot (`economy_sim.cpp`,
  `edge/`) **não** é compilada aqui (depende de godot-cpp). Se um teste precisar
  da conversão Value↔Variant, ele não pertence a este alvo.
- `co_loss` é validação do builder de dados; o teste confere o número declarado
  no `recipes.json` real, não uma simulação de perda.
- O fixture tem 2 cidades e não substitui a bateria Godot de mundo real; é o
  nível de unidade (regra isolada) da pirâmide.
