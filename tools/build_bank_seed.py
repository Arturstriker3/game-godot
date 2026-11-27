#!/usr/bin/env python
# Gera data/definitions/economy/bank_seed.json a partir do harness medido.
#
# Entradas:
#   --artifact  JSON do tools/c1_world_ai_365.gd rodado com --banks-diag=1 no
#               baseline default (bancos DESLIGADOS = bit a bit o main).
#   --wdi       JSON {ISO3: {year, value}} de FB.BNK.CAPA.ZS (World Bank WDI,
#               "Bank capital to assets ratio (%)") baixado por
#               tools/fetch_bank_capital_wdi.py.
#   --banks     banks.json (roles_present por nacao).
#   --out       destino (default data/definitions/economy/bank_seed.json).
#
# Regra (docs/economy/rework/R2_plano.md §3.2/§10.3/§10.5/§10.6):
#   * `steady_loans` e o unico numero que define o ALVO DO MODELO por nacao
#     (capital_to_loans_target x steady_loans); vem MEDIDO do baseline;
#   * `model_gdp_annual` vem medido (PIB diario somado e anualizado x365);
#   * `capital_to_assets` = WDI medido quando existe; senao o fallback
#     declarado 0,10 (§10.6 — o mesmo valor nas quatro faixas de renda);
#   * `assets_pct_gdp`/`capital_pct_gdp`/`reference_target` ficam `null`
#     (`no_data`): nao ha serie inequivoca de ativos/PIB no repo e nada e
#     digitado a mao; o gap de referencia fica declaradamente ausente;
#   * `roles_present` vem de banks.json e e a unica lista capitalizavel.
#
# Nada aqui e dado de jogo inventado: cada campo tem a fonte no proprio JSON.
import argparse
import json
import os
import sys

CAPITAL_TO_LOANS_TARGET = 0.10
FALLBACK_CAPITAL_TO_ASSETS = 0.10  # §10.6 (igual nas 4 faixas de renda)

WDI_SOURCE = ("World Bank WDI FB.BNK.CAPA.ZS (Bank capital to assets ratio, %), "
              "ultimo ano disponivel por pais")
WDI_URL = ("https://api.worldbank.org/v2/country/all/indicator/"
           "FB.BNK.CAPA.ZS?format=json&date=2015:2024&per_page=20000")
GDP_SOURCE = ("harness tools/c1_world_ai_365.gd (baseline default, bancos "
              "desligados): PIB diario por cidade somado por nacao e "
              "anualizado x365")
LOANS_SOURCE = ("harness tools/c1_world_ai_365.gd no fecho do baseline default "
                "(banks.diag >= dia): carteira de credito por nacao (steady "
                "state medido)")


def load(path: str):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


def roles_present(banks: dict, iso: str) -> list:
    row = (banks.get("nations") or {}).get(iso, {})
    out = []
    for role in ("public", "private", "foreign"):
        entry = row.get(role) or {}
        if entry.get("status") == "present":
            out.append(role)
    return out


# Participacoes declaradas da regra C4 (docs/economy/C4_banks.md §3), as mesmas
# constantes de sim/bank.h (kSharePublic/Private/Foreign), renormalizadas entre
# os papeis PRESENTES. Nao e dado: e a regra declarada publicada por nacao.
DECLARED_SHARES = {"public": 0.45, "private": 0.40, "foreign": 0.15}


def roles_shares(present: list) -> dict:
    weight = sum(DECLARED_SHARES[role] for role in present)
    out = {"public_share": None, "private_share": None, "foreign_share": None}
    if weight <= 0.0:
        return out
    for role in present:
        out[role + "_share"] = DECLARED_SHARES[role] / weight
    return out


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--artifact", required=True)
    parser.add_argument("--wdi", required=True)
    parser.add_argument("--banks", default="data/definitions/economy/banks.json")
    parser.add_argument("--out",
                        default="data/definitions/economy/bank_seed.json")
    args = parser.parse_args()

    artifact = load(args.artifact)
    diag = artifact.get("banks_diag") or {}
    timeline = diag.get("timeline") or []
    if not timeline:
        print("artifact sem banks_diag.timeline (rode com --banks-diag=1)",
              file=sys.stderr)
        return 2
    final = timeline[-1]
    gdp = final.get("gdp_annual") or diag.get("gdp_annual") or {}
    nations = {row["nation"]: row for row in final.get("nations", [])}
    wdi = load(args.wdi)
    banks = load(args.banks)

    out_nations = {}
    stats = {"wdi": 0, "fallback": 0, "no_loans": 0, "no_roles": 0}
    for iso in sorted(set(nations) | set(wdi)):
        bank_row = nations.get(iso, {})
        steady = float(bank_row.get("loans", 0.0))
        model_gdp = float(gdp.get(iso, 0.0))
        present = roles_present(banks, iso)
        capital_to_assets = None
        source_key = "declared_default"
        confidence = "estimated"
        if iso in wdi and wdi[iso].get("value"):
            capital_to_assets = float(wdi[iso]["value"]) / 100.0
            source_key = "wdi_fb_bnk_capa"
            confidence = "partial"  # ativos/PIB segue no_data
            stats["wdi"] += 1
        elif present:
            capital_to_assets = FALLBACK_CAPITAL_TO_ASSETS
            stats["fallback"] += 1
        else:
            confidence = "no_data"
        if steady <= 0.0:
            stats["no_loans"] += 1
        if not present:
            stats["no_roles"] += 1
        out_nations[iso] = {
            "steady_loans": steady,
            "model_gdp_annual": model_gdp,
            "assets_pct_gdp": None,
            "capital_to_assets": capital_to_assets,
            "capital_pct_gdp": None,
            "reference_target": None,
            "confidence": confidence,
            "source_key": source_key,
            "roles_present": present,
            **roles_shares(present),
        }

    payload = {
        "source": "World Bank GFDD/WDI + BIS; ver docs/economy/rework/R2_plano.md",
        "year": 2024,
        "capital_to_loans_target": CAPITAL_TO_LOANS_TARGET,
        "model_gdp_source": GDP_SOURCE,
        "steady_loans_source": LOANS_SOURCE,
        "wdi_source": WDI_SOURCE,
        "wdi_url": WDI_URL,
        "fallback_note": ("capital_to_assets sem WDI = 0,10 declarado (§10.6, "
                          "igual nas quatro faixas de renda); nao e medicao"),
        "reference_note": ("assets_pct_gdp/capital_pct_gdp/reference_target = "
                           "null (no_data): sem serie inequivoca de ativos/PIB; "
                           "nada digitado a mao"),
        "artifact": os.path.basename(args.artifact),
        "counts": stats,
        "nations": out_nations,
    }
    with open(args.out, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(payload, handle, ensure_ascii=False, indent=1,
                  sort_keys=True)
        handle.write("\n")
    print("escrito %s: %d nacoes (%d WDI, %d fallback, %d sem carteira, "
          "%d sem papel present)" % (args.out, len(out_nations), stats["wdi"],
                                     stats["fallback"], stats["no_loans"],
                                     stats["no_roles"]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
