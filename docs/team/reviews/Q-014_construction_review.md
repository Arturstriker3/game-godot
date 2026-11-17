# Q-014 — Revisão FINAL da construção física (§15) + S17

**Data:** 2026-11-17
**Revisora:** gerente-qualidade (DeepSeek V4 Pro)
**Escopo revisado:** `3108bf4` (S17, cadência semanal de `nations[].labor`), `e3d2d48`
(construção como projeto físico), `858049a` (testes), `d5d9c4f` (goldens S0 + docs),
`a3eefb4` (calibração D-062: carência, duração por tipo, reserva de capital),
`a270a33` (IA enxerga obras; algodão/madeira fora da decisão), `2100a5a` (registro D-062).
**Decisões do dono consideradas:** a piora de comida em importadores sem agricultura
(SOL/DJI/SAH) é **aceita** (plano §18; ajuda externa ONU/FMI depois). Este parecer **não
reprova por isso** — registra os números.

**Veredito: APROVADO COM RESSALVA.** Núcleo §15 correto e determinístico; 3 ressalvas
obrigatórias antes do fechamento da fase (R1–R3) e 2 de documentação (R4–R5).

---

## 1. Ambiente

| Item | Valor |
| --- | --- |
| Godot | 4.7.2-stable (`ed1daf0bf`), Windows x86_64 |
| DLL | debug real no slot `gdextension/bin/…template_debug…dll`, 20.868.608 B, SHA-256 `574DA1F5B1061B045FBFD78EC6C4FDC4F5AAE418023A3EEC5DF4860969044107` (= `.real_debug`) |
| Build | debug (headless carrega o slot debug); C++ mais novo que a DLL (00:07:56 × DLL 00:08:05) |
| Modo | `--headless`; threads=1 e 8 (goldens), default (demais) |
| Base | HEAD `2100a5a`; worktree com artefatos do usuário fora do commit |

## 2. Critério × evidência

### 2.1 Goldens S0 regenerados (justificativa: a simulação muda de propósito, §15)

A calibração §15/D-062 mudou o cronograma de obra, a reserva de capital e a tabela
`c_*` do bloco `light` — portanto os 3 goldens **tinham de ser regenerados**. Não é
"golden ajustado para passar": o demo também mudou (o `status_sha256` do demo
`c224caf0…` → `65a414f0…`), o que **contraria a afirmação de D-060** de que só o mundo real
mudaria. O demo de 365 d tem obras (fábricas de referência) e a nova carência/duração
altera o resultado; a mudança é consistente com §15, mas deve ser declarada.

| Golden | save_sha256 (novo) | status_sha256 (novo) | t1 == t8 |
| --- | --- | --- | --- |
| `real_60_ai` | `d2fe7bf34309abd241e400429ed6c58202cb720c9a90574957880b4792942fc6` | `4d4e9ebeb6442ef38fb964f34b13abb84542c23bfbe70604e0df2c82335bb470` | sim |
| `real_60_noai` | `3caf713a1c01a1283a61ccfc0c2153511c2a7133186f2cc524a60ae3ef2bf8d1` | `abde91538c3bd5aadcd2d9bc1da411a9c66e02bf1562f39cbcb39bfa3c5c1af9` | sim |
| `demo_365_noai` | `7f5c68eec2fd0b1bf2687c5187e9ad53aa66428f87665d8640e152b12b5d1d67` | `65a414f0de49014f6dccce39bd89d5a99bc7cf792c0dbc26fe6407fd02b46268` | sim |

- Antigos (HEAD): `real_60_ai` `68437f8f…`/`9a1c1d9f…`; `real_60_noai` `02c3f09e…`/`a3ccdbaa…`;
  `demo_365_noai` `346d8a3a…`/`c224caf0…`.
- **Determinismo:** t1 == t8 nos 3; **10× threads=8** no `real_60_ai` = **1 hash único**
  (`d2fe7bf3…`, 10/10). `verificador-flash` reproduziu t1 e t8 independentemente
  (arquivos byte-idênticos).

### 2.2 Testes

| Teste | Resultado |
| --- | --- |
| `economy_integration_test` | verde |
| `economy_ai_test` | verde |
| `population_demo_test` | verde |
| `economy_tick_async_test` | verde (sync 3168 ms / async 3977 ms; 1 tick por vez) |
| `production_chain_test` | verde |
| `investment_test` | verde (conservação 1.650.000 → 1.650.000 em 40 d) |
| `labor_market_test` | verde |
| `pop_needs_test` | verde |
| `construction_project_test` | verde (**material atrasa, capital reservado não pausa, conclusão opera, dinheiro conservado**) |
| `services_test` (cena, autoload) | verde (save v12 e migração v9) |
| `economy_save_load_roundtrip_test --mode=world --days=60 --save-at=30` | **0 diffs** |
| `economy_save_load_roundtrip_test --mode=demo --days=120 --save-at=60` | **0 diffs** |
| `economy_world_test --days=60 --ai=1` | verde (2 cidades, 20 t/dia, drift ~0) |
| `economy_world_test --days=60` (sem IA) | **VERMELHO** — 0 cidades, 0 t/dia (ver R1) |

"Bateria 8/8" = os 8 módulos acima + `services_test` via cena = **9/9 verdes**;
`construction_project_test` também verde.

### 2.3 Sweep 365 d ×5 (novo baseline)

Comando:

```powershell
& $godot --headless --path . --script res://tools/economy_world_sweep.gd -- `
  --days=365 --baseline=res://docs/economy/perf/services_fix_world_sweep_v2.json `
  --out=res://docs/economy/perf/construction_world_sweep.json
```

Resultado salvo em `docs/economy/perf/construction_world_sweep.json` (arquivo é
**baseline medido**, não sweep verde: o comando sai **1** por desenho, pelas nações de comida).

- **Falências de construção:** **6** em cada uma das 5 variantes (`real`, `real_seed1`,
  `real_seed2`, `half`, `xgs`) — limite calculado pelo script **16** (v2: 14 + tolerância 2);
  teto do critério do dono **≤28**. **Fecha** (6 ≤ 28).
- **Dinheiro conservado:** `money_total` final **2.200.539.587,39894** vs init
  **2.200.539.587,400009** → **−0,0011 XGS** (tolerância do teste 0,01). Fecha.
  Confirmado por corrida independente (`drift=-0.0011`).
- **Serviços (informativo):** 52.808 negócios / 6.910 falências (v2: 48.077 / 13.495) —
  a demanda de construção aquece o setor e reduz a falência em ~48%; registrar como efeito
  colateral positivo, não como critério.
- **Comida:** o script compara contra o **`worst_food` do v2** (normalização de array em
  `economy_world_sweep.gd:196-212`), não contra o f9. Por isso a tabela abaixo traz as duas
  colunas — vs v2 (critério do script) e vs f9 (baseline pré-conserto).

| Nação | f9 | v2 (worst) | Q-014 | Δ vs v2 | Δ vs f9 | Classe |
| --- | --- | --- | --- | --- | --- | --- |
| SOL | 0,9734 | 0,9863 | **0,5528** | −43,3 p.p. | **−42,1 p.p.** | importador aceito |
| JOR | 1,0000 | 1,0000 | **0,7707** | −22,9 p.p. | **−22,9 p.p.** | **não declarada** |
| YEM | 1,0000 | 1,0000 | **0,9100** | −9,0 p.p. | **−9,0 p.p.** | **não declarada** |
| SGP | 1,0000 | 1,0000 | **0,9138** | −8,6 p.p. | **−8,6 p.p.** | **não declarada** |
| DJI | 0,4717 | 0,3948 | **0,3088** | −8,6 p.p. | **−16,3 p.p.** | importador aceito |
| LBY | 0,9302 | 1,0000 | 0,9216 | −7,8 p.p. | **−0,9 p.p.** | dentro vs f9 |
| MRT | 0,9516 | 1,0000 | 0,9532 | −4,7 p.p. | **+0,2 p.p.** | melhor vs f9 |
| SAH | 0,2675 | 0,2207 | 0,2167 | −0,4 p.p. | **−5,1 p.p.** | importador aceito |
| ESP | 0,9961 | 0,9965 | 0,9844 | −1,2 p.p. | −1,2 p.p. | dentro |
| PSX | 0,1665 | 0,1832 | 0,1792 | −0,4 p.p. | +1,3 p.p. | dentro |

**Leitura:** vs o baseline pré-conserto (f9), **6 nações** pioram >2,5 p.p.: SOL, DJI, SAH
(aceitas pelo dono) **e JOR, YEM, SGP (não declaradas)**. LBY e MRT só aparecem abaixo da
tolerância quando comparadas ao `worst_food` do v2 — contra o f9 estão dentro. JOR (−22,9)
é a maior regressão nova em nação que **não** estava na lista aceita; YEM/SGP são
importadores/entrepostos (Singapore sem agricultura). Confirmar a classe é decisão do dono
(R3).

## 3. Achados

### A1 (R1) — Plantas de referência do bootstrap viram obra de 90 d; gate F2 sem IA fica vermelho

`demo_seed_industry()` (economy_sim.cpp:1161) semeia moinho+padaria de referência via
`demo_build` (:3206/:3224) e as cadeias finais (:3290/:3305). Com §15, `demo_build` cria
**projeto** (`instant_construction_ == false`). Medido em 60 d sem IA (dump do save v12):

- 12 projetos `flour`/`bread`, todos `strategic=true`, `duration_days` 85–90,
  `days_elapsed=60`, `paused=false`, `pause_reason=""` → **não é falta de material nem de
  dinheiro: é o prazo de 90 d**. Nenhum concluiu em 60 d.
- Resultado: `[world] industrializacao: 0 cidades > 0 | 0,0 t/dia` e exit **1**.
- Pré-§15 (logs do próprio repo): `f8_world_ai_60.log` = **8 cidades / 396 t/dia**;
  `f8_world_60_noai.log` = **6 cidades / 216 t/dia**, ambos `[world] OK`.
- Com IA em 60 d: 2 cidades / 20 t/dia (exit 0). **Em 365 d com IA: 8 cidades / 463,6 t/dia**
  — não há colapso de longo prazo; o problema é o horizonte de 60 d e o fato de a
  "indústria inicial" ser tratada como obra nova.

**Por que importa:** as plantas semeadas representam **estoque industrial pré-existente**
do cenário, não obra nova; colocá-las em obra de 90 d quebra a guarda F2
("industrialização nunca fica em 0") e faz o mundo ficar sem indústria por um trimestre.
**Correção proposta (não implementada):** semear as plantas de referência como prédio
**concluído** (custo debitado na criação, sem `DemoConstructionProject`), mantendo a
construção física só para obra/expansão nova. Alternativa aceitável: `instant_construction`
no bootstrap (`demo_seed_industry` apenas). Não alargar o teste de 60 d — isso esconderia a
causa.

### A2 (R2) — Algodão/madeira fora da IA: **é muleta** — cotton/wood/fabric/cellulose/paper ficam não produzíveis

Depois de `a270a33`, **nenhum** agente abre `cotton`/`wood`:

- `private_investor.gd:375-381`: antes era `LAND_COMPETING && not food_secure → continue`;
  agora `continue` **incondicional**.
- `group_investor.gd:562-563`: `return` **incondicional** (a checagem de aptidão em :589 é
  código morto, já saiu em :563).
- `government_ai.gd` só varre `FOOD_RESOURCES` (comida) e `MINERAL_RESOURCES` (mina);
  não abre lavoura industrial.
- `data/definitions/scenario/buildings.json`: **0** prédios de cotton/wood (834 no total);
  nenhuma receita produz cotton/wood; `fabric` ← `cotton` (único insumo);
  `cellulose_a` ← `wood`; `paper` ← `cellulose`; `sawdust` só é subproduto de `planks`
  (que também depende de `wood`).

**Efeito:** a partir do dia 0, `cotton`, `wood` (e por consequência `fabric`, `cellulose`,
`paper`) têm **oferta estruturalmente zero**. O bootstrap continua semeando
`SEED-fabric`/`SEED-clothes`/`SEED-cellulose`/`SEED-paper` (economy_sim.cpp:3344-3348) —
fábricas que nunca podem operar sem insumo — e `needs.json`/`services.json` continuam
demandando papel. Isso explica a industrialização quase nula em 60 d (2 cidades/20 t/dia;
`max 0.0004`) e o comentário agora **falso** em economy_sim.cpp:3277-3280 ("a IA constrói
as fazendas de algodão/madeira quando o shortage aparece").

**Por que é muleta:** a exclusão não resolve a causa (demanda/exportação de matéria-prima
inexistente); remove o **único** fornecedor e transforma 5 bens em não produzíveis. O ganho
de "falências 179→6" é parcialmente obtido por **não construir** (builds 1.955→781), não
por viabilidade econômica.

**Correção proposta (não implementada), em ordem de custo:**
1. **Demanda derivada, não "shortage latente":** dimensionar a escassez de cotton/wood pela
   capacidade a jusante existente/em obra (`fabric`/`cellulose` × insumo da receita) e não
   pelo mercado do próprio bem; o gate de margem com aptidão já existe — manter a exclusão
   só quando a demanda derivada for 0.
2. **Semear a fazenda bruta junto da fábrica:** o bootstrap semeia só a fábrica
   (economy_sim.cpp:3277). Semear a fazenda de cotton/wood strategic na cidade de maior
   aptidão quando a fábrica é semeada (como o moinho de farinha) dá insumo à cadeia.
3. **Demanda externa real:** dar a cotton/wood (ou a fabric/paper) uma demanda de
   exportação no modelo; o preço deixa de cair ao piso e a fazenda vira viável por margem,
   não por exceção.
4. Até F3, se a exclusão ficar: **não semear** as fábricas que não podem operar (evita
   prejuízo estratégico subsidiado) e declarar explicitamente que fabric/paper estão
   desligados no mundo.

### A3 (R3) — Comida: 7 nações fora da tolerância vs v2; 3 delas não estavam na decisão do dono

Ver a tabela da §2.3. Aceitas: SOL/DJI/SAH. **Novas:** JOR (−22,9 p.p. vs f9), YEM
(−9,0), SGP (−8,6). Todas são importadoras de alimento no modelo, mas JOR tem
`share_agri` 0,409 — não é "sem agricultura" pela métrica de emprego. Pedir confirmação
explícita (dono/@gerente-simulacao) de que essas 3 entram na mesma classe aceita, com
número; se não entrarem, é regressão a tratar. LBY/MRT: dentro da tolerância contra o f9
(o script os penaliza por comparar com o `worst_food` do v2).

### A4 (R4) — Documentação: D-062 colado em D-060 quebra a tabela §14

`GAME_STATE.md:999` tem `… | || D-062 | …` na **mesma linha** de D-060: D-062 não é linha
própria da tabela. O commit `2100a5a` ("registra D-062") não se sustenta na forma do
arquivo. Corrigir na próxima edição do `GAME_STATE.md` (dono: @gerente-simulacao).

### A5 (R5) — Golden do demo mudou; D-060 e D-062 precisam dizer isso

`demo_365_noai` mudou de status (`c224caf0…` → `65a414f0…`) e de save (`346d8a3a…` →
`7f5c68ee…`). A justificativa "demo intacto" de D-060 não vale mais para §15 calibrado.
Não é erro (o demo tem obra/fábrica de referência), mas o `GAME_STATE` deve registrar.

## 4. Observações não bloqueantes

- **S17 (`3108bf4`)** mantém o contrato estrito: `nations[].labor` é exibição (IA lê
  `light`/`market_totals`), publicação semanal cacheada; os goldens novos embutem a mudança.
  Nenhuma tela/ferramenta depende de `labor` diário (grep no escopo do Q-012). Verde.
- **Perf** não foi re-medido nesta revisão (a DLL debug é a de teste; a fase S17 já tem
  perfil commitado e a pendência de máquina ociosa do Q-012 permanece). Não há regressão
  medida de perf neste parecer — "não medido" por falta de build release no slot.
- **Dinheiro:** `money_total` no `meta` do golden varia ~3e-6 entre execuções (não entra no
  hash; soma fora do contrato). Drift real medido em 365 d: −0,0011 XGS.

## 5. Veredito e condições

**APROVADO COM RESSALVA.** O núcleo §15 (projeto físico, carência, reserva de capital,
tabela `c_*`, save v12 aditivo) está **correto, determinístico e conserva dinheiro**;
`construction_project_test` e round-trip (real+demo) verdes; bateria 9/9; sweep 365 d ×5
com 6 falências de construção (≤28).

**Ressalvas obrigatórias antes de fechar a fase:**
1. **R1** — semear a indústria de referência como prédio concluído (ou `instant` no
   bootstrap) e devolver `economy_world_test --days=60` (sem IA) ao verde; não alargar o
   teste.
2. **R2** — trocar a exclusão incondicional de cotton/wood por gate de demanda derivada +
   fazenda bruta semeada (ou desligar explicitamente fabric/paper até F3); remover o
   comentário falso de economy_sim.cpp:3277-3280.
3. **R3** — confirmar JOR/YEM/SGP na classe de comida aceita (com número) ou tratar como
   regressão.
4. **R4** — corrigir a linha D-062 no `GAME_STATE.md` §14.
5. **R5** — registrar que o golden do demo mudou (§15 calibrado), retificando D-060.

Nenhuma ressalva é veto: o jogo roda com IA e em 365 d a capacidade industrial medida é
463,6 t/dia; R1/R2 são correções de escopo/modelo com caminho curto, e R3 é decisão do dono.

## 6. Reprodução

```powershell
$godot = "C:\Users\artur\OneDrive\Área de Trabalho\Godot_v4.7.2-stable_win64.exe"

# goldens (t1 e t8; 10x para determinismo)
& $godot --headless --path . --script res://tools/economy_refactor_equiv.gd -- `
  --mode=world --days=60 --ai=1 --threads=1 --out=docs/economy/refactor/baseline/real_60_ai.json
& $godot --headless --path . --script res://tools/economy_refactor_equiv.gd -- `
  --mode=demo --days=365 --ai=0 --threads=8 --out=<tmp>.json

# bateria
& $godot --headless --path . --script res://tools/economy_integration_test.gd
& $godot --headless --path . --script res://tools/construction_project_test.gd
& $godot --headless --path . res://tools/services_test.tscn

# round-trip
& $godot --headless --path . --script res://tools/economy_save_load_roundtrip_test.gd -- `
  --mode=world --days=60 --save-at=30
& $godot --headless --path . --script res://tools/economy_save_load_roundtrip_test.gd -- `
  --mode=demo --days=120 --save-at=60

# mundo (mesmo comando, com e sem --ai)
& $godot --headless --path . --script res://tools/economy_world_test.gd -- --days=60

# sweep 365 d x5
& $godot --headless --path . --script res://tools/economy_world_sweep.gd -- `
  --days=365 --baseline=res://docs/economy/perf/services_fix_world_sweep_v2.json `
  --out=res://docs/economy/perf/construction_world_sweep.json
```

Evidência local (não versionada por `*.log` no `.gitignore`):
`docs/economy/perf/construction_world_sweep.log`; JSONs por variante em
`%APPDATA%\Godot\app_userdata\Open Grand Strategy\world_sweep\*_365.json`.
