# Início do jogo em 2027-01-01 (D-084)

Decisão do dono: o calendário do jogo passa a começar em **1º de janeiro de
2027**. Branch `feat/start-2027`, worktree `opengs-wt-2027`, base = merge da B2
(`2cc156e`) em `balance/global-resources`.

## 1. Onde a data inicial vivia

Varredura em `gdextension/src`, `core`, `game`, `data/definitions` e `tools`:

| lugar | o que era | destino |
| --- | --- | --- |
| `core/economy/economy.gd` `const START_UNIX` | `1767225600` = 2026-01-01T00:00:00Z — **único ponto** que define a data | → `1798761600` (2027-01-01T00:00:00Z) |
| `core/economy/economy.gd` `game_date()` | `Time.get_datetime_dict_from_unix_time(START_UNIX + dia * 86400)`; único consumidor da constante | inalterado (segue a constante) |
| `core/ui/hud.gd` (rótulo da data) | `"%d %s %d" % [dia, mês, ano]` a partir de `Economy.game_date()` | inalterado (passa a mostrar 2027) |
| `core/ui/hud.gd` (`_sample_history`) | `month_key = "AAAA-MM"` do histórico de 12 meses, de `Economy.game_date()` | inalterado (passa a gerar 2027-…) |
| `core/stats/history_service.gd` | consome `month_key`; não calcula data | inalterado |
| `tools/history_service_test.gd` | calendário sintético do teste com 2026 fixo (`_month_key`, `_last_day_of`) | → 2027 (coerência com o jogo) |
| `gdextension/src` (núcleo C++ / CLI) | **não tem calendário**: nenhuma ocorrência de `year`/`date`; save e status carregam só `day` | nada a mudar |
| relógio do sistema | nenhuma leitura de `Time.get_*_from_system` em `core/` ou `game/` | — |
| `data/ui/templates/quadro.json` (galeria T5) | "jan 2025"/"jan 2026" **literais de demonstração** (`@tpl.quadro.month.start/end`) | deixado como está: não vem do calendário e mexer invalidaria as capturas da galeria |
| `data/definitions/**` | citações de fonte (aglomerações 2026-01-01, estrutura etária 2026) | são dado/fonte, não data de início |

## 2. O que não depende do ano (conferido)

- **Cadência A6 (D-080).** Os períodos são contados em **dias de simulação**:
  `day % período` e `(dia − índice) % período`, com `month_marker` medindo os
  dias decorridos da janela (`sim/city_gov.cpp`, `sim/housing.cpp`,
  `sim/urban_land.cpp`, `sim/services.cpp`, `sim/ai/*`). Não há dia da semana
  nem mês civil no núcleo, então a troca de ano não muda a cadência.
- **Save.** Guarda `day` (inteiro); o dia 1 passa a ser 2027-01-01. Saves v11…v17
  carregam sem migração. O `month_key` já gravado no histórico da HUD (até 12
  meses) mantém o ano antigo até sair pela janela de 12 meses — é rótulo de
  série, não estado do mundo.
- **Goldens.** O hash de save/status vem do núcleo C++, que não conhece data.

## 3. Base de dados (não reprojetada — declarado)

A base é **2025/2026** e **não foi reprojetada** para 2027:

- `data/definitions/population/age_structure_2026.json` — UN WPP2024, médio 2026;
- `tools/agglomerations_2026.csv` — citypopulation.de (T. Brinkhoff), referência 2026-01-01;
- população/economia de cenário vindas da **extração 2025** (ver `docs/plans/world_all_data.md`).

O calendário do jogo começa em 2027 com a base de 2025/2026. **A projeção da
base para 2027 é item de calibração em aberto**, não um erro silencioso: o mundo
do dia 1 é o dado de 2025/2026 rotulado como 2027.

## 4. Evidências

- **A0** (60 d, IA + urbano): `save 84d28700…` / `status 5f91d311…` — iguais ao
  golden vigente; 2 rodadas 0 diffs e rodada 1 × baseline 0 diffs.
- **S0**: `real_60_ai d64df2a1…/fc560221…`, `real_60_noai 7070ff67…/bb88d8bb…`,
  `demo_365_noai a9dbd376…/ab317791…` — 0 diffs cada. **Nenhum golden foi
  regerado** (o hash não mudou, como esperado).
- **CLI × Godot, 10 d com IA, threads = 1**, a partir do snapshot do dia 0
  (`export_world_snapshot --ai=1`, 834 cidades, 59.165.845 B): replay do Godot e
  `opengs_sim_cli` geram arquivos **byte-idênticos** (`BE66BBEB…`), comparador em
  tolerância 0 → **0 diferenças**, `money_total` 2.177.839.209,15, 0 violação de
  invariante.
- **Testes verdes:** `history_service_test` (série mensal 2027-03..2028-02),
  `economy_integration_test` (schema 17), `population_demo_test`,
  `economy_ai_test`, `population_age_test`, `strata_test`,
  `economy_save_load_roundtrip_test` (0 diffs).

## 5. Pendências

- **Calibração:** projetar a base 2025/2026 para 2027 (população, estrutura
  etária, aglomerações) quando o dono definir a regra.
