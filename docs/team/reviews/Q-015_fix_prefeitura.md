# Q-015 — frente B: prefeitura (expansão/adensamento) — correção do gate e da decisão

- **Branch:** `fix/q015-prefeitura`, base `a8d7984` (`balance/global-resources`).
- **Worktree:** `G:/projetos/opengs-wt-q15b` (a `opengs-wt-w2` estava suja).
- **Data:** 2026-11-17. **Dono:** gerente-simulacao. **Modelos:** DeepSeek Flash apenas.
- **Ambiente:** Godot 4.7.2 stable `--headless`, CMake Ninja + clang (llvm-mingw 20261104),
  DLL debug da própria worktree (`65812100…`, 23.285.248 B; as medições rodaram com o mesmo
  código, só comentários mudaram depois). Editor Godot aberto em outro
  processo; nenhum processo de outra tarefa foi tocado.

Escopo: `gdextension/src/sim/city_gov.cpp/.h`, a lógica de decisão/gate de `urban_land.cpp`
(`start_project`/`committed_cost`) e `tools/city_gov_test.gd`. `housing.cpp`, constantes de
capacidade de zona (FAR, m² por unidade/trabalhador), a função de ocupação, `save_load.cpp` e
`GAME_STATE.md` **não** foram alterados.

## 1. Causa confirmada (B3)

O parecer dizia "`gov.projects = []` e `month_expense_infrastructure = 0` nas 6 cidades; suspeita:
`start_project` compara `committed_cost(owner="state") + custo` com o caixa municipal". O que a
medição e o código mostram:

1. **`gov.projects` não é evidência de "nenhuma obra".** Depois da fila única (§15+§19,
   `69e2790`), a obra urbana é um `DemoConstructionProject` (`kind = CONSTRUCTION_URBAN_INFRA`),
   não um `CityGovProject`. `gov.projects` fica vazio por desenho. O campo decisivo é
   `month_expense_infrastructure`, e era ele que zerava.
2. **Causa do zero medido no parecer: o gatilho de ocupação, não o caixa.** A amostra do parecer
   (BRA-SAO, JPN-TOK, NGA-LAG, IND-MUM, USA-NEW, AUS-WEI) tem ocupação RES **0,086–0,404** no
   dia 0 depois da correção de população (`29e244d`); Mumbai caiu de **1,963** (mundo antigo) para
   **0,240**. Nenhuma passa de `kBusyOccupancy = 0,90` (`city_gov.h:51`), então `decide_works`
   nunca entra no ramo de obra. Isso foi confirmado empiricamente: em **NPL-POK** (ocupação RES
   **1,375**, a única das 834 cidades acima do gatilho no dia 0), a prefeitura **já abre obra todo
   mês** neste HEAD (ver tabela "antes").
3. **O gate apontado é um bug real, latente** (`urban_land.cpp:515-535` antes da correção), e foi
   provado com A/B: `start_project` somava `committed_cost(ws, city, "state")` ao custo e comparava
   com `payer->cash` (caixa municipal). Contas diferentes entravam na mesma régua:
   - as obras **§15 de prédio do Estado** saem de `city.government_budget`
     (`economy_sim.cpp:4674-4678`, `demo_build`), nunca do caixa municipal;
   - as obras que o **município** paga (a urbana em `urban_land.cpp:562` e a moradia do governo em
     `housing.cpp:543-545`) já debitam o custo inteiro no ato, então o resíduo
     (`total_cost - paid`) era cobrado duas vezes.

   Efeito medido no teste novo (`city_gov_test`, caso `_test_full_city_cash_gate`): cidade cheia,
   caixa municipal 3.000.000 e uma obra §15 estatal de 3.000.000 em voo →
   **gate antigo: nenhuma obra**; **gate corrigido: obra de 0,4440 km² / 1.649.170 XGS, do caixa
   municipal**, concluída.

A correção também achou dois defeitos de decisão no mesmo caminho:

4. **O mês gastava o caixa inteiro.** A expansão comprava `caixa / custo_km²` km²; em NPL-POK o
   caixa fechava **todo mês em 0** (mín. 0, máx. 0 nos 12 meses de 365 d). Isso viola "sem
   explosão" e impedia o município de juntar caixa para uma obra maior.
5. **Cidade cheia nunca avaliava moradia social.** `decide_works` saía com `return` no ramo de
   solo; se a obra de solo era recusada (caixa curto) ou já estava em andamento, o déficit popular
   não era olhado. As duas frentes são independentes na spec W2 e em `city_gov.h:110`.

## 2. O que mudou

| Arquivo | Mudança |
| --- | --- |
| `urban_land.cpp` (`start_project`) | Gate por conta: com `payer` (prefeitura), `budget = payer->cash` e **sem** `committed_cost` — o caixa já é líquido de todas as obras do município. Dono "state"/"private" mantém `committed + cost` (régua §15 do orçamento público/firma; mudar ali altera a economia privada). |
| `city_gov.cpp` (`decide_works`) | Obra de solo e moradia social são frentes **independentes**; a cidade cheia não sai mais da função antes de avaliar o déficit popular. |
| `city_gov.cpp`/`.h` | `kWorksCashShare = 0,5`: cada frente consome no máximo metade do caixa disponível no fechamento (explícito, declarado; não vem de fonte externa). Expansão e lote social usam a mesma régua. |
| `city_gov_test.gd` | Caso novo de cidade cheia com caixa + obra estatal em voo (A/B do gate), conta de caixa, conclusão, capacidade e teto de gasto; a chamada explícita de moradia social passou ao dia 31 (a decisão do dia 30 agora também abre a frente de moradia). |

Sem campo de save novo: nada em `save_load.cpp`, versão do save continua **14** e o round-trip
segue 0 diffs.

### "Custo crescente" do spec — conferido, já aplicado

- Expansão: `base × (1 + 0,5·(raio/raio₀ − 1)) × (1 + 0,8·fração da reserva usada)`
  (`urban_land.cpp:79-102, 408-415`), e o **prazo** cresce com a distância
  (`project.capacity × distance_factor`, `urban_land.cpp:552-554`). A reserva consumida na sonda
  sobe de 4,90 para 4,67 km² em 365 d, então o relevo encarece de fato.
- Adensamento por nível: LOW→MID `0,35`, MID→HIGH `0,85` (`sim_constants.h:544-545`) e a receita
  MID→HIGH troca para mais aço/elétrica (`urban_land.cpp:141-174`, pesos 1,8/1,6).
- Terra: `urban_land::phase_month` roda no tick (`economy_sim.cpp:1205`) e move `land_price` por
  ocupação e renda — comprovado em `urban_land_test` (`preco RES AGRIA 376,6 → 657,3 XGS/m²`).

Nada disso precisou de mudança; a frente só confirmou e registra a evidência.

## 3. Medição 365 d (`--ai=1 --urban=1 --threads=1 --fx=real`)

Comando (idêntico antes/depois, só muda o sufixo e o binário):

```powershell
godot --headless --path <worktree> --script res://tools/economy_world_test.gd -- --ai=1 --days=365 `
  --urban=1 --threads=1 --fx=real --baseline=res://docs/economy/perf/construction_world_sweep.json `
  --out=user://world_sweep/q15b_real_365_<tag>.json `
  --urban-cities=NPL-POK,TZA-ARU,CUB-SAN,HKG-HON,BRA-SAO,IND-MUM,AUS-WEI `
  --urban-out=user://q15b_urban_365_<tag>.json
```

Cidades: as 4 de maior ocupação RES no dia 0 (NPL-POK 1,375; TZA-ARU 0,813; CUB-SAN 0,645;
HKG-HON 0,545) + as três pedidas (BRA-SAO, IND-MUM, AUS-WEI). O "antes" reproduz **byte a byte**
o run do `69e2790` (`docs/economy/perf/obras_urban_real_365.json`, sha256 `90D02741…`), o que
valida a referência.

| Cidade | Ocup. RES d0 → d365 | km² área | Obras de solo | km² ganhos | Caixa municipal fim | Desp. infra (último mês) |
| --- | --- | --- | --- | --- | --- | --- |
| NPL-POK — antes | 1,375 → 1,327 | 16,30 → 16,59 | abre todo mês | **+0,29** | **0** | 33.786 |
| **NPL-POK — depois** | 1,375 → 1,335 | 16,30 → 16,52 | abre todo mês | **+0,22** | **29.591** | 29.589 |
| TZA-ARU antes / depois | 0,813 → 0,809 / 0,807 | 15,60 | **0** | 0 | 54.096 / 110.399 | 0 |
| CUB-SAN antes / depois | 0,645 → 0,639 / 0,639 | 50,50 | **0** | 0 | 33.828 / 49.501 | 0 |
| HKG-HON antes / depois | 0,545 → 0,542 / 0,541 | 92,50 | **0** | 0 | 1.180.737 / 1.816.977 | 0 |
| BRA-SAO antes / depois | 0,086 → 0,085 / 0,085 | 1.724,30 | **0** | 0 | 858.707 / 1.659.375 | 0 |
| IND-MUM antes / depois | 0,240 → 0,239 / 0,239 | 531,40 | **0** | 0 | 2.511.147 / 1.319.765 | 0 |
| AUS-WEI antes / depois | 0,404 → 0,400 / 0,400 | 0,50 | **0** | 0 | 0 / 137 | 0 |

Leitura:

- **Cidade com folga não abre obra** (as 6 abaixo do gatilho, antes e depois) e o caixa acumula —
  objetivo 4 na metade "não abrir obra com folga".
- **Cidade cheia abre e conclui** obra de expansão nos dois lados; a diferença é a conta de caixa:
  antes o caixa fechava em **0** todo mês (gastava 100%), depois fica **positivo** (gasta ≤50% por
  frente). O km² anual cai de +0,29 para +0,22 justamente por isso.
- **Sonda mensal** (mesmo binário, sem a IA do runner, 365 d, só NPL-POK): **12/12 meses com obra
  de solo aberta**, área 16,3000 → **16,5326 km² (+0,2326)**, reserva 4,90 → 4,67, caixa mínimo
  **2.973** e final **36.641** (antes: 0 em todos os fechamentos); **4 lotes de moradia social**
  (dias 30, 120, 210, 300: 1.017 / 1.057 / 3.705 / 4.272 XGS). Nenhum mês com caixa negativo.

## 4. Portões do 365 d

| Portão | Antes | Depois | Limite | Veredito |
| --- | --- | --- | --- | --- |
| Falências de construção | 6 | **5** | ≤ 28 | PASSA |
| Drift de dinheiro | +0,0002 | **−0,0007** | 0,01 | PASSA |
| Comida (nações pioradas > 2,5 p.p.) | 2 (AFG −11,3; SOL −3,5) | **0** | — | PASSA |
| Serviço (falências / negócios) | 7.083 / 53.324 | 6.970 / 53.421 | — | — |

Comida: 179 nações comparadas, **0 pioraram**. AFG voltou a 1,000 (antes 0,887) e SOL subiu a
0,619 (antes 0,518; baseline 0,553). Piores do mundo seguem PSX 0,201 / SAH 0,228 / DJI 0,406.

**Determinismo:** o run `--threads=8` (365 d, mesmos args) gerou JSON **byte-idêntico** ao de
`--threads=1` (sha256 `0F7071725903B3A3…`); o retrato urbano difere só no campo `"threads"`.

## 5. Testes

| Teste | Resultado |
| --- | --- |
| `tools/city_gov_test.gd` | **verde** (exit 0) — inclui o caso novo de cidade cheia |
| `tools/urban_land_test.gd` | **verde** (exit 0) |
| `tools/construction_project_test.gd` | **verde** (exit 0) |
| `tools/economy_integration_test.gd` | **verde** (exit 0) |
| `tools/economy_save_load_roundtrip_test.gd` | **verde** (0 diffs) |
| `tools/construction_queue_test.gd` | **verde** (fila única intacta) |

A/B do gate: com o `committed_cost` de volta no ramo municipal, o caso novo **reprova**
("cidade cheia com caixa e obra estatal em voo: prefeitura nao abriu obra", exit 1); com a
correção, passa. Save v14 com obra em andamento: round-trip 0 diffs; threads 1/8 idênticos
(`city_gov_test` dia 32, `urban_land_test`, e o mundo de 365 d).

## 6. Pendências honestas

1. **Gatilho x dado:** só 1 das 834 cidades (NPL-POK) passa de 0,90 hoje. A regra da spec é
   "ocupação > 0,90"; quem recalibrar a capacidade de zona (frente paralela) vai mudar quantas
   cidades reagem. Nada aqui inventou ocupação para forçar obra.
2. **Parcela de expansão pequena em cidade sem dado municipal:** o piso declarado de
   `kDemoUrbanInfraFloorPerKm2 = 1e6` XGS/km² contra caixa mensal de milhares de XGS dá obras de
   0,003–0,03 km²/mês. É o modelo atual (não inventei número); fica registrado para a calibração.
3. **Mesmo duplo cômputo no caminho não municipal:** `committed_cost` continua somando ao custo no
   orçamento público e no caixa da firma em `start_project`, embora `demo_build`/`housing::start_project`
   já debitem o custo inteiro na abertura. Corrigir ali mexe na economia privada (§15/AI) — fora do
   escopo desta frente, fica como follow-up com sweep próprio.
4. **Moradia (B1/B2)** segue com `units_built_month = 0` — é outra frente; esta entrega só garante
   que a prefeitura não fica mais presa no gate nem no `return` da cidade cheia.
5. `housing::start_social_housing` continua limitado pelo caixa cheio quando chamado pela API; a
   divisão em lotes de 50% vale para a decisão da prefeitura (`decide_works`), que é o caminho de
   produção.

## 7. Reprodução

```powershell
$godot = "C:\Users\artur\OneDrive\Área de Trabalho\Godot_v4.7.2-stable_win64.exe"
$env:PATH = "G:/tools/llvm-mingw-20261104-ucrt-x86_64/bin;G:/tools/cpp-tools/Scripts;" + $env:PATH
& "G:/tools/cpp-tools/Lib/site-packages/cmake/data/bin/cmake.exe" -S gdextension -B gdextension/build-debug `
  -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ `
  -DGODOTCPP_DIR=G:/projetos/war-economy/godot-cpp
& "G:/tools/cpp-tools/Lib/site-packages/cmake/data/bin/cmake.exe" --build gdextension/build-debug
& $godot --headless --path . --script res://tools/city_gov_test.gd
& $godot --headless --path . --script res://tools/urban_land_test.gd
& $godot --headless --path . --script res://tools/construction_project_test.gd
& $godot --headless --path . --script res://tools/economy_integration_test.gd
```

Artefatos do sweep (não versionados): `user://world_sweep/q15b_real_365_before.json`,
`…_after.json`, `…_after_t8.json`, e `user://q15b_urban_365_{before,after,after_t8}.json`.
