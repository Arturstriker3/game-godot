# B3 — População real por província e por cidade: aglomeração + interior

> Parte B3 do plano `perf_pop_financas_plano.md` ("Cidade = aglomeração + interior"),
> executada em 2026-11-17 na branch `data/b3-hinterland-pop`. Somente dados: nenhum
> código de jogo, C++, `core/`, `game/`, `map/` foi tocado e o Godot não foi rodado.
>
> Entrega: `data/definitions/population/hinterland_2026.json` (834 cidades, 179 nações)
> gerado por `tools/build_hinterland_pop.py` a partir do raster WorldPop 2026.
> O arquivo ainda **não é consumido** por nenhum código; a integração na simulação é a
> etapa seguinte de B3.

## 1. Fonte (o que foi baixado)

| | |
|---|---|
| Fonte | **WorldPop Global2 R2025A v1** (`alpha`), ano **2026**, 1 km, mosaico global *constrained* |
| Arquivo | `global_pop_2026_CN_1km_R2025A_v1.tif` — 289.556.261 B (276 MiB) |
| URL | `https://data.worldpop.org/GIS/Population/Global_2015_2030/R2025A/2026/0_Mosaicked/v1/1km/constrained/global_pop_2026_CN_1km_R2025A_v1.tif` |
| SHA-256 | `61c487e765ef904caf3239d66d635465f5871d3ee6684ffe604d82e25bff0aa8` |
| Coleta | 2026-11-17 |
| Licença | **CC BY 4.0** — WorldPop, University of Southampton (DOI `10.5258/SOTON/WP00845`) |
| Grade | WGS84, 1/120° (30 arc-seg), **43200 × 17280**, origem lon −180 / lat **84** até **−60** |
| Nodata | **−99999** (mar e área não modelada) — tratado como 0 |
| Compressão | LZW, tiles de 512×512, `float32` |

O arquivo fica em `tools/data/worldpop/` (gitignored) com o registro de procedência em
`tools/data/worldpop/worldpop_sources.json`. O SHA-256 e a URL também estão gravados
dentro do JSON entregue.

Decisão de fonte: ver `b3_population_raster_options.md` (WorldPop 2026 escolhido sobre
GPW v4.11 2020 por ter o **ano exato** e totais alinhados à WPP 2024, a mesma base da
pirâmide etária de B1).

## 2. Método (passo a passo)

1. **Reamostragem 1 km → pixel do atlas.** Para cada célula de 30 arc-seg, o **centro**
   cai no pixel do atlas 8192 × 4096 por
   `col = floor((lon+180)·8192/360)`, `row = floor((90−lat)·4096/180)`.
   Sem interpolação e sem repartir área: a célula de ~0,93 km² no equador entra inteira
   no pixel do atlas (~17,5 km × 17,5 km no equador, encolhendo com cos(lat)).
   O passo é 5,2734 células WorldPop por pixel do atlas nos dois eixos; os tiles de borda
   são cortados e as células de nodata (< 0) viram 0.
2. **Soma por província.** A célula entra na província cujo pixel tem id em
   `map/map_data/atlas/world_province.png` (`id = R + G·256`).
   *A máscara de terra/nav do `compute_province_areas.py` (`world_sdf` × `nav_mask`)**não**
   é aplicada aqui*: medi-la mostrou que ela corta **147,6 M de pessoas (1,8% do mundo)**,
   quase tudo em centros urbanos costeiros (Istambul, Manila, Lagos, Hong Kong, Kochi) e
   nas ilhas removidas pela curadoria. A célula populada mora dentro da província
   desenhada, então ela é da província; a máscara continua valendo para **área**.
   Resultado: 100% da população do raster cai dentro de alguma província (0 fora).
3. **Província → cidade dona.** Réplica em Python de
   `GeographyService.assign_provinces` + `_share_starved_provinces`
   (`tools/geography_replica.py`): a cidade da mesma nação mais próxima do centróide da
   província, restrita à mesma `landmass`; cidade sem terra recebe fração da província
   mais próxima (3 casos hoje: PRI-PON/PRI-SAN, JPN-NAH/JPN-MIY, NPL-KAT/NPL-BHA).
   Nenhuma das 834 cidades ficou sem província (1 a 186 províncias por cidade).
4. **Fator por nação.** `factor = nations.json[pop] / soma_raster_das_províncias_da_nação`;
   `population_region = soma_raster · factor`, arredondada em **inteiros por maior resto**
   dentro da nação, de modo que a soma das cidades de cada nação é **exatamente** o total
   de `nations.json` (verificado: 179/179 nações fecham).
5. **Interior.** `hinterland_population = population_region − urban_population`
   (`urban_population` de `cities.json`; piso 0, negativos listados na §5).

## 3. Resultados

- População do raster: **8.264.687.067**. Em províncias do atlas: **8.264.687.067** (100%).
  Fora de nação jogável (Kashmir/KAS, Antártica, ilhas sem curadoria): **4.400**.
- Região somada das 834 cidades: **8.203.501.062** = total de `nations.json` (2026).
- Urbano (aglomeração): **2.110.546.632** (1,923 bi de `agglomeration`, 181 M de
  `sim_population`, 6,1 M de `agglomeration_capped`).
- Interior: **6.115.528.841**.
- Fator por nação: mediana **0,9941**; p05 0,8539; p95 1,0710.
  35 de 179 nações fora de ±5% e 16 fora de ±10% — as 7 piores:
  SAH 2,71 · DMA 0,136 · KNA 0,318 · LCA 0,413 · FJI 0,577 · GNQ 1,409 · SOL 1,328.
  Causa: províncias pequenas/insulares cujo polígono do atlas cobre mais (ou menos)
  território do que a nação (Antilhas, Fiji, Saara Ocidental) — o fator preserva o total
  nacional e concentra o erro dentro da própria nação. Ficam como lista de revisão.
- 940 províncias de nações jogáveis sem população no raster: 132 não têm nenhum pixel no
  atlas (as mesmas do fallback de área) e as demais são unidades menores que um pixel
  (comunas da Suíça/San Marino, castelli, Melilla/Ceuta) ou vazias por natureza
  (Ártico: CAN 386, RUS 241, GRL/DNK 201). A população delas entra pelo fator da nação.

### Top 20 cidades por interior

| cidade | região | urbano | interior |
|---|---:|---:|---:|
| IND-PAT Patna | 226.162.854 | 3.000.000 | 223.162.854 |
| CHN-WUH Wuhan | 152.628.770 | 12.900.000 | 139.728.770 |
| CHN-JIN Jinan | 136.091.156 | 6.950.000 | 129.141.156 |
| IND-KOL Kolkata | 130.710.619 | 18.100.000 | 112.610.619 |
| IND-LUC Lucknow | 108.544.665 | 4.325.000 | 104.219.665 |
| CHN-CHO Chongqing | 91.897.319 | 13.200.000 | 78.697.319 |
| IND-DEL New Delhi | 112.547.743 | 36.900.000 | 75.647.743 |
| CHN-XIAN Xi'an | 86.895.831 | 13.700.000 | 73.195.831 |
| IND-LUD Ludhiana | 71.914.201 | 2.175.000 | 69.739.201 |
| IND-HYD Hyderabad | 72.835.890 | 12.100.000 | 60.735.890 |
| CHN-KUN Kunming | 66.092.389 | 6.950.000 | 59.142.389 |
| IND-NAG Nagpur | 62.427.540 | 3.625.000 | 58.802.540 |
| IND-JAI Jaipur | 60.777.115 | 4.650.000 | 56.127.115 |
| IND-IMP Imphal | 55.858.248 | 277.196 | 55.581.052 |
| CHN-BEI Beijing | 76.295.642 | 21.800.000 | 54.495.642 |
| IND-KAN Kanpur | 58.157.725 | 3.750.000 | 54.407.725 |
| CHN-HEF Hefei | 61.165.454 | 8.250.000 | 52.915.454 |
| CHN-NAN Nanjing | 61.686.971 | 9.750.000 | 52.936.971 |
| IND-AHM Ahmedabad | 60.615.802 | 10.200.000 | 50.415.802 |
| IND-BAN Bengaluru | 65.293.411 | 15.300.000 | 49.993.411 |

A irregularidade (Patna com 226 M) **não é erro de dado**: é a regra A1 vigente
(província = admin-1 grande; dona = cidade mais próxima do centróide). A Índia tem 25
cidades para 322 distritos, então um município do Bihar herda províncias vizinhas inteiras.
Nenhum rateio proporcional à população urbana foi aplicado — exatamente como o plano manda.

## 4. Validação

### 4.1 Invariantes (todas passam)

| checagem | resultado |
|---|---|
| total mundial do raster | 8,2647 bi (+0,7% da WPP 2026; o mosaico inclui a faixa habitada inteira) |
| soma das cidades = `nations.json` | **igual, 179/179 nações, total 8.203.501.062** |
| cidade sem província | 0 de 834 |
| hinterland negativo (após piso) | 7 cidades listadas abaixo |
| rodar 2× → mesmo SHA-256 | **sim** (`ec00fe68…`, ver §6) |

### 4.2 Dez nações conferidas contra a urbanização da ONU (WUP via Banco Mundial)

Referência: `SP.URB.TOTL.IN.ZS`, ano 2025, consultado em 2026-11-17
(`data/definitions/population/urban_share_reference.json`).

| ISO3 | região | urbano (nosso) | % urb | WUP 2025 | desvio |
|---|---:|---:|---:|---:|---:|
| CHN | 1.406.585.000 | 319.777.297 | 22,7 | 66,3 | −43,6 |
| IND | 1.463.865.525 | 192.718.747 | 13,2 | 35,7 | −22,5 |
| USA | 341.784.857 | 132.713.081 | 38,8 | 80,2 | −41,4 |
| IDN | 285.721.236 | 63.169.765 | 22,1 | 59,4 | −37,3 |
| PAK | 255.219.554 | 59.237.147 | 23,2 | 39,5 | −16,3 |
| NGA | 237.527.782 | 47.555.000 | 20,0 | 63,8 | −43,8 |
| BRA | 212.812.405 | 83.253.122 | 39,1 | 88,2 | −49,1 |
| BGD | 175.686.899 | 31.249.154 | 17,8 | 33,2 | −15,5 |
| RUS | 143.513.328 | 50.929.889 | 35,5 | 75,3 | −39,8 |
| ETH | 135.472.051 | 8.036.400 | 5,9 | 24,1 | −18,2 |

**O total nacional está certo; a fatia urbana não.** O `urban_population` de `cities.json`
só soma as aglomerações do roster e as *city proper* do GeoNames, não a população urbana
do país: no mundo dá **25,7%** contra **~57%** reais. Ou seja, o segmento "urbano" de B3
hoje representa metade dos urbanitas e a outra metade cai no interior — com as regras
rurais de emprego/renda/moradia aplicadas a gente que mora em cidade.

Isso **não** foi corrigido aqui (mexeria no contrato de `urban_population`, que alimenta a
habitação). É a decisão pendente nº 1 para o dono (ver §5).

### 4.3 Cidades com interior zero e com região < aglomeração

Interior zero (8): `AUS-BRI`, `IND-MUM`, `JPN-OSA`, `KOR-BUS`, `KWT-KUW`, `MEX-JUZ`,
`SGP-SIN`, `USA-POR`.

Sete delas são região **menor** que a aglomeração (o interior foi a 0 pelo piso; o valor
negativo está registrado no log e aqui):

| cidade | região | aglomeracão | leitura |
|---|---:|---:|---|
| IND-MUM Mumbai | 15.725.808 | 28.100.000 | a aglomeração é metropolitana e atravessa distritos que pertencem a outras cidades |
| JPN-OSA Osaka | 10.740.441 | 17.700.000 | idem (Keihanshin) |
| KWT-KUW Kuwait | 3.603.680 | 4.825.000 | aglomeração inclui subúrbios fora da província |
| KOR-BUS Busan | 3.801.247 | 3.950.000 | idem |
| AUS-BRI Brisbane | 2.803.065 | 3.575.000 | idem |
| USA-POR Portland | 1.650.680 | 2.475.000 | idem |
| MEX-JUZ Juárez | 1.385.668 | 1.660.000 | idem |

`SGP-SIN` é cidade-Estado: região = aglomeração = 6.111.175, interior 0 por natureza.

### 4.4 Conferência independente

Um verificador Flash separado reexecutou o construtor (mesmo SHA), reconferiu o
invariante nacional (0 de 179 nações falham), a integridade das 834 cidades
(`hinterland == max(0, região − urbano)`, `urban_population` idêntico a `cities.json`),
o SHA-256 do TIFF e a réplica do `geography_service.gd` — **aprovado**. Notas do parecer:
a réplica usa float64 onde o GD usa `Vector2` (só mudaria em empate exato, não observado) e
não usa o fallback `_city_landmass` (inócuo: as 834 cidades têm `landmass` em `cities.json`).

Amostra contra fonte externa (Banco Mundial `SP.POP.TOTL`, 2025):

| nação | raster | Banco Mundial | desvio |
|---|---:|---:|---:|
| BRA | 213.045.274,6 | 212.812.405 | +0,11% |
| JPN | 122.751.958,5 | 123.366.734 | −0,50% |
| NGA | 239.210.682,2 | 237.527.782 | +0,71% |

Estados brasileiros contra o Censo 2022 (IBGE, agregado 4709): Bahia 14.373.002 no raster
vs 14.141.626 (+1,64%); Minas Gerais 21.451.570 vs 20.539.989 (+4,44%) — coerente com o
raster de 2026 (+4,9% sobre o Censo de 2022 no país inteiro).

## 5. Limitações e decisões pendentes

1. **Urbanização (bloqueante para o aceite de B3).** Com `urban_population` como está, a
   fatia urbana simulada é 25,7% contra ~57% reais. Opções: (a) derivar `urban_population`
   da WUP por nação (novo dado, mexe no contrato da habitação); (b) aceitar o interior
   "inchado" e reancorar as regras rurais; (c) tratar o excedente urbano como um terceiro
   segmento. Só o dono decide.
2. **Aglomeração cross-provincial** empurra 7 cidades para região < urbano. Alternativa
   futura: dar à cidade a província que contém o núcleo da aglomeração, não só as do
   centróide mais próximo. Hoje o dado segue a regra A1 vigente.
3. **Fatores extremos** (DMA 0,136; KNA 0,318; LCA 0,413; FJI 0,577; SAH 2,71): polígonos
   do atlas que cobrem mais/menos que a nação. O total nacional fica certo; a distribuição
   interna dessas nações pequenas fica distorcida. Lista completa sai no log do script.
4. **Faixa do mosaico**: cobre 84°N a 60°S. Antártica, Ilhas Sandwich etc. não existem no
   raster — nenhuma delas tem população nem é jogável.
5. **Release alpha**: `R2025A v1` pode ser substituído pela WorldPop. O SHA-256 fixa a
   versão usada; qualquer troca de raster muda o SHA do JSON e aparece no relatório.

## 6. Reprodutibilidade

```powershell
# download (uma vez, 289.556.261 B)
curl.exe -L -o tools/data/worldpop/global_pop_2026_CN_1km_R2025A_v1.tif `
  "https://data.worldpop.org/GIS/Population/Global_2015_2030/R2025A/2026/0_Mosaicked/v1/1km/constrained/global_pop_2026_CN_1km_R2025A_v1.tif"

$env:OMP_NUM_THREADS = "1"          # 1 núcleo: nada de competir com a A6
G:/tools/cpp-tools/Scripts/python.exe tools/build_hinterland_pop.py
```

- Tempo: **17 s** (sha256 do TIFF 2 s, PNGs 2 s, raster 12 s, resto < 1 s), 1 núcleo,
  pico de memória ~500 MB.
- Determinístico: duas execuções seguidas geram o mesmo arquivo,
  SHA-256 `ec00fe68c0a272fa7dd0188464e530de3288f208513ae5c5e2a1d578f2e3f934`
  (sem data de execução no JSON; ordem fixa; soma em float64; arredondamento por maior
  resto com desempate pela chave).
- Dependências: só `numpy`, `tifffile` (+ `imagecodecs` para LZW), `Pillow` — as mesmas
  já usadas em `tools/`; nada novo instalado.

## 7. Formato da entrega

`data/definitions/population/hinterland_2026.json`

```jsonc
{
  "version": "B3-hinterland-2026-v1",
  "source": "WorldPop Global2 R2025A v1 — global_pop_2026_CN_1km_R2025A_v1.tif …",
  "url": "https://data.worldpop.org/…/global_pop_2026_CN_1km_R2025A_v1.tif",
  "sha256": "61c487e7…",
  "retrieved": "2026-11-17",
  "license": "CC BY 4.0 (WorldPop, University of Southampton)",
  "method": "…",
  "nations": { "IND": { "factor": 0.995551, "raster_population": 1470406826.7,
                        "target_population": 1463865525 } },
  "cities": { "IND-PAT": { "population_region": 226162854,
                           "urban_population": 3000000,
                           "hinterland_population": 223162854,
                           "provinces_count": 24,
                           "region_raw": 226162854.4,
                           "urban_population_source": "agglomeration" } }
}
```

Arquivos novos/alterados nesta branch:

- `tools/build_hinterland_pop.py` — construtor determinístico.
- `tools/geography_replica.py` — réplica de `assign_provinces` (reutilizável por outros
  construtores offline).
- `data/definitions/population/hinterland_2026.json` — a entrega.
- `data/definitions/population/urban_share_reference.json` — referência WUP/BM para a
  validação (10 nações, com URL, ano e data de coleta).
- `docs/plans/b3_hinterland_data.md` — este relatório.
