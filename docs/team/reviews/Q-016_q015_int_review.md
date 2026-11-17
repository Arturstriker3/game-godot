# Q-016 — revisão final da integração do veto Q-015

- **Objeto:** `fix/q015-int` @ **`006d2ce`** (integra zonas Q-015-C + moradia Q-015-A +
  prefeitura Q-015-B sobre `a8d7984`), worktree `G:/projetos/opengs-wt-q15int`.
  Relatórios lidos: `Q-015_integracao.md`, `Q-015_s19_review.md`, `Q-015_fix_zonas.md`,
  `Q-015_fix_moradia.md`, `Q-015_fix_prefeitura.md`.
- **Data:** 2026-11-17. **Revisor:** `gerente-qualidade`. **Modelos:** só DeepSeek Flash
  (`executor-flash` + `verificador-flash`).
- **Ambiente:** Godot 4.7.2 stable, `--headless`, DLL **debug** real no slot
  (`gdextension/bin/economy_gdextension.windows.template_debug.x86_64.dll`,
  **23.294.976 B**, SHA256 `6720655F24EA0E58…`); 8 núcleos / 16 threads; `user://`
  compartilhado entre worktrees (`docs/economy_ai/decisions.json` e
  `docs/economy_long_run/run.csv` chegam modificados ao rodar testes — **não** entram no commit).
  Nenhum editor/MCP tocado.
- **Veredito:** **APROVADO COM RESSALVA.** As três frentes coexistem, o dinheiro conserva, o
  save round-trip fecha **0 diffs** e o sweep ×5 aprova. **Mas o portão ×5, como estava, não
  media câmbio** (R1) — corrigido nesta revisão, só em `tools/`. B3 continua não disparando no
  mundo real por falta de demografia (R2), e duas constantes de calibração sustentam o gate do
  incorporador (R3).

## 1. Achado principal — o portão "×5" era tautológico para o câmbio

O relatório de integração (§6) apresentou os 5 relatórios do sweep como **byte-idênticos
(`sha256 78c91417…`, 303.410 B)** e leu isso como "determinismo, como o desenho F5 exige".
Medido nesta revisão:

| Fato | Evidência |
| --- | --- |
| `data/definitions/economy/currencies.json` tem `regime: null` (topo) e **136/136** moedas sem `regime`; a própria nota do dado diz *"Sem fonte para regime cambial (peg/float); declarar na F5"* | leitura do JSON |
| O núcleo só trata moeda como flutuante com `regime == "float"` (`economy_sim.cpp:548`) e `demo_competitiveness` devolve **1.0** com `peg` (`fx.cpp:125-127`); com peg a taxa só entra em `price_local` (exibição) | `fx.cpp:108-130` |
| Logo `--fx=real`, `--fx=half` (0,5), `--seed=1/2` (±1e-9) e `--currencies=0` **não têm por onde** mudar trajetória, e o relatório não publica `price_local` | estrutura do relatório: `meta`, `nations`, `labor`, `sectors`, `ownership` |
| Reproduzido: 60 d `real` × `half` × `xgs` → **byte-idênticos** (`sha256 568CD4249D79200B`, 300.268 B), igual ao hash que o relatório citava | 3 processos, 60 d, `--urban=1` |
| O baseline histórico `construction_world_sweep.json` também tem as 5 variantes idênticas (falências 6/6/6/6/6, serviços 6.910/52.808 iguais) | `meta` do baseline |

**Conclusão:** a igualdade não é evidência de robustez cambial — é consequência de **todas as
moedas do dado rodarem `peg`**. O caminho que contém o feedback real (`taxa ÷ taxa-base` →
competitividade das ofertas → comércio) nunca foi exercitado por nenhuma variante. O portão
aprovava sem medir o mecanismo.

**Correção (só ferramenta, nada de núcleo nem de dado):** sonda `--fx=float` no runner (força
`regime: float` no topo e em cada moeda; **taxas do dado, sem inventar número**), `meta.fx` +
`meta.trajectory_hash` no relatório, 6ª variante `float_probe` no
`tools/s19_world_sweep_parallel.ps1` e gate de sensibilidade no `tools/aggregate_s19_sweep.py`
(as 5 de aceite têm de fechar o **mesmo** `trajectory_hash`; a sonda tem de **diferir**).
Ver §2: a sonda **diferencia** e derruba EGY em 23,2 p.p. — o caminho existe e não estava coberto.

## 2. Sweep 365 d ×5 + sonda (`-OutPrefix q16_`, `--urban=1`, `--threads=1`)

6 processos, **3,0 min** de parede. Agregação: `tools/aggregate_s19_sweep.py` (exit **0**),
baseline novo `docs/economy/perf/q16_world_sweep.json`.

| Variante | Falências | Drift de dinheiro | Nações > 2,5 p.p. | Pior delta | `trajectory_hash` |
| --- | ---: | ---: | ---: | ---: | --- |
| `real` | 5 | −0,0000 | 0 / 179 | DJI −0,60 p.p. | `67c7a6bb…` |
| `real_seed1` | 5 | −0,0000 | 0 / 179 | DJI −0,60 p.p. | `67c7a6bb…` |
| `real_seed2` | 5 | −0,0000 | 0 / 179 | DJI −0,60 p.p. | `67c7a6bb…` |
| `half` | 5 | −0,0000 | 0 / 179 | DJI −0,60 p.p. | `67c7a6bb…` |
| `xgs` | 5 | −0,0000 | 0 / 179 | DJI −0,60 p.p. | `67c7a6bb…` |
| **`float_probe`** (sonda, fora do gate) | **9** | −0,0003 | **1** (EGY −23,21 p.p.) | EGY 0,768 | **`72963e08…`** |

- **Aceite:** falências 5 ≤ 28 ✔ · drift 0,0000 ≪ 0,01 ✔ · comida 0/179 acima da tolerância ✔.
  Números **idênticos** aos do relatório de integração (DJI −0,60 p.p.; 5 falências).
- **Comparação justa:** `real` 365 d × relatório anterior → **0 nações diferentes**; os campos
  novos (`meta.fx`, `meta.trajectory_hash`) não mudam a simulação — `real`, `half` e `xgs` têm
  payload (sem `meta`) **exatamente igual** (`real == half == xgs` → `True`), e o relatório
  agora **difere por arquivo** apenas no `meta` (303.656 / 303.651 / 303.631 B). O critério de
  comparação passou a ser `trajectory_hash`, não o byte do arquivo.
- **Sonda:** hash diferente em 365 d, **EGY 0,768** (delta −23,21 p.p.), falências 9. Ou seja:
  **se algum dia o dado declarar câmbio flutuante, o mundo atual reprova comida** — é decisão
  de dado/dono, não defeito desta integração; mas agora está medido em vez de suposto.

## 3. Diff `006d2ce` × `a8d7984` (só `gdextension/src` e `tools`)

15 arquivos, +763/−82. Revisão linha a linha do que muda comportamento:

| Arquivo | Mudança | Veredito |
| --- | --- | --- |
| `sim/urban_land.cpp` | `zone_capacity` multiplica por `zone_fill`; `zone_capacity_for` põe piso `uso ÷ 0,95` na zona RES; `zone_usage` desconta `informal_units`; gate de caixa por conta em `start_project` | Correto e coerente com o diagnóstico do B3 (dupla contagem `committed_cost("state")` × caixa municipal). `informal` fora da zona formal fecha B4. |
| `sim/sim_constants.h` | `kDemoUrbanBandOccupancy`, `kDemoUrbanOccupancyCap`, `kDemoHousingBurdenFloor`, `kDemoHousingBuildCostYears 14→6`, `kDemoHousingLandShareMax`, `kDemoHousingSlumSevereShortage`, `kDemoHousingMaterialShare → kDemoConstructionMaterialShare` | Todos **declarados como calibração**; ver R3. Material alinhado ao §15 corrige uma inconsistência real (2,7×). |
| `sim/housing.cpp/.h` | custo = aluguel×12×6 + lote (m²÷FAR) com teto de 45% do valor; aluguel cobrado da **renda** (reserva em `phase_income` + pagamento em `phase_housing`); piso de ônus 0,15 reancorado na renda corrente; favela exige falta **popular própria**; lote limitado pelo caixa da firma | Fecha B1/B2/B5 sem criar dinheiro (`drift 0,0000` em 365 d). |
| `sim/consumption.cpp` | `needs_budget -= housing::daily_rent_obligation(...)` depois da comida | Ordem correta (comida → aluguel → life/luxury). |
| `sim/city_gov.cpp/.h` | frentes de solo e moradia **independentes**; `kWorksCashShare = 0,5` por frente; lote social limitado por caixa | Corrige o "return" que impedia avaliar moradia quando a obra de solo não abria. |
| `sim/construction.cpp` | crédito de terra da moradia = mesma fórmula do custo (lote÷FAR, teto 45% do orçamento) | Necessário para `reserved > 0`; conta bate com `build_cost_unit`. |
| `edge/save_load.cpp` | `zone_fill` salvo/carregado; ausência → 1,0 | **Migração OK** (§5). |
| `economy_sim.cpp` | `zone_fill` calculado no loader pelo alvo da faixa; `demo_get_urban_city` publica capacidade **efetiva** | Coerente: `uso/capacidade ≤ 1` por construção. |
| `tools/city_gov_test.gd` | fixture de cidade cheia via round-trip de save (`_force_busy_zone`, 0,95) + teste B3 completo | **Conflito semântico 1 resolvido corretamente**: o fixture passou a representar o estado que o jogo atinge, não a comprimir área (que o `zone_fill` recalcula). Guarda explícita se a ocupação não chegar a 0,90. |
| `tools/urban_land_test.gd` | AGRIA declara `urban_population` 6 M (alvo 0,85) e a asserção muda de 0,9 → 0,85 | Reflete a calibração declarada; não esconde falha (a capacidade continua sendo checada). |
| `tools/urban_zone_audit.gd` | auditoria dia 0 das 834 cidades | Ferramenta nova, sem efeito no jogo. |

**Conflito semântico 2 (terra = m²÷FAR):** correto em si (o lote não tem a área construída; torre
FAR 6 entrava 6× acima). O **teto de 45%** é o ponto frágil — ver R3.

**Mudança de save sem migração:** nenhuma. `zone_fill` é aditivo com default 1,0; demais campos
(`rent_cap_ratio`, `stress_days`, `rent_burden`, `informal_units`) já existiam e continuam
serializados.

## 4. Números sem fonte (auditoria)

Nenhuma cidade, nação ou taxa foi inventada. Todas as constantes novas são **calibração
declarada** (comentário no próprio header), o que a regra do projeto admite — mas três delas
**sustentam o gate** e devem ficar explícitas para o dono:

| Constante | Valor | Classificação |
| --- | --- | --- |
| `kDemoHousingBuildCostYears` | 14 → **6** | Derivada **do gate** (B ≤ 6,66 em Tóquio para `price ≥ 1,2×custo`), não de série de custo. Declarada. |
| `kDemoHousingLandShareMax` | **0,45** | Faixa "20-50% em metrópoles" **sem citação no repo**; o valor fecha exatamente a margem (obra 30% + terra 45% = 75% ≤ 83,3%). Declarada, mas é o teto que faz o número passar. |
| `kWorksCashShare` | **0,5** | Declarada "não vem de fonte externa" (regra do "sem explosão"). |
| `kDemoUrbanBandOccupancy` | 0,55/0,70/0,72/0,85 | Calibração declarada; hierarquia com fontes citadas para o estoque (UN DESA, Atlas 2016, WB SDG 11.1.1). |
| `kDemoUrbanOccupancyCap` | 0,95 | Regra declarada (piso de capacidade), resolve NPL-POK sem inventar área. |
| `kDemoHousingBurdenFloor` | 0,15 | Piso da faixa 0,15-0,40 (a faixa não tem citação no repo). |
| `kDemoHousingSlumSevereShortage` | 0,20 | Calibração declarada. |

## 5. Save / migração

`zone_fill` entrou em `to_dict`/`from_dict` com default **1,0** quando ausente (save antigo ou
cenário sintético) — comportamento anterior preservado, sem recalibrar mundo em andamento.
Efeito colateral declarado: um save **anterior** ao Q-015, carregado no binário novo, continua com
a capacidade "de envelope" (ocupação pode passar de 1), enquanto um mundo novo nasce calibrado.
Não é corrupção: é ausência explícita. Round-trip de save (§7) fecha com o campo presente.

## 6. Ocupação não sobe e a prefeitura não abre obra — **não há demografia no núcleo**

Confirmado no código, não só na medição:

- `simulation_population` só é **escrito** nos loaders (`economy_sim.cpp:756`,
  `save_load.cpp:1201`, `scenario_fixture.cpp:40,61`). Não existe `phase_population` — as fases
  são `month, arrival, reset, fleet, labor, production, raw, income, housing, services, trade,
  accounting, prices, clamp, fiscal, fx, invariants`.
- `labor.cpp:670-671` faz `pop.population = city.simulation_population` **todo dia**, com o mesmo
  valor. Logo demanda, domicílios e "cidade cheia" são constantes.

Consequência medida (números do relatório de integração §5.1-5.2, **não** re-medidos nesta
revisão porque o runner não exporta ocupação): ocupação RES **cai** 0,849 → 0,842 em 365 d (SP),
da prefeitura é **0,90** e a calibração nasce em **0,85-0,849**, chegar lá exigiria +5,9% de
estoque formal — que sem crescimento populacional não acontece; o estoque líquido **cai**.
Portanto **não é latência que fecha sozinha**: é ausência de demografia. O gate do B3 está
corrigido e provado no `city_gov_test` (0,95 → expansão de 0,4585 km² / 1.642.140 XGS), mas no
mundo real ele só passa a disparar quando houver crescimento de população (ou decisão de
calibração do dono: baixar o gatilho, acelerar a oferta, ou aceitar que expansão é evento raro).

## 7. EGY em 60/120 d e round-trip de save

- **EGY (nação):** 1,000 → **0,857 em 60 d** → **0,546 em 120 d** → **1,000 em 365 d**. Pior
  cidade EGY-CAI: 0,744 (60 d) e **0,237** (120 d); no dia 60 o estoque de trigo e o caixa das
  firmas de Cairo estão **zerados** e o consumo fecha em 74% da necessidade. **Não é regressão do
  Q-015:** sem §19 (`--urban=0`) o mesmo dia 60 dá **0,694**, pior ainda. Causa provável:
  transiente de importação de trigo (Cairo com população de cenário grande e uma única usina de
  2.882 t/d; cidade dependente de importação até o comércio/frota amadurecer) — não instrumentei a
  cadeia além disso. O critério de aceite do sweep é 365 d, onde fecha em 1,000. A nota antiga do
  runner ("resíduo de 60 d é EGY 1,000→0,979") **subestimava o vale** e foi corrigida (§10).
- **Round-trip de save:** `economy_save_load_roundtrip_test` → `save=936ed973…`,
  `status=904dab4c…` **iguais** entre 60 d direto e 30+30 d com save/load: **0 diffs**. O teste
  carrega `urban_land.json` real, então `zone_fill` está no caminho.

## 8. Testes

| Teste | Resultado |
| --- | --- |
| `city_gov_test` | verde (fixture 0,95 + B3) |
| `urban_land_test` | verde |
| `housing_market_test` | verde |
| `construction_queue_test` | verde |
| `construction_project_test` | verde |
| `economy_integration_test` | verde |
| `economy_save_load_roundtrip_test` | **0 diffs** |
| `economy_world_test` 60/120 d | falha **conhecida** (EGY, §7) — 365 d passa |
| sweep 365 d ×5 + sonda | exit 0 (§2) |

Não re-rodei `economy_long_run_test` (365 d) nem `perf_day_peak` (números do relatório de
integração: 41,63 ms de mediana, memória 299,8 MB) — fora do critério desta revisão.

## 9. Ressalvas (o que fica para o dono)

- **R1 — Câmbio:** o portão ×5 não cobria o caminho float; agora a sonda cobre e mostra que o
  mundo **reprovaria comida** (EGY −23,2 p.p.) se o dado declarasse flutuação. Antes de ligar
  `regime: float` em `currencies.json`, rodar o sweep com a sonda e tratar EGY.
- **R2 — B3:** gate corrigido, mas depende de demografia para disparar no mundo real. Decisão de
  calibração (oferta, gatilho ou aceitar raridade), não de código.
- **R3 — Teto de 45% e B=6:** calibração declarada sem fonte no repo, escolhida para a margem do
  incorporador ser alcançável. Acima do teto o preço da terra **deixa de pesar** no custo — o
  sinal ocupação→preço fica parcialmente decorativo. Registrar como limitação conhecida.
- **R4 — Matéria de dado:** população de cenário de Cairo (86,1 M) e uma usina de trigo de
  2.882 t/d explicam o vale de EGY; é `@gerente-dados`, não esta integração.

## 10. Correções desta revisão (só `tools/`)

1. `tools/economy_world_test.gd`: `--fx=float` (sonda), `meta.fx`, `meta.trajectory_hash`;
   comentário do transiente de EGY corrigido com os números medidos (60/120 d) e aviso de que a
   igualdade das taxas `peg` é estrutural.
2. `tools/s19_world_sweep_parallel.ps1`: 6ª variante `float_probe`.
3. `tools/aggregate_s19_sweep.py`: gate de sensibilidade + bloco `meta.probe` (falências/drift/pior
   comida da sonda).

Nenhum arquivo de núcleo (C++), dado ou regra de negócio foi alterado.

## 11. Reprodução

```powershell
$godot = "C:\Users\artur\OneDrive\Área de Trabalho\Godot_v4.7.2-stable_win64.exe"
$base  = "res://docs/economy/perf/construction_world_sweep.json"

# 60 d real × half × xgs (byte-idênticos) e 120 d
foreach ($fx in "real","half") {
  & $godot --headless --path . --script res://tools/economy_world_test.gd -- --ai=1 --days=60 `
    --urban=1 --threads=1 --fx=$fx --baseline=$base --out=user://world_sweep/q16_60_$fx.json
}
& $godot --headless --path . --script res://tools/economy_world_test.gd -- --ai=1 --days=60 `
  --urban=1 --threads=1 --currencies=0 --baseline=$base --out=user://world_sweep/q16_60_xgs.json
& $godot --headless --path . --script res://tools/economy_world_test.gd -- --ai=1 --days=120 `
  --urban=1 --threads=1 --fx=real --baseline=$base --out=user://world_sweep/q16_120_real.json

# sweep ×5 + sonda (3,0 min) e agregação
powershell -NoProfile -ExecutionPolicy Bypass -File tools/s19_world_sweep_parallel.ps1 -OutPrefix q16_
G:/tools/cpp-tools/Scripts/python.exe tools/aggregate_s19_sweep.py `
  --baseline docs/economy/perf/construction_world_sweep.json `
  --dir "$env:APPDATA/Godot/app_userdata/Open Grand Strategy/world_sweep" `
  --prefix q16_ --suffix _365.json --logdir "$env:TEMP/opengs_q015_sweep" `
  --out docs/economy/perf/q16_world_sweep.json

# save round-trip
& $godot --headless --path . --script res://tools/economy_save_load_roundtrip_test.gd
```
