# B5 — Infraestrutura do estado + concessões de mineração/terra

> Decisão **D-089** (registrada em `GAME_STATE.md` §14). Fase §B5 do
> `docs/plans/perf_pop_financas_plano.md` (aprovada pelo dono, 2026-11-17).
> Branch `feat/b5-state-levers`, base `b9ebf43` (B1–B4 + minas + save v20).

## 1. O que entra

Dois blocos por cidade, sobre a **Região** da B3 (estados inteiros):

1. **Infraestrutura do estado** — três níveis por região: **estrada** (`infra_road`),
   **energia/luz** (`infra_power`) e **água** (`infra_water`), cada um `0..kInfraMaxLevel` (= 3).
2. **Concessões de mineração e terra (F9)** — uma mina/fazenda da região pode ser
   **estatal**, **concedida a grupo nacional** ou **estrangeiro**, com **royalty** sobre a produção.

Sem UI/painel nesta fase (decisão do dono): o núcleo, a fila A3 e o status/A4 só.

## 2. Parâmetros (modelo declarado)

Nenhum número por país é inventado. O nível **inicial** de cada infra usa dado real
com fonte quando existir no repositório (geografia/cobertura) — **ainda não há dado de
cobertura carregado nesta fase**, então o nível inicial é **0 (ausência explícita)**. Os
efeitos abaixo são **modelo declarado**, constantes nomeadas em `gdextension/src/sim/sim_constants.h`:

| Constante | Valor | Significado |
|---|---|---|
| `kInfraMaxLevel` | 3 | Nível máximo de cada infra |
| `kInfraTypeCount` | 3 | Tipos: 0 estrada, 1 energia, 2 água |
| `kInfraAccessPenaltyMax` | 0,30 | Penalidade máx. de frete/overhead com estrada 0 (cai linear a 0 no máx.) |
| `kInfraProductivityWeight` | {0,40; 0,35; 0,25} | Peso de cada tipo na produtividade do trabalho primário |
| `kInfraProductivityGain` | 0,30 | Ganho máx. de produtividade (fração sobre a base) no nível máximo |
| `kInfraExodusBoostMax` | 0,50 | Fator máx. de expulsão no êxodo B4 com infra 0 (cai a 1,0 no máx.) |
| `kInfraExodusWeight` | {0,50; 0,30; 0,20} | Peso de cada tipo na cobertura combinada do êxodo |
| `kInfraCostPerCapitaPerLevel` | 0,01 | Custo da obra (XGS) por nível, por habitante da região |
| `kInfraBuildDays` | 90 | Prazo da obra de infra (projeto físico §15) |
| `kInfraMaintenancePerCapitaPerLevelPerDay` | 5e-7 | Manutenção (XGS/dia) por nível, por habitante |
| `kConcessionRoyaltyRate` | 0,05 | Royalty/imposto padrão de concessão (fração da receita) |

## 3. Efeitos (item 1)

Os multiplicadores são **derivados** (recalculados a cada uso, não persistidos):

- **Custo de acesso** `infra_access_multiplier = 1 + kInfraAccessPenaltyMax × (1 − estrada/máx)`.
  Aplica ao preço efetivo que a região paga ao comprar comida do polo; a firma recebe o
  preço do mercado e a diferença (frete) vai a `transport_cash` — **dinheiro conservado**.
  Não há preço separado por estado nem mercado imobiliário rural.
- **Produtividade** `infra_productivity_multiplier = 1 + kInfraProductivityGain × Σ(weight × nível/máx)`.
  Escala o salário semanal do setor primário da região (pago do MESMO caixa da firma — conservado).
- **Êxodo** `infra_exodus_multiplier = 1 + kInfraExodusBoostMax × (1 − Σ(weight × nível/máx))`.
  Multiplica o fluxo de êxodo rural da B4 (região sem infra expulsa mais gente; pessoas conservadas).

## 4. Obra e manutenção (item 1)

- **Comando** `infra_build(city_id, infra_type)` → fila A3 (`CommandType::InfraBuild`),
  ação `action_infra_build`: valida tipo, nível < máx, **anti-duplicação** (uma obra do tipo
  em voo) e caixa (limite de comprometimento `committed_construction`). Custo
  `kInfraCostPerCapitaPerLevel × region_pop`, pago pelo **governo nacional** (a cota equalizada
  do tesouro vive em `city.government_budget` — a prefeitura não paga).
- **Obra** vira projeto físico §15 (`kind = CONSTRUCTION_REGION_INFRA`, `infra_type`), prazo
  `kInfraBuildDays`; ao concluir, `SimCore::infra_level_up` sobe UM nível (saturado em `kInfraMaxLevel`).
- **Manutenção** diária `region.infra_maintenance = kInfraMaintenancePerCapitaPerLevelPerDay ×
  nível_total × region_pop`, debitada do caixa do governo e paga ao setor de serviços — conservado.

## 5. Concessões (item 2)

- **Modelo F9 reaproveitado** (`Group`, `owner` = `state` | `national:<g>` | `foreign:<g>@<n>`);
  nada de sistema de donos novo.
- **Comando** `concede(city_id, building_id, group_id, royalty_rate)` →
  `CommandType::Concede` / `apply_concede`: só para mina/fazenda (`recipe < 0 && input_product < 0`)
  estatal; transfere ao grupo (nacional/estrangeiro, respeitando a política de IDE), o
  comprador paga o valor contábil (`capacity × capital_per_capacity`) ao governo e
  `building.royalty` recebe a taxa (parâmetro > 0, senão `kConcessionRoyaltyRate`).
  **Transferência de dinheiro conservada** (comprador → governo).
- **Efeito de renda**: na fase de contabilidade, para construção concedida (`royalty > 0`,
  dono != state), `royalty_paid = min(receita × royalty, lucro)` é debitado do lucro do
  concessionário e creditado ao governo (`building.royalty_paid`, derivado). Dinheiro conservado.
- **Status/A4** `get_region_status(id)` ganha o bloco `concessions` (id, resource,
  output_product, owner, royalty, royalty_paid) por mina/fazenda da região.

## 6. IA do governo (item 1)

Heurística **determinística** (sem RNG), no fim de `plan_city` (depois de comida e mineração):

- **Gatilho**: déficit alimentar da **região** (`region_food_deficit ≥ kMinShortageTons`) OU
  êxodo já observado (`migration_rural_in_total > 0`).
- **Prioridade**: o tipo de **nível mais baixo** (cobertura pior primeiro); empate na ordem
  estrada → energia → água. Custo limitado a `kBudgetShare` (25%) do orçamento nacional;
  uma obra por cidade a cada `kBuildCooldownDays` (30 d).

## 6.1 IA de concessão (item 2)

O governo **concede** mina estatal ociosa a grupo **privado nacional** (heurística
determinística, constante `kConcessionFloorStageFactor` = 0,10): alvo = mina do Estado com
`stage_factor <= 0,10` (esgotada, ~o piso da D-086); comprador = grupo `!state_owned` da mesma
nação com caixa ≥ valor contábil; **nunca estrangeiro** (a política de IDE nunca bloqueia); o
preço é o valor contábil (`capacity × capital_per_capacity` — nunca abaixo). Uma concessão por
cidade por cooldown. 365 d: **346 concessões** (o comando, o royalty e o status já existiam).

## 7. Save v21

- `kSaveVersion = 21`. Campos novos: `region.infra` (`road`/`power`/`water`) e
  `building.royalty`; `ConstructionProject.infra_type`/`kind` (obra de infra).
- **Migração v20→v21**: os blocos ausentes nascem com **nível 0** e **royalty 0** — nada é
  inventado para trás; o save v20 carrega e vira v21 sem quebrar `status.mode`/chaves antigas.

## 8. Resultados e pendências

**Medido (b96f28b):**
- **365 d com IA**: falências **4** (≤ 28), drift **~0** (−0,002, ruído de serialização), infra
  **1813** obras (754 estrada / 619 luz / 440 água em 693 regiões), **346 concessões** da IA.
- **Comida**: mínimo por cidade = AFG **0,0** (`pop.satisfaction`, medida crua). É **pré-existente**:
  a base `b9ebf43` já tinha AFG fora do portão (`docs/economy/perf/b4_world_sweep.json`:
  `food_avg30` 0,575, importador sem agricultura — ver `GAME_STATE.md` §15). A penalidade de
  acesso (30% com infra 0) incide sobre a comida da **região** (frete/overhead), não sobre o
  `pop.satisfaction` urbano medido; o nível inicial de infra é 0 (ausência explícita, sem dado
  de cobertura carregado — não se inventa dado por país).
- **Goldens A0/S0 regerados** (2 rodadas idênticas):
  | golden | save_sha256 antes → depois |
  |---|---|
  | `A0_60_urban.json` | `8759c538…` → `bdd355b3…` |
  | `real_60_ai.json` | `2795407e…` → `9e5dab44…` |
  | `real_60_noai.json` | `39164a11…` → `0ab8f317…` |
  | `demo_365_noai.json` | `d189fe63…` → `98bbfde8…` (status `14d2c387…` inalterado) |
  Mudaram **por desenho** (B5 muda resultado; o demo AGRIA/PORTUS só muda o save, não o status).
- **CLI×Godot 10 d**: `save` **byte-idêntico** (33.522.730 bytes, `--ai=1`, sem urbano).
- **Bench 3×60 d** (CLI release, snapshot real): 21,74 / 22,57 / 21,88 dias/s (média **22,06**,
  variação **±1,9%**), `ms_dia` 44,3–46,0; `docs/economy/perf/b5_bench.json`.

**Pendências:**
- Nível inicial de infra por dado real de cobertura (eletricidade/água) — geografia futura.
- Recalibrar a penalidade de acesso se o dado de cobertura inicial for carregado (hoje 0 → 30%).

## 9. Correções da revisão de entrega (D1–D3)

- **D1 — concessão residual em estatizar/privatizar (corrigido):** `apply_nationalize` e
  `apply_privatize` não zeravam `building.royalty`/`royalty_paid`; a sequência
  conceder → estatizar → privatizar deixava 5% de royalty e o ativo seguia publicado como
  concessão. Agora ambos limpam o royalty e o estado de concessão (ativo estatal ou de
  propriedade plena não paga royalty). Teste no `b5_state_levers_test.gd` (§3) cobre a ida e a
  volta, com conservação de dinheiro.
- **D2 — regressão do teto de royalty pelo caixa da firma:** `sim_core.cpp` limita o royalty
  efetivo por `receita × taxa`, pelo lucro e pelo **caixa** da firma. O teste monta o cenário
  determinístico que expõe o bug antigo: mina estatal concedida cuja receita vem da
  transferência interna de insumo (planta de aço consome o minério do estoque — a receita
  atribuída sobe sem entrar caixa) e caixa da firma = 0. Com o código antigo a invariante
  diária acusa "dinheiro não conservado" (drift 16.999.587 no dia 1) e o teste falha; com o
  teto, violações = 0 e o royalty pago fica no que o caixa permite.
- **D3 — mensagens "v20" obsoletas:** os 7 testes cujas asserções já eram v21
  (`b3_region`, `construction_queue`, `economy_save_load_v11_compat`, `mine_stage`, `services`,
  `urban_land`, `urban_load`) passaram a dizer v21 onde a mensagem é a versão atual; o OK final
  do `b4_migration` também. As referências a "v19→v20"/"v20→v21" como **etapa** de migração
  ficam.
- **Verificação:** bateria verde um por vez (b5, b4_migration, b3_region, mine_stage, services
  pela cena, economy_integration, economy_ai, roundtrip, v11_compat, construction_queue,
  urban_load, urban_land, city_gov, population_age, strata, history_service); golden
  `A0_60_urban` **inalterado** (2 rodadas, 0 diferenças); CLI×Godot 10 d `--ai=1`
  **byte-idêntico** (74.052.047 bytes, SHA-256 `F31BEAA4…`).

