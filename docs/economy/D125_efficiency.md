# D-125 — Eficiência de produção (estilo HOI4)

Status: **implementada** (núcleo, IA e save; UI pendente). Spec aprovada pelo
dono em 23/11/2026 (`specs/D-125_eficiencia_producao.md`). Rodada de 365 d
dentro do portão em 23/11/2026 (ver "Verificação").

## Regra

Nada produz no máximo de imediato. Construção nova — ou que troca de produto —
começa com eficiência baixa e cresce com os dias de operação até o teto:

| Tipo | Inicial | Maturidade |
|---|---|---|
| Fábrica / refinaria | 30% | 90% em 90 d; 100% em 180 d |
| Mina | 25% | 100% em ~240 d |
| Fazenda | 20% | 100% em ~1 ano (365 d) |
| Pecuária | 20% | ciclo dos animais (540 d) |

A eficiência **multiplica a produção** (principal, insumos e subprodutos na
mesma proporção), mas **uma única vez**: ela entra como teto da capacidade da
construção, `capacity × output_scale × aptidão × trabalho × energia ×
eficiência`. Folha e opex continuam cheios — a planta emprega a equipe
enquanto aprende. `last_utilization` (base da folha) é a operação SEM
eficiência; `utilization` publicado é a produção ÷ capacidade.

> **Correção (commit `e468836c`, 23/11/2026):** o alvo de vendas era
> multiplicado pela eficiência *e* o teto por construção já a aplicava — dupla
> penalidade. Com a rampa, o alvo encolhia a cada dia (venda < alvo ×
> eficiência), o estoque não repunha e a planta zerava antes da primeira
> reposição, falindo com folha cheia (algodão e minas). O alvo agora é
> toneladas de mercado (`sold_prev` + reposição do estoque-alvo) e a eficiência
> entra só no teto de capacidade.

## Troca de produto

* Dias de transição **sem produção**: refinaria 7, fábrica militar 14, Fab de
  semicondutores 30, fábrica comum 7; fazenda = próxima janela de plantio do
  calendário agrícola do produto-alvo (erro honesto quando não há plano).
* Retenção: mesma família (`goods.json.sector`) guarda 60% da eficiência;
  família diferente zera (recomeça no inicial da classe).
* Minas não trocam de produto (a jazida manda): a API recusa.

## Refinaria: modos

| Modo | Gasolina | Diesel | Querosene |
|---|---|---|---|
| padrão | 0,45 | 0,29 | 0,09 |
| foco diesel | 0,38 | 0,36 | 0,09 |
| foco querosene | 0,40 | 0,28 | 0,15 |

A troca de modo leva 3 dias sem produção (transição). O padrão é idêntico ao
dado `gasoline_a` da F0 (45/29/9), então a calibração do ano-base não muda.

Os modos valem **só para o refino de petróleo**: a receita que entrega gasolina
E consome petróleo bruto (`gasoline_a`, prédio `refinery` no `recipes.json`).
O prefixo do id não é assinatura do processo — `gasoline_b` é o etanol
(`chemical_plant`, insumo `ethanol`) e uma fábrica que consome petróleo
(plástico) não é refinaria; a API recusa o modo nessas construções.

## Equipamento (trator / máquina de mineração)

* `equipment` (0..1) é o nível do equipamento da fazenda/mina. Sem equipamento
  o teto de maturação é 85%; com equipamento 100% e crescimento ×1,5.
* Equipamento exige diesel: sem estoque no mercado da cidade o trator/máquina
  fica parado no dia (não acelera, não consome). A eficiência já conquistada
  NÃO cai — o equipamento parado só impede a maturação seguinte.
* Mundo inicial (ano-base) nasce maduro: `efficiency = 1`, `equipment = 1`.

## Save e parâmetros

Save **v40 aditivo**: por construção `production_efficiency`,
`efficiency_days`, `transition_days`, `equipment`, `refinery_mode`; bloco
`definitions.efficiency` com os parâmetros. Save v39 e anteriores carregam com
eficiência madura (o estoque existente do ano-base) — nada inventado no passado.

Parâmetros em `data/definitions/production/efficiency.json` (defaults de
balanceamento declarados), espelhados em `sim/efficiency.h` — o espelho
compilado é **idêntico** ao JSON (teste `d125_efficiency_test` item 8), para o
mundo não mudar se o arquivo não carregar. A IA usa a API
com custo de transição declarado (`ai.*` no JSON): só troca quando o ganho de
receita do modo cobre a produção perdida na transição.

O JSON não declara mais `equipment.ranch_products` nem
`refinery_modes.recipe_prefix`: nenhum código os lia (a pecuária é o produto
`meat` no núcleo e o refino é identificado pela receita — saída gasolina +
insumo petróleo), então os campos saíram em vez de virarem contrato morto.

O gate de payback do investidor privado (`private_investor.gd` e o porte
`ai_private.cpp`) usa a eficiência **esperada** — a média da curva no horizonte
de payback, não a capacidade cheia. Folha/opex continuam cheios; a planta
existente continua a rampa a partir da eficiência corrente.

O investidor de **grupo** (`ai_group.cpp`) desconta a mesma média da rampa do
candidato não alimentar (`ai_expected_new_efficiency`, a mesma
`eff::mean_efficiency` e os mesmos horizontes do privado: receita/bulk 730 d,
primário 150 d) em vez do valor fixo 0,5. A cesta alimentar mantém o peso cheio:
a comida é prioridade declarada do grupo e o candidato só nasce com escassez
local. O porte GDScript (`group_investor.gd`, usado pelas ferramentas antigas)
mantém o valor fixo; a regra viva do jogo é a do núcleo C++.

## Verificação

Ver `GAME_STATE.md` (D-125) para os números da rodada de 365 d.

Rodada 365 base de 23/11/2026 (`e468836c` + fixture do teste em
`b25d1b27`, `efficiency.json` real, sem override,
`--control=res://docs/economy/perf/c6_gate_base.json`, 391 s): portão
**92/179** nações abaixo de 0,975 (controle c6 93; mediana 0,9735; média
0,9065), **3 falências** (as 2 minas PRI-PON do controle + `MLT-VAL` de trigo
por insolvência bancária; eram **20** de algodão antes da correção da dupla
penalidade), **0 violações**, **0 dias travados**, drift 0,005610, digest
`6ae5b3e8`. A variante A (`equipment.fuel_per_capacity_day=0`, só como
referência) deu **2 falências**, comida **94/179** (mediana 0,9711), 0
violações, drift 0,005596, digest `cdda1daf`. Critério do gate — falências
≤ 3, comida ≤ 97/179, 0 violações — **atendido**.

O `economy_ai_test` usa um **choque de preço do trigo** como fixture do
cenário de escassez (2× o preço de partida, sustentado): com a rampa, a
fazenda nova rende ~36% da capacidade média no horizonte de payback de 150 d,
e o gate da IA desconta essa média. Asserções e demais cenários não mudaram.

Ressalvas do QA (24/11/2026, `8cac8516`): a rodada 365 base foi reexecutada e
reproduziu o mesmo resultado — **3 falências**, **comida 92/179**, **0
violações**, 0 travados, drift 0,005610, digest `6ae5b3e8`; a troca do 0,5 do
investidor de grupo pela média da rampa não moveu a trajetória do mundo base.

## API (sem UI)

* `change_product(city_id, building_id, product, recipe_id="")`
* `set_refinery_mode(city_id, building_id, mode)` (`standard|diesel|jet` ou 0..2)
* `set_building_equipment(city_id, building_id, level)`
* `set_efficiency_params(values)` — JSON de defaults
* `get_efficiency_params()` — parâmetros aplicados (JSON ou espelho)
* `mature_world_efficiency()` — marca o mundo semeado como maduro (ano-base)
