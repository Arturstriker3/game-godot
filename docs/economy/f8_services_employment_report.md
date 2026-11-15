# F8 — emprego em serviços por nação (medição de 60 d)

Data: 2026-11-15. Binário: `economy_gdextension.windows.template_debug` (debug).
Comando (por variação):

```powershell
Godot_v4.7.2-stable_win64.exe --headless --path . --script res://tools/economy_world_test.gd -- `
  --days=60 [--ai=1] --out=docs/economy/perf/f8_world_{60_noai,ai_60}.json
```

## Métrica

`economy_world_test.gd::_service_employment` agrega por nação:

- `service_workers` (novo campo de status, F8) — vagas de serviço **preenchidas**
  (`city.service_jobs * service_fraction`, publicado em `demo_fill_city_status`);
- `pop.employed` — empregados totais do mercado de trabalho (já incluem as vagas
  de serviço preenchidas);
- parcela = `service_workers / pop.employed`.

## Resultado (dia 60)

| Nação | sem IA | com IA | referência real (OIT ~2023) | diferença |
|---|---|---|---|---|
| USA | 99,5% | 99,4% | 79% | +20,4 p.p. |
| DEU | 99,4% | 99,4% | 72% | +27,4 p.p. |
| BRA | 99,5% | 99,4% | 70% | +29,4 p.p. |
| IND | 99,5% | 99,4% | 32% | +67,4 p.p. |
| NGA | 99,5% | 99,4% | 50% | +49,4 p.p. |

Mundo (179 nações): 99,4%. Dinheiro conservado (drift 0,0000), comida por nação
em 100% da necessidade, nenhuma violação de invariante.

## Por que não fecha com a realidade (hipótese)

O setor de serviços é o **empregador residual** do modelo (F7): todo trabalhador
que não está numa construção e não está desempregado é contado em serviços
(`economy_sim.cpp::demo_labor_day` → `service_jobs = residual_labor * (1 - u)`).

- O cenário real tem 834 construções (1 por cidade) e apenas 216 t/dia de
  capacidade industrial no dia 60 — emprego industrial/agrícola simulado ≈ 0,5%
  da força de trabalho.
- O modelo **não tem emprego agrícola nem informal** como categoria. Na Índia
  real ~45% da força está na agricultura e ~25% na indústria; na Nigéria a
  agricultura é ~35%. Esse contingente cai inteiro em "serviços" no modelo.

Logo o número é estrutural (não é ruído de calibração de preço/salário) e não
pode ser corrigido por ajuste de constantes de balanço: exige uma categoria de
emprego própria.

## Proposta (fora do escopo desta fase — decisão do dono/Game Systems Designer)

1. Criar emprego de **subsistência/agrícola** absorvendo o resíduo que não é
   serviço nem construção, com parcela derivada da estrutura real de emprego
   (OIT/Banco Mundial) por nível de renda — não inventar por país.
2. Esse contingente já recebe a "renda externa de subsistência" (F7); o desenho
   precisa decidir se a produção agrícola dele é simulada (lavouras) ou só
   contabilizada como emprego.
3. Impactos a recalibrar: folha de serviços (receita), impostos de serviço,
   PIB/GDP per capita, consumo por nível e a garantia de comida. Exige sweep de
   365 d próprio — não entra como ajuste silencioso.

Enquanto isso, o relatório acima fica como **evidência do gap**, não como
aprovação do número.

## Contrato de status para a tela de Serviços (UI, F8-E)

Por cidade (`get_demo_status().cities[i]`, ordem dos setores = `services.json`):

| Campo | Tipo | Uso |
|---|---|---|
| `service_jobs` | float | vagas-alvo do setor de serviços (empregador residual) |
| `service_workers` | float | vagas ocupadas (emprego efetivo do setor) |
| `service_wage` | float | salário médio pago no setor (folha / vagas) |
| `service_revenue`, `service_profit`, `service_cash` | float | receita, lucro e caixa do setor na cidade |
| `services` | PackedFloat64Array | escalares por setor, na ordem de `service_sectors` |
| `services_detail` (API `demo_get_services_detail(city_id)`) | Dictionary | `sectors[]` (demanda por nível, B2B, governo, exportação, cobertura, preço), `businesses[]` (`id`, `sector`, `name_seed`, `profit`, `loss_days`, `capital`) |
| `service_businesses`, `service_bankruptcies` (raiz) | int | contagem global e falências |

Os nomes dos negócios são gerados no cliente por
`core/culture/business_name_generator.gd` a partir de `sector` + `name_seed` +
cultura do dono (`CultureDb.city_mix(city_id)`), como `tools/services_test.gd`
faz em `_names_unique`.
