"""Gera as camadas geopolíticas do mapa a partir das fronteiras reais (Natural Earth).

Não recria o mapa: só rasteriza os países na mesma projeção equirretangular do
terreno e escreve duas texturas alinhadas pixel a pixel:

  map/map_data/country_borders.png  (L8,    5000x2500) fronteiras entre países
  map/map_data/nation_fill.png      (RGBA8, 2500x1250) preenchimento das nações jogáveis

Uso:
  python tools/build_political_layer.py

Requer Natural Earth 10m admin-0 (NE_DIR, padrão G:\\tools\\ne_extract).
"""

from __future__ import annotations

import os

import numpy as np
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAP_DATA = os.path.join(ROOT, "map", "map_data")
NE_DIR = os.environ.get("NE_DIR", r"G:\tools\ne_extract")
SHAPEFILE = os.path.join(NE_DIR, "ne_10m_admin_0_countries", "ne_10m_admin_0_countries")

BORDERS_OUT = os.path.join(MAP_DATA, "country_borders.png")
FILL_OUT = os.path.join(MAP_DATA, "nation_fill.png")

WIDTH = 5000
HEIGHT = 2500
LAT_MIN = -90.0
LAT_MAX = 90.0

# Cores das nações jogáveis, iguais às do HUD (UiKit.NATION_COLORS).
NATION_COLORS = {
    "GBR": (0xC7, 0x4A, 0x4F),
    "FRA": (0x4A, 0x78, 0xC7),
    "DEU": (0x8A, 0x8F, 0x96),
    "NED": (0xD8, 0x83, 0x3A),
    "BEL": (0xCB, 0xB6, 0x4A),
}


def unwrap_and_project(ring: np.ndarray) -> np.ndarray:
    """Longitudes contínuas (evita polígonos que cruzam o antimeridiano)."""
    lon = ring[:, 0].astype(np.float64).copy()
    lat = ring[:, 1].astype(np.float64)

    unwrapped = np.empty_like(lon)
    unwrapped[0] = lon[0]
    for index in range(1, lon.size):
        delta = lon[index] - lon[index - 1]
        while delta > 180.0:
            delta -= 360.0
        while delta < -180.0:
            delta += 360.0
        unwrapped[index] = unwrapped[index - 1] + delta

    x = (unwrapped + 180.0) / 360.0 * WIDTH
    y = (LAT_MAX - lat) / (LAT_MAX - LAT_MIN) * HEIGHT
    return np.column_stack((x, y))


def draw_rings(draw: ImageDraw.ImageDraw, rings: list[np.ndarray], value: int) -> None:
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
            draw.polygon(shifted.tolist(), fill=value)


def load_countries():
    """(tag, área, partes) por país, dos maiores para os menores."""
    import shapefile

    reader = shapefile.Reader(SHAPEFILE, encoding="utf-8")
    field_names = [field[0] for field in reader.fields[1:]]
    countries = []

    for shape, record in zip(reader.shapes(), reader.records()):
        fields = dict(zip(field_names, record))
        tag = (fields.get("ADM0_A3") or fields.get("ISO_A3") or "").strip()
        if not tag or tag == "-99":
            continue

        parts = []
        for begin, end in zip(shape.parts, list(shape.parts[1:]) + [len(shape.points)]):
            parts.append(np.array(shape.points[begin:end], dtype=np.float64))
        if not parts:
            continue

        area = sum(abs(ring_area(part)) for part in parts)
        countries.append((tag, area, parts))

    countries.sort(key=lambda item: item[1], reverse=True)
    return countries


def ring_area(ring: np.ndarray) -> float:
    x = ring[:, 0]
    y = ring[:, 1]
    return 0.5 * float(np.sum(x * np.roll(y, -1) - np.roll(x, -1) * y))


def build_owner_ids(countries) -> tuple[np.ndarray, dict[str, int]]:
    canvas = Image.new("I", (WIDTH, HEIGHT), 0)
    draw = ImageDraw.Draw(canvas)

    tag_index: dict[str, int] = {}
    for position, (tag, _area, parts) in enumerate(countries, start=1):
        if tag not in tag_index:
            tag_index[tag] = position
        draw_rings(draw, parts, tag_index[tag])

    return np.array(canvas, dtype=np.int32), tag_index


def write_borders(owner_ids: np.ndarray) -> int:
    land = owner_ids != 0
    border = np.zeros(owner_ids.shape, dtype=bool)

    horizontal = land[:, :-1] & land[:, 1:] & (owner_ids[:, :-1] != owner_ids[:, 1:])
    vertical = land[:-1, :] & land[1:, :] & (owner_ids[:-1, :] != owner_ids[1:, :])

    border[:, :-1] |= horizontal
    border[:-1, :] |= vertical

    Image.fromarray((border * 255).astype(np.uint8), mode="L").save(BORDERS_OUT)
    return int(border.sum())


def write_fill(owner_ids: np.ndarray, tag_index: dict[str, int]) -> None:
    sampled = owner_ids[::2, ::2]
    height, width = sampled.shape
    pixels = np.zeros((height, width, 4), dtype=np.uint8)

    # O jogo chama a Holanda de NED; o Natural Earth usa NLD.
    natural_earth_tag = {"NED": "NLD"}
    for tag, color in NATION_COLORS.items():
        index = tag_index.get(natural_earth_tag.get(tag, tag), 0)
        if index == 0:
            continue
        mask = sampled == index
        pixels[mask, 0] = color[0]
        pixels[mask, 1] = color[1]
        pixels[mask, 2] = color[2]
        pixels[mask, 3] = 255

    Image.fromarray(pixels, mode="RGBA").save(FILL_OUT)


def alignment_iou(owner_ids: np.ndarray) -> float:
    """Compara a máscara de terra com nav_mask.png para conferir o alinhamento."""
    nav_path = os.path.join(MAP_DATA, "nav_mask.png")
    if not os.path.exists(nav_path):
        return 0.0

    nav = np.asarray(Image.open(nav_path).convert("L"))
    nav_land = nav > 128
    land = (owner_ids != 0)[::2, ::2]
    intersection = int((land & nav_land).sum())
    union = int((land | nav_land).sum())
    return 0.0 if union == 0 else intersection / union


def main() -> None:
    if not os.path.exists(SHAPEFILE + ".shp"):
        raise SystemExit(f"Fonte Natural Earth ausente: {SHAPEFILE}.shp")

    countries = load_countries()
    owner_ids, tag_index = build_owner_ids(countries)

    borders = write_borders(owner_ids)
    write_fill(owner_ids, tag_index)

    land_pixels = int((owner_ids != 0).sum())
    print(f"paises={len(tag_index)} pixels_de_terra={land_pixels} pixels_de_fronteira={borders}")
    print(f"alinhamento_com_nav_mask iou={alignment_iou(owner_ids):.3f}")
    print(f"escrito: {BORDERS_OUT}")
    print(f"escrito: {FILL_OUT}")


if __name__ == "__main__":
    main()
