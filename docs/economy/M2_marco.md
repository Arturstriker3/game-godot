# M2 — fechamento da Parte B (bateria longa do marco)

> Marco **M2** de `docs/plans/perf_pop_financas_plano.md` (§Marcos): *"parar, rodar a
> bateria longa e atualizar o `GAME_STATE.md` antes de seguir"*.
>
> - **Data:** 2026-11-18.
> - **Checkout:** `G:/projetos/opengs-game`, branch `balance/global-resources`,
>   HEAD `f2a845a` (merge da B5: Parte A + B1–B5 + minas, **save v21**).
> - **Host:** não ocioso durante a bateria, o sweep e o bench — outra worktree
>   (`opengs-wt-b7`) rodava Godot headless (`tools/_tmp_split_diag.gd`, PIDs
>   21576/28956) e havia Chrome/Steam ativos. Onde o critério de repetibilidade
>   depende disso, o número que vale é a **mediana** (mesma regra de A5/A6/A7/B2).
> - **Editor:** nenhum editor com janela foi aberto ou encerrado; os dois
>   `godot-editor-mcp` (sem janela) seguiram rodando. A DLL debug do slot foi
>   devolvida pelo próprio bench com hash conferido.
> - **Código de simulação:** nada foi alterado nesta rodada. A única mudança de
>   ferramenta é `tools/b5_world_ai_365.gd` aceitar `--out=` (evidência do M2 sem
>   sobrescrever a da B5).

## 1. O que o marco cobre

| Fase | O que entrou | Save | Doc |
|---|---|---|---|
| A0–A7 + TURBO | benchmark, núcleo/CLI, IDs, fila A3, status A4, IA em C++, cadência A6, comércio A7 | v15 | `docs/economy/refactor/A*_*.md` |
| B1 | demografia por 5 faixas etárias (WPP2024) | v16 | `docs/economy/B1_demography.md` |
| B2 | 5 quintis de renda com patrimônio, consumo por estrato, Gini medido | v17 | `docs/economy/B2_strata.md` |
| B3 | Região = estados inteiros (6,10 bi de interior, 129 países) | v19 | `docs/economy/B3_region.md` |
| B4 | migração (êxodo rural + cidade→cidade) | v20 | `docs/economy/B4_migration.md` |
| B5 | infraestrutura do estado (estrada/luz/água) + concessões com royalty | v21 | `docs/economy/B5_state_levers.md` |
| minas | estágio de mina (D-086), semeadura pela produção real 2024 (D-087) | v18 | `docs/economy/mine_stages.md` |

Decisões no `GAME_STATE.md` §14: **D-082** (B1), **D-083** (B2), **D-084** (início em
2027-01-01), **D-085** (B3), **D-086/D-087** (minas), **D-088** (B4), **D-089** (B5).

## 2. Bateria longa — 73 testes, um por vez

Todos os `tools/*_test.gd` que são `extends SceneTree` (65) e todas as cenas de teste
`tools/*_test.tscn` (8 — `services_test` é **cena**: com `--script` o processo trava),
cada um em processo próprio, com timeout individual e exit code capturado.

**Resultado: 66 verdes / 7 vermelhos**, 2.192,5 s de execução somada (~36,5 min).
Nenhum timeout.

| teste | tipo | resultado | s |
|---|---|---|---|
| alerts_nav_test | script | OK | 8,4 |
| b3_region_test | script | OK | 128,7 |
| b4_migration_test | script | OK | 155,6 |
| b5_state_levers_test | script | OK | 49,2 |
| camera_limit_test | script | OK | 17 |
| city_gov_test | script | OK | 182,3 |
| city_tooltip_test | script | FALHA | 7,8 |
| command_replay_test | script | OK | 6,7 |
| construction_project_test | script | OK | 6,9 |
| construction_queue_test | script | OK | 238,2 |
| culture_db_test | script | OK | 6,7 |
| currency_trade_test | script | OK | 6,7 |
| cursor_hover_test | script | OK | 12,1 |
| deposit_pick_test | script | OK | 14,1 |
| deposit_ring_test | script | OK | 10,6 |
| economy_ai_test | script | OK | 9,9 |
| economy_bistability_test | script | FALHA | 151,8 |
| economy_city_catchment_test | script | OK | 83,8 |
| economy_city_status_test | script | OK | 6,7 |
| economy_deposit_ownership_test | script | FALHA | 49,4 |
| economy_external_income_test | script | OK | 6,9 |
| economy_income_distribution_test | script | FALHA | 7 |
| economy_integration_test | script | OK | 8,2 |
| economy_light_signals_test | script | OK | 8,9 |
| economy_live_path_test | script | OK | 10,9 |
| economy_long_run_test | script | OK | 8,1 |
| economy_save_load_roundtrip_test | script | OK | 40,4 |
| economy_save_load_v11_compat_test | script | OK | 32,1 |
| economy_tick_async_test | script | OK | 13,3 |
| economy_world_test | script | OK | 14,6 |
| fleet_fuel_test | script | OK | 6,7 |
| goods_data_test | script | OK | 6,7 |
| history_service_test | script | OK | 6,6 |
| housing_market_test | script | OK | 84,2 |
| investment_test | script | OK | 7 |
| labor_market_test | script | OK | 6,8 |
| logistics_template_test | script | OK | 8,5 |
| map_pick_test | script | OK | 14,8 |
| market_template_test | script | OK | 10 |
| mine_stage_test | script | OK | 8,1 |
| minimap_territory_test | script | OK | 8,1 |
| minimap_view_test | script | OK | 6,8 |
| nation_city_list_test | script | FALHA | 7,9 |
| nation_highlight_test | script | OK | 7,5 |
| politics_dossier_test | script | OK | 8,4 |
| pop_needs_test | script | FALHA | 6,9 |
| population_age_test | script | OK | 49,1 |
| population_demo_test | script | OK | 6,6 |
| population_template_test | script | OK | 16,3 |
| port_capacity_test | script | OK | 6,8 |
| production_chain_test | script | OK | 6,7 |
| production_template_test | script | OK | 8,8 |
| services_business_audit_test | script | OK | 47,1 |
| services_template_test | script | OK | 10 |
| statistics_template_test | script | OK | 9,4 |
| status_query_test | script | OK | 16,6 |
| strata_test | script | OK | 172,3 |
| territory_test | script | OK | 11,1 |
| trade_flow_tooltip_test | script | OK | 11,8 |
| trade_template_test | script | OK | 8,1 |
| turbo_equivalence_test | script | OK | 92,7 |
| urban_land_test | script | OK | 7,9 |
| urban_load_test | script | OK | 27,5 |
| wheat_card_test | script | FALHA | 12,7 |
| wheat_info_test | script | OK | 14,1 |
| business_name_test | scene | OK | 8,4 |
| convoy_lifecycle_test | scene | OK | 81,2 |
| label_layout_test | scene | OK | 10,7 |
| map_frame_test | scene | OK | 15,2 |
| menu_smoke_test | scene | OK | 7,1 |
| nav_strait_test | scene | OK | 8,7 |
| services_test | scene | OK | 8,7 |
| vehicle_spacing_test | scene | OK | 8,9 |

### 2.1 Os 7 vermelhos — atribuição

Nenhum é regressão desta rodada (não houve mudança de código): são testes que ficaram
para trás de fases já mescladas. Cada item fica registrado como dívida de teste.

| teste | falha | causa atribuída | evidência |
|---|---|---|---|
| `city_tooltip_test` | "nenhuma cidade com porto no resumo" | A A4 (D-079) tornou o `last_summary` **leve**: cidades/portos só por `ui_snapshot`/`get_city_status`. O teste procura cidades com porto no resumo diário. | `core/economy/economy.gd` `_publish()` ("A4: o sinal carrega só o RESUMO LEVE") |
| `nation_city_list_test` | "resumo sem cidades da nacao GBR" | Mesmo motivo: lê `HUD._summary.cities`, que a A4 deixou fora do resumo. | idem |
| `economy_income_distribution_test` | fatias basic/life/luxury fora da meta F7 (USA/DEU basic 0,400; somas 0,6–0,8) | Afirma o consumo por **classe F7**; a B2 (D-083) passou o consumo para **quintis** (`stratum_level`), mantendo a classe só como publicação. | `docs/economy/B2_strata.md` §2 |
| `pop_needs_test` | "renda baixa: satisfação life 0,200 sem orçamento" | Mesma troca de contrato da B2 (consumo do estrato mais pobre). | idem |
| `economy_bistability_test` | GBR 730 d = 38,4% (meta ≥70%) | Roda a **IA GDScript legada** (`industry_templates.gd`), que a A5 (D-075) tirou do caminho do jogo e não recebeu as fases B. | `GAME_STATE.md` §15 "Parâmetros duplicados" |
| `economy_deposit_ownership_test` | "nenhum extrator construído em 180 dias" | Mesma IA legada — sem a semeadura/regras de mina da D-086/D-087. | idem |
| `wheat_card_test` | "linha de terreno ausente ou incompleta" | Monta a ficha sob **locale en** (rótulos "Climate: temperate humid…") e exige o texto pt-BR; fragilidade de i18n do teste, não do jogo. | log da bateria (`labels` em inglês) |

## 3. Equivalência A0 — mundo real, 60 d, IA + urbano

`tools/world_equivalence.ps1 -Rounds 2 -Baseline docs/economy/refactor/baseline/A0_60_urban.json`

| item | resultado |
|---|---|
| rodada 1 × rodada 2 | **0 diferenças** (comparador campo a campo, tolerância 0) |
| rodada 1 × golden | **shas e config iguais** |
| `save_sha256` | `bdd355b3bdb028233af25114541f76eb8879c6d7320398490f7a2ad2ad5ea373` |
| `status_sha256` | `ae6c500b5e2c8aeaf4f0ecbcadde7bd7f1f04fb28a63e113156a75ccd0726b77` |
| `money_total` | 6.523.551.982,9000225 XGS |
| falências / negócios / produtos | 0 / 41.802 / 113 |
| tempo | 38 s e 49 s por rodada |

O merge da B5 é **determinístico e idêntico ao golden vigente** (o orquestrador já o
havia regerado na B5; esta rodada confirma que nada se moveu desde então).

## 4. CLI × Godot — 10 dias com IA

1. `tools/export_world_snapshot.gd --ai=1 --urban=0` → snapshot 61.834.369 B
   (849 cidades, 3.222 construções, 113 produtos, `industry_seeded=true`).
2. `tools/snapshot_replay.gd --ai=1 --days=10` (Godot) × `opengs_sim_cli.exe --days=10 --ai=1`.

| item | resultado |
|---|---|
| arquivos | **byte-idênticos**, 70.854.209 B, SHA-256 `2D7D35C49CEFCD234556DF3470D9BCB289F61288ACEFBFBD71611C651F64512B` |
| comparador (`world_equivalence_compare.py`, tolerância 0) | **0 diferenças** |
| violações de invariante | 0 |
| `money_total` | 6.523.551.982,90 XGS |
| tempo | Godot 42 s de parede (1.721 ms de tick nos 10 d); CLI 22 s (13.059,4 ms total, 1.305,9 ms/dia **incluindo** a carga de 61,8 MB) |

## 5. Sweep 365 d — real + sonda float

`tools/s19_world_sweep_parallel.ps1 -Variants real,float -OutPrefix m2_ -Baseline docs/economy/perf/b4_world_sweep.json`
(2 processos, `--urban=1 --threads=1 --ai=1`), agregado por `tools/aggregate_s19_sweep.py`
→ `docs/economy/perf/m2_world_sweep.json`. Duração 3,7 min.

| item | real (aceite) | sonda float | limite |
|---|---|---|---|
| falências de construção | **4** | 4 | 28 |
| drift de dinheiro | **−0,0022** | −0,0012 | 0,01 |
| negócios de serviço (abertos / falidos) | 41.506 / 12.536 | — | — |
| `trajectory_hash` | `218b4be0…` | `13d1e6df…` | peg idêntico entre variantes peg; sonda **tem** de diferir |
| nações fora do portão de comida (média 30 d, tol. 3 p.p.) | **12** | 16 | 0 |
| moedas carregadas | 136 (0 flutuantes) | 136 (136 flutuantes) | — |

**Comida (dia 365, `food_avg30`):** mediana **0,7065**, 40 nações ≥ 0,975, 59 ≥ 0,90,
139 < 0,975, 50 < 0,50, 25 < 0,25, 11 < 0,05; pior **PSX 0,000**.

**As 12 nações do real fora do portão** (média de 30 d, baseline B4 → M2):

| nação | B4 | M2 | Δ (p.p.) |
|---|---|---|---|
| ARE | 0,653 | 0,121 | −53,2 |
| AGO | 0,884 | 0,706 | −17,8 |
| KGZ | 0,319 | 0,188 | −13,1 |
| BFA | 0,748 | 0,632 | −11,6 |
| GNQ | 1,000 | 0,894 | −10,6 |
| CAF | 0,570 | 0,489 | −8,1 |
| NER | 0,675 | 0,596 | −7,8 |
| COD | 0,989 | 0,928 | −6,1 |
| KEN | 0,715 | 0,658 | −5,7 |
| TZA | 0,751 | 0,704 | −4,8 |
| SOM | 0,763 | 0,729 | −3,4 |
| HTI | 0,401 | 0,371 | −3,0 |

Na sonda float somam-se BEN, DEU, GMB, HND, LSO e MMR (16 no total). `DJI` cai 36,1 p.p.
mas é isenta declarada (junto de SOL/SAH/JOR/YEM); nenhuma isenção nova foi criada.

**Piores 20 do real (`food_avg30`):** PSX 0,000 · BHR 0,000 · KWT 0,000 · QAT 0,000 ·
SAH 0,000 · EGY 0,002 · SAU 0,013 · MNG 0,030 · MRT 0,040 · JOR 0,044 · DJI 0,044 ·
SGP 0,065 · YEM 0,075 · ARE 0,121 · LBY 0,163 · SOL 0,163 · TKM 0,164 · SDN 0,169 ·
LKA 0,170 · CRI 0,171.

**Melhoras > 3 p.p. vs B4:** GMB +55,3 · BGD +20,2 · THA +16,5 · LBY +16,3 · CHE +7,8 ·
LSO +5,0 · BDI +4,5 · AUT +4,2 (10 nações).

## 6. Mundo em 365 d — obras, concessões, migração e urbanização

| medida (365 d, IA + urbano) | valor |
|---|---|
| obras de infra concluídas pela IA | **1.813** — 754 estrada / 619 luz / 440 água, em **693 regiões** |
| concessões ativas (royalty > 0) | **346** |
| falências / drift | 4 / **−0,002192** (`docs/economy/perf/m2_world_ai_365.json`) |
| comida mínima por nação (satisfação) | AFG **0,0000** (dívida pré-existente da B3, ver §8) |
| urbanização (mundo) | **0,256709 → 0,256209** |
| população urbana / região | 2.105.914.306 → 2.119.704.251 (+13,79 M) / 6.097.586.756 → 6.153.620.747 (+56,03 M) |
| migrantes interior → polo (`rural_in`) | **18.983,7** |
| cidade → cidade (`city_in` = `city_out`) | 37.479,5 / 37.479,5 |
| BRA / CHN / demais principais | BRA rural_in 0; CHN 1.184; IND, NGA, ETH, USA, IDN, PAK, BGD, EGY 0 |

Evidência: `docs/economy/perf/m2_urbanization_365.json`. **Leitura:** a migração move
18.984 pessoas em um ano enquanto o interior cresce **+56,0 M** — a urbanização
**cai** 0,05 p.p. porque a demografia do interior domina o êxodo. É a dívida de
calibração "migração baixa" quantificada (§8).

## 7. Bench canônico — 3 × 60 d (release de medição, 60 d, editor fechado)

`tools/bench_canonical.ps1 -Out docs/economy/perf/m2_bench.json`; DLL release
**reconstruída do HEAD** (`2.996.224 B`, `OGS_PROFILE_RELEASE=ON`, build sem warnings)
e devolvida a debug no slot com hash conferido ao final.

| métrica | rodadas (ms) | mediana | variação |
|---|---|---|---|
| `tick_wall_ms` = `day_total_ms` | 36,73 / 39,61 / 40,33 | **39,61** | **9,08%** |
| memória estática | 142,4 MB nas 3 | — | — |

**Critério A0 de 3% entre rodadas: NÃO FECHOU (9,08%)** — o host não estava ocioso
(outra worktree com Godot headless + Chrome/Steam, avisado pelo próprio script).
Como em A5/A6/A7/B2, o número que vale é a **mediana**: **39,61 ms** de dia/tick,
**3,93×** o A0 (155,51 ms) e praticamente igual ao bench da B2 (39,03 ms, +1,5%,
dentro do ruído do host).

Fases (mediana, ms): `ai` **13,78** (`ai_view` 10,48), `arrival` **5,92**, `prologue_snapshot`
**5,27**, `trade` **2,67** (`trade_food` 1,73), `services` 1,79, `production` 1,74,
`raw` 1,19, `reset` 0,87, `fiscal` 0,87, `income` 0,81, `accounting` 0,64, `housing` 0,52,
`invariants` 0,35, **`migration` 0,29**, `prices` 0,17, `labor` 0,17, **`region` 0,14**,
`clamp` 0,11, `land_month` 0,05, **`demography` 0,016**, `status` 0,008. O caminho da
B1–B5 custa **~0,45 ms/dia** (`migration` + `region` + `demography`) — o custo continua
na IA e no retrato do prólogo.

## 8. Dívidas de calibração consolidadas

| # | dívida | número medido (M2) | mecanismo / por que não é knob | próximo passo |
|---|---|---|---|---|
| 1 | **Comida — portão do sweep** | 12 nações (real) / 16 (float) pioram > 3 p.p. vs B4; 139/179 < 0,975; mediana 0,7065 | A B5 cobra **+30% de acesso** (`kInfraAccessPenaltyMax`) na comida comprada pela Região no polo e **todas as regiões nascem com infra 0** (não há dado de cobertura carregado). As 12 são países com interior grande (ARE, AGO, KGZ, BFA, GNQ, CAF, NER, COD, KEN, TZA, SOM, HTI). | Semear o nível inicial de infra por dado real (eletricidade/água) **ou** recalibrar a penalidade — decisão do dono; nenhuma isenção foi criada |
| 2 | **Comida — importadores sem agricultura** | PSX/BHR/KWT/QAT/SAH 0,000; ARE 0,121; EGY 0,002 | Bistabilidade de caixa: poupança → 0, preço do trigo no teto, consumo segue. Falta o item de modelo **tesouro/solidariedade nacional de importação** (D-065/D-071, F9 §15). | Item de modelo na Parte C (crédito/tesouro), não calibração |
| 3 | **Interior sem produção própria** | interior **6,10 bi** compra comida do polo; 139 nações < 0,975 (a D-086b media 141) | A Região (B3) tem salário primário e consumo, mas **não produz** comida própria — toda a comida do interior passa pelo mercado do polo. | Desenho: produção rural própria (B6/C) ou renda rural que financie a compra |
| 4 | **Minério no teto de 3×** | ferro `m_desired` **20.821.206,7 t/dia** vs ~17,8 mil t/dia de capacidade; petróleo/gás/carvão/ferro/bauxita em 3,000× no dia 365 | A pressão de preço usa `(desired − latent)/estoque` e a produção soma `desired` a plena capacidade **antes** de saber se há insumo/venda — a escassez satura o clamp com qualquer oferta de mina (não é efeito da semeadura D-087). | Mudar a regra de demanda desejada/pressão (contar só o que a usina consegue comprar/processar) |
| 5 | **Migração baixa** | **18.983,7** interior→polo em 365 d (interior +56,0 M no ano); urbanização **−0,05 p.p.**; cidade→cidade 37.479,5 in/out | `kMigrationGapThreshold`/`kMigrationMonthlyMax` (0,5%/mês) + vaga de moradia: o fluxo realizado é ~0,0003% do estoque do interior. | Recalibrar o gatilho/vaga **depois** de resolver a comida do interior (#1/#3) — senão o êxodo só muda a fome de lugar |
| 6 | **AUS — bauxita semeada** | AUS-BRO bauxita 0,144 e AUS-WEI 0,156 de utilização; falidas por **folha** (o resto das 15 foi corrigido na D-086b) | As duas maiores minas de bauxita operam a ~15% da capacidade por venda e pagam equipe cheia; o teto de 3× impede o preço de subir. | Depende de #4 (preço) ou de régua de folha por utilização sustentada |
| 7 | **Aluguel Q1** | AUS Q1 **0,545** (teto 0,40); Q1/Q2 sem caixa em BRA/JPN/NGA/USA | Demanda do Q1 sem renda compatível com o preço do solo (B2 §10); a B6 (patrimônio da região) muda o colateral. | Desenho B6/C1 (aluguel do anel periurbano) |
| 8 | **Bench pós-D-086b** | **39,61 ms** de dia/tick (3,93× o A0), 142,4 MB | Primeiro bench canônico depois da D-087; o critério de 3% não fecha em host compartilhado. Não há A/B por `use_seed` depois da D-087. | Re-medir em janela ociosa; A/B `use_seed` só se o custo da semente for questionado |
| 9 | **7 testes defasados** | §2.1 | A4 (resumo leve), B2 (consumo por quintil), A5 (IA C++) e locale (i18n) | Migrar os testes para `ui_snapshot`/`get_city_status`, quintis e IA C++ na próxima passada de manutenção |

## 9. Estado depois do M2

- **Save v21**, migrações v14 → v21 cobertas pelos testes (`economy_save_load_v11_compat`,
  `roundtrip`, `construction_queue`, `b3_region`, `b4_migration`, `b5_state_levers`).
- **Determinismo provado** no marco: A0 60 d 0 diferenças (2 rodadas), CLI × Godot 10 d
  byte-idênticos, sweep com hash peg repetido e sonda float diferente.
- **O que o M2 NÃO fecha:** B6 (patrimônio imobiliário da região) e B7 (informalidade,
  serviços informais, mortalidade, insatisfação) — são o **M2b**, já registrados no plano
  (`docs/plans/perf_pop_financas_plano.md` §B6/§B7) e aprovados pelo dono em 2026-11-18.
