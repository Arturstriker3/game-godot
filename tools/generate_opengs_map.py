#!/usr/bin/env python3
"""Gera o mapa do opengs em projeção cilíndrica (equirretangular), sem Antártida
e sem ilhotas, com resolução maior que o demo.

Saída no formato opengs:
  - map/map_data/provinces.png  (cor-chave por província)
  - map/map_data/ne2.png        (terreno/satélite)
  - map/map_data/height.png     (elevação plana)
  - map/map_data/normal.png     (normal plana)
  - data/definitions/provinces.txt
  - data/definitions/territories.txt
  - data/definitions/countries.txt
  - data/history/provinces/provinces.json
  - data/history/territories/territories.json
  - data/history/countries/*.json

Fontes (domínio público):
  - ne_10m_admin_0_countries        -> fronteiras dos países
  - ne_10m_populated_places_simple  -> densidade populacional (Voronoi)
  - NASA GIBS Blue Marble           -> textura de terreno
"""

import colorsys
import json
import math
import os
import sys
import urllib.request

import numpy as np
import shapefile
from PIL import Image, ImageDraw
from scipy.ndimage import distance_transform_edt, gaussian_filter, label as ndlabel
from shapely.geometry import Point, Polygon
from shapely.ops import unary_union
from shapely.strtree import STRtree

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NE_DIR = os.environ.get("NE_DIR", r"G:\tools\ne_extract")
OUT_DIR = os.path.join(ROOT)
MAP_DATA = os.path.join(OUT_DIR, "map", "map_data")
DEFS = os.path.join(OUT_DIR, "data", "definitions")
HIST = os.path.join(OUT_DIR, "data", "history")

# Projeção cilíndrica sem a Antártida (lat -60..84).
WIDTH = 8192
LAT_MIN = -60.0
LAT_MAX = 84.0
HEIGHT = int(round(WIDTH * (LAT_MAX - LAT_MIN) / 360.0))

TARGET_FACTOR = 2.4
TARGET_MIN = 3
TARGET_MAX = 100
MIN_PROVINCE_PIXELS = 140
SMALL_ISLAND_PIXELS = 500
DENSITY_SIGMA = 7.0
DENSITY_BASE = 0.12
SEED = 2026

ADMIN0_SHP = os.path.join(NE_DIR, "ne_10m_admin_0_countries", "ne_10m_admin_0_countries")
PLACES_GEOJSON = os.environ.get(
    "PLACES_GEOJSON",
    r"g:\projetos\war-economy\data\geo\ne_10m_populated_places_simple.geojson",
)
BACKGROUND_CACHE = os.environ.get(
    "BACKGROUND_CACHE",
    r"G:\tools\ne_downloads\blue_marble_crop.jpg",
)
BACKGROUND_URL = (
    "https://gibs.earthdata.nasa.gov/wms/epsg4326/best/wms.cgi"
    "?SERVICE=WMS&REQUEST=GetMap&VERSION=1.1.1"
    "&LAYERS=BlueMarble_ShadedRelief_Bathymetry"
    "&BBOX=-180,{lat_min},180,{lat_max}&WIDTH={w}&HEIGHT={h}"
    "&FORMAT=image/jpeg&SRS=EPSG:4326"
).format(lat_min=LAT_MIN, lat_max=LAT_MAX, w=WIDTH, h=HEIGHT)

SEA_LEVEL = 0x87  # 135: mesmo referencial do shader (0x87/0xFF).


# --------------------------------------------------------------------------- #
# Geometria
# --------------------------------------------------------------------------- #

def ring_area(ring):
    points = np.asarray(ring, dtype=np.float64)
    if points.shape[0] < 3:
        return 0.0
    x = points[:, 0]
    y = points[:, 1]
    return 0.5 * float(np.dot(x, np.roll(y, -1)) - np.dot(np.roll(x, -1), y))


def split_exterior_holes(parts):
    rings = [part for part in parts if len(part) >= 3]
    if not rings:
        return []
    largest = max(rings, key=lambda ring: abs(ring_area(ring)))
    exterior_sign = 1.0 if ring_area(largest) >= 0 else -1.0
    exteriors = []
    for ring in rings:
        signed = ring_area(ring)
        if signed == 0.0:
            continue
        if (signed > 0) == (exterior_sign > 0):
            exteriors.append(ring)
    return exteriors


def unwrap_and_project(ring):
    points = np.asarray(ring, dtype=np.float64)
    lons = points[:, 0]
    lats = points[:, 1]
    deltas = np.diff(lons)
    corrections = np.where(deltas > 180.0, -360.0, np.where(deltas < -180.0, 360.0, 0.0))
    offsets = np.concatenate(([0.0], np.cumsum(corrections)))
    xs = (lons + offsets + 180.0) / 360.0 * WIDTH
    ys = (LAT_MAX - lats) / (LAT_MAX - LAT_MIN) * HEIGHT
    return np.column_stack((xs, ys))


def draw_rings(draw, rings, fill):
    for ring in rings:
        points = unwrap_and_project(ring)
        if points.shape[0] < 3:
            continue
        x_min = float(points[:, 0].min())
        x_max = float(points[:, 0].max())
        for shift in (-WIDTH, 0, WIDTH):
            if x_max + shift < 0 or x_min + shift > WIDTH:
                continue
            shifted = points.copy()
            shifted[:, 0] += shift
            draw.polygon(shifted.tolist(), fill=fill)


def read_shapes(path):
    reader = shapefile.Reader(path, encoding="utf-8")
    field_names = [f[0] for f in reader.fields[1:]]
    records = []
    for shape, record in zip(reader.shapes(), reader.records()):
        parts = []
        for begin, end in zip(shape.parts, list(shape.parts[1:]) + [len(shape.points)]):
            parts.append(np.array(shape.points[begin:end], dtype=np.float64))
        records.append((parts, dict(zip(field_names, record))))
    return records


def build_geometry(exteriors):
    polygons = []
    for ring in exteriors:
        if len(ring) < 4:
            continue
        try:
            polygons.append(Polygon(ring))
        except ValueError:
            continue
    if not polygons:
        return None
    geometry = unary_union(polygons)
    if geometry.is_empty:
        return None
    if not geometry.is_valid:
        geometry = geometry.buffer(0)
    return geometry


# --------------------------------------------------------------------------- #
# Cores
# --------------------------------------------------------------------------- #

def hash_string(value):
    hashed = 2166136261
    for char in value.encode("utf-8"):
        hashed ^= char
        hashed = (hashed * 16777619) & 0xFFFFFFFF
    return hashed


def generate_key_colors(count):
    colors = []
    used = {(0, 0, 0)}
    index = 0
    while len(colors) < count:
        hue = (index * 0.618033988749895) % 1.0
        saturation = 0.45 + 0.55 * (((index * 37) % 211) / 210.0)
        value = 0.45 + 0.55 * (((index * 89) % 223) / 222.0)
        r, g, b = colorsys.hsv_to_rgb(hue, saturation, value)
        key = (int(r * 255), int(g * 255), int(b * 255))
        if key not in used:
            used.add(key)
            colors.append(key)
        index += 1
    return colors


def country_color(tag, mapcolor13):
    try:
        family = (int(mapcolor13) - 1) % 13
    except (TypeError, ValueError):
        family = hash_string(tag) % 13
    hashed = hash_string(tag)
    hue_shift = ((hashed & 0xFF) / 255.0 - 0.5) * 0.05
    saturation = 0.42 + ((hashed >> 8) & 0xFF) / 255.0 * 0.34
    value = 0.60 + ((hashed >> 16) & 0xFF) / 255.0 * 0.32
    hue = (family / 13.0 + hue_shift) % 1.0
    r, g, b = colorsys.hsv_to_rgb(hue, saturation, value)
    return (int(r * 255), int(g * 255), int(b * 255))


# --------------------------------------------------------------------------- #
# Entrada
# --------------------------------------------------------------------------- #

def collect_countries():
    grouped = {}
    for parts, props in read_shapes(ADMIN0_SHP):
        code = str(props.get("ADM0_A3", "")).strip()
        if code in ("", "-99", "NONE"):
            code = str(props.get("NAME", "UNK")).strip()
        entry = grouped.setdefault(code, {
            "code": code,
            "name": str(props.get("ADMIN") or props.get("NAME") or code).strip(),
            "mapcolor13": props.get("MAPCOLOR13"),
            "pop": float(props.get("POP_EST") or 0),
            "exteriors": [],
        })
        entry["exteriors"].extend(split_exterior_holes(parts))
    return grouped


def read_places():
    try:
        with open(PLACES_GEOJSON, "r", encoding="utf-8") as handle:
            data = json.load(handle)
    except OSError:
        print("Aviso: cidades não encontradas; densidade uniforme.", file=sys.stderr)
        return []
    places = []
    for feature in data["features"]:
        props = feature["properties"]
        coordinates = feature["geometry"]["coordinates"]
        places.append((
            float(coordinates[0]),
            float(coordinates[1]),
            float(props.get("pop_max") or 0),
        ))
    return places


def build_density(places):
    points = np.zeros((HEIGHT, WIDTH), dtype=np.float32)
    for lon, lat, population in places:
        if population <= 0:
            continue
        x = int((lon + 180.0) / 360.0 * WIDTH)
        y = int((LAT_MAX - lat) / (LAT_MAX - LAT_MIN) * HEIGHT)
        if 0 <= x < WIDTH and 0 <= y < HEIGHT:
            points[y, x] += math.log10(1.0 + population)
    density = gaussian_filter(points, DENSITY_SIGMA)
    peak = float(density.max())
    if peak > 0.0:
        density /= peak
    return density + DENSITY_BASE


def ensure_background():
    if not os.path.exists(BACKGROUND_CACHE):
        os.makedirs(os.path.dirname(BACKGROUND_CACHE), exist_ok=True)
        print("Baixando imagem de satélite (NASA GIBS)...")
        urllib.request.urlretrieve(BACKGROUND_URL, BACKGROUND_CACHE)
    image = Image.open(BACKGROUND_CACHE).convert("RGB")
    if image.size != (WIDTH, HEIGHT):
        image = image.resize((WIDTH, HEIGHT), Image.LANCZOS)
    return image


# --------------------------------------------------------------------------- #
# Geração
# --------------------------------------------------------------------------- #

def province_target(code, pop):
    millions = max(pop, 0.0) / 1_000_000.0
    if millions <= 0.0:
        return TARGET_MIN
    return max(TARGET_MIN, min(TARGET_MAX, round(TARGET_FACTOR * math.sqrt(millions))))


def rasterize_countries(countries):
    image = Image.new("I", (WIDTH, HEIGHT), 0)
    draw = ImageDraw.Draw(image)
    areas = {
        code: sum(abs(ring_area(ring)) for ring in info["exteriors"])
        for code, info in countries.items()
    }
    order = sorted(countries, key=lambda code: areas[code], reverse=True)
    for index, code in enumerate(order, start=1):
        countries[code]["cid"] = index
        draw_rings(draw, countries[code]["exteriors"], index)
    return np.array(image), [code for code in order if areas[code] > 0]


def remove_small_islands(country_map):
    """Remove ilhotas: componentes de terra pequenos viram oceano (0)."""
    land = country_map > 0
    components, count = ndlabel(land)
    sizes = np.bincount(components.ravel())
    small = np.zeros(count + 1, dtype=bool)
    small[1:] = sizes[1:] < SMALL_ISLAND_PIXELS
    to_remove = small[components] & land
    if to_remove.any():
        country_map = country_map.copy()
        country_map[to_remove] = 0
    return country_map


def voronoi_block(mask, density_grid, count, rng, first_id):
    rows, cols = np.nonzero(mask)
    weights = density_grid[mask].astype(np.float64)
    total_weight = float(weights.sum())
    if total_weight <= 0.0:
        weights = np.ones_like(weights)
        total_weight = float(weights.size)

    count = max(1, min(count, int(weights.size)))
    chosen = rng.choice(weights.size, size=count, replace=False, p=weights / total_weight)

    seed_rows = rows[chosen]
    seed_cols = cols[chosen]
    ids = np.arange(first_id, first_id + count, dtype=np.int32)

    seed_mask = np.zeros(mask.shape, dtype=bool)
    seed_mask[seed_rows, seed_cols] = True
    seed_ids = np.zeros(mask.shape, dtype=np.int32)
    seed_ids[seed_rows, seed_cols] = ids

    _, indices = distance_transform_edt(~seed_mask, return_indices=True)
    assigned = seed_ids[indices[0], indices[1]]
    return assigned, seed_rows, seed_cols, ids


def generate_provinces(country_map, countries, codes, density):
    rng = np.random.default_rng(SEED)
    province_map = np.zeros((HEIGHT, WIDTH), dtype=np.int32)
    provinces = []
    next_id = 1

    for code in codes:
        info = countries[code]
        country_mask = country_map == info["cid"]
        pixel_count = int(country_mask.sum())
        if pixel_count == 0:
            continue

        rows, cols = np.nonzero(country_mask)
        top, bottom = int(rows.min()), int(rows.max()) + 1
        left, right = int(cols.min()), int(cols.max()) + 1

        sub_country = country_mask[top:bottom, left:right]
        sub_density = density[top:bottom, left:right]

        count = province_target(code, info["pop"])
        count = min(count, max(1, pixel_count // MIN_PROVINCE_PIXELS))

        assigned, seed_rows, seed_cols, ids = voronoi_block(
            sub_country, sub_density, count, rng, next_id
        )

        block = province_map[top:bottom, left:right]
        block[sub_country] = assigned[sub_country]
        province_map[top:bottom, left:right] = block

        for province_id, seed_row, seed_col in zip(ids, seed_rows, seed_cols):
            provinces.append({
                "id": int(province_id),
                "owner": code,
                "py": int(seed_row) + top,
                "px": int(seed_col) + left,
            })
        next_id += count

    return province_map, provinces


def fill_cracks(province_map):
    sea = province_map == 0
    if not sea.any():
        return province_map
    mask_image = Image.fromarray(np.where(sea, 255, 0).astype(np.uint8), "L").copy()
    ImageDraw.floodfill(mask_image, (0, 0), 128)
    ocean = np.asarray(mask_image) == 128
    enclosed = sea & ~ocean
    if not enclosed.any():
        return province_map
    components, count = ndlabel(enclosed)
    sizes = np.bincount(components.ravel())
    small = np.zeros(count + 1, dtype=bool)
    small[1:] = sizes[1:] < 3000
    cracks = small[components] & enclosed
    if not cracks.any():
        return province_map
    _, nearest = distance_transform_edt(province_map == 0, return_indices=True)
    province_map[cracks] = province_map[nearest[0], nearest[1]][cracks]
    return province_map


# --------------------------------------------------------------------------- #
# Saída (formato opengs)
# --------------------------------------------------------------------------- #

def write_provinces_txt(path, provinces):
    with open(path, "w", encoding="utf-8") as handle:
        for province in provinces:
            r, g, b = province["key"]
            handle.write(
                f"{province['id_str']};land;{r};{g};{b};"
                f"{province['px']};{province['py']};PLAINS\n"
            )


def write_countries_txt(path, countries, provinces):
    owners = {province["owner"] for province in provinces}
    with open(path, "w", encoding="utf-8") as handle:
        for code in sorted(owners):
            handle.write(f"{code}\n")


def write_country_history(path, countries, provinces):
    owners = {province["owner"] for province in provinces}
    for code in sorted(owners):
        info = countries.get(code, {"name": code, "mapcolor13": None})
        r, g, b = country_color(code, info.get("mapcolor13"))
        data = {
            "map_color": [round(r / 255.0, 4), round(g / 255.0, 4), round(b / 255.0, 4)],
            "base_name": str(info.get("name") or code),
            "ideology": "DEMOCRACY",
        }
        with open(os.path.join(path, code + ".json"), "w", encoding="utf-8") as handle:
            json.dump(data, handle, indent="\t")
            handle.write("\n")


def write_province_history(path, provinces):
    data = {}
    for province in provinces:
        data[province["id_str"]] = {
            "province_owner": province["owner"],
            "province_controller": province["owner"],
        }
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(data, handle, indent="\t")
        handle.write("\n")


def write_territories(territories_txt, territories_json, provinces):
    # Um território por país (estado = nação, no primeiro passo).
    grouped = {}
    for province in provinces:
        grouped.setdefault(province["owner"], []).append(province)

    with open(territories_txt, "w", encoding="utf-8") as handle:
        for index, (owner, group) in enumerate(sorted(grouped.items()), start=1):
            tid = f"TRT{index:06d}"
            first = group[0]
            r, g, b = (0, 0, 0)
            handle.write(f"{tid};land;{r};{g};{b};{first['px']};{first['py']}\n")

    territory_json = {}
    for index, (owner, group) in enumerate(sorted(grouped.items()), start=1):
        tid = f"TRT{index:06d}"
        territory_json[tid] = {"provinces": [p["id_str"] for p in group]}

    with open(territories_json, "w", encoding="utf-8") as handle:
        json.dump(territory_json, handle, indent="\t")
        handle.write("\n")


def main():
    for required in (ADMIN0_SHP + ".shp",):
        if not os.path.exists(required):
            print(f"Fonte ausente: {required}", file=sys.stderr)
            return 1

    os.makedirs(MAP_DATA, exist_ok=True)
    os.makedirs(DEFS, exist_ok=True)
    os.makedirs(os.path.join(HIST, "countries"), exist_ok=True)

    print(f"Grid: {WIDTH}x{HEIGHT} | lat {LAT_MIN}..{LAT_MAX}")

    print("Lendo países...")
    countries = collect_countries()
    country_map, codes = rasterize_countries(countries)

    print("Removendo ilhotas...")
    country_map = remove_small_islands(country_map)
    # Reaproveita a ordem dos códigos que ainda têm pixels.
    codes = [code for code in codes if int((country_map == countries[code]["cid"]).sum()) > 0]

    print("Calculando densidade populacional...")
    density = build_density(read_places())

    print("Gerando províncias (Voronoi por densidade)...")
    province_map, provinces = generate_provinces(country_map, countries, codes, density)
    province_map = fill_cracks(province_map)

    # Remove províncias sem pixels e compacta a numeração.
    survivors = []
    for province in provinces:
        if int((province_map == province["id"]).sum()) > 0:
            survivors.append(province)
    remap = np.zeros(int(province_map.max()) + 1, dtype=np.int32)
    for new_id, province in enumerate(survivors, start=1):
        remap[province["id"]] = new_id
        province["id"] = new_id
        province["id_str"] = f"PRV{new_id:06d}"
    province_map = remap[province_map]

    palette = np.zeros((len(survivors) + 1, 3), dtype=np.uint8)
    for province, key in zip(survivors, generate_key_colors(len(survivors))):
        province["key"] = key
        palette[province["id"]] = key

    print("Escrevendo saídas...")
    Image.fromarray(palette[province_map], "RGB").save(os.path.join(MAP_DATA, "provinces.png"))
    ensure_background().save(os.path.join(MAP_DATA, "ne2.png"))

    # Terreno plano: elevação no nível do mar e normal apontando para cima.
    height = np.full((HEIGHT, WIDTH), SEA_LEVEL, dtype=np.uint8)
    Image.fromarray(height, "L").save(os.path.join(MAP_DATA, "height.png"))
    normal = np.zeros((HEIGHT, WIDTH, 3), dtype=np.uint8)
    normal[:, :, 0] = 128
    normal[:, :, 1] = 128
    normal[:, :, 2] = 255
    Image.fromarray(normal, "RGB").save(os.path.join(MAP_DATA, "normal.png"))

    write_provinces_txt(os.path.join(DEFS, "provinces.txt"), survivors)
    write_countries_txt(os.path.join(DEFS, "countries.txt"), countries, survivors)
    write_country_history(os.path.join(HIST, "countries"), countries, survivors)
    write_province_history(os.path.join(HIST, "provinces", "provinces.json"), survivors)
    write_territories(
        os.path.join(DEFS, "territories.txt"),
        os.path.join(HIST, "territories", "territories.json"),
        survivors,
    )

    counts = {}
    for province in survivors:
        counts[province["owner"]] = counts.get(province["owner"], 0) + 1
    print(f"Países: {len(counts)} | Províncias: {len(survivors)}")
    print("Províncias por país (maiores):")
    for code in sorted(counts, key=lambda key: -counts[key])[:15]:
        print(f"  {code}: {counts[code]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
