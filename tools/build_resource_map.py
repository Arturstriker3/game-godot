"""Gera os assets das camadas de mapa:

1. assets/ui/map_mode_political.png e map_mode_resources.png — glifos brancos
   (o HUD aplica a cor por modulate).
2. map/map_data/atlas/resources.png — mapa de recursos: pontos coloridos para
   os depósitos minerais e losangos para o trigo (aptidão por território).

Fontes: tools/mode_icons/ (ícones, créditos no CREDITS.txt) e
data/definitions/natural/ (depósitos e agricultura do Natural Earth).

Uso: python tools/build_resource_map.py
"""

import json
import os

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ICONS = os.path.join(ROOT, "tools", "mode_icons")
NATURAL = os.path.join(ROOT, "data", "definitions", "natural")

RESOURCE_COLORS = {
    "coal": (62, 59, 56),
    "copper": (192, 106, 60),
    "gas": (78, 159, 176),
    "iron": (124, 136, 148),
    "oil": (91, 74, 46),
    "wheat": (217, 180, 90),
}
EDGE = (23, 20, 16, 210)

# 4096 × 2048 = mesma proporção do plano (1000 × 500).
W, H = 4096, 2048
WHEAT_MIN = 0.40


def white_icon(src_name: str, dst_name: str) -> None:
    im = Image.open(os.path.join(ICONS, src_name)).convert("RGBA")
    out = Image.new("RGBA", im.size, (255, 255, 255, 255))
    out.putalpha(im.getchannel("A"))
    out = out.resize((128, 128), Image.LANCZOS)
    out.save(os.path.join(ROOT, "assets", "ui", dst_name))
    print("icon", dst_name, out.size)


def read_json(name: str) -> dict:
    with open(os.path.join(NATURAL, name), encoding="utf-8") as handle:
        return json.load(handle)


def to_px(px: float, py: float) -> tuple:
    return (px / 5000.0 * W, py / 2500.0 * H)


def build_resources() -> None:
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)

    deposits = read_json("deposits.json")["deposits"]
    counts = {}
    for dep in deposits:
        resource = str(dep.get("resource", ""))
        color = RESOURCE_COLORS.get(resource)
        if color is None:
            continue
        x, y = to_px(float(dep.get("px", 0.0)), float(dep.get("py", 0.0)))
        r = 2.0
        draw.ellipse([x - r - 0.8, y - r - 0.8, x + r + 0.8, y + r + 0.8], fill=EDGE)
        draw.ellipse([x - r, y - r, x + r, y + r], fill=color + (255,))
        counts[resource] = counts.get(resource, 0) + 1

    agriculture = read_json("agriculture.json")["territories"]
    nature = read_json("nature.json")["territories"]
    wheat_count = 0
    for territory, crops in agriculture.items():
        value = float(crops.get("wheat", 0.0))
        if value < WHEAT_MIN:
            continue
        centroid = nature.get(territory, {}).get("centroid_px", [])
        if len(centroid) < 2:
            continue
        x, y = to_px(float(centroid[0]), float(centroid[1]))
        size = 3.2
        alpha = int(120 + 135 * min(1.0, (value - WHEAT_MIN) / 0.35))
        draw.polygon([(x, y - size), (x + size, y), (x, y + size), (x - size, y)],
                     fill=RESOURCE_COLORS["wheat"] + (alpha,))
        wheat_count += 1

    out = os.path.join(ROOT, "map", "map_data", "atlas", "resources.png")
    img.save(out)
    print("resources.png", img.size, counts, "wheat", wheat_count)


white_icon("political.png", "map_mode_political.png")
white_icon("resources.png", "map_mode_resources.png")
build_resources()
