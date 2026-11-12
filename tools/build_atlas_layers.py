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
  region_province.png RGB8  id da província admin-1 por pixel (id = R + G*256,
                      B=0; id 0 preenchido com a província de terra mais próxima)
  world_sdf.png     RGB8  mesmo formato, mundo inteiro em baixa resolução
  world_color.png   RGB8
  world_province.png RGB8 id GLOBAL da província admin-1 por pixel (id = R+G*256,
                      sem zeros: preenchido com a província de terra mais próxima)
  provinces.json      id global, país (ADM0_A3), nação (tag do jogo), cor estática,
                      centroide (5000x2500) e área global de cada província
  minimap.png       RGB8  mapa político global reduzido para o minimapa
  nations.json      âncora do nome de cada nação jogável (pixels 5000x2500)
  removed_nations.png  L8  terra das nações apagadas pela curadoria (1 = mar
                    depois da remoção); lido por tools/generate_nav_mask.py
  removed_islands.png L8  terra das ilhas pequenas removidas pela regra de
                    área (1 = mar); lido por tools/generate_nav_mask.py

Ocupação (tools/world_nation_curation.py, OCCUPIED_REGIONS/OCCUPIED_FRONT):
antes dos SDFs o dono das províncias ocupadas é repintado no raster e a parte
ocupada das províncias divididas recebe um id sintético (depois do último
admin-1 da fonte). O id original continua com a Ucrânia, então nenhuma
referência antiga muda de significado; `provinces.json` ganha as linhas novas.

Uso:
  python tools/build_atlas_layers.py

Requer Natural Earth 10m admin-0 e admin-1 (NE_DIR, padrão G:\\tools\\ne_extract),
numpy, pillow, pyshp e scipy.
"""

from __future__ import annotations

import json
import math
import os
import sys

import numpy as np
import shapefile
from PIL import Image, ImageDraw
from scipy.ndimage import (binary_dilation as nd_dilate, distance_transform_edt,
                           find_objects as nd_objects, label as nd_label,
                           sum as nd_sum)

from world_nation_curation import (OCCUPIED_FRONT, OCCUPIED_REGIONS,
                                   atlas_owner_map, atlas_removed)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "map", "map_data", "atlas")
NE_DIR = os.environ.get("NE_DIR", r"G:\tools\ne_extract")
ADMIN0 = os.path.join(NE_DIR, "ne_10m_admin_0_countries", "ne_10m_admin_0_countries")
ADMIN1 = os.path.join(NE_DIR, "ne_10m_admin_1_states_provinces",
                      "ne_10m_admin_1_states_provinces")

# Região de alta resolução (graus). Precisa bater com `region_bounds` no shader.
REGION = (-25.0, 33.0, 45.0, 72.0)  # lon0, lat0, lon1, lat1
REGION_W = 4096
WORLD_W = 8192  # ~4,9 km/texel; era 4096 (~9,8 km/texel)
SUPERSAMPLE = 2

# Plano global do jogo em pixels (o mundo cilíndrico é 5000×2500).
GLOBAL_W = 5000.0
GLOBAL_H = 2500.0

# Escala linear px -> px global: região (4096 px sobre 70°) e mundo (WORLD_W px
# sobre 360°). A área escala com o quadrado.
REGION_TO_GLOBAL = (REGION[2] - REGION[0]) / REGION_W * (GLOBAL_W / 360.0)
WORLD_TO_GLOBAL = GLOBAL_W / WORLD_W

# Ilhotes e ilhas de terra removidos da máscara (curadoria 2026-11). Cada
# spec é um componente FECHADO dentro da caixa (nunca toca a costa continental:
# o componente que encosta na borda da caixa é preservado). `max_px` é o teto
# em pixels da grade alta (supersample 2) para o componente.
#
# - Shadwan e as ilhas de Tiran/Sanafir na entrada do Golfo de Suez atrapalham
#   o pathfinding do canal (33,5–34,7E 27–28N).
# - r2 (2026-11-12): ilhas grandes que a regra de área (<2.000 km²) não pega e
#   que o usuário mandou remover. Todas ficam sem cidade/porto no cenário. As
#   ilhas <2.000 km² (Maurício, Mayotte, Martinica, Açores, Madeira,
#   Ogasawara, Páscoa, San Andrés, Polinésia) continuam saindo pela regra de
#   área; as que viraram país/território removido (MUS, PYF) já saem em
#   `read_admin0` via `atlas_removed()`. Guam voltou na r3 (território USA) e
#   Okinawa/Miyakojima ficam pelos seeds abaixo.
REMOVED_ISLETS = [
    {"name": "suez_gulf_shadwan", "lon": 33.98, "lat": 27.51,
     "radius_deg": 0.18, "max_px": 4096},
    {"name": "suez_gulf_tiran", "lon": 34.62, "lat": 27.955,
     "radius_deg": 0.22, "max_px": 4096},
    {"name": "reunion", "lon": 55.52, "lat": -21.11,
     "radius_deg": 0.45, "max_px": 8000},
    {"name": "martinique", "lon": -61.0, "lat": 14.65,
     "radius_deg": 0.35, "max_px": 8000},
    {"name": "svalbard", "lon": 17.5, "lat": 78.5,
     "radius_deg": 9.5, "max_px": 200000},
    {"name": "bioko", "lon": 8.75, "lat": 2.45,
     "radius_deg": 1.6, "max_px": 8000},
    {"name": "azores", "lon": -27.9, "lat": 38.5,
     "radius_deg": 3.6, "max_px": 8000},
    {"name": "madeira", "lon": -16.9, "lat": 32.75,
     "radius_deg": 0.9, "max_px": 5000},
]

# Curadoria global de ilhas pequenas (fase B, 2026-11). A regra é por LIMIAR
# DE ÁREA, não por caixa: todo componente de terra ISOLADO (8-vizinhos) com
# área < ISLAND_MAX_KM2 vira mar, no atlas e no nav_mask (este último lê
# `removed_islands.png`, gerado aqui). Ficam:
#   - ilhas com cidade, porto/capital ou jazida do cenário (`load_keep_points`);
#   - ilhas a menos de COASTAL_KEEP_KM de uma massa de terra maior — ilhas
#     costeiras coladas (Eubeia, Samos) não podem abrir buraco no litoral;
#   - componentes que encostam na borda da camada (continente cortado);
#   - seeds explícitos de ISLAND_KEEP_SEEDS (exceções sem entidade no cenário).
# A área é calculada em km² reais na projeção equirretangular do atlas,
# somando cos(lat) por linha — nunca em pixels. A decisão é tomada UMA vez, na
# camada mundial; a camada da região recebe a mesma máscara (apply_removed_mask)
# para o mapa regional e o global nunca divergirem.
ISLAND_MAX_KM2 = 2000.0
COASTAL_KEEP_KM = 3.0
# Exceções que não têm entidade no cenário: as Malvinas voltaram como território
# britânico (GBR) com Stanley; o seed garante a ilha mesmo se o recorte de costa
# ficar ambíguo. As demais ilhas com cidade/porto se mantêm por `load_keep_points`.
# Curadoria de ilhas devolvidas (r3, 2026-11-12): Okinawa (Naha), Miyakojima
# (cidade `JPN-MIY`) e Guam (Hagåtña, território dos EUA) ficam no atlas e no
# nav_mask. As três têm cidade/porto no cenário; o seed garante a ilha mesmo
# quando o build roda antes da promoção do `cities.json` (ordem do pipeline).
ISLAND_KEEP_SEEDS = [
    ("falklands_stanley", -58.90, -51.70),
    ("okinawa_naha", 127.68, 26.21),
    ("miyakojima", 125.28, 24.79),
    ("guam_hagatna", 144.75, 13.47),
]

# Faixas usadas só no relatório/log (a remoção é global). A primeira que
# contém o centroide vence; fora de todas = "Outros".
ISLAND_REGIONS = [
    ("Ártico", -180.0, 180.0, 66.5, 90.0),
    ("Caribe", -90.0, -58.0, 8.0, 28.0),
    ("Mediterrâneo", -6.0, 37.0, 30.0, 46.0),
    ("Sudeste Asiático", 95.0, 142.0, -12.0, 24.0),
    ("Pacífico", 110.0, 180.0, -50.0, 60.0),
    ("Pacífico", -180.0, -70.0, -50.0, 60.0),
    ("Índico", 40.0, 110.0, -45.0, 30.0),
    ("Norte da Europa", -25.0, 35.0, 54.0, 72.0),
]

# Entidades que mantêm a ilha onde estão. Cidade/porto: x/y do plano 5000x2500
# do cenário. Jazida: lon/lat do MRDS/suplemento. Raio em texels para absorver
# o arredondamento do raster (portos ficam na borda d'água).
KEEP_ENTITY_RADIUS = {"city": 2, "port": 2, "deposit": 1}
SCENARIO_DIR = os.path.join(ROOT, "data", "definitions", "scenario")
NATURAL_DIR = os.path.join(ROOT, "data", "definitions", "natural")

# Código do Natural Earth -> tag do jogo (o jogo usa NED para a Holanda).
GAME_TAGS = {"GBR": "GBR", "FRA": "FRA", "DEU": "DEU", "NLD": "NED",
             "BEL": "BEL", "VEN": "VEN", "SUR": "SUR", "GUY": "GUY"}

# Curadoria de nações (tools/world_nation_curation.py): microestados e
# territórios grandes têm as geometrias FUNDIDAS no dono (a nação deixa de
# existir e o vizinho/mãe pinta a área); territórios minúsculos e ilhas
# soberanas removidas NÃO são rasterizados (a terra vira mar). O id global das
# províncias admin-1 não muda: só o dono e a presença no raster mudam.
ATLAS_OWNER = atlas_owner_map()
ATLAS_REMOVED = atlas_removed()

# Cores das nações jogáveis. Fonte de verdade: a coluna `color` de
# data/definitions/world/out/nations.csv (tom "papelaria" da F3, saturação ×0,86
# + correção do par BWA×AGO). Mantidas aqui para o atlas estático e o editor
# continuarem idênticos ao mapa dinâmico; `territory_map.gd` lê o CSV.
NATION_COLORS = {
    "GBR": (154, 78, 74),
    "FRA": (75, 101, 160),
    "DEU": (113, 117, 123),
    "NED": (183, 121, 73),
    "BEL": (122, 160, 75),
    "VEN": (213, 188, 71),
    "SUR": (73, 137, 92),
    "GUY": (147, 100, 156),
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
    removed = []
    merged = {}
    for shape, record in zip(reader.shapes(), reader.records()):
        fields = dict(zip(names, record))
        tag = str(fields.get("ADM0_A3") or "").strip()
        if not tag or tag == "-99":
            continue
        parts = split_parts(shape)
        if not parts:
            continue
        if tag in ATLAS_REMOVED:
            removed.extend(parts)
            continue
        area = sum(abs(ring_area(part)) for part in parts)
        owner = ATLAS_OWNER.get(tag, tag)
        if owner != tag:
            merged.setdefault(owner, []).extend(parts)
            continue
        countries.append({
            "tag": tag,
            "mapcolor": int(fields.get("MAPCOLOR9") or 1),
            "parts": parts,
            "area": area,
        })
    # Funde a geometria dos microestados/territórios grandes no dono. O desenho
    # em ordem mantém a forma do dono por baixo e preenche os enclaves com o
    # MESMO id de dono — a fronteira interna desaparece (anexação real).
    for owner, parts in merged.items():
        target = next((c for c in countries if c["tag"] == owner), None)
        if target is None:
            raise SystemExit(f"curadoria: dono {owner} ausente do admin-0")
        target["parts"].extend(parts)
        target["area"] += sum(abs(ring_area(part)) for part in parts)
    countries.sort(key=lambda item: item["area"], reverse=True)
    return countries, removed


def read_admin1_all():
    """Lê TODAS as províncias admin-1 do shapefile (sem filtro de bbox).

    O id global é o índice 1..N na lista única. Devolve as geometrias, o
    código ADM0, a bbox (para a seleção da região) e o ISO 3166-2 (chave
    estável das decisões de curadoria por província, ex.: UA-09)."""
    reader = shapefile.Reader(ADMIN1, encoding="utf-8")
    names = [field[0] for field in reader.fields[1:]]
    shapes = []
    countries = []
    bboxes = []
    iso_codes = []
    for shape, record in zip(reader.shapes(), reader.records()):
        parts = split_parts(shape)
        if not parts:
            continue
        fields = dict(zip(names, record))
        adm0 = str(fields.get("adm0_a3") or fields.get("ADM0_A3") or "").strip()
        shapes.append(parts)
        countries.append(adm0)
        bboxes.append(shape.bbox)
        iso_codes.append(str(fields.get("iso_3166_2") or "").strip())
    return shapes, countries, bboxes, iso_codes


def select_region(provinces, province_countries, province_bboxes, bounds):
    """Filtra as províncias que intersectam a bbox da região, mantendo o id global."""
    lon0, lat0, lon1, lat1 = bounds
    sel_shapes = []
    sel_countries = []
    sel_ids = []
    for index, (parts, bbox) in enumerate(zip(provinces, province_bboxes)):
        x0, y0, x1, y1 = bbox
        if x1 < lon0 or x0 > lon1 or y1 < lat0 or y0 > lat1:
            continue
        sel_shapes.append(parts)
        sel_countries.append(province_countries[index])
        sel_ids.append(index + 1)  # id global
    return sel_shapes, sel_countries, sel_ids


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


def rasterize_ids(grid, polygons, values):
    """Rasteriza ids em uint16 (países e províncias cabem em 16 bits)."""
    return grid.rasterize(polygons, values).astype(np.uint16)


def remove_islets(owners, provinces, grid, specs, mark_mask=None):
    """Zera componentes de terra isolados dentro das caixas de curadoria.

    Cada spec é (lon, lat, raio em graus, teto de pixels). O componente só é
    removido se estiver INTEIRO dentro da caixa (nenhum pixel na borda) e tiver
    área <= max_px; assim a costa continental, que atravessa a borda, nunca é
    tocada. O id de província dos pixels removidos também volta a 0.
    `mark_mask`, quando dado, recebe True nos pixels removidos para entrar no
    `removed_islands.png` (o nav_mask limpa a mesma terra).
    """
    for spec in specs:
        lon, lat = spec["lon"], spec["lat"]
        radius = spec["radius_deg"]
        x0 = int((lon - radius - grid.lon0) / (grid.lon1 - grid.lon0) * grid.width)
        x1 = int((lon + radius - grid.lon0) / (grid.lon1 - grid.lon0) * grid.width) + 1
        y0 = int((grid.lat1 - lat - radius) / (grid.lat1 - grid.lat0) * grid.height)
        y1 = int((grid.lat1 - lat + radius) / (grid.lat1 - grid.lat0) * grid.height) + 1
        x0, x1 = max(0, x0), min(grid.width, x1)
        y0, y1 = max(0, y0), min(grid.height, y1)
        if x1 <= x0 or y1 <= y0:
            continue
        sub = owners[y0:y1, x0:x1] != 0
        labels, count = nd_label(sub)
        for index in range(1, count + 1):
            ys, xs = np.where(labels == index)
            area = int(xs.size)
            if area > spec["max_px"]:
                continue
            if (xs.min() == 0 or ys.min() == 0
                    or xs.max() == sub.shape[1] - 1
                    or ys.max() == sub.shape[0] - 1):
                continue  # encosta na borda da caixa: pode ser costa real
            abs_x = xs + x0
            abs_y = ys + y0
            owners[abs_y, abs_x] = 0
            if provinces is not None:
                provinces[abs_y, abs_x] = 0
            if mark_mask is not None:
                mark_mask[abs_y, abs_x] = True
            print(f"[islet] {spec['name']}: removido componente de {area} px "
                  f"em lon={lon:.3f} lat={lat:.3f}")


def grid_pixel(grid, lon, lat):
    """Pixel (x, y) do centro lon/lat na grade do atlas."""
    x = int(round((lon - grid.lon0) / (grid.lon1 - grid.lon0) * grid.width - 0.5))
    y = int(round((grid.lat1 - lat) / (grid.lat1 - grid.lat0) * grid.height - 0.5))
    return x, y


def seed_label(labels, x, y, radius=3):
    """Id do componente de terra mais próximo do seed (tolerância em px).

    Ilhas pequenas têm o centroide a 1-2 texels de um pixel de terra; sem a
    tolerância o seed cairia na água e a ilha seria removida."""
    best = 0
    best_distance = None
    height, width = labels.shape
    for dy in range(-radius, radius + 1):
        for dx in range(-radius, radius + 1):
            xx, yy = x + dx, y + dy
            if 0 <= xx < width and 0 <= yy < height and labels[yy, xx]:
                distance = dx * dx + dy * dy
                if best_distance is None or distance < best_distance:
                    best_distance, best = distance, int(labels[yy, xx])
    return best


def island_region(lon, lat):
    """Nome da faixa de relatório que contém o centroide (primeira vence)."""
    for name, lon0, lon1, lat0, lat1 in ISLAND_REGIONS:
        if lon0 <= lon <= lon1 and lat0 <= lat <= lat1:
            return name
    return "Outros"


def load_keep_points():
    """Cidade, porto/capital e jazida que impedem a remoção da ilha.

    Cidades e portos usam x/y do plano 5000×2500 do cenário (o mesmo que o
    jogo); jazidas usam lon/lat do MRDS/suplemento. A lista é lida a cada
    build: uma cidade nova numa ilha pequena a mantém automaticamente."""
    points = []
    for filename, kind in (("cities.json", "city"), ("ports.json", "port")):
        path = os.path.join(SCENARIO_DIR, filename)
        if not os.path.exists(path):
            continue
        with open(path, encoding="utf-8") as handle:
            for item in json.load(handle):
                if item.get("x") is None or item.get("y") is None:
                    continue
                points.append({
                    "kind": kind,
                    "name": str(item.get("id", kind)),
                    "lon": float(item["x"]) / GLOBAL_W * 360.0 - 180.0,
                    "lat": 90.0 - float(item["y"]) / GLOBAL_H * 180.0,
                })
    deposits_path = os.path.join(NATURAL_DIR, "deposits.json")
    if os.path.exists(deposits_path):
        with open(deposits_path, encoding="utf-8") as handle:
            for item in json.load(handle).get("deposits", []):
                if item.get("lon") is None or item.get("lat") is None:
                    continue
                points.append({
                    "kind": "deposit",
                    "name": str(item.get("id", "deposit")),
                    "lon": float(item["lon"]),
                    "lat": float(item["lat"]),
                })
    return points


def remove_small_islands(owners, provinces, grid, layer, countries=None,
                         keep_mask=False, dry_run=False):
    """Remove todo componente de terra isolado com área < ISLAND_MAX_KM2.

    Componentes de 8-vizinhos: a ilha é uma massa de terra sem ligação com
    outra. Ficam as ilhas com cidade/porto/jazida (`load_keep_points`), a menos
    de COASTAL_KEEP_KM de uma massa maior (proteção do litoral), as cortadas
    pela borda da camada e os seeds de ISLAND_KEEP_SEEDS. A área soma
    cos(lat) por linha na projeção equirretangular (km² reais).

    Devolve `(removed_mask, report)`: `removed_mask` é None sem `keep_mask`;
    `report` é a lista de componentes removidos (dicts) para documentação.
    Com `dry_run`, nada é alterado — só o relatório é produzido."""
    radius = 6371.0
    dlon = math.radians((grid.lon1 - grid.lon0) / grid.width)
    dlat = math.radians((grid.lat1 - grid.lat0) / grid.height)
    span_lon = grid.lon1 - grid.lon0
    span_lat = grid.lat1 - grid.lat0
    land = owners != 0
    print(f"[ilhas:{layer}] procurando componentes em {grid.width}x{grid.height}")
    labels, count = nd_label(land, structure=np.ones((3, 3), dtype=bool))
    sizes = nd_sum(land, labels, np.arange(count + 1)).astype(np.int64)
    objects = nd_objects(labels)
    removed_mask = np.zeros(owners.shape, dtype=bool) if keep_mask else None

    keep_points = load_keep_points()
    keep_labels = {}
    unresolved = []
    for point in keep_points:
        x, y = grid_pixel(grid, point["lon"], point["lat"])
        component = seed_label(labels, x, y, KEEP_ENTITY_RADIUS[point["kind"]])
        if component:
            keep_labels.setdefault(component, point)
        elif point["kind"] != "deposit":
            unresolved.append(point)
    seed_labels = {}
    for name, slon, slat in ISLAND_KEEP_SEEDS:
        x, y = grid_pixel(grid, slon, slat)
        component = seed_label(labels, x, y, 3)
        if component:
            seed_labels[component] = name
        else:
            print(f"[ilhas:{layer}] AVISO: seed {name} caiu fora da terra")

    def tag_of(index, row, col):
        if countries is None:
            return "?"
        owner = int(owners[row, col])
        if 0 < owner <= len(countries):
            return countries[owner - 1]["tag"]
        return "?"

    # Passo 1: área de cada componente. Os que já passam do teto por qualquer
    # latitude possível são marcados como massa maior sem rasterizar o recorte.
    size_cap = 60000
    big_ids = []
    candidates = []
    for index in range(1, count + 1):
        pixels = int(sizes[index])
        if pixels <= 0:
            continue
        sl = objects[index - 1]
        top_lat = grid.lat1 - sl[0].start / grid.height * span_lat
        bottom_lat = grid.lat1 - sl[0].stop / grid.height * span_lat
        max_abs_lat = min(max(abs(top_lat), abs(bottom_lat)), 89.9)
        min_px_km2 = (radius ** 2) * dlon * dlat * math.cos(math.radians(max_abs_lat))
        if pixels > size_cap or pixels * min_px_km2 >= ISLAND_MAX_KM2:
            big_ids.append(index)
            continue

        sub = labels[sl] == index
        ys_rel, xs_rel = np.nonzero(sub)
        rows = ys_rel + sl[0].start
        cols = xs_rel + sl[1].start
        lat_rows = grid.lat1 - (rows + 0.5) / grid.height * span_lat
        pixels_km2 = (radius ** 2) * dlon * dlat * np.cos(np.radians(lat_rows))
        km2 = float(pixels_km2.sum())
        lat = float(lat_rows.mean())
        lon = grid.lon0 + (float(cols.mean()) + 0.5) / grid.width * span_lon
        tag = tag_of(index, int(rows.mean()), int(cols.mean()))

        reason = None
        detail = ""
        if index in keep_labels:
            reason = "entidade"
            detail = f"{keep_labels[index]['kind']}:{keep_labels[index]['name']}"
        elif index in seed_labels:
            reason = "seed"
            detail = seed_labels[index]
        elif (sl[0].start == 0 or sl[1].start == 0
              or sl[0].stop == grid.height or sl[1].stop == grid.width):
            reason = "borda"

        if reason is None and km2 >= ISLAND_MAX_KM2:
            big_ids.append(index)
            continue

        candidates.append({
            "index": index, "sl": sl, "km2": km2, "lat": lat, "lon": lon,
            "tag": tag, "pixels": pixels, "reason": reason, "detail": detail,
            "region": island_region(lon, lat),
        })

    is_big = np.zeros(count + 1, dtype=bool)
    if big_ids:
        is_big[np.asarray(big_ids, dtype=np.int64)] = True

    # Passo 2: para os candidatos restantes, a distância até uma massa MAIOR
    # decide entre "ilha costeira colada" (fica) e ilha isolada (sai).
    removed = []
    kept = []
    for cand in candidates:
        if cand["reason"]:
            kept.append(cand)
            continue
        sl = cand["sl"]
        lat = cand["lat"]
        dx_km = radius * dlon * math.cos(math.radians(lat))
        dy_km = radius * dlat
        pad = int(math.ceil(COASTAL_KEEP_KM / max(min(dx_km, dy_km), 1e-6))) + 2
        pad = min(pad, 80)
        y0 = max(0, sl[0].start - pad)
        x0 = max(0, sl[1].start - pad)
        y1 = min(grid.height, sl[0].stop + pad)
        x1 = min(grid.width, sl[1].stop + pad)
        ref = is_big[labels[y0:y1, x0:x1]]
        edge_km = float("inf")
        if ref.any():
            distance = distance_transform_edt(~ref, sampling=(dy_km, dx_km))
            sub = labels[sl] == cand["index"]
            ys_rel, xs_rel = np.nonzero(sub)
            local = distance[ys_rel + sl[0].start - y0, xs_rel + sl[1].start - x0]
            # Distância entre centros de pixels menos meio pixel de cada lado:
            # um canal de 1 texel ainda conta como "colado".
            edge_km = float(local.min()) - 0.5 * (dx_km + dy_km)
        if edge_km <= COASTAL_KEEP_KM:
            cand["reason"] = "costa"
            cand["detail"] = f"{edge_km:.1f} km"
            kept.append(cand)
            continue

        sub = labels[sl] == cand["index"]
        ys_rel, xs_rel = np.nonzero(sub)
        rows = ys_rel + sl[0].start
        cols = xs_rel + sl[1].start
        if not dry_run:
            owners[rows, cols] = 0
            if provinces is not None:
                provinces[rows, cols] = 0
            if removed_mask is not None:
                removed_mask[rows, cols] = True
        removed.append(cand)

    removed_ids = {cand["index"] for cand in removed}
    leaked = sorted(removed_ids & set(keep_labels))
    if leaked:
        raise SystemExit(f"[ilhas:{layer}] ERRO: {len(leaked)} ilha(s) com "
                         f"cidade/porto/jazida caíram na remoção")
    if unresolved:
        for point in unresolved:
            print(f"[ilhas:{layer}] AVISO: {point['kind']}:{point['name']} "
                  f"não caiu em terra na grade (lon={point['lon']:.3f} "
                  f"lat={point['lat']:.3f})")

    # Relatório: por faixa, removidos e mantidos por motivo.
    summary = {}
    for cand in removed:
        row = summary.setdefault(cand["region"], [0, 0.0, 0, 0, 0])
        row[0] += 1
        row[1] += cand["km2"]
    for cand in kept:
        row = summary.setdefault(cand["region"], [0, 0.0, 0, 0, 0])
        if cand["reason"] == "entidade":
            row[2] += 1
        elif cand["reason"] == "costa":
            row[3] += 1
        else:
            row[4] += 1

    if removed:
        print(f"[ilhas:{layer}] removidos {len(removed)} componentes, "
              f"{sum(c['pixels'] for c in removed)} px, "
              f"~{sum(c['km2'] for c in removed):.0f} km²")
    else:
        print(f"[ilhas:{layer}] nenhum componente abaixo de {ISLAND_MAX_KM2:.0f} km²")
    print(f"[ilhas:{layer}] {'região':<18} {'remov.':>7} {'km²':>10} "
          f"{'entidade':>9} {'costa':>6} {'borda/seed':>11}")
    for name, row in sorted(summary.items(), key=lambda item: -item[1][1]):
        print(f"[ilhas:{layer}] {name:<18} {row[0]:>7} {row[1]:>10.0f} "
              f"{row[2]:>9} {row[3]:>6} {row[4]:>11}")

    limit = len(removed) if dry_run else 60
    for cand in sorted(removed, key=lambda item: -item["km2"])[:limit]:
        print(f"[ilhas:{layer}] remove {cand['region']}/{cand['tag']} "
              f"{cand['pixels']:5d} px ~{cand['km2']:7.1f} km² "
              f"lon={cand['lon']:8.3f} lat={cand['lat']:7.3f}")
    if len(removed) > limit:
        print(f"[ilhas:{layer}] ... mais {len(removed) - limit} remoções "
              f"(log completo só no modo --report-islands)")

    for cand in sorted((c for c in kept if c["reason"] == "entidade"),
                       key=lambda item: -item["km2"]):
        print(f"[ilhas:{layer}] mantém {cand['detail']} "
              f"({cand['region']}/{cand['tag']} ~{cand['km2']:.0f} km²)")

    return removed_mask, removed


def apply_removed_mask(owners, provinces, grid, ref_grid, ref_mask):
    """Repete no raster grosso a remoção feita na camada fina (região).

    A camada mundial (4,9 km/texel) funde ilhas a menos de 1 texel do
    continente — Samos fica a 1,6 km da Anatólia — e o componente passa a
    encostar na borda da caixa, escapando da remoção por componente. A máscara
    da região é ~2,6x mais fina; com 1 px de dilatação, todo pixel mundial que
    cai nela vira água. Nunca toca a máscara fora da caixa do Egeu."""
    if ref_mask is None or not ref_mask.any():
        return 0
    mask = nd_dilate(ref_mask, np.ones((3, 3), dtype=bool))
    ys, xs = np.nonzero(mask)
    lon_lo = ref_grid.lon0 + xs.min() / ref_grid.width * (ref_grid.lon1 - ref_grid.lon0)
    lon_hi = ref_grid.lon0 + (xs.max() + 1) / ref_grid.width * (ref_grid.lon1 - ref_grid.lon0)
    lat_hi = ref_grid.lat1 - ys.min() / ref_grid.height * (ref_grid.lat1 - ref_grid.lat0)
    lat_lo = ref_grid.lat1 - (ys.max() + 1) / ref_grid.height * (ref_grid.lat1 - ref_grid.lat0)
    x0 = max(0, int((lon_lo - grid.lon0) / (grid.lon1 - grid.lon0) * grid.width))
    x1 = min(grid.width, int((lon_hi - grid.lon0) / (grid.lon1 - grid.lon0) * grid.width) + 1)
    y0 = max(0, int((grid.lat1 - lat_hi) / (grid.lat1 - grid.lat0) * grid.height))
    y1 = min(grid.height, int((grid.lat1 - lat_lo) / (grid.lat1 - grid.lat0) * grid.height) + 1)
    cols = np.arange(x0, x1)
    lons = grid.lon0 + (cols + 0.5) / grid.width * (grid.lon1 - grid.lon0)
    rx = ((lons - ref_grid.lon0) / (ref_grid.lon1 - ref_grid.lon0)
          * ref_grid.width).astype(np.int32)
    np.clip(rx, 0, ref_grid.width - 1, out=rx)
    cleared = 0
    for row in range(y0, y1):
        lat = grid.lat1 - (row + 0.5) / grid.height * (grid.lat1 - grid.lat0)
        ry = int((ref_grid.lat1 - lat) / (ref_grid.lat1 - ref_grid.lat0) * ref_grid.height)
        ry = min(max(ry, 0), ref_grid.height - 1)
        hit = mask[ry, rx]
        if not hit.any():
            continue
        hit_cols = cols[hit]
        owners[row, hit_cols] = 0
        if provinces is not None:
            provinces[row, hit_cols] = 0
        cleared += int(hit_cols.size)
    return cleared


def coast_field(land):
    """Distância assinada à costa em texels da grade alta (>0 = terra)."""
    if not land.any():
        return np.full(land.shape, -1e4, dtype=np.float32)
    if land.all():
        return np.full(land.shape, 1e4, dtype=np.float32)
    coast = distance_transform_edt(land).astype(np.float32)
    coast -= distance_transform_edt(~land).astype(np.float32)
    return coast


def process_rows(owners, provinces, supersample):
    """Campos SDF (resolução baixa) de uma janela de linhas da grade alta.

    Devolve (costa, fronteira de país, divisa admin-1), cada um com
    altura = linhas // supersample."""
    land = owners != 0
    coast_lo = downsample(coast_field(land), supersample) / supersample

    owner_neighbor = border_neighbors(owners, land)
    border_field = signed_border_distance(owners, owner_neighbor)
    # O salto do eixo medial continua um salto (o shader o descarta pelo gradiente).
    borders_lo = downsample_signed(border_field, supersample) / supersample
    del border_field, owner_neighbor

    if provinces is not None:
        valid = land & (provinces != 0)
        # Divisas de província em TODOS os pares (inclusive entre países): o
        # canal B também marca a fronteira entre nações, para a fase dinâmica.
        province_neighbor = border_neighbors(provinces, valid)
        province_field = signed_border_distance(provinces, province_neighbor)
        provinces_lo = downsample_signed(province_field, supersample) / supersample
        del province_neighbor, province_field
    else:
        provinces_lo = np.full(coast_lo.shape, 1e6, dtype=np.float32)
    return coast_lo, borders_lo, provinces_lo


def plan_occupation(iso_codes, bboxes):
    """Reatribui dono de província admin-1 (OCCUPIED_REGIONS).

    Devolve uma lista de especificações na ordem das províncias da fonte:
      {pid, target, mode ("whole"|"front"), new_id (0 = mantém o id), bbox}
    `new_id` começa depois do último admin-1 e é estável: regenerar a mesma
    fonte produz os mesmos ids. O id ORIGINAL de uma província dividida fica
    com a Ucrânia (nenhuma referência antiga muda de significado)."""
    target = OCCUPIED_REGIONS.get("target", "")
    whole = OCCUPIED_REGIONS.get("whole_provinces", {})
    front = OCCUPIED_REGIONS.get("front_provinces", {})
    if not target or not (whole or front):
        return []
    specs = []
    next_id = len(iso_codes) + 1
    for index, code in enumerate(iso_codes):
        if code in whole:
            mode, new_id = "whole", 0
        elif code in front:
            mode, new_id = "front", next_id
            next_id += 1
        else:
            continue
        specs.append({
            "pid": index + 1,
            "target": target,
            "mode": mode,
            "new_id": new_id,
            "bbox": tuple(float(v) for v in bboxes[index]),
        })
    return specs


def pixel_window(grid, bbox):
    """Janela (x0, x1, y0, y1) da bbox lon/lat na grade, com 2 px de margem."""
    lon0, lat0, lon1, lat1 = bbox
    x0 = int(math.floor((lon0 - grid.lon0) / (grid.lon1 - grid.lon0) * grid.width)) - 2
    x1 = int(math.ceil((lon1 - grid.lon0) / (grid.lon1 - grid.lon0) * grid.width)) + 2
    y0 = int(math.floor((grid.lat1 - lat1) / (grid.lat1 - grid.lat0) * grid.height)) - 2
    y1 = int(math.ceil((grid.lat1 - lat0) / (grid.lat1 - grid.lat0) * grid.height)) + 2
    return (max(x0, 0), min(x1, grid.width), max(y0, 0), min(y1, grid.height))


def front_mask(grid, ring, window):
    """Máscara booleana do polígono da frente dentro da janela da grade."""
    x0, x1, y0, y1 = window
    image = Image.new("L", (x1 - x0, y1 - y0), 0)
    draw = ImageDraw.Draw(image)
    points = [((lon - grid.lon0) / (grid.lon1 - grid.lon0) * grid.width - x0,
               (grid.lat1 - lat) / (grid.lat1 - grid.lat0) * grid.height - y0)
              for lon, lat in ring]
    draw.polygon(points, fill=255)
    return np.asarray(image, dtype=np.uint8) > 0


def paint_occupation(owners, provinces, grid, countries, specs):
    """Repinta dono (e id, se dividida) das províncias ocupadas.

    Trabalha só na janela da bbox de cada província: a memória não cresce com
    o mundo. `owners` recebe o índice do país alvo (a cor estática e a
    fronteira do canal G seguem daqui); nas províncias divididas o raster de
    id recebe o id NOVO na parte ocupada, então o mapa dinâmico (dono por id)
    também mostra a frente."""
    if not specs or provinces is None:
        return 0
    index_by_tag = {country["tag"]: index
                    for index, country in enumerate(countries, start=1)}
    painted = 0
    for spec in specs:
        target_index = index_by_tag.get(spec["target"])
        if target_index is None:
            raise SystemExit(
                f"ocupação: alvo {spec['target']} ausente do admin-0")
        window = pixel_window(grid, spec["bbox"])
        x0, x1, y0, y1 = window
        if x1 <= x0 or y1 <= y0:
            continue
        sub_owners = owners[y0:y1, x0:x1]
        sub_provinces = provinces[y0:y1, x0:x1]
        hit = sub_provinces == spec["pid"]
        if spec["mode"] == "front":
            hit &= front_mask(grid, OCCUPIED_FRONT, window)
        sub_owners[hit & (sub_owners != 0)] = target_index
        if spec["new_id"]:
            sub_provinces[hit] = spec["new_id"]
        painted += int(hit.sum())
    return painted


def build_layer(name, bounds, width, countries, province_shapes,
                province_countries=None, province_ids=None, supersample=SUPERSAMPLE,
                band_rows=None, color_step=1, return_removed=False,
                removed_reference=None, islands=False, occupation=None):
    """Constrói uma camada do atlas.

    `band_rows` (linhas da grade BAIXA) processa os campos de distância em
    faixas horizontais com margem, para caber em memória em camadas enormes
    (mundo em 8192 px). A margem cobre o alcance codificado dos SDFs (costa
    ±64 texels baixos = 128 altos), então o resultado é igual ao da grade
    inteira. `color_step` > 1 grava `<name>_color.png` em resolução menor
    (a cor é chapada por país; não precisa da resolução do SDF)."""
    hi = Grid(bounds, width * supersample)
    lo_height = hi.height // supersample
    lo_width = width
    grid = Grid(bounds, width)
    print(f"[{name}] raster {hi.width}x{hi.height} -> {width}x{lo_height}")

    owners = rasterize_ids(hi, [c["parts"] for c in countries], range(1, len(countries) + 1))
    provinces = None
    if province_shapes:
        provinces = rasterize_ids(hi, province_shapes, province_ids)
    if occupation:
        painted = paint_occupation(owners, provinces, hi, countries, occupation)
        print(f"[{name}] ocupação: {painted} px repintados "
              f"({len(occupation)} província(s))")
    if REMOVED_ISLETS:
        islet_mask = np.zeros(owners.shape, dtype=bool)
        remove_islets(owners, provinces, hi, REMOVED_ISLETS, mark_mask=islet_mask)
    else:
        islet_mask = None
    removed_mask = None
    if islands:
        removed_mask, _removed = remove_small_islands(
            owners, provinces, hi, name, countries, keep_mask=return_removed)
        if islet_mask is not None:
            removed_mask = islet_mask if removed_mask is None else (removed_mask | islet_mask)
    if removed_reference is not None:
        cleared = apply_removed_mask(owners, provinces, hi, *removed_reference)
        print(f"[{name}] {cleared} px da remoção de ilhas da camada mundial "
              f"(o raster fino preservaria a ilha)")

    coast_lo = np.empty((lo_height, lo_width), dtype=np.float32)
    borders_lo = np.empty((lo_height, lo_width), dtype=np.float32)
    provinces_lo = np.empty((lo_height, lo_width), dtype=np.float32)
    margin = 160 if band_rows else 0  # linhas altas; > 128 (alcance da costa)
    step = band_rows or lo_height
    for start in range(0, lo_height, step):
        stop = min(start + step, lo_height)
        hi_start = max(start * supersample - margin, 0)
        hi_stop = min(stop * supersample + margin, hi.height)
        if band_rows:
            print(f"[{name}] faixa {start}-{stop} / {lo_height}")
        c, b, p = process_rows(
            owners[hi_start:hi_stop, :],
            provinces[hi_start:hi_stop, :] if provinces is not None else None,
            supersample)
        off = (start * supersample - hi_start) // supersample
        n = stop - start
        coast_lo[start:stop] = c[off:off + n, :lo_width]
        borders_lo[start:stop] = b[off:off + n, :lo_width]
        provinces_lo[start:stop] = p[off:off + n, :lo_width]
        del c, b, p

    border_unsigned_lo = np.abs(borders_lo)

    sdf = np.dstack([
        encode(coast_lo, SDF_COAST_SCALE, 128.0),
        encode(borders_lo, SDF_LINE_SCALE, 128.0),
        encode(provinces_lo, SDF_LINE_SCALE, 128.0),
    ])
    Image.fromarray(sdf, "RGB").save(os.path.join(OUT, f"{name}_sdf.png"), optimize=True)
    del sdf, coast_lo, borders_lo, provinces_lo

    province_ids_lo = None
    if provinces is not None:
        # Raster de id de província na resolução baixa (amostragem pontual).
        province_ids_lo = provinces[::supersample, ::supersample][:lo_height, :lo_width].astype(np.int32)
    del provinces

    owners_lo = owners[::supersample, ::supersample][:lo_height, :lo_width]
    del owners
    palette = np.zeros((len(countries) + 1, 3), dtype=np.uint8)
    for index, country in enumerate(countries, start=1):
        palette[index] = country_color(country)
    land_lo = owners_lo != 0
    # Estende a cor da terra sobre o mar: o filtro bilinear nunca mistura a
    # cor de uma nação com "nada" na costa (era a causa do halo branco).
    nearest = distance_transform_edt(~land_lo, return_distances=False, return_indices=True)
    owners_filled = owners_lo[nearest[0], nearest[1]]
    del nearest
    color = palette[owners_filled]
    color_out = color[::color_step, ::color_step] if color_step > 1 else color
    Image.fromarray(np.ascontiguousarray(color_out), "RGB").save(
        os.path.join(OUT, f"{name}_color.png"), optimize=True)

    if province_ids_lo is not None:
        write_province_png(name, province_ids_lo)

    return {
        "grid": grid,
        "owners": owners_lo,
        "land": land_lo,
        "borders": border_unsigned_lo,
        "color": color,
        "province_ids": province_ids_lo,
        "removed_mask": removed_mask,
        "removed_grid": hi,
    }

def country_color(country):
    game_tag = GAME_TAGS.get(country["tag"])
    if game_tag:
        return NATION_COLORS[game_tag]
    return NEUTRAL_PALETTE[(country["mapcolor"] - 1) % len(NEUTRAL_PALETTE)]


def write_province_png(name, ids_lo):
    """Grava o raster de id de província (id = R + G*256, B=0).

    Pixels com id 0 (mar/lacunas) recebem o id da província de terra mais
    próxima (mesmo preenchimento usado para a cor política), para o shader
    nunca amostrar id 0 e o preenchimento por filtro nunca misturar com "nada"."""
    nearest = distance_transform_edt(ids_lo == 0, return_distances=False, return_indices=True)
    ids_filled = ids_lo[nearest[0], nearest[1]]
    del nearest
    rgb = np.zeros((*ids_filled.shape, 3), dtype=np.uint8)
    rgb[..., 0] = (ids_filled & 0xFF).astype(np.uint8)
    rgb[..., 1] = ((ids_filled >> 8) & 0xFF).astype(np.uint8)
    Image.fromarray(rgb, "RGB").save(os.path.join(OUT, f"{name}_province.png"), optimize=True)


def write_provinces_json(region, world, province_countries, countries, occupation=None):
    """provinces.json: uma entrada por id global com área > 0 em qualquer camada.

    Centroide (x, y) e área vêm da camada MUNDIAL (px globais 5000×2500);
    quando uma província só existe na região (pequena demais para o mundo),
    usa a camada da região como fallback. `province_countries` já inclui os
    ids sintéticos da ocupação (o dono é o alvo); `occupation` só troca o
    `country`/cor das províncias inteiras que mudaram de dono."""
    region_ids = region["province_ids"]
    world_ids = world["province_ids"]
    total = len(province_countries)
    country_by_tag = {c["tag"]: c for c in countries}
    country_override = {}
    for spec in occupation or []:
        if spec["mode"] == "whole":
            country_override[spec["pid"]] = spec["target"]

    def centroid_of(ids_lo, grid):
        counts = np.bincount(ids_lo.ravel(), minlength=total + 1)
        ys, xs = np.indices(ids_lo.shape)
        xsum = np.bincount(ids_lo.ravel(), weights=xs.ravel().astype(np.float64),
                           minlength=total + 1)
        ysum = np.bincount(ids_lo.ravel(), weights=ys.ravel().astype(np.float64),
                           minlength=total + 1)
        return counts, xsum, ysum

    world_counts, world_xsum, world_ysum = centroid_of(world_ids, world["grid"])
    region_counts, region_xsum, region_ysum = centroid_of(region_ids, region["grid"])

    rows = []
    for pid in range(1, total + 1):
        area_world = int(world_counts[pid])
        if area_world > 0:
            area_global = area_world * (WORLD_TO_GLOBAL ** 2)
            row = world_ysum[pid] / area_world
            col = world_xsum[pid] / area_world
            grid = world["grid"]
        else:
            area_region = int(region_counts[pid])
            if area_region == 0:
                continue
            area_global = area_region * (REGION_TO_GLOBAL ** 2)
            row = region_ysum[pid] / area_region
            col = region_xsum[pid] / area_region
            grid = region["grid"]

        lon = grid.lon0 + (col + 0.5) / grid.width * (grid.lon1 - grid.lon0)
        lat = grid.lat1 - (row + 0.5) / grid.height * (grid.lat1 - grid.lat0)
        adm0 = country_override.get(pid) or province_countries[pid - 1]
        game_tag = GAME_TAGS.get(adm0)
        nation = game_tag if game_tag else adm0
        country = country_by_tag.get(adm0)
        color = list(country_color(country)) if country else list(NEUTRAL_PALETTE[0])
        rows.append({
            "id": pid,
            "country": adm0,
            "nation": nation,
            "color": color,
            "x": round((lon + 180.0) / 360.0 * GLOBAL_W, 1),
            "y": round((90.0 - lat) / 180.0 * GLOBAL_H, 1),
            "area_global": round(area_global, 1),
        })

    data = {
        "nation_colors": {tag: list(rgb_value) for tag, rgb_value in NATION_COLORS.items()},
        "provinces": rows,
    }
    with open(os.path.join(OUT, "provinces.json"), "w", encoding="utf-8") as handle:
        json.dump(data, handle, indent="\t")
        handle.write("\n")


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
            "x": round((lon + 180.0) / 360.0 * GLOBAL_W, 1),
            "y": round((90.0 - lat) / 180.0 * GLOBAL_H, 1),
            "extent_px": round(float(np.sqrt(rows.size)) / grid.width * GLOBAL_W
                               * (grid.lon1 - grid.lon0) / 360.0, 1),
        }
    return anchors


def write_minimap(world):
    color = world["color"].astype(np.float32)
    sea = np.array([34, 64, 74], dtype=np.float32)
    ink = np.array([24, 28, 30], dtype=np.float32)
    land = world["land"][..., None]
    image = np.where(land, color * 0.92, sea)
    border = (world["borders"] < 1.0 * WORLD_W / 4096)[..., None] & land
    image = np.where(border, ink, image)
    small = Image.fromarray(image.astype(np.uint8), "RGB").resize((512, 256), Image.LANCZOS)
    small.save(os.path.join(OUT, "minimap.png"), optimize=True)


def write_removed_land(removed_parts):
    """Grava a máscara da terra das nações apagadas (1 = terra removida).

    O `nav_mask.png` é derivado de um raster antigo (`provinces.png`, fora do
    repositório); esta máscara permite que `tools/generate_nav_mask.py` apague
    no mar as ilhas que saíram do atlas. Resolução do mundo (8192×4096)."""
    grid = Grid((-180.0, -90.0, 180.0, 90.0), WORLD_W)
    canvas = Image.new("L", (grid.width, grid.height), 0)
    draw = ImageDraw.Draw(canvas)
    for ring in removed_parts:
        draw.polygon(grid.project(ring), fill=255)
    canvas.save(os.path.join(OUT, "removed_nations.png"), optimize=True)
    print(f"[removidas] {len(removed_parts)} anel(éis) de terra viraram mar "
          f"(removed_nations.png)")


def write_removed_islets(removed_mask, removed_grid):
    """Grava `removed_islands.png` (1 = terra que virou mar).

    Mesma resolução de `removed_nations.png` (8192×4096). A redução é por
    MAX (qualquer texel removido marca o pixel), para não sumir um ilhéu fino.
    Lida por `tools/generate_nav_mask.py`."""
    if removed_mask is None:
        return
    height = removed_mask.shape[0] // 2 * 2
    width = removed_mask.shape[1] // 2 * 2
    blocks = removed_mask[:height, :width].reshape(height // 2, 2, width // 2, 2)
    small = blocks.any(axis=(1, 3))
    image = Image.fromarray((small.astype(np.uint8) * 255), "L")
    image.save(os.path.join(OUT, "removed_islands.png"), optimize=True)
    print(f"[ilhas] removed_islands.png: {int(small.sum())} px de terra viraram "
          f"mar na grade {removed_grid.width}x{removed_grid.height}")


def report_islands():
    """Relatório seco da regra de ilhas (não escreve nada; `--report-islands`)."""
    countries, _removed = read_admin0()
    print(f"países={len(countries)} | nações apagadas={len(ATLAS_REMOVED)}")
    hi = Grid((-180.0, -90.0, 180.0, 90.0), WORLD_W * SUPERSAMPLE)
    owners = rasterize_ids(hi, [c["parts"] for c in countries],
                           range(1, len(countries) + 1))
    # Mesma ordem do build: as caixas de curadoria (Suez/Shadwan) saem antes,
    # senão o relatório conta esses ilhotes como removidos pela regra de área.
    remove_islets(owners, None, hi, REMOVED_ISLETS)
    remove_small_islands(owners, None, hi, "world", countries, dry_run=True)


def main():
    for path in (ADMIN0, ADMIN1):
        if not os.path.exists(path + ".shp"):
            raise SystemExit(f"Fonte Natural Earth ausente: {path}.shp")
    os.makedirs(OUT, exist_ok=True)

    countries, removed_parts = read_admin0()
    print(f"países={len(countries)} | nações apagadas={len(ATLAS_REMOVED)}")

    # Uma lista ÚNICA de províncias admin-1; id global = índice 1..N.
    provinces, province_countries, province_bboxes, iso_codes = read_admin1_all()
    print(f"províncias admin-1 no mundo={len(provinces)}")
    # O id GLOBAL não muda (contrato do dataset). Só o DONO muda (fusão) e a
    # presença no raster (nação apagada).
    original_countries = list(province_countries)
    province_countries = [ATLAS_OWNER.get(tag, tag) for tag in province_countries]
    kept_ids = {index + 1 for index, tag in enumerate(original_countries)
                if tag not in ATLAS_REMOVED}

    # Ocupação russa (world_nation_curation.OCCUPIED_REGIONS): reatribui o dono
    # por província e cria um id sintético para a parte ocupada das províncias
    # divididas. Os ids novos entram DEPOIS do último admin-1, na ordem da
    # fonte, então a regeneração é determinística.
    occupation = plan_occupation(iso_codes, province_bboxes)
    new_ids = sorted(spec["new_id"] for spec in occupation if spec["new_id"])
    for new_id in new_ids:
        if new_id != len(province_countries) + 1:
            raise SystemExit("ocupação: alocação de id fora de ordem")
        province_countries.append(OCCUPIED_REGIONS["target"])
    if occupation:
        print(f"ocupação: {len(occupation)} província(s) reatribuída(s), "
              f"{len(new_ids)} id(s) novo(s) para a parte ocupada")

    region_shapes, region_countries, region_ids = select_region(
        provinces, province_countries, province_bboxes, REGION)
    region_rows = [(shape, country, pid) for shape, country, pid
                   in zip(region_shapes, region_countries, region_ids)
                   if pid in kept_ids]
    region_shapes = [row[0] for row in region_rows]
    region_countries = [row[1] for row in region_rows]
    region_ids = [row[2] for row in region_rows]
    print(f"províncias na região={len(region_shapes)}")

    # Camada mundial PRIMEIRO: a regra de ilhas é decidida uma vez nela e a
    # camada da região recebe a mesma máscara (o mapa regional tem 2,6× mais
    # resolução e, sem isso, manteria uma ilha que o mundo já converteu em mar).
    world_ids = [index + 1 for index, tag in enumerate(original_countries)
                 if tag not in ATLAS_REMOVED]
    world_shapes = [shape for shape, tag in zip(provinces, original_countries)
                    if tag not in ATLAS_REMOVED]
    world_countries = [country for tag, country
                       in zip(original_countries, province_countries)
                       if tag not in ATLAS_REMOVED]
    world = build_layer("world", (-180.0, -90.0, 180.0, 90.0), WORLD_W, countries,
                        world_shapes, world_countries, world_ids, supersample=SUPERSAMPLE,
                        band_rows=512, color_step=max(WORLD_W // 4096, 1),
                        return_removed=True, islands=True, occupation=occupation)

    region = build_layer("region", REGION, REGION_W, countries,
                         region_shapes, region_countries, region_ids,
                         supersample=SUPERSAMPLE,
                         removed_reference=(world["removed_grid"],
                                            world["removed_mask"]),
                         occupation=occupation)
    with open(os.path.join(OUT, "nations.json"), "w", encoding="utf-8") as handle:
        json.dump(nation_anchors(region, countries), handle, indent="\t")
        handle.write("\n")

    write_provinces_json(region, world, province_countries, countries, occupation)
    write_minimap(world)
    write_removed_land(removed_parts)
    write_removed_islets(world["removed_mask"], world["removed_grid"])
    print(f"escrito em {OUT}")


if __name__ == "__main__":
    if "--report-islands" in sys.argv:
        report_islands()
    else:
        main()
