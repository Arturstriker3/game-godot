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

### 6.1 Cores tradicionais das nações (F3)

`data/definitions/world/traditional_colors.csv` (`iso3,color_hex,nota`) guarda a
cor historicamente associada de **102 nações**, já no tom "papelaria" (levemente
dessaturado, como FRA/DEU/GBR): os 19 países do G20 (a UE não é país; a África
do Sul entra em verde-oliva) e ~100 Estados com convenção estável em atlas
políticos e jogos de grande estratégia (HoI/EU/Victoria), cobrindo América
Latina, África, Oriente Médio, Ásia Central, Sudeste Asiático, Oceania e todos os
Bálcãs. Regras do resolvedor (`tools/nation_colors.py`):

1. **Prioridade:** a cor tradicional vence a paleta automática; as demais nações
   mantêm a cor automática do importador (dessaturação ×0,86).
2. **Adjacência de TERRA:** `world_province.png` com a máscara de
   `world_sdf.png` (R>128), pares com ≥ 10 px de contato; cache em
   `data/definitions/world/nation_adjacency.json` (162 tags com aresta).
3. **ΔE:** alvo 30, piso aceito 25 entre vizinhos; também exige distância RGB
   ≥ 0,16 (a mesma rede de segurança de `territory_map.gd`), para o runtime não
   trocar a cor do CSV por um tom de papelaria.
4. **Conflito entre duas tradicionais:** muda-se só o TOM da de menor prioridade
   (matiz ±12°, luminosidade ±16%, saturação ±12%) — a família (matiz) é mantida.
   A automática só cede (inclusive mudando de família) quando a tradicional
   vizinha não tem candidato dentro da própria família.

Estado atual: **102 tradicionais, 300 pares de vizinhos, menor ΔE 30,1**
(CHN–IND), **nenhum par abaixo de 30**. Das 61 novas, 48 mantiveram o tom pedido e
13 foram refinadas pelo resolvedor dentro da família (KEN, OMN, BLR, TUN, BGD,
NPL, JOR, CZE, GEO, LTU, HRV, ARM, LUX); as 41 anteriores mantêm exatamente a cor
já publicada. 12 nações automáticas vizinhas cederam o tom (TCD, MOZ, ZMB, ZWE,
CIV, LAO, KHM, TJK, BEN, TGO, BDI, LIE) — 73 linhas de `nations.csv` mudaram no
total. O relatório completo sai em `colors` no `_meta.json` do build e no stdout
do aplicador. Evidência visual: `docs/map_visual/colors_trad2_africa/`,
`colors_trad2_middle_east/` e `colors_trad2_se_asia/` (político, 1920×1080).

Regeneração: `tools/apply_traditional_colors.py` regrava **apenas** a coluna
`color` de `scenario/nations.csv` (+ `nations.json` e `provinces.json ->
nation_colors`); o pipeline completo (`tools/build_world_cities.py
--write-scenario`) usa a mesma função. `map/territory_map.gd` lê o CSV para
**todas** as nações, inclusive as 8 jogáveis; as texturas estáticas
`world_color.png`/`region_color.png` não são regeradas por este script (o mapa
dinâmico usa a textura de donos publicada em runtime).

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

## 13. Curadoria do mapa: ilhas removidas do Egeu (2026-11)

> **Substituída pela §15 (t59).** A caixa `REMOVED_ISLAND_REGIONS` descrita
> abaixo foi trocada pela regra global de área < 2.000 km². Esta seção fica
> como registro da rodada anterior: 61 dos 70 componentes continuam fora, 9
> mantêm terra (fusão com massa maior/costa) e Gökçeada e a ilha de Mármara,
> antes preservadas, saem. A lista canônica atual é `islet_removed.csv`.

`REMOVED_ISLAND_REGIONS` (em `tools/build_atlas_layers.py` e
`tools/generate_nav_mask.py`) zera todo componente de terra inteiro dentro da
caixa **23,35–30,05E / 35,25–41,05N** com área **≤ 3.000 km²**, exceto Creta,
Eubeia e as ilhas do mar de Mármara (Gökçeada/Imbros e Bozcaada/Tenedos do
Egeu saem; Marmara e Avşa ficam). O mesmo corte vale para o `nav_mask.png` e
para os rasters do atlas (`region_sdf`, `region_province`, `region_color` e
`world_*`). **70 componentes, 16.050 px** na grade alta da região (supersample
2: 8.192 px sobre 70°). O log do build (`[ilhas:region]`) não registra nomes:
a coluna "nome aproximado" foi atribuída pelo centroide; componentes de 1 px
ficam sem nome.

| Nome aproximado | lon | lat | Área (px) |
|---|---:|---:|---:|
| Lesbos | 26.244 | 39.171 | 2452 |
| Rodes | 27.951 | 36.186 | 2028 |
| Quios | 26.016 | 38.404 | 1295 |
| Lemnos | 25.226 | 39.920 | 777 |
| Samos | 26.815 | 37.735 | 727 |
| Tasos | 24.656 | 40.689 | 624 |
| Naxos | 25.488 | 37.062 | 623 |
| Andros | 24.852 | 37.862 | 584 |
| Karpathos | 27.158 | 35.600 | 476 |
| Imbros (Gökçeada) | 25.838 | 40.164 | 450 |
| Kos | 27.126 | 36.817 | 438 |
| Icária | 26.152 | 37.598 | 396 |
| Skyros | 24.553 | 38.868 | 333 |
| Tinos | 25.132 | 37.610 | 308 |
| Paros | 25.198 | 37.062 | 302 |
| Samotrácia | 25.583 | 40.458 | 295 |
| Milos | 24.439 | 36.707 | 263 |
| Amorgos | 25.906 | 36.849 | 195 |
| Kea | 24.331 | 37.616 | 192 |
| Kalymnos | 26.973 | 36.992 | 179 |
| Ios | 25.332 | 36.722 | 164 |
| Kythnos | 24.419 | 37.400 | 164 |
| Skopelos | 23.691 | 39.135 | 163 |
| Astypalaia | 26.352 | 36.573 | 157 |
| Mykonos | 25.377 | 37.458 | 142 |
| Sifnos | 24.706 | 36.978 | 141 |
| Santorini (Thira) | 25.435 | 36.400 | 136 |
| Syros | 24.918 | 37.434 | 134 |
| Aegina | 23.493 | 37.737 | 133 |
| Serifos | 24.481 | 37.160 | 119 |
| Symi | 27.832 | 36.587 | 118 |
| Kassos | 26.930 | 35.397 | 112 |
| Tilos | 27.365 | 36.428 | 110 |
| Leros | 26.833 | 37.149 | 100 |
| Alonnisos | 23.914 | 39.216 | 94 |
| Skiathos | 23.459 | 39.173 | 83 |
| Hydra | 23.476 | 37.338 | 80 |
| Psara | 25.578 | 38.576 | 76 |
| Tenedos (Bozcaada) | 26.040 | 39.824 | 74 |
| Agios Efstratios | 25.004 | 39.519 | 73 |
| Sikinos | 25.126 | 36.682 | 72 |
| Patmos | 26.559 | 37.331 | 67 |
| Kimolos | 24.560 | 36.814 | 66 |
| Nisyros | 27.167 | 36.587 | 59 |
| Anafi | 25.782 | 36.373 | 58 |
| Folegandros | 24.905 | 36.632 | 56 |
| Antiparos | 25.058 | 37.005 | 54 |
| Ilhéu ao sul de Rodes | 27.569 | 36.229 | 49 |
| Ilhéu a leste de Alonnisos | 24.067 | 39.329 | 43 |
| Gyaros | 24.712 | 37.611 | 35 |
| Ilhéu a oeste de Milos | 24.640 | 36.773 | 34 |
| Ilhéu a leste de Ios | 25.455 | 36.843 | 32 |
| Poros | 23.482 | 37.528 | 31 |
| Ilhéu a oeste de Amorgos | 25.649 | 36.891 | 29 |
| Ilhéu a noroeste de Amorgos | 25.812 | 37.113 | 24 |
| Ilhéu a sudeste do Peloponeso | 23.460 | 36.913 | 7 |
| Ilhéu a leste de Quios | 26.838 | 38.524 | 7 |
| Ilhéu a sudeste do Peloponeso | 23.883 | 36.838 | 5 |
| Ilhéu de 1 px | 28.094 | 36.055 | 1 |
| Ilhéu de 1 px | 28.880 | 36.670 | 1 |
| Ilhéu de 1 px | 28.299 | 36.721 | 1 |
| Ilhéu de 1 px | 25.735 | 36.798 | 1 |
| Ilhéu de 1 px | 25.838 | 37.140 | 1 |
| Ilhéu de 1 px | 25.240 | 37.149 | 1 |
| Ilhéu de 1 px | 26.949 | 37.790 | 1 |
| Ilhéu de 1 px | 26.453 | 38.422 | 1 |
| Ilhéu de 1 px | 26.812 | 38.542 | 1 |
| Ilhéu de 1 px | 23.360 | 39.960 | 1 |
| Ilhéu de 1 px | 23.830 | 40.011 | 1 |
| Ilhéu de 1 px | 25.453 | 40.037 | 1 |

Os ids **PRV002832** e **PRV002866** (as duas maiores ilhas do Egeu) também
saíram de `data/definitions/natural/province_areas.json`,
`data/definitions/provinces.txt`, `data/history/provinces/provinces.json` e
`data/history/territories/territories.json`. Nenhuma cidade, porto ou jazida
usava esses ids.

O ilhote **Shadwan** (33,98N 27,51E) continua removido por `REMOVED_ISLETS`
(camada mundial) e o `world_sdf`/`world_province` já o tratam como mar; a
captura política confirma água no ponto.

**Correção do preto:** o cache de importação do Godot pode continuar servindo
o atlas antigo (o jogo não reimporta ao rodar), e nele as ilhas ainda eram
terra com ids que já não existiam em `provinces.json`. O shader lia o texel 0
da textura de donos — nunca escrito — e pintava preto. Além de reimportar os
PNGs (`godot --headless --import`), `atlas_common.gdshaderinc::atlas_owner`
agora recusa id ≤ 0/fora da textura e os dois `dyn_block` marcam "sem dono";
o fragmento descarta a máscara de terra nesses pixels, então id/owner ausente
renderiza mar, nunca preto.

## 14. Curadoria de micro-nações e territórios (2026-11, t57)

Decisão aprovada pelo usuário e centralizada em `tools/world_nation_curation.py`
(fonte única lida por `build_world_cities.py`, `build_atlas_layers.py`,
`generate_nav_mask.py` e `build_province_nature.py`). O critério evita que uma
regeneração completa reintroduza uma nação removida ou perca uma fusão:

- **Fica** a nação soberana com `população ≥ 500.000` **ou** `área ≥ 10.000 km²`;
- **Ficam sempre** as exceções estratégicas `STRATEGIC_KEEP`: Singapura,
  Bahrein, Catar, Brunei, Luxemburgo, Malta, Chipre e Djibuti (abaixo do limiar);
- **Microestados europeus** não viram mar nem nação própria: o território e as
  províncias passam ao vizinho (anexação);
- **Dependentes grandes** (população/área acima do limiar, ou nomeados pelo
  usuário) são fundidos na nação-mãe;
- **Ilhas soberanas e dependentes minúsculos** são apagados: cidade, porto,
  construções e nação saem; no atlas e no `nav_mask.png` a terra vira mar;
- Quatro ilhas soberanas que passariam pelo critério por área/população
  (Vanuatu, Maldivas, Comores, Cabo Verde) saem por decisão explícita: ilha sem
  hinterlândia continental não paga o custo de mapa. Malvinas também saem por
  decisão explícita apesar dos 12.173 km².

`audit_source()` falha se uma nação abaixo do limiar não estiver classificada,
se um território não tiver destino, se uma tag não existir na fonte ou se uma
tag tiver dois destinos. Total: **60 tags** — 5 anexadas, 7 fundidas, 48 apagadas
(21 soberanas + 27 territórios).

### 14.1 Números antes/depois

| Item | Antes | Depois |
|---|---:|---:|
| Nações no cenário | 231 | **171** |
| Cidades no cenário | 858 | **794** |
| Portos no cenário | 138 | **137** |
| Construções iniciais | 858 | **794** |
| Chaves i18n | 1.458 | **1.273** |
| Províncias do atlas | 4.527 | **4.272** |
| Territórios em `history/territories` | 250 | **202** |

A revisão de balance da t57 mediu **fome em 166 das 171 nações** do cenário
reduzido (o teste headless de longo prazo continua verde: 365 dias, 0 violação
de conservação — §15).

### 14.2 Anexadas (5) — território e províncias passam ao vizinho

| Tag | Nação | Passa para | Pop. | Área km² |
|---|---|---|---:|---:|
| AND | Andorra | ESP | 82.904 | 468 |
| LIE | Liechtenstein | CHE | 41.024 | 160 |
| MCO | Mônaco | FRA | 38.341 | 1 |
| SMR | San Marino | ITA | 34.109 | 61 |
| VAT | Vaticano | ITA | 921 | 0 |

### 14.3 Fundidas (7) — províncias, cidades e portos passam à nação-mãe

| Tag | Território | Passa para | Pop. | Área km² |
|---|---|---|---:|---:|
| GRL | Groenlândia | DNK | 56.831 | 2.166.086 |
| GUM | Guam | USA | 168.999 | 549 |
| HKG | Hong Kong | CHN | 7.498.900 | 1.092 |
| MAC | Macau | CHN | 685.900 | 33 |
| NCL | Nova Caledônia | FRA | 295.333 | 19.060 |
| PRI | Porto Rico | USA | 3.184.835 | 9.104 |
| PYF | Polinésia Francesa | FRA | 282.465 | 4.167 |

### 14.4 Apagadas (48) — a terra vira mar

**Soberanas (21):** São Cristóvão e Névis (KNA), Antígua e Barbuda (ATG),
Dominica (DMA), Santa Lúcia (LCA), São Vicente e Granadinas (VCT), Granada
(GRD), Barbados (BRB), Tuvalu (TUV), Nauru (NRU), Kiribati (KIR), Ilhas
Marshall (MHL), Palau (PLW), Micronésia (FSM), Tonga (TON), Samoa (WSM), São
Tomé e Príncipe (STP), Seicheles (SYC), Vanuatu (VUT), Maldivas (MDV), Comores
(COM), Cabo Verde (CPV).

**Territórios (27):** Gibraltar (GIB), Bermudas (BMU), Malvinas (FLK), Ilhas
Virgens Americanas (VIR), Ilhas Virgens Britânicas (VGB), Ilhas Faroé (FRO),
Samoa Americana (ASM), Marianas do Norte (MNP), Curaçao (CUW), Aruba (ABW),
Sint Maarten (SXM), Cayman (CYM), Turcas e Caicos (TCA), Anguilla (AIA),
Montserrat (MSR), Santa Helena (SHN), Ilhas Cook (COK), Niue (NIU), Wallis e
Futuna (WLF), São Pedro e Miquelon (SPM), São Martinho FR (MAF), São Bartolomeu
(BLM), Norfolk (NFK), Ilha de Man (IMN), Jersey (JEY), Guernsey (GGY), Åland
(ALD).

**Exceções explícitas (acima do limiar, saem por decisão do usuário):**
Vanuatu (335.169 hab., 12.200 km²), Maldivas (529.676, 300 km²), Comores
(882.847, 2.170 km²), Cabo Verde (527.326, 4.033 km²) e Malvinas (2.638,
12.173 km²).

### 14.5 Rótulo "VILA" no Pacífico (Port Vila / Vanuatu)

A captura `docs/map_visual/micro_cleanup/pacific/` tem `tag: before` no
manifest, mas foi tirada às 12:33 — depois do rebuild do atlas (12:27) — e já
não mostra a ilha. A origem do rótulo era a cidade `VUT-POR` (Port-Vila,
168,31E / 17,73S), que existia em `data/definitions/scenario/cities.json` no
HEAD e era produzida por `build_world_cities.py` a partir da linha `VUT-POR` de
`data/definitions/world/cities_source.csv`; o nome era desenhado por
`map/city_world.gd` (`_build_cities` → `Label3D`) e a ilha vinha do atlas
(no HEAD, `world_sdf` = 218 no ponto).

Com `VUT` na lista `REMOVED_SOVEREIGNS`, a cidade sai do cenário (0 ocorrências
de `VUT-POR`), a ilha sai do atlas (`world_sdf` = 0 no ponto) e o
`nav_mask.png` passa a tratar a coordenada como água (pixel 0). A recaptura
`docs/map_visual/micro_cleanup/pacific_after/` (tag `after`, mesmo foco
`FJI-SUV`, 1920×1080) não mostra a ilha nem o rótulo em nenhum zoom
(d013/d026/d052/d120); Fiji/Suva, Honiara, Nouméa e Port Moresby continuam.

### 14.6 Sobras e poda reprodutível

Os arquivos de **fonte** (`nations_source.csv`, `cities_source.csv`,
`ports_source.csv`) e os **relatórios** (`world/_coverage_before_after.csv`,
`world/_import_report.csv`, `world/out/_meta.json`) mantêm as 60 tags por
desenho: a curadoria exige que a tag exista na fonte e registra o destino.
Cidades anexadas/fundidas preservam o id de origem (`GRL-*`, `MCO-*`, `SMR-*`…)
com `nation`/`home_nation` já apontando para a mãe; as chaves i18n dessas
cidades continuam válidas. `data/definitions/natural/country_names.json` é um
dicionário de nomes da fonte (não um roster) e por isso também mantém as tags.

Ficavam defasados três inputs derivados do atlas, capazes de reintroduzir as
ilhas removidas numa regeneração de `build_natural_geography.py`:

```powershell
python tools/reconstruct_natural_inputs.py --prune
# provinces.txt: -255 (4.272) | provinces.json: -255 (4.272)
# territories.json: -48 territórios vazios (202)
```

A poda também reescreve `province_owner`/`province_controller` das províncias
que mudaram de dono (anexações e fusões), então `data/history/provinces/
provinces.json` passa a ter os mesmos 191 donos do atlas. Verificação: nenhuma
das 60 tags aparece como dono em `data/` ou `map/` fora de fonte/relatório.

### 14.7 Como reproduzir

```powershell
python tools/build_world_cities.py --check     # 171 / 794 / 137, 0 avisos
python tools/build_world_cities.py             # world/out + scenario/
python tools/build_atlas_layers.py             # atlas/*.png, provinces.json, removed_nations.png
python tools/generate_nav_mask.py              # nav_mask.png (terra removida vira água)
python tools/build_province_nature.py          # province_areas/nature e depósitos
python tools/reconstruct_natural_inputs.py --prune
```

## 15. Curadoria global de ilhas: limiar de 2.000 km² (2026-11, t59)

**Regra aprovada pelo usuário.** Todo componente de terra **isolado** (8-vizinhos)
com área **< 2.000 km²** vira mar no atlas e no `nav_mask.png`. A decisão é tomada
**uma vez**, na camada mundial (`WORLD_W = 8192`, supersample 2 → 16.384×8.192,
~2,4 km/texel em latitude), e a camada da região recebe a mesma máscara
(`apply_removed_mask`), para o mapa regional e o global nunca divergirem.
Substitui a caixa do Egeu da §13 (ver nota no fim desta seção).

Implementação: `remove_small_islands()` em `tools/build_atlas_layers.py`;
`ISLAND_MAX_KM2 = 2000.0`. Constantes vizinhas:

| Constante | Valor | Papel |
|---|---:|---|
| `ISLAND_MAX_KM2` | 2.000 | limiar de área real (km²) |
| `COASTAL_KEEP_KM` | 3,0 | ilha a menos disso de massa maior fica (costa colada) |
| `ISLAND_KEEP_SEEDS` | `[]` | exceções manuais (nome, lon, lat) |
| `KEEP_ENTITY_RADIUS` | city 2 / port 2 / deposit 1 texel | tolerância do seed da entidade |

O que **fica**:

1. ilha com **cidade, porto/capital ou jazida** do cenário (`load_keep_points()` lê
   `data/definitions/scenario/cities.json`, `ports.json` e
   `data/definitions/natural/deposits.json` a cada build — cidade nova numa ilha
   pequena a mantém automaticamente);
2. ilha a **≤ 3 km** de uma massa de terra maior (proteção do litoral: Eubeia,
   Samos, ilhas costeiras que abririam buraco na costa);
3. componente que **encosta na borda** da camada (continente cortado pelo raster);
4. seeds explícitos de `ISLAND_KEEP_SEEDS`.

O que **sai**: todo o resto abaixo de 2.000 km² — inclusive ilhas com população
real, desde que não tenham cidade/porto/jazida no cenário (ex.: Lesbos, Simeulue,
Fuerteventura, Natuna Besar; ver lista completa em `docs/plans/islet_removed.csv`).

### 15.1 Números do build aplicado (t59)

- **1.632 componentes**, **350.020 km²** somados (0 componentes ≥ 2.000 km²).
- 120.883 px na grade mundial do build (16.384×8.192).
- 191 componentes ≥ 500 km² concentram 188.503 km² (54%); 753 ≥ 100 km²
  concentram 311.892 km² (89%); mediana 83,4 km²; menor 2,0 km²;
  maior 1.979,7 km² (Simeulue).
- Splits gravados: `map/map_data/atlas/removed_islands.png` (1 = terra virou mar,
  8.192×4.096), lido por `tools/generate_nav_mask.py`.

| Faixa (`ISLAND_REGIONS`) | Removidos | km² | mantidos: entidade | costa | borda/seed |
|---|---:|---:|---:|---:|---:|
| Pacífico | 568 | 90.646 | 20 | 63 | 5 |
| Outros (fora das faixas) | 283 | 81.221 | 5 | 80 | 0 |
| Sudeste Asiático | 259 | 60.817 | 14 | 14 | 0 |
| Ártico | 206 | 58.965 | 3 | 64 | 0 |
| Caribe | 99 | 19.440 | 5 | 6 | 0 |
| Mediterrâneo | 86 | 18.351 | 2 | 11 | 0 |
| Índico | 75 | 10.619 | 4 | 5 | 0 |
| Norte da Europa | 56 | 9.962 | 0 | 14 | 0 |
| **Total** | **1.632** | **350.020** | **53** | **257** | **5** |

"Outros" e "Mediterrâneo" incluem as ilhas do Egeu da §13, agora decididas pela
regra global (Creta e Eubeia ficam por área; Gökçeada e a ilha de Mármara saem
por não ter entidade; Bozcaada/Tenedos funde-se à Anatólia no raster mundial).
Shadwan (Golfo de Suez) **não** entra nesta conta: já era removido antes, por
`REMOVED_ISLETS`, e o build real o apaga antes da regra de área.

### 15.2 Os 15 maiores removidos

Nome aproximado atribuído pelo centroide/bbox + gazetteer (GeoNames por país:
`T.ISL/T.ISLS/T.ATOL`; casamento pelo ponto mais próximo do centroide dentro do
bbox do componente, com desempate por nome de ilha × rochedo/ponta). A área é a
medida no raster (ver 15.4). Lista completa, ordenada por área decrescente, em
`docs/plans/islet_removed.csv` (1.632 linhas; colunas `rank`,
`nome_aproximado`, `confianca`, `origem_nome`, `regiao`, `pais`, `tag`, `lon`,
`lat`, `area_km2`, `pixels`, bbox, `cidade_proxima`, `cidade_km`, `gn_nome`,
`gn_km`). Só os 15 abaixo foram conferidos um a um (bbox + Wikidata/artigo:
`confianca=alta`); os demais são **aproximados** — `media` = o ponto do
gazetteer caiu dentro do bbox do componente, `baixa` = veio de vizinho/cidade
ou não houve casamento. Nome repetido em componentes diferentes só ocorre em
8 casos (fragmentos do mesmo arquipélago).

| # | Nome aproximado | País | lon | lat | km² |
|---:|---|---|---:|---:|---:|
| 1 | Simeulue | IDN | 96,066 | 2,645 | 1.979,7 |
| 2 | Lesbos | GRC | 26,249 | 39,171 | 1.915,9 |
| 3 | Bolshoy Begichev | RUS | 112,516 | 74,333 | 1.910,2 |
| 4 | Charcot | ATA | −75,109 | −69,959 | 1.904,5 |
| 5 | Natuna Besar (Bunguran) | IDN | 108,210 | 3,933 | 1.899,8 |
| 6 | Ilhas Pagai (Norte e Sul) | IDN | 100,233 | −2,888 | 1.889,9 |
| 7 | Mainland (Shetland) | GBR | −1,237 | 60,380 | 1.873,5 |
| 8 | Vidal Gormaz | CHL | −74,790 | −52,030 | 1.873,1 |
| 9 | Kunashir | RUS | 145,959 | 44,167 | 1.862,6 |
| 10 | Fuerteventura | ESP | −14,043 | 28,403 | 1.822,0 |
| 11 | Great Inagua | BHS | −73,349 | 21,080 | 1.810,2 |
| 12 | Air Force | CAN | −74,087 | 67,925 | 1.803,7 |
| 13 | Guadalupe | FRA | −61,561 | 16,238 | 1.788,2 |
| 14 | Graham Bell | RUS | 64,150 | 80,918 | 1.731,8 |
| 15 | Catanduanes | PHL | 124,231 | 13,789 | 1.727,6 |

O componente é o que foi removido, não a ilha "oficial": em arquipélagos densos
o raster funde ilhas vizinhas num só componente (Pagai do Norte + do Sul;
Mainland + Yell/Unst nas Shetlands), e o nome é o da ilha principal.

### 15.3 Como manter uma ilha (exceção manual)

Duas formas, ambas **sem** mexer no código da regra:

1. **Entidade no cenário (preferida):** registre cidade, porto ou jazida na ilha
   (`cities.json`/`ports.json` em x/y do plano 5000×2500, ou `deposits.json` em
   lon/lat). O build mantém a ilha automaticamente na próxima execução.
2. **Seed explícito:** acrescente `("nome", lon, lat)` em `ISLAND_KEEP_SEEDS`
   (`tools/build_atlas_layers.py`):

```python
ISLAND_KEEP_SEEDS = [
    ("Samos", 26.815, 37.735),   # já fica pela costa; exemplo de formato
    ("MinhaIlha", 12.345, -3.210),
]
```

O seed usa o componente de terra mais próximo em até 3 texels (o centroide de
uma ilha pequena pode cair na água). Seed fora da terra gera aviso
`[ilhas:world] AVISO: seed ... caiu fora da terra` e não protege nada.

### 15.4 Sanidade da área (latitudes altas)

A área **não** é contada em pixels: `remove_small_islands()` soma
`R²·dlon·dlat·cos(lat)` linha a linha na projeção equirretangular
(`lat` do centro de cada linha). Sem o `cos(lat)`, um componente do Ártico a 80°N
sairia ~5,8× maior. Verificação do build: o teto de área usado na triagem
(`min_px_km2` com a latitude mais extrema do recorte) nunca deixa um componente
passar como "pequeno" por engano em latitude alta.

Limite conhecido: o raster mundial (~4,9 km/texel, supersample 2,4 km) **infla**
ilhas pequenas em ~5–25% (1 texel de costa a mais em cada borda). Comparações
com a área real: Simeulue 1.979 vs 1.754 (+13%), Lesbos 1.916 vs 1.633 (+17%),
Fuerteventura 1.822 vs 1.660 (+10%), Catanduanes 1.728 vs 1.482 (+17%). O erro
é para cima, então a regra é **conservadora**: nenhuma ilha acima de 2.000 km²
reais foi removida; na prática o corte real fica em ~1.600–1.900 km². A soma de
350.020 km² é a soma dessas áreas medidas (não de áreas oficiais).

### 15.5 Como reproduzir e conferir

```powershell
# relatório seco da regra: não escreve nada, lista removidos e mantidos
python tools/build_atlas_layers.py --report-islands

# build completo (atlas + removed_islands.png) e máscara de navegação
python tools/build_atlas_layers.py
python tools/generate_nav_mask.py

# o jogo usa o cache .godot/imported/: reimportar antes de capturar
& $godot --headless --path . --import
& $godot --path . res://tools/map_capture.tscn -- --set=map3d --mode=political --tag=after --focus=GRC-ATH
```

Conferências do build aplicado: `--report-islands` imprime
`removidos 1632 componentes ... ~350020 km²`, a tabela por faixa de 15.1 e a
lista dos 60 maiores; nenhum `mantém` pode ser de ilha com cidade/porto/jazida
(o build aborta com `ERRO: ... ilha(s) com cidade/porto/jazida caíram na
remoção`). Capturas: `docs/map_visual/islet_cleanup/{aegean_med,caribbean,
indonesia_philippines,scandinavia_baltic}/` (d013/d026/d052/d120).

### 15.6 Relação com a §13 (caixa do Egeu)

A `REMOVED_ISLAND_REGIONS` (caixa 23,35–30,05E / 35,25–41,05N, ≤ 3.000 km²) foi
**substituída** pela regra global desta seção em `build_atlas_layers.py` e
`generate_nav_mask.py`. O resultado no Egeu **não** é idêntico: 61 dos 70
componentes da §13 continuam removidos; 9 mantêm terra porque, no raster mundial
de ~4,9 km/texel, fundem-se a uma massa maior (Samos com a Anatólia, Hidra com o
Peloponeso) ou ficam pela proteção de costa, e 5 dos "ilhéus de 1 px" daquela
rodada nem formam componente próprio na camada mundial. Creta e Eubeia continuam
(área). Gökçeada e a ilha de Mármara, que a caixa mantinha por exceção, agora
**saem** (menos de 2.000 km², sem entidade). Os números da §13 descrevem a
rodada anterior; a lista canônica atual é `docs/plans/islet_removed.csv`.

## 16. Curadoria r2 — Caribe, Malvinas, Ucrânia/Crimeia, ilhas remotas (2026-11, t63)

Rodada de curadoria sobre o roster mundial (`tools/world_nation_curation.py` +
`tools/world_city_curation.py`). O que está **no disco** (e ainda **não
commitado**): **177 nações, 799 cidades e 142 portos**
(`data/definitions/scenario/{nations,cities,ports}.json`). A validação de dados
e a economia seguem verdes; esta seção registra o que foi conferido no jogo.

### 16.1 Microestados fundidos (t57)

`MICROSTATES` em `tools/world_nation_curation.py` absorve o território e as
províncias no vizinho e **remove a nação do roster**: `VAT → ITA`, `SMR → ITA`,
`MCO → FRA`, `LIE → CHE`. As cidades continuam existindo, agora com
`nation`/`home_nation` do receptor (`VAT-VATI` e `SMR-SANM` → ITA, `MCO-MONA`
→ FRA, `LIE-VADU` → CHE). Consequência direta: **não existe mais nação jogável
sem província**, então `tools/territory_test.gd` deixou de ter exceção
(`NO_PROVINCE_NATIONS` vazia) e passa a exigir província para as 177.

### 16.2 Conferência de cidades e donos (in-game, 2026-11-12)

Fonte: `cities.json` + `TerritoryMap` construído pela cena principal em
headless (`owners()`, `provinces()`, `city_for_province()`). "Prov. dono" é a
nação dona das províncias atribuídas à cidade; "nº" é quantas províncias a
cidade administra.

| cidade | id | dono | prov. dono | nº | observação |
|---|---|---|---|---|---|
| Brest (Belarus) | `BLR-BRE` | BLR | BLR | 2 | — |
| Białystok | `POL-BIA` | POL | POL | 2 | — |
| Gdańsk | `POL-GDA` | POL | POL | 2 | porto `POL-GDA` (port_of `POL-GDA`) |
| Kaliningrado | `RUS-KGD` | RUS | RUS | 1 | porto `RUS-KGD`; enclave |
| Lviv | `UKR-LVI` | UKR | UKR | 8 | — |
| Sevastopol (Crimeia) | `RUS-SEV` | RUS | RUS | 2 | porto `RUS-SEV` |
| Kherson | `UKR-KHE` | UKR | UKR | 3 | frente |
| Zaporizhzhia | `UKR-ZAP` | UKR | UKR | 1 | frente |
| Donetsk | `RUS-DON` | RUS | **UKR** | **0** | **pendência da r2** (resolvida na r4): cidade RUS na província 926 (UKR), atribuída a `UKR-KHA` |
| Mariupol | `RUS-MAR` | RUS | **UKR** | **0** | **pendência da r2** (resolvida na r4): mesma província 926 |
| Cluj-Napoca | `ROU-CLJ` | ROU | ROU | 17 | — |
| Oradea | — | — | — | — | **ausente** do roster (ROU tem Bucareste, Cluj-Napoca, Iaşi) |
| Kaohsiung | `TWN-KAO` | TWN | TWN | 4 | — |
| Palma | `ESP-PMI` | ESP | ESP | 1 | — |
| Ajaccio | `FRA-AJA` | FRA | FRA | 2 | — |
| Cagliari | `ITA-CAG` | ITA | ITA | 8 | — |
| Bari (sul da Itália) | `ITA-BAR` | ITA | ITA | 12 | — |
| Stanley (Malvinas) | `FLK-STA` | GBR | GBR | 1 | — |
| Nuuk | `GRL-NUU` | DNK | DNK | 1 | — |
| Ilulissat | `GRL-ILU` | DNK | DNK | 5 | — |
| Valletta | `MLT-VAL` | MLT | MLT | 55 | única cidade de MLT; **sem Victoria** (Gozo) |
| Ciudad de la Paz | `GNQ-CIU` | GNQ | GNQ | 3 | capital GNQ no continente (Bata também no continente) |

Notas de leitura:

- **Toda cidade do roster tem `nation == home_nation`** — não há ocupação
  modelada por `home_nation` hoje; o único descompasso é geográfico (Donetsk e
  Mariupol estão dentro de província UKR).
- As 55 províncias de Malta são micro-polígonos admin-1 do Natural Earth sobre
  o arquipélago, não território no exterior.
- Portos legados ligados a ex-microestados: `ITA-ROM`/`ITA-ROME`
  (`port_of VAT-VATI`) e `ITA-PAD` (`port_of SMR-SANM`). A referência é válida
  (as cidades agora são ITA), mas o porto fica preso à cidade do antigo
  microestado — registrado como dívida cosmética, não como dado quebrado.

### 16.3 Capturas (1920×1080, político)

`docs/map_visual/curation_r2/{caribbean,ukraine_poland,indian_ocean_west,mediterranean_west}/`
(`map3d_political_d013/d026/d052/d120.png` + manifesto), focos `CUB-HAV`,
`UKR-LVI`, `MDG-ANT` e `ITA-CAG`. Sem manchas pretas: a varredura dos 16 PNGs
encontra no máximo **135 px quase-pretos em bloco** (0,007% de 1920×1080),
contra as manchas de milhares de pixels das rodadas anteriores — o shader trata
id/dono ausente como mar (GAME_STATE §4).

### 16.4 Como reproduzir

```powershell
& $godot --headless --path . --script res://tools/territory_test.gd
& $godot --path . res://tools/map_capture.tscn -- --set=map3d --mode=political --tag=r2 --focus=CUB-HAV --run=curation_r2/caribbean --width=1920 --height=1080
# idem com --focus=UKR-LVI / MDG-ANT / ITA-CAG e --run=curation_r2/{ukraine_poland,indian_ocean_west,mediterranean_west}
```

## 17. Curadoria r3 — Okinawa, Miyako, Guam, Oradea, Ruse e portos da Itália (2026-11-12, t64)

Rodada sobre os mesmos mecanismos da r2 (`tools/world_nation_curation.py`,
`tools/world_city_curation.py`, `ISLAND_KEEP_SEEDS` em
`tools/build_atlas_layers.py`). O roster do cenário fica com **177 nações, 802
cidades e 144 portos** (`data/definitions/scenario/*.json`); o atlas tem
**4.223 províncias** (+1: Guam voltou como província dos EUA) e o
`nav_mask.png` ganhou **48 células de terra** (11 cheias + 37 de costa) sem
perder nenhuma.

### 17.1 Ilhas devolvidas

As três entram no atlas **e** no `nav_mask` por `ISLAND_KEEP_SEEDS` (mesma
decisão nas duas ferramentas) + cidade/porto no cenário. Okinawa (1.206 km²) e
Guam (549 km²) ficam abaixo do limiar de 2.000 km² e sem o seed seriam
removidas; Miyakojima (159 km²) já estava na lista de seeds da r2 por ter a
cidade `JPN-MIY` e continua.

| ilha | cidade | id | pop. (fonte) | porto | `port_of` |
|---|---|---|---:|---|---|
| Okinawa | Naha | `JPN-NAH` | 317.625 (censo 2020, Statistics Bureau of Japan) | Naha, costa oeste (26,2167N 127,6667E) | `JPN-NAH` |
| Miyakojima | Miyakojima | `JPN-MIY` | 54.908 (já no roster; GeoNames 2026) | — | — |
| Guam | Hagåtña | `GUM-HAG` | 943 (2020 Census of Guam, CDP) | Apra Harbor (13,443N 144,652E) | `GUM-HAG` |

Guam volta a ser **território dos EUA**: `GUM` saiu de `REMOVED_TERRITORIES`
e entrou em `MERGED_TERRITORIES` (`GUM → USA`), o mesmo destino da r1. A
província admin-1 de Guam (id 3074) reaparece com `nation = USA`; a população
da cidade é a do CDP de Hagåtña (943), não a da ilha (153.836), porque o modelo
de cidade é o núcleo urbano.

### 17.2 Cidades novas

| cidade | id | nação | pop. (fonte) | coordenadas | nota |
|---|---|---|---:|---|---|
| Oradea | `ROU-ORA` | ROU | 183.105 (INS, censo 2021) | 47,0722N 21,9211E | capital de Bihor, ~10 km da fronteira húngara |
| Ruse | `BGR-RUS` | BGR | 123.134 (NSI, censo 2021) | 43,8486N 25,9540E | maior cidade búlgara do Danúbio, ~65 km de Bucareste |

As duas encostam em fronteira (Oradea com a Hungria; Ruse com a Romênia, cujo
limite é o próprio Danúbio). São **decisão explícita do usuário**, registrada no
`note` de cada entrada; a regra de fronteira de 40 km vale para a seleção
automática, não para `FORCED_CITIES`.

### 17.3 Microestados: cidade duplicada só sai se estiver colada

Regra aplicada: cidade de microestado anexado continua no cenário como cidade do
anexador, **exceto** se estiver a < 60 km de outra cidade (mesmo núcleo urbano).
Distâncias medidas no plano do jogo:

| cidade | id | nação | vizinha mais próxima | km | decisão |
|---|---|---|---|---:|---|
| Cidade do Vaticano | `VAT-VATI` | ITA | Roma (`ITA-ROM`) | 5,0 | **removida** |
| Mônaco | `MCO-MONA` | FRA | Gênova (`ITA-GEN`) | 141,9 | mantida |
| Andorra-a-Velha | `AND-AND` | ESP | Toulouse (`FRA-TLS`) | 121,1 | mantida |
| San Marino | `SMR-SANM` | ITA | Roma (`ITA-ROM`) | 226,2 | mantida |
| Vaduz | `LIE-VADU` | CHE | Zürich (`CHE-ZUR`) | 77,2 | mantida (não citada no pedido, mesmo critério) |

A saída do Vaticano é a **única exceção ao contrato legado** (as 40 cidades da
fase 1): `LEGACY_EXCLUDED` em `world_city_curation.py` declara a exceção e
`build_world_cities.py` passa a exigir `40 - len(LEGACY_EXCLUDED)` = 39.

### 17.4 Portos dos ex-microestados

| porto | antes (`port_of`) | depois | motivo |
|---|---|---|---|
| `ITA-ROM` | `VAT-VATI` | `ITA-ROM` | porto de Roma (Ostia/Tibre) |
| `ITA-ROME` | `VAT-VATI` | `ITA-ROM` | segunda entrada de Roma; aponta para a mesma cidade |
| `ITA-PAD` | `SMR-SANM` | `ITA-MIL` | Pádua/Veneza não estão no roster; o porto fica no litoral de Veneza e o território (Veneto) é administrado pela cidade italiana mais próxima, Milão — a mesma regra de província do jogo |

O mapa fica em `PORT_OF_OVERRIDES` (ids da **fonte**; a promoção traduz para o
id final com o mesmo `plan_city_ids` das cidades). O build valida que o alvo
existe e é da mesma nação; `assign_port_of` só aceita o override nesse caso.

### 17.5 Donetsk/Mariupol (RUS em província UKR) — mantido, documentado

Decisão do usuário nesta rodada: **não mexer no território**; o ajuste da frente
é feito depois. `RUS-DON` e `RUS-MAR` continuam com `nation = RUS` dentro da
província 926 (dona **UKR**, atribuída a `UKR-KHA`) e seguem sem província
própria — a pendência permanece declarada em `GAME_STATE.md` §15 e na §16.2.

> **Resolvido na r4 (§18):** a UA-14 foi dividida na frente aproximada; a parte
> ocupada virou a província 4597 (RUS), administrada por `RUS-DON`.

### 17.6 Como reproduzir

```powershell
# pipeline completo (na ordem)
python tools/build_atlas_layers.py
python tools/generate_nav_mask.py
python tools/reconstruct_natural_inputs.py --prune
python tools/compute_province_areas.py
python tools/compute_territory_areas.py
python tools/build_province_nature.py
python tools/build_resource_map.py
python tools/build_world_cities.py --write-scenario
python tools/apply_traditional_colors.py --refresh-adjacency
python tools/build_scenario_data.py
python tools/validate_scenario_data.py
& $godot --headless --path . --import

# testes e capturas
& $godot --headless --path . --script res://tools/territory_test.gd
& $godot --headless --path . --script res://tools/nav_strait_test.gd
& $godot --headless --path . --script res://tools/economy_integration_test.gd
& $godot --headless --path . --script res://tools/map_frame_test.gd
& $godot --path . res://tools/map_capture.tscn -- --set=map3d --mode=political --tag=r3 --focus=JPN-NAH --run=curation_r3/okinawa_taiwan --width=1920 --height=1080
# idem com --focus=ROU-BUC e --run=curation_r3/romenia_bulgaria
```

Conferência rápida do atlas/nav (pixel): Naha e Hagåtña com `world_sdf.r > 128`
(terra), `removed_islands.png = 0` e `removed_nations.png = 0`; `nav_mask.png`
com célula de terra (128/255) nos três seeds. Nenhuma célula de terra do
`nav_mask` muda para água nesta rodada — só as 48 devolvidas.

## 18. Curadoria r4 — território ocupado pela Rússia na Ucrânia (2026) e porto único de Roma (2026-11-12)

Rodada de **dados**: o mapa político passa a mostrar como **RUS** o território
ocupado pela Rússia na Ucrânia na situação ~2026, e o porto duplicado de Roma é
fundido em um só. O roster fica com **177 nações, 802 cidades e 143 portos**
(uma linha a menos em `ports.csv`/`ports.json`); o atlas passa a **4.226
províncias** (+3 ids sintéticos para a parte ocupada das províncias divididas).

### 18.1 Fonte e precisão (declaradas)

A linha de frente é **aproximada** a partir dos mapas públicos do **ISW**
(`Russia's Offensive Campaign Assessment` / "Ukraine Control Map") e do
**DeepStateMAP**, na situação observada em **2025–2026**. A geometria é um
polígono em graus (≈ 10 km de resolução de desenho) e o raster do atlas tem
~4,9 km/texel: o resultado é uma **carta aproximada**, não uma fronteira
reconhecida nem um levantamento topográfico. A decisão e o polígono vivem em
`tools/world_nation_curation.py` (`OCCUPIED_REGIONS` + `OCCUPIED_FRONT`) —
mudou a fonte, edita-se o polígono e roda-se o pipeline; **nada é editado à mão
em JSON**.

### 18.2 Mecanismo (reprodutível)

`tools/build_atlas_layers.py` lê `OCCUPIED_REGIONS` (por código **ISO 3166-2**
da província admin-1 do Natural Earth, ex.: `UA-14`) e `OCCUPIED_FRONT`:

* `whole_provinces` — a província inteira muda de dono (Luhansk);
* `front_provinces` — só os pixels dentro do polígono mudam de dono **e**
  recebem um **id sintético** (alocado depois do último admin-1 da fonte, na
  ordem das províncias → determinístico). O id original continua com a Ucrânia,
  então nenhuma referência antiga (jazida, natureza, save) muda de significado;
* `paint_occupation` trabalha só na janela da bbox de cada província (memória
  não cresce com o mundo), repinta `owners` (cor estática e fronteira do canal
  G do SDF) e relabela `provinces` (canal B + raster de id usado pelo shader).

O dono no jogo não é lido do arquivo de cor: `TerritoryMap` publica o dono por
id de província em `province_owner` e o shader usa esse valor quando
`dynamic_territory` está ligado; a atribuição província→cidade vem de
`provinces.json` (`nation`), então catchment e jazidas acompanham
automaticamente (`GeographyService.nation_tag_at`).

### 18.3 O que mudou (províncias)

Áreas da tabela são a estimativa do projeto (`tools/compute_province_areas.py`,
Voronoi dos centróides sobre o `nav_mask`; `data_quality = estimate_display`),
não a área oficial do oblast.

| província | id | ISO | antes | depois | centroide | área km² (estimativa) |
|---|---|---:|---|---|---|---:|
| Crimeia | 1640 | UA-43 | RUS | RUS | 34,39E 45,31N | 17.288 |
| Sevastopol | 2273 | UA-40 | RUS | RUS | 33,63E 44,58N | 7.429 |
| Luhansk (inteira) | 923 | UA-09 | UKR | **RUS** | 39,01E 48,97N | 38.377 |
| Donetsk ocupada | **4597** | UA-14 | — | **RUS** | 37,84E 47,79N | 26.545 |
| Donetsk ucraniana (Kramatorsk/Pokrovsk) | 926 | UA-14 | UKR | UKR | 37,38E 48,60N | 17.769 |
| Zaporizhzhia ocupada (sul) | **4599** | UA-23 | — | **RUS** | 35,78E 47,09N | 18.609 |
| Zaporizhzhia ucraniana (cidade) | 2272 | UA-23 | UKR | UKR | 35,47E 47,79N | 13.237 |
| Kherson margem esquerda | **4598** | UA-65 | — | **RUS** | 33,77E 46,45N | 17.326 |
| Kherson margem direita (cidade) | 1641 | UA-65 | UKR | UKR | 33,28E 47,03N | 20.025 |

Crimeia/Sevastopol não precisaram de mudança: o admin-0 do Natural Earth já as
traz como RUS (era a origem da pendência da r2). **Kharkiv (922), Dnipro
(3224), Kyiv (4366), Mykolaiv (2274) e as cidades de Zaporizhzhia/Kherson
continuam UKR.**

### 18.4 Polígono da frente (`OCCUPIED_FRONT`)

Pontos (lon, lat): `31.60,46.38 → 32.20,46.47 → 32.60,46.53 → 33.20,46.72 →
33.70,46.90 → 34.30,47.20 → 34.60,47.56 → 35.10,47.58 → 35.70,47.55 →
36.40,47.70 → 36.90,47.85 → 37.20,48.15 → 37.90,48.60 → 38.10,48.80 →
38.90,49.05`, fechando pela caixa externa (`41.20,49.60 → 41.20,43.80 →
31.60,43.80`). Só pixels das províncias de `front_provinces` são repintados,
então a caixa externa não afeta outro país.

Conferência pixel a pixel no `world_province.png` (nations após o build):

| referência | coordenada | id | nação |
|---|---|---:|---|
| Donetsk | 37,80E 48,02N | 4597 | RUS |
| Mariupol | 37,54E 47,10N | 4597 | RUS |
| Kramatorsk | 37,53E 48,72N | 926 | UKR |
| Pokrovsk | 37,18E 48,29N | 926 | UKR |
| Zaporizhzhia | 35,14E 47,84N | 2272 | UKR |
| Melitopol | 35,37E 46,84N | 4599 | RUS |
| Berdiansk | 36,79E 46,76N | 4599 | RUS |
| Kherson | 32,62E 46,64N | 1641 | UKR |
| Nova Kakhovka | 33,37E 46,75N | 4598 | RUS |
| Enerhodar | 34,65E 47,51N | 4599 | RUS |
| Kharkiv | 36,23E 49,99N | 922 | UKR |
| Dnipro | 35,05E 48,46N | 3224 | UKR |
| Luhansk | 39,31E 48,57N | 923 | RUS |
| Kyiv | 30,52E 50,45N | 4366 | UKR |
| Sevastopol | 33,52E 44,62N | 2273 | RUS |

### 18.5 Catchment e jazidas

A bacia de captação e a posse mineral são derivadas da **província** no
runtime. Resultado (mesma regra A1 de `GeographyService`):

| província | cidade que administra |
|---|---|
| Luhansk (923) e Donetsk ocupada (4597) | `RUS-DON` |
| Zaporizhzhia ocupada (4599) | `RUS-MAR` |
| Kherson margem esquerda (4598) | `RUS-SEV` |
| Donetsk ucraniana (926) | `UKR-KHA` (cidade UKR mais próxima) |
| Zaporizhzhia ucraniana (2272) | `UKR-ZAP` |
| Kherson margem direita (1641) | `UKR-KHE` |

Jazidas: o campo legado `owner` de `deposits.json` reflete o dono do território
natural e mudou em 11 registros (Donbas/Azov → RUS); o **runtime não lê esse
campo** — `assign_deposits` usa `nation_tag_at(px, py)`, a província do atlas.
Efeito prático: carvão/ferro de Donbas e Azov passam para RUS-DON/RUS-MAR.

### 18.6 Porto duplicado de Roma

`ITA-ROME` (ponto "snap na costa" de 2026, ~2 km da praia de Ostia) era o mesmo
porto de `ITA-ROM` (foz do Tibre/Fiumicino-Ostia, o ponto legado), ambos com
`name = "Rome"` e `port_of = ITA-ROM`. Decisão em
`tools/world_city_curation.py` (`EXCLUDED_PORTS`): fica **`ITA-ROM`**; sai a
chave i18n `port.name.ITA-ROME` e a linha correspondente em `ports.csv/json`
(144 → 143). Nenhuma rota, prédio ou save referencia o id do porto: `ITA-ROME`
como **cidade** da fonte continua existindo (`cities_source.csv`) e é o alvo de
`port_of` (`PORT_OF_OVERRIDES`: `ITA-ROM → ITA-ROME`, traduzido para a cidade
final `ITA-ROM`). `build_world_cities.py` passou a **avisar** quando dois portos
da mesma nação têm o mesmo nome e o mesmo `port_of`.

Duplicatas do mesmo tipo encontradas nesta rodada e deixadas para depois:
`CHN-GUAN`×`CHN-GUA` (Guangzhou), `CHN-SHAN`×`CHN-SHA` (Shanghai),
`IDN-MEDA`×`IDN-MED` (Medan), `JPN-NAGO`×`JPN-NAG` (Nagoya),
`TUR-BURS`×`TUR-BUR` (Bursa) e `USA-LOSA`×`USA-LOS` (Los Angeles). Elas foram
fundidas na r5 — ver §18.10. O aviso do build continua como rede de segurança.

### 18.7 "Tomar cidade entrega território" — o que existe

Existe o **vínculo cidade→província**, não uma mecânica de guerra:

* `map/territory_map.gd::assign()` liga cada província à cidade mais próxima da
  **mesma `home_nation`** (vínculo estável) e `update()` pinta o dono de cada
  província com a **`nation` atual** da cidade dona; mudar a nação de uma
  cidade repinta as províncias dela na textura `province_owner` (lida por
  `dyn_block` no shader — o `dyn_nation`).
* O gatilho existente é de **debug/seleção**: `city_world.debug_set_city_nation()`
  / `debug_clear_territory_overrides()`. `tools/territory_test.gd` cobre o
  caminho completo (BEL-BRU → FRA repinta as províncias de Bruxelas e só elas;
  clear volta tudo; a textura de dono publica o índice certo) e passa nesta
  rodada.
* **Não existe** captura/ocupação de cidade em jogo (nem cerco, nem combate,
  nem troca de dono por evento). Faltaria: um sistema de guerra/ocupação que
  escreva a `nation` da cidade (ou um override persistente) e chame
  `territory.update()`/`refresh_modes`; o repaint em si **já funciona** e não
  precisou ser implementado.

### 18.8 Como reproduzir

```powershell
$env:PYTHONUTF8=1
python tools/build_atlas_layers.py            # dono + ids sintéticos + SDF/cor
python tools/reconstruct_natural_inputs.py --prune   # dono/território (histórico)
python tools/compute_province_areas.py        # área por província (Voronoi)
python tools/build_province_nature.py         # province_nature/nature/agriculture/deposits
python tools/compute_territory_areas.py       # área por território
python tools/build_world_cities.py --write-scenario  # portos/cidades/i18n
python tools/build_scenario_data.py           # scenario/*.json + i18n.json
python tools/validate_scenario_data.py
& $godot --headless --path . --import
```

`nav_mask.png` não muda (nenhuma ilha entrou/saiu; a terra é a mesma), então
`generate_nav_mask.py` não é necessário nesta rodada.

Os ids sintéticos vivem no **atlas** (`provinces.json`) e nos rasters/SDFs —
é o que o jogo lê. Os inputs legados `data/definitions/provinces.txt` e
`data/history/provinces/provinces.json` continuam listando só as províncias
admin-1 de origem (mesma defasagem já existente para as 63 províncias do
Caribe da r2): eles alimentam só `build_natural_geography.py`/`generate_nav_mask.py`,
que não rodaram aqui; a natureza das províncias novas é gerada por
`build_province_nature.py` direto do atlas. `reconstruct_natural_inputs.py
--prune` sincroniza o dono das províncias existentes (Luhansk) e move Luhansk
de TRT000232 (UKR) para TRT000190 (RUS).

### 18.9 Verificação e evidência

* `validate_scenario_data`: **0 erros, 0 avisos** (10 arquivos).
* `build_world_cities.py --check`: **exit 0** (143 portos, nenhuma referência
  órfã; só os avisos das duplicatas pré-existentes).
* `tools/territory_test.gd`: **OK** (177 nações com província; dono inicial ==
  nação; override repinta; textura de dono).
* `tools/nav_strait_test.tscn`: **PASS** (mares conectados, portos no mar).
* `tools/economy_integration_test.gd`: **OK** (geografia, cadeia, investimento,
  save/load).
* `tools/economy_city_catchment_test.gd`: **OK** (partição, conservação de área,
  toda cidade com terra; 4.204 províncias com aptidão).
* `godot --headless --import`: reimportou os rasters do atlas e o i18n.
* Captura política 1920×1080: `docs/map_visual/ukraine_front/`
  (`map3d_political_d052.png`, `map3d_political_d026.png`, foco `UKR-KYI`,
  `manifest_map3d_political.json`) + recortes `front_crop_d026.png` e
  `front_crop_d052.png` mostrando a frente RUS/UKR.

### 18.10 Adendo r5 — portos duplicados fundidos (2026-11-12)

O bloco acima (§18.1–18.9) é a rodada r4; os números de portos lá (143) são
os de antes deste adendo. A fonte de `ports_source.csv` mantém os 114 portos
do cenário da fase 1 e acrescenta 23 pontos "snap na costa" de 2026. Para as
cidades que **já tinham** porto legado, os dois registros sobreviveram ao
`promote_scenario`: um pino no centro da cidade (terra) e outro na costa. Na
r5 todos foram fundidos em `EXCLUDED_PORTS` (`tools/world_city_curation.py`),
ficando o ponto de costa:

| cidade | mantido (ponto de costa) | removido (centro da cidade) | entre os dois |
|---|---|---|---:|
| Guangzhou | `CHN-GUAN` — foz do rio das Pérolas, a caminho de Nansha | `CHN-GUA` | 20,7 km |
| Xangai | `CHN-SHAN` — Waigaoqiao, foz do Yangtzé | `CHN-SHA` | 14,0 km |
| Medan | `IDN-MEDA` — Belawan, o porto real | `IDN-MED` | 19,1 km |
| Nagoya | `JPN-NAGO` — Nagoya Port, baía de Ise | `JPN-NAG` | 17,6 km |
| Bursa | `TUR-BURS` — costa de Mudanya, mar de Mármara | `TUR-BUR` | 17,0 km |
| Los Angeles | `USA-LOSA` — costa de Playa del Rey/Dockweiler | `USA-LOS` | 17,0 km |
| Caracas (extra) | `VEN-LAG` — La Guaira, o porto real (legado) | `VEN-CCS` | 14,4 km |

Critério: manter o registro **melhor situado em mar aberto/porto real**. A
distância assinada à costa no `world_sdf.png` (R, `(r-128)/2` texels de
4,9 km) mostra o legado a +24,5 km (Guangzhou), +34,3 (Xangai), +44,1 (Medan),
+44,1 (Nagoya), +34,3 (Bursa), +34,3 (LA) e +29,4 (Caracas) *dentro* de terra;
os mantidos ficam a +4,9–9,8 km, já na costa. `CHN-GUAN` é o ponto do estuário
mais próximo do porto de Nansha (~29 km; Nansha é distrito de Guangzhou, sem
entrada própria no roster);
`CHN-SHAN` é Waigaoqiao (o maior terminal de contentores de Xangai);
`USA-LOSA` está a ~25 km ao norte de San Pedro/Long Beach, que ficam fora da
célula de ≈8 km/px — é o melhor dos dois pontos disponíveis.

**Detector geral** (nome normalizado igual, par < 15 km, mesmo `port_of` com
mais de um porto): além dos seis pares, só apareceu `VEN-CCS`×`VEN-LAG` (14,4
km, mesmo `port_of = VEN-CCS`), fundido como extra. Os demais grupos de
`port_of` com >1 porto (Durrës/Tirana, Baku/Sumqayıt, Semarang/Yogyakarta,
Reykjavík/Reykjanesbær, Fukuoka/Kitakyushu, Nagoya/Hamamatsu,
Tokyo/Sendai, Podgorica/Nikšić, Miami/Jacksonville) são **portos distintos e
reais**, longe entre si; o `port_of` deles vem da regra "cidade da mesma nação
mais próxima" do `assign_port_of`, que não tem teto de distância (Hamamatsu→
Nagoya, Jacksonville→Miami) — pendência de dados separada, não um pino
duplicado, fora do escopo desta rodada.

**Reprodução e verificação:**

* `python tools/build_world_cities.py --write-scenario` → **143 → 136 portos**,
  136/136 com `port_of`; o aviso de duplicata ficou silencioso. O build também
  passou a ignorar `EXCLUDED_PORTS` na checagem de "porto do cenário fora da
  fonte" (o porto sai de propósito), então a primeira execução já fecha limpa.
* `python tools/build_scenario_data.py` → `ports.json` 136.
* `python tools/validate_scenario_data.py` → **0 erros, 0 avisos** (10 arquivos).
* `build_world_cities.py --check` → **exit 0**.
* i18n: saem 7 chaves `port.name.*` (21 linhas no `i18n.json`); as chaves
  `city.name.*` das mesmas cidades (Shanghai, Guangzhou, Medan, Nagoya, Bursa,
  Los Angeles, Caracas) ficam. 1.646 chaves no total.
* Nenhum `port_of` órfão ou de outra nação (136/136); nenhuma cidade perdeu
  porto: a checagem `nearest_port` (mesma nação + `landmass`, haversine como no
  núcleo C++) antes/depois mudou só 19 cidades — as 7 envolvidas e vizinhas do
  mesmo país, com 2–20 km de deslocamento para a costa.
* `nav_strait_test.tscn`: **PASS**; `economy_integration_test.gd`: **OK**;
  `convoy_lifecycle_test.tscn`: **OK** (20 dias, 408 viagens/408 chegadas,
  0 fantasmas).

## 19. Curadoria r6 — Sudão do Sul, Kosovo e Somalilândia jogáveis + população nacional 2026 (2026-11-12)

Rodada de **dados e contrato**: **179 nações, 807 cidades e 137 portos**
(antes 177/802/136). Entram **Sudão do Sul (SSD)** e **Somalilândia (SOL)** pelo
`NATION_SUPPLEMENT` de `tools/world_nation_curation.py`; **Kosovo (XKX)** já
estava no roster e passa a receber as províncias do atlas com tag **KOS**.

### 19.1 Curadoria

| nação | cidades forçadas | população nacional 2026 | `sim_population` | PIB PPP |
|---|---|---:|---:|---|
| SSD | `SSD-JUB` (Juba, capital), `SSD-MAL` (Malakal), `SSD-WAU` (Wau) | 12.188.788 | 475.009 | ausente (declarado) |
| SOL | `SOL-HAR` (Hargeisa, capital), `SOL-BER` (Berbera, com porto) | 5.700.000 | 1.450.000 | ausente (declarado) |
| XKX | `XKX-PRI` (Pristina, capital), `XKX-PRIZ` (Prizren) | 1.576.876 | 721.464 | ausente (declarado) |

* `ATLAS_TAG_ALIASES = {"KOS": "XKX", "SDS": "SSD"}` converte a tag do atlas
  (Natural Earth) para a tag do jogo em `reconstruct_natural_inputs.py`,
  `build_province_nature.py` e na adjacência de cores
  (`build_world_cities.game_graph`).
* `NATION_CODE_OVERRIDES` (`geography_service.gd` e `build_province_nature.py`)
  e `PROVINCE_TAG_OVERRIDES` ganham `SSD → SDS`.
* Bandeiras: `ss` (flagcdn.com) e `sol` (Wikimedia Commons, Somalilândia não
  tem ISO 3166-1 — o campo `flag` do supplement declara o arquivo). `xk` já
  existia. `map_frame_test` exige bandeira em toda capital visível e passa.
* A cota de cidades é a mesma fórmula; nenhuma nação existente perdeu cidade ou
  porto. `validate_scenario_data` fecha **0 erros / 0 avisos**.

### 19.2 População: exibição × simulação

O jogo exibe a **população nacional real de 2026** rateada entre as cidades;
a simulação continua na **aglomeração da fonte** (`sim_population`), para o
balanço calibrado não mudar.

| contrato | campo | total |
|---|---|---:|
| população exibida (HUD, painéis, nações) | `population` | **8.203.501.062** |
| unidade de simulação (comida, caixa, empregos, preços) | `sim_population` | **1.271.720.509** |

`tools/build_world_cities.distribute_national_population` divide
`nations.csv.population` entre as cidades proporcionalmente à aglomeração, com
resto por maior fração e desempate por id (determinístico); a soma por nação é
exata. O C++ (`DemoCity` e o `City` legado) lê `sim_population` com fallback
para `population` (save antigo ou cenário sintético), serializa o campo em
`demo_to_dict`/`demo_from_dict`, preenche o benchmark com
`sim_population = population` e usa a unidade de simulação em renda externa,
necessidade, metas de caixa, mercado de trabalho e divisões per-capita do
status. O status expõe `sim_population` por cidade; o total exibido soma
`population`. `GeographyService.resolve_initial_farms` e o crescimento urbano
(`map/city_world.gd`) também usam `sim_population`.

### 19.3 Pipeline e verificação

```powershell
$env:PYTHONUTF8=1
python tools/build_world_cities.py --write-scenario
python tools/build_atlas_layers.py
python tools/generate_nav_mask.py
python tools/reconstruct_natural_inputs.py --prune
python tools/compute_province_areas.py
python tools/build_province_nature.py
python tools/build_resource_map.py
python tools/apply_traditional_colors.py --refresh-adjacency
python tools/build_scenario_data.py
python tools/validate_scenario_data.py
python tools/build_world_cities.py --check
& $godot --headless --path . --import
```

* `validate_scenario_data`: **0 erros, 0 avisos** (10 arquivos).
* `build_world_cities.py --check`: **exit 0**.
* `territory_test.gd`: **OK** — 179 nações jogáveis com província, dono inicial
  igual à nação.
* `map_frame_test.tscn`: **OK** — 300 frames, 179/179 nações com cor, 61
  capitais visíveis com 100% de bandeira.
* `economy_world_test.gd`: **OK** — 60 dias, 0 violações, `money_total`
  conservado (2.098.338.839,85); 179/179 nações com satisfação média ≥ 70%
  (177 com todas as cidades ≥ 70%; BEL e NED têm uma cidade em 60%/40%).
* `economy_long_run_test.gd`: **OK** — 365 dias sem violação.
* `perf_day_peak.tscn`: **exit 0** — tick 75,2 ms médio (p95 94,6; máx 149,0),
  frame de pico 80,2 ms com 807 cidades.
* Captura política 1920×1080: `docs/map_visual/new_nations/horn/` (foco
  `SOL-HAR`, d120/d052) e `docs/map_visual/new_nations/balkans/` (foco
  `XKX-PRI`, d120/d052), manifestos sem avisos.




