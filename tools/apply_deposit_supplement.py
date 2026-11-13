"""Aplica o complemento de depósitos reais ao deposits.json gerado.

O extrato MRDS não traz o pré-sal brasileiro, os campos noruegueses do Mar do
Norte nem parte do gás do Oriente Médio. Este script clona um registro-modelo
do mesmo recurso (para manter a distribuição de atributos) e troca posição,
nome e reservas. Localização real; atributos estimados, como nos demais.

Pode ser rodado de novo (idempotente por nome). Se o deposits.json for
regenerado por tools/build_natural_geography.py, rode este script depois.

Uso: python tools/apply_deposit_supplement.py
"""

import json
import os

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEPOSITS = os.path.join(ROOT, "data", "definitions", "natural", "deposits.json")
SUPPLEMENT = os.path.join(ROOT, "data", "definitions", "natural",
                          "deposits_supplement.json")
NATURE = os.path.join(ROOT, "data", "definitions", "natural", "nature.json")
NAV_MASK = os.path.join(ROOT, "map", "map_data", "nav_mask.png")
PROVINCES = os.path.join(ROOT, "map", "map_data", "atlas", "provinces.json")
WORLD_PROVINCE = os.path.join(ROOT, "map", "map_data", "atlas", "world_province.png")
TEMPLATES = {"oil": "Brent", "gas": "Troll", "coal": "Ruhr", "diamond": "Rio Verde",
             "copper": "El Soldado"}

## Dimensões do mapa e regra de água (iguais a build_resource_map.py /
## geography_service.gd): terra pode tudo; água só petróleo/gás encostado na
## costa. O raster world_province.png tem o mar preenchido com a província de
## terra mais próxima, então ele já resolve o "território costeiro mais próximo"
## para campos offshore (Safaniya->SAU, Shtokman->RUS).
MAP_W, MAP_H = 5000.0, 2500.0
COASTAL_RESOURCES = ("oil", "gas")


def read_json(path):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def find_template(deposits, name):
    for deposit in deposits:
        if name in deposit.get("names", []):
            return deposit
    raise SystemExit("template não encontrado: " + name)


def to_pixels(lon, lat):
    return ((lon + 180.0) / 360.0 * 5000.0, (90.0 - lat) / 180.0 * 2500.0)


def load_nav():
    return Image.open(NAV_MASK).convert("L")


def nav_cell(nav, px, py):
    return int(px / MAP_W * nav.width), int(py / MAP_H * nav.height)


def load_province_raster():
    return Image.open(WORLD_PROVINCE).convert("RGB")


def load_provinces():
    """province_id -> country (adm0_a3)."""
    with open(PROVINCES, encoding="utf-8") as handle:
        data = json.load(handle)
    province_country = {}
    for province in data.get("provinces", []):
        province_country[int(province["id"])] = str(province.get("country", ""))
    return province_country


def nearest_territory(nature, px, py):
    """Id do território cujo centróide está mais próximo de (px, py)."""
    return nearest_territory_among(nature, list(nature.keys()), px, py)


def nearest_territory_among(nature, territory_ids, px, py):
    """Território (entre territory_ids) com centróide mais próximo de (px, py)."""
    best = None
    best_dist = float("inf")
    for territory in territory_ids:
        entry = nature.get(territory, {})
        centroid = entry.get("centroid_px", [])
        if len(centroid) < 2:
            continue
        cx, cy = float(centroid[0]), float(centroid[1])
        dist = (cx - px) ** 2 + (cy - py) ** 2
        if dist < best_dist:
            best_dist = dist
            best = territory
    return best


def assign_territory(record, nature, nav, province_raster, province_country,
                     country_territories):
    """Atribui territory a um depósito pelo território que CONTÉM o ponto.

    A província raster (world_province.png) resolve o país dono: em terra, a
    província real do ponto; no mar, o raster preenche o mar com a província de
    terra mais próxima — i.e., o território costeiro mais próximo. O país
    (adm0_a3) é então mapeado para um territory (TRTxxxxxx) da natureza, o mais
    próximo do ponto. Offshorização (água) de petróleo/gás é marcada com
    data_quality "estimated_offshore_assignment". Retorna True se atribuiu."""
    px = float(record.get("px", 0.0))
    py = float(record.get("py", 0.0))
    x, y = nav_cell(nav, px, py)
    on_land = (0 <= x < nav.width and 0 <= y < nav.height) \
        and nav.getpixel((x, y)) >= 128

    territory = None
    rx = int(px / MAP_W * province_raster.width)
    ry = int(py / MAP_H * province_raster.height)
    if 0 <= rx < province_raster.width and 0 <= ry < province_raster.height:
        r, g, _b = province_raster.getpixel((rx, ry))
        province_id = int(round(r + g * 256))
        country = province_country.get(province_id, "")
        if country:
            territory = nearest_territory_among(
                nature, country_territories.get(country, []), px, py)
    if territory is None:
        # País sem territory na natureza (ex.: estados sem territory): usa o
        # território mais próximo por centróide.
        territory = nearest_territory(nature, px, py)
    if not territory:
        return False

    record["territory"] = territory
    # Offshore só vira "estimated_offshore_assignment" quando o campo não trouxe
    # um data_quality específico (ex.: North Field/Khuff trazem
    # "estimated_public_source" no suplemento e devem preservá-lo).
    if not on_land and str(record.get("resource", "")) in COASTAL_RESOURCES \
            and record.get("data_quality") in (None, "", "real_location_estimated_attributes"):
        record["data_quality"] = "estimated_offshore_assignment"
    return True


def assign_country_territory(record, nature, country_territories, px, py):
    """Atribui `territory` ao território mais próximo da NAÇÃO DECLARADA em
    `owner` (adm0_a3), sem consultar o raster de província em terra.

    É o caminho dos campos offshore cuja posição o raster pinta no vizinho:
    Bayu-Undan/Greater Sunrise caem no mar de Timor preenchido como IDN/AUS,
    mas o Tratado do Mar de Timor (2018) os atribui a TLS. Retorna True se o
    país tem território na natureza e a atribuição foi feita."""
    country = str(record.get("owner", ""))
    if country in ("", "NNN"):
        return False
    territory = nearest_territory_among(
        nature, country_territories.get(country, []), px, py)
    if not territory:
        return False
    record["territory"] = territory
    return True


def write_json(path, data):
    """Escreve no formato do `deposits.json` atual: indentação por TAB, LF e
    newline final. O formato antigo (indent=1, CRLF) reescrevia o arquivo
    inteiro a cada rodada e escondia o diff real."""
    text = json.dumps(data, indent="\t", ensure_ascii=False) + "\n"
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)


def _max_id_suffix(deposits):
    best = 899999
    for deposit in deposits:
        raw = str(deposit.get("id", ""))
        tail = raw.rsplit("-", 1)[-1]
        if tail.isdigit():
            best = max(best, int(tail))
    return best


def _normalize_ids(deposits):
    """Garante IDs únicos: renumera registros com id vazio ou duplicado,
    continuando a partir do maior sufixo existente. Determinístico (ordem)."""
    seen = set()
    next_id = _max_id_suffix(deposits) + 1
    changed = 0
    for deposit in deposits:
        rid = str(deposit.get("id", ""))
        if rid and rid not in seen:
            seen.add(rid)
            continue
        resource = str(deposit.get("resource", "coal")).upper()
        deposit["id"] = "DEP-%s-%06d" % (resource, next_id)
        seen.add(deposit["id"])
        next_id += 1
        changed += 1
    return changed


def main():
    data = read_json(DEPOSITS)
    deposits = data["deposits"]
    existing = {str(d.get("names", [""])[0]).lower() for d in deposits}
    by_name = {}
    for deposit in deposits:
        for deposit_name in deposit.get("names", []):
            by_name[str(deposit_name).lower()] = deposit
    templates = {key: find_template(deposits, name) for key, name in TEMPLATES.items()}
    nature = read_json(NATURE).get("territories", {})
    nav = load_nav()
    province_raster = load_province_raster()
    province_country = load_provinces()
    country_territories = {}
    for territory, entry in nature.items():
        country_territories.setdefault(
            str(entry.get("adm0_a3", "")), []).append(territory)

    added = 0
    synced_owners = 0
    next_id = _max_id_suffix(deposits) + 1
    for field in read_json(SUPPLEMENT)["fields"]:
        name = field["name"]
        if name.lower() in existing:
            # Registro já presente (MRDS/curado): o suplemento é a fonte do
            # `owner` explícito quando ele existe. Quantidade/qualidade do
            # registro existente são preservadas (a calibração pode tê-las
            # ajustado); só a posse declarada é sincronizada.
            deposit = by_name.get(name.lower())
            if deposit is not None and field.get("owner"):
                if str(deposit.get("owner", "")) != str(field["owner"]):
                    deposit["owner"] = str(field["owner"])
                    synced_owners += 1
            continue
        resource = field["resource"]
        template = templates[resource]
        px, py = to_pixels(field["lon"], field["lat"])
        ratio = float(field["quantity_kt"]) / float(template["quantity_kt"])
        record = dict(template)
        record["id"] = "DEP-%s-%06d" % (resource.upper(), next_id)
        next_id += 1
        record["territory"] = ""
        record["owner"] = field.get("owner", "NNN")
        record["px"] = round(px, 1)
        record["py"] = round(py, 1)
        record["lon"] = field["lon"]
        record["lat"] = field["lat"]
        record["quantity_kt"] = float(field["quantity_kt"])
        record["recoverable_kt"] = round(float(template["recoverable_kt"]) * ratio, 1)
        record["quality"] = float(field["quality"])
        record["depth_m"] = int(field["depth_m"])
        # Taxa pela fórmula (reserva recuperável / 25 anos), não pela razão do
        # template — senão diamante (template com taxa no piso 1,0) fica inflado.
        record["max_rate_kt_day"] = max(1.0, round(record["recoverable_kt"] / (25.0 * 365.0), 1))
        record["status"] = field.get("status", "Producer")
        record["names"] = [name]
        record["sites"] = 1
        record["data_quality"] = field.get("data_quality",
                                           "real_location_estimated_attributes")
        if field.get("source"):
            record["source"] = field["source"]
        # Posse explícita (TLS no Mar de Timor, QAT no North Field): atribui o
        # território da NAÇÃO DECLARADA, não o raster de província em terra —
        # o mar de Timor é preenchido pelo raster com IDN e o campo é TLS.
        if str(record.get("owner", "")) not in ("", "NNN") \
                and assign_country_territory(record, nature, country_territories, px, py):
            pass
        deposits.append(record)
        existing.add(name.lower())
        by_name[name.lower()] = record
        added += 1

    # Repara depósitos órfãos (territory vazio): atribui o território pelo
    # ponto (terra) ou pelo território costeiro mais próximo (offshore até
    # ~320 km). Idempotente — só toca registros sem territory. Cobre tanto os
    # suplementos acima quanto os de add_rare_earths.py / add_uranium.py que
    # também entravam com territory vazio.
    repaired = 0
    offshore = 0
    unassigned = 0
    for deposit in deposits:
        if str(deposit.get("territory", "")) != "":
            continue
        px = float(deposit.get("px", 0.0))
        py = float(deposit.get("py", 0.0))
        # Posse declarada vence o raster: cobre campos offshore cujo país real
        # não é o preenchimento de mar do world_province.png.
        if str(deposit.get("owner", "")) not in ("", "NNN") \
                and assign_country_territory(
                    deposit, nature, country_territories, px, py):
            repaired += 1
            continue
        if assign_territory(deposit, nature, nav, province_raster,
                            province_country, country_territories):
            repaired += 1
            if deposit.get("data_quality") == "estimated_offshore_assignment":
                offshore += 1
        else:
            unassigned += 1

    if added or repaired:
        write_json(DEPOSITS, data)

    # Ajustes de campos maduros: reserva restante em vez da original.
    # Idempotência: o nome do ajuste fica em `_meta.supplement_adjustments_applied`
    # e um ajuste já aplicado não é reaplicado. Sem isso, rodar o suplemento sobre
    # um deposits.json JÁ CALIBRADO reduzia a reserva dos campos maduros de novo
    # (Brent/Forties/Statfjord/Oseberg/Groningen) — a calibração posterior não é
    # chamada nesta rodada, então o total mundial tinha de ficar intacto.
    by_name = {}
    for deposit in deposits:
        for name in deposit.get("names", []):
            by_name[str(name).lower()] = deposit
    applied_adjustments = data.setdefault("_meta", {}).setdefault(
        "supplement_adjustments_applied", [])
    if not isinstance(applied_adjustments, list):
        applied_adjustments = []
        data["_meta"]["supplement_adjustments_applied"] = applied_adjustments
    adjusted = 0
    for adjustment in read_json(SUPPLEMENT).get("adjustments", []):
        name = str(adjustment["name"])
        if name in applied_adjustments:
            continue
        if adjustment.get("already_applied"):
            # Ajuste histórico já incorporado à base calibrada (ex.: Brent).
            applied_adjustments.append(name)
            continue
        deposit = by_name.get(name.lower())
        if deposit is None:
            continue
        old = float(deposit.get("quantity_kt", 0.0))
        new = float(adjustment["quantity_kt"])
        ratio = new / old if old > 0.0 else 0.0
        deposit["quantity_kt"] = new
        deposit["recoverable_kt"] = round(float(deposit.get("recoverable_kt", 0.0)) * ratio, 1)
        deposit["max_rate_kt_day"] = round(
                float(deposit.get("max_rate_kt_day", 0.0)) * ratio, 1)
        if "status" in adjustment:
            deposit["status"] = adjustment["status"]
        applied_adjustments.append(name)
        adjusted += 1

    # Garante unicidade (corrige IDs duplicados de execuções antigas).
    renumbered = _normalize_ids(deposits)

    if added or adjusted or renumbered or repaired or synced_owners:
        write_json(DEPOSITS, data)
    print("depósitos adicionados:", added, "de", len(read_json(SUPPLEMENT)["fields"]),
          "| proprietários sincronizados:", synced_owners,
          "| ajustados:", adjusted, "| IDs renumerados:", renumbered,
          "| território atribuído:", repaired, "(offshore:", offshore,
          "| sem terra:", unassigned, ")")


main()
