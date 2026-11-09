extends RefCounted
class_name IndustryTemplates

## Parâmetros de engenharia das construções usadas pela IA. Ficam separados
## dos dados de mercado e são compartilhados por investidores e governo.

const TEMPLATES := {
	"wheat": {"output": 0, "input": -1, "jobs_per_ton": 7.5,
		"wage": 2.0, "opex_per_ton": 2.5, "input_ratio": 0.0},
	"flour": {"output": 1, "input": 0, "jobs_per_ton": 4.0,
		"wage": 2.0, "opex_per_ton": 4.0, "input_ratio": 1.2},
	"bread": {"output": 2, "input": 1, "jobs_per_ton": 6.67,
		"wage": 1.5, "opex_per_ton": 6.67, "input_ratio": 0.83},
	"iron": {"output": 3, "input": -1, "jobs_per_ton": 3.35,
		"wage": 1.0, "opex_per_ton": 6.65, "input_ratio": 0.0},
}

const FOOD_RESOURCES: Array[String] = ["wheat", "flour", "bread"]
const MINERAL_RESOURCES: Array[String] = ["iron", "coal", "copper", "oil", "gas"]


static func has(resource: String) -> bool:
	return TEMPLATES.has(resource)


static func output_of(resource: String) -> int:
	return int(TEMPLATES.get(resource, {}).get("output", -1))


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
