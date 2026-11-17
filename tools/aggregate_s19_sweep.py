"""Q-015 — agrega as 5 variacoes do sweep 365 d (§19) num baseline unico.

O sweep padrao (`tools/economy_world_sweep.gd`) roda as variacoes em serie
chamando `economy_world_test.gd`. Para a revisao Q-015 as 5 variacoes rodaram
como 5 processos Godot simultaneos com `--threads=1 --urban=1`, cada um
gravando o seu relatorio por variacao. Este script junta os 5 relatorios no
mesmo formato de `construction_world_sweep.json` (meta + nations com o PIOR
caso por nacao), sem inventar numero: nacao ausente do baseline fica fora.

Uso:
  python tools/aggregate_s19_sweep.py
    --baseline docs/economy/perf/construction_world_sweep.json
    --dir "%APPDATA%/Godot/app_userdata/Open Grand Strategy/world_sweep"
    --prefix s19_ --suffix _365.json
    --logdir <pasta dos logs .log>            (opcional: le o drift de dinheiro)
    --out docs/economy/perf/s19_world_sweep.json
"""

import argparse
import json
import os
import re
import sys

VARIANTS = [
    ("real", ["--fx=real"]),
    ("real_seed1", ["--fx=real", "--seed=1"]),
    ("real_seed2", ["--fx=real", "--seed=2"]),
    ("half", ["--fx=half"]),
    ("xgs", ["--currencies=0"]),
]
FOOD_TOLERANCE = 0.025
OWNER_BANKRUPTCY_LIMIT = 28


def load(path):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def baseline_food(baseline):
    """Aceita `{tag: {food}}` (f4) e `[{tag, worst_food}]` (f9/Q-014)."""
    nations = baseline.get("nations", {})
    out = {}
    if isinstance(nations, dict):
        for tag, row in nations.items():
            out[tag] = float(row.get("food", 1.0))
    elif isinstance(nations, list):
        for row in nations:
            out[str(row["tag"])] = float(row.get("worst_food", 1.0))
    return out


def log_drift(path):
    if not path or not os.path.exists(path):
        return None
    pattern = re.compile(r"dinheiro inicial=([-\d.]+) final=([-\d.]+) drift=([-\d.]+)")
    drift = None
    with open(path, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            match = pattern.search(line)
            if match:
                drift = float(match.group(3))
    return drift


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--dir", required=True)
    parser.add_argument("--prefix", default="s19_")
    parser.add_argument("--suffix", default="_365.json")
    parser.add_argument("--logdir", default=None)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    baseline = load(args.baseline)
    before = baseline_food(baseline)
    worst = {}
    bankruptcies = {}
    service = {}
    drift = {}

    for name, _args in VARIANTS:
        path = os.path.join(args.dir, args.prefix + name + args.suffix)
        if not os.path.exists(path):
            print("FALHA: relatorio ausente: " + path)
            return 2
        report = load(path)
        meta = report.get("meta", {})
        bankruptcies[name] = int(meta.get("bankruptcies", -1))
        service[name] = {
            "bankruptcies": int(meta.get("service_bankruptcies", -1)),
            "businesses": int(meta.get("service_businesses", -1)),
        }
        log_path = None
        if args.logdir:
            log_path = os.path.join(args.logdir, name + ".log")
        drift[name] = log_drift(log_path)
        for tag, row in report.get("nations", {}).items():
            if tag not in before:
                continue
            after = float(row.get("food", 1.0))
            delta = after - before[tag]
            if tag not in worst or delta < worst[tag]["delta"]:
                worst[tag] = {"delta": delta, "food": after, "variant": name}

    rows = []
    for tag, entry in worst.items():
        rows.append({
            "tag": tag,
            "baseline": before[tag],
            "worst_food": entry["food"],
            "delta": entry["delta"],
            "variant": entry["variant"],
        })
    rows.sort(key=lambda row: row["delta"])

    failures = [row["tag"] for row in rows if row["delta"] < -FOOD_TOLERANCE]
    over = {name: value for name, value in bankruptcies.items()
            if value > OWNER_BANKRUPTCY_LIMIT}

    report = {
        "meta": {
            "days": 365,
            "baseline": "res://" + args.baseline.replace("\\", "/"),
            "food_tolerance": FOOD_TOLERANCE,
            "bankruptcy_limit": OWNER_BANKRUPTCY_LIMIT,
            "bankruptcies": bankruptcies,
            "service_businesses": service,
            "money_drift": drift,
            "urban": True,
            "threads": 1,
            "parallel_processes": 5,
            "variants": [{"name": name, "args": args} for name, args in VARIANTS],
        },
        "nations": rows,
    }

    with open(args.out, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(report, handle, indent=2, ensure_ascii=False)
        handle.write("\n")

    print("escrito " + args.out)
    for name, value in bankruptcies.items():
        print("%-11s falencias=%d drift=%s" % (name, value, drift[name]))
    print("nao fecham comida: %s" % (", ".join(failures) if failures else "nenhuma"))
    print("acima do teto do dono (%d): %s" % (
        OWNER_BANKRUPTCY_LIMIT, ", ".join(over) if over else "nenhuma"))
    return 1 if (failures or over) else 0


if __name__ == "__main__":
    sys.exit(main())
