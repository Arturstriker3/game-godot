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
  `power.cpp`).

Regressão coberta: `economy_save_load_roundtrip_test`, `economy_integration_test`,
`c3_bank_credit_test`, `c4_banks_test`, `c5_stock_exchange_test`, `c6_bank_crisis_test`
(pinos de versão de save v34→v35), `market_template_test` e `goods_data_test`.

## 6. Rodada 365 dias (evidência)

<!-- RODADA_365 -->
Rodada canônica: `godot --headless --path . --script res://tools/c1_world_ai_365.gd --
--days=365 --out=res://docs/economy/perf/c1_world_ai_365_e2e3.json`
(365 d, threads=1, ~240 s, saída `docs/economy/perf/c1_world_ai_365_e2e3.json`).
"Antes" = `docs/economy/perf/c1_world_ai_365.json` (mesmo mundo, sem energia ligada).

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


## 7. Dívidas declaradas

- Vento sem série mensal (GWA não publica) — fator anual, declarado.
- `oil: null` na matriz Ember — capacidade de óleo não publicada por país (D-115(d)).
- Conexão parcial é um limite de troca por cidade (não se modela fluxo físico entre
  subestações); interligação internacional (E5) e obra de redução de perdas ainda não.
- E4 detalhada (estratos/indústria por bem), E6 (mapa) e E7 (distribuidora) fora desta
  fase.
