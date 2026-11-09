"""Gera os cursores de mão a partir de ícones prontos (tools/cursor_sources).

- cursor_pointer.png  <- MDI "hand-back-right" (mão aberta, hover em porto)
- cursor_grab.png     <- Phosphor "hand-grabbing-fill" (mão fechada, futuro
                         estado "segurando")

Aplica o estilo dos demais cursores: latão (#C9A24A) com contorno escuro
uniforme. O contorno é a silhueta dilatada (o desenho original fica intacto,
sem erosão), então os dedos finos não somem em 32 px.

Uso: python tools/build_cursor_icons.py
"""

import os

from PIL import Image, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(HERE, "cursor_sources")
OUT = os.path.join(ROOT, "assets", "ui")

CANVAS = 32
CONTENT = 26          # maior dimensão do ícone dentro do canvas (seta tem 28, lupa 27)
OUTLINE = 11          # MaxFilter em 256 px ~ 1,4 px em 32

brass = (201, 162, 74, 255)
dark = (12, 12, 12, 255)


def build(src_name: str, out_name: str) -> None:
    im = Image.open(os.path.join(SRC, src_name)).convert("RGBA")

    # Margem para o contorno não ser cortado na borda.
    pad = OUTLINE
    padded = Image.new("RGBA", (im.width + pad * 2, im.height + pad * 2), (0, 0, 0, 0))
    padded.alpha_composite(im, (pad, pad))

    a = padded.getchannel("A")
    dil = a.filter(ImageFilter.MaxFilter(OUTLINE))
    d = Image.new("RGBA", padded.size, dark)
    d.putalpha(dil)
    b = Image.new("RGBA", padded.size, brass)
    b.putalpha(a)
    comp = Image.alpha_composite(d, b)

    comp = comp.crop(comp.getbbox())
    scale = CONTENT / float(max(comp.size))
    w = max(1, round(comp.width * scale))
    h = max(1, round(comp.height * scale))
    comp = comp.resize((w, h), Image.LANCZOS)

    canvas = Image.new("RGBA", (CANVAS, CANVAS), (0, 0, 0, 0))
    canvas.alpha_composite(comp, ((CANVAS - w) // 2, (CANVAS - h) // 2))
    canvas.save(os.path.join(OUT, out_name))
    print(out_name, canvas.getbbox(), "conteudo", w, "x", h)


build("mdi_hand_back_right.png", "cursor_pointer.png")
build("ph_hand_grabbing_fill.png", "cursor_grab.png")
