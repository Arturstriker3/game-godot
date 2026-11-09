"""Gera o círculo (anel metálico) que envolve o ícone de construção."""

from PIL import Image, ImageDraw

S = 512
img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)

# Anel de latão (#C9A24A) com contorno escuro, centro transparente (o ícone fica dentro).
brass = (201, 162, 74, 255)
dark = (14, 20, 19, 255)
cx = cy = S / 2
r_outer = 248
r_inner = 228

d.ellipse([cx - r_outer, cy - r_outer, cx + r_outer, cy + r_outer], outline=dark, width=8)
d.ellipse([cx - r_outer + 4, cy - r_outer + 4, cx + r_outer - 4, cy + r_outer - 4], outline=brass, width=16)

img.save("assets/icons/places/ring.png")
print("ring.png OK")
