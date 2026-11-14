extends RefCounted
class_name IndustryTemplates

## Parâmetros de engenharia das construções usadas pela IA. Ficam separados
## dos dados de mercado e são compartilhados por investidores e governo.

const TEMPLATES := {
	"wheat": {"output": 0, "input": -1, "jobs_per_ton": 7.5,
		"wage": 2.0, "opex_per_ton": 2.5, "input_ratio": 0.0, "caloric_factor": 1.0},
	"iron": {"output": 1, "input": -1, "jobs_per_ton": 3.35,
		"wage": 1.0, "opex_per_ton": 6.65, "input_ratio": 0.0},
	"rice": {"output": 2, "input": -1, "jobs_per_ton": 7.5,
		"wage": 2.0, "opex_per_ton": 2.5, "input_ratio": 0.0, "caloric_factor": 1.0},
	"maize": {"output": 3, "input": -1, "jobs_per_ton": 7.5,
		"wage": 2.0, "opex_per_ton": 2.5, "input_ratio": 0.0, "caloric_factor": 1.0},
	"potato": {"output": 16, "input": -1, "jobs_per_ton": 7.5,
		"wage": 2.0, "opex_per_ton": 2.5, "input_ratio": 0.0, "caloric_factor": 0.25},

	# --- Extração primária de minerais/energéticos (B4) ----------------------
	# Ordem de grandeza (não precisão): jobs_per_ton = empregos por t/dia,
	# wage = salário nominal por trabalhador/dia, opex_per_ton = custo variável
	# por tonelada. Base: mina de ferro do cenário demo (3,35 empregos/t, salário
	# 1,0, opex 6,65/t — a referência da cadeia viva). As demais derivam por
	# intensidade relativa: bulk a céu aberto (carvão, bauxita, fosfato, enxofre)
	# é mais barato e menos mão-de-obra/t; poços (petróleo/gás) são automatizados
	# (poucos empregos/t); metais profundos e metais de alto valor (cobre, chumbo,
	# tungstênio, terras raras, urânio) têm mais opex; metais preciosos (ouro,
	# diamante) têm alto opex/t mas preço muito superior. A profundidade da jazida
	# ainda multiplica o opex no núcleo (demo_depth_opex_factor, até 3×).
	"coal": {"output": 12, "input": -1, "jobs_per_ton": 1.5,
		"wage": 1.0, "opex_per_ton": 2.5, "input_ratio": 0.0},
	"copper": {"output": 13, "input": -1, "jobs_per_ton": 3.0,
		"wage": 1.0, "opex_per_ton": 8.0, "input_ratio": 0.0},
	"gas": {"output": 14, "input": -1, "jobs_per_ton": 0.6,
		"wage": 1.5, "opex_per_ton": 4.0, "input_ratio": 0.0},
	"oil": {"output": 15, "input": -1, "jobs_per_ton": 0.8,
		"wage": 1.5, "opex_per_ton": 6.0, "input_ratio": 0.0},
	"bauxite": {"output": 4, "input": -1, "jobs_per_ton": 2.0,
		"wage": 1.0, "opex_per_ton": 3.0, "input_ratio": 0.0},
	"lead": {"output": 5, "input": -1, "jobs_per_ton": 3.0,
		"wage": 1.0, "opex_per_ton": 8.0, "input_ratio": 0.0},
	"gold": {"output": 6, "input": -1, "jobs_per_ton": 8.0,
		"wage": 1.5, "opex_per_ton": 40.0, "input_ratio": 0.0},
	"phosphate": {"output": 7, "input": -1, "jobs_per_ton": 2.0,
		"wage": 1.0, "opex_per_ton": 3.0, "input_ratio": 0.0},
	"tungsten": {"output": 8, "input": -1, "jobs_per_ton": 5.0,
		"wage": 1.2, "opex_per_ton": 30.0, "input_ratio": 0.0},
	"rare_earths": {"output": 9, "input": -1, "jobs_per_ton": 5.0,
		"wage": 1.2, "opex_per_ton": 35.0, "input_ratio": 0.0},
	"sulfur": {"output": 10, "input": -1, "jobs_per_ton": 2.0,
		"wage": 1.0, "opex_per_ton": 2.0, "input_ratio": 0.0},
	"diamond": {"output": 11, "input": -1, "jobs_per_ton": 10.0,
		"wage": 1.5, "opex_per_ton": 50.0, "input_ratio": 0.0},
	"uranium": {"output": 20, "input": -1, "jobs_per_ton": 4.0,
		"wage": 1.5, "opex_per_ton": 45.0, "input_ratio": 0.0},

	# --- Renováveis não alimentares (B4) -------------------------------------
	# Soja/algodão são lavouras (mesma base do trigo: 7,5 empregos/t, salário 2,0,
	# opex 2,5/t); madeira é serraria/floresta (mais mecânica, menos gente). A
	# soja também é alimento da cesta (B3, fator calórico 1,3 — fonte USDA/FAO
	# em economy_sim.h): entra na cesta substituível, não mais como exportável.
	"soy": {"output": 17, "input": -1, "jobs_per_ton": 7.5,
		"wage": 2.0, "opex_per_ton": 2.5, "input_ratio": 0.0, "caloric_factor": 1.3},
	"cotton": {"output": 18, "input": -1, "jobs_per_ton": 7.5,
		"wage": 2.0, "opex_per_ton": 2.5, "input_ratio": 0.0},
	"wood": {"output": 19, "input": -1, "jobs_per_ton": 5.0,
		"wage": 1.5, "opex_per_ton": 3.0, "input_ratio": 0.0},
}

## Cesta alimentar substituível (B3): trigo, arroz, milho, batata e soja.
## `caloric_factor` converte tonelada real em tonelada-equivalente de grão
## (batata ~1/4; soja ~1,3 — fonte FAO/USDA em economy_sim.h).
const FOOD_RESOURCES: Array[String] = ["wheat", "rice", "maize", "potato", "soy"]

## Minerais/energéticos (extração primária com jazida). A IA só constrói quando
## a cidade tem depósito com reserva restante (GeographyService declara via
## demo_set_deposit).
const MINERAL_RESOURCES: Array[String] = [
	"iron", "coal", "copper", "gas", "oil", "bauxite", "lead", "gold",
	"phosphate", "tungsten", "rare_earths", "sulfur", "diamond", "uranium",
]

## Renováveis não alimentares exportáveis (aptidão agrícola/cobertura florestal).
## Soja saiu daqui: agora é alimento da cesta (B3).
const RENEWABLE_RESOURCES: Array[String] = ["cotton", "wood"]

## Bens cujo fator de produção divide a mesma terra/trabalho da cesta alimentar
## (lavouras industriais e silvicultura). A IA não os abre em cidade com déficit
## alimentar estrutural: a prioridade é comida, e a fazenda de algodão/madeira
## competiria pela bacia e pela mão de obra.
const LAND_COMPETING_RESOURCES: Array[String] = ["cotton", "wood"]

## Bens bulk de baixo valor (B4): extratores de alto volume e margem fina por
## tonelada, com reserva de décadas. O investidor privado usa um horizonte de
## payback maior para eles do que para metais/energéticos de margem alta — senão
## a mina de enxofre (margem ~7–11/t → payback 180–290 d) nunca fecha a conta de
## 150 dias e fica com extração 0. Só o enxofre entra aqui: carvão (margem ~2/t,
## payback ~1000 d) é fino demais até para o horizonte estendido e segue só com a
## estatal; fosfato/bauxita já fluem via `observed_cost` (a estatal abre e o
## investidor observa o custo real) — relaxá-los superexpandiria e tiraria
## firma/investimento dos renováveis (soja/madeira) que vêm depois na iteração.
const BULK_LOW_VALUE_RESOURCES: Array[String] = ["sulfur"]

## Dados das cadeias (F2): receitas e ids de produto vêm dos mesmos JSON que o
## núcleo carrega. O template de uma receita NÃO fixa índices: o índice é o
## `order` do goods.json, preservando os 21 primários em 0..20.
const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"

static var _loaded := false
static var _goods_index: Dictionary = {}
static var _recipes_by_output: Dictionary = {}
static var _recipe_templates: Dictionary = {}
static var _all_cache: Dictionary = {}
## produto (índice do núcleo) → nome do recurso/template (para atravessar cadeias).
static var _resource_by_product: Dictionary = {}
## Templates NÃO alimentares pré-achatados como Array de entradas (evita o
## `FOOD_RESOURCES.has` e o hash de String por template no laço quente do
## investidor — 114 bens × cidade × dia).
static var _non_food_templates: Array = []


static func _ensure_loaded() -> void:
	if _loaded:
		return
	_loaded = true
	_goods_index.clear()
	_recipes_by_output.clear()
	_recipe_templates.clear()
	_all_cache.clear()

	var goods_file := FileAccess.open(GOODS_PATH, FileAccess.READ)
	if goods_file != null:
		var goods: Variant = JSON.parse_string(goods_file.get_as_text())
		goods_file.close()
		if typeof(goods) == TYPE_ARRAY:
			for index in (goods as Array).size():
				var entry: Variant = goods[index]
				if typeof(entry) != TYPE_DICTIONARY:
					continue
				var id := String((entry as Dictionary).get("id", ""))
				if id.is_empty():
					continue
				_goods_index[id] = int((entry as Dictionary).get("order", index))

	var recipes_file := FileAccess.open(RECIPES_PATH, FileAccess.READ)
	if recipes_file == null:
		return
	var recipes: Variant = JSON.parse_string(recipes_file.get_as_text())
	recipes_file.close()
	if typeof(recipes) != TYPE_ARRAY:
		return
	for entry: Variant in recipes:
		if typeof(entry) != TYPE_DICTIONARY:
			continue
		var recipe: Dictionary = entry
		var output := String(recipe.get("output", ""))
		if output.is_empty() or not _goods_index.has(output):
			continue
		var inputs: Array = []
		for item: Variant in recipe.get("inputs", []):
			if typeof(item) != TYPE_DICTIONARY:
				continue
			var good := String((item as Dictionary).get("good", ""))
			if not _goods_index.has(good):
				continue
			inputs.append({
				"good": good,
				"product": int(_goods_index[good]),
				"qty": float((item as Dictionary).get("qty", 1.0)),
			})
		if inputs.is_empty():
			continue
		var normalized := {
			"id": String(recipe.get("id", "")),
			"output_id": output,
			"output": int(_goods_index[output]),
			"sector": String(recipe.get("sector", "")),
			"yield": maxf(0.001, float(recipe.get("yield", 1.0))),
			"inputs": inputs,
			"jobs_per_ton": float(recipe.get("jobs_per_ton", 7.5)),
			"wage": float(recipe.get("wage", 2.0)),
			"opex_per_ton": float(recipe.get("opex_per_ton", 2.5)),
			"capital_per_capacity": float(recipe.get("capital_per_capacity", 2000.0)),
			"byproducts": recipe.get("byproducts", []),
		}
		if not _recipes_by_output.has(output):
			_recipes_by_output[output] = []
		(_recipes_by_output[output] as Array).append(normalized)

	for output: String in _recipes_by_output:
		var list: Array = _recipes_by_output[output]
		# Template de vitrine: a primeira alternativa declarada define a
		# engenharia (jobs/opex); o custo real é sempre recalculado pela
		# alternativa mais barata disponível (best_recipe).
		var first: Dictionary = list[0]
		_recipe_templates[output] = {
			"output": int(first["output"]),
			"output_id": output,
			"input": int((first["inputs"][0] as Dictionary)["product"]),
			"inputs": first["inputs"],
			"recipe": String(first["id"]),
			"recipes": list,
			"yield": float(first["yield"]),
			"jobs_per_ton": float(first["jobs_per_ton"]),
			"wage": float(first["wage"]),
			"opex_per_ton": float(first["opex_per_ton"]),
			"capital_per_capacity": float(first["capital_per_capacity"]),
			"input_ratio": 0.0,
			"caloric_factor": 0.0,
		}

	_all_cache = TEMPLATES.duplicate(true)
	for output: String in _recipe_templates:
		if not _all_cache.has(output):
			_all_cache[output] = _recipe_templates[output]

	_resource_by_product.clear()
	for resource: String in _all_cache:
		var product := int((_all_cache[resource] as Dictionary).get("output", -1))
		if product >= 0 and not _resource_by_product.has(product):
			_resource_by_product[product] = resource

	# Lista plana dos templates não alimentares, na MESMA ordem de _all_cache
	# (primários por declaração, depois receitas por ordem do recipes.json).
	# Preserva a ordem de decisão do laço original.
	_non_food_templates.clear()
	for resource: String in _all_cache:
		if FOOD_RESOURCES.has(resource):
			continue
		var template: Dictionary = _all_cache[resource]
		_non_food_templates.append({
			"resource": resource,
			"output": int(template.get("output", -1)),
			"is_recipe": template.has("recipes"),
			"template": template,
		})


## Todos os templates (primários + receitas industriais), carregados uma vez.
static func all_templates() -> Dictionary:
	_ensure_loaded()
	return _all_cache


## Lista pré-achatada dos templates não alimentares (ordem estável = ordem de
## _all_cache). Usada pelo laço quente do investidor privado.
static func non_food_templates() -> Array:
	_ensure_loaded()
	return _non_food_templates


## Receitas alternativas de um bem (array vazio quando é primário).
static func recipes_for(resource: String) -> Array:
	_ensure_loaded()
	return _recipes_by_output.get(resource, [])


## Preço de mercado JÁ é âncora XGS desde a F5/opção c (`market.price` e
## `import_price` são âncora); a moeda local só existe na exibição
## (`price_local = preço ÷ taxa`). A IA compara direto, sem converter.
static func anchor_price(city: Dictionary, product: int) -> float:
	var markets: Array = city.get("markets", [])
	if product < 0 or product >= markets.size():
		return 0.0
	return float((markets[product] as Dictionary).get("price", 0.0))


## Alternativa de menor custo corrente na cidade (preço dos insumos / yield).
## Devolve {} quando nenhuma receita tem insumo disponível.
static func best_recipe(resource: String, city: Dictionary) -> Dictionary:
	var best: Dictionary = {}
	var best_cost := INF
	for recipe: Dictionary in recipes_for(resource):
		var cost := 0.0
		var available := true
		for item: Dictionary in recipe["inputs"]:
			var product := int(item["product"])
			var markets: Array = city.get("markets", [])
			if product < 0 or product >= markets.size():
				available = false
				break
			var price := anchor_price(city, product)
			if price <= 0.0:
				price = 1.0
			cost += float(item["qty"]) * price / float(recipe["yield"])
		if available and (best.is_empty() or cost < best_cost):
			best_cost = cost
			best = recipe
	return best



static func has(resource: String) -> bool:
	_ensure_loaded()
	return TEMPLATES.has(resource) or _recipe_templates.has(resource)


static func output_of(resource: String) -> int:
	_ensure_loaded()
	if TEMPLATES.has(resource):
		return int(TEMPLATES[resource].get("output", -1))
	return int(_recipe_templates.get(resource, {}).get("output", -1))


## Fator calórico de uma cultura (tonelada-equivalente de grão por tonelada
## real). Batata ~1/4; demais culturas 1.0. Fonte FAO em economy_sim.h.
static func caloric_factor(resource: String) -> float:
	if TEMPLATES.has(resource):
		return float(TEMPLATES[resource].get("caloric_factor", 1.0))
	return float(_recipe_templates.get(resource, {}).get("caloric_factor", 1.0))


## Conjunto O(1) dos produtos produzidos por construções locais (chave =
## output_product). A IA consulta isso em _input_available em vez de varrer
## city["buildings"] por insumo a cada receita do laço quente (custo medido:
## receita × insumo × construção × template por cidade).
static func local_products(city: Dictionary) -> Dictionary:
	var products: Dictionary = {}
	for building in city.get("buildings", []):
		var product := int(building.get("output_product", -1))
		if product >= 0:
			products[product] = true
	return products


## true se a cidade já tem uma construção que produz o bem (mesmo output). Usado
## pela IA para preferir expandir um produtor estabelecido em vez de introduzir
## uma cultura/bem novo redundante (fix do churn de milho, B4).
static func has_producer(city: Dictionary, resource: String) -> bool:
	var product := output_of(resource)
	for building in city.get("buildings", []):
		if int(building.get("output_product", -1)) == product:
			return true
	return false


## Capacidade NOMINAL (t/dia) já instalada na cidade para um produto (soma de
## `capacity` das construções que produzem o bem). É o quanto da terra/limite da
## cidade já está ocupado — usado pela IA para não exceder o teto de capacidade
## por área cultivável (terra limitada, geography_service).
static func existing_capacity(city: Dictionary, product: int) -> float:
	var total := 0.0
	for building in city.get("buildings", []):
		if int(building.get("output_product", -1)) == product:
			total += float(building.get("capacity", 0.0))
	return total


## Produção local de alimento hoje, em toneladas-equivalentes de grão (soma do
## supply dos alimentos da cesta × fator calórico). É o contraponto do "need" do
## POP: a diferença indica o quanto a cidade depende de importação.
static func local_food_supply(city: Dictionary) -> float:
	var total := 0.0
	for resource in FOOD_RESOURCES:
		var template: Dictionary = TEMPLATES[resource]
		var product := int(template["output"])
		var supply := float(city["markets"][product].get("supply", 0.0))
		total += supply * float(template.get("caloric_factor", 1.0))
	return total


## Necessidade alimentar da cidade em t-eq de grão (mesma unidade de
## `pop.need` e de `local_food_supply`). 0 quando o status não a declara.
static func food_need(city: Dictionary) -> float:
	return maxf(0.0, float(city.get("pop", {}).get("need", 0.0)))


## Nome do recurso/template que produz um produto do núcleo ("" quando o
## produto não tem template conhecido). Usado para atravessar cadeias: o
## insumo ausente pode ser produzido localmente ou comprado de outra cidade.
static func resource_for_product(product: int) -> String:
	_ensure_loaded()
	return String(_resource_by_product.get(product, ""))


## Custo variável estimado por tonelada, com o preço do insumo de mercado.
static func variable_cost(resource: String, city: Dictionary) -> float:
	var template: Dictionary = all_templates().get(resource, {})
	if template.is_empty():
		return 0.0
	var unit := (float(template["jobs_per_ton"]) * float(template["wage"])
		+ float(template["opex_per_ton"]))
	if template.has("recipes"):
		var recipe := best_recipe(resource, city)
		if recipe.is_empty():
			return unit
		var inputs: Array = recipe["inputs"]
		for item: Dictionary in inputs:
			var product := int(item["product"])
			unit += (float(item["qty"]) / float(recipe["yield"])
				* anchor_price(city, product))
		return unit
	var input_product := int(template["input"])
	if input_product >= 0:
		unit += (float(template["input_ratio"])
			* anchor_price(city, input_product))
	return unit


## Custo variável por tonelada de uma mina, incluindo o multiplicador de
## profundidade da jazida (opex_factor) que o núcleo C++ aplica ao opex de
## extração (até 3×). Sem jazida declarada, o fator é 1.0 (comportamento
## legado). É a margem "honesta" para decidir se vale abrir a mina: minas muito
## fundas de mineral de baixo valor não devem ser construídas.
static func variable_cost_mined(resource: String, city: Dictionary) -> float:
	var template: Dictionary = all_templates()[resource]
	var opex_factor := 1.0
	var deposits: Dictionary = city.get("deposits", {})
	if deposits.has(resource):
		opex_factor = float(deposits[resource].get("opex_factor", 1.0))
	return (float(template["jobs_per_ton"]) * float(template["wage"])
		+ float(template["opex_per_ton"]) * opex_factor)


## Demanda externa declarada (t/dia) de um produto na cidade; 0 quando ausente.
## É o teto de exportação que o GeographyService declara como a participação da
## nação no mercado mundial (reserva esgotada em ~25 anos).
static func external_demand_of(city: Dictionary, product: int) -> float:
	var demand: Array = city.get("external_demand", [])
	if product < 0 or product >= demand.size():
		return 0.0
	return float(demand[product])


## Custo variável de um produtor existente, usando custos e produção reais.
static func observed_cost(city: Dictionary, product: int) -> float:
	var total_cost := 0.0
	var total_output := 0.0
	for building in city.get("buildings", []):
		if int(building.get("output_product", -1)) != product:
			continue
		var output := (float(building.get("capacity", 0.0))
			* float(building.get("utilization", 0.0)))
		if output <= 0.0:
			continue
		total_cost += float(building.get("costs", 0.0))
		total_output += output
	return total_cost / total_output if total_output > 0.0 else -1.0
