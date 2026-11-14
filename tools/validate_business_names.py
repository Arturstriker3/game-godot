# -*- coding: utf-8 -*-
"""Valida a base de nomes de empresas por língua (Fase F8 — serviços).

Falha (código 1) se:
  - alguma cultura de `cultures.json` ficar sem língua no mapa;
  - o mapa apontar para língua sem arquivo (ou arquivo órfão sem uso);
  - um arquivo não tiver os 20 setores, ou algum setor com menos de 10 tipos;
  - faltarem qualificadores (<40), sobrenomes (<60) ou sufixos jurídicos (3..5);
  - houver padrões repetidos, fora de 5..8, sem placeholder, com placeholder
    inválido, todos com `{place}` ou nenhum com `{place}`;
  - houver termo repetido dentro de uma lista;
  - a grafia nativa não tiver a mesma contagem do romanizado;
  - uma cultura cuja língua tem arquivo próprio divergir dele sem estar
    documentada em `nearest` (aproximação exige motivo escrito).

Uso:
  & "C:\\Users\\artur\\AppData\\Local\\Python\\bin\\python.exe" tools/validate_business_names.py
"""

from __future__ import annotations

import glob
import io
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CULTURE_DIR = os.path.join(ROOT, "data", "definitions", "culture")
NAME_DIR = os.path.join(CULTURE_DIR, "business_names")
CULTURES_PATH = os.path.join(CULTURE_DIR, "cultures.json")
MAP_PATH = os.path.join(CULTURE_DIR, "culture_language.json")

SECTORS = [
    "retail", "wholesale", "food_service", "hospitality_tourism",
    "freight_logistics", "passenger_transport", "finance",
    "insurance_pension", "health", "education", "telecom", "it",
    "media_advertising", "professional_services", "real_estate",
    "construction", "maintenance_repair", "personal_services",
    "leisure_culture", "cleaning_security_waste",
]
ALLOWED = {"type", "qualifier", "surname", "sector_word", "place", "legal"}
PLACEHOLDER_RE = re.compile(r"\{(\w+)\}")

# Nomes de língua (campo `language` de cultures.json) que têm arquivo próprio.
EXACT = {
    "português": "pt", "espanhol": "es", "inglês": "en", "francês": "fr",
    "francês (quebequense)": "fr", "alemão": "de", "italiano": "it",
    "neerlandês": "nl", "neerlandês (flamengo)": "nl", "russo": "ru",
    "ucraniano": "uk", "polonês": "pl", "romeno": "ro", "turco": "tr",
    "persa": "fa", "hebraico": "he", "hindi-urdu": "hi", "bengali": "bn",
    "urdu": "ur", "mandarim": "zh", "japonês": "ja", "coreano": "ko",
    "vietnamita": "vi", "tailandês": "th", "malaio": "ms", "tagalo": "tl",
    "suaíli": "sw", "hauçá": "ha", "grego": "el", "sueco": "sv",
    "árabe peninsular": "ar", "árabe magrebino": "ar", "árabe egípcio": "ar",
    "árabe levantino": "ar", "árabe iraquiano": "ar", "árabe sudanês": "ar",
    "árabe chadiano": "ar",
}


def read_json(path: str):
    with io.open(path, encoding="utf-8-sig") as handle:
        return json.load(handle)


def check_language(path: str, errors: list[str], warnings: list[str]) -> str:
    code = os.path.splitext(os.path.basename(path))[0]
    data = read_json(path)

    def fail(message: str) -> None:
        errors.append(f"{code}: {message}")

    if data.get("code") != code:
        fail(f"campo code {data.get('code')!r} != nome do arquivo")
    if not data.get("language"):
        fail("campo language vazio")
    if data.get("script") not in {
        "latin", "cyrillic", "greek", "arabic", "hebrew", "devanagari",
        "bengali", "han", "japanese", "hangul", "thai", "ethiopic",
    }:
        fail(f"script inválido: {data.get('script')!r}")

    sector_words = data.get("sector_words", {})
    types = data.get("types", {})
    if set(sector_words) != set(SECTORS):
        missing = set(SECTORS) - set(sector_words)
        extra = set(sector_words) - set(SECTORS)
        fail(f"sector_words fora do contrato (faltam {sorted(missing)}, sobram {sorted(extra)})")
    if set(types) != set(SECTORS):
        missing = set(SECTORS) - set(types)
        extra = set(types) - set(SECTORS)
        fail(f"types fora do contrato (faltam {sorted(missing)}, sobram {sorted(extra)})")

    for sector in SECTORS:
        word = sector_words.get(sector, "")
        if not isinstance(word, str) or not word.strip():
            fail(f"{sector}.sector_word vazio")
        items = types.get(sector, [])
        if len(items) != 10:
            fail(f"{sector}: {len(items)} tipos (esperado 10)")
        lowered = [str(item).lower() for item in items]
        if len(set(lowered)) != len(lowered):
            fail(f"{sector}: tipo repetido")

    for field, minimum, maximum in (
        ("qualifiers", 40, 40),
        ("surnames", 60, 60),
        ("legal_suffixes", 3, 5),
    ):
        items = data.get(field, [])
        if not (minimum <= len(items) <= maximum):
            fail(f"{field}: {len(items)} itens (esperado {minimum}..{maximum})")
        lowered = [str(item).lower() for item in items]
        if len(set(lowered)) != len(lowered):
            fail(f"{field}: termo repetido")
        for item in items:
            if not isinstance(item, str) or not item.strip():
                fail(f"{field}: termo vazio")

    patterns = data.get("patterns", [])
    if not 5 <= len(patterns) <= 8:
        fail(f"patterns: {len(patterns)} (esperado 5..8)")
    if len(set(patterns)) != len(patterns):
        fail("patterns: padrão repetido")
    used_placeholders: set[str] = set()
    for pattern in patterns:
        if not isinstance(pattern, str) or not pattern.strip():
            fail(f"pattern vazio: {pattern!r}")
            continue
        found = set(PLACEHOLDER_RE.findall(pattern))
        if not found:
            fail(f"pattern sem placeholder: {pattern!r}")
        invalid = found - ALLOWED
        if invalid:
            fail(f"placeholder inválido {sorted(invalid)} em {pattern!r}")
        if len(pattern.split()) > 6:
            fail(f"pattern longo demais ({len(pattern.split())} tokens): {pattern!r}")
        used_placeholders |= found
    if not any("{place}" in str(p) for p in patterns):
        fail("nenhum padrão usa {place}")
    if not any("{place}" not in str(p) for p in patterns):
        fail("todos os padrões exigem {place}")
    missing_slots = {"type", "surname", "sector_word", "place"} - used_placeholders
    if missing_slots:
        fail(f"padrões não usam {sorted(missing_slots)}")

    native_script = bool(data.get("native_script"))
    native = data.get("native", {})
    if native_script:
        if not native:
            fail("native_script=true sem bloco native")
        else:
            if set(native.get("sector_words", {})) != set(SECTORS):
                fail("native.sector_words fora do contrato")
            if set(native.get("types", {})) != set(SECTORS):
                fail("native.types fora do contrato")
            for sector in SECTORS:
                if len(native.get("types", {}).get(sector, [])) != 10:
                    fail(f"native.{sector}: contagem != 10")
            for field in ("qualifiers", "surnames", "legal_suffixes"):
                if len(native.get(field, [])) != len(data.get(field, [])):
                    fail(f"native.{field}: contagem != romanizado")
    else:
        if native:
            fail("native preenchido com native_script=false")
        if data.get("script", "latin") != "latin":
            warnings.append(f"{code}: script {data.get('script')} sem grafia nativa")

    return code


def main() -> int:
    errors: list[str] = []
    warnings: list[str] = []

    cultures = read_json(CULTURES_PATH)
    culture_by_id = {row["id"]: row for row in cultures}
    mapping = read_json(MAP_PATH)

    default = mapping.get("default", "")
    codes = {os.path.splitext(os.path.basename(p))[0]
             for p in glob.glob(os.path.join(NAME_DIR, "*.json"))}
    if default not in codes:
        errors.append(f"mapa: default {default!r} sem arquivo de língua")

    mapped = mapping.get("cultures", {})
    nearest = mapping.get("nearest", {})

    missing = sorted(set(culture_by_id) - set(mapped))
    extra = sorted(set(mapped) - set(culture_by_id))
    if missing:
        errors.append(f"mapa: culturas sem língua: {missing}")
    if extra:
        errors.append(f"mapa: culturas inexistentes: {extra}")
    for culture_id, code in mapped.items():
        if code not in codes:
            errors.append(f"mapa: {culture_id} → {code!r} sem arquivo")

    for culture_id, reason in nearest.items():
        if culture_id not in mapped:
            errors.append(f"mapa: nearest de cultura não mapeada: {culture_id}")
        if not isinstance(reason, str) or not reason.strip():
            errors.append(f"mapa: nearest sem motivo escrito: {culture_id}")

    for culture_id, row in culture_by_id.items():
        exact = EXACT.get(row.get("language", ""))
        if culture_id in nearest:
            continue
        if exact is None:
            errors.append(
                f"mapa: {culture_id} ({row.get('language')!r}) sem arquivo próprio "
                f"e sem nota em nearest")
        elif mapped.get(culture_id) != exact:
            errors.append(
                f"mapa: {culture_id} deveria ser {exact} "
                f"(fala {row.get('language')!r}), está {mapped.get(culture_id)!r}")

    used_codes = set(mapped.values())
    unused = sorted(codes - used_codes)
    if unused:
        warnings.append(f"línguas sem cultura: {unused}")

    for path in sorted(glob.glob(os.path.join(NAME_DIR, "*.json"))):
        check_language(path, errors, warnings)

    if errors:
        print(f"FALHA: {len(errors)} erro(s) em {len(codes)} línguas / "
              f"{len(mapped)} culturas.")
        for message in errors[:60]:
            print(" - " + message)
        if len(errors) > 60:
            print(f" ... e mais {len(errors) - 60}")
        return 1

    print(f"OK: {len(codes)} línguas, {len(cultures)} culturas mapeadas "
          f"({len(nearest)} aproximações documentadas), 20 setores × 10 tipos.")
    for message in warnings:
        print(" aviso: " + message)
    return 0


if __name__ == "__main__":
    sys.exit(main())
