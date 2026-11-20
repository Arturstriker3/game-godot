#!/usr/bin/env python3
"""Validador do dado de regimes cambiais (fx_regimes_2026.json).

Confere, contra data/definitions/economy/currencies.json:

- toda moeda do jogo tem exatamente uma entrada (e nenhuma entrada a mais);
- a classificacao e uma das declaradas em classification_values;
- game_regime bate com game_regime_map[classification];
- ancora, quando existe, e uma moeda do proprio jogo e nao a propria moeda;
- source presente e nao vazio em toda entrada, exceto "unknown" (que declara a ausencia);
- band_width so aparece com band_width_source (o Apendice II.9 do AREAER nao declara larguras);
- "unknown" nao tem regime nem ancora;
- nations bate com currencies[].nations, e as excecoes declaradas pertencem a moeda.

Sai com codigo != 0 em qualquer erro. Nao altera arquivos.
"""

from __future__ import annotations

import json
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DATA = ROOT / "data" / "definitions" / "economy"
REGIMES = DATA / "fx_regimes_2026.json"
CURRENCIES = DATA / "currencies.json"
GAME_REGIMES = {"floating", "band", "peg"}


def load(path: Path):
    with path.open(encoding="utf-8") as fh:
        return json.load(fh)


def main() -> int:
    errors: list[str] = []
    warnings: list[str] = []

    if not REGIMES.exists():
        print(f"ERRO: {REGIMES} nao existe")
        return 1

    data = load(REGIMES)
    currencies = load(CURRENCIES)["currencies"]
    game = {c["code"]: c for c in currencies}
    entries = data.get("currencies") or {}

    allowed = set((data.get("classification_values") or {}).keys())
    regime_map = data.get("game_regime_map") or {}
    if not allowed:
        errors.append("classification_values vazio")
    if not regime_map:
        errors.append("game_regime_map vazio")
    for key, value in regime_map.items():
        if value is not None and value not in GAME_REGIMES:
            errors.append(f"game_regime_map[{key}] = {value!r} nao e regime do jogo")

    # cobertura exata
    missing = sorted(set(game) - set(entries))
    extra = sorted(set(entries) - set(game))
    if missing:
        errors.append(f"moedas do jogo sem entrada: {missing}")
    if extra:
        errors.append(f"entradas sem moeda no jogo: {extra}")

    for code, entry in sorted(entries.items()):
        where = f"currencies[{code}]"
        cls = entry.get("classification")
        if cls not in allowed:
            errors.append(f"{where}: classification {cls!r} fora de classification_values")
            continue
        expected_regime = regime_map.get(cls)
        if entry.get("game_regime") != expected_regime:
            errors.append(
                f"{where}: game_regime {entry.get('game_regime')!r} != "
                f"game_regime_map[{cls}] = {expected_regime!r}"
            )
        source = entry.get("source")
        if not isinstance(source, str) or not source.strip():
            errors.append(f"{where}: source ausente")

        anchor = entry.get("anchor")
        if anchor is not None:
            if anchor == code:
                errors.append(f"{where}: ancora aponta para a propria moeda")
            elif anchor not in game:
                errors.append(f"{where}: ancora {anchor!r} nao e moeda do jogo")
        atype = entry.get("anchor_type")
        if atype not in {"currency", "composite", "not_stated", "none"}:
            errors.append(f"{where}: anchor_type invalido {atype!r}")
        if (anchor is None) != (atype != "currency"):
            errors.append(
                f"{where}: anchor={anchor!r} incoerente com anchor_type={atype!r}"
            )

        if entry.get("band_width") is not None and not entry.get("band_width_source"):
            errors.append(f"{where}: band_width sem band_width_source")

        if cls == "unknown":
            if entry.get("game_regime") is not None:
                errors.append(f"{where}: unknown nao pode ter game_regime")
            if anchor is not None:
                errors.append(f"{where}: unknown nao pode ter ancora")

        if code in game and entry.get("nations") != (game[code].get("nations") or []):
            errors.append(f"{where}: nations diverge de currencies.json")

        exceptions = entry.get("exceptions") or {}
        for nation, nation_cls in exceptions.items():
            if nation not in (game.get(code, {}).get("nations") or []):
                errors.append(f"{where}: excecao {nation} nao pertence a moeda")
            if nation_cls not in allowed:
                errors.append(f"{where}: excecao {nation} com classificacao {nation_cls!r}")

    # resumo
    by_cls = Counter(e.get("classification") for e in entries.values())
    by_regime = Counter(e.get("game_regime") for e in entries.values())
    unknown = sorted(c for c, e in entries.items() if e.get("classification") == "unknown")
    pegs = [
        (c, e.get("anchor"))
        for c, e in sorted(entries.items())
        if e.get("game_regime") == "peg" and e.get("anchor")
    ]
    no_anchor_pegs = sorted(
        c for c, e in entries.items()
        if e.get("game_regime") == "peg" and e.get("anchor") is None
    )

    print(f"moedas: {len(entries)} (jogo: {len(game)})")
    print("por classificacao: " + ", ".join(f"{k}={v}" for k, v in sorted(by_cls.items())))
    print(
        "por regime do jogo: "
        + ", ".join(f"{k}={v}" for k, v in sorted(by_regime.items(), key=lambda kv: str(kv[0])))
    )
    print(f"unknown ({len(unknown)}): {', '.join(unknown)}")
    print(f"pegs com ancora ({len(pegs)}): " + ", ".join(f"{c}->{a}" for c, a in pegs))
    if no_anchor_pegs:
        print(f"pegs sem ancora no dado ({len(no_anchor_pegs)}): {', '.join(no_anchor_pegs)}")

    if warnings:
        print()
        for w in warnings:
            print(f"AVISO: {w}")
    if errors:
        print()
        for e in errors:
            print(f"ERRO: {e}")
        print(f"\nFALHOU: {len(errors)} erro(s)")
        return 1
    print("\nOK: dado de regimes cambiais valido")
    return 0


if __name__ == "__main__":
    sys.exit(main())
