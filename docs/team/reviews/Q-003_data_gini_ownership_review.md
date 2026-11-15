# Q-003 — Revisão dos dados reais (Gini + estoque de IDE/propriedade)

- **Revisor:** gerente-qualidade
- **Data:** 2026-11-15
- **Escopo:** commit `51b73e0` do @gerente-dados — `data/definitions/economy/inequality.json`,
  `data/definitions/economy/ownership_baseline.json`, `tools/build_*.py`,
  `tools/validate_*.py`, `docs/economy/initial_world_data.md`.
- **Veredito:** **APROVADO** (com 2 observações de melhoria, não bloqueiam).

## Critério de aceite (QUADRO D-001 / D-002)

| Item | Resultado | Evidência |
| --- | --- | --- |
| Validador inequality retorna 0 | ✅ | `validate_inequality.py` exit 0 (179 nações; 154 observados; 25 estimados; 0 sem dado) |
| Validador ownership retorna 0 | ✅ | `validate_ownership_baseline.py` exit 0 (179 nações; IDE em 178; setores estatais em 12; sem dado 1) |
| Amostra vs World Bank SI.POV.GINI | ✅ 11/11 | Ginis conferidos na API do WB batem exatamente |
| Amostra vs IMF DIP | ✅ 14/14 | Estoques de IDE conferidos no cache bruto do FMI DIP batem exatamente |
| Estimativas marcadas | ✅ | 25 nações com `estimated:true` + `method` + `confidence=estimated` |

## 1. Validadores

```text
$ python tools/validate_inequality.py
nações no nations.csv: 179 | entradas no JSON: 179
observados: 154 | estimados: 25 | sem dado: 0
OK: 179 nações, 154 observados, 25 estimados            EXIT=0

$ python tools/validate_ownership_baseline.py
OK: 179 nações · IDE em 178 · SOE numérico em 0 · setores estatais documentados em 12 · sem dado em 1 · 0 erros   EXIT=0
```

## 2. Amostra de 15 nações vs fontes

Amostra escolhida para cobrir os 3 tipos: `USA DEU BRA CHN IND NGA ZAF NOR RUS FRA JPN`
(observados WB), `SAU NZL SGP` (estimativas), `SOL` (sem dado).

**Gini (World Bank API `SI.POV.GINI`)** — conferência direta via `api.worldbank.org`; o valor
do JSON para o `gini_year` declarado bate com o valor publicado no ano correspondente:

| Nação | JSON gini | ano | WB API | Match |
| --- | --- | --- | --- | --- |
| USA | 41.8 | 2024 | 41.8 | ✅ |
| DEU | 33.7 | 2022 | 33.7 | ✅ |
| BRA | 50.3 | 2024 | 50.3 | ✅ |
| CHN | 36.0 | 2022 | 36.0 | ✅ |
| IND | 23.0 | 2023 | 23.0 | ✅ |
| NGA | 33.9 | 2022 | 33.9 | ✅ |
| ZAF | 54.1 | 2022 | 54.1 | ✅ |
| NOR | 26.5 | 2023 | 26.5 | ✅ |
| RUS | 33.0 | 2023 | 33.0 | ✅ |
| FRA | 31.8 | 2023 | 31.8 | ✅ |
| JPN | 32.3 | 2020 | 32.3 | ✅ |

**Estoques de IDE (IMF DIP / CDIS)** — conferência contra o cache bruto HTTP
(`tools/data/ownership_cache/dip/*.csv`, gitignored), série `G001` (total) com DV_TYPE preferido
(`O` reportado; `SCC` espelho):

| Nação | JSON inward (USD) | JSON outward (USD) | IMF cache | Match |
| --- | --- | --- | --- | --- |
| USA | 5.707.721.000.000 | 6.826.786.000.000 | G001(O) | ✅ |
| DEU | 1.240.286.609.400 | 2.208.642.182.700 | G001(O) | ✅ |
| BRA | 621.199.361.576,30 | 313.083.889.538,13 | G001(O) | ✅ |
| CHN | 3.729.410.895.614,36 | 3.139.931.063.459,23 | G001(O) | ✅ |
| IND | 803.045.296.134,69 | 136.270.263.199,69 | G001(O) | ✅ |
| NGA | 42.053.838.608,94 | 9.865.338.941,86 | G001(O) | ✅ |
| ZAF | 115.478.491.012,50 | 153.577.630.227,00 | G001(O) | ✅ |
| NOR | 162.141.409.691,63 | 231.470.837.004,41 | G001(O) | ✅ |
| RUS | 225.881.261.692,86 | 188.321.990.959,52 | G001(SCC) | ✅ |
| FRA | 967.449.961.038,52 | 1.551.606.864.522,46 | G001(O) | ✅ |
| JPN | 207.434.521.479,62 | 2.113.077.959.431,31 | G001(O) | ✅ |
| SAU | 70.383.450.934,17 | 80.566.978.917,36 | G001(SCC) | ✅ |
| NZL | 89.318.668.922,73 | 16.558.375.634,52 | G001(O) | ✅ |
| SGP | 2.474.376.828.640,74 | 1.087.138.478.811,30 | G001(O) | ✅ |
| SOL | — (no_data) | — (no_data) | ausente no FMI/WB | ✅ |

Os 14 valores batem exatamente (o JSON reproduz o `OBS_VALUE` bruto do FMI DIP). `SOL`
(Somalilândia) está `no_data` com motivo "economia não reportada pelo FMI DIP nem pelo World Bank"
— correto, nunca inventado.

## 3. Estimativas marcadas

25 nações sem WB usam `regional_estimate` com `estimated:true` + `method` + `confidence=estimated`
(ex.: `SAU` middle_east=39, `NZL` oceania=36, `SGP` asia=38, `SOL` africa=43). O validador exige
exatamente essa coerência (`estimated:true` ⇔ `source==regional_estimate` + method + confidence),
e passa.

## Observações (não bloqueiam; encaminhar ao @gerente-dados)

1. **Fallback de Gini só usou estimativa regional.** A decisão D-001 previa "fonte alternativa
   documentada quando existir (OECD/Eurostat/SWIID/nacional); senão estimativa regional". O dado
   final aplicou `regional_estimate` a **todas** as 25 nações sem WB — nenhuma fonte OECD/SWIID/
   nacional foi documentada. `NZL`, `SGP` e `TWN` têm estatística nacional publicada; poderiam
   subir de "estimativa regional" para "observado com fonte nacional". Não é veto: está marcado
   honestamente como estimativa (nunca fabricado).
2. **SOE numérico = 0.** A decisão D-002 citava "OECD ~38 + casos nacionais documentados" para
   peso de estatal. O `note` do arquivo explica que as bases públicas (OECD, IMF Fiscal Monitor
   out/2020 cap.3) não trazem, por país e em % do PIB, um valor verificável nesta rodada — então
   `soe_share_gdp/soe_share_employment` ficaram null (`soe_no_data` + `soe_reason`). É a escolha
   conservadora correta (nenhum percentual inventado); o conceito qualitativo `state_sectors` foi
   documentado para 12 nações (BRA CHN DEU FRA IND JPN MEX NOR RUS SAU USA ZAF) com refs citáveis.
   Vale o @gerente-dados confirmar se tenta a fonte OECD numa rodada futura ou formaliza o adiamento.

## Conclusão

Dados fiéis às fontes (WB e FMI DIP), validados por script e conferidos por amostra independente.
Nenhum número inventado; ausências explicitadas com motivo. **APROVADO.**
