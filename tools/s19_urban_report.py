"""Q-015 — le o probe urbano de 365 d e resume os numeros de plausibilidade.

Uso: python tools/s19_urban_report.py <urban_json> [--md]
Somente leitura do JSON do runner; nada e recalculado a partir de suposicao.
"""

import json
import sys


def total_area(row):
    return sum(float(v) for v in row.get("area_km2", []))


def fmt(value, digits=2):
    return ("%." + str(digits) + "f") % value


def main():
    path = sys.argv[1]
    data = json.load(open(path, encoding="utf-8"))
    print("dias=%s threads=%s urban=%s" % (data.get("days"), data.get("threads"),
                                           data.get("urban")))
    header = ("cidade   | km2 ini->fim (delta) | reserva | ocup RES/COM/IND | "
              "burden | aluguel POP/MED/ALT | informal ini->fim (delta) | "
              "caixa | IPTU/mes | alvo/mes | IPTU/alvo")
    print(header)
    for city in data.get("cities", []):
        start = data["start"]["cities"].get(city)
        end = data["end"]["cities"].get(city)
        if not start or not end:
            print("%-8s | sem dado" % city)
            continue
        a0, a1 = total_area(start), total_area(end)
        occ = end.get("zone_occupancy", [])
        rent = end.get("rent", [])
        gov = end.get("gov", {})
        iptu = float(gov.get("month_revenue_iptu", 0.0))
        target = float(gov.get("target_revenue_month", 0.0))
        ratio = iptu / target if target > 1e-9 else 0.0
        print("%-8s | %s->%s (%+.2f) | %s->%s | %s | %s | %s | %s->%s (%+.0f) | "
              "%s | %s | %s | %s" % (
                  city, fmt(a0), fmt(a1), a1 - a0,
                  fmt(float(start.get("reserve_km2", 0.0)), 1),
                  fmt(float(end.get("reserve_km2", 0.0)), 1),
                  "/".join(fmt(float(v), 3) for v in occ),
                  fmt(float(end.get("rent_burden", 0.0)), 3),
                  "/".join(fmt(float(v), 1) for v in rent),
                  fmt(float(start.get("informal_units", 0.0)), 0),
                  fmt(float(end.get("informal_units", 0.0)), 0),
                  float(end.get("informal_units", 0.0))
                  - float(start.get("informal_units", 0.0)),
                  fmt(float(gov.get("cash", 0.0)), 0),
                  fmt(iptu, 0), fmt(target, 0), fmt(ratio, 3)))
    print()
    for city in data.get("cities", []):
        end = data["end"]["cities"].get(city)
        if not end:
            continue
        gov = end.get("gov", {})
        print("%s: stress=%s cond=%s vac=%s built_mes=%s unidades=%s "
              "aluguel_pago_mes=%s (fam=%s firma=%s gov=%s) manut=%s "
              "desp_infra=%s desp_social=%s receita_taxas=%s repasse=%s "
              "pct_gdp=%s projetos=%d informal_km2=%s" % (
                  city, fmt(float(end.get("housing_stress", 0.0)), 3),
                  fmt(float(end.get("condition", 0.0)), 3),
                  "/".join(fmt(float(v), 3) for v in end.get("vacancy", [])),
                  fmt(float(end.get("units_built_month", 0.0)), 0),
                  [fmt(sum(row), 0) for row in end.get("units", [])],
                  fmt(float(end.get("rent_paid_month", 0.0)), 0),
                  fmt(float(end.get("rent_to_households", 0.0)), 0),
                  fmt(float(end.get("rent_to_firms", 0.0)), 0),
                  fmt(float(end.get("rent_to_government", 0.0)), 0),
                  fmt(float(gov.get("month_expense_maintenance", 0.0)), 0),
                  fmt(float(gov.get("month_expense_infrastructure", 0.0)), 0),
                  fmt(float(gov.get("month_expense_social_housing", 0.0)), 0),
                  fmt(float(gov.get("month_revenue_fees", 0.0)), 0),
                  fmt(float(gov.get("month_transfer_paid", 0.0)), 0),
                  fmt(float(gov.get("revenue_pct_gdp", 0.0)), 4),
                  len(gov.get("projects", [])),
                  fmt(float(end.get("informal_km2", 0.0)), 2)))


if __name__ == "__main__":
    main()
