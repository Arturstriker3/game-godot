# Parte G2 — armazenagem, reserva nacional e mapeamento de armazém (dados)

> Insumo da **PARTE G2** de `docs/plans/perf_pop_financas_plano.md` (§"PARTE G2 —
> Armazenagem, reserva nacional e camada nacional"). Entrega **só de dados**: nenhum
> código de núcleo, GDScript, `.tscn` ou UI. O jogo começa em **2027-01-01**.
>
> Branch `data/g2-storage`, worktree `G:/projetos/opengs-wt-data-g2`, base
> `balance/global-resources` `c17fb411`. Integração no núcleo (capacidade inicial por
> país, reserva do governo, IA privada de armazém) é trabalho do `@gerente-simulacao`.

## 1. Arquivos

| arquivo | conteúdo | entradas |
|---|---|---|
| `data/definitions/economy/storage_capacity.json` | capacidade de grãos (Mt), câmara fria (m³) e armazenagem de gás (TWh) por nação | 179 nações |
| `data/definitions/economy/strategic_reserves.json` | estoque público de petróleo (kt e dias), estoque/capacidade pública de grãos (t) e reservas de ouro (t) | 179 nações |
| `data/definitions/economy/goods_storage_map.json` | os 113 bens de `goods.json` → tipo de armazém | 113 bens |
| `tools/storage_g2_reference.json` | tabela curada dos valores publicados, com trecho literal e URL por fonte | 57 valores / 18 fontes |
| `tools/fetch_storage_g2.py` | coleta bruta (IMF IFS, Eurostat, EIA) com cache + SHA-256 e data de acesso | `--fetch` |
| `tools/build_storage_g2.py` | constrói os três JSONs (regras, conversões, cobertura) | — |
| `tools/validate_storage_g2.py` | validador (**exit 0**) | — |

Todo valor carrega `basis ∈ {observed, rule, none}`, a fonte (`source`) e o ano/período.
`observed` = publicado; `rule` = regra declarada em `_meta.rules`, reproduzível; `none` =
sem dado, com motivo escrito em `note` — nunca há número inventado.

## 2. Cobertura

| bloco | observado | regra | sem dado |
|---|---|---|---|
| capacidade de grãos (Mt) | **7** países (USA, BRA, CHN, UKR, KAZ, RUS, FRA) | **163** (k × produção anual) | 9 (sem produção FAOSTAT) |
| câmara fria (m³) | **17** países (GCCA 2020) | **162** (k × população) | 0 |
| gás — capacidade de trabalho (TWh) | **22** países (IGU 2025 + GIE + EIA) | — | 157 (regra do jogo: 0) |
| petróleo público (kt / dias) | **39** (Eurostat 34, USA, JPN, KOR, IND, CHN) | — | 140 (regra do jogo: 0) |
| grãos públicos (t) | **3** (IND estoque e capacidade, IDN e PHL capacidade) | — | 176 |
| ouro (t) | **161** (IMF IFS, 194 áreas) | — | 18 (microestados/áreas sem série) |

Totais calculados (derivados, não fonte):

- grãos **3.559 Mt** (observado: 1.899 Mt);
- câmara fria **1,35 bilhão de m³** (observado: 676 milhões de m³);
- gás **4.393 TWh** de capacidade de trabalho — todo observado;
- petróleo público **237 Mt**; ouro **32.840 t**.

## 3. Fontes

| chave | fonte | ano | licença | uso |
|---|---|---|---|---|
| `imf_ifs_gold` | IMF — *International Financial Statistics*, `RAFAGOLDV_OZT` (milhões de onças troy finas) | 2024–2025 | © IMF, citação | reservas de ouro do BC |
| `eurostat_stk_oem` | Eurostat — *Emergency oil stocks in days equivalent* (`STK_EUE_DIR`, `IMP_DNC`, `IC_DC`) | 2025-09/10 | © EU, CC BY 4.0 | estoque público de petróleo na UE/EEE e vizinhos |
| `eia_spr` | EIA — *Weekly U.S. Ending Stocks of Crude Oil in SPR* (`WCSSTUS1`) | sem. de 11/2026 | domínio público | SPR dos EUA |
| `eia_us_gas_capacity` | EIA — *Underground Natural Gas Storage Capacity* (working gas) | 2026 | domínio público | gás dos EUA |
| `eia_tie_67504` | EIA *Today in Energy* — estoques estratégicos de petróleo 2025 (foto dez/2025) | 2025 | domínio público | JPN, KOR, IND, CHN (estoque) |
| `doe_spr_quick_facts` | DOE/OPR — *SPR Quick Facts* | 2025 | domínio público | capacidade do SPR e 125 dias |
| `knoc_facilities` | KNOC — *Operation of Stockpiling Facilities* | 2026 | © KNOC, citação | capacidade pública da Coreia |
| `mopng_spr` | MoPNG (Índia) — *Strategic Crude Oil Storage* | 2026 | © GoI, citação | capacidade do ISPRL (5.330 kt) |
| `usda_nass_grain_stocks_capacity` | USDA NASS — *Grain Stocks* (on-farm + off-farm) | 2025 | domínio público | capacidade de grãos dos EUA |
| `ibge_pesquisa_estoques_2s2025` | IBGE — *Pesquisa de Estoques* 2º sem/2025 (via Canal Rural) | 2025 | © IBGE, citação | capacidade estática do Brasil |
| `nfsra_warehouse_capacity` | NFSRA (China) em coletiva (via Global Times) | 2025 | © Global Times, citação | capacidade de armazéns da China |
| `pib_fci_state_storage_2025` | PIB/FCI (via DD India/Prasar Bharati) | 2025 | © PIB, citação | capacidade pública da Índia |
| `fci_central_pool_stock_jul2025` | FCI — central pool (via iGrain India) | 2025 | © FCI, citação | estoque público da Índia |
| `elevatorist_ukraine_elevators_2025` | Elevatorist/USM — elevadores da Ucrânia | 2025 | © USM, citação | capacidade da Ucrânia |
| `kaz_minagri_storage_2025` | Ministério da Agricultura do Cazaquistão (via APK-Inform) | 2025 | © APK-Inform, citação | capacidade do Cazaquistão |
| `reuters_russia_storage_2026` | Reuters — armazenagem da Rússia | 2026 | © Reuters, citação | capacidade da Rússia (**estimativa**) |
| `cooperation_agricole_silos_2026` | La Coopération Agricole / Process Alimentaire — silos franceses | 2026 | © Process Alimentaire, citação | capacidade da França |
| `bulog_warehouse_capacity_2026` | DPR RI — armazéns do Bulog | 2026 | © DPR RI, citação | capacidade pública da Indonésia |
| `nfa_warehouse_capacity_2025` | NFA (via Philippine Star) | 2025 | © Philippine Star, citação | capacidade pública das Filipinas |
| `gcca_2020` | GCCA/IARW — *2020 Global Cold Storage Capacity Report* | 2020 | © GCCA, citação | câmara fria (17 países) |
| `igu_ugs_2025` | IGU — *Underground Gas Storage* 2025 (WGV em operação) | 2025 | © IGU, citação | gás (20 países) |
| `gie_storage_db_2026` | GIE — *Storage Database* (Storage Map 2026) | 2026 | © GIE, citação | gás da Romênia |
| `faostat_qcl_production` | FAOSTAT QCL, produção de trigo/arroz/milho/soja | 2024 | CC BY 4.0 | base da regra de grãos (reuso de `crop_yields.json`) |
| `wdi_population` | World Bank WDI `SP.POP.TOTL` | 2025 | CC BY 4.0 | base da regra de frio (reuso de `nations_source.csv`) |

Regra de escolha entre fontes conflitantes: **dado nacional oficial > organismo
internacional > associação setorial > reportagem com número atribuído**. Quando a página
oficial bloqueou leitura automática (IBGE, PIB, NASS em parte), o valor veio de uma
reprodução fiel citada na própria entrada (`note` diz qual página foi bloqueada). Nada
disso é usado sem o trecho literal em `tools/storage_g2_reference.json → sources.<chave>.quote`.

## 4. Como cada número foi obtido

### 4.1 Ouro (`strategic_reserves.json → gold`)
IMF IFS `RAFAGOLDV_OZT`, última observação mensal de cada área (194 áreas), consultada via
API pública do DBnomics (`provider IMF, dataset IFS`, cache em
`tools/_tmp_storage_raw/imf_ifs_gold.json`). Conversão exata:
`t = milhões_de_onças × 1e6 × 31,1034768 g / 1e6`. Inclui depósitos de ouro e ouro
emprestado, como a série define. `period` guarda o mês; `stale: true` marca quem não
reporta desde 2024 (ex.: RUS, 2024-06). Áreas do IFS em ISO-2 foram mapeadas para o ISO-3
do jogo (SOL→SB, SSD→SS são aliases explícitos).

### 4.2 Petróleo público (`strategic_reserves.json → oil`)
- **UE/EEE e vizinhos (34):** Eurostat `nrg_stk_oem`. `days_net_imports` =
  `STK_EUE_DIR` do último mês; `public_stocks_kt` = dias × `max(IMP_DNC, IC_DC)` do mesmo
  mês (é a regra da Diretiva 2009/119/CE: o denominador é o maior entre importação líquida
  diária e consumo interno diário). Sem denominador publicado no mesmo mês → `kt` nulo,
  dias preservados. `stale` marca a última observação antiga (GBR, 2020-10).
- **EUA:** série semanal da EIA (`WCSSTUS1`), semana de 13/11/2026: **276.361 mil barris**
  → 37.696 kt a 0,1364 t/bbl. Capacidade 97.390 kt e 125 dias vêm do DOE (*SPR Quick Facts*).
- **JPN, KOR, IND, CHN:** foto de dez/2025 do *Today in Energy* (EIA), convertida de Mbbl.
  CHN é **estimativa** (`estimate: true`) e cobre só estoque governamental estimado, porque a
  China não publica o volume oficial do SPR. Capacidades: KNOC 146 Mbbl (KOR) e ISPRL
  5.330 kt (IND).
- **Demais 140:** `basis: none` com motivo. No jogo, reserva pública inicial = 0 (regra
  declarada em `_meta.rules.oil_public`) até o país criar reserva.

### 4.3 Capacidade de grãos (`storage_capacity.json → grain`)
- **Observado (7):** USA 647,4 Mt (NASS, on-farm 13,6 bn bu + off-farm 11,9 bn bu a
  0,0254012 t/bu); BRA 233,8 Mt (IBGE, capacidade útil 2º sem/2025); CHN 730 Mt (NFSRA,
  piso "exceeded"); UKR 52,67 Mt (elevadores); KAZ 30,7 Mt; RUS 160 Mt (**estimativa**
  Reuters); FRA 44 Mt (rede de silos).
- **Regra (163):** `capacity_mt = k_graos × produção_anual / 1e6`, com
  **k_graos = 1,1007**, a mediana de `capacidade observada / produção observada` nos 7
  países acima (a dispersão vai de 0,842 no Brasil a 1,527 na Rússia; Ucrânia 0,941 e
  Cazaquistão 1,512). Produção = FAOSTAT QCL
  trigo+arroz+milho+soja do último ano ≤2024 (`crop_yields.json`). O campo
  `months_of_production` é derivado e sempre recalculado.
- **Sem dado (9):** BHR, GNQ, ISL, KNA, SAH, SGP, SOL, TWN, XKX — sem produção de grãos na
  base FAOSTAT do jogo; não há regra aplicável.

### 4.4 Câmara fria (`storage_capacity.json → cold`)
GCCA/IARW 2020 (Tabela 1) para 17 países, em m³. `CAN` é estimativa do próprio relatório.
Para os demais, `capacity_m3 = k_frio × população`, com **k_frio = 0,1698 m³/habitante**,
mediana dos 17 observados (0,042 na África do Sul a 0,83 nos Países Baixos).

### 4.5 Gás (`storage_capacity.json → gas`)
Capacidade de trabalho (working gas volume) em operação: IGU *Underground Gas Storage 2025*
(20 países, bcm convertidos a TWh a 11,0 TWh/bcm), GIE *Storage Database 2026* (Romênia) e
EIA (EUA, 4.793.009 MMcf = 1.493 TWh). Os 157 restantes ficam `none` com motivo: a AGSI+
exige chave de API e só cobre a Europa; não há base global pública além do IGU. No jogo,
capacidade inicial 0 até construir armazém de gás.

## 5. Mapeamento bem → armazém (`goods_storage_map.json`)

Segue a tabela do plano (§PARTE G2), com dois armazéns especiais (cofre e paiol militar):

| armazém | onde | bens (primário) |
|---|---|---|
| `silo` | região + porto | trigo, arroz, milho, soja, farelo, ração, farinha — 7 |
| `cold_room` | cidade + porto | carne, batata, vacinas — 3 |
| `fuel_tank` | região + porto | petróleo, combustível, etanol, químicos — 4 |
| `gas_storage` | região | gás — 1 |
| `bulk_yard` | região (mina) + porto | carvão, carvão vegetal, minérios (ferro, bauxita, cobre, chumbo, tungstênio), fosfato, enxofre, madeira, algodão, serragem, fertilizantes — 14 |
| `vault` | cidade | ouro (bruto e em barra), diamante, urânio, terras raras (brutas e processadas), joias — 7 |
| `military_depot` | economia de guerra | pólvora, explosivos, estojos, projéteis, munições (leve e artilharia), armas leves, artilharia, drones militares, equipamento de infantaria, ração de combate — 11 |
| `general_warehouse` | cidade | todo o resto — 66 (semiprocessados e finais) |

Cada bem aceita armazém **secundário** quando o plano permite (ex.: `flour` em silo ou
armazém geral; `vaccines` em câmara fria ou armazém; `canned_food` só armazém geral). O
campo `reason` explica a escolha caso a caso; 64 bens não têm regra própria e caem em
`general_warehouse`, listados em `coverage.fallback_general_warehouse`.

## 6. Lacunas conhecidas

- **Grãos:** Canadá, Argentina, Austrália, México, Turquia, Polônia, Espanha, Itália e
  Alemanha ficaram na regra — StatCan/MAGyP/ABARES não entregaram número legível (403/PDF
  sem total nacional; no Canadá, a capacidade licenciada dos elevadores ~8 Mt subestimaria
  muito e foi descartada de propósito).
- **Índia (viés conhecido):** a capacidade **nacional** não é publicada, só a do setor
  público. A regra dá 425,4 Mt para uma produção de 386,5 Mt; como a rede pública cobre
  91,8 Mt, a regra quase certamente sobrestimou — trate como teto, não como medição. O
  número público real está em `grain_public.capacity_t`.
- **Brasil:** adotado 233,8 Mt (IBGE, 2º sem/2025). O release seguinte do IBGE (1º sem/2026)
  informa 237,6 Mt (+1,7%), 1,6% acima; ficou registrado no `note` da entrada.
- **Frio:** a série é de 2020; GCCA 2022/2024 são só para membros e o IIR bloqueia a
  tabela. Rússia e Argentina não têm entrada.
- **Petróleo:** IEA (*Oil stocks of IEA countries*) responde 403; AUS, CAN, MEX, NOR, CHE,
  TUR e NZL ficaram sem entrada. Dias de importação para JPN/KOR/IND não localizados.
- **Gás:** fora do IGU não há publicação; China usa o WGV do IGU (19,83 bcm), não o número
  maior que circula em reportagens.
- **Ouro:** 18 áreas sem série no IFS (microestados, PRK, SDN, SAH).

## 7. Reconstruir e validar

```powershell
# coleta bruta (cache em tools/_tmp_storage_raw/, ignorado pelo git, apagável)
& "C:\Users\artur\python-sdk\python3.13.2\python.exe" tools/fetch_storage_g2.py --fetch
# constrói os três JSONs
& "C:\Users\artur\python-sdk\python3.13.2\python.exe" tools/build_storage_g2.py
# valida (179 nações, 113 bens, valores >= 0, cobertura e totais recalculados)
& "C:\Users\artur\python-sdk\python3.13.2\python.exe" tools/validate_storage_g2.py
```

O validador recusa: nação faltando/sobrando, `basis` incoerente, valor negativo, `rule`
que não reproduz a fórmula, cobertura/totais que não batem, `observed` sem fonte, ouro que
não fecha a conversão de onça, bem sem armazém e armazém inválido.

## 8. Decisões e conflitos

1. **EUA — gás:** IGU dá 138,09 bcm (1.519 TWh) e a EIA 4.793.009 MMcf (1.493 TWh).
   Vale a EIA (fonte nacional e mais recente); o desvio de 1,7% fica registrado aqui.
2. **EUA — petróleo:** a foto de dez/2025 (413 Mbbl) foi descartada em favor da série
   semanal de nov/2026 (276 Mbbl), que é a mais recente e a mesma que o jogo veria.
3. **Brasil — grãos:** usado o IBGE (*Pesquisa de Estoques*, capacidade útil) e não o
   número da Conab, por ser a estatística oficial de capacidade estática do país; a Conab
   não publicou total nacional comparável no período.
4. **Índia — grãos:** capacidade e estoque são do *central pool* (FCI + agências
   estaduais), não da capacidade privada — o número é público, não total nacional.
5. **Turquia — petróleo:** a série da Eurostat reporta 0 dias de estoque de emergência;
   o 0 é preservado como observado, não como ausência de dado.
6. **Regra de grãos ancorada na amostra:** k = mediana dos 7 observados, não um número
   escolhido a dedo. Isso deixa a capacidade mundial em ~3,6 Gt, compatível com a soma dos
   grandes publicadores; países sem publicação ficam no mesmo patamar proporcional.
7. **Sem mercado nacional:** esta entrega é só leitura (capacidade e reserva por nação),
   como o plano exige; nenhum preço ou fluxo nacional foi criado.
