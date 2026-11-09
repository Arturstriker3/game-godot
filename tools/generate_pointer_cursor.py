"""Gera o cursor de punho (latão) para hover sobre porto/construção.

Pose: punho fechado vindo de cima, nós dos dedos embaixo — como quem agarra
algo no chão. Estilo gravado dos demais cursores: latão (#C9A24A) com
contorno escuro uniforme.

Regra de legibilidade em 32 px: UMA massa sólida, sem detalhes finos.
Só o vinco do polegar e três separações curtas entre os nós dos dedos.
Renderiza em 8x e reduz com LANCZOS.

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


# --- Silhueta: punho sólido, braço entrando por cima ---
# Antebraço (sai pela borda superior).
rrect((12.6, -1.0, 19.4, 7.0), 1.6)
# Massa do punho.
rrect((8.5, 5.5, 23.5, 23.5), 6.0)
# Nós dos dedos embaixo (quatro protuberâncias rasas).
for cx in (10.9, 14.4, 17.9, 21.2):
    rrect((cx - 1.9, 19.8, cx + 1.9, 25.0), 1.85)
# Polegar à esquerda.
rrect((5.9, 9.0, 12.8, 17.0), 3.0)

# --- Detalhes (camada separada, recortada pela silhueta) ---
detail = Image.new("RGBA", (W, W), (0, 0, 0, 0))
dd = ImageDraw.Draw(detail)

# Separações entre os nós dos dedos (curtas, só embaixo).
for x in (12.6, 16.1, 19.6):
    dd.line([sc(x), sc(20.2), sc(x), sc(22.9)], fill=dark, width=sc(0.75))

# Vinco do polegar (borda direita do polegar).
dd.line([sc(12.9), sc(10.0), sc(13.2), sc(13.4), sc(12.9), sc(16.8)],
        fill=dark, width=sc(0.75), joint="curve")

# Reflexo no dorso (um traço claro só).
dd.line([sc(10.8), sc(8.2), sc(10.8), sc(11.6)], fill=brass_light, width=sc(0.9))

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
