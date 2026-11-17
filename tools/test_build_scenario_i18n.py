"""Teste rápido da mescla de i18n do `build_scenario_data.py` (recal3, item 4).

Regressão que este teste tranca: regenerar `i18n.json` só a partir do
`i18n.csv` APAGAVA as 137 chaves que não nascem de CSV (`map.region.*`,
`city.services.*`, `dock.services`, `map.icon.industry`) e a UI caía para a
chave crua. A mescla preserva o que só existe no JSON, deixa o CSV vencer no
conflito e não apaga valor por linha vazia do CSV.

    <python> tools/test_build_scenario_i18n.py
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from build_scenario_data import merge_i18n  # noqa: E402

EXISTING = {
    "pt_BR": {"city.name.X": "Cidade antiga", "map.region.europe": "Europa",
              "dock.services": "Serviços do cais"},
    "en": {"city.name.X": "Old city", "map.region.europe": "Europe",
           "dock.services": "Dock services"},
    "es": {"city.name.X": "Ciudad vieja", "map.region.europe": "Europa",
           "dock.services": "Servicios del muelle"},
}
CSV_ROWS = [
    # conflito: o CSV vence nos três idiomas
    {"key": "city.name.X", "pt_BR": "Cidade nova", "en": "New city",
     "es": "Ciudad nueva"},
    # chave nova
    {"key": "port.name.Y", "pt_BR": "Porto", "en": "Port", "es": "Puerto"},
    # valor vazio no CSV NÃO apaga o que já estava
    {"key": "dock.services", "pt_BR": "", "en": "", "es": ""},
]

merged, stats = merge_i18n(CSV_ROWS, EXISTING)

assert merged["pt_BR"]["city.name.X"] == "Cidade nova", merged["pt_BR"]
assert merged["en"]["city.name.X"] == "New city", merged["en"]
assert merged["es"]["city.name.X"] == "Ciudad nueva", merged["es"]
assert merged["pt_BR"]["port.name.Y"] == "Porto", merged["pt_BR"]
# chave que só existe no JSON sobrevive
assert merged["en"]["map.region.europe"] == "Europe", merged["en"]
assert merged["es"]["map.region.europe"] == "Europa", merged["es"]
# linha de CSV vazia não apaga
assert merged["pt_BR"]["dock.services"] == "Serviços do cais", merged["pt_BR"]
# o dicionário de entrada não é mutado
assert EXISTING["pt_BR"]["city.name.X"] == "Cidade antiga", EXISTING
assert stats["from_csv"] == 3, stats
assert stats["preserved"] == 1, stats          # map.region.europe
assert stats["overridden"] == 1, stats         # city.name.X

# idempotência: mesclar o resultado de novo não muda nada
again, _ = merge_i18n(CSV_ROWS, merged)
assert again == merged, "mescla nao e idempotente"

print("OK: mescla de i18n preserva chaves do JSON e o CSV vence no conflito "
      f"({stats})")
