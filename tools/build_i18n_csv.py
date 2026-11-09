"""Extrai o dicionário STRINGS do i18n.gd para um CSV editável.

Uso único (seed): gera data/definitions/i18n/i18n.csv a partir do STRINGS
hardcoded. Depois o CSV vira a fonte e build_scenario_data.py gera o JSON.

Formato do CSV: key,pt_BR,en,es
"""

import ast
import csv
import os
import re

SRC = os.path.join("core", "i18n", "i18n.gd")
OUT_DIR = os.path.join("data", "definitions", "i18n")
OUT = os.path.join(OUT_DIR, "i18n.csv")

LOCALES = ["pt_BR", "en", "es"]


def main() -> None:
    with open(SRC, encoding="utf-8") as fh:
        src = fh.read()

    start = src.index("const STRINGS :=")
    brace = src.index("{", start)
    depth = 0
    end = brace
    for i in range(brace, len(src)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                end = i + 1
                break

    data = ast.literal_eval(src[brace:end])  # {locale: {key: value}}

    keys: set[str] = set()
    for loc in LOCALES:
        keys.update(data.get(loc, {}).keys())
    keys = sorted(keys)

    os.makedirs(OUT_DIR, exist_ok=True)
    with open(OUT, "w", encoding="utf-8", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["key", "pt_BR", "en", "es"])
        for key in keys:
            w.writerow([
                key,
                data.get("pt_BR", {}).get(key, ""),
                data.get("en", {}).get(key, ""),
                data.get("es", {}).get(key, ""),
            ])

    print(f"{len(keys)} chaves -> {OUT}")


if __name__ == "__main__":
    main()
