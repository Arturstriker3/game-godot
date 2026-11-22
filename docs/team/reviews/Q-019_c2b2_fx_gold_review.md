# Q-019 — Revisão de qualidade da C2b2 (D-106), branch `feat/c2b2-fx-gold`

Data: 2026-11-20. Revisor: gerente-qualidade.
Escopo revisado: `34c8002b..875fdbf1` (7 commits; 2 já existentes). Nada executado
neste parecer — a bateria `tools/run_tests.ps1 -All` estava rodando no worktree.

## Veredito

**APROVADO COM RESSALVA.** O default atende à regra dura do dono e o hash de
trajetória é bit-idêntico ao da base D-105. As ressalvas são de **documentação**
(números que não descrevem o que o código faz), não de código; nenhuma exige
remedição. Não há veto.

## Regra dura do dono (default 365 d)

| critério | exigido | medido (`c2b2_world_ai_365_run{1,2}.json`) |
|---|---|---|
| portão de comida | não pior que 86/179 | **86/179** (`below_gate=86`, mediana 0,9778281) |
| falências | = 2 | **2** |
| violações de invariante | = 0 | **0** (`invariant_violation_samples` vazio, travados 0) |
| hash idêntico à base D-105 | — | **`f759f4c0…abeafb`**, igual |

run1 e run2 são **byte-idênticos exceto `wall_s`** (192,207 s / 192,607 s) — conferido
por diff normalizado. Dinheiro final 6 523 551 982,90166, drift máx. diário 4,18e-13.

## Evidências conferidas contra os JSONs

- **Estresse fraco** (`c2b2_fx_scenario_365.json`, `--fx-seed-days=30`):
  `funded_currencies=47`, reservas 5 399 876 805,29843 âncora, `devaluations=0`,
  `vault_gold_tonnes=32 839,7073`, hash `f759f4c0…` = default. Confere com §9.2.
- **Estresse forte** (`c2b2_fx_stress_365.json`): 136 moedas em `peg` (`regimes.peg=136`),
  `currencies_devalued=47`, `devaluations=47`, 47 moedas distintas com `devaluations=1`,
  `reserves=0`, `bankruptcies=2`, `invariant_violations=0`, `below_gate=90` (mediana
  0,9749213), `money_end` 6 499 950 986,03863 (drift −0,0036178), `coverage_mean`
  0,00697394, hash `af8f6939…`, 195,8 s. Distribuição por dia **idêntica** à do doc:
  21 dias, min 31 / max 61, soma 47 (6/3/2/1/3/1/2/1/2/2/3/1/2/**7**/1/4/1/2/1/1/1).
  Amostras PGK/QAR/SBD no dia 31 confirmadas.
- **Variante rejeitada** (`c2b2_vault_maintenance_365.json`): 3 falências, `below_gate=85`,
  mediana 0,9779577, hash `e5389c77…`, dinheiro 6 523 551 982,90254. Números do doc §4.1
  conferem (a leitura deles, não — ver R2).
- **Goldens** (`A0_60_urban.json`, `real_60_ai.json`): `status_sha256` inalterados
  (`8047b236…`, `60adbc2d…`), `save_sha256` trocados (`e6c4564b…`, `b6dbebc1…`),
  demais campos (money_total, bankruptcies 0, products, service_businesses) idênticos.
  Diferença de payload é só o bloco aditivo `fx` do v29 — leitura correta.
- **v28 → v29**: migração coberta por `tools/c2b2_fx_gold_test.gd:411` (`old["version"]=28`);
  os 13 testes do commit `875fdbf1` só trocam constante/comparação de versão (e duas
  mensagens de falha) — nenhuma referência a `28` sobrou em `tools/*.gd`.
- **Código**: `storage_type_accepts` mapeia produto→categoria por nome
  (`kVaultProducts`), não por índice de `goods.json`; o cofre só passa a aceitar a própria
  categoria. `phase_storage` isenta manutenção por `storage_type_is_structure_only`.
  Lacuna §8 confirmada no código (`exchange.cpp:38-41` e `:325`): em `peg` o sombreado é
  `base_rate`, então paridade = base não gera gap.

## Ressalvas (documentação; corrigir antes do merge)

1. **§9.3, unidade errada: "23,6 bi de moeda local recolhida".** A diferença de dinheiro é
   6 523 551 982,90 − 6 499 950 986,04 = **23 600 996,86 → 23,6 mi** (milhão), não bilhão.
   É exatamente −0,362 % de 6,52 bi; o "bi" está errado por 1000×.
2. **§4.1 e D-106: "derrubava uma nação no portão" está invertido.** `below_gate` conta
   nações **abaixo** do portão (`c1_world_ai_365.gd:174`), então 85 < 86 é **melhor**, e a
   mediana da variante rejeitada também é melhor (0,9779577 > 0,9778281). O único
   regresso da variante é a **3ª falência** (e o hash sair da base). O motivo do veto à
   manutenção é esse — a frase do doc e a do `GAME_STATE.md` precisam ser reescritas.
3. **§9.3: "todas as 136 moedas … reservas = 3 dias".** Só **47** foram fundadas
   (`funded_currencies=47`, as com importação medida). As outras 89 ficam em `peg` sem
   reserva e caem no ramo administrativo (`exchange.cpp:329`): não defendem, não
   desvalorizam. As 47 desvalorizações são exatamente as 47 fundadas — isso é bom, mas o
   texto deveria dizer; hoje "47 de 136" lê como se as 136 tivessem sido defendidas.
   Idem "reserva 0 (136 moedas sem reserva)": 47 esgotadas + 89 nunca fundadas.
4. **§9.3: a reserva semeada não é 3 dias ao câmbio de mercado.** O comando de `peg`
   aplica na hora (`dispatch_command`, fora do dia), então `seed_reserves_from_imports`
   divide pela paridade já sobrevalorizada (0,8×): 674 984 600,66 = **1,25 ×** os 3 dias
   de mercado do cenário fraco (539 987 680,53). Continua conservador (esgota no dia 31),
   mas ou se diz "3 dias à paridade do cenário", ou se semeia antes de trocar o regime.
5. **Ouro só?** O doc fala "para o bem gold", mas o cofre isento e com capacidade aceita
   **toda a categoria** `kVaultProducts` (gold, diamond, uranium, rare_earths, gold_bar,
   processed_rare_earths, jewelry), com capacidade compartilhada e sem manutenção. Hoje o
   impacto é nulo (nenhum cofre/paiol tem capacidade fora do ouro semeado — provado pelo
   hash idêntico à base), mas ou se documenta, ou se restringe a aceitação a `gold`.
6. **Provos sem artefato:** "CLI × Godot 60 d, 849 cidades, 0 diferenças, tolerância 0"
   (§9.4) e "A0: 0 diferenças entre rodadas" só existem como linha de tabela. A D-104
   versionou o SHA do dump do CLI; aqui não há arquivo. Recomendo um JSON curto em
   `docs/economy/perf/` com sha/exit/contagem para o fechamento.
7. **Nits:** D-106 cita base `30c98e67`, mas o ponto de ramificação é `34c8002b`
   (`git merge-base --is-ancestor` confirma). Nomes de arquivos `.uid` não rastreados no
   worktree não entram no commit.

## Perguntas explícitas

1. **Isentar via `storage_type_is_structure_only` (cofre + paiol) é aceitável?** Sim — o
   predicado já governa "estrutura não recebe produto nem recebe obra da IA", e o hash
   idêntico à base prova que nenhum paiol tinha capacidade para gerar fluxo. Ressalva R5:
   o efeito real é a categoria inteira do cofre, não só o ouro; declarar ou restringir.
2. **Regerar A0/S0 com o mesmo `status_sha256` é a leitura correta?** Sim: o hash de
   status não inclui a versão do save e o bloco é aditivo; status e dinheiro idênticos =
   mundo parado, payload mudou. O único guarda do formato antigo passa a ser o teste de
   migração 28 → 29 — ele existe.
3. **Regra dura atendida?** Sim: 86/179 (= base), falências 2, violações 0 — e o hash
   bit-idêntico à D-105 é prova mais forte que o próprio portão.
4. **Peg com paridade = base_rate nunca gasta reserva: lacuna registrada?** Sim, em §8
   (último item), em §9.3 ("Achado de mecânica") e em D-106; confirmado no código. Não é
   bug escondido — mas ver R3: a consequência (89 moedas sem defesa) merece uma linha.
