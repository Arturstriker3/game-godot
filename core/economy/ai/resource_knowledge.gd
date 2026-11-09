extends RefCounted
class_name ResourceKnowledge

## Conhecimento mineral dos agentes. Nenhum agente vê todos os depósitos:
## só entram no conhecimento os depósitos revelados por prospecção (ou por
## descoberta explícita). A fonte é sempre o GeographyService.

const PROSPECT_COST_PER_SITE := 2000.0

var _geo: Object
var _known: Dictionary = {}  # "city_id|resource" -> Array[Dictionary]


func setup(geo: Object) -> void:
	_geo = geo


func known_deposits(city_id: String, center: Vector2, resource: String) -> Array:
	var key := _key(city_id, resource)
	if not _known.has(key):
		_known[key] = []
	return _known[key]


func add_known(city_id: String, center: Vector2, resource: String, deposit: Dictionary) -> void:
	var entries := known_deposits(city_id, center, resource)
	var deposit_id := String(deposit.get("id", ""))
	for entry in entries:
		if String(entry.get("id", "")) == deposit_id:
			return
	entries.append(deposit.duplicate(true))


func has_known(city_id: String, center: Vector2, resource: String) -> bool:
	return not known_deposits(city_id, center, resource).is_empty()


func known_quality(city_id: String, center: Vector2, resource: String) -> float:
	var best := 0.0
	for entry in known_deposits(city_id, center, resource):
		best = maxf(best, float(entry.get("quality", 0.5)))
	return best


func known_quantity_kt(city_id: String, center: Vector2, resource: String) -> float:
	var total := 0.0
	for entry in known_deposits(city_id, center, resource):
		total += float(entry.get("recoverable_kt", 0.0))
	return total


## Depósitos do território ainda não conhecidos, ordenados por dificuldade.
func unrevealed_deposits(city_id: String, center: Vector2, resource: String) -> Array:
	if _geo == null:
		return []
	var all: Array = _geo.deposits_for(city_id, center)
	var known_ids := {}
	for entry in known_deposits(city_id, center, resource):
		known_ids[String(entry.get("id", ""))] = true

	var candidates: Array = []
	for deposit in all:
		if String(deposit.get("resource", "")) != resource:
			continue
		if known_ids.has(String(deposit.get("id", ""))):
			continue
		candidates.append(deposit)
	candidates.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		var difficulty_a := float(a.get("discovery_difficulty", 1.0))
		var difficulty_b := float(b.get("discovery_difficulty", 1.0))
		if is_equal_approx(difficulty_a, difficulty_b):
			return String(a.get("id", "")) < String(b.get("id", ""))
		return difficulty_a < difficulty_b
	)
	return candidates


## Revela até floor(budget / custo_por_site) depósitos desconhecidos.
func prospect(city_id: String, center: Vector2, resource: String,
		budget: float) -> Array:
	var sites := int(floor(budget / PROSPECT_COST_PER_SITE))
	if sites <= 0:
		return []
	var candidates := unrevealed_deposits(city_id, center, resource)
	var revealed: Array = []
	for index in mini(sites, candidates.size()):
		var deposit: Dictionary = candidates[index]
		add_known(city_id, center, resource, deposit)
		revealed.append(deposit)
	return revealed


func _key(city_id: String, resource: String) -> String:
	return "%s|%s" % [city_id, resource]
