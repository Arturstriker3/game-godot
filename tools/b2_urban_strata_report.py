"""B2 — ônus e pago/faturado do aluguel POR ESTRATO INQUILINO (6 cidades).

Lê o probe urbano de 365 d (`economy_world_test.gd --urban-cities=... 
--urban-out=...`) e imprime, por cidade e quintil inquilino:
  ônus = aluguel pago no dia / renda disponível do dia do estrato;
  pago/faturado = aluguel pago / aluguel faturado ao estrato (rent_bill);
  informal = posse informal do estrato (a favela é só do mais pobre).

Uso: python tools/b2_urban_strata_report.py docs/economy/perf/b2_urban_365.json
Somente leitura; nada é recalculado a partir de suposição.
"""

import json
import sys

BURDEN_RANGE = (0.15, 0.40)
PAID_MIN = 0.70


def fmt(value, digits=3):
    return ("%." + str(digits) + "f") % value


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else \
        "docs/economy/perf/b2_urban_365.json"
    data = json.load(open(path, encoding="utf-8"))
    print("dias=%s cidades=%s" % (data.get("days"), data.get("cities")))
    failures = []
    summary = {}
    for city in data.get("cities", []):
        row = data.get("end", {}).get("cities", {}).get(city)
        if not row:
            print("%-8s | sem dado no fim" % city)
            continue
        paid = [float(v) for v in row.get("strata_rent_paid", [])]
        bill = [float(v) for v in row.get("strata_rent_bill", [])]
        income = [float(v) for v in row.get("strata_income", [])]
        tenure = [float(v) for v in row.get("strata_tenure_rent", [])]
        print("\n%s" % city)
        entry = {"burden": [], "paid_ratio": []}
        for i in range(5):
            if tenure[i] <= 0.05:
                continue
            burden = paid[i] / income[i] if income[i] > 1e-9 else 0.0
            ratio = paid[i] / bill[i] if bill[i] > 1e-9 else 0.0
            entry["burden"].append(burden)
            entry["paid_ratio"].append(ratio)
            print("  Q%d inquilino=%.2f ônus=%s pago/faturado=%s" % (
                i + 1, tenure[i], fmt(burden), fmt(ratio)))
            if not (BURDEN_RANGE[0] <= burden <= BURDEN_RANGE[1]):
                failures.append("%s Q%d ônus %s fora de %.2f-%.2f" % (
                    city, i + 1, fmt(burden), BURDEN_RANGE[0], BURDEN_RANGE[1]))
            if ratio + 1e-6 < PAID_MIN:
                failures.append("%s Q%d pago/faturado %s < %.2f" % (
                    city, i + 1, fmt(ratio), PAID_MIN))
        summary[city] = entry
        # Favela: só o estrato mais pobre pode ter posse informal.
        for i in range(1, 5):
            if float(row.get("strata_tenure_informal", [0] * 5)[i]) > 1e-6:
                failures.append("%s Q%d com posse informal > 0" % (city, i + 1))
        print("  informal_units=%s housing_stress=%s" % (
            fmt(float(row.get("informal_units", 0.0)), 1),
            fmt(float(row.get("housing_stress", 0.0)))))
    if failures:
        print("\nREPROVADO (%d):" % len(failures))
        for failure in failures:
            print(" - " + failure)
        sys.exit(1)
    print("\nAPROVADO: ônus e pago/faturado por estrato na faixa; favela só no Q1.")


if __name__ == "__main__":
    main()
