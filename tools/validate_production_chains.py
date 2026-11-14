#!/usr/bin/env python3
"""Validador dos dados das cadeias de produção (Fase F1).

Guarda o contrato de `data/definitions/economy/{goods,recipes,needs,currencies}.json`,
do i18n (`product.<id>`/`resource.<id>` × pt-BR/en/es) e dos ícones do Icon Forge:

  - 113 bens, ids únicos, `order` 0..112 e os 21 do núcleo em 0..20 na ordem canônica;
  - todo insumo, saída e subproduto de receita existe; sem ciclo; `yield > 0`;
  - todo bem tem produtor (receita, subproduto declarado ou alias) e consumidor
    (insumo de receita, necessidade das POPs, `use` institucional declarado ou
    `consumer_note`) — nada de órfão;
  - necessidades com substitutos existentes e pesos do plano;
  - uma moeda por código de `nations.json`, taxa > 0 ou `unknown:no_gdp` explícito;
  - i18n completo nos três idiomas e ícone (manifesto + SVG) para todo bem.

Uso: `python tools/validate_production_chains.py`
Saída: 0 problemas → código 0; qualquer problema → código 1.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from build_goods_chain import cmd_check  # noqa: E402  (depende do sys.path acima)

if __name__ == "__main__":
    sys.exit(cmd_check())
