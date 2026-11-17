# Q-015 — FRENTE A: mercado de moradia (B1, B2, B5 e §5)

- **Branch:** `fix/q015-moradia` (worktree `G:/projetos/opengs-wt-q15a`), base
  `balance/global-resources` = `a8d7984` (inclui `69e2790`, fila única de obras).
- **Data:** 2026-11-17. **Dono:** gerente-simulacao. **Modelos:** só Flash.
- **Escopo tocado:** `gdextension/src/sim/housing.cpp`, `housing.h`, bloco de
  moradia de `sim_constants.h` e o ponto mínimo de `consumption.cpp` (reserva do
  aluguel no orçamento do dia). Nada de `urban_land.cpp`, `city_gov.cpp` ou
  constantes de capacidade de zona (FAR, m²/unidade).
- **Medição sempre com `--urban=1`**, 6 cidades, 365 d, DLL debug da própria
  worktree (build incremental Ninja/clang).

## 1. O que estava errado (e o que NÃO estava)

### B1 — nenhuma unidade construída: confirmado, causa = gate de lucro

`housing.cpp:498-505` exige `price ≥ 1,2 × custo_unit`, com
`custo_unit = aluguel×12×kDemoHousingBuildCostYears + m²×land_price` e
`price = aluguel×12÷0,05` (20 anos de aluguel). Com `B = 14` (70% do valor) e a
terra a 30–44% do valor (o índice de renda local de Tóquio multiplica o
`land_price` por ~1,47), custo+terra somava **100–114% do preço**: a margem de
20% era **matematicamente inalcançável** e nenhuma obra abria — a fila única
não entregava moradia porque **não havia obra na fila**. A fila em si funciona
(ver §4).

Derivação do novo valor (documentada no próprio `sim_constants.h`):

```text
price ≥ 1,2 × (12·B·aluguel + terra)
240·aluguel ≥ 14,4·B·aluguel + 1,2·terra
caso neutro (terra = 30% do preço):      B ≤ 0,5333 ÷ 0,05 = 10,67 anos
JPN-TOK (terra = 44% do preço medido):   B ≤ 6,66 anos
```

`kDemoHousingBuildCostYears = 6,0` (custo de obra ≈ 30% do valor de mercado,
faixa de custo de reposição em metrópoles caras 25–40%) fica 10% abaixo do teto
de Tóquio e deixa margem de 25% (TOK) a 63% (SP).

Também: o lote da incorporadora agora é limitado pelo caixa das firmas
(`units = min(falta, 0,5% da demanda, caixa/custo_unit)`), em vez de o
`start_project` recusar em silêncio por falta de caixa.

### B2 — "só 20% do aluguel é pago" era ARTEFATO DE MEDIÇÃO

O aluguel **já era pago integralmente** no `a8d7984`. Prova com o próprio
probe antes da mudança (`q15a_before_urban_365.json`):

| Cidade | `rent_paid` (dia) | fatura/dia (unidades×posse×aluguel÷30) | pago/faturado (dia) | `rent_paid_month`/fatura mensal |
| --- | --- | --- | --- | --- |
| BRA-SAO | 34.317,9 | 34.317,9 | **1,000** | 0,2000 |
| JPN-TOK | 50.151,0 | 50.151,0 | **1,000** | 0,2000 |
| NGA-LAG | 13.936,3 | 13.936,3 | **1,000** | 0,2000 |
| IND-MUM | 26.428,6 | 26.428,6 | **1,000** | 0,2000 |
| USA-NEW | 42.780,1 | 42.780,1 | **1,000** | 0,2000 |
| AUS-WEI | 18,0 | 18,0 | **1,000** | 0,2000 |

`rent_paid_month` é **mês-corrente**: o snapshot cai 6 dias depois do
fechamento mensal, então o numerador é 6 dias e o denominador é a fatura de 30
dias → 0,2 **em todas as cidades** (não é mercado). E `rent_burden` era
publicado com esse numerador parcial todo dia, ficando ~5× abaixo do real
(0,034 em SP contra 0,179 no mês cheio).

Mudanças:
1. **Fonte do aluguel:** `daily_rent_obligation` (nova, `housing.h/cpp`) calcula
   o aluguel do dia (fatura ÷ 30, limitado ao teto de ônus de 0,40 da renda dos
   inquilinos). `phase_income` (consumption.cpp) **desconta essa obrigação do
   orçamento discricionário**, depois da comida e antes de life/luxury — a
   ordem que a spec §19/W3 declara. `phase_housing` executa a transferência
   (poupança → família/empresa/prefeitura + manutenção), nunca abaixo da
   reserva de comida.
2. **Métrica:** `rent_burden` passa a ser calculado **no fechamento do mês**
   com o acumulado cheio, antes de zerar os contadores.
3. **Piso da faixa (0,15):** o nível é ancorado uma única vez na renda do dia 1;
   a renda nominal cresce com a industrialização (1,5× em MUM, ~6× em WEI em
   365 d) e o aluguel ficava congelado → ônus medido 0,11 (MUM) e 0,085 (WEI).
   Quando o ônus de **mercado** do mês fechado (fatura ÷ renda, independente do
   pago) cai abaixo de `kDemoHousingBurdenFloor = 0,15`, o nível é reancorado na
   **renda corrente** até o piso. É a conversão do nível da fonte (renda de
   hoje), não um passo de mercado: o ±2%/mês por desequilíbrio continua sendo o
   único sinal de oferta/demanda.

### B5 — favela por transbordo de depreciação: corrigido

O gatilho usava `popular_shortage` **depois da filtragem**, que soma ao popular
o excedente de demanda dos padrões altos: a depreciação de 1%/ano do estoque
**médio** (93% do estoque de Mumbai) virava "falta popular" e abria favela.

Agora a falta é a **própria** do padrão popular
(`households_by_tier[POPULAR] × escala − estoque popular`), e a favela só abre
com **nenhuma vaga total** E falta popular > 5% E (aluguel popular **no teto de
ônus** — os pobres sem renda para alugar — **ou** falta severa ≥ 20%, quando a
falta é de estoque, `kDemoHousingSlumSevereShortage`, calibração declarada).

### §5 — capacidade do setor e fração de material

- **Capacidade:** a obra de moradia mora na lista única (`kind =
  CONSTRUCTION_HOUSING`) desde `69e2790`; o tick do §15
  (`construction.cpp:327-333`) pausa **qualquer** obra quando
  `sector_workers[1] × 20 XGS/dia` não comporta o dia. Divergência 2 do §5
  **resolvida pela unificação** (antes a moradia pagava 100% adiantado sem
  checar capacidade).
- **Material:** `kDemoHousingMaterialShare` era 0,40 contra 0,15 do §15 (2,7×
  mais material pela mesma obra em XGS). Alinhado a
  `kDemoConstructionMaterialShare` (0,15) — a moradia não é exceção da fila
  única.

## 2. Medição antes × depois (365 d, `--threads=1`, DLL debug)

Antes = `q15a_before_*` (a8d7984); depois = `q15a_final_*` (branch). `Ônus`
é o campo publicado (mês corrente no antes; último mês fechado no depois).
`Entregues` = estoque final − estoque inicial×(1−1%/ano): unidades só entram
por obra de moradia concluída (`apply_completed_project`); a favela está fora
da conta porque o informal não variou.

| Cidade | Ônus antes | Ônus depois | Pago/faturado (dia) | Estoque (%/ano) | Entregues (acima da depreciação) | Informal Δ | Obras da prefeitura |
| --- | --- | --- | --- | --- | --- | --- | --- |
| BRA-SAO | 0,034 | **0,179** | 1,000 | −0,569 | **+29.570** | 0 | 0 (obras de moradia social: ver §4) |
| JPN-TOK | 0,045 | **0,260** | 1,000 | −0,350 | **+115.214** | 0 | 0 |
| NGA-LAG | 0,049 | **0,246** | 1,000 | −0,426 | +26.981 | 0 | 0 |
| IND-MUM | 0,028 | **0,264** | 1,000 | −0,353 | +41.506 | **0** (antes +25.094) | 0 |
| USA-NEW | 0,054 | **0,288** | 1,000 | −0,823 | +16.028 | 0 | 0 |
| AUS-WEI | 0,017 | **0,318** | 1,000 | −0,830 | +2 | 0 | 0 |

- **Ônus 0,15–0,40 nas 6 cidades:** ATINGIDO (0,179–0,318).
- **Pago/faturado ≥ 0,70:** ATINGIDO (1,000 nas 6; já era 1,000 antes — o
  "0,20" era o artefato acima).
- **≥ 1 obra de moradia entregue em BRA-SAO e JPN-TOK:** ATINGIDO (+29.570 e
  +115.214 unidades acima da depreciação; ver §4 para obras em andamento).
- **Estoque não cresce > 3%/ano:** ATINGIDO (todas as 6 **caem** 0,35–0,83%/ano;
  a obra cobre parte da depreciação de 1%/ano, não a supera).
- **Favela só no último caso:** ATINGIDO (Δ informal = 0 nas 6; MUM deixou de
  crescer por transbordo).

Portões do mundo (365 d): **falências de construção 6** (limite 28; era 6 no
baseline) · **drift de dinheiro 0,0002** (limite 0,01; 2.200.539.587,40 →
2.200.539.587,40) · **comida: 179 nações comparadas, 0 pioraram** (AFG 1,000;
SOL 0,574 contra 0,553 do baseline) · invariantes sem violação nos 365 dias ·
negócios de serviço 53.339 (antes 53.324).

## 3. Determinismo e save

- `--threads=1` vs `--threads=8`: JSON principal **byte-idêntico**
  (`sha256 78e484de…`); o retrato urbano difere só no campo `threads`.
- Round-trip de save: `economy_save_load_roundtrip_test` 30+30 d == 60 d
  direto, **0 diffs** (`save=f15062ee…`). Nenhum campo salvo novo.

## 4. Por que a fila única não entregava moradia

Não era a fila: era o gate de lucro (nenhuma obra era criada). Com o custo
corrigido, a fila passa a receber moradia e entregar. Evidência em probe de
150 d (6 cidades, com IA):

```text
[hsg] BRA-SAO owner=state units=10521  d=30/90 paused=false
[hsg] JPN-TOK owner=state units=37634  d=30/90 paused=false
[hsg] IND-MUM owner=private units=906  d=60/90 paused=false
[hsg] IND-MUM owner=private units=916  d=30/90 paused=false
[hsg] NGA-LAG owner=state units=11564  d=30/90 paused=false
[hsg] USA-NEW owner=state units=3490   d=30/90 paused=false
[hsg] IND-MUM owner=state units=15787  d=30/90 paused=false
[hsg] projetos=8 unidades_em_obra=81736
```

No mundo inteiro (834 cidades, 150 d): **1.055 obras de moradia em andamento,
1.189.714 unidades em obra** (a maioria incorporadora privada). A prefeitura
também passou a abrir moradia social (W2→W3) nas 5 cidades grandes; isso é o
caminho de moradia social, não o `gov.projects` de infraestrutura do B3 (outra
frente).

## 5. Testes rodados (todos verdes)

```powershell
godot --headless --path <wt> --script res://tools/housing_market_test.gd          # exit 0
godot --headless --path <wt> --script res://tools/construction_project_test.gd    # exit 0
godot --headless --path <wt> --script res://tools/economy_integration_test.gd     # exit 0
godot --headless --path <wt> --script res://tools/economy_save_load_roundtrip_test.gd  # 0 diffs
```

Medição (worktree `G:/projetos/opengs-wt-q15a`):

```powershell
godot --headless --path G:/projetos/opengs-wt-q15a --script res://tools/economy_world_test.gd `
  -- --ai=1 --days=365 --urban=1 --threads=1 --fx=real `
  --baseline=res://docs/economy/perf/construction_world_sweep.json `
  --out=user://world_sweep/q15a_final_365.json `
  --urban-cities=BRA-SAO,JPN-TOK,NGA-LAG,IND-MUM,USA-NEW,AUS-WEI `
  --urban-out=user://q15a_final_urban_365.json
```

## 6. Pendências honestas

1. **B3 (prefeitura/infra)** continua fora desta frente: `gov.projects` segue 0
   nas 6 cidades (é `city_gov.cpp`, de outra frente).
2. **Autoconstrução popular** segue inalcançável no mundo real: exige
   `custo×4 ≤ folga de poupança` e a poupança da POP fica na reserva de comida.
   A oferta popular no ano vem da moradia social (W2→W3); o estoque popular
   ainda deprecia 1%/ano.
3. **Piso do ônus (0,15)** é uma reancoragem na renda corrente: em AUS-WEI o
   aluguel sobe de uma vez (~0,085 → 0,15 de ônus). É conversão de nível, não
   passo de mercado; se o dono preferir indexação mensal contínua, é decisão de
   contrato (muda o teste "sobra → aluguel cai", que compara nível nominal).
4. **Dado de entrada:** população de cenário de IND-MUM (B4) é outra frente;
   com ela, a ocupação da zona e o tamanho do mercado popular mudam.
5. `GAME_STATE.md` e `docs/team/QUADRO.md` não foram editados (a integração faz).
