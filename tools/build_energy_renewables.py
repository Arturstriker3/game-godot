"""Potencial solar, eólico e hídrico por província e por cidade (Parte E, E1).

Solar e eólico — DADO MEDIDO:
  WorldClim 2.1, 10 arc-min, mensal (12 meses):
    wc2.1_10m_srad_01..12.tif  radiação solar (kJ m-2 dia-1)
    wc2.1_10m_wind_01..12.tif  velocidade do vento (m/s)
  Amostragem no centróide da província (mesma convenção de
  data/definitions/natural/province_nature.json). Centróide em oceano/nodata:
  usa o pixel de terra mais próximo (varredura em anéis determinística) e
  registra o deslocamento. Cidade = média ponderada pela área da província
  (área do raster x fração da província que pertence à cidade).

Hídrico — PROXY DECLARADO (method="proxy_precip_relief_river"):
  por província,
    raw = (precip_mm/1000) * area_km2 * (relief_m/1000) * runoff * river_factor
  onde
    precip_mm  = precipitação anual de province_nature.json (WorldClim bio_12);
    relief_m   = max-min da altitude (WorldClim 2.1 elev) nos pixels de terra da
                 província na grade 4096x2048 do atlas;
    runoff     = coeficiente de escoamento tipo Budyko simples:
                 clamp((precip - PET)/precip, 0, 1), PET = 25*max(T,0)+200 mm/ano
                 (ordem de grandeza de Thornthwaite) — evita que deserto grande
                 apareça como grande potencial só por causa da área;
    river_factor = gate de presença de rio: cobertura = fração dos pixels de terra
                 da província a <= 50 km de um rio do Natural Earth 10m
                 (rivers_lake_centerlines, TODOS os scaleranks — o corte <= 4 do
                 desenho do mapa deixa a Noruega sem rio nenhum);
                 fator = clamp(cobertura/1.0, 0.02, 1.0).

  NORMALIZAÇÃO: o total mundial do proxy é ancorado em 16.400 TWh/ano de
  potencial técnico hidrelétrico mundial (IJHD, "Hydropower Essentials", citado
  em OSTI/ETDEWEB 21594387: "overall technical potential ... more than
  16 400 TWh/yr"). A distribuição ENTRE nações é do proxy: não existe dataset
  público legível por máquina, país a país, de potencial técnico hidrelétrico
  (IHA/World Energy Council publicam números de relatório em PDF). Por isso
  cada registro leva normalized="global_anchor", confidence="low" e
  national_anchor_missing=true; nenhum total nacional foi inventado.

Área das províncias: contada no raster do atlas (world_province + world_sdf),
não em province_areas.json — que tem células degeneradas (ex.: "Tasmania 1"
com 1,78 milhão de km²).

Uso: python tools/build_energy_renewables.py
Saída: data/definitions/energy/renewable_potential.json
"""

from __future__ import annotations

import math
import os
import sys

import numpy as np
import shapefile
import tifffile
from PIL import Image, ImageDraw
from scipy import ndimage

import build_energy_common as ec

ROOT = ec.ROOT
WC = os.path.join(ROOT, "tools", "data", "worldclim")
NE_DIR = os.environ.get("NE_DIR", r"G:\tools\ne_extract")
RIVERS = os.path.join(NE_DIR, "ne_10m_rivers_lake_centerlines",
                      "ne_10m_rivers_lake_centerlines")
ELEV = os.path.join(WC, "wc2.1_10m_elev.tif")
OUT = os.path.join(ROOT, "data", "definitions", "energy", "renewable_potential.json")

CELL = 1.0 / 6.0
SRAD_NODATA = 65535
WIND_NODATA = -1.0e30
ELEV_NODATA = -32768
RIVER_COVER_KM = 50.0
RIVER_COVER_TARGET = 1.0
RIVER_FACTOR_FLOOR = 0.02
GLOBAL_HYDRO_TWH_YEAR = 16400.0
DAYS_IN_MONTH = [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
MAX_SNAP_PX = 40

# Grade de análise do relevo/rios: 4096x2048 sobre o mundo. O pixel tem 0,0879
# em x e em y (quadrado em graus), o que torna a distância euclidiana direta.
GRID_W = 4096
GRID_H = 2048


def load_monthly(prefix):
    stack = []
    for month in range(1, 13):
        path = os.path.join(WC, "wc2.1_10m_%s_%02d.tif" % (prefix, month))
        if not os.path.exists(path):
            raise SystemExit("raster ausente: %s (rode tools/fetch_worldclim_energy.py)" % path)
        stack.append(tifffile.imread(path))
    return np.stack(stack)


def is_nodata(value, nodata):
    # vento: nodata é -3.4e38 (float32); valor normal é 0..30 m/s.
    if nodata == WIND_NODATA:
        return value < -1.0e30
    return value >= nodata


def sample_monthly(stack, lon, lat, nodata):
    """Série mensal no centróide; se nodata, usa o pixel válido mais próximo."""
    height, width = stack.shape[1], stack.shape[2]
    col = int(round((lon + 180.0) / CELL))
    row = int(round((90.0 - lat) / CELL))
    col = max(0, min(width - 1, col))
    row = max(0, min(height - 1, row))
    series = stack[:, row, col].astype(np.float64)
    if not any(is_nodata(v, nodata) for v in series):
        return series.tolist(), 0
    for radius in range(1, MAX_SNAP_PX + 1):
        best = None
        for dy in range(-radius, radius + 1):
            for dx in range(-radius, radius + 1):
                if max(abs(dx), abs(dy)) != radius:
                    continue
                r = row + dy
                c = col + dx
                if r < 0 or c < 0 or r >= height or c >= width:
                    continue
                candidate = stack[:, r, c].astype(np.float64)
                if any(is_nodata(v, nodata) for v in candidate):
                    continue
                if best is None:
                    best = candidate.tolist()
        if best is not None:
            return best, radius
    return None, None


def build_analysis_grid():
    """Elevação e distância aos rios na grade 4096x2048 (pixels quadrados)."""
    elev = tifffile.imread(ELEV).astype(np.float32)
    elev[elev <= ELEV_NODATA + 1] = np.nan
    rows = np.minimum((np.arange(GRID_H) + 0.5) / GRID_H * elev.shape[0],
                      elev.shape[0] - 1).astype(np.int32)
    cols = np.minimum((np.arange(GRID_W) + 0.5) / GRID_W * elev.shape[1],
                      elev.shape[1] - 1).astype(np.int32)
    elev_grid = elev[np.ix_(rows, cols)]

    river_mask = Image.new("L", (GRID_W, GRID_H), 0)
    draw = ImageDraw.Draw(river_mask)
    reader = shapefile.Reader(RIVERS)
    drawn = 0
    for shape_record in reader.iterShapeRecords():
        points = shape_record.shape.points
        if len(points) < 2:
            continue
        pixel = [((lon + 180.0) / 360.0 * (GRID_W - 1),
                  (90.0 - lat) / 180.0 * (GRID_H - 1)) for lon, lat in points]
        draw.line(pixel, fill=255, width=1)
        drawn += 1
    mask = np.asarray(river_mask) > 0
    distance_px = ndimage.distance_transform_edt(~mask)
    return elev_grid, distance_px, drawn


def main():
    cities = ec.load_cities()
    provinces = ec.load_provinces()
    areas = ec.raster_province_area_km2()
    nature = ec.read_json(ec.PROVINCE_NATURE_PATH).get("provinces", {})
    city_prov = ec.city_provinces(cities, provinces)

    print("carregando WorldClim mensal...")
    srad = load_monthly("srad")
    wind = load_monthly("wind")
    print("grade de relevo/rios...")
    elev_grid, river_distance_px, rivers_drawn = build_analysis_grid()

    ids = ec.load_world_province_ids()[::2, ::2]
    land = ec.load_world_land_mask()[::2, ::2]
    keep = np.isin(ids, np.array([p["id"] for p in provinces], dtype=np.int32))
    valid = keep & land
    flat_ids = ids[valid]
    elev_values = elev_grid[valid]
    # distância em graus; vira km com o cosseno da latitude da linha.
    river_deg = river_distance_px[valid] * (360.0 / GRID_W)
    rows = np.arange(GRID_H, dtype=np.float64)
    cos_lat = np.cos(np.deg2rad(90.0 - (rows + 0.5) / GRID_H * 180.0))
    cos_2d = np.broadcast_to(cos_lat[:, None], (GRID_H, GRID_W))
    river_km = river_deg * 111.32 * cos_2d[valid]
    del cos_2d, elev_grid, river_distance_px

    max_id = max(p["id"] for p in provinces)
    has_elev = ~np.isnan(elev_values)
    elev_flat = np.where(has_elev, elev_values, -1e9)
    relief_max = ndimage.maximum(elev_flat, labels=flat_ids, index=np.arange(0, max_id + 1))
    elev_flat_min = np.where(has_elev, elev_values, 1e9)
    relief_min = ndimage.minimum(elev_flat_min, labels=flat_ids, index=np.arange(0, max_id + 1))
    counts = np.bincount(flat_ids, minlength=max_id + 1)
    river_cover = np.bincount(flat_ids,
                              weights=(river_km <= RIVER_COVER_KM).astype(np.float64),
                              minlength=max_id + 1)
    del flat_ids, elev_values, river_km, has_elev, elev_flat, elev_flat_min, ids, land, keep, valid

    province_out = {}
    raw_total = 0.0
    missing_climate = []
    for province in provinces:
        pid = province["id"]
        lon, lat = ec.map_to_lonlat(province["x"], province["y"])
        srad_series, srad_shift = sample_monthly(srad, lon, lat, SRAD_NODATA)
        wind_series, wind_shift = sample_monthly(wind, lon, lat, WIND_NODATA)
        n = counts[pid] if pid < counts.size else 0
        relief = None
        if n > 0 and relief_max[pid] > -1e8 and relief_min[pid] < 1e8:
            relief = float(relief_max[pid] - relief_min[pid])
        river_factor = None
        if n > 0:
            coverage = float(river_cover[pid] / n)
            river_factor = max(RIVER_FACTOR_FLOOR,
                               min(1.0, coverage / RIVER_COVER_TARGET))
        entry = {
            "province_id": pid,
            "nation": province["nation"],
            "area_km2": round(areas.get(pid, 0.0), 1),
            "solar": None,
            "wind": None,
            "hydro": None,
        }
        if srad_series is None or wind_series is None:
            missing_climate.append(pid)
        else:
            solar_month = [round(v / 3600.0, 2) for v in srad_series]
            wind_month = [round(v, 2) for v in wind_series]
            solar_year = sum(v * DAYS_IN_MONTH[i] for i, v in enumerate(solar_month))
            entry["solar"] = {
                "kwh_m2_day_mean": round(sum(solar_month) / 12.0, 2),
                "kwh_m2_year": round(solar_year, 1),
                "min_month": int(np.argmin(solar_month)) + 1,
                "min_month_kwh_m2_day": min(solar_month),
                "max_month": int(np.argmax(solar_month)) + 1,
                "max_month_kwh_m2_day": max(solar_month),
                "monthly_kwh_m2_day": solar_month,
                "source": "WorldClim 2.1 srad (10-min, mensal)",
                "sample_offset_px": srad_shift,
            }
            entry["wind"] = {
                "m_s_mean": round(sum(wind_month) / 12.0, 2),
                "min_month": int(np.argmin(wind_month)) + 1,
                "min_month_m_s": min(wind_month),
                "max_month": int(np.argmax(wind_month)) + 1,
                "max_month_m_s": max(wind_month),
                "monthly_m_s": wind_month,
                "source": "WorldClim 2.1 wind (10-min, mensal)",
                "sample_offset_px": wind_shift,
            }
        climate = nature.get(str(pid), {})
        precip = float(climate.get("precip_mm", 0.0))
        temp = float(climate.get("temp_c", 0.0))
        # Coeficiente de escoamento tipo Budyko simples: excesso de chuva sobre a
        # evapotranspiração potencial (PET ~ 25*T + 200 mm/ano, ordem de grandeza
        # de Thornthwaite). Deserto grande deixa de pontuar só pela área.
        pet = 25.0 * max(temp, 0.0) + 200.0
        runoff = min(1.0, max(0.0, (precip - pet) / precip)) if precip > 0.0 else 0.0
        area = areas.get(pid, 0.0)
        if precip > 0.0 and area > 0.0 and relief is not None and river_factor is not None:
            raw = (precip / 1000.0) * area * (relief / 1000.0) * runoff * river_factor
            raw_total += raw
            entry["hydro"] = {
                "precip_mm": round(precip, 0),
                "relief_m": round(relief, 0),
                "runoff_factor": round(runoff, 3),
                "river_factor": round(river_factor, 4),
                "raw_index": round(raw, 3),
                "method": "proxy_precip_relief_river",
                "confidence": "low",
                "normalized": "global_anchor",
                "national_anchor_missing": True,
            }
        else:
            entry["hydro"] = {
                "precip_mm": round(precip, 0),
                "relief_m": None if relief is None else round(relief, 0),
                "runoff_factor": round(runoff, 3),
                "river_factor": None if river_factor is None else round(river_factor, 4),
                "raw_index": 0.0,
                "method": "proxy_precip_relief_river",
                "confidence": "no_data",
                "normalized": "global_anchor",
                "national_anchor_missing": True,
                "no_data_reason": "sem precipitação/área/relevo para a província",
            }
        province_out[str(pid)] = entry

    scale = GLOBAL_HYDRO_TWH_YEAR / raw_total if raw_total > 0 else 0.0
    nation_hydro = {}
    for entry in province_out.values():
        hydro = entry["hydro"]
        hydro["twh_per_year"] = round(hydro["raw_index"] * scale, 4)
        nation = entry["nation"]
        nation_hydro[nation] = nation_hydro.get(nation, 0.0) + hydro["twh_per_year"]
    for entry in province_out.values():
        entry["hydro"]["nation_twh_per_year"] = round(
            nation_hydro.get(entry["nation"], 0.0), 2)

    city_out = {}
    for city in cities:
        links = city_prov.get(city["id"], [])
        weight = 0.0
        solar_month = [0.0] * 12
        wind_month = [0.0] * 12
        hydro_raw = 0.0
        covered = 0
        for pid, share in links:
            entry = province_out.get(str(pid))
            if not entry or entry["solar"] is None:
                continue
            w = entry["area_km2"] * share
            weight += w
            for i in range(12):
                solar_month[i] += entry["solar"]["monthly_kwh_m2_day"][i] * w
                wind_month[i] += entry["wind"]["monthly_m_s"][i] * w
            hydro_raw += entry["hydro"]["raw_index"] * share
            covered += 1
        if weight <= 0.0:
            city_out[city["id"]] = {"city_id": city["id"], "nation": city["tag"],
                                    "solar": None, "wind": None, "hydro": None,
                                    "provinces": len(links), "provinces_with_data": 0}
            continue
        solar_month = [v / weight for v in solar_month]
        wind_month = [v / weight for v in wind_month]
        city_out[city["id"]] = {
            "city_id": city["id"],
            "nation": city["tag"],
            "area_km2": round(weight, 1),
            "provinces": len(links),
            "provinces_with_data": covered,
            "solar": {
                "kwh_m2_day_mean": round(sum(solar_month) / 12.0, 2),
                "kwh_m2_year": round(sum(v * DAYS_IN_MONTH[i]
                                         for i, v in enumerate(solar_month)), 1),
                "min_month": int(np.argmin(solar_month)) + 1,
                "max_month": int(np.argmax(solar_month)) + 1,
                "monthly_kwh_m2_day": [round(v, 2) for v in solar_month],
            },
            "wind": {
                "m_s_mean": round(sum(wind_month) / 12.0, 2),
                "min_month": int(np.argmin(wind_month)) + 1,
                "max_month": int(np.argmax(wind_month)) + 1,
                "monthly_m_s": [round(v, 2) for v in wind_month],
            },
            "hydro": {
                "twh_per_year": round(hydro_raw * scale, 4),
                "method": "proxy_precip_relief_river",
                "confidence": "low",
                "normalized": "global_anchor",
                "national_anchor_missing": True,
            },
        }

    nation_out = {}
    for tag, twh in nation_hydro.items():
        nation_out[tag] = {"hydro_twh_per_year": round(twh, 3)}
    for entry in city_out.values():
        if entry["solar"] is None:
            continue
        # Solar/eólico nacional = média das cidades ponderada pela área da bacia.
        agg = nation_out.setdefault(entry["nation"], {"hydro_twh_per_year": 0.0})
        agg["_solar_w"] = agg.get("_solar_w", 0.0) + entry["solar"]["kwh_m2_day_mean"] * entry["area_km2"]
        agg["_solar_wa"] = agg.get("_solar_wa", 0.0) + entry["area_km2"]
        agg["_wind_w"] = agg.get("_wind_w", 0.0) + entry["wind"]["m_s_mean"] * entry["area_km2"]
        agg["_wind_wa"] = agg.get("_wind_wa", 0.0) + entry["area_km2"]
    for agg in nation_out.values():
        solar_area = agg.pop("_solar_wa", 0.0)
        wind_area = agg.pop("_wind_wa", 0.0)
        agg["solar_kwh_m2_day_mean"] = (round(agg.pop("_solar_w") / solar_area, 2)
                                        if solar_area > 0 else None)
        agg["wind_m_s_mean"] = (round(agg.pop("_wind_w") / wind_area, 2)
                                if wind_area > 0 else None)

    # As séries mensais por província já foram agregadas nas cidades: não vão
    # para o arquivo (o consumidor da simulação é a cidade).
    for entry in province_out.values():
        if entry["solar"]:
            entry["solar"].pop("monthly_kwh_m2_day", None)
        if entry["wind"]:
            entry["wind"].pop("monthly_m_s", None)

    meta = {
        "generated_by": "tools/build_energy_renewables.py",
        "plan": "docs/plans/perf_pop_financas_plano.md, PARTE E, E1",
        "solar_wind": {
            "source": "WorldClim 2.1 (10 arc-min) srad e wind, 12 meses",
            "url": "https://geodata.ucdavis.edu/climate/worldclim/2_1/base/",
            "citation": "Fick, S.E. & Hijmans, R.J. (2017) WorldClim 2, Int. J. Climatol. 37:4302-4315",
            "license": "não declarada explicitamente na página oficial do WorldClim (uso acadêmico/atribuição)",
            "units": {"srad": "kJ m-2 dia-1 convertido para kWh m-2 dia-1 (/3600)",
                      "wind": "m/s a 10 m"},
            "sampling": ("centróide da província (nearest pixel), mesma convenção de "
                         "province_nature.json; centróide em oceano -> pixel de terra mais "
                         "próximo em anéis, deslocamento em sample_offset_px"),
            "aggregation": "cidade = média ponderada por área_km2 x fração da província na cidade",
            "area_source": ("área contada no raster do atlas (world_province + world_sdf, "
                            "8192x4096, cosseno da latitude); province_areas.json foi "
                            "descartado na agregação por ter células degeneradas "
                            "(ex.: 'Tasmania 1' com 1,78 milhão de km²)"),
            "confidence": "high (climatologia medida; resolução 10 arc-min é grossa para sítio; WorldClim wind suaviza o relevo e subestima vento local)",
        },
        "hydro": {
            "method": "proxy_precip_relief_river",
            "formula": ("raw = precip_mm/1000 * area_km2 * relief_m/1000 * runoff * "
                        "river_factor"),
            "relief": ("max-min da altitude (WorldClim 2.1 elev) nos pixels de terra da "
                       "província, na grade 4096x2048 do atlas"),
            "runoff": ("coeficiente tipo Budyko: clamp((precip - PET)/precip, 0, 1) com "
                       "PET = 25*max(T,0)+200 mm/ano (ordem de grandeza de Thornthwaite)"),
            "river_factor": ("gate de presença de rio: cobertura = fração dos pixels de "
                             "terra da província a <= 50 km de um rio do Natural Earth 10m "
                             "(rivers_lake_centerlines, todos os scaleranks); "
                             "fator = clamp(cobertura, 0.02, 1.0)"),
            "normalization": {
                "anchor": "potencial técnico hidrelétrico mundial",
                "value_twh_year": GLOBAL_HYDRO_TWH_YEAR,
                "source": ("IJHD, Hydropower Essentials (OSTI/ETDEWEB 21594387): "
                           "'overall technical potential ... more than 16 400 TWh/yr'"),
                "scope": "global",
                "why_not_national": ("não há dataset público legível por máquina de "
                                     "potencial técnico hidrelétrico por país; IHA/World "
                                     "Energy Council publicam apenas números de relatório "
                                     "em PDF. A divisão entre nações é do proxy; nenhum "
                                     "total nacional foi inventado "
                                     "(national_anchor_missing=true)."),
            },
            "confidence": "low",
            "no_data_note": ("províncias sem precipitação/área/relevo ficam com "
                             "confidence='no_data', raw_index=0, no_data_reason e "
                             "0,0 TWh — ver counts.provinces_no_hydro_data"),
        },
        "counts": {
            "provinces": len(province_out),
            "cities": len(city_out),
            "nations": len(nation_out),
            "rivers_drawn": rivers_drawn,
            "provinces_without_climate_sample": len(missing_climate),
            "provinces_no_hydro_data": sum(
                1 for entry in province_out.values()
                if entry["hydro"]["confidence"] == "no_data"),
            "raw_index_total": round(raw_total, 1),
            "hydro_scale_twh_per_raw_index": float("%.12g" % scale),
        },
    }
    payload = {"meta": meta, "by_nation": nation_out, "by_city": city_out,
               "by_province": province_out}
    text = ['{', ' "meta": %s,' % _compact(meta),
            ' "by_nation": %s,' % _compact(nation_out),
            ' "by_city": %s,' % _compact(city_out),
            ' "by_province": [']
    lines = ["  " + _compact({"id": int(k), **v}) for k, v in province_out.items()]
    text.append(",\n".join(lines))
    text.append(" ]")
    text.append("}")
    ec.write_text(OUT, "\n".join(text) + "\n")

    top = sorted(nation_out.items(), key=lambda kv: kv[1]["hydro_twh_per_year"],
                 reverse=True)[:8]
    print("províncias: %d | cidades: %d | nações: %d"
          % (len(province_out), len(city_out), len(nation_out)))
    print("rios rasterizados: %d | províncias sem amostra de clima: %d"
          % (rivers_drawn, len(missing_climate)))
    print("escala hidro: %.6g TWh/índice | total mundial %.1f TWh/ano"
          % (scale, raw_total * scale))
    print("top hidro (proxy, global anchor):")
    for tag, entry in top:
        print("  %-4s %8.1f TWh/ano" % (tag, entry["hydro_twh_per_year"]))
    return 0


def _compact(obj):
    import json
    return json.dumps(obj, ensure_ascii=False, separators=(",", ":"))


if __name__ == "__main__":
    sys.exit(main())
