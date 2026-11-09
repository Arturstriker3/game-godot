"""Gera gráficos e métricas do laboratório econômico (WarEconomy).

Lê docs/economy_lab/<tag>/runs.json e os CSVs por cenário/horizonte e grava
docs/economy_lab/charts/*.png e docs/economy_lab/<tag>/metrics.md.

Uso:
  python tools/economy_report.py --tag final
"""

from __future__ import annotations

import argparse
import csv
import json
import math
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

ROOT = Path(__file__).resolve().parents[1]
LAB = ROOT / "docs" / "economy_lab"

SCENARIOS = [
    ("01_agricola", "01 · Agrícola (trigo)"),
    ("02_industrial_importador", "02 · Industrial importador"),
    ("03_desemprego_estrutural", "03 · Desemprego inicial alto"),
    ("04_escassez_alimentos", "04 · Escassez de alimentos"),
    ("05_crescimento_populacional", "05 · Crescimento populacional"),
    ("06_investimento_privado", "06 · Investimento privado"),
    ("07_baixa_renda_precos_altos", "07 · Baixa renda / preços altos"),
    ("08_recursos_sem_infraestrutura", "08 · Recurso sem infraestrutura"),
]
HORIZONS = [30, 90, 365, 1825, 3650]

COLORS = {
    "AGRIA": "#c9a24a",
    "PORTUS": "#6f8f8c",
    "total": "#e2dfd2",
    "need": "#d26a5f",
    "prod": "#7fa86b",
    "stock": "#b0764a",
}


def read_csv(path: Path) -> list[dict]:
    rows: list[dict] = []
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        for raw in csv.DictReader(handle):
            row: dict[str, float | str] = {}
            for key, value in raw.items():
                if key == "city":
                    row[key] = value
                else:
                    try:
                        row[key] = float(value)
                    except (TypeError, ValueError):
                        row[key] = 0.0
            rows.append(row)
    return rows


def by_city(rows: list[dict]) -> dict[str, list[dict]]:
    grouped: dict[str, list[dict]] = defaultdict(list)
    for row in rows:
        grouped[str(row["city"])].append(row)
    for city_rows in grouped.values():
        city_rows.sort(key=lambda item: float(item["day"]))
    return grouped


def aggregate(rows: list[dict], column: str) -> tuple[np.ndarray, np.ndarray]:
    days = sorted({float(row["day"]) for row in rows})
    values = []
    for day in days:
        total = sum(
            float(row[column])
            for row in rows
            if float(row["day"]) == day
        )
        values.append(total)
    return np.array(days), np.array(values)


def load_scenario(tag: str, scenario: str, horizon: int) -> list[dict]:
    path = LAB / tag / scenario / f"{horizon}d.csv"
    if not path.exists():
        return []
    return read_csv(path)


def longest_available(tag: str, scenario: str) -> int:
    for horizon in reversed(HORIZONS):
        if (LAB / tag / scenario / f"{horizon}d.csv").exists():
            return horizon
    return 0


def add_horizon_lines(ax, text_color="#9aa69b") -> None:
    xmax = ax.get_xlim()[1]
    for day, label in ((365, "1 ano"), (1825, "5 anos")):
        if day > xmax:
            continue
        ax.axvline(day, color="#2c3b35", linewidth=0.8, linestyle="--")
        ax.text(day, ax.get_ylim()[1], label, color=text_color,
                fontsize=6, ha="center", va="bottom")


def style_axis(ax, title: str, ylabel: str = "") -> None:
    ax.set_title(title, fontsize=9, color="#e2dfd2", pad=4)
    ax.set_ylabel(ylabel, fontsize=7, color="#9aa69b")
    ax.tick_params(labelsize=6, colors="#9aa69b")
    ax.grid(True, color="#22302b", linewidth=0.6)
    for spine in ax.spines.values():
        spine.set_color("#2c3b35")


def grid_figure(title: str, subtitle: str = "") -> tuple[plt.Figure, np.ndarray]:
    fig, axes = plt.subplots(4, 2, figsize=(15, 13), sharex=False)
    fig.patch.set_facecolor("#0a1112")
    fig.suptitle(title, color="#e2dfd2", fontsize=15, x=0.5, y=0.985)
    if subtitle:
        fig.text(0.5, 0.955, subtitle, color="#9aa69b", fontsize=9, ha="center")
    return fig, axes.ravel()


def save(fig: plt.Figure, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(path, dpi=150, facecolor=fig.get_facecolor(),
                bbox_inches="tight")
    plt.close(fig)


def plot_production_consumption(tag: str, out: Path) -> None:
    fig, axes = grid_figure(
        "Produção, consumo e cobertura de estoque",
        "Produção primária de trigo / necessidade e consumo / necessidade (eixo esquerdo, 1,0 = suficiente); "
        "estoque de alimentos em dias de consumo (eixo direito, limitado a 30).")
    for ax, (scenario, label) in zip(axes, SCENARIOS):
        horizon = longest_available(tag, scenario)
        rows = load_scenario(tag, scenario, horizon)
        if not rows:
            ax.set_visible(False)
            continue
        days = sorted({float(row["day"]) for row in rows})
        production = []
        need = []
        consumed = []
        stock_days = []
        for day in days:
            day_rows = [row for row in rows if float(row["day"]) == day]
            prod = sum(float(row["food_production"]) for row in day_rows)
            need_value = sum(float(row["need"]) for row in day_rows)
            cons = sum(float(row["consumed"]) for row in day_rows)
            stock = sum(
                float(row[f"stock_{product}"])
                for row in day_rows
                for product in ("wheat", "flour", "bread")
            )
            production.append(prod / max(need_value, 1.0))
            need.append(1.0)
            consumed.append(cons / max(need_value, 1.0))
            stock_days.append(min(30.0, stock / max(cons, 1.0)))
        ax.plot(days, production, color=COLORS["prod"], linewidth=1.0, label="trigo produzido / necessidade")
        ax.plot(days, consumed, color=COLORS["total"], linewidth=0.9, label="consumo / necessidade")
        ax.axhline(1.0, color="#9aa69b", linewidth=0.6, linestyle=":")
        style_axis(ax, f"{label} · {horizon}d", "razão")
        ax.set_ylim(0, max(1.6, float(np.nanmax(production)) * 1.05))
        twin = ax.twinx()
        twin.plot(days, stock_days, color=COLORS["stock"], linewidth=0.8, alpha=0.8)
        twin.set_ylim(0, 30)
        twin.tick_params(labelsize=6, colors="#b0764a")
        twin.set_ylabel("estoque (dias)", fontsize=6, color="#b0764a")
        ax.legend(fontsize=6, loc="upper right", frameon=False, labelcolor="#9aa69b")
        add_horizon_lines(ax)
    fig.tight_layout(rect=(0, 0, 1, 0.94))
    save(fig, out / "02_producao_consumo_estoque.png")


def plot_prices(tag: str, out: Path) -> None:
    for suffix, horizon_label in (("curto", "365 dias"), ("longo", "10 anos")):
        fig, axes = grid_figure(
            f"Preços relativos à base ({horizon_label})",
            "Faixa permitida: 0,5× a 3,0× o preço-base; linha 1,0 = base. "
            "Cheio = Agria, tracejado = Portus.")
        for ax, (scenario, label) in zip(axes, SCENARIOS):
            horizon = 365 if suffix == "curto" else longest_available(tag, scenario)
            rows = load_scenario(tag, scenario, horizon)
            if not rows:
                ax.set_visible(False)
                continue
            grouped = by_city(rows)
            city_styles = {"AGRIA": "-", "PORTUS": "--"}
            product_colors = {"wheat": "#c9a24a", "flour": "#b0764a", "bread": "#6f8f8c"}
            for city, city_rows in grouped.items():
                days = np.array([float(r["day"]) for r in city_rows])
                for product, color in product_colors.items():
                    base = {"wheat": 40.0, "flour": 70.0, "bread": 80.0}[product]
                    ratio = np.array([float(r[f"price_{product}"]) / base for r in city_rows])
                    ax.plot(days, ratio, color=color, linewidth=0.9,
                            linestyle=city_styles.get(city, "-"),
                            label=product if city == "AGRIA" else None)
            ax.axhline(1.0, color="#9aa69b", linewidth=0.6, linestyle=":")
            ax.axhline(0.5, color="#d26a5f", linewidth=0.5, linestyle="--")
            ax.axhline(3.0, color="#d26a5f", linewidth=0.5, linestyle="--")
            style_axis(ax, f"{label} · {horizon}d", "preço / base")
            ax.set_ylim(0.3, 3.2)
            ax.legend(fontsize=5, loc="upper right", frameon=False,
                      labelcolor="#9aa69b", ncol=3)
            add_horizon_lines(ax)
        fig.tight_layout(rect=(0, 0, 1, 0.94))
        save(fig, out / f"03_precos_{suffix}.png")


def plot_employment(tag: str, out: Path) -> None:
    fig, axes = grid_figure(
        "Emprego",
        "Desemprego (% da força de trabalho) e pessoas empregadas (% da população). "
        "Horizonte de 10 anos.")
    for ax, (scenario, label) in zip(axes, SCENARIOS):
        horizon = longest_available(tag, scenario)
        rows = load_scenario(tag, scenario, horizon)
        if not rows:
            ax.set_visible(False)
            continue
        days = sorted({float(row["day"]) for row in rows})
        unemployment = []
        employed_share = []
        for day in days:
            day_rows = [row for row in rows if float(row["day"]) == day]
            labor = sum(float(row["labor_force"]) for row in day_rows)
            employed = sum(float(row["employed"]) for row in day_rows)
            population = sum(float(row["population"]) for row in day_rows)
            unemployed = sum(float(row["unemployed"]) for row in day_rows)
            unemployment.append(100.0 * unemployed / max(labor, 1.0))
            employed_share.append(100.0 * employed / max(population, 1.0))
        ax.plot(days, unemployment, color="#d26a5f", linewidth=1.0, label="desemprego")
        ax.plot(days, employed_share, color="#7fa86b", linewidth=0.9, label="emprego / população")
        style_axis(ax, f"{label} · {horizon}d", "%")
        ax.set_ylim(0, 100)
        ax.legend(fontsize=6, loc="upper right", frameon=False, labelcolor="#9aa69b")
        add_horizon_lines(ax)
    fig.tight_layout(rect=(0, 0, 1, 0.94))
    save(fig, out / "04_emprego.png")


def plot_income(tag: str, out: Path) -> None:
    fig, axes = grid_figure(
        "Salários, renda e padrão de vida",
        "Salário médio, renda disponível per capita e padrão de vida (cestas alimentares "
        "por renda), em escala logarítmica. Horizonte de 10 anos.")
    for ax, (scenario, label) in zip(axes, SCENARIOS):
        horizon = longest_available(tag, scenario)
        rows = load_scenario(tag, scenario, horizon)
        if not rows:
            ax.set_visible(False)
            continue
        days = sorted({float(row["day"]) for row in rows})
        wage = []
        disposable = []
        living = []
        for day in days:
            day_rows = [row for row in rows if float(row["day"]) == day]
            employed = sum(float(row["employed"]) for row in day_rows)
            wage.append(sum(float(row["wage_income"]) for row in day_rows)
                        / max(employed, 1.0))
            disposable.append(sum(float(row["disposable_pc"]) for row in day_rows)
                              / max(1, len(day_rows)))
            living.append(min(float(row["living_standard"]) for row in day_rows))
        ax.plot(days, wage, color="#c9a24a", linewidth=1.0, label="salário médio")
        ax.plot(days, disposable, color="#6f8f8c", linewidth=0.9, label="renda pc")
        ax.plot(days, living, color="#7fa86b", linewidth=0.9, label="padrão de vida")
        style_axis(ax, f"{label} · {horizon}d", "valor (log)")
        ax.set_yscale("symlog", linthresh=0.01)
        ax.set_ylim(0, 10)
        ax.legend(fontsize=6, loc="upper right", frameon=False, labelcolor="#9aa69b")
        add_horizon_lines(ax)
    fig.tight_layout(rect=(0, 0, 1, 0.94))
    save(fig, out / "05_renda.png")


def plot_money(tag: str, out: Path) -> None:
    fig, axes = grid_figure(
        "Caixas e conservação de dinheiro",
        "Firmas, serviços, governo e famílias. A linha tracejada é o dinheiro total, "
        "que deve permanecer constante (fronteira do sistema).")
    for ax, (scenario, label) in zip(axes, SCENARIOS):
        horizon = longest_available(tag, scenario)
        rows = load_scenario(tag, scenario, horizon)
        if not rows:
            ax.set_visible(False)
            continue
        days = sorted({float(row["day"]) for row in rows})
        for column, color, name in (
            ("firm_cash", "#c9a24a", "firmas"),
            ("service_cash", "#6f8f8c", "serviços"),
            ("government_budget", "#b0764a", "governo"),
            ("savings", "#7fa86b", "famílias"),
        ):
            values = [sum(float(r[column]) for r in rows if float(r["day"]) == d) for d in days]
            ax.plot(days, values, color=color, linewidth=0.9, label=name)
        money = [
            next(float(r["money_total"]) for r in rows if float(r["day"]) == d)
            for d in days
        ]
        ax.plot(days, money, color="#e2dfd2", linewidth=0.8, linestyle="--",
                label="dinheiro total")
        style_axis(ax, f"{label} · {horizon}d", "caixa")
        ax.set_yscale("symlog", linthresh=1.0)
        ax.legend(fontsize=6, loc="upper right", frameon=False, labelcolor="#9aa69b")
        add_horizon_lines(ax)
    fig.tight_layout(rect=(0, 0, 1, 0.94))
    save(fig, out / "06_moeda_caixas.png")


def plot_trade(tag: str, out: Path) -> None:
    fig, axes = grid_figure(
        "Comércio físico entre cidades",
        "Toneladas/dia exportadas e importadas. O saldo monetário está em metrics.md. "
        "Horizonte de 10 anos.")
    for ax, (scenario, label) in zip(axes, SCENARIOS):
        horizon = longest_available(tag, scenario)
        rows = load_scenario(tag, scenario, horizon)
        if not rows:
            ax.set_visible(False)
            continue
        days = sorted({float(row["day"]) for row in rows})
        exported = [sum(float(r["exported_wheat"]) + float(r["exported_bread"]) + float(r["exported_iron"])
                        for r in rows if float(r["day"]) == d) for d in days]
        imported = [sum(float(r["imported_wheat"]) + float(r["imported_bread"])
                        for r in rows if float(r["day"]) == d) for d in days]
        ax.plot(days, exported, color="#7fa86b", linewidth=1.0, label="exportado (t/dia)")
        ax.plot(days, imported, color="#d26a5f", linewidth=1.0, label="importado (t/dia)")
        style_axis(ax, f"{label} · {horizon}d", "t/dia")
        top = max(max(exported) if exported else 1.0, max(imported) if imported else 1.0)
        ax.set_ylim(0, top * 1.15 + 0.1)
        ax.legend(fontsize=6, loc="upper right", frameon=False, labelcolor="#9aa69b")
        add_horizon_lines(ax)
    fig.tight_layout(rect=(0, 0, 1, 0.94))
    save(fig, out / "07_comercio.png")


def plot_horizon_summary(tag: str, runs: list[dict], out: Path) -> None:
    metrics = [
        ("gdp_pc_ex_construction_end", "PIB pc (sem construção)", "{:.4f}", 0.0, None),
        ("unemployment_end", "Desemprego final", "{:.1%}", 0.0, 1.0),
        ("satisfaction_end", "Satisfação final", "{:.2f}", 0.0, 1.0),
        ("price_ratio_end", "Preço final / base", "{:.2f}", 0.0, 3.2),
        ("stock_days_end", "Estoque final (dias de venda)", "{:.0f}", 0.0, 365.0),
        ("alert_share", "Dias com alerta", "{:.0%}", 0.0, 1.0),
    ]
    fig, axes = plt.subplots(2, 3, figsize=(17, 9))
    fig.patch.set_facecolor("#0a1112")
    fig.suptitle("Panorama por cenário e horizonte", color="#e2dfd2", fontsize=15)
    lookup = {(run["scenario"], int(run["days"])): run for run in runs}
    for ax, (column, title, fmt, _lo, hi) in zip(axes.ravel(), metrics):
        matrix = np.zeros((len(SCENARIOS), len(HORIZONS)))
        for i, (scenario, _) in enumerate(SCENARIOS):
            for j, horizon in enumerate(HORIZONS):
                run = lookup.get((scenario, horizon))
                matrix[i, j] = float(run.get(column, 0.0)) if run else np.nan
        if hi is None:
            vmax = np.nanmax(matrix) if np.isfinite(np.nanmax(matrix)) else 1.0
        else:
            vmax = hi
        vmax = max(float(vmax), 1.0)
        image = ax.imshow(matrix, aspect="auto", cmap="RdYlGn" if column != "price_ratio_end" else "RdYlGn_r",
                          vmin=0.0, vmax=vmax)
        ax.set_xticks(range(len(HORIZONS)))
        ax.set_xticklabels([f"{h}d" for h in HORIZONS], fontsize=7)
        ax.set_yticks(range(len(SCENARIOS)))
        ax.set_yticklabels([label for _, label in SCENARIOS], fontsize=6)
        ax.set_title(title, color="#e2dfd2", fontsize=9)
        for i in range(len(SCENARIOS)):
            for j in range(len(HORIZONS)):
                value = matrix[i, j]
                if np.isfinite(value):
                    ax.text(j, i, fmt.format(value), ha="center", va="center",
                            fontsize=6, color="#0a1112")
        ax.tick_params(colors="#9aa69b")
        for spine in ax.spines.values():
            spine.set_color("#2c3b35")
        cbar = fig.colorbar(image, ax=ax, fraction=0.035, pad=0.02)
        cbar.ax.tick_params(labelsize=5, colors="#9aa69b")
    fig.tight_layout(rect=(0, 0, 1, 0.94))
    save(fig, out / "01_panorama_horizontes.png")


def plot_alerts(tag: str, runs: list[dict], out: Path) -> None:
    kinds = ["desemprego", "satisfacao", "estoque", "conta_externa"]
    colors = ["#d26a5f", "#d08a3e", "#b0764a", "#6f8f8c"]
    fig, ax = plt.subplots(figsize=(14, 5))
    fig.patch.set_facecolor("#0a1112")
    ax.set_facecolor("#0a1112")
    width = 0.2
    lookup = {(run["scenario"], int(run["days"])): run for run in runs}
    x = np.arange(len(SCENARIOS))
    for offset, (kind, color) in enumerate(zip(kinds, colors)):
        values = []
        for scenario, _ in SCENARIOS:
            total = 0
            for horizon in HORIZONS:
                run = lookup.get((scenario, horizon))
                if run:
                    total += int(run.get("alert_kinds", {}).get(kind, 0))
            values.append(total)
        ax.bar(x + (offset - 1.5) * width, values, width=width, color=color, label=kind)
    ax.set_xticks(x)
    ax.set_xticklabels([label for _, label in SCENARIOS], rotation=20, ha="right", fontsize=8)
    ax.set_ylabel("dias com alerta (soma dos 5 horizontes)", color="#9aa69b")
    ax.set_title("Alertas econômicos por categoria", color="#e2dfd2", fontsize=12)
    ax.legend(frameon=False, labelcolor="#9aa69b", fontsize=8)
    ax.grid(True, axis="y", color="#22302b", linewidth=0.6)
    ax.tick_params(colors="#9aa69b")
    for spine in ax.spines.values():
        spine.set_color("#2c3b35")
    fig.tight_layout()
    save(fig, out / "08_alertas.png")


def plot_resources(tag: str, out: Path) -> None:
    fig, axes = plt.subplots(2, 2, figsize=(14, 8))
    fig.patch.set_facecolor("#0a1112")
    fig.suptitle("Recursos naturais: trigo (01) e ferro (08), primeiros 365 dias",
                 color="#e2dfd2", fontsize=14)
    panels = [
        ("01_agricola", "wheat", "01 · trigo: preço e estoque", True),
        ("01_agricola", "wheat", "01 · trigo: produção e consumo", False),
        ("08_recursos_sem_infraestrutura", "iron", "08 · ferro: preço e estoque", True),
        ("08_recursos_sem_infraestrutura", "iron", "08 · ferro: produção e exportação", False),
    ]
    for ax, (scenario, product, label, price_panel) in zip(axes.ravel(), panels):
        rows = load_scenario(tag, scenario, 365)
        if not rows:
            ax.set_visible(False)
            continue
        days = sorted({float(row["day"]) for row in rows})
        if price_panel:
            price = [sum(float(r[f"price_{product}"]) for r in rows if float(r["day"]) == d)
                     / max(1, sum(1 for r in rows if float(r["day"]) == d)) for d in days]
            stock = [sum(float(r[f"stock_{product}"]) for r in rows if float(r["day"]) == d)
                     for d in days]
            ax.plot(days, price, color="#c9a24a", linewidth=1.2, label="preço")
            twin = ax.twinx()
            twin.plot(days, stock, color="#b0764a", linewidth=1.0, alpha=0.85)
            twin.set_ylabel("estoque (t)", color="#b0764a", fontsize=7)
            twin.tick_params(colors="#b0764a", labelsize=6)
            ax.legend(fontsize=7, frameon=False, labelcolor="#9aa69b", loc="upper left")
            left_label = "preço"
        else:
            production = [sum(float(r["food_production"]) if product == "wheat"
                              else float(r["iron_production"])
                              for r in rows if float(r["day"]) == d) for d in days]
            if product == "iron":
                flow = [sum(float(r["exported_iron"]) for r in rows if float(r["day"]) == d)
                        for d in days]
                flow_label = "exportação (t/dia)"
            else:
                flow = [sum(float(r["consumed"]) for r in rows if float(r["day"]) == d)
                        for d in days]
                flow_label = "consumo (t/dia)"
            ax.plot(days, production, color="#7fa86b", linewidth=1.1, label="produção")
            ax.plot(days, flow, color="#e2dfd2", linewidth=1.0, label=flow_label)
            ax.legend(fontsize=7, frameon=False, labelcolor="#9aa69b", loc="upper right")
            left_label = "t/dia"
        if scenario.startswith("08"):
            ax.axvline(30, color="#d08a3e", linewidth=0.8, linestyle="--")
            ax.text(32, ax.get_ylim()[1] * 0.95, "mina (dia 30)", color="#d08a3e",
                    fontsize=6, va="top")
        style_axis(ax, label, left_label)
        ax.tick_params(labelsize=6, colors="#9aa69b")
        for spine in ax.spines.values():
            spine.set_color("#2c3b35")
    fig.tight_layout(rect=(0, 0, 1, 0.94))
    save(fig, out / "09_recursos_naturais.png")


def write_metrics_md(tag: str, runs: list[dict], out: Path) -> None:
    lines = [
        "# Métricas do laboratório econômico",
        "",
        f"Tag: `{tag}` · execuções: {len(runs)}",
        "",
        "| Cenário | Dias | PIB pc* | Desemp. | Satisf. | Preço/base | Estoque (dias) | Alertas | Violações |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for run in runs:
        lines.append(
            "| {scenario} | {days} | {gdp:.4f} | {unemp:.1%} | {sat:.2f} | {price:.2f} | {stock:.0f} | {alerts} | {viol} |".format(
                scenario=run["scenario"],
                days=run["days"],
                gdp=float(run.get("gdp_pc_ex_construction_end", run["gdp_pc_end"])),
                unemp=float(run["unemployment_end"]),
                sat=float(run["satisfaction_end"]),
                price=float(run["price_ratio_end"]),
                stock=float(run["stock_days_max"]),
                alerts=int(run["alerts"]),
                viol=int(run["violations"]),
            )
        )
    lines += [
        "",
        "\\* PIB per capita do último dia, excluindo receita de construção one-off.",
        "",
        "Alertas são dias em que desemprego > 25%, satisfação < 0,8, estoque > 365 dias "
        "de venda ou conta externa negativa; não são violações contábeis.",
        "",
    ]
    (out / "metrics.md").write_text("\n".join(lines), encoding="utf-8")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tag", default="final")
    args = parser.parse_args()
    tag = args.tag
    data = LAB / tag
    runs_path = data / "runs.json"
    if not runs_path.exists():
        raise SystemExit(f"não encontrei {runs_path}; rode tools/economy_lab.gd -- --tag={tag}")
    runs = json.loads(runs_path.read_text(encoding="utf-8"))["runs"]
    out = LAB / "charts"
    out.mkdir(parents=True, exist_ok=True)

    plot_horizon_summary(tag, runs, out)
    plot_production_consumption(tag, out)
    plot_prices(tag, out)
    plot_employment(tag, out)
    plot_income(tag, out)
    plot_money(tag, out)
    plot_trade(tag, out)
    plot_alerts(tag, runs, out)
    plot_resources(tag, out)
    write_metrics_md(tag, runs, data)
    print(f"[report] {len(runs)} execuções, gráficos em {out}")


if __name__ == "__main__":
    main()


