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
	# opex 2,5/t); madeira é serraria/floresta (mais mecânica, menos gente).
	"soy": {"output": 17, "input": -1, "jobs_per_ton": 7.5,
		"wage": 2.0, "opex_per_ton": 2.5, "input_ratio": 0.0},
	"cotton": {"output": 18, "input": -1, "jobs_per_ton": 7.5,
		"wage": 2.0, "opex_per_ton": 2.5, "input_ratio": 0.0},
	"wood": {"output": 19, "input": -1, "jobs_per_ton": 5.0,
		"wage": 1.5, "opex_per_ton": 3.0, "input_ratio": 0.0},
}

## Cesta alimentar substituível (B3): trigo, arroz, milho e batata — soja fica
## fora por ora. `caloric_factor` converte tonelada real em tonelada-equivalente
## de grão (batata ~1/4 das calorias; fonte FAO em economy_sim.h).
const FOOD_RESOURCES: Array[String] = ["wheat", "rice", "maize", "potato"]

## Minerais/energéticos (extração primária com jazida). A IA só constrói quando
## a cidade tem depósito com reserva restante (GeographyService declara via
## demo_set_deposit).
const MINERAL_RESOURCES: Array[String] = [
	"iron", "coal", "copper", "gas", "oil", "bauxite", "lead", "gold",
	"phosphate", "tungsten", "rare_earths", "sulfur", "diamond", "uranium",
]

## Renováveis não alimentares exportáveis (aptidão agrícola/cobertura florestal).
const RENEWABLE_RESOURCES: Array[String] = ["soy", "cotton", "wood"]

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


static func has(resource: String) -> bool:
	return TEMPLATES.has(resource)


static func output_of(resource: String) -> int:
	return int(TEMPLATES.get(resource, {}).get("output", -1))


## Fator calórico de uma cultura (tonelada-equivalente de grão por tonelada
## real). Batata ~1/4; demais culturas 1.0. Fonte FAO em economy_sim.h.
static func caloric_factor(resource: String) -> float:
	return float(TEMPLATES.get(resource, {}).get("caloric_factor", 1.0))


## true se a cidade já tem uma construção que produz o bem (mesmo output). Usado
## pela IA para preferir expandir um produtor estabelecido em vez de introduzir
## uma cultura/bem novo redundante (fix do churn de milho, B4).
static func has_producer(city: Dictionary, resource: String) -> bool:
	var product := output_of(resource)
	for building in city.get("buildings", []):
		if int(building.get("output_product", -1)) == product:
			return true
	return false


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


## Custo variável estimado por tonelada, com o preço do insumo de mercado.
static func variable_cost(resource: String, city: Dictionary) -> float:
	var template: Dictionary = TEMPLATES[resource]
	var unit := (float(template["jobs_per_ton"]) * float(template["wage"])
		+ float(template["opex_per_ton"]))
	var input_product := int(template["input"])
	if input_product >= 0:
		unit += (float(template["input_ratio"])
			* float(city["markets"][input_product].get("price", 0.0)))
	return unit


## Custo variável por tonelada de uma mina, incluindo o multiplicador de
## profundidade da jazida (opex_factor) que o núcleo C++ aplica ao opex de
## extração (até 3×). Sem jazida declarada, o fator é 1.0 (comportamento
## legado). É a margem "honesta" para decidir se vale abrir a mina: minas muito
## fundas de mineral de baixo valor não devem ser construídas.
static func variable_cost_mined(resource: String, city: Dictionary) -> float:
	var template: Dictionary = TEMPLATES[resource]
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
