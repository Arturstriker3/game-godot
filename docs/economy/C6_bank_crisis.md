# C6 — Crise bancária, corrida e resolução

Plano: `docs/plans/perf_pop_financas_plano.md`, PARTE C / C6.
Antecessoras: C1 (D-102, balanço), C2a (D-103, BC por moeda), C2b1 (D-104,
taxa/emissão), C2b2 (D-106, câmbio/padrão/ouro), C3 (D-107, crédito),
C4 (D-110, bancos com dono), C4b (D-111, crédito ligado), C5 (D-112, bolsa),
D-113 (calibração pré-C6).

Código: `gdextension/src/sim/bank_crisis.{h,cpp}`, campos novos em
`sim/sim_types.h` (`Bank`, `DepositInsurance`, `BankCrisisEvent`,
`BankCrisisPolicy`), `sim/bank.cpp` (aperto no `lend` e fusão),
`sim/sim_day.cpp` (fase `kPhaseBankCrisis`), `sim/sim_core.cpp` (snapshot,
reversão e `money_total`), `sim/balance.{h,cpp}` e `sim/central_bank.cpp`
(fundo na enumeração de caixa e na atribuição por moeda),
`sim/save_state.cpp` (save v34) e `src/economy_sim.{h,cpp}` (API/status).
Dados: `data/definitions/economy/bank_crisis.json` (todos os números são
**default de jogo declarado**, não dado observado) e `banks.json` (nomes
fictícios, como na C4).

## 1. O que a C6 faz

A C4 deu dono aos bancos e dinheiro bancário, mas declarou
`resolution: "C6"`: o patrimônio podia ficar negativo e ninguém socorria o
banco. A C6 fecha o ciclo com cinco peças, todas conservando dinheiro:

1. **Indicadores por banco/dia** — índice de capital (patrimônio/ativos),
   liquidez (caixa/depósitos) e inadimplência da carteira
   (atrasados/(empréstimos+atrasados)).
2. **Confiança do depositante (0..1)**, suavizada, função de solvência,
   liquidez, perdas recentes, contágio dos outros bancos da mesma nação e da
   confiança da bolsa (C5).
3. **Corrida** abaixo do limiar: uma fração declarada dos depósitos sai por
   dia e vira papel-moeda, depósito do banco público (fuga para qualidade) ou
   remessa da matriz estrangeira (fuga de capital).
4. **Emprestador de última instância** — o BC da moeda redesconta contra o
   colateral com taxa básica + spread, criando M0 registrado na dívida do
   banco. Dolarizado/sem moeda própria **não tem** emprestador.
5. **Aperto de crédito** abaixo do capital mínimo e **resolução** quando o
   patrimônio fica negativo: acionistas zerados → depósito não segurado →
   fundo de seguro → Tesouro/dívida pública → incorporação pelo banco público.

Nada aqui é feature de jogador nesta fase (API + status + testes); o painel e
as alavancas de política entram depois, como as demais fases financeiras.

## 2. Indicadores e a referência do depositante

Para cada banco com posição (ativos > 0 ou depósitos > 0):

```
ativos          = caixa + empréstimos + atrasados
capital_ratio   = patrimônio / ativos          (patrimônio pode ser < 0)
liquidez        = caixa / depósitos             (0 sem depósitos)
inadimplência   = atrasados / (empréstimos + atrasados)
```

Os indicadores são **publicados** todo dia. Só o índice de capital tem um
**mínimo regulatório declarado** (`capital.min_ratio`), usado no aperto de
crédito (§4); o mínimo de liquidez está **reservado** (0, nenhuma regra o
consome — ver §9).

**Confiança.** A referência de cada banco é o **melhor estado já conhecido**
(índice de capital e liquidez no primeiro fechamento com posição, seguido para
cima quando o banco melhora). A confiança mede **deterioração** contra essa
referência — não um nível absoluto. Isso é uma decisão de modelagem necessária:
o banco da C4 nasce com patrimônio ~0,4 % dos ativos e caixa ~0 (o crédito cria
depósito, não reserva), então um nível absoluto faria **todo** banco do mundo
base correr sem nenhum choque.

```
solvência = clamp(capital_ratio / capital_ratio_ref, 0, 1)
liquidez  = clamp(liquidez / liquidez_ref, 0, 1)
perdas    = clamp(1 - (perda_recente_EMA / ativos) / loss_scale, 0, 1)
contágio  = média da confiança dos OUTROS bancos da mesma nação no fecho anterior
            (banco único na nação = 1,0 — ausência declarada, não otimismo)
mercado   = clamp((confiança_da_bolsa_C5 - 0,65) / 0,70, 0, 1)
            (sem bolsa = 1,0)

raw        = Σ peso_i × sinal_i / Σ peso_i
confiança += (raw - confiança) × smoothing        (clamp 0..1)
```

Pesos declarados: solvência 0,30, liquidez 0,10, perdas 0,40, contágio 0,15,
mercado 0,05. A perda recente é uma EMA de ~10 dias; a **primeira** perda entra
inteira (o choque é visto no dia em que ocorre).

## 3. Corrida e emprestador de última instância

Abaixo de `run_threshold`, o banco entra em corrida:

```
saque_pedido = min(depósitos × run_share, depósitos)
```

1. **Fuga de capital da matriz estrangeira** (só papel `foreign`): até
   `capital_flight_share` do saque sai do caixa do banco e vai para a posição
   do exterior (soma zero; não mexe em depósito).
2. **Redesconto** (se há BC): o que falta de caixa para o saque é emprestado
   pelo BC da moeda contra o colateral `(empréstimos + atrasados) × haircut`,
   limitado a `max_share_of_assets` dos ativos. O valor entra em
   `bank.cash`/`bank.bc_borrowing` e em `emission_today` (M0 registrado).
3. **Pagamento**: `min(saque_pedido, caixa)`. O que sai vira:
   - **fuga para qualidade** (`flight_to_quality`): depósito e caixa migram
     para o **banco público** da nação (se existir e não for o próprio banco);
   - **papel-moeda**: o depósito cai e a reserva é liberada. O M0 **mantém a
     soma** (a reserva vira circulação: muda a composição, não o total), o M1
     também — é o que uma corrida faz de verdade. A variação de depósitos entra
     no `bank_money_today` do dia (mesma conta da C4).
4. **Dolarizado / sem moeda própria** (`fx_no_policy_nations`, IMF AREAER):
   **não há emprestador de última instância**. A corrida paga só o caixa
   disponível e para; nada de M0 novo. O Tesouro continua sendo o socorro de
   solvência (§5).
5. **União monetária**: o BC é o da moeda (EUR, XOF…) — a mesma entidade por
   moeda da C2a; o país não precisa emitir sozinho.
6. **Juro de penalidade** do redesconto: só o **spread** (a taxa básica já é
   cobrada pela fase de crédito da C3/C4), pago do caixa com a mesma convenção
   da C3 (pagar o BC destrói reserva e reduz a dívida registrada no dia).

## 4. Aperto de crédito

```
fator = 1                                     se capital_ratio ≥ min_ratio
fator = clamp(capital_ratio / min_ratio, floor, 1)   abaixo do mínimo
```

`lend()` multiplica o limite e o pedido pelo fator: banco abaixo do índice de
capital **corta crédito novo proporcionalmente**; no mínimo, zero. O fator é
publicado por banco (`credit_factor`). Como o banco da C4 não tem capital
semeado no mundo real, o aperto atua no começo da partida enquanto o
patrimônio retido é fino — medido em §11.

## 5. Resolução (patrimônio < 0)

`rombo = -patrimônio`. A ordem declarada e o dono de cada custo:

| # | etapa | quem paga | efeito no caixa |
|---|---|---|---|
| 1 | **Acionistas zerados** | dono do banco (`capital`) | nenhum (é claim); o papel na bolsa é baixado a 0 **se** houver (a C5 lista grupos — gancho declarado) |
| 2 | **Depósito não segurado** | depositantes acima do teto (`1 - insured_share`) | corte cobrado do caixa dos tomadores proporcional à dívida, limitado ao caixa de cada um; o depósito é destruído junto |
| 3 | **Seguro de depósito** | fundo da nação (prêmio dos bancos) | fundo → caixa do banco; nunca cria dinheiro |
| 4 | **Tesouro (caixa)** | orçamento público | prefeitura → caixa do banco (transferência) |
| 5 | **Tesouro (dívida)** | dívida pública carregada pelo BC da moeda (ou pelo exterior, sem BC; o título entra em `external_bonds` e no `Ledger.external_claims`) | caixa do banco + `gov_bonds` + emissão registrada (BC) ou posição externa (`foreign_cash`) |
| 6 | **Incorporação** | banco público | absorve saldos e o residual como perda do público (equity cai); o papel quebrado vira casca (`absorbed_into`, share 0) |
| 7 | **Liquidação** | depositantes (sem Tesouro/BC/banco público) | residual declarado no patrimônio negativo; nenhuma soma quebra |

**Custos.** O evento guarda `gap` e o custo de cada parte. Os custos que
**fecham o rombo** (`depositantes + fundo + Tesouro + incorporação`) somam
**exatamente** o `gap`; o capital dos acionistas é registrado à parte porque,
com patrimônio negativo, o claim já vale zero (não fecha buraco).

**Materialidade (`insolvency_tolerance`).** Patrimônio negativo de fração de
unidade é **arredondamento** do juro/dividendo, não rombo. A tolerância efetiva
é `max(1,0 XGS, ativos × 1e−6)`: a absoluta cobre bancos pequenos e a **razão**
cobre o ruído proporcional ao tamanho da carteira. Sem ela, o mundo base mediu
**14 “resoluções” de epsilon** (rombo de 1e−9 a 0,71 XGS); só com a absoluta,
**7 entre 1,01 e 1,68 XGS** em bancos de milhões — todas pagas pelo fundo e sem
qualquer efeito econômico. As duas tolerâncias são default de jogo publicado em
`policy`; o banco abaixo dela fica calmo e monitorado, sem entrar na hierarquia.

**Estrangeiro:** a matriz **só remete** — a corrida do papel estrangeiro leva
até `capital_flight_share` do saque para a posição do exterior (§3). O modelo
**não** tem socorro da matriz: a decisão de injetar capital não é simulada e o
abandono é representado pela remessa antes do saque. Diplomacia (sanções,
congelamento, reciprocidade) fica fora.

## 6. Conservação de dinheiro e a identidade da C1

Cada movimento fecha em uma das três formas:

- **transferência** (prêmio, fundo→banco, prefeitura→banco, banco→banco,
  banco→exterior): `money_total` não muda;
- **emissão registrada** (redesconto, dívida pública carregada pelo BC):
  `money_after = money_before + emission_today` e o passivo do BC cresce;
- **destruição de dinheiro bancário** (saída para papel-moeda, corte de
  depósito): `Δdepósitos` entra no `bank_money_today` e o caixa do depositante
  cai junto; M0 e M1 se conservam pelos agregados.

O **fundo de seguro** é caixa do mundo: entra em `SimCore::money_total()`, no
`Ledger.cash_total()`/`financial_sum()` (`insurance_fund`) e na atribuição por
moeda da C2a. O invariante diário (`financial_sum ≈ 0`, `cash_residual ≈ 0`,
M0 atribuível) vale em todas as fases da crise — ver `tools/c6_bank_crisis_test.gd`
(cenários f e g, incluindo o resgate sem BC com o título em poder do exterior).

## 7. Save v34 e migração v33 → v34

```json
{
  "version": 34,
  "banks": {
    "banks": {
      "BRA/private": {
        "confidence": 1.0, "loss_seeded": false, "loss_ema": 0.0,
        "loss_today": 0.0,
        "run_days": 0, "last_run_day": -1, "run_withdrawn": 0.0,
        "run_cash": 0.0, "run_public": 0.0,
        "lolr_borrowed": 0.0, "lolr_interest": 0.0, "lolr_days": 0,
        "insured_payout": 0.0, "bailin_cut": 0.0, "treasury_rescue": 0.0,
        "capital_flight": 0.0,
        "resolution_day": -1, "resolved": false, "resolution": "none",
        "absorbed_into": "", "baseline_seeded": false,
        "open_capital_ratio": 0.0, "open_liquidity_ratio": 0.0,
        "last_capital_ratio": 0.0, "last_liquidity_ratio": 0.0,
        "last_npl_ratio": 0.0
      }
    }
  },
  "monetary": {
    "gov_bonds": { "BRA": 0.0 },
    "external_bonds": { "BRA": 0.0 }
  },
  "bank_crisis": {
    "enabled": true,
    "policy": { "...": "regra salva (0 = default declarado)" },
    "funds": {
      "BRA": { "fund": 0.0, "premiums": 0.0, "payouts": 0.0,
               "last_payout_day": -1 }
    },
    "events": [
      { "day": 0, "bank": "BRA/private", "nation": "BRA",
        "kind": "resolution", "cause": "equity_negative", "gap": 0.0,
        "cost_shareholders": 0.0, "cost_depositors": 0.0,
        "cost_fund": 0.0, "cost_treasury": 0.0, "cost_takeover": 0.0,
        "capital_flight": 0.0, "lolr": 0.0, "emission": 0.0 }
    ],
    "events_total": 0
  }
}
```

**Migração v33 → v34:** aditiva. Sem o bloco `bank_crisis` e sem as chaves por
banco, `bank_crisis::after_load` inicia **todos os bancos calmos** (confiança
1,0, sem corrida, sem resolução, referência ainda não semeada), fundo 0 e
nenhum evento — nada é inventado para trás. `loss_today` viaja no bloco de cada
banco (continuação idêntica no meio do dia) e `monetary.external_bonds` guarda
os títulos do resgate sem BC em poder do exterior; ausentes em v33 = 0/vazio.
O anel de eventos lido do save é limitado a `kEventRing` (64), como o publicado
em `register_event`. Round-trip v34 é bit-exato e a continuação de 5 dias é
idêntica (cenário e).

## 8. Status A4 — `get_bank_status()`

Acrescenta ao contrato da C4:

```
policy:      crisis_enabled, capital_min_ratio, liquidity_min_ratio (reservado:
             nenhuma regra o consome), loss_scale, loss_ema_days,
             confidence_smoothing, run_threshold, run_share,
             flight_to_quality, lolr_spread, lolr_haircut, lolr_max_assets,
             insured_share, premium_daily, fund_target_ratio,
             capital_flight_share, treasury_debt_rescue, takeover_public,
             crisis_source, rescue_order
totals:      confidence_sum/min/max, fund, premiums, payouts, runs, run_days,
             resolutions, run_withdrawn, run_cash, run_public,
             capital_flight, lolr_borrowed, lolr_interest, insured_payout,
             bailin_cut, treasury_rescue, crisis_events
banks[]:     confidence, capital_ratio, liquidity_ratio, npl_ratio,
             credit_factor, run_days, last_run_day, run_withdrawn, run_cash,
             run_public, capital_flight, lolr_borrowed, lolr_interest,
             insured_payout, bailin_cut, treasury_rescue, resolved,
             resolution, resolution_day, absorbed_into
insurance[]: nation, fund, premiums, payouts, last_payout_day
events[]:    anel de 64 eventos (dia, banco, nação, tipo, causa, gap e custo
             por parte)
```

`confidence_min/max` consideram só bancos com posição (banco vazio não tem
sinal). `runs` é o número de bancos em corrida **hoje**; `run_days` é a soma dos
**dias em corrida correntes** (zera quando o banco sai da corrida — não é um
acumulado histórico). `capital_flight` é a remessa da matriz estrangeira; não
há campo `run_foreign` separado (o mesmo valor não aparece duas vezes no
contrato). `resolutions` conta o dia da resolução de **todo** banco, inclusive
a casca absorvida pelo banco público (ela ficou sem saldo, mas a resolução
aconteceu); `resolved_markets` considera só quem ainda tem linha com posição.

## 9. Parâmetros (`bank_crisis.json`)

Todos são **default de jogo** (`source: "default de jogo"`); não há fonte
pública de índice regulatório por banco, teto de seguro, prêmio de fundo,
spread de redesconto ou ordem de bail-in no repositório. O JSON é o espelho
declarado e o núcleo tem os mesmos valores compilados em
`sim/bank_crisis.h` (mesma convenção do `bank_credit.json` da C3/D-111); o teste
confere JSON × núcleo campo a campo.

| chave | valor | razão declarada |
|---|---|---|
| `capital.min_ratio` | 0,001 (0,1 %) | piso do **modelo**: o banco da C4 vive ~0,4 % de patrimônio/ativos; 0,1 % deixa margem para o crédito sem choque |
| `liquidity.min_ratio` | 0,0 | **reservado**: nenhuma regra consome o mínimo de liquidez (o sinal de confiança usa a referência do próprio banco); declarado para não prometer efeito inexistente |
| `losses.confidence_scale` | 0,02 | perda de 2 % dos ativos zera o sinal de perdas |
| `losses.ema_days` | 10 | memória da perda recente |
| `confidence.smoothing` | 0,12/dia | sem saltos diários |
| `confidence.weights` | 0,30/0,10/0,40/0,15/0,05 | solvência/liquidez/perdas/contágio/mercado |
| `confidence.run_threshold` | 0,35 | limiar da corrida |
| `confidence.run_share` | 0,08/dia | fração dos depósitos que sai por dia |
| `confidence.flight_to_quality` | 0,45 | fatia do saque que migra ao banco público |
| `lolr.spread` | 0,03 | penalidade sobre a taxa básica |
| `lolr.collateral_haircut` | 0,70 | colateral aceito |
| `lolr.max_share_of_assets` | 0,50 | teto do redesconto |
| `insurance.insured_share` | 0,60 | teto do seguro declarado como fatia dos depósitos (o modelo não tem cadastro de depositante) |
| `insurance.premium_daily` | 0,00005 | prêmio diário sobre depósitos (só do caixa disponível) |
| `insurance.fund_target_ratio` | 0,01 | alvo do fundo (1 % dos depósitos segurados) |
| `resolution.capital_flight_share` | 0,25 | fatia do saque remetida pela matriz estrangeira |
| `resolution.insolvency_tolerance` | 1,0 XGS | materialidade absoluta: rombo de fração de unidade é arredondamento, não insolvência |
| `resolution.insolvency_tolerance_ratio` | 1e−6 dos ativos | materialidade relativa: o ruído de juro acompanha o tamanho da carteira |
| `resolution.treasury_debt_rescue` | true | Tesouro pode emitir dívida carregada pelo BC |
| `resolution.takeover_public` | true | residual pode virar banco público |

## 10. Teste — `tools/c6_bank_crisis_test.gd`

O runner descobre o teste sozinho (`*_test.gd`). Cenários forçados:

| # | cenário | o que verifica |
|---|---|---|
| P | JSON × núcleo | todos os parâmetros do arquivo são os do default compilado |
| a | perda de 30 % na carteira do banco privado | confiança cai, há corrida, o BC redesconta, o banco **sobrevive** (sem resolução) e segue solvente |
| b | perda de 60 % (insolvente) | bail-in + fundo + Tesouro; `depositantes+fundo+Tesouro+incorporação == gap` |
| c | contágio | banco da mesma nação perde confiança; banco de USA não |
| d | dolarizado | sem redesconto (`bc_borrowing = 0`), a corrida para no caixa |
| e | save v34 | round-trip bit-exato e continuação de 5 dias idêntica |
| f | conservação | identidade da C1 e `invariant_violations = 0` em todos os dias de todos os cenários |
| g | resgate sem BC | dolarizado + Tesouro sem caixa: o rombo vira dívida em poder do exterior (`external_bonds`); custos fecham o rombo, a identidade passa e o mundo avança (o tick não é revertido) |

O fixture é o cenário AGRIA/PORTUS (1 grupo); o teste declara 100 % da
participação no papel-alvo no save para a carteira ir para o banco certo e usa
o grupo real do fixture (sem inventar grupo, cidade ou dinheiro).

## 11. Medição 365 d (mundo base, IA, `threads=1`, DLL debug)

Saída: `docs/economy/perf/c6_gate_base.json` (harness `tools/c1_world_ai_365.gd`,
uma rodada). Default do jogo (crédito ligado, juro da safra ligado).

| medida | antes (D-113/D-111) | C6 | regra |
|---|---|---|---|
| falências | 2 | 2 | ≤ 2 |
| portão de comida | 94/179 | 93/179 (mediana 0,9702) | ≤ 98 |
| violações do invariante | 0 | 0 | 0 |
| dias travados | 0 | 0 | 0 |
| corridas / resoluções / eventos | — | **0 / 0 / 0** | idealmente 0 |
| fundo de seguro / prêmios | — | 204.853,29 / 204.853,29 | transferência do caixa do banco (dividendo da carteira C5); não cria dinheiro |
| redesconto / fuga de capital | — | 0 / 0 | — |
| M0 / M1 | 6.523.551.982,90 / 6.548.409.274,25 | 6.523.551.982,90 / 6.556.188.871,83 | M0 conservado |
| dinheiro (drift) | 0,003812 (dinheiro bancário) | 0,005436 (dinheiro bancário) | sem vazamento |
| dívida dos grupos / contratos | 24.551.743,26 / 2.515 | 32.599.940,73 / 2.529 | — |
| hash da trajetória | `66534505` | `9fcd0caa` | novo formato (D-105 + C6) |

Re-rodado após as correções da QA (t16): mesmas falências, portão, violações,
fundo e hash — o resgate sem BC não toca o mundo base (0 resoluções). O JSON de
evidência foi regravado (só o `wall_s` muda).

**Golden A0 (60 d, IA + urbano, semente 0) — antes/depois da C6:** a fase muda
o mundo base pelo aperto de crédito. `money_total` 6.527.733.844,71 →
**6.527.733.251,16** (−593,55; −0,000009 %) e negócios de serviço 39.959 →
**39.960**; 0 falências nos dois. Rodado 2 vezes com save/status shas
idênticos; a equivalência CLI × Godot A0 (10 d) saiu byte-idêntica (0
diferenças com tolerância 0). Os goldens S0 (`real_60_ai`, `real_60_noai`,
`demo_365_noai`) só mudam o `save_sha256` (bloco v34: `loss_today`,
`external_bonds` e sem os campos mortos); o `status_sha256` do S0 é o mesmo.
Goldens v34 pós-QA: A0 save `5fadba28…` / status `f6ec0427…`; `real_60_ai`
`edc686ee…` / `1c2a85fa…`; `real_60_noai` `6d657f25…` / `8ae9f20b…`;
`demo_365_noai` `dc67ad5c…` / `10583589…`. CLI × Godot A0 (10 d): sha256
`5D178D9D…EEB44249`, 82.744.869 B, **0 bytes diferentes**.

Leitura: **o mundo base não piorou** — as duas falências são as mesmas, o
portão até melhora em 1 nação, o invariante fecha em todos os dias e não há
corrida nem resolução. O fundo de seguro acumula **204.853,29** durante o ano
(os bancos recebem dividendo da carteira da C5 e pagam o prêmio do caixa; o
prêmio é limitado ao patrimônio para não criar insolvência de centavos). O crédito de empresa cresce (32,6 M contra 24,5 M) e o
dinheiro bancário acompanha (drift 0,0054, que **é** o estoque de depósitos,
não vazamento): o aperto de crédito do começo da partida (patrimônio retido
ainda fino) muda a trajetória do crédito sem piorar nenhum dos portões. O hash
de trajetória muda porque o digest da D-105 passou a incluir confiança, eventos,
fundo e fuga de capital (§12) — formato novo, comparação válida a partir daqui.

## 12. Digest de trajetória (D-105) com os estados da C6

`core/economy/trajectory_digest.gd` passou a incluir, por dia:
`soma da confiança dos bancos com posição`, `total de eventos de crise`,
`saldo do fundo` e `fuga de capital acumulada`. Como o formato mudou, os hashes
anteriores (`66534505…`, `f759f4c0…`) não são comparáveis com os novos — a
partir da D-114 o hash mede também o estado de crise.

## 13. Dívidas declaradas

- **Capital bancário no mundo base**: a inicialização do jogo não semeia
  capital de banco (0 no save novo); o patrimônio cresce do juro retido. O
  aperto de crédito do começo é consequência declarada disso, não uma
  calibração do C6 (a semeadura de capital é decisão de outra fase).
- **Teto do seguro**: o modelo não tem cadastro de depositante; o teto é uma
  fatia declarada dos depósitos (60 %). Um teto por depositante exige a camada
  de contas (futura). O **prêmio não pode levar o patrimônio a negativo**: um
  banco sem capital paga prêmio só até o patrimônio que tem (sem isso, o próprio
  prêmio criava uma “insolvência” de centavos e a resolução era paga pelo fundo
  que o prêmio acabou de encher — artefato medido e corrigido).
- **Banco listado**: a C5 lista grupos, não bancos; o gancho de baixar o papel
  a zero existe, mas hoje nenhum banco tem papel. Quando a C5 listar bancos, a
  regra do §5(1) já está pronta.
- **Resolução repetida**: um banco resolvido por bail-in/fundo/Tesouro continua
  monitorado; só a liquidação sem lastro não se repete (não há o que cortar).
- **Diplomacia do banco estrangeiro** (sanções, congelamento, reciprocidade,
  abandono da matriz como ato político) fica fora, como na C4.
- **Painel de UI e alavancas de jogador** (resgate, exigência de capital,
  controle de capitais) ficam para depois da Fase H, como as demais fases.
