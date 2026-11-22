#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Backend OpenAI para a grade de 16 ícones de recurso (opengs-icons).

Gera UMA folha 4×4 com 16 ícones de inventário de jogo militar moderno /
grande estratégia sério (tom Modern Warfare / Arma). O estilo atual (v3) é
SEMI-REALISTA: materiais e proporções reais, texturas detalhadas, paleta
dessaturada e suja (cinzas, oliva, ferrugem, ocre), luz dura de cima-esquerda
com sombra de contato e contorno escuro FINO só para legibilidade. A referência
serve apenas de enquadramento (objeto isolado, vista 3/4, margem), não de traço.
Recorta as 16 células, apara, remove o fundo e salva 128 px e 64 px em
``assets/icons/illustrated_source/gen_api/<id>/<id>_v<n>.png`` (+ ``_64.png``
e metadados em ``.json``).

A referência padrão é ``ref_sheet_military_pack.png``. ``refs`` / ``--refs``
aceita uma ou mais imagens (caminho absoluto ou nome dentro de ref_style).
``variant`` / ``--tag`` (ex.: ``v3``) fixa o número da variação para não
sobrescrever um lote anterior.

Ferramenta MCP: ``generate_grid(ids, descriptions, quality, dry_run, refs, variant)``.

Uso direto (CLI, mesma máquina):

    python tools/icon_gen/openai_grid.py --dry-run --quality medium
    python tools/icon_gen/openai_grid.py --quality medium          # chamada real
    python tools/icon_gen/openai_grid.py --tag v3                  # lote 3 (semi-realista)
    python tools/icon_gen/openai_grid.py --refs a.png|b.png       # referências extras
    python tools/icon_gen/openai_grid.py --spend                   # só o gasto
    python tools/icon_gen/openai_grid.py --rebuild-sheet           # refaz a folha
    python tools/icon_gen/openai_grid.py --serve                   # servidor MCP

Custo e segredo:

- A chave vem de ``OPENAI_API_KEY`` ou do arquivo em ``OGS_ICONAPI_KEY_FILE``
  (padrão: G:/projetos/opengs-icons-private/secrets/openai_api_key.txt) e as
  imagens de estilo de ``OGS_ICONAPI_REF_DIR``
  (padrão: G:/projetos/opengs-icons-private/ref_style).
  A chave NUNCA é impressa, logada, commitada ou copiada: qualquer texto de
  erro passa por ``_mask_secrets`` e o relatório mostra no máximo os 4 últimos
  caracteres.
- O gasto acumulado fica em ``tools/icon_gen/api_spend.json`` (fora do git) e
  uma chamada é recusada se o total passar de US$ 4,00 (orçamento duro; a
  conta tem US$ 5,00 no total).
- Sem retry automático do SDK: uma tentativa = uma cobrança. A única exceção é
  o fallback de fundo transparente (1 nova tentativa, registrada).

Modelo e preços conferidos na documentação em 2026-11-21:
- modelos atuais: gpt-image-2.6-sunburst / -flare, gpt-image-2.5-sunburst /
  -flare, gpt-image-2 (``gpt-image-1`` foi desligado em 2026-10-23);
- preço por 1M de tokens: texto $5, imagem de entrada $8, imagem de saída $30;
- ``/v1/images/edits`` aceita até 16 imagens e ``background=transparent``.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import math
import os
import re
import sys
from datetime import datetime, timezone
from pathlib import Path

ICON_GEN_DIR = Path(__file__).resolve().parent
REPO_ROOT = ICON_GEN_DIR.parents[1]

GEN_API_DIR = REPO_ROOT / "assets" / "icons" / "illustrated_source" / "gen_api"
SHEET_PATH = REPO_ROOT / "docs" / "ui" / "icons_api_sheet.html"
SPEND_PATH = Path(
    os.environ.get("OGS_ICONAPI_SPEND", str(ICON_GEN_DIR / "api_spend.json"))
)
CACHE_DIR = ICON_GEN_DIR / ".cache" / "openai_grid"
RAW_DIR = CACHE_DIR / "raw"
STATUS_PATH = ICON_GEN_DIR / "status_grid.json"
SPEC_PATH = REPO_ROOT / "tools" / "icon_forge" / "resource_icons.json"

# Segredo e referências ficam fora do repositório e fora do Fooocus (removido
# em t31, fim da produção pela API). Overrides: OPENAI_API_KEY /
# OGS_ICONAPI_KEY_FILE e OGS_ICONAPI_REF_DIR.
DEFAULT_KEY_FILE = Path(
    r"G:\projetos\opengs-icons-private\secrets\openai_api_key.txt"
)
DEFAULT_REF_DIR = Path(r"G:\projetos\opengs-icons-private\ref_style")
# Lote 1 (v1) usou ref_sheet_goods_transparent.png + ref_sheet_minerals.png
# (estilo pintado à mão). O lote 2 (v2) seguiu a folha do pack militar do dono
# como estilo (cartoon com contorno grosso). O lote 3 (v3) mantém a mesma folha
# apenas como enquadramento e troca o traço por semi-realismo militar.
# Os prompts/descrições de v1 e v2 ficam gravados nos JSONs de cada ícone.
REF_FILES = ("ref_sheet_military_pack.png",)
REF_TARGET_WIDTH = 512  # menor tamanho ainda útil para o estilo (menos tokens)

DEFAULT_MODEL = "gpt-image-2.6-sunburst"
MODEL_CHOICES = (
    "gpt-image-2.6-sunburst",
    "gpt-image-2.6-flare",
    "gpt-image-2.5-sunburst",
    "gpt-image-2.5-flare",
    "gpt-image-2",
)
QUALITY_CHOICES = ("low", "medium", "high", "xhigh", "max")
DEFAULT_QUALITY = "medium"
DEFAULT_SIZE = "1024x1024"

BUDGET_USD = 4.00
# Preços por 1M de tokens (documentação oficial, 2026-11-21).
PRICE_TEXT_INPUT = 5.00
PRICE_IMAGE_INPUT = 8.00
PRICE_IMAGE_OUTPUT = 30.00
# Tokens de saída por imagem 1024² (estimativa conservadora; a real vem do
# `usage` da resposta). Valores da tabela histórica p/ low/medium/high.
OUTPUT_TOKENS_SQUARE = {"low": 272, "medium": 1767, "high": 4160, "xhigh": 8320, "max": 16640}
# Aproximação de tokens de imagem de entrada em alta fidelidade (pixels/512).
IMAGE_INPUT_PIXELS_PER_TOKEN = 512

ICON_ID_RE = re.compile(r"^[a-z0-9][a-z0-9_-]{0,63}$")
SECRET_RE = re.compile(r"(sk|sess|org)-[A-Za-z0-9_\-]{8,}")

# Metáforas das matérias-primas na FORMA MINERAL/BRUTA (v3), em inglês,
# prontas para o prompt. Derivadas de tools/icon_forge/resource_icons.json.
DEFAULT_DESCRIPTIONS: dict[str, str] = {
    "coal": "a pile of black bituminous coal lumps, dusty",
    "copper": "a raw copper ore chunk: dark rock with green malachite and azurite crust and native copper veins",
    "gas": "an industrial compressed natural gas steel cylinder with worn paint",
    "iron": "chunks of hematite iron ore, dark reddish-grey metallic rock",
    "oil": "crude oil in a worn steel barrel with a black oil spill",
    "bauxite": "reddish-brown bauxite ore rubble",
    "lead": "galena ore: cubic silvery-grey crystals set in rock",
    "gold": "rough gold ore nuggets embedded in white quartz",
    "phosphate": "a pile of pale beige porous phosphate rock",
    "tungsten": "wolframite ore: black heavy prismatic crystals in rock",
    "rare_earths": "monazite rare-earth ore chunks with reddish-brown crystals beside a small heap of grey oxide powder",
    "sulfur": "a crust of yellow sulfur crystals on rock",
    "diamond": "rough, uncut diamonds embedded in blue-grey kimberlite rock, NOT a cut gem",
    "uranium": "uranium ore: black pitchy pitchblende rock with a bright yellow uraninite crust, no radiation symbol",
    "wheat": "a tied bundle of ripe wheat sheaves",
    "wood": "stacked cut timber logs with sawn ends",
}

TEST_IDS = (
    "coal", "copper", "gas", "iron",
    "oil", "bauxite", "lead", "gold",
    "phosphate", "tungsten", "rare_earths", "sulfur",
    "diamond", "uranium", "wheat", "wood",
)

STYLE_PROMPT = """\
Create ONE square sprite sheet: a strict 4x4 grid of sixteen separate game
inventory icons for a serious modern-military / grand-strategy game (tone of
Modern Warfare, Arma, a serious strategy title), one single object per cell.

STYLE — semi-realistic military game art:
- SEMI-REALISTIC rendering with believable proportions and real materials
  (rock, ore, matte metal, wood, dust), not stylized shapes;
- richly detailed textures: stone grain and cracks, mineral crust, oxidation,
  chipped and worn paint, dust, small facets that catch the light;
- dirty, desaturated palette: greys, olive drab, rust, ochre, dull steel,
  aged brass, weathered wood; low saturation everywhere, no candy colors;
- hard directional light from the upper LEFT, clear light/shadow split across
  each object, a small tight contact shadow hugging the base; lit faces
  clearly brighter than shadowed faces, no soft glow, no halo;
- THIN dark outline (dark brown or near-black, 1-2 px at icon size) used only
  to keep the silhouette readable; inner details are not outlined and the line
  must never look like a cartoon stroke;
- three-quarter view of one isolated object, natural silhouette, centered in
  its cell with a clean empty margin (about 10% of the cell); all sixteen
  objects drawn at the same visual weight and scale;
- nothing touches or crosses a cell border.

The reference image is ONLY a framing guide (single isolated object,
three-quarter view, clear margin). Do NOT copy its cartoon style: no flat cel
shading, no thick uniform outlines, no toy look.

AVOID: cute, toy-like or child-cartoon shapes; thick uniform outlines; flat
vector shading; saturated or neon colors; drawing-style sparkles and star
glints; symbols, logos, emblems or warning signs standing in for the material
itself (for example a radiation trefoil instead of the ore).

BACKGROUND: fully transparent PNG (background=transparent, output_format=png).
If transparency is impossible, use one flat light neutral grey (#D9D9D9), no
gradient, no vignette, no texture, no horizon.

HARD RULES: no text, no letters, no numbers, no labels, no logos, no watermarks,
no frames, no drawn grid lines, no duplicated objects, no people, no hands.
The numbered list below is only an instruction for the layout: never render the
numbers or the words "Row 1" in the image.

GRID LAYOUT (row by row, left to right):
"""


# ---------------------------------------------------------------------------
# Utilidades gerais
# ---------------------------------------------------------------------------

def log(msg: str) -> None:
    print(f"[iconapi] {_mask_secrets(msg)}", file=sys.stderr, flush=True)


def _mask_secrets(text: object) -> str:
    return SECRET_RE.sub(lambda m: f"{m.group(1)}-***", str(text))


def _key_hint(key: str) -> str:
    return f"...{key[-4:]}" if len(key) >= 4 else "(curta)"


def _json(payload: dict) -> str:
    return json.dumps(payload, ensure_ascii=False, indent=2)


def _error(kind: str, message: str, **extra) -> str:
    payload = {"ok": False, "error": kind, "message": _mask_secrets(message)}
    payload.update(extra)
    return _json(payload)


def _now() -> str:
    return datetime.now(timezone.utc).astimezone().isoformat(timespec="seconds")


def _split_ids(ids: str | list[str] | None) -> list[str]:
    if not ids:
        return []
    if isinstance(ids, str):
        parts = re.split(r"[,\s]+", ids.strip())
    else:
        parts = [str(x) for x in ids]
    return [p for p in (x.strip() for x in parts) if p]


def _validate_ids(ids: list[str]) -> list[str]:
    out: list[str] = []
    for icon_id in ids:
        value = (icon_id or "").strip()
        if not ICON_ID_RE.fullmatch(value):
            raise ValueError(
                f"id inválido: {icon_id!r} (minúsculas, dígitos, '_' ou '-', até 64 caracteres)"
            )
        if value in out:
            raise ValueError(f"id repetido: {value!r}")
        out.append(value)
    return out


def _split_refs(refs: str | list[str] | None) -> list[str]:
    """Aceita lista ou string com caminhos separados por '|', ';' ou quebra."""
    if not refs:
        return []
    if isinstance(refs, str):
        parts = re.split(r"[|;\n]+", refs)
    else:
        parts = [str(x) for x in refs]
    return [p for p in (x.strip() for x in parts) if p]


def _parse_variant(tag: int | str | None) -> int | None:
    """Converte `tag`/`variant` (2, "2" ou "v2") no número da variação."""
    if tag is None or (isinstance(tag, str) and not tag.strip()):
        return None
    if isinstance(tag, int):
        value = tag
    else:
        match = re.fullmatch(r"v?(\d+)", str(tag).strip().lower())
        if not match:
            raise ValueError(f"tag inválida: {tag!r} (use 2, '2' ou 'v2')")
        value = int(match.group(1))
    if value < 1:
        raise ValueError(f"tag inválida: {tag!r} (a variação começa em 1)")
    return value


def _variant_files(icon_id: str, variation: int) -> list[Path]:
    id_dir = GEN_API_DIR / icon_id
    return [
        id_dir / f"{icon_id}_v{variation}.png",
        id_dir / f"{icon_id}_v{variation}_64.png",
        id_dir / f"{icon_id}_v{variation}.json",
    ]


def _write_json(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(payload, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
        newline="\n",
    )


def _write_status(**fields) -> None:
    try:
        _write_json(STATUS_PATH, {"updated_at": _now(), **fields})
    except OSError as exc:  # status é conveniência, não derruba a geração
        log(f"não consegui escrever {STATUS_PATH.name}: {exc}")


# ---------------------------------------------------------------------------
# Chave da API (nunca sai daqui)
# ---------------------------------------------------------------------------

def _api_key_path() -> Path:
    return Path(os.environ.get("OGS_ICONAPI_KEY_FILE", str(DEFAULT_KEY_FILE)))


def read_api_key() -> str:
    """Lê a chave do ambiente ou do arquivo; nunca loga o conteúdo."""
    env_key = (os.environ.get("OPENAI_API_KEY") or "").strip()
    if env_key:
        return env_key
    path = _api_key_path()
    if not path.is_file():
        raise FileNotFoundError(
            f"chave da OpenAI não encontrada em {path} (defina OPENAI_API_KEY ou "
            "OGS_ICONAPI_KEY_FILE)"
        )
    key = path.read_text(encoding="utf-8-sig").strip().strip('"').strip("'").strip()
    if not key:
        raise ValueError(f"arquivo de chave vazio: {path}")
    return key


# ---------------------------------------------------------------------------
# Gasto e orçamento
# ---------------------------------------------------------------------------

def load_spend() -> dict:
    if not SPEND_PATH.is_file():
        return {"budget_usd": BUDGET_USD, "total_usd": 0.0, "calls": []}
    try:
        data = json.loads(SPEND_PATH.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        log(f"api_spend.json ilegível ({exc}); começando do zero")
        return {"budget_usd": BUDGET_USD, "total_usd": 0.0, "calls": []}
    data.setdefault("budget_usd", BUDGET_USD)
    data.setdefault("calls", [])
    data["total_usd"] = round(float(data.get("total_usd") or 0.0), 6)
    return data


def save_spend(data: dict) -> None:
    data["budget_usd"] = BUDGET_USD
    data["total_usd"] = round(sum(float(c.get("cost_usd") or 0.0) for c in data["calls"]), 6)
    data["updated_at"] = _now()
    _write_json(SPEND_PATH, data)


def _token_cost(model: str, text_tokens: int, image_tokens_in: int, image_tokens_out: int) -> float:
    del model  # 2.5 e 2.6 têm a mesma tabela
    return round(
        text_tokens / 1e6 * PRICE_TEXT_INPUT
        + image_tokens_in / 1e6 * PRICE_IMAGE_INPUT
        + image_tokens_out / 1e6 * PRICE_IMAGE_OUTPUT,
        6,
    )


def _usage_breakdown(usage: object) -> dict:
    """Extrai tokens do `usage` da resposta (dict ou objeto do SDK)."""
    if usage is None:
        return {"text_tokens": 0, "image_input_tokens": 0, "image_output_tokens": 0}
    get = usage.get if isinstance(usage, dict) else lambda k, d=None: getattr(usage, k, d)
    details = get("input_tokens_details") or {}
    dget = details.get if isinstance(details, dict) else lambda k, d=0: getattr(details, k, d)
    out_details = get("output_tokens_details") or {}
    oget = out_details.get if isinstance(out_details, dict) else lambda k, d=None: getattr(out_details, k, d)
    text_in = int(dget("text_tokens", 0) or 0)
    image_in = int(dget("image_tokens", 0) or 0)
    out_total = int(get("output_tokens", 0) or 0)
    image_out = oget("image_tokens", None)
    if image_out is None:
        image_out = out_total
    return {
        "text_tokens": text_in,
        "image_input_tokens": image_in,
        "image_output_tokens": int(image_out),
    }


def estimate_cost(
    prompt: str,
    quality: str,
    size: str,
    ref_sizes: list[tuple[int, int]],
) -> dict:
    text_tokens = max(1, math.ceil(len(prompt) / 4))
    image_tokens = sum(
        max(1, math.ceil(w * h / IMAGE_INPUT_PIXELS_PER_TOKEN)) for w, h in ref_sizes
    )
    per_image = OUTPUT_TOKENS_SQUARE.get(quality, OUTPUT_TOKENS_SQUARE["medium"])
    # tamanho arbitrário: escala os tokens pela área relativa a 1024²
    try:
        w, h = (int(x) for x in size.lower().split("x"))
        if w > 0 and h > 0:
            per_image = round(per_image * (w * h) / (1024 * 1024))
    except (ValueError, AttributeError):
        pass
    out_tokens = max(per_image, 1)
    cost = _token_cost(DEFAULT_MODEL, text_tokens, image_tokens, out_tokens)
    return {
        "text_tokens": text_tokens,
        "image_input_tokens": image_tokens,
        "image_output_tokens": out_tokens,
        "cost_usd": cost,
    }


# ---------------------------------------------------------------------------
# Descrições e prompt
# ---------------------------------------------------------------------------

def _resource_specs() -> dict:
    if not SPEC_PATH.is_file():
        return {}
    try:
        data = json.loads(SPEC_PATH.read_text(encoding="utf-8-sig"))
    except (OSError, ValueError) as exc:
        log(f"não consegui ler {SPEC_PATH.name}: {exc}")
        return {}
    icons = data.get("icons") if isinstance(data, dict) else None
    return icons if isinstance(icons, dict) else {}


def description_for(icon_id: str) -> str:
    if icon_id in DEFAULT_DESCRIPTIONS:
        return DEFAULT_DESCRIPTIONS[icon_id]
    spec = _resource_specs().get(icon_id) or {}
    label = (spec.get("label") or "").strip()
    if not label:
        raise ValueError(
            f"sem descrição para {icon_id!r}: passe `descriptions` ou cadastre a metáfora"
        )
    return f"a hand-painted icon of {label.lower()} as a single recognizable object"


def build_prompt(items: list[tuple[str, str]]) -> str:
    if not 1 <= len(items) <= 16:
        raise ValueError(f"a grade é 4x4: de 1 a 16 itens por folha (recebi {len(items)})")
    lines = [STYLE_PROMPT]
    for row in range(4):
        cells = []
        for col in range(4):
            idx = row * 4 + col
            if idx < len(items):
                icon_id, desc = items[idx]
                cells.append(f"{idx + 1}) {icon_id}: {desc}")
            else:
                cells.append(f"{idx + 1}) (empty cell — leave it completely blank)")
        lines.append(f"Row {row + 1}: " + "; ".join(cells) + ".")
    lines.append("")
    lines.append(
        "Rendering order matters: keep the 4x4 order above, equal cell sizes, and "
        "do not merge cells or move objects between rows."
    )
    if len(items) < 16:
        missing = ", ".join(str(i) for i in range(len(items) + 1, 17))
        lines.append(
            f"The last cells ({missing}) are intentionally EMPTY: draw nothing there, "
            "not even a shadow. Never duplicate an earlier object to fill them."
        )
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Imagens de referência
# ---------------------------------------------------------------------------

def _ref_dir() -> Path:
    return Path(os.environ.get("OGS_ICONAPI_REF_DIR", str(DEFAULT_REF_DIR)))


def _resolve_ref(ref: str) -> Path:
    """Caminho absoluto passa direto; nome simples resolve dentro de ref_style."""
    path = Path(ref)
    return path if path.is_absolute() else _ref_dir() / path


def prepare_refs(
    refs: str | list[str] | None = None,
    target_width: int = REF_TARGET_WIDTH,
) -> tuple[list[tuple[str, bytes, str]], list[tuple[int, int]]]:
    """Reduz as referências de estilo para JPEG pequeno (menos tokens)."""
    from PIL import Image

    names = _split_refs(refs) or list(REF_FILES)
    paths = [_resolve_ref(name) for name in names]
    missing = [str(path) for path in paths if not path.is_file()]
    if missing:
        raise FileNotFoundError(
            f"referência(s) de estilo ausente(s) em {_ref_dir()}: {', '.join(missing)}"
        )

    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    files: list[tuple[str, bytes, str]] = []
    sizes: list[tuple[int, int]] = []
    from io import BytesIO

    for path in paths:
        with Image.open(path) as im:
            im = im.convert("RGB")
            if im.width > target_width:
                height = max(1, round(im.height * target_width / im.width))
                im = im.resize((target_width, height), Image.LANCZOS)
            buf = BytesIO()
            im.save(buf, format="JPEG", quality=88, optimize=True)
            data = buf.getvalue()
            sizes.append(im.size)
            files.append((f"{path.stem}_{im.width}.jpg", data, "image/jpeg"))
            log(f"referência {path.name} → {im.width}x{im.height} ({len(data) // 1024} KB)")
    return files, sizes


# ---------------------------------------------------------------------------
# Pós-processamento (recorte, fundo, apara, 128/64)
# ---------------------------------------------------------------------------

def _border_color(im) -> tuple[int, int, int]:
    w, h = im.size
    px = im.load()
    points = [(x, 0) for x in range(0, w, max(1, w // 24))]
    points += [(x, h - 1) for x in range(0, w, max(1, w // 24))]
    points += [(0, y) for y in range(0, h, max(1, h // 24))]
    points += [(w - 1, y) for y in range(0, h, max(1, h // 24))]
    reds = sorted(px[x, y][:3][0] for x, y in points)
    greens = sorted(px[x, y][:3][1] for x, y in points)
    blues = sorted(px[x, y][:3][2] for x, y in points)
    mid = len(reds) // 2
    return reds[mid], greens[mid], blues[mid]


def _flatten_bg_to_alpha(im, soft: int = 10, hard: int = 32):
    """Troca um fundo chapado (cinza claro etc.) por transparência."""
    from PIL import Image, ImageChops

    if im.mode != "RGBA":
        im = im.convert("RGBA")
    alpha = im.getchannel("A")
    if alpha.getextrema()[0] < 8:
        return im  # já tem transparência de verdade

    rgb = im.convert("RGB")
    bg = Image.new("RGB", rgb.size, _border_color(rgb))
    diff = ImageChops.difference(rgb, bg)
    r, g, b = diff.split()
    dist = ImageChops.lighter(ImageChops.lighter(r, g), b)
    lut = []
    for v in range(256):
        if v <= soft:
            lut.append(0)
        elif v >= hard:
            lut.append(255)
        else:
            lut.append(round((v - soft) * 255 / (hard - soft)))
    mask = dist.point(lut)
    im.putalpha(mask)
    return im


def _trim(im, pad_ratio: float = 0.06):
    from PIL import Image

    alpha = im.getchannel("A")
    bbox = alpha.getbbox()
    if not bbox:
        return im
    im = im.crop(bbox)
    side = max(im.width, im.height)
    side = max(1, round(side * (1 + 2 * pad_ratio)))
    canvas = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    canvas.paste(im, ((side - im.width) // 2, (side - im.height) // 2), im)
    return canvas


def _split_grid(im, rows: int = 4, cols: int = 4, inset: float = 0.01):
    from PIL import Image

    if im.mode != "RGBA":
        im = im.convert("RGBA")
    cell_w, cell_h = im.width / cols, im.height / rows
    cells = []
    for row in range(rows):
        for col in range(cols):
            dx, dy = round(cell_w * inset), round(cell_h * inset)
            box = (
                round(col * cell_w) + dx,
                round(row * cell_h) + dy,
                round((col + 1) * cell_w) - dx,
                round((row + 1) * cell_h) - dy,
            )
            cells.append(im.crop(box))
    return cells


def _save_sizes(tile, out_dir: Path, icon_id: str, variation: int) -> dict:
    from PIL import Image

    out_dir.mkdir(parents=True, exist_ok=True)
    main = out_dir / f"{icon_id}_v{variation}.png"
    small = out_dir / f"{icon_id}_v{variation}_64.png"
    tile.resize((128, 128), Image.LANCZOS).save(main)
    tile.resize((64, 64), Image.LANCZOS).save(small)
    return {"png": str(main), "png_64": str(small)}


def _next_variation(icon_id: str) -> int:
    id_dir = GEN_API_DIR / icon_id
    if not id_dir.is_dir():
        return 1
    found = [
        int(m.group(1))
        for p in id_dir.glob(f"{icon_id}_v*.png")
        if (m := re.fullmatch(re.escape(icon_id) + r"_v(\d+)(?:_64)?\.png", p.name))
    ]
    return max(found, default=0) + 1


# ---------------------------------------------------------------------------
# Folha de revisão
# ---------------------------------------------------------------------------

VARIATION_NOTES = {
    1: "pintado à mão (lote 1)",
    2: "cartoon da referência (contorno grosso)",
    3: "semi-realista militar (luz dura, paleta suja)",
}


def collect_entries() -> list[dict]:
    """Lê todos os metadados de gen_api (todas as variações)."""
    entries: list[dict] = []
    if not GEN_API_DIR.is_dir():
        return entries
    for id_dir in sorted(p for p in GEN_API_DIR.iterdir() if p.is_dir()):
        for meta_path in sorted(id_dir.glob(f"{id_dir.name}_v*.json")):
            try:
                meta = json.loads(meta_path.read_text(encoding="utf-8"))
            except (OSError, ValueError):
                continue
            variation = meta.get("variation")
            if not isinstance(variation, int):
                continue
            entries.append(
                {
                    "id": id_dir.name,
                    "label": meta.get("label") or "",
                    "description": meta.get("description") or "",
                    "variation": variation,
                    "cell": meta.get("cell"),
                    "empty": meta.get("empty"),
                    "cost_usd": meta.get("cost_usd"),
                    "png": str(id_dir / f"{id_dir.name}_v{variation}.png"),
                    "png_64": str(id_dir / f"{id_dir.name}_v{variation}_64.png"),
                }
            )
    return entries


def _ordered_ids(entries: list[dict]) -> list[str]:
    known = {icon_id: idx for idx, icon_id in enumerate(TEST_IDS)}
    return sorted({e["id"] for e in entries}, key=lambda v: (known.get(v, len(known)), v))


def render_sheet(entries: list[dict], call_meta: dict, spend: dict) -> Path:
    """Folha comparativa: cada matéria-prima com as variações lado a lado."""
    import html as _html

    def esc(value: object) -> str:
        return _html.escape(str(value), quote=True)

    def rel(path: str | Path) -> str:
        return Path(os.path.relpath(path, SHEET_PATH.parent)).as_posix()

    SHEET_PATH.parent.mkdir(parents=True, exist_ok=True)
    grouped: dict[str, list[dict]] = {}
    for entry in entries:
        grouped.setdefault(entry["id"], []).append(entry)

    cards = []
    for icon_id in _ordered_ids(entries):
        variants = sorted(grouped[icon_id], key=lambda e: int(e.get("variation") or 0))
        label = next((v.get("label") for v in variants if v.get("label")), "")
        blocks = []
        for entry in variants:
            variation = entry.get("variation", "?")
            note = VARIATION_NOTES.get(int(variation) if str(variation).isdigit() else 0, "")
            blocks.append(
                f'<div class="variant{" empty" if entry.get("empty") else ""}">'
                f'<div class="vlabel">v{esc(variation)}'
                f'{"" if not note else " · " + esc(note)}</div>'
                f'<div class="pics">'
                f'<img class="big" src="{esc(rel(entry["png"]))}" '
                f'alt="{esc(icon_id)} v{esc(variation)} 128">'
                f'<img class="small" src="{esc(rel(entry["png_64"]))}" '
                f'alt="{esc(icon_id)} v{esc(variation)} 64"></div>'
                f'<div class="desc">{esc(entry.get("description") or "")}</div>'
                f'<div class="meta">célula {esc(entry.get("cell", "?"))}'
                f"{' · vazia' if entry.get('empty') else ''}"
                f' · US$ {esc(entry.get("cost_usd", "?"))} (1/16)</div>'
                "</div>"
            )
        cards.append(
            f'<div class="card">'
            f'<div class="id">{esc(icon_id)} <span class="tag">{esc(label)}</span></div>'
            f'<div class="variants">{"".join(blocks)}</div>'
            "</div>"
        )
    raw = call_meta.get("sheet_image")
    raw_block = (
        f'<p class="meta">Folha gerada: <a href="{esc(rel(raw))}">{esc(Path(raw).name)}</a> '
        f"({esc(call_meta.get('size'))}, {esc(call_meta.get('model'))}, "
        f"qualidade {esc(call_meta.get('quality'))})</p>"
        if raw
        else ""
    )
    html_text = f"""<!DOCTYPE html>
<html lang="pt-BR">
<head>
<meta charset="utf-8">
<title>OpenGS — ícones de recurso (OpenAI, grade 4×4)</title>
<style>
:root{{--paper:#ece5d3;--ink:#2b2a26;--muted:#6b675c;--brass:#8a6d3b;--line:#c9c0aa;}}
*{{box-sizing:border-box}}
body{{margin:0;padding:32px;background:var(--paper);color:var(--ink);
font:14px/1.45 system-ui,Segoe UI,Roboto,sans-serif}}
h1{{font-size:22px;margin:0 0 4px}}
.meta{{color:var(--muted);margin:0 0 8px}}
.note{{max-width:70ch}}
.grid{{display:grid;grid-template-columns:repeat(auto-fill,minmax(440px,1fr));gap:14px;margin-top:20px}}
.card{{background:#fff;border:1px solid var(--line);border-radius:6px;padding:12px;display:flex;flex-direction:column;gap:10px}}
.variants{{display:flex;gap:14px;flex-wrap:wrap;align-items:flex-start}}
.variant{{display:flex;flex-direction:column;gap:6px;max-width:200px}}
.variant.empty{{background:#fdf6ec;border:1px dashed #c49a63;border-radius:4px;padding:6px}}
.vlabel{{font-size:12px;font-weight:600;color:var(--brass)}}
.pics{{display:flex;align-items:flex-end;gap:10px}}
.pics img{{image-rendering:auto;background:repeating-conic-gradient(#f4f0e6 0 25%,#e7e1d2 0 50%) 0 0/16px 16px;border:1px solid var(--line);border-radius:4px}}
.pics .big{{width:128px;height:128px}}
.pics .small{{width:64px;height:64px}}
.id{{font-weight:600}}
.tag{{font-size:12px;color:var(--muted);font-weight:400}}
.desc,.meta{{font-size:12px;color:var(--muted)}}
a{{color:var(--brass)}}
</style>
</head>
<body>
<h1>OpenGS — ícones de recurso gerados pela API da OpenAI</h1>
{raw_block}
<p class="meta note">v1 é o lote pintado à mão. v2 copia o traço cartoon de
<code>ref_sheet_military_pack.png</code> (contorno escuro grosso, cel shading chapado).
v3 responde ao feedback do dono: tom de jogo militar moderno (Modern Warfare / Arma),
semi-realista, materiais e texturas reais, paleta dessaturada e suja, luz dura de
cima-esquerda com sombra de contato e contorno fino só para legibilidade; a referência
passa a valer apenas como enquadramento. Todas as matérias-primas aparecem na forma
mineral/bruta.</p>
<p class="meta">{esc(len(grouped))} matérias-primas • variações {esc(sorted({e.get("variation") for e in entries}))} •
gasto real desta chamada US$ {esc(call_meta.get('cost_usd'))} •
gasto acumulado US$ {esc(spend.get('total_usd'))} de US$ {esc(spend.get('budget_usd'))} (orçamento) •
{esc(call_meta.get('created_at'))}</p>
<p class="meta">Custo é estimado por tokens do <code>usage</code>; a conta oficial sai no painel da OpenAI.</p>
<div class="grid">
{''.join(cards)}
</div>
</body>
</html>
"""
    SHEET_PATH.write_text(html_text, encoding="utf-8", newline="\n")
    return SHEET_PATH


def rebuild_sheet() -> dict:
    """Recria a folha comparativa a partir dos JSONs salvos (sem API)."""
    entries = collect_entries()
    if not entries:
        return {"ok": False, "error": "missing", "message": f"nenhum ícone em {GEN_API_DIR}"}
    latest = max(entries, key=lambda e: str(e.get("variation")))
    last = {}
    last_path = GEN_API_DIR / latest["id"] / f"{latest['id']}_v{latest['variation']}.json"
    if last_path.is_file():
        try:
            last = json.loads(last_path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            last = {}
    call_meta = {
        "sheet_image": last.get("sheet_image"),
        "model": last.get("model"),
        "quality": last.get("quality"),
        "size": last.get("size"),
        "cost_usd": last.get("call_cost_usd"),
        "created_at": last.get("created_at"),
    }
    path = render_sheet(entries, call_meta, load_spend())
    return {"ok": True, "path": str(path), "icons": len(entries)}


# ---------------------------------------------------------------------------
# Geração
# ---------------------------------------------------------------------------

def _client():
    from openai import OpenAI

    key = read_api_key()
    log(f"chave carregada ({_key_hint(key)}); nenhuma cópia é gravada")
    return OpenAI(api_key=key, max_retries=0, timeout=600.0)


def generate_grid(
    ids: str | list[str] | None = None,
    descriptions: str | list[str] | None = None,
    quality: str = DEFAULT_QUALITY,
    dry_run: bool = False,
    model: str = DEFAULT_MODEL,
    size: str = DEFAULT_SIZE,
    refs: str | list[str] | None = None,
    variant: int | str | None = None,
) -> str:
    """Gera uma folha 4×4 (16 ícones) com a API de imagens da OpenAI.

    Args:
        ids: até 16 ids separados por vírgula (ou lista), na ordem linha a linha;
            menos de 16 deixa as últimas células vazias (lote final).
        descriptions: descrições na mesma quantidade (lista ou separadas por
            '|'), opcional; sem elas usa as metáforas do icon_forge para
            matérias-primas.
        quality: low, medium, high, xhigh ou max (padrão medium).
        dry_run: mostra prompt, referências e custo estimado sem chamar a API.
        model: gpt-image-2.6-sunburst (padrão), -flare, 2.5 ou gpt-image-2.
        size: 1024x1024 (padrão) ou outro tamanho múltiplo de 16.
        refs: uma ou mais imagens de referência (caminho absoluto ou nome em
            ref_style), separadas por '|', ';' ou lista; vazio usa a padrão.
        variant: número da variação (2, "2" ou "v2"); vazio usa a próxima livre.
            Se os arquivos da variação já existirem, nada é chamado.

    Devolve JSON com os caminhos dos 16 PNGs (128 e 64), a folha HTML, o custo
    real (usage) e o gasto acumulado. Recusa se o orçamento de US$ 4,00 estourar.
    """
    try:
        icon_ids = _validate_ids(_split_ids(ids) or list(TEST_IDS))
        if not 1 <= len(icon_ids) <= 16:
            return _error("input", f"preciso de 1 a 16 ids (recebi {len(icon_ids)})")
        if isinstance(descriptions, str):
            descs = [d.strip() for d in descriptions.split("|") if d.strip()]
        else:
            descs = [" ".join(str(d).split()) for d in (descriptions or []) if str(d).strip()]
        if descs and len(descs) != len(icon_ids):
            return _error(
                "input",
                f"descriptions precisa ter {len(icon_ids)} itens (recebi {len(descs)})",
            )
        if not descs:
            descs = [description_for(i) for i in icon_ids]
        quality = (quality or DEFAULT_QUALITY).strip().lower()
        if quality not in QUALITY_CHOICES:
            return _error("input", f"quality inválida: {quality!r} (use {', '.join(QUALITY_CHOICES)})")
        model = (model or DEFAULT_MODEL).strip().lower()
        if model not in MODEL_CHOICES:
            return _error("input", f"model inválido: {model!r} (use {', '.join(MODEL_CHOICES)})")
        size = (size or DEFAULT_SIZE).strip().lower()
        variant_no = _parse_variant(variant)
        ref_names = _split_refs(refs) or list(REF_FILES)
    except (ValueError, TypeError, FileNotFoundError) as exc:
        return _error("input", str(exc))

    if variant_no is not None:
        clashes = sorted(
            str(path)
            for icon_id in icon_ids
            for path in _variant_files(icon_id, variant_no)
            if path.exists()
        )
        if clashes:
            return _error(
                "input",
                f"a variação v{variant_no} já existe; nada foi chamado (evita sobrescrever). "
                f"Primeiro conflito: {clashes[0]}",
                variant=variant_no,
            )

    specs = _resource_specs()
    items = list(zip(icon_ids, descs))
    prompt = build_prompt(items)
    try:
        ref_files, ref_sizes = prepare_refs(ref_names)
    except (OSError, FileNotFoundError) as exc:
        return _error("input", str(exc))
    estimate = estimate_cost(prompt, quality, size, ref_sizes)
    spend = load_spend()
    spent = float(spend.get("total_usd") or 0.0)
    # Pior caso: a 1ª tentativa falha no fundo transparente e há 1 fallback.
    worst_case = round(estimate["cost_usd"] * 2, 6)
    projected = round(spent + worst_case, 6)

    base = {
        "model": model,
        "quality": quality,
        "size": size,
        "variant": variant_no,
        "refs": ref_names,
        "icons": [
            {"id": icon_id, "label": (specs.get(icon_id) or {}).get("label", ""),
             "description": desc, "cell": idx + 1}
            for idx, (icon_id, desc) in enumerate(items)
        ],
        "references": [
            {"name": name, "size": list(dim), "kb": round(len(data) / 1024, 1)}
            for (name, data, _mime), dim in zip(ref_files, ref_sizes)
        ],
        "estimate": {
            **estimate,
            "worst_case_usd": worst_case,
            "projected_total_usd": projected,
        },
        "budget": {"spent_usd": spent, "budget_usd": BUDGET_USD, "headroom_usd": round(BUDGET_USD - spent, 6)},
        "transparent_background": True,
        "fallback_background": "#D9D9D9",
        "seed": None,
    }

    if projected > BUDGET_USD:
        return _error(
            "budget",
            f"a chamada levaria o gasto a US$ {projected:.4f}, acima do teto de US$ {BUDGET_USD:.2f}",
            **base,
        )

    if dry_run:
        _write_status(phase="dry-run", estimate=estimate, quality=quality, model=model)
        return _json({"ok": True, "dry_run": True, "prompt": prompt, **base})

    # ---- chamada real ----
    try:
        client = _client()
    except (OSError, ValueError) as exc:
        return _error("key", str(exc), **base)

    uploads = [(name, data, mime) for name, data, mime in ref_files]
    RAW_DIR.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    attempts: list[dict] = []
    background_used = "transparent"

    def call(background: str):
        return client.images.edit(
            model=model,
            image=uploads,
            prompt=prompt,
            background=background,
            output_format="png",
            quality=quality,
            size=size,
            n=1,
            safety_identifier=hashlib.sha256(b"opengs-iconapi").hexdigest()[:32],
        )

    def run_attempt(background: str) -> tuple[object | None, str | None]:
        nonlocal background_used
        _write_status(phase="gerando", background=background, quality=quality, model=model)
        log(f"chamando {model} quality={quality} size={size} background={background}")
        try:
            return call(background), None
        except Exception as exc:  # noqa: BLE001 - queremos a mensagem da API mascarada
            msg = _mask_secrets(f"{type(exc).__name__}: {exc}")
            status = getattr(exc, "status_code", None)
            attempts.append({"background": background, "error": msg, "status": status})
            log(f"falhou ({status}): {msg}")
            return None, msg

    response, err = run_attempt("transparent")
    if response is None and "background" in (err or "").lower():
        background_used = "opaque"
        prompt = prompt.replace(
            "BACKGROUND: fully transparent PNG (background=transparent, output_format=png).",
            "BACKGROUND: one flat light neutral grey (#D9D9D9), no gradient, no texture.",
        )
        log("modelo recusou fundo transparente; 1 nova tentativa com fundo cinza chapado")
        response, err = run_attempt("opaque")
    if response is None:
        _write_status(phase="erro", error=attempts[-1] if attempts else err)
        return _error(
            "api",
            f"a API recusou a chamada ({err}); ver o texto completo em `attempts`",
            attempts=attempts,
            **base,
        )

    usage = _usage_breakdown(getattr(response, "usage", None))
    cost = _token_cost(model, usage["text_tokens"], usage["image_input_tokens"], usage["image_output_tokens"])
    data = getattr(response, "data", None) or []
    if not data or not getattr(data[0], "b64_json", None):
        _write_status(phase="erro", error="resposta sem b64_json")
        return _error("api", "resposta sem imagem (b64_json ausente)", attempts=attempts, **base)

    raw_path = RAW_DIR / f"{stamp}_grid_{quality}.png"
    raw_path.write_bytes(base64.b64decode(data[0].b64_json))
    log(f"folha bruta: {raw_path} ({raw_path.stat().st_size // 1024} KB)")

    from PIL import Image

    _write_status(phase="pós-processo", sheet=str(raw_path))
    with Image.open(raw_path) as sheet_im:
        sheet_im = sheet_im.convert("RGBA")
        if background_used == "opaque":
            sheet_im = _flatten_bg_to_alpha(sheet_im)
        cells = _split_grid(sheet_im)

        entries: list[dict] = []
        for idx, ((icon_id, desc), cell) in enumerate(zip(items, cells)):
            variation = variant_no if variant_no is not None else _next_variation(icon_id)
            tile = _trim(_flatten_bg_to_alpha(cell))
            empty = tile.getchannel("A").getbbox() is None
            paths = _save_sizes(tile, GEN_API_DIR / icon_id, icon_id, variation)
            meta = {
                "id": icon_id,
                "label": (specs.get(icon_id) or {}).get("label", ""),
                "description": desc,
                "variation": variation,
                "tag": f"v{variation}",
                "cell": idx + 1,
                "empty": empty,
                "png": paths["png"],
                "png_64": paths["png_64"],
                "model": model,
                "quality": quality,
                "size": size,
                "background": background_used,
                "refs": ref_names,
                "seed": None,
                "prompt": prompt,
                "created_at": _now(),
                "sheet_image": str(raw_path),
                "usage": usage,
                "cost_usd": round(cost / 16, 6),
                "cost_is_share_of_call": True,
                "call_cost_usd": cost,
                "estimate_call_usd": estimate["cost_usd"],
                "attempts": attempts,
            }
            _write_json(GEN_API_DIR / icon_id / f"{icon_id}_v{variation}.json", meta)
            entries.append(
                {
                    "id": icon_id,
                    "label": meta["label"],
                    "description": desc,
                    "variation": variation,
                    "cell": idx + 1,
                    "empty": empty,
                    **paths,
                    "call": {
                        "sheet_image": str(raw_path),
                        "model": model,
                        "quality": quality,
                        "size": size,
                        "cost_usd": cost,
                        "created_at": _now(),
                    },
                }
            )
            log(f"célula {idx + 1:02d} {icon_id} v{variation} → {paths['png']}")

    spend["calls"].append(
        {
            "created_at": _now(),
            "model": model,
            "quality": quality,
            "size": size,
            "background": background_used,
            "variant": variant_no,
            "refs": ref_names,
            "icons": icon_ids,
            "usage": usage,
            "cost_usd": cost,
            "estimate_usd": estimate["cost_usd"],
            "sheet_image": str(raw_path),
            "attempts": attempts,
        }
    )
    save_spend(spend)
    sheet_path = render_sheet(
        collect_entries(),
        {
            "sheet_image": str(raw_path),
            "model": model,
            "quality": quality,
            "size": size,
            "cost_usd": cost,
            "created_at": _now(),
        },
        spend,
    )
    _write_status(phase="ocioso", last_cost_usd=cost, total_usd=spend["total_usd"], sheet=str(sheet_path))
    log(f"folha de revisão: {sheet_path}")

    return _json(
        {
            "ok": True,
            "dry_run": False,
            "sheet_image": str(raw_path),
            "sheet_html": str(sheet_path),
            "icons": entries,
            "usage": usage,
            "cost_usd": cost,
            "spend_total_usd": spend["total_usd"],
            "budget_usd": BUDGET_USD,
            "headroom_usd": round(BUDGET_USD - spend["total_usd"], 6),
            "background": background_used,
            "attempts": attempts,
            "model": model,
            "quality": quality,
            "size": size,
        }
    )


# ---------------------------------------------------------------------------
# Servidor MCP
# ---------------------------------------------------------------------------

def register_grid_tools(mcp) -> None:
    """Registra `generate_grid` num FastMCP já existente (server.py do MCP)."""
    # O nome local da tool sombreia o da função do módulo; pega a global para
    # não recursar (a tool apenas delega para a implementação de verdade).
    generate_grid_impl = globals()["generate_grid"]

    @mcp.tool()
    def generate_grid(
        ids: str = ",".join(TEST_IDS),
        descriptions: str = "",
        quality: str = DEFAULT_QUALITY,
        dry_run: bool = False,
        model: str = DEFAULT_MODEL,
        size: str = DEFAULT_SIZE,
        refs: str = "",
        variant: str = "",
    ) -> str:
        """Gera uma folha 4×4 (16 ícones de recurso) com a API de imagens da OpenAI.

        Args:
            ids: 16 ids separados por vírgula, na ordem linha a linha.
            descriptions: 16 descrições separadas por '|' (vazio = metáforas do icon_forge).
            quality: low, medium (padrão), high, xhigh ou max.
            dry_run: mostra prompt e custo estimado sem chamar a API (comece aqui).
            model: gpt-image-2.6-sunburst (padrão) ou outro modelo GPT Image atual.
            size: 1024x1024 (padrão) ou outro tamanho múltiplo de 16.
            refs: imagens de estilo separadas por '|' (vazio = ref_sheet_military_pack.png).
            variant: número/tag da variação ("v3"); vazio = próxima livre, sem sobrescrever.

        Recorta as 16 células, apara, remove o fundo chapado e salva 128 e 64 px
        em assets/icons/illustrated_source/gen_api/<id>/<id>_v<n>.png (+ _64).
        Registra o custo (usage × preço) em tools/icon_gen/api_spend.json e
        recusa a chamada se o acumulado passar de US$ 4,00. A chave nunca é
        impressa nem copiada para o repositório.
        """
        return generate_grid_impl(
            ids=ids,
            descriptions=descriptions,
            quality=quality,
            dry_run=dry_run,
            model=model,
            size=size,
            refs=refs,
            variant=variant,
        )


def _serve() -> None:
    try:
        from mcp.server.fastmcp import FastMCP
    except ImportError as exc:  # pragma: no cover
        raise SystemExit(
            "a lib 'mcp' não está no venv; rode:\n"
            "  uv pip install --python tools/icon_gen/.venv/Scripts/python.exe 'mcp>=1.12,<2'"
            f"\nDetalhe: {exc}"
        )
    mcp = FastMCP("opengs-icons-openai")
    register_grid_tools(mcp)
    mcp.run()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Grade 4x4 de ícones de recurso via OpenAI")
    parser.add_argument("--ids", default=",".join(TEST_IDS), help="até 16 ids separados por vírgula")
    parser.add_argument("--descriptions", default="", help="descrições separadas por '|' (mesma quantidade de ids)")
    parser.add_argument("--quality", default=DEFAULT_QUALITY, choices=QUALITY_CHOICES)
    parser.add_argument("--model", default=DEFAULT_MODEL, choices=MODEL_CHOICES)
    parser.add_argument("--size", default=DEFAULT_SIZE)
    parser.add_argument(
        "--refs",
        default="",
        help="imagens de estilo (caminho absoluto ou nome em ref_style), separadas por '|'",
    )
    parser.add_argument(
        "--tag",
        default="",
        help="variação do lote (ex.: v3); não sobrescreve um lote já existente",
    )
    parser.add_argument("--dry-run", action="store_true", help="só prompt + custo estimado")
    parser.add_argument("--spend", action="store_true", help="mostra o gasto acumulado")
    parser.add_argument("--rebuild-sheet", action="store_true", help="refaz a folha HTML")
    parser.add_argument("--serve", action="store_true", help="inicia o servidor MCP (stdio)")
    args = parser.parse_args(argv)

    if args.serve:
        _serve()
        return 0
    if args.spend:
        spend = load_spend()
        print(_json({
            "spent_usd": spend["total_usd"],
            "budget_usd": spend["budget_usd"],
            "headroom_usd": round(spend["budget_usd"] - spend["total_usd"], 6),
            "calls": [
                {k: c.get(k) for k in ("created_at", "model", "quality", "size", "cost_usd")}
                for c in spend["calls"]
            ],
        }))
        return 0
    if args.rebuild_sheet:
        payload = rebuild_sheet()
        print(_json(payload))
        return 0 if payload.get("ok") else 1

    payload = generate_grid(
        ids=args.ids,
        descriptions=args.descriptions,
        quality=args.quality,
        dry_run=args.dry_run,
        model=args.model,
        size=args.size,
        refs=args.refs,
        variant=args.tag,
    )
    print(payload)
    try:
        return 0 if json.loads(payload).get("ok") else 1
    except ValueError:
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
