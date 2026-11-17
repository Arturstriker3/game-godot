#!/usr/bin/env python3
"""Comparador de equivalência funcional A1-A5 (docs/plans/perf_pop_financas_plano.md).

Compara dois dumps gravados por `tools/world_equivalence.gd` campo a campo e
relata o PRIMEIRO caminho divergente com o valor de cada lado, depois até
--max-diffs divergências (com a contagem total no fim).

    python tools/world_equivalence_compare.py A.json B.json [--tolerance 0] [--max-diffs 20]

- Caminho legível: `save.world.cities[12].markets[3].price`.
- Tipos diferentes são divergência: `1` (int) e `1.0` (float) não batem, assim
  como `true` e `1`. Isso é intencional: uma mudança de tipo no save já quebra
  o contrato de dados, mesmo com o mesmo valor numérico.
- Floats: `|a-b| <= tolerance`; com tolerance 0 exige igualdade exata (0.0 e
  -0.0 são iguais; NaN nunca é igual a NaN).
- `--ignore` (opcional, repetível) pula caminhos voláteis, p.ex.
  `--ignore meta.generated_utc` (timestamp de parede, não faz parte do mundo).
  Um prefixo ignora o ramo inteiro (`--ignore meta`).

Saída: 0 = idênticos, 1 = há divergência, 2 = erro de leitura/argumentos.
Só stdlib; o texto sai em UTF-8.
"""

import argparse
import json
import math
import re
import sys
from typing import Any, List, Tuple

MAX_VALUE_CHARS = 120
MISSING = "<ausente>"
_IDENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


def short(value: Any) -> str:
    """repr curto e estável (<= 120 chars) para caber numa linha de relatório."""
    try:
        text = repr(value)
    except Exception:  # nunca deixe o relatório cair por causa de um valor
        text = "<repr falhou>"
    text = " ".join(text.split())
    if len(text) > MAX_VALUE_CHARS:
        text = text[: MAX_VALUE_CHARS - 3] + "..."
    return text


def kind_name(value: Any) -> str:
    if value is None:
        return "null"
    if isinstance(value, bool):
        return "bool"
    if isinstance(value, int):
        return "int"
    if isinstance(value, float):
        return "float"
    if isinstance(value, str):
        return "str"
    if isinstance(value, list):
        return "list"
    if isinstance(value, dict):
        return "dict"
    return type(value).__name__


def join_key(path: str, key: str) -> str:
    if _IDENT.match(key):
        return f"{path}.{key}" if path else key
    return f"{path}[{key!r}]" if path else f"[{key!r}]"


def join_index(path: str, index: int) -> str:
    return f"{path}[{index}]"


class DiffCollector:
    def __init__(self, tolerance: float, max_diffs: int, ignored: List[str]) -> None:
        self.tolerance = tolerance
        self.max_diffs = max_diffs
        self.ignored = [item for item in ignored if item]
        self.total = 0
        self.reported: List[Tuple[str, str, str]] = []  # (caminho, A, B)

    def ignored_path(self, path: str) -> bool:
        for prefix in self.ignored:
            if path == prefix or path.startswith(prefix + ".") or path.startswith(prefix + "["):
                return True
        return False

    def add(self, path: str, a: Any, b: Any) -> None:
        self.total += 1
        if len(self.reported) < self.max_diffs:
            self.reported.append((path, short(a), short(b)))

    def walk(self, a: Any, b: Any, path: str) -> None:
        if self.ignored_path(path):
            return

        ka = kind_name(a)
        kb = kind_name(b)
        if ka != kb:
            self.add(path, a, b)
            return

        if ka == "dict":
            self._walk_dict(a, b, path)
        elif ka == "list":
            self._walk_list(a, b, path)
        elif ka == "float":
            self._walk_float(a, b, path)
        else:
            if a != b:  # int / str / bool / null
                self.add(path, a, b)

    def _walk_float(self, a: float, b: float, path: str) -> None:
        if math.isnan(a) or math.isnan(b):
            # NaN != NaN: divergência mesmo quando os dois lados imprimem "nan".
            self.total += 1
            if len(self.reported) < self.max_diffs:
                self.reported.append((path, "<NaN>" if math.isnan(a) else short(a),
                                      "<NaN>" if math.isnan(b) else short(b)))
            return
        if a == b:  # cobre +0.0 == -0.0 e inf == inf
            return
        if math.isinf(a) or math.isinf(b):
            self.add(path, a, b)
            return
        if abs(a - b) <= self.tolerance:
            return
        self.add(path, a, b)

    def _walk_dict(self, a: dict, b: dict, path: str) -> None:
        for key in a:
            sub = join_key(path, str(key))
            if key in b:
                self.walk(a[key], b[key], sub)
            else:
                self.add(sub, a[key], MISSING)
        for key in b:
            if key not in a:
                self.add(join_key(path, str(key)), MISSING, b[key])

    def _walk_list(self, a: list, b: list, path: str) -> None:
        if len(a) != len(b):
            self.add(path, f"<lista com {len(a)} itens>", f"<lista com {len(b)} itens>")
        for index in range(min(len(a), len(b))):
            self.walk(a[index], b[index], join_index(path, index))


def load_json(path: str) -> Any:
    # utf-8-sig aceita arquivo com e sem BOM (editores Windows gravam BOM;
    # JSON.stringify do Godot nao). Sem BOM o comportamento e identico a utf-8.
    with open(path, "r", encoding="utf-8-sig") as handle:
        return json.load(handle)


def main(argv: List[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Compara dois dumps de tools/world_equivalence.gd campo a campo.")
    parser.add_argument("a", help="JSON do lado A (baseline)")
    parser.add_argument("b", help="JSON do lado B (nova execução)")
    parser.add_argument("--tolerance", type=float, default=0.0,
                        help="tolerância absoluta para floats (0 = igualdade exata)")
    parser.add_argument("--max-diffs", type=int, default=20,
                        help="máximo de divergências relatadas (contagem total sempre sai)")
    parser.add_argument("--ignore", action="append", default=[],
                        help="caminho volátil a pular (repetível), p.ex. meta.generated_utc")
    args = parser.parse_args(argv)

    if args.tolerance < 0:
        print("[equiv] erro: --tolerance nao pode ser negativo", file=sys.stderr)
        return 2
    if args.max_diffs < 1:
        print("[equiv] erro: --max-diffs deve ser >= 1", file=sys.stderr)
        return 2

    try:
        data_a = load_json(args.a)
    except (OSError, ValueError) as exc:
        print(f"[equiv] erro ao ler {args.a}: {exc}", file=sys.stderr)
        return 2
    try:
        data_b = load_json(args.b)
    except (OSError, ValueError) as exc:
        print(f"[equiv] erro ao ler {args.b}: {exc}", file=sys.stderr)
        return 2

    print(f"[equiv] comparando {args.a} vs {args.b} | tolerancia={args.tolerance:g}")

    collector = DiffCollector(args.tolerance, args.max_diffs, args.ignore)
    collector.walk(data_a, data_b, "")

    if collector.total == 0:
        print("[equiv] 0 diferenças")
        return 0

    for order, (path, value_a, value_b) in enumerate(collector.reported, start=1):
        suffix = " (primeira)" if order == 1 else ""
        print(f"[equiv] divergencia {order}: {path if path else '<raiz>'}{suffix}")
        print(f"[equiv]   A = {value_a}")
        print(f"[equiv]   B = {value_b}")
    if collector.total > len(collector.reported):
        print(f"[equiv] ... {collector.total - len(collector.reported)} divergencias nao listadas")
    first_path = collector.reported[0][0] or "<raiz>"
    plural = "diferença" if collector.total == 1 else "diferenças"
    print(f"[equiv] total: {collector.total} {plural} (primeira em {first_path})")
    return 1


if __name__ == "__main__":
    if hasattr(sys.stdout, "reconfigure"):
        try:
            sys.stdout.reconfigure(encoding="utf-8")
            sys.stderr.reconfigure(encoding="utf-8")
        except Exception:
            pass
    sys.exit(main(sys.argv[1:]))
