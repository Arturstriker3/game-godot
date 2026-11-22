"""Baixa GHI/PVOUT (Global Solar Atlas) e vento a 100 m (Global Wind Atlas)
nos centrÃ³ides das provÃ­ncias do atlas, para o potencial solar/eÃ³lico do E1.

Por que ponto a ponto: os GeoTIFF globais do GSA/GWA sÃ£o enormes (GHI 2,7 GB
comprimido; vento 250 m ~14 GB por camada) e nÃ£o sÃ£o COG (nÃ£o dÃ¡ para ler uma
janela por HTTP range). A API pÃºblica devolve o mesmo dado de long-term average
no ponto, com a sÃ©rie mensal do GSA, e o GWA devolve o clima generalizado de
Weibull. O cache guarda os valores brutos; o builder Ã© determinÃ­stico em cima
dele.

LicenÃ§as verificadas em 2026-11-22:
  * Global Solar Atlas 2.x (Solargis/ESMAP/Banco Mundial) â€” CC BY 4.0
    (https://datacatalog.worldbank.org/search/dataset/0038645);
  * Global Wind Atlas 3/4 (DTU/ESMAP/Banco Mundial) â€” CC BY 4.0
    (https://datacatalog.worldbank.org/search/dataset/0038643).

Uso:
    python tools/data/energy/fetch_atlas_points.py [--workers 24] [--limit N] [--force]

SaÃ­da (cache, gitignored): tools/data/energy/cache/atlas_points.json
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime, timezone

import requests

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import build_energy_common as ec  # noqa: E402

CACHE = os.path.join(ROOT, "tools", "data", "energy", "cache", "atlas_points.json")
GSA_URL = "https://api.globalsolaratlas.info/data/lta"
GWA_URL = "https://globalwindatlas.info/api/gwa/custom/Lib/"
GSA_LICENSE = {
    "name": "CC BY 4.0",
    "url": "https://creativecommons.org/licenses/by/4.0/",
    "evidence": ("World Bank Data Catalog: World - GHI/PVOUT GIS Data (Global "
                 "Solar Atlas), license Creative Commons Attribution 4.0"),
    "dataset": "Global Solar Atlas 2.x (Solargis for ESMAP/World Bank)",
}
GWA_LICENSE = {
    "name": "CC BY 4.0",
    "url": "https://creativecommons.org/licenses/by/4.0/",
    "evidence": ("World Bank Data Catalog: World - Wind Speed and Power Density "
                 "GIS Data (Global Wind Atlas), license CC BY 4.0; GWA site "
                 "declares free use with attribution"),
    "dataset": "Global Wind Atlas 3/4 (DTU Wind Energy for ESMAP/World Bank)",
}
RHO = 1.225  # kg/m3 ao nÃ­vel do mar; o builder corrige pela altitude (ETOPO1)
MAX_RETRIES = 3
SAVE_EVERY = 250
_lock = threading.Lock()


def load_points():
    provinces = ec.load_provinces()
    points = []
    for province in provinces:
        lon, lat = ec.map_to_lonlat(province["x"], province["y"])
        points.append({"pid": province["id"], "lat": round(lat, 5),
                       "lon": round(lon, 5)})
    points.sort(key=lambda p: p["pid"])
    return points


def parse_gwa(text):
    """GWA Generalized Wind Climate -> mÃ©dias a 100 m por classe de rugosidade."""
    lines = [line for line in text.splitlines() if line.strip()]
    if len(lines) < 5:
        raise ValueError("GWC curto")
    roughness_n, height_n, sector_n = map(int, lines[1].split())
    roughness = [float(v) for v in lines[2].split()]
    heights = [float(v) for v in lines[3].split()]
    if len(roughness) != roughness_n or len(heights) != height_n:
        raise ValueError("GWC com dimensÃµes inesperadas")
    values = []
    for line in lines[4:]:
        values.extend(float(v) for v in line.split())
    per_roughness = sector_n + height_n * sector_n * 2
    if len(values) < per_roughness * roughness_n:
        raise ValueError("GWC truncado")
    if 100.0 not in heights:
        raise ValueError("GWC sem altura de 100 m")
    height_index = heights.index(100.0)
    out = {"roughness_classes": roughness, "heights": heights,
           "sectors": sector_n, "by_roughness": []}
    for r in range(roughness_n):
        base = r * per_roughness
        freq = values[base:base + sector_n]
        offset = base + sector_n
        # bloco de alturas: A e k intercalados por altura
        a_offset = offset + height_index * sector_n * 2
        k_offset = a_offset + sector_n
        a_values = values[a_offset:a_offset + sector_n]
        k_values = values[k_offset:k_offset + sector_n]
        total_freq = sum(freq)
        if total_freq <= 0.0:
            continue
        mean_speed = 0.0
        mean_cube = 0.0
        for f, a, k in zip(freq, a_values, k_values):
            if a <= 0.0 or k <= 0.0:
                continue
            w = f / total_freq
            mean_speed += w * a * math.gamma(1.0 + 1.0 / k)
            mean_cube += w * (a ** 3) * math.gamma(1.0 + 3.0 / k)
        out["by_roughness"].append({
            "roughness_m": roughness[r],
            "m_s_mean_100m": round(mean_speed, 4),
            "power_density_w_m2_100m": round(0.5 * RHO * mean_cube, 2),
            "frequency": [round(v, 4) for v in freq],
            "a": [round(v, 4) for v in a_values],
            "k": [round(v, 4) for v in k_values],
        })
    return out


def fetch_gsa(session, lat, lon):
    response = session.get(GSA_URL, params={"loc": "%.5f,%.5f" % (lat, lon)},
                           timeout=25)
    response.raise_for_status()
    data = response.json()
    annual = data.get("annual", {}).get("data", {})
    monthly = data.get("monthly", {}).get("data", {})
    out = {
        "ghi_year_kwh_m2": annual.get("GHI"),
        "pvout_year_kwh_kwp": annual.get("PVOUT_csi"),
        "ghi_monthly_kwh_m2": monthly.get("GHI"),
        "pvout_monthly_kwh_kwp": monthly.get("PVOUT_csi"),
        "temp_monthly_c": monthly.get("TEMP"),
        "metadata_version": data.get("annual", {}).get("metadata", {}).get("version"),
    }
    if out["ghi_year_kwh_m2"] is None or not out["ghi_monthly_kwh_m2"]:
        raise ValueError("GSA sem GHI")
    return out


def fetch_gwa(session, lat, lon):
    response = session.get(GWA_URL, params={"lat": "%.5f" % lat, "long": "%.5f" % lon},
                           timeout=25)
    response.raise_for_status()
    return parse_gwa(response.text)


def fetch_one(point, session):
    result = dict(point)
    errors = []
    for attempt in range(MAX_RETRIES):
        try:
            result["solar"] = fetch_gsa(session, point["lat"], point["lon"])
            break
        except Exception as exc:  # noqa: BLE001
            errors.append("GSA: %s" % exc)
            time.sleep(0.5 * (attempt + 1))
    for attempt in range(MAX_RETRIES):
        try:
            result["wind"] = fetch_gwa(session, point["lat"], point["lon"])
            break
        except Exception as exc:  # noqa: BLE001
            errors.append("GWA: %s" % exc)
            time.sleep(0.5 * (attempt + 1))
    if errors:
        result["errors"] = errors
    return result


def save(cache, points):
    payload = {
        "meta": cache["meta"],
        "points": points,
    }
    tmp = CACHE + ".tmp"
    os.makedirs(os.path.dirname(CACHE), exist_ok=True)
    with open(tmp, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
                     + "\n")
    os.replace(tmp, CACHE)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--workers", type=int, default=24)
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--force", action="store_true")
    parser.add_argument("--retry-errors", action="store_true",
                        help="tenta de novo os pontos que ficaram com erro")
    args = parser.parse_args()

    points = load_points()
    if args.limit:
        points = points[:args.limit]
    cache = {"meta": {"generated_by": "tools/data/energy/fetch_atlas_points.py",
                      "plan": "docs/plans/perf_pop_financas_plano.md, PARTE E, E1",
                      "sources": {"gsa": GSA_LICENSE, "gwa": GWA_LICENSE},
                      "accessed_at_utc": None,
                      "rho_kg_m3": RHO,
                      "method": ("1 ponto por provÃ­ncia (centrÃ³ide); GSA annual "
                                 "GHI/PVOUT + sÃ©rie mensal; GWA Generalised Wind "
                                 "Climate a 100 m, mÃ©dia por classe de rugosidade"),
                      "counts": {}}, "points": {}}
    if os.path.exists(CACHE) and not args.force:
        with open(CACHE, encoding="utf-8") as handle:
            cache = json.load(handle)
    done = cache["points"]
    if args.retry_errors:
        todo = [p for p in points
                if str(p["pid"]) in done and done[str(p["pid"])].get("errors")]
        for point in todo:
            done[str(point["pid"])].pop("errors", None)
    else:
        todo = [p for p in points if str(p["pid"]) not in done]
    print("pontos: %d | em cache: %d | a baixar: %d"
          % (len(points), len(done), len(todo)))
    if not todo:
        return 0

    session = requests.Session()
    session.headers.update({
        "User-Agent": "opengs-game energy data collection (research; project repo)",
        "Referer": "https://globalwindatlas.info",
    })
    started = time.time()
    saved = 0
    with ThreadPoolExecutor(max_workers=max(1, args.workers)) as pool:
        futures = {pool.submit(fetch_one, point, session): point for point in todo}
        for index, future in enumerate(as_completed(futures), 1):
            result = future.result()
            with _lock:
                done[str(result["pid"])] = result
                saved += 1
            if index % 100 == 0 or index == len(todo):
                elapsed = time.time() - started
                print("  %d/%d (%.1f%%) %.0fs"
                      % (index, len(todo), index * 100.0 / len(todo), elapsed))
            if saved % SAVE_EVERY == 0 or index == len(todo):
                with _lock:
                    cache["meta"]["accessed_at_utc"] = datetime.now(
                        timezone.utc).replace(microsecond=0).strftime(
                            "%Y-%m-%dT%H:%M:%SZ")
                    cache["meta"]["counts"] = {
                        "provinces_requested": len(points),
                        "provinces_cached": len(done),
                        "errors": sum(1 for value in done.values()
                                      if value.get("errors")),
                    }
                    save(cache, done)
    print("cache: %s (%d pontos)" % (CACHE, len(done)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
