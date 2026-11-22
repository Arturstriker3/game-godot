# B1 — Demografia por faixa etária

Fase B1 do plano `docs/plans/perf_pop_financas_plano.md`. Sai de "1 POP por
cidade" para **cinco coortes** (0–14, 15–24, 25–44, 45–64, 65+) e faz a idade
dirigir trabalho, educação, saúde e reserva de recrutamento. **Muda resultado
de propósito** (recalibração da Parte B): a população total deixa de ser
constante e passa a crescer/encolher pelo fluxo natural.

## 1. Dado (2026, fonte pública)

`data/definitions/population/age_structure_2026.json`, gerado por
`tools/build_age_structure.py` de três arquivos do **UN WPP2024 (Medium,
2026)**:

| campo | uso |
| --- | --- |
| `bands` (5 frações) | pirâmide do dia 0 por nação; soma 1 |
| `frac_18_49` | reserva de recrutamento (fração da população total) |
| `cbr_per_1000` | nascimentos por mil/ano |
| `cdr_per_1000` | mortes por mil/ano |
| `tfr_2026` | guardado para fases seguintes (não usado em B1) |

São **179 nações** (mesmas tags do cenário). Nação sem dado fica no perfil
paramétrico anterior (ausência explícita): não se inventa coorte.

## 2. Estado

`Pop` ganhou `age_band[5]` (pessoas, mesma unidade de
`simulation_population`), `children` (a faixa 0–14), `demography_ready` e os
fatores `health_need_factor`/`education_need_factor` com os pesos de
referência `health_weight_ref`/`education_weight_ref`.

- `seed_demography` distribui a população de simulação pelas frações nacionais
  e guarda os pesos de referência do dia 0.
- `apply_demography` deriva `population = Σ bandas`, `working_age = 15–24 +
  25–44 + 45–64`, `retirees = 65+`, `children = 0–14`,
  `recruit_reserve = frac_18_49 × população` e os fatores de serviço.
- `apply_labor_profile` usa as coortes quando `demography_ready`; sem dado,
  mantém exatamente o perfil paramétrico anterior.

## 3. Fluxo mensal (cadência A6)

`SimCore::phase_demography` roda no dia da cidade
(`cadence_due(dia, índice, monthly_period)`, A6), em laço serial determinístico,
**antes** do trabalho. Um fechamento mensal:

1. `nascimentos = população × CBR/1000 ÷ 12`, todos na faixa 0–14.
2. `mortes = população × CDR/1000 ÷ 12`, repartidas entre as faixas pelo perfil
   relativo `kAgeDeathWeight` (a taxa **bruta** é o dado; só a composição é
   modelo).
3. envelhecimento: 1/15 da faixa 0–14, 1/10 da 15–24, 1/20 da 25–44 e 1/20 da
   45–64 passam à faixa seguinte por ano (÷12 no mês).
4. a escolaridade guardada é reescalada pela razão da idade ativa (o mix de
   níveis é preservado) e `city.population` (população regional exibida) é
   reescalada pela mesma razão da população de simulação.

Não há migração (B4). Os pesos de mortalidade e as frações de envelhecimento
são **modelo declarado** em `sim_constants.h` (ordem de grandeza de uma tábua
de vida; não são dado por país) e não pretendem reproduzir a pirâmide futura de
nenhuma nação — apenas a direção (país jovem envelhece, país velho encolhe).

## 4. A idade dirige

- **Trabalho:** `working_age` vem das coortes; participação por escolaridade
  continua a calibração anterior (`kLaborParticipation*`). A força de trabalho
  é `education[level] × participation − studying`.
- **Estudantes:** `students = children + studying` (0–14 em idade escolar mais
  quem está em formação fora da força).
- **Educação:** a necessidade de referência do setor é multiplicada por
  `education_need_factor` (peso das faixas 0–14 e 15–24 relativo ao dia 0).
- **Saúde:** a necessidade de referência é multiplicada por
  `health_need_factor` (65+ pesa mais, 45–64 sobe, jovens pouco).
- **Recrutamento:** `frac_18_49` observada × população corrente.
- **População variável:** consumo, necessidade de comida, pressão de moradia
  (`simulation_population / pop_anchor`), demanda de serviços, pesos fiscais e
  de investimento já leem o campo corrente — nenhum cache foi criado. É isso
  que destrava a prefeitura quando a cidade cresce (§19, pendência R2 do
  D-067).

## 5. Save v16 e snapshot

`pop.age_bands` (5 doubles), `pop.health_weight_ref` e
`pop.education_weight_ref` são gravados. **v15 e anteriores migram**: as
coortes são semeadas da estrutura nacional na carga, a escolaridade guardada é
reescalada pela razão da idade ativa e a força de trabalho é recalculada; sem
dado nacional a cidade fica no perfil paramétrico legado. Round-trip exato é
coberto por `economy_save_load_roundtrip_test` (0 diffs).

A definição estática viaja no snapshot do CLI (`definitions.age_structure`,
array ordenado pela tag) para o executável C++ rodar o mesmo mundo sem Godot.

## 6. Status

`pop.age_bands` (Array de 5) é publicado na ficha pesada
(`get_city_status`), republicado só quando muda (uma vez por mês). O resumo
diário não carrega o bloco, como os demais campos pesados da A4. Cidade sem
estrutura nacional (só o demo e cenários sintéticos) publica
`[0,0,0,0,0]` no status enquanto o save grava `[]` — as duas formas são a
mesma ausência explícita; no mundo real as 834 cidades têm dado, e a soma
das coortes é a população em todas.

## 7. Validação de padrão real

- `tools/population_age_test.gd`: pirâmide do dia 0 igual ao dado (7 nações,
  todas as cidades, tolerância 2e-4), soma das coortes = população em todas as
  cidades, sinal e taxa do primeiro ano contra `(CBR−CDR)`, JPN/DEU envelhecem,
  NGA não colapsa a fração jovem, dinheiro conservado.
- Projeção de 10 anos via CLI (3650 dias, IA) — ver §8.

## 8. Resultados (2026-11-17)

**Pirâmide do dia 0 e primeiro ano** (`population_age_test`, 365 d sem IA):
as frações das 5 faixas batem o WPP nas 834 cidades das 7 nações amostradas
(tolerância 2e-4) e a soma das coortes é a população em todas as cidades,
antes e depois. Taxa do primeiro ano vs `(CBR−CDR)` do dado: JPN −0,666%
(esperado −0,668), NGA +2,098% (+2,034), BRA +0,439% (+0,434), DEU −0,429%
(−0,421), USA +0,156% (+0,154), IND +0,904% (+0,887), AFG +3,103% (+2,824).
JPN 65+ 30,20→30,84% e DEU 24,24→24,87% (envelhecem); NGA 0-14 cai 0,57 p.p.
em fração, mas **cresce em absoluto** (+0,6%/ano) porque o total cresce
+2,1%/ano. Dinheiro drift 0,0001; nenhuma violação de invariante.

**Projeção de 10 anos (CLI, 3650 dias, IA, snapshot do dia 0):** população de
simulação do mundo 1,3199 → 1,4197 bi (+7,56%). Por nação, contra a taxa
natural composta de `(CBR−CDR)` do próprio dado de 2026:

| nação | 2026 | 2036 | Δ | natural WPP | desvio | 65+ 2026→2036 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| JPN | 24.487.700 | 22.891.082 | −6,52% | −6,48% | −0,04 p.p. | 30,20 → 35,81% |
| NGA | 39.372.000 | 48.405.817 | +22,94% | +22,31% | +0,64 p.p. | 3,09 → 4,75% |
| BRA | 47.474.294 | 49.612.906 | +4,50% | +4,42% | +0,08 p.p. | 11,92 → 19,12% |
| DEU | 18.641.548 | 17.859.814 | −4,19% | −4,13% | −0,06 p.p. | 24,24 → 29,63% |
| USA | 49.014.115 | 49.784.940 | +1,57% | +1,55% | +0,02 p.p. | 18,84 → 24,24% |
| IND | 80.331.704 | 87.885.414 | +9,40% | +9,23% | +0,17 p.p. | 7,61 → 13,59% |

O desvio máximo é 0,64 p.p. (NGA; o fluxo mensal roda 12/13 vezes por ano conforme
o índice da cidade, então a taxa medida não é exatamente a anual). JPN e DEU
encolhem e envelhecem, NGA cresce ~2,03%/ano (a taxa do dado — a expectativa de
"~2,4%" do plano não é a do WPP2026 para a Nigéria) e BRA ~0,43%/ano. O CSV
WPP 2036 não está no repositório (só o pacote de 2026), então a comparação de
2036 é com a taxa natural do próprio dado e com a ordem de grandeza das
projeções publicadas, não campo a campo. Evidência por nação (população e
pirâmide no dia 0 e no dia 3650): `docs/economy/perf/b1_projection_10y.json`.

**Sweep 365 d (real + sonda float, `b1_world_sweep.json`, baseline
`partA_world_sweep.json`):** falências **7** (real) / **11** (float) contra
teto 28; drift **−0,0001** / −0,0004; hash `11b6bc9e…` com a sonda diferente
(`800796dd…`). Comida: **4 nações reprovam no real** (LBY −11,72, AFG −7,71,
ARE −7,43, EGY −6,76 p.p.) e **2 no float** (EGY −22,23, SYR −3,05); detalhe
por nação e a lista de isenções em `b1_sweep_detail.json`. Ver §9.

**Equivalência e goldens:** CLI × Godot com IA (snapshot do dia 0, 60 d,
threads=1) → comparador com tolerância 0: **0 diferenças**
(`b1_equivalence.json`). Os goldens foram
regenerados e explicados: A0 60 d `save 4cc3e3c7…`/`status c4ebe5ee…` (2
rodadas, 0 diffs), S0 `real_60_ai` `76a092b3…`/`b052f14b…` e `real_60_noai`
`1582000d…`/`77c42a77…`; no demo o `save` muda só pelas chaves aditivas
(`3f80dd7e…`) e o `status_sha256` continua `10b6f339…` — a simulação
AGRIA/PORTUS não mudou. O harness S0 e o `world_equivalence.gd` passaram a
carregar a estrutura etária. Threads 1 × 16 no mundo real 60 d: mesmo
`trajectory_hash` (`43187a04…`) e 0 diffs. **Bench canônico N=3** (host
ocioso, editor fechado, release de medição, 60 d): tick de parede = dia total
**28,15 ms** (28,26/27,69/28,15), variação **2,05% ≤ 3%**, memória 130,3 MB,
fase `demography` **0,013 ms** (`docs/economy/perf/b1_main_bench.json`). O save sem tick e a migração
v15→v16 são cobertos pelo `population_age_test`; a revisão independente
achou e fez corrigir um bug real (`students` não incluía `children` no load
v16 até o primeiro dia).

**Urbano 365 d (SP, TOK, LAG, MUM, NEW, WEI; `b1_urban_365.json` /
`b1_urban_365_summary.json`):** ônus do aluguel **0,176–0,395** (faixa
0,15–0,40; WEI 0,395 encosta no teto), pago/faturado do **mês**
**0,91–0,99** (mínimo WEI 0,907; todos ≥0,7; a fatura mensal usa unidades ×
posse de aluguel × aluguel e o pago é o `rent_paid_month` no fechamento),
favela **Δ0** nas seis. População de LAG +2,05%, MUM +0,89%, SP +0,43%,
WEI +0,43%, NEW +0,15%, TOK −0,67%. A ocupação residencial fica em
0,843–0,846 (WEI 0,546) e o gatilho de obra da prefeitura é 0,90:
**nenhuma obra abriu em 1 ano** — o horizonte de 2%/ano não leva a ocupação
ao gatilho. É calibração do §19 (decisão do dono), não da demografia.

## 9. Desvios conhecidos (não mascarados)

**Comida dos importadores.** O A/B com a demografia **sem o fluxo mensal**
(medição da sessão de implementação; stdout não versionado, resumo em
`b1_sweep_detail.json`) devolve LBY 0,973 / AFG 1,000 / ARE 1,000 / EGY
0,994 (todas dentro do portão): a semeadura, as coortes e os fatores de
serviço do dia 0 estão limpos. O fluxo acrescenta 1–3% de população por ano e a cidade importadora
sem agricultura amplifica isso pela **bistabilidade de caixa** já registrada
na F9/§15 e em D-065/D-071 (poupança → 0, preço do trigo ~3×, consumo cai).
Fechar o portão exige o **item de modelo** (tesouro/solidariedade nacional de
importação), não um knob de demografia; nenhuma isenção nova foi criada e as
4 nações seguem no relatório. Decisão do dono.

## 10. Limites

- A composição das mortes por faixa e as frações de envelhecimento são
  **modelo** (ordem de grandeza de tábua de vida), não dado por país.
- Sem migração (B4) e sem estratos de renda (B2): a pirâmide é única por
  cidade.
- O mix de escolaridade continua vindo do índice de renda; a demografia
  apenas reescala o total pela idade ativa.
- A necessidade de comida continua per capita (não por faixa); só saúde e
  educação usam os pesos por idade.
- Sem crescimento monetário (Parte C): população crescendo dilui caixa por
  habitante — efeito visível nos importadores marginais.

