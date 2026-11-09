"""Gera ícones de produto/recurso em SVG, só com código.

Estilo único: viewBox 24x24, traço 2, cantos redondos, branco (tingido no Godot).
Sem dependências e sem licença de terceiros.

Uso:
    <python> tools/generate_product_icons.py
Saída:
    assets/icons/products/<nome>.svg
"""

import os

OUT = os.path.join("assets", "icons", "products")
STROKE = (
    'stroke="#FFFFFF" stroke-width="2" fill="none" '
    'stroke-linecap="round" stroke-linejoin="round"'
)


def svg(body: str) -> str:
    return (
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" '
        'width="24" height="24">\n' + body.strip() + "\n</svg>\n"
    )


def path(d: str) -> str:
    return f'  <path d="{d}" {STROKE}/>'


def circle(cx: float, cy: float, r: float) -> str:
    return f'  <circle cx="{cx}" cy="{cy}" r="{r}" {STROKE}/>'


ICONS = {}

# ---------------------------------------------------------------- trigo
wheat = [path("M12 21V4")]
for y in (7, 10, 13, 16):
    wheat.append(path(f"M12 {y}C9.6 {y} 8.2 {y - 1.6} 8 {y - 3.2}C10.6 {y - 3.2} 12 {y - 1.8} 12 {y}"))
    wheat.append(path(f"M12 {y}C14.4 {y} 15.8 {y - 1.6} 16 {y - 3.2}C13.4 {y - 3.2} 12 {y - 1.8} 12 {y}"))
wheat.append(path("M12 4C10.6 2.8 10.2 1.6 12 1C13.8 1.6 13.4 2.8 12 4Z"))
ICONS["wheat"] = svg("\n".join(wheat))

# ---------------------------------------------------------------- farinha
ICONS["flour"] = svg("\n".join([
    path("M8 8H16L18 18C18.2 19.1 17.3 20 16.2 20H7.8C6.7 20 5.8 19.1 6 18Z"),
    path("M9 8L12 4.8L15 8"),
    path("M9.5 13H14.5"),
]))

# ---------------------------------------------------------------- pão
ICONS["bread"] = svg("\n".join([
    path("M3 15C3 10.5 7 8 12 8C17 8 21 10.5 21 15V17C21 18.1 20.1 19 19 19H5C3.9 19 3 18.1 3 17Z"),
    path("M8 11.5L9.2 14.5"),
    path("M12 10.8V14.5"),
    path("M16 11.5L14.8 14.5"),
]))

# ---------------------------------------------------------------- ferro
ICONS["iron"] = svg("\n".join([
    path("M4 14H20L17 20H7Z"),
    path("M8 17H12"),
]))

# ---------------------------------------------------------------- fábrica
ICONS["factory"] = svg("\n".join([
    path("M3 20V13L7 10V13L11 10V13L15 10V13L19 10V20Z"),
    path("M17 10V4H20V10"),
    path("M6 20V16.5H9V20"),
]))

# ---------------------------------------------------------------- caixa
ICONS["crate"] = svg("\n".join([
    path("M4 6H20V20H4Z"),
    path("M4 6L20 20"),
    path("M20 6L4 20"),
]))

# ---------------------------------------------------------------- moeda
ICONS["coin"] = svg("\n".join([
    circle(12, 12, 8.5),
    circle(12, 12, 5.5),
    path("M12 8.5V15.5"),
]))

# ---------------------------------------------------------------- óleo
ICONS["oil"] = svg("\n".join([
    path("M12 2.5C15 7 17 9 17 12.5A5 5 0 1 1 7 12.5C7 9 9 7 12 2.5Z"),
    path("M10.2 12.8C10.6 14.6 11.1 15.5 12.4 16.1"),
]))

# ---------------------------------------------------------------- espada
ICONS["sword"] = svg("\n".join([
    path("M12 2L10.6 4.8H13.4Z"),
    path("M12 2V14"),
    path("M7.5 14H16.5"),
    path("M10.5 14V21H13.5V14"),
]))

# ---------------------------------------------------------------- rifle
ICONS["rifle"] = svg("\n".join([
    # AK simples: coronha, receptor, cano, guarda-mao, carregador curvo, mira.
    path("M1.5 10L6 9.2V14L1.5 13.2Z"),
    path("M6 9H13V14H6Z"),
    path("M13 11H22"),
    path("M13 9H17V11H13Z"),
    path("M10 14C12 15 13 17 12.5 20C12.3 21 10.8 21 10.5 20C10.2 17.5 9.8 15.5 9.5 14Z"),
    path("M19 11V8.5"),
    path("M21 10.5H23V12H21"),
    path("M7 14L9 14L8.2 18L6.2 17.5Z"),
    path("M8 14C8 16 10.5 16 10.5 14"),
]))

# ---------------------------------------------------------------- caminhão
ICONS["truck"] = svg("\n".join([
    path("M2.5 7.5H13.5V16H2.5Z"),
    path("M13.5 9.5H18L21 12.5V16H13.5Z"),
    circle(7, 18, 2),
    circle(17, 18, 2),
]))

# ---------------------------------------------------------------- navio
ICONS["ship"] = svg("\n".join([
    path("M4.5 15H19.5L17.5 19H6.5Z"),
    path("M9 15V7"),
    path("M15 15V9.5"),
    path("M9 7L12 8L9 9"),
    path("M3 21.5C4.2 20.5 5.4 20.5 6.6 21.5C7.8 22.5 9 22.5 10.2 21.5C11.4 20.5 12.6 20.5 13.8 21.5C15 22.5 16.2 22.5 17.4 21.5C18.6 20.5 19.8 20.5 21 21.5"),
]))

# ---------------------------------------------------------------- avião
ICONS["plane"] = svg("\n".join([
    path("M12 2.5V21.5"),
    path("M4 9L12 12L20 9"),
    path("M8 19L12 17L16 19"),
]))

LICENSE = """Product icons — generated procedurally by tools/generate_product_icons.py.
Original geometry, released as CC0 (public domain). No third-party assets.
"""


def main() -> None:
    os.makedirs(OUT, exist_ok=True)
    for name, content in ICONS.items():
        with open(os.path.join(OUT, f"{name}.svg"), "w", encoding="utf-8") as f:
            f.write(content)
        print(f"{name}.svg")
    with open(os.path.join(OUT, "LICENSE.txt"), "w", encoding="utf-8") as f:
        f.write(LICENSE)
    print(f"OK: {len(ICONS)} ícones em {OUT}")


if __name__ == "__main__":
    main()
