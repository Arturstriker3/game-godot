# Revisão de Balanceamento Global de Recursos — Fase A (diagnóstico)

> Data: 2026-11-10 · Autor: game-systems-designer (DeepSeek V4 Pro)
> Escopo: **somente diagnóstico** — nenhuma correção aplicada. Scripts criados em
> `tools/` (`balance_review.gd`, `balance_review.py`) e dados em `docs/economy/`.
>
> **Fonte última: o código.** Tudo abaixo foi medido executando o mesmo pipeline
> do jogo ao vivo (`initialize_demo_world` + `demo_set_goods` +
> `GeographyService.apply_to_sim` + `EconomicAI`) em headless por 730 dias.

---

## 0. Resumo executivo

A economia global está **quebrada em três camadas independentes**, e as três se
somam num colapso total:

1. **O jogo ao vivo declara ZERO aptidão agrícola e ZERO reserva mineral para as 8
   nações.** `economy.gd` chama `apply_to_sim` **antes** de `load_data()`, e as
   funções de acesso por nação (`crop_suitability_nation`/`forest_cover_nation`/
   `deposits_for_nation`) não carregam os dados. Resultado: satisfação alimentar
   **0% desde o dia 1** e nenhuma mina de mineral na vida real.
2. **19 dos 23 bens não têm produtor nem consumidor.** A IA só tem templates para
   trigo/farinha/pão/ferro. Arroz, milho, batata, soja, algodão, madeira e os
   12 minerais restantes têm reserva/aptidão declaradas, mas **nenhuma construção
   pode produzi-los** — produção = 0 para sempre.
3. **As reservas estratégicas estão órfãs.** 99 depósitos "curados" (pré-sal Tupi/
   Búzios, Mar do Norte, gás do Oriente Médio, terras raras de Bayan Obo, carvão de
   Kalimantan) têm `territory` vazio, então o `GeographyService` os descarta.
   Isso some com **78% do petróleo, 68% do gás, 69% do carvão, 87% do diamante e
   73% das terras raras** do mundo — Arábia Saudita, Irã, Emirados, Catar, Chile e
   Cazaquistão ficam com **zero** do seu recurso-real.

Além disso há regressões de baseline (Brasil soja 0,76 vs 0,87 documentado) e um
loop descontrolado da IA construindo moinhos de farinha inúteis (544 no cenário
quebrado, 160+ no corrigido).

---

## 1. Metodologia

| Item | Como foi medido |
|---|---|
| Simulação | `tools/balance_review.gd` (SceneTree headless), replica exatamente `economy.gd::_ready` + `_run_day` (incluindo `EconomicAI.run` a cada 7 dias) por 730 dias. Dois cenários: `--fix-load` desligado (= jogo ao vivo atual) e ligado (= `load_data()` chamado antes de `apply_to_sim`). |
| Tabela por nação/bem | `get_demo_status()` → `markets[produto].{supply,demand,stock}` somado por nação; `pop.satisfaction`/`pop.consumed`/`pop.need` para alimentação. |
| Reservas globais | `tools/balance_review.py` agrega `deposits.json` (5.727 registros) por `territory → adm0_a3`. |
| Baseline | MAX por nação sobre `agriculture.json` (737 territórios), comparado com os valores de referência da tarefa. |
| Baseline de testes | 6 testes existentes rodados headless (resultado na §2). |

Arquivos de saída: `docs/economy/balance_review_sim_{broken,fixed}.json` e
`docs/economy/balance_review_static.json`.

---

## 2. Linha de base dos testes existentes

| Teste | Resultado | Observação |
|---|---|---|
| `economy_world_test.gd` | ✅ OK (60 d, sem violação) | **Não** chama `apply_to_sim` — por isso não vê o bug §3.1. BEL-BRU 79% já sinalizava déficit. |
| `economy_ai_test.gd` | ✅ OK | Usa AGRIA/PORTUS **sem** `nation` → cai no fallback `crop_suitability` (que carrega dados). Não cobre o caminho por nação. |
| `economy_integration_test.gd` | ✅ OK | Chama `geo.load_data()` antes de `apply_to_sim` — mascara o bug §3.1. |
| `population_demo_test.gd` | ✅ OK | Cenário demo AGRIA/PORTUS, não o mundo real. |
| `wheat_info_test.gd` | ✅ OK (1550 renováveis, pick consistente) | Ficha agrícola, não economia. |
| `deposit_ring_test.gd` | ✅ OK (marcadores uniformes) | Render, não economia. |

> **Conclusão:** nenhum teste existente exercita o caminho `apply_to_sim` **com
> nações reais sem `load_data()` prévio** — exatamente o caminho do jogo ao vivo.
> Por isso o bug §3.1 passou despercebido.

---

## 3. (a) Economia das 8 nações jogáveis

### 3.1 Jogo ao vivo **como está** (`--fix-load` desligado) — 730 dias

**Colapso total a partir do dia 1** (satisfação alimentar 0% em todas as nações):

| Nação | pop | cons/need (t/d) | satisfação | governo (fim) |
|---|---|---|---|---|
| GBR | 9.000.000 | 0 / 2700 | **0%** | 8,16M |
| FRA | 12.150.000 | 0 / 3645 | **0%** | 0,14M |
| DEU | 6.000.000 | 0 / 1800 | **0%** | 5,32M |
| NED | 2.500.000 | 0 / 750 | **0%** | 1,66M |
| BEL | 2.500.000 | 0 / 750 | **0%** | 1,82M |
| VEN | 2.900.000 | 0 / 870 | **0%** | 2,06M |
| SUR | 241.000 | 0 / 72 | **0%** | 0,01M |
| GUY | 118.000 | 0 / 35 | **0%** | 0,02M |

(Total de moinhos de farinha construídos pela IA no período: **544**, todos com
utilização 0%; **0 minas de ferro**.)

- **Fome artificial total**: 0% em todas as nações, dia 1 em diante. Causa raiz §3.1.
- **Loop de construção**: a IA governamental constrói **544 moinhos de farinha**
  (`STATE-FLOUR-*`, todos com utilização 0%) porque o sinal de escassez nunca
  fecha (moinho sem trigo não produz, a escassez persiste, e ela constrói outro).
- **Nenhuma mina de ferro** é construída (suitability de ferro = 0, pois os
  depósitos também não foram declarados).

### 3.2 Cenário **corrigido** (`load_data()` antes de `apply_to_sim`) — 730 dias

| Nação | pop | satisfação (fim) | tendência | trigo prod/dia | trigo cons/dia | ferro (kt) | problema |
|---|---|---|---|---|---|---|---|
| GBR | 9,0M | 100% | oscila 37–100% | 1652 | 3261 | 198 | depende de importação |
| FRA | 12,15M | 100% | estável | 5910 | 16056 | 1.212 | produtor excedente |
| DEU | 6,0M | 100% | estável | 1833 | 2081 | 490 | ok |
| NED | 2,5M | 100% | oscila 18–100% | 322 | 2569 | 45 | importador estrutural |
| BEL | 2,5M | 83% | oscila 46–100% | 339 | 623 | 87 | déficit crônico |
| VEN | 2,9M | **15%** | colapso | 148 | 213 | 503 | **fome** (trigo 0,17) |
| SUR | 0,24M | **46%** | oscila 4–84% | 3,5 | 51 | 21 | **fome** (trigo 0,05) |
| GUY | 0,12M | **0%** | colapso | 0 | 0 | 82 | **fome** (trigo 0,00) |

- **Fome real nas Guianas/Venezuela**: VEN/SUR/GUY têm aptidão de trigo ≈0 (tropical)
  e **não há template para arroz/milho** — a única cadeia alimentar é trigo→farinha→pão.
  A Venezuela (petróleo real) e a Guiana (bauxita real) passam fome porque o modelo
  alimentar é monotônico (só trigo).
- **Mercado de trabalho/construções**: 276 falências ao longo de 2 anos; o nº de
  moinhos cresce até ~160 e depois colapsa para ~73 (falências).
- **Conta externa**: `foreign_cash = −6,18M` — o "resto do mundo" concede crédito
  infinito para comprar ferro/exportações (bug documentado, sem orçamento externo).

---

## 4. (b) Cadeia dos 23 bens (cenário corrigido, fim de 730 dias)

Produção/consumo **globais** (t/dia), reserva **global** (todas as 193 nações,
não só as 8 jogáveis) e nº de construções ativas.

| # | bem | prod/dia | cons/dia | estoque | reserva global (kt) | constr. | situação |
|---|---|---|---|---|---|---|---|
| 0 | wheat | 10207 | 24854 | 565 | — (renovável) | 28 | produzido; deficitário |
| 1 | flour | 6795 | 6795 | 0 | — | 68 | produzido; insumo = trigo |
| 2 | bread | 862 | 862 | 0 | — | 10 | produzido; insumo = farinha |
| 3 | iron | 119 | 119 | 240 | 63.795.815 | 7 | produzido (só via estatal) |
| 4 | rice | **0** | 0 | 0 | — | **0** | SEM PRODUTOR |
| 5 | maize | **0** | 0 | 0 | — | **0** | SEM PRODUTOR |
| 6 | bauxite | **0** | 0 | 0 | 15.525.969 | **0** | SEM PRODUTOR |
| 7 | lead | **0** | 0 | 0 | 5.185.559 | **0** | SEM PRODUTOR |
| 8 | gold | **0** | 0 | 0 | 637 | **0** | SEM PRODUTOR |
| 9 | phosphate | **0** | 0 | 0 | 24.374.847 | **0** | SEM PRODUTOR |
| 10 | tungsten | **0** | 0 | 0 | 181.691 | **0** | SEM PRODUTOR |
| 11 | rare_earths | **0** | 0 | 0 | 123.753 | **0** | SEM PRODUTOR |
| 12 | sulfur | **0** | 0 | 0 | 1.752.316 | **0** | SEM PRODUTOR |
| 13 | diamond | **0** | 0 | 0 | 36 | **0** | SEM PRODUTOR |
| 14 | coal | **0** | 0 | 0 | 163.897.679 | **0** | SEM PRODUTOR |
| 15 | copper | **0** | 0 | 0 | 6.670.122 | **0** | SEM PRODUTOR |
| 16 | gas | **0** | 0 | 0 | 14.706.978 | **0** | SEM PRODUTOR |
| 17 | oil | **0** | 0 | 0 | 34.599.939 | **0** | SEM PRODUTOR |
| 18 | potato | **0** | 0 | 0 | — | **0** | SEM PRODUTOR |
| 19 | soy | **0** | 0 | 0 | — | **0** | SEM PRODUTOR |
| 20 | cotton | **0** | 0 | 0 | — | **0** | SEM PRODUTOR |
| 21 | wood | **0** | 0 | 0 | — | **0** | SEM PRODUTOR |
| 22 | uranium | **0** | 0 | 0 | 141.345 | **0** | SEM PRODUTOR |

### 4.1 Receitas/insumos (`input_per_output`)

Só existem **duas** receitas na cadeia, hardcoded no C++ e duplicadas em
`industry_templates.gd`:

| bem | insumo | input_per_output | fonte |
|---|---|---|---|
| flour | wheat | 1,2 | `economy_sim.cpp` (`input_per_output=1.2`) / `industry_templates.gd:11` |
| bread | flour | 0,83 | `economy_sim.cpp` / `industry_templates.gd:13` |

**Todos os outros 20 bens** são extração primária (`input_product = -1`), sem
refino (aço, munição, fertilizante, alumínio, eletrônica não existem). Nenhum
insumo está "sem fonte" **porque nenhum outro insumo existe**. Nenhum produto tem
"sem uso" no sentido de processamento — mas 19 deles nem produzem nem consomem
(órfãos completos).

### 4.2 Reserva × extração (anos até esgotar)

Taxa de extração medida é **zero** para 19 bens (não há mina/lavoura). Só ferro
é extraído (119 t/dia). Anos até esgotar:

- **ferro**: 63,8M kt ÷ (119 t/dia × 365) ≈ **1,47 milhão de anos** — reserva
  absurdamente grande frente a qualquer extração plausível.
- **demais 18 minerais**: ∞ (extração 0 por falta de produtor).

**Ordem de grandeza vs mundo real** (referências aproximadas, sem fonte primária
neste repo — apenas sinalização):

| bem | dado (kt) | ref. aproximada | veredito |
|---|---|---|---|
| coal | 163,9M | ~1,07e9 kt (1,07 tri t) | **~6,5× baixo** |
| oil | 34,6M | ~240e6 kt | **~7× baixo** |
| gas | 14,7M | ~140e6 kt | **~10× baixo** |
| iron | 63,8M | ~180e6 kt | **~2,8× baixo** |
| bauxite | 15,5M | ~30e6 kt | ~2× baixo |
| phosphate | 24,4M | ~70e6 kt | ~3× baixo |
| copper | 6,67M | ~870k kt | **~7,7× alto** |
| lead | 5,19M | ~90k kt | **~58× alto** |
| gold | 637 | ~50 kt | **~13× alto** |
| tungsten | 181,7k | ~3,3k kt | **~55× alto** |
| uranium | 141,3k | ~6–8k kt | **~20× alto** |
| rare_earths | 123,8k | ~120k kt | ordem certa, **mas 73% órfão** |
| diamond | 36,4 | (sem "reserva" padrão) | sinalizar |

> Fonte de dados: `deposits.json` usa `quantity_kt` estimado deterministicamente a
> partir de `sites`/`status` do MRDS (`"quantity/quality/depth/cost estimated
> deterministically"` no `_meta`). Os desvios acima são esperados de um extrato de
> "depósitos" (não "reservas recuperáveis"), mas a **inconsistência de direção**
> (minerais de alto valor inflados, fósseis subestimados) indica calibração ausente.

---

## 5. (c) Distribuição global

### 5.1 Recursos estratégicos órfãos (territory vazio → descartados)

`GeographyService._index_deposits` descarta depósitos com `territory == ""`.
99 registros (os **suplementos curados** do `deposits_supplement.json`) estão nessa
situação, escondendo das nações:

| recurso | % global órfão | depósitos órfãos |
|---|---|---|
| oil | **78,2%** (27,05M kt) | Tupi/Búzios/Mero (pré-sal BRA), campos do Golfo, etc. |
| gas | **68,3%** (10,05M kt) | Mar do Norte, Oriente Médio |
| coal | **69,4%** (113,79M kt) | Kalimantan (IDN) etc. |
| diamond | **86,5%** (32 de 36 kt) | — |
| rare_earths | **72,7%** (90M kt) | Bayan Obo (CHN) etc. |

### 5.2 Regiões-chave (medido)

| Região | esperado | medido | veredito |
|---|---|---|---|
| Oriente Médio/OPEP petróleo | Arábia Saudita #1 | **SAU 0, IRN 0, ARE 0, QAT 0, DZA 0, LBY 0**; só KWT (1,4M) e IRQ (0,5M) têm algo | **INCOERENTE** |
| China terras raras | ~60% do mundo | CHN = 1.503 kt (**1,2%** do global) | **INCOERENTE** |
| China tungstênio | ~85% do mundo | CHN = 5.048 kt (2,8% do global) | **INCOERENTE** |
| Brasil/ARG/EUA agro | BR soja 0,87 | BR soja **0,762** | **REGREDIU** |
| Austrália/Chile/Peru cobre | Chile #1 | **CHL = 0**; PER 201.736, AUS 201.905 | **INCOERENTE** (Chile vazio) |
| África do Sul/Botsuana diamante | top-2 | ZAF 0,5 kt, BWA 0,1 kt (global 36 kt, 32 órfão) | **INCOERENTE** |
| Cazaquistão/Canadá/Austrália urânio | KAZ #1 | **KAZ = 0**; CAN 1.939, AUS 2.708 | **INCOERENTE** |
| Marrocos fosfato | #1 | MAR = 637.698 kt (2,6% do global) | ok (mas EUA aparece #1, errado) |
| Guiné/Austrália bauxita | top-2 | GIN 169.267, AUS 450.038 | ok (mas EUA aparece #1, errado) |

### 5.3 Baseline de aptidão (MAX por nação) — regressão confirmada

| nação | cultura | documentado | medido | Δ |
|---|---|---|---|---|
| BRA | soy | 0,87 | **0,762** | −0,108 |
| BRA | cotton | 0,85 | **0,731** | −0,119 |
| BRA | maize | 0,87 | **0,746** | −0,124 |
| UKR | wheat | 1,00 | **0,901** | −0,099 |
| UKR | maize | 1,00 | **0,834** | −0,166 |
| ARG | soy / maize | 0,99 | 0,986 / 0,988 | −0,004 (≤0,01) |
| CHN | soy | 0,91 | 0,906 | −0,004 |
| RUS | wheat / maize | 1,00 | 0,998 | −0,002 |

> Verifiquei o valor **no commit `f0453f4`** (o próprio "balanceamento global por
> estação") e ele **já contém** BRA soy 0,762 — ou seja, o valor 0,87 do
> GAME_STATE §15 **nunca esteve nos dados commitados**. É um desvio
> documentação × dado (ou o fix não aterrissou), não uma regressão recente.
> EUA (soja/milho 0,987/0,989) e Índia (trigo ≥0,78) e China trigo (0,97) estão OK.

---

## 6. (d) Números sem fonte (data_quality / provavelmente inventados)

- **Reservas minerais** (`deposits.json`): 5.690 registros
  `real_location_estimated_attributes` — a **localização** é real (MRDS), mas
  `quantity_kt`/`quality`/`depth_m`/`extraction_cost` são **estimativas
  determinísticas** (ver `_meta`), não dados reais. 37 registros `curated`
  (suplementos) idem.
- **Preços-base** (`goods.json` / `balance.json`): ouro 3000, diamante 5000,
  urânio 500, terras raras 400, etc. **sem campo de fonte**. Valores arbitrários,
  não ancorados em mercado real (e.g., ouro a 3000 vs ferro 60 — relação ~50×
  contra ~400× real).
- **`balance.json` duplica** `base_prices` e os `market_price` dos cenários,
  repetindo 23 valores × N cidades — risco de divergência com `goods.json`/C++
  (dívida já listada no GAME_STATE §15).
- **`nature.json`**: 702 territórios `real_climate` + **35 `test`** (centróide em
  água, valores sintéticos).
- **Parâmetros de engenharia** (`industry_templates.gd`): `jobs_per_ton`, `wage`,
  `opex_per_ton` — herança do cenário demo, sem vínculo com o C++.

---

## 7. (d) Lista PRIORIZADA de correções propostas (NÃO aplicadas)

### P0 — Bloqueia tudo: `apply_to_sim` roda antes dos dados carregarem

| | |
|---|---|
| **Arquivo/linha** | `core/economy/economy.gd:68` (chamada) + `core/economy/geography_service.gd:245` (`apply_to_sim`); funções `:153`/`:162`/`:144` não chamam `load_data()` |
| **Atual** | `apply_to_sim` itera `_nature`/`_agriculture`/`_deposits` **vazios** quando chamado antes de `load_data()`; o jogo chama `apply_to_sim` antes de `_setup_ai` (que é quem carrega) |
| **Proposto** | `geo.load_data()` no início de `apply_to_sim` (idempotente) **ou** em `economy.gd:67` antes da chamada. Preferível: dentro de `apply_to_sim`, para nenhum chamador repetir o erro |
| **Justificativa** | É a causa do colapso total (§3.1): 0% de comida e zero mineral desde o dia 1 no jogo real |
| **Fonte/qualidade** | Medido (simulação headless 730 d + probe direto). Alta confiança |

### P1 — Sem produtor para 19/23 bens (IA só tem 4 templates)

| | |
|---|---|
| **Arquivo/linha** | `core/economy/ai/industry_templates.gd:7-16` (`TEMPLATES` só tem wheat/flour/bread/iron); `:19` (`MINERAL_RESOURCES` lista 5 mas sem templates) |
| **Atual** | `TEMPLATES` = {wheat, flour, bread, iron}; `MINERAL_RESOURCES` = [iron, coal, copper, oil, gas] sem entradas |
| **Proposto** | Adicionar template de extração primária (e.g. `{"output": N, "input": -1, jobs/wage/opex}`) para os 12 minerais restantes e os 6 renováveis restantes (arroz/milho/batata/soja/algodão/madeira) |
| **Justificativa** | Sem isso, arroz/milho/soja/etc. nunca alimentam as nações tropicais e 18 minerais nunca são extraídos (§4) |
| **Fonte/qualidade** | Medido (produção=0 para 19 bens após 730 d). Alta confiança |

### P2 — Fome tropical: cadeia alimentar é monotônica (só trigo)

| | |
|---|---|
| **Arquivo/linha** | `economy_sim.cpp` fase 5 (`kDemoFoodProducts = 3`, preferência `{bread, flour, wheat}` ~linha 1819); `industry_templates.gd:18` (`FOOD_RESOURCES` = só trigo/farinha/pão) |
| **Atual** | Só trigo→farinha→pão entra na cesta alimentar; arroz/milho/batata/soja (índices 4/5/18/19) têm `food:true` em `goods.json` mas **não** entram no consumo |
| **Proposto** | Incluir arroz/milho/batata/soja no consumo alimentar (`kDemoFoodProducts`/preferência) e na segurança alimentar da IA |
| **Justificativa** | VEN (trigo 0,17), SUR (0,05) e GUY (0,00) têm arroz 0,75/0,72/0,76 e passam fome (§3.2) |
| **Fonte/qualidade** | Medido (VEN 15%, SUR 46%, GUY 0% após 730 d). Alta confiança |

### P3 — Suplementos curados órfãos (territory vazio)

| | |
|---|---|
| **Arquivo/linha** | `data/definitions/natural/deposits.json` (99 registros `"territory": ""`); gerado por `tools/apply_deposit_supplement.py` |
| **Atual** | Suplementos (Tupi/Búzios BRA, Mar do Norte, Golfo, Bayan Obo CHN, Kalimantan IDN) entram sem `territory` → `_index_deposits` descarta |
| **Proposto** | Atribuir `territory` (e `adm0_a3`) por coordenada/país no `apply_deposit_supplement.py` e regenerar `deposits.json` |
| **Justificativa** | SAU/IRN/ARE/QAT/DZA/LBY/CHL/KAZ ficam com zero do seu recurso-real; 78% do petróleo e 73% das terras raras somem (§5) |
| **Fonte/qualidade** | Medido (agregação por `territory → adm0_a3`). Alta confiança |

### P4 — Loop de moinhos de farinha da IA governamental

| | |
|---|---|
| **Arquivo/linha** | `core/economy/ai/government_ai.gd:49-101` (segurança alimentar) |
| **Atual** | Quando há escassez de farinha, constrói moinho; o moinho precisa de trigo (`_input_available` verifica estoque>0,1 no momento, não produção sustentável); sem trigo, não produz, a escassez persiste, constrói outro → 544 moinhos |
| **Proposto** | Exigir insumo estrutural (produtor local/rota estável), não estoque pontual; ou construir a **fazenda de trigo** em vez do moinho quando o elo a montante falta; limitar 1 construção/recurso/ciclo |
| **Justificativa** | 544 moinhos inúteis + 276 falências no período (§3) |
| **Fonte/qualidade** | Medido (trace 30-d). Alta confiança |

### P5 — Baseline de aptidão divergente da documentação

| | |
|---|---|
| **Arquivo/linha** | `data/definitions/natural/agriculture.json` (valores por território); gerado por `tools/apply_real_climate.py`/`build_natural_geography.py`; documentado em `GAME_STATE.md` §15 |
| **Atual** | BRA soy 0,762 (doc 0,87); BRA algodão 0,731 (0,85); BRA milho 0,746 (0,87); UKR trigo 0,901 (1,00); UKR milho 0,834 (1,00) |
| **Proposto** | Reconciliar: ou corrigir a fórmula `bio10/bio11` para atingir os valores documentados, ou corrigir o GAME_STATE para os valores reais — **decidir antes de mexer** (o fix não aterrissou nos dados) |
| **Justificativa** | Baseline declarado "não pode regredir"; os dados nunca bateram com a documentação (§5.3) |
| **Fonte/qualidade** | Medido (MAX por nação) + `git show f0453f4` (o commit já tinha 0,762). Média confiança quanto à causa |

### P6 — Calibração de reservas (ordem de grandeza)

| | |
|---|---|
| **Arquivo/linha** | `data/definitions/natural/deposits.json` (`quantity_kt` estimado) |
| **Atual** | Minerais de alto valor inflados (chumbo 58×, tungstênio 55×, urânio 20×, ouro 13×, cobre 7,7×) e fósseis subestimados (carvão 6,5×, petróleo 7×, gás 10×) |
| **Proposto** | Aplicar fator de escala por commodity (ancorado em reservas provadas publicadas) em `build_natural_geography.py`/`add_mrds_minerals.py`; documentar a fonte |
| **Justificativa** | Anos-para-esgotar não fazem sentido (ferro = 1,47M anos) e distorcem a economia futura |
| **Fonte/qualidade** | Comparação com referências aproximadas. Baixa confiança (sem fonte primária no repo) — **sinalizar, não aplicar cegamente** |

### P7 — Preços-base sem fonte

| | |
|---|---|
| **Arquivo/linha** | `data/definitions/economy/goods.json` (base_price) + `data/definitions/economy/balance.json` (`base_prices`, duplicado) |
| **Atual** | Preços arbitrários (ouro 3000, diamante 5000) sem campo de fonte; duplicados em dois arquivos |
| **Proposto** | Adicionar `source`/`data_quality` aos bens; unificar preços-base numa fonte única |
| **Justificativa** | "Nada de número inventado"; duplicação já listada no GAME_STATE §15 |
| **Fonte/qualidade** | Inspeção estática. Média confiança |

---

## 8. Regressões/riscos observados (para o `qa-docs` revisar)

1. **Nenhum teste cobre o caminho real do jogo** (`apply_to_sim` com nações reais sem
   `load_data()` prévio) — sugerido um teste `economy_world_geography_test.gd`.
2. `economy_world_test.gd` dá falso-verde (não aplica geografia, por isso não vê o
   bug P0).
3. O valor do GAME_STATE §15 (baseline 0,87 etc.) diverge do dado commitado desde
   `f0453f4` — documentação a reconciliar.
4. `MINERAL_RESOURCES` em `industry_templates.gd:19` referencia recursos sem
   template (contrato quebrado entre a intenção e a implementação).

---

*Fase A encerrada — nenhuma correção aplicada, apenas `tools/balance_review.gd`,
`tools/balance_review.py`, os JSONs de evidência e este relatório.*

---

## 9. Depois da parte 1 (B1 + B2 + B5 + cobre chileno)

> Data: 2026-11-10. Correções **aplicadas e medidas** rodando o mesmo pipeline
> headless por 730 dias (`balance_review.gd`, sem `--fix-load` — o caminho real do
> jogo). B1 removeu farinha/pão (**21 produtos**, trigo consumido direto); B2
> zerou os depósitos órfãos; B5 implantou o anti-loop de construção; e o cobre
> chileno (Escondida/Chuquicamata/El Teniente) entrou via suplemento.

### 9.1 As 8 nações após 730 dias

| Nação | pop | trigo prod (t/d) | trigo cons (t/d) | atendimento (sat.) | observação |
|---|---|---|---|---|---|
| GBR | 9,00M | 1.405 | 2.758 | **100%** | importador atendido |
| FRA | 12,15M | 5.910 | 6.713 | **99%** | produtor excedente |
| DEU | 6,00M | 1.800 | 1.800 | **100%** | autossuficiente |
| NED | 2,50M | 275 | 2.029 | **62%** | importador estrutural |
| BEL | 2,50M | 423 | 1.085 | **53%** | déficit crônico |
| VEN | 2,90M | 148 | 205 | **17%** | fome (trigo ≈0,17) |
| SUR | 0,24M | 3,5 | 26 | **36%** | fome (trigo ≈0,05) |
| GUY | 0,12M | 0 | 32 | **89%** | quase coberto (importa) |

- **O colapso total do dia 1 sumiu**: a satisfação saiu de 0% (bug P0) para
  53–100% na Europa. As nações tropicais (VEN/SUR) seguem com fome porque a
  cadeia alimentar continua **monotônica (só trigo)** — arroz/milho/batata/soja
  entram na parte 2 (P1/P2).
- **0 falências** no período (antes: 276) e **nenhuma enxurrada de estatais**:
  o anti-loop B5 segurou o governo em ≤6 estatais de trigo por cidade, todas com
  utilização >0,7 (sem moinhos/flour de Fase A).
- Produção global de trigo 9.965 t/d vs consumo 14.648 t/d (estoque 18.348 t);
  ferro 140 t/d (14 minas, sem loop).

### 9.2 Distribuição global antes/depois

| recurso | antes (Fase A) | depois (parte 1) |
|---|---|---|
| petróleo | 78% órfão; **SAU 0** | SAU 4,1M kt (#2), BRA 4,5M (#1) |
| gás | 68% órfão; **IRN 0** | IRN 2,7M kt (#2), NOR 800k |
| carvão | 69% órfão; **KAZ 0** | KAZ 8,0M kt (#5), CHN 65,7M |
| terras raras | 73% órfão; CHN 1,2% | CHN 64,5M kt (**52%** do global) |
| cobre | **CHL 0** | CHL **51.000 kt** (3 minas reais) |
| diamante | 86,5% órfão | resolvido (BWA/ZAF/CAN no topo) |

- **Depósitos órfãos: 0** (antes 99). SAU/IRN/BRA/GBR/NOR/KAZ/CHN recuperaram o
  recurso-real. `data_quality` agora inclui 3 registros
  `estimated_public_source` (o cobre chileno).
- **Cobre chileno**: Escondida (30.000 kt), Chuquicamata (6.000 kt) e El Teniente
  (15.000 kt) com coordenadas reais, `source` citando BHP/Codelco/Mining
  Technology/USGS. O restante do cobre chileno do MRDS (El Soldado etc.) segue
  atribuído a ARG/BOL/PER por uma limitação de territórios (Chile tem 1 território,
  na Antártica) — fora do escopo da parte 1, e o cobre global segue inflado ~7,7×
  (P6, sinalizado, não aplicado).
- **QAT**: não tem território em `nature.json`; seu campo de gás North Field
  (1,5M kt) fica atribuído a KWT (território mais próximo). **Não inventado** —
  limitação documentada.

### 9.3 Baseline de aptidão (reconciliado)

Baseline corrigido para os valores reais de `agriculture.json` (MAX por nação):
BRA soja 0,762, algodão 0,731, milho 0,746; UKR trigo 0,901, milho 0,834; ARG
0,986/0,988; CHN soja 0,906; RUS 0,998. `balance_review.py` agora reporta
**0 regressões** (antes: BRA/UKR eram falsos positivos de documentação).

---

## 10. Depois do B3 (cesta alimentar substituível)

> Data: 2026-11-10. B3 substituiu a demanda alimentar monotônica por uma **cesta
> de calorias substituível** entre **trigo, arroz, milho e batata** (soja fora).
> Fatores calóricos em toneladas-equivalentes de grão: trigo/arroz/milho **1,0**,
> batata **0,25** (fonte FAO/INFOODS — cereais ~3600 kcal/kg vs batata fresca
> ~770 kcal/kg). A cidade consome do estoque local **proporcional à
> disponibilidade** e compra o mais barato por caloria; atendimento = calorias
> atendidas / necessárias. A IA escolhe a cultura de **melhor aptidão × preço**.

### 10.1 Correções desta retomada (a sessão anterior morreu no meio)

Duas regressões do trabalho interrompido foram corrigidas no núcleo:

1. **Fase 4d vendia alimento ao exterior.** A venda externa de "recursos brutos"
   (`kDemoFirstRawProduct=2`) varria **arroz (2), milho (3) e batata (16)** —
   agora alimentos — e os exportava antes do consumo. As cidades produtoras de
   arroz/milho vendiam tudo e ficavam sem comer. Fix: a fase 4d pula
   `demo_food_calories(product) > 0`.
2. **Excedente vendável travava a exportação.** O vendedor só exportava acima da
   cobertura de **3 dias**, mas a produção visa exatamente `sold_prev × 3 dias` —
   o estoque estaciona na cobertura e **nada** fica vendável (Caiena, sem
   produção, morria de fome). Fix: exportar acima da necessidade de **1 dia**.

### 10.2 Atendimento das 8 nações após 730 dias

| Nação | pop | atendimento | cultura dominante (fim) | observação |
|---|---|---|---|---|
| GBR | 9,00M | **100%** | trigo | produtor + importador atendido |
| FRA | 12,15M | **100%** | trigo (+ milho em Caiena) | produtor excedente |
| DEU | 6,00M | **100%** | trigo | autossuficiente |
| NED | 2,50M | **100%** | trigo | importador estrutural atendido |
| BEL | 2,50M | **100%** | trigo | atendido |
| VEN | 2,90M | **~0%** | trigo (0,17) / arroz (falido) | **colapso** |
| SUR | 0,24M | **~5%** | trigo (0,05) / arroz (falido) | **colapso** |
| GUY | 0,12M | **~0%** | arroz (falido) | **colapso** |

A Europa inteira fecha em 100%. As três nações tropicais (**VEN/SUR/GUY**)
colapsam para ~0–5% — abaixo da meta de ~70%.

### 10.3 Mix de cultura (o que a IA de fato construiu)

- **Europa** (GBR/FRA/DEU/NED/BEL): trigo, aptidão 0,57–0,95 — autossuficiente.
- **Caiena (FRA-CAY)**: a IA substituiu a dependência de importação por **milho**
  (aptidão melhor que trigo) e fechou 100% junto com Paris.
- **VEN/SUR/GUY**: a IA **chegou a construir** fazendas de arroz (aptidão
  0,75/0,72/0,76, a melhor cultura local), mas elas **faliram** — sobrou estoque
  de arroz parado (VEN 3.226 t) com produção e consumo zerados.

### 10.4 Causa real do colapso tropical (não é número inventado)

O colapso é a mesma **crise de demanda/liquidez** já documentada no GAME_STATE
§15 ("fome com alimento em estoque"), agora concentrada nas nações tropicais:

1. **Fazenda inicial inadequada.** `buildings.json` declara a produção inicial
   como **efetiva** (VEN 870 t/d = a própria necessidade), mas o núcleo multiplica
   pela aptidão do trigo — 0,17 em VEN, 0,05 em SUR, 0,00 em GUY. A fazenda
   inicial produz ~148/3,5/0 t/d em vez de 870/72/35.
2. **Transição cara.** Para cobrir o déficit, a cidade importa comida e o governo
   subsidia a cesta. O orçamento público de VEN (2,9M) e o caixa das firmas
   esgotam em ~21 dias, antes de as fazendas de arroz (construídas pela IA ao
   longo de dias) gerarem receita própria.
3. **Espiral de pobreza.** Sem caixa, as fazendas de arroz não pagam salário →
   não empregam → a população fica sem renda → não compra o arroz parado no
   estoque → as fazendas não vendem → falem (prejuízo > 60 dias). A satisfação
   chega a 51% (VEN) e 100% (SUR/GUY) nos primeiros 100–150 dias e **desaba**
   quando a liquidez acaba.

Não foi injetado dinheiro artificial nem dado inventado. A meta de ~70% nas 8
nações **não foi atingida** para VEN/SUR/GUY; a causa é estrutural (aptidão
tropical × fazenda inicial de trigo + liquidez de transição), não uma violação
de conservação — `money_total` segue constante e os invariantes passam.

---

## 11. Depois do fix da fazenda inicial (cultura por aptidão × caloria)

> Data: 2026-11-10. A fazenda inicial de cada cidade deixou de ser **fixa em
> trigo**: o `GeographyService.resolve_initial_farms()` resolve a cultura para o
> **alimento básico de maior aptidão × fator calórico** da nação (trigo/arroz/
> milho/batata; fatores 1,0/1,0/1,0/0,25 — fonte FAO em `economy_sim.h`). O C++
> (`initialize_demo_world`) lê o campo `resource` do edifício e mapeia o nome ao
> produto via `demo_product_index` (ausente → trigo, comportamento legado). A
> capacidade nominal (área/trabalhadores) não muda: a produção efetiva segue
> `capacidade × aptidão da cultura escolhida`. Medido pelo mesmo pipeline
> headless de 730 dias (`balance_review.gd`, sem `--fix-load`).

### 11.1 As 8 nações após 730 dias

| Nação | pop | cultura inicial | atendimento | tendência | observação |
|---|---|---|---|---|---|
| GBR | 9,00M | trigo (0,772) | **4%** | COLAPSO | ver §11.3 |
| FRA | 12,15M | trigo (0,985) | 100% | estável | produtor excedente |
| DEU | 6,00M | **milho** (0,823) | 100% | estável | melhor básico local |
| NED | 2,50M | trigo (0,816) | 100% | estável | importador atendido |
| BEL | 2,50M | trigo (0,757) | 100% | estável | atendido |
| VEN | 2,90M | **arroz** (0,753) | **100%** | estável | era ~0% |
| SUR | 0,24M | **arroz** (0,716) | **100%** | estável | era ~5% |
| GUY | 0,12M | **arroz** (0,758) | **100%** | estável | era ~0% |

- **A regressão tropical sumiu:** VEN/SUR/GUY saem de ~0–5% para **100%** — a
  fazenda inicial agora planta arroz (a melhor cultura local), e não trigo
  (aptidão 0,17/0,05/0,00). `money_total` conserva (58.424.850) e **0 violações
  de invariante**.
- **DEU migrou para milho** (aptidão 0,823 × 1,0 > trigo 0,733 × 1,0) e segue em
  100% — é a consequência literal da regra "maior aptidão × caloria".
- **Nova regressão: GBR 100% → 4%.** Não é bug do fix; é a mesma instabilidade
  estrutural de §15 (§11.3).

### 11.2 Mix de cultura (fim)

- **Europa:** GBR/NED/BEL/FRA trigo; **DEU milho**; Caiena milho (a IA já
  substituía, §10.3).
- **Trópicos:** VEN/SUR/GUY arroz (fazenda inicial + estatais de arroz, sem
  falência — o arroz não fica mais parado no estoque).

### 11.3 Causa real do colapso de GBR (não é número inventado)

Isolado por cenário (probe `deu_maize` / `tropics_rice` / `deu+tropics`): **cada
mudança isolada é benigna** (GBR fica 99–100%), mas **as duas juntas** derrubam
GBR para 4%. É uma **instabilidade biestável** — o mesmo "fome com alimento em
estoque" de §15, agora em Londres:

1. A saída de DEU do trigo (−33 t/d de excedente) e a saída de VEN/SUR/GUY do
   mercado de trigo (−~151 t/d de produção) somam uma perturbação **minúscula**
   (~180 t/d num mercado de ~10 mil t/d), mas suficiente para cruzar o ponto de
   bifurcação.
2. Londres cai do **equilíbrio de emprego alto** (fazenda a 77% de utilização,
   autossuficiente, renda salarial 117 mil) para o de **emprego baixo** (fazenda
   a 4%, renda salarial 9 mil): o orçamento público drena ~25 mil/dia em subsídio
   de cesta + importação e zera em ~360 dias; a população empobrece ao lado de
   3,4 mil t de trigo em estoque.
3. A IA privada super-expande a fazenda de Londres (1300 → 3260 t/d) porque o
   sinal de "escassez" (necessidade − produção) **não enxerga a limitação de
   demanda** (renda da população) — sintoma, não causa.

**Meta (~70% nas 8):** 7/8 atingem (VEN/SUR/GUY fixados, DEU migrado); **GBR
falha (4%)**. A causa é a bistabilidade da economia (não uma violação de
conservação nem um bug da fazenda inicial). Se a meta for rígida, a mitigação é
**aplicar a troca de cultura só onde o trigo é inviável** (aptidão < 0,30 — só os
trópicos): nesse recorte as 8 nações fecham em ≥ 99% sem o efeito colateral em
GBR. Corrigir a bistabilidade em si (ex.: o sinal de escassez do investidor
considerar a demanda/renda) fica para uma onda posterior — fora do escopo deste
fix.

---

## 12. Depois da mitigação + correção de raiz da biestabilidade

> Data: 2026-11-10. Duas mudanças aplicadas e medidas pelo mesmo pipeline
> headless de 730 dias (`balance_review.gd`, sem `--fix-load`), com teste de
> regressão dedicado (`tools/economy_bistability_test.gd`).

### 12.1 Mitigação (item 1) — trocar a cultura SÓ onde o trigo é inviável

`GeographyService.resolve_initial_farms()` ganhou um `_initial_staple()`: a
fazenda inicial fica em **trigo quando a aptidão de trigo da nação ≥ 0,30** e só
muda para o básico de maior aptidão × caloria (arroz nos trópicos) quando o trigo
é inviável. Resultado 730 d: **as 8 nações fecham em ≥ 99%** (GBR 99%, demais
99–100%), sem o efeito colateral de tirar a DEU do mercado de trigo. `money_total`
conserva (58.424.850) e **0 violações de invariante**.

| Nação | cultura inicial | atendimento (fim) |
|---|---|---|
| GBR | trigo (0,772) | **99%** |
| FRA | trigo (0,985) | 100% |
| DEU | **trigo** (0,733) | 100% |
| NED | trigo (0,816) | 100% |
| BEL | trigo (0,757) | 100% |
| VEN | arroz (0,753) | 100% |
| SUR | arroz (0,716) | 100% |
| GUY | arroz (0,758) | 100% |

### 12.2 Correção de raiz (item 2) — o gate de "excesso de oferta no sistema" não vale para comida

Diagnóstico da §11.3 **confirmado e refinado**: a super-expansão da fazenda de
Londres (1300 → 3260 t/d) era **sintoma**, não causa. A causa é que o
`PrivateInvestor._try_template` tinha um gate — *"excesso de oferta no sistema"* —
que lia o **excedente GLOBAL** de trigo (a FRA acumulava estoque conforme Londres
importava menos) e **barrava a expansão da fazenda de Londres ANTES da
autossuficiência**. Londres seguia importando ~184 t/d, o orçamento público
(subcesta + importação) drenava ~21 mil/dia até zerar (~dia 420) e, sem subsídio,
a renda caía → a produção da fazenda (que segue `sold_prev`) despencava → o
equilíbrio de baixo emprego (fazenda a 4%, renda 9 mil) se instalava ao lado de
3,4 mil t de trigo em estoque.

A correção de raiz (escopo pequeno, uma condicional): o gate de excedente
**global** passa a valer só para bens não alimentares. Para a cesta (trigo/arroz/
milho/batata), o investidor considera a **demanda real local** (o déficit
`need − supply` da própria cidade), não o excedente do mercado mundial.

> A versão alternativa "o sinal deve usar o consumo em vez da necessidade"
> (`consumed − supply` + ponderação por renda ganha) foi **testada e descartada**:
> ela REDUZ a resposta do investidor ao déficit e derruba a GBR mais rápido
> (orçamento zera no dia ~300, satisfação 4,5%). A super-expansão não era o
> problema — era o remédio parcial (reduzia a importação de 1.696 → 184 t/d).

**Série temporal de Londres — antes (perturbação total, sem correção) vs depois (com correção de raiz):**

| dia | orçamento (antes) | atendimento (antes) | orçamento (depois) | atendimento (depois) | emprego fazenda (depois) |
|---|---|---|---|---|---|
| 30 | 8,97 M | 100% | 8,97 M | 100% | 9.900 |
| 120 | 7,26 M | 100% | 7,26 M | 100% | 10.350 |
| 210 | 3,65 M | 100% | 3,75 M | 100% | 10.500 |
| 300 | 0,89 M | 100% | 1,90 M | 100% | 10.500 |
| 360 | 0,00 M | 86% | 1,49 M | 100% | 10.500 |
| 420 | 0 | 39% | 1,51 M | 100% | 10.500 |
| 540 | 0 | 4% | 1,66 M | 100% | 10.500 |
| 720 | 0 | 4% | 1,88 M | 100% | 10.500 |

- **Antes:** o orçamento de Londres zera no dia ~360–420 e o atendimento desaba
  para 4% (fazenda a 4% de utilização). **Depois:** o orçamento drena até ~1,5 M
  (dia 360) e **recupera** para ~1,9 M — a fazenda estabiliza em ~77% de
  utilização e o atendimento permanece 100%.
- **Robustez (item 2):** com a correção de raiz, a GBR fica **≥ 70% (100%) mesmo
  SEM a mitigação**, aplicando a perturbação original (trocar DEU para milho e
  tirar os trópicos do mercado de trigo). Verificado por
  `tools/economy_bistability_test.gd` (730 d, assert GBR ≥ 70%).

### 12.3 Efeito colateral e regressão observada (para o `qa-docs`)

- **10 falências em 730 d** (era 0 no cenário quebrado): todas de **milho**
  (`AI-MAIZE-*` privada + `STATE-MAIZE-*` estatal, em pares, a cada ~60 d), em
  DEU/Caiena. São fazendas redundantes: a IA (B3, "melhor aptidão × preço")
  planta milho mesmo quando a cidade já é autossuficiente em trigo. A remoção do
  gate de excedente global (antes o bloqueava) deixa essa redundância emergir.
  **Não afeta a meta** (satisfação 100% nas 8, `money_total` conserva, 0
  violações) — é a mesma churn de milho já vista no B3, agora visível porque o
  gate global não a esconde mais. Registrado como regressão menor a reconciliar
  (a IA escolher a cultura já coberta localmente), não como bloqueio.
- **Testes:** `economy_live_path_test`, `economy_world_test`,
  `economy_integration_test`, `economy_ai_test`, `population_demo_test`,
  `economy_long_run_test`, `economy_bistability_test` e `balance_review.gd` —
  todos exit 0.

---

## 13. Calibração de reservas (B6)

> Data: 2026-11-10. Correção **P6** da §7. Aplicada por
> `tools/calibrate_reserves.py`, que reescala `quantity_kt` de cada depósito
> para bater com totais de **reservas publicadas** (não o extrato de
> "ocorrências" do MRDS). Fonte dos alvos: `data/definitions/natural/
> reserve_calibration.json` (`data_quality:"published_aggregate"`), que traz
> por recurso o **total mundial em kt** e as **participações dos principais
> detentores** (top ~8–10 + `"resto"`) — USGS Mineral Commodity Summaries
> (metálicos/não-metálicos), Energy Institute Statistical Review
> (carvão/petróleo/gás) e OECD-NEA/IAEA Red Book (urânio).

### 13.1 Algoritmo

1. Dentro de um país, **mantém-se a proporção relativa** entre os depósitos do
   MRDS (todos escalados pelo mesmo fator país = meta ÷ atual).
2. País com participação na fonte mas **sem depósito** em `deposits.json`:
   **não se cria depósito fictício** — apenas sinaliza-se; a fração fica fora
   da soma mundial (documentado).
3. `"resto"` é distribuído **proporcionalmente** entre os demais países com
   depósito (preserva a razão relativa entre eles).
4. `recoverable_kt` e `max_rate_kt_day` são recomputados pelas fórmulas já
   usadas (`× recovery` e `/ (25×365)`), mantendo a consistência do registro.
5. Idempotente (re-rodar converge os fatores a 1,0). Registros `"facility"`
   (fundições/refinarias) são ignorados, como no restante da cadeia.

### 13.2 Total antes/depois, top-5 e fonte

| recurso | antes (kt) | depois (kt) | fator | top-5 depois | fonte (year) |
|---|---|---|---|---|---|
| iron | 63.795.815 | 190.000.000 | ×2,98 | AUS 58,0M · BRA 34,0M · RUS 29,0M · CHN 20,0M · UKR 6,5M | USGS MCS 2025 |
| copper | 6.721.122 | 980.000 | ×0,146 | CHL 190k · PER 100k · AUS 100k · RUS 80k · COD 80k | USGS MCS 2025 |
| coal | 163.097.679 | 1.074.108.000 | ×6,59 | USA 248,9M · RUS 162,2M · AUS 150,2M · CHN 143,2M · IND 111,1M | EI 2024 |
| oil | 34.599.939 | 232.566.200 | ×6,72 | VEN 41,4M · SAU 40,5M · CAN 22,9M · IRN 21,5M · IRQ 19,8M | EI 2024 |
| gas | 14.706.978 | 92.650.677 | ×6,30 | RUS 28,0M · IRN 24,1M · USA 9,4M · AUS 7,0M · KWT 6,0M | EI 2024 |
| bauxite | 15.525.969 | 27.999.990 | ×1,80 | GIN 7,4M · AUS 5,3M · VNM 3,7M · USA 2,7M · BRA 2,7M | USGS MCS 2025 |
| lead | 5.185.560 | 95.000 | ×0,0183 | AUS 35,0k · CHN 22,0k · RUS 8,9k · PER 6,0k · MEX 5,6k | USGS MCS 2025 |
| gold | 637 | 64 | ×0,101 | RUS 12 · AUS 12 · ZAF 5 · CHN 3,1 · USA 3,0 | USGS MCS 2025 |
| phosphate | 24.374.848 | 73.000.000 | ×2,99 | MAR 50,0M · CHN 3,4M · EGY 2,8M · TUN 2,5M · RUS 2,4M | USGS MCS 2026 |
| tungsten | 181.691 | 4.700 | ×0,0259 | CHN 2,5k · AUS 570 · USA 559 · RUS 400 · VNM 170 | USGS MCS 2025/26 |
| rare_earths | 123.753 | 96.500 | ×0,78 | CHN 44,0k · BRA 21,0k · RUS 10,0k · IND 6,9k · AUS 4,2k | USGS MCS 2024 |
| uranium | 141.345 | 5.630 | ×0,0398 | AUS 1,7k · KAZ 815 · CAN 582 · USA 546 · NAM 498 | Red Book 2025 |
| sulfur | 1.752.316 | (inalterado) | — | (sem total publicado → mantém MRDS) | USGS MCS 2025 |
| diamond | 36 | (inalterado) | — | (sem reserva padrão → mantém MRDS) | USGS MCS 2025 |

### 13.3 Verificação (top-5 coerente com a fonte)

- **petróleo** VEN/SAU/IRN ✓ · **gás** RUS/IRN ✓ (QAT reportado) · **carvão**
  USA/CHN/RUS/AUS ✓ · **cobre** Chile #1 ✓ · **terras raras e tungstênio**
  China #1 ✓ · **urânio** AUS/KAZ/CAN ✓ · **fosfato** Marrocos #1 (68%) ✓ ·
  **bauxita** Guiné #1 ✓ · **ouro** AUS/RUS ✓ · **diamante** BWA/CAN/ZAF já
  coerente (sem total publicado) · **ferro** AUS/BRA/RUS ✓.
- O **viés de cobertura do MRDS** (EUA #1 em fosfato/bauxita/ferro/urânio)
  **desaparece dos topos**: EUA sai de #1 em fosfato (→ 1,4%, Marrocos 68%),
  ferro (→ #8, Austrália #1), urânio (→ #4, Austrália #1).

### 13.4 Países da fonte sem depósito (sinalizados, não inventados)

| recurso | país(es) sem depósito no `deposits.json` |
|---|---|
| oil | QAT (1,5%) |
| gas | QAT (13,1% — sem território em `nature.json`), TKM (7,2%), CHN (4,5%), VEN (3,4%), SAU (3,2%), NGA (2,9%) |
| bauxite | JAM (6,7% — sem território) |
| rare_earths | VNM (18,3%), GRL (1,2%) |
| uranium | CHN (4,6%) |

Consequência: a soma mundial **não fecha no total calibrado** para os recursos
acima — a fração dos países ausentes fica de fora (gas fica em ~66% do total
real, rare_earths ~80%, oil ~98%). É a limitação honesta dos dados (o MRDS +
suplementos não cobre gás da China/Arábia/Turcomenistão nem terras raras do
Vietnã), **não** um número inventado.

### 13.5 Limitações conhecidas

- **Residual do MRDS no "resto":** para minerais em que os EUA não estão entre
  os top-10 reais mas dominam o extrato MRDS (bauxita, tungstênio, urânio), o
  "resto" — distribuído proporcionalmente ao MRDS — ainda deixa os EUA elevados
  (bauxita #4, tungstênio #3, urânio #4). Corrigir exige outra fonte de
  distribuição de "resto" (ex.: produção mineira), fora do escopo deste B6.
- **petróleo/gás em kt** derivam de conversão de barris/m³ documentada no
  `note` do JSON (as participações são independentes da conversão).
- **enxofre/diamante** ficam sem calibração por falta de total de reserva
  publicado comparável (USGS não divulga "reservas" para esses dois).

Evidência: `docs/economy/reserve_calibration_report.json` (totais antes/depois,
top-5 e países ausentes por recurso) e `deposits.json` regenerado +
`resource_points.json` (2.859 marcadores minerais). Testes: `deposit_ring_test.gd`
exit 0; `validate_scenario_data.py` reporta apenas erros pré-existentes não
relacionados (goods.json com 21 bens vs `GOODS_ALLOWED_IDS` desatualizado; i18n
linha 18 com colunas a mais).

---

## 14. Fechamento do B4 — churn de minas, enxofre/diamante e o estado final

> Data: 2026-11-10. Fecha a onda B4 com três correções na IA de mineração e
> mede o estado final pelo mesmo pipeline headless de 730 dias
> (`balance_review.gd`, sem `--fix-load` — o caminho real do jogo).

### 14.1 Correções aplicadas

1. **Margem honesta da mina (governo e investidor).** A margem passou a incluir
   o fator de profundidade da jazida — `preço − (jobs·wage + opex·opex_factor)`
   via `IndustryTemplates.variable_cost_mined`, lendo `city["deposits"][recurso]
   ["opex_factor"]` (o núcleo C++ já multiplicava o opex por esse fator, até 3×;
   a IA estimava margem otimista). Minas muito fundas de mineral de baixo valor
   deixam de ser abertas.
2. **Capacidade dimensionada pela demanda externa.** A mina estatal deixou de
   nascer com capacidade fixa (10 t/d) e passou a `minf(10, maxf(2, demanda_externa
   × 1.2))`. As minas de demanda pequena (ouro/diamante/urânio/tungstênio, ~1 t/d)
   agora nascem com 2 t/d e operam com utilização ~0,5 — antes nasciam com 10 t/d,
   operavam a 0,10, davam prejuízo e faliam em ciclo (churn). O investidor privado
   usa o mesmo teto (`shortage` limitado a `demanda_externa × 1.2`).
3. **Ordenação por margem (governo).** Os candidatos a mina estatal são ordenados
   por margem decrescente, para diamante/urânio não ficarem sempre atrás do
   cooldown de construção (o cooldown de 30 d + `return` após a 1ª obra fazia os
   minerais do fim da lista nunca serem construídos).
4. **Payback estendido para bulk de baixo valor.** O investidor privado ganhou
   `MIN_PAYBACK_DAYS_BULK = 730 d` para o **enxofre** (margem ~7–11/t → payback
   180–290 d; o gate de 150 d barrava). **Só o enxofre** entra nessa lista:
   carvão (margem ~2/t) é fino demais até para 730 d e segue só com a estatal;
   fosfato/bauxita já fluem via `observed_cost` (a estatal abre e o investidor
   observa o custo real) — relaxá-los superexpandiria e tiraria firma/investimento
   dos renováveis (soja/madeira) que vêm depois na iteração.

### 14.2 Resultado: as 8 nações após 730 dias

`dinheiro_total = 58.424.850` (conservado) · `falências = 0` (era **32**, meta
≤ 10) · `violações = 0` · satisfação alimentar **100% nas 8** (GBR/FRA/DEU
99–100%). `foreign_cash = −324,7 M` (saldo externo líquido; a exportação bruta
soma ~1,25 M/dia).

| Nação | o que extrai/exporta (t/d) | receita externa (valor/d) | anos até esgotar (mínimos) |
|---|---|---|---|
| GBR | ferro 19,6 · cobre 115,3 · petróleo 1481,7 · bauxita 8,1 · carvão 8,5 · chumbo 7,3 · fosfato 8,0 · tungstênio 1,0 | ~210 mil/d | petróleo 705 a · cobre 24 a · chumbo 34 a |
| FRA | petróleo 3078,8 · cobre 385,7 · ferro 40,0 · urânio 2,0 · milho 15,7 · bauxita 17,1 · chumbo 16,9 · fosfato 17,0 · tungstênio 4,1 | ~451 mil/d | petróleo 814 a · cobre 28 a · urânio 15 a |
| DEU | bauxita 2313,0 · cobre 494,7 · ferro 20,0 · chumbo 7,6 · carvão 4,5 · fosfato 2,7 · tungstênio 1,0 | ~137 mil/d | cobre 23 a · chumbo 64 a · bauxita 126 a |
| NED | gás 2557,1 · ferro 20,0 · fosfato 7,3 · chumbo 1,3 | ~232 mil/d | gás 214 a · chumbo 24 a |
| BEL | fosfato 1315,3 · ferro 20,0 · soja 18,7 · madeira 16,7 | ~21 mil/d | fosfato 35 a |
| VEN | petróleo 1060,2 · cobre 108,3 · ferro 20,0 · terras raras 9,0 · **enxofre 8,9** · bauxita 7,9 · chumbo 3,8 · ouro 1,0 · **diamante 1,0** · urânio 1,0 · tungstênio 1,0 | ~165 mil/d | ouro 2 a · tungstênio 2 a · diamante 2 a · urânio 2 a · cobre 24 a |
| SUR | milho 46,7 · arroz 11,0 | ~2,6 mil/d | — (renováveis) |
| GUY | petróleo 151,8 · cobre 17,8 · ferro 20,0 · bauxita 8,9 · terras raras 3,5 · chumbo 1,0 | ~27,5 mil/d | chumbo 21 a · cobre 23 a · terras raras 138 a |

- **Enxofre e diamante saíram do 0**: VEN exporta **8,9 t/d de enxofre** (receita
  401/d) e **1,0 t/d de diamante** (5.250/d) — antes `SEM_PRODUTOR`.
- **Falências zeraram**: as minas de ouro/diamante/urânio/tungstênio/chumbo que
  churnavam (32 falências) agora nascem com 2 t/d e utilização ~0,5, sem ciclo de
  falência/recriação.
- **Algodão segue `SEM_PRODUTOR`** — **aceitável e documentado**: nenhuma das 8
  nações tem aptidão de algodão > 0,30 (o `MIN_SUITABILITY` do investidor), então
  não há lavoura viável; não é um bug, é ausência honesta de dado.
- **Batata segue `SEM_PRODUTOR`**: fator calórico 0,25 faz o "melhor básico" de
  cada nação nunca ser batata; as fazendas iniciais ficam em trigo/arroz/milho.
  Sem regressão — já era assim no baseline.

### 14.3 Verificação

`economy_live_path_test`, `economy_world_test`, `economy_integration_test`,
`economy_ai_test`, `population_demo_test`, `economy_long_run_test`,
`economy_bistability_test`, `deposit_ring_test`, `balance_review.gd` e
`python tools/validate_scenario_data.py` — **todos exit 0** (10 verificações).

---

## 15. Soja na cesta alimentar (B3-soja)

> Esta seção foi reconstruída nesta rodada de fechamento: `GAME_STATE.md` §15 já
> citava "`balance_review.md` §15", mas o arquivo commitado terminava em §14.3.

- **Mudança:** a soja entrou na cesta de calorias substituível. Núcleo:
  `kDemoFoodProducts = 5`, `kDemoFoodProductIds = {0,2,3,16,17}` (trigo, arroz,
  milho, batata, soja) e `kDemoCaloricFactor = {1.0, 1.0, 1.0, 0.25, 1.3}`
  (`gdextension/src/economy_sim.h`); `data/definitions/economy/goods.json`
  mantém `soy.food = true`; `IndustryTemplates.FOOD_RESOURCES` ganhou soja
  (`caloric_factor 1.3`) e soja saiu de `RENEWABLE_RESOURCES`;
  `GeographyService.STAPLE_CROPS`/`STAPLE_CALORIC` idem.
- **Fator calórico 1,3 (FAO/USDA):** soja seca (grãos maduros) ≈ 4.460 kcal/kg
  (USDA FoodData Central) contra trigo ≈ 3.400 kcal/kg (FAO/INFOODS) →
  446/340 ≈ 1,31, arredondado para 1,3. Não é preferência arbitrária: é
  densidade calórica por tonelada, na mesma unidade das outras culturas.
- **Sem exportação:** a fase 4d já pula bens com `demo_food_calories > 0`, então
  a soja é consumida e negociada entre cidades — não vira commodity de
  exportação para o "resto do mundo".
- **Onde a soja vence:** **VEN** (a fazenda inicial resolve para soja) e
  **Caiena** (a IA constrói soja) — são os trópicos em que o trigo é inviável
  (aptidão < 0,30) e a soja tem a maior densidade calórica; SUR/GUY seguem em
  arroz e a Europa inteira segue em trigo.
- **Resultado 730 d (`balance_review.gd`):** soja **produzida e consumida**
  (produção 699,8 t/d, consumo 779,5 t/d, 9 construções), **8/8 nações em 100%**
  de satisfação alimentar, **0 falências**, 0 violações de invariante e
  `money_total` conservado. Sem regressão: GBR 99,8% em
  `economy_bistability_test`.
- **Verificação:** `economy_live_path_test`, `economy_world_test`,
  `economy_integration_test`, `economy_ai_test`, `population_demo_test`,
  `economy_long_run_test`, `economy_bistability_test`, `balance_review.gd` e
  `python tools/validate_scenario_data.py` — todos exit 0.

---

## 16. Bacia agrícola por província + terra limitada + churn de fazendas

> Data: 2026-11-11. Fecha a rodada "cidades novas + bacia agrícola por província":
> **33 cidades reais** em 8 nações (FRA 9, DEU 7, GBR 6, NED 3, BEL 2, VEN 4,
> SUR 1, GUY 1), com aptidão de cultura e madeira por **bacia de províncias
> admin-1** (não mais o máximo nacional nem o território grosso) e **terra
> limitada** (capacidade da fazenda tetada pela área cultivável × produtividade
> FAO). Medido pelo mesmo pipeline headless de 730 dias (`balance_review.gd`).

### 16.1 Bacia agrícola por província (aptidão por cidade)

A unidade de atribuição deixou de ser o **território natural** (739, grosso —
França 11, Reino Unido 4, Bélgica 1) e passou para a **província admin-1** do
atlas (**4.529** no total; **418** pertencem às 8 nações jogáveis, em **33
cidades**). `GeographyService.assign_provinces` (regra A1) particiona as
províncias de cada nação entre as cidades — a cidade da **mesma nação** mais
próxima do centróide, restrita à **mesma massa de terra** (landmass, proxy de
continente). Cada província herda a aptidão e a cobertura florestal do
território natural que a contém (centróide mais próximo) e tem área própria em
`data/definitions/natural/province_areas.json` (`tools/compute_province_areas.py`,
mesmo Voronoi equirretangular de `compute_territory_areas.py`). A aptidão da
cidade é a **média ponderada pela área cultivável** das suas províncias
(`city_crop_suitability`). Resolve as 9 cidades que ficavam sem terra (Marselha,
Estrasburgo, Le Havre, Antuérpia, Eindhoven, Caracas, Bristol, Newcastle,
Stuttgart). Validado por `tools/economy_city_catchment_test.gd`.

Efeito na aptidão (exemplos reais, média ponderada da bacia):

| Cidade | trigo | arroz | milho | batata | soja |
|---|---|---|---|---|---|
| London (GBR-LDN) | 0,75 | 0,00 | 0,56 | 0,81 | 0,20 |
| Manchester (GBR-MAN) | **0,18** | 0,00 | 0,38 | 0,20 | 0,15 |
| Newcastle (GBR-NCL) | **0,26** | 0,00 | 0,39 | 0,29 | 0,14 |
| Glasgow (GBR-GLA) | **0,10** | 0,00 | 0,37 | 0,06 | 0,17 |
| Paris (FRA-PAR) | 0,92 | 0,00 | 0,83 | 0,95 | 0,59 |
| Marseille (FRA-MRS) | **0,27** | 0,03 | 0,56 | 0,19 | 0,42 |
| Caracas (VEN-CCS) | 0,02 | 0,69 | 0,46 | 0,00 | 0,63 |
| Maracaibo (VEN-MAR) | 0,17 | 0,09 | 0,38 | 0,00 | **0,24** |
| Valencia (VEN-VLN) | 0,04 | 0,59 | 0,21 | 0,00 | **0,18** |

O antigo máximo nacional dava à GBR toda 0,77 de trigo (Manchester plantava
como East Anglia); agora cada cidade planta como a sua bacia — a consequência é
que as cidades fora da região agrícola principal viram **importadoras** (ver
16.3).

### 16.2 Produtividade FAO e terra limitada

A capacidade NOMINAL de uma fazenda é tetada pela área cultivável da bacia ×
produtividade média FAO (`CROP_YIELD_T_PER_KM2_PER_DAY` em
`geography_service.gd`, t/ha/ano convertida para t/km²/dia × 100 ÷ 365):

| cultura | t/ha/ano | t/km²/dia |
|---|---|---|
| trigo | 3,5 | ~0,959 |
| arroz (casca) | 4,6 | ~1,260 |
| milho | 5,8 | ~1,589 |
| batata (fresca) | 21,0 | ~5,753 |
| soja | 2,8 | ~0,767 |

A produção efetiva segue `capacidade × aptidão`, então o teto físico real =
Σ(área × cultivável × aptidão) × produtividade. Área cultivável por bacia
(exemplos): London 10.493 km², Paris 27.802, Manchester 11.815, Amsterdam
7.789, Ciudad Guayana 135.863. Cidade sem bacia (cenário demo sem nação) não
aplica o teto. Validado por `economy_city_catchment_test.gd` (a produção de
trigo da França após 365 d não excede a capacidade da área cultivável total).

### 16.3 Cidades importadoras (abaixo do ponto de equilíbrio)

O custo variável de uma fazenda de alimento é fixo (`jobs_per_ton 7,5 × wage
2,0 + opex_per_ton 2,5 = 17,5` por t/dia de capacidade); a receita é
`aptidão × preço`. O **ponto de equilíbrio** é `aptidão = 17,5/preço`: trigo
(40) → 0,44; milho (42) → 0,42; soja (45) → 0,39; arroz (55) → 0,32. Abaixo
disso a fazenda dá prejuízo estrutural e **fale** após 60 dias de prejuízo.

Cidades com **todas** as culturas abaixo do equilíbrio viraram importadoras
estruturais — não por falta de terra, mas por aptidão: **Manchester** (máx.
milho 0,38), **Newcastle** (0,39), **Glasgow** (0,37) e **Maracaibo** (0,38).
A rede nacional (Londres/Paris + portos) as alimenta; a fazenda inicial de
trigo/soja dessas cidades (cultura escolhida pela aptidão **nacional**) falha e
é substituída pelo investidor privado pela cultura certa da bacia (milho em
Manchester/Newcastle/Glasgow, arroz em Valencia — 0,59 > equilíbrio).

### 16.4 Falências: 37 → 5 (churn de fazendas)

Com as 33 cidades, o pipeline de 730 d reportava **37 falências** (era 0 com as
9 cidades originais), todas churn de fazendas: **32 milho** (estatais de 20 t/d
em Manchester/Newcastle/Marselha), **4 trigo** (fazendas iniciais de Manchester/
Newcastle) e **1 soja** (inicial de Maracaibo).

- **Causa raiz (milho):** o `GovernmentAI` construía fazendas de milho estatais
  de **20 t/d** (teto `MAX_STATE_CAPACITY`, dimensionado para minas) ao lado do
  investidor privado, que já construía fazendas de milho de **2.185 t/d**
  (`demo_invest` expande). O gatilho era o déficit **estrutural** (`need −
  produção local`) e o preço **de escassez** (o milho espigava a ~126 durante a
  crise); quando a escassez passava, a estatal de 20 t/d não tinha demanda
  (utilização ~0,08) e falia — e o cooldown de 30 d recriava outra.
- **Causa raiz (trigo/soja):** a fazenda inicial de cidades com bacia abaixo do
  equilíbrio (16.3) nasce dimensionada à necessidade (825 t/d em Manchester)
  mas produz `capacidade × aptidão` (148 t/d a 0,18), paga folha/opex pela
  capacidade cheia e fale.

Correções em `core/economy/ai/government_ai.gd` (escopo pequeno, só na IA):

1. **Respeitar demanda regional:** o gatilho de segurança alimentar passou de
   `need − produção local` (dependência de importação) para **`need − consumo`**
   (fome real). Cidade alimentada por comércio regional não constrói fazenda
   redundante.
2. **Viabilidade (aptidão da cidade):** `_best_food_crop` passou a exigir
   `aptidão × preço > custo variável` — não abre fazenda abaixo do ponto de
   equilíbrio.
3. **Não duplicar produtor:** o governo só constrói alimento quando a cidade
   **não tem nenhum produtor** da cesta (`_has_food_producer`) — "cria estatais
   quando o capital privado não chega", como no docstring.

Resultado 730 d: **5 falências** (≤ meta de 10) e **8/8 nações ≥ 70%** de
satisfação no fim: GBR 98%, FRA 99%, DEU/NED/BEL/SUR/GUY 100%, VEN 88%.
`money_total` conserva (110.639.100) e **0 violações de invariante**. As 5
falências restantes são as **fazendas iniciais** de Manchester (trigo, d61),
Maracaibo (soja, d61), Valencia (soja, d61), Newcastle (trigo, d460) e **1
milho privado** em Marselha (d697, sem demanda local) — a consequência honesta
da bacia (16.3), não um bug de IA: a satisfação se mantém porque o investidor
privado reconstrói a cultura certa. `balance_review.gd` → **5 falências**,
`economy_city_catchment_test`/`economy_live_path`/`economy_world`/
`economy_integration`/`economy_ai`/`population_demo`/`economy_long_run`/
`economy_bistability`/`territory`/`deposit_ring` + `validate_scenario_data.py`
— todos exit 0.

---

## 17. Posse mineral por cidade (A1 na exibição)

> Data: 2026-11-11. Fecha a tarefa "cada jazida pertence a uma única cidade".
> A regra A1 já valia na **simulação** (`_assign_deposits` → `apply_to_sim`,
> IA e núcleo C++) — o que faltava era a **exibição**: ficha/legenda da
> cidade ainda usavam o total nacional. Nenhuma regra econômica mudou.

### 17.1 Antes — a ficha e a legenda mostravam a nação

`map/city_world.gd::_resource_entries` chamava
`GeographyService.deposits_for_nation`, então **todas as cidades de uma nação
listavam o mesmo conjunto** — com a reserva nacional repetida em cada uma:

| Nação | recursos listados | cidades que repetiam |
|---|---|---|
| GBR | 9 | 8 |
| FRA | 9 | 10 |
| DEU | 9 | 8 |
| NED | 4 | 4 |
| BEL | 3 | 3 |
| VEN | 12 | 5 |
| SUR | 9 | 1 |
| GUY | 8 | 1 |

O que **já** era por cidade: a partição real (`_assign_deposits`) alimentava
`apply_to_sim` — IA e núcleo C++. Ela distribui **261 jazidas** nas 8 nações
(GBR 25, FRA 46, DEU 41, NED 4, BEL 3, VEN 93, SUR 24, GUY 25) com **0
duplicação**: a soma das cidades de uma nação é a reserva nacional.

### 17.2 Depois — um dono por jazida em todas as telas

Novos acessos em `core/economy/geography_service.gd` — `assign_deposits`
(idempotente, substitui o antigo `_assign_deposits`), `deposits_for_city`,
`city_mineral_reserves`, `deposit_owner_city`, `owners_for_territories`,
`ensure_deposit_ownership` e `scenario_cities` — e os quatro pontos de exibição
passam a ler a **mesma** atribuição:

| Tela | Arquivo | O que mostra agora |
|---|---|---|
| Legenda da cidade | `map/city_world.gd` | jazidas próprias + culturas da bacia |
| Cartão da cidade | `core/ui/city_panel.gd` | jazidas próprias, com vazio explícito |
| Heat do mapa | `map/map_mode_layer.gd` | conta jazidas da cidade, não do território |
| Ficha do depósito | `map/map_mode_layer.gd` + `core/ui/hud.gd` | linha "Cidade dona"/"Cidades donas" |

A ficha do depósito resolve o dono pelos **territórios reais do marcador**
(`owners_for_territories`) e mostra até 3 nomes + contagem — o marcador agregado
nunca atribui tudo à capital. Cidade sem jazida própria mostra o estado vazio
("Esta cidade não possui jazidas próprias.") em vez do minério do vizinho.

Resultado: **38/40 cidades** têm mineral próprio; **DEU-STR (Stuttgart)** e
**BEL-ANR (Antuérpia)** não têm jazida. A IA abre **132 prédios minerais em
180 d**, todos na cidade dona do recurso.

i18n (pt-BR/en/es, `i18n.csv` + `i18n.json`, 420 chaves por idioma): novas
chaves `city.resources.deposits.empty`, `map.legend.deposit_owner` e
`map.legend.deposit_owners`; `city.resources.deposits` passou de "Ocorrências
conhecidas" para "Jazidas da cidade" e `map.legend.city_hint` declara a regra.
`tools/validate_scenario_data.py` passou a **cruzar ids de cidade/porto com as
chaves `city.name.*`/`port.name.*`**; o cruzamento apontou 3 faltas
(**VEN-LAG, SUR-PBM, GUY-GEO**), adicionadas nesta rodada.

### 17.3 Verificação

| Verificação | Resultado |
|---|---|
| `godot --headless --path . --script res://tools/economy_deposit_ownership_test.gd` (novo) | **exit 0** — 261 jazidas com 1 dono, soma por nação == nacional, 132 extratores após 180 d todos na cidade dona |
| `docs/economy/balance_review_sim.json` (pipeline 730 d) | `days=730`, **4 falências**, **0 violações**, `money_total` **115.330.050**; comida GBR **98%**, FRA **99%**, demais **100%** |
| `tools/validate_scenario_data.py` (Python real, pós-fechamento) | **exit 0** — `Resumo: tudo ok — 0 erros, 0 aviso(s) em 10 arquivo(s) verificado(s)`; cidades 40, portos 18, buildings 39, nations 8, goods 21, i18n 420 registros — 0 chaves `city.name.*`/`port.name.*` faltando |

---

## 18. Fluidez na virada do dia: mapa de recursos incremental + IA escalonada

> Data: 2026-11-11. Fecha a tarefa "duas melhorias de fluidez". Medição com
> `tools/perf_day_peak.gd --map-mode=political|resources` (headless, mesmo
> cenário/IA do jogo, execuções seriais de 30 dias) e balanceamento com
> `tools/balance_review.gd --days=730`. Nenhuma regra econômica mudou.

### 18.1 Causa real do travamento (mapa de recursos)

`MapModeLayer.refresh()` roda a cada `status_updated` (todo dia) via
`city_world.refresh_modes`. Ele criava um `GeographyService` novo e chamava
`load_data()`, reparseando `deposits.json` (3,3 MB) + áreas/províncias **todos
os dias** — inclusive no mapa político e no mapa de recursos, onde o campo de
calor nem está visível. Medido: **2.025,9 / 2.136,1 / 2.261,9 ms** (média/p95/máx)
no mapa de recursos e **2.901,9 / 3.159,8 / 3.171,2 ms** no político.

Correções em `map/map_mode_layer.gd`:

1. **Geografia uma vez:** `setup()` cria o `GeographyService` e carrega os JSONs
   uma única vez; `refresh()` reusa a instância.
2. **Heat sob demanda:** `refresh()` recalcula apenas a série do modo de calor
   **ativo** — político e recursos custam ~0 por dia. A UV dos centros é
   calculada uma vez (`_ensure_uv`) e `set_mode()` recomputa apenas a série
   pedida.
3. **Sem reter o status:** as cidades são copiadas para colunas tipadas (id,
   centro, população, produção, industrialização). Guardar o array de
   dicionários do status mantinha os mercados/prédios do dia anterior vivos e
   transferia a liberação dos dicts para a virada (~0,5 ms).

Correções em `map/territory_map.gd` (`update()` chamado todo dia):

4. **Assinatura da atribuição** (cidades → província): sem mudança de id/nação
   de origem/centro, não roda `assign` (O(4.529 províncias × cidades)).
5. **Assinatura dos donos** (nação da cidade + overrides): sem mudança, não
   varre as 4.529 províncias nem reenvia a textura RGBA8. Isso valia
   **~3,5 ms/dia**; a textura só é reescrita quando um dono muda de fato.

### 18.2 IA econômica escalonada (sem pico de ~24 ms)

Antes, a cada 7 dias, `EconomicAI.run()` chamava `sim.get_demo_status()` de novo
(o `tick_demo()` já devolve o mesmo dicionário) e planejava as 40 cidades de uma
vez: **6,3–8,8 ms** de status + **9,8–15,7 ms** de IA no mesmo frame do tick.

Agora (`core/economy/ai/economic_ai.gd` + `economy.gd`):

- **Escalonamento por cidade:** a cidade de índice `i` planeja quando
  `dia % 7 == i % 7`. A cadência por cidade continua **7 dias** (mesmo número
  de planejamentos), mas cada dia concentra ~1/7 das cidades (~6 de 40).
- **Fatiamento por frame:** `plan(day, summary)` só enfileira; `pump(1.500 µs)`
  roda uma cidade por vez e devolve o controle. O autoload `Economy` continua
  nos frames seguintes (`_process`), inclusive em pausa — nada de IA no frame
  da virada.
- **Sem status duplicado:** `run`/`plan` aceitam o dicionário do `tick_demo()`;
  `get_demo_status()` só é chamado por ferramentas/testes que não passam o
  resumo, e apenas em dias com cidade agendada.

### 18.3 Antes × depois (perf_day_peak, 30 dias, serial)

| medição (média / p95 / máx) | antes | depois |
|---|---|---|
| mapa de recursos (`refresh` + território) | 2.025,9 / 2.136,1 / 2.261,9 ms | **0,22 / 0,25 / 0,40 ms** |
| mapa político (`refresh` + território) | 2.901,9 / 3.159,8 / 3.171,2 ms | **0,23 / 0,28 / 0,36 ms** |
| `get_demo_status()` extra (dias de IA) | 0,81 / 6,12 / 6,31 ms | **0** |
| IA no dia | 1,30 / 9,83 / 9,83 ms (tudo no dia 7) | plano **0,01 ms**; trabalho **0,70–0,72 / 1,25–1,30 / 1,76–1,39 ms/dia** fatiado |
| frame de pico na virada (recursos) | **≈2.038 ms** (mapa) | **8,25 / 9,48 / 11,79 ms** |
| frame de pico na virada (político) | ≈2.915 ms (mapa) | **9,08 / 11,33 / 12,29 ms** |
| pump por frame (comboios + IA) | — | 1,51–1,52 / 1,58–1,59 / ~3,0 ms |

Observação: `tick_demo()` (C++, que já monta o dicionário consumido pelo HUD)
domina o frame restante (~8–11 ms no headless). O mapa e a IA somam **< 1 ms**
no frame da virada. Duas execuções políticas deram 12,29 ms e 13,53 ms de máximo
(variação do tick, não do código); os dias sem IA ficam na mesma faixa.

### 18.4 Balanceamento (730 dias) e verificação

`balance_review.gd --days=730` com a IA escalonada:

| métrica | resultado |
|---|---|
| `money_total` | **115.330.050** (conservado; `invariant_violations=[]`) |
| falências | **5** (≤ 10; §16 já registrava 5 — §17 citou 4 numa execução anterior) |
| satisfação alimentar | **8/8 ≥ 70%**: GBR 98%, FRA 99%, DEU/NED/BEL/VEN/SUR/GUY 100% |
| violações de invariante | **0** |

Equivalência: cada cidade continua planejando a cada 7 dias; o número de
planejamentos por cidade em 730 dias é o mesmo. A diferença de fase (cidade 0
planeja no dia 7, cidade 6 no dia 6) não alterou a satisfação, a conservação
de dinheiro nem as metas de falência.

Testes headless executados (todos **exit 0**): `economy_city_catchment_test`,
`economy_live_path_test`, `economy_world_test`, `economy_integration_test`,
`economy_ai_test`, `population_demo_test`, `economy_long_run_test`,
`economy_bistability_test`, `territory_test`, `deposit_ring_test`,
`economy_deposit_ownership_test`, `map_pick_test` e a cena
`label_layout_test.tscn`. `economy_panel_test.gd` não foi rodado (BOM
pré-existente, conforme `AGENTS.md`).

---

## 19. Fase 1 — 66 nações e 252 cidades (Europa + G20)

### 19.1 Escopo

O cenário saiu de **8 nações / 40 cidades** para **66 nações / 252 cidades**
(Europa + G20), com 18 portos e 252 fazendas iniciais. A geografia deixou de
ter tabela fixa: `GeographyService` lê `data/definitions/scenario/nations.json`
como fonte de verdade dos códigos e resolve as divergências só onde existem
(`NED → NLD`, `XKX → KOS` nas províncias). Nação sem província no atlas
(Vaticano) recebe uma fração simbólica (0,1%) da província mais próxima para
não ficar sem terra.

Duas lacunas de dado são declaradas, não preenchidas: **KOS** não tem território
na base natural atual (XKX fica sem jazida própria) e **VAT** não tem província
(terra doada).

### 19.2 Correções econômicas (causa → correção)

| sintoma | causa medida | correção |
|---|---|---|
| 83 das 252 fazendas iniciais fechavam no dia 61 | folha e opex seguiam a **capacidade nominal** inteira, mesmo com aptidão de 4% — a fazenda pagava 100% do custo para produzir 4% | núcleo dimensiona `jobs` e `opex` pela **fração apta** da terra (`aptitude` vinda do GDScript); produção efetiva continua capacidade × aptidão |
| SAU, CHN e cidades de índice alto ficavam fora da fila de compra todos os dias | teto de **64 embarques/dia**; todas as compradoras com estoque zero empatavam em cobertura relativa 3,0 e as 64 primeiras consumiam o teto | teto **512** (cai sozinho quando os estoques enchem) e fila por **menor cobertura relativa** (`gap / necessidade`), não por gap absoluto |
| megacidades (São Paulo, Xangai, Baku) com fome ao lado de dinheiro parado | família com poupança alta e firma sem capital de giro não conseguia pagar a carga | a **poupança das famílias** entra como pagamento da carga (mesmo dinheiro, sem transferência nova) |
| contagem de falências inflada (99 "falências" em 730 d) | mina com **jazida esgotada** era contada como insolvência | reserva zerada encerra a mina no dia seguinte e vai para `depleted_mines`, não para `bankruptcies` |
| AZE/ISL/SAU com fazenda inerte (aptidão 0) | cultura inicial escolhida pela **melhor aptidão nacional**, não da bacia | `_initial_staple()` é **estritamente local**: trigo só onde a bacia tem trigo ≥ 0,30; senão o básico de maior aptidão × caloria **da cidade** |
| Viena importadora permanente sem renda | meta nacional rateada pela terra apta de cada cidade (Viena: 15% da terra, 89% da população) | cada cidade se alimenta da própria bacia; o excedente nacional (necessidade × 1,10) é rateado pela **terra ociosa** |
| faixas de rótulo sobrepostas na Europa densa | capital ignorava colisão de tela; com 66 nações (Roma/Vaticano a < 1 px, 5 capitais nos Bálcãs) os nomes se sobrepunham | capital só cede a **outra capital de maior população**; texto abaixo de 15% de alfa não entra no layout (sem nome-fantasma no zoom out) |
| `No loader found for res://assets/flags/me.png` | 9 bandeiras novas sem `.import` | `godot --headless --path . --import`: 176 PNGs / 176 `.import`; teste headless confirma **66/66** bandeiras carregando como `Texture2D` |

### 19.3 Resultado de 730 dias

| métrica | resultado |
|---|---|
| `money_total` | **1.966.826.637,60** (conservado em todos os dias) |
| `foreign_cash` | **−21.596.489.966,24** (conta externa sem orçamento — §15) |
| falências (insolvência) | **8** |
| fechamentos totais | **184** (8 insolvências + **176 minas com jazida esgotada**) |
| violações de invariante | **0** |
| satisfação alimentar | **62/66 ≥ 70%**; 4 pendências de modelo (ISL 0%, AZE 10%, SAU 29%, IDN 69,4%) |

As 8 insolvências são todas **fazendas privadas de arroz/trigo/milho** com
utilização ≤ 15% e prejuízo antes de fechar: ZAF 1, IDN 2, USA 1, CHN 2, RUS 1,
MEX 1 — nenhuma é mina.

### 19.4 Tabela por nação (fim do dia 730)

`comida` = satisfação alimentar da nação (`consumed / need` agregado);
`falências` = insolvências; `fechamentos` = fechamentos de prédios, que incluem
as minas com reserva esgotada. Fonte: `docs/economy/bankruptcy_diag.json`
(`nations` + `closures`).

| nação | pop. | comida | falências | fechamentos |
|---|---:|---:|---:|---:|
| ALB | 614.415 | 100% | 0 | 0 |
| AND | 20.430 | 100% | 0 | 0 |
| ARG | 20.360.000 | 100% | 0 | 1 |
| ARM | 1.724.667 | 70% | 0 | 0 |
| AUS | 14.692.752 | 100% | 0 | 0 |
| AUT | 2.703.270 | 100% | 0 | 0 |
| AZE | 4.152.000 | 10% | 0 | 1 |
| BEL | 3.517.430 | 100% | 0 | 2 |
| BGR | 1.599.489 | 100% | 0 | 0 |
| BIH | 917.837 | 100% | 0 | 0 |
| BLR | 2.601.193 | 100% | 0 | 0 |
| BRA | 72.300.000 | 100% | 0 | 22 |
| CAN | 17.760.000 | 100% | 0 | 3 |
| CHE | 1.651.631 | 100% | 0 | 2 |
| CHN | 268.753.766 | 100% | 2 | 14 |
| CYP | 354.452 | 100% | 0 | 0 |
| CZE | 1.964.296 | 100% | 0 | 3 |
| DEU | 17.307.900 | 100% | 0 | 7 |
| DNK | 2.115.273 | 100% | 0 | 0 |
| ESP | 15.250.000 | 100% | 0 | 6 |
| EST | 485.431 | 100% | 0 | 0 |
| FIN | 1.840.646 | 100% | 0 | 6 |
| FRA | 18.650.060 | 100% | 0 | 2 |
| GBR | 20.791.406 | 100% | 0 | 5 |
| GEO | 1.706.949 | 100% | 0 | 0 |
| GRC | 3.892.778 | 100% | 0 | 3 |
| GUY | 170.373 | 100% | 0 | 2 |
| HRV | 813.422 | 100% | 0 | 0 |
| HUN | 2.702.402 | 100% | 0 | 0 |
| IDN | 60.970.000 | 69% | 2 | 16 |
| IND | 163.500.000 | 92% | 0 | 6 |
| IRL | 1.794.004 | 100% | 0 | 3 |
| ISL | 138.642 | 0% | 0 | 2 |
| ITA | 17.373.260 | 100% | 0 | 1 |
| JPN | 83.690.000 | 100% | 0 | 12 |
| KOR | 36.495.000 | 100% | 0 | 0 |
| LIE | 5.197 | 100% | 0 | 0 |
| LTU | 831.746 | 100% | 0 | 0 |
| LUX | 76.684 | 100% | 0 | 0 |
| LVA | 820.698 | 100% | 0 | 0 |
| MCO | 32.965 | 100% | 0 | 0 |
| MDA | 792.994 | 100% | 0 | 0 |
| MEX | 53.220.000 | 74% | 1 | 14 |
| MKD | 549.940 | 100% | 0 | 1 |
| MLT | 6.794 | 100% | 0 | 0 |
| MNE | 295.064 | 100% | 0 | 0 |
| NED | 3.478.680 | 100% | 0 | 0 |
| NOR | 1.584.029 | 100% | 0 | 0 |
| POL | 6.139.159 | 100% | 0 | 1 |
| PRT | 4.310.000 | 100% | 0 | 3 |
| ROU | 2.628.954 | 100% | 0 | 1 |
| RUS | 39.120.000 | 100% | 1 | 6 |
| SAU | 17.675.000 | 29% | 0 | 5 |
| SMR | 4.500 | 100% | 0 | 0 |
| SRB | 1.680.000 | 100% | 0 | 1 |
| SUR | 240.924 | 100% | 0 | 3 |
| SVK | 648.781 | 100% | 0 | 0 |
| SVN | 368.429 | 100% | 0 | 0 |
| SWE | 2.958.462 | 100% | 0 | 1 |
| TUR | 32.093.684 | 100% | 0 | 5 |
| UKR | 6.425.000 | 100% | 0 | 0 |
| USA | 116.250.000 | 100% | 1 | 12 |
| VAT | 829 | 100% | 0 | 0 |
| VEN | 7.293.300 | 100% | 0 | 7 |
| XKX | 721.464 | 100% | 0 | 0 |
| ZAF | 26.387.693 | 100% | 1 | 5 |

### 19.5 Pendência de modelo: ISL, AZE, SAU e IDN

As quatro nações abaixo de 70% **não são bug de dados nem de conservação** —
`money_total` conserva e não há violação de invariante em 730 dias. Elas têm
pouca ou nenhuma terra cultivável na bacia e dependem de **importar** comida,
mas o modelo não tem um mecanismo de pagamento para isso:

- **ISL (0%)** e **AZE (10%)**: produção local praticamente zero; a cidade
  precisa comprar de fora do país e não há **renda urbana** que financie a
  compra contínua (o orçamento público drena e a demanda fica sem lastro).
- **SAU (29%)**: idem — a base é deserto; o petróleo não fecha a conta de
  alimento porque a receita de exportação não é convertida em **tesouro** com
  que a cidade compre comida.
- **IDN (69,4%)**: arquipélago com bacias desiguais; a produção nacional não
  chega às ilhas deficitárias, e o frete/estoque não substitui a falta de
  renda para importar.

**O que falta no modelo** (registrado como pendência, não implementado aqui):
renda urbana/tesouro que permita à cidade **importar** alimento de forma
sustentável — hoje a compra externa é financiada por `foreign_cash` negativo
sem orçamento (§15) e não existe tesouro nacional acumulável. Enquanto isso, o
número honesto é o que está na tabela: essas quatro nações não fecham 70%.

### 19.6 Verificação

Testes headless executados nesta fase, cada um com exit code próprio (todos
**exit 0**): `economy_integration_test`, `economy_live_path_test`,
`economy_bistability_test`, `population_demo_test`, `territory_test`,
`economy_long_run_test`, `economy_city_catchment_test`,
`economy_deposit_ownership_test`, `deposit_ring_test`, `map_pick_test`,
`economy_tick_async_test` e as cenas `label_layout_test`,
`convoy_lifecycle_test` e `nav_strait_test`. `economy_panel_test.gd` não foi
rodado (BOM pré-existente, conforme `AGENTS.md`).

Desempenho (build release, medido nesta fase): tick de 11,1 / 14,3 ms (p50/p99)
e benchmark de 650 cidades em 23,6 ms.

## 20. Fechamento da base natural + renda externa por PIB PPP per capita

### 20.1 Escopo

Duas frentes que estavam abertas desde a fase 1:

1. **Base natural por província:** a aptidão agrícola e a posse mineral passaram a
   usar a **província admin-1** como unidade (4.385 das 4.529 províncias têm
   raster; as 144 restantes caem no fallback do território natural, declarado em
   `_meta.provinces_without_raster`). O gerador é
   `tools/build_province_nature.py` (WorldClim 2.1 10-min no centróide de cada
   província + tipo de solo) e o arquivo é
   `data/definitions/natural/province_nature.json`. `GeographyService` usa a
   aptidão da bacia de captação de cada cidade (média ponderada pela área
   cultivável, regra A1) e a mesma regra para a posse mineral — sem máximo
   nacional e sem duplicação de jazida.
2. **Renda externa coerente com o mundo real:** o subsídio uniforme de
   0,012/pessoa/dia (todos recebiam o mesmo do resto do mundo) foi substituído
   por renda de **serviços/exportações não modelados proporcional ao PIB PPP per
   capita real** da nação.

Ainda nesta frente, o rótulo `landmass` da Europa foi unificado: o continente
aparecia como `continent` (BE/DE/FR/NL/VAT e portos legados) e `europe` (demais
países). Como a posse mineral usa a massa de terra, o minério da Lorena
(atribuído à cidade mais próxima, Luxemburgo, `europe`) ia para Fort-de-France,
Mamoudzou e Saint-Denis — 771 mil kt de ferro atravessando o Atlântico.
`tools/normalize_landmass.py` corrigiu **126 cidades** (121 `europe→continent` +
5 ilhas que herdavam o rótulo do país: Martinica, Mayotte, Reunião, Açores e
Svalbard); o cenário passou de 80 para **84 rótulos** de massa de terra.

### 20.2 Regra da renda externa

`GeographyService._build_nation_income` calcula, uma vez:

| passo | regra |
|---|---|
| 1 | PIB PPP per capita = `gdp_ppp_bn × 1e9 / population` (`nations.json`) — **199 nações** com dado |
| 2 | nação sem PIB/população → **mediana da sua região**, marcada `estimated` — **32 nações** (AIA, CUB, ERI, PRK, TWN, VEN, YEM…) |
| 3 | índice = per capita / média mundial ponderada por população (**US$ 25.805**) |
| 4 | **piso** de 10% da média (0,0012/pessoa/dia) para não zerar nação pobre — **11 nações** no piso (AFG, BDI, CAF, COD, LBR, MDG, MOZ, MWI, NER, SDN, SOM) |
| 5 | renormalização iterativa para a média ponderada voltar a **1,0** (a escala-base de 0,012/pessoa/dia é preservada) |

O núcleo recebe o índice por cidade (`demo_set_external_income_index`, clamp
[0, 8]) e paga `população × 0,012 × índice` por dia, debitando `foreign_cash` — o
dinheiro não é criado (`money_total` conservado). A renda externa não entra no
gasto discricionário de serviços (senão a cidade sem terra gastava 45% da renda
que deveria virar comida).

Extremos medidos: SGP 6,32×, LUX 6,06×, IRL 6,00×, MAC 5,30×, **QAT 4,99×,
ISL 3,23×, SAU 2,86×**; as 11 nações no piso ficam em 0,10×. Uma nação rica sem
terra agora **importa** (a renda paga a cesta); uma nação pobre sem terra
continua sem lastro — comportamento declarado, não um bug.

Contrato C++: campo `DemoCity::external_income_index` (default 1,0), binding
`demo_set_external_income_index`, serialização em `demo_to_dict`/`demo_from_dict`
(save antigo sem o campo → 1,0, nunca 0) e `external_income_index` no status da
cidade. `pop.external_income` é exposto no status e no save.

### 20.3 Resultado de 730 dias (231 nações / 858 cidades)

| métrica | antes (subsídio uniforme) | agora (PIB PPP per capita) |
|---|---:|---:|
| `money_total` | conservado | **2.079.099.603,90** (conservado, 0 violações) |
| `foreign_cash` | −45,7 bi | **−47.833.401.191,05** |
| insolvências (falências) | 19 | **31** |
| violações de invariante | 0 | **0** |
| soberanas < 70% (fim do dia 730) | 0 (mín. 81%) | **8 de 197** (mín. **10,4%**) |
| satisfação média das soberanas | — | **97,4%** (186 em 100%) |
| comida mundial (consumido/necessidade) | — | **97,2%** (367.462 / 378.018 t-dia) |

As 31 insolvências são o efeito esperado de menos renda externa nas nações
pobres (o caixa de firmas de serviço/fazenda que antes era sustentado pelo
subsídio uniforme deixa de fechar). Minas esgotadas continuam contadas à parte.
Fonte: `docs/economy/balance_review_sim_broken.json` (gerado por
`tools/balance_review.gd --days=730`; artefato de medição, não versionado).

### 20.4 As 8 soberanas abaixo de 70%

`comida` = satisfação no fim do dia 730; `média 120 d` = média das amostras do
trace entre os dias 610 e 720 (distingue fragilidade estrutural de um dia ruim
de entrega); `terra apta` = soma das aptidões das culturas alimentares das
cidades da nação.

| nação | pop. | PIB PPP pc | índice | comida (fim) | média 120 d | terra apta | causa |
|---|---:|---:|---:|---:|---:|---:|---|
| KIR | 42.192 | US$ 3.663 | 0,14 | 10,4% | 11,3% | 0,00 | ilha de coral, zero terra apta; renda financia ~14% da cesta |
| PSX | 838.304 | US$ 5.006 | 0,19 | 20,8% | 27,8% | 0,00 | território sem bacia agrícola; renda de 0,19× a média |
| SAH | 21.036 | US$ 5.198 | 0,20 | 25,1% | 24,9% | 0,00 | deserto, zero terra apta; mesma causa |
| DJI | 676.518 | US$ 8.445 | 0,33 | 37,7% | 41,4% | 0,00 | deserto/porto, sem bacia; renda cobre ~1/3 da cesta |
| PAK | 46.811.991 | US$ 6.574 | 0,25 | 39,8% | 71,9% | 3,32 | população grande com renda 0,25×: produção doméstica não cobre a demanda e a importação só cabe no orçamento em parte dos dias (oscila 40–100%) |
| SDN | 4.465.144 | US$ 2.193 | 0,10 | 44,1% | 12,0% | 0,73 | no piso; quase sem terra apta e renda de 0,0012/pessoa/dia |
| CPV | 206.881 | US$ 12.137 | 0,47 | 63,4% | 64,7% | 0,04 | arquipélago seco; renda média-baixa e quase nenhuma bacia |
| TKM | 1.260.924 | US$ 22.602 | 0,87 | 69,1% | 100,0% | 0,00 | deserto com renda quase média; o valor do fim do dia é ruído de entrega (média 120 d = 100%) |

**Leitura honesta:** cinco casos (KIR, PSX, SAH, DJI, SDN) são exatamente o que
o desenho prevê — **nação pobre sem terra não sustenta a importação**; os outros
três são limítrofes: PAK tem terra mas a renda per capita é 1/4 da média, CPV
fica no fio, e TKM só aparece na lista pelo snapshot do último dia. Nenhum dos
oito viola invariante, estoque negativo ou conservação de dinheiro.

### 20.5 Verificação (um teste por vez, exit code)

| teste/cena | exit | observação |
|---|---:|---|
| `economy_external_income_test` (novo) | 0 | perfil (231 nações, 32 estimadas, 11 no piso, média ponderada 1,0), pagamento, conservação, save/load, clamp |
| `economy_long_run_test` | 0 | 365 dias sem violação de conservação |
| `economy_bistability_test` | 0 | GBR 100% sob perturbação de culturas |
| `population_demo_test` | **1** | falha pré-existente da renda externa — ver abaixo |
| `economy_ai_test` | 0 | cenários de alimento/ferro/anti-loop |
| `economy_tick_async_test` | 0 | 60 dias síncronos == assíncronos, 1 tick por vez |
| `deposit_ring_test` | 0 | raio de pick uniforme (±1 px) |
| `map_pick_test` | 0 | ponto/hover/seleção/pick uniformes |
| `economy_deposit_ownership_test` | 0 | cada jazida tem 1 dono; 1.994 extratores na cidade dona |
| `economy_live_path_test` | 0 | 231 nações com aptidão e depósitos sem `load_data()` prévio |
| `economy_integration_test` | 0 | cadeia, investimento, save/load e migração v1→v3 |
| `territory_test` | 0 | atribuição, override e textura de dono |
| `economy_city_catchment_test` | 0 | partição, área, aptidão local e terra limitada |
| cena `convoy_lifecycle_test` | 0 | 392 viagens, 0 fantasmas |
| cena `nav_strait_test` | 0 | Gibraltar, Dardanelos/Bósforo e Øresund |
| cena `label_layout_test` | 0 | 0 linhas e 0 sobreposições |
| cena `map_frame_test` | 0 | 300 frames, 231/231 cores, portos badge |

**`population_demo_test` (vermelho):** a asserção "choque de preço não reduziu a
poupança" falha com 343.362,85 → 373.217,26. A causa foi isolada com o cenário
sintético: com o índice de renda externa em **0** as mesmas 40 jornadas dão
235.362,85 → 229.217,26 (**passa**); com a renda externa ativa a família não
consegue comprar trigo a 100× e o dinheiro não gasto **acumula em poupança**.
Ou seja, a falha nasce da renda externa (herdada da rodada anterior a esta
mudança), não da proporcionalidade por PIB — a cidade sintética não tem nação e
usa índice 1,0, exatamente o comportamento uniforme antigo. Fica registrada como
pendência de QA: a expectativa do teste (choque drena poupança) deixou de ser um
invariante do modelo; decidir entre ajustar a asserção (gasto alimentar como
fração da renda) ou o desenho do choque — não foi alterado aqui.

### 20.6 Desempenho (858 cidades reais)

`tools/perf_day_peak.tscn --days=10` no caminho do jogo ao vivo (autoload
`Economy` + IA + comboios), headless:

| etapa | média | p95 | máx |
|---|---:|---:|---:|
| tick_demo (C++) | 92,2 ms | 119,9 ms | 119,9 ms |
| enfileirar pedidos | 4,5 ms | 5,9 ms | 5,9 ms |
| IA planejar (agenda do dia) | 13,6 ms | 14,8 ms | 14,8 ms |
| VIRADA (tick + plano + mapa) | 110,4 ms | 146,1 ms | 146,1 ms |
| PUMP por frame | 1,56 ms | 1,65 ms | 13,1 ms |

Memória estática do processo com as **858 cidades** carregadas: **234,9 MB**
(sim + IA + comboios, sem o mapa 3D). O tick cresceu em relação às 252 cidades
(11 ms → ~92 ms), como esperado por 3,4× mais cidades; a IA escalonada mantém o
pico de frame em ~1,6 ms por frame durante o pump.

### 20.7 Riscos e pendências

- `foreign_cash` segue **sem orçamento** (§15): a renda externa é paga por uma
  conta que pode ficar negativa por desenho. O total é conservado por tick, mas
  o passivo externo cresce (−47,8 bi em 730 d) e não é financiado por nada.
- O piso de 10% é uma escolha de desenho (não um dado): baixá-lo aprofunda a
  fome nas nações pobres; elevá-lo aproxima do subsídio uniforme que se quis
  eliminar. A sensibilidade está declarada em 20.4.
- 32 nações usam **mediana regional** porque `nations.csv` não tem PIB PPP
  (CUB, PRK, TWN, VEN, YEM…). São marcadas `estimated` no perfil; substituir por
  dado publicado é a correção definitiva.
- O choque de preço deixou de drenar poupança com renda externa ativa (§20.5).
