#!/usr/bin/env python3
"""Valida os dados de cenário do OpenGS Game.

Verifica os pares CSV/JSON em ``data/definitions/scenario/`` (cidades, portos,
construções e nações) e, se existir, ``data/definitions/i18n/i18n.csv``.

Uso::

    python tools/validate_scenario_data.py
    python tools/validate_scenario_data.py --root .
    python tools/validate_scenario_data.py --strict

Códigos de saída:

* ``0`` — nenhum erro (avisos são permitidos, salvo com ``--strict``);
* ``1`` — erros de validação (ou avisos com ``--strict``);
* ``2`` — arquivo ausente, ilegível ou com estrutura inválida.

Somente biblioteca padrão.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Optional

DEFAULT_ROOT = Path(__file__).resolve().parent.parent

SCENARIO_DIR = Path("data/definitions/scenario")
I18N_FILE = Path("data/definitions/i18n/i18n.csv")

X_RANGE = (0.0, 5000.0)
Y_RANGE = (0.0, 2500.0)

CITY_FIELDS = [
    "id",
    "nation",
    "home_nation",
    "name",
    "population",
    "x",
    "y",
    "landmass",
    "capital",
]
PORT_FIELDS = ["id", "name", "nation", "x", "y", "landmass"]
BUILDING_FIELDS_CSV = ["city_id", "building_id", "name", "output_tons_per_day"]
BUILDING_FIELDS_JSON = ["city_id", "id", "name", "output_tons_per_day"]
NATION_FIELDS = ["id", "name", "flag"]
I18N_HEADER = ["key", "pt_BR", "en", "es"]


class TableError(Exception):
    """Falha estrutural que impede validar o arquivo."""


@dataclass
class Row:
    label: str
    data: dict


@dataclass
class Section:
    label: str
    records: Optional[int] = None
    errors: list = field(default_factory=list)
    warnings: list = field(default_factory=list)


class Report:
    def __init__(self) -> None:
        self.sections: list[Section] = []
        self._index: dict[str, Section] = {}
        self.io_failure = False

    def section(self, label: str) -> Section:
        sec = self._index.get(label)
        if sec is None:
            sec = Section(label)
            self.sections.append(sec)
            self._index[label] = sec
        return sec

    def error(self, label: str, message: str) -> None:
        self.section(label).errors.append(message)

    def warning(self, label: str, message: str) -> None:
        self.section(label).warnings.append(message)


# --------------------------------------------------------------------------
# Utilidades
# --------------------------------------------------------------------------


def is_blank(value: Any) -> bool:
    if value is None:
        return True
    if isinstance(value, str):
        return value.strip() == ""
    return False


def as_text(value: Any) -> str:
    if value is None:
        return ""
    if isinstance(value, str):
        return value.strip()
    return str(value).strip()


def fmt_number(value: float) -> str:
    if isinstance(value, float) and math.isnan(value):
        return "nan"
    if float(value).is_integer():
        return str(int(value))
    return f"{value:g}"


def parse_number(value: Any) -> Optional[float]:
    """Converte para float; devolve None se não for um número finito."""
    if isinstance(value, bool):
        return None
    if isinstance(value, (int, float)):
        number = float(value)
        if math.isnan(number) or math.isinf(number):
            return None
        return number
    if isinstance(value, str):
        stripped = value.strip()
        if not stripped:
            return None
        try:
            number = float(stripped)
        except ValueError:
            return None
        if math.isnan(number) or math.isinf(number):
            return None
        return number
    return None


def parse_capital(value: Any) -> Optional[int]:
    """Aceita bool, 0/1 numéricos e strings "0"/"1"."""
    if isinstance(value, bool):
        return 1 if value else 0
    if isinstance(value, int):
        return value if value in (0, 1) else None
    if isinstance(value, float):
        if value in (0.0, 1.0):
            return int(value)
        return None
    if isinstance(value, str):
        stripped = value.strip()
        if stripped in ("0", "1"):
            return int(stripped)
    return None


def rel_path(path: Path, root: Path) -> str:
    try:
        return path.relative_to(root).as_posix()
    except ValueError:
        return path.as_posix()


def emit_structure(sec: Section, structure: list) -> None:
    for label, message, level in structure:
        text = f"{label}: {message}"
        if level == "warning":
            sec.warnings.append(text)
        else:
            sec.errors.append(text)


def check_required(sec: Section, row: Row, fields: list) -> None:
    missing = [name for name in fields if is_blank(row.data.get(name))]
    if missing:
        sec.errors.append(
            f"{row.label}: campo obrigatório ausente ou vazio: {', '.join(missing)}"
        )


def check_id(
    sec: Section,
    row: Row,
    entity_id: str,
    seen: dict,
    entity_label: str,
) -> None:
    if not entity_id:
        return
    if entity_id in seen:
        sec.errors.append(
            f"{row.label}: id de {entity_label} duplicado '{entity_id}' "
            f"(já usado em {seen[entity_id]})"
        )
    else:
        seen[entity_id] = row.label


def check_coordinate(
    sec: Section,
    row: Row,
    name: str,
    bounds: tuple,
) -> None:
    raw = row.data.get(name)
    if is_blank(raw):
        return
    value = parse_number(raw)
    if value is None:
        sec.errors.append(f"{row.label}: {name} não é numérico ({raw!r})")
        return
    low, high = bounds
    if not (low <= value <= high):
        sec.errors.append(
            f"{row.label}: {name}={fmt_number(value)} fora do intervalo "
            f"[{fmt_number(low)}, {fmt_number(high)}]"
        )


# --------------------------------------------------------------------------
# Leitura dos arquivos
# --------------------------------------------------------------------------


def read_csv_table(path: Path) -> tuple[list, list, list]:
    """Devolve (cabeçalho, linhas, problemas estruturais)."""
    structure: list = []
    rows: list = []
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        reader = csv.DictReader(handle)
        if not reader.fieldnames:
            raise TableError("arquivo vazio (sem cabeçalho)")
        header = [name.strip() if isinstance(name, str) else name for name in reader.fieldnames]
        if any(name is None or name == "" for name in header):
            raise TableError("cabeçalho com coluna sem nome")
        for raw in reader:
            label = f"linha {reader.line_num}"
            if None in raw:
                plural = "s" if len([v for k, v in raw.items() if k is None]) > 1 else ""
                structure.append(
                    (label, f"linha tem coluna{plural} a mais que o cabeçalho", "error")
                )
                raw = {key: value for key, value in raw.items() if key is not None}
            if not raw or all(is_blank(value) for value in raw.values()):
                structure.append((label, "linha vazia", "warning"))
                continue
            data = {}
            for key, value in raw.items():
                data[str(key).strip()] = value.strip() if isinstance(value, str) else value
            rows.append(Row(label, data))
    return header, rows, structure


def read_json_rows(path: Path) -> list:
    try:
        with path.open("r", encoding="utf-8-sig") as handle:
            data = json.load(handle)
    except json.JSONDecodeError as exc:
        raise TableError(f"JSON inválido: {exc}") from exc
    if isinstance(data, dict):
        arrays = [value for value in data.values() if isinstance(value, list)]
        if len(arrays) != 1:
            raise TableError("esperado um array JSON de objetos na raiz")
        data = arrays[0]
    if not isinstance(data, list):
        raise TableError("esperado um array JSON de objetos na raiz")
    rows = []
    for index, item in enumerate(data, start=1):
        if not isinstance(item, dict):
            raise TableError(f"item {index} não é um objeto JSON")
        rows.append(Row(f"item {index}", item))
    return rows


def load_pair(
    report: Report,
    root: Path,
    scenario_dir: Path,
    stem: str,
    csv_required: list,
    json_required: list,
) -> dict:
    """Carrega ``stem.csv`` e ``stem.json``; devolve {formato: (sec, rows, structure)}."""
    loaded: dict = {}
    for fmt, required in (("csv", csv_required), ("json", json_required)):
        path = scenario_dir / f"{stem}.{fmt}"
        sec = report.section(rel_path(path, root))
        if not path.exists():
            sec.errors.append("arquivo obrigatório não encontrado")
            report.io_failure = True
            continue
        try:
            if fmt == "csv":
                header, rows, structure = read_csv_table(path)
                missing = [name for name in required if name not in header]
                if missing:
                    sec.errors.append(
                        f"cabeçalho sem coluna(s) obrigatória(s): {', '.join(missing)}"
                    )
                    sec.records = len(rows)
                    continue
            else:
                rows = read_json_rows(path)
                structure = []
            sec.records = len(rows)
            if not rows:
                sec.warnings.append("arquivo sem registros")
            loaded[fmt] = (sec, rows, structure)
        except (OSError, UnicodeDecodeError, TableError) as exc:
            sec.errors.append(f"não foi possível ler o arquivo: {exc}")
            report.io_failure = True
    return loaded


# --------------------------------------------------------------------------
# Validações por entidade
# --------------------------------------------------------------------------


def validate_cities(sec: Section, rows: list, structure: list) -> set:
    emit_structure(sec, structure)
    ids: set = set()
    seen: dict = {}
    for row in rows:
        data = row.data
        check_required(sec, row, CITY_FIELDS)

        city_id = as_text(data.get("id"))
        check_id(sec, row, city_id, seen, "cidade")
        if city_id:
            ids.add(city_id)

        population = parse_number(data.get("population"))
        if not is_blank(data.get("population")):
            if population is None:
                sec.errors.append(
                    f"{row.label}: population não é numérico ({data.get('population')!r})"
                )
            elif population <= 0:
                sec.errors.append(
                    f"{row.label}: population deve ser > 0 (valor: {fmt_number(population)})"
                )

        check_coordinate(sec, row, "x", X_RANGE)
        check_coordinate(sec, row, "y", Y_RANGE)

        raw_capital = data.get("capital")
        if not is_blank(raw_capital) and parse_capital(raw_capital) is None:
            sec.errors.append(
                f"{row.label}: capital deve ser 0 ou 1 (valor: {raw_capital!r})"
            )
    return ids


def validate_ports(sec: Section, rows: list, structure: list) -> set:
    emit_structure(sec, structure)
    ids: set = set()
    seen: dict = {}
    for row in rows:
        data = row.data
        check_required(sec, row, PORT_FIELDS)

        port_id = as_text(data.get("id"))
        check_id(sec, row, port_id, seen, "porto")
        if port_id:
            ids.add(port_id)

        check_coordinate(sec, row, "x", X_RANGE)
        check_coordinate(sec, row, "y", Y_RANGE)
    return ids


def validate_nations(sec: Section, rows: list, structure: list) -> set:
    emit_structure(sec, structure)
    ids: set = set()
    seen: dict = {}
    for row in rows:
        check_required(sec, row, NATION_FIELDS)
        nation_id = as_text(row.data.get("id"))
        check_id(sec, row, nation_id, seen, "nação")
        if nation_id:
            ids.add(nation_id)
    return ids


def validate_buildings(
    sec: Section,
    rows: list,
    structure: list,
    fields: list,
    city_ids: Optional[set],
    cities_label: str,
) -> set:
    emit_structure(sec, structure)
    ids: set = set()
    seen: dict = {}
    for row in rows:
        data = row.data
        check_required(sec, row, fields)

        building_id = as_text(data.get("building_id")) or as_text(data.get("id"))
        check_id(sec, row, building_id, seen, "construção")
        if building_id:
            ids.add(building_id)

        city_id = as_text(data.get("city_id"))
        if city_id and city_ids is not None and city_id not in city_ids:
            sec.errors.append(
                f"{row.label}: city_id '{city_id}' não existe em {cities_label}"
            )

        output = parse_number(data.get("output_tons_per_day"))
        if not is_blank(data.get("output_tons_per_day")):
            if output is None:
                sec.errors.append(
                    f"{row.label}: output_tons_per_day não é numérico "
                    f"({data.get('output_tons_per_day')!r})"
                )
            elif output < 0:
                sec.errors.append(
                    f"{row.label}: output_tons_per_day deve ser >= 0 "
                    f"(valor: {fmt_number(output)})"
                )
    return ids


def check_nation_refs(
    sec: Section,
    rows: list,
    nation_ids: Optional[set],
    nations_label: str,
) -> None:
    if nation_ids is None:
        return
    for row in rows:
        for field_name in ("nation", "home_nation"):
            value = as_text(row.data.get(field_name))
            if value and value not in nation_ids:
                sec.warnings.append(
                    f"{row.label}: {field_name} '{value}' não existe em {nations_label}"
                )


def validate_i18n(sec: Section, path: Path) -> None:
    total = 0
    seen: dict = {}
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        reader = csv.reader(handle)
        try:
            header = next(reader)
        except StopIteration:
            sec.errors.append("arquivo vazio (sem cabeçalho)")
            return
        header = [cell.strip() for cell in header]
        if header != I18N_HEADER:
            sec.errors.append(
                f"cabeçalho esperado '{','.join(I18N_HEADER)}'; "
                f"encontrado '{','.join(header)}'"
            )
        for record in reader:
            line = reader.line_num
            if not any(cell.strip() for cell in record):
                sec.warnings.append(f"linha {line}: linha vazia")
                continue
            total += 1
            if len(record) != 4:
                sec.errors.append(
                    f"linha {line}: esperado 4 colunas, encontrado {len(record)}"
                )
            key = record[0].strip() if record else ""
            if not key:
                sec.errors.append(f"linha {line}: chave vazia")
            elif key in seen:
                sec.warnings.append(
                    f"linha {line}: chave duplicada '{key}' (já usada na linha {seen[key]})"
                )
            else:
                seen[key] = line
    sec.records = total


# --------------------------------------------------------------------------
# Consistência CSV x JSON
# --------------------------------------------------------------------------


def compare_sets(
    report: Report,
    entity: str,
    what: str,
    csv_items: Optional[set],
    json_items: Optional[set],
    csv_label: str,
    json_label: str,
) -> None:
    if csv_items is None or json_items is None:
        return
    only_csv = sorted(csv_items - json_items)
    only_json = sorted(json_items - csv_items)
    if not only_csv and not only_json:
        return

    def listing(items: list) -> str:
        shown = ", ".join(str(item) for item in items[:10])
        if len(items) > 10:
            shown += f", ... (+{len(items) - 10})"
        return shown

    parts = []
    if only_csv:
        parts.append(f"{len(only_csv)} só em {csv_label} ({listing(only_csv)})")
    if only_json:
        parts.append(f"{len(only_json)} só em {json_label} ({listing(only_json)})")
    sec = report.section("consistência CSV x JSON")
    sec.warnings.append(f"{entity}: {what} divergentes — " + "; ".join(parts))


# --------------------------------------------------------------------------
# Saída
# --------------------------------------------------------------------------


def print_report(report: Report, root: Path) -> tuple:
    print("OpenGS - validação dos dados de cenário")
    print(f"raiz: {root}")
    print()

    total_errors = 0
    total_warnings = 0
    failing_sections = 0

    for sec in report.sections:
        parts = [sec.label]
        if sec.records is not None:
            parts.append(f"({sec.records} registro(s))")
        if sec.errors:
            status = f"{len(sec.errors)} erro(s)"
            failing_sections += 1
        elif sec.records is None:
            status = "não verificado"
        else:
            status = "ok"
        if sec.warnings:
            status += f", {len(sec.warnings)} aviso(s)"
        print(f"{' '.join(parts)}: {status}")
        for message in sec.errors:
            print(f"  ERRO  {message}")
        for message in sec.warnings:
            print(f"  AVISO {message}")
        total_errors += len(sec.errors)
        total_warnings += len(sec.warnings)

    print()
    if total_errors:
        print(
            f"Resumo: {total_errors} erro(s) e {total_warnings} aviso(s) em "
            f"{len(report.sections)} arquivo(s) verificado(s); "
            f"{failing_sections} com erro(s)."
        )
    else:
        print(
            f"Resumo: tudo ok — 0 erros, {total_warnings} aviso(s) em "
            f"{len(report.sections)} arquivo(s) verificado(s)."
        )
    return total_errors, total_warnings


# --------------------------------------------------------------------------
# Fluxo principal
# --------------------------------------------------------------------------


def build_report(root: Path) -> Report:
    report = Report()
    scenario_dir = root / SCENARIO_DIR
    i18n_path = root / I18N_FILE

    cities = load_pair(report, root, scenario_dir, "cities", CITY_FIELDS, CITY_FIELDS)
    ports = load_pair(report, root, scenario_dir, "ports", PORT_FIELDS, PORT_FIELDS)
    buildings = load_pair(
        report,
        root,
        scenario_dir,
        "buildings",
        BUILDING_FIELDS_CSV,
        BUILDING_FIELDS_JSON,
    )
    nations = load_pair(report, root, scenario_dir, "nations", NATION_FIELDS, NATION_FIELDS)

    city_ids = {}
    for fmt in ("csv", "json"):
        if fmt in cities:
            sec, rows, structure = cities[fmt]
            city_ids[fmt] = validate_cities(sec, rows, structure)

    port_ids = {}
    for fmt in ("csv", "json"):
        if fmt in ports:
            sec, rows, structure = ports[fmt]
            port_ids[fmt] = validate_ports(sec, rows, structure)

    nation_ids = {}
    for fmt in ("csv", "json"):
        if fmt in nations:
            sec, rows, structure = nations[fmt]
            nation_ids[fmt] = validate_nations(sec, rows, structure)

    building_ids = {}
    for fmt in ("csv", "json"):
        if fmt not in buildings:
            continue
        sec, rows, structure = buildings[fmt]
        fields = BUILDING_FIELDS_CSV if fmt == "csv" else BUILDING_FIELDS_JSON
        reference = city_ids.get(fmt)
        cities_label = rel_path(scenario_dir / f"cities.{fmt}", root)
        if reference is None:
            sec.warnings.append(
                f"referências de city_id não verificadas: {cities_label} indisponível"
            )
        building_ids[fmt] = validate_buildings(
            sec, rows, structure, fields, reference, cities_label
        )

    for fmt in ("csv", "json"):
        cities_label = rel_path(scenario_dir / f"cities.{fmt}", root)
        if fmt in cities and fmt in nation_ids:
            sec, rows, _structure = cities[fmt]
            check_nation_refs(sec, rows, nation_ids[fmt], rel_path(scenario_dir / f"nations.{fmt}", root))
        if fmt in ports and fmt in nation_ids:
            sec, rows, _structure = ports[fmt]
            check_nation_refs(sec, rows, nation_ids[fmt], rel_path(scenario_dir / f"nations.{fmt}", root))

    for entity, csv_items, json_items in (
        ("cities", city_ids.get("csv"), city_ids.get("json")),
        ("ports", port_ids.get("csv"), port_ids.get("json")),
        ("nations", nation_ids.get("csv"), nation_ids.get("json")),
        ("buildings", building_ids.get("csv"), building_ids.get("json")),
    ):
        compare_sets(
            report,
            entity,
            "ids",
            csv_items,
            json_items,
            f"{entity}.csv",
            f"{entity}.json",
        )

    if not scenario_dir.is_dir():
        report.section(rel_path(scenario_dir, root)).errors.append(
            "diretório de cenário não encontrado"
        )
        report.io_failure = True

    i18n_sec = report.section(rel_path(i18n_path, root))
    if i18n_path.exists():
        try:
            validate_i18n(i18n_sec, i18n_path)
        except (OSError, UnicodeDecodeError) as exc:
            i18n_sec.errors.append(f"não foi possível ler o arquivo: {exc}")
            report.io_failure = True
    else:
        i18n_sec.warnings.append("arquivo opcional não encontrado; verificação ignorada")

    return report


def main(argv: Optional[list] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Valida os dados de cenário do OpenGS Game (CSV/JSON e i18n).",
    )
    parser.add_argument(
        "--root",
        default=str(DEFAULT_ROOT),
        help="raiz do projeto (padrão: diretório acima de tools/)",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="trata avisos como erros (código de saída 1)",
    )
    args = parser.parse_args(argv)

    root = Path(args.root).expanduser().resolve()
    report = build_report(root)
    total_errors, total_warnings = print_report(report, root)

    if report.io_failure:
        return 2
    if total_errors:
        return 1
    if args.strict and total_warnings:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
