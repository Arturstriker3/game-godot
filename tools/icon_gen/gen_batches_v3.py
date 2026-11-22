#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Produção dos ícones v3 aprovados (OpenAI), em lotes temáticos.

Lê ``tools/icon_gen/batches_v3_production.json`` (ids + descrições em inglês,
já conferidos contra ``resource_icons.json`` e contra o pedido de construções) e
chama ``openai_grid.generate_grid`` um lote por vez, sempre com
``quality=medium``, ``background=transparent`` e a tag ``v3``.

Uso:

    python tools/icon_gen/gen_batches_v3.py --list
    python tools/icon_gen/gen_batches_v3.py --batch 1      # chama a API (1 lote)
    python tools/icon_gen/gen_batches_v3.py --check 1      # recortes + montagem
    python tools/icon_gen/gen_batches_v3.py --sheet        # folha HTML (fundo escuro)

Depois de cada lote o script:

- patcheia os JSONs de cada ícone com ``batch``/``batch_title`` (para a folha);
- anexa uma linha em ``%TEMP%/opengs_iconapi_prod_progress.txt``;
- reescreve ``tools/icon_gen/status_grid.json`` com ``lote i/n``;
- guarda o resumo da resposta em ``.cache/openai_grid/batches/batch_NN.json``.

O gasto acumulado vem de ``api_spend.json`` (ignorado pelo git). O script para
antes de chamar se o gasto já estiver em ``stop_usd`` (US$ 3,00, teto da tarefa)
e avisa se passar do teto duro do orçamento (US$ 4,00). A chave nunca é
impressa: o ``openai_grid`` mascara qualquer segredo.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import openai_grid as og  # noqa: E402

PLAN_PATH = og.ICON_GEN_DIR / "batches_v3_production.json"
PROGRESS_PATH = Path(os.environ.get("TEMP") or os.environ.get("TMP") or ".") / (
    "opengs_iconapi_prod_progress.txt"
)
BATCH_CACHE = og.CACHE_DIR / "batches"
REPLACED_DIR = og.CACHE_DIR / "replaced"
MONTAGE_PATH = og.CACHE_DIR / "montage"


def now() -> str:
    return datetime.now().astimezone().isoformat(timespec="seconds")


def load_plan(path: Path = PLAN_PATH) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def batches(plan: dict) -> list[dict]:
    return plan["batches"]


def batch_of(plan: dict, number: int) -> dict:
    for batch in batches(plan):
        if int(batch["n"]) == number:
            return batch
    raise SystemExit(f"lote {number} não existe em {PLAN_PATH.name}")


def append_progress(line: str) -> None:
    PROGRESS_PATH.parent.mkdir(parents=True, exist_ok=True)
    with PROGRESS_PATH.open("a", encoding="utf-8", newline="\n") as fh:
        fh.write(f"{now()} {line}\n")


def write_status(**fields) -> None:
    payload = {"updated_at": now(), **fields}
    og.STATUS_PATH.write_text(
        json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )


def v3_paths(icon_id: str) -> list[Path]:
    return og._variant_files(icon_id, 3)


def archive_existing(icon_id: str) -> str | None:
    """Move o v3 antigo (png, 64 e json) para .cache/replaced/ (não apaga)."""
    files = [p for p in v3_paths(icon_id) if p.exists()]
    if not files:
        return None
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    target = REPLACED_DIR / f"{icon_id}_v3_{stamp}"
    target.mkdir(parents=True, exist_ok=True)
    for path in files:
        path.replace(target / path.name)
    return str(target)


def patch_meta(icon_id: str, number: int, title: str, size: int) -> None:
    meta_path = og.GEN_API_DIR / icon_id / f"{icon_id}_v3.json"
    if not meta_path.is_file():
        raise SystemExit(f"meta ausente depois da geração: {meta_path}")
    meta = json.loads(meta_path.read_text(encoding="utf-8"))
    meta["batch"] = number
    meta["batch_title"] = title
    meta["batch_size"] = size
    meta_path.write_text(
        json.dumps(meta, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )


def print_summary(payload: dict) -> None:
    keys = (
        "ok",
        "error",
        "message",
        "cost_usd",
        "spend_total_usd",
        "headroom_usd",
        "sheet_image",
        "sheet_html",
        "background",
        "attempts",
    )
    brief = {k: payload.get(k) for k in keys if payload.get(k) not in (None, [], {})}
    if payload.get("ok") is False:
        for extra in ("error", "message"):
            if payload.get(extra):
                brief[extra] = payload[extra]
    print(json.dumps(brief, ensure_ascii=False, indent=2))


def run_batch(plan: dict, number: int) -> int:
    batch = batch_of(plan, number)
    items: dict[str, str] = batch["items"]
    ids = list(items)
    descs = [items[i] for i in ids]
    title = batch.get("title") or ""
    total = len(batches(plan))
    quality = plan.get("quality") or og.DEFAULT_QUALITY
    tag = int(plan.get("tag") or 3)
    refs = plan.get("refs") or list(og.REF_FILES)
    stop_usd = float(plan.get("stop_usd") or 3.0)

    spend = og.load_spend()
    spent = float(spend.get("total_usd") or 0.0)
    if spent >= stop_usd:
        msg = f"PARADO: gasto acumulado US$ {spent:.4f} já passou do teto da tarefa (US$ {stop_usd:.2f})"
        print(msg)
        append_progress(msg)
        write_status(phase="parado por orçamento", total_usd=spent, stop_usd=stop_usd)
        return 3

    already = [
        i
        for i in ids
        if i not in (plan.get("replace_existing") or [])
        and all(p.exists() for p in v3_paths(i))
    ]
    if already:
        msg = f"lote {number}: já existe v3 para {len(already)} ids; nada foi chamado"
        print(msg)
        write_status(phase="ocioso", batch=number, total_batches=total, note=msg)
        return 0

    for icon_id in plan.get("replace_existing") or []:
        if icon_id in items:
            moved = archive_existing(icon_id)
            if moved:
                print(f"v3 antigo de {icon_id} movido para {moved}")

    write_status(
        phase=f"lote {number}/{total}",
        batch=number,
        total_batches=total,
        title=title,
        ids=ids,
        stage="chamando API",
        total_usd=spent,
    )
    append_progress(f"LOTE {number}/{total} ({title}) iniciando: {len(ids)} ids")

    started = time.time()
    raw = og.generate_grid(
        ids=ids,
        descriptions=descs,
        quality=quality,
        variant=tag,
        refs=refs,
    )
    elapsed = time.time() - started
    payload = json.loads(raw)
    print_summary(payload)
    if not payload.get("ok"):
        append_progress(
            f"LOTE {number}/{total} ({title}) FALHOU: {payload.get('error')}: {payload.get('message')}"
        )
        write_status(
            phase="erro",
            batch=number,
            total_batches=total,
            title=title,
            error=payload.get("error"),
            message=payload.get("message"),
            total_usd=float(og.load_spend().get("total_usd") or 0.0),
        )
        return 2

    for icon_id in ids:
        patch_meta(icon_id, number, title, len(ids))
    for icon_id, info in (batch.get("restore") or {}).items():
        meta_path = og.GEN_API_DIR / icon_id / f"{icon_id}_v3.json"
        meta = json.loads(meta_path.read_text(encoding="utf-8"))
        meta.update(info)
        meta_path.write_text(
            json.dumps(meta, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
        )

    BATCH_CACHE.mkdir(parents=True, exist_ok=True)
    summary_path = BATCH_CACHE / f"batch_{number:02d}.json"
    summary_path.write_text(
        json.dumps(
            {
                "batch": number,
                "title": title,
                "ids": ids,
                "descriptions": items,
                "elapsed_s": round(elapsed, 1),
                "sheet_image": payload.get("sheet_image"),
                "cost_usd": payload.get("cost_usd"),
                "spend_total_usd": payload.get("spend_total_usd"),
                "background": payload.get("background"),
                "created_at": now(),
            },
            ensure_ascii=False,
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )

    total_usd = float(payload.get("spend_total_usd") or 0.0)
    append_progress(
        f"LOTE {number}/{total} ({title}) ok: {len(ids)}/{len(ids)} "
        f"custo US$ {float(payload.get('cost_usd') or 0):.4f} acumulado US$ {total_usd:.4f} "
        f"({elapsed:.0f}s)"
    )
    write_status(
        phase=f"lote {number}/{total}",
        batch=number,
        total_batches=total,
        title=title,
        ids=ids,
        stage="concluído",
        last_cost_usd=payload.get("cost_usd"),
        total_usd=total_usd,
        stop_usd=stop_usd,
        batch_summary=str(summary_path),
    )
    if total_usd > stop_usd:
        msg = f"PARADO: gasto acumulado US$ {total_usd:.4f} passou de US$ {stop_usd:.2f}"
        print(msg)
        append_progress(msg)
        return 3
    return 0


def check_batch(plan: dict, number: int) -> int:
    """Confere os recortes do lote: vazios, colados na borda, fundo sujo."""
    from PIL import Image, ImageChops

    batch = batch_of(plan, number)
    ids = list(batch["items"])
    first_meta = og.GEN_API_DIR / ids[0] / f"{ids[0]}_v3.json"
    if not first_meta.is_file():
        raise SystemExit(f"sem meta v3 para {ids[0]}; rode --batch {number} antes")
    meta = json.loads(first_meta.read_text(encoding="utf-8"))
    sheet_path = Path(meta.get("sheet_image") or "")
    if not sheet_path.is_file():
        raise SystemExit(f"folha bruta ausente: {sheet_path}")

    with Image.open(sheet_path) as im:
        sheet = im.convert("RGBA")
    cells = og._split_grid(sheet)
    cw, ch = cells[0].width, cells[0].height
    rows: list[dict] = []
    flags = 0
    for idx, icon_id in enumerate(ids):
        cell = cells[idx]
        alpha = cell.getchannel("A")
        # Objeto visível: alpha >= 40. Abaixo disso é sombra/anti-aliasing suave,
        # que existe de propósito no estilo (não conta como "cortado").
        vis = alpha.point(lambda v: 255 if v >= 40 else 0)
        bbox = vis.getbbox()
        hist = alpha.histogram()
        if bbox is None or sum(hist[40:]) < 40:
            rows.append({"id": icon_id, "empty": True, "cell": idx + 1})
            flags += 1
            continue
        left, top, right, bottom = bbox
        margins = {
            "left": left,
            "top": top,
            "right": cw - right,
            "bottom": ch - bottom,
        }
        # Conta pixels visíveis (alpha >= 40) encostados em cada borda; menos de
        # 8 px é ruído, não corte.
        edges = {
            "left": vis.crop((0, 0, 2, ch)),
            "right": vis.crop((cw - 2, 0, cw, ch)),
            "top": vis.crop((0, 0, cw, 2)),
            "bottom": vis.crop((0, ch - 2, cw, ch)),
        }
        cut = [
            side
            for side, value in margins.items()
            if value <= 1 and sum(edges[side].histogram()[128:]) >= 8
        ]
        coverage = sum(vis.histogram()[128:]) / max(1, cw * ch)
        # Halo: alpha 8..39 fora da caixa do objeto (mancha de fundo visível).
        halo_box = (
            max(0, left - 6),
            max(0, top - 6),
            min(cw, right + 6),
            min(ch, bottom + 6),
        )
        halo_px = 0
        ring = alpha.point(lambda v: 255 if 8 <= v < 40 else 0)
        mask = Image.new("L", (cw, ch), 255)
        mask.paste(0, halo_box)
        halo_px = sum(ImageChops.multiply(ring, mask).histogram()[128:])
        halo = halo_px / max(1, cw * ch)
        issue = []
        if cut:
            issue.append("cortado:" + ",".join(cut))
        if coverage < 0.02:
            issue.append(f"minúsculo:{coverage:.3f}")
        if coverage > 0.98:
            issue.append(f"cheio:{coverage:.3f}")
        if halo > 0.005:
            issue.append(f"halo:{halo:.3f}")
        if issue:
            flags += 1
        rows.append(
            {
                "id": icon_id,
                "cell": idx + 1,
                "coverage": round(coverage, 3),
                "margins": margins,
                "halo": round(halo, 4),
                "issues": issue,
            }
        )

    MONTAGE_PATH.mkdir(parents=True, exist_ok=True)
    tile = 128
    gap = 8
    pad = 8
    size = pad * 2 + tile * 4 + gap * 3
    montage = Image.new("RGBA", (size, size), (23, 25, 28, 255))
    for idx, icon_id in enumerate(ids):
        tile_path = og.GEN_API_DIR / icon_id / f"{icon_id}_v3.png"
        if not tile_path.is_file():
            continue
        with Image.open(tile_path) as icon:
            icon = icon.convert("RGBA")
        x = pad + (idx % 4) * (tile + gap)
        y = pad + (idx // 4) * (tile + gap)
        montage.alpha_composite(icon, (x, y))
    out = MONTAGE_PATH / f"batch_{number:02d}.png"
    montage.save(out)

    print(
        json.dumps(
            {
                "batch": number,
                "title": batch.get("title"),
                "sheet": str(sheet_path),
                "montage": str(out),
                "cells": len(ids),
                "flags": flags,
                "empty": [r["id"] for r in rows if r.get("empty")],
                "issues": [r for r in rows if r.get("issues")],
            },
            ensure_ascii=False,
            indent=2,
        )
    )
    if flags:
        write_status(
            phase=f"lote {number}/{len(batches(plan))}",
            batch=number,
            title=batch.get("title"),
            stage="conferência com falhas",
            flagged=[r["id"] for r in rows if r.get("issues") or r.get("empty")],
        )
    return 0


def meta_entries() -> list[dict]:
    entries: list[dict] = []
    if not og.GEN_API_DIR.is_dir():
        return entries
    for id_dir in sorted(p for p in og.GEN_API_DIR.iterdir() if p.is_dir()):
        meta_path = id_dir / f"{id_dir.name}_v3.json"
        png = id_dir / f"{id_dir.name}_v3.png"
        if not meta_path.is_file() or not png.is_file():
            continue
        try:
            meta = json.loads(meta_path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        entries.append(
            {
                "id": id_dir.name,
                "label": meta.get("label") or "",
                "description": meta.get("description") or "",
                "cell": meta.get("cell") or 0,
                "batch": int(meta.get("batch") or 0),
                "batch_title": meta.get("batch_title") or "",
                "cost_usd": meta.get("cost_usd"),
                "png": png,
                "png_64": id_dir / f"{id_dir.name}_v3_64.png",
            }
        )
    return entries


def build_sheet(plan: dict) -> Path:
    import html as _html

    def esc(value: object) -> str:
        return _html.escape(str(value), quote=True)

    def rel(path: Path) -> str:
        return Path(os.path.relpath(path, og.SHEET_PATH.parent)).as_posix()

    entries = meta_entries()
    if not entries:
        raise SystemExit("nenhum v3 encontrado em gen_api")
    titles = {int(b["n"]): (b.get("title") or "") for b in batches(plan)}
    grouped: dict[int, list[dict]] = {}
    for entry in entries:
        grouped.setdefault(entry["batch"], []).append(entry)

    spend = og.load_spend()
    sections = []
    for number in sorted(grouped):
        items = sorted(grouped[number], key=lambda e: (int(e["cell"]), e["id"]))
        title = titles.get(number) or ("matérias-primas do lote 1 (aprovado)" if number == 0 else "")
        heading = f"Lote {number} — {title}" if number else "Lote 0 — matérias-primas aprovadas"
        cards = []
        for entry in items:
            cards.append(
                f'<figure class="card">'
                f'<div class="pics"><img class="big" src="{esc(rel(entry["png"]))}" '
                f'alt="{esc(entry["id"])}" loading="lazy">'
                f'<img class="small" src="{esc(rel(entry["png_64"]))}" '
                f'alt="{esc(entry["id"])} 64" loading="lazy"></div>'
                f'<figcaption><span class="id">{esc(entry["id"])}</span>'
                f'<span class="desc">{esc(entry["description"])}</span></figcaption>'
                f"</figure>"
            )
        sections.append(
            f'<section class="batch"><h2>{esc(heading)} '
            f'<span class="count">{len(items)} ícones</span></h2>'
            f'<div class="grid">{"".join(cards)}</div></section>'
        )

    total_cost = sum(float(e.get("cost_usd") or 0) for e in entries)
    html_text = f"""<!DOCTYPE html>
<html lang="pt-BR">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>OpenGS — ícones v3 (OpenAI, por lote)</title>
<style>
:root{{--bg:#141619;--panel:#1d2026;--line:#333a44;--ink:#e8e3d7;--muted:#9aa1ab;--brass:#c8a24a;}}
*{{box-sizing:border-box}}
body{{margin:0;padding:32px;background:var(--bg);color:var(--ink);
font:14px/1.45 system-ui,Segoe UI,Roboto,sans-serif}}
h1{{font-size:22px;margin:0 0 6px}}
h2{{font-size:16px;margin:0 0 12px;color:var(--brass);border-bottom:1px solid var(--line);padding-bottom:6px}}
.count{{color:var(--muted);font-weight:400;font-size:12px;margin-left:8px}}
p{{color:var(--muted);max-width:80ch;margin:0 0 8px}}
.batch{{margin:28px 0 0}}
.grid{{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:12px}}
.card{{margin:0;background:var(--panel);border:1px solid var(--line);border-radius:6px;padding:8px;
display:flex;flex-direction:column;gap:6px}}
.pics{{display:flex;align-items:flex-end;gap:6px}}
.pics img{{background:#101215;border:1px solid #262b33;border-radius:4px}}
.pics .big{{width:128px;height:128px}}
.pics .small{{width:64px;height:64px}}
.id{{display:block;font-weight:600;font-size:12px;word-break:break-word}}
.desc{{display:block;font-size:11px;color:var(--muted);line-height:1.3}}
code{{color:var(--brass)}}
</style>
</head>
<body>
<h1>OpenGS — ícones v3 gerados pela API da OpenAI</h1>
<p>Estilo v3 (aprovado pelo dono): semi-realista militar, materiais e texturas
reais, paleta dessaturada e suja, luz dura de cima-esquerda com sombra de
contato e contorno fino só para legibilidade. Referência
<code>ref_sheet_military_pack.png</code> usada apenas como enquadramento.
Construções aparecem como miniatura em vista 3/4, sem chão nem paisagem.</p>
<p>{len(entries)} ícones v3 • {len(grouped)} lotes •
gasto acumulado da API US$ {spend.get("total_usd")} de US$ {spend.get("budget_usd")} •
custo atribuído aos ícones desta folha US$ {round(total_cost, 4)}.
PNGs em <code>assets/icons/illustrated_source/gen_api/</code> (ignorados pelo git).</p>
{"".join(sections)}
</body>
</html>
"""
    og.SHEET_PATH.parent.mkdir(parents=True, exist_ok=True)
    og.SHEET_PATH.write_text(html_text, encoding="utf-8", newline="\n")
    return og.SHEET_PATH


def list_plan(plan: dict) -> int:
    spend = og.load_spend()
    print(
        json.dumps(
            {
                "plan": str(PLAN_PATH),
                "batches": [
                    {
                        "n": b["n"],
                        "title": b.get("title"),
                        "size": len(b["items"]),
                        "first": next(iter(b["items"])),
                        "last": next(reversed(b["items"])),
                    }
                    for b in batches(plan)
                ],
                "total_icons": sum(len(b["items"]) for b in batches(plan)),
                "spent_usd": spend.get("total_usd"),
                "stop_usd": plan.get("stop_usd"),
                "budget_usd": spend.get("budget_usd"),
            },
            ensure_ascii=False,
            indent=2,
        )
    )
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Produção dos ícones v3 em lotes de 16")
    parser.add_argument("--plan", default=str(PLAN_PATH), help="JSON com os lotes")
    parser.add_argument("--batch", type=int, help="gera o lote N (1-based)")
    parser.add_argument("--check", type=int, help="confere os recortes do lote N")
    parser.add_argument("--sheet", action="store_true", help="reescreve a folha HTML")
    parser.add_argument("--list", action="store_true", help="lista os lotes e o gasto")
    args = parser.parse_args(argv)

    plan = load_plan(Path(args.plan))
    if args.list:
        return list_plan(plan)
    if args.check:
        return check_batch(plan, args.check)
    if args.sheet:
        path = build_sheet(plan)
        print(json.dumps({"ok": True, "sheet": str(path)}, ensure_ascii=False))
        return 0
    if args.batch:
        return run_batch(plan, args.batch)
    parser.print_help()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
