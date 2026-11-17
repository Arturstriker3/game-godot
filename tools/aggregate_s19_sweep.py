"""Q-015 — agrega as variacoes do sweep 365 d (§19) num baseline unico.

O sweep padrao (`tools/economy_world_sweep.gd`) roda as variacoes em serie
chamando `economy_world_test.gd`. Para a revisao Q-015 as variacoes rodaram
como processos Godot simultaneos com `--threads=1 --urban=1`, cada um gravando
o seu relatorio por variacao. Este script junta os relatorios no mesmo formato
de `construction_world_sweep.json` (meta + nations com o PIOR caso por nacao),
sem inventar numero: nacao ausente do baseline fica fora.

Q-016 — portao de sensibilidade do cambio. Todo relatorio grava
`meta.trajectory_hash` (sha256 de nations+labor+sectors+ownership) e `meta.fx`.
O script exige duas coisas e retorna 1 ("FALHA") quando qualquer uma quebra:
  (i) as variantes de aceite (`--variants`, tudo que nao comeca com `float`)
      tem de fechar o MESMO `trajectory_hash`;
  (ii) a SONDA (`float`, `--fx=float`, regime flutuante forçado no teste) tem
       de fechar um hash DIFERENTE — se fechar o mesmo, o caminho cambial esta
       inerte e a repeticao nao prova determinismo nenhum.
A sonda NAO entra no gate de comida nem no teto de falencias: o delta de comida
dela e so impresso como informacao. Um resultado em `meta.trajectory.fx` traz
o `meta.fx` de cada variante (modo, semente, moedas, flutuantes, soma das taxas).

Desde a A7 da linha principal (2026-11-17) o padrao e `real,float`: as 5
variacoes peg antigas (real, real_seed1, real_seed2, half, xgs) fechavam o
mesmo hash desde o Q-016, entao rodar todas era CPU a toa. As duas continuam
sendo prova de determinismo (a peg repetida) e de sensibilidade (a sonda). Os
nomes antigos seguem aceitos.

Uso:
  python tools/aggregate_s19_sweep.py
    --baseline docs/economy/perf/construction_world_sweep.json
    --dir "%APPDATA%/Godot/app_userdata/Open Grand Strategy/world_sweep"
    --prefix s19_ --suffix _365.json
    --variants real,real_seed1,real_seed2,half,xgs,float   (opcional)
    --logdir <pasta dos logs .log>            (opcional: le o drift de dinheiro)
    --out docs/economy/perf/s19_world_sweep.json
"""

import argparse
import json
import os
import re
import sys

VARIANTS_DEFAULT = "real,float"
# Argumentos conhecidos por nome de variante (registro do `meta.variants`).
KNOWN_ARGS = {
    "real": ["--fx=real"],
    "real_seed1": ["--fx=real", "--seed=1"],
    "real_seed2": ["--fx=real", "--seed=2"],
    "half": ["--fx=half"],
    "xgs": ["--currencies=0"],
    "real_nourban": ["--fx=real", "--urban=0"],
    "float": ["--fx=float"],
    "float_probe": ["--fx=float"],
}
# Nome de variante que comeca com isto e SONDA (fora do gate de aceite).
PROBE_PREFIX = "float"
# Nome com que a sonda foi gravada nos prefixos antigos (s19_/q16_/recal*_).
PROBE_LEGACY = "float_probe"
FOOD_TOLERANCE = 0.025
OWNER_BANKRUPTCY_LIMIT = 28
# P4 (recal3, 2026-11-17): o portão de comida usa a MÉDIA DOS ÚLTIMOS 30 DIAS por
# nação (`food_avg30` no relatório do runner), não a foto do último dia — EGY é
# importador em levas e a foto do dia 365 cai no vale de um ciclo diário. As duas
# medidas ficam no baseline; o portão é o delta da média30.
GATE_METRIC = "avg30"
FOOD_WINDOW_DAYS = 30
# Isentos declarados pelo head: quedas de comida nestas nações não reprovam.
FOOD_EXEMPT = ("SOL", "DJI", "SAH", "JOR", "YEM")


def split_variants(spec):
    """`"real,float"` -> (peg=[...], sonda="float"|None).

    A sonda e a variante cujo nome comeca com `float` (`--fx=float`); todas as
    outras formam o conjunto de aceite (peg), que tem de fechar o mesmo hash.
    """
    names = [part.strip() for part in spec.split(",") if part.strip()]
    peg, probe = [], None
    for name in names:
        if name.startswith(PROBE_PREFIX):
            if probe is None:
                probe = name
        else:
            peg.append(name)
    return peg, probe


def variant_file(directory, prefix, name, suffix):
    """Relatorio de uma variacao; a sonda `float` cai no nome antigo se preciso."""
    path = os.path.join(directory, prefix + name + suffix)
    if not os.path.exists(path) and name.startswith(PROBE_PREFIX):
        legacy = os.path.join(directory, prefix + PROBE_LEGACY + suffix)
        if os.path.exists(legacy):
            return legacy
    return path


def variant_log(logdir, name):
    """Log da variacao (mesmo fallback de nome da sonda)."""
    if not logdir:
        return None
    path = os.path.join(logdir, name + ".log")
    if not os.path.exists(path) and name.startswith(PROBE_PREFIX):
        legacy = os.path.join(logdir, PROBE_LEGACY + ".log")
        if os.path.exists(legacy):
            return legacy
    return path


def load(path):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def baseline_food(baseline):
    """`{tag: {food, avg30}}` a partir de um relatório de corrida (dict `nations`)
    ou de um baseline agregado por este script (lista de linhas).

    `food` = último dia (compatibilidade), `avg30` = média de 30 dias (portão).
    `None` em `avg30` quando aquele baseline é anterior à medida.
    """
    nations = baseline.get("nations", {})
    out = {}
    if isinstance(nations, dict):
        for tag, row in nations.items():
            out[tag] = {
                "food": float(row.get("food", 1.0)),
                "avg30": (float(row["food_avg30"]) if row.get("food_avg30")
                          is not None else None),
            }
    elif isinstance(nations, list):
        for row in nations:
            # `baseline_*` é o valor do baseline DAQUELA rodada; `worst_*` é o
            # valor MEDIDO nela. Ao ler um arquivo agregado como baseline da
            # rodada seguinte, a medida de referência é `worst_*` (quando o
            # `baseline_*` não existe, o arquivo foi agregado contra um baseline
            # antigo sem a métrica).
            avg = row.get("food_avg30")
            if avg is None:
                avg = row.get("baseline_avg30")
            if avg is None:
                avg = row.get("worst_avg30")
            out[str(row["tag"])] = {
                "food": float(row.get("food",
                                      row.get("baseline_last_day",
                                              row.get("worst_food", 1.0)))),
                "avg30": (float(avg) if avg is not None else None),
            }
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
    parser.add_argument("--variants", default=VARIANTS_DEFAULT,
                        help="variantes separadas por virgula; nome comecando "
                             "com `float` e a sonda (padrao: real,float)")
    args = parser.parse_args()

    peg_variants, probe_name = split_variants(args.variants)
    if not peg_variants:
        print("FALHA: --variants precisa de ao menos uma variante de aceite "
              "(ex.: real,float)")
        return 2

    baseline = load(args.baseline)
    before = baseline_food(baseline)
    worst = {}
    bankruptcies = {}
    service = {}
    drift = {}
    trajectory_hashes = {}
    fx = {}

    for name in peg_variants:
        path = variant_file(args.dir, args.prefix, name, args.suffix)
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
        log_path = variant_log(args.logdir, name)
        drift[name] = log_drift(log_path)
        for tag, row in report.get("nations", {}).items():
            if tag not in before:
                continue
            prev = before[tag]
            last = float(row.get("food", 1.0))
            avg = row.get("food_avg30")
            avg = float(avg) if avg is not None else None
            delta_last = last - prev["food"]
            # Portão: média30 x média30 quando os dois lados têm a medida; sem
            # ela no baseline, cai para último dia (registrado como "legacy").
            if avg is not None and prev["avg30"] is not None:
                gate_delta = avg - prev["avg30"]
                metric = "avg30"
            else:
                gate_delta = delta_last
                metric = "last_day"
            entry = {
                "delta": gate_delta, "metric": metric,
                "food_avg30": avg, "baseline_avg30": prev["avg30"],
                "food_last_day": last, "baseline_last_day": prev["food"],
                "delta_last_day": delta_last, "variant": name,
            }
            if tag not in worst or gate_delta < worst[tag]["delta"]:
                worst[tag] = entry

    # Q-016 — sonda: fora do gate de comida e do teto de falencias. So entra no
    # portao de sensibilidade e tem o pior delta de comida impresso.
    probe_path = (variant_file(args.dir, args.prefix, probe_name, args.suffix)
                  if probe_name else None)
    probe_hash = None
    probe_worst = None
    probe_summary = None
    if probe_path and os.path.exists(probe_path):
        probe_report = load(probe_path)
        probe_meta = probe_report.get("meta", {})
        probe_hash = probe_meta.get("trajectory_hash")
        fx[probe_name] = probe_meta.get("fx", {})
        # Evidencia da sonda: falencias e drift FICAM registrados no baseline;
        # so nao entram no gate (a sonda nao e variante de aceite).
        probe_summary = {
            "bankruptcies": int(probe_meta.get("bankruptcies", -1)),
            "money_drift": log_drift(variant_log(args.logdir, probe_name)),
        }
        for tag, row in probe_report.get("nations", {}).items():
            if tag not in before:
                continue
            avg = row.get("food_avg30")
            delta_last = float(row.get("food", 1.0)) - before[tag]["food"]
            if avg is not None and before[tag]["avg30"] is not None:
                delta = float(avg) - before[tag]["avg30"]
                metric = "avg30"
            else:
                delta = delta_last
                metric = "last_day"
            if probe_worst is None or delta < probe_worst["delta"]:
                probe_worst = {"tag": tag, "delta": delta, "metric": metric,
                               "food": float(row["food"]),
                               "food_avg30": (float(avg) if avg is not None
                                              else None),
                               "delta_last_day": delta_last}
    elif probe_name:
        print("AVISO: sonda ausente (%s) — sensibilidade do cambio nao avaliada"
              % probe_path)

    # (i) as variantes de aceite tem de fechar o mesmo hash; (ii) a sonda tem
    # de diferir. Hash ausente (relatorio antigo) nao conta como "igual".
    known_peg = [h for h in (trajectory_hashes[name] for name in peg_variants) if h]
    peg_identical = len(known_peg) == len(peg_variants) and len(set(known_peg)) == 1
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
        trajectory_failures.append(
            "as %d variantes de aceite nao tem o mesmo trajectory_hash"
            % len(peg_variants))
    if probe_differs is False:
        trajectory_failures.append(
            "sonda --fx=float com o MESMO trajectory_hash (caminho cambial inerte)")

    rows = []
    for tag, entry in worst.items():
        rows.append({
            "tag": tag,
            "metric": entry["metric"],
            # compatibilidade: `baseline`/`worst_food` continuam sendo o ÚLTIMO dia
            "baseline": entry["baseline_last_day"],
            "worst_food": entry["food_last_day"],
            "baseline_avg30": entry["baseline_avg30"],
            "worst_avg30": entry["food_avg30"],
            # aliases com o nome que o relatório do runner usa (`food`,
            # `food_avg30`): um agregado também serve de baseline para o runner
            "food": entry["food_last_day"],
            "food_avg30": entry["food_avg30"],
            "delta": entry["delta"],
            "delta_last_day": entry["delta_last_day"],
            "variant": entry["variant"],
        })
    rows.sort(key=lambda row: row["delta"])

    failures = [row["tag"] for row in rows
                if row["delta"] < -FOOD_TOLERANCE
                and row["tag"] not in FOOD_EXEMPT]
    exempt_hits = [row["tag"] for row in rows
                   if row["delta"] < -FOOD_TOLERANCE
                   and row["tag"] in FOOD_EXEMPT]
    legacy_metric = [row["tag"] for row in rows if row["metric"] != GATE_METRIC]
    over = {name: value for name, value in bankruptcies.items()
            if value > OWNER_BANKRUPTCY_LIMIT}

    report = {
        "meta": {
            "days": 365,
            "baseline": "res://" + args.baseline.replace("\\", "/"),
            "food_tolerance": FOOD_TOLERANCE,
            "food_metric": {
                "gate": GATE_METRIC,
                "window_days": FOOD_WINDOW_DAYS,
                "last_day_field": "worst_food",
                "avg30_field": "worst_avg30",
                "exempt": list(FOOD_EXEMPT),
                "nations_gated_on_avg30": len(rows) - len(legacy_metric),
                "nations_gated_on_last_day": legacy_metric,
            },
            "bankruptcy_limit": OWNER_BANKRUPTCY_LIMIT,
            "bankruptcies": bankruptcies,
            "service_businesses": service,
            "money_drift": drift,
            "urban": True,
            "threads": 1,
            "parallel_processes": len(peg_variants) + (1 if probe_name else 0),
            "variants": [{"name": name, "args": KNOWN_ARGS.get(name, [])}
                         for name in peg_variants],
            "trajectory": trajectory,
            "probe": {
                "name": probe_name,
                "args": KNOWN_ARGS.get(probe_name, []) if probe_name else [],
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
    print("nao fecham comida (%s): %s" % (
        GATE_METRIC, ", ".join(failures) if failures else "nenhuma"))
    if exempt_hits:
        print("quedas isentas: " + ", ".join(exempt_hits))
    if legacy_metric:
        print("sem media30 no baseline (portao caiu para ultimo dia): %s"
              % ", ".join(legacy_metric))
    print("acima do teto do dono (%d): %s" % (
        OWNER_BANKRUPTCY_LIMIT, ", ".join(over) if over else "nenhuma"))
    for name in peg_variants:
        print("hash %-11s %s" % (name, trajectory_hashes[name]))
    if probe_name:
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
