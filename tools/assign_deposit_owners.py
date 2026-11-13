"""Atribui a nação dona de cada depósito e alinha o `territory` à posse.

A posse mineral do jogo é a nação do PONTO REAL do depósito. O
`GeographyService` lê o campo `owner` (ISO-3) deste arquivo; quando ele falta,
cai na província admin-1 mais próxima (regra histórica por centróide). Este
script substitui a regra histórica por uma regra declarada em três camadas:

  1. **Offshore (petróleo/gás em água)** — linha mediana aproximada: a nação
     costeira mais próxima. Acha a célula de terra mais próxima no `nav_mask`
     e lê o país da província em `world_province.png`. É o critério que evita
     usar o preenchimento de mar do raster (que jogava o North Field em IRN,
     o Bayu-Undan em IDN etc.).
  2. **Terra perto de fronteira** — a nação da província mais próxima do
     centróide continua valendo, EXCETO quando o raster de província do ponto
     discorda e o ponto está a <= 10 km da fronteira: aí vale a nação do
     raster (tolera o erro de ~5 km que perdia Kigali e Aleppo).
  3. **Exceções explícitas** — `deposit_ownership_overrides.json`, para campos
     cujo raster/costa erra o país real (Bayu-Undan/Greater Sunrise -> TLS
     pelo Tratado do Mar de Timor 2018; Rovuma -> MOZ; Brent -> GBR).

O `territory` de cada registro é reatribuído ao território mais próximo da
nação dona, para que mapa, natureza e calibração leiam a mesma posse.

Uso: python tools/assign_deposit_owners.py
Depois: python tools/build_resource_map.py
"""

import json
import math
import os

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NATURAL = os.path.join(ROOT, "data", "definitions", "natural")
DEPOSITS = os.path.join(NATURAL, "deposits.json")
NATURE = os.path.join(NATURAL, "nature.json")
OVERRIDES = os.path.join(NATURAL, "deposit_ownership_overrides.json")
PROVINCES = os.path.join(ROOT, "map", "map_data", "atlas", "provinces.json")
WORLD_PROVINCE = os.path.join(ROOT, "map", "map_data", "atlas", "world_province.png")
NAV_MASK = os.path.join(ROOT, "map", "map_data", "nav_mask.png")

MAP_W, MAP_H = 5000.0, 2500.0
COASTAL_RESOURCES = ("oil", "gas")
## Tolerância da regra de fronteira: ~10 km. Medido nos dois casos perdidos
## (Kigali: 1 px de raster ~5 km; Aleppo: 2 px ~10 km). Acima disso a jazida é
## tratada como do vizinho (os 20 casos legítimos de Herat/Osh/Sanaa etc. têm
## raster == centróide e não entram nesta checagem).
BORDER_TOLERANCE_KM = 10.0
## Raio de busca da costa no nav_mask (~320 km, o mesmo raio costeiro do jogo).
COASTAL_SEARCH_NAV_CELLS = 20
## Janela de busca da fronteira no world_province (px de raster). 6 px ~ 24 km
## no equador e ~19 km a 37°N — cobre a tolerância de 10 km com folga.
BORDER_SEARCH_RASTER_PX = 6


def read_json(path):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def write_json(path, data):
    """Mesmo formato do `deposits.json` atual: TAB, LF e newline final."""
    text = json.dumps(data, indent="\t", ensure_ascii=False) + "\n"
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)


def load_provinces():
    provinces = read_json(PROVINCES).get("provinces", [])
    xs = np.array([float(p.get("x", 0.0)) for p in provinces])
    ys = np.array([float(p.get("y", 0.0)) for p in provinces])
    return provinces, xs, ys


def nearest_province(provinces, xs, ys, px, py):
    """Província de centróide mais próximo (empate: primeiro da ordem do
    atlas, como o `_nearest_province` do GDScript)."""
    index = int(np.argmin((xs - px) ** 2 + (ys - py) ** 2))
    return provinces[index]


def load_country_of_province(provinces):
    return {int(p.get("id", 0)): str(p.get("country", "")) for p in provinces}


def load_nav():
    return Image.open(NAV_MASK).convert("L")


def load_world_province():
    return Image.open(WORLD_PROVINCE).convert("RGB")


def raster_country(raster, country_of_province, px, py):
    rx = int(px / MAP_W * raster.width)
    ry = int(py / MAP_H * raster.height)
    if not (0 <= rx < raster.width and 0 <= ry < raster.height):
        return ""
    r, g, _b = raster.getpixel((rx, ry))
    return country_of_province.get(int(r + g * 256), "")


def nav_is_water(nav, px, py):
    nx = int(px / MAP_W * nav.width)
    ny = int(py / MAP_H * nav.height)
    if not (0 <= nx < nav.width and 0 <= ny < nav.height):
        return False
    return nav.getpixel((nx, ny)) < 128


def nearest_land_country(nav, raster, country_of_province, px, py):
    """País da célula de terra mais próxima (linha mediana aproximada).
    Retorna "" se não houver terra dentro de COASTAL_SEARCH_NAV_CELLS."""
    cx = int(px / MAP_W * nav.width)
    cy = int(py / MAP_H * nav.height)
    for radius in range(1, COASTAL_SEARCH_NAV_CELLS + 1):
        best = None
        best_distance = float("inf")
        for dy in range(-radius, radius + 1):
            for dx in range(-radius, radius + 1):
                if max(abs(dx), abs(dy)) != radius:
                    continue
                nx = cx + dx
                ny = cy + dy
                if not (0 <= nx < nav.width and 0 <= ny < nav.height):
                    continue
                if nav.getpixel((nx, ny)) < 128:
                    continue
                distance = dx * dx + dy * dy
                if distance < best_distance \
                        or (distance == best_distance and best is not None
                            and (ny, nx) < (best[1], best[0])):
                    best_distance = distance
                    best = (nx, ny)
        if best is not None:
            land_px = (best[0] + 0.5) * MAP_W / nav.width
            land_py = (best[1] + 0.5) * MAP_H / nav.height
            return raster_country(raster, country_of_province, land_px, land_py)
    return ""


def border_distance_km(raster, country_of_province, px, py, country, lat):
    """Menor distância (km) de um pixel do `country` perto do ponto. Usa a
    janela de BORDER_SEARCH_RASTER_PX; retorna inf se não achar."""
    rx = int(px / MAP_W * raster.width)
    ry = int(py / MAP_H * raster.height)
    best = float("inf")
    lon_per_px = 360.0 / raster.width
    lat_per_px = 180.0 / raster.height
    for dy in range(-BORDER_SEARCH_RASTER_PX, BORDER_SEARCH_RASTER_PX + 1):
        for dx in range(-BORDER_SEARCH_RASTER_PX, BORDER_SEARCH_RASTER_PX + 1):
            nx = min(raster.width - 1, max(0, rx + dx))
            ny = min(raster.height - 1, max(0, ry + dy))
            r, g, _b = raster.getpixel((nx, ny))
            if country_of_province.get(int(r + g * 256), "") != country:
                continue
            dlon_km = dx * lon_per_px * 111.32 * math.cos(math.radians(lat))
            dlat_km = dy * lat_per_px * 110.57
            best = min(best, math.hypot(dlon_km, dlat_km))
    return best


def nearest_territory(country_territories, nature, country, px, py):
    best = ""
    best_distance = float("inf")
    for territory in country_territories.get(country, []):
        centroid = nature.get(territory, {}).get("centroid_px", [])
        if len(centroid) < 2:
            continue
        distance = (float(centroid[0]) - px) ** 2 + (float(centroid[1]) - py) ** 2
        if distance < best_distance or (distance == best_distance and territory < best):
            best_distance = distance
            best = territory
    return best


def override_for(deposit, overrides):
    deposit_id = str(deposit.get("id", ""))
    by_id = overrides.get("by_id", {})
    if deposit_id in by_id:
        return str(by_id[deposit_id])
    by_name = overrides.get("by_name", {})
    for name in deposit.get("names", []):
        owner = by_name.get(str(name))
        if owner:
            return str(owner)
    return ""


def main():
    data = read_json(DEPOSITS)
    deposits = data.get("deposits", [])
    nature = read_json(NATURE).get("territories", {})
    overrides = read_json(OVERRIDES)
    provinces, px_array, py_array = load_provinces()
    country_of_province = load_country_of_province(provinces)
    nav = load_nav()
    raster = load_world_province()

    country_territories = {}
    territory_country = {}
    for territory, entry in nature.items():
        country = str(entry.get("adm0_a3", ""))
        territory_country[territory] = country
        country_territories.setdefault(country, []).append(territory)

    by_rule = {}
    changed_owner = 0
    changed_territory = 0
    missing_owner = 0
    for deposit in deposits:
        if deposit.get("facility"):
            continue
        px = float(deposit.get("px", 0.0))
        py = float(deposit.get("py", 0.0))
        lat = 90.0 - py / MAP_H * 180.0
        province = nearest_province(provinces, px_array, py_array, px, py)
        centroid_country = str(province.get("country", ""))
        true_country = raster_country(raster, country_of_province, px, py)
        water = nav_is_water(nav, px, py)
        rule = "centroid"
        owner = centroid_country
        if water and str(deposit.get("resource", "")) in COASTAL_RESOURCES:
            coastal = nearest_land_country(
                nav, raster, country_of_province, px, py)
            owner = coastal or centroid_country
            rule = "offshore_coast"
        elif true_country and true_country != centroid_country:
            distance = border_distance_km(
                raster, country_of_province, px, py, centroid_country, lat)
            if distance <= BORDER_TOLERANCE_KM:
                owner = true_country
                rule = "border_tolerance"
        explicit = override_for(deposit, overrides)
        if explicit:
            owner = explicit
            rule = "override"
        if not owner:
            missing_owner += 1
            continue
        if str(deposit.get("owner", "")) != owner:
            changed_owner += 1
        deposit["owner"] = owner
        # Só grava a regra quando ela não é a histórica (centróide): mantém o
        # arquivo legível e o diff restrito às exceções auditáveis.
        if rule == "centroid":
            deposit.pop("owner_rule", None)
        else:
            deposit["owner_rule"] = rule
        by_rule[rule] = int(by_rule.get(rule, 0)) + 1
        # Só mexe no `territory` quando o território atual é de OUTRO país. Se
        # já é da nação dona, ele continua sendo a região natural que contém o
        # ponto (não vale trocar pelo centróide mais próximo dentro do país).
        current_territory = str(deposit.get("territory", ""))
        if not current_territory \
                or territory_country.get(current_territory, "") != owner:
            territory = nearest_territory(
                country_territories, nature, owner, px, py)
            if territory and current_territory != territory:
                changed_territory += 1
                deposit["territory"] = territory

    kept = sum(1 for deposit in deposits if not deposit.get("facility"))
    previous = data.get("_meta", {}).get("deposit_ownership", {})
    kept_before = (data.get("_meta", {}).get("relevance_floor") or {}).get(
        "kept_total")
    data.setdefault("_meta", {})["deposit_ownership"] = {
        "applied_by": "tools/assign_deposit_owners.py",
        "rules": {
            "offshore_coast": "petróleo/gás em água: país da terra mais próxima (nav_mask + world_province)",
            "border_tolerance": "terra: raster de província vence o centróide se o ponto está a <= 10 km da fronteira",
            "centroid": "terra: nação da província de centróide mais próximo (regra histórica)",
            "override": "exceção explícita de deposit_ownership_overrides.json",
        },
        "border_tolerance_km": BORDER_TOLERANCE_KM,
        "coastal_search_nav_cells": COASTAL_SEARCH_NAV_CELLS,
        "by_rule": by_rule,
        "changed_owner": changed_owner,
        "changed_territory": changed_territory,
        "missing_owner": missing_owner,
        "overrides": overrides.get("by_name", {}),
    }
    relevance = data.get("_meta", {}).get("relevance_floor")
    if relevance is not None:
        relevance["kept_total"] = kept

    # Idempotência: uma rodada sem mudança de posse não reescreve o arquivo (e
    # não zera os contadores da rodada que de fato mudou o dado). Só grava
    # quando a posse/território mudou, quando o `_meta` ainda não existe ou
    # quando o total de jazidas mudou (suplemento/calibração).
    dirty = bool(changed_owner or changed_territory or missing_owner
                 or not previous or str(kept_before) != str(kept))
    if dirty:
        write_json(DEPOSITS, data)
    else:
        print("sem mudanças de posse: deposits.json preservado")
    print("depósitos com posse:", kept, "| owner alterado:", changed_owner,
          "| territory alterado:", changed_territory,
          "| sem nação:", missing_owner)
    for rule in sorted(by_rule):
        print("  %-16s %d" % (rule, by_rule[rule]))
    print("  overrides:", ", ".join("%s=%s" % item
          for item in sorted(overrides.get("by_name", {}).items())))


if __name__ == "__main__":
    main()
