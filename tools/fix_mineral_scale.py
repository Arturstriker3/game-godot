"""Corrige a escala das reservas minerais em deposits.json.

Dois bugs inflavam as reservas em ordens de grandeza:

1. **Erro de unidade** em ouro/diamante/urânio: o `base_kt` estava em kilotoneladas
   quando a ordem natural é toneladas (ouro 100 kt → 0,1 kt; diamante 5 → 0,02;
   urânio 20.000 → 200). Um depósito de ouro chegava a 655 Mt — mais ouro do que
   existe no mundo.

2. **Acúmulo linear de `sites`**: a quantidade era `base_kt × sites`, onde `sites`
   é a contagem CRUA de registros MRDS na célula (até ~4.400 para ouro). Milhares
   de ocorrências ≠ milhares de minas. Troca para `sqrt(sites)` (retornos
   decrescentes): uma célula com 1.400 registros vira ~37 minas equivalentes.

O script re-deriva a quantidade como
    base_novo × sqrt(sites) × status_factor × noise,
recuperando o `noise` original (0,6–1,4) a partir da quantidade antiga.
Idempotente: guarda um marcador `_scale_fixed` para não re-aplicar.

Uso: python tools/fix_mineral_scale.py
Depois: python tools/build_resource_map.py
"""

import json
import math
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEPOSITS = os.path.join(ROOT, "data", "definitions", "natural", "deposits.json")

RECOVERY = {
    "iron": 0.45, "copper": 0.40, "coal": 0.55, "oil": 0.25, "gas": 0.60,
    "bauxite": 0.80, "lead": 0.70, "gold": 0.90, "phosphate": 0.85,
    "tungsten": 0.60, "rare_earths": 0.65, "sulfur": 0.75, "diamond": 0.95,
    "uranium": 0.80,
}

# base_kt ORIGINAL (usado na geração) — necessário para recuperar o noise.
BASE_OLD = {
    "iron": 30000.0, "copper": 3000.0, "coal": 150000.0, "oil": 50000.0,
    "gas": 30000.0, "bauxite": 20000.0, "lead": 2000.0, "gold": 100.0,
    "phosphate": 30000.0, "tungsten": 200.0, "rare_earths": 150.0,
    "sulfur": 5000.0, "diamond": 5.0, "uranium": 20000.0,
}

# base_kt corrigido (só os que mudaram; os demais mantêm o original).
BASE_NEW = {
    "gold": 0.1,
    "diamond": 0.02,
    "uranium": 200.0,
}

STATUS_FACTOR = {
    "Producer": 2.0, "Past Producer": 1.5, "Plant": 1.2,
    "Prospect": 0.6, "Occurrence": 0.2,
}

# Campos curados de urânio (erro de unidade) → quantidade real em kt.
CURATED_URANIUM = {
    "arlit": 300.0, "rössing": 300.0, "cigar lake": 150.0,
    "mcarthur river": 200.0, "olympic dam": 2300.0, "ranger": 100.0,
    "inkai": 200.0, "krasnokamensk": 150.0,
}


def set_quantity(dep, resource, quantity):
    recovery = RECOVERY[resource]
    dep["quantity_kt"] = round(quantity, 3)
    dep["recoverable_kt"] = round(quantity * recovery, 3)
    dep["max_rate_kt_day"] = max(1.0, round(quantity * recovery / (25.0 * 365.0), 3))


def main():
    data = json.load(open(DEPOSITS, encoding="utf-8"))
    deposits = data["deposits"]
    fixed_mrds = 0
    fixed_curated = 0

    for dep in deposits:
        resource = dep.get("resource")
        if resource not in RECOVERY:
            continue
        if dep.get("_scale_fixed"):
            continue

        if dep.get("data_quality") == "curated":
            name = str(dep.get("names", [""])[0]).lower()
            if resource == "uranium" and name in CURATED_URANIUM:
                set_quantity(dep, resource, CURATED_URANIUM[name])
                fixed_curated += 1
            elif resource == "gold":
                # Trinidad: 187 kt → 187 t (mesmo erro de unidade do base_kt).
                set_quantity(dep, resource, float(dep.get("quantity_kt", 0.0)) * 0.001)
                fixed_curated += 1
            # demais curados (ferro/cobre/carvão/petróleo/gás) mantêm valores manuais.
            dep["_scale_fixed"] = True
            continue

        sites = max(1, int(dep.get("sites", 1)))
        status = str(dep.get("status", "Unknown"))
        status_factor = STATUS_FACTOR.get(status, 0.4)
        old_quantity = float(dep.get("quantity_kt", 0.0))
        old_base = BASE_OLD.get(resource)
        if old_base is None or old_quantity <= 0.0:
            dep["_scale_fixed"] = True
            continue

        noise = old_quantity / (old_base * sites * status_factor)
        new_base = BASE_NEW.get(resource, old_base)
        new_quantity = new_base * math.sqrt(sites) * status_factor * noise
        set_quantity(dep, resource, new_quantity)
        dep["_scale_fixed"] = True
        fixed_mrds += 1

    text = json.dumps(data, indent=1, ensure_ascii=False).replace("\n", "\r\n")
    with open(DEPOSITS, "w", encoding="utf-8", newline="") as handle:
        handle.write(text)
    print("reservas corrigidas: %d derivados + %d curados" % (fixed_mrds, fixed_curated))


if __name__ == "__main__":
    main()
