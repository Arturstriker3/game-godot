#!/usr/bin/env python3
"""Avaliação objetiva das capturas do mapa e ponte opcional para modelo com visão.

Duas camadas, explicitamente separadas:

1. ``pixel-metrics`` (sempre disponível): medidas determinísticas calculadas com
   numpy/PIL — brilho, contraste, saturação, colorfulness, nitidez (variância do
   laplaciano), densidade de bordas, distribuição por região, paleta dominante e
   comparação entre duas imagens (MAE, RMSE, SSIM). Isso NÃO é inspeção visual:
   são estatísticas de pixels.

2. ``vision-model`` (opcional): envia a imagem a um modelo multimodal compatível
   com a API OpenAI (``/chat/completions``) e pede diagnósticos objetivos sobre
   cores, iluminação, nitidez, relevo, escala e legibilidade. Só é marcado como
   inspeção visual quando um modelo realmente responde.

Configuração do modelo visual (nunca versionar a chave):

    VISION_API_BASE=https://api.openai.com/v1
    VISION_API_KEY=...
    VISION_MODEL=gpt-4o-mini

Uso:
    python tools/vision_review.py --images docs/map_visual/baseline_before/*.png \
        --out docs/map_visual/baseline_before/metrics.json \
        --markdown docs/map_visual/baseline_before/metrics.md
"""
from __future__ import annotations

import argparse
import base64
import glob
import io
import json
import os
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

import numpy as np
from PIL import Image

VISION_PROMPT = """Você é um revisor visual de mapas de Grand Strategy.
Analise a imagem enviada e produza apenas JSON válido, sem texto fora do JSON.

Formato obrigatório:
{
  "overall_score": 1-5,
  "colors": {"score": 1-5, "natural": true/false, "issues": ["..."]},
  "lighting": {"score": 1-5, "issues": ["..."]},
  "sharpness": {"score": 1-5, "issues": ["..."]},
  "relief": {"score": 1-5, "issues": ["..."]},
  "scale": {"score": 1-5, "issues": ["..."]},
  "legibility": {"score": 1-5, "issues": ["..."]},
  "problems": ["problemas concretos observáveis"],
  "top_fixes": ["correções priorizadas"]
}

Avalie apenas o que é visível: oceanos, continentes, relevo, fronteiras,
cidades, contraste e leitura de rótulos. Não invente elementos ausentes."""


def _vision_config() -> dict:
    base = os.environ.get("VISION_API_BASE", "").strip()
    key = os.environ.get("VISION_API_KEY", "").strip()
    model = os.environ.get("VISION_MODEL", "").strip()
    return {
        "configured": bool(base and key and model),
        "api_base": base,
        "model": model,
        "api_key": key,
    }


def _load_rgb(path: Path) -> np.ndarray:
    image = Image.open(path).convert("RGB")
    return np.asarray(image, dtype=np.float32) / 255.0


def _box_mean(values: np.ndarray, radius: int) -> np.ndarray:
    """Média em janela quadrada usando soma integral (rápida e sem scipy)."""
    padded = np.pad(values, radius, mode="edge")
    integral = padded.cumsum(0).cumsum(1)
    integral = np.pad(integral, ((1, 0), (1, 0)), mode="constant")
    window = 2 * radius + 1
    total = (
        integral[window:, window:]
        - integral[:-window, window:]
        - integral[window:, :-window]
        + integral[:-window, :-window]
    )
    return total / float(window * window)


def _laplacian(image: np.ndarray) -> np.ndarray:
    return (
        -4.0 * image
        + np.roll(image, 1, 0)
        + np.roll(image, -1, 0)
        + np.roll(image, 1, 1)
        + np.roll(image, -1, 1)
    )


def _ssim(left: np.ndarray, right: np.ndarray) -> float:
    radius = 5
    mu_left = _box_mean(left, radius)
    mu_right = _box_mean(right, radius)
    sigma_left = _box_mean(left * left, radius) - mu_left * mu_left
    sigma_right = _box_mean(right * right, radius) - mu_right * mu_right
    sigma_both = _box_mean(left * right, radius) - mu_left * mu_right
    c1, c2 = 0.01 ** 2, 0.03 ** 2
    numerator = (2 * mu_left * mu_right + c1) * (2 * sigma_both + c2)
    denominator = (mu_left ** 2 + mu_right ** 2 + c1) * (sigma_left + sigma_right + c2)
    return float(np.mean(numerator / denominator))


def _class_stats(rgb: np.ndarray, luma: np.ndarray, saturation: np.ndarray,
                 mask: np.ndarray) -> dict | None:
    """Estatísticas de uma classe de pixels (água ou terra)."""
    if not bool(mask.any()):
        return None
    pixels = rgb[mask]
    mean = pixels.mean(axis=0)
    channels = [max(0, min(255, int(round(float(value) * 255.0)))) for value in mean]
    return {
        "hex": "#%02X%02X%02X" % tuple(channels),
        "mean_rgb": [round(float(value), 4) for value in mean],
        "saturation": round(float(saturation[mask].mean()), 4),
        "luminance": round(float(luma[mask].mean()), 4),
        "share": round(float(mask.mean()), 4),
    }


def measure(path: Path) -> dict:
    rgb = _load_rgb(path)
    height, width, _ = rgb.shape
    luma = 0.2126 * rgb[:, :, 0] + 0.7152 * rgb[:, :, 1] + 0.0722 * rgb[:, :, 2]

    maximum = rgb.max(axis=2)
    minimum = rgb.min(axis=2)
    saturation = np.where(maximum > 0.0, (maximum - minimum) / np.maximum(maximum, 1e-6), 0.0)

    red_green = rgb[:, :, 0] - rgb[:, :, 1]
    yellow_blue = 0.5 * (rgb[:, :, 0] + rgb[:, :, 1]) - rgb[:, :, 2]
    colorfulness = float(
        np.hypot(red_green.std(), yellow_blue.std())
        + 0.3 * np.hypot(red_green.mean(), yellow_blue.mean())
    )

    laplacian = _laplacian(luma)
    gradient_x = np.roll(luma, -1, 1) - np.roll(luma, 1, 1)
    gradient_y = np.roll(luma, -1, 0) - np.roll(luma, 1, 0)
    gradient = np.hypot(gradient_x, gradient_y)

    local_mean = _box_mean(luma, 8)
    local_contrast = np.sqrt(np.maximum(_box_mean((luma - local_mean) ** 2, 8), 0.0))

    regions = []
    for row in range(3):
        for column in range(3):
            y0 = (height * row) // 3
            y1 = (height * (row + 1)) // 3
            x0 = (width * column) // 3
            x1 = (width * (column + 1)) // 3
            block = luma[y0:y1, x0:x1]
            regions.append(round(float(block.mean()), 4))

    palette = Image.open(path).convert("RGB").quantize(colors=6)
    palette_colors = palette.getpalette()[: 6 * 3]
    counts = np.bincount(np.asarray(palette).ravel(), minlength=6)[:6]
    total = float(counts.sum()) or 1.0
    dominant = []
    for index in range(6):
        share = float(counts[index]) / total
        if share <= 0.01:
            continue
        color = palette_colors[index * 3 : index * 3 + 3]
        dominant.append({
            "hex": "#%02X%02X%02X" % tuple(color),
            "share": round(share, 4),
        })
    dominant.sort(key=lambda item: item["share"], reverse=True)

    # Água = canal azul dominante sobre vermelho/verde (mesma heurística do shader).
    dominance = np.maximum(rgb[:, :, 0], rgb[:, :, 1]) - rgb[:, :, 2]
    water_mask = dominance < 0.0

    return {
        "path": str(path).replace("\\", "/"),
        "width": width,
        "height": height,
        "megapixels": round(width * height / 1e6, 3),
        "luminance_mean": round(float(luma.mean()), 4),
        "luminance_std": round(float(luma.std()), 4),
        "contrast_p95_p5": round(float(np.percentile(luma, 95) - np.percentile(luma, 5)), 4),
        "shadow_clip_pct": round(float((luma <= 0.02).mean() * 100.0), 2),
        "highlight_clip_pct": round(float((luma >= 0.98).mean() * 100.0), 2),
        "saturation_mean": round(float(saturation.mean()), 4),
        "colorfulness": round(colorfulness, 4),
        "sharpness_laplacian": round(float(laplacian.var()), 6),
        "edge_density": round(float((gradient > 0.08).mean()), 4),
        "local_contrast_mean": round(float(local_contrast.mean()), 4),
        "regions_luminance": regions,
        "dominant_colors": dominant,
        "water": _class_stats(rgb, luma, saturation, water_mask),
        "land": _class_stats(rgb, luma, saturation, ~water_mask),
        "sharpness_land": round(float(laplacian[~water_mask].var()), 6)
        if bool((~water_mask).any()) else 0.0,
        "sharpness_water": round(float(laplacian[water_mask].var()), 6)
        if bool(water_mask.any()) else 0.0,
    }


def compare(left_path: Path, right_path: Path) -> dict:
    left_image = Image.open(left_path).convert("RGB")
    right_image = Image.open(right_path).convert("RGB")
    if left_image.size != right_image.size:
        right_image = right_image.resize(left_image.size, Image.LANCZOS)

    left = np.asarray(left_image, dtype=np.float32) / 255.0
    right = np.asarray(right_image, dtype=np.float32) / 255.0
    left_luma = 0.2126 * left[:, :, 0] + 0.7152 * left[:, :, 1] + 0.0722 * left[:, :, 2]
    right_luma = 0.2126 * right[:, :, 0] + 0.7152 * right[:, :, 1] + 0.0722 * right[:, :, 2]
    difference = left - right

    return {
        "left": str(left_path).replace("\\", "/"),
        "right": str(right_path).replace("\\", "/"),
        "mae": round(float(np.abs(difference).mean()), 5),
        "rmse": round(float(np.sqrt((difference ** 2).mean())), 5),
        "ssim": round(max(-1.0, min(1.0, _ssim(left_luma, right_luma))), 5),
        "luminance_delta": round(float(left_luma.mean() - right_luma.mean()), 5),
        "pixels_changed_pct": round(float((np.abs(difference).max(axis=2) > 0.08).mean() * 100.0), 2),
    }


def ask_vision_model(path: Path, config: dict) -> dict:
    """Envia a imagem a um modelo multimodal compatível com a API OpenAI."""
    image = Image.open(path).convert("RGB")
    image.thumbnail((1280, 1280), Image.LANCZOS)
    buffer = io.BytesIO()
    image.save(buffer, format="JPEG", quality=88)
    encoded = base64.b64encode(buffer.getvalue()).decode("ascii")

    payload = {
        "model": config["model"],
        "temperature": 0,
        "max_tokens": 1200,
        "messages": [
            {"role": "system", "content": "Responda sempre em JSON válido, em português."},
            {
                "role": "user",
                "content": [
                    {"type": "text", "text": VISION_PROMPT},
                    {
                        "type": "image_url",
                        "image_url": {
                            "url": "data:image/jpeg;base64," + encoded,
                            "detail": "high",
                        },
                    },
                ],
            },
        ],
    }

    request = urllib.request.Request(
        config["api_base"].rstrip("/") + "/chat/completions",
        data=json.dumps(payload).encode("utf-8"),
        headers={
            "Content-Type": "application/json",
            "Authorization": "Bearer " + config["api_key"],
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=120) as response:
            body = json.loads(response.read().decode("utf-8"))
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as error:
        return {"status": "error", "error": str(error)}

    try:
        content = body["choices"][0]["message"]["content"]
        if isinstance(content, list):
            content = "".join(part.get("text", "") for part in content)
        cleaned = content.strip()
        if cleaned.startswith("```"):
            cleaned = cleaned.split("```")[1]
            cleaned = cleaned[4:] if cleaned.lower().startswith("json") else cleaned
        return {"status": "ok", "model": config["model"], "diagnostics": json.loads(cleaned)}
    except (KeyError, IndexError, json.JSONDecodeError) as error:
        return {"status": "error", "error": "resposta inesperada: %s" % error, "raw": body}


def _write_markdown(path: Path, report: dict) -> None:
    lines = [
        "# Métricas objetivas das capturas",
        "",
        "Método: `%s`. Inspeção visual por modelo: **%s**." % (
            report["method"],
            "sim (%s)" % report["vision_model"] if report["visual_inspection"] else "não",
        ),
        "",
        "> Estas são estatísticas de pixels, não julgamento estético.",
        "",
        "| imagem | brilho | contraste | saturação | nitidez | bordas |",
        "| --- | --- | --- | --- | --- | --- |",
    ]
    for name, metrics in report["images"].items():
        lines.append("| %s | %.3f | %.3f | %.3f | %.5f | %.3f |" % (
            name,
            metrics["luminance_mean"],
            metrics["contrast_p95_p5"],
            metrics["saturation_mean"],
            metrics["sharpness_laplacian"],
            metrics["edge_density"],
        ))

    lines += [
        "",
        "## Classes de pixels (água × terra)",
        "",
        "| imagem | água | sat. água | lum. água | hex água | sat. terra | lum. terra |",
        "| --- | --- | --- | --- | --- | --- | --- |",
    ]
    for name, metrics in report["images"].items():
        water = metrics.get("water") or {}
        land = metrics.get("land") or {}
        lines.append("| %s | %.3f | %.3f | %.3f | %s | %.3f | %.3f |" % (
            name,
            water.get("share", 0.0),
            water.get("saturation", 0.0),
            water.get("luminance", 0.0),
            water.get("hex", "-"),
            land.get("saturation", 0.0),
            land.get("luminance", 0.0),
        ))

    if report["comparisons"]:
        lines += [
            "",
            "## Comparações",
            "",
            "| A | B | MAE | RMSE | SSIM | Δbrilho | pixels alterados |",
            "| --- | --- | --- | --- | --- | --- | --- |",
        ]
        for item in report["comparisons"]:
            lines.append("| %s | %s | %.4f | %.4f | %.4f | %.4f | %.1f%% |" % (
                Path(item["left"]).name,
                Path(item["right"]).name,
                item["mae"],
                item["rmse"],
                item["ssim"],
                item["luminance_delta"],
                item["pixels_changed_pct"],
            ))

    if report.get("vision_diagnostics"):
        lines += ["", "## Diagnósticos do modelo visual", ""]
        for name, result in report["vision_diagnostics"].items():
            lines.append("### %s" % name)
            lines.append("")
            lines.append("```json")
            lines.append(json.dumps(result, ensure_ascii=False, indent=2))
            lines.append("```")
            lines.append("")

    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--images", nargs="+", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--markdown", default="")
    parser.add_argument("--compare", nargs=2, action="append", default=[],
                        help="par de imagens (A B) para comparar")
    parser.add_argument("--vision", action="store_true",
                        help="também pedir diagnóstico ao modelo multimodal")
    args = parser.parse_args()

    paths: list[Path] = []
    for pattern in args.images:
        paths.extend(Path(item) for item in glob.glob(pattern))
    paths = sorted({path for path in paths if path.suffix.lower() == ".png"})
    if not paths:
        raise SystemExit("nenhuma imagem encontrada")

    config = _vision_config()
    report = {
        "method": "pixel-metrics",
        "visual_inspection": False,
        "vision_model": None,
        "vision_status": "desativado" if not args.vision else (
            "configurado" if config["configured"] else "sem credenciais VISION_*"
        ),
        "generated_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "images": {},
        "comparisons": [],
        "vision_diagnostics": {},
    }

    for path in paths:
        report["images"][path.name] = measure(path)

    for left, right in args.compare:
        report["comparisons"].append(compare(Path(left), Path(right)))

    if args.vision and config["configured"]:
        successful = 0
        for path in paths:
            result = ask_vision_model(path, config)
            report["vision_diagnostics"][path.name] = result
            successful += 1 if result.get("status") == "ok" else 0
        if successful:
            report["visual_inspection"] = True
            report["vision_model"] = config["model"]
            report["method"] = "pixel-metrics+vision-model"

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")

    if args.markdown:
        markdown_path = Path(args.markdown)
        markdown_path.parent.mkdir(parents=True, exist_ok=True)
        _write_markdown(markdown_path, report)

    print(json.dumps({
        "images": len(report["images"]),
        "comparisons": len(report["comparisons"]),
        "visual_inspection": report["visual_inspection"],
        "vision_status": report["vision_status"],
        "out": str(out_path).replace("\\", "/"),
    }, ensure_ascii=False))


if __name__ == "__main__":
    main()
