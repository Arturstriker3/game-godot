# Parecer — D-007 Parte G (agricultura sazonal, só dados)

Verificador: verificador-flash (independente) · Worktree: `G:\projetos\opengs-wt-data-agri` (branch `data/g-agriculture`)
Data: 2026-11-18 · Nenhum arquivo de dado, código ou git foi alterado por esta revisão.

VEREDITO: **APROVADO COM RESSALVAS**

## Critério de aceite (D-007, `docs/team/QUADRO.md` linha 75)

> (1) 179/179 nações em cada arquivo; (2) só ids de primário de comida do jogo (`wheat`,`rice`,`maize`,`potato`,`soy`) + `feed`/`meat`; (3) meses 1–12, valores ≥0; (4) todo número com `source`+`year`; ausência = `no_data`/regra declarada com motivo; (5) `tools/validate_agriculture_g.py` exit 0; (6) verificador-flash confere BRA soja/milho, IND arroz/trigo, CHN arroz/trigo, USA milho/soja, NGA mandioca/milho, UKR trigo, EGY trigo.

## Comandos e saída

| # | Comando | Resultado |
|---|---|---|
| V1 | `python tools/validate_agriculture_g.py` | `EXIT=0`, `erros: 0`, `avisos: 125` (todos listados no fim da saída) |
| V2 | recomputo FAOSTAT QCL a partir de `tools/_tmp_agri_raw/FAO_QCL.zip` (itens 15/27/56/116/236/125, elementos 5312/5510, último ano ≤2024 com área>0) | 1.000 pares área×item; amostra bate 100% |
| V3 | recomputo USDA PSD (`psd_grains.zip`, `psd_oilseeds.zip`; atributos 176/125, `1000 MT`, último Market_Year ≤2024, último mês) | amostra bate 100% (estoque e dias) |
| V4 | leitura do cache `tools/_tmp_agri_raw/fubc/FUBC_1_to_9_data.csv` (`Aver_N_rate_kg_ha`) | 7/7 batem |
| V5 | parsing `tools/_tmp_agri_raw/aphlis/NG_maize.html` (tabela `Datatable`, colunas 2013–2022) | 2022 "All steps" = **17.4**; "Household-level storage" = **4.6**; "Market storage" = 0.0 |
| V6 | `pypdf` em `FAO_2011_i2697e.pdf`, Annex 4 (p. 26–27) | cereais/postharvest: N.América+Oceania **2%**, Ásia industrializada **10%**, Europa **4%**, Norte da África/Ásia Oc./Central **8%**, LatAm oleaginosas **3%** |
| V7 | `pypdf` em `inpho_WHEAT/RICE/MAIZE.pdf` + parsing `foodkeeper.json` (abas Product) | frases e IDs da fonte conferem (ver Tabela 2) |
| V8 | leitura de `fao_crop_calendar.csv` e `crop_calendar_gaul0.zip` | EGY trigo e dados dos doadores conferem |

## Cobertura e contrato
- 179 nações em `crop_calendar`, `crop_yields`, `crop_stocks`, `postharvest_losses`, `fertilizer_n`, `livestock_cycle`; 12 produtos em `product_shelf_life`; 895 entradas por arquivo de cultura (179×5).
- Ids de cultura existem em `economy/goods.json` com `kind=raw`, `food=true` (wheat/rice/maize/potato/soy). Não há id fora do conjunto (nem `cassava`).
- `product_shelf_life`: 12/12 produtos existem em `goods.json`; `livestock_cycle.products`: `meat` (bem do jogo) e `milk` com `in_game:false` — correto, `milk` **não** existe em `goods.json`.
- Formato: UTF-8 sem BOM, sem CRLF (checagem 11 do validador).

## Tabela 1 — amostra contra a fonte (recomputada do cache bruto)

| Entrada | JSON | Recomputado da fonte | Veredito |
|---|---|---|---|
| BRA soy yield | área 45.906.902 ha · prod 144.473.768 t · 3,147 t/ha · 2024 · flag A | QCL Brazil/236 2024: 45.906.902 / 144.473.768 → 3,147 | OK |
| BRA maize yield | 21.186.816 ha · 114.953.303 t · 5,426 t/ha · 2024 · A | QCL Brazil/56 2024: 21.186.816 / 114.953.303 → 5,426 | OK |
| BRA soy stock | 37.688.000 t · 221,3 d · my 2024 | PSD Brazil/Soybean my2024: ending 37.688 · dom.cons. 62.162 → 221,3 d | OK |
| BRA maize stock | 10.604.000 t · 41,8 d · 2024 | PSD Brazil/Corn: 10.604 · 92.500 → 41,8 d | OK |
| IND rice yield | 50.511.930 ha · 217.867.864,2 t · 4,313 · 2024 | QCL India/27: idem | OK |
| IND wheat yield | 31.832.884 ha · 113.292.368 t · 3,559 · 2024 | QCL India/15: idem | OK |
| IND rice stock | 48.000.000 t · 144,4 d | PSD India/Rice,Milled: 48.000 · 121.357 → 144,4 | OK |
| IND wheat stock | 11.800.000 t · 39,5 d | PSD India/Wheat: 11.800 · 108.961 → 39,5 | OK |
| CHN rice yield | 29.006.900 ha · 207.530.000 t · 7,155 · `faostat_area="China, mainland"` | QCL *China, mainland*/27: idem (agregado "China"=351 daria 29.256.900/209.173.000 — **não** usado) | OK |
| CHN wheat yield | 23.587.400 ha · 140.100.000 t · 5,94 | QCL *China, mainland*/15: idem | OK |
| CHN rice stock | 104.500.000 t · 263,1 d | PSD China/Rice: 104.500 · 144.957 → 263,1 | OK |
| CHN wheat stock | 127.775.000 t · 310,9 d | PSD China/Wheat: 127.775 · 150.000 → 310,9 | OK |
| USA maize yield | 33.547.180 ha · 377.632.690 t · 11,257 · 2024 | QCL USA/56: idem | OK |
| USA soy yield | 34.823.570 ha · 118.836.440 t · 3,413 · 2024 | QCL USA/236: idem | OK |
| USA maize stock | 39.404.000 t · 46,2 d | PSD USA/Corn: 39.404 · 311.240 → 46,2 | OK |
| USA soy stock | 8.840.000 t · 46,9 d | PSD USA/Soybean: 8.840 · 68.837 → 46,9 | OK |
| NGA maize yield | 5.500.000 ha · 11.164.000 t · 2,03 · flag X | QCL Nigeria/56 2024: idem | OK |
| NGA maize stock | 331.000 t · 10,2 d | PSD Nigeria/Corn: 331 · 11.800 → 10,2 | OK |
| NGA maize fertilizante | 9,2 kg N/ha · 2017 | FUBC 9 Nigeria/Maize/2017 `Aver_N_rate`=9,1815 | OK |
| NGA maize perdas | 17,4% · armazém 4,6% · 2022 | APHLIS NG/maize 2022: All steps 17,4; Household storage 4,6; Market 0,0 | OK |
| **NGA mandioca** | **id `cassava` inexistente** (correto) | QCL Nigeria/item **125 "Cassava; fresh"** 2024: **10.722.539 ha / 62.624.946,19 t** (flag I/E) | OK (exclusão correta) |
| UKR wheat yield | 4.883.800 ha · 22.441.110 t · 4,595 · 2024 | QCL Ukraine/15: idem | OK |
| UKR wheat stock | 726.000 t · 31,2 d | PSD Ukraine/Wheat: 726 · 8.500 → 31,2 | OK |
| UKR wheat fertilizante | 103,8 · 2018 | FUBC Ukraine/Wheat/2018: 103,766 | OK |
| UKR wheat perdas | 4,0% (Europa) | FAO 2011 p.26 Europa/cereais postharvest = 4% | OK |
| EGY wheat yield | 1.386.892 ha · 9.400.000 t · 6,778 · flag X | QCL Egypt/15 2024: idem | OK |
| EGY wheat stock | 2.911.000 t · 53,1 d | PSD Egypt/Wheat: 2.911 · 20.000 → 53,1 | OK |
| EGY wheat fertilizante | 180,4 · 2019 | FUBC Egypt/Wheat/2019: 180,365 | OK |
| EGY wheat perdas | 8,0% | FAO 2011 p.27 Norte da África/Ásia Oc.-Central cereais = 8% | OK |
| IND wheat fertilizante | 112,3 · 2018 | FUBC India/Wheat/2018: 112,331 | OK |
| CHN rice fertilizante | 177,6 · 2018 | FUBC China/Rice/2018: 177,6 | OK |
| BRA soy fertilizante | 15,7 · 2018 | FUBC Brazil/Soybeans/2018: 15,667 | OK |
| USA maize fertilizante | 157,6 · 2019 | FUBC USA/Maize/2019: 157,648 | OK |
| USA maize perdas | 2,0% | FAO 2011 p.26 N.América+Oceania cereais = 2% | OK |
| CHN rice perdas | 10,0% | FAO 2011 p.26 Ásia industrializada cereais = 10% | OK |
| EGY wheat calendário | `observed`, plantio 11, colheita 4–5, `window_labels` "Nov 15-Nov 30"/"Apr 25-May 25", source_year 2021 | FAO CSV EGY001–EGY012: Wheat, bread Sowing Nov 15–Nov 30 / Harvesting Apr 25–May 25, lastUpdated 2021-02-26 | OK |
| BRA soy calendário | `rule_neighbor`, tier `same_region`, donor BOL, 2.136,9 km | BOL é `observed`; FAO BOL Soybean Sowing Jun 01–Jun 30 + Nov 15–Dec 20 / Harvesting Oct 10–Nov 20 + Mar 01–Apr 15; JSON idêntico ao doador | OK |
| IND rice calendário | `rule_neighbor`, `same_region`, donor MMR, 1.965,6 km | MMR `observed`; janelas MMR idênticas; nota declara a agregação | OK |
| IND wheat calendário | `rule_neighbor`, `same_region`, donor MMR | MMR wheat Sowing Sep–Dec / Harvesting Feb–Apr; JSON idêntico | OK |
| CHN rice calendário | `rule_neighbor`, `same_region`, donor BGD, 2.495,7 km | JRC BGD: Aman 15-23/29-36, Aus 4-13/16-23, Boro 34-6/10-15 → P 12–8 / H 4–12 idêntico | OK |
| USA maize calendário | `rule_neighbor`, `same_band_nearest`, donor PRK, 10.726,5 km | JRC "North Korea" Maize sos 9-16 eos 28-32 → P 3–6 / H 10–11; USA não existe no JRC | OK (herança distante) |
| UKR wheat calendário | `rule_neighbor`, `same_band_nearest`, donor AZE, 1.689,1 km | FAO AZE wheat: 8 janelas de semeadura e 9 de colheita, exatamente as `window_labels` do JSON | OK (herança fraca) |

Auditoria global adicional (não exigida, feita para descartar mascaramento):
- 658/658 entradas `rule_neighbor` têm meses **idênticos** aos do doador, e **658/658** doadores são `observed`.
- Nenhuma entrada `observed` tem `rule`; nenhuma `rule_neighbor` sem `rule.tier/donor/distance_km`; nenhuma safra sem `source`.
- Todas as 895 entradas têm `source`; **0** entradas com dado e `source` nulo.

## Tabela 2 — prateleira (3 produtos escolhidos) e demais observados

| Produto | JSON | Fonte citada | Conferido no cache | Veredito |
|---|---|---|---|---|
| wheat | 365 d · `ambient_dry` · INPhO | "For longer periods of storage from 4-12 months, the moisture content must be reduced to 13 percentage or below" | frase literal em `inpho_WHEAT.pdf`; 12×30,4=364,8→365 | OK |
| maize | 274 d · INPhO | "season- long storage (6-9 months)" | frase literal em `inpho_MAIZE.pdf`; 9×30,4=273,6→274 | OK |
| potato | 61 d · FoodKeeper id 297 | `DOP_Pantry` 1–2 Months | `foodkeeper.json` Product ID 297 "Potatoes": 1/2 Months; 2×30,4=60,8→61 | OK |
| rice | 365 d | INPhO "6-12 months" (paddy) | frase literal em `inpho_RICE.pdf` | OK |
| flour 365 · bread 18 · bagged_rice 730 · meat 5 · canned 1825 | FoodKeeper ids 222/195/338/40/372 | — | valores e métricas batem no cache | OK |
| soy / bran / feed | `null` + `no_data` + motivo | — | FoodKeeper não tem bran/feed; INPhO soja só dá duração de armazenagem (não vida útil) | OK |

Todos os 9 valores `observed` têm `source` **e** `url` (nenhum valor sem URL).

## Tabela 3 — amostra aleatória fora do escopo (semente 7, 5 nações × 2 culturas)

| Entrada | JSON | Recomputado | Veredito |
|---|---|---|---|
| LTU potato | 14.670 ha · 265.330 t · 18,087 | QCL: idem | OK |
| DNK wheat | 475.600 ha · 3.348.010 t · 7,04 | QCL: idem | OK |
| NER rice | 48.714 ha · 164.768 t · 3,382 | QCL: idem | OK |
| BEN rice | 128.501 ha · 500.605,3 t · 3,896 | QCL: 500.605,34 | OK |
| BLR maize | 300.000 ha · 1.700.000 t · 5,667 | QCL: idem | OK |
| TJK wheat | 270.641 ha · 880.734 t · 3,254 | QCL: idem | OK |
| CAF maize | 270.000 ha · 200.000 t · 0,741 | QCL: idem | OK |
| MMR soy | 132.000 ha · 140.000 t · 1,061 | QCL: idem | OK |
| VEN soy | 5.867 ha · 11.057,2 t · 1,885 | QCL: 11.057,23 | OK |
| BGD maize | 496.681 ha · 4.592.508 t · 9,246 | QCL: idem | OK |
| BLR/TJK/CAF/VEN/BGD stocks | 177.000/39,2 · 397.000/67,4 · 10.000/18,2 · 23.000/74,3 · 134.000/6,9 | PSD my2024: idênticos | OK |

## Diff conferido

Somente arquivos do escopo: `data/definitions/agriculture/*.json` (7 arquivos novos), `tools/agri_common.py`, `tools/build_agri_calendar.py`, `tools/build_agri_fertilizer.py`, `tools/build_agri_livestock.py`, `tools/build_agri_losses.py`, `tools/build_agri_yields.py`, `tools/validate_agriculture_g.py` (novos) e `tools/_tmp_agri_raw/` (cache, untracked). O único arquivo rastreado modificado é `docs/team/QUADRO.md` (a própria linha D-007). Nenhum arquivo de `game/`, `core/`, `gdextension/`, `map/`, `ui/`, `assets/`, `data/definitions/economy/` ou `scenario/` foi tocado. Nenhum teste existente foi alterado.

## Divergências

Nenhuma divergência numérica. Divergências de contrato/metadata:

1. **`product_shelf_life` não tem `year`** em nenhum dos 12 produtos (nove `observed` com `source`, `url`, `notes`, mas sem ano de publicação; `sources.FOODKEEPER`/`FAO_INPHO_*` têm `accessed` e não `year`). O critério (4) pede `source`+`year` "quando aplicável" — aqui o ano é aplicável e está ausente. Arquivo/campo: `data/definitions/agriculture/product_shelf_life.json` → `products.*.year` (inexistente) e `sources.*.year`.
2. **Ausência da mandioca não está declarada em lugar nenhum.** O único rastro é `fertilizer_n._meta.crop_labels_excluded` conter `"Cassava"` no meio de ~400 rótulos do FUBC — o que é uma lista de exclusão de rótulo da fonte, não uma declaração de que a mandioca (10,72 Mha / 62,62 Mt na Nigéria em 2024, ~2× a área e ~5,6× a produção do milho nigeriano) está fora do jogo por não ser bem do jogo. `docs/data/agriculture_g.md` ainda não existe (é o passo 7, do gerente) e `_meta` dos 7 arquivos não menciona o assunto. Arquivo/campo: nenhum (falta).
3. **O validador eleva o teto de sanidade da batata para 60 t/ha** (briefing: 30 t/ha) e trata 30–60 como aviso — 39 avisos de batata. Não viola o critério D-007 (que só exige valores ≥0) e está documentado no docstring e na saída; fica registrado para o gerente saber que a checagem não é a do briefing ao pé da letra.

## Ressalvas e limitações (dado fraco, mas declarado)

- **Calendário herdado**: 658/895 (73,5%) são `rule_neighbor` (350 `same_region`, 263 `same_band_nearest`, 45 `adjacent_band`); só 237 são observados. **95 entradas herdam de doador a >5.000 km**, com casos extremos: NZL soy ← ZAF 11.827,8 km; AUS rice ← CHL 11.748,4 km; **USA maize e USA rice ← PRK 10.726,5 km**; AUS wheat ← ZAF 10.552,5 km; USA soy ← AZE 10.489,8 km.
- Heranças de qualidade duvidosa mesmo quando "perto" ou na mesma faixa: **UKR wheat ← AZE** (janelas de primavera+outono do Azerbaijão agregadas em plantio 9–5 / colheita 5–9, contra o real set–out/jul–ago da Ucrânia) e **DNK wheat ← TUN** (`adjacent_band`, 2.137,7 km, clima oposto). Os meses são fiéis ao doador, mas o doador é fraco.
- Três entradas herdadas misturam duas safras do doador (BRA soy, entre elas): a safra 1 de BRA soy (plantio 6 / colheita 10–11) vem da "segunda safra" boliviana e não descreve a safra principal brasileira; está como `rule_neighbor` (não mascarado).
- `postharvest_losses`: **677/895 (75,6%) são `regional_observed` do FAO 2011** (taxa regional aplicada ao país, ano 2011, etapa "postharvest handling and storage", % do que entra na etapa, não % da produção); só 86 são APHLIS por país (40 nações, só África subsaariana) e 132 são `regional_default` (mediana da região do jogo). O `method` de cada entrada declara isso.
- `crop_stocks`: **26 entradas USDA_PSD com `marketing_year` 1990 (7, soja europeia) ou 1998 (19, trigo/milho do Leste europeu)**; todas as demais são 2024. O `note` registra "PSD sem estoque > 0 em 2024".
- `crop_yields`: **20 entradas com `year` < 2020** (mínimos 1977 GUY soy, 1977 JOR soy, 1979 LCA maize, 1980 GBR maize, 1982 CHL soy) — é o último ano com área>0 na fonte, então é o dado honesto, mas está muito defasado. 241 entradas `no_data` (SAH, SOL, TWN, XKX e culturas sem área no FAOSTAT).
- `fertilizer_n`: 311 observados, todos FUBC; **91 entradas observadas vêm de levantamentos FUBC 1–5 (1984–2001)**, porque não há taxa no relatório 9 para esse país/cultura — o `year` de cada entrada expõe isso. 584 são `regional_default` (mediana declarada).
- Estoques `rule_days_consumption` usam consumo PSD quando existe **em qualquer ano** (pode ser 1998) e, senão, produção FAOSTAT como proxy — declarado em `consumption_basis`/`note`, mas `marketing_year` fica 2024 mesmo quando a base é antiga.
- `product_shelf_life`: 3 de 12 são `no_data` (soy, bran, feed) — declarado com motivo; não há valor inventado.
- `tools/_tmp_agri_raw/` (~48 MB de cache: QCL 34 MB, PSD 6,7 MB, HTML/PDF) está **untracked e não ignorado**; precisa ficar fora do commit do gerente.

## Falta para aprovar sem ressalvas

1. Declarar a mandioca em um lugar explícito: nota em `_meta` (ex.: `crop_yields._meta.crops_not_in_game` com `cassava` + motivo) e/ou seção do `docs/data/agriculture_g.md` dizendo que mandioca/milho são os dois principais cultivos nigerianos e só o milho é bem do jogo (FAOSTAT 2024: 10,72 Mha / 62,62 Mt).
2. Dar `year` (ou uma nota "a fonte não publica ano; ver `sources.*.accessed`") aos 12 produtos de `product_shelf_life`, como já se fez para o JRC no calendário.
3. Manter `tools/_tmp_agri_raw/` fora do commit e registrar as limitações acima (calendário herdado >5.000 km, estoque 1990/1998, perda regional FAO 2011, fertilizante pré-2002, cotas de batata) no `docs/data/agriculture_g.md` — passo 7 do gerente.

---

**Nota do gerente-dados (2026-11-18, depois do parecer):** a ressalva 1 foi fechada — a mandioca da NGA passou a existir em `crop_yields.json → non_game_reference.NGA.crops.cassava` com `in_game: false` (FAOSTAT item 125, 2024: 10.722.539 ha / 62.624.946,19 t / 5,840 t/ha), declarada no doc §1. A ressalva 3 foi atendida no doc §6 e o cache foi apagado (os construtores têm `--fetch`). A ressalva 2 (`product_shelf_life` sem `year`) segue aberta e documentada no §6.6: prateleira é valor de referência, não série anual.
