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

