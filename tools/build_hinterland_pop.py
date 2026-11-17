#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""B3 - cidade = aglomeracao + interior: popula��o real por regiao de cidade.

Le o mosaico global WorldPop Global2 R2025A v1 (1 km, 2026, "constrained"),
soma a popula��o em cada PROVINCIA do atlas admin-1 (mesma grade/proje��o de
`tools/compute_province_areas.py`: `atlas/world_province.png` 8192x4096
equirretangular, mascaras `world_sdf.png` e `nav_mask.png`), atribui cada
provincia a sua cidade dona com a replica Python de
`core/economy/geography_service.gd` (`tools/geography_replica.py`) e grava
`data/definitions/population/hinterland_2026.json`.

Saida por cidade:
    population_region      soma das provincias que a cidade administra
    urban_population       aglomeracao (cities.json, campo `urban_population`)
    hinterland_population  population_region - urban_population (piso 0)

Reamostragem 1 km -> pixel do atlas: cada celula WorldPop de 30 arc-seg tem o
CENTRO mapeado para o pixel do atlas que o contem
(`col = floor((lon+180)*8192/360)`, `row = floor((90-lat)*4096/180)`), sem
interpola��o e sem repartir area -- a celula 1 km (~0,93 km2 no equador) entra
inteira no pixel do atlas (~17,5 km x 17,5 km no equador, caindo com cos(lat)).
A celula entra na provincia do pixel; a mascara de terra/nav do
`compute_province_areas.py` NAO e aplicada aqui (ela cortaria 1,8% da populacao
mundial, concentrada em centros urbanos costeiros). O que cai em pixel sem
provincia (`id = 0`) e contado em separado e declarado no relatorio.

Ajuste por na��o: o total raster de cada na��o (soma das suas provincias) e
comparado com a popula��o de `scenario/nations.json`; o fator
`population_region_raw * factor` e apportionado em INTEIROS por maior resto
dentro da na��o para que a soma das cidades bata exatamente o total nacional.

Uso:
  python tools/build_hinterland_pop.py            (le o TIFF padrao)
  python tools/build_hinterland_pop.py --tif ARQ  (outro mosaico 1 km)

Deterministico: sem data de execu��o no JSON, ordem fixa, soma em float64 e
apportionamento por maior resto -> duas execu��es no mesmo TIFF geram o mesmo
SHA-256.
"""
import argparse
import hashlib
import json
import math
import os
import sys

import numpy as np
import tifffile
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import geography_replica  # noqa: E402

Image.MAX_IMAGE_PIXELS = None

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROVINCE_RASTER = os.path.join(ROOT, "map", "map_data", "atlas", "world_province.png")
WORLD_SDF = os.path.join(ROOT, "map", "map_data", "atlas", "world_sdf.png")
NAV = os.path.join(ROOT, "map", "map_data", "nav_mask.png")
PROVINCES = os.path.join(ROOT, "map", "map_data", "atlas", "provinces.json")
CITIES = os.path.join(ROOT, "data", "definitions", "scenario", "cities.json")
NATIONS = os.path.join(ROOT, "data", "definitions", "scenario", "nations.json")
URBAN_REF = os.path.join(ROOT, "data", "definitions", "population",
                         "urban_share_reference.json")
OUT = os.path.join(ROOT, "data", "definitions", "population", "hinterland_2026.json")

DEFAULT_TIF = os.path.join(ROOT, "tools", "data", "worldpop",
                           "global_pop_2026_CN_1km_R2025A_v1.tif")
SRC_URL = ("https://data.worldpop.org/GIS/Population/Global_2015_2030/R2025A/"
           "2026/0_Mosaicked/v1/1km/constrained/global_pop_2026_CN_1km_R2025A_v1.tif")
SOURCE = ("WorldPop Global2 R2025A v1 - global_pop_2026_CN_1km_R2025A_v1.tif "
          "(1 km, ano 2026, mosaico global 'constrained'; CC BY 4.0 - "
          "WorldPop, University of Southampton; DOI 10.5258/SOTON/WP00845)")
VERSION = "B3-hinterland-2026-v1"
RETRIEVED = "2026-11-17"
ID_SPACE = 256 * 256

## Nacoes conferidas na valida��o contra a urbaniza��o da ONU (WUP via Banco Mundial).
CHECK_NATIONS = ["CHN", "IND", "USA", "IDN", "PAK", "NGA", "BRA", "BGD", "RUS", "ETH"]


def load_json(path):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def atlas_province_ids():
    """Id de provincia por pixel de `world_province.png`.

    Devolve tambem a mascara de terra navegavel do `compute_province_areas.py`
    (`world_sdf.png` x `nav_mask.png`), usada SO como diagnostico: a atribuicao
    de populacao NAO a aplica. Ela corta ~1,8% da populacao mundial (centros
    urbanos costeiros, lagos e as ilhas removidas pela curadoria) -- a celula
    mora dentro da provincia desenhada, entao a populacao dela e da provincia.
    """
    raster = np.asarray(Image.open(PROVINCE_RASTER).convert("RGB"), dtype=np.int32)
    height, width = raster.shape[:2]
    ids = (raster[:, :, 0] + raster[:, :, 1] * 256).astype(np.int32)
    del raster
    land = np.asarray(Image.open(WORLD_SDF).convert("RGBA"))[:, :, 0] > 128
    nav = np.asarray(Image.open(NAV).convert("L")
                     .resize((width, height), Image.NEAREST)) >= 128
    keep = land & nav
    return ids, keep, width, height


def worldpop_grid(page):
    """Shape e georreferencia do TIFF (recusa qualquer coisa fora de WGS84)."""
    shape = page.shape
    if len(shape) != 2:
        raise SystemExit("esperava TIFF 2D de 1 banda; veio %r" % (shape,))
    height, width = int(shape[0]), int(shape[1])
    scale = tuple(float(v) for v in page.tags["ModelPixelScaleTag"].value)
    tie = tuple(float(v) for v in page.tags["ModelTiepointTag"].value)
    dx, dy = scale[0], scale[1]
    lon0 = tie[3] - tie[0] * dx
    lat0 = tie[4] + tie[1] * dy
    if abs(dx - 1.0 / 120.0) > 1e-9 or abs(dy - 1.0 / 120.0) > 1e-9:
        print("[aviso] passo do raster = %.9f x %.9f graus (esperado 1/120)"
              % (dx, dy))
    # O mosaico "constrained" da WorldPop cobre a faixa habitada (lat 84 -> -60
    # no R2025A), nao o globo inteiro: a origem e lida do arquivo, nao suposta.
    if abs(lon0 + 180.0) > 1e-6:
        raise SystemExit("origem inesperada: lon0=%.6f" % lon0)
    return height, width, lon0, lat0, dx, dy


def province_population(tif_path, ids, keep, atlas_w, atlas_h):
    """Soma a popula��o 1 km por provincia do atlas.

    Devolve (totais por id, soma global do raster, soma fora de provincia/mascara).
    """
    ids_flat = ids.ravel()
    keep_flat = keep.ravel()
    totals = np.zeros(ID_SPACE, dtype=np.float64)
    raster_total = 0.0
    outside = 0.0
    masked_out = 0.0
    with tifffile.TiffFile(tif_path) as handle:
        page = handle.pages[0]
        rows_wp, cols_wp, lon0, lat0, dx, dy = worldpop_grid(page)
        lon_center = lon0 + (np.arange(cols_wp, dtype=np.float64) + 0.5) * dx
        lat_center = lat0 - (np.arange(rows_wp, dtype=np.float64) + 0.5) * dy
        col_map = np.clip(((lon_center + 180.0) * atlas_w / 360.0)
                          .astype(np.int64), 0, atlas_w - 1)
        row_map = np.clip(((90.0 - lat_center) * atlas_h / 180.0)
                          .astype(np.int64), 0, atlas_h - 1)
        for segment in page.segments(maxworkers=1):
            data, indices = segment[0], segment[1]
            row0, col0 = int(indices[2]), int(indices[3])
            block = np.asarray(data, dtype=np.float64)[0, :, :, 0]
            rows = min(block.shape[0], row_map.size - row0)
            cols = min(block.shape[1], col_map.size - col0)
            block = np.ascontiguousarray(block[:rows, :cols])
            np.nan_to_num(block, copy=False, nan=0.0, posinf=0.0, neginf=0.0)
            # O mosaico marca nodata com -99999 (mar, regiao nao modelada):
            # populacao nao e negativa, entao tudo abaixo de zero vira zero.
            np.maximum(block, 0.0, out=block)
            raster_total += float(block.sum())
            flat = (row_map[row0:row0 + rows][:, None] * atlas_w
                    + col_map[col0:col0 + cols][None, :])
            pid = ids_flat[flat]
            selected = (pid > 0) & (pid < ID_SPACE)
            if selected.any():
                totals += np.bincount(pid[selected], weights=block[selected],
                                      minlength=ID_SPACE)
                # diagnostico: quanto a mascara de area/nav do atlas cortaria
                masked = selected & ~keep_flat[flat]
                masked_out += float(block[masked].sum())
            outside += float(block[~selected].sum())
            del flat, pid, selected
    return {int(pid): float(totals[pid]) for pid in np.nonzero(totals)[0]}, \
        raster_total, outside, masked_out, \
        (rows_wp, cols_wp, lat0, lat0 - rows_wp * dy)


def apportion(values, total):
    """Arredonda `values` para inteiros mantendo `sum == total` (maior resto).

    Empate de resto desempatado pela chave, entao e deterministico.
    """
    keys = list(values)
    if not keys:
        return {}
    base = {key: int(math.floor(values[key])) for key in keys}
    rest = int(total) - sum(base.values())
    # Guarda contra entrada absurda (fator errado, unidade trocada): sem isso o
    # laco abaixo distribuiria bilhoes de unidades e pareceria travar.
    if abs(rest) > 2 * len(keys) + 1000:
        raise SystemExit("apportion: resto %d em %d cidades (valores %.1f..%.1f, total %d)"
                         % (rest, len(keys), min(values.values()),
                            max(values.values()), total))
    if rest > 0:
        order = sorted(keys, key=lambda key: (-(values[key] - base[key]), key))
        for index in range(rest):
            base[order[index % len(order)]] += 1
    elif rest < 0:
        order = sorted(keys, key=lambda key: ((values[key] - base[key]), key))
        index = 0
        while rest < 0 and index < 100 * len(order):
            key = order[index % len(order)]
            if base[key] > 0:
                base[key] -= 1
                rest += 1
            index += 1
    return base


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--tif", default=DEFAULT_TIF, help="mosaico WorldPop 1 km")
    args = parser.parse_args()
    if not os.path.isfile(args.tif):
        raise SystemExit("TIFF nao encontrado: %s (ver %s)" % (args.tif, SRC_URL))

    checksum = sha256_file(args.tif)
    print("fonte: %s" % SOURCE)
    print("arquivo: %s  sha256=%s" % (os.path.relpath(args.tif, ROOT), checksum))

    ids, keep, atlas_w, atlas_h = atlas_province_ids()
    pops, raster_total, outside, masked_out, wp_shape = province_population(
        args.tif, ids, keep, atlas_w, atlas_h)
    del ids, keep

    provinces = load_json(PROVINCES)["provinces"]
    cities_raw = load_json(CITIES)
    nations_raw = load_json(NATIONS)
    playable = {str(nation["id"]) for nation in nations_raw}
    target = {str(nation["id"]): float(nation.get("population") or 0.0)
              for nation in nations_raw}

    catchment, share = geography_replica.assign_provinces(
        cities_raw, provinces, playable)

    # --- populacao raster por nacao ----------------------------------------
    tag_of = geography_replica.province_tag_map(playable)
    atlas_to_game = geography_replica.game_tag_map(playable)
    raster_by_nation = {tag: 0.0 for tag in playable}
    unassigned_population = 0.0
    provinces_without_pixel = 0
    for entry in provinces:
        pid = int(entry["id"])
        value = pops.get(pid, 0.0)
        game_tag = atlas_to_game.get(str(entry.get("nation") or ""), "")
        if game_tag == "":
            continue
        raster_by_nation[game_tag] += value
        if value <= 0.0:
            provinces_without_pixel += 1

    factor = {}
    for tag in playable:
        raster_pop = raster_by_nation[tag]
        if raster_pop > 0.0:
            factor[tag] = target[tag] / raster_pop
        else:
            factor[tag] = 1.0

    # --- populacao bruta por cidade e apportionamento por nacao -------------
    raw_region = {}
    for city in cities_raw:
        city_id = str(city["id"])
        raw_region[city_id] = geography_replica.catchment_population(
            catchment, share, pops, city_id) * factor.get(str(city["nation"]), 1.0)

    region = {}
    by_nation = {}
    urban_by_city = {str(city["id"]): float(city.get("urban_population") or 0.0)
                     for city in cities_raw}
    for city in cities_raw:
        by_nation.setdefault(str(city["nation"]), []).append(str(city["id"]))
    fallback_nations = {}
    for tag, city_ids in by_nation.items():
        scaled = {city_id: raw_region.get(city_id, 0.0) for city_id in city_ids}
        if sum(scaled.values()) <= 0.0:
            # Nacao sem NENHUMA populacao no raster (nao modelada pela WorldPop
            # ou sem provincia com pixel no atlas): o total nacional entra pelo
            # peso da aglomeracao, declarado como derivado -- nunca fica zero
            # em silencio.
            weights = {city_id: urban_by_city.get(city_id, 0.0)
                       for city_id in city_ids}
            if sum(weights.values()) <= 0.0:
                weights = {city_id: 1.0 for city_id in city_ids}
            total_weight = sum(weights.values())
            scaled = {city_id: target.get(tag, 0.0) * weights[city_id] / total_weight
                      for city_id in city_ids}
            fallback_nations[tag] = "urban_proportional"
        region.update(apportion(scaled, round(target.get(tag, 0.0))))

    # --- saida por cidade ---------------------------------------------------
    cities_out = {}
    negatives = []
    urban_sources = {}
    for city in cities_raw:
        city_id = str(city["id"])
        urban = int(round(float(city.get("urban_population") or 0.0)))
        region_pop = int(region.get(city_id, 0))
        hinterland = region_pop - urban
        if hinterland < 0:
            negatives.append((city_id, region_pop, urban))
            hinterland = 0
        cities_out[city_id] = {
            "population_region": region_pop,
            "urban_population": urban,
            "hinterland_population": hinterland,
            "provinces_count": len(catchment.get(city_id, [])),
            "region_raw": round(raw_region.get(city_id, 0.0), 1),
            "urban_population_source": str(city.get("urban_population_source") or ""),
        }
        urban_sources[str(city.get("urban_population_source") or "")] = \
            urban_sources.get(str(city.get("urban_population_source") or ""), 0) + urban

    nations_out = {}
    for tag in sorted(playable):
        entry = {
            "factor": round(factor[tag], 6),
            "raster_population": round(raster_by_nation[tag], 1),
            "target_population": int(round(target[tag])),
        }
        if tag in fallback_nations:
            entry["fallback"] = fallback_nations[tag]
        nations_out[tag] = entry

    method = (
        "populacao das celulas 1 km do mosaico WorldPop 2026 somada por "
        "provincia do atlas (world_province.png 8192x4096, id=R+G*256; toda "
        "celula cujo pixel tem id de provincia entra na provincia, inclusive a "
        "faixa costeira que a mascara area/nav do compute_province_areas.py "
        "descartaria -- ela cortaria 1,8% da populacao mundial); o centro de "
        "cada celula de 30 arc-seg cai no pixel do atlas por floor "
        "((lon+180)*8192/360, (90-lat)*4096/180), sem interpolar; provincia -> "
        "cidade dona pela replica de GeographyService.assign_provinces "
        "(tools/geography_replica.py, inclui _share_starved_provinces); "
        "population_region = soma das provincias x fator da nacao, apportionado "
        "em inteiros por maior resto; hinterland_population = "
        "population_region - urban_population (piso 0)"
    )
    out = {
        "version": VERSION,
        "source": SOURCE,
        "url": SRC_URL,
        "sha256": checksum,
        "retrieved": RETRIEVED,
        "license": "CC BY 4.0 (WorldPop, University of Southampton)",
        "method": method,
        "nations": nations_out,
        "cities": {city_id: cities_out[city_id] for city_id in sorted(cities_out)},
    }
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(out, handle, ensure_ascii=False, indent=1, sort_keys=True)
        handle.write("\n")
    out_hash = hashlib.sha256(open(OUT, "rb").read()).hexdigest()

    # --- validacao ----------------------------------------------------------
    total_region = sum(entry["population_region"] for entry in cities_out.values())
    total_hinterland = sum(e["hinterland_population"] for e in cities_out.values())
    total_urban = sum(e["urban_population"] for e in cities_out.values())
    print("\n== raster ==")
    print("grade WorldPop %dx%d (1/120 grau, lat %.0f..%.0f) -> atlas %dx%d"
          % (wp_shape[1], wp_shape[0], wp_shape[2], wp_shape[3], atlas_w, atlas_h))
    print("soma do raster (todas as celulas) = %.0f" % raster_total)
    print("populacao em provincia do atlas     = %.0f" % sum(pops.values()))
    print("fora de provincia (id 0)            = %.0f" % outside)
    print("que a mascara area/nav cortaria     = %.0f (nao cortada)" % masked_out)
    print("provincias jogaveis sem pixel/pop   = %d" % provinces_without_pixel)
    non_playable_pop = sum(
        pops.get(int(entry["id"]), 0.0) for entry in provinces
        if atlas_to_game.get(str(entry.get("nation") or ""), "") == "")
    print("populacao em provincia nao jogavel  = %.0f" % non_playable_pop)

    print("\n== totais ==")
    print("regiao somada (todas as cidades)    = %d" % total_region)
    print("nations.json somado                 = %d" % round(sum(target.values())))
    print("urbano (cities.json)                = %d" % total_urban)
    print("interior                            = %d" % total_hinterland)
    print("urbanizacao resultante              = %.1f%%"
          % (100.0 * total_urban / max(1, total_region)))
    print("fontes de urban_population          = %s" % urban_sources)
    factors_tag = [tag for tag in playable if raster_by_nation[tag] > 0.0]
    factors = [factor[tag] for tag in factors_tag]
    print("fator por nacao: min %.3f  max %.3f  |f-1|>5%%: %d de %d"
          % (min(factors), max(factors),
             sum(1 for value in factors if abs(value - 1.0) > 0.05), len(factors)))
    print("nacoes sem raster (fallback pela aglomeracao): %s"
          % (", ".join(sorted(fallback_nations)) or "nenhuma"))
    outliers = sorted((abs(factor[tag] - 1.0), tag) for tag in factors_tag)
    print("\n== nacoes com |fator-1| > 5% (15 piores) ==")
    for delta, tag in sorted(outliers, reverse=True)[:15]:
        print("  %-4s fator=%.3f  raster=%12.0f  nations.json=%12d"
              % (tag, factor[tag], raster_by_nation[tag], round(target[tag])))
    # invariante: a soma das cidades de cada nacao e exatamente o total nacional
    bad = []
    for tag, city_ids in by_nation.items():
        total = sum(region.get(city_id, 0) for city_id in city_ids)
        if total != int(round(target.get(tag, 0.0))):
            bad.append((tag, total, int(round(target.get(tag, 0.0)))))
    print("nacoes cuja soma de cidades != nations.json: %d" % len(bad))
    for row in bad[:10]:
        print("  %-4s soma=%d alvo=%d" % row)

    if os.path.isfile(URBAN_REF):
        ref = load_json(URBAN_REF)
        rows = ref.get("nations", {})
        print("\n== 10 nacoes vs urbanizacao ONU (WUP via Banco Mundial) ==")
        print("%-4s %13s %13s %6s %8s %8s %8s"
              % ("ISO3", "regiao", "urbano", "%urb", "ref%", "ano", "desvio"))
        for tag in CHECK_NATIONS:
            ids_nation = [city_id for city_id in by_nation.get(tag, [])]
            region_nation = sum(region.get(city_id, 0) for city_id in ids_nation)
            urban_nation = sum(cities_out[city_id]["urban_population"]
                               for city_id in ids_nation)
            share_pct = 100.0 * urban_nation / region_nation if region_nation else 0.0
            reference = rows.get(tag, {})
            ref_pct = float(reference.get("urban_share", 0.0)) * 100.0
            print("%-4s %13d %13d %6.1f %8.1f %8s %+7.1f"
                  % (tag, region_nation, urban_nation, share_pct, ref_pct,
                     reference.get("year", "?"), share_pct - ref_pct))

    top = sorted(cities_out.items(), key=lambda item: -item[1]["hinterland_population"])
    print("\n== top 20 cidades por interior ==")
    name_of = {str(city["id"]): city.get("name", "") for city in cities_raw}
    for city_id, entry in top[:20]:
        print("%-12s %-26s regiao=%12d urbano=%12d interior=%12d"
              % (city_id, name_of.get(city_id, "")[:26], entry["population_region"],
                 entry["urban_population"], entry["hinterland_population"]))
    zero = [city_id for city_id, e in cities_out.items() if e["hinterland_population"] == 0]
    print("\ninterior zero: %d cidades  |  regiao < urbano: %d"
          % (len(zero), len(negatives)))
    for row in negatives[:20]:
        print("  NEGATIVO %-12s regiao=%d urbano=%d" % row)
    print("\n-> %s  sha256=%s" % (os.path.relpath(OUT, ROOT), out_hash))
    return 0


if __name__ == "__main__":
    sys.exit(main())
