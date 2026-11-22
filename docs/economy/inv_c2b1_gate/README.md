# Investigação — contradição do fechamento da C2b1 (hash × portão de comida)

Data: 2026-11-20. Base investigada: `balance/global-resources` @ `26b12472` (C2b1/D-104).
Worktrees temporários (removidos ao fim): `G:/projetos/opengs-wt-c2b-inv` (branch `inv/c2b1-gate`)
e `G:/projetos/opengs-wt-c2a-base` (detached `71c980cc`). Nada foi commitado, mesclado ou enviado.

## Como foi medido

- Mesmo harness nos dois lados: cópia instrumentada de `tools/c1_world_ai_365.gd`
  (`tools/_tmp_c2b_inv_365.gd`), mesmo mundo do autoload, `set_sim_threads(1)`, IA ligada,
  `--days=365`, Godot 4.7.2 `--headless`, extensão GDExtension compilada em **Debug**
  (mesmo perfil `build-debug` do relatório) em cada worktree.
- Um Godot por vez; RAM livre ≥ 12 GB; parede ≈ 200 s por rodada.
- Os dois números do relatório foram **reproduzidos exatamente** (85/179 e 88/179, mesmo hash
  legado), portanto a divergência não é ruído de máquina.

## 1. O que o hash de trajetória cobre

`tools/c1_world_ai_365.gd` (linha 358) e `tools/g2_world_ai_365.gd` (linha 238):

```gdscript
hash_lines.append("%d:%.2f:%d" % [day_now, money, int(status.get("bankruptcies", -1))])
```

Cobre **só** três escalares por dia: número do dia, `money_total` em **2 casas decimais** e
`bankruptcies`. **Não** cobre: crédito da safra, estoque privado/público, M0/títulos, taxa
básica, inflação, preços, razões de comida — nada do que a C2b1 mudou.

Prova direta: quatro mundos materialmente diferentes têm **o mesmo** hash legado
(`f7631ba0…`), enquanto o hash estrito (dinheiro com 6 casas + falências + soma das razões de
comida) separa os quatro:

| rodada | hash legado | hash estrito |
|---|---|---|
| C2a | `f7631ba0ba09…` | `16835b2b45f3…` |
| E1 (só silo) | `f7631ba0ba09…` | `9a6c9e3de8cf…` |
| E2 (só crédito) | `f7631ba0ba09…` | `730067fdbe72…` |
| C2b1 | `f7631ba0ba09…` | `15136804d9ad…` |

O próprio `money_total` já difere na 4ª casa (6 523 551 982,9022 → …982,9013), invisível no
`%.2f`. O hash não é um atestado de "mundo idêntico": é um atestado de "dinheiro com 2 casas e
contagem de falências iguais".

## 2. 365 d com IA — números lado a lado

| medida | C2a `71c980cc` | E1 só silo | E2 só crédito | C2b1 `26b12472` |
|---|---|---|---|---|
| portão de comida (< 0,975) | **85/179** | **86/179** | **87/179** | **88/179** |
| mediana da razão (30 d) | 0,9797876 | 0,9778278 | 0,9778951 | 0,9778951 |
| falências | 2 | 2 | 2 | 2 |
| violações / dias travados | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 |
| dinheiro total (fim) | 6 523 551 982,9022 | …982,9017 | …982,9014 | …982,9013 |
| estoque privado (t) | 59 659 963 | 59 590 418 | 59 722 042 | 59 708 745 |
| M0 medido (fim) | 14 471 319 268 | 14 471 324 522 | 14 470 740 482 | 14 471 050 711 |
| taxa básica p50 / p10 / p90 | 0 / 0 / 0 | — | — | 0,52 % / 0,00 % / 6,11 % (136 moedas) |
| inflação anualizada p50 / p10 / p90 | 0,91 / 0,71 / 4,51 % | — | — | 0,91 / 0,71 / 4,37 % (179 países) |
| emissão total | 0 | 0 | 0 | 0 |

E1 = C2b1 com o crédito da safra desligado (`policy_rate = 0` em `agriculture.cpp`).
E2 = C2b1 com o custo de capital do silo desligado (`capital_charge = 0` em `ai_storage.cpp`).
Ambos revertidos depois (worktree sem alteração de código ao fim).

## 3. O mecanismo da piora (real, não ruído)

- **Canal crédito da safra (D-099)** — `agriculture.cpp:453-460`: o envelope vira
  `asset / (60 · (1 + r · 60/365))`, e `labor.cpp:561-564` multiplica a **folha paga** da
  fazenda por esse fator. Em taxa 0 o comportamento é o de antes. Sozinho: 85 → **87** e move
  **124/179** países. Entraram no portão: **CIV, BHR, RUS**; saiu: JOR.
- **Canal custo de capital do silo (G2)** — `ai_storage.cpp:193-198`: o payback do silo privado
  passa a exigir `build_cost · anos · taxa`. Sozinho: 85 → **86** e move **101/179** países.
  Entraram: **LBR, PRK**; saiu: JOR.
- Juntos: **88**. A atribuição do relatório ("+2 crédito, +1 silo") está **correta**; o que está
  errado é a leitura do hash.
- Produção agrícola do ano é **idêntica** nos quatro (32 463 948,696 t): o canal não muda a
  safra física, muda a **renda** (folha paga) e o consumo das famílias.
- O desconto doméstico direto é pequeno (1 − 1/(1+r·60/365): 0,33 % em BHR a 1,95 %,
  3,0 % em KOR a 18,8 %), mas o portão é um agregado com propagação por preços/comércio — países
  com taxa **zero** (CIV, LBR) também se movem. BHR cai 8,4 pp com taxa 1,95 %: efeito de
  amplificação/preço, não do próprio juro.
- Falências não mudam (2 em todas as rodadas): a piora não passa por fechamento de planta.

## 4. Proposta (não implementada)

1. **Corrigir o hash (medição, custo ~0):** somar ao `hash_lines` o dinheiro com 6 casas e a soma
   das razões de comida do dia (já calculadas em `_food_ratio`) e publicar no JSON um digest
   mensal com estoque privado e M0. Sem isso nenhum A/B futuro da política monetária é auditável.
2. **Portão:** o juro da linha da safra hoje é **custo sem credor** (não há banco até a C4 — a
   própria seção 6 da `C2b1_monetary_policy.md` declara isso) aplicado sobre uma regra de
   **liquidez**. Menor correção: manter a taxa medida/publicada no status, mas **não** encolher
   o envelope até existir credor → portão volta a **86/179** (medido em E1), com o número
   honesto e sem mascarar. Alternativa B, se o canal tiver de continuar comportamental: usar a
   taxa **real** (nominal − CPI 12 m, piso 0) no envelope — exige nova rodada de 365 d.
3. Se o critério de investimento do silo (E1, +1) for mantido: declarar 86/179 e atualizar
   tabela, goldens e o `C2b1_monetary_policy.md`; a linha "hash de trajetória igual" sai.

## Evidências brutas

`run_c2a_71c980cc.json`, `run_c2b1_26b12472.json`, `run_e1_storage_only.json`,
`run_e2_credit_only.json` (saída integral do harness instrumentado: portão por país, digest
mensal, estoque por tipo, taxas por moeda/nação, produção por país).
