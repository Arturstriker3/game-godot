"""Gera os assets das camadas de mapa:

1. assets/ui/map_mode_political.png e map_mode_resources.png — glifos brancos
   (o HUD aplica a cor por modulate).
2. assets/icons/places/resource_dot.png e resource_wheat.png — marcadores.
3. map/map_data/atlas/resource_points.json — AMOSTRA REPRESENTATIVA dos pontos de
   recursos (minerais e renováveis). Cada marcador carrega o agregado da região
   que representa (somatório de reserva / aptidão média ponderada por área + área),
   para a ficha de depósito/área continuar coerente mesmo com menos marcadores.

Orçamento (data/definitions/natural/marker_budget.json): total mundial por
recurso. Divisão entre países pela participação real — produção FAOSTAT para
culturas (crop_production_shares.json), reserve_calibration.json para minerais —
com escala amortecida (marcadores ∝ √participação), teto de 12% do orçamento por
país e piso 1 para país com participação relevante (≥0,5%) ou aptidão ≥0,6 /
depósito relevante. Dentro do país, escolhem-se os territórios de maior aptidão
(culturas) ou maiores depósitos (minerais), com distância mínima real ≥ ~180 km
entre marcadores do MESMO recurso (Poisson-disc/greedy) e nunca no mar
(nav_mask), mantendo o anel anti-sobreposição entre recursos do mesmo território.

PISO POR NAÇÃO (minerais): toda nação com ≥1 jazida tem ≥1 marcador. A nação
escolhe UM recurso antes da repartição (aquele onde tem a maior participação
mundial) e esse piso CONSOME a cota do recurso — o total mundial por recurso
continua o de projeto. Depois da repartição, uma rede de segurança
(`ensure_nation_marker_floors`) cobre a nação que ainda ficou sem marcador (a
distância mínima de 120 km pode barrar a cota); esse excedente é declarado em
`marker_budget.json` (`floor_extra`), porque inflar a cota realimenta o
`apportion` (que preenche até o orçamento) e o total subiria de novo.

Fontes: tools/mode_icons/ (ícones, créditos no CREDITS.txt) e
data/definitions/natural/ (depósitos e agricultura do Natural Earth).

Uso: python tools/build_resource_map.py
"""

import json
import math
import os

import numpy as np
from PIL import Image, ImageDraw
from scipy.spatial import cKDTree

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ICONS = os.path.join(ROOT, "tools", "mode_icons")
NATURAL = os.path.join(ROOT, "data", "definitions", "natural")
PLACES = os.path.join(ROOT, "assets", "icons", "places")

MINERALS = ["coal", "copper", "gas", "iron", "oil",
            "bauxite", "lead", "gold", "phosphate", "tungsten",
            "rare_earths", "sulfur", "diamond", "uranium"]
RENEWABLE_KEYS = ["wheat", "rice", "maize", "potato", "soy", "cotton", "wood"]

## Diâmetro do marcador em px de mapa. MARKER_SIZE = 1.0 unidade de mundo e
## 1 unidade de mundo = 5 px de mapa (ver map_mode_layer._marker_world); usa-se
## D = 7 px para cobrir também o anel de seleção (2.8 × MARKER_SIZE).
MARKER_DIAMETER_PX = 7.0
## Espaçamento mínimo entre marcadores de um território (>= 1.6 × D). Os
## marcadores de um território são distribuídos em anel hexagonal ao redor do
## centróide; o anel garante distância >= MARKER_SPACING_PX entre quaisquer dois.
MARKER_SPACING_PX = 1.6 * MARKER_DIAMETER_PX
## Raio de busca (em unidades de MARKER_SPACING_PX) de células de terra ao
## redor da âncora para posicionar os marcadores de um território. Grande o
## bastante para territórios costeiros (metade do anel cai no mar), pequeno o
## bastante para não invadir o território vizinho.
MARKER_RING_MAX = 6
## Códigos de status no resource_points.json (legenda do depósito).
STATUS_CODES = {"Producer": 0, "Past Producer": 1, "Prospect": 2, "Occurrence": 3, "Plant": 3}
WHEAT_MIN = 0.40
## Limiar de cobertura florestal para o marcador de madeira (mais baixo que o
## de culturas — floresta é mais difundida que uma cultura específica).
FOREST_MIN = 0.30
## Distância máxima (em células do nav_mask) para encostar um ponto de trigo
## cujo centróide caiu em água. 40 células ~ 80 px de mapa (~15 km); além
## disso é ilha remota e o ponto é descartado.
WHEAT_SNAP_MAX_NAV_PX = 40
EDGE = (23, 20, 16, 235)

## Regra de água: nenhum recurso terrestre no mar. Petróleo/gás podem estar
## offshore (pré-sal, Mar do Norte), mas só até 20 células do nav_mask
## (~320 km da costa) — o suficiente para os campos reais, sem pontos no meio
## do oceano. A extração futura continua sendo uma regra de jogo (só na costa).
COASTAL_RESOURCES = ("oil", "gas")
COASTAL_RADIUS_CELLS = 20
MAP_W, MAP_H = 5000.0, 2500.0

## Distância mínima real entre marcadores do MESMO recurso (~120 km).
## 120 km (em vez de 180) para caber o cluster de gás do Golfo Pérsico
## (Khuff/Awali-BHR e North Field-QAT distam ~135 km) sem fundir campos de
## países vizinhos. Continua legível: ~2,4× o diâmetro do marcador (7 px).
MIN_SAME_RESOURCE_DIST_KM = 120.0
## Teto por país sobre o orçamento do recurso (12%).
CEILING_FRACTION = 0.12
## Participação mínima para piso 1 "relevante" (0,5%).
RELEVANT_SHARE = 0.005
## Aptidão mínima para piso 1 de país numa cultura (0,6).
RELEVANT_SUITABILITY = 0.60


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


## Terra do atlas (`world_sdf.png`, ~4,9 km/px): a mesma base do mapa visual e
## mais fina que a célula de ~16 km do nav_mask. Carregada sob demanda.
_WORLD_LAND = None


def world_land():
    global _WORLD_LAND
    if _WORLD_LAND is None:
        path = os.path.join(ROOT, "map", "map_data", "atlas", "world_sdf.png")
        if not os.path.exists(path):
            _WORLD_LAND = False
        else:
            _WORLD_LAND = np.asarray(
                Image.open(path).convert("RGB"))[:, :, 0] > 128
    return _WORLD_LAND


def is_atlas_land(px: float, py: float) -> bool:
    """True se o ponto cai em TERRA no atlas (resolução de ~4,9 km)."""
    land = world_land()
    if land is False:
        return False
    height, width = land.shape
    x = int(px / MAP_W * width)
    y = int(py / MAP_H * height)
    if not (0 <= x < width and 0 <= y < height):
        return False
    return bool(land[y, x])


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


def nearest_territory(nature: dict, px: float, py: float):
    """Id do território cujo centróide está mais próximo de (px, py)."""
    best = None
    best_dist = float("inf")
    for territory, entry in nature.items():
        centroid = entry.get("centroid_px", [])
        if len(centroid) < 2:
            continue
        cx, cy = float(centroid[0]), float(centroid[1])
        dist = (cx - px) ** 2 + (cy - py) ** 2
        if dist < best_dist:
            best_dist = dist
            best = territory
    return best


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


def place_ring(nav: Image.Image, ax: float, ay: float, count: int,
               spacing: float) -> list:
    """Posições finais (px de mapa) para `count` marcadores ao redor da âncora
    (ax, ay) de um território. Enumeram-se as células de TERRA mais próximas da
    âncora (em ordem de distância, desempate por ângulo) e atribuem-se os
    marcadores com espaçamento mínimo `spacing` entre si. Nunca no mar; se não
    houver terra suficiente (ilhas minúsculas), os excedentes colapsam na âncora
    — sobreposição inevitável, pois o marcador não pode ficar no mar."""
    ax_cell, ay_cell = nav_cell(nav, ax, ay)
    search = int(MARKER_RING_MAX * spacing / (MAP_W / nav.width)) + 2
    cells = []
    for dy in range(-search, search + 1):
        for dx in range(-search, search + 1):
            nx, ny = ax_cell + dx, ay_cell + dy
            if not (0 <= nx < nav.width and 0 <= ny < nav.height):
                continue
            if nav.getpixel((nx, ny)) >= 128:
                cells.append(((nx + 0.5) * MAP_W / nav.width,
                              (ny + 0.5) * MAP_H / nav.height))
    cells.sort(key=lambda p: ((p[0] - ax) ** 2 + (p[1] - ay) ** 2,
                              math.atan2(p[1] - ay, p[0] - ax)))

    used = []
    result = []
    for _ in range(count):
        chosen = None
        for lx, ly in cells:
            if all(math.hypot(lx - ux, ly - uy) >= spacing for ux, uy in used):
                chosen = (lx, ly)
                break
        if chosen is None:
            chosen = (ax, ay)
        used.append(chosen)
        result.append(chosen)
    return result


## Distância real (km) entre dois pontos do mapa via aproximação equirretangular
## (comprime a longitude por cos(lat) — correto o bastante para o piso de ~180 km).
def km_distance(px1: float, py1: float, px2: float, py2: float) -> float:
    def _lat(py):
        return 90.0 - py / MAP_H * 180.0

    def _lon(px):
        return px / MAP_W * 360.0 - 180.0

    mean_lat = math.radians((_lat(py1) + _lat(py2)) / 2.0)
    dlon_km = (_lon(px2) - _lon(px1)) * 111.32 * math.cos(mean_lat)
    dlat_km = (_lat(py2) - _lat(py1)) * 110.57
    return math.hypot(dlon_km, dlat_km)


def compute_territory_areas(nature: dict, nav: Image.Image) -> dict:
    """Área (km²) por território, estimada atribuindo cada pixel de TERRA do
    nav_mask ao território de centróide mais próximo (célula de Voronoi do sítio
    — coerente com a geração Voronoi legada dos territórios). A área de cada
    célula usa a projeção equirretangular (cos(lat)); é uma estimativa honesta
    para exibição, não dado de simulação."""
    tids, centroids = [], []
    for tid, entry in nature.items():
        centroid = entry.get("centroid_px", [])
        if len(centroid) < 2:
            continue
        tids.append(tid)
        centroids.append((float(centroid[0]), float(centroid[1])))

    nav_arr = np.asarray(nav)
    ys, xs = np.where(nav_arr >= 128)
    if ys.size == 0:
        return {tid: 0.0 for tid in tids}

    # Centróides em coordenadas do nav_mask (metade da resolução do mapa).
    nav_centroids = np.array([(px / 2.0, py / 2.0) for px, py in centroids])
    tree = cKDTree(nav_centroids)
    points = np.column_stack((xs, ys)).astype(float)
    _, idx = tree.query(points, k=1)

    lat_deg = 90.0 - (ys / nav.height) * 180.0
    dlon = math.radians(360.0 / nav.width)
    dlat = math.radians(180.0 / nav.height)
    radius_km = 6371.0
    cell_area = (radius_km ** 2) * np.cos(np.deg2rad(lat_deg)) * dlon * dlat
    areas = np.bincount(idx, weights=cell_area, minlength=len(tids))
    return {tid: float(areas[i]) for i, tid in enumerate(tids)}


def apportion(budget: int, weights: dict, floors: set) -> dict:
    """Divide `budget` marcadores entre países proporcionalmente a √participação,
    com teto por país (12%) e piso 1 para os países `floors`. Total <= budget,
    salvo quando o piso por si só excede o orçamento — aí o piso vence (nenhuma
    nação com jazida pode ficar sem marcador) e o build avisa."""
    ceiling = max(1, int(round(budget * CEILING_FRACTION)))
    counts = {code: 0 for code in weights}
    order = sorted(weights, key=lambda c: (-weights[c], c))

    # Pisos primeiro (países relevantes + piso por nação). Eles consomem o
    # orçamento: o restante é distribuído abaixo.
    for code in order:
        if code in floors:
            counts[code] = 1
    total = sum(counts.values())
    if total > budget:
        # O piso é requisito de correção; o orçamento é meta de legibilidade.
        # Nunca cortar daqui: o arquivo de orçamento precisa subir (declarado).
        print("[piso] orçamento %d < %d pisos de país: piso mantido"
              % (budget, total))

    remaining = budget - total
    total_weight = sum(weights.values())
    # Distribui o restante por "maior déficit" (fatia √ - fração já alocada),
    # respeitando o teto por país. Determinístico (ordem estável por peso).
    while remaining > 0:
        best = None
        best_deficit = -1.0
        for code in weights:
            if counts[code] >= ceiling:
                continue
            deficit = (weights[code] / total_weight) - (counts[code] / budget)
            if deficit > best_deficit:
                best_deficit = deficit
                best = code
        if best is None:
            break
        counts[best] += 1
        remaining -= 1
    return counts


def deposit_is_offshore(nav: Image.Image, dep: dict) -> bool:
    """True se o depósito é offshore: petróleo/gás cujo ponto cai em água
    (nav < 128) dentro do raio costeiro. Recursos não costeiros em água são
    imprecisão de coordenada do MRDS e são encostados em terra na posição."""
    if str(dep.get("resource", "")) not in COASTAL_RESOURCES:
        return False
    px = float(dep.get("px", 0.0))
    py = float(dep.get("py", 0.0))
    x, y = nav_cell(nav, px, py)
    if not (0 <= x < nav.width and 0 <= y < nav.height):
        return False
    if nav.getpixel((x, y)) >= 128:
        return False
    return deposit_allowed(nav, dep)


def deposit_position(nav: Image.Image, dep: dict, offshore: bool) -> tuple:
    """Posição do marcador de um depósito: offshore mantém o campo em água;
    os demais ficam em TERRA. O nav_mask (célula de ~16 km) decide a navegação,
    mas o atlas é mais fino (~4,9 km/px): depósito que cai em terra no atlas
    NÃO é deslocado para a célula vizinha — ilhas menores que a célula de
    16 km (Santa Lúcia, Fiji) perdiam o marcador para o continente. Só quando
    nem o atlas tem terra ali é que se encosta na terra mais próxima."""
    px = float(dep.get("px", 0.0))
    py = float(dep.get("py", 0.0))
    if offshore:
        return (px, py)
    x, y = nav_cell(nav, px, py)
    if 0 <= x < nav.width and 0 <= y < nav.height \
            and nav.getpixel((x, y)) < 128:
        if is_atlas_land(px, py):
            return (px, py)
        landed = nearest_land_px(nav, px, py, WHEAT_SNAP_MAX_NAV_PX)
        if landed is not None:
            return landed
    return (px, py)


def build_mineral_markers(nav, nature, deposits, budgets, calibration,
                          area_km2, territory_country, country_territories,
                          floor_resources=None):
    markers = []
    for resource in MINERALS:
        budget = int(budgets.get(resource, 0))
        if budget <= 0:
            continue

        # Um registro por depósito do recurso (país, território, offshore e
        # posição resolvidos). Seeds = maiores depósitos, não territórios.
        rows = []  # (dep, country, territory, offshore, (px, py))
        for dep in deposits:
            if str(dep.get("resource", "")) != resource or dep.get("facility"):
                continue
            tid = str(dep.get("territory", ""))
            if not tid or tid not in nature:
                continue
            # A nação dona vem do pipeline de posse (`owner`); o território
            # natural é a região geográfica e pode ser de outro país quando a
            # nação não tem território na base (Timor-Leste, por exemplo).
            # Sem `owner`, mantém o país do território (comportamento legado).
            code = str(dep.get("owner", "")) or territory_country.get(tid, "")
            if not code:
                continue
            offshore = deposit_is_offshore(nav, dep)
            rows.append((dep, code, tid, offshore,
                         deposit_position(nav, dep, offshore)))
        if not rows:
            continue

        # Reserva por país (pós-calibração; depósito relevante = país relevante).
        country_reserve = {}
        for dep, code, _tid, _off, _pos in rows:
            country_reserve[code] = country_reserve.get(code, 0.0) \
                + float(dep.get("quantity_kt", 0.0))

        participation = build_participation(
            calibration_shares(calibration, resource), country_reserve)

        floors = {code for code in participation
                  if participation[code] >= RELEVANT_SHARE}
        # Piso por nação: toda nação com ≥1 jazida tem ≥1 marcador, no recurso
        # onde ela tem a maior participação mundial (escolhido antes da
        # repartição, em `nation_floor_resources`). O piso consome a cota do
        # recurso — o total mundial por recurso não muda.
        for code, floor_resource in (floor_resources or {}).items():
            if floor_resource == resource and code in participation:
                floors.add(code)
        # Piso 1 para país com depósito relevante: campo real nomeado (curado ou
        # suplemento com fonte pública). Ex.: Khuff/Awali (BHR) tem só ~0,13% da
        # reserva mundial de gás, mas é um campo produtor real que deve aparecer
        # no mapa — senão o piso de participação (0,5%) o apagaria.
        for dep, code, _tid, _off, _pos in rows:
            if str(dep.get("data_quality", "")) in ("curated", "estimated_public_source"):
                floors.add(code)
        counts = apportion(budget, {c: math.sqrt(p) for c, p in participation.items()},
                           floors)

        placed = []  # posições já usadas DESTE recurso (Poisson-disc global)
        # Seeds = maiores depósitos do MUNDO primeiro (não por país), para que o
        # maior campo (ex.: North Field do Catar) não seja barrado por um campo
        # vizinho menor processado antes por ordem de participação. Cada país
        # ainda respeita a cota `counts` e o espaçamento de 180 km.
        seeds_by_country = {}
        candidates = sorted(rows, key=lambda r: -float(r[0].get("quantity_kt", 0.0)))
        for dep, code, tid, offshore, pos in candidates:
            quota = int(counts.get(code, 0))
            mine = seeds_by_country.setdefault(code, [])
            if quota <= 0 or len(mine) >= quota:
                continue
            if all(km_distance(pos[0], pos[1], s["pos"][0], s["pos"][1])
                   >= MIN_SAME_RESOURCE_DIST_KM for s in mine) \
                    and all(km_distance(pos[0], pos[1], sp[0], sp[1])
                            >= MIN_SAME_RESOURCE_DIST_KM for sp in placed):
                mine.append({"pos": pos, "dep": dep, "tid": tid,
                             "offshore": offshore,
                             "members": [(dep, tid, offshore)]})
                placed.append(pos)

        # Demais depósitos agregados ao seed de posição mais próxima (mesmo país).
        for dep, code, tid, offshore, pos in rows:
            seeds = seeds_by_country.get(code, [])
            if not seeds:
                continue
            if any(dep.get("id") == s["dep"].get("id") for s in seeds):
                continue
            best = min(seeds, key=lambda s: (pos[0] - s["pos"][0]) ** 2
                       + (pos[1] - s["pos"][1]) ** 2)
            best["members"].append((dep, tid, offshore))

        for code in sorted(counts, key=lambda c: (-counts[c], c)):
            for seed in seeds_by_country.get(code, []):
                markers.append(make_mineral_marker(resource, seed, area_km2,
                                                   territory_country))
    return markers


def make_mineral_marker(resource, seed, area_km2, territory_country):
    """Marcador de um seed (depósito) + os depósitos do país a ele agregados.
    Reserva = soma; qualidade/profundidade = médias ponderadas pela reserva;
    status = moda. `aggregate` = mais de um depósito fundido; `region_count` =
    número de territórios distintos cobertos pelo aglomerado."""
    members = seed["members"]  # [(dep, territory, offshore), ...]
    reserve = quality_w = depth_w = 0.0
    statuses = []
    tids = []
    for dep, tid, offshore in members:
        qty = float(dep.get("quantity_kt", 0.0))
        reserve += qty
        quality_w += float(dep.get("quality", 0.0)) * qty
        depth_w += float(dep.get("depth_m", 0.0)) * qty
        statuses.append(str(dep.get("status", "Unknown")))
        tids.append(tid)
    # Moda dos status com desempate determinístico (maior contagem, depois
    # ordem alfabética). Antes o desempate vinha da ordem de iteração de um
    # `set` de strings, que muda com o PYTHONHASHSEED — duas rodadas seguidas
    # geravam `resource_points.json` diferentes.
    status = 4
    if statuses:
        counts = {}
        for value in statuses:
            counts[value] = counts.get(value, 0) + 1
        top = sorted(counts.items(), key=lambda item: (-item[1], item[0]))[0][0]
        status = STATUS_CODES.get(top, 4)
    distinct_tids = sorted(set(tids))
    area = sum(area_km2.get(tid, 0.0) for tid in distinct_tids)
    return {
        "kind": "mineral",
        "px": round(seed["pos"][0], 1), "py": round(seed["pos"][1], 1),
        "resource": resource,
        "reserve": round(reserve, 3),
        "quality": round(quality_w / reserve, 3) if reserve else 0.0,
        "depth": int(round(depth_w / reserve)) if reserve else 0,
        "status": status,
        "territory": seed["tid"],
        "count": int(len(members)),
        # O marcador fica NA POSIÇÃO do seed: `offshore` reflete se essa
        # posição cai em água (não se TODOS os membros são offshore — um seed
        # offshore agrega campos onshore vizinhos do mesmo país e mesmo assim
        # permanece no mar).
        "offshore": 1 if seed["offshore"] else 0,
        "region": {
            # Nação dona (posse) quando declarada; o território é a região
            # geográfica que contém o ponto (pode ser de outro país em campos
            # offshore de nações sem território natural, ex.: Timor-Leste).
            "country": str(seed["dep"].get("owner", ""))
                    or territory_country.get(seed["tid"], ""),
            "territories": distinct_tids,
            "area_km2": round(area, 1),
            "aggregate": len(members) > 1,
            "region_count": len(distinct_tids),
        },
    }


def build_renewable_markers(nav, nature, agriculture, budgets, crop_shares,
                            area_km2, territory_country, country_territories):
    markers = []
    for crop in RENEWABLE_KEYS:
        budget = int(budgets.get(crop, 0))
        if budget <= 0:
            continue
        threshold = FOREST_MIN if crop == "wood" else WHEAT_MIN

        terr_value = {}
        for tid in nature:
            value = float(nature[tid].get("forest_cover", 0.0)) if crop == "wood" \
                else float(agriculture.get(tid, {}).get(crop, 0.0))
            if value >= threshold:
                terr_value[tid] = value
        if not terr_value:
            continue

        # "Participação" por país: produção FAOSTAT (culturas) ou floresta
        # (madeira), com "resto" distribuído pela aptidão/cobertura real.
        if crop == "wood":
            country_weight = {}
            for tid, value in terr_value.items():
                code = territory_country.get(tid, "")
                if code:
                    country_weight[code] = country_weight.get(code, 0.0) \
                        + value * area_km2.get(tid, 0.0)
            total = sum(country_weight.values()) or 1.0
            participation = {c: w / total for c, w in country_weight.items()}
        else:
            country_value = {}
            for tid, value in terr_value.items():
                code = territory_country.get(tid, "")
                if code:
                    country_value[code] = country_value.get(code, 0.0) + value
            participation = build_participation(
                crop_shares.get(crop, {}).get("shares", {}), country_value)

        floors = set()
        for code in participation:
            if participation[code] >= RELEVANT_SHARE:
                floors.add(code)
        for tid, value in terr_value.items():
            if value >= RELEVANT_SUITABILITY:
                floors.add(territory_country.get(tid, ""))
        floors.discard("")
        counts = apportion(budget, {c: math.sqrt(p) for c, p in participation.items()},
                           floors)

        placed = []
        for code in sorted(counts, key=lambda c: (-counts[c], c)):
            n = counts[code]
            if n <= 0:
                continue
            candidates = []
            for tid in country_territories.get(code, []):
                if tid not in terr_value:
                    continue
                centroid = nature.get(tid, {}).get("centroid_px", [])
                if len(centroid) < 2:
                    continue
                px, py = float(centroid[0]), float(centroid[1])
                if nav.getpixel(nav_cell(nav, px, py)) < 128:
                    landed = nearest_land_px(nav, px, py, WHEAT_SNAP_MAX_NAV_PX)
                    if landed is None:
                        continue
                    px, py = landed
                candidates.append((tid, terr_value[tid], (px, py)))
            candidates.sort(key=lambda c: -c[1])

            seeds = []
            for tid, _value, pos in candidates:
                if len(seeds) >= n:
                    break
                if all(km_distance(pos[0], pos[1], sp[0], sp[1]) >= MIN_SAME_RESOURCE_DIST_KM
                       for sp in placed):
                    seeds.append((tid, pos))
                    placed.append(pos)

            if not seeds:
                continue
            regions = assign_regions(candidates, seeds)
            for seed_tid, seed_pos in seeds:
                markers.append(make_renewable_marker(
                    crop, seed_tid, seed_pos, regions.get(seed_tid, [seed_tid]),
                    terr_value, nature, agriculture, area_km2, territory_country))
    return markers


def assign_regions(candidates, seeds):
    """Cada candidato (não-seed) vai para o seed de posição mais próxima.
    Retorna {seed_tid: [tids]}, com o seed como primeiro elemento."""
    seed_by_tid = {s[0]: s for s in seeds}
    seed_tids = [s[0] for s in seeds]
    regions = {s[0]: [s[0]] for s in seeds}
    for tid, _w, pos in candidates:
        if tid in seed_by_tid:
            continue
        best = min(seed_tids,
                   key=lambda st: (pos[0] - seed_by_tid[st][1][0]) ** 2
                   + (pos[1] - seed_by_tid[st][1][1]) ** 2)
        regions[best].append(tid)
    return regions


def make_renewable_marker(crop, anchor, pos, region_tids, terr_value,
                          nature, agriculture, area_km2, territory_country):
    total_area = 0.0
    value_w = 0.0
    fert_w = 0.0
    cult_w = 0.0
    for tid in region_tids:
        area = area_km2.get(tid, 0.0)
        total_area += area
        value_w += terr_value[tid] * area
        fert_w += float(nature.get(tid, {}).get("fertility", 0.0)) * area
        cult_w += float(nature.get(tid, {}).get("cultivable", 0.0)) * area
    # Territórios sem área (ilhas minúsculas sem célula de terra no nav_mask):
    # usa a aptidão/fertilidade diretas, sem ponderar por área (=0).
    if total_area > 0.0:
        value = value_w / total_area
        fertility = fert_w / total_area
        cultivable = cult_w / total_area
    else:
        n = float(len(region_tids)) or 1.0
        value = sum(terr_value[tid] for tid in region_tids) / n
        fertility = sum(
            float(nature.get(tid, {}).get("fertility", 0.0)) for tid in region_tids) / n
        cultivable = sum(
            float(nature.get(tid, {}).get("cultivable", 0.0)) for tid in region_tids) / n
    return {
        "kind": "renewable",
        "px": round(pos[0], 1), "py": round(pos[1], 1),
        "resource": crop,
        "value": round(value, 3),
        "territory": anchor,
        "region": {
            "country": territory_country.get(anchor, ""),
            "territories": sorted(set(region_tids)),
            "area_km2": round(total_area, 1),
            "fertility": round(fertility, 3),
            "cultivable": round(cultivable, 3),
        },
    }


def calibration_shares(calibration: list, resource: str) -> dict:
    """shares (nomeado + resto) do recurso na reserve_calibration.json."""
    for spec in calibration:
        if spec.get("resource") == resource:
            shares = spec.get("shares", {})
            if shares:
                return {str(k): float(v) for k, v in shares.items()}
    return {}


def build_participation(named_shares: dict, country_totals: dict) -> dict:
    """Participação (0..1) por país: países nomeados usam a fração da fonte;
    o 'resto' é distribuído proporcionalmente ao total real (reserva/aptidão)
    dos países não nomeados. País nomeado sem território/depósito é ignorado
    (a fração fica fora — documentado). Sem fonte (enxofre/diamante, total
    'unknown'), usa a proporção direta dos totais reais."""
    named = {code: frac for code, frac in named_shares.items() if code != "resto"}
    resto_frac = float(named_shares.get("resto", 0.0))
    if not named and resto_frac <= 0.0:
        total = sum(country_totals.values())
        if total > 0.0:
            return {c: v / total for c, v in country_totals.items()}
        return {}
    participation = dict(named)
    non_named = [c for c in country_totals if c not in named]
    if non_named and resto_frac > 0.0:
        total_non_named = sum(country_totals[c] for c in non_named)
        if total_non_named > 0.0:
            for code in non_named:
                participation[code] = resto_frac * country_totals[code] / total_non_named
        else:
            for code in non_named:
                participation[code] = 0.0
    return participation


def nation_participation(nature, deposits, calibration, territory_country) -> dict:
    """Participação (0..1) de cada país na reserva de cada recurso mineral.

    Mesma conta de `build_mineral_markers` (reserva do país / participação da
    fonte), mas feita uma vez para todos os recursos — usada só pelo piso de
    marcadores por nação."""
    totals = {}
    for dep in deposits:
        if dep.get("facility"):
            continue
        resource = str(dep.get("resource", ""))
        if resource not in MINERALS:
            continue
        tid = str(dep.get("territory", ""))
        if not tid or tid not in nature:
            continue
        code = str(dep.get("owner", "")) or territory_country.get(tid, "")
        if not code:
            continue
        bucket = totals.setdefault(resource, {})
        bucket[code] = bucket.get(code, 0.0) + float(dep.get("quantity_kt", 0.0))
    return {resource: build_participation(calibration_shares(calibration, resource),
                                          country_totals)
            for resource, country_totals in totals.items()}


def nation_floor_resources(nature, deposits, calibration, territory_country) -> dict:
    """Recurso do piso de cada nação com ≥1 jazida: `{ISO3: recurso}`.

    O piso é do país, não do recurso: a nação entra no mapa UMA vez, no recurso
    onde tem a maior participação mundial (empate: maior reserva, depois o
    recurso e o id do depósito — determinístico). Essa escolha é feita antes da
    repartição, para o piso consumir a cota do recurso (o total mundial por
    recurso continua o de projeto)."""
    participation = nation_participation(nature, deposits, calibration,
                                         territory_country)
    best = {}
    for dep in deposits:
        if dep.get("facility"):
            continue
        resource = str(dep.get("resource", ""))
        if resource not in MINERALS:
            continue
        tid = str(dep.get("territory", ""))
        if not tid or tid not in nature:
            continue
        code = str(dep.get("owner", "")) or territory_country.get(tid, "")
        if not code:
            continue
        share = float(participation.get(resource, {}).get(code, 0.0))
        row = (-share, -float(dep.get("quantity_kt", 0.0)), resource,
               str(dep.get("id", "")))
        if code not in best or row < best[code]:
            best[code] = row
    return {code: row[2] for code, row in best.items()}


def ensure_nation_marker_floors(nav, nature, deposits, calibration, area_km2,
                                territory_country, markers):
    """Piso: toda nação com ≥1 jazida tem ≥1 marcador mineral no mapa.

    A repartição por participação (√participação, teto de 12%, piso só para
    país "relevante") dava cota 0 a nações com reserva pequena mas real — o
    cobre de Kabanga (RWA, 189,9 kt) aparece na economia de RWA-KIG e não tinha
    marcador nenhum. Aqui cada nação ainda sem marcador recebe UM, do recurso
    onde ela tem a MAIOR participação mundial (empate: maior reserva, depois o
    recurso e o id do depósito — ordem determinística), na posição do depósito
    (mesma regra `deposit_position`). O marcador extra respeita a distância
    mínima entre marcadores do MESMO recurso quando há um depósito alternativo
    que a respeite; se não houver, o piso vale mesmo assim (a nação não pode
    ficar invisível) e o caso é impresso para auditoria.

    Devolve a lista de marcadores adicionados (mesmo formato dos demais)."""
    covered = {str(m["region"].get("country", "")) for m in markers}
    rows = []  # (dep, code, tid)
    for dep in deposits:
        if dep.get("facility"):
            continue
        tid = str(dep.get("territory", ""))
        if not tid or tid not in nature:
            continue
        code = str(dep.get("owner", "")) or territory_country.get(tid, "")
        if not code or code in covered:
            continue
        rows.append((dep, code, tid))
    if not rows:
        return []

    participation = nation_participation(nature, deposits, calibration,
                                         territory_country)
    candidates = {}
    for dep, code, tid in rows:
        resource = str(dep.get("resource", ""))
        share = float(participation.get(resource, {}).get(code, 0.0))
        candidates.setdefault(code, []).append(
            (-share, -float(dep.get("quantity_kt", 0.0)), resource,
             str(dep.get("id", "")), dep, tid))
    for code in candidates:
        candidates[code].sort(key=lambda row: row[:4])

    placed = {}
    for marker in markers:
        placed.setdefault(str(marker.get("resource", "")), []).append(
            (float(marker["px"]), float(marker["py"])))

    added = []
    for code in sorted(candidates):
        chosen = None
        blocked = None
        for _share, _qty, resource, _dep_id, dep, tid in candidates[code]:
            offshore = deposit_is_offshore(nav, dep)
            pos = deposit_position(nav, dep, offshore)
            free = all(km_distance(pos[0], pos[1], other[0], other[1])
                       >= MIN_SAME_RESOURCE_DIST_KM
                       for other in placed.get(resource, []))
            if free:
                chosen = (resource, dep, tid, offshore, pos)
                break
            if blocked is None:
                blocked = (resource, dep, tid, offshore, pos)
        if chosen is None:
            # Piso garantido: nenhuma jazida da nação respeita a distância
            # mínima do recurso — o marcador entra no maior depósito e fica
            # registrado no log (a alternativa é a nação sumir do mapa).
            chosen = blocked
            print("[piso] %s: marcador de %s a menos de %.0f km de outro do "
                  "mesmo recurso (piso mantido)" % (code, chosen[0],
                                                   MIN_SAME_RESOURCE_DIST_KM))
        resource, dep, tid, offshore, pos = chosen
        placed.setdefault(resource, []).append(pos)
        seed = {"pos": pos, "dep": dep, "tid": tid, "offshore": offshore,
                "members": [(dep, tid, offshore)]}
        added.append(make_mineral_marker(resource, seed, area_km2,
                                         territory_country))
    if added:
        print("[piso] %d marcador(es) para nações que estavam sem marcador: %s"
              % (len(added), ", ".join(sorted(
                  {str(m["region"]["country"]) for m in added}))))
    return added


def apply_ring_anti_overlap(nav, nature, markers):

    """Anel anti-sobreposição entre recursos DIFERENTES do MESMO território
    (regra pré-existente). Reagrupa os marcadores onshore ancorados no mesmo
    território ao redor do centróide; offshore mantém a posição de campo.

    Com os minerais posicionados por DEPÓSITO (não por centróide), o anel não
    pode mais empurrar um marcador para perto de outro do MESMO recurso — o
    deslocamento é rejeitado quando violaria os ~180 km de Poisson-disc."""
    by_terr = {}
    for marker in markers:
        if marker.get("offshore"):
            continue
        by_terr.setdefault(marker["territory"], []).append(marker)
    moved = 0
    for tid, group in by_terr.items():
        if len(group) <= 1:
            continue
        centroid = nature.get(tid, {}).get("centroid_px", [])
        if len(centroid) < 2:
            continue
        ax, ay = float(centroid[0]), float(centroid[1])
        if nav.getpixel(nav_cell(nav, ax, ay)) < 128:
            landed = nearest_land_px(nav, ax, ay, WHEAT_SNAP_MAX_NAV_PX)
            if landed is None:
                continue
            ax, ay = landed
        positions = place_ring(nav, ax, ay, len(group), MARKER_SPACING_PX)
        for marker, (px, py) in zip(group, positions):
            if _violates_min_distance(marker, px, py, markers):
                continue
            if abs(px - float(marker["px"])) < 0.05 \
                    and abs(py - float(marker["py"])) < 0.05:
                continue
            moved += 1
            marker["px"] = round(px, 1)
            marker["py"] = round(py, 1)
    if moved:
        print("[anel] %d marcador(es) reposicionado(s) no anel do território"
              % moved)


def _violates_min_distance(marker: dict, px: float, py: float,
                           markers: list) -> bool:
    """True se (px, py) fica a menos de MIN_SAME_RESOURCE_DIST_KM de outro
    marcador do MESMO recurso (posições atuais do conjunto)."""
    for other in markers:
        if other is marker or other.get("resource") != marker.get("resource"):
            continue
        if km_distance(px, py, other["px"], other["py"]) < MIN_SAME_RESOURCE_DIST_KM:
            return True
    return False


def build_points() -> None:
    nav = load_nav()
    deposits = read_json("deposits.json")["deposits"]
    nature = read_json("nature.json")["territories"]
    agriculture = read_json("agriculture.json")["territories"]
    budgets = read_json("marker_budget.json")["budgets"]
    crop_shares = read_json("crop_production_shares.json")["crops"]
    calibration = read_json("reserve_calibration.json")["resources"]

    territory_country = {tid: str(entry.get("adm0_a3", ""))
                         for tid, entry in nature.items()}
    country_territories = {}
    for tid, code in territory_country.items():
        if code:
            country_territories.setdefault(code, []).append(tid)

    area_km2 = compute_territory_areas(nature, nav)

    mineral_markers = build_mineral_markers(
        nav, nature, deposits, budgets, calibration, area_km2,
        territory_country, country_territories,
        floor_resources=nation_floor_resources(
            nature, deposits, calibration, territory_country))
    # Rede de segurança do piso: nação com jazida que ainda ficou sem marcador
    # (as cotas do recurso já incluem o piso; aqui só entram as que a distância
    # mínima do recurso barrou).
    mineral_markers += ensure_nation_marker_floors(
        nav, nature, deposits, calibration, area_km2, territory_country,
        mineral_markers)
    renewable_markers = build_renewable_markers(
        nav, nature, agriculture, budgets, crop_shares, area_km2,
        territory_country, country_territories)

    apply_ring_anti_overlap(nav, nature, mineral_markers + renewable_markers)

    minerals = []
    renewables = []
    for marker in mineral_markers:
        minerals.append([marker["px"], marker["py"],
                         MINERALS.index(marker["resource"]),
                         marker["reserve"], marker["quality"], marker["depth"],
                         marker["status"], marker["territory"], marker["count"],
                         marker["offshore"], marker["region"]])
    for marker in renewable_markers:
        renewables.append([marker["px"], marker["py"],
                           RENEWABLE_KEYS.index(marker["resource"]),
                           marker["value"], marker["territory"], marker["region"]])

    out = os.path.join(ROOT, "map", "map_data", "atlas", "resource_points.json")
    with open(out, "w", encoding="utf-8") as handle:
        json.dump({"minerals": minerals, "renewables": renewables},
                  handle, separators=(",", ":"))
    print("resource_points.json", len(minerals), "marcadores minerais",
          "| renováveis", len(renewables))


white_icon("political.png", "map_mode_political.png")
white_icon("resources.png", "map_mode_resources.png")
marker(64, "dot").save(os.path.join(PLACES, "resource_dot.png"))
marker(64, "diamond").save(os.path.join(PLACES, "resource_wheat.png"))
star().save(os.path.join(PLACES, "star.png"))
hover_ring().save(os.path.join(PLACES, "deposit_hover.png"))

build_points()
