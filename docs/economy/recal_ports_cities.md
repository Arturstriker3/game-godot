# Recalibração 3 — portos faltantes, landmass de porto e populações administrativas

Branch `recal/ports-cities` (base `balance/global-resources` @ `c773150`), worktree
`G:/projetos/opengs-wt-recal2`. Nenhum arquivo de `core/`, `gdextension/src/` ou `ui/`
foi alterado: a DLL desta worktree é o build debug do HEAD (`92B3E43A…`).

Fecha as três dívidas de dado abertas em `recal_areas_pop.md` §13 (portos sem porto,
portos com `landmass` trocada) e em `q016_city_pop_fix.md` §4.3 (população de
unidade administrativa), mais a pendência de ferramenta do §10.1 (i18n apagado).

| Frente | Antes | Depois |
|---|---|---|
| Portos no cenário | 145 | **157** (+14 novos, −2 do Cáspio) |
| Portos com `landmass` ≠ cidade que servem | 15 | **0** |
| Nações-ilha/costeiras sem porto | 14 | **0** |
| Cidades com população de unidade administrativa | 5 (AFG-MAZ já corrigida; +4) | **2 listadas, não corrigidas** (§3.2) |
| `i18n.json` regenerado pelo pipeline | perdia 137 chaves | **mescla, preserva** |

---

## 1. Frente 1 — 14 nações ilha/costeiras ganharam porto

Regra do `sim/trade.cpp`: a travessia entre massas de terra exige um porto da nação
**em cada ponta**; `sim/logistics.cpp::nearest_port` filtra por
`port.nation == nation && port.landmass == landmass` da cidade. Nação sozinha na sua
massa de terra e sem porto não compra nem vende fora dela. Era o caso de PHL, CUB,
NZL, MDG, JAM, LKA, BHS, FJI, TTO, BRB, SLB (uma landmass cada) e de BHR, QAT, BRN
(costa sem porto nenhum).

Um porto principal por nação, na cidade do roster mais adequada (`FORCED_PORTS`,
seção r10 de `tools/world_city_curation.py`). **Em todos os 14 casos a cidade
adequada já estava no roster** — nenhuma cidade nova entrou.

| Nação | Porto / cidade | lat, lon | Fonte do cais | `nav_mask` | Terra |
|---|---|---|---|---|---|
| PHL | `PHL-MAN` Manila | 14,5900 N, 120,9600 E | Port of Manila (South Harbor), Baía de Manila (porto 14,5967 N 120,9544 E) | comp principal, snap 1 cél. | 9,78 km |
| CUB | `CUB-HAV` Havana | 23,1450 N, 82,3700 W | Havana Harbor (23,1330 N 82,3418 W); ponto na boca do porto | comp principal, cél. de costa | 0,00 km |
| NZL | `NZL-AUC` Auckland | 36,8438 S, 174,7769 E | Port of Auckland, Waitematā Harbour | comp principal, cél. de costa | 0,00 km |
| MDG | `MDG-TOA` Toamasina | 18,1553 S, 49,4267 E | Port autonome de Toamasina (bacia portuária) | comp principal | 6,92 km |
| JAM | `JAM-KIN` Kingston | 17,9500 N, 76,8000 W | Kingston Harbour | comp principal, cél. de costa | 6,92 km |
| LKA | `LKA-COL` Colombo | 6,9528 N, 79,8447 E | Port of Colombo | comp principal, cél. de costa | 4,89 km |
| BHS | `BHS-NAS` Nassau | 25,0791 N, 77,3466 W | Nassau Harbour / Prince George Wharf | comp principal | 4,89 km |
| FJI | `FJI-SUV` Suva | 18,1350 S, 178,4250 E | Port of Suva (King's Wharf) | comp principal, cél. de costa | 0,00 km |
| TTO | `TTO-POR` Port of Spain | 10,6600 N, 61,5200 W | Port of Spain, Golfo de Paria | comp principal, cél. de costa | 4,89 km |
| BRB | `BRB-BRI` Bridgetown | 13,0969 N, 59,6131 W | Bridgetown, Deep Water Harbour | comp principal | 4,89 km |
| SLB | `SLB-HON` Honiara | 9,4319 S, 159,9556 E | Honiara, Point Cruz | comp principal, cél. de costa | 0,00 km |
| BHR | `BHR-MAN` Khalifa Bin Salman | 26,2000 N, 50,6200 E | KBSP (BHKBS), 26°12'N 050°37'E — APM Terminals, *KBSP Port Information*; UN/LOCODE UNECE | comp principal, cél. de costa | 0,00 km |
| QAT | `QAT-DOH` Hamad Port | 25,0150 N, 51,6020 E | Hamad Port, Umm Al-Houl (ao sul de Doha) | comp principal, cél. de costa | 4,89 km |
| BRN | `BRN-BAN` Muara | 5,0242 N, 115,0705 E | Muara Port, baía de Brunei | comp principal | 6,92 km |

**Como foi conferido** (não é "parece perto da costa"):

- **Água navegável**: cada cais foi amostrado em `map/map_data/nav_mask.png`; a célula
  (ou a mais próxima, ≤ 8 células) pertence ao **componente de água principal**
  (2.110.356 células), o mesmo critério do `nav_strait_test.gd`. Os 14 entraram na
  lista `PORTS` do teste com uma **semente de mar aberto** ao lado — PASS (§6).
- **Terra**: distância ao texel de terra mais próximo em `world_sdf.png` ≤ 10 km
  (`COAST_TOLERANCE_KM` do validador). Pior caso **PHL-MAN 9,78 km** (2 texels de
  4,89 km; a Baía de Manila é estreita no raster, mas passa).
- **`port_of`**: aponta para a cidade do roster da mesma nação; `validate_scenario_data`
  exige isso e passou com **0 erros/0 avisos**.
- **`landmass`**: igual à da cidade que o porto serve (senão o porto nunca é escolhido).

Ajuste declarado: em PHL, CUB e TTO a coordenada do **cais publicado** caiu em célula
de terra do raster de 16 km/célula; o ponto usado é a célula de água/costa imediatamente
ao lado (≤ 1 célula). O cais publicado e a fonte ficam no campo `source` da entrada de
`FORCED_PORTS` (a curadoria é a fonte; o `ports.csv` emitido tem só
`id,name,nation,x,y,landmass,port_of`, como todos os outros 143 portos).

Efeito medido: as 14 nações ficam em **1,000** de comida antes e depois — elas já se
sustentavam com a própria fazenda; o porto lhes dá **acesso ao mercado** (exportar o
excedente, importar o que faltar), não comida de graça.

---

## 2. Frente 2 — 15 portos com `landmass` trocada: causa e correção

Os 15 portos do §13 (`IDN` 9, `TUR` 3, `AZE` 2, `GEO` 1) tinham rótulo de massa de
terra que **nenhuma cidade do cenário usa**. `sim/logistics.cpp::nearest_port` filtra
por `nation + landmass`, então eles **nunca eram escolhidos**: existiam no mapa e não
no comércio.

**Causa medida — não é ilha, é rótulo legado:**

| Grupo | Rótulo no porto | Rótulo nas cidades | Diagnóstico |
|---|---|---|---|
| IDN (9) | `java` | `indonesia` (19 cidades) | `java` é o rótulo do Voronoi antigo (já citado no docstring da r5) e **não existe em cidade nenhuma**. O modelo trata o arquipélago como a massa única `indonesia`; só `IDN-MEDA` (Sumatra) escapou. |
| TUR (3) | `continent` | `asia` (14 cidades) | `continent` é o rótulo da Europa continental; a normalização (`tools/normalize_landmass.py`, `europe → continent`) passou pelas **cidades** e não pelos portos. |
| GEO (1) | `continent` | `asia` | mesma troca de rótulo. Batumi é **Mar Negro** → alcança o oceano pelo Bósforo (`nav_strait_test` já cobre o eixo). |
| AZE (2) | `continent` | `asia` | mesma troca de rótulo, **mas** Baku e Sumqayit estão no **Cáspio** (§2.1). |

**Correção**: 13 portos recebem a `landmass` da cidade que servem
(`PORT_LANDMASS_FIX`, `tools/world_city_curation.py`), aplicada por
`tools/build_world_cities.py` na emissão do cenário e por
`tools/normalize_landmass.py` na fonte (`ports_source.csv`, `scenario/ports.{csv,json}`)
— as duas passadas são idempotentes: depois de aplicar, a passada r10 reporta
**0 linha(s)**. (O `--check` do script inteiro ainda acusa as 4 linhas pré-existentes
de GRC-HER, cidade e porto em `europe` × canônico `continent`; é outra normalização,
consistente entre si e fora do escopo desta rodada — não foi tocada.)

### 2.1 Os 2 portos do Cáspio saem do cenário (não é muleta, é rota que não existe)

O Cáspio é **mar fechado** e o modelo não tem navegação nele: no `nav_mask.png` a
bacia é um componente de água **próprio, de 2.183 células** (conectividade 8, a do
`NavGrid`), separado do oceano (2.110.356). Corrigir o rótulo de Baku/Sumqayit para
`asia` os **ativava** e o Azerbaijão passava a comprar comida no mercado mundial por
uma rota que não existe.

Medido (365 d, `--ai=1 --urban=1 --threads=1 --fx=real`, comida média de 30 d, dado
final da rodada — 157 portos, fazenda pela regra POP_PER_TON):

| Configuração | LBY | EGY |
|---|---:|---:|
| recal (dado do `c773150`) | 1,000 | 0,906 |
| recal3 **sem** os 2 portos do Cáspio (o que ficou commitado) | 1,000 | **0,930** |
| recal3 **com** os 2 portos do Cáspio ativos (`landmass` `asia`) | 1,000 | **0,897** |

Primeira medição (antes da correção da fazenda do §3, 159 portos com o Cáspio ativo):
**LBY 0,878** e EGY 0,859 — foi o que levantou a suspeita. Depois da fazenda corrigida
o LBY não é mais afetado, mas o EGY perde **3,3 p.p.** com uma rota que não existe.

O Azerbaijão **não tem porto oceânico**; os dois portos do Cáspio entram em
`EXCLUDED_PORTS` com o motivo. `AZE` fica sem rota marítima — que é o estado correto
do dado (o país tem litoral, mas só em mar fechado).

---

## 3. Frente 3 — população de unidade administrativa (Weifang, Peshawar e 2 iguais)

`sim_population` é a **aglomeração** (unidade de simulação). Weifang e Peshawar traziam
a **prefeitura/distrito** — `q016_city_pop_fix.md` §4.3 já apontava as duas e deixava a
decisão pendente. A referência usada é a **mesma tabela que o pipeline já usa** para
`agglomeration_population` (`tools/agglomerations_2026.csv`, snapshot de Thomas
Brinkhoff / citypopulation.de de 2026-01-01), o que mantém o número comparável com as
outras 650 cidades.

| Cidade | Antes (`sim_population`) | Depois | Fonte (`agglomerations_2026.csv`) | Razão |
|---|---:|---:|---|---:|
| CHN-WEI Weifang | 9.386.705 | **3.375.000** | citypopulation.de: Wéifāng 3.375.000 (Adm-Urb), rank 178 | 0,36 |
| PAK-PES Peshawar | 4.758.762 | **2.175.000** | citypopulation.de: Peshāwar 2.175.000 (CUA), rank 282 | 0,46 |
| CHN-HAR Harbin | 10.009.854 | **5.600.000** | citypopulation.de: Hā'ĕrbīn 5.600.000 (Adm-Urb), rank 100 | 0,56 |
| TUR-DIYA Diyarbakır | 1.833.684 | **1.120.000** | citypopulation.de: Diyarbakır 1.120.000 (Aggl), rank 585 | 0,61 |

As quatro entraram em `POPULATION_OVERRIDES` (com `source`, `method:
agglomeration_table`, `confidence: high` e `note`), no mesmo formato de AFG-MAZ. Harbin
e Diyarbakır não estavam no pedido, mas são **o mesmo defeito com a mesma fonte
clara**: o valor do cenário é exatamente o total da unidade de cima (prefeitura de
Harbin no censo 2020 = 10.009.854; província de Diyarbakır = 1.833.684).

### 3.1 A fazenda inicial tem de acompanhar a população (senão a cidade exporta terra que não tem)

`build_world_cities.apply_population_overrides()` agora recalcula também o
`initial_farm_tons` pela **mesma regra da fonte** (`POP_PER_TON = 0,0003` t/dia por
habitante), e não só o `sim_population`. Sem isso a fazenda continuava dimensionada
para a população **errada**: o `output_tons_per_day` do CSV é o **teto** em
`resolve_initial_farms()` (que faz `capacity = max(capacity, alvo)` e nunca baixa), então
Weifang ficava com **2.816 t/dia** para uma necessidade de **1.012**.

| Cidade | Fazenda antes (t/dia) | Fazenda depois |
|---|---:|---:|
| CHN-WEI Weifang | 2.816,0 | **1.012,5** |
| PAK-PES Peshawar | 1.427,6 | **652,5** |
| CHN-HAR Harbin | 3.003,0 | **1.680,0** |
| TUR-DIYA Diyarbakır | 550,1 | **336,0** |
| AFG-MAZ Mazār-e Sharīf | 145,4 | **145,3** (mesma regra; 0,1 t/dia) |

Esse era o resíduo que fazia CHN/PAK/TUR abastecerem o mundo com produção que a
população corrigida não justifica. Com a fazenda certa, o portão de comida fecha
inteiro (§5.4) — e **não** por ajuste de constante: é o mesmo `0,0003` que a fonte usa
para todas as 834 cidades.

### 3.2 Mesmo padrão, **não** corrigido (fonte não é inequívoca)

Varredura de todas as 834 cidades contra a tabela de aglomerações (`sim/agg ≥ 1,5`):
sobram **2** casos que não corrigi —

| Cidade | `sim_population` | Aglomeração | Por que não mexer |
|---|---:|---:|---|
| VNM-HAI Haiphong | 2.625.200 | 1.460.000 | 2.625.200 não é um total administrativo identificável (a municipalidade vietnamita é ~2,1 M); precisa de fonte primária do GSO. |
| CHN-CHO Chongqing | 22.000.000 | 13.200.000 | não é a municipalidade (32 M) nem a aglomeração da tabela (13,2 M); o número não tem unidade declarada. |

Nenhuma das duas foi tocada. Ficam registradas para uma rodada com fonte primária.

---

## 4. Frente 4 — `build_scenario_data.py` não apaga mais o `i18n.json`

Antes: o script reconstruía `i18n.json` **só** a partir de `i18n.csv` e apagava as 137
chaves que não nascem de CSV (`map.region.*`, `map.legend.routes.*`,
`city.services.*`, `dock.services`, `map.icon.industry`) — a UI caía para a chave crua.
Na rodada 2 isso foi contornado à mão (§10.1 do `recal_areas_pop.md`).

Agora `merge_i18n()` mescla: **o CSV manda** nas chaves que ele tem, **o que só existe
no JSON é preservado**, e linha vazia de CSV não apaga valor. O build imprime
`i18n.json: 2252 chaves (CSV 2113, preservadas do JSON 139, sobrescritas 0)`.
As 139 (137 + os 2 nomes dos portos do Cáspio, que saíram do cenário) ficam como chaves
órfãs inofensivas — o merge **não** apaga nada por projeto.

Teste: `tools/test_build_scenario_i18n.py` (3 casos: CSV vence no conflito, chave só do
JSON sobrevive, linha vazia não apaga; + idempotência). **OK**.

---

## 5. Frente 5 — portão de comida por média de 30 dias

### 5.1 Implementação (em `tools/`, nada no núcleo)

- `tools/economy_world_test.gd`: a cada dia, agrega `p_need`/`p_consumed` do bloco
  **leve** do status por nação (sem `get_city_status`, que é o detalhe pesado) e mantém
  uma janela deslizante de `FOOD_WINDOW_DAYS = 30`. O relatório passa a gravar
  `food` (último dia, como antes), **`food_avg30`**, `food_avg30_days`,
  `food_avg30_min`, `food_avg30_max`, e `meta.food_metric` declara o portão.
- O **hash de trajetória** ignora os campos derivados `food_avg30*`: assim o baseline
  remedido fecha **exatamente** o hash do `recal2` (`e13f92a5…`), provando que a
  mudança é só de relatório.
- `tools/aggregate_s19_sweep.py`: o portão é o delta de `food_avg30` (≤ 2,5 p.p.),
  isentos `SOL, DJI, SAH, JOR, YEM`; as duas medidas vão para o arquivo agregado
  (`baseline`/`worst_food` = último dia; `baseline_avg30`/`worst_avg30` = média 30 d).
- `tools/s19_world_sweep_parallel.ps1`: novos `-Baseline` e `-FoodExempt`; o runner
  recebe `--food-exempt`, então a isenção é a mesma nas duas pontas.

### 5.2 Baseline remedido (`recal3base`)

O `recal2_world_sweep.json` só tinha a foto do último dia. Para comparar média com
média, o **dado do `c773150` foi remedido** com o runner novo:

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools/s19_world_sweep_parallel.ps1 -OutPrefix recal3base_
python tools/aggregate_s19_sweep.py --baseline docs/economy/perf/recal2_world_sweep.json \
  --dir "<user>/world_sweep" --prefix recal3base_ --suffix _365.json \
  --logdir "%TEMP%/opengs_q015_sweep" --out docs/economy/perf/recal3base_world_sweep.json
```

Artefato: `docs/economy/perf/recal3base_world_sweep.json`. Hash de trajetória
`e13f92a5…` nas 5 variantes — **idêntico ao `recal2`**, sonda `f37205f1…` diferente.

### 5.3 Sweep novo (`recal3`)

```
powershell -NoProfile -ExecutionPolicy Bypass -File tools/s19_world_sweep_parallel.ps1 `
  -OutPrefix recal3_ -Baseline res://docs/economy/perf/recal3base_world_sweep.json
python tools/aggregate_s19_sweep.py --baseline docs/economy/perf/recal3base_world_sweep.json \
  --dir "<user>/world_sweep" --prefix recal3_ --suffix _365.json \
  --logdir "%TEMP%/opengs_q015_sweep" --out docs/economy/perf/recal3_world_sweep.json
```

365 d, `--ai=1 --urban=1 --threads=1`, 6 processos (5 variantes + sonda `--fx=float`).
Artefato: `docs/economy/perf/recal3_world_sweep.json`.

### 5.4 Portões

| Portão | Limite | Medido | Veredito |
|---|---|---|---|
| Falências de construção (pior variante) | ≤ 28 | **8** (8 nas 5) | OK |
| Dinheiro conservado (drift) | ≤ 0,01 | **−0,0010** (sonda −0,0000) | OK |
| Determinismo peg ×5 | hash igual | `0817959a…` nas 5 | OK |
| Sonda `--fx=float` | hash diferente | `8bf2ceda…` | OK |
| Comida (média 30 d), pior nação **não isenta** | ≤ 2,5 p.p. | **MRT −1,73 p.p.** | **OK** |
| Isentas que caem | — | YEM −7,81 p.p.; DJI −2,75 p.p. | isentas |
| Pior nação (último dia) | — | SAH 0,196 | informativo |

**Todas as 5 variantes de aceite passam** — inclusive EGY, que **melhora** (+2,44 p.p.
na média de 30 d e +35,96 p.p. na foto do último dia). Só a sonda `--fx=float` (que não
entra no portão) tem falhas internas de baseline: 11 falências, EGY 0,743, LBY 0,792,
PSX 0,196 — o regime cambial é sensível, como já era no Q-016.

### 5.5 Comida por nação — as duas medidas

`recal` = `recal3base` (dado do `c773150`, medida nova) · `novo` = `recal3`.

| Nação | recal último dia | recal média 30 d | novo último dia | novo média 30 d | Δ média 30 d | Δ último dia |
|---|---:|---:|---:|---:|---:|---:|
| EGY | 0,640 | 0,906 | 1,000 | 0,930 | **+2,44 p.p.** | +35,96 p.p. |
| LBY | 1,000 | 1,000 | 1,000 | 1,000 | 0,00 | 0,00 |
| YEM (isenta) | 0,942 | 0,988 | 1,000 | 0,910 | −7,81 p.p. | +5,83 p.p. |
| PSX | 0,190 | 0,239 | 0,197 | 0,228 | −1,01 | +0,73 |
| SAH (isenta) | 0,184 | 0,205 | 0,196 | 0,216 | +1,10 | +1,23 |
| DJI (isenta) | 0,351 | 0,353 | 0,345 | 0,325 | −2,75 | −0,55 |
| SOL (isenta) | 0,614 | 0,630 | 0,616 | 0,635 | +0,55 | +0,27 |
| SGP | 1,000 | 1,000 | 1,000 | 1,000 | 0,00 | 0,00 |
| MRT | 0,956 | 0,962 | 0,931 | 0,945 | −1,73 | −2,43 |
| SAU | 1,000 | 1,000 | 1,000 | 1,000 | 0,00 | 0,00 |
| ESP | 0,969 | 0,969 | 0,969 | 0,969 | 0,00 | 0,00 |
| SOM | 0,989 | 0,997 | 1,000 | 0,993 | −0,35 | +1,11 |
| PHL/CUB/NZL/MDG/JAM/LKA/BHS/FJI/TTO/BRB/SLB/BHR/QAT/BRN | 1,000 | 1,000 | 1,000 | 1,000 | 0,00 | 0,00 |
| IDN / TUR / GEO | 1,000 | 1,000 | 1,000 | 1,000 | 0,00 | 0,00 |
| CHN / PAK | 1,000 | 1,000 | 1,000 | 1,000 | 0,00 | 0,00 |

As outras 148 nações ficam dentro de ±1 p.p. na média de 30 d (o pior é SOM −0,35 p.p.;
o melhor, MNG +0,38 p.p.).

**Leitura:** o EGY — o importador sem aptidão que o head já havia identificado como
oscilante (0,64–1,00 no ciclo diário) — foi o caso que motivou a métrica nova, e é o
que mais ganhou: **0,640 → 1,000 na foto e 0,906 → 0,930 na média de 30 d**. A
reprovação que apareceu no meio da rodada (−2,91 p.p.) era o resíduo da fazenda de
Weifang/Harbin/Peshawar/Diyarbakır dimensionada para a população antiga (§3.1): com a
fazenda pela regra, o mundo fecha **sem nenhuma nação não isenta abaixo de −1,8 p.p.**
Nenhuma muleta foi criada — sem subsídio, sem tarifa, sem constante, sem aptidão
inventada.

---

## 6. Frente 6 — goldens, testes e validação

Goldens regenerados porque o dado mudou; cada um revalidado **contra si mesmo** com
0 diferenças.

| Golden | save_sha256 | status_sha256 |
|---|---|---|
| `A0_60_urban.json` (60 d, IA, `--urban=1`, 2 rodadas) | `dcad384c67b53a1b0b5152fda8bd315b9fc331a563e66270dbdf4894dc43c06a` | `46047575196a7f60b7c708f15fa006dfa21b2789c9949c6b1093e021b93b53d4` |
| `real_60_ai.json` | `042f6f548db0171c3c4a04f505d2c6fcac6ae5a8da600c8ffa90b8316b35db22` | `4c69889f725290da7cb4ed235c49ae90b2916b83181e922d8a42631d6cc80d4f` |
| `real_60_noai.json` | `ee3c5e850fd281092852f8562a66a6061cc9910a6d0573f80ec643d5cc9d25cd` | `8d6f3d4444d66b9acb493a1b418ac5c9c023d3879557662b79ccb244df803217` |
| `demo_365_noai.json` | `652b5f8b…` (**inalterado**) | `65a414f0…` (**inalterado**) |

`demo_365_noai` não mudou: cenário sintético AGRIA/PORTUS, alheio ao dado do mundo.

| Teste | Resultado |
|---|---|
| `economy_integration_test.gd` | **OK** |
| `economy_ai_test.gd` | **OK** |
| `population_demo_test.gd` | **OK** |
| `economy_save_load_roundtrip_test.gd` | **OK — 0 diffs** |
| `economy_refactor_equiv.gd` × 3 goldens S0 | **OK — 0 diferenças** em cada |
| `world_equivalence.ps1 -Baseline A0_60_urban.json -Rounds 2` | **PASS — 0 diferenças** |
| `nav_strait_test.tscn` (17 portos, incl. os 14 novos) | **PASS** |
| `validate_scenario_data.py` | **0 erros, 0 avisos** (11 arquivos; 157 portos) |
| `test_build_scenario_i18n.py` | **OK** |

Pipeline rodado: `build_world_cities.py --write-scenario` (0 AVISO, 157 portos,
`port_of` em 157/157) → `build_scenario_data.py` → `validate_scenario_data.py`.

---

## 7. Pendências

1. **Nenhuma nação não isenta reprova** no portão de comida (pior: MRT −1,73 p.p.).
   `YEM −7,81 p.p.` e `DJI −2,75 p.p.` na média 30 d caem dentro da isenção declarada;
   a série das duas merece acompanhamento na próxima rodada.
2. **VNM-HAI** e **CHN-CHO**: mesmo padrão de unidade administrativa, sem fonte
   inequívoca (§3.2).
3. `i18n.json` guarda 139 chaves que não vêm do CSV (por desenho). Se um dia houver
   uma faxina, ela tem de ser explícita — o pipeline não apaga mais nada.
4. `AZE` fica sem porto (Cáspio). Se o jogo ganhar navegação em mar fechado, os dois
   portos voltam com `landmass` `asia`.
5. **Pendência de ferramenta (menor):** `build_world_cities.py --check` conta 1.349
   chaves de i18n e o `--write-scenario` 2.115; o `--check` não inclui as chaves de
   cidade/porto na contagem. Não afeta o dado emitido.
