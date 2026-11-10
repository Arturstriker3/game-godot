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
## Distância máxima (em células do nav_mask) para encostar um ponto de trigo
## cujo centróide caiu em água. 40 células ~ 80 px de mapa (~15 km); além
## disso é ilha remota e o ponto é descartado.
WHEAT_SNAP_MAX_NAV_PX = 40
EDGE = (23, 20, 16, 235)

## Mapa de províncias (id = R + G*256) e máscara de navegação para marcar
## offshore e validar pontos de trigo.
PROVINCE_PATH = os.path.join(ROOT, "map", "map_data", "atlas", "world_province.png")

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


def nearest_land_px(nav: Image.Image, px: float, py: float,
                    max_cells: int):
    """Terra mais próxima de (px, py) em células do nav_mask, ou None se não
    houver terra dentro de max_cells. Retorna (px, py) no centro da célula."""
    cx, cy = nav_cell(nav, px, py)
    for radius in range(1, max_cells + 1):
        for dy in range(-radius, radius + 1):
            for dx in range(-radius, radius + 1):
                if max(abs(dx), abs(dy)) != radius:
                    continue
                nx, ny = cx + dx, cy + dy
                if not (0 <= nx < nav.width and 0 <= ny < nav.height):
                    continue
                if nav.getpixel((nx, ny)) >= 128:
                    return ((nx + 0.5) * MAP_W / nav.width,
                            (ny + 0.5) * MAP_H / nav.height)
    return None


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
    deposits = read_json("deposits.json")["deposits"]
    minerals = []
    skipped = 0
    skipped_facility = 0
    for dep in deposits:
        resource = str(dep.get("resource", ""))
        if resource not in MINERALS:
            continue
        if dep.get("facility"):
            skipped_facility += 1
            continue  # fundição/refinaria: não é depósito mineral
        if not deposit_allowed(nav, dep):
            skipped += 1
            continue
        px = float(dep.get("px", 0.0))
        py = float(dep.get("py", 0.0))
        # Província dona (id = R + G*256); offshore = pixel de água no nav_mask
        # (a mesma máscara que o jogo usa para terra/água, sem artefatos de SDF).
        cx = int(px / MAP_W * provinces.width)
        cy = int(py / MAP_H * provinces.height)
        pr, pg, _pb = provinces.getpixel((cx, cy))
        province_id = int(round(pr + pg * 256))
        nx, ny = nav_cell(nav, px, py)
        offshore = 0 if nav.getpixel((nx, ny)) >= 128 else 1
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
        px, py = float(centroid[0]), float(centroid[1])
        # Centróide em água (ilhas/arquipélagos): encosta na terra mais próxima
        # se estiver perto; senão descarta (ilha remota, o marcador não cabe).
        if nav.getpixel(nav_cell(nav, px, py)) < 128:
            landed = nearest_land_px(nav, px, py, WHEAT_SNAP_MAX_NAV_PX)
            if landed is None:
                continue
            px, py = landed
        wheat.append([
            round(px, 1),
            round(py, 1),
            round(value, 2),
        ])

    out = os.path.join(ROOT, "map", "map_data", "atlas", "resource_points.json")
    with open(out, "w", encoding="utf-8") as handle:
        json.dump({"minerals": minerals, "wheat": wheat}, handle,
                  separators=(",", ":"))
    print("resource_points.json", len(minerals), "minerais", len(wheat), "trigo",
          "| descartados no mar:", skipped, "| facilities:", skipped_facility)


white_icon("political.png", "map_mode_political.png")
white_icon("resources.png", "map_mode_resources.png")
marker(64, "dot").save(os.path.join(PLACES, "resource_dot.png"))
marker(64, "diamond").save(os.path.join(PLACES, "resource_wheat.png"))
star().save(os.path.join(PLACES, "star.png"))
hover_ring().save(os.path.join(PLACES, "deposit_hover.png"))

build_points()
