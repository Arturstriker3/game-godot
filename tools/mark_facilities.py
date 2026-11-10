"""Marca registros de instalação (fundição/refinaria/usina) no deposits.json com
`"facility": true`, para que o mapa de recursos e o acesso das cidades os
ignorem — não são depósitos minerais, são plantas de processamento.

Idempotente: reaplicar não altera nada (e remove a flag de registros que
deixaram de casar a denylist).

Por que pelo NOME e não pelo status? O USGS MRDS rotula "sites" que são plantas
de processamento, mas o campo `status:"Plant"` não é confiável: mistura
fundições com minas reais (Batu Hijau, Boddington, Kanmantoo, Monywa...).
Então a identificação usa:

- palavras-chave no nome (inglês): facility, refinery, smelter, concentrator,
  works, metallurgical, processing, foundry;
- nomes curados de fundições sem palavra-chave: Rönnskär, Avonmouth ISF, Budel,
  Crotone, Topionice (Bor), Iwami (Mitsui), Oskaloosa.

Pipeline (depois de regenerar deposits.json):
  python tools/build_natural_geography.py
  python tools/apply_deposit_supplement.py
  python tools/mark_facilities.py
  python tools/build_resource_map.py
"""

import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEPOSITS = os.path.join(ROOT, "data", "definitions", "natural", "deposits.json")

FACILITY_KEYWORDS = [
    "facility", "refinery", "smelter", "concentrator",
    "works", "metallurgical", "processing", "foundry",
]
FACILITY_EXTRA = [
    "ronnskar", "avonmouth", "budel", "crotone",
    "topionice", "iwami", "oskaloosa",
]


def is_facility(name: str) -> bool:
    lower = str(name).lower()
    if any(keyword in lower for keyword in FACILITY_KEYWORDS):
        return True
    return any(token in lower for token in FACILITY_EXTRA)


def main() -> int:
    with open(DEPOSITS, encoding="utf-8") as handle:
        data = json.load(handle)

    marked = 0
    cleared = 0
    for deposit in data["deposits"]:
        flag = is_facility(deposit.get("names", [""])[0])
        if flag:
            if not deposit.get("facility"):
                deposit["facility"] = True
                marked += 1
        elif deposit.get("facility"):
            deposit.pop("facility", None)
            cleared += 1

    # Preserva o formato atual do arquivo (indent=1, CRLF, sem LF final).
    text = json.dumps(data, indent=1, ensure_ascii=False).replace("\n", "\r\n")
    with open(DEPOSITS, "w", encoding="utf-8", newline="") as handle:
        handle.write(text)

    print("marcados como facility: %d | flag removida: %d | total: %d" % (
        marked, cleared, len(data["deposits"])))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
