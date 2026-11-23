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
diesel **58,00** por tonelada — contra 43,50 do antigo `fuel`. O aumento é
esperado: a tonelada de combustível refinado agora carrega o custo do
petróleo/gás/carvão dividido pelas frações úteis (83% no caso da refinaria).

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
| Rodada 365 (`c1_world_ai_365.gd --control=c6_gate_base.json`) | ver §9 |

## 9. Rodada de 365 dias

Comando:

```powershell
& $godot --headless --path . --script res://tools/c1_world_ai_365.gd -- `
  --days=365 --out=res://docs/economy/perf/f0_world_ai_365.json `
  --control=res://docs/economy/perf/c6_gate_base.json
```

Medição e leitura dos portões (falências ≤ 2, comida ≤ 95/179, 0 violações):

| Métrica | Medido | Portão |
|---|---:|---|
| Dia final / travados | 365 / 0 | — |
| Violações de invariante | 0 | 0 |
| Falências | 2 (mesmas do controle: USA PRI-PON copper/iron, dia 126) | ≤ 2 |
| Comida (média 30 d, < 0,975) | **97/179** (mediana 0,9703; média 0,9058) | **≤ 95 — FALHOU** |
| Extras vs controle | 5 (PRK, ATG, KNA, AUS, KWT) | — |
| Drift de caixa | 0,567% | — |
| Parede | 335,2 s (`threads=1`, DLL debug) | — |

Saída: `docs/economy/perf/f0_world_ai_365.json` (trajectory `50278f4f`).

**Diagnóstico (não amolecido).** Contra a rodada D-122 (`c1_world_ai_365_renew.json`,
95/179, extras CIV/ATG/PRK/AUS), esta rodada mantém falências (2), violações (0) e
balanço praticamente idênticos (massa monetária 6,5606e9 × 6,5600e9; estoque das
firmas dentro de 0,03%), mas a lista do portão muda: **KNA e KWT entram** e **CIV
sai**. A média de consumo de comida do mundo **sobe** (0,9058 × 0,9051): não é
queda global, é redistribuição em torno de 0,975.

Os dois que entram são importadores puros (KNA 12,9 mil hab. e 1 prédio de comida
com saída 0; KWT 4,27 M hab. e 2 prédios com saída 0) e cruzam o portão já no dia
1, com preço de comida acima do ano-base (`food_price_ratio` 1,05 e 1,10). A
hipótese principal é o **preço/custo do frete**: o `fuel` genérico tinha receita
de fábrica com rendimento 1,0 a partir de petróleo; hoje a gasolina vem da
refinaria a 0,45 por lote e o diesel de gás/carvão a 0,30/0,15 — o diesel ficou
mais caro de produzir (58,00/t contra os 43,50 do `fuel` no frete), encarecendo a
carga de comida para quem não produz e depende de navio. Os preços de combustível
declarados (78,61/78,61/58,00) amplificam esse canal.

**Próximo passo proposto (não executado):** medir oferta de diesel (produção por
receita e estoque de mercado por nação) ao longo de 30–60 d e a taxa de construção
de refinarias pela IA; se a oferta de diesel é o gargalo, o caminho é dado de
produção/logística (ou a IA de obra), nunca o teto do portão.

**Resultado:** implementação funcional e testada; **portão de comida não aceito**
(97 > 95), aguardando decisão do dono/Game Systems Designer.

## 10. Pendências

* O portão de comida é sensível ao preço do combustível: a gasolina a 78,61
  (contra 43,50 do `fuel`) encarece o transporte doméstico. Se o número do
  portão piorar, o próximo passo é diagnosticar o consumo por nação (não
  amolecer o portão).
* `fuel.svg` continua em `assets/icons/resources/` (ícone do bem removido); a
  limpeza fica para o `icon_forge` quando o `resource_icons.json` não citar
  mais o id.
