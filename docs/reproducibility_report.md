# Relatório de reprodutibilidade do dataset natural

Data: 2026-11-10 · Autoria: agente de reconstrução (subagente) + revisão do orquestrador

## Objetivo

Tornar reprodutível a geração de `data/definitions/natural/{nature,agriculture,deposits}.json`,
reconstruindo os inputs ausentes documentados em `GAME_STATE.md` ("Reprodução do dataset").

## O que foi reconstruído

| Caminho | Status | Origem |
| --- | --- | --- |
| `tools/data/mrds_deposits.jsonl` | gitignored (download) | `tools/fetch_usgs_deposits.py` (USGS WFS via r.jina.ai) |
| `data/definitions/provinces.txt` | gerado (4.529 linhas) | `map/map_data/atlas/provinces.json` |
| `data/history/provinces/provinces.json` | gerado (4.529 donos) | idem |
| `data/history/territories/territories.json` | gerado (~250 territórios) | idem |
| `tools/reconstruct_natural_inputs.py` | commitado | gerador da reconstrução |

## Formato e consistência

- `provinces.txt`: uma linha por província admin-1 do atlas, no formato legado
  `id;land;r;g;b;px;py;PLAINS`. O id preserva o **id global do atlas** (`PRV%06d`), de modo
  que o mesmo ponto `(px, py)` mapeie para a mesma província do raster
  `world_province.png` (id = R + G*256) usado por `build_resource_map.py`.
- `provinces.json`: `{PRVxxxxxx: {province_owner, province_controller}}`; dono = `nation` do atlas.
- `territories.json`: **um território por nação** (`TRT%06d`), o mesmo desenho do gerador
  legado ("estado = nação, no primeiro passo").

## Verificação dry-run (sem executar `main()`)

`build_natural_geography.py` consegue ler os inputs reconstruídos:

- `read_provinces()` → 4.529 províncias (kind `land`, px/py dentro de 5000×2500).
- `read_owners()` → 4.529 donos (nenhuma província sem dono).
- `read_territories()` → ~250 territórios.
- `build_territory_map()` → ~250 territórios / 4.529 províncias, 0 órfãos.

## Divergências conhecidas (o que NÃO é reproduzido fielmente)

1. **Províncias/territórios commitados vs. atlas.** Os dados naturais commitados
   (`nature.json` 737 territórios, `deposits.json` 2.249 depósitos) vieram de um gerador
   **legado Voronoi** (~14.402 províncias em projeção 8192×, lat −60..84; 737 territórios).
   O gerador legado (`generate_opengs_map.py`) e o atlas atual divergem:
   - `generate_opengs_map.py`: 8192×, lat −60..84, províncias Voronoi, escreve `map/map_data/*`.
   - `build_atlas_layers.py`: 5000×2500 global, 4.529 províncias admin-1, escreve `map/map_data/atlas/*`.
   A reconstrução adota a fonte **canônica atual** (atlas 5000×2500), então uma regeneração
   produz ~4.529 províncias / ~250 territórios — **diferente** dos 14.402 / 737 commitados.

2. **Dono (owner).** Os artefatos commitados usam `GBR/FRA/DEU` + `NNN` (estágio antigo);
   a reconstrução usa o campo `nation` do atlas (8 nações jogáveis + ADM0_A3).

3. **Blue Marble ausente.** `generate_opengs_map.py` precisa de
   `G:\tools\ne_downloads\blue_marble_crop.jpg` (NASA GIBS) para `ne2.png` — não é necessário
   para `build_natural_geography.py`, mas bloqueia a regeneração completa do mapa legado.

## O que falta para o pipeline completo rodar

1. **MRDS** — o download via `r.jina.ai` do WFS do USGS precisa concluir (~289 mil registros).
2. **Aceitar a divergência** — a regeneração com os inputs reconstruídos produz uma base
   consistente com o atlas atual (5000×2500, admin-1), mas **diferente** dos artefatos
   commitados. Reprodução byte-a-byte exigiria reconstituir o gerador Voronoi legado exato.
3. **Cadeia downstream** (escrevem dados commitados; não executada nesta tarefa):
   `build_natural_geography.py` → `apply_deposit_supplement.py` → `mark_facilities.py` →
   `apply_real_climate.py` → `build_resource_map.py`.

## Arquivos commitados tocados

Nenhum. `git status` mostrou apenas entradas novas (todas autorizadas) além dos untracked
pré-existentes do usuário (`Claude outputs/`, `tools/claude_bridge/`).
