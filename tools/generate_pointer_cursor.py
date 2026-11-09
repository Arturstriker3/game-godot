"""Gera o cursor de punho (latão) para hover sobre porto/construção.

Pose: punho fechado apontando para a esquerda, braço saindo pelo canto
inferior direito — a mesma diagonal da lupa (lente no alto-esquerda, cabo
embaixo-direita) e da seta.

Estilo: latão (#C9A24A) com contorno escuro uniforme, igual aos demais
cursores. Para ler em 32 px: uma massa sólida, sem detalhes finos — só as
quatro pontas dos dedos na borda esquerda, o vinco do polegar e um reflexo.

Uso: python tools/generate_pointer_cursor.py
"""

from PIL import Image, ImageChops, ImageDraw, ImageFilter

S = 32
SS = 8  # supersampling
W = S * SS

brass = (201, 162, 74, 255)         # #C9A24A
brass_light = (224, 189, 106, 255)
dark = (12, 12, 12, 255)

mask = Image.new("L", (W, W), 0)
md = ImageDraw.Draw(mask)


def sc(v):
    return int(round(v * SS))


def rrect(box, radius, target=md):
    target.rounded_rectangle([sc(v) for v in box], radius=sc(radius), fill=255)


def capsule(points, width, target=md):
    pts = [(sc(x), sc(y)) for x, y in points]
    w = sc(width)
    target.line(pts, fill=255, width=w, joint="curve")
    for x, y in (pts[0], pts[-1]):
        target.ellipse([x - w / 2, y - w / 2, x + w / 2, y + w / 2], fill=255)


# --- Silhueta: punho apontando para a esquerda ---
# Braço saindo para o canto inferior direito.
capsule([(18.5, 16.0), (30.0, 26.0)], 6.0)
# Massa do punho.
rrect((7.6, 8.0, 20.5, 22.0), 5.5)
# Quatro pontas de dedo dobradas na borda esquerda.
for cy in (10.6, 13.6, 16.6, 19.6):
    rrect((4.0, cy - 1.75, 9.2, cy + 1.75), 1.75)
# Polegar no alto-esquerda.
rrect((7.8, 5.0, 14.4, 9.8), 2.3)

# --- Detalhes (camada separada, recortada pela silhueta) ---
detail = Image.new("RGBA", (W, W), (0, 0, 0, 0))
dd = ImageDraw.Draw(detail)

# Separações entre os dedos (curtas, na borda esquerda).
for y in (12.1, 15.1, 18.1):
    dd.line([sc(4.5), sc(y), sc(8.8), sc(y)], fill=dark, width=sc(0.75))

# Vinco do polegar.
dd.line([sc(8.6), sc(9.4), sc(14.0), sc(9.7)], fill=dark, width=sc(0.75))

# Reflexo no dorso (um traço claro só).
dd.line([sc(9.0), sc(8.9), sc(12.6), sc(8.9)], fill=brass_light, width=sc(0.9))

# Contorno: interior em latão = máscara erodida.
inner = mask.filter(ImageFilter.MinFilter(9))

img = Image.new("RGBA", (W, W), (0, 0, 0, 0))
dark_layer = Image.new("RGBA", (W, W), dark)
dark_layer.putalpha(mask)
brass_layer = Image.new("RGBA", (W, W), brass)
brass_layer.putalpha(inner)
img = Image.alpha_composite(img, dark_layer)
img = Image.alpha_composite(img, brass_layer)

detail.putalpha(ImageChops.multiply(detail.getchannel("A"), mask))
img = Image.alpha_composite(img, detail)

img = img.resize((S, S), Image.LANCZOS)
img.save("assets/ui/cursor_pointer.png")
print("cursor_pointer.png OK")
