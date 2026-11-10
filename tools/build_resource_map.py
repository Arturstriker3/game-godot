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
import os

from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ICONS = os.path.join(ROOT, "tools", "mode_icons")
NATURAL = os.path.join(ROOT, "data", "definitions", "natural")
PLACES = os.path.join(ROOT, "assets", "icons", "places")

MINERALS = ["coal", "copper", "gas", "iron", "oil"]
WHEAT_MIN = 0.40
EDGE = (23, 20, 16, 235)


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


def read_json(name: str) -> dict:
    with open(os.path.join(NATURAL, name), encoding="utf-8") as handle:
        return json.load(handle)


def build_points() -> None:
    deposits = read_json("deposits.json")["deposits"]
    minerals = []
    for dep in deposits:
        resource = str(dep.get("resource", ""))
        if resource not in MINERALS:
            continue
        minerals.append([
            round(float(dep.get("px", 0.0)), 1),
            round(float(dep.get("py", 0.0)), 1),
            MINERALS.index(resource),
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
    print("resource_points.json", len(minerals), "minerais", len(wheat), "trigo")


white_icon("political.png", "map_mode_political.png")
white_icon("resources.png", "map_mode_resources.png")
marker(64, "dot").save(os.path.join(PLACES, "resource_dot.png"))
marker(64, "diamond").save(os.path.join(PLACES, "resource_wheat.png"))
build_points()
