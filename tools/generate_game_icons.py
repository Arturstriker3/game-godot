"""Gera ícones de produto a partir de emoji, como silhueta monocromática.

Mesma ideia do web-asset-generator: emoji + Pillow (sem IA de imagem).
Cada emoji é renderizado em 256x256 e achatado para BRANCO com o alpha do
glifo — no Godot, tinge-se via `modulate` (latão/steel).

Requer Pillow:
    <python> -m pip install Pillow

Uso:
    <python> tools/generate_game_icons.py
Saída:
    assets/icons/products/<nome>.png

Licença: os GLIFOS vêm da fonte de emoji do sistema (Segoe UI Emoji). Para um
lançamento, troque `EMOJI_FONT` por Noto Color Emoji (SIL OFL 1.1) ou use
Twemoji (CC-BY 4.0) — o script é o mesmo.
"""

import os

from PIL import Image, ImageDraw, ImageFont

SIZE = 256
OUT = os.path.join("assets", "icons", "products")
EMOJI_FONT = "C:/Windows/Fonts/seguiemj.ttf"

# produto -> emoji (aproximações: flour=bowl, iron=pick, rifle=pistol)
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


def main() -> None:
    os.makedirs(OUT, exist_ok=True)
    font = ImageFont.truetype(EMOJI_FONT, SIZE)
    for name, emoji in EMOJIS.items():
        img = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
        draw = ImageDraw.Draw(img)
        try:
            draw.text((0, 0), emoji, font=font, embedded_color=True)
        except TypeError:
            draw.text((0, 0), emoji, font=font)

        alpha = img.getchannel("A")
        coverage = sum(1 for v in alpha.getdata() if v > 0)
        # Silhueta branca: cor fixa + alpha do glifo (anti-aliased).
        white = Image.new("RGBA", (SIZE, SIZE), (255, 255, 255, 255))
        white.putalpha(alpha)
        white.save(os.path.join(OUT, f"{name}.png"))
        print(f"{name}.png  coverage={coverage}")

    with open(os.path.join(OUT, "LICENSE.txt"), "w", encoding="utf-8") as fh:
        fh.write(LICENSE)
    print(f"OK: {len(EMOJIS)} ícones em {OUT}")


if __name__ == "__main__":
    main()
