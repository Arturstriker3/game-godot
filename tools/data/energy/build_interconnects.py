"""E5 - interconexoes entre redes eletricas vizinhas (dado derivado).

Gera ``data/definitions/energy/interconnects.json`` a partir de DADO publico ja
versionado no repositorio:

* ``data/definitions/energy/national_demand.json`` (Ember Yearly Electricity
  Data, CC BY 4.0) - ``net_imports_twh`` por nacao no ano-base do parque:
  positivo = importador liquido, negativo = exportador liquido.
* ``data/definitions/energy/land_borders.json`` (Natural Earth + atlas do
  projeto) - pares de nacoes com fronteira TERRESTRE e o comprimento em km.

Regra de derivacao (declarada, nao e dado observado):

1. Fluxo medio do importador: ``net_imports_twh * 1e6 / 8760`` MW (media anual
   da importacao liquida que o dado Ember publica).
2. O fluxo do importador e repartido entre os vizinhos EXPORTADORES
   proporcionalmente a exportacao liquida de cada um (Ember). Nacao importadora
   sem vizinho exportador fica sem link e entra em ``unserved_importers``.
3. Capacidade da linha = fluxo medio x ``HEADROOM``. O dado nao publica a
   capacidade de cada interligacao (nao ha dataset por par no repositorio);
   a folga de pico e DEFAULT DE JOGO declarado. Razao: a NTC (Net Transfer
   Capacity) da ENTSO-E e a capacidade maxima de troca entre duas areas
   compativel com seguranca - um limite de CAPACIDADE, nao um fluxo medio
   (ENTSO-E, "Definitions of Transfer Capacities in liberalised Electricity
   Markets"). A simulacao despacha energia DIARIA, entao a linha e dimensionada
   no fluxo medio com folga de 25% para a variacao diaria; sem a folga, metade
   dos dias ficaria limitada ao proprio fluxo medio.

Ligacoes SUBMARINAS conhecidas NAO entram: o repositorio nao tem dataset
publico por par (nem capacidades nem fluxos), e inventar MW seria fabricar
dado. ``meta.submarine_links`` declara a ausencia.

Uso:
    python tools/data/energy/build_interconnects.py [--check]

``--check`` nao grava: compara o arquivo em disco com o que seria gerado e
sai com codigo != 0 se divergir (para o CI/rodada de dados).
"""

from __future__ import annotations

import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
ENERGY = ROOT / "data" / "definitions" / "energy"
SCENARIO = ROOT / "data" / "definitions" / "scenario"

DEMAND_PATH = ENERGY / "national_demand.json"
BORDERS_PATH = ENERGY / "land_borders.json"
CITIES_PATH = SCENARIO / "cities.json"
OUT_PATH = ENERGY / "interconnects.json"

HOURS_PER_YEAR = 8760.0
TWH_TO_MW = 1_000_000.0 / HOURS_PER_YEAR
# Default de jogo declarado (ver docstring): folga de pico sobre o fluxo medio.
HEADROOM = 1.25
# Piso de ruido: fluxo medio abaixo disso nao justifica uma linha no modelo.
MIN_FLOW_MW = 0.5

# Fronteira terrestre que NÃO é ligação de rede nacional: território
# ultramarino cuja rede elétrica não pertence à rede nacional do país. A
# exclusão é declarada e auditável (não é dado: é geografia de rede).
OVERSEAS_ONLY_PAIRS = {
    ("BRA", "FRA"): (
        "fronteira terrestre apenas na Guiana Francesa; a rede nacional "
        "francesa está na Europa ('continent' no atlas, separado de 'guiana') "
        "e não se liga à rede brasileira"
    ),
}

# Ligações cujo dado derivado (Ember × fronteira terrestre) sai ZERO ou
# subdimensionado porque o par é submarino ou porque o vizinho não é
# exportador líquido no ano-base (as duas pontas importam em anos diferentes).
# A capacidade vem de fonte pública por par — não é modelo: é capacidade
# nominal publicada pelo regulador/operador. Mesmo assim o núcleo decide a
# direção do fluxo pelo déficit diário (o campo `importer` aqui é só relatório).
PUBLIC_LINKS = [
    {
        "a": "FRA", "b": "GBR", "capacity_mw": 4000.0,
        "source": "ofgem_interconnectors",
        "note": "IFA 2.000 + IFA2 1.000 + ElecLink 1.000 MW",
    },
    {
        "a": "GBR", "b": "NLD", "capacity_mw": 1000.0,
        "source": "ofgem_interconnectors",
        "note": "BritNed 1.000 MW",
    },
    {
        "a": "BEL", "b": "GBR", "capacity_mw": 1000.0,
        "source": "ofgem_interconnectors",
        "note": "Nemo Link 1.000 MW",
    },
    {
        "a": "GBR", "b": "NOR", "capacity_mw": 1400.0,
        "source": "ofgem_interconnectors",
        "note": "North Sea Link 1.400 MW",
    },
    {
        "a": "GBR", "b": "IRL", "capacity_mw": 1000.0,
        "source": "ofgem_interconnectors",
        "note": "East-West 500 + Greenlink 504 MW (arredondado a 1 GW pelo Ofgem)",
    },
    {
        "a": "DNK", "b": "NOR", "capacity_mw": 1632.0,
        "source": "energinet_dk_interconnectors",
        "note": "Skagerrak 1-4 (DK1-NO2) 1.632 MW",
    },
    {
        "a": "DNK", "b": "SWE", "capacity_mw": 2015.0,
        "source": "energinet_dk_interconnectors",
        "note": "Konti-Skan 715 (DK1-SE3) + Oresund 1.300 (DK2-SE4) MW",
    },
    {
        "a": "DEU", "b": "DNK", "capacity_mw": 3500.0,
        "source": "energinet_dk_interconnectors",
        "note": "Endrup-Alemanha 2.500 + Kontek 600 + Kriegers Flak 400 MW",
    },
    {
        "a": "BGD", "b": "IND", "capacity_mw": 1000.0,
        "source": "adb_bgd_ind",
        "note": "Baharampur-Bheramara 400 kV 2xD/c + 2x500 MW HVDC B2B",
    },
]

SOURCES = [
    {
        "id": "ember_yearly",
        "title": "Yearly Electricity Data (global release)",
        "publisher": "Ember Energy Research CIC",
        "url": "https://files.ember-energy.org/public-downloads/generation/outputs/release_generation_yearly_global.csv",
        "landing_url": "https://ember-energy.org/data/yearly-electricity-data/",
        "license": "CC BY 4.0",
        "used_for": "net_imports_twh por nacao no ano-base do parque (importador/exportador liquido)",
        "used_in": "data/definitions/energy/national_demand.json",
    },
    {
        "id": "land_borders",
        "title": "Fronteiras terrestres do atlas do projeto",
        "publisher": "Natural Earth (dominio publico) processado por tools/build_energy_borders.py",
        "url": None,
        "landing_url": "https://www.naturalearthdata.com/",
        "license": "dominio publico (Natural Earth); raster do atlas e artefato do projeto",
        "used_for": "pares de nacoes vizinhas com fronteira terrestre e comprimento em km",
        "used_in": "data/definitions/energy/land_borders.json",
    },
    {
        "id": "entsoe_ntc",
        "title": "Definitions of Transfer Capacities in liberalised Electricity Markets",
        "publisher": "ENTSO-E",
        "url": "https://www.entsoe.eu/fileadmin/user_upload/_library/ntc/entsoe_transferCapacityDefinitions.pdf",
        "landing_url": "https://www.entsoe.eu/",
        "license": "documento publico de referencia (citacao)",
        "used_for": (
            "razao da folga de pico: NTC e a capacidade maxima de troca "
            "entre duas areas compativel com seguranca (limite de "
            "capacidade, nao fluxo medio)"
        ),
        "used_in": "meta.headroom_rationale (default de jogo, nao e dado por par)",
    },
    {
        "id": "ofgem_interconnectors",
        "title": "Interconnectors (cap and floor regime; capacity by link)",
        "publisher": "Ofgem (GB regulator)",
        "url": "https://www.ofgem.gov.uk/energy-policy-and-regulation/policy-and-regulatory-programmes/interconnectors",
        "landing_url": "https://www.ofgem.gov.uk/",
        "license": "publicacao regulatoria publica (citacao)",
        "accessed_at_utc": "2026-11-22T06:40:00Z",
        "used_for": (
            "capacidade nominal das ligacoes submarinas GB-Franca "
            "(IFA/IFA2/ElecLink), GB-Paises Baixos (BritNed), GB-Belgica "
            "(Nemo Link), GB-Noruega (North Sea Link) e GB-Irlanda "
            "(East-West/Greenlink)"
        ),
        "used_in": "links[method=public_capacity]",
    },
    {
        "id": "energinet_dk_interconnectors",
        "title": (
            "Danish electricity market implementation plan according to "
            "Article 20 of Regulation (EU) 2019/943 (table 5: interconnector "
            "capacity)"
        ),
        "publisher": "Danish Energy Agency / Energinet",
        "url": "https://ens.dk/media/7817/download",
        "landing_url": "https://ens.dk/",
        "license": "publicacao publica (citacao)",
        "accessed_at_utc": "2026-11-22T06:40:00Z",
        "used_for": (
            "capacidade nominal das ligacoes da Dinamarca: Skagerrak 1-4 "
            "(NO), Konti-Skan + Oresund (SE) e Endrup/Kontek/Kriegers Flak (DE)"
        ),
        "used_in": "links[method=public_capacity]",
    },
    {
        "id": "adb_bgd_ind",
        "title": (
            "SASEC Second Bangladesh-India Electrical Grid Interconnection "
            "Project (Baharampur-Bheramara)"
        ),
        "publisher": "Asian Development Bank (ADB)",
        "url": "https://www.adb.org/projects/44192-016/main",
        "landing_url": "https://www.adb.org/",
        "license": "publicacao publica (citacao)",
        "accessed_at_utc": "2026-11-22T06:40:00Z",
        "used_for": (
            "capacidade da ligacao Bangladesh-India: 2x500 MW HVDC "
            "back-to-back em Bheramara (500 MW em 2013, 1.000 MW apos o "
            "segundo projeto SASEC)"
        ),
        "used_in": "links[method=public_capacity]",
    },
]


def load(path: Path):
    with path.open(encoding="utf-8") as handle:
        return json.load(handle)


def build() -> dict:
    demand = load(DEMAND_PATH)
    borders = load(BORDERS_PATH)
    cities = load(CITIES_PATH)

    universe = sorted({str(city.get("nation", "")) for city in cities} - {""})
    by_nation = demand.get("by_nation", {})

    net_imports = {}
    for tag in universe:
        row = by_nation.get(tag)
        if not isinstance(row, dict):
            continue
        value = row.get("net_imports_twh")
        if isinstance(value, (int, float)):
            net_imports[tag] = float(value)

    neighbors: dict[str, set[str]] = defaultdict(set)
    border_km = {}
    for row in borders.get("borders", []):
        a = str(row.get("a", ""))
        b = str(row.get("b", ""))
        if not a or not b or a == b:
            continue
        if a not in universe or b not in universe:
            continue
        if tuple(sorted((a, b))) in OVERSEAS_ONLY_PAIRS:
            continue
        neighbors[a].add(b)
        neighbors[b].add(a)
        border_km[tuple(sorted((a, b)))] = float(row.get("length_km") or 0.0)

    exporters = {
        tag: -value for tag, value in net_imports.items() if value < 0.0
    }
    importers = {
        tag: value for tag, value in net_imports.items() if value > 0.0
    }

    links = []
    unserved = []
    per_importer = {}
    for importer in sorted(importers):
        avg_import_mw = importers[importer] * TWH_TO_MW
        exporter_neighbors = {
            tag: exporters[tag]
            for tag in neighbors.get(importer, set())
            if exporters.get(tag, 0.0) > 0.0
        }
        if not exporter_neighbors:
            unserved.append(
                {
                    "nation": importer,
                    "net_imports_twh": round(importers[importer], 6),
                    "avg_import_mw": round(avg_import_mw, 6),
                    "reason": "sem vizinho exportador com dado Ember",
                }
            )
            continue
        export_sum = sum(exporter_neighbors.values())
        allocated = 0.0
        rows = []
        for exporter in sorted(exporter_neighbors):
            share = exporter_neighbors[exporter] / export_sum
            flow_mw = avg_import_mw * share
            if flow_mw < MIN_FLOW_MW:
                continue
            capacity_mw = flow_mw * HEADROOM
            a, b = sorted((importer, exporter))
            rows.append(
                {
                    "a": a,
                    "b": b,
                    "importer": importer,
                    "exporter": exporter,
                    "border_km": round(
                        border_km.get((a, b), 0.0), 3
                    ),
                    "importer_net_imports_twh": round(
                        importers[importer], 6
                    ),
                    "importer_avg_import_mw": round(avg_import_mw, 6),
                    "exporter_net_export_twh": round(
                        exporter_neighbors[exporter], 6
                    ),
                    "neighbor_export_sum_twh": round(export_sum, 6),
                    "share": round(share, 8),
                    "avg_flow_mw": round(flow_mw, 6),
                    "capacity_mw": round(capacity_mw, 6),
                    "headroom": HEADROOM,
                    "method": "ember_net_imports_x_land_borders",
                }
            )
            allocated += flow_mw
        if rows:
            links.extend(rows)
            per_importer[importer] = {
                "avg_import_mw": round(avg_import_mw, 6),
                "allocated_mw": round(allocated, 6),
                "links": len(rows),
            }
        else:
            unserved.append(
                {
                    "nation": importer,
                    "net_imports_twh": round(importers[importer], 6),
                    "avg_import_mw": round(avg_import_mw, 6),
                    "reason": "fluxo alocado abaixo do piso declarado",
                }
            )

    links.sort(key=lambda row: (row["a"], row["b"]))
    # --- Ligações submarinas / de capacidade pública por par (QA t80).
    # A derivação Ember × fronteira terrestre não vê par submarino (GB, DK) e
    # não cria par quando o vizinho não é exportador líquido no ano-base
    # (BGD-IND). Para esses pares a capacidade é o valor NOMINAL publicado
    # pelo regulador/operador; o núcleo decide a direção diária pelo déficit.
    by_pair = {(row["a"], row["b"]): row for row in links}
    public_applied = []
    for item in PUBLIC_LINKS:
        a, b = sorted((item["a"], item["b"]))
        capacity = float(item["capacity_mw"])
        source = item["source"]
        row = by_pair.get((a, b))
        if row is not None:
            if capacity > float(row["capacity_mw"]):
                row["capacity_mw"] = round(capacity, 6)
                row["method"] = "public_capacity"
                row["source"] = source
                row["capacity_note"] = item["note"]
                public_applied.append(
                    {
                        "a": a,
                        "b": b,
                        "capacity_mw": round(capacity, 6),
                        "source": source,
                        "previous_method": "ember_net_imports_x_land_borders",
                    }
                )
            continue
        importer = ""
        exporter = ""
        if net_imports.get(a, 0.0) > 0.0 and net_imports.get(b, 0.0) <= 0.0:
            importer, exporter = a, b
        elif net_imports.get(b, 0.0) > 0.0 and net_imports.get(a, 0.0) <= 0.0:
            importer, exporter = b, a
        row = {
            "a": a,
            "b": b,
            "importer": importer,
            "exporter": exporter,
            "border_km": 0.0,
            "importer_net_imports_twh": round(net_imports.get(importer, 0.0), 6),
            "importer_avg_import_mw": 0.0,
            "exporter_net_export_twh": round(-net_imports.get(exporter, 0.0), 6),
            "neighbor_export_sum_twh": 0.0,
            "share": 0.0,
            "avg_flow_mw": 0.0,
            "capacity_mw": round(capacity, 6),
            "headroom": None,
            "method": "public_capacity",
            "source": source,
            "capacity_note": item["note"],
        }
        links.append(row)
        by_pair[(a, b)] = row
        public_applied.append(
            {
                "a": a,
                "b": b,
                "capacity_mw": round(capacity, 6),
                "source": source,
                "previous_method": None,
            }
        )
    links.sort(key=lambda row: (row["a"], row["b"]))
    total_capacity = sum(row["capacity_mw"] for row in links)
    total_flow = sum(row["avg_flow_mw"] for row in links)
    out = {
        "meta": {
            "generated_by": "tools/data/energy/build_interconnects.py",
            "plan": "docs/plans/perf_pop_financas_plano.md, PARTE E, E5",
            "decision": "D-117 - comercio de energia entre redes nacionais vizinhas",
            "kind": "derived_data_plus_declared_model",
            "method": (
                "fluxo medio do importador = net_imports_twh Ember x 1e6/8760; "
                "repartido entre os vizinhos exportadores (fronteira terrestre "
                "de land_borders.json) proporcional a exportacao liquida Ember "
                "de cada um; capacidade = fluxo medio x headroom. Pares "
                "submarinos/sem exportador liquido usam a capacidade nominal "
                "publicada por par (method=public_capacity)"
            ),
            "headroom_factor": HEADROOM,
            "headroom_rationale": (
                "default de jogo declarado: a NTC (ENTSO-E) e a capacidade "
                "maxima de troca entre duas areas, um limite de capacidade e "
                "nao um fluxo medio; o modelo despacha energia diaria e a "
                "linha e dimensionada no fluxo medio x 1,25 para a variacao "
                "diaria caber sem saturar em metade dos dias; nas ligacoes "
                "com `method=public_capacity` vale o valor NOMINAL publicado"
            ),
            "submarine_links": {
                "included": True,
                "method": (
                    "capacidade nominal por par de fonte publica (Ofgem, "
                    "Energinet/Danish Energy Agency, ADB); nao e fluxo medio "
                    "derivado de Ember"
                ),
                "pairs": len(public_applied),
                "sources": sorted({row["source"] for row in public_applied}),
                "rationale": (
                    "QA t80: GB e DK nao tem par terrestre (ilhas) e o par "
                    "BGD-IND nao existe na derivacao porque a India e "
                    "importadora liquida no ano-base; sem essas linhas a "
                    "capacidade e ZERO e o par nao entra no modelo"
                ),
            },
            "excluded_pairs": [
                {"a": a, "b": b, "reason": reason}
                for (a, b), reason in sorted(OVERSEAS_ONLY_PAIRS.items())
            ],
            "rules": {
                "universe": "nacoes do cenario (data/definitions/scenario/cities.json)",
                "importer": "net_imports_twh > 0 no ano-base Ember",
                "exporter": "net_imports_twh < 0 no ano-base Ember",
                "min_flow_mw": MIN_FLOW_MW,
                "no_self_link": "par com a mesma nacao e descartado",
                "deterministic": "pares ordenados por (a, b) e vizinhos por tag",
            },
            "counts": {
                "nations_in_scenario": len(universe),
                "nations_with_ember_net_imports": len(net_imports),
                "importers": len(importers),
                "exporters": len(exporters),
                "links": len(links),
                "links_public_capacity": len(public_applied),
                "importers_with_link": len(per_importer),
                "unserved_importers": len(unserved),
                "total_avg_flow_mw": round(total_flow, 6),
                "total_capacity_mw": round(total_capacity, 6),
            },
            "known_limits": (
                "a reparticao proporcional ignora limites internos de cada "
                "exportador (o dado Ember e anual e nacional); ilhas e "
                "importadores sem vizinho exportador continuam sem compra - "
                "a ausencia e declarada em unserved_importers"
            ),
        },
        "_sources": SOURCES,
        "public_capacity_applied": public_applied,
        "unserved_importers": unserved,
        "by_importer": per_importer,
        "links": links,
    }
    return out


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    data = build()
    text = json.dumps(data, ensure_ascii=False, indent=1) + "\n"
    if args.check:
        if not OUT_PATH.exists():
            print(f"[interconnects] FALTA {OUT_PATH}")
            return 1
        current = OUT_PATH.read_text(encoding="utf-8")
        if current != text:
            print("[interconnects] DIFERENTE do gerado (rode sem --check)")
            return 1
        print("[interconnects] ok: arquivo identico ao gerado")
        return 0

    OUT_PATH.write_text(text, encoding="utf-8", newline="\n")
    counts = data["meta"]["counts"]
    print(
        "[interconnects] links=%d importadores=%d sem link=%d capacidade=%.1f MW"
        % (
            counts["links"],
            counts["importers_with_link"],
            counts["unserved_importers"],
            counts["total_capacity_mw"],
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
