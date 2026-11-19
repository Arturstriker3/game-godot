# B6 — Patrimônio imobiliário da região (D-095)

Terra produtiva, moradia do interior e anel periurbano da cidade-polo como
**patrimônio agregado por região** — sem prédio a prédio no campo, sem
construtora e sem compra/venda individual (o dono aprovou em 2026-11-18; ver
`docs/plans/perf_pop_financas_plano.md` § B6).

O objetivo é dar patrimônio real às pops do interior (6,10 bi em 2026) e ligar
riqueza a terra/moradia, deixando o ativo pronto para virar garantia na Parte C
(C1/C4). A cadência é **mensal** (A6), pela fase `property` do núcleo
(`gdextension/src/sim/property.cpp`).

## Dado

`data/definitions/economy/land_property.json` (fonte, regra de rateio e
`fallback_rule` em `docs/data/land_property.md`; validador
`tools/validate_land_property.py`):

- hectares de terra agrícola por nação (FAOSTAT "Land Use" 00006610, 2023);
- Gini fundiário por nação (FAO LINEQ v1) quando observado.

O arquivo é **nacional**; o rateio para o estado é a regra declarada no próprio
arquivo, aplicada por `core/economy/land_property.gd`:

```
ha_estado = ha_nacao × w_estado ÷ Σ w
w_estado  = area_km2(estado) × aptidao_agricola(estado)
aptidao   = média da aptidão 0..1 das províncias do estado (province_nature)
Σ w = 0   → w = area_km2
clamp: ha_estado ≤ 100 × area_km2 (o estado inteiro); o excedente volta aos
       estados não clampados, proporcional a w
```

O Gini fundiário cai para a mediana da região do jogo (n ≥ 3) e depois para a
mediana global — as duas lidas do bloco `regional_reference`. Nada é
interpolado nem inventado: região sem hectares fica **inerte**
(`property_ready` falso).

`LandProperty.apply(sim)` é chamado depois de `initialize_world`/`set_urban` e
também depois de um load; o núcleo só semeia quando a região ainda não tem
patrimônio, então um save v23 mantém o estado gravado e um save v22 recebe o
dado.

## Modelo no núcleo

**Terra.** O valor por hectare é a renda fundiária **anual** da região
capitalizada pelo cap rate do projeto (`kPropertyCapRate` = 5%/ano, o mesmo de
§19.6), multiplicada pela infra B5 e pelo índice de commodities:

```
alvo/ha = (renda primária diária × 30) ÷ 0,05
          × (1 + 0,50 × cobertura_infra)          # pesos de kInfraProductivityWeight
          × índice_commodities                    # preço ÷ preço-base D0 dos
                                                  # primários do polo, 0,25..4,00
```

Amortecimento mensal declarado: 25% da distância ao alvo
(`kPropertyValueDamping`) com passo máximo de 10%/mês
(`kPropertyValueStepMax`). Boom de commodity sobe, quebra derruba; obra de
estrada/luz/água valoriza.

**Moradia do interior.** Autoconstruída (não há mercado de solo no campo,
§B3): vale 40% da unidade **popular** do polo (`kPropertyRuralHomeShare`), sem
aluguel de mercado — é patrimônio do dono.

**Anel periurbano.** Preço = preço popular do polo × desconto de distância
(35% por 100 km do raio efetivo derivado da área real dos estados,
`kPropertyRingDistanceDiscount`) × (1 − 30% × penalidade da estrada B5,
`kPropertyRingRoadPenalty`). O aluguel sobe com a fatia da população que vive
no anel (`kPropertyRingRentPressure` = 0,75). Quando o polo está cheio (limite
de vaga da B4), o excedente do êxodo **assenta no anel** em vez de ser barrado:
absorção de 0,5%/mês da população urbana (`kPropertyRingMonthlyAbsorb`). O
morador do anel continua dentro de `region_pop` — é um **subconjunto**, as
pessoas são conservadas.

**Propriedade por quintil (B2).** A terra segue o Gini **fundiário** declarado
(mesma lognormal da B2) quando observado; sem ele, a repartição de renda da
região. Moradia e anel seguem a concentração de capital
(`kStrataWealthConcentration`). Terra + moradia + anel entram em
`stratum_wealth` (poupança + imóveis + terra + participações, 0 até a C5).

**Imposto sobre a propriedade.** Alíquota **persistida**, padrão **0**
(`kPropertyTaxDefaultRate`). O status publica a receita potencial mensal
(valor de mercado × alíquota ÷ 12); enquanto a alíquota for 0 nada é
arrecadado — a arrecadação é do macro político, não desta fase.

## Dinheiro e conservação

Valor de ativo **não é dinheiro**: a valorização da terra/moradia/anel muda
`stratum_wealth`, nunca `money_total`. O único fluxo é o **aluguel do anel**:
debitado dos quintis inquilinos (pela poupança) e creditado aos **mesmos**
quintis donos (pela renda) na **mesma quantia** — `money_total` inalterado.
`tools/b6_property_test.gd` cobre as três pontas: valorização não vira
dinheiro, aluguel conserva, transbordo para o anel conserva pessoas.

## Save v23

O bloco `region.property` (hectares, Gini, valor por hectare e valor contábil,
renda mensal, índice de commodities, preço/aluguel da moradia, população,
preço, aluguel e fluxo do anel, migração para o anel, alíquota, receita
potencial e os cinco quintis de `land_value`, `home_value`, `ring_value` e
`wealth`) entra no save **v23**. Um save v22 (e anteriores) não tem o bloco: a
região carrega **inerte** (`property_ready` falso) e o GDScript reaplica o dado
depois do load; a alíquota nasce 0. Nada é inventado para trás. Round-trip
v23 é idêntico (coberto por `tools/economy_save_load_roundtrip_test.gd`).

## Aceite medido (2026-11-19, build debug)

`tools/b6_world_ai_365.gd` — mundo real, IA ligada, 365 d:

| métrica | valor |
|---|---|
| dia final / travados / violações | 365 / 0 / 0 |
| falências | 4 (teto 28) |
| drift de dinheiro | 0,001616 (em 6,52 bi) |
| regiões com patrimônio | 844 de 849 cidades |
| terra | 6.766.615.174 ha; 353,8 bi de valor de mercado |
| moradia do interior / anel | 108,4 bi / 0,97 bi |
| patrimônio do interior | 471,2 bi |
| anel | 40,65 mi de pessoas (0,665% da população regional); 3,89 mi/mês de aluguel pago |
| imposto (alíquota 0) | 0 arrecadado; receita potencial publicada |
| Gini de riqueza do mundo (quintis) | 0,4899 (top SWZ 0,6179; bottom SOL 0,0040) |
| portão de comida (avg 30 d < 0,975) | 147/179 nações (base B5 ~148 — sem regressão) |

Goldens regerados em 2 rodadas idênticas: A0 urbano
`02721cfb…`/`f59e7968…`, `real_60_ai` `e9a6a344…`/`9c9785a0…`,
`real_60_noai` `b8bc6c07…`/`3a0dd9af…`, `demo_365_noai`
`82be1864…`/`a7a8e16a…`. Custo da fase: a fase `property` mede **9,0 µs/dia**
de 7.546 µs/dia de tick perfilado (**0,12%**) em 60 d; no parede, 60 d com B6
15,4 s × 14,1 s sem B6 (host ocupado — número indicativo). 365 d com IA:
143 s (≈2,6 d/s) no build debug.

## Limites (abertos de propósito)

- Sem mercado de solo nem aluguel de mercado no interior; sem aluguel
  prédio a prédio.
- O anel é a periferia/favela **agregada** da região, não um distrito próprio
  no mapa.
- Aluguel do anel só redistribui entre quintis; crédito rural, hipoteca e
  garantia são da Parte C (C1/C4).
- Imposto sobre a propriedade só publica a receita potencial até existir a
  parte política/leis.
