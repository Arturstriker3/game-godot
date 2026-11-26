# G3 — Semeadura do catálogo (LOTE 1: vestuário/calçado)

**Data:** 2026-11-26 · **Branch:** `feat/g3-catalogo` (worktree `opengs-wt-g3`) · **Base:** `main` `128ae77b` (G2)
**Spec:** `G3_semeadura_catalogo.md` + princípio “Semeadura por produtores reais” (G2 §2.3–2.7)
**Decisões do dono:** `seed_fraction` 0,02 em L1–L3 e 0,10 em L4–L5 (D-G3-1); teto de preço
`MA30(dia 365) ÷ MA30(dia 0) ≤ 1,15`.

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

## 7. Estado e próximos passos

- **Sem merge** (QA depois dos lotes 1–3, conforme o briefing). Branch `feat/g3-catalogo`.
- **Lotes 2–5 não aterrissaram.** O gate do L1 está reprovado; a decisão de continuar/rever
  a âncora ou o critério de preço de bens finais de massa é do dono/Game Systems Designer.
- Evidências: `G:/opengs_tmp/g3/{on365,on365_f010,off365}.json` e logs `*.log`.
