# Parecer Q-005 — Revisão dos serviços v2 (financiamento por service_cash + calibração)

> Gerente-qualidade · 2026-11-15 · commit revisado: `b95c7f3` (S-002 v2, resposta ao veto Q-004)
>
> **Veredito: APROVADO COM RESSALVA** — o conserto é correto e o audit 90 d + bateria
> reproduzem; mas a **evidência do sweep 365 d está obsoleta** (não reproduz com a DLL do
> commit) e **DJI segue regredindo** em comida por lacuna de modelo, não por bug.

---

## 1. O que foi revisado

| Item | Conclusão |
| --- | --- |
| Financiamento nunca toca a poupança de comida | **Correto** (§2) |
| Muleta `×0,68` removida (salário integral) | **Correto** (§2) |
| Dinheiro conservado | **Conservado** (drift ≤ 0,01) (§2, §3) |
| Audit 90 d + bateria 8/8 | **Reproduz** (§3) |
| Sweep 365 d (`services_fix_world_sweep_v2.json`) | **NÃO reproduz — evidência obsoleta** (§4) |
| Diagnóstico SOL/DJI | Quantificado (§5) |
| Modelo "exportação de serviços" | Avaliado, não implementado (§6) |

---

## 2. Verificação estrutural do diff (leitura do código)

**2.1 Financiamento da abertura (`demo_service_open`)** — confirmado, a poupança que paga
comida **não é tocada**. Ordem de financiamento:

```cpp
const double food_reserve = demo_city_food_reserve(city);
const double savings_surplus = std::max(0.0, city.pop.savings - food_reserve);
const double from_sector = std::min(capital, std::max(0.0, city.service_cash));
const double from_savings = std::min(capital - from_sector, savings_surplus);
if (from_sector + from_savings < capital) { return false; }
```

- 1º caixa do próprio setor (`service_cash`, lucros retidos) — vira equity sem movimento de caixa;
- 2º poupança das famílias **acima da reserva alimentar** — move `savings → service_cash` e é
  devolvida no fechamento;
- Se a soma não cobre o capital sem tocar a reserva, **não abre** (`return false`).

`demo_city_food_reserve()` = `need × kDemoFoodReserveDays × per_ge`, onde `need = population ×
kDemoNeedPerCapita` e `per_ge` é o custo da cesta por GE/dia de `demo_food_basket_cost`. É a
**mesma regra prudencial da F3** (`pop.need × kDemoFoodReserveDays × food_unit_cost`,
`economy_sim.cpp:4560`). Como `kDemoFoodReserveDays = 3650` excede a poupança inicial
(`kDemoSavingsPerCapita`), na prática `savings_surplus = 0` e **só o `service_cash` financia** —
exatamente o que o commit declara.

**2.2 Muleta `×0,68` removida.** O lambda `business_wage` (teto `price÷jobs_per_unit×0,68`) foi
apagado; agora `business.wages = business.workers * wage` (salário de mercado integral). A margem
passa a vir da âncora de preço escalada pelo salário (`base_price = price × clamp(wage/0,15; 0,10;
4,0) × 0,85`) + banda apertada `[0,70; 1,40]` + reserva `4→1`. **Não há muleta escondida**: o
`grep` por `0.68` no diff confirma a remoção, e não há novo teto contábil.

**2.3 Conservação de dinheiro.** `from_sector` não move caixa (fica em `service_cash`);
`from_savings` move `savings → service_cash` e, no fechamento, devolve à POP no máximo a parcela
que ela aportou (`business.capital = from_savings`). Nenhuma fonte ou sumidouro novo. ✅

---

## 3. Execução independente (executor-flash + verificador-flash)

**Ambiente:** build debug (`economy_gdextension.windows.template_debug.x86_64.dll`, 13.252.608 B,
19:55 — hash igual ao backup `.real_debug`), Godot 4.7.2 headless, modo de tick real com IA,
locale pt-BR. Nenhum build foi necessário (DLL já em dia com o commit; fonte C++ sem alteração
não commitada).

**Audit 90 d (`services_business_audit_test.gd`)** — exit 0:

| Métrica | Declarado no commit | Medido (Q-005) |
| --- | --- | --- |
| Empregados == 40 postos | 0% | **0,0%** |
| Mediana de postos/negócio | 846 | **846,3** (min 18 · max 623.658) |
| Margem agregada | 26,8% | **26,76%** (receita 555,1 M / custo 406,6 M) |
| Falências de serviço (90 d) | 2206 | **2206** (mês [0, 72, 2134]) |
| Drift de dinheiro | 0,0001 | **0,000057** (tolerância 0,01) |

**Bateria 8/8 verde**: `economy_integration` 0 · `economy_ai` 0 · `population_demo` 0 ·
`production_chain` 0 · `investment` 0 · `labor_market` 0 · `pop_needs` 0 · `services` (via cena
`services_test.tscn`) 0.

**Verificador-flash: APROVADO** — todos os números do audit e do sweep conferem, byte a byte,
contra os JSON em disco (`services_business_audit_q005.json`, `services_fix_world_sweep_v2.json`,
`f9_invest_world_sweep.json`).

**Achado novo (não bloqueia, registrar):** o audit emitiu `[bug] demand_largely_unserved`
(servida média **62,6%**, fração de setor-dias com demanda não atendida **72,1%**). A v1 (Q-004)
tinha **46,0%** de não atendida — a calibração (banda mais apertada + margem mais baixa) reduziu a
oferta de serviços. O commit reconhece a causa ("intermediários 32–95% pela receita de insumo sem
capacidade no fornecedor"). A demanda segue "majoritariamente atendida" (62,6%), mas o teto de
oferta é um ponto de calibração a observar.

---

## 4. ACHADO CRÍTICO — o sweep 365 d commitado NÃO reproduz

O critério de aceite do S-002 é "sweep 365 d sem nação pior em comida vs `f9_invest_world_sweep`".
O relatório commitado (`services_fix_world_sweep_v2.json`) declara:

| Nação | baseline (f9) | pior comida (commit) | delta declarado |
| --- | --- | --- | --- |
| SOL | 0,9734 | **0,6387** | **−33,5 p.p.** |
| DJI | 0,4717 | **0,3552** | **−11,6 p.p.** |

Refazendo o **seed padrão** (variante `real`, `--fx=real`, 365 d, mesmo `economy_world_test.gd`
que o sweep usa), a DLL do commit produz:

| Nação | comida (reproduzido, 365 d) | delta real vs f9 |
| --- | --- | --- |
| SOL | **0,986** (SOL-HAR 1,000 · SOL-BER 0,920) | **+1,3 p.p. (melhora, não regressão)** |
| DJI | **0,395** (DJI-DJI 0,396 · DJI-ALI 0,382) | **−7,8 p.p. (regressão, ~3× a tolerância de 2,5)** |

Também divergem os agregados de serviço: o commit declara **61.662 negócios / 1.522 falências**;
a reprodução dá **≈48.077 negócios / ≈13.495 falências**.

**Causa da divergência (evidência):** o relatório por variante em `user://world_sweep/real_365.json`
tem timestamp **18:24**, enquanto a fonte C++ foi finalizada às **19:38–19:54** e a DLL foi
recompilada às **19:55** (commit às **20:15**). Ou seja, o sweep foi rodado com uma **DLL
anterior à final** e **não foi regenerado** após a calibração/financiamento finais. A prova
interna está no próprio commit: o cabeçalho diz "DJI recupera **0,355 → 0,395**", mas o corpo e o
JSON dizem "DJI **0,355**" — o número real da DLL final é **0,395**, que bate com a reprodução
aqui. A alegação central "SOL −33,5 p.p., regressão NOVA" **não é verdadeira** para o código
commitado: SOL **melhora** +1,3 p.p.

**Implicação:** o parecer do commit ("mesma lacuna de renda/tesouro de importador") continua
válido **para DJI** (segue regredindo, −7,8 p.p.), mas **não para SOL** (que se recupera). A
evidência commitada precisa ser **regenerada** (pendência do @gerente-simulacao).

---

## 5. Diagnóstico de SOL e DJI (de onde vinha/vem a renda)

Renda por cidade no dia 365 (DLL do commit, pipeline com IA; valores em XGS/dia):

| Cidade | comida | poupança | renda externa | salário | renda de capital | transferências | receita de serviço | in_kind |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| SOL-HAR | **1,000** | 0 | 2.701 | 4.587 | **41.748** | 6.950 | 56.831 | 0 |
| SOL-BER | 0,920 | 0 | 563 | 1.058 | 376 | 215 | 1.664 | 11,3 |
| DJI-DJI | **0,396** | 0 | 2.471 | 1.441 | **0** | 216 | 1.546 | 0 |
| DJI-ALI | 0,382 | 0 | 197 | 88 | 0 | 13 | 89 | 0 |

- **SOL** é financiado pela **renda de capital** (F9-investimento: lucro dos grupos devolvido ao
  estrato dono — 41.748 em SOL-HAR) + salário + renda externa. Por isso a capital Hargeisa fecha
  em **1,000** de comida e a nação fecha em 0,986 (a calibração de serviços **não** o derruba —
  a renda de capital domina).
- **DJI** tem **renda de capital = 0** (a camada de investimento não gera capital para DJI no
  mundo v2) e **`in_kind = 0`** (100% da comida importada). Sobra renda externa 2.471 (porto/
  trânsito) + salário 1.441 + transferências 216 — insuficiente para importar a necessidade, daí
  0,396 (Djibouti) / 0,382 (Ali Sabih).
- Ambos têm `external_income_index` **baixo** (DJI 0,33 · SOL 0,19), abaixo do limiar 1,2 que
  semeia serviço exportável (`demo_seed_services`). Ou seja: a renda externa real dessas economias
  (porto/remessas) está **sub-representada** no índice, e é exatamente essa a lacuna que DJI sente.
- **Preço da cesta** (alimento mais caro dos 5 slots) está em **55,0 XGS** nas 4 cidades (âncora
  fixa, inalterada pela calibração de serviços) — o que muda não é o preço, é a **renda disponível**.

**Conclusão do diagnóstico:** a regressão de comida é de **renda** (importador sem agricultura com
renda externa sub-modelada), não de preço e não de calibração de serviço. É a **mesma lacuna de
renda/tesouro de importador** da F3 §19.5, agora **restrita a DJI** (SOL se recuperou).

---

## 6. Opção de modelo "renda de exportação de serviços" (avaliada, não implementada)

**Proposta:** dar a importadores sem agricultura uma renda de exportação de serviços com base real —
DJI = porto/trânsito para a Etiópia + bases militares estrangeiras; SOL = remessas/ajuda + pesca +
madeira (olíbano/mirra).

**Dados reais que existiriam (fontes públicas, verificáveis):**

- **DJI (Djibuti):** economia ~75% serviços; o Porto de Djibuti movimenta ~95% do comércio exterior
  da Etiópia (via ferroviária Addis–Djibuti); receitas de trânsito/porto; arrendamentos de bases
  militares (França, EUA, China, Japão) — ex.: China ~US$ 100 M/ano, EUA ~US$ 63 M/ano (averbados
  em orçamento de defesa). PIB PPP ~US$ 10 bi. → tudo mapeável para `external_income` do porto.
- **SOL (Somalilândia):** remessas da diáspora estimadas em ~US$ 1–2 bi/ano (~30–50% do PIB),
  exportação de gado (camelos/ovinos ao Golfo), pesca (sub-explorada), olíbano/mirra; Porto de
  Berbera (investimento DP World). → mapeável para `external_income` de remessas/exportação.

**O que o modelo já tem:** `external_income_index` + serviço exportável semeado quando
`external_income_index > 1,2`. O problema medido é que DJI/SOL estão em **0,33/0,19**, abaixo do
limiar — a renda externa real não está calibrada no índice.

**Tamanho do efeito esperado (ordem de grandeza, a partir do diagnóstico §5):** para DJI subir de
0,395 → 0,472 (fechar a regressão), precisa importar ~15 t-eq/dia a mais na capital (74 → 89 de 188
de necessidade). A ~55 XGS/t-eq, são **~800 XGS/dia** de renda adicional — uma fração pequena da
renda externa real de Djibuti (bilhões/ano ÷ 365 ÷ escala). Ou seja, uma **calibração modesta** do
`external_income_index` de DJI (e do porto de Berbera, para SOL) provavelmente **fecha a lacuna
sem tocar na calibração de serviços**. É decisão de **modelo/dono** (o gerente-dados levantaria as
fontes; o gerente-simulacao integraria), não parte desta revisão.

---

## 7. Veredito

**APROVADO COM RESSALVA.** O conserto de serviços está **correto** (financiamento pelo
`service_cash` sem tocar a poupança de comida; muleta removida; dinheiro conservado; audit 90 d e
bateria 8/8 reproduzem). **Não veta** — reverter desfaria um conserto correto.

Ressalvas (com dono e ação):

1. **[Evidência — @gerente-simulacao, obrigatório antes de fechar]** regenerar o sweep 365 d
   (`services_fix_world_sweep_v2.json`) com a DLL final. O arquivo commitado é obsoleto (18:24 vs
   fonte 19:54/DLL 19:55) e sua alegação "SOL −33,5 p.p." é **falsa** para o código commitado
   (SOL real = +1,3 p.p.; DJI real = −7,8 p.p.).
2. **[Modelo — dono do jogo]** DJI segue regredindo −7,8 p.p. em comida (tolerância 2,5 p.p.), por
   lacuna de renda de importador sem agricultura (renda de capital 0, `in_kind` 0, renda externa
   sub-calibrada em 0,33). SOL **não** regride mais. Direção sugerida: calibrar `external_income`
   de DJI/SOL com as fontes reais do §6 (custo baixo, provável fechamento da lacuna).
3. **[Calibração — observar]** o audit 90 d emitia `demand_largely_unserved` (72,1% de setor-dias
   com demanda não atendida; era 46% na v1). A demanda segue majoritária (62,6%), mas o teto de
   oferta (banda [0,70;1,40] + margem de entrada 8%) pode estar apertado demais para intermediários.

Observação de processo (não bloqueia): a DLL debug ativa já é a `.real_debug` (13.252.608 B, 19:55,
hash idêntico) — **nada a restaurar**; nenhum build foi feito nesta revisão.

---

*Evidência desta revisão: execução `services_business_audit_q005.json` (untracked, não commitado),
`tools/_tmp_income_diag.gd` (temporário, não commitado), re-run do seed padrão 365 d com a DLL do
commit. Verificador-flash: APROVADO. Nenhum arquivo de código foi alterado.*
