"""Adiciona Bahrein (BHR) e Catar (QAT) à base de 737 territórios.

A base legada de 737 territórios (nature.json/agriculture.json do HEAD) não tem
territórios para BHR e QAT: a península do Catar e o arquipélago do Bahrein
caíam dentro do território vizinho (KWT/SAU) por centróide mais próximo. Este
passo cria 1 território para cada um e reatribui os depósitos do Golfo aos seus
países reais.

Idempotente — roda ANTES de tools/apply_deposit_supplement.py na cadeia.

1. Cria 1 território BHR e 1 QAT a partir das províncias desses países em
   `map/map_data/atlas/provinces.json` (centróide ponderado por área). Clima e
   aptidões pelo MESMO método do apply_real_climate (WorldClim 2.1 10-min no
   centróide + fórmulas de build_natural_geography), para esses dois pontos.
2. Reatribui depósitos cujo país real (province raster world_province.png) é
   QAT ou BHR ao território novo — cobre o North Field (Catar) e qualquer
   registro MRDS nesses países, "removendo" a área dos vizinhos por
   reatribuição (não há polígono por território em nature.json: a fronteira é
   o centróide mais próximo, então o centróide novo já recorta a célula).
3. Corrige Ghawar -> SAU (na base legada ficava em KWT).

Ids novos: continua após o maior TRT existente (não colidem). Depois rode
apply_deposit_supplement.py (Khuff/Awali -> BHR) e calibrate_reserves.py.

Uso: python tools/add_gulf_territories.py
"""

import json
import os
import sys

import tifffile
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_natural_geography as bng  # noqa: E402

NATURAL = os.path.join(ROOT, "data", "definitions", "natural")
NATURE = os.path.join(NATURAL, "nature.json")
AGRICULTURE = os.path.join(NATURAL, "agriculture.json")
DEPOSITS = os.path.join(NATURAL, "deposits.json")
PROVINCES = os.path.join(ROOT, "map", "map_data", "atlas", "provinces.json")
WORLD_PROVINCE = os.path.join(ROOT, "map", "map_data", "atlas", "world_province.png")
WC = os.path.join(ROOT, "tools", "data", "worldclim")

MAP_W, MAP_H = 5000.0, 2500.0
CELL = 0.16666666666666666  # 10 arc-min

GULF = {
    "BHR": {"name": "Bahrain", "name_pt": "Bahrein",
            "name_en": "Bahrain", "name_es": "Baréin"},
    "QAT": {"name": "Qatar", "name_pt": "Catar",
            "name_en": "Qatar", "name_es": "Catar"},
}


def read_json(path):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def write_json_tab(path, data):
    """Formato dos naturais (indent=tab, CRLF, LF final) — igual a apply_real_climate."""
    text = json.dumps(data, indent="\t", ensure_ascii=False).replace("\n", "\r\n") + "\r\n"
    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write(text)


def write_json_1(path, data):
    """Formato de deposits.json (indent=1, CRLF) — igual a apply_deposit_supplement."""
    text = json.dumps(data, indent=1, ensure_ascii=False).replace("\n", "\r\n")
    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write(text)


def load_worldclim():
    elev = tifffile.imread(os.path.join(WC, "wc2.1_10m_elev.tif"))
    bio1 = tifffile.imread(os.path.join(WC, "wc2.1_10m_bio_1.tif"))
    bio10 = tifffile.imread(os.path.join(WC, "wc2.1_10m_bio_10.tif"))
    bio11 = tifffile.imread(os.path.join(WC, "wc2.1_10m_bio_11.tif"))
    bio12 = tifffile.imread(os.path.join(WC, "wc2.1_10m_bio_12.tif"))
    return elev, bio1, bio10, bio11, bio12


def sample_worldclim(rasters, lon, lat):
    elev, bio1, bio10, bio11, bio12 = rasters
    col = int(round((lon + 180.0) / CELL))
    row = int(round((90.0 - lat) / CELL))
    h, w = elev.shape
    col = max(0, min(w - 1, col))
    row = max(0, min(h - 1, row))
    return (elev[row, col], bio1[row, col], bio10[row, col],
            bio11[row, col], bio12[row, col])


def province_by_country():
    with open(PROVINCES, encoding="utf-8") as handle:
        data = json.load(handle)
    by_country = {}
    for province in data.get("provinces", []):
        by_country.setdefault(str(province.get("country", "")), []).append(province)
    return by_country


def centroid_of(provinces):
    total = sum(float(p["area_global"]) for p in provinces)
    cx = sum(float(p["x"]) * float(p["area_global"]) for p in provinces) / total
    cy = sum(float(p["y"]) * float(p["area_global"]) for p in provinces) / total
    return cx, cy


def make_territory(code, provinces, rasters, names):
    """Território novo + aptidões, pelo método do apply_real_climate no centróide."""
    cx, cy = centroid_of(provinces)
    lon, lat = bng.pixel_to_lonlat(cx, cy)
    altitude_raw, temp_raw, warm_raw, cold_raw, precip_raw = sample_worldclim(
        rasters, lon, lat)
    if altitude_raw < -1000 or temp_raw < -100.0:
        raise SystemExit("%s: centróide em água no WorldClim (lon %.2f lat %.2f)"
                         % (code, lon, lat))

    altitude = float(altitude_raw)
    temperature = float(temp_raw)
    warm_temp = float(warm_raw)
    cold_temp = float(cold_raw)
    precip = float(precip_raw)

    zone = bng.zone_for_latitude(lat)
    aridity = bng.aridity_from_climate(temperature, precip)
    climate = bng.climate_label(zone, aridity)
    soil_type, fertility = bng.soil_for(zone, aridity)
    coastal = True  # ilha (BHR) e península (QAT) — litoral por definição
    water = bng.clamp(0.12 + precip / 1500.0 + (0.08 if coastal else 0.0), 0.0, 1.0)
    if fertility >= 0.7 and water >= 0.6:
        soil_type, fertility = "fluvisol", 0.88
    forest = bng.forest_for(zone, aridity, altitude, fertility)
    relief = "mountain" if altitude >= 1500.0 else "hills" if altitude >= 500.0 else "plain"
    relief_penalty = 0.7 if relief == "mountain" else 0.3 if relief == "hills" else 0.0
    cultivable = bng.clamp(
        fertility * water * (1.0 - 0.6 * forest) * (1.0 - relief_penalty), 0.0, 1.0)

    # O Golfo Pérsico é bacia sedimentar (é o motivo do petróleo/gás); o box
    # "Zagros" de build_natural_geography (lat >= 26) pegaria o Bahrein como
    # dobra de montanha por estar no limite — força o valor honesto.
    geology = "sedimentary_basin"

    entry = {
        "owner": "NNN",
        "provinces": len(provinces),
        "centroid_px": [round(cx, 1), round(cy, 1)],
        "climate": climate,
        "soil_type": soil_type,
        "relief": relief,
        "geology": geology,
        "coastal": coastal,
        "data_quality": "real_climate",
        "temp_c": round(temperature, 1),
        "precip_mm": round(precip),
        "fertility": round(fertility, 3),
        "water": round(water, 3),
        "altitude_m": round(altitude),
        "forest_cover": round(forest, 3),
        "fishing": round(bng.fishing_for(lon, lat, coastal), 3),
        "cultivable": round(cultivable, 3),
        "lon": round(lon, 2),
        "lat": round(lat, 2),
        "name": names["name"],
        "name_pt": names["name_pt"],
        "name_en": names["name_en"],
        "name_es": names["name_es"],
        "adm0_a3": code,
    }
    agriculture = {}
    for crop in bng.CROPS:
        agriculture[crop] = bng.crop_suitability_seasonal(
            crop, temperature, warm_temp, cold_temp, precip, fertility, altitude)
    return entry, agriculture


def next_territory_ids(nature):
    used = set()
    for tid in nature:
        if tid.startswith("TRT") and tid[3:].isdigit():
            used.add(int(tid[3:]))
    return {code: "TRT%06d" % (max(used) + 1 + i)
            for i, code in enumerate(sorted(GULF))}


def load_province_raster():
    return Image.open(WORLD_PROVINCE).convert("RGB")


def load_province_country():
    with open(PROVINCES, encoding="utf-8") as handle:
        data = json.load(handle)
    return {int(p["id"]): str(p.get("country", ""))
            for p in data.get("provinces", [])}


def true_country(raster, province_country, px, py):
    """País (adm0_a3) do ponto, via província raster (mar preenchido com a
    província de terra mais próxima)."""
    rx = int(px / MAP_W * raster.width)
    ry = int(py / MAP_H * raster.height)
    if not (0 <= rx < raster.width and 0 <= ry < raster.height):
        return ""
    r, g, _b = raster.getpixel((rx, ry))
    province_id = int(round(r + g * 256))
    return province_country.get(province_id, "")


def nearest_territory(nature, territory_ids, px, py):
    best, best_dist = None, float("inf")
    for tid in territory_ids:
        centroid = nature.get(tid, {}).get("centroid_px", [])
        if len(centroid) < 2:
            continue
        dist = (float(centroid[0]) - px) ** 2 + (float(centroid[1]) - py) ** 2
        if dist < best_dist:
            best_dist, best = dist, tid
    return best


def sync_north_field(deposits):
    """North Field (Catar): a base legada o trazia curado em lon 51.5, ~95 km do
    Khuff/Awali (BHR) — abaixo do piso de 120 km entre marcadores do MESMO
    recurso em build_resource_map.py, o que barraria o marcador de gás do
    Bahrein. Sincroniza com o suplemento (lon 51.9, ~135 km do Khuff), a fonte
    pública mais recente (Energy Institute/QatarEnergy)."""
    changed = 0
    for deposit in deposits:
        if any("North Field" in n for n in deposit.get("names", [])):
            px, py = bng.lonlat_to_pixel(51.9, 26.0)
            deposit["lon"] = 51.9
            deposit["lat"] = 26.0
            deposit["px"] = round(px, 1)
            deposit["py"] = round(py, 1)
            changed += 1
    return changed


def reassign_deposits(deposits, nature, country_territories, raster, province_country):
    """Reatribui depósitos de QAT/BHR ao território novo e corrige Ghawar -> SAU."""
    reassigned = 0
    for deposit in deposits:
        if deposit.get("facility"):
            continue
        px = float(deposit.get("px", 0.0))
        py = float(deposit.get("py", 0.0))
        country = true_country(raster, province_country, px, py)
        target = None
        if country in ("QAT", "BHR"):
            target = nearest_territory(
                nature, country_territories.get(country, []), px, py)
        elif country == "SAU" and any("Ghawar" in n for n in deposit.get("names", [])):
            # Ghawar é o maior campo da Arábia Saudita; a base legada o atribuiu
            # a KWT (centróide mais próximo). Corrige explicitamente.
            target = nearest_territory(
                nature, country_territories.get("SAU", []), px, py)
        if target and str(deposit.get("territory", "")) != target:
            deposit["territory"] = target
            reassigned += 1
    return reassigned


def main():
    nature = read_json(NATURE)
    agriculture = read_json(AGRICULTURE)
    deposits = read_json(DEPOSITS)
    territories = nature["territories"]
    ag_territories = agriculture["territories"]

    by_country = province_by_country()
    rasters = load_worldclim()

    existing = {str(e.get("adm0_a3", "")) for e in territories.values()}
    new_ids = next_territory_ids(territories)

    created = []
    new_created = 0
    for code in sorted(GULF):
        if code in existing:
            # Já existe (idempotência): reusa o id atual.
            created.append((code, "existente"))
            continue
        provinces = by_country.get(code)
        if not provinces:
            raise SystemExit("%s sem províncias em provinces.json" % code)
        entry, ag = make_territory(code, provinces, rasters, GULF[code])
        tid = new_ids[code]
        territories[tid] = entry
        ag_territories[tid] = ag
        created.append((code, tid))
        new_created += 1

    # Reatribui depósitos (North Field -> QAT, Ghawar -> SAU, MRDS em QAT/BHR).
    country_territories = {}
    for tid, entry in territories.items():
        country_territories.setdefault(str(entry.get("adm0_a3", "")), []).append(tid)
    raster = load_province_raster()
    province_country = load_province_country()
    reassigned = reassign_deposits(
        deposits["deposits"], territories, country_territories,
        raster, province_country)
    synced = sync_north_field(deposits["deposits"])

    if new_created:
        write_json_tab(NATURE, nature)
        write_json_tab(AGRICULTURE, agriculture)
    if reassigned or synced:
        write_json_1(DEPOSITS, deposits)

    for code, tid in created:
        print("território %s -> %s" % (code, tid))
    print("depósitos reatribuídos:", reassigned, "| North Field sincronizado:", synced)


if __name__ == "__main__":
    main()
