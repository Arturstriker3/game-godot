"""Gera o cursor de mão (latão) para hover sobre porto/construção.

Mesmo estilo do cursor de lupa (tools/generate_loupe_cursor.py): silhueta
gravada em latão (#C9A24A) com contorno escuro uniforme, hotspot na ponta
do dedo. Renderiza em 8x e reduz com LANCZOS para suavizar as bordas.

A silhueta é uma máscara (união das formas); o latão é a máscara erodida,
deixando o contorno escuro aparecer em toda a volta.

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


def poly(points):
    md.polygon([(sc(x), sc(y)) for x, y in points], fill=255)


# Silhueta: dedo indicador, dobras dos dedos, polegar, palma e punho.
rrect((7.0, 1.5, 11.5, 16.0), 1.6)        # indicador
rrect((12.6, 10.6, 15.8, 16.0), 1.2)      # dobra 1
rrect((16.2, 11.6, 19.4, 16.0), 1.2)      # dobra 2
poly([(19.2, 12.5), (23.5, 13.0), (26.0, 15.0), (26.0, 18.0),
      (23.5, 20.0), (20.0, 20.0), (19.2, 18.0)])  # polegar
rrect((6.5, 13.0, 21.5, 24.0), 2.0)       # palma
rrect((9.0, 23.8, 20.0, 29.3), 1.2)       # punho

# Contorno: máscara cheia em escuro; interior em latão = máscara erodida ~1,6 px.
inner = mask.filter(ImageFilter.MinFilter(13))

img = Image.new("RGBA", (W, W), (0, 0, 0, 0))
dark_layer = Image.new("RGBA", (W, W), dark)
dark_layer.putalpha(mask)
brass_layer = Image.new("RGBA", (W, W), brass)
brass_layer.putalpha(inner)
img = Image.alpha_composite(dark_layer, brass_layer)

d = ImageDraw.Draw(img)

# Dobras gravadas dos dedos (linhas escuras curtas, dentro da palma).
d.line([sc(15.9), sc(13.4), sc(15.9), sc(22.2)], fill=dark, width=int(0.9 * SS))
d.line([sc(19.0), sc(14.2), sc(19.0), sc(21.6)], fill=dark, width=int(0.9 * SS))

# Separação do punho.
d.line([sc(9.8), sc(24.3), sc(19.2), sc(24.3)], fill=dark, width=int(0.9 * SS))

# Reflexo sutil no dedo indicador.
d.line([sc(8.4), sc(3.6), sc(8.4), sc(12.4)], fill=brass_light, width=SS)

img = img.resize((S, S), Image.LANCZOS)
img.save("assets/ui/cursor_pointer.png")
print("cursor_pointer.png OK")
