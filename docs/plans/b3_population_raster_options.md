# B3 — Raster de população por província: opções e recomendação

> Pesquisa de 2026-11-17, sem download de raster. Objetivo: escolher a fonte que soma a
> população por província do atlas e daí para a cidade dona (plano `perf_pop_financas_plano.md`, §B3).
> Nenhum arquivo grande foi baixado; tamanhos marcados "medido" vieram de HEAD/índice em
> 2026-11-17, os marcados "estimado" são cálculo de células × bytes.

## Comparação

| | **GPW v4.11** (CIESIN/NASA SEDAC) | **WorldPop Global2, R2025A v1** |
|---|---|---|
| Fonte | CIESIN — Columbia University / NASA SEDAC | WorldPop — Univ. Southampton |
| DOI | 10.7927/H4JW8BX5 | 10.5258/SOTON/WP00840 (1 km, por país) · WP00845 (mosaicos) |
| Licença | **CC BY 4.0** — uso comercial e não comercial, só atribuição | **CC BY 4.0** — idem |
| Anos disponíveis | 2000, 2005, 2010, 2015, **2020** (passo de 5 anos) | **2015 a 2030, anual** |
| Ano mais próximo de 2026 | **2020 (−6 anos)** | **2026 (exato)** |
| Resolução | 30 arc-seg (~1 km); agregados 2,5′, 15′, 30′, 1° | 1 km global; 100 m e 1 km por país |
| Formato | GeoTIFF, ASCII, WMS (54 granules no conjunto) | GeoTIFF, WGS84, 1 banda por arquivo |
| Tamanho | ~1,9 GB descomprimido por ano (estimado: 933 M células × 2 bytes); SEDAC entrega por blocos | Mosaico global 1 km, todos os anos 2015–2030: **5,2 GB em zip único (medido)**; por país 1 km ≈ **46 MB** (exemplo medido: BRA 2020 1 km agregado, 45.780.882 B) |
| Totais nacionais | Base censitária própria (variante ajustada existe, mas a **WPP 2015**) | **Alinhados à WPP 2024** (release statement, p. 1) |
| Estrutura | Só total por célula | Total **e** idade×sexo (0–1, 1–4, 5–9 … 90+) |
| Cobertura | Global | 242 países; **9 países/territórios não modelados** (desabitados ou total estático) |
| Maturidade | Estável (2018) | **Alpha** (R2025A v1), pode mudar; declara isso no release statement |

## Recomendação: **WorldPop Global2 R2025A v1, 1 km, ano 2026**
1. **Ano exato**: 2026 existe; GPW para em 2020 — usar GPW exigiria inventar uma projeção de 6 anos, o que o projeto proíbe.
2. **Totais alinhados à WPP 2024**, a mesma fonte de `age_structure_2026.json` (B1). O rateio por província e a pirâmide etária passam a fechar na mesma população nacional.
3. 1 km basta: a menor província do atlas ainda tem dezenas de células; 100 m só multiplicaria download e tempo.
4. Licença CC BY 4.0 nos dois casos; a atribuição vai no JSON gerado e no doc de fontes.
5. Riscos aceitos e declarados: release **alpha** (fixar `R2025A v1` no nome do arquivo e registrar SHA-256) e os 9 países não modelados (preencher com GPW 2020 marcado `method:"fallback_gpw2020"`).

**Plano B**: GPW v4.11 2020 (30 arc-seg), se o alpha do WorldPop for reprovado na revisão; perde 6 anos e o alinhamento com a WPP 2024.

## Pipeline proposto (uma passada, offline)
1. Baixar o mosaico 1 km (5,2 GB) **uma vez** em `tools/data/worldpop/` (gitignored) ou, se o espaço pesar, os zips por país do ano 2026; registrar URL, `R2025A v1` e SHA-256 em `tools/data/worldpop_sources.json`.
2. Reprojetar as províncias do atlas (`data/definitions/world/`) para EPSG:4326 (o WorldPop 1 km está em WGS84, 30 arc-seg ≈ 0,008333°) e rasterizá-las num índice de província com `pyshp` + `scipy.ndimage`, na mesma grade do raster.
3. Somar a população por província. Nas células de fronteira, ratear pela fração de área da célula que cai em cada província (evita perder ou contar duas vezes a borda).
4. Validar: soma global ≈ 8,2 bi; soma por nação vs WPP 2024 (tolerância 0,5%); províncias sem dado listadas, nunca estimadas em silêncio.
5. Fator de compatibilidade: os totais de `nations.json` são de vintage anterior à WPP 2024 (ex.: USA 341,8 M vs 349,0 M). Calcular `factor = pop_nations.json / pop_wpp2024` por nação e registrar; aplicar o fator ao rateio para não quebrar o contrato atual. Nações com |factor−1| > 5% vão para a lista de revisão.
6. Cidade dona da província (`assign_provinces`) recebe o total; **interior = soma das províncias − população da aglomeração** (roster de cidades), com piso 0 e log quando o interior ficar negativo (sinal de província mal recortada).
7. Saída versionada `data/definitions/world/province_population_2026.json` com `{version, source, url, sha256, retrieved, method, nations:{ISO3:{province_pop, total, factor, flags}}, cities:{...}}` e um script reprodutível `tools/build_province_population.py`.
8. Aceite de B3: população nacional simulada = dado real (depois do fator do passo 5) e nenhuma cidade com interior negativo.
