# Área agrícola e Gini fundiário por nação — insumo da B6

> Fase **B6** (patrimônio imobiliário da região). Dois dados por nação, ambos com fonte:
> **área de terra agrícola em hectares** e **concentração fundiária (Gini de terra)**.
> Onde a fonte não cobre a nação, o campo é `null` com regra declarada — nenhum número
> foi estimado, interpolado ou herdado de vizinho.

- Dataset: `data/definitions/economy/land_property.json`
- Validador: `tools/validate_land_property.py` (Python puro, exit ≠ 0 em erro)
- Branch/worktree: `feat/b6-region-property` em `G:/projetos/opengs-wt-b6`
- **Não existe** `land_property_states.json`: ver §3.3.

## 1. O que o arquivo tem

Uma entrada por nação de `data/definitions/scenario/nations.json` (**179**, os mesmos ids;
o validador compara o conjunto exato):

| campo | unidade | regra |
|---|---|---|
| `agricultural_land_ha` | hectares (inteiro) | FAOSTAT item `00006610` "Agricultural land", ano **2023** |
| `year` | ano | o mesmo da série |
| `source` | chave de `sources` | `faostat_land_use` nas 178 com dado |
| `land_gini` | índice `0..1` (4 casas) | FAO LINEQ v1, `lo_gini_all`, censo mais recente da nação |
| `land_gini_year` | ano do censo/levantamento | `source_year` da linha usada |
| `land_gini_source` | chave de `sources` | `fao_lineq_v1` nas 155 com dado |
| `land_gini_census` | texto | nome do censo/levantamento de origem |
| `basis` | `observed` \| `none` | `observed` = `agricultural_land_ha` veio da fonte; `none` = sem dado |

Blocos de apoio: `coverage` (contagens recontadas pelo validador), `sources`,
`method`, `cross_check`, `regional_reference` (medianas derivadas) e
`state_distribution` (regra de rateio por estado, §3.3).

## 2. Fontes

| métrica | fonte | publicador | licença | nações do jogo | ano |
|---|---|---|---|---|---|
| `agricultural_land_ha` | FAOSTAT "Land Use", item `00006610`, elemento Area `005110` (hectares) | FAO | CC BY 4.0 | **178 / 179** | 2023 em todas |
| `land_gini` | LINEQ v1, variável `lo_gini_all` | FAO (Cabrera Cevallos, de la O Campos & O'Neill, 2026), Zenodo DOI 10.5281/zenodo.18630849 | **CC BY-NC-SA 4.0** | **155 / 179** | 1960–2023 (mediana 2015) |
| conferência (não usado) | World Bank `AG.LND.AGRI.K2` (km²) | World Bank WDI (espelho FAOSTAT) | CC BY 4.0 | 175 comparadas | 2024 |

- **FAOSTAT em hectares, sem conversão.** A série `00006610` já é publicada em ha
  (definição FAO: terra cultivada + pastagens e prados permanentes), então não houve
  `km² × 100` nem `% × área`. Foi lida via a republicação do Our World in Data
  (`https://ourworldindata.org/grapher/land-use.csv`, `lastUpdated 2026-02-25`), que
  mantém a variável FAOSTAT original (`00006610 || Area | 005110 || hectares`); a API
  FAOSTAT (`fenixservices.fao.org`) respondeu **521/403** no momento da coleta e não foi
  usada como caminho de download.
- **Gini fundiário é estimativa modelada, não medição.** O LINEQ reconstrói a
  distribuição de área por faixa de tamanho a partir das tabulações do censo agropecuário
  (WCA) e calcula o Gini por interpolação Pareto generalizada. A própria linha do LINEQ se
  descreve como "Modeled estimates based on FAO WCA Main Results …"; `land_gini_census`
  guarda esse texto. É Gini de **área operada entre estabelecimentos agrícolas** — não é
  Gini de propriedade registral, nem de valor da terra, nem inclui população sem terra.
- **World Bank `AG.LND.AGRI.K2` foi conferido e recusado como série principal.** Ele é
  publicado em km² (`× 100 = ha`) e o espelho do WB estava defasado frente à revisão
  2025 da FAOSTAT: das **175** nações comparáveis, **130** batem dentro de 2% e **45**
  divergem, em alguns casos muito (ARG 80,8 M ha no WB contra 116,1 M ha na FAOSTAT;
  CHN 391,8 M contra 520,4 M — a diferença é da ordem da área de pastagem, o que sugere
  espelho incompleto, não revisão de pasto). Os seis piores
  desvios ficam em `cross_check.worst_offsets_pct`. Nada do WB entra nos valores.

## 3. Regras

### 3.1 Sem dado = `null`

`agricultural_land_ha = null`, `year = null`, `source = null`, `basis = "none"`; e, para o
Gini, `land_gini = null`, `land_gini_year = null`, `land_gini_source = null`,
`land_gini_census = null`. O validador **falha** se houver número sem fonte, ano sem dado
ou `basis` incoerente.

### 3.2 Fallback declarado (não gravado como se fosse observação)

`regional_reference` traz a **mediana dos `land_gini` observados deste próprio arquivo** por
região do jogo, com `n`, `min` e `max` — é derivado, não fonte externa. Regra de runtime
declarada no JSON: usar `land_gini` da nação; se `null`, a mediana da região (se `n ≥ 3`);
senão a mediana global (`0,6706`, n=155). As 24 nações sem Gini são: AFG, AGO, ARE, BDI,
BIH, CUB, DJI, ERI, GMB, GNQ, GUY, PNG, PRK, SAH, SDN, SLB, SOL, SOM, SSD, TKM, TWN, UKR,
UZB, XKX.

| região | mediana | n | faixa |
|---|---|---|---|
| africa | 0,538 | 40 | 0,389–0,918 |
| asia | 0,592 | 27 | 0,431–0,904 |
| europe | 0,696 | 38 | 0,409–0,994 |
| middle_east | 0,751 | 13 | 0,529–0,918 |
| north_america | 0,768 | 22 | 0,456–0,947 |
| oceania | 0,815 | 4 | 0,787–0,915 |
| south_america | 0,850 | 11 | 0,748–0,976 |

### 3.3 Distribuição por estado: em runtime, não em arquivo

`land_property_states.json` **não foi criado** porque não há fonte por admin-1 integrada:
FAOSTAT e LINEQ publicam só por nação, e as bases subnacionais existentes cobrem um país
cada (USDA NASS `Farms and Land in Farms` para os EUA; IBGE/SIDRA Censo Agropecuário 2017
para o Brasil), cada uma com extração própria e mapeamento de nome para `state_id`. Em vez
de um arquivo com um país só — que passaria a ideia de cobertura que não existe — o rateio
fica declarado em `state_distribution.rule`:

```text
ha_estado = ha_nacao * w_estado / soma(w)
w_estado  = area_km2(estado) * aptidao_agricola(estado)
aptidao   = média dos territórios TRT que interceptam o estado
            (data/definitions/natural/agriculture.json, 0..1)
se soma(w) = 0  ->  w_estado = area_km2(estado)
clamp: ha_estado <= 100 * area_km2(estado); o excedente volta
       para os estados não clampados, proporcional a w
```

A soma dos estados é sempre o total nacional (conservação). Estados vêm de
`data/definitions/population/state_regions.json`.

## 4. Cobertura

- **Área agrícola**: **178 / 179** nações, todas em **2023**. Sem dado: **XKX** (Kosovo),
  que não aparece na FAOSTAT nem no LINEQ.
- **Gini fundiário**: **155 / 179** nações. Faixa **0,389** (CAF, censo de 1974) a
  **0,9938** (RUS, 2016); mediana **0,6706**.
- **Idade do Gini** (declarada, não corrigida): 12 nações com censo até 1979, 16 em
  1980–1999, 19 em 2000–2009, **108 em 2010 ou depois**. Ano mínimo 1960 (ATG, BRN, MYS,
  ZWE). Nada foi extrapolado para o presente.

Amostra (`nations`):

| país | área agrícola (ha, 2023) | Gini (ano) | censo de origem |
|---|---|---|---|
| BRA | 236.758.800 | 0,8643 (2017) | FAO WCA — Brazil, censo 2017 |
| USA | 421.540.380 | 0,8649 (2022) | FAO WCA — United States, censo 2017 |
| IND | 178.571.000 | 0,4455 (2019) | Socio-economic Survey, NSSO |
| CHN | 520.413.000 | 0,5825 (2016) | Third National Agricultural Census |
| NGA | 69.398.210 | 0,5340 (2019) | Living Standards Survey, NBS |
| RUS | 215.701.000 | 0,9938 (2016) | FAO WCA — Rússia, censo 2016 |
| NER | 46.598.500 | 0,4040 (2022) | Harmonized Survey on Household Living Conditions |
| TWN | 779.000 | — | sem censo no LINEQ |
| XKX | — | — | sem área e sem censo |

## 5. O que não foi obtido (e por quê)

1. **Subnacional** — nenhum hectare por estado/província. Ver §3.3.
2. **Kosovo (`XKX`)** — não está na FAOSTAT nem no LINEQ; o WB devolve a série toda nula
   para `XKX`. Fica `null`, sem estimativa.
3. **Gini de propriedade registral** — não existe base global comparável; cadastros
   nacionais e o Land Matrix não produzem Gini. O que há (LINEQ) mede área **operada**.
4. **Censos antigos** — 28 nações com Gini anterior a 2000, mínimo 1960. Substituir exige
   censo agropecuário mais recente por país; não foi feito.
5. **API FAOSTAT direta** — `fenixservices.fao.org` respondeu 521/403 durante a coleta;
   o caminho usado foi a republicação OWID do mesmo item FAOSTAT (mesma variável, ha).
6. **Licença do LINEQ** — **CC BY-NC-SA 4.0**, uso **não comercial** com compartilha-igual.
   O código do projeto é MIT: **antes de distribuir build comercial, este bloco precisa ser
   autorizado pela FAO ou substituído** por um Gini derivado da base de censos FAOSTAT
   (CC BY 4.0), com o método de interpolação declarado. O `note` do JSON repete o aviso.

## 6. Como validar / regenerar

```powershell
$py = "C:\Users\artur\AppData\Local\Python\bin\python.exe"
& $py tools/validate_land_property.py    # exit 0 = OK
```

O validador checa: JSON parseável, UTF-8 sem BOM, LF, formato canônico (indent 2, chaves
ordenadas — igual aos vizinhos de `data/definitions/economy/`), ids iguais aos de
`nations.json`, `ha > 0`, `0 < land_gini < 1`, coerência `null`/`basis`/ano/fonte, toda
chave de fonte existente em `sources` (e todo `sources` com title/publisher/year/license/
url), `coverage` recontada, medianas regionais recalculadas e `state_distribution`
coerente (e, se `land_property_states.json` existir, hectares por estado contra
`state_regions.json`).

Regenerar sem construtor versionado (três passos, todos verificáveis):

```powershell
# 1) área: CSV do grapher (variável FAOSTAT 00006610 || Area | 005110 || hectares)
curl.exe -sL "https://ourworldindata.org/grapher/land-use.csv" -o land-use.csv
# 2) gini: planilha do LINEQ (aba `data`, coluna `lo_gini_all`, `source_year`)
curl.exe -sL -A "Mozilla/5.0" "https://zenodo.org/records/18630849/files/LINEQ_V1_FINAL.xlsx?download=1" -o lineq.xlsx
# 3) montar nations (último ano por país) e rodar o validador
& $py tools/validate_land_property.py
```

O construtor usado nesta coleta **não** foi versionado (fora do escopo da entrega da B6);
os passos acima reproduzem os mesmos números, e o Gini resultante foi conferido contra o
World Bank para as 175 nações onde as duas séries existem (`cross_check`).
