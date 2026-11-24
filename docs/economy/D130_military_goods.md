# D-130 — Produtos militares novos

**Data:** 2026-11-24
**Branch:** `feat/military-goods` (worktree `G:/projetos/opengs-wt-milgoods`)
**Base:** `balance/global-resources` `ca783dba`
**Escopo:** dados (`goods.json`, `recipes.json`, `needs.json`, i18n, ícones), lista de paiol do núcleo, teste C++/GDScript e doc. Sem merge/push.

## 1. Bens e receitas

Todos os coeficientes são **1 unidade por insumo** (`coef_source: default_unit`), o padrão do arquivo. `jobs_per_ton` 4, `wage` 2, `opex_per_ton` 16 e `capital_per_capacity` 12.000 são **iguais aos de `small_arms`/`infantry_gear`** (`params_source: reuse:small_arms/infantry_gear`), declarados na spec e reproduzidos pelo builder. A aramida fica no setor químico (mesma fábrica dos químicos); os demais no setor militar (mesma fábrica de `small_arms`/`infantry_gear`). Os preços são derivados pela fórmula do `tools/build_goods_chain.py` (margem de intermediário/final/durável do próprio arquivo), nunca digitados.

| id | nome pt-BR | setor | kind/tier | insumos |
|---|---|---|---|---|
| `aramid` | Aramida | chemical | intermediate/semi | `chemicals` + `refined_sulfur` |
| `ballistic_ceramic_plate` | Placa cerâmica balística | military | intermediate/semi | `bauxite` + `aramid` |
| `tactical_equipment` | Equipamento tático | military | final/final | `aramid` + `ballistic_ceramic_plate` + `rubber` + `synthetic_fabric` |
| `command_equipment` | Equipamento de comando | military | final/final | `computer` + `electronic_components` + `batteries` + `wires` |
| `drone_jammer` | Bloqueador de drones | military | final/final | `circuit_board` + `electronic_components` + `batteries` + `plastic` |
| `at_launcher` | Lança-foguetes anti-tanque | military | final/final | `steel` + `hardware` + `planks` |
| `at_rocket` | Foguete anti-tanque | military | final/final | `explosives` + `gunpowder` + `steel_sheet` |
| `aa_launcher` | Lançador antiaéreo portátil | military | final/final | `sensors` + `electronic_components` + `explosives` + `aluminum_ingot` |

Cadeia fechada: `chemicals`/`refined_sulfur` → **aramida** → **placa cerâmica balística** → **equipamento tático**. `aramid` e `ballistic_ceramic_plate` são bens intermediários (consumidos pela receita seguinte); os seis finais entram na cesta institucional `army`.

`infantry_gear` mantém o **id** e passa a exibir **"Equipamento de apoio"** (en `Support equipment`, es `Equipo de apoyo`).

## 2. Demanda militar declarada

`needs.json` (gerado da spec) ganhou o bloco `military_demand`:

- `unit`: unidade de referência por soldado/ano;
- `params_source`: `declared:unit_reference`;
- `per_soldier_unit_year`: **1,0** para `uniforms`, `infantry_gear`, `small_arms`, `ammunition` e **1,0** para `tactical_equipment`, `command_equipment`, `drone_jammer`, `at_launcher`, `at_rocket`, `aa_launcher`.

Não existe efetivo militar no modelo nem âncora observada de consumo por soldado; o valor é **default de balanceamento declarado**, proporcional 1:1 aos itens que já estavam na lista `army`. Não é dado de simulação e o núcleo ainda não consome essa cesta (a lista institucional é declaração de intenção, como as demais tags `use`). A lista `army` de `needs.json` passa a incluir os seis bens finais novos.

## 3. IA, armazenagem e listas fixas

- `gdextension/src/sim/storage.cpp`: os sete bens militares novos entram em `kMagazineProducts` (paiol). A aramida fica no armazém geral (química).
- `data/definitions/economy/goods_storage_map.json`: `military_depot` para a placa, os três equipamentos e os três lançadores/foguetes (a placa e o equipamento tático têm `general_warehouse` como secundário); apenas `aramid` cai no `general_warehouse` declarado. O mapa é gerado por `tools/build_storage_g2.py`, que também ganhou as regras explícitas.
- `needs.json` institucional `army` é a lista que a IA de importação/consumo consulta quando a cesta for ligada; os novos ids já estão nela.

## 4. Ícones

Fontes aprovadas (repo privado `G:/projetos/opengs-icons-private/gemini_weapons/products/`). A primeira leva foi processada localmente (fundo cinza `#D9D9D9` aproximado → alfa, borda suavizada, recorte ao objeto com ~10% de margem, centralização); os **ícones finais aprovados pelo dono** vieram depois de `products/ingame_v2/` e `ingame_v2/64/` e foram copiados **byte a byte**, sem reprocessar:

| origem (final, `ingame_v2`) | destino |
|---|---|
| `aramid.png` | `assets/icons/illustrated/aramid.png` |
| `ballistic_ceramic_plate.png` | `assets/icons/illustrated/ballistic_ceramic_plate.png` |
| `tactical_equipment.png` | `assets/icons/illustrated/tactical_equipment.png` |
| `command_equipment.png` | `assets/icons/illustrated/command_equipment.png` |
| `drone_jammer.png` | `assets/icons/illustrated/drone_jammer.png` |
| `at_launcher.png` | `assets/icons/illustrated/at_launcher.png` |
| `at_rocket.png` | `assets/icons/illustrated/at_rocket.png` |
| `aa_launcher.png` | `assets/icons/illustrated/aa_launcher.png` |
| `infantry_gear.png` | `assets/icons/illustrated/infantry_gear.png` |
| `uniforms.png` | `assets/icons/illustrated/uniforms.png` |
| `shoes.png` | `assets/icons/illustrated/shoes.png` |

Cada destino tem **128×128 RGBA** e **64×64 RGBA**; os 1.024 px originais não entram no repositório. O fallback SVG do Icon Forge (`assets/icons/resources/<id>.svg`) também foi gerado para os oito ids novos, com `resource_icons.json` sincronizado (label/cor/tier).

## 5. Verificação

- `tools/build_goods_chain.py build` e `check`: 0 problemas; 123 bens, 122 receitas.
- `tools/icon_forge/icon_forge.py check`: 0 problemas.
- `sim_unit_tests`: 40/40 casos, 344 asserções (casos D-130 novos em `gdextension/tests/test_military_goods.cpp`).
- `tools/goods_data_test.gd`: verde, incluindo o novo check de PNG ilustrado 128/64 com alfa para **todo** bem.
- `tools/run_tests.ps1 -Tier pr -Base balance/global-resources`: resultado na seção 6 (preenchido após a rodada).

## 6. Rodada de 90 dias (`-Tier pr`)

Comando: `tools/run_tests.ps1 -Tier pr -Base balance/global-resources` (lock `G:\opengs_tmp\opengs_godot_test.lock`, tmp `G:\opengs_tmp`), com a extensão Debug recompilada (`-j4`, inclui `storage.cpp`). Run `20261124_190224`: **45/45 OK**, 0 falhas/crash/timeout; parede **1.859,7 s** (~31,0 min). `sim_unit_tests` 0,28 s; `c1_world_ai_90` 120,9 s no runner.

Rodada `c1_world_ai_90` (JSON `G:\opengs_tmp\c1_world_ai_90_d130_dllnew.json`):

| métrica | valor |
|---|---:|
| comida abaixo de 0,975 | **83/179** (média 0,9313; mediana 0,9778) |
| falências industriais | **0** |
| negócios de serviços fechados | 582 |
| violações do invariante | **0** |
| dias travados | 0 |
| drift | 0,011852 |
| dinheiro (início → fim) | 6.523.551.982,90 → 6.600.868.292,74 |

Comparação com a main (controle): a mesma ferramenta e os mesmos argumentos em `balance/global-resources` `ca783dba`, com a DLL de `bin/` também recompilada em Debug, produzem **a mesma rodada** — todos os campos do JSON batem e só `wall_s` difere (`G:\opengs_tmp\c1_world_ai_90_main.json`). A D-130, portanto, **não altera** o resultado de 90 d: a cesta `army` é declaração (o núcleo não a consome), nenhum bem novo é produzido no horizonte e a lista de paiol só muda a classificação de armazém.

Nota de escopo: a rodada anterior (`20261124_181633`, 596 fechamentos, drift 0,001321) usou uma DLL de `bin/` mais velha que o fonte da main; depois da recompilação dos dois lados, os números são 582 e 0,011852 em ambos. A diferença era da DLL defasada, não da D-130.
