"""Gera o registro único de fontes/licenças da energia: `_sources.json`.

Cada fonte entra com URL, data de acesso, licença verificada, evidência da
licença, citação, arquivos que a consomem e a transformação aplicada. Fontes
avaliadas e recusadas também ficam registradas (com o motivo) — assim a decisão
de não usar não se perde.

O script lê os manifests dos caches para não repetir hash/URL à mão; o que não
tem manifest (WorldClim, IRENA, GEM, IJHD) está declarado aqui com a data e a
evidência da verificação.

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
OUT = os.path.join(ec.DEFS, "energy", "_sources.json")
VERIFIED_ON = "2026-11-21"


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

    sources = [
        {
            "id": "gppd_wri",
            "title": "Global Power Plant Database v1.3.0",
            "publisher": "World Resources Institute (WRI)",
            "url": gppd.get("source_url"),
            "accessed_at_utc": gppd.get("accessed_at_utc"),
            "sha256": gppd.get("sha256"),
            "reference_period": "2021 (base descontinuada nessa versao)",
            "license": {
                "name": "CC BY 4.0",
                "url": "https://creativecommons.org/licenses/by/4.0/",
                "commercial_ok": True,
                "verified_on": VERIFIED_ON,
                "evidence": ("licenca declarada no repositorio WRI/global-power-plant-database "
                             "e no README da v1.3.0"),
            },
            "citation": gppd.get("citation"),
            "used_in": ["data/definitions/energy/power_plants_2026.json"],
            "transform": ("usina individual (nome, fonte, MW, lat/lon) atribuida a cidade dona "
                          "da provincia pelo raster do atlas; nenhum MW extrapolado"),
            "status": "used",
        },
        {
            "id": "ember_yearly",
            "title": "Yearly Electricity Data (global release)",
            "publisher": "Ember Energy Research CIC",
            "url": ember.get("url"),
            "landing_url": ember.get("landing_url"),
            "accessed_at_utc": ember.get("accessed_at_utc"),
            "sha256": ember.get("sha256"),
            "reference_period": "1985-2025 (capacidade por fonte; 2025 parcial por pais)",
            "license": {
                "name": "CC BY 4.0",
                "url": "https://ember-energy.org/creative-commons/",
                "commercial_ok": True,
                "verified_on": VERIFIED_ON,
                "evidence": ("\"All content is released under a Creative Commons Attribution "
                             "Licence (CC-BY-4.0)\" na pagina do dataset e no rodape de "
                             "ember-energy.org; a API declara o mesmo."),
            },
            "citation": ember.get("citation"),
            "used_in": ["data/definitions/energy/national_capacity.json",
                        "data/definitions/energy/hydro_potential.json"],
            "transform": ("Capacity (GW) x 1000 -> MW; ultimo ano nao vazio entre 2024 e 2025 "
                          "por pais; matriz mapeada para as 9 fontes do jogo"),
            "status": "used",
        },
        {
            "id": "wb_clear_hydro",
            "title": "Technically feasible hydropower potential developed (%)",
            "publisher": "World Bank (Data360, base WB_CLEAR)",
            "url": clear.get("url"),
            "metadata_url": ("https://data360files.worldbank.org/data360-data/metadata/WB_CLEAR/"
                             "WB_CLEAR_HP_TECHNFEASIBLE_PT.pdf"),
            "accessed_at_utc": clear.get("accessed_at_utc"),
            "sha256": clear.get("sha256"),
            "reference_period": "2021 (unico ano do indicador)",
            "license": {
                "name": "CC BY 4.0",
                "url": "https://creativecommons.org/licenses/by/4.0/",
                "commercial_ok": True,
                "verified_on": VERIFIED_ON,
                "evidence": ("metadata oficial do indicador no Data360 declara CC BY 4.0 "
                             "com a clausula WIPO de mediacao"),
            },
            "citation": clear.get("citation"),
            "used_in": ["data/definitions/energy/hydro_potential.json"],
            "transform": ("potencial_mw = hidro_instalada_2021_mw / (developed_pct/100); "
                          "divisao interna do pais pelo proxy precip x desnivel x rio"),
            "status": "used",
        },
        {
            "id": "wb_wdi",
            "title": "World Development Indicators - acesso a eletricidade e perdas T&D",
            "publisher": "World Bank (Data360, base WB_WDI; agua = JMP WHO/UNICEF)",
            "url": wdi.get("endpoints", {}).get("data"),
            "accessed_at_utc": wdi.get("fetched_at_utc"),
            "reference_period": "ano mais recente nao vazio por pais (sem interpolacao)",
            "license": {
                "name": "CC BY 4.0",
                "url": "https://creativecommons.org/licenses/by/4.0/",
                "commercial_ok": True,
                "verified_on": VERIFIED_ON,
                "evidence": "licenca declarada no Data360/WDI para os indicadores usados",
            },
            "citation": wdi.get("citation"),
            "used_in": ["data/definitions/energy/access_losses.json"],
            "transform": ("ultimo ano nao vazio por pais/indicador; ausente fica null "
                          "(sem interpolacao)"),
            "status": "used",
        },
        {
            "id": "worldclim_21",
            "title": "WorldClim 2.1 - srad, wind e elev (10 arc-min)",
            "publisher": "Fick & Hijmans (UC Davis)",
            "url": "https://geodata.ucdavis.edu/climate/worldclim/2_1/base/",
            "landing_url": "https://www.worldclim.org/data/worldclim21.html",
            "license": {
                "name": "uso academico e nao comercial (NAO e CC BY)",
                "url": "https://www.worldclim.org/about.html",
                "commercial_ok": False,
                "verified_on": VERIFIED_ON,
                "quote": ("The data are freely available for academic use and other "
                          "non-commercial use. Redistribution or commercial use is not "
                          "allowed without prior permission."),
                "evidence": ("verificado em 2026-11-21 na pagina About do worldclim.org; "
                             "a pagina do dataset (worldclim21.html) nao declara licenca"),
            },
            "citation": ("WorldClim 2.1, Fick & Hijmans 2017, Int. J. Climatol. 37:4302-4315"),
            "used_in": ["data/definitions/energy/renewable_potential.json",
                        "data/definitions/energy/hydro_potential.json"],
            "transform": ("srad kJ/m2/dia -> kWh/m2/dia; media anual e serie mensal por "
                          "provincia/cidade; elev -> desnivel do proxy hidrico"),
            "status": "used_restricted",
            "blocker": ("licenca NAO permite distribuicao/uso comercial. Antes de release "
                        "comercial: pedir permissao escrita a info@worldclim.org ou trocar "
                        "solar/eolico/relevo por Global Solar Atlas / Global Wind Atlas "
                        "(CC BY 4.0) e um MDE publico (SRTM/GMTED, dominio publico). O "
                        "clima do jogo (province_nature.json) ja usa a mesma fonte."),
        },
        {
            "id": "natural_earth_10m",
            "title": "Natural Earth 10m - rios e divisas",
            "publisher": "Natural Earth",
            "url": "https://www.naturalearthdata.com/downloads/10m-physical-vectors/",
            "license": {
                "name": "dominio publico",
                "url": "https://www.naturalearthdata.com/about/terms-of-use/",
                "commercial_ok": True,
                "verified_on": VERIFIED_ON,
                "evidence": "Natural Earth declara dominio publico (sem restricao de uso)",
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
            "title": "Hydropower Essentials (potencial tecnico mundial, 16.400 TWh/ano)",
            "publisher": "IJHD (citado via OSTI/ETDEWEB 21594387)",
            "url": "https://www.osti.gov/etdeweb/biblio/21594387",
            "license": {
                "name": "citacao de um numero factual, nao redistribuicao de base",
                "commercial_ok": True,
                "verified_on": VERIFIED_ON,
                "evidence": "apenas a frase de potencial global e citada, com atribuicao",
            },
            "used_in": ["data/definitions/energy/renewable_potential.json"],
            "transform": ("ancora GLOBAL de 16.400 TWh/ano no proxy hidrico antigo (a ancora "
                          "nacional do E1 substitui essa normalizacao em hydro_potential.json)"),
            "status": "citation_only",
        },
        {
            "id": "irena_elecstat",
            "title": "IRENA ELECSTAT - capacidade por tecnologia",
            "publisher": "IRENA",
            "url": irena.get("source_url"),
            "license": {
                "name": "IRENA Terms and Conditions (nao comercial)",
                "url": "https://www.irena.org/terms-and-conditions",
                "commercial_ok": False,
                "verified_on": "2026-11-17",
                "quote": (irena.get("license", {}).get("quote") or "")[:400],
                "evidence": "art. 4.1 dos termos: uso livre apenas nao comercial",
            },
            "status": "rejected",
            "reason": ("usada pela coleta anterior apenas para comparar totais; removida das "
                       "saidas versionadas porque os termos nao permitem uso comercial. A "
                       "comparacao agora e contra a Ember (CC BY 4.0)."),
        },
        {
            "id": "gem_trackers",
            "title": "Global Energy Monitor - trackers de usinas",
            "publisher": "Global Energy Monitor",
            "url": "https://globalenergymonitor.org/projects/",
            "license": {
                "name": "CC BY 4.0 na licenca, com atestacao de nao uso comercial no formulario",
                "url": "https://globalenergymonitor.org/creative-commons-public-license",
                "commercial_ok": False,
                "verified_on": VERIFIED_ON,
                "evidence": ("paginas de download exigem aceitar 'I will not use this data for "
                             "commercial purposes'; isencao comercial so por pedido a GEM"),
            },
            "status": "rejected",
            "reason": ("nao foi usada: o GPPD (CC BY 4.0) ja cobre usina/MW/coordenada e a GEM "
                       "adiciona uma atestacao de nao uso comercial por download."),
        },
    ]

    payload = {
        "meta": {
            "generated_by": "tools/data/energy/build_sources_registry.py",
            "plan": "docs/plans/perf_pop_financas_plano.md, PARTE E, E1",
            "verified_on": VERIFIED_ON,
            "rule": ("toda fonte usada precisa de URL, data de acesso e licenca verificada; "
                     "fonte com licenca incompativel com uso comercial entra como "
                     "used_restricted com blocker, ou como rejected"),
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
