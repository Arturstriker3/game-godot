"""Gera o cursor de lupa (latão) para hover sobre construção no mapa.

Substitui a antiga silhueta de cidade. Estilo HOI3: anel + cabo em latão
(#C9A24A) com contorno escuro, hotspot no centro (16,16).
"""

import math

from PIL import Image, ImageDraw

S = 32
img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)

brass = (201, 162, 74, 255)        # #C9A24A
brass_light = (224, 189, 106, 255)
dark = (12, 12, 12, 255)

cx, cy, r = 15.0, 15.0, 8.0

# Anel da lente (contorno escuro + aro de latão).
d.ellipse([cx - r - 1.5, cy - r - 1.5, cx + r + 1.5, cy + r + 1.5], outline=dark, width=2)
d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=brass, width=2)

# Reflexo sutil no vidro.
d.arc([cx - r + 2, cy - r + 2, cx + r - 2, cy + r - 2], 210, 300, fill=brass_light, width=1)

# Cabo, saindo da lente a 45° até o canto inferior direito.
a = math.radians(45)
hx = cx + (r + 1) * math.cos(a)
hy = cy + (r + 1) * math.sin(a)
d.line([hx, hy, 29.5, 29.5], fill=dark, width=6)
d.line([hx, hy, 29.5, 29.5], fill=brass, width=3)

img.save("assets/ui/cursor_loupe.png")
print("cursor_loupe.png OK")
