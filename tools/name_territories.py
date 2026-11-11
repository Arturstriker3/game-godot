"""Nomeia os territórios de nature.json com a região admin-1 mais próxima do
Natural Earth (localizada em pt/en/es), para a ficha não exibir "TRT000324".

Lê o shapefile ne_10m_admin_1_states_provinces (NE_DIR) e, para cada um dos 737
territórios, acha a região cujo ponto representativo (latitude/longitude do
próprio shapefile) está mais próximo do centróide. Grava name_pt/name_en/name_es
(e name nativo + adm0_a3) no nature.json.

Sem shapely: usa distância ao ponto representativo, não point-in-polygon.

Uso: python tools/name_territories.py
"""

import json
import math
import os

import shapefile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NATURE = os.path.join(ROOT, "data", "definitions", "natural", "nature.json")
NE_DIR = os.environ.get("NE_DIR", r"G:\tools\ne_extract")
SHP = os.path.join(NE_DIR, "ne_10m_admin_1_states_provinces",
                   "ne_10m_admin_1_states_provinces")
## Natural Earth admin-0: nome localizado de cada PAÍS (adm0_a3 -> pt/en/es),
## para a legenda do mapa mostrar "Região de Estados Unidos" em vez do código
## ISO "USA". É a fonte de nomes de país já usada pelo projeto (Natural Earth).
ADM0_SHP = os.path.join(NE_DIR, "ne_10m_admin_0_countries",
                        "ne_10m_admin_0_countries")


def pixel_to_lonlat(px, py):
    return px / 5000.0 * 360.0 - 180.0, 90.0 - py / 2500.0 * 180.0


def _clean(name):
    """Remove hífens do nome (ex.: 'Île-de-France' -> 'Île de France')."""
    return str(name).replace("-", " ")


def load_country_names():
    """adm0_a3 -> {pt_BR, en, es} a partir do Natural Earth admin-0.

    Usa o shapefile de países (ne_10m_admin_0_countries) com os nomes
    localizados NAME_PT/NAME_EN/NAME_ES. Retorna {} se o shapefile faltar.
    """
    if not os.path.exists(ADM0_SHP + ".shp"):
        return {}
    sf = shapefile.Reader(ADM0_SHP)
    fields = [f[0] for f in sf.fields[1:]]
    wanted = ("ADM0_A3", "NAME_PT", "NAME_EN", "NAME_ES")
    if "ADM0_A3" not in fields:
        return {}
    idx = {name: fields.index(name) for name in wanted if name in fields}

    def cell(record, field):
        if field not in idx:
            return ""
        return _clean(str(record[idx[field]] or "")) or ""

    names = {}
    for record in sf.iterRecords():
        code = str(record[idx["ADM0_A3"]] or "").strip()
        if not code:
            continue
        en = cell(record, "NAME_EN")
        pt = cell(record, "NAME_PT")
        es = cell(record, "NAME_ES")
        names[code] = {
            "pt_BR": pt or en or code,
            "en": en or pt or code,
            "es": es or en or code,
        }
    return names


def main():
    if not os.path.exists(SHP + ".shp"):
        raise SystemExit("shapefile admin-1 ausente em " + NE_DIR)

    sf = shapefile.Reader(SHP)
    fields = [f[0] for f in sf.fields[1:]]
    need = ["name", "name_pt", "name_en", "name_es", "adm0_a3", "latitude", "longitude"]
    missing = [f for f in need if f not in fields]
    if missing:
        raise SystemExit("campos ausentes no shapefile: " + str(missing))

    regions = []
    for record in sf.iterRecords():
        lat = record[fields.index("latitude")]
        lon = record[fields.index("longitude")]
        if lat is None or lon is None:
            continue
        regions.append({
            "name": record[fields.index("name")] or "",
            "name_pt": record[fields.index("name_pt")] or "",
            "name_en": record[fields.index("name_en")] or "",
            "name_es": record[fields.index("name_es")] or "",
            "adm0_a3": record[fields.index("adm0_a3")] or "",
            "lat": float(lat), "lon": float(lon),
        })

    data = json.load(open(NATURE, encoding="utf-8"))
    territories = data["territories"]

    # adm0_a3 vem do DONO do território (fonte de verdade: provinces.json/
    # territories.json via o campo "owner" de build_natural_geography.py), NÃO
    # da região admin-1 mais próxima. A base de 250 territórios agrupa províncias
    # ultramarinas junto do país (ex.: NED = Holanda + Caribe, FRA = França +
    # Guiana), puxando o centróide para o mar/outro país — a região mais próxima
    # vira de país errado (La Coruña/ESP para a Holanda). Só a Holanda diverge
    # entre o tag do jogo (NED) e o ISO-3 do Natural Earth (NLD).
    OWNER_TO_ISO3 = {"NED": "NLD"}

    def owner_iso3(tag):
        tag = str(tag or "").strip()
        return OWNER_TO_ISO3.get(tag, tag)

    # Índice das regiões admin-1 por país, para o nome vir de uma região DENTRO
    # do país do dono (não da região estrangeira mais próxima do centróide).
    regions_by_country = {}
    for region in regions:
        regions_by_country.setdefault(region["adm0_a3"], []).append(region)

    named = 0
    for territory_id, entry in territories.items():
        iso3 = owner_iso3(entry.get("owner", ""))
        if iso3:
            entry["adm0_a3"] = iso3
        centroid = entry.get("centroid_px", [])
        if len(centroid) < 2:
            continue
        lon, lat = pixel_to_lonlat(float(centroid[0]), float(centroid[1]))
        best = None
        best_dist = float("inf")
        candidates = regions_by_country.get(iso3) or regions
        for region in candidates:
            dlon = lon - region["lon"]
            dlat = lat - region["lat"]
            dist = dlon * dlon + dlat * dlat
            if dist < best_dist:
                best_dist = dist
                best = region
        if best is None:
            continue
        entry["name"] = _clean(best["name"])
        entry["name_pt"] = _clean(best["name_pt"] or best["name"])
        entry["name_en"] = _clean(best["name_en"] or best["name"])
        entry["name_es"] = _clean(best["name_es"] or best["name"])
        named += 1

    # Nomes de país (para a legenda "Região de {país}") em pt/en/es.
    country_names = load_country_names()
    if country_names:
        data["country_names"] = country_names

    text = json.dumps(data, indent="\t", ensure_ascii=False).replace("\n", "\r\n") + "\r\n"
    with open(NATURE, "w", encoding="utf-8", newline="") as handle:
        handle.write(text)
    print("territórios nomeados:", named, "de", len(territories),
          "| países nomeados:", len(country_names))


if __name__ == "__main__":
    main()
