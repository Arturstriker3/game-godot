# Área das províncias — células degeneradas no Voronoi (`province_areas.json`)

**Data:** 2026-11-17 · **Branch:** `data/province-area-fix` · **Escopo:** dado + gerador
Python (`tools/compute_province_areas.py`). **Nenhuma** execução de Godot, build ou
regeneração de camada do atlas nesta frente.

## 1. Resumo

`data/definitions/natural/province_areas.json` era gerado por um **Voronoi dos
centróides** das províncias sobre os pixels de terra do `nav_mask`. Esse método
não é restrito ao território da província: cada pixel de terra vai para o
centróide mais próximo **em graus**, não em km. Como 1° de longitude vale
`cos(lat)` vezes 1° de latitude, no hemisfério sul uma província a −40° "vence"
o continente antártico inteiro.

Resultado: **806 províncias (5,2% das 15.547)** com área fora da faixa
[0,5× – 2×] da área contada no raster do atlas, somando **9,65 milhões de km²
fantasma** (835 províncias e 9,63 M km² se a referência for só a terra do
`world_sdf`, sem o `nav_mask`). O caso extremo é `Tasmania 1` (id 14087):
**1.781.428 km²** contra **8.211 km²** contados na mesma máscara (8.351 km² só
com a terra do SDF) e 8.661 km² no Natural Earth — **217×**.

A correção troca a estimativa por **contagem dos pixels da própria província no
raster do atlas** (`world_province.png`, 8192×4096, ~4,9 km/texel), mascarada
pela terra do `world_sdf.png` e pelo `nav_mask.png`. Determinística, só numpy/PIL,
mesmo comando de antes. Efeito medido: a área cultivável somada das 834 cidades
cai **15,3%** (26,82 → 22,72 milhões de km²), concentrada em 4 cidades
(AUS-GEE, AUS-PER, CHL-PUN, NZL-CHR) que herdavam a Antártida.

## 2. Varredura: quem divergia e de quanto

Base de comparação (por província, `id` de `map/map_data/atlas/provinces.json`):

| Referência | Fonte | Total |
|---|---|---|
| **JSON (antes)** | `province_areas.json` (Voronoi/nav_mask) | 148.377.983 km² |
| **Raster do atlas (máscara do gerador)** | `world_province.png` ∩ terra de `world_sdf.png` ∩ `nav_mask.png` | 145.725.949 km² |
| **Raster do atlas (só a terra do SDF)** | `world_province.png` ∩ `world_sdf.png` (R > 128) | 146.566.502 km² |
| **Admin-1 do Natural Earth** | `provinces.json:area_global` × km²/px (cos lat) | 147.076.136 km² |

Critério do briefing (razão > 2× ou < 0,5×), contra a referência do próprio
gerador (raster ∩ `nav_mask`):

| Classe | Províncias | Efeito |
|---|---|---|
| Inflada (> 2×) | **738** | +9.651.338 km² fantasma |
| Deficitária (< 0,5×) | **68** | −8.170.782 km² |
| Sem pixel no raster (fallback NE) | 132 | — |
| **Total divergente** | **806 de 15.547 (5,2%)** | líquido +1.480.556 km² |

Confrontando só com a terra do `world_sdf` (sem descontar lagos e ilhas
removidas) as classes viram **741 infladas / 94 deficitárias = 835 (5,4%)**, com
9.626.522 km² de excesso. A diferença é água de lago e ilha removida — os dois
lados são verdadeiros; o gerador usa a máscara mais restritiva de propósito
(área cultivável não inclui lago).

Contra o admin-1 do Natural Earth: **832** províncias fora da faixa — ou seja,
o raster (4,9 km) e o vetor do NE concordam entre si, e o JSON discordava dos
dois.

**142 nações afetadas.** Topo por área fantasma:

| Nação | Províncias | Excesso (km²) |
|---|---|---|
| AUS | 10 | 4.352.445 |
| CHL | 7 | 2.105.681 |
| ATA | 1 | 1.188.170 |
| NZL | 5 | 635.905 |
| PHL | 40 | 78.050 |
| GBR | 94 | 64.718 |
| MDG | 1 | 48.216 |
| USA | 7 | 46.884 |
| HUN | 24 | 45.696 |
| UGA | 20 | 40.925 |

Duas classes distintas, com causas distintas:

1. **Roubo da Antártida** (AUS, CHL, NZL, MDG, ATA): poucas províncias, erros de
   30× a 283×. É o bug estrutural.
2. **Microprovíncia inflada em região densa** (GBR, SVN, PHL, LVA, HUN, AZE…):
   a célula de Voronoi precisa ter pelo menos o tamanho da célula de 16 km do
   `nav_mask`; uma província de 30 km² herda 1.500 km². Erros de 2× a 50×, mas
   em áreas absolutas pequenas.

## 3. Causa

`tools/compute_province_areas.py` (versão anterior):

```python
nav_centroids = [(px/2.0, py/2.0) for px, py in centroids]   # plano 2500x1250
tree = cKDTree(nav_centroids)
_, idx = tree.query(points, k=1)                            # pixel -> centróide
```

Três problemas somados:

1. **Voronoi sem restrição.** O pixel de terra vai para o centróide mais próximo,
   atravessando oceano e continente. Não há noção de "território da província".
2. **Métrica em graus, não em km.** `cKDTree` usa distância euclidiana no plano
   equirretangular. A 60°S, 1° de longitude = 55 km contra 111 km de latitude: o
   alcance leste-oeste é contado ~2× maior.
3. **Antártida com pouquíssimos centróides.** O continente tem só 2 províncias no
   atlas (id 1724 "Antarctica" e 2550), com centróides entre −80° e −90°. Um
   centróide tasmaniano a −43° fica "mais perto" (em graus) do litoral antártico
   a −70° do que o centróide antártico a −80°, 120° de longitude adiante.

Medição do roubo: o `nav_mask` tem **12.397.241 km² de terra abaixo de −60°**
(8,4% da terra do mapa). O Voronoi distribuiu isso entre **20 províncias**, e as
duas da Antártida ficaram com apenas **5,35 milhões**: os outros **~7,05 milhões
de km²** foram para a Tasmânia 1, Western Australia 6/49/131/235, Magallanes
(CHL), Southland (NZL) e Anosy 2 (MDG). No raster, a Antártida é uma província só:
**12.204.985 km²** (id 1724).

## 4. Impacto na economia

`Core/economy/geography_service.gd`:

- `_load_provinces()` lê `province_areas.json` → `_area_by_province` (linha 1591);
- `province_cultivable_area_km2()` = `área × cultivable` (fração herdada do
  território natural pelo centróide da província);
- `city_cultivable_area_km2()` = soma das províncias da bacia (`assign_provinces`)
  ponderada por `province_share`;
- `city_land_capacity_t_day()` = área cultivável × produtividade FAO → **é o teto
  de terra das fazendas**; `city_crop_suitability()` usa a mesma área como peso.

Não existe `tools/build_energy_*.py` nesta branch, então a réplica província→cidade
foi escrita em Python seguindo `assign_provinces` (cidade da mesma nação mais
próxima do centróide, restrita à mesma `landmass`; cidade sem terra recebe fração
da província mais próxima da nação). Ela fica em `tools/audit_province_areas.py
--cities` e é reproduzível:

```powershell
python -c "open(r'$env:TEMP\old.json','wb').write(__import__('subprocess').run(['git','show','HEAD:data/definitions/natural/province_areas.json'],capture_output=True).stdout)"
python tools/audit_province_areas.py --areas $env:TEMP\old.json --cities $env:TEMP\old.json
```

**Área cultivável somada das 834 cidades (km²):**

| | antes | depois | Δ |
|---|---|---|---|
| Total | 26.817.348 | 22.718.474 | **−15,3%** |

Cidades que mais perdem:

| Cidade | Nação | antes | depois | perda | % |
|---|---|---|---|---|---|
| AUS-GEE | AUS | 1.292.940 | 55.628 | 1.237.312 | 95,7% |
| AUS-PER | AUS | 1.173.091 | 46.851 | 1.126.239 | 96,0% |
| CHL-PUN | CHL | 868.072 | 54.287 | 813.785 | 93,7% |
| NZL-CHR | NZL | 430.642 | 74.808 | 355.834 | 82,6% |
| CHL-PUE | CHL | 126.758 | 108.982 | 17.776 | 14,0% |
| MDG-FIA | MDG | 81.420 | 65.067 | 16.353 | 20,1% |
| CAN-HAL | CAN | 101.787 | 91.643 | 10.144 | 10,0% |
| AUS-DAR | AUS | 153.477 | 144.137 | 9.340 | 6,1% |
| JPN-SAP | JPN | 63.764 | 54.667 | 9.097 | 14,3% |
| PHL-MAN | PHL | 44.303 | 35.765 | 8.538 | 19,3% |

**Quanto da área cultivável vinha das células divergentes:** AUS-GEE 95,9%,
AUS-PER 96,7%, CHL-PUN 95,6%, NZL-CHR 85,1%, HUN-BUD 73,8%, KOR-SEO 64,2%,
PHL-CEB 43,0%. Nessas cidades o teto de fazendas era, na prática, a Antártida.

O efeito é pior do que "área a mais": a **aptidão agrícola da província vem do
clima no seu centróide**. `Tasmania 1` (cultivable 0,696, `temperate_humid`) e
`Western Australia 6` (cultivable 0,488, `subtropical_humid`) aplicavam clima
tasmaniano e subtropical a 1,78 e 1,15 milhão de km² de gelo antártico. A
produção agrícola australiana, chilena e neozelandesa estava superdimensionada
por terra que não existe no mapa.

## 5. Correção aplicada

`tools/compute_province_areas.py` passa a contar o raster, não a estimar:

- id da província por pixel em `map/map_data/atlas/world_province.png`
  (**id = R + G·256**, confirmado: no centróide da Tasmânia 1 o pixel é 14087);
- máscara de terra = `world_sdf.png` canal R > 128 (mesma definição do shader)
  **E** `nav_mask.png` ≥ 128 (terra navegável: sem lagos, sem ilhas/nações
  removidas pela curadoria);
- cada pixel vale `R² · cos(lat) · dlon · dlat` (R = 6371 km) — a mesma projeção
  da camada mundial; acumula em blocos de 512 linhas (`bincount` sobre 65.536 ids);
- **fallback mantido:** província sem nenhum pixel no raster recebe
  `provinces.json:area_global` (Natural Earth admin-1) na mesma projeção. Agora
  são **132** províncias (6.361,5 km²) contra 240 antes; nenhuma área positiva é
  sobrescrita;
- `_meta` declara método, raster, máscaras, resolução, contagem e soma do fallback.

Custo: ~1,7 s, só numpy/PIL (o `scipy.spatial.cKDTree` saiu). Comando inalterado:

```powershell
python tools/compute_province_areas.py
# províncias=15547  área_terra_total=145732310 km²  natural_earth=132 (6361 km²)
```

Sanidade: **0** províncias com área zero; todas as 15.547 têm valor.

## 6. Antes / depois — as 10 maiores divergências

| id | Província | Nação | antes (km²) | depois (km²) | NE (km²) | antes | depois |
|---|---|---|---|---|---|---|---|
| 14087 | Tasmania 1 | AUS | 1.781.428 | 8.211 | 8.661 | 217,0× | 0,95× |
| 2550 | (Antártida, sem nome) | ATA | 1.192.390 | 4.220 | 5.711 | 282,5× | 0,74× |
| 11304 | Western Australia 6 | AUS | 1.155.760 | 4.697 | 4.874 | 246,0× | 0,96× |
| 11429 | Western Australia 131 | AUS | 1.013.944 | 8.470 | 8.724 | 119,7× | 0,97× |
| 9201 | Magallanes y Antártica Chilena | CHL | 844.686 | 8.810 | 10.004 | 95,9× | 0,88× |
| 9208 | Magallanes y Antártica Chilena | CHL | 834.165 | 10.705 | 12.029 | 77,9× | 0,89× |
| 14142 | Southland 2 | NZL | 376.438 | 12.466 | 12.942 | 30,2× | 0,96× |
| 11347 | Western Australia 49 | AUS | 295.572 | 9.666 | 9.977 | 30,6× | 0,97× |
| 9203 | Magallanes y Antártica Chilena | CHL | 272.812 | 7.064 | 8.441 | 38,6× | 0,84× |
| 14143 | Southland 3 | NZL | 252.413 | 9.060 | 9.411 | 27,9× | 0,96× |

E o lado que faltava — a Antártida volta a ter o continente:

| id | Província | antes | depois | NE |
|---|---|---|---|---|
| 1724 | Antarctica | 4.196.467 | **12.194.545** | 12.304.644 |

Aproximação com o Natural Earth depois da correção (razão nova ÷ NE):

| Métrica | antes | depois |
|---|---|---|
| Províncias fora de [0,5×–2×] vs NE | 832 | **43** |
| Províncias que encolheram > 2× | — | 760 (liberando 9,67 M km²) |
| Soma das áreas | 148.377.983 | 145.732.310 |

As 760 que encolheram > 2× são majoritariamente microprovíncias de GBR (97),
SVN (69), PHL (41), LVA (40), HUN (24) e AZE (22) — classe 2 da seção 2.

## 7. Resíduos conhecidos (declarados, não corrigidos)

1. **43 províncias ainda fora de [0,5×–2×] vs o Natural Earth**, todas menores no
   raster do que no vetor: `Buvuma` (UGA), `Kalangala` (UGA), `Michigan 11/19/25`
   (USA), `Ontario 48` (CAN), `Kagera 2` (TZA), `South East` (SGP). São
   províncias cujo polígono do NE inclui **água de lago** (Vitória, Grandes
   Lagos) e que o `nav_mask` — corretamente — desconta. Para área cultivável, a
   versão menor é a desejada; o resíduo só existe porque o vetor conta lago como
   terra.
2. **132 províncias no fallback NE** (ilhas menores que um texel, faixas finas de
   costa). Declaradas em `_meta`.
3. O total do raster (145,73 M km²) fica 0,9% abaixo do total do NE (147,08 M)
   justamente pelo desconto de lagos e de ilhas removidas. Coerente.

## 8. O que ainda precisa rodar (fora desta frente)

- `godot --headless --path . --script res://tools/economy_world_test.gd`
  (se existir nesta branch) e `economy_integration_test.gd`: a bacia das 834
  cidades e o teto de fazendas mudam — esperado **queda** de área cultivável,
  sobretudo AUS/CHL/NZL. Nenhum teste de Godot foi rodado aqui (briefing).
- **Golden/balance**: `docs/economy/balance_review_sim*.json` e o golden de
  equivalência A0 ficam inválidos para qualquer cenário com AUS/CHL/NZL; a
  regeneração é da frente de balanceamento, não desta.
- `economy_long_run_test.gd` só depois do golden novo.
- `GAME_STATE.md` §14: decisão a registrar pela frente que integrar (não tocado
  aqui para não conflitar com a branch de balanceamento).

## 9. Arquivos tocados

- `tools/compute_province_areas.py` — método reescrito (contagem no raster).
- `tools/audit_province_areas.py` — **novo**: auditoria reproduzível (JSON vs
  raster vs Natural Earth, e réplica da bacia das cidades com `--cities`).
- `data/definitions/natural/province_areas.json` — regenerado (15.547 valores).
- `docs/economy/province_area_fix.md` — este relatório.

Comandos de auditoria (Python 3.14, `C:\Users\artur\AppData\Local\Python\bin\python.exe`):

```powershell
python tools/compute_province_areas.py        # regenera (determinístico, ~1,7 s)
python tools/audit_province_areas.py          # valida o JSON atual
python tools/audit_province_areas.py --areas <antigo.json> --cities <antigo.json>
```

Nenhuma escrita fora destes quatro arquivos.

## 10. Verificação

Verificador independente (sessão separada) recalculou com script próprio:

- idempotência: duas execuções do gerador produzem o mesmo SHA-256;
- amostra de 10 províncias contra a contagem no raster: todas com diferença
  ≤ 0,04 km² (arredondamento de 0,1 no JSON);
- `_meta` confere; soma 145.732.310 km²; **0** províncias com área zero ou
  negativa;
- 3 províncias do fallback conferidas contra `area_global` (diferença < 0,01%);
- Antártida 4.196.467 → 12.194.545 km²; AUS 14087/11304/11429/11347 entre 4,7 e
  9,7 mil km²;
- tabelas de divergência e o top 10 reproduzidos.

