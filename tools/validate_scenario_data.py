#!/usr/bin/env python3
"""Valida os dados de cenário do OpenGS Game.

Verifica os pares CSV/JSON em ``data/definitions/scenario/`` (cidades, portos,
construções e nações), o arquivo de bens da economia
``data/definitions/economy/goods.json`` e, se existir,
``data/definitions/i18n/i18n.csv``.

Também cruza cada cidade/porto com o ATLAS: o ponto tem de estar em terra na
camada que o shader usa (``world_sdf.png`` ou a região de alta resolução) e
dentro de uma província com dono (``world_province.png`` + ``provinces.json``).
Cidade/porto em água aberta (sem terra a ``COAST_TOLERANCE_KM``) é ERRO — é o
caso da ilha removida em que a entidade ficou para trás. Os pontos encostados
na costa (até 2 texels do raster, 4,9 km/texel) são a quantização do atlas, não
uma cidade no mar; os portos ainda ficam no lado de ÁGUA da costa por desenho
(curadoria r5). Use ``--land-report`` para ver a distância à terra de cada um.

Uso::

    python tools/validate_scenario_data.py
    python tools/validate_scenario_data.py --root .
    python tools/validate_scenario_data.py --strict
    python tools/validate_scenario_data.py --land-report

Códigos de saída:

* ``0`` — nenhum erro (avisos são permitidos, salvo com ``--strict``);
* ``1`` — erros de validação (ou avisos com ``--strict``);
* ``2`` — arquivo ausente, ilegível ou com estrutura inválida.

A checagem de dados é só biblioteca padrão. A leitura do atlas usa numpy e
Pillow — as mesmas dependências de ``build_atlas_layers.py`` — e, sem elas,
o validador avisa que a checagem de terra não foi executada.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Optional

DEFAULT_ROOT = Path(__file__).resolve().parent.parent

SCENARIO_DIR = Path("data/definitions/scenario")
ECONOMY_DIR = Path("data/definitions/economy")
GOODS_FILE = ECONOMY_DIR / "goods.json"
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
PORT_FIELDS = ["id", "name", "nation", "x", "y", "landmass", "port_of"]
BUILDING_FIELDS_CSV = ["city_id", "building_id", "name", "output_tons_per_day"]
BUILDING_FIELDS_JSON = ["city_id", "id", "name", "output_tons_per_day"]
# Schema do plano §5. `population`/`gdp_ppp_bn` são opcionais: nação sem dado
# publicado fica sem o campo (a UI declara a ausência) — não se inventa número.
NATION_FIELDS = [
    "id",
    "name",
    "flag",
    "color",
    "capital",
    "currency",
    "government",
    "ai_profile",
    "region",
]
COLOR_PATTERN = re.compile(r"^#[0-9a-fA-F]{6}$")
FLAGS_DIR = Path("assets/flags")

# --- Atlas: cidade/porto tem de estar em terra ------------------------------
# Mesmas texturas e convenção do shader (`atlas_common.gdshaderinc`): canal R =
# distância assinada à costa, > 128 é terra. A região (Europa) tem 1,9 km/texel
# e substitui o mundo dentro de `REGION_BOUNDS` (a mesma caixa do shader).
WORLD_SDF_FILE = Path("map/map_data/atlas/world_sdf.png")
REGION_SDF_FILE = Path("map/map_data/atlas/region_sdf.png")
WORLD_PROVINCE_FILE = Path("map/map_data/atlas/world_province.png")
ATLAS_PROVINCES_FILE = Path("map/map_data/atlas/provinces.json")
LAND_MIN_VALUE = 129  # R > 128 = terra
REGION_BOUNDS = (-25.0, 33.0, 45.0, 72.0)
# 2 texels do mundo (~4,9 km cada): a costa do atlas é um campo de distância
# discretizado, então a cidade costeira pode cair 1 texel no lado da água sem
# que isso seja "cidade no mar". Uma ilha removida deixa a cidade a centenas de
# quilômetros da terra — muito acima deste limite.
COAST_TOLERANCE_KM = 10.0
KM_PER_WORLD_TEXEL = 40075.0 / 8192.0   # largura do mundo (8192 texels)
KM_PER_REGION_TEXEL = 40075.0 / 360.0 * (REGION_BOUNDS[2] - REGION_BOUNDS[0]) / 4096.0
PLAN_SIZE = (5000.0, 2500.0)

GOODS_FIELDS = ["id", "name", "base_price", "food"]
# Bens conhecidos pelo núcleo C++ (demo_product_index em economy_sim.cpp), na
# mesma ordem de data/definitions/economy/goods.json.
GOODS_ALLOWED_IDS = (
    "wheat",
    "iron",
    "rice",
    "maize",
    "bauxite",
    "lead",
    "gold",
    "phosphate",
    "tungsten",
    "rare_earths",
    "sulfur",
    "diamond",
    "coal",
    "copper",
    "gas",
    "oil",
    "potato",
    "soy",
    "cotton",
    "wood",
    "uranium",
)
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
        self.entities: list = []

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


def load_json_file(report: Report, root: Path, path: Path) -> Optional[tuple]:
    """Carrega um JSON de objetos isolado (sem par CSV); devolve (sec, rows) ou None."""
    sec = report.section(rel_path(path, root))
    if not path.exists():
        sec.errors.append("arquivo obrigatório não encontrado")
        report.io_failure = True
        return None
    try:
        rows = read_json_rows(path)
    except (OSError, UnicodeDecodeError, TableError) as exc:
        sec.errors.append(f"não foi possível ler o arquivo: {exc}")
        report.io_failure = True
        return None
    sec.records = len(rows)
    if not rows:
        sec.warnings.append("arquivo sem registros")
    return sec, rows


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
            elif population < 0:
                sec.errors.append(
                    f"{row.label}: population não pode ser negativa "
                    f"(valor: {fmt_number(population)})"
                )
            elif population == 0:
                # Zero é um dado da fonte, não um número faltando (Plymouth foi
                # abandonada após a erupção; Ngerulmud tem 0 habitantes no
                # GeoNames). Fica avisado para não passar em silêncio; a
                # economia trata a cidade com necessidade 0.
                sec.warnings.append(
                    f"{row.label}: population 0 declarada na fonte"
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


def validate_nations(sec: Section, rows: list, structure: list, root: Path) -> set:
    emit_structure(sec, structure)
    ids: set = set()
    seen: dict = {}
    for row in rows:
        check_required(sec, row, NATION_FIELDS)
        nation_id = as_text(row.data.get("id"))
        check_id(sec, row, nation_id, seen, "nação")
        if nation_id:
            ids.add(nation_id)

        color = as_text(row.data.get("color"))
        if color and not COLOR_PATTERN.match(color):
            sec.errors.append(
                f"{row.label}: color deve ser #RRGGBB (valor: {color!r})"
            )

        flag = as_text(row.data.get("flag"))
        if flag and not (root / FLAGS_DIR / f"{flag}.png").exists():
            sec.errors.append(
                f"{row.label}: bandeira ausente em assets/flags/{flag}.png"
            )
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


def validate_goods(sec: Section, rows: list) -> set:
    """Valida os bens da economia (id, nome, preço-base e flag de comida)."""
    ids: set = set()
    seen: dict = {}
    allowed = ", ".join(GOODS_ALLOWED_IDS)
    for row in rows:
        data = row.data
        check_required(sec, row, GOODS_FIELDS)

        good_id = as_text(data.get("id"))
        check_id(sec, row, good_id, seen, "bem")
        if good_id:
            ids.add(good_id)
            if good_id not in GOODS_ALLOWED_IDS:
                sec.errors.append(
                    f"{row.label}: id '{good_id}' não é conhecido pelo núcleo C++ "
                    f"(permitidos: {allowed})"
                )

        price = parse_number(data.get("base_price"))
        if not is_blank(data.get("base_price")):
            if price is None:
                sec.errors.append(
                    f"{row.label}: base_price não é numérico "
                    f"({data.get('base_price')!r})"
                )
            elif price <= 0:
                sec.errors.append(
                    f"{row.label}: base_price deve ser > 0 "
                    f"(valor: {fmt_number(price)})"
                )

        raw_food = data.get("food")
        if not isinstance(raw_food, bool) and not is_blank(raw_food):
            sec.errors.append(
                f"{row.label}: food deve ser booleano true/false "
                f"(valor: {raw_food!r})"
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
                message = (
                    f"{row.label}: {field_name} '{value}' não existe em {nations_label}"
                )
                if field_name == "nation":
                    sec.errors.append(message)
                else:
                    sec.warnings.append(message)


def check_port_of(
    sec: Section,
    ports: list,
    cities: list,
    cities_label: str,
) -> None:
    """`port_of`, quando declarado, é uma cidade da MESMA nação do porto.

    O mapa usa o campo para saber de quem é o porto (badge quando o ponto de
    costa encosta no tecido urbano). Cidade inexistente ou de outra nação
    deixaria o porto órfão em silêncio.
    """
    city_nation = {
        as_text(row.data.get("id")): as_text(row.data.get("nation")) for row in cities
    }
    for row in ports:
        port_of = as_text(row.data.get("port_of"))
        if not port_of:
            continue
        if port_of not in city_nation:
            sec.errors.append(
                f"{row.label}: port_of '{port_of}' não existe em {cities_label}"
            )
        elif city_nation[port_of] != as_text(row.data.get("nation")):
            sec.errors.append(
                f"{row.label}: port_of '{port_of}' é de outra nação"
            )


def check_nation_i18n(sec: Section, i18n: dict, nation_ids: set) -> None:
    """Toda nação tem nome nos 3 idiomas no contrato novo e no alias legado."""
    for tag in sorted(nation_ids):
        for prefix in ("nation.name.", "nation."):
            key = prefix + tag
            entry = i18n.get(key)
            if entry is None:
                sec.errors.append(f"nação '{tag}' sem chave i18n '{key}'")
                continue
            for locale in ("pt_BR", "en", "es"):
                if is_blank(entry.get(locale)):
                    sec.errors.append(
                        f"nação '{tag}': chave '{key}' sem texto em {locale}"
                    )


def check_capitals(
    sec: Section,
    cities: list,
    nation_ids: set,
    cities_label: str,
) -> None:
    """Cada nação tem ≥1 cidade e exatamente uma capital declarada."""
    count: dict = {tag: 0 for tag in nation_ids}
    total: dict = {tag: 0 for tag in nation_ids}
    for row in cities:
        tag = as_text(row.data.get("nation"))
        if tag not in nation_ids:
            continue
        total[tag] = total.get(tag, 0) + 1
        if as_text(row.data.get("capital")).lower() in ("1", "true", "yes"):
            count[tag] = count.get(tag, 0) + 1
    for tag in sorted(nation_ids):
        if total.get(tag, 0) <= 0:
            sec.errors.append(f"nação '{tag}' sem cidade em {cities_label}")
        elif count.get(tag, 0) != 1:
            sec.errors.append(
                f"nação '{tag}' tem {count.get(tag, 0)} capital(is) em "
                f"{cities_label}; esperado exatamente 1"
            )


def validate_i18n(sec: Section, path: Path) -> dict:
    total = 0
    seen: dict = {}
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        reader = csv.reader(handle)
        try:
            header = next(reader)
        except StopIteration:
            sec.errors.append("arquivo vazio (sem cabeçalho)")
            return set()
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
                    f"linha {line}: chave duplicada '{key}' "
                    f"(já usada na linha {seen[key]['line']})"
                )
            else:
                values = [cell.strip() for cell in record[1:4]]
                while len(values) < 3:
                    values.append("")
                seen[key] = {
                    "line": line,
                    "pt_BR": values[0],
                    "en": values[1],
                    "es": values[2],
                }
    sec.records = total
    return seen


def check_i18n_ids(
    sec: Section,
    i18n_keys: set,
    city_ids: dict,
    port_ids: dict,
) -> None:
    """Cruza os ids de cidades/portos com as chaves city.name.*/port.name.*.

    Um id sem chave cai no fallback cru do CSV na legenda (I18n.city_name /
    port_name devolvem o id ou o nome do CSV em vez do nome localizado). Era o
    caso de VEN-LAG, SUR-PBM e GUY-GEO.
    """
    for entity, prefix, id_map in (
        ("cidade", "city.name.", city_ids),
        ("porto", "port.name.", port_ids),
    ):
        ids: set = set()
        for values in id_map.values():
            ids |= values
        for item in sorted(ids):
            key = prefix + item
            if key not in i18n_keys:
                sec.errors.append(
                    f"{entity} '{item}' sem chave i18n '{key}'"
                )


# --------------------------------------------------------------------------
# Atlas: cidade/porto em terra e dentro de província com dono
# --------------------------------------------------------------------------

_RASTER_CACHE: dict = {}


def _numpy_and_pillow():
    """Importa numpy/Pillow sob demanda (dependências do pipeline do atlas)."""
    try:
        import numpy as np
        from PIL import Image
    except ImportError:
        return None, None
    return np, Image


def load_land_raster(root: Path, relative: Path):
    """Canal R (costa) do SDF do atlas como matriz, ou None.

    Devolve ``(array, km_por_texel, motivo)``; ``motivo`` só é preenchido quando
    a textura não pôde ser lida.
    """
    key = str(relative)
    if key in _RASTER_CACHE:
        return _RASTER_CACHE[key]
    np, Image = _numpy_and_pillow()
    if np is None:
        result = (None, 0.0, "numpy/Pillow ausentes")
        _RASTER_CACHE[key] = result
        return result
    path = root / relative
    if not path.exists():
        result = (None, 0.0, f"{relative.as_posix()} ausente")
        _RASTER_CACHE[key] = result
        return result
    try:
        with Image.open(path) as image:
            # Só o canal R (costa); Luminância misturaria fronteiras (G/B).
            array = np.asarray(image.getchannel(0))
    except (OSError, ValueError) as exc:
        result = (None, 0.0, f"{relative.as_posix()} ilegível: {exc}")
        _RASTER_CACHE[key] = result
        return result
    km = KM_PER_WORLD_TEXEL if relative == WORLD_SDF_FILE else KM_PER_REGION_TEXEL
    result = (array, km, "")
    _RASTER_CACHE[key] = result
    return result


def load_province_raster(root: Path):
    """Id global da província por pixel (R + G*256) ou None."""
    key = "__provinces__"
    if key in _RASTER_CACHE:
        return _RASTER_CACHE[key]
    np, Image = _numpy_and_pillow()
    if np is None:
        result = (None, {}, "numpy/Pillow ausentes")
        _RASTER_CACHE[key] = result
        return result
    path = root / WORLD_PROVINCE_FILE
    data_path = root / ATLAS_PROVINCES_FILE
    if not path.exists() or not data_path.exists():
        result = (None, {}, "raster/roster de províncias ausente")
        _RASTER_CACHE[key] = result
        return result
    try:
        with Image.open(path) as image:
            rgb = np.asarray(image.convert("RGB")).astype(np.int32)
        with data_path.open("r", encoding="utf-8") as handle:
            data = json.load(handle)
        provinces = {int(item.get("id", 0)): item
                     for item in data.get("provinces", [])}
        ids = rgb[:, :, 0] + rgb[:, :, 1] * 256
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        result = (None, {}, f"províncias ilegíveis: {exc}")
        _RASTER_CACHE[key] = result
        return result
    result = (ids, provinces, "")
    _RASTER_CACHE[key] = result
    return result


def plan_to_lonlat(x: float, y: float) -> tuple:
    """x/y do plano 5000×2500 para lon/lat equirretangular."""
    return (x / PLAN_SIZE[0] * 360.0 - 180.0,
            90.0 - y / PLAN_SIZE[1] * 180.0)


def lonlat_to_pixel(lon: float, lat: float, width: int, height: int) -> tuple:
    return (int(round((lon + 180.0) / 360.0 * width - 0.5)),
            int(round((90.0 - lat) / 180.0 * height - 0.5)))


def region_to_pixel(lon: float, lat: float, width: int, height: int) -> tuple:
    lon0, lat0, lon1, lat1 = REGION_BOUNDS
    return (int(round((lon - lon0) / (lon1 - lon0) * width - 0.5)),
            int(round((lat1 - lat) / (lat1 - lat0) * height - 0.5)))


def point_in_region(lon: float, lat: float) -> bool:
    lon0, lat0, lon1, lat1 = REGION_BOUNDS
    return lon0 <= lon <= lon1 and lat0 <= lat <= lat1


def nearest_land_km(array, px: int, py: int, km_per_texel: float,
                    start_radius: int = 64) -> float:
    """Distância (km) até o texel de terra mais próximo; ``inf`` se não houver."""
    height, width = array.shape
    radius = start_radius
    while True:
        x0, x1 = max(0, px - radius), min(width, px + radius + 1)
        y0, y1 = max(0, py - radius), min(height, py + radius + 1)
        window = array[y0:y1, x0:x1] >= LAND_MIN_VALUE
        ys, xs = window.nonzero()
        if len(ys):
            dx = xs + x0 - px
            dy = ys + y0 - py
            return float(math.sqrt(int((dx * dx + dy * dy).min())) * km_per_texel)
        if x0 == 0 and y0 == 0 and x1 == width and y1 == height:
            return float("inf")
        radius = min(radius * 4, max(width, height))


def land_distance_km(root: Path, x: float, y: float) -> tuple:
    """``(distância_km, é_terra, camada, motivo)`` do ponto no atlas.

    Usa a região de alta resolução quando o ponto está dentro dela — a mesma
    escolha do shader.
    """
    lon, lat = plan_to_lonlat(x, y)
    if point_in_region(lon, lat):
        array, km, reason = load_land_raster(root, REGION_SDF_FILE)
        if array is not None:
            px, py = region_to_pixel(lon, lat, array.shape[1], array.shape[0])
            if 0 <= px < array.shape[1] and 0 <= py < array.shape[0]:
                if int(array[py, px]) >= LAND_MIN_VALUE:
                    return 0.0, True, "região", ""
                return (nearest_land_km(array, px, py, km), False,
                        "região", reason)
    array, km, reason = load_land_raster(root, WORLD_SDF_FILE)
    if array is None:
        return 0.0, True, "mundo", reason
    px, py = lonlat_to_pixel(lon, lat, array.shape[1], array.shape[0])
    if not (0 <= px < array.shape[1] and 0 <= py < array.shape[0]):
        return 0.0, True, "mundo", "ponto fora do raster"
    if int(array[py, px]) >= LAND_MIN_VALUE:
        return 0.0, True, "mundo", ""
    return nearest_land_km(array, px, py, km), False, "mundo", ""


def province_owner(root: Path, x: float, y: float) -> tuple:
    """``(id, país_dono, motivo)`` da província do ponto no atlas."""
    ids, provinces, reason = load_province_raster(root)
    if ids is None:
        return 0, "", reason
    lon, lat = plan_to_lonlat(x, y)
    px, py = lonlat_to_pixel(lon, lat, ids.shape[1], ids.shape[0])
    if not (0 <= px < ids.shape[1] and 0 <= py < ids.shape[0]):
        return 0, "", "ponto fora do raster"
    key = int(ids[py, px])
    record = provinces.get(key, {})
    return key, as_text(record.get("country")), ""


def check_land_points(report: Report, root: Path, loaded: dict, kind: str) -> None:
    """Cidade/porto em água aberta ou em província sem dono = erro.

    Roda sobre o JSON (o arquivo que o jogo carrega). A camada usada é a mesma
    do shader: a região de alta resolução dentro de ``REGION_BOUNDS``, o mundo
    fora dela.
    """
    if "json" not in loaded:
        return
    sec, rows, _structure = loaded["json"]
    if not rows:
        return
    _, _, reason = load_land_raster(root, WORLD_SDF_FILE)
    if reason:
        sec.warnings.append(f"checagem de terra não executada: {reason}")
        return
    for row in rows:
        x = parse_number(row.data.get("x"))
        y = parse_number(row.data.get("y"))
        if x is None or y is None:
            continue  # check_coordinate já aponta o problema
        distance, on_land, layer, error = land_distance_km(root, x, y)
        if error:
            sec.warnings.append(f"checagem de terra não executada: {error}")
            return
        item = as_text(row.data.get("id")) or row.label
        lon, lat = plan_to_lonlat(x, y)
        if not on_land and distance > COAST_TOLERANCE_KM:
            sec.errors.append(
                f"{row.label}: {kind} '{item}' em água no atlas "
                f"({layer}, {fmt_number(round(distance, 1))} km da terra; "
                f"lat {lat:.3f}, lon {lon:.3f}) — ilha removida? remova em "
                "tools/world_city_curation.py (EXCLUDED/EXCLUDED_PORTS) ou "
                "corrija a coordenada"
            )
            continue
        if on_land:
            province_id, country, why = province_owner(root, x, y)
            if why:
                sec.warnings.append(f"checagem de província não executada: {why}")
                return
            if not country:
                sec.errors.append(
                    f"{row.label}: {kind} '{item}' está na província "
                    f"{province_id} sem dono no atlas "
                    f"(lat {lat:.3f}, lon {lon:.3f})"
                )


def land_report(root: Path, loaded_pairs: list) -> None:
    """Tabela de auditoria: distância à terra de cada cidade/porto."""
    print()
    print("Distância à terra no atlas (pontos no lado da água):")
    print(f"{'tipo':7} {'id':11} {'nome':28} {'lat':>8} {'lon':>9} "
          f"{'km':>9}  camada")
    entries = []
    for kind, loaded in loaded_pairs:
        if "json" not in loaded:
            continue
        _sec, rows, _structure = loaded["json"]
        for row in rows:
            x = parse_number(row.data.get("x"))
            y = parse_number(row.data.get("y"))
            if x is None or y is None:
                continue
            distance, on_land, layer, _error = land_distance_km(root, x, y)
            if on_land or distance <= 0.0:
                continue
            lon, lat = plan_to_lonlat(x, y)
            entries.append((distance, kind, as_text(row.data.get("id")),
                            as_text(row.data.get("name")), lat, lon, layer))
    entries.sort(reverse=True)
    for distance, kind, item, name, lat, lon, layer in entries:
        print(f"{kind:7} {item:11} {name[:28]:28} {lat:8.3f} {lon:9.3f} "
              f"{distance:9.1f}  {layer}")


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

    goods_path = root / GOODS_FILE
    goods = load_json_file(report, root, goods_path)
    if goods is not None:
        sec, rows = goods
        validate_goods(sec, rows)

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
            nation_ids[fmt] = validate_nations(sec, rows, structure, root)
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
            check_capitals(sec, rows, nation_ids[fmt], cities_label)
        if fmt in ports and fmt in nation_ids:
            sec, rows, _structure = ports[fmt]
            check_nation_refs(sec, rows, nation_ids[fmt], rel_path(scenario_dir / f"nations.{fmt}", root))
            if fmt in cities:
                _city_sec, city_rows, _city_structure = cities[fmt]
                check_port_of(sec, rows, city_rows, cities_label)

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
    i18n_keys: Optional[dict] = None
    if i18n_path.exists():
        try:
            i18n_keys = validate_i18n(i18n_sec, i18n_path)
        except (OSError, UnicodeDecodeError) as exc:
            i18n_sec.errors.append(f"não foi possível ler o arquivo: {exc}")
            report.io_failure = True
    else:
        i18n_sec.warnings.append("arquivo opcional não encontrado; verificação ignorada")

    if i18n_keys is not None:
        check_i18n_ids(i18n_sec, i18n_keys, city_ids, port_ids)
        nation_union: set = set()
        for values in nation_ids.values():
            nation_union |= values
        check_nation_i18n(i18n_sec, i18n_keys, nation_union)

    check_land_points(report, root, cities, "cidade")
    check_land_points(report, root, ports, "porto")
    report.entities = [("cidade", cities), ("porto", ports)]
    return report


def main(argv: Optional[list] = None) -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Valida os dados do OpenGS Game "
            "(cenário CSV/JSON, bens da economia e i18n)."
        ),
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
    parser.add_argument(
        "--land-report",
        action="store_true",
        help="lista a distância à terra de cada cidade/porto (auditoria do atlas)",
    )
    args = parser.parse_args(argv)

    root = Path(args.root).expanduser().resolve()
    report = build_report(root)
    total_errors, total_warnings = print_report(report, root)
    if args.land_report:
        land_report(root, getattr(report, "entities", []))

    if report.io_failure:
        return 2
    if total_errors:
        return 1
    if args.strict and total_warnings:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
