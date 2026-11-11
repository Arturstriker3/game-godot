"""Gera map/map_data/nav_mask.png a partir das provincias do mapa.

Cada celula cobre 2x2 px do provinces.png e vale:
  255 = celula toda terra
    0 = celula toda agua
  128 = celula de costa (mistura terra/agua)

O trade_convoys.gd usa essa mascara para caminhoes andarem so em terra e
navios so em agua. Rode novamente sempre que o mapa mudar.

ESTREITOS: cada celula cobre ~16 km, entao estreitos reais de 1-15 km somem na
mascara (Gibraltar tem 14 km; o Oresund, 4 km). Sem correcao o Mediterraneo, o
mar Negro e o Baltico ficam em componentes de agua separados do Atlantico e o
comboio nao acha rota (ex.: Marseille->Bristol). A tabela STRAITS abaixo lista,
por estreito, a caixa geografica e uma semente de cada mar; o gerador procura o
MENOR corte de terra entre os dois mares dentro da caixa (0-1 BFS: agua custa 0,
terra/costa custa 1) e abre um canal de 3 celulas de largura no eixo
perpendicular. So celulas de terra/costa viram agua - agua nunca e "secada" - e
o resultado e verificado no fim (os dois mares tem de sair conectados).
"""
import io
from collections import deque
from pathlib import Path

import numpy as np
from PIL import Image
from scipy import ndimage

ROOT = Path(__file__).resolve().parents[1]
PROVINCES_TXT = ROOT / "data" / "definitions" / "provinces.txt"
PROVINCES_PNG = ROOT / "map" / "map_data" / "provinces.png"
OUTPUT = ROOT / "map" / "map_data" / "nav_mask.png"
BLOCK = 2

## Mesmo limiar do `MASK_WATER_MAX` do trade_convoys.gd.
WATER_MAX = 2
## Largura do canal: 3 celulas (1 de cada lado da linha de menor corte).
STRAIT_HALF_WIDTH = 1
## Teto de seguranca: se um mapa novo exigir abrir mais que isso num estreito,
## e sinal de que a caixa pegou o estreito errado - falhe em vez de inundar.
MAX_CARVED_CELLS = 80

## nome, caixa (lon0, lon1, lat0, lat1), mar A (lon, lat), mar B (lon, lat)
STRAITS = [
    ("gibraltar", (-6.6, -4.6, 35.4, 36.4), (-7.0, 35.7), (-3.0, 36.6)),
    ("dardanelos", (25.6, 28.4, 39.9, 41.0), (25.0, 39.4), (28.5, 40.7)),
    ("bosphoro", (28.6, 29.6, 40.8, 41.5), (28.5, 40.7), (29.5, 41.6)),
    ("oresund", (12.3, 13.1, 55.3, 56.2), (10.6, 56.6), (13.0, 55.0)),
    ("suez", (32.0, 33.0, 29.5, 31.4), (32.0, 31.6), (32.6, 29.8)),
    ("panama", (-80.2, -79.2, 8.6, 9.6), (-80.4, 9.6), (-79.0, 8.5)),
    ("kiel", (8.2, 11.4, 53.6, 55.4), (8.4, 54.6), (10.9, 54.6)),
]


def load_water_colors() -> set:
    water = set()
    with io.open(PROVINCES_TXT, encoding="utf-8") as handle:
        for line in handle:
            parts = line.strip().split(";")
            if len(parts) < 8 or parts[1] not in ("ocean", "lake"):
                continue
            water.add((int(parts[2]), int(parts[3]), int(parts[4])))
    return water


def pixel_of(lon: float, lat: float, width: int, height: int) -> tuple:
    return (int(round((lon + 180.0) / 360.0 * width - 0.5)),
            int(round((90.0 - lat) / 180.0 * height - 0.5)))


def water_labels(mask: np.ndarray) -> np.ndarray:
    """Componentes de agua com 4-vizinhos (a mesma conectividade do A* fino)."""
    labels, _count = ndimage.label(
        mask <= WATER_MAX, structure=[[0, 1, 0], [1, 1, 1], [0, 1, 0]])
    return labels


def carve_strait(mask: np.ndarray, name: str, box: tuple, sea_a: tuple,
                 sea_b: tuple) -> int:
    """Abre o menor canal ligando os dois mares. Devolve celulas abertas."""
    height, width = mask.shape
    labels = water_labels(mask)
    ax, ay = pixel_of(*sea_a, width, height)
    bx, by = pixel_of(*sea_b, width, height)
    comp_a = int(labels[ay, ax])
    comp_b = int(labels[by, bx])
    if comp_a == 0 or comp_b == 0:
        print("  %-11s semente em terra - ignorado" % name)
        return 0
    if comp_a == comp_b:
        print("  %-11s ja conectado" % name)
        return 0

    x0, y0 = pixel_of(box[0], box[3], width, height)
    x1, y1 = pixel_of(box[1], box[2], width, height)
    box_w = x1 - x0 + 1
    box_h = y1 - y0 + 1
    behind = np.full((box_h, box_w), -1, dtype=np.int32)
    dist = np.full((box_h, box_w), np.iinfo(np.int32).max, dtype=np.int32)
    window = labels[y0:y1 + 1, x0:x1 + 1]
    queue = deque()
    for yy, xx in zip(*np.nonzero(window == comp_a)):
        dist[yy, xx] = 0
        queue.append((yy, xx))
    goal = None
    while queue:
        y, x = queue.popleft()
        if window[y, x] == comp_b:
            goal = (y, x)
            break
        for dy, dx in ((1, 0), (0, 1), (-1, 0), (0, -1)):
            ny, nx = y + dy, x + dx
            if not (0 <= ny < box_h and 0 <= nx < box_w):
                continue
            cost = 0 if window[ny, nx] else 1
            best = dist[y, x] + cost
            if best < dist[ny, nx]:
                dist[ny, nx] = best
                behind[ny, nx] = y * box_w + x
                if cost == 0:
                    queue.appendleft((ny, nx))
                else:
                    queue.append((ny, nx))
    if goal is None:
        raise SystemExit("estreito %s: nenhum caminho entre os dois mares" % name)

    path = []
    cy, cx = goal
    while True:
        path.append((cx + x0, cy + y0))
        step = behind[cy, cx]
        if step < 0:
            break
        cy, cx = step // box_w, step % box_w
    path.reverse()
    land = [(px, py) for px, py in path if mask[py, px] > WATER_MAX]
    if not land:
        raise SystemExit("estreito %s: caminho sem terra (semente no mesmo mar?)"
                         % name)
    # Eixo do corte decide a perpendicular do canal.
    span_x = land[-1][0] - land[0][0]
    span_y = land[-1][1] - land[0][1]
    perp = (0, 1) if abs(span_x) >= abs(span_y) else (1, 0)
    carved = set()
    for px, py in land:
        for side in range(-STRAIT_HALF_WIDTH, STRAIT_HALF_WIDTH + 1):
            nx = px + perp[0] * side
            ny = py + perp[1] * side
            if not (x0 <= nx <= x1 and y0 <= ny <= y1):
                continue
            if mask[ny, nx] > WATER_MAX:
                mask[ny, nx] = 0
                carved.add((nx, ny))
    if len(carved) > MAX_CARVED_CELLS:
        raise SystemExit("estreito %s: %d celulas abertas (teto %d)"
                         % (name, len(carved), MAX_CARVED_CELLS))

    after = water_labels(mask)
    if int(after[ay, ax]) != int(after[by, bx]):
        raise SystemExit("estreito %s: canal aberto mas os mares seguem "
                         "separados" % name)
    xs = [c[0] for c in carved]
    ys = [c[1] for c in carved]
    print("  %-11s aberto: %d celulas, custo=%d terra, x%d..%d y%d..%d"
          % (name, len(carved), dist[goal], min(xs), max(xs), min(ys), max(ys)))
    return len(carved)


def carve_straits(mask: np.ndarray) -> int:
    total = 0
    for name, box, sea_a, sea_b in STRAITS:
        total += carve_strait(mask, name, box, sea_a, sea_b)
    return total


def build_mask() -> np.ndarray:
    water = load_water_colors()
    pixels = np.asarray(Image.open(PROVINCES_PNG).convert("RGB"))

    flat = pixels.reshape(-1, 3)
    is_water = np.array([tuple(int(v) for v in px) in water for px in flat])
    land = ~is_water.reshape(pixels.shape[:2])

    height, width = land.shape
    blocks = land.reshape(height // BLOCK, BLOCK, width // BLOCK, BLOCK)
    fraction = blocks.mean(axis=(1, 3))
    return np.where(fraction >= 0.999, 255, np.where(fraction <= 0.001, 0, 128)).astype(np.uint8)


def main() -> None:
    if PROVINCES_PNG.exists():
        mask = build_mask()
    elif OUTPUT.exists():
        # provinces.png saiu do repositorio (o raster do mapa agora vive no
        # atlas). A mascara publicada e a base e recebe so a correcao dos
        # estreitos - sem regenerar o mapa inteiro.
        print("provinces.png ausente: aplicando estreitos sobre a mascara atual")
        mask = np.asarray(Image.open(OUTPUT).convert("L")).copy()
    else:
        raise SystemExit("sem provinces.png e sem nav_mask.png: nada a gerar")

    opened = carve_straits(mask)
    if opened:
        print("estreitos: %d celulas de terra viraram agua" % opened)

    Image.fromarray(mask, mode="L").save(OUTPUT)
    print("gerado %s (%dx%d)" % (OUTPUT, mask.shape[1], mask.shape[0]))


if __name__ == "__main__":
    main()
