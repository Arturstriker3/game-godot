# Q-018 — B3: região da cidade = estados inteiros (BRA/IND/USA à mão)

> Revisor: `verificador-flash` (Flash diferente do executor) · 2026-11-17
> Escopo: `data/definitions/population/state_regions.json` (`sha256 2e375cfb…`),
> `tools/build_state_regions.py`, branch `data/b3-states` (worktree `opengs-wt-data-states`).
> Veredito: **APROVADO com ressalvas (nenhuma bloqueante)**.

## Checagens (7/7)

| # | Checagem | Resultado |
|---|---|---|
| 1 | Estrutura | 4.226 estados, 834 cidades, 15.547 províncias; 0 órfãs, 0 duplicadas; BRA 27/849, IND 35/322, USA 53/948 províncias cobertas; 0 estado de outra nação; 6 `city_id` nulo (territórios de população 0) |
| 2 | Nome/nação | 30 nomes conferidos (10 por país) = 100% iguais ao registro de mesmo `adm1_code` na fonte NE; nação correta (Guam com `adm0_a3=GUM` e tag de jogo `USA`, como no atlas); 2.103/2.119 (99,2%) das províncias de BRA/IND/USA têm centroide dentro do próprio polígono |
| 3 | Cidade → estado | São Paulo, Rio, Curitiba, Porto Alegre, Manaus, Belém; Mumbai, Patna, Kolkata, New Delhi, Chennai; NYC, Chicago, Washington, Houston, Los Angeles — estado contenedor presente em `cities[].states`; `population_region` = soma das províncias possuídas (ou ≤ soma dos estados listados quando o estado é dividido) |
| 4 | Somas | BRA 212.812.405, IND 1.463.865.525, USA 341.784.857 e global 8.203.501.062 = `nations.json`; `region_population == max(0, região − urbano)` em 834/834 |
| 5 | Adjacência | SP×MG, MG×RJ, UP×Bihar e CA×OR com divisa comum (0,000°); controles SP×AM (13,6°) e CA×TX (7,8°) não vizinhos — o critério discrimina |
| 6 | Antes × depois | Patna 226.162.854 → 133.266.004 (−41%); BRA Porto Alegre +6,07 M; IND Kolkata +110,8 M / New Delhi −55,5 M; USA Nashville −18,3 M / Miami +15,6 M |
| 7 | Erros grosseiros | 0 população negativa, 0 área negativa, 0 estado sem província, 0 região negativa; `sha256` do arquivo = o declarado |

## Ressalvas (registradas, não bloqueiam)

- **R1** — 17 cidades com `population_region < urban_population` (piso 0 no interior):
  Buenos Aires 4,85 M/17 M, Shanghai 24,33 M/42,3 M, Hong Kong 4,10 M/7,5 M, Chennai 11,78 M/13,3 M,
  Miyakojima 0/54.908. É consequência da aglomeração do roster ser maior que o estado inteiro —
  decisão de economia (opção 3 do plano), não erro de dado.
- **R2** — Belém fica 0,007° (0,7 km) fora do polígono do Pará (baía/maré) e caiu no fallback de
  centroide; resultado correto (Pará).
- **R3** — 2 estados com `name` vazio (`ATA+99?`, `RUS+99?`) porque a fonte NE também vem sem nome;
  o build copia o nome verbatim em vez de inventar. Sugestão futura: usar `iso_3166_2` como rótulo.
- **R4** — 16 províncias de BRA/IND/USA (0,8%) têm centroide fora do próprio estado (slivers de
  fronteira/partes disjuntas: Rajasthan 30 e MP 9 caem em UP, Puducherry em TN). Efeito local, dentro
  da mesma nação.
- **R5** — Miyakojima e Ponce sem nenhum estado (segunda cidade de estado com uma única província).
- **R6** — higiene: 11 rascunhos `tools/_tmp_b3_*.py` no worktree (removidos antes do commit).
- **R7** — o revisor conferiu o SHA entregue, mas não reexecutou o build (WorldPop 1 km + raster
  8192×4096 é caro). O determinismo 2× foi medido pelo gerente.

## Estado do repositório na revisão

`git status` só com arquivos novos; `git diff` vazio; nada de `core/`, `game/`, `map/` ou C++;
Godot não executado.
