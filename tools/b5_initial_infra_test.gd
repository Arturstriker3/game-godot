extends SceneTree

## B5 (D-089) — INFRAESTRUTURA INICIAL REAL. Teste do dado + da ponte ao núcleo.
##
## 1. O arquivo `data/definitions/scenario/state_infrastructure_2026.json` é lido
##    e a regra `% -> nível 0..3` aplicada em GDScript
##    (`core/economy/initial_infra.gd`) reproduz EXATAMENTE o campo
##    `infra_levels` pré-calculado pelo construtor do dado, para os 4.226
##    estados — a regra do jogo e a do validador são a mesma.
## 2. Os exemplos do `docs/data/state_infrastructure.md` §5 batem
##    (BRA/IND/NGA/DEU/CHN, granularidade nacional).
## 3. Estado sem observação (`no_data`) → nível 0 (ausência explícita) e estado
##    sem rural mas com total → fallback declarado no arquivo.
## 4. Núcleo: `set_region_infra` aplica os níveis por estado às regiões e o save
##    os persiste; região com mais de um estado usa o MENOR nível (pior caso) e
##    estado sem entrada vale 0.
##
## Uso: godot --headless --path . --script res://tools/b5_initial_infra_test.gd

const InitialInfraScript := preload("res://core/economy/initial_infra.gd")
const RegionDataScript := preload("res://core/economy/region_data.gd")

const STATE_INFRA_PATH := "res://data/definitions/scenario/state_infrastructure_2026.json"
const CITIES_PATH := "res://data/definitions/scenario/cities.json"
const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"

## docs/data/state_infrastructure.md §5 (valores nacionais propagados aos estados).
const EXAMPLES := {
	"BRA": [2, 3, 3],
	"IND": [2, 3, 2],
	"NGA": [1, 0, 1],
	"DEU": [3, 3, 3],
	"CHN": [3, 3, 2],
}
const TYPES := ["road", "power", "water"]

var _failures: Array = []


func _init() -> void:
	_run()
	if _failures.is_empty():
		print("[b5-infra] OK: dado real (4.226 estados), exemplos, fallback e núcleo.")
		quit(0)
	else:
		print("[b5-infra] FALHAS (%d):" % _failures.size())
		for failure: String in _failures:
			print(" - " + failure)
		quit(1)


func _fail(message: String) -> void:
	_failures.append(message)


func _json(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("não abriu " + path)
		return null
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data


func _levels_of(levels: Dictionary, state_id: String) -> Array:
	var entry: Dictionary = levels.get(state_id, {})
	return [int(entry.get("road", -1)), int(entry.get("power", -1)),
		int(entry.get("water", -1))]


func _run() -> void:
	var document: Variant = _json(STATE_INFRA_PATH)
	if typeof(document) != TYPE_DICTIONARY:
		_fail("dado ausente ou inválido: " + STATE_INFRA_PATH)
		return
	var doc: Dictionary = document
	var states: Dictionary = doc.get("states", {})
	var levels := InitialInfraScript.levels_by_state()
	if levels.size() != states.size():
		_fail("levels_by_state=%d != estados do arquivo=%d" % [
			levels.size(), states.size()])

	_check_rule_matches_file(states, levels)
	_check_examples(states, levels)
	_check_no_data(states, levels)
	_check_fallback(states, levels)
	_check_bands()
	_check_core(states, levels)


## A regra do jogo (recalculada) e o campo `infra_levels` do dado concordam.
func _check_rule_matches_file(states: Dictionary, levels: Dictionary) -> void:
	var mismatches: Array = []
	for state_id: Variant in states:
		var entry: Dictionary = states[state_id]
		var declared: Dictionary = entry.get("infra_levels", {})
		var computed := _levels_of(levels, String(state_id))
		for index in TYPES.size():
			if int(declared.get(TYPES[index], 0)) != int(computed[index]):
				mismatches.append("%s.%s: arquivo=%d regra=%d" % [state_id,
					TYPES[index], int(declared.get(TYPES[index], 0)),
					int(computed[index])])
	if not mismatches.is_empty():
		_fail("regra %%->nível diverge do arquivo em %d campos (1º: %s)" % [
			mismatches.size(), String(mismatches[0])])


## Exemplos documentados (granularidade nacional: todo estado da nação igual).
func _check_examples(states: Dictionary, levels: Dictionary) -> void:
	for tag: String in EXAMPLES:
		var expected: Array = EXAMPLES[tag]
		var checked := 0
		for state_id: Variant in states:
			var entry: Dictionary = states[state_id]
			if String(entry.get("nation", "")) != tag:
				continue
			if String(entry.get("data_status", "")) != "observed_national":
				continue
			checked += 1
			var computed := _levels_of(levels, String(state_id))
			if computed != expected:
				_fail("%s/%s: %s != doc %s" % [tag, state_id,
					str(computed), str(expected)])
				return
		if checked == 0:
			_fail("%s: nenhum estado observed_national no arquivo" % tag)
		else:
			print("[b5-infra] %s: %d estados em %s (doc)" % [
				tag, checked, str(expected)])


## Estado sem observação: ausência explícita, nível 0 nas três dimensões.
func _check_no_data(states: Dictionary, levels: Dictionary) -> void:
	var checked := 0
	for state_id: Variant in states:
		var entry: Dictionary = states[state_id]
		if String(entry.get("data_status", "")) != "no_data":
			continue
		checked += 1
		var computed := _levels_of(levels, String(state_id))
		if computed != [0, 0, 0]:
			_fail("%s no_data com níveis %s (esperado 0/0/0)" % [
				state_id, str(computed)])
	if checked == 0:
		_fail("nenhum estado no_data no arquivo (esperado 57)")
	else:
		print("[b5-infra] no_data: %d estados em 0/0/0" % checked)


## Rural ausente e total presente: o fallback declarado no arquivo é usado.
func _check_fallback(states: Dictionary, levels: Dictionary) -> void:
	var fallbacks := {"power": 0, "water": 0}
	for state_id: Variant in states:
		var entry: Dictionary = states[state_id]
		var computed: Dictionary = levels.get(String(state_id), {})
		for kind in ["power", "water"]:
			var rural_key := "electricity_access_rural_pct" if kind == "power" \
					else "water_basic_rural_pct"
			var total_key := "electricity_access_pct" if kind == "power" \
					else "water_basic_pct"
			var rural: Variant = entry.get(rural_key, null)
			var total: Variant = entry.get(total_key, null)
			var source := String(computed.get(kind + "_source", ""))
			if rural == null and total != null:
				fallbacks[kind] = int(fallbacks[kind]) + 1
				if source != "fallback":
					_fail("%s.%s sem rural: fonte=%s (esperado fallback)" % [
						state_id, kind, source])
			elif rural != null and source != "field":
				_fail("%s.%s com rural: fonte=%s (esperado field)" % [
					state_id, kind, source])
	# A doc registra 2 nações em fallback de energia (GRD, PRK) e 16 na água.
	if int(fallbacks["power"]) == 0 or int(fallbacks["water"]) == 0:
		_fail("fallback não exercitado (power=%d water=%d)" % [
			int(fallbacks["power"]), int(fallbacks["water"])])
	else:
		print("[b5-infra] fallback total: energia %d estados, água %d estados" % [
			int(fallbacks["power"]), int(fallbacks["water"])])


## Cortes da regra documentada (docs/data/state_infrastructure.md §3).
func _check_bands() -> void:
	var rule := InitialInfraScript.conversion_rule()
	var power: Array = rule.get("power", {}).get("bands", [])
	var water: Array = rule.get("water", {}).get("bands", [])
	var road: Array = rule.get("road", {}).get("bands", [])
	var cases := [
		{"value": 49.99, "bands": power, "metric": "pct", "want": 0},
		{"value": 50.0, "bands": power, "metric": "pct", "want": 1},
		{"value": 74.99, "bands": power, "metric": "pct", "want": 1},
		{"value": 75.0, "bands": water, "metric": "pct", "want": 2},
		{"value": 94.99, "bands": water, "metric": "pct", "want": 2},
		{"value": 95.0, "bands": power, "metric": "pct", "want": 3},
		{"value": 1.99, "bands": road, "metric": "index", "want": 0},
		{"value": 2.0, "bands": road, "metric": "index", "want": 1},
		{"value": 2.99, "bands": road, "metric": "index", "want": 1},
		{"value": 3.0, "bands": road, "metric": "index", "want": 2},
		{"value": 3.99, "bands": road, "metric": "index", "want": 2},
		{"value": 4.0, "bands": road, "metric": "index", "want": 3},
		{"value": 4.3, "bands": road, "metric": "index", "want": 3},
		{"value": null, "bands": power, "metric": "pct", "want": 0},
	]
	for item: Dictionary in cases:
		var got: int = InitialInfraScript.level_from_bands(item["value"],
			item["bands"], String(item["metric"]), 0)
		if got != int(item["want"]):
			_fail("corte %s (%s): %d != %d" % [str(item["value"]),
				String(item["metric"]), got, int(item["want"])])


## Núcleo: estados -> regiões (pior caso), persistência no save e ausência.
func _check_core(states: Dictionary, levels: Dictionary) -> void:
	if not ClassDB.class_exists("EconomySim"):
		_fail("EconomySim ausente (sem extensão compilada)")
		return
	var cities: Array = _json(CITIES_PATH)
	RegionDataScript.attach(cities)
	# Uma cidade real de cada nação do exemplo (a primeira com região).
	var by_nation := {}
	for city: Dictionary in cities:
		var tag := String(city.get("nation", ""))
		if not EXAMPLES.has(tag) or by_nation.has(tag):
			continue
		if (city.get("region", {}) as Dictionary).is_empty():
			continue
		by_nation[tag] = city
	if by_nation.size() != EXAMPLES.size():
		_fail("cidades do exemplo não resolvidas: %s" % str(by_nation.keys()))
		return

	# Região com DOIS estados (BRA + NGA) para a regra do pior caso: copia a
	# cidade BRA e acrescenta um estado de NGA.
	var mixed: Dictionary = (by_nation["BRA"] as Dictionary).duplicate(true)
	var bra_states: Array = (by_nation["BRA"] as Dictionary)["region"]["states"]
	var nga_states: Array = (by_nation["NGA"] as Dictionary)["region"]["states"]
	mixed["id"] = "B5-MIXED"
	mixed["region"]["states"] = bra_states.duplicate(true)
	if (nga_states as Array).size() > 0:
		(mixed["region"]["states"] as Array).append(
			(nga_states[0] as Dictionary).duplicate(true))
	# Região sem estados: não pode ser tocada (fica em 0).
	var empty: Dictionary = (by_nation["DEU"] as Dictionary).duplicate(true)
	empty["id"] = "B5-EMPTY"
	empty["region"]["states"] = []

	var world: Array = []
	for tag: String in by_nation:
		var city: Dictionary = (by_nation[tag] as Dictionary).duplicate(true)
		city["id"] = "B5-" + tag
		world.append(city)
	world.append(mixed)
	world.append(empty)

	var sim = EconomySim.new()
	sim.set_goods(_json(GOODS_PATH))
	sim.set_recipes(_json(RECIPES_PATH))
	sim.set_needs(_json(NEEDS_PATH))
	sim.initialize_world(world, [], [])
	if not sim.has_method("set_region_infra"):
		_fail("binário sem set_region_infra (rode o build)")
		return
	sim.set_region_infra(levels)

	for tag: String in EXAMPLES:
		var expected: Array = EXAMPLES[tag]
		var infra: Dictionary = sim.get_region_status("B5-" + tag).get("infra", {})
		var got := [int(infra.get("road", -1)), int(infra.get("power", -1)),
			int(infra.get("water", -1))]
		if got != expected:
			_fail("núcleo %s: %s != %s" % [tag, str(got), str(expected)])
		else:
			print("[b5-infra] núcleo %s: %s" % [tag, str(got)])

	# Pior caso: BRA (2/3/3) + NGA (1/0/1) -> 1/0/1.
	var mixed_infra: Dictionary = sim.get_region_status("B5-MIXED").get("infra", {})
	var mixed_got := [int(mixed_infra.get("road", -1)),
		int(mixed_infra.get("power", -1)), int(mixed_infra.get("water", -1))]
	if mixed_got != [1, 0, 1]:
		_fail("região multi-estado: %s != [1, 0, 1] (pior caso BRA+NGA)" %
			str(mixed_got))
	# Sem estados: não foi tocada.
	var empty_infra: Dictionary = sim.get_region_status("B5-EMPTY").get("infra", {})
	var empty_got := [int(empty_infra.get("road", -1)),
		int(empty_infra.get("power", -1)), int(empty_infra.get("water", -1))]
	if empty_got != [0, 0, 0]:
		_fail("região sem estados foi tocada: %s" % str(empty_got))

	# Persistência: o save leva os níveis (formato v21+, sem mudança de schema).
	# Sem `tick_day` aqui: este mundo sintético de 7 cidades não tem portos nem
	# camada urbana (`set_urban`) e o dia não é o objeto deste teste — o tick
	# completo com infra é coberto pelo A/B de 365 d e pela bateria.
	var save: Dictionary = sim.to_dict()
	var persisted := {}
	for city: Dictionary in save.get("cities", []):
		persisted[String(city.get("id", ""))] = \
			city.get("region", {}).get("infra", {})
	for tag: String in EXAMPLES:
		var expected: Array = EXAMPLES[tag]
		var infra: Dictionary = persisted.get("B5-" + tag, {})
		var got := [int(infra.get("road", -1)), int(infra.get("power", -1)),
			int(infra.get("water", -1))]
		if got != expected:
			_fail("save %s: %s != %s" % [tag, str(got), str(expected)])

	# Idempotência: reaplicar o mesmo dado não muda nada.
	sim.set_region_infra(levels)
	var again: Dictionary = sim.get_region_status("B5-BRA").get("infra", {})
	if int(again.get("road", -1)) != 2 or int(again.get("power", -1)) != 3 \
			or int(again.get("water", -1)) != 3:
		_fail("reaplicar o dado mudou BRA: %s" % str(again))
