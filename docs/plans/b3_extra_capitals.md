# B3 opção 3 — capitais extras para estados grandes sem cidade

Fase B3 (`docs/economy/B3_region.md`). A B3 fechou a região da cidade como
**estados admin-1 inteiros** e deixou uma ressalva de dado aberta em D-005
(`docs/team/QUADRO.md`): **17 cidades com região menor que o urbano** (piso 0) e,
para os estados grandes que não têm cidade no elenco, a **"opção 3 futura"**.
Este trabalho executa a opção 3. Alcance exato: os **15 estados admin-1 acima
de 20 milhões de habitantes** que o B3 anexou a um vizinho por
`attach_rule=neighbor_largest_population` (isto é, sem cidade própria) ganham a
**capital administrativa real** como cidade do jogo. A capital passa a ser dona
**apenas do seu estado**; nenhum BFS é refeito e nenhum outro `attach_rule`
muda. Saída: `data/definitions/scenario/extra_capitals_b3.json` (15 cidades,
bloco `i18n` em 3 locales, `state_patch`, `neighbor_patch`,
`population_rateio_after`) e o patch correspondente em
`data/definitions/population/state_regions.json` (**4.226 estados, 834 → 849
cidades**, soma **8.203.501.062** preservada).

## Dado

15 linhas, ordem = população do estado decrescente (a mesma de
`B3_region.md` §6). Tabela conferida contra `extra_capitals_b3.json`; nenhum
número foi arredondado ou estimado aqui.

| estado | país | pop. 2026 | capital | id | lat | lon | cidade própria (GeoNames) | aglomeração 2026 | pop. urbana usada | fonte da população urbana | x | y | população rateada |
| --- | --- | ---: | --- | --- | ---: | ---: | ---: | ---: | ---: | --- | ---: | ---: | ---: |
| Henan | CHN | 98 807 834 | Zhengzhou | `CHN-ZHE` | 34.7578 | 113.6486 | 6650532 | 10500000 | 10 500 000 | citypopulation.de 2026 (linha 46 «Zhèngzhōu», Adm-Urb) | 4078.5 | 767.3 | 38950493 |
| Hebei | CHN | 73 973 601 | Shijiazhuang | `CHN-SHI` | 38.0414 | 114.4786 | 5758403 | 6600000 | 6 600 000 | citypopulation.de 2026 (linha 86 «Shíjiāzhuāng», Adm-Urb) | 4090.0 | 721.6 | 24483167 |
| Hunan | CHN | 65 364 287 | Changsha | `CHN-CHA` | 28.1987 | 112.9709 | 3093980 | 11600000 | 11 600 000 | citypopulation.de 2026 (linha 42 «Chángshā», Adm-Urb) | 4069.0 | 858.4 | 43031020 |
| Guangxi | CHN | 49 792 110 | Nanning | `CHN-NANN` | 22.8167 | 108.3167 | 5977185 | 5600000 | 5 600 000 | citypopulation.de 2026 (linha 100 «Nánníng», Adm-Urb) | 4004.4 | 933.1 | 20773596 |
| Odisha | IND | 48 251 532 | Bhubaneswar | `IND-BHU` | 20.2724 | 85.8338 | 885363 | 2175000 | 2 175 000 | citypopulation.de 2026 (linha 282 «Bhubaneswar», Congl) | 3692.1 | 968.4 | 15723832 |
| Jiangxi | CHN | 44 779 744 | Nanchang | `CHN-NANC` | 28.6840 | 115.8531 | 2357839 | 5250000 | 5 250 000 | citypopulation.de 2026 (linha 111 «Nánchāng», Adm-Urb) | 4109.1 | 851.6 | 19475246 |
| Jharkhand | IND | 42 050 057 | Ranchi | `IND-RAN` | 23.3432 | 85.3094 | 1120374 | 1670000 | 1 670 000 | citypopulation.de 2026 (linha 376 «Rānchi», Aggl) | 3684.9 | 925.8 | 12073011 |
| Guizhou | CHN | 38 625 398 | Guiyang | `CHN-GUI` | 26.5833 | 106.7167 | 4506134 | 5350000 | 5 350 000 | citypopulation.de 2026 (linha 106 «Guìyáng», Adm-Urb) | 3982.2 | 880.8 | 19846203 |
| Kerala | IND | 37 552 378 | Thiruvananthapuram | `IND-THI` | 8.4855 | 76.9492 | 788271 | — | 788 271 | GeoNames 2026 (sem linha na tabela) | 3568.7 | 1132.1 | 5698686 |
| Assam | IND | 37 535 208 | Dispur | `IND-GUW` | 26.1844 | 91.7458 | 962334 | 1260000 | 1 260 000 | citypopulation.de 2026 (linha 514 «Guwāhāti», Aggl) | 3774.2 | 886.3 | 9108979 |
| Shanxi | CHN | 34 668 260 | Taiyuan | `CHN-TAI` | 37.8694 | 112.5603 | 5305061 | 6300000 | 6 300 000 | citypopulation.de 2026 (linha 91 «Tàiyuán», Adm-Urb) | 4063.3 | 724.0 | 23370296 |
| Haryana | IND | 33 801 450 | Chandigarh | `IND-CHA` | 30.7363 | 76.7884 | 970602 | 2850000 | 2 850 000 | citypopulation.de 2026 (linha 210 «Chandīgarh», Congl) | 3566.5 | 823.1 | 20603642 |
| Chhattisgarh | IND | 31 993 104 | Raipur | `IND-RAI` | 21.2333 | 81.6333 | 1027264 | — | 1 027 264 | GeoNames 2026 (sem linha na tabela) | 3633.8 | 955.1 | 7426449 |
| Inner Mongol | CHN | 23 897 150 | Hohhot | `CHN-HOH` | 40.8106 | 111.6522 | 3446100 | 2900000 | 2 900 000 | citypopulation.de 2026 (linha 205 «Hūhéhàotè», Adm-Urb) | 4050.7 | 683.2 | 10757755 |
| Jilin | CHN | 23 873 808 | Changchun | `CHN-CHAN` | 43.8800 | 125.3228 | 5691024 | 5300000 | 5 300 000 | citypopulation.de 2026 (linha 109 «Chángchūn», Adm-Urb) | 4240.6 | 640.6 | 19660725 |

Colunas:

- `estado` / `país` / `pop. 2026` — `state_regions.json` (`name`, `nation`,
  `population_2026`). A população vem do raster **WorldPop 2026 apportionado
  por província** no pipeline da B3 (`docs/plans/b3_state_regions.md`); este
  trabalho não baixa nem remede o raster.
- `capital` — capital administrativa do estado (campo
  `provenance.capital_of_state`). Em Assam é **Dispur**, a sede de governo
  dentro da aglomeração de Guwahati (ver "Decisões e casos de borda").
- `id` — id da cidade nova, seguindo `make_city_id`
  (`tools/import_world_sources.py`); ver alínea (a) abaixo.
- `lat` / `lon` — coordenadas da cidade no **GeoNames `cities15000`** (dump
  2026); o `geonameid` de cada uma está em `provenance.geonameid`.
- `cidade própria (GeoNames)` — `sim_population`: a cidade própria da mesma
  fonte, em habitantes.
- `aglomeração 2026` — `agglomeration_population`, de
  `tools/agglomerations_2026.csv` (**Thomas Brinkhoff, citypopulation.de**,
  ref. 2026-01-01), com `row_rank`, `row_name` e `status` (`Adm-Urb`, `Aggl` ou
  `Congl`) copiados para `provenance.agglomeration`. `—` = sem linha na tabela.
- `pop. urbana usada` — `urban_population`, o peso que a simulação usa:
  aglomeração quando há linha (limitada à população da nação pelo mesmo
  `assign_urban_population` de `tools/build_world_cities.py`); senão cidade
  própria (`urban_population_source=sim_population`, a regra das 454 cidades
  atuais sem aglomeração). Nas 15, nenhum corte por nação se aplicou — as 13
  com linha ficam com `urban_population_source=agglomeration`.
- `fonte da população urbana` — qual das duas regras valeu, por cidade.
- `x` / `y` — `lonlat_to_xy` (`lon`/`lat` do GeoNames) no mapa 5000x2500.
- `população rateada` — `population` **derivada, não medida**: rateio da
  população da nação (`nations.json`) entre as cidades da nação por peso
  `urban_population`, soma exata por nação, resto pelas maiores frações
  (desempate por id) — a mesma `distribute_national_population`. **Não é a
  população da cidade** nem a do estado.

Constantes nas 15: `capital` = `false` (não são capitais nacionais), `landmass` =
`"asia"` (como as vizinhas) e `production` = `0.0` (é a soma de `buildings.json`
e a cidade nova nasce sem prédio). Caso que merece leitura: **Hohhot** tem
aglomeração (2.900.000) **menor** que a cidade própria (3.446.100); a regra do
projeto usa a aglomeração quando ela existe, não o máximo das duas.

## Fontes

- **GeoNames — `cities15000` (dump 2026)**:
  `https://download.geonames.org/export/dump/cities15000.zip`. Cidade própria,
  coordenadas e `geonameid`. Licença **CC BY 4.0**. Acesso **2026-11-18**
  (campo `retrieved` em `provenance.city_proper`).
- **Thomas Brinkhoff / citypopulation.de — aglomerações**:
  `https://www.citypopulation.de/en/world/agglomerations/`. População
  aglomerada, ref. **2026-01-01**. Uso **com atribuição ao link** (licença
  declarada em `extra_capitals_b3.json > license`). A linha usada está
  versionada em `tools/agglomerations_2026.csv` (`row_rank`, `row_name`,
  `status`).
- **WorldPop 2026 (via B3)** — população de estado. Não é baixada aqui: vem
  de `state_regions.json`, cujo pipeline está em
  `docs/plans/b3_state_regions.md`; este arquivo não declara URL própria do
  raster.

Arquivos do repositório usados pelo build:

- `data/definitions/population/state_regions.json` — estados (`name`, `nation`,
  `population_2026`, `attach_rule`, `city_id`) e `cities[]`.
- `data/definitions/scenario/nations.json` — população nacional (rateio).
- `data/definitions/scenario/cities.json` — elenco atual (834) para o rateio
  pós-inserção.
- `tools/agglomerations_2026.csv` — tabela de aglomerações versionada.
- `tools/build_extra_capitals_b3.py` — builder (método e regras na docstring).
- `data/definitions/scenario/extra_capitals_b3.json` — saída (fonte dos
  números deste documento).

## Decisões e casos de borda

**(a) Ids.** Seguem `make_city_id` (`tools/import_world_sources.py`): ISO3 + as
3 primeiras letras do nome normalizado; se ocupado, 4 e depois 5 letras; por
último sufixo numérico. A ordem de criação é a população do estado
decrescente, e é ela que decide a colisão: `CHN-NAN` é Nanjing (já
existente), então **Nanning** (Guangxi, 49.792.110) fica `CHN-NANN`; logo
**Nanchang** (Jiangxi, 44.779.744) fica `CHN-NANC`. **Changsha** (Hunan,
65.364.287) entra antes e leva `CHN-CHA`; **Changchun** (Jilin, 23.873.808),
depois, fica `CHN-CHAN`.

**(b) Thiruvananthapuram não tem linha na tabela de aglomerações ≥1 M.** A
maior de Kerala na tabela é **Kozhikode, 1.070.000**, então a capital não
casa. Vale a **regra das 454 cidades sem aglomeração**: `urban_population` =
cidade própria **788.271** (GeoNames). Referência disponível e **não usada**:
Census of India 2011, UA Thiruvananthapuram = **1.679.754** (ano e definição
diferentes da tabela de 2026). Fica registrada em `provenance.note`.

**(c) Raipur.** A tabela 2026 traz a conurbação como **"Bhilai (incl.
Raipur)", 3.475.000** (rank 169, `Congl`), cujo polo nominal é Bhilai; o
casamento por nome não casa Raipur. Pela regra, fica a cidade própria
**1.027.264**. Trocar para a conurbação inteira é mudar 3 campos desta cidade
(registrado em `provenance.note`).

**(d) Guwahati / Dispur.** A linha da tabela é **"Guwāhāti"** (rank 514,
`Aggl`, 1.260.000) — a mesma cidade, transliteração diferente — e o casamento
foi feito à mão (declarado em `method`). A cidade do jogo é **Guwahati**
(`IND-GUW`, id e nome da aglomeração); **Dispur** é a sede de governo dentro
da aglomeração e aparece como `capital_of_state`, não como cidade separada.

**(e) Chandigarh.** É capital de Haryana e território próprio (union
territory). A cidade do jogo (`IND-CHA`) fica **com o estado de Haryana**
(`IND-2430`, 33.801.450), que é o estado que a opção 3 precisava atender; a
aglomeração usada é a linha 210 da tabela ("Chandīgarh", `Congl`,
2.850.000).

**(f) `capital` = `false` nas 15.** No elenco, `capital` marca capital nacional
(179 cidades, uma por nação); nenhuma das 15 é capital de país — são capitais
administrativas de estado.

**(g) `production` = `0.0`.** O campo é a soma de `buildings.json` e a cidade
nova nasce sem prédio; zero é ausência declarada, não medição.

**(h) `population` é derivado e foi recomputado.** O rateio rodou com o elenco
novo (834 + 15): **53 cidades** mudam de valor — 28 de nação CHN (26 com
prefixo `CHN`, mais `HKG-HON` e `MAC-MAC`) e 25 de nação IND. O mapa completo
old→new está em `population_rateio_after.changed`; nenhuma nação muda de
total (a soma por nação é exata).

## O que mudou em `state_regions.json`

**15 estados reatribuídos**: `attach_rule` `neighbor_largest_population` →
`primary_city` e `city_id` → a capital nova. A população do estado não muda;
o estado só troca de dono.

| estado (admin-1) | id do estado | cidade nova | anexado antes a |
| --- | --- | --- | --- |
| Henan | `CHN-1812` | `CHN-ZHE` | CHN-XIAN |
| Hebei | `CHN-1811` | `CHN-SHI` | CHN-BEI |
| Hunan | `CHN-1808` | `CHN-CHA` | CHN-GUA |
| Guangxi | `CHN-1152` | `CHN-NANN` | CHN-GUA |
| Odisha | `IND-2444` | `IND-BHU` | IND-KOL |
| Jiangxi | `CHN-1817` | `CHN-NANC` | CHN-GUA |
| Jharkhand | `IND-3256` | `IND-RAN` | IND-KOL |
| Guizhou | `CHN-1153` | `CHN-GUI` | CHN-CHE |
| Kerala | `IND-2442` | `IND-THI` | IND-BAN |
| Assam | `IND-2477` | `IND-GUW` | IND-KOL |
| Shanxi | `CHN-1805` | `CHN-TAI` | CHN-XIAN |
| Haryana | `IND-2430` | `IND-CHA` | IND-DEL |
| Chhattisgarh | `IND-3260` | `IND-RAI` | IND-MUM |
| Inner Mongol | `CHN-1838` | `CHN-HOH` | CHN-XIAN |
| Jilin | `CHN-1828` | `CHN-CHAN` | CHN-SHE |

**9 vizinhos encolheram** (`neighbor_patch`). "antes" = `population_region_after`
+ `population_removed` (aritmética sobre os dois campos do próprio patch):

| vizinho | estados que saíram | população removida | `population_region` antes → depois |
| --- | ---: | ---: | ---: |
| CHN-XIAN | 3 | 157.373.244 | 203.898.126 → 46.524.882 |
| CHN-BEI | 1 | 73.973.601 | 96.057.294 → 22.083.693 |
| CHN-GUA | 3 | 159.936.141 | 238.067.643 → 78.131.502 |
| CHN-CHE | 1 | 38.625.398 | 127.492.265 → 88.866.867 |
| CHN-SHE | 1 | 23.873.808 | 52.223.497 → 28.349.689 |
| IND-KOL | 3 | 127.836.797 | 241.558.646 → 113.721.849 |
| IND-BAN | 1 | 37.552.378 | 109.150.261 → 71.597.883 |
| IND-DEL | 1 | 33.801.450 | 57.077.126 → 23.275.676 |
| IND-MUM | 1 | 31.993.104 | 50.707.266 → 18.714.162 |

**Totais** (`checks` no JSON): **4.226** estados (inalterado), **849** cidades
(834 + 15), soma dos estados = soma das regiões = **8.203.501.062** (preservada,
e a soma fecha por nação).

O arquivo ganha uma chave de topo **`patch`** (`id: b3-extra-capitals-1`,
`source`, `states_reassigned`, `cities_added` e uma nota) que registra o que foi
feito e avisa que as 15 cidades ainda não estão em `cities.json` — quem abrir o
JSON vê a dependência sem precisar deste documento.

**Consequência honesta — o piso 0 de `region_population` (D-005) piora um
pouco.** Passam a ser **19** as cidades com região menor que o urbano (eram 17);
as novas são **IND-DEL** e **IND-MUM**, porque os estados que lhes davam
interior foram justamente os reatribuídos: a região própria que resta — NCT de
Delhi **23.275.676** e o resto de Maharashtra **18.714.162** — é menor que a
aglomeração das duas cidades. Com o piso, `region_population` das duas vira
**0**, e o número de cidades com região 0 sobe de 18 para **20** (as 17 com
região < urbano mais `SGP-SIN`, cuja região é exatamente 0, dão as 18 de
`tools/b3_region_test.gd`). Por isso
urbano + rural **não fecha** o total: `checks.urban_plus_rural_after` =
**8.277.303.923** (é a sobreposição contada duas vezes). A soma que fecha por
nação é **`population_region`** — é a invariante declarada, não
`urban + region_population`.

## O que o código precisa fazer (só descrição, nada implementado)

1. **Carregar e mesclar.** Ler `data/definitions/scenario/extra_capitals_b3.json`,
   acrescentar `cities[]` (15 registros) a `data/definitions/scenario/cities.json`
   (**834 → 849**) e o bloco `i18n` às três locales de
   `data/definitions/i18n/i18n.json` (`pt_BR`, `en`, `es`; 15 chaves
   `city.name.<id>` por locale).
2. **Reaplicar o rateio nacional.** `distribute_national_population` precisa rodar
   sobre o elenco de 849 para CHN e IND — usando o mapa old→new fornecido em
   `population_rateio_after.changed` ou regerando pelo build. Sem isso, as 15
   cidades ficam com `population` derivada de um elenco que não existe no
   `cities.json`.
3. **`urban_land.json` e `housing_city.json`.** `data/definitions/economy/urban_land.json`
   e `data/definitions/economy/housing_city.json` têm hoje uma entrada por cidade
   (834 chaves em `cities`, em cada um) e `tools/urban_load_test.gd` exige
   exatamente **834** (`land_data.size() != 834 or housing_data.size() != 834`).
   As 15 precisam de entrada ou de **ausência declarada** — e o teste, de
   atualização coerente com a decisão.
4. **`buildings.json`.** As 15 não têm prédio (`production` = `0.0`); decidir
   se nascem assim ou com um seed mínimo, e registrar a decisão.
5. **Ordem de entrada dos dois arquivos.** `core/economy/region_data.gd` itera as
   cidades de `cities.json` e resolve `state_regions["cities"][<id>]`; cidade sem
   entrada fica **sem** o bloco `region` (ausência explícita, região inerte).
   Enquanto a mesclagem do item 1 não acontecer, as 15 entradas novas de
   `state_regions.json` ficam inertes — **mas os vizinhos já perderam a
   população** no arquivo patchado. Os dois arquivos têm de entrar juntos.
6. **Testes que fixam o elenco.** `tools/b3_region_test.gd` fixa a soma
   `8203501062` e `EXPECTED_ZERO_REGION := 18` (com este patch são **20** cidades
   com região 0: entram IND-DEL e IND-MUM); goldens de demografia e de escala
   podem fixar 834 cidades. Todos precisam ser reconferidos.
7. **Save.** O save v18 **não muda de versão** (o contrato é por cidade e não
   mudou), mas o **número de cidades muda** — save, status e goldens de mundo
   precisam ser reconferidos contra o elenco de 849.

## Reprodução e validação

```powershell
$py = "G:\tools\cpp-tools\Scripts\python.exe"
& $py tools/build_extra_capitals_b3.py --check   # só confere, não grava
& $py tools/build_extra_capitals_b3.py --write   # grava extra_capitals_b3.json + state_regions.json
```

- `--check` conferido neste worktree: `exit=0`, `ok: estados=4226 cidades=849
  soma=8203501062 (+15 cidades, 15 estados)`. Ele também valida que o rateio
  reproduz `cities.json` antes do patch, que os 15 estados são exatamente os
  `>20M` sem `primary_city`, que os ids são livres e que as somas fecham.
- `--write` é **idempotente por construção**: o build chama `unapply` e
  desfaz o patch antes de reaplicar, então duas execuções produzem o mesmo
  byte/sha256 nos dois arquivos. Verificação:
  `Get-FileHash data/definitions/scenario/extra_capitals_b3.json -Algorithm SHA256`
  antes e depois da segunda execução.

Validador dedicado (somente leitura, stdlib apenas):

```powershell
& $py tools/validate_extra_capitals_b3.py
```

Conferido neste worktree: `exit=0`, `RESULTADO: OK - 15/15 estados atendidos,
849 cidades, soma 8.203.501.062, 0 problemas`. Ele confere cobertura (`15/15`
estados atendidos, `estados >20M sem cidade propria: 0`), as somas por nação
(`179/179`), as **19** cidades com região abaixo do urbano (esperado 19) e o
rateio (`834 reproduzidas (0 divergentes), 15 novas, 53 alteradas`); avisa
também que o i18n ainda **não** foi aplicado em `i18n.json` — coerente com o
item 1 da seção anterior.

## Pendências e decisões do dono

- **Kerala (Thiruvananthapuram).** Ficou a cidade própria **788.271** (GeoNames),
  pela regra das cidades sem aglomeração. A referência do Census of India 2011
  para a UA é **1.679.754** — ano e definição diferentes. Usar o censo é trocar
  a urban da cidade e refazer o rateio; hoje está **não usada** e registrada.
- **Chhattisgarh (Raipur).** Ficou a cidade própria **1.027.264**; a conurbação
  **Bhilai–Raipur, 3.475.000** (rank 169, `Congl`) não casou por nome. Adotar a
  conurbação é trocar 3 campos da cidade e refazer o rateio.
- **Seed de prédios das 15.** Hoje `production` = `0.0` (sem prédio em
  `buildings.json`). Decidir se a cidade nasce sem indústria/emprego próprio ou
  com um seed mínimo, e qual.
- **`urban_land.json` / `housing_city.json` das 15.** Sem entrada hoje; a
  cobertura (834) e `tools/urban_load_test.gd` precisam acompanhar a decisão.
