# Fase 1a — dados do mundo (Europa + G20)

> Status: **só dados**. Nada em `data/definitions/scenario/` mudou, o jogo não lê
> nada de `data/definitions/world/` ainda e nenhum código de jogo foi alterado.
> Contrato a montante: `docs/plans/world_nations_plan.md` (§4 regra de cidades,
> §5 schema de nações, §7 fase F1).

## 1. O que foi entregue

| arquivo | papel |
|---|---|
| `data/definitions/world/nations_source.csv` | 66 nações: ISO3/ISO2, nome pt/en/es, capital, moeda ISO 4217, governo, região, população 2025, PIB PPP 2025, cor, `data_quality`, `source` |
| `data/definitions/world/cities_source.csv` | 252 cidades: lat/lon, população + ano, base da população (aglomeração *ou* cidade), capital, porto, papel, landmass, `source`, marcação `legacy` |
| `data/definitions/world/ports_source.csv` | 114 portos: lat/lon (ponto de costa), landmass, `source` |
| `tools/build_world_cities.py` | gerador (só stdlib): aplica cota, espaçamento e projeção; escreve `out/` |
| `data/definitions/world/out/` | `nations.csv`, `cities.csv`, `ports.csv`, `buildings.csv`, `i18n.csv`, `_meta.json` |
| `assets/flags/*.png` | 9 bandeiras que faltavam (ver `assets/flags/CREDITS.txt`) |

Reproduzir/validar:

```powershell
python tools/build_world_cities.py           # gera data/definitions/world/out/
python tools/build_world_cities.py --check   # valida sem escrever (exit != 0 se houver aviso)
```

## 2. Contagens

- **nações: 66** — 50 na Europa (inclui Rússia, Turquia, Chipre, Cáucaso, microestados
  e Kosovo), 13 membros do G20 fora da Europa e as 3 nações sul-americanas que já
  existiam (VEN, SUR, GUY);
- **cidades: 252** (das quais **40 legadas**, com id, coordenadas e população intactos);
- **portos: 114** (**18 legados** intactos + 96 derivados);
- **construções (fazenda inicial): 252**;
- **chaves i18n: 432** (`nation.name.*` 66, `city.name.*` 252, `port.name.*` 114);
- soma da cota calculada: 244; a diferença (+12/−4) está explicada em §4.

### Cidades por nação (cidades/cota)

```
ALB 2/2   AND 1/2   ARG 3/3   ARM 2/2   AUS 4/4   AUT 2/2   AZE 2/2   BEL 3/2
BGR 2/2   BIH 2/2   BLR 2/2   BRA 12/12 CAN 4/4   CHE 2/2   CHN 12/12 CYP 2/2
CZE 2/2   DEU 9/9   DNK 2/2   ESP 4/4   EST 2/2   FIN 2/2   FRA 10/6  GBR 8/6
GEO 2/2   GRC 2/2   GUY 2/2   HRV 2/2   HUN 2/2   IDN 12/12 IND 12/12 IRL 2/2
ISL 2/2   ITA 6/6   JPN 10/10 KOR 6/6   LIE 1/1   LTU 2/2   LUX 1/2   LVA 2/2
MCO 1/1   MDA 2/2   MEX 9/9   MKD 2/2   MLT 1/2   MNE 2/2   NED 4/3   NOR 2/2
POL 4/4   PRT 2/2   ROU 2/2   RUS 11/11 SAU 4/4   SMR 1/1   SRB 2/2   SUR 1/2
SVK 2/2   SVN 2/2   SWE 2/2   TUR 7/7   UKR 3/3   USA 12/12 VAT 1/1   VEN 5/1
XKX 2/2   ZAF 4/4
```

**Nações com 1 cidade (8):** AND, LIE, LUX, MCO, MLT, SMR, SUR, VAT.
Em AND/LUX/MLT/SUR a cota pediria 2, mas não existe uma segunda aglomeração a
≥ 25 km da capital — a nação fica com 1, como manda §4 ("se a lista acaba, a
nação fica com menos"). Em LIE/MCO/SMR/VAT a cota é 1 porque não há PIB PPP
publicado.

## 3. Fontes e anos-base

| campo | fonte | ano | coluna |
|---|---|---|---|
| população do país | World Bank WDI `SP.POP.TOTL` | 2025 | `population`, `population_year` |
| PIB PPP | World Bank WDI `NY.GDP.MKTP.PP.CD` (US$ PPP correntes) | 2025 | `gdp_ppp_bn`, `gdp_year` |
| população das cidades | **citypopulation.de — Major Agglomerations of the World** (aglomeração, referência 2026-01-01) | 2026 | `population`, `pop_src = agglomeration` |
| população das cidades < 1 M | **GeoNames cities15000** (cidade administrativa, dump 2026) | 2026 | `population`, `pop_src = city_proper` |
| coordenadas | GeoNames cities15000 | 2026 | `lat`, `lon` |
| portos derivados | snap na costa do próprio `map/map_data/atlas/world_sdf.png` (≤ 30 km) | — | `ports_source.csv` |
| capital, moeda, continente | GeoNames `countryInfo` (ISO 3166) | 2026 | `capital_*`, `currency` |

Os dois indicadores do World Bank foram lidos em 2026‑11‑11 (`lastupdated` do
próprio serviço), com valor de 2025 para ambos.

Nenhum número foi estimado. Onde a fonte não publica, o campo fica **vazio** e a
nação recebe `data_quality` diferente de `ok` (§5).

## 4. Regra de cidades aplicada

```
cidades = min(12, 1 + clamp(1 + floor(pop/25M) + floor(PIB_PPP/1,5e12), 1, 12))
```

- **Desvio documentado:** o §4 do plano escreve `1 (capital) + clamp(...,1,12)`;
  com isso as cinco maiores nações (IND, CHN, USA, IDN, BRA) dariam **13**
  cidades, contra o "**teto duro: 12 cidades por nação**" do §9 e a faixa 6–12 do
  §8. Vale o teto: o resultado é limitado a 12 e a capital entra na contagem.
  Os exemplos do §4 são internamente inconsistentes (um país de 68 M/4,2 T
  aparece como 5 cidades, mas a aritmética da própria fórmula dá 6).
- **Capital sempre presente**; entra nas 12 e não consome uma vaga extra.
- **Seleção:** capital + as demais aglomerações por população decrescente, com
  **espaçamento mínimo de 25 km** (evita bairros/distritos que o GeoNames lista
  como cidade — ex.: os distritos de Viena, Tallinn, Erevan, Belgrado).
- **Aglomeração não se conta duas vezes:** quando uma aglomeração da fonte já
  engloba outra ("incl. Shenzhen" em Guangzhou, "incl. Yokohama" em Tóquio,
  "incl. Bekasi/Depok" em Jacarta), a cidade citada é descartada.
- **Legado manda:** as 40 cidades atuais entram sempre, mesmo acima da cota
  (BEL 3/2, FRA 10/6, GBR 8/6, NED 4/3, VEN 5/1), e os 18 portos atuais são
  copiados sem alteração.

Projeção: `x = (lon+180)/360·5000`, `y = (90−lat)/180·2500`. Conferida contra as
40 cidades atuais: as coordenadas legadas voltam idênticas (0 divergências), e
Londres do GeoNames (−0,1257 / 51,5085) dá exatamente o `2498.2 / 534.6` do
cenário — a projeção é a mesma do mapa.

Fazenda inicial: `população × 0,0003 t/dia` para cidade nova. Nas 40 cidades
legadas o valor de `buildings.csv` do cenário atual foi **copiado** (as 5
capitais têm déficit/superávit intencional e a razão não é `0,0003`).

Ids: as 40 cidades legadas mantêm o id atual (`FRA-PAR`, `GBR-LDN`, …). Cidade
nova recebe `ISO3` + 3–5 letras do nome sem acento (`JPN-TOK`, `BRA-BRAS`,
`CHN-XIAN`), ampliando a sigla apenas em caso de colisão dentro da nação. O id do
porto é o da cidade quando o porto leva o nome da cidade; os 18 portos legados
mantêm id e nome (inclusive os que não coincidem com a cidade, como `FRA-CAY`
= Mana e `VEN-LAG` = La Guaira). Os nomes em `buildings.csv` são gravados em
UTF-8 (o arquivo do cenário atual está em mojibake; os números são os mesmos).

## 5. Lacunas de fonte e limitações declaradas

1. **VEN** — sem PIB PPP 2025 no World Bank: `data_quality = missing_gdp`,
   cota 1. Mantidas as 5 cidades legadas (contrato).
2. **VAT, LIE, MCO, SMR** — sem PIB PPP publicado e, no VAT, sem população no
   World Bank: `data_quality = missing`, cota 1, uma cidade (a capital).
3. **Base populacional mista** — 190 cidades com população de aglomeração
   (2026, incluindo as 40 legadas) e **62** com população de cidade
   administrativa (GeoNames), porque a aglomeração não chega a 1 M e não há série
   de aglomeração legível por máquina para esse recorte. A coluna `pop_src` diz
   qual é qual em cada linha.
4. **Populações legadas são mais antigas que a fonte 2026** (ex.: London
   11,36 M no cenário vs. 15,4 M na aglomeração 2026). Ficaram como estão por
   contrato de F1; normalizar é uma decisão de balanceamento da fase seguinte.
5. **Definição de aglomeração é a da fonte** — algumas entradas são
   administrativas e largas (ex.: Xiamen 15,8 M, Guangzhou 73,6 M com a Delta do
   Pérola). A coluna `note` guarda o texto da fonte (`incl. …`).
6. **`NED` não é ISO3** (o ISO3 dos Países Baixos é `NLD`). Mantido `NED` porque
   é o id do cenário atual; idem `XKX` (Kosovo, código de uso livre) e `GBR`
   agora com nome *United Kingdom* em vez de *England*.
7. **Europa é uma escolha de escopo:** Rússia, Turquia, Chipre e Cáucaso
   (ARM/AZE/GEO) entram como `region = europe`; a Rússia tem cidades asiáticas
   na mesma landmass `continent`.
8. **Cores políticas:** as 8 nações atuais mantêm a cor do mapa
   (`provinces.json → nation_colors`); as outras 58 recebem cor determinística
   (ângulo dourado, saturação/luminosidade do tema papel). **A unicidade entre
   nações vizinhas ainda não foi verificada** — é tarefa da fase de mapa.
9. **Portos derivados** são um snap na costa do SDF do próprio mapa; o nome do
   porto repete o da cidade (regra do jogo: porto de mesmo nome não duplica
   rótulo). Ficam a ≤ 30 km do centro da cidade.
10. **Landmass** (`continent`, `britain`, `japan`, `java`, `sumatra`, …) é um
    token de rota terrestre do jogo. Para a F1 ele é atribuído por nação, com
    exceções por ilha declaradas no gerador; nenhum consumidor do jogo o usa
    ainda fora da Europa.

## 6. Bandeiras

9 nações da F1 não tinham PNG: `ad, li, mc, me, mt, rs, sm, va, xk`. Foram
baixadas em `w40` (largura 40 px, proporção original), o mesmo formato das 167
existentes, de **flagcdn.com** (domínio público, a partir dos SVGs do Wikimedia
Commons). Detalhes e licença em `assets/flags/CREDITS.txt`. Resultado: **0
nações da F1 sem bandeira**.

## 7. Verificação desta entrega

```powershell
python tools/build_world_cities.py --check
python -c "import csv;print(len(list(csv.DictReader(open('data/definitions/world/out/cities.csv',encoding='utf-8')))))"
```

Checagens feitas (script de conferência, sem Godot):

- 40 cidades legadas: `id`, `nation`, `home_nation`, `landmass`, `capital`,
  `x`, `y` e `population` **idênticos** a `data/definitions/scenario/cities.csv`
  (0 divergências);
- 18 portos legados: `id`, `name`, `nation`, `landmass`, `x`, `y` idênticos
  (0 divergências);
- 40 fazendas legadas: `building_id` e `output_tons_per_day` idênticos
  (0 divergências);
- toda nação com 1–12 cidades e exatamente 1 capital; 66 capitais para 66 nações;
- 0 ids duplicados em cidades e portos; 0 cidade sem `name_pt`/`name_es`;
- `i18n.csv` com 432 chaves e o mesmo cabeçalho `key,pt_BR,en,es` de
  `data/definitions/i18n/i18n.csv`.

Não se rodou Godot nesta tarefa (outra sessão estava compilando/testando) e nenhum
arquivo de `data/definitions/scenario/`, `core/`, `ui/` ou `map/` foi alterado.
