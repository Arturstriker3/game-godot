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
- ~~Nível inicial de infra por dado real de cobertura (eletricidade/água) — geografia
  futura.~~ **Feito em 2026-11-18 (§10):** o nível inicial vem de
  `data/definitions/scenario/state_infrastructure_2026.json` (World Bank WDI/LPI).
- ~~Recalibrar a penalidade de acesso se o dado de cobertura inicial for carregado
  (hoje 0 → 30%).~~ **Decidido no §10:** mantida em 0,30 — o A/B de 365 d mostra que
  ela não governa o portão de comida (tirar o frete muda *quem* está abaixo, não
  *quantos*). O portão continua como dívida da trilha de energia/preço (M2 #10).

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

## 10. Complemento — infraestrutura INICIAL com o dado real (2026-11-18)

Branch `feat/b5-real-infra` (worktree `G:/projetos/opengs-wt-infra`, base
`balance/global-resources` `e6655af6`, já com B1–B5 + B7 + preço livre, save v22).

### O que mudou

- `data/definitions/scenario/state_infrastructure_2026.json` (tarefa de dados
  D-007) deixa de ser só dado: `core/economy/initial_infra.gd` reaplica a regra
  %→nível declarada **no próprio arquivo** (cortes lidos de `conversion_rule`,
  não duplicados), rural com fallback total na energia/água e LPI na estrada;
  **sem observação = 0** (ausência explícita, não cobertura zero). A
  granularidade do arquivo é nacional: nada é rateado nem estimado aqui.
- `GeographyService.apply_initial_infra(sim)` → `set_region_infra` (núcleo) é
  chamado **depois de `initialize_world` e antes do primeiro dia**, pelo autoload
  (`Economy._ready`) e pelos harnesses (`export_world_snapshot.gd`,
  `world_equivalence.gd`, `economy_refactor_equiv.gd`) com o MESMO arquivo.
- **Região com mais de um estado** usa o **menor nível** de cada tipo entre eles
  (pior caso — a mesma lógica rural que escolhe o indicador). Estado sem entrada
  = 0. Como o dado é nacional, hoje todos os estados de uma nação têm o mesmo
  nível; a regra só muda o resultado quando houver dado subnacional.
- **Save continua v22**: `region.infra` já existia desde o v21; nenhum campo
  novo, nenhuma migração. Save antigo carrega os níveis que foram semeados.
- Gancho de laboratório: `set_balance({"infra_access_penalty": v})` permite a
  variante (iii) do A/B sem tocar na constante `kInfraAccessPenaltyMax` (0,30).
- Teste novo: `tools/b5_initial_infra_test.gd` — regra × campo `infra_levels` do
  arquivo nos 4.226 estados, exemplos do doc (BRA/IND/NGA/DEU/CHN), `no_data` → 0,
  fallback rural→total, ponte ao núcleo com região multi-estado e persistência
  no save.

### Ponto de partida (dia 0)

Soma dos níveis no cenário (849 cidades, 827 regiões com infra):
**5.091** níveis = **1.292 estrada + 1.973 luz + 1.826 água**.

### A/B em 365 d com IA (HEAD `e6655af6`, debug, 849 cidades)

`tools/b5_infra_ab_365.gd` roda `tools/b5_world_ai_365.gd` uma vez por variante,
com o caminho do autoload (B7 incluso — informalidade/saúde) e IA ligada.
Portão = média de 30 d de consumido/necessidade por nação < 0,975
(`docs/economy/M2_marco.md`). Obras = Δ dos níveis de infra no ano (1 nível = 1
obra). Evidência: `docs/economy/perf/b5_infra_ab_365.json`.

| variante | infra inicial | nações fora do portão | mediana | média | falências | drift | obras da IA (estrada/luz/água) | concessões |
|---|---|---|---|---|---|---|---|---|
| (i) infra 0 (antes) | 0 | **147**/179 | 0,6836 | 0,6478 | 4 | +0,0013 | **1.974** (793/660/521) | 292 |
| (ii) infra real | **5.091** | **149**/179 | 0,6836 | **0,6550** | 4 | +0,0018 | **1.272** (845/199/228) | 304 |
| (iii) real, sem a penalidade de acesso | 5.091 | **148**/179 | 0,6836 | 0,6495 | 4 | +0,0016 | 1.279 (848/202/229) | 299 |

**Causa da diferença (medida, não atribuída):**

- **Obras da IA caem 35,6%** (1.974 → 1.272): com o dado real a luz já está no
  máximo em 74% da população (e a água em 27%), então a IA deixa de abrir obra
  onde o nível já é 3 — luz −70% (660 → 199), água −56% (521 → 228). A estrada
  **sobe** 6,6% (793 → 845) porque passa a ser o tipo de nível mais baixo da
  maioria (peso 0,40 na produtividade, 0,50 no êxodo): a IA prioriza a cobertura
  pior, como desenhado.
- **Comida: a mediana não muda** (0,68360 nas três variantes) e a média melhora
  0,7 p.p. com o dado real (0,6478 → 0,6550). O **portão não fecha**: 149 nações
  seguem abaixo de 0,975 (duas a mais que antes), com 4 entrando (ALB, AUS, BGR,
  EST) e 2 saindo (BRB, ECU).
- **A penalidade de acesso NÃO é a causa do portão.** Removê-la (iii) devolve
  148 nações abaixo (uma a menos que ii), a mediana continua igual e a média
  fica 0,2 p.p. acima da variante (i): mudam QUEM está abaixo (ARE 0,016 → 0,267
  e SAH 0,000 → 0,128 melhoram; DJI 0,381 → 0,022, SAU 0,035 → 0,007 e MRT
  0,011 → 0,000 pioram), não QUANTOS. São mercados finos/bistáveis (necessidade
  pequena, trigo no teto, caixa colando em zero) — a mesma dívida do M2 #2/#10.
  O nível absoluto do portão neste HEAD já carrega o efeito documentado do preço
  livre (D-091/D-092, 31 nações > 2,5 p.p. no M2 #10), então a comparação válida
  é interna (i × ii × iii), não contra o número do M2.
- Falências (4, teto 28) e drift (≤ 0,0018 em ~6,5 bi = ruído de serialização)
  não mudam; **nenhum invariante violado** nos três mundos.

> **Correção do contador (2026-11-19).** O JSON deste A/B (HEAD `e6655af6`) foi gerado
> quando o `b5_world_ai_365.gd` somava com `++violation_count`/`++stalled_days`; em
> GDScript 4.7.2 `++x` não incrementa, então `invariant_violations`/`stalled_days` eram 0
> por construção. O harness já usa `+= 1` desde o B6 (`b3bd829c`); reexecutado no HEAD
> `1069b879`, o A/B dá **0 violações e 0 dias travados nas três variantes** — a conclusão
> se mantém, agora com contador real. (O JSON não foi regerado para não trocar os demais
> números da tabela por drift de balance posterior.)

**Decisão:** o dado real entra como estado inicial — é o dado, e elimina **702**
obras de infra que a IA abria sobre cobertura que já existia. A penalidade de
acesso **fica em 0,30**: o A/B mostra que ela não governa o portão de comida, e
recalibrá-la só mudaria o sinal de mercados finos. O portão continua aberto como
dívida da trilha de energia/preço (M2 #10), não da B5.

### Verificação

- Bateria **10/10 verde**, um por vez: `b5_state_levers`, `b7_informality`,
  `b4_migration`, `b3_region`, `mine_stage`, `services` (cena),
  `economy_integration`, `economy_ai`, `economy_save_load_roundtrip`,
  `price_formation`; mais o teste novo `b5_initial_infra_test` (4.226 estados).
- **Goldens regerados em 2 rodadas (0 diferenças):** A0 `A0_60_urban`
  save `8004f7e7…` → `71e60ec0…`, status `6243eb1c…` → `3cf931e6…`
  (money 6.523.551.982,900011 → ,900002; 41.646 → 41.616 negócios de serviço);
  S0 `real_60_ai` `288ab187…/18b33422…` → `6a9fba92…/cdb1cc5a…`,
  `real_60_noai` `1912d9cd…/1109900e…` → `6bfa2e76…/05a65d9c…`
  (43.024 negócios); `demo_365_noai` **inalterado**
  (`fe278e79…/9742d3e8…`) — o cenário sintético AGRIA/PORTUS não tem regiões,
  então nada mudou por lá.
- **CLI × Godot 10 d** (`--ai=1`, sem urbano): **byte-idêntico**,
  73.724.692 bytes, SHA-256 `4FC8E18C…`.
