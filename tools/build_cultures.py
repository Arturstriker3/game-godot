#!/usr/bin/env python3
"""Gera os dados de cultura do OpenGS Game.

Entrada (curadoria, ``tools/culture_sources/``):
    cultures.csv          catálogo de culturas (id, nomes i18n, família, idioma,
                          religião, notas). A coluna ``color`` é opcional: quando
                          ausente/vazia, a cor é derivada de forma determinística.
    nation_cultures.csv   por nação: cultura principal, culturas aceitas,
                          composição étnica (``id:share;...``), fonte e ano.
    city_overrides_*.csv  dado regional por cidade (``city_id,culture,share,
                          source``); a soma pode ser < 100 e o resto vira
                          ``outras``.

Saída (``data/definitions/culture/``):
    cultures.csv / cultures.json
    nation_cultures.csv
    city_cultures.csv

Regras:
* nenhum número é inventado aqui: o script só agrega/normaliza a curadoria;
* cidade sem override usa a composição nacional com um ajuste de diversidade
  declarado (capitais e metrópoles mais diversas) e ``source=national_fallback``;
* cores são geradas em OKLCH e escolhidas para manter ΔE00 >= 25 dentro da
  mesma família (modo de mapa futuro).

Uso::

    <python> tools/build_cultures.py
    <python> tools/build_cultures.py --check   # não grava, só relata
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCES = Path("tools/culture_sources")
OUT_DIR = Path("data/definitions/culture")
SCENARIO = Path("data/definitions/scenario")

CULTURE_FIELDS = ["id", "name_pt", "name_en", "name_es", "family", "language", "religion", "color"]
NATION_FIELDS = ["nation", "primary_culture", "accepted_cultures", "composition", "source", "year"]
CITY_FIELDS = ["city_id", "nation", "city", "cultures", "source"]
OVERRIDE_FIELDS = ["city_id", "culture", "share", "source"]

OTHER = "outras"
MIN_DELTA_E = 25.0
MAX_NAMED_CITIES = 5

# Peso de diversidade do fallback nacional: p = 1 - k aplicado às frações.
DIVERSITY_BY_POP = ((5_000_000, 0.22), (1_000_000, 0.14), (250_000, 0.08))
CAPITAL_EXTRA = 0.08
MAX_DIVERSITY = 0.30


def read_csv(path: Path, fields: list[str]) -> list[dict]:
    if not path.exists():
        raise SystemExit(f"arquivo de curadoria ausente: {path}")
    with path.open("r", encoding="utf-8", newline="") as fh:
        reader = csv.DictReader(fh)
        missing = [f for f in fields if f not in (reader.fieldnames or [])]
        if missing:
            raise SystemExit(f"{path}: colunas ausentes {missing}")
        return [{k: (v or "").strip() for k, v in row.items()} for row in reader]


def parse_composition(text: str, where: str) -> list[tuple[str, float]]:
    entries: list[tuple[str, float]] = []
    for raw in text.split(";"):
        raw = raw.strip()
        if not raw:
            continue
        if ":" not in raw:
            raise SystemExit(f"{where}: composição inválida {raw!r}")
        cid, share = raw.split(":", 1)
        try:
            value = float(share)
        except ValueError:
            raise SystemExit(f"{where}: fração inválida {raw!r}")
        if value <= 0:
            raise SystemExit(f"{where}: fração não positiva {raw!r}")
        entries.append((cid.strip(), value))
    total = sum(v for _, v in entries)
    if abs(total - 100.0) > 0.05:
        raise SystemExit(f"{where}: composição soma {total:g}, esperado 100")
    return entries


def round_shares(entries: list[tuple[str, float]], where: str) -> list[tuple[str, int]]:
    """Arredonda para inteiros preservando a soma 100 (maior resto)."""
    if not entries:
        raise SystemExit(f"{where}: composição vazia")
    total = sum(v for _, v in entries)
    scaled = [(cid, v * 100.0 / total) for cid, v in entries]
    floors = [(cid, int(math.floor(v)), v - math.floor(v)) for cid, v in scaled]
    remaining = 100 - sum(f[1] for f in floors)
    order = sorted(range(len(floors)), key=lambda i: (-floors[i][2], floors[i][0]))
    result = [[cid, value] for cid, value, _ in floors]
    for idx in order[:remaining]:
        result[idx][1] += 1
    return [(cid, value) for cid, value in result]


# ---------------------------------------------------------------------------
# Cor: OKLCH -> sRGB e CIEDE2000
# ---------------------------------------------------------------------------


def oklch_to_srgb(light: float, chroma: float, hue_deg: float):
    h = math.radians(hue_deg)
    a = chroma * math.cos(h)
    b = chroma * math.sin(h)
    l_ = light + 0.3963377774 * a + 0.2158037573 * b
    m_ = light - 0.1055613458 * a - 0.0638541728 * b
    s_ = light - 0.0894841775 * a - 1.2914855480 * b
    l_, m_, s_ = l_**3, m_**3, s_**3
    r = +4.0767416621 * l_ - 3.3077115913 * m_ + 0.2309699292 * s_
    g = -1.2684380046 * l_ + 2.6097574011 * m_ - 0.3413193965 * s_
    bl = -0.0041960863 * l_ - 0.7034186147 * m_ + 1.7076147010 * s_

    def gamma(x: float) -> float:
        x = max(0.0, min(1.0, x))
        return 1.055 * (x ** (1 / 2.4)) - 0.055 if x > 0.0031308 else 12.92 * x

    rgb = tuple(gamma(v) for v in (r, g, bl))
    clipped = any(not (-0.002 <= v <= 1.002) for v in (r, g, bl))
    return rgb, clipped


def srgb_to_lab(rgb) -> tuple[float, float, float]:
    def linear(x: float) -> float:
        return x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4

    r, g, b = (linear(v) for v in rgb)
    x = (0.4124564 * r + 0.3575761 * g + 0.1804375 * b) / 0.95047
    y = (0.2126729 * r + 0.7151522 * g + 0.0721750 * b) / 1.00000
    z = (0.0193339 * r + 0.1191920 * g + 0.9503041 * b) / 1.08883

    def f(t: float) -> float:
        return t ** (1 / 3) if t > 0.008856 else (7.787 * t + 16 / 116)

    fx, fy, fz = f(x), f(y), f(z)
    return (116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz))


def ciede2000(lab1, lab2) -> float:
    l1, a1, b1 = lab1
    l2, a2, b2 = lab2
    c1 = math.hypot(a1, b1)
    c2 = math.hypot(a2, b2)
    c_bar = (c1 + c2) / 2
    g = 0.5 * (1 - math.sqrt(c_bar**7 / (c_bar**7 + 25**7))) if c_bar else 0.5
    a1p, a2p = (1 + g) * a1, (1 + g) * a2
    c1p, c2p = math.hypot(a1p, b1), math.hypot(a2p, b2)
    h1p = math.degrees(math.atan2(b1, a1p)) % 360 if (a1p or b1) else 0.0
    h2p = math.degrees(math.atan2(b2, a2p)) % 360 if (a2p or b2) else 0.0
    dlp = l2 - l1
    dcp = c2p - c1p
    if c1p * c2p == 0:
        dhp = 0.0
    elif abs(h2p - h1p) <= 180:
        dhp = h2p - h1p
    elif h2p - h1p > 180:
        dhp = h2p - h1p - 360
    else:
        dhp = h2p - h1p + 360
    dHp = 2 * math.sqrt(c1p * c2p) * math.sin(math.radians(dhp) / 2)
    l_bar = (l1 + l2) / 2
    c_bar_p = (c1p + c2p) / 2
    if c1p * c2p == 0:
        h_bar = h1p + h2p
    elif abs(h1p - h2p) <= 180:
        h_bar = (h1p + h2p) / 2
    elif h1p + h2p < 360:
        h_bar = (h1p + h2p + 360) / 2
    else:
        h_bar = (h1p + h2p - 360) / 2
    t = (
        1
        - 0.17 * math.cos(math.radians(h_bar - 30))
        + 0.24 * math.cos(math.radians(2 * h_bar))
        + 0.32 * math.cos(math.radians(3 * h_bar + 6))
        - 0.20 * math.cos(math.radians(4 * h_bar - 63))
    )
    d_theta = 30 * math.exp(-(((h_bar - 275) / 25) ** 2))
    rc = 2 * math.sqrt(c_bar_p**7 / (c_bar_p**7 + 25**7)) if c_bar_p else 0.0
    sl = 1 + 0.015 * (l_bar - 50) ** 2 / math.sqrt(20 + (l_bar - 50) ** 2)
    sc = 1 + 0.045 * c_bar_p
    sh = 1 + 0.015 * c_bar_p * t
    rt = -math.sin(math.radians(2 * d_theta)) * rc
    return math.sqrt(
        (dlp / sl) ** 2
        + (dcp / sc) ** 2
        + (dHp / sh) ** 2
        + rt * (dcp / sc) * (dHp / sh)
    )


def build_candidate_palette() -> list[tuple[str, tuple[float, float, float]]]:
    palette = []
    for light in range(28, 89, 6):
        for chroma in (0.06, 0.10, 0.14, 0.18):
            for hue in range(0, 360, 8):
                rgb, clipped = oklch_to_srgb(light / 100.0, chroma, hue)
                if clipped:
                    continue
                palette.append((hex_of(rgb), srgb_to_lab(rgb)))
    return palette


def hex_of(rgb) -> str:
    return "#" + "".join(f"{max(0, min(255, round(v * 255))):02x}" for v in rgb)


def assign_colors(cultures: list[dict]) -> dict[str, str]:
    palette = build_candidate_palette()
    families: dict[str, list[dict]] = {}
    for culture in cultures:
        families.setdefault(culture["family"], []).append(culture)
    colors: dict[str, str] = {}
    for family, members in sorted(families.items()):
        offset = zlib.crc32(family.encode("utf-8")) % len(palette)
        ordered = palette[offset:] + palette[:offset]
        chosen: list[tuple[str, tuple[float, float, float]]] = []
        for culture in sorted(members, key=lambda c: c["id"]):
            if culture.get("color"):
                hex_value = culture["color"]
                rgb = tuple(int(hex_value[i : i + 2], 16) / 255 for i in (1, 3, 5))
                chosen.append((hex_value, srgb_to_lab(rgb)))
                colors[culture["id"]] = hex_value
                continue
            best = None
            for hex_value, lab in ordered:
                if not chosen:
                    score = 0.0
                else:
                    score = min(math.dist(lab, lab2) for _, lab2 in chosen)
                if best is None or score > best[0]:
                    best = (score, hex_value, lab)
                if chosen and best[0] >= 45:
                    break
            chosen.append((best[1], best[2]))
            colors[culture["id"]] = best[1]
        minimum = min(
            (math.dist(a[1], b[1]) for i, a in enumerate(chosen) for b in chosen[i + 1 :]),
            default=99.0,
        )
        minimum_de00 = min(
            (ciede2000(a[1], b[1]) for i, a in enumerate(chosen) for b in chosen[i + 1 :]),
            default=99.0,
        )
        print(
            f"  família {family:<22} {len(members):>2} cores, "
            f"ΔE*ab mínimo {minimum:.1f} (ΔE00 {minimum_de00:.1f})"
        )
        if minimum < MIN_DELTA_E:
            raise SystemExit(
                f"família {family}: ΔE*ab {minimum:.1f} < {MIN_DELTA_E:g}; ajuste o catálogo"
            )
    return colors


# ---------------------------------------------------------------------------
# Fallback nacional para cidades
# ---------------------------------------------------------------------------


def diversity_weight(population: int, capital: bool) -> float:
    weight = 0.0
    for threshold, value in DIVERSITY_BY_POP:
        if population >= threshold:
            weight = value
            break
    if capital:
        weight += CAPITAL_EXTRA
    return min(weight, MAX_DIVERSITY)


def city_mix(national: list[tuple[str, float]], population: int, capital: bool) -> list[tuple[str, float]]:
    """Achata a distribuição nacional (diversidade urbana) e mantém 5 + outras."""
    weight = diversity_weight(population, capital)
    adjusted = [(cid, (share / 100.0) ** (1.0 - weight)) for cid, share in national]
    total = sum(v for _, v in adjusted)
    adjusted = [(cid, v / total * 100.0) for cid, v in adjusted]
    adjusted.sort(key=lambda e: (-e[1], e[0]))
    named = [e for e in adjusted if e[0] != OTHER][:MAX_NAMED_CITIES]
    rest = sum(v for cid, v in adjusted if cid != OTHER and (cid, v) not in named)
    other_share = rest + sum(v for cid, v in adjusted if cid == OTHER)
    if other_share > 0:
        named.append((OTHER, other_share))
    return [e for e in named if e[1] > 0.001]


def main(argv: list[str] | None = None) -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description="Gera os dados de cultura do OpenGS Game.")
    parser.add_argument("--check", action="store_true", help="não grava arquivos, só relata")
    args = parser.parse_args(argv)

    cultures = read_csv(SOURCES / "cultures.csv", CULTURE_FIELDS[:-1])
    ids = [c["id"] for c in cultures]
    if len(ids) != len(set(ids)):
        raise SystemExit("cultures.csv: ids duplicados")
    known = set(ids)

    nations = read_csv(SOURCES / "nation_cultures.csv", NATION_FIELDS)
    scenario_nations = {r["id"] for r in read_csv(SCENARIO / "nations.csv", ["id"])}
    if {r["nation"] for r in nations} != scenario_nations:
        missing = sorted(scenario_nations - {r["nation"] for r in nations})
        extra = sorted({r["nation"] for r in nations} - scenario_nations)
        raise SystemExit(f"nation_cultures.csv: cobertura inválida faltando={missing} sobrando={extra}")

    nation_mix: dict[str, list[tuple[str, float]]] = {}
    for row in nations:
        where = f"nation_cultures.csv:{row['nation']}"
        mix = parse_composition(row["composition"], where)
        for cid, _ in mix:
            if cid != OTHER and cid not in known:
                raise SystemExit(f"{where}: cultura desconhecida {cid!r}")
        if row["primary_culture"] not in {cid for cid, _ in mix}:
            raise SystemExit(f"{where}: cultura principal fora da composição")
        for accepted in filter(None, row["accepted_cultures"].split(";")):
            if accepted not in known:
                raise SystemExit(f"{where}: cultura aceita desconhecida {accepted!r}")
        nation_mix[row["nation"]] = mix

    overrides: dict[str, list[tuple[str, float, str]]] = {}
    for path in sorted(SOURCES.glob("city_overrides_*.csv")):
        for row in read_csv(path, OVERRIDE_FIELDS):
            where = f"{path.name}:{row['city_id']}"
            if row["culture"] not in known:
                raise SystemExit(f"{where}: cultura desconhecida {row['culture']!r}")
            try:
                share = float(row["share"])
            except ValueError:
                raise SystemExit(f"{where}: fração inválida {row['share']!r}")
            overrides.setdefault(row["city_id"], []).append((row["culture"], share, row["source"]))

    cities = read_csv(SCENARIO / "cities.csv", ["id", "nation", "name", "population", "capital"])
    unknown_city = sorted(set(overrides) - {c["id"] for c in cities})
    if unknown_city:
        raise SystemExit(f"overrides para cidades inexistentes: {unknown_city}")

    colors = assign_colors(cultures)

    out_cultures = [
        {
            "id": c["id"],
            "name_pt": c["name_pt"],
            "name_en": c["name_en"],
            "name_es": c["name_es"],
            "family": c["family"],
            "language": c["language"],
            "religion": c["religion"],
            "color": colors[c["id"]],
        }
        for c in cultures
    ]

    city_rows = []
    regional = 0
    for city in cities:
        population = int(float(city["population"] or 0))
        capital = city["capital"] == "1"
        if city["id"] in overrides:
            entries = overrides[city["id"]]
            total = sum(share for _, share, _ in entries)
            if total > 100.05:
                raise SystemExit(f"city_overrides:{city['id']} soma {total:g} > 100")
            entries = list(entries)
            if total < 99.95:
                entries.append((OTHER, 100.0 - total, entries[0][2]))
            rounded = round_shares([(cid, share) for cid, share, _ in entries], city["id"])
            sources = []
            for _, _, source in entries:
                if source not in sources:
                    sources.append(source)
            source = " + ".join(sources)
            regional += 1
        else:
            rounded = round_shares(
                city_mix(nation_mix[city["nation"]], population, capital), city["id"]
            )
            source = "national_fallback"
        mixture = ";".join(f"{cid}:{share}" for cid, share in rounded)
        city_rows.append(
            {
                "city_id": city["id"],
                "nation": city["nation"],
                "city": city["name"],
                "cultures": mixture,
                "source": source,
            }
        )

    total = len(city_rows)
    print(f"culturas: {len(cultures)}")
    print(f"nações: {len(nations)}")
    print(f"cidades: {total} (regional {regional}, fallback {total - regional})")

    if args.check:
        print("--check: nada gravado")
        return 0

    out_dir = ROOT / OUT_DIR
    out_dir.mkdir(parents=True, exist_ok=True)
    write_csv(out_dir / "cultures.csv", CULTURE_FIELDS, out_cultures)
    with (out_dir / "cultures.json").open("w", encoding="utf-8", newline="\n") as fh:
        json.dump(out_cultures, fh, ensure_ascii=False, indent=2)
        fh.write("\n")
    write_csv(out_dir / "nation_cultures.csv", NATION_FIELDS, nations)
    write_csv(out_dir / "city_cultures.csv", CITY_FIELDS, city_rows)
    print(f"gravado em {OUT_DIR.as_posix()}")
    return 0


def write_csv(path: Path, fields: list[str], rows: list[dict]) -> None:
    with path.open("w", encoding="utf-8", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=fields, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


if __name__ == "__main__":
    sys.exit(main())
