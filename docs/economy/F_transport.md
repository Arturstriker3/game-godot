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
