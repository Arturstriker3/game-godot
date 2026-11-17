# A2 — IDs inteiros e arrays contíguos no caminho quente

> Fase **A2** de `docs/plans/perf_pop_financas_plano.md` (Parte A). Branch
> `perf/a2-ids`, worktree `opengs-wt-a2`, saída de `balance/global-resources`
> (`85ca5b8`, HEAD do A1b). **Não muda nenhum número**: o golden A0 continua
> saindo idêntico e o CLI de 60 dias sem IA continua byte-idêntico ao replay do
> Godot.

## 1. Perfil primeiro: onde string/mapa/busca por id apareciam

### 1.1 O que a tabela de fases do A0 já dizia

Baseline `docs/economy/perf/baseline_A0.json` (release com `OGS_PROFILE_RELEASE`,
editor fechado, 60 d, 834 cidades; valor = mediana entre as 3 rodadas, campo
`aggregate.<fase>.median_across_rounds`):

| fase | ms (mediana) | o que está dentro |
| --- | ---: | --- |
| `arrival` | 5,48 | **tudo desde o tick anterior**: retrato do mundo, índices, preços de abertura, `urban_land` mensal e a chegada de verdade |
| `trade` | 6,27 | comércio de comida (5,47) + bens (0,79) |
| `trade_food_buyers` | 4,40 | laço de compradores (o "prep" do dia sai por subtração) |
| `services` | 1,31 | serviços + moradia (F8/§19 W3) |
| `reset` | 0,74 | zera fluxos do dia |
| `production` / `raw` / `income` | 0,75 / 0,71 / 0,73 | fases paralelizadas por cidade |
| `fiscal` | 0,48 | prefeitura + equalização + dividendos |
| `accounting` | 0,32 | receita/lucro por construção, F9, PIB |
| `status` | 15,86 | publicação do status (fora do `tick_wall`; alvo da A4) |
| `tick_wall` | 36,58 | tick completo |

Duas leituras mudaram o plano desta fase:

1. **`arrival` não é a chegada.** A fase medida é o intervalo desde a última
   marca, então carrega o prólogo inteiro (retrato + índices + preços). Para
   separar, o prólogo ganhou três zonas (`prologue_snapshot`,
   `prologue_indexes`, `prologue_other`) — a de índices fica aninhada na
   "other" (é o mesmo intervalo; o valor puro sai por subtração).
2. **`trade_food_buyers` é 70% do comércio** e roda 834 × K vizinhos × 5
   alimentos por dia.

### 1.2 Os dez pontos mais caros (inventário)

| # | ponto | custo no dia | destino |
| --- | --- | --- | --- |
| 1 | `SimCore::rebuild_indexes()` **por tick**: 834 hashes de id de cidade, 846 hashes de `building.city`, 179 nós de mapa e 834 `vector` realocados | ~0,4–0,5 ms (release) | **feito**: caminho barato (`false`) no prólogo; o resto só no carregamento |
| 2 | `tariff_rate` por (comprador × candidato × alimento): monta **4 `std::string`** e faz até 4 hashes | 0 no mundo canônico (ver abaixo) | **feito**: chave inteira `(comprador, vendedor, produto)` |
| 3 | `sellers_in_landmass` / `port_sellers_in_landmass`: hash de string por vendedor e por comprador | ~1,0–1,7 mil hashes/dia | **feito**: vetor por índice de massa de terra |
| 4 | comparações `landmass == landmass` (string) por candidato, por oferta e por embarque | ~1,2 mil comparações/dia | **feito**: comparação por índice |
| 5 | `fx_rate(nation)` repetido no pedido de comércio (3–4 hashes por embarque) | ≤512 embarques/dia | **feito**: usa o cache `city_fx` já montado no prep |
| 6 | `Group::profit_by_host` (`std::map<std::string,double>`): inserção por (construção, nação) na contabilidade, iteração no fiscal | ≤846 inserções/dia | **mantido** (chave do save v14 e da ordem das somas; ver §5) |
| 7 | `cities_by_nation` por hash de nação (`credit_nation_households`, `fleet_buy_core`) | ≤198 grupos/dia + mensal | **mantido** (volume pequeno; mapa já existente) |
| 8 | `fiscal`: `by_nation` reconstruído por dia (834 hashes + 179 vetores) | ~0,08 ms (`fiscal_equalize`) | **mantido** (medido: cabe no orçamento; evita reordenar somas) |
| 9 | `owner_kind` + `group_index` por construção (parse de prefixo + `substr` + hash) | ≤846 construções/dia | **mantido** (o dono muda em runtime: semeadura, obra, nacionalização) |
| 10 | `find_city` por embarque na chegada; `city_lookup` em `rebuild_service_indexes` (12 mil negócios no dia 0, 54 mil em 60 d) | ≤512/dia + carga | **mantido**: a chegada é ≤512 embarques (hash de 30 ns) e o índice de serviços é reconstruído só no carregamento |

**Achado do perfil:** no mundo canônico (urban, IA, 60 d) o save v14 carrega
**0 tarifas** (`sv["tariffs"] == 0`) e não existe arquivo de tarifas em
`data/definitions/economy/`. Ou seja, `tariff_rate` sai no `if (lookup.empty())`
antes de montar qualquer string: o item 2 **não rende nada medido** hoje; ele
rende para mundo com tarifa declarada pelo jogador/IA (`set_tariff`, F5), e é
por isso que a implementação ficou com fallback textual. O mesmo raciocínio vale
para `fdi_mode`/`sector_tax_holiday`/`sector_subsidy` (item 9) — próximos da
conta, mas resolvidos por mapa de string de (nação, setor).

## 2. O que mudou

### 2.1 `NameTable` (nação e massa de terra)

`sim/world_state.h` ganhou `NameTable`: `names` (índice → nome) + `lookup`
(nome → índice). `build()` deduplica e **ordena lexicograficamente**; `find()`
devolve `-1` para ausência (estado explícito). `WorldState` guarda
`nation_table` e `landmass_table`.

A ordem lexicográfica é requisito, não estética: qualquer estrutura ordenada por
índice reproduz exatamente a ordem de iteração do antigo
`std::map<std::string, ...>` — e somas em ponto flutuante dependem da ordem.

### 2.2 Índices derivados nos tipos quentes

Campos **derivados** (nunca serializados; o save continua v14):

| tipo | campo | preenchido em | consumidor |
| --- | --- | --- | --- |
| `City` | `nation_index`, `landmass_index` | `rebuild_indexes(true)` | `trade_food`/`trade_goods` |
| `Building` | `city_index` | `rebuild_indexes(true)` / resolvido sob demanda | `rebuild_indexes(false)` |

(Só entram campos com consumidor. `Port`, obra, negócio de serviço e embarque
continuam resolvendo o id por hash — o volume deles é a carga do mundo, não o
tick.)

`rebuild_indexes(bool full = true)`:

- `full=false` (**todo tick**): só o índice de construções por cidade. Usa
  `Building.city_index` (sem hash), e `clear()` em vez de `assign()` para
  preservar a capacidade dos 834 vetores. Resultado idêntico ao antigo
  `assign(cities.size(), {})` + `fill`.
- `full=true` (carregamento, `set_urban`/`initialize_world`, cenário AQT
  AGRIA/PORTUS, rollback de invariante): tabelas de nomes, índices de nação/
  massa de terra das cidades, `city_lookup`, `cities_by_nation`/`fleet_order` e
  o índice de tarifas.

Os chamadores de carregamento já passavam por `rebuild_indexes()`; o default
`full=true` mantém o contrato antigo sem tocar em nenhum deles.

### 2.3 Tarifa por chave inteira

`WorldState::tariff_by_index`: `unordered_map<uint64_t, double>` com chave
`(comprador+1)<<42 | (vendedor+1)<<21 | (produto+1)` e `-1` codificando o
curinga `*`. `rebuild_tariff_lookup()` monta o mapa textual (intocado) **e** o
índice na mesma passada; `set_tariff` refaz os dois.

Resolução: `tariff_rate_at_nation(b, s, p)` procura, na mesma ordem do caminho
textual, `(b,s,p) → (b,*,p) → (b,s,*) → (b,*,*)`. Se `tariff_by_index.size() !=
tariff_lookup.size()` (alguma linha com nação/produto não resolvível) **ou** a
nação está fora da tabela, `tariff_rate`/`tariff_rate_cities` caem no caminho
textual — o resultado é sempre o mesmo. `trade_food`/`trade_goods` usam
`tariff_rate_cities` (índices de nação das duas cidades).

### 2.4 Comércio de comida: massa de terra e câmbio

- contagem de vendedores por massa de terra em `std::vector<int>` indexado
  (`sellers_in_landmass`, `port_sellers_in_landmass`);
- `same_landmass_city(a, b)` compara índices e cai na string se algum índice
  faltar (nunca acontece com cidade carregada);
- `order.origin_price` / `fx_rate` / `seller_fx_rate` do pedido usam
  `city_fx[...]` (o mesmo valor que `fx_rate(nation)` devolveria; o câmbio roda
  na fase `fx`, depois do comércio).

### 2.5 Zonas de perfil finas

`sim_types.h` (`Phase`) + `economy_sim.cpp` (`kNames`): `prologue_snapshot`,
`prologue_indexes`, `prologue_other`, `arrival_transit`, `fiscal_city_gov`,
`fiscal_equalize`, `fiscal_groups`. `tools/perf_day_peak.gd` publica as fases
novas no JSON do bench. Sem `OGS_PROFILING` as macros continuam vazias (custo
zero no release publicado).

Nota de medição: uma zona por cidade (dentro do laço) sobrescreveria o slot 834
vezes — ficou **fora**; a contabilidade F9 é lida em bloco pela fase
`accounting`.

## 3. Por que continua bit-idêntico

- Nenhuma soma foi reordenada: só lookups, comparações e caches mudaram de
  string para índice.
- `NameTable` é lexicográfico, então um mapa ordenado por índice teria a mesma
  ordem de um `std::map<std::string, …>` (não há mapa novo por índice nesta
  fase; a propriedade é o que permite usá-los depois).
- `sellers_in_landmass` era `unordered_map` (ordem irrelevante: só contagens); o
  vetor devolve as mesmas contagens.
- `rebuild_indexes(false)` produz exatamente a mesma lista de construções por
  cidade (mesma ordem: índice de construção crescente).
- Tarifa: fallback textual sempre que o índice não cobre o caso.
- Índices derivados nunca entram no save; o save continua v14 e o snapshot é o
  mesmo `definitions + save`.

## 4. Aceite

Rodado no worktree `opengs-wt-a2`, DLL **debug** construída localmente
(`build-debug`), editor fechado.

a) **Equivalência A0** — `tools/world_equivalence.ps1 -Rounds 2
   -Baseline docs/economy/refactor/baseline/A0_60_urban.json`:
   **0 diferenças**; `save 3e3ebbbe…1a3d0b7`, `status d79bfe32…c95ac9b`,
   `money 2.200.539.587,40`, 0 falências — os mesmos shas do A0.

b) **Determinismo** — rodada 1 × rodada 2: 0 diferenças (tolerância 0).

c) **Threads 1 × 8** — `world_equivalence.gd --threads=1` e `--threads=8`:
   mesmos dois shas do golden nas duas.

d) **CLI × Godot** — snapshot exportado (54.875.755 B), replay de 60 d no Godot
   e `opengs_sim_cli --days=60` (threads 1 e 8): **três arquivos byte-idênticos**,
   SHA-256 `79c7897a415c6c593230c64a2adda908bf97046d0dcfdccc50f253ccacbf8feb`
   (o mesmo do A1). 834 cidades, 0 violações de invariante.

e) **Bateria** — verdes: `economy_integration_test`, `economy_ai_test`,
   `population_demo_test`, `economy_save_load_roundtrip_test` (30+30 d, 0 diffs),
   `construction_project_test`, `housing_market_test`, `city_gov_test`.

f) **Bench canônico** — `tools/bench_canonical.ps1 -Rounds 3
   -Out docs/economy/perf/a2_ids.json` (release com `OGS_PROFILE_RELEASE`,
   60 d/dia, 3 rodadas). **As três tentativas caíram em host carregado por outra
   frente** (sweep de 6 processos Godot, verificado no `Get-Process` antes e
   depois de cada rodada). Resultado registrado em `a2_ids.json`:

   | medição | tick_wall (ms) | dia total (ms) | variação entre rodadas | veredito |
   | --- | ---: | ---: | ---: | --- |
   | A0 (baseline, host ocioso) | 36,58 | 155,51 | 1,96% (tick) / 1,18% (dia) | OK |
   | A2 (host com sweep de outra frente) | 48,78 | 210,63 | 10,05% / 5,71% | reprovado (>3%) |

   (Foram três tentativas, todas com o mesmo sweep ativo; o JSON versionado é o
   da última — as anteriores foram sobrescritas por ela.)

   Ou seja: **nenhuma dessas medições serve para comparar com o A0** — a carga de
   terceiros inflaciona o tick em ~30% e a variação entre rodadas fica acima do
   limite de 3% do próprio bench. O que a medição A2 mostra de estrutural:

   | fase | A0 | A2 | leitura |
   | --- | ---: | ---: | --- |
   | `prologue_indexes` | — (não existia) | **0,010** | o caminho barato que substituiu o rebuild completo |
   | `prologue_snapshot` | — (não existia) | 5,456 | retrato do mundo: maior custo isolado do prólogo |
   | `prologue_other` | — (não existia) | 1,002 | resto do prólogo + preços de abertura |
   | `trade_food_buyers` | 4,40 | 5,25 | +19%: acompanha a inflação da carga, não uma regressão |
   | `status` | 15,86 | 20,65 | fora do `tick_wall`; alvo da A4 |

   **Conclusão honesta:** nesta máquina e com este mundo, o ganho da A2 fica
   **abaixo do ruído do bench** — o trabalho removido (estimativa de 0,4 ms/dia pelas operações: 834 hashes
   de id + 846 hashes de `building.city` + 179 nós de mapa + 834 realocações)
   é ~1% de um tick de 36 ms, e o tick é dominado por `status` (A4) e pelo
   retrato (A3). O valor estrutural da fase é o que ela destrava: estado quente
   indexado por inteiro e tabela de nomes pronta para as coortes de B/C.
   **Pendência:** re-rodar o bench em host ocioso e publicar o número (o script
   aborta/reprova por variação; não há como "forçar" um veredito).

## 5. Limites desta fase

- **`Group::profit_by_host` continua `std::map<std::string, double>`.** É chave
  do save v14 (ordem alfabética) e a ordem das somas do fiscal depende dela;
  trocar por índice exige mapear nomes↔índices no save e remapear em cada
  `rebuild_indexes(true)`. Custo medido: ≤846 inserções/dia (~0,1 ms em debug).
- **`group_flows`, `confidence_penalty_day`, `fdi_lookup`, `incentive_lookup`,
  `minimum_wages`** continuam por string: preservam a ordem/orientação do save e
  são poucos milhares de acessos por dia (medidos dentro de `fiscal_groups` e
  `fiscal_equalize`).
- **`sector_tax_holiday`/`sector_subsidy`** montam `nação + "|" + setor` por
  linha de grupo (~1,7 mil construções/dia). Próximo passo natural: `NameTable`
  de setor + tabela densa `[nação × setor]` (mesma receita da tarifa).
- **`owner_kind`** faz parse do dono por construção na contabilidade; o dono
  muda em runtime (semeadura de grupos, obra, nacionalização), então cachear
  exige invalidação em três pontos — ficou para depois.
- **`snapshot_state` é o maior custo isolado do prólogo** (`prologue_snapshot`):
  cópia de 834 cidades (~27 KB cada), 12–54 mil negócios e as construções. Não é
  custo de ID: é banda de memória e pertence à fila de comandos (A3) / status
  sob demanda (A4).
- **`status` (~15,9 ms)** continua sendo a maior fase do dia — alvo da A4.
- **Carga do snapshot no CLI**: não foi tocada; o parse continua custando ~1 s
  para 52 MiB.
