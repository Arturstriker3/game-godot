"""Recalibra as reservas minerais em deposits.json para totais publicados.

A quantidade de cada depósito (`quantity_kt`) vem originalmente de uma
estimativa determinística do MRDS (contagem de `sites`/`status`), que mede
"ocorrências/depósitos" e não "reservas recuperáveis". O resultado é uma
inconsistência de direção documentada em docs/economy/balance_review.md §4.2:
minerais de alto valor inflados (chumbo/tungstênio/urânio/ouro/cobre) e fósseis
subestimados (carvão/petróleo/gás).

Este script reescala `quantity_kt` de cada depósito para que:

  (a) a soma mundial bata com o total calibrado
      (data/definitions/natural/reserve_calibration.json, world_total_kt); e
  (b) a soma por país bata com a participação do país (shares da mesma fonte).

Regras:

  - Dentro de um país, mantém-se a proporção relativa entre os depósitos do
    MRDS (todos escalados pelo mesmo fator país).
  - País listado na fonte (share nomeado) mas sem depósito no deposits.json:
    não se cria depósito fictício — apenas sinaliza-se no relatório. A fração
    desse país fica de fora da soma mundial (documentado).
  - "resto" é distribuído proporcionalmente entre os demais países com
    depósito (a razão entre os países não listados é preservada).
  - Recurso com world_total_kt "unknown" (enxofre, diamante) é ignorado.

Idempotente: re-rodar reproduz o mesmo resultado (fatores convergem a 1.0).

Uso: python tools/calibrate_reserves.py
Depois: python tools/build_resource_map.py
"""

import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NATURAL = os.path.join(ROOT, "data", "definitions", "natural")
DEPOSITS = os.path.join(NATURAL, "deposits.json")
NATURE = os.path.join(NATURAL, "nature.json")
CALIBRATION = os.path.join(NATURAL, "reserve_calibration.json")
REPORT = os.path.join(ROOT, "docs", "economy", "reserve_calibration_report.json")

# Fração recuperável por recurso (mesmos valores de fix_mineral_scale.py /
# build_natural_geography.py RESOURCES[resource]["recovery"]).
RECOVERY = {
    "iron": 0.45, "copper": 0.40, "coal": 0.55, "oil": 0.25, "gas": 0.60,
    "bauxite": 0.80, "lead": 0.70, "gold": 0.90, "phosphate": 0.85,
    "tungsten": 0.60, "rare_earths": 0.65, "sulfur": 0.75, "diamond": 0.95,
    "uranium": 0.80,
}

# Piso de arredondamento (1 t = 0,001 kt) para não zerar depósitos minúsculos
# (ex.: ouro) durante o reescalonamento. Deriva de conservação desprezível.
QUANTITY_FLOOR_KT = 0.001


def read_json(path):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def write_json(path, data):
    text = json.dumps(data, indent=1, ensure_ascii=False).replace("\n", "\r\n")
    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write(text)


def country_of(territory_code, deposit):
    return territory_code.get(deposit.get("territory", ""), "")


def main():
    data = read_json(DEPOSITS)
    deposits = data["deposits"]
    nature = read_json(NATURE)["territories"]
    calibration = read_json(CALIBRATION)["resources"]

    territory_code = {tid: entry.get("adm0_a3", "") for tid, entry in nature.items()}

    report = []
    print("=== calibrate_reserves.py ===")

    for spec in calibration:
        resource = spec["resource"]
        total = spec["world_total_kt"]
        if total == "unknown" or total is None:
            print("%-12s sem total publicado -> sem calibração (mantém MRDS)"
                  % resource)
            report.append({"resource": resource, "status": "skipped_unknown",
                           "reason": spec.get("note", "")})
            continue

        shares = spec["shares"]
        named = {code: frac for code, frac in shares.items() if code != "resto"}
        resto_frac = shares.get("resto", 0.0)

        # Agrega a quantidade atual por país (pula fundições/refinarias e <=0).
        by_country = {}
        by_deposit = []
        for dep in deposits:
            if dep.get("resource") != resource:
                continue
            if dep.get("facility"):
                continue
            qty = float(dep.get("quantity_kt", 0.0))
            if qty <= 0.0:
                continue
            code = country_of(territory_code, dep)
            if not code:
                continue
            by_country[code] = by_country.get(code, 0.0) + qty
            by_deposit.append((dep, code, qty))

        before = sum(by_country.values())
        if before <= 0.0:
            print("%-12s sem depósitos -> sem calibração" % resource)
            report.append({"resource": resource, "status": "no_deposits"})
            continue

        # Meta por país: nomeados direto; "resto" proporcional aos não listados.
        targets = {}
        missing = []
        for code, frac in named.items():
            if code in by_country:
                targets[code] = total * frac
            else:
                missing.append({"code": code, "share": round(frac, 4)})

        resto_total = total * resto_frac
        non_named = [code for code in by_country if code not in named]
        non_named_current = sum(by_country[c] for c in non_named)
        for code in non_named:
            if non_named_current > 0.0:
                targets[code] = resto_total * (by_country[code] / non_named_current)
            else:
                targets[code] = 0.0

        # Aplica o fator por país (mantém proporção relativa dentro do país).
        scaled = 0
        for dep, code, qty in by_deposit:
            target = targets.get(code, 0.0)
            current = by_country[code]
            factor = (target / current) if current > 0.0 else 1.0
            new_qty = qty * factor
            if new_qty < QUANTITY_FLOOR_KT:
                new_qty = QUANTITY_FLOOR_KT
            new_qty = round(new_qty, 3)
            dep["quantity_kt"] = new_qty
            recovery = RECOVERY.get(resource, 1.0)
            recoverable = round(new_qty * recovery, 3)
            dep["recoverable_kt"] = recoverable
            dep["max_rate_kt_day"] = max(1.0, round(recoverable / (25.0 * 365.0), 3))
            scaled += 1

        # Soma depois.
        after = {}
        for dep, code, _qty in by_deposit:
            after[code] = after.get(code, 0.0) + float(dep["quantity_kt"])
        after_total = sum(after.values())
        top5 = sorted(after.items(), key=lambda kv: -kv[1])[:5]

        entry = {
            "resource": resource,
            "status": "calibrated",
            "world_target_kt": total,
            "before_total_kt": round(before, 3),
            "after_total_kt": round(after_total, 3),
            "factor": round(after_total / before, 6) if before else 0.0,
            "top5_after": [{"code": c, "kt": round(v, 3)} for c, v in top5],
            "missing_countries": missing,
        }
        report.append(entry)
        print("%-12s total %12.0f -> %12.0f kt (x%.3g) | scaled %d depósitos"
              % (resource, before, after_total,
                 (after_total / before) if before else 0.0, scaled))
        print("    top-5 depois: " + ", ".join(
            "%s=%.0f" % (c, v) for c, v in top5))
        if missing:
            print("    [AVISO] país(es) da fonte sem depósito: " + ", ".join(
                "%s(%.1f%%)" % (m["code"], m["share"] * 100) for m in missing))

    # Registra a calibração nos metadados.
    data["_meta"]["reserve_calibration"] = {
        "applied_by": "tools/calibrate_reserves.py",
        "source": "data/definitions/natural/reserve_calibration.json",
        "note": ("quantity_kt rescalado por país para bater com reservas "
                 "publicadas (USGS/EI/Red Book); proporção relativa do MRDS "
                 "preservada dentro de cada país; país da fonte sem depósito "
                 "fica fora da soma (ver relatório).")
    }

    write_json(DEPOSITS, data)
    os.makedirs(os.path.dirname(REPORT), exist_ok=True)
    with open(REPORT, "w", encoding="utf-8") as handle:
        json.dump({"generated_by": "tools/calibrate_reserves.py", "resources": report},
                  handle, indent=2, ensure_ascii=False)
    print("\nrelatório:", REPORT)


if __name__ == "__main__":
    main()
