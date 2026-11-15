# Parecer Q-004 — Revisão do conserto de serviços (S-002)

> Gerente-qualidade · 2026-11-15 · commits revisados: `b13faa6` (testes vermelhos) e `1bd048b` (fix F8)
>
> **Veredito: VETADO** — com qualificação: o núcleo do bug F8 está **correto e resolvido**;
> o veto é **estrito e único** sobre a regressão de comida de DJI, que viola o critério de
> aceite "sweep 365 d sem nação pior vs `f9_invest_world_sweep`".

---

## 1. O que foi revisado

| Commit | Conteúdo | Conclusão |
| --- | --- | --- |
| `b13faa6` | Ajusta `services_test` (save v10→v11) e `production_chain_test` (invariante de trabalho) | **Legítimo — não esconde bug** (§2) |
| `1bd048b` | Remove teto de 40 postos, folha sobre efetivos, custo de trabalho limitado, `capital_per_unit` ÷10, `opens` 1→5, goldens S0 regenerados | **Bug principal resolvido; DJI regride** (§3–§6) |

## 2. `b13faa6` — os testes vermelhos eram contrato mudado, não regressão

- `services_test` esperava **save v10**; o F9 (`56113de`, "save v11 com migração de v10") subiu a
  versão. O ajuste para v11 é **correto** — confirmado no commit do F9.
- `production_chain_test::_test_labor_priority` testava o **racionamento por classe (§5.3 do F2)**
  (`primary_fill`/`secondary_fill`). O F7 (`64ed8c8`) substituiu esse mecanismo por **matching por
  escolaridade** (`demo_labor_day`, `education_mix`). Verifiquei no código atual: não há mais
  `primary_fill`/`secondary_fill`; a escolaridade (`education`, `education_share`,
  `demo_education_mix_for_sector`) é o mecanismo vigente. A reescrita para as invariantes reais do
  F7 (fazenda mantém folha; fábrica contrata sem exceder a própria folha; emprego ≤ força de
  trabalho) é **legítima** e não afrouxa para esconder bug: a asserção nova continua verificando um
  limite real (`factory_workers <= factory_jobs`).
- **Nenhum arquivo do núcleo foi alterado nesse commit** (só `tools/*.gd` e `GAME_STATE.md`). ✅

## 3. Build e bateria

- **Build debug + release**: `cmake --build gdextension/build-debug` e `--build-release` retornaram
  `ninja: no work to do` (exit 0) — o binário já está em dia com o commit `1bd048b`. Sem erros,
  sem warnings pendentes.
- **DLL debug real**: `gdextension/bin/economy_gdextension.windows.template_debug.x86_64.dll`
  presente (13.252.096 B, hash `50F721F1…`, timestamp do commit). Observação não-bloqueante: a DLL
  debug ativa difere do backup `.real_debug` (14.188.032 B, `51C337A0…`, de 14:26) — o `.real_debug`
  é o build debug **anterior** ao fix e não foi atualizado. Não afeta a auditoria (a DLL ativa é a
  do commit), mas recomendo regerar o `.real_debug` no fim da sessão de build.
- **Bateria (8 testes, exit code)**: `economy_integration` 0 · `economy_ai` 0 ·
  `population_demo` 0 · `production_chain` 0 · `investment` 0 · `labor_market` 0 · `pop_needs` 0 ·
  `services` 0. **8/8 verdes.**
  - Nota de processo: `services_test.gd` é `extends Node` e roda **pela cena**
    `res://tools/services_test.tscn`, não por `--script` (por `--script` o processo não instancia o
    MainLoop e pendura). Rodado pela cena, passou em 7 s.

## 4. Audit 90 d (mundo real + IA) — `services_business_audit_test.gd`

| Métrica | Declarado (S-002) | Medido | Status |
| --- | --- | --- | --- |
| Empregados com exatamente 40 postos | 0% | **0,0%** (share_40) | ✅ |
| Mediana de postos por negócio | 462 | **462,1** (min 18 · max 553.496) | ✅ |
| Demanda atendida | 86,5% | **86,51%** (não-atendida 46,04%) | ✅ |
| Falências de serviço | 278 (era 33.914) | **278** (por mês [0, 3, 275]) | ✅ |
| Margem (receita−custo)/receita | +60,5% | **+60,52%** | ✅ |
| Dinheiro conservado | drift ≈ 0,0001 | **0,000064** (2.200.539.587,40 → …587,40) | ✅ |

- Lucro diário: **844.330 positivos / 12.213 negativos / 8.260 zero** (era 443 pos / 575.359 neg).
  A distribuição inverteu de "estruturalmente negativo" para "majoritariamente positivo".
- Findings: apenas `seed_profit_zero` (severity **info**, artefato de captura do dia 0 — esperado).
  **Nenhum finding de bug** (`employees_stuck_at_40`, `service_bankruptcy_churn`,
  `profit_overwhelmingly_negative`, `demand_largely_unserved` todos ausentes).
- Plausibilidade por país (rico × pobre): o teto `business_wage` (§6) só é vinculante em país rico
  (onde o salário de mercado excede o valor gerado por posto); em país pobre o salário de mercado
  fica abaixo e vale integralmente — comportamento coerente com o desenho. A mediana de 462 postos
  confirma que a distribuição **varia** (não mais travada em 40).

## 5. Sweep 365 d × 5 variações

- **Falências de construções**: 16/16/16/16/16 (limite **28**) ✅.
- **Falências de serviço**: **1.522** (era 217.757, −99,3%) ✅, 61.662 negócios.
- Robustez cambial: os 5 JSONs por variante têm SHA-256 idêntico → trajetória invariante a FX/semente.

### O problema do baseline (achado de evidência)

O critério de aceite do S-002 é "sem nação pior em comida **vs `f9_invest_world_sweep`**". O script
`economy_world_sweep.gd` tem `DEFAULT_BASELINE = f4_world_ai_365.json` hardcoded e só lê baseline no
formato `nations`-como-dicionário (`food` por nação); o `f9_invest_world_sweep.json` tem `nations`
como **array** (`worst_food`), então **não pode ser usado como baseline do script**. O relatório
commitado (`services_fix_world_sweep.json`) foi, portanto, gerado **contra f4**, não contra f9.

| Nação | vs f4 (o que o script mediu) | vs f9_invest (o critério real) |
| --- | --- | --- |
| **DJI** | 0,3907 → 0,3552 (**−3,5 p.p.**) | **0,4717 → 0,3552 (**−11,65 p.p.**)** |
| **MRT** | 1,0000 → 0,9579 (**−4,2 p.p.**) | **0,9516 → 0,9579 (**+0,62 p.p.**)** |

Ou seja, a comparação declarada pelo @gerente-simulacao ("DJI regride 0,472→0,355; MRT melhora
0,9516→0,958") é a **correta** (vs f9), e bate com a medição. Mas o artefato commitado usa f4 e
**mostra MRT como pior caso**, o que induz a erro. **Recomendo** (não bloqueia este parecer, é
pendência do gerente-simulacao): ou o sweep passa a aceitar `--baseline=f9_invest_world_sweep.json`
(normalizando o formato `nations`), ou o relatório commitado registra a comparação f9 ao lado da f4.

### Veredito do critério

- Tolerância de comida: **2,5 p.p.** (`FOOD_TOLERANCE = 0.025`).
- **DJI viola**: −11,65 p.p. vs f9 (≈ 4,7× a tolerância). É **regressão nova** (a F9-investimento
  tinha elevado DJI de ~0,34 para 0,472; o fix de serviços devolve para 0,355, anulando a melhoria).
- **MRT melhora** (+0,62 p.p.) — não é regressão contra o critério real.
- Nenhuma outra nação tem queda relevante.

## 6. Avaliação conceitual (perguntas da revisão)

### 6.1 O limite "custo de trabalho ≤ preço÷jobs_per_unit×0,68" é uma muleta?

**Sim, é uma muleta de calibração — mas não vaza dinheiro.** Análise:

- `business_wage = min(wage, (price/jobs_per_unit) × 0.68)`. O `employment_note` de `services.json`
  declara a calibração: "receita por posto ~0,22 XGS/dia (salário ~0,15 + insumos)", ou seja,
  0,15/0,22 = **0,68** — o teto reproduz a âncora salarial declarada.
- **O problema:** o preço do serviço é essencialmente **fixo** (~0,22/posto), enquanto o salário de
  mercado (F7, `demo_labor_day`) é **dinâmico**. Em país rico o salário de mercado excede o valor
  gerado por posto; sem o teto, o negócio formal ficaria estruturalmente no prejuízo. O teto "congela"
  o custo **contábil** do negócio no breakeven × 0,68, **desacoplando-o do salário real pago**.
- **Não é muleta contábil-monetária:** a folha **real** (`city.service_wage_bill`, paga em
  `demo_labor_day`) continua pagando o salário de mercado integral. O `business.wages` é apenas a
  **atribuição marginal** usada para calcular `profit`/`margem`. O teto **não cria nem destrói
  dinheiro**; ele mascara a inconsistência preço×salário na **métrica exibida**, não na economia real.
- **Conclusão:** é uma muleta de *apresentação/calibração* que esconde que o preço do serviço está
  sub-calibrado em relação ao salário de mercado em país rico. A correção de raiz é tornar o preço do
  serviço sensível ao salário (ou recalibrar `price`/`jobs_per_unit` para breakeven ≥ salário em país
  rico), ou aceitar explicitamente que o lucro por negócio é uma medida contábil e não econômica.

### 6.2 A margem média de +60% é realista?

**Não.** Margem líquida de +60% é irrealista para serviços (referência real: retail ~3–5%,
alimentação ~5–10%, serviços profissionais ~15–30%). Ela é o subproduto direto da muleta do §6.1:
o teto garante folha ≤ 68% da receita por posto, e como os insumos de bens são baratos
(236.540 no total vs folha 324,2 M) e os insumos de serviço modestos (94,6 M), a margem dispara
para +60,5%. A folha média ficou em ~30,5% da receita (não 68%), indicando que o teto não é
vinculante na maioria dos negócios — a margem alta vem, sobretudo, de **preço de serviço elevado em
relação ao custo efetivo**. É um sinal de calibração de preço ainda pendente, não de um setor
genuinamente rentável.

## 7. Recomendações (para o @gerente-simulacao)

**Para desbloquear o veto (DJI)** — o mecanismo é a drenagem da `pop.savings` (orçamento de comida)
pelo capital dos negócios de serviço, agravada por `opens` 1→5 e pelo `affordable` que sobe 10× com
o `capital_per_unit` ÷10. Três direções, em ordem de custo:

1. **Constantes (A/B rápido, recomendado primeiro):** reduzir `kDemoServiceOpensPerDay` 5→2/3 e/ou
   subir `kDemoServiceCapitalReserve` 4→8. Ataque o sintoma (volume de drenagem) sem mudar modelo.
2. **Financiamento estrutural (correto, precisa do aval do dono):** financiar a abertura de negócio
   pelo `city.service_cash` (o caixa do próprio setor, que já retém 8 dias de folha+insumos) em vez
   da `pop.savings`. Desacopla o capital de serviço do orçamento de comida — ataca a causa raiz.
3. **Não recomendado como 1ª opção:** mexer de novo no `capital_per_unit`, que já foi ÷10 e tem
   efeito ambíguo (menos capital por negócio, mas mais negócios financiáveis).

**Para a evidência (pendência de processo, não bloqueia):**
- Gerar o sweep contra o baseline f9 (ou registrar a comparação f9 lado a lado no relatório).
- Regerar o `.real_debug` ao fim do build.
- Considerar endurecer a mensagem da asserção de educação privada em `services_test.gd`
  (`0.5040 > 0.5040` é ilegível com `%.4f`).

## 8. Síntese

- **Correto e aprovado:** remoção do teto de 40, folha sobre efetivos, devolução limitada ao capital
  aportado, entrada por demanda, `capital_per_unit` ÷10. Empregados variam, demanda 86,5% atendida,
  falências caem de 33.914→278 (90 d) e 217.757→1.522 (365 d), dinheiro conservado, bateria 8/8.
- **Veta (único ponto):** regressão de comida de DJI 0,472→0,355 (−11,65 p.p. vs f9, tolerância
  2,5 p.p.), importador sem agricultura cuja poupança-orçamento de comida é drenada pelo capital dos
  serviços. Veto se resolve com **número** (DJI voltar a ≤ −2,5 p.p.) ou **decisão do dono**.
- **Observações de calibração (não bloqueiam, devem ser decididas):** o teto 0,68 é muleta de
  apresentação que mascara preço×salário sub-calibrado; a margem +60% é irrealista e herda disso.
