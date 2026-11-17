# E1 — Dados de energia: coleta antecipada (parque, potencial, acesso e fronteiras)

Escopo: **só coleta de dado**. Nada entrou na simulação, no núcleo, em `core/`, `game/`, `map/` ou UI.
Plano: `docs/plans/perf_pop_financas_plano.md`, PARTE E (E1/E5/E7).
Branch: `data/e1-energy`. Nenhum merge, nenhum push.

## Artefatos

| Arquivo | Conteúdo | Registros |
|---|---|---|
| `data/definitions/energy/power_plants_2026.json` | usinas (GPPD) + agregado por cidade, por nação × fonte e comparação nacional | 34.936 usinas (5.707 GW), 729 cidades, 161 nações |
| `data/definitions/energy/renewable_potential.json` | solar e eólico mensal; proxy hídrico | 15.547 províncias, 834 cidades, 186 tags de nação |
| `data/definitions/energy/access_losses.json` | acesso à eletricidade, perdas de rede, água potável gerida com segurança | 217 países (173 jogáveis) |
| `data/definitions/energy/land_borders.json` | pares de nações com fronteira terrestre | 299 pares, 152 nações |

Scripts reprodutíveis (`python tools/build_energy_<x>.py`), todos determinísticos — rodados 2× com SHA-256 idêntico:

```text
build_energy_plants.py     -> power_plants_2026.json     B50A48AEC100DFC7A29DEECB233C7A57B33ABDACDAB34F05FFB080494C5F965B
build_energy_renewables.py -> renewable_potential.json   E7EA779F2554C85E79F5B636C578B6DFAAFA9D6A8FA08B89AD48AD16399AF904
build_energy_access.py     -> access_losses.json         DEDF622E4B564468FC58315E366A017BAA37A7CF4E6CA38414F1853BE791AA07
build_energy_borders.py    -> land_borders.json          5465B7E5017A21C12219595E80AC657E325084DCB6235C7EE65DB7E6290F8451
```

Base comum: `tools/build_energy_common.py` — réplica em Python de `GeographyService.assign_provinces`
(província → cidade da mesma nação e *landmass*, partição por área). Validação da réplica: 834/834 cidades
com terra, Σ frações por província = 1, área por nação conservada contra o atlas.

Coletas (cache em `tools/data/`, gitignored, com URL + SHA-256 + data de acesso no manifest de cada uma):
`tools/fetch_gppd.py`, `tools/fetch_worldclim_energy.py`, `tools/fetch_wdi_energy.py`,
`tools/fetch_irena_capacity.py`.

## Fontes, licenças e citações

| Fonte | Uso | Licença | Observação |
|---|---|---|---|
| **Global Power Plant Database v1.3.0** (WRI, 2021) | parque instalado | **CC BY 4.0** | CSV 11.973.385 B, sha256 `4b1f93e0…`; 34.936 usinas, 167 países, nenhuma sem MW ou coordenada |
| **WorldClim 2.1** `srad` e `wind` (10 arc-min, mensal) | potencial solar/eólico | **não declarada** na página oficial; citação obrigatória de fato | Fick & Hijmans (2017), Int. J. Climatol. 37:4302-4315; zips com sha256 em `manifest_energy.json` |
| **World Bank WDI** via API Data360 (ind. `EG.ELC.ACCS.*`, `EG.ELC.LOSS.ZS`, `SH.H2O.SMDW.*`) | acesso, perdas, água | **CC BY 4.0** | água = base **JMP WHO/UNICEF** publicada pelo WDI; endpoint antigo `api.worldbank.org/.../indicator` está 502/timeout, por isso Data360 |
| **IRENA ELECSTAT** (`Country_ELECSTAT_2026_H2_PX.px`) | comparação de capacidade instalada 2024/2025 | **Termos IRENA, art. 4.1**: uso **não comercial** com atribuição; outros usos exigem autorização escrita | ⚠️ ver "Pendências" — restrição pode não servir a um jogo comercial |
| **IJHD, *Hydropower Essentials*** (citado em OSTI/ETDEWEB 21594387) | âncora global do proxy hídrico | texto técnico citado | "overall technical potential … more than 16 400 TWh/yr" |
| **Natural Earth 10m** (rios, admin-0/1) | rios do proxy hídrico; base do atlas | domínio público | mesmos vetores de `tools/build_atlas_layers.py` (`NE_DIR`) |

## Métodos e resultados

### 1. Parque instalado (E1)
- Atribuição: lat/lon → pixel do mapa 5000×2500 → província no raster `world_province.png` → cidade dona
  pela regra da simulação. 34.934 de 34.936 usinas caem em cidade (2 fora: ponto sem província).
- `method:"gppd_2021_unadjusted"` em **todos** os registros: o GPPD é de ~2021 e **nada foi extrapolado
  para 2026**.
- Grupos de fonte: hydro, solar, wind, coal, gas, oil, nuclear, biomass, other (Waste→biomass, Petcoke→coal,
  Geothermal/Storage/Cogeneration/Wave→other). `fuel_source` preserva o rótulo original.
- Matriz mundial (MW): coal 1.967.741 · gas 1.492.959 · hydro 1.052.995 · nuclear 407.912 · wind 263.053 ·
  oil 261.221 · solar 188.183 · biomass 48.926 · other 22.613.

**Comparação com a IRENA (capacidade instalada, 2025, grid = All)** — 160 nações comparáveis, **156
abaixo** da IRENA:

| Nações | GPPD (MW) | IRENA 2025 (MW) | Desvio |
|---|---|---|---|
| SUR | 5 | 730 | −99,3% |
| LCA | 3 | 94 | −96,8% |
| NPL | 588 | 4.084 | −85,6% |
| AGO | 1.071 | 6.522 | −83,6% |
| BEN | 61 | 337 | −82,0% |
| DOM | 1.858 | 8.419 | −77,9% |
| TZA | 995 | 4.160 | −76,1% |
| UKR | 48.570 | 27.951 | **+73,8%** |

Leitura: o desvio negativo generalizado é base desatualizada (~2021) + cobertura incompleta do GPPD
(autoprodução, usinas pequenas). O **+73,8% da Ucrânia** é o caso oposto e precisa de olho: o GPPD inclui
usinas no território ocupado e possivelmente dupla contagem de unidades térmicas grandes — **não** corrigir
sem fonte. Lista completa em `meta.national_comparison`.

### 2. Potencial solar e eólico (E1)
- Amostragem no centróide da província (mesma convenção de `province_nature.json`); centróide em oceano usa o
  pixel de terra mais próximo (deslocamento em `sample_offset_px`). 0 províncias sem amostra.
- Por província: média anual (kWh/m²/dia e kWh/m²/ano; m/s), mês mínimo/máximo. Por cidade: média ponderada
  pela **área** da província × fração da cidade, com a série mensal de 12 meses (para fator de capacidade do E2).
- Sanidade (nação → solar kWh/m²/dia · vento m/s): NGA 4,80 · 2,08; AUS 5,72 · 3,18; BRA 4,31 · 1,50;
  DEU 2,85 · 3,33; USA 3,96 · 3,55; NOR 2,32 · 3,67.
- Limite declarado: 10 arc-min é grosso para sítio e o `wind` do WorldClim suaviza relevo (subestima vento).

### 3. Potencial hídrico — proxy (E1)
- `method:"proxy_precip_relief_river"`, `confidence:"low"` em todo registro.
- `raw = precip/1000 × área × relief/1000 × runoff × river_factor`;
  `relief` = max−min da altitude dentro dos pixels de terra da província; `runoff` = coeficiente tipo Budyko
  com PET ≈ 25·T+200 mm/ano; `river_factor` = cobertura (fração da província a ≤50 km de um rio Natural Earth)
  com piso 0,02.
- **Normalização ancorada só no GLOBAL**: o mundo soma 16.400 TWh/ano (IJHD). A **divisão entre nações é do
  proxy** — `normalized:"global_anchor"`, `national_anchor_missing:true`. Não existe dataset público legível
  por máquina de potencial técnico hidrelétrico por país (IHA/WEC publicam em PDF), e transcrever à mão
  arriscaria inventar número. Top do proxy: COL 1.312 · CHN 1.271 · IND 1.122 · RUS 1.086 · IDN 1.003 ·
  USA 966 · BRA 842 · CAN 829 TWh/ano. Noruega 72, COD 274, NPL 353 — ordem de grandeza coerente, valores
  absolutos **não** são dado nacional.
- Duas correções durante a coleta: (a) rios com `scalerank ≤ 4` deixavam a Noruega com fator ~0,0002 — passou
  a usar todos os rios do Natural Earth; (b) sem coeficiente de escoamento, a Austrália aparecia em 2º lugar
  mundial (deserto grande × área) — daí o Budyko simples.
- 254 províncias (1,6%) ficam com `confidence:"no_data"`, `raw_index` 0 e `no_data_reason` (sem
  precipitação/área/relevo); contadas em `meta.counts.provinces_no_hydro_data`.

### 4. Acesso e perdas (E5/E7)
- Ano mais recente não vazio por país/indicador; ausente = `null` (sem interpolação).
- Cobertura: acesso à eletricidade 216 países (urbano/rural 214), perdas T&D 150, água segura 151
  (urbano 104, rural 85).
- 6 nações jogáveis sem registro no WDI: PSX, SAH, SOL, SSD, TWN, XKX (`SSD` existe no WDI como país `SSD` e
  entra; os demais não têm série).
- Amostra (eletricidade total/urb/rural · perdas · água total): BRA 99,8/99,9/99,2 · 14,9% · 88,6;
  NGA 62,5/85,5/23,5 · 14,1% · 29,9; NOR 100/100/100 · 4,6% · 99,1; CHN 100/100/100 · 3,1% · (só urbana 96,4).

### 5. Fronteiras terrestres (E5)
- Derivadas do raster de donos (`world_province.png`) com máscara de **terra** (`world_sdf.png`, R>128), o que
  exclui fronteira marítima. 4-vizinhança, comprimento em km com cosseno da latitude, pares < 30 km descartados.
- 299 pares entre 152 nações; 927 pares de província abaixo do mínimo; 115 descartados por nação não jogável.
- Controles negativos passaram: FRA-GBR, DNK-SWE, IDN-SGP, RUS-USA, CHN-TWN, ESP-MAR e ITA-TUN **não** aparecem.
- Controles positivos: CHN 14 vizinhos, BRA 10, DEU 9, FRA 8 (inclui BRA/SUR pela Guiana), NOR 3, USA 2.
- Comprimento é aproximado: rasterização em 4-vizinhança superestima fronteira diagonal (CAN-USA 9.841 km vs
  ~8.900 oficial; KAZ-RUS 8.983 vs ~7.644). A **lista de pares** é o dado; o km é ±20%.

## Achados de qualidade de dado (fora do escopo, mas relevantes)

1. `data/definitions/natural/province_areas.json` tem células degeneradas: a província 14087 ("Tasmania 1")
   recebe **1.781.428 km²** (Tasmânia real ≈ 68 mil km²), e a soma de AUS dá 12,1 M km² (real 7,7 M). Os
   scripts de energia **não** usam esse arquivo: a área é contada no raster do atlas
   (`build_energy_common.raster_province_area_km2`, total 145,9 M km², coerente com a terra emersa).
   Vale corrigir `tools/compute_province_areas.py` na frente do mapa.
2. GPPD: 50% das usinas sem ano de comissionamento, 14.068 sem `owner` — campos ficam `null`, sem invenção.
3. IRENA 2024/2025 tem 224 países; a comparação cobre 160 das 161 nações com usina (XKX não está na IRENA).

## Pendências / decisões para o dono

1. **Licença da IRENA é uso não comercial com atribuição.** Ela só é usada na *comparação* (não vai para o
   jogo como dado de capacidade), mas se o projeto for comercial, a comparação deve sair do repositório ou ser
   trocada por fonte CC BY. GPPD (CC BY 4.0), WDI (CC BY 4.0) e Natural Earth (domínio público) não têm esse
   problema; o WorldClim não declara licença e pede apenas atribuição acadêmica.
2. **Potencial hídrico nacional**: falta uma fonte país a país. Quando houver (IHA/WEC/IEA), basta preencher
   `national_anchor` e reescalar por nação no `build_energy_renewables.py` — o campo já existe como
   `national_anchor_missing`.
3. E2/E3/E5/E6 ainda não consomem estes arquivos: nada foi ligado à simulação.
