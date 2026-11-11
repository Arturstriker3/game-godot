#!/usr/bin/env python3
"""Icon Forge — gera os ícones do OpenGS a partir de uma especificação JSON.

Sem LLM e sem dependências (só a biblioteca padrão do Python).
Um agente (ou você) descreve o ícone com PRIMITIVAS num JSON; o script aplica
o estilo da casa e escreve SVGs brancos 24x24 prontos para o Godot tingir.

Uso:
  python tools/icon_forge/icon_forge.py build  [spec.json] [--out assets/icons/resources]
  python tools/icon_forge/icon_forge.py check  [spec.json]     # valida regras de estilo
  python tools/icon_forge/icon_forge.py sheet  [spec.json] [--out docs/ui/icons_sheet.html]
  python tools/icon_forge/icon_forge.py new    <chave>          # imprime um esqueleto de spec

Formato da spec (ver resource_icons.json):
{
  "style": {...opcional, sobrescreve STYLE...},
  "icons": {
    "coal": {"label": "Carvão", "color": "#3E3B38", "layers": [
        {"el": "path", "d": "M3 19 ...", "mode": "fill"},
        {"el": "path", "d": "M7 9.5 ...", "mode": "stroke"},
        {"el": "path", "d": "M9 15 ...", "mode": "cut"}
    ]}
  }
}

Camadas ("layers"), desenhadas na ordem:
  el:   "path" (d) | "circle" (cx,cy,r) | "ellipse" (cx,cy,rx,ry) | "rect" (x,y,w,h,rx?) | "polygon" (points)
  mode: "fill"    -> forma sólida branca
        "stroke"  -> contorno branco (largura = style.stroke, ou "w")
        "cut"     -> linha VAZADA (recorte transparente sobre as formas; largura style.cut ou "w")
        "cutfill" -> área VAZADA sólida (furos, sementes, olhos da batata)
  rot:  [ângulos] -> repete a camada girando em torno do centro (12,12) ou de "origin":[x,y]
  transform: string SVG opcional (ex.: "rotate(-30 8 9.5)")

Regras da casa (o comando check verifica):
  - viewBox 0 0 24 24; desenho dentro da área segura 1.5..22.5
  - no máx. 1 silhueta principal + 3 detalhes; detalhes internos são "cut" (vazados),
    nunca outra cor — o Godot tinge o branco, então só existe UMA cor + transparência
  - traço 1.8 (contornos), recorte 1.1–1.4; nada abaixo de 1.0 (some em 16 px)
  - preferir silhueta cheia ("fill") a contornos: lê melhor pequeno
"""
from __future__ import annotations
import json, math, re, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_SPEC = HERE / "resource_icons.json"

STYLE = {
    "size": 24,
    "stroke": 1.8,      # contornos
    "cut": 1.2,         # recortes internos
    "safe_min": 1.5,
    "safe_max": 22.5,
    "min_width": 1.0,
    "max_layers": 8,
    "color": "#fff",
}

NUM = re.compile(r"-?\d*\.?\d+(?:e-?\d+)?")


def _attrs(layer: dict, style: dict) -> str:
    el = layer.get("el", "path")
    if el == "path":
        geo = f'd="{layer["d"]}"'
    elif el == "circle":
        geo = f'cx="{layer["cx"]}" cy="{layer["cy"]}" r="{layer["r"]}"'
    elif el == "ellipse":
        geo = f'cx="{layer["cx"]}" cy="{layer["cy"]}" rx="{layer["rx"]}" ry="{layer["ry"]}"'
    elif el == "rect":
        geo = f'x="{layer["x"]}" y="{layer["y"]}" width="{layer["w_"] if "w_" in layer else layer["width"]}" height="{layer["height"]}"'
        if "rx" in layer:
            geo += f' rx="{layer["rx"]}"'
    elif el == "polygon":
        geo = f'points="{layer["points"]}"'
    else:
        raise ValueError(f"primitiva desconhecida: {el}")
    return geo


def _element(layer: dict, style: dict, ink: str) -> list[str]:
    el = layer.get("el", "path")
    mode = layer.get("mode", "fill")
    geo = _attrs(layer, style)
    if mode in ("fill", "cutfill"):
        paint = f'fill="{ink}"'
    elif mode in ("stroke", "cut"):
        width = layer.get("w", style["stroke"] if mode == "stroke" else style["cut"])
        paint = (f'fill="none" stroke="{ink}" stroke-width="{width}" '
                 'stroke-linecap="round" stroke-linejoin="round"')
    else:
        raise ValueError(f"mode desconhecido: {mode}")
    out = []
    angles = layer.get("rot") or [None]
    ox, oy = layer.get("origin", [12, 12])
    for a in angles:
        t = layer.get("transform", "")
        if a is not None:
            t = (t + f" rotate({a} {ox} {oy})").strip()
        tr = f' transform="{t}"' if t else ""
        out.append(f"<{el} {geo} {paint}{tr}/>")
    return out


def build_svg(icon: dict, style: dict) -> str:
    base, cuts = [], []
    for layer in icon["layers"]:
        mode = layer.get("mode", "fill")
        if mode in ("cut", "cutfill"):
            cuts += _element(layer, style, "#000")
        else:
            base += _element(layer, style, style["color"])
    s = style["size"]
    head = f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {s} {s}" width="{s}" height="{s}">'
    if not cuts:
        return head + "".join(base) + "</svg>\n"
    mask = (f'<defs><mask id="m" maskUnits="userSpaceOnUse" x="0" y="0" width="{s}" height="{s}">'
            f'<rect width="{s}" height="{s}" fill="#fff"/>{"".join(cuts)}</mask></defs>')
    return head + mask + f'<g mask="url(#m)">{"".join(base)}</g></svg>\n'


def _coords(layer: dict) -> list[float]:
    el = layer.get("el", "path")
    if el == "path":
        return [float(n) for n in NUM.findall(layer["d"])]
    if el == "polygon":
        return [float(n) for n in NUM.findall(layer["points"])]
    if el == "circle":
        return [layer["cx"] - layer["r"], layer["cy"] - layer["r"], layer["cx"] + layer["r"], layer["cy"] + layer["r"]]
    if el == "ellipse":
        return [layer["cx"] - layer["rx"], layer["cy"] - layer["ry"], layer["cx"] + layer["rx"], layer["cy"] + layer["ry"]]
    if el == "rect":
        w = layer.get("width", layer.get("w_", 0))
        return [layer["x"], layer["y"], layer["x"] + w, layer["y"] + layer["height"]]
    return []


def check(spec: dict, style: dict) -> list[str]:
    """Regras simples e determinísticas. Retorna lista de problemas."""
    problems = []
    for key, icon in spec["icons"].items():
        layers = icon.get("layers", [])
        if not layers:
            problems.append(f"{key}: sem camadas")
            continue
        if not any(l.get("mode", "fill") in ("fill", "stroke") for l in layers):
            problems.append(f"{key}: só tem recortes; precisa de uma forma base")
        if len(layers) > style["max_layers"]:
            problems.append(f"{key}: {len(layers)} camadas (máx. {style['max_layers']}) — simplifique")
        for i, layer in enumerate(layers):
            if layer.get("mode") in ("stroke", "cut") and layer.get("w", 9) < style["min_width"]:
                problems.append(f"{key}[{i}]: traço {layer['w']} < {style['min_width']} some em 16px")
            # coordenadas absolutas (comandos minúsculos são relativos — não checamos)
            if layer.get("el", "path") == "path" and re.search(r"[a-z]", re.sub(r"[eE]", "", layer["d"])):
                continue
            nums = _coords(layer)
            if nums and not layer.get("rot") and not layer.get("transform"):
                lo, hi = min(nums), max(nums)
                if lo < style["safe_min"] - 0.01 or hi > style["safe_max"] + 0.01:
                    problems.append(f"{key}[{i}]: sai da área segura ({lo:.1f}..{hi:.1f})")
        if "label" not in icon:
            problems.append(f"{key}: falta 'label' (nome pt-BR para a folha de revisão)")
    return problems


def sheet(spec: dict, style: dict, out: Path) -> None:
    """Folha HTML de revisão: cada ícone em 48/24/16 px, na tinta e num chip de cor."""
    import base64
    cells = []
    for key, icon in spec["icons"].items():
        uri = "data:image/svg+xml;base64," + base64.b64encode(build_svg(icon, style).encode()).decode()
        def ic(sz, col="#2b2418"):
            return (f'<i style="display:inline-block;width:{sz}px;height:{sz}px;background:{col};'
                    f'-webkit-mask:url({uri}) center/contain no-repeat;mask:url({uri}) center/contain no-repeat"></i>')
        color = icon.get("color", "#999")
        chip = (f'<span style="display:inline-grid;place-items:center;width:20px;height:20px;border-radius:4px;'
                f'border:1px solid #2b241880;background:{color}59">{ic(15)}</span>')
        cells.append(f'<div class="c"><div class="b">{ic(48)}</div><div>{ic(24)} {ic(16)} {chip}</div>'
                     f'<b>{icon.get("label", key)}</b><small>{key}</small></div>')
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(
        '<!doctype html><meta charset="utf-8"><title>Icon Forge</title><style>'
        'body{background:#e6dbbd;font:13px monospace;color:#2b2418;padding:20px}'
        '.g{display:grid;grid-template-columns:repeat(auto-fill,minmax(130px,1fr));gap:14px}'
        '.c{display:flex;flex-direction:column;align-items:center;gap:6px;background:#efe6cc;border:1px solid #2b241844;padding:10px}'
        '.b{width:64px;height:64px;display:grid;place-items:center}small{opacity:.6}</style>'
        f'<h2>Icon Forge · {len(cells)} ícones</h2><div class="g">{"".join(cells)}</div>', encoding="utf-8")


def skeleton(key: str) -> str:
    return json.dumps({key: {"label": key.title(), "color": "#888888", "layers": [
        {"el": "path", "d": "M4 20 L12 4 L20 20 Z", "mode": "fill"},
        {"el": "path", "d": "M8 16 H16", "mode": "cut"}]}}, ensure_ascii=False, indent=2)


def main(argv: list[str]) -> int:
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return 0
    cmd, args = argv[0], argv[1:]
    if cmd == "new":
        print(skeleton(args[0] if args else "novo"))
        return 0
    out = None
    if "--out" in args:
        i = args.index("--out")
        out = Path(args[i + 1])
        args = args[:i] + args[i + 2:]
    spec_path = Path(args[0]) if args else DEFAULT_SPEC
    spec = json.loads(spec_path.read_text(encoding="utf-8"))
    style = {**STYLE, **spec.get("style", {})}
    if cmd == "check":
        problems = check(spec, style)
        for p in problems:
            print("PROBLEMA:", p)
        print(f"{len(spec['icons'])} ícones, {len(problems)} problema(s)")
        return 1 if problems else 0
    if cmd == "build":
        problems = check(spec, style)
        for p in problems:
            print("aviso:", p)
        out = out or Path("assets/icons/resources")
        out.mkdir(parents=True, exist_ok=True)
        for key, icon in spec["icons"].items():
            (out / f"{key}.svg").write_text(build_svg(icon, style), encoding="utf-8")
        print(f"{len(spec['icons'])} SVGs em {out}")
        return 0
    if cmd == "sheet":
        out = out or Path("docs/ui/icons_sheet.html")
        sheet(spec, style, out)
        print(f"folha de revisão: {out}")
        return 0
    print(f"comando desconhecido: {cmd}")
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
