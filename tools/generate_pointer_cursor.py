"""Gera o cursor de mão agarrando ("grab") para hover sobre porto/construção.

Pose: mão vista de cima, punho entrando pela borda superior, dedos curvados
para baixo e polegar aberto — como quem pega algo do chão.

Estilo: latão (#C9A24A) com contorno escuro uniforme, igual aos demais
cursores. A silhueta é desenhada como UMA massa (punho + palma + dedos
sobrepostos + polegar); as separações dos dedos são linhas escuras por cima.
Em 32 px isso lê muito melhor do que dedos recortados.
Renderiza em 8x e reduz com LANCZOS.

Uso: python tools/generate_pointer_cursor.py
"""

from PIL import Image, ImageDraw, ImageFilter

S = 32
SS = 8  # supersampling
W = S * SS

brass = (201, 162, 74, 255)         # #C9A24A
brass_light = (224, 189, 106, 255)
dark = (12, 12, 12, 255)

mask = Image.new("L", (W, W), 0)
md = ImageDraw.Draw(mask)


def sc(v):
    return v * SS


def rrect(box, radius):
    md.rounded_rectangle([sc(v) for v in box], radius=sc(radius), fill=255)


def capsule(points, width):
    pts = [(sc(x), sc(y)) for x, y in points]
    w = int(width * SS)
    md.line(pts, fill=255, width=w, joint="curve")
    for x, y in (pts[0], pts[-1]):
        md.ellipse([x - w / 2, y - w / 2, x + w / 2, y + w / 2], fill=255)


# Punho entrando pela borda superior.
rrect((12.8, 0.0, 20.2, 9.6), 2.6)
# Palma (dorso da mão).
rrect((10.0, 6.4, 22.0, 18.2), 4.0)

# Dedos largos e SOBREPOSTOS: formam uma massa só (sem vãos recortados).
# As pontas curvam para dentro (em direção ao centro), como quem agarra.
capsule([(12.2, 16.0), (10.6, 20.4), (10.4, 24.4), (11.6, 26.6)], 4.4)
capsule([(14.9, 17.0), (14.2, 21.2), (14.4, 25.6), (15.4, 27.8)], 4.4)
capsule([(17.4, 17.0), (18.2, 21.2), (18.2, 25.4), (17.4, 27.6)], 4.4)
capsule([(19.9, 16.0), (21.3, 19.4), (21.6, 22.6), (20.6, 24.8)], 4.2)

# Polegar grosso, colado à palma (a dobra entre os dois vira um vinco escuro).
capsule([(12.0, 10.8), (9.0, 12.2), (6.8, 14.8), (6.6, 17.8)], 5.2)

# Contorno: máscara cheia em escuro; interior em latão = máscara erodida.
inner = mask.filter(ImageFilter.MinFilter(9))

img = Image.new("RGBA", (W, W), (0, 0, 0, 0))
dark_layer = Image.new("RGBA", (W, W), dark)
dark_layer.putalpha(mask)
brass_layer = Image.new("RGBA", (W, W), brass)
brass_layer.putalpha(inner)
img = Image.alpha_composite(dark_layer, brass_layer)

d = ImageDraw.Draw(img)

# Vinco do punho (curto, só no meio do dorso).
d.line([sc(14.2), sc(8.2), sc(19.0), sc(8.2)], fill=dark, width=int(0.7 * SS))

# Separações dos dedos: linhas curtas do dorso até a ponta.
d.line([sc(12.9), sc(19.8), sc(13.0), sc(23.6), sc(13.4), sc(25.8)],
       fill=dark, width=int(0.85 * SS), joint="curve")
d.line([sc(16.3), sc(20.2), sc(16.4), sc(24.2), sc(16.6), sc(26.8)],
       fill=dark, width=int(0.85 * SS), joint="curve")
d.line([sc(19.7), sc(19.8), sc(19.8), sc(23.0), sc(19.6), sc(25.2)],
       fill=dark, width=int(0.85 * SS), joint="curve")

# Vinco do polegar (separa do dorso).
d.line([sc(10.2), sc(12.2), sc(9.2), sc(15.2)], fill=dark,
       width=int(0.85 * SS), joint="curve")

# Reflexo no dorso da mão.
d.line([sc(12.2), sc(9.0), sc(12.2), sc(12.4)], fill=brass_light, width=SS)

img = img.resize((S, S), Image.LANCZOS)
img.save("assets/ui/cursor_pointer.png")
print("cursor_pointer.png OK")
