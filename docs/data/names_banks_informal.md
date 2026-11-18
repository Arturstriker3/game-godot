# Bancos por nação (C4) e serviços informais (B7) — dados e fontes

Status: **dados apenas**, versão **2** de `data/definitions/economy/banks.json`.
Nada aqui está ligado ao C++ nem ao save/load. Os arquivos são lidos no boot
como os demais `data/definitions/economy/*.json`; a Parte C (bancos) e a B7
(informalidade) passam a ter nomes e catálogo prontos quando o código dessas
partes for escrito.

## 1. Objetivo

**C4 — bancos.** O plano da Parte C pede 3 bancos por nação, com dono dado pelo
sistema da F9 (estatal / nacional / estrangeiro). O núcleo não precisa saber
"qual o banco deste país": precisa de um **nome** para exibir e para o grupo
econômico que o representa. Este arquivo entrega, por nação, o **dado
estrutural** do setor (existe banco estatal? qual o **país de origem
predominante** do capital estrangeiro? qual o ano/base do ranking?) com fonte, e
um **nome fictício** para os três papéis — mais o **banco central da moeda** com
rótulo institucional genérico.

**B7 — informais.** O plano pede um setor próprio onde os trabalhadores
informais atuam. O catálogo dá as **categorias** com nome genérico nos três
idiomas e os **termos locais** por país (camelô, boda-boda, jeepney, tianguis…),
para a UI e para o texto de evento usarem o nome que o jogador reconhece (§10).

## 2. Política de nomes (direitos e marcas)

Nenhum **nome de banco real** é dado de jogo. Motivo: nome comercial é marca de
terceiro; exibir uma instituição real como "o banco estatal da nação X" cria
associação factual que o jogo não sustenta (e não precisa). O que é **real** é
só a estrutura — e ela vem com fonte:

- existe instituição bancária **estatal** de porte comparável no ranking?
- o setor privado doméstico tem um líder identificável?
- o capital estrangeiro tem um **país de origem predominante**?

Os nomes saem do **mesmo sistema de nomes do jogo** (`F9`), não de uma lista
paralela:

- `core/culture/business_name_generator.gd` monta o nome a partir da **cultura
  da nação** (mapa cultura → língua em `culture_language.json`) e do **setor**
  `finance`, usando `data/definitions/culture/business_names/<língua>.json`
  (tipos, qualificadores, sobrenomes, sufixos legais, padrões como
  `{type} {qualifier}`). O `place` é a **capital** da nação.
- A semente é determinística por **nação + papel**:
  `absi(hash("<ISO3>:<papel>")) % 100000` — o mesmo formato de semente do
  `GroupNamer` (`absi(hash(id)) % 100000`). Mesmo dado, mesmo nome, sempre.
- **Sem cultura, sem nome.** Se a nação não tem cultura mapeada, o gerador
  devolve vazio e o chamador não inventa: a ausência fica registrada na rodada
  do gerador. O dado nunca preenche um nome por chute.
- **Filial estrangeira é um grupo só.** O nome do banco estrangeiro é gerado
  **uma vez por país de origem** (semente `<ORIGEM>:foreign`) e repetido em
  todas as nações com aquela origem — é o mesmo grupo econômico atuando em
  vários países, como a F9 modela.
- Todo nome exibido carrega a proveniência no próprio JSON: `name_basis` (e
  `branch_name_basis` na filial) **começa com `fictício:`** e cita a semente.
- `public.proposed_name` existe **só** para nação **sem banco estatal**
  (`public.status = none`): o jogo não exibe banco público sem nome, e não se
  inventa um banco estatal que a fonte não tem. Ele vem com
  `proposed_name_basis` na mesma marcação.
- **Banco central** não é nome de mercado: usa rótulo institucional genérico em
  pt-BR/en/es, por país (`Banco Central de <país>` / `Central Bank of <país>` /
  `Banco Central de <país>`) ou por união monetária (zona do euro, união
  monetária oeste-africana, união monetária da África Central, união monetária
  do Caribe Oriental).

Consequência prática: nenhum texto do jogo (HUD, ficha de nação, tabela de
bancos) precisa de licença de marca, e trocar o gerador não quebra o contrato —
o dado estrutural é que manda no dono (F9).

## 3. Arquivos

| arquivo | conteúdo |
| --- | --- |
| `data/definitions/economy/banks.json` | 179 nações; banco central, público, privado, estrangeiro; 257 fontes |
| `data/definitions/economy/informal_services.json` | 14 categorias, 275 termos locais em 85 países; 28 fontes |
| `data/definitions/i18n/i18n.csv` e `.json` | 22 chaves (`banks.*` 5, `informal.*` 17) em pt-BR/en/es |
| `tools/validate_names_data.py` | validador (contrato, cobertura e marcas) |
| `tools/real_bank_names_denylist.txt` | lista de marcas reais usada **só** pelo validador |
| `tools/gen_bank_names.gd` | gerador dos nomes fictícios (Godot headless) |
| `tools/apply_bank_names.py` | aplica os nomes no JSON (idempotente) |

`tools/gen_bank_proposals.gd` foi **removido**: a proposta de nome público agora
sai do mesmo gerador dos três papéis.

## 4. Contrato do `banks.json` (v2)

Topo do arquivo: `version`, `generated_at`, `owners_note`, `policy`, `note`,
`coverage`, `nations`, `sources`.

Exemplo real — **BRA** (os três papéis `present`):

```json
"BRA": {
  "name": "Brazil",
  "currency": "BRL",
  "central_bank": {
    "kind": "national", "zone": null, "status": "present",
    "name": "Banco Central de Brasil",
    "name_en": "Central Bank of Brazil",
    "name_es": "Banco Central de Brasil",
    "source_key": "bis_cbanks",
    "note": "Banco central nacional de Brasil; nome institucional genérico."
  },
  "public": {
    "status": "present", "owner": "state", "source_key": "<chave em sources>",
    "name": "Faria & Cunha S.A.",
    "name_basis": "fictício: BusinessNameGenerator/finance; semente absi(hash(\"BRA:public\")) % 100000",
    "basis": "total_assets", "year": 2024
  },
  "private": {
    "status": "present", "owner": "national", "source_key": "<chave em sources>",
    "name": "Monteiro & Rodrigues & Filhos",
    "name_basis": "fictício: BusinessNameGenerator/finance; semente absi(hash(\"BRA:private\")) % 100000",
    "basis": "total_assets", "year": 2024
  },
  "foreign": {
    "status": "present", "owner": "foreign", "source_key": "<chave em sources>",
    "origin_iso3": "ESP", "origin_name": "Spain", "origin_basis": "total_assets",
    "branch_candidate": "Gutiérrez y Rubio S.A.",
    "branch_status": "derived_from_origin_largest",
    "branch_name_basis": "fictício: BusinessNameGenerator/finance; semente absi(hash(\"ESP:foreign\")) % 100000"
  }
}
```

Exemplo real — **DEU** (união monetária e `foreign: no_data`):

```json
"DEU": {
  "name": "Germany",
  "currency": "EUR",
  "central_bank": {
    "kind": "monetary_union", "zone": "euro", "status": "present",
    "name": "Banco Central da Zona do Euro",
    "name_en": "Central Bank of the Euro Area",
    "name_es": "Banco Central de la Zona del Euro",
    "source_key": "europe_a_bis_cbdir"
  },
  "public": {
    "status": "present", "owner": "state", "source_key": "bundesbank_de",
    "name": "Finanzen Schulze", "basis": "total_assets", "year": 2024
  },
  "foreign": {
    "status": "no_data", "owner": "foreign", "source_key": "bundesbank_de",
    "origin_iso3": null, "origin_name": null, "origin_basis": null,
    "branch_candidate": null, "branch_status": null, "branch_name_basis": null,
    "note": "Não verificável: sem país de origem predominante na fonte consultada."
  }
}
```

Os exemplos são cópia do arquivo (campos `note` repetitivos omitidos). Alguns
valores de `source_key` aparecem como `<chave em sources>` porque a chave real
carrega sigla de instituição e o próprio validador barra marca no doc; o par
real está no JSON e no bloco `sources`.

Regras:

- `status`: `present` (a estrutura existe; nome fictício + ano/base + fonte),
  `none` (verificado que **não há**) ou `no_data` (não verificável). Em
  `none`/`no_data` o `name` é `null` e uma `note` explica o motivo. **Nenhum
  campo é preenchido por chute.**
- `private.owner` é sempre `national`: o líder **privado doméstico**. Banco
  estatal não conta como privado; filial estrangeira não conta como doméstica.
- `foreign` guarda só o **país de origem predominante** (`origin_iso3`) e a
  filial **derivada** do maior banco desse país (`branch_status` diz que é
  derivada; o nome é o do grupo de origem, ver §2). Onde não há origem
  predominante verificável, `no_data`.
- `public` pode ser **banco de fomento** (desenvolvimento), não só banco
  comercial estatal, mas o v2 não separa os dois casos: registra o maior banco
  de capital estatal por ativos no ranking. `status: none` traz `proposed_name`
  fictício; `no_data` não traz nome nenhum.
- `central_bank` é **por moeda**: `kind` = `national` | `monetary_union` |
  `none`, `zone` = `euro` | `uemoa` | `cemac` | `ecs` (ou `null`). Panamá é o
  único `none` (não tem autoridade monetária própria; a moeda PAB fica sem
  banco central nomeado). Montenegro e Kosovo usam o euro **com** banco
  central nacional — por isso `zone` é só das nações da zona.
- `legal_name` **foi removido** (era a razão social real). Os títulos de
  `sources` foram reduzidos a três rótulos genéricos — relatório anual do banco
  citado, lista de bancos do país (verbete) e "fonte citada para o dado
  estrutural do setor bancário" —, exceto 12 chaves de publicação/dataset
  (ranking do The Banker, base do World Bank, Claessens & van Horen, diretório
  do BIS), cujo título é a própria referência. **URLs e anos foram
  preservados**: eles são a proveniência.

## 5. Fontes do dado estrutural

- **Ranking por ativos**: The Banker, *Top 1000 World Banks* — decide "quem é o
  maior" e o ano do exercício.
- **Base comparativa de sistemas financeiros**: World Bank, *Global Financial
  Development Database* (GFDD).
- **Globalização bancária / origem do capital**: literatura de Claessens & van
  Horen (presença de bancos estrangeiros por país de origem).
- **Diretório de bancos centrais**: BIS — o diretório saiu do ar na reformulação
  do site em 2026; as entradas que o usavam foram mantidas por título/ano e o
  rótulo foi conferido na fonte de cada autoridade monetária.
- **Multilaterais e agências**: IMF (relatórios de país e TA), EBRD, IFC.
- **Supervisores nacionais** (relatórios de supervisão bancária, relatórios
  anuais do setor) e **imprensa financeira local**, usados quando não há ranking
  internacional para o país.

As URLs ficam no JSON como **proveniência** e podem conter domínio de
instituição (é o endereço da fonte). Os **títulos** foram genericizados por
política de marcas: um título não é lugar para marca de banco.

## 6. Cobertura

### 6.1 Bancos (179 nações, 136 moedas, 257 fontes)

| região | nações | público presente | privado presente | estrangeiro presente |
| --- | ---: | ---: | ---: | ---: |
| África | 51 | 38 | 33 | 37 |
| Ásia | 32 | 26 | 30 | 14 |
| Europa | 41 | 27 | 37 | 29 |
| Oriente Médio | 14 | 8 | 11 | 8 |
| América do Norte e Caribe | 23 | 13 | 15 | 17 |
| Oceania | 6 | 5 | 3 | 5 |
| América do Sul | 12 | 10 | 11 | 9 |
| **total** | **179** | **127** | **140** | **119** |

Por papel: `public` **127** present / 36 none / 16 no_data; `private` **140** /
12 / 27; `foreign` **119** / 11 / 49. São **386 nomes fictícios** exibidos
(127 + 140 + 119), todos com `name_basis`/`branch_name_basis`. As 36 nações sem
banco estatal receberam `proposed_name` fictício (36/36 cobertas). Banco central
nomeado em **135 das 136 moedas** — a exceção é o PAB, de Panamá (§4). Tipos:
138 `national`, 40 `monetary_union` (20 euro, 8 oeste-africana, 6 África
Central, 6 Caribe Oriental) e 1 `none`.

### 6.2 Serviços informais (14 categorias, 275 termos, 85 países)

Categorias: comércio ambulante, comida de rua, transporte informal de
passageiros, transporte informal de carga, construção/reforma informal, serviço
doméstico, serviços pessoais de rua, guarda de carros/estacionamento, catação e
reciclagem, agricultura de subsistência, reparos informais, produção domiciliar
e artesanato, pequeno comércio/quiosque, jornal diário.

Cada termo traz `iso3`, `term`, `lang` e `source_key`; 97 dos 275 trazem também
uma `note` explicando o uso, e 132 dos 275 são `common_usage` (§10).

## 7. Lacunas declaradas

- **`public: no_data` (16)**: AFG, BDI, CAF, COG, GAB, GIN, MRT, NIC, PRK, SLV,
  SUR, SYR, TGO, TKM, VCT, YEM — ranking de ativos não publicado ou titularidade
  estatal não confirmada.
- **`private: no_data` (27)**: BEN, BRB, COG, CRI, DJI, DMA, GIN, GNB, GNQ, GRD,
  IRQ, KNA, LUX, MDA, MLI, MOZ, POL, SEN, SLV, SSD, SYR, TCD, URY, VCT, XKX, YEM,
  ZMB — setor dominado por filiais estrangeiras, ou titularidade doméstica não
  confirmada.
- **`foreign: no_data` (49)**: sistemas fechados ou fragmentados (DEU, FRA, ESP,
  NED, CHE, DNK, IRL, ISL, NOR, AUS, SGP, RUS…) e economias onde não há país de
  origem predominante verificável.
- **Sem banco estatal (36)**: ALB, ARM, ATG, AUS, AUT, BRB, CAN, COD, CYP, DJI,
  DNK, EST, FIN, GEO, GNB, GRD, GUY, ISR, JAM, JOR, LBN, LTU, LVA, MDA, MEX,
  MLT, MNE, MWI, NED, PSX, SAH, SGP, SOM, SWE, USA, XKX. Em vários há banco de
  **fomento**, não banco comercial estatal; o v2 não separa os dois casos.
  Todos receberam `proposed_name` fictício.

## 8. Verificação

```powershell
C:\Users\artur\AppData\Local\Python\bin\python.exe tools\validate_names_data.py
```

(`python` puro **falha** nesta máquina — é o atalho da Microsoft Store; use o
caminho acima. Sai com código 1 e lista de problemas; sucesso imprime
`OK — dados de nomes válidos` e as contagens.)

O validador checa, sem tocar no jogo:

- JSON em UTF-8 **sem BOM**; uma entrada por nação do jogo (179/179); `name` e
  `currency` iguais a `nations.csv`; `legal_name` ausente;
- banco central **genérico por país ou união monetária**, com o rótulo fixo nas
  três línguas, `kind`/`zone` coerentes com a moeda e Panamá como único `none`;
- **status** válidos, `present` com ano/base/fonte, `none`/`no_data` com motivo
  e sem nome; `public: none` com `proposed_name` e `public: present` sem ele;
  `owner` fixo por papel; `branch_name_basis` citando a origem;
- **unicidade**: nome de público/privado não colide entre si nem com filial (a
  filial pode repetir entre países da mesma origem);
- **marcas**: nenhum nome de `tools/real_bank_names_denylist.txt` nos campos de
  nome/nota, nos **títulos** de fonte (o `url` nunca é varrido), nos valores
  `banks.*` do i18n **nem neste documento** — o doc é lido e varrido como texto;
- `coverage` do JSON **conferido contra a contagem real** (nações, status por
  papel, nomes fictícios, centrais nomeados, propostas);
- B7: ids únicos, nomes nos três idiomas, ISO3 existente no jogo, fonte
  declarada em cada termo.

**Nota metodológica.** Uma amostra de nações e de termos locais foi conferida
por verificador independente contra a fonte citada; uma entrada de banco foi
**corrigida** — a instituição apontada como estatal era, na verdade, privada
(grupo estrangeiro), e a entrada passou a `no_data` com a nota citando as
estatais sem ranking de ativos. Nenhuma correção foi feita "para o teste
passar": o dado mudou, não o critério.

## 9. Como regenerar

```powershell
$godot = "C:\Users\artur\OneDrive\Área de Trabalho\Godot_v4.7.2-stable_win64.exe"

# 1. nomes fictícios (Saída: tools/data/bank_shards/generated_names.json)
& $godot --headless --path . --script res://tools/gen_bank_names.gd

# 2. aplica no JSON (reescreve banks.json de forma idempotente)
C:\Users\artur\AppData\Local\Python\bin\python.exe tools\apply_bank_names.py

# 3. valida
C:\Users\artur\AppData\Local\Python\bin\python.exe tools\validate_names_data.py
```

`tools/data/bank_shards/` é intermediário (não versionado: `tools/data/*` está
no `.gitignore`). O passo 1 lê `data/definitions/world/out/nations.csv` (capital
da nação e da origem), `culture_language.json`, `business_names/<língua>.json`,
o `foreign.origin_iso3` de `banks.json` e o `hash` de `"<ISO3>:<papel>"`; o passo
2 é **idempotente** — mesma semente, mesmas chaves,
UTF-8 sem BOM —, então rodar duas vezes produz o **mesmo SHA256** do arquivo
(verificado numa cópia, sem tocar no dado do repositório). O passo 2 também
reescreve os títulos de fonte para os rótulos genéricos e recalcula o bloco
`coverage`.

## 10. B7 — serviços informais

`informal_services.json` tem `version`, `note`, `linking_note`, `sources` e
`categories[]`. Cada categoria traz `id`, `name_pt/name_en/name_es`,
`description_pt`, `formal_sector` (o setor de `services.json` que ela atende ou
com que concorre; `null` quando não é serviço) e `local_terms[]`.

Exemplos de termos locais: `camelô` (BRA), `tianguis` (MEX), `mantero` (ARG),
`buhonero` (VEN), `madan sara` (HTI), `higgler` (JAM), `boda-boda`
(KEN/UGA/TZA), `matatu` (KEN), `dala-dala` (TZA), `okada` e `keke napep` (NGA),
`trotro` (GHA), `chapa` (MOZ), `candongueiro` (AGO), `zemidjan` (BEN),
`gbaka` (CIV), `car rapide` (SEN), `louage` (TUN), `tuk-tuk` (THA/EGY),
`songthaew` e `win motosai` (THA), `jeepney`, `tricycle` e `pedicab` (PHL),
`ojek`, `angkot`, `becak` e `warung` (IDN), `xe om` (VNM), `marshrutka`
(RUS/UKR/GEO/ARM/KAZ/…), `dolmuş` (TUR), `servees` (JOR/LBN/SYR/PSX),
`sherut` (ISR), `mikrolet` (TLS), `PMV` (PNG), `zabaleen` (EGY),
`cartonero` (ARG), `pepenador` (MEX), `catador` (BRA), `jua kali` (KEN/UGA),
`spaza shop` (ZAF), `duka` (KEN/TZA/UGA), `kirana store` (IND), `sari-sari
store` (PHL), `hanout` (MAR/TUN), `bakkal` (TUR), `kayayei` (GHA),
`restavèk` (HTI), `kasambahay` (PHL), `colf` (ITA).

São **palavras comuns** do país, não marca: por isso ficam como estão, na
grafia local, com `lang` e, quando há, uma `note`. A fonte de cada termo é uma pesquisa de força
de trabalho nacional ou uma publicação acadêmica sobre o país.

Fontes do catálogo:

- ILO, *Women and Men in the Informal Economy: A Statistical Picture*, 3ª ed.
  (2018) e a atualização estatística de 2023 (58% do emprego mundial).
- ILOSTAT, tópico de informalidade.
- WIEGO, *Statistical Picture of the World's Informal Workers*.
- Pesquisas nacionais de força de trabalho (IBGE/PNAD Contínua, INEGI/ENOE,
  DANE/GEIH, INEI, INDEC, Statistics South Africa/QLFS, KNBS, NBS Tanzânia,
  UBOS, GSS Gana, NBS Nigéria, CAPMAS, HCP Marrocos, PSA Filipinas, BPS
  Indonésia, NSO Tailândia, GSO Vietnã, MoSPI Índia, BBS Bangladesh, PBS
  Paquistão, ABS, Stats NZ, entre outras), usadas para ancorar a categoria no
  país.

Quando o termo não tem publicação única catalogada, o `source_key` é
`common_usage`, que declara no próprio arquivo: "termo de uso corrente,
documentado em literatura acadêmica local e imprensa; sem publicação única
catalogada". É a **ausência honesta**, não uma citação inventada.
