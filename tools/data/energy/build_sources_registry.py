"""Gera o registro único de fontes/licenças da energia: `_sources.json`.

Cada fonte entra com URL, data de acesso, licença verificada, evidência da
licença, citação, arquivos que a consomem e a transformação aplicada. Fontes
avaliadas e recusadas também ficam registradas (com o motivo) — assim a decisão
de não usar não se perde.

D-115 (2026-11-21): o WorldClim saiu do E1 (licença não comercial) e entrou
GSA/GWA (CC BY 4.0), CHIRPS (domínio público/CC BY 4.0), ETOPO1 (domínio
público) e CGIAR PET v3.1 (CC BY 4.0). A separação do óleo foi tentada com
OWID e EIA; nenhuma das duas publica capacidade instalada de óleo por país, então
a lacuna fica declarada.

Uso:
    python tools/data/energy/build_sources_registry.py
Saída: data/definitions/energy/_sources.json
"""

from __future__ import annotations

import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import build_energy_common as ec  # noqa: E402

CACHE = os.path.join(ROOT, "tools", "data", "energy", "cache")
HYDRO_CACHE = os.path.join(CACHE, "hydro")
OUT = os.path.join(ec.DEFS, "energy", "_sources.json")
VERIFIED_ON = "2026-11-22"
ACCESSED_AT = "2026-11-22T01:45:00Z"


def read_json(path: str) -> dict:
    if not os.path.exists(path):
        return {}
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def main() -> int:
    ember = read_json(os.path.join(CACHE, "ember_yearly_global.manifest.json"))
    clear = read_json(os.path.join(CACHE, "wb_hydro_developed_pct.manifest.json"))
    gppd = read_json(os.path.join(ROOT, "tools", "data", "gppd", "manifest.json"))
    wdi = read_json(os.path.join(ROOT, "tools", "data", "wdi_energy_manifest.json"))
    irena = read_json(os.path.join(ROOT, "tools", "data", "irena_capacity_manifest.json"))
    hydro = read_json(os.path.join(HYDRO_CACHE, "manifest.json"))
    atlas = read_json(os.path.join(CACHE, "atlas_points.json"))

    gsa_meta = (atlas.get("meta") or {}).get("sources", {}).get("gsa", {})
    gwa_meta = (atlas.get("meta") or {}).get("sources", {}).get("gwa", {})
    atlas_accessed = (atlas.get("meta") or {}).get("accessed_at_utc") or ACCESSED_AT

    def hydro_entry(key):
        entry = hydro.get(key) or {}
        return {
            "url": entry.get("url"),
            "accessed_at_utc": entry.get("accessed_at_utc") or ACCESSED_AT,
            "sha256": entry.get("sha256"),
            "bytes": entry.get("bytes"),
            "extracted": entry.get("extracted"),
        }

    sources = [
        {
            "id": "gppd_wri",
            "title": "Global Power Plant Database v1.3.0",
            "publisher": "World Resources Institute (WRI)",
            "url": gppd.get("source_url"),
            "accessed_at_utc": gppd.get("accessed_at_utc") or ACCESSED_AT,
            "sha256": gppd.get("sha256"),
            "reference_period": "2021 (base descontinuada nessa versão)",
            "license": {
                "name": "CC BY 4.0",
                "url": "https://creativecommons.org/licenses/by/4.0/",
                "commercial_ok": True,
                "verified_on": VERIFIED_ON,
                "evidence": ("licença declarada no repositório WRI/global-power-plant-database "
                             "e no README da v1.3.0"),
            },
            "citation": gppd.get("citation"),
            "used_in": ["data/definitions/energy/power_plants_base_2021_gppd.json",
                        "data/definitions/energy/power_plants_2026_scaled.json"],
            "transform": ("usina individual (nome, fonte, MW, lat/lon) atribuída à cidade "
                          "controladora no atlas (D-115a); nenhum MW extrapolado; o "
                          "escalonamento 2024-2025 é declarado no arquivo scaled"),
            "status": "used",
        },
        {
            "id": "ember_yearly",
            "title": "Yearly Electricity Data (global release)",
            "publisher": "Ember Energy Research CIC",
            "url": ember.get("url"),
            "landing_url": ember.get("landing_url"),
            "accessed_at_utc": ember.get("accessed_at_utc") or ACCESSED_AT,
            "sha256": ember.get("sha256"),
            "reference_period": "1985-2025 (capacidade por fonte; 2025 parcial por país)",
            "license": {
                "name": "CC BY 4.0",
                "url": "https://ember-energy.org/creative-commons/",
                "commercial_ok": True,
                "verified_on": VERIFIED_ON,
                "evidence": ("\"All content is released under a Creative Commons Attribution "
                             "Licence (CC-BY-4.0)\" na página do dataset e no rodapé de "
                             "ember-energy.org; a API declara o mesmo."),
            },
            "citation": ember.get("citation"),
            "used_in": ["data/definitions/energy/national_capacity.json",
                        "data/definitions/energy/hydro_potential.json",
                        "data/definitions/energy/power_plants_2026_scaled.json"],
            "transform": ("Capacity (GW) x 1000 -> MW; último ano não vazio entre 2024 e "
                          "2025 por país; matriz mapeada para as 9 fontes do jogo; alvo do "
                          "escalonamento por nação x fonte"),
            "status": "used",
        },
        {
            "id": "wb_clear_hydro",
            "title": "Technically feasible hydropower potential developed (%)",
            "publisher": "World Bank (Data360, base WB_CLEAR)",
            "url": clear.get("url"),
            "metadata_url": ("https://data360files.worldbank.org/data360-data/metadata/WB_CLEAR/"
                             "WB_CLEAR_HP_TECHNFEASIBLE_PT.pdf"),
            "accessed_at_utc": clear.get("accessed_at_utc") or ACCESSED_AT,
            "sha256": clear.get("sha256"),
            "reference_period": "2021 (único ano do indicador)",
            "license": {
                "name": "CC BY 4.0",
                "url": "https://creativecommons.org/licenses/by/4.0/",
                "commercial_ok": True,
                "verified_on": VERIFIED_ON,
                "evidence": ("metadata oficial do indicador no Data360 declara CC BY 4.0 "
                             "com a cláusula WIPO de mediação"),
            },
            "citation": clear.get("citation"),
            "used_in": ["data/definitions/energy/hydro_potential.json"],
            "transform": ("potencial_mw = hidro_instalada_2021_mw / (developed_pct/100); "
                          "divisão interna do país pelo proxy CHIRPS x PET x relevo x rio"),
            "status": "used",
        },
        {
            "id": "wb_wdi",
            "title": "World Development Indicators - acesso à eletricidade e perdas T&D",
            "publisher": "World Bank (Data360, base WB_WDI; água = JMP WHO/UNICEF)",
            "url": wdi.get("endpoints", {}).get("data"),
            "accessed_at_utc": wdi.get("fetched_at_utc") or ACCESSED_AT,
            "reference_period": "ano mais recente não vazio por país (sem interpolação)",
            "license": {
                "name": "CC BY 4.0",
                "url": "https://creativecommons.org/licenses/by/4.0/",
                "commercial_ok": True,
                "verified_on": VERIFIED_ON,
                "evidence": "licença declarada no Data360/WDI para os indicadores usados",
            },
            "citation": wdi.get("citation"),
            "used_in": ["data/definitions/energy/access_losses.json"],
            "transform": ("último ano não vazio por país/indicador; ausente fica null "
                          "(sem interpolação)"),
            "status": "used",
        },
        {
            "id": "gsa_solar_atlas",
            "title": "Global Solar Atlas 2.x — GHI e PVOUT (anual + mensal)",
            "publisher": "Solargis para ESMAP/Banco Mundial",
            "url": "https://globalsolaratlas.info/",
            "api_url": "https://api.globalsolaratlas.info/data/lta",
            "accessed_at_utc": atlas_accessed,
            "reference_period": "long-term average (até 2025; versão de dados 1.7)",
            "license": {
                "name": (gsa_meta.get("name") or "CC BY 4.0"),
                "url": "https://creativecommons.org/licenses/by/4.0/",
                "commercial_ok": True,
                "verified_on": VERIFIED_ON,
                "evidence": ("World Bank Data Catalog, World - GHI/PVOUT GIS Data (Global "
                             "Solar Atlas): license Creative Commons Attribution 4.0. "
                             "Verificado em 2026-11-22."),
            },
            "citation": ("Global Solar Atlas 2.x (Solargis/ESMAP/World Bank), CC BY 4.0; "
                         "https://globalsolaratlas.info"),
            "used_in": ["data/definitions/energy/renewable_potential.json"],
            "transform": ("amostra no centróide da província; GHI/PVOUT anual e série "
                          "mensal convertida para kWh/m2/dia e kWh/kWp/dia; cidade = média "
                          "ponderada pela área"),
            "status": "used",
        },
        {
            "id": "gwa_wind_atlas",
            "title": "Global Wind Atlas 3/4 — vento a 100 m (Weibull)",
            "publisher": "DTU Wind Energy para ESMAP/Banco Mundial",
            "url": "https://globalwindatlas.info/",
            "api_url": "https://globalwindatlas.info/api/gwa/custom/Lib/",
            "accessed_at_utc": atlas_accessed,
            "reference_period": "climatologia (GWA 3/4)",
            "license": {
                "name": (gwa_meta.get("name") or "CC BY 4.0"),
                "url": "https://creativecommons.org/licenses/by/4.0/",
                "commercial_ok": True,
                "verified_on": VERIFIED_ON,
                "evidence": ("World Bank Data Catalog, World - Wind Speed and Power Density "
                             "GIS Data (Global Wind Atlas): license CC BY 4.0. Verificado "
                             "em 2026-11-22."),
            },
            "citation": ("Global Wind Atlas 3/4 (DTU/ESMAP/World Bank), CC BY 4.0; "
                         "https://globalwindatlas.info"),
            "used_in": ["data/definitions/energy/renewable_potential.json"],
            "transform": ("Weibull a 100 m por classe de rugosidade; média e densidade de "
                          "potência na classe 0,100 m; cidade = média ponderada pela área; "
                          "sem série mensal (lacuna declarada)"),
            "status": "used",
        },
        {
            "id": "chirps_v3",
            "title": "CHIRPS v3.0 — precipitação anual 2025",
            "publisher": "Climate Hazards Center, UC Santa Barbara",
            **hydro_entry("chirps_v3_2025"),
            "reference_period": "2025 (anual)",
            "license": {
                "name": "domínio público / CC BY 4.0",
                "url": "https://creativecommons.org/licenses/by/4.0/",
                "commercial_ok": True,
                "verified_on": VERIFIED_ON,
                "evidence": ("CHC declara CHIRPS3 em domínio público, registrado como "
                             "CC BY 4.0 (chc.ucsb.edu/data/chirps3)"),
            },
            "citation": ("Funk et al. (2026), CHIRPS v3, Sci Data 13:718; "
                         "https://doi.org/10.15780/G2JQ0P"),
            "used_in": ["data/definitions/energy/renewable_potential.json"],
            "transform": ("precipitação anual (mm/ano) amostrada no centróide da província; "
                          "proxy hídrico bruto, normalizado pela âncora nacional"),
            "status": "used",
        },
        {
            "id": "etopo1_noaa",
            "title": "ETOPO1 Ice Surface — relevo global 1 arc-min",
            "publisher": "NOAA/NCEI",
            **hydro_entry("etopo1"),
            "reference_period": "2008 (modelo estático)",
            "license": {
                "name": "domínio público (NOAA/NCEI)",
                "url": "https://www.ngdc.noaa.gov/mgg/global/relief/ETOPO1/docs/ETOPO1.pdf",
                "commercial_ok": True,
                "verified_on": VERIFIED_ON,
                "evidence": "NOAA/NCEI distribui ETOPO1 sem restrição de uso",
            },
            "citation": ("Amante & Eakins (2009), ETOPO1 1 Arc-Minute Global Relief Model, "
                         "NOAA Technical Memorandum NESDIS NGDC-24"),
            "used_in": ["data/definitions/energy/renewable_potential.json"],
            "transform": ("desnível (max-min) por província na grade 4096x2048; altitude "
                          "média para corrigir a densidade do ar do GWA"),
            "status": "used",
        },
        {
            "id": "cgiar_pet_v3_1",
            "title": "Global Aridity Index and PET Database v3.1 — PET anual",
            "publisher": "CGIAR-CSI (Trabucco & Zomer)",
            **hydro_entry("cgiar_pet_v3_1"),
            "reference_period": "climatologia 1970-2000 (v3.1)",
            "license": {
                "name": "CC BY 4.0",
                "url": "https://creativecommons.org/licenses/by/4.0/",
                "commercial_ok": True,
                "verified_on": VERIFIED_ON,
                "evidence": ("figshare 7504448 declara license CC BY 4.0; é um produto "
                             "derivado, não o WorldClim bruto"),
            },
            "citation": ("Zomer et al. (2008); Trabucco & Zomer (2019), Global Aridity "
                         "Index and PET Database v3.1"),
            "used_in": ["data/definitions/energy/renewable_potential.json"],
            "transform": ("PET anual (mm/ano) no centróide da província; runoff = "
                          "clamp((precip-PET)/precip, 0, 1)"),
            "status": "used",
        },
        {
            "id": "natural_earth_10m",
            "title": "Natural Earth 10m - rios e divisas",
            "publisher": "Natural Earth",
            "url": "https://www.naturalearthdata.com/downloads/10m-physical-vectors/",
            "accessed_at_utc": ACCESSED_AT,
            "license": {
                "name": "domínio público",
                "url": "https://www.naturalearthdata.com/about/terms-of-use/",
                "commercial_ok": True,
                "verified_on": VERIFIED_ON,
                "evidence": "Natural Earth declara domínio público (sem restrição de uso)",
            },
            "used_in": ["data/definitions/energy/renewable_potential.json",
                        "data/definitions/energy/land_borders.json",
                        "data/definitions/energy/hydro_potential.json"],
            "transform": ("rios desenhados na grade 4096x2048; cobertura a 50 km vira "
                          "river_factor; divisas derivadas do raster de donos"),
            "status": "used",
        },
        {
            "id": "ijhd_hydropower_essentials",
            "title": "Hydropower Essentials (potencial técnico mundial, 16.400 TWh/ano)",
            "publisher": "IJHD (citado via OSTI/ETDEWEB 21594387)",
            "url": "https://www.osti.gov/etdeweb/biblio/21594387",
            "accessed_at_utc": ACCESSED_AT,
            "license": {
                "name": "citação de um número factual, não redistribuição de base",
                "commercial_ok": True,
                "verified_on": VERIFIED_ON,
                "evidence": "apenas a frase de potencial global é citada, com atribuição",
            },
            "used_in": ["data/definitions/energy/renewable_potential.json"],
            "transform": ("âncora GLOBAL de 16.400 TWh/ano no proxy hídrico; a âncora "
                          "nacional do E1 é aplicada em hydro_potential.json"),
            "status": "citation_only",
        },
        {
            "id": "worldclim_21",
            "title": "WorldClim 2.1 - srad, wind e elev (10 arc-min)",
            "publisher": "Fick & Hijmans (UC Davis)",
            "url": "https://geodata.ucdavis.edu/climate/worldclim/2_1/base/",
            "landing_url": "https://www.worldclim.org/data/worldclim21.html",
            "accessed_at_utc": ACCESSED_AT,
            "license": {
                "name": "uso acadêmico e não comercial (NÃO é CC BY)",
                "url": "https://www.worldclim.org/about.html",
                "commercial_ok": False,
                "verified_on": VERIFIED_ON,
                "quote": ("The data are freely available for academic use and other "
                          "non-commercial use. Redistribution or commercial use is not "
                          "allowed without prior permission."),
                "evidence": ("verificado em 2026-11-22 na página About do worldclim.org; "
                             "a página do dataset (worldclim21.html) não declara licença"),
            },
            "status": "rejected",
            "reason": ("D-115c: substituído por Global Solar Atlas/Global Wind Atlas "
                       "(CC BY 4.0), CHIRPS (domínio público), CGIAR PET v3.1 (CC BY "
                       "4.0) e ETOPO1 (domínio público). O clima do jogo "
                       "(province_nature.json) ainda usa WorldClim — dívida separada, "
                       "registrada no GAME_STATE §15."),
        },
        {
            "id": "owid_energy",
            "title": "Our World in Data — Energy dataset",
            "publisher": "Our World in Data",
            "url": "https://catalog.ourworldindata.org/energy/owid_energy/owid_energy.csv",
            "accessed_at_utc": ACCESSED_AT,
            "license": {
                "name": "CC BY 4.0",
                "url": "https://creativecommons.org/licenses/by/4.0/",
                "commercial_ok": True,
                "verified_on": VERIFIED_ON,
                "evidence": "OWID publica o dataset de energia sob CC BY 4.0",
            },
            "status": "rejected",
            "reason": ("tentativa de separar óleo (D-115d): o dataset tem geração de "
                       "eletricidade por fonte (oil_electricity_twh), mas NÃO capacidade "
                       "instalada de óleo por país; não serve para MW. Lacuna declarada."),
        },
        {
            "id": "eia_international",
            "title": "EIA International Energy Data (bulk INTL)",
            "publisher": "U.S. Energy Information Administration",
            "url": "https://www.eia.gov/opendata/bulk/INTL.zip",
            "accessed_at_utc": ACCESSED_AT,
            "license": {
                "name": "domínio público (governo dos EUA)",
                "url": "https://www.eia.gov/about/copyrights_reuse.php",
                "commercial_ok": True,
                "verified_on": VERIFIED_ON,
                "evidence": "dados do governo dos EUA são de domínio público",
            },
            "status": "rejected",
            "reason": ("tentativa de separar óleo (D-115d): o bulk INTL tem 'Fossil fuels "
                       "electricity installed capacity' agregado e 'Petroleum fossil fuel' "
                       "apenas como geração (billion kWh); NÃO há capacidade instalada de "
                       "óleo por país. Lacuna declarada."),
        },
        {
            "id": "irena_elecstat",
            "title": "IRENA ELECSTAT - capacidade por tecnologia",
            "publisher": "IRENA",
            "url": irena.get("source_url"),
            "accessed_at_utc": irena.get("accessed_at_utc") or ACCESSED_AT,
            "license": {
                "name": "IRENA Terms and Conditions (não comercial)",
                "url": "https://www.irena.org/terms-and-conditions",
                "commercial_ok": False,
                "verified_on": "2026-11-17",
                "quote": (irena.get("license", {}).get("quote") or "")[:400],
                "evidence": "art. 4.1 dos termos: uso livre apenas não comercial",
            },
            "status": "rejected",
            "reason": ("usada pela coleta anterior apenas para comparar totais; removida das "
                       "saídas versionadas porque os termos não permitem uso comercial. A "
                       "comparação agora é contra a Ember (CC BY 4.0)."),
        },
        {
            "id": "gem_trackers",
            "title": "Global Energy Monitor - trackers de usinas",
            "publisher": "Global Energy Monitor",
            "url": "https://globalenergymonitor.org/projects/",
            "accessed_at_utc": ACCESSED_AT,
            "license": {
                "name": "CC BY 4.0 na licença, com atestação de não uso comercial no formulário",
                "url": "https://globalenergymonitor.org/creative-commons-public-license",
                "commercial_ok": False,
                "verified_on": VERIFIED_ON,
                "evidence": ("páginas de download exigem aceitar 'I will not use this data for "
                             "commercial purposes'; isenção comercial só por pedido à GEM"),
            },
            "status": "rejected",
            "reason": ("não foi usada: o GPPD (CC BY 4.0) já cobre usina/MW/coordenada e a GEM "
                       "adiciona uma atestação de não uso comercial por download."),
        },
    ]

    payload = {
        "meta": {
            "generated_by": "tools/data/energy/build_sources_registry.py",
            "plan": "docs/plans/perf_pop_financas_plano.md, PARTE E, E1",
            "verified_on": VERIFIED_ON,
            "owner_decision": "D-115(c/d): GSA/GWA, CHIRPS/PET/ETOPO1 e tentativa de óleo",
            "rule": ("toda fonte usada precisa de URL, data de acesso e licença verificada; "
                     "fonte com licença incompatível com uso comercial entra como "
                     "used_restricted com blocker, ou como rejected"),
            "credits_debt": ("antes do E2 ligar o dado, a tela de créditos precisa citar "
                             "Ember, WRI (GPPD), Banco Mundial (WDI/âncora hídrica), "
                             "GSA/GWA, CHIRPS, CGIAR-CSI PET, ETOPO1 e Natural Earth"),
            "counts": {
                "used": sum(1 for s in sources if s["status"] == "used"),
                "used_restricted": sum(1 for s in sources if s["status"] == "used_restricted"),
                "citation_only": sum(1 for s in sources if s["status"] == "citation_only"),
                "rejected": sum(1 for s in sources if s["status"] == "rejected"),
            },
        },
        "sources": sources,
    }
    ec.write_json(OUT, payload)
    print("fontes registradas: %d" % len(sources))
    for source in sources:
        license_name = source["license"]["name"]
        ok = source["license"].get("commercial_ok")
        print("  %-26s %-8s %-8s %s" % (source["id"], source["status"],
                                        "comercial" if ok else "restrita", license_name))
    return 0


if __name__ == "__main__":
    sys.exit(main())
