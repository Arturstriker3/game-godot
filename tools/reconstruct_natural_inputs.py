"""Reconstrói os inputs ausentes do pipeline de geografia natural.

Lê `map/map_data/atlas/provinces.json` (lista canônica de províncias admin-1
em projeção equirretangular 5000×2500 — a mesma que o jogo usa em
`TerritoryMap`) e deriva, no formato consumido por
`tools/build_natural_geography.py`:

  data/definitions/provinces.txt              id;kind;r;g;b;px;py;terreno
  data/history/provinces/provinces.json       província -> dono
  data/history/territories/territories.json   território -> [províncias]

Não escreve em `map/map_data/*` nem em `data/definitions/natural/*`.

Divergência conhecida (ver `docs/debug/reproducibility_report.md`): os dados
naturais commitados foram gerados por um gerador legado Voronoi (~14.402
províncias em 8192× lat −60..84, 737 territórios). Esta reconstrução usa as
4.529 províncias admin-1 do atlas e agrupa **um território por nação**
(estado = nação, mesmo desenho do gerador legado).
"""

import colorsys
import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ATLAS = os.path.join(ROOT, "map", "map_data", "atlas", "provinces.json")
DEFS = os.path.join(ROOT, "data", "definitions")
HIST = os.path.join(ROOT, "data", "history")


def key_colors(count):
    """Cores-chave únicas e determinísticas (mesma família do gerador legado)."""
    colors = []
    used = {(0, 0, 0)}
    index = 0
    while len(colors) < count:
        hue = (index * 0.618033988749895) % 1.0
        sat = 0.45 + 0.55 * (((index * 37) % 211) / 210.0)
        val = 0.45 + 0.55 * (((index * 89) % 223) / 222.0)
        r, g, b = colorsys.hsv_to_rgb(hue, sat, val)
        key = (int(r * 255), int(g * 255), int(b * 255))
        if key not in used:
            used.add(key)
            colors.append(key)
        index += 1
    return colors


def main():
    with open(ATLAS, encoding="utf-8") as handle:
        data = json.load(handle)
    provinces = data["provinces"]
    colors = key_colors(len(provinces))

    def nation_of(province):
        return province.get("nation") or province.get("country") or "NNN"

    # provinces.txt — o id preserva o id global do atlas (PRV%06d), para que o
    # mesmo ponto (px, py) mapeie para a MESMA província que o raster
    # world_province.png (id = R + G*256) usado por build_resource_map.py.
    os.makedirs(DEFS, exist_ok=True)
    with open(os.path.join(DEFS, "provinces.txt"), "w", encoding="utf-8") as handle:
        for province, (r, g, b) in zip(provinces, colors):
            handle.write(
                f"PRV{province['id']:06d};land;{r};{g};{b};"
                f"{province['x']};{province['y']};PLAINS\n"
            )

    province_history = {}
    for province in provinces:
        pid = f"PRV{province['id']:06d}"
        nation = nation_of(province)
        province_history[pid] = {
            "province_owner": nation,
            "province_controller": nation,
        }
    os.makedirs(os.path.join(HIST, "provinces"), exist_ok=True)
    with open(os.path.join(HIST, "provinces", "provinces.json"), "w", encoding="utf-8") as handle:
        json.dump(province_history, handle, indent="\t", ensure_ascii=False)
        handle.write("\n")

    # territories.json — um território por nação (estado = nação).
    grouped = {}
    for province in provinces:
        grouped.setdefault(nation_of(province), []).append(
            f"PRV{province['id']:06d}"
        )
    territories = {}
    for index, nation in enumerate(sorted(grouped), start=1):
        territories[f"TRT{index:06d}"] = {"provinces": grouped[nation]}
    os.makedirs(os.path.join(HIST, "territories"), exist_ok=True)
    with open(os.path.join(HIST, "territories", "territories.json"), "w", encoding="utf-8") as handle:
        json.dump(territories, handle, indent="\t", ensure_ascii=False)
        handle.write("\n")

    print(f"provinces.txt:      {len(provinces)} províncias")
    print(f"provinces.json:     {len(province_history)} donos")
    print(f"territories.json:   {len(territories)} territórios (por nação)")


if __name__ == "__main__":
    main()
