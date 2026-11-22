"""Validador dos dados de energia do E1 (Parte E do plano).

Checa, arquivo por arquivo, o que o aceite exige:
  * `_sources.json` — toda fonte tem licenca verificada, com evidencia e a
    declaracao explicita de uso comercial; toda saida versionada aponta para uma
    fonte registrada; fonte na lista de bloqueio nao comercial nunca pode estar
    em uso;
  * `national_capacity.json` — matriz por nao na ordem do dado real 2024-2025:
    9 fontes por pais, soma das fontes == total da fonte (<= 0,5 MW), ano
    escolhido com `year_reason` declarado, oleo marcado como lacuna, cobertura
    das 179 nacoes declarada;
  * `power_plants_base_2021_gppd.json` — coordenada dentro do mundo, provincia
    do raster DENTRO da nacao da usina, cidade dona da mesma nacao, e a soma por
    nacao bate com a lista de usinas;
  * `power_plants_2026_scaled.json` — `method` em toda usina, alvo Ember por
    nacao x fonte (<= 0,01 MW) e usinas sem alvo nacional declaradas;
  * `renewable_potential.json` — solar/eolico em faixa fisica e mensal presente;
    hidrico com PET de licenca comercial (CHELSA CC0), sem WorldClim/CGIAR;
  * `hydro_potential.json` — soma das provincias == ancora nacional (<= 0,01 MW)
    e as fracoes somam 1 dentro do pais;
  * `access_losses.json` — percentuais em 0..100 e cobertura das nacoes, com o
    WDI ligado às tags do jogo (inclui PSE->PSX);
  * `land_borders.json` — pares simetricos (a,b)/(b,a), sem par consigo mesmo.

Uso:
    python tools/validate_energy_data.py [--require-commercial]
    python tools/validate_energy_data.py --selftest-commercial

`--require-commercial` falha se alguma fonte usada tiver licenca restrita ou
estiver na lista de bloqueio (WorldClim, CGIAR-CSI PET v3.1, IRENA ELECSTAT,
Global Energy Monitor). Sem a flag, elas aparecem como BLOQUEIO no relatorio.
`--selftest-commercial` prova que a guarda pega uma fonte nao comercial usada,
mesmo com `commercial_ok: true` mentindo no registro.
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

# Lista de bloqueio EXPLÍCITA de fontes cujo termo é incompatível com uso
# comercial — independente do que o registro diga. O WorldClim é o caso raiz
# (uso acadêmico/não comercial); o PET CGIAR-CSI v3.1 é derivado do WorldClim e
# "provided for non-commercial use"; IRENA e GEM também restringem uso comercial.
NON_COMMERCIAL_SOURCE_IDS = {
    "worldclim_21",
    "cgiar_pet_v3_1",
    "irena_elecstat",
    "gem_trackers",
}
# Marcadores textuais: pega uma fonte que declara `commercial_ok: true` mas cujo
# nome/evidência/citação diz o contrário.
NON_COMMERCIAL_LICENSE_MARKERS = (
    "non-commercial",
    "noncommercial",
    "não comercial",
    "nao comercial",
    "academic use",
    "academic/non-commercial",
    "uso acadêmico",
    "uso academico",
    "not allowed without prior permission",
)


def commercial_violations(sources: list[dict]) -> list[str]:
    """Fontes em uso que violam a política comercial (lista de bloqueio + texto)."""
    violations = []
    for source in sources:
        if source.get("status") not in ("used", "used_restricted"):
            continue
        license_info = source.get("license") or {}
        if source.get("id") in NON_COMMERCIAL_SOURCE_IDS:
            violations.append("%s: id na lista de bloqueio não comercial" % source["id"])
            continue
        if license_info.get("commercial_ok") is not True:
            violations.append("%s: commercial_ok != true" % source["id"])
            continue
        text = " ".join(str(license_info.get(key, "")) for key in
                        ("name", "evidence", "quote", "url")).lower()
        hits = [marker for marker in NON_COMMERCIAL_LICENSE_MARKERS if marker in text]
        if hits:
            violations.append("%s: texto da licença com marcador restritivo (%s)"
                              % (source["id"], hits[0]))
    return violations


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
            report.check(bool(source.get("accessed_at_utc")),
                         "%s: fonte usada sem accessed_at_utc" % source["id"])
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
    # Guarda independente do registro: lista de bloqueio + texto da licença.
    violations = commercial_violations(registry["sources"])
    if violations:
        message = ("fontes em uso que violam a política comercial: %s"
                   % "; ".join(violations))
        if require_commercial:
            report.check(False, message)
        else:
            report.warn(message)
    # A decisão D-115(c) tirou o WorldClim do E1; ele não pode voltar como fonte usada.
    used_ids = {source["id"] for source in registry["sources"]
                if source["status"] in ("used", "used_restricted")}
    report.check("worldclim_21" not in used_ids,
                 "WorldClim ainda consta como fonte usada no E1 (D-115c)")
    # re-QA t33: o PET CGIAR-CSI v3.1 é derivado do WorldClim (uso não comercial).
    report.check("cgiar_pet_v3_1" not in used_ids,
                 "PET CGIAR-CSI v3.1 ainda consta como fonte usada (derivado do WorldClim)")
    used_licenses = {source["id"]: source.get("license") or {}
                     for source in registry["sources"]
                     if source["id"] in used_ids}
    for source_id in ("ember_yearly", "gppd_wri", "chelsa_pet_v21", "chirps_v3"):
        license_info = used_licenses.get(source_id) or {}
        report.check(license_info.get("commercial_ok") is True,
                     "%s: fonte base sem licença comercial declarada" % source_id)


def selftest_commercial() -> int:
    """Prova que a guarda comercial pega uma fonte não comercial em uso."""
    real = load("_sources.json")["sources"]
    if commercial_violations(real):
        print("SELFTEST FALHOU: o registro real já viola a política comercial")
        return 1
    fake = [{
        "id": "fake_noncommercial",
        "status": "used",
        "license": {
            "name": "academic use only",
            "verified_on": "2026-11-22",
            "commercial_ok": True,  # mente de propósito
            "evidence": "provided for non-commercial use",
        },
    }]
    violations = commercial_violations(fake)
    if not violations:
        print("SELFTEST FALHOU: fonte não comercial usada passou pela guarda")
        return 1
    by_id = [{
        "id": "worldclim_21",
        "status": "used",
        "license": {"name": "CC BY 4.0", "commercial_ok": True,
                    "verified_on": "2026-11-22", "evidence": "ok"},
    }]
    if not commercial_violations(by_id):
        print("SELFTEST FALHOU: id da lista de bloqueio não foi barrado")
        return 1
    print("SELFTEST OK: %s" % violations[0])
    return 0


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
        reason = entry.get("year_reason")
        report.check(reason in ("all_9_sources_and_total", "all_9_sources_no_total",
                                "total_only_partial_sources",
                                "partial_sources_any_positive"),
                     "%s: year_reason ausente/inválido (%s)" % (tag, reason))
        missing_sources = entry.get("sources_missing")
        zero_absent = entry.get("sources_absent_as_zero")
        report.check(isinstance(missing_sources, list),
                     "%s: sources_missing não é lista" % tag)
        report.check(isinstance(zero_absent, list),
                     "%s: sources_absent_as_zero não é lista" % tag)
        if reason in ("all_9_sources_and_total", "all_9_sources_no_total"):
            report.check(set(missing_sources or []) <= set(zero_absent or []),
                         "%s: fonte ausente sem prova de zero (sources_absent_as_zero)"
                         % tag)
            report.check(all(value is not None
                             for key, value in entry["by_source"].items()
                             if key != "oil"),
                         "%s: ano completo com fonte null (fora do óleo)" % tag)
        else:
            report.check(bool(missing_sources),
                         "%s: ano incompleto sem sources_missing declarado" % tag)
        if reason == "all_9_sources_no_total":
            report.check(entry.get("total_source") == "sum_sources",
                         "%s: sem Total generation mas total_source != sum_sources" % tag)
    reasons = payload["meta"]["counts"].get("nations_year_reason") or {}
    report.check(sum(reasons.values()) == len(nations),
                 "national_capacity: nations_year_reason não cobre todas as nações")
    report.check(nations["DEU"]["year"] == 2025,
                 "national_capacity: DEU deveria usar 2025 (9 fontes completas)")
    report.check(nations["DEU"]["year_reason"] == "all_9_sources_no_total",
                 "national_capacity: DEU 2025 deve ser all_9_sources_no_total")
    report.check(abs(nations["DEU"]["total_mw"] - 268380.0) <= 1.0,
                 "national_capacity: DEU 2025 deveria somar ~268.380 MW (%.1f)"
                 % nations["DEU"]["total_mw"])
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
    payload = load("power_plants_base_2021_gppd.json")
    plants = payload["plants"]
    by_nation = payload["by_nation"]
    by_city = payload["by_city"]
    atlas_to_tag = {ec.province_tag(tag): tag for tag in ec.load_registry()[0]}
    province_tag = {p["id"]: atlas_to_tag.get(p["nation"], "")
                    for p in ec.load_provinces()}
    city_tag = {c["id"]: c["tag"] for c in ec.load_cities()}
    causes = {"gppd_coord_error", "deleted_territory", "border_pixel", "occupied"}
    discarded_ids = {item["id"] for item in
                     (payload["meta"].get("discarded") or {}).get("items", [])}

    total = 0.0
    per_nation: dict[str, float] = {}
    city_totals: dict[str, float] = {}
    outside = 0
    conflicts = 0
    by_cause: dict[str, int] = {}
    undeclared = []
    wrong_city = 0
    zero_or_negative = []
    for plant in plants:
        lat, lon = plant["lat"], plant["lon"]
        mw = float(plant["capacity_mw"])
        report.check(mw > 0, "%s: capacity_mw <= 0" % plant["id"])
        if mw <= 0:
            zero_or_negative.append(plant["id"])
        total += mw
        tag = plant.get("nation")
        if tag:
            per_nation[tag] = per_nation.get(tag, 0.0) + mw
        if plant.get("city_id"):
            cid = plant["city_id"]
            city_totals[cid] = city_totals.get(cid, 0.0) + mw
            report.check(city_tag.get(cid, "") == (tag or ""),
                         "%s: nation %s != cidade %s" % (plant["id"], tag, cid))
        if not (-90.0 <= lat <= 90.0 and -180.0 <= lon <= 180.0):
            outside += 1
            continue
        report.check(bool(plant.get("gppd_country")),
                     "%s: sem gppd_country (rastreio do GPPD)" % plant["id"])
        report.check(plant["id"] not in discarded_ids,
                     "%s: usina descartada ainda está no parque" % plant["id"])
        province_nation = ""
        if plant.get("province_id"):
            province_nation = province_tag.get(int(plant["province_id"]), "")
        report.check(not plant.get("province_nation") or
                     plant["province_nation"] == province_nation,
                     "%s: province_nation != província do atlas" % plant["id"])
        declared = plant.get("location_conflict")
        cause = plant.get("location_cause")
        if declared:
            conflicts += 1
            report.check(cause in causes,
                         "%s: location_conflict sem causa válida (%s)" % (plant["id"], cause))
            if cause:
                by_cause[cause] = by_cause.get(cause, 0) + 1
            report.check(declared == province_nation,
                         "%s: location_conflict declarado %s != província %s"
                         % (plant["id"], declared, province_nation))
            report.check(not tag or tag == province_nation,
                         "%s: nation %s != controlador do atlas %s"
                         % (plant["id"], tag, province_nation))
        elif cause:
            # Só é aceitável em usina sem cidade (território sem nação jogável).
            report.check(not plant.get("city_id"),
                         "%s: location_cause sem location_conflict" % plant["id"])
        if province_nation and plant.get("gppd_country_game") and \
                province_nation != plant["gppd_country_game"] and not declared:
            undeclared.append((plant["id"], plant["gppd_country_game"], province_nation))
        if plant.get("city_id") and tag and city_tag.get(plant["city_id"], "") != tag:
            wrong_city += 1
    report.check(outside == 0, "%d usinas fora do mundo (-90..90 / -180..180)" % outside)
    report.check(not undeclared,
                 "%d usinas com província de outra nação SEM location_conflict: %s"
                 % (len(undeclared), undeclared[:5]))
    report.check(not zero_or_negative, "%d usinas com MW <= 0: %s"
                 % (len(zero_or_negative), zero_or_negative[:5]))
    meta_conflicts = payload["meta"].get("location_conflicts") or {}
    report.check(meta_conflicts.get("count") == conflicts,
                 "meta.location_conflicts.count (%s) != conflitos reais (%d)"
                 % (meta_conflicts.get("count"), conflicts))
    reported = meta_conflicts.get("by_cause") or {}
    for cause in causes:
        if cause == "gppd_coord_error":
            continue
        report.check((reported.get(cause) or {}).get("plants", 0) == by_cause.get(cause, 0),
                     "meta.location_conflicts.by_cause.%s (%s) != real (%d)"
                     % (cause, (reported.get(cause) or {}).get("plants"), by_cause.get(cause, 0)))
    coord = meta_conflicts.get("coord_error") or {}
    report.check(coord.get("cause") == "gppd_coord_error",
                 "meta.location_conflicts.coord_error ausente")
    report.check(coord.get("discarded") == len(discarded_ids),
                 "coord_error.discarded != lista de descartadas")
    report.check("gppd_coord_error" not in reported,
                 "gppd_coord_error entrou em by_cause/count (deve ficar fora do count)")
    report.check(wrong_city == 0,
                 "%d usinas em cidade de nação diferente de `nation`" % wrong_city)

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
    # Invariante QA: Σ by_city[nacao] == by_nation[nacao] (<= 0.01 MW).
    for tag, entry in by_nation.items():
        city_sum = round(sum(c["total_mw"] for c in by_city.values()
                             if c["nation"] == tag), 3)
        report.check(abs(city_sum - entry["total_mw"]) <= 0.01,
                     "%s: Σ by_city %.3f != by_nation %.3f"
                     % (tag, city_sum, entry["total_mw"]))
    inv = payload["meta"].get("invariant") or {}
    report.check(inv.get("ok") is True and abs(float(inv.get("difference_mw", 1.0))) <= 0.01,
                 "meta.invariant do parque base não fecha")
    report.check(abs(float(inv.get("total_mw", 0.0)) - total) <= 0.01,
                 "meta.invariant.total_mw != soma das usinas")
    if conflicts:
        report.warn("%d usinas com nação do GPPD != nação do atlas "
                    "(declaradas em location_conflict, por causa)" % conflicts)


def check_scaled_plants(report: Report) -> None:
    payload = load("power_plants_2026_scaled.json")
    plants = payload["plants"]
    meta = payload["meta"]
    capacity = load("national_capacity.json")["nations"]
    city_by_id = {c["id"]: c for c in ec.load_cities()}
    report.check(meta.get("data_year") == 2026, "scaled: data_year != 2026")
    report.check(meta.get("capacity_target_years") == [2024, 2025],
                 "scaled: capacity_target_years != [2024, 2025]")
    report.check(meta.get("base_reference_year") == 2021,
                 "scaled: base_reference_year != 2021")

    by_city: dict[str, float] = {}
    by_nation: dict[str, float] = {}
    by_nation_source: dict[tuple[str, str], float] = {}
    synthetic = 0
    zero = 0
    for plant in plants:
        mw = float(plant["capacity_mw"])
        report.check(mw >= 0, "%s: capacity_mw negativo" % plant["id"])
        if mw == 0:
            zero += 1
            report.check(plant.get("scaling_note") in ("retired_by_ember_target_zero",
                                                        "scaled_to_zero"),
                         "%s: MW 0 sem nota de fonte desativada" % plant["id"])
        tag = plant.get("nation")
        source = plant.get("fuel_group")
        if tag is not None:
            report.check(tag in capacity or tag in meta["gaps"]["nations_without_ember"],
                         "%s: nation %s sem matriz Ember nem lacuna" % (plant["id"], tag))
        report.check(source in ("hydro", "solar", "wind", "coal", "gas", "oil",
                                "nuclear", "biomass", "other"),
                     "%s: fuel_group inválido" % plant["id"])
        if tag:
            by_nation[tag] = by_nation.get(tag, 0.0) + mw
            by_nation_source[(tag, source)] = by_nation_source.get(
                (tag, source), 0.0) + mw
        cid = plant.get("city_id")
        if cid:
            report.check(cid in city_by_id, "%s: city_id inexistente" % plant["id"])
            report.check(city_by_id.get(cid, {}).get("tag") == tag,
                         "%s: cidade %s não é da nação %s" % (plant["id"], cid, tag))
            by_city[tag] = by_city.get(tag, 0.0) + mw
        if plant.get("synthetic"):
            synthetic += 1
            report.check(tag is not None, "%s: sintética sem nação" % plant["id"])
            report.check(plant.get("scaling_method") == "ember_city_allocation",
                         "%s: sintética sem método de rateio" % plant["id"])
            report.check(bool(cid), "%s: sintética sem cidade" % plant["id"])
            report.check(bool(plant.get("scaling_note")),
                         "%s: sintética sem base de peso declarada" % plant["id"])
        else:
            report.check(plant.get("scaling_method") in
                         ("scaled_to_ember", "unscaled"),
                         "%s: scaling_method inválido" % plant["id"])
            report.check("capacity_mw_gppd" in plant,
                         "%s: sem capacity_mw_gppd" % plant["id"])
            method = plant.get("method")
            report.check(method in ("gppd_2021_scaled_to_ember",
                                    "gppd_2021_unadjusted"),
                         "%s: method ausente/inválido na usina real (%s)"
                         % (plant["id"], method))
            report.check(
                (method == "gppd_2021_scaled_to_ember") ==
                (plant.get("scaling_method") == "scaled_to_ember"),
                "%s: method e scaling_method discordam" % plant["id"])
    report.check(synthetic > 0, "scaled: nenhuma usina sintética (esperado > 0)")
    report.check(zero >= 0, "scaled: contador de zeros inválido")

    # Invariante QA: Σ by_city[nacao] == by_nation[nacao] (<= 0.01 MW).
    for tag in sorted(set(by_city) | set(by_nation)):
        report.check(abs(by_city.get(tag, 0.0) - by_nation.get(tag, 0.0)) <= 0.01,
                     "%s: by_city %.3f != by_nation %.3f"
                     % (tag, by_city.get(tag, 0.0), by_nation.get(tag, 0.0)))
    inv = meta.get("invariant") or {}
    report.check(inv.get("ok") is True and abs(float(inv.get("difference_mw", 1.0))) <= 0.01,
                 "scaled: meta.invariant não fecha")
    total_scaled = round(sum(float(p["capacity_mw"]) for p in plants), 3)
    mw_without_nation = round(sum(float(p["capacity_mw"]) for p in plants
                                  if not p.get("nation")), 3)
    report.check(abs(total_scaled - sum(by_nation.values()) - mw_without_nation) <= 0.05,
                 "scaled: total (%.3f) != by_nation (%.3f) + sem nação (%.3f)"
                 % (total_scaled, sum(by_nation.values()), mw_without_nation))
    report.check(abs(float(meta["counts"]["total_mw"]) - total_scaled) <= 0.01,
                 "scaled: meta.counts.total_mw != soma das usinas")
    report.check(abs(float(meta["counts"]["mw_without_city"]) - mw_without_nation) <= 0.01,
                 "scaled: meta.counts.mw_without_city != soma sem nação")

    # Desvio por nação x fonte vs Ember (declarado no próprio arquivo e
    # recalculado das usinas).
    check = meta.get("nation_source_check") or {}
    failures = []
    for tag, entry in capacity.items():
        for source, target in (entry.get("by_source") or {}).items():
            key = "%s/%s" % (tag, source)
            row = check.get(key)
            report.check(row is not None,
                         "scaled: falta nation_source_check %s" % key)
            if row is None:
                continue
            if target is None:
                report.check(row.get("target_mw") is None,
                             "%s: alvo null mas target_mw preenchido" % key)
                continue
            scaled = round(by_nation_source.get((tag, source), 0.0), 3)
            report.check(abs(scaled - float(target)) <= 0.01,
                         "%s: usinas somam %.3f MW vs Ember %.3f MW"
                         % (key, scaled, target))
            report.check(row.get("ok") is True,
                         "%s: nation_source_check.ok falso" % key)
            if not row.get("ok"):
                failures.append(key)
    report.check(not failures, "scaled: %d falhas em nation_source_check: %s"
                 % (len(failures), failures[:8]))
    report.check(not meta.get("gaps", {}).get("nation_source_failures"),
                 "scaled: meta.gaps.nation_source_failures não está vazio")

    # Usinas reais sem alvo nacional (nação sem matriz Ember) — declaradas.
    gap = (meta.get("gaps") or {}).get("national_target_missing") or {}
    target_missing = [plant for plant in plants
                      if plant.get("scaling_note") == "national_target_missing"]
    report.check(gap.get("plants") == len(target_missing),
                 "scaled: gaps.national_target_missing.plants (%s) != real (%d)"
                 % (gap.get("plants"), len(target_missing)))
    missing_mw = round(sum(float(p["capacity_mw"]) for p in target_missing), 3)
    report.check(abs(float(gap.get("mw", -1.0)) - missing_mw) <= 0.01,
                 "scaled: gaps.national_target_missing.mw != real (%.3f)" % missing_mw)
    report.check(set(gap.get("by_nation") or {}) ==
                 {plant["nation"] for plant in target_missing},
                 "scaled: gaps.national_target_missing.by_nation != usinas afetadas")
    for plant in target_missing:
        report.check(plant.get("scaling_method") == "unscaled",
                     "%s: national_target_missing sem scaling_method unscaled"
                     % plant["id"])

    # Óleo: não escalonado, porque a Ember não separa.
    base = load("power_plants_base_2021_gppd.json")["by_nation"]
    for tag, entry in base.items():
        base_oil = float((entry.get("by_fuel") or {}).get("oil") or 0.0)
        row = check.get("%s/oil" % tag)
        if row is not None and row.get("target_mw") is None:
            report.check(abs(float(row.get("scaled_mw", 0.0)) - base_oil) <= 0.01,
                         "%s: óleo escalonado sem alvo Ember" % tag)


def check_renewables(report: Report) -> None:
    payload = load("renewable_potential.json")
    meta = payload["meta"]
    solar_wind = meta.get("solar_wind") or {}
    report.check((solar_wind.get("license") or {}).get("commercial_ok") is True,
                 "renewable_potential: licença solar/eólica não comercial")
    source_text = " ".join(str(solar_wind.get(key, "")) for key in
                           ("solar_source", "wind_source"))
    report.check("WorldClim" not in source_text,
                 "renewable_potential: solar/eólico ainda cita WorldClim (D-115c)")
    hydro_meta = meta.get("hydro") or {}
    hydro_text = " ".join(str(hydro_meta.get(key, "")) for key in
                          ("precip", "pet", "relief", "runoff"))
    report.check("CHIRPS" in str(hydro_meta.get("precip", "")),
                 "renewable_potential: hídrico sem CHIRPS")
    report.check("CHELSA" in str(hydro_meta.get("pet", "")),
                 "renewable_potential: hídrico sem PET CHELSA")
    report.check("CGIAR" not in hydro_text,
                 "renewable_potential: hídrico ainda usa PET CGIAR-CSI (não comercial)")
    pet_source = hydro_meta.get("pet_source") or {}
    pet_dataset = "%s %s" % (pet_source.get("dataset", ""),
                             pet_source.get("forcing", ""))
    report.check("WorldClim" not in pet_dataset,
                 "renewable_potential: PET ainda derivado do WorldClim (D-115c)")
    report.check("ERA5" in str(pet_source.get("forcing", "")),
                 "renewable_potential: PET sem forçante ERA5 declarada")
    pet_license = pet_source.get("license") or {}
    report.check(pet_license.get("commercial_ok") is True,
                 "renewable_potential: licença do PET não comercial")
    report.check("CC0" in str(pet_license.get("name", "")) or
                 "domínio público" in str(pet_license.get("name", "")),
                 "renewable_potential: PET sem licença pública declarada")
    report.check(bool(pet_license.get("verified_on")) and
                 bool(pet_license.get("url")),
                 "renewable_potential: PET sem URL/data de verificação da licença")
    provinces = payload["by_province"]
    report.check(len(provinces) > 15000, "renewable_potential: poucas províncias")
    bad_solar = bad_wind = 0
    for entry in provinces:
        solar = entry.get("solar") or {}
        wind = entry.get("wind") or {}
        for label, block in (("solar", solar), ("wind", wind)):
            if not block:
                continue
            method = block.get("sample_method")
            report.check(method in ("api_point", "nearest_cached_province"),
                         "província %s/%s: sample_method inválido (%s)"
                         % (entry.get("province_id"), label, method))
            if method == "nearest_cached_province":
                report.check(block.get("sample_source_pid") is not None and
                             block.get("sample_distance_km") is not None,
                             "província %s/%s: fallback sem origem/distância declarada"
                             % (entry.get("province_id"), label))
        solar_value = solar.get("kwh_m2_day_mean")
        wind_value = wind.get("m_s_mean")
        if solar_value is not None and not (0.0 < solar_value < 9.0):
            bad_solar += 1
        if wind_value is not None and not (0.0 < wind_value < 20.0):
            bad_wind += 1
        hydro = entry.get("hydro") or {}
        raw = hydro.get("raw_index")
        report.check(raw is None or raw >= 0.0,
                     "província %s: raw_index negativo" % entry.get("province_id"))
        if hydro.get("confidence") == "no_data":
            report.check(hydro.get("raw_index") == 0.0,
                         "província %s: no_data com raw_index != 0" % entry.get("province_id"))
    report.check(bad_solar == 0, "%d províncias com solar fora da faixa física" % bad_solar)
    report.check(bad_wind == 0, "%d províncias com vento fora da faixa física" % bad_wind)
    cities = payload["by_city"]
    for city_id, entry in list(cities.items())[:50]:
        solar = entry.get("solar") or {}
        months = solar.get("monthly_kwh_m2_day") or []
        report.check(len(months) == 12, "%s: série solar mensal != 12" % city_id)
        wind = entry.get("wind") or {}
        if wind and wind.get("monthly_available") is False:
            report.check(wind.get("monthly_m_s") is None,
                         "%s: vento sem série mensal mas com monthly_m_s" % city_id)
    report.check((meta.get("counts") or {}).get("provinces_outside_chirps", 0) > 0,
                 "renewable_potential: lacuna CHIRPS (>60N) não declarada")


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
    playable = {entry["nation"] for entry in by_country.values()
                if entry.get("nation")}
    report.check(playable <= set(tags),
                 "access_losses: nation fora da lista de nações do jogo: %s"
                 % sorted(playable - set(tags))[:5])
    report.check(len(playable) >= 176,
                 "access_losses: só %d das 179 nações jogáveis" % len(playable))
    for tag in ("SSD", "XKX", "PSX"):
        report.check(tag in playable,
                     "access_losses: WDI não ligado à tag do jogo %s" % tag)
    bridged = payload["meta"].get("bridged_rows") or []
    report.check(any(row.get("wdi_iso3") == "PSE" and row.get("nation") == "PSX"
                     for row in bridged),
                 "access_losses: alias PSE->PSX não registrado em meta.bridged_rows")
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
    parser.add_argument("--selftest-commercial", action="store_true",
                        help="prova que a guarda comercial pega fonte não comercial")
    args = parser.parse_args()
    if args.selftest_commercial:
        return selftest_commercial()
    report = Report()
    check_sources(report, args.require_commercial)
    check_national_capacity(report)
    check_plants(report)
    check_scaled_plants(report)
    check_renewables(report)
    check_hydro(report)
    check_access(report)
    check_borders(report)
    return report.summary()


if __name__ == "__main__":
    sys.exit(main())
