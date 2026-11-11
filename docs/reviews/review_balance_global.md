# Revisão — branch `balance/global-resources`

- **Data:** 2026-11-10
- **Escopo:** commits `main..HEAD` (9cb2e96 ui, e4cf69f icons, 17a2fc7 map, 6e8098b economy, c275741 tools)
- **Tipo:** somente leitura (nenhum arquivo de código/cena/dados alterado)
- **Artefato:** este relatório

## 1. Execução (evidência reproduzível)

Executável: `C:\Users\artur\OneDrive\Área de Trabalho\Godot_v4.7.2-stable_win64.exe` (4.7.2.stable).

Suíte headless (runner `SceneTree`), cada teste via `godot --headless --path . --script res://tools/<teste>.gd`:

| Teste | Saída | Exit |
|---|---|---|
| economy_live_path_test | "OK: as 8 nações receberam aptidão e depósitos sem load_data() prévio." | 0 |
| economy_world_test | "OK: modelo demo rodou 60 dias no cenario real sem violacao." drift=-0.0000 | 0 |
| economy_integration_test | "OK: geografia, cadeia, investimento e save/load validados." | 0 |
| economy_ai_test | "OK: cenários de alimento, ferro e anti-loop reagiram de forma plausível." | 0 |
| population_demo_test | "OK: conservação e ciclo econômico validados." | 0 |
| economy_long_run_test | "OK: 365 dias sem violação de conservação." | 0 |
| economy_bistability_test | "OK: GBR sustentada (>= 70%) sob a perturbação de culturas." | 0 |
| deposit_ring_test | "PASS: ponto, hover, seleção e pick uniformes (±1px)" | 0 |
| wheat_info_test | "OK: ficha agrícola e pick de trigo consistentes" | 0 |

Validação de dados:

| Comando | Saída | Exit |
|---|---|---|
| `python tools/validate_scenario_data.py` | "tudo ok — 0 erros, 0 aviso(s) em 10 arquivo(s)" | 0 |

> **Limitação do validador:** `validate_scenario_data.py` cobre apenas
> `cities/ports/buildings/nations` (csv+json), `goods.json` e `i18n.csv`. Ele **não**
> valida `data/definitions/natural/deposits.json` nem `reserve_calibration.json` —
> exatamente onde estão os achados de dados desta revisão.

`economy_panel_test.gd` não foi rodado (BOM pré-existente em `core/ui/city_economy_panel.gd`,
conforme `AGENTS.md`; fora do escopo desta branch).

## 2. Achados por severidade

### BLOQUEANTE

Nenhum. A suíte completa passa, a conservação de dinheiro é verificada por
`demo_check_invariants` (drift < tolerância) e nenhum teste falhou.

### ALTO

#### A1. Depósitos minerais nacionais aplicados por cidade (duplicação em FRA)
- **Arquivo:linha:** `core/economy/geography_service.gd:346-353` (`apply_to_sim`)
- **Evidência:** `apply_to_sim` usa `deposits_for_nation(code)` (agregado de **todos** os
  territórios da nação) e chama `demo_set_deposit` para **cada cidade** da nação. FRA tem
  2 cidades (`FRA-PAR` e `FRA-CAY`, `scenario/cities.json:14-72`). Saída do
  `economy_live_path_test` mostra a lista de FRA duplicada:
  `FRA ... depósitos=iron, bauxite, lead, tungsten, uranium, copper, phosphate, oil, iron, bauxite, lead, tungsten, uranium, copper, phosphate, oil`.
  Resultado: a reserva nacional é contada **2×** (uma por cidade) e a geografia é incorreta
  (Cayenne recebe minério do continente francês). `demo_set_deposit`
  (`economy_sim.cpp:2545`) é idempotente por **cidade**, não por nação, então não há
  proteção contra a duplicação.
- **Correção sugerida:** declarar a reserva nacional uma única vez (na capital/nação) e
  distribuir por cidade proporcionalmente à produção, ou atribuir depósitos pelo território
  da própria cidade (`deposits_for(city_id, center)`), não pelo agregado nacional.

#### A2. Save antigo (v1) carrega sem migrar índices de produto
- **Arquivo:linha:** `gdextension/src/economy_sim.cpp:2903-3072` (`demo_from_dict`), guarda em `2905-2909`, buildings em `3050-3072` (esp. `3063`)
- **Evidência:** após remover farinha/pão, o enum `DemoProduct` deslocou **-2**:
  `Iron 3→1, Rice 4→2, Maize 5→3, …` (`economy_sim.h:250-256`). `demo_from_dict` aceita
  `version <= kDemoSaveVersion` (rejeita só versões **futuras**) e lê `output_product`
  diretamente, **sem migração**. Um save v1 com moinho (farinha, índice 1) e padaria (pão,
  índice 2) passa a ser interpretado como mina de ferro (1) e fazenda de arroz (2); o ferro
  (3→na verdade 3 vira milho). O comentário `economy_sim.cpp:60-61` ("Saves v1 … continuam
  carregando") é enganoso: carrega sem crash, mas com mapeamento de produto corrompido.
- **Correção sugerida:** no `demo_from_dict`, quando `version < 2`, remapear
  `output_product/input_product` pelo deslocamento de -2 (índices ≥1) e descartar
  flour/bread; ou recusar v1 explicitamente em vez de aceitá-lo silenciosamente.

#### A3. Demanda externa de ferro hardcoded; teto calibrado ignorado
- **Arquivo:linha:** `gdextension/src/economy_sim.cpp:729-730` (`kDemoIronExportDemand = 20.0`,
  `kDemoIronProduct = 1`), `1716-1733` (fase 4c); `1749` (loop inicia em `kDemoFirstRawProduct = 2`)
- **Evidência:** `GeographyService.apply_to_sim` calcula e declara o teto de ferro via
  `demo_set_external_demand(city_id, "iron", ceiling)` (`geography_service.gd:356-359`), mas o
  loop da fase 4d (`economy_sim.cpp:1749`) começa no produto 2 (arroz) e **pula o ferro (1)**.
  O ferro é exportado pela fase 4c com `min(stock, 20.0)` fixo, ignorando a reserva calibrada
  e o horizonte de ~25 anos. O teto data-driven de ferro é escrito mas nunca lido.
- **Correção sugerida:** remover o caso especial 4c e deixar o ferro fluir pelo mesmo
  mecanismo `external_demand` (o loop já pula alimentos via `demo_food_calories`), ou
  iniciar o loop em 1.

### MÉDIO

#### M1. Magnitude de reservas implausível + `source` ausente nos depósitos
- **Arquivo:linha:** `data/definitions/natural/deposits.json` (`_meta`); registros sem `source`
- **Evidência:** `economy_live_path_test` reporta `GBR reserva=40833349 kt` (~40,8 Gt) para o
  Reino Unido — ordens de grandeza acima das reservas reais. O `_meta` declara
  "quantity/quality/depth/cost estimated deterministically from sites and status" (MRDS
  "Occurrence", não reserva). 5727 de 5730 depósitos **não têm campo `source`** (só
  `data_quality`), a proveniência fica apenas no `_meta`. Documentado como "ordem de
  grandeza", mas os valores são impossíveis como reservas literais.
- **Correção sugerida:** rotular a grandeza como "recurso estimado" (não "reserva"), adicionar
  `source`/unidade por registro e refletir a incerteza na UI de recursos.

#### M2. Campo `kind` com semântica inconsistente (tipo × produto)
- **Arquivo:linha:** `gdextension/src/economy_sim.cpp:997` (`building.kind = building.output_product`)
- **Evidência:** o struct documenta `kind = 0` como "fazenda (único tipo hoje)"
  (`economy_sim.h:286`), e `initialize_demo`/`demo_build` deixam `kind = 0`. Mas
  `initialize_demo_world` sobrescreve `kind` com o **índice do produto** (wheat=0, rice=2…).
  Serialização em `2874`/restauração em `3057` mantêm o valor, mas o significado muda entre
  os caminhos. Se qualquer UI ler `kind` como tipo, comporta-se de forma diferente no modo mundo.
- **Correção sugerida:** manter `kind` como tipo (0) e usar `output_product` para o produto;
  ou renomear o campo para eliminar a ambiguidade.

#### M3. `sulfur` e `diamond` sem calibração (tratamento inconsistente)
- **Arquivo:linha:** `data/definitions/natural/reserve_calibration.json:262-281`
- **Evidência:** `world_total_kt: "unknown"` e `shares: {}` para enxofre e diamante
  (documentado: "mantém o dado MRDS"). Os demais 12 recursos são rescalados para reservas
  publicadas; estes dois permanecem com magnitude MRDS (ocorrência), criando uma mistura de
  grandezas no mesmo arquivo de depósitos.
- **Correção sugerida:** documentar na UI/saída que esses dois recursos não são calibrados,
  ou excluí-los da exportação até haver fonte publicada.

#### M4. Comentários stale sobre índices de produto
- **Arquivo:linha:** `gdextension/src/economy_sim.cpp:731-735` (`kDemoFirstRawProduct = 2` com
  "2=arroz"), `60-61` ("v2: … e 18 produtos")
- **Evidência:** o comentário chama arroz (índice 2) de "primeiro produto bruto sem consumo
  local", mas arroz é alimento da cesta B3 e **é** consumido (o loop só o pula via
  `demo_food_calories`). E `kDemoProducts = 21`, não "18 produtos".
- **Correção sugerida:** reescrever os dois comentários para refletir o estado atual.

### BAIXO

#### B1. `soy` marcado `food: true` mas fora da cesta calórica
- **Arquivo:linha:** `data/definitions/economy/goods.json:104-109`; `economy_sim.h:264-268`
- **Evidência:** `soy` tem `"food": true`, mas `kDemoFoodProductIds = {0,2,3,16}` exclui soja
  (17) — a soja é vendida como matéria-prima (fase 4d). Sinal inconsistente para consumidores
  do `goods.json` (`food` deveria refletir a cesta).
- **Correção sugerida:** definir `"food": false` para `soy`, ou documentar a exceção.

#### B2. Default de `suitability` contradiz comentário "zero-inicializado"
- **Arquivo:linha:** `gdextension/src/economy_sim.h:379` (`suitability[kDemoProducts] = {1.0, 1.0, 1.0}`)
- **Evidência:** o array inicializa ferro(1) e arroz(2) em 1.0, mas o comentário em
  `initialize_demo_world` (`economy_sim.cpp:971-973`) afirma "os demais recursos ficam em 0.0".
  Latente no caminho real (o `apply_to_sim` sobrescreve tudo), mas engana na leitura.
- **Correção sugerida:** alinhar o comentário ao valor real (`{1.0, 1.0, 1.0, 0, …}`).

#### B3. Warnings de imagem no headless (pré-existente, fora da branch)
- **Evidência:** `deposit_ring_test`/`wheat_info_test` emitem
  `WARNING: Loaded resource as image file … world_sdf.png / world_province.png`
  (`map/territory_map.gd:278-284`). Não é introduzido por esta branch (os arquivos de mapa
  não foram alterados), mas os dois testes poluem o log.
- **Correção sugerida:** importar as imagens como `Image` (não carregar como textura) nesses
  testes, em tarefa separada.

## 3. Itens verificados sem problemas

- **Índices de produto:** o enum `DemoProduct` (21) e `kDemoProductNames[21]` batem com a
  ordem de `goods.json` após a remoção de farinha/pão; `kDemoIronProduct = 1` e o índice de
  ferro nos testes (`produced_total[1]`) foram atualizados corretamente.
- **Serialização `demo_to_dict`/`demo_from_dict`:** simétrica campo a campo (cidades, pop,
  mercados, buildings, trânsito, depósitos, external_demand); save/load round-trip validado
  pelo `economy_integration_test`. Único gap é a migração de índice (A2).
- **Divisão por zero:** `scarcity` (`economy_sim.cpp:2049-2050`) protegida por
  `std::max(stock+supply, 0.001)`; preços com `clamp` de banda e passo diário.
- **Conservação de dinheiro:** `demo_check_invariants` (1084-1136) valida caixa/estoque/preço
  e reverte o tick em violação; `economy_world_test` drift=-0.0000.
- **Anti-loop de IA:** `government_ai.gd`/`private_investor.gd` implementam cooldown (30 d),
  teto por tipo (6) e utilização mínima (0.60); testado por `_scenario_no_build_loop` (730 d).
- **Testes:** **nenhum assert foi removido ou afrouxado**. `economy_ai_test.gd` ganhou um
  cenário novo (anti-loop) e apenas renomeou "farinha/pão" → "alimento/trigo".
- **i18n:** chaves removidas (`product.flour`, `product.bread`, `map.legend.city`) sem
  referências restantes; novas (`map.legend.uncheck_all_tip`, `map.legend.situation.*`,
  `map.legend.depth*`) presentes em pt/en/es.
- **UI:** `CHECK_ALL_TEXTURE`/`check_all.svg` removidos sem referência órfã; `product_icons.gd`
  lista as 21 matérias-primas e os 21 SVGs existem em `assets/icons/resources/`.
- **reserve_calibration.json:** `data_quality` presente, `source` presente, `shares` somam
  1.0, sem país duplicado.

## 4. Contagem

| Severidade | Qtd |
|---|---|
| Bloqueante | 0 |
| Alto | 3 |
| Médio | 4 |
| Baixo | 3 |

## 5. Resolução (aplicada em 2026-11-10)

| ID | Achado | Status | Correção aplicada |
|---|---|---|---|
| A1 | Depósitos nacionais duplicados por cidade (FRA 2×) | ✅ Corrigido | Nova regra em `GeographyService.apply_to_sim`: cada depósito vai a **UMA** cidade — a cidade da **mesma nação** mais próxima, restrita à **mesma landmass** (proxy de continente via `landmass` das cidades). Guiana não herda minério da França continental (e vice-versa); soma das reservas por nação == soma dos depósitos. Validado por `economy_live_path_test` (a: sem duplicação; b: DEU/GBR/FRA mantêm carvão/ferro/bauxita; c: Caiena isolada). |
| A2 | Save v1 carrega com índices de produto deslocados | ✅ Corrigido | `kDemoSaveVersion = 3` + migração em `demo_from_dict` para `version < 3` (remapeia índices, descarta farinha/pão). |
| A3 | Demanda externa de ferro hardcoded (`20.0`), teto calibrado ignorado | ✅ Corrigido | Ferro flui pelo teto `external_demand` data-driven (reserva/25 anos), sem caso especial 4c. |
| M1 | Reservas implausíveis + `source` ausente | ✅ Corrigido | `source` presente em todos os registros (MRDS/suplemento curado); grandeza rotulada "recurso estimado" + calibração B6. |
| M2 | `kind` com semântica inconsistente (tipo × produto) | ✅ Corrigido | `kind` mantido como tipo; produto em `output_product`. |
| M3 | `sulfur`/`diamond` sem calibração | ⚠️ Documentado | Mantido o dado MRDS (sem total publicado); registrado no `_meta`. |
| M4 | Comentários stale de índices | ✅ Corrigido | Comentários reescritos para o estado atual (21 produtos). |
| B1 | `soy` com `food: true` fora da cesta | ✅ Corrigido | `goods.json`: `soy` → `"food": false`. |
| B2 | Default de `suitability` contradiz comentário | ✅ Corrigido | Comentário alinhado ao valor real. |
| B3 | Warnings de imagem no headless | ⚠️ Pré-existente | Fora da branch; tarefa separada. |
| **B7** | **(novo)** depósitos-ruído "0,0 t" após a calibração | ✅ Corrigido | Formatação adaptativa (kg/t/kt/Mt/Gt) em `hud.gd::_format_mass`; piso de relevância em `calibrate_reserves.py` (remove depósito `< 0,1%` da reserva nacional **E** `< mínimo absoluto por recurso`); `build_resource_map.py` preserva 3 casas decimais. **452 depósitos removidos** (ouro 237, chumbo 81, urânio 41, cobre 40, tungstênio 31, ferro 19, terras raras 3). Marcadores minerais: **2.859 → 2.766**. |

Suíte pós-resolução (headless, exit 0): `economy_live_path`, `economy_world`,
`economy_integration`, `economy_ai`, `population_demo`, `economy_long_run`,
`economy_bistability`, `deposit_ring`, `wheat_info` + `validate_scenario_data.py`
(0 erros) + `balance_review.gd` (730 d, **8 nações a 100%** de satisfação alimentar,
**0 falências**, 0 violações, dinheiro conservado).

