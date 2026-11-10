"""Aplica clima real (WorldClim 2.1 10-min) a nature.json e agriculture.json.

Substitui altitude/temperatura/precipitação sintéticas (faixas de latitude +
caixas de montanha) por medições reais e re-deriva as fórmulas (solo, relevo,
fertilidade, água, floresta, cultivável, aptidão de culturas) com esses valores
reais — reusa as funções de tools/build_natural_geography.py, sem duplicar.

Entradas (tools/data/worldclim/, gitignored — baixe com tools/fetch_worldclim.py):
  wc2.1_10m_elev.tif   altitude (m), int16, 2160x1080 (10 arc-min)
  wc2.1_10m_bio_1.tif  temperatura média anual (°C), float32
  wc2.1_10m_bio_12.tif precipitação anual (mm), float32

Uso:
  python tools/apply_real_climate.py
  python tools/build_resource_map.py   # regenera resource_points.json (trigo)

Limitação honesta: a amostragem é no CENTRÓIDE do território (não a média das
províncias, porque data/history/territories.json está fora do repo). Territórios
cujo centróide cai em água (ilhas/arquipélagos) mantêm os valores sintéticos
(`data_quality: "test"`); os demais passam a `data_quality: "real_climate"`.
"""

import json
import os

import numpy as np
import tifffile

import build_natural_geography as bng

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NATURAL = os.path.join(ROOT, "data", "definitions", "natural")
WC = os.path.join(ROOT, "tools", "data", "worldclim")

ELEV_PATH = os.path.join(WC, "wc2.1_10m_elev.tif")
BIO1_PATH = os.path.join(WC, "wc2.1_10m_bio_1.tif")
BIO10_PATH = os.path.join(WC, "wc2.1_10m_bio_10.tif")
BIO11_PATH = os.path.join(WC, "wc2.1_10m_bio_11.tif")
BIO12_PATH = os.path.join(WC, "wc2.1_10m_bio_12.tif")

CELL = 0.16666666666666666  # 10 arc-min = 1/6 grau


def read_json(path):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def write_json_tab(path, data):
    """Preserva o formato atual (indent=tab, CRLF, LF final)."""
    text = json.dumps(data, indent="\t", ensure_ascii=False).replace("\n", "\r\n") + "\r\n"
    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write(text)


def sample(rasters, lon, lat):
    col = int(round((lon + 180.0) / CELL))
    row = int(round((90.0 - lat) / CELL))
    h, w = rasters[0].shape
    col = max(0, min(w - 1, col))
    row = max(0, min(h - 1, row))
    return tuple(raster[row, col] for raster in rasters)


def main() -> int:
    for path in (ELEV_PATH, BIO1_PATH, BIO10_PATH, BIO11_PATH, BIO12_PATH):
        if not os.path.exists(path):
            raise SystemExit("raster ausente: %s (rode tools/fetch_worldclim.py)" % path)

    elev = tifffile.imread(ELEV_PATH)
    bio1 = tifffile.imread(BIO1_PATH)
    bio10 = tifffile.imread(BIO10_PATH)
    bio11 = tifffile.imread(BIO11_PATH)
    bio12 = tifffile.imread(BIO12_PATH)

    nature = read_json(os.path.join(NATURAL, "nature.json"))
    agriculture = read_json(os.path.join(NATURAL, "agriculture.json"))
    territories = nature["territories"]
    crops = list(bng.CROPS.keys())

    updated = 0
    skipped_water = 0

    for territory_id, data in territories.items():
        px, py = data["centroid_px"]
        lon, lat = bng.pixel_to_lonlat(px, py)
        altitude_raw, temp_raw, warm_raw, cold_raw, precip_raw = sample(
            (elev, bio1, bio10, bio11, bio12), lon, lat)

        # Nodata: elev oceano = -32768; bio oceano = -3.4e38. Centróide em água
        # (ilha/arquipélago): mantém os valores sintéticos.
        if altitude_raw < -1000 or temp_raw < -100.0:
            skipped_water += 1
            continue

        altitude = float(altitude_raw)
        temperature = float(temp_raw)
        warm_temp = float(warm_raw)
        cold_temp = float(cold_raw)
        precip = float(precip_raw)

        # Clima re-derivado com dados reais: zona por latitude + aridez real
        # (De Martonne sobre temp/precip medidos), substituindo as caixas.
        zone = bng.zone_for_latitude(lat)
        aridity = bng.aridity_from_climate(temperature, precip)
        climate = bng.climate_label(zone, aridity)
        soil_type, fertility = bng.soil_for(zone, aridity)
        coastal = bool(data.get("coastal", False))
        water = bng.clamp(0.12 + precip / 1500.0 + (0.08 if coastal else 0.0), 0.0, 1.0)
        if fertility >= 0.7 and water >= 0.6:
            soil_type, fertility = "fluvisol", 0.88
        forest = bng.forest_for(zone, aridity, altitude, fertility)
        relief = "mountain" if altitude >= 1500.0 else "hills" if altitude >= 500.0 else "plain"
        relief_penalty = 0.7 if relief == "mountain" else 0.3 if relief == "hills" else 0.0
        cultivable = bng.clamp(
            fertility * water * (1.0 - 0.6 * forest) * (1.0 - relief_penalty), 0.0, 1.0)

        data["climate"] = climate
        data["altitude_m"] = round(altitude)
        data["temp_c"] = round(temperature, 1)
        data["precip_mm"] = round(precip)
        data["soil_type"] = soil_type
        data["fertility"] = round(fertility, 3)
        data["water"] = round(water, 3)
        data["relief"] = relief
        data["forest_cover"] = round(forest, 3)
        data["cultivable"] = round(cultivable, 3)
        data["data_quality"] = "real_climate"

        updated += 1

        # Recalcula a aptidão de cada cultura com clima real. Temperatura de
        # cultivo por estação (trimestre quente/frio), não média anual.
        if territory_id in agriculture["territories"]:
            ag = agriculture["territories"][territory_id]
            for crop in crops:
                ag[crop] = bng.crop_suitability_seasonal(
                    crop, temperature, warm_temp, cold_temp, precip, fertility, altitude)

    nature["_meta"]["data_quality"] = (
        "real_climate (WorldClim 2.1 10-min) para altitude/temp/precip; "
        "clima/solo/relevo derivados das fórmulas re-alimentadas")
    agriculture["_meta"]["data_quality"] = (
        "real_climate (WorldClim 2.1 10-min); aptidão recalculada com clima real")
    agriculture["_meta"]["crops"] = list(bng.CROPS.keys())

    write_json_tab(os.path.join(NATURAL, "nature.json"), nature)
    write_json_tab(os.path.join(NATURAL, "agriculture.json"), agriculture)

    print("territórios atualizados: %d | mantidos sintéticos (centróide em água): %d"
          % (updated, skipped_water))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
