#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Valida data/definitions/economy/land_property.json (fase B6, patrimônio da região).

Checa:
  1. JSON parseável, UTF-8 sem BOM, LF, formato canônico (indent=2, chaves ordenadas);
  2. as nações do arquivo são exatamente as de data/definitions/scenario/nations.json;
  3. todo campo obrigatório existe e tem tipo coerente (número/null);
  4. agricultural_land_ha > 0 quando presente, year plausível, source referenciado;
  5. land_gini em (0, 1) quando presente, com year/source coerentes e null quando não há;
  6. basis coerente com a presença (ou não) de agricultural_land_ha;
  7. toda chave citada em `source`/`land_gini_source` existe em `sources` e todo `sources`
     tem title/publisher/year/license/url;
  8. `coverage` bate com a contagem recalculada (inclusive listas de quem não tem dado);
  9. `regional_reference` (medianas derivadas) é consistente com os valores do arquivo;
 10. `state_distribution.file` nulo exige reason+rule; se existir
     data/definitions/economy/land_property_states.json, valida hectares por estado
     contra data/definitions/population/state_regions.json.

Uso:
    py -3 tools/validate_land_property.py

Sai 0 quando está tudo consistente; 1 com a lista de problemas.
"""

from __future__ import annotations

import datetime
import json
import os
import statistics
import sys
from typing import Dict, List

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NATIONS_JSON = os.path.join(ROOT, "data", "definitions", "scenario", "nations.json")
LAND_JSON = os.path.join(ROOT, "data", "definitions", "economy", "land_property.json")
STATES_JSON = os.path.join(ROOT, "data", "definitions", "economy", "land_property_states.json")
STATE_REGIONS = os.path.join(ROOT, "data", "definitions", "population", "state_regions.json")

BASIS = ("observed", "regional_estimate", "none")
REQUIRED_FIELDS = ("agricultural_land_ha", "year", "source", "land_gini",
                   "land_gini_year", "land_gini_source", "basis")
GINI_YEAR_MIN = 1929
HA_MAX = 2_000_000_000  # maior nação do mundo (RUS) fica bem abaixo
SOURCE_FIELDS = ("title", "publisher", "year", "license", "url")
SAMPLES = ("USA", "BRA", "IND", "CHN", "NGA", "SAU", "TWN", "XKX")


def is_int(value: object) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def is_num(value: object) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def load_json(path: str) -> object:
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def check_format(problems: List[str]) -> None:
    raw = open(LAND_JSON, "rb").read()
    if raw.startswith(b"\xef\xbb\xbf"):
        problems.append("arquivo começa com BOM (deve ser UTF-8 sem BOM)")
    if b"\r" in raw:
        problems.append("arquivo tem CR (deve ser LF)")
    text = raw.decode("utf-8")
    if not text.endswith("\n"):
        problems.append("arquivo não termina com newline")
    canonical = json.dumps(json.loads(text), indent=2, ensure_ascii=False, sort_keys=True) + "\n"
    if canonical != text:
        problems.append("formato fora do padrão do projeto (indent=2, chaves ordenadas, LF)")


def check_sources(payload: dict, problems: List[str], current_year: int) -> List[str]:
    sources = payload.get("sources")
    if not isinstance(sources, dict) or not sources:
        problems.append("bloco sources ausente/vazio")
        return []
    for key in sorted(sources):
        src = sources[key]
        if not isinstance(src, dict):
            problems.append("sources.%s não é objeto" % key)
            continue
        for field in SOURCE_FIELDS:
            if not src.get(field):
                problems.append("sources.%s sem %s" % (key, field))
        year = src.get("year")
        if not is_int(year) or not (1900 <= year <= current_year + 1):
            problems.append("sources.%s.year inválido: %r" % (key, year))
        url = src.get("url")
        if isinstance(url, str) and not url.startswith("http"):
            problems.append("sources.%s.url não é http(s): %r" % (key, url))
    return sorted(sources)


def check_nation(code: str, entry: object, sources: List[str], problems: List[str],
                 current_year: int) -> str:
    """Devolve 'with_ha' / 'no_ha' e registra problemas."""
    if not isinstance(entry, dict):
        problems.append("%s: entrada não é objeto" % code)
        return "no_ha"
    for field in REQUIRED_FIELDS:
        if field not in entry:
            problems.append("%s: campo obrigatório ausente: %s" % (code, field))

    ha = entry.get("agricultural_land_ha")
    year = entry.get("year")
    source = entry.get("source")
    basis = entry.get("basis")

    if basis not in BASIS:
        problems.append("%s: basis inválido: %r" % (code, basis))

    if ha is None:
        if basis not in (None, "none"):
            problems.append("%s: ha nulo com basis %r (esperado 'none')" % (code, basis))
        for field, value in (("year", year), ("source", source)):
            if value is not None:
                problems.append("%s: ha nulo mas %s = %r" % (code, field, value))
        kind = "no_ha"
    else:
        if not is_num(ha):
            problems.append("%s: agricultural_land_ha não é número: %r" % (code, ha))
        elif not (0 < float(ha) <= HA_MAX):
            problems.append("%s: agricultural_land_ha fora de (0, %d]: %r" % (code, HA_MAX, ha))
        if basis != "observed":
            problems.append("%s: ha presente com basis %r (esperado 'observed')" % (code, basis))
        if not is_int(year) or not (1961 <= year <= current_year):
            problems.append("%s: year inválido para ha: %r" % (code, year))
        if source not in sources:
            problems.append("%s: source %r não está em sources" % (code, source))
        kind = "with_ha"

    gini = entry.get("land_gini")
    gini_year = entry.get("land_gini_year")
    gini_source = entry.get("land_gini_source")
    census = entry.get("land_gini_census")
    if gini is None:
        for field, value in (("land_gini_year", gini_year), ("land_gini_source", gini_source),
                             ("land_gini_census", census)):
            if value is not None:
                problems.append("%s: land_gini nulo mas %s = %r" % (code, field, value))
    else:
        if not is_num(gini):
            problems.append("%s: land_gini não é número: %r" % (code, gini))
        elif not (0.0 < float(gini) < 1.0):
            problems.append("%s: land_gini fora de (0, 1): %r" % (code, gini))
        elif round(float(gini), 4) != float(gini):
            problems.append("%s: land_gini com mais de 4 casas: %r" % (code, gini))
        if not is_int(gini_year) or not (GINI_YEAR_MIN <= gini_year <= current_year):
            problems.append("%s: land_gini_year inválido: %r" % (code, gini_year))
        if gini_source not in sources:
            problems.append("%s: land_gini_source %r não está em sources" % (code, gini_source))
        if census is not None and (not isinstance(census, str) or not census.strip()):
            problems.append("%s: land_gini_census deveria ser texto não vazio ou null" % code)
    return kind


def check_states(payload: dict, problems: List[str], nations: Dict[str, dict]) -> int:
    """Valida o arquivo por estado, se existir; senão exige reason/rule declarados."""
    dist = payload.get("state_distribution") or {}
    declared_file = dist.get("file")
    if declared_file is None:
        if not dist.get("reason") or not dist.get("rule"):
            problems.append("state_distribution.file nulo sem reason/rule declarados")
    if not os.path.exists(STATES_JSON):
        if declared_file is not None:
            problems.append("state_distribution.file declarado (%r) mas o arquivo não existe"
                            % declared_file)
        return 0

    data = load_json(STATES_JSON)
    states_meta = (load_json(STATE_REGIONS) or {}).get("states") or {}
    states = data.get("states")
    if not isinstance(states, dict):
        problems.append("land_property_states.json: bloco states ausente")
        return 0
    sources = data.get("sources") or {}
    totals: Dict[str, float] = {}
    for sid, value in states.items():
        if sid not in states_meta:
            problems.append("land_property_states: %s não existe em state_regions.json" % sid)
            continue
        ha = value.get("agricultural_land_ha") if isinstance(value, dict) else None
        if not is_num(ha) or ha <= 0:
            problems.append("land_property_states: %s hectares inválidos: %r" % (sid, ha))
            continue
        src = value.get("source")
        if src not in sources:
            problems.append("land_property_states: %s source %r fora de sources" % (sid, src))
        nation = states_meta[sid].get("nation")
        totals[nation] = totals.get(nation, 0.0) + float(ha)
        area_km2 = states_meta[sid].get("area_km2")
        if is_num(area_km2) and ha > 100.0 * float(area_km2) * 1.001:
            problems.append("land_property_states: %s tem %s ha acima da área do estado (%s km2)"
                            % (sid, ha, area_km2))

    entries = payload.get("nations") or {}
    for nation, total in totals.items():
        national = (entries.get(nation) or {}).get("agricultural_land_ha")
        if national is None:
            problems.append("land_property_states: %s soma %s ha mas a nação não tem total"
                            % (nation, round(total)))
        elif abs(total - national) > 0.01 * national:
            problems.append("land_property_states: %s soma %s ha != nacional %s ha (>1%%)"
                            % (nation, round(total), national))
    return len(states)


def main() -> int:
    current_year = datetime.date.today().year
    problems: List[str] = []

    check_format(problems)
    payload = load_json(LAND_JSON)
    nations_list = load_json(NATIONS_JSON)
    nations = {row["id"]: row for row in nations_list}

    if payload.get("version") != 1:
        problems.append("version deveria ser 1, veio %r" % payload.get("version"))
    units = payload.get("units") or {}
    if units.get("agricultural_land") != "ha":
        problems.append("units.agricultural_land deveria ser 'ha', veio %r"
                        % units.get("agricultural_land"))
    if units.get("land_gini") != "index 0..1":
        problems.append("units.land_gini deveria ser 'index 0..1', veio %r"
                        % units.get("land_gini"))
    if not isinstance(payload.get("note"), str) or len(payload.get("note", "")) < 80:
        problems.append("note ausente/curto demais para o padrão do projeto")

    sources = check_sources(payload, problems, current_year)

    entries = payload.get("nations")
    if not isinstance(entries, dict):
        print("ERRO: bloco nations ausente")
        return 1
    extra = sorted(set(entries) - set(nations))
    missing = sorted(set(nations) - set(entries))
    if extra:
        problems.append("nações no JSON que não existem em nations.json: %s" % ", ".join(extra))
    if missing:
        problems.append("nações de nations.json ausentes no JSON: %s" % ", ".join(missing))

    with_ha: List[str] = []
    no_ha: List[str] = []
    with_gini: List[str] = []
    for code in sorted(nations):
        entry = entries.get(code)
        if entry is None:
            continue
        kind = check_nation(code, entry, sources, problems, current_year)
        (with_ha if kind == "with_ha" else no_ha).append(code)
        if (entry or {}).get("land_gini") is not None:
            with_gini.append(code)

    coverage = payload.get("coverage") or {}
    no_gini = sorted(set(nations) - set(with_gini))
    recomputed = {
        "nations_total": len(nations),
        "nations_with_agricultural_land": len(with_ha),
        "nations_no_agricultural_land": len(no_ha),
        "nations_with_land_gini": len(with_gini),
        "nations_no_land_gini": len(no_gini),
    }
    for field, value in recomputed.items():
        if coverage.get(field) != value:
            problems.append("coverage.%s = %r, recalculado = %r"
                            % (field, coverage.get(field), value))
    if sorted(coverage.get("nations_no_agricultural_land_ids") or []) != sorted(no_ha):
        problems.append("coverage.nations_no_agricultural_land_ids != lista recalculada")
    if sorted(coverage.get("nations_no_land_gini_ids") or []) != no_gini:
        problems.append("coverage.nations_no_land_gini_ids != lista recalculada")

    if with_ha:
        years = [entries[c]["year"] for c in with_ha]
        decl = coverage.get("agricultural_land_year") or {}
        if decl.get("min") != min(years) or decl.get("max") != max(years):
            problems.append("coverage.agricultural_land_year não bate (%r..%r vs %r..%r)"
                            % (decl.get("min"), decl.get("max"), min(years), max(years)))
        counts = coverage.get("agricultural_land_source_counts") or {}
        from_sources: Dict[str, int] = {}
        for code in with_ha:
            src = entries[code]["source"]
            from_sources[src] = from_sources.get(src, 0) + 1
        if counts != from_sources:
            problems.append("coverage.agricultural_land_source_counts %r != %r"
                            % (counts, from_sources))

    if with_gini:
        gyears = [entries[c]["land_gini_year"] for c in with_gini]
        gvals = [float(entries[c]["land_gini"]) for c in with_gini]
        decl = coverage.get("land_gini_year") or {}
        if decl.get("min") != min(gyears) or decl.get("max") != max(gyears):
            problems.append("coverage.land_gini_year não bate (%r..%r vs %r..%r)"
                            % (decl.get("min"), decl.get("max"), min(gyears), max(gyears)))
        rng = coverage.get("land_gini_range") or {}
        if rng.get("min") != min(gvals) or rng.get("max") != max(gvals):
            problems.append("coverage.land_gini_range não bate")
        buckets = coverage.get("land_gini_year_buckets") or {}
        if sum(buckets.values()) != len(with_gini):
            problems.append("coverage.land_gini_year_buckets soma %d != %d"
                            % (sum(buckets.values()), len(with_gini)))

    regional = payload.get("regional_reference") or {}
    regions_decl = regional.get("regions") or {}
    by_region: Dict[str, List[float]] = {}
    for code in with_gini:
        by_region.setdefault(nations[code]["region"], []).append(float(entries[code]["land_gini"]))
    for region, values in by_region.items():
        block = regions_decl.get(region) or {}
        if not block:
            problems.append("regional_reference sem a região %r" % region)
            continue
        if block.get("n") != len(values):
            problems.append("regional_reference.%s.n = %r != %d"
                            % (region, block.get("n"), len(values)))
        expected = round(statistics.median(values), 4)
        if block.get("median_land_gini") != expected:
            problems.append("regional_reference.%s.median_land_gini = %r != %r"
                            % (region, block.get("median_land_gini"), expected))
        for field in ("median_land_gini", "min", "max"):
            value = block.get(field)
            if not is_num(value) or not (0.0 < float(value) < 1.0):
                problems.append("regional_reference.%s.%s fora de (0,1): %r"
                                % (region, field, value))
    if (regional.get("global") or {}).get("n") != len(with_gini):
        problems.append("regional_reference.global.n != nations_with_land_gini")

    cross = payload.get("cross_check") or {}
    compared = cross.get("nations_compared")
    within = cross.get("nations_within_2pct")
    over = cross.get("nations_over_2pct")
    if all(is_int(v) for v in (compared, within, over)) and within + over != compared:
        problems.append("cross_check: within_2pct + over_2pct != nations_compared")

    states_checked = check_states(payload, problems, nations)

    print("nações em nations.json: %d | entradas no JSON: %d" % (len(nations), len(entries)))
    print("área agrícola observada: %d | sem área: %d" % (len(with_ha), len(no_ha)))
    print("land_gini observado: %d | sem gini: %d" % (len(with_gini), len(no_gini)))
    if with_gini:
        print("anos do gini: %d..%d (mediana %s)"
              % (min(entries[c]["land_gini_year"] for c in with_gini),
                 max(entries[c]["land_gini_year"] for c in with_gini),
                 statistics.median(entries[c]["land_gini_year"] for c in with_gini)))
    print("fontes declaradas: %s" % ", ".join(sources))
    print("estados validados: %d" % states_checked)
    print("amostra:")
    for code in SAMPLES:
        entry = entries.get(code) or {}
        print("  %-4s ha=%-12s ano=%-5s gini=%-7s ano=%-5s base=%s" % (
            code,
            entry.get("agricultural_land_ha"),
            entry.get("year") if entry.get("year") is not None else "-",
            entry.get("land_gini") if entry.get("land_gini") is not None else "-",
            entry.get("land_gini_year") if entry.get("land_gini_year") is not None else "-",
            entry.get("basis")))

    if problems:
        print()
        print("PROBLEMAS (%d):" % len(problems))
        for problem in problems:
            print(" -", problem)
        return 1

    print()
    print("OK: %d nações (%d com área, %d com gini fundiário)"
          % (len(nations), len(with_ha), len(with_gini)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
