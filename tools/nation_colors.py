"""Cores das nações do mapa político: tradicionais + paleta automática.

Fonte de verdade da cor de cada nação:
1. `data/definitions/world/traditional_colors.csv` — cores historicamente
   associadas, já no tom "papelaria" (levemente dessaturado). Têm PRIORIDADE:
   nenhuma outra regra troca a família da cor (matiz) de uma nação tradicional.
2. Paleta automática — a cor que o importador escolheu por adjacência
   (`COLOR_OVERRIDE` + ΔE ≥ 30) e que o build apenas dessatura (×0,86).

A adjacência é de TERRA (mesma regra do mapa: `world_province.png` com a máscara
de `world_sdf.png`). Pares vizinhos com ΔE < 25 são inaceitáveis; o alvo é 30,
o mesmo piso dos pares puramente automáticos. Quando uma cor tradicional fica
parecida com a de um vizinho, o TOM dela é ajustado dentro da família
(matiz ±12°, luminosidade ±10%, saturação ±10%) — nunca a matiz de outra nação.
Se ainda assim não houver candidato, a nação automática vizinha cede.

O resultado também respeita a rede de segurança do runtime: distância RGB
euclidiana ≥ 0,16 entre vizinhos (`territory_map.gd`, `COLOR_MIN_DISTANCE`),
para `_build_nation_palette` não substituir a cor do CSV por um tom de papelaria.

Uso programático:
    from nation_colors import load_traditional, adjacency, assign_colors
    cores, relatorio = assign_colors(base, traditional, adjacency, ordem)

Uso de linha de comando (regenera só a coluna `color` do cenário):
    <python> tools/apply_traditional_colors.py
"""
from __future__ import annotations

import colorsys
import csv
import json
import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
WORLD = os.path.join(ROOT, "data", "definitions", "world")
ATLAS = os.path.join(ROOT, "map", "map_data", "atlas")
TRADITIONAL_PATH = os.path.join(WORLD, "traditional_colors.csv")
ADJACENCY_CACHE = os.path.join(WORLD, "nation_adjacency.json")

# Piso aceito e alvo desejado de ΔE(Lab, CIE76) entre vizinhos de terra.
MIN_DELTA_E = 25.0
TARGET_DELTA_E = 30.0
# Distância RGB mínima aceita pelo runtime (`territory_map.gd`).
MIN_RGB_DISTANCE = 0.16
# Ajuste de tom permitido em torno da cor pedida: mantém a família.
HUE_OFFSETS_DEG = (0.0, 6.0, -6.0, 12.0, -12.0)
LIGHT_FACTORS = (1.0, 0.92, 1.08, 0.84, 1.16)
SAT_FACTORS = (1.0, 0.88, 1.12)


def hex_rgb(value):
    value = value.strip().lstrip("#")
    if len(value) != 6:
        raise ValueError(f"cor inválida: {value!r}")
    return tuple(int(value[i:i + 2], 16) for i in (0, 2, 4))


def rgb_hex(rgb):
    return "#%02x%02x%02x" % tuple(max(0, min(255, int(round(c)))) for c in rgb)


def rgb_hls(value):
    return colorsys.rgb_to_hls(*(c / 255.0 for c in hex_rgb(value)))


def hls_hex(hue, light, sat):
    light = max(0.12, min(0.90, light))
    sat = max(0.0, min(1.0, sat))
    return rgb_hex(tuple(c * 255.0 for c in colorsys.hls_to_rgb(hue % 1.0, light, sat)))


def _paper_palette():
    """Tons "papelaria" cobrindo o círculo cromático (fallback de segurança).

    Só entra quando a família da cor automática não consegue se afastar de uma
    vizinha tradicional. Ordem fixa: o resultado é determinístico.
    """
    palette = []
    for hue in range(0, 360, 20):
        for light, sat in ((0.50, 0.34), (0.62, 0.28), (0.74, 0.22)):
            palette.append(hls_hex(hue / 360.0, light, sat))
    return palette


PAPER_PALETTE = _paper_palette()


def rgb_lab(rgb):
    def lin(c):
        c = c / 255.0
        return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4
    r, g, b = (lin(c) for c in rgb)
    x = (r * 0.4124 + g * 0.3576 + b * 0.1805) / 0.95047
    y = (r * 0.2126 + g * 0.7152 + b * 0.0722)
    z = (r * 0.0193 + g * 0.1192 + b * 0.9505) / 1.08883
    def f(t):
        return t ** (1 / 3) if t > 0.008856 else 7.787 * t + 16 / 116
    return 116 * f(y) - 16, 500 * (f(x) - f(y)), 200 * (f(y) - f(z))


def delta_e(a, b):
    la, lb = rgb_lab(hex_rgb(a)), rgb_lab(hex_rgb(b))
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(la, lb)))


def rgb_distance(a, b):
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(hex_rgb(a), hex_rgb(b)))) / 255.0


# --------------------------------------------------------------- entradas
def load_traditional(path=TRADITIONAL_PATH):
    """Lê iso3 -> cor final (tom papelaria). Valida o formato e a unicidade."""
    out = {}
    with open(path, encoding="utf-8-sig", newline="") as fh:
        reader = csv.DictReader(fh)
        for row in reader:
            tag = (row.get("iso3") or "").strip().upper()
            color = (row.get("color_hex") or "").strip().lower()
            if not tag:
                continue
            if not color.startswith("#") or len(color) != 7:
                raise ValueError(f"{path}: {tag} com cor inválida {color!r}")
            hex_rgb(color)
            if tag in out:
                raise ValueError(f"{path}: {tag} duplicado")
            out[tag] = color
    return out


def load_scenario_colors(path):
    with open(path, encoding="utf-8-sig", newline="") as fh:
        return {row["id"].strip(): row["color"].strip().lower()
                for row in csv.DictReader(fh) if row.get("color", "").strip()}


def adjacency_from_rasters(atlas_dir=ATLAS, min_pixels=10):
    """Adjacência de TERRA entre tags do mapa (dict tag -> lista ordenada).

    Duas províncias vizinhas em pixels do raster, ambos os lados em terra, com
    pelo menos `min_pixels` contatos, viram uma aresta entre as nações. A
    máscara de terra é o canal R do `world_sdf.png` (>128), a mesma do shader.
    """
    import numpy as np
    from PIL import Image

    provinces = json.load(open(os.path.join(atlas_dir, "provinces.json"),
                               encoding="utf-8"))["provinces"]
    tags = sorted({p["nation"] for p in provinces})
    index = {t: i for i, t in enumerate(tags)}
    by_id = np.zeros(65536, dtype=np.int16)
    for province in provinces:
        by_id[int(province["id"])] = index[province["nation"]]

    im = np.asarray(Image.open(os.path.join(atlas_dir, "world_province.png")))
    pid = im[:, :, 0].astype(np.uint16) + im[:, :, 1].astype(np.uint16) * 256
    tag = by_id[pid]
    del pid, im
    sdf = np.asarray(Image.open(os.path.join(atlas_dir, "world_sdf.png")))
    land = sdf[:, :, 0] > 128
    del sdf

    counts = {}

    def add(a, b, mask):
        m = mask & (a != b)
        key = a[m].astype(np.int32) * 256 + b[m].astype(np.int32)
        values, freqs = np.unique(key, return_counts=True)
        for value, count in zip(values.tolist(), freqs.tolist()):
            x, y = divmod(value, 256)
            pair = (min(x, y), max(x, y))
            counts[pair] = counts.get(pair, 0) + count

    add(tag[:, :-1], tag[:, 1:], land[:, :-1])
    add(tag[:-1, :], tag[1:, :], land[:-1, :])

    graph = {t: set() for t in tags}
    for (x, y), count in counts.items():
        if count < min_pixels:
            continue
        graph[tags[x]].add(tags[y])
        graph[tags[y]].add(tags[x])
    return {t: sorted(v) for t, v in sorted(graph.items())}


def load_adjacency(path=ADJACENCY_CACHE):
    """Adjacência do cache; se o raster mudou, `refresh_adjacency` regrava."""
    with open(path, encoding="utf-8") as fh:
        return {k: list(v) for k, v in json.load(fh)["adjacency"].items()}


def load_or_refresh_adjacency(path=ADJACENCY_CACHE):
    """Usa o cache commitado; se ele não existir, recalcula do raster."""
    if os.path.exists(path):
        return load_adjacency(path)
    return refresh_adjacency(path)


def refresh_adjacency(path=ADJACENCY_CACHE):
    graph = adjacency_from_rasters()
    payload = {
        "generated_by": "tools/nation_colors.py",
        "rule": "world_province.png + máscara de terra world_sdf.png (R>128); "
                "pares com >= 10 px de contato",
        "adjacency": graph,
    }
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(payload, fh, ensure_ascii=False, indent=1, sort_keys=True)
        fh.write("\n")
    return graph


# --------------------------------------------------------------- resolução
def _candidates(color, traditional):
    """Cores candidatas na mesma família, da mais próxima à mais distante."""
    hue, light, sat = rgb_hls(color)
    hue_span = HUE_OFFSETS_DEG if traditional else HUE_OFFSETS_DEG[:3]
    light_span = LIGHT_FACTORS if traditional else LIGHT_FACTORS[:3]
    sat_span = SAT_FACTORS if traditional else SAT_FACTORS[:3]
    ordered = []
    for hue_offset in hue_span:
        for light_factor in light_span:
            for sat_factor in sat_span:
                candidate = hls_hex(hue + hue_offset / 360.0,
                                    light * light_factor, sat * sat_factor)
                if candidate not in ordered:
                    ordered.append(candidate)
    return ordered


def _fits(candidate, neighbors):
    for other in neighbors:
        if delta_e(candidate, other) < MIN_DELTA_E:
            return False
        if rgb_distance(candidate, other) < MIN_RGB_DISTANCE:
            return False
    return True


def _score(candidate, neighbors):
    if not neighbors:
        return 100.0
    return min(delta_e(candidate, other) for other in neighbors)


def _pick(candidate_base, neighbors, traditional, used):
    """Melhor cor: família primeiro; a automática pode cair na paleta de papel.

    A tradicional nunca muda de família (matiz) — é o pedido do usuário. A
    automática, se a família não resolver, usa a paleta papery completa.
    `used` evita repetir um tom já aplicado a outra nação (variedade do mapa).
    """
    family = _candidates(candidate_base, traditional)
    if traditional:
        pools = [family]
    else:
        broad = sorted(PAPER_PALETTE, key=lambda c: (delta_e(c, candidate_base), c))
        pools = [family, broad]
    for require_unused in (True, False):
        for candidates in pools:
            for candidate in candidates:
                if require_unused and candidate in used:
                    continue
                if _score(candidate, neighbors) >= TARGET_DELTA_E and _fits(candidate, neighbors):
                    return candidate, True
    best, best_score, ok = candidate_base, -1.0, False
    for candidates in pools:
        for candidate in candidates:
            score = _score(candidate, neighbors)
            if score > best_score and _fits(candidate, neighbors):
                best, best_score, ok = candidate, score, True
    if ok:
        return best, False
    # Nenhum candidato passa do piso: escolhe o de maior ΔE (fica registrado).
    flat = [c for candidates in pools for c in candidates]
    return max(flat, key=lambda c: (_score(c, neighbors), -flat.index(c))), False


def assign_colors(base_colors, traditional, graph, order):
    """Resolve a cor final de cada nação.

    `order` define a prioridade (nações maiores primeiro). Tradicionais são
    resolvidas antes das automáticas, então a família delas nunca cede a uma
    cor automática; se duas tradicionais vizinhas coincidem, a de menor
    prioridade é que muda de TOM.
    """
    traditional = {t: c.lower() for t, c in traditional.items()}
    colors = {}
    used = set()
    changed = []

    def assign(tag, base, is_traditional):
        neighbors = [colors[n] for n in graph.get(tag, ()) if n in colors]
        if not neighbors:
            colors[tag] = base
            used.add(base)
            return
        if _score(base, neighbors) >= TARGET_DELTA_E and _fits(base, neighbors):
            colors[tag] = base
            used.add(base)
            return
        chosen, reached_target = _pick(base, neighbors, is_traditional, used)
        colors[tag] = chosen
        used.add(chosen)
        changed.append({
            "tag": tag,
            "kind": "traditional" if is_traditional else "auto",
            "from": base,
            "to": chosen,
            "min_delta_e": round(_score(chosen, neighbors), 1),
            "target_met": reached_target,
        })

    ordered = [t for t in order if t in base_colors]
    ordered += [t for t in sorted(base_colors) if t not in ordered]
    for tag in ordered:
        if tag in traditional:
            assign(tag, traditional[tag], True)
    for tag in ordered:
        if tag in traditional or tag in colors:
            continue
        assign(tag, base_colors[tag], False)

    return colors, _report(colors, graph, traditional, changed)


def _report(colors, graph, traditional, changed):
    pairs = []
    for tag, neighbors in graph.items():
        if tag not in colors:
            continue
        for other in neighbors:
            if other not in colors or other <= tag:
                continue
            value = delta_e(colors[tag], colors[other])
            pairs.append((round(value, 1), tag, other))
    pairs.sort()
    below_target = [p for p in pairs if p[0] < TARGET_DELTA_E]
    below_floor = [p for p in pairs if p[0] < MIN_DELTA_E]
    rgb_below = []
    for tag, neighbors in graph.items():
        if tag not in colors:
            continue
        for other in neighbors:
            if other not in colors or other <= tag:
                continue
            value = rgb_distance(colors[tag], colors[other])
            if value < MIN_RGB_DISTANCE:
                rgb_below.append((round(value, 3), tag, other))
    rgb_below.sort()
    return {
        "min_delta_e": pairs[0][0] if pairs else None,
        "min_pair": list(pairs[0]) if pairs else None,
        "pairs": len(pairs),
        "below_target": [list(p) for p in below_target],
        "below_floor": [list(p) for p in below_floor],
        "rgb_below_min": [list(p) for p in rgb_below],
        "changed": changed,
        "traditional_count": sum(1 for t in traditional if t in colors),
    }
