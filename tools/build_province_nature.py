"""Aptidão agrícola e geografia natural POR PROVÍNCIA (admin-1 do atlas).

Por que existe: `agriculture.json`/`nature.json` amostram o clima UMA vez no
centróide do território. O território é grosso (739 regiões para o mundo) e o
centróide cai no deserto/água com frequência: Egito ficava com trigo 0, Israel
0, Líbano 0 — não por falta de terra, mas por um único ponto de amostragem.
A bacia de captação do jogo já é POR PROVÍNCIA (GeographyService.assign_provinces);
este arquivo dá a cada província a sua própria aptidão, amostrada no centróide
dela com a MESMA fonte (WorldClim 2.1 10-min) e as MESMAS fórmulas de
tools/build_natural_geography.py.

Entradas:
  map/map_data/atlas/provinces.json            4529 províncias (id, nation, x, y)
  data/definitions/natural/province_areas.json área (km²) por província
  data/definitions/natural/nature.json         territórios (centróides, p/ herança)
  data/definitions/natural/agriculture.json    aptidão por território (fallback)
  tools/data/worldclim/*.tif                   WorldClim 2.1 10-min (não versionado)

Saídas (versionadas):
  data/definitions/natural/province_nature.json  aptidão por cultura + natureza
  data/definitions/natural/nature.json           cultivável/floresta/temp/precip
                                                 por território (média por área)
  data/definitions/natural/agriculture.json      aptidão por território (média
                                                 ponderada por área das províncias)
                                                 e posse adm0_a3 por maioria de área

Nada é inventado: clima/temperatura/precipitação/altitude vêm do raster; as
fórmulas (solo, fertilidade, água, floresta, cultivável, aptidão trapezoidal)
são as mesmas do pipeline existente. Província com centróide em água (nodata do
raster) NÃO ganha entrada e cai no fallback do território — a ausência é
declarada, não preenchida com número fictício.

Uso:
  python tools/build_province_nature.py
"""

import json
import os
import sys

import tifffile

import build_natural_geography as bng
from world_nation_curation import TERRITORY_MOTHER

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NATURAL = os.path.join(ROOT, "data", "definitions", "natural")
PROVINCES = os.path.join(ROOT, "map", "map_data", "atlas", "provinces.json")
WC = os.path.join(ROOT, "tools", "data", "worldclim")

ELEV_PATH = os.path.join(WC, "wc2.1_10m_elev.tif")
BIO1_PATH = os.path.join(WC, "wc2.1_10m_bio_1.tif")
BIO10_PATH = os.path.join(WC, "wc2.1_10m_bio_10.tif")
BIO11_PATH = os.path.join(WC, "wc2.1_10m_bio_11.tif")
BIO12_PATH = os.path.join(WC, "wc2.1_10m_bio_12.tif")
CELL = 0.16666666666666666  # 10 arc-min = 1/6 grau

# Tag do jogo -> ISO-3 do Natural Earth (a mesma tabela do GeographyService).
NATION_CODE_OVERRIDES = {"NED": "NLD", "XKX": "KOS"}


def read_json(path):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def write_json_tab(path, data):
    """Formato do projeto: indent=tab, CRLF, newline final."""
    text = json.dumps(data, indent="\t", ensure_ascii=False).replace("\n", "\r\n") + "\r\n"
    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write(text)


def sample(rasters, lon, lat):
    col = int(round((lon + 180.0) / CELL))
    row = int(round((90.0 - lat) / CELL))
    height, width = rasters[0].shape
    col = max(0, min(width - 1, col))
    row = max(0, min(height - 1, row))
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
    rasters = (elev, bio1, bio10, bio11, bio12)

    provinces = read_json(PROVINCES).get("provinces", [])
    if not provinces:
        raise SystemExit("provinces.json sem províncias")
    province_areas = read_json(os.path.join(NATURAL, "province_areas.json")).get("areas", {})
    nature_doc = read_json(os.path.join(NATURAL, "nature.json"))
    agriculture_doc = read_json(os.path.join(NATURAL, "agriculture.json"))
    territories = nature_doc["territories"]
    agriculture = agriculture_doc["territories"]
    crops = list(bng.CROPS.keys())

    nav = bng.load_nav_mask()

    # Amostragem por província. `data_quality` individual declara a fonte.
    per_province = {}
    skipped_water = 0
    for province in provinces:
        pid = int(province.get("id", 0))
        if pid <= 0:
            continue
        px = float(province.get("x", 0.0))
        py = float(province.get("y", 0.0))
        lon, lat = bng.pixel_to_lonlat(px, py)
        altitude_raw, temp_raw, warm_raw, cold_raw, precip_raw = sample(rasters, lon, lat)
        if altitude_raw < -1000 or temp_raw < -100.0:
            skipped_water += 1
            continue

        altitude = float(altitude_raw)
        temperature = float(temp_raw)
        warm_temp = float(warm_raw)
        cold_temp = float(cold_raw)
        precip = float(precip_raw)
        zone = bng.zone_for_latitude(lat)
        aridity = bng.aridity_from_climate(temperature, precip)
        climate = bng.climate_label(zone, aridity)
        soil_type, fertility = bng.soil_for(zone, aridity)
        coastal = bng.is_coastal(nav, px, py)
        water = bng.clamp(0.12 + precip / 1500.0 + (0.08 if coastal else 0.0), 0.0, 1.0)
        if fertility >= 0.7 and water >= 0.6:
            soil_type, fertility = "fluvisol", 0.88
        forest = bng.forest_for(zone, aridity, altitude, fertility)
        relief = "mountain" if altitude >= 1500.0 else "hills" if altitude >= 500.0 else "plain"
        relief_penalty = 0.7 if relief == "mountain" else 0.3 if relief == "hills" else 0.0
        cultivable = bng.clamp(
            fertility * water * (1.0 - 0.6 * forest) * (1.0 - relief_penalty), 0.0, 1.0)

        per_province[pid] = {
            "nation": str(province.get("nation", "")),
            "area_km2": float(province_areas.get(str(pid), province.get("area_global", 0.0))),
            "px": px,
            "py": py,
            "climate": climate,
            "temp_c": round(temperature, 1),
            "precip_mm": round(precip),
            "altitude_m": round(altitude),
            "soil_type": soil_type,
            "fertility": round(fertility, 3),
            "water": round(water, 3),
            "forest_cover": round(forest, 3),
            "cultivable": round(cultivable, 3),
            "crops": {
                crop: bng.crop_suitability_seasonal(
                    crop, temperature, warm_temp, cold_temp, precip, fertility, altitude)
                for crop in crops
            },
        }

    # Província -> território: MESMA regra do runtime (centróide mais próximo,
    # percorrendo os territórios na ordem do JSON e mantendo o primeiro em caso
    # de empate). A herança é a mesma de GeographyService._load_provinces.
    order = [(tid, float(entry["centroid_px"][0]), float(entry["centroid_px"][1]))
             for tid, entry in territories.items()]
    by_territory = {tid: [] for tid, _, _ in order}
    for pid, data in per_province.items():
        best = ""
        best_distance = float("inf")
        for tid, cx, cy in order:
            distance = (cx - data["px"]) ** 2 + (cy - data["py"]) ** 2
            if distance < best_distance:
                best_distance, best = distance, tid
        by_territory[best].append(pid)

    # Média ponderada por área das províncias do território (substitui a
    # amostra única no centróide). Território sem província herda a província
    # mais próxima — o mesmo chão, com a mesma fonte real.
    numeric_nature = ["temp_c", "precip_mm", "fertility", "water", "altitude_m",
                      "forest_cover", "cultivable"]
    aggregated = 0
    inherited = 0
    for tid, pids in by_territory.items():
        entry = territories[tid]
        if not pids:
            cx, cy = entry["centroid_px"]
            nearest = None
            nearest_distance = float("inf")
            for pid, data in per_province.items():
                distance = (data["px"] - cx) ** 2 + (data["py"] - cy) ** 2
                if distance < nearest_distance:
                    nearest_distance, nearest = distance, pid
            if nearest is None:
                continue
            data = per_province[nearest]
            for field in numeric_nature:
                entry[field] = data[field]
            if tid in agriculture:
                agriculture[tid] = dict(data["crops"])
            by_territory[tid] = [nearest]
            inherited += 1
            continue
        weight_total = sum(per_province[pid]["area_km2"] for pid in pids)
        if weight_total <= 0.0:
            continue
        for field in numeric_nature:
            total = sum(per_province[pid][field] * per_province[pid]["area_km2"]
                        for pid in pids)
            entry[field] = round(total / weight_total, 3)
        if tid in agriculture:
            weights = {crop: 0.0 for crop in crops}
            for pid in pids:
                area = per_province[pid]["area_km2"]
                for crop in crops:
                    weights[crop] += area * per_province[pid]["crops"][crop]
            agriculture[tid] = {
                crop: round(weights[crop] / weight_total, 3) for crop in crops
            }
        aggregated += 1

    # Posse: maioria de ÁREA das províncias do território. A tabela NE->ISO3 é
    # a identidade, exceto NED->NLD e XKX->KOS (as mesmas do GeographyService).
    owned = 0
    unowned = []
    for tid, pids in by_territory.items():
        if not pids:
            unowned.append(tid)
            # Território apagado pela curadoria não tem mais província no
            # atlas. O dono não pode continuar sendo uma nação que saiu do
            # cenário: herda a nação-mãe declarada (SPM->FRA, MNP->USA).
            entry = territories[tid]
            old_owner = str(entry.get("owner") or entry.get("adm0_a3") or "")
            mother = TERRITORY_MOTHER.get(old_owner)
            if mother:
                entry["owner"] = mother
                entry["adm0_a3"] = NATION_CODE_OVERRIDES.get(mother, mother)
            continue
        area_by_tag = {}
        for pid in pids:
            tag = per_province[pid]["nation"]
            area_by_tag[tag] = area_by_tag.get(tag, 0.0) + per_province[pid]["area_km2"]
        tag = max(area_by_tag.items(), key=lambda item: (item[1], item[0]))[0]
        code = NATION_CODE_OVERRIDES.get(tag, tag)
        territories[tid]["owner"] = tag
        territories[tid]["adm0_a3"] = code
        owned += 1

    # Depósitos: o campo `owner` (legado, não lido pelo runtime) passa a refletir
    # o dono do território. O runtime usa nature.json.adm0_a3 em assign_deposits.
    deposits_doc = read_json(os.path.join(NATURAL, "deposits.json"))
    for deposit in deposits_doc.get("deposits", []):
        tid = str(deposit.get("territory", ""))
        if tid in territories:
            deposit["owner"] = territories[tid].get("owner", deposit.get("owner", ""))

    # Documento por província (o que o runtime usa + proveniência).
    province_doc = {
        "_meta": {
            "description": "Aptidão agrícola e natureza por província (admin-1)",
            "data_quality": "real_climate (WorldClim 2.1 10-min) no centróide de cada província",
            "method": "mesmas fórmulas de tools/build_natural_geography.py",
            "crops": crops,
            "provinces_without_raster": skipped_water,
            "source": "tools/build_province_nature.py",
        },
        "provinces": {
            str(pid): {
                "nation": data["nation"],
                "climate": data["climate"],
                "temp_c": data["temp_c"],
                "precip_mm": data["precip_mm"],
                "altitude_m": data["altitude_m"],
                "soil_type": data["soil_type"],
                "fertility": data["fertility"],
                "water": data["water"],
                "forest_cover": data["forest_cover"],
                "cultivable": data["cultivable"],
                "crops": data["crops"],
            }
            for pid, data in sorted(per_province.items())
        },
    }

    nature_doc["_meta"]["data_quality"] = (
        "real_climate (WorldClim 2.1 10-min) por província; território = média "
        "ponderada por área das províncias (fallback: amostra do centróide)")
    nature_doc["_meta"]["territories_inherited_nearest_province"] = inherited
    agriculture_doc["_meta"]["data_quality"] = (
        "real_climate (WorldClim 2.1 10-min) por província; aptidão do território "
        "= média ponderada por área")
    agriculture_doc["_meta"]["crops"] = crops

    write_json_tab(os.path.join(NATURAL, "province_nature.json"), province_doc)
    write_json_tab(os.path.join(NATURAL, "nature.json"), nature_doc)
    write_json_tab(os.path.join(NATURAL, "agriculture.json"), agriculture_doc)
    write_json_tab(os.path.join(NATURAL, "deposits.json"), deposits_doc)

    print("províncias com raster: %d | sem raster (centróide em água): %d"
          % (len(per_province), skipped_water))
    print("territórios agregados por província: %d | herdados da província mais próxima: %d"
          % (aggregated, inherited))
    print("territórios com dono: %d | sem dono: %s"
          % (owned, ",".join(unowned) if unowned else "nenhum"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
