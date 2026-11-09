"""Baixa os ícones de produto do game-icons.net (CC BY 3.0) via Iconify.

Grava cada ícone como SVG 512x512 com preenchimento branco (tingível no Godot
via modulate). Gera CREDITS.txt com a atribuição.

Uso:
    <python> tools/fetch_game_icons.py
Saída:
    assets/icons/products/<nome>.svg + CREDITS.txt
"""

import json
import os
import urllib.request

OUT = os.path.join("assets", "icons", "products")

# produto -> id do game-icons.net
ICONS = {
    "wheat": "wheat",
    "flour": "flour",
    "bread": "bread",
    "iron": "metal-bar",
    "factory": "factory",
    "crate": "cargo-crate",
    "coin": "coins",
    "oil": "oil-drum",
    "sword": "crossed-swords",
    "rifle": "ak47",
    "truck": "truck",
    "ship": "cargo-ship",
    "plane": "jet-fighter",
}

URL = "https://api.iconify.design/game-icons.json?icons=" + ",".join(ICONS.values())

CREDITS = (
    "Ícones: game-icons.net (https://game-icons.net) — CC BY 3.0.\n"
    "Cada ícone é de seu autor no game-icons.net (ver a página do ícone para o\n"
    "crédito individual). Recoloridos para branco para uso tingível no jogo.\n"
)


def main() -> None:
    os.makedirs(OUT, exist_ok=True)
    req = urllib.request.Request(URL, headers={"User-Agent": "opengs-icon-fetch"})
    with urllib.request.urlopen(req, timeout=30) as resp:
        data = json.load(resp)

    icons = data["icons"]
    missing = data.get("not_found", [])

    for name, icon in ICONS.items():
        if icon in missing:
            print(f"FALTOU {name} <- {icon}")
            continue
        body = icons[icon]["body"].replace('fill="currentColor"', 'fill="#FFFFFF"')
        svg = (
            '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512" '
            f'width="512" height="512">{body}</svg>\n'
        )
        with open(os.path.join(OUT, f"{name}.svg"), "w", encoding="utf-8") as fh:
            fh.write(svg)
        print(f"{name} <- {icon}")

    with open(os.path.join(OUT, "CREDITS.txt"), "w", encoding="utf-8") as fh:
        fh.write(CREDITS)
    if missing:
        print("MISSING:", missing)
    print("OK")


if __name__ == "__main__":
    main()
