extends RefCounted
class_name GroupInvestor

## F9 — decisão de investimento por GRUPO/EMPRESA (docs §14).
##
## Cada grupo decide em cadência BAIXA e escalonada: um grupo de índice `i`
## decide nos dias em que `day % interval_days == i % interval_days`. Nunca por
## tick. O trabalho diário é de poucos grupos (≈ nº de grupos / 30) e cada
## decisão é O(cidades candidatas × setor), sem alocar status extra: usa o
## `summary` do tick que o autoload já publica.
##
## Retorno esperado × risco: a margem unitária (preço de mercado − custo
## variável da melhor receita) vira retorno diário sobre o capital; o risco
## desconta confiança da nação (nacionalização), tarifa do anfitrião (no IDE),
## abertura do setor e joint venture. Financia com o caixa do grupo.
##
## A proteção alimentar existente é preservada: enquanto a satisfação alimentar
## média do mundo estiver abaixo de `FOOD_SECURE_SATISFACTION`, o grupo só
## investe em alimento (nunca em bem de luxo/industrial).

const Templates := preload("res://core/economy/ai/industry_templates.gd")
const CultureDb := preload("res://core/culture/culture_db.gd")
const BusinessNameGenerator := preload("res://core/culture/business_name_generator.gd")

## Cadência por grupo (dias). Baixa por desenho: 30 dias.
const INTERVAL_DAYS := 30
## Caixa mínimo para abrir um ativo (abaixo disso o grupo poupa).
const MIN_CAPITAL := 6000.0
## Capacidade máxima de um ativo por decisão (t/dia).
const MAX_CAPACITY := 12.0
## Retorno diário mínimo (margem / capital) para investir, após risco.
const MIN_RETURN := 0.0018
## Satisfação alimentar média abaixo da qual só se investe em comida.
const FOOD_SECURE_SATISFACTION := 0.98
## Fração do caixa comprometida por decisão.
const CASH_SHARE := 0.5

## Setor do bem (goods.json) → setor de nome de empresa (business_names).
const SECTOR_TO_BUSINESS := {
	"agro": "wholesale", "food": "food_service", "mining": "construction",
	"energy": "construction", "metal": "construction",
	"machinery": "construction", "chemical": "construction",
	"wood": "construction", "textile": "retail", "luxury": "retail",
	"packaging": "retail", "electronics": "it",
	"transport": "freight_logistics", "military": "professional_services",
	"construction": "construction", "primary": "wholesale",
	"industry": "construction", "state": "finance",
}

var _sim: Object
var _geo: Object
var _groups: Array = []
var _decisions: Array = []
var _sector_of: Dictionary = {}
var _sector_ready := false
var _investment_status: Dictionary = {}
var _investment_day := -1


func setup(sim: Object, geo: Object) -> void:
	_sim = sim
	_geo = geo
	refresh_groups()


## Recarrega o ranking de grupos do núcleo (sob demanda: não é status do tick).
func refresh_groups() -> void:
	_groups = []
	if _sim != null and _sim.has_method("demo_get_groups"):
		_groups = _sim.demo_get_groups()


func get_decisions() -> Array:
	return _decisions.duplicate(true)


func clear_decisions() -> void:
	_decisions.clear()


## Nomeia os grupos pela cultura de origem (business_name_generator.gd). O
## `place_by_nation` (opcional) dá o nome da cidade-sede usado nos padrões com
## `{place}`. Garante unicidade dentro do lote; o núcleo só armazena.
func name_groups(place_by_nation: Dictionary = {}) -> int:
	if _sim == null or _groups.is_empty():
		return 0
	if not BusinessNameGenerator.load_data() or not CultureDb.load_data():
		return 0
	var generator := BusinessNameGenerator.new()
	var names: Dictionary = {}
	var used: Dictionary = {}
	for group: Dictionary in _groups:
		var id := String(group.get("id", ""))
		if id.is_empty():
			continue
		var origin := String(group.get("origin_nation", ""))
		var sector := String(group.get("sector", ""))
		var culture := CultureDb.national_culture(origin)
		var business_sector := String(SECTOR_TO_BUSINESS.get(sector, "wholesale"))
		var place := String(place_by_nation.get(origin, ""))
		var seed_value := absi(hash(id)) % 100000
		var name := ""
		if culture != "" and BusinessNameGenerator.has_sector(
				BusinessNameGenerator.language_for(culture), business_sector):
			name = generator.generate(culture, business_sector, place, seed_value)
		if name.is_empty():
			# Fallback declarado: rótulo do setor + nação. Nunca inventa cultura.
			name = "%s %s" % [business_sector.capitalize(), origin]
		var key := name.to_lower()
		if used.has(key):
			var suffix := 2
			while used.has("%s %d" % [key, suffix]):
				suffix += 1
			name = "%s %d" % [name, suffix]
			key = name.to_lower()
		used[key] = true
		names[id] = name
	if names.is_empty():
		return 0
	return int(_sim.demo_set_group_names(names))


## Roda a decisão dos grupos cuja vez é o dia. Síncrono e barato: chamado no
## plano da IA, nunca durante o tick.
func run_day(day: int, status: Dictionary) -> void:
	# O caixa e a carteira mudam fora desta camada (tick, política, outros
	# grupos): recarrega o ranking a cada dia de decisao. E O(construcoes),
	# uma vez por dia, nunca por tick.
	refresh_groups()
	if _groups.is_empty():
		return
	var food_secure := _food_secure(status)
	var cities: Array = status.get("cities", [])
	if cities.is_empty():
		return
	for index in _groups.size():
		if index % INTERVAL_DAYS != day % INTERVAL_DAYS:
			continue
		_decide(day, _groups[index], cities, status, food_secure)


## Melhor oportunidade para um grupo (exposta para o teste de aceite). Devolve
## {} quando nada compensa. Não executa.
func best_opportunity(group: Dictionary, cities: Array, status: Dictionary,
		food_secure: bool = true) -> Dictionary:
	var cash := float(group.get("cash", 0.0))
	if cash < MIN_CAPITAL:
		return {}
	_ensure_sector_map()
	var origin := String(group.get("origin_nation", ""))
	var group_sector := String(group.get("sector", ""))
	var best: Dictionary = {}
	var best_score := MIN_RETURN
	for city: Dictionary in cities:
		var city_nation := String(city.get("nation", ""))
		var foreign := city_nation != origin
		for resource in Templates.FOOD_RESOURCES:
			var candidate := _evaluate(group, city, resource, foreign, status)
			if not candidate.is_empty() and float(candidate["score"]) > best_score:
				best = candidate
				best_score = float(candidate["score"])
		if not food_secure:
			continue
		for resource in _sector_of.get(group_sector, []):
			if Templates.FOOD_RESOURCES.has(resource):
				continue
			var candidate := _evaluate(group, city, resource, foreign, status)
			if not candidate.is_empty() and float(candidate["score"]) > best_score:
				best = candidate
				best_score = float(candidate["score"])
	return best


func _decide(day: int, group: Dictionary, cities: Array, status: Dictionary,
		food_secure: bool) -> void:
	var opportunity := best_opportunity(group, cities, status, food_secure)
	if opportunity.is_empty():
		return
	var city: Dictionary = opportunity["city"]
	var capacity := float(opportunity["capacity"])
	var partner := String(opportunity.get("partner", ""))
	var building_id := "GRP-%s-%d" % [
		String(group.get("id", "")).replace("-", ""), day]
	var result: Dictionary = _sim.demo_group_invest(
		String(group["id"]), String(city["id"]), building_id,
		String(opportunity["resource"]), int(opportunity["product"]), capacity,
		String(opportunity.get("recipe", "")), partner)
	var created := bool(result.get("created", false))
	var decision := {
		"day": day,
		"agent": "group",
		"group": String(group.get("id", "")),
		"city": String(city.get("id", "")),
		"action": "build_group" if created else "skip",
		"resource": String(opportunity["resource"]),
		"cost": float(result.get("cost", 0.0)),
		"capacity": capacity,
		"foreign": bool(opportunity.get("foreign", false)),
		"score": float(opportunity["score"]),
		"reason": "retorno %.4f/dia" % float(opportunity["score"]) if created
			else String(result.get("error", "")),
	}
	_decisions.append(decision)
	if created:
		# Mantém o caixa local em dia para a próxima decisão do mesmo grupo.
		for index in _groups.size():
			if String(_groups[index].get("id", "")) == String(group.get("id", "")):
				_groups[index]["cash"] = float(_groups[index].get("cash", 0.0)) \
					- float(result.get("cost", 0.0))
				break


func _evaluate(group: Dictionary, city: Dictionary, resource: String,
		foreign: bool, status: Dictionary) -> Dictionary:
	if not Templates.has(resource):
		return {}
	var template: Dictionary = Templates.all_templates().get(resource, {})
	if template.is_empty():
		return {}
	var product := int(template.get("output", -1))
	if product < 0:
		return {}
	var markets: Array = city.get("markets", [])
	if product >= markets.size():
		return {}
	var market: Dictionary = markets[product]
	var price := float(market.get("price", 0.0))
	var shortage := float(market.get("shortage", 0.0))
	var sold := float(market.get("sold", 0.0))
	var desired := float(market.get("desired", 0.0))
	if price <= 0.0 or (shortage <= 0.0 and sold <= 0.0 and desired <= 0.0):
		return {}
	var is_food := Templates.FOOD_RESOURCES.has(resource)
	# Nunca investir em comida onde já há excesso local.
	if is_food and shortage <= 0.0:
		return {}
	var city_nation := String(city.get("nation", ""))
	var group_sector := String(group.get("sector", ""))
	if foreign:
		if city_nation == String(group.get("origin_nation", "")):
			return {}
		# IDE só quando o mercado local justifica e a abertura permite.
		if shortage <= 0.0 and desired < 1.0:
			return {}
		var policy := _policy(city_nation, group_sector)
		var mode := int(policy.get("mode", 0))
		if mode == 3:
			return {}
		if mode == 1 and not _is_incumbent(group, city_nation, resource, status):
			return {}
	# Unidade de custo (melhor receita corrente na cidade).
	var recipe: Dictionary = Templates.best_recipe(resource, city)
	var recipe_id := String(recipe.get("id", ""))
	if template.has("recipes") and recipe.is_empty():
		return {}
	var unit_cost := Templates.variable_cost(resource, city)
	var margin := price - unit_cost
	if margin <= 0.0:
		return {}
	var capital_per_capacity := float(template.get("capital_per_capacity", 2000.0))
	if not recipe.is_empty():
		capital_per_capacity = float(recipe.get("capital_per_capacity", 2000.0))
	if capital_per_capacity <= 0.0:
		return {}
	var cash := float(group.get("cash", 0.0))
	var capacity := clampf(cash * CASH_SHARE / capital_per_capacity, 0.5,
		MAX_CAPACITY)
	var cost := capacity * capital_per_capacity
	if cost > cash:
		capacity = cash / capital_per_capacity
		cost = capacity * capital_per_capacity
	if capacity < 0.5:
		return {}
	# Rampa de operação (fábrica nova começa em 25%, como no núcleo).
	var ramp := 1.0 if is_food else 0.5
	var profit_day := margin * capacity * ramp
	if profit_day <= 0.0:
		return {}
	var risk := 1.0
	var partner := ""
	var local_share := 0.0
	if foreign:
		var policy := _policy(city_nation, group_sector)
		var mode := int(policy.get("mode", 0))
		var tariff := float(_sim.demo_get_tariff(city_nation,
			String(group.get("origin_nation", "")), resource))
		# IDE evita a tarifa: quanto maior a tarifa, mais o investimento local
		# compensa (o risco de mercado cresce, mas o prêmio também).
		risk *= 1.0 + maxf(0.0, 0.4 - tariff) * 2.0
		risk *= 1.0 + (1.0 - _confidence(city_nation, status)) * 2.0
		if mode == 2:
			local_share = float(policy.get("min_local", 0.3))
			if local_share <= 0.0:
				local_share = 0.3
			partner = _partner_for(group, city_nation, cost * local_share)
			if partner.is_empty():
				return {}
			risk *= 1.0 + local_share
	var score := profit_day / maxf(cost, 1.0) / risk
	return {
		"city": city,
		"resource": resource,
		"product": product,
		"recipe": recipe_id,
		"capacity": capacity,
		"cost": cost,
		"margin": margin,
		"score": score,
		"foreign": foreign,
		"partner": partner,
		"local_share": local_share,
	}


func _policy(nation: String, sector: String) -> Dictionary:
	var policies: Array = _investment_status.get("fdi_policies", [])
	for entry: Dictionary in policies:
		if String(entry.get("nation", "")) == nation \
				and String(entry.get("sector", "")) == sector:
			return entry
	return {"mode": 0, "min_local": 0.0}


func _confidence(nation: String, status: Dictionary) -> float:
	if _investment_day != int(status.get("day", -1)):
		_investment_day = int(status.get("day", -1))
		if _sim.has_method("demo_get_investment_status"):
			_investment_status = _sim.demo_get_investment_status()
	var nations: Dictionary = _investment_status.get("nations", {})
	var row: Dictionary = nations.get(nation, {})
	return float(row.get("confidence", 1.0))


func _is_incumbent(group: Dictionary, nation: String, resource: String,
		status: Dictionary) -> bool:
	var group_id := String(group.get("id", ""))
	for city: Dictionary in status.get("cities", []):
		if String(city.get("nation", "")) != nation:
			continue
		for building: Dictionary in city.get("buildings", []):
			if String(building.get("owner_group", "")) != group_id:
				continue
			if String(building.get("resource", "")) == resource:
				return true
	return false


func _partner_for(group: Dictionary, nation: String, required: float) -> String:
	for candidate: Dictionary in _groups:
		if String(candidate.get("id", "")) == String(group.get("id", "")):
			continue
		if String(candidate.get("origin_nation", "")) != nation:
			continue
		if bool(candidate.get("state_owned", false)):
			continue
		if float(candidate.get("cash", 0.0)) >= required:
			return String(candidate.get("id", ""))
	return ""


func _food_secure(status: Dictionary) -> bool:
	var total := 0.0
	var count := 0
	for city: Dictionary in status.get("cities", []):
		var pop: Dictionary = city.get("pop", {})
		if pop.has("satisfaction"):
			total += float(pop.get("satisfaction", 1.0))
			count += 1
	if count == 0:
		return true
	return total / float(count) >= FOOD_SECURE_SATISFACTION


## Mapa setor → [recursos] a partir de goods.json (uma leitura por instância).
func _ensure_sector_map() -> void:
	if _sector_ready:
		return
	_sector_ready = true
	_sector_of.clear()
	var file := FileAccess.open("res://data/definitions/economy/goods.json",
		FileAccess.READ)
	if file == null:
		return
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	if typeof(data) != TYPE_ARRAY:
		return
	for entry: Variant in data:
		if typeof(entry) != TYPE_DICTIONARY:
			continue
		var item: Dictionary = entry
		var sector := String(item.get("sector", ""))
		var id := String(item.get("id", ""))
		if sector.is_empty() or id.is_empty():
			continue
		if not _sector_of.has(sector):
			_sector_of[sector] = []
		(_sector_of[sector] as Array).append(id)
