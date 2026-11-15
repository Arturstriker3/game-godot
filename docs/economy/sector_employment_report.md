# F9 — emprego por setor e piso de subsistência agrícola

Data: 2026-11-15. Binário: `economy_gdextension.windows.template_debug` (debug,
12.648.448 B). Este relatório substitui `f8_services_employment_report.md` (a
medição da F8 fica no §5 como histórico).

```powershell
Godot_v4.7.2-stable_win64.exe --headless --path . --script res://tools/economy_world_test.gd -- `
  --ai=1 --days=60 --out=res://docs/economy/perf/f9_world_ai_60.json
```

## 1. O que o núcleo faz (`gdextension/src/economy_sim.{h,cpp}`)

`demo_update_sector_employment` roda no fim de `demo_labor_day` (o emprego do dia
já está conhecido) e publica, **por cidade**:

| índice | setor | origem |
|---|---|---|
| 0 | agropecuária | âncora ILO interpolada no log da TFP × fator de produção local `t/dia × 55,0 / TFP^1,4486`, limitado a [0,9; 1,1] da âncora |
| 1 | indústria/mineração | âncora ILO; piso adicional = valor adicionado do dia × 3,0 / TFP^1,4486 |
| 2 | serviços | **residual** (`empregados − agro − indústria`) |
| 3 | informal | **sobreposição** (fração dos três primeiros; não soma com eles) |

As âncoras vêm de `data/definitions/economy/sector_employment.json` (ILOSTAT,
share of total employment 2023; ver §2). Sem o arquivo (binário antigo ou teste
sem `demo_set_sector_employment`), vale o **espelho compilado** das mesmas cinco
âncoras. O `economy.gd` (autoload) carrega o JSON antes de criar o mundo.

**Piso de subsistência em espécie** (`demo_tick_core`, depois da compra de
alimento): se a cidade **não cobriu a cesta** e tem produção agrícola, as
famílias agrícolas comem da própria produção — limitado pela necessidade dessas
famílias e por `own_consumption_share` (0,5) da produção declarada. O alimento
sai do **estoque** (conservação: não é comprado, não há pagamento nem criação de
comida), entra em `pop.consumed` e é publicado em `in_kind_food`.

**Correção de contrato:** o espelho de escalares do status tinha
`kDemoStatusScalarCount = 30` com **31 escritas** desde a F7 (escrita fora do
limite). Agora são **33** e batem com as chamadas de `sync_scalar`
(`service_jobs_filled` e `in_kind_food` entraram). Save/load inalterado: os
campos novos são derivados, não entram no save (schema continua **v10**).

## 2. Âncoras (dados observados, não alvo)

| Nação | índice de renda | agricultura | indústria | informal |
|---|---|---|---|---|
| NGA | 0,371 | 35,0% | 12,0% | 92,0% |
| IND | 0,457 | 43,0% | 25,0% | 90,0% |
| BRA | 0,912 | 9,0% | 20,0% | 37,0% |
| DEU | 2,935 | 1,3% | 27,0% | 8,0% |
| USA | 3,503 | 1,5% | 19,0% | 6,0% |

As 174 nações restantes são **estimativa por interpolação linear no log do
índice de renda** (o mesmo índice da TFP da F7); o arquivo declara
`"estimated": true`. Serviços não é âncora observada — é o resíduo
`1 − agricultura − indústria` da mesma fonte.

## 3. Resultado — 60 d com IA (dia 60, 179 nações)

| Nação | empregados | agro | ref. | indústria | ref. | serviços | ref. | informal | ref. |
|---|---|---|---|---|---|---|---|---|---|
| USA | 19.782.715 | 1,4% | 1,5% | 19,0% | 19,0% | 79,6% | 79,5% | 6,0% | 6,0% |
| DEU | 7.532.663 | 1,4% | 1,3% | 27,0% | 27,0% | 71,6% | 71,7% | 8,0% | 8,0% |
| BRA | 19.420.751 | 8,4% | 9,0% | 20,0% | 20,0% | 71,6% | 71,0% | 37,0% | 37,0% |
| IND | 31.159.258 | 42,2% | 43,0% | 25,0% | 25,0% | 32,8% | 32,0% | 90,0% | 90,0% |
| NGA | 14.969.406 | 35,2% | 35,0% | 12,0% | 12,0% | 52,8% | 53,0% | 92,0% | 92,0% |

Mundo (179 nações): agro **16,1%**, indústria **20,8%**, serviços **63,1%**,
informal **46,2%** (sobreposição). Autoconsumo no dia 60: **0 t-eq/dia** — o
mundo fecha o dia 60 com comida 1,000 nas cinco nações de referência, então o
piso não tem déficit para cobrir. Dinheiro conservado (drift 0,0000), 0
falências de construção, 0 violações de invariante.

Diferença máxima vs. referência: **0,8 p.p.** (IND agro 42,2 vs 43,0 e IND
serviços 32,8 vs 32,0; BRA agro 8,4 vs 9,0). A estrutura agora é comparável à
realidade, diferente da F8 (§5).

**Nota de honestidade sobre a comida:** EGY 0,508 (pior nação no dia 60) e DJI
0,965 já eram exatamente esses na F8 (`f8_world_ai_60.json`); o JSON de 60 d da
F9 é **idêntico** campo a campo em comida e dinheiro. Ou seja: em 60 d a F9 não
muda a trajetória — o piso de subsistência só age quando há déficit, e em 60 d
não há.

## 4. Sweep de 365 d — critério **não** fechado (residual aberto)

`tools/economy_world_sweep.gd` (365 d, 5 variações: `real`, `real+seed1`,
`real+seed2`, `half`, `xgs`) contra `docs/economy/perf/f4_world_ai_365.json`.
Relatório: `docs/economy/perf/f9_world_sweep.json`.

| métrica | resultado |
|---|---|
| falências de construção (limite 28) | **11** em todas as 5 variações — passa |
| nações piores em comida além de 2,5 p.p. | **3** — MRT, UZB, DJI — falha |

| nação | f4 (baseline) | F7 opt. | F8 | **F9** | F9 vs f4 |
|---|---|---|---|---|---|
| MRT | 1,000 | 1,000 | 0,954 | **0,951** | −4,9 p.p. |
| UZB | 0,948 | 1,000 | 0,896 | **0,905** | −4,3 p.p. |
| DJI | 0,391 | 0,379 | 0,413 | **0,343** | −4,7 p.p. |
| SOL | 0,609 | 0,616 | 0,600 | **0,593** | −1,6 p.p. (dentro) |

O residual **não é novo**: MRT e UZB já estavam fora da tolerância na F8
(§15 do `GAME_STATE`, aberto) e o piso em espécie **melhora UZB** (0,896 →
0,905) e deixa MRT praticamente igual (0,954 → 0,951). A **regressão nova é
DJI** (0,413 → 0,343), que na F8 estava *acima* do baseline.

**A/B medido (365 d, `own_consumption_share` = 0,0 × 0,5):** desligar o piso no
dado devolve **exatamente** os números da F8 — MRT 0,954, UZB 0,896, DJI 0,413,
SOL 0,600 e 11 falências (`docs/economy/perf/f9_world_inkind0_365.json`). Isso
prova que o piso é a **única** mudança funcional da F9 (o espelho de escalares
não altera valor nenhum — §3) e mede o trade-off: **UZB +0,9 p.p., MRT −0,3 p.p.
e DJI −7,0 p.p.** O piso age só em cidade com déficit e retira estoque do mercado
**depois** da compra local — consome o excedente que iria para a exportação. O
modelo não tem solidariedade nacional nem tesouro que financie importação
sustentável (resíduo "zero-sum" registrado no §15 desde a F3): quem importa
comida paga o custo. DJI é importador líquido sem produção agrícola própria, por
isso cai; UZB, que tem produção, melhora.

**Conclusão:** nenhum valor do knob fecha o critério — com 0,0 restam MRT e UZB
(residual da F8, estrutural); com 0,5 eles continuam e DJI entra. O fechamento
exige o item de modelo (renda/tesouro de importação, §15·F3), não calibração. O
dado ficou em **0,5** (calibração da F9 preservada) e o A/B fica registrado para
a decisão.

## 5. Histórico — F8 (serviços como empregador residual)

A F8 media `service_workers` (vagas **pagas** preenchidas) / `pop.employed` e
encontrava **99,4%** em todas as nações no dia 60 (USA 99,4%, DEU 99,4%, BRA
99,4%, IND 99,4%, NGA 99,4%) — porque o modelo não tinha emprego agrícola nem
informal: todo trabalhador fora de construção caía em serviços. Aquele número
era evidência do **gap**, não aprovação. A F9 fechou o gap (§3).

Contrato de status que mudou de significado (a UI de Serviços precisa saber):

| Campo | Antes (F8) | Agora (F9) |
|---|---|---|
| `service_workers` | vagas de serviço **preenchidas** | **emprego do setor** de serviços (fatia estrutural) |
| `service_jobs_filled` | — (novo) | vagas de serviço efetivamente **pagas** (salário e folha saem daqui) |
| `sector_workers` | — (novo) | `PackedFloat64Array[4]`: agro, indústria, serviços, informal |
| `sector_share` | — (novo) | `PackedFloat64Array[4]`: fração do emprego (0..1) |
| `in_kind_food` | — (novo) | autoconsumo em espécie do dia (t-eq) |

Os demais campos de serviço (`service_jobs`, `service_wage`, `service_revenue`,
`service_profit`, `service_cash`, `services`, `services_detail`,
`service_businesses`, `service_bankruptcies`) não mudaram.
