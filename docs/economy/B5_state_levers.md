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

A IA de **concessão** do governo fica **pendente** nesta entrega (ver §8).

## 7. Save v21

- `kSaveVersion = 21`. Campos novos: `region.infra` (`road`/`power`/`water`) e
  `building.royalty`; `ConstructionProject.infra_type`/`kind` (obra de infra).
- **Migração v20→v21**: os blocos ausentes nascem com **nível 0** e **royalty 0** — nada é
  inventado para trás; o save v20 carrega e vira v21 sem quebrar `status.mode`/chaves antigas.

## 8. Pendências desta entrega

- IA do governo **não usa** `concede`/`privatize`/`estatize` ainda (heurística declarada, ver §6,
  item 2 do plano); o comando, o royalty e o status estão prontos e testados.
- Nível inicial de infra por dado real de cobertura (eletricidade/água) — geografia futura.
- Goldens A0/S0 e bench 3×60 d **não regerados** (B5 muda resultado; a bateria que roda está verde).
