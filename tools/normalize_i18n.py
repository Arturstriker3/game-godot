"""Normaliza o i18n.csv: aspas nos valores com vírgula e remove chaves duplicadas.

Linhas com vírgula sem aspas são lidas por csv.DictReader como colunas extras,
corrompendo o i18n.json. Como as três traduções são paralelas (mesmo número de
vírgulas), reconstruímos cada valor juntando os fragmentos.

Uso: python tools/normalize_i18n.py
"""

import csv
import io

PATH = r"G:/projetos/opengs-game/data/definitions/i18n/i18n.csv"

raw = io.open(PATH, encoding="utf-8-sig", newline="").read()
lines = raw.splitlines()
header = lines[0]
out = [header]
seen = {}
skipped = 0
for line in lines[1:]:
    if not line.strip():
        continue
    fields = next(csv.reader([line]))
    key = fields[0]
    if key in seen:
        skipped += 1
        continue
    seen[key] = True
    n = len(fields) - 1
    if n == 3:
        out.append(line)  # linha já correta
        continue
    if n <= 0 or n % 3 != 0:
        out.append(line)  # não sabemos reconstruir; mantém
        continue
    k = n // 3
    parts = [",".join(fields[1 + i * k:1 + (i + 1) * k]) for i in range(3)]
    out.append('%s,"%s","%s","%s"' % tuple([key] + [p.replace('"', '""') for p in parts]))

io.open(PATH, "w", encoding="utf-8", newline="\n").write("\n".join(out) + "\n")
print("linhas normalizadas:", len(out) - 1, "| duplicadas removidas:", skipped)
