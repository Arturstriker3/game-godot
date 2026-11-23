# D-125 — Eficiência de produção (estilo HOI4)

Status: implementação em curso. Spec aprovada pelo dono em 23/11/2026
(`specs/D-125_eficiencia_producao.md`).

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
mesma proporção); folha e opex continuam cheios — a planta emprega a equipe
enquanto aprende. `last_utilization` (base da folha) é a operação SEM
eficiência; `utilization` publicado é a produção ÷ capacidade.

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
balanceamento declarados), espelhados em `sim/efficiency.h`. A IA usa a API
com custo de transição declarado (`ai.*` no JSON): só troca quando o ganho de
receita do modo cobre a produção perdida na transição.

## API (sem UI)

* `change_product(city_id, building_id, product, recipe_id="")`
* `set_refinery_mode(city_id, building_id, mode)` (`standard|diesel|jet` ou 0..2)
* `set_building_equipment(city_id, building_id, level)`
* `set_efficiency_params(values)` — JSON de defaults
* `mature_world_efficiency()` — marca o mundo semeado como maduro (ano-base)

## Verificação

Ver `GAME_STATE.md` (D-125) para os números da rodada de 365 d.
