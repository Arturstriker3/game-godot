"""Busca ícones de lugar (cidade/porto) no game-icons.net via Iconify.

Grava SVG branco (tingível) em assets/icons/places/. Mesma licença CC BY 3.0.
"""

import json
import os
import urllib.request

OUT = os.path.join("assets", "icons", "places")
ICONS = {"city": "modern-city", "port": "harbor-dock"}
URL = "https://api.iconify.design/game-icons.json?icons=" + ",".join(ICONS.values())


def main() -> None:
    os.makedirs(OUT, exist_ok=True)
    req = urllib.request.Request(URL, headers={"User-Agent": "opengs-icon-fetch"})
    with urllib.request.urlopen(req, timeout=30) as resp:
        data = json.load(resp)

    for name, icon in ICONS.items():
        body = data["icons"][icon]["body"].replace('fill="currentColor"', 'fill="#FFFFFF"')
        svg = (
            '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512" '
            f'width="512" height="512">{body}</svg>\n'
        )
        with open(os.path.join(OUT, f"{name}.svg"), "w", encoding="utf-8") as fh:
            fh.write(svg)
        print(f"{name} <- {icon}")

    print("OK")


if __name__ == "__main__":
    main()
