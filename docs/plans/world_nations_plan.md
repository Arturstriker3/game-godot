# Plano — todas as nações (escala mundial, 2026)

> Status: **plano** (nada implementado além do que já está no `GAME_STATE.md`).
> Escopo: dados de nações/cidades/bandeiras, orçamento de escala e critérios de aceite.
> Fora de escopo: UI nova, modelos 3D, reescrita do núcleo econômico.
>
> Base de escala: os números de performance desta onda (§2) já foram medidos; este
> documento é o contrato de **dados** para levar o mesmo mundo a todas as nações.

---

## 1. Objetivo

Hoje o jogo roda um cenário de **8 nações / 40 cidades** (`data/definitions/scenario/nations.csv`,
`cities.json`). A meta é o mundo completo, em três fases, sem quebrar:

1. **determinismo** — mesmo save, mesmo resultado;
2. **conservação** — dinheiro/estoque/preço (invariantes em runtime);
3. **orçamento de frame** — o dia não pode aparecer no frame (tick em worker, §2);
4. **i18n** — pt-BR, en, es para todo nome novo;
5. **honestidade** — onde não há dado publicado, o campo fica vazio e a UI mostra ausência,
   nunca um número inventado.

---

## 2. Auditoria de escala (2026)

Medições registradas no `GAME_STATE.md` §13.3–13.4 (`tools/world_scale_bench.tscn`,
`tools/ai_scale_bench.gd`, `tools/perf_day_peak.gd`; mundo sintético de ~190 nações /
650 cidades, DLL debug, headless):

| sistema | antes | depois | onde |
|---|---|---|---|
| tick C++/dia (650 cidades) | 87,3 ms | **27,0 ms** (p95 34,4) | §13.4 |
| fase `status` do tick | 62,3 ms | **10,8 ms** | §13.4 |
| IA — trabalho/dia (650) | 441,9 ms | **8,7 ms** | §13.3 |
| IA — pico de frame | 29,2 ms | **1,7 ms** | §13.3 |
| mapa (`refresh` + território) | 4,5 s | **0,12 s** | §13.2 |
| memória do mapa | +525 MB | **+18 MB** | §13.2 |
| memória do status (double buffer) | — | +41 MB (650 cidades) | §13.4 |
| `perf_day_peak` (cenário real, 10 d) | — | tick 4,4 ms · pico de frame 5,4 ms | §13.3 |

Leitura: o que impedia a escala era **custo por cidade**, não o número de cidades. Com
tick ~27 ms, status ~11 ms e IA ~9 ms/dia em 650 cidades, o mundo completo cabe no
orçamento; o que falta é **dado** (nações, cidades, bandeiras, traduções).

### 2.1 Estado dos dados hoje (verificado no repositório)

- `nations.csv`: **8 nações** (GBR, FRA, DEU, NED, BEL, VEN, SUR, GUY), colunas `id,name,flag`.
- `nations.csv` chama **GBR de "England"** — errado: o id ISO3 é do Reino Unido.
- `cities.json`: **40 cidades**; `ports.json` correspondente.
- `assets/flags/`: **167 PNGs** (ISO2 minúsculo). Para ~190 nações faltam **33** (levantadas
  na auditoria; a lista é reemitida pelo script de verificação do §7.1).
- `nations.json` é o espelho carregado pelo jogo; `validate_scenario_data.py` cruza ids de
  cidade/porto com as chaves `city.name.*`/`port.name.*` do i18n.

---

## 3. Fontes de dados (todas publicadas, nada estimado)

| campo | fonte | observação |
|---|---|---|
| população do país | **UN World Urbanization Prospects 2026** + **World Bank WDI** (`SP.POP.TOTL`) | usar o ano-base do arquivo, registrado em `_meta` |
| PIB PPP | **World Bank WDI** (`NY.GDP.MKTP.PP.CD`) | bilhões de US$ PPP correntes |
| população urbana / aglomerações | **UN WUP** + **Demographia World Urban Areas** | decide a população das cidades, não só a contagem |
| geometria / fronteiras / nomes | **Natural Earth** (admin-0) | mesma base do território atual |
| capital, moeda, governo | Natural Earth (`capital`, `iso_a3`) + CIA World Factbook (moeda/governo) | sem invenção: campo vazio quando a fonte não cobre |

Toda linha do `nations.csv` deve poder ser rastreada até a fonte. O arquivo ganha
`_meta` no `.json` (ano-base, versão da Natural Earth, URL/hash do download).

---

## 4. Regra de cidades (contrato)

Para cada nação:

```
cidades = 1 (capital, sempre)
        + clamp( 1 + floor(população / 25 000 000) + floor(PIB_PPP / 1,5 × 10¹²), 1, 12 )
```

- **Capital** é obrigatória e não entra no clamp; é a cidade-sede do governo.
- O termo extra cresce com população **e** com PIB PPP — um país populoso e pobre não
  recebe o mesmo que um populoso e rico.
- Faixas resultantes (verificação automática no §7):
  - **potências: 6–12** cidades (EUA, China, Índia, Alemanha, Japão…);
  - **médios: 2–5** (França, Reino Unido, Brasil, México, Coreia…);
  - **pequenos: 1–2** (Islândia, Luxemburgo, Cabo Verde…).
- País sem dado de população ou PIB PPP: **1 cidade (capital)** e a linha marcada
  `data_quality:"missing"` — o jogo mostra o estado vazio, não inventa contagem.
- As cidades vêm de UN WUP/Demographia por aglomeração; quando a fonte tem mais
  aglomerações que o teto, entram as de maior população. Nenhuma cidade é inventada
  (nem "capital administrativa" fictícia): se a lista acaba, a nação fica com menos.

Exemplo de aplicação (valores **ilustrativos**, arredondados só para mostrar a conta;
o arquivo usa os valores da base):

| nação (exemplo) | pop | PIB PPP | conta | cidades |
|---|---|---|---|---|
| potência | 1 400 M | 35 T | 1 + clamp(1+56+23, 1, 12) | **12** (teto) |
| médio | 68 M | 4,2 T | 1 + clamp(1+2+2, 1, 12) | **5** |
| pequeno | 0,6 M | 0,09 T | 1 + clamp(1+0+0, 1, 12) | **2** |

---

## 5. Schema de `nations.csv`

Hoje: `id,name,flag`. Alvo:

```csv
id,name,flag,color,capital,currency,government,ai_profile,region,population,gdp_ppp_bn
DEU,Germany,de,#2f6f4f,Berlin,EUR,republic,industrial,europe,84700000,5900
GBR,United Kingdom,gb,#3b4f8f,London,GBP,monarchy,maritime,europe,69200000,4200
```

| coluna | tipo | regra |
|---|---|---|
| `id` | ISO3 maiúsculo | chave primária; igual ao `nation` das cidades/depósitos |
| `name` | texto | nome em inglês (fonte Natural Earth); tradução vive no i18n (§6) |
| `flag` | ISO2 minúsculo | `assets/flags/<flag>.png`; ausência = sem bandeira, nunca placeholder inventado |
| `color` | `#RRGGBB` | cor do mapa político; única por nação vizinha |
| `capital` | texto | nome em inglês da capital (i18n `city.name.<CAPITAL>`) |
| `currency` | ISO 4217 | moeda corrente |
| `government` | enum | `republic`, `monarchy`, `federation`, `city_state`, `other` |
| `ai_profile` | enum | `industrial`, `maritime`, `resource`, `agrarian`, `balanced` (perfil da IA econômica) |
| `region` | enum | `europe`, `north_america`, `south_america`, `africa`, `middle_east`, `asia`, `oceania` |
| `population` | inteiro | habitantes (ano-base em `_meta`) |
| `gdp_ppp_bn` | número | US$ PPP bilhões, 1 casa |

`nations.json` continua sendo o espelho carregado pelo jogo (`tools/build_scenario_data.py`
gera os dois). Campos novos entram como **opcionais** no leitor: save antigo continua válido
(o contrato de save/load é `kDemoSaveVersion`, ver §6.2 do `GAME_STATE.md`).

---

## 6. Bandeiras e traduções

- **33 ISO2 sem PNG** em `assets/flags/` (auditoria 2026). Fonte: a mesma das 167 atuais
  (bandeiras ISO2). Regra de aceite: **0 faltantes** para as nações do `nations.csv`.
- **GBR**: `name` passa de `England` para `United Kingdom`; a chave nova
  `nation.name.GBR` entra em pt-BR ("Reino Unido"), en ("United Kingdom") e es ("Reino Unido").
- Toda nação ganha `nation.name.<ISO3>` nos três idiomas (as bandeiras/cores não mudam com o
  idioma; o layout não pode quebrar em pt-BR, o mais longo).
- Reaproveitar `tools/build_i18n_csv.py`; `validate_scenario_data.py` passa a exigir a chave
  de cada nação (hoje ele só valida cidade/porto).

---

## 7. Fases

### F0 — base (pré-requisito, sem dado novo)
1. schema §5 no `nations.csv` + `tools/build_scenario_data.py` + leitor opcional;
2. `GBR` → `United Kingdom` + i18n `nation.name.*`;
3. script `tools/check_nations.py` (§7.1) e lista de bandeiras faltantes.

### F1 — Europa + G20 (≈ 60 nações / ≈ 250 cidades)
Europa completa + EUA, China, Índia, Japão, Coreia, Indonésia, Brasil, México, Canadá,
Austrália, Turquia, Arábia Saudita, África do Sul, Argentina. É a fase que fecha o
"mundo jogável" com o cenário atual (Europa) já validado.

### F2 — Américas + Ásia
América Latina e Caribe, Sudeste Asiático, Sul da Ásia e Ásia Central, Oceania.

### F3 — resto
África subsaariana, Oriente Médio restante, ilhas e territórios ultramarinos.
Cada fase: dados → validação → balanceamento → ajuste de cidades.

---

## 8. Critérios de aceite (mensuráveis)

| # | critério | como medir |
|---|---|---|
| A1 | toda nação tem capital no próprio território (Natural Earth) e 1–12 cidades | `tools/check_nations.py` |
| A2 | contagem por faixa: potências 6–12, médios 2–5, pequenos 1–2 | `tools/check_nations.py` (recalcula a fórmula §4) |
| A3 | 0 bandeiras faltantes; 0 ids sem `nation.name.*` nos 3 idiomas | `tools/check_nations.py` + `validate_scenario_data.py` |
| A4 | save/load round-trip idêntico (`JSON.stringify` igual) | `tools/economy_integration_test.gd` |
| A5 | conservação: 0 violações e falências ≤10 em 730 dias | `tools/balance_review.gd --days=730` |
| A6 | comida: 100% das nações ≥70% de satisfação em 730 dias | `balance_review` (hoje 8/8 ≥98% com 40 cidades) |
| A7 | tick ≤60 ms com 650 cidades e nenhum dia >150 ms com 200 | `tools/world_scale_bench.tscn` |
| A8 | determinismo: tick síncrono = assíncrono (10 dias) | `world_scale_bench --determinism=10` (`identical=true`) |
| A9 | virada do dia sem pico >12 ms no cenário real | `tools/perf_day_peak.tscn --days=10` |
| A10 | memória: status ≤+45 MB (650) e mapa ≤+20 MB | `world_scale_bench` / `ui_capture` |
| A11 | i18n: captura sem corte nos 3 idiomas | `tools/ui_capture.tscn` |

---

## 9. Riscos e não-objetivos

- **Balanceamento é por cenário:** 60 nações mudam a competição por alimento/minério. A5–A6
  são o portão de cada fase; não se ajusta a regra econômica para "fazer passar" um dado.
- **Dados ausentes:** país sem PIB PPP publicado fica com 1 cidade e `data_quality:"missing"`
  — a UI declara a ausência; não se preenche com estimativa.
- **Não-objetivos:** refazer o mapa 3D, novos modelos de veículo, UI nova, refino industrial.
- **Teto duro:** 12 cidades por nação é orçamento de simulação + leitura de mapa; subir o teto
  exige novo bench (A7/A9) antes.

---

### 9.1 Como verificar este plano (sem rodar o jogo)

```powershell
# 1. schema e contagem (A1–A3) — script do F0
python tools/check_nations.py
# 2. dados de cenário (ids de cidade/porto + i18n)
python tools/validate_scenario_data.py
```

Nenhuma medição deste documento foi estimada: as de §2 vêm do `GAME_STATE.md` §13.2–13.4
e dos benches em `tools/`; as de §2.1 foram contadas nos arquivos do repositório.
