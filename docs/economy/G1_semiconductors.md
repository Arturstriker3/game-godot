# G1 — Semicondutores, microchips e drone FPV

Status: **parte 0 implementada** (núcleo, save, rename de exibição e teste
unitário). As partes seguintes (bens `microchips`/`fpv_drone`, receitas da fab
de semicondutores e telas) ainda não estão no código. Decisão do dono registrada
no `GAME_STATE.md` (§ "Decidido pelo dono", G1).

Esta nota cobre apenas a **parte 0**: a infraestrutura de receita que as partes
seguintes usam — prazo de obra próprio e classe da planta — e o rename de
exibição de `chips` para **Semicondutores**. Nenhuma receita do jogo declara os
campos novos ainda, então o mundo é bit-idêntico ao `148d4df6`.

## Campos novos da receita

`Recipe` (`gdextension/src/sim/sim_types.h`) ganhou dois campos opcionais. Ambos
são lidos de `data/definitions/economy/recipes.json` por
`EconomySim::set_recipes` e do save por `recipes_from`.

| Campo | Tipo | Default | Consumidor | Efeito na simulação |
|---|---|---|---|---|
| `construction_days` | inteiro ≥ 0 | `0` | `add_construction_project` (núcleo) e `brain_construction_days` (forecast D-131) | prazo-base da obra da fábrica; `0`/ausente mantém `kConstructionFactoryDays` |
| `plant_class` | string UTF-8 | `""` | status da cidade (`buildings[].plant_class`) | **nenhum** — metadado de exibição/i18n |

### `construction_days`

- Só vale para **fábrica** (`tmpl.is_recipe` / `project.factory`): receita de
  refino, química, eletrônica etc. Fazenda, mina e extração primária continuam
  usando `kConstructionFoodDays` (15), `kConstructionMineDays` (45) e
  `kConstructionPrimaryDays` (30), porque a obra delas não nasce de uma receita.
- Com valor declarado > 0, ele **substitui** `kConstructionFactoryDays` (60)
  como prazo-base. A escala por tamanho continua igual e é aplicada depois:
  `size_ratio = max(1, capacidade) / 10`, `size_scale = clamp(1 + 0,5·log2(size_ratio), 0,5, 2,5)`,
  `duration_days = max(1, round(base_days · size_scale))`. Ou seja, declarar 180
  não garante 180 dias: uma planta de 40 t/dia entrega 180 · 1,5 = 270.
- O prazo é **cronograma**, não custo: capex, material, folha de obra, pausa e
  reembolso continuam pelas regras da §15/D-086. O campo não cria desconto nem
  aceleração de rampa (a rampa D-125 começa no dia em que a obra termina).
- No forecast do cérebro (D-131 R1), `brain_construction_days` usa a receita
  **escolhida** pelo investidor (privado: `recipe_index` do `ai_best_recipe`;
  grupo: `recipe_index` do candidato). O template sem receita escolhida cai na
  primeira receita da saída, a mesma que já fornecia os parâmetros do template.
  `0`/ausente mantém a tabela como antes.
- Parser: número finito e > 0 vira inteiro; acima de 3650 é capado em 3650;
  negativo, zero, ausente ou não finito vira 0.

### `plant_class`

- Metadado puro: **não** entra em eficiência, folha, opex, preço, produção nem
  no digest de trajetória. Existe para a UI/status nomear a classe da planta
  (ex.: `fab`, `refinery`, `chemical`) quando as receitas da G1 forem escritas.
- Publicado em `get_city_status()` como `buildings[].plant_class`; construção
  sem receita publica `""`. O status é reconstruído quando a receita do prédio
  muda, então o campo acompanha a troca de produto.
- Parser: `String(entry.get("plant_class", ""))` convertido para UTF-8; vazio é
  o default (nenhuma classe declarada).

## Save: aditivo, sem bump de versão

`recipes_to` grava `construction_days` e `plant_class` em cada entrada do array
`definitions.recipes`; `recipes_from` lê os dois como **opcionais**.

- **Save v42 antigo** (escrito antes desta mudança) não tem as chaves. A carga
  usa os defaults do struct (`0` e `""`), que são exatamente o comportamento
  anterior: tabela de prazo atual e nenhuma classe. Nada é inventado para trás.
- **Compatibilidade de ida e volta:** depois do primeiro save no binário novo,
  as chaves aparecem no array e o round-trip é bit-exato. O formato geral do
  save não mudou — nenhum bloco novo na raiz, nenhum campo obrigatório — por
  isso a versão **continua v42**, sem migração.
- Saves ainda mais antigos (v41 e anteriores) também não têm o bloco
  `definitions.ai_brain`; o caminho aditivo da D-131 continua valendo e os dois
  campos seguem o mesmo princípio: ausência de chave = default documentado.

## Rename de exibição: `chips` → Semicondutores

- `goods.json`: `name` do bem `chips` passa de "Chips" para "Semicondutores".
  O **id não muda** (`chips`), então save, receitas, mercados e digest ficam
  intactos.
- i18n pt-BR/en/es: `product.chips` e `resource.chips` passam a
  Semicondutores / Semiconductors / Semiconductores. A chave
  `building.semiconductor_fab` (Fab de semicondutores / Semiconductor fab / Fab
  de semiconductores) já fica reservada para a parte seguinte.
- `tools/icon_forge/resource_icons.json`: rótulo do ícone `chips` acompanha o
  rename; o desenho não muda.
- Nenhum número de simulação é tocado: nome é apresentação.

## Verificação

- **Teste unitário C++** (`gdextension/tests/test_ai_brain.cpp`, caso
  `G1: construction_days da receita entra na obra e no forecast`): com a receita
  `chip` do mundo de bolso, `add_construction_project` devolve
  `kConstructionFactoryDays` com `construction_days = 0`, 180 com
  `construction_days = 180` (capacidade 10 t/dia, escala 1,0), e
  `ai_brain_construction_days` acompanha os dois casos pela receita escolhida.
- **Bit-identidade:** `tools/c1_world_ai_365.gd --days=365` (cérebro ligado,
  mesmo caminho da D-131) neste worktree contra o baseline ON do `148d4df6`
  (`G:\opengs_tmp\d131d_365_on.json`, `trajectory_sha256 67d950b7…`). Resultado
  nesta rodada: ver abaixo.
- **Tier pr completo** (`-Tier pr -Base balance/global-resources`): ver abaixo.

### Resultado desta rodada

- 365 d ON: `trajectory_sha256` =
  `67d950b7fc0ceaa4dc9c2d4a3f9ee36d1776a4019f255f0ae75de1cf7db6cb26`; baseline
  ON do `148d4df6` = o mesmo valor. **Bit-idêntico:** 0 falências, 850
  investimentos (726 privado / 124 grupo), comida 93/179 — nenhuma receita
  declara `construction_days`/`plant_class`, então o mundo não muda. Rodada:
  `c1_world_ai_365.gd --days=365`, 137,35 s, `G:\opengs_tmp\g1_p0_wt_365_on.json`.
- `-Tier pr`: **45/45 OK** (run `20261125_042514`: 44 testes Godot +
  `sim_unit_tests` CLI; inclui `goods_data_test` e `c1_world_ai_90`). O caso G1
  isolado (`sim_unit_tests.exe -tc="G1*"`) passa com 8 asserções.

## Fora do escopo (partes seguintes)

- Bens `microchips` e `fpv_drone`, receitas da fab de semicondutores, mudança
  das cadeias de computador/celular/robô/drone e arte de ícones.
- Telas/listas que mostram `plant_class`; a chave i18n já existe, o componente
  visual ainda não.
- Calibração de prazo de obra por receita: os valores só entram quando o Game
  Systems Designer declarar as receitas da G1. Até lá, `construction_days = 0`
  em todas as receitas e o comportamento é o da tabela atual.
