#!/usr/bin/env python3
"""Validador da Parte G (agricultura sazonal).

Somente leitura: abre os arquivos de `data/definitions/agriculture/`, confere as
11 regras do briefing e reprova (exit 1) quando algo não bate. Não gera, não
corrige e não reescreve dado.

Uso:
    python tools/validate_agriculture_g.py

Saída: uma seção por checagem (OK ou lista de problemas), resumo por arquivo,
totais, avisos separados dos erros e `EXIT=0` / `EXIT=1`.

Teto de rendimento (checagem 5): o briefing fixa 30 t/ha, que vale para
wheat/rice/maize/soy (máximos observados no arquivo: 22,5 t/ha). A batata passa
disso em condição de campo: o próprio crop_yields traz 51,3 t/ha de média
nacional (FAOSTAT). Por isso `YIELD_CEILING` separa o teto da batata (60 t/ha,
acima de qualquer média nacional publicada) e registra como AVISO toda batata
acima de 30 — assim o teto do briefing continua visível, mas não reprova dado
real. Ajuste a tabela se o gerente decidir outro valor.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEF = ROOT / "data" / "definitions"
AGRI = DEF / "agriculture"
NATIONS_PATH = DEF / "scenario" / "nations.json"
GOODS_PATH = DEF / "economy" / "goods.json"

CROPS = ("wheat", "rice", "maize", "potato", "soy")

SHELF_PRODUCTS = (
    "wheat",
    "rice",
    "maize",
    "potato",
    "soy",
    "flour",
    "bran",
    "bread",
    "bagged_rice",
    "feed",
    "meat",
    "canned_food",
)

LIVESTOCK_PRODUCTS = ("meat", "milk", "feed")

# Teto de sanidade por cultura (t/ha). Cereais e soja: 30 do briefing.
# Batata: 60 (médias nacionais publicadas chegam a ~51 t/ha; ver docstring).
YIELD_CEILING = {
    "wheat": 30.0,
    "rice": 30.0,
    "maize": 30.0,
    "soy": 30.0,
    "potato": 60.0,
}

CALENDAR_STATUS = {"observed", "rule_neighbor", "no_data"}
YIELD_STATUS = {"observed", "no_data"}
STOCK_METHODS = {"USDA_PSD", "rule_days_consumption"}
LOSS_STATUS = {"observed", "regional_observed", "regional_default", "no_data"}
LOSS_SOURCE = {
    "observed": "APHLIS",
    "regional_observed": "FAO_2011",
    "regional_default": "RULE_REGIONAL_MEDIAN",
}
FERT_STATUS = {"observed", "regional_default", "no_data"}
RULE_TIERS = {"same_region", "same_band_nearest", "adjacent_band"}
HEMISPHERES = {"north", "south"}

# Arquivos com uma entrada por nação (6) e o arquivo por produto (1).
NATION_FILES = (
    "crop_calendar",
    "crop_yields",
    "crop_stocks",
    "postharvest_losses",
    "fertilizer_n",
    "livestock_cycle",
)
CROP_FILES = (
    "crop_calendar",
    "crop_yields",
    "crop_stocks",
    "postharvest_losses",
    "fertilizer_n",
)
# livestock_cycle não é por cultura (cadeia feed -> meat, mais leite de
# referência): `_meta.crops` não se aplica a ele, como `nations` não se aplica
# a product_shelf_life (por produto). Ver a nota impressa no cabeçalho.
ALL_FILES = NATION_FILES + ("product_shelf_life",)

META_KEYS = ("dataset", "version", "generated_by", "retrieved")


class Report:
    """Acumula problemas (reprovam) e avisos (não reprovam)."""

    def __init__(self) -> None:
        self.errors: list[str] = []
        self.warnings: list[str] = []

    def err(self, path: str, msg: str) -> None:
        self.errors.append(f"{path}: {msg}")

    def warn(self, path: str, msg: str) -> None:
        self.warnings.append(f"{path}: {msg}")


def is_num(value) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def is_int(value) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def read_bytes(path: Path) -> bytes:
    return path.read_bytes()


def load_json(path: Path):
    # Decodifica tolerando BOM para que a checagem 11 possa *relatar* o BOM em
    # vez de o validador quebrar na leitura (o problema continua sendo erro).
    text = path.read_bytes().decode("utf-8-sig")
    return json.loads(text)


def walk_sources(node, prefix: str = ""):
    """Todo campo `source` (string) dentro do payload, com o caminho."""
    if isinstance(node, dict):
        for key, value in node.items():
            path = f"{prefix}.{key}" if prefix else key
            if key == "source" and isinstance(value, str):
                yield path, value
            else:
                yield from walk_sources(value, path)
    elif isinstance(node, list):
        for index, value in enumerate(node):
            yield from walk_sources(value, f"{prefix}[{index}]")


def check_structure(docs: dict[str, dict], report: Report) -> None:
    for name in ALL_FILES:
        doc = docs[name]
        if not isinstance(doc, dict):
            report.err(name, "o arquivo não é um objeto JSON")
            continue
        meta = doc.get("_meta")
        if not isinstance(meta, dict):
            report.err(name, "falta `_meta` (objeto)")
            meta = {}
        for key in META_KEYS:
            if not meta.get(key):
                report.err(f"{name}._meta", f"falta `{key}`")
        sources = doc.get("sources")
        if not isinstance(sources, dict) or not sources:
            report.err(name, "falta `sources` (objeto não vazio)")
            sources = {}

        if name in NATION_FILES:
            nations = doc.get("nations")
            if not isinstance(nations, dict):
                report.err(name, "falta `nations` (objeto)")
                continue
            if name != "livestock_cycle" and (
                not isinstance(meta.get("crops"), (list, dict)) or not meta.get("crops")
            ):
                report.err(f"{name}._meta", "falta `crops`")
            declared = meta.get("nations")
            if not is_int(declared):
                report.err(f"{name}._meta", "falta `nations` (contagem inteira)")
            elif declared != len(nations):
                report.err(
                    f"{name}._meta.nations",
                    f"declara {declared}, mas o arquivo tem {len(nations)} nações",
                )
            referenced = doc["nations"]
        else:
            # product_shelf_life é por produto: não tem (nem deve ter) `nations`.
            products = doc.get("products")
            if not isinstance(products, dict):
                report.err(name, "falta `products` (objeto)")
                continue
            declared = meta.get("products")
            if not is_int(declared):
                report.err(f"{name}._meta", "falta `products` (contagem inteira)")
            elif declared != len(products):
                report.err(
                    f"{name}._meta.products",
                    f"declara {declared}, mas o arquivo tem {len(products)} produtos",
                )
            referenced = doc["products"]

        for path, source in walk_sources(referenced, ""):
            if source not in sources:
                report.err(f"{name}.{path}", f"source `{source}` não existe em `sources`")


def check_nations(docs: dict[str, dict], expected: set[str], report: Report) -> None:
    for name in NATION_FILES:
        actual = set(docs[name].get("nations", {}))
        unknown = sorted(actual - expected)
        missing = sorted(expected - actual)
        if unknown:
            report.err(f"{name}.nations", f"ids desconhecidos: {unknown}")
        if missing:
            report.err(f"{name}.nations", f"nações faltando: {missing}")
        if not unknown and not missing and len(actual) != 179:
            report.err(f"{name}.nations", f"{len(actual)} nações (esperado 179)")


def check_crops(docs: dict[str, dict], goods: dict[str, dict], report: Report) -> None:
    for name in CROP_FILES:
        for nation_id, nation in docs[name]["nations"].items():
            keys = set(nation.get("crops", {}))
            extra = sorted(keys - set(CROPS))
            missing = sorted(set(CROPS) - keys)
            if extra:
                report.err(
                    f"{name}.nations.{nation_id}.crops", f"culturas fora do conjunto: {extra}"
                )
            if missing:
                report.err(
                    f"{name}.nations.{nation_id}.crops", f"culturas faltando: {missing}"
                )

    for crop in CROPS:
        row = goods.get(crop)
        if row is None:
            report.err("goods.json", f"cultura `{crop}` não existe em economy/goods.json")
        elif row.get("kind") != "raw" or row.get("food") is not True:
            report.err(
                "goods.json",
                f"cultura `{crop}` não é kind=raw/food=true "
                f"(kind={row.get('kind')!r}, food={row.get('food')!r})",
            )

    livestock = docs["livestock_cycle"]
    products = livestock.get("products", {})
    for product in LIVESTOCK_PRODUCTS:
        if product in goods:
            continue
        entry = products.get(product)
        if entry is None:
            report.err(
                "livestock_cycle.products",
                f"`{product}` não existe em goods.json e não está declarado",
            )
        elif entry.get("in_game") is not False:
            report.err(
                f"livestock_cycle.products.{product}",
                "não existe em goods.json e não está declarado `in_game:false`",
            )
    for product in products:
        if product not in goods and products[product].get("in_game") is not False:
            report.err(
                f"livestock_cycle.products.{product}",
                "não existe em goods.json e não está declarado `in_game:false`",
            )

    shelf = docs["product_shelf_life"].get("products", {})
    expected_shelf = set(SHELF_PRODUCTS)
    actual_shelf = set(shelf)
    missing = sorted(expected_shelf - actual_shelf)
    extra = sorted(actual_shelf - expected_shelf)
    if missing:
        report.err("product_shelf_life.products", f"produtos faltando: {missing}")
    if extra:
        report.err("product_shelf_life.products", f"produtos fora do conjunto: {extra}")
    for product in sorted(actual_shelf):
        if product not in goods:
            report.err(
                f"product_shelf_life.products.{product}",
                "produto não existe em economy/goods.json",
            )


def check_calendar(doc: dict, report: Report) -> None:
    for nation_id, nation in doc["nations"].items():
        for crop_id, crop in nation["crops"].items():
            base = f"crop_calendar.nations.{nation_id}.crops.{crop_id}"
            if crop.get("hemisphere") not in HEMISPHERES:
                report.err(f"{base}.hemisphere", f"valor inválido: {crop.get('hemisphere')!r}")
            status = crop.get("data_status")
            if status not in CALENDAR_STATUS:
                report.err(f"{base}.data_status", f"valor inválido: {status!r}")
            seasons = crop.get("seasons")
            if not isinstance(seasons, list):
                report.err(f"{base}.seasons", "não é lista")
                seasons = []

            for index, season in enumerate(seasons):
                spath = f"{base}.seasons[{index}]"
                for side in ("planting", "harvest"):
                    window = season.get(side)
                    if not isinstance(window, dict):
                        report.err(f"{spath}.{side}", "não é objeto com start_month/end_month")
                        continue
                    for key in ("start_month", "end_month"):
                        value = window.get(key)
                        if not is_int(value) or not 1 <= value <= 12:
                            report.err(f"{spath}.{side}.{key}", f"mês inválido: {value!r}")
                source = season.get("source")
                if not isinstance(source, str) or not source:
                    report.err(f"{spath}.source", "faltando")
                if "source_year" not in season:
                    report.err(f"{spath}.source_year", "campo ausente")
                else:
                    year = season["source_year"]
                    if year is None:
                        if source != "JRC_ASAP":
                            report.err(
                                f"{spath}.source_year",
                                f"null para source `{source}` (só JRC_ASAP não publica ano)",
                            )
                    elif not is_int(year) or not 1961 <= year <= 2030:
                        report.err(f"{spath}.source_year", f"ano inválido: {year!r}")

            if status == "rule_neighbor":
                rule = crop.get("rule")
                if not isinstance(rule, dict):
                    report.err(f"{base}.rule", "ausente em rule_neighbor")
                else:
                    if rule.get("tier") not in RULE_TIERS:
                        report.err(f"{base}.rule.tier", f"valor inválido: {rule.get('tier')!r}")
                    donor = rule.get("donor")
                    if not isinstance(donor, str) or not donor:
                        report.err(f"{base}.rule.donor", "faltando")
                    elif donor not in doc["nations"]:
                        report.err(f"{base}.rule.donor", f"nação doadora desconhecida: {donor}")
                    distance = rule.get("distance_km")
                    if not is_num(distance) or distance < 0:
                        report.err(f"{base}.rule.distance_km", f"valor inválido: {distance!r}")
            if status == "no_data":
                reason = crop.get("reason")
                if not isinstance(reason, str) or not reason.strip():
                    report.err(f"{base}.reason", "obrigatório em no_data")

            if crop.get("all_year") is True and not seasons:
                report.warn(f"{base}.all_year", "all_year sem meses (sazonalidade anual)")
            for index, season in enumerate(seasons):
                planting = season.get("planting") or {}
                harvest = season.get("harvest") or {}
                if (
                    planting.get("start_month") is not None
                    and planting.get("start_month") == harvest.get("start_month")
                    and planting.get("end_month") == harvest.get("end_month")
                ):
                    report.warn(
                        f"{base}.seasons[{index}]",
                        "plantio e colheita com a mesma janela "
                        f"({planting.get('start_month')}..{planting.get('end_month')})",
                    )


def check_yields(doc: dict, report: Report) -> None:
    for nation_id, nation in doc["nations"].items():
        for crop_id, crop in nation["crops"].items():
            base = f"crop_yields.nations.{nation_id}.crops.{crop_id}"
            for field in ("yield_t_ha", "area_harvested_ha", "production_t"):
                value = crop.get(field)
                if value is None:
                    continue
                if not is_num(value):
                    report.err(f"{base}.{field}", f"não numérico: {value!r}")
                elif value < 0:
                    report.err(f"{base}.{field}", f"negativo: {value}")

            yld = crop.get("yield_t_ha")
            ceiling = YIELD_CEILING.get(crop_id, 30.0)
            if is_num(yld):
                if yld > ceiling:
                    report.err(
                        f"{base}.yield_t_ha",
                        f"{yld} t/ha acima do teto de sanidade de {ceiling:g} t/ha",
                    )
                elif crop_id == "potato" and yld > 30.0:
                    report.warn(
                        f"{base}.yield_t_ha",
                        f"{yld} t/ha acima dos 30 t/ha do briefing (batata; teto usado "
                        f"{ceiling:g})",
                    )

            year = crop.get("year")
            if year is not None:
                if not is_int(year) or not 1961 <= year <= 2024:
                    report.err(f"{base}.year", f"ano fora de 1961..2024: {year!r}")

            status = crop.get("data_status")
            if status not in YIELD_STATUS:
                report.err(f"{base}.data_status", f"valor inválido: {status!r}")
            if status == "no_data":
                reason = crop.get("reason")
                if not isinstance(reason, str) or not reason.strip():
                    report.err(f"{base}.reason", "obrigatório em no_data")


def check_stocks(doc: dict, report: Report) -> None:
    for nation_id, nation in doc["nations"].items():
        for crop_id, crop in nation["crops"].items():
            base = f"crop_stocks.nations.{nation_id}.crops.{crop_id}"
            status = crop.get("data_status")
            for field in ("ending_stocks_t", "days_of_consumption"):
                value = crop.get(field)
                if value is None:
                    continue
                if not is_num(value):
                    report.err(f"{base}.{field}", f"não numérico: {value!r}")
                elif value < 0:
                    report.err(f"{base}.{field}", f"negativo: {value}")

            method = crop.get("method")
            if method not in STOCK_METHODS:
                report.err(f"{base}.method", f"valor inválido: {method!r}")

            year = crop.get("marketing_year")
            if year is None:
                if status != "no_data":
                    report.err(f"{base}.marketing_year", "null fora de no_data")
            elif not is_int(year) or not 1961 <= year <= 2024:
                report.err(f"{base}.marketing_year", f"ano fora de 1961..2024: {year!r}")

            if method == "rule_days_consumption" and status != "no_data":
                days = crop.get("days_of_consumption")
                if not is_num(days) or abs(days - 60.0) > 1e-9:
                    report.err(
                        f"{base}.days_of_consumption",
                        f"{days!r} em rule_days_consumption (a regra declara 60)",
                    )


def check_losses(doc: dict, report: Report) -> None:
    for nation_id, nation in doc["nations"].items():
        for crop_id, crop in nation["crops"].items():
            base = f"postharvest_losses.nations.{nation_id}.crops.{crop_id}"
            for field in ("loss_pct", "storage_share_pct"):
                value = crop.get(field)
                if value is None:
                    continue
                if not is_num(value):
                    report.err(f"{base}.{field}", f"não numérico: {value!r}")
                elif not 0 <= value <= 100:
                    report.err(f"{base}.{field}", f"fora de 0..100: {value}")

            status = crop.get("data_status")
            if status not in LOSS_STATUS:
                report.err(f"{base}.data_status", f"valor inválido: {status!r}")
                continue

            if status in ("observed", "regional_observed"):
                refs = crop.get("refs")
                if not isinstance(refs, list) or not refs:
                    report.err(f"{base}.refs", f"obrigatório em {status}")
                else:
                    for index, ref in enumerate(refs):
                        url = ref.get("url") if isinstance(ref, dict) else None
                        if not isinstance(url, str) or not url.strip():
                            report.err(f"{base}.refs[{index}].url", "faltando")

            expected_source = LOSS_SOURCE.get(status)
            if expected_source and crop.get("source") != expected_source:
                report.err(
                    f"{base}.source",
                    f"{crop.get('source')!r} incoerente com data_status `{status}` "
                    f"(esperado {expected_source})",
                )


def check_fertilizer(doc: dict, report: Report) -> None:
    for nation_id, nation in doc["nations"].items():
        for crop_id, crop in nation["crops"].items():
            base = f"fertilizer_n.nations.{nation_id}.crops.{crop_id}"
            status = crop.get("data_status")
            value = crop.get("n_kg_ha")
            if value is None:
                if status != "no_data":
                    report.err(f"{base}.n_kg_ha", "null fora de no_data")
            elif not is_num(value):
                report.err(f"{base}.n_kg_ha", f"não numérico: {value!r}")
            elif value < 0 or value > 400:
                report.err(f"{base}.n_kg_ha", f"fora de 0..400 kg N/ha: {value}")

            year = crop.get("year")
            if year is not None:
                if not is_int(year) or not 1980 <= year <= 2024:
                    report.err(f"{base}.year", f"ano fora de 1980..2024: {year!r}")

            if status not in FERT_STATUS:
                report.err(f"{base}.data_status", f"valor inválido: {status!r}")
            if status == "observed" and year is None:
                report.err(f"{base}.year", "observed exige ano")


def check_livestock(docs: dict[str, dict], report: Report) -> None:
    doc = docs["livestock_cycle"]
    for nation_id, nation in doc["nations"].items():
        base = f"livestock_cycle.nations.{nation_id}"
        for block, field in (
            ("cattle", "carcass_kg_per_head"),
            ("milk", "kg_per_head_year"),
        ):
            entry = nation.get(block)
            if not isinstance(entry, dict):
                report.err(f"{base}.{block}", "faltando")
                continue
            value = entry.get(field)
            if value is not None:
                if not is_num(value):
                    report.err(f"{base}.{block}.{field}", f"não numérico: {value!r}")
                elif value <= 0:
                    report.err(f"{base}.{block}.{field}", f"deve ser > 0: {value}")
            year = entry.get("year")
            if year is None:
                if entry.get("data_status") != "no_data":
                    report.err(f"{base}.{block}.year", "null fora de no_data")
            elif not is_int(year) or not 1961 <= year <= 2024:
                report.err(f"{base}.{block}.year", f"ano fora de 1961..2024: {year!r}")

    products = doc.get("products", {})
    for product in ("meat", "milk"):
        entry = products.get(product)
        pbase = f"livestock_cycle.products.{product}"
        if not isinstance(entry, dict):
            report.err(pbase, "faltando")
            continue
        if entry.get("seasonality") != "continuous":
            report.err(
                f"{pbase}.seasonality", f"esperado `continuous`, veio {entry.get('seasonality')!r}"
            )
        factor = entry.get("monthly_factor")
        if factor is not None:
            has_support = bool(entry.get("source")) and bool(
                entry.get("url") or entry.get("refs")
            )
            if not has_support:
                report.err(
                    f"{pbase}.monthly_factor",
                    "série mensal sem source/url que a sustente",
                )

    shelf = docs["product_shelf_life"].get("products", {})
    for product, entry in shelf.items():
        sbase = f"product_shelf_life.products.{product}"
        days = entry.get("storage_days")
        if days is not None:
            if not is_num(days):
                report.err(f"{sbase}.storage_days", f"não numérico: {days!r}")
            elif days <= 0:
                report.err(f"{sbase}.storage_days", f"deve ser > 0: {days}")
        status = entry.get("data_status")
        if status not in ("observed", "no_data"):
            report.err(f"{sbase}.data_status", f"valor inválido: {status!r}")
        if status == "observed":
            if not entry.get("source"):
                report.err(f"{sbase}.source", "observed exige source")
            url = entry.get("url")
            if not isinstance(url, str) or not url.strip():
                report.err(f"{sbase}.url", "observed exige url")


def check_cross(yields: dict, stocks: dict, report: Report) -> None:
    for nation_id, nation in yields["nations"].items():
        for crop_id, crop in nation["crops"].items():
            if crop.get("data_status") != "no_data":
                continue
            stock = stocks["nations"].get(nation_id, {}).get("crops", {}).get(crop_id, {})
            if stock.get("method") == "USDA_PSD":
                report.warn(
                    f"cross.{nation_id}.{crop_id}",
                    "crop_yields=no_data, mas crop_stocks traz estoque USDA_PSD observado",
                )


def check_format(report: Report) -> None:
    for name in ALL_FILES:
        path = AGRI / f"{name}.json"
        try:
            raw = read_bytes(path)
        except OSError as exc:  # pragma: no cover - só se o arquivo sumir
            report.err(name, f"não foi possível ler: {exc}")
            continue
        if raw.startswith(b"\xef\xbb\xbf"):
            report.err(name, "arquivo com BOM UTF-8")
        crlf = raw.count(b"\r\n")
        if crlf:
            report.err(name, f"contém CRLF ({crlf} ocorrência(s))")
        try:
            raw.decode("utf-8")
        except UnicodeDecodeError as exc:
            report.err(name, f"não é UTF-8 válido: {exc}")


def status_counts(name: str, doc: dict) -> tuple[int, dict[str, int]]:
    counts: dict[str, int] = {}
    entries = 0
    if name == "product_shelf_life":
        for entry in doc.get("products", {}).values():
            entries += 1
            status = entry.get("data_status")
            counts[str(status)] = counts.get(str(status), 0) + 1
        return entries, counts
    for nation in doc["nations"].values():
        if name == "livestock_cycle":
            blocks = [nation.get("cattle"), nation.get("milk")]
        else:
            blocks = list(nation.get("crops", {}).values())
        for entry in blocks:
            if not isinstance(entry, dict):
                continue
            entries += 1
            status = entry.get("data_status")
            counts[str(status)] = counts.get(str(status), 0) + 1
    return entries, counts


def main() -> int:
    docs: dict[str, dict] = {}
    report = Report()

    for name in ALL_FILES:
        path = AGRI / f"{name}.json"
        if not path.exists():
            print(f"ERRO  {name}: arquivo ausente ({path})")
            return 1
        try:
            docs[name] = load_json(path)
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            print(f"ERRO  {name}: não é JSON UTF-8 válido ({exc})")
            print("EXIT=1")
            return 1

    expected_nations = {row["id"] for row in load_json(NATIONS_PATH)}
    goods = {row["id"]: row for row in load_json(GOODS_PATH)}

    checks = (
        ("1) estrutura (_meta/sources/nations/source citado)", check_structure, (docs, report)),
        ("2) nações (179 ids exatos)", check_nations, (docs, expected_nations, report)),
        ("3) culturas e produtos", check_crops, (docs, goods, report)),
        ("4) calendário", check_calendar, (docs["crop_calendar"], report)),
        ("5) rendimento/área", check_yields, (docs["crop_yields"], report)),
        ("6) estoque", check_stocks, (docs["crop_stocks"], report)),
        ("7) perdas pós-colheita", check_losses, (docs["postharvest_losses"], report)),
        ("8) fertilizante N", check_fertilizer, (docs["fertilizer_n"], report)),
        ("9) pecuária e prateleira", check_livestock, (docs, report)),
        (
            "10) coerência transversal (yields x stocks)",
            check_cross,
            (docs["crop_yields"], docs["crop_stocks"], report),
        ),
        ("11) formato (UTF-8 sem BOM, sem CRLF)", check_format, (report,)),
    )

    print("== Validador da Parte G (agricultura sazonal) ==")
    print(f"pasta: {AGRI}")
    print()
    print("nota: `_meta.crops` é exigido nos 5 arquivos de cultura; livestock_cycle é")
    print("      por produto (cadeia feed -> meat, leite de referência) e não tem")
    print("      culturas, e product_shelf_life é por produto (não tem `nations`).")
    print()

    jrc_null = sum(
        1
        for nation in docs["crop_calendar"]["nations"].values()
        for crop in nation["crops"].values()
        for season in crop.get("seasons", [])
        if season.get("source") == "JRC_ASAP" and season.get("source_year") is None
    )
    post_notes = {
        3: [
            f"safras com source_year=null aceitas: {jrc_null} "
            "(todas JRC_ASAP, que não publica ano)"
        ]
    }

    for index, (title, func, args) in enumerate(checks):
        before_err = len(report.errors)
        before_warn = len(report.warnings)
        func(*args)
        new_err = report.errors[before_err:]
        new_warn = report.warnings[before_warn:]
        print(f"[{title}]")
        if new_err:
            print(f"  PROBLEMAS: {len(new_err)}")
            for line in new_err:
                print(f"    - {line}")
        else:
            print("  OK")
        for note in post_notes.get(index, []):
            print(f"  {note}")
        if new_warn:
            print(f"  avisos: {len(new_warn)}")
            for line in new_warn[:20]:
                print(f"    ~ {line}")
            if len(new_warn) > 20:
                print(f"    ~ ... +{len(new_warn) - 20} aviso(s)")
        print()

    print("-- resumo por arquivo --")
    global_status: dict[str, int] = {}
    total_entries = 0
    for name in ALL_FILES:
        doc = docs[name]
        entries, counts = status_counts(name, doc)
        total_entries += entries
        for status, count in counts.items():
            global_status[status] = global_status.get(status, 0) + count
        if name in NATION_FILES:
            scope = f"{len(doc['nations'])} nações"
        else:
            scope = f"{len(doc.get('products', {}))} produtos"
        parts = ", ".join(f"{status}={count}" for status, count in sorted(counts.items()))
        print(f"  {name:<22} {scope:<12} {entries:>4} entradas   {parts}")

    print()
    print("-- totais globais --")
    print(f"  arquivos: {len(ALL_FILES)}")
    print(f"  entradas: {total_entries}")
    print(
        "  por data_status: "
        + ", ".join(f"{status}={count}" for status, count in sorted(global_status.items()))
    )
    print(f"  erros: {len(report.errors)}")
    print(f"  avisos: {len(report.warnings)}")
    print()

    if report.warnings:
        print("-- avisos (não reprovam) --")
        for line in report.warnings:
            print(f"  ~ {line}")
        print()

    if report.errors:
        print("-- erros (reprovam) --")
        for line in report.errors:
            print(f"  ! {line}")
        print()
        print("EXIT=1")
        return 1

    print("EXIT=0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
