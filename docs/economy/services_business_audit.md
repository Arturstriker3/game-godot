# Auditoria dos negócios de serviço (F8) — comportamento ao longo do tempo

> Gerente-qualidade · 2026-11-15 · ferramenta `tools/services_business_audit_test.gd`
>
> **Escopo:** medir, SEM alterar o núcleo, como os negócios de serviço se comportam
> ao longo do tempo (90 dias no mundo real com IA), a partir da observação de que
> "no dia 1 todos os negócios aparecem com 40 empregados e lucro 0,0".
>
> **Veredito:** confirmado **bug de núcleo** — não é artefato de captura. A
> distribuição de empregados é travada em 40 postos por construção, e o lucro é
> estruturalmente negativo, com falência em massa e demanda quase não atendida.
> A correção é do **@gerente-simulacao** (núcleo C++/`services.json`).

---

## 1. O que foi medido

| Métrica | Como | Fonte |
| --- | --- | --- |
| Negócios abertos/fechados por mês | dif. do conjunto de ids entre dias | `demo_get_services_detail` |
| Empregados por negócio (min/mediana/max) | `jobs` (postos) e `workers` (efetivos) | idem |
| Lucro diário (min/mediana/max, +/0/−) | `profit` por negócio/dia | idem |
| Receita vs. custos | `revenue` vs `wages`+`input_cost`+`service_cost` | idem |
| Falências de serviço | `demo_get_service_bankruptcies` (cumulativo) | núcleo |
| Demanda atendida | `(demand − unmet) / demand` por setor | `demo_get_services_detail` |

Rodado com o **pipeline real do jogo**: `cities/ports/buildings.json` (834 cidades /
179 nações), `goods/recipes/needs/services/currencies/sector_employment.json`, geografia
real + `EconomicAI`. Janela de 90 dias, nações-âncora USA/DEU/BRA/IND/NGA/CHN.

---

## 2. Resultados (90 dias, mundo real com IA)

### 2.1 Principais números (fonte: `services_business_audit.json`)

| Métrica | Valor |
| --- | --- |
| Empregados (postos) por negócio | min 20,8 · mediana **40,0** · max 45,0 — **88,7% têm exatamente 40** (n=13.460) |
| Trabalhadores efetivos | min **1,1** · mediana 40,0 · max 45,0 |
| Lucro diário | min −32.844 · mediana −531,9 · max +4,56 — **443 positivos / 575.359 negativos** (98,0% negativos) |
| Receita vs. custos | receita 11,0 M vs. custo 719,9 M — margem **−6.441,7%** (nenhum setor com margem positiva) |
| Demanda | servida média **8,3%** · não atendida **99,1%** |
| Falências de serviço (90 d) | **33.914 falências** para 45.448 negócios ativos (0,75×) |
| Falências por mês | mês 1: 0 · mês 2: 11.433 · mês 3: 22.481 |
| Dinheiro | inicial 2.200.539.587 → final 2.200.539.587 (drift 0,0001 — conservado) |

Piores margens por nação: CHN −15.921%, USA −4.484%, BRA −4.183%, IND −3.861%, DEU −3.202%,
NGA −1.970%. Piores por setor: construction −9.963%, wholesale −8.066%,
cleaning/security/waste −7.374%, personal_services −7.294%, retail −7.085%.

O padrão de abertos/fechados por mês confirma o **treadmill**: o grupo de setores aberto no
mês 1 (finance, retail, food_service, health, wholesale, freight…) fecha em bloco no mês 2,
enquanto um segundo grupo (construction, it, media, professional, real_estate, telecom…)
abre — e fecha no mês 3, reabrindo o primeiro. É o ciclo de 60 dias de prejuízo
(`kDemoServiceLossDays = 60`) batendo como um relógio.

### 2.2 Retrato do dia 0/1 (o "bug" original da captura)

| Métrica | Dia 0 (semente, pré-tick) |
| --- | --- |
| Empregados (postos) | imensa maioria em **40** (min 20,8 · mediana 40 · max 45,0 — as exceções são `min_capacity×jobs_per_unit`, §3.1) |
| Lucro | todos **0,0** (2.021 negócios) |

O "40 empregados e lucro 0,0" do dia 1 é, portanto, a **fotografia da semente**: 40 vem do
teto `40.0/jobs_per_unit` (bug real, §3.1) e 0,0 é só o estado pré-tick (lucro só existe
após o 1º tick). Após os ticks o lucro deixa de ser 0 — mas fica **negativo** (§3.3).

### 2.3 Baseline F8 já registrado (365 dias)

`docs/economy/perf/f8_world_sweep.json` (`service_businesses`): **38.157 negócios** e
**216.501 falências de serviço** em 365 dias. Ou seja, cada negócio faliu, em média,
~5,7× no ano — o mesmo ciclo medido aqui em 90 dias, estendido.

---

## 3. Diagnóstico (causa raiz)

### 3.1 Empregados travados em 40 — teto rígido `40.0/jobs_per_unit`

O valor 40 não vem de população, demanda nem capital: é uma **constante mágica** no núcleo,
nos dois pontos em que um negócio nasce:

- **Semente** (`economy_sim.cpp:9568`):
  ```cpp
  const double capacity = std::max(definition.min_capacity,
      std::min(40.0 / definition.jobs_per_unit, definition.min_capacity * 6.0));
  ```
- **Entrada por demanda** (`economy_sim.cpp:10019`):
  ```cpp
  const double capacity = std::max(definition.min_capacity,
      std::min(market.unmet, 40.0 / definition.jobs_per_unit));
  ```

Como `jobs = capacity × jobs_per_unit` (linha 9472), **todo** negócio nasce com 40 postos
(ou `min_capacity × jobs_per_unit` quando este é > 40: passenger 42, construction 45).
Por isso a distribuição não varia: é o desenho, não um estado inicial.

### 3.2 Lucro 0,0 no dia 1 — artefato de captura (não é o bug principal)

No dia 0 (semente, antes do 1º tick) `business.profit` vale 0 porque o lucro só é calculado
em `demo_services_day` (linha 9902), que roda no primeiro tick. A captura do dia 1
simplesmente fotografou o estado pré-tick. **Isso não é bug** — mas mascara o bug real do
item 3.3.

### 3.3 Lucro estruturalmente negativo — folha paga sobre POSTOS, não sobre o trabalho real

`economy_sim.cpp:9900`:
```cpp
business.wages = total_business_jobs > 1e-12
    ? city.service_wage_bill * business.jobs / total_business_jobs : 0.0;
```

- A folha agregada de serviços (`city.service_wage_bill`) é **toda** distribuída entre os
  negócios proporcionalmente a `jobs` (postos), **não** a `workers` (efetivos, que já
  descontam a utilização na linha 9857).
- Resultado: um negócio com utilização ~0% paga a folha **cheia** dos seus 40 postos,
  enquanto a receita só é realizada na fração atendida da demanda (`market.revenue`,
  linha 9812) — que é ~1% porque a capacidade semeada (40 postos × 16 negócios/cidade) é
  ordens de grandeza maior que a demanda em unidades.
- Consequência: prejuízo persistente → `loss_days` acumula → falência aos 60 dias
  (`kDemoServiceLossDays = 60`, `economy_sim.cpp:114`), e o ciclo recomeça (a demanda
  continua não atendida, então o setor reabre).

### 3.4 Demanda ~não atendida

`served = min(supply, demand)` (linha 9804). Apesar do excesso de capacidade nominal, a
receita realizada é minúscula e o `unmet` fica alto por setor/dia — o gargalo é que a
**oferta não está dimensionada para a demanda em dinheiro** (`money / price`), e os
negócios fecham antes de amortizar o capital. A métrica "servida média" mede isso.

---

## 4. Evidências (arquivos/linhas do núcleo, NÃO alterados aqui)

| Achado | Local |
| --- | --- |
| Teto de 40 postos na semente | `gdextension/src/economy_sim.cpp:9568–9569` |
| Teto de 40 postos na entrada | `gdextension/src/economy_sim.cpp:10019–10020` |
| `jobs = capacity × jobs_per_unit` | `gdextension/src/economy_sim.cpp:9472` |
| Folha sobre `jobs` (não `workers`) | `gdextension/src/economy_sim.cpp:9900–9901` |
| `workers` já desconta utilização | `gdextension/src/economy_sim.cpp:9857` |
| Receita realizada só na fração atendida | `gdextension/src/economy_sim.cpp:9812` |
| Falência aos 60 dias de prejuízo | `gdextension/src/economy_sim.cpp:114` |
| Baseline F8 (216.501 falências/365 d) | `docs/economy/perf/f8_world_sweep.json` |
| Catálogo com `min_capacity`/`jobs_per_unit` | `data/definitions/economy/services.json` |

---

## 5. Proposta de correção (para @gerente-simulacao)

1. **Remover o teto `40.0`** e dimensionar a capacidade por demanda/cidade — ex.: capacidade
   inicial proporcional a `simulation_population` (ou à demanda esperada em unidades), com
   mínimo `min_capacity` e sem o teto arbitrário de 40 postos.
2. **Pagar a folha sobre `workers` (efetivos), não `jobs` (postos)**, para negócio ocioso
   não sangrar — ou, se a folha for contratual (postos fixos), reduzir a semente à demanda
   real para a utilização não colapsar.
3. **Reconciliar a folha de serviços com a receita de serviços**: hoje `service_wage_bill`
   (setor inteiro, F7) é rateado só pelos ~40 postos de cada negócio simulado, enquanto a
   receita desses negócios é a fração atendida da demanda — as duas escalas não casam.
4. Depois da correção, revalidar com esta auditoria: espera-se distribuição de empregados
   **variada** (não 94% em 40) e lucro com **positivos e negativos** (não só negativo), e
   queda drástica das falências de serviço.

Não mexi no núcleo; o dono do arquivo corrige. A auditoria fica como guarda de regressão.

---

## 6. Como reproduzir

```powershell
$godot = "C:\Users\artur\OneDrive\Área de Trabalho\Godot_v4.7.2-stable_win64.exe"
& $godot --headless --path . --script res://tools/services_business_audit_test.gd -- --days=90 --out=res://docs/economy/services_business_audit.json
```
