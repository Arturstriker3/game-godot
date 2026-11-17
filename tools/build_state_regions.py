#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""B3 - regiao da cidade = ESTADOS INTEIROS (admin-1 do Natural Earth).

Decisao do dono (docs/plans/perf_pop_financas_plano.md, B3): a Regiao de cada
cidade passa a ser um ou mais ESTADOS admin-1 completos, seguindo a divisa
real, em vez de "provincia -> cidade mais proxima da mesma nacao". A cidade e a
principal do estado.

Regras (todas deterministicas, sem numero inventado):

1. provincia do atlas -> admin-1 pai. O pipeline (`tools/build_atlas_layers.py`)
   garante que o id do admin-1 e o indice 1..N da fonte Natural Earth e que as
   filhas da subdivisao recebem id novo DEPOIS do ultimo admin-1, nome
   "<pai> N" ou o nome da cidade semente, alocadas em ordem crescente de pai.
   A reconstrucao usa as tres provas, nesta ordem:
     a) id <= N_ADMIN1: o proprio id e o pai;
     b) bloco de ocupacao (OCCUPIED_FRONT): ids logo apos N_ADMIN1 com nome de
       admin-1 ucraniano e nacao RUS;
     c) filha: nome sem o sufixo " N" com pai unico de mesmo nome/nacao; o
       intervalo monotonico entre filhas resolvidas como cerca; geometria
       (ponto-no-poligono do admin-1 da fonte) quando ainda ha mais de um
       candidato; erro se o resultado nao for monotonico (prova do pipeline).
2. populacao: mesma fonte e mesmo raster do hinterland (WorldPop 2026, 1 km),
   somada por provincia e apportionada em INTEIROS por nacao (maior resto) para
   que a soma das cidades bata `scenario/nations.json`. Apportionamento por
   PROVINCIA garante que estado, cidade e nacao somem o mesmo total.
3. area do estado: soma das areas reais de `natural/province_areas.json`.
4. estado com 0 cidades no roster: anexado ao estado VIZINHO (mesma nacao,
   fronteira comum no raster de provincias) que ja pertence a uma cidade. O
   anexo cresce em aneis (BFS multi-fonte): todos os estados de um anel sao
   decididos juntos, e quando dois vizinhos do mesmo anel tem cidade vence a de
   MAIOR POPULACAO (`cities.json`). A populacao comparada e a da cidade, nao a
   do estado: admin-1 minusculo (Westminster, 12 km2; Cair de Skopje, 5 km2)
   fica com populacao 0 no apportion e decidiria o anexo por acaso. Sem caminho
   ate nenhuma cidade, cai para o estado com cidade de centroide mais proximo.
5. estado com 2+ cidades: o estado inteiro vai para a cidade de MAIOR
   populacao (`cities.json`), e cada outra cidade fica com as provincias do
   estado das quais ela e a mais proxima (Voronoi dentro do estado). Cidade sem
   nenhuma provincia depois disso fica com regiao 0 e e listada no relatorio.
6. adjacencia entre estados: pares de pixels vizinhos de provincias diferentes
   em `world_province.png` (8192x4096, id = R + G*256) restritos a terra
   navegavel (`world_sdf.png` R>128 e `nav_mask.png`), senao o preenchimento
   do mar com a provincia de terra mais proxima criaria divisas no oceano.
   48 admin-1 sem nenhum pixel no raster (Westminster, Cair de Skopje,
   municipios de San Marino/Malta/Eslovenia) usam os 4 estados de mesma nacao
   com centroide mais proximo.

Saida: `data/definitions/population/state_regions.json`
    states: {state_id: {name, nation, provinces, area_km2, population_2026,
                        city_id, attach_rule}}
    cities: {city_id: {states, population_region, urban_population,
                       region_population}}

`state_id` e o `adm1_code` do Natural Earth (unico, estavel); quando uma mesma
admin-1 aparece em duas nacoes (frente RUS x UKR), a parte ocupada recebe o
sufixo "@RUS".

Uso:
  python tools/build_state_regions.py [--tif ARQ] [--compare]

Determinismo: sem data de execucao, ordem fixa, soma em float64, desempates por
id -> duas execucoes geram o mesmo SHA-256.
"""
import argparse
import bisect
import hashlib
import json
import os
import re
import sys

import numpy as np
import shapefile
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import build_hinterland_pop as hinterland  # noqa: E402
import geography_replica  # noqa: E402

Image.MAX_IMAGE_PIXELS = None

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "data", "definitions", "population", "state_regions.json")
PROVINCES = os.path.join(ROOT, "map", "map_data", "atlas", "provinces.json")
PROVINCE_RASTER = os.path.join(ROOT, "map", "map_data", "atlas", "world_province.png")
WORLD_SDF = os.path.join(ROOT, "map", "map_data", "atlas", "world_sdf.png")
NAV = os.path.join(ROOT, "map", "map_data", "nav_mask.png")
PROVINCE_AREAS = os.path.join(ROOT, "data", "definitions", "natural",
                              "province_areas.json")
PROVINCE_NATURE = os.path.join(ROOT, "data", "definitions", "natural",
                               "province_nature.json")
CITIES = os.path.join(ROOT, "data", "definitions", "scenario", "cities.json")
NATIONS = os.path.join(ROOT, "data", "definitions", "scenario", "nations.json")
HINTERLAND = os.path.join(ROOT, "data", "definitions", "population",
                          "hinterland_2026.json")
NE_DIR = os.environ.get("NE_DIR", r"G:\tools\ne_extract")
ADMIN1 = os.path.join(NE_DIR, "ne_10m_admin_1_states_provinces",
                      "ne_10m_admin_1_states_provinces")

VERSION = "B3-state-regions-2026-v1"
SRC_URL = ("https://data.worldpop.org/GIS/Population/Global_2015_2030/R2025A/"
           "2026/0_Mosaicked/v1/1km/constrained/global_pop_2026_CN_1km_R2025A_v1.tif")
SOURCE = ("WorldPop Global2 R2025A v1 - global_pop_2026_CN_1km_R2025A_v1.tif "
          "(1 km, ano 2026, mosaico global 'constrained'; CC BY 4.0 - "
          "WorldPop, University of Southampton; DOI 10.5258/SOTON/WP00845)")
NE_SOURCE = ("Natural Earth 1:10m admin-1 states/provinces "
             "(ne_10m_admin_1_states_provinces, versao em G:\\tools\\ne_extract)")
RETRIEVED = "2026-11-17"
ID_SPACE = 256 * 256

MAJOR_NATIONS = ["IND", "BRA", "USA", "CHN", "NGA", "RUS", "IDN", "PAK",
                 "FRA", "DEU"]


def load_json(path):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def norm(name):
    return re.sub(r"\s+", " ", str(name or "").strip().lower())


def lonlat_of(entry):
    """Centroide (px globais 5000x2500) -> (lon, lat), projecao do atlas."""
    return (float(entry["x"]) / 5000.0 * 360.0 - 180.0,
            90.0 - float(entry["y"]) / 2500.0 * 180.0)


# ---------------------------------------------------------------------------
# admin-1 da fonte
# ---------------------------------------------------------------------------
class Admin1:
    """Le a fonte Natural Earth admin-1 sob demanda (shapefile de 4.596)."""

    def __init__(self, path=ADMIN1):
        if not os.path.exists(path + ".shp"):
            raise SystemExit("fonte Natural Earth admin-1 ausente: %s.shp" % path)
        self.reader = shapefile.Reader(path)
        names = [field[0] for field in self.reader.fields][1:]
        self.fields = names
        self.records = [dict(zip(names, list(rec))) for rec in self.reader.records()]
        self.count = len(self.records)
        # bbox de cada admin-1 numa passada so (os pontos sao descartados)
        self.bboxes = []
        for shape in self.reader.iterShapes():
            self.bboxes.append(tuple(float(v) for v in shape.bbox))
        if len(self.bboxes) != self.count:
            raise SystemExit("admin-1: bbox e registros nao batem")

    def meta(self, adm1_id):
        rec = self.records[adm1_id - 1]
        return {
            "name": str(rec.get("name") or "").strip(),
            "adm1_code": str(rec.get("adm1_code") or "").strip(),
            "iso": str(rec.get("iso_3166_2") or "").strip(),
            "adm0": str(rec.get("adm0_a3") or "").strip(),
        }

    def contains(self, adm1_id, lon, lat):
        """Ponto dentro do poligono admin-1 (ray casting, multiplas partes)."""
        shape = self.reader.shape(adm1_id - 1)
        points = np.asarray(shape.points, dtype=np.float64)
        parts = list(shape.parts) + [len(points)]
        for start, stop in zip(parts[:-1], parts[1:]):
            ring = points[start:stop]
            if len(ring) < 3:
                continue
            x, y = ring[:, 0], ring[:, 1]
            inside = False
            j = len(ring) - 1
            for i in range(len(ring)):
                yi, yj = y[i], y[j]
                if (yi > lat) != (yj > lat):
                    xcross = (x[j] - x[i]) * (lat - yi) / (yj - yi) + x[i]
                    if lon < xcross:
                        inside = not inside
                j = i
            if inside:
                return True
        return False

    def distance(self, adm1_id, lon, lat):
        """Distancia minima (graus) do ponto ao contorno admin-1."""
        shape = self.reader.shape(adm1_id - 1)
        points = np.asarray(shape.points, dtype=np.float64)
        parts = list(shape.parts) + [len(points)]
        best = float("inf")
        for start, stop in zip(parts[:-1], parts[1:]):
            ring = points[start:stop]
            if len(ring) < 2:
                continue
            a = ring
            b = np.roll(ring, -1, axis=0)
            ab = b - a
            ap = np.array([lon, lat]) - a
            denom = (ab * ab).sum(axis=1)
            t = np.clip((ab * ap).sum(axis=1) / np.maximum(denom, 1e-12), 0.0, 1.0)
            closest = a + ab * t[:, None]
            dist = np.hypot(closest[:, 0] - lon, closest[:, 1] - lat).min()
            best = min(best, float(dist))
        return best


def province_parent_map(provinces, admin1):
    """provincia -> id do admin-1 pai, com as provas do pipeline."""
    by_id = {int(entry["id"]): entry for entry in provinces}
    total_admin1 = admin1.count
    parent = {}
    for pid in by_id:
        if pid <= total_admin1:
            parent[pid] = pid
    kids = sorted((entry for entry in provinces if int(entry["id"]) > total_admin1),
                  key=lambda entry: int(entry["id"]))
    if not kids:
        return parent, set()

    ukr_names = {norm(admin1.meta(i + 1)["name"])
                 for i in range(admin1.count)
                 if admin1.meta(i + 1)["adm0"] == "UKR"}
    parents_with_name = {}
    for pid, entry in by_id.items():
        if pid <= total_admin1:
            parents_with_name.setdefault((str(entry["nation"]), norm(entry["name"])),
                                         []).append(pid)
    # bloco de ocupacao: id novo com nome de admin-1 ucraniano e nacao RUS
    occupation = []
    for entry in kids:
        if str(entry["nation"]) != "RUS" or norm(entry["name"]) not in ukr_names:
            continue
        target = parents_with_name.get(("UKR", norm(entry["name"]))) or \
            parents_with_name.get(("RUS", norm(entry["name"]))) or []
        if len(target) != 1:
            raise SystemExit("ocupacao: pai ambiguo para %s (%s)"
                             % (entry["id"], entry["name"]))
        occupation.append((int(entry["id"]), target[0]))
    occupation_ids = {pid for pid, _ in occupation}
    if occupation_ids:
        expected = min(occupation_ids)
        if sorted(occupation_ids) != list(range(expected,
                                                expected + len(occupation_ids))):
            raise SystemExit("ocupacao: ids novos nao sao consecutivos")
        if expected != total_admin1 + 1:
            raise SystemExit("ocupacao: bloco fora do inicio da faixa nova")
    for pid, target in occupation:
        parent[pid] = target
    roots = set(parent.values()) | occupation_ids

    # nome: "<pai> N" (o pai mantem o nome, as filhas ganham sufixo numerico)
    number = re.compile(r"^(.*) (\d+)$")
    rest = [entry for entry in kids if int(entry["id"]) not in occupation_ids]
    name_parent = {}
    unresolved = []
    for entry in rest:
        pid = int(entry["id"])
        match = number.match(str(entry["name"]).strip())
        candidate = None
        if match:
            key = (str(entry["nation"]), norm(match.group(1)))
            found = parents_with_name.get(key) or []
            if len(found) == 1:
                candidate = found[0]
        if candidate is None:
            unresolved.append(entry)
        else:
            name_parent[pid] = candidate
            parent[pid] = candidate

    # cerca monotonica + geometria para as que o nome nao resolveu
    resolved_ids = sorted(name_parent)
    for entry in unresolved:
        pid = int(entry["id"])
        index = bisect.bisect_left(resolved_ids, pid)
        low = name_parent[resolved_ids[index - 1]] if index > 0 else 0
        high = name_parent[resolved_ids[index]] if index < len(resolved_ids) else 10 ** 6
        candidates = sorted(root for root in roots
                            if low <= root <= high
                            and str(by_id[root]["nation"]) == str(entry["nation"]))
        if not candidates:
            candidates = sorted(root for root in roots
                                if str(by_id[root]["nation"]) == str(entry["nation"])
                                and low <= root)
        lon, lat = lonlat_of(entry)
        # id do bloco de ocupacao usa a geometria do admin-1 de origem
        def geo_id(root):
            return parent.get(root, root) if root > admin1.count else root

        inside = [root for root in candidates
                  if admin1.contains(geo_id(root), lon, lat)]
        if len(inside) == 1:
            chosen = inside[0]
        elif inside:
            chosen = min(inside)
        elif candidates:
            chosen = min(candidates,
                         key=lambda root: (admin1.distance(geo_id(root), lon, lat),
                                           root))
        else:
            raise SystemExit("filha sem pai candidato: %s (%s, %s)"
                             % (pid, entry["name"], entry["nation"]))
        parent[pid] = chosen

    # prova do pipeline: filhas da subdivisao alocadas em ordem crescente de
    # pai (o bloco de ocupacao vem antes e nao entra na prova)
    subdivision = [entry for entry in kids
                   if int(entry["id"]) not in occupation_ids]
    order = [parent[int(entry["id"])] for entry in subdivision]
    for index in range(len(order) - 1):
        if order[index + 1] < order[index]:
            raise SystemExit("mapa pai->filha nao monotonico em %s: %r"
                             % (subdivision[index]["id"],
                                order[max(0, index - 3):index + 4]))
    return parent, occupation_ids


# ---------------------------------------------------------------------------
# raster de provincias
# ---------------------------------------------------------------------------
def province_raster():
    raster = np.asarray(Image.open(PROVINCE_RASTER).convert("RGB"), dtype=np.int32)
    height, width = raster.shape[:2]
    ids = (raster[:, :, 0] + raster[:, :, 1] * 256).astype(np.uint16)
    del raster
    land = np.asarray(Image.open(WORLD_SDF).convert("RGBA"))[:, :, 0] > 128
    nav = np.asarray(Image.open(NAV).convert("L")
                     .resize((width, height), Image.NEAREST)) >= 128
    return ids, land & nav, width, height


def state_adjacency(state_index, state_count, chunk_rows=512):
    """Pares de estados com fronteira no raster (contagem de pixels vizinhos)."""
    pairs = {}
    height = state_index.shape[0]
    for row0 in range(0, height, chunk_rows):
        row1 = min(row0 + chunk_rows + 1, height)
        block = state_index[row0:row1]
        if block.shape[0] < 2:
            continue
        for a, b in ((block[:-1, :], block[1:, :]),
                     (block[:, :-1], block[:, 1:])):
            mask = (a != b) & (a > 0) & (b > 0)
            if not mask.any():
                continue
            left = a[mask].astype(np.int64)
            right = b[mask].astype(np.int64)
            low = np.minimum(left, right)
            high = np.maximum(left, right)
            key = low * state_count + high
            unique, counts = np.unique(key, return_counts=True)
            for k, c in zip(unique.tolist(), counts.tolist()):
                pairs[k] = pairs.get(k, 0) + c
        del block
    out = {}
    for key, count in pairs.items():
        low, high = divmod(key, state_count)
        out[(low, high)] = count
    return out


# ---------------------------------------------------------------------------
# populacao, area e aptidao
# ---------------------------------------------------------------------------
def province_population(tif_path):
    ids, keep, width, height = province_raster()
    pops, raster_total, outside, masked_out, wp_shape = \
        hinterland.province_population(tif_path, ids, keep, width, height)
    return ids, keep, width, height, pops, {
        "raster_total": raster_total,
        "outside": outside,
        "masked_out": masked_out,
        "wp_shape": wp_shape,
    }


def nation_factors(provinces, pops, playable, target):
    atlas_to_game = geography_replica.game_tag_map(playable)
    raster_by_nation = {tag: 0.0 for tag in playable}
    for entry in provinces:
        game_tag = atlas_to_game.get(str(entry.get("nation") or ""), "")
        if game_tag == "":
            continue
        raster_by_nation[game_tag] += pops.get(int(entry["id"]), 0.0)
    factor = {}
    for tag in playable:
        factor[tag] = (target[tag] / raster_by_nation[tag]
                       if raster_by_nation[tag] > 0.0 else 1.0)
    return factor


def apportion_provinces(provinces, pops, factor, target, playable):
    """populacao inteira por provincia, somando exatamente o total nacional."""
    atlas_to_game = geography_replica.game_tag_map(playable)
    by_nation = {}
    for entry in provinces:
        tag = atlas_to_game.get(str(entry.get("nation") or ""), "")
        if tag == "":
            continue
        pid = int(entry["id"])
        by_nation.setdefault(tag, {})[pid] = pops.get(pid, 0.0) * factor[tag]
    out = {}
    for tag in sorted(by_nation):
        out.update(hinterland.apportion(by_nation[tag], round(target[tag])))
    return out


# ---------------------------------------------------------------------------
# distribuicao cidade <-> estado
# ---------------------------------------------------------------------------
def city_home_state(city, admin1, states, roots_of_tag, provinces_by_id,
                    parent):
    """admin-1 que contem a cidade (geometria da fonte), na nacao da cidade.

    O raster do atlas pode jogar uma cidade costeira na provincia vizinha
    (Nova York -> Nova Jersey, San Juan, Naha); a fonte admin-1 e a definicao
    do estado, entao ela manda aqui. Sem poligono contendo o ponto, cai para o
    estado de provincia com centroide mais proximo.
    """
    tag = str(city["nation"])
    options = roots_of_tag.get(tag) or {}
    if not options:
        return None
    lon, lat = lonlat_of(city)
    inside = []
    for root, state_id in options.items():
        base = parent.get(root, root) if root > admin1.count else root
        bbox = admin1.bboxes[base - 1]
        if not (bbox[0] <= lon <= bbox[2] and bbox[1] <= lat <= bbox[3]):
            continue
        if admin1.contains(base, lon, lat):
            inside.append((root, state_id))
    if inside:
        return min(inside)[1]
    cx, cy = float(city["x"]), float(city["y"])

    def centroid_distance(state_id):
        pids = states[state_id]["provinces"]
        return min((float(provinces_by_id[pid]["x"]) - cx) ** 2
                   + (float(provinces_by_id[pid]["y"]) - cy) ** 2
                   for pid in pids)

    return min(options.values(),
               key=lambda sid: (centroid_distance(sid), sid))


def home_province_of(city, state, ids, width, height, provinces_by_id):
    """Provincia do estado que contem a cidade (pixel; senao a mais proxima)."""
    col = min(width - 1, max(0, int(float(city["x"]) / 5000.0 * width)))
    row = min(height - 1, max(0, int(float(city["y"]) / 2500.0 * height)))
    pid = int(ids[row, col])
    if pid in state["provinces"]:
        return pid
    cx, cy = float(city["x"]), float(city["y"])
    return min(state["provinces"],
               key=lambda other: ((float(provinces_by_id[other]["x"]) - cx) ** 2
                                  + (float(provinces_by_id[other]["y"]) - cy) ** 2,
                                  other))


def distance2(province, city):
    dx = float(province["x"]) - float(city["x"])
    dy = float(province["y"]) - float(city["y"])
    return dx * dx + dy * dy


def assign_cities(ids, width, height, provinces_by_id, states, cities,
                  roots_of_tag, admin1, parent):
    """Devolve {province_id: city_id} e os casos especiais declarados.

    Regra do dono: o estado inteiro vai para a cidade de MAIOR populacao; as
    outras cidades do estado ficam com as provincias das quais SAO a mais
    proxima (Voronoi dentro do estado). A cidade principal nunca perde a
    provincia que a contem, e cidade que sobra sem nenhuma provincia e
    declarada no relatorio (nao some em silencio).
    """
    city_by_id = {str(city["id"]): city for city in cities}
    home_province = {}
    cities_of_state = {}
    for city in cities:
        city_id = str(city["id"])
        state_id = city_home_state(city, admin1, states, roots_of_tag,
                                   provinces_by_id, parent)
        if state_id is None:
            continue
        pid = home_province_of(city, states[state_id], ids, width, height,
                               provinces_by_id)
        home_province[city_id] = pid
        cities_of_state.setdefault(state_id, []).append(city_id)

    owner = {}
    specials = {"primary_left_empty": [], "city_left_empty": [],
                "city_no_home_state": []}
    for state_id in sorted(states):
        members = sorted(cities_of_state.get(state_id, []),
                         key=lambda cid: (-float(city_by_id[cid].get("population")
                                                 or 0.0), cid))
        province_ids = states[state_id]["provinces"]
        if not members:
            continue
        primary = members[0]
        primary_home = home_province.get(primary)
        for pid in province_ids:
            if pid == primary_home:
                owner[pid] = primary
                continue
            province = provinces_by_id[pid]
            best = None
            for city_id in members[1:]:
                distance = distance2(province, city_by_id[city_id])
                if best is None or (distance, city_id) < best:
                    best = (distance, city_id)
            if best is not None and best[0] < distance2(province,
                                                       city_by_id[primary]):
                owner[pid] = best[1]
            else:
                owner[pid] = primary
        for city_id in members[1:]:
            if not any(cid == city_id for cid in owner.values()):
                specials["city_left_empty"].append((city_id, state_id))
        if not any(cid == primary for cid in owner.values()):
            specials["primary_left_empty"].append((primary, state_id))
    return owner, home_province, cities_of_state, specials


# ---------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--tif", default=hinterland.DEFAULT_TIF,
                        help="mosaico WorldPop 1 km")
    parser.add_argument("--compare", action="store_true",
                        help="compara com hinterland_2026.json e imprime tabelas")
    args = parser.parse_args()
    if not os.path.isfile(args.tif):
        raise SystemExit("TIFF nao encontrado: %s (ver %s)" % (args.tif, SRC_URL))

    checksum = hinterland.sha256_file(args.tif)
    print("fonte: %s" % SOURCE)
    print("arquivo: %s  sha256=%s" % (os.path.relpath(args.tif, ROOT), checksum))

    provinces = load_json(PROVINCES)["provinces"]
    provinces_by_id = {int(entry["id"]): entry for entry in provinces}
    admin1 = Admin1()
    print("admin-1 da fonte: %d" % admin1.count)

    parent, occupation_ids = province_parent_map(provinces, admin1)
    missing = [pid for pid in provinces_by_id if pid not in parent]
    if missing:
        raise SystemExit("provincias sem pai: %r" % missing[:10])
    print("provincias mapeadas: %d de %d" % (len(parent), len(provinces_by_id)))

    nations = load_json(NATIONS)
    playable = {str(nation["id"]) for nation in nations}
    target = {str(nation["id"]): float(nation.get("population") or 0.0)
              for nation in nations}
    # nacao do estado/cidade = tag do JOGO (NED, XKX, SSD), nao o ADM0_A3 do
    # atlas (NLD, KOS, SDS): e o elo com cities.json/nations.json
    atlas_to_game = geography_replica.game_tag_map(playable)
    game_of_province = {pid: atlas_to_game.get(str(entry["nation"]),
                                               str(entry["nation"]))
                        for pid, entry in provinces_by_id.items()}

    # --- estados ------------------------------------------------------------
    # O bloco de ocupacao (OCCUPIED_FRONT) e um ESTADO proprio da nacao alvo:
    # ele nao se junta ao admin-1 de origem, senao os dois lados da frente
    # colidiriam na mesma chave. `parent` continua dando a geometria.
    group_of = {pid: (pid if pid in occupation_ids else root)
                for pid, root in parent.items()}
    members = {}
    for pid, root in group_of.items():
        members.setdefault(root, []).append(pid)
    states = {}
    province_state = {}
    for root in sorted(members):
        base = parent[root] if root in occupation_ids else root
        meta = admin1.meta(base)
        tags = sorted({game_of_province[pid] for pid in members[root]})
        for tag in tags:
            state_id = meta["adm1_code"] if (len(tags) == 1 and
                                             root not in occupation_ids) else \
                "%s@%s" % (meta["adm1_code"], tag)
            pids = sorted(pid for pid in members[root]
                          if game_of_province[pid] == tag)
            states[state_id] = {
                "name": meta["name"],
                "nation": tag,
                "provinces": pids,
                "adm1_code": meta["adm1_code"],
                "iso_3166_2": meta["iso"],
                "root": root,
            }
            for pid in pids:
                province_state[pid] = state_id
    print("estados: %d (admin-1 com parte ocupada: %d)"
          % (len(states), len(occupation_ids)))
    roots_of_tag = {}
    for state_id in sorted(states):
        roots_of_tag.setdefault(states[state_id]["nation"], {})[
            states[state_id]["root"]] = state_id

    # --- populacao / area ---------------------------------------------------
    ids, keep, width, height, pops, diag = province_population(args.tif)
    cities = load_json(CITIES)
    factor = nation_factors(provinces, pops, playable, target)
    pop_by_province = apportion_provinces(provinces, pops, factor, target, playable)
    areas = load_json(PROVINCE_AREAS)["areas"]
    nature = load_json(PROVINCE_NATURE).get("provinces", {})
    city_by_id = {str(city["id"]): city for city in cities}

    owner, home_province, cities_of_state, specials = assign_cities(
        ids, width, height, provinces_by_id, states, cities, roots_of_tag,
        admin1, parent)

    # --- adjacencia e anexacao de estado sem cidade -------------------------
    state_ids = sorted(states)
    state_index = {state_id: index + 1 for index, state_id in enumerate(state_ids)}
    lut = np.zeros(ID_SPACE, dtype=np.uint16)
    for pid, state_id in province_state.items():
        lut[pid] = state_index[state_id]
    state_px = np.where(keep, lut[ids], 0)
    del keep
    pairs = state_adjacency(state_px, len(state_ids) + 1)
    pixel_count = np.bincount(state_px.ravel(), minlength=len(state_ids) + 1)
    del state_px
    neighbors = {state_id: set() for state_id in state_ids}
    for (low, high), _count in pairs.items():
        a, b = state_ids[low - 1], state_ids[high - 1]
        if states[a]["nation"] != states[b]["nation"]:
            continue
        neighbors[a].add(b)
        neighbors[b].add(a)

    # 48 admin-1 minusculos (Westminster, Cair de Skopje, municipios de San
    # Marino, Malta, Eslovenia) nao tem NENHUM pixel no raster: sem isto
    # ficariam isolados e anexariam a cidade errada. Vizinhanca aproximada
    # pelos 4 estados de mesma nacao com centroide mais proximo, declarada.
    by_nation_states = {}
    for state_id in state_ids:
        by_nation_states.setdefault(states[state_id]["nation"], []).append(state_id)

    def state_center(state_id):
        pids = states[state_id]["provinces"]
        return (sum(float(provinces_by_id[pid]["x"]) for pid in pids) / len(pids),
                sum(float(provinces_by_id[pid]["y"]) for pid in pids) / len(pids))

    pixel_free = [state_id for state_id in state_ids
                  if pixel_count[state_index[state_id]] == 0]
    for state_id in pixel_free:
        cx, cy = state_center(state_id)
        peers = [other for other in by_nation_states[states[state_id]["nation"]]
                 if other != state_id]
        nearest = sorted(peers, key=lambda other: (
            (state_center(other)[0] - cx) ** 2
            + (state_center(other)[1] - cy) ** 2, other))[:4]
        for other in nearest:
            neighbors[state_id].add(other)
            neighbors[other].add(state_id)

    city_of_state = {}
    for state_id, members_list in cities_of_state.items():
        city_of_state[state_id] = max(
            members_list,
            key=lambda cid: (float(city_by_id[cid].get("population") or 0.0), cid))

    pending = [state_id for state_id in state_ids if state_id not in city_of_state]
    city_population = {cid: float(city_by_id[cid].get("population") or 0.0)
                       for cid in city_by_id}
    attach_rule = {}
    # Anel a anel (BFS multi-fonte): o estado sem cidade junta-se ao VIZINHO
    # que ja pertence a uma cidade; quando ha mais de um na mesma distancia,
    # vence a cidade de maior populacao. Sem ordem arbitraria: todos os aneis
    # sao decididos juntos, entao o resultado nao depende do id do estado.
    frontier = {state_id: city_of_state[state_id] for state_id in city_of_state}
    while frontier and pending:
        candidates = {}
        for state_id in sorted(frontier):
            city_id = frontier[state_id]
            for other in neighbors[state_id]:
                if other in city_of_state:
                    continue
                key = (city_population.get(city_id, 0.0), city_id)
                if other not in candidates or key > candidates[other]:
                    candidates[other] = key
        if not candidates:
            break
        for other in sorted(candidates):
            population, city_id = candidates[other]
            city_of_state[other] = city_id
            attach_rule[other] = "neighbor_largest_population"
        frontier = {other: city_of_state[other] for other in sorted(candidates)}
        pending = [state_id for state_id in pending if state_id not in city_of_state]

    # nenhum vizinho com cidade: estado com cidade mais proximo (centroide)
    for state_id in list(pending):
        nation = states[state_id]["nation"]
        pids = states[state_id]["provinces"]
        cx = sum(float(provinces_by_id[pid]["x"]) for pid in pids) / len(pids)
        cy = sum(float(provinces_by_id[pid]["y"]) for pid in pids) / len(pids)
        options = [other for other in state_ids
                   if states[other]["nation"] == nation and other in city_of_state]
        if not options:
            attach_rule[state_id] = "no_city_in_nation"
            continue

        def centroid_distance(other):
            other_pids = states[other]["provinces"]
            ox = sum(float(provinces_by_id[pid]["x"]) for pid in other_pids) / \
                len(other_pids)
            oy = sum(float(provinces_by_id[pid]["y"]) for pid in other_pids) / \
                len(other_pids)
            return (ox - cx) ** 2 + (oy - cy) ** 2

        best = min(options, key=lambda other: (centroid_distance(other), other))
        attach_rule[state_id] = "nearest_centroid"
        city_of_state[state_id] = city_of_state[best]
        pending.remove(state_id)

    # provincias dos estados que ficaram sem cidade em nacao sem cidade
    for state_id in pending:
        for pid in states[state_id]["provinces"]:
            owner.pop(pid, None)

    # provincias dos estados anexados/primarios que ainda nao tem dono
    for state_id in state_ids:
        city_id = city_of_state.get(state_id)
        if city_id is None:
            continue
        for pid in states[state_id]["provinces"]:
            owner.setdefault(pid, city_id)

    # --- agregacao ----------------------------------------------------------
    city_states = {}
    for pid, city_id in owner.items():
        city_states.setdefault(city_id, set()).add(province_state[pid])
    city_region = {}
    for pid, city_id in owner.items():
        city_region[city_id] = city_region.get(city_id, 0) + \
            pop_by_province.get(pid, 0)

    urban_by_city = {str(city["id"]): int(round(float(city.get("urban_population")
                                                      or 0.0)))
                     for city in cities}
    cities_out = {}
    for city in cities:
        city_id = str(city["id"])
        region = int(city_region.get(city_id, 0))
        urban = urban_by_city[city_id]
        cities_out[city_id] = {
            "states": sorted(city_states.get(city_id, set())),
            "population_region": region,
            "urban_population": urban,
            "region_population": max(0, region - urban),
        }

    states_out = {}
    for state_id in state_ids:
        state = states[state_id]
        pids = state["provinces"]
        states_out[state_id] = {
            "name": state["name"],
            "nation": state["nation"],
            "provinces": pids,
            "area_km2": round(sum(float(areas.get(str(pid), 0.0))
                                  for pid in pids), 1),
            "population_2026": int(sum(pop_by_province.get(pid, 0)
                                       for pid in pids)),
            "city_id": city_of_state.get(state_id),
            "attach_rule": attach_rule.get(state_id, "primary_city"),
        }

    multi_city = {}
    for state_id, members_list in cities_of_state.items():
        if len(members_list) > 1:
            multi_city[state_id] = sorted(
                members_list,
                key=lambda cid: (-float(city_by_id[cid].get("population") or 0.0),
                                 cid))

    method = (
        "regiao da cidade = estados admin-1 INTEIROS do Natural Earth "
        "(ne_10m_admin_1_states_provinces); provincia -> admin-1 pai "
        "reconstruido com as tres provas do pipeline tools/build_atlas_layers.py "
        "(id <= 4596 = id da fonte; bloco de ocupacao RUS com nome de admin-1 "
        "ucraniano; filha = '<pai> N' com pai unico de mesmo nome/nacao, cerca "
        "monotonica de ids e, quando ambiguo, ponto-no-poligono do admin-1), "
        "validado pela monotonidade das filhas (ordem de alocacao do pipeline); "
        "populacao por provincia = WorldPop 2026 1 km somada no raster "
        "world_province.png (id=R+G*256) x fator da nacao, apportionada em "
        "inteiros por maior resto dentro da nacao para somar nations.json; "
        "estado sem cidade no roster anexado ao vizinho de fronteira comum no "
        "raster que ja pertence a uma cidade, anel a anel (BFS multi-fonte), "
        "vencendo a CIDADE de maior populacao quando ha mais de um vizinho no "
        "mesmo anel; sem vizinho com cidade = estado com cidade mais proximo "
        "pelo centroide; estado com 2+ cidades "
        "vai inteiro para a de maior populacao e as demais ficam com as "
        "provincias do estado das quais sao as mais proximas (Voronoi); "
        "adjacencia por pixels vizinhos de estados diferentes no raster de "
        "provincias restrito a terra navegavel (world_sdf x nav_mask), com os "
        "4 estados de mesma nacao mais proximos para as 48 admin-1 sem pixel; "
        "region_population = population_region - urban_population (piso 0)"
    )

    out = {
        "version": VERSION,
        "source": SOURCE,
        "source_admin1": NE_SOURCE,
        "url": SRC_URL,
        "sha256": checksum,
        "retrieved": RETRIEVED,
        "license": "CC BY 4.0 (WorldPop, University of Southampton)",
        "method": method,
        "states": states_out,
        "cities": {city_id: cities_out[city_id] for city_id in sorted(cities_out)},
    }
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(out, handle, ensure_ascii=False, indent=1, sort_keys=True)
        handle.write("\n")
    out_hash = hashlib.sha256(open(OUT, "rb").read()).hexdigest()

    # --- validacao ----------------------------------------------------------
    total_states = sum(entry["population_2026"] for entry in states_out.values())
    total_region = sum(entry["population_region"] for entry in cities_out.values())
    total_urban = sum(entry["urban_population"] for entry in cities_out.values())
    total_region_pop = sum(entry["region_population"] for entry in cities_out.values())
    print("\n== raster ==")
    print("soma do raster (todas as celulas) = %.0f" % diag["raster_total"])
    print("populacao em provincia do atlas     = %.0f" % sum(pops.values()))
    print("fora de provincia (id 0)            = %.0f" % diag["outside"])
    print("\n== totais ==")
    print("estados                             = %d" % len(states_out))
    print("cidades                             = %d" % len(cities_out))
    print("soma dos estados                    = %d" % total_states)
    print("soma das regioes das cidades        = %d" % total_region)
    print("nations.json somado                 = %d" % round(sum(target.values())))
    print("urbano (cities.json)                = %d" % total_urban)
    print("regiao (regiao - urbano)            = %d" % total_region_pop)
    if total_region > total_states:
        raise SystemExit("cidades somam mais que os estados")
    if total_region != total_states:
        print("[aviso] %d de populacao em estado sem cidade em nacao sem cidade"
              % (total_states - total_region))

    bad_nation = []
    nation_region = {}
    for city in cities:
        city_id = str(city["id"])
        nation_region[str(city["nation"])] = nation_region.get(str(city["nation"]), 0) \
            + cities_out[city_id]["population_region"]
    for tag, total in sorted(nation_region.items()):
        if total != int(round(target.get(tag, 0.0))):
            bad_nation.append((tag, total, int(round(target.get(tag, 0.0)))))
    print("nacoes cuja soma de cidades != nations.json: %d" % len(bad_nation))
    for row in bad_nation[:10]:
        print("  %-4s soma=%d alvo=%d" % row)

    print("\n== anexacao de estado sem cidade ==")
    print("estados sem cidade no roster        = %d" % len(attach_rule))
    for rule in ("neighbor_largest_population", "nearest_centroid",
                 "no_city_in_nation"):
        count = sum(1 for value in attach_rule.values() if value == rule)
        print("  %-28s = %d" % (rule, count))
    print("multi-cidade (2+ cidades no estado) = %d" % len(multi_city))
    for key in ("primary_left_empty", "city_left_empty", "city_no_home_state"):
        print("  %-28s = %d" % (key, len(specials[key])))
        for city_id, state_id in specials[key]:
            print("      %-12s %-20s %-24s"
                  % (city_id, city_by_id.get(city_id, {}).get("name", "?"),
                     states[state_id]["name"][:24]))

    empty = sorted(city_id for city_id, entry in cities_out.items()
                   if not entry["states"])
    print("cidades sem nenhum estado           = %d" % len(empty))
    for city_id in empty[:30]:
        print("  %-12s %s" % (city_id, city_by_id[city_id]["name"]))

    if args.compare:
        compare(provinces, provinces_by_id, areas, nature, cities, states,
                states_out, cities_out, cities_of_state, multi_city, attach_rule,
                city_of_state, empty, city_by_id, owner)

    print("\n-> %s  sha256=%s" % (os.path.relpath(OUT, ROOT), out_hash))
    return 0


def compare(provinces, provinces_by_id, areas, nature, cities, states,
            states_out, cities_out, cities_of_state, multi_city, attach_rule,
            city_of_state, empty, city_by_id, owner):
    """Antes (hinterland_2026) x depois (estados) + terra cultivavel."""
    before = load_json(HINTERLAND)["cities"]
    name_of = {str(city["id"]): str(city.get("name") or "") for city in cities}
    nation_of = {str(city["id"]): str(city["nation"]) for city in cities}
    playable = {str(nation["id"]) for nation in load_json(NATIONS)}

    rows = []
    for city in cities:
        city_id = str(city["id"])
        old = int(before.get(city_id, {}).get("population_region", 0))
        new = int(cities_out[city_id]["population_region"])
        delta = new - old
        pct = (100.0 * delta / old) if old else (float("inf") if new else 0.0)
        rows.append((city_id, name_of[city_id], nation_of[city_id], old, new,
                     delta, pct))

    print("\n== top 20 regioes (depois) x antes ==")
    print("%-12s %-24s %-4s %13s %13s %9s"
          % ("cidade", "nome", "iso", "antes", "depois", "delta%"))
    for row in sorted(rows, key=lambda r: (-r[4], r[0]))[:20]:
        print("%-12s %-24s %-4s %13d %13d %8.1f%%"
              % (row[0], row[1][:24], row[2], row[3], row[4], row[6]))

    changed = [r for r in rows if r[3] > 0 and abs(r[6]) > 20.0]
    changed_big = [r for r in changed if abs(r[5]) > 100000]
    print("\ncidades com regiao mudando >20%%       = %d de %d"
          % (len(changed), len(rows)))
    print("  das quais com |delta| > 100 mil      = %d" % len(changed_big))

    print("\n== por nacao (IND BRA USA CHN NGA RUS IDN PAK FRA DEU) ==")
    print("%-4s %6s %6s %8s %10s %12s %12s %8s"
          % ("iso", "est", "cid", "sem_cid", "multi", "antes", "depois", ">20%"))
    for tag in MAJOR_NATIONS:
        nation_states = [sid for sid, s in states.items() if s["nation"] == tag]
        nation_cities = [str(c["id"]) for c in cities if str(c["nation"]) == tag]
        old = sum(int(before.get(cid, {}).get("population_region", 0))
                  for cid in nation_cities)
        new = sum(int(cities_out[cid]["population_region"]) for cid in nation_cities)
        moved = sum(1 for r in rows if r[2] == tag and r[3] > 0 and abs(r[6]) > 20.0)
        print("%-4s %6d %6d %8d %10d %12d %12d %8d"
              % (tag, len(nation_states), len(nation_cities),
                 sum(1 for sid in nation_states if sid not in cities_of_state),
                 sum(1 for sid in multi_city if states[sid]["nation"] == tag),
                 old, new, moved))

    print("\n== maiores variacoes absolutas ==")
    for row in sorted(rows, key=lambda r: -abs(r[5]))[:15]:
        print("%-12s %-24s %-4s %13d %13d %+13d %8.1f%%"
              % (row[0], row[1][:24], row[2], row[3], row[4], row[5], row[6]))

    # --- terra cultivavel ---------------------------------------------------
    catchment, share = geography_replica.assign_provinces(
        cities, provinces, playable)
    cultivable = {}
    missing_nature = 0
    for pid, entry in provinces_by_id.items():
        item = nature.get(str(pid))
        if item is None:
            missing_nature += 1
            cultivable[pid] = 0.0
        else:
            cultivable[pid] = float(areas.get(str(pid), 0.0)) * \
                float(item.get("cultivable") or 0.0)

    land_rows = []
    old_land = {}
    new_land = {}
    for city in cities:
        city_id = str(city["id"])
        old = 0.0
        for pid in catchment.get(city_id, []):
            fraction = float(share.get(city_id, {}).get(pid, 1.0))
            old += cultivable.get(pid, 0.0) * fraction
        new = sum(cultivable.get(pid, 0.0)
                  for pid, city in owner.items() if city == city_id)
        old_land[city_id] = old
        new_land[city_id] = new
        delta = new - old
        pct = (100.0 * delta / old) if old > 1.0 else 0.0
        land_rows.append((city_id, name_of[city_id], nation_of[city_id],
                          old, new, delta, pct))

    print("\n== terra cultivavel por cidade: maiores variacoes absolutas ==")
    print("provincias sem aptidao (cultivable=0) = %d" % missing_nature)
    print("%-12s %-24s %-4s %12s %12s %12s %8s"
          % ("cidade", "nome", "iso", "antes_km2", "depois_km2", "delta", "%"))
    for row in sorted(land_rows, key=lambda r: -abs(r[5]))[:15]:
        print("%-12s %-24s %-4s %12.0f %12.0f %+12.0f %7.1f%%"
              % (row[0], row[1][:24], row[2], row[3], row[4], row[5], row[6]))

    print("\n== terra cultivavel por nacao ==")
    for tag in MAJOR_NATIONS:
        old = sum(old_land[cid] for cid in old_land if nation_of[cid] == tag)
        new = sum(new_land[cid] for cid in new_land if nation_of[cid] == tag)
        print("%-4s antes=%12.0f depois=%12.0f delta=%+12.0f (%+.1f%%)"
              % (tag, old, new, new - old, 100.0 * (new - old) / max(old, 1.0)))

    print("\n== estados com 2+ cidades (20 maiores por populacao) ==")
    for state_id in sorted(multi_city,
                           key=lambda sid: -states_out[sid]["population_2026"])[:20]:
        names = ["%s" % name_of[cid] for cid in multi_city[state_id]]
        print("%-10s %-26s %-4s %12d  %s"
              % (state_id, states[state_id]["name"][:26],
                 states[state_id]["nation"],
                 states_out[state_id]["population_2026"],
                 " | ".join(names)))
    print("\n== estados sem cidade (lista completa) ==")
    for state_id in sorted(attach_rule):
        print("%-10s %-28s %-4s %-28s -> %s"
              % (state_id, states[state_id]["name"][:28],
                 states[state_id]["nation"], attach_rule[state_id],
                 name_of.get(city_of_state.get(state_id), "?")))
    if empty:
        print("\n== cidades sem nenhum estado ==")
        for city_id in empty:
            print("%-12s %-24s %s" % (city_id, name_of[city_id], nation_of[city_id]))


if __name__ == "__main__":
    sys.exit(main())
