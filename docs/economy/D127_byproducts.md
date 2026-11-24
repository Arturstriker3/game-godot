# D-127 — Subprodutos com balanço de massa real

**Status:** implementado em `feat/d127-byproducts` (base `feat/f1-fleet`, HEAD `6caae697`).
**Escopo:** dados (`goods.json`/`recipes.json` via spec) + `tools/build_goods_chain.py`.
Sem bens novos, sem C++, sem UI. O dono vetou "cavaco" como bem novo: a parte não
aproveitada da tora fica como **perda declarada**, não como produto.

## 1. Decisão

Nenhuma receita pode criar matéria do nada. A regra aplicada a toda a cadeia é:

```
Σ saídas (produto principal + subprodutos) + perda declarada ≤ Σ insumos
```

A perda declarada é o campo `co_loss` (fração por lote de insumo), que já existia
para a co-produção da refinaria (D-123) e agora vale para qualquer receita. O
subproduto continua precificado pela **regra de sobra**: `0,10 × preço do pai`
(`MARGIN_BYPRODUCT` em `tools/build_goods_chain.py`).

## 2. Receitas alteradas

| receita | antes | depois | perda declarada |
|---|---|---|---|
| `flour` (moagem) | 1 trigo → 1 farinha + 1 farelo (**Σ 2 > 1**) | 1 trigo → 0,75 farinha + 0,25 farelo | 0 |
| `planks` (serraria) | 1 madeira → 1 tábua + 1 serragem (**Σ 2 > 1**) | 1 madeira → 0,55 tábua + 0,12 serragem | 0,33 (casca/cavaco/umidade) |
| `cellulose_b` | 1 serragem → 0,7 celulose | 1 serragem → 0,4545 celulose (2,2:1) | 0 |
| `pallets_b` | 1 serragem → 0,7 palete | 1 serragem → 0,8696 palete (1,15:1) | 0 |

`gasoline_a` (controle, já conforme): 1 petróleo → 0,45 gasolina + 0,29 diesel +
0,09 querosene + 0,17 perda = 1,00.

### Fontes dos rendimentos

- **Moagem 75% de extração:** a farinha "straight grade" tem extração de 72–80%,
  com ~75% como referência de panificação; farelo + gérmen ficam com o restante
  (~25%). USDA ERS *Wheat Data* (flour extraction/milling yield) e FAO.
- **Serraria 50–60% em tábua, 8–13% de serragem:** FAO, *Forest Product
  Conversion Factors* (tabela de material balance: sawnwood 45–63%, sawdust
  8–13%, chips/slabs 19–44%, shrinkage 1–12%) —
  <https://openknowledge.fao.org/server/api/core/bitstreams/c39a1bcc-439c-4ca3-8cea-78c40e5fc5f7/content>;
  FAO, *Trash or treasure?* (recuperação de serraria 42–60%, média 50,8%) —
  <https://www.fao.org/4/x6966e/x6966e02.htm>; USDA Forest Service FPL,
  *Estimating hardwood sawmill conversion efficiency* —
  <https://research.fs.usda.gov/treesearch/download/35.pdf>.
  A parcela de 0,33 do lote é casca + cavaco + encolhimento, isto é, perda
  declarada, **não** um bem.
- **Serragem → celulose 2,2:1 (~45%)** e **serragem → palete 1,15:1 (~87%)**:
  parâmetros da spec D-127 (ordem de grandeza industrial de polpa/painéis).
  Não há série pública única para "serragem" como commodity; ficam declarados.

## 3. Auditoria de todas as receitas

Varredura de `recipes.json` (114 receitas) com a fórmula acima:

- **Antes:** 2 violações — `flour` (2 > 1) e `planks` (2 > 1).
- **Depois:** **0 violações**.
- 61 receitas têm mais de um insumo (Σ insumos ≥ 2 t para 1 t de saída principal),
  portanto já eram conservativas.
- Só 3 receitas tinham `byproducts`/`outputs`: `flour`, `planks`, `gasoline_a`.
- **Ausência declarada:** o catálogo de 115 bens **não tem cana, açúcar, bagaço
  nem melaço**. O exemplo "bagaço da cana" da spec não tem dado para corrigir;
  nada foi inventado. Farelo (`bran`) e serragem (`sawdust`) são os únicos bens
  `kind: "byproduct"` do jogo.

## 4. Diff do `goods.json` (21 preços derivados)

Só mudam bens alcançados pelas 4 receitas acima (ou pela cascata derivada).
Todos continuam `derived:v1` ou `derived:byproduct_of:*:v1`; nenhuma procedência
foi trocada por número calibrado à mão.

| bem | antes | depois | Δ | fonte |
|---|---:|---:|---:|---|
| `flour` | 50,00 | 66,67 | +33,3% | derived:v1 |
| `bran` | 5,00 | 6,67 | +33,4% | 0,10 × flour |
| `bread` | 72,50 | 96,67 | +33,3% | flour |
| `bagged_rice` | 91,47 | 112,59 | +23,1% | rota papel (era plástico) |
| `feed` | 7,81 | 10,42 | +33,4% | rota farelo |
| `meat` | 11,32 | 15,11 | +33,5% | feed |
| `canned_food` | 31,06 | 55,90 | +80,0% | meat + containers |
| `combat_ration` | 143,01 | 184,53 | +29,0% | canned_food/meat |
| `alcohol` | 65,39 | 84,74 | +29,6% | rota batata + containers |
| `premium_spirits` | 140,13 | 205,26 | +46,5% | alcohol + planks |
| `organic_fertilizer` | 24,17 | 32,24 | +33,4% | bran |
| `planks` | 31,25 | 56,82 | +81,8% | yield 0,55 |
| `sawdust` | 3,12 | 5,68 | +82,1% | 0,10 × planks |
| `furniture` | 56,25 | 102,28 | +81,8% | planks |
| `luxury_furniture` | 213,75 | 259,78 | +21,5% | planks + fabric |
| `cellulose` | 5,57 | 15,62 | +180,4% | rota serragem (2,2:1) |
| `paper` | 8,08 | 22,65 | +180,3% | cellulose |
| `pallets` | 6,46 | 9,47 | +46,6% | rota serragem |
| `tools` | 154,06 | 191,14 | +24,1% | planks + iron |
| `construction_material` | 181,25 | 218,33 | +20,5% | planks + hardware |
| `containers` | 10,10 | 23,44 | +132,1% | rota alumínio (era papel) |

**Leitura dos diffs:**

- `planks` sobe porque 1 t de tora rende 0,55 t de tábua (o custo por tonelada de
  tábua dobra). `sawdust` acompanha como 10% do pai.
- `cellulose`/`paper`/`containers` mudam de **rota mais barata**: antes a serragem
  era 1:1 e sustentava celulose barata; agora 2,2:1, a celulose de madeira fica
  relativamente mais barata e `containers` passa da rota papel para a de alumínio.
- `canned_food` e `combat_ration` acumulam `meat` (feed←farelo) e `containers`.
- `bagged_rice` troca embalagem plástica por papel (mais barato no novo preço).
- Nenhum bem fora da cascata de `flour`/`planks` mudou.

## 5. Builder (`tools/build_goods_chain.py`)

- `byproducts` da spec aceitam **quantidade por lote**:
  `{"id": "bran", "qty": 0.25}`; string legada continua valendo como 1 t.
- `co_loss` deixou de ser exclusivo de `outputs[]` e vale para qualquer receita.
- A checagem de conservação passou a rodar para **todas** as receitas (antes só
  para co-produção). `check` = **0 problemas**.
- `byproduct_coef_source` fica `spec_qty` quando há quantidade declarada e
  `default_unit` na forma legada.

O núcleo C++ já lia `byproduct_qty` (`production.cpp`, `ai_view.cpp`,
`ai_group.cpp`, `sim_core.cpp`, save): nada mudou no contrato C++/GDScript e o
save não muda (é leitura de `recipes.json` no início).

## 6. Verificação

- `python tools/build_goods_chain.py check` → **0 problemas**.
- `tools/goods_data_test.gd`, `tools/production_chain_test.gd`,
  `tools/production_template_test.gd`, `tools/f0_fuels_test.gd` → ver §7.
- Rodada 365 com `--control=res://docs/economy/perf/c6_gate_base.json` → ver §7.

## 7. Resultados dos testes (24/11/2026)

Worktree `opengs-wt-d127`, após o merge de `balance/global-resources`
(`fa788c55` = F0 + D-125; DLL debug reconstruída do fonte na própria worktree,
`--headless --import` exit 0).

| teste | resultado |
| --- | --- |
| `tools/d127_byproducts_test.gd` (novo) | **verde** — 114 receitas sem criar massa; 3 receitas com subproduto/perda fechando exato; serraria 0,55 + 0,12 + 0,33; serragem 2,2:1 e 1,15:1; 115 bens = spec; runtime 0,12/0,55 |
| `production_chain_test` | OK |
| `production_template_test` | PASS |
| `price_formation_test` | OK (piso de custo 600/648) |
| `d125_efficiency_test` | OK (rampa, modos, save v40, determinismo) |
| `f0_fuels_test` | OK (45/29/9, frota, migração v39) |
| `economy_save_load_roundtrip_test` | OK (save/status idênticos, 0 diffs) |
| `market_template_test` | PASS (115 bens, moedas, ofertas) |
| `python tools/build_goods_chain.py check` | 0 problemas |

Rodada 365 (`c1_world_ai_365.gd --control=res://docs/economy/perf/c6_gate_base.json`,
365 s, 1 thread, JSON em `G:/opengs_tmp/c1_world_ai_365.json`):

- **falências 4**, **comida 94/179** (mediana 0,9727), **0 violações**, 0 travados,
  drift 0,005618, digest `01b835ea`.
- Falências: as 2 do controle (`pri_pon_copper_mine` e `pri_pon_iron_mine`, dia
  126), `chl_san_wheat` (dia 153) e `mlt_val_wheat` (dia 154).
- **A/B na mesma branch, só com o dado anterior** (`6caae697`: `goods.json` +
  `recipes.json` antigos trocados no lugar): **3 falências** (as 2 PRI-PON +
  `chl_san_wheat`), **comida 92/179**, drift 0,005610, digest `6ae5b3e8` — o
  mesmo da D-125. Portanto: (a) o merge de F0/D-125 está limpo (o dado anterior
  reproduz a rodada da D-125 bit a bit); (b) a 3ª falência da D-125 é a
  `chl_san_wheat`, não a `mlt_val_wheat` como a linha D-125 registrou; (c) o
  dado D-127 **acrescenta** exatamente uma falência, a `mlt_val_wheat`.

**Portão de falências da spec (≤3) NÃO atendido — por 1, e não foi amolecido.**
`mlt_val_wheat` fecha por insolvência bancária com `loss_days=0`, receita 0,
`power_ration_days=153` (de 153 dias) e energia no teto de preço (1,658); é a
fazenda isolada que a D-124/D-125 já descrevem como marginal, que perde a
última venda quando o custo real da cadeia alimentar (farinha/pão +33%, ração
+33%) encolhe a demanda. O preço específico responsável **não foi isolado** por
A/B de preço único — o efeito é da cadeia, e o caminho para fechar o portão é
decisão de balanceamento (Game Systems Designer), não de dado inventado: ou o
dono aceita 4 falências, ou se mexe na regra de preço derivado (por exemplo
creditar o valor do subproduto no preço do pai, que hoje cobra a tora inteira só
da tábua e depois vende a serragem a 10% dessa tábua), ou se revisa a planta
marginal isolada.

## 8. Riscos e regressões a observar

- **Preço real vs. preço derivado:** a regra `custo / yield × margem` não credita
  o valor do subproduto, então a tábua sobe 82% e a farinha 33%. É consequência
  declarada da regra existente, não um número novo escolhido à mão.
- **Comida:** a farinha perde 25% de massa por tonelada de trigo; o farelo vira
  ração. O portão de comida (≤97/179) é o teste decisivo.
- **Rotas alternativas:** `cellulose_b`/`pallets_b` continuam declaradas, mas com
  serragem escassa (0,12 t/tora) tendem a perder para madeira/tábua; isso é
  esperado e não remove a rota do dado.
