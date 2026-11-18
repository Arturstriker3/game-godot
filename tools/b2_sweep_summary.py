"""B2 — resumo do sweep 365 d (real + sonda float) contra o baseline da B1.

Lê os relatórios `b2_<variante>_365.json` do runner (economy_world_test) e o
baseline `b1_world_sweep.json`, e imprime:
  - falências, drift e hash da trajetória por variante;
  - nações fora do portão de comida de 30 dias (Δ contra a B1 > 2,5 p.p.),
    separando as isenções declaradas (SOL, DJI, SAH, JOR, YEM) e as nações em
    diagnóstico (LBY, AFG, ARE, EGY) das demais;
  - o pior Δ de comida de 30 dias.

Uso: python tools/b2_sweep_summary.py [dir_do_sweep]
Somente leitura; nada é recalculado a partir de suposição.
"""

import json
import os
import sys

EXEMPT = {"SOL", "DJI", "SAH", "JOR", "YEM"}
DIAGNOSED = {"LBY", "AFG", "ARE", "EGY"}
VARIANTS = ("real", "float")
TOLERANCE = 0.025


def load(path):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def main():
    sweep_dir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.environ.get("APPDATA", ""), "Godot", "app_userdata",
        "Open Grand Strategy", "world_sweep")
    baseline_path = "docs/economy/perf/b1_world_sweep.json"
    baseline = load(baseline_path)
    base_rows = baseline.get("nations", [])
    base_nations = ({row["tag"]: row for row in base_rows}
                    if isinstance(base_rows, list) else base_rows)
    failures = []
    for variant in VARIANTS:
        path = os.path.join(sweep_dir, "b2_%s_365.json" % variant)
        if not os.path.exists(path):
            print("sem relatório: " + path)
            continue
        data = load(path)
        meta = data.get("meta", {})
        bankruptcies = meta.get("bankruptcies", {})
        if isinstance(bankruptcies, dict):
            bankruptcies = bankruptcies.get(variant, 0)
        drift = meta.get("money_drift", 0.0)
        if isinstance(drift, dict):
            drift = drift.get(variant, 0.0)
        print("\n== %s ==" % variant)
        print("dias=%s hash=%s falências=%s drift=%s" % (
            meta.get("days"), str(meta.get("trajectory_hash", ""))[:12],
            bankruptcies, drift))
        if int(bankruptcies) > 28:
            failures.append("%s: falências %s > 28" % (variant, bankruptcies))
        if abs(float(drift)) > 0.01:
            failures.append("%s: drift %s > 0,01" % (variant, drift))
        outside = []
        worst = (0.0, "")
        raw = data.get("nations", {})
        rows = ([{"tag": tag, **row} for tag, row in raw.items()]
                if isinstance(raw, dict) else raw)
        for row in rows:
            tag = row["tag"]
            base = base_nations.get(tag)
            if not base:
                continue
            value = float(row.get("food_avg30", row.get("food", 0.0)))
            base_value = float(base.get("worst_avg30",
                                        base.get("worst_food", 0.0)))
            delta = value - base_value
            if abs(delta) > abs(worst[0]):
                worst = (delta, tag)
            if delta < -TOLERANCE and tag not in EXEMPT:
                outside.append((tag, delta))
        print("pior Δ comida 30 d: %+.1f p.p. (%s)" % (
            worst[0] * 100.0, worst[1]))
        print("fora do portão (fora das isenções):")
        for tag, delta in sorted(outside, key=lambda item: item[1]):
            group = "diagnóstico" if tag in DIAGNOSED else "B2"
            print("  %-4s %+.1f p.p. [%s]" % (tag, delta * 100.0, group))
            if group == "B2":
                failures.append("%s: %s comida %+.1f p.p." % (
                    variant, tag, delta * 100.0))
        if not outside:
            print("  nenhuma")
    if failures:
        print("\nREPROVADO (%d):" % len(failures))
        for failure in failures:
            print(" - " + failure)
        sys.exit(1)
    print("\nAPROVADO: falências, drift e comida dentro do portão.")


if __name__ == "__main__":
    main()
