# Quadro compartilhado da equipe — OpenGS Game

Quadro de comunicação assíncrona entre os gerentes (ver `docs/team/PROTOCOLO.md`).
Cada gerente escreve na sua seção. Mensagens para outro gerente vão em `@gerente-x`.

**Status possíveis:** `planejada` → `em execução` → `verificando` → `revisão` → `aprovada`
(ou `bloqueada` / `vetada`).

---

## @gerente-simulacao

| ID | Objetivo | Critério de aceite | Subtarefas | Status | Bloqueios |
| --- | --- | --- | --- | --- | --- |
| S-001 | Refatoração do núcleo C++ conforme `docs/economy/core_refactor_plan.md` (ordem Apêndice C) | Por passo: build debug+release verde; bateria verde; equivalência byte a byte (status/save 60 d com IA) = 0 diffs; commit por passo verde; DLL debug real no fim | 1. S0 harness+goldens (gerente) · 2. S4 remover API morta+benchmark (executor-flash) · 3. otimização STATUS (tick p95≤50, status≤30) · 4. S5+ módulos sim/ + edge/ (um por vez) | em execução | — |

### Decisões

- **2026-11-15 · S-001/S0:** equivalência byte a byte = hash SHA-256 de `JSON.stringify(...,"",true,true)` sobre `demo_to_dict()` e `get_demo_status()`; `demo_get_profile()` é registrado no golden como meta informativa (é dado de tempo, não determinístico) — o contrato estrito é status/save. Ordem Apêndice C: S0 → S4 → status → S5…S14 → S16 → S15/S17 → S18.

---

## @gerente-dados

| ID | Objetivo | Critério de aceite | Subtarefas | Status | Bloqueios |
| --- | --- | --- | --- | --- | --- |
| D-001 | Gini real por nação (WB SI.POV.GINI + quintis/decis + fallback) em `inequality.json` | 179/179 nações cobertas; todo valor tem `source`, `refs` (title/url/year) e `confidence`; `tools/validate_inequality.py` retorna 0 | 1. executor-flash: `build_inequality.py` + JSON; 2. verificador-flash: conferir amostra vs WB; 3. gerente: consolidar | em execução | — |
| D-002 | Propriedade inicial (estoque IDE in/out FMI DIP + origens + peso estatal) em `ownership_baseline.json` | 179/179 nações; IDE em USD com ano/fonte; lacunas com `no_data` + motivo; refs obrigatórias; `tools/validate_ownership_baseline.py` retorna 0 | 1. executor-flash: `build_ownership_baseline.py` + JSON; 2. verificador-flash: conferir amostra vs FMI/OCDE; 3. gerente: consolidar | em execução | API FMI DIP (sdmx-csv) |

### Decisões

- **Gini em 0..100 (escala nativa do WB SI.POV.GINI).** O `income_distribution.json` (0..1) fica intocado; o `inequality.json` é a fonte por nação e a integração no núcleo é pós-refactor (nota em §6).
- **Fallback de Gini (27 nações sem WB):** fonte alternativa documentada quando existir (OECD/Eurostat/SWIID/nacional); senão estimativa regional das 7 regiões já em `income_distribution.json`, marcada `estimated: true` + `method`. Nunca inventar.
- **Peso de estatal (SOE):** cobertura real é escassa (OECD ~38 + casos nacionais documentados). Fora disso, `no_data` com motivo "sem fonte publicada de peso de estatal". Não se fabrica percentual.

---

## @gerente-qualidade

| ID | Objetivo | Critério de aceite | Subtarefas | Status | Bloqueios |
| --- | --- | --- | --- | --- | --- |
| Q-001 | Auditoria dos negócios de serviço (F8) ao longo do tempo | 90 d mundo real com IA; empregados/negócio com distribuição que varia (não tudo 40); lucro diário com positivos e negativos; falências/aberturas/fechamentos por mês; demanda atendida mensurável | 1. criar `tools/services_business_audit_test.gd` (gerente); 2. rodar 90 d (`executor-flash`); 3. conferir números vs JSON (`verificador-flash`); 4. diagnóstico em `docs/economy/services_business_audit.md` (gerente) | verificando | — |

### Pareceres e vetos

- **Q-001 (parcial, smoke 1 d):** negócios travados em 40 postos (94,1% == 40), lucro só negativo/zero, receita ≪ folha, demanda ~0,8% atendida. Bug de núcleo → @gerente-simulacao (não é meu arquivo).

---

## Mensagens entre gerentes

<!--
Formato:

### @gerente-destino — de @gerente-origem · AAAA-MM-DD
Tarefa: S-000
Pergunta/decisão: ...
Resposta: ... (quem respondeu, quando)
-->
