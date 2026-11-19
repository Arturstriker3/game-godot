extends SceneTree

## Distribuição de renda por nação: Gini estimado (7 regiões) + quintis da B2.
##
## Verifica o contrato novo:
##  1. `income_distribution.json` cobre as 7 regiões do cenário e é TODO
##     `estimated` (nenhuma nação tem Gini observado no projeto);
##  2. o núcleo recebe o Gini por cidade (`set_income_gini`) e o publica no
##     save (save/load preserva);
##  3. a repartição da renda é por QUINTIL (B2/D-083): cinco estratos com 20%
##     das pessoas cada, `income_share` somando 1 e renda/consumo crescendo do
##     Q1 para o Q5. `income_level_share` é medido dos estratos (não é mais a
##     meta F7 por classe, que a B2 aposentou);
##  4. a TFP publicada é o FATOR COMPLETO `derived:gdp_ppp` (não prêmio):
##     EUA ~3,5 > DEU ~2,9 > BRA ~0,9 > IND ~0,46 > NGA ~0,37.
##
## Uso: godot --headless --path . --script res://tools/economy_income_distribution_test.gd

const GeoScript := preload("res://core/economy/geography_service.gd")

const GOODS_PATH := "res://data/definitions/economy/goods.json"
const RECIPES_PATH := "res://data/definitions/economy/recipes.json"
const NEEDS_PATH := "res://data/definitions/economy/needs.json"

## B2 (D-083): cinco quintis de renda; cada um começa com 20% das pessoas.
const STRATA := 5
## Faixa plausível do Gini medido dos quintis (nenhuma âncora entra na renda).
const GINI_MIN := 0.15
const GINI_MAX := 0.75
## Pares de âncoras mais próximas que isto não entram na conferência de ordem.
const GINI_ORDER_STEP := 0.05

const FOCUS := ["USA", "DEU", "BRA", "IND", "NGA"]
const CITY_ID := "%s-CAP"
const POPULATION := 5_000_000.0

var _failures: Array[String] = []


func _init() -> void:
	_test_profile()
	_test_shares()
	_test_save_load()
	_test_productivity_order()
	if _failures.is_empty():
		print("[gini] OK: Gini estimado por região, quintis com renda crescente, save e TFP.")
		quit(0)
	else:
		print("[gini] FALHAS (%d):" % _failures.size())
		for failure in _failures:
			print(" - " + failure)
		quit(1)


func _fail(message: String) -> void:
	_failures.append(message)


func _load_json(path: String) -> Variant:
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		_fail("não abriu " + path)
		return null
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	return data


func _load_array(path: String) -> Array:
	var data: Variant = _load_json(path)
	return data if typeof(data) == TYPE_ARRAY else []


func _load_dict(path: String) -> Dictionary:
	var data: Variant = _load_json(path)
	return data if typeof(data) == TYPE_DICTIONARY else {}


# --- 1. Perfil de Gini -------------------------------------------------------

func _test_profile() -> void:
	var geo = GeoScript.new()
	var profile: Dictionary = geo.gini_profile()
	var meta: Dictionary = profile.get("meta", {})
	if not bool(meta.get("available", false)):
		_fail("perfil: income_distribution.json indisponível (%s)" % meta.get(
			"reason", "?"))
		return
	if not bool(meta.get("estimated", false)):
		_fail("perfil: Gini sem dado por nação deveria ser marcado estimated")
	var regions: Dictionary = profile.get("by_region", {})
	if regions.size() != 7:
		_fail("perfil: %d regiões (esperado 7 do nations.json)" % regions.size())
	for region: String in regions:
		var value := float(regions[region])
		if value < 0.20 or value > 0.75:
			_fail("perfil: Gini de %s fora da faixa plausível (%.2f)" % [region, value])
	# Toda nação precisa resolver para uma região (nada de Gini 0 silencioso).
	var nations := _load_array("res://data/definitions/scenario/nations.json")
	var missing := 0
	for nation in nations:
		var entry: Dictionary = geo.nation_gini(String(nation.get("id", "")))
		if not bool(entry.get("available", false)):
			missing += 1
			if missing <= 3:
				_fail("perfil: nação %s sem região de Gini" % String(nation.get("id", "")))
	print("[gini] perfil: %d regiões, %d nações resolvidas, todo `estimated`" % [
		regions.size(), nations.size() - missing])
	print("[gini] regiões: %s" % str(regions))


# --- 2/3. Fatias por nação ---------------------------------------------------

func _new_world() -> Object:
	var geo = GeoScript.new()
	var cities: Array = []
	for tag: String in FOCUS:
		cities.append({
			"id": CITY_ID % tag,
			"name": tag + " Capital",
			"nation": tag,
			"home_nation": tag,
			"landmass": "testland",
			"population": POPULATION,
			"sim_population": POPULATION,
			"x": 100.0 + float(cities.size()) * 10.0,
			"y": 100.0,
		})
	var sim = EconomySim.new()
	sim.set_goods(_load_array(GOODS_PATH))
	sim.set_recipes(_load_array(RECIPES_PATH))
	sim.set_needs(_load_dict(NEEDS_PATH))
	sim.initialize_world(cities, [], [])
	for city in cities:
		var tag := String(city["nation"])
		sim.set_external_income_index(String(city["id"]),
			geo.external_income_index(tag))
		var gini: Dictionary = geo.nation_gini(tag)
		sim.set_income_gini(String(city["id"]), float(gini.get("gini", 0.0)))
	sim.tick_day()
	return sim


## Quintis por nação, do bloco pesado `pop.income_strata` (B2/D-083).
func _strata_by_nation(sim: Object) -> Dictionary:
	var out := {}
	for city in sim.get_status().get("cities", []):
		var city_id := String(city.get("id", ""))
		var heavy: Dictionary = sim.get_city_status(city_id)
		var pop: Dictionary = heavy.get("pop", {})
		var strata: Dictionary = pop.get("income_strata", {})
		var people: Array = strata.get("population", [])
		if people.size() != STRATA:
			_fail("%s: status sem income_strata[%d]" % [city_id, STRATA])
			continue
		out[String(city.get("nation", ""))] = {
			"people": people,
			"share": strata.get("income_share", []),
			"income": strata.get("income", []),
			"level": strata.get("level", []),
			"gini": float(strata.get("gini", 0.0)),
			"level_share": pop.get("income_level_share", []),
			"population": float(pop.get("population", 0.0)),
		}
	return out


func _test_shares() -> void:
	var sim = _new_world()
	var geo = GeoScript.new()
	var rows := _strata_by_nation(sim)
	var gini_measured := {}
	var gini_anchor := {}
	print("[gini] quintis por nação (renda | nível):")
	for tag: String in FOCUS:
		if not rows.has(tag):
			_fail("%s: sem quintis no status" % tag)
			continue
		var row: Dictionary = rows[tag]
		var share: Array = row["share"]
		var level: Array = row["level"]
		var people: Array = row["people"]
		var population := float(row["population"])
		var sum_share := 0.0
		var sum_people := 0.0
		var parts: PackedStringArray = []
		for s in STRATA:
			sum_share += float(share[s])
			sum_people += float(people[s])
			parts.append("%.0f%%/n%d" % [float(share[s]) * 100.0, int(level[s])])
		print("  %-4s %s" % [tag, " · ".join(parts)])
		if absf(sum_share - 1.0) > 1e-6:
			_fail("%s: income_share soma %.6f (esperado 1,0)" % [tag, sum_share])
		if absf(sum_people - population) > population * 1e-6:
			_fail("%s: soma dos quintis %.3f != população %.3f" % [
				tag, sum_people, population])
		# Semeadura do dia 0: quintil = 20% das pessoas.
		for s in STRATA:
			if absf(float(people[s]) - population / STRATA) > population * 0.01:
				_fail("%s: Q%d com %.0f pessoas (esperado 20%% de %.0f)" % [
					tag, s + 1, float(people[s]), population])
		# A renda cresce do quintil mais pobre para o mais rico.
		if float(share[STRATA - 1]) <= float(share[0]):
			_fail("%s: renda do Q5 (%.3f) não supera o Q1 (%.3f)" % [
				tag, float(share[STRATA - 1]), float(share[0])])
		# Consumo por estrato: nível não decrescente (Q5 nunca abaixo do Q1).
		for s in range(1, STRATA):
			if int(level[s]) < int(level[s - 1]):
				_fail("%s: nível do Q%d (%d) abaixo do Q%d (%d)" % [
					tag, s + 1, int(level[s]), s, int(level[s - 1])])
		# `income_level_share` é medido dos estratos e continua somando a POP.
		var level_share: Array = row["level_share"]
		if level_share.size() < 4:
			_fail("%s: income_level_share[4] ausente" % tag)
		else:
			var sum_levels := 0.0
			for value in level_share:
				sum_levels += float(value)
			if absf(sum_levels - 1.0) > 1e-6:
				_fail("%s: income_level_share soma %.6f" % [tag, sum_levels])
		# Gini publicado é medido dos quintis (a âncora regional só semeia).
		var gini := float(row["gini"])
		var anchor := float(geo.nation_gini(tag).get("gini", 0.0))
		if gini < GINI_MIN or gini > GINI_MAX:
			_fail("%s: Gini medido %.3f fora da faixa plausível [%.2f, %.2f]" % [
				tag, gini, GINI_MIN, GINI_MAX])
		gini_measured[tag] = gini
		gini_anchor[tag] = anchor
		print("  %-4s Gini medido %.3f (âncora %.3f)" % [tag, gini, anchor])
	# A ordem do Gini medido acompanha a âncora regional. O número é medido dos
	# quintis, então a igualdade exata não é contrato — e âncoras quase iguais
	# (DEU × NGA, 0,002) não entram na conferência de ordem.
	for first: String in FOCUS:
		if not gini_measured.has(first):
			continue
		for second: String in FOCUS:
			if second <= first or not gini_measured.has(second):
				continue
			var anchor_gap := float(gini_anchor[first]) - float(gini_anchor[second])
			if absf(anchor_gap) <= GINI_ORDER_STEP:
				continue
			var measured_gap := float(gini_measured[first]) - float(gini_measured[second])
			if anchor_gap * measured_gap < 0.0:
				_fail("Gini fora de ordem: %s %.3f (âncora %.3f) × %s %.3f (âncora %.3f)" % [
					first, float(gini_measured[first]), float(gini_anchor[first]),
					second, float(gini_measured[second]), float(gini_anchor[second])])


# --- 4. Save/load ------------------------------------------------------------

func _test_save_load() -> void:
	var sim = _new_world()
	var save: Dictionary = sim.to_dict()
	var geo = GeoScript.new()
	for city in save.get("cities", []):
		var tag := String(city.get("nation", ""))
		var expected := float(geo.nation_gini(tag).get("gini", 0.0))
		if absf(float(city.get("income_gini", -1.0)) - expected) > 1e-9:
			_fail("save: %s não gravou o Gini %.3f (%.3f)" % [
				tag, expected, float(city.get("income_gini", -1.0))])
	var loaded = EconomySim.new()
	loaded.set_goods(_load_array(GOODS_PATH))
	loaded.set_recipes(_load_array(RECIPES_PATH))
	loaded.set_needs(_load_dict(NEEDS_PATH))
	loaded.from_dict(save)
	# Save antigo (sem o campo) mantém a repartição legada (Gini 0), nunca 1,0.
	var legacy := save.duplicate(true)
	for city in legacy.get("cities", []):
		city.erase("income_gini")
	var restored = EconomySim.new()
	restored.set_goods(_load_array(GOODS_PATH))
	restored.set_recipes(_load_array(RECIPES_PATH))
	restored.set_needs(_load_dict(NEEDS_PATH))
	restored.from_dict(legacy)
	for city in restored.to_dict().get("cities", []):
		if absf(float(city.get("income_gini", -1.0))) > 1e-9:
			_fail("save antigo: %s ganhou Gini %.3f do nada" % [
				String(city.get("nation", "")), float(city.get("income_gini", -1.0))])


# --- 5. TFP como fator completo ---------------------------------------------

func _test_productivity_order() -> void:
	var sim = _new_world()
	var values := {}
	for city in sim.get_status().get("cities", []):
		values[String(city.get("nation", ""))] = float(
			city.get("productivity_index", 0.0))
	var expected := {
		"USA": 3.5, "DEU": 2.9, "BRA": 0.91, "IND": 0.46, "NGA": 0.37,
	}
	for tag: String in FOCUS:
		if not values.has(tag):
			_fail("TFP: %s ausente no status" % tag)
			continue
		if absf(float(values[tag]) - float(expected[tag])) > 0.05:
			_fail("TFP: %s %.3f != %.2f (derived:gdp_ppp)" % [
				tag, float(values[tag]), float(expected[tag])])
	# Fator completo: países pobres têm produtividade < 1 (não prêmio max(1,.)).
	if float(values.get("NGA", 1.0)) >= 1.0 or float(values.get("IND", 1.0)) >= 1.0:
		_fail("TFP: país abaixo da média mundial recebeu prêmio >= 1,0")
	print("[gini] TFP: USA %.2f DEU %.2f BRA %.2f IND %.2f NGA %.2f" % [
		float(values.get("USA", 0.0)), float(values.get("DEU", 0.0)),
		float(values.get("BRA", 0.0)), float(values.get("IND", 0.0)),
		float(values.get("NGA", 0.0))])
