"""Validador dos dados de energia do E1 (Parte E do plano).

Checa, arquivo por arquivo, o que o aceite exige:
  * `_sources.json` — toda fonte tem licenca verificada, com evidencia e a
    declaracao explicita de uso comercial; toda saida versionada aponta para uma
    fonte registrada;
  * `national_capacity.json` — matriz por nao na ordem do dado real 2024-2025:
    9 fontes por pais, soma das fontes == total da fonte (<= 0,5 MW), oleo
    marcado como lacuna, cobertura das 179 nacoes declarada;
  * `power_plants_2026.json` — coordenada dentro do mundo, provincia do raster
    DENTRO da nacao da usina, cidade dona da mesma nacao, e a soma por nacao
    bate com a lista de usinas;
  * `renewable_potential.json` — solar/eolico em faixa fisica e mensal presente;
  * `hydro_potential.json` — soma das provincias == ancora nacional (<= 0,01 MW)
    e as fracoes somam 1 dentro do pais;
  * `access_losses.json` — percentuais em 0..100 e cobertura das nacoes;
  * `land_borders.json` — pares simetricos (a,b)/(b,a), sem par consigo mesmo.

Uso:
    python tools/validate_energy_data.py [--require-commercial]

`--require-commercial` falha se alguma fonte usada tiver licenca restrita
(hoje o WorldClim). Sem a flag, ela aparece como BLOQUEIO no relatorio.
Saida: codigo 0 = tudo passou; 1 = alguma checagem falhou.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, ROOT)

import build_energy_common as ec  # noqa: E402

ENERGY = os.path.join(ec.DEFS, "energy")
TOLERANCE_MW = 0.5
TOLERANCE_HYDRO_MW = 0.01


class Report:
    def __init__(self) -> None:
        self.failures: list[str] = []
        self.warnings: list[str] = []
        self.checks = 0

    def check(self, condition: bool, message: str) -> None:
        self.checks += 1
        if not condition:
            self.failures.append(message)

    def warn(self, message: str) -> None:
        self.warnings.append(message)

    def summary(self) -> int:
        for message in self.warnings:
            print("AVISO: %s" % message)
        for message in self.failures:
            print("FALHA: %s" % message)
        print("%d checagens | %d falhas | %d avisos"
              % (self.checks, len(self.failures), len(self.warnings)))
        return 0 if not self.failures else 1


def load(name: str) -> dict:
    path = os.path.join(ENERGY, name)
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def find_non_finite(value, path="") -> list[str]:
    bad = []
    if isinstance(value, float):
        if not math.isfinite(value):
            bad.append(path)
    elif isinstance(value, dict):
        for key, item in value.items():
            bad.extend(find_non_finite(item, "%s.%s" % (path, key)))
    elif isinstance(value, list):
        for index, item in enumerate(value):
            bad.extend(find_non_finite(item, "%s[%d]" % (path, index)))
    return bad


def check_sources(report: Report, require_commercial: bool) -> None:
    registry = load("_sources.json")
    used_files = set()
    restricted = []
    for source in registry["sources"]:
        license_info = source.get("license") or {}
        report.check(bool(license_info.get("name")), "%s: sem nome de licenca" % source["id"])
        report.check(isinstance(license_info.get("commercial_ok"), bool),
                     "%s: commercial_ok nao declarado" % source["id"])
        report.check(bool(license_info.get("verified_on")),
                     "%s: sem data de verificacao da licenca" % source["id"])
        report.check(bool(license_info.get("evidence") or license_info.get("quote")),
                     "%s: sem evidencia da licenca" % source["id"])
        if source["status"] in ("used", "used_restricted"):
            used_files.update(source.get("used_in", []))
            if not license_info.get("commercial_ok"):
                restricted.append(source["id"])
    for relative in sorted(used_files):
        report.check(os.path.exists(os.path.join(ROOT, "..", relative)),
                     "arquivo consumidor ausente: %s" % relative)
    if restricted:
        message = ("fontes com licenca restrita em uso: %s (ver blocker em _sources.json)"
                   % ", ".join(restricted))
        if require_commercial:
            report.check(False, message)
        else:
            report.warn(message)


def check_national_capacity(report: Report) -> None:
    payload = load("national_capacity.json")
    tags, _ = ec.load_registry()
    nations = payload["nations"]
    matrix = payload["meta"]["source_map"]
    sources = ("hydro", "solar", "wind", "coal", "gas", "oil", "nuclear", "biomass", "other")
    report.check(set(matrix) == set(sources),
                 "source_map nao cobre as 9 fontes do jogo")
    report.check(not find_non_finite(payload), "national_capacity: NaN/inf no arquivo")
    missing = payload["meta"]["gaps"]["nations_without_ember"]
    report.check(len(nations) + len(missing) == len(tags),
                 "cobertura: %d nações + %d lacunas != %d do jogo"
                 % (len(nations), len(missing), len(tags)))
    report.check(len(tags) == 179, "lista de nações do jogo mudou (%d != 179)" % len(tags))
    for tag, entry in nations.items():
        report.check(set(entry["by_source"]) == set(sources),
                     "%s: by_source nao tem as 9 fontes" % tag)
        report.check(entry["year"] in (2024, 2025),
                     "%s: ano %s fora da janela 2024-2025" % (tag, entry["year"]))
        report.check(entry["total_mw"] > 0, "%s: total_mw <= 0" % tag)
        delta = entry["check"]["delta_mw"]
        report.check(abs(delta) <= TOLERANCE_MW,
                     "%s: soma das fontes difere do total em %.3f MW" % (tag, delta))
        report.check(entry["check"]["sum_matches_total"],
                     "%s: check.sum_matches_total falso" % tag)
    oil_nulls = sum(1 for entry in nations.values() if entry["by_source"]["oil"] is None)
    report.check(oil_nulls == len(nations),
                 "oleo deveria ser lacuna declarada em toda nação (%d/%d nulos)"
                 % (oil_nulls, len(nations)))
    report.check("oil" in payload["meta"]["gaps"], "national_capacity: lacuna do oleo sem motivo")
    comparison = payload["plant_comparison"]
    report.check(len(comparison) >= 150,
                 "plant_comparison cobre %d nações (esperado >= 150)" % len(comparison))
    for tag, entry in comparison.items():
        report.check(entry["national_total_mw"] > 0, "%s: comparação sem total nacional" % tag)
        for source, row in entry["by_source"].items():
            report.check(row["plant_mw"] >= 0, "%s/%s: MW de usina negativo" % (tag, source))


def check_plants(report: Report) -> None:
    payload = load("power_plants_2026.json")
    plants = payload["plants"]
    by_nation = payload["by_nation"]
    tag_by_code = {ec.nation_code(tag): tag for tag in ec.load_registry()[0]}
    atlas_to_tag = {ec.province_tag(tag): tag for tag in ec.load_registry()[0]}
    province_tag = {p["id"]: atlas_to_tag.get(p["nation"], "")
                    for p in ec.load_provinces()}
    city_tag = {c["id"]: c["tag"] for c in ec.load_cities()}

    total = 0.0
    per_nation: dict[str, float] = {}
    outside = 0
    conflicts = 0
    undeclared = []
    wrong_city = 0
    for plant in plants:
        lat, lon = plant["lat"], plant["lon"]
        mw = float(plant["capacity_mw"])
        report.check(mw > 0, "%s: capacity_mw <= 0" % plant["id"])
        total += mw
        if plant.get("nation"):
            per_nation[plant["nation"]] = per_nation.get(plant["nation"], 0.0) + mw
        if not (-90.0 <= lat <= 90.0 and -180.0 <= lon <= 180.0):
            outside += 1
            continue
        expected = tag_by_code.get(plant.get("country_iso3") or "", "")
        province_nation = ""
        if plant.get("province_id"):
            province_nation = province_tag.get(int(plant["province_id"]), "")
        declared = plant.get("location_conflict")
        if declared:
            conflicts += 1
            report.check(declared == province_nation,
                         "%s: location_conflict declarado %s != província %s"
                         % (plant["id"], declared, province_nation))
        if province_nation and expected and province_nation != expected:
            if not declared:
                undeclared.append((plant["id"], expected, province_nation))
            if plant.get("city_id") and city_tag.get(plant["city_id"], "") != expected:
                wrong_city += 1
    report.check(outside == 0, "%d usinas fora do mundo (-90..90 / -180..180)" % outside)
    report.check(not undeclared,
                 "%d usinas com província de outra nação SEM location_conflict: %s"
                 % (len(undeclared), undeclared[:5]))
    meta_conflicts = payload["meta"].get("location_conflicts") or {}
    report.check(meta_conflicts.get("count") == conflicts,
                 "meta.location_conflicts.count (%s) != conflitos reais (%d)"
                 % (meta_conflicts.get("count"), conflicts))
    report.check(wrong_city <= conflicts,
                 "%d usinas em cidade de outra nação (conflitos declarados: %d)"
                 % (wrong_city, conflicts))
    mw_without_nation = sum(float(p["capacity_mw"]) for p in plants if not p.get("nation"))
    sum_nation = round(sum(v for v in per_nation.values()) + mw_without_nation, 3)
    report.check(abs(sum_nation - total) < 0.01,
                 "soma por nação + sem nação (%.3f) != soma das usinas (%.3f)"
                 % (sum_nation, total))
    for tag, entry in by_nation.items():
        report.check(abs(entry["total_mw"] - per_nation.get(tag, 0.0)) < 0.01,
                     "%s: by_nation.total_mw != soma das usinas" % tag)
    report.check(abs(payload["meta"]["counts"]["total_mw"] - round(total, 3)) <= 0.01,
                 "meta.counts.total_mw != soma das usinas")
    if conflicts:
        report.warn("%d usinas com nação do GPPD != nação da província do atlas "
                    "(declaradas em location_conflict, ver meta.top_pairs)" % conflicts)


def check_renewables(report: Report) -> None:
    payload = load("renewable_potential.json")
    provinces = payload["by_province"]
    report.check(len(provinces) > 15000, "renewable_potential: poucas províncias")
    bad_solar = bad_wind = 0
    for entry in provinces:
        solar = entry.get("solar") or {}
        wind = entry.get("wind") or {}
        solar_value = solar.get("kwh_m2_day_mean")
        wind_value = wind.get("m_s_mean")
        if solar_value is not None and not (0.0 < solar_value < 9.0):
            bad_solar += 1
        if wind_value is not None and not (0.0 <= wind_value < 20.0):
            bad_wind += 1
        hydro = entry.get("hydro") or {}
        raw = hydro.get("raw_index")
        report.check(raw is None or raw >= 0.0,
                     "província %s: raw_index negativo" % entry.get("province_id"))
    report.check(bad_solar == 0, "%d províncias com solar fora da faixa física" % bad_solar)
    report.check(bad_wind == 0, "%d províncias com vento fora da faixa física" % bad_wind)
    cities = payload["by_city"]
    for city_id, entry in list(cities.items())[:50]:
        months = (entry.get("solar") or {}).get("monthly_kwh_m2_day") or []
        report.check(len(months) == 12, "%s: série solar mensal != 12" % city_id)


def check_hydro(report: Report) -> None:
    payload = load("hydro_potential.json")
    nations = payload["by_nation"]
    provinces = payload["by_province"]
    totals: dict[str, float] = {}
    shares: dict[str, float] = {}
    for entry in provinces:
        nation = entry["nation"]
        if entry.get("potential_mw") is not None:
            totals[nation] = totals.get(nation, 0.0) + float(entry["potential_mw"])
        if entry.get("share_of_nation") is not None:
            shares[nation] = shares.get(nation, 0.0) + float(entry["share_of_nation"])
    for tag, entry in nations.items():
        anchor = entry["technical_potential_mw"]
        if anchor is None:
            report.check(entry["national_anchor_missing"],
                         "%s: sem âncora mas national_anchor_missing falso" % tag)
            continue
        report.check(anchor > 0, "%s: âncora <= 0" % tag)
        report.check(not entry["national_anchor_missing"],
                     "%s: âncora presente mas marcada como ausente" % tag)
        if entry.get("distribution_missing"):
            report.check(abs(totals.get(tag, 0.0)) < 1e-9,
                         "%s: âncora sem proxy mas províncias receberam MW" % tag)
            continue
        covered = totals.get(tag, 0.0)
        report.check(abs(covered - anchor) <= TOLERANCE_HYDRO_MW,
                     "%s: províncias somam %.3f MW vs âncora %.3f MW"
                     % (tag, covered, anchor))
        if tag in shares:
            report.check(abs(shares[tag] - 1.0) <= 1e-5,
                         "%s: frações das províncias somam %.6f" % (tag, shares[tag]))
    world = payload["meta"]["counts"]["world_technical_potential_mw"]
    report.check(3000000.0 < world < 7000000.0,
                 "potencial técnico mundial declarado fora da faixa de sanidade: %.0f MW" % world)
    report.check(not find_non_finite(payload), "hydro_potential: NaN/inf no arquivo")
    for city_id, entry in list(payload["by_city"].items())[:20]:
        report.check(entry["potential_mw"] is None or entry["potential_mw"] >= 0.0,
                     "%s: potencial de cidade negativo" % city_id)


def check_access(report: Report) -> None:
    payload = load("access_losses.json")
    by_country = payload["by_country"]
    tags, _ = ec.load_registry()
    playable = [tag for tag in tags if ec.nation_code(tag) in by_country]
    report.check(len(playable) >= 170,
                 "access_losses: só %d das 179 nações jogáveis" % len(playable))
    fields = [field["key"] for field in payload["meta"]["fields"]]
    for iso3, entry in by_country.items():
        for key in fields:
            value = (entry.get(key) or {}).get("value")
            if value is None:
                continue
            report.check(0.0 <= float(value) <= 100.0,
                         "%s/%s: percentual fora de 0..100 (%s)" % (iso3, key, value))


def check_borders(report: Report) -> None:
    payload = load("land_borders.json")
    rows = payload["borders"]
    pairs = [(row["a"], row["b"]) for row in rows]
    report.check(all(a != b for a, b in pairs), "land_borders: par de nação consigo mesma")
    report.check(all(a < b for a, b in pairs),
                 "land_borders: par não normalizado (a < b)")
    report.check(len(set(pairs)) == len(pairs), "land_borders: par repetido")
    neighbors = payload["neighbors"]
    degree: dict[str, int] = {}
    for row in payload["borders"]:
        degree[row["a"]] = degree.get(row["a"], 0) + 1
        degree[row["b"]] = degree.get(row["b"], 0) + 1
    for tag, count in neighbors.items():
        report.check(int(count) == degree.get(tag, 0),
                     "%s: vizinhos declarados %s != pares %d"
                     % (tag, count, degree.get(tag, 0)))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--require-commercial", action="store_true",
                        help="falha se uma fonte usada tiver licença restrita")
    args = parser.parse_args()
    report = Report()
    check_sources(report, args.require_commercial)
    check_national_capacity(report)
    check_plants(report)
    check_renewables(report)
    check_hydro(report)
    check_access(report)
    check_borders(report)
    return report.summary()


if __name__ == "__main__":
    sys.exit(main())
