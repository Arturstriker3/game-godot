# §19 W0 — solo urbano, moradia e prefeitura: entrega e evidências

Pacote W0 da `docs/economy/s19_urban_spec.md`, feito numa worktree própria
(`feat/s19-w0`) a partir de `balance/global-resources` (2100a5a). Sem lógica
diária: só estruturas, constantes, loader dos quatro JSON e save/load v13.

## O que entrou

- `gdextension/src/sim/urban_types.h` (novo): `Zone`, `Density`, `Tier`,
  `Owner`, `Tenure` e as structs `CityLand`, `CityHousing`, `CityGov`.
- `WorldState`: vetores paralelos a `cities` — `land`, `housing`, `city_gov`.
- `sim_constants.h`: save `v13` (campo aditivo) e constantes do §19 (m² por
  unidade por densidade/padrão, FAR por densidade, cap rate 0,05/ano,
  depreciação anual de 1%, custo de infraestrutura por km² e taxa local).
- Fachada (`EconomySim`): `demo_set_urban(urban_land, housing_city, housing,
  municipal_finance)` e leitura `demo_get_urban_city(city_id)` (teste/UI).
- `core/economy/economy.gd`: carrega os quatro JSON **depois** do mundo (o
  casamento é pelo id da cidade) e chama o loader se o binário tiver o método.
- `edge/save_load.cpp`: bloco `urban` (v13) com ida-e-volta por id e migração
  v12→v13 trivial (sem a chave, os vetores ficam como estão; num load frio,
  vazios — não há lógica diária a reconstruir).

## Decisões de carga (documentadas no código)

- Área por zona = `built_up_km2` × `zoning_split` (residencial/comercial/
  industrial) da fonte.
- Gabarito = `height_class` (low/mid/high), aplicado às três zonas no W0.
- Estoque por padrão = `housing_stock_by_tier`; a repartição por densidade é
  **derivada** da capacidade física permitida (FAR até o gabarito da cidade),
  não um dado observado. Cidade baixa fica 100% em casas.
- Posse = `tenure_by_tier` (own/rent/informal) e `homeownership_rate`; o
  estoque alugado é atribuído a empresas até W2/W3 criarem moradia social.
- Aluguel/preço = USD/m² da fonte × m² da unidade do padrão. O USD da fonte é
  derivado da renda PPP e a âncora XGS é referenciada no dólar (`currencies.json`:
  USD = 1,0), então 1 USD = 1 XGS; a taxa da nação entra na exibição local
  (`price_local = XGS ÷ taxa`), não na carga. Multiplicar por `rate_per_anchor`
  converteria o valor em moeda local e mudaria a magnitude em 5–10×.
- Sem preço de terra na fonte: `land_price = 0` (ausência explícita; W1 move).
- Sem caixa municipal na fonte: `cash = 0` (W2 acumula receita).
- `iptu_rate` = peso do IPTU no total de tributos / 100 (proxy da alíquota
  anual; calibração real no W2); `transfer_share` = transferências
  constitucionais / 100; `fee_rate` = constante de projeto 0,02 até o W2.

## Construção

- Debug: `cmake -S gdextension -B gdextension/build-debug` (Ninja, clang) +
  `--build` → link OK.
- Release: `-DCMAKE_BUILD_TYPE=Release -DGODOTCPP_TARGET=template_release` →
  link OK.
- `godot_cpp` em `sim/`: nenhum include/uso; o novo header é STL puro.

## Testes

Todos rodados na worktree, com a DLL debug da própria worktree:

| Teste | Resultado |
| --- | --- |
| `tools/urban_load_test.gd` (novo) | OK — São Paulo, Tóquio, Lagos e Weipa: área = built-up, unidades = estoque por padrão, tenure/owner somam 1, aluguel/preço > 0 em XGS e relação preço/aluguel = 240 (cap rate 0,05), prefeitura com IPTU/taxa/repasse; save/load v13 idêntico; v12→v13 sem estado urbano |
| `economy_save_load_roundtrip_test.gd` real 60d (30+30) | OK, 0 diffs |
| `economy_save_load_roundtrip_test.gd` demo 60d (30+30) | OK, 0 diffs |
| `economy_integration_test.gd` | OK (schema 13; migrações v1→v3 e v7→v8) |

## Goldens (save_sha256 muda só por campos novos)

Baseline = HEAD `2100a5a` compilada numa worktree separada (mesmo toolchain),
mesmos 60 dias, sem §19. A simulação não mudou: `status_sha256` idêntico nos
dois mundos. O save novo difere **apenas** pela versão 13 e pelo bloco `urban`:
removendo `urban` e voltando a versão para 12, o SHA bate exatamente com o do
binário anterior.

| Mundo (60d) | save antigo (v12) | save novo (v13) | save novo sem `urban`, v12 | status (antigo = novo) |
| --- | --- | --- | --- | --- |
| real | `3caf713a1c01a1283a61ccfc0c2153511c2a7133186f2cc524a60ae3ef2bf8d1` | `bfcf80ac1a039da0235a10eebd6aab29bcff947e7e783847fd6d551f2d486431` | `3caf713a…f2bf8d1` (igual) | `abde91538c3bd5aadcd2d9bc1da411a9c66e02bf1562f39cbcb39bfa3c5c1af9` |
| demo AGRIA/PORTUS | `0928069dee9b5fc1572b514f3cb9058fd51c61834d44ad3fb84da6fb75cac541` | `bf56ebe7994f4939c664c04bbbe5e34a3f80ecec7a6599b712bd2dd939ee48a3` | `0928069d…75cac541` (igual) | `c4240ef38afcde77e7f56188e9dea9a64ec296a543cd980e796c4b2c9094e122` |

No cenário demo não há cidade com dado urbano: o bloco sai vazio (`urban: []`)
e o status é bit a bit o mesmo do binário anterior.

## Fora do escopo (pacotes seguintes)

- W1: preço da terra, ocupação/capacidade, zona como limite de negócio e
  expansão/adensamento como projeto §15.
- W2: fluxo mensal da prefeitura (IPTU, taxas, repasse, manutenção, obras e
  moradia social), IA de expandir/adensar e calibração da receita municipal.
- W3: mercado de moradia (demanda por estrato, aluguel, construção, depreciação,
  favela) — hoje `condition`, `vacancy` e `housing_stress` ainda não se movem.
