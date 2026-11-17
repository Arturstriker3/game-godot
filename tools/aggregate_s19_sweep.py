"""Q-015 — agrega as 5 variacoes do sweep 365 d (§19) num baseline unico.

O sweep padrao (`tools/economy_world_sweep.gd`) roda as variacoes em serie
chamando `economy_world_test.gd`. Para a revisao Q-015 as 5 variacoes rodaram
como 5 processos Godot simultaneos com `--threads=1 --urban=1`, cada um
gravando o seu relatorio por variacao. Este script junta os 5 relatorios no
mesmo formato de `construction_world_sweep.json` (meta + nations com o PIOR
caso por nacao), sem inventar numero: nacao ausente do baseline fica fora.

Q-016 — portao de sensibilidade do cambio. Todo relatorio grava
`meta.trajectory_hash` (sha256 de nations+labor+sectors+ownership) e `meta.fx`.
O script exige duas coisas e retorna 1 ("FALHA") quando qualquer uma quebra:
  (i) as 5 variantes de aceite tem de fechar o MESMO `trajectory_hash`;
  (ii) a SONDA `float_probe` (`--fx=float`, regime flutuante forçado no teste)
       tem de fechar um hash DIFERENTE — se fechar o mesmo, o caminho cambial
       esta inerte e o "×5" nao prova determinismo nenhum.
A sonda NAO entra no gate de comida nem no teto de falencias: o delta de comida
dela e so impresso como informacao. Um resultado em `meta.trajectory.fx` traz
o `meta.fx` de cada variante (modo, semente, moedas, flutuantes, soma das taxas).

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
# Q-016: sonde de sensibilidade do cambio. Nao e variante de aceite.
PROBE = ("float_probe", ["--fx=float"])


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
    trajectory_hashes = {}
    fx = {}

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
        trajectory_hashes[name] = meta.get("trajectory_hash")
        fx[name] = meta.get("fx", {})
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

    # Q-016 — sonda: fora do gate de comida e do teto de falencias. So entra no
    # portao de sensibilidade e tem o pior delta de comida impresso.
    probe_name = PROBE[0]
    probe_path = os.path.join(args.dir, args.prefix + probe_name + args.suffix)
    probe_hash = None
    probe_worst = None
    probe_summary = None
    if os.path.exists(probe_path):
        probe_report = load(probe_path)
        probe_meta = probe_report.get("meta", {})
        probe_hash = probe_meta.get("trajectory_hash")
        fx[probe_name] = probe_meta.get("fx", {})
        # Evidencia da sonda: falencias e drift FICAM registrados no baseline;
        # so nao entram no gate (a sonda nao e variante de aceite).
        probe_summary = {
            "bankruptcies": int(probe_meta.get("bankruptcies", -1)),
            "money_drift": log_drift(os.path.join(args.logdir, probe_name + ".log"))
            if args.logdir else None,
        }
        for tag, row in probe_report.get("nations", {}).items():
            if tag not in before:
                continue
            delta = float(row.get("food", 1.0)) - before[tag]
            if probe_worst is None or delta < probe_worst["delta"]:
                probe_worst = {"tag": tag, "delta": delta, "food": float(row["food"])}
    else:
        print("AVISO: sonda ausente (%s) — sensibilidade do cambio nao avaliada"
              % probe_path)

    # (i) as 5 variantes de aceite tem de fechar o mesmo hash; (ii) a sonda tem
    # de diferir. Hash ausente (relatorio antigo) nao conta como "igual".
    known_peg = [h for h in (trajectory_hashes[name] for name, _ in VARIANTS) if h]
    peg_identical = len(known_peg) == len(VARIANTS) and len(set(known_peg)) == 1
    peg_hash = known_peg[0] if peg_identical else None
    if peg_hash is not None and probe_hash is not None:
        probe_differs = probe_hash != peg_hash
    else:
        probe_differs = None
    trajectory = {
        "peg_hash": peg_hash,
        "peg_identical": peg_identical,
        "float_probe_hash": probe_hash,
        "float_probe_differs": probe_differs,
        "fx": fx,
    }
    trajectory_failures = []
    if not peg_identical:
        trajectory_failures.append("as 5 variantes nao tem o mesmo trajectory_hash")
    if probe_differs is False:
        trajectory_failures.append(
            "sonda --fx=float com o MESMO trajectory_hash (caminho cambial inerte)")

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
            "trajectory": trajectory,
            "probe": {
                "name": probe_name,
                "args": PROBE[1],
                "trajectory_hash": probe_hash,
                "worst_food": probe_worst,
                **(probe_summary or {}),
            },
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
    for name, _args in VARIANTS:
        print("hash %-11s %s" % (name, trajectory_hashes[name]))
    print("hash %-11s %s" % (probe_name, probe_hash))
    if probe_worst is not None:
        print("sonda worst_food %s delta=%+.4f (so informacao, fora do gate)"
              % (probe_worst["tag"], probe_worst["delta"]))
    print("fronteira peg=%s sonda_difere=%s" % (peg_identical, probe_differs))
    if trajectory_failures:
        print("FALHA: " + "; ".join(trajectory_failures))
    return 1 if (failures or over or trajectory_failures) else 0


if __name__ == "__main__":
    sys.exit(main())
