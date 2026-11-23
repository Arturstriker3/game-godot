# F0 — combustíveis por grau (gasoline / diesel / jet_fuel)

> Decisão **D-123** no `GAME_STATE.md` (§14). Branch `feat/f1-fleet`, worktree
> `opengs-wt-f1`. Substitui o bem genérico `fuel` da F4 por três graus com
> cadeia e consumo próprios: gasolina (ciclo Otto), diesel (ciclo diesel) e
> querosene de aviação (turbina).

## 1. O que muda

| Antes (F4) | Agora (F0) |
|---|---|
| 1 bem `fuel` (order 47) produzido por 4 receitas `fuel_a..fuel_d` | 3 bens finais: `gasoline` (order 47), `diesel` (113), `jet_fuel` (114) |
| refinaria de petróleo com 1 saída | **co-produção**: `gasoline_a` entrega 3 saídas no mesmo lote |
| frota com um estoque `fuel` (caminhão, navio e avião no mesmo tanque) | frota com **dois estoques** (`diesel`, `jet_fuel`) e alvo por modal |
| usina a óleo queimava `oil` cru | usina a óleo queima **diesel** (produto refinado) |
| POP/passageiros/exército/frete consumiam `fuel` | POP e passageiros: **gasolina**; exército e frete: **diesel**; aviação: **jet_fuel** |

Preços-base declarados (`goods.json`): gasoline **78,61**, jet_fuel **78,61**,
diesel **58,00** por tonelada — contra 43,50 do antigo `fuel`. O preço é
**derivado do custo da rota mais barata**, não do rateio do lote: a refinaria
(`45,00/0,83 × 1,45`) daria 78,61 às três saídas, mas o diesel é puxado para
baixo pela rota CTL (`6,00/0,15 × 1,45 = 58,00`, ver §9.4).

## 2. Cadeia (dado, não motor)

`data/definitions/economy/recipes.json`:

| Receita | Prédio | Insumo | Saídas (por t de insumo) |
|---|---|---|---|
| `gasoline_a` | refinery | 1 t oil | gasoline 0,45 + diesel 0,29 + jet_fuel 0,09 (**perda 0,17**) |
| `gasoline_b` | chemical_plant | 1 t ethanol | gasoline 0,80 (mistura E10) |
| `diesel_a` | refinery | 1 t gas | diesel 0,30 (GTL de baixo rendimento) |
| `diesel_b` | chemical_plant | 1 t coal | diesel 0,15 (CTL de baixo rendimento) |

Frações da refinaria: **EIA Refinery yield (EUA, 2024)** — gasolina 45%,
destilados 29%, querosene de aviação 9%; os 17% restantes são perdas/outros
não modelados (`co_loss`, `byproduct_coef_source: spec_qty`). Nada disso vem do
código: o motor lê `outputs[]` do JSON.

Consumidores (`needs.json`):

* `levels.life.categories.transport.substitutes = ["gasoline"]` (POP);
* `institutional.pop_life` inclui `gasoline`; `institutional.army` inclui
  `diesel`; `institutional.vehicle_fleet` declara `diesel` **e** `jet_fuel`;
* serviços de transporte de passageiros consomem gasolina; logística de carga,
  diesel (ver `services.json`).

## 3. Co-produção no núcleo

* `EconomySim::set_recipes` aceita `outputs: [{good, qty}]`. A **primeira**
  saída é o produto principal (capacidade, demanda e preço de referência do
  prédio); as demais entram em `recipe.byproducts[]` com a **qty do dado** — o
  caminho legado `byproducts` (coeficiente unitário) continua válido para
  receitas antigas.
* `pick_recipe` abate do custo por tonelada o valor das saídas extras; a margem
  da firma (`phase_production`) soma a receita delas. Sem isso a refinaria
  pareceria deficitária (o custo do petróleo dividido só pela gasolina) e
  cortaria produção com diesel/querosene vendáveis.
* `phase_production` já creditava cada subproduto no mercado e em
  `produced_today/produced_total`; com a qty do dado, a saída total por lote é
  0,83 t por tonelada de petróleo. `tools/f0_fuels_test.gd` mede as três
  frações e a conservação (250/161,111/50 t com 555,556 t de petróleo).

## 4. Frota por grau

`Fleet` (sim_types.h) trocou `fuel/fuel_target` por
`diesel/diesel_target/jet_fuel/jet_target` e ganhou contadores por grau
(`diesel_used_today/total`, `jet_*`, `diesel_bought_total`, `jet_bought_total`),
mantendo os totais `fuel_used_*`/`fuel_bought_total` como soma dos dois.

* `init_fleets`/`phase_fleet` derivam os alvos separando o consumo diário de
  cada modal (`kFleetFuelPerTruckDay/ShipDay/PlaneDay`, já declarados) e rateiam
  o piso per capita antigo pela participação de cada grau — nenhuma constante
  nova.
* `TransportDemand` passou a carregar `diesel` e `jet_fuel`. `route_demand`
  credita o grau do modal; `fleet_capacity_scale` limita por grau e marca
  `fuel_limited`; `fleet_reserve` debita cada tanque.
* `fleet_fuel_price(mode)` usa o preço do grau que o modal queima: avião paga
  querosene, caminhão/navio pagam diesel (`leg_unit_freight`).
* `get_fleet` publica `diesel`, `diesel_target`, `jet_fuel`, `jet_target`,
  `diesel_used_*`, `jet_used_*`, `diesel_bought_total`, `jet_bought_total` e os
  totais antigos. `set_fleet(nation, trucks, ships, planes, diesel, jet_fuel,
  spare_parts)` (hook de teste/administração).

## 5. Energia

`power.cpp`: `fuel_product_of(POWER_OIL)` = **diesel** (era `oil`). A usina
térmica a óleo compra diesel no mercado local, como as demais compram carvão e
gás. `storage.cpp` classifica `gasoline`, `diesel` e `jet_fuel` em
`fuel_tank` (o mapa de armazém em `goods_storage_map.json` já traz os três).

## 6. Save v39 e migração

`kSaveVersion` 38 → **39**. Saves antigos (com o bem `fuel`) migram no load
normal (o jogo já carregou a lista NOVA de bens com `set_goods`):

| Estado antigo | Migração |
|---|---|
| estoque de `fuel` no mercado da cidade | rateado 45/83 gasolina, 29/83 diesel, 9/83 querosene (preço de cada grade = preço-base declarado) |
| estoque de `fuel` em armazém (`site.stock`) | mesma divisão, inclusive `inflow_t` |
| frota `fuel`/`fuel_target` | rateada pelo consumo diário dos modais daquela frota (avião → querosene; caminhão/navio → diesel); `*_used_total`/`*_bought_total` divididos na mesma proporção |
| construção/obra com `output_product = fuel` | passa a produzir `gasoline` |
| receita `fuel_a..fuel_d` | `fuel_a→gasoline_a`, `fuel_b→gasoline_b`, `fuel_c→diesel_a`, `fuel_d→diesel_b` |
| carga em trânsito de `fuel` | vira `gasoline` com a mesma tonelagem e a mesma reserva de frota |
| `shipment.fuel` | dividido em `shipment.diesel`/`jet_fuel` pelo mix de modais do pedido |
| histórico `produced_total/consumed_total` de `fuel` | descartado (bem que não existe mais; é histórico, não estoque) |

Num **snapshot** antigo (`to_snapshot`/`from_snapshot`) a lista de produtos vem
do próprio arquivo e `fuel` continua existindo: mercado, armazém e receitas
**não** migram (o mundo antigo segue com o bem e a cadeia que tinha). Só o
tanque da frota é dividido em `diesel`/`jet_fuel`, porque o `struct` não tem
mais `fuel` — e, sem `diesel` na lista antiga, a reposição automática daquela
frota não encontra o bem (limitação declarada; snapshots antigos não são o
caminho de save do jogo).

## 7. UI

A tela de logística (`data/ui/templates/logistica.json` + 
`core/ui/screens/logistics_screen.gd`) mostra **Diesel** e **Querosene** em
colunas/linhas próprias no lugar do antigo "Combustível". Rótulos curtos para a
coluna: `fleet.diesel` ("Diesel") e `fleet.jet` ("Querosene"/"Jet fuel"/
"Queroseno") em pt-BR, en e es; o nome completo do bem continua em
`product.jet_fuel` ("Querosene de aviação") nas demais telas.

Captura (`--phase=f0 --run=f0_fuels --sizes=1366x768,1920x1080`): as telas
26–29 de logística saíram com `issues: []` nas duas resoluções em
`docs/ui/captures/f0/f0_fuels/layout_report.json`.

**Ícones (2026-11-23).** Os PNGs ilustrados aprovados pelo dono
(`v3_extra/fuels/APPROVED.json`: gasoline=a, diesel=a, jet_fuel=c, refinery=a)
entraram em `assets/icons/illustrated/` (128 px) e `illustrated/64/` (64 px);
`refinery` vai em `illustrated/places/` porque é construção
(`ProductIcons.place_texture`). `ProductIcons` resolve os bens pelo id do
`goods.json`, sem mapa extra; o SVG do Icon Forge (`resource_icons.json` cita os
três) continua como fallback. `fuel.svg`/`.import` foram removidos — o bem não
existe e o id saiu do `resource_icons.json`. `batches_v3_production.json` ganhou
o prompt de `refinery` como registro da arte.

## 8. Verificação

| Verificação | Resultado |
|---|---|
| `tools/f0_fuels_test.gd` (novo) | OK — frações 45/29/9, conservação 83%, frota exigindo o grau do modal, usina a óleo→diesel, mapeamento do dado e migração v39 (mercado, armazém, frota, receita) |
| `tools/goods_data_test.gd` | OK |
| `tools/e2e3_power_test.gd` | OK |
| `tools/economy_save_load_roundtrip_test.gd` | OK (60 d direto × 30+30 com o mesmo save/status SHA) |
| `tools/fleet_fuel_test.gd` | OK (frota derivada, sem frota/sem diesel, compra de truck, desgaste) |
| `tools/logistics_template_test.gd` | PASS |
| `tools/pop_needs_test.gd` | OK |
| `ui_capture` (`--phase=f0 --run=f0_fuels --sizes=1366x768,1920x1080`) | telas 26–29 de logística com `issues: []` nas duas resoluções; o runner saiu com código 1 por exceções do próprio harness nas telas de serviços (a tela em si passa no `services_template_test`) |
| Rodada 365 com `--fuel-diag=1` (`c1_world_ai_365.gd --control=c6_gate_base.json`) | ver §9 — trajectory `50278f4f` idêntica à rodada sem o diagnóstico |

## 9. Rodada de 365 dias

Comando:

```powershell
& $godot --headless --path . --script res://tools/c1_world_ai_365.gd -- `
  --days=365 --fuel-diag=1 `
  --out=res://docs/economy/perf/f0_world_ai_365_fuel.json `
  --control=res://docs/economy/perf/c6_gate_base.json
```

`--fuel-diag=1` (novo, só leitura) fotografa no dia 1 e no fecho, por nação:
oferta/demanda/desejo/estoque e preço de gasolina, diesel e querosene,
capacidade instalada por receita (`gasoline_a`, `diesel_a`, `diesel_b`), frota
por grau e o frete pago por tonelada de comida importada (ordens do dia).
Também publica `food_gate.ratios` (média de 30 d de **todas** as nações no
fecho); `food_chain.food_avg30` continua sendo o valor do dia em que a nação
cruzou o portão (no dia 1 um importador sem estoque mede 0 — não é o número do
portão).

Medição e leitura dos portões (falências ≤ 2, comida ≤ 95/179, 0 violações):

| Métrica | Medido | Portão |
|---|---:|---|
| Dia final / travados | 365 / 0 | — |
| Violações de invariante | 0 | 0 |
| Falências | 2 (mesmas do controle: USA PRI-PON copper/iron, dia 126) | ≤ 2 |
| Comida (média 30 d, < 0,975) | **97/179** (mediana 0,9703; média 0,9058) | **≤ 95 — FALHOU** |
| Extras vs controle | 5 (PRK, ATG, KNA, AUS, KWT) | — |
| Drift de caixa | 0,567% | — |
| Trajectory | `50278f4f` (idêntica à rodada anterior — o diagnóstico não muda o mundo) | — |
| Parede | 339,3 s (`threads=1`, DLL debug, com o diagnóstico) | — |

Saída: `docs/economy/perf/f0_world_ai_365_fuel.json`.

Contra a rodada D-122 (`c1_world_ai_365_renew.json`, 95/179), entram **KNA
0,9722** e **KWT 0,9742**, sai **CIV 0,9828** (todos contra o limiar de 0,975);
a média (0,9051 → 0,9058) e a mediana (0,9702 → 0,9703) **melhoram**. É
redistribuição de margem, não queda global. Os 5 extras são os mesmos da rodada
anterior (D-123); KNA e KWT cruzam por 0,003 e 0,001.

### 9.1 Combustível por nação (dia 1 → dia 365)

| Nação | Gasolina oferta/demanda/desejo (t/dia) | Diesel oferta/demanda (t/dia) | Preço local gasolina/diesel | Refino (`gasoline_a`/alt.) | Frota diesel (estoque/alvo) | Comida importada |
|---|---|---|---|---:|---:|---:|
| Mundo | 0/0 / **23 657 → 23 807** | 0/0 | 78,61 / 58,00 | **0 / 0** | — | — |
| KNA | 0/0 / 0,15 | 0/0 | 78,61 / 58,00 | 0 / 0 | 2 048/2 048 → 2 040/2 048 | 12 t → 9 t; 76,93 → 26,48/t |
| KWT | 0/0 / 47,64 → 48,02 | 0/0 | 78,61 / 58,00 | 0 / 0 | 40 194/40 194 → 39 495/40 513 | 0 → 1 858 t; 23,27/t |
| CIV | 0/0 / 94,61 → 96,85 | 0/0 | 78,61 / 58,00 | 0 / 0 | 79 654/79 654 → 51 377/81 522 | 0 → 2 107 t; 29,92/t |
| USA | 0/0 / 1 476,73 → 1 478,62 | 0/0 | 78,61 / 58,00 | 0 / 0 | 1 255 291/1 255 291 → 1 207 507/1 256 874 | 0 |

Querosene: oferta/demanda 0/0 no mundo, dia 1 e dia 365; as POPs não consomem
querosene (o consumo é a frota aérea, que gasta a reserva interna).

### 9.2 Capacidade de refino = 0 no mundo inteiro (erro de cadeia, não de migração)

O seed não tem fábrica de combustível: `seed_industry` só semeia farinha/pão
(6 maiores cidades), algodão→tecido→roupa e madeira→celulose→papel (3 cadeias).
O mundo anterior à F0 também não tinha refinaria: a IA nunca abre uma porque
`ai_group.cpp` avalia o candidato por **insumo÷yield sem creditar as saídas
extras** — `45,00 / 0,45 = 100,00/t` contra a gasolina a 78,61 → margem
negativa. É o mesmo defeito que a D-123 corrigiu em `pick_recipe`/margem da
firma, mas **não** na avaliação de investimento da IA (C++) nem nas rotas
alternativas (`diesel_a`: `30,00/0,30 = 100,00`; `diesel_b`: `6,00/0,15 = 40,00`
+ custo-base ≈ 58,00 → margem zero). Resultado: gasolina/diesel/querosene com
**oferta 0 e vendido 0** o ano todo, apesar do desejo das POPs (23,8 mil t/dia de
gasolina) — a satisfação do nível "life" fica 0. **Não é erro de migração de
save:** `f0_fuels_test` cobre a conversão de construções `fuel`→`gasoline` e o
mundo novo nunca teve essas construções.

### 9.3 KWT

KWT tem 0 de capacidade semeada e 0 construída o ano todo; a usina a óleo
compra diesel **importado com prêmio 1,5×** (não há estoque local). O cenário
não tem dado de capacidade de refino por país — semear KWT (ou qualquer país)
exige fonte pública (EIA/OGJ) e decisão do Game Systems Designer; não se
inventa o número.

### 9.4 Preço do diesel: é a rota CTL, não o rateio da co-produção

`tools/build_goods_chain.py` precifica cada bem pelo **custo da rota mais
barata** (margem final 45%):

| Rota | Custo antes da margem | Preço |
|---|---:|---:|
| Refinaria (co-produção) | `45,00 / 0,83 = 54,22` | 78,61 (gasolina, diesel, querosene) |
| GTL (gás) | `30,00 / 0,30 = 100,00` | 145,00 |
| **CTL (carvão)** | `6,00 / 0,15 = 40,00` | **58,00 ← escolhida para o diesel** |

Ou seja: o rateio do lote **não** é a causa do 58,00 (a refinaria sozinha daria
78,61 às três saídas). Se o rateio fosse por valor de mercado ou por energia, o
diesel da refinaria ficaria em ~79,5 — e o `min` entre rotas continuaria
escolhendo o CTL a 58,00. A razão diesel/gasolina do jogo (0,74/t) vem da rota
mais barata. Para comparação real, a EIA (EUA, 2024: gasolina US$ 3,30/gal,
diesel US$ 3,76/gal) dá diesel **+14% por galão** e **≈ igual por tonelada**
(massas específicas 0,745 e 0,85 kg/L) — mas o modelo precifica **custo de
produção**, não preço de bomba, e o CTL com carvão a 6,00/t é o produtor
marginal. Mudar isso é decisão de dado/método do designer (rota CTL ou regra de
preço), não um ajuste no número.

### 9.5 O frete da comida não carrega combustível (hipótese anterior refutada)

`freight.cpp` separa `leg_unit_freight` (tarifa + combustível + frota) de
`leg_unit_freight_food`, que cobra **só a tarifa** (`distância × 0,02/t·km` no
navio). O combustível físico continua exigido no embarque (`route_demand`), mas
o preço do diesel não entra no custo da comida importada. No frete geral o
termo de combustível é `0,000003 × 58,00 = 0,000174/t·km` — 0,9% da tarifa de
0,02; trocar o diesel de 43,50 para 58,00 move o frete em ~0,3%. A hipótese
"diesel caro encarece a comida" registrada na rodada anterior **não se
sustenta** nos números nem no código.

A frota nasce com reserva semeada e não a esgota em 365 d: KNA 2 040/2 048 t
(−0,4%), KWT 39 495/40 513 (−2,5%) e CIV 51 377/81 522 (−37%, justamente a
nação que **sai** do portão). O gargalo físico do diesel não explica as
entradas.

### 9.6 Conclusão e decisão pendente

A implementação (co-produção, frota por grau, usina a óleo→diesel, save v39,
UI, ícones) está funcional e testada. O portão de comida **continua 97/179**
(limite 95) por duas nações que cruzam por 0,003 e 0,001, com médias melhores.
A capacidade de refino zero é o defeito estrutural encontrado — e ele pede uma
decisão de dado/método: **(a)** semear refino real por país com fonte pública
(EIA/OGJ) ou **(b)** creditar a co-produção na avaliação de investimento da IA
(`ai_group.cpp`, núcleo C++) e reabrir a rodada. O teto do portão não foi
mexido.

## 10. Pendências

* Capacidade de refino: semear por país (fonte pública) **ou** creditar a
  co-produção na IA de investimento; hoje não existe nenhuma refinaria no mundo
  novo, então gasolina/diesel/querosene nunca são produzidos e a satisfação do
  nível "life" fica 0.
* Preço do diesel 58,00 vs gasolina 78,61: a diferença vem da rota CTL, não do
  rateio; se o dono quiser diesel ≈ gasolina por tonelada, a decisão é sobre a
  rota CTL (yield/preço do carvão) ou sobre a regra de preço, não sobre o
  número.
* `fuel.svg` foi removido (o bem não existe mais e o id não está em
  `resource_icons.json`); os SVGs do Icon Forge de gasoline/diesel/jet_fuel
  continuam como fallback, coerentes com os demais bens.

## 11. F0 — refino: calibração do ano-base e diagnóstico das falências (D-124)

### 11.1 O que mudou

O seed de refino deixou de aplicar a fração simulada ao dado do EI e passou a
ser calibrado pelo **próprio mundo simulado**, no mesmo princípio da D-119/E2:

1. **Geografia** — Energy Institute 2025, *Oil: Refining capacity* (2024):
   `share_nação = thousand_bpd_nação / 104 523` (Total World).
2. **Demanda** — `fuel_diag.day_1.world.gasoline.desired` = **23 656,925
   t/dia** (medido no dia 1 pelo `c1_world_ai_365.gd --fuel-diag=1`).
   Diesel e querosene têm desejo 0 no dia 1 porque a frota gasta a reserva
   semeada; a demanda de mercado de diesel aparece ao longo do ano (~500 t/dia
   no dia 70). Não se inventa consumo.
3. **Utilização** — **0,80** (throughput ÷ capacidade de destilação; EI 2025:
   ~83 mil b/d de throughput contra 104,5 mil b/d de capacidade em 2024).
4. **Teto de insumo** — a demanda acima exigiria ~52 571 t/dia de petróleo a
   80%; as minas de petróleo do cenário somam **2 694,071 t/dia** de capacidade
   (medido em `f1_refining_measure.json`). O mundo semeado usa
   `min(23 656,925 / 0,80, 2 694,071 × 0,45 / 0,80)` = **1 515,415 t/dia** de
   capacidade principal; alocado nas 66 nações do dado = **1 476,256 t/dia**.
5. **Alocação** — primeiro as cidades com **mina de petróleo** (a refinaria
   nasce junto do insumo); as vagas restantes vão para cidades-porto. Sem
   isso, a planta de porto sem petróleo local não abre demanda efetiva no dia
   1 e nunca começa.

A fração simulada continua registrada no JSON como referência, mas **não é mais
aplicada**: a âncora de demanda/oferta já está na escala do mundo simulado.

**Usos reais que o cenário não modela** (declarados no `meta.calibration`):
petroquímica, aviação civil fora da frota aérea, aquecimento residencial e
comercial a óleo, maquinário agrícola/de construção fora da frota e consumo
militar fora do exército modelado. Por isso a demanda medida é menor que a
real; o número não foi inflado para caber.

### 11.2 Medição (dia 1 → dia 365)

Fonte: `docs/economy/perf/f1_refining_measure.json` (blocos `day_1` e `final`,
gerados por `--refining-diag=1`).

| Métrica | Dia 1 | Dia 365 |
|---|---:|---:|
| Plantas semeadas | 122 | 116 |
| Capacidade semeada (t/dia) | 1 460,1 | 1 522,1 |
| Utilização média | 0,229 | 0,222 |
| Receita / custos / lucro do setor (por dia) | 9 425 / 51 238 / −41 814 | 70 417 / 41 628 / **+28 789** |
| Gasolina: desejado / vendido / oferta (t/dia) | 23 657 / 120 / 334 | 23 854 / 421 / 338 |
| Diesel vendido (t/dia) | 0 | 30,6 |
| Petróleo: capacidade das minas (t/dia) | 2 694,1 | 2 882,9 |

O setor **no agregado** fecha o ano no azul (a receita dos exportadores cobre
os custos); as falências são localizadas em plantas de baixa venda.

### 11.3 A/B de 365 dias (mesmo código, só o seed muda)

| Braço | Falências | Comida (<0,975) | Violações | Travados |
|---|---:|---:|---:|---:|
| **Sem refino** (`--refining=0`) | **2** (PRI-PON cobre/ferro, as do controle) | 97/179 | 0 | 0 |
| **Com refino** (calibrado) | **10** | **94/179** | 0 | 0 |

O refino **melhora a comida** (97 → 94) e não cria violação, mas **piora as
falências**: 8 entidades extras. O teto do portão (≤2) **não foi movido**; a
rodada **não fecha** o critério de falências.

Extras: 6 refinarias (`IRQ-BAG`, `IRQ-ERB`, `ZAF-CAP`, `ZAF-DUR`, `ARG-BUE`,
`IND-MUM`), a fazenda `MLT-VAL` (dia 154) e a mina `usa_ren_oil_mine` (dia
289). Sem refino, `MLT-VAL` e `usa_ren` não falham — são efeito de mercado do
seed, não causas pré-existentes.

### 11.4 Cadeia causal das 8 falências extras

1. **Contabilidade da construção ignora os co-produtos.** `phase_accounting`
   (`gdextension/src/sim/sim_core.cpp`) credita `building.revenue` só com o
   produto principal (gasolina). O diesel e o querosene do mesmo lote vão para
   `city.revenue_by_product[diesel/jet]` e não voltam para a refinaria. Com os
   preços-base, o lucro contábil por tonelada de gasolina é **−39,39**
   (78,61 − 100 de petróleo − 18 de folha/opex); com o crédito dos co-produtos
   seria **+13,71**. A D-123 corrigiu esse crédito no **motor de produção**
   (`pick_recipe`/margem), mas **não** na contabilidade.
2. **Demanda efetiva << desejo.** O desejo de gasolina é 23 854 t/dia, mas o
   vendido no fecho é 421 t/dia (1,8%): o combustível é necessidade de baixa
   prioridade e as POPs não têm caixa para ele. A capacidade calibrada (1 460
   t/dia principal) ainda é ~3,5× a venda efetiva.
3. **Regra do produtor sem venda.** Quando `sold_prev <= 0`, `production.cpp`
   repõe a **capacidade efetiva** cheia, todo dia, não só no primeiro dia. A
   refinaria de cidade com pouca venda enche o estoque, vende quase nada e paga
   folha/opex/insumo — o `loss_days` chega a 60 e a planta fecha.
4. **Efeito de mercado nas primárias.** A mudança de preço/frete/óleo do seed
   leva `MLT-VAL` (trigo) e `usa_ren_oil_mine` (óleo remoto, sem refinaria
   local) ao mesmo `loss_days`; o A/B sem refino mostra que elas sobrevivem.

### 11.5 Conclusão e próximo passo

A **calibração** (share EI × demanda do dia 1 ÷ 0,80, com teto da oferta
simulada) e a **alocação no insumo** reduziram as falências de **29 → 10** e
mantiveram comida e invariantes; o teto do portão **não** foi alterado. O
critério de falências **não fecha** (10 > 2) porque o problema restante é de
**regra do núcleo**, não de dado:

* creditar os co-produtos em `phase_accounting` (mesma conta da D-123) e
* não repor capacidade cheia quando `sold_prev <= 0` em planta secundária,
  ou revisar a prioridade/afordabilidade do combustível no consumo.

Essas mudanças são do **Game Systems Designer** (C++/economia) e não foram
feitas aqui. O dado calibrado e o diagnóstico ficam versionados para a próxima
rodada; a alternativa de marcar as refinarias como `strategic`/`state` para
subsidiar o prejuízo **não** foi usada (seria amolecer o portão).

As duas correções de núcleo pedidas acima foram implementadas e verificadas na
§12; a leitura da §11 é a do **antes** (10 falências).

## 12. F0 — verificação após as correções de núcleo (D-124)

### 12.1 O que foi corrigido (4 commits, `feat/f1-fleet`)

| Commit | Correção |
|---|---|
| `c08f859f` | `phase_accounting` credita a receita de **todas** as saídas do lote (gasolina + diesel + querosene) em `building.revenue`, a mesma conta da D-123. |
| `4103c18e` | Indústria **sem venda** (`sold_prev <= 0`) não repõe a capacidade efetiva cheia todo dia; a planta encolhe com a demanda em vez de encher estoque e pagar folha/opex/insumo. |
| `2e3c1c24` | Preço do co-produto pela **rota principal** e rateio do custo do lote por **valor de mercado** (EIA); `goods.json`: gasolina 78,61 → **81,08**, diesel 58,00 → **74,90**, querosene 78,61 → **78,22**. |
| `b454cb97` | Fixture de multi-insumo mede o limitador na **1ª** oferta (teste do `production_chain_test`). |

### 12.2 Testes (Godot 4.7.2, headless, exit 0)

`f0_fuels_test`, `goods_data_test`, `e2e3_power_test`,
`economy_save_load_roundtrip_test`, `production_chain_test` e
`production_template_test`: **todos OK**, sem falha. O build-debug (ninja) já
estava atualizado contra os fontes.

### 12.3 Rodada de 365 dias

`c1_world_ai_365.gd -- --days=365 --control=res://docs/economy/perf/c6_gate_base.json
--refining-diag=1 --fuel-diag=1` (306,9 s), saída em
`docs/economy/perf/f0_world_ai_365_coproducts.json`:

| Braço | Falências | Comida (<0,975) | Violações | Travados |
|---|---:|---:|---:|---:|
| **Com refino** (corrigido) | **3** | **94/179** | 0 | 0 |
| Sem refino (`--refining=0`) | 2 | 95/179 | 0 | 0 |
| Sem energia (`--power=0`, com refino) | 4 | 96/179 | 0 | 0 |

Saídas: `f0_world_ai_365_coproducts.json` (braço corrigido),
`f0_verify_365_noref.json` e `f0_verify_365_nopower.json`.

Falências do braço corrigido: `PRI-PON` cobre e ferro (dia 126, `loss_days=60`,
as duas do controle) e **`MLT-VAL` trigo (dia 155)**. Mediana da comida 0,9729;
drift de caixa 0,005504; trajetória `be98cd2f`.

A comida passa (**94 ≤ 95**) e os invariantes passam (0 violações, 0 travados).
O **portão de falências 3/179 foi ACEITO pelo dono em 23/11/2026** — 2 minas PRI-PON + a fazenda de trigo `MLT-VAL` por insolvência bancária.

### 12.4 Combustíveis (dia 1 → dia 365)

Preço médio por nação (média simples das nações com preço; petróleo incluso):

| Bem | Dia 1 | Dia 365 | × petróleo (dia 1 → 365) |
|---|---:|---:|---:|
| Petróleo | 45,05 | 48,01 | — |
| Gasolina | 81,92 | **104,17** | 1,82 → **2,17** |
| Diesel | 74,90 | **74,90** | 1,66 → **1,56** |
| Querosene | 78,22 | **78,22** | 1,74 → **1,63** |

O diesel e o querosene ficam no `base_price` derivado (rota principal) porque
não têm escassez de mercado (oferta 214,7 contra venda 22,1 t/dia; querosene
acumula 25 253 t em estoque). A gasolina sobe 27,2% no ano com a escassez de
demanda efetiva contra oferta.

Demanda efetiva ÷ desejo (mundo): gasolina **119,9 / 23 657,0** no dia 1
(0,51%) e **360,0 / 23 864,2** no dia 365 (**1,51%**); diesel **0 / 0** no dia 1
e **22,1 / 62,5** no fecho (**35,4%**); querosene **0 / 0** nos dois dias. O
desejo de gasolina continua ~66× a venda: o limitador é a **afordabilidade**,
não a oferta (333,1 t/dia ofertadas).

### 12.5 Refino semeado

| Métrica | Dia 1 | Dia 365 |
|---|---:|---:|
| Plantas semeadas | 122 | **122** (nenhuma fechou) |
| Capacidade (t/dia) | 1 460,1 | 1 791,5 |
| Utilização média | 0,229 | 0,186 |
| Receita / custos / lucro do setor (por dia) | 9 720,7 / 51 238,2 / −41 517,5 | 60 887,6 / 41 569,9 / **+19 317,6** |
| Plantas com lucro > 0 no fecho | — | **90/122** |

A receita do dia 1 passou de 9 425 (antes, §11.2) para **9 720,7** — efeito
conjunto do crédito dos co-produtos e dos novos preços-base; as seis refinarias
que falhavam (`IRQ-BAG`, `IRQ-ERB`,
`ZAF-CAP`, `ZAF-DUR`, `ARG-BUE`, `IND-MUM`) e a mina `usa_ren_oil_mine`
**sobrevivem** ao ano. O setor fecha no azul no agregado, com 32 plantas ainda
no vermelho (pior: `ARE-DUB`, −1 167/dia).

### 12.6 A falência restante e a conclusão

`MLT-VAL` **não** fecha por `loss_days` (o contador está em **0**): o banco
executa o grupo por insolvência (`bank.cpp`, `credit_default_days >=
default_days`). No fecho a fazenda está **parada** (receita = custos = lucro =
0, utilização 0) com capacidade de 3,684 t/dia (semente de 2,0 t/dia), numa
nação sem refinaria e com petróleo a 45,0. Ela **sobrevive sem o seed de
refino** (braço de 2 falências) e **também morre sem a camada de energia**
(dia 157), então o resíduo é **efeito de preço/mercado do seed**, não da
contabilidade de co-produtos nem da regra de produção sem venda.

As três correções resolveram **7 das 8** falências extras da §11.3 (as 6
refinarias e `usa_ren`); resta **1** (`MLT-VAL`). O **portão de falências 3/179 foi ACEITO pelo dono em 23/11/2026**
(2 minas PRI-PON + fazenda de trigo `MLT-VAL` por insolvência bancária); o
teto não foi movido.

Depois da correção de preço dos subprodutos (t145: `producers` volta à
saída principal; bran 5, sawdust 3,12, organic_fertilizer 24,17, meat
11,32, canned_food 31,06, combat_ration 143,01), a MESMA rodada mede
**5 falências** — as 3 acima + as minas de óleo USA `usa_ren` (dia 136)
e `usa_bil` (dia 167), ambas com receita 0 e utilização 15-17% — e
**comida 98/179** (0 violações). O controle roda com o `goods.json`
anterior reproduz 3/94 e a trajetória `be98cd2f`, então o desvio vem
da correção de DADOS, não do núcleo C++; decisão do dono pendente. Próxima medição
necessária: PL e dívida do grupo `national:G-MLT-food-157` ao longo do ano
(hoje só o contador da planta é publicado) para dizer se a insolvência vem da
folha atrasada na entressafra ou do crédito de safra.
