# E2/E3 — Usinas, geração, rede e preço da energia (D-116)

Escopo: **ligar ao núcleo** o dado de energia do E1 (D-115) e implementar as etapas
E2 (usinas e geração) e E3 (rede, despacho e preço) do plano
`docs/plans/perf_pop_financas_plano.md` §PARTE E. Branch `feat/e2e3-power-grid`,
worktree `G:/projetos/opengs-wt-e2e3`.

O que **não** entrou nesta fase (declarado): E4 (demanda por estrato/indústria detalhada —
a demanda é agregada por cidade e renda), E5 (interligação internacional e obra de
redução de perdas), E6 (modo de mapa) e E7 (distribuidora como 21º serviço). A E5 já
tem a regra de conexão da cidade (conectada/parcial/isolada) e o dado de perdas; a
venda entre nações vizinhas fica para a fase seguinte.

## 1. O que entrou

### E2 — Usinas e geração

- Usina = entidade do núcleo (`PowerPlant`) no **interior** da cidade, com dono
  (governo da cidade ou grupo F9), capex, O&M, empregos e salário; obra física pelo §15
  (`build_power_plant`, 180 dias por padrão).
- Fontes com o nome canônico do núcleo: `hydro`, `solar`, `wind`, `coal`, `gas`, `oil`,
  `nuclear`, `biomass`, `other`.
- **Teto por cidade e por fonte**: renovável = potencial do mapa (hídrico do
  `hydro_potential.json`; solar/eólico = área × conversão declarada em
  `power_params.json`); térmica = parque × `thermal_cap_multiple` (modelo declarado,
  porque não existe potencial geográfico de carvão/gás). Sem teto declarado, a cidade
  não inventa local novo (teto = parque atual).
- **Fator de capacidade** por fonte; o solar pode ser sobreposto pelo **PVOUT mensal**
  do Global Solar Atlas (`solar_month`, 12 valores). Vento não tem série mensal na
  fonte (dívida declarada do E1).

### E3 — Rede, despacho e preço

- **Uma rede por nação** (`PowerGridState`), com lista de membros:
  - **conectada** — participa do pool nacional; não tem limite de troca;
  - **parcial** — troca com a rede até `line_mw` (geração local + importação até a
    linha); o resto é local;
  - **isolada** — roda só com as usinas da própria cidade.
  A nação só tem operador nacional se o acesso à eletricidade do dado (WDI) for
  ≥ `operator_access_pct`; a nota 0,65·urbano + 0,35·rural decide conectada/parcial.
- **Despacho por mérito**: custo marginal da usina = combustível (preço de mercado do
  bem, com prêmio de 1,5× quando a compra é importada) + O&M. A **última usina ligada
  define o preço** da rede, em XGS/MWh, com piso e teto de escassez declarados.
  Falta de geração → preço no teto de escassez.
- **Eletricidade não é estocável**: balanço diário por rede, sem estoque.
- **Perdas T&D**: base do dado por país, reduzida pela infraestrutura de energia do B5,
  com **piso técnico > 0** (4 % por padrão) — obra nunca zera a perda.
- **Racionamento** (fração servida < 0,995) reduz a **indústria** (quase proporcional à
  fração servida, com piso de 5 % de geração própria/gerador), os **serviços** (piso de
  50 %), o **emprego** (as vagas industriais e de serviço seguem os mesmos fatores) e a
  **satisfação** (o `stress` cresce com a duração do blecaute em dias seguidos). O
  **PIB perdido** usa o VOLL declarado (múltiplo do preço da rede).
- **Combustível**: cada MWh térmico consome `fuel_t_per_mwh[fonte]` do mercado local;
  o que não houver no estoque é importado com prêmio. O combustível comprado **sai do
  estoque do mercado** e entra na demanda do dia.
- **Liquidação financeira (soma zero)**: famílias, firmas, serviços e governo pagam a
  conta do servido; o pool paga combustível (firmas locais ou exterior), O&M, salários e
  o lucro ao dono. O caixa agregado do setor (`power_cash`) entra no `money_total` e
  fecha o dia em zero.
- **IA**: onde o racionamento persiste (`build_ration_days`), o Estado abre obra de usina
  na cidade conectada de maior população com folga de potencial (cooldown declarado).

## 2. Dados e atribuição (E1/D-115)

| Arquivo | Uso no núcleo | Fonte / licença |
|---|---|---|
| `access_losses.json` | acesso total/urbano/rural e perdas T&D por país | Banco Mundial, WDI — CC BY 4.0 |
| `power_plants_2026_scaled.json` | parque instalado por cidade e fonte (MW) | WRI GPPD v1.3.0 (CC BY 4.0) escalonado à matriz Ember 2024–2025 (CC BY 4.0) |
| `renewable_potential.json` | área para o teto solar/eólico e PVOUT mensal (solar) | Global Solar Atlas / Global Wind Atlas — CC BY 4.0 |
| `hydro_potential.json` | potencial hídrico por cidade | CHIRPS (CC BY 4.0), PET CHELSA v2.1 (CC0 1.0), ETOPO1 (domínio público), rios Natural Earth |
| `power_params.json` | parâmetros de MODELO (não são dado observado) | espelho declarado de `sim/power.h` |

Créditos de tela (i18n pt-BR/en/es): chaves `credits.energy.*` em
`core/i18n/i18n.gd` — Ember, WRI GPPD, Banco Mundial, Global Solar Atlas, Global Wind
Atlas, CHIRPS, CHELSA, ETOPO1 e Natural Earth, com a licença de cada uma.

Nenhum número de dado foi inventado: cidade sem linha nos arquivos fica
`has_data = false` e **não** recebe blecaute fabricado (a ausência é declarada).

## 3. Parâmetros de modelo (declarados)

Todos em `data/definitions/energy/power_params.json` (`params`), espelhados em
`sim/power.h`; o teste `tools/e2e3_power_test.gd` confere o espelho contra o binário.

- Demanda: kWh/pessoa/dia no índice de renda 1,0, elasticidade-renda e teto do índice;
  participação famílias/indústria/serviços/outros (soma 1).
- **Escala da economia**: a demanda, as contas e os custos usam a **população
  simulada** (aglomerações), a mesma que a economia precifica; o parque do dado é
  nacional (população exibida) e é multiplicado, por nação, pela fração
  simulada/exibida, para MW e dinheiro viverem na mesma escala. Sem simulação
  (fixture), a fração é 1 e o parque é exatamente o do dado. A geração medida contra
  a Ember é, portanto, a **fração simulada** do total nacional — declarado no §6.
- Capacidade: fatores por fonte (anual), conversão área→MW solar/eólico, múltiplo do
  teto térmico.
- Rede: acesso mínimo de operador, cortes urbano/rural, fração e mínimo da linha parcial.
- Preço: escala, piso e teto de escassez.
- Falta: pisos de indústria, geração própria e serviços; sensibilidade e prazo do
  `stress`; limiar de racionamento; VOLL.
- Custo de obra (XGS/MW) e prazo por fonte; custo e prazo da linha.

## 4. Save v35

- Bloco aditivo `power`: caixa, regra (`policy` = parâmetros), estado por cidade
  (conexão, linha, acesso, perdas, teto, PVOUT mensal, demanda/servido/preço/racionamento
  e efeitos do dia), usinas (incluindo **geração do dia**) e a rede por nação.
- O bloco só é gravado quando o mundo tem energia semeada (`power_ready`); um mundo sem
  energia não grava o bloco, então salvar→carregar→salvar tem o **mesmo hash**.
- `PowerService.apply_to_sim(..., {"only_missing": true})` não sobrescreve o estado que
  veio do save; save v34 e anterior chega sem o bloco e o PowerService semeia o parque.
- Arquivo de teste: `tools/e2e3_power_test.gd` (round-trip bit-exato + continuação).

## 5. Testes desta fase

`tools/e2e3_power_test.gd` cobre, no fixture AGRIA/PORTUS:

1. mérito e preço marginal (hidro entra primeiro; carvão vira marginal e define o preço);
2. racionamento derruba a indústria de forma quase proporcional e sobe o desemprego;
3. blecaute longo pesa mais na satisfação (o `stress` cresce com a duração);
4. o PIB contrai e o PIB perdido (VOLL) aparece;
5. o combustível da térmica sai do mercado e entra no consumo do dia;
6. conservação de dinheiro e soma financeira zero do setor;
7. cidade isolada × conexão parcial × conectada (limite da linha);
8. obra de linha muda o estado para conectada;
9. save v35 round-trip bit-exato e `only_missing` não re-semeia;
10. determinismo (dois mundos iguais → mesmo status de energia);
11. ausência declarada não vira blecaute.

Dois defeitos de integração foram encontrados e corrigidos por este teste:

- **folha de serviços**: o núcleo cobrava a folha cheia mesmo com a energia reduzindo as
  vagas de serviço (`power_service_factor`), destruindo dinheiro e revertendo o tick por
  violação de conservação (`labor.cpp` corrigido: a folha segue a energia servida);
- **save do dia**: o bloco de energia não guardava a geração do dia nem os efeitos por
  cidade, e o load criava `power_cities` mesmo sem bloco — o round-trip das C1–C6
  divergia. O save passou a guardar os campos e a não inventar estado (`save_state.cpp`,
  `sim/save/power.cpp`).

Regressão coberta: `economy_save_load_roundtrip_test`, `economy_integration_test`,
`c3_bank_credit_test`, `c4_banks_test`, `c5_stock_exchange_test`, `c6_bank_crisis_test`
(pinos de versão de save v34→v35), `market_template_test` e `goods_data_test`.

## 6. Rodada 365 dias (evidência)

<!-- RODADA_365 -->
**Nota (QA t80):** os números desta seção são a rodada **sem E5** (âncora),
preservada em `docs/economy/perf/e2e3_anchor_365.json`; o arquivo canônico
`c1_world_ai_365_e2e3.json` foi regerado com o E5 e o dado de interconexões do
§7 (109/179 e 5 falências) — a seção 7 é a leitura corrente.
Rodada canônica: `godot --headless --path . --script res://tools/c1_world_ai_365.gd --
--days=365 --out=res://docs/economy/perf/c1_world_ai_365_e2e3.json`
(365 d, threads=1, ~240 s, saída `docs/economy/perf/c1_world_ai_365_e2e3.json`).
"Antes" = `docs/economy/perf/c1_world_ai_365.json` (mundo sem energia ligada, medido
antes da E2/E3). O controle **no mesmo commit** da E2/E3 está no §6.1 (2 falências,
93/179): a referência antiga é de um commit anterior e não isola só a energia.

| Métrica | Antes (sem energia) | Depois (E2/E3) | Portão |
|---|---:|---:|---|
| Dia final / travados | 365 / 0 | 365 / 0 | — |
| Violações de invariante | 0 | 0 | 0 |
| Falências | 2 | **5** | ≤ 2 — **REPROVA** |
| Portão de comida (abaixo de 0,93) | 86/179 | **124/179** | ≤ 93 (meta ≤ 86) — **REPROVA** |
| Comida média / mediana | 0,9094 / 0,9778 | 0,8481 / 0,9105 | — |
| Pior nação (avg30) | SLB 0,159 | PSX 0,190 | — |
| Digest D-105 (`trajectory_sha256`) | `f759f4c0…` | `cfff05de…` | — |

Energia (ano; agregados do mundo simulado):

| Métrica | Valor |
|---|---:|
| Preço médio ponderado pelo servido | 6,46 XGS/MWh |
| Demanda servida (média do ano) | 94,1 % (mínimo diário 93,5 %) |
| Cidades racionando (pico) | 348 |
| Redes com racionamento no fecho | 69 de 148 |
| Cidades conectadas / parciais / isoladas | 694 / 48 / 0 (93,5 % conectadas) |
| Perdas T&D (média ponderada) | 5,92 % |
| Geração no dia final (TWh/dia) | gás 9,03 · hidro 4,72 · vento 3,28 · carvão 2,81 · solar 2,60 · nuclear 0,37 · other 0,50 · biomassa 0,18 · óleo 0,16 |
| Geração total (dia final) | 23,6 TWh/dia ≈ 8,6 PWh/ano = **fração simulada** (a economia modela as aglomerações, não o mundo exibido) |

Top 5 por energia servida × capacidade Ember (capacidade não é geração; comparação declarada):

| Nação | Servido (MWh/dia) | Fração servida | Preço (XGS/MWh) | Ember (MW, ano) |
|---|---:|---:|---:|---:|
| USA | 4.323.288 | 1,000 | 0,253 | 1.333.250 (2025) |
| CHN | 3.818.896 | 1,000 | 0,228 | 3.726.510 (2025) |
| JPN | 1.514.615 | 1,000 | 0,253 | 322.910 (2025) |
| RUS | 952.491 | 1,000 | 0,300 | 237.470 (2025) |
| KOR | 828.150 | 1,000 | 0,253 | 158.880 (2025) |

**Resultado: o portão de comida e as falências REPROVAM.** A rodada foi parada aqui para
diagnóstico, sem merge/push:

1. **Racionamento estrutural.** O parque do dado × fator de capacidade cobre o mundo
   (113M MWh/dia de disponibilidade para 78M de demanda), mas **85 nações** têm
   disponibilidade nacional menor que a própria demanda e **não há comércio entre redes**
   (E5 não implementada). Nações sem parque no dado (TLS, PSX, YEM, SOM…) e redes
   pequenas/importadoras (BEL, CHE, DNK, DEU, GBR, NED…) racionam o ano inteiro, com
   preço no teto de escassez.
2. **O canal do portão é a consequência declarada.** O fator de serviço/indústria
   reduz vagas e folha (~3–5 % de renda) e a conta de energia no preço de escassez pesa
   no orçamento das famílias; 38 nações cruzam o limiar de 0,93 (AFG, MDG, CIV, MMR,
   NPL, BGD, TZA, KEN, MOZ, IND, BOL… e AUS, RUS, BHR, GNQ). O mesmo canal explica as
   3 falências extras.
3. **Bugs de integração já corrigidos** (não são amolecimento de consequência):
   - **escala da economia**: parque e demanda passaram a usar a população simulada
     (antes a conta era cobrada de 8,2 bi de pessoas com dinheiro de 1,27 bi — 6,5×);
   - **repasse industrial**: o custo de energia por unidade de valor chegava a **740×**
     o preço base (Jacarta) porque a conta de 42 % da demanda total era dividida pelo
     valor de poucas plantas locais; agora é limitado por
     `industry_cost_share_max` = 0,05 (5 % do preço base, declarado).
4. **Não foi amolecido**: preço de escassez, fatores de indústria/serviço, insatisfação
   por duração e VOLL permanecem. Reduzir o teto de preço ou os fatores faria o portão
   passar por decreto, contra a regra da fase.
5. **Próximo passo recomendado antes de reabrir o portão:** E5 (interligação entre redes
   vizinhas com fronteira terrestre, `land_borders.json` já no repositório) e/ou E7
   (tarifa regulada da distribuidora), que são as alavancas que o plano prevê para a
   falta. Sem elas, o mundo base não fecha o portão com energia ligada.

### 6.1 Calibração da demanda (pós-QA t67) — medida, portão ainda REPROVA

A curva de demanda foi reajustada por dado, não por chute (§3 e
`meta.demand_calibration` em `power_params.json`): OLS log-log da demanda Ember por
pessoa/dia contra o índice de renda, descontando acesso e perdas T&D (os termos que
`sim/power/grid.cpp` aplica). n=135, R²=0,681, expoente **0,912238** e âncora
**7,93828 kWh/pessoa/dia** (eram 9,0 e 1,0). Foram excluídas 40 nações com Ember
< 1,37 kWh/pessoa/dia (cobertura fraca), com regra e lista no JSON; não há série
pública de clima/indústria no repositório independente da renda (o emprego
industrial das demais nações é estimado a partir do próprio índice), então não
entrou termo adicional. Preço de escassez, VOLL, fatores de produção, insatisfação,
perdas e racionamento **não** mudaram.

Rodada 365 no mesmo commit, adicionando a curva nova (e controle sem energia):

| Métrica | Sem energia (controle) | Curva antiga | Curva calibrada | Portão |
|---|---:|---:|---:|---|
| Falências | 2 | 5 | **3** | ≤ 2 — REPROVA |
| Violações de invariante | 0 | 0 | 0 | 0 |
| Portão de comida (abaixo de 0,975) | 93/179 | 124/179 | **124/179** | ≤ 93 (meta ≤ 86) — REPROVA |
| Preço médio ponderado (XGS/MWh) | — | 6,46 | 4,67 | — |
| Demanda servida (média / mínima diária) | — | 94,1 / 93,5 % | 96,2 / 95,9 % | — |
| Redes com racionamento no fecho | — | 69/148 | 56/148 | — |
| Demanda total (TWh/dia) | — | 25,2 | 21,3 | — |
| Demanda modelada / Ember (mediana; p90, 147 redes) | — | 1,21; 2,23 | 1,08; 1,97 | — |
| Nações com disponibilidade < demanda | — | 87 | **76** | — |

O controle sem energia (mesmo commit, `PowerService` desligado) dá 2 falências e
93/179 — ou seja, o gate já está no limite do regulamento sem energia, e a E2/E3
acrescenta 1 falência e 31 nações. Das 36 nações que cruzam o portão com a curva
nova, 30 têm racionamento (rede nacional ou cidade isolada por acesso < 60 %), 5
têm rede sem racionamento e cruzam por efeito geral com perda ≤ 0,05, e SAH (sem
dado de energia) cruza só por efeito geral (0,32).

Diagnóstico (não é amolecimento): a curva de 2 parâmetros corrige a mediana, mas
não os outliers de renda alta e consumo baixo — GBR 1,57×, SGP 1,33× e DEU 1,30×
o Ember; no mesmo PIB PPP pc o consumo real varia de 12 a 36 kWh/pessoa/dia por
clima, indústria e preço, que o modelo não tem. Dos 76 déficits, ~35 são
estruturais (importadores reais sem E5 e cidades isoladas pelo corte de acesso) e
os demais incluem as 40 nações de cobertura fraca excluídas do ajuste (YEM, NGA…),
cujo índice herda a mediana regional (YEM não tem PIB em `nations.json`). Conclusão:
**parar aqui**, sem tocar em consequência; a alavanca continua sendo E5 e/ou dado
de demanda por nação (E4).



## 7. Diagnóstico fino do QA t80 (D-117): entidade, escala e interconexões

Rodada canônica com o runner instrumentado (365 d, threads=1, 292 s):
`godot --headless --path . --script res://tools/c1_world_ai_365.gd --
--days=365 --out=res://docs/economy/perf/c1_world_ai_365_e2e3.json
--control=res://docs/economy/perf/c6_gate_base.json`. As duas rodadas de
comparação estão em `docs/economy/perf/e2e3_diag_365_before.json` (dado antigo,
122 ligações) e no arquivo canônico (dado novo, 131 ligações). O runner agora
lista cada falência (firma, cidade, nação, setor, dia, último custo/receita e
racionamento da cidade), o primeiro dia de cada nação no portão de comida e a
cadeia do portão no fecho (`food_chain`), além da conta de energia / PIB por
nação (`energy_economy`) e do fluxo anual por ligação (`interconnect_links`).
Nada disso move dinheiro: o hash de trajetória do mesmo mundo não muda.

### 7.1 Bug do runner (corrigido)

`import_mwh`/`export_mwh` da rede são **acumulados no ano** em `sim/power/grid.cpp`
e não são resetados no laço diário; o runner dividia por 24 como se fossem do
dia, inflando a média em MW **365×** (ITA aparecia com 536.667 MW em vez de
1.470). A média correta é `mwh / (dias × 24)`, e a participação é
`importado no ano / demanda do ano`. Corrigido e coberto no `top10_importers`.

### 7.2 Falências por entidade (5)

| Dia | Nação/cidade | Firma | Setor | Última receita / custo | Energia da cidade | Causa |
|---:|---|---|---|---|---|---|
| 126 | USA/PRI-PON | `pri_pon_copper_mine` | cobre | 0,000 / 1,119 | 0,35 XGS/MWh, 100 % servida, 0 d racionando | receita zero, 60 d de prejuízo — **não é energia** (presente no controle) |
| 126 | USA/PRI-PON | `pri_pon_iron_mine` | ferro | 0,000 / 1,663 | idem | idem |
| 316 | SAH/MEH, TIC, TIF | 3 fazendas `sah_*_wheat` | trigo | +6,8 / 0,0 (lucro) | SAH sem rede (conta 0,00) | execução do banco: `SAH/public` com PL negativo no dia 315 e as construções do grupo fecham (C6) |

O controle sem energia tem 2 falências; as duas minas dos EUA têm receita zero
e nenhum racionamento (perfil de fechamento não-energético), mas o JSON antigo
não traz o registro por entidade. As 3 extras
**não** vêm do preço/serviço de energia na firma: SAH não tem rede elétrica e as
fazendas fecham no dia seguinte à resolução bancária, com lucro no último dia.
É o canal de crédito (C6), não o canal E2/E3. O grupo SAH não tem histórico de
perdas no runner — dívida em aberto para separar gatilho de cascata.

### 7.3 Interconexões (dado público por par)

A derivação Ember × fronteira terrestre deixa GB e Dinamarca em **zero** (ilhas,
sem par terrestre) e BGD–IND em zero (a Índia é importadora líquida no ano-base,
não entra como exportadora). Entraram 9 pares com capacidade nominal publicada
(Ofgem, Energinet/Danish Energy Agency, ADB): FRA 4.000, NLD 1.000, BEL 1.000,
NOR 1.400, IRL 1.000 para GB; NOR 1.632, SWE 2.015, DEU 3.500 para DNK; BGD–IND
1.000. O núcleo escala a capacidade pela fração simulada da mesma forma que o
parque; 97 das 131 ligações viram rede (as demais têm ponta sem operador
nacional). Fontes e método em `data/definitions/energy/interconnects.json`
(`meta.submarine_links` e `_sources`).

Importação do ano contra a Ember (a Ember também escalada pela demanda do
modelo, porque o modelo é a fração simulada):

| Métrica | Antes (122 ligações) | Depois (131) | Ember escalada |
|---|---:|---:|---:|
| Importado no ano | 45,9 TWh | **54,8 TWh** | 98,9 TWh |
| Importado / demanda | 0,449 % | **0,536 %** | 0,967 % |
| GBR | 0 | 5,03 TWh (3,36 %) | 5,75 % |
| DNK | 0 | 2,34 TWh (15,63 %) | 17,22 % |
| BGD | 0 | 1,50 TWh (7,23 %) | 12,31 % |
| THA | 3,95 TWh (4,95 %) | 3,95 TWh (4,95 %) | 15,54 % |
| USA | 0 | 0 | 0,27 % |

O restante da lacuna é **excedente do exportador**, não linha: FRA–GBR usa 29 %
da capacidade, LAO–THA 23 %, e 40 ligações ficam com fluxo zero porque o
importador já está 100 % servido (USA–CAN: o modelo dos EUA não tem déficit —
0,27 % é ruído) ou porque o exportador está no limite (IRL–GBR: GBR 100 %
servida mas sem sobra; MDA–UKR, BIH–MNE). Não é subdimensionamento de linha.

### 7.4 Sanidade econômica do preço e da escala

Conta de energia / PIB no modelo, no ano (mesma unidade XGS, `energy_economy`):

| Métrica | Modelo | Real (referência) |
|---|---:|---|
| Conta / PIB — p10 / p50 / p90 | 1,59 % / **11,72 %** / 394,8 % | 1,5–4 % (IEA WEO/WEI; Eurostat; Banco Mundial) |
| Conta / PIB — agregado do mundo | **56,99 %** | ~2 % |
| Energia no valor industrial (6 cidades com planta) | **5,0 % (teto)** em todas | 1–4 % (IEA/Banco Mundial: ~0,2–0,4 kWh por US$ de valor agregado) |
| Preço normal da energia (fora da escassez) | 0,32 XGS/MWh | — |
| Energia em kg de trigo por MWh | ~8 kg (0,32 ÷ 40 XGS/t) | ~40 kg (US$ 10/MWh ÷ US$ 250/t) |

O preço **não** é a escala errada: ele é o custo marginal (O&M 0,10–0,35
XGS/MWh; combustível é centavos), o caixa do setor fecha em **0** (toda a receita
é paga como combustível/O&M/salário, `power.cash`), e em relação aos bens a
energia do modelo é **5× mais barata** que a real. O denominador é que está
subdimensionado: o PIB simulado dos EUA é 18 XGS/pessoa/ano ≈ 0,45 t de
trigo-equivalente por pessoa, contra ~300 t no real — enquanto a demanda elétrica
é ancorada na Ember real. A mediana de 11,7 % e o agregado de 57 % são, portanto,
**calibração do PIB simulado (ou da escala da demanda)**, não preço de energia.
Cortar `price_scale` não fecharia a conta (a mediana é dominada pelo teto de
escassez, que não se toca) e deixaria custo marginal sem pagamento (o caixa do
setor já fecha em 0). Recomendação ao Game Systems Designer:
ancorar o PIB simulado (ou escalar a demanda energética pela fração de PIB
modelado), decisão de núcleo, não de dado de energia.

### 7.5 Portão de comida e classificação

Resultado: 109/179 abaixo de 0,975 (controle sem energia: 93; âncora sem E5:
124; dado antigo: 107). 19 nações cruzam por causa da energia e 3 saem do
portão (BHR, DMA, SUR). Cadeia das 19 (`food_chain`): **nenhuma tem agro ou
indústria alimentícia no modelo** e o preço da comida fica em 0,89–1,30× o dia
1 — o canal é renda/serviço, não preço.

| Grupo | Nações | Leitura |
|---|---|---|
| Racionamento de energia (365 d ou cidades isoladas) | AGO, BDI, BGD, ETH, KEN, LBY, MDA, MOZ, PRK, SWZ, TZA, XKX | consequência declarada do racionamento físico: indústria/serviço/emprego caem (desemprego 0,09–0,41) e a renda não paga comida importada |
| Energia servida, mas sem rede/ligação (ilha ou sem dado) | ISL (preço no teto o ano todo, comida 0,000), SAH (sem rede, comida 0,000) | (b) dado/calibração: ISL é ilha sem interligação na lista; SAH não tem parque |
| Energia plenamente servida a 0,32–0,35 XGS/MWh | ATG, AUS, KWT, VCT, KNA | fora do canal de energia: cruzam por efeito geral/food 0,000 (KNA sem agricultura no modelo) |

Classificação final: **(a) consequência esperada** — 12 nações acima, mais o
racionamento persistente de NGA/CIV/GNQ etc.; **(b) calibração** — PIB simulado
vs demanda ancorada (7.4), preço da ISL no teto sem interligação, e as 5 nações
sem canal de energia; **(c) bug** — só o do runner (7.1) e a ausência das
ligações GB/DK/BGD (7.3), ambos corrigidos. VOLL, preço de escassez,
racionamento, insatisfação e perdas **não** foram tocados.

## 8. Proposta (rascunho, 22/11/2026) — t82: as 4 nações, a escala PIB × físico e o SAH

Sem alterar o sim. Evidência: `docs/economy/perf/c1_world_ai_365_e2e3.json` (canônico, E5),
`e2e3_anchor_365.json` (sem E5), `e2e3_diag_365_before.json` (dado antigo) e
`c6_gate_base.json` (controle sem energia). O runner passa a publicar, por nação,
`model_scale`, `demand_vs_ember_demand` e `served_vs_ember_generation` (mesma razão de
`sim/power/grid.cpp`), para o próximo QA não confundir fração simulada com déficit.

### 8.1 (1) ATG, AUS, KWT, VCT: **não** há déficit de 32–35 % — não há bug a corrigir

| Nação | Servido (3 rodadas) | Fração simulada | Parque no modelo (MW) | FC do parque | Preço (XGS/MWh) |
|---|---:|---:|---:|---:|---:|
| AUS | 1,000 | 0,7301 | 84.684 (de 115.980) | 28,1 % | 0,3225 |
| KWT | 1,000 | 0,8717 | 18.522 (de 21.248) | 48,5 % | 0,3497 |
| ATG | 1,000 | 0,5492 | 64,3 (de 117,0) | 34,8 % | 0,3155 |
| VCT | 1,000 | 0,2454 | 17,2 (de 70,0) | 25,7 % | 0,3155 |
| KNA | 1,000 | 0,2754 | 22,0 (de 80,0) | 31,4 % | 0,3155 |

- `served_ratio` = **1,000** nas três rodadas de energia, e `demand_vs_ember_demand` =
  0,973–0,998 (sonda de 3 d: `c1_world_ai_365.gd --days=3`, sem rodada longa nova): a
  âncora D-116 é aplicada exatamente como declarada.
- **Parque não é subatribuído**: a soma de `by_city` é igual a `by_nation` em **176/176**
  nações (100 %), e acesso a eletricidade é 100 % nas quatro (operador nacional ≥ 60 %,
  rede nacional, sem cidade isolada).
- A rede é nacional nas quatro (NEM/AEMO cobre 18 cidades no modelo; WA não está no
  cenário, então “WA separada” não entra na conta). Nenhuma das quatro raciona (0 dias).
- O único 28–35 % existente nos dados é o **fator de capacidade do parque**
  (geração ÷ MW × 8.760), que reproduz a Ember (AUS 28,1 % real × 28,1 % modelo) — é
  física correta, não falta de energia. O “0,32–0,35” que aparece no §7.5 é o **preço**
  (0,3155–0,3497 XGS/MWh), não uma fração servida: a leitura do t82 confluiu os dois.
- **Conclusão**: erro de dado/regra **não há**; nada a corrigir nesta frente (o commit
  desta rodada é este §8 + os campos de diagnóstico do runner).

### 8.2 (2) A escala: o lado fora é o **dinheiro** (PIB), não a demanda física

Demanda física: `demand_vs_ember_demand` tem mediana **0,978** em 147 redes (mín. 0,857) —
a energia do modelo é a Ember × fração simulada, com a renda do ano movendo só ~2 %.

Lado monetário (`energy_economy`, amostra de 21 nações; PIB real PPP de `nations.json`):

| Nação | PIB sim (XGS/pessoa/ano) | PIB real PPP (US$/pessoa/ano) | Razão | MWh/pessoa/ano (sim × Ember) | Conta/PIB (sim) | Conta/PIB (real, US$ 90/MWh) |
|---|---:|---:|---:|---:|---:|---:|
| USA | 6,72 | 90.027 | 1/13.400 | 13,1 × 13,3 | 68,1 % | 1,33 % |
| CHN | 5,82 | 29.333 | 1/5.000 | 7,5 × 7,5 | 44,8 % | 2,29 % |
| DEU | 15,10 | 75.407 | 1/5.000 | 6,1 × 6,2 | 13,1 % | 0,74 % |
| BRA | 12,21 | 23.433 | 1/1.900 | 3,5 × 3,6 | 9,2 % | 1,40 % |
| IND | 10,21 | 11.748 | 1/1.150 | 1,4 × 1,4 | 4,3 % | 1,10 % |
| amostra | 8,40 | 38.094 | 1/4.500 | — | p50 11,7 % · agregado 57,0 % | 1,5–4 % (referência) |

- **Diagnóstico**: as quantidades físicas estão certas; o que está fora é o **nível de
  preços em XGS**. Há dois níveis incompatíveis na mesma unidade: 1 XGS ≈ **US$ 6**
  em bens (trigo 40 XGS/t × US$ 250/t) e ≈ **US$ 4.900** em trabalho (salário 0,05
  XGS/dia × US$ 247/dia de PIB PPP). A energia foi convertida no nível dos **bens**
  (0,32 XGS/MWh ≈ US$ 2 a US$ 10-20 de custo marginal real) e é paga no nível do
  **trabalho** — daí a conta de 68 % do PIB nos EUA. A afirmação “energia 5× mais barata
  que a real relativa ao trigo” está correta e **não** explica a conta: o trigo do modelo
  também está 6× abaixo do real, mas o salário está ~800× abaixo dele.
- **O que NÃO resolve**: (i) incluir serviços no PIB — `city.gdp` já é
  `value_added + service_revenue` (o setor de serviços está lá; falta nível, não setor);
  (ii) separar demanda industrial de residencial — muda o rateio da conta
  (`share_households` 0,30 / indústria 0,42 / serviços 0,21 / governo 0,07), não a razão
  conta/PIB; (iii) multiplicar XGS por constante — os dois lados da razão escalam juntos.
- **Correção mínima (proposta)**: declarar os parâmetros de energia em **unidades de
  salário** (`10 %` das linhas de custo atuais): custo marginal ≈ 0,05 salário-dia/MWh,
  tarifa ≈ 0,4 salário-dia/MWh (real: US$ 110 ÷ US$ 247 = 0,45) e VOLL ≈ 45 salário-dia
  (real US$ 10.000/MWh). Isso é ÷17 no O&M/piso e ÷13,6 no teto. Efeito estimado:
  EUA 68,1 % → **4,0 %**, CHN 44,8 % → **2,6 %**, agregado 57,0 % → **3,35 %**,
  p50 11,7 % → 0,69 %, p90 394,8 % → 23,2 %. O p50 abaixo da faixa é o que falta de
  **capex + T&D + tributo** (E7: a tarifa regulada devolve a faixa a 1,5–4 %); o p90
  acima é o **preço de escassez** cobrado sobre toda a carga servida (simplificação
  declarada, consequência — não escala).
- **Efeito nos portões** (estimativa, não medido): o canal da conta é 30 % no orçamento
  das famílias (`charge_households`), 42 % nas firmas e 21 % nos serviços; ÷17 devolve o
  dreno doméstico a ~1 % do PIB. Esperado: portão de comida 109 → **93–97** (os fatores
  físicos de racionamento permanecem) e falências 5 → **3** (as 2 minas PRI-PON existem
  no controle; as 3 fazendas SAH são crédito, §8.3). Racionalização: o portão com energia
  hoje mede **calibração de preço**, não falta de energia.

### 8.3 (3) SAH: banco de fio de faca, não o canal de energia

- SAH não tem dado de energia: `has_data=false`, `power_price=0`, `served=1,000`,
  0 dias racionando, parque zero → **canal direto de energia é nulo**.
- A resolução `SAH/public` (`cause: equity_negative`) aparece nas **três** rodadas de
  energia — dia **309** (dado antigo), **315** (canônico), **321** (sem E5), gap de
  **12,7–16,4 mil XGS** — e **não** aparece no controle sem energia. É sistemática em
  “energia ligada”, não um artefato do E5 (muda de dia a cada variante de dado).
- As 3 fazendas fecham no dia seguinte **com lucro** (6,8 / 94,3 / 1,8 XGS): é execução
  do banco (C6), não insolvência operacional. O gap do banco é **40× o PIB anual de SAH**
  (314 XGS/ano para 21 mil pessoas): é um banco marginal, de PL ~0.
- **Veredito**: consequência de **cascata determinística** (C6) amplificada por um banco
  no fio da navalha; o gatilho é o desvio de preços/renda do mundo inteiro (o setor
  elétrico compra combustível no mercado e move renda em 148 redes), não a conta de
  energia de SAH. Não deve ser contada como custo da E2/E3. Dívida de medição para
  fechar: trajetória diária de PL/dívida de `G-SAH-food-205` e do banco nos dois mundos.

### 8.4 (4) Três opções para o dono decidir

| Opção | Prós | Contras | Portões estimados |
|---|---|---|---|
| **A — corrigir a escala monetária** (energia em unidades de salário, §8.2) **e reabrir os portões** (D-118 + nova rodada 365) | Única opção coerente com o dado físico e com a realidade; conta/PIB volta a 1,5–4 %; mantém VOLL, fatores, racionamento e insatisfação | Muda consequência por calibração (padrão de vida sobe); exige rodada nova de 365 d e registro D-118; risco de “passar por decreto” se não for documentado | comida **93–97**/179; falências **3** |
| **B — aceitar portões recalibrados com energia** (registrar D-117/D-118 como estão) | Nenhuma mudança de núcleo; o portão mede honestamente o preço atual | Sela um mundo em que a conta de luz vale 57 % do PIB agregado (até 4.900 % por nação); p50/p90 seguem fora da faixa de referência e serão contestados de novo | comida **109**/179; falências **5** |
| **C — manter E2/E3/E5 atrás de flag desligada até G/H** (E7 tarifa regulada) | Protege o mundo estável; nenhuma rodada nova agora; a energia física fica pronta | Energia não comprovada no mundo real; o trabalho da E2/E3 fica sem validação de 365 d; o D-116 não fecha | mundo atual: comida **93**/179; falências **2** |

Recomendação do GSD: **A**, no mesmo pacote da **E7** (capex + T&D + tributo devolvem o
p50 à faixa de 1,5–4 %), depois de G/H; **B** como fecho de fase se o dono quiser encerrar
agora; **C** se a fase precisar fechar antes da decisão de escala.

### 8.5 Opção A executada (22/11/2026): unidade salário-dia na energia (D-118)

Implementado no núcleo (`sim/power.{h,cpp}` + `economy_sim.cpp`), sem tocar a forma do preço:

- **Unidade.** `price_floor`, `price_ceiling`, `opex_per_mwh_*`, `capex_per_mw_*` e
  `capex_per_mw_line` são declarados em **salário-dia**. `PowerParams::resolve_money`
  converte uma vez para XGS: `xgs = valor_salario_dia × salary_day_xgs`.
- **Fator derivado do próprio sim.** No seed, `salary_day_xgs` é a média ponderada pela
  população simulada do salário diário semeado do trabalhador não qualificado
  (`Pop::wage_by_level[0]`). Mundo medido: **0,055273 XGS/dia** — o mesmo nível da
  comparação do §8.2 (0,05). Não é constante escolhida à mão: vem do estado do mundo.
  Sem população/salário, cai no `kLaborWageInitial` declarado (0,055). A âncora vai ao
  save no mapa `policy` do bloco `power` (v36, aditivo), então o load não a re-deriva com
  o salário do dia; o status publica `salary_day_xgs`, `price_floor` e `price_ceiling`.
- **Custo marginal.** `preço = âncora × (combustível_do_mercado + O&M_salário_dia)`. A
  usina continua **comprando** o combustível no mercado de bens pelo preço do bem (com o
  prêmio de importação de 1,5×), e o pagamento/estoque saem como antes; a conversão é só
  do **preço** (formação), então a ordem de mérito e os custos relativos entre usinas
  ficam idênticos aos da regra declarada. VOLL continua múltiplo do preço da rede;
  escassez, racionamento, insatisfação e perdas não mudaram.

**Medido na rodada 365 do mesmo commit** (`c1_world_ai_365.gd --days=365
--control=res://docs/economy/perf/c6_gate_base.json`, 0 violações, 0 travados):

| Métrica | Antes (D-117) | Depois (D-118) |
|---|---:|---:|
| Conta/PIB agregada | 56,99 % | **2,66 %** |
| Conta/PIB p10 / p50 / p90 | 1,59 / 11,72 / 394,75 % | 0,084 / **0,634** / 10,26 % |
| Preço da rede típico (XGS/MWh) | 0,3155–0,35 | 0,0140–0,0194 (hidro 0,0083) |
| Energia servida | 0,9962 | 0,9962 |
| Falências (entidade) | 5 | **2** |
| Resolução de banco (C6) | 1 (SAH, dia 315) | **0** |
| Portão de comida | 109/179 | **99/179** (controle 93) |

Tabela das 10 maiores nações por PIB (conta/PIB, %; real do §8.2 onde citado):

| Nação | Antes | Depois | Real (US$ 90/MWh) |
|---|---:|---:|---:|
| CHN | 44,83 | 2,33 | 2,29 |
| IND | 4,33 | 0,24 | 1,10 |
| BRA | 9,22 | 0,50 | 1,40 |
| USA | 68,09 | **3,35** | 1,33 |
| JPN | 41,46 | 2,15 | — |
| DEU | 13,07 | 0,69 | 0,74 |
| MEX | 15,26 | 0,83 | — |
| FRA | 16,85 | 0,93 | — |
| GBR | 17,83 | 0,94 | — |
| CAN | 51,79 | 2,69 | — |

**Leitura.** O agregado e os EUA entram na faixa; a **mediana fica abaixo** dela. Como o
preço é único e proporcional, agregado/p50 = 4,20 > 4/1,5 = 2,67: **nenhum fator uniforme
coloca mediana e agregado na faixa ao mesmo tempo**. O que falta na mediana é a margem de
varejo (distribuidora + T&D + tributo) — a E7 — e a dispersão nacional de MWh por XGS do
sim (0,35 a 194) é maior que a real e não se corrige pelo preço. É o mesmo resultado que o
§8.2 previa (p50 0,69 %).

**Portão de comida 99/179 (controle 93): 7 extras, todos já extras antes da unidade.**
Quatro têm frota modelada abaixo da demanda (LBY 0,847; MDA 0,601; PRK 0,823; XKX 0,730;
365 d racionando — canal físico), ISL tem `food30 = 0,000` com 99,9 % de energia servida
(lacuna de comida do dado, não preço) e ATG/AUS estão na margem (0,533/0,934, energia
100 % servida, preço 0,017). A unidade tirou **12 extras** e **eliminou a cascata do banco
SAH** (0 resoluções; as 3 fazendas não fecham mais). O alvo ≤93 não é atingido: são 99
abaixo contra 93 do controle, e os 7 extras listados não têm canal monetário de energia.

**Falências 2** — as duas minas PRI-PON/USA (receita 0, 60 d de prejuízo), presentes no
controle sem energia; nenhuma nova e nenhuma do SAH.

### 8.6 Resíduo do portão de comida: E2/E3/E5 (22/11/2026)

Rodada 365 com IA (`threads=1`, `--control=res://docs/economy/perf/c6_gate_base.json`,
dado com as ligações de MDA e do Kosovo), evidência em
`docs/economy/perf/e2e3_food_residual_365.json`:

| Métrica | Controle (sem energia) | D-117 | D-118 | D-118 + ligações |
|---|---:|---:|---:|---:|
| Falências por entidade | 2 | 5 | 2 | **2** |
| Portão de comida (<0,975) | 93/179 | 109 | 99 | **102** |
| Extras vs controle | — | 19 | 7 | **9** |
| Conta de energia / PIB (agregado) | — | 56,99 % | 2,66 % | **2,65 %** |
| p50 / p90 da conta/PIB | — | 11,72 / 394,8 % | 0,634 / 10,26 % | **0,633 / 10,01 %** |
| % servida (mínima) | — | 0,9961 | 0,9962 | **0,9962 (0,9958)** |
| Ligações aplicadas (com fluxo) | — | 97 (57) | 97 (57) | **100 (58)** |
| Importado no ano / demanda | — | 0,536 % | 0,536 % | **0,536 %** |

Importado no ano: 54,84 TWh, 0,04 % acima da D-118 (abaixo: por que as 3 linhas
novas movem tão pouco). 0 violações de invariante, 0 dias travados, drift 0,0055.

#### (1) Dado: as ligações de MDA e do Kosovo

A derivação Ember × fronteira terrestre não criava o par em nenhum dos dois casos:
ROU é importadora líquida no ano-base (a regra só aceita vizinho exportador) e o
atlas não tem fronteira para XKX (`land_borders.json` sem Kosovo). Entraram com a
capacidade nominal publicada por par (`method=public_capacity`):

| Par | Capacidade | Fonte |
|---|---:|---|
| MDA–ROU | 600 MW | EBRD/EIB/IEA: back-to-back de Vulcănești (Isaccea–Vulcănești–Chișinău) |
| XKX–ALB | 400 MW | OST (via Voshtina et al., JSDEWES 12(2), 1120509): NTC Albania–Kosovo |
| XKX–MKD | 330 MW | SEE CAO: 400 kV Ferizaj 2–Shkupi 5, NTC de 330 MW (alocação de jan/2021) |

`XKX–SRB` e `XKX–MNE` ficam **declarados** em `meta.declared_absence_links`, sem
capacidade: o NTC por par Kosovo–Sérvia não está acordado nem publicado (Energy
Community, caso ECS-02/21, decisão 2025/02/MC-EnC) e o de Montenegro também não; o
total das 4 fronteiras (1.316 MW de importação) está na Estratégia de Energia do
Kosovo, sem repartição por par. `meta.submarine_links` virou
`meta.public_capacity_links`, com `by_reason` (ilha, vizinho importador líquido,
fronteira ausente no atlas). 131 → 134 ligações no dado; 100 aplicadas (as demais
têm ponta sem operador nacional).

**As 3 linhas novas movem pouca energia.** `XKX–ALB` entregou **35.997 MWh no ano**
(ALB exporta 37.497; 4 % de perda) — 1,1 % da demanda de XKX (3,4 TWh) e ~2 % da
capacidade da linha (4,1 MW médios). `MDA–ROU` e `XKX–MKD` ficaram em **0 MWh**. O
importado no ano vai de 54,820 para 54,843 TWh (+0,04 %). Motivo: os quatro
vizinhos são **importadores líquidos** no ano-base Ember (ALB +0,33; SRB +2,03;
MKD +0,92; ROU +3,74 TWh) e a regra da E5 (`sim/power/grid.cpp`) vende só a
**capacidade disponível não despachada** do exportador — rede 100 % servida quase
não tem sobra, e a ALB só tem sobra hídrica em poucos dias. O tamanho do déficit
modelado é coerente com a realidade:

| Nação | Déficit modelado (não servido) | Importação real Ember (ano-base) |
|---|---:|---:|
| MDA | 39,9 % (0,601 servido) | 43,9 % da demanda (2,608 / 5,943 TWh) |
| XKX | 27,0 % (0,730 servido) | 21,3 % da demanda (1,587 / 7,465 TWh) |

Ou seja: o modelo entrega como corte o que o país importa no mundo real; falta o
**canal de compra** (regra da E5 + ano-base em que todos os vizinhos importam), não
a linha nem a capacidade. Classificação: **(b) dado/regra**, não erro da ligação
adicionada.

#### (2) ISL: comida 0 com 99,9 % de energia servida

- Déficit físico de **3.520 MWh/ano em 6.706.123** (0,052 %, ~10 MWh/dia) e preço no
  **teto** (1,658 XGS/MWh) **todos os dias** → conta de 11,11 M XGS/ano = **884 % do
  PIB** de ISL. Com o preço normal da hidro/geotermia (0,0083 XGS/MWh) a conta seria
  ~0,4 % do PIB, dentro da faixa de referência.
- ISL não tem interligação **por dado**: `ember_net_imports_twh = 0` e não há
  fronteira terrestre — a ausência é correta.
- Não é divergência de semente: ISL **não** está no portão do controle sem energia
  (≥0,975) e o valor é **idêntico** nas três rodadas com energia (D-117, âncora sem
  E5 e D-118), inclusive sem ligação internacional. O efeito é o bloco de energia,
  não a E5.
- Canal: **regra declarada do preço de escassez** (cobrado sobre 100 % da carga
  servida, §8.2) disparada por um vão de 0,05 % do próprio parque. Classificação:
  **(b)** regra declarada; o remédio é a margem de varejo (E7) ou o preço de
  escassez só sobre o MWh não servido — não a cadeia de comida.

#### (3) AUS e ATG: margem com energia 100 % servida

| Nação | Comida (controle) | Comida (energia) | Servido | Dias racionando | Preço final | No teto? |
|---|---:|---:|---:|---:|---:|---|
| AUS | ≥0,975 (fora das 93) | **0,934** | 1,000 | 0 | 0,0178 | não |
| ATG | ≥0,975 (fora das 93) | **0,533** | 1,000 | 0 | 0,0174 | não |

O controle versionado não guarda o valor por nação acima do limiar (só a lista das
93 e os 10 piores), então o valor do controle é um **limite** (≥0,975), não um
número. O valor com energia é **bit-idêntico** nas três rodadas com energia (D-117,
âncora sem E5 e D-118): não é preço de energia nem E5. Canal: nem serviço (100 %
servida, 0 dia racionando, preço normal a 2,7 % do teto) nem escassez — a posição
vem do **lado da comida** (preço 1,10–1,16× o do dia 1, gasto 12 % do PIB,
desemprego 0,04): disponibilidade/renda. O A/B no código de hoje (energia
desligada, `tools/c1_world_ai_365.gd --power=0`) mostra: **sem energia nenhuma das 9 extras cruza o portão** (93/179, o mesmo número do controle versionado, e as 9 ficam em ≥0,975) e **com energia as 9 cruzam** (102/179) — a diferença entre os dois mundos é exatamente o conjunto das 9. O bloco de energia é a causa; o valor exato por nação no braço sem energia não é publicado (o runner só detalha quem cruza).

Classificação: **(b) margem/canal não isolado** — AUS 3 % abaixo do limiar e ATG um
degrau maior, com o mesmo valor em todo mundo com energia.

#### (4) LBY e PRK: o déficit modelado é coerente com a escassez real

| Nação | Servido | Dias racionando | Apagões reais documentados |
|---|---:|---:|---|
| LBY | 0,847 | 365 | GECOL: cortes de 8 h a 36 h por dia em Trípoli e três blecautes grandes em 2026 (Libya Herald 2025-07 e 2026-08; Arab News: até 36 h em Qasr Bin Ghashir) |
| PRK | 0,823 | 365 | EIA: rolling blackouts na capital; RFA (2024): 1 h/dia em áreas rurais; AsiaPress (2026-02): mais de 20 h/dia |

Os dois são importadores líquidos ~0 na Ember (sem par de compra modelado) e a
direção do déficit é a documentada; o modelo é **mais brando** que o relato real
(84,7 % e 82,3 % servidos). Classificação: **(a) consequência real** (D-116).

#### (5) As três nações que cruzaram nesta rodada (CIV, KNA, SUR)

Sem energia nova em jogo: CIV (0,970, 365 d racionando, 91,2 % servido) é **(a)**
racionamento físico; KNA (0,000, 100 % servida, 1 prédio de comida e 0 t de
produção) é **(b) dado** (mesma família de SAH, §7.5); SUR (0,9735, já abaixo no
controle) é **(b) margem**. Nenhuma tem canal de energia: CIV/KNA/SUR não são ponta
de ligação nova e os 36 GWh entregues no Kosovo não chegam lá. O que mudou foi a
trajetória (o mundo é caótico: `served_ratio` difere na 6ª decimal e a confiança
mínima do banco vai de 0,992 para 0,494, sem nenhum evento C6). **O portão tem
margem de ±3 nações**: comparar 102 com 93 não separa energia de ruído.

#### Classificação final do resíduo (9 extras)

| Grupo | Nações | Classe |
|---|---|---|
| Racionamento físico 365 d | LBY, PRK, MDA, XKX, CIV | (a) — com (b) regra/dado na compra de MDA/XKX |
| Preço de escassez sobre 0,05 % de déficit | ISL | (b) regra declarada |
| Comida sem canal de energia (100 % servida, preço normal) | ATG, AUS, KNA | (b) margem/dado |

**(c) bug:** nenhum encontrado no dado nem na cadeia. O único defeito de medição é
`importer_short_days` (conta só dias com fluxo, porque o núcleo publica o importador
só quando há fluxo) — declarado, não corrigido (é do C++, fora desta rodada).

**O que NÃO foi tocado:** VOLL, preço de escassez, racionamento, insatisfação,
perdas, limite do portão (0,975), a regra de comércio da E5 e o núcleo C++.

**Atualização D-120 (§9):** a tarifa de varejo (E7) e o contrato de suprimento do
ano-base levam o mesmo mundo a **95/179** com **3 extras** (CIV, PRK, AUS) — a
classificação e a dívida do p50 estão em §9.5/§9.6.

## 9. E7 (D-120) — tarifa de varejo e contrato de suprimento do ano-base

Evidência: `docs/economy/perf/e2e3_food_e7_365.json`; a repetição
`e2e3_food_e7_365c.json` é a mesma rodada (mesmo `trajectory_sha256`
`057ebb80…`, único campo diferente `wall_s` 298,83 → 299,802).

### 9.1 Tarifa de varejo por rede

O consumidor passa a pagar a **tarifa**, não o spot do dia:

    tarifa = atacado_médio ÷ (1 − retail_network_share) + repasse_de_escassez

- **Atacado médio**: custo ponderado da energia entregue à rede no dia (geração
  doméstica ao custo de mérito + importado ao preço de fronteira), suavizado por
  média móvel de `retail_scarcity_days` = **30 d**.
- **Repasse de escassez**: `max(0, spot − atacado) × fração NÃO servida`, também
  suavizado. Só incide sobre o déficit (um vão de 0,05 % não multiplica a conta
  inteira, o defeito do §8.6) e **parte de zero** — a média móvel não cobra o pico
  do primeiro dia.
- **Parcela de rede/tributos**: `retail_network_share` = **0,55**, declarado pela
  composição pública da tarifa (Eurostat `nrg_pc_204` e IEA *Energy Prices and
  Taxes*: rede + tributos ≈ 45–60 % da tarifa residencial da UE). Não é um número
  por país: o dado por rede é o custo médio de geração da própria rede, que varia
  com o parque.
- **Circulação do dinheiro** (`phase_power_settle`): famílias, firmas, serviços e
  governo pagam a tarifa; o gerador recebe o **atacado médio**; a diferença (rede
  + tributos + repasse) vai ao caixa do Estado **pelo caixa que já existe**.
  Conta que o caixa não cobre **não vira dívida**: é perda do setor. Soma zero,
  `power_cash` dentro de `money_total`, invariantes da C1 fecham.
- **Intocados**: o spot de escassez, o VOLL, o racionamento, a insatisfação e as
  perdas. O spot continua publicado e é o que alimenta VOLL/diagnóstico; a tarifa
  é só a conta do consumidor.

### 9.2 Contrato de suprimento do ano-base (comércio bruto)

- Dado novo `links[].contract_mw` (`tools/data/energy/build_interconnects.py`):
  fluxo **bruto** do importador no ano-base Ember (`net_imports_twh × 1e6/8760`
  MW), rateado entre as linhas dele **proporcional à capacidade** (único
  repartidor público por par) e limitado pela capacidade. Pares em que os dois
  lados importam usam a direção **declarada** no documento do par (MDA–ROU:
  EBRD/EIB/IEA; XKX–ALB: OST/JSDEWES; XKX–MKD: SEE CAO).
- No despacho, o contrato é entregue **antes** da demanda marginal do exportador
  (carga = demanda + contratos), até a capacidade. O alvo do dia é limitado ao
  **déficit esperado do importador** (demanda − disponibilidade local do próprio
  parque), então não se gera energia ociosa; se o exportador está curto, o
  contrato cai **proporcionalmente** e a própria demanda divide a escassez na
  mesma proporção.
- Preço do contrato: **custo médio de atacado** do exportador ÷ (1 − perda da
  linha) — não o spot marginal do dia. A **E5 continua por cima**: a sobra spot é
  vendida depois do contrato.
- Resultado no ano: 100 ligações aplicadas, **20 com fluxo spot** e importado
  total de **62,84 TWh** (0,615 % da demanda; +8,0 TWh vs a D-119 — o contrato).

### 9.3 Fontes

| Item | Fonte | Uso |
|---|---|---|
| Parcela de rede + tributos | Eurostat `nrg_pc_204`; IEA *Energy Prices and Taxes* | 45–60 % → `retail_network_share` 0,55 |
| Fluxo bruto do contrato | Ember yearly (`net_imports_twh`) | MW do importador rateado por capacidade |
| Capacidade por par | Ember × fronteira terrestre; `meta.public_capacity_links` (EBRD/EIB/IEA, OST/JSDEWES, SEE CAO) | teto do contrato |
| Custo de geração e combustível | mercado do próprio sim (D-116/D-118) | atacado médio |
| Perda da linha | piso técnico de T&D já existente (`loss_floor_pct`) | preço de fronteira do contrato |

### 9.4 Medição 365 d (mesmo controle da D-119)

| Métrica | Controle (sem energia) | D-119 | **D-120 (E7)** |
|---|---:|---:|---:|
| Falências por entidade | 2 | 2 | **2** (as duas minas PRI-PON) |
| Portão de comida (<0,975) | 93/179 | 102 | **95** |
| Extras vs controle | — | 9 | **3** (CIV, PRK, AUS) |
| Conta de energia / PIB (agregado) | — | 2,65 % | **1,248 %** |
| p50 / p90 / máx. da conta/PIB | — | 0,633 / 10,01 / — | **0,658 / 3,52 / 24,35 % (URY)** |
| % servida (mínima) | — | 0,9962 | **0,99610** |
| Ligações aplicadas (com fluxo spot) | — | 100 (58) | **100 (20)** |
| Importado no ano | — | 54,84 TWh | **62,84 TWh (0,615 %)** |
| Spot médio ponderado / tarifa (dia 365) | — | 0,0178–0,0194 | **0,1509 / 0,0304 XGS/MWh** |
| Violações / travados / drift | 0 / 0 / — | 0 / 0 / 0,0055 | **0 / 0 / 0,005545** |

O `average_price` do JSON (0,1509 XGS/MWh) é o spot médio ponderado do ano
(campo legado; a conta do consumidor passou a ser `tariff`, 0,0304 no dia 365).
O importado sobe 8,0 TWh porque o contrato é uma camada de **entrega** acima da
E5; a fração servida e o racionamento não mudaram (a mesma rede física).

### 9.5 Classificação das três nações extras do portão

| Nação | comida (30 d) | servida | dias racionando | preço/tarifa | Classe |
|---|---:|---:|---:|---:|---|
| CIV | 0,9722 | 0,9125 | 365 | 0,0330 | **(a) consequência real** — racionamento físico (D-116) |
| PRK | 0,8409 | 0,8328 | 365 | 1,3733 | **(a) consequência real** — déficit documentado (EIA/RFA/AsiaPress), modelo mais brando |
| AUS | 0,9344 | 1,000 | 0 | 0,0178 | **(b) margem pré-existente** — energia 100 % servida, preço normal; o valor é o mesmo da D-119 (0,934) |

As **seis nações que deixaram o portão em relação à D-119** — ISL, MDA, XKX,
LBY, ATG e KNA — têm em comum a conta cobrada pela **tarifa** (e, em MDA/XKX, o
contrato). O caso extremo era ISL: o spot no teto sobre 100 % da carga dava
884 % do PIB (§8.6); com a tarifa, o consumidor paga o custo médio e o prêmio só
sobre o déficit, e ISL sai do portão. Limite de medição declarado: o runner só
publica o valor de quem está **abaixo** de 0,975, então a separação fina das seis
não é medida (mesma limitação do §8.6).

### 9.6 Dívida declarada e portão pendente

- **Portão de comida 95/179 contra teto 93: REPROVA por 2 nações.** Duas têm causa
  declarada (a) — CIV e PRK, racionamento físico 365 d — e a terceira (AUS) é
  margem (b) já presente sem energia. **A aceitação do portão 95/179 está
  PENDENTE DE DECISÃO DO DONO.** Nenhum limite, consequência ou preço foi
  amolecido; a margem de ±3 nações do §8.6 continua valendo.
- **p50 da conta/PIB 0,658 % (agregado 1,248 %, p90 3,52 %) abaixo da faixa de
  referência 1,5–4,0 %.** É **dívida de calibração da escala de preços da D-118**
  (conversão XGS/MWh ↔ PIB simulado), não do E7: a tarifa só levou a conta para o
  custo médio e o agregado caiu de 2,65 % para 1,25 %. **Não se mexe agora**
  (decisão do dono). O §8.5 já mostra por que um fator uniforme não fecha
  mediana e agregado ao mesmo tempo (agregado/p50 = 1,90 < 4/1,5 = 2,67).

### 9.7 Save v37 e testes

- Save **v37 aditivo**: por cidade `average_cost`, `tariff`, `scarcity_pass`; por
  rede `average_cost`, `tariff`, `scarcity_pass`, `retail_margin`; por ligação
  `contract_importer/exporter/mw/flow_mwh/sent_mwh/price/value`. Save v36 carrega
  com as chaves ausentes = 0 (a tarifa do próximo tick nasce do atacado derivado
  — nada inventado para trás); round-trip v37 bit-exato coberto pelo
  `e2e3_power_test`.
- `tools/e7_tariff_test.gd`: conservação/soma zero com a tarifa, caso ISL (déficit
  0,05 %, spot no teto, tarifa perto do custo médio), racionamento da D-116
  intacto e contrato (entrega, queda proporcional, save v37).
- `e2e3_power_test.gd` e `e5_interconnect_test.gd` em save v37.

### 9.8 Fechamento — goldens A0/S0 e equivalência CLI × Godot

Os harnesses dos goldens (`tools/world_equivalence.gd` e
`tools/economy_refactor_equiv.gd`) **não** chamam `set_power_definitions`, então
rodam com `power_ready = false`: não entram parque, rede, tarifa nem contrato.
Ainda assim o save mudou de v34 para v37, e os goldens foram **regerados** com a
DLL debug `C193A493…` (registro exigido pelo `A0_README` §5), 2 rodadas
idênticas em cada um:

| Golden | status (antes = depois) | save antes → depois |
|---|---|---|
| `A0_60_urban` (60 d, IA, urbano) | `f6ec0427…` (idêntico) | `5fadba28…` → `ee07fd9c…` |
| `real_60_ai` (60 d, IA) | `1c2a85fa…` (idêntico) | `edc686ee…` → `f0e4fbed…` |
| `real_60_noai` (60 d, sem IA) | `8ae9f20b…` (idêntico) | `6d657f25…` → `1e36319e…` |
| `demo_365_noai` (365 d, AGRIA/PORTUS) | `10583589…` (idêntico) | `dc67ad5c…` → `d507e0db…` |

**Campos que mudaram no save** (medido; o `status_sha256` idêntico prova que a
trajetória de 60/365 d não mudou):

1. `version` **34 → 37** (D-116/D-117/D-120);
2. `construction_projects[].power_fuel = -1`, `power_mw = 0`,
   `power_nation_a = ""`, `power_nation_b = ""` — chaves aditivas da obra de
   energia escritas para **todas** as 2.413 obras do A0 (o bloco `power` em si
   **não** é gravado sem energia semeada);
3. `pop.dissatisfaction*` e `region.dissatisfaction*` — na origem deste fechamento
   o divisor da normalização chegou a incluir o peso novo
   `kDissatisfactionPower = 0,20`, somado mesmo com estresse de energia zero
   (`sim_constants.h`/`informality.cpp`): **interior 0,50 → 0,70** (índice =
   71,4 % do anterior) e **urbano 1,00 → 1,20** (83,3 %). **Corrigido na t106**
   (§9.10): o peso voltou a ser ADITIVO e o divisor é o antigo.

**Equivalência CLI × Godot:** snapshot do dia 0 (`export_world_snapshot.gd`,
`--ai=1 --urban=0`, 67.773.105 B) → 10 d com IA, `threads=1`, no Godot
(`snapshot_replay.gd`) e no `opengs_sim_cli.exe`: arquivos **byte-idênticos**,
79.618.805 B, SHA-256 `56AA9E8E…`, 0 violações de invariante.

### 9.9 Bateria de testes do fechamento (96 testes)

`tools/run_tests.ps1 -All -MinFreeDiskGB 6` no HEAD `ddd50d77` + esta rodada:
**72 verdes / 24 vermelhos**. Nenhum vermelho é do E7
(`e2e3_power_test`, `e5_interconnect_test`, `e7_tariff_test` e
`economy_save_load_roundtrip_test` verdes). Classificação dos 24:

- **23 por pino de versão do save defasado** (testes esperam v34/v35/27/28/25/32
  e o save é v37 desde a D-120): `agriculture_seasonal`, `b3_region`,
  `b4_migration`, `b5_state_levers`, `b6_property`, `c1_balance`, `c2_central_bank`,
  `c2b1_monetary`, `c2b2_fx_gold`, `c2c_real_fx`, `c3_bank_credit`, `c4_banks`,
  `c5_stock_exchange`, `c6_bank_crisis`, `city_gov`, `construction_queue`,
  `economy_integration`, `economy_save_load_v11_compat`, `mine_stage`, `services`,
  `storage_g2`, `urban_land`, `urban_load`. A falha é só a versão (ex.:
  "save version 37 != 34"); os valores medidos e o round-trip passam.
- **2 por divisor da insatisfação** (`b7_informality_test`): espera
  informalidade 0,10/0,50 = 0,20 e inflação 0,15/0,50 = 0,30; com o peso de
  energia no divisor o teste media **0,143 e 0,214**. Resolvido na **t106**
  (§9.10): o divisor voltou a 0,50/1,00 e o teste está verde.

O runner terminou com `OutOfMemoryException` ao gravar o `summary.json` (os 96
exit codes foram capturados no log e são a base da contagem); a suite já estava
vermelha antes desta rodada de docs (só docs/JSON foram commitados aqui).

### 9.10 Correção pós-QA t106 — insatisfação sem diluição (D-116)

A D-116 tinha somado `kDissatisfactionPower` (0,20) ao **divisor** da
insatisfação: interior 0,50 → 0,70 e urbano 1,00 → 1,20, reduzindo a
insatisfação de base em **28,6 % / 16,7 %** mesmo com energia 100 % servida —
o sistema amolecia. Correção (HEAD desta rodada):

- `informality.cpp`: o divisor é o **antigo** (comida+inflação+informalidade no
  interior; +desemprego+aluguel no urbano) e o termo de energia entra
  **aditivo**: `índice = clamp(base_sem_energia + 0,20 · power_stress, 0, 1)`.
  Com `power_stress = 0` o índice é **idêntico** ao de antes da energia.
- `b7_informality_test`: volta a pinar 0,10/0,50 = 0,20 e 0,15/0,50 = 0,30.
- `e2e3_power_test` (13): mundo iluminado ⇒ `índice == soma das 5 componentes`
  (tolerância 1e-9); mundo escuro ⇒ `índice − base = 0,20 · stress`.
  (O fixture AGRIA/PORTUS não tem bloco de região; o interior fica no B7.)
- Digest D-105 (`core/economy/trajectory_digest.gd`): entram, por dia,
  geração somada por fonte, carga servida, racionamento (`demanda − servida`),
  caixa do setor, soma da tarifa de varejo por rede e fluxo de importação
  (campos 12–17 de `FIELDS`). Sem energia semeada o núcleo publica zeros
  declarados. Trajetórias de energia diferentes passam a dar sha diferente.
- `sim/power/retail.cpp`: guarda `plant_city_in_range` (assert + skip determinístico) nas
  três listas do `phase_power_settle` que indexam `cities[plant.city_index]`;
  comentários do E5/E7 corrigidos (o importado é ledger `import_value`; o
  pagamento é a tarifa no caixa global, não débito imediato em `power_cash`).

**Medido:** `e2e3_power_test`, `e5_interconnect_test`, `e7_tariff_test`,
`b7_informality_test` e `economy_save_load_roundtrip_test` **verdes**. Rodada
365 d com `--control=c6_gate_base.json` (0 violações, 0 travados):

| Métrica | Valor |
|---|---:|
| Falências | 2 (as duas minas PRI-PON/USA, dia 126) |
| Portão de comida | 95/179 (mediana 0,9698; controle 93) |
| Conta/PIB | agregada 1,248 % · p50 0,658 % |
| Carga servida | 99,65 % (mín. 99,61 %) |
| `trajectory_sha256` | `866113fe…` (17 campos) |

Goldens A0/S0 regerados (o índice de insatisfação voltou ao valor pré-energia
no save; o **status** global segue idêntico, provando que a trajetória não
mudou): `A0 ee07fd9c → 2b3b0b6e`, `real_60_ai f0e4fbed → b1774c3f`,
`real_60_noai 1e36319e → e9c6e473`, `demo_365 d507e0db → e7bf1a25`; cada um
gerado e reconferido com o mesmo binário (2 rodadas → 0 diferenças).
Equivalência CLI × Godot (snapshot do dia 0, 10 d, IA, `threads=1`):
**byte-idênticos**, 79.615.141 B, SHA `1EEDAB08…`, 0 violações.

## 10. Dívidas declaradas

- Escala monetária da energia: **resolvida na unidade** (D-118, §8.5) — parâmetros em
  salário-dia, âncora derivada do sim no seed e gravada no save. A **margem de varejo
  (E7, §9)** entrou e a conta passou a ser cobrada pela tarifa; segue aberto o **p50
  0,658 % e o agregado 1,248 % abaixo da faixa 1,5–4,0 %** — **dívida de calibração
  da escala de preços da D-118**, declarada e não mexida nesta rodada. O portão de
  comida está em **95/179 (3 extras, §9.5)** e a **aceitação está pendente de decisão
  do dono**.
- **Portão de comida tem margem de ±3 nações** (§8.6/§9.4): as três linhas novas
  entregam só 36 GWh (0,04 % do importado) e ainda assim moveram CIV/KNA/SUR,
  porque o mundo diverge na 6ª decimal. Comparar 95 com 93 não separa energia de
  ruído; o aceite precisa de banda, não de número exato.
- `importer_short_days` do runner conta só dias com fluxo (o núcleo publica o
  importador só quando há fluxo): a coluna não mede o déficit do importador. Correção
  exige o núcleo publicar o déficit nacional do dia — fora desta rodada. Vale também
  para o contrato: o alvo do dia já é limitado ao déficit, mas o runner não publica a
  queda por par.
- Canal de AUS (comida 0,934 com energia 100 % servida e preço normal): o A/B
  `--power=0` isola o bloco de energia, mas não separa renda de disponibilidade de
  comida; falta um diagnóstico de comida por nação (importação, estoque, transporte)
  no runner. ATG/KNA/LBY deixaram o portão na D-120 pela conta da tarifa, mas o valor
  deles acima de 0,975 não é publicado (limite de medição).
- Comércio da E5: só compra de vizinho com **capacidade disponível não despachada**.
  A E7 acrescentou o **contrato bruto** do ano-base (§9.2) por cima da sobra; em
  ano-base em que todos os vizinhos importam (Kosovo), a linha sem contrato continua
  existindo e sem venda spot — mudar isso é regra de núcleo, não dado.
- Vento sem série mensal (GWA não publica) — fator anual, declarado. A D-122
  acrescenta variação diária **declarada** para a capacidade NOVA (não é série
  medida); o parque do ano-base continua na âncora Ember (ver §11).
- `oil: null` na matriz Ember — capacidade de óleo não publicada por país (D-115(d)).
- Conexão parcial é um limite de troca por cidade (não se modela fluxo físico entre
  subestações); interligação internacional (E5) e obra de redução de perdas ainda não.
- E4 detalhada (estratos/indústria por bem) e E6 (mapa) fora desta fase.

## 11. Renováveis — balanceamento de jogabilidade (D-122)

**Decisão do dono (22/11/2026):** renovável (hídrica/solar/eólica) deve ser **mais
cara de construir e produzir menos**; a térmica (carvão/gás/óleo) é **barata de
construir**, compra combustível caro/importado e é despachável. Nuclear é
despachável e estável. O balanceamento vale para a **jogabilidade** — obra nova
(jogador e IA) e comportamento operacional — e **não** toca a calibração do
ano-base: as usinas existentes continuam reproduzindo a geração Ember
(D-116/D-119/D-120), verificado em ±1 % (teste 365 d da suíte).

Todo número abaixo é **DEFAULT DE BALANCEAMENTO DECLARADO** em
`data/definitions/energy/power_params.json` (`meta.renewables_balance`), **não**
dado observado: a fonte pública cobre acesso/perdas, parque, PVOUT/vento e
potencial hídrico; o preço/CF da usina NOVA é regra de jogo.

### 11.1 Custo declarado (obra nova)

Multiplicador de capex por fonte (aplicado só à obra NOVA; o parque semeado
mantém o capex do ano-base): **hídrica 2,4; solar 2,2; eólica 2,2**; térmicas e
nuclear 1,0. Prazo: hídrica **420 d** (antes 180 d); demais fontes seguem 180 d.

Capital por MWh esperado = `capex/MW ÷ (8760 h × CF)` (o jogo não amortiza
capital: o capex é pago na obra) + O&M. CF da usina nova = `min(potencial do
sítio, cf_cap) × balance_penalty`. Unidade: salário-dia (D-118).

| fonte | capex/MW antes | capex/MW depois | CF antes | CF novo (teto × penalidade) | capital/MWh-ano antes | capital/MWh-ano depois | O&M/MWh | custo/MWh esperado antes | depois |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| solar | 700 | **1 540** | 0,22 | ≤ 0,2375 (0,25 × 0,95) | 0,363 | **0,740** | 0,10 | 0,463 | **0,840** |
| eólica | 750 | **1 650** | 0,35 | ≤ 0,405 (0,45 × 0,90) | 0,245 | **0,465** | 0,12 | 0,365 | **0,585** |
| hídrica | 900 | **2 160** | 0,50 | 0,475 (0,50 × 0,95) | 0,205 | **0,519** | 0,15 | 0,355 | **0,669** |
| carvão | 1 200 | 1 200 | 0,60 | 0,60 | 0,228 | 0,228 | 0,25 | 0,478 | 0,478 |
| gás | 1 000 | 1 000 | 0,55 | 0,55 | 0,208 | 0,208 | 0,22 | 0,428 | 0,428 |
| óleo | 900 | 900 | 0,40 | 0,40 | 0,257 | 0,257 | 0,30 | 0,557 | 0,557 |
| nuclear | 2 600 | 2 600 | 0,85 | 0,85 | 0,349 | 0,349 | 0,35 | 0,699 | 0,699 |
| biomassa | 800 | 800 | 0,55 | 0,55 | 0,166 | 0,166 | 0,30 | 0,466 | 0,466 |
| outras | 900 | 900 | 0,45 | 0,45 | 0,228 | 0,228 | 0,30 | 0,528 | 0,528 |

Leitura: depois da D-122, solar/eólica/hídrica ficam **acima de carvão/gás/óleo**
tanto no capex/MW quanto no custo por MWh esperado. O **O&M segue menor** (a
renovável é barata de operar) e o combustível da térmica não está na coluna —
é o custo operacional que a térmica paga e a renovável não. A tabela do status
(`build_costs`, XGS já resolvidos pela âncora) é a fonte do jogador/IA; o teste
`tools/renewables_balance_test.gd` confere os mesmos números.

### 11.2 Produção: potencial do sítio, teto e penalidade

- **Solar:** CF = PVOUT médio do Global Solar Atlas (kWh/kWp/dia ÷ 24 h),
  teto **0,25**, penalidade **0,95**; a forma mensal do PVOUT continua valendo
  mês a mês.
- **Eólica:** CF = `0,30 × (v / 6 m/s)^3` com `v` = vento médio anual do GWA a
  100 m (E6), teto **0,45**, penalidade **0,90**. Sem amostra de vento, usa o
  CF declarado `cf_wind` (não se inventa sítio).
- **Hídrica:** CF declarado 0,50, teto 0,55, penalidade 0,95; o potencial
  hídrico (`hydro_potential.json`) limita a capacidade em MW, como antes.
- **Térmica/nuclear:** CF declarado da fonte, sem teto novo — despacháveis e
  estáveis (o resíduo sazonal das térmicas da E2 continua).
- **Variação diária determinística** (solar/eólica/hídrica) da usina NOVA:
  fase derivada da nação por FNV-1a (nunca `std::hash`); as harmônicas têm
  período que divide 365 dias, então a média em **qualquer janela de 365 dias é
  exatamente 1** — o ano-base não se move. Amplitudes: solar 0,18, eólica 0,30,
  hídrica 0,25 (cheia no fim do inverno/início da primavera do hemisfério, dia
  75 no norte e 255 no sul, ±30 d pela nação).
- **Parque do ano-base:** fica na âncora Ember (`cf_scale`), sem a variação
  declarada — não há série diária pública de vento/vazão para ele; o dado
  disponível para essas usinas é anual (GWA sem série mensal; hídrico sem vazão
  mensal). A calibração do ano-base, portanto, não muda (0 % de desvio; o teste
  exige ≤ 1 %).
- **Curtailment:** sem armazenamento, a renovável disponível e não despachada é
  desperdiçada. O núcleo publica `curtailment_mwh` por rede e no mundo; ela não
  entra em `served_mwh` nem em geração.

### 11.3 Crédito de capacidade e a IA

Crédito de capacidade no pico (fração da potência que conta como firme): solar
**0,35**, eólica **0,20**, hídrica **0,55**, térmica/nuclear **1,0**. A IA de
obra (§5) compara o **custo do primeiro ano por MW firme**:
`(capex da obra + combustível declarado × 8760 h × CF) ÷ crédito`, com o
combustível ao preço do mercado local (prêmio de importação quando falta
estoque). Onde a falta persiste, a térmica é a escolha mais barata por MW firme,
embora pague combustível; a renovável tem capex alto e crédito baixo. O jogador
vê a mesma troca em `build_costs` e `new_plants` no status (capex,
multiplicador, O&M, CF, teto, penalidade, crédito, prazo), sem precisar de UI
nova.

### 11.4 Save v38 e testes

Aditivo: `power.plants[].cf_base` (CF de sítio da usina nova; 0 = parque
calibrado), `power.cities[].wind_ms` (vento do sítio) e
`power.grids[].curtailment_mwh`. Save v37 carrega com as chaves ausentes = 0 —
a usina antiga continua calibrada e a cidade sem vento não inventa potencial.

Testes: `tools/renewables_balance_test.gd` (capex/MW e custo por MWh esperado
contra as térmicas; CF de sítio com teto e penalidade; variação diária presente
e determinística; térmica estável; curtailment; ano-base Ember ±1 % para
solar/eólica/hídrica em 365 d; média anual 1 da hídrica nova; save do `cf_base`),
`e2e3_power_test`, `e5_interconnect_test`, `e7_tariff_test` e
`economy_save_load_roundtrip_test` verdes.

### 11.5 Rodada 365 (mesmo controle da D-119/D-120)

`godot --headless --path . --script res://tools/c1_world_ai_365.gd --
--days=365 --out=res://docs/economy/perf/c1_world_ai_365_renew.json
--control=res://docs/economy/perf/c6_gate_base.json` (366 d de relógio, 306,7 s,
`threads=1`, `trajectory_sha256 d548cb17…`).

| métrica | D-120 (controle/prévio) | D-122 |
|---|---:|---:|
| falências | 2 | **2** |
| portão de comida (abaixo de 0,975) | 95/179 | **95/179** |
| extras vs `c6_gate_base` (93) | CIV, PRK, AUS | CIV, ATG, PRK, AUS |
| conta/PIB agregada | 1,248 % | **1,235 %** |
| conta/PIB p50 | 0,658 % | **0,641 %** |
| energia servida | 0,99610 | **0,99674** |
| violações de invariante / travados | 0 / 0 | **0 / 0** |

A troca declarada aparece no parque CONSTRUÍDO pela IA no ano: menos solar e
hidro nova, mais térmica despachável (a capacidade existente do ano-base é a
mesma nos dois mundos; o que muda é a obra nova). Com o combustível dentro do
critério da IA, o ano fecha dentro do portão do dono (falências ≤ 2, comida ≤ 95).
O preço da energia em PRT, o caso da falência extra da primeira medição sem o
combustível no critério, não mudou entre os mundos: aquela rodada ficou fora por
efeito de cadeia de mercados, e foi substituída por esta.
