"""Gera o cursor de informação ("i") dos mapas de dados.

Mesmo estilo gravado dos demais cursores: anel e "i" em latão (#C9A24A) com
contorno escuro, hotspot no centro (16,16). Renderiza em 8x e reduz.

Uso: python tools/generate_info_cursor.py
"""

from PIL import Image, ImageDraw

S = 32
SS = 8
W = S * SS

brass = (201, 162, 74, 255)
dark = (12, 12, 12, 255)

img = Image.new("RGBA", (W, W), (0, 0, 0, 0))
d = ImageDraw.Draw(img)


def sc(v):
    return int(round(v * SS))


def ring(cx, cy, r, width, color):
    d.ellipse([sc(cx - r), sc(cy - r), sc(cx + r), sc(cy + r)],
              outline=color, width=sc(width))


# Anel da lente de informação.
ring(16, 16, 10.0, 2.4, dark)
ring(16, 16, 9.2, 1.6, brass)

# "i": ponto, haste e base com serifa.
d.ellipse([sc(14.9), sc(8.9), sc(17.1), sc(11.1)], fill=dark)
d.ellipse([sc(15.2), sc(9.2), sc(16.8), sc(10.8)], fill=brass)
d.rectangle([sc(15.15), sc(12.4), sc(16.85), sc(21.0)], fill=dark)
d.rectangle([sc(15.45), sc(12.7), sc(16.55), sc(20.7)], fill=brass)
d.line([sc(13.6), sc(21.2), sc(18.4), sc(21.2)], fill=dark, width=sc(1.6))
d.line([sc(13.9), sc(21.0), sc(18.1), sc(21.0)], fill=brass, width=sc(0.9))

img = img.resize((S, S), Image.LANCZOS)
img.save("assets/ui/cursor_info.png")
print("cursor_info.png OK")
