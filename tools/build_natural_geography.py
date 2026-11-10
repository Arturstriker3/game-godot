"""Gera a geografia natural por território e os depósitos minerais do jogo.

Entradas:
  data/definitions/provinces.txt              províncias (id;tipo;rgb;px;py;terreno)
  data/history/provinces/provinces.json       dono de cada província
  data/history/territories/territories.json   território -> províncias
  map/map_data/nav_mask.png                   máscara de água/terra (costa)
  tools/data/mrds_deposits.jsonl              USGS MRDS (tools/fetch_usgs_deposits.py)

Saídas (versionadas):
  data/definitions/natural/nature.json        clima, solo, relevo, água, floresta, geologia, pesca
  data/definitions/natural/agriculture.json   aptidão por cultura (0..1)
  data/definitions/natural/deposits.json      depósitos minerais e energéticos
  data/definitions/natural/_sources.json      metadados e nível de confiança dos dados

Método:
  - A projeção é a mesma do mapa: equirretangular global 5000x2500.
  - Clima/solo/relevo vêm de faixas de latitude + tabelas curadas de regiões
    (desertos, montanhas, escudos). São parâmetros de teste identificados como
    `data_quality: "test"` — não são dados climáticos medidos.
  - Depósitos usam localização real do USGS MRDS; quantidade/qualidade/
    profundidade/custo são estimativas determinísticas a partir do tipo de
    recurso, status e número de registros (`data_quality:
    "real_location_estimated_attributes"`).
  - Nada usa aleatoriedade global: as estimativas derivam de hash estável do id.
"""

import hashlib
import json
import math
import os
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFS = os.path.join(ROOT, "data", "definitions")
HIST = os.path.join(ROOT, "data", "history")
MAP_DATA = os.path.join(ROOT, "map", "map_data")
NATURAL_DIR = os.path.join(DEFS, "natural")
## O extrato MRDS não traz vários campos reais (pré-sal, Mar do Norte, gás).
## Depois de regenerar deposits.json, rode tools/apply_deposit_supplement.py.
MRDS_PATH = os.path.join(ROOT, "tools", "data", "mrds_deposits.jsonl")

MAP_W, MAP_H = 5000.0, 2500.0

# --------------------------------------------------------------------------
# Projeção
# --------------------------------------------------------------------------

def pixel_to_lonlat(px, py):
    return px / MAP_W * 360.0 - 180.0, 90.0 - py / MAP_H * 180.0


def lonlat_to_pixel(lon, lat):
    return (lon + 180.0) / 360.0 * MAP_W, (90.0 - lat) / 180.0 * MAP_H


# --------------------------------------------------------------------------
# Tabelas curadas (dados de teste identificados)
# --------------------------------------------------------------------------

# (lon_min, lon_max, lat_min, lat_max, altitude_m, geologia)
MOUNTAINS = [
    ("Andes", -82.0, -63.0, -56.0, 12.0, 2800, "volcanic_arc"),
    ("Alpes", 5.0, 17.0, 43.5, 48.5, 1800, "fold_belt"),
    ("Pireneus", -2.5, 3.5, 41.5, 44.0, 1500, "fold_belt"),
    ("Carpates", 17.0, 27.0, 43.5, 50.0, 1200, "fold_belt"),
    ("Cadeia Escandinava", 4.0, 19.0, 57.0, 71.0, 1000, "fold_belt"),
    ("Urais", 55.0, 66.0, 48.0, 69.0, 900, "fold_belt"),
    ("Caucaso", 37.0, 51.0, 39.0, 44.5, 2200, "fold_belt"),
    ("Zagros", 43.0, 59.0, 26.0, 38.0, 1600, "fold_belt"),
    ("Himalaia", 68.0, 100.0, 26.0, 37.0, 3800, "fold_belt"),
    ("Planalto do Tibete", 74.0, 106.0, 27.0, 39.0, 4200, "fold_belt"),
    ("Montanhas Rochosas", -127.0, -103.0, 29.0, 56.0, 2200, "fold_belt"),
    ("Apalaches", -86.0, -74.0, 31.0, 46.0, 800, "fold_belt"),
    ("Atlas", -11.0, 11.0, 27.0, 37.0, 1300, "fold_belt"),
    ("Planalto Etiope", 34.0, 43.0, 4.0, 16.0, 1900, "volcanic_arc"),
    ("Rift Africano", 27.0, 41.0, -16.0, 12.0, 1100, "volcanic_arc"),
    ("Drakensberg", 25.0, 33.0, -36.0, -24.0, 1500, "fold_belt"),
    ("Grande Cordilheira Divisória", 144.0, 153.0, -39.0, -14.0, 800, "fold_belt"),
    ("Alpes Japoneses", 135.0, 143.0, 33.0, 41.0, 1600, "volcanic_arc"),
    ("Anatólia", 27.0, 46.0, 35.0, 43.0, 1200, "fold_belt"),
    ("Planalto Iraniano", 44.0, 63.0, 24.0, 41.0, 1000, "fold_belt"),
    ("Planalto Mongol", 84.0, 121.0, 41.0, 53.0, 1300, "fold_belt"),
    ("Altai", 79.0, 96.0, 44.0, 56.0, 1500, "fold_belt"),
    ("Verkhoyansk", 119.0, 151.0, 54.0, 71.0, 900, "fold_belt"),
    ("Alasca", -168.0, -138.0, 54.0, 71.0, 1600, "fold_belt"),
    ("Groenlândia", -58.0, -18.0, 59.0, 84.0, 1500, "crystalline_shield"),
]

# (lon_min, lon_max, lat_min, lat_max, classe)
ARIDITY = [
    ("Sahara", -18.0, 33.0, 14.0, 31.0, "arid"),
    ("Arábia", 33.0, 60.0, 13.0, 32.0, "arid"),
    ("Sahel", -18.0, 36.0, 11.0, 18.0, "semi_arid"),
    ("Irã e Ásia Central", 44.0, 82.0, 29.0, 48.0, "semi_arid"),
    ("Gobi e Taklamakan", 74.0, 117.0, 34.0, 49.0, "arid"),
    ("Austrália interior", 114.0, 146.0, -33.0, -19.0, "arid"),
    ("Atacama", -73.0, -67.0, -31.0, -17.0, "arid"),
    ("Patagônia", -74.0, -64.0, -53.0, -39.0, "semi_arid"),
    ("Grande Bacia", -121.0, -107.0, 34.0, 44.0, "semi_arid"),
    ("Kalahari", 17.0, 29.0, -29.0, -19.0, "semi_arid"),
    ("Pradarias frias", 60.0, 120.0, 46.0, 56.0, "semi_arid"),
]

# Escudos cristalinos (geologia)
SHIELDS = [
    ("Escudo Canadense", -115.0, -60.0, 45.0, 72.0),
    ("Escudo Báltico", 10.0, 35.0, 55.0, 71.0),
    ("Escudo Brasileiro", -60.0, -35.0, -30.0, 5.0),
    ("Escudo Africano", 10.0, 40.0, -28.0, 12.0),
    ("Escudo Australiano", 115.0, 145.0, -35.0, -18.0),
    ("Escudo Indiano", 72.0, 88.0, 8.0, 25.0),
    ("Escudo Siberiano", 90.0, 125.0, 55.0, 72.0),
]

UPWELLING = [
    ("Peru", -85.0, -70.0, -20.0, -4.0),
    ("Namíbia", 8.0, 18.0, -31.0, -15.0),
    ("Norocidental da África", -26.0, -9.0, 9.0, 31.0),
]

# Culturas: (temp_min, temp_opt_min, temp_opt_max, temp_max,
#            precip_min, precip_opt_min, precip_opt_max, precip_max, altitude_max)
CROPS = {
    "wheat": (4.0, 10.0, 18.0, 24.0, 300.0, 450.0, 800.0, 1100.0, 2600.0),
    "rice": (16.0, 22.0, 28.0, 34.0, 800.0, 1200.0, 2000.0, 2600.0, 2000.0),
    "maize": (10.0, 18.0, 26.0, 32.0, 400.0, 600.0, 1000.0, 1400.0, 2600.0),
    "potato": (2.0, 8.0, 16.0, 22.0, 350.0, 500.0, 800.0, 1100.0, 3600.0),
    "soy": (12.0, 20.0, 28.0, 33.0, 500.0, 700.0, 1200.0, 1600.0, 2200.0),
    "sugarcane": (18.0, 24.0, 30.0, 36.0, 900.0, 1300.0, 2200.0, 2800.0, 1400.0),
    "coffee": (12.0, 17.0, 24.0, 28.0, 700.0, 1000.0, 1800.0, 2200.0, 2100.0),
    "fruit": (4.0, 10.0, 22.0, 28.0, 350.0, 500.0, 1000.0, 1300.0, 2400.0),
}

RESOURCES = {
    "iron": {"codes": {"FE", "FE_P"}, "base_kt": 30000.0, "recovery": 0.45,
             "depth": 140.0, "value": 12.0},
    "copper": {"codes": {"CU", "CU_O", "CU_S"}, "base_kt": 3000.0, "recovery": 0.40,
               "depth": 360.0, "value": 60.0},
    "coal": {"codes": {"COA"}, "prefix": "COA", "base_kt": 150000.0, "recovery": 0.55,
             "depth": 450.0, "value": 6.0},
    "oil": {"codes": {"OIL"}, "prefix": "OIL", "base_kt": 50000.0, "recovery": 0.25,
            "depth": 2200.0, "value": 45.0},
    "gas": {"codes": {"GAS"}, "base_kt": 30000.0, "recovery": 0.60,
            "depth": 2800.0, "value": 30.0},
}

STATUS_FACTOR = {
    "Producer": 2.0,
    "Past Producer": 1.5,
    "Plant": 1.2,
    "Prospect": 0.6,
    "Occurrence": 0.2,
}

# Depósitos de teste curados para os recursos em que o MRDS compacto tem poucos
# pontos com coordenada (carvão, petróleo e gás) e para ancorar os grandes
# distritos conhecidos de ferro e cobre. Coordenadas aproximadas de depósitos
# reais; quantidade/qualidade/profundidade são parâmetros de teste.
# (recurso, nome, lat, lon, quantidade_kt, qualidade, profundidade_m, status)
CURATED_DEPOSITS = [
    # Ferro
    ("iron", "Kiruna", 67.85, 20.23, 2400000.0, 0.86, 400.0, "Producer"),
    ("iron", "Lorraine", 49.15, 5.95, 900000.0, 0.62, 250.0, "Past Producer"),
    ("iron", "Pilbara", -22.6, 118.5, 5000000.0, 0.82, 150.0, "Producer"),
    ("iron", "Carajás", -6.0, -50.2, 4200000.0, 0.88, 200.0, "Producer"),
    ("iron", "Krivoy Rog", 47.9, 33.4, 1800000.0, 0.70, 600.0, "Producer"),
    ("iron", "Kursk", 51.7, 36.2, 3000000.0, 0.74, 500.0, "Producer"),
    ("iron", "Anshan", 41.1, 123.0, 1600000.0, 0.66, 300.0, "Producer"),
    ("iron", "Minas Gerais", -20.0, -43.9, 2200000.0, 0.78, 350.0, "Producer"),
    ("iron", "Labrador Trough", 54.5, -66.5, 1500000.0, 0.68, 450.0, "Prospect"),
    # Cobre
    ("copper", "Chuquicamata", -22.3, -68.9, 120000.0, 0.84, 900.0, "Producer"),
    ("copper", "Escondida", -24.3, -69.1, 150000.0, 0.88, 1100.0, "Producer"),
    ("copper", "El Teniente", -34.1, -70.4, 90000.0, 0.80, 1000.0, "Producer"),
    ("copper", "Grasberg", -4.1, 137.1, 80000.0, 0.82, 1200.0, "Producer"),
    ("copper", "Bingham Canyon", 40.5, -112.2, 60000.0, 0.70, 800.0, "Producer"),
    ("copper", "Norilsk", 69.3, 88.2, 70000.0, 0.76, 900.0, "Producer"),
    ("copper", "Katanga", -10.7, 25.5, 65000.0, 0.72, 500.0, "Producer"),
    ("copper", "Morenci", 33.1, -109.4, 45000.0, 0.66, 700.0, "Producer"),
    # Carvão
    ("coal", "Ruhr", 51.5, 7.0, 6000000.0, 0.78, 800.0, "Past Producer"),
    ("coal", "Alta Silésia", 50.2, 18.8, 5000000.0, 0.74, 700.0, "Producer"),
    ("coal", "Donbas", 48.3, 38.0, 4500000.0, 0.70, 900.0, "Producer"),
    ("coal", "Kuzbass", 54.5, 86.5, 8000000.0, 0.80, 400.0, "Producer"),
    ("coal", "Shanxi", 37.5, 112.5, 9000000.0, 0.76, 350.0, "Producer"),
    ("coal", "Powder River", 44.0, -105.5, 7000000.0, 0.72, 200.0, "Producer"),
    ("coal", "Apalaches", 37.5, -82.0, 4000000.0, 0.68, 500.0, "Past Producer"),
    ("coal", "Bowen", -22.5, 148.0, 3500000.0, 0.75, 300.0, "Producer"),
    ("coal", "Witbank", -26.0, 29.2, 3000000.0, 0.73, 250.0, "Producer"),
    ("coal", "Raniganj", 23.6, 87.1, 2500000.0, 0.66, 400.0, "Producer"),
    ("coal", "Yorkshire", 53.6, -1.4, 1200000.0, 0.70, 700.0, "Past Producer"),
    # Petróleo
    ("oil", "Ghawar", 25.4, 49.6, 1400000.0, 0.90, 2200.0, "Producer"),
    ("oil", "Kirkuk", 35.5, 44.4, 500000.0, 0.82, 1500.0, "Producer"),
    ("oil", "Brent", 60.8, 1.7, 450000.0, 0.80, 2800.0, "Past Producer"),
    ("oil", "Prudhoe Bay", 70.3, -148.3, 600000.0, 0.84, 2600.0, "Producer"),
    ("oil", "Permian", 31.8, -102.4, 900000.0, 0.78, 2500.0, "Producer"),
    ("oil", "Alberta", 57.0, -111.5, 800000.0, 0.70, 700.0, "Producer"),
    ("oil", "Maracaibo", 10.0, -71.0, 550000.0, 0.76, 1800.0, "Producer"),
    ("oil", "Níger", 4.8, 6.5, 400000.0, 0.74, 2000.0, "Producer"),
    ("oil", "Daqing", 46.6, 125.1, 500000.0, 0.72, 1500.0, "Producer"),
    ("oil", "Sibéria Ocidental", 61.0, 75.0, 1100000.0, 0.80, 2400.0, "Producer"),
    # Gás
    ("gas", "Urengoy", 66.0, 76.5, 1200000.0, 0.86, 3000.0, "Producer"),
    ("gas", "North Field", 26.0, 51.5, 1500000.0, 0.90, 3200.0, "Producer"),
    ("gas", "South Pars", 27.0, 52.5, 1400000.0, 0.88, 3100.0, "Producer"),
    ("gas", "Groningen", 53.3, 6.6, 300000.0, 0.82, 2900.0, "Past Producer"),
    ("gas", "Troll", 60.6, 3.7, 500000.0, 0.84, 3300.0, "Producer"),
    ("gas", "Permian Gás", 31.8, -102.4, 600000.0, 0.78, 2800.0, "Producer"),
    ("gas", "NW Shelf", -20.0, 115.5, 550000.0, 0.80, 3000.0, "Producer"),
    ("gas", "Trinidad", 10.4, -61.3, 250000.0, 0.76, 2600.0, "Producer"),
    # Petróleo offshore (pré-sal brasileiro e Mar do Norte norueguês)
    ("oil", "Tupi (Lula)", -25.0, -43.0, 1000000.0, 0.86, 2200.0, "Producer"),
    ("oil", "Búzios", -25.2, -42.0, 900000.0, 0.85, 2300.0, "Producer"),
    ("oil", "Mero", -24.5, -41.0, 800000.0, 0.84, 2200.0, "Producer"),
    ("oil", "Roncador", -22.0, -40.0, 600000.0, 0.78, 1800.0, "Producer"),
    ("oil", "Johan Sverdrup", 58.9, 2.4, 700000.0, 0.83, 1900.0, "Producer"),
    ("oil", "Ekofisk", 56.5, 3.2, 400000.0, 0.79, 3000.0, "Producer"),
    ("oil", "Gullfaks", 61.2, 2.3, 350000.0, 0.80, 2500.0, "Producer"),
    ("oil", "Oseberg", 60.5, 2.8, 300000.0, 0.79, 2600.0, "Producer"),
    # Gás (Oriente Médio, Norte da África e Barents)
    ("gas", "South Pars", 27.0, 52.5, 1400000.0, 0.88, 3100.0, "Producer"),
    ("gas", "Hassi R'Mel", 32.9, 3.3, 400000.0, 0.82, 2500.0, "Producer"),
    ("gas", "Bab", 23.9, 53.7, 300000.0, 0.78, 2800.0, "Producer"),
    ("gas", "Rhourde Nouss", 30.5, 5.5, 250000.0, 0.76, 2700.0, "Producer"),
    ("gas", "Snøhvit", 71.5, 21.0, 300000.0, 0.80, 3200.0, "Producer"),
]


# --------------------------------------------------------------------------
# Utilidades determinísticas
# --------------------------------------------------------------------------

def stable_unit(*parts):
    """Número estável em [0, 1] derivado dos argumentos (sem RNG global)."""
    raw = "|".join(str(part) for part in parts).encode("utf-8")
    digest = hashlib.sha1(raw).hexdigest()
    return int(digest[:12], 16) / float(0xFFFFFFFFFFFF)


def clamp(value, low, high):
    return max(low, min(high, value))


def in_box(lon, lat, box):
    """As tabelas têm o nome na posição 0: (nome, lon_min, lon_max, lat_min, lat_max...)."""
    return box[1] <= lon <= box[2] and box[3] <= lat <= box[4]


def dominant(values, default):
    if not values:
        return default
    counts = {}
    for value in values:
        counts[value] = counts.get(value, 0) + 1
    return max(counts.items(), key=lambda item: (item[1], item[0]))[0]


# --------------------------------------------------------------------------
# Leitura das definições
# --------------------------------------------------------------------------

def read_provinces():
    provinces = {}
    with open(os.path.join(DEFS, "provinces.txt"), "r", encoding="utf-8") as handle:
        for line in handle:
            parts = line.strip().split(";")
            if len(parts) < 8:
                continue
            province_id, kind = parts[0], parts[1]
            try:
                px, py = float(parts[5]), float(parts[6])
            except ValueError:
                continue
            provinces[province_id] = {
                "id": province_id,
                "kind": kind,
                "px": px,
                "py": py,
                "terrain": parts[7],
            }
    return provinces


def read_owners():
    with open(os.path.join(HIST, "provinces", "provinces.json"), "r", encoding="utf-8") as handle:
        raw = json.load(handle)
    return {key: value.get("province_owner", "") for key, value in raw.items()}


def read_territories():
    with open(os.path.join(HIST, "territories", "territories.json"), "r", encoding="utf-8") as handle:
        return json.load(handle)


def build_territory_map(provinces, owners, territories):
    """Território -> províncias de terra; província -> território."""
    province_to_territory = {}
    territory_members = {}
    for territory_id, data in territories.items():
        members = []
        for province_id in data.get("provinces", []):
            province = provinces.get(province_id)
            if province is None or province["kind"] == "lake":
                continue
            members.append(province_id)
            province_to_territory[province_id] = territory_id
        if members:
            territory_members[territory_id] = members
    return territory_members, province_to_territory


# --------------------------------------------------------------------------
# Natureza por província
# --------------------------------------------------------------------------

def load_nav_mask():
    image = Image.open(os.path.join(MAP_DATA, "nav_mask.png")).convert("L")
    return image


def is_coastal(nav, px, py, radius=7):
    width, height = nav.size
    cx = int(px / MAP_W * width)
    cy = int(py / MAP_H * height)
    for dy in range(-radius, radius + 1):
        for dx in range(-radius, radius + 1):
            if dx * dx + dy * dy > radius * radius:
                continue
            x, y = cx + dx, cy + dy
            if 0 <= x < width and 0 <= y < height:
                if nav.getpixel((x, y)) < 128:
                    return True
    return False


def annual_temperature(abs_lat):
    """Temperatura média anual aproximada por latitude (tabela de teste)."""
    table = [(0.0, 26.0), (10.0, 26.0), (20.0, 24.0), (30.0, 20.0), (40.0, 15.0),
             (50.0, 11.0), (60.0, 5.0), (70.0, -5.0), (80.0, -15.0), (90.0, -20.0)]
    for index in range(len(table) - 1):
        low_lat, low_temp = table[index]
        high_lat, high_temp = table[index + 1]
        if low_lat <= abs_lat <= high_lat:
            ratio = (abs_lat - low_lat) / (high_lat - low_lat)
            return low_temp + (high_temp - low_temp) * ratio
    return -20.0


def climate_for(lon, lat, altitude):
    abs_lat = abs(lat)
    if abs_lat < 10.0:
        zone = "tropical"
    elif abs_lat < 23.5:
        zone = "tropical_seasonal"
    elif abs_lat < 35.0:
        zone = "subtropical"
    elif abs_lat < 50.0:
        zone = "temperate"
    elif abs_lat < 60.0:
        zone = "cool_temperate"
    elif abs_lat < 66.5:
        zone = "subpolar"
    else:
        zone = "polar"

    aridity = "humid"
    for box in ARIDITY:
        if in_box(lon, lat, box):
            aridity = box[5]
            break

    if aridity == "arid":
        climate = zone + "_desert"
    elif aridity == "semi_arid":
        climate = zone + "_steppe"
    else:
        climate = zone + "_humid"

    base_temp = annual_temperature(abs_lat) - altitude * 0.0065
    if aridity == "arid":
        base_temp += 3.0
    if zone == "polar":
        base_temp -= 5.0
    temperature = clamp(base_temp, -30.0, 34.0)

    base_precip = {
        "tropical": 2000.0,
        "tropical_seasonal": 1400.0,
        "subtropical": 800.0,
        "temperate": 850.0,
        "cool_temperate": 650.0,
        "subpolar": 450.0,
        "polar": 200.0,
    }[zone]
    if aridity == "arid":
        base_precip *= 0.15
    elif aridity == "semi_arid":
        base_precip *= 0.45
    base_precip *= 1.0 + altitude / 5000.0
    return climate, zone, aridity, temperature, clamp(base_precip, 60.0, 4000.0)


def soil_for(zone, aridity):
    if aridity == "arid":
        return "arenosol", 0.08
    if aridity == "semi_arid":
        return "kastanozem", 0.35
    if zone == "tropical":
        return "ferralsol", 0.42
    if zone == "tropical_seasonal":
        return "acrisol", 0.48
    if zone == "subtropical":
        return "cambisol", 0.62
    if zone == "temperate":
        return "luvisol", 0.76
    if zone == "cool_temperate":
        return "podzol", 0.52
    if zone == "subpolar":
        return "podzol", 0.32
    return "tundra", 0.10


def relief_for(lon, lat, province_id):
    for box in MOUNTAINS:
        if in_box(lon, lat, box):
            altitude = box[5] * (0.8 + 0.4 * stable_unit(province_id, "alt"))
            return box[6], altitude
    altitude = 100.0 + 90.0 * stable_unit(province_id, "plain")
    return "sedimentary_basin", altitude


def forest_for(zone, aridity, altitude, fertility):
    base = {
        "tropical": 0.80,
        "tropical_seasonal": 0.55,
        "subtropical": 0.38,
        "temperate": 0.34,
        "cool_temperate": 0.50,
        "subpolar": 0.55,
        "polar": 0.02,
    }[zone]
    if aridity == "arid":
        base = 0.02
    elif aridity == "semi_arid":
        base *= 0.3
    base *= clamp(1.0 - altitude / 4500.0, 0.2, 1.0)
    base *= 0.6 + 0.6 * fertility
    return clamp(base, 0.0, 0.95)


def fishing_for(lon, lat, coastal):
    if not coastal:
        return 0.03
    for box in UPWELLING:
        if in_box(lon, lat, box):
            return 0.92
    if abs(lat) >= 30.0:
        return 0.78
    return 0.52


def crop_suitability(crop, temperature, precip, fertility, altitude):
    temp_min, temp_opt_min, temp_opt_max, temp_max, \
        precip_min, precip_opt_min, precip_opt_max, precip_max, alt_max = CROPS[crop]

    def trapezoid(value, low, opt_low, opt_high, high):
        if value <= low or value >= high:
            return 0.0
        if opt_low <= value <= opt_high:
            return 1.0
        if value < opt_low:
            return (value - low) / (opt_low - low)
        return (high - value) / (high - opt_high)

    temp_factor = trapezoid(temperature, temp_min, temp_opt_min, temp_opt_max, temp_max)
    water_factor = trapezoid(precip, precip_min, precip_opt_min, precip_opt_max, precip_max)
    soil_factor = clamp((fertility - 0.15) / 0.65, 0.0, 1.0)
    alt_factor = clamp(1.0 - altitude / (alt_max * 1.4), 0.0, 1.0)
    return round(temp_factor * water_factor * (0.35 + 0.65 * soil_factor) * alt_factor, 3)


def nature_for(province, nav):
    px, py = province["px"], province["py"]
    lon, lat = pixel_to_lonlat(px, py)
    coastal = is_coastal(nav, px, py)

    geology, altitude = relief_for(lon, lat, province["id"])
    if geology == "sedimentary_basin":
        for shield in SHIELDS:
            if in_box(lon, lat, shield):
                geology = "crystalline_shield"
                break

    climate, zone, aridity, temperature, precip = climate_for(lon, lat, altitude)
    soil_type, fertility = soil_for(zone, aridity)
    water = clamp(0.12 + precip / 1500.0 + (0.08 if coastal else 0.0), 0.0, 1.0)
    if fertility >= 0.7 and water >= 0.6:
        soil_type, fertility = "fluvisol", 0.88

    forest = forest_for(zone, aridity, altitude, fertility)
    relief = "mountain" if altitude >= 1500.0 else "hills" if altitude >= 500.0 else "plain"
    relief_penalty = 0.7 if relief == "mountain" else 0.3 if relief == "hills" else 0.0
    cultivable = clamp(fertility * water * (1.0 - 0.6 * forest) * (1.0 - relief_penalty),
                       0.0, 1.0)

    return {
        "climate": climate,
        "temp_c": round(temperature, 1),
        "precip_mm": round(precip),
        "soil_type": soil_type,
        "fertility": round(fertility, 3),
        "water": round(water, 3),
        "relief": relief,
        "altitude_m": round(altitude),
        "forest_cover": round(forest, 3),
        "geology": geology,
        "fishing": round(fishing_for(lon, lat, coastal), 3),
        "cultivable": round(cultivable, 3),
        "coastal": coastal,
    }


# --------------------------------------------------------------------------
# Agregação por território
# --------------------------------------------------------------------------

NUMERIC_NATURE = ["temp_c", "precip_mm", "fertility", "water", "altitude_m",
                  "forest_cover", "fishing", "cultivable"]


def aggregate_territory(territory_id, members, province_nature, provinces):
    nx, ny = 0.0, 0.0
    values = {key: [] for key in NUMERIC_NATURE}
    climates, soils, reliefs, geologies = [], [], [], []
    coastal = False

    for province_id in members:
        province = provinces[province_id]
        nature = province_nature[province_id]
        nx += province["px"]
        ny += province["py"]
        for key in NUMERIC_NATURE:
            values[key].append(nature[key])
        climates.append(nature["climate"])
        soils.append(nature["soil_type"])
        reliefs.append(nature["relief"])
        geologies.append(nature["geology"])
        coastal = coastal or nature["coastal"]

    count = float(len(members))
    entry = {
        "owner": "",
        "provinces": len(members),
        "centroid_px": [round(nx / count, 1), round(ny / count, 1)],
        "climate": dominant(climates, "temperate_humid"),
        "soil_type": dominant(soils, "luvisol"),
        "relief": dominant(reliefs, "plain"),
        "geology": dominant(geologies, "sedimentary_basin"),
        "coastal": coastal,
        "data_quality": "test",
    }
    for key in NUMERIC_NATURE:
        entry[key] = round(sum(values[key]) / count, 3)
    lon, lat = pixel_to_lonlat(entry["centroid_px"][0], entry["centroid_px"][1])
    entry["lon"] = round(lon, 2)
    entry["lat"] = round(lat, 2)
    return entry


def aggregate_agriculture(members, province_agriculture):
    totals = {crop: 0.0 for crop in CROPS}
    for province_id in members:
        for crop in CROPS:
            totals[crop] += province_agriculture[province_id][crop]
    count = float(len(members))
    return {crop: round(totals[crop] / count, 3) for crop in CROPS}


# --------------------------------------------------------------------------
# Depósitos minerais
# --------------------------------------------------------------------------

def build_province_grid(provinces, valid_ids):
    grid = {}
    cell = 20.0
    for province_id in valid_ids:
        province = provinces[province_id]
        key = (int(province["px"] // cell), int(province["py"] // cell))
        grid.setdefault(key, []).append(province_id)
    return grid, cell


def nearest_province(grid, cell, provinces, px, py, max_radius_px):
    gx, gy = int(px // cell), int(py // cell)
    max_ring = int(max_radius_px // cell) + 1
    best_id, best_dist = None, float("inf")
    for ring in range(max_ring + 1):
        for dy in range(-ring, ring + 1):
            for dx in range(-ring, ring + 1):
                if max(abs(dx), abs(dy)) != ring:
                    continue
                for province_id in grid.get((gx + dx, gy + dy), []):
                    province = provinces[province_id]
                    dist = (province["px"] - px) ** 2 + (province["py"] - py) ** 2
                    if dist < best_dist:
                        best_dist, best_id = dist, province_id
        if best_id is not None:
            break
    if best_id is None or math.sqrt(best_dist) > max_radius_px:
        return None
    return best_id


def resource_for_codes(codes):
    tokens = set(codes.replace(",", " ").split())
    for resource, spec in RESOURCES.items():
        if tokens & spec["codes"]:
            return resource
        prefix = spec.get("prefix")
        if prefix and any(token.startswith(prefix) for token in tokens):
            return resource
    return None


def load_mrds():
    records = []
    if not os.path.exists(MRDS_PATH):
        print("AVISO: %s ausente; gerando depósitos vazios." % MRDS_PATH)
        return records
    seen = set()
    with open(MRDS_PATH, "r", encoding="utf-8") as handle:
        for line in handle:
            try:
                record = json.loads(line)
            except ValueError:
                continue
            dep_id = str(record.get("id", ""))
            if not dep_id or dep_id in seen:
                continue
            seen.add(dep_id)
            records.append(record)
    return records


def deposit_payload(territory_id, resource, px, py, sites, names, status,
                    quantity_kt, quality, depth_m, curated):
    spec = RESOURCES[resource]
    recoverable = quantity_kt * spec["recovery"]
    extraction_cost = round(20.0 + depth_m * 0.02 + (1.0 - quality) * 30.0, 1)
    tech = 1 if depth_m < 500.0 and quality > 0.5 else 2 if depth_m < 2000.0 else 3
    max_rate = max(1.0, round(recoverable / (25.0 * 365.0), 1))
    discovery = round(0.1 if curated else
                      0.15 if status == "Producer" else
                      0.35 if status == "Past Producer" else
                      0.6 if status == "Prospect" else 0.75, 2)
    lon, lat = pixel_to_lonlat(px, py)
    return {
        "id": "",
        "territory": territory_id,
        "owner": "",
        "resource": resource,
        "px": round(px, 1),
        "py": round(py, 1),
        "lon": round(lon, 2),
        "lat": round(lat, 2),
        "quantity_kt": round(quantity_kt, 1),
        "recoverable_kt": round(recoverable, 1),
        "quality": round(quality, 3),
        "depth_m": round(depth_m),
        "extraction_cost": extraction_cost,
        "required_tech": tech,
        "max_rate_kt_day": max_rate,
        "discovery_difficulty": discovery,
        "sites": sites,
        "status": status,
        "names": names,
        "data_quality": "curated" if curated else "real_location_estimated_attributes",
    }


def build_deposits(provinces, province_to_territory, owners):
    records = load_mrds()
    valid_ids = list(province_to_territory.keys())
    grid, cell = build_province_grid(provinces, valid_ids)

    groups = {}
    for record in records:
        resource = resource_for_codes(record.get("codes", ""))
        if resource is None:
            continue
        px, py = lonlat_to_pixel(record["lon"], record["lat"])
        if px < 0 or px >= MAP_W or py < 0 or py >= MAP_H:
            continue

        radius = 160.0 if resource in ("oil", "gas") else 70.0
        province_id = nearest_province(grid, cell, provinces, px, py, radius)
        if province_id is None:
            continue
        territory_id = province_to_territory.get(province_id)
        if territory_id is None:
            continue

        key = (territory_id, resource, int(px // 50.0), int(py // 50.0))
        group = groups.setdefault(key, {"sites": 0, "px": 0.0, "py": 0.0,
                                        "names": [], "status": {}})
        group["sites"] += 1
        group["px"] += px
        group["py"] += py
        if record.get("name") and len(group["names"]) < 5:
            group["names"].append(record["name"])
        status = record.get("status", "Unknown") or "Unknown"
        group["status"][status] = group["status"].get(status, 0) + 1

    entries = []
    occupied = []
    for key, group in groups.items():
        territory_id, resource, grid_x, grid_y = key
        centroid_x = group["px"] / group["sites"]
        centroid_y = group["py"] / group["sites"]
        status = max(group["status"].items(), key=lambda item: (item[1], item[0]))[0]
        status_factor = STATUS_FACTOR.get(status, 0.4)
        noise = 0.6 + 0.8 * stable_unit(territory_id, resource, grid_x, grid_y)
        quantity = RESOURCES[resource]["base_kt"] * group["sites"] * status_factor * noise
        quality = round(0.35 + 0.5 * stable_unit(
            territory_id, resource, grid_x, grid_y, "q"), 3)
        depth = RESOURCES[resource]["depth"] * (0.6 + 1.0 * stable_unit(
            territory_id, resource, grid_x, grid_y, "d"))
        entries.append((resource, territory_id, centroid_x, centroid_y,
                        deposit_payload(territory_id, resource, centroid_x, centroid_y,
                                        group["sites"], group["names"], status,
                                        quantity, quality, depth, False)))
        occupied.append((resource, centroid_x, centroid_y))

    # Completa com os depósitos curados onde o MRDS não tem ponto representativo.
    for curated in CURATED_DEPOSITS:
        resource, name, lat, lon, quantity, quality, depth, status = curated
        px, py = lonlat_to_pixel(lon, lat)
        if any(item[0] == resource and
               (item[1] - px) ** 2 + (item[2] - py) ** 2 < 30.0 ** 2
               for item in occupied):
            continue
        province_id = nearest_province(grid, cell, provinces, px, py, 260.0)
        if province_id is None:
            continue
        territory_id = province_to_territory.get(province_id)
        if territory_id is None:
            continue
        entries.append((resource, territory_id, px, py,
                        deposit_payload(territory_id, resource, px, py, 1, [name],
                                        status, quantity, quality, depth, True)))
        occupied.append((resource, px, py))

    entries.sort(key=lambda item: (item[0], item[1], item[2], item[3]))
    deposits = []
    for index, (resource, _territory, _px, _py, payload) in enumerate(entries, start=1):
        payload["id"] = "DEP-%s-%06d" % (resource.upper(), index)
        payload["owner"] = owners.get(payload["territory"], "")
        deposits.append(payload)
    return deposits


# --------------------------------------------------------------------------
# Principal
# --------------------------------------------------------------------------

def main():
    os.makedirs(NATURAL_DIR, exist_ok=True)

    provinces = read_provinces()
    owners = read_owners()
    territories = read_territories()
    territory_members, province_to_territory = build_territory_map(
        provinces, owners, territories)
    territory_owner = {
        territory_id: owners.get(members[0], "")
        for territory_id, members in territory_members.items()
    }

    nav = load_nav_mask()
    province_nature = {}
    province_agriculture = {}
    for province_id in province_to_territory:
        province = provinces[province_id]
        nature = nature_for(province, nav)
        province_nature[province_id] = nature
        province_agriculture[province_id] = {
            crop: crop_suitability(crop, nature["temp_c"], nature["precip_mm"],
                                   nature["fertility"], nature["altitude_m"])
            for crop in CROPS
        }

    nature = {"_meta": {
        "description": "Geografia natural por território",
        "projection": "equirretangular global 5000x2500",
        "data_quality": "test",
        "method": "faixas de latitude + tabelas curadas de aridez, montanhas e escudos",
    }, "territories": {}}
    agriculture = {"_meta": {
        "description": "Aptidão agrícola por território (0..1)",
        "data_quality": "test",
        "crops": list(CROPS.keys()),
    }, "territories": {}}

    for territory_id in sorted(territory_members.keys()):
        members = territory_members[territory_id]
        entry = aggregate_territory(territory_id, members, province_nature, provinces)
        entry["owner"] = territory_owner.get(territory_id, "")
        nature["territories"][territory_id] = entry
        agriculture["territories"][territory_id] = aggregate_agriculture(
            members, province_agriculture)

    deposits = build_deposits(provinces, province_to_territory, territory_owner)

    sources = {
        "generated_by": "tools/build_natural_geography.py",
        "projection": "equirretangular 5000x2500 (lon -180..180, lat -90..90)",
        "nature": {
            "quality": "test",
            "method": "latitude bands + curated aridity/mountain/shield boxes",
            "note": "parâmetros de teste identificados; substituir por WorldClim/FAO/SRTM",
        },
        "agriculture": {
            "quality": "test",
            "method": "trapezoidal climate/water/soil suitability per crop",
        },
        "deposits": {
            "source": "USGS MRDS WFS compact (dep_id, site, status, commodity, point) + curated supplements",
            "quality": "real location, estimated attributes; curated where MRDS lacks coordinates",
            "records": len(load_mrds()),
            "curated_supplements": len(CURATED_DEPOSITS),
            "note": "quantity/quality/depth/cost estimated deterministically from sites and status",
        },
    }

    def write(name, payload):
        path = os.path.join(NATURAL_DIR, name)
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(payload, handle, indent="\t", ensure_ascii=False)
            handle.write("\n")
        print("%-22s %8.1f KB" % (name, os.path.getsize(path) / 1024.0))

    write("nature.json", nature)
    write("agriculture.json", agriculture)
    write("deposits.json", {"_meta": sources["deposits"], "deposits": deposits})
    write("_sources.json", sources)

    print("Territórios: %d | províncias de terra: %d | depósitos: %d" % (
        len(nature["territories"]), len(province_to_territory), len(deposits)))
    counts = {}
    for deposit in deposits:
        counts[deposit["resource"]] = counts.get(deposit["resource"], 0) + 1
    print("Depósitos por recurso:", counts)


if __name__ == "__main__":
    sys.exit(main())
