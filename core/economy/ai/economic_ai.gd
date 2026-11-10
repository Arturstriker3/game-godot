extends RefCounted
class_name EconomicAI

## Orquestrador da camada de decisão econômica. Roda em intervalo fixo,
## depois do tick do EconomySim, e mantém um log explicável de cada decisão.
## Não usa LLM: as decisões são heurísticas determinísticas.

const PrivateInvestorScript := preload("res://core/economy/ai/private_investor.gd")
const GovernmentAIScript := preload("res://core/economy/ai/government_ai.gd")
const Templates := preload("res://core/economy/ai/industry_templates.gd")

var sim: Object
var geo: Object
var private_investor: Object
var government: Object
var decisions: Array[Dictionary] = []
var interval_days := 7


func setup(sim_node: Object, geography: Object) -> bool:
	sim = sim_node
	geo = geography
	private_investor = PrivateInvestorScript.new()
	private_investor.setup(sim)
	government = GovernmentAIScript.new()
	government.setup(sim)
	return geo.load_data()


## Executa a IA no dia informado. Com force=true, ignora o intervalo.
func run(day: int, force := false) -> Array:
	if not force and day % interval_days != 0:
		return decisions
	var status: Dictionary = sim.get_demo_status()
	private_investor.run(day, status)
	government.run(day, status)
	decisions.append_array(private_investor.drain())
	decisions.append_array(government.drain())
	return decisions


func get_decisions() -> Array:
	return decisions.duplicate(true)


func print_log() -> void:
	for decision in decisions:
		print("[AI] dia %d %-10s %-6s %-10s %-8s custo %.0f | %s" % [
			int(decision.get("day", 0)),
			String(decision.get("agent", "")),
			String(decision.get("city", "")),
			String(decision.get("action", "")),
			String(decision.get("resource", "")),
			float(decision.get("cost", 0.0)),
			String(decision.get("reason", "")),
		])


func write_log(path: String) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		return
	file.store_string(JSON.stringify(decisions, "  "))
	file.close()


func _find_city(status: Dictionary, city_id: String) -> Dictionary:
	for city in status.get("cities", []):
		if String(city["id"]) == city_id:
			return city
	return {}


func _center(city: Dictionary) -> Vector2:
	var center: Array = city.get("center", [0.0, 0.0])
	if center.size() < 2:
		return Vector2.ZERO
	return Vector2(float(center[0]), float(center[1]))
