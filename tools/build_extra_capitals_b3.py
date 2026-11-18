"""B3 opcao 3 - capitais extras para estados grandes sem cidade.

Gera `data/definitions/scenario/extra_capitals_b3.json` e aplica o patch em
`data/definitions/population/state_regions.json` (estado -> capital propria).

Regra (decisao do dono, B3 opcao 3): cada estado admin-1 com populacao 2026
acima de 20 milhoes que hoje e anexado a um vizinho
(`attach_rule=neighbor_largest_population`) ganha a sua capital administrativa
real como cidade propria. A capital passa a ser dona apenas do seu estado; os
demais estados continuam como estao (nenhum BFS e refeito).

Nenhum numero e inventado. Por cidade:

  * `sim_population` = cidade propria, GeoNames `cities15000` (dump 2026,
    https://download.geonames.org/export/dump/cities15000.zip, geonameid
    registrado no campo `provenance`);
  * `agglomeration_population` / `urban_population` = aglomeracao 2026-01-01 da
    tabela versionada `tools/agglomerations_2026.csv` (Thomas Brinkhoff,
    citypopulation.de) — a MESMA fonte/regra de `tools/build_world_cities.py`
    (`assign_urban_population`). Sem linha na tabela -> `sim_population`
    (`urban_population_source=sim_population`), como nas 454 cidades atuais;
  * `population` = rateio da populacao nacional por peso `urban_population`
    (mesma funcao de `tools/build_world_cities.py`), recalculado com o elenco
    novo. E valor DERIVADO: o arquivo guarda o valor por cidade e o mapa
    completo old->new em `population_rateio_after`;
  * `x`/`y` = `lonlat_to_xy` (mapa 5000x2500) das coordenadas GeoNames;
  * `production` = 0.0 (soma das construcoes; cidade nova nasce sem predio);
  * `capital` = false (nao e capital nacional);
  * `landmass` = "asia" (as 15 ficam na Asia, como as cidades vizinhas).

Uso:
    <python> tools/build_extra_capitals_b3.py --check   # so confere
    <python> tools/build_extra_capitals_b3.py --write   # grava os JSONs
"""

import argparse
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
STATE_REGIONS = os.path.join(ROOT, "data", "definitions", "population",
                             "state_regions.json")
CITIES = os.path.join(ROOT, "data", "definitions", "scenario", "cities.json")
NATIONS = os.path.join(ROOT, "data", "definitions", "scenario", "nations.json")
OUT = os.path.join(ROOT, "data", "definitions", "scenario",
                   "extra_capitals_b3.json")

MAP_W, MAP_H = 5000.0, 2500.0
EXPECTED_TOTAL = 8_203_501_062
AGGLOMERATION_URL = "https://www.citypopulation.de/en/world/agglomerations/"
GEONAMES_URL = "https://download.geonames.org/export/dump/cities15000.zip"
GEONAMES_RETRIEVED = "2026-11-18"

# (id, nome, nation, state_id, nome do estado, capital, geonameid, lat, lon,
#  cidade propria GeoNames, linha da aglomeracao 2026 (nome na tabela, status,
#  valor) ou None, i18n pt, i18n es)
#
# Ordem: populacao do estado decrescente (a mesma do doc B3_region.md §6).
# O id segue `make_city_id` de tools/import_world_sources.py (ISO3 + 3 letras
# do nome, 4/5 em caso de colisao, nesta ordem).
ROWS = [
    ("CHN-ZHE", "Zhengzhou", "CHN", "CHN-1812", "Henan",
     "Zhengzhou", 1784658, 34.75778, 113.64861, 6650532,
     (46, "Zhèngzhōu", "Adm-Urb", 10500000), "Zhengzhou", "Zhengzhou"),
    ("CHN-SHI", "Shijiazhuang", "CHN", "CHN-1811", "Hebei",
     "Shijiazhuang", 1795270, 38.04139, 114.47861, 5758403,
     (86, "Shíjiāzhuāng", "Adm-Urb", 6600000), "Shijiazhuang", "Shijiazhuang"),
    ("CHN-CHA", "Changsha", "CHN", "CHN-1808", "Hunan",
     "Changsha", 1815577, 28.19874, 112.97087, 3093980,
     (42, "Chángshā", "Adm-Urb", 11600000), "Changsha", "Changsha"),
    ("CHN-NANN", "Nanning", "CHN", "CHN-1152", "Guangxi",
     "Nanning", 1799869, 22.81667, 108.31667, 5977185,
     (100, "Nánníng", "Adm-Urb", 5600000), "Nanning", "Nanning"),
    ("IND-BHU", "Bhubaneswar", "IND", "IND-2444", "Odisha",
     "Bhubaneswar", 1275817, 20.27241, 85.83385, 885363,
     (282, "Bhubaneswar", "Congl", 2175000), "Bhubaneswar", "Bhubaneswar"),
    ("CHN-NANC", "Nanchang", "CHN", "CHN-1817", "Jiangxi",
     "Nanchang", 1800163, 28.68396, 115.85306, 2357839,
     (111, "Nánchāng", "Adm-Urb", 5250000), "Nanchang", "Nanchang"),
    ("IND-RAN", "Ranchi", "IND", "IND-3256", "Jharkhand",
     "Ranchi", 1258526, 23.34316, 85.3094, 1120374,
     (376, "Rānchi", "Aggl", 1670000), "Ranchi", "Ranchi"),
    ("CHN-GUI", "Guiyang", "CHN", "CHN-1153", "Guizhou",
     "Guiyang", 1809461, 26.58333, 106.71667, 4506134,
     (106, "Guìyáng", "Adm-Urb", 5350000), "Guiyang", "Guiyang"),
    ("IND-THI", "Thiruvananthapuram", "IND", "IND-2442", "Kerala",
     "Thiruvananthapuram", 1254163, 8.4855, 76.94924, 788271,
     None, "Thiruvananthapuram", "Thiruvananthapuram"),
    ("IND-GUW", "Guwahati", "IND", "IND-2477", "Assam",
     "Dispur", 1271476, 26.1844, 91.7458, 962334,
     (514, "Guwāhāti", "Aggl", 1260000), "Guwahati", "Guwahati"),
    ("CHN-TAI", "Taiyuan", "CHN", "CHN-1805", "Shanxi",
     "Taiyuan", 1793511, 37.86944, 112.56028, 5305061,
     (91, "Tàiyuán", "Adm-Urb", 6300000), "Taiyuan", "Taiyuan"),
    ("IND-CHA", "Chandigarh", "IND", "IND-2430", "Haryana",
     "Chandigarh", 1274746, 30.73629, 76.7884, 970602,
     (210, "Chandīgarh", "Congl", 2850000), "Chandigarh", "Chandigarh"),
    ("IND-RAI", "Raipur", "IND", "IND-3260", "Chhattisgarh",
     "Raipur", 1258980, 21.23333, 81.63333, 1027264,
     None, "Raipur", "Raipur"),
    ("CHN-HOH", "Hohhot", "CHN", "CHN-1838", "Inner Mongol",
     "Hohhot", 2036892, 40.81056, 111.65222, 3446100,
     (205, "Hūhéhàotè", "Adm-Urb", 2900000), "Hohhot", "Hohhot"),
    ("CHN-CHAN", "Changchun", "CHN", "CHN-1828", "Jilin",
     "Changchun", 2038180, 43.88, 125.32278, 5691024,
     (109, "Chángchūn", "Adm-Urb", 5300000), "Changchun", "Changchun"),
]

# Vizinho a quem o estado estava anexado ANTES do patch (D-005). Serve para
# desfazer o patch e tornar o build idempotente (rodar 2x = mesmo byte).
ATTACHED_TO = {
    "CHN-ZHE": "CHN-XIAN",
    "CHN-SHI": "CHN-BEI",
    "CHN-CHA": "CHN-GUA",
    "CHN-NANN": "CHN-GUA",
    "IND-BHU": "IND-KOL",
    "CHN-NANC": "CHN-GUA",
    "IND-RAN": "IND-KOL",
    "CHN-GUI": "CHN-CHE",
    "IND-THI": "IND-BAN",
    "IND-GUW": "IND-KOL",
    "CHN-TAI": "CHN-XIAN",
    "IND-CHA": "IND-DEL",
    "IND-RAI": "IND-MUM",
    "CHN-HOH": "CHN-XIAN",
    "CHN-CHAN": "CHN-SHE",
}

# Nota de fonte por cidade sem aglomeracao na tabela (nao se inventa numero).
NO_AGGLOMERATION_NOTE = {
    "IND-THI": ("Thiruvananthapuram nao esta na tabela de aglomeracoes >=1M de "
                "2026 (a maior de Kerala la e Kozhikode, 1.070.000). Fica a "
                "regra das 454 cidades sem aglomeracao: urban = cidade propria "
                "(GeoNames). Referencia disponivel, nao usada: Census of India "
                "2011, UA Thiruvananthapuram = 1.679.754 (ano/definicao "
                "diferentes)."),
    "IND-RAI": ("A tabela 2026 traz a conurbacao como 'Bhilai (incl. Raipur)', "
                "3.475.000 (rank 169, Congl), cujo polo nominal e Bhilai; o "
                "casamento por nome nao casa Raipur. Fica pela regra: urban = "
                "cidade propria (GeoNames 1.027.264). Se o dono quiser a "
                "conurbacao inteira, e trocar 3 campos desta cidade."),
}


def load(path):
    with open(path, "r", encoding="utf-8") as fh:
        return json.load(fh)


def dump_state_regions(path, data):
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(json.dumps(data, indent=1, sort_keys=True,
                            ensure_ascii=False) + "\n")


def dump_json(path, data):
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(json.dumps(data, indent=2, ensure_ascii=False))


def lonlat_to_xy(lon, lat):
    return (round((lon + 180.0) / 360.0 * MAP_W, 1),
            round((90.0 - lat) / 180.0 * MAP_H, 1))


def rateio(cities, nation_pop):
    """Mesmo contrato de build_world_cities.distribute_national_population.

    weight = urban_population; soma por nacao exata; resto pelas maiores
    fracoes, desempate por id. Devolve {city_id: population}.
    """
    by_nation = {}
    for city in cities:
        by_nation.setdefault(city["nation"], []).append(city)
    out = {}
    for tag, group in by_nation.items():
        total = int(nation_pop.get(tag, 0))
        weights = [max(0, int(city["urban_population"])) for city in group]
        weight_sum = sum(weights)
        if total <= 0:
            for city in group:
                out[city["id"]] = int(city.get("population", 0) or 0)
            continue
        if weight_sum <= 0:
            base, extra = divmod(total, len(group))
            for index, city in enumerate(group):
                out[city["id"]] = base + (1 if index < extra else 0)
            continue
        raw = [total * weight / weight_sum for weight in weights]
        values = [int(value) for value in raw]
        remainder = total - sum(values)
        order = sorted(range(len(group)),
                       key=lambda i: (-(raw[i] - values[i]), group[i]["id"]))
        for index in order[:remainder]:
            values[index] += 1
        for city, value in zip(group, values):
            out[city["id"]] = value
    return out


def unapply(nstates, ncities):
    """Desfaz o patch (se ja aplicado), reconstruindo o estado de D-005.

    Idempotencia: o build sempre parte do estado sem as 15 capitais. Reverte
    estado -> vizinho, devolve a populacao e remove a cidade nova.
    """
    restored = []
    for cid, sid in ((r[0], r[3]) for r in ROWS):
        state = nstates.get(sid)
        if state is None or state["city_id"] != cid:
            continue
        old = ATTACHED_TO[cid]
        state["city_id"] = old
        state["attach_rule"] = "neighbor_largest_population"
        entry = ncities[old]
        entry["population_region"] += state["population_2026"]
        entry["states"] = sorted(set(entry["states"]) | {sid})
        entry["region_population"] = max(
            0, entry["population_region"] - entry["urban_population"])
        ncities.pop(cid, None)
        restored.append(cid)
    return restored


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--write", action="store_true")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()

    regions = load(STATE_REGIONS)
    cities = load(CITIES)
    nations = load(NATIONS)
    states = regions["states"]
    rcities = regions["cities"]
    restored = unapply(states, rcities)
    if restored:
        print("patch ja aplicado: revertido para reaplicar (%s)" % ", ".join(restored))

    problems = []
    nation_pop = {row["id"]: int(row["population"]) for row in nations}

    # 1. O rateio reproduz o estado atual das 834? Se nao, a regra mudou.
    base = rateio(cities, nation_pop)
    drift = [c["id"] for c in cities if int(c["population"]) != base[c["id"]]]
    if drift:
        problems.append("rateio atual nao reproduz cities.json em %d cidades (%s)"
                        % (len(drift), ", ".join(drift[:6])))

    # 2. Os 15 candidatos: estado >20M anexado a vizinho, sem cidade propria.
    expected = set()
    for _cid, _name, _nation, sid, sname, *_rest in ROWS:
        state = states.get(sid)
        if state is None:
            problems.append("estado %s inexistente" % sid)
            continue
        expected.add(sid)
        if state["name"] != sname:
            problems.append("%s: nome na fonte %r != %r" % (sid, state["name"], sname))
        if state["population_2026"] <= 20_000_000:
            problems.append("%s: populacao %d <= 20M" % (sid, state["population_2026"]))
        if state["attach_rule"] == "primary_city":
            problems.append("%s: ja tem cidade propria" % sid)
    found = {sid for sid, state in states.items()
             if state["population_2026"] > 20_000_000
             and state["attach_rule"] != "primary_city"}
    if found != expected:
        problems.append("criterio >20M sem cidade: faltam %s ; sobram %s"
                        % (sorted(expected - found), sorted(found - expected)))

    # 3. Ids unicos e livres.
    used = {c["id"] for c in cities} | set(rcities)
    for cid, name, *_rest in ROWS:
        if cid in used:
            problems.append("id %s ja existe" % cid)
        used.add(cid)

    # 4. Monta as cidades novas.
    new_cities = []
    for (cid, name, nation, sid, sname, admin_name, geonameid, lat, lon,
         proper, agglomeration, i18n_pt, i18n_es) in ROWS:
        state = states[sid]
        x, y = lonlat_to_xy(lon, lat)
        if agglomeration is not None:
            row_rank, table_name, status, value = agglomeration
            agg_value = value
            urban = min(value, nation_pop.get(nation, value))
            source = "agglomeration" if urban == value else "agglomeration_capped"
            agg_source = {
                "table": "tools/agglomerations_2026.csv",
                "row_rank": row_rank,
                "row_name": table_name,
                "status": status,
                "reference_date": "2026-01-01",
                "url": AGGLOMERATION_URL,
            }
        else:
            agg_value = None
            urban = int(proper)
            source = "sim_population"
            agg_source = None
        record = {
            "id": cid,
            "nation": nation,
            "home_nation": nation,
            "name": name,
            "population": 0,  # preenchido abaixo pelo rateio pos-insercao
            "sim_population": float(proper),
            "agglomeration_population": agg_value,
            "urban_population": int(urban),
            "urban_population_source": source,
            "production": 0.0,
            "x": x,
            "y": y,
            "landmass": "asia",
            "capital": False,
            "provenance": {
                "state_id": sid,
                "state_name": sname,
                "state_population_2026": state["population_2026"],
                "attached_before": {
                    "city_id": state["city_id"],
                    "attach_rule": state["attach_rule"],
                },
                "capital_of_state": admin_name,
                "geonameid": geonameid,
                "lat": lat,
                "lon": lon,
                "city_proper": {
                    "value": int(proper),
                    "source": "GeoNames cities15000 (2026, city proper)",
                    "url": GEONAMES_URL,
                    "retrieved": GEONAMES_RETRIEVED,
                },
                "agglomeration": agg_source,
                "note": NO_AGGLOMERATION_NOTE.get(cid, ""),
            },
        }
        new_cities.append(record)

    # 5. Rateio pos-insercao (elenco 834 + 15).
    roster = [{"id": c["id"], "nation": c["nation"],
               "urban_population": c["urban_population"],
               "population": c["population"]} for c in cities]
    roster += [{"id": c["id"], "nation": c["nation"],
                "urban_population": c["urban_population"],
                "population": c["population"]} for c in new_cities]
    after = rateio(roster, nation_pop)
    for record in new_cities:
        record["population"] = float(after[record["id"]])

    # 6. Patch em state_regions (estado -> capital; vizinho perde o estado).
    new_regions = json.loads(json.dumps(regions))
    nstates = new_regions["states"]
    ncities = new_regions["cities"]
    removed = {}
    for record in new_cities:
        prov = record["provenance"]
        sid = prov["state_id"]
        state = nstates[sid]
        old_city = state["city_id"]
        entry = ncities[old_city]
        if sid not in entry["states"]:
            problems.append("%s nao esta na lista de %s" % (sid, old_city))
        state["city_id"] = record["id"]
        state["attach_rule"] = "primary_city"
        entry["states"] = [s for s in entry["states"] if s != sid]
        entry["population_region"] -= state["population_2026"]
        entry["region_population"] = max(
            0, entry["population_region"] - entry["urban_population"])
        ncities[record["id"]] = {
            "population_region": state["population_2026"],
            "region_population": max(0, state["population_2026"] - record["urban_population"]),
            "states": [sid],
            "urban_population": record["urban_population"],
        }
        info = removed.setdefault(old_city, {"removed_states": [], "population_removed": 0})
        info["removed_states"].append(sid)
        info["population_removed"] += state["population_2026"]

    for old_city, info in removed.items():
        entry = ncities[old_city]
        info["population_region_after"] = entry["population_region"]
        info["region_population_after"] = entry["region_population"]
        info["states_after"] = entry["states"]
        info["region_below_urban_after"] = (
            entry["population_region"] < entry["urban_population"])

    # Metadado do patch: o arquivo se explica sozinho e avisa que as 15
    # capitais ainda nao estao em cities.json (nao podem entrar separados).
    new_regions["patch"] = {
        "id": "b3-extra-capitals-1",
        "date": "2026-11-18",
        "generated_by": "tools/build_extra_capitals_b3.py",
        "source": "data/definitions/scenario/extra_capitals_b3.json",
        "states_reassigned": len(new_cities),
        "cities_added": len(new_cities),
        "note": ("15 estados >20M ganharam a capital propria. As 15 cidades "
                 "novas ainda NAO estao em cities.json: enquanto a mesclagem "
                 "nao acontecer (code/data), os estados novos ficam sem quem "
                 "consuma a populacao e os vizinhos ja perderam a deles — os "
                 "dois arquivos tem de entrar juntos."),
    }

    # 7. Invariantes de soma. `population_region` e o total que fecha por nacao;
    # `region_population` tem piso 0 (D-005: 17 cidades com regiao < urbano ja
    # existiam; o piso duplica a sobreposicao, entao urbano+rural NAO fecha o
    # total — invariante declarada e a soma das regioes).
    total_states = sum(s["population_2026"] for s in nstates.values())
    total_regions = sum(c["population_region"] for c in ncities.values())
    total_urban = sum(c["urban_population"] for c in ncities.values())
    total_region_pop = sum(c["region_population"] for c in ncities.values())
    clamped = sorted(cid for cid, c in ncities.items()
                     if c["population_region"] < c["urban_population"])
    if total_states != EXPECTED_TOTAL:
        problems.append("soma dos estados %d != %d" % (total_states, EXPECTED_TOTAL))
    if total_regions != EXPECTED_TOTAL:
        problems.append("soma das regioes %d != %d" % (total_regions, EXPECTED_TOTAL))
    base_clamped = {cid for cid, c in regions["cities"].items()
                    if c["population_region"] < c["urban_population"]}
    new_clamped = sorted(set(clamped) - base_clamped)

    payload = {
        "version": "b3-extra-capitals-1",
        "generated": "2026-11-18",
        "generated_by": "tools/build_extra_capitals_b3.py",
        "what": ("Capital administrativa real (cidade nova) para os 15 estados "
                 "admin-1 acima de 20 milhoes que o B3 anexou a um vizinho por "
                 "attach_rule=neighbor_largest_population. Cada capital passa a "
                 "ser dona APENAS do seu estado; nenhum outro attach muda."),
        "method": (
            "Estado e populacao: data/definitions/population/state_regions.json "
            "(WorldPop 2026 apportionado por provincia; soma 8.203.501.062). "
            "Cidade propria e coordenadas: GeoNames cities15000 2026 (geonameid "
            "em provenance). Aglomeracao e populacao urbana: "
            "tools/agglomerations_2026.csv (Thomas Brinkhoff, citypopulation.de, "
            "ref. 2026-01-01) casada a mao quando o nome da tabela difere "
            "(Guwāhāti=Guwahati); sem linha na tabela vale a regra das 454 "
            "cidades sem aglomeracao (urban = cidade propria). `population` e o "
            "rateio da populacao nacional por peso urban_population, recalculado "
            "com o elenco novo (valor derivado, nao medido). `production` = 0.0 "
            "porque e a soma de buildings.json e a cidade nova nasce sem predio. "
            "x/y = lonlat_to_xy do mapa 5000x2500."
        ),
        "license": ("GeoNames: CC BY 4.0. Thomas Brinkhoff / citypopulation.de: "
                    "uso com atribuicao ao link."),
        "sources": {
            "state_population": "data/definitions/population/state_regions.json",
            "city_proper": GEONAMES_URL,
            "urban_agglomeration": AGGLOMERATION_URL,
            "agglomeration_table": "tools/agglomerations_2026.csv",
            "nation_population": "data/definitions/scenario/nations.json",
        },
        "i18n": {
            "pt_BR": {"city.name." + r[0]: r[11] for r in ROWS},
            "en": {"city.name." + r[0]: r[1] for r in ROWS},
            "es": {"city.name." + r[0]: r[12] for r in ROWS},
        },
        "i18n_note": ("Nomes iguais ao ingles quando nao ha exonymo consagrado "
                      "em pt/es (mesma pratica das cidades atuais: Pequim/Cantao "
                      "so onde existe forma consagrada)."),
        "cities": new_cities,
        "state_patch": {
            r["provenance"]["state_id"]: {
                "city_id": r["id"],
                "attach_rule_before": r["provenance"]["attached_before"]["attach_rule"],
                "attached_to_before": r["provenance"]["attached_before"]["city_id"],
                "attach_rule_after": "primary_city",
            } for r in new_cities
        },
        "neighbor_patch": removed,
        "population_rateio_after": {
            "rule": ("build_world_cities.distribute_national_population: peso = "
                     "urban_population, soma exata por nacao, resto pelas maiores "
                     "fracoes (desempate por id)"),
            "changed": {c["id"]: {"before": int(c["population"]),
                                  "after": after[c["id"]]}
                        for c in cities
                        if int(c["population"]) != after[c["id"]]},
        },
        "checks": {
            "states": len(nstates),
            "cities": len(ncities),
            "sum_states": total_states,
            "sum_regions": total_regions,
            "sum_urban": total_urban,
            "sum_region_population": total_region_pop,
            "cities_added": len(new_cities),
            "states_reassigned": len(new_cities),
            "cities_region_below_urban_before": len(base_clamped),
            "cities_region_below_urban_after": len(clamped),
            "cities_region_below_urban_new": new_clamped,
            "urban_plus_rural_after": total_urban + total_region_pop,
            "urban_plus_rural_note": (
                "Nao fecha o total por causa do piso 0 de region_population nas "
                "%d cidades com regiao < urbano (D-005). A soma que fecha por "
                "nacao e population_region." % len(clamped)),
        },
    }

    if problems:
        print("PROBLEMAS (%d):" % len(problems))
        for problem in problems:
            print(" -", problem)
        return 1

    print("ok: estados=%d cidades=%d soma=%d (+%d cidades, %d estados)"
          % (len(nstates), len(ncities), total_states, len(new_cities),
             len(new_cities)))
    for record in new_cities:
        print("  %-8s %-20s urb=%-9d pop=%-11d x=%7.1f y=%6.1f"
              % (record["id"], record["name"], record["urban_population"],
                 record["population"], record["x"], record["y"]))

    if args.write:
        dump_json(OUT, payload)
        dump_state_regions(STATE_REGIONS, new_regions)
        print("escrito:", OUT)
        print("escrito:", STATE_REGIONS)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
