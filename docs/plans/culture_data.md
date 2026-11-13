# Dados de cultura — v2 (uma nação, uma cultura)

Status: **dados apenas**. Nada aqui está ligado à economia, ao C++ ou ao
save/load. A UI lê o catálogo em modo somente leitura (População T2/T3 e
Dossiê T1).

## 1. Objetivo

Dar ao jogo uma base cultural **simples e determinística** para a futura
migração: cada pessoa pertence a uma **cultura nacional** (o gentílico do país)
ou a um **povo sem estado** reconhecido. A v2 substitui o modelo v1.2, que
misturava culturas nacionais, subgrupos (`parent`) e identidades hifenizadas
(`mexicano_americano`, `porto_riquenho`).

Regra central: **cultura X ↔ nação X**. Uma cultura nacional tem `nation` igual
à tag do país no cenário; um povo sem estado tem `nation` vazio e `home_region`
com as cidades-núcleo. Minorias em uma cidade são **outras culturas nacionais**
(diáspora/imigração), nunca culturas híbridas.

## 2. Arquivos e contrato

Saída em `data/definitions/culture/`:

| arquivo | linhas | conteúdo |
| --- | --- | --- |
| `cultures.csv` | 206 | `id,name_pt,name_en,name_es,family,language,religion,nation,home_region,color` |
| `cultures.json` | 206 | mesmo conteúdo, para consumo em GDScript |
| `nation_cultures.csv` | 179 | `nation,primary_culture,accepted_cultures,composition,source,year` |
| `city_cultures.csv` | 834 | `city_id,nation,city,cultures,source` |

Regras de leitura:

- `composition` e `cultures` são listas `cultura:percentual` separadas por `;`,
  somando **100** (inteiros, maior resto).
- `outras` é o resíduo, **não é cultura**; nunca aparece em `cultures.csv` nem
  em `accepted_cultures`.
- `nation` preenchido ⇒ cultura nacional (exatamente **uma por nação**);
  `home_region` vazio. `nation` vazio ⇒ povo sem estado; `home_region` lista
  cidades do cenário (`USA-HON`) ou a tag do país quando não há cidade-núcleo
  (`basco`: `ESP;FRA`).
- `primary_culture` de cada nação é **a** cultura nacional daquela nação.
- `source`/`year` guardam a procedência do número (censo, estimativa regional,
  `national_fallback`); a UI não exibe mais a fonte.
- Não existem as colunas `parent` nem `subcultures`; o validador falha se
  aparecerem.

## 3. Modelo

1. **Nacional (179).** Um gentílico por nação do cenário, incluindo SOL, XKX e
   SSD: `americano` (USA), `brasileiro`, `mexicano`, `britânico` (funde inglês,
   escocês, galês e norte-irlandês), `espanhol` (funde castelhano e galego),
   `francês`, `alemão`, `chinês` (funde han, cantonês, hokkien, hakka e
   teochew), `indiano` (funde as grandes culturas linguísticas), `nigeriano`
   (funde iorubá, igbo, hauçá e fulani), `srilankês`, `paquistanês`,
   `somalilandês`, `kosovar`, `sul-sudanês`, etc.
2. **Diáspora.** Em cidades e composições, a minoria é a cultura nacional do
   país de origem: Los Angeles `mexicano` (não mexican-American), Nova York
   `americano` + `dominicano` + `chinês` (Porto Rico é USA ⇒ `americano`),
   Berlim `alemão` + `turco`, Paris `francês` + `marroquino`, Londres `britânico`
   + `indiano`/`paquistanês`/`bengali`/`jamaicano`.
3. **Povos sem estado (27).** Só povos com identidade forte e fonte, necessários
   para separatismo e migração interna: curdo, catalão, basco, quebequense,
   uigur, tibetano, tâmil (Sri Lanka), tártaro da Crimeia, berbere/amazigue,
   tuaregue, checheno, tártaro, afar, belúchi, papua (Nova Guiné Ocidental),
   maori, aborígene australiano, havaiano, inuit, nativo americano, indígena
   brasileiro, quíchua, aimará, guarani, maia, mapuche e nahua.
   Ficam de fora as etnias internas de nações com Estado (iorubá/igbo/hauçá na
   Nigéria, zulu/xhosa na África do Sul, han na China): elas fundem-se no
   gentílico nacional.
4. **Sem híbridos.** Não existem identidades “X-americano”, “afro”, “hispano”,
   “anglo”, subgrupos nem `parent`. O validador tem lista negada por id e por
   nome.

## 4. Migração v1.2 → v2

`tools/migrate_cultures_v2.py` transforma a curadoria antiga sem inventar
números:

- grupo interno de uma nação → cultura nacional da nação (zulu na ZAF →
  `sul_africano`; han na CHN → `chinês`; han em TWN → `taiwanês`).
- mesmo grupo fora do país de origem → cultura nacional de origem (punjabi em
  Londres → `indiano`; urdu em Londres → `paquistanês`; turco em Berlim →
  `turco`; árabe magrebino na França → `marroquino`, a maior origem do grupo).
- `porto_riquenho` → `americano`; `mexicano_americano` → `mexicano`;
  `canadense_ingles` → `canadense`; `flamengo`/`valão` → `belga`;
  `escocês`/`galês`/`norte-irlandês`/`inglês` → `britânico`.
- o que não tem nação de origem clara na curadoria (`caribenho` fora do Caribe,
  `melanésio` na Nova Caledônia, `inglês` nas Filipinas) vai para `outras`.

Composições antigas por etnia interna colapsam no gentílico (Nigéria
`nigeriano:66;outras:34`, Índia `indiano:91;outras:9`, Paquistão
`paquistanês:68;outras:32`). Onde o censo mede origem estrangeira (EUA, Canadá,
Reino Unido, França, Alemanha, Golfo), a diáspora permanece como cultura
nacional de origem.

## 5. Exemplos (saída de `city_cultures.csv`)

| cidade | mistura |
| --- | --- |
| Los Angeles (USA-LOS) | `mexicano:45;americano:27;chinês:8;filipino:4;sul-coreano:3;outras:13` |
| Nova York (USA-NEW) | `americano:56;chinês:11;dominicano:6;mexicano:5;outras:22` |
| São Paulo (BRA-SAO) | `brasileiro:78;italiano:7;português:5;japonês:3;outras:7` |
| Londres (GBR-LDN) | `britânico:50;jamaicano:5;bengali:4;indiano:4;paquistanês:3;outras:34` |
| Berlim (DEU-BER) | `alemão:64;turco:12;polonês:5;russo:5;outras:14` (fallback nacional) |
| Paris (FRA-PAR) | `francês:60;marroquino:11;italiano:5;português:5;outras:19` |
| Barcelona (ESP-BAR) | `catalão:55;espanhol:30;outras:15` |
| Montreal (CAN-MON) | `quebequense:65;canadense:15;outras:20` |
| Lagos (NGA-LAG) | `nigeriano:90;outras:10` |
| Mumbai (IND-MUM) | `indiano:75;outras:25` |

## 6. Uso futuro em migração

- Fluxo externo: cultura X → nação X (`nation` do catálogo); a diáspora já
  existente em uma cidade é o estoque inicial.
- Fluxo interno: povos sem estado usam `home_region` como origem/destino
  (curdo, tâmil, quebequense, mapuche…), sem criar culturas regionais.
- A UI lê `CultureDb` somente para exibir; nada é enviado à simulação. O campo
  `nation` do catálogo é a chave para ligar cultura e país quando a migração
  for implementada.

## 7. Validação e limites

- `tools/build_cultures.py` gera; `tools/validate_cultures.py` valida
  (0 erros): bijection nação↔cultura nacional, `home_region` válido, ausência
  de `parent`/`subcultures`, lista negada de híbridos/raça, soma 100 e ΔE*ab
  ≥ 25 dentro da família.
- Limitações conhecidas: o cenário não tem cidade-núcleo basca (uso `ESP;FRA`);
  a diversidade do fallback urbano amplifica minorias pequenas (declarado no
  `source`); a composição nacional colapsa etnias internas por decisão de
  modelo — o detalhe étnico não é simulado.
- Regenerar: `python tools/build_cultures.py && python tools/validate_cultures.py`.
