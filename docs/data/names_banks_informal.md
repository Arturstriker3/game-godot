# Bancos por nação (C4) e serviços informais (B7) — dados e fontes

Status: **dados apenas**. Nada aqui está ligado ao C++ nem ao save/load. Os
arquivos são lidos no boot como os demais `data/definitions/economy/*.json`; a
Parte C (bancos) e a B7 (informalidade) passam a ter nomes e catálogo reais
quando o código dessas partes for escrito.

## 1. Objetivo

**C4 — bancos.** O plano da Parte C pede 3 bancos por nação, com dono dado pelo
sistema da F9 (estatal / nacional / estrangeiro). O núcleo não precisa saber
"qual o banco deste país": precisa de um **nome** para exibir e para o grupo
econômico que o representa. Este arquivo entrega o nome real do maior banco
**público**, do maior banco **privado doméstico** e do banco **estrangeiro**
(país de origem predominante + maior banco desse país), além do **banco central
da moeda**.

**B7 — informais.** O plano pede um setor próprio onde os trabalhadores
informais atuam. Este arquivo entrega o **catálogo de categorias** com nome
genérico nos três idiomas e os **termos locais** por país (camelô, boda-boda,
jeepney, tianguis…), para a UI e para o texto de evento usarem o nome que o
jogador reconhece.

## 2. Como o jogo já nomeia empresas e grupos (e como estes dados seguem)

O jogo **não** guarda nomes de empresa em CSV por país. Ele gera:

- `core/economy/group_namer.gd` (`GroupNamer`) lê o ranking de grupos do núcleo e
  nomeia cada um pela **cultura da nação de origem** e pelo **setor**; a semente
  é `absi(hash(id)) % 100000` e o lugar vem da cidade-sede.
- `core/culture/business_name_generator.gd` monta o nome a partir de
  `data/definitions/culture/business_names/<língua>.json` (setores, tipos,
  sobrenomes, sufixos legais, padrões como `{type} {qualifier}`), com o mapa
  cultura → língua em `culture_language.json`. Sem cultura/língua, o gerador
  devolve vazio e o chamador usa um rótulo declarado — nunca inventa cultura.

Por isso:

- os nomes de banco **reais** entram como dado estático (nome, ano, fonte),
  exatamente como `ownership_baseline.json` guarda o estoque de IDE;
- o único nome **fictício** do arquivo é `public.proposed_name`, para país sem
  banco estatal, e ele é gerado pelo **mesmo** `BusinessNameGenerator` com setor
  `finance` e a semente do `GroupNamer` — `tools/gen_bank_proposals.gd` reproduz
  o sorteio. Ele vem marcado com `proposed_name_basis` e nunca substitui um nome
  real.

## 3. Arquivos e contrato

| arquivo | conteúdo |
| --- | --- |
| `data/definitions/economy/banks.json` | 179 nações; banco central, público, privado, estrangeiro; `sources` com 257 referências |
| `data/definitions/economy/informal_services.json` | 14 categorias, 275 termos locais em 85 países; `sources` |
| `data/definitions/i18n/i18n.csv` e `.json` | 22 chaves novas (`banks.*`, `informal.*`) em pt-BR/en/es |
| `tools/validate_names_data.py` | validador (nações cobertas, ids únicos, status, fontes) |
| `tools/gen_bank_proposals.gd` | gerador determinístico dos nomes propostos |

### 3.1 `banks.json`

```json
"BRA": {
  "name": "Brazil",
  "currency": "BRL",
  "central_bank": {"name": "Banco Central do Brasil", "name_en": "...", "name_es": "...", "source_key": "..."},
  "public":  {"status": "present", "owner": "state",    "name": "...", "legal_name": "...", "basis": "total_assets", "year": 2023, "source_key": "..."},
  "private": {"status": "present", "owner": "national", "name": "...", "year": 2023, "source_key": "..."},
  "foreign": {"status": "present", "owner": "foreign",  "origin_iso3": "ESP", "branch_candidate": "Banco Santander, S.A.", "branch_status": "derived_from_origin_largest", "source_key": "..."}
}
```

Regras:

- `status`: `present` (nome real, ano e fonte), `none` (verificado que não há) ou
  `no_data` (não verificável). Em `none`/`no_data`, `name` é `null` e `note`
  explica o motivo. **Nenhum campo é preenchido por chute.**
- `private.owner` é sempre `national`: o maior banco **privado doméstico**. Banco
  estatal não conta como privado e filial estrangeira não conta como doméstica.
- `foreign` guarda só o **país de origem predominante** do capital estrangeiro
  (`origin_iso3`) e o maior banco **desse país** como `branch_candidate`. O nome
  da filial local não é inventado: `branch_status` diz que é derivado.
- `public` com `status: none` traz `proposed_name` **fictício** (ver §2), para o
  jogo não exibir banco público sem nome.
- Banco central é **por moeda**: zona do euro usa o BCE; UEMOA/BCEAC usam
  BCEAO/BEAC; OECS usa o ECCB; país dolarizado sem autoridade própria (Panamá)
  fica com `status: none` + nota. A comparação entre nações da mesma moeda é
  feita pelo validador (divergência vira aviso, não erro, porque cada país pode
  grafar o mesmo banco na sua língua).

### 3.2 `informal_services.json`

Cada categoria tem `id`, `name_pt/name_en/name_es`, `description_pt`,
`formal_sector` (setor de `services.json` que ela atende ou com que concorre;
`null` quando não é serviço) e `local_terms[]` com `iso3`, `term`, `lang`,
`note` e `source_key`.

Termo sem publicação única catalogada usa `source_key: "common_usage"`, que
declara no próprio arquivo: "termo de uso corrente, documentado em literatura
acadêmica local e imprensa; sem publicação única catalogada". É a ausência
honesta, não uma citação inventada.

## 4. Fontes

### 4.1 Bancos

- **Ranking por ativos**: The Banker, *Top 1000 World Banks* (edição de 2024/2025)
  — https://www.thebanker.com/Top-1000-World-Banks; complementado por
  `thebanks.eu` e por relatórios anuais dos próprios bancos (ano do exercício).
- **Banco central**: site oficial de cada autoridade monetária; BCE para a zona
  do euro; BCEAO/BEAC para as uniões monetárias africanas; ECCB para o Caribe
  Oriental.
- **Capital estrangeiro**: relatórios de supervisão bancária, relatórios anuais
  e bases de fusão/aquisição; a referência metodológica é a literatura de
  globalização bancária (Claessens & van Horen) e o *Global Financial Development
  Database* do Banco Mundial — https://www.worldbank.org/en/publication/global-financial-development.
  Onde não existe ranking por país de origem, o campo fica `no_data`.
- **Nota sobre links**: o antigo diretório de bancos centrais do BIS
  (`bis.org/cbanks.htm`) saiu do ar na reformulação do site em 2026; as
  referências que o usavam foram mantidas por título/ano e o nome foi conferido
  no site de cada banco central.

### 4.2 Informalidade

- ILO, *Women and Men in the Informal Economy: A Statistical Picture*, 3ª ed.
  (2018) — https://www.ilo.org/publications/women-and-men-informal-economy-statistical-picture-third-edition
  e a atualização estatística de 2023 (58% do emprego mundial).
- ILOSTAT, tópico de informalidade — https://ilostat.ilo.org/topics/informality/
- WIEGO, *Statistical Picture* — https://www.wiego.org/statistical-picture
- Pesquisas de força de trabalho nacionais (IBGE/PNAD Contínua, INEGI/ENOE,
  DANE/GEIH, INEI, INDEC, Statistics South Africa/QLFS, KNBS, NBS Tanzânia,
  UBOS, GSS Gana, NBS Nigéria, CAPMAS, HCP Marrocos, PSA Filipinas, BPS
  Indonésia, NSO Tailândia, GSO Vietnã, MoSPI Índia, BBS Bangladesh, PBS
  Paquistão, ABS, Stats NZ), usadas para ancorar a categoria no país.

## 5. Cobertura

### 5.1 Bancos (179 nações, 136 moedas, 257 fontes)

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

Por papel: `public` 127 present / 36 none / 16 no_data; `private` 140 / 12 / 27;
`foreign` 119 / 11 / 49. As 36 nações com `public: none` receberam
`proposed_name` fictício (100% cobertas). Banco central nomeado em 135 das 136
moedas (Panamá não tem banco central próprio — ver §3.1).

### 5.2 Serviços informais (14 categorias, 275 termos, 85 países)

Categorias: comércio ambulante, comida de rua, transporte informal de
passageiros, transporte informal de carga, construção/reforma informal, serviço
doméstico, serviços pessoais de rua, guarda de carros/estacionamento, catação e
reciclagem, agricultura de subsistência, reparos informais, produção domiciliar e
artesanato, pequeno comércio/quiosque, jornal diário.

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
store` (PHL), `hanout` (MAR/TUN), `bakkal` (TUR), `kayayei`
(GHA), `restavèk` (HTI), `kasambahay` (PHL), `colf` (ITA).

## 6. Lacunas declaradas

- **`public: no_data` (16)**: AFG, BDI, CAF, COG, GAB, GIN, MRT, NIC, PRK, SLV, SUR,
  SYR, TGO, TKM, VCT, YEM — ranking de ativos não publicado ou disputa entre dois
  candidatos.
- **`private: no_data` (27)**: BEN, BRB, COG, CRI, DJI, DMA, GIN, GNB, GNQ, GRD,
  IRQ, KNA, LUX, MDA, MLI, MOZ, POL, SEN, SLV, SSD, SYR, TCD, URY, VCT, XKX, YEM,
  ZMB — setor dominado por filiais estrangeiras, ou titularidade doméstica não
  confirmada.
- **`foreign: no_data` (49)**: inclui sistemas fechados ou fragmentados
  (DEU, FRA, ESP, NED, CHE, DNK, IRL, ISL, NOR, AUS, SGP, RUS, CHN…), onde não há
  país de origem predominante verificável.
- **Sem banco público (36)**: inclui EUA, MEX, CAN, AUT, SWE, DNK, FIN, NED, SGP,
  ISR, JOR, LBN; nesses casos há banco de fomento, mas não banco comercial
  estatal — a nota de cada entrada diz qual.

## 7. Verificação

- `python tools/validate_names_data.py` — 179/179 nações, ids únicos, ISO3 de
  termos válido, todo `source_key` resolvido, `status` válido, `present` com
  ano/fonte, `none`/`no_data` com motivo. Sai com código 1 em qualquer falha.
  **Nesta máquina não há Python instalado** (só o atalho da Microsoft Store, que
  falha); a mesma checagem foi executada com um espelho GDScript headless
  (`tools/_tmp_validate_names.gd`, não versionado) e passou.
- Amostra conferida por um verificador independente contra a fonte citada
  (resultado no relatório da tarefa): 9 nações e 10 termos locais. 8/9 nações e
  10/10 termos aprovados; **GIN reprovado e corrigido** — a Banque Nationale de
  Guinée é banco privado (grupo mauritano AON), não estatal; a entrada passou a
  `no_data` citando a BDG/BNIG como estatais sem ranking de ativos.
- Link: a citação primária é **título + ano**; o `url` é auxiliar. O antigo
  diretório de bancos centrais do BIS (`bis.org/cbanks.htm`) morreu na reforma do
  site em 2026 e foi removido (`url: null` + `url_note`). Uma varredura
  automática das 257 URLs falhou em 61 — em boa parte sites institucionais que
  bloqueiam requisição automática (The Banker, IMF, bancos centrais), não links
  inexistentes —, por isso só o BIS foi removido em massa.

- Nota sobre `note`: quando a nota cita percentual ou valor de ativo, o número é
  o da fonte da entrada — não foi refeito um a um nesta rodada.

## 8. Como regenerar

```powershell
# nomes propostos (fictícios) para país sem banco público
& $godot --headless --path . --script res://tools/gen_bank_proposals.gd

# validação
python tools/validate_names_data.py
```

Os nomes propostos dependem só de `culture_language.json`,
`business_names/<língua>.json` e do `hash` do ISO3 — a mesma semente do
`GroupNamer`, portanto reproduzíveis.
