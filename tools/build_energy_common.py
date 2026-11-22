"""Base comum dos construtores de dados de energia (Parte E do plano).

Reúne os carregadores e a réplica em Python da atribuição província -> cidade
do jogo (`GeographyService.assign_provinces`, core/economy/geography_service.gd).
O objetivo é que todos os `tools/build_energy_*.py` falem a MESMA unidade
geográfica da simulação: a província admin-1 do atlas atribuída a UMA cidade
jogável, com partição por área (Σ frações da província = 1).

Nada aqui é dado de energia: é só geografia/atribuição, reaproveitada.

Projeção: o mundo do jogo é equirretangular 5000x2500 (lon -180..180,
lat 90..-90). Províncias do atlas e cidades usam essas coordenadas.
"""

from __future__ import annotations

import json
import math
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFS = os.path.join(ROOT, "data", "definitions")
ATLAS = os.path.join(ROOT, "map", "map_data", "atlas")

CITIES_PATH = os.path.join(DEFS, "scenario", "cities.json")
NATIONS_PATH = os.path.join(DEFS, "scenario", "nations.json")
PROVINCES_PATH = os.path.join(ATLAS, "provinces.json")
PROVINCE_AREAS_PATH = os.path.join(DEFS, "natural", "province_areas.json")
PROVINCE_NATURE_PATH = os.path.join(DEFS, "natural", "province_nature.json")
WORLD_PROVINCE_PATH = os.path.join(ATLAS, "world_province.png")
WORLD_SDF_PATH = os.path.join(ATLAS, "world_sdf.png")

MAP_W = 5000.0
MAP_H = 2500.0

# Espelho de GeographyService.NATION_CODE_OVERRIDES / PROVINCE_TAG_OVERRIDES.
NATION_CODE_OVERRIDES = {"NED": "NLD", "XKX": "KOS", "SSD": "SDS"}
PROVINCE_TAG_OVERRIDES = {"XKX": "KOS", "SSD": "SDS"}
CITY_STATE_MIN_SHARE = 0.001


def read_json(path):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def write_json(path, data, indent=1):
    """JSON determinístico: indent fixo, LF, UTF-8 sem BOM, newline final."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    text = json.dumps(data, indent=indent, ensure_ascii=False, sort_keys=False) + "\n"
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)


def write_text(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)


def nation_code(tag):
    return NATION_CODE_OVERRIDES.get(tag, tag)


def province_tag(tag):
    return PROVINCE_TAG_OVERRIDES.get(tag, tag)


def load_registry():
    """tag do jogo -> tag de província do atlas (inverso de nation_for_province_tag)."""
    nations = read_json(NATIONS_PATH)
    tags = [str(n.get("id", "")) for n in nations if str(n.get("id", ""))]
    by_province_tag = {}
    for tag in tags:
        by_province_tag[province_tag(tag)] = tag
    return tags, by_province_tag


def load_cities():
    cities = read_json(CITIES_PATH)
    out = []
    for city in cities:
        out.append({
            "id": str(city.get("id", "")),
            "tag": str(city.get("nation", "")),
            "x": float(city.get("x", 0.0)),
            "y": float(city.get("y", 0.0)),
            "landmass": str(city.get("landmass", "")),
            "name": str(city.get("name", "")),
            "population": float(city.get("population") or 0.0),
            "capital": bool(city.get("capital", False)),
        })
    return out


def load_provinces():
    raw = read_json(PROVINCES_PATH)
    out = []
    for record in raw.get("provinces", []):
        pid = int(record.get("id", 0))
        if pid <= 0:
            continue
        out.append({
            "id": pid,
            "nation": str(record.get("nation", "")),
            "country": str(record.get("country", "")),
            "name": str(record.get("name", "")),
            "x": float(record.get("x", 0.0)),
            "y": float(record.get("y", 0.0)),
        })
    return out


def load_province_areas():
    areas = read_json(PROVINCE_AREAS_PATH).get("areas", {})
    return {int(k): float(v) for k, v in areas.items()}


def _dist2(a, b):
    dx = a[0] - b[0]
    dy = a[1] - b[1]
    return dx * dx + dy * dy


def _landmass_of(point, cities):
    best = ""
    best_distance = math.inf
    for city in cities:
        d = _dist2((city["x"], city["y"]), point)
        if d < best_distance:
            best_distance = d
            best = city["landmass"]
    return best


def _nearest_city(candidates, point, landmass):
    best_id = ""
    best_distance = math.inf
    for city in candidates:
        if landmass != "" and city["landmass"] != landmass:
            continue
        d = _dist2((city["x"], city["y"]), point)
        if d < best_distance:
            best_distance = d
            best_id = city["id"]
    return best_id


def _nearest_city_of_nation(city_groups, cities, tag, point):
    candidates = city_groups.get(tag, [])
    if not candidates:
        candidates = [c for c in cities if c["tag"] == tag]
        if not candidates:
            return ""
    landmass = _landmass_of(point, cities)
    candidate = _nearest_city(candidates, point, landmass)
    if candidate != "":
        return candidate
    return _nearest_city(candidates, point, "")


def _nearest_nation_province(provinces, tag, center, cities, landmass):
    nearest = None
    best_distance = math.inf
    atlas_tag = province_tag(tag)
    if atlas_tag == "":
        return None
    for province in provinces:
        if province["nation"] != atlas_tag:
            continue
        point = (province["x"], province["y"])
        if landmass != "" and _landmass_of(point, cities) != landmass:
            continue
        d = math.sqrt(_dist2(point, center))
        if d < best_distance:
            best_distance = d
            nearest = province
    return nearest


def _nearest_any_province(provinces, center, cities, landmass):
    nearest = None
    best_distance = math.inf
    for province in provinces:
        point = (province["x"], province["y"])
        if landmass != "" and _landmass_of(point, cities) != landmass:
            continue
        d = math.sqrt(_dist2(point, center))
        if d < best_distance:
            best_distance = d
            nearest = province
    return nearest


def assign_provinces(cities=None, provinces=None):
    """Réplica de GeographyService.assign_provinces.

    Devolve (catchment, shares):
      catchment: {city_id: [province_id, ...]}
      shares:    {city_id: {province_id: fração}}
    A soma das frações de uma província é 1 quando há dono; províncias de nação
    não jogável ficam sem dono (como no jogo).
    """
    if cities is None:
        cities = load_cities()
    if provinces is None:
        provinces = load_provinces()
    _, nation_by_province_tag = load_registry()

    city_groups = {}
    for city in cities:
        city_groups.setdefault(city["tag"], []).append(city)

    catchment = {}
    for province in provinces:
        tag = nation_by_province_tag.get(province["nation"], "")
        if tag == "":
            continue
        point = (province["x"], province["y"])
        city_id = _nearest_city_of_nation(city_groups, cities, tag, point)
        if city_id == "":
            continue
        catchment.setdefault(city_id, []).append(province["id"])

    shares = {}
    # _share_starved_provinces: cidade jogável sem nenhuma província recebe uma
    # fração da província da nação mais próxima (nunca fica sem terra).
    by_nation = {}
    for city in cities:
        by_nation.setdefault(city["tag"], []).append(city)
    province_by_id = {p["id"]: p for p in provinces}
    for _tag, peers in by_nation.items():
        for city in peers:
            if catchment.get(city["id"]):
                continue
            _share_nearest_province(
                city, peers, cities, catchment, shares, provinces,
                province_by_id, city_groups, nation_by_province_tag)
    return catchment, shares


def _share_nearest_province(city, _peers, all_cities, catchment, shares,
                            provinces, province_by_id, city_groups,
                            nation_by_province_tag):
    tag = city["tag"]
    center = (city["x"], city["y"])
    landmass = city["landmass"]
    nearest = _nearest_nation_province(provinces, tag, center, all_cities, landmass)
    if nearest is None:
        nearest = _nearest_nation_province(provinces, tag, center, all_cities, "")
    city_state = nearest is None
    if nearest is None:
        nearest = _nearest_any_province(provinces, center, all_cities, landmass)
    if nearest is None:
        nearest = _nearest_any_province(provinces, center, all_cities, "")
    if nearest is None:
        return
    pid = nearest["id"]
    point = (nearest["x"], nearest["y"])
    owner_tag = nation_by_province_tag.get(nearest["nation"], "")
    owner_id = city["id"] if owner_tag == "" else _nearest_city(
        city_groups.get(owner_tag, []), point, "")
    if owner_id == city["id"]:
        return
    if owner_id == "":
        shares.setdefault(city["id"], {})[pid] = CITY_STATE_MIN_SHARE
        catchment.setdefault(city["id"], []).append(pid)
        return
    owner = next((c for c in all_cities if c["id"] == owner_id), None)
    if owner is None:
        return
    owner_distance = math.sqrt(_dist2((owner["x"], owner["y"]), point))
    city_distance = math.sqrt(_dist2(center, point))
    total = city_distance + owner_distance
    city_share = owner_distance / total if total > 0.0 else 0.5
    if city_state:
        city_share = max(city_share, CITY_STATE_MIN_SHARE)
    owner_shares = shares.get(owner_id, {})
    owner_share = float(owner_shares.get(pid, 1.0))
    city_share = min(city_share, owner_share * 0.5)
    if city_share <= 0.0:
        return
    owner_shares[pid] = owner_share - city_share
    shares[owner_id] = owner_shares
    city_shares = shares.get(city["id"], {})
    city_shares[pid] = city_share
    shares[city["id"]] = city_shares
    catchment.setdefault(city["id"], []).append(pid)


def province_city_index(cities=None, provinces=None):
    """{province_id: (city_id, fração)} — o dono de cada província."""
    if cities is None:
        cities = load_cities()
    if provinces is None:
        provinces = load_provinces()
    catchment, shares = assign_provinces(cities, provinces)
    owner = {}
    for city_id, pids in catchment.items():
        for pid in pids:
            owner[pid] = (city_id, float(shares.get(city_id, {}).get(pid, 1.0)))
    return owner


def city_provinces(cities=None, provinces=None):
    """{city_id: [(province_id, fração), ...]} na ordem do atlas."""
    if cities is None:
        cities = load_cities()
    if provinces is None:
        provinces = load_provinces()
    catchment, shares = assign_provinces(cities, provinces)
    out = {}
    for city_id, pids in catchment.items():
        out[city_id] = [(pid, float(shares.get(city_id, {}).get(pid, 1.0)))
                        for pid in pids]
    return out


# --- Conversões de coordenada ------------------------------------------------

def lonlat_to_map(lon, lat):
    """Coordenada geográfica -> pixel do mapa do jogo (5000x2500)."""
    return ((float(lon) + 180.0) / 360.0 * MAP_W,
            (90.0 - float(lat)) / 180.0 * MAP_H)


def map_to_lonlat(x, y):
    return (float(x) / MAP_W * 360.0 - 180.0,
            90.0 - float(y) / MAP_H * 180.0)


def load_world_province_ids():
    """Raster 8192x4096 de ids de província (id = R + G*256)."""
    import numpy as np
    from PIL import Image
    Image.MAX_IMAGE_PIXELS = None
    image = Image.open(WORLD_PROVINCE_PATH)
    array = np.asarray(image)
    return array[:, :, 0].astype(np.int32) + array[:, :, 1].astype(np.int32) * 256


def load_world_land_mask():
    """Terra = R > 128 no world_sdf.png (128 + d*2 texels; > 128 é terra)."""
    import numpy as np
    from PIL import Image
    Image.MAX_IMAGE_PIXELS = None
    image = Image.open(WORLD_SDF_PATH)
    array = np.asarray(image)
    return array[:, :, 0] > 128


def raster_province_area_km2():
    """Área (km²) de cada província contando pixels de TERRA do raster do atlas.

    Deriva a área da MESMA fonte que define o dono da província
    (world_province.png + world_sdf.png), em vez de confiar no Voronoi de
    province_areas.json — que tem células degeneradas (ex.: "Tasmania 1" com
    1,78 milhão de km²). Cada pixel vale, na latitude da linha,
    (360/W)*111,32*cos(lat) km por (180/H)*110,574 km.
    """
    import numpy as np
    ids = load_world_province_ids()
    land = load_world_land_mask()
    height, width = ids.shape
    step_lon = 360.0 / width
    step_lat = 180.0 / height
    size = int(ids.max()) + 1
    areas = np.zeros(size, dtype=np.float64)
    for y in range(height):
        row = ids[y]
        mask = land[y]
        values = row[mask]
        if values.size == 0:
            continue
        lat = 90.0 - (y + 0.5) * step_lat
        pixel_km2 = (step_lon * 111.32 * math.cos(math.radians(lat))
                     * step_lat * 110.574)
        areas += np.bincount(values, minlength=size) * pixel_km2
    return {int(i): float(v) for i, v in enumerate(areas) if v > 0.0}
