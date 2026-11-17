# §19 — integração dos pacotes W0/W1/W2/W3: entrega e evidências

Branch `feat/s19-int` (worktree `../opengs-wt-int`), saída de
`balance/global-resources` (1feee6c). Merges na ordem pedida: `fix/q014`
(30fdd8a), `feat/s19-w0` (9a5217e), `feat/s19-w1` (75a10e2), `feat/s19-w2`
(d9c327f), `feat/s19-w3` (5a9885a). Spec: `docs/economy/s19_urban_spec.md`.

## Conflitos resolvidos

| Arquivo | Resolução |
| --- | --- |
| `gdextension/CMakeLists.txt` | `urban_land.cpp` + `city_gov.cpp` + `housing.cpp` (os três pacotes) |
| `gdextension/src/sim/sim_core.cpp` | `demo_money_total` usa `city_gov::money_total` (caixa municipal + fundos de obra), que já cobre o caixa de onde sai o aluguel do dono público (W3) |
| `gdextension/src/sim/sim_constants.h` | blocos do W1 (solo/zona) e do W3 (moradia) lado a lado, sem colisão de nome |
| `gdextension/src/edge/save_load.cpp` | blocos aditivos `city_gov` (W2) e `housing` (W3) separados, cada um com seu `if (data.has(...))` |
| `urban_types.h` / `economy_sim.cpp` / `construction.cpp` | auto-merge limpo (campos e chamadas independentes) |

Bug de integração encontrado no merge: `CityHousing::rent_paid_today` não era
serializado — o round-trip do save zerava o aluguel do dia e o teste do W1
divergia. Corrigido em `save_load.cpp` (`rent_paid_today`).

## Ligações entre pacotes (spec §19, "Integração")

1. **W2 → W1 (obra de solo).** A prefeitura não executa mais obra própria:
   `city_gov::decide_works` lê a ocupação pela regra do W1
   (`urban_land::zone_occupancy`, medida em trabalhadores/unidades) e chama
   `urban_land::start_project` (obra §15 com marcador `urban_action`), pagando
   do **caixa da prefeitura** (`payer = &gov` → débito em `CityGov.cash`, não no
   orçamento público genérico). O custo é computado por
   `urban_land::expansion_cost_per_km2` / `densify_cost`. `CityGov::projects`
   (formato do W2) fica apenas para LER saves escritos antes desta integração.
2. **W2 → W3 (moradia social).** `housing::start_social_housing` abre o projeto
   de moradia do W3 com `HOUSING_PROJECT_GOVERNMENT`, pagando do caixa
   municipal, comprando os mesmos materiais do §15 no mercado local e pagando a
   mão de obra ao setor de construção. Na entrega, as unidades entram no
   estoque, a posse vai para `OWNER_GOVERNMENT` e a fração de posse do padrão
   vira aluguel (subsidiado). Teste: `city_gov_test` (`moradia social`).
3. **W3 (obra de moradia) usa o projeto §15?** NÃO — desvio documentado. A
   obra de moradia do W3 é um projeto de slot fixo por cidade
   (`HousingProject`) porque o §15 materializa PRÉDIO (`DemoBuilding`) e a
   moradia entrega UNIDADES agregadas num estoque por densidade × padrão; usar
   o §15 exigiria inventar um prédio por lote ou um tipo de conclusão novo.
   O que se manteve do §15: materiais comprados no mercado local com os mesmos
   pesos, pagamento à vista (material) + mão de obra ao setor de construção,
   prazo declarado, e a obra não abre sem material. Custo/entrega entram em
   `construction_revenue_pending` como qualquer obra.

## Calibração

### (a) Custo crescente do solo (spec, "Custo crescente")

- Expandir: custo/km² × `(1 + 0,5 × (raio/raio_inicial − 1))` ×
  `(1 + 0,8 × fração_da_reserva_usada)`. O raio é o disco de área igual à soma
  das zonas formais (RES+COM+IND); a área informal não entra na régua. O prazo
  do §15 também cresce com a distância.
- Adensar: LOW→MID custa 0,35 do km² de infraestrutura (reforço de água,
  esgoto e energia); MID→HIGH custa 0,85 (estrutura de aço, elevadores, redes
  pesadas) e a receita de material do §15 passa a pesar mais
  `steel_structure` (×1,8) e `electrical_material` (×1,6), renormalizada para a
  mesma fração de material do custo total.
- Terra: o preço do terreno (W1) já sobe com ocupação e renda — encarece
  qualquer obra na cidade cheia e entra no custo de construção de moradia.

### (b) Custo de infraestrutura compatível com a receita municipal

A fonte não publica custo de urbanização por cidade e a escala monetária do
modelo não é a do PIB real (o PIB simulado de São Paulo é ~4 ordens de grandeza
menor que o observado), então US$/km² da literatura não transporta. A âncora é
a RESTRIÇÃO OBSERVADA, em meses de receita municipal alvo
(`subnational_revenue_pct_gdp` × PIB do mês):

- `kDemoUrbanInfraCostMonths = 2,0` → 1 km² custa 2 meses de receita alvo;
- manutenção urbana consome 45% do alvo, sobram 55% do ano para capital;
- conta: `12 meses × 0,55 ÷ 2 meses/km² = 3,3 km²/ano` no raio inicial; os
  fatores de distância e relevo reduzem esse número conforme a cidade cresce
  (retornos decrescentes de propósito).
- sem dado municipal (cenário sintético), piso declarado de 1.000.000 XGS/km².

Medido no mundo real (31 dias, bootstrapped, DLL debug):

| Cidade | alvo/mês (XGS) | custo/km² (XGS) | km²/ano no orçamento |
| --- | --- | --- | --- |
| BRA-SAO | 723.870 | 1.447.740 | 3,30 |
| JPN-TOK | 1.189.885 | 2.379.769 | 3,30 |
| CHN-SHA | 1.093.481 | 2.186.963 | 3,30 |
| DEU-BER | 964.379 | 1.928.758 | 3,30 |
| NGA-LAG | 327.471 | 1.000.000 (piso) | 2,16 |
| IND-DEL | 7.429 | 1.000.000 (piso) | 0,05 |

### (c) Zonas COM/IND por TRABALHADOR

Antes: m² por "negócio agregado" (250 m²/negócio de serviço, 25.000 m²/planta).
Agora a capacidade da zona é o **emprego que cabe no gabarito**:

- serviços/escritório: **25 m² por trabalhador** (faixa OECD de espaço de
  escritório 15–35 m²/pessoa, meio da faixa);
- indústria urbana: **150 m² por trabalhador** (faixa 100–200 m²/pessoa em
  parques industriais/galpões com pátio e logística).

O uso da zona soma `jobs` dos negócios de serviço (COM) e `jobs` das plantas
urbanas, incluindo obras de fábrica em andamento (IND). A decisão de abrir
negócio/fábrica passa o emprego que a unidade vai criar para
`zone_has_room` — em vez de 1 "slot". Efeito: metrópole limita pelo emprego,
não pelo número de estabelecimentos agregados.

Medido no mundo real (dia 31, ocupação RES/COM/IND): São Paulo 0,210 / 0,011 /
0,000; Tóquio 0,106 / 0,003 / 0,000; Berlim 0,822 / 0,021 / 0,000; Lagos 0,748 /
0,052 / 0,000. O `zoning_split` observado da fonte (GHSL/Atlas) dá muito solo
comercial/industrial para o emprego atual — a zona COM/IND só morde em cidade
pequena (Weipa: 0,456 / 0,063) e depois do adensamento; a zona que aperta hoje
é a **residencial**, e é ela que a prefeitura vê em `decide_works`.

## Testes

Rodados na worktree, com a DLL debug da própria worktree (template_debug,
build Ninja/clang):

| Teste | Resultado |
| --- | --- |
| `economy_integration_test` | OK (schema 13, migrações v1→v3 e v7→v8) |
| `urban_load_test` | OK (amostra real, save/load v13 idêntico, v12→v13) |
| `urban_land_test` | OK (preço × ocupação, zona como limite pendente, obra §15, custo crescente, save/load, threads 1/8) |
| `city_gov_test` | OK (fluxo mensal, repasse, caixa, decisão por §15, moradia social W3, custo/km² = 2 meses do alvo, save/load, threads 1/8) |
| `housing_market_test` | OK (aluguel sobe/cai, renda do dono, conservação, favela no último caso, round-trip, threads 1/8) |
| round-trip real 60 d | 0 diffs |
| round-trip demo 60 d | 0 diffs |
| goldens S0 | regenerados (justificativa: a simulação muda por desenho com §19); threads=1 e 8 idênticos ao golden |
| sweep 365 d com IA | ver `docs/economy/perf/s19_int_world_ai_365.json` |

### Sweep 365 d (IA, seed padrão, threads auto=8, DLL debug final)

| Métrica | Valor |
| --- | --- |
| falências de construção | **8** (limite do dono 28; baseline `construction_world_sweep` 16) |
| falências de serviço / negócios | 7.152 / 52.470 (igual ao baseline de serviço) |
| dinheiro (drift) | **−0,0013 XGS** (tolerância 0,01) |
| comida por nação vs `construction_world_sweep.json` | **0 de 179 nações pioram** |
| maiores ganhos | DJI +8,1 · LBY +7,8 · JOR +7,4 · SOL +4,9 · SGP +4,5 · YEM +1,7 p.p. |

O resíduo de comida de JOR/YEM/SOL/DJI (abaixo do baseline pré-§15) é o mesmo
já aceito pelo dono em D-062/Q-014: é a lacuna de renda/tesouro de importador
sem agricultura, não a calibração urbana; contra o baseline imediatamente
anterior à integração nenhuma nação piora.

