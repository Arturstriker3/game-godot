"""Gera ícones de produto/recurso em SVG, só com código.

Estilo: viewBox 32x32, traço 1.8, cantos redondos, branco (tingido no Godot),
com áreas preenchidas para dar peso/detalhe. Sem dependências e sem licença.

Uso:
    <python> tools/generate_product_icons.py
Saída:
    assets/icons/products/<nome>.svg
"""

import math
import os

OUT = os.path.join("assets", "icons", "products")
STROKE = (
    'stroke="#FFFFFF" stroke-width="1.8" fill="none" '
    'stroke-linecap="round" stroke-linejoin="round"'
)
FILL = 'fill="#FFFFFF" stroke="none"'


def svg(body: str) -> str:
    return (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32" '
        'width="32" height="32">\n' + body.strip() + "\n</svg>\n"
    )


def p(d: str) -> str:
    return f'  <path d="{d}" {STROKE}/>'


def f(d: str) -> str:
    return f'  <path d="{d}" {FILL}/>'


def circle(cx: float, cy: float, r: float) -> str:
    return f'  <circle cx="{cx}" cy="{cy}" r="{r}" {STROKE}/>'


def dot(cx: float, cy: float, r: float = 0.9) -> str:
    return f'  <circle cx="{cx}" cy="{cy}" r="{r}" {FILL}/>'


ICONS = {}

# ---------------------------------------------------------------- trigo
parts = [p("M16 30V5")]
for y in (9, 13, 17, 21):
    parts.append(f(f"M16 {y}C13.4 {y - 0.6} 11.7 {y - 2.5} 12.3 {y - 4.9}"
                   f"C14.7 {y - 4.4} 16 {y - 2.5} 16 {y}Z"))
    parts.append(f(f"M16 {y}C18.6 {y - 0.6} 20.3 {y - 2.5} 19.7 {y - 4.9}"
                   f"C17.3 {y - 4.4} 16 {y - 2.5} 16 {y}Z"))
parts += [
    f("M16 23C12 22 9 24 7.5 28C11.5 28.5 14.5 26.5 16 23Z"),
    f("M16 23C20 22 23 24 24.5 28C20.5 28.5 17.5 26.5 16 23Z"),
    f("M16 5C14.2 3.4 13.8 1.8 16 1.2C18.2 1.8 17.8 3.4 16 5Z"),
    p("M16 5L13.5 1.5"),
    p("M16 5L18.5 1.5"),
    p("M16 5V0.8"),
]
ICONS["wheat"] = svg("\n".join(parts))

# ---------------------------------------------------------------- farinha
ICONS["flour"] = svg("\n".join([
    p("M9 11H23L25 26C25.2 27.4 24 29 22.5 29H9.5C8 29 6.8 27.4 7 26Z"),
    p("M11 11L16 7L21 11"),
    dot(16, 6.2, 1.3),
    p("M12 11C13 13 15 13 16 11"),
    p("M16 11C17 13 19 13 20 11"),
    p("M12 18H20V23H12Z"),
    p("M16 19V22"),
    p("M16 19C15 19 14.5 20 15 20.8"),
    p("M16 19C17 19 17.5 20 17 20.8"),
    p("M8.5 26H23.5"),
]))
ICONS["flour"] = ICONS["flour"]

# ---------------------------------------------------------------- pão
ICONS["bread"] = svg("\n".join([
    p("M5 20C5 13 10 10 16 10C22 10 27 13 27 20V24C27 25.5 25.8 27 24 27H8C6.2 27 5 25.5 5 24Z"),
    p("M9 19C11 16 21 16 23 19C21 22 11 22 9 19Z"),
    dot(12, 18.6, 0.7),
    dot(16, 18, 0.7),
    dot(20, 18.6, 0.7),
    p("M8 12L10 15"),
    p("M12 10.5L13 14"),
    p("M16 10V14"),
    p("M20 10.5L19 14"),
    p("M24 12L22 15"),
    p("M7 24H25"),
]))

# ---------------------------------------------------------------- ferro
ICONS["iron"] = svg("\n".join([
    p("M5 18L9 13H23L27 18Z"),
    p("M5 18H27L23 28H9Z"),
    p("M9 18L12 28"),
    p("M23 18L20 28"),
    p("M7 20H12"),
    p("M17 25H22"),
]))

# ---------------------------------------------------------------- fábrica
ICONS["factory"] = svg("\n".join([
    p("M4 28V18L8 15V18L12 15V18L16 15V18L20 15V18L24 15V28Z"),
    p("M21 15V7H25V15"),
    p("M23 5C24.5 4 24.5 2.5 23 1.5"),
    p("M21 4C22 3 22 2 21 1"),
    f("M6.5 20H9.5V23H6.5Z"),
    f("M11.5 20H14.5V23H11.5Z"),
    f("M16.5 20H19.5V23H16.5Z"),
    p("M12 28V23H16V28"),
    p("M4 18H24"),
]))

# ---------------------------------------------------------------- caixa
ICONS["crate"] = svg("\n".join([
    p("M5 12H23V28H5Z"),
    p("M5 12L11 6H29L23 12"),
    p("M23 12L29 6V22L23 28"),
    p("M9 12V28"),
    p("M13 12V28"),
    p("M17 12V28"),
    dot(7, 14, 0.8),
    dot(21, 14, 0.8),
    dot(7, 26, 0.8),
    dot(21, 26, 0.8),
]))

# ---------------------------------------------------------------- moeda
coin = [circle(16, 16, 11), circle(16, 16, 8)]
for i in range(16):
    a = i * math.pi / 8.0
    x1 = 16 + math.cos(a) * 10.1
    y1 = 16 + math.sin(a) * 10.1
    x2 = 16 + math.cos(a) * 11.8
    y2 = 16 + math.sin(a) * 11.8
    coin.append(p(f"M{x1:.2f} {y1:.2f}L{x2:.2f} {y2:.2f}"))
coin.append(p("M16 10L17.6 14L22 14L18.6 16.6L19.8 21L16 18.6L12.2 21L13.4 16.6L10 14L14.4 14Z"))
ICONS["coin"] = svg("\n".join(coin))

# ---------------------------------------------------------------- óleo
ICONS["oil"] = svg("\n".join([
    p("M9 13H23V27H9Z"),
    p("M9 17H23"),
    p("M9 23H23"),
    p("M12 13V27"),
    p("M16 13V27"),
    p("M20 13V27"),
    f("M16 3C18 6 19 7.5 19 9A3 3 0 1 1 13 9C13 7.5 14 6 16 3Z"),
    p("M8 29H24"),
]))

# ---------------------------------------------------------------- espada
ICONS["sword"] = svg("\n".join([
    p("M16 3L13 8V20H19V8Z"),
    p("M16 6V19"),
    p("M9 20H23"),
    p("M13.5 20V27H18.5V20"),
    p("M13.5 22L18.5 23.5"),
    p("M13.5 24L18.5 25.5"),
    dot(16, 28.2, 1.5),
]))

# ---------------------------------------------------------------- rifle (AK)
ICONS["rifle"] = svg("\n".join([
    # coronha de madeira
    f("M2 13.4L8 12.8L8.4 18.6L2 18Z"),
    # receptor
    p("M8 13H18V18.6H8Z"),
    # cano e tubo de gás
    p("M18 14.6H30"),
    p("M18 11.6H26"),
    # guarda-mão de madeira
    f("M18 12H24V13.4H18Z"),
    # massa de mira
    p("M27 14.6V11.2"),
    p("M26.1 11.2H27.9"),
    # alça traseira
    p("M20 11.6V10.2"),
    # boca do cano
    p("M29.4 13.7H31V15.5H29.4"),
    # pistola
    f("M9.3 18.6L12.4 18.6L11.4 24.2L8.3 23.6Z"),
    # gatilho
    p("M11 18.6C11 21.1 14 21.1 14 18.6"),
    # carregador curvo (banana)
    f("M13.8 18.6C17.1 19.6 18.3 22.6 17.4 26.5C17 27.9 14.7 27.9 14.1 26.5C13.5 23.6 13.1 21 12.7 18.6Z"),
    # porta de ejeção
    p("M14.8 14.1H17.7V16.1H14.8Z"),
]))

# ---------------------------------------------------------------- caminhão
ICONS["truck"] = svg("\n".join([
    p("M3 10H17V22H3Z"),
    p("M8 10V22"),
    p("M13 10V22"),
    p("M17 12H24L28 16V22H17Z"),
    p("M19 14H23L25 17H19Z"),
    circle(8, 24, 3),
    circle(24, 24, 3),
    dot(8, 24, 1.1),
    dot(24, 24, 1.1),
    p("M24.5 18V22"),
    p("M26.5 18V22"),
    dot(26.5, 18.5, 0.8),
]))

# ---------------------------------------------------------------- navio
ICONS["ship"] = svg("\n".join([
    p("M4 20H28L25 27H7Z"),
    p("M6 22.5H26"),
    p("M7 25H25"),
    dot(9, 24, 0.7),
    dot(13, 24, 0.7),
    dot(17, 24, 0.7),
    p("M11 20V13H18V20"),
    p("M12.5 15H16.5V17H12.5Z"),
    p("M19 20V10H22V20"),
    p("M19 13H22"),
    p("M25 20V7"),
    p("M25 7L28 8L25 9"),
    p("M20.5 8C22 7 22 5.5 20.5 4.5"),
    p("M3 30C4.5 29 6 29 7.5 30C9 31 10.5 31 12 30C13.5 29 15 29 16.5 30"),
]))

# ---------------------------------------------------------------- avião
ICONS["plane"] = svg("\n".join([
    p("M16 3C17.5 3 18 5 18 9V24C18 28 17.5 29.5 16 29.5C14.5 29.5 14 28 14 24V9C14 5 14.5 3 16 3Z"),
    p("M4 13L14 16V19L4 16Z"),
    p("M28 13L18 16V19L28 16Z"),
    p("M8 14.5L14 16"),
    p("M24 14.5L18 16"),
    p("M9 25L16 23L23 25L23 27L16 26L9 27Z"),
    p("M15 8H17V11H15Z"),
    circle(9.5, 14.6, 1.2),
    circle(22.5, 14.6, 1.2),
]))

LICENSE = """Product icons — generated procedurally by tools/generate_product_icons.py.
Original geometry, released as CC0 (public domain). No third-party assets.
"""


def main() -> None:
    os.makedirs(OUT, exist_ok=True)
    for name, content in ICONS.items():
        with open(os.path.join(OUT, f"{name}.svg"), "w", encoding="utf-8") as fh:
            fh.write(content)
        print(f"{name}.svg")
    with open(os.path.join(OUT, "LICENSE.txt"), "w", encoding="utf-8") as fh:
        fh.write(LICENSE)
    print(f"OK: {len(ICONS)} ícones em {OUT}")


if __name__ == "__main__":
    main()
