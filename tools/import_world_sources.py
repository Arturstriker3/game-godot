"""Importa as fontes do mundo (2026) para data/definitions/world/*.csv.

Etapa de *dados* (não roda Godot, não toca data/definitions/scenario/).
Lê snapshots baixados de:
    World Bank WDI  SP.POP.TOTL / NY.GDP.MKTP.PP.CD / NV.IND.MANF.ZS /
                    NY.GDP.TOTL.RT.ZS / NV.AGR.TOTL.ZS
    GeoNames       countryInfo.txt, cities15000.txt, cities500.txt
    Natural Earth  map/map_data/atlas/world_province.png, world_sdf.png,
                   provinces.json
    país/nome i18n data/definitions/natural/country_names.json
    Wikidata       rótulos pt-br/pt/es das cidades (fallback: inglês)

Regras (docs/plans/world_all_data.md):
    cota = 1 (capital) + clamp(1 + floor(pop/25M) + floor(PIB_PPP_bn/1500)
                               + floor(área_km²/600k), 1, 24)
    cobertura: toda cidade-fonte (GeoNames, pop ≥ 1.000) a ≤ 700 km de uma
               cidade escolhida da mesma nação.
    fronteira: cidade nova a ≥ 40 km de outro país (≥ 25 km se área < 50.000 km²);
               capital ou cidade entre as 5 maiores da nação pode ficar e é marcada.
    porto: cidade a ≤ 50 km da costa; ponto de costa a ≥ 15 km do centro vira
           porto separado; abaixo disso a cidade é marcada como porto dela mesma.
    cor: vizinhos de fronteira com ΔE ≥ 30 (Lab); correções em COLOR_OVERRIDE.

Uso:
    python tools/import_world_sources.py --cache <dir> [--write] [--fetch-names]
    python tools/import_world_sources.py --check --cache <dir>

`--cache` é o diretório com os snapshots baixados (não versionados).
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import os
import re
import sys
import time
import unicodedata
import urllib.parse
import urllib.request
from collections import defaultdict

import numpy as np
from PIL import Image
from scipy import ndimage

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
WORLD = os.path.join(ROOT, "data", "definitions", "world")
SCENARIO = os.path.join(ROOT, "data", "definitions", "scenario")
ATLAS = os.path.join(ROOT, "map", "map_data", "atlas")
COUNTRY_NAMES = os.path.join(ROOT, "data", "definitions", "natural", "country_names.json")

MAX_CITIES = 25          # 1 capital + clamp(..., 1, 24)
COVER_KM = 700.0
BORDER_KM = 40.0
BORDER_SMALL_KM = 25.0
SMALL_AREA_KM2 = 50_000.0
MIN_SPACING_KM = 25.0
SUBURB_KM = 60.0
POP_PER_TON = 0.0003
PORT_MAX_CITY_COAST_KM = 25.0
PORT_MIN_POINT_KM = 15.0

UN = """AFG ALB DZA AND AGO ATG ARG ARM AUS AUT AZE BHS BHR BGD BRB BLR BEL BLZ BEN BTN BOL BIH BWA BRA BRN BGR BFA BDI CPV KHM CMR CAN CAF TCD CHL CHN COL COM COG CRI CIV HRV CUB CYP CZE COD DNK DJI DMA DOM ECU EGY SLV GNQ ERI EST SWZ ETH FJI FIN FRA GAB GMB GEO DEU GHA GRC GRD GTM GIN GNB GUY HTI HND HUN ISL IND IDN IRN IRQ IRL ISR ITA JAM JPN JOR KAZ KEN KIR PRK KOR KWT KGZ LAO LVA LBN LSO LBR LBY LIE LTU LUX MDG MWI MYS MDV MLI MLT MHL MRT MUS MEX FSM MDA MCO MNG MNE MAR MOZ MMR NAM NRU NPL NLD NZL NIC NER NGA MKD NOR OMN PAK PLW PAN PNG PRY PER PHL POL PRT QAT ROU RUS RWA KNA LCA VCT WSM SMR STP SAU SEN SRB SYC SLE SGP SVK SVN SLB SOM ZAF SDS ESP LKA SDN SUR SWE CHE SYR TJK TZA THA TLS TGO TON TTO TUN TUR TKM TUV UGA UKR ARE GBR USA URY UZB VUT VEN VNM YEM ZMB ZWE""".split()
OBSERVERS = ["VAT", "PSE"]
SPECIAL = ["TWN", "XKX", "SAH"]
TERRITORIES = """GRL FRO PRI NCL PYF GUM ASM MNP VIR HKG MAC CUW ABW SXM BMU CYM TCA VGB
AIA MSR SHN COK NIU WLF SPM MAF BLM FLK NFK GIB IMN JEY GGY ALD""".split()

NAME_ALIAS = {"NED": "NLD"}
TAG_TO_COUNTRYINFO = {
    "NED": "NLD", "XKX": "XKX", "PSX": "PSE", "SAH": "ESH",
    "SDS": "SSD", "ALD": "ALA", "KOS": "XKX",
}
# códigos ISO2 do GeoNames que pertencem a uma nação do mapa
CITY_COUNTRY_ALIAS = {
    "GF": "FRA", "GP": "FRA", "MQ": "FRA", "RE": "FRA", "YT": "FRA",
    "SJ": "NOR", "AX": "ALD",
}

CONTINENT = {"EU": "europe", "AS": "asia", "AF": "africa", "NA": "north_america",
             "SA": "south_america", "OC": "oceania", "AN": "antarctica"}
MIDDLE_EAST = {"BHR", "IRN", "IRQ", "ISR", "JOR", "KWT", "LBN", "OMN", "QAT",
               "SAU", "SYR", "ARE", "YEM", "PSX"}

MONARCHY = set("""AND ATG AUS BHS BHR BEL BLZ BTN CAN DNK ESP GBR GRD JAM JOR JPN
KHM KNA KWT LCA LES LIE LUX MAR MCO MYS NLD NOR NZL OMN PNG QAT SAU SLB SWE SWZ
THA TON TUV ARE VAT VCT BRN""".split())
FEDERATION = set("""ARG AUS AUT BEL BIH BRA CAN CHE COM DEU ETH FSM IND IRQ MEX MYS
NGA NPL PAK RUS SDN SDS SOM USA VEN ARE""".split())
CITY_STATE = {"SGP", "VAT"}

# Nomes pt-BR onde o rótulo `pt-br` do Wikidata repete a variante lusitana.
PT_BR_OVERRIDE = {"Nova Iorque": "Nova York"}

# Nomes correntes que o GeoNames guarda em forma longa/transliterada.
# (nome GeoNames: (inglês, pt-BR, espanhol)) — fonte: rótulos do Wikidata da
# cidade corrente (ex.: Mosul Q83317 → pt "Mossul").
NAME_OVERRIDE = {
    "Al Mawşil al Jadīdah": ("Mosul", "Mossul", "Mosul"),
    "Şalālah": ("Salalah", "Salalah", "Salalah"),
    "‘Ibrī": ("Ibri", "Ibri", "Ibri"),
    "Chişinău": ("Chisinau", "Chisinau", "Chisinau"),
}

# Cores ajustadas à mão depois da seleção: vizinhos com ΔE < 30 (LIMIAR_COR)
# passavam despercebidos porque as cores fixas do atlas não entram no reparo.
# Ver docs/plans/world_all_data.md, seção "Cores".
COLOR_OVERRIDE = {
    "BEL": "#7ba744",   # era #b69c4a, ΔE 25,1 de NED (laranja)
    "FRA": "#4462a7",   # era #506c9c, ΔE 25,3 de DEU (cinza)
    "SUR": "#448e5a",   # era #488ca0, ΔE 24,5 de FRA (Guiana Francesa)
}
LIMIAR_COR = 30.0

# massas de terra nomeadas; ilhas não listadas recebem `island_<tag>_<componente>`
ISLAND_TOKEN = {
    "ISL": "iceland", "IRL": "ireland", "GBR": "britain", "JPN": "japan",
    "LKA": "sri_lanka", "MDG": "madagascar", "NZL": "new_zealand",
    "PHL": "philippines", "IDN": "indonesia", "TWN": "taiwan", "CUB": "cuba",
    "JAM": "jamaica", "TTO": "trinidad", "BHS": "bahamas", "BRB": "barbados",
    "MLT": "malta", "CYP": "cyprus", "SGP": "singapore", "MUS": "mauritius",
    "SYC": "seychelles", "CPV": "cape_verde", "STP": "sao_tome", "COM": "comoros",
    "MDV": "maldives", "FJI": "fiji", "SLB": "solomon", "VUT": "vanuatu",
    "WSM": "samoa", "TON": "tonga", "KIR": "kiribati", "FSM": "micronesia",
    "PLW": "palau", "MHL": "marshall", "NRU": "nauru", "TUV": "tuvalu",
    "GRL": "greenland", "FRO": "faroe", "SPM": "saint_pierre", "BMU": "bermuda",
    "CYM": "cayman", "TCA": "turks_caicos", "VGB": "virgin_british",
    "VIR": "virgin_us", "AIA": "anguilla", "MSR": "montserrat", "SHN": "saint_helena",
    "NCL": "new_caledonia", "PYF": "french_polynesia", "WLF": "wallis_futuna",
    "COK": "cook", "NIU": "niue", "NFK": "norfolk", "FLK": "falkland",
    "GIB": "gibraltar", "IMN": "isle_of_man", "JEY": "jersey", "GGY": "guernsey",
    "ALD": "aland", "HKG": "hong_kong", "MAC": "macau", "PRI": "puerto_rico",
    "GUM": "guam", "ASM": "american_samoa", "MNP": "marianas", "CUW": "curacao",
    "ABW": "aruba", "SXM": "sint_maarten", "MAF": "saint_martin", "BLM": "saint_barthelemy",
}

PALETTE = [
    "#a74444", "#a75d44", "#a77744", "#9a8f44", "#7ba744", "#4aa744", "#44a765",
    "#44a78e", "#4491a7", "#4462a7", "#5d44a7", "#8244a7", "#a7449b", "#a7446f",
    "#8e4444", "#8e6a44", "#7d7a44", "#5f8e44", "#448e5a", "#448e83", "#446f8e",
    "#4a4a8e", "#6b448e", "#8e447e", "#c07840", "#b08a50", "#8f9a55", "#5f9a55",
    "#55a08a", "#5580a0", "#6a6aa0", "#8a55a0", "#a0558a", "#a06050", "#7a6a50",
]


# ---------------------------------------------------------------- utilidades
def norm(text: str) -> str:
    return "".join(c for c in unicodedata.normalize("NFD", text)
                   if unicodedata.category(c) != "Mn").lower().strip()


def key(text: str) -> str:
    return re.sub(r"[^a-z0-9]", "", norm(text))


def clean_name(value: str, fallback: str) -> str:
    if not value:
        return fallback
    if "desambigua" in value.lower():
        return fallback
    value = re.sub(r"\s*\([^)]*\)\s*$", "", value).strip()
    return value or fallback


def km_between(a, b):
    lon1, lat1 = a
    lon2, lat2 = b
    dlon = (lon2 - lon1 + 180.0) % 360.0 - 180.0
    return math.hypot(dlon * 111.32 * math.cos(math.radians((lat1 + lat2) / 2.0)),
                      (lat2 - lat1) * 110.57)


def lonlat_px(lon, lat, w, h):
    return (lon + 180.0) / 360.0 * w, (90.0 - lat) / 180.0 * h


def hex_rgb(value):
    value = value.lstrip("#")
    return tuple(int(value[i:i + 2], 16) for i in (0, 2, 4))


def rgb_hex(rgb):
    return "#%02x%02x%02x" % tuple(int(max(0, min(255, v))) for v in rgb)


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
    la, lb = rgb_lab(a), rgb_lab(b)
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(la, lb)))


def quota(pop, gdp_bn, area_km2):
    extra = 1 + int(pop // 25_000_000) + int(gdp_bn // 1500) + int(area_km2 // 600_000)
    return 1 + max(1, min(24, extra))


def write_csv(directory, name, rows, fields):
    os.makedirs(directory, exist_ok=True)
    with open(os.path.join(directory, name), "w", encoding="utf-8", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow({k: row.get(k, "") for k in fields})


def read_csv(path):
    with open(path, encoding="utf-8-sig", newline="") as fh:
        return list(csv.DictReader(fh))


# ---------------------------------------------------------------- snapshots
def load_worldbank(cache, name):
    path = os.path.join(cache, name)
    if not os.path.exists(path):
        return {}
    raw = json.load(open(path, encoding="utf-8"))
    rows = raw[1] if len(raw) > 1 else []
    best = {}
    for row in rows:
        iso = row.get("countryiso3code") or ""
        value = row.get("value")
        if not iso or value is None:
            continue
        year = int(row["date"])
        if iso not in best or year > best[iso][1]:
            best[iso] = (float(value), year)
    return best


def load_country_info(cache):
    info = {}
    with open(os.path.join(cache, "countryInfo.txt"), encoding="utf-8") as fh:
        for line in fh:
            if line.startswith("#"):
                continue
            f = line.rstrip("\n").split("\t")
            if len(f) < 17:
                continue
            info[f[1]] = {
                "iso2": f[0], "iso3": f[1], "name": f[4], "capital": f[5],
                "area": float(f[6] or 0), "pop": int(f[7] or 0),
                "continent": f[8], "currency_code": f[10], "currency": f[11],
                "neighbours": [x for x in f[16].split(",") if x],
            }
    return info


def load_cities(cache, name, minimum=0):
    out = defaultdict(list)
    with open(os.path.join(cache, name), encoding="utf-8") as fh:
        for line in fh:
            f = line.rstrip("\n").split("\t")
            if len(f) < 19:
                continue
            pop = int(f[14] or 0)
            if pop < minimum:
                continue
            out[f[8]].append({
                "id": int(f[0]), "name": f[1], "ascii": f[2],
                "alternates": f[3], "lat": float(f[4]), "lon": float(f[5]),
                "fcode": f[7], "cc": f[8], "admin1": f[10], "pop": pop,
            })
    return out


# ---------------------------------------------------------------- core
class World:
    def __init__(self, cache):
        self.cache = cache
        self.pop = load_worldbank(cache, "wb_pop.json")
        self.gdp = load_worldbank(cache, "wb_gdp.json")
        self.manuf = load_worldbank(cache, "wb_manuf.json")
        self.rents = load_worldbank(cache, "wb_rents.json")
        self.agri = load_worldbank(cache, "wb_agri.json")
        self.info = load_country_info(cache)
        self.names = json.load(open(COUNTRY_NAMES, encoding="utf-8"))
        self.provinces = json.load(open(os.path.join(ATLAS, "provinces.json"),
                                        encoding="utf-8"))["provinces"]
        self.nation_colors = {k: rgb_hex(v) for k, v in
                              json.load(open(os.path.join(ATLAS, "provinces.json"),
                                             encoding="utf-8"))["nation_colors"].items()}
        self.tag_names = sorted({p["nation"] for p in self.provinces})
        self.tag_countries = defaultdict(set)
        for p in self.provinces:
            self.tag_countries[p["nation"]].add(p["country"])
        self._load_raster()
        self.cities15 = load_cities(cache, "cities15000.txt")
        self.cities5 = load_cities(cache, "cities500.txt", minimum=1000)
        self.iso2tag = {}
        self.entities = self._entities()
        for tag, ent in self.entities.items():
            self.iso2tag[ent["info"]["iso2"]] = tag
        for iso2, tag in CITY_COUNTRY_ALIAS.items():
            self.iso2tag[iso2] = tag
        self.legacy_cities = {r["id"]: r for r in read_csv(os.path.join(SCENARIO, "cities.csv"))}
        self.source_cities = {r["id"]: r for r in
                              read_csv(os.path.join(WORLD, "cities_source.csv"))}
        self.legacy_source = {k: v for k, v in self.source_cities.items()
                              if v.get("legacy") == "yes"}
        self.legacy_ports = read_csv(os.path.join(SCENARIO, "ports.csv"))

    def _load_raster(self):
        im = np.asarray(Image.open(os.path.join(ATLAS, "world_province.png")))
        pid = im[:, :, 0].astype(np.uint16) + im[:, :, 1].astype(np.uint16) * 256
        del im
        name_index = {t: i for i, t in enumerate(self.tag_names)}
        tag_of_id = np.zeros(65536, dtype=np.int16)
        for p in self.provinces:
            tag_of_id[p["id"]] = name_index[p["nation"]]
        self.tag = tag_of_id[pid]
        del pid
        sdf = np.asarray(Image.open(os.path.join(ATLAS, "world_sdf.png")))
        self.land = sdf[:, :, 0] > 128
        del sdf
        self.H, self.W = self.tag.shape
        self.index = {t: i for i, t in enumerate(self.tag_names)}
        self.index["XKX"] = self.index["KOS"]
        border = np.zeros(self.tag.shape, dtype=bool)
        border[:, :-1] |= self.tag[:, :-1] != self.tag[:, 1:]
        border[:-1, :] |= self.tag[:-1, :] != self.tag[1:, :]
        border &= self.land
        self.dist_border = ndimage.distance_transform_edt(~border)
        self.land4 = self.land[::4, ::4]
        self.tag4 = self.tag[::4, ::4]
        self.H4, self.W4 = self.tag4.shape
        self.components, _ = ndimage.label(self.land4)

    # ------------------------------------------------------------ entidades
    def _entities(self):
        out = {}
        for raster_tag in self.tag_names:
            tag = "XKX" if raster_tag == "KOS" else raster_tag
            iso3 = TAG_TO_COUNTRYINFO.get(raster_tag, raster_tag)
            if iso3 not in self.info:
                continue
            info = self.info[iso3]
            if info["iso3"] in UN:
                status = "sovereign"
            elif info["iso3"] in OBSERVERS:
                status = "observer"
            elif tag in SPECIAL:
                status = "special"
            elif tag in TERRITORIES:
                status = "territory"
            else:
                continue
            names = (self.names.get(tag) or self.names.get(raster_tag)
                     or self.names.get(NAME_ALIAS.get(tag, "")))
            if names is None:
                continue
            out[tag] = {"tag": tag, "info": info, "status": status, "names": names}
        if "VAT" not in out and "VAT" in self.info and "VAT" in self.names:
            out["VAT"] = {"tag": "VAT", "info": self.info["VAT"],
                          "status": "observer", "names": self.names["VAT"]}
        return out

    # ------------------------------------------------------------ distâncias
    def border_km(self, lat, lon):
        px, py = lonlat_px(lon, lat, self.W, self.H)
        if not (0 <= px < self.W and 0 <= py < self.H):
            return 0.0
        d = self.dist_border[int(py), int(px)]
        return float(d * min(4.86, 4.89 * math.cos(math.radians(lat))))

    def coast_distance_km(self, lat, lon):
        px, py = lonlat_px(lon, lat, self.W, self.H)
        best = (1e9, None)
        for radius in (16, 32, 64):
            y0, y1 = max(0, int(py) - radius), min(self.H, int(py) + radius + 1)
            x0, x1 = max(0, int(px) - radius), min(self.W, int(px) + radius + 1)
            sub = self.land[y0:y1, x0:x1]
            coast = sub & ~ndimage.binary_erosion(sub, iterations=1)
            ys, xs = np.where(coast)
            for y, x in zip(ys, xs):
                clat = 90.0 - (y0 + y) * 180.0 / self.H
                clon = -180.0 + (x0 + x) * 360.0 / self.W
                d = km_between((lon, lat), (clon, clat))
                if d < best[0]:
                    best = (d, (clat, clon))
            if best[0] < radius * 4.9:
                break
        return best

    def own_land(self, tag, lat, lon, tolerance=3):
        if tag not in self.index:
            return True
        px, py = lonlat_px(lon, lat, self.W, self.H)
        x, y = int(px), int(py)
        for radius in range(0, tolerance + 1):
            y0, y1 = max(0, y - radius), min(self.H, y + radius + 1)
            x0, x1 = max(0, x - radius), min(self.W, x + radius + 1)
            sub = self.tag[y0:y1, x0:x1][self.land[y0:y1, x0:x1]]
            if (sub == self.index[tag]).any():
                return True
        return False

    def component_of(self, lat, lon):
        px, py = lonlat_px(lon, lat, self.W4, self.H4)
        x, y = min(self.W4 - 1, max(0, int(px))), min(self.H4 - 1, max(0, int(py)))
        for radius in range(0, 4):
            y0, y1 = max(0, y - radius), min(self.H4, y + radius + 1)
            x0, x1 = max(0, x - radius), min(self.W4, x + radius + 1)
            sub = self.components[y0:y1, x0:x1]
            values = sub[sub > 0]
            if len(values):
                return int(values.max())
        return 0

    # ------------------------------------------------------------ cidades
    def candidates(self, tag):
        ent = self.entities[tag]
        rows = list(self.cities15.get(ent["info"]["iso2"], []))
        rows += self.cities5.get(ent["info"]["iso2"], [])
        # estados/territórios cujo ISO2 difere do país do mapa
        for iso2, mapped in CITY_COUNTRY_ALIAS.items():
            if mapped == tag:
                rows += self.cities15.get(iso2, [])
                rows += self.cities5.get(iso2, [])
        by_key = {}
        for row in rows:
            if abs(row["lat"]) < 0.01 and abs(row["lon"]) < 0.01:
                continue
            if (self.entities[tag]["info"]["area"] >= 20_000
                    and not self.own_land(tag, row["lat"], row["lon"], tolerance=6)):
                continue
            key = (round(row["lat"], 2), round(row["lon"], 2))
            old = by_key.get(key)
            if old is None:
                by_key[key] = row
            elif row["fcode"] == "PPLC" and old["fcode"] != "PPLC":
                by_key[key] = row
            elif old["fcode"] != "PPLC" and row["pop"] > old["pop"]:
                by_key[key] = row
        return list(by_key.values())

    def find_capital(self, tag, rows):
        ent = self.entities[tag]
        pplc = [c for c in rows if c["fcode"] == "PPLC"]
        if pplc:
            return max(pplc, key=lambda c: c["pop"])
        target = key(ent["info"]["capital"])
        exact = [c for c in rows if key(c["name"]) == target or key(c["ascii"]) == target]
        if exact:
            return max(exact, key=lambda c: c["pop"])
        alt = [c for c in rows if target in {key(x) for x in c["alternates"].split(",")}]
        if alt:
            return max(alt, key=lambda c: c["pop"])
        if tag == "PSX":
            ramallah = [c for c in rows if norm(c["name"]) == "ramallah"]
            if ramallah:
                return max(ramallah, key=lambda c: c["pop"])
        if tag == "SAH":
            tifariti = [c for c in rows if key(c["name"]) == "tifariti"]
            if tifariti:
                return max(tifariti, key=lambda c: c["pop"])
        return None

    def settlement_radius(self, chosen, rows):
        worst, hole = -1.0, None
        for c in rows:
            d = min(km_between((c["lon"], c["lat"]), (s["lon"], s["lat"])) for s in chosen)
            if d > worst:
                worst, hole = d, c
        return worst, hole

    def select(self, tag, rows, cap, target):
        area = self.entities[tag]["info"]["area"]
        threshold = BORDER_SMALL_KM if area < SMALL_AREA_KM2 else BORDER_KM
        top_pop = sorted(rows, key=lambda c: -c["pop"])
        top5 = {c["id"] for c in top_pop[:5]}
        chosen = [cap]
        covered = []

        def essential(c):
            return c["id"] == cap["id"] or c["id"] in top5

        def border_ok(c):
            return self.border_km(c["lat"], c["lon"]) >= threshold or essential(c)

        def spacing_ok(c, reference=None):
            for s in chosen:
                d = km_between((c["lon"], c["lat"]), (s["lon"], s["lat"]))
                if d < MIN_SPACING_KM:
                    return False
                if d < SUBURB_KM and s["pop"] >= 2 * max(1, c["pop"]):
                    return False
            return True

        fill_n = max(0, min(target, int(math.ceil(target * 0.6))) - 1)
        for c in top_pop:
            if len(chosen) >= 1 + fill_n:
                break
            if c in chosen or not border_ok(c) or not spacing_ok(c):
                continue
            chosen.append(c)
        for _ in range(target + 12):
            radius, hole = self.settlement_radius(chosen, rows)
            if radius <= COVER_KM:
                break
            near = [c for c in rows if c not in chosen and border_ok(c)
                    and km_between((hole["lon"], hole["lat"]), (c["lon"], c["lat"])) <= 500]
            if near:
                pick = max(near, key=lambda c: (c["pop"], -km_between(
                    (hole["lon"], hole["lat"]), (c["lon"], c["lat"]))))
            else:
                pool = [c for c in rows if c not in chosen and border_ok(c)]
                if not pool:
                    break
                pick = min(pool, key=lambda c: km_between(
                    (hole["lon"], hole["lat"]), (c["lon"], c["lat"])))
            if km_between((hole["lon"], hole["lat"]), (pick["lon"], pick["lat"])) >= radius * 0.98:
                break
            if pick in chosen:
                break
            pick["coverage"] = True
            chosen.append(pick)
            covered.append(pick)
        for c in top_pop:
            if len(chosen) >= target:
                break
            if c in chosen or not border_ok(c) or not spacing_ok(c):
                continue
            chosen.append(c)
        return chosen, covered

    # ------------------------------------------------------------ auxiliares
    def derive_profile(self, iso3):
        if iso3 in self.rents and self.rents[iso3][0] >= 10:
            return "resource"
        if iso3 in self.manuf and self.manuf[iso3][0] >= 15:
            return "industrial"
        if iso3 in self.agri and self.agri[iso3][0] >= 20:
            return "agrarian"
        return "balanced"

    def government(self, tag):
        if tag in CITY_STATE:
            return "city_state"
        if self.entities[tag]["status"] == "territory":
            return "territory"
        if tag in MONARCHY:
            return "monarchy"
        if tag in FEDERATION:
            return "federation"
        return "republic"

    def region(self, tag):
        if tag in MIDDLE_EAST:
            return "middle_east"
        return CONTINENT.get(self.entities[tag]["info"]["continent"], "asia")

    # ------------------------------------------------------------ cobertura
    def land_radius(self, tag, cities):
        """Raio máximo (km) de um ponto de terra da nação até uma cidade."""
        if tag not in self.index:
            return 0.0
        ti = self.index[tag]
        mask = (self.tag4 == ti) & self.land4
        ys, xs = np.where(mask)
        if len(ys) == 0:
            return 0.0
        y0, y1 = max(0, ys.min() - 1), min(self.H4, ys.max() + 2)
        x0, x1 = max(0, xs.min() - 1), min(self.W4, xs.max() + 2)
        wrap = x0 == 0 and x1 == self.W4
        sub = mask[y0:y1, x0:x1]
        seed = np.ones(sub.shape, dtype=bool)
        lat_mid = 90.0 - (y0 + y1) / 2.0 * (180.0 / self.H4)
        kmy = 180.0 / self.H4 * 110.57
        kmx = 360.0 / self.W4 * 111.32 * math.cos(math.radians(lat_mid))
        for lat, lon in cities:
            px = (lon + 180.0) / 360.0 * self.W4 - x0
            py = (90.0 - lat) / 180.0 * self.H4 - y0
            if 0 <= py < sub.shape[0] and 0 <= px < sub.shape[1]:
                seed[int(py), int(px)] = False
        if seed.all():
            return 0.0
        if wrap:
            seed = np.concatenate([seed, seed], axis=1)
        dist = ndimage.distance_transform_edt(seed, sampling=(kmy, kmx))
        if wrap:
            dist = dist[:, :sub.shape[1]]
        dist[~sub] = -1
        return float(dist.max())

    # ------------------------------------------------------------ massas
    def landmass(self, tag, city):
        ent = self.entities[tag]
        if ent["status"] == "territory" and tag in ISLAND_TOKEN:
            return ISLAND_TOKEN[tag]
        name = norm(city["name"])
        if tag == "FRA" and name in {"cayenne"}:
            return "guiana"
        if tag == "USA" and name in {"honolulu"}:
            return "hawaii"
        if tag == "PRT" and name in {"funchal", "ponta delgada"}:
            return "madeira" if name == "funchal" else "azores"
        if tag == "ESP" and name in {"las palmas de gran canaria", "santa cruz de tenerife"}:
            return "canarias"
        if tag == "AUS" and name in {"hobart"}:
            return "tasmania"
        if tag in ISLAND_TOKEN:
            return ISLAND_TOKEN[tag]
        return CONTINENT.get(ent["info"]["continent"], "asia")

    # ------------------------------------------------------------ nomes
    def _get_json(self, url, label):
        req = urllib.request.Request(
            url, headers={"User-Agent": "OpenGS-data/1.0 (world data import)"})
        for attempt in range(4):
            try:
                with urllib.request.urlopen(req, timeout=60) as fh:
                    return json.load(fh)
            except Exception as exc:  # 429/rede: espera e tenta de novo
                if attempt == 3:
                    print(f"AVISO: {label}: {exc}", file=sys.stderr)
                    return None
                time.sleep(5 * (attempt + 1))
        return None

    def _wikidata_items(self, titles):
        """title da enwiki -> (wikibase_item, é desambiguação)."""
        params = {"action": "query", "format": "json", "prop": "pageprops",
                  "ppprop": "wikibase_item|disambiguation", "redirects": 1,
                  "titles": "|".join(titles)}
        url = "https://en.wikipedia.org/w/api.php?" + urllib.parse.urlencode(params)
        data = self._get_json(url, "Wikipedia enwiki")
        if not data:
            return {}
        query = data.get("query", {})
        alias = {}
        for item in query.get("normalized", []) + query.get("redirects", []):
            alias[item["from"]] = item["to"]
        by_title = {}
        for page in query.get("pages", {}).values():
            if "missing" in page or "invalid" in page:
                continue
            props = page.get("pageprops") or {}
            by_title[page["title"]] = (props.get("wikibase_item", ""),
                                       "disambiguation" in props)
        out = {}
        for title in titles:
            final = alias.get(title, title)
            out[title] = by_title.get(final, ("", False))
        return out

    def _wikidata_labels(self, qids):
        """QID -> {labels: pt_BR, pt, es}."""
        out = {}
        qids = [q for q in sorted(set(qids)) if q]
        for i in range(0, len(qids), 40):
            batch = qids[i:i + 40]
            params = {"action": "wbgetentities", "format": "json",
                      "ids": "|".join(batch), "props": "labels",
                      "languages": "pt-br|pt|es"}
            url = "https://www.wikidata.org/w/api.php?" + urllib.parse.urlencode(params)
            data = self._get_json(url, "Wikidata")
            if not data:
                continue
            for qid, ent in data.get("entities", {}).items():
                labels = ent.get("labels") or {}
                out[qid] = {
                    "pt_BR": (labels.get("pt-br") or {}).get("value", ""),
                    "pt": (labels.get("pt") or {}).get("value", ""),
                    "es": (labels.get("es") or {}).get("value", ""),
                }
            time.sleep(1.0)
        return out

    def fetch_names(self, titles, fetch=True):
        """Nomes pt-BR/es dos rótulos do Wikidata (pt-br -> pt -> inglês, es -> inglês).

        Cache em disco: {nome GeoNames: rótulo}; string vazia = sem tradução
        (a cidade fica com o nome inglês, declarado no relatório).
        """
        caches = {}
        for lang in ("pt", "es"):
            path = os.path.join(self.cache, f"city_names_{lang}.json")
            caches[lang] = (json.load(open(path, encoding="utf-8"))
                            if os.path.exists(path) else {})
        if not fetch:
            return caches["pt"], caches["es"]
        wanted = sorted({t for t in titles if t})
        pending = [t for t in wanted
                   if not caches["pt"].get(t) or not caches["es"].get(t)]
        for i in range(0, len(pending), 40):
            batch = pending[i:i + 40]
            items = self._wikidata_items(batch)
            qids = {t: item[0] for t, item in items.items() if item[0] and not item[1]}
            labels = self._wikidata_labels(list(qids.values()))
            for title in batch:
                qid = qids.get(title)
                lab = labels.get(qid, {}) if qid else {}
                caches["pt"][title] = clean_name(
                    lab.get("pt_BR") or lab.get("pt") or "", "")
                caches["es"][title] = clean_name(lab.get("es") or "", "")
            time.sleep(1.0)
        for lang in ("pt", "es"):
            json.dump(caches[lang], open(os.path.join(self.cache, f"city_names_{lang}.json"),
                                         "w", encoding="utf-8"), ensure_ascii=False,
                      indent=0, sort_keys=True)
        return caches["pt"], caches["es"]

    # ------------------------------------------------------------ cores
    def adjacency(self):
        a = self.tag.astype(np.int32)
        counts = defaultdict(int)

        def add(left, right, mask):
            key = left[mask].astype(np.int32) * 256 + right[mask].astype(np.int32)
            values, freqs = np.unique(key, return_counts=True)
            for value, n in zip(values.tolist(), freqs.tolist()):
                x, y = divmod(value, 256)
                if x != y:
                    counts[(min(x, y), max(x, y))] += n

        add(a[:, :-1], a[:, 1:], self.land[:, :-1])
        add(a[:-1, :], a[1:, :], self.land[:-1, :])
        graph = defaultdict(set)
        def name(idx):
            t = self.tag_names[idx]
            return "XKX" if t == "KOS" else t
        for (x, y), n in counts.items():
            if n < 10:
                continue
            graph[name(x)].add(name(y))
            graph[name(y)].add(name(x))
        return graph

    def choose_colors(self, tags, phase1_colors):
        graph = self.adjacency()
        fixed = {t: c for t, c in self.nation_colors.items() if t in tags}
        order = sorted(tags, key=lambda t: -self.entities[t]["info"]["area"])
        colors = {}
        usage = defaultdict(int)
        for tag in order:
            if tag in fixed:
                colors[tag] = fixed[tag]
                usage[fixed[tag]] += 1
                continue
            neigh = [colors[n] for n in graph.get(tag, ()) if n in colors]
            def score(hexcolor):
                if not neigh:
                    return 100.0
                return min(delta_e(hex_rgb(hexcolor), hex_rgb(c)) for c in neigh)
            valid = [c for c in PALETTE if score(c) >= 35] or PALETTE
            best = min(valid, key=lambda c: (usage[c], -score(c), PALETTE.index(c)))
            colors[tag] = best
            usage[best] += 1
        changed = []
        for tag in order:
            if tag in fixed:
                continue
            neigh = [colors[n] for n in graph.get(tag, ()) if n in colors and n != tag]
            if not neigh:
                continue
            if min(delta_e(hex_rgb(colors[tag]), hex_rgb(c)) for c in neigh) < 30:
                best = max(PALETTE, key=lambda c: (min(
                    delta_e(hex_rgb(c), hex_rgb(n)) for n in neigh), -usage[c]))
                if best != colors[tag]:
                    changed.append((tag, colors[tag], best))
                    usage[colors[tag]] -= 1
                    usage[best] += 1
                    colors[tag] = best
        for tag, hexcolor in COLOR_OVERRIDE.items():
            if tag in colors:
                colors[tag] = hexcolor
        return colors, graph, changed

    # ------------------------------------------------------------ portos
    def port_for(self, tag, city):
        if tag not in self.index:
            return "no", None, 0.0
        dist, point = self.coast_distance_km(city["lat"], city["lon"])
        if point is None or dist > PORT_MAX_CITY_COAST_KM:
            return "no", None, dist
        if dist < PORT_MIN_POINT_KM:
            return "city", None, dist
        # o ponto de costa precisa pertencer à mesma nação
        px, py = lonlat_px(point[1], point[0], self.W, self.H)
        if self.tag[int(py), int(px)] != self.index[tag]:
            return "no", None, dist
        return "yes", point, dist

    # ------------------------------------------------------------ execução
    def run(self, write=False, fetch_names=False):
        problems = []
        phase1 = {r["iso3"]: r for r in read_csv(os.path.join(WORLD, "nations_source.csv"))}
        phase1_colors = {k: v["color"] for k, v in phase1.items() if v.get("color")}
        before = {}
        before_path = os.path.join(WORLD, "out", "cities.csv")
        if os.path.exists(before_path):
            for row in read_csv(before_path):
                lat, lon = self.inverse_project(float(row["x"]), float(row["y"]))
                before.setdefault(row["nation"], []).append({"lat": lat, "lon": lon})
        legacy_cities = self.legacy_cities
        legacy_ids = set(legacy_cities)

        # 1. cidades por nação
        selected = {}
        for tag in sorted(self.entities):
            rows = self.candidates(tag)
            if tag == "VAT":
                legacy_row = self.legacy_cities.get("VAT-VATI")
                if not legacy_row:
                    problems.append("VAT: cidade do cenário ausente")
                    continue
                lat, lon = self.inverse_project(float(legacy_row["x"]), float(legacy_row["y"]))
                name_en = legacy_row["name"]
                cap = {"id": -1, "name": name_en, "ascii": name_en,
                       "alternates": "", "lat": lat, "lon": lon, "fcode": "PPLC",
                       "cc": "VA", "admin1": "", "pop": int(legacy_row["population"]),
                       "legacy": legacy_row, "legacy_row": "VAT-VATI"}
                selected[tag] = ([cap], [], 1, cap)
                continue
            if not rows:
                problems.append(f"{tag}: sem cidade no GeoNames")
                continue
            cap = self.find_capital(tag, rows)
            if cap is None:
                problems.append(f"{tag}: capital não encontrada no GeoNames")
                continue
            ent = self.entities[tag]
            pop = self.pop.get(ent["info"]["iso3"], (ent["info"]["pop"], None))[0]
            gdp = self.gdp.get(ent["info"]["iso3"], (0.0, None))[0] / 1e9
            target = quota(pop, gdp, ent["info"]["area"])
            chosen, covered = self.select(tag, rows, cap, target)
            selected[tag] = (chosen, covered, target, cap)

        # 2. legado (ids/coordenadas/população exatos; só as 40 do cenário)
        legacy_by_nation = defaultdict(list)
        for cid in self.legacy_source:
            row = legacy_cities.get(cid)
            if row:
                legacy_by_nation[row["nation"]].append(row)
        for tag, rows in legacy_by_nation.items():
            if tag not in selected:
                problems.append(f"{tag}: nação legada não está na lista nova")
                continue
            chosen, covered, target, cap = selected[tag]
            ids = {c["id"] for c in chosen}
            legacy_done = {c.get("legacy_row") for c in chosen}
            for row in rows:
                if row["id"] == "VEN-PAY":
                    continue  # substituída por Ciudad Bolívar (regra C)
                if row["id"] in ids or row["id"] in legacy_done:
                    continue
                lat, lon = self.inverse_project(float(row["x"]), float(row["y"]))
                # a seleção nova pode ter escolhido a mesma cidade com outro id
                chosen = [c for c in chosen if "legacy" in c or km_between(
                    (c["lon"], c["lat"]), (lon, lat)) >= 20]
                chosen.append({
                    "id": -1, "name": row["name"], "ascii": row["name"],
                    "alternates": "", "lat": lat, "lon": lon, "fcode": "PPL",
                    "cc": "", "admin1": "", "pop": int(row["population"]),
                    "legacy": row,
                })
            selected[tag] = (chosen, covered, target, cap)

        # 2a. VEN: substitui Puerto Ayacucho por Ciudad Bolívar (interior, regra C)
        if "VEN" in selected:
            chosen, covered, target, cap = selected["VEN"]
            if not any(key(c["name"]) == "ciudadbolivar" for c in chosen):
                chosen.append({
                    "id": 3645532, "name": "Ciudad Bolívar", "ascii": "Ciudad Bolivar",
                    "alternates": "", "lat": 8.12366, "lon": -63.54694,
                    "fcode": "PPLA", "cc": "VE", "admin1": "", "pop": 412619,
                    "forced_id": "VEN-CBL",
                })
            selected["VEN"] = (chosen, covered, target, cap)

        # 2b. componentes habitados distantes (> COVER_KM) sem cidade escolhida
        extras = 0
        extra_by_tag = defaultdict(int)
        for tag, (chosen, covered, target, cap) in list(selected.items()):
            if tag not in self.index:
                continue
            rows = self.candidates(tag)
            reps = {self.component_of(c["lat"], c["lon"]) for c in chosen}
            best = {}
            for c in rows:
                comp = self.component_of(c["lat"], c["lon"])
                if comp in reps:
                    continue
                if comp not in best or c["pop"] > best[comp]["pop"]:
                    best[comp] = c
            for comp, c in best.items():
                if min(km_between((c["lon"], c["lat"]), (s["lon"], s["lat"]))
                       for s in chosen) <= COVER_KM:
                    continue
                c["coverage"] = True
                chosen.append(c)
                extras += 1
                extra_by_tag[tag] += 1
            selected[tag] = (chosen, covered, target, cap)
        if extras:
            print(f"componentes habitados adicionados fora da cota: {extras}")

        # 3. cores
        tags = list(selected)
        colors, graph, changed = self.choose_colors(tags, phase1_colors)

        # 4. nomes traduzidos
        titles = []
        for tag, (chosen, _, _, _) in selected.items():
            for c in chosen:
                if "legacy" not in c:
                    titles.append(c["name"])
        tr_pt, tr_es = self.fetch_names(titles, fetch=fetch_names)

        # 5. saída
        nations_out, cities_out, ports_out = [], [], []
        buildings_out, i18n_out = [], []
        report = []
        used_ids = set(self.legacy_cities)
        legacy_port_ids = {p["id"] for p in self.legacy_ports}
        for tag in sorted(selected):
            ent = self.entities[tag]
            info = ent["info"]
            names = ent["names"]
            chosen, covered, target, cap = selected[tag]
            pop = self.pop.get(info["iso3"])
            gdp = self.gdp.get(info["iso3"])
            population = int(pop[0]) if pop else int(info["pop"])
            gdp_bn = round(gdp[0] / 1e9, 1) if gdp else ""
            profile = phase1.get(tag, {}).get("ai_profile") or self.derive_profile(info["iso3"])
            cap_en = cap["name"] if cap else info["capital"]
            q = target
            pop_src = ("World Bank WDI SP.POP.TOTL (último ano)"
                       if pop else "população: GeoNames countryInfo 2026 (sem série WDI)")
            gdp_src = ("World Bank WDI NY.GDP.MKTP.PP.CD (último ano)"
                       if gdp else "PIB PPP: ausente nas fontes (não estimado)")
            source_txt = f"{pop_src}; {gdp_src}; área/moeda: GeoNames countryInfo 2026"
            nations_out.append({
                "iso3": tag, "iso2": info["iso2"], "name_en": names["en"],
                "name_pt": names["pt_BR"], "name_es": names["es"],
                "capital_en": cap_en, "capital_pt": "", "capital_es": "",
                "currency": info["currency_code"], "government": self.government(tag),
                "region": self.region(tag), "ai_profile": profile,
                "population": population,
                "population_year": pop[1] if pop else "",
                "gdp_ppp_bn": gdp_bn, "gdp_year": gdp[1] if gdp else "",
                "data_quality": "ok" if (pop and gdp) else (
                    "missing_gdp" if pop else "missing"),
                "quota": q, "area_km2": round(info["area"], 1),
                "status": ent["status"], "color": colors[tag],
                "source": source_txt,
            })
            i18n_out.append({"key": f"nation.name.{tag}", "pt_BR": names["pt_BR"],
                             "en": names["en"], "es": names["es"]})
            i18n_out.append({"key": f"nation.{tag}", "pt_BR": names["pt_BR"],
                             "en": names["en"], "es": names["es"]})

            # capital primeiro
            cap_id = cap["id"] if cap else None
            chosen.sort(key=lambda c: (c["id"] != cap_id, -c["pop"]))
            legacy_n = 0
            for c in chosen:
                legacy = c.get("legacy")
                if legacy:
                    legacy_n += 1
                    cid = legacy["id"]
                    src = self.legacy_source.get(cid) or self.source_cities.get(cid, {})
                    name_en = src.get("name_en") or legacy["name"]
                    name_pt = src.get("name_pt") or name_en
                    name_es = src.get("name_es") or name_en
                    lat, lon = self.inverse_project(float(legacy["x"]), float(legacy["y"]))
                    population_c = int(legacy["population"])
                    capital = legacy["capital"]
                    port = src.get("port", "no")
                    role = src.get("role", "")
                    farm = src.get("initial_farm_tons") or f"{population_c * POP_PER_TON:.1f}"
                    note = "legado preservado (id, coordenadas e população)"
                    source = "scenario/cities.csv (legado, base 2026)"
                    landmass = legacy["landmass"]
                else:
                    cid = c.get("forced_id") or self.make_city_id(tag, c["name"], used_ids)
                    name_en = c["name"]
                    lat, lon = c["lat"], c["lon"]
                    population_c = int(c["pop"])
                    capital = "1" if cap and c["id"] == cap["id"] else "0"
                    kind, point, coast_km = c.get("_port") or self.port_for(tag, c)
                    c["_port"] = (kind, point, coast_km)
                    if kind == "yes" and point:
                        own_d = km_between((c["lon"], c["lat"]), (point[1], point[0]))
                        closer = any(other is not c and km_between(
                            (other["lon"], other["lat"]), (point[1], point[0])) < own_d * 0.8
                            for other in chosen)
                        if closer:
                            kind, point, coast_km = "no", None, coast_km
                    port = "yes" if kind == "yes" else ("city" if kind == "city" else "no")
                    role = ("capital" if capital == "1" else
                            "port" if port in ("yes", "city") else
                            "industrial" if population_c >= 500_000 or c.get("forced_id")
                            else "regional")
                    farm = f"{population_c * POP_PER_TON:.1f}"
                    note = ("centro regional (cobertura)" if c.get("coverage")
                            else "maior cidade disponível")
                    if c.get("forced_id") == "VEN-CBL":
                        note = "substitui VEN-PAY (Puerto Ayacucho), a < 5 km da fronteira"
                    source = "GeoNames cities15000/cities500 2026 (city proper)"
                    landmass = self.landmass(tag, c)
                    name_pt = clean_name(tr_pt.get(name_en, ""), name_en)
                    name_pt = PT_BR_OVERRIDE.get(name_pt, name_pt)
                    name_es = clean_name(tr_es.get(name_en, ""), name_en)
                    if name_en in NAME_OVERRIDE:
                        name_en, name_pt, name_es = NAME_OVERRIDE[name_en]
                used_ids.add(cid)
                bkm = round(self.border_km(lat, lon), 1)
                if bkm < (BORDER_SMALL_KM if info["area"] < SMALL_AREA_KM2 else BORDER_KM):
                    note = f"{note}; fronteira a {bkm} km" + (
                        " (legado)" if legacy else " (capital/maior cidade)")
                cities_out.append({
                    "nation": tag, "id": cid, "name_en": name_en, "name_pt": name_pt,
                    "name_es": name_es, "lat": lat, "lon": lon,
                    "population": population_c, "population_year": 2026,
                    "capital": capital, "port": port, "role": role,
                    "landmass": landmass,
                    "pop_src": "agglomeration" if legacy else "city_proper",
                    "note": note, "source": source,
                    "legacy": "yes" if legacy else "no",
                    "initial_farm_tons": farm,
                    "border_km": bkm,
                })
                buildings_out.append({
                    "city_id": cid,
                    "building_id": (cid.split("-", 1)[1].lower() if legacy
                                    else cid.lower().replace("-", "_")) + "_wheat",
                    "name": f"{name_pt} — trigo",
                    "output_tons_per_day": farm,
                })
                i18n_out.append({"key": f"city.name.{cid}", "pt_BR": name_pt,
                                 "en": name_en, "es": name_es})
                if port == "yes" and not legacy and cid not in legacy_port_ids:
                    point = self.port_for(tag, c)[1]
                    if point:
                        ports_out.append({
                            "nation": tag, "id": cid, "name": name_en,
                            "lat": point[0], "lon": point[1],
                            "landmass": landmass,
                            "source": "snap na costa de world_sdf.png (2026)",
                            "port_of": cid, "coast_km": round(
                                self.port_for(tag, c)[2], 1),
                        })
                        i18n_out.append({"key": f"port.name.{cid}", "pt_BR": name_pt,
                                         "en": name_en, "es": name_es})
            over = max(0, len(chosen) - legacy_n - extra_by_tag.get(tag, 0) - q)
            report.append({
                "tag": tag, "target": q, "cities": len(chosen), "legacy": legacy_n,
                "extra": extra_by_tag.get(tag, 0), "over_quota": over,
                "settle_km": round(self.settlement_radius(chosen, self.candidates(tag))[0], 1),
                "land_km": round(self.land_radius(tag, [
                    (c["lat"], c["lon"]) for c in chosen]), 1),
                "status": ent["status"], "area_km2": ent["info"]["area"],
            })

        # 6. portos legados intactos
        port_ids = {p["id"] for p in ports_out}
        for row in self.legacy_ports:
            if row["id"] in port_ids:
                problems.append(f"porto legado {row['id']} colidiu com porto novo")
                continue
            ports_out.append({
                "nation": row["nation"], "id": row["id"], "name": row["name"],
                "lat": self.inverse_project(float(row["x"]), float(row["y"]))[0],
                "lon": self.inverse_project(float(row["x"]), float(row["y"]))[1],
                "landmass": row["landmass"], "source": "scenario/ports.csv (legado)",
                "port_of": "", "coast_km": "",
            })
            i18n_out.append({"key": f"port.name.{row['id']}", "pt_BR": row["name"],
                             "en": row["name"], "es": row["name"]})

        # 7. capital pt/es a partir da cidade capital
        city_names = {c["id"]: c for c in cities_out}
        for n in nations_out:
            cap_city = next((c for c in cities_out
                             if c["nation"] == n["iso3"] and c["capital"] == "1"), None)
            if cap_city:
                n["capital_en"] = cap_city["name_en"]
                n["capital_pt"] = cap_city["name_pt"]
                n["capital_es"] = cap_city["name_es"]

        # 8. checagens
        for row in report:
            if row["cities"] > 40:
                problems.append(f"{row['tag']}: {row['cities']} cidades (acima de 40)")
            if row["settle_km"] > COVER_KM + 1:
                problems.append(f"{row['tag']}: cobertura {row['settle_km']:.0f} km > {COVER_KM:.0f}")
        for c in cities_out:
            if c["id"] == "VEN-PAY":
                problems.append("VEN-PAY ainda presente")
            if c["legacy"] == "yes":
                src = self.legacy_cities.get(c["id"])
                if src and (abs(float(src["x"]) - self.project(c["lat"], c["lon"])[0]) > 0.2
                            or abs(float(src["y"]) - self.project(c["lat"], c["lon"])[1]) > 0.2):
                    problems.append(f"{c['id']}: coordenada legada divergente")
        if not any(c["id"] == "VEN-CBL" for c in cities_out):
            problems.append("VEN-CBL (Ciudad Bolívar) ausente: substitui VEN-PAY")
        ids = [c["id"] for c in cities_out]
        if len(ids) != len(set(ids)):
            problems.append("cidades: ids duplicados")
        pids = [p["id"] for p in ports_out]
        if len(pids) != len(set(pids)):
            problems.append("portos: ids duplicados")
        below = []
        for tag, neighbours in graph.items():
            if tag not in colors:
                continue
            for other in neighbours:
                if other not in colors or other <= tag:
                    continue
                d = delta_e(hex_rgb(colors[tag]), hex_rgb(colors[other]))
                if d < LIMIAR_COR:
                    below.append((round(d, 1), tag, other))
        below.sort()
        for d, a, b in below:
            problems.append(f"cores: ΔE {d} entre vizinhos {a}-{b} (< {LIMIAR_COR})")

        print(f"nações: {len(nations_out)}")
        print(f"cidades: {len(cities_out)} (legado: {sum(1 for c in cities_out if c['legacy'] == 'yes')})")
        print(f"portos: {len(ports_out)} (legado: {len(self.legacy_ports)})")
        print(f"construções: {len(buildings_out)}")
        print(f"chaves i18n: {len(i18n_out)}")
        print(f"cobertura > 700 km: {sum(1 for r in report if r['settle_km'] > 701)} nações")
        print(f"cores trocadas por conflito: {len(changed)}")
        names_new = [c for c in cities_out if c["legacy"] == "no"]
        print(f"nomes sem pt-BR: {sum(1 for c in names_new if c['name_pt'] == c['name_en'])}"
              f" | sem es: {sum(1 for c in names_new if c['name_es'] == c['name_en'])}"
              f" de {len(names_new)} novas")
        print(f"vizinhos com ΔE < {LIMIAR_COR}: {len(below)}")
        for p in problems:
            print(f"AVISO: {p}")

        if not write:
            return problems, report

        nation_fields = ["iso3", "iso2", "name_en", "name_pt", "name_es", "capital_en",
                         "capital_pt", "capital_es", "currency", "government", "region",
                         "ai_profile", "population", "population_year", "gdp_ppp_bn",
                         "gdp_year", "data_quality", "quota", "area_km2", "status",
                         "color", "source"]
        city_fields = ["nation", "id", "name_en", "name_pt", "name_es", "lat", "lon",
                       "population", "population_year", "capital", "port", "role",
                       "landmass", "pop_src", "note", "source", "legacy",
                       "initial_farm_tons", "border_km"]
        port_fields = ["nation", "id", "name", "lat", "lon", "landmass", "source",
                       "port_of", "coast_km"]
        write_csv(WORLD, "nations_source.csv", nations_out, nation_fields)
        write_csv(WORLD, "cities_source.csv", cities_out, city_fields)
        write_csv(WORLD, "ports_source.csv", ports_out, port_fields)
        write_csv(WORLD, "_import_report.csv", report,
                  ["tag", "target", "cities", "legacy", "extra", "over_quota",
                   "settle_km", "land_km", "status", "area_km2"])
        by_tag = {r["tag"]: r for r in report}
        coverage_rows = []
        for tag, (chosen, _, _, _) in sorted(selected.items()):
            old = before.get(tag)
            before_km = ""
            if old:
                before_km = round(self.settlement_radius(old, self.candidates(tag))[0], 1)
            coverage_rows.append({
                "tag": tag, "before_cities": len(old) if old else 0,
                "before_km": before_km, "after_cities": by_tag[tag]["cities"],
                "after_km": by_tag[tag]["settle_km"],
            })
        write_csv(os.path.join(WORLD), "_coverage_before_after.csv", coverage_rows,
                  ["tag", "before_cities", "before_km", "after_cities", "after_km"])
        meta = {
            "phase": "F2",
            "generated_by": "tools/import_world_sources.py",
            "sources": {
                "population": "World Bank WDI SP.POP.TOTL (último ano disponível)",
                "gdp_ppp": "World Bank WDI NY.GDP.MKTP.PP.CD (último ano disponível)",
                "cities": "GeoNames cities15000 + cities500 (2026, city proper)",
                "city_names": "Wikidata: rótulo pt-br (fallback pt, inglês); es (fallback inglês)",
                "area": "GeoNames countryInfo (2026)",
                "borders": "map/map_data/atlas/world_province.png",
                "coast": "map/map_data/atlas/world_sdf.png",
            },
            "rules": {
                "quota": "1 + clamp(1 + floor(pop/25M) + floor(gdp_bn/1500) + floor(area_km2/600k), 1, 24)",
                "coverage_km": COVER_KM,
                "border_km": BORDER_KM,
                "border_small_km": BORDER_SMALL_KM,
                "small_area_km2": SMALL_AREA_KM2,
                "min_spacing_km": MIN_SPACING_KM,
                "port_city_coast_km": PORT_MAX_CITY_COAST_KM,
                "port_point_km": PORT_MIN_POINT_KM,
                "color_min_delta_e": LIMIAR_COR,
                "color_overrides": COLOR_OVERRIDE,
            },
            "counts": {
                "nations": len(nations_out),
                "cities": len(cities_out),
                "ports": len(ports_out),
                "legacy_cities": sum(1 for c in cities_out if c["legacy"] == "yes"),
                "legacy_ports": len(self.legacy_ports),
            },
        }
        json.dump(meta, open(os.path.join(WORLD, "_import_meta.json"), "w",
                             encoding="utf-8"), ensure_ascii=False, indent=2)
        return problems, report

    @staticmethod
    def project(lat, lon):
        return (lon + 180.0) / 360.0 * 5000.0, (90.0 - lat) / 180.0 * 2500.0

    @staticmethod
    def inverse_project(x, y):
        return 90.0 - y / 2500.0 * 180.0, x / 5000.0 * 360.0 - 180.0

    @staticmethod
    def make_city_id(tag, name, used):
        ascii_name = norm(name)
        letters = re.sub(r"[^a-z]", "", ascii_name).upper()
        for n in (3, 4, 5):
            cid = f"{tag}-{letters[:n]}"
            if cid not in used:
                return cid
        i = 2
        while f"{tag}-{letters[:3]}{i}" in used:
            i += 1
        return f"{tag}-{letters[:3]}{i}"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", required=True, help="diretório dos snapshots")
    parser.add_argument("--write", action="store_true")
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--fetch-names", action="store_true")
    args = parser.parse_args()
    world = World(args.cache)
    problems, report = world.run(write=args.write, fetch_names=args.fetch_names)
    if args.check:
        return 1 if problems else 0
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

