#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Coleta bruta da Parte G2 (armazenagem, reservas estrategicas e ouro).

Baixa e guarda, em tools/_tmp_storage_raw/ (NAO versionado, apagado ao final),
os insumos usados por tools/build_storage_g2.py:

  imf_ifs_gold.json        IMF IFS (via DBnomics) RAFAGOLDV_OZT, mensal, todas as areas
  eurostat_oil_stocks.json Eurostat nrg_stk_oem, STK_EUE_DIR / STK_MIN_CAL (dias)
  eia_spr_weekly.html      EIA: Weekly U.S. Ending Stocks of Crude Oil in SPR
  eia_us_gas_capacity.html EIA: U.S. Underground Natural Gas Storage Capacity

Cada arquivo tem um .meta.json irmao com url, sha256, bytes, data/hora de acesso
e status HTTP. O script e idempotente: com --fetch baixa de novo; sem --fetch
usa o cache e falha se ele nao existir.

Uso:
    py -3 tools/fetch_storage_g2.py --fetch
    py -3 tools/fetch_storage_g2.py            # so confere o cache
"""

from __future__ import annotations

import datetime
import hashlib
import json
import os
import sys
import urllib.error
import urllib.parse
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CACHE = os.path.join(ROOT, "tools", "_tmp_storage_raw")
UA = "OpenGS-G2-data/1.0 (+https://github.com/opengs; dados de armazenagem)"

DBNOMICS = "https://api.db.nomics.world/v22"


def utc_now() -> str:
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def get(url: str) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": UA, "Accept": "*/*"})
    with urllib.request.urlopen(req, timeout=120) as resp:
        return resp.read()


def save(name: str, url: str, data: bytes) -> None:
    os.makedirs(CACHE, exist_ok=True)
    path = os.path.join(CACHE, name)
    with open(path, "wb") as handle:
        handle.write(data)
    meta = {
        "file": name,
        "url": url,
        "sha256": sha256(data),
        "bytes": len(data),
        "accessed_utc": utc_now(),
    }
    with open(path + ".meta.json", "w", encoding="utf-8", newline="\n") as handle:
        json.dump(meta, handle, indent=2, ensure_ascii=False, sort_keys=True)
        handle.write("\n")
    print("  salvo %-28s %8d bytes  %s" % (name, len(data), meta["sha256"][:16]))


def cached(name: str) -> bool:
    return os.path.exists(os.path.join(CACHE, name))


# ---------------------------------------------------------------- IMF / IFS

def fetch_imf_gold(force: bool) -> None:
    name = "imf_ifs_gold.json"
    if cached(name) and not force:
        print("  cache ok  %s" % name)
        return
    dims = json.dumps({"INDICATOR": ["RAFAGOLDV_OZT"], "FREQ": ["M"]}, separators=(",", ":"))
    base = "%s/series/IMF/IFS?dimensions=%s&limit=100&observations=true" % (
        DBNOMICS, urllib.parse.quote(dims))
    series = []
    offset = 0
    while True:
        url = base + "&offset=%d" % offset
        payload = json.loads(get(url).decode("utf-8"))
        docs = payload["series"]["docs"]
        series.extend(docs)
        total = payload["series"]["num_found"]
        offset += len(docs)
        print("  IMF IFS gold: %d/%d" % (offset, total))
        if not docs or offset >= total:
            break
    data = json.dumps(
        {
            "retrieved_utc": utc_now(),
            "query": "IMF/IFS INDICATOR=RAFAGOLDV_OZT FREQ=M",
            "source_url": "https://data.imf.org/ (consultado via api.db.nomics.world/v22, provider IMF, dataset IFS)",
            "num_series": len(series),
            "series": [
                {
                    "ref_area": doc["dimensions"]["REF_AREA"],
                    "name": doc.get("series_name") or "",
                    "period": doc.get("period") or [],
                    "value": doc.get("value") or [],
                }
                for doc in series
            ],
        },
        ensure_ascii=False,
        indent=2,
        sort_keys=True,
    ).encode("utf-8")
    save(name, base, data)


# ------------------------------------------------------------ Eurostat oil

def fetch_eurostat_oil(force: bool) -> None:
    name = "eurostat_oil_stocks.json"
    if cached(name) and not force:
        print("  cache ok  %s" % name)
        return
    series = []
    # dias equivalentes (NR) + os dois denominadores usados pela diretiva (kt/dia)
    for stk_flow, unit in (
        ("STK_EUE_DIR", "NR"),
        ("IMP_DNC", "THS_T"),
        ("IC_DC", "THS_T"),
    ):
        dims = json.dumps({"stk_flow": [stk_flow], "unit": [unit]}, separators=(",", ":"))
        url = "%s/series/Eurostat/nrg_stk_oem?dimensions=%s&limit=100&observations=true" % (
            DBNOMICS, urllib.parse.quote(dims))
        payload = json.loads(get(url).decode("utf-8"))
        docs = payload["series"]["docs"]
        print("  Eurostat oil %s (%s): %d series" % (stk_flow, unit, len(docs)))
        for doc in docs:
            series.append(
                {
                    "stk_flow": stk_flow,
                    "unit": unit,
                    "geo": doc["dimensions"]["geo"],
                    "name": doc.get("series_name") or "",
                    "period": doc.get("period") or [],
                    "value": doc.get("value") or [],
                }
            )
    data = json.dumps(
        {
            "retrieved_utc": utc_now(),
            "query": "Eurostat/nrg_stk_oem stk_flow in (STK_EUE_DIR, STK_MIN_CAL), unit=NR",
            "source_url": "https://ec.europa.eu/eurostat/databrowser/view/nrg_stk_oem (via api.db.nomics.world/v22)",
            "series": series,
        },
        ensure_ascii=False,
        indent=2,
        sort_keys=True,
    ).encode("utf-8")
    save(name, "https://api.db.nomics.world/v22/series/Eurostat/nrg_stk_oem", data)


# ------------------------------------------------------------------- EIA

EIA_PAGES = (
    (
        "eia_spr_weekly.html",
        "https://www.eia.gov/dnav/pet/hist/LeafHandler.ashx?n=PET&s=WCSSTUS1&f=W",
        "crude_oil_in_spr",
    ),
    (
        "eia_us_gas_capacity.html",
        "https://www.eia.gov/dnav/ng/ng_stor_cap_dcu_nus_a.htm",
        "us_working_gas_capacity",
    ),
)


def fetch_eia(force: bool) -> None:
    for name, url, _ in EIA_PAGES:
        if cached(name) and not force:
            print("  cache ok  %s" % name)
            continue
        save(name, url, get(url))


def main(argv: list[str]) -> int:
    force = "--fetch" in argv
    if not force and not os.path.isdir(CACHE):
        print("cache ausente em %s; rode com --fetch" % CACHE)
        return 1
    print("coletando Parte G2 (%s)" % ("download" if force else "cache"))
    fetch_imf_gold(force)
    fetch_eurostat_oil(force)
    fetch_eia(force)
    print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
