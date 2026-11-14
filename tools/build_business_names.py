# -*- coding: utf-8 -*-
"""Gera a base de nomes de empresas por língua (Fase F8 — serviços).

Entrada: `tools/business_names_data/*.py` (listas curadas por língua).
Saída:
  - `data/definitions/culture/business_names/<code>.json` (1 por língua);
  - `data/definitions/culture/culture_language.json` (cultura → língua).

Formato de entrada: cada módulo expõe `LANGS = {code: {...}}`. Em línguas de
escrita não latina, cada termo vem como "Romanizado|Nativo"; o gerador separa
a grafia latina (usada pelo jogo) da nativa (guardada no campo `native`).

Uso:
  & "C:\\Users\\artur\\AppData\\Local\\Python\\bin\\python.exe" tools/build_business_names.py

O script é determinístico: rodar de novo com os mesmos dados não muda a saída.
"""

from __future__ import annotations

import importlib.util
import io
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA_DIR = os.path.join(ROOT, "tools", "business_names_data")
OUT_DIR = os.path.join(ROOT, "data", "definitions", "culture", "business_names")
MAP_PATH = os.path.join(ROOT, "data", "definitions", "culture", "culture_language.json")

SECTORS = [
    "retail",
    "wholesale",
    "food_service",
    "hospitality_tourism",
    "freight_logistics",
    "passenger_transport",
    "finance",
    "insurance_pension",
    "health",
    "education",
    "telecom",
    "it",
    "media_advertising",
    "professional_services",
    "real_estate",
    "construction",
    "maintenance_repair",
    "personal_services",
    "leisure_culture",
    "cleaning_security_waste",
]

SCRIPTS = {
    "latin", "cyrillic", "greek", "arabic", "hebrew",
    "devanagari", "bengali", "han", "japanese", "hangul", "thai", "ethiopic",
}

# Cultura → (língua, motivo). None em `motivo` = a cultura fala a própria
# língua do arquivo; texto = aproximação documentada (sem arquivo da língua).
CULTURE_LANGUAGE = {
    "afegao": ("fa", "pashto sem arquivo; persa é a língua culta mais próxima"),
    "angolano": ("pt", "umbundu sem arquivo; Angola é lusófona"),
    "albanes": ("it", "albanês sem arquivo; tradição comercial latina mais próxima"),
    "emiratense": ("ar", None),
    "argentino": ("es", None),
    "armenio": ("ru", "armênio sem arquivo; esfera comercial russo-soviética"),
    "antiguano": ("en", "crioulo inglês sem arquivo; inglês é a língua oficial"),
    "australiano": ("en", None),
    "austriaco": ("de", None),
    "azerbaijano": ("tr", "azeri sem arquivo; turco é a língua turcomana mais próxima"),
    "burundines": ("sw", "kirundi sem arquivo; suaíli é a língua franca bantu oriental"),
    "belga": ("nl", None),
    "beninense": ("yo", "fon sem arquivo; iorubá é a língua comercial vizinha"),
    "burquinense": ("fr", "mooré sem arquivo; Burkina Faso é francófona"),
    "bengali": ("bn", None),
    "bulgaro": ("ru", "búlgaro sem arquivo; eslavo meridional em cirílico"),
    "bareinita": ("ar", None),
    "bahamense": ("en", "crioulo inglês sem arquivo; inglês é a língua oficial"),
    "bosnio": ("pl", "bósnio sem arquivo; eslavo meridional em alfabeto latino"),
    "bielorrusso": ("ru", "bielorrusso sem arquivo; eslavo oriental em cirílico"),
    "belizense": ("es", "espanhol/crioulo inglês; o componente hispânico é majoritário"),
    "boliviano": ("es", None),
    "brasileiro": ("pt", None),
    "barbadense": ("en", "crioulo inglês sem arquivo; inglês é a língua oficial"),
    "bruneano": ("ms", None),
    "butanes": ("zh", "tibetano sem arquivo; chinês é a língua estatal"),
    "botsuanense": ("en", "tsuana sem arquivo; inglês é a língua oficial"),
    "centro_africano": ("fr", "sango sem arquivo; República Centro-Africana é francófona"),
    "canadense": ("en", None),
    "suico": ("de", None),
    "chileno": ("es", None),
    "chines": ("zh", None),
    "marfinense": ("fr", "akan sem arquivo; Costa do Marfim é francófona"),
    "camaronense": ("fr", "fang sem arquivo; Camarões é francófono"),
    "congoles_rdc": ("fr", "lingala sem arquivo; RDC é francófona"),
    "congoles": ("fr", "kikongo sem arquivo; Congo é francófono"),
    "colombiano": ("es", None),
    "costa_riquenho": ("es", None),
    "cubano": ("es", None),
    "cipriota": ("el", None),
    "tcheco": ("pl", "tcheco sem arquivo; eslavo ocidental em alfabeto latino"),
    "alemao": ("de", None),
    "djibutiense": ("ar", "somali sem arquivo; árabe é língua oficial do Djibuti"),
    "dominiquense": ("en", "crioulo inglês sem arquivo; inglês é a língua oficial"),
    "dinamarques": ("sv", "dinamarquês sem arquivo; nórdico continental mais próximo"),
    "dominicano": ("es", None),
    "argelino": ("ar", None),
    "equatoriano": ("es", None),
    "egipcio": ("ar", None),
    "eritreu": ("am", "tigrínio sem arquivo; amárico compartilha escrita e região"),
    "espanhol": ("es", None),
    "estoniano": ("sv", "estoniano sem arquivo; comércio nórdico mais próximo"),
    "etiope": ("am", "oromo sem arquivo; amárico é a língua de Estado"),
    "finlandes": ("sv", "finlandês sem arquivo; comércio nórdico mais próximo"),
    "fijiano": ("en", "fijiano sem arquivo; inglês é a língua oficial"),
    "frances": ("fr", None),
    "gabonense": ("fr", "fang sem arquivo; Gabão é francófono"),
    "britanico": ("en", None),
    "georgiano": ("ru", "georgiano sem arquivo; esfera comercial russo-soviética"),
    "ganense": ("en", "akan sem arquivo; inglês é a língua oficial"),
    "guineense": ("fr", "fula sem arquivo; Guiné é francófona"),
    "gambiano": ("en", "wolof sem arquivo; inglês é a língua oficial"),
    "bissauguineense": ("pt", "crioulo sem arquivo; Guiné-Bissau é lusófona"),
    "equatoguineense": ("es", "fang sem arquivo; Guiné Equatorial é hispanófona"),
    "grego": ("el", None),
    "granadino": ("en", "crioulo inglês sem arquivo; inglês é a língua oficial"),
    "guatemalteco": ("es", None),
    "guianense": ("en", "crioulo inglês sem arquivo; inglês é a língua oficial"),
    "hondurenho": ("es", None),
    "croata": ("pl", "croata sem arquivo; eslavo meridional em alfabeto latino"),
    "haitiano": ("fr", "crioulo haitiano sem arquivo; francês é a língua de Estado"),
    "hungaro": ("pl", "húngaro sem arquivo; Europa central em alfabeto latino"),
    "indonesio": ("id", "javanês sem arquivo; indonésio é a língua nacional"),
    "indiano": ("hi", None),
    "irlandes": ("en", "irlandês sem arquivo; inglês é a língua comercial"),
    "iraniano": ("fa", None),
    "iraquiano": ("ar", None),
    "islandes": ("sv", "islandês sem arquivo; nórdico ocidental mais próximo"),
    "israelense": ("he", None),
    "italiano": ("it", None),
    "jamaicano": ("en", "crioulo inglês sem arquivo; inglês é a língua oficial"),
    "jordaniano": ("ar", None),
    "japones": ("ja", None),
    "cazaque": ("tr", "cazaque sem arquivo; língua turcomana mais próxima"),
    "queniano": ("sw", "kikuyu sem arquivo; suaíli é a língua franca regional"),
    "quirguiz": ("tr", "quirguiz sem arquivo; língua turcomana mais próxima"),
    "cambojano": ("th", "khmer sem arquivo; tailandês é o vizinho comercial"),
    "sancristovense": ("en", "crioulo inglês sem arquivo; inglês é a língua oficial"),
    "sul_coreano": ("ko", None),
    "kuwaitiano": ("ar", None),
    "laosiano": ("th", "lao sem arquivo; tailandês é a língua tai-kadai mais próxima"),
    "libanes": ("ar", None),
    "liberiano": ("en", "kpelle sem arquivo; inglês é a língua oficial"),
    "libio": ("ar", None),
    "santa_luciense": ("en", "crioulo inglês sem arquivo; inglês é a língua oficial"),
    "srilankes": ("hi", "cingalês sem arquivo; indo-ariano mais próximo"),
    "lesotense": ("en", "soto sem arquivo; inglês é a língua oficial"),
    "lituano": ("pl", "lituano sem arquivo; eslavo/báltico em alfabeto latino"),
    "luxemburgues": ("de", "luxemburguês sem arquivo; alemão é a língua de comércio"),
    "leto": ("pl", "letão sem arquivo; báltico em alfabeto latino"),
    "marroquino": ("ar", None),
    "moldavio": ("ro", None),
    "malgaxe": ("fr", "malgaxe sem arquivo; Madagascar é francófono"),
    "mexicano": ("es", None),
    "macedonio": ("ru", "macedônio sem arquivo; eslavo meridional em cirílico"),
    "malines": ("fr", "bambara sem arquivo; Mali é francófono"),
    "maltes": ("it", "maltês sem arquivo; italiano é a língua comercial histórica"),
    "birmanes": ("th", "birmanês sem arquivo; tailandês é o vizinho comercial"),
    "montenegrino": ("pl", "montenegrino sem arquivo; eslavo meridional em latino"),
    "mongol": ("ru", "mongol sem arquivo; escrita e comércio russos"),
    "mocambicano": ("pt", "macua sem arquivo; Moçambique é lusófono"),
    "mauritano": ("ar", None),
    "malauiano": ("en", "chewa sem arquivo; inglês é a língua oficial"),
    "malasio": ("ms", None),
    "namibiano": ("en", "oshiwambo sem arquivo; inglês é a língua oficial"),
    "neerlandes": ("nl", None),
    "nigerino": ("ha", None),
    "nigeriano": ("ha", None),
    "nicaraguense": ("es", None),
    "noruegues": ("sv", "norueguês sem arquivo; nórdico continental mais próximo"),
    "nepales": ("hi", "nepali sem arquivo; indo-ariano mais próximo"),
    "neozelandes": ("en", None),
    "omanense": ("ar", None),
    "paquistanes": ("ur", None),
    "panamenho": ("es", None),
    "peruano": ("es", None),
    "filipino": ("tl", None),
    "papuano": ("en", "línguas papuas sem arquivo; inglês é a língua oficial"),
    "polones": ("pl", None),
    "norte_coreano": ("ko", None),
    "portugues": ("pt", None),
    "paraguaio": ("es", None),
    "palestino": ("ar", None),
    "catariano": ("ar", None),
    "romeno": ("ro", None),
    "russo": ("ru", None),
    "ruandes": ("sw", "kinyarwanda sem arquivo; suaíli é a língua franca bantu"),
    "saaraui": ("ar", None),
    "saudita": ("ar", None),
    "sudanes": ("ar", None),
    "senegales": ("fr", "wolof sem arquivo; Senegal é francófono"),
    "singapurense": ("zh", None),
    "salomonico": ("en", "pidgin melanésio sem arquivo; inglês é a língua oficial"),
    "serra_leones": ("en", "kpelle sem arquivo; inglês é a língua oficial"),
    "salvadorenho": ("es", None),
    "somalilandes": ("ar", "somali sem arquivo; árabe é língua oficial"),
    "somali": ("ar", "somali sem arquivo; árabe é língua oficial"),
    "servio": ("ru", "sérvio sem arquivo; eslavo meridional em cirílico"),
    "sul_sudanes": ("en", "dinka sem arquivo; inglês é a língua oficial"),
    "surinames": ("nl", "hindi-urdu de comunidade; neerlandês é a língua oficial"),
    "eslovaco": ("pl", "eslovaco sem arquivo; eslavo ocidental em alfabeto latino"),
    "esloveno": ("pl", "esloveno sem arquivo; eslavo meridional em alfabeto latino"),
    "sueco": ("sv", None),
    "suaziliense": ("en", "suázi sem arquivo; inglês é a língua oficial"),
    "sirio": ("ar", None),
    "chadiano": ("ar", None),
    "togoles": ("fr", "ewé sem arquivo; Togo é francófono"),
    "tailandes": ("th", None),
    "tadjique": ("fa", "tadjique sem arquivo; persa é a língua iraniana mais próxima"),
    "turcomeno": ("tr", "turcomeno sem arquivo; língua turcomana mais próxima"),
    "timorense": ("pt", "tétum sem arquivo; Timor-Leste é lusófono"),
    "trinitario": ("en", "crioulo inglês sem arquivo; inglês é a língua oficial"),
    "tunisiano": ("ar", None),
    "turco": ("tr", None),
    "taiwanes": ("zh", None),
    "tanzaniano": ("sw", None),
    "ugandense": ("sw", "luganda sem arquivo; suaíli é a língua franca regional"),
    "ucraniano": ("uk", None),
    "uruguaio": ("es", None),
    "americano": ("en", None),
    "uzbeque": ("tr", "uzbeque sem arquivo; língua turcomana mais próxima"),
    "sao_vicentino": ("en", "crioulo inglês sem arquivo; inglês é a língua oficial"),
    "venezuelano": ("es", None),
    "vietnamita": ("vi", None),
    "kosovar": ("it", "albanês sem arquivo; tradição comercial latina mais próxima"),
    "iemenita": ("ar", None),
    "sul_africano": ("en", "zulu sem arquivo; inglês é a língua oficial"),
    "zambiano": ("en", "bemba sem arquivo; inglês é a língua oficial"),
    "zimbabuense": ("en", "shona sem arquivo; inglês é a língua oficial"),
    "curdo": ("tr", "curdo sem arquivo; turco é a língua estatal mais próxima"),
    "catalao": ("es", "catalão sem arquivo; espanhol é a língua estatal"),
    "basco": ("es", "basco sem arquivo; espanhol é a língua estatal"),
    "quebequense": ("fr", None),
    "uigur": ("zh", "uigur sem arquivo; chinês é a língua estatal"),
    "tibetano": ("zh", "tibetano sem arquivo; chinês é a língua estatal"),
    "tamil": ("hi", "tâmil sem arquivo; indo-ariano é a língua indiana de referência"),
    "tartaro_crimeia": ("tr", "tártaro da Crimeia sem arquivo; turco é o mais próximo"),
    "berbere": ("ar", "tamazigue sem arquivo; árabe é a língua de comércio do Magrebe"),
    "tuaregue": ("ar", "tamasheq sem arquivo; árabe é a língua de comércio do Saara"),
    "checheno": ("ru", "checheno sem arquivo; esfera comercial russa"),
    "tatar": ("tr", "tártaro sem arquivo; língua turcomana mais próxima"),
    "afar": ("am", "afar sem arquivo; amárico é a língua de Estado etíope"),
    "balochi": ("ur", "balúchi sem arquivo; urdu é a língua comercial do Paquistão"),
    "papua": ("id", "línguas papuas sem arquivo; indonésio é a língua provincial"),
    "maori": ("en", "maori sem arquivo; inglês é a língua comercial"),
    "aborigene": ("en", "línguas aborígenes sem arquivo; inglês é a língua comercial"),
    "havaiano": ("en", "havaiano sem arquivo; inglês é a língua comercial"),
    "inuit": ("en", "inuktitut sem arquivo; inglês é a língua comercial"),
    "nativo_americano": ("en", "diversas línguas indígenas; inglês é a língua comercial"),
    "indigena_brasileiro": ("pt", "línguas indígenas brasileiras; português é a língua comercial"),
    "quechua": ("es", "quíchua sem arquivo; espanhol é a língua comercial"),
    "aimara": ("es", "aimará sem arquivo; espanhol é a língua comercial"),
    "guarani": ("es", "guarani sem arquivo; espanhol é a língua comercial"),
    "maia": ("es", "línguas maias sem arquivo; espanhol é a língua comercial"),
    "mapuche": ("es", "mapudungun sem arquivo; espanhol é a língua comercial"),
    "nahua": ("es", "náuatle sem arquivo; espanhol é a língua comercial"),
}


def load_module(path: str):
    spec = importlib.util.spec_from_file_location("business_names_data", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def split_term(term: str) -> tuple[str, str]:
    if "|" in term:
        roman, native = term.split("|", 1)
        return roman.strip(), native.strip()
    return term.strip(), term.strip()


def build_language(code: str, raw: dict) -> dict:
    sector_words = raw["sector_words"]
    types = raw["types"]
    qualifiers = raw["qualifiers"]
    surnames = raw["surnames"]
    legal = raw["legal_suffixes"]
    patterns = raw["patterns"]
    script = raw.get("script", "latin")
    if script not in SCRIPTS:
        raise SystemExit(f"{code}: script desconhecido {script!r}")

    native: dict[str, object] = {
        "sector_words": {},
        "types": {},
        "qualifiers": [],
        "surnames": [],
        "legal_suffixes": [],
    }
    has_native = False

    def take(term: str, where: str):
        nonlocal has_native
        roman, nat = split_term(term)
        if not roman:
            raise SystemExit(f"{code}: termo vazio em {where}")
        if nat != roman:
            has_native = True
        return roman, nat

    out_words = {}
    for sector in SECTORS:
        roman, nat = take(sector_words[sector], f"{sector}.sector_word")
        out_words[sector] = roman
        native["sector_words"][sector] = nat

    out_types = {}
    for sector in SECTORS:
        items = types[sector]
        if len(items) != 10:
            raise SystemExit(f"{code}.{sector}: {len(items)} tipos (esperado 10)")
        romans = []
        nats = []
        for item in items:
            roman, nat = take(item, f"{sector}.type")
            romans.append(roman)
            nats.append(nat)
        if len(set(x.lower() for x in romans)) != len(romans):
            raise SystemExit(f"{code}.{sector}: tipo repetido")
        out_types[sector] = romans
        native["types"][sector] = nats

    for field in ("qualifiers", "surnames", "legal_suffixes"):
        src = {"qualifiers": qualifiers, "surnames": surnames,
               "legal_suffixes": legal}[field]
        romans = []
        nats = []
        for item in src:
            roman, nat = take(item, field)
            romans.append(roman)
            nats.append(nat)
        if len(set(x.lower() for x in romans)) != len(romans):
            raise SystemExit(f"{code}.{field}: termo repetido")
        if field == "qualifiers" and len(romans) != 40:
            raise SystemExit(f"{code}.qualifiers: {len(romans)} (esperado 40)")
        if field == "surnames" and len(romans) != 60:
            raise SystemExit(f"{code}.surnames: {len(romans)} (esperado 60)")
        if field == "legal_suffixes" and not 3 <= len(romans) <= 5:
            raise SystemExit(f"{code}.legal_suffixes: {len(romans)} (esperado 3..5)")
        native[field] = nats
        if field == "qualifiers":
            out_qualifiers = romans
        elif field == "surnames":
            out_surnames = romans
        else:
            out_legal = romans

    if not 5 <= len(patterns) <= 8:
        raise SystemExit(f"{code}.patterns: {len(patterns)} (esperado 5..8)")
    allowed = {"type", "qualifier", "surname", "sector_word", "place", "legal"}
    for pattern in patterns:
        if not isinstance(pattern, str) or not pattern.strip():
            raise SystemExit(f"{code}: padrão inválido {pattern!r}")
        found = set(re.findall(r"\{(\w+)\}", pattern))
        if not found:
            raise SystemExit(f"{code}: padrão sem placeholder {pattern!r}")
        if not found <= allowed:
            raise SystemExit(f"{code}: placeholder inválido em {pattern!r}: {found - allowed}")
    if not any("{place}" in p for p in patterns):
        raise SystemExit(f"{code}: nenhum padrão com {{place}}")
    if not any("{place}" not in p for p in patterns):
        raise SystemExit(f"{code}: todos os padrões exigem {{place}}")

    entry = {
        "code": code,
        "language": raw["language"],
        "script": script,
        "native_script": has_native,
        "sector_words": out_words,
        "types": out_types,
        "qualifiers": out_qualifiers,
        "surnames": out_surnames,
        "legal_suffixes": out_legal,
        "patterns": patterns,
    }
    if has_native:
        entry["native"] = native
    return entry


def main() -> int:
    modules = sorted(
        name for name in os.listdir(DATA_DIR)
        if name.endswith(".py") and not name.startswith("_")
    )
    langs: dict[str, dict] = {}
    for name in modules:
        module = load_module(os.path.join(DATA_DIR, name))
        for code, raw in module.LANGS.items():
            if code in langs:
                raise SystemExit(f"língua duplicada em módulos: {code}")
            langs[code] = raw

    os.makedirs(OUT_DIR, exist_ok=True)
    built = {}
    for code in sorted(langs):
        entry = build_language(code, langs[code])
        built[code] = entry
        path = os.path.join(OUT_DIR, f"{code}.json")
        with io.open(path, "w", encoding="utf-8", newline="\n") as handle:
            json.dump(entry, handle, ensure_ascii=False, indent=2)
            handle.write("\n")

    with io.open(os.path.join(ROOT, "data", "definitions", "culture", "cultures.json"),
                 encoding="utf-8-sig") as handle:
        cultures = json.load(handle)
    culture_ids = [row["id"] for row in cultures]
    missing = [cid for cid in culture_ids if cid not in CULTURE_LANGUAGE]
    extra = [cid for cid in CULTURE_LANGUAGE if cid not in culture_ids]
    if missing:
        raise SystemExit(f"culturas sem mapeamento: {missing}")
    if extra:
        raise SystemExit(f"mapeamento para culturas inexistentes: {extra}")

    mapping = {}
    nearest = {}
    for cid, (code, reason) in CULTURE_LANGUAGE.items():
        if code not in built:
            raise SystemExit(f"{cid}: língua {code!r} sem arquivo")
        mapping[cid] = code
        if reason:
            nearest[cid] = reason

    document = {
        "version": 1,
        "default": "en",
        "note": ("Toda cultura de cultures.json tem uma língua. Culturas cuja língua "
                 "não tem arquivo próprio usam a mais próxima, listada em `nearest` "
                 "com o motivo."),
        "cultures": dict(sorted(mapping.items())),
        "nearest": dict(sorted(nearest.items())),
    }
    with io.open(MAP_PATH, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(document, handle, ensure_ascii=False, indent=2)
        handle.write("\n")

    print(f"OK: {len(built)} línguas, {len(mapping)} culturas, "
          f"{len(nearest)} aproximações documentadas.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
