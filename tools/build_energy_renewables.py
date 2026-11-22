"""Potencial solar, eólico e hídrico por província e por cidade (Parte E, E1).

SOLAR/EÓLICO — DADO MEDIDO (D-115c):
  * Global Solar Atlas 2.x (Solargis/ESMAP/Banco Mundial, CC BY 4.0):
    GHI e PVOUT anuais + série mensal, no centróide da província;
  * Global Wind Atlas 3/4 (DTU/ESMAP/Banco Mundial, CC BY 4.0): clima
    generalizado de Weibull a 100 m, no centróide da província. A média e a
    densidade de potência usam a classe de rugosidade 0,100 m (campo aberto/
    pastagem), declarada; a densidade é corrigida pela densidade do ar na
    altitude média da província (ETOPO1).
  O GWA NÃO publica série mensal: o vento fica com média anual e
  `monthly_available: false` (lacuna declarada, sem inventar sazonalidade).
  Cidade = média ponderada pela área da província na cidade.

HÍDRICO — PROXY DECLARADO (method="proxy_precip_relief_river_free"):
  por província,
    raw = (precip_mm/1000) * area_km2 * (relief_m/1000) * runoff * river_factor
  onde
    precip_mm  = CHIRPS v3.0 anual 2025 (domínio público/CC BY 4.0), 0,05°;
    relief_m   = max-min da altitude ETOPO1 (domínio público) nos pixels de
                 terra da província na grade 4096x2048 do atlas;
    runoff     = clamp((precip - PET)/precip, 0, 1), PET anual da Global
                 Aridity and PET Database v3.1 (CC BY 4.0);
    river_factor = cobertura de rio Natural Earth 10m a <= 50 km, piso 0,02.
  NÃO usa WorldClim. CHIRPS cobre 60°N-60°S: províncias fora dessa faixa ficam
  `no_data` (lacuna declarada), sem preencher com outra fonte restrita.
  NORMALIZAÇÃO GLOBAL: o total mundial do proxy é ancorado em 16.400 TWh/ano
  (IJHD), com confidence="low"; a normalização nacional do E1 é feita em
  build_hydro_national.py com a âncora do Banco Mundial.

Área das províncias: contada no raster do atlas (world_province + world_sdf),
não em province_areas.json — que tem células degeneradas.

NE_DIR: pasta com `ne_10m_rivers_lake_centerlines` (Natural Earth 10m). Lida
de `NE_DIR`; se ausente, tenta `tools/data/natural_earth` e o caminho legado
`G:/tools/ne_extract`. Se nenhum existir, falha com instrução (nada de caminho
absoluto silencioso).

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
HYDRO_CACHE = os.path.join(ROOT, "tools", "data", "energy", "cache", "hydro")
ATLAS_CACHE = os.path.join(ROOT, "tools", "data", "energy", "cache",
                           "atlas_points.json")
CHIRPS = os.path.join(HYDRO_CACHE, "chirps-v3.0.2025.tif")
ETOPO = os.path.join(HYDRO_CACHE, "ETOPO1_Ice_g_geotiff.tif")
PET = os.path.join(HYDRO_CACHE, "et0_v31_yr.tif")
OUT = os.path.join(ROOT, "data", "definitions", "energy",
                   "renewable_potential.json")


def _find_ne_dir():
    """Pasta do Natural Earth 10m, sem caminho absoluto silencioso."""
    env = os.environ.get("NE_DIR", "").strip()
    candidates = [env] if env else []
    candidates += [
        os.path.join(ROOT, "tools", "data", "natural_earth"),
        os.path.join(ROOT, "tools", "data", "ne_extract"),
        r"G:\tools\ne_extract",
    ]
    for candidate in candidates:
        if candidate and os.path.exists(os.path.join(
                candidate, "ne_10m_rivers_lake_centerlines",
                "ne_10m_rivers_lake_centerlines.shp")):
            return candidate
    raise SystemExit(
        "Natural Earth 10m não encontrado. Defina NE_DIR para a pasta que "
        "contém ne_10m_rivers_lake_centerlines/ (shapefile), ou coloque-a em "
        "tools/data/natural_earth/.")


NE_DIR = _find_ne_dir()
RIVERS = os.path.join(NE_DIR, "ne_10m_rivers_lake_centerlines",
                      "ne_10m_rivers_lake_centerlines")

RIVER_COVER_KM = 50.0
RIVER_COVER_TARGET = 1.0
RIVER_FACTOR_FLOOR = 0.02
GLOBAL_HYDRO_TWH_YEAR = 16400.0
DAYS_IN_MONTH = [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31]
WIND_ROUGHNESS = 0.100  # m — campo aberto/pastagem, declarado
RHO_SEA_LEVEL = 1.225   # kg/m3 — mesma referência do GWA

# Grade de análise do relevo/rios: 4096x2048 sobre o mundo.
GRID_W = 4096
GRID_H = 2048


class Raster:
    """GeoTIFF norte-sul com amostragem no ponto e busca de pixel válido."""

    def __init__(self, path):
        if not os.path.exists(path):
            raise SystemExit("raster ausente: %s (rode fetch_hydro_inputs.py)" % path)
        with tifffile.TiffFile(path) as tif:
            page = tif.pages[0]
            self.array = page.asarray()
            self.shape = self.array.shape
            self.dtype = self.array.dtype
            tags = page.tags
            self.nodata = None
            if "GDAL_NODATA" in tags:
                try:
                    self.nodata = float(str(tags["GDAL_NODATA"].value).strip("\x00"))
                except ValueError:
                    self.nodata = None
            scale = tags["ModelPixelScaleTag"].value if "ModelPixelScaleTag" in tags else None
            tie = tags["ModelTiepointTag"].value if "ModelTiepointTag" in tags else None
            if scale and tie:
                self.sx = float(scale[0])
                self.sy = float(scale[1])
                i, j, _k, x, y, _z = tie
                self.lon0 = float(x) - i * self.sx
                self.lat0 = float(y) + j * self.sy
            else:
                # Fallback global equirretangular (lon -180..180, lat 90..-90).
                self.sx = 360.0 / self.shape[1]
                self.sy = 180.0 / self.shape[0]
                self.lon0 = -180.0
                self.lat0 = 90.0
        if self.array.ndim != 2:
            raise SystemExit("raster não 2D: %s" % path)
        if self.nodata is None:
            # CHIRPS usa -9999; ETOPO1 usa -32768; PET v3.1 usa -9999.
            self.nodata = -9999.0

    def _valid(self, value):
        if not np.isfinite(value):
            return False
        value = float(value)
        if self.nodata < 0:
            return value > self.nodata + 1.0
        # Nodata positivo (ex.: PET uint16 com 65535): o válido é diferente e
        # menor que o nodata.
        return value != self.nodata and value < self.nodata - 1.0

    def pixel(self, lon, lat):
        col = int(math.floor((lon - self.lon0) / self.sx))
        row = int(math.floor((self.lat0 - lat) / self.sy))
        if col < 0 or row < 0 or row >= self.shape[0] or col >= self.shape[1]:
            return None, None, None
        return row, col, self.array[row, col]

    def sample(self, lon, lat, max_radius=80):
        row, col, value = self.pixel(lon, lat)
        if row is None:
            return None, None
        if self._valid(value):
            return float(value), 0
        for radius in range(1, max_radius + 1):
            best = None
            for dy in range(-radius, radius + 1):
                for dx in range(-radius, radius + 1):
                    if max(abs(dx), abs(dy)) != radius:
                        continue
                    r = row + dy
                    c = col + dx
                    if r < 0 or c < 0 or r >= self.shape[0] or c >= self.shape[1]:
                        continue
                    candidate = self.array[r, c]
                    if not self._valid(candidate):
                        continue
                    if best is None or abs(dx) + abs(dy) < best[0]:
                        best = (abs(dx) + abs(dy), float(candidate))
            if best is not None:
                return best[1], radius
        return None, None

    def downsample(self, width, height):
        rows = np.minimum((np.arange(height) + 0.5) / height * self.shape[0],
                          self.shape[0] - 1).astype(np.int32)
        cols = np.minimum((np.arange(width) + 0.5) / width * self.shape[1],
                          self.shape[1] - 1).astype(np.int32)
        return self.array[np.ix_(rows, cols)]


def build_analysis_grid(elev_raster):
    """Elevação e distância aos rios na grade 4096x2048 (pixels quadrados)."""
    elev = elev_raster.downsample(GRID_W, GRID_H).astype(np.float32)
    elev[elev <= elev_raster.nodata + 1.0] = np.nan

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
    return elev, distance_px, drawn


def load_atlas_points():
    import json
    if not os.path.exists(ATLAS_CACHE):
        raise SystemExit("cache GSA/GWA ausente: rode tools/data/energy/fetch_atlas_points.py")
    with open(ATLAS_CACHE, encoding="utf-8") as handle:
        payload = json.load(handle)
    return payload


def wind_100m(wind):
    """Seleciona a classe de rugosidade declarada e devolve média/PD."""
    by_rough = wind.get("by_roughness") or []
    chosen = None
    for entry in by_rough:
        if abs(float(entry["roughness_m"]) - WIND_ROUGHNESS) < 1e-9:
            chosen = entry
            break
    if chosen is None and by_rough:
        chosen = by_rough[len(by_rough) // 2]
    if chosen is None:
        return None
    return {
        "roughness_m": float(chosen["roughness_m"]),
        "m_s_mean": float(chosen["m_s_mean_100m"]),
        "power_density_w_m2": float(chosen["power_density_w_m2_100m"]),
    }


def main():
    cities = ec.load_cities()
    provinces = ec.load_provinces()
    areas = ec.raster_province_area_km2()
    city_prov = ec.city_provinces(cities, provinces)
    atlas = load_atlas_points()["points"]
    province_by_id = {p["id"]: p for p in provinces}
    cached_by_nation = {}
    for pid_str, point in atlas.items():
        pid = int(pid_str)
        if point.get("solar") and point.get("wind"):
            nation = province_by_id.get(pid, {}).get("nation", "")
            cached_by_nation.setdefault(nation, []).append({
                "pid": pid, "lat": float(point["lat"]), "lon": float(point["lon"]),
                "solar": point["solar"], "wind": point["wind"],
            })

    def nearest_cached(lat, lon, nation, max_deg):
        """Ponto medido mais próximo (mesma nação; fallback global)."""
        pools = [cached_by_nation.get(nation, [])]
        pools.append([item for items in cached_by_nation.values() for item in items])
        for pool in pools:
            best = None
            best_d = max_deg
            cos_lat = math.cos(math.radians(lat))
            for item in pool:
                d_lon = (item["lon"] - lon) * cos_lat
                d_lat = item["lat"] - lat
                d = math.sqrt(d_lon * d_lon + d_lat * d_lat)
                if d < best_d:
                    best_d = d
                    best = item
            if best is not None:
                return best, best_d
        return None, None

    print("carregando ETOPO1, CHIRPS e PET...")
    elev_raster = Raster(ETOPO)
    chirps = Raster(CHIRPS)
    pet = Raster(PET)
    print("  ETOPO1 %s | CHIRPS %s | PET %s"
          % (elev_raster.shape, chirps.shape, pet.shape))

    print("grade de relevo/rios...")
    elev_grid, river_distance_px, rivers_drawn = build_analysis_grid(elev_raster)
    ids = ec.load_world_province_ids()[::2, ::2]
    land = ec.load_world_land_mask()[::2, ::2]
    province_ids = np.array([p["id"] for p in provinces], dtype=np.int32)
    keep = np.isin(ids, province_ids)
    valid = keep & land
    flat_ids = ids[valid]
    elev_values = elev_grid[valid]
    river_deg = river_distance_px[valid] * (360.0 / GRID_W)
    rows = np.arange(GRID_H, dtype=np.float64)
    cos_lat = np.cos(np.deg2rad(90.0 - (rows + 0.5) / GRID_H * 180.0))
    cos_2d = np.broadcast_to(cos_lat[:, None], (GRID_H, GRID_W))
    river_km = river_deg * 111.32 * cos_2d[valid]
    del cos_2d, river_distance_px

    max_id = max(p["id"] for p in provinces)
    has_elev = ~np.isnan(elev_values)
    elev_flat = np.where(has_elev, elev_values, -1e9)
    relief_max = ndimage.maximum(elev_flat, labels=flat_ids,
                                 index=np.arange(0, max_id + 1))
    elev_flat_min = np.where(has_elev, elev_values, 1e9)
    relief_min = ndimage.minimum(elev_flat_min, labels=flat_ids,
                                 index=np.arange(0, max_id + 1))
    elev_sum = ndimage.sum(np.where(has_elev, elev_values, 0.0), labels=flat_ids,
                           index=np.arange(0, max_id + 1))
    counts = np.bincount(flat_ids, minlength=max_id + 1)
    river_cover = np.bincount(flat_ids,
                              weights=(river_km <= RIVER_COVER_KM).astype(np.float64),
                              minlength=max_id + 1)
    del flat_ids, elev_values, river_km, has_elev, elev_flat, elev_flat_min, ids, land, keep, valid

    province_out = {}
    raw_total = 0.0
    missing_climate = []
    outside_chirps = []
    fallback_count = 0
    for index, province in enumerate(provinces, 1):
        if index % 2000 == 0:
            print("  províncias: %d/%d" % (index, len(provinces)))
        pid = province["id"]
        lon, lat = ec.map_to_lonlat(province["x"], province["y"])
        point = atlas.get(str(pid)) or {}
        solar_raw = point.get("solar") or {}
        wind_raw = point.get("wind") or {}
        sample_method = "api_point"
        sample_source_pid = None
        sample_distance_km = None
        if not solar_raw or not wind_raw:
            fallback, distance = nearest_cached(lat, lon, province["nation"], 12.0)
            if fallback is not None:
                solar_raw = fallback["solar"]
                wind_raw = fallback["wind"]
                sample_method = "nearest_cached_province"
                sample_source_pid = fallback["pid"]
                sample_distance_km = round(distance * 111.0, 1)
                fallback_count += 1
        n = counts[pid] if pid < counts.size else 0
        relief = None
        mean_elev = None
        if n > 0 and relief_max[pid] > -1e8 and relief_min[pid] < 1e8:
            relief = float(relief_max[pid] - relief_min[pid])
            mean_elev = float(elev_sum[pid] / n)
        river_factor = None
        if n > 0:
            coverage = float(river_cover[pid] / n)
            river_factor = max(RIVER_FACTOR_FLOOR, min(1.0, coverage / RIVER_COVER_TARGET))

        entry = {
            "province_id": pid,
            "nation": province["nation"],
            "area_km2": round(areas.get(pid, 0.0), 1),
            "solar": None,
            "wind": None,
            "hydro": None,
        }

        # --- solar (GSA: anual + 12 meses) ---------------------------------
        ghi_year = solar_raw.get("ghi_year_kwh_m2")
        pvout_year = solar_raw.get("pvout_year_kwh_kwp")
        ghi_month = solar_raw.get("ghi_monthly_kwh_m2")
        pvout_month = solar_raw.get("pvout_monthly_kwh_kwp")
        if ghi_year is not None and ghi_month and len(ghi_month) == 12:
            ghi_daily = [float(v) / DAYS_IN_MONTH[i] for i, v in enumerate(ghi_month)]
            pvout_daily = ([float(v) / DAYS_IN_MONTH[i] for i, v in enumerate(pvout_month)]
                           if pvout_month and len(pvout_month) == 12 else None)
            entry["solar"] = {
                "kwh_m2_day_mean": round(float(ghi_year) / 365.25, 3),
                "kwh_m2_year": round(float(ghi_year), 1),
                "ghi_kwh_m2_year": round(float(ghi_year), 1),
                "pvout_kwh_kwp_year": (round(float(pvout_year), 1)
                                       if pvout_year is not None else None),
                "pvout_kwh_kwp_day_mean": (round(float(pvout_year) / 365.25, 3)
                                           if pvout_year is not None else None),
                "min_month": int(np.argmin(ghi_daily)) + 1,
                "min_month_kwh_m2_day": round(min(ghi_daily), 3),
                "max_month": int(np.argmax(ghi_daily)) + 1,
                "max_month_kwh_m2_day": round(max(ghi_daily), 3),
                "monthly_kwh_m2_day": [round(v, 3) for v in ghi_daily],
                "monthly_pvout_kwh_kwp_day": (
                    [round(v, 3) for v in pvout_daily] if pvout_daily else None),
                "source": "Global Solar Atlas 2.x (GSA/Solargis, CC BY 4.0)",
                "sample_offset_px": None,
                "sample_method": sample_method,
                "sample_source_pid": sample_source_pid,
                "sample_distance_km": sample_distance_km,
            }
        else:
            missing_climate.append(pid)

        # --- vento (GWA: Weibull 100 m, sem série mensal) -------------------
        wind = wind_100m(wind_raw) if wind_raw else None
        if wind is not None:
            # Densidade do ar na altitude média da província (atm. padrão).
            rho = RHO_SEA_LEVEL * math.exp(-max(mean_elev or 0.0, 0.0) / 8500.0)
            entry["wind"] = {
                "m_s_mean": round(wind["m_s_mean"], 3),
                "min_month": None,
                "min_month_m_s": None,
                "max_month": None,
                "max_month_m_s": None,
                "monthly_m_s": None,
                "monthly_available": False,
                "power_density_w_m2": round(
                    wind["power_density_w_m2"] * (rho / RHO_SEA_LEVEL), 1),
                "roughness_m": wind["roughness_m"],
                "source": "Global Wind Atlas 3/4 (GWA/DTU, CC BY 4.0)",
                "sample_offset_px": None,
                "sample_method": sample_method,
                "sample_source_pid": sample_source_pid,
                "sample_distance_km": sample_distance_km,
            }
        else:
            missing_climate.append(pid)

        # --- hídrico (CHIRPS + PET + ETOPO1 + rios) -------------------------
        # CHIRPS cobre 60N-60S; fora disso não há amostra (lacuna declarada).
        if abs(lat) > 60.0:
            precip, precip_shift = None, None
        else:
            precip, precip_shift = chirps.sample(lon, lat, max_radius=8)
        if precip is None:
            pet_value, pet_shift = None, None
        else:
            # Centroide de província em terra: raio curto basta; se não achar,
            # é no_data (não varre o oceano inteiro).
            pet_value, pet_shift = pet.sample(lon, lat, max_radius=4)
        if precip is None:
            outside_chirps.append(pid)
            entry["hydro"] = {
                "precip_mm": None,
                "pet_mm": None if pet_value is None else round(float(pet_value), 0),
                "relief_m": None if relief is None else round(relief, 0),
                "runoff_factor": None,
                "river_factor": None if river_factor is None else round(river_factor, 4),
                "raw_index": 0.0,
                "method": "proxy_precip_relief_river_free",
                "confidence": "no_data",
                "normalized": "global_anchor",
                "national_anchor_missing": True,
                "no_data_reason": "fora da cobertura CHIRPS (60N-60S) ou sem pixel válido",
            }
        elif pet_value is None or relief is None or river_factor is None:
            entry["hydro"] = {
                "precip_mm": round(float(precip), 0),
                "pet_mm": None if pet_value is None else round(float(pet_value), 0),
                "relief_m": None if relief is None else round(relief, 0),
                "runoff_factor": None,
                "river_factor": None if river_factor is None else round(river_factor, 4),
                "raw_index": 0.0,
                "method": "proxy_precip_relief_river_free",
                "confidence": "no_data",
                "normalized": "global_anchor",
                "national_anchor_missing": True,
                "no_data_reason": "sem PET/relevo/rio para a província",
            }
        else:
            precip = float(precip)
            pet_value = float(pet_value)
            runoff = (min(1.0, max(0.0, (precip - pet_value) / precip))
                      if precip > 0.0 else 0.0)
            area = areas.get(pid, 0.0)
            raw = (precip / 1000.0) * area * (relief / 1000.0) * runoff * river_factor
            raw_total += raw
            entry["hydro"] = {
                "precip_mm": round(precip, 0),
                "pet_mm": round(pet_value, 0),
                "relief_m": round(relief, 0),
                "runoff_factor": round(runoff, 3),
                "river_factor": round(river_factor, 4),
                "raw_index": round(raw, 3),
                "method": "proxy_precip_relief_river_free",
                "confidence": "low",
                "normalized": "global_anchor",
                "national_anchor_missing": True,
                "sample_offset_px": {"chirps": precip_shift, "pet": pet_shift},
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

    # --- agregação por cidade (área x fração da província) -----------------
    city_out = {}
    for city in cities:
        links = city_prov.get(city["id"], [])
        weight = 0.0
        wind_weight = 0.0
        pvout_weight = 0.0
        solar_month = [0.0] * 12
        pvout_month = [0.0] * 12
        solar_year = 0.0
        pvout_year = 0.0
        wind_sum = 0.0
        wind_pd = 0.0
        hydro_raw = 0.0
        covered = 0
        for pid, share in links:
            entry = province_out.get(str(pid))
            if not entry or entry["solar"] is None:
                continue
            w = entry["area_km2"] * share
            weight += w
            solar_year += entry["solar"]["kwh_m2_year"] * w
            for i in range(12):
                solar_month[i] += entry["solar"]["monthly_kwh_m2_day"][i] * w
                if entry["solar"]["monthly_pvout_kwh_kwp_day"]:
                    pvout_month[i] += entry["solar"]["monthly_pvout_kwh_kwp_day"][i] * w
            if entry["solar"]["pvout_kwh_kwp_year"] is not None:
                pvout_year += entry["solar"]["pvout_kwh_kwp_year"] * w
                pvout_weight += w
            if entry["wind"] is not None:
                wind_sum += entry["wind"]["m_s_mean"] * w
                wind_pd += entry["wind"]["power_density_w_m2"] * w
                wind_weight += w
            hydro_raw += entry["hydro"]["raw_index"] * share
            covered += 1
        if weight <= 0.0:
            city_out[city["id"]] = {"city_id": city["id"], "nation": city["tag"],
                                    "solar": None, "wind": None, "hydro": None,
                                    "provinces": len(links), "provinces_with_data": 0}
            continue
        solar_month = [v / weight for v in solar_month]
        pvout_month = [v / pvout_weight for v in pvout_month] if pvout_weight > 0 else None
        city_out[city["id"]] = {
            "city_id": city["id"],
            "nation": city["tag"],
            "area_km2": round(weight, 1),
            "provinces": len(links),
            "provinces_with_data": covered,
            "solar": {
                "kwh_m2_day_mean": round(solar_year / weight / 365.25, 3),
                "kwh_m2_year": round(solar_year / weight, 1),
                "pvout_kwh_kwp_year": (round(pvout_year / pvout_weight, 1)
                                       if pvout_weight > 0 else None),
                "pvout_kwh_kwp_day_mean": (round(pvout_year / pvout_weight / 365.25, 3)
                                           if pvout_weight > 0 else None),
                "min_month": int(np.argmin(solar_month)) + 1,
                "max_month": int(np.argmax(solar_month)) + 1,
                "monthly_kwh_m2_day": [round(v, 3) for v in solar_month],
                "monthly_pvout_kwh_kwp_day": ([round(v, 3) for v in pvout_month]
                                              if pvout_month else None),
            },
            "wind": ({
                "m_s_mean": round(wind_sum / wind_weight, 3),
                "min_month": None,
                "max_month": None,
                "monthly_m_s": None,
                "monthly_available": False,
                "power_density_w_m2": round(wind_pd / wind_weight, 1),
            } if wind_weight > 0 else None),
            "hydro": {
                "twh_per_year": round(hydro_raw * scale, 4),
                "method": "proxy_precip_relief_river_free",
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
        agg = nation_out.setdefault(entry["nation"], {"hydro_twh_per_year": 0.0})
        agg["_solar_w"] = agg.get("_solar_w", 0.0) + entry["solar"]["kwh_m2_day_mean"] * entry["area_km2"]
        agg["_solar_wa"] = agg.get("_solar_wa", 0.0) + entry["area_km2"]
        if entry.get("wind") is not None:
            agg["_wind_w"] = agg.get("_wind_w", 0.0) + entry["wind"]["m_s_mean"] * entry["area_km2"]
            agg["_wind_wa"] = agg.get("_wind_wa", 0.0) + entry["area_km2"]
            if entry["wind"].get("power_density_w_m2") is not None:
                agg["_wind_pd_w"] = agg.get("_wind_pd_w", 0.0) + entry["wind"]["power_density_w_m2"] * entry["area_km2"]
                agg["_wind_pd_wa"] = agg.get("_wind_pd_wa", 0.0) + entry["area_km2"]
        if entry["solar"].get("pvout_kwh_kwp_day_mean") is not None:
            agg["_pvout_w"] = agg.get("_pvout_w", 0.0) + entry["solar"]["pvout_kwh_kwp_day_mean"] * entry["area_km2"]
            agg["_pvout_wa"] = agg.get("_pvout_wa", 0.0) + entry["area_km2"]
    for agg in nation_out.values():
        solar_area = agg.pop("_solar_wa", 0.0)
        wind_area = agg.pop("_wind_wa", 0.0)
        pvout_area = agg.pop("_pvout_wa", 0.0)
        wind_pd_area = agg.pop("_wind_pd_wa", 0.0)
        agg["solar_kwh_m2_day_mean"] = (round(agg.pop("_solar_w") / solar_area, 3)
                                        if solar_area > 0 else None)
        agg["wind_m_s_mean"] = (round(agg.pop("_wind_w") / wind_area, 3)
                                if wind_area > 0 else None)
        agg["solar_pvout_kwh_kwp_day_mean"] = (
            round(agg.pop("_pvout_w") / pvout_area, 3) if pvout_area > 0 else None)
        agg["wind_power_density_w_m2"] = (
            round(agg.pop("_wind_pd_w") / wind_pd_area, 1) if wind_pd_area > 0 else None)

    # As séries mensais por província já foram agregadas nas cidades.
    for entry in province_out.values():
        if entry["solar"]:
            entry["solar"].pop("monthly_kwh_m2_day", None)
            entry["solar"].pop("monthly_pvout_kwh_kwp_day", None)

    meta = {
        "generated_by": "tools/build_energy_renewables.py",
        "plan": "docs/plans/perf_pop_financas_plano.md, PARTE E, E1",
        "owner_decision": "D-115(c): GSA/GWA no lugar do WorldClim; hídrico com CHIRPS/PET/ETOPO1",
        "solar_wind": {
            "solar_source": "Global Solar Atlas 2.x (Solargis/ESMAP/World Bank), GHI e PVOUT",
            "wind_source": "Global Wind Atlas 3/4 (DTU/ESMAP/World Bank), Weibull a 100 m",
            "url": "https://globalsolaratlas.info / https://globalwindatlas.info",
            "license": {
                "name": "CC BY 4.0",
                "url": "https://creativecommons.org/licenses/by/4.0/",
                "commercial_ok": True,
                "evidence": ("World Bank Data Catalog 0038645 (GHI) e 0038643 (vento) "
                             "declaram CC BY 4.0; GSA/GWA são gratuitos com atribuição"),
            },
            "citation": ("Global Solar Atlas 2.x (Solargis/ESMAP); Global Wind Atlas 3/4 "
                         "(DTU/ESMAP); Banco Mundial, CC BY 4.0"),
            "units": {"ghi": "kWh/m2/ano e kWh/m2/dia (mensal / dias do mês)",
                      "pvout": "kWh/kWp/ano e kWh/kWp/dia",
                      "wind": "m/s a 100 m; densidade W/m2"},
            "sampling": ("1 ponto por província no centróide (cache "
                         "fetch_atlas_points.py); quando o ponto falta, usa o ponto "
                         "medido mais próximo da mesma nação (raio 12°) com "
                         "`sample_method: nearest_cached_province` e distância "
                         "declarada; cidade = média ponderada por area_km2 x fração "
                         "da província na cidade"),
            "wind_roughness_m": WIND_ROUGHNESS,
            "wind_monthly": ("GWA não publica série mensal; `monthly_m_s` é null e "
                             "`monthly_available` é false — lacuna declarada"),
            "area_source": ("área contada no raster do atlas (world_province + "
                            "world_sdf, 8192x4096, cosseno da latitude)"),
            "confidence": ("high (climatologia medida); GSA 1 km/PVOUT e GWA 250 m "
                           "amostrados no centróide; vento é média anual, sem sazonalidade"),
        },
        "hydro": {
            "method": "proxy_precip_relief_river_free",
            "formula": ("raw = precip_mm/1000 * area_km2 * relief_m/1000 * runoff * "
                        "river_factor"),
            "precip": "CHIRPS v3.0 anual 2025, 0,05° (domínio público/CC BY 4.0)",
            "pet": ("Global Aridity and PET Database v3.1 (CGIAR-CSI), PET anual, "
                    "30 arc-sec (CC BY 4.0)"),
            "relief": ("max-min da altitude ETOPO1 (1 arc-min, domínio público) nos "
                       "pixels de terra da província, na grade 4096x2048 do atlas"),
            "runoff": ("clamp((precip - PET)/precip, 0, 1) com precip CHIRPS e PET "
                       "anual; não usa temperatura/WorldClim"),
            "river_factor": ("cobertura = fração dos pixels de terra da província a "
                             "<= 50 km de um rio do Natural Earth 10m; "
                             "fator = clamp(cobertura, 0.02, 1.0)"),
            "normalization": {
                "anchor": "potencial técnico hidrelétrico mundial",
                "value_twh_year": GLOBAL_HYDRO_TWH_YEAR,
                "source": ("IJHD, Hydropower Essentials (OSTI/ETDEWEB 21594387): "
                           "'overall technical potential ... more than 16 400 TWh/yr'"),
                "scope": "global",
                "why_not_national": ("a divisão entre nações é do proxy; a âncora "
                                     "nacional do E1 é aplicada em "
                                     "build_hydro_national.py"),
            },
            "coverage_gap": ("CHIRPS cobre 60°N-60°S; províncias fora dessa faixa "
                             "ficam `no_data`, sem preencher com WorldClim"),
            "confidence": "low",
            "no_data_note": ("províncias sem precipitação/PET/relevo/rio ficam com "
                             "confidence='no_data', raw_index=0, no_data_reason e "
                             "0,0 TWh — ver counts.provinces_no_hydro_data"),
        },
        "counts": {
            "provinces": len(province_out),
            "cities": len(city_out),
            "nations": len(nation_out),
            "rivers_drawn": rivers_drawn,
            "provinces_without_climate_sample": len(set(missing_climate)),
            "provinces_with_nearest_fallback": fallback_count,
            "provinces_outside_chirps": len(set(outside_chirps)),
            "provinces_no_hydro_data": sum(
                1 for entry in province_out.values()
                if entry["hydro"]["confidence"] == "no_data"),
            "raw_index_total": round(raw_total, 1),
            "hydro_scale_twh_per_raw_index": float("%.12g" % scale),
            "atlas_points_cached": len(atlas),
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
    print("rios rasterizados: %d | sem amostra GSA/GWA: %d | fora CHIRPS: %d"
          % (rivers_drawn, len(set(missing_climate)), len(set(outside_chirps))))
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
