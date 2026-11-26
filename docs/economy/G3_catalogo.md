# G3 — Semeadura do catálogo (LOTE 1: vestuário/calçado)

**Data:** 2026-11-26 · **Branch:** `feat/g3-catalogo` (worktree `opengs-wt-g3`) · **Base:** `main` `128ae77b` (G2)
**Spec:** `G3_semeadura_catalogo.md` (rev. 2) + princípio “Semeadura por produtores reais” (G2 §2.3–2.7)
**Decisões do dono:** `seed_fraction` 0,02 em L1–L3 e 0,10 em L4–L5 (D-G3-1); teto de preço
`MA30(dia 365) ÷ MA30(dia 0) ≤ 1,15`.
**Revisão 2 (recalibração):** o 0,02 reprovou no gate de 365 d; o L1 passa a **0,90** por bem
(princípio “básico maduro nasce abastecido”, §7). A decisão registrada era 0,02 — a mudança
**aguarda confirmação do dono antes do merge**.
**Revisão 3 (correção 2, §8):** a saturação da semeadura (55 330 t/dia de shortfall com 439
plantas) foi atacada no teto por planta: o L1 declara `max_plant_capacity_per_100k = 10`
(t/dia por 100 mil habitantes simulados) e o alvo do país é dividido pelas cidades-alvo. O gate
de 365 d **continua reprovando** `clothes` (1,7750) e agora também `fabric` (1,3041); a hipótese
medida é que o gargalo restante está a montante (algodão/tecido), não no teto da planta.
**Revisão 4 (correção 3, §9):** a cadeia foi fechada a montante com o algodão semeado por
produtores reais (USDA FAS) via área de safra (`set_crop_plan`). O LOTE 1 **fecha** na métrica
oficial do dono (`MA30 d365 ÷ d0`): `clothes` 0,9702 · `fabric` 0,6897 · `shoes` 0,8895;
cadeia viva (16/16/5 países), comida 95/179, 2 falências, drift 0,0236, OFF `ded6f54b…`.

Este lote entrega **somente o LOTE 1** (`clothes`, `fabric`, `shoes`) e o **reuso** do mecanismo
G2. Lotes 2–5 não aterrissam aqui; o gate é medido no acumulado (hoje, G2 + L1).

## 1. Dado — `bootstrap_industry.json` v2

- `version: 2`; os 21 bens do G2 e seus números **não mudaram**. Três bens novos, no fim da
  lista (ordem determinística da semeadura):

  | bem | lot | `anchor_source` | produtores (participação) | fonte |
  |---|---|---|---|---|
  | `clothes` | L1 | `needs:basic/clothing` | CHN 0,30 · BGD 0,09 · VNM 0,08 · IND 0,07 · IDN 0,05 · TUR 0,04 · USA 0,04 · BRA 0,03 · ITA 0,03 · DEU 0,03 · KHM 0,03 · PAK 0,03 · ESP 0,02 · MEX 0,02 · LKA 0,02 · THA 0,02 · outros 0,10 | WTO *World Trade Statistical Review 2024* (exportações de vestuário como proxy de local) |
  | `fabric` | L1 | `needs:basic/clothing + life/footwear` | CHN 0,55 · IND 0,10 · USA 0,05 · PAK 0,04 · TUR 0,04 · BRA 0,03 · IDN 0,03 · VNM 0,03 · KOR 0,02 · ITA 0,02 · outros 0,09 | `default:balancing` (base ITMF/UN Comtrade) |
  | `shoes` | L1 | `needs:life/footwear` | CHN 0,52 · VNM 0,11 · IND 0,09 · IDN 0,05 · BRA 0,04 · TUR 0,03 · ITA 0,02 · MEX 0,02 · THA 0,02 · PAK 0,02 · DEU 0,02 · outros 0,06 | APICCAPS *World Footwear Yearbook 2024* |

- **Correção declarada em `shoes`:** a linha da spec imprime `outros 0,04`, mas os produtores
  listados somam 0,94 — a linha fecharia 0,98. Para cumprir o contrato §2.3 (participações somam
  1,00), o residual é `0,06`. Nenhum produtor foi inventado; a diferença é residual declarado.
- **Residual não vira planta:** `outros` é `unseeded_share`; LKA não tem cidade
  `sim_population ≥ 1 M` e também fica residual (publicado), como no G2 (CHE/EST).
- Capacidade em runtime (nunca literal): `cap_planta = share × âncora_mundial × seed_fraction`,
  piso 0,05 e teto 20 t/dia; cidade pelo critério padrão (1ª ≥ 5 M, senão a maior ≥ 1 M;
  `score` desc, nação/id asc); `budget_share` 0,50; ids `SEED-<bem>-<cidade>`.

## 2. Leitor — `seed_fraction` por bem (menor extensão necessária)

O leitor do G2 (`set_bootstrap_industry`) só tinha **uma** `seed_fraction` global. Como o dono
aprovou **0,02 para o L1** e os 21 bens do G2 precisam continuar em **0,10**, o leitor ganhou um
campo opcional por bem:

- `BootstrapGood::seed_fraction` (default 0 = herda `BootstrapIndustry::seed_fraction`);
  `bootstrap::plan` usa `good.seed_fraction > 0 ? good.seed_fraction : cfg.seed_fraction`;
  `set_bootstrap_industry` lê `entry.seed_fraction`.
- **G2 inalterado:** nenhum bem do G2 declara o campo, então todos herdam 0,10. O G3 usa
  `seed_fraction: 0,02` nos três bens do L1. A estrutura plana `goods` foi mantida (v1-compatível);
  `lot`/`anchor_source` são metadados de rastreio.
- `sim_unit_tests` ganhou o caso “seed_fraction por bem sobrepõe o global e o ausente herda”.

## 3. Testes

- `sim_unit_tests` (Release, C++): **75/75 casos, 758 asserções, 0 falhas** (era 74/74).
- `-Tier pr -Base main`: **42/42 OK** (run `20261126_035116`; 0 falha, 0 crash, 0 timeout),
  incluindo `economy_integration`, `economy_ai`, `population_demo`, `save_load_roundtrip`,
  `price_formation`, `production_chain`, `c1_world_ai_90` e `determinism_cross_process`.
  O runner avisou que a mudança toca o núcleo e recomenda `-Tier nightly` antes de um merge.

## 4. Gate de 365 dias — LOTE 1 a 0,02 (decisão do dono)

Comando: `c1_world_ai_365.gd --days=365 --bootstrap=1 --ai-brain=1 --goods-diag=clothes,fabric,shoes`
(DLL `-O3`, `threads=1`, mundo real; saída `G:/opengs_tmp/g3/on365.json`).

| critério | meta | medido (0,02) | resultado |
|---|---|---|---|
| violações / dias travados | 0 / 0 | **0 / 0** | OK |
| falências | ≤ 3 | **3** (BRA/iron d120, CHL/wheat d210, BRA/iron d332) | OK |
| portão de comida | ≤ 97/179 | **95/179** (mediana 0,96935) | OK |
| drift / máx. diário | < 0,03 | **0,014485 / 0,014608** | OK |
| `clothes` produzido **e** vendido | ≥ 3 países | **4** (CHN 77,3; USA 3,2; BGD 2,1; BRA 0,7 t/dia) | OK |
| `fabric` idem | ≥ 3 países | **5** (CHN 95,4; KOR 8,8; + vendas USA/BGD/BRA) | OK |
| `shoes` idem | ≥ 3 países | **2** (CHN 35,7; BRA 1,2 t/dia) | **REPROVA** |
| `clothes` MA30 d365 ÷ d0 | ≤ 1,15 | **3,1064** (411,82 / 132,57) | **REPROVA** |
| `fabric` idem | ≤ 1,15 | **1,2770** (111,96 / 87,68) | **REPROVA** |
| `shoes` idem | ≤ 1,15 | **1,2196** (262,12 / 214,93) | **REPROVA** |

Trajetória `a5bc02d6…`; 763 investimentos (677 privado / 86 grupo); parede 119,7 s (0,33 s/dia).

## 5. Diagnóstico — braço A (0,10), só medição

Para separar “fração” de “limite da cadeia”, o mesmo gate foi rodado com `seed_fraction = 0,10`
nos três bens (JSON temporário, restaurado ao 0,02 antes do commit; saída
`G:/opengs_tmp/g3/on365_f010.json`, trajetória `6972f9ad…`):

| critério | 0,02 | 0,10 |
|---|---:|---:|
| falências | 3 | 2 |
| comida | 95/179 | 96/179 |
| `clothes` MA30 d365 ÷ d0 | **3,1064** | **2,5127** |
| `fabric` idem | **1,2770** | 1,1046 (passa) |
| `shoes` idem | **1,2196** | 1,0647 (passa) |
| países com produção+venda (clothes/fabric/shoes) | 4 / 5 / 2 | 4 / 5 / 2 |

**Leitura:** subir a fração 5× melhora `fabric`/`shoes` no preço, mas **não** resolve `clothes`
(2,51 > 1,15) nem os **2 países** de `shoes`. A produção continua concentrada na China
(`clothes` 98,2 t/dia de um total de 2%–10% da âncora de 17,6 kt/dia). O gargalo não é só a
fração: as plantas semeadas fora da China não transformam capacidade em produção/venda
(insumo de algodão/tecido, demanda final limitada por renda e o teto anti-manada da IA).
Nenhum limiar, participação ou âncora foi afrouxado para “passar”; o lote **reprova** na
fração aprovada e também no braço A.

## 6. OFF — bit a bit

`--bootstrap=0 --ai-brain=0`, 365 d: **`trajectory_sha256 ded6f54b…` idêntico** ao OFF do G2
(0 falências, 0 violações, comida 97/179, drift 0,016673). O L1 fica atrás de `enabled`; com as
flags off nada novo roda.

## 7. Recalibração — princípio “básico maduro nasce abastecido” (0,90)

**Por que o 0,02 reprovou.** Vestuário é **necessidade básica de massa**, não bem em crescimento:
a demanda-âncora é enorme (17,6 kt/dia só em `clothes`) e o mundo real já nasce abastecido.
Semear 2–10% cria escassez artificial; o cérebro não a fecha em 1 ano porque o investidor tem
teto anti-manada (3 plantas por 30 d). A fração pequena serve a bens em crescimento (eletrônicos,
duráveis novos), não a básicos maduros.

**Princípio novo.** Para bem de necessidade básica madura, `seed_fraction` por bem ≈ **0,90 do
necessário para a demanda-âncora** (default declarado). A capacidade continua
`participação × âncora_mundial × seed_fraction`, distribuída pelos produtores reais como sempre;
o cérebro ainda constrói o crescimento e a margem. Aplicado a `clothes`, `fabric` e `shoes`
(`data/definitions/economy/bootstrap_industry.json`); os 21 bens do G2 seguem em 0,10.

**Gate de 365 d a 0,90** (`c1_world_ai_365.gd --days=365 --bootstrap=1 --ai-brain=1
--goods-diag=clothes,fabric,shoes`; saída `G:/opengs_tmp/g3/on365_f090.json`, trajetória
`108d9ad9…`, parede 114,3 s):

| critério | meta | medido (0,90) | resultado |
|---|---|---|---|
| violações / dias travados | 0 / 0 | **0 / 0** | OK |
| falências | ≤ 3 | **2** (BRA/iron d120, CHL/wheat d210) | OK |
| portão de comida | ≤ 97/179 | **94/179** (mediana 0,9704) | OK |
| drift / máx. diário | < 0,03 | **0,014074** | OK |
| `clothes`/`fabric`/`shoes` produzido **e** vendido | ≥ 3 países | **4 / 5 / 4** | OK |
| `clothes` MA30 d365 ÷ d0 | ≤ 1,15 | **2,2733** (318,37 / 140,05) | **REPROVA** |
| `fabric` idem | ≤ 1,15 | **1,1166** (98,19 / 87,93) | OK |
| `shoes` idem | ≤ 1,15 | **1,1114** (249,98 / 224,91) | OK |

**Por que `clothes` ainda reprova — hipótese medida.** A fração não chega ao plano: a semente
não coloca 0,90 da âncora no mundo, porque o número de **plantas satura**. O plano só pode criar
**uma planta por cidade elegível**, com teto de **20 t/dia** por planta; nas nações produtoras
não há cidades suficientes para cobrir a âncora de massa. Evidência na mesma DLL/mundo:

| | 0,02 | 0,10 (braço A) | 0,90 |
|---|---:|---:|---:|
| plantas semeadas (G2+L1) | 334 | 430 | **439** |
| `bootstrap_shortfall` (t/dia) | 4 865 | 7 620 | **55 330** |
| `clothes` produzido (t/dia) | 83,3 | 109,2 | **137,1** |
| `clothes` MA30 d365 ÷ d0 | 3,1064 | 2,5127 | **2,2733** |

De 0,10 para 0,90 o alvo cresce 9×, mas só entram **+9 plantas** (430 → 439) e o shortfall
cresce 7,3×: o gargalo é a capacidade por planta/cidade, não a fração em si. A produção de
`clothes` sobe 25,6% (109,2 → 137,1) e o preço cai de 2,51 para 2,27, mas não fecha 1,15. Fechar o princípio de
verdade exige **capacidade por planta proporcional ao bem** (ou mais de uma planta por cidade) —
implementado na correção 2 (§8); nenhum limiar, participação ou âncora foi afrouxado.

## 8. Correção 2 — teto da planta proporcional à cidade (medida)

**O que mudou (núcleo):** o teto fixo de **20 t/dia por planta** não conta a âncora de massa.
`BootstrapGood` ganhou o campo opcional `max_plant_capacity_per_100k` (t/dia por 100 mil
habitantes **simulados**; `0`/ausente = caminho do G2 inalterado). Quando declarado:

- o alvo do país (`share × âncora × fração`) é dividido pelo **número de cidades-alvo**
  (as elegíveis pelo gate 5 M → maior ≥1 M), sem blocos artificiais de 20 t/dia; o piso
  `capacity_min` limita quantas cidades entram (não cria migalhas);
- cada planta só é limitada por `valor × população / 100 000` (a cidade pode ter fábrica
  grande; 10 t/dia/100k é o default declarado do lote) — abaixo do orçamento por cidade
  (`pop/6000` t/dia com `capital_per_capacity = 3 000` e `budget_share = 0,5`), então o teto
  não briga com o caixa;
- `set_bootstrap_industry` lê `entry.max_plant_capacity_per_100k`;
- **G2 intacto:** os 21 bens não declaram o campo e passam pelo mesmo código (blocos de
  `capacity_max`); `sim_unit_tests` cobre os dois caminhos no mesmo mundo.
- JSON: `clothes`, `fabric` e `shoes` declaram `max_plant_capacity_per_100k: 10.0`.

**Testes:** `sim_unit_tests` **76/76** (771 asserções, era 75/758), incluindo o caso novo
“teto proporcional por cidade e ausente mantém o G2”.

**Smoke de 30 d** (`--days=30 --bootstrap=1 --ai-brain=1 --goods-diag=clothes,fabric,shoes`;
`G:/opengs_tmp/g3/smoke_fix.json`):

| | 0,90 saturado | correção 2 |
|---|---:|---:|
| plantas (G2+L1) | 439 | **426** |
| `bootstrap_shortfall` (t/dia) | 55 330 | **10 262** (−81,5%) |

O shortfall cai drasticamente mas não zera: o que sobra é o teto proporcional batendo em
cidades pequenas das nações produtoras (ex.: uma única cidade de ~2 M não absorve o alvo
nacional) e o orçamento local — nada foi inventado para “fechar” o número.

**Gate de 365 d com a correção** (`G:/opengs_tmp/g3/on365_fix.json`, trajetória `51386a8e…`):

| critério | meta | 0,02 | 0,90 | correção 2 |
|---|---|---:|---:|---:|
| violações / dias travados | 0 / 0 | 0 / 0 | 0 / 0 | **0 / 0** |
| falências | ≤ 3 | 3 | 2 | **2** (BRA/iron d120, CHL/wheat d210) |
| portão de comida | ≤ 97/179 | 95 | 94 | **93/179** |
| drift / máx. diário | < 0,03 | 0,014485 | 0,014074 | **0,020888** |
| `clothes`/`fabric`/`shoes` produzido **e** vendido | ≥ 3 países | 4/5/2 | 4/5/4 | **3/4/1** |
| `clothes` MA30 d365 ÷ d0 | ≤ 1,15 | 3,1064 | 2,2733 | **1,7750** (225,21 / 140,05) |
| `fabric` idem | ≤ 1,15 | 1,2770 | 1,1166 | **1,3041** (114,11 / 87,93) |
| `shoes` idem | ≤ 1,15 | 1,2196 | 1,1114 | **1,0800** (225,12 / 221,34) |

**Leitura medida.** A capacidade semeada deixou de saturar: `clothes` produzido+vendido sobe de
137 para **299 t/dia** (CHN 226, USA 59, BRA 15) e o preço cai de 2,27 para **1,78** — a
correção ataca o gargalo certo da planta. Mas o preço **não fecha** e `fabric` **piora**
(1,12 → 1,30): a produção de `clothes` agora acompanha o **tecido** disponível (347 t/dia de
`fabric` vendido; a cadeia é limitada pelo algodão), e a capacidade de `fabric` 37× maior
disputa o algodão e sobe o custo unitário (fabric 114 vs 98 no dia 365). `shoes` passa no preço
(1,08) mas cai de 4 para **1 país** produtor: as plantas de BRA/ITA etc. ficam sem insumo/caixa
e não vendem. Ou seja: o teto de 20 t/dia era **um** gargalo, não **o** gargalo; o restante
está a montante (algodão → tecido) e na cobertura de países — fora do escopo desta correção.

## 9. Correção 3 — algodão por produtores reais (medição 4) e fechamento do LOTE 1

**O que mudou (commit `624ed142`).** A correção 2 mediu que o gargalo restante estava **a
montante**: a capacidade de `fabric` semeada a 0,90 tinha ~37× a oferta de algodão do dia 0, o
algodão virava gargalo e o custo unitário subia a cadeia toda (`fabric` 1,3041). Em vez de
reduzir a semente do tecido, a cadeia foi **fechada a montante** com o mesmo princípio das
plantas:

- `data/definitions/agriculture/cotton_seed.json` — produtores reais de algodão em pluma
  2024/25 (USDA FAS *Cotton: World Markets and Trade*, mar/2026): China 0,27 · Índia 0,20 ·
  Brasil 0,14 · EUA 0,12 · Paquistão 0,04 · Austrália 0,05 · Turquia 0,03 · Uzbequistão 0,03
  (89,6% declarados; os 10,4% restantes **não** são semeados); rendimento 0,859 t/ha; aptidão
  mínima 0,30; **janela contínua declarada** (o dado do jogo não tem calendário de algodão —
  nenhum mês de plantio/colheita foi inventado).
- `core/economy/cotton_seed.gd` declara a **área de safra** dos produtores reais pelo contrato
  existente `set_crop_plan` (nenhuma construção nova): alvo =
  `min(capacidade instalada de fabric, demanda de tecido da cadeia) × algodão por tecido −
  lavouras já instaladas`; rateio pela participação de produção e, dentro da nação, área
  cultivável × aptidão da cidade; plano sem fazenda rastreada colhe maduro (D-125).
- O `RefiningService` chama a semente logo depois de `seed_industry` e **só** com
  `bootstrap.enabled`; com as flags desligadas nada é declarado.

Medido no dia 0 (`on365_c3.log`): `[algodão] alvo=16 865,2 t/dia (fabric 16 865,2, final
30 517,4, lavouras existentes 0,0) planos=19 área=6 091 292 ha rendimento=0,859
produtores=AUS,BRA,CHN,IND,PAK,TUR,USA sobra=15,0%`.

**Gate de 365 d com a correção 3** (`G:/opengs_tmp/g3/on365_c3.json`, trajetória
`c4668fb4…`, parede 122,2 s):

| critério | meta | medido | resultado |
|---|---|---|---|
| violações / dias travados | 0 / 0 | **0 / 0** | OK |
| falências | ≤ 3 | **2** (BRA/iron d147, CHL/wheat d210) | OK |
| portão de comida | ≤ 97/179 | **95/179** (mediana 0,9693) | OK |
| drift / máx. diário | < 0,03 | **0,023558 / 0,024841** | OK |
| `clothes`/`fabric`/`shoes` produzido **e** vendido | ≥ 3 países | **16 / 16 / 5** | OK |
| `clothes` preço MA30 d365 ÷ d0 | ≤ 1,15 | **0,9702** (147,98 / 152,52) | **OK** |
| `fabric` idem | ≤ 1,15 | **0,6897** (65,20 / 94,54) | **OK** |
| `shoes` idem | ≤ 1,15 | **0,8895** (251,54 / 282,80) | **OK** |

Cadeia viva: `fabric` vendido **20,8 kt/dia em 16 países** (`supply` 7,5 kt/dia),
`clothes` 7,2 kt/dia em 16 países, `shoes` 304 t/dia em 5; algodão **162 fazendas**, 19 planos
(2 fechadas); 438 plantas de bootstrap, `bootstrap_shortfall` **10 262 t/dia**.

**As DUAS leituras do preço — a oficial é `d365 ÷ d0`.** A métrica **oficial** do gate,
por decisão do dono, é `MA30(dia 365) ÷ MA30(dia 0) ≤ 1,15`. A leitura `MA30(dia 365) ÷
preço-base` do bem (`base_price`) é **apenas informativa**: o MA30 do dia 0 já nasce acima da
base por causa do estado herdado (C/D — `clothes` 1,202×, `shoes` 1,357×, `fabric` 1,080×), de
modo que dividir pela base mistura "o preço subiu em 365 d" com "o dia 0 já estava alto".

| bem | oficial d365 ÷ d0 | informativa d365 ÷ base |
|---|---:|---:|
| `clothes` | **0,9702** | 1,1663 |
| `fabric` | **0,6897** | 0,7452 |
| `shoes` | **0,8895** | 1,2068 |

A leitura ÷base **não** é gate e nenhum limiar foi movido; ela é publicada só para rastreio.
Nas duas leituras, `fabric` cai (o algodão deixou de ser gargalo e o tecido ficou barato) e
`clothes`/`shoes` ficam estáveis na oficial.

**A saga completa (cada passo atacou o gargalo que o anterior mediu):**

| passo | clothes / fabric / shoes (d365÷d0) | o que o passo mediu |
|---|---|---|
| 0,02 (decisão original) | 3,1064 / 1,2770 / 1,2196 | fração pequena em bem de massa = escassez artificial |
| 0,10 (braço A, diagnóstico) | 2,5127 / 1,1046 / 1,0647 | 5× a fração não fecha `clothes` |
| 0,90 ("básico maduro nasce abastecido") | 2,2733 / 1,1166 / 1,1114 | a fração não chega ao plano: 1 planta/cidade × teto de 20 t/dia satura (shortfall 55 330) |
| teto proporcional à cidade (correção 2) | 1,7750 / 1,3041 / 1,0800 | o teto era **um** gargalo; agora o algodão limita o tecido |
| **algodão por produtores reais (correção 3)** | **0,9702 / 0,6897 / 0,8895** | **cadeia inteira abastecida: passa** |

**OFF bit a bit:** `--bootstrap=0 --ai-brain=0`, 365 d: `trajectory_sha256 ded6f54b…` idêntico
ao controle (0 falências, 0 violações, comida 97/179, drift 0,016673).

**Estado.** LOTE 1 **fechado pela métrica oficial** do dono. `sim_unit_tests` **76/76** (771
asserções) e `-Tier pr -Base main` **49/49 OK** (run `20261126_074912`; 0 falha, 0 crash, 0
timeout). **Pendente antes do merge:** (a) confirmação do dono para o princípio "básico
maduro nasce abastecido ~0,90" (a decisão registrada era 0,02); (b) `-Tier nightly`, que o
runner recomenda por a mudança tocar o núcleo; (c) LOTE 2 (alimentos/bebidas), que segue o
mesmo padrão: cadeia inteira a montante abastecida (inclusive insumos agrícolas por produtores
reais) + `max_plant_capacity_per_100k` por bem.

Evidências: `G:/opengs_tmp/g3/{on365,on365_f010,on365_f090,on365_fix,on365_c3,off365,
off365_f090,off365_fix,off365_c3,smoke_f090,smoke_fix,smoke_c3_30}.json` e logs `*.log`.

## 10. LOTE 2 — alimentos/bebidas (andamento)

**Dado.** `meat`, `feed`, `canned_food`, `alcohol` e `containers` entram no
`bootstrap_industry.json` com `lot: L2`, `seed_fraction: 0,90` (princípio do §7) e
`max_plant_capacity_per_100k: 10` (teto da correção 2). Participações = tabela §4 da spec rev.2
(FAO *Meat Market Review 2024* no `meat`; o resto `declared:balancing`), cada linha somando 1,00
com `outros` residual. `premium_spirits`, que estava no lote, **saiu para o LOTE 4**: a receita
depende de `planks` (L4) e o bem não tem como nascer abastecido antes disso (mediu 0 vendido).

**Insumos a montante.** Os cinco primários que alimentam `feed` e `alcohol` (maize, wheat, rice,
soy, potato) **já** nascem semeados por produtores reais: o `AgricultureService` declara, por
nação, a área colhida FAOSTAT (`crop_yields.json`), o calendário (`crop_calendar.json`) e o rateio
por capacidade de terra — o mesmo contrato que o algodão usa (`set_crop_plan`). O lote **não
duplica** essa semente nem sobrescreve plano existente; a medição diz se a área cobre a demanda
nova. (`bran`, variante c do `feed`, não tem produtor no dia 0; o `choose_recipe` cai na primeira
receita com aptidão local — milho —, como no resto do jogo.)

**Smoke 30 d ON** (`c1_world_ai_365.gd --days=30 --bootstrap=1 --ai-brain=1
--goods-diag=meat,feed,canned_food,alcohol,premium_spirits,containers,maize,wheat,plastic,
aluminum_ingot`; `G:/opengs_tmp/g3/l2_smoke_30.json`, trajetória `35e1c82e…`, 9,1 s):

| métrica | medido |
|---|---|
| falências / violações / travados | **0 / 0 / 0** |
| drift | **0,017667** |
| comida (avg30) | 108/179 (mediana 0,9555; **sem braço de controle** — não atribuível ao L2) |
| plantas de bootstrap / shortfall / sobra | 536 / **207 143 t/dia** / 4,8% |
| grãos a montante | maize **413 kt/dia em 164 países** (MA30 1,0026), wheat **697 kt/dia em 134** (1,0402) |
| vendas (avg30) | meat 1 415 t/dia em 10 · feed 3 288 em 10 · alcohol 82 em 6 · containers 66 em 5 · canned_food 0,7 em 1 · premium_spirits 0 |

**Bloqueios medidos.** (1) `containers` escolhe a **primeira** receita (`containers_a`,
`aluminum_ingot`), porque nenhum insumo tem aptidão local no `choose_recipe`; o alumínio não tem
planta no dia 0 (preço 18,75 fixo, 0 vendido) → embalagem sem insumo → `canned_food` em 1 país.
(3) O shortfall de 207 kt/dia mostra que a fração 0,90 não chega ao plano nos bens de massa
(mesma família do §7, agora com âncoras maiores).

### 10.1 Correção do elo do alumínio (após o merge do H1 `806554ef`)

**Mecanismo (dado, não regra).** O bootstrap ganha o campo opcional por bem
`capacity_from: "<bem-fonte>"`: o alvo mundial do bem-insumo passa a ser o consumo dos planos
**já semeados** do bem-fonte — `Σ capacidade × qty / yield` da receita escolhida para cada planta
— e não `âncora × seed_fraction`. As participações só distribuem esse total e são normalizadas
sem o residual `outros` (o insumo do lote precisa ser coberto). O bem-fonte deve ser declarado
antes no JSON; campo ausente/0 mantém o caminho G2 **bit a bit** (teste C++ novo cobre os dois
caminhos e o caso `no_input_need`).

**Dado.** `aluminum_ingot` entra no L2 com `capacity_from: containers` e as participações USGS
— *Mineral Commodity Summaries 2025*, Aluminum, World Smelter Production 2024e (kt): China
44 000, Índia 4 200, Rússia 3 880, Canadá 3 320, EAU 2 690; normalizadas sobre os cinco
produtores nomeados (0,7574 / 0,0723 / 0,0668 / 0,0572 / 0,0463). A bauxita **já** nasce
minerada no dia 0 (`extraction_production_2024.json`, USGS MCS 2025, 13 países): o lote não
duplica a mineração. `aluminum_ingot` é a única transformação nova (`1 bauxita → 1 lingote`,
`recipes.json`).

**Medição do plano (2 d, instrumentação removida depois).** O alvo do alumínio = **9 312 t/dia**
(capacidade `containers_a` semeada do lote); semeia **497 t/dia** (25 plantas de 20 t/dia), o
resto barrado pelo `budget_share` das cidades (as mesmas cidades-alvo de `containers`). Os
containers só consomem ~580 t/dia no ano, então o alvo do plano é capacidade potencial, não a
demanda realizada — é a mesma natureza do shortfall dos outros bens do lote.

**Smoke 30 d ON (corrigido).** `--days=30 --bootstrap=1 --ai-brain=1 --goods-diag=meat,feed,
canned_food,alcohol,containers,aluminum_ingot,bauxite,plastic,maize,wheat`
(`G:/opengs_tmp/g3/l2_fix_smoke30.json`, 9,4 s): 0 falências/0 violações/0 travados; drift
**0,017448**; comida 108/179 (igual ao smoke anterior; a correção não mexe na comida em 30 d);
**544** plantas construídas (plano 556), shortfall **214 377 t/dia** (era 207 143 — o alumínio
acrescenta 8,8 kt/dia de alvo que o orçamento não constrói); `aluminum_ingot` **produz 395,7
t/dia e vende 740,3 em 6 países** (stock em trânsito); `containers` **840 t/dia em 8 países**
(era 66 em 5); `canned_food` **15,4 t/dia em 2** (era 0,7 em 1); bauxita 81 países produtores.

### 10.2 Gate de 365 d do L2 (acumulado L1 + L2) — REPROVADO

`c1_world_ai_365.gd --days=365 --bootstrap=1 --ai-brain=1 --goods-diag=...`
(`G:/opengs_tmp/g3/l2_on365.json`, trajetória `21cb74ae…`, 476 s). Controle **sem o L2** no
mesmo código (`--bootstrap-path=G:/opengs_tmp/g3/bootstrap_l1_only.json`, trajetória
`c4668fb4…` = o ON do L1): drift 0,023558, comida 95/179, 2 falências, 426 plantas.

| critério | medido (L2) | veredito |
|---|---|---|
| violações / travados | 0 / 0 | OK |
| falências | 2 (≤3) | OK |
| comida (avg30) | **91/179** (≤97; mediana 0,97297) | OK |
| drift | **0,032657** (máx. diário 0,036812) | **REPROVA** (>0,03) |
| bens do lote ≥3 países | meat 12, feed 10, alcohol 9, containers 7, aluminum 7, **canned_food 2** | **REPROVA** (canned_food) |
| MA30 d365÷d0 ≤1,15 | meat 0,9663; alcohol 0,9760; aluminum 0,9108; **feed 1,4279; containers 1,2076; canned_food 1,1886** | **REPROVA** (3 bens) |

**Atribuição.** O braço de controle (L1-only) reproduz o ON do L1 bit a bit (`c4668fb4…`,
drift 0,023558, comida 95/179), então o merge do H1 não muda nada e o delta é do L2: drift
**+0,009099** (0,023558 → 0,032657) e comida **-4** (95 → 91). O `canned_food` só vende em CHN e
IND (2 países) apesar de o alumínio e os containers estarem vivos: o gargalo passou a ser a
própria demanda/fábrica de `canned_food`, não o insumo.

**Leitura honesta.** A correção do alumínio funciona (o elo existe, produz e vende; containers
saem de 66 → 840 t/dia e `canned_food` de 0,7 → 15,4), mas o **gate de 365 d do L2 REPROVA** nos
três critérios acima — nada foi afrouxado. O shortfall do plano **não caiu** (207 143 → 214 377)
porque o alvo do alumínio (capacidade dos containers do lote) é potencial e o orçamento das
cidades só constrói 497 t/dia; medir o elo pela demanda realizada exigiria um critério novo no
bootstrap (fora desta tarefa). O merge do L2 fica bloqueado até uma decisão sobre drift/preços/
`canned_food`.
