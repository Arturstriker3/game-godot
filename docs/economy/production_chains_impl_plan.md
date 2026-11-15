# Plano de implementação — cadeias de produção (93 bens novos), necessidades, frota e câmbio

> **Documento de planejamento.** Nada aqui foi implementado. Nenhum arquivo de jogo, dado
> ou núcleo C++ foi alterado na elaboração deste plano. Fonte da especificação:
> `docs/economy/production_chains_spec.json` (lido integralmente: 93 bens, receitas
> alternativas com `yield`, byproducts, `use`, `durable`, `pop_needs` em 3 níveis,
> `consumption_rules`, `market`).
>
> Fontes de verdade usadas para o estado atual: `gdextension/src/economy_sim.h`,
> `gdextension/src/economy_sim.cpp`, `core/economy/*.gd`, `core/economy/ai/*.gd`,
> `core/ui/**`, `tools/icon_forge/*`, `data/definitions/**`, `GAME_STATE.md` §6/§8/§10/§13,
> `docs/economy/port_design.md`.

---

## 1. Objetivo

Levar o jogo de **21 bens primários com uma única cadeia viva (cesta de calorias + extração)**
para **114 bens com cadeia industrial real**, mantendo as regras já validadas (conservação de
dinheiro, invariantes, portos, frete, IA determinística, status double-buffered).

Escopo aprovado pelo usuário, em cinco frentes:

1. Todos os 93 bens novos no jogo — dados + simulação C++ (`EconomySim`).
2. Ícone para cada bem novo, pelo pipeline existente (Icon Forge).
3. Frete com **frota nacional** (`truck`, `cargo_ship`, `cargo_plane`) e **combustível**:
   sem frota/combustível a carga não sai; reaproveitar rotas, frete, portos (capacidade/fila/transbordo).
4. **Moeda própria por nação com câmbio**; preço entregue = origem + frete + tarifa convertido
   para a moeda do comprador; o comprador escolhe o **menor preço entregue** em camadas
   `local` / `regional` / `global`.
5. **POPs** consomem conforme `pop_needs` (consumíveis por mês, duráveis com reposição 10%/ano).

Restrição de escopo deste documento: só o plano. Nenhuma cena principal foi aberta, nada foi
commitado.

---

## 2. Estado atual (verificado no código)

### 2.1 Núcleo C++ — um modelo, dois mundos

`EconomySim` (`gdextension/src/economy_sim.h:30`) é a única classe exposta. Convive com dois
modelos, e **o jogo ao vivo usa só o segundo** (`core/economy/economy.gd:83`):

| Modelo | Entrada | Estado | Papel hoje |
|---|---|---|---|
| legacy | `initialize()` / `tick()` / `get_status()` (`economy_sim.cpp:188`, `:224`, `:351`) | `cities_`, `ports_`, `in_transit_`, `last_orders_` | **só benchmark/referência.** Comércio em 3 camadas (`MarketScope::Regional/National/International`, `h:246`) com `dispatch_trade` (`:404`), `build_route` (`:454`), `make_leg` (`:503`) |
| demo / mundo | `initialize_demo_world()` (`:1067`) + `demo_tick_core()` (`:2209`) + `get_demo_status()` (`:3433`) | `demo_cities_`, `demo_buildings_`, `demo_ports_`, `demo_in_transit_` | **o jogo.** Tick roda em `WorkerThreadPool` (`core/economy/tick_scheduler.gd`) |

Ponto importante: as **três camadas de mercado existem apenas no modelo legacy**. No mundo demo
todo pedido é gravado com `order.scope = MarketScope::International`
(`economy_sim.cpp:1838`); as "camadas" viram apenas o conjunto de candidatos (K vizinhos + M
ofertas mais baratas do globo, `:1519-1520`). **O requisito 4 precisa reintroduzir camadas no
modelo demo**, não no legacy.

### 2.2 Produtos são fixos em tempo de compilação

Isso condiciona **toda** a F2/F3/F5:

- `enum class DemoProduct` com 21 valores (`h:346-352`), `kDemoProducts = 21` (`h:354`),
  `kDemoProductNames[21]` (`cpp:866-870`).
- Estado por produto **embutido em arrays C de tamanho fixo**:
  - `DemoCity`: `revenue_by_product[21]`, `imported/exported/import_value/export_value[21]`,
    `suitability[21]`, `external_demand[21]`, `deposits[21]` (`h:484-499`), `markets[21]` (`h:501`);
  - `DemoPop`: `desired_by_category[5]`, `consumed_by_category[5]` (`h:432-433`);
  - globais: `demo_produced_[]`, `demo_consumed_total_[]`, `demo_produced_today_[]`,
    `demo_consumed_today_[]` (`h:880-883`).
- Bens de entrada são indexados **por nome**: `demo_product_index()` varre
  `kDemoProductNames` (`cpp:3712-3719`); `demo_set_goods()` lê `id`/`base_price` do JSON
  (`cpp:940-955`).
- Cesta alimentar é uma **tabela fixa de 5 slots** com fatores calóricos
  (`kDemoFoodProductIds` / `kDemoCaloricFactor`, `h:366-367`; `demo_food_product` /
  `demo_food_calories`, `cpp:3721-3732`).
- `kDemoFirstRawProduct = 1` (`cpp:812`) delimita a fase de exportação bruta; ela **pula
  alimentos via `demo_food_calories`** (`cpp:2542-2545`). Com 114 bens, esse filtro passaria a
  "exportar" os 93 novos por teto, sem mercado — precisa ser reescrito (ver §5.4).

### 2.3 Produção: um insumo, sem receita, sem subproduto

`DemoBuilding` (`h:381-408`): `input_product` (int, `-1` = extração), `input_per_output`
(toneladas de insumo por tonelada de produto), `output_product`, `capacity`, `jobs`, `wage`,
`opex`, `workers`, `utilization`, `output_scale`, `loss_days`, `bankrupt`, `depleted`.

Fase 4 do tick (`cpp:2416-2524`):

- `target = sold_prev + (sold_prev × inventory_cover_days − stock_before) × inventory_recovery`
  (estoque-alvo de 3 dias, recuperação 0,5 — `cpp:2470-2481`);
- `desired_output = target × capacity_share × labor`, limitado por
  `input_used = min(needed_input, input_market.stock)` (`cpp:2485-2494`);
- extração multiplica por aptidão × eficiência da jazida e debita a reserva
  (`cpp:2457-2513`);
- consumo de insumo paga preço de mercado e credita `revenue_by_product[input]`
  (`cpp:2499-2501`).

Limitações objetivas para a spec:

- **um** insumo por construção (a spec tem receitas de até **6** insumos — `car`);
- **sem alternativas** de receita (a spec tem 2–6 por bem);
- **sem subprodutos** (`bran`, `sawdust`);
- **sem conceito de "recipe"**: o edifício carrega o par insumo/produto, não a receita.

Criação/expansão: `demo_build(city, id, resource, input_product, output_product, capacity,
jobs, wage, opex, input_per_output, owner, cost)` (`cpp:3901`) e `demo_invest` (`cpp:3847`,
capital fixo `kDemoCapitalPerCapacity = 2000` t/dia, `cpp:830`).

**`industrialization`** é derivado, não simulado: `secondary_capacity / total_capacity`
(`cpp:3191-3208`), onde "secundário" = construção com `input_product >= 0`. Como **nenhuma
construção do cenário tem insumo** (`data/definitions/scenario/buildings.json` só tem fazendas
primárias: 834 registros, chaves `city_id,id,name,output_tons_per_day`; o `resource` é resolvido
por `GeographyService.resolve_initial_farms`, `geography_service.gd:625`), `industrialization`
é **0 em todas as cidades do mundo real**. É o "sair de 0" do requisito.

### 2.4 Trabalho é rateado por um único fator

Na fase 3 (`cpp:2354-2412`):

```
available = max(0, labor_force − service_jobs)
fill      = clamp(available / Σ jobs, 0, 1)     // aplicado a TODAS as construções
building.workers = building.jobs × fill
```

**Consequência crítica para F2:** abrir fábricas numa cidade **reduz proporcionalmente** a mão
de obra das fazendas (o `fill` cai) — industrializar sem realocar trabalho derruba a produção de
alimento. Este é o risco de balanço nº 1 (§9) e obriga uma decisão explícita em F2.

### 2.5 Consumo das POPs: uma necessidade, 5 alimentos substituíveis

Fase 5 (`cpp:2572-2700`):

- `pop.need = simulation_population × kDemoNeedPerCapita` (`0,0003` t-eq/dia, `cpp:727`);
- cesta com fatores calóricos (`1,0 / 1,0 / 1,0 / 0,25 / 1,3`);
- consumo **proporcional à disponibilidade local**, com orçamento limitado
  (`cpp:2644-2694`);
- subsídio de subsistência do governo quando o orçamento não cobre (`cpp:2624-2633`);
- `pop.satisfaction = consumed / need` (`cpp:2697`).

Não existe: segundo/terceiro nível de necessidade, duráveis, reposição, nem categoria fora de
alimento. `desired_by_category`/`consumed_by_category` são 5 posições fixas.

### 2.6 Comércio, frete e portos hoje

`demo_trade_food()` (`cpp:1323-1921`) — **só alimento**:

- compradores = cidades com `food_ge` abaixo de `need × inventory_cover_days`
  (`cpp:1379-1387`), ordenados por **cobertura relativa** (`cpp:1411-1416`);
- candidatos = K vizinhos por grade de ~250 km (`kDemoTradeCellMapUnits = 32`, `cpp:863`,
  raio limitado a 48 células com porto / 8 sem porto, `cpp:1602`) + M ofertas mais baratas por
  caloria do globo (`cpp:1669-1679`);
- custo unitário = `(price + unit_freight) / factor` (`cpp:1732`);
- frete = **distância em linha reta × tarifa fixa** — caminhão `0,10` t·km
  (`kDemoFreightPerTonKm`, `cpp:750`), navio `0,02` (`cpp:753`); trechos em `make_leg`
  (`cpp:503`) com `ModeSpec` caminhão 25 t/65 km/h, navio 2 000 t/35 km/h, avião 20 t/750 km/h
  (`cpp:167-186`);
- portos: capacidade/fila/transbordo em `demo_port_charge` (`cpp:2054`),
  `demo_init_port_capacities` (`cpp:1999`), `demo_expand_port` (`cpp:2070`); regra "porto
  nacional na mesma massa de terra" em `cpp:1426-1461`; 144 portos, 877 277 t/dia instalados
  (GAME_STATE §6);
- teto de embarques `kDemoMaxShipmentsPerDay = 512` (`cpp:852`); teto de carga por cidade
  `kDemoTradeCapacityPerCapita = 0,001` t/dia (`cpp:857`);
- pagamento: firma + governo + poupança das famílias (`cpp:1785-1813`).

**Não existe**: frota, combustível, capacidade de veículo por nação, moeda, câmbio, tarifa,
nem comércio inter-cidade de **bens não alimentares** (esses só saem por `external_demand`,
fase 4c, `cpp:2526-2570`).

### 2.7 Preços

Fase 8 (`cpp:2849-2921`): só cidade **com produtor local** recalcula preço; alvo =
`base × (1 + clamp(scarcity − 1, −0,5, 2))` com piso `cost_floor × 1,05` (custo do insumo +
mão de obra/opex por unidade de capacidade), suavização `0,20`, banda `[0,5×, 3×]` do preço-base.
Limite diário ±5% do preço-base aplicado **uma vez** em fase 9 (`cpp:2925-2944`).
Cidade sem produtor (importadora) mantém o preço — **o preço de importação não forma preço local**:
lacuna direta para o requisito "preço entregue" (F5).

### 2.8 Status e save (custos a vigiar)

- `get_demo_status()` usa **double buffer** (`demo_status_slots_[2]`, `h:891`) e reescreve
  dicionários no lugar. Cada cidade publica `markets` com **21 dicionários**
  (`cpp:3301-3364`) + 4 arrays de fluxo + `deposits` + `buildings` + `pop`.
- Números medidos (`docs/economy/perf/perf_858_release_after.txt`, 858 cidades):
  tick **30,14 ms** médio (p95 34,97), fase `status` **18,84 ms** (das quais `status_cities`
  **14,58 ms**), `trade` 4,93 ms, frame de pico 6,97 ms, memória estática **432,3 MB**.
  GAME_STATE §6 fala em ~41 MB só para o cache do status em 650 cidades.
- Save: `kDemoSaveVersion = 3` (`cpp:81`), `demo_to_dict` (`cpp:4007`) serializa **arrays por
  índice de produto**; migração de índice em `demo_migrate_product_index` /
  `demo_migrate_product_array` (`cpp:882-910`) — existe precedente de remoção de bens
  (farinha/pão), útil como molde para a mudança de 21→114.
- Não há serviço de save no jogo: `demo_to_dict`/`demo_from_dict` só são usados por testes e
  laboratório (`tools/economy_integration_test.gd:230`, `tools/economy_ai_test.gd:203`,
  `tools/economy_lab.gd:112`, `tools/world_scale_bench.gd:135`, `tools/port_capacity_test.gd:234`).
- **Benchmark não serve para o teste de escala de produtos**: `initialize_benchmark`
  (`cpp:4517`) modela `kBenchProducts = 1` (`h:626`). Medir 114 bens exige o caminho real
  (`tools/perf_day_peak.tscn`) ou estender o bench.

### 2.9 GDScript: dados, IA, UI

| Arquivo | Papel | Acoplamento ao "21" |
|---|---|---|
| `core/economy/economy.gd` | autoload: carrega `cities/ports/buildings/goods/nations`, chama `initialize_demo_world` → `demo_set_goods` → `GeographyService.apply_to_sim` → `demo_warm_status` (`:76-97`) | nenhum explícito; repassa arrays |
| `core/economy/geography_service.gd` | aptidão por cidade (6 culturas + madeira), depósitos (regra A1), teto de exportação = reserva/~25 anos, índice de renda externa (`:1338-1397`); decide a cultura da fazenda inicial e a terra (`:625-740`) | `apply_to_sim` declara **só** as 6 culturas + madeira + minerais com depósito |
| `core/economy/ai/industry_templates.gd` | `TEMPLATES` com **índices de produto hardcoded 0..20** (`:7-68`), listas `FOOD_RESOURCES`/`MINERAL_RESOURCES`/`RENEWABLE_RESOURCES` | **duplica o mapa de índices do C++** |
| `core/economy/ai/private_investor.gd` | gates de investimento: margem > 0,5/t, payback < 150 d (730 d para bulk), risco por variância de preço (`:169-358`) | idem |
| `core/economy/ai/government_ai.gd` | estatal de segurança alimentar + mineração estratégica | idem |
| `core/ui/product_icons.gd` | `RESOURCES` (21 ids) de `assets/icons/resources/`, `FILES` (8) de `assets/icons/products/`; `texture(id)`/`make(id,cor,tam)` | **lista manual de 21** |
| `core/ui/screens/{economy,production,trade}_screen.gd` | tabelas T2; Produção tem aba "Cadeia" que hoje lista **produtores de trigo** (`production_screen.gd:181-197`) | itera `markets` por índice |
| `core/ui/hud.gd` | usa `ProductIcons` (`:819`, `:1276`) | — |

### 2.10 Dados

- `data/definitions/economy/goods.json` — **21 itens** `{id,name,base_price,food}`.
- `data/definitions/economy/balance.json` — **duplica** `products[21]` + `base_prices[21]` e
  cenários do laboratório com arrays de 21 posições (`market_stock`, `market_price`).
- `data/definitions/scenario/buildings.json` — 834 fazendas; `cities.json` 834;
  `ports.json` 144; `nations.json` 179 — **já tem `currency`** (ex.: AFG → `AFN`), sem taxa.
- i18n (`data/definitions/i18n/i18n.csv`): existe `product.wheat` e `resource.*` dos 21;
  **não há** chaves para os 93 novos.
- `data/definitions/natural/*` — aptidão agrícola (9 culturas) e 5.281 depósitos; **não cobre
  os insumos industriais novos** (não precisa: eles derivam de receita).

### 2.11 Pipeline de ícones (estado real, corrigindo a premissa)

`tools/icon_forge` **está versionado** (`git ls-files` → `README.md`, `icon_forge.py`,
`resource_icons.json`, `variants.json`). O que **não** está versionado são arquivos de estudo
em andamento: `natural_preview.py`, `style_preview.py`, `serious_preview.py`,
`raw_symbol_options.json`, `tier_examples.json` (`git status` → `??`).

- Manifesto: `tools/icon_forge/resource_icons.json` → `{"style":{}, "icons": {id: {label,
  color, layers[]}}}`; **21 ícones**; `tier` opcional (`raw` padrão, `semi`, `final`) define a
  moldura do estilo colorido (`README.md` § Estilo colorido).
- CLI (`icon_forge.py`): `check` (valida regras), `build --out assets/icons/resources`
  (SVGs brancos 24×24, tingidos por `modulate`), `sheet --out docs/ui/icons_sheet.html`,
  `new <id>`, `styled --out assets/icons/styled`.
- Saída atual: 21 `.svg` em `assets/icons/resources/` (+ `.import` gerados pelo Godot).
- Regra do projeto (AGENTS.md): ícone **não** se desenha à mão no SVG nem se baixa — descreve-se
  em `resource_icons.json` e roda-se `check`/`build`.

### 2.12 Testes existentes relevantes

| Teste | Runner | Toca autoload? | Uso no plano |
|---|---|---|---|
| `tools/economy_integration_test.gd` | `--script` (`extends SceneTree`) | não (instancia classes) | valida geografia, cadeia, investimento, save/load; **afirma 21 produtos** (`:100`, `:286`) |
| `tools/economy_ai_test.gd` | `--script` | não | IA + save/load |
| `tools/population_demo_test.gd` | `--script` | não | conservação + choque de preço |
| `tools/economy_world_test.gd` | `--script` | não | 60 dias no cenário real, invariantes, sem NaN |
| `tools/economy_long_run_test.gd` | `--script` | não | lento (730 d) |
| `tools/port_capacity_test.gd` | `--script` | não | capacidade/fila/expansão |
| `tools/economy_tick_async_test.gd` | `--script` | parcial | tick em thread |
| `tools/perf_day_peak.tscn`, `tools/ui_capture.tscn`, `tools/map_capture.tscn`,
  `tools/convoy_lifecycle_test.tscn`, `tools/vehicle_spacing_capture.tscn`,
  `tools/ui_template_gallery.tscn` | **cena** | **sim** (`Economy`, `I18n`) | perf real, captura de UI |
| `tools/menu_smoke_test.tscn`, `tools/nation_city_list_test` etc. | cena / script | depende | smoke |

**Regra para os testes novos deste plano:** teste que só instancia `EconomySim`/serviços →
`--script`; teste que lê `Economy.sim`, `Economy.last_summary` ou `I18n` como autoload →
**cena `.tscn`**, nunca `--script`. `ui_capture.tscn` roda com janela (não é headless).

---

## 3. Lacunas spec → código (mapa curto)

| Item da spec | Hoje | Onde muda |
|---|---|---|
| 93 bens + 21 primários | 21 fixos em enum/array | `economy_sim.h/.cpp` (F2) + `goods.json` (F1) |
| `recipes[]` com 2–6 alternativas e `yield` | um `input_product` + `input_per_output` | `recipes.json` (F1) + `DemoBuilding`/fase 4 (F2) |
| `byproducts[]` (`bran`, `sawdust`) | inexistente | fase 4 (F2) |
| `durable` + reposição 10%/ano | inexistente | `DemoPop` + fase 5 (F3) |
| `pop_needs` basic/life/luxury com substituição | 1 nível, 5 alimentos | `needs.json` (F1) + fase 5 (F3) |
| `consumption_rules` (construção, porto, minas, exército) | parcial/sem dados | F2 (insumos de construção/porto) e F3/F4 |
| frete exige frota + fuel | frete = distância × tarifa, sem veículo físico | F4 |
| moeda por nação + câmbio | unidade nominal única | `currencies.json` (F1) + F5 |
| preço entregue + camadas | só preço de produtor; camada fixa "International" | F5 |
| ícones para 93 bens | 21 + 8 | F1 |

---

## 4. Schema de dados proposto

Princípio: **o C++ não lê arquivos**. Todo dado novo entra por um setter no núcleo, no mesmo
padrão de `demo_set_goods` (`cpp:940`) e `demo_set_balance` (`cpp:3751`), chamado por
`core/economy/economy.gd` logo depois de `initialize_demo_world`. Assim o balanceamento continua
editável sem recompilar (GAME_STATE §6 "Bens data-driven").

### 4.1 `data/definitions/economy/goods.json` (expandido, 114 itens)

```json
{
  "id": "flour",
  "name": "Farinha",
  "base_price": 47.0,
  "base_price_source": "derived:v1",
  "sector": "food",
  "kind": "intermediate",
  "durable": false,
  "tier": "semi",
  "icon_color": "#E0CBA8",
  "food": false,
  "equivalent": { "unit": "t", "per_ton": 1.0 },
  "order": 21
}
```

Decisões de schema:

- **`order` explícito** (0..113) — o índice do produto é o contrato do C++, do status e do save.
  Mantém os 21 primários **nas posições 0..20, na ordem de `goods.json` de HOJE** (`wheat, iron,
  rice, maize, bauxite, lead, gold, phosphate, tungsten, rare_earths, sulfur, diamond, coal,
  copper, gas, oil, potato, soy, cotton, wood, uranium`), **não** na ordem do array `primaries`
  da spec (que começa `wheat, rice, maize, potato, soy…`). O enum `DemoProduct`
  (`economy_sim.h:346-352`), `kDemoProductNames` (`economy_sim.cpp:866-870`), `balance.json`,
  `industry_templates.gd` (índices hardcoded) e os testes existentes dependem dessa ordem. Os 93
  novos entram em 21..113.
- **`tier`** (raw/semi/final) alimenta o Icon Forge e a moldura do ícone.
- **`icon_color`** já existe hoje dentro de `resource_icons.json`; aqui é a fonte única e o
  gerador de ícones lê daqui (o manifesto passa a ser derivado, não editado em dois lugares).
- **`base_price` derivado, com procedência declarada.** A spec **não traz preços**. Regra
  proposta (documentada e reproduzível, nada "inventado no olho"):

  ```
  custo_insumos(recipe) = Σ qty_i × base_price(good_i) / yield
  base_price(good)      = min over recipes( custo_insumos ) × (1 + margem[kind] )
  margem: raw 0 · byproduct 0.10 · intermediate 0.25 · final 0.45 · durable/luxury 0.80
  ```

  Primários mantêm o preço atual (fonte: `goods.json` de hoje, dado já calibrado). O script
  `tools/build_goods_chain.py` calcula em ordem topológica do grafo de receitas e escreve
  `base_price` + `base_price_source`; `check` falha se houver ciclo, bem sem receita e sem
  primário, ou `base_price <= 0`. Valor derivado é **estimativa**, e o documento/relatório
  diz isso — nunca é vendido como dado real.

### 4.2 `data/definitions/economy/recipes.json` (novo)

A spec lista `inputs: ["steel","mechanical_parts","wires","cast_parts"]` e `yield: 1.0` **sem
quantidades**. Isso é ambíguo e precisa de uma normalização explícita:

```json
{
  "id": "engine_a",
  "building": "factory",
  "sector": "machinery",
  "output": "engine",
  "yield": 1.0,
  "inputs": [
    { "good": "steel",         "qty": 1.0 },
    { "good": "mechanical_parts", "qty": 1.0 },
    { "good": "wires",         "qty": 1.0 },
    { "good": "cast_parts",    "qty": 1.0 }
  ],
  "byproducts": [],
  "jobs_per_ton": 4.0,
  "wage": 1.8,
  "opex_per_ton": 12.0,
  "capital_per_capacity": 6000.0,
  "note": "coeficiente unitário padrão (spec sem qty)"
}
```

- **`yield`** = toneladas de saída por "lote" definido pela lista de insumos. Receitas de um
  insumo só (`flour: wheat → 1.0`, `bran → feed 0.8`) ficam idênticas à leitura natural
  "t saída / t entrada".
- **Multi-insumo**: coeficiente `1.0` em cada insumo é o **default declarado** (não um número
  inventado escondido) e o script marca essas receitas com `"coef_source": "default_unit"` para
  revisão humana. `charcoal` (nota "produz o MESMO item coal") é modelado como receita com
  `output: "coal"`.
- `jobs_per_ton`/`wage`/`opex_per_ton`/`capital_per_capacity` seguem a ordem de grandeza já
  usada em `industry_templates.gd` (fazenda 7,5 empregos/t, salário 2,0, opex 2,5/t; minas por
  intensidade — `industry_templates.gd:7-96`) e são calibrados no laboratório, não na spec.
- `build_goods_chain.py check` valida: todo `good` existe, **todo bem não-primário tem ≥1 receita
  ou é subproduto declarado de outra receita** (caso real: `bran` e `sawdust`, que a spec declara
  `kind: "byproduct"` com `recipes: []`), nenhum ciclo, `yield > 0`, subproduto não é insumo de si
  mesmo.
- Inventário da spec, verificado: **93 bens**, no máximo **6 receitas alternativas por bem**
  (`feed`, 6) e **6 insumos por receita** (`car`, 6); **2 bens com subproduto** (`flour → bran`,
  `planks → sawdust`); **17 bens duráveis** (`furniture`, `luxury_furniture`, `tractor`,
  `mining_machine`, `robots`, `industrial_machinery`, `truck`, `car`, `cargo_ship`, `cargo_plane`,
  `appliances`, `computer`, `phone`, `drone`, `jewelry`, `small_arms`, `artillery`).

### 4.3 `data/definitions/economy/needs.json` (novo)

```json
{
  "levels": {
    "basic":   { "priority": 1, "satisfaction_weight": 0.60, "categories": {
      "food":     { "substitutes": ["bread","meat","canned_food","bagged_rice","wheat","rice","maize","potato","soy"],
                    "per_capita_t_month": 0.009, "anchor": "existing_food_need" },
      "clothing": { "substitutes": ["clothes"], "per_capita_t_month": 0.0004, "anchor": "estimated" } } },
    "life":    { "priority": 2, "satisfaction_weight": 0.30, "categories": { "...": {} } },
    "luxury":  { "priority": 3, "satisfaction_weight": 0.10, "categories": { "...": {} } }
  },
  "durable_replacement_per_year": 0.1,
  "equivalence": { "meat": { "protein": 1.0 }, "canned_food": { "protein": 0.8 } }
}
```

Honestidade de dados, explicitada no arquivo:

- **Alimento tem âncora física real** no código: `kDemoNeedPerCapita = 0,0003` t-eq/dia
  (`cpp:727`) = `0,009` t-eq/mês. `per_capita_t_month` do nível `basic/food` **deriva** desse
  valor já calibrado (não é número novo).
- **As demais categorias não têm âncora no código.** Duas opções, a decidir com o dono do jogo:
  (a) declarar `anchor: "estimated"` e calibrar por **participação no orçamento**
  (`share_of_disposable`), começando por ordens de grandeza e ajustando no laboratório; ou
  (b) deixar a categoria sem consumo até existir dado, com **estado vazio honesto** na UI.
  Recomendação: (a) com `anchor: "estimated"` visível no arquivo e no relatório; nunca apresentar
  como medição.
- **Substituição** usa `equivalence` (t-eq por tonelada), generalizando
  `demo_food_calories` — o mesmo mecanismo que hoje converte batata em 0,25 t-eq.

### 4.4 `data/definitions/economy/currencies.json` (novo)

`nations.json` já tem `currency` (179 nações). Falta a taxa:

```json
{ "version": 1, "anchor": "XGS",
  "currencies": [
    { "code": "USD", "nations": ["USA","ECU","SLV"], "rate_per_anchor": 1.0, "source": "anchor" },
    { "code": "AFN", "nations": ["AFG"], "rate_per_anchor": 0.014, "source": "derived:gdp_ppp" }
  ] }
```

- **`source` obrigatório.** `anchor` é a moeda interna da simulação (1,0). Taxas iniciais
  derivadas do **PIB PPP per capita já carregado** (`nations.json: gdp_ppp_bn/population`, o
  mesmo insumo de `external_income_index`, `geography_service.gd:1338-1359`) como proxy de nível
  de preços, com fórmula documentada e `check` que proíbe `rate <= 0`. Alternativa mais fiel:
  taxas reais publicadas (com citação e data) — melhor para o jogo, custo maior de curadoria;
  decisão aberta (§10).
- Regime cambial (fixo/flutuante) é dado, não código: `regime: "peg" | "float"`, com o núcleo
  aplicando drift só se `float`.
- **Histórico e perfis (só dados, 2026-11-15):** `data/definitions/economy/fx_history/<CODE>.json`
  (136 moedas, 9 101 pontos anuais, 1925–2025) e `data/definitions/economy/fx_profiles.json`
  (volatilidade, p95, crise >25%/ano, meia-vida, regime de fato IRR 1940–2016 e `confidence`).
  `currencies.json` continua com `regime: null` — nenhum código foi alterado. Detalhes em
  `docs/economy/fx_history.md`.

### 4.5 i18n

`data/definitions/i18n/i18n.csv`: **93 × 3** entradas novas (`product.<id>` / `resource.<id>`),
nomes vindos do `name` pt-BR da spec + traduções en/es. Verificação em
`tools/validate_scenario_data.py` (já existe para cidades/portos) estendida a produtos.
Nenhuma string de UI é hardcoded (regra do projeto).

### 4.6 Ícones (derivados, não desenhados à mão)

- `tools/build_goods_chain.py` gera `tools/icon_forge/resource_icons.json` a partir de
  `goods.json` (id, label pt-BR, `color` = `icon_color`, `tier`), preservando os 21 existentes.
- **As 93 silhuetas são trabalho manual de descrição** (primitivas `path/circle/rect/polygon`,
  `README.md` § Como desenhar), em lotes por setor, com revisão por `sheet`. Isso é o item mais
  custoso de F1 — ver §6.
- `core/ui/product_icons.gd`: `RESOURCES` deixa de ser lista manual e passa a ser derivada do
  `goods.json` (leitura única no `_init`/`static func`), com `FILES` mantendo os 8 ícones de UI.

### 4.7 `data/definitions/economy/balance.json`

`products`/`base_prices` passam a ser gerados de `goods.json` (mesma ordem), e os cenários com
arrays de 21 posições (`market_stock`, `market_price`) ganham um campo `by_product` nomeado
para não quebrar quando a lista cresce. `model` ganha as chaves novas de frota/FX quando F4/F5
chegarem.

---

## 5. Contrato C++ proposto (o que muda e o que **não** muda)

### 5.1 Contagem de produtos

- `kDemoProducts` deixa de ser `constexpr = 21` e passa a **variável de instância**
  (`int demo_products_`, teto `kDemoMaxProducts = 160`) preenchida por
  `demo_set_goods(Array)`; `kDemoProductNames[21]` vira `std::vector<std::string>`
  `demo_product_names_` + `std::unordered_map<std::string,int> demo_product_lookup_`
  (O(1) no lugar da varredura de `demo_product_index`, `cpp:3712`).
- **Alternativa rejeitada (por ora):** manter `constexpr 114`. Menos código agora, mas o
  balanceamento volta a exigir recompilação e o bench/lab perde a capacidade de rodar subconjuntos.
  Recomendação: instância, com os arrays por produto passando a `std::vector<double>`
  dimensionado em `initialize_demo_world`.
- Custo de memória estimado por cidade (arrays por produto):

  | Campo | 21 bens | 114 bens |
  |---|---:|---:|
  | `markets[21]` (`DemoMarket` = 8 doubles) | 1,34 KB | **7,30 KB** |
  | `deposits[21]` (`DemoDeposit` ≈ 7 doubles + 3 bool, alinhado) | 1,0 KB | 5,5 KB |
  | 7 arrays de double (`revenue_by_product`, `imported`, `exported`, `import_value`, `export_value`, `suitability`, `external_demand`) | 1,18 KB | **6,38 KB** |
  | **total/cidade** | ~3,5 KB | **~19,2 KB** |
  | × 834 cidades | ~2,9 MB | **~16 MB** |

  Aceitável no estado da simulação; o risco real é o **cache do status** (§8.2).

### 5.2 Receitas

`DemoRecipe` (novo, tabela do mundo, não por cidade):

```cpp
struct DemoRecipe {
    int output = -1;
    double yield = 1.0;
    int input_count = 0;
    int inputs[kDemoMaxRecipeInputs];      // 6 = máximo da spec (receita de `car`)
    double input_qty[kDemoMaxRecipeInputs];
    int byproduct_count = 0;
    int byproducts[kDemoMaxRecipeInputs];
    double byproduct_qty[kDemoMaxRecipeInputs];
    double capital_per_capacity = 2000.0;
};
```

`DemoBuilding` ganha `int recipe = -1;` e **mantém** `input_product`/`input_per_output` para
as construções primárias (compatibilidade do save e do status atual). Regra de escolha da
alternativa (a decisão econômica que a spec deixa implícita): **custo unitário corrente mais
baixo** entre as alternativas cujos insumos estão disponíveis na cidade; empate → primeira
receita declarada. Determinístico e explicável no log da IA.

Produção com N insumos (fase 4), com o lote definido pela lista de insumos:

```
lote    = desired_output / yield                       // nº de lotes para a saída desejada
escala  = clamp( min_i( stock_i / (qty_i × lote) ), 0, 1 )   // insumo mais escasso
output  = lote × escala × yield
consumo_i        = qty_i × lote × escala               // debitado de markets[i].stock
subproduto_j     = byprod_qty_j × lote × escala        // creditado no mercado do subproduto
```

`desired_output` continua vindo de `sold_prev` + estoque-alvo (`cpp:2470-2481`) e do
`capacity_share × labor`; só a conversão insumo→produto deixa de ser um único
`input_per_output`. Subproduto entra em `supply`/`stock` do seu mercado e é vendável como
qualquer bem (é o que permite `bran → feed` e `sawdust → cellulose`).

### 5.3 Força de trabalho (bloqueador de F2)

O `fill` único (§2.4) precisa virar alocação explícita. Proposta mínima e verificável:
duas classes de construção — **primária (extração/fazenda)** e **secundária (fábrica)** — com
prioridade configurável (`demo_labor_priority_secondary = 1.0` no laboratório), dividindo
`available` entre classes por um fator determinístico. Sem isso, cada fábrica nova derruba a
colheita e o cenário real perde segurança alimentar — exatamente o tipo de regressão que
`economy_world_test` (60 d) e `economy_long_run_test` (730 d) pegam.

### 5.4 Comércio e exportação bruta

- Fase 4c deixa de usar `demo_food_calories` como filtro e passa a exportar **somente bens
  cujo `external_demand > 0` e que não sejam insumo de nenhuma construção ativa no mundo**
  (regra explícita, não "não é comida"). Os 93 novos com receita ficam fora da venda externa.
- `demo_trade_food()` evolui para `demo_trade_goods()`: demanda por cidade = necessidades das
  POPs (F3) + insumos das construções em operação; **por produto demandado** roda a busca de
  candidatos (K vizinhos + M externos), com teto por produto e global
  (`kDemoMaxShipmentsPerDay`). Isso evita o custo O(cidades × 114 × candidatos).

### 5.5 Frota e combustível (F4)

```cpp
struct Fleet { int trucks; int ships; int planes; double fuel_stock; double fuel_used_today; };
```

Por nação (`demo_fleets_`). Regras:

- rota terrestre exige `trucks ≥ ceil(tons / 25)`; marítima exige `ships ≥ ceil(tons / 2000)`
  (capacidade do `ModeSpec`, `cpp:167-186`); avião idem com 20 t;
- `fuel` consumido por t·km e por modal (coeficientes em `balance.json`, derivados da tarifa:
  hoje a tarifa é o único preço de frete; o combustível vira um **insumo físico**, não um
  segundo preço);
- sem frota ou sem combustível → **nenhum pedido é criado**, com motivo em `order.reason`/log
  (a UI mostra "sem frota" / "sem combustível", estado vazio honesto);
- frota é construída pela produção de `truck`/`cargo_ship`/`cargo_plane` (bens `final` da
  spec, hoje com 0 construções) e se desgasta por uso (`spare_parts` por mês, conforme
  `consumption_rules`);
- portos (capacidade/fila/transbordo/demurrage) **não mudam**: continuam cobrando sobre a carga
  da rota marítima.

### 5.6 Moeda e preço entregue (F5)

Decisão de arquitetura com impacto no invariante de dinheiro (`demo_check_invariants`,
`cpp:1269`):

- **Recomendado:** todos os caixas continuam na **unidade-âncora interna** (como hoje). Moeda e
  câmbio vivem na **camada de decisão de compra**: `delivered = (origin + freight + tariff) ×
  rate(seller→anchor) / rate(buyer→anchor)`; o comprador compara em **sua** moeda; a liquidação
  usa o valor-âncora (conservação intacta, `money_total` continua escalar).
- **Rejeitado:** caixa por moeda nacional. Quebra o invariante escalar, exige um vetor de
  moedas no snapshot/rollback (`DemoSnapshot`, `h:521`) e multiplica o custo do save.
- `tarifa`/`subsídio`/`cota`/`embargo` entram como dados por par de nações (default 0/nenhum);
  tarifa é paga pelo comprador ao **próprio governo** (dinheiro conservado).
- Camadas `local` (mesma cidade) / `regional` (mesma nação ou raio `kRegionalRadiusKm = 500`,
  `cpp:84`) / `global` passam a ser escopo real no comércio demo, com `order.scope` refletindo
  o que aconteceu (hoje é sempre `International`, `cpp:1838`).
- **Passo cambial (fase futura):** a taxa deixa de ser fixa quando a nação declarar regime
  `float`/`band`. O tamanho do passo e a chance de crise **não são inventados**: vêm de
  `data/definitions/economy/fx_profiles.json` (histórico 1925–2025 em
  `data/definitions/economy/fx_history/`, ver `docs/economy/fx_history.md`):
  `volatility_annual_log/√365` ≈ passo diário típico, `crisis_frequency` (ex.: BRL 0,45;
  ARS 0,46; CHF 0,01) = chance anual de evento extremo, `max_1y_depreciation_log` = teto do
  evento e `mean_reversion_half_life_years` = velocidade de retorno (hoje `currencies.json` não
  declara regime, então o mundo real segue com taxa fixa e bit-idêntico ao XGS).
  Moeda com perfil `estimated`/`no_data` (`KPW`, `SLS`) não pode receber passo próprio.

---

## 6. Fases

Cada fase é **independente e verificável**: build verde, testes da fase verdes, `GAME_STATE.md`
atualizado. Nenhuma fase depende de abrir a cena principal.

### F1 — Dados, receitas, necessidades, moedas e ícones

**Objetivo:** todo o conteúdo novo existe como dado validado; o jogo ainda roda exatamente como
hoje (21 bens), porque nada disso é consumido pelo núcleo.

**Arquivos:**

- novos: `data/definitions/economy/recipes.json`, `needs.json`, `currencies.json`,
  `tools/build_goods_chain.py`, `tools/goods_data_test.gd`;
- alterados: `data/definitions/economy/goods.json` (114), `data/definitions/economy/balance.json`,
  `data/definitions/i18n/i18n.csv` (+93×3), `tools/icon_forge/resource_icons.json` (+93),
  `core/ui/product_icons.gd` (lista derivada), `assets/icons/resources/*.svg` (gerados).

**Entregável extra:** `docs/economy/production_chains_data.md` com a tabela derivada de preços,
as receitas normalizadas e a marcação `estimated`/`derived` de cada número.

**Critério de aceite:**

- `python tools/build_goods_chain.py check` → 0 problemas: 114 ids únicos, ordem 0..113 estável
  com os 21 primários em 0..20, nenhum insumo/uso/subproduto órfão, sem ciclo, todo bem com
  receita, primário ou subproduto declarado (`bran`, `sawdust`);
- `python tools/icon_forge/icon_forge.py check` → 0 problemas; `build` gera 114 SVGs;
- toda chave i18n × 3 idiomas; `goods_data_test` verde;
- jogo roda com o binário atual, sem nenhum aviso novo.

**Teste headless:** `& $godot --headless --path . --script res://tools/goods_data_test.gd`
(instancia só `FileAccess`/`JSON`, sem autoload → `--script` é correto). Import dos SVGs novos
exige **uma** passada do editor (import automático) — registrar isso como passo manual.

**Custo/risco:** o gargalo é desenhar 93 silhuetas legíveis em 16 px. Recomendação: entregar em
**5 lotes por setor** (food/agro, textile/wood, chemical, metal/machinery, transport/electronics/
military/luxury/packaging), cada lote com `sheet` + revisão visual antes do próximo. Não bloquear
F2 esperando os 93: F2 depende de `goods.json`, não dos ícones.

### F2 — Receitas e fábricas no C++ (industrialização sai de 0)

**Objetivo:** 114 bens vivos; construções com 1..6 insumos, alternativas e subprodutos;
`industrialization > 0` no cenário real sem perder segurança alimentar.

**Arquivos:** `gdextension/src/economy_sim.h`, `economy_sim.cpp`, `core/economy/economy.gd`
(carregar e repassar `recipes.json`), `core/economy/ai/industry_templates.gd` (ler receitas em
vez dos índices hardcoded), `core/economy/ai/private_investor.gd`,
`core/economy/ai/government_ai.gd`, `data/definitions/scenario/buildings.json` **só se** houver
decisão de semear fábricas.

**Mudanças-chave:** §5.1 (contagem variável), §5.2 (receitas/subprodutos), §5.3 (alocação de
trabalho), §5.4 (venda externa), `demo_set_recipes(Array)`, `demo_build(..., recipe_id, ...)`,
status (`buildings` ganha `recipe`, `inputs[]`, `byproducts[]`), save (v4 + migração 21→114 por
`order`), `demo_fill_city_status` dimensionado por `demo_products_`.

**Critério de aceite:**

- cenário real: após 180 dias, ≥ 1 cidade com `industrialization > 0` e ≥ 1 cadeia de 3 níveis
  operando (ex.: `wheat → flour(+bran) → bread`), com `money_total` conservado e 0 violações de
  invariante;
- produção de alimento (satisfação média) não cai mais de 2 p.p. vs. baseline de 180 d — protege
  contra o efeito §5.3; medir com `tools/economy_rebalance_scan.gd` (já existe) antes/depois e
  registrar em `docs/economy/`;
- subproduto aparece em `produced_today` e é consumível (ex.: `bran → feed`);
- receita alternativa escolhida pelo menor custo corrente, determinístico.

**Testes:**

- `& $godot --headless --path . --script res://tools/economy_integration_test.gd` (atualizar
  `21` → contagem nova; hoje falha em `:100`/`:286`);
- `& $godot --headless --path . --script res://tools/economy_world_test.gd` (60 d, invariantes);
- `& $godot --headless --path . --script res://tools/economy_ai_test.gd` (IA + save/load v4);
- `& $godot --headless --path . --script res://tools/population_demo_test.gd` (regressão do
  cenário AGRIA/PORTUS, que perde as suposições de 21 posições);
- **novo** `tools/production_chain_test.gd` (`--script`): cria moinho+padaria numa cidade de
  teste, alimenta trigo, confere `flour`, `bran`, `bread`, consumo ≤ estoque, sem estoque
  negativo, e troca de receita alternativa quando o insumo escasseia;
- build obrigatório antes de tudo: `cmake -S gdextension -B gdextension/build-release
  -DGODOTCPP_DIR=... && cmake --build gdextension/build-release` (DLL em `gdextension/bin`, ver
  `gdextension/economy.gdextension`).

**Custo/risco:** é a fase mais cara. O `DemoCity`/status/save/bench tocam arrays por produto em
~40 pontos (`kDemoProducts` aparece em todo o `demo_tick_core` e no cache). Estratégia: fazer
**primeiro** a troca 21→variável com os 21 bens apenas (nenhuma regra nova), rodar a suíte
econômica e só então adicionar receitas/subprodutos.

### F3 — Necessidades das POPs com substituição

**Objetivo:** 3 níveis (basic/life/luxury), substituição por `equivalence`, duráveis com
reposição de 10%/ano, satisfação por nível.

**Arquivos:** `gdextension/src/economy_sim.{h,cpp}` (`DemoPop` por categoria, fase 5),
`core/economy/economy.gd`, `data/definitions/economy/needs.json`,
`core/ui/screens/population_screen.gd` + `core/ui/city_panel.gd` (exibição dos níveis),
`data/definitions/i18n/i18n.csv`.

**Mudanças:** `DemoPop` com `std::vector<Category>` (id, substitutes, want, got, spend,
durable_stock), orçamento por prioridade (basic → life → luxury), `need`/`satisfaction`
agregados com pesos, `equivalence` substituindo `kDemoFoodProductIds`/`kDemoCaloricFactor`
(`h:366-367`), reposição `durable_stock × 0.1/365` por dia devolvida ao mercado como sucata
(estoque de bens duráveis usados) — ou descartada, a decidir (§10).

**Critério de aceite:** substituição determinística (só `bread` → só `rice` mantém satisfação
de `basic/food`); duráveis acumulam e decaem exatamente 10% em 365 d simulados; categoria sem
dado mostra **estado vazio**, nunca 100%.

**Testes:** `population_demo_test.gd` estendido (`--script`); **novo**
`tools/pop_needs_test.gd` (`--script`): 1 cidade, 365 d, 3 níveis, decaimento de duráveis e
substituição.

### F4 — Frota + combustível no frete

**Objetivo:** carga só sai com veículo e combustível; frota é bem econômico (produzida por
fábricas), não um número mágico.

**Arquivos:** `economy_sim.{h,cpp}` (Fleet, `demo_set_fleet`, consumo no `demo_trade_goods`),
`core/economy/economy.gd`, `core/economy/trade_convoys.gd` (mostrar motivo "sem frota"),
`core/ui/screens/trade_screen.gd` (frota/combustível), i18n, `balance.json` (coeficientes).

**Critério de aceite:** sem `trucks` → nenhum pedido terrestre criado, motivo registrado; com
1 caminhão e combustível → pedido criado e entregue; combustível insuficiente → pedido não sai
(ou sai proporcional, a decidir); portos (capacidade/fila/demurrage) com o mesmo comportamento
atual — `port_capacity_test` continua verde.

**Testes:** **novo** `tools/freight_fleet_test.gd` (`--script`, cenário AGRIA/PORTUS):
frota zero → 0 pedidos; frota 1 → pedido; combustível zerado → 0 pedidos;
`& $godot --headless --path . --script res://tools/port_capacity_test.gd` (regressão).

### F5 — Moedas, câmbio e preço entregue

**Objetivo:** preço entregue = origem + frete + tarifa, convertido para a moeda do comprador;
comprador escolhe o menor; camadas `local`/`regional`/`global` reais.

**Arquivos:** `economy_sim.{h,cpp}` (`demo_set_currencies`, `delivered_price`, escopo no
`demo_trade_goods`, `order.scope` real), `data/definitions/economy/currencies.json`,
`core/economy/economy.gd`, `core/ui/screens/trade_screen.gd` (origem/frete/tarifa/câmbio/
entregue), `core/economy/ai/*` (a IA decide por preço entregue, não local), i18n.

**Critério de aceite:** status publica `delivered_price`, `origin_price`, `freight`, `tariff`,
`fx_rate` por pedido; teste determinístico em que mudar a taxa **inverte** o vendedor escolhido;
`money_total` continua conservado; tarifa entra no orçamento do governo comprador.

**Testes:** **novo** `tools/currency_trade_test.gd` (`--script`): 2 nações, 2 moedas, 2 vendedores;
inverte a taxa e confere a troca de fornecedor; `economy_world_test.gd` como regressão.

### F6 — UI (mercado, cadeia, ícones)

**Objetivo:** o jogador vê os 114 bens com ícone, cadeia de produção real e preço entregue.

**Arquivos:** `core/ui/screens/economy_screen.gd` (aba Mercado: tabela por bem com filtro por
setor, busca, estado vazio honesto), `production_screen.gd` (aba Cadeia deixa de listar
"produtores de trigo" e passa a mostrar insumos → produto → subprodutos de uma cadeia real),
`trade_screen.gd` (colunas de preço entregue/moeda/frota), `product_icons.gd`, `hud.gd`
(legenda de recursos), `data/ui/templates/*.json`, i18n.

**Critério de aceite:** as três telas mostram bens reais com ícone e sem overflow em
1920×1080/1600×900/1366×768/2560×1440; troca de idioma não quebra layout; termos sem dado
aparecem como estado vazio ("sem comércio registrado"), nunca zero fabricado.

**Testes:** `& $godot --path . res://tools/ui_capture.tscn -- --phase=f4 --run=final
--sizes=1920x1080,1600x900,1366x768,2560x1440` (**roda com janela**) + `layout_report.json`;
`production_template_test.gd` / `trade_template_test.gd` / `statistics_template_test.gd`
(`--script`, instanciam o HUD manualmente). **Não** abrir a cena principal.

---

## 7. Ordem, dependências e checkpoints

```
F1 dados/ícones ──► F2 receitas no C++ ──► F3 necessidades ──► F4 frota+fuel ──► F5 moedas/câmbio ──► F6 UI
        │                    │                    │                  │                 │
        └── goods.json ──────┘                    └── needs.json ────┘                 └── currencies.json
                                             (F3 e F4 podem trocar de ordem;
                                              F5 depende de F4 no preço entregue)
```

Checkpoint por fase (obrigatório):

1. `cmake --build` (só F2+), sem warnings novos;
2. testes da fase verdes (lista em cada fase);
3. `economy_world_test` verde (60 d) — é o guarda de regressão econômica;
4. `GAME_STATE.md` §6/§8/§9/§13 + §14 (decisão) atualizados, com "não verificado" onde não rodou;
5. commit **não** é feito por este agente sem pedido explícito.

Sugestão de granularidade de PR: F1 em 5 PRs (um por lote de ícones) + 1 de dados; F2 em 3
(21→variável; receitas; subprodutos + IA); F3/F4/F5 um cada; F6 um por tela.

---

## 8. Riscos de performance (114 bens × 834 cidades) e como medir

### 8.1 Baseline medido (não estimado)

`docs/economy/perf/perf_858_release_after.txt`, 858 cidades, 21 bens:

| Etapa | média | p95 |
|---|---:|---:|
| tick C++ (worker) | 30,14 ms | 34,97 ms |
| fase `status` | 18,84 ms | 21,65 ms |
| └ `status_cities` | 14,58 ms | 16,51 ms |
| fase `trade` | 4,93 ms | 8,81 ms |
| frame de pico (main) | 6,97 ms | 8,62 ms |
| memória estática | 432,3 MB | — |

### 8.2 Risco 1 — status por produto (o maior)

`demo_fill_city_status` cria **1 Dictionary por (cidade, produto)** e o reescreve com comparação
de doubles (`cpp:3309-3363`). Com 114 bens: **834 × 114 = 95 076** dicionários por slot, ×2 slots
(double buffer) ≈ **190 mil**. Projeção grosseira a partir dos 14,58 ms/21 bens: **60–90 ms** só
em `status_cities` — o tick iria de 30 ms para > 100 ms.

Mitigações, em ordem de preferência:

1. **Publicar mercado só de produto ativo** (`stock/demand/supply/sold/shortage > 0`), com um
   `market_index` (PackedInt32Array) para o consumidor mapear. A cidade típica hoje movimenta
   2–5 dos 21; com 114 a fração ativa deve ficar em 3–10. Ganho quase linear, custo baixo,
   consumidor precisa de mapa (mudança de contrato → documentar).
2. **SoA** (`PackedFloat64Array` por campo, tamanho `cities × products` em `market_totals`) e a
   UI/IA lendo por índice — elimina os 190 mil Dictionaries, mas muda todo consumidor GDScript
   (`production_screen`, `private_investor`, `city_panel`).
3. Manter como está só se a medição mostrar `status` ≤ 30 ms p95 (improvável).

**Decisão recomendada:** opção 1 na F2 (junto com a mudança de contagem), avaliando a 2 se a UI
de F6 precisar de tudo.

### 8.3 Risco 2 — comércio genérico

O comércio atual é O(compradores × candidatos × 5 alimentos) com teto de 512 embarques
(`cpp:852`). Ingênuo com 114 bens: O(compradores × candidatos × 114) — ~20× pior. Mitigação
obrigatória (§5.4): **iterar os produtos demandados por cidade** (necessidades ativas + insumos
de construções que operam), não os 114. Teto adicional `kDemoMaxShipmentsPerDay` por produto.

### 8.4 Risco 3 — fases O(cidades × bens)

`open_prices` aloca `cities × products` doubles por tick (`cpp:2264`); fase 8 varre
`products` com varredura interna das construções locais (`cpp:2863-2912`); fase 9 idem
(`cpp:2928-2944`). Com 114: 834 × 114 = 95 mil iterações por fase — barato em CPU, mas as
**alocações por tick** devem migrar para buffers reaproveitados (mesmo padrão do
`DemoStatusCache`).

### 8.5 Risco 4 — benchmark não mede isso

`initialize_benchmark` modela 1 produto (`h:626`). Ou se estende o bench para `N` produtos, ou
**toda medição de F2+ é feita no caminho real**: `tools/perf_day_peak.tscn` (cena, usa `Economy`)
com `demo_set_profiling(true)`, lendo `demo_get_profile()` (fases em µs, `cpp:2168`).

### 8.6 Portões de aceite de performance

Medir **antes e depois** de cada fase que toca o tick (F2, F3, F4, F5), com o mesmo cenário e o
mesmo número de dias, e registrar em `docs/economy/perf/`:

| métrica | portão |
|---|---|
| tick p95 | ≤ 50 ms (baseline 35) |
| `status` p95 | ≤ 30 ms (baseline 21,7) |
| frame de pico (main) | ≤ 12 ms (baseline 8,6) |
| memória estática | ≤ 750 MB (baseline 432) |
| trabalho da IA/dia fatiado | ≤ 30 ms (baseline 24,4) |

Se um portão estourar: **não** seguir para a fase seguinte; aplicar 8.2/8.3/8.4 primeiro.

---

## 9. Riscos de balanço

1. **Trabalho diluído (crítico).** `fill` único (§2.4) faz cada fábrica roubar mão de obra da
   fazenda. Industrializar sem realocação = fome. Mitigação: §5.3 + teste de satisfação média em
   180 d (F2).
2. **Escada inflacionária.** O piso de preço é `cost_floor × 1,05` com `cost_floor` derivado do
   preço do insumo (`cpp:2889-2904`); com 3–5 níveis de cadeia, cada elo soma 5% + margem. Risco
   de o bem final sair caro e ninguém comprar (fábrica falece em 60 d, `kDemoLossDaysBeforeClose`).
   Mitigação: teste que reconstrói o custo total da cadeia e compara com o preço de mercado;
   limitar o multiplicador acumulado (ex.: preço final ≤ 4× custo de insumos primários) e calibrar
   no laboratório.
3. **Subproduto sem comprador.** `bran`/`sawdust` entram em estoque sem demanda → `surplus` alto →
   preço no piso → prejuízo no moinho. Mitigação: garantir receita consumidora (`feed`,
   `cellulose`, `pallets`) **antes** de ligar o subproduto; ou creditar subproduto a preço de
   reserva (com aviso de estimativa).
4. **Alternativas de receita com custos muito diferentes** virando "sempre a mais barata" e
   tornando 2/3 das receitas decorativas. Mitigação: medir a distribuição de escolha no
   laboratório; exigir ≥ 2 receitas usadas por bem com ≥ 2 alternativas, senão ajustar `qty`.
5. **Bens finais de consumo sem consumidor calibrado.** `shoes`, `phone`, `appliances`,
   `computer`, `jewelry` só existem se `needs.json` tiver categoria com âncora. Sem isso, fábrica
   de `phone` nasce e morre. Mitigação: F3 **antes** de ligar essas cadeias, ou marcar as cadeias
   como "sem demanda — não construir" na IA.
6. **Militar sem exército.** `use: army` não tem consumidor no código (não há exército simulado).
   Mitigação: declarar essas cadeias como **produção sem demanda** — a IA não constrói — e
   registrar a pendência em §10 deste plano (não fingir consumo).
7. **Duráveis com reposição 10%/ano** podem inflar estoque doméstico e deprimir preço (a família
   compra uma geladeira a cada 10 anos; o produtor vê `sold_prev` baixo e produz pouco — correto,
   mas o `inventory_cover_days = 3` não se aplica a duráveis). A spec tem **17 duráveis**
   (`furniture`, `luxury_furniture`, `tractor`, `mining_machine`, `robots`,
   `industrial_machinery`, `truck`, `car`, `cargo_ship`, `cargo_plane`, `appliances`, `computer`,
   `phone`, `drone`, `jewelry`, `small_arms`, `artillery`). Mitigação: cobertura própria para
   duráveis (ex.: 30–90 dias) e teste de convergência em 1.825 d.
8. **Câmbio movendo o comércio inteiro.** Uma taxa mal calibrada faz o mundo todo comprar do
   mesmo país. Mitigação: teste de inversão (F5) + relatório de participação por nação no
   laboratório; clamp de variação diária como o preço (±5%).
9. **Frota como gargalo absoluto.** Se a frota nasce 0, **nada** é comerciado no dia 1 e o mundo
   passa fome enquanto as fábricas de caminhão não existem. Mitigação: frota inicial derivada da
   população/PIB (com `source: derived`), ou transição explícita com `demo_set_fleet` no
   `initialize_demo_world` — decisão aberta (§10), nunca silenciosa.
10. **Perda de segurança alimentar** é o sintoma final de 1, 2, 5 e 9. Guarda único:
    `pop.satisfaction` média e p10 em 60/180/730 d, comparados ao baseline atual.

---

## 10. Decisões abertas (precisam do dono do jogo antes de F2/F5)

| # | Decisão | Opções | Recomendação |
|---|---|---|---|
| 1 | `base_price` dos 93 | derivar de receita + margem por tipo; ou curadoria manual com fonte | derivar (reprodutível) e marcar `derived`; calibrar no laboratório |
| 2 | Quantidades de `needs` fora de alimento | `share_of_budget` estimado; ou categoria sem consumo até haver dado | estimado + rótulo `estimated` na UI/relatório |
| 3 | Semente industrial | IA constrói (nenhuma geografia inventada); ou semear fábricas em `buildings.json` | IA só; se o "sair de 0" demorar demais, semear **capitais** com nota "estimativa" |
| 4 | Frota inicial | derivada de população/PIB; ou zero (mundo sem comércio até produzir caminhões) | derivada, com `demo_set_fleet` e `source` documentado |
| 5 | Câmbio | taxas reais publicadas (com citação/data); ou derivadas do PIB PPP já carregado | derivadas agora, reais quando houver curadoria |
| 6 | Camada de liquidação | âncora interna (recomendada); ou caixa por moeda | âncora interna |
| 7 | Sucata de duráveis | volta ao mercado; ou descarte com perda | descarte com perda declarada (evita mercado de "usados" sem regra) |
| 8 | Tarifa inicial | 0 para todos; ou tarifa por bloco comercial | 0 (política é conteúdo de gameplay, não calibração) |
| 9 | Cadeias militares (`ammunition`, `artillery`, `uniforms`…) | sem demanda até existir exército simulado (IA não constrói); ou consumo público fictício via `government` | sem demanda, declarado — a spec não define consumo de exército e o núcleo não tem exército |
| 10 | Consumo institucional (`government`: `paper`, `computer`) | ligar como categoria de necessidade do Estado; ou manter sem consumo | ligar em F3 como categoria própria (é o único consumidor natural de `paper`/`computer` fora das POPs) |

Sem essas respostas, F2 pode começar (receitas/multi-insumo não depende de 1–10 exceto o preço
base, que é dado de F1), mas **F5 não**.

---

## 11. Definição de pronto

- 114 bens em `goods.json` na ordem canônica (0..20 preservados), com preço, setor, `tier`,
  i18n × 3 e ícone próprio importado pelo Godot;
- receitas normalizadas, com alternativas e subprodutos, validadas por script;
- `industrialization > 0` no cenário real com satisfação alimentar preservada;
- POPs com 3 níveis, substituição e duráveis com reposição anual;
- frete exigindo frota + combustível, reaproveitando portos/fila/transbordo;
- preço entregue com câmbio e camadas, escolha pelo menor preço na moeda do comprador;
- portões de performance de §8.6 medidos e registrados em `docs/economy/perf/`;
- `GAME_STATE.md` §6/§8/§9/§13 atualizados, decisões em §14, pendências em §15;
- nenhum teste da suíte econômica regredido; lista de testes rodados registrada no encerramento.

## 12. Decisões do dono do jogo (2026-11-14)

Aceitas as recomendações da §10, com estas confirmações:
1. base_price derivado (receita + margem), marcado `derived`.
2. needs fora de alimento estimados, marcados `estimated`.
3. Semente industrial pela IA; se demorar, semear capitais com nota "estimativa".
4. Frota inicial derivada de população/PIB.
5. Câmbio derivado do PIB PPP; liquidação por âncora interna.
6. Âncora interna.
7. Sucata de duráveis: descarte com perda.
8. Tarifa inicial 0.
9. Militares: SEM demanda por enquanto (exército vem bem depois).
10. Governo consome paper/computer como necessidade do Estado (F3).

Diretriz: implementar TODAS as fases F1→F6 em sequência. Foco = economia realista funcionando no jogo
(industrialização > 0, comércio de todos os bens, preço entregue, frota+combustível, moedas), sem regredir comida/população.

## 13. Fases futuras aprovadas (depois de F6)

- **F7 — Trabalho, educação e renda:** POP dividida em idade ativa / força de trabalho / empregados / desempregados / fora da força (estudantes, aposentados); reserva recrutável 18–49 (para exército futuro). 3 níveis de educação (básico, intermediário, superior) com salários ~1×/2×/4×; cada edifício pede mistura de níveis (sem qualificação rende menos ou não abre). Renda = salário + lucro + benefícios e define o nível de consumo (basic/life/luxury). Salário alto num nível leva jovens a estudar (saem da força, voltam qualificados, consomem Educação).
- **F8 — Serviços:** 20 setores simulados (Varejo, Atacado, Alimentação fora de casa, Hospedagem e turismo, Logística de carga, Transporte de passageiros, Finanças, Seguros e previdência, Saúde, Educação, Telecom, TI, Mídia e publicidade, Serviços profissionais, Imobiliário, Construção, Manutenção e reparos, Serviços pessoais, Lazer e cultura, Limpeza/segurança/resíduos). Cada setor: compradores (POP por nível, empresas, governo), bens consumidos, serviços consumidos (1–2), mistura de educação. Locais, exceto Finanças/TI/Turismo/Logística (exportáveis). A POP abre negócios quando há demanda e renda; lucro vira renda da POP. Os 100 negócios são tipos visuais dentro dos setores (sem custo de simulação). Detalhe completo no Claude Doc "OpenGS — Cadeias de produção e consumo cultural".
- Nota F8: Educação e Saúde do setor de serviços são PRIVADAS — complementam em menor escala os serviços públicos (escolas/hospitais/universidades do governo), que serão uma fase própria depois (F9 — Serviços públicos). POP com renda usa o privado; o resto depende do público.
- **Futuro (dono do jogo, 2026-11-14):** câmbio fixo atual é provisório. Fase futura de **política cambial e monetária**: regimes por nação (flutuante, banda, peg a outra moeda, união monetária), banco central, juros, reservas, inflação; e **restrições de comércio** (hoje qualquer nação comercia com qualquer uma): acordos, blocos, embargos, sanções, cotas, relações diplomáticas afetando o acesso ao mercado. A âncora XGS continua só como unidade contábil interna (régua), não como regime.
- **Revisão F7 (dono, 2026-11-15):** salário NÃO é multiplicador fixo por qualificação (os 1×/2×/4× saem). Salário é DINÂMICO por mercado de trabalho de cada cidade/nação e nível de educação: oferta (trabalhadores daquele nível) × demanda (vagas que pedem aquele nível), produtividade/margem das firmas, riqueza do país (PIB per capita / nível de preços), com rigidez e ajuste gradual. Educação dá acesso às vagas e eleva produtividade; o prêmio salarial emerge da escassez. Preparar ganchos para política futura: salário mínimo por país (piso), sindicatos/rigidez, diferenças regionais.
