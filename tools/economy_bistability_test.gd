extends SceneTree

## Teste de regressão da biestabilidade de liquidez (GAME_STATE §15
## "fome com alimento em estoque").
##
## Reproduz a PERTURBAÇÃO que derrubava a GBR: trocar a cultura inicial pela de
## MAIOR aptidão × caloria em TODAS as nações (DEU → milho, trópicos → arroz),
## o que tira a DEU do mercado de trigo e esvazia o trigo tropical. Sem a
## correção de raiz do sinal de escassez (demanda real + renda), Londres entrava
## no equilíbrio de baixo emprego: fazenda a ~4% de utilização e satisfação da
## GBR em ~4% no fim de 730 dias.
##
## O teste roda o CAMINHO DO JOGO, não um mundo mínimo:
##   - as mesmas definições de `core/economy/economy.gd` (região B3, receitas,
##     necessidades, serviços, moedas, emprego setorial, estrutura etária,
##     informalidade/saúde da B7 e camada urbana do §19);
##   - fazendas sem a mitigação + minas semeadas (D-086/D-087);
##   - a IA do NÚCLEO (A5/D-075): `set_ai_enabled(true)` decide dentro do
##     `tick_day()` em C++ — a IA GDScript legada saiu do caminho do jogo.
##
## O alvo antigo de 70% de satisfação media a calibração anterior à B3/B5. Com a
## penalidade de acesso da B5 e sem dado inicial de infraestrutura, a comida do
## mundo inteiro está abaixo do portão (dívida #1 do M2, ABERTA) — manter 70%
## faria o teste medir a calibração, não a regressão. O que este teste guarda é o
## MECANISMO do colapso, medido no fim dos 730 dias:
##   - utilização das fazendas de Londres (o colapso era ~4%);
##   - satisfação de Londres (o colapso era ~4%);
##   - o invariante do mundo (nenhuma violação) e a IA ter decidido de fato.
## Os pisos abaixo são folga sobre o medido na base atual (util 0,80; satisfação
## 0,45), não metas de calibração.
##
## Uso: godot --headless --path . --script res://tools/economy_bistability_test.gd

const GeographyServiceScript := preload("res://core/economy/geography_service.gd")
const RegionDataScript := preload("res://core/economy/region_data.gd")

const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const PORTS_PATH := "res://data/definitions/scenario/ports.json"
const BUILDINGS_PATH := "res://data/definitions/scenario/buildings.json"
const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"
const SERVICES_PATH := "res://data/definitions/economy/services.json"
const CURRENCIES_PATH := "res://data/definitions/economy/currencies.json"
const SECTOR_EMPLOYMENT_PATH := "res://data/definitions/economy/sector_employment.json"
const AGE_STRUCTURE_PATH := "res://data/definitions/population/age_structure_2026.json"
const INFORMALITY_PATH := "res://data/definitions/economy/informality.json"
const HEALTH_ACCESS_PATH := "res://data/definitions/population/health_access.json"
const URBAN_LAND_PATH := "res://data/definitions/economy/urban_land.json"
const HOUSING_CITY_PATH := "res://data/definitions/economy/housing_city.json"
const HOUSING_PATH := "res://data/definitions/economy/housing.json"
const MUNICIPAL_FINANCE_PATH := "res://data/definitions/economy/municipal_finance.json"

const DAYS := 730
## Pisos do mecanismo (o colapso media ~0,04 nos dois; medido hoje: 0,80/0,45).
const LONDON_UTIL_FLOOR := 0.50
const LONDON_SAT_FLOOR := 0.30
const TRACE_EVERY := 90

var _failures: Array[String] = []


func _init() -> void:
	_run()
	if _failures.is_empty():
		print("[bistability] OK: Londres fora do equilíbrio de baixo emprego sob a perturbação de culturas.")
		quit(0)
	else:
		print("[bistability] FALHAS (%d):" % _failures.size())
		for failure in _failures:
			print(" - " + failure)
		quit(1)


func _json(path: String) -> Variant:
	var parsed: Variant = JSON.parse_string(FileAccess.get_file_as_string(path))
	if typeof(parsed) != TYPE_ARRAY and typeof(parsed) != TYPE_DICTIONARY:
		_failures.append("JSON inválido ou ausente: " + path)
	return parsed


## Definição opcional (B7): arquivo ausente é `{}`, como no autoload.
func _json_optional(path: String) -> Dictionary:
	if not FileAccess.file_exists(path):
		return {}
	var parsed: Variant = JSON.parse_string(FileAccess.get_file_as_string(path))
	return parsed if typeof(parsed) == TYPE_DICTIONARY else {}


## Perturbação: troca a cultura inicial pela de maior aptidão × caloria em TODAS
## as nações (o comportamento pré-mitigação). DEU → milho, VEN/SUR/GUY → arroz.
func _full_swap_farms(geo: Object, buildings: Array, cities: Array) -> Array:
	var nation_of := {}
	for city in cities:
		nation_of[String(city.get("id", ""))] = String(city.get("nation", ""))
	var resolved: Array = []
	for building in buildings:
		var entry: Dictionary = building.duplicate(true)
		var tag := String(nation_of.get(String(entry.get("city_id", "")), ""))
		var code := String(geo.nation_code(tag))
		entry["resource"] = geo.best_staple_nation(code) if code != "" else "wheat"
		resolved.append(entry)
	return resolved


func _run() -> void:
	var cities: Array = _json(CITIES_PATH)
	var ports: Array = _json(PORTS_PATH)
	var buildings: Array = _json(BUILDINGS_PATH)
	if cities.is_empty() or buildings.is_empty():
		return

	# B3: o bloco `region` entra antes da criação do mundo, como no jogo.
	RegionDataScript.attach(cities)

	var geo := GeographyServiceScript.new()
	var sim = EconomySim.new()
	# Definições na mesma ordem e com os mesmos arquivos de `economy.gd`.
	sim.set_goods(_json(GOODS_PATH))
	sim.set_recipes(_json(RECIPES_PATH))
	sim.set_needs(_json(NEEDS_PATH))
	sim.set_services(_json(SERVICES_PATH))
	sim.set_currencies(_json(CURRENCIES_PATH))
	sim.set_sector_employment(_json(SECTOR_EMPLOYMENT_PATH))
	sim.set_age_structure(_json(AGE_STRUCTURE_PATH))
	# B7: informalidade e cobertura de saúde, como o jogo. Ausência de arquivo
	# (binário antigo) cai na âncora do núcleo, igual a `economy.gd`.
	if sim.has_method("set_informality"):
		sim.set_informality(_json_optional(INFORMALITY_PATH))
	if sim.has_method("set_health_access"):
		sim.set_health_access(_json_optional(HEALTH_ACCESS_PATH))

	# Perturbação: troca total (DEU → milho, trópicos → arroz), SEM a mitigação.
	var mitigated: Array = geo.resolve_initial_farms(_json(BUILDINGS_PATH), cities)
	buildings = _full_swap_farms(geo, buildings, cities)
	# A perturbação tem de ser real: conta as fazendas cuja cultura muda em
	# relação ao caminho do jogo (senão o teste mede o mundo intacto).
	var swapped := 0
	for index in mini(buildings.size(), mitigated.size()):
		if String(buildings[index].get("resource", "")) \
				!= String(mitigated[index].get("resource", "")):
			swapped += 1
	if swapped == 0:
		_failures.append("perturbação não trocou nenhuma cultura")
	print("[bistability] perturbação: %d de %d fazendas com cultura trocada" % [
		swapped, buildings.size()])
	if geo.has_method("resolve_initial_mines"):
		buildings = geo.resolve_initial_mines(buildings, cities)
	sim.initialize_world(cities, ports, buildings)
	# §19: solo, moradia e prefeitura por cidade (depois do mundo).
	sim.set_urban(_json(URBAN_LAND_PATH), _json(HOUSING_CITY_PATH),
			_json(HOUSING_PATH), _json(MUNICIPAL_FINANCE_PATH))
	geo.apply_to_sim(sim, _cities_with_center(cities))
	# A5/D-075: a decisão econômica roda dentro do núcleo, no fim do dia.
	sim.set_ai_enabled(true)

	print("day\tGBR_farm_workers\tGBR_farm_util\tGBR_budget\tGBR_sat")
	for day in DAYS:
		sim.tick_day()
		if (day + 1) % TRACE_EVERY == 0:
			_trace_london(day + 1, sim)

	var final: Dictionary = sim.get_status()
	var london := _city(final, "GBR-LDN")
	if london.is_empty():
		_failures.append("GBR-LDN não encontrada no status final")
		return

	var heavy: Dictionary = sim.get_city_status("GBR-LDN")
	var pop: Dictionary = heavy.get("pop", {})
	var satisfaction := float(pop.get("satisfaction", 0.0))
	var util := _food_util(heavy)
	print("[bistability] GBR satisfação final = %.1f%%, fazenda = %.2f (pisos %.0f%% / %.2f)" % [
		satisfaction * 100.0, util, LONDON_SAT_FLOOR * 100.0, LONDON_UTIL_FLOOR])

	var violations: Array = final.get("invariant_violations", [])
	if violations.size() > 0:
		_failures.append("invariante violado: %s" % String(violations[0]))
	var commands: Array = sim.get_command_log() if sim.has_method("get_command_log") else []
	if commands.is_empty():
		_failures.append("a IA do núcleo não emitiu comando em %d dias" % DAYS)
	if util < LONDON_UTIL_FLOOR:
		_failures.append("biestabilidade: fazenda de Londres em %.2f < %.2f (colapso)" % [
			util, LONDON_UTIL_FLOOR])
	if satisfaction < LONDON_SAT_FLOOR:
		_failures.append("biestabilidade: satisfação da GBR %.1f%% < %.0f%%" % [
			satisfaction * 100.0, LONDON_SAT_FLOOR * 100.0])


func _city(status: Dictionary, city_id: String) -> Dictionary:
	for city in status.get("cities", []):
		if String(city["id"]) == city_id:
			return city
	return {}


## Utilização média das fazendas de alimento de uma cidade, ponderada pela
## capacidade — a assinatura do equilíbrio de baixo emprego era ~0,04.
func _food_util(heavy: Dictionary) -> float:
	var util_sum := 0.0
	var cap_sum := 0.0
	for building in heavy.get("buildings", []):
		if int(building.get("output_product", -1)) < 0:
			continue
		# Só fazendas de alimento (trigo/arroz/milho/batata/soja), não minas.
		var resource := String(building.get("resource", ""))
		if not (resource in ["wheat", "rice", "maize", "potato", "soy"]):
			continue
		util_sum += float(building.get("utilization", 0.0)) * float(building.get("capacity", 0.0))
		cap_sum += float(building.get("capacity", 0.0))
	return util_sum / cap_sum if cap_sum > 0.0 else 0.0


func _trace_london(day: int, sim: Object) -> void:
	var status: Dictionary = sim.get_status()
	var london := _city(status, "GBR-LDN")
	if london.is_empty():
		return
	var heavy: Dictionary = sim.get_city_status("GBR-LDN")
	var workers := 0.0
	for building in heavy.get("buildings", []):
		var resource := String(building.get("resource", ""))
		if int(building.get("output_product", -1)) >= 0 \
				and resource in ["wheat", "rice", "maize", "potato", "soy"]:
			workers += float(building.get("workers", 0.0))
	print("%d\t%.0f\t%.2f\t%.0f\t%.3f" % [
		day, workers, _food_util(heavy),
		float(london.get("government_budget", 0.0)),
		float((heavy.get("pop", {}) as Dictionary).get("satisfaction", 0.0)),
	])


func _cities_with_center(cities: Array) -> Array:
	var result: Array = []
	for city in cities:
		result.append({
			"id": String(city.get("id", "")),
			"nation": String(city.get("nation", "")),
			"center": Vector2(float(city.get("x", 0.0)), float(city.get("y", 0.0))),
		})
	return result
