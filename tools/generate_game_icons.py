"""Gera ícones de produto a partir de emoji, centralizados e sem corte.

Renderiza o emoji, mede o contorno do glifo (alpha), recorta e ajusta (contain)
dentro de um quadrado 256x256 com margem uniforme. Gera duas versões:
    <nome>.png        -> silhueta branca (tingível)
    <nome>_color.png  -> cores nativas do emoji

Requer Pillow:
    <python> -m pip install Pillow

Licença: glifos da fonte de emoji do sistema (Segoe UI Emoji). Para lançamento,
troque EMOJI_FONT por Noto Color Emoji (SIL OFL 1.1) ou use Twemoji (CC-BY 4.0).
"""

import os

from PIL import Image, ImageDraw, ImageFont

SIZE = 256
PAD = 20
OUT = os.path.join("assets", "icons", "products")
EMOJI_FONT = "C:/Windows/Fonts/seguiemj.ttf"

EMOJIS = {
    "wheat": "\U0001F33E",        # 🌾
    "flour": "\U0001F963",        # 🥣
    "bread": "\U0001F35E",        # 🍞
    "iron": "\u26CF\ufe0f",       # ⛏️
    "factory": "\U0001F3ED",      # 🏭
    "crate": "\U0001F4E6",        # 📦
    "coin": "\U0001FA99",         # 🪙
    "oil": "\U0001F6E2\ufe0f",    # 🛢️
    "sword": "\u2694\ufe0f",      # ⚔️
    "rifle": "\U0001F52B",        # 🔫
    "truck": "\U0001F69A",        # 🚚
    "ship": "\U0001F6A2",         # 🚢
    "plane": "\u2708\ufe0f",      # ✈️
}

LICENSE = """Game icons generated from emoji glyphs (tools/generate_game_icons.py).
For release, use Noto Color Emoji (SIL OFL 1.1) or Twemoji (CC-BY 4.0) as the
glyph source; the system font is used only for local prototyping.
"""


def _render_ink(emoji: str, font: ImageFont.FreeTypeFont) -> Image.Image:
    measure = Image.new("RGBA", (1, 1), (0, 0, 0, 0))
    md = ImageDraw.Draw(measure)
    bbox = md.textbbox((0, 0), emoji, font=font)
    w = max(1, bbox[2] - bbox[0])
    h = max(1, bbox[3] - bbox[1])

    canvas = Image.new("RGBA", (w + PAD * 2, h + PAD * 2), (0, 0, 0, 0))
    d = ImageDraw.Draw(canvas)
    try:
        d.text((PAD - bbox[0], PAD - bbox[1]), emoji, font=font, embedded_color=True)
    except TypeError:
        d.text((PAD - bbox[0], PAD - bbox[1]), emoji, font=font)

    return canvas.crop((PAD, PAD, PAD + w, PAD + h))


def _fit_square(ink: Image.Image, size: int) -> Image.Image:
    w, h = ink.size
    scale = min(size / w, size / h)
    nw = max(1, int(w * scale))
    nh = max(1, int(h * scale))
    resized = ink.resize((nw, nh), Image.LANCZOS)
    out = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    out.paste(resized, ((size - nw) // 2, (size - nh) // 2), resized)
    return out


def main() -> None:
    os.makedirs(OUT, exist_ok=True)
    font = ImageFont.truetype(EMOJI_FONT, SIZE)
    for name, emoji in EMOJIS.items():
        ink = _render_ink(emoji, font)
        fitted = _fit_square(ink, SIZE)

        fitted.save(os.path.join(OUT, f"{name}_color.png"))

        alpha = fitted.getchannel("A")
        white = Image.new("RGBA", (SIZE, SIZE), (255, 255, 255, 255))
        white.putalpha(alpha)
        white.save(os.path.join(OUT, f"{name}.png"))

        coverage = sum(alpha.histogram()[1:])
        print(f"{name}  coverage={coverage}")

    with open(os.path.join(OUT, "LICENSE.txt"), "w", encoding="utf-8") as fh:
        fh.write(LICENSE)
    print(f"OK: {len(EMOJIS)} ícones em {OUT}")


if __name__ == "__main__":
    main()
