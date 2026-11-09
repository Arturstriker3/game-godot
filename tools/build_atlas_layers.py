"""Gera as camadas do mapa político "atlas" a partir dos vetores do Natural Earth.

Em vez de máscaras binárias ampliadas (que viram escadinha no zoom), grava
campos de distância (SDF): o shader reconstrói linhas nítidas e com
anti-aliasing em qualquer distância de câmera.

Saídas em map/map_data/atlas/:
  region_sdf.png    RGB8  Europa em alta resolução
                    R = distância assinada à costa (128 + d*2 texels; >128 = terra)
                    G = distância assinada à fronteira de país (128 + d*8 texels)
                    B = distância assinada à divisa admin-1 (128 + d*8 texels)
                    Nas fronteiras o sinal vem da ordem dos ids dos dois lados:
                    um campo sem sinal tem um "V" na linha que a interpolação
                    bilinear achata (a linha some quando passa entre texels); com
                    sinal o cruzamento por zero sobrevive. O sinal inverte no eixo
                    medial, longe da linha — o shader descarta esse salto.
  region_color.png  RGB8  cor política (estendida sobre o mar, sem borda alfa)
  world_sdf.png     RGB8  mesmo formato, mundo inteiro em baixa resolução
  world_color.png   RGB8
  minimap.png       RGB8  mapa político global reduzido para o minimapa
  nations.json      âncora do nome de cada nação jogável (pixels 5000x2500)

Uso:
  python tools/build_atlas_layers.py

Requer Natural Earth 10m admin-0 e admin-1 (NE_DIR, padrão G:\\tools\\ne_extract),
numpy, pillow, pyshp e scipy.
"""

from __future__ import annotations

import json
import os

import numpy as np
import shapefile
from PIL import Image, ImageDraw
from scipy.ndimage import distance_transform_edt, label as nd_label

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "map", "map_data", "atlas")
NE_DIR = os.environ.get("NE_DIR", r"G:\tools\ne_extract")
ADMIN0 = os.path.join(NE_DIR, "ne_10m_admin_0_countries", "ne_10m_admin_0_countries")
ADMIN1 = os.path.join(NE_DIR, "ne_10m_admin_1_states_provinces",
                      "ne_10m_admin_1_states_provinces")

# Região de alta resolução (graus). Precisa bater com `region_bounds` no shader.
REGION = (-25.0, 33.0, 45.0, 72.0)  # lon0, lat0, lon1, lat1
REGION_W = 4096
WORLD_W = 4096
SUPERSAMPLE = 2

# Código do Natural Earth -> tag do jogo (o jogo usa NED para a Holanda).
GAME_TAGS = {"GBR": "GBR", "FRA": "FRA", "DEU": "DEU", "NLD": "NED", "BEL": "BEL"}

# Cores das nações jogáveis: tons sóbrios de carta, legíveis sob o terreno.
NATION_COLORS = {
    "GBR": (160, 72, 68),
    "FRA": (80, 108, 156),
    "DEU": (112, 117, 124),
    "NED": (192, 120, 64),
    "BEL": (182, 156, 74),
}

# Nações não jogáveis: variação discreta de pergaminho por MAPCOLOR9 do Natural
# Earth (convenção cartográfica para vizinhos não terem a mesma cor).
NEUTRAL_PALETTE = [
    (201, 190, 160), (186, 190, 162), (199, 178, 152), (178, 186, 176),
    (205, 192, 166), (184, 172, 142), (192, 181, 168), (176, 184, 156),
    (196, 182, 142),
]

# Costa com alcance de ±64 texels: o mar precisa de espaço para a batimetria.
SDF_COAST_SCALE = 2.0
SDF_LINE_SCALE = 8.0


def read_admin0():
    reader = shapefile.Reader(ADMIN0, encoding="utf-8")
    names = [field[0] for field in reader.fields[1:]]
    countries = []
    for shape, record in zip(reader.shapes(), reader.records()):
        fields = dict(zip(names, record))
        tag = str(fields.get("ADM0_A3") or "").strip()
        if not tag or tag == "-99":
            continue
        parts = split_parts(shape)
        if not parts:
            continue
        area = sum(abs(ring_area(part)) for part in parts)
        countries.append({
            "tag": tag,
            "mapcolor": int(fields.get("MAPCOLOR9") or 1),
            "parts": parts,
            "area": area,
        })
    countries.sort(key=lambda item: item["area"], reverse=True)
    return countries


def read_admin1(bounds):
    lon0, lat0, lon1, lat1 = bounds
    reader = shapefile.Reader(ADMIN1, encoding="utf-8")
    shapes = []
    for shape in reader.shapes():
        x0, y0, x1, y1 = shape.bbox
        if x1 < lon0 or x0 > lon1 or y1 < lat0 or y0 > lat1:
            continue
        parts = split_parts(shape)
        if parts:
            shapes.append(parts)
    return shapes


def split_parts(shape):
    parts = []
    bounds = list(shape.parts) + [len(shape.points)]
    for begin, end in zip(bounds[:-1], bounds[1:]):
        ring = np.asarray(shape.points[begin:end], dtype=np.float64)
        if ring.shape[0] >= 3:
            parts.append(ring)
    return parts


def ring_area(ring):
    x = ring[:, 0]
    y = ring[:, 1]
    return 0.5 * float(np.sum(x * np.roll(y, -1) - np.roll(x, -1) * y))


class Grid:
    """Projeção equirretangular de uma janela lon/lat para pixels."""

    def __init__(self, bounds, width):
        self.lon0, self.lat0, self.lon1, self.lat1 = bounds
        self.width = width
        self.height = int(round(width * (self.lat1 - self.lat0) / (self.lon1 - self.lon0)))

    def project(self, ring):
        x = (ring[:, 0] - self.lon0) / (self.lon1 - self.lon0) * self.width
        y = (self.lat1 - ring[:, 1]) / (self.lat1 - self.lat0) * self.height
        return list(zip(x.tolist(), y.tolist()))

    def rasterize(self, polygons, values):
        canvas = Image.new("I", (self.width, self.height), 0)
        draw = ImageDraw.Draw(canvas)
        for parts, value in zip(polygons, values):
            for ring in parts:
                draw.polygon(self.project(ring), fill=int(value))
        return np.array(canvas, dtype=np.int32)


def border_neighbors(ids, valid, group=None):
    """Para cada pixel de borda, o id do vizinho do outro lado (0 = não é borda).

    `group` restringe a borda a vizinhos do mesmo grupo (divisa de província só
    dentro do mesmo país)."""
    neighbor = np.zeros(ids.shape, dtype=np.int32)
    pairs = (
        ((slice(None), slice(None, -1)), (slice(None), slice(1, None))),
        ((slice(None, -1), slice(None)), (slice(1, None), slice(None))),
    )
    for first, second in pairs:
        a = ids[first]
        b = ids[second]
        ok = valid[first] & valid[second] & (a != b)
        if group is not None:
            ok &= group[first] == group[second]
        neighbor[first] = np.where(ok, b, neighbor[first])
        neighbor[second] = np.where(ok, a, neighbor[second])
    return neighbor


def signed_border_distance(ids, neighbor):
    """Distância à borda com sinal pela ordem dos ids (lado de id menor < 0)."""
    border = neighbor != 0
    if not border.any():
        return np.full(ids.shape, 1e6, dtype=np.float32)
    dist, (rows, cols) = distance_transform_edt(~border, return_indices=True)
    nearest_id = ids[rows, cols]
    same_side = nearest_id == ids
    other = np.where(same_side, neighbor[rows, cols], nearest_id)
    # A borda real fica entre os dois pixels marcados: meio pixel de correção.
    magnitude = np.where(same_side, dist + 0.5, np.maximum(dist - 0.5, 0.0))
    del dist, rows, cols, nearest_id
    return np.where(ids > other, magnitude, -magnitude).astype(np.float32)


def downsample(values, factor):
    height, width = values.shape
    height -= height % factor
    width -= width % factor
    view = values[:height, :width].reshape(height // factor, factor, width // factor, factor)
    return view.mean(axis=(1, 3))


def downsample_signed(values, factor):
    """Média 2×2 que não cria zero falso no salto de sinal do eixo medial.

    Bloco com sinais opostos e magnitudes pequenas é uma fronteira real (média
    correta); com magnitudes grandes é o eixo medial, onde a média daria ~0 e
    viraria linha — ali usa-se a amostra pontual, preservando o salto."""
    height, width = values.shape
    height -= height % factor
    width -= width % factor
    blocks = values[:height, :width].reshape(height // factor, factor, width // factor, factor)
    mean = blocks.mean(axis=(1, 3))
    low = blocks.min(axis=(1, 3))
    high = blocks.max(axis=(1, 3))
    medial = (low < 0.0) & (high > 0.0) & (np.minimum(-low, high) > factor)
    return np.where(medial, blocks[:, 0, :, 0], mean)


def encode(values, scale, offset):
    return np.clip(np.round(offset + values * scale), 0, 255).astype(np.uint8)


def build_layer(name, bounds, width, countries, province_shapes):
    hi = Grid(bounds, width * SUPERSAMPLE)
    lo_height = hi.height // SUPERSAMPLE
    print(f"[{name}] raster {hi.width}x{hi.height} -> {width}x{lo_height}")

    owners = hi.rasterize([c["parts"] for c in countries], range(1, len(countries) + 1))
    land = owners != 0

    coast = distance_transform_edt(land).astype(np.float32)
    coast -= distance_transform_edt(~land).astype(np.float32)
    coast_lo = downsample(coast, SUPERSAMPLE) / SUPERSAMPLE
    del coast

    owner_neighbor = border_neighbors(owners, land)
    border_field = signed_border_distance(owners, owner_neighbor)
    # O salto do eixo medial continua um salto (o shader o descarta pelo gradiente).
    borders_lo = downsample_signed(border_field, SUPERSAMPLE) / SUPERSAMPLE
    border_unsigned_lo = np.abs(borders_lo)
    del border_field, owner_neighbor

    if province_shapes:
        provinces = hi.rasterize(province_shapes, range(1, len(province_shapes) + 1))
        valid = land & (provinces != 0)
        # Só divisas internas: a fronteira de país já tem linha própria.
        province_neighbor = border_neighbors(provinces, valid, group=owners)
        province_field = signed_border_distance(provinces, province_neighbor)
        provinces_lo = downsample_signed(province_field, SUPERSAMPLE) / SUPERSAMPLE
        del provinces, province_neighbor, province_field
    else:
        provinces_lo = np.full(coast_lo.shape, 1e6, dtype=np.float32)

    shape = coast_lo.shape
    borders_lo = borders_lo[:shape[0], :shape[1]]
    border_unsigned_lo = border_unsigned_lo[:shape[0], :shape[1]]
    provinces_lo = provinces_lo[:shape[0], :shape[1]]

    sdf = np.dstack([
        encode(coast_lo, SDF_COAST_SCALE, 128.0),
        encode(borders_lo, SDF_LINE_SCALE, 128.0),
        encode(provinces_lo, SDF_LINE_SCALE, 128.0),
    ])
    Image.fromarray(sdf, "RGB").save(os.path.join(OUT, f"{name}_sdf.png"), optimize=True)

    owners_lo = owners[::SUPERSAMPLE, ::SUPERSAMPLE][:coast_lo.shape[0], :coast_lo.shape[1]]
    del owners, land
    palette = np.zeros((len(countries) + 1, 3), dtype=np.uint8)
    for index, country in enumerate(countries, start=1):
        palette[index] = country_color(country)
    land_lo = owners_lo != 0
    # Estende a cor da terra sobre o mar: o filtro bilinear nunca mistura a
    # cor de uma nação com "nada" na costa (era a causa do halo branco).
    _, nearest = distance_transform_edt(~land_lo, return_indices=True)
    owners_filled = owners_lo[nearest[0], nearest[1]]
    color = palette[owners_filled]
    Image.fromarray(color, "RGB").save(os.path.join(OUT, f"{name}_color.png"), optimize=True)

    return {
        "grid": Grid(bounds, width),
        "owners": owners_lo,
        "land": land_lo,
        "borders": border_unsigned_lo,
        "color": color,
    }


def country_color(country):
    game_tag = GAME_TAGS.get(country["tag"])
    if game_tag:
        return NATION_COLORS[game_tag]
    return NEUTRAL_PALETTE[(country["mapcolor"] - 1) % len(NEUTRAL_PALETTE)]


def nation_anchors(layer, countries):
    """Âncora do nome de cada nação jogável (pixels 5000x2500).

    Usa o polo de inacessibilidade da maior massa contígua (o ponto mais longe
    da borda), não o centroide: em países recortados como a Grã-Bretanha o
    centroide fica perto da costa e o nome vaza para o mar."""
    grid = layer["grid"]
    anchors = {}
    for index, country in enumerate(countries, start=1):
        game_tag = GAME_TAGS.get(country["tag"])
        if not game_tag:
            continue
        labels, count = nd_label(layer["owners"] == index)
        if count == 0:
            continue
        sizes = np.bincount(labels.ravel())
        sizes[0] = 0
        mass = labels == int(np.argmax(sizes))
        rows, cols = np.nonzero(mass)
        inner = distance_transform_edt(mass)
        row, col = np.unravel_index(int(np.argmax(inner)), inner.shape)
        lon = grid.lon0 + (col + 0.5) / grid.width * (grid.lon1 - grid.lon0)
        lat = grid.lat1 - (row + 0.5) / grid.height * (grid.lat1 - grid.lat0)
        anchors[game_tag] = {
            "x": round((lon + 180.0) / 360.0 * 5000.0, 1),
            "y": round((90.0 - lat) / 180.0 * 2500.0, 1),
            "extent_px": round(float(np.sqrt(rows.size)) / grid.width * 5000.0
                               * (grid.lon1 - grid.lon0) / 360.0, 1),
        }
    return anchors


def write_minimap(world):
    color = world["color"].astype(np.float32)
    sea = np.array([34, 64, 74], dtype=np.float32)
    ink = np.array([24, 28, 30], dtype=np.float32)
    land = world["land"][..., None]
    image = np.where(land, color * 0.92, sea)
    border = (world["borders"] < 1.0)[..., None] & land
    image = np.where(border, ink, image)
    small = Image.fromarray(image.astype(np.uint8), "RGB").resize((512, 256), Image.LANCZOS)
    small.save(os.path.join(OUT, "minimap.png"), optimize=True)


def main():
    for path in (ADMIN0, ADMIN1):
        if not os.path.exists(path + ".shp"):
            raise SystemExit(f"Fonte Natural Earth ausente: {path}.shp")
    os.makedirs(OUT, exist_ok=True)

    countries = read_admin0()
    print(f"países={len(countries)}")
    provinces = read_admin1(REGION)
    print(f"províncias na região={len(provinces)}")

    region = build_layer("region", REGION, REGION_W, countries, provinces)
    with open(os.path.join(OUT, "nations.json"), "w", encoding="utf-8") as handle:
        json.dump(nation_anchors(region, countries), handle, indent="\t")
        handle.write("\n")
    del region

    world = build_layer("world", (-180.0, -90.0, 180.0, 90.0), WORLD_W, countries, [])
    write_minimap(world)
    print(f"escrito em {OUT}")


if __name__ == "__main__":
    main()
