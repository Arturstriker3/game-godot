#!/usr/bin/env python
# Baixa o ratio real de capital bancario (World Bank WDI FB.BNK.CAPA.ZS,
# "Bank capital to assets ratio (%)") e grava o ultimo ano por pais.
#
# Fonte (citada no bank_seed.json gerado):
#   https://api.worldbank.org/v2/country/all/indicator/FB.BNK.CAPA.ZS?format=json&date=2015:2024&per_page=20000
#
# Uso:
#   python tools/fetch_bank_capital_wdi.py --out G:/opengs_tmp/r2/wdi_fb_bnk_capa.json
import argparse
import json
import urllib.request

URL = ("https://api.worldbank.org/v2/country/all/indicator/"
       "FB.BNK.CAPA.ZS?format=json&date=2015:2024&per_page=20000")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    with urllib.request.urlopen(URL, timeout=120) as response:
        data = json.load(response)
    latest = {}
    for row in data[1]:
        value = row.get("value")
        code = row.get("countryiso3code") or ""
        if value is None or not code:
            continue
        year = int(row["date"])
        if code not in latest or year > latest[code]["year"]:
            latest[code] = {"year": year, "value": value}
    with open(args.out, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(latest, handle, ensure_ascii=False, indent=1, sort_keys=True)
        handle.write("\n")
    print("escrito %s (%d paises, atualizado %s)"
          % (args.out, len(latest), data[0].get("lastupdated")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
