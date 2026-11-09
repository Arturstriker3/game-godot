"""Baixa os depósitos minerais do USGS MRDS via serviço WFS (compacto).

O download oficial (mrds-csv.zip) está atrás de um desafio Cloudflare; este
script usa o endpoint WFS do mesmo serviço por meio do leitor r.jina.ai, que
devolve o GML como texto. O resultado é cacheado em `tools/data/` (não
versionado) e consumido por `build_natural_geography.py`.

Campos disponíveis no WFS compacto: dep_id, site_name, dev_stat, fips_code,
url, code_list (códigos de commodity) e a coordenada do ponto.

Uso:
  python tools/fetch_usgs_deposits.py

O script retoma de onde parou (tools/data/mrds_state.json) e deduplica por id.
"""

import json
import os
import re
import sys
import time
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "tools", "data")
OUT_JSONL = os.path.join(OUT_DIR, "mrds_deposits.jsonl")
STATE_PATH = os.path.join(OUT_DIR, "mrds_state.json")

BASE_URL = "https://mrdata.usgs.gov/services/wfs/mrds"
READER_URL = "https://r.jina.ai/"
PAGE_SIZE = 20000
REQUEST_DELAY = 1.0
MAX_RETRIES = 5

FEATURE_RE = re.compile(r"<gml:featureMember>(.*?)</gml:featureMember>", re.S)
POS_RE = re.compile(r"<gml:pos>\s*([-\d.]+)\s+([-\d.]+)\s*</gml:pos>")


def field(block, name):
    match = re.search(r"<ms:%s>([^<]*)</ms:%s>" % (name, name), block)
    return match.group(1).strip() if match else ""


def fetch(url):
    last_error = None
    for attempt in range(MAX_RETRIES):
        try:
            request = urllib.request.Request(url, headers={"User-Agent": "WarEconomy-geography/1.0"})
            with urllib.request.urlopen(request, timeout=300) as response:
                return response.read().decode("utf-8", "replace")
        except Exception as error:  # noqa: BLE001 - rede instável, tentamos de novo
            last_error = error
            wait = min(60, 2 ** attempt * 2)
            print("[retry %d/%d] %s (aguardando %ds)" % (
                attempt + 1, MAX_RETRIES, error, wait), flush=True)
            time.sleep(wait)
    raise RuntimeError("Falha ao baixar %s: %s" % (url, last_error))


def fetch_page(start_index):
    query = ("%s?service=WFS&version=1.1.0&request=GetFeature&typename=mrds"
             "&maxFeatures=%d&startIndex=%d" % (BASE_URL, PAGE_SIZE, start_index))
    text = fetch(READER_URL + query)
    start = text.find("<?xml")
    return text[start:] if start >= 0 else text


def parse_features(xml_text):
    records = []
    for block in FEATURE_RE.findall(xml_text):
        dep_id = field(block, "dep_id")
        if not dep_id:
            continue
        position = POS_RE.search(block)
        if position is None:
            continue
        records.append({
            "id": dep_id,
            "name": field(block, "site_name"),
            "status": field(block, "dev_stat"),
            "fips": field(block, "fips_code"),
            "codes": field(block, "code_list"),
            "lat": float(position.group(1)),
            "lon": float(position.group(2)),
        })
    return records


def load_state():
    if os.path.exists(STATE_PATH):
        try:
            with open(STATE_PATH, "r", encoding="utf-8") as handle:
                return json.load(handle)
        except (OSError, ValueError):
            pass
    return {"next_start": 0, "raw_total": 0, "written": 0}


def save_state(state):
    with open(STATE_PATH, "w", encoding="utf-8") as handle:
        json.dump(state, handle, indent=2)


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    state = load_state()
    start = int(state.get("next_start", 0))
    raw_total = int(state.get("raw_total", 0))
    written = int(state.get("written", 0))

    with open(OUT_JSONL, "a", encoding="utf-8") as output:
        while True:
            xml_text = fetch_page(start)
            blocks = FEATURE_RE.findall(xml_text)
            raw_count = len(blocks)
            if raw_count == 0:
                print("Página vazia em start=%d; download concluído." % start, flush=True)
                break

            records = parse_features(xml_text)
            for record in records:
                output.write(json.dumps(record, ensure_ascii=False) + "\n")
            output.flush()

            start += raw_count
            raw_total += raw_count
            written += len(records)
            state.update({"next_start": start, "raw_total": raw_total, "written": written})
            save_state(state)

            print("[mrds] start=%d raw=%d pontos=%d total_escrito=%d" % (
                start, raw_count, len(records), written), flush=True)

            if raw_count < PAGE_SIZE:
                print("Última página; download concluído.", flush=True)
                break
            time.sleep(REQUEST_DELAY)

    print("MRDS: %d registros brutos, %d com coordenada -> %s" % (
        raw_total, written, OUT_JSONL))


if __name__ == "__main__":
    sys.exit(main())
