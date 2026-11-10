"""Gera os assets das camadas de mapa:

1. assets/ui/map_mode_political.png e map_mode_resources.png — glifos brancos
   (o HUD aplica a cor por modulate).
2. assets/icons/places/resource_dot.png e resource_wheat.png — marcadores.
3. map/map_data/atlas/resource_points.json — pontos dos depósitos minerais e
   dos territórios com trigo. O mapa desenha em billboards (nítidos em qualquer
   zoom), em vez de uma textura ampliada.

Fontes: tools/mode_icons/ (ícones, créditos no CREDITS.txt) e
data/definitions/natural/ (depósitos e agricultura do Natural Earth).

Uso: python tools/build_resource_map.py
"""

import json
import math
import os

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ICONS = os.path.join(ROOT, "tools", "mode_icons")
NATURAL = os.path.join(ROOT, "data", "definitions", "natural")
PLACES = os.path.join(ROOT, "assets", "icons", "places")

MINERALS = ["coal", "copper", "gas", "iron", "oil"]
## Códigos de status no resource_points.json (legenda do depósito).
STATUS_CODES = {"Producer": 0, "Past Producer": 1, "Prospect": 2, "Occurrence": 3, "Plant": 3}
WHEAT_MIN = 0.40
EDGE = (23, 20, 16, 235)

## Mapa de províncias (id = R + G*256) e SDF da costa (R: >128 terra) para
## marcar offshore.
PROVINCE_PATH = os.path.join(ROOT, "map", "map_data", "atlas", "world_province.png")
SDF_PATH = os.path.join(ROOT, "map", "map_data", "atlas", "world_sdf.png")

## Regra de água: nenhum recurso terrestre no mar. Petróleo/gás podem estar
## offshore (pré-sal, Mar do Norte), mas só até 20 células do nav_mask
## (~320 km da costa) — o suficiente para os campos reais, sem pontos no meio
## do oceano. A extração futura continua sendo uma regra de jogo (só na costa).
COASTAL_RESOURCES = ("oil", "gas")
COASTAL_RADIUS_CELLS = 20
MAP_W, MAP_H = 5000.0, 2500.0


def white_icon(src_name: str, dst_name: str) -> None:
    im = Image.open(os.path.join(ICONS, src_name)).convert("RGBA")
    out = Image.new("RGBA", im.size, (255, 255, 255, 255))
    out.putalpha(im.getchannel("A"))
    out = out.resize((128, 128), Image.LANCZOS)
    out.save(os.path.join(ROOT, "assets", "ui", dst_name))
    print("icon", dst_name, out.size)


def marker(size: int, shape: str) -> Image.Image:
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    c = size / 2.0
    r = size * 0.40
    if shape == "dot":
        d.ellipse([c - r, c - r, c + r, c + r], fill=(255, 255, 255, 255),
                  outline=EDGE, width=5)
    else:
        pts = [(c, c - r * 1.2), (c + r * 1.2, c), (c, c + r * 1.2), (c - r * 1.2, c)]
        d.polygon(pts, fill=(255, 255, 255, 255), outline=EDGE)
        d.line(pts + [pts[0]], fill=EDGE, width=5, joint="curve")
    return img


def star(size: int = 512) -> Image.Image:
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    cx = cy = size / 2.0
    outer = size * 0.38
    inner = size * 0.155
    points = []
    for index in range(10):
        radius = outer if index % 2 == 0 else inner
        angle = -math.pi / 2.0 + index * math.pi / 5.0
        points.append((cx + radius * math.cos(angle), cy + radius * math.sin(angle)))
    d.polygon(points, fill=(255, 255, 255, 255))
    return img


def hover_ring(size: int = 128) -> Image.Image:
    """Anel de hover em volta do depósito sob o mouse."""
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    c = size / 2.0
    r = size * 0.455
    d.ellipse([c - r, c - r, c + r, c + r], outline=(12, 12, 12, 235),
              width=max(3, size // 20))
    d.ellipse([c - r + size * 0.02, c - r + size * 0.02,
               c + r - size * 0.02, c + r - size * 0.02],
              outline=(201, 162, 74, 255), width=max(2, size // 28))
    return img


def read_json(name: str) -> dict:
    with open(os.path.join(NATURAL, name), encoding="utf-8") as handle:
        return json.load(handle)


def load_nav():
    path = os.path.join(ROOT, "map", "map_data", "nav_mask.png")
    return Image.open(path).convert("L")


def nav_cell(nav: Image.Image, px: float, py: float) -> tuple:
    return (int(px / MAP_W * nav.width), int(py / MAP_H * nav.height))


def deposit_allowed(nav: Image.Image, dep: dict) -> bool:
    """Terra pode tudo; água só petróleo/gás encostado na costa."""
    px = float(dep.get("px", 0.0))
    py = float(dep.get("py", 0.0))
    x, y = nav_cell(nav, px, py)
    if not (0 <= x < nav.width and 0 <= y < nav.height):
        return False
    if nav.getpixel((x, y)) >= 128:
        return True
    if str(dep.get("resource", "")) not in COASTAL_RESOURCES:
        return False
    for dy in range(-COASTAL_RADIUS_CELLS, COASTAL_RADIUS_CELLS + 1):
        for dx in range(-COASTAL_RADIUS_CELLS, COASTAL_RADIUS_CELLS + 1):
            nx = x + dx
            ny = y + dy
            if not (0 <= nx < nav.width and 0 <= ny < nav.height):
                continue
            if nav.getpixel((nx, ny)) >= 102:
                return True
    return False


def build_points() -> None:
    nav = load_nav()
    provinces = Image.open(PROVINCE_PATH).convert("RGB")
    sdf = Image.open(SDF_PATH).convert("RGB")
    deposits = read_json("deposits.json")["deposits"]
    minerals = []
    skipped = 0
    for dep in deposits:
        resource = str(dep.get("resource", ""))
        if resource not in MINERALS:
            continue
        if not deposit_allowed(nav, dep):
            skipped += 1
            continue
        px = float(dep.get("px", 0.0))
        py = float(dep.get("py", 0.0))
        # Província dona (id = R + G*256); offshore = pixel de água no nav_mask.
        cx = int(px / MAP_W * provinces.width)
        cy = int(py / MAP_H * provinces.height)
        pr, pg, _pb = provinces.getpixel((cx, cy))
        province_id = int(round(pr + pg * 256))
        # Costa (SDF) define offshore: R <= 128 é água.
        sx = int(px / MAP_W * sdf.width)
        sy = int(py / MAP_H * sdf.height)
        offshore = 0 if sdf.getpixel((sx, sy))[0] > 128 else 1
        minerals.append([
            round(px, 1),
            round(py, 1),
            MINERALS.index(resource),
            round(float(dep.get("quantity_kt", 0.0)), 1),
            round(float(dep.get("quality", 0.0)), 3),
            int(dep.get("depth_m", 0)),
            STATUS_CODES.get(str(dep.get("status", "")), 4),
            (str(dep.get("names", [""])[0]) if dep.get("names") else "")[:40],
            province_id,
            offshore,
        ])

    agriculture = read_json("agriculture.json")["territories"]
    nature = read_json("nature.json")["territories"]
    wheat = []
    for territory, crops in agriculture.items():
        value = float(crops.get("wheat", 0.0))
        if value < WHEAT_MIN:
            continue
        centroid = nature.get(territory, {}).get("centroid_px", [])
        if len(centroid) < 2:
            continue
        wheat.append([
            round(float(centroid[0]), 1),
            round(float(centroid[1]), 1),
            round(value, 2),
        ])

    out = os.path.join(ROOT, "map", "map_data", "atlas", "resource_points.json")
    with open(out, "w", encoding="utf-8") as handle:
        json.dump({"minerals": minerals, "wheat": wheat}, handle,
                  separators=(",", ":"))
    print("resource_points.json", len(minerals), "minerais", len(wheat), "trigo",
          "| descartados no mar:", skipped)


white_icon("political.png", "map_mode_political.png")
white_icon("resources.png", "map_mode_resources.png")
marker(64, "dot").save(os.path.join(PLACES, "resource_dot.png"))
marker(64, "diamond").save(os.path.join(PLACES, "resource_wheat.png"))
star().save(os.path.join(PLACES, "star.png"))
hover_ring().save(os.path.join(PLACES, "deposit_hover.png"))

build_points()
