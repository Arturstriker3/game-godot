#!/usr/bin/env python3
"""Gera e valida os dados das cadeias de produção (Fase F1 do plano).

Fontes de verdade:
  - docs/economy/production_chains_spec.json  (94 bens, receitas, necessidades)
  - data/definitions/scenario/nations.json    (PIB PPP e moeda por nação)

Saídas (nada aqui toca a simulação C++; o núcleo ignora os bens que não conhece):
  - data/definitions/economy/goods.json       (115 bens, ordem canônica 0..114)
  - data/definitions/economy/recipes.json     (receitas normalizadas)
  - data/definitions/economy/needs.json       (necessidades das POPs)
  - data/definitions/economy/currencies.json  (moeda por nação e câmbio derivado)
  - data/definitions/i18n/i18n.csv            (product.<id> e resource.<id> × 3 idiomas)
  - tools/icon_forge/resource_icons.json      (label/cor/tier sincronizados)
  - docs/economy/production_chains_data.md    (tabela de preços derivados)

Uso:
  python tools/build_goods_chain.py build    # regera os dados e roda a validação
  python tools/build_goods_chain.py check    # só valida (não escreve nada)
  python tools/build_goods_chain.py report   # reescreve o relatório markdown

Todos os números derivados carregam a procedência (`base_price_source`,
`params_source`, `anchor`, `source`). Nada de preço "inventado no olho": os
primários mantêm o preço já calibrado e os demais saem da fórmula de §4.1 do
plano; necessidades fora de alimento e parâmetros de fábrica são declarados
como estimativa (`estimated`) até o laboratório calibrar.
"""
from __future__ import annotations

import csv
import io
import json
import re
import statistics
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SPEC_PATH = ROOT / "docs/economy/production_chains_spec.json"
NATIONS_PATH = ROOT / "data/definitions/scenario/nations.json"
ECONOMY_DIR = ROOT / "data/definitions/economy"
GOODS_PATH = ECONOMY_DIR / "goods.json"
RECIPES_PATH = ECONOMY_DIR / "recipes.json"
NEEDS_PATH = ECONOMY_DIR / "needs.json"
CURRENCIES_PATH = ECONOMY_DIR / "currencies.json"
I18N_PATH = ROOT / "data/definitions/i18n/i18n.csv"
ICON_MANIFEST_PATH = ROOT / "tools/icon_forge/resource_icons.json"
ICON_SVG_DIR = ROOT / "assets/icons/resources"
REPORT_PATH = ROOT / "docs/economy/production_chains_data.md"

# Ordem canônica dos 21 bens já simulados pelo núcleo C++ (enum DemoProduct e
# kDemoProductNames). NÃO reordenar: é o contrato do status, do save e da IA.
CANONICAL_PRIMARIES = [
    "wheat", "iron", "rice", "maize", "bauxite",
    "lead", "gold", "phosphate", "tungsten", "rare_earths",
    "sulfur", "diamond", "coal", "copper", "gas",
    "oil", "potato", "soy", "cotton", "wood", "uranium",
]
# Preço-base atual (goods.json anterior ao F1). Dado calibrado, preservado.
# O build lê o goods.json existente e só usa esta tabela como semente.
LEGACY_PRIMARY_PRICES = {
    "wheat": 40.0, "iron": 60.0, "rice": 55.0, "maize": 42.0, "bauxite": 15.0,
    "lead": 25.0, "gold": 3000.0, "phosphate": 10.0, "tungsten": 200.0,
    "rare_earths": 400.0, "sulfur": 15.0, "diamond": 5000.0, "coal": 6.0,
    "copper": 60.0, "gas": 30.0, "oil": 45.0, "potato": 35.0, "soy": 45.0,
    "cotton": 70.0, "wood": 25.0, "uranium": 500.0,
}
PRIMARY_SECTORS = {
    "wheat": "food", "rice": "food", "maize": "food", "potato": "food",
    "soy": "food", "cotton": "textile", "wood": "wood",
    "coal": "energy", "oil": "energy", "gas": "energy", "uranium": "energy",
    "iron": "mining", "copper": "mining", "bauxite": "mining", "lead": "mining",
    "gold": "mining", "tungsten": "mining", "rare_earths": "mining",
    "phosphate": "mining", "sulfur": "mining", "diamond": "mining",
}
PRIMARY_FOOD = {"wheat", "rice", "maize", "potato", "soy"}

# §4.1 do plano: margem por tipo de bem. `durable`/`luxury` vêm antes de
# `intermediate`/`final` (um bem durável é final + margem de durável).
MARGIN_RAW = 0.0
MARGIN_BYPRODUCT = 0.10
MARGIN_INTERMEDIATE = 0.25
MARGIN_FINAL = 0.45
MARGIN_DURABLE = 0.80
MARGIN_LUXURY = 0.80

# Parâmetros de engenharia por setor (empregos/t, salário, opex/t, capital por
# t/dia de capacidade). Ordem de grandeza copiada de
# core/economy/ai/industry_templates.gd (fazenda 7,5 / 2,0 / 2,5) e do exemplo do
# plano (motor: 4,0 / 1,8 / 12,0 / 6.000). ESTIMATIVA até o laboratório de F2.
BUILDING_PARAMS = {
    "food": (6.0, 1.8, 6.0, 4000.0),
    "agro": (6.0, 1.8, 7.0, 5000.0),
    "textile": (8.0, 1.6, 6.0, 3000.0),
    "wood": (6.0, 1.7, 4.0, 3500.0),
    "chemical": (3.0, 2.0, 12.0, 12000.0),
    "metal": (3.0, 2.0, 10.0, 10000.0),
    "machinery": (4.0, 1.8, 12.0, 6000.0),
    "construction": (5.0, 1.8, 6.0, 4000.0),
    "transport": (3.5, 2.0, 14.0, 9000.0),
    "electronics": (5.0, 2.2, 18.0, 15000.0),
    "packaging": (6.0, 1.7, 5.0, 3000.0),
    "luxury": (9.0, 2.0, 10.0, 5000.0),
    "military": (4.0, 2.0, 16.0, 12000.0),
}

SECTORS = set(BUILDING_PARAMS) | {"energy", "mining"}
KINDS = {"raw", "intermediate", "final", "byproduct"}
TIERS = {"raw", "semi", "final"}
KIND_TO_TIER = {"raw": "raw", "intermediate": "semi", "byproduct": "semi", "final": "final"}

# Bens que a spec declara como o MESMO item de outro (nota de `charcoal`:
# "produz o MESMO item coal"). Modelado como receita com output `coal` (§4.2).
GOOD_ALIASES = {"charcoal": "coal"}

# Consumidores institucionais declarados pela spec (`use`). Não são receitas:
# são regras de consumo que F2/F3/F4 vão ligar no núcleo.
INSTITUTIONAL_TAGS = {
    "army", "government", "port", "farm", "mine", "factory", "building",
    "vehicle_fleet", "machines", "workshop", "construction", "treasury",
    "pop_basic", "pop_life", "pop_luxury",
}
# `use: ["many"]` não é consumidor por si: o bem tem de aparecer em receitas.
VAGUE_USE_TAGS = {"many"}

# Necessidades das POPs (§4.3). O alimento tem âncora física no núcleo
# (kDemoNeedPerCapita = 0,0003 t-eq/dia = 0,009 t-eq/mês). As demais categorias
# não têm âncora: são ESTIMATIVA declarada, derivadas de uma fração do alimento.
FOOD_ANCHOR_T_MONTH = 0.009
CLOTHING_ANCHOR_T_MONTH = 0.0004  # exemplo do próprio plano §4.3
LEVEL_SHARES_OF_FOOD = {"life": 0.25, "luxury": 0.05}
LEVEL_WEIGHTS = {"basic": 0.60, "life": 0.30, "luxury": 0.10}
CATEGORY_SHARES = {
    "life": {
        "protein": 0.20, "footwear": 0.10, "health": 0.15, "home": 0.20,
        "communication": 0.10, "leisure": 0.10, "transport": 0.15,
    },
    "luxury": {"status": 0.40, "tech": 0.35, "transport": 0.25},
}
# Fatores de equivalência (t-eq por tonelada). Os cinco primeiros são os fatores
# calóricos REAIS do núcleo (kDemoCaloricFactor); pão 1,6 vem da nota da spec
# ("rende ~1.6x grão cru"); os demais são estimativa declarada.
EQUIVALENCE = {
    "calories": {
        "wheat": 1.0, "rice": 1.0, "maize": 1.0, "potato": 0.25, "soy": 1.3,
        "bread": 1.6, "bagged_rice": 1.0, "meat": 1.0, "canned_food": 0.8,
    },
    "protein": {"meat": 1.0, "canned_food": 0.8},
}

# Traduções en/es dos 94 bens novos (pt-BR vem do `name` da spec).
TRANSLATIONS = {
    "flour": ("Flour", "Harina"),
    "bran": ("Bran", "Salvado"),
    "bread": ("Bread", "Pan"),
    "bagged_rice": ("Bagged rice", "Arroz envasado"),
    "feed": ("Animal feed", "Pienso"),
    "meat": ("Meat", "Carne"),
    "canned_food": ("Canned food", "Comida enlatada"),
    "combat_ration": ("Combat ration", "Ración de combate"),
    "alcohol": ("Alcoholic beverage", "Bebida alcohólica"),
    "premium_spirits": ("Premium spirits", "Destilado premium"),
    "ethanol": ("Ethanol", "Etanol"),
    "fertilizer": ("Fertilizer", "Fertilizante"),
    "organic_fertilizer": ("Organic fertilizer", "Abono orgánico"),
    "fabric": ("Fabric", "Tejido"),
    "synthetic_fabric": ("Synthetic fabric", "Tejido sintético"),
    "clothes": ("Clothes", "Ropa"),
    "uniforms": ("Uniforms", "Uniformes"),
    "shoes": ("Footwear", "Calzado"),
    "planks": ("Planks", "Tablas"),
    "sawdust": ("Sawdust", "Serrín"),
    "furniture": ("Furniture", "Muebles"),
    "luxury_furniture": ("Luxury furniture", "Muebles de lujo"),
    "cellulose": ("Cellulose", "Celulosa"),
    "paper": ("Paper", "Papel"),
    "pallets": ("Pallets", "Palés"),
    "charcoal": ("Charcoal", "Carbón vegetal"),
    "gasoline": ("Gasoline", "Gasolina"),
    "diesel": ("Diesel", "Diésel"),
    "jet_fuel": ("Jet fuel", "Queroseno de aviación"),
    "plastic": ("Plastic", "Plástico"),
    "chemicals": ("Chemicals", "Productos químicos"),
    "rubber": ("Rubber", "Caucho"),
    "refined_sulfur": ("Refined sulfur", "Azufre refinado"),
    "gunpowder": ("Gunpowder", "Pólvora"),
    "explosives": ("Explosives", "Explosivos"),
    "carbon_fiber": ("Carbon fiber", "Fibra de carbono"),
    "medicine": ("Medicine", "Medicamentos"),
    "antibiotics": ("Antibiotics", "Antibióticos"),
    "vaccines": ("Vaccines", "Vacunas"),
    "plastic_parts": ("Plastic parts", "Piezas plásticas"),
    "iron_ingot": ("Iron ingot", "Lingote de hierro"),
    "steel": ("Steel", "Acero"),
    "steel_sheet": ("Steel sheet", "Chapas de acero"),
    "special_alloy": ("Special alloy", "Aleación especial"),
    "cast_parts": ("Cast parts", "Piezas fundidas"),
    "mechanical_parts": ("Mechanical parts", "Piezas mecánicas"),
    "hardware": ("Hardware", "Ferretería"),
    "copper_ingot": ("Copper ingot", "Lingote de cobre"),
    "wires": ("Wires and cables", "Cables"),
    "aluminum_ingot": ("Aluminum ingot", "Lingote de aluminio"),
    "lead_ingot": ("Lead ingot", "Lingote de plomo"),
    "tungsten_ingot": ("Tungsten ingot", "Lingote de tungsteno"),
    "gold_bar": ("Gold bar", "Barra de oro"),
    "processed_rare_earths": ("Processed rare earths", "Tierras raras procesadas"),
    "tools": ("Tools", "Herramientas"),
    "industrial_tools": ("Industrial tools", "Herramientas industriales"),
    "precision_tools": ("Precision tools", "Herramientas de precisión"),
    "engine": ("Engine", "Motor"),
    "micromotor": ("Micromotor and coils", "Micromotor y bobinas"),
    "tires": ("Tires", "Neumáticos"),
    "chassis": ("Chassis", "Chasis"),
    "tractor": ("Tractor", "Tractor"),
    "mining_machine": ("Mining machine", "Máquina de minería"),
    "robots": ("Industrial robots", "Robots industriales"),
    "industrial_machinery": ("Industrial machinery", "Maquinaria industrial"),
    "spare_parts": ("Spare parts", "Piezas de repuesto"),
    "construction_material": ("Construction material", "Material de construcción"),
    "steel_structure": ("Steel structure", "Estructura metálica"),
    "electrical_material": ("Electrical material", "Material eléctrico"),
    "truck": ("Truck", "Camión"),
    "car": ("Automobile", "Automóvil"),
    "naval_hull": ("Naval hull", "Casco naval"),
    "cargo_ship": ("Cargo ship", "Buque de carga"),
    "fuselage": ("Fuselage", "Fuselaje"),
    "cargo_plane": ("Cargo plane", "Avión de carga"),
    "electronic_components": ("Electronic components", "Componentes electrónicos"),
    "circuit_board": ("Circuit board", "Placa de circuito"),
    "chips": ("Chips", "Chips"),
    "sensors": ("Sensors and optics", "Sensores y óptica"),
    "batteries": ("Batteries", "Baterías"),
    "appliances": ("Home appliances", "Electrodomésticos"),
    "computer": ("Computer", "Computadora"),
    "phone": ("Mobile phone", "Teléfono móvil"),
    "drone": ("Drone", "Dron"),
    "containers": ("Containers", "Recipientes"),
    "jewelry": ("Jewelry", "Joyas"),
    "casing": ("Cartridge casing", "Estuche"),
    "projectile": ("Projectile", "Proyectil"),
    "ammunition": ("Ammunition", "Munición"),
    "artillery_ammo": ("Artillery ammunition", "Munición de artillería"),
    "small_arms": ("Small arms", "Armas ligeras"),
    "artillery": ("Artillery", "Artillería"),
    "military_drone": ("Military drone", "Dron militar"),
    "infantry_gear": ("Infantry gear", "Equipo de infantería"),
}

COLOR_PATTERN = re.compile(r"^#[0-9a-fA-F]{6}$")


def load_json(path: Path):
    return json.loads(path.read_text(encoding="utf-8"))


def dump_json(path: Path, data) -> None:
    """JSON de 2 espaços, LF e uma newline final (mesmo formato do goods.json)."""
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, ensure_ascii=False, indent=2) + "\n",
                    encoding="utf-8", newline="\n")


def margin_for(good: dict) -> float:
    if good["kind"] == "raw":
        return MARGIN_RAW
    if good["kind"] == "byproduct":
        return MARGIN_BYPRODUCT
    if good["durable"] or good["sector"] == "luxury":
        return MARGIN_DURABLE
    if good["kind"] == "intermediate":
        return MARGIN_INTERMEDIATE
    return MARGIN_FINAL


# --------------------------------------------------------------------------
# Receitas
# --------------------------------------------------------------------------


def recipe_outputs(good: dict, recipe: dict) -> list[dict]:
    """Saídas explícitas de uma receita de co-produção (`outputs[]`).

    A primeira saída é a principal (define `output`/`yield` para o núcleo
    legado e para a IA); as demais são co-produtos com quantidade por lote de
    insumo. `co_loss` é a fração declarada que vira perda/outros não modelados
    e não entra em `outputs`.
    """
    co_outputs = recipe.get("co_outputs", [])
    if not co_outputs:
        return []
    main = GOOD_ALIASES.get(good["id"], good["id"])
    outputs = [{"good": main, "qty": float(recipe["yield"])}]
    for item in co_outputs:
        outputs.append({"good": item["id"], "qty": float(item["qty"])})
    return outputs


def byproduct_decl(item) -> dict:
    """Normaliza a declaração de subproduto da spec.

    Forma legada: string (`"bran"`) = 1 t por lote de insumo. Forma com
    quantidade real: `{"id": "sawdust", "qty": 0.12}`. A quantidade é por
    lote de insumo, igual ao `yield` da saída principal.
    """
    if isinstance(item, str):
        return {"good": item, "qty": 1.0}
    if isinstance(item, dict):
        good_id = item.get("id") or item.get("good")
        return {"good": good_id, "qty": float(item.get("qty", 1.0))}
    raise ValueError(f"subproduto inválido: {item!r}")


def build_recipes(spec: dict) -> list[dict]:
    recipes: list[dict] = []
    for good in spec["goods"]:
        sector = good["sector"]
        jobs, wage, opex, capital = BUILDING_PARAMS.get(sector, BUILDING_PARAMS["machinery"])
        alternatives = good["recipes"]
        for index, recipe in enumerate(alternatives):
            suffix = ""
            if len(alternatives) > 1:
                suffix = "_" + chr(ord("a") + index)
            inputs = [{"good": name, "qty": 1.0} for name in recipe["inputs"]]
            outputs = recipe_outputs(good, recipe)
            if outputs:
                # Co-produção: `byproducts` repete as saídas extras para o
                # núcleo atual (output + byproducts) e `outputs` documenta a
                # lista completa e conserva a quantidade de cada fração.
                byproducts = [dict(item) for item in outputs[1:]]
                spec_qty = True
            else:
                byproducts = [byproduct_decl(item)
                              for item in good.get("byproducts", [])]
                spec_qty = any(abs(item["qty"] - 1.0) > 1e-9
                               for item in byproducts)
            entry = {
                "id": good["id"] + suffix,
                "building": recipe.get("building", "factory"),
                "sector": sector,
                # `charcoal` produz o MESMO item `coal` (§4.2 do plano).
                "output": GOOD_ALIASES.get(good["id"], good["id"]),
                "yield": float(recipe["yield"]),
                "inputs": inputs,
                "byproducts": byproducts,
                "jobs_per_ton": jobs,
                "wage": wage,
                "opex_per_ton": opex,
                "capital_per_capacity": capital,
                "coef_source": "default_unit" if len(inputs) > 1 else "spec",
                "params_source": "estimated:order_of_magnitude",
            }
            if outputs:
                entry["outputs"] = outputs
            if recipe.get("co_loss", 0.0) > 0.0:
                entry["co_loss"] = float(recipe["co_loss"])
            if outputs:
                entry["byproduct_coef_source"] = "spec_qty"
            note_parts = [good.get("note", ""), recipe.get("note", "")]
            if outputs:
                note_parts.append(
                    "co-produção: saídas em outputs[] com quantidade por lote; "
                    "a fração co_loss é perda/outros não modelados")
            elif byproducts:
                if spec_qty:
                    entry["byproduct_coef_source"] = "spec_qty"
                    note_parts.append(
                        "subproduto com quantidade por lote declarada na spec")
                else:
                    entry["byproduct_coef_source"] = "default_unit"
                    note_parts.append(
                        "subproduto com coeficiente unitário padrão (spec sem qty)")
                if recipe.get("co_loss", 0.0) > 0.0:
                    note_parts.append(
                        "a fração co_loss é perda declarada")
            note = " | ".join(part for part in note_parts if part).strip(" |")
            if note:
                entry["note"] = note
            recipes.append(entry)
    return recipes


# --------------------------------------------------------------------------
# Preços derivados
# --------------------------------------------------------------------------


def co_allocation_by_recipe(spec: dict) -> dict[str, dict]:
    """`recipe_id` → bloco `co_allocation` declarado na spec.

    Só receitas de co-produção (`co_outputs`) têm o bloco. O id segue a mesma
    regra de `build_recipes` (sufixo `_a`, `_b`… quando há alternativas).
    """
    out: dict[str, dict] = {}
    for good in spec["goods"]:
        alternatives = good["recipes"]
        for index, recipe in enumerate(alternatives):
            alloc = recipe.get("co_allocation")
            if not alloc:
                continue
            suffix = "_" + chr(ord("a") + index) if len(alternatives) > 1 else ""
            out[good["id"] + suffix] = alloc
    return out


def co_product_unit_prices(recipe: dict, alloc: dict, batch_cost: float,
                           margins: dict[str, float]) -> dict[str, float]:
    """Preço de cada saída de uma receita de co-produção.

    O custo do lote NÃO é dividido por tonelada: ele é rateado pelo VALOR DE
    MERCADO de cada saída (`qty × preço de mercado por tonelada`), com o preço
    de mercado vindo da fonte declarada em `co_allocation` (EIA: USD/gal × 42
    gal/barril × barris por tonelada). Nenhum número é escolhido à mão.
    """
    usd = alloc.get("usd_per_gallon", {})
    bbl = alloc.get("barrels_per_tonne", {})
    weights: dict[str, float] = {}
    quantities: dict[str, float] = {}
    for item in recipe["outputs"]:
        good = item["good"]
        if good not in usd or good not in bbl:
            raise ValueError(
                f"{recipe['id']}: co_allocation sem preço/barris para '{good}'")
        quantity = float(item["qty"])
        per_tonne = float(usd[good]) * 42.0 * float(bbl[good])
        quantities[good] = quantity
        weights[good] = quantity * per_tonne
    total = sum(weights.values())
    if total <= 0.0:
        raise ValueError(f"{recipe['id']}: co_allocation com valor total zero")
    return {
        good: batch_cost * weight / total / quantities[good]
        * (1.0 + margins.get(good, 0.0))
        for good, weight in weights.items()
    }


def price_order(spec: dict, producers: dict[str, list[dict]],
                byproduct_parent: dict[str, str]) -> list[str]:
    """Ordem topológica determinística (empate pela ordem da spec)."""
    ids = [g["id"] for g in spec["goods"]]
    rank = {gid: i for i, gid in enumerate(ids)}
    deps: dict[str, set[str]] = {gid: set() for gid in ids}
    for gid, recipes in producers.items():
        for recipe in recipes:
            for item in recipe["inputs"]:
                if item["good"] in deps and item["good"] != gid:
                    deps[gid].add(item["good"])
            # Co-produto é precificado depois do produto principal (só quando o
            # gid iterado é o próprio principal; senão cria ciclo entre irmãos).
            if recipe.get("outputs") and gid == recipe["output"]:
                for item in recipe["outputs"][1:]:
                    if item["good"] in deps and item["good"] != gid:
                        deps[item["good"]].add(gid)
    for child, parent in byproduct_parent.items():
        if child in deps and parent in deps and child != parent:
            deps[child].add(parent)

    ordered: list[str] = []
    remaining = dict(deps)
    while remaining:
        ready = sorted((gid for gid, need in remaining.items() if not need), key=lambda g: rank[g])
        if not ready:
            raise ValueError("ciclo no grafo de receitas: " + ", ".join(sorted(remaining)))
        for gid in ready:
            ordered.append(gid)
            del remaining[gid]
        for need in remaining.values():
            need.difference_update(ready)
    return ordered


def compute_prices(spec: dict, recipes: list[dict], primaries: dict[str, float]) -> dict[str, dict]:
    # `producers` conta só a SAÍDA PRINCIPAL de cada receita. Subproduto e
    # co-produto de `outputs[]` não podem entrar aqui: se entrarem, a regra
    # `gid in byproduct_parent and gid not in producers` nunca vale e todo
    # subproduto (farelo, serragem, adubo…) sai precificado pelo custo da
    # receita em vez de 0,10 × preço do pai (t145).
    producers: dict[str, list[dict]] = {}
    # Co-produtos de co-produção (`outputs[1:]`) ficam em tabela própria: eles
    # têm preço pela rota principal, sem virar "produtor" para a regra acima.
    co_producers: dict[str, list[dict]] = {}
    for recipe in recipes:
        producers.setdefault(recipe["output"], []).append(recipe)
        for item in recipe.get("outputs", [])[1:]:
            co_producers.setdefault(item["good"], []).append(recipe)
    byproduct_parent = {}
    for good in spec["goods"]:
        for recipe in good["recipes"]:
            for child in good.get("byproducts", []):
                byproduct_parent[byproduct_decl(child)["good"]] = good["id"]

    spec_by_id = {g["id"]: g for g in spec["goods"]}
    margins = {gid: margin_for(good) for gid, good in spec_by_id.items()}
    alloc_by_recipe = co_allocation_by_recipe(spec)
    recipes_by_id = {recipe["id"]: recipe for recipe in recipes}
    # Co-produto (t141): o preço-base vem da ROTA PRINCIPAL — a receita que
    # gera o bem no lote. Rota alternativa de produção isolada (CTL/etanol)
    # não define o preço-base do co-produto.
    co_product_parent: dict[str, str] = {}
    for rid in alloc_by_recipe:
        recipe = recipes_by_id.get(rid)
        if recipe is None:
            continue
        for item in recipe.get("outputs", [])[1:]:
            co_product_parent.setdefault(item["good"], rid)

    prices: dict[str, dict] = {}
    for gid, price in primaries.items():
        prices[gid] = {"price": price, "source": "calibrated:legacy", "margin": None}

    for gid in price_order(spec, producers, byproduct_parent):
        good = spec_by_id[gid]
        if gid in GOOD_ALIASES:
            parent = GOOD_ALIASES[gid]
            prices[gid] = {
                "price": prices[parent]["price"],
                "source": f"alias:{parent}",
                "margin": None,
            }
            continue
        if gid in byproduct_parent and gid not in producers and gid not in co_producers:
            parent = byproduct_parent[gid]
            price = MARGIN_BYPRODUCT * prices[parent]["price"]
            prices[gid] = {
                "price": max(0.01, round(price, 2)),
                "source": f"derived:byproduct_of:{parent}:v1",
                "margin": MARGIN_BYPRODUCT,
            }
            continue
        margin = margins[gid]
        candidates: list[tuple[float, str]] = []
        route_recipes = producers.get(gid, []) + co_producers.get(gid, [])
        if gid in co_product_parent:
            route_recipes = [recipe for recipe in route_recipes
                             if recipe["id"] == co_product_parent[gid]]
        for recipe in route_recipes:
            if recipe.get("outputs"):
                total_output = sum(item["qty"] for item in recipe["outputs"])
            else:
                total_output = float(recipe["yield"])
            if total_output <= 0.0:
                continue
            total_input = sum(item["qty"] * prices[item["good"]]["price"]
                              for item in recipe["inputs"])
            if recipe.get("outputs"):
                alloc = alloc_by_recipe.get(recipe["id"])
                if alloc:
                    # Rateio por VALOR DE MERCADO declarado (fonte no dado):
                    # o custo do lote vira o preço de cada saída.
                    allocated = co_product_unit_prices(
                        recipe, alloc, total_input, margins)
                    if gid in allocated:
                        candidates.append((
                            allocated[gid],
                            "derived:co_product_market_value:v2"))
                        continue
                # Co-produção sem fonte de rateio: mantém a regra antiga (custo
                # do lote dividido por tonelada de TODAS as saídas).
                unit_cost = total_input / total_output
                candidates.append((unit_cost * (1.0 + margin),
                                   "derived:co_product:v1"))
                continue
            unit_cost = total_input / total_output
            candidates.append((unit_cost * (1.0 + margin), "derived:v1"))
        if not candidates:
            raise ValueError(f"bem sem receita nem subproduto nem alias: {gid}")
        price, source = min(candidates, key=lambda item: item[0])
        prices[gid] = {
            "price": max(0.01, round(price, 2)),
            "source": source,
            "margin": margin,
        }
    return prices


# --------------------------------------------------------------------------
# goods.json
# --------------------------------------------------------------------------


def build_goods(spec: dict, prices: dict[str, dict], icons: dict, legacy_prices: dict) -> list[dict]:
    primaries: dict[str, float] = {}
    for gid in CANONICAL_PRIMARIES:
        primaries[gid] = float(legacy_prices.get(gid, LEGACY_PRIMARY_PRICES[gid]))
    out: list[dict] = []
    icons_by_id = icons
    for order, gid in enumerate(CANONICAL_PRIMARIES):
        icon = icons_by_id[gid]
        out.append({
            "order": order,
            "id": gid,
            "name": icon["label"],
            "sector": PRIMARY_SECTORS[gid],
            "kind": "raw",
            "tier": "raw",
            "durable": False,
            "base_price": primaries[gid],
            "base_price_source": "calibrated:legacy",
            "price_source": "calibrated",
            "icon_color": icon["color"],
            "food": gid in PRIMARY_FOOD,
            "use": [],
        })
        if gid == "uranium":
            out[-1]["consumer_note"] = "pendente: eletricidade/reator (spec.pending 'urânio')"
    for offset, good in enumerate(spec["goods"]):
        gid = good["id"]
        icon = icons_by_id[gid]
        entry = {
            "order": 21 + offset,
            "id": gid,
            "name": good["name"],
            "sector": good["sector"],
            "kind": good["kind"],
            "tier": KIND_TO_TIER[good["kind"]],
            "durable": bool(good["durable"]),
            "base_price": prices[gid]["price"],
            "base_price_source": prices[gid]["source"],
            "price_source": "derived" if prices[gid]["source"].startswith(("derived", "alias")) else "calibrated",
            "icon_color": icon["color"],
            "food": False,
            "use": list(good.get("use", [])),
        }
        if gid in GOOD_ALIASES:
            entry["alias_of"] = GOOD_ALIASES[gid]
        note = good.get("note", "")
        if note:
            entry["note"] = note
        out.append(entry)
    return out


# --------------------------------------------------------------------------
# needs.json
# --------------------------------------------------------------------------


def build_needs(spec: dict, goods: list[dict]) -> dict:
    by_id = {g["id"]: g for g in goods}
    levels = {}
    for level, categories in spec["pop_needs"].items():
        built = {}
        for category, substitutes in categories.items():
            if level == "basic" and category == "food":
                per_capita = FOOD_ANCHOR_T_MONTH
                anchor = "existing_food_need"
                rule = "kDemoNeedPerCapita (0,0003 t-eq/dia) × 30 dias"
            elif level == "basic" and category == "clothing":
                per_capita = CLOTHING_ANCHOR_T_MONTH
                anchor = "estimated"
                rule = "exemplo do plano §4.3; calibrar no laboratório"
            else:
                total = LEVEL_SHARES_OF_FOOD[level] * FOOD_ANCHOR_T_MONTH
                share = CATEGORY_SHARES[level][category]
                per_capita = round(total * share, 6)
                anchor = "estimated"
                rule = (f"{LEVEL_SHARES_OF_FOOD[level]} × âncora de alimento × "
                        f"{share} da cesta do nível")
            built[category] = {
                "substitutes": list(substitutes),
                "per_capita_t_month": per_capita,
                "anchor": anchor,
                "anchor_rule": rule,
            }
            equivalence_key = "calories" if category == "food" else (
                "protein" if category == "protein" else category)
            if equivalence_key in EQUIVALENCE:
                built[category]["equivalence_key"] = equivalence_key
        levels[level] = {
            "priority": {"basic": 1, "life": 2, "luxury": 3}[level],
            "satisfaction_weight": LEVEL_WEIGHTS[level],
            "categories": built,
        }

    institutional: dict[str, list[str]] = {}
    for good in goods:
        for tag in good.get("use", []):
            if tag in INSTITUTIONAL_TAGS:
                institutional.setdefault(tag, []).append(good["id"])
    for tag in institutional:
        institutional[tag].sort()

    return {
        "version": 1,
        "note": (
            "Necessidades das POPs. Só basic/food tem âncora física no núcleo "
            "(kDemoNeedPerCapita); as demais categorias são ESTIMATIVA declarada "
            "(anchor: estimated) e devem ser calibradas no laboratório na F3."
        ),
        "source": "docs/economy/production_chains_spec.json#pop_needs + plano §4.3",
        "levels": levels,
        "durable_replacement_per_year": float(spec["durable_replacement_per_year"]),
        "equivalence": EQUIVALENCE,
        "equivalence_note": (
            "Fatores de calories vêm de kDemoCaloricFactor (batata 0,25; soja 1,3); "
            "pão 1,6 vem da nota da spec. protein e as categorias não alimentares "
            "são estimativa."
        ),
        "institutional": institutional,
        "consumption_rules": list(spec["consumption_rules"]),
        "pending": list(spec.get("pending", [])),
    }


# --------------------------------------------------------------------------
# currencies.json
# --------------------------------------------------------------------------


def build_currencies(nations: list[dict]) -> dict:
    def ppp_per_capita(nation: dict) -> float | None:
        gdp = nation.get("gdp_ppp_bn")
        population = nation.get("population")
        if not gdp or not population:
            return None
        return float(gdp) * 1e9 / float(population)

    reference = ppp_per_capita(next(n for n in nations if n["id"] == "USA"))
    groups: dict[str, dict] = {}
    for nation in nations:
        code = nation["currency"]
        group = groups.setdefault(code, {"nations": [], "ppp": [], "missing": []})
        group["nations"].append(nation["id"])
        value = ppp_per_capita(nation)
        if value is None:
            group["missing"].append(nation["id"])
        else:
            group["ppp"].append(value)

    entries = []
    for code in sorted(groups):
        group = groups[code]
        entry = {"code": code, "nations": sorted(group["nations"])}
        if group["ppp"]:
            # A moeda do país de referência vale 1,0 por definição (é o numerário
            # da fórmula); as demais usam a mediana do nível de preços das nações.
            rate = 1.0 if "USA" in group["nations"] else statistics.median(group["ppp"]) / reference
            entry["rate_per_anchor"] = round(max(rate, 0.0001), 6)
            entry["source"] = "anchor" if code == "USD" else "derived:gdp_ppp"
        else:
            # Sem PIB PPP publicado para nenhuma nação da moeda: estado honesto.
            entry["rate_per_anchor"] = None
            entry["source"] = "unknown:no_gdp"
        if group["missing"]:
            entry["nations_without_gdp"] = sorted(group["missing"])
        entries.append(entry)

    return {
        "version": 1,
        "anchor": "XGS",
        "anchor_note": (
            "XGS é a unidade-âncora interna (1,0). Os caixas continuam na âncora "
            "(plano §5.6); a moeda só entra na decisão de compra."
        ),
        "reference": "USA",
        "formula": (
            "rate_per_anchor(moeda) = mediana(gdp_ppp_bn × 1e9 / population das nações "
            "da moeda) / (mesmo valor dos EUA). Proxy de nível de preços; é ESTIMATIVA "
            "derivada, não taxa de mercado."
        ),
        "rate_note": (
            "rate_per_anchor = valor em âncora de 1 unidade da moeda local. Moeda sem "
            "PIB PPP publicado fica com rate null e source unknown:no_gdp — a F5 decide "
            "o que fazer sem inventar número."
        ),
        "regime": None,
        "regime_note": "Sem fonte para regime cambial (peg/float); declarar na F5.",
        "currencies": entries,
    }


# --------------------------------------------------------------------------
# i18n
# --------------------------------------------------------------------------


def update_i18n(goods: list[dict]) -> int:
    raw = I18N_PATH.read_bytes().decode("utf-8")
    rows = list(csv.DictReader(io.StringIO(raw)))
    existing = {row["key"] for row in rows}
    added = 0
    for good in goods:
        gid = good["id"]
        en, es = TRANSLATIONS.get(gid, (good["name"], good["name"]))
        for prefix in ("product.", "resource."):
            key = prefix + gid
            if key in existing:
                continue
            rows.append({"key": key, "pt_BR": good["name"], "en": en, "es": es})
            existing.add(key)
            added += 1
    rows.sort(key=lambda row: row["key"])
    out = io.StringIO()
    writer = csv.DictWriter(out, fieldnames=["key", "pt_BR", "en", "es"], lineterminator="\r\n")
    writer.writeheader()
    writer.writerows(rows)
    I18N_PATH.write_bytes(out.getvalue().encode("utf-8"))
    return added


# --------------------------------------------------------------------------
# Manifesto de ícones
# --------------------------------------------------------------------------


def sync_icons(goods: list[dict]) -> None:
    manifest = load_json(ICON_MANIFEST_PATH)
    icons = manifest["icons"]
    for good in goods:
        icon = icons.get(good["id"])
        if icon is None:
            raise ValueError(f"ícone ausente no manifesto: {good['id']}")
        icon["label"] = good["name"]
        icon["color"] = good["icon_color"]
        icon["tier"] = good["tier"]
    ICON_MANIFEST_PATH.write_text(json.dumps(manifest, ensure_ascii=False, indent=1),
                                  encoding="utf-8", newline="\n")


# --------------------------------------------------------------------------
# Relatório
# --------------------------------------------------------------------------


def write_report(goods: list[dict], recipes: list[dict], spec: dict) -> None:
    lines = [
        "# Cadeias de produção — dados derivados (F1)",
        "",
        "Gerado por `tools/build_goods_chain.py report`. Não editar à mão.",
        "",
        f"- Bens: **{len(goods)}** (21 primários em `order` 0..20, 94 novos em 21..114)",
        f"- Receitas: **{len(recipes)}** (coeficiente unitário padrão onde a spec não traz qty)",
        "- Preços: primários = `calibrated:legacy` (dado já calibrado); novos = `derived:v1`",
        "  pela fórmula `min_receita(Σ qty × preço_insumo / yield) × (1 + margem)`.",
        "- Co-produtos: preço da ROTA PRINCIPAL (a receita de co-produção), com o",
        "  custo do lote rateado por valor de mercado declarado (EIA) — não por tonelada.",
        "- Margens: raw 0 · byproduct 0,10 · intermediate 0,25 · final 0,45 · durável/luxo 0,80.",
        "- Subprodutos sem receita própria: 0,10 × preço do bem que os gera.",
        "- `charcoal` é alias de `coal` (a spec diz que produz o MESMO item).",
        "- Parâmetros de fábrica (`jobs_per_ton`, `wage`, `opex_per_ton`, "
        "`capital_per_capacity`) são **estimativa** de ordem de grandeza até o laboratório de F2.",
        "",
        "## Preços derivados",
        "",
        "| # | bem | tier | kind | setor | preço | procedência |",
        "|---:|---|---|---|---|---:|---|",
    ]
    for good in goods:
        lines.append(
            f"| {good['order']} | `{good['id']}` | {good['tier']} | {good['kind']} | "
            f"{good['sector']} | {good['base_price']:.2f} | `{good['base_price_source']}` |"
        )
    lines += ["", "## Receitas normalizadas", ""]
    for recipe in recipes:
        inputs = ", ".join(f"{item['qty']:g}× `{item['good']}`" for item in recipe["inputs"])
        byproducts = ", ".join(f"{item['qty']:g}× `{item['good']}`" for item in recipe["byproducts"])
        line = f"- `{recipe['id']}` â†’ `{recipe['output']}` (yield {recipe['yield']:g}): {inputs}"
        if byproducts:
            line += f" + subproduto {byproducts}"
        lines.append(line)

    # Co-produtos: método e conta a partir da spec, não de número escolhido.
    goods_by_id = {good["id"]: good for good in goods}
    spec_by_id = {good["id"]: good for good in spec["goods"]}
    margins = {gid: margin_for(good) for gid, good in spec_by_id.items()}
    alloc_by_recipe = co_allocation_by_recipe(spec)
    recipes_by_id = {recipe["id"]: recipe for recipe in recipes}
    lines += ["", "## Co-produtos: rota principal rateada por valor de mercado", "",
              "O custo do lote é rateado pelo valor de mercado de cada saída "
              "(`qty × USD/t`), não por tonelada. USD/t = preço da fonte "
              "(USD/gal) × 42 gal/barril × barris por tonelada.", ""]
    for rid, alloc in sorted(alloc_by_recipe.items()):
        recipe = recipes_by_id.get(rid)
        if recipe is None:
            continue
        batch_cost = sum(float(item["qty"]) * float(goods_by_id[item["good"]]["base_price"])
                         for item in recipe["inputs"])
        unit = co_product_unit_prices(recipe, alloc, batch_cost, margins)
        total_value = sum(
            float(item["qty"]) * float(alloc["usd_per_gallon"][item["good"]])
            * 42.0 * float(alloc["barrels_per_tonne"][item["good"]])
            for item in recipe["outputs"])
        lines += [
            f"### `{rid}` — custo do lote {batch_cost:.2f} (insumos a preço-base)",
            "",
            f"Fonte: {alloc.get('source', '')}",
            "",
            "| saída | qty/lote | USD/t | valor no lote | custo unitário | preço-base |",
            "|---|---:|---:|---:|---:|---:|",
        ]
        for item in recipe["outputs"]:
            co_id = item["good"]
            per_tonne = float(alloc["usd_per_gallon"][co_id]) * 42.0 \
                * float(alloc["barrels_per_tonne"][co_id])
            weight = float(item["qty"]) * per_tonne
            unit_cost = batch_cost * weight / total_value / float(item["qty"])
            lines.append(
                f"| `{co_id}` | {float(item['qty']):g} | {per_tonne:.2f} | "
                f"{weight:.2f} | {unit_cost:.2f} | {unit[co_id]:.2f} |")
        lines.append("")
    REPORT_PATH.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


# --------------------------------------------------------------------------
# Validação
# --------------------------------------------------------------------------


def check_all() -> list[str]:
    problems: list[str] = []
    spec = load_json(SPEC_PATH)
    goods = load_json(GOODS_PATH)
    recipes = load_json(RECIPES_PATH)
    needs = load_json(NEEDS_PATH)
    currencies = load_json(CURRENCIES_PATH)
    icons = load_json(ICON_MANIFEST_PATH)["icons"]
    nations = load_json(NATIONS_PATH)
    spec_ids = [g["id"] for g in spec["goods"]]

    # --- goods
    if len(goods) != 115:
        problems.append(f"goods.json: {len(goods)} bens (esperado 115)")
    if len(spec_ids) != 94:
        problems.append(f"spec: {len(spec_ids)} bens novos (esperado 94)")
    ids = [g.get("id", "") for g in goods]
    if len(set(ids)) != len(ids):
        problems.append("goods.json: ids repetidos")
    if ids[:21] != CANONICAL_PRIMARIES:
        problems.append("goods.json: os 21 primários não estão em 0..20 na ordem canônica")
    if set(ids[21:]) != set(spec_ids):
        faltando = sorted(set(spec_ids) - set(ids[21:]))
        sobrando = sorted(set(ids[21:]) - set(spec_ids))
        problems.append(f"goods.json: bens divergentes da spec (faltam {faltando}, sobram {sobrando})")
    by_id = {g["id"]: g for g in goods}
    for index, good in enumerate(goods):
        label = f"goods.json[{index}] {good.get('id', '?')}"
        if good.get("order") != index:
            problems.append(f"{label}: order {good.get('order')} != {index}")
        if not good.get("id"):
            problems.append(f"{label}: sem id")
        if not isinstance(good.get("name"), str) or not good["name"].strip():
            problems.append(f"{label}: sem name")
        price = good.get("base_price")
        if not isinstance(price, (int, float)) or price <= 0:
            problems.append(f"{label}: base_price inválido ({price!r})")
        if good.get("base_price_source") not in ("calibrated:legacy", "derived:v1") \
                and not str(good.get("base_price_source", "")).startswith(("derived:", "alias:")):
            problems.append(f"{label}: base_price_source inesperado ({good.get('base_price_source')!r})")
        expected_source = "derived" if str(good.get("base_price_source", "")).startswith(("derived", "alias")) else "calibrated"
        if good.get("price_source") != expected_source:
            problems.append(f"{label}: price_source {good.get('price_source')!r} != {expected_source!r}")
        if good.get("sector") not in SECTORS:
            problems.append(f"{label}: setor desconhecido ({good.get('sector')!r})")
        if good.get("kind") not in KINDS:
            problems.append(f"{label}: kind desconhecido ({good.get('kind')!r})")
        if good.get("tier") not in TIERS:
            problems.append(f"{label}: tier desconhecido ({good.get('tier')!r})")
        elif good.get("tier") != KIND_TO_TIER.get(good.get("kind")):
            problems.append(f"{label}: tier {good.get('tier')} incompatível com kind {good.get('kind')}")
        if not isinstance(good.get("durable"), bool):
            problems.append(f"{label}: durable não é booleano")
        if not isinstance(good.get("food"), bool):
            problems.append(f"{label}: food não é booleano")
        if not COLOR_PATTERN.match(str(good.get("icon_color", ""))):
            problems.append(f"{label}: icon_color inválido")
        if not isinstance(good.get("use"), list):
            problems.append(f"{label}: use deve ser lista")

    # --- receitas
    recipe_ids = set()
    producers: dict[str, list[dict]] = {}
    produced_by_byproduct: dict[str, str] = {}
    inputs_used: set[str] = set()
    for index, recipe in enumerate(recipes):
        label = f"recipes.json[{index}] {recipe.get('id', '?')}"
        rid = recipe.get("id")
        if not rid:
            problems.append(f"{label}: sem id")
        elif rid in recipe_ids:
            problems.append(f"{label}: id repetido")
        recipe_ids.add(rid)
        output = recipe.get("output")
        if output not in by_id:
            problems.append(f"{label}: output '{output}' não existe em goods.json")
        else:
            producers.setdefault(output, []).append(recipe)
        if not recipe.get("building"):
            problems.append(f"{label}: sem building")
        if not isinstance(recipe.get("yield"), (int, float)) or recipe["yield"] <= 0:
            problems.append(f"{label}: yield deve ser > 0")
        if not recipe.get("inputs"):
            problems.append(f"{label}: sem insumos")
        input_mass = 0.0
        for item in recipe.get("inputs", []):
            if item.get("good") not in by_id:
                problems.append(f"{label}: insumo '{item.get('good')}' não existe")
            if not isinstance(item.get("qty"), (int, float)) or item["qty"] <= 0:
                problems.append(f"{label}: qty inválida em '{item.get('good')}'")
            else:
                input_mass += float(item["qty"])
            inputs_used.add(item.get("good"))
        outputs = recipe.get("outputs", [])
        output_mass = float(recipe.get("yield", 0.0))
        if outputs:
            if outputs[0].get("good") != output:
                problems.append(f"{label}: outputs[0] difere do output")
            if abs(float(outputs[0].get("qty", -1)) - float(recipe["yield"])) > 1e-9:
                problems.append(f"{label}: outputs[0].qty difere do yield")
            output_mass = 0.0
            for item in outputs:
                if item.get("good") not in by_id:
                    problems.append(f"{label}: saída '{item.get('good')}' não existe")
                if not isinstance(item.get("qty"), (int, float)) or item["qty"] <= 0:
                    problems.append(f"{label}: qty inválida em saída '{item.get('good')}'")
                else:
                    output_mass += float(item["qty"])
                if item.get("good") != output:
                    produced_by_byproduct.setdefault(item.get("good"), rid)
            for item in recipe.get("byproducts", []):
                if item.get("good") not in {out.get("good") for out in outputs[1:]}:
                    problems.append(f"{label}: byproducts e outputs divergem em '{item.get('good')}'")
        else:
            for item in recipe.get("byproducts", []):
                if item.get("good") not in by_id:
                    problems.append(f"{label}: subproduto '{item.get('good')}' não existe")
                if item.get("good") == output:
                    problems.append(f"{label}: subproduto igual ao output")
                if not isinstance(item.get("qty"), (int, float)) or item["qty"] <= 0:
                    problems.append(f"{label}: qty inválida em subproduto '{item.get('good')}'")
                else:
                    output_mass += float(item["qty"])
                produced_by_byproduct.setdefault(item.get("good"), rid)
        # Conservação de massa vale para TODA receita, não só co-produção:
        # saída principal + subprodutos + perda declarada ≤ insumos.
        co_loss = float(recipe.get("co_loss", 0.0))
        if co_loss < 0.0:
            problems.append(f"{label}: co_loss não pode ser negativo")
        if input_mass > 0.0 and output_mass + co_loss > input_mass + 1e-9:
            problems.append(
                f"{label}: conservação — saídas {output_mass:g} + perda {co_loss:g} "
                f"> insumos {input_mass:g}")
        for key in ("jobs_per_ton", "wage", "capital_per_capacity"):
            if not isinstance(recipe.get(key), (int, float)) or recipe[key] <= 0:
                problems.append(f"{label}: {key} deve ser > 0")
        if not isinstance(recipe.get("opex_per_ton"), (int, float)) or recipe["opex_per_ton"] < 0:
            problems.append(f"{label}: opex_per_ton deve ser >= 0")
        if not recipe.get("coef_source"):
            problems.append(f"{label}: sem coef_source")

    # ciclo (por insumo; subproduto não fecha ciclo econômico porque não é insumo de si)
    graph: dict[str, set[str]] = {gid: set() for gid in by_id}
    for recipe in recipes:
        output_ids = [item["good"] for item in recipe.get("outputs", [])]
        if not output_ids and recipe.get("output"):
            output_ids = [recipe["output"]]
        for output_id in output_ids:
            for item in recipe.get("inputs", []):
                if item.get("good") in graph and output_id in graph:
                    graph[output_id].add(item["good"])
    state: dict[str, int] = {}

    def visit(node: str, trail: list[str]) -> None:
        mark = state.get(node, 0)
        if mark == 1:
            problems.append("ciclo de receitas: " + " â†’ ".join(trail + [node]))
            return
        if mark == 2:
            return
        state[node] = 1
        for dep in sorted(graph.get(node, ())):
            visit(dep, trail + [node])
        state[node] = 2

    for gid in by_id:
        visit(gid, [])

    # --- produtor e consumidor de cada bem
    declared_consumers: dict[str, list[str]] = {}
    for need_level in needs["levels"].values():
        for category in need_level["categories"].values():
            for gid in category["substitutes"]:
                declared_consumers.setdefault(gid, []).append("pop_need")
    for gid, good in by_id.items():
        is_primary = gid in CANONICAL_PRIMARIES
        if not is_primary:
            has_producer = gid in producers or gid in produced_by_byproduct or gid in GOOD_ALIASES
            if not has_producer:
                problems.append(f"'{gid}': bem não-primário sem receita nem subproduto declarado")
        consumers: list[str] = []
        if gid in inputs_used:
            consumers.append("recipe_input")
        consumers += declared_consumers.get(gid, [])
        consumers += [tag for tag in good.get("use", []) if tag in INSTITUTIONAL_TAGS]
        if gid in GOOD_ALIASES:
            consumers.append("alias:" + GOOD_ALIASES[gid])
        if good.get("consumer_note"):
            consumers.append("declared:" + good["consumer_note"])
        if not consumers:
            problems.append(f"'{gid}': bem sem consumidor declarado "
                            f"(use={good.get('use')}) — declare em `use` ou `consumer_note`")

    # --- co-produção: rateio por VALOR DE MERCADO declarado (fonte no dado)
    spec_recipes: dict[str, dict] = {}
    for spec_good in spec["goods"]:
        alternatives = spec_good["recipes"]
        for index, recipe in enumerate(alternatives):
            suffix = "_" + chr(ord("a") + index) if len(alternatives) > 1 else ""
            spec_recipes[spec_good["id"] + suffix] = recipe
    for recipe in recipes:
        if not recipe.get("outputs"):
            continue
        rid = recipe["id"]
        label = f"recipes.json {rid}"
        alloc = spec_recipes.get(rid, {}).get("co_allocation")
        if not alloc:
            problems.append(f"{label}: co-produção sem `co_allocation` (fonte do rateio)")
            continue
        if not str(alloc.get("source", "")).strip():
            problems.append(f"{label}: co_allocation sem source")
        for item in recipe["outputs"]:
            co_id = item["good"]
            if float(alloc.get("usd_per_gallon", {}).get(co_id, 0.0)) <= 0.0:
                problems.append(f"{label}: co_allocation sem usd_per_gallon de '{co_id}'")
            if float(alloc.get("barrels_per_tonne", {}).get(co_id, 0.0)) <= 0.0:
                problems.append(f"{label}: co_allocation sem barrels_per_tonne de '{co_id}'")

    # --- needs
    for level_name, level in needs["levels"].items():
        if level_name not in LEVEL_WEIGHTS:
            problems.append(f"needs.json: nível desconhecido {level_name}")
            continue
        if abs(level.get("satisfaction_weight", 0) - LEVEL_WEIGHTS[level_name]) > 1e-9:
            problems.append(f"needs.json: peso de {level_name} diverge do plano")
        for category, data in level["categories"].items():
            if not data.get("substitutes"):
                problems.append(f"needs.json: {level_name}/{category} sem substitutos")
            for gid in data.get("substitutes", []):
                if gid not in by_id:
                    problems.append(f"needs.json: substituto '{gid}' não existe")
            if data.get("per_capita_t_month", 0) <= 0:
                problems.append(f"needs.json: {level_name}/{category} per_capita <= 0")
            if data.get("anchor") not in ("existing_food_need", "estimated"):
                problems.append(f"needs.json: {level_name}/{category} anchor inválido")
    total_weight = sum(level.get("satisfaction_weight", 0) for level in needs["levels"].values())
    if abs(total_weight - 1.0) > 1e-6:
        problems.append(f"needs.json: pesos dos níveis somam {total_weight} (esperado 1.0)")
    replacement = needs.get("durable_replacement_per_year")
    if not isinstance(replacement, (int, float)) or not 0 < replacement <= 1:
        problems.append("needs.json: durable_replacement_per_year deve estar em (0, 1]")

    # --- currencies
    nation_codes = {n["currency"] for n in nations}
    currency_codes = {entry["code"] for entry in currencies["currencies"]}
    if nation_codes != currency_codes:
        problems.append(f"currencies.json: códigos divergentes (faltam {sorted(nation_codes - currency_codes)}, "
                        f"sobram {sorted(currency_codes - nation_codes)})")
    for entry in currencies["currencies"]:
        label = f"currencies.json {entry['code']}"
        if not entry.get("nations"):
            problems.append(f"{label}: sem nações")
        if not entry.get("source"):
            problems.append(f"{label}: sem source")
        rate = entry.get("rate_per_anchor")
        if rate is None:
            if not str(entry.get("source", "")).startswith("unknown:"):
                problems.append(f"{label}: rate null sem source 'unknown:'")
            continue
        if not isinstance(rate, (int, float)) or rate <= 0:
            problems.append(f"{label}: rate_per_anchor deve ser > 0")

    # --- i18n
    i18n_rows = list(csv.DictReader(io.StringIO(I18N_PATH.read_bytes().decode("utf-8"))))
    i18n = {row["key"]: row for row in i18n_rows}
    for gid in by_id:
        for prefix in ("product.", "resource."):
            key = prefix + gid
            row = i18n.get(key)
            if row is None:
                problems.append(f"i18n: falta {key}")
            elif not all((row.get(lang) or "").strip() for lang in ("pt_BR", "en", "es")):
                problems.append(f"i18n: {key} incompleto em pt_BR/en/es")
            elif prefix == "product." and row["pt_BR"].strip() != by_id[gid]["name"].strip():
                problems.append(f"i18n: {key} diverge do name em goods.json")

    # --- ícones
    if set(icons) != set(by_id):
        problems.append(f"manifesto de ícones: {len(icons)} ícones para {len(by_id)} bens "
                        f"(faltam {sorted(set(by_id) - set(icons))}, sobram {sorted(set(icons) - set(by_id))})")
    for gid, good in by_id.items():
        icon = icons.get(gid)
        if icon is None:
            continue
        if icon.get("label") != good["name"]:
            problems.append(f"ícone '{gid}': label diverge do name em goods.json")
        if str(icon.get("color", "")).lower() != good["icon_color"].lower():
            problems.append(f"ícone '{gid}': cor diverge do icon_color em goods.json")
        if icon.get("tier") != good["tier"]:
            problems.append(f"ícone '{gid}': tier diverge do goods.json")
        svg = ICON_SVG_DIR / f"{gid}.svg"
        if not svg.is_file():
            problems.append(f"ícone '{gid}': SVG ausente em {ICON_SVG_DIR.relative_to(ROOT)}")

    # --- preços derivados reproduzíveis
    try:
        primaries = {gid: by_id[gid]["base_price"] for gid in CANONICAL_PRIMARIES}
        expected = compute_prices(spec, recipes, primaries)
        for gid, info in expected.items():
            if gid in primaries:
                continue
            actual = by_id[gid]["base_price"]
            if abs(actual - info["price"]) > 0.011:
                problems.append(f"preço de '{gid}': {actual} != derivado {info['price']} (rode build)")
    except Exception as exc:  # noqa: BLE001 - o check precisa reportar, não estourar
        problems.append(f"preços derivados: {exc}")

    return problems


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------


def cmd_build() -> int:
    spec = load_json(SPEC_PATH)
    manifest = load_json(ICON_MANIFEST_PATH)
    icons = manifest["icons"]
    missing_icons = [g["id"] for g in spec["goods"] if g["id"] not in icons]
    if missing_icons:
        print("ícones ausentes no manifesto:", ", ".join(missing_icons))
        return 1

    legacy_prices = dict(LEGACY_PRIMARY_PRICES)
    if GOODS_PATH.is_file():
        for good in load_json(GOODS_PATH):
            if good.get("id") in legacy_prices and isinstance(good.get("base_price"), (int, float)):
                legacy_prices[good["id"]] = float(good["base_price"])

    recipes = build_recipes(spec)
    primaries = {gid: legacy_prices[gid] for gid in CANONICAL_PRIMARIES}
    prices = compute_prices(spec, recipes, primaries)
    goods = build_goods(spec, prices, icons, legacy_prices)
    needs = build_needs(spec, goods)
    currencies = build_currencies(load_json(NATIONS_PATH))

    dump_json(GOODS_PATH, goods)
    dump_json(RECIPES_PATH, recipes)
    dump_json(NEEDS_PATH, needs)
    dump_json(CURRENCIES_PATH, currencies)
    added = update_i18n(goods)
    sync_icons(goods)
    write_report(goods, recipes, spec)
    print(f"bens: {len(goods)} · receitas: {len(recipes)} · moedas: {len(currencies['currencies'])} "
          f"· i18n novas: {added}")

    problems = check_all()
    for problem in problems:
        print("PROBLEMA:", problem)
    print(f"check: {len(problems)} problema(s)")
    return 1 if problems else 0


def cmd_check() -> int:
    problems = check_all()
    for problem in problems:
        print("PROBLEMA:", problem)
    print(f"check: {len(problems)} problema(s)")
    return 1 if problems else 0


def cmd_report() -> int:
    write_report(load_json(GOODS_PATH), load_json(RECIPES_PATH), load_json(SPEC_PATH))
    print(f"relatório: {REPORT_PATH.relative_to(ROOT)}")
    return 0


def main(argv: list[str]) -> int:
    command = argv[0] if argv else "check"
    if command == "build":
        return cmd_build()
    if command == "check":
        return cmd_check()
    if command == "report":
        return cmd_report()
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
