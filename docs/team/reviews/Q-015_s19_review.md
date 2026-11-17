# Q-015 — §19 integrado (solo urbano, prefeitura e moradia): parecer de qualidade

- **Objeto:** `4c1eb9d` (integra W0–W3 em `balance/global-resources`) + `82fdd9c` (goldens S0).
  Spec: `docs/economy/s19_urban_spec.md`. Relatório do dono: `docs/economy/s19_integration_report.md`.
- **Data:** 2026-11-17. **Revisor:** gerente-qualidade. **Verificador independente:** `verificador-flash`
  (APROVADO nos 7 itens, com 2 ressalvas de redação — incorporadas aqui).
- **Veredito:** **VETADO** para promoção/merge. O núcleo técnico passa (dinheiro, determinismo,
  save, perf), mas o §19 **não funciona no mundo real**: em 365 dias **nenhuma unidade de moradia é
  construída**, **nenhuma obra da prefeitura começa** e o **ônus de aluguel medido (0,016–0,052)
  está fora da faixa real**. Além disso, a evidência de sweep do S-019 **não exercitou o §19**.

Ambiente: Godot 4.7.2 stable, `--headless`, DLL debug final `3B531795…` (23.262.720 B, build 03:03:40)
e release `6EA3F400…` (1.736.192 B, 03:03:51); editor Godot **aberto** durante toda a revisão
(PID 11460) — diferente do Q-012, e é o confundidor do item 3. Máquina: 8 núcleos físicos / 16 lógicos.

## 1. Achado que precede tudo: o sweep 365 d do S-019 não carregava o §19

`tools/economy_world_test.gd` **nunca chamava `demo_set_urban`**. O núcleo zera
`ws_.land/housing/city_gov` em `initialize_demo_world` (`economy_sim.cpp:712-716`) e só os preenche
por `demo_set_urban` — chamado pelo autoload (`core/economy/economy.gd:114-123`) e pelos testes
urbanos, **não** pelo runner de mundo. `tools/economy_world_sweep.gd` usa esse mesmo runner.

Consequência: `docs/economy/perf/s19_int_world_ai_365.json` e o "0/179 nações pioram comida" do
`docs/economy/s19_integration_report.md` descrevem um mundo **sem** solo urbano, prefeitura e
moradia. A prova A/B está no item 2: **sem** `--urban=1` o mundo reproduz exatamente as **8**
falências de construção do S-019.

O runner foi corrigido nesta revisão (só ferramenta, aditivo) com `--urban=1`, `--threads=N`,
`--urban-cities=` e `--urban-out=`; o `--urban` é **opt-in** para preservar as baselines históricas.

## 2. Sweep 365 d ×5 em paralelo (mesma seed/variantes do sweep, `--threads=1` por processo)

Execução: `tools/s19_world_sweep_parallel.ps1` (5 processos simultâneos, 3 min de parede; logs em
`%TEMP%\opengs_q015_sweep\`). Agregação: `tools/aggregate_s19_sweep.py`. Novo baseline:
**`docs/economy/perf/s19_world_sweep.json`**. Baseline de comida: `construction_world_sweep.json`.

| Variante | Falências construção | Drift de dinheiro | Serviço (falências/negócios) |
| --- | --- | --- | --- |
| `real` | **5** | −0,0014 | 7.242 / 53.540 |
| `real_seed1` | **5** | −0,0014 | 7.242 / 53.540 |
| `real_seed2` | **5** | −0,0014 | 7.242 / 53.540 |
| `half` | **5** | −0,0014 | 7.242 / 53.540 |
| `xgs` | **5** | −0,0014 | 7.242 / 53.540 |
| `real_nourban` (A/B **sem** §19) | **8** | −0,0013 | 7.152 / 52.470 |

- **Falências ≤28: APROVADO** — 5 em todas as variantes (baseline 6; o §19 inclusive **reduz**
  para 5). Dinheiro conservado: |drift| = 0,0014 XGS (tolerância 0,01).
- **Determinismo:** os 5 relatórios são **byte-idênticos** (sha256 do arquivo
  `013197f4…`, hash de `nations` `9dbaf4d1…`). É o desenho F5 (preço na âncora) — as variantes de
  câmbio/semente **não podem** mudar trajetória; o resultado confirma o invariante. Limitação
  registrada: o relatório por variante não grava os args usados (a fonte é o script acima).
- **Comida por nação (179/179 comparadas):** 1 nação acima de 2,5 p.p. — **SOL 0,5528 → 0,5221
  (−3,07 p.p.)**, na classe de resíduo aceita pelo dono (JOR/YEM/SOL/DJI/SAH). Abaixo da tolerância,
  os desvios materiais: JOR −1,38 / LBY −1,03 / DJI −0,83 / MNG −0,76 / SAH −0,56 / MRT −0,36 p.p.
  Há ainda 16 nações com delta entre −0,025 e 0 apenas por poeira de float (1,0 → 0,99999999999999x),
  não são regressão.
- **Efeito do §19 no serviço:** +90 falências (7.152 → 7.242) e +1.070 negócios (52.470 → 53.540)
  vs o mundo sem §19 — pequeno, não bloqueia.

## 3. Perf release (DLL release no slot debug, 3× `--days=30 --tick-mode=worker --threads=8`)

| Run | tick p95 "em regime (dia > 2)" |
| --- | --- |
| 1 | 57,49 ms |
| 2 | 45,06 ms |
| 3 | 42,48 ms |
| **Mediana** | **45,06 ms** |

Meta **≤50 ms: aprovada na mediana** (45,06 ms; baseline Q-012 43,05 ms → **+2,0 ms, +4,7%**).
Ressalva: 1 de 3 runs deu 57,49 ms (dispersão já vista no Q-011) e o editor estava aberto — não é
possível separar §19 de contenção; memória estática 301 MB (834 cidades). DLL debug restaurada ao
fim (**SHA256 `3B531795…` idêntico ao de antes da troca**; backup `q015_debug_backup.dll` removido).

## 4. Plausibilidade urbana em 365 d (probe `docs/economy/perf/s19_urban_365.json`)

Amostra: São Paulo, Tóquio, Lagos, Mumbai, Nova York e Weipa (cidade pequena).

| Cidade | km² ano | Ocupação RES/COM/IND | Rent burden | Aluguel pago / faturado | Caixa (XGS) | IPTU / receita-alvo | Informal (Δ) | Obras |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| BRA-SAO | 1724,30 → 1724,30 (+0,00) | 0,208 / 0,012 / 0,000 | **0,034** | **0,200** | 570.049 | 0,072 | 2.502.403 (0) | 0 |
| JPN-TOK | 4489,30 → 4489,30 (+0,00) | 0,106 / 0,005 / 0,000 | **0,039** | **0,200** | 638.712 | 0,108 | 420.999 (0) | 0 |
| NGA-LAG | 527,90 → 527,90 (+0,00) | 0,744 / 0,087 / 0,000 | **0,046** | **0,200** | 501.908 | 0,149 | 9.661.906 (0) | 0 |
| IND-MUM | 531,40 → 531,40 (+0,00) | **1,963** / 0,252 / 0,000 | **0,024** | **0,200** | 2.244.543 | 0,108 | 3.103.193 (**+246.316**) | 0 |
| USA-NEW | 7478,50 → 7478,50 (+0,00) | 0,074 / 0,001 / 0,000 | **0,052** | **0,200** | 142.977 | 0,182 | 22.032 (0) | 0 |
| AUS-WEI | 0,60 → 0,60 (+0,00) | 0,453 / 0,000 / 0,000 | **0,016** | **0,200** | 0 | 0,101 | 1 (0) | 0 |

Números absolutamente plausíveis: **IPTU/receita municipal 7–18%** (por construção
`iptu_rate × 0,30 × PIBmês`, `city_gov.h:45`) contra a fonte `municipal_finance.json` — imposto sobre
imóveis ≈ 0,58% do PIB no BRA, 3,13% nos USA, 1,70% no JPN e 0,45% na IND, alinhado à faixa real;
**caixa municipal ≥ 0** nas 6 cidades; **nenhuma dívida**; vacância 0,000 (artefato: oferta ≡ demanda).
Abaixo, o que **não** é plausível.

### B1 (bloqueante) — Nenhuma unidade de moradia é construída em 365 dias

`units_built_month = 0` e `projects = []` nas 6 cidades; o estoque **só deprecia** (São Paulo
16.793.462 → 16.664.403, **−0,77%**/ano). A condição da incorporadora
(`housing.cpp:498-505`) é `price ≥ 1,2 × custo_unit`, com
`custo_unit = aluguel×12×14 + m²×preço_da_terra` e `price = aluguel×12/0,05`:
- São Paulo (POPULAR): custo 46,02 XGS vs preço 44,75 (precisaria de 55,2) — reprovado por 19%;
- Tóquio (POPULAR): custo 42,17 vs preço 35,99 — reprovado por 40%.

Como a terra entra com ~0,33 do preço e a obra com ~0,70, o custo nasce **≈ 1,03× o preço** — a
margem de 20% é **matematicamente inalcançável** com `cap_rate = 0,05` e `kDemoHousingBuildCostYears =
14`. A autoconstrução exige `custo×4 ≤ folga_de_poupança` (`housing.cpp:525`) e também não abre.
Efeito: a oferta do W3 não responde a nada.

### B2 (bloqueante) — Só ~20% do aluguel faturado é cobrado; `rent_burden` 10× abaixo do desenhado

A razão `rent_paid_month ÷ Σ(unidades×fração_alugada×aluguel)` é **0,2000 nas seis cidades** (5
dígitos idênticos — é um teto estrutural, não mercado). O aluguel é debitado da **poupança acima da
reserva de comida** (`housing.cpp:269-270,355-357`), não da renda: com a poupança ~ igual à reserva,
80% da fatura nunca é paga. Resultado: `rent_burden` medido **0,016–0,052** contra o próprio desenho
(0,28, `kDemoHousingInitialBurden`) e a faixa real 0,15–0,40. Dinheiro é conservado (o que não é pago
simplesmente não muda de mão), mas a renda de aluguel dos donos (família/empresa/prefeitura) é ~1/5
do fluxo projetado — e o sinal `housing_stress` sai subestimado (0,019–0,052).

### B3 (bloqueante) — A prefeitura não abre nenhuma obra em 365 dias

`gov.projects = []` e `month_expense_infrastructure = 0` nas 6 cidades. O caso decisivo é **Mumbai**:
ocupação RES **1,963** (> gatilho 0,90), caixa 2.244.543 XGS, custo 1.578.626 XGS/km² e reserva
175,4 km² — a decisão de expandir **deveria** abrir ~1 km². Não abre. Não há densificação possível
(gabarito já `high`). Suspeita com linha e arquivo: `start_project` compara
`committed_cost(owner="state") + custo` contra o **caixa municipal** (`urban_land.cpp:32-48,515-520`),
somando obras §15 estatais que não são pagas por esse caixa. Não instrumentei o contador — a
confirmação é do dono, mas o efeito medido é claro: W2→W1 é **no-op** no mundo real.

### B4 (bloqueante de dado, herdado) — Mumbai com ocupação 1,96 (>100% da capacidade)

Estoque 52,8 M unidades contra capacidade RES 26,8 M (531,4 km², FAR 6). A causa é a população de
cenário de **231.280.307** para `IND-MUM` contra **5.791.139** para `IND-DEL` na mesma nação
(`data/definitions/scenario/cities.json`; `housing_city.json` herda o estoque). É o mesmo problema que
o Q-013 já registrou (população de cenário, não aglomeração) — o §19 o amplifica para capacidade de
zona. Também: `informal_share` da cidade 5,4% mas 76,4% do padrão popular informal — coerente entre si,
incoerente com Mumbai real (40–50% de favela).

### B5 — Favela só em Mumbai, por um mecanismo de depreciação (não de população)

`informal_units` cresce **+246.316 (+8,6%)** apenas em Mumbai; zero nas outras cinco. A população de
simulação é **constante** nas 6 cidades (12,69 M em Mumbai, início = fim). O mecanismo é a depreciação
de 1%/ano do estoque **médio** (93% do estoque de Mumbai, split de Gini) transbordando para o
**popular** (7%): o excedente cruza o gatilho de 5% (`kDemoHousingSlumShortage`) e a favela cresce 1%
da demanda popular/mês. Em São Paulo o mesmo transbordo é 1,4% (abaixo do gatilho). Está “onde já
havia”, mas cresce por falta de oferta, não por demanda.

## 5. Desvio “obra de moradia não usa o projeto §15” — avaliado

**Aceitável na estrutura, com duas divergências que precisam ser declaradas.**

Procede o argumento do relatório: o §15 materializa `DemoBuilding` e a moradia entrega unidades
agregadas por densidade × padrão; forçar o §15 exigiria inventar um prédio por lote. O que foi mantido
está correto: compra de material no mercado local (mesmos **pesos** 0,50/0,30/0,20), mão de obra ao
`service_cash`, prazo de 90 d, entrega no estoque, caixa do dono debitado, dinheiro conservado
(`housing.cpp:418-480,687`). Porém:
1. **Fração de material 40%** (`kDemoHousingMaterialShare = 0,40`) contra **15%** do §15
   (`kDemoConstructionMaterialShare = 0,15`): a mesma obra em XGS exige **2,7× mais material**. Os
   pesos são os mesmos; a “receita” não é. Não está documentado como decisão.
2. **Sem checagem de capacidade do setor de construção** (`sector_workers[1] × 20 XGS/dia`,
   `construction.cpp:173-181,229-235`): obra §15 pausa quando o setor não comporta; moradia nunca pausa
   e paga 100% adiantado (o §15 paga por dia, do fundo reservado). Com 4 slots/cidade × 834 cidades,
   a construção de moradia pode exceder a capacidade física do setor.

## 6. O que passa (não bloqueia)

- Falências de construção **5 ≤ 28** nas 5 variantes; dinheiro **−0,0014** (tol. 0,01); determinismo
  (5 variantes byte-idênticas, como o F5 exige); SOL −3,07 p.p. na classe aceita pelo dono.
- Perf release p95 **45,06 ms** (mediana de 3) ≤ 50 ms.
- IPTU/receita e caixa municipal plausíveis (item 4).
- Testes: esta revisão **não re-rodou a bateria 8/8** nem os testes urbanos (fora do critério pedido;
  evidência do S-019). O verificador independente re-rodou o smoke do runner com a DLL debug
  restaurada (exit 0) e conferiu 7/7 itens deste parecer.

## 7. Condições para levantar o veto

1. **Moradia (B1/B2):** mostrar ≥1 projeto de moradia aberto e entregue num mercado com falta
   (São Paulo ou Tóquio) e `rent_burden` na faixa 0,15–0,40, com aluguel cobrado da **renda** (não da
   poupança) e `pago/faturado ≥ 0,7`. Ajustar `cap_rate`/anos de aluguel/terra de forma que
   `price ≥ 1,2 × custo` seja alcançável.
2. **Prefeitura (B3):** ≥1 obra do W2→W1 iniciada em cidade cheia (Mumbai) em 365 d, com a conta de
   caixa; ou declarar e corrigir o gate `committed_cost` × caixa municipal.
3. **Dado (B4):** corrigir/inventariar a população de cenário de `IND-MUM` (231 M vs 5,8 M em Delhi) e
   definir se unidade informal conta na capacidade da zona formal (ocupação >1 não deve existir).
4. **Evidência:** o relatório do S-019 deve declarar que o sweep 365 d **não tinha §19**; o runner
   corrigido (`--urban=1`) e `docs/economy/perf/s19_world_sweep.json` passam a ser a referência.

## 8. Reprodução

```powershell
# 5 variantes em paralelo (threads=1), §19 ligado; A/B sem §19 com -NoUrban
powershell -NoProfile -ExecutionPolicy Bypass -File tools/s19_world_sweep_parallel.ps1 -NoUrban

# baseline agregado (novo)
python tools/aggregate_s19_sweep.py --baseline docs/economy/perf/construction_world_sweep.json `
  --dir "$env:APPDATA/Godot/app_userdata/Open Grand Strategy/world_sweep" --prefix s19_ `
  --suffix _365.json --logdir "$env:TEMP/opengs_q015_sweep" --out docs/economy/perf/s19_world_sweep.json

# probe urbano (6 cidades) e leitura
godot --headless --path . --script res://tools/economy_world_test.gd -- --ai=1 --days=365 `
  --urban=1 --threads=1 --fx=real --baseline=res://docs/economy/perf/construction_world_sweep.json `
  --out=user://world_sweep/s19_probe_real_365.json `
  --urban-cities=BRA-SAO,JPN-TOK,NGA-LAG,IND-MUM,USA-NEW,AUS-WEI --urban-out=user://s19_urban_365.json
python tools/s19_urban_report.py "$env:APPDATA/Godot/app_userdata/Open Grand Strategy/s19_urban_365.json"
```

Perf: DLL release no slot debug + `godot --headless --path . res://tools/perf_day_peak.tscn --
--days=30 --tick-mode=worker --threads=8` (3×); restaurar a debug (`3B531795…`) ao fim.
