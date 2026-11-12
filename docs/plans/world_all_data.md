# Todas as nações — dados do mundo (2026)

Fase de **dados**. Nada aqui é consumido pelo jogo ainda: `data/definitions/scenario/`
não foi tocado (a promoção é outra tarefa, junto com o mapa). O objetivo desta
fase é ter, para as 231 nações do mapa, uma seleção de cidades com cobertura,
fronteira e portos definidos por regra, com fonte por linha e ausência declarada
onde a fonte não tem o dado.

## 1. Fontes

| Dado | Fonte | Ano |
| --- | --- | --- |
| População da nação | World Bank WDI `SP.POP.TOTL` | último ano disponível (2024/2025) |
| PIB PPP | World Bank WDI `NY.GDP.MKTP.PP.CD` | último ano disponível |
| Perfil de IA | WDI `NV.IND.MANF.ZS`, `NY.GDP.TOTL.RT.ZS`, `NV.AGR.TOTL.ZS` | idem |
| Área, moeda, continente, vizinhos | GeoNames `countryInfo.txt` | 2026 |
| Cidades (população, coordenadas) | GeoNames `cities15000` + `cities500` (pop ≥ 1.000) | 2026, **city proper** |
| Nomes pt-BR / es | rótulos do Wikidata (`pt-br` → `pt` → inglês; `es` → inglês) | 2026 |
| Fronteiras / costa | `map/map_data/atlas/world_province.png`, `world_sdf.png` | 2026 |
| Bandeiras | flagcdn.com `w40` (domínio público) | 2026 |

Os 40 arquivos de cidade marcados `legacy=yes` mantêm a **população do cenário**
(base aglomeração, herdada da fase 1) — por isso `cities_source.csv` tem
`pop_src` por linha (`agglomeration` para as 40 legadas, `city_proper` para as
818 novas). Misturar as duas bases é uma dívida declarada (§8).

## 2. Regras

```
cota(nação) = 1 + clamp(1 + ⌊pop/25M⌋ + ⌊PIB_PPP_bn/1500⌋ + ⌊área_km²/600k⌋, 1, 24)
cobertura   = toda cidade-fonte da nação a ≤ 700 km de uma cidade escolhida
fronteira   = cidade nova a ≥ 40 km de outro país (≥ 25 km se área < 50.000 km²)
espaçamento = 25 km entre cidades; 60 km se a maior tiver o dobro da população
porto       = cidade a ≤ 25 km da costa; ponto de costa a ≥ 15 km vira porto próprio
cor         = vizinhos de fronteira com ΔE(Lab) ≥ 30
```

Capital e as cinco maiores cidades da nação entram mesmo dentro da faixa de
fronteira (ficam marcadas no campo `note` com a distância). A cota é aplicada às
cidades novas; a cobertura pode ultrapassá-la (§4) e as legadas entram por
contrato.

## 3. Contagens

| Item | Valor |
| --- | --- |
| Nações | **231** |
| Cidades | **858** (40 legadas + 818 novas) |
| Portos | **137** (114 do cenário + 23 pontos de costa) |
| Construções iniciais (`*_wheat`) | 858 |
| Chaves i18n | 1.457 |

Nações por tipo: **192 soberanas**, **34 territórios**, **3 especiais**
(Taiwan, Kosovo, Sara Ocidental), **2 observadoras** (Palestina, Vaticano).

Por região: África 55, Europa 52, América do Norte 38, Ásia 35, Oceania 24,
Oriente Médio 14, América do Sul 13.

Cidades por nação: mínima 1, mediana 2, média 3,7, máxima 40.

Top 10 por cidades: RUS 40, USA 28, BRA 25, CAN 25, CHN 25, IND 25, IDN 19,
AUS 18, PAK 14, FRA 13.

### 3.1 Nações acima da cota (24)

A busca de cobertura continua depois de a cota ser atingida, então países
grandes ou com ilhas distantes passam do alvo: **RUS 40/25**, USA 28/25,
CAN 25/20, AUS 18/16, ARG 9/7, DZA 7/6, COL 6/5, PYF 5/2, CHL 5/3, LBY 5/4,
NOR 4/2, PNG 4/2, PRT 4/2, MRT 4/3, MYS 4/3, COG 3/2, ECU 3/2, FIN 3/2,
FSM 3/2, NZL 3/2, OMN 3/2, SAH 3/2, SHN 3/2, SWE 3/2. O teto duro de 40
cidades por nação é respeitado (RUS está exatamente nele).

### 3.2 Nações abaixo da cota (45)

A fonte acabou: todas ficam com **1 de 2** cidades. São 19 soberanas pequenas
(AND, ATG, BHR, BRB, DMA, GMB, GRD, KNA, LCA, LIE, LUX, MCO, NRU, PLW, SGP, SMR,
SYC, TUV, VCT) e 26 territórios de uma cidade só (ABW, AIA, ALD, ASM, BLM, BMU,
COK, CUW, FLK, FRO, GGY, GIB, GUM, HKG, IMN, JEY, MAC, MAF, MNP, MSR, NFK, NIU,
SPM, SXM, VGB, WLF). Nenhuma cidade foi inventada para preencher cota.

## 4. Cobertura antes/depois (`_coverage_before_after.csv`)

| | Antes (fase 1) | Depois |
| --- | --- | --- |
| Raio máximo até a cidade mais próxima | **9.024,5 km** (FRA, Guiana Francesa) | **699,9 km** (PRY) |
| Raio médio | 886,0 km | 293,8 km |
| Nações com raio > 700 km | 19 | **0** |

As 19 nações que estouravam: ARG, AUS, BRA, CAN, CHN, ESP, FIN, FRA, IDN, IND,
JPN, MEX, NOR, PRT, RUS, SAU, SWE, USA, ZAF. O pior caso antes era a França
somando Guiana Francesa, Polinésia e Nova Caledônia a Paris.

## 5. Contrato legado

- **40 cidades** com `legacy=yes` mantêm id, coordenadas e população do cenário
  (`build_world_cities.py` valida os três contra `scenario/cities.csv`).
- **18 portos** do cenário anterior à fase 1 (lista `LEGACY_PORTS` no build:
  Dover, Calais, Hamburgo, Amsterdã, Antuérpia, Mana, La Guaira, Paramaribo,
  Georgetown, Le Havre, Marselha, Saint-Nazaire, Roterdã, Glasgow, Bristol,
  Newcastle, Maracaibo, Belfast) mantêm id e coordenada.
- **VEN-PAY (Puerto Ayacucho) → VEN-CBL (Ciudad Bolívar)**: a cidade ficava a
  menos de 5 km da fronteira colombiana; Ciudad Bolívar é o centro regional
  equivalente no interior. VEN-CBL está na fonte e **não** está no cenário
  (só entra na promoção).

### 5.1 O que não é preservado

As outras **212 cidades do cenário** fora do contrato legado não são preservadas
por id: a seleção nova as re-deriva do GeoNames, então a mesma cidade física
aparece com id novo (Tirana: `ALB-TIR` → `ALB-TIRA`). Como a seleção nova é maior
e por cidade própria, várias delas também mudam de população (Brasília 3,9 M
aglomeração → 2,2 M cidade). **A promoção precisa de plano de migração de id**
(save/load, construções e rotas de porto apontam para o id antigo).

## 6. Cores

O importador exige ΔE ≥ 35 na escolha e repara pares < 30, mas as cores fixas do
atlas não entravam no reparo. Auditoria dos 304 pares de vizinhos de fronteira
encontrou três abaixo de 30:

| Par | ΔE antes | Correção |
| --- | --- | --- |
| FRA–SUR (via Guiana Francesa) | 24,5 | SUR `#488ca0` → `#448e5a` |
| BEL–NED | 25,1 | BEL `#b69c4a` → `#7ba744` |
| DEU–FRA | 25,3 | FRA `#506c9c` → `#4462a7` |

Resultado: **menor par 35,1** (ZAF–ZWE), nenhum par < 35. As correções estão em
`COLOR_OVERRIDE` no importador e são reaplicadas a cada geração; o build só
consome `color` de `nations_source.csv`.

## 7. Bandeiras

231 nações → 231 PNGs `w40` em `assets/flags/`: 167 originais + 9 da fase 1 +
**55 desta fase** (flagcdn.com, domínio público). Nenhuma falta; `CREDITS.txt`
registra origem e licença. Os arquivos `.import` serão gerados pelo Godot na
próxima abertura do editor (esta tarefa não roda Godot).

## 8. Lacunas declaradas (nada estimado)

1. **PIB PPP ausente em 32 nações** (AIA, ALD, ASM, BLM, COK, CUB, ERI, FLK, GGY,
   GIB, GUM, IMN, JEY, LIE, MAF, MCO, MNP, MSR, NCL, NFK, NIU, PRK, PYF, SAH, SHN,
   SPM, TWN, VAT, VEN, VGB, WLF, YEM): `gdp_ppp_bn` vazio e `data_quality =
   missing_gdp`. A cota dessas nações usa só população e área.
2. **População sem série do Banco Mundial em 16 nações** (AIA, ALD, BLM, COK, FLK,
   GGY, JEY, MSR, NFK, NIU, SAH, SHN, SPM, TWN, VAT, WLF): usa-se o valor do
   GeoNames `countryInfo` e a coluna `source` diz isso linha a linha.
3. **Nomes sem rótulo no Wikidata: 74 cidades em pt, 71 em es.** Ficam com a
   grafia inglesa/GeoNames (ex.: `Ust’-Ilimsk`, `ş`/`ā` em transliterações
   asiáticas). 248 cidades têm exônimo pt (`Moscou`, `Pequim`, `Nova York`,
   `Cidade do México`, `Xangai`, `Bombaim`, `Calcutá`, `Bagdá`, `Teerã`, …).
   Quatro nomes longos do GeoNames foram corrigidos por `NAME_OVERRIDE`
   (Mosul, Salalah, Ibri, Chisinau).
4. **Bases de população mistas**: 818 cidades em base cidade (GeoNames) e 40
   legadas em base aglomeração (cenário). Comparar populações entre elas é
   enganoso; `pop_src` está por linha.
5. **Portos sem badge de cidade**: 114 dos 137 portos vêm do cenário como pontos
   independentes (`port_of` vazio) porque o cenário não guarda a cidade de
   origem; só os 23 novos têm `port_of`. Na UI eles não têm cidade para exibir
   badge. 330 cidades têm `port = city` (o porto é a própria cidade).
6. **Cidades em ilhas e possessões remotas**: 34 territórios entram com 1–5
   cidades. O usuário decide quais remover (base territorial própria).

## 9. Territórios (para o usuário escolher o que remover)

Com número de cidades: **GRL 5**, **PYF 5**, SHN 3, CYM 2, NCL 2, PRI 2, TCA 2,
VIR 2, e com 1 cidade: ABW, AIA, ALD, ASM, BLM, BMU, COK, CUW, FLK, FRO, GGY,
GIB, GUM, HKG, IMN, JEY, MAC, MAF, MNP, MSR, NFK, NIU, SPM, SXM, VGB, WLF.

## 10. Como reproduzir

```powershell
# 1. fontes (precisa do cache GeoNames/Banco Mundial baixado, não versionado)
python tools/import_world_sources.py --write --fetch-names --cache <dir>
# 2. projeção e validação do contrato
python tools/build_world_cities.py --check
python tools/build_world_cities.py            # gera data/definitions/world/out/
```

`--check` falha (exit ≠ 0) com qualquer aviso: cota divergente, capital ≠ 1,
cobertura > 700 km, legado divergente, porto legado fora do cenário, VEN-PAY
presente, VEN-CBL ausente, ΔE de vizinhos < 30, id duplicado. Estado atual:
**0 avisos**.

## 11. Verificação desta fase

- `import_world_sources.py --check`: 231 nações, 858 cidades (40 legadas), 137
  portos, 0 nações com cobertura > 700 km, 0 vizinhos com ΔE < 30.
- `build_world_cities.py --check`: 231 / 858 / 137 / 858 construções / 1.457
  chaves i18n, 0 avisos.
- `out/` regerado a partir das fontes, sem escrever em `scenario/`.

## 12. Pendências

1. Promoção para `data/definitions/scenario/` (não feita aqui) com migração de
   id das 212 cidades re-derivadas e merge de `i18n.csv`.
2. `GAME_STATE.md` ainda descreve 66 nações / 252 cidades; atualizar na promoção.
3. Badge de porto para os 114 portos sem `port_of` exige decidir a cidade de
   origem no cenário.
